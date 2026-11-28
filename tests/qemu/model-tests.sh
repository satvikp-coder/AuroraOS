#!/bin/sh
# tests/qemu/model-tests.sh - SYS_MODEL (11): bounded, read-only EL0 access to the VERIFIED objects of the model volume (kernel/proc/modelsys.c).
# Disposable images under build/model-tests/ only. The kernel is built with AURORA_MODEL_TEST=1 (kernel/modeltest.c), which launches the
# installer-owned EL0 program tests/programs/modelclient.c from the system disk under identities the KERNEL chooses.
#   1. valid 3-object volume (built by tests/tools/mkmodel from synthetic files with known bytes), both bus orders: as PRINCIPAL_INFER the client
#      reads the count, every INFO, every object whole in 4096-byte chunks and gets the exact AE_* code for every hostile call; the SAME program
#      under an ordinary identity gets EPERM for every op; 5 rounds of crash / exit mid-object followed by a passing run; no page or process leaks;
#      the digests EL0 sees are the SHA-256 of the files on the host; the model image is byte-for-byte unchanged.
#   2. no model volume: PRINCIPAL_INFER gets ENOSYS for every op, an ordinary identity still EPERM.
#   3. a volume that failed its boot check (damaged object): ENOSYS, as if absent.
# Silent boots, timeouts, wrong counts or an unexpected panic are FAILURES.
#
# Usage: ./tests/qemu/model-tests.sh      Requires aarch64-linux-gnu-gcc, qemu-system-aarch64, a host C compiler and python3.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT" || exit 1

EXPECTED_VALID=9         # setup + identity + full + denied x2 + 3 lifecycle + no leaks (keep in sync with kernel/modeltest.c)
EXPECTED_NOVOL=5         # setup + identity + novol + denied + no leaks
EXPECTED_CLIENT_PASS=46  # "mc: PASS" lines of one FULL run (keep in sync with tests/programs/modelclient.c)
EXPECTED_PROC_TESTS=128

OUT="$ROOT/build/model-tests"
rm -rf "$OUT"; mkdir -p "$OUT"
FAILS=0; PASSES=0
ok()  { PASSES=$((PASSES + 1)); echo "  PASS: $1"; }
bad() { FAILS=$((FAILS + 1));   echo "  FAIL: $1"; }
expect()     { if grep -Eq -- "$2" "$1"; then ok "$3"; else bad "$3 (pattern: $2)"; fi; }
expect_not() { if grep -Eq -- "$2" "$1"; then bad "$3 (found: $2)"; else ok "$3"; fi; }
count_is()   { n="$(tr -d '\r' < "$1" | grep -Ec -- "$2")"; if [ "$n" -eq "$3" ]; then ok "$4 ($n)"; else bad "$4 (want $3, got $n)"; fi; }

TIMED_OUT=0
run_qemu() { # kernel log extra-args...
    k="$1"; l="$2"; shift 2
    : > "$l"; TIMED_OUT=1
    qemu-system-aarch64 -machine virt,highmem=off -cpu cortex-a53 -smp 1 -m 256M -kernel "$k" -nographic -serial file:"$l" -monitor none "$@" &
    pid=$!; i=0
    while [ "$i" -lt 480 ]; do
        if grep -qE "Boot complete|KERNEL PANIC|SELFTEST FAILED|Kernel halted" "$l" 2>/dev/null; then TIMED_OUT=0; break; fi
        kill -0 "$pid" 2>/dev/null || { TIMED_OUT=0; break; }
        sleep 0.5; i=$((i + 1))
    done
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
}
finished() {
    if [ "$TIMED_OUT" -eq 1 ]; then bad "$1: timed out"; else ok "$1: finished without timing out"; fi
    if [ -s "$2" ]; then ok "$1: serial output produced"; else bad "$1: silent boot failure"; fi
    expect_not "$2" "KERNEL PANIC|SELFTEST FAILED" "$1: no panic and no failed self-test"
}
sum() { md5sum "$1" | cut -d' ' -f1; }
sysonly() { echo "-global virtio-mmio.force-legacy=false -drive file=$1,if=none,format=raw,id=s0 -device virtio-blk-device,drive=s0"; }
two() { # sysfirst|modelfirst system-image model-image
    if [ "$1" = sysfirst ]; then
        echo "-global virtio-mmio.force-legacy=false -drive file=$2,if=none,format=raw,id=s0 -device virtio-blk-device,drive=s0 -drive file=$3,if=none,format=raw,id=m0,readonly=on -device virtio-blk-device,drive=m0"
    else
        echo "-global virtio-mmio.force-legacy=false -drive file=$3,if=none,format=raw,id=m0,readonly=on -device virtio-blk-device,drive=m0 -drive file=$2,if=none,format=raw,id=s0 -device virtio-blk-device,drive=s0"
    fi
}

