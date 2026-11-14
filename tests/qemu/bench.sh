#!/bin/sh
# tests/qemu/bench.sh - native renderer CPU-time baseline under QEMU/TCG (NOT a CI gate: timings vary by host).
# Builds the AURORA_BENCH kernel, boots it once with ramfb, waits for "bench: done" and prints the guest's own measurements
# (median / p95 over 40 samples after 5 warm-ups, CNTPCT counter). Fails only if the run does not complete or a result is
# missing/implausible (zero samples, median > p95). Disposable files under build/bench/.
# Usage: ./tests/qemu/bench.sh        (BENCH_QEMU_ARGS may add extra QEMU arguments)
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT" || exit 1
OUT="$ROOT/build/bench"; rm -rf "$OUT"; mkdir -p "$OUT"
AURORA_BENCH=1 BUILD_DIR="$OUT/k" ./build.sh >/dev/null 2>&1 || { echo "bench: build failed"; exit 1; }
L="$OUT/bench.log"; : > "$L"
qemu-system-aarch64 -machine virt,highmem=off -cpu cortex-a53 -smp 1 -m 256M -kernel "$OUT/k/kernel.bin" \
    -display none -serial file:"$L" -monitor none -device ramfb $BENCH_QEMU_ARGS &
QPID=$!
trap 'kill $QPID 2>/dev/null; wait $QPID 2>/dev/null' EXIT
i=0
while [ "$i" -lt 600 ]; do
    grep -q "bench: done" "$L" 2>/dev/null && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.5; i=$((i + 1))
done
echo "qemu: $(qemu-system-aarch64 --version | head -1); cpu cortex-a53, 1 vCPU, 256 MiB, machine virt (TCG)"
grep "^bench:" "$L" | tr -d '\r'
grep -q "bench: done" "$L" || { echo "bench: did not finish"; exit 1; }
bad=0
for n in $(grep "^bench: .* median_us=" "$L" | tr -d '\r' | sed 's/^bench: \([^ ]*\) .*/\1/'); do
    m=$(grep "^bench: $n " "$L" | sed 's/.*median_us=\([0-9]*\).*/\1/'); p=$(grep "^bench: $n " "$L" | sed 's/.*p95_us=\([0-9]*\).*/\1/')
    [ "$m" -le "$p" ] || { echo "implausible: $n median > p95"; bad=1; }
done
[ "$(grep -c '^bench: .* median_us=' "$L")" -eq 12 ] || { echo "expected 12 cases"; bad=1; }
[ "$bad" -eq 0 ] && echo "bench: sanity OK" || exit 1
