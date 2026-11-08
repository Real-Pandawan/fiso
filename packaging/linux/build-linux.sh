#!/usr/bin/env bash
# Builds FISO Explorer for Linux x64 and produces the AppImage.
#   ./packaging/linux/build-linux.sh
# Requirements: a C++20 compiler (GCC 11+), CMake >= 3.20, Node.js >= 20, git.
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build-linux"
APP="$ROOT/fiso-explorer"
JOBS="$(nproc 2>/dev/null || echo 4)"

step() { printf '\n==> %s\n' "$*"; }
fail() { printf '\nerror: %s\n' "$*" >&2; exit 1; }

[[ "$(uname -s)" == "Linux" ]] || fail "run this script on Linux"
[[ "$(uname -m)" == "x86_64" ]] || fail "this script builds the x64 AppImage on an x86_64 machine"
command -v cmake >/dev/null || fail "CMake missing (e.g. 'sudo apt install cmake')"
command -v node >/dev/null || fail "Node.js missing (version 20 or later)"
(( "$(node -p 'process.versions.node.split(".")[0]')" >= 20 )) || fail "Node.js 20 or later is required"

step "Engine and command-line tool"
cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" -j "$JOBS"

step "Tests"
(cd "$BUILD" && ctest --output-on-failure)

BIN="$BUILD/fiso-cli/fiso"
if ldd "$BIN" | grep -v -E 'linux-vdso|libc\.so|libm\.so|ld-linux'; then
  fail "fiso depends on a library that is not part of every distribution (see above)"
fi

step "Application"
mkdir -p "$APP/bin"
cp "$BIN" "$APP/bin/fiso"
cd "$APP"
npm ci --no-audit --no-fund
rm -rf dist
npx electron-builder --linux AppImage --x64 --publish never
APPIMAGE="$(ls -1 dist/*.AppImage | head -1)"
[[ -f "$APPIMAGE" ]] || fail "no AppImage produced"
echo "$APPIMAGE"
