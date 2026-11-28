"""Export deterministic Qwen3 BPE merge ranks, ordered by (left ID, right ID).

Usage: python tools/model/export_qwen3_merges.py tokenizer.json output.qtm
Wire format: eight-byte AURQTM1 + NUL, base vocab count and merge count (u32),
then one (left, right, result, rank) u32 record per merge. All fields are
little-endian. Ranks come from the original list order, independently of IDs.
The table operates on base vocabulary IDs inside a pretokenized span. Unicode
normalization, pretokenization, special-token matching and chat templates
require separate processing.
Source: https://huggingface.co/Qwen/Qwen3-0.6B/blob/main/tokenizer.json
"""
import argparse
from pathlib import Path
import struct

from export_qwen3_pieces import MAX_IDS, load_tokenizer


def export_merges(document):
    model = document.get("model") if isinstance(document, dict) else None
    if not isinstance(model, dict) or model.get("type") != "BPE":
        raise ValueError("expected a BPE model")
    if any(model.get(k) for k in ("dropout", "continuing_subword_prefix", "end_of_word_suffix",
                                  "ignore_merges", "byte_fallback")):
        raise ValueError("unsupported BPE configuration")
    vocab, merges = model.get("vocab"), model.get("merges")
    if not isinstance(vocab, dict) or not 0 < len(vocab) <= MAX_IDS:
        raise ValueError("invalid base vocabulary")
    if any(type(i) is not int for i in vocab.values()) or set(vocab.values()) != set(range(len(vocab))):
        raise ValueError("expected dense unique base IDs")
    if not isinstance(merges, list) or len(merges) > MAX_IDS:
        raise ValueError("invalid merge count")

    records = {}
    for rank, pair in enumerate(merges):
        if not isinstance(pair, list) or len(pair) != 2 or not all(isinstance(p, str) and p for p in pair):
            raise ValueError(f"invalid pair at rank {rank}")
        left, right = pair
        try:
            ids = (vocab[left], vocab[right])
            result = vocab[left + right]
        except KeyError as error:
            raise ValueError(f"missing merge vocabulary entry at rank {rank}") from error
        if ids in records:
            raise ValueError(f"duplicate pair at rank {rank}")
        records[ids] = result, rank
    table = bytearray(struct.pack("<8sII", b"AURQTM1", len(vocab), len(merges)))
    for ids, value in sorted(records.items()):
        table.extend(struct.pack("<4I", *ids, *value))
    return table


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tokenizer", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        table = export_merges(load_tokenizer(args.tokenizer))
        with args.output.open("xb") as out:
            out.write(table)
    except (ValueError, OSError, RecursionError) as error:
        parser.exit(1, f"merge export failed: {error}\n")


if __name__ == "__main__":
    main()
