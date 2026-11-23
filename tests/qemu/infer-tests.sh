#!/bin/sh
# tests/qemu/infer-tests.sh - the first native inference PRIMITIVE: an EL0 program (tests/programs/inferclient.c) reads int8 tensor objects from the
# verified (integrity-only, NOT authenticated) model volume through SYS_MODEL (11), streams them in reads of at most 4096 bytes and computes an int8
# matrix-vector product with int32 accumulation and integer requantization (tests/programs/matvec_core.h, the code tests/kernel-host/matvec_test.c
# exercises). The host computes the expected rows with an INDEPENDENT reference (tests/kernel-host/matvec_ref.h, via tests/tools/mktensor) and this
# script compares them line for line with what EL0 printed. This is NOT a model and NOT a forward pass; the output is untrusted data nothing acts on.
# Disposable images under build/infer-tests/ only. The kernel is built with AURORA_INFER_TEST=1 (kernel/infertest.c).
#   1. a volume of 11 tensor objects: 3 valid (37 x 300 random; 1024 x 4096 at the accumulator's worst case, saturating high; 4 x 4096 saturating
#      low) and 8 hostile (header claims more rows than the object holds, truncated object, wrong kind, object shorter than the header, columns over
#      the limit, one trailing byte, a zero row scale, zero rows). Valid objects must match the reference exactly; hostile ones must be rejected
#      with the right reason, cleanly (exit 3, no fault, no result printed). The same program under an ordinary identity is refused (EPERM).
#   2. no model volume: PRINCIPAL_INFER gets ENOSYS and the program exits cleanly; an ordinary identity gets EPERM.
# Silent boots, timeouts, wrong counts or an unexpected panic are FAILURES.
#
# Usage: ./tests/qemu/infer-tests.sh      Requires aarch64-linux-gnu-gcc, qemu-system-aarch64, a host C compiler.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT" || exit 1

NOBJ=11
EXPECTED_VALID=$((1 + NOBJ + 1 + 1))   # setup + one per object + denied + no leaks (keep in sync with kernel/infertest.c)
EXPECTED_NOVOL=4         # setup + novol infer + novol ordinary + no leaks
EXPECTED_PROC_TESTS=128

OUT="$ROOT/build/infer-tests"
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
    while [ "$i" -lt 600 ]; do
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
two() { echo "-global virtio-mmio.force-legacy=false -drive file=$1,if=none,format=raw,id=s0 -device virtio-blk-device,drive=s0 -drive file=$2,if=none,format=raw,id=m0,readonly=on -device virtio-blk-device,drive=m0"; }
# the EL0 lines of the run with mode $2 (1-based), "[user pN] " prefix removed
section() { tr -d '\r' < "$1" | sed -n 's/^\[user p[0-9]*\] //p' | awk -v m="ic: mode $2" '/^ic: mode /{on = ($0 == m)} on'; }

echo "== building =="
AURORA_INFER_TEST=1 BUILD_DIR="$OUT/kernel" ./build.sh >/dev/null 2>&1 || { bad "infer-test kernel build failed"; echo "infer-tests: $PASSES passed, $FAILS failed"; exit 1; }
KERNEL="$OUT/kernel/kernel.bin"
CC="${CROSS_COMPILE:-aarch64-linux-gnu-}gcc"
PF="-O1 -ffreestanding -fno-builtin -nostdlib -static -no-pie -fno-pic -fno-pie -mgeneral-regs-only -fno-stack-protector -Wall -Wextra -Werror -Wl,-T,tests/programs/user.ld -Wl,-z,noexecstack -Wl,-z,max-page-size=4096 -Wl,--build-id=none"
$CC $PF -o "$OUT/inferclient.elf" tests/programs/inferclient.c && ok "the EL0 matvec program compiles as a static AArch64 ET_EXEC with -mgeneral-regs-only (no floating point)" || { bad "inferclient failed to compile"; echo "infer-tests: $PASSES passed, $FAILS failed"; exit 1; }
cc -std=c11 -Wall -Wextra -O1 -I tests/tools/shim -I kernel/include -o "$OUT/mkafs" tests/tools/mkafs.c kernel/fs/afs.c && ok "mkafs compiled" || { bad "mkafs failed to compile"; exit 1; }
cc -std=c11 -Wall -Wextra -O1 -I kernel/include -o "$OUT/mkmodel" tests/tools/mkmodel.c kernel/fs/modelstore.c kernel/crypto/sha256.c && ok "mkmodel compiled" || { bad "mkmodel failed to compile"; exit 1; }
cc -std=c11 -Wall -Wextra -O1 -o "$OUT/mktensor" tests/tools/mktensor.c && ok "mktensor compiled" || { bad "mktensor failed to compile"; exit 1; }