echo "== building =="
AURORA_MODEL_TEST=1 BUILD_DIR="$OUT/kernel" ./build.sh >/dev/null 2>&1 || { bad "model-test kernel build failed"; echo "model-tests: $PASSES passed, $FAILS failed"; exit 1; }
KERNEL="$OUT/kernel/kernel.bin"
CC="${CROSS_COMPILE:-aarch64-linux-gnu-}gcc"
PF="-O1 -ffreestanding -fno-builtin -nostdlib -static -no-pie -fno-pic -fno-pie -mgeneral-regs-only -fno-stack-protector -Wall -Wextra -Werror -Wl,-T,tests/programs/user.ld -Wl,-z,noexecstack -Wl,-z,max-page-size=4096 -Wl,--build-id=none"
$CC $PF -o "$OUT/modelclient.elf" tests/programs/modelclient.c && ok "the EL0 model client compiles as a static AArch64 ET_EXEC" || { bad "modelclient failed to compile"; echo "model-tests: $PASSES passed, $FAILS failed"; exit 1; }
cc -std=c11 -Wall -Wextra -O1 -I tests/tools/shim -I kernel/include -o "$OUT/mkafs" tests/tools/mkafs.c kernel/fs/afs.c && ok "mkafs compiled" || { bad "mkafs failed to compile"; exit 1; }
cc -std=c11 -Wall -Wextra -O1 -I kernel/include -o "$OUT/mkmodel" tests/tools/mkmodel.c kernel/fs/modelstore.c kernel/crypto/sha256.c && ok "mkmodel compiled" || { bad "mkmodel failed to compile"; exit 1; }

echo "== provisioning =="
# byte(k, i) = (i*131 + k*17 + (i >> 9)) & 0xFF: the same formula is in modelclient.c, which checks every byte it reads
python3 -c '
import sys
for k, (name, n) in enumerate([("weights.bin", 10000), ("tok.json", 700), ("cfg.bin", 512)]):
    open(sys.argv[1] + "/" + name, "wb").write(bytes(((i * 131 + k * 17 + (i >> 9)) & 0xFF) for i in range(n)))
' "$OUT" && ok "synthetic objects written (10000, 700 and 512 bytes)" || bad "object generation failed"
objs="weights.bin=$OUT/weights.bin tokenizer.json=$OUT/tok.json config=$OUT/cfg.bin"
"$OUT/mkmodel" "$OUT/valid.img" $objs > "$OUT/mkmodel.log" 2>&1 && ok "valid 3-object model image built" || bad "mkmodel (valid) failed"
"$OUT/mkmodel" --corrupt-object 0 "$OUT/bad.img" $objs >> "$OUT/mkmodel.log" 2>&1 && ok "model image with a damaged object built" || bad "mkmodel (--corrupt-object) failed"
"$OUT/mkafs" "$OUT/sys.img" 2048 installer 0x7 modelclient.elf "$OUT/modelclient.elf" > "$OUT/mkafs.log" 2>&1 && ok "system disk provisioned with the installer-owned modelclient.elf" || { bad "mkafs failed"; cat "$OUT/mkafs.log"; }

