// VSBridge: plays the Varispeed device's input on a real output device (e.g. the MOTU),
// resampling the sped-up / slowed-down stream into the output device's real clock.
//
//   Varispeed input IOProc --> lock-free ring buffer --> libsamplerate --> output IOProc
//
// Each input callback records where its audio sits on the Varispeed timeline (host time ->
// ring position). The output plays that timeline delayed by a fixed D seconds, so the
// resampling ratio for each output buffer is simply (timeline frames spanned) / (output
// frames), which follows speed changes and both devices' clock drift exactly. A small
// correction removes accumulated position error; D adapts to the smallest safe value.
#ifndef VSBridge_h
#define VSBridge_h

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VSBridgeQualityBest = 0,    // libsamplerate SRC_SINC_BEST_QUALITY
    VSBridgeQualityMedium = 1,  // SRC_SINC_MEDIUM_QUALITY
    VSBridgeQualityFast = 2,    // SRC_SINC_FASTEST
} VSBridgeQuality;

typedef struct {
    const char *outputDeviceUID;    // NULL = first device whose name contains outputNameHint
    const char *outputNameHint;     // e.g. "UltraLite"
    int outputChannels[2];          // 0-based output channels for left/right (e.g. {0,1} = Main Out 1-2)
    VSBridgeQuality quality;
    uint32_t inputBufferFrames;     // IO buffer this process asks of Varispeed (0 = leave as is)
    uint32_t outputBufferFrames;    // IO buffer this process asks of the output device (0 = leave as is)
    double safetyMarginMs;          // extra ring-buffer cushion beyond the minimum
    bool muteOutput;                // run everything but write silence (for silent testing)
} VSBridgeConfig;

typedef struct {
    bool running;
    char outputDeviceName[128];
    double inputSampleRate, outputSampleRate;   // nominal
    double inputRealRate, outputRealRate;       // frames per real second (from rate scalars)
    double speed;                               // inputRealRate / inputSampleRate
    double ringFillMs;                          // audio queued in the ring, in real time
    double targetFillMs;                        // D: how far behind the Varispeed timeline the output plays
    double correctionPPM;                       // position-error correction applied this cycle
    double latencyMs;                           // bridge-added: D + resampler delay
    double outputDeviceLatencyMs;               // output device's own latency + safety offset
    double cpuLoad;                             // fraction of the output IO cycle spent in the bridge
    uint64_t underruns, overflows, resets, resyncs;
    uint64_t glitches, inputGlitches;           // test-tone detector on output / on raw Varispeed input
    uint64_t inputCycles, outputCycles;
    const char *lastError;                      // static string or NULL
} VSBridgeStats;

typedef struct VSBridge VSBridge;

void VSBridgeDefaultConfig(VSBridgeConfig *config);
VSBridge *VSBridgeCreate(const VSBridgeConfig *config);
bool VSBridgeStart(VSBridge *bridge);           // returns false and sets lastError on failure
void VSBridgeStop(VSBridge *bridge);
void VSBridgeGetStats(VSBridge *bridge, VSBridgeStats *stats);
void VSBridgeResetCounters(VSBridge *bridge);
void VSBridgeDestroy(VSBridge *bridge);

#ifdef __cplusplus
}
#endif

#endif