echo "== provisioning =="
T="$OUT/mktensor"; L="$OUT/mktensor.log"; : > "$L"
gen() { out="$1"; shift; "$T" "$@" > "$OUT/$out.ref" 2>> "$L" || bad "mktensor $out failed"; }
gen tensor   "$OUT/tensor.bin" 37 300 24 0x1234
gen maxdims  --fill-x -128 --fill-w -128 --mult 2147483647 "$OUT/maxdims.bin" 1024 4096 31 1
gen neg      --fill-x 127 --fill-w -128 --mult 1 "$OUT/neg.bin" 4 4096 0 2
gen lie      --claim-rows 38 "$OUT/lie.bin" 37 300 24 0x1234
gen trunc    --cut 100 "$OUT/trunc.bin" 37 300 24 0x1234
gen huge     "$OUT/huge.bin" 1 4097 0 3
gen extra    --extra 1 "$OUT/extra.bin" 37 300 24 0x1234
gen badscale --zero-mult 5 "$OUT/badscale.bin" 37 300 24 0x1234
gen zerodims --claim-rows 0 "$OUT/zerodims.bin" 1 8 0 4
head -c 20 "$OUT/tensor.bin" > "$OUT/short.bin"
ok "tensor objects and their reference results written"
[ "$(wc -l < "$OUT/tensor.ref")" -eq 37 ] && [ "$(wc -l < "$OUT/maxdims.ref")" -eq 1024 ] && ok "reference: 37 + 1024 rows" || bad "reference row counts"
grep -qv '^ic: y [0-9]* 67108864 127$' "$OUT/maxdims.ref" && bad "reference: maxdims is not 67108864 -> 127 in every row" || ok "reference: every maxdims row is 4096 * (-128)^2 = 67108864 and saturates to 127"
grep -qv '^ic: y [0-9]* -66584576 -128$' "$OUT/neg.ref" && bad "reference: neg is not -66584576 -> -128 in every row" || ok "reference: every neg row is 4096 * 127 * -128 = -66584576 and saturates to -128"
# object index:      0           1             2         3            4               5              6           7            8           9            10
objs="tensor:1=$OUT/tensor.bin maxdims:1=$OUT/maxdims.bin lie:1=$OUT/lie.bin trunc:1=$OUT/trunc.bin wrongkind:0=$OUT/tensor.bin short:1=$OUT/short.bin \
huge:1=$OUT/huge.bin extra:1=$OUT/extra.bin badscale:1=$OUT/badscale.bin zerodims:1=$OUT/zerodims.bin neg:1=$OUT/neg.bin"
"$OUT/mkmodel" "$OUT/valid.img" $objs > "$OUT/mkmodel.log" 2>&1 && ok "11-object model volume built (kinds set with name:kind)" || { bad "mkmodel failed"; cat "$OUT/mkmodel.log"; }
# a ':0' suffix must produce exactly the bytes of the plain spec (mkmodel stays byte-compatible)
"$OUT/mkmodel" "$OUT/k0a.img" a=$OUT/tensor.bin b=$OUT/short.bin >/dev/null 2>&1 && "$OUT/mkmodel" "$OUT/k0b.img" a:0=$OUT/tensor.bin b:0x0=$OUT/short.bin >/dev/null 2>&1 \
  && cmp -s "$OUT/k0a.img" "$OUT/k0b.img" && ok "mkmodel: 'name:0=file' is byte-identical to 'name=file'" || bad "mkmodel: kind 0 suffix changed the image"
"$OUT/mkmodel" "$OUT/bad.img" "a:x=$OUT/tensor.bin" >/dev/null 2>&1 && bad "mkmodel accepted a non-numeric kind" || ok "mkmodel refuses a non-numeric kind"
"$OUT/mkafs" "$OUT/sys.img" 2048 installer 0x7 inferclient.elf "$OUT/inferclient.elf" > "$OUT/mkafs.log" 2>&1 && ok "system disk provisioned with the installer-owned inferclient.elf" || { bad "mkafs failed"; cat "$OUT/mkafs.log"; }

