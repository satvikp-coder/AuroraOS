#!/bin/sh
# tests/qemu/mm-tests.sh — QEMU regression tests for kernel memory management.
#
# 1. Normal boots (256M, 128M, 512M RAM, and EL2-entry) must report the device-tree RAM size,
#    pass every in-kernel self-test and print "Memory management OK".
# 2. Deliberate-fault kernels (built separately with AURORA_FAULT_TEST=n) must raise a genuine
#    hardware exception with the expected exception class, fault status and fault address.
#
# Requires: aarch64-linux-gnu toolchain, qemu-system-aarch64.   Usage: ./tests/qemu/mm-tests.sh

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT" || exit 1

OUT="$ROOT/build/mm-tests"
rm -rf "$OUT"; mkdir -p "$OUT"
FAILS=0; PASSES=0

ok()   { PASSES=$((PASSES + 1)); echo "  PASS: $1"; }
bad()  { FAILS=$((FAILS + 1));   echo "  FAIL: $1"; }
expect() { # log pattern description
    if grep -Eq -- "$2" "$1"; then ok "$3"; else bad "$3 (pattern: $2)"; fi
}
expect_not() {
    if grep -Eq -- "$2" "$1"; then bad "$3 (found: $2)"; else ok "$3"; fi
}

run_qemu() { # kernel.bin machine-opts memory logfile
    : > "$4"
    qemu-system-aarch64 -machine "$2" -cpu cortex-a53 -smp 1 -m "$3" -kernel "$1" \
        -nographic -serial file:"$4" -monitor none &
    pid=$!
    i=0
    while [ "$i" -lt 60 ]; do
        grep -qE "Boot complete|KERNEL PANIC|SELFTEST FAILED|did not fault" "$4" 2>/dev/null && break
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.5; i=$((i + 1))
    done
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
}

normal_boot() { # name machine mem expected-size-hex
    name="$1"; log="$OUT/$name.log"
    echo "== normal boot: $name =="
    BUILD_DIR="$OUT/normal" ./build.sh >/dev/null 2>&1 || { bad "$name: build failed"; return; }
    run_qemu "$OUT/normal/kernel.bin" "$2" "$3" "$log"
    expect "$log" "Welcome to AuroraOS" "$name: boot banner"
    expect "$log" "Device tree at 0x[0-9a-f]+: RAM base 0x0000000040000000 size $4" "$name: RAM size detected from device tree"
    expect "$log" "Exception vectors installed at EL1" "$name: running at EL1"
    expect "$log" "MMU enabled" "$name: MMU enabled"
    expect "$log" "AURORA-MM-SELFTEST: 45 passed, 0 failed" "$name: all 45 in-kernel tests pass"
    expect_not "$log" "\[test\].*FAIL" "$name: no failing test"
    expect "$log" "Memory management OK" "$name: memory management OK"
    expect_not "$log" "KERNEL PANIC|WARNING" "$name: no panic or warning"
}

normal_boot "ram-256M"     "virt,highmem=off"                 256M 0x0000000010000000
normal_boot "ram-128M"     "virt,highmem=off"                 128M 0x0000000008000000
normal_boot "ram-512M"     "virt,highmem=off"                 512M 0x0000000020000000
normal_boot "el2-entry"    "virt,highmem=off,virtualization=on" 256M 0x0000000010000000

# ---- Deliberate fault tests ------------------------------------------------
NM=aarch64-linux-gnu-nm
sym() { $NM "$1" | awk -v s="$2" '$3 == s { print $1 }'; }   # nm prints 16 hex digits for 64-bit ELF

fault_case() { # n description EC FSC-hex access symbol-for-FAR(or -)
    n="$1"; name="fault-$n"; dir="$OUT/$name"; log="$OUT/$name.log"
    echo "== fault test $n: $2 =="
    AURORA_FAULT_TEST="$n" BUILD_DIR="$dir" ./build.sh >/dev/null 2>&1 || { bad "$name: build failed"; return; }
    run_qemu "$dir/kernel.bin" "virt,highmem=off" 256M "$log"
    expect "$log" "Memory management OK" "$name: memory management passed before the fault"
    expect "$log" "\[fault-test\] case $n:" "$name: fault test started"
    expect "$log" "\[FAULT\] EC=0x0*$3 " "$name: exception class $3"
    expect "$log" "\[FAULT\] FSC=0x0*$4 " "$name: fault status $4 ($5)"
    [ -n "$7" ] && expect "$log" "access=$7" "$name: access type $7"
    if [ "$6" != "-" ]; then
        addr="$(sym "$dir/kernel.elf" "$6")"
        [ -n "$addr" ] || bad "$name: could not resolve symbol $6"
        expect "$log" "\[FAULT\] FAR=0x$addr" "$name: fault address is $6 (0x$addr)"
    fi
    expect "$log" "KERNEL PANIC" "$name: kernel reports the fault and halts"
    expect_not "$log" "did not fault|Boot complete" "$name: execution did not continue past the fault"
}

#            n desc                                 EC  FSC description                access  FAR-symbol
fault_case   1 "read unmapped 0xDEAD0000"           25  05  "translation fault level 1" -        read
fault_case   2 "write to kernel text (read-only)"   25  0f  "permission fault level 3"  __text_start write
fault_case   3 "execute from data page (W^X)"       21  0f  "permission fault level 3"  -        ""
fault_case   4 "touch the stack guard page"         25  07  "translation fault level 3" __stack_guard read
fault_case   5 "write to rodata"                    25  0f  "permission fault level 3"  __rodata_start write

# Case 1 FAR must be the exact unmapped address.
expect "$OUT/fault-1.log" "\[FAULT\] FAR=0x00000000dead0000" "fault-1: fault address is 0xDEAD0000"

echo
echo "mm-tests: $PASSES passed, $FAILS failed"
[ "$FAILS" -eq 0 ]
