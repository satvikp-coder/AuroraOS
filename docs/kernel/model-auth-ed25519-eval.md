# Ed25519 Implementation Evaluation and Trust-Root Separation

Status: **evaluation / design prep.** Follow-up to [`model-trust-design.md`](model-trust-design.md). No cryptographic code, keys, or signature verification paths are added with this document.

---

## 1. Freestanding C Ed25519 Implementation Evaluation

AuroraOS kernel verification runs in a freestanding C environment without `libc`, dynamic memory allocation (`malloc`), or a kernel CSPRNG (Ed25519 verification is deterministic). Per `model-trust-design.md` §3, only vetted, constant-time-by-construction implementations with RFC 8032 test vector coverage are considered.

| Candidate | License | Freestanding / Dependencies | Code Size (Verify-Only) | Stack Usage | Assessment |
|---|---|---|---|---|---|
| **Monocypher** (`crypto_ed25519_check` / `crypto_check`) | CC0-1.0 OR 2-clause BSD (dual) | Single `.c`/`.h` pair (`monocypher.c`, `monocypher.h`), zero external deps, no `malloc`, C99 | ~1,500–2,000 LOC (full file); verify-only subset is smaller; ~8–12 KB `.text` (x86_64 `-Os`) | ~1.5–2.5 KB | **Recommended.** Clean, well-audited, dual-licensed (permissive), explicit RFC 8032 / Ed25519 support, constant-time field arithmetic, and easy to trim or compile with `-ffunction-sections` / `--gc-sections`. |
| **TweetNaCl** (`crypto_sign_open`) | Public Domain (CC0-equivalent) | Single `.c`/`.h` pair, zero external deps, no `malloc` | ~700 LOC total; ~6–10 KB `.text` | ~2–4 KB | **Viable, but less ergonomic.** Very compact and public domain, but uses `crypto_sign_open` (attached signature) rather than detached `verify(sig, msg, pk)` without a wrapper, and its heavily compressed style is harder to audit. |
| **Compact ref10 / `orlp/ed25519`** (`ed25519_verify`) | zlib License | Small `.c`/`.h` set, no `malloc` for verify | ~1,200–1,800 LOC; ~10–15 KB `.text` | ~1.5–3 KB | **Viable.** Straightforward ref10-derived API with detached `ed25519_verify`; however, Monocypher has stronger modern audit history and simpler single-file vendoring. |

### Selection Criteria & Requirements
- **No custom field arithmetic:** Vendor the chosen implementation verbatim (or minimal verify-only subset) with upstream version/commit pinned.
- **No RNG / heap:** Only signature verification (`verify(sig, msg, msg_len, pk)`) is needed in the kernel.
- **Mandatory host tests at vendor time:**
  - Full RFC 8032 §7.1 test vectors.
  - Mutation checks: flipping any single bit in the message, public key, or signature must fail verification.

---

## 2. Test-Only vs. Production Trust Root Separation

Per `model-trust-design.md` §3 and §5, the kernel image is the root of trust and holds a compiled-in table of `(key_id, pubkey[32])` entries.

1. **Compile-Time Separation:**
   - Production builds compile only the production trust-root table (`AURORA_MODEL_TRUST_PROD`).
   - Test/CI builds may enable `AURORA_MODEL_TRUST_TEST` to include a throwaway CI test public key.
2. **Build-Time Guard:**
   - A preprocessor `#if` / `_Static_assert` guard must **fail the build** if `AURORA_MODEL_TRUST_TEST` (or any test key) is enabled in a non-test / release build configuration.
3. **Key Custody & Isolation:**
   - Production private signing keys are never stored in the repository, CI environment, or developer machines running untrusted code.
   - CI generates an ephemeral keypair per run (or uses a dedicated CI-only test key) for `tests/tools/` signing tests.
   - No public key is ever read from the model volume itself.
4. **Fail-Closed Policy:**
   - Unknown `key_id`, invalid signature, or malformed trailer yields `INVALID` (not `integrity only`), unless an explicit development-only build flag is set (which must emit a loud boot warning and be forbidden in CI release paths).

---

## 3. Negative Test Matrix

All cases below must be covered by host unit tests and/or QEMU `model-tests.sh` integration tests before enabling `AUTHENTICATED` state in `SYS_MODEL`.

| Category | Test Case | Expected Result | Target Layer |
|---|---|---|---|
| **Malformed Signatures** | Single-bit flip in 64-byte signature | Reject (`INVALID`) | Host + QEMU |
| **Malformed Signatures** | Single-bit flip in header/table bytes (with SHA-256 digests recomputed) | Reject (`INVALID`) — defeats v1 digest-recompute attack | Host + QEMU |
| **Malformed Signatures** | Single-bit flip in compiled-in or test public key | Reject (`INVALID`) | Host |
| **Malformed Signatures** | All-zero or non-canonical `R` / `S` signature components | Reject (`INVALID`) | Host |
| **Unknown Key IDs** | Trailer references `key_id` not present in compiled-in trust table | Reject (`INVALID`) | Host + QEMU |
| **Unknown Key IDs** | Volume signed by a revoked `key_id` (removed from kernel table) | Reject (`INVALID`) | QEMU |
| **Downgrade / Rollback** | Validly signed v2 volume with `model_version < MIN_MODEL_VERSION` | Reject (`INVALID` / refused) | Host + QEMU |
| **Downgrade / Rollback** | Validly signed v2 volume with `model_version == MIN_MODEL_VERSION` | Accept (`AUTHENTICATED`) | Host + QEMU |
| **Downgrade / Rollback** | Unauthenticated v1 volume (integrity-only) | Reported as `integrity only`, **never** `AUTHENTICATED` | QEMU |
| **Truncated Manifests** | Truncated AURMODEL v2 header or object table | Reject (`INVALID`) | Host |
| **Truncated Manifests** | Truncated signature trailer (fewer bytes than expected fixed trailer size) | Reject (`INVALID`) | Host |
| **Truncated Manifests** | Oversize or trailing garbage beyond declared trailer bounds | Reject (`INVALID`) | Host |
