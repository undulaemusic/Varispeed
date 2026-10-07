#!/bin/bash
# Varispeed installer: builds everything from source, installs the app to /Applications and the
# driver to /Library/Audio/Plug-Ins/HAL, then restarts Core Audio once.
# It never changes your default audio devices or touches any other audio driver.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
APP_SRC="$ROOT/build/Varispeed.app"
APP_DEST="/Applications/Varispeed.app"
DRV_SRC="$ROOT/build/Varispeed.driver"
DRV_DEST="/Library/Audio/Plug-Ins/HAL/Varispeed.driver"

# --- requirements
if [ "$(sw_vers -productVersion | cut -d. -f1)" -lt 13 ]; then
  echo "Varispeed needs macOS 13 (Ventura) or newer."; exit 1
fi
if ! xcode-select -p >/dev/null 2>&1 || ! command -v swiftc >/dev/null 2>&1; then
  echo "Apple's Command Line Tools are needed to build Varispeed."
  echo "Run this, follow the prompt, then run ./install.sh again:"
  echo "    xcode-select --install"
  exit 1
fi

# --- build
echo "==> Building the Varispeed driver..."
bash "$ROOT/scripts/build_driver.sh"
echo "==> Building the Varispeed app..."
bash "$ROOT/scripts/build_app.sh"

# --- install the app (quit a running copy first)
osascript -e 'quit app id "com.undulaemusic.Varispeed"' >/dev/null 2>&1 || true
sleep 1
echo "==> Installing the app to /Applications..."
if ! { rm -rf "$APP_DEST" && cp -R "$APP_SRC" "$APP_DEST"; } 2>/dev/null; then
  sudo rm -rf "$APP_DEST"; sudo cp -R "$APP_SRC" "$APP_DEST"
fi

# --- install the driver (needs your password) and restart Core Audio once
echo
echo "==> Installing the audio driver. macOS will ask for your password."
echo "    Then Core Audio restarts: ALL audio stops for a few seconds. Pause anything playing."
sudo rm -rf "$DRV_DEST"
sudo cp -R "$DRV_SRC" "$DRV_DEST"
sudo chown -R root:wheel "$DRV_DEST"
sudo killall coreaudiod 2>/dev/null || sudo launchctl kickstart -kp system/com.apple.audio.coreaudiod
sleep 3

echo
echo "==> Done. Opening Varispeed (look for the dial icon in your menu bar)."
echo "    macOS will ask for microphone access the first time: click Allow."
echo "    (That's how the app hears the Varispeed device. Nothing is recorded unless you press Record.)"
open "$APP_DEST"
