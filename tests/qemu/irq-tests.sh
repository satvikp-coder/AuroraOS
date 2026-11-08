#!/bin/sh
# tests/qemu/irq-tests.sh — QEMU regression tests for interrupts, the generic timer and the exception stack.
#
# Normal boots (several RAM sizes, plus entry at EL2) must report the GICv2/timer description taken from
# the device tree, run EXACTLY the expected number of in-kernel [irq-test] checks (each waits for a real
# emulated interrupt), pass them all, still pass every memory self-test, and never panic.
# Two deliberate-fault kernels then prove the exception-stack design:
#   case 6: a genuine kernel-stack overflow (recursion into the guard page): diagnostics are printed from
#           the SEPARATE exception stack, FAR/SP identify the guard page.
#   case 7: a fault inside the timer interrupt handler: reported as a NESTED exception with the exact address.
# Silent boots, timeouts, a wrong test count, missing evidence or an unexpected panic are FAILURES.
#
# Usage: ./tests/qemu/irq-tests.sh      Requires the aarch64 toolchain and qemu-system-aarch64.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT" || exit 1

EXPECTED_IRQ_TESTS=43   # keep in sync with kernel/irqtest.c
EXPECTED_MM_TESTS=93    # memory self-tests must still all run and pass

OUT="$ROOT/build/irq-tests"
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

normal_boot() { # name machine mem expected-entry-info
    name="$1"; log="$OUT/$name.log"
    echo "== interrupts + timer, normal boot: $name =="
    BUILD_DIR="$OUT/normal" ./build.sh >/dev/null 2>&1 || { bad "$name: build failed"; return; }
    run_qemu "$OUT/normal/kernel.bin" "$2" "$3" "$log"
    finished "$name" "$log"
    expect "$log" "\[info\] boot entry: $4[[:space:]]*$" "$name: observed entry state is $4 (what QEMU really did, not assumed)"
    expect "$log" "GICv2 at 0x0000000008000000/0x0000000008010000, timer frequency [0-9]+ Hz, interrupt ID 30" "$name: GICv2 and timer taken from the device tree"
    n="$(grep -c '^\[irq-test\]' "$log")"
    if [ "$n" -eq "$EXPECTED_IRQ_TESTS" ]; then ok "$name: ran exactly $EXPECTED_IRQ_TESTS interrupt/timer checks"; else bad "$name: ran $n interrupt checks, expected $EXPECTED_IRQ_TESTS"; fi
    expect "$log" "AURORA-IRQ-SELFTEST: $EXPECTED_IRQ_TESTS passed, 0 failed" "$name: all interrupt/timer checks pass"
    expect_not "$log" "\[irq-test\].*FAIL" "$name: no failing interrupt check"
    expect "$log" "\[irq-test\] masking: with IRQs masked .*handler has NOT run: PASS" "$name: masked IRQ stays pending and is not taken"
    expect "$log" "\[irq-test\] unmasking: enabling IRQs delivers exactly one interrupt.*: PASS" "$name: unmasking delivers exactly one interrupt"
    expect "$log" "\[irq-test\] timer: 1 kHz periodic ticks are delivered by real interrupts.*: PASS" "$name: periodic ticks arrive by interrupt"
    expect "$log" "\[irq-test\] handler context: runs on the separate exception stack.*: PASS" "$name: handler runs on the separate exception stack"
    expect "$log" "\[irq-test\] irq: unknown interrupts .*: PASS" "$name: unknown interrupts handled safely"
    expect "$log" "\[irq-test\] irq: spurious/reserved IDs are counted.*: PASS" "$name: spurious interrupts handled safely"
    expect "$log" "AURORA-MM-SELFTEST: $EXPECTED_MM_TESTS passed, 0 failed" "$name: memory self-tests still all pass"
    expect "$log" "Interrupts and timer OK" "$name: interrupt subsystem reported OK"
    expect "$log" "Boot complete" "$name: boot completed"
    expect_not "$log" "KERNEL PANIC|FATAL|WARNING|\[FAULT\]" "$name: no panic, fatal error or fault"
}
normal_boot ram-256M  "virt,highmem=off"                   256M "EL1, SPSel=1"
normal_boot ram-512M  "virt,highmem=off"                   512M "EL1, SPSel=1"
normal_boot el2-entry "virt,highmem=off,virtualization=on" 256M "EL2, SPSel=1"

