#!/bin/sh
# Host-side tests for the device-tree parser, allocator reservation logic and physical-target mapping policy.
# Needs only a C compiler. Built with AddressSanitizer + UBSan.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build/host-tests"
mkdir -p "$OUT"
CFLAGS="-std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -I $ROOT/kernel/include"
cc $CFLAGS -o "$OUT/fdt_pmm_test" "$ROOT/tests/kernel-host/fdt_pmm_test.c" "$ROOT/kernel/mm/fdt.c" "$ROOT/kernel/mm/pmm.c"
cc $CFLAGS -o "$OUT/policy_test" "$ROOT/tests/kernel-host/policy_test.c" "$ROOT/kernel/mm/mmpolicy.c"
"$OUT/fdt_pmm_test"
"$OUT/policy_test"
