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
    // σκιά με cache (το θάμπωμα DropShadow ήταν το πιο ακριβό σημείο της ζωγραφικής): ξαναϋπολογίζεται μόνο όταν αλλάζει μέγεθος/χρώμα. kind = σχήμα του path (0 πλαίσιο, 1 πλακίδιο, 2 έλλειψη)
    void drawShadowCached (juce::Graphics&, const juce::DropShadow&, const juce::Path&, int kind);
    void drawBezel (juce::Graphics&, juce::Rectangle<float> bezel, juce::Rectangle<float> screen,
                    juce::Colour screenColour, float innerShadow);
    void drawGlass (juce::Graphics&, juce::Rectangle<int> screen);   // ελαφρύ τζάμι πάνω από οθόνη (προσωρινή εικόνα, ζωγραφίζεται μία φορά ανά μέγεθος)
    void drawTileShadow (juce::Graphics&, juce::Rectangle<float> tile);                  // σκιά κάτω από τετράγωνο κουμπί
}


// ---- Smart master: 6 ζώνες σε 3 ζεύγη λωρίδων (μπορντώ, πορτοκάλι, θερμό κίτρινο), φωτεινό -> σκοτεινό προς το κέντρο ----
namespace hobby
{
    inline const char* const smartBandNames[6] = { "Sub", "Low", "Low-Mid", "Mid", "High-Mid", "High" };
    inline const juce::Colour kSmartOver { 0xfffaf4e4 };   // κρεμ: το κομμάτι πάνω από το όριο
    inline juce::Colour smartLaneColour (int k)
    {
        static const juce::Colour c[6] { juce::Colour (0xffc44054), juce::Colour (0xff842438),    // μπορντώ
                                         juce::Colour (0xffffa040), juce::Colour (0xffcc6e1c),    // πορτοκάλι
                                         juce::Colour (0xffffd65c), juce::Colour (0xffd6a428) };  // θερμό κίτρινο
        return c[juce::jlimit (0, 5, k)];
    }

