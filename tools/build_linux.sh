#!/bin/sh
# Configure + build the release (live recompilation) Linux build with clang and Ninja.
# Run from the repository root after setting up lib/ and applying patches/ (see README).
set -e
cd "$(dirname "$0")/.."
if [ ! -f build-live/build.ninja ]; then
  # Extra flags for newer toolchains (harmless on older ones):
  # - clang 23 turns -Wuninitialized-const-pointer into an error inside rt64's bundled imgui;
  # - RmlUi's robin_hood.h relies on <cstdint> being included transitively;
  # - CMake 4 refuses some bundled projects' old cmake_minimum_required without a policy floor.
  cmake -S . -B build-live -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DMT_LIVE_RECOMP=ON \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    "-DCMAKE_CXX_FLAGS=-Wno-unknown-warning-option -Wno-error=uninitialized-const-pointer -include cstdint"
fi
cmake --build build-live --target MarioTennisRecompiled
