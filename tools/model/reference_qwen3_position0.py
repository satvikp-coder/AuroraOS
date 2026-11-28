"""Independent NumPy reference for one Qwen3-0.6B position (BOS by default).

The caller supplies an upstream BF16 safetensors checkpoint. At position zero,
each causal query head sees exactly one value, so no cache is allocated.
Only local, read-only model files are used. NumPy is optional tooling.
"""
import argparse
import json
import struct

import numpy as np

from check_qwen3_checkpoint import check_checkpoint


def read_exact(stream, offset, count):
    stream.seek(offset)
    data = stream.read(count)
    if len(data) != count:
        raise ValueError("short tensor read")
    return data


class Checkpoint:
    def __init__(self, stream):
        check_checkpoint(stream)
        self.stream = stream
        size, = struct.unpack("<Q", read_exact(stream, 0, 8))
        self.header = json.loads(read_exact(stream, 8, size))
        self.data_start = 8 + size

    def read(self, name, start=0, end=None):
        entry = self.header[name]
        shape = entry["shape"]
        if len(shape) == 1:
            rows, cols = 1, shape[0]
        else:
            rows, cols = shape
        end = rows if end is None else end
        if not 0 <= start <= end <= rows:
            raise ValueError("tensor row interval out of range")
        offset = self.data_start + entry["data_offsets"][0] + 2 * start * cols
        data = read_exact(self.stream, offset, 2 * (end - start) * cols)
        words = np.frombuffer(data, dtype="<u2").astype(np.uint32)
        return (words << 16).view(np.float32).reshape(end - start, cols)


def rms(x, gain):
    return x * (1.0 / np.sqrt(np.mean(x * x) + 1e-6)) * gain


def next_token(checkpoint, token):
    embed = "model.embed_tokens.weight"
    x = checkpoint.read(embed, token, token + 1)[0]
    for layer in range(28):
        prefix = f"model.layers.{layer}."

        def weight(name):
            return checkpoint.read(prefix + name + ".weight")

        before = rms(x, weight("input_layernorm")[0])
        v = (weight("self_attn.v_proj") @ before).reshape(8, 128)
        grouped = np.repeat(v, 2, axis=0).reshape(2048)
        x = x + weight("self_attn.o_proj") @ grouped
        after = rms(x, weight("post_attention_layernorm")[0])
        gate = weight("mlp.gate_proj") @ after
        up = weight("mlp.up_proj") @ after
        with np.errstate(over="ignore"):
            activated = (gate / (1.0 + np.exp(-gate))) * up
        x = x + weight("mlp.down_proj") @ activated

    x = rms(x, checkpoint.read("model.norm.weight")[0])
    logits = np.empty(151936, dtype=np.float32)
    # The published config ties the output head to the embedding matrix.
    for start in range(0, len(logits), 2048):
        stop = min(start + 2048, len(logits))
        logits[start:stop] = checkpoint.read(embed, start, stop) @ x
    if not np.isfinite(logits).all():
        raise ValueError("non-finite reference logits")
    best = int(np.argmax(logits))
    return best, float(logits[best])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("checkpoint", type=argparse.FileType("rb"))
    parser.add_argument("--token", type=int, default=151643)
    args = parser.parse_args()
    with args.checkpoint as stream:
        token, logit = next_token(Checkpoint(stream), args.token)
    print(f"BF16 reference next token {token}, selected logit {logit:.9g}")


if __name__ == "__main__":
    main()
