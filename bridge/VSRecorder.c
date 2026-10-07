// VSRecorder: see VSRecorder.h.
#include "VSRecorder.h"
#include "../third_party/libsamplerate/samplerate.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define kRecRingFrames   (1u << 19)          // ~11 s at 48 kHz: plenty of slack for the writer thread
#define kRecRingMask     (kRecRingFrames - 1)
#define kWavHeaderBytes  58                  // RIFF + fmt (18) + fact + data headers

struct VSRecorder {
    float *ring;
    _Atomic uint64_t writePos, readPos;
    _Atomic bool active;
    _Atomic uint64_t dropped, framesWritten;
    _Atomic float peak;

    FILE *file;
    char path[1024];
    double sampleRate;
    pthread_t thread;
    _Atomic bool threadRunning;
};

#pragma mark - WAV

static void PutU32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void PutU16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

// 32-bit IEEE float stereo WAV header (WAVE_FORMAT_IEEE_FLOAT with an 18-byte fmt and a fact chunk).
static void WriteWavHeader(FILE *f, double sampleRate, uint64_t frames) {
    uint8_t h[kWavHeaderBytes];
    uint32_t dataBytes = (uint32_t)(frames * 2 * sizeof(float));
    memcpy(h, "RIFF", 4);       PutU32(h + 4, kWavHeaderBytes - 8 + dataBytes);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);  PutU32(h + 16, 18);
    PutU16(h + 20, 3);                                   // IEEE float
    PutU16(h + 22, 2);                                   // channels
    PutU32(h + 24, (uint32_t)llround(sampleRate));
    PutU32(h + 28, (uint32_t)llround(sampleRate) * 2 * sizeof(float));
    PutU16(h + 32, 2 * sizeof(float));                   // block align
    PutU16(h + 34, 32);                                  // bits per sample
    PutU16(h + 36, 0);                                   // cbSize
    memcpy(h + 38, "fact", 4);  PutU32(h + 42, 4); PutU32(h + 46, (uint32_t)frames);
    memcpy(h + 50, "data", 4);  PutU32(h + 54, dataBytes);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, f);
}

#pragma mark - Writer thread

static void Drain(VSRecorder *rec) {
    uint64_t r = atomic_load_explicit(&rec->readPos, memory_order_relaxed);
    uint64_t w = atomic_load_explicit(&rec->writePos, memory_order_acquire);
    while (r < w) {
        uint64_t start = r & kRecRingMask;
        uint64_t n = w - r;
        if (start + n > kRecRingFrames) n = kRecRingFrames - start;   // up to the wrap point
        fwrite(&rec->ring[start * 2], sizeof(float) * 2, n, rec->file);
        r += n;
        atomic_fetch_add_explicit(&rec->framesWritten, n, memory_order_relaxed);
    }
    atomic_store_explicit(&rec->readPos, r, memory_order_release);
}

static void *WriterThread(void *arg) {
    VSRecorder *rec = arg;
    while (atomic_load(&rec->threadRunning)) {
        Drain(rec);
        usleep(20000);
    }
    Drain(rec);
    return NULL;
}

#pragma mark - Sample rate conversion (offline, after recording)

static bool ConvertFile(const char *path, double fromRate, double toRate, uint64_t frames) {
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.converting", path);
    FILE *in = fopen(path, "rb"), *out = fopen(tmp, "wb");
    if (!in || !out) { if (in) fclose(in); if (out) fclose(out); return false; }
    int err = 0;
    SRC_STATE *src = src_new(SRC_SINC_BEST_QUALITY, 2, &err);
    if (!src) { fclose(in); fclose(out); return false; }

    enum { kChunk = 8192 };
    double ratio = toRate / fromRate;
    long outCap = (long)ceil(kChunk * ratio) + 256;
    float *ib = malloc(sizeof(float) * 2 * kChunk), *ob = malloc(sizeof(float) * 2 * outCap);
    WriteWavHeader(out, toRate, 0);
    fseek(in, kWavHeaderBytes, SEEK_SET);

    uint64_t outFrames = 0, remaining = frames;
    bool ok = true, eof = false;
    long have = 0, offset = 0;                            // unconsumed input frames in ib
    while (ok) {
        if (have == 0 && !eof) {
            long n = (long)(remaining < kChunk ? remaining : kChunk);
            n = (long)fread(ib, sizeof(float) * 2, (size_t)n, in);
            remaining -= (uint64_t)n;
            have = n; offset = 0;
            if (remaining == 0 || n == 0) eof = true;
        }
        SRC_DATA d = { .data_in = ib + offset * 2, .data_out = ob, .input_frames = have, .output_frames = outCap,
                       .end_of_input = eof ? 1 : 0, .src_ratio = ratio };
        if (src_process(src, &d) != 0) { ok = false; break; }
        offset += d.input_frames_used; have -= d.input_frames_used;
        fwrite(ob, sizeof(float) * 2, (size_t)d.output_frames_gen, out);
        outFrames += (uint64_t)d.output_frames_gen;
        if (eof && have == 0 && d.output_frames_gen == 0) break;
    }
    WriteWavHeader(out, toRate, outFrames);
    src_delete(src);
    free(ib); free(ob);
    fclose(in);
    fclose(out);
    if (!ok) { unlink(tmp); return false; }
    return rename(tmp, path) == 0;
}

