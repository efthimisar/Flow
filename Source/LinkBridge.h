#pragma once
// Λεπτό περιτύλιγμα για το Ableton Link, ώστε οι κεφαλίδες του Link να μένουν έξω από τον κώδικα JUCE.
#include <memory>

class LinkBridge
{
public:
    LinkBridge();
    ~LinkBridge();

    static bool isAvailable();
    void enable (bool on);
    bool isEnabled() const;
    int  numPeers() const;

    // audio thread: τρέχον tempo και θέση beat (με quantum 4 = μέτρο) για τη στιγμή που θα ακουστεί το buffer
    void capture (double outputLatencySeconds, double& tempo, double& beat);
    // οποιοδήποτε thread: αλλαγή tempo για όλη τη συνεδρία Link
    void setTempo (double bpm);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
