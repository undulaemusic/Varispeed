#!/bin/bash
# Builds build/Varispeed.app (menu bar app + bridge + recorder) and signs it.
# Signs with your Apple Development certificate if there is one (so macOS remembers the
# microphone permission across rebuilds), otherwise ad-hoc.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OBJ="$ROOT/build/app_obj"
APP="$ROOT/build/Varispeed.app"
mkdir -p "$OBJ"

CFLAGS=(-O2 -Wall -target arm64-apple-macos13.0 -DHAVE_CONFIG_H -I"$ROOT/third_party/libsamplerate")
C_SOURCES=(
  bridge/VSBridge.c bridge/VSRecorder.c app/Sources/VSControl.c
  third_party/libsamplerate/samplerate.c third_party/libsamplerate/src_linear.c
  third_party/libsamplerate/src_sinc.c third_party/libsamplerate/src_zoh.c
)
OBJECTS=()
for src in "${C_SOURCES[@]}"; do
  o="$OBJ/$(basename "${src%.c}").o"
  clang "${CFLAGS[@]}" -c "$ROOT/$src" -o "$o"
  OBJECTS+=("$o")
done

swiftc -O -target arm64-apple-macos13.0 -parse-as-library \
  -import-objc-header "$ROOT/app/Sources/Bridging.h" \
  "$ROOT"/app/Sources/*.swift "${OBJECTS[@]}" \
  -framework CoreAudio -framework AVFoundation -framework SwiftUI -framework AppKit \
  -o "$OBJ/Varispeed"

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$OBJ/Varispeed" "$APP/Contents/MacOS/Varispeed"
cp "$ROOT/app/Info.plist" "$APP/Contents/Info.plist"
[ -f "$ROOT/app/AppIcon.icns" ] && cp "$ROOT/app/AppIcon.icns" "$APP/Contents/Resources/" || true

IDENTITY=$(security find-identity -v -p codesigning 2>/dev/null | awk -F'"' '/Apple Development/ {print $2; exit}')
codesign --force --sign "${IDENTITY:--}" "$APP"
echo "Built $APP (signed: ${IDENTITY:-ad-hoc})"
