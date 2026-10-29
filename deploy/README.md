# Deploy

The optional Docker appliance: one image with the web host and a Chrome the
agent can drive. From the repository root:

```sh
docker compose -f deploy/compose.yaml up --build -d
docker compose -f deploy/compose.yaml logs uagent
deploy/footprint.sh                # image size, memory and processes of the running stack
```

A release publishes the image as `ghcr.io/timongentzsch/uagent:<version>`
(and `latest`, for amd64 and arm64) and attaches this `compose.yaml` and
`seccomp-chrome.json`. The one-line installer runs those from
`~/.local/share/uagent/container/deploy`, with a `.env` naming the version and
the workspaces folder, and `docker compose up -d --no-build --pull always`;
API keys go in with `docker compose exec uagent uagent setup` and stay on the
data volume.

Setup, the Compose variables and the isolation model are in
[Browser](../docs/BROWSER.md); [Web](../docs/WEB.md) covers pairing and origins.

| Path | Contents |
| --- | --- |
| `Dockerfile` | Builds the web UI and the native binary, then a Debian image with Chrome and a nonroot `agent` user |
| `compose.yaml` | The service: read-only root, loopback-only port, data volume, workspaces mount, health check |
| `chrome-policy.json` | Managed Chrome policy; [Browser](../docs/BROWSER.md) says what it blocks |
| `seccomp-chrome.json` | Seccomp profile that lets Chrome create its own user namespace |
| `footprint.sh` | Snapshot of the running container, for comparing before and during browser use |
| `NOTICE.md`, `APACHE-2.0.txt` | Origin and license of the seccomp profile |
