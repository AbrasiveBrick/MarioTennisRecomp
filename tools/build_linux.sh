#!/bin/sh
# Configure + build the release (live recompilation) Linux build with clang and Ninja.
# Run from the repository root after setting up lib/ and applying patches/ (see README).
set -e
cd "$(dirname "$0")/.."
if [ ! -f build-live/build.ninja ]; then
  cmake -S . -B build-live -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DMT_LIVE_RECOMP=ON
fi
cmake --build build-live --target MarioTennisRecompiled
