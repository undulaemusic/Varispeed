// vssweep: stress test for runtime speed changes.
// Runs an IOProc on Varispeed (tone out, record in) and, while it runs, sweeps the speed
// through a series of targets with different ramp times. Every IO cycle it checks that the
// input sample time continues exactly where the previous cycle ended (no jumps, gaps or
// backwards time) and once per second reports the measured speed vs. the driver's speed.
//
// Usage: vssweep [seconds-per-step=3] [zts-period-frames] [clock-algorithm: raww|iirf|mavg] [min-ramp-seconds]
#include "vsdevice.h"
#include <mach/mach_time.h>
#include <stdatomic.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>
#include <time.h>

static _Atomic uint64_t gFrames = 0, gCycles = 0, gDiscontinuities = 0, gBackwards = 0;
static _Atomic double gLastEnd = -1, gWorstJump = 0;
static double gPhase = 0, gRate = 48000;
// discontinuity log (written only by the IO thread, read after stop)
typedef struct { uint64_t host; double sampleTime, jump; UInt32 frames; } JumpEvent;
static JumpEvent gEvents[256]; static _Atomic int gNumEvents = 0;

static OSStatus ioProc(AudioObjectID dev, const AudioTimeStamp *now, const AudioBufferList *in, const AudioTimeStamp *inTime,
                       AudioBufferList *out, const AudioTimeStamp *outTime, void *ctx) {
    if (in && in->mNumberBuffers > 0) {
        UInt32 frames = in->mBuffers[0].mDataByteSize / (sizeof(float) * in->mBuffers[0].mNumberChannels);
        double last = atomic_load(&gLastEnd);
        if (last >= 0) {
            double jump = inTime->mSampleTime - last;
            if (fabs(jump) > 0.5) {
                atomic_fetch_add(&gDiscontinuities, 1);
                if (jump < 0) atomic_fetch_add(&gBackwards, 1);
                if (fabs(jump) > fabs(atomic_load(&gWorstJump))) atomic_store(&gWorstJump, jump);
                int n = atomic_load(&gNumEvents);
                if (n < 256) { gEvents[n] = (JumpEvent){ inTime->mHostTime, inTime->mSampleTime, jump, frames }; atomic_store(&gNumEvents, n + 1); }
            }
        }
        atomic_store(&gLastEnd, inTime->mSampleTime + frames);
        atomic_fetch_add(&gFrames, frames);
        atomic_fetch_add(&gCycles, 1);
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

static double gStart = 0;
static double now_s(void) {
    static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e9;
}

int main(int argc, char **argv) {
    double step = argc > 1 ? atof(argv[1]) : 3.0;
    AudioObjectID dev = vs_find_device_by_uid(kVarispeed_DeviceUID);
    if (!dev) { fprintf(stderr, "Varispeed not found\n"); return 1; }
    AudioObjectPropertyAddress ra = { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 sz = sizeof(gRate); AudioObjectGetPropertyData(dev, &ra, 0, NULL, &sz, &gRate);

    if (argc > 2) vs_set_double(dev, kVarispeedProperty_DebugPeriod, atof(argv[2]));
    if (argc > 3 && strlen(argv[3]) == 4) {
        const char *a = argv[3];
        vs_set_double(dev, kVarispeedProperty_DebugClockAlgorithm, (double)(((UInt32)a[0] << 24) | ((UInt32)a[1] << 16) | ((UInt32)a[2] << 8) | (UInt32)a[3]));
    }
    if (getenv("VS_MAXRISE")) vs_set_double(dev, kVarispeedProperty_DebugMaxRise, atof(getenv("VS_MAXRISE")));
    double minRamp = argc > 4 ? atof(argv[4]) : 0.0;
    double period = 0, algo = 0;
    vs_get_double(dev, kVarispeedProperty_DebugPeriod, &period);
    vs_get_double(dev, kVarispeedProperty_DebugClockAlgorithm, &algo);
    UInt32 ai = (UInt32)algo;
    UInt32 bufFrames = 0; sz = sizeof(bufFrames);
    AudioObjectPropertyAddress bfa = { kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    printf("buffer %u frames, ", (AudioObjectGetPropertyData(dev, &bfa, 0, NULL, &sz, &bufFrames), bufFrames));
    printf("zero-timestamp period %.0f frames, clock algorithm '%c%c%c%c', min ramp %.2fs, %.0f Hz\n", period,
           (char)(ai >> 24), (char)(ai >> 16), (char)(ai >> 8), (char)ai, minRamp, gRate);

    struct { double speed, ramp; } steps[] = {
        {1.0, 0}, {0.75, 0}, {2.0, 0}, {0.5, 0}, {1.0, 0},          // instant jumps across the range
        {0.5, 1.0}, {2.0, 2.0}, {0.75, 0.5}, {1.5, 0.1}, {1.0, 0.5},  // ramps
    };
    int nsteps = sizeof steps / sizeof steps[0];
    // VS_STEPS="0.5:0,2:0.5,..." overrides the built-in step list (speed:ramp pairs)
    if (getenv("VS_STEPS")) {
        nsteps = 0;
        for (char *tok = strtok(strdup(getenv("VS_STEPS")), ","); tok && nsteps < 10; tok = strtok(NULL, ","))
            sscanf(tok, "%lf:%lf", &steps[nsteps].speed, &steps[nsteps].ramp), nsteps++;
    }

    vs_set_double(dev, kVarispeedProperty_RampSeconds, 0);
    vs_set_double(dev, kVarispeedProperty_TargetSpeed, 1.0);

    if (getenv("VS_BUFFER")) {
        UInt32 frames = (UInt32)atoi(getenv("VS_BUFFER"));
        AudioObjectPropertyAddress ba = { kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        AudioObjectSetPropertyData(dev, &ba, 0, NULL, sizeof(frames), &frames);
    }
    if (getenv("VS_RATE")) {
        Float64 r = atof(getenv("VS_RATE"));
        AudioObjectSetPropertyData(dev, &ra, 0, NULL, sizeof(r), &r);
        sleep(1);
        sz = sizeof(gRate); AudioObjectGetPropertyData(dev, &ra, 0, NULL, &sz, &gRate);
    }
    AudioDeviceIOProcID pid;
    if (AudioDeviceCreateIOProcID(dev, ioProc, NULL, &pid) != noErr) { fprintf(stderr, "IOProc failed\n"); return 1; }
    AudioDeviceStart(dev, pid);
    sleep(1);
    atomic_store(&gDiscontinuities, 0); atomic_store(&gBackwards, 0); atomic_store(&gWorstJump, 0);

    gStart = now_s();
    // VS_RANDOM=N: instead of the step list, change to a random speed (min-max range) with a random
    // ramp every 1-6 s for N seconds, printing one summary line per minute.
    if (getenv("VS_RANDOM")) {
        double total = atof(getenv("VS_RANDOM")), t0 = now_s(), nextChange = 0, nextReport = 60;
        srandom((unsigned)time(NULL));
        int changes = 0;
        while (now_s() - t0 < total) {
            double t = now_s() - t0;
            if (t >= nextChange) {
                double lo = log2(kVarispeed_MinSpeed), hi = log2(kVarispeed_MaxSpeed);
                double sp = pow(2.0, lo + (hi - lo) * (random() / (double)RAND_MAX));
                double rp = (random() % 4 == 0) ? 0.0 : 3.0 * (random() / (double)RAND_MAX);
                vs_set_double(dev, kVarispeedProperty_RampSeconds, rp);
                vs_set_double(dev, kVarispeedProperty_TargetSpeed, sp);
                nextChange = t + 1.0 + 5.0 * (random() / (double)RAND_MAX);
                changes++;
            }
            if (t >= nextReport) {
                printf("%4.0f min: %d speed changes, discontinuities %llu\n", t / 60, changes, (unsigned long long)atomic_load(&gDiscontinuities));
                fflush(stdout);
                nextReport += 60;
            }
            usleep(50000);
        }
        steps[0].speed = 1.0; steps[0].ramp = 0.5; nsteps = 1; step = 3;
    }
    printf("%-6s %-6s %-5s | %-9s %-9s %-9s\n", "target", "ramp", "t", "driver", "measured", "discont");
    for (int s = 0; s < nsteps; s++) {
        vs_set_double(dev, kVarispeedProperty_RampSeconds, fmax(steps[s].ramp, minRamp));
        vs_set_double(dev, kVarispeedProperty_TargetSpeed, steps[s].speed);
        double t0 = now_s();
        if (getenv("VS_EVENTS")) printf("-- step %d: target %.3f ramp %.2f at t=%.3fs\n", s, steps[s].speed, steps[s].ramp, t0 - gStart);
        uint64_t f0 = atomic_load(&gFrames); double tw = t0;
        while (now_s() - t0 < step) {
            usleep(500000);
            double t = now_s(); uint64_t f = atomic_load(&gFrames);
            double cur = 0; vs_get_double(dev, kVarispeedProperty_CurrentSpeed, &cur);
            printf("%-6.3f %-6.2f %-5.1f | %-9.4f %-9.4f %-9llu\n", steps[s].speed, steps[s].ramp, t - t0, cur,
                   (f - f0) / (gRate * (t - tw)), (unsigned long long)atomic_load(&gDiscontinuities));
            f0 = f; tw = t;
        }
    }
    AudioDeviceStop(dev, pid);
    AudioDeviceDestroyIOProcID(dev, pid);
    vs_set_double(dev, kVarispeedProperty_RampSeconds, 0.5);
    vs_set_double(dev, kVarispeedProperty_TargetSpeed, 1.0);
    if (getenv("VS_EVENTS")) {
        mach_timebase_info_data_t tb; mach_timebase_info(&tb);
        for (int i = 0; i < atomic_load(&gNumEvents); i++)
            printf("  jump %+.0f frames at sample %.0f (buffer %u) t=%.3fs\n", gEvents[i].jump, gEvents[i].sampleTime, gEvents[i].frames,
                   (double)gEvents[i].host * tb.numer / tb.denom / 1e9 - gStart);
    }
    printf("\nIO cycles: %llu  discontinuities: %llu  backwards: %llu  worst jump: %.1f frames\n",
           (unsigned long long)atomic_load(&gCycles), (unsigned long long)atomic_load(&gDiscontinuities),
           (unsigned long long)atomic_load(&gBackwards), atomic_load(&gWorstJump));
    return 0;
}
