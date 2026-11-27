#!/bin/sh
# Fetch the pinned Zelda64Recomp checkout, link its lib/ folder here and apply patches/.
# Used by the release workflow; also works for a fresh local Linux checkout.
# Usage: sh tools/setup_lib.sh [directory for the Zelda64Recomp clone, default ../Zelda64Recomp]
set -e
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
Z64="${1:-$ROOT/../Zelda64Recomp}"
Z64_COMMIT=b65c482

if [ ! -d "$Z64/.git" ]; then
  git clone https://github.com/Zelda64Recomp/Zelda64Recomp "$Z64"
fi
git -C "$Z64" checkout -q "$Z64_COMMIT"
git -C "$Z64" submodule update --init --recursive --depth 1 || git -C "$Z64" submodule update --init --recursive

if [ ! -e lib ]; then
  case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) # A junction needs no admin rights.
      powershell -NoProfile -Command "New-Item -ItemType Junction -Path lib -Target '$(cygpath -w "$Z64/lib")' | Out-Null" ;;
    *) ln -s "$Z64/lib" lib ;;
  esac
fi

apply() { # apply <dir> <patch>: skip when already applied
  if git -C "$1" apply --check "$2" 2>/dev/null; then
    git -C "$1" apply "$2"
  elif git -C "$1" apply --reverse --check "$2" 2>/dev/null; then
    echo "already applied: $2"
  else
    echo "patch does not apply: $2" >&2; exit 1
  fi
}
apply lib/N64ModernRuntime "$ROOT/patches/n64modernruntime-mario-tennis.patch"
apply lib/N64ModernRuntime/N64Recomp "$ROOT/patches/n64recomp-mario-tennis.patch"
apply lib/rt64 "$ROOT/patches/rt64-mario-tennis.patch"
# Needed by clang 23+, harmless before.
apply lib/N64ModernRuntime/N64Recomp/lib/fmt "$ROOT/patches/fmt-clang23.patch"
