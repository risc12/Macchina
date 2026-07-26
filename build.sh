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
#   ./build.sh              # build the new/ Studio targets + new-cpp/ (default)
#   ./build.sh cpp          # just the new-cpp/ C++ targets
#   ./build.sh all          # v3 targets + new-cpp/ + legacy client/server
#   ./build.sh studio       # new/clients/StudioDemo -> build/StudioDemo
#   ./build.sh ledprobe     # new/tools/LedProbe    -> build/LedProbe
#   ./build.sh ledpoke      # new/tools/LedPoke     -> build/LedPoke
#   ./build.sh hiaclient    # new/tools/HIAClient   -> build/HIAClient
#   ./build.sh legacy       # NICommon MacchinaClient + MacchinaServer
#   ./build.sh check        # syntax-check every v3 source
#   ./build.sh dev <Client> [args...]   # watch new-cpp/clients/<Client>/, rebuild
#                                        # + restart on change (needs entr)
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

# new-cpp/ — the C++20 port (CoreFoundation-only, no ObjC). See new-cpp/README.md.
CPPFLAGS=(-std=c++20 -Wall -Inew-cpp -mmacosx-version-min=10.13)
CPP_CORE_SRC=(new-cpp/core/transport/*.cpp new-cpp/core/transport-macos/*.cpp
              new-cpp/core/protocol/*.cpp new-cpp/core/client/*.cpp)
CPP_STUDIO_SRC=(new-cpp/studio/*.cpp)
CPP_GFX_SRC=(new-cpp/gfx/*.cpp)
CPP_OSC_SRC=(new-cpp/daemon/osc/*.cpp)

build_cpp () {
  local name="$1" main="$2"
  echo "==> Building $name (new-cpp/core + new-cpp/studio)"
  clang++ "${CPPFLAGS[@]}" -framework CoreFoundation \
    "${CPP_CORE_SRC[@]}" "${CPP_STUDIO_SRC[@]}" "${CPP_GFX_SRC[@]}" "${CPP_OSC_SRC[@]}" \
    "$main" -o "$OUT/$name"
  echo "    -> $OUT/$name"
}

build_cpp_all () {
  build_cpp HandshakeSmoke new-cpp/tools/HandshakeSmoke/main.cpp
  build_cpp StudioDemoCpp  new-cpp/clients/StudioDemo/main.cpp
  build_cpp StudioProbe    new-cpp/clients/StudioProbe/main.cpp
  build_cpp HelloScreen    new-cpp/clients/HelloScreen/main.cpp
  build_cpp Showcase       new-cpp/clients/Showcase/main.cpp
  build_cpp DevTool        new-cpp/clients/DevTool/main.cpp
  build_cpp macchinad      new-cpp/daemon/main.cpp
}

# compile_commands.json for clangd (nvim/VSCode LSP). Regenerate after
# adding/removing source files: ./build.sh compdb
compdb () {
  echo "==> Generating compile_commands.json"
  local dir; dir="$(pwd)"
  {
    echo "["
    local first=1 f
    for f in "${CPP_CORE_SRC[@]}" "${CPP_STUDIO_SRC[@]}" "${CPP_GFX_SRC[@]}" "${CPP_OSC_SRC[@]}" \
             new-cpp/tools/*/main.cpp new-cpp/clients/*/main.cpp new-cpp/daemon/main.cpp; do
      [ $first -eq 1 ] || echo ","
      first=0
      printf '  {"directory": "%s",\n   "file": "%s",\n   "arguments": ["clang++"' "$dir" "$f"
      local a; for a in "${CPPFLAGS[@]}" -c "$f"; do printf ', "%s"' "$a"; done
      printf ']}'
    done
    for f in "${CORE_SRC[@]}" "${STUDIO_SRC[@]}" new/clients/*/main.m new/tools/*/main.m; do
      echo ","
      printf '  {"directory": "%s",\n   "file": "%s",\n   "arguments": ["clang"' "$dir" "$f"
      local a; for a in "${NEWCFLAGS[@]}" -c "$f"; do printf ', "%s"' "$a"; done
      printf ']}'
    done
    echo
    echo "]"
  } > compile_commands.json
  echo "    -> compile_commands.json ($(grep -c '"file"' compile_commands.json) entries)"
}

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
  new)        build_new_all; build_cpp_all ;;
  studio)    build_new StudioDemo new/clients/StudioDemo/main.m ;;
  ledprobe)  build_new LedProbe   new/tools/LedProbe/main.m ;;
  ledpoke)   build_new LedPoke    new/tools/LedPoke/main.m ;;
  hiaclient) build_new HIAClient  new/tools/HIAClient/main.m ;;
  cpp)       build_cpp_all ;;
  compdb)    compdb ;;
  legacy)
    build_legacy MacchinaClient MacchinaClient/main.m
    build_legacy MacchinaServer MacchinaServer/main.m
    ;;
  check)     syntax_check ;;
  all)
    build_new_all
    build_cpp_all
    build_legacy MacchinaClient MacchinaClient/main.m
    build_legacy MacchinaServer MacchinaServer/main.m
    ;;
  # Internal: re-invoked by `dev` (below) inside entr's subshell, where the
  # array vars (CPPFLAGS etc.) from the top of this script aren't inherited.
  client-build)
    build_cpp "$2" "new-cpp/clients/$2/main.cpp"
    ;;
  dev)
    client="${2:?usage: $0 dev <ClientName> [client-args...]}"
    main="new-cpp/clients/$client/main.cpp"
    [ -f "$main" ] || { echo "no such client: $main" >&2; exit 2; }
    command -v entr >/dev/null || { echo "entr not found — brew install entr" >&2; exit 2; }
    shift 2
    echo "==> dev: watching new-cpp/clients/$client/ — rebuild + restart on change (Ctrl-C to stop)"
    find "new-cpp/clients/$client" -type f \( -name '*.cpp' -o -name '*.hpp' \) | \
      entr -rn bash -c './build.sh client-build "$1" && exec "./'"$OUT"'/$1" "${@:2}"' _ "$client" "$@"
    ;;
  *) echo "usage: $0 [new|all|cpp|compdb|studio|ledprobe|ledpoke|hiaclient|legacy|check|dev]" >&2; exit 2 ;;
esac

echo "Done."
