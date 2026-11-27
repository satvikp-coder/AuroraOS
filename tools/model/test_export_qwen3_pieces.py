import struct
import unittest

from export_qwen3_pieces import export_table


def document():
    return {"model": {"type": "BPE", "vocab": {"A": 0, "\u0100": 1, "\u0120B": 2}},
            "decoder": {"type": "ByteLevel"},
            "added_tokens": [{"id": 3, "content": "<end>", "special": True}]}


class ExportTest(unittest.TestCase):
    def test_layout_and_byte_mapping(self):
        table = export_table(document())
        self.assertEqual(struct.unpack_from("<8sI5I", table),
                         (b"AURQTP1\0", 4, 0, 1, 2, 0x80000004, 9))
        self.assertEqual(table[32:], b"A\0 B<end>")

    def test_reject_id_collision(self):
        data = document()
        data["added_tokens"][0]["id"] = 1
        with self.assertRaisesRegex(ValueError, "duplicate ID"):
            export_table(data)

    def test_reject_unknown_decoder(self):
        data = document()
        data["decoder"]["type"] = "Metaspace"
        with self.assertRaisesRegex(ValueError, "ByteLevel"):
            export_table(data)

    def test_reject_unmapped_vocab(self):
        data = document()
        data["model"]["vocab"] = {"\u2603": 0}
        with self.assertRaisesRegex(ValueError, "non-ByteLevel"):
            export_table(data)


if __name__ == "__main__":
    unittest.main()
