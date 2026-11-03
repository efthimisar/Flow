// ΠΡΟΣΟΧΗ: το IOKit ορίζει δικό του "Point" (MacTypes.h) που συγκρούεται με το juce::Point. Γι' αυτό είναι σε ξεχωριστό αρχείο, χωρίς "using namespace juce".
#include "KeyScreens.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#if JUCE_MAC
 #include <IOKit/IOKitLib.h>
 #include <IOKit/IOCFPlugIn.h>
 #include <IOKit/usb/IOUSBLib.h>
#endif

struct KeyScreens::Impl
{
#if JUCE_MAC
    IOUSBDeviceInterface300** dev = nullptr;
    IOUSBInterfaceInterface300** intf = nullptr;
    UInt8 pipe = 0;
#endif
    std::thread worker;
    std::mutex m;
    std::condition_variable cv;
    std::vector<uint8_t> pending[2];
    bool has[2] = { false, false };
    std::atomic<bool> quit { false };
    int sent = 0, failed = 0;
};

KeyScreens::KeyScreens() : impl (new Impl) {}
KeyScreens::~KeyScreens() { stop(); }

#if JUCE_MAC
static juce::String hex (int v) { return "0x" + juce::String::toHexString (v); }

// Βρίσκει το interface 3 (οθόνες) του S88/S61/S49 MK2 και ανοίγει τη συσκευή· επιστρέφει κενό string αν πέτυχε
static juce::String openScreenInterface (IOUSBDeviceInterface300**& devOut, IOUSBInterfaceInterface300**& intfOut, UInt8& pipeOut, const std::function<void (const juce::String&)>& say)
{
    const int pids[3] = { 0x1630, 0x1620, 0x1610 };
    for (int pid : pids)
    {
        CFMutableDictionaryRef match = IOServiceMatching ("IOUSBHostDevice");
        if (match == nullptr) return "IOServiceMatching failed";
        int vid = 0x17cc, p = pid;
        CFNumberRef vn = CFNumberCreate (kCFAllocatorDefault, kCFNumberIntType, &vid);
        CFNumberRef pn = CFNumberCreate (kCFAllocatorDefault, kCFNumberIntType, &p);
        CFDictionarySetValue (match, CFSTR ("idVendor"), vn);
        CFDictionarySetValue (match, CFSTR ("idProduct"), pn);
        CFRelease (vn); CFRelease (pn);

        io_iterator_t it = 0;
        if (IOServiceGetMatchingServices (0, match, &it) != KERN_SUCCESS || it == 0) continue;   // το match καταναλώνεται

        io_service_t svc = IOIteratorNext (it);
        IOObjectRelease (it);
        if (svc == 0) continue;
        say ("USB device found, pid " + hex (pid));

        IOCFPlugInInterface** plug = nullptr; SInt32 score = 0;
        const kern_return_t kr = IOCreatePlugInInterfaceForService (svc, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score);
        IOObjectRelease (svc);
        if (kr != KERN_SUCCESS || plug == nullptr) return "device plug-in failed " + hex ((int) kr);

        IOUSBDeviceInterface300** dev = nullptr;
        const HRESULT qr = (*plug)->QueryInterface (plug, CFUUIDGetUUIDBytes (kIOUSBDeviceInterfaceID300), (LPVOID*) &dev);
        (*plug)->Release (plug);
        if (qr != S_OK || dev == nullptr) return "device interface query failed " + hex ((int) qr);

        IOUSBFindInterfaceRequest req;
        req.bInterfaceClass = kIOUSBFindInterfaceDontCare;
        req.bInterfaceSubClass = kIOUSBFindInterfaceDontCare;
        req.bInterfaceProtocol = kIOUSBFindInterfaceDontCare;
        req.bAlternateSetting = kIOUSBFindInterfaceDontCare;
        io_iterator_t iit = 0;
        if ((*dev)->CreateInterfaceIterator (dev, &req, &iit) != kIOReturnSuccess || iit == 0)
        { (*dev)->Release (dev); return "CreateInterfaceIterator failed"; }

        juce::String seen;
        io_service_t isvc;
        while ((isvc = IOIteratorNext (iit)) != 0)
        {
            IOCFPlugInInterface** iplug = nullptr; SInt32 sc = 0;
            const kern_return_t ikr = IOCreatePlugInInterfaceForService (isvc, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID, &iplug, &sc);
            IOObjectRelease (isvc);
            if (ikr != KERN_SUCCESS || iplug == nullptr) { seen << "?; "; continue; }
            IOUSBInterfaceInterface300** intf = nullptr;
            const HRESULT iq = (*iplug)->QueryInterface (iplug, CFUUIDGetUUIDBytes (kIOUSBInterfaceInterfaceID300), (LPVOID*) &intf);
            (*iplug)->Release (iplug);
            if (iq != S_OK || intf == nullptr) { seen << "?; "; continue; }

            UInt8 num = 0;
            (*intf)->GetInterfaceNumber (intf, &num);
            seen << (int) num << "; ";
            if (num != 3) { (*intf)->Release (intf); continue; }

            IOReturn orr = (*intf)->USBInterfaceOpen (intf);
            if (orr != kIOReturnSuccess)
            {
                say ("interface 3 open failed " + hex ((int) orr) + " (exclusive access = another driver/app has it); trying seize");
                orr = (*intf)->USBInterfaceOpenSeize (intf);
            }
            if (orr != kIOReturnSuccess)
            {
                (*intf)->Release (intf); IOObjectRelease (iit); (*dev)->Release (dev);
                return "interface 3 open failed " + hex ((int) orr);
            }

            UInt8 nep = 0;
            (*intf)->GetNumEndpoints (intf, &nep);
            UInt8 found = 0; juce::String eps;
            for (UInt8 i = 1; i <= nep; ++i)
            {
                UInt8 dir = 0, number = 0, ttype = 0, interval = 0; UInt16 mps = 0;
                if ((*intf)->GetPipeProperties (intf, i, &dir, &number, &ttype, &mps, &interval) == kIOReturnSuccess)
                {
                    eps << "pipe " << (int) i << " dir " << (int) dir << " ep " << (int) number << " type " << (int) ttype << " mps " << (int) mps << "; ";
                    if (dir == kUSBOut && ttype == kUSBBulk && number == 3) found = i;
                }
            }
            say ("interface 3 open OK, endpoints: " + eps);
            if (found == 0)
            {
                (*intf)->USBInterfaceClose (intf); (*intf)->Release (intf); IOObjectRelease (iit); (*dev)->Release (dev);
                return "bulk out endpoint 3 not found";
            }
            IOObjectRelease (iit);
            devOut = dev; intfOut = intf; pipeOut = found;
            return {};
        }
        IOObjectRelease (iit);
        (*dev)->Release (dev);
        return "interface 3 not found (interfaces: " + seen + ")";
    }
    return "No Komplete Kontrol MK2 found on USB";
}
#endif

