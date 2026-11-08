#!/bin/sh
# tests/qemu/mm-tests.sh — QEMU regression tests for kernel memory management.
#
# 1. Normal boots (256M, 128M, 512M RAM, EL2 entry) must report the device-tree RAM size, run
#    EXACTLY the expected number of in-kernel self-tests, pass all of them, and never panic.
# 2. Fail-closed boots: no device tree -> FATAL, allocator never initialised, no tests run.
#    The explicit DEV-ONLY fixed-RAM build must announce itself and then pass.
# 3. A device tree with a memory reserve map and /reserved-memory children (including no-map)
#    must boot, report its reservations, and keep every allocation out of them.
# 4. Deliberate-fault kernels (built separately with AURORA_FAULT_TEST=n) must raise a genuine
#    hardware exception with the expected class, fault status and fault address.
#
# A boot that produces no output, times out, panics when it should not, runs fewer or more tests
# than expected, or reports a wrong fault address is a FAILURE.
#
# Requires: aarch64-linux-gnu toolchain, qemu-system-aarch64, a host C compiler.
# Usage: ./tests/qemu/mm-tests.sh

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT" || exit 1

EXPECTED_TESTS=93      # (70 before the physical-target policy tests, +23) number of "[test]" lines a healthy boot prints (kept in sync with kernel/mm/selftest.c)

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

TIMED_OUT=0
run_qemu() { # kernel machine-opts memory logfile [extra qemu args...]
    kernel="$1"; machine="$2"; mem="$3"; log="$4"; shift 4
    : > "$log"; TIMED_OUT=1
    qemu-system-aarch64 -machine "$machine" -cpu cortex-a53 -smp 1 -m "$mem" -kernel "$kernel" \
        -nographic -serial file:"$log" -monitor none "$@" &
    pid=$!
    i=0
    while [ "$i" -lt 240 ]; do
        if grep -qE "Boot complete|KERNEL PANIC|SELFTEST FAILED|did not fault|Kernel halted" "$log" 2>/dev/null; then TIMED_OUT=0; break; fi
        kill -0 "$pid" 2>/dev/null || { TIMED_OUT=0; break; }
        sleep 0.5; i=$((i + 1))
    done
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
}

no_timeout() { # logname
    if [ "$TIMED_OUT" -eq 1 ]; then bad "$1: timed out waiting for the kernel to finish"; else ok "$1: finished without timing out"; fi
}
produced_output() {
    if [ -s "$2" ]; then ok "$1: kernel produced serial output"; else bad "$1: silent boot failure (no serial output)"; fi
}
healthy() { # logname log ramsize
    expect "$2" "Welcome to AuroraOS" "$1: boot banner"
    expect "$2" "Device tree at 0x[0-9a-f]+: RAM base 0x0000000040000000 size $3" "$1: RAM size detected from device tree"
    expect "$2" "Exception vectors installed at EL1" "$1: running at EL1"
    expect "$2" "MMU enabled" "$1: MMU enabled"
    n="$(grep -c '^\[test\]' "$2")"
    if [ "$n" -eq "$EXPECTED_TESTS" ]; then ok "$1: ran exactly $EXPECTED_TESTS tests"; else bad "$1: ran $n tests, expected $EXPECTED_TESTS (incomplete or extra execution)"; fi
    expect "$2" "AURORA-MM-SELFTEST: $EXPECTED_TESTS passed, 0 failed" "$1: all $EXPECTED_TESTS in-kernel tests pass"
    expect_not "$2" "\[test\].*FAIL" "$1: no failing test"
    expect "$2" "Memory management OK" "$1: memory management OK"
    expect "$2" "\[info\] no-map regions policy-tested: 0" "$1: policy tests ran (default device tree has no no-map region, so that case is vacuous here)"
    expect "$2" "\[test\] policy: RW alias of the first kernel-text page is refused \(TEXT_WRITE\): PASS" "$1: writable alias of kernel text refused"
    expect "$2" "\[test\] policy: RW alias of the first rodata page is refused \(RODATA_WRITE\): PASS" "$1: writable alias of rodata refused"
    expect "$2" "Boot complete" "$1: reached the end of boot"
    expect_not "$2" "KERNEL PANIC|WARNING|FATAL|DEV MODE" "$1: no panic, warning or dev-mode banner"
}

