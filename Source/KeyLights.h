#pragma once

#include <juce_core/juce_core.h>
#include <memory>

// Φώτα πλήκτρων του Native Instruments Komplete Kontrol (S-series MK2) μέσω USB HID (η υλοποίηση είναι στο KeyLights.cpp).
// Πρωτόκολλο βάσει των έργων SynthesiaKontrol (sonic74 / ojacques, MIT) και KompleteKontrolLightGuideSongConfigs (MIT):
//   vendor 0x17cc, S88 MK2 = 0x1630. Πρώτα [0xa0, 0, 0] (light-guide mode), μετά [0x81, 88 bytes], ένα byte ανά πλήκτρο (A0 = 0).
// Το byte είναι hue * 4 + φωτεινότητα (0..3), 0 = σβηστό. Δεν έχει δοκιμαστεί σε πραγματικό S88 MK2 από τον προγραμματιστή.
class KeyLights
{
public:
    KeyLights();
    ~KeyLights();

    bool isOn() const { return active; }
    juce::String getStatus() const { return status; }
    bool start();
    void stop();
    // keys[0] = A0 (νότα 21)
    bool sendKeys (const uint8_t* keys, int count);
    int getKeyCount() const { return keyCount; }
    int getFirstNote() const { return offset; }

private:
    bool fail (const juce::String& why);

    struct Impl;
    std::unique_ptr<Impl> impl;
    bool active = false;
    int keyCount = 88, offset = 21;
    juce::String status;
};
