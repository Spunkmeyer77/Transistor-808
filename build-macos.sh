#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
if [[ "$(uname -s)" != Darwin ]]; then
  echo "Run this script on macOS." >&2
  exit 1
fi
if ! xcode-select -p >/dev/null || ! command -v cmake >/dev/null; then
  echo "Install Apple Command Line Tools and CMake 3.22+ before building." >&2
  exit 1
fi
sdk="$PWD/build/max-sdk-base"
if [[ ! -d "$sdk/.git" ]]; then
  mkdir -p "$PWD/build"
  git clone https://github.com/Cycling74/max-sdk-base.git "$sdk"
fi
sdk_revision=c03a2922a2a8ff149165a1e2ea134e9321d6e202
git -C "$sdk" checkout --detach "$sdk_revision"
cmake -S . -B build/macos -DCMAKE_BUILD_TYPE=Release -DMAX_SDK_BASE="$sdk"
cmake --build build/macos --config Release --target lemus808_tilde --parallel
echo "External built at build/macos/externals/ticco808cal~.mxo"
