// Small helpers shared by the Varispeed command-line tools.
#ifndef vsdevice_h
#define vsdevice_h

#include <CoreAudio/CoreAudio.h>
#include <stdlib.h>
#include <string.h>
#include "../driver/BlackHole/VarispeedProperties.h"

#define kVarispeed_DeviceUID "Varispeed_UID"

static AudioObjectID vs_find_device_by_uid(const char *uid) {
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFStringRef cfuid = CFStringCreateWithCString(NULL, uid, kCFStringEncodingUTF8);
    AudioObjectID dev = kAudioObjectUnknown;
    UInt32 size = sizeof(dev);
    OSStatus err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, sizeof(cfuid), &cfuid, &size, &dev);
    CFRelease(cfuid);
    return err == noErr ? dev : kAudioObjectUnknown;
}

static OSStatus vs_get_double(AudioObjectID dev, AudioObjectPropertySelector sel, double *out) {
    AudioObjectPropertyAddress a = { sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFPropertyListRef plist = NULL;
    UInt32 size = sizeof(plist);
    OSStatus err = AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, &plist);
    if (err != noErr) return err;
    if (!plist || CFGetTypeID(plist) != CFNumberGetTypeID()) { if (plist) CFRelease(plist); return kAudioHardwareUnspecifiedError; }
    CFNumberGetValue((CFNumberRef)plist, kCFNumberFloat64Type, out);
    CFRelease(plist);
    return noErr;
}

static OSStatus vs_set_double(AudioObjectID dev, AudioObjectPropertySelector sel, double value) {
    AudioObjectPropertyAddress a = { sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFNumberRef num = CFNumberCreate(NULL, kCFNumberFloat64Type, &value);
    CFPropertyListRef plist = num;
    OSStatus err = AudioObjectSetPropertyData(dev, &a, 0, NULL, sizeof(plist), &plist);
    CFRelease(num);
    return err;
}

#endif
