#!/bin/sh
# tests/qemu/process-tests.sh — QEMU regression tests for EL0 processes, syscalls, the scheduler and isolation.
#
# Every boot must: run EXACTLY the expected number of [proc-test] checks, pass all of them, print the kernel-tagged
# output lines of real EL0 programs, show the intentional user faults as "[proc N] FAULT at EL0" (never as a kernel
# panic), still pass the memory and interrupt suites, and finish ("Userspace OK", "Boot complete").
# A silent boot, a timeout, a wrong count, a kernel panic in a normal boot, or a missing fault line is a FAILURE.
# Fault case 8 then proves a real KERNEL fault after user processes have run still reaches the kernel panic path.
#
# Usage: ./tests/qemu/process-tests.sh     Requires the aarch64 toolchain and qemu-system-aarch64.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT" || exit 1

EXPECTED_PROC_TESTS=79   # keep in sync with kernel/proctest.c
EXPECTED_MM_TESTS=93
EXPECTED_IRQ_TESTS=43

OUT="$ROOT/build/process-tests"
rm -rf "$OUT"; mkdir -p "$OUT"
FAILS=0; PASSES=0
ok()  { PASSES=$((PASSES + 1)); echo "  PASS: $1"; }
bad() { FAILS=$((FAILS + 1));   echo "  FAIL: $1"; }
expect()     { if grep -Eq -- "$2" "$1"; then ok "$3"; else bad "$3 (pattern: $2)"; fi; }
expect_not() { if grep -Eq -- "$2" "$1"; then bad "$3 (found: $2)"; else ok "$3"; fi; }

TIMED_OUT=0
run_qemu() { # kernel machine memory log
    : > "$4"; TIMED_OUT=1
    qemu-system-aarch64 -machine "$2" -cpu cortex-a53 -smp 1 -m "$3" -kernel "$1" -nographic -serial file:"$4" -monitor none &
    pid=$!; i=0
    while [ "$i" -lt 240 ]; do
        if grep -qE "Boot complete|KERNEL PANIC|SELFTEST FAILED|did not fault|Kernel halted" "$4" 2>/dev/null; then TIMED_OUT=0; break; fi
        kill -0 "$pid" 2>/dev/null || { TIMED_OUT=0; break; }
        sleep 0.5; i=$((i + 1))
    done
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
}
finished() {
    if [ "$TIMED_OUT" -eq 1 ]; then bad "$1: timed out"; else ok "$1: finished without timing out"; fi
    if [ -s "$2" ]; then ok "$1: serial output produced"; else bad "$1: silent boot failure"; fi
}

