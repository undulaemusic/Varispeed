// vsbridge: runs the Varispeed -> output-device bridge from the command line and prints stats.
//
//   vsbridge [--out L,R] [--quality best|medium|fast] [--in-buffer N] [--out-buffer N]
//            [--margin MS] [--seconds N] [--mute] [--device-uid UID]
//
// --out takes 1-based channel numbers (MOTU: 1,2 = Main Out; 11,12 = Phones).
#include "VSBridge.h"
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t gStop = 0;
static void onSignal(int s) { gStop = 1; }

int main(int argc, char **argv) {
    VSBridgeConfig cfg;
    VSBridgeDefaultConfig(&cfg);
    double seconds = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--out") && v) { int l, r; if (sscanf(v, "%d,%d", &l, &r) == 2) { cfg.outputChannels[0] = l - 1; cfg.outputChannels[1] = r - 1; } i++; }
        else if (!strcmp(a, "--quality") && v) { cfg.quality = !strcmp(v, "fast") ? VSBridgeQualityFast : !strcmp(v, "medium") ? VSBridgeQualityMedium : VSBridgeQualityBest; i++; }
        else if (!strcmp(a, "--in-buffer") && v) { cfg.inputBufferFrames = (uint32_t)atoi(v); i++; }
        else if (!strcmp(a, "--out-buffer") && v) { cfg.outputBufferFrames = (uint32_t)atoi(v); i++; }
        else if (!strcmp(a, "--margin") && v) { cfg.safetyMarginMs = atof(v); i++; }
        else if (!strcmp(a, "--seconds") && v) { seconds = atof(v); i++; }
        else if (!strcmp(a, "--device-uid") && v) { cfg.outputDeviceUID = v; i++; }
        else if (!strcmp(a, "--mute")) cfg.muteOutput = true;
        else { fprintf(stderr, "usage: vsbridge [--out L,R] [--quality best|medium|fast] [--in-buffer N] [--out-buffer N] [--margin MS] [--seconds N] [--mute]\n"); return 1; }
    }
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    VSBridge *b = VSBridgeCreate(&cfg);
    if (!VSBridgeStart(b)) {
        VSBridgeStats s; VSBridgeGetStats(b, &s);
        fprintf(stderr, "Bridge failed to start: %s\n", s.lastError ? s.lastError : "unknown error");
        return 1;
    }
    VSBridgeStats s;
    sleep(1);
    VSBridgeGetStats(b, &s);
    printf("Bridge running: Varispeed (%.0f Hz) -> %s (%.0f Hz), channels %d,%d%s. Ctrl-C to stop.\n",
           s.inputSampleRate, s.outputDeviceName, s.outputSampleRate, cfg.outputChannels[0] + 1, cfg.outputChannels[1] + 1, cfg.muteOutput ? " [MUTED]" : "");
    printf("%7s %7s %8s %8s %9s %8s %6s | %5s %5s %5s %5s\n", "time", "speed", "fill ms", "target", "corr ppm", "lat ms", "cpu%", "under", "over", "glitch", "reset");
    double t = 0;
    while (!gStop && (seconds <= 0 || t < seconds)) {
        sleep(1); t += 1;
        VSBridgeGetStats(b, &s);
        printf("%6.0fs %6.1f%% %8.2f %8.2f %9.0f %8.2f %6.2f | %5llu %5llu %5llu %5llu\n", t, s.speed * 100, s.ringFillMs, s.targetFillMs, s.correctionPPM,
               s.latencyMs, s.cpuLoad * 100, (unsigned long long)s.underruns, (unsigned long long)s.overflows, (unsigned long long)s.glitches, (unsigned long long)s.resets);
        fflush(stdout);
        if (s.lastError) { fprintf(stderr, "Error: %s\n", s.lastError); break; }
    }
    VSBridgeGetStats(b, &s);
    printf("\nSummary: underruns %llu, overflows %llu, glitches %llu, resets %llu, output device latency %.2f ms\n",
           (unsigned long long)s.underruns, (unsigned long long)s.overflows, (unsigned long long)s.glitches, (unsigned long long)s.resets, s.outputDeviceLatencyMs);
    VSBridgeDestroy(b);
    return 0;
}
