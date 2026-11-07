#!/bin/bash
#
# tests/security/scanner-verification-test.sh
#
# Verifies Gitleaks scanner behavior using temporary fixtures in an
# isolated temporary Git repository. No credential-looking material is
# ever committed to AuroraOS history.
#
# Usage: bash scanner-verification-test.sh /path/to/gitleaks-binary
#
# Exit status: 0 if both positive and negative cases behave correctly;
#              non-zero otherwise.

set -euo pipefail

GITLEAKS_BIN="${1:-/tmp/gitleaks}"

if [ ! -x "$GITLEAKS_BIN" ]; then
    echo "ERROR: Gitleaks binary not found or not executable: $GITLEAKS_BIN" >&2
    exit 1
fi

echo "Using Gitleaks binary: $GITLEAKS_BIN"
"$GITLEAKS_BIN" version

# Define both temp repo variables up front so the EXIT trap never
# references an unbound variable (set -u).
POSITIVE_REPO=""
NEGATIVE_REPO=""
trap 'rm -rf "$POSITIVE_REPO" "$NEGATIVE_REPO"' EXIT

# ---------------------------------------------------------------
# CASE A: Positive test — synthetic credential matching an enabled
#         Gitleaks rule (AWS access key ID).
#         Must be detected AND produce a failing exit status.
#         Uses a separate temporary Git repository.
# ---------------------------------------------------------------

POSITIVE_REPO="$(mktemp -d)"

cd "$POSITIVE_REPO"
git init -q
git config user.email "scanner-test@example.com"
git config user.name "Scanner Verification Test"

POSITIVE_FILE="$POSITIVE_REPO/positive-fixture.txt"
cat > "$POSITIVE_FILE" <<'EOF'
# SCANNER VERIFICATION FIXTURE — POSITIVE CASE
#
# This file contains a synthetic AWS access key ID used solely to verify
# that Gitleaks can detect real-looking credentials. It is NOT a real
# credential and must never be used for anything. It exists only inside
# this temporary test repository and is never committed to AuroraOS.

AWS_ACCESS_KEY_ID=AKIAIOSFODNN7EXAMPLE
AWS_SECRET_ACCESS_KEY=wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY
EOF

git add positive-fixture.txt
git -c user.email="scanner-test@example.com" -c user.name="Scanner Verification Test" commit -qm "Add positive scanner verification fixture"

echo ""
echo "=== CASE A: Positive — synthetic credential should be DETECTED ==="
set +e
POSITIVE_OUTPUT="$("$GITLEAKS_BIN" detect --enable-rule=aws-access-token --source "$POSITIVE_REPO" -v 2>&1)"
POSITIVE_EXIT=$?
set -e
echo "$POSITIVE_OUTPUT"

if [ "$POSITIVE_EXIT" -eq 0 ]; then
    echo "FAIL: Positive case expected non-zero exit status (credential detected), got 0" >&2
    exit 1
fi

if ! echo "$POSITIVE_OUTPUT" | grep -qi "AKIAIOSFODNN7EXAMPLE"; then
    echo "FAIL: Positive case did not detect the synthetic AWS access key ID" >&2
    exit 1
fi

echo "PASS: Positive case detected credential with exit status $POSITIVE_EXIT"

# ---------------------------------------------------------------
# CASE B: Negative test — ordinary documentation containing words
#         such as "password", "secret", and "token" in prose context.
#         Must NOT trigger any detection and exit successfully.
#         Uses a separate temporary Git repository.
# ---------------------------------------------------------------

NEGATIVE_REPO="$(mktemp -d)"

cd "$NEGATIVE_REPO"
git init -q
git config user.email "scanner-test@example.com"
git config user.name "Scanner Verification Test"

NEGATIVE_FILE="$NEGATIVE_REPO/negative-fixture.txt"
cat > "$NEGATIVE_FILE" <<'EOF'
# SCANNER VERIFICATION FIXTURE — NEGATIVE CASE
#
# This file contains ordinary documentation terminology (the words
# "password", "secret", "token") in prose context. These are NOT actual
# credential values and must NOT trigger any Gitleaks detection.
#
# Used solely to verify the scanner does not produce false positives
# on ordinary documentation language. Exists only inside this temporary
# test repository and is never committed to AuroraOS.

# The Aurora Glass design system uses a "secret" token for theming that
# is documented in DESIGN.md. This is ordinary prose, not a credential.

# Aura AI stores a "password" hash in the Passport service,
# access-controlled by the user. Ordinary documentation language.

# The "token" bucket in the design system refers to visual design
# tokens (colors, spacing, typography), not an authentication token.
# See tokens.json.
EOF

git add negative-fixture.txt
git -c user.email="scanner-test@example.com" -c user.name="Scanner Verification Test" commit -qm "Add negative scanner verification fixture"

echo ""
echo "=== CASE B: Negative — ordinary words should NOT be DETECTED ==="
set +e
NEGATIVE_OUTPUT="$("$GITLEAKS_BIN" detect --source "$NEGATIVE_REPO" -v 2>&1)"
NEGATIVE_EXIT=$?
set -e
echo "$NEGATIVE_OUTPUT"

if [ "$NEGATIVE_EXIT" -ne 0 ]; then
    echo "FAIL: Negative case expected zero exit status (no credentials), got $NEGATIVE_EXIT" >&2
    exit 1
fi

echo "PASS: Negative case produced no detections with exit status $NEGATIVE_EXIT"

echo ""
echo "=== SCANNER VERIFICATION COMPLETE ==="
echo "Positive case: credential detected (exit $POSITIVE_EXIT) ✓"
echo "Negative case: no false positives (exit $NEGATIVE_EXIT) ✓"
exit 0
