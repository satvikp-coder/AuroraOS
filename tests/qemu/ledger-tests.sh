#!/bin/sh
# tests/qemu/ledger-tests.sh - the DURABLE Passport on the REAL kernel and a real AuroraFS disk (Cycle 20): grants and revocations survive a guest restart,
# instance-bound grants do not, damaged / newer / rolled-back ledgers deny everything and are left byte-for-byte untouched, a read-only or missing disk
# disables protected actions honestly, and the table limits hold. Test kernels (AURORA_LEDGER_TEST=n) run a fixed script at boot and log "ledger: ..."
# lines; the disk image persists between boots exactly like a real disk. The crash-at-every-write and uncertain-write cases live in the host test
# (tests/kernel-host/ledger_test.c): QEMU cannot cut the power in the middle of a write. TCG only: no claim about real hardware.
# Usage: ./tests/qemu/ledger-tests.sh
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT" || exit 1
OUT="$ROOT/build/ledger-tests"; rm -rf "$OUT"; mkdir -p "$OUT"
FAILS=0; PASSES=0
ok()  { PASSES=$((PASSES + 1)); echo "  PASS: $1"; }
bad() { FAILS=$((FAILS + 1));   echo "  FAIL: $1"; }
QPID=""
stop_qemu() { if [ -n "$QPID" ]; then kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; QPID=""; fi; }
trap stop_qemu EXIT
trap 'stop_qemu; exit 130' INT TERM
LEG="-global virtio-mmio.force-legacy=false"
PASSPORT=2130706436        # 0x7F000004, the reserved Passport principal that owns passport.v1 and anchor.v1
TRAIL=2130706437           # 0x7F000005, the reserved Trail principal that owns trail.v1

cc -std=c11 -Wall -Wextra -O1 -I tests/tools/shim -I kernel/include -o "$OUT/mkafs" tests/tools/mkafs.c kernel/fs/afs.c && ok "mkafs compiled" || { bad "mkafs failed to compile"; exit 1; }
python3 "$SCRIPT_DIR/ledger_gen.py" "$OUT/gen" && ok "hand-built ledger files generated" || { bad "ledger_gen failed"; exit 1; }
for t in 0 1 2 3 4; do
    AURORA_LEDGER_TEST=$([ "$t" = 0 ] && echo "" || echo "$t") BUILD_DIR="$OUT/k$t" ./build.sh >/dev/null 2>&1 && ok "test kernel $t builds" || { bad "test kernel $t failed to build"; exit 1; }
done

# boot NAME KERNEL IMAGE [ro]  -> runs the guest until the shell is ready, leaves the log in $OUT/NAME.log
boot() {
    name="$1"; k="$2"; img="$3"; mode="$4"
    L="$OUT/$name.log"; : > "$L"
    if [ -z "$img" ]; then DRIVE=""; elif [ "$mode" = ro ]; then DRIVE="-drive file=$img,if=none,format=raw,id=d0,readonly=on -device virtio-blk-device,drive=d0"; else DRIVE="-drive file=$img,if=none,format=raw,id=d0 -device virtio-blk-device,drive=d0"; fi
    qemu-system-aarch64 -machine virt,highmem=off -cpu cortex-a53 -smp 1 -m 256M -kernel "$OUT/k$k/kernel.bin" \
        -display none -serial file:"$L" -monitor none -device ramfb $LEG -device virtio-keyboard-device -device virtio-tablet-device $DRIVE &
    QPID=$!
    n=0; while [ $n -lt 240 ]; do if grep -q "shell: ready keyboards=" "$L" 2>/dev/null; then break; fi; sleep 0.5; n=$((n + 1)); done
    sleep 1; stop_qemu
}
expect() {   # log pattern description
    if grep -qE -- "$2" "$OUT/$1.log"; then ok "$3"; else bad "$3 (pattern: $2)"; fi
}
refuse() {   # log pattern description
    if grep -qE -- "$2" "$OUT/$1.log"; then bad "$3 (found: $2)"; else ok "$3"; fi
}
nopanic() { if grep -qE "KERNEL PANIC|SELFTEST FAILED" "$OUT/$1.log"; then bad "$1: panic or failed self-test"; else ok "$1: no panic, no failed self-test"; fi; }
same_disk() { if [ "$(sha256sum < "$1")" = "$2" ]; then ok "$3"; else bad "$3"; fi; }

