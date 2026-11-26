# Install

This page installs, runs as a service, updates and removes µAgent on one
machine. The Compose appliance is in [Container](container.md); reaching the
web host from other devices is in [Remote access](remote-access.md).

## Run the installer

```sh
curl -fsSL https://raw.githubusercontent.com/timongentzsch/uAgent/master/install.sh | sh
```

Prebuilt archives are available from [GitHub Releases](https://github.com/timongentzsch/uAgent/releases/latest).

Piped to `sh` it installs a release; `./install.sh` in a checkout builds
that checkout. It asks on `/dev/tty`: *container* or *here* (where `docker
info` answers), Chrome and TigerVNC via `apt-get`, the Chrome policy, a
`PATH` line, `uagent setup` for keys, and the web service.

Reading a PDF's pages as text needs poppler (`apt install poppler-utils`,
`brew install poppler`); the container has it. Without it the agent gets
the PDF whole, as models that read files take it.

On Linux, pictures need ImageMagick (`apt install imagemagick`; version 6
or 7): to scale one over 2048 px or 4 MiB, to send HEIC or SVG, and to read
part of a picture. The container has it; macOS uses its own tools. Without
it a small picture goes out as it is, and the others reach the model as a
file reference that says why.

| Variable | Default | Effect |
| --- | --- | --- |
| `UAGENT_INSTALL` | asked; else `here` | `container` or `here` |
| `UAGENT_VERSION` | latest release | release to install |
| `UAGENT_PREFIX` | `~/.local` | install prefix |
| `UAGENT_NONINTERACTIVE` | unset | `1`: ask nothing; no keys, `PATH`, policy or service |
| `UAGENT_BUILD_DIR`, `UAGENT_BUILD_JOBS` | `build/release`, 4 | source build tree and jobs |
| `UAGENT_WEB_PORT` | 8080 | port written into the service |

## Platforms and the loader check

Archives exist for `linux-x86_64` and `linux-arm64` (glibc 2.38 or newer)
and `macos-arm64` (macOS 15 or newer). Elsewhere, or when the unpacked
binary fails `uagent --version` (a missing library), the installer clones
that release's tag into `~/.local/share/uagent/src` and builds it: git,
CMake 3.21+, a C++20 compiler, libcurl 7.66+, zlib and OpenSSL 3. Configuring
downloads two pinned header libraries from `github.com`; CONTRIBUTING says how
to build offline. To build the web client from source, use Node.js 24 and npm
as CI does.

## What is installed where

| Path | Content |
| --- | --- |
| `<prefix>/bin/uagent` | the executable, web client embedded |
| `<prefix>/share/uagent/` | `skills/`, `sheet/`, `remote/<build id>/` (the remote bundle), `install` (`release`, `source`, `checkout` or `container`) |
| `<prefix>/share/doc/uagent/` | README, SECURITY, docs, licences |
| `/etc/opt/chrome/policies/managed/uagent.json` | Chrome policy, Linux, when accepted |

State lives in `~/.uagent` ([On-disk layout](../interfaces/on-disk.md)).
New `share/` trees are copied in full before the old ones are replaced.

## Verify a release

The installer checks the package against its companion SHA-256 file. The
release also includes a signed `SHA256SUMS` manifest covering all archives
and a GitHub build attestation for each archive.

To verify the Linux x86-64 archive, run the following in an empty directory
with GitHub CLI and cosign installed. Change the tag or archive target to
verify a different release.

```sh
repo=timongentzsch/uAgent
tag=v1.13.0
archive="uagent-${tag#v}-linux-x86_64.tar.gz"
gh release download "$tag" --repo "$repo" -p "$archive" -p SHA256SUMS -p SHA256SUMS.sigstore.json
cosign verify-blob --bundle SHA256SUMS.sigstore.json \
  --certificate-identity "https://github.com/$repo/.github/workflows/release.yml@refs/tags/$tag" \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com SHA256SUMS
sha256sum --check --ignore-missing SHA256SUMS
gh attestation verify "$archive" --repo "$repo" --source-ref "refs/tags/$tag"
```

## Build and install from source

`./install.sh` in a checkout rebuilds `web/dist` when
`web/scripts/build-state.js check` reports changed inputs, builds `uagent` in
`build/release`, installs the remote bundle, then runs
`cmake --install <build> --prefix <prefix> --strip`. Plain `cmake --install`
installs no bundle, and SSH hosts then fail with `A complete three-target
remote bundle for this exact build is required.` The installer takes the
bundle for `uagent --build-id` from `<prefix>/share/uagent/remote/`,
`UAGENT_REMOTE_BUNDLE` or the matching release asset, and checks it with
`uagent --check-remote-bundle <dir>` first. An unpublished checkout has
none: `UAGENT_LOCAL_ONLY=1` installs without one.

## Run the web host as a service

| System | File | Content |
| --- | --- | --- |
| Linux | `~/.config/systemd/user/uagent-web.service` | `ExecStart=<prefix>/bin/uagent --web`, the installing shell's `PATH`, `UAGENT_WEB_PORT`, `Restart=on-failure`; optional `loginctl enable-linger` |
| macOS | `~/Library/LaunchAgents/com.github.timongentzsch.uagent.web.plist` | the same, `RunAtLoad`, `KeepAlive` on failure, log `~/Library/Logs/uagent-web.log` |

The installer then runs `uagent --web`, which finds the running host and
prints a pairing link.
Runtimes run in their own systemd scope and survive a unit restart
([Overview](../architecture/overview.md#processes-and-threads)).

## Update

Rerunning the installer restarts an existing unit, unless
`UAGENT_NONINTERACTIVE=1`. Settings → About shows the version and install
kind. **Check for updates** asks `api.github.com` and caches the answer in
`~/.uagent/config/update.json`; `web.updateCheck` asks daily
([Settings](../interfaces/settings.md)). **Update** appears for `release` and
`source` installs with `web.terminal` on and a writable prefix. It runs that
release's `install.sh` with `UAGENT_NONINTERACTIVE=1`, `UAGENT_INSTALL=here`,
`UAGENT_VERSION` and `UAGENT_PREFIX` (30 min limit), then the web host
re-executes itself; runtimes on the old binary restart once idle. A checkout
shows `git pull && ./install.sh`; a container shows the installer command.

## Remove

```sh
# Linux
systemctl --user disable --now uagent-web && rm ~/.config/systemd/user/uagent-web.service
# macOS
launchctl bootout gui/$(id -u)/com.github.timongentzsch.uagent.web && rm ~/Library/LaunchAgents/com.github.timongentzsch.uagent.web.plist
rm -rf ~/.local/bin/uagent ~/.local/share/uagent ~/.local/share/doc/uagent
rm -rf ~/.uagent    # every conversation, memory, key and trace
```

The rc-file `PATH` line and the Chrome policy stay until deleted.

## When it goes wrong

| Message | Cause and action |
| --- | --- |
| `checksum mismatch for <archive>; nothing was installed` | damaged download; retry, check `SHA256SUMS` |
| `building from source needs Node.js (npm) for the web interface` | no npm and no built `web/dist` |
| `No complete matching remote bundle. Set UAGENT_REMOTE_BUNDLE …` | set it, or `UAGENT_LOCAL_ONLY=1` |
| `Installed …, but the web interface did not answer on port <port>.` | port taken or host failed; `systemctl --user status uagent-web` or the log |
| `this install cannot be updated from here` (About) | not an installer's install, terminal off, or prefix unwritable |
