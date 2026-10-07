// vsaggtest: Milestone 4. Can a Core Audio aggregate device with drift correction bridge
// Varispeed to a real output? Creates a *private* aggregate (exists only while this runs, never
// saved): Varispeed = clock master, the default output device = sub-device with drift
// compensation. Plays a quiet 440 Hz tone (with a click-free pulse each second) into its outputs 1-2,
// steps through speeds, and counts overloads. At speed s you should hear 440*s Hz.
//
// Usage: vsaggtest [seconds-per-speed=8] [speeds...]   (default speeds: 1 0.98 0.9 0.75 0.5)
#include "vsdevice.h"
#include <CoreAudio/AudioHardware.h>
#include <stdatomic.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>

static double gPhase = 0, gRate = 44100, gPulse = 0;
static _Atomic uint64_t gOverloads = 0, gCycles = 0;
static UInt32 gOutputFirstChannel = 2;   // Varispeed's 2 output channels come first in the aggregate
static int gSilent = 0;

static OSStatus ioProc(AudioObjectID dev, const AudioTimeStamp *now, const AudioBufferList *in, const AudioTimeStamp *inTime,
                       AudioBufferList *out, const AudioTimeStamp *outTime, void *ctx) {
    atomic_fetch_add(&gCycles, 1);
    UInt32 chanBase = 0;
    for (UInt32 n = 0; n < out->mNumberBuffers; n++) {
        AudioBuffer *b = &out->mBuffers[n];
        UInt32 ch = b->mNumberChannels, frames = b->mDataByteSize / (sizeof(float) * ch);
        float *d = b->mData;
        memset(d, 0, b->mDataByteSize);
        if (n == out->mNumberBuffers - 1 || chanBase >= gOutputFirstChannel) {   // output device stream
            double phase = gPhase, pulse = gPulse;
            for (UInt32 i = 0; i < frames; i++) {
                double env = 0.6 + 0.4 * cos(2 * M_PI * pulse);              // 1 Hz soft pulse
                float v = gSilent ? 0.0f : (float)(0.05 * env * sin(phase));   // about -26 dBFS
                phase += 2 * M_PI * 440.0 / gRate; if (phase > 2 * M_PI) phase -= 2 * M_PI;
                pulse += 1.0 / gRate; if (pulse >= 1) pulse -= 1;
                d[i * ch] = v;                                                // outputs 1-2
                if (ch >= 2) d[i * ch + 1] = v;
            }
            gPhase = phase; gPulse = pulse;
        }
        chanBase += ch;
    }
    return noErr;
}

static OSStatus overloadListener(AudioObjectID obj, UInt32 n, const AudioObjectPropertyAddress *a, void *ctx) {
    atomic_fetch_add(&gOverloads, 1);
    return noErr;
}

// The Mac's current default output device (read only; never changed).
static AudioObjectID findOutput(char *uidOut, size_t len) {
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectID dev = 0;
    UInt32 sz = sizeof(dev);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, &dev) || !dev) return 0;
    CFStringRef uid = NULL; sz = sizeof(uid);
    AudioObjectPropertyAddress ua = { kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    if (AudioObjectGetPropertyData(dev, &ua, 0, NULL, &sz, &uid) || !uid) return 0;
    CFStringGetCString(uid, uidOut, len, kCFStringEncodingUTF8); CFRelease(uid);
    return dev;
}

