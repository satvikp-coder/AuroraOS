"""Host regression tests for the original Qwen3-0.6B checkpoint layout."""
import io
import json
import math
import struct
import unittest

from check_qwen3_checkpoint import check_checkpoint, expected_shapes


class HeaderOnly(io.BytesIO):
    """A seekable virtual file that fails if the checker reads any weight bytes."""

    def __init__(self, directory, payload):
        raw = json.dumps(directory).encode()
        self.header_size = 8 + len(raw)
        self.file_size = self.header_size + payload
        super().__init__(struct.pack("<Q", len(raw)) + raw)

    def seek(self, offset, whence=0):
        if (offset, whence) == (0, 2):
            return self.file_size
        return super().seek(offset, whence)

    def read(self, size=-1):
        if size < 0 or self.tell() + size > self.header_size:
            raise AssertionError("read outside header")
        return super().read(size)


class CheckpointTest(unittest.TestCase):
    def setUp(self):
        self.header = {}
        self.payload = 0
        for name, shape in expected_shapes().items():
            end = self.payload + math.prod(shape) * 2
            self.header[name] = dict(dtype="BF16", shape=shape, data_offsets=[self.payload, end])
            self.payload = end

    def check(self):
        return check_checkpoint(HeaderOnly(self.header, self.payload))

    def test_complete_checkpoint(self):
        self.assertEqual(self.check(), (311, 1503264768))

    def test_hidden_width_substituted_for_query_width(self):
        self.header["model.layers.27.self_attn.q_proj.weight"]["shape"][0] = 1024
        with self.assertRaisesRegex(ValueError, "expected shape"):
            self.check()

    def test_missing_key_norm(self):
        del self.header["model.layers.0.self_attn.k_norm.weight"]
        with self.assertRaisesRegex(ValueError, "missing"):
            self.check()

    def test_wrong_dtype(self):
        self.header["lm_head.weight"]["dtype"] = "F16"
        with self.assertRaisesRegex(ValueError, "expected BF16"):
            self.check()

    def test_overlapping_offsets(self):
        entry = self.header["lm_head.weight"]
        entry["data_offsets"] = [n - 1 for n in entry["data_offsets"]]
        with self.assertRaisesRegex(ValueError, "overlap"):
            self.check()

    def test_truncated_payload(self):
        self.payload -= 1
        with self.assertRaisesRegex(ValueError, "file size"):
            self.check()


if __name__ == "__main__":
    unittest.main()
