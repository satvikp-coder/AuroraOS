# Model volume trust and signing: design (not implemented)

Status: **design only.** Nothing in this document exists in the kernel. Today the model volume is **integrity-checked, not authenticated** (see `model-asset-store.md`). No code, key or signature path was added with this document, and no statement elsewhere may call a model "signed", "authentic" or "trusted" until the steps below are implemented and tested.

## 1. Threat model
Assets: the weights and tokenizer a native inference runtime would read; the decisions made from its output.
Attackers considered:
- **A: offline image tamperer.** Rewrites the model disk image (a malicious download, a shared host). Can recompute every unkeyed SHA-256 in the AURMODEL table. Defeats today's check completely.
- **B: downgrade.** Replaces a newer model with an older, genuinely signed one that has known problems.
- **C: key theft / substitution.** Swaps the verification key, or steals the signing key.
- **D: malicious but correctly signed model.** Signing proves origin, not safety. Model output stays untrusted data (already enforced: nothing acts on it directly; see `model-asset-store.md`).
Out of scope: a compromised kernel image (it holds the trust root), physical attacks, side channels.

## 2. What each mechanism proves
| Mechanism | Detects | Does not detect |
|---|---|---|
| Structural validation (`ms_parse`) | malformed header/table, overlap, out-of-range | any deliberate edit that stays well-formed |
| Unkeyed SHA-256 per object (today) | accidental damage, truncation, mix-ups | an attacker who recomputes digests |
| Digest allowlist compiled into the kernel | any change to a known object set | new models need a kernel rebuild; no revocation |
| Signature over the header digest (proposed) | any edit by a party without the private key | key compromise, downgrade (needs section 4), malicious signed content |

## 3. Proposed scheme
1. **Sign the table, not the objects.** The header already commits to every object through its digests. A detached signature over the SHA-256 of the header+table bytes therefore covers the whole volume. Stored in a new fixed-size trailer region declared by a header version bump (AURMODEL v2); v1 volumes stay valid as "integrity only" and must never be reported as authenticated.
2. **Algorithm: Ed25519 (RFC 8032).** Small, deterministic, no per-signature randomness (no RNG needed in the kernel), 32-byte public key, 64-byte signature. Chosen over RSA/ECDSA to avoid padding and nonce pitfalls.
3. **Vetted implementation only.** No home-made field arithmetic. Candidates must be a small, auditable, license-compatible, constant-time-by-construction implementation that ships RFC 8032 test vectors (for example the reference-derived "ref10"-style code used by TweetNaCl/Monocypher). The choice is a design-review item; the commit that adds it must include the full RFC 8032 section 7.1 vectors as a host test plus a mutation check (flip one bit of message, key and signature each: must reject).
4. **Trust root.** One or more public keys compiled into the kernel image (a small table with key ids). The kernel image is already the root of trust; it is built and tested in CI. Key rotation = kernel update. No key is ever read from the model volume itself.
5. **Verification point.** At `modelvol_check` time, after the existing integrity pass: verify the signature over the header digest against the compiled-in keys. New state `AUTHENTICATED` only if both pass; `SYS_MODEL` INFO exposes the state so a runtime can refuse unauthenticated volumes by policy. The boot line distinguishes `integrity ok` from `authenticated (key id N)`.
6. **Fail closed.** Bad signature, unknown key id, or malformed trailer yields INVALID (not "integrity only"), unless an explicit development build flag allows unauthenticated volumes; that flag prints a loud boot line and is never set in CI release paths.

## 4. Downgrade and revocation
- A signed monotonic **model version** and a **minimum acceptable version** compiled into the kernel (raise it by kernel update). A signed older model below the minimum is refused.
- Revocation is by key id: removing a key from the compiled table revokes everything it signed. There is no online revocation (no network, by design).
- Not solved: a device with persistent storage that must remember a version floor across kernel updates. Out of scope until AuroraFS has an authenticated metadata area.

## 5. Signing tool and key handling
- Host-side `tests/tools/` signer (separate from `mkmodel`): reads a private key from a file path, never from the command line, never from the repository.
- **CI uses a throwaway test key** generated per run; the real signing key is never in CI, in the repository, or on a developer machine used for builds that run untrusted code. Real key custody is the maintainer's decision and is explicitly **not** made here.
- The test key's public half is compiled in only under a test build flag; a production build with the test key must fail a build-time check.

## 6. Test plan (acceptance for the implementation PR)
1. Host: RFC 8032 vectors; tampered message/key/signature rejected; truncated and oversize trailers rejected; every parser bound fuzz-tested like `modelstore` (existing host harness style).
2. QEMU (`model-tests.sh` extension): signed valid volume -> `authenticated`; same volume with one weight byte changed and digests **recomputed** -> INVALID signature (this is the exact attack that passes today); unknown key id; wrong key; v1 volume -> `integrity only`, never `authenticated`; downgraded version -> refused.
3. The existing `KNOWN LIMIT` test (digest-recompute attack succeeds) stays for v1 and gains a v2 counterpart that must **fail**.
4. Docs and boot output keep the two words apart: never "authentic" for v1.

## 7. Explicitly not decided here
Which Ed25519 implementation to vendor; who holds the signing key; whether models will ever be installed from inside the OS (that needs a separate install-path design with permission prompts); whether to also encrypt models. Each needs the maintainer's decision before code.