echo "== 1. valid model volume =="
for order in sysfirst modelfirst; do
    cp "$OUT/sys.img" "$OUT/sys-$order.img"; mb="$(sum "$OUT/valid.img")"
    log="$OUT/valid-$order.log"; run_qemu "$KERNEL" "$log" $(two $order "$OUT/sys-$order.img" "$OUT/valid.img"); finished "valid-$order" "$log"
    expect "$log" "\[model\] volume valid: 3 objects" "valid-$order: the boot check accepted the volume"
    expect "$log" "AURORA-PROC-SELFTEST: $EXPECTED_PROC_TESTS passed, 0 failed" "valid-$order: userspace suite still passes (incl. syscall 11 -> EPERM for an identity-less process)"
    expect "$log" "AURORA-MODEL-SELFTEST: $EXPECTED_VALID passed, 0 failed" "valid-$order: all $EXPECTED_VALID model checks pass"
    expect_not "$log" "\[model-test\].*: FAIL" "valid-$order: no failing model check"
    for t in "identity: PRINCIPAL_INFER is refused by launch_register" "full: modelclient as PRINCIPAL_INFER" "denied: the SAME program under an ordinary application identity gets EPERM" \
             "denied: the FULL mode under an ordinary identity fails" "lifecycle: 5 processes that FAULT" "lifecycle: 5 processes that EXIT" "lifecycle: after each crash" "no leaks:"; do
        expect "$log" "\[model-test\] $t.*: PASS" "valid-$order: $t"
    done
    # only the model-test part of the log (the process self-tests before it have their own deliberate faults)
    sec="$OUT/valid-$order.section"; tr -d '\r' < "$log" | sed -n '/\[model-test\] setup/,$p' > "$sec"
    # 6 FULL runs as PRINCIPAL_INFER (1 + one per lifecycle round) each print EXPECTED_CLIENT_PASS lines; the FULL-as-ordinary run prints none
    count_is "$sec" "\[user p[0-9]+\] mc: PASS (count|info|read|hostile|identity|after)" $((EXPECTED_CLIENT_PASS * 6)) "valid-$order: every check of the 6 authorised FULL runs passed inside EL0"
    count_is "$sec" "\[user p[0-9]+\] mc: failures 0" 7 "valid-$order: 6 FULL runs + 1 DENIED run report 0 failures"
    count_is "$sec" "\[user p[0-9]+\] mc: PASS denied: every op" 1 "valid-$order: the DENIED run saw EPERM for every op and nothing written"
    count_is "$sec" "\[user p[0-9]+\] mc: FAIL" "$EXPECTED_CLIENT_PASS" "valid-$order: the only FAIL lines are the $EXPECTED_CLIENT_PASS checks of the FULL mode run under an ordinary identity (it was refused everything)"
    count_is "$sec" "\[user p[0-9]+\] mc: PASS mid-read" 10 "valid-$order: 10 mid-object runs read their first chunk before crashing / exiting"
    count_is "$sec" "FAULT at EL0" 5 "valid-$order: exactly the 5 crash runs faulted"
    for k in 0 1 2; do
        f="$OUT/weights.bin"; [ $k -eq 1 ] && f="$OUT/tok.json"; [ $k -eq 2 ] && f="$OUT/cfg.bin"
        want="$(sha256sum "$f" | cut -d' ' -f1)"
        got="$(tr -d '\r' < "$log" | grep -E "\] mc: digest $k " | head -1 | sed 's/.* mc: digest [0-9] //')"
        if [ "$got" = "$want" ]; then ok "valid-$order: object $k's digest seen by EL0 is the host SHA-256 of its file"; else bad "valid-$order: digest $k (want $want, got '$got')"; fi
    done
    if [ "$(sum "$OUT/valid.img")" = "$mb" ]; then ok "valid-$order: the model image is byte-for-byte unchanged"; else bad "valid-$order: the model image changed"; fi
done

echo "== 2. no model volume =="
cp "$OUT/sys.img" "$OUT/sys-none.img"
log="$OUT/none.log"; run_qemu "$KERNEL" "$log" $(sysonly "$OUT/sys-none.img"); finished none "$log"
expect_not "$log" "\[model\]" "none: no model volume, no [model] line"
expect "$log" "AURORA-MODEL-SELFTEST: $EXPECTED_NOVOL passed, 0 failed" "none: all $EXPECTED_NOVOL model checks pass"
expect "$log" "\[model-test\] novol: with no valid model volume, PRINCIPAL_INFER gets ENOSYS for every op.*: PASS" "none: PRINCIPAL_INFER gets ENOSYS for every op"
expect "$log" "\[model-test\] novol: an ordinary identity still gets EPERM.*: PASS" "none: an ordinary identity gets EPERM (identity checked first)"
count_is "$log" "\[user p[0-9]+\] mc: PASS novol: every op" 1 "none: the client itself saw ENOSYS for all 12 ops"

echo "== 3. a model volume that failed its boot check =="
cp "$OUT/sys.img" "$OUT/sys-bad.img"; bb="$(sum "$OUT/bad.img")"
log="$OUT/invalid.log"; run_qemu "$KERNEL" "$log" $(two sysfirst "$OUT/sys-bad.img" "$OUT/bad.img"); finished invalid "$log"
expect "$log" "\[model\] volume INVALID: object digest mismatch" "invalid: the boot check rejected the damaged object"
expect "$log" "AURORA-MODEL-SELFTEST: $EXPECTED_NOVOL passed, 0 failed" "invalid: all $EXPECTED_NOVOL model checks pass"
count_is "$log" "\[user p[0-9]+\] mc: PASS novol: every op" 1 "invalid: PRINCIPAL_INFER gets ENOSYS for every op: an invalid volume is never readable"
if [ "$(sum "$OUT/bad.img")" = "$bb" ]; then ok "invalid: the model image is byte-for-byte unchanged"; else bad "invalid: the model image changed"; fi

echo "== 4. a kernel without the test hook never launches the client =="
BUILD_DIR="$OUT/normal" ./build.sh >/dev/null 2>&1 || bad "normal build failed"
cp "$OUT/sys.img" "$OUT/sys-normal.img"
log="$OUT/normal.log"; run_qemu "$OUT/normal/kernel.bin" "$log" $(two sysfirst "$OUT/sys-normal.img" "$OUT/valid.img"); finished normal "$log"
expect "$log" "\[model\] volume valid: 3 objects" "normal: the volume is verified at boot"
expect_not "$log" "model-test|mc: " "normal: no model test and no client in a normal kernel"

echo "model-tests: $PASSES passed, $FAILS failed"
[ "$FAILS" -eq 0 ]