# Controlled entry variants (tests only, see build.sh): the CPU REALLY arrives with SPSel=0, which the default QEMU
# entry never does. start.S must still produce correct stacks, so the whole suite (which reads SP_EL1, SP_EL0 and
# takes real exceptions and interrupts) has to pass.
entry_variant() { # name AURORA_TEST_ENTRY machine expected-entry-info
    name="$1"; log="$OUT/$name.log"
    echo "== controlled boot entry: $name =="
    AURORA_TEST_ENTRY="$2" BUILD_DIR="$OUT/$name" ./build.sh >/dev/null 2>&1 || { bad "$name: build failed"; return; }
    run_qemu "$OUT/$name/kernel.bin" "$3" 256M "$log"
    finished "$name" "$log"
    expect "$log" "\[info\] boot entry: $4 \(CONTROLLED TEST ENTRY MODE" "$name: the CPU really arrived as $4"
    expect "$log" "\[irq-test\] boot stacks: .*: PASS" "$name: SP_EL1/SP_EL0 initialised correctly despite the unusual entry state"
    expect "$log" "\[irq-test\] boot entry: .*: PASS" "$name: entry state matched the declared variant"
    expect "$log" "\[irq-test\] exceptions: a synchronous exception from kernel code preserves.*: PASS" "$name: exception round trip preserves registers"
    n="$(grep -c '^\[irq-test\]' "$log")"
    if [ "$n" -eq "$EXPECTED_IRQ_TESTS" ]; then ok "$name: ran all $EXPECTED_IRQ_TESTS interrupt checks"; else bad "$name: ran $n checks, expected $EXPECTED_IRQ_TESTS"; fi
    expect "$log" "AURORA-IRQ-SELFTEST: $EXPECTED_IRQ_TESTS passed, 0 failed" "$name: all interrupt checks pass (real timer IRQs after the unusual entry)"
    expect "$log" "AURORA-MM-SELFTEST: $EXPECTED_MM_TESTS passed, 0 failed" "$name: memory self-tests pass"
    expect_not "$log" "KERNEL PANIC|FATAL|\[FAULT\]" "$name: no panic or fault"
}
entry_variant entry-el1-spsel0 spsel0 "virt,highmem=off"                   "EL1, SPSel=0"
entry_variant entry-el2-el1t   el1t   "virt,highmem=off,virtualization=on" "EL2, SPSel=0"

NM=aarch64-linux-gnu-nm
sym() { $NM "$1" | awk -v s="$2" '$3 == s { print $1 }'; }

echo "== fault 6: genuine kernel stack overflow =="
AURORA_FAULT_TEST=6 BUILD_DIR="$OUT/f6" ./build.sh >/dev/null 2>&1 || bad "fault-6: build failed"
run_qemu "$OUT/f6/kernel.bin" "virt,highmem=off" 256M "$OUT/f6.log"
finished "fault-6" "$OUT/f6.log"
expect "$OUT/f6.log" "Interrupts and timer OK" "fault-6: all earlier checks passed before the overflow"
expect "$OUT/f6.log" "\[fault-test\] case 6:" "fault-6: overflow test started"
expect "$OUT/f6.log" "\[FAULT\] exception sync/EL1t" "fault-6: reported as a synchronous exception from kernel code"
expect "$OUT/f6.log" "\[FAULT\] EC=0x0*25 " "fault-6: data abort"
expect "$OUT/f6.log" "\[FAULT\] FSC=0x0*07 " "fault-6: level-3 translation fault (unmapped guard page)"
guard="$(sym "$OUT/f6/kernel.elf" __stack_guard)"; gp="$(echo "$guard" | cut -c1-13)"
expect "$OUT/f6.log" "\[FAULT\] FAR=0x$gp[0-9a-f]{3}" "fault-6: fault address lies inside the stack guard page (0x$guard)"
expect "$OUT/f6.log" "STACK OVERFLOW detected" "fault-6: the kernel recognised the overflow"
hsp="$(grep -o 'handler SP=0x[0-9a-f]*' "$OUT/f6.log" | head -1 | cut -d= -f2 | sed 's/^0x//')"
bot="$(sym "$OUT/f6/kernel.elf" __exc_stack_bottom)"; top="$(sym "$OUT/f6/kernel.elf" __exc_stack_top)"
if [ -n "$hsp" ] && [ $((0x$hsp)) -ge $((0x$bot)) ] && [ $((0x$hsp)) -lt $((0x$top)) ]; then ok "fault-6: diagnostics ran on the exception stack (SP 0x$hsp within 0x$bot..0x$top)"; else bad "fault-6: handler SP (${hsp:-none}) is not inside the exception stack"; fi
expect "$OUT/f6.log" "KERNEL PANIC" "fault-6: kernel panics cleanly with diagnostics"
expect_not "$OUT/f6.log" "did not fault|Boot complete" "fault-6: execution did not continue"

echo "== fault 7: fault inside the timer interrupt handler (nested exception) =="
AURORA_FAULT_TEST=7 BUILD_DIR="$OUT/f7" ./build.sh >/dev/null 2>&1 || bad "fault-7: build failed"
run_qemu "$OUT/f7/kernel.bin" "virt,highmem=off" 256M "$OUT/f7.log"
finished "fault-7" "$OUT/f7.log"
expect "$OUT/f7.log" "Interrupts and timer OK" "fault-7: all earlier checks passed"
expect "$OUT/f7.log" "\[fault-test\] case 7:" "fault-7: test started"
expect "$OUT/f7.log" "\[FAULT\] exception sync/EL1h" "fault-7: taken on SP_EL1, i.e. inside the handler"
expect "$OUT/f7.log" "NESTED exception" "fault-7: reported as NESTED"
expect "$OUT/f7.log" "\[FAULT\] EC=0x0*25 " "fault-7: data abort"
expect "$OUT/f7.log" "\[FAULT\] FAR=0x00000000dead0000" "fault-7: exact fault address"
expect "$OUT/f7.log" "KERNEL PANIC" "fault-7: kernel panics cleanly"
expect_not "$OUT/f7.log" "did not fault|Boot complete" "fault-7: execution did not continue"

echo
echo "irq-tests: $PASSES passed, $FAILS failed"
[ "$FAILS" -eq 0 ]
