#!/bin/sh
# Installs µAgent the most capable way this machine allows: it says what it
# found, recommends, and asks once. In a container µAgent runs its web
# interface, browser and terminal isolated from this machine; installed here
# it has the terminal, the web interface when Node.js is present, and the
# browser on Linux with Chrome and TigerVNC. UAGENT_INSTALL=container|here
# chooses without asking; without a terminal it installs here. Nothing runs
# with sudo unless you agree.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build=${UAGENT_BUILD_DIR:-"$root/build/release"}
prefix=${UAGENT_PREFIX:-"$HOME/.local"}
build_jobs=${UAGENT_BUILD_JOBS:-${CMAKE_BUILD_PARALLEL_LEVEL:-4}}

has() { command -v "$1" >/dev/null 2>&1; }
yes_no() { [ "$1" = yes ] && echo found || echo missing; }
ask() {
  [ -t 0 ] || return 1
  printf '%s ' "$1"
  read -r answer || return 1
  [ "$answer" = y ] || [ "$answer" = Y ]
}

docker=no
if has docker && docker info >/dev/null 2>&1; then docker=yes; fi
node=no
if has npm; then node=yes; fi
# The web interface, and with it the browser, needs Node.js or a built web/dist.
web=$node
if [ -f "$root/web/dist/.assets" ]; then web=yes; fi
browser=no
if [ "$(uname)" = Linux ] && has google-chrome-stable && has Xtigervnc &&
   has xauth; then
  browser=yes
fi
if [ "$(uname)" = Linux ]; then
  echo "Docker: $(yes_no "$docker"), Node.js: $(yes_no "$node"), Chrome and TigerVNC: $(yes_no "$browser")"
else
  echo "Docker: $(yes_no "$docker"), Node.js: $(yes_no "$node"); the agent's browser runs in the container or on Linux"
fi

# The container keeps an agent that runs commands and browses away from this
# machine's files and logins, so it comes first where Docker runs.
# A run without a terminal (a script, CI) installs here, as before.
choice=${UAGENT_INSTALL:-here}
if [ -z "${UAGENT_INSTALL:-}" ] && [ "$docker" = yes ] && [ -t 0 ]; then
  echo "1) In a container (recommended): web interface, browser and terminal, isolated from this machine"
  echo "2) Here: the terminal$([ "$web" = yes ] && echo ", the web interface")$([ "$web$browser" = yesyes ] && echo " and the browser")"
  printf 'Install [1]: '
  read -r answer || answer=
  [ "$answer" = 2 ] || choice=container
fi

if [ "$choice" = container ]; then
  mkdir -p "$root/workspaces"
  docker compose -f "$root/deploy/compose.yaml" up --build -d
  echo "Open http://127.0.0.1:${UAGENT_WEB_PORT:-8080} and pair with the code in:"
  echo "  docker compose -f $root/deploy/compose.yaml logs uagent"
  exit 0
fi

# The agent's browser needs Chrome and TigerVNC on Linux; offer the packages
# the container image installs.
if [ "$(uname)" = Linux ] && [ "$web" = yes ] && [ "$browser" = no ] && has apt-get &&
   ask "Install Chrome and TigerVNC for the agent's browser, with sudo? [y/N]"; then
  deb=$(mktemp --suffix=.deb)
  curl -fsSL -o "$deb" \
    "https://dl.google.com/linux/direct/google-chrome-stable_current_$(dpkg --print-architecture).deb"
  sudo apt-get install -y tigervnc-standalone-server xauth fonts-noto-core \
    fonts-noto-color-emoji "$deb"
  rm -f "$deb"
fi

# Reuse a built UI only while its source inputs match. Without npm, CMake
# can still embed a supplied bundle or build the CLI-only configuration.
if [ "$node" = yes ] && ! node "$root/web/scripts/build-state.js" check; then
  if ! node "$root/web/scripts/build-state.js" check-dependencies; then
    npm ci --prefix "$root/web"
    node "$root/web/scripts/build-state.js" record-dependencies
  fi
  npm run build --prefix "$root/web"
fi
# Configured on every run: a tree first built without the web UI gains it
# once Node.js or a built web/dist is here.
cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
  -DUAGENT_WEB="$([ "$web" = yes ] && echo ON || echo OFF)"
cmake --build "$build" --target uagent --parallel "$build_jobs"
cmake --install "$build" --prefix "$prefix" --strip
# Chrome reads managed policy only from a root-owned directory.
policy=/etc/opt/chrome/policies/managed/uagent.json
if [ "$(uname)" = Linux ] && [ "$web" = yes ] && has google-chrome-stable &&
   ! cmp -s "$root/deploy/chrome-policy.json" "$policy" 2>/dev/null; then
  if ask "Install the Chrome policy for the agent's browser (downloads PDFs, blocks the password pages), with sudo? [y/N]"; then
    sudo install -D -m 0644 "$root/deploy/chrome-policy.json" "$policy"
  else
    echo "To install it later: sudo install -D -m 0644 $root/deploy/chrome-policy.json $policy"
  fi
fi
if [ "$web" = yes ]; then
  echo "Installed $prefix/bin/uagent. Start the web interface with: uagent --web"
else
  echo "Installed $prefix/bin/uagent. Start it with: uagent"
fi