echo "== scenario: nodisk =="
boot nodisk 1 ""
nopanic nodisk
expect nodisk "ledger: state=no-disk commit=0 ok=0" "no disk: state no-disk, protected actions unavailable (ok=0)"
expect nodisk "ledger: op grant 1001 files-write rc=1 state=no-disk" "no disk: a durable grant is refused (rc 1 = not available)"
expect nodisk "ledger: op instance 1003 timer gen 5 rc=0" "no disk: a RAM-only instance grant still works (essential rights need no disk)"

echo "== scenario: fresh disk, grant, restart =="
"$OUT/mkafs" "$OUT/main.img" 2048 > /dev/null 2>&1 && ok "empty AuroraFS image formatted" || bad "mkafs failed"
boot fresh 1 "$OUT/main.img"
nopanic fresh
expect fresh "ledger: state=ok commit=0 ok=1 fs_err=0" "fresh disk: healthy, empty, commit 0"
expect fresh "ledger: op grant 1001 files-write rc=0 state=ok" "grant 1001 files-write: committed"
expect fresh "ledger: op grant 1001 timer rc=0 state=ok" "grant 1001 timer: committed"
expect fresh "ledger: op grant 1002 navigate rc=0 state=ok" "grant 1002 navigate: committed"
expect fresh "ledger: op instance 1003 timer gen 5 rc=0" "an instance-bound grant is accepted in RAM"
expect fresh "ledger: op reserved 0x7F000004 navigate rc=6" "a durable grant to the reserved Passport identity is refused"
expect fresh "ledger: op principal 0 navigate rc=6" "a durable grant to principal 0 is refused"
expect fresh "ledger: after state=ok commit=3" "three durable commits"
expect fresh "ledger: trail state=ok records=0 commit=0 ok=1" "fresh disk: the Trail is healthy and empty"
expect fresh "ledger: boot marker rc=0 trail=ok" "the boot marker (kind 14) was appended and committed"
boot restart1 0 "$OUT/main.img"
nopanic restart1
expect restart1 "ledger: state=ok commit=3 ok=1 fs_err=0" "RESTART: the ledger is healthy at commit 3"
expect restart1 "ledger: grant principal=1001 perm=3 durable" "RESTART: 1001 files-write survived"
expect restart1 "ledger: grant principal=1001 perm=7 durable" "RESTART: 1001 timer survived"
expect restart1 "ledger: grant principal=1002 perm=1 durable" "RESTART: 1002 navigate survived"
refuse restart1 "principal=1003" "RESTART: the instance-bound grant did NOT survive"
expect restart1 "ledger: grants=3" "RESTART: exactly three grants"
expect restart1 "ledger: trail state=ok records=4 commit=4 ok=1" "RESTART: the boot marker and the three audited grants survived (4 records)"
expect restart1 "ledger: trail rec seq=1 kind=14" "RESTART: record 1 is the first boot marker (kind 14)"
expect restart1 "ledger: trail rec seq=2 kind=12 principal=1001" "RESTART: the grant of 1001 was audited (kind 12)"
expect restart1 "ledger: trail rec seq=4 kind=12 principal=1002" "RESTART: the grant of 1002 was audited (kind 12)"

echo "== scenario: revoke, restart =="
boot revoke 2 "$OUT/main.img"
nopanic revoke
expect revoke "ledger: op revoke 1001 files-write rc=0 state=ok" "revoke 1001 files-write: committed"
expect revoke "ledger: op revoke 4242 timer \(never granted\) rc=2 state=ok" "revoking what was never granted is refused by the Passport and the ledger stays healthy"
expect revoke "ledger: after state=ok commit=4" "one more commit (the refused revoke wrote nothing)"
expect revoke "ledger: trail rec seq=7 kind=13 principal=1001" "the revocation was audited (kind 13); the refused one left no record"
refuse revoke "ledger: trail rec seq=8" "the refused revoke wrote no Trail record"
boot restart2 0 "$OUT/main.img"
expect restart2 "ledger: state=ok commit=4 ok=1" "RESTART: commit 4"
refuse restart2 "principal=1001 perm=3" "RESTART: the revoked grant stays revoked"
expect restart2 "ledger: grant principal=1001 perm=7 durable" "RESTART: the other 1001 grant remains"
expect restart2 "ledger: grants=2" "RESTART: exactly two grants"

