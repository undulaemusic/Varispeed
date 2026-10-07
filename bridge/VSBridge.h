// VSBridge: plays the Varispeed device's input on a real output device (e.g. the MOTU),
// resampling the sped-up / slowed-down stream into the output device's real clock.
//
//   Varispeed input IOProc --> lock-free ring buffer --> libsamplerate --> output IOProc
//
// The resampling ratio is fed forward from both devices' HAL rate scalars (which carry the
// varispeed ratio and the output crystal's drift), plus a slow PI correction that holds the
// ring buffer at a target fill level so it never runs dry or overflows.
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
    double ringFillMs;                          // smoothed, in real time
    double targetFillMs;
    double correctionPPM;                       // PI correction applied on top of the feed-forward ratio
    double latencyMs;                           // ring + resampler + output buffer (bridge-added)
    double outputDeviceLatencyMs;               // output device's own latency + safety offset
    double cpuLoad;                             // fraction of the output IO cycle spent in the bridge
    uint64_t underruns, overflows, glitches, resets;
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
