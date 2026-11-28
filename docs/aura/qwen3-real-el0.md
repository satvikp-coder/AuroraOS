# Real-weight Qwen3 first-token probe in EL0

The opt-in QEMU probe runs the published Qwen3-0.6B configuration through
all 28 layers in an isolated userspace process. It reads quantized tensor
blocks from the verified model volume through SYS_MODEL and uses the tied
embedding matrix for output logits. It reports the greedy next token after
one input token, 151643.

At position zero, each query head has one eligible key. Its attention
weight is exactly one, so the compact probe can use the corresponding value
head directly. This keeps its activation workspace within the existing
128 KiB userspace heap. Multi-token prefill, KV caching, autoregressive EL0
decode, a tokenizer, and Aura integration still require separate work.

Use the Apache-2.0 [Qwen3-0.6B checkpoint](https://huggingface.co/Qwen/Qwen3-0.6B)
and the existing converter. The upstream single-file checkpoint has SHA-256
f47f71177f32bcd101b7573ec9171e6a57f4f4d31148d38e382306f42996874b.

```sh
python3 tools/model/convert_qwen3_checkpoint.py --allow-scale-out model.safetensors model.aurq3
sh tests/qemu/qwen3-real-el0-tests.sh model.aurq3
```

The probe requires a native C compiler, the AArch64 GNU toolchain, Python 3,
and QEMU. It keeps host output, guest serial output, and disposable disk
images in the printed scratch directory. Allow about 600 MiB of scratch
disk space in addition to the converted model. QEMU uses 256 MiB of guest
RAM and runs with networking disabled.

The test compares the quantized host and guest token IDs exactly and checks
the selected logit with absolute and relative tolerances of 1e-4. It also
checks the authorized process exit and the kernel's page and process
accounting. The independent NumPy reference uses the original BF16 weights
and reports its own greedy token and selected logit.

```sh
python3 tools/model/reference_qwen3_position0.py model.safetensors
```

A local run with QEMU 11.1 and the converted 569 MiB model produced token
2 on both host and guest. The selected logits were 7.5526276 on the host
and 7.5526309 in EL0. The guest reported a 64,664-byte arena, refused the
ordinary application identity, and reclaimed all process and page resources.
QEMU boot, model verification, and inference together took 96 seconds.

The independent BF16 reference returned token 14582 with logit 12.2307625.
Host and EL0 agreement establishes cross-platform consistency for this
quantized model. The first token differs from the original BF16 checkpoint,
so the current conversion needs an accuracy investigation before acceptance.
