#!/bin/bash
# Builds build/Varispeed.driver (the Core Audio plug-in) from the BlackHole fork in driver/.
# Needs only Apple's Command Line Tools (no Xcode). Builds for this Mac's CPU unless ARCHS is set.
# Signs ad-hoc unless SIGN_IDENTITY is set.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/driver/BlackHole"
OUT="$ROOT/build/Varispeed.driver"
OBJ="$ROOT/build/driver_obj"
mkdir -p "$ROOT/build" "$OBJ"

driverName="Varispeed"
bundleID="com.undulaemusic.Varispeed.driver"
icon="BlackHole.icns"
version="1.0"
ARCHS="${ARCHS:-$(uname -m)}"
SIGN_IDENTITY="${SIGN_IDENTITY:--}"   # "-" = ad-hoc

DEFINES=(
  -DkDriver_Name="\"$driverName\""
  -DkPlugIn_BundleID="\"$bundleID\""
  -DkPlugIn_Icon="\"$icon\""
  -DkHas_Driver_Name_Format=false
  -DkDevice_Name="\"Varispeed\""
  -DkDevice2_Name="\"Varispeed_Mirror\""
  -DkNumber_Of_Channels=2
  -DkSampleRates=44100,48000,88200,96000
  -DkCanBeDefaultDevice=false
  -DkCanBeDefaultSystemDevice=false
  -DkVarispeed_DefaultSpeed="${DEFAULT_SPEED:-1.0}"
)
ARCH_FLAGS=(); for a in $ARCHS; do ARCH_FLAGS+=(-arch "$a"); done

clang -bundle -Os -std=gnu11 -mmacosx-version-min=12.0 "${ARCH_FLAGS[@]}" \
  -Wall -Wno-four-char-constants -Wno-unused-function -Wno-format-extra-args \
  "${DEFINES[@]}" -I"$SRC" \
  "$SRC/BlackHole.c" \
  -framework CoreAudio -framework CoreFoundation -framework Accelerate \
  -o "$OBJ/$driverName" 2> "$ROOT/build/driver_build.log" \
  || { grep -E "error" "$ROOT/build/driver_build.log"; echo "** BUILD FAILED ** (full log: build/driver_build.log)"; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT/Contents/MacOS" "$OUT/Contents/Resources"
cp "$OBJ/$driverName" "$OUT/Contents/MacOS/$driverName"
# (no icon: the only one in the BlackHole source is their logo, which we may not use)
sed -e "s/\${EXECUTABLE_NAME}/$driverName/g" \
    -e "s/\$(PRODUCT_BUNDLE_IDENTIFIER)/$bundleID/g" \
    -e "s/\${PRODUCT_NAME}/$driverName/g" \
    -e "s/\$(MARKETING_VERSION)/$version/g" \
    "$SRC/BlackHole.plist" > "$OUT/Contents/Info.plist"
plutil -lint -s "$OUT/Contents/Info.plist"

codesign --force --sign "$SIGN_IDENTITY" "$OUT" 2>/dev/null
echo "** BUILD SUCCEEDED ** ($OUT, $ARCHS)"
