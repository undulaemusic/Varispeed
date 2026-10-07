// VSBridge: see VSBridge.h for the overview.
//
// Realtime rules for the two IOProcs: no allocation, no locks, no Objective-C, no logging.
// They talk to each other and to the control thread only through C11 atomics.

#include "VSBridge.h"
#include "VSRecorder.h"
#include "../third_party/libsamplerate/samplerate.h"

#include <CoreAudio/CoreAudio.h>
#include <mach/mach_time.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define kVarispeedUID          "Varispeed_UID"
#define kRingFrames            (1u << 16)          // power of two; ~0.3 s even at 2x / 96 kHz
#define kRingMask              (kRingFrames - 1)
#define kScratchFrames         16384
#define kChannels              2

// Timeline: every input callback records (host time of its first frame, ring position).
// The output plays the ring at position P(t - D): the input timeline delayed by D seconds.
#define kHistory               2048                 // power of two; >= 0.6 s of 64-frame callbacks at 2x/96 kHz
#define kHistoryMask           (kHistory - 1)
#define kPhaseGain             0.05                 // fraction of position error corrected per output cycle
#define kMaxPhaseCorrection    0.01                 // ... but never more than 1 % of the cycle's frames
#define kResyncSeconds         0.03                 // position error beyond this -> jump instead of correcting
#define kInitialDelay          0.015
#define kMinDelay              0.002
#define kMaxDelay              0.2
#define kLowSlackSeconds       0.001                // spare input below this -> lengthen D a little
#define kDelayStepUp           0.001
#define kDelayDecayPerSecond   0.001                // shorten D this fast while there's plenty of slack
#define kDecaySlackSeconds     0.003
#define kSlackWindowSeconds    2.0
#define kMaxDelayStep          0.005                // one underrun raises D by at most this
#define kChunkFloorFactor      3.0                  // D >= this many input buffers (in real time at the current speed)
#define kBigSlackSeconds       0.03                 // more spare input than this -> cut D in one step
#define kInputPausedSeconds    0.1                  // no input for this long -> output silence, don't adapt

struct VSBridge {
    VSBridgeConfig config;
    char outputUID[256];

    AudioObjectID inDevice, outDevice;
    AudioDeviceIOProcID inProc, outProc;
    bool started;

    // ring buffer (single producer: input IOProc, single consumer: output IOProc)
    float *ring;
    _Atomic uint64_t writePos, readPos;

    // input side -> output side
    _Atomic double inNominalRate, outNominalRate;
    _Atomic double inRateScalar;                    // HAL rate scalar of Varispeed (= 1/speed)
    uint64_t histHost[kHistory], histPos[kHistory]; // written by input thread, published by histCount
    _Atomic uint64_t histCount;
    uint64_t histFloor;                             // output thread ignores entries before this (set on reset)
    _Atomic bool resetRequested;

    // output-thread state
    SRC_STATE *src;
    float *srcIn, *srcOut;
    bool primed;
    double playPos;                                 // input frames the resampler has been told to consume (exact,
                                                    // unlike readPos, which runs ahead by libsamplerate's buffering)
    double delay;                                   // D, seconds
    double minSlack, slackWindowStart;              // smallest spare input (s) seen in this window
    double lastStepUpTime, clock;                   // output-thread time in seconds (sum of cycles)
    double hostTicksPerSecond;
    float glitchX1, glitchX2, glitchPeak;
    int glitchWarmup;
    float inGlitchX1, inGlitchX2, inGlitchPeak;   // same detector on the raw Varispeed input (input thread)
    int inGlitchWarmup;
    double resamplerDelayInputFrames;

    // stats (written by IO threads, read by anyone)
    _Atomic double statInRealRate, statOutRealRate, statFillSeconds, statTargetSeconds, statCorrection, statCpu, statSpeed;
    _Atomic uint64_t underruns, overflows, glitches, inputGlitches, resets, resyncs, inputCycles, outputCycles;

    // output channel mapping, fixed at start
    struct { UInt32 buffer, channel, stride; bool valid; } outMap[kChannels];
    double outputDeviceLatencySeconds;

