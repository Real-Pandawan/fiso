#!/usr/bin/env bash
# Builds FISO Explorer for macOS and produces the .dmg installer.
#   ./packaging/macos/build-mac.sh x64     # Intel
#   ./packaging/macos/build-mac.sh arm64   # Apple Silicon
# Requirements: Xcode Command Line Tools, CMake >= 3.20, Node.js >= 20, git.
# The app is signed ad hoc (no Apple Developer ID); see docs/BUILDING.md.
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

ARCH="${1:-$(uname -m | sed 's/x86_64/x64/')}"
case "$ARCH" in
  x64)   CMAKE_ARCH=x86_64 ;;
  arm64) CMAKE_ARCH=arm64 ;;
  *) echo "usage: $0 x64|arm64" >&2; exit 1 ;;
esac

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build-mac-$ARCH"
APP="$ROOT/fiso-explorer"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

step() { printf '\n==> %s\n' "$*"; }
fail() { printf '\nerror: %s\n' "$*" >&2; exit 1; }

[[ "$(uname -s)" == "Darwin" ]] || fail "run this script on macOS"
xcode-select -p >/dev/null 2>&1 || fail "Xcode Command Line Tools missing: run 'xcode-select --install'"
command -v cmake >/dev/null || fail "CMake missing: 'brew install cmake'"
command -v node >/dev/null || fail "Node.js missing: 'brew install node'"
(( "$(node -p 'process.versions.node.split(".")[0]')" >= 20 )) || fail "Node.js 20 or later is required"

step "Engine and command-line tool ($CMAKE_ARCH, macOS 12+)"
cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES="$CMAKE_ARCH" -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0
cmake --build "$BUILD" -j "$JOBS"

if [[ "$CMAKE_ARCH" == "$(uname -m)" ]]; then
  step "Tests"
  (cd "$BUILD" && ctest --output-on-failure)
fi

BIN="$BUILD/fiso-cli/fiso"
[[ "$(lipo -archs "$BIN")" == "$CMAKE_ARCH" ]] || fail "unexpected architecture for fiso"
if otool -L "$BIN" | tail -n +2 | grep -v -E '/usr/lib/|/System/Library/'; then
  fail "fiso depends on a non-system library (see above)"
fi

step "Application bundle"
mkdir -p "$APP/bin"
cp "$BIN" "$APP/bin/fiso"
APPEX="$APP/native-build/FISOThumbnail.appex/Contents"
rm -rf "$APP/native-build" && mkdir -p "$APPEX/MacOS"
cp "$APP/native/thumbnail/Info.plist" "$APPEX/"
cp "$BUILD/fiso-explorer/native/thumbnail/FISOThumbnail" "$APPEX/MacOS/"

cd "$APP"
npm ci --no-audit --no-fund
rm -rf dist
export CSC_IDENTITY_AUTO_DISCOVERY=false
npx electron-builder --mac dir "--$ARCH" --publish never
APPDIR="$(ls -d dist/mac*/"FISO Explorer.app" | head -1)"
[[ -d "$APPDIR" ]] || fail "application bundle not produced"

step "Ad hoc signature"
codesign --force --sign - "$APPDIR/Contents/Resources/bin/fiso"
codesign --force --deep --sign - "$APPDIR"
codesign --force --sign - --entitlements native/thumbnail/entitlements.plist "$APPDIR/Contents/PlugIns/FISOThumbnail.appex"
codesign --force --sign - "$APPDIR"
codesign --verify --deep --strict "$APPDIR"

step "Disk image"
npx electron-builder --mac dmg "--$ARCH" --prepackaged "$APPDIR" --publish never
DMG="$(ls -1 dist/*.dmg | head -1)"
[[ -f "$DMG" ]] || fail "no .dmg produced"
echo "$DMG"
