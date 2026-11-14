#!/bin/sh
# Keeps the agent's Google Chrome on the data volume: Google's own stable
# package, checked against the repository index Google signs, unpacked
# beside the one in use and switched to by a link. The browser takes the
# new one the next time it starts.
#   chrome-update          check once
#   chrome-update --loop   now, then daily (sooner after a failure)
set -eu

root=${UAGENT_BROWSER_DATA:-/data/browser}/chrome
repo=${UAGENT_CHROME_REPO:-https://dl.google.com/linux/chrome/deb}
keyring=${UAGENT_CHROME_KEYRING:-/usr/local/share/uagent/google-chrome.gpg}
arch=$(dpkg --print-architecture)
mkdir -p "$root" "$root/bin"

say() {
  printf '%s\n' "$1" >"$root/status.new" && mv "$root/status.new" "$root/status"
}
fetch() { curl -fsS --retry 3 --retry-delay 5 -o "$2" "$1"; }
# The value of `field` in the stanza of Packages that names our package.
field() {
  awk -v want="$1" '
    /^Package: / { ours = ($2 == "google-chrome-stable") }
    ours && index($0, want ": ") == 1 { print substr($0, length(want) + 3); exit }
  ' "$work/Packages"
}

check() {
  work=$root/work
  rm -rf "$work"
  mkdir -p "$work"
  fetch "$repo/dists/stable/Release" "$work/Release"
  fetch "$repo/dists/stable/Release.gpg" "$work/Release.gpg"
  gpgv --quiet --keyring "$keyring" "$work/Release.gpg" "$work/Release" 2>/dev/null ||
    { echo "the index is not signed by Google's key"; return 1; }
  sum=$(awk -v path="main/binary-$arch/Packages" '
    /^SHA256:/ { on = 1; next } /^[^ ]/ { on = 0 }
    on && $3 == path { print $1; exit }' "$work/Release")
  [ -n "$sum" ] || { echo "the index lists no packages for $arch"; return 1; }
  fetch "$repo/dists/stable/main/binary-$arch/Packages" "$work/Packages"
  echo "$sum  $work/Packages" | sha256sum --check --status ||
    { echo "the package list does not match the index"; return 1; }
  version=$(field Version)
  file=$(field Filename)
  sum=$(field SHA256)
  case "$version$file$sum" in
    *[!A-Za-z0-9._/+~:-]* | "") echo "the package list is not as expected"; return 1 ;;
  esac
  if [ "$(readlink "$root/current" 2>/dev/null)" = "$version" ]; then
    say "ready $version"
    rm -rf "$work"
    return 0
  fi
  [ -e "$root/current" ] || say "downloading $version"
  fetch "$repo/$file" "$work/chrome.deb"
  echo "$sum  $work/chrome.deb" | sha256sum --check --status ||
    { echo "the package does not match its checksum"; return 1; }
  rm -rf "$root/$version"
  dpkg-deb --extract "$work/chrome.deb" "$root/$version"
  [ -x "$root/$version/opt/google/chrome/google-chrome" ] ||
    { echo "the package holds no Chrome"; return 1; }
  # A running Chrome keeps the folder it started from: only one older than
  # the one being replaced goes.
  before=$(readlink "$root/current" 2>/dev/null || true)
  for old in "$root"/*/; do
    old=$(basename "$old")
    case $old in "$version" | "$before" | bin | work) ;; *) rm -rf "${root:?}/$old" ;; esac
  done
  ln -sfn ../current/opt/google/chrome/google-chrome "$root/bin/google-chrome-stable"
  ln -sfn "$version" "$root/current.new"
  mv -T "$root/current.new" "$root/current"
  rm -rf "$work"
  say "ready $version"
}

attempt() {
  # A subshell of its own, so a step that fails ends the check.
  set +e
  (set -e; check) >"$root/reason" 2>/dev/null
  code=$?
  set -e
  reason=$(cat "$root/reason")
  rm -f "$root/reason"
  [ "$code" -ne 0 ] || return 0
  # What is installed stays in use; only a first download has nothing.
  [ -e "$root/current" ] || say "failed ${reason:-Chrome could not be downloaded}"
  echo "chrome-update: ${reason:-Chrome could not be downloaded}" >&2
  return 1
}

if [ "${1:-}" != --loop ]; then
  attempt
  exit
fi
while :; do
  if attempt; then sleep 86400; else sleep 600; fi
done
