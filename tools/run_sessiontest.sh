#!/bin/sh
#=============================================================================
# run_sessiontest.sh
#
# Server-lifecycle conformance suite. Four things a dedicated server must get
# right before anyone can play on it:
#
#   1. bind      A server that cannot take its port MUST die, not pretend.
#                Something else already holding the UDP port (a stale
#                container, a second launch) used to leave a process that
#                ticked forever with no socket, printing "Running." and
#                "Clients: 0" — healthy-looking and unreachable.
#   2. lurker    A connected peer that has not asked to join is not a player:
#                no spawn, no snapshots.
#   3. join      ...and once it does ask, it becomes one.
#   4. phantom   Idle peers do not count toward MatchConfig::MIN_PLAYERS, so
#                clients parked on the title screen cannot start — and burn —
#                a match nobody is playing.
#   5. match     The control for 4: two peers that DO join must still carry the
#                room WAITING -> COUNTDOWN -> PLAYING.
#
# Runs entirely over loopback. Uses a high port so a real server on 7777 (or a
# container publishing it) does not interfere.
#
# Usage: tools/run_sessiontest.sh [port]     (default 7901)
#=============================================================================
set -u

PORT="${1:-7901}"
MAP="${MAP:-shipment.map}"
SRV=./game_server
TEST=./session_test
LOGDIR="${TMPDIR:-/tmp}"
FAILED=0

for bin in "$SRV" "$TEST"; do
    if [ ! -x "$bin" ]; then
        echo "missing $bin - build with:"
        echo "  make"
        echo "  g++ -std=c++17 -O2 -INetwork -IThirdParty/enet/include tools/session_test.cpp \\"
        echo "      -o session_test -LThirdParty/enet/lib -lenet -lpthread"
        exit 2
    fi
done

echo "===== server session suite | port=$PORT | map=$MAP ====="

#-----------------------------------------------------------------------------
# 1. bind: the second server on a taken port must exit non-zero.
#-----------------------------------------------------------------------------
echo
echo "--- [1/5] bind: a server that cannot take its port must exit non-zero"
"$SRV" --port="$PORT" --map="$MAP" > "$LOGDIR/sess_s1.log" 2>&1 &
S1=$!
sleep 2

if ! kill -0 "$S1" 2>/dev/null; then
    echo "   FAIL the first server did not stay up - port $PORT already in use?"
    cat "$LOGDIR/sess_s1.log"
    exit 2
fi

# The second one is given 5s. A server that ignores its own bind failure never
# exits at all, so the timeout IS the failure.
timeout 5 "$SRV" --port="$PORT" --map="$MAP" > "$LOGDIR/sess_s2.log" 2>&1
RC=$?
if [ "$RC" -eq 0 ]; then
    echo "   FAIL second server exited 0 - a failed bind reported success"
    FAILED=1
elif [ "$RC" -eq 124 ]; then
    echo "   FAIL second server never exited - it is ticking with no socket"
    grep -E "ERROR|Running|Clients" "$LOGDIR/sess_s2.log" | head -4 | sed 's/^/        /'
    FAILED=1
else
    echo "   PASS second server exited $RC"
fi

#-----------------------------------------------------------------------------
# 2-5. connect/join split, against the healthy first server.
#-----------------------------------------------------------------------------
echo
echo "--- [2/5] lurker"
"$TEST" --scenario lurker --port "$PORT" --secs 8 || FAILED=1

echo
echo "--- [3/5] join"
"$TEST" --scenario join --port "$PORT" --secs 8 || FAILED=1

echo
echo "--- [4/5] phantom"
"$TEST" --scenario phantom --port "$PORT" --secs 16 || FAILED=1

echo
echo "--- [5/5] match"
"$TEST" --scenario match --port "$PORT" || FAILED=1

kill "$S1" 2>/dev/null
wait "$S1" 2>/dev/null

echo
if [ "$FAILED" -eq 0 ]; then
    echo "===== ALL PASS ====="
else
    echo "===== FAILURES ====="
fi
exit "$FAILED"
