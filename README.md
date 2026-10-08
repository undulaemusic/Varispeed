# Varispeed

### THIS IS 100% VIBE CODED. USE AT YOUR OWN RISK. I DIDN'T EVEN LOOK AT THE CODE.

**Tape-style varispeed for your whole DAW on macOS.** Slow down or speed up everything your DAW plays (playhead, automation, synths, plugins, effect tails) with the pitch following the speed, like a tape machine. It's like Logic Pro's Varispeed, but for Ableton Live or any other DAW.

- **50% to 200% speed** (−12 to +12 semitones), changed live while playing
- **Smooth, tape-like glides** between speeds, as quick as your DAW's buffer size allows without glitching
- **Records what you hear** to a WAV file at your project's sample rate, ready to drag back into your DAW
- **Works with any audio interface**, or your Mac's built-in speakers
- A small **menu bar app**; nothing to configure in your DAW beyond picking an output

## How it works

Your DAW doesn't watch a clock. It renders audio whenever its audio device asks for the next block. Varispeed is a virtual audio device whose clock deliberately runs **slower or faster than real time**. At 75%, it asks for audio 75% as often, so the entire DAW engine runs at 75% speed without knowing anything is different.

The Varispeed menu bar app then takes that slowed (or sped-up) stream and plays it on your real audio interface in real time, resampling it so the pitch follows the speed, just like tape.

## Requirements

- A Mac running **macOS 13 (Ventura) or newer**. Tested on Apple Silicon with macOS 26. Intel Macs should work but are untested.
- Apple's free **Command Line Tools**, used to build Varispeed on your Mac. If you don't have them, the installer tells you how to get them (one command: `xcode-select --install`). You do **not** need full Xcode.
- Your Mac's admin password, to install the audio driver.

## Install

In Terminal:

```bash
git clone https://github.com/undulaemusic/Varispeed.git
cd Varispeed
./install.sh
```

Downloaded the ZIP instead? Unzip it, then in Terminal type `cd ` (with a space), drag the unzipped folder onto the Terminal window, press Return, and run `bash install.sh`.

The installer:

1. Builds the driver and the app on your Mac (about a minute).
2. Installs the app to `/Applications` and the driver to `/Library/Audio/Plug-Ins/HAL/`. It asks for your password for the driver.
3. Restarts Core Audio once. **All audio on your Mac stops for a few seconds**, so pause anything that's playing.
4. Opens Varispeed. Look for the **dial icon in your menu bar**.

The first time it opens, macOS asks for **microphone access**. Click **Allow**. That's how the app listens to the Varispeed device; nothing is recorded unless you press Record.

The installer never changes your default audio devices or touches any other audio driver. Varispeed can't even become your Mac's default output.

## Set up your DAW