bool KeyScreens::start()
{
#if JUCE_MAC
    if (active) return true;
    status = {};
    const auto err = openScreenInterface (impl->dev, impl->intf, impl->pipe, [this] (const juce::String& s) { log (s); });
    if (err.isNotEmpty()) { status = err; log (err); stop(); return false; }

    impl->quit = false;
    impl->sent = impl->failed = 0;
    impl->has[0] = impl->has[1] = false;
    impl->worker = std::thread ([this]
    {
        std::vector<uint8_t> buf;
        for (;;)
        {
            int screen = -1;
            {
                std::unique_lock<std::mutex> lk (impl->m);
                impl->cv.wait (lk, [this] { return impl->quit.load() || impl->has[0] || impl->has[1]; });
                if (impl->quit.load()) return;
                screen = impl->has[0] ? 0 : 1;
                buf.clear();
                static const uint8_t hdr[20] = { 0x84, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xe0, 0x01, 0x10, 0x00, 0x00, 0xff, 0x00 };
                static const uint8_t ftr[8] = { 0x03, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00 };
                buf.insert (buf.end(), hdr, hdr + 20);
                buf[2] = (uint8_t) screen;
                buf.insert (buf.end(), impl->pending[screen].begin(), impl->pending[screen].end());
                buf.insert (buf.end(), ftr, ftr + 8);
                impl->has[screen] = false;
            }
            // κομμάτια πολλαπλάσια των 512 bytes (μέγιστο πακέτο USB high-speed), ώστε το τελευταίο κοντό πακέτο να σημαίνει το τέλος του transfer
            const UInt32 chunk = 32768;
            IOReturn r = kIOReturnSuccess;
            for (size_t off = 0; off < buf.size() && r == kIOReturnSuccess; off += chunk)
            {
                const UInt32 n = (UInt32) std::min<size_t> (chunk, buf.size() - off);
                r = (*impl->intf)->WritePipeTO (impl->intf, impl->pipe, buf.data() + off, n, 1000, 1000);
            }
            if (r == kIOReturnSuccess) { if (++impl->sent == 1) log ("first frame sent OK"); }
            else if (++impl->failed <= 3) log ("write failed " + hex ((int) r));
        }
    });

    active = true;
    status = "OK (2 screens 480x272)";
    log (status);
    return true;
#else
    status = "macOS only";
    return false;
#endif
}

void KeyScreens::stop()
{
#if JUCE_MAC
    {
        std::lock_guard<std::mutex> lk (impl->m);
        impl->quit = true;
    }
    impl->cv.notify_all();
    if (impl->worker.joinable()) impl->worker.join();
    if (impl->intf != nullptr)
    {
        (*impl->intf)->USBInterfaceClose (impl->intf);
        (*impl->intf)->Release (impl->intf);
        impl->intf = nullptr;
    }
    if (impl->dev != nullptr)
    {
        (*impl->dev)->Release (impl->dev);
        impl->dev = nullptr;
    }
#endif
    active = false;
}

void KeyScreens::submit (int screen, const uint8_t* pixels)
{
    if (! active || screen < 0 || screen > 1 || pixels == nullptr) return;
    {
        std::lock_guard<std::mutex> lk (impl->m);
        impl->pending[screen].assign (pixels, pixels + frameBytes);
        impl->has[screen] = true;
    }
    impl->cv.notify_one();
}
