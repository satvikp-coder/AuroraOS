# Qwen3-0.6B host benchmark (HOST-ONLY, issue #12 preparation)

**This measures the model on a development PC under WSL. It says nothing about AuroraOS: no EL0 runtime, large mapping, FP/SIMD or memory exists for it there, and issue #12 stays open.** Raw proposals and metrics: `tests/aura-eval/results/`. Harness: `bench_llama.py`, judged by `run_eval.py` (the real validator and policy; the model is an untrusted proposer and nothing it returned was executed).

## Provenance (as actually done)
| Item | Value |
|---|---|
| Model repo / revision | `Qwen/Qwen3-0.6B-GGUF` @ `23749fefcc72300e3a2ad315e1317431b06b590a` |
| Source file | `Qwen3-0.6B-Q8_0.gguf`, 639,446,688 bytes, SHA-256 `9465e63a22add5354d9bb4b99e90117043c7124007664907259bd16d043bb031` (equals the Hugging Face LFS etag) |
| Runtime | `ggml-org/llama.cpp` tag `v1.0.0`, commit `b1b770d90d94cad9e9ea5b993bdbc383a522fb28`, CPU only (`GGML_NATIVE=ON`, no OpenMP), built with a static CMake 3.31.6 |
| Quantization | `llama-quantize --allow-requantize Qwen3-0.6B-Q8_0.gguf Qwen3-0.6B-Q4_K_M.gguf Q4_K_M 4` |
| Result file | `Qwen3-0.6B-Q4_K_M.gguf`, 396,704,416 bytes, SHA-256 `15e434d8ddf0af38fe3f7003af6ce6eac4edada6df34324aa45979f9cc79f02e` |
| **Deviation** | The official release has no f16 or Q4_K_M file. This Q4_K_M was **requantized from the official Q8_0**, not converted from the safetensors via f16, so it is not the build `qwen3-local-feasibility.md` recommends and its quality may differ slightly. The safetensors route (needs Python torch) was not done. |
| Decoding | greedy (temperature 0, seed 1), JSON-schema-constrained, thinking disabled, max 80 tokens, context 2048, 4 threads, 11 few-shot examples taken only from the dev split |
| Host | WSL2 distro on a 4-vCPU, 3.9 GB WSL RAM Windows PC (not otherwise idle: other tools were running but no QEMU or Playwright) |

## Results (115 held-out examples)
| | all | navigation | space | settings | unsupported | destructive | multistep |
|---|---|---|---|---|---|---|---|
| schema valid | 85.2% | 86.7% | 100% | 92.3% | 62.5% | 75.0% | 100% |
| intent accuracy | **41.7%** | 26.7% | 50.0% | 61.5% | 18.8% | 75.0% | 14.3% |
| decision accuracy | 57.4% | 33.3% | 50.0% | 61.5% | 18.8% | 75.0% | 14.3% |

* **UNSAFE proposals: 11 of 115; false authorisations: 11; identity false authorisations: 0.** The 11 are proposals that the real pipeline would let through as an S0 navigation where the dataset says nothing may proceed (for example answering a two-command request or a misleading question with `open_screen`). They are harmless in effect (navigation only) but are exactly the failure the gate exists for: **a model must not be wired in as it is.** The forged, stale and right-less requesters were never allowed.
* Latency (end to end per request, includes the ~600-token few-shot prompt): median 4.35 s, p95 7.97 s. Prompt processing median 0.69 s; generation about 8.8 tokens/s.
* Memory: RSS 871 MB after load, **924 MB peak** (the 397 MB weights plus KV cache and runtime on the host).
* For comparison the deterministic parser scores intent 80.7%, 0 unsafe on all 140 examples (`qwen3-evaluation.md`).

## What this means
At this size, with this prompt and requantized weights, the model is **worse than the deterministic parser on every safety-relevant column** and slow on a CPU. It could only ever be a proposer behind the validator and policy, and the 11 unsafe proposals show it needs either a better prompt, the f16-derived Q4_K_M, or a stricter rule that model proposals for non-actionable text never reach S0. No claim is made about prompt-tuned or fine-tuned performance; the held-out set was not used for tuning.

## Still blocking native inference
Weights alone (397 MB) exceed a 256 MiB guest; EL0 has no large mappings or file-backed mmap; FP/SIMD in EL0 is not enabled; no native runtime or tokenizer exists. Nothing in this document changes that.
