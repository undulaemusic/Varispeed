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

#define kVarispeed_MinSpeed              0.5
#define kVarispeed_MaxSpeed              2.0
#define kVarispeed_MaxRampSeconds        30.0

#endif
