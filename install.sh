#!/bin/bash
# Installs Varispeed.driver into the system HAL plug-in folder and restarts Core Audio.
# Touches ONLY /Library/Audio/Plug-Ins/HAL/Varispeed.driver. Never changes default devices.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
SRC="$ROOT/build/Varispeed.driver"
DEST="/Library/Audio/Plug-Ins/HAL/Varispeed.driver"

if [ ! -d "$SRC" ]; then
  echo "No build found. Building first..."
  "$ROOT/scripts/build_driver.sh"
fi

echo "Installing Varispeed driver (needs your Mac password)."
sudo rm -rf "$DEST"
sudo cp -R "$SRC" "$DEST"
sudo chown -R root:wheel "$DEST"

echo "Restarting Core Audio. All audio will cut out for a few seconds."
sudo killall coreaudiod 2>/dev/null || sudo launchctl kickstart -kp system/com.apple.audio.coreaudiod
sleep 3
echo "Done. Varispeed should now appear in Audio MIDI Setup."
