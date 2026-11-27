"""Compare all channels to independently computed PyTorch 2.9.1 CPU goldens."""
import math
import unittest

from qwen3_attention_fixture import fixture


class Qwen3AttentionTest(unittest.TestCase):
    def test_context(self):
        # torch.nn.functional.rms_norm over the head dimension, followed by
        # split-half RoPE and scaled_dot_product_attention with is_causal=True
        # and enable_gqa=True. Stored float32 results keep this test stdlib-only.
        expected = [
            [[-2, 0, 4, -3], [-2, 0, 4, -3], [1, 3, -6, 0], [1, 3, -6, 0]],
            [[-0.86987668, 1.69518518, -1.08555543, -0.17469153],
             [-0.89631057, 1.65553427, -0.96660274, -0.24077635],
             [2.47717047, 5.21575546, -3.04565930, 3.69292593],
             [2.22355819, 4.83533716, -3.55288363, 3.05889559]],
            [[-0.28446573, 2.57330155, -2.73541903, 0.30435058],
             [1.45948207, 5.18922329, -0.92383963, -4.99512482],
             [3.46724582, 2.61722994, -1.06550813, 2.08447599],
             [4.39771891, -2.73441601, 0.79543811, -2.33669686]],
        ]
        context = fixture()["expected_context"]
        self.assertEqual(len(context), len(expected))
        for pos, heads in enumerate(context):
            self.assertEqual(len(heads), len(expected[pos]))
            for h, vector in enumerate(heads):
                self.assertEqual(len(vector), len(expected[pos][h]))
                for i, value in enumerate(vector):
                    with self.subTest(position=pos, head=h, channel=i):
                        self.assertTrue(math.isclose(value, expected[pos][h][i],
                                                     abs_tol=1e-6, rel_tol=1e-5))


if __name__ == "__main__":
    unittest.main()
