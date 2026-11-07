// ΠΡΟΣΟΧΗ: το IOKit ορίζει δικό του "Point" (MacTypes.h) που συγκρούεται με το juce::Point. Γι' αυτό είναι σε ξεχωριστό αρχείο, χωρίς "using namespace juce".
#include "KeyScreens.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <mutex>
#include <thread>

#if JUCE_MAC
 #include <CoreFoundation/CoreFoundation.h>
 #include <IOKit/IOKitLib.h>
 #include <IOKit/IOCFPlugIn.h>
 #include <IOKit/usb/IOUSBLib.h>
#endif

#if JUCE_MAC
// ---- Δρόμος μέσω του agent της NI (NIHostIntegrationAgent), με CFMessagePort ----
// Πρωτόκολλο όπως στο Encdr (RufusIbiza/Encdr, GPL-3.0-or-later, αρχεία usb/ni_service/protocol.rs και macos.rs). Τα bytes του καρέ είναι τα ίδια με το USB bulk transfer.
// Το Encdr το έχει επαληθεύσει σε πραγματικό υλικό (Maschine Mk3, macOS) και το δηλώνει για το Komplete Kontrol S-series MK2· για το S88 MK2 δεν έχει δοκιμαστεί από εμάς.
// Οι υπηρεσίες της NI πρέπει να τρέχουν· αν δεν τρέχουν ή η σύνδεση αποτύχει, το KeyScreens πέφτει στο άμεσο USB.
namespace niservice
{
    static const uint32_t kTrue = 0x74727565, kClientKind = 0x4e694d32, kClientRole = 0x70726d79;
    static const uint32_t kMsgDeviceConnect = 0x03447500, kMsgInstanceConnect = 0x03444900, kMsgAckNotification = 0x03404300,
                          kMsgDeviceState = 0x03447143, kMsgDisplay = 0x03647344, kNoteDeviceOn = 0x03444e2b;

    using Bytes = std::vector<uint8_t>;
    static void putW (Bytes& v, uint32_t w) { for (int i = 0; i < 4; ++i) v.push_back ((uint8_t) (w >> (8 * i))); }
    static Bytes words (std::initializer_list<uint32_t> ws) { Bytes v; for (auto w : ws) putW (v, w); return v; }
    static bool le32 (const Bytes& d, size_t at, uint32_t& out)
    {
        if (at + 4 > d.size()) return false;
        out = (uint32_t) d[at] | ((uint32_t) d[at + 1] << 8) | ((uint32_t) d[at + 2] << 16) | ((uint32_t) d[at + 3] << 24);
        return true;
    }
    static std::string text (const Bytes& d, size_t at, size_t len)
    {
        if (at + len > d.size()) return {};
        std::string s ((const char*) d.data() + at, len);
        while (! s.empty() && s.back() == '\0') s.pop_back();
        return s;
    }

    struct Port
    {
        CFMessagePortRef p = nullptr;
        ~Port() { close(); }
        void close() { if (p != nullptr) { CFMessagePortInvalidate (p); CFRelease (p); p = nullptr; } }
        bool open (const std::string& name, int attempts)
        {
            CFStringRef n = CFStringCreateWithCString (kCFAllocatorDefault, name.c_str(), kCFStringEncodingUTF8);
            for (int a = 0; a < attempts && p == nullptr; ++a)
            {
                p = CFMessagePortCreateRemote (kCFAllocatorDefault, n);
                if (p == nullptr && a + 1 < attempts) std::this_thread::sleep_for (std::chrono::milliseconds (200));
            }
            CFRelease (n);
            return p != nullptr;
        }
        // στέλνει ένα μήνυμα· η απάντηση (αν υπάρχει) επιστρέφεται αντιγραμμένη. Επιστρέφει το status του CFMessagePort (0 = επιτυχία)
        int request (const Bytes& msg, double timeoutSecs, Bytes& reply)
        {
            reply.clear();
            CFDataRef data = CFDataCreate (kCFAllocatorDefault, msg.data(), (CFIndex) msg.size());
            if (data == nullptr) return -4;
            CFDataRef rep = nullptr;
            const SInt32 st = CFMessagePortSendRequest (p, 0, data, timeoutSecs, timeoutSecs, kCFRunLoopDefaultMode, &rep);
            CFRelease (data);
            if (rep != nullptr)
            {
                const CFIndex n = CFDataGetLength (rep);
                if (n > 0) reply.assign (CFDataGetBytePtr (rep), CFDataGetBytePtr (rep) + n);
                CFRelease (rep);
            }
            return (int) st;
        }
    };