    const char *_Atomic lastError;
    VSRecorder *_Atomic recorder;
};

#pragma mark - Helpers

static double HostTicksPerSecond(void) {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    return 1e9 * (double)tb.denom / (double)tb.numer;
}

static AudioObjectID DeviceForUID(const char *uid) {
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFStringRef cf = CFStringCreateWithCString(NULL, uid, kCFStringEncodingUTF8);
    AudioObjectID dev = kAudioObjectUnknown;
    UInt32 size = sizeof(dev);
    OSStatus err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, sizeof(cf), &cf, &size, &dev);
    CFRelease(cf);
    return err == noErr ? dev : kAudioObjectUnknown;
}

static bool CopyStringProperty(AudioObjectID dev, AudioObjectPropertySelector sel, char *out, size_t len) {
    AudioObjectPropertyAddress a = { sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFStringRef s = NULL;
    UInt32 size = sizeof(s);
    if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, &s) != noErr || !s) return false;
    bool ok = CFStringGetCString(s, out, (CFIndex)len, kCFStringEncodingUTF8);
    CFRelease(s);
    return ok;
}

static AudioObjectID FindDeviceByNameHint(const char *hint, char *uidOut, size_t uidLen) {
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &size) != noErr) return kAudioObjectUnknown;
    AudioObjectID *ids = malloc(size);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, ids);
    AudioObjectID found = kAudioObjectUnknown;
    for (UInt32 i = 0; i < size / sizeof(AudioObjectID) && !found; i++) {
        char name[256];
        if (CopyStringProperty(ids[i], kAudioObjectPropertyName, name, sizeof name) && strstr(name, hint)
            && CopyStringProperty(ids[i], kAudioDevicePropertyDeviceUID, uidOut, uidLen)) {
            found = ids[i];
        }
    }
    free(ids);
    return found;
}

static double NominalRate(AudioObjectID dev) {
    AudioObjectPropertyAddress a = { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    Float64 rate = 0;
    UInt32 size = sizeof(rate);
    AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, &rate);
    return rate;
}

static UInt32 UInt32Property(AudioObjectID dev, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope) {
    AudioObjectPropertyAddress a = { sel, scope, kAudioObjectPropertyElementMain };
    UInt32 v = 0, size = sizeof(v);
    AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, &v);
    return v;
}

static void SetBufferFrames(AudioObjectID dev, UInt32 frames) {
    if (!frames) return;
    AudioObjectPropertyAddress a = { kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectSetPropertyData(dev, &a, 0, NULL, sizeof(frames), &frames);
}

// Turn off the streams an IOProc doesn't use, so the HAL doesn't do work for them.
static void SetStreamUsage(AudioObjectID dev, AudioDeviceIOProcID proc, AudioObjectPropertyScope scope, bool on) {
    AudioObjectPropertyAddress a = { kAudioDevicePropertyIOProcStreamUsage, scope, kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(dev, &a, 0, NULL, &size) != noErr || size < sizeof(AudioHardwareIOProcStreamUsage)) return;
    AudioHardwareIOProcStreamUsage *usage = calloc(1, size);
    usage->mIOProc = (void *)proc;
    if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, usage) == noErr) {
        for (UInt32 i = 0; i < usage->mNumberStreams; i++) usage->mStreamIsOn[i] = on;
        AudioObjectSetPropertyData(dev, &a, 0, NULL, size, usage);
    }
    free(usage);
}

static int SRCType(VSBridgeQuality q) {
    switch (q) {
        case VSBridgeQualityMedium: return SRC_SINC_MEDIUM_QUALITY;
        case VSBridgeQualityFast:   return SRC_SINC_FASTEST;
        default:                    return SRC_SINC_BEST_QUALITY;
    }
}

// Approximate group delay of libsamplerate's sinc filters, in input frames: one side of the
// symmetric filter (coefficient table length / oversampling increment, from its *_coeffs.h).
static double SRCDelayFrames(VSBridgeQuality q) {
    switch (q) {
        case VSBridgeQualityMedium: return 22438.0 / 491.0;
        case VSBridgeQualityFast:   return 2464.0 / 128.0;
        default:                    return 340239.0 / 2381.0;
    }
}

