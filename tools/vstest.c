// vstest: measures how fast the Varispeed device's clock runs against the wall clock.
// Plays a 440 Hz tone into Varispeed's output and records its input for N wall-clock
// seconds, then reports frames received vs. what a normal-speed device would deliver.
//
// Usage: vstest [seconds=10] [device name=Varispeed]
#include <CoreAudio/CoreAudio.h>
#include <mach/mach_time.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

static _Atomic uint64_t gFramesIn = 0;
static _Atomic uint64_t gFramesNonSilent = 0;
static double gPhase = 0, gRate = 48000;
static _Atomic double gFirstInSampleTime = -1, gLastInSampleTime = -1;

static AudioObjectID findDevice(const char *name) {
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &size);
    AudioObjectID *ids = malloc(size);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, ids);
    AudioObjectID found = 0;
    for (UInt32 i = 0; i < size / sizeof(AudioObjectID); i++) {
        CFStringRef s = NULL; UInt32 ss = sizeof(s);
        AudioObjectPropertyAddress na = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        if (AudioObjectGetPropertyData(ids[i], &na, 0, NULL, &ss, &s) == noErr && s) {
            char buf[256]; CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8); CFRelease(s);
            if (strcmp(buf, name) == 0) { found = ids[i]; break; }
        }
    }
    free(ids);
    return found;
}

static OSStatus ioProc(AudioObjectID dev, const AudioTimeStamp *now, const AudioBufferList *in, const AudioTimeStamp *inTime,
                       AudioBufferList *out, const AudioTimeStamp *outTime, void *ctx) {
    if (in && in->mNumberBuffers > 0) {
        const AudioBuffer *b = &in->mBuffers[0];
        UInt32 frames = b->mDataByteSize / (sizeof(float) * b->mNumberChannels);
        atomic_fetch_add(&gFramesIn, frames);
        const float *d = b->mData;
        for (UInt32 i = 0; i < frames; i++) if (fabsf(d[i * b->mNumberChannels]) > 1e-4f) { atomic_fetch_add(&gFramesNonSilent, frames); break; }
        if (atomic_load(&gFirstInSampleTime) < 0) atomic_store(&gFirstInSampleTime, inTime->mSampleTime);
        atomic_store(&gLastInSampleTime, inTime->mSampleTime + frames);
    }
    if (out) for (UInt32 n = 0; n < out->mNumberBuffers; n++) {
        AudioBuffer *b = &out->mBuffers[n];
        UInt32 ch = b->mNumberChannels, frames = b->mDataByteSize / (sizeof(float) * ch);
        float *d = b->mData;
        for (UInt32 i = 0; i < frames; i++) {
            float v = 0.1f * sinf((float)gPhase);
            gPhase += 2 * M_PI * 440.0 / gRate; if (gPhase > 2 * M_PI) gPhase -= 2 * M_PI;
            for (UInt32 c = 0; c < ch; c++) d[i * ch + c] = v;
        }
    }
    return noErr;
}

int main(int argc, char **argv) {
    double seconds = argc > 1 ? atof(argv[1]) : 10.0;
    const char *name = argc > 2 ? argv[2] : "Varispeed";
    AudioObjectID dev = findDevice(name);
    if (!dev) { fprintf(stderr, "Device '%s' not found\n", name); return 1; }

    AudioObjectPropertyAddress ra = { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 sz = sizeof(gRate);
    AudioObjectGetPropertyData(dev, &ra, 0, NULL, &sz, &gRate);

    AudioDeviceIOProcID pid;
    if (AudioDeviceCreateIOProcID(dev, ioProc, NULL, &pid) != noErr) { fprintf(stderr, "IOProc create failed\n"); return 1; }
    AudioDeviceStart(dev, pid);
    sleep(1); // let the clock settle
    uint64_t f0 = atomic_load(&gFramesIn);
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    uint64_t t0 = mach_absolute_time();
    usleep((useconds_t)(seconds * 1e6));
    uint64_t f1 = atomic_load(&gFramesIn);
    double elapsed = (double)(mach_absolute_time() - t0) * tb.numer / tb.denom / 1e9;
    AudioDeviceStop(dev, pid);
    AudioDeviceDestroyIOProcID(dev, pid);

    double got = (double)(f1 - f0), normal = gRate * elapsed;
    printf("Device:            %s @ %.0f Hz nominal\n", name, gRate);
    printf("Wall-clock time:   %.3f s\n", elapsed);
    printf("Frames received:   %.0f\n", got);
    printf("Normal-speed would be: %.0f\n", normal);
    printf("Measured speed:    %.4f  (%.2f%%)\n", got / normal, 100.0 * got / normal);
    printf("Signal looped back: %s\n", atomic_load(&gFramesNonSilent) > 0 ? "yes" : "NO (silence)");
    return 0;
}