echo "== scenario: read-only disk =="
SUM="$(sha256sum < "$OUT/main.img")"
boot rodisk 1 "$OUT/main.img" ro
nopanic rodisk
expect rodisk "ledger: state=read-only commit=4 ok=0" "read-only: verified and loaded, but protected actions unavailable (ok=0)"
expect rodisk "ledger: grant principal=1002 perm=1 durable" "read-only: the verified grants are visible"
expect rodisk "ledger: op grant 1001 files-write rc=4 state=read-only" "read-only: a grant is refused (rc 4 = read-only)"
same_disk "$OUT/main.img" "$SUM" "read-only: the disk image is byte-for-byte unchanged"

echo "== scenario: Trail records survive a restart =="
boot trail 4 "$OUT/main.img"
nopanic trail
expect trail "ledger: trail state=ok records=8 commit=8" "the Trail holds the earlier boot markers and audited changes (8 records)"
expect trail "ledger: op trail append grant record rc=0 state=ok" "a grant record (kind 12) was appended"
expect trail "ledger: op trail append revoke record rc=0 state=ok" "a revoke record (kind 13) was appended"
expect trail "ledger: op trail append invalid kind 99 rc=6" "an invalid record is refused (rc 6) and writes nothing"
boot trailre 0 "$OUT/main.img"
nopanic trailre
expect trailre "ledger: trail state=ok records=11 commit=11 ok=1" "RESTART: eleven records (8 + the test boot's marker + 2 appended)"
expect trailre "ledger: trail rec seq=10 kind=12 principal=1001" "RESTART: the appended grant record survived"
expect trailre "ledger: trail rec seq=11 kind=13 principal=1001" "RESTART: the appended revoke record survived"
expect trailre "ledger: trail rec seq=1 kind=14" "RESTART: the oldest record is still record 1"
expect trailre "ledger: state=ok commit=4 ok=1" "RESTART: the Passport is unaffected by the Trail traffic"

echo "== scenario: table full =="
"$OUT/mkafs" "$OUT/full.img" 2048 > /dev/null 2>&1
boot full 3 "$OUT/full.img"
nopanic full
expect full "ledger: op fill stopped at 32 rc=2" "32 principals fit, the 33rd is refused by the Passport"
expect full "ledger: op grant after full rc=2 state=ok" "a grant on a full table is refused and the ledger stays healthy"
expect full "ledger: after state=ok commit=32" "32 commits"
boot fullre 0 "$OUT/full.img"
expect fullre "ledger: grants=32" "RESTART: all 32 grants survived"

echo "== scenario: damaged, newer and inconsistent ledgers (built by the host) =="
mk() {   # imagename owner:file ...  (passport.v1 and/or anchor.v1 from the generated set)
    img="$OUT/$1.img"; log="$OUT/mkafs_$1.log"; shift
    args=""
    for pair in "$@"; do fname="${pair%%=*}"; ffile="${pair##*=}"; own=$PASSPORT; [ "$fname" = trail.v1 ] && own=$TRAIL; args="$args $own 3 $fname $OUT/gen/$ffile"; done
    # shellcheck disable=SC2086
    "$OUT/mkafs" "$img" 2048 $args > "$log" 2>&1
}
scenario() {   # name description state-pattern-after expect-honoured(0|1)  files...
    name="$1"; desc="$2"; want="$3"; honoured="$4"; shift 4
    mk "$name" "$@"
    SUM="$(sha256sum < "$OUT/$name.img")"
    k=1; [ "$honoured" = 1 ] && k=0        # damaged ledgers also try to mutate (and must fail); healthy ones are only loaded
    boot "$name" $k "$OUT/$name.img"
    nopanic "$name"
    expect "$name" "ledger: state=$want" "$desc: state $want"
    if [ "$honoured" = 0 ]; then
        refuse "$name" "ledger: grant principal=[0-9]+ perm=[0-9]+ durable" "$desc: NO durable grant is honoured (empty, denying table)"
        expect "$name" "ledger: op grant 1001 files-write rc=1" "$desc: a new grant is refused"
        same_disk "$OUT/$name.img" "$SUM" "$desc: the disk image is byte-for-byte unchanged (evidence preserved, nothing overwritten)"
    else
        expect "$name" "ledger: grant principal=1001 perm=3 durable" "$desc: the verified grants are loaded"
    fi
}
scenario corrupt "garbage passport.v1" "corrupt" 0 passport.v1=garbage.bin anchor.v1=a2.bin
scenario flipped "one flipped byte in passport.v1" "corrupt" 0 passport.v1=p_flipped.bin anchor.v1=a2.bin
scenario newer "a NEWER passport.v1" "newer-version" 0 passport.v1=p_newer.bin anchor.v1=a2.bin
scenario anewer "a NEWER anchor.v1" "newer-version" 0 passport.v1=p_good2.bin anchor.v1=a_newer.bin
scenario rollback "passport.v1 behind its anchor (an old file restored)" "rollback-detected" 0 passport.v1=p_good2.bin anchor.v1=a3.bin
scenario ahead2 "passport.v1 two commits ahead of the anchor" "rollback-detected" 0 passport.v1=p_good3.bin anchor.v1=a1.bin
scenario noanchor "anchor.v1 deleted at commit 2" "rollback-detected" 0 passport.v1=p_good2.bin
scenario nopassport "passport.v1 deleted" "rollback-detected" 0 anchor.v1=a2.bin
scenario consistent "a consistent hand-built pair" "ok commit=2" 1 passport.v1=p_good2.bin anchor.v1=a2.bin
scenario repair "passport.v1 one commit ahead (the crash window)" "ok commit=2" 1 passport.v1=p_good2.bin anchor.v1=a1.bin
boot repair2 0 "$OUT/repair.img"
expect repair2 "ledger: state=ok commit=2 ok=1" "the crash window was REPAIRED on the previous boot: this boot is a plain consistent load"

