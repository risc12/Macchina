#!/bin/bash
#
# build.sh — command-line build, independent of Xcode.
#
# Layout (2026 reorg):
#   new/core/     generic NHL2 v3 client library (device-agnostic transport + connect)
#   new/studio/   Maschine Studio specifics (NIStudioController: channels, LEDs, input)
#   new/clients/  runnable apps (StudioDemo)
#   new/tools/    interactive probes + research (LedProbe, LedPoke, HIAClient, attic/)
#   NICommon/, MacchinaClient/, MacchinaServer/  — legacy 2012 MK1 code (untouched)
#
# The project is ARC, Foundation-only, deployment target 10.7.
#
# Usage:
#   ./build.sh              # build the new/ Studio targets (default)
#   ./build.sh all          # v3 targets + legacy client/server
#   ./build.sh studio       # new/clients/StudioDemo -> build/StudioDemo
#   ./build.sh ledprobe     # new/tools/LedProbe    -> build/LedProbe
#   ./build.sh ledpoke      # new/tools/LedPoke     -> build/LedPoke
#   ./build.sh hiaclient    # new/tools/HIAClient   -> build/HIAClient
#   ./build.sh legacy       # NICommon MacchinaClient + MacchinaServer
#   ./build.sh check        # syntax-check every v3 source
#
set -euo pipefail
cd "$(dirname "$0")"

OUT="build"
mkdir -p "$OUT"

FRAMEWORKS=(-framework Foundation)
NEWCFLAGS=(-fobjc-arc -x objective-c -Inew/core -Inew/studio -Wall -mmacosx-version-min=10.7)
LEGACYCFLAGS=(-fobjc-arc -x objective-c -INICommon -Wall -mmacosx-version-min=10.7)

CORE_SRC=(new/core/*.m)
STUDIO_SRC=(new/studio/*.m)
COMMON_SRC=(NICommon/*.m)

# A new/ target = core + studio + one main.m.
build_new () {
  local name="$1" main="$2"
  echo "==> Building $name (new/core + new/studio)"
  clang "${NEWCFLAGS[@]}" "${FRAMEWORKS[@]}" "${CORE_SRC[@]}" "${STUDIO_SRC[@]}" "$main" -o "$OUT/$name"
  echo "    -> $OUT/$name"
}

build_legacy () {
  local name="$1" main="$2"
  echo "==> Building $name (legacy NICommon)"
  clang "${LEGACYCFLAGS[@]}" "${FRAMEWORKS[@]}" "${COMMON_SRC[@]}" "$main" -o "$OUT/$name"
  echo "    -> $OUT/$name"
}

build_new_all () {
  build_new StudioDemo new/clients/StudioDemo/main.m
  build_new LedProbe   new/tools/LedProbe/main.m
  build_new LedPoke    new/tools/LedPoke/main.m
  build_new HIAClient  new/tools/HIAClient/main.m
}

syntax_check () {
  echo "==> Syntax-checking new/ sources"
  local failed=0
  for f in "${CORE_SRC[@]}" "${STUDIO_SRC[@]}" \
           new/clients/StudioDemo/main.m new/tools/LedProbe/main.m \
           new/tools/LedPoke/main.m new/tools/HIAClient/main.m; do
    if clang "${NEWCFLAGS[@]}" -fsyntax-only "$f" 2>/tmp/macchina_syn.err; then
      echo "    ok   $f"
    else
      echo "    FAIL $f"; sed 's/^/         /' /tmp/macchina_syn.err; failed=1
    fi
  done
  return $failed
}

case "${1:-new}" in
  new)        build_new_all ;;
  studio)    build_new StudioDemo new/clients/StudioDemo/main.m ;;
  ledprobe)  build_new LedProbe   new/tools/LedProbe/main.m ;;
  ledpoke)   build_new LedPoke    new/tools/LedPoke/main.m ;;
  hiaclient) build_new HIAClient  new/tools/HIAClient/main.m ;;
  legacy)
    build_legacy MacchinaClient MacchinaClient/main.m
    build_legacy MacchinaServer MacchinaServer/main.m
    ;;
  check)     syntax_check ;;
  all)
    build_new_all
    build_legacy MacchinaClient MacchinaClient/main.m
    build_legacy MacchinaServer MacchinaServer/main.m
    ;;
  *) echo "usage: $0 [new|all|studio|ledprobe|ledpoke|hiaclient|legacy|check]" >&2; exit 2 ;;
esac

echo "Done."
