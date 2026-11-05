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
    uint8_t inBuf[512] = {};
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
static void inputReportCb (void* ctx, IOReturn, void*, IOHIDReportType, uint32_t reportID, uint8_t* report, CFIndex len)
{
    juce::ignoreUnused (reportID);
    if (auto* self = static_cast<KeyLights*> (ctx)) self->logInput (report, (int) len);
}

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
    ctlHave = false;

    impl->manager = IOHIDManagerCreate (kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (impl->manager == nullptr) return fail ("IOHIDManagerCreate failed");

    int vid = 0x17cc;
    CFNumberRef vnum = CFNumberCreate (kCFAllocatorDefault, kCFNumberIntType, &vid);
    CFMutableDictionaryRef match = CFDictionaryCreateMutable (kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue (match, CFSTR (kIOHIDVendorIDKey), vnum);
    CFRelease (vnum);
    IOHIDManagerSetDeviceMatching (impl->manager, match);
    CFRelease (match);

    const IOReturn r = IOHIDManagerOpen (impl->manager, kIOHIDOptionsTypeNone);
    if (r != kIOReturnSuccess)
        return fail ("IOHIDManagerOpen: 0x" + juce::String::toHexString ((int) r) + " (Input Monitoring permission may be needed in System Settings > Privacy)");

    CFSetRef set = IOHIDManagerCopyDevices (impl->manager);
    if (set == nullptr || CFSetGetCount (set) == 0)
    {
        if (set != nullptr) CFRelease (set);
        return fail ("No Native Instruments (USB) device found");
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
            IOHIDDeviceRegisterInputReportCallback (d, impl->inBuf, (CFIndex) sizeof (impl->inBuf), inputReportCb, this);
            IOHIDDeviceScheduleWithRunLoop (d, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
            lastInitMs = juce::Time::getMillisecondCounter();
            keyCount = pid == 0x1630 ? 88 : pid == 0x1620 ? 61 : 49;
            offset = pid == 0x1630 ? 21 : 36;
        }
        else
        {
            IOHIDDeviceClose (d, kIOHIDOptionsTypeNone);
        }
    }
    CFRelease (set);

    if (impl->device == nullptr) return fail ("Komplete Kontrol MK2 not found (found: " + found + ")");

    active = true;
    status = "OK (" + juce::String (keyCount) + " keys)";
    return true;
#else
    return fail ("macOS only");
#endif
}

void KeyLights::stop()
{
#if JUCE_MAC
    if (impl->device != nullptr)
    {
        IOHIDDeviceRegisterInputReportCallback (impl->device, impl->inBuf, (CFIndex) sizeof (impl->inBuf), nullptr, nullptr);
        IOHIDDeviceUnscheduleFromRunLoop (impl->device, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
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
    // Το Komplete Kontrol / Maschine (NIHostIntegrationAgent) μπορεί να αλλάξει τη λειτουργία του πληκτρολογίου. Ξαναστέλνουμε το
    // [0xa0, 0, 0] (light-guide mode) κάθε 1 s, ώστε να ξαναπαίρνουν ισχύ τα φώτα του Flow. ΠΕΙΡΑΜΑΤΙΚΟ: δεν έχει δοκιμαστεί σε S88 MK2.
    const auto nowMs = juce::Time::getMillisecondCounter();
    if (nowMs - lastInitMs >= 1000)
    {
        lastInitMs = nowMs;
        const uint8_t init[3] = { 0xa0, 0x00, 0x00 };
        IOHIDDeviceSetReport (impl->device, kIOHIDReportTypeOutput, init[0], init, sizeof (init));
    }
    std::vector<uint8_t> data ((size_t) keyCount + 1, 0);
    data[0] = 0x81;
    for (int i = 0; i < keyCount && i < count; ++i) data[(size_t) i + 1] = keys[i];
    const IOReturn r = IOHIDDeviceSetReport (impl->device, kIOHIDReportTypeOutput, data[0], data.data(), (CFIndex) data.size());
    if (r != kIOReturnSuccess)
    {
        status = "write failed 0x" + juce::String::toHexString ((int) r);
        return false;
    }
    return true;
#else
    juce::ignoreUnused (keys, count);
    return false;
#endif
}

// Καταγράφει μόνο ΑΛΛΑΓΜΕΝΑ bytes ενός report (id = πρώτο byte), με όριο ρυθμού και συνολικό όριο, ώστε να μη γεμίσει το log.
// Αντιστοίχιση bits → αριθμός κουμπιού, βάσει της καταγραφής του χρήστη (S88 MK2, report 0x01, 32 bytes). 1 = πάτημα encoder, 2/3/4/5 = πάνω/κάτω/αριστερά/δεξιά
static const struct { int byteIx; uint8_t mask; int ctl; } kS88Map[] = {
    { 6, 0x08, 1 }, { 6, 0x20, 2 }, { 6, 0x40, 3 }, { 6, 0x10, 4 }, { 6, 0x80, 5 },
    { 5, 0x04, 6 }, { 5, 0x02, 7 }, { 5, 0x01, 8 }, { 5, 0x10, 9 }, { 5, 0x20, 10 }, { 5, 0x08, 11 },
    { 2, 0x80, 12 }, { 2, 0x08, 13 }, { 2, 0x04, 14 }, { 2, 0x40, 15 }, { 2, 0x02, 16 }, { 2, 0x01, 17 }, { 2, 0x20, 18 },
    { 3, 0x08, 19 }, { 3, 0x04, 20 }, { 2, 0x10, 21 }, { 3, 0x02, 22 }, { 3, 0x01, 23 },
    { 4, 0x01, 24 }, { 4, 0x02, 25 },
    { 3, 0x10, 26 }, { 3, 0x40, 27 }, { 3, 0x80, 28 }, { 3, 0x20, 29 },
    { 4, 0x04, 30 }, { 4, 0x08, 31 }, { 4, 0x10, 32 }, { 4, 0x40, 33 }, { 4, 0x20, 34 },
    { 1, 0x10, 35 }, { 1, 0x20, 36 }, { 1, 0x40, 37 }, { 1, 0x80, 38 }, { 1, 0x01, 39 }, { 1, 0x02, 40 }, { 1, 0x04, 41 }, { 1, 0x08, 42 },
};

void KeyLights::logInput (const uint8_t* data, int len)
{
    if (data != nullptr && len >= 31 && data[0] == 0x01 && onControl)
    {
        // Το πληκτρολόγιο στέλνει report μόνο όταν αλλάζει κάτι: το πρώτο report μετά το άνοιγμα είναι συνήθως ήδη το πρώτο πάτημα.
        // Γι' αυτό, αν δεν υπάρχει προηγούμενο report, τα κουμπιά συγκρίνονται με «όλα ελεύθερα» (αλλιώς το πρώτο πάτημα χανόταν)· ο encoder δεν έχει γνωστή αφετηρία και αγνοείται.
        {
            for (const auto& m : kS88Map)
            {
                const bool was = ctlHave && (ctlPrev[m.byteIx] & m.mask) != 0, now = (data[m.byteIx] & m.mask) != 0;
                if (was != now) onControl (m.ctl, now ? 1 : 0);
            }
            if (ctlHave)
            {
                const int d = (((int) (data[30] & 0x0f) - (int) (ctlPrev[30] & 0x0f) + 8) & 15) - 8;   // μετρητής 4 bit: + = δεξιά
                if (d != 0) onControl (100, d);
            }
        }
        for (int i = 0; i < 31; ++i) ctlPrev[i] = data[i];
        ctlHave = true;
    }
    if (! inputLogger || data == nullptr || len <= 0) return;
    const int id = data[0];
    const int n = juce::jmin (len, 64);
    const auto now = juce::Time::getMillisecondCounter();
    if (now - lastLogMs >= 1000) { lastLogMs = now; logBudget = 25; }   // έως 25 γραμμές ανά δευτερόλεπτο
    if (logBudget <= 0 || totalLogged >= 1500) return;

    juce::String diff;
    const bool first = lastLen[id] == 0;
    for (int i = 1; i < n; ++i)
        if (first || lastReport[id][i] != data[i])
            diff << " [" << i << "]=" << juce::String::toHexString (data[i]);
    for (int i = 0; i < n; ++i) lastReport[id][i] = data[i];
    lastLen[id] = n;
    if (diff.isEmpty()) return;
    --logBudget; ++totalLogged;
    inputLogger ("s88 hid in: id=0x" + juce::String::toHexString (id) + " len=" + juce::String (len) + diff);
}
