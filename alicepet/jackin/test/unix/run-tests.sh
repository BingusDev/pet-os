#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Runs the macOS/Linux Jack In client against a stand-in PET on 127.0.0.1 (never
# a real PET). Needs only bash and nc, which both systems ship with.
#
#   bash alicepet/jackin/test/unix/run-tests.sh
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
CLIENT="$HERE/../../unix/jackin-client.sh"
OPENER="$HERE/record-open.sh"
PORT=${JACKIN_TEST_PORT:-17341}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/jackin-test.XXXXXX")
FAILS=0
NC_PID=
CLIENT_PID=
case "$(uname -s)" in Darwin) EXPECT_OS=macos ;; *) EXPECT_OS=linux ;; esac

pass() { echo "PASS $*"; }
fail() { echo "FAIL $*"; FAILS=$((FAILS + 1)); }

# check DESCRIPTION FILE TEXT: TEXT must appear in FILE (fixed string).
check() {
    if grep -qF -- "$3" "$2" 2>/dev/null; then pass "$1"; else fail "$1 (missing: $3)"; fi
}
# absent DESCRIPTION FILE TEXT: TEXT must not appear in FILE.
absent() {
    if grep -qF -- "$3" "$2" 2>/dev/null; then fail "$1 (found: $3)"; else pass "$1"; fi
}

# Stop the stand-in PET: nc and the tail feeding it. Matched by command line,
# because the PID of a piped background job is not the nc process.
stop_pet_procs() {
    pkill -f "tail -f $WORK/inbox" 2>/dev/null
    pkill -f "nc -l 127.0.0.1 $PORT" 2>/dev/null
    NC_PID=
}

cleanup() {
    [ -n "$CLIENT_PID" ] && kill "$CLIENT_PID" 2>/dev/null
    stop_pet_procs
    rm -rf "$WORK"
}
trap cleanup EXIT

# start_pet OUTFILE: nc listens on 127.0.0.1:$PORT and writes what the client
# sends to OUTFILE. Lines appended to $WORK/inbox are sent to the client.
# tail -f keeps nc's stdin open; a fifo here made BSD nc exit early.
start_pet() {
    : >"$WORK/inbox"
    tail -f "$WORK/inbox" | nc -l 127.0.0.1 "$PORT" >"$1" 2>/dev/null &
    NC_PID=$!
    sleep 1
}

# say LINE: queue one PET message for the client.
say() { printf '%s\n' "$1" >>"$WORK/inbox"; }

run_client() {
    JACKIN_PET=127.0.0.1 JACKIN_PORT="$PORT" JACKIN_OPENER="$OPENER" \
        JACKIN_TEST_OPENED="$WORK/opened" JACKIN_TRUST_FILE="$WORK/trust" \
        bash "$CLIENT" </dev/null >"$WORK/client.out" 2>&1 &
    CLIENT_PID=$!
}

stop_pet() {
    stop_pet_procs
}

TOKEN=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef

# ---- scenario 1: a full session, ending with jack out -----------------------
echo "== scenario 1: session, PET messages, jack out"
start_pet "$WORK/pet1.out"
run_client
sleep 1
say '{"t":"pet","operator":"Holden"}'
say '{"t":"chips","list":[{"id":"jellyfin","name":"Jellyfin"},{"id":"retro","name":"Retro"}]}'
say '{"t":"alice","ok":true,"text":"Hi, I am Alice.","heard":"","cmd":"","step":0}'
say '{"t":"run","id":"r1","cmd":"Get-Process","agent":true}'
say '{"t":"ask","what":"full"}'
say '{"t":"net_open","id":"n1","host":"example.invalid","port":80}'
say '{"t":"play","kind":"video","title":"Test Movie","url":"http://127.0.0.1:9/x.mp4","item":"abc","start":0,"base":0,"length":100,"seekable":true}'
say '{"t":"scan"}'
say "{\"t\":\"trust\",\"token\":\"$TOKEN\"}"
say '{"t":"jackout","why":"test done"}'
wait "$CLIENT_PID"; CODE=$?
CLIENT_PID=
stop_pet

check "hello sent" "$WORK/pet1.out" '"t":"hello"'
check "hello names this OS" "$WORK/pet1.out" "\"os\":\"$EXPECT_OS\""
check "hello is Read mode" "$WORK/pet1.out" '"mode":"read"'
check "hello has no full access" "$WORK/pet1.out" '"full":false'
absent "never claims full access" "$WORK/pet1.out" '"full":true'
check "hello has no trust token the first time" "$WORK/pet1.out" '"trust":""'
if grep -qE '"time":1[0-9]{9}[,}]' "$WORK/pet1.out"; then pass "hello carries the clock"; else fail "hello carries the clock"; fi
check "trust token kept" "$WORK/trust" "$TOKEN"
if [ "$(stat -c %a "$WORK/trust" 2>/dev/null || stat -f %Lp "$WORK/trust")" = 600 ]; then pass "trust token is private"; else fail "trust token is private"; fi
check "scan sent" "$WORK/pet1.out" '"t":"scan"'
check "scan log lines sent" "$WORK/pet1.out" '"t":"log"'
check "run declined with read-only" "$WORK/pet1.out" '"reason":"read-only"'
check "net_open answered with net_fail" "$WORK/pet1.out" '"t":"net_fail"'
check "video handed off (transcode left running)" "$WORK/pet1.out" '"state":"handoff"'
check "bye sent on quit" "$WORK/pet1.out" '"t":"bye"'
check "client shows the operator" "$WORK/client.out" 'operator: Holden'
check "client shows chips" "$WORK/client.out" 'PET chips: Jellyfin,Retro'
check "client shows Alice" "$WORK/client.out" 'alice: Hi, I am Alice.'
check "client says the run was not run" "$WORK/client.out" 'not run'
check "client says the share was refused" "$WORK/client.out" "doesn't do that"
check "video handed to the opener" "$WORK/opened" 'http://127.0.0.1:9/x.mp4'
check "client reports jack out" "$WORK/client.out" 'PET says jack out: test done'
if [ "$CODE" -eq 0 ]; then pass "exit 0 on jack out"; else fail "exit code $CODE on jack out"; fi

# ---- scenario 2: the PET goes away mid-session ------------------------------
echo "== scenario 2: link drops"
start_pet "$WORK/pet2.out"
run_client
sleep 1
say '{"t":"pet","operator":"Holden"}'
say '{"t":"trust","token":""}'
sleep 1
stop_pet_procs
wait "$CLIENT_PID"; CODE=$?
CLIENT_PID=

check "the kept token is shown at the next link" "$WORK/pet2.out" "\"trust\":\"$TOKEN\""
if [ -e "$WORK/trust" ]; then fail "token forgotten when the PET says so"; else pass "token forgotten when the PET says so"; fi
check "client notices the link is gone" "$WORK/client.out" 'the link to the PET is gone'
if [ "$CODE" -eq 1 ]; then pass "exit 1 when the link drops"; else fail "exit code $CODE after link drop"; fi

# ---- scenario 3: no PET at the address ---------------------------------------
echo "== scenario 3: PET not reachable"
JACKIN_PET=127.0.0.1 JACKIN_PORT=1 bash "$CLIENT" </dev/null >"$WORK/client3.out" 2>&1
CODE=$?
check "clear message when no PET answers" "$WORK/client3.out" "can't reach the PET"
if [ "$CODE" -eq 1 ]; then pass "exit 1 when no PET answers"; else fail "exit code $CODE with no PET"; fi

echo
if [ "$FAILS" -eq 0 ]; then
    echo "ALL PASS"
else
    echo "$FAILS FAILED"
    exit 1
fi
