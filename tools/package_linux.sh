#!/bin/sh
# Pack build-live/ into a shareable archive: the executable (stripped), its UI assets,
# shader presets and controller database. No ROM, game code or game data goes in.
# Usage: sh tools/package_linux.sh [version]  ->  dist/MarioTennisRecompiled-<version>-linux-x86_64.tar.gz
set -e
cd "$(dirname "$0")/.."
VERSION="${1:-dev}"
NAME=MarioTennisRecompiled
STAGE="dist/$NAME"
rm -rf "$STAGE" && mkdir -p "$STAGE"
cp build-live/$NAME "$STAGE/"
strip "$STAGE/$NAME"
cp -r assets slang-shaders recompcontrollerdb.txt LICENSE "$STAGE/"
cp tools/release/README-linux.txt "$STAGE/README.txt"
# Refuse to ship anything that looks like a ROM or save.
if find "$STAGE" -type f \( -iname '*.z64' -o -iname '*.n64' -o -iname '*.v64' -o -iname '*.rom' -o -iname '*.eep' -o -iname '*.sra' \) | grep .; then
  echo "ROM or save file found in the package, aborting" >&2; exit 1
fi
OUT="dist/$NAME-$VERSION-linux-x86_64.tar.gz"
tar -C dist -czf "$OUT" "$NAME"
echo "$OUT"