#pragma mark - Public API

VSRecorder *VSRecorderCreate(void) {
    VSRecorder *rec = calloc(1, sizeof *rec);
    rec->ring = calloc(kRecRingFrames * 2, sizeof(float));
    return rec;
}

void VSRecorderDestroy(VSRecorder *rec) {
    if (!rec) return;
    if (atomic_load(&rec->active)) VSRecorderStop(rec, 0);
    free(rec->ring);
    free(rec);
}

bool VSRecorderStart(VSRecorder *rec, const char *path, double sampleRate) {
    if (atomic_load(&rec->active) || sampleRate <= 0) return false;
    rec->file = fopen(path, "wb");
    if (!rec->file) return false;
    snprintf(rec->path, sizeof rec->path, "%s", path);
    rec->sampleRate = sampleRate;
    WriteWavHeader(rec->file, sampleRate, 0);
    atomic_store(&rec->writePos, 0);
    atomic_store(&rec->readPos, 0);
    atomic_store(&rec->dropped, 0);
    atomic_store(&rec->framesWritten, 0);
    atomic_store(&rec->peak, 0.0f);
    atomic_store(&rec->threadRunning, true);
    pthread_create(&rec->thread, NULL, WriterThread, rec);
    atomic_store_explicit(&rec->active, true, memory_order_release);
    return true;
}

bool VSRecorderStop(VSRecorder *rec, double targetSampleRate) {
    if (!atomic_load(&rec->active)) return false;
    atomic_store_explicit(&rec->active, false, memory_order_release);
    usleep(50000);                                        // let an in-flight IO cycle finish pushing
    atomic_store(&rec->threadRunning, false);
    pthread_join(rec->thread, NULL);
    uint64_t frames = atomic_load(&rec->framesWritten);
    WriteWavHeader(rec->file, rec->sampleRate, frames);
    fclose(rec->file);
    rec->file = NULL;
    if (frames == 0) { unlink(rec->path); return false; }
    if (targetSampleRate > 0 && fabs(targetSampleRate - rec->sampleRate) > 0.5)
        return ConvertFile(rec->path, rec->sampleRate, targetSampleRate, frames);
    return true;
}

void VSRecorderPush(VSRecorder *rec, const float *x, uint32_t frames) {
    if (!rec || !atomic_load_explicit(&rec->active, memory_order_acquire)) return;
    uint64_t w = atomic_load_explicit(&rec->writePos, memory_order_relaxed);
    uint64_t r = atomic_load_explicit(&rec->readPos, memory_order_acquire);
    if (kRecRingFrames - (w - r) < frames) {
        atomic_fetch_add_explicit(&rec->dropped, frames, memory_order_relaxed);
        return;
    }
    float peak = atomic_load_explicit(&rec->peak, memory_order_relaxed);
    for (uint32_t i = 0; i < frames; i++) {
        float *dst = &rec->ring[((w + i) & kRecRingMask) * 2];
        dst[0] = x[i * 2];
        dst[1] = x[i * 2 + 1];
        float a = fmaxf(fabsf(dst[0]), fabsf(dst[1]));
        if (a > peak) peak = a;
    }
    atomic_store_explicit(&rec->peak, peak, memory_order_relaxed);
    atomic_store_explicit(&rec->writePos, w + frames, memory_order_release);
}

void VSRecorderGetStatus(VSRecorder *rec, VSRecorderStatus *s) {
    s->recording = atomic_load(&rec->active);
    s->seconds = rec->sampleRate > 0 ? (double)atomic_load(&rec->writePos) / rec->sampleRate : 0;
    s->peak = atomic_load(&rec->peak);
    s->droppedFrames = atomic_load(&rec->dropped);
}
