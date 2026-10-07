// vstiming: what does a Varispeed client (like Live) experience? Opens an IOProc on Varispeed
// (default 256-frame buffer) at a fixed speed and logs, per IO cycle, the gap since the previous
// cycle and the time left until that cycle's output deadline, both relative to the cycle's
// "fair share" of real time (buffer frames / (speed * rate)). Silent: writes zeros.
//
// Usage: vstiming [speed=1] [seconds=8] [buffer=256]
#include "vsdevice.h"
#include <mach/mach_time.h>
#include <stdatomic.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>

#define kMax 200000
static double gGap[kMax], gLead[kMax];
static _Atomic int gN = 0;
static uint64_t gLast = 0;
static double gTicksPerSec;

static OSStatus ioProc(AudioObjectID dev, const AudioTimeStamp *now, const AudioBufferList *in, const AudioTimeStamp *inTime,
                       AudioBufferList *out, const AudioTimeStamp *outTime, void *ctx) {
    uint64_t t = mach_absolute_time();
    int n = atomic_load(&gN);
    if (gLast && n < kMax) {
        gGap[n] = (double)(t - gLast) / gTicksPerSec;
        gLead[n] = ((double)outTime->mHostTime - (double)t) / gTicksPerSec;
        atomic_store(&gN, n + 1);
    }
    gLast = t;
    return noErr;
}

static int cmp(const void *a, const void *b) { double x = *(double *)a, y = *(double *)b; return x < y ? -1 : x > y; }

int main(int argc, char **argv) {
    double speed = argc > 1 ? atof(argv[1]) : 1, secs = argc > 2 ? atof(argv[2]) : 8;
    UInt32 buf = argc > 3 ? (UInt32)atoi(argv[3]) : 256;
    mach_timebase_info_data_t tb; mach_timebase_info(&tb); gTicksPerSec = 1e9 * tb.denom / tb.numer;
    AudioObjectID dev = vs_find_device_by_uid(kVarispeed_DeviceUID);
    if (!dev) return 1;
    vs_set_double(dev, kVarispeedProperty_RampSeconds, 0.1);
    vs_set_double(dev, kVarispeedProperty_TargetSpeed, speed);
    AudioObjectPropertyAddress ba = { kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectSetPropertyData(dev, &ba, 0, NULL, sizeof(buf), &buf);
    Float64 rate = 0; UInt32 sz = sizeof(rate);
    AudioObjectPropertyAddress ra = { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectGetPropertyData(dev, &ra, 0, NULL, &sz, &rate);
    AudioDeviceIOProcID pid;
    AudioDeviceCreateIOProcID(dev, ioProc, NULL, &pid);
    AudioDeviceStart(dev, pid);
    sleep(3); atomic_store(&gN, 0);           // skip the ramp
    usleep((useconds_t)(secs * 1e6));
    AudioDeviceStop(dev, pid);
    int n = atomic_load(&gN);
    double fair = buf / (speed * rate);
    double lo = 1e9, sumG = 0; int bursts = 0, late = 0;
    for (int i = 0; i < n; i++) {
        sumG += gGap[i];
        if (gGap[i] < 0.25 * fair) bursts++;
        if (gLead[i] < 0.5 * fair) late++;
        if (gLead[i] < lo) lo = gLead[i];
        gGap[i] /= fair;                         // normalise for percentiles
    }
    qsort(gGap, n, sizeof(double), cmp);
    printf("speed %4.2f: %5d cycles, fair share %5.2f ms | gap p1 %4.2f p50 %4.2f p99 %4.2f (x fair) | back-to-back %4.1f%% | <half time left %4.1f%% | min time left %5.2f ms\n",
           speed, n, fair * 1000, gGap[n / 100], gGap[n / 2], gGap[n * 99 / 100], 100.0 * bursts / n, 100.0 * late / n, lo * 1000);
    vs_set_double(dev, kVarispeedProperty_RampSeconds, 0.5);
    vs_set_double(dev, kVarispeedProperty_TargetSpeed, 1.0);
    return 0;
}