    struct Queue { std::mutex m; std::vector<Bytes> items; };

    static CFDataRef onNote (CFMessagePortRef, SInt32, CFDataRef data, void* info)
    {
        auto* q = (Queue*) info;
        Bytes b;
        if (data != nullptr) { const CFIndex n = CFDataGetLength (data); if (n > 0) b.assign (CFDataGetBytePtr (data), CFDataGetBytePtr (data) + n); }
        std::lock_guard<std::mutex> lk (q->m);
        q->items.push_back (std::move (b));
        return nullptr;   // κενή απάντηση = επιβεβαίωση
    }

    // θύρα ειδοποιήσεων που ο agent συνδέεται πίσω σε αυτήν· εξυπηρετείται στο δικό της νήμα
    struct NoteServer
    {
        std::thread th;
        std::atomic<bool> stop { false };
        std::atomic<int> ready { 0 };
        ~NoteServer() { stop = true; if (th.joinable()) th.join(); }
        bool start (const std::string& name, Queue* q)
        {
            th = std::thread ([this, name, q]
            {
                CFStringRef n = CFStringCreateWithCString (kCFAllocatorDefault, name.c_str(), kCFStringEncodingUTF8);
                CFMessagePortContext ctx = { 0, q, nullptr, nullptr, nullptr };
                Boolean freeInfo = false;
                CFMessagePortRef port = CFMessagePortCreateLocal (kCFAllocatorDefault, n, onNote, &ctx, &freeInfo);
                CFRelease (n);
                if (port == nullptr) { ready = -1; return; }
                CFRunLoopSourceRef src = CFMessagePortCreateRunLoopSource (kCFAllocatorDefault, port, 0);
                CFRunLoopRef rl = CFRunLoopGetCurrent();
                if (src == nullptr) { CFMessagePortInvalidate (port); CFRelease (port); ready = -1; return; }
                CFRunLoopAddSource (rl, src, kCFRunLoopDefaultMode);
                ready = 1;
                while (! stop.load()) CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.2, false);
                CFRunLoopRemoveSource (rl, src, kCFRunLoopDefaultMode);
                CFMessagePortInvalidate (port);
                CFRelease (src);
                CFRelease (port);
            });
            for (int i = 0; i < 100 && ready.load() == 0; ++i) std::this_thread::sleep_for (std::chrono::milliseconds (20));
            return ready.load() == 1;
        }
    };

    struct Session
    {
        Queue queue;
        Port deviceReq, instanceReq;
        std::unique_ptr<NoteServer> deviceNote, instanceNote;
        std::string serial, bootstrapUsed;
        int rejected = 0;

        // «register»: ζητά θύρες από το bootstrap, στήνει τη θύρα ειδοποιήσεων και δηλώνει το όνομά της
        std::string registerWith (const std::string& bootstrapName, const Bytes& request, Port& reqOut, std::unique_ptr<NoteServer>& noteOut)
        {
            Port boot;
            if (! boot.open (bootstrapName, 10)) return "could not open the NI service port '" + bootstrapName + "'";
            Bytes reply;
            const int st = boot.request (request, 2.0, reply);
            if (st != 0) return "the NI service did not answer (status " + std::to_string (st) + ")";
            if (reply.empty()) return "the NI service sent an empty reply";
            boot.close();
            uint32_t w = 0, reqLen = 0, noteLen = 0;
            if (! le32 (reply, 0, w) || w != kTrue) return "the NI service refused the connection";
            if (! le32 (reply, 4, reqLen)) return "short reply";
            const std::string reqName = text (reply, 8, reqLen);
            if (! le32 (reply, 8 + reqLen, noteLen)) return "short reply (notification)";
            const std::string noteName = text (reply, 12 + reqLen, noteLen);
            if (reqName.empty() || noteName.empty()) return "unusable port names in the reply";

            noteOut.reset (new NoteServer());
            if (! noteOut->start (noteName, &queue)) return "could not create the notification port '" + noteName + "'";
            if (! reqOut.open (reqName, 10)) return "could not open the request port '" + reqName + "'";
            Bytes ack = words ({ kMsgAckNotification, kTrue, 0, (uint32_t) noteName.size() });
            ack.insert (ack.end(), noteName.begin(), noteName.end()); ack.push_back (0);
            Bytes ignore;
            reqOut.request (ack, 0.5, ignore);
            return {};
        }

        std::string connectVia (const std::string& bootstrapName, uint32_t pid, const std::function<void (const std::string&)>& say)
        {
            auto err = registerWith (bootstrapName, words ({ kMsgDeviceConnect, pid, kClientKind, kClientRole, 0 }), deviceReq, deviceNote);
            if (! err.empty()) return err;
            Bytes ignore;
            deviceReq.request (words ({ kMsgDeviceState }), 0.5, ignore);

            // περιμένει να αναφέρει ο agent τη συσκευή (serial)
            serial.clear();
            const auto t0 = std::chrono::steady_clock::now();
            while (serial.empty() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds (3))
            {
                {
                    std::lock_guard<std::mutex> lk (queue.m);
                    for (auto& d : queue.items)
                    {
                        uint32_t w = 0, len = 0;
                        if (le32 (d, 0, w) && w == kNoteDeviceOn && le32 (d, 12, len)) { serial = text (d, 16, len); if (! serial.empty()) break; }
                    }
                    queue.items.clear();
                }
                if (serial.empty()) std::this_thread::sleep_for (std::chrono::milliseconds (20));
            }
            if (serial.empty()) return "the NI service did not report a connected device (pid 0x" + juce::String::toHexString ((int) pid).toStdString() + ")";
            say ("NI service reports device " + serial + "; registering for its screens");

            Bytes inst = words ({ kMsgInstanceConnect, pid, kClientKind, kClientRole, (uint32_t) serial.size() + 1 });
            inst.insert (inst.end(), serial.begin(), serial.end()); inst.push_back (0);
            err = registerWith (bootstrapName, inst, instanceReq, instanceNote);
            if (! err.empty()) return err;

            Bytes project = words ({ 0x0349734e, 0x70001006, 0xf6b24000, 5 });
            const char* nm = "Flow";
            project.insert (project.end(), nm, nm + 5);   // με το τελικό NUL
            const std::vector<Bytes> setup = {
                words ({ 0x03566775, 0x4b657973 }), words ({ 0x03566775, 0x524b6579 }), words ({ 0x03436753 }), project,
                words ({ 0x0344674d, pid }), words ({ 0x03434300, 0x73747274 }), words ({ 0x03497354, 0 }), words ({ 0x03436746 }),
                words ({ 0x03497374, kTrue }), words ({ 0x03445200, 2 }), words ({ 0x03444100, 3, 0, 0x04e297b0 }) };
            for (auto& m : setup) instanceReq.request (m, 0.04, ignore);   // κάποια απαντιούνται, άλλα όχι: καμία δεν χρειάζεται
            return {};
        }

        std::string connect (uint32_t pid, const std::function<void (const std::string&)>& say)
        {
            std::string last = "the NI hardware service is not running";
            for (const char* name : { "com.native-instruments.NIHostIntegrationAgent", "NIHWMainHandler" })
            {
                { Port probe; if (! probe.open (name, 1)) continue; }
                bootstrapUsed = name;
                const auto e = connectVia (name, pid, say);
                if (e.empty()) return {};
                say (std::string ("NI service bootstrap '") + name + "' did not connect: " + e);
                last = e;
            }
            return last;
        }

        // στέλνει ένα καρέ (τα ίδια bytes με το USB bulk transfer) στην οθόνη screen· επιστρέφει κενό αν έγινε δεκτό
        std::string sendDisplay (int screen, const Bytes& commands)
        {
            Bytes msg = words ({ kMsgDisplay, (uint32_t) screen, 0, (480u << 16) | 272u, (uint32_t) commands.size() });
            msg.insert (msg.end(), commands.begin(), commands.end());
            Bytes reply;
            const int st = instanceReq.request (msg, 1.5, reply);
            if (st != 0) return "the NI service did not accept a frame (status " + std::to_string (st) + ")";
            uint32_t w = 0;
            if (! le32 (reply, 0, w) || w != kTrue) return "the NI service rejected a frame (another client may hold the screens)";
            return {};
        }
    };
}
#endif