echo "== 1. a model volume of tensor objects =="
cp "$OUT/sys.img" "$OUT/sys-valid.img"; mb="$(sum "$OUT/valid.img")"
log="$OUT/valid.log"; run_qemu "$KERNEL" "$log" $(two "$OUT/sys-valid.img" "$OUT/valid.img"); finished valid "$log"
expect "$log" "\[model\] volume valid: $NOBJ objects" "valid: the boot check accepted the volume"
expect "$log" "AURORA-PROC-SELFTEST: $EXPECTED_PROC_TESTS passed, 0 failed" "valid: userspace suite still passes"
expect "$log" "AURORA-INFER-SELFTEST: $EXPECTED_VALID passed, 0 failed" "valid: all $EXPECTED_VALID kernel-side infer checks pass"
expect_not "$log" "\[infer-test\].*: FAIL" "valid: no failing kernel-side check"
# only the infer-test part of the log (the process self-tests before it have their own deliberate faults)
isec="$OUT/valid.section"; tr -d '\r' < "$log" | sed -n '/\[infer-test\] setup/,$p' > "$isec"
expect_not "$isec" "FAULT at EL0" "valid: no EL0 fault on any object, valid or hostile"
k=0
for want in "0 -" "0 -" "3 size" "3 size" "3 kind" "3 short" "3 dims" "3 size" "3 scale" "3 dims" "0 -"; do
    code="${want% *}"; why="${want#* }"; m=$((k + 1))
    expect "$log" "\[infer-test\] object $k as PRINCIPAL_INFER: terminated with exit $code: PASS" "valid: object $k terminated with exit $code"
    sec="$OUT/valid-mode$m.txt"; section "$log" "$m" > "$sec"
    if [ "$code" = 3 ]; then
        count_is "$sec" "^ic: reject $why$" 1 "valid: object $k rejected as '$why'"
        count_is "$sec" "^ic: y " 0 "valid: object $k printed no result"
    fi
    k=$((k + 1))
done
for pair in "1 tensor" "2 maxdims" "11 neg"; do
    m="${pair% *}"; name="${pair#* }"
    section "$log" "$m" | grep '^ic: y ' > "$OUT/el0-$name.txt"
    if cmp -s "$OUT/el0-$name.txt" "$OUT/$name.ref"; then ok "valid: EL0 result for '$name' equals the host reference ($(wc -l < "$OUT/$name.ref") rows, every acc and q)"
    else bad "valid: EL0 result for '$name' differs from the reference"; diff "$OUT/el0-$name.txt" "$OUT/$name.ref" | head -5; fi
done
count_is "$OUT/valid-mode1.txt" "^ic: done 37 rows 300 cols$" 1 "valid: tensor (37 x 300, 3 reads of payload) completed"
count_is "$OUT/valid-mode2.txt" "^ic: done 1024 rows 4096 cols$" 1 "valid: maxdims (1024 x 4096, the dimension limits) completed"
count_is "$OUT/valid-mode1.txt" "^ic: object 0 tensor size 11580 kind 1$" 1 "valid: EL0 saw the tensor's verified size and kind"
expect "$log" "\[infer-test\] denied: object 0 under an ordinary application identity: exit 4: PASS" "denied: an ordinary identity exits 4"
count_is "$log" "\] ic: refused -7$" 1 "denied: the program saw EPERM (-7) and printed no result"
count_is "$log" "\] ic: y " $((37 + 1024 + 4)) "valid: exactly the rows of the three valid objects were printed"
expect "$log" "\[infer-test\] no leaks:.*: PASS" "valid: no page or process leaked"
if [ "$(sum "$OUT/valid.img")" = "$mb" ]; then ok "valid: the model image is byte-for-byte unchanged"; else bad "valid: the model image changed"; fi

echo "== 2. no model volume =="
cp "$OUT/sys.img" "$OUT/sys-none.img"
log="$OUT/none.log"; run_qemu "$KERNEL" "$log" $(sysonly "$OUT/sys-none.img"); finished none "$log"
expect_not "$log" "\[model\]" "none: no model volume, no [model] line"
expect "$log" "AURORA-INFER-SELFTEST: $EXPECTED_NOVOL passed, 0 failed" "none: all $EXPECTED_NOVOL kernel-side infer checks pass"
expect "$log" "\[infer-test\] novol: PRINCIPAL_INFER with no valid model volume: exit 4: PASS" "none: PRINCIPAL_INFER exits cleanly (4)"
expect "$log" "\[infer-test\] novol: an ordinary identity: exit 4: PASS" "none: an ordinary identity exits cleanly (4)"
count_is "$log" "\] ic: refused -4$" 1 "none: PRINCIPAL_INFER saw ENOSYS (-4)"
count_is "$log" "\] ic: refused -7$" 1 "none: the ordinary identity saw EPERM (-7): identity is checked first"
count_is "$log" "\] ic: y " 0 "none: no result printed"
isec="$OUT/none.section"; tr -d '\r' < "$log" | sed -n '/\[infer-test\] setup/,$p' > "$isec"
expect_not "$isec" "FAULT at EL0" "none: no EL0 fault"

echo "infer-tests: $PASSES passed, $FAILS failed"
[ "$FAILS" -eq 0 ]
