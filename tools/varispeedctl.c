// varispeedctl: read or set the Varispeed device's speed.
//
//   varispeedctl                 show current speed, target and ramp time
//   varispeedctl 0.8             glide to 80% speed
//   varispeedctl -- -2st         glide to 2 semitones down (also: +7st)
//   varispeedctl --ramp 2 0.5    set ramp time to 2 s, then glide to 50%
//   varispeedctl --ramp 0        make speed changes instant
//   varispeedctl --watch         print the current speed 10x per second
#include "vsdevice.h"
#include <math.h>
#include <stdio.h>
#include <unistd.h>

static void usage(void) {
    fprintf(stderr, "usage: varispeedctl [--ramp SECONDS] [SPEED | Nst] | --watch\n"
                    "  SPEED is a ratio from %.2f to %.2f (1 = normal); Nst is semitones, e.g. -12st\n",
            kVarispeed_MinSpeed, kVarispeed_MaxSpeed);
}

static void show(AudioObjectID dev) {
    double cur = 0, tgt = 0, ramp = 0;
    vs_get_double(dev, kVarispeedProperty_CurrentSpeed, &cur);
    vs_get_double(dev, kVarispeedProperty_TargetSpeed, &tgt);
    vs_get_double(dev, kVarispeedProperty_RampSeconds, &ramp);
    printf("speed %.4f (%.1f%%, %+.2f st)  target %.4f  ramp %.2fs\n", cur, cur * 100, 12 * log2(cur), tgt, ramp);
}

int main(int argc, char **argv) {
    AudioObjectID dev = vs_find_device_by_uid(kVarispeed_DeviceUID);
    if (dev == kAudioObjectUnknown) { fprintf(stderr, "Varispeed device not found. Is the driver installed?\n"); return 1; }

    int i = 1;
    for (; i < argc; i++) {
        if (strcmp(argv[i], "--watch") == 0) {
            for (;;) { show(dev); usleep(100000); }
        } else if (strcmp(argv[i], "--ramp") == 0 && i + 1 < argc) {
            OSStatus e = vs_set_double(dev, kVarispeedProperty_RampSeconds, atof(argv[++i]));
            if (e) { fprintf(stderr, "failed to set ramp (%d)\n", (int)e); return 1; }
        } else if (strcmp(argv[i], "--") == 0) {
            continue;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(); return 0;
        } else {
            char *end = NULL;
            double v = strtod(argv[i], &end);
            if (end == argv[i]) { usage(); return 1; }
            if (strcmp(end, "st") == 0) v = pow(2.0, v / 12.0);
            else if (*end) { usage(); return 1; }
            OSStatus e = vs_set_double(dev, kVarispeedProperty_TargetSpeed, v);
            if (e) { fprintf(stderr, "failed to set speed (%d)\n", (int)e); return 1; }
        }
    }
    show(dev);
    return 0;
}