struct KeyScreens::Impl
{
#if JUCE_MAC
    IOUSBDeviceInterface300** dev = nullptr;
    IOUSBInterfaceInterface300** intf = nullptr;
    UInt8 pipe = 0;
    std::unique_ptr<niservice::Session> svc;   // δρόμος μέσω του agent της NI (αν συνδέθηκε)
    uint32_t svcPid = 0;                       // pid της συσκευής για την επανασύνδεση
    int reclaims = 0;
#endif
    std::thread worker;
    std::mutex m;
    std::condition_variable cv;
    std::vector<uint8_t> pending[2];
    bool has[2] = { false, false };
    std::atomic<bool> quit { false };
    std::atomic<bool> handedOver { false };    // οι οθόνες δόθηκαν στο Komplete Kontrol: το Flow δεν στέλνει τίποτα
    std::atomic<bool> reclaimNow { false };    // ζητήθηκε χειροκίνητη επανάκτηση των οθονών (κουμπί MIDI του S88)
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
    // 1ος δρόμος: άμεσο USB (δοκιμασμένο στο S88 MK2, δουλεύει και με τον agent της NI ανοιχτό).
    // 2ος δρόμος, μόνο αν το USB δεν ανοίγει: ο agent της NI. Ο δρόμος του agent δεν έχει επιβεβαιωθεί για το S88 MK2: στη δοκιμή «συνδέθηκε» και δέχτηκε τα καρέ, αλλά οι οθόνες έμειναν στο λογότυπο.
    const auto err = openScreenInterface (impl->dev, impl->intf, impl->pipe, [this] (const juce::String& s) { log (s); });
    if (err.isNotEmpty())
    {
        log ("direct USB not available (" + err + "), trying the NI service route");
        for (uint32_t pid : { 0x1630u, 0x1620u, 0x1610u })
        {
            auto session = std::make_unique<niservice::Session>();
            const auto serr = session->connect (pid, [this] (const std::string& s) { log (juce::String (s)); });
            if (serr.empty()) { impl->svc = std::move (session); impl->svcPid = pid; impl->reclaims = 0; log ("NI service route connected (" + juce::String (impl->svc->bootstrapUsed) + ", pid 0x" + juce::String::toHexString ((int) pid) + ")"); break; }
            log ("NI service route not available for pid 0x" + juce::String::toHexString ((int) pid) + ": " + juce::String (serr));
            if (serr.find ("not running") != std::string::npos) break;
        }
        if (impl->svc == nullptr) { status = err; log (err); stop(); return false; }
    }
    const bool viaService = impl->svc != nullptr;