**Ableton Live:** Settings → Audio → set **Audio Output Device** to **Varispeed**. Set **Audio Input Device** to **No Device** (see [Limitations](#limitations)).

**Other DAWs:** choose **Varispeed** as the audio output device in the DAW's audio preferences.

Then, in the Varispeed menu (dial icon):

- Turn on **Output to device** and choose your audio interface and which outputs to use. It starts with your Mac's current default output.

Press play in your DAW and you should hear it as usual. At 100%, it sounds exactly like your DAW, just with about 20 ms of extra latency.

## Using it

| Control | What it does |
|---|---|
| **Speed slider** | 50% at the left, **100% in the middle**, 200% at the right. **Double-click** it to reset to 100%. |
| **Speed readouts** | **Double-click** the percentage or the semitone value to type an exact number, then press Return. |
| **−12 st / −1 st / +1 st / +12 st** | Bump the speed down or up by that many semitones from where it is now. |
| **100%** (or ⌘0) | Back to normal speed. |

## Recording

Press **Record**, play with the speed, press **Stop**. The latest take appears in the menu: **drag and drop it into your DAW**. Earlier takes stay in the recordings folder.

- Takes are 32-bit float stereo WAV files at **Varispeed's sample rate, i.e. your project's**, so the speed changes are baked in and the take plays back exactly as you heard it.
- They're saved to `Music/Varispeed Recordings` by default. Use **Change…** to pick another folder. Click the folder name to show it in Finder.

**Recording in other apps (OBS, etc.):** don't record your DAW's app audio or the Varispeed device's input. Those carry the audio *before* the speed change, so they sound normal-speed. Instead, record your audio interface's **loopback** input if it has one, or use Varispeed's own recorder.

## Limitations

- **Live input while varispeeding isn't supported.** Use Varispeed as the output device with no input device. Recording external audio into your DAW would need the input to run on Varispeed's clock too.
- **Your DAW's tempo display doesn't change**, and its time display falls behind (or ahead of) a real clock. That's expected: the DAW thinks it's playing normally.
- **MIDI clock and Ableton Link drift** from external gear while you're not at 100%.
- **Latency:** the passthrough to your interface adds about 20 ms at 100%, more at slow speeds (roughly 25–45 ms) because each buffer lasts longer in real time.
- **Buffer sizes** up to 256 samples. Every speed change glides as quickly as possible without glitching, which depends on the DAW's buffer size: at 128 and 256 an octave takes about 0.4 s down and 0.8 s up, at 64 twice as long, at 32 four times.
- Stereo only (2 channels). Sample rates 44.1, 48, 88.2 and 96 kHz.

## Uninstall

```bash
./uninstall.sh
```

This removes the driver, the app and its settings, then restarts Core Audio once. **Your recordings are never deleted.**

## Troubleshooting

- **The app says the driver isn't loaded / Varispeed isn't in my DAW's device list:** run `./install.sh` again, or restart your Mac.
- **No sound:** check that **Output to device** is on and the right device and outputs are chosen. Also check System Settings → Privacy & Security → **Microphone** and make sure **Varispeed** is allowed.
- **macOS audio acting strangely** (apps hanging when they start audio, high CPU from `coreaudiod`) after several installs or uninstalls: **restart your Mac.** Restarting Core Audio many times in a row can confuse macOS's audio system; a reboot clears it.

## For developers

```
driver/      Modified BlackHole (Core Audio HAL plug-in) with the variable-speed clock
bridge/      C engine: Varispeed input → lock-free ring → libsamplerate → output device; WAV recorder
app/         SwiftUI menu bar app (hosts the bridge, controls the driver)
tools/       Command-line tools: varispeedctl, vsbridge, vssweep, vstest, vstiming, vsaggtest
scripts/     build_driver.sh, build_app.sh, plus test and measurement scripts
third_party/ libsamplerate 0.2.2 (BSD)
```

Build everything with `scripts/build_driver.sh`, `scripts/build_app.sh` and `make` (tools). `build/varispeedctl 0.8` sets the speed from Terminal; `build/varispeedctl --watch` shows it live.

Things learned the hard way (details in the code comments):

- The driver keeps the **nominal sample rate unchanged** and stretches its zero-timestamp spacing by 1/speed. It reports `kAudioDevicePropertyClockAlgorithm = Raw`; with Core Audio's default smoothing, speed changes lag by seconds and top out around 1.33×.
- Core Audio estimates the rate from the previous zero-timestamp period, so every speed change is "unannounced". Changing speed too fast makes it skip ahead (speeding up) or stall, and occasionally abandon the device clock (slowing down). The driver therefore limits changes to 0.5 semitones per period up and 1 per period down, using glides that are linear in 1/speed (constant semitones per device frame).
- Core Audio caches `kAudioDevicePropertyZeroTimeStampPeriod`, so the period (1024 frames) is fixed at build time.
- Core Audio's view of the Varispeed timeline jolts slightly at every timestamp update, which is audible as ~36 Hz flutter during glides if you follow it directly. So the driver publishes its exact glide curve (custom property `'vsrp'`), and the bridge follows that curve, steering gently toward Core Audio's timeline. That brings glide roughness down from about 54 to about 0.5 cents rms.
- libsamplerate buffers input internally, so the ring buffer's read position isn't the play position. The bridge tracks the play position separately.

Driver controls are custom Core Audio properties on the Varispeed device (`driver/BlackHole/VarispeedProperties.h`): target speed, glide time, current speed and the glide curve.

## Credits & license

Varispeed is free software under the **GNU General Public License v3.0** (see [LICENSE](LICENSE)).

- The driver is a modified version of **[BlackHole](https://github.com/ExistentialAudio/BlackHole)** by Existential Audio Inc. (GPL-3.0). Thank you! Varispeed is **not affiliated with or endorsed by Existential Audio**, and doesn't use the BlackHole name, logo or branding. See [driver/LICENSE](driver/LICENSE).
- Resampling by **[libsamplerate](https://github.com/libsndfile/libsamplerate)** by Erik de Castro Lopo (BSD 2-Clause, see [third_party/libsamplerate/COPYING](third_party/libsamplerate/COPYING)).
