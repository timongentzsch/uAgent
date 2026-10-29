#!/bin/sh
# Installs µAgent the most capable way this machine allows: it says what it
# found, recommends, and asks once. Piped from the web it installs the latest
# release:
#
#   curl -fsSL https://raw.githubusercontent.com/timongentzsch/uAgent/master/install.sh | sh
#
# and run from a checkout it builds that checkout. In a container µAgent runs
# its web interface, browser and terminal isolated from this machine;
# installed here it has the terminal, the web interface, and the browser on
# Linux with Chrome and TigerVNC. Then it asks for your API keys and a default
# model (uagent setup), and offers to keep the web interface running.
#
# UAGENT_INSTALL=container|here chooses without asking; UAGENT_VERSION picks a
# release; UAGENT_PREFIX (~/.local) is where it installs here. Without a
# terminal, or with UAGENT_NONINTERACTIVE=1, nothing is asked: it installs
# here and leaves keys and services alone. Nothing runs with sudo unless you
# agree.
set -eu
repo=timongentzsch/uAgent
prefix=${UAGENT_PREFIX:-"$HOME/.local"}
data=${XDG_DATA_HOME:-"$HOME/.local/share"}/uagent
build_jobs=${UAGENT_BUILD_JOBS:-${CMAKE_BUILD_PARALLEL_LEVEL:-4}}
port=${UAGENT_WEB_PORT:-8080}

has() { command -v "$1" >/dev/null 2>&1; }
yes_no() { [ "$1" = yes ] && echo found || echo missing; }
# Questions go to the terminal itself, so they work while the script arrives
# on standard input.
interactive=no
if [ -z "${UAGENT_NONINTERACTIVE:-}" ] && (: </dev/tty) 2>/dev/null; then
  interactive=yes
fi
reply() {
  answer=
  [ "$interactive" = yes ] || return 1
  printf '%s ' "$1" >/dev/tty
  read -r answer </dev/tty || answer=
}
ask() { reply "$1 [y/N]" && { [ "$answer" = y ] || [ "$answer" = Y ]; }; }
ask_yes() { reply "$1 [Y/n]" && [ "$answer" != n ] && [ "$answer" != N ]; }