normal_boot() { # name machine mem
    name="$1"; log="$OUT/$name.log"
    echo "== userspace, normal boot: $name =="
    BUILD_DIR="$OUT/normal" ./build.sh >/dev/null 2>&1 || { bad "$name: build failed"; return; }
    run_qemu "$OUT/normal/kernel.bin" "$2" "$3" "$log"
    finished "$name" "$log"
    n="$(grep -c '^\[proc-test\]' "$log")"
    if [ "$n" -eq "$EXPECTED_PROC_TESTS" ]; then ok "$name: ran exactly $EXPECTED_PROC_TESTS process checks"; else bad "$name: ran $n process checks, expected $EXPECTED_PROC_TESTS"; fi
    expect "$log" "AURORA-PROC-SELFTEST: $EXPECTED_PROC_TESTS passed, 0 failed" "$name: all process checks pass"
    expect_not "$log" "\[proc-test\].*FAIL" "$name: no failing process check"
    expect "$log" "AURORA-MM-SELFTEST: $EXPECTED_MM_TESTS passed, 0 failed" "$name: memory self-tests still pass"
    expect "$log" "AURORA-IRQ-SELFTEST: $EXPECTED_IRQ_TESTS passed, 0 failed" "$name: interrupt self-tests still pass"
    expect "$log" "\[proc-test\] EL0 execution: program A ran to completion at EL0.*: PASS" "$name: a real EL0 program ran (every exception arrived with SPSR=EL0t)"
    expect "$log" "\[proc-test\] scheduler: two processes alternate strictly.*: PASS" "$name: two processes were scheduled round-robin"
    expect "$log" "\[proc-test\] context switching preserves user registers.*: PASS" "$name: user registers survive context switches"
    expect "$log" "\[proc-test\] isolation: a process reading ANOTHER process's private page.*: PASS" "$name: another process's memory is unreachable"
    expect "$log" "\[proc-test\] syscall validation: kernel pointer.*: PASS" "$name: bad syscall arguments are rejected"
    expect "$log" "\[proc-test\] preemption: every switch between them was a TIMER switch.*: PASS" "$name: processes that never yield were switched by timer interrupts (0 yields)"
    expect "$log" "\[proc-test\] preemption: each timer interrupt hit the middle of user code.*: PASS" "$name: preemption landed mid-user-code and registers/SP_EL0 survived"
    expect "$log" "\[proc-test\] preemption: TTBR0_EL1 .*: PASS" "$name: TTBR0/ASID matched the running process on every EL0 exception"
    expect "$log" "\[proc-test\] preemption stress: .*: PASS" "$name: sustained preemption stress passed without leaks"
    expect "$log" "\[proc-test\] ipc: a blocking receiver parks as BLOCKED.*: PASS" "$name: blocking receive parks, is woken by a send, exact bytes in order"
    expect "$log" "\[proc-test\] ipc validation: oversized.*: PASS" "$name: IPC rejects oversized, bad pid, kernel pointer and unauthorised destinations"
    expect "$log" "\[proc-test\] ipc cleanup: a receiver that exits.*: PASS" "$name: a dead receiver's mailbox is freed and later sends fail"
    expect "$log" "\[proc-test\] ipc: a lone blocked receiver is reported as SCHED_DEADLOCK.*: PASS" "$name: a receiver nobody will wake is reported, not hung"
    expect "$log" "\[proc-test\] ipc authorisation: recipient exits normally, its pid is reused.*: PASS" "$name: a stale grant does not survive the recipient exiting and its pid being reused"
    expect "$log" "\[proc-test\] ipc authorisation: recipient faults, its pid is reused.*: PASS" "$name: ... nor the recipient faulting"
    expect "$log" "\[proc-test\] ipc authorisation: recipient is destroyed while BLOCKED, its pid is reused.*: PASS" "$name: ... nor destroying a BLOCKED recipient"
    expect "$log" "\[proc-test\] ipc authorisation: recipient is destroyed while READY.*: PASS" "$name: ... nor destroying a READY recipient"
    expect "$log" "\[proc-test\] ipc authorisation \(end to end\).*: PASS" "$name: end to end, an EL0 sender is refused (-EPERM) by the new occupant of a reused pid"
    expect "$log" "\[proc-test\] ipc authorisation: grants to nonexistent.*: PASS" "$name: grants to ineligible recipients are refused and leave nothing behind"
    expect "$log" "\[proc-test\] ipc authorisation: no system call lets a process grant itself permission.*: PASS" "$name: no system call can grant authority"
    expect "$log" "\[proc-test\] ipc authorisation: 20 rounds.*: PASS" "$name: grant/destroy/reuse churn never lets a stale grant through"
    expect "$log" "\[proc-test\] ipc: 12 rounds.*: PASS" "$name: repeated create/send/receive/destroy leaks nothing"
    expect "$log" "\[proc-test\] watchdog: .*: PASS" "$name: a never-yielding loop cannot hang the kernel"
    c="$(grep -c '^\[proc [0-9]*\] FAULT at EL0' "$log")"
    if [ "$c" -eq 15 ]; then ok "$name: the 15 intentional user faults were each reported as an EL0 fault"; else bad "$name: saw $c EL0 fault reports, expected 15"; fi
    expect "$log" "^\[user p1\] A" "$name: kernel-tagged output of an EL0 program (A) reached the console"
    expect "$log" "^\[user p2\] B" "$name: kernel-tagged output of an EL0 program (B) reached the console"
    expect "$log" "Userspace OK" "$name: userspace reported OK"
    expect "$log" "Boot complete" "$name: boot completed"
    expect_not "$log" "KERNEL PANIC|FATAL|WARNING|KERNEL BUG|\[FAULT\]" "$name: no kernel panic, kernel bug or kernel fault (user faults are not kernel faults)"
}
normal_boot ram-128M  "virt,highmem=off"                   128M
normal_boot ram-256M  "virt,highmem=off"                   256M
normal_boot ram-512M  "virt,highmem=off"                   512M
normal_boot el2-entry "virt,highmem=off,virtualization=on" 256M

echo "== fault 8: a real kernel fault after user processes have run =="
AURORA_FAULT_TEST=8 BUILD_DIR="$OUT/f8" ./build.sh >/dev/null 2>&1 || bad "fault-8: build failed"
run_qemu "$OUT/f8/kernel.bin" "virt,highmem=off" 256M "$OUT/f8.log"
finished "fault-8" "$OUT/f8.log"
expect "$OUT/f8.log" "Userspace OK" "fault-8: user processes ran first"
expect "$OUT/f8.log" "\[fault-test\] case 8:" "fault-8: kernel fault test started"
expect "$OUT/f8.log" "\[FAULT\] exception sync/EL1t" "fault-8: reported as a KERNEL exception"
expect "$OUT/f8.log" "\[FAULT\] EC=0x0*25 " "fault-8: kernel data abort"
expect "$OUT/f8.log" "\[FAULT\] FAR=0x00000000dead0000" "fault-8: exact fault address"
expect "$OUT/f8.log" "KERNEL PANIC" "fault-8: the kernel panics (a kernel fault is never 'recovered' like a user fault)"
expect_not "$OUT/f8.log" "did not fault|Boot complete" "fault-8: execution did not continue"

echo "== production configuration compiles without the test-only SVC hook =="
if AURORA_PRODUCTION=1 BUILD_DIR="$OUT/prod" ./build.sh >/dev/null 2>&1; then ok "production build (no AURORA_SELFTEST_HOOKS) links"; else bad "production build failed"; fi

echo
echo "process-tests: $PASSES passed, $FAILS failed"
[ "$FAILS" -eq 0 ]
