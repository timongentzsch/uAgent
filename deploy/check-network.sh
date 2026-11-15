#!/bin/sh
# Checks the running stack from the agent's side: a public address answers,
# private ones are refused, and the agent cannot change that.
# Usage: deploy/check-network.sh [docker compose arguments, e.g. -f FILE]
# CHECK_LISTENER=ADDRESS:PORT names a private address known to be listening,
# so its refusal is the rules' doing and not a closed port's.
set -eu
[ "$#" -gt 0 ] || set -- -f "$(dirname "$0")/compose.yaml"

docker compose "$@" exec -T -e "LISTENER=${CHECK_LISTENER:-}" uagent sh -eu -c '
fail() { echo "FAIL: $1" >&2; exit 1; }
get() { curl -sS -m 10 -o /dev/null "$1" 2>/dev/null; }
get https://example.com || fail "a public address is not reachable"
hex=$(awk "\$2 == \"00000000\" { print \$3; exit }" /proc/net/route)
gateway=$(printf "%d.%d.%d.%d" "0x$(echo "$hex" | cut -c7-8)" \
  "0x$(echo "$hex" | cut -c5-6)" "0x$(echo "$hex" | cut -c3-4)" \
  "0x$(echo "$hex" | cut -c1-2)")
for address in "$gateway:80" 10.0.0.1:80 172.17.0.1:80 192.168.1.1:80 \
    100.64.0.1:80 169.254.169.254:80 $LISTENER; do
  # Refused at once, not left to time out.
  ! curl -sS -m 5 -o /dev/null "http://$address/" 2>/dev/null || fail "$address answers"
done
grep -q "^CapEff:[[:space:]]*0*$" /proc/self/status || fail "the agent holds capabilities"
grep -q "^NoNewPrivs:[[:space:]]*1" /proc/self/status || fail "no-new-privileges is off"
! nft list ruleset >/dev/null 2>&1 || fail "the agent can read the rules as root would"
! nft flush ruleset >/dev/null 2>&1 || fail "the agent can change the rules"
[ "$(id -u)" = 10001 ] || fail "the agent is not UID 10001"
echo "network: public reachable, private refused, rules out of the agent'"'"'s reach"
'