# A file beside CMakeLists.txt is a checkout, built as it is. Anything else
# (the script piped to sh) installs a release.
mode=release
case $0 in
  */*) script=$0 ;;
  *) script=./$0 ;;
esac
if [ -f "$script" ] && [ -f "$(dirname -- "$script")/CMakeLists.txt" ]; then
  mode=source
  root=$(CDPATH='' cd -- "$(dirname -- "$script")" && pwd)
  build=${UAGENT_BUILD_DIR:-"$root/build/release"}
fi

os=$(uname -s)
case $os-$(uname -m) in
  Linux-x86_64) target=linux-x86_64 ;;
  Linux-aarch64 | Linux-arm64) target=linux-arm64 ;;
  Darwin-arm64) target=macos-arm64 ;;
  *) target= ;;
esac
# What the release archives are built against (the CI runners): an older
# system installs a binary that cannot start, so it is not offered one.
min_glibc=2.38
min_macos=15
at_least() {
  # Whether version $1 is $2 or newer, comparing major.minor.
  [ "$(printf '%s\n%s\n' "$2" "$1" | sort -t. -k1,1n -k2,2n | head -n 1)" = "$2" ]
}
unsupported=
if [ -z "$target" ]; then
  unsupported="no prebuilt µAgent for $os $(uname -m)"
elif [ "$os" = Linux ]; then
  glibc=$(getconf GNU_LIBC_VERSION 2>/dev/null | sed -n 's/^glibc //p')
  if [ -z "$glibc" ]; then
    unsupported="the prebuilt µAgent needs glibc, and this system has another C library"
  elif ! at_least "$glibc" "$min_glibc"; then
    unsupported="the prebuilt µAgent needs glibc $min_glibc or newer; this system has $glibc"
  fi
else
  mac_version=$(sw_vers -productVersion 2>/dev/null || echo 0)
  if ! at_least "$mac_version" "$min_macos"; then
    unsupported="the prebuilt µAgent needs macOS $min_macos or newer; this Mac has $mac_version"
  fi
fi
# Without a binary that runs here, build the source instead, kept so the
# next update reuses its build.
build_from_source() {
  echo "Building µAgent from source: $1."
  echo "That needs git, CMake 3.21+, a C++20 compiler, libcurl and zlib (Node.js adds the web interface)."
  has git || { echo "git is missing" >&2; exit 1; }
  if [ -d "$data/src/.git" ]; then
    git -C "$data/src" pull --ff-only --quiet
  else
    mkdir -p "$data"
    git clone --depth 1 --quiet "https://github.com/$repo.git" "$data/src"
  fi
  # The container question is behind us: what follows installs here.
  UAGENT_INSTALL=here exec sh "$data/src/install.sh"
}

docker=no
if has docker && docker info >/dev/null 2>&1; then docker=yes; fi
browser=no
if [ "$os" = Linux ] && has google-chrome-stable && has Xtigervnc && has xauth; then
  browser=yes
fi
if [ "$mode" = source ]; then
  node=no
  if has npm; then node=yes; fi
  # The web interface, and with it the browser, needs Node.js or a built
  # web/dist.
  web=$node
  if [ -f "$root/web/dist/.assets" ]; then web=yes; fi
  found="Docker: $(yes_no "$docker"), Node.js: $(yes_no "$node")"
else
  # Release archives carry the web interface.
  web=yes
  found="Docker: $(yes_no "$docker")"
fi
if [ "$os" = Linux ]; then
  echo "$found, Chrome and TigerVNC: $(yes_no "$browser")"
else
  echo "$found; the agent's browser runs in the container or on Linux"
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM
fetch() { curl -fsSL --retry 2 -o "$2" "$1"; }
version=${UAGENT_VERSION:-}
version=${version#v}
release_version() {
  [ -n "$version" ] && return
  # The latest release, from where its page redirects.
  latest=$(curl -fsSLI -o /dev/null -w '%{url_effective}' "https://github.com/$repo/releases/latest")
  version=${latest##*/v}
  case $version in
    '' | */*) echo "cannot find the latest µAgent release" >&2; exit 1 ;;
  esac
}

# The container keeps an agent that runs commands and browses away from this
# machine's files and logins, so it comes first where Docker runs.
choice=${UAGENT_INSTALL:-here}
if [ -z "${UAGENT_INSTALL:-}" ] && [ "$docker" = yes ] && [ "$interactive" = yes ]; then
  here=
  [ "$mode" = release ] && [ -n "$unsupported" ] && here=", built from source ($unsupported)"
  echo "1) In a container (recommended): web interface, browser and terminal, isolated from this machine"
  echo "2) Here: the terminal$([ "$web" = yes ] && echo ", the web interface")$([ "$web$browser" = yesyes ] && echo " and the browser")$here"
  reply 'Install [1]:' || true
  [ "$answer" = 2 ] || choice=container
fi

# A release's container is its published image, run from the compose files
# of the same release; a checkout builds its own.
if [ "$choice" = container ]; then
  if [ "$mode" = source ]; then
    deploy=$root/deploy
    mkdir -p "$root/workspaces"
  else
    release_version
    deploy=$data/container/deploy
    base="https://github.com/$repo/releases/download/v$version"
    if ! fetch "$base/compose.yaml" "$tmp/compose.yaml" 2>/dev/null ||
       ! fetch "$base/seccomp-chrome.json" "$tmp/seccomp-chrome.json" 2>/dev/null; then
      echo "µAgent $version publishes no container image; installing here instead."
      choice=here
    else
      mkdir -p "$deploy"
      mv "$tmp/compose.yaml" "$tmp/seccomp-chrome.json" "$deploy/"
      workspaces=$HOME/uagent-workspaces
      if [ -f "$deploy/.env" ]; then
        workspaces=$(sed -n 's/^UAGENT_WORKSPACES=//p' "$deploy/.env")
      elif reply "Folder the agent works in, all it sees of this machine [$workspaces]:" &&
           [ -n "$answer" ]; then
        workspaces=$answer
      fi
      mkdir -p "$workspaces"
      printf 'UAGENT_VERSION=%s\nUAGENT_WORKSPACES=%s\n' "$version" "$workspaces" >"$deploy/.env"
    fi
  fi
fi
if [ "$choice" = container ]; then
  compose() { docker compose -f "$deploy/compose.yaml" "$@"; }
  if [ "$mode" = source ]; then
    compose up --build -d
  else
    compose up -d --no-build --pull always
  fi
  printf 'Waiting for the container'
  tries=0
  until compose exec -T uagent curl -fs -o /dev/null "http://127.0.0.1:$port/" 2>/dev/null; do
    tries=$((tries + 1))
    if [ "$tries" -ge 60 ]; then
      echo; echo "It did not answer within a minute; see: docker compose -f $deploy/compose.yaml logs uagent" >&2
      exit 1
    fi
    printf .
    sleep 1
  done
  echo
  # Keys and the model live on the container's data volume, so they outlast
  # updates.
  if [ "$interactive" = yes ]; then
    if compose exec -T uagent uagent --control '{"kind":"provider"}' 2>/dev/null |
       grep -q '"configured":true'; then
      ! ask "Change the API keys?" || compose exec uagent uagent setup </dev/tty || true
    else
      compose exec uagent uagent setup </dev/tty || true
    fi
  fi
  # Docker restarts the container with itself; that is only "always" when
  # Docker starts at boot.
  if [ "$os" = Linux ] && has systemctl &&
     [ "$(systemctl is-enabled docker 2>/dev/null)" != enabled ] &&
     ask "Docker does not start at boot. Enable it, with sudo, so µAgent is always available?"; then
    sudo systemctl enable docker
  elif [ "$os" = Darwin ]; then
    echo "To keep µAgent available, let Docker Desktop start when you log in (Settings, General)."
  fi
  echo "Open the link below to pair this browser; to pair another device later:"
  echo "  docker compose -f $deploy/compose.yaml exec uagent uagent --web"
  compose exec -T uagent uagent --web
  exit 0
fi

if [ "$mode" = release ] && [ -n "$unsupported" ]; then build_from_source "$unsupported"; fi

# The agent's browser needs Chrome and TigerVNC on Linux; offer the packages
# the container image installs.
if [ "$os" = Linux ] && [ "$web" = yes ] && [ "$browser" = no ] && has apt-get &&
   ask "Install Chrome and TigerVNC for the agent's browser, with sudo?"; then
  deb=$tmp/chrome.deb
  fetch "https://dl.google.com/linux/direct/google-chrome-stable_current_$(dpkg --print-architecture).deb" "$deb"
  sudo apt-get install -y tigervnc-standalone-server xauth fonts-noto-core \
    fonts-noto-color-emoji "$deb"
fi

bin=$prefix/bin/uagent
installed=
if [ -x "$bin" ]; then installed=$("$bin" --version 2>/dev/null | sed 's/^uagent //'); fi
if [ "$mode" = source ]; then
  # Reuse a built UI only while its source inputs match. Without npm, CMake
  # can still embed a supplied bundle or build the CLI-only configuration.
  if [ "$node" = yes ] && ! node "$root/web/scripts/build-state.js" check; then
    if ! node "$root/web/scripts/build-state.js" check-dependencies; then
      npm ci --prefix "$root/web"
      node "$root/web/scripts/build-state.js" record-dependencies
    fi
    npm run build --prefix "$root/web"
  fi
  if [ ! -f "$build/CMakeCache.txt" ]; then
    cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
  fi
  # Every run says whether the web UI is built: a tree first configured
  # without it gains it once Node.js or a built web/dist is here, and a tree
  # configured by hand keeps its other choices.
  cmake -S "$root" -B "$build" -DUAGENT_WEB="$([ "$web" = yes ] && echo ON || echo OFF)"
  cmake --build "$build" --target uagent --parallel "$build_jobs"
  cmake --install "$build" --prefix "$prefix" --strip
  policy_source=$root/deploy/chrome-policy.json
else
  release_version
  archive=uagent-$version-$target.tar.gz
  base="https://github.com/$repo/releases/download/v$version"
  if [ -n "$installed" ] && [ "$installed" != "$version" ]; then
    echo "Updating µAgent $installed to $version"
  else
    echo "Installing µAgent $version"
  fi
  fetch "$base/$archive" "$tmp/$archive"
  fetch "$base/$archive.sha256" "$tmp/$archive.sha256"
  expected=$(cut -d' ' -f1 <"$tmp/$archive.sha256")
  if has sha256sum; then
    actual=$(sha256sum "$tmp/$archive" | cut -d' ' -f1)
  else
    actual=$(shasum -a 256 "$tmp/$archive" | cut -d' ' -f1)
  fi
  if [ -z "$expected" ] || [ "$expected" != "$actual" ]; then
    echo "checksum mismatch for $archive; nothing was installed" >&2
    exit 1
  fi
  tar -xzf "$tmp/$archive" -C "$tmp"
  unpacked=$tmp/uagent-$version
  # A binary this system cannot load (a library it lacks) replaces nothing.
  if ! problem=$("$unpacked/bin/uagent" --version 2>&1 >/dev/null); then
    build_from_source "the prebuilt µAgent does not start here: $(echo "$problem" | head -n 1)"
  fi
  mkdir -p "$prefix/bin" "$prefix/share/doc"
  # What a release ships replaces what an earlier one did; a binary that is
  # running keeps its old file until it restarts.
  rm -rf "$prefix/share/uagent" "$prefix/share/doc/uagent"
  cp -R "$unpacked/share/uagent" "$prefix/share/uagent"
  cp -R "$unpacked/share/doc/uagent" "$prefix/share/doc/uagent"
  cp "$unpacked/bin/uagent" "$bin.new"
  mv -f "$bin.new" "$bin"
  policy_source=$tmp/chrome-policy.json
  fetch "https://raw.githubusercontent.com/$repo/v$version/deploy/chrome-policy.json" "$policy_source" 2>/dev/null ||
    policy_source=
fi

# Chrome reads managed policy only from a root-owned directory.
policy=/etc/opt/chrome/policies/managed/uagent.json
if [ "$os" = Linux ] && [ "$web" = yes ] && [ -n "$policy_source" ] && has google-chrome-stable &&
   ! cmp -s "$policy_source" "$policy" 2>/dev/null; then
  if ask "Install the Chrome policy for the agent's browser (downloads PDFs, blocks the password pages), with sudo?"; then
    sudo install -D -m 0644 "$policy_source" "$policy"
  elif [ "$mode" = source ]; then
    echo "To install it later: sudo install -D -m 0644 $policy_source $policy"
  fi
fi

[ "$interactive" = yes ] || {
  echo "Installed $bin."
  exit 0
}

# Where the shell looks for it.
case :$PATH: in
  *:"$prefix/bin":*) ;;
  *)
    case ${SHELL##*/} in
      zsh) rc=$HOME/.zshrc line="export PATH=\"$prefix/bin:\$PATH\"" ;;
      fish) rc=$HOME/.config/fish/conf.d/uagent.fish line="fish_add_path $prefix/bin" ;;
      bash) rc=$HOME/.bashrc line="export PATH=\"$prefix/bin:\$PATH\""
        [ "$os" = Darwin ] && rc=$HOME/.bash_profile ;;
      *) rc=$HOME/.profile line="export PATH=\"$prefix/bin:\$PATH\"" ;;
    esac
    if ask_yes "$prefix/bin is not on your PATH. Add it in $rc?"; then
      mkdir -p "$(dirname -- "$rc")"
      grep -qsF "$line" "$rc" || printf '\n%s\n' "$line" >>"$rc"
      echo "Open a new terminal, or run: $line"
    else
      echo "To find uagent, add this to your shell's startup file: $line"
    fi
    ;;