#pragma mark - Realtime IOProcs

// Test-tone glitch detector: for a sine of amplitude A and angular frequency w (rad/sample), the
// second difference never exceeds A*w^2. Missing / repeated samples produce much larger spikes.
// Only meaningful while a pure tone of <= 880 Hz is playing (used by the automated tests).
typedef struct { float *x1, *x2, *peak; int *warmup; _Atomic uint64_t *count; } GlitchState;

static void DetectGlitches(GlitchState g, const float *x, UInt32 frames, UInt32 stride, double rate) {
    double wmax = 2.0 * M_PI * 1000.0 / rate;
    for (UInt32 i = 0; i < frames; i++) {
        float v = x[i * stride];
        float a = fabsf(v);
        *g.peak = a > *g.peak ? a : *g.peak * 0.99999f;
        float d2 = v - 2.0f * *g.x1 + *g.x2;
        if (*g.warmup > 0) (*g.warmup)--;
        else if (*g.peak > 0.01f && fabsf(d2) > 4.0 * *g.peak * wmax * wmax + 1e-4) {
            atomic_fetch_add_explicit(g.count, 1, memory_order_relaxed);
            *g.warmup = 64;   // count one event, not every sample of it
        }
        *g.x2 = *g.x1;
        *g.x1 = v;
    }
}

static OSStatus InputIOProc(AudioObjectID dev, const AudioTimeStamp *now, const AudioBufferList *in, const AudioTimeStamp *inTime,
                            AudioBufferList *out, const AudioTimeStamp *outTime, void *ctx) {
    VSBridge *b = ctx;
    if (!in || in->mNumberBuffers == 0) return noErr;
    const AudioBuffer *buf = &in->mBuffers[0];
    UInt32 ch = buf->mNumberChannels ? buf->mNumberChannels : 1;
    UInt32 frames = buf->mDataByteSize / (UInt32)(sizeof(float) * ch);
    const float *src = buf->mData;

    if (inTime->mFlags & kAudioTimeStampRateScalarValid && inTime->mRateScalar > 0)
        atomic_store_explicit(&b->inRateScalar, inTime->mRateScalar, memory_order_relaxed);

    uint64_t w = atomic_load_explicit(&b->writePos, memory_order_relaxed);
    uint64_t r = atomic_load_explicit(&b->readPos, memory_order_acquire);
    if (kRingFrames - (w - r) < frames) {
        atomic_fetch_add_explicit(&b->overflows, 1, memory_order_relaxed);
    } else {
        for (UInt32 i = 0; i < frames; i++) {
            float *dst = &b->ring[((w + i) & kRingMask) * kChannels];
            dst[0] = src[i * ch];
            dst[1] = src[i * ch + (ch > 1 ? 1 : 0)];
        }
        // record where this buffer sits on the Varispeed timeline, then publish the frames
        if (inTime->mFlags & kAudioTimeStampHostTimeValid) {
            uint64_t n = atomic_load_explicit(&b->histCount, memory_order_relaxed);
            b->histHost[n & kHistoryMask] = inTime->mHostTime;
            b->histPos[n & kHistoryMask] = w;
            atomic_store_explicit(&b->histCount, n + 1, memory_order_release);
        }
        atomic_store_explicit(&b->writePos, w + frames, memory_order_release);
    }
    DetectGlitches((GlitchState){ &b->inGlitchX1, &b->inGlitchX2, &b->inGlitchPeak, &b->inGlitchWarmup, &b->inputGlitches },
                   src, frames, ch, atomic_load_explicit(&b->inNominalRate, memory_order_relaxed));
    atomic_fetch_add_explicit(&b->inputCycles, 1, memory_order_relaxed);
    return noErr;
}