normal_boot() { # name machine mem expected-size-hex
    name="$1"; log="$OUT/$name.log"
    echo "== normal boot: $name =="
    BUILD_DIR="$OUT/normal" ./build.sh >/dev/null 2>&1 || { bad "$name: build failed"; return; }
    run_qemu "$OUT/normal/kernel.bin" "$2" "$3" "$log"
    no_timeout "$name"; produced_output "$name" "$log"
    healthy "$name" "$log" "$4"
}

normal_boot "ram-256M"     "virt,highmem=off"                 256M 0x0000000010000000
normal_boot "ram-128M"     "virt,highmem=off"                 128M 0x0000000008000000
normal_boot "ram-512M"     "virt,highmem=off"                 512M 0x0000000020000000
normal_boot "el2-entry"    "virt,highmem=off,virtualization=on" 256M 0x0000000010000000

# ---- Fail-closed memory detection -------------------------------------------
echo "== no device tree: refuse to guess the memory map =="
# QEMU passes no DTB to an ELF kernel (x0 = 0), which is exactly "device tree missing".
run_qemu "$OUT/normal/kernel.elf" "virt,highmem=off" 256M "$OUT/no-dtb.log"
no_timeout "no-dtb"; produced_output "no-dtb" "$OUT/no-dtb.log"
expect     "$OUT/no-dtb.log" "FATAL: invalid or missing device tree" "no-dtb: kernel reports the missing device tree"
expect     "$OUT/no-dtb.log" "Kernel halted; memory manager NOT initialised" "no-dtb: kernel halts"
expect_not "$OUT/no-dtb.log" "Physical memory manager ready|\[test\]|MMU enabled|Boot complete|RAM base" "no-dtb: allocator, MMU and tests never ran"

echo "== explicit DEV-ONLY fixed-RAM build =="
AURORA_FIXED_RAM=1 BUILD_DIR="$OUT/fixed" ./build.sh >/dev/null 2>&1 || bad "fixed-ram: build failed"
run_qemu "$OUT/fixed/kernel.elf" "virt,highmem=off" 256M "$OUT/fixed.log"
no_timeout "fixed-ram"; produced_output "fixed-ram" "$OUT/fixed.log"
expect "$OUT/fixed.log" "DEV MODE \(AURORA_FIXED_RAM\)" "fixed-ram: loud DEV MODE banner"
expect "$OUT/fixed.log" "AURORA-MM-SELFTEST: $EXPECTED_TESTS passed, 0 failed" "fixed-ram: still passes every test"
echo "== the normal build has no fixed-RAM fallback =="
if strings "$OUT/normal/kernel.elf" | grep -q "DEV MODE"; then bad "normal build contains the dev-mode fallback"; else ok "normal build contains no dev-mode fallback"; fi

# ---- Device tree with reservations ----------------------------------------
echo "== device tree with reserve map + /reserved-memory (incl. no-map) =="
HOST="$OUT/host"; mkdir -p "$HOST"
cc -std=c11 -O1 -I kernel/include -o "$HOST/fdt_pmm_test" tests/kernel-host/fdt_pmm_test.c kernel/mm/fdt.c kernel/mm/pmm.c \
    && "$HOST/fdt_pmm_test" --emit "$HOST" >/dev/null && ok "host tool emitted reserved.dtb" || bad "could not emit reserved.dtb"