esac

# Keys and the default model, before a service starts that reads them.
if "$bin" --help 2>/dev/null | grep -q '^uagent setup'; then
  if (cd "$HOME" && "$bin" --control '{"kind":"provider"}' 2>/dev/null) |
     grep -q '"configured":true'; then
    ! ask "Change the API keys?" || "$bin" setup </dev/tty || true
  else
    "$bin" setup </dev/tty || true
  fi
fi

# The web interface as a service of this user, started at login.
service=
if "$bin" --help 2>/dev/null | grep -q -- '--web'; then
  if [ "$os" = Linux ] && has systemctl && systemctl --user show-environment >/dev/null 2>&1; then
    service=systemd
    unit=${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user/uagent-web.service
    existing=$unit
  elif [ "$os" = Darwin ]; then
    service=launchd
    label=com.github.timongentzsch.uagent.web
    plist=$HOME/Library/LaunchAgents/$label.plist
    existing=$plist
  fi
fi
web_service=no
if [ -n "$service" ] && [ -f "$existing" ]; then
  # Installed before: it keeps running, on the new binary.
  web_service=yes
  if [ "$service" = systemd ]; then
    systemctl --user restart uagent-web
  else
    launchctl kickstart -k "gui/$(id -u)/$label"
  fi
elif [ -n "$service" ] &&
     ask_yes "Keep the web interface running in the background, starting when you log in?"; then
  web_service=yes
  if [ "$service" = systemd ]; then
    mkdir -p "$(dirname -- "$unit")"
    # Agent commands find the tools of the shell that installed it. A
    # runtime moves into its own scope (docs/WEB.md), so the default kill
    # policy stays.
    cat >"$unit" <<EOF
[Unit]
Description=µAgent web interface
After=network-online.target

[Service]
ExecStart=$bin --web
Environment="PATH=$prefix/bin:$PATH"
Environment=UAGENT_WEB_PORT=$port
Restart=on-failure

[Install]
WantedBy=default.target
EOF
    systemctl --user daemon-reload
    systemctl --user enable --now uagent-web
    if [ "$(loginctl show-user "$USER" -p Linger --value 2>/dev/null)" = no ] &&
       ask "Also keep it running while you are logged out?"; then
      loginctl enable-linger "$USER"
    fi
    echo "Stop and remove it with: systemctl --user disable --now uagent-web && rm $unit"
  else
    mkdir -p "$(dirname -- "$plist")" "$HOME/Library/Logs"
    cat >"$plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>$label</string>
  <key>ProgramArguments</key>
  <array><string>$bin</string><string>--web</string></array>
  <key>EnvironmentVariables</key>
  <dict>
    <key>PATH</key><string>$prefix/bin:$PATH</string>
    <key>UAGENT_WEB_PORT</key><string>$port</string>
  </dict>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><dict><key>SuccessfulExit</key><false/></dict>
  <key>StandardOutPath</key><string>$HOME/Library/Logs/uagent-web.log</string>
  <key>StandardErrorPath</key><string>$HOME/Library/Logs/uagent-web.log</string>
</dict>
</plist>
EOF
    launchctl bootout "gui/$(id -u)/$label" 2>/dev/null || true
    launchctl bootstrap "gui/$(id -u)" "$plist"
    echo "Stop and remove it with: launchctl bootout gui/$(id -u)/$label && rm $plist"
  fi
fi

if [ "$web_service" = yes ]; then
  # The service's host answers, then hands out a pairing code.
  # Asking before it answers would start a second host in the foreground.
  tries=0
  until curl -fs -o /dev/null "http://127.0.0.1:$port/"; do
    tries=$((tries + 1))
    [ "$tries" -lt 20 ] || break
    sleep 0.5
  done
  if [ "$tries" -lt 20 ]; then
    echo "Installed $bin. The web interface is running; open the link below to pair this browser."
    echo "Start the terminal with: uagent  (uagent --web pairs another device)"
    "$bin" --web || true
  else
    echo "Installed $bin, but the web interface did not answer on port $port." >&2
    if [ "$service" = systemd ]; then
      echo "See: systemctl --user status uagent-web (set UAGENT_WEB_PORT if another program uses $port)" >&2
    else
      echo "See: ~/Library/Logs/uagent-web.log (set UAGENT_WEB_PORT if another program uses $port)" >&2
    fi
  fi
elif [ "$web" = yes ]; then
  echo "Installed $bin. Start it with: uagent, or the web interface with: uagent --web"
else
  echo "Installed $bin. Start it with: uagent"
fi