// Ring position that the Varispeed timeline had reached at host time t (interpolated between
// input callbacks, extrapolated past the newest one). False if t is older than the history.
static bool PositionAt(VSBridge *b, double t, double inRate, double *outPos) {
    uint64_t n = atomic_load_explicit(&b->histCount, memory_order_acquire);
    if (n < 2) return false;
    if (n < b->histFloor + 2) return false;
    uint64_t newest = n - 1, oldest = n > kHistory - 16 ? n - (kHistory - 16) : 0;
    if (oldest < b->histFloor) oldest = b->histFloor;
    double tNew = (double)b->histHost[newest & kHistoryMask];
    if (t >= tNew) {
        *outPos = (double)b->histPos[newest & kHistoryMask] + (t - tNew) / b->hostTicksPerSecond * inRate;
        return true;
    }
    if (t < (double)b->histHost[oldest & kHistoryMask]) return false;
    uint64_t lo = oldest, hi = newest;               // invariant: host[lo] <= t < host[hi]
    while (hi - lo > 1) {
        uint64_t mid = lo + (hi - lo) / 2;
        if ((double)b->histHost[mid & kHistoryMask] <= t) lo = mid; else hi = mid;
    }
    double h0 = (double)b->histHost[lo & kHistoryMask], h1 = (double)b->histHost[hi & kHistoryMask];
    double p0 = (double)b->histPos[lo & kHistoryMask], p1 = (double)b->histPos[hi & kHistoryMask];
    *outPos = h1 > h0 ? p0 + (p1 - p0) * (t - h0) / (h1 - h0) : p0;
    return true;
}

static void Reset(VSBridge *b) {
    src_reset(b->src);
    b->primed = false;
    b->histFloor = atomic_load_explicit(&b->histCount, memory_order_acquire);   // old timeline no longer valid
    b->delay = kInitialDelay;
    b->glitchWarmup = 4096;
    atomic_fetch_add_explicit(&b->resets, 1, memory_order_relaxed);
}