run_qemu "$OUT/normal/kernel.bin" "virt,highmem=off" 256M "$OUT/reserved.log" -dtb "$HOST/reserved.dtb"
no_timeout "reserved-dtb"; produced_output "reserved-dtb" "$OUT/reserved.log"
expect "$OUT/reserved.log" "reserved regions: 3" "reserved-dtb: all 3 reservations (1 reserve-map + 2 reserved-memory) were parsed"
expect "$OUT/reserved.log" "AURORA-MM-SELFTEST: $EXPECTED_TESTS passed, 0 failed" "reserved-dtb: allocator and MMU tests pass with the reservations"
expect "$OUT/reserved.log" "\[test\] pmm: exhaustion never hands out the device-tree blob or its reserved regions: PASS" "reserved-dtb: no allocation fell inside a reserved region"
expect "$OUT/reserved.log" "\[info\] no-map regions policy-tested: 1" "reserved-dtb: the policy was tested against the real no-map region (1 region)"
expect "$OUT/reserved.log" "\[test\] policy: no-map memory is refused for every attribute, also inside mixed requests, and nothing is installed: PASS" "reserved-dtb: aliases of no-map memory refused for every attribute"
expect "$OUT/reserved.log" "\[test\] mmu: no-map regions are unmapped, other reserved regions stay mapped: PASS" "reserved-dtb: the no-map region is unmapped"
expect_not "$OUT/reserved.log" "KERNEL PANIC|FATAL|\[test\].*FAIL" "reserved-dtb: no panic"

# ---- Deliberate fault tests ------------------------------------------------
NM=aarch64-linux-gnu-nm
sym() { $NM "$1" | awk -v s="$2" '$3 == s { print $1 }'; }   # nm prints 16 hex digits for 64-bit ELF

fault_case() { # n description EC FSC-hex fsc-desc symbol-for-FAR(or -) access
    n="$1"; name="fault-$n"; dir="$OUT/$name"; log="$OUT/$name.log"
    echo "== fault test $n: $2 =="
    AURORA_FAULT_TEST="$n" BUILD_DIR="$dir" ./build.sh >/dev/null 2>&1 || { bad "$name: build failed"; return; }
    run_qemu "$dir/kernel.bin" "virt,highmem=off" 256M "$log"
    no_timeout "$name"; produced_output "$name" "$log"
    expect "$log" "Memory management OK" "$name: memory management passed before the fault"
    expect "$log" "\[fault-test\] case $n:" "$name: fault test started"
    expect "$log" "\[FAULT\] EC=0x0*$3 " "$name: exception class $3"
    expect "$log" "\[FAULT\] FSC=0x0*$4 " "$name: fault status $4 ($5)"
    [ -n "$7" ] && expect "$log" "access=$7" "$name: access type $7"
    if [ "$6" != "-" ]; then
        addr="$(sym "$dir/kernel.elf" "$6")"
        if [ -n "$addr" ]; then expect "$log" "\[FAULT\] FAR=0x$addr" "$name: fault address is $6 (0x$addr)"
        else bad "$name: could not resolve symbol $6"; fi
    fi
    expect "$log" "KERNEL PANIC" "$name: kernel reports the fault and halts"
    expect_not "$log" "did not fault|Boot complete" "$name: execution did not continue past the fault"
}

#            n desc                                 EC  FSC description                symbol          access
fault_case   1 "read unmapped 0xDEAD0000"           25  05  "translation fault level 1" -              read
fault_case   2 "write to kernel text (read-only)"   25  0f  "permission fault level 3"  __text_start   write
fault_case   3 "execute from data page (W^X)"       21  0f  "permission fault level 3"  -              ""
fault_case   4 "touch the stack guard page"         25  07  "translation fault level 3" __stack_guard  read
fault_case   5 "write to rodata"                    25  0f  "permission fault level 3"  __rodata_start write

# Case 1 FAR must be the exact unmapped address.
expect "$OUT/fault-1.log" "\[FAULT\] FAR=0x00000000dead0000" "fault-1: fault address is 0xDEAD0000"

echo
echo "mm-tests: $PASSES passed, $FAILS failed"
[ "$FAILS" -eq 0 ]
