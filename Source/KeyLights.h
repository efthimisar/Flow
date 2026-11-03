#pragma once

#include <juce_core/juce_core.h>
#include <memory>
#include <functional>

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
    // Διαγνωστικό: καταγράφει (μόνο ανάγνωση) τα HID reports που στέλνει το πληκτρολόγιο (κουμπιά, 4D encoder, joystick). Καλείται στο κύριο νήμα.
    void setInputLogger (std::function<void (const juce::String&)> f) { inputLogger = std::move (f); }
    void logInput (const uint8_t* data, int len);
    // Χειριστήρια του S88 MK2 (HID report 0x01): ctl 1..50 = κουμπιά όπως αριθμήθηκαν στην εικόνα του χρήστη (value 1 = πάτημα, 0 = άφημα), ctl 100 = περιστροφή encoder (value = ±βήματα)
    std::function<void (int ctl, int value)> onControl;

private:
    bool fail (const juce::String& why);

    struct Impl;
    std::unique_ptr<Impl> impl;
    bool active = false;
    int keyCount = 88, offset = 21;
    juce::uint32 lastInitMs = 0;   // πότε στάλθηκε τελευταία φορά το [0xa0, 0, 0] (light-guide mode)
    juce::String status;
    std::function<void (const juce::String&)> inputLogger;
    uint8_t lastReport[256][64] = {};
    int lastLen[256] = {};
    juce::uint32 lastLogMs = 0;
    int logBudget = 0, totalLogged = 0;
    uint8_t ctlPrev[32] = {};
    bool ctlHave = false;
public:
    void armInputLog() { logBudget = 0; totalLogged = 0; }
};