static OSStatus OutputIOProc(AudioObjectID dev, const AudioTimeStamp *now, const AudioBufferList *in, const AudioTimeStamp *inTime,
                             AudioBufferList *out, const AudioTimeStamp *outTime, void *ctx) {
    VSBridge *b = ctx;
    uint64_t t0Ticks = mach_absolute_time();
    if (!out || out->mNumberBuffers == 0) return noErr;
    UInt32 frames = out->mBuffers[0].mDataByteSize / (UInt32)(sizeof(float) * (out->mBuffers[0].mNumberChannels ? out->mBuffers[0].mNumberChannels : 1));
    if (frames > kScratchFrames) frames = kScratchFrames;
    atomic_fetch_add_explicit(&b->outputCycles, 1, memory_order_relaxed);

    if (atomic_exchange_explicit(&b->resetRequested, false, memory_order_acq_rel)) Reset(b);

    double inNominal = atomic_load_explicit(&b->inNominalRate, memory_order_relaxed);
    double outNominal = atomic_load_explicit(&b->outNominalRate, memory_order_relaxed);
    double inRate = inNominal / atomic_load_explicit(&b->inRateScalar, memory_order_relaxed);
    double outScalar = (outTime->mFlags & kAudioTimeStampRateScalarValid && outTime->mRateScalar > 0) ? outTime->mRateScalar : 1.0;
    double outRate = outNominal / outScalar;
    double dt = frames / outRate;
    b->clock += dt;

    // host times at which this output buffer starts and ends
    double tStart = (outTime->mFlags & kAudioTimeStampHostTimeValid) ? (double)outTime->mHostTime : (double)t0Ticks;
    double tEnd = tStart + dt * b->hostTicksPerSecond;
    double ticksD = b->delay * b->hostTicksPerSecond;

    float *res = b->srcOut;
    UInt32 produced = 0;
    double p0, p1;
    uint64_t w = atomic_load_explicit(&b->writePos, memory_order_acquire);
    uint64_t r = atomic_load_explicit(&b->readPos, memory_order_relaxed);

    // Input paused (device reconfiguring, or IO stopped)? Play silence and resync when it resumes,
    // rather than treating the gap as an underrun and growing D.
    uint64_t hc = atomic_load_explicit(&b->histCount, memory_order_acquire);
    bool inputPaused = hc <= b->histFloor
        || (double)t0Ticks - (double)b->histHost[(hc - 1) & kHistoryMask] > (kInputPausedSeconds + b->delay) * b->hostTicksPerSecond;
    if (inputPaused) b->primed = false;

    if (!inputPaused && PositionAt(b, tStart - ticksD, inRate, &p0) && PositionAt(b, tEnd - ticksD, inRate, &p1) && p1 > p0) {
        double lookahead = b->resamplerDelayInputFrames * fmax(1.0, (p1 - p0) / frames) + 8.0;

        // jump instead of correcting when far off (first cycle, after underruns, rate changes)
        double err = b->playPos - p0;                      // > 0: we're ahead of the timeline
        if (!b->primed || fabs(err) > kResyncSeconds * inRate) {
            double minPos = (double)w - (double)(kRingFrames - 8192);
            double target = fmax(p0, minPos);
            if (b->primed) atomic_fetch_add_explicit(&b->resyncs, 1, memory_order_relaxed);
            r = (uint64_t)llround(target);
            b->playPos = (double)r;
            err = 0;
            b->primed = true;
            src_reset(b->src);
            b->glitchWarmup = 4096;
        }

        // consume the timeline's span for this buffer, nudged to remove the position error
        double span = p1 - p0;
        double nudge = kPhaseGain * err;
        double maxNudge = kMaxPhaseCorrection * span;
        if (nudge > maxNudge) nudge = maxNudge;
        if (nudge < -maxNudge) nudge = -maxNudge;
        double inFrames = span - nudge;
        double ratio = frames / inFrames;
        if (ratio < 1.0 / 256.0) ratio = 1.0 / 256.0;
        if (ratio > 256.0) ratio = 256.0;
        atomic_store_explicit(&b->statCorrection, -nudge / span, memory_order_relaxed);

        // how much spare input is there beyond what this cycle needs?
        double slack = ((double)w - (b->playPos + inFrames + lookahead)) / inRate;
        b->playPos += inFrames;
        if (b->clock - b->slackWindowStart > kSlackWindowSeconds) {
            if (b->minSlack > kBigSlackSeconds) b->delay -= b->minSlack - kDecaySlackSeconds;   // resyncs once
            else if (b->minSlack > kDecaySlackSeconds) b->delay -= kDelayDecayPerSecond * kSlackWindowSeconds;
            b->minSlack = slack;
            b->slackWindowStart = b->clock;
        } else if (slack < b->minSlack) {
            b->minSlack = slack;
        }
        if (slack < kLowSlackSeconds && b->clock - b->lastStepUpTime > 0.05) {
            b->delay += fmin(kDelayStepUp - fmin(0.0, slack), kMaxDelayStep);   // an underrun also adds the shortfall
            b->lastStepUpTime = b->clock;
        }
        // Slower speed = each input buffer lasts longer in real time, so raise D ahead of time
        // instead of waiting for an underrun. The position correction absorbs the gradual change.
        double inBuf = b->config.inputBufferFrames ? b->config.inputBufferFrames : 512;
        double floor = kChunkFloorFactor * inBuf / inRate + lookahead / inRate;
        if (b->delay < floor) b->delay = floor;
        if (b->delay < kMinDelay) b->delay = kMinDelay;
        if (b->delay > kMaxDelay) b->delay = kMaxDelay;

        for (int guard = 0; produced < frames && guard < 16; guard++) {
            w = atomic_load_explicit(&b->writePos, memory_order_acquire);
            uint64_t avail = w > r ? w - r : 0;
            uint64_t want = (uint64_t)ceil((frames - produced) / ratio) + 8;
            uint64_t n = avail < want ? avail : want;
            if (n > kScratchFrames) n = kScratchFrames;
            for (uint64_t i = 0; i < n; i++) {
                const float *s = &b->ring[((r + i) & kRingMask) * kChannels];
                b->srcIn[i * kChannels] = s[0];
                b->srcIn[i * kChannels + 1] = s[1];
            }
            SRC_DATA d = {
                .data_in = b->srcIn, .data_out = res + produced * kChannels,
                .input_frames = (long)n, .output_frames = (long)(frames - produced),
                .end_of_input = 0, .src_ratio = ratio,
            };
            if (src_process(b->src, &d) != 0) break;
            r += (uint64_t)d.input_frames_used;
            produced += (UInt32)d.output_frames_gen;
            if (d.input_frames_used == 0 && d.output_frames_gen == 0) break;   // starved
        }
        atomic_store_explicit(&b->readPos, r, memory_order_release);
        if (produced < frames) atomic_fetch_add_explicit(&b->underruns, 1, memory_order_relaxed);
        atomic_store_explicit(&b->statSpeed, span / dt / inNominal, memory_order_relaxed);
    }
    if (produced < frames) memset(res + produced * kChannels, 0, (frames - produced) * kChannels * sizeof(float));

    DetectGlitches((GlitchState){ &b->glitchX1, &b->glitchX2, &b->glitchPeak, &b->glitchWarmup, &b->glitches }, res, frames, kChannels, outNominal);
    VSRecorderPush(atomic_load_explicit(&b->recorder, memory_order_acquire), res, frames);

    if (!b->config.muteOutput) {
        for (int c = 0; c < kChannels; c++) {
            if (!b->outMap[c].valid || b->outMap[c].buffer >= out->mNumberBuffers) continue;
            float *dst = out->mBuffers[b->outMap[c].buffer].mData;
            UInt32 stride = b->outMap[c].stride, off = b->outMap[c].channel;
            for (UInt32 i = 0; i < frames; i++) dst[i * stride + off] = res[i * kChannels + c];
        }
    }

    atomic_store_explicit(&b->statInRealRate, inRate, memory_order_relaxed);
    atomic_store_explicit(&b->statOutRealRate, outRate, memory_order_relaxed);
    atomic_store_explicit(&b->statFillSeconds, ((double)w - b->playPos) / inRate, memory_order_relaxed);
    atomic_store_explicit(&b->statTargetSeconds, b->delay, memory_order_relaxed);
    atomic_store_explicit(&b->statCpu, (double)(mach_absolute_time() - t0Ticks) / b->hostTicksPerSecond / dt, memory_order_relaxed);
    return noErr;
}