    impl->quit = false;
    impl->sent = impl->failed = 0;
    impl->has[0] = impl->has[1] = false;
    impl->worker = std::thread ([this]
    {
        std::vector<uint8_t> buf;
        int lastScreen = 1;
        juce::uint32 lastReclaimMs = 0;
        bool lastRejected = false;
        for (;;)
        {
            int screen = -1;
            {
                std::unique_lock<std::mutex> lk (impl->m);
                impl->cv.wait (lk, [this] { return impl->quit.load() || impl->reclaimNow.load() || impl->has[0] || impl->has[1]; });
                if (impl->quit.load()) return;
                if (impl->reclaimNow.exchange (false))
                {
                    // κουμπί MIDI: αν το Komplete Kontrol έχει τις οθόνες (ή τις έχουμε δώσει) → τις παίρνει το Flow, αλλιώς τις δίνει στο Komplete Kontrol
                    const bool take = impl->handedOver.load() || lastRejected;
                    lk.unlock();
                    if (impl->svc != nullptr || impl->handedOver.load())
                    {
                        if (take)
                        {
                            auto fresh = std::make_unique<niservice::Session>();
                            const auto cerr = fresh->connect (impl->svcPid, [] (const std::string&) {});
                            if (cerr.empty()) { impl->svc = std::move (fresh); impl->handedOver = false; lastRejected = false; log ("screens: re-registered by the MIDI button to take the screens back"); }
                            else log (juce::String ("screens: re-register (MIDI button) failed: ") + juce::String (cerr));
                        }
                        else
                        {
                            impl->svc.reset();   // το Flow αποσυνδέεται: ό,τι άλλο πελάτης (το plugin) μπορεί να πάρει τις οθόνες
                            impl->handedOver = true;
                            log ("screens: handed over (MIDI button); press MIDI again to take them back");
                        }
                    }
                    lk.lock();
                    if (! impl->handedOver.load())
                    {
                        if (! impl->pending[0].empty()) impl->has[0] = true;
                        if (! impl->pending[1].empty()) impl->has[1] = true;
                    }
                }
                if (impl->handedOver.load()) { impl->has[0] = impl->has[1] = false; continue; }   // τα καρέ δεν στέλνονται όσο οι οθόνες είναι στο plugin
                if (! impl->has[0] && ! impl->has[1]) continue;
                const int other = 1 - lastScreen;   // εναλλάσσονται οι οθόνες: αλλιώς η αριστερή, που ανανεώνεται συνεχώς, άφηνε τη δεξιά να παίρνει καρέ σπάνια
                screen = impl->has[other] ? other : lastScreen;
                lastScreen = screen;
                buf.clear();
                static const uint8_t hdr[20] = { 0x84, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xe0, 0x01, 0x10, 0x00, 0x00, 0xff, 0x00 };
                static const uint8_t ftr[8] = { 0x03, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00 };
                buf.insert (buf.end(), hdr, hdr + 20);
                buf[2] = (uint8_t) screen;
                buf.insert (buf.end(), impl->pending[screen].begin(), impl->pending[screen].end());
                buf.insert (buf.end(), ftr, ftr + 8);
                impl->has[screen] = false;
            }
            if (impl->svc != nullptr)   // μέσω του agent της NI: ολόκληρο το καρέ ως ένα μήνυμα
            {
                const auto e = impl->svc->sendDisplay (screen, buf);
                lastRejected = ! e.empty();
                if (e.empty()) { if (++impl->sent == 1) log ("first frame sent OK (NI service)"); }
                else
                {
                    if (++impl->failed <= 3) log (juce::String (e));
                    // Άλλος πελάτης (π.χ. το plugin Komplete Kontrol μέσα σε block) πήρε τις οθόνες: ξαναδηλώνεται το Flow ως πελάτης, το πολύ μία φορά ανά 2 s
                    const auto nowMs = juce::Time::getMillisecondCounter();
                    if (nowMs - lastReclaimMs >= 2000)
                    {
                        lastReclaimMs = nowMs;
                        auto fresh = std::make_unique<niservice::Session>();
                        const auto cerr = fresh->connect (impl->svcPid, [] (const std::string&) {});
                        if (cerr.empty())
                        {
                            impl->svc = std::move (fresh);
                            if (++impl->reclaims <= 5) log ("screens: re-registered with the NI service to take the screens back (" + juce::String (impl->reclaims) + ")");
                        }
                        else if (impl->reclaims++ < 5) log ("screens: re-register failed: " + juce::String (cerr));
                    }
                }
                continue;
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
    status = viaService ? "OK (2 screens 480x272, via NI service)" : "OK (2 screens 480x272)";
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
    if (impl->worker.joinable())   // να σταλεί το τελευταίο καρέ (π.χ. το λογότυπο στο κλείσιμο) πριν σταματήσει ο worker
        for (int i = 0; i < 100; ++i)
        {
            { std::lock_guard<std::mutex> lk (impl->m); if (! impl->has[0] && ! impl->has[1]) break; }
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
        }
    {
        std::lock_guard<std::mutex> lk (impl->m);
        impl->quit = true;
    }
    impl->cv.notify_all();
    if (impl->worker.joinable()) impl->worker.join();
    impl->svc.reset();
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

void KeyScreens::reclaim()
{
    if (! active) return;
    impl->reclaimNow = true;
    impl->cv.notify_one();
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
