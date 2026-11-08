#!/bin/sh
# Host-side tests for the device-tree parser and allocator reservation logic (needs only gcc).
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build/host-tests"
mkdir -p "$OUT"
cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -I "$ROOT/kernel/include" -o "$OUT/fdt_pmm_test" \
    "$ROOT/tests/kernel-host/fdt_pmm_test.c" "$ROOT/kernel/mm/fdt.c" "$ROOT/kernel/mm/pmm.c"
"$OUT/fdt_pmm_test"
