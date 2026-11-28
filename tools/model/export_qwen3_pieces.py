"""Export a Qwen3 ByteLevel ID-to-bytes table for bounded random-access decoding.

Usage: python tools/model/export_qwen3_pieces.py tokenizer.json output.qtp
The eight-byte magic AURQTP1 + NUL precedes a little-endian uint32 ID count,
then count+1 uint32 offsets relative to the start of the byte pool. Offset bit
31 marks a special ID; the final unflagged offset is the pool length. Subtract
consecutive masked offsets to get a piece length. Concatenate pieces before
UTF-8 decoding; token boundaries can split a character and pieces can hold NUL.
This export preserves published IDs. AuroraOS's synthetic tokenizer uses its
own byte IDs and C-string pieces and cannot consume this table directly.
q3p_read_piece() in kernel/include/qwen3_pieces.h reads the exported table.

Source: https://huggingface.co/Qwen/Qwen3-0.6B/blob/main/tokenizer.json
Verified source SHA-256: aeb13307a71acd8fe81861d94ad54ab689df773318809eed3cbe794b4492dae4
This input exports 151669 IDs (1583001 bytes). Model projection rows beyond
the tokenizer's last ID have no piece. Encoding and chat templates are outside
this ID-to-byte table.
"""
import argparse
import json
from pathlib import Path
import struct

MAX_JSON = 16 * 1024 * 1024
MAX_IDS = 151936  # Qwen3-0.6B output projection
MAX_PIECE = 4096
SPECIAL = 1 << 31


def byte_alphabet():
    # ByteLevel keeps printable Latin-1 except the soft hyphen and maps other
    # bytes in ascending order onto consecutive code points starting at U+0100.
    visible = set(range(33, 127)) | set(range(161, 173)) | set(range(174, 256))
    mapped = {chr(i): i for i in visible}
    for index, byte in enumerate(sorted(set(range(256)) - visible)):
        mapped[chr(256 + index)] = byte
    return mapped


def export_table(document):
    if not isinstance(document, dict):
        raise ValueError("expected a tokenizer object")
    model, decoder = document.get("model"), document.get("decoder")
    if not isinstance(model, dict) or model.get("type") != "BPE":
        raise ValueError("expected BPE model")
    if not isinstance(decoder, dict) or decoder.get("type") != "ByteLevel":
        raise ValueError("expected ByteLevel decoder")
    vocab, added = model.get("vocab"), document.get("added_tokens")
    if not isinstance(vocab, dict) or not isinstance(added, list):
        raise ValueError("invalid vocabulary or added-token list")
    count = len(vocab) + len(added)
    if not 0 < count <= MAX_IDS:
        raise ValueError("unsupported ID count")
    pieces = [None] * count
    alphabet = byte_alphabet()

    def insert(token_id, data, special):
        if type(token_id) is not int or not 0 <= token_id < count or pieces[token_id] is not None:
            raise ValueError(f"invalid or duplicate ID {token_id}")
        if not 0 < len(data) <= MAX_PIECE:
            raise ValueError(f"invalid piece length at ID {token_id}")
        pieces[token_id] = data, special

    for piece, token_id in vocab.items():
        try:
            raw = bytes(alphabet[c] for c in piece)
        except KeyError as error:
            raise ValueError(f"non-ByteLevel symbol in vocabulary at ID {token_id}") from error
        insert(token_id, raw, False)
    for token in added:
        if (not isinstance(token, dict) or not isinstance(token.get("content"), str)
                or type(token.get("special")) is not bool):
            raise ValueError("invalid added token")
        # Qwen's added tokens are printable ASCII tags. This restriction keeps
        # their literal bytes identical to the ByteLevel decoder's output.
        content = token["content"]
        if not content.isascii() or not content.isprintable():
            raise ValueError("unsupported added-token spelling")
        insert(token.get("id"), content.encode("ascii"), token["special"])

    offsets, pool = bytearray(), bytearray()
    for data, special in pieces:
        offsets.extend(struct.pack("<I", len(pool) | (SPECIAL if special else 0)))
        pool.extend(data)
    offsets.extend(struct.pack("<I", len(pool)))
    return struct.pack("<8sI", b"AURQTP1", count) + offsets + pool


def unique_keys(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key {key}")
        result[key] = value
    return result


def load_tokenizer(path):
    with path.open("rb") as stream:
        raw = stream.read(MAX_JSON + 1)
    if len(raw) > MAX_JSON:
        raise ValueError("tokenizer JSON exceeds 16 MiB")
    return json.loads(raw, object_pairs_hook=unique_keys)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tokenizer", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        table = export_table(load_tokenizer(args.tokenizer))
        with args.output.open("xb") as stream:
            stream.write(table)
    except (ValueError, OSError, RecursionError) as error:
        parser.exit(1, f"piece export failed: {error}\n")


if __name__ == "__main__":
    main()
