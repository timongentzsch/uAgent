# Deploy

The optional Docker appliance: one image with the web host and a Chrome the
agent can drive. From the repository root:

```sh
docker compose -f deploy/compose.yaml up --build -d
docker compose -f deploy/compose.yaml logs uagent
deploy/footprint.sh                # image size, memory and processes of the running stack
```

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
