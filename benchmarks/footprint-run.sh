#!/bin/bash
# Runs the footprint comparison inside footprint.Dockerfile's image. Every
# tool talks to one endpoint that serves Claude Haiku 5.5 on the OpenAI
# Responses route at $PROXY_URL/v1 and $PROXY_URL/openai/v1 and on Anthropic
# Messages at $PROXY_URL/anthropic; the key comes in as PROXY_KEY. The
# settings written here live only in the container.
set -euo pipefail
: "${PROXY_URL:?set PROXY_URL}" "${PROXY_KEY:?set PROXY_KEY}"
model=${MODEL:-claude-haiku-5-5}

mkdir -p ~/.uagent/config ~/.codex
cat >~/.uagent/config/settings.json <<JSON
{"format": 3, "all": {"model": "proxy/$model", "approval.mode": "yolo",
 "providers": {"proxy": {"base_url": "$PROXY_URL/v1", "api_key": "$PROXY_KEY",
 "wire_api": "responses"}}}, "projects": {}}
JSON
chmod 600 ~/.uagent/config/settings.json
# Codex's own web search tool is not served by the endpoint.
cat >~/.codex/config.toml <<TOML
model = "$model"
model_provider = "proxy"
web_search = "disabled"
[model_providers.proxy]
name = "proxy"
base_url = "$PROXY_URL/openai/v1"
env_key = "PROXY_KEY"
wire_api = "responses"
TOML
export ANTHROPIC_BASE_URL=$PROXY_URL/anthropic ANTHROPIC_API_KEY=$PROXY_KEY
export ANTHROPIC_MODEL=$model ANTHROPIC_DEFAULT_HAIKU_MODEL=$model

cd ~
exec python3 /opt/bench/compare_agents.py \
  --uagent /opt/uagent/bin/uagent --uagent-model "proxy/$model" \
  --claude ~/.local/bin/claude --codex ~/codex/bin/codex --codex-config \
  --omp ~/omp/node_modules/.bin/omp --omp-model "$model" \
  --chart /out/footprint.svg --output /out/footprint.json "$@"
