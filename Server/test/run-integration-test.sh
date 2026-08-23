#!/usr/bin/env bash
# End-to-end check of the dedicated server, run INSIDE the container image.
#
#   docker run --rm --platform linux/amd64 --entrypoint bash \
#     -v "$PWD/test:/test:ro" crossover-server /test/run-integration-test.sh
#
# Covers both transports: clients speaking UDP straight to the relay (how it runs
# on a VPS) and clients speaking UDP to a local tunnel that reaches the relay over
# TCP (how it runs on Railway).

set -uo pipefail

PASS=0
FAIL=0
OUT=/tmp/crossover-test
mkdir -p "$OUT"

check() { # check <description> <condition-result>
  if [ "$2" = "0" ]; then echo "  PASS  $1"; PASS=$((PASS+1));
  else echo "  FAIL  $1"; FAIL=$((FAIL+1)); fi
}

has() { grep -qE "$2" "$1"; }

echo "=============================================================="
echo " Crossover dedicated server -- integration test"
echo "=============================================================="

export CROSSOVER_GAME_PORT=60000
export CROSSOVER_TUNNEL_PORT=60001
export CROSSOVER_STATUS_PORT=8080
export CROSSOVER_MAX_PLAYERS=8
export CROSSOVER_SPAWN_X=1500.5 CROSSOVER_SPAWN_Y=2500.25 CROSSOVER_SPAWN_Z=42.0
export CROSSOVER_STATE_FILE=/tmp/crossover-spawn.txt

rm -f "$CROSSOVER_STATE_FILE"
crossover-server > "$OUT/server.log" 2>&1 &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null; jobs -p | xargs -r kill 2>/dev/null' EXIT

for _ in $(seq 1 50); do
  curl -fsS http://127.0.0.1:8080/health >/dev/null 2>&1 && break
  sleep 0.2
done

echo
echo "-- startup ---------------------------------------------------"
check "server process is alive"            "$(kill -0 $SERVER_PID 2>/dev/null; echo $?)"
check "protocol self-test passed"          "$(has "$OUT/server.log" 'protocol self-test passed'; echo $?)"
check "relay bound UDP 60000"              "$(has "$OUT/server.log" 'listening on UDP 60000'; echo $?)"
check "tunnel bound TCP 60001"             "$(has "$OUT/server.log" 'listening on TCP 60001'; echo $?)"
check "health endpoint answers"            "$(curl -fsS http://127.0.0.1:8080/health | grep -q '^ok$'; echo $?)"

echo
echo "-- scenario 1: three players, direct UDP ---------------------"
crossover-testclient 127.0.0.1 60000 alice 12 > "$OUT/alice.log" 2>&1 &
A=$!
sleep 2
crossover-testclient 127.0.0.1 60000 bob   9  > "$OUT/bob.log"   2>&1 &
B=$!
sleep 2
crossover-testclient 127.0.0.1 60000 carol 4  > "$OUT/carol.log" 2>&1 &
C=$!

sleep 3
STATUS_JSON=$(curl -fsS http://127.0.0.1:8080/ 2>/dev/null)
echo "  status: $STATUS_JSON" | cut -c1-200

check "status reports 3 players"           "$(echo "$STATUS_JSON" | grep -q '"players":3'; echo $?)"
check "status reports tcp-tunnel transport" "$(echo "$STATUS_JSON" | grep -q '\"transport\":\"tcp-tunnel\"'; echo $?)"

wait $A $B $C
echo
check "alice got network id 1"             "$(has "$OUT/alice.log" 'assigned_id id=1'; echo $?)"
check "bob got network id 2"               "$(has "$OUT/bob.log"   'assigned_id id=2'; echo $?)"
check "carol got network id 3"             "$(has "$OUT/carol.log" 'assigned_id id=3'; echo $?)"
check "alice spawned at configured point"  "$(has "$OUT/alice.log" 'spawn=\(1500.50,2500.25,42.00\)'; echo $?)"
check "bob spawned on alice (anchor mode)" "$(has "$OUT/bob.log"   'assigned_id id=2 spawn=\(1[45][0-9][0-9]\.'; echo $?)"
check "alice saw bob join"                 "$(has "$OUT/alice.log" 'peer_joined id=2'; echo $?)"
check "alice saw carol join"               "$(has "$OUT/alice.log" 'peer_joined id=3'; echo $?)"
check "bob received a roster"              "$(has "$OUT/bob.log"   'roster count=1'; echo $?)"
check "bob's roster listed alice"          "$(has "$OUT/bob.log"   'roster_entry id=1'; echo $?)"
check "carol's roster listed both"         "$(has "$OUT/carol.log" 'roster count=2'; echo $?)"
check "alice received peer movement"       "$(has "$OUT/alice.log" 'peer_moved id=[23]'; echo $?)"
check "bob received peer movement"         "$(has "$OUT/bob.log"   'peer_moved id=[13]'; echo $?)"
check "alice saw carol leave"              "$(has "$OUT/alice.log" 'peer_left id=3'; echo $?)"
check "no client hit protocol mismatch"    "$(! grep -q 'incompatible_protocol' "$OUT"/*.log; echo $?)"
check "server logged the disconnects"      "$(has "$OUT/server.log" 'player 3 disconnected'; echo $?)"
check "ids are recycled after a leave"     "$(has "$OUT/server.log" 'assigned network id 3'; echo $?)"

echo
echo "-- scenario 2: two players through the TCP tunnel -------------"
crossover-tunnel 127.0.0.1 60001 60010 > "$OUT/tunnel-d.log" 2>&1 &
TD=$!
crossover-tunnel 127.0.0.1 60001 60011 > "$OUT/tunnel-e.log" 2>&1 &
TE=$!
sleep 1

crossover-testclient 127.0.0.1 60010 dave 9 > "$OUT/dave.log" 2>&1 &
D=$!
sleep 2
crossover-testclient 127.0.0.1 60011 erin 6 > "$OUT/erin.log" 2>&1 &
E=$!
sleep 3
TUNNEL_JSON=$(curl -fsS http://127.0.0.1:8080/ 2>/dev/null)
check "status reports 2 tunnel connections" "$(echo "$TUNNEL_JSON" | grep -q '"tunnelConnections":2'; echo $?)"
wait $D $E
kill $TD $TE 2>/dev/null

check "dave connected through the tunnel"  "$(has "$OUT/dave.log" 'assigned_id'; echo $?)"
check "erin connected through the tunnel"  "$(has "$OUT/erin.log" 'assigned_id'; echo $?)"
check "dave saw erin join"                 "$(has "$OUT/dave.log" 'peer_joined'; echo $?)"
check "erin's roster listed dave"          "$(has "$OUT/erin.log" 'roster_entry'; echo $?)"
check "dave received movement over tunnel" "$(has "$OUT/dave.log" 'peer_moved'; echo $?)"
check "erin received movement over tunnel" "$(has "$OUT/erin.log" 'peer_moved'; echo $?)"
check "tunnel gave each player its own port" \
      "$(test "$(grep -c 'tunnel open from' "$OUT/server.log")" -ge 2; echo $?)"

echo
echo "-- shutdown --------------------------------------------------"
check "rendezvous point was persisted"     "$(test -s "$CROSSOVER_STATE_FILE"; echo $?)"
kill -TERM $SERVER_PID
sleep 1
check "server handled SIGTERM cleanly"     "$(has "$OUT/server.log" 'shutting down'; echo $?)"

echo
echo "=============================================================="
echo " $PASS passed, $FAIL failed"
echo "=============================================================="
[ "$FAIL" -eq 0 ]
