#!/bin/sh
# Checks the running stack's browser as a paired device meets it: the browser
# service starts the Chrome the container fetched. The device it pairs is
# logged out again.
# Usage: deploy/check-browser.sh [docker compose arguments, e.g. -f FILE]
set -eu
[ "$#" -gt 0 ] || set -- -f "$(dirname "$0")/compose.yaml"

docker compose "$@" exec -T uagent sh -eu -c '
fail() { echo "FAIL: $1" >&2; exit 1; }
origin=http://127.0.0.1:${UAGENT_WEB_PORT:-8080}
jar=$(mktemp)
trap "rm -f $jar" EXIT
send() {
  curl -sS -m 90 -b "$jar" -c "$jar" -H "Origin: $origin" \
    -H "Content-Type: application/json" -d "$2" "$origin$1"
}
command() {
  id=$(od -An -N16 -tx1 /dev/urandom | tr -d " \n")
  send /api/command "{\"v\":2,\"kind\":\"$1\",\"request_id\":\"$id\"$2}"
}
code=$(uagent --web | sed -n "s/^Pairing code[^:]*: //p")
send /api/auth "{\"code\":\"$code\",\"name\":\"check-browser\"}" >/dev/null
taken=$(command browser ",\"action\":\"takeover\"")
command browser ",\"action\":\"stop\"" >/dev/null
command logout "" >/dev/null
case $taken in
  *"\"accepted\":true"*) ;;
  *) fail "the browser did not start: $taken" ;;
esac
echo "browser: Chrome starts under the browser service"
'
