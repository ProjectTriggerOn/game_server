#!/bin/sh
#=============================================================================
# run_floodtest.sh
#
# Before/after scenario for the single-client-flood mitigation. Runs entirely
# over container loopback so the UDP flood never touches the host network.
#
# Timeline (30s):
#   T+0s   server starts (pinned to core 0)
#   T+0s   VICTIM: a normal 60/s player joins and measures its snapshot interval
#   T+10s  FLOOD: attacker blasts InputCmd as fast as possible for 15s
#   T+25s  flood ends; victim runs 5s more, then everything is torn down
#
# The server's own status lines (Tick/MaxWork/Net) show the impact; the VICTIM
# line shows the other player's experience (snapshot interval should stay ~31ms
# if the server is healthy, and balloon under an unmitigated flood).
#
# Override the attacker mode from `docker run`: FLOOD_MODE=junk for a malformed
# flood instead of valid input.
#=============================================================================
set -e

NCORES=$(nproc)
FLOOD_MODE="${FLOOD_MODE:-flood}"
echo "===== flood test | cores=$NCORES | attacker mode=$FLOOD_MODE ====="

# Core pinning (server isolated so tick slowdown is attributable to packet work,
# not CPU theft). Falls back to no pinning when fewer than 3 cores are available.
if [ "$NCORES" -ge 3 ]; then
    SRV_PIN="taskset -c 0"
    VIC_PIN="taskset -c 1"
    LOAD_PIN="taskset -c 2-$((NCORES - 1))"
else
    SRV_PIN=""; VIC_PIN=""; LOAD_PIN=""
    echo "(warning: <3 cores, not pinning — server/clients will share cores)"
fi

$SRV_PIN ./game_server --map default.map > /tmp/server.log 2>&1 &
SRV=$!
sleep 2
echo "--- server up (pid $SRV)"

$VIC_PIN ./floodtest --label VICTIM --rate 60 --secs 30 &
VIC=$!

sleep 10
echo "--- [T+10s] >>> FLOOD START <<<"
$LOAD_PIN ./floodtest --label FLOOD --$FLOOD_MODE --secs 15
echo "--- [T+25s] >>> FLOOD END <<<"

wait "$VIC" 2>/dev/null || true
sleep 1
kill "$SRV" 2>/dev/null || true
sleep 1

echo ""
echo "===== SERVER STATUS LINES (Tick / MaxWork / Net / throttle per ~1s) ====="
grep -E "Tick:|Net:|throttled" /tmp/server.log || cat /tmp/server.log
echo "===== END ====="
