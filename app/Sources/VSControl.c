#include "VSControl.h"
#include "../../driver/BlackHole/VarispeedProperties.h"
#include <dispatch/dispatch.h>
#include <stdlib.h>
#include <string.h>

#define kVarispeedUID "Varispeed_UID"

AudioObjectID VSControlVarispeedDevice(void) {
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFStringRef uid = CFSTR(kVarispeedUID);
    AudioObjectID dev = kAudioObjectUnknown;
    UInt32 size = sizeof(dev);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, sizeof(uid), &uid, &size, &dev) != noErr) return kAudioObjectUnknown;
    return dev;
}

bool VSControlGetDouble(AudioObjectID dev, AudioObjectPropertySelector sel, double *value) {
    AudioObjectPropertyAddress a = { sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFPropertyListRef plist = NULL;
    UInt32 size = sizeof(plist);
    if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, &plist) != noErr || !plist) return false;
    bool ok = CFGetTypeID(plist) == CFNumberGetTypeID() && CFNumberGetValue((CFNumberRef)plist, kCFNumberFloat64Type, value);
    CFRelease(plist);
    return ok;
}

bool VSControlSetDouble(AudioObjectID dev, AudioObjectPropertySelector sel, double value) {
    AudioObjectPropertyAddress a = { sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFNumberRef num = CFNumberCreate(NULL, kCFNumberFloat64Type, &value);
    CFPropertyListRef plist = num;
    OSStatus err = AudioObjectSetPropertyData(dev, &a, 0, NULL, sizeof(plist), &plist);
    CFRelease(num);
    return err == noErr;
}

double VSControlNominalSampleRate(AudioObjectID dev) {
    AudioObjectPropertyAddress a = { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    Float64 rate = 0;
    UInt32 size = sizeof(rate);
    AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, &rate);
    return rate;
}

static bool CopyString(AudioObjectID dev, AudioObjectPropertySelector sel, char *out, size_t len) {
    AudioObjectPropertyAddress a = { sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFStringRef s = NULL;
    UInt32 size = sizeof(s);
    if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, &s) != noErr || !s) return false;
    bool ok = CFStringGetCString(s, out, (CFIndex)len, kCFStringEncodingUTF8);
    CFRelease(s);
    return ok;
}

int VSControlListOutputDevices(VSOutputDevice *devices, int max) {
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &size) != noErr) return 0;
    AudioObjectID *ids = malloc(size);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, ids);
    int count = 0;
    for (UInt32 i = 0; i < size / sizeof(AudioObjectID) && count < max; i++) {
        VSOutputDevice *d = &devices[count];
        if (!CopyString(ids[i], kAudioDevicePropertyDeviceUID, d->uid, sizeof d->uid)) continue;
        if (!strcmp(d->uid, kVarispeedUID) || !strcmp(d->uid, "Varispeed_2_UID")) continue;
        if (!CopyString(ids[i], kAudioObjectPropertyName, d->name, sizeof d->name)) continue;
        AudioObjectPropertyAddress sa = { kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain };
        UInt32 bs = 0;
        if (AudioObjectGetPropertyDataSize(ids[i], &sa, 0, NULL, &bs) != noErr || bs == 0) continue;
        AudioBufferList *bl = malloc(bs);
        int ch = 0;
        if (AudioObjectGetPropertyData(ids[i], &sa, 0, NULL, &bs, bl) == noErr)
            for (UInt32 j = 0; j < bl->mNumberBuffers; j++) ch += (int)bl->mBuffers[j].mNumberChannels;
        free(bl);
        if (ch < 1) continue;
        d->outputChannels = ch;
        count++;
    }
    free(ids);
    return count;
}

void VSControlObserveDeviceList(void (*callback)(void *), void *context) {
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectAddPropertyListenerBlock(kAudioObjectSystemObject, &a, dispatch_get_main_queue(),
                                        ^(UInt32 n, const AudioObjectPropertyAddress *addrs) { callback(context); });
}
