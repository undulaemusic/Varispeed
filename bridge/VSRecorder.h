// VSRecorder: records the bridge's output (what you hear) to a 32-bit float stereo WAV file.
// The audio thread only copies samples into a lock-free ring (VSRecorderPush); a background
// thread writes the file. On stop, the file is converted to the requested sample rate if it
// differs from the rate it was recorded at.
#ifndef VSRecorder_h
#define VSRecorder_h

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VSRecorder VSRecorder;

typedef struct {
    bool recording;
    double seconds;              // recorded so far (or total, after stop)
    double peak;                 // peak level since start, 0..1+
    uint64_t droppedFrames;      // frames lost because the writer fell behind (should stay 0)
} VSRecorderStatus;

VSRecorder *VSRecorderCreate(void);
void VSRecorderDestroy(VSRecorder *rec);

// Starts writing interleaved stereo frames recorded at sampleRate into path. Returns false on error.
bool VSRecorderStart(VSRecorder *rec, const char *path, double sampleRate);

// Stops, finalizes the file and converts it to targetSampleRate (0 = keep). Blocks until done.
// Returns false if no file was written or conversion failed.
bool VSRecorderStop(VSRecorder *rec, double targetSampleRate);

// Realtime-safe: called from the output IOProc. Ignored unless recording.
void VSRecorderPush(VSRecorder *rec, const float *interleavedStereo, uint32_t frames);

void VSRecorderGetStatus(VSRecorder *rec, VSRecorderStatus *status);

#ifdef __cplusplus
}
#endif

#endif
