#!/usr/bin/env bash
# The quick loop on one change: build what changed, run the narrowest tests.
#   .github/dev.sh unit [FILTER]  unit tests, those whose name has FILTER
#   .github/dev.sh int FILTER     integration tests whose name has FILTER
#   .github/dev.sh web [SPEC…]    the bundle, then browser specs in Chromium
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
tree=build/dev
build() {
  [ -f web/dist/.assets ] || (cd web && npm run build >/dev/null)
  [ -f "$tree/build.ninja" ] || cmake --preset dev >/dev/null
  cmake --build --preset dev --parallel
}
case "${1:-}" in
  unit) build && "$tree/uagent_tests" ${2:+-k "$2"} ;;
  int) [ -n "${2:-}" ] && build && python3 tests/integration.py "$tree/uagent" -k "$2" -j 4 ;;
  web)
    shift
    (cd web && npm run build >/dev/null)
    build
    cd web && UAGENT_TEST_BINARY="../$tree/uagent" npx playwright test --project=chromium "$@"
    ;;
  *) sed -n 2,6p "$0" >&2; exit 2 ;;
esac
