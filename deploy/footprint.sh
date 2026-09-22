#!/bin/sh
# Snapshot one appliance state. Run before first browser use, during takeover,
# and after Stop browser to compare the same container on the same host.
set -eu
cd "$(dirname "$0")/.."
container=$(docker compose -f deploy/compose.yaml ps -q uagent)
if [ -z "$container" ]; then
  echo 'Start the appliance with docker compose -f deploy/compose.yaml up -d first.' >&2
  exit 1
fi
image=$(docker inspect --format '{{.Image}}' "$container")
echo "Chrome: $(docker exec "$container" google-chrome-stable --version)"
echo "Image bytes: $(docker image inspect --format '{{.Size}}' "$image")"
echo "Compressed image bytes: $(docker image save "$image" | gzip -1 | wc -c | tr -d ' ')"
echo 'Largest installed packages (KiB):'
docker exec "$container" dpkg-query -W -f='${Installed-Size}\t${binary:Package}\n' | sort -nr | head -15
echo 'Current cgroup memory and CPU:'
docker stats --no-stream --format '{{.MemUsage}}  {{.CPUPerc}}' "$container"
echo 'Current processes:'
docker exec "$container" ps -eo pid,ppid,comm
echo 'Lazy browser viewer bundle bytes (raw / gzip):'
for asset in web/dist/assets/browser-*.js; do
  [ -f "$asset" ] || continue
  printf '%s  ' "$asset"
  wc -c < "$asset" | tr -d '\n '
  printf ' / '
  gzip -c "$asset" | wc -c | tr -d ' \n'
  echo
done
