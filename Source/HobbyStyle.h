#pragma once
// HobbyStyle — κοινή αισθητική για τα plugins του HobbyAudio (JUCE 8).
// Βλ. DESIGN.md. Χρειάζεται στο CMake: juce_add_binary_data(... SOURCES Resources/Carlito-Bold.ttf)
#include <juce_audio_utils/juce_audio_utils.h>

namespace hobby
{
    // ---- χρώματα ----
    inline const juce::Colour kBodyTop     { 0xfff6f1ea };
    inline const juce::Colour kBodyBottom  { 0xffe8e0d6 };
    inline const juce::Colour kScreen      { 0xff262220 };   // σκούρη οθόνη
    inline const juce::Colour kStripScreen { 0xff3a3531 };   // σκούρη λωρίδα κουμπιών
    inline const juce::Colour kGridLine    { 0xff3a3430 };
    inline const juce::Colour kText        { 0xfff3f4f6 };
    inline const juce::Colour kInk         { 0xff4a4038 };   // σύμβολα στα πετάλια
    inline const juce::Colour kIconGrey    { 0xffb7bbc6 };
    inline const juce::Colour kOn          { 0xff7fcf98 };
    inline const juce::Colour kOff         { 0xfff0766f };
    inline const juce::Colour kSage        { 0xff9ad4a8 };
    inline const juce::Colour kLilac       { 0xffc4aef0 };
    inline const juce::Colour kPeach       { 0xfff5b88a };
    inline const juce::Colour kCoral       { 0xffff8f84 };
    inline const juce::Colour kRed         { 0xfff26b6b };
    inline const juce::Colour kButter      { 0xfff5de8a };
    inline const juce::Colour kBlue        { 0xff8fc8f0 };
    inline const juce::Colour kRose        { 0xfff4a7b9 };
    inline const juce::Colour kRingOff     { 0xffddd4c8 };
    inline const juce::Colour kMuted       { 0xff3a3d46 };

    inline juce::Colour mix (juce::Colour a, juce::Colour b, float t) { return a.interpolatedWith (b, juce::jlimit (0.0f, 1.0f, t)); }

    // 1 στην αρχή κάθε κύκλου (phase σε κύκλους), μαλακή πτώση — για αναβόσβημα
    float pulse (double phase);

    // γραμματοσειρά Carlito Bold από το BinaryData (Resources/Carlito-Bold.ttf)
    juce::Typeface::Ptr loadTypeface();
    juce::Font makeFont (const juce::Typeface::Ptr&, float height);

    // ---- ζωγραφιά ----
    void drawBody (juce::Graphics&, juce::Rectangle<int> bounds);                        // κρεμ βάση
    void drawBrandMark (juce::Graphics&, const juce::Typeface::Ptr&, int windowWidth);   // "π/-" πάνω στο κέντρο
    inline float kBrandY = 50.0f;   // ύψος του Flow και του π (ορίζεται από τη γεωμετρία του παραθύρου)
    void drawEmbossedText (juce::Graphics&, const juce::Typeface::Ptr&, const juce::String&, float x);   // ανάγλυφο γράμμα στο χρώμα του φόντου (αριστερά, πάνω)
    // ανάγλυφο πλαίσιο με βαθουλωμένη οθόνη (οθόνη: innerShadow ~18, λωρίδα: ~15)
    void drawBezel (juce::Graphics&, juce::Rectangle<float> bezel, juce::Rectangle<float> screen,
                    juce::Colour screenColour, float innerShadow);
    void drawGlass (juce::Graphics&, juce::Rectangle<int> screen);   // ελαφρύ τζάμι πάνω από οθόνη (προσωρινή εικόνα, ζωγραφίζεται μία φορά ανά μέγεθος)
    void drawTileShadow (juce::Graphics&, juce::Rectangle<float> tile);                  // σκιά κάτω από τετράγωνο κουμπί
}

// Σύμβολα που ζωγραφίζονται με διανύσματα. Πρόσθεσε νέα στο τέλος του enum και στο switch του drawSymbol.
enum class Symbol { PadRecord, PadOverdub, Play, Stop, Cancel, Undo,
                    StartQ, LengthQ, Snap, Loop, Sync, Tempo, Thru,
                    Block, Learn, Vst, Window, Hold, Arp, Folder, Fx, Chord, Mute, Tape, Orb, Speaker, Sliders, Rings, Follow, Menu,
                    Dice, Camera };

void drawSymbol (juce::Graphics&, Symbol, juce::Point<float> centre, float size, juce::Colour);

// Επίπεδο στρογγυλό πετάλι: setComponentID("footswitch"), property "symbol" (int Symbol) και "ring" (int ARGB δακτυλίου).
// Πετάλι 100x100 px: δακτύλιος 92 px, λευκό πρόσωπο 80 px.
class HobbyLookAndFeel : public juce::LookAndFeel_V4
{
public:
    HobbyLookAndFeel();
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool, bool) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool, bool) override;
    // μπαλονάκια βοήθειας: κάτω στο κέντρο του παραθύρου, όχι δίπλα στο ποντίκι
    juce::Rectangle<int> getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos, juce::Rectangle<int> parentArea) override
    {
        auto r = juce::LookAndFeel_V4::getTooltipBounds (tipText, screenPos, parentArea);
        auto area = parentArea;
        if (auto* c = juce::Desktop::getInstance().getMainMouseSource().getComponentUnderMouse())
            if (auto* top = c->getTopLevelComponent())
                area = top->getScreenBounds();
        r.setPosition (area.getCentreX() - r.getWidth() / 2, area.getBottom() - r.getHeight() - 14);
        return r.constrainedWithin (parentArea);
    }
};

