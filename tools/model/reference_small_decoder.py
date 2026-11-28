"""Regenerate the small two-layer host-test goldens, with optional CPU PyTorch.

Run python tools/model/reference_small_decoder.py. The C host tests consume
frozen goldens and do not need PyTorch. Float64 values flow between layers.
"""
import torch
import torch.nn.functional as nn


def layer(inputs, number):
    def weights(role, rows, cols):
        values = [[((i*7 + j*11 + i*j*3 + role*5 + number*3) % 19 - 9) / 16
                   for j in range(cols)] for i in range(rows)]
        return torch.tensor(values, dtype=torch.float64)

    def norm(x, gain):
        return nn.rms_norm(x, (x.shape[-1],), torch.tensor(gain, dtype=x.dtype) / 4096, 1e-6)

    angles = torch.outer(torch.arange(3, dtype=torch.float64),
                         torch.tensor([1., .001], dtype=torch.float64)).repeat(1, 2)[:, None, :]

    def rotate(x, gain):
        x = norm(x, gain)
        first, second = x.chunk(2, dim=-1)
        return x * angles.cos() + torch.cat((-second, first), dim=-1) * angles.sin()

    x = norm(inputs, [4096,3584,4608,3840])
    q = rotate((x @ weights(0,8,4).T).reshape(3,2,4), [4096,3072,5120,3584])
    k = rotate((x @ weights(1,4,4).T).reshape(3,1,4), [3584,4608,4096,3840])
    v = (x @ weights(2,4,4).T).reshape(3,1,4)
    context = nn.scaled_dot_product_attention(q.transpose(0,1), k.transpose(0,1),
                                              v.transpose(0,1), is_causal=True, enable_gqa=True)
    residual = inputs + context.transpose(0,1).reshape(3,8) @ weights(3,4,8).T
    x = norm(residual, [3840,4352,4096,3584])
    return residual + (nn.silu(x @ weights(4,6,4).T) * (x @ weights(5,6,4).T)) @ weights(6,4,6).T


if __name__ == "__main__":
    span = torch.tensor([[12,-20,18,27], [-17,22,31,-9], [9,25,-13,15]], dtype=torch.float64) / 32
    for index in range(2):
        span = layer(span, index)
        print((span * 32).round().int().tolist())
