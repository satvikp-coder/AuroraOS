import struct
import unittest

from export_qwen3_merges import export_merges


def document():
    return {"model": {"type": "BPE", "vocab": {"a": 0, "b": 1, "ab": 2, "ba": 3},
                      "merges": [["b", "a"], ["a", "b"]]}}


class MergeExportTest(unittest.TestCase):
    def test_sort_keeps_original_priority(self):
        table = export_merges(document())
        self.assertEqual(struct.unpack_from("<8sII", table), (b"AURQTM1\0", 4, 2))
        self.assertEqual(list(struct.iter_unpack("<4I", table[16:])),
                         [(0, 1, 2, 1), (1, 0, 3, 0)])

    def test_missing_output(self):
        data = document()
        data["model"]["merges"][0] = ["a", "a"]
        with self.assertRaisesRegex(ValueError, "missing"):
            export_merges(data)

    def test_duplicate_pair(self):
        data = document()
        data["model"]["merges"][0] = ["a", "b"]
        with self.assertRaisesRegex(ValueError, "duplicate pair"):
            export_merges(data)

    def test_ignored_merges(self):
        data = document()
        data["model"]["ignore_merges"] = True
        with self.assertRaisesRegex(ValueError, "configuration"):
            export_merges(data)


if __name__ == "__main__":
    unittest.main()