// Κουμπί Sound Palette: ένα ορθογώνιο με δύο μισά. Αριστερά το ζάρι (Palette), δεξιά η κάμερα (κράτα τον ήχο του επιλεγμένου block).
// Η κάμερα ανάβει πράσινη για λίγο όταν αποθηκευτεί ήχος.
class PaletteTile : public juce::Component, public juce::TooltipClient, private juce::Timer
{
public:
    std::function<void()> onDice, onCamera;
    juce::String diceTip, cameraTip;
    void setDiceOn (bool b)   { if (b != diceOn) { diceOn = b; repaint(); } }
    void setCameraOn (bool b) { if (b != camOn) { camOn = b; repaint(); } }
    void flashCamera()        { flash = true; repaint(); startTimer (400); }
    juce::String getTooltip() override { return hoverRight ? cameraTip : diceTip; }
    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent& e) override { hoverRight = e.x >= getWidth() / 2; }
    void mouseMove (const juce::MouseEvent& e) override  { hoverRight = e.x >= getWidth() / 2; }
    void mouseDown (const juce::MouseEvent& e) override  { downHalf = e.x >= getWidth() / 2 ? 2 : 1; repaint(); }
    void mouseUp (const juce::MouseEvent& e) override
    {
        const int half = downHalf;
        downHalf = 0;
        repaint();
        if (half == 0 || ! contains (e.getPosition())) return;
        if ((e.x >= getWidth() / 2 ? 2 : 1) != half) return;
        if (half == 1) { if (onDice) onDice(); }
        else if (onCamera) onCamera();
    }
private:
    void timerCallback() override { flash = false; stopTimer(); repaint(); }
    bool diceOn = false, camOn = true, flash = false, hoverRight = false;
    int downHalf = 0;
};

// Τετράγωνο κουμπί 52x52 χωρίς μενού: κάθε πάτημα → επόμενη τιμή, μετά την τελευταία ξανά η πρώτη.
// Αν isOnOff: το σύμβολο είναι πράσινο στο index 1 (On) και κόκκινο στο index 0 (Off).
// Ένα ορθογώνιο κουμπί με στρογγυλεμένες άκρες και δύο μισά: αριστερά ORB (σφαίρα → παράθυρο), δεξιά REC motion (τελεία).
// Όλα άσπρα όταν είναι σβηστά, πράσινα όταν είναι ενεργά.
class OrbRecTile : public juce::Component, public juce::SettableTooltipClient
{
public:
    OrbRecTile() { setMouseCursor (juce::MouseCursor::PointingHandCursor); }
    void setState (int orbStage, bool recOn) { if (orbStage != stage || recOn != rec) { stage = orbStage; rec = recOn; repaint(); } }
    void setTips (const juce::String& orbTip, const juce::String& recTip) { tipOrb = orbTip; tipRec = recTip; }
    juce::String getTooltip() override { return hoverRight ? tipRec : tipOrb; }
    std::function<void()> onOrb, onRec;

    void mouseMove (const juce::MouseEvent& e) override { const bool r = e.x >= getWidth() / 2; if (r != hoverRight || ! hover) { hoverRight = r; hover = true; repaint(); } }
    void mouseExit (const juce::MouseEvent&) override { hover = false; down = 0; repaint(); }
    void mouseDown (const juce::MouseEvent& e) override { down = e.x >= getWidth() / 2 ? 2 : 1; repaint(); }
    void mouseUp (const juce::MouseEvent& e) override
    {
        const int d = down; down = 0; repaint();
        if (! getLocalBounds().contains (e.getPosition())) return;
        if (d == 1 && onOrb) onOrb();
        else if (d == 2 && onRec) onRec();
    }
    void paint (juce::Graphics& g) override;

private:
    int stage = 0, down = 0;
    bool rec = false, hover = false, hoverRight = false;
    juce::String tipOrb, tipRec;
};

class ValueTile : public juce::Button
{
public:
    ValueTile (Symbol s, juce::Typeface::Ptr face) : juce::Button ({}), symbol (s), typeface (face) {}
    void setValues (const juce::StringArray& v, bool onOff) { labels = v; isOnOff = onOff; repaint(); }
    void setIndex (int i) { if (i != index) { index = i; repaint(); } }
    // προαιρετικό χρώμα συμβόλου (π.χ. πράσινο όταν η λειτουργία είναι ενεργή)
    void setIconColour (juce::Colour c) { if (! hasOverride || c != iconOverride) { iconOverride = c; hasOverride = true; repaint(); } }
    void setRound (bool r) { round = r; repaint(); }
    void setSymbol (Symbol s) { if (s != symbol) { symbol = s; repaint(); } }
    void setNumber (int n, bool filled) { if (n != number || filled != numberFilled) { number = n; numberFilled = filled; repaint(); } }   // αριθμός μέσα σε κύκλο (0 = κανονικό σύμβολο)
    void setDots (int n) { if (n != dots) { dots = n; repaint(); } }   // τελείες κάτω από το εικονίδιο (π.χ. 1 = Mono, 2 = Poly)
    std::function<void()> onRightClick;   // δεξί κλικ / ctrl-κλικ
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) { if (onRightClick) onRightClick(); return; }
        juce::Button::mouseDown (e);
    }
    int getIndex() const { return index; }
    int getNumValues() const { return labels.size(); }
    void paintButton (juce::Graphics&, bool isHighlighted, bool isDown) override;
private:
    Symbol symbol;
    juce::Typeface::Ptr typeface;
    juce::StringArray labels;
    bool isOnOff = false;
    bool round = false;
    bool hasOverride = false;
    juce::Colour iconOverride;
    int index = 0;
    int dots = 0;
    int number = 0;
    bool numberFilled = false;
};
