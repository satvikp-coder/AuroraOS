"""Synthetic end-to-end conversion, using the same preflight with tiny shapes."""
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

from convert_qwen3_checkpoint import convert, convert_file

# A single GQA layer, two query heads, one KV head of width two.
SHAPES = {"model.embed_tokens.weight": [3, 2], "model.norm.weight": [2],
          "lm_head.weight": [3, 2]}
for name, shape in [
    ("input_layernorm", [2]), ("self_attn.q_proj", [4, 2]), ("self_attn.k_proj", [2, 2]),
    ("self_attn.v_proj", [2, 2]), ("self_attn.q_norm", [2]), ("self_attn.k_norm", [2]),
    ("self_attn.o_proj", [2, 4]), ("post_attention_layernorm", [2]),
    ("mlp.gate_proj", [3, 2]), ("mlp.up_proj", [3, 2]), ("mlp.down_proj", [2, 3]),
]:
    SHAPES[f"model.layers.0.{name}.weight"] = shape


def fixture(tied=True, first_value=None):
    header, payload, values = {}, bytearray(), {}
    for name, shape in reversed(list(SHAPES.items())):
        count = shape[0] * (shape[1] if len(shape) == 2 else 1)
        # Halfway cases at shift 0 exercise ties-to-even on both signs.
        vector = ([127, -1.5, 2.5, -2.5, 0.5, -0.5, 3.5, -3.5])[:count]
        if name == "model.norm.weight":
            vector = [0, 0]
        if name == "model.layers.0.self_attn.q_norm.weight":
            vector = [0.125, -0.25]
        if name == "lm_head.weight" and not tied:
            vector[1] = -1.75  # same int8 result as -1.5; raw BF16 still differs
        if first_value is not None and name == "model.embed_tokens.weight":
            vector[0] = first_value
        start = len(payload)
        for value in vector:
            payload.extend(struct.pack("<f", value)[2:])
        header[name] = dict(dtype="BF16", shape=shape, data_offsets=[start, len(payload)])
        values[name] = vector
    raw = json.dumps(header).encode()
    return struct.pack("<Q", len(raw)) + raw + payload, values


class ConvertTest(unittest.TestCase):
    def setUp(self):
        stub = patch("check_qwen3_checkpoint.expected_shapes", return_value=SHAPES)
        stub.start()
        self.addCleanup(stub.stop)

    def test_full_geometry_and_reconstruction(self):
        for tied in (False, True):
            src, values = fixture(tied)
            dest = io.BytesIO()
            with patch("convert_qwen3_checkpoint.CHUNK", 4):
                count, size = convert(io.BytesIO(src), dest)
            data = dest.getvalue()
            self.assertEqual(size, len(data))
            self.assertEqual(struct.unpack_from("<8s8IQ2I", data),
                             (b"AURQ3V2\0", 2, 3, 2, 3, 1, 2, 2, 13 if tied else 14,
                              size, 1, tied))
            cursor = 56 + 32 * count
            expected = list(SHAPES)
            roles = [1, 14, 10, 2, 3, 4, 5, 12, 13, 6, 11, 7, 8, 9]
            if tied:
                del expected[2], roles[2]
            for i, (name, expected_role) in enumerate(zip(expected, roles)):
                layer, role, rows, cols, shift, offset, n = struct.unpack_from("<HHIIIQQ", data, 56 + 32*i)
                self.assertEqual((offset, role, layer), (cursor, expected_role, 0 if name.startswith("model.layers") else 65535))
                self.assertEqual((rows, cols), tuple(SHAPES[name]) if len(SHAPES[name]) == 2 else (1, SHAPES[name][0]))
                self.assertEqual(n, rows * cols)
                integers = struct.unpack_from(f"<{n}b", data, cursor)
                for original, q in zip(values[name], integers):
                    self.assertEqual(q, round(original * 2**shift))
                    self.assertLessEqual(abs(original - q / 2**shift), 0.5 / 2**shift)
                if name.endswith("q_norm.weight"):
                    self.assertEqual(shift, 8)
                cursor += n
            self.assertEqual(cursor, size)

    def test_preserve_files_and_clean_up_after_failure(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            src, dst = root / "in", root / "out"
            src.write_bytes(fixture()[0])
            convert_file(src, dst)
            before = dst.read_bytes()
            with self.assertRaises(FileExistsError):
                convert_file(src, dst)
            self.assertEqual(before, dst.read_bytes())
            dst.unlink()
            for invalid in (float("nan"), float("inf"), -128):
                src.write_bytes(fixture(first_value=invalid)[0])
                with self.assertRaisesRegex(ValueError, "unrepresentable"):
                    convert_file(src, dst)
                self.assertEqual(list(root.iterdir()), [src])


if __name__ == "__main__":
    unittest.main()