#pragma mark - Property listeners (HAL notification thread)

static OSStatus RateListener(AudioObjectID obj, UInt32 n, const AudioObjectPropertyAddress *addrs, void *ctx) {
    VSBridge *b = ctx;
    atomic_store(&b->inNominalRate, NominalRate(b->inDevice));
    atomic_store(&b->outNominalRate, NominalRate(b->outDevice));
    atomic_store(&b->resetRequested, true);
    return noErr;
}

static OSStatus AliveListener(AudioObjectID obj, UInt32 n, const AudioObjectPropertyAddress *addrs, void *ctx) {
    VSBridge *b = ctx;
    if (!UInt32Property(obj, kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal))
        atomic_store(&b->lastError, obj == b->inDevice ? "Varispeed device went away" : "Output device went away");
    return noErr;
}

#pragma mark - Public API

void VSBridgeDefaultConfig(VSBridgeConfig *c) {
    memset(c, 0, sizeof *c);
    c->outputNameHint = "UltraLite";
    c->outputChannels[0] = 0;
    c->outputChannels[1] = 1;
    c->quality = VSBridgeQualityBest;
    c->inputBufferFrames = 64;
    c->outputBufferFrames = 128;
    c->safetyMarginMs = 2.0;
}

VSBridge *VSBridgeCreate(const VSBridgeConfig *config) {
    VSBridge *b = calloc(1, sizeof *b);
    b->config = *config;
    b->config.outputDeviceUID = NULL;   // copied into outputUID at start
    if (config->outputDeviceUID) snprintf(b->outputUID, sizeof b->outputUID, "%s", config->outputDeviceUID);
    b->ring = calloc(kRingFrames * kChannels, sizeof(float));
    b->srcIn = calloc(kScratchFrames * kChannels, sizeof(float));
    b->srcOut = calloc(kScratchFrames * kChannels, sizeof(float));
    int err = 0;
    b->src = src_new(SRCType(config->quality), kChannels, &err);
    b->hostTicksPerSecond = HostTicksPerSecond();
    b->resamplerDelayInputFrames = SRCDelayFrames(config->quality);
    atomic_store(&b->inRateScalar, 1.0);
    return b;
}