echo "== scenario: damaged, newer and inconsistent TRAIL files (built by the host) =="
tscenario() {   # name description trail-state passport-state records-or-empty files...
    name="$1"; desc="$2"; twant="$3"; pwant="$4"; rec="$5"; shift 5
    mk "$name" "$@"
    SUM="$(sha256sum < "$OUT/$name.img")"
    k=1; [ "$twant" = ok ] && k=0
    boot "$name" $k "$OUT/$name.img"
    nopanic "$name"
    expect "$name" "ledger: trail state=$twant" "$desc: Trail state $twant"
    expect "$name" "ledger: state=$pwant" "$desc: Passport state $pwant"
    if [ "$twant" = ok ]; then
        expect "$name" "ledger: trail state=ok records=$rec" "$desc: $rec records loaded"
    else
        expect "$name" "ledger: op grant 1001 files-write rc=1" "$desc: a Passport grant is refused too (nothing may be written while evidence is held)"
        same_disk "$OUT/$name.img" "$SUM" "$desc: the disk image is byte-for-byte unchanged (evidence preserved)"
    fi
}
tscenario tconsistent "a consistent Passport+Trail set" ok ok 3 passport.v1=p_good2.bin anchor.v1=at_ok.bin trail.v1=t_good3.bin
tscenario trepair "trail.v1 one record ahead of its anchor (the crash window)" ok ok 3 passport.v1=p_good2.bin anchor.v1=at_ahead1.bin trail.v1=t_good3.bin
tscenario tcorrupt "one flipped byte in trail.v1" corrupt held 0 passport.v1=p_good2.bin anchor.v1=at_ok.bin trail.v1=t_flipped.bin
tscenario tnewer "a NEWER trail.v1" newer-version held 0 passport.v1=p_good2.bin anchor.v1=at_ok.bin trail.v1=t_newer.bin
tscenario trollback "trail.v1 behind its anchor (an old file restored)" rollback-detected held 0 passport.v1=p_good2.bin anchor.v1=at_behind.bin trail.v1=t_good3.bin
tscenario tahead2 "trail.v1 two records ahead of its anchor" rollback-detected held 0 passport.v1=p_good2.bin anchor.v1=at_ahead2.bin trail.v1=t_good3.bin
tscenario tbadhead "same count but a different chain head" rollback-detected held 0 passport.v1=p_good2.bin anchor.v1=at_badhead.bin trail.v1=t_good3.bin
tscenario tdeleted "trail.v1 deleted while the anchor vouches for 3 records" rollback-detected held 0 passport.v1=p_good2.bin anchor.v1=at_ok.bin
boot trepair2 0 "$OUT/trepair.img"
expect trepair2 "ledger: trail state=ok records=4 commit=4" "the Trail crash window was REPAIRED on the previous boot (3 records + that boot's marker = 4)"

echo "ledger-tests: $PASSES passed, $FAILS failed"
[ "$FAILS" -eq 0 ]
