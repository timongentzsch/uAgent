#!/bin/sh
# The agent's container: Chrome kept current on the data volume, beside the
# web host. UAGENT_CHROME=off leaves Chrome alone.
[ "${UAGENT_CHROME:-auto}" = off ] || chrome-update --loop &
exec /usr/local/bin/uagent --web
