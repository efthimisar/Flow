#pragma once

#include <juce_core/juce_core.h>
#include <vector>

#if JUCE_MAC
 #include <IOKit/hid/IOHIDLib.h>
#endif

// Φώτα πλήκτρων του Native Instruments Komplete Kontrol (S-series MK2) μέσω USB HID.
// Πρωτόκολλο βάσει των έργων SynthesiaKontrol (sonic74 / ojacques, MIT) και KompleteKontrolLightGuideSongConfigs (MIT):
//   vendor 0x17cc, S88 MK2 = 0x1630. Πρώτα [0xa0, 0, 0] (light-guide mode), μετά [0x81, 88 bytes], ένα byte ανά πλήκτρο (A0 = 0).
// Το byte είναι hue * 4 + φωτεινότητα (0..3), 0 = σβηστό. Δεν έχει δοκιμαστεί σε πραγματικό S88 MK2 από τον προγραμματιστή.
class KeyLights
{
public:
    ~KeyLights() { stop(); }

    bool isOn() const { return active; }
    juce::String getStatus() const { return status; }

    bool start()
    {
#if JUCE_MAC
        if (active) return true;
        status = {};

        manager = IOHIDManagerCreate (kCFAllocatorDefault, kIOHIDOptionsTypeNone);
        if (manager == nullptr) return fail ("IOHIDManagerCreate απέτυχε");

        int vid = 0x17cc;
        CFNumberRef vnum = CFNumberCreate (kCFAllocatorDefault, kCFNumberIntType, &vid);
        CFMutableDictionaryRef match = CFDictionaryCreateMutable (kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue (match, CFSTR (kIOHIDVendorIDKey), vnum);
        CFRelease (vnum);
        IOHIDManagerSetDeviceMatching (manager, match);
        CFRelease (match);

        const IOReturn r = IOHIDManagerOpen (manager, kIOHIDOptionsTypeNone);
        if (r != kIOReturnSuccess) return fail ("IOHIDManagerOpen: 0x" + juce::String::toHexString ((int) r) + " (ίσως χρειάζεται άδεια Input Monitoring στο Σύστημα > Απόρρητο)");

        CFSetRef set = IOHIDManagerCopyDevices (manager);
        if (set == nullptr || CFSetGetCount (set) == 0)
        {
            if (set != nullptr) CFRelease (set);
            return fail ("Δεν βρέθηκε συσκευή Native Instruments (USB)");
        }

        const CFIndex n = CFSetGetCount (set);
        std::vector<const void*> devs ((size_t) n);
        CFSetGetValues (set, devs.data());

        juce::String found;
        for (CFIndex i = 0; i < n && device == nullptr; ++i)
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
                device = d;
                keyCount = pid == 0x1630 ? 88 : pid == 0x1620 ? 61 : 49;
                offset = pid == 0x1630 ? 21 : 36;
            }
            else
            {
                IOHIDDeviceClose (d, kIOHIDOptionsTypeNone);
            }
        }
        CFRelease (set);

        if (device == nullptr) return fail ("Δεν βρέθηκε Komplete Kontrol MK2 (βρέθηκαν: " + found + ")");

        active = true;
        status = "OK (" + juce::String (keyCount) + " πλήκτρα)";
        return true;
#else
        return fail ("Μόνο σε macOS");
#endif
    }

    void stop()
    {
#if JUCE_MAC
        if (device != nullptr)
        {
            std::vector<uint8_t> off ((size_t) keyCount + 1, 0);
            off[0] = 0x81;
            IOHIDDeviceSetReport (device, kIOHIDReportTypeOutput, off[0], off.data(), off.size());
            IOHIDDeviceClose (device, kIOHIDOptionsTypeNone);
            CFRelease (device);
            device = nullptr;
        }
        if (manager != nullptr)
        {
            IOHIDManagerClose (manager, kIOHIDOptionsTypeNone);
            CFRelease (manager);
            manager = nullptr;
        }
#endif
        active = false;
    }

    // keys[0] = A0 (νότα 21). Μέγεθος: ό,τι πλήκτρα έχει το keyboard.
    int getKeyCount() const { return keyCount; }
    int getFirstNote() const { return offset; }

    bool sendKeys (const uint8_t* keys, int count)
    {
#if JUCE_MAC
        if (device == nullptr) return false;
        std::vector<uint8_t> data ((size_t) keyCount + 1, 0);
        data[0] = 0x81;
        for (int i = 0; i < keyCount && i < count; ++i) data[(size_t) i + 1] = keys[i];
        const IOReturn r = IOHIDDeviceSetReport (device, kIOHIDReportTypeOutput, data[0], data.data(), data.size());
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

private:
    bool fail (const juce::String& why)
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

    IOHIDManagerRef manager = nullptr;
    IOHIDDeviceRef device = nullptr;
#endif
    bool active = false;
    int keyCount = 88, offset = 21;
    juce::String status;
};
