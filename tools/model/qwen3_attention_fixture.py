"""Emit a synthetic Qwen3 attention fixture as JSON (stdlib only).

Input and output order is [token][head][channel]. Inputs are projected Q/K/V,
and expected output is the attention context before the output projection.
The float64 oracle uses position IDs 0..2 and no dropout. For float32, compare
every context channel with atol=1e-6 and rtol=1e-5. Quantized implementations
need their own error analysis. See Transformers v4.51.0 Qwen3Attention:
https://github.com/huggingface/transformers/blob/v4.51.0/src/transformers/models/qwen3/modeling_qwen3.py
"""
import json
import math


def normalize_rotate(vector, gain, position, epsilon, theta):
    width = len(vector)
    rms = math.sqrt(sum(x * x for x in vector) / width + epsilon)
    scaled = [x * weight / rms for x, weight in zip(vector, gain)]
    half = width // 2
    result = [0.0] * width
    for i in range(half):
        angle = position * theta ** (-2 * i / width)
        cosine, sine = math.cos(angle), math.sin(angle)
        x, y = scaled[i], scaled[i + half]
        result[i] = x * cosine - y * sine
        result[i + half] = x * sine + y * cosine
    return result


def attention(q, k, v, q_gain, k_gain, epsilon, theta):
    def prepare(sequence, gain):
        return [[normalize_rotate(vector, gain, pos, epsilon, theta)
                 for vector in heads] for pos, heads in enumerate(sequence)]

    queries, keys = prepare(q, q_gain), prepare(k, k_gain)
    group_size = len(q[0]) // len(k[0])
    width = len(q_gain)
    output = []
    for pos, heads in enumerate(queries):
        context = []
        for head, query in enumerate(heads):
            kv_head = head // group_size
            scores = [sum(a * b for a, b in zip(query, keys[t][kv_head])) / math.sqrt(width)
                      for t in range(pos + 1)]
            maximum = max(scores)
            weights = [math.exp(score - maximum) for score in scores]
            total = sum(weights)
            context.append([sum(w * v[t][kv_head][i] for t, w in enumerate(weights)) / total
                            for i in range(width)])
        output.append(context)
    return output


def fixture():
    def projected(head_count, seed):
        return [[[((t + 1) * (c + 2) + h * 3 + c * c + seed) % 13 - 6
                   for c in range(4)] for h in range(head_count)] for t in range(3)]

    inputs = dict(q=projected(4, 0), k=projected(2, 5), v=projected(2, 2),
                  q_gain=[1.0, 0.5, 1.25, 0.75], k_gain=[0.75, 1.5, 1.0, 0.5],
                  epsilon=1e-6, theta=1_000_000)
    return dict(inputs=inputs, expected_context=attention(**inputs))


if __name__ == "__main__":
    print(json.dumps(fixture()))