    // Smart master: 3 δακτύλιοι (μπάσο έξω, μεσαία, πρίμα μέσα), 270° ανοιχτοί κάτω. Γεμίζουν από αριστερά προς τα δεξιά (φορά ρολογιού).
    // Κάθε δακτύλιος δείχνει μια γενική εικόνα δύο ζωνών: γέμισμα ως τη μεγαλύτερη στάθμη, δύο λεπτές έγχρωμες παύλες (μία ανά ζώνη), ένα λευκό όριο.
    // Το γέμισμα είναι γκρι με διαβάθμιση (σκούρο χαμηλά, φωτεινό ψηλά) και διαφορά τόνου από τον εξωτερικό προς τον εσωτερικό δακτύλιο.
    // outerEdge = εξωτερική άκρη του πρώτου δακτυλίου, total = συνολικό πάχος προς τα μέσα. vals/lim = 0..1 ανά ζώνη (lim μόνο όταν active).
    inline juce::Colour smartDashColour (int k)   // μπορντώ, πορτοκάλι, κίτρινο: χαμηλή ζώνη φωτεινότερη, υψηλή πιο σκούρα
    {
        static const juce::uint32 c[6] { 0xffc44054, 0xff962c40, 0xffffa040, 0xffd67824, 0xffffd65c, 0xffdcaa2c };
        return juce::Colour (c[juce::jlimit (0, 5, k)]);
    }
    inline void drawSmartLanes (juce::Graphics& g, float cx, float cy, float outerEdge, float total, const float* vals, const float* lim,
                                bool active, juce::Colour trackC, juce::Colour tickC, juce::Colour blockC)
    {
        (void) total;
        // οι παύλες κάθονται στα κενά ανάμεσα στους κύκλους του drawSmartWidth: ίδιο βήμα (20/118 της εξωτερικής ακτίνας)
        const float step = outerEdge * (20.0f / 118.0f), w = step * 0.65f;
        const float full = juce::degreesToRadians (270.0f), start = juce::degreesToRadians (-135.0f);
        static const int baseGrey[3][3] { { 244, 241, 236 }, { 160, 157, 152 }, { 88, 85, 82 } };
        const float dim = active ? 1.0f : 0.55f;
        float r = outerEdge - step * 0.5f;
        for (int ring = 0; ring < 3; ++ring)
        {
            auto arc = [&] (float f0, float f1, juce::Colour c)
            {
                if (f1 <= f0 + 0.0003f) return;
                juce::Path p; p.addCentredArc (cx, cy, r, r, 0.0f, start + full * f0, start + full * f1, true);
                g.setColour (c); g.strokePath (p, juce::PathStrokeType (w));
            };
            const int k0 = ring * 2, k1 = ring * 2 + 1;
            const float v0 = juce::jlimit (0.0f, 1.0f, vals[k0]), v1 = juce::jlimit (0.0f, 1.0f, vals[k1]);
            const float top = juce::jmax (v0, v1);
            const float lm = active ? juce::jmax (lim[k0], lim[k1]) : 2.0f;   // πιο χαλαρό όριο της ομάδας
            const bool limited = active && lm < 0.999f;
            // κανένας δακτύλιος: μόνο δύο λεπτές παύλες ανά ομάδα, με σβηστή ουρά πίσω τους· κρεμ όταν η ζώνη περνά το όριο
            const float dl = 0.008f, tailLen = 0.40f;
            const int nt = 36;
            for (int j = 0; j < 2; ++j)
            {
                const int k = ring * 2 + j;
                const float v = j == 0 ? v0 : v1;
                // χρώμα του block με τονικές διαφορές: ο εξωτερικός δακτύλιος (μπάσα) πιο σκούρος, ο εσωτερικός (πρίμα) πιο φωτεινός· μέσα στο ζεύγος η χαμηλή ζώνη λίγο φωτεινότερη
                static const float tone[6] { 0.62f, 0.50f, 0.95f, 0.80f, 1.30f, 1.12f };
                const juce::Colour c = (limited && v > lm) ? kSmartOver : (v < 0.004f ? juce::Colour (0xff6f6962) : blockC.withMultipliedBrightness (tone[k]));   // χωρίς ήχο (άνοιγμα): γκρι, χωρίς χρώμα block
                for (int i = 0; i < nt; ++i)
                {
                    const float al = 0.5f * std::pow (1.0f - (float) (i + 1) / (float) nt, 1.8f) * dim;
                    arc (juce::jmax (0.0f, v - tailLen * (float) (i + 1) / (float) nt), juce::jmax (0.0f, v - tailLen * (float) i / (float) nt), c.withAlpha (al));
                }
                arc (juce::jmax (0.0f, v - dl), juce::jmax (v, dl), c);
            }
            if (limited)
            {
                const float a = start + full * lm;
                const float dx = std::sin (a), dy = -std::cos (a), dr = juce::jmax (1.8f, outerEdge * 0.0203f);
                g.setColour (tickC);
                g.fillEllipse (cx + dx * r - dr, cy + dy * r - dr, dr * 2.0f, dr * 2.0f);   // το όριο: τελεία στο μέσο της λωρίδας
            }
            r -= step;
        }
    }