static bool Fail(VSBridge *b, const char *msg) {
    atomic_store(&b->lastError, msg);
    VSBridgeStop(b);
    return false;
}

bool VSBridgeStart(VSBridge *b) {
    if (b->started) return true;
    atomic_store(&b->lastError, NULL);
    if (!b->src) return Fail(b, "Could not create the resampler");

    b->inDevice = DeviceForUID(kVarispeedUID);
    if (!b->inDevice) return Fail(b, "Varispeed device not found (is the driver installed?)");
    b->outDevice = b->outputUID[0] ? DeviceForUID(b->outputUID)
                                   : FindDeviceByNameHint(b->config.outputNameHint ? b->config.outputNameHint : "UltraLite", b->outputUID, sizeof b->outputUID);
    if (!b->outDevice) return Fail(b, "Output device not found");
    if (b->outDevice == b->inDevice) return Fail(b, "Output device can't be Varispeed itself");

    // Map the chosen output channels onto the output device's stream buffers.
    AudioObjectPropertyAddress sa = { kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(b->outDevice, &sa, 0, NULL, &size) != noErr) return Fail(b, "Can't read output channels");
    AudioBufferList *bl = malloc(size);
    AudioObjectGetPropertyData(b->outDevice, &sa, 0, NULL, &size, bl);
    for (int c = 0; c < kChannels; c++) {
        b->outMap[c].valid = false;
        UInt32 base = 0;
        for (UInt32 i = 0; i < bl->mNumberBuffers; i++) {
            UInt32 n = bl->mBuffers[i].mNumberChannels;
            if (b->config.outputChannels[c] >= (int)base && b->config.outputChannels[c] < (int)(base + n)) {
                b->outMap[c].buffer = i;
                b->outMap[c].channel = (UInt32)b->config.outputChannels[c] - base;
                b->outMap[c].stride = n;
                b->outMap[c].valid = true;
            }
            base += n;
        }
    }
    free(bl);
    if (!b->outMap[0].valid) return Fail(b, "Chosen output channels don't exist on the output device");

    atomic_store(&b->inNominalRate, NominalRate(b->inDevice));
    atomic_store(&b->outNominalRate, NominalRate(b->outDevice));
    double outRate = atomic_load(&b->outNominalRate);
    b->outputDeviceLatencySeconds = (UInt32Property(b->outDevice, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput)
                                   + UInt32Property(b->outDevice, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput)) / (outRate > 0 ? outRate : 48000.0);

    SetBufferFrames(b->inDevice, b->config.inputBufferFrames);
    SetBufferFrames(b->outDevice, b->config.outputBufferFrames);

    atomic_store(&b->writePos, 0);
    atomic_store(&b->readPos, 0);
    atomic_store(&b->histCount, 0);
    b->histFloor = 0;
    src_reset(b->src);
    b->primed = false;
    b->delay = kInitialDelay;
    b->minSlack = 1.0;
    b->clock = b->slackWindowStart = b->lastStepUpTime = 0;
    b->glitchWarmup = 4096;

    if (AudioDeviceCreateIOProcID(b->inDevice, InputIOProc, b, &b->inProc) != noErr) return Fail(b, "Can't open Varispeed input");
    if (AudioDeviceCreateIOProcID(b->outDevice, OutputIOProc, b, &b->outProc) != noErr) return Fail(b, "Can't open output device");
    SetStreamUsage(b->inDevice, b->inProc, kAudioObjectPropertyScopeOutput, false);
    SetStreamUsage(b->outDevice, b->outProc, kAudioObjectPropertyScopeInput, false);

    AudioObjectPropertyAddress ra = { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectPropertyAddress la = { kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectAddPropertyListener(b->inDevice, &ra, RateListener, b);
    AudioObjectAddPropertyListener(b->outDevice, &ra, RateListener, b);
    AudioObjectAddPropertyListener(b->inDevice, &la, AliveListener, b);
    AudioObjectAddPropertyListener(b->outDevice, &la, AliveListener, b);

    b->started = true;
    if (AudioDeviceStart(b->inDevice, b->inProc) != noErr) return Fail(b, "Can't start Varispeed input");
    if (AudioDeviceStart(b->outDevice, b->outProc) != noErr) return Fail(b, "Can't start output device");
    return true;
}

void VSBridgeStop(VSBridge *b) {
    AudioObjectPropertyAddress ra = { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectPropertyAddress la = { kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    if (b->outProc) {
        AudioDeviceStop(b->outDevice, b->outProc);
        AudioDeviceDestroyIOProcID(b->outDevice, b->outProc);
        b->outProc = NULL;
    }
    if (b->inProc) {
        AudioDeviceStop(b->inDevice, b->inProc);
        AudioDeviceDestroyIOProcID(b->inDevice, b->inProc);
        b->inProc = NULL;
    }
    if (b->started) {
        AudioObjectRemovePropertyListener(b->inDevice, &ra, RateListener, b);
        AudioObjectRemovePropertyListener(b->outDevice, &ra, RateListener, b);
        AudioObjectRemovePropertyListener(b->inDevice, &la, AliveListener, b);
        AudioObjectRemovePropertyListener(b->outDevice, &la, AliveListener, b);
    }
    b->started = false;
}

void VSBridgeGetStats(VSBridge *b, VSBridgeStats *s) {
    memset(s, 0, sizeof *s);
    s->running = b->started;
    if (b->outDevice) CopyStringProperty(b->outDevice, kAudioObjectPropertyName, s->outputDeviceName, sizeof s->outputDeviceName);
    s->inputSampleRate = atomic_load(&b->inNominalRate);
    s->outputSampleRate = atomic_load(&b->outNominalRate);
    s->inputRealRate = atomic_load(&b->statInRealRate);
    s->outputRealRate = atomic_load(&b->statOutRealRate);
    s->speed = atomic_load(&b->statSpeed);
    s->ringFillMs = atomic_load(&b->statFillSeconds) * 1000.0;
    s->targetFillMs = atomic_load(&b->statTargetSeconds) * 1000.0;
    s->correctionPPM = atomic_load(&b->statCorrection) * 1e6;
    double resamplerMs = s->inputRealRate > 0 ? b->resamplerDelayInputFrames / s->inputRealRate * 1000.0 : 0;
    s->latencyMs = s->targetFillMs + resamplerMs;
    s->outputDeviceLatencyMs = b->outputDeviceLatencySeconds * 1000.0;
    s->cpuLoad = atomic_load(&b->statCpu);
    s->underruns = atomic_load(&b->underruns);
    s->overflows = atomic_load(&b->overflows);
    s->glitches = atomic_load(&b->glitches);
    s->inputGlitches = atomic_load(&b->inputGlitches);
    s->resyncs = atomic_load(&b->resyncs);
    s->resets = atomic_load(&b->resets);
    s->inputCycles = atomic_load(&b->inputCycles);
    s->outputCycles = atomic_load(&b->outputCycles);
    s->lastError = atomic_load(&b->lastError);
}

void VSBridgeResetCounters(VSBridge *b) {
    atomic_store(&b->underruns, 0);
    atomic_store(&b->overflows, 0);
    atomic_store(&b->glitches, 0);
    atomic_store(&b->inputGlitches, 0);
    atomic_store(&b->resyncs, 0);
    atomic_store(&b->resets, 0);
}

void VSBridgeSetRecorder(VSBridge *b, VSRecorder *recorder) {
    atomic_store_explicit(&b->recorder, recorder, memory_order_release);
}

void VSBridgeDestroy(VSBridge *b) {
    if (!b) return;
    VSBridgeStop(b);
    if (b->src) src_delete(b->src);
    free(b->ring);
    free(b->srcIn);
    free(b->srcOut);
    free(b);
}
