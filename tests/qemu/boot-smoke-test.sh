#!/bin/sh
# tests/qemu/boot-smoke-test.sh — Automated QEMU serial boot smoke test
#
# Builds the kernel, boots QEMU headlessly, captures serial output, and
# checks for a genuine boot marker. Exits nonzero on failure or timeout.
#
# Usage: ./tests/qemu/boot-smoke-test.sh [memory-size]
#
# Memory size examples: 256M, 512M, 1G

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
KERNEL="$PROJECT_ROOT/build/kernel.bin"
LOG="$PROJECT_ROOT/build/qemu-boot.log"
TIMEOUT_SECS=120
MEMORY="${1:-512M}"

cd "$PROJECT_ROOT"

echo "[smoke-test] Building kernel..."
./build.sh >/dev/null 2>&1

if [ ! -f "$KERNEL" ]; then
    echo "FAIL: kernel ELF not produced at $KERNEL" >&2
    exit 1
fi

echo "[smoke-test] Booting QEMU with ${MEMORY} (timeout: ${TIMEOUT_SECS}s)..."

# Launch QEMU in the background, capturing serial output to a log file.
# -nographic -serial stdio sends UART to stdout; we redirect to $LOG.
: > "$LOG"
qemu-system-aarch64 \
    -machine virt,highmem=off \
    -cpu cortex-a53 \
    -smp 1 \
    -m "$MEMORY" \
    -kernel "$KERNEL" \
    -nographic \
    -serial file:"$LOG" \
    -monitor none \
    -pidfile "$PROJECT_ROOT/build/qemu.pid" &

QEMU_PID=$!

# Wait for all boot self-tests to complete or stop on a reported failure.
ELAPSED=0
BOOT_OK=0
while [ "$ELAPSED" -lt "$TIMEOUT_SECS" ]; do
    if [ -f "$LOG" ] && grep -q "Boot complete" "$LOG" 2>/dev/null; then
        BOOT_OK=1
        break
    fi
    if grep -Eq "KERNEL PANIC|SELFTEST FAILED" "$LOG"; then break; fi
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        # QEMU exited early
        break
    fi
    sleep 1
    ELAPSED=$((ELAPSED + 1))
done

# Stop QEMU gracefully
if kill -0 "$QEMU_PID" 2>/dev/null; then
    kill "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
fi

echo "=== QEMU serial output ==="
cat "$LOG" 2>/dev/null || true
echo "========================="

if [ "$BOOT_OK" -eq 1 ]; then
    echo "PASS: AuroraOS booted successfully in QEMU ARM64 virt with ${MEMORY}."
    exit 0
else
    echo "FAIL: AuroraOS did not boot within ${TIMEOUT_SECS}s with ${MEMORY}." >&2
    exit 1
fi
