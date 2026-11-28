# Monocypher 4.0.3

AuroraOS host tests call the optional SHA-512 Ed25519 verifier, `crypto_ed25519_check`. Its detached-signature API returns zero on success. The optional module uses arithmetic from the core module. The vendored source files and licence come verbatim from the official release archive. Upstream offers BSD-2-Clause or CC0-1.0. [Licence](LICENCE.md)

The release archive SHA-512 matched the checksum on the upstream download site. The archive also matched the GitHub release asset SHA-256. The tag resolves directly to the unsigned commit `ab2b16dd619ad5f6979a4fbe69cfa324a6fcc35f`. The four release source files match that commit after substituting the packaged version string for `__git__` and normalizing checkout line endings. [Upstream releases and checksums](https://monocypher.org/download/) | [GitHub release](https://github.com/LoupVaillant/Monocypher/releases/tag/4.0.3)

```text
SHA-512 40904ada5c7ee4f7741733e38b69a30a4b0561cbffba5ffe7c2dce16136d540251ec0d9056ff606510d3b5b708fb8a40db7e0870d4a0b2dc17ba2bfb880f8965
SHA-256 8cc9bc341a66249016db9bd70e9142d8d0aef9945973744b1ac05dbc55d8ee66
```

Upstream lists 4.0.3 as fixing its June 2026 EdDSA timing disclosure. Recheck disclosures when updating the pin. [Upstream advisory history](https://monocypher.org/quality-assurance/disclosures)

The host suite exercises the five pure Ed25519 vectors, every single-bit mutation of the supplied keys, messages and signatures, an out-of-range signature scalar and all-zero signatures. Only public vector fields appear in the test. [RFC 8032 section 7.1](https://www.rfc-editor.org/rfc/rfc8032#section-7.1)

To reproduce the standalone AArch64 link check from the repository root, run the following. The kernel supplies the compiler-generated call to `memset`. The linker retains the verifier and its dependencies. The output is a link probe.

```sh
src=third_party/monocypher/src
aarch64-linux-gnu-gcc -Os -std=c99 -ffreestanding -fno-builtin \
    -fno-tree-loop-distribute-patterns -fno-stack-protector -mstrict-align \
    -mgeneral-regs-only -ffunction-sections -fdata-sections -nostdlib -static \
    -Wl,--gc-sections,-e,crypto_ed25519_check -I"$src" "$src/monocypher.c" \
    "$src/optional/monocypher-ed25519.c" kernel/lib.c -o /tmp/ed25519-link.elf
aarch64-linux-gnu-nm -u /tmp/ed25519-link.elf
```
