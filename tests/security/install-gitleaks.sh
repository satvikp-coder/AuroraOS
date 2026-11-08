#!/bin/sh
# tests/security/install-gitleaks.sh — download a PINNED Gitleaks release and verify its SHA-256 before using it.
#
# - The version and the expected SHA-256 are pinned here. The checksum was taken from the project's published
#   gitleaks_8.24.3_checksums.txt and independently recomputed from the downloaded archive.
# - Transient network failures (HTTP errors, resets) are retried with a delay; a checksum MISMATCH is never retried
#   or ignored: it fails the job (that is a changed or corrupted artifact, not a transient fault).
# - Secret scanning is not weakened: if the binary cannot be obtained and verified, the CI job fails.
#
# Usage: install-gitleaks.sh [DEST_DIR]      Prints the path of the verified binary on the last line.
set -eu

VERSION=8.24.3
SHA256=9991e0b2903da4c8f6122b5c3186448b927a5da4deef1fe45271c3793f4ee29c
DEST="${1:-/tmp/gitleaks-$VERSION}"
URL="https://github.com/gitleaks/gitleaks/releases/download/v${VERSION}/gitleaks_${VERSION}_linux_x64.tar.gz"

mkdir -p "$DEST"
ARCHIVE="$DEST/gitleaks.tar.gz"

attempt=1
while :; do
    rm -f "$ARCHIVE"
    if curl -sS --fail --location --connect-timeout 20 --max-time 120 "$URL" -o "$ARCHIVE" 2>"$DEST/curl.err"; then
        break
    fi
    echo "download attempt $attempt failed: $(cat "$DEST/curl.err")" >&2
    if [ "$attempt" -ge 5 ]; then echo "giving up after $attempt attempts" >&2; exit 1; fi
    sleep $((attempt * 5))
    attempt=$((attempt + 1))
done

ACTUAL="$(sha256sum < "$ARCHIVE" | cut -d" " -f1)"
if [ "$ACTUAL" != "$SHA256" ]; then
    echo "CHECKSUM MISMATCH for gitleaks v$VERSION: expected $SHA256, got $ACTUAL" >&2
    exit 1
fi
tar -xzf "$ARCHIVE" -C "$DEST" gitleaks
chmod +x "$DEST/gitleaks"
"$DEST/gitleaks" version >&2
echo "$DEST/gitleaks"