    // Πλάτος stereo: 6 μισοί δακτύλιοι στη δεξιά πλευρά (κορυφή → κάτω), ομόκεντροι· οι 4 εξωτερικοί (58..118) είναι τα όρια των 3 ομάδων παυλών.
    // Ανάβουν από μέσα προς τα έξω με ομαλό άναμμα. Η κορυφή τους έχει διαφορετικό μήκος (ο εξωτερικός ξεκινά στο 12:00, κάθε επόμενος 12° πιο κάτω).
    // Γκρι με τονική διαβάθμιση (σκούρος μέσα, φωτεινός έξω), λευκοί όταν περνά το όριο, παχύτερος = όριο. lev[6] (προαιρετικό) = αποτύπωμα.
    inline void drawSmartWidth (juce::Graphics& g, float cx, float cy, float rmax, float /*unused*/, float unit, float width, float lim, bool active, bool on,
                                const float* lev = nullptr)
    {
        constexpr int n = 6;
        const float wv = juce::jlimit (0.0f, 1.0f, width);
        const bool over = active && wv > lim;
        const juce::Colour offC (47, 44, 43);
        const float thin = juce::jmax (1.0f, rmax * 0.0102f), thick = juce::jmax (1.5f, rmax * 0.0203f);
        int limIdx = -1;
        if (active && lim < 0.999f)
        {
            float best = 9.0f;
            for (int j = 0; j < n; ++j) { const float d = std::abs ((float) (j + 1) / (float) n - lim); if (d < best) { best = d; limIdx = j; } }
        }
        (void) unit;
        static const float rad[n] { 18.0f, 38.0f, 58.0f, 78.0f, 98.0f, 118.0f };
        for (int j = 0; j < n; ++j)
        {
            const float ri = rmax * rad[j] / 118.0f;
            const float sm = juce::jlimit (0.0f, 1.0f, (wv - (float) j / (float) n) * (float) n);
            const float lv = ! on ? 1.0f : lev != nullptr ? juce::jlimit (0.0f, 1.0f, lev[j]) : sm;
            const int gr = 88 + j * 112 / (n - 1);
            const juce::Colour tgt = ! on ? juce::Colour (74, 70, 66) : over ? juce::Colour (250, 250, 250) : juce::Colour ((juce::uint8) gr, (juce::uint8) (gr - 2), (juce::uint8) (gr - 4));
            juce::Path p;
            p.addCentredArc (cx, cy, ri, ri, 0.0f, juce::degreesToRadians (12.0f * (float) (n - 1 - j)), juce::MathConstants<float>::pi, true);
            g.setColour (offC.interpolatedWith (tgt, lv));
            g.strokePath (p, juce::PathStrokeType (j == limIdx ? thick : thin));
        }
    }

    // κείμενο διόρθωσης: μέχρι 3 ζώνες με τη μεγαλύτερη μείωση (μετά την αποδοχή acc) και το πλάτος. Κενό αν δεν χρειάζεται διόρθωση.
    inline juce::String smartCorrectionText (const float* need, float needW, float acc)
    {
        int order[6] { 0, 1, 2, 3, 4, 5 };
        std::sort (order, order + 6, [&] (int a, int b) { return need[a] > need[b]; });
        juce::String out;
        for (int i = 0; i < 3; ++i)
            if (need[order[i]] > 0.5f)
                out += juce::String (out.isEmpty() ? "" : "  ") + smartBandNames[order[i]] + " -" + juce::String (need[order[i]] * acc, 1) + " dB";
        if (needW < 0.97f)
            out += juce::String (out.isEmpty() ? "" : "  ") + "Width " + juce::String ((int) std::round (100.0f - (1.0f - needW) * acc * 100.0f)) + " %";
        return out;
    }
}

// Σύμβολα που ζωγραφίζονται με διανύσματα. Πρόσθεσε νέα στο τέλος του enum και στο switch του drawSymbol.
enum class Symbol { PadRecord, PadOverdub, Play, Stop, Cancel, Undo,
                    StartQ, LengthQ, Snap, Loop, Sync, Tempo, Thru,
                    Block, Learn, Vst, Window, Hold, Arp, Folder, Fx, Chord, Mute, Tape, Orb, Speaker, Sliders, Rings, Follow, Menu };

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

// Τετράγωνο κουμπί 52x52 χωρίς μενού: κάθε πάτημα → επόμενη τιμή, μετά την τελευταία ξανά η πρώτη.
// Αν isOnOff: το σύμβολο είναι πράσινο στο index 1 (On) και κόκκινο στο index 0 (Off).
// Ένα ορθογώνιο κουμπί με στρογγυλεμένες άκρες και δύο μισά: αριστερά ORB (σφαίρα → παράθυρο), δεξιά REC motion (τελεία).
// Όλα άσπρα όταν είναι σβηστά, πράσινα όταν είναι ενεργά.

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
