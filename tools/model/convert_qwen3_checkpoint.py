"""Convert the original Qwen3-0.6B BF16 safetensors to an AURQ3V2 int8 object.

Usage: python tools/model/convert_qwen3_checkpoint.py model.safetensors model.aurq3
The destination must be new and its filesystem must support hard links. Keep
the source immutable during conversion. Each tensor uses the greatest shift
in [0,31] for which max(abs(weight)) * 2**shift <= 127. For an all-zero
tensor shift is 0. Round to nearest with ties to even. Decode as q / 2**shift.
Finite inputs exceeding 127 and nonfinite inputs are rejected; no clipping.
Each representable value has absolute error at most 0.5 / 2**shift. This
per-tensor baseline does not calibrate activations or establish inference quality.
"""
import argparse
from array import array
import math
import os
from pathlib import Path
import struct
import sys
import tempfile

from check_qwen3_checkpoint import read_checkpoint_directory

CHUNK = 256 * 1024  # even number of bytes
LAYER_ROLES = (
    ("input_layernorm", 2), ("self_attn.q_proj", 3), ("self_attn.k_proj", 4),
    ("self_attn.v_proj", 5), ("self_attn.q_norm", 12), ("self_attn.k_norm", 13),
    ("self_attn.o_proj", 6), ("post_attention_layernorm", 11),
    ("mlp.gate_proj", 7), ("mlp.up_proj", 8), ("mlp.down_proj", 9),
)


def blocks(stream, offset, length):
    while length:
        stream.seek(offset)
        size = min(CHUNK, length)
        block = stream.read(size)
        if len(block) != size:
            raise ValueError("truncated tensor data")
        yield block
        offset += size
        length -= size


def words(block):
    result = array("H")
    result.frombytes(block)
    if sys.byteorder != "little":
        result.byteswap()
    return result


def bf16(bits):
    return struct.unpack("<f", struct.pack("<I", bits << 16))[0]


def tensor_shift(stream, offset, length):
    # Unsigned BF16 magnitudes are ordered by their bit representations.
    peak = 0
    for block in blocks(stream, offset, length):
        peak = max(peak, max(word & 0x7fff for word in words(block)))
    value = bf16(peak)
    if not math.isfinite(value) or value > 127:
        raise ValueError("unrepresentable BF16 weight")
    if value == 0:
        return 0
    shift = 31
    while math.ldexp(value, shift) > 127:
        shift -= 1
    return shift


def quantized_values(shift):
    # Cache a bounded 64 KiB lookup table for each shift actually encountered.
    table = bytearray(65536)
    for word in range(65536):
        x = bf16(word) * (1 << shift)
        if abs(x) <= 127:
            table[word] = round(x) & 255
    return table


def tensor_layout(layers, tied):
    yield "model.embed_tokens.weight", 65535, 1
    yield "model.norm.weight", 65535, 14
    if not tied:
        yield "lm_head.weight", 65535, 10
    for layer in range(layers):
        for name, role in LAYER_ROLES:
            yield f"model.layers.{layer}.{name}.weight", layer, role


def convert(stream, out):
    header, base, _ = read_checkpoint_directory(stream)
    embed = header["model.embed_tokens.weight"]
    head = header["lm_head.weight"]
    start, end = embed["data_offsets"]
    tied = all(a == b for a, b in zip(blocks(stream, base + start, end - start),
               blocks(stream, base + head["data_offsets"][0], end - start)))
    vocab, hidden = embed["shape"]
    first = "model.layers.0."
    dim, = header[first + "self_attn.q_norm.weight"]["shape"]
    q_heads = header[first + "self_attn.q_proj.weight"]["shape"][0] // dim
    kv_heads = header[first + "self_attn.k_proj.weight"]["shape"][0] // dim
    intermediate = header[first + "mlp.gate_proj.weight"]["shape"][0]
    layers = sum(name.endswith(".input_layernorm.weight") for name in header)
    layout = list(tensor_layout(layers, tied))
    cursor = 56 + 32 * len(layout)
    out.seek(cursor)
    directory = bytearray()
    tables = {}
    for name, layer, role in layout:
        entry = header[name]
        start, end = entry["data_offsets"]
        try:
            shift = tensor_shift(stream, base + start, end - start)
        except ValueError as error:
            raise ValueError(f"{name}: {error}") from error
        if shift not in tables:
            tables[shift] = quantized_values(shift)
        lut = tables[shift]
        for block in blocks(stream, base + start, end - start):
            out.write(bytes(lut[word] for word in words(block)))
        shape = entry["shape"]
        rows, cols = shape if len(shape) == 2 else (1, shape[0])
        size = rows * cols
        directory.extend(struct.pack("<HHIIIQQ", layer, role, rows, cols, shift, cursor, size))
        cursor += size
    out.seek(0)
    out.write(struct.pack("<8s8IQ2I", b"AURQ3V2", 2, vocab, hidden, intermediate, layers,
                          q_heads, dim, len(layout), cursor, kv_heads, tied))
    out.write(directory)
    return len(layout), cursor


def convert_file(source, destination):
    if os.path.lexists(destination):
        raise FileExistsError(f"destination exists: {destination}")
    with source.open("rb") as stream:
        original = os.fstat(stream.fileno())
        with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as temp:
            temp_path = Path(temp.name)
            try:
                stats = convert(stream, temp)
                current = os.fstat(stream.fileno())
                if (original.st_size, original.st_mtime_ns) != (current.st_size, current.st_mtime_ns):
                    raise ValueError("source changed during conversion")
                temp.flush()
                os.fsync(temp.fileno())
                temp.close()
                os.link(temp_path, destination)
                return stats
            finally:
                temp.close()
                temp_path.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    try:
        tensors, size = convert_file(args.source, args.destination)
    except (ValueError, OSError, RecursionError) as error:
        parser.exit(1, f"conversion failed: {error}\n")
    print(f"Converted {tensors} tensors into {size} bytes")


if __name__ == "__main__":
    main()
