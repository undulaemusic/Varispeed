#!/bin/bash
# Builds Varispeed.driver from the BlackHole fork in ../driver.
# Output: build/Varispeed.driver (ad-hoc signed unless SIGN_IDENTITY is set).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT/driver"

driverName="Varispeed"
bundleID="com.undulaemusic.Varispeed.driver"
icon="BlackHole.icns"
SIGN_IDENTITY="${SIGN_IDENTITY:--}"   # "-" = ad-hoc

xcodebuild \
  -project BlackHole.xcodeproj \
  -target BlackHole \
  -configuration "${CONFIGURATION:-Release}" \
  CONFIGURATION_BUILD_DIR="$ROOT/build" \
  SYMROOT="$ROOT/build/sym" OBJROOT="$ROOT/build/obj" \
  PRODUCT_NAME="$driverName" \
  PRODUCT_BUNDLE_IDENTIFIER="$bundleID" \
  ARCHS="arm64 x86_64" ONLY_ACTIVE_ARCH=NO \
  CODE_SIGN_STYLE=Manual DEVELOPMENT_TEAM="" CODE_SIGN_IDENTITY="$SIGN_IDENTITY" \
  GCC_PREPROCESSOR_DEFINITIONS='$GCC_PREPROCESSOR_DEFINITIONS
  kDriver_Name=\"'$driverName'\"
  kPlugIn_BundleID=\"'$bundleID'\"
  kPlugIn_Icon=\"'$icon'\"
  kHas_Driver_Name_Format=false
  kDevice_Name=\"Varispeed\"
  kDevice2_Name=\"Varispeed_Mirror\"
  kNumber_Of_Channels=2
  kSampleRates=44100,48000,88200,96000
  kCanBeDefaultDevice=false
  kCanBeDefaultSystemDevice=false
  kVarispeed_DefaultSpeed=${DEFAULT_SPEED:-1.0}' \
  | grep -E "error|warning: .*BlackHole.c|BUILD (SUCCEEDED|FAILED)" || true

test -d "$ROOT/build/$driverName.driver"
codesign -dv "$ROOT/build/$driverName.driver" 2>&1 | grep -E "Identifier|Signature|Authority" || true
