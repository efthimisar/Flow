#pragma once

#include <juce_graphics/juce_graphics.h>

// Στιγμιότυπο ενός παραθύρου της εφαρμογής όπως το δείχνει το σύστημα (macOS). Το createComponentSnapshot της JUCE δεν "βλέπει"
// τα native παράθυρα των plugin και βγάζει μαύρο. Επιστρέφει άκυρη εικόνα αν δεν βρεθεί το παράθυρο.
juce::Image flowCaptureWindow (const juce::String& windowTitle);
