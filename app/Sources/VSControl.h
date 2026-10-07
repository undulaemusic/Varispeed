// C helpers the menu bar app uses to talk to Core Audio and the Varispeed driver.
#ifndef VSControl_h
#define VSControl_h

#include <CoreAudio/CoreAudio.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// The Varispeed device, or kAudioObjectUnknown if the driver isn't installed.
AudioObjectID VSControlVarispeedDevice(void);

// Read / write one of the driver's custom properties (kVarispeedProperty_*). Values are doubles.
bool VSControlGetDouble(AudioObjectID device, AudioObjectPropertySelector selector, double *value);
bool VSControlSetDouble(AudioObjectID device, AudioObjectPropertySelector selector, double value);

double VSControlNominalSampleRate(AudioObjectID device);

typedef struct {
    char uid[256];
    char name[256];
    int outputChannels;
} VSOutputDevice;

// Fills up to max devices that have output channels (Varispeed itself excluded). Returns the count.
int VSControlListOutputDevices(VSOutputDevice *devices, int max);

// Calls back on the main queue whenever devices are added or removed.
void VSControlObserveDeviceList(void (*callback)(void *context), void *context);

#ifdef __cplusplus
}
#endif

#endif