int main(int argc, char **argv) {
    double step = argc > 1 ? atof(argv[1]) : 8.0;
    gSilent = getenv("VS_SILENT") != NULL;
    double defaults[] = { 1.0, 0.98, 0.9, 0.75, 0.5 };
    int nspeeds = argc > 2 ? argc - 2 : 5;

    AudioObjectID vs = vs_find_device_by_uid(kVarispeed_DeviceUID);
    char outUID[256];
    AudioObjectID output = findOutput(outUID, sizeof outUID);
    if (!vs || !output) { fprintf(stderr, "Need the Varispeed driver and a default output device\n"); return 1; }

    // Match Varispeed's nominal rate to the output's (we never change the output device's rate).
    AudioObjectPropertyAddress ra = { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 sz = sizeof(gRate);
    AudioObjectGetPropertyData(output, &ra, 0, NULL, &sz, &gRate);
    AudioObjectSetPropertyData(vs, &ra, 0, NULL, sizeof(gRate), &gRate);
    sleep(1);

    // Private aggregate: Varispeed is the clock; the output is drift-compensated at max quality.
    CFStringRef vsUID = CFSTR(kVarispeed_DeviceUID);
    CFStringRef mUID = CFStringCreateWithCString(NULL, outUID, kCFStringEncodingUTF8);
    int one = 1, quality = kAudioAggregateDriftCompensationMaxQuality;
    CFNumberRef cfOne = CFNumberCreate(NULL, kCFNumberIntType, &one), cfQ = CFNumberCreate(NULL, kCFNumberIntType, &quality);
    const void *vsKeys[] = { CFSTR(kAudioSubDeviceUIDKey) }, *vsVals[] = { vsUID };
    const void *mKeys[] = { CFSTR(kAudioSubDeviceUIDKey), CFSTR(kAudioSubDeviceDriftCompensationKey), CFSTR(kAudioSubDeviceDriftCompensationQualityKey) };
    const void *mVals[] = { mUID, cfOne, cfQ };
    CFDictionaryRef subVs = CFDictionaryCreate(NULL, vsKeys, vsVals, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionaryRef subM = CFDictionaryCreate(NULL, mKeys, mVals, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const void *subs[] = { subVs, subM };
    CFArrayRef subList = CFArrayCreate(NULL, subs, 2, &kCFTypeArrayCallBacks);
    const void *keys[] = { CFSTR(kAudioAggregateDeviceNameKey), CFSTR(kAudioAggregateDeviceUIDKey), CFSTR(kAudioAggregateDeviceSubDeviceListKey),
                           CFSTR(kAudioAggregateDeviceMainSubDeviceKey), CFSTR(kAudioAggregateDeviceIsPrivateKey) };
    const void *vals[] = { CFSTR("Varispeed Aggregate Test"), CFSTR("Varispeed_AggregateTest_UID"), subList, vsUID, cfOne };
    CFDictionaryRef desc = CFDictionaryCreate(NULL, keys, vals, 5, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    AudioObjectID agg = 0;
    OSStatus err = AudioHardwareCreateAggregateDevice(desc, &agg);
    if (err || !agg) { fprintf(stderr, "Could not create aggregate (%d)\n", (int)err); return 1; }
    sleep(1);

    AudioObjectPropertyAddress oa = { kAudioDeviceProcessorOverload, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectAddPropertyListener(agg, &oa, overloadListener, NULL);
    AudioDeviceIOProcID pid;
    AudioDeviceCreateIOProcID(agg, ioProc, NULL, &pid);
    vs_set_double(vs, kVarispeedProperty_RampSeconds, 0.5);
    vs_set_double(vs, kVarispeedProperty_TargetSpeed, 1.0);
    AudioDeviceStart(agg, pid);
    printf("Playing a quiet tone through the default output (channels 1-2) at %.0f Hz sample rate.\n", gRate);

    for (int i = 0; i < nspeeds; i++) {
        double s = argc > 2 ? atof(argv[i + 2]) : defaults[i];
        vs_set_double(vs, kVarispeedProperty_TargetSpeed, s);
        uint64_t o0 = atomic_load(&gOverloads);
        printf("speed %5.1f%%  (tone should be %5.1f Hz, %+.2f semitones) ... ", s * 100, 440 * s, 12 * log2(s));
        fflush(stdout);
        sleep((unsigned)step);
        printf("overloads: %llu\n", (unsigned long long)(atomic_load(&gOverloads) - o0));
    }

    AudioDeviceStop(agg, pid);
    AudioDeviceDestroyIOProcID(agg, pid);
    AudioHardwareDestroyAggregateDevice(agg);
    vs_set_double(vs, kVarispeedProperty_TargetSpeed, 1.0);
    printf("Aggregate removed. Total IO cycles %llu, overloads %llu\n", (unsigned long long)atomic_load(&gCycles), (unsigned long long)atomic_load(&gOverloads));
    return 0;
}
