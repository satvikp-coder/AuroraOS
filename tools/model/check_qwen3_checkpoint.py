"""Check the published Qwen3-0.6B BF16 safetensors layout before conversion.

Usage: python tools/model/check_qwen3_checkpoint.py path/to/model.safetensors
Reads only the header. Tensor contents and model authenticity are outside this check.
Architecture: https://huggingface.co/Qwen/Qwen3-0.6B/blob/main/config.json
"""
import argparse
import json
import math
import os
import struct


def expected_shapes():
    hidden, intermediate, head_dim = 1024, 3072, 128
    query_width, kv_width = 16 * head_dim, 8 * head_dim
    shapes = {
        "model.embed_tokens.weight": [151936, hidden],
        "lm_head.weight": [151936, hidden],  # Serialized even with tied embeddings.
        "model.norm.weight": [hidden],
    }
    layer_shapes = {
        "input_layernorm": [hidden],
        "post_attention_layernorm": [hidden],
        "self_attn.q_norm": [head_dim],
        "self_attn.k_norm": [head_dim],
        "self_attn.q_proj": [query_width, hidden],
        "self_attn.k_proj": [kv_width, hidden],
        "self_attn.v_proj": [kv_width, hidden],
        "self_attn.o_proj": [hidden, query_width],
        "mlp.gate_proj": [intermediate, hidden],
        "mlp.up_proj": [intermediate, hidden],
        "mlp.down_proj": [hidden, intermediate],
    }
    for layer in range(28):
        for role, shape in layer_shapes.items():
            shapes[f"model.layers.{layer}.{role}.weight"] = shape
    return shapes


def unique_keys(items):
    result = {}
    for key, value in items:
        if key in result:
            raise ValueError(f"duplicate header key {key}")
        result[key] = value
    return result


def check_checkpoint(stream):
    size = stream.seek(0, os.SEEK_END)
    stream.seek(0)
    prefix = stream.read(8)
    if len(prefix) != 8:
        raise ValueError("truncated length prefix")
    length, = struct.unpack("<Q", prefix)
    # This fixed architecture has 311 tensors and fits comfortably below 1 MiB.
    if not 0 < length <= min(1024 * 1024, size - 8):
        raise ValueError("invalid header length")
    header = json.loads(stream.read(length).decode("utf-8"), object_pairs_hook=unique_keys)
    if not isinstance(header, dict):
        raise ValueError("expected a header object")
    header.pop("__metadata__", None)
    expected = expected_shapes()
    if header.keys() != expected.keys():
        missing = sorted(expected.keys() - header.keys())
        extra = sorted(header.keys() - expected.keys())
        raise ValueError(f"tensor name mismatch (missing {missing}, extra {extra})")
    ranges = []
    for name, shape in expected.items():
        entry = header[name]
        if not isinstance(entry, dict) or entry.get("dtype") != "BF16":
            raise ValueError(f"{name}: expected BF16")
        dims = entry.get("shape")
        if dims != shape or any(type(dim) is not int for dim in dims):
            raise ValueError(f"{name}: expected shape {shape}")
        offsets = entry.get("data_offsets")
        if (not isinstance(offsets, list) or len(offsets) != 2
                or any(type(n) is not int for n in offsets)):
            raise ValueError(f"{name}: invalid data_offsets")
        start, end = offsets
        if end - start != 2 * math.prod(shape):
            raise ValueError(f"{name}: wrong byte count")
        ranges.append((start, end, name))
    cursor = 0
    for start, end, name in sorted(ranges):
        if start != cursor:
            raise ValueError(f"{name}: overlap or gap in tensor data")
        cursor = end
    if cursor != size - length - 8:
        raise ValueError("payload extent differs from file size")
    return len(header), cursor


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("checkpoint", type=argparse.FileType("rb"))
    args = parser.parse_args()
    try:
        with args.checkpoint as stream:
            tensors, payload = check_checkpoint(stream)
    except (ValueError, OSError, RecursionError) as error:
        parser.exit(1, f"checkpoint check failed: {error}\n")
    print(f"Qwen3-0.6B layout verified: {tensors} BF16 tensors, {payload} payload bytes")


if __name__ == "__main__":
    main()
