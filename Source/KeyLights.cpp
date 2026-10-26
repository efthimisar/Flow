// ΠΡΟΣΟΧΗ: το IOKit ορίζει δικό του "Point" (MacTypes.h) που συγκρούεται με το juce::Point. Γι' αυτό είναι σε ξεχωριστό αρχείο, χωρίς "using namespace juce".
#include "KeyLights.h"
#include <vector>

#if JUCE_MAC
 #include <IOKit/hid/IOHIDLib.h>
#endif

struct KeyLights::Impl
{
#if JUCE_MAC
    IOHIDManagerRef manager = nullptr;
    IOHIDDeviceRef device = nullptr;
#endif
};

KeyLights::KeyLights() : impl (new Impl) {}
KeyLights::~KeyLights() { stop(); }

bool KeyLights::fail (const juce::String& why)
{
    status = why;
    stop();
    return false;
}

#if JUCE_MAC
static int intProperty (IOHIDDeviceRef d, CFStringRef key)
{
    int v = 0;
    if (auto ref = (CFNumberRef) IOHIDDeviceGetProperty (d, key))
        CFNumberGetValue (ref, kCFNumberIntType, &v);
    return v;
}
#endif

bool KeyLights::start()
{
#if JUCE_MAC
    if (active) return true;
    status = {};

    impl->manager = IOHIDManagerCreate (kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (impl->manager == nullptr) return fail ("IOHIDManagerCreate απέτυχε");

    int vid = 0x17cc;
    CFNumberRef vnum = CFNumberCreate (kCFAllocatorDefault, kCFNumberIntType, &vid);
    CFMutableDictionaryRef match = CFDictionaryCreateMutable (kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue (match, CFSTR (kIOHIDVendorIDKey), vnum);
    CFRelease (vnum);
    IOHIDManagerSetDeviceMatching (impl->manager, match);
    CFRelease (match);

    const IOReturn r = IOHIDManagerOpen (impl->manager, kIOHIDOptionsTypeNone);
    if (r != kIOReturnSuccess)
        return fail ("IOHIDManagerOpen: 0x" + juce::String::toHexString ((int) r) + " (ίσως χρειάζεται άδεια Input Monitoring στο Σύστημα > Απόρρητο)");

    CFSetRef set = IOHIDManagerCopyDevices (impl->manager);
    if (set == nullptr || CFSetGetCount (set) == 0)
    {
        if (set != nullptr) CFRelease (set);
        return fail ("Δεν βρέθηκε συσκευή Native Instruments (USB)");
    }

    const CFIndex n = CFSetGetCount (set);
    std::vector<const void*> devs ((size_t) n);
    CFSetGetValues (set, devs.data());

    juce::String found;
    for (CFIndex i = 0; i < n && impl->device == nullptr; ++i)
    {
        auto d = (IOHIDDeviceRef) devs[(size_t) i];
        const int pid = intProperty (d, CFSTR (kIOHIDProductIDKey));
        const int page = intProperty (d, CFSTR (kIOHIDPrimaryUsagePageKey));
        found << "pid 0x" << juce::String::toHexString (pid) << " page 0x" << juce::String::toHexString (page) << "; ";

        if (pid != 0x1630 && pid != 0x1620 && pid != 0x1610) continue;   // S88 / S61 / S49 MK2
        if (IOHIDDeviceOpen (d, kIOHIDOptionsTypeNone) != kIOReturnSuccess) continue;

        const uint8_t init[3] = { 0xa0, 0x00, 0x00 };
        if (IOHIDDeviceSetReport (d, kIOHIDReportTypeOutput, init[0], init, sizeof (init)) == kIOReturnSuccess)
        {
            CFRetain (d);
            impl->device = d;
            keyCount = pid == 0x1630 ? 88 : pid == 0x1620 ? 61 : 49;
            offset = pid == 0x1630 ? 21 : 36;
        }
        else
        {
            IOHIDDeviceClose (d, kIOHIDOptionsTypeNone);
        }
    }
    CFRelease (set);

    if (impl->device == nullptr) return fail ("Δεν βρέθηκε Komplete Kontrol MK2 (βρέθηκαν: " + found + ")");

    active = true;
    status = "OK (" + juce::String (keyCount) + " πλήκτρα)";
    return true;
#else
    return fail ("Μόνο σε macOS");
#endif
}

void KeyLights::stop()
{
#if JUCE_MAC
    if (impl->device != nullptr)
    {
        std::vector<uint8_t> off ((size_t) keyCount + 1, 0);
        off[0] = 0x81;
        IOHIDDeviceSetReport (impl->device, kIOHIDReportTypeOutput, off[0], off.data(), (CFIndex) off.size());
        IOHIDDeviceClose (impl->device, kIOHIDOptionsTypeNone);
        CFRelease (impl->device);
        impl->device = nullptr;
    }
    if (impl->manager != nullptr)
    {
        IOHIDManagerClose (impl->manager, kIOHIDOptionsTypeNone);
        CFRelease (impl->manager);
        impl->manager = nullptr;
    }
#endif
    active = false;
}

bool KeyLights::sendKeys (const uint8_t* keys, int count)
{
#if JUCE_MAC
    if (impl->device == nullptr) return false;
    std::vector<uint8_t> data ((size_t) keyCount + 1, 0);
    data[0] = 0x81;
    for (int i = 0; i < keyCount && i < count; ++i) data[(size_t) i + 1] = keys[i];
    const IOReturn r = IOHIDDeviceSetReport (impl->device, kIOHIDReportTypeOutput, data[0], data.data(), (CFIndex) data.size());
    if (r != kIOReturnSuccess)
    {
        status = "αποτυχία εγγραφής 0x" + juce::String::toHexString ((int) r);
        return false;
    }
    return true;
#else
    juce::ignoreUnused (keys, count);
    return false;
#endif
}
