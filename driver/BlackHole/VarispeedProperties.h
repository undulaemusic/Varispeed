// Custom AudioObject properties exposed by the Varispeed device.
// All values are CFNumber (doubles) passed as CFPropertyListRef.
#ifndef VarispeedProperties_h
#define VarispeedProperties_h

// Target speed ratio s (settable). 1.0 = normal, 0.5 = half speed. Clamped to [min, max].
#define kVarispeedProperty_TargetSpeed   0x76737064 /* 'vspd' */
// Ramp time in seconds used to glide to a new target speed (settable). 0 = jump.
#define kVarispeedProperty_RampSeconds   0x76737274 /* 'vsrt' */
// Speed the clock is running at right now, mid-ramp included (read only).
#define kVarispeedProperty_CurrentSpeed  0x76736373 /* 'vscs' */
// The current ramp, so clients can compute the exact speed curve (read only, notifies on change):
// CFArray of 5 CFNumbers [fromSpeed, targetSpeed, rampStartHostTime, rampTicks, generation].
// s(t) = 1 / (1/from + (1/target - 1/from) * clamp((t - start) / ticks, 0, 1)); ticks <= 0 means s = target.
#define kVarispeedProperty_RampParameters 0x76737270 /* 'vsrp' */

// Development tuning (take effect the next time IO starts on the device):
// zero-timestamp period in frames, and the HAL clock algorithm ('raww', 'iirf', 'mavg').
#define kVarispeedProperty_DebugPeriod          0x76737a70 /* 'vszp' */
#define kVarispeedProperty_DebugClockAlgorithm  0x76736361 /* 'vsca' */
// Max semitones the speed may rise per zero-timestamp period (applies to the next change).
#define kVarispeedProperty_DebugMaxRise         0x76736d72 /* 'vsmr' */
// Max semitones the speed may fall per zero-timestamp period (applies to the next change).
#define kVarispeedProperty_DebugMaxFall         0x76736d66 /* 'vsmf' */

// Core Audio identities. Unique so Varispeed never collides with another device on the Mac.
// Changing these makes DAWs treat Varispeed as a new device (users re-select it once).
#define kVarispeed_DeviceUID             "com.undulaemusic.Varispeed.device"
#define kVarispeed_Device2UID            "com.undulaemusic.Varispeed.device2"   // hidden mirror device
#define kVarispeed_ModelUID              "com.undulaemusic.Varispeed.model"
#define kVarispeed_BoxUID                "com.undulaemusic.Varispeed.box"

// Smallest IO buffer (frames) any DAW-type client has used in the last ~2 s; 0 = none (read only).
// The Varispeed app's own passthrough is not counted.
#define kVarispeedProperty_ClientBufferFrames   0x76736362 /* 'vscb' */
// Current glide scale (read only): multiplies the per-period glide limits below.
#define kVarispeedProperty_GlideScale           0x76736773 /* 'vsgs' */

// Glide limits, shared with the app so it can show the quickest glide. With 1/s changing linearly
// in time, a glide from s1 to s2 takes at least |1/s1 - 1/s2| / (c * sampleRate) seconds, where
// c = ln(2)/12 * (max semitones per period) * glideScale / zeroTimeStampPeriod.
//
// The HAL learns a speed change one zero-timestamp period late; the resulting timing error grows
// with (semitones per period) x period and must stay below what the DAW's IO buffer can absorb.
// Calibrated at a 1024-frame period: busy DAWs at 128-frame buffers are clean at the limits
// below, smaller buffers need proportionally gentler glides. So:
//   glideScale = clamp((ReferencePeriod / period) * (clientBuffer / FullSpeedBufferFrames),
//                      MinGlideScale, period / ReferencePeriod)
// (upper bound: never faster per frame than at the reference). No DAW seen -> clientBuffer = 128.
//
// The HAL allows IO buffers up to about 3/8 of the zero-timestamp period: 1024 -> 384, so DAWs can
// use up to 256-frame buffers (1536 would allow 512 but makes every glide ~2x gentler at 128).
#define kVarispeed_ZeroTimeStampPeriod          1024
#define kVarispeed_ReferencePeriod              1024.0
#define kVarispeed_MaxRiseSemitonesPerPeriod    0.5
#define kVarispeed_MaxFallSemitonesPerPeriod    1.0
#define kVarispeed_MinRampSeconds               0.1
#define kVarispeed_FullSpeedBufferFrames        128.0
#define kVarispeed_MinGlideScale                (1.0 / 16.0)
#define kVarispeed_AppBundleID                  "com.undulaemusic.Varispeed"

#define kVarispeed_MinSpeed              0.5
#define kVarispeed_MaxSpeed              2.0
#define kVarispeed_MaxRampSeconds        30.0

#endif
