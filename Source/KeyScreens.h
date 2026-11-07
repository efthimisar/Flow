#pragma once

#include <juce_core/juce_core.h>
#include <functional>
#include <memory>
#include <vector>

// Οι δύο έγχρωμες οθόνες (480x272, BGR565 big-endian) του Komplete Kontrol S-series MK2 μέσω USB (interface 3, bulk out 0x03).
// Πρωτόκολλο όπως καταγράφεται στο έργο Encdr (nullobject/Encdr, GPL-3.0): ένα bulk transfer = [20-byte header][480*272*2 bytes][8-byte footer],
// με το byte[2] του header να διαλέγει οθόνη (0 = αριστερά, 1 = δεξιά). Η υλοποίηση είναι στο KeyScreens.cpp (macOS, IOUSBLib).
// Πρώτα δοκιμάζεται ο agent της NI (CFMessagePort, όπως στο Encdr)· αν δεν συνδεθεί, πέφτει στο άμεσο USB.
// ΠΕΙΡΑΜΑΤΙΚΟ: δεν έχει δοκιμαστεί σε πραγματικό S88 MK2 από τον προγραμματιστή.
class KeyScreens
{
public:
    static constexpr int width = 480, height = 272;
    static constexpr int frameBytes = width * height * 2;

    KeyScreens();
    ~KeyScreens();

    bool isOn() const { return active; }
    juce::String getStatus() const { return status; }
    std::function<void (const juce::String&)> logger;   // καλείται από οποιοδήποτε νήμα

    bool start();
    void stop();
    // pixels = 480*272 λέξεις 16 bit, ήδη σε BGR565 big-endian (261.120 bytes). screen: 0 = αριστερά, 1 = δεξιά. Κρατά μόνο το πιο πρόσφατο καρέ.
    void reclaim();   // ξαναπαίρνει τις οθόνες (όταν τις έχει πάρει άλλος πελάτης, π.χ. το plugin Komplete Kontrol)
    void submit (int screen, const uint8_t* pixels);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    bool active = false;
    juce::String status;
    void log (const juce::String& s) { if (logger) logger ("screens: " + s); }
};
