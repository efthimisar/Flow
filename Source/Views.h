#pragma once
// Οθόνες του Flow: loops, πληκτρολόγιο, κεφαλίδα πάνελ plugin, tempo και tap.
#include <juce_audio_utils/juce_audio_utils.h>
#include "Engine.h"
#include "HobbyStyle.h"
#include "QrCode.h"

// μετρά πόση ώρα παίρνει ένα paint και γράφει στο Flow-log.txt όσα καρέ ξεπερνούν τα 25 ms (το πολύ ένα μήνυμα ανά δευτερόλεπτο)
struct PaintTimer
{
    const char* name; int w, h; double t0 = juce::Time::getMillisecondCounterHiRes();
    PaintTimer (const char* n, int ww, int hh) : name (n), w (ww), h (hh) {}
    ~PaintTimer()
    {
        const double now = juce::Time::getMillisecondCounterHiRes(), dt = now - t0;
        static double lastLog = 0.0;
        if (dt > 25.0 && now - lastLog > 1000.0)
        {
            lastLog = now;
            flow::flowLog (juce::String ("paint: ") + name + " " + juce::String (dt, 1) + " ms (clip " + juce::String (w) + "x" + juce::String (h) + ")");
        }
    }
};

using namespace juce;

static inline String gr (const char* utf8Text) { return String::fromUTF8 (utf8Text); }

static inline float textWidthOf (const Font& f, const String& t)
{
    GlyphArrangement ga;
    ga.addLineOfText (f, t, 0.0f, 0.0f);
    return ga.getBoundingBox (0, -1, true).getWidth();
}

// Χρώματα των blocks: κάθε block έχει μία απόχρωση της παλέτας των φώτων του MK2 (blockHue = δείκτης 0..15:
// 0 κόκκινο, 1 πορτοκαλί, 2 ανοιχτό πορτοκαλί, 3 ζεστό κίτρινο, 4 κίτρινο, 5 λάιμ, 6 πράσινο, 7 μέντα, 8 τυρκουάζ, 9 κυανό,
// 10 μπλε, 11 δαμασκηνί, 12 βιολετί, 13 μοβ, 14 ματζέντα, 15 φούξια). Στην οθόνη φαίνεται η παστέλ εκδοχή της ίδιας απόχρωσης.
static inline int blockHue (int b)
{
    static const int h[9] { 1, 3, 7, 6, 10, 12, 15, 9, 8 };   // πορτοκαλί, ζεστό κίτρινο, μέντα, πράσινο, μπλε, βιολετί, φούξια, κυανό, τυρκουάζ
    return h[((b % 9) + 9) % 9];
}

// ανοιχτό για τα λευκά πλήκτρα, πιο βαθύ για τα μαύρα
static inline Colour blockLight (int b)
{
    static const Colour c[9] { Colour (0xffe8a06a), Colour (0xffe6c85f), Colour (0xff6fd3b0), Colour (0xff7cc97f), Colour (0xff6f8fe8),
                               Colour (0xff9a7fe6), Colour (0xffe578b8), Colour (0xff66b0e6), Colour (0xff5fcfcb) };
    return c[((b % 9) + 9) % 9];
}

static inline Colour blockDeep (int b)
{
    static const Colour c[9] { Colour (0xffb06a38), Colour (0xffae922e), Colour (0xff35a283), Colour (0xff48964c), Colour (0xff4561b8),
                               Colour (0xff6a4cb8), Colour (0xffb04884), Colour (0xff3a80b4), Colour (0xff2f9d99) };
    return c[((b % 9) + 9) % 9];
}

static inline String noteName (int n) { return MidiMessage::getMidiNoteName (n, true, true, 4); }

static inline float textWidth (const Font& f, const String& s)
{
    GlyphArrangement ga;
    ga.addLineOfText (f, s, 0.0f, 0.0f);
    return ga.getBoundingBox (0, -1, true).getWidth();
}

static inline String clockText (double seconds)
{
    const int total = (int) seconds;
    return String::formatted ("%02d:%02d:%02d", total / 3600, (total / 60) % 60, total % 60);
}

static inline StringArray activeBlocks (const flow::Engine& engine)
{
    StringArray list;
    for (int b = 0; b < flow::numBlocks; ++b)
        if (engine.active[(size_t) b].load() != 0 && b != engine.specialBlock.load())   // το ειδικό block δεν έχει λωρίδα λούπας
            list.add (String (b));
    return list;
}

//==============================================================================
// Λίστα plugins μέσα στην οθόνη (στη θέση των loops): ↻ νέα σάρωση, … επιλογή αρχείου, ✕ κλείσιμο. Κλικ σε όνομα = επιλογή.
class PluginListView : public Component
{
public:
    explicit PluginListView (Typeface::Ptr face) : typeface (face)
    {
        search.setFont (hobby::makeFont (typeface, 15.0f));
        search.setTextToShowWhenEmpty (String::fromUTF8 ("search\xe2\x80\xa6"), hobby::kIconGrey);   // αναζήτηση…
        search.setColour (TextEditor::backgroundColourId, Colour (0xff2d2926));
        search.setColour (TextEditor::textColourId, hobby::kText);
        search.setColour (TextEditor::outlineColourId, Colours::transparentBlack);
        search.setColour (TextEditor::focusedOutlineColourId, Colour (0xff4a433d));
        search.setColour (CaretComponent::caretColourId, hobby::kText);
        search.setIndents (10, 6);
        search.onTextChange = [this] { scroll = 0; repaint(); };
        search.onEscapeKey = [this] { if (search.isEmpty()) close(); else search.clear(); };
        addAndMakeVisible (search);
        setVisible (false);
    }

    void resized() override { search.setBounds (jmax (260, getWidth() - 124 - 14 - 190 - 10), 8, 190, 28); }

    void open (const String& t, const String& s, Colour c, StringArray names, StringArray formats,
               std::function<void (int)> pick, std::function<void()> rescan, std::function<void()> file,
               StringArray pluginKeys = {}, StringArray favourites = {}, std::function<void (const StringArray&)> favChangedFn = {})
    {
        title = t; sub = s; colour = c; items = names; fmts = formats;
        onPick = pick; onRescan = rescan; onFile = file;
        keys = pluginKeys; favs = favourites; favChanged = favChangedFn; showAll = false;
        scroll = 0;
        search.clear();
        setVisible (true);
        resized();
        search.grabKeyboardFocus();
        repaint();
    }

    void close() { setVisible (false); }

    void paint (Graphics& g) override
    {
        const int w = getWidth();
        hot.clear();
        g.setColour (hobby::kScreen);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 12.0f);

        g.setFont (hobby::makeFont (typeface, 17.0f));
        g.setColour (colour);
        g.drawText (title, 26, 8, 200, 28, Justification::centredLeft, false);
        g.setFont (hobby::makeFont (typeface, 14.0f));
        g.setColour (hobby::kIconGrey);
        g.drawText (sub + String::fromUTF8 (anyFavForTitle() ? (showAll ? "  \xc2\xb7  all" : "  \xc2\xb7  favorites") : "  \xc2\xb7  mark favorites with f"),
                    26 + (int) std::ceil (textWidth (hobby::makeFont (typeface, 17.0f), title)) + 12, 8, 300, 28, Justification::centredLeft, true);

        auto lamp = [&] (float cx, const String& glyph, Colour c, std::function<void()> fn)
        {
            const Rectangle<float> r (cx - 12.0f, 22.0f - 12.0f, 24.0f, 24.0f);
            g.setColour (c.withAlpha (0.25f));
            g.fillEllipse (r);
            g.setColour (c);
            g.setFont (hobby::makeFont (typeface, 15.0f));
            g.drawText (glyph, r, Justification::centred, false);
            hot.push_back ({ r.expanded (4.0f), fn });
        };
        lamp ((float) w - 34.0f, String::fromUTF8 ("\xe2\x9c\x95"), hobby::kRed, [this] { close(); });
        lamp ((float) w - 64.0f, String::fromUTF8 ("\xe2\x80\xa6"), hobby::kOn, [this] { if (onFile) onFile(); close(); });
        lamp ((float) w - 94.0f, String::fromUTF8 ("\xe2\x86\xbb"), hobby::kOn, [this] { if (onRescan) onRescan(); close(); });
        // ≡ = όλη η λίστα (πάτημα ξανά = μόνο τα favorites με f). Αν δεν υπάρχουν favorites, φαίνονται όλα
        lamp ((float) w - 124.0f, String::fromUTF8 ("\xe2\x89\xa1"), showAll ? hobby::kOn : hobby::kIconGrey, [this] { showAll = ! showAll; scroll = 0; repaint(); });

        const int cols = w >= 640 ? 2 : 1;
        const float gapX = 20.0f, left = 26.0f, rowH = 34.0f, top = 58.0f;
        const float colW = ((float) w - 2.0f * left - gapX * (float) (cols - 1)) / (float) cols;
        const int rowsVisible = jmax (1, (int) ((float) (getHeight() - 58 - 8) / rowH));
        std::vector<int> vis;   // ευρετήριο των plugins που ταιριάζουν στην αναζήτηση
        const String q = search.getText().trim();
        bool anyFav = false;
        for (int i = 0; i < items.size() && ! anyFav; ++i) anyFav = i < keys.size() && favs.contains (keys[i]);
        const bool onlyFavs = ! showAll && anyFav && q.isEmpty();   // η αναζήτηση ψάχνει πάντα σε όλα
        for (int i = 0; i < items.size(); ++i)
        {
            if (! q.isEmpty()) { if (items[i].containsIgnoreCase (q)) vis.push_back (i); }
            else if (! onlyFavs || (i < keys.size() && favs.contains (keys[i]))) vis.push_back (i);
        }
        const int total = (int) vis.size();
        const int totalRows = (total + cols - 1) / cols;
        scroll = jlimit (0, jmax (0, totalRows - rowsVisible), scroll);

        for (int r = 0; r < rowsVisible; ++r)
            for (int c = 0; c < cols; ++c)
            {
                const int k = (r + scroll) * cols + c;
                if (k >= total) continue;
                const int idx = vis[(size_t) k];
                const float x = left + (float) c * (colW + gapX), y = top + (float) r * rowH;
                const Rectangle<float> rr (x, y, colW, rowH - 6.0f);
                g.setColour (Colour (0xff2d2926));
                g.fillRoundedRectangle (rr, 6.0f);
                const bool fav = idx < keys.size() && favs.contains (keys[idx]);
                const Rectangle<float> fr (rr.getX() + 6.0f, rr.getY() + (rr.getHeight() - 22.0f) * 0.5f, 22.0f, 22.0f);
                if (fav) { g.setColour (hobby::kOn.withAlpha (0.22f)); g.fillEllipse (fr); }
                g.setColour (fav ? hobby::kOn : hobby::kIconGrey.withAlpha (0.4f));
                g.setFont (hobby::makeFont (typeface, 16.0f));
                g.drawText ("f", fr, Justification::centred, false);
                hot.push_back ({ fr.expanded (3.0f), [this, idx]
                {
                    if (idx >= keys.size()) return;
                    const auto k = keys[idx];
                    if (favs.contains (k)) favs.removeString (k); else favs.add (k);
                    if (favChanged) favChanged (favs);
                    repaint();
                } });
                g.setColour (hobby::kText);
                g.setFont (hobby::makeFont (typeface, 15.0f));
                g.drawText (items[idx], rr.withTrimmedLeft (38.0f).withTrimmedRight (60.0f), Justification::centredLeft, true);
                g.setColour (hobby::kIconGrey.withAlpha (0.6f));
                g.setFont (hobby::makeFont (typeface, 11.0f));
                g.drawText (fmts[idx], rr.withTrimmedRight (12.0f), Justification::centredRight, false);
                hot.push_back ({ rr, [this, idx] { if (onPick) onPick (idx); close(); } });
            }

        if (totalRows > rowsVisible)   // λεπτή μπάρα κύλισης δεξιά
        {
            const float th = (float) (getHeight() - 58 - 8);
            const float bh = jmax (16.0f, th * (float) rowsVisible / (float) totalRows);
            const float by = top + (th - bh) * (float) scroll / (float) jmax (1, totalRows - rowsVisible);
            g.setColour (Colour (0xff4a433d));
            g.fillRoundedRectangle ((float) w - 12.0f, by, 4.0f, bh, 2.0f);
        }
    }

    void mouseDown (const MouseEvent& e) override
    {
        for (auto& h : hot)
            if (h.first.contains (e.position)) { auto fn = h.second; fn(); return; }
    }

    bool anyFavForTitle() const
    {
        for (int i = 0; i < items.size() && i < keys.size(); ++i) if (favs.contains (keys[i])) return true;
        return false;
    }

    void mouseWheelMove (const MouseEvent&, const MouseWheelDetails& wd) override
    {
        scroll += wd.deltaY > 0 ? -1 : 1;
        repaint();
    }

private:
    Typeface::Ptr typeface;
    String title, sub;
    Colour colour { Colours::white };
    StringArray items, fmts, keys, favs;
    bool showAll = false;
    std::function<void (const StringArray&)> favChanged;
    std::function<void (int)> onPick;
    std::function<void()> onRescan, onFile;
    TextEditor search;
    int scroll = 0;
    std::vector<std::pair<Rectangle<float>, std::function<void()>>> hot;
};

//==============================================================================
// Κουτί tempo: κλικ = πληκτρολόγηση 40-240, σύρσιμο πάνω/κάτω = αλλαγή.
class TempoBox : public Component, public SettableTooltipClient, private TextEditor::Listener
{
public:
    TempoBox (flow::Engine& e, Typeface::Ptr face) : engine (e), typeface (face)
    {
        setMouseCursor (MouseCursor::UpDownResizeCursor);
    }

    std::function<void()> onChange;

    void paint (Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat();
        g.setColour (Colour (0xff2d2926));
        g.fillRoundedRectangle (r, 7.0f);
        g.setColour (Colour (0xff3a3430));
        g.drawRoundedRectangle (r.reduced (0.5f), 7.0f, 1.0f);
        if (editor != nullptr) return;

        g.setFont (hobby::makeFont (typeface, 17.0f));
        g.setColour (hobby::kText);
        g.drawText (String ((int) std::lround (engine.tempo.load())) + " BPM", getLocalBounds(), Justification::centred, false);
        if (engine.isLinkEnabled())
        {
            g.setColour (engine.linkPeersOut.load() > 0 ? hobby::kOn : hobby::kIconGrey);
            g.fillEllipse (9.0f, r.getCentreY() - 3.0f, 6.0f, 6.0f);
        }
    }

    void mouseDown (const MouseEvent& e) override
    {
        dragStartY = e.y;
        dragStartTempo = engine.tempo.load();
        dragged = false;
    }

    void mouseDrag (const MouseEvent& e) override
    {
        if (editor != nullptr) return;
        const int dy = dragStartY - e.y;
        if (std::abs (dy) > 3) dragged = true;
        if (dragged)
        {
            engine.setTempo (std::round (dragStartTempo + (double) dy / 3.0));
            if (onChange) onChange();
            repaint();
        }
    }

    void mouseUp (const MouseEvent&) override
    {
        if (! dragged && editor == nullptr) startEdit();
    }

private:
    void startEdit()
    {
        committing = false;
        editor = std::make_unique<TextEditor>();
        editor->setFont (hobby::makeFont (typeface, 17.0f));
        editor->setJustification (Justification::centred);
        editor->setInputRestrictions (3, "0123456789");
        editor->setColour (TextEditor::backgroundColourId, Colours::transparentBlack);
        editor->setColour (TextEditor::textColourId, hobby::kText);
        editor->setColour (TextEditor::outlineColourId, Colours::transparentBlack);
        editor->setColour (TextEditor::focusedOutlineColourId, Colours::transparentBlack);
        editor->setColour (TextEditor::highlightColourId, Colour (0xff5a4f45));
        editor->setColour (CaretComponent::caretColourId, hobby::kText);
        editor->setText (String ((int) std::lround (engine.tempo.load())), false);
        editor->setBounds (getLocalBounds().reduced (6, 2));
        editor->addListener (this);
        addAndMakeVisible (editor.get());
        editor->grabKeyboardFocus();
        editor->selectAll();
        repaint();
    }

    void finish (bool apply)
    {
        if (committing || editor == nullptr) return;
        committing = true;
        if (apply)
        {
            const int v = editor->getText().getIntValue();
            if (v > 0)
            {
                engine.setTempo ((double) v);
                if (onChange) onChange();
            }
        }
        SafePointer<TempoBox> sp (this);
        MessageManager::callAsync ([sp]
        {
            if (sp != nullptr) { sp->editor.reset(); sp->repaint(); }
        });
    }

    void textEditorReturnKeyPressed (TextEditor&) override { finish (true); }
    void textEditorEscapeKeyPressed (TextEditor&) override { finish (false); }
    void textEditorFocusLost (TextEditor&) override        { finish (true); }

    flow::Engine& engine;
    Typeface::Ptr typeface;
    std::unique_ptr<TextEditor> editor;
    int dragStartY = 0;
    double dragStartTempo = 120.0;
    bool dragged = false, committing = false;
};

// Κυκλάκι tap: γκρι = σβηστό, πράσινο όσο γίνονται taps.
class TapDot : public Component, public SettableTooltipClient
{
public:
    explicit TapDot (flow::Engine& e) : engine (e) {}

    void paint (Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (Colour (0xff2d2926));
        g.fillEllipse (r);
        g.setColour (Colour (0xff3a3430));
        g.drawEllipse (r, 1.0f);
        g.setColour (engine.tapActiveOut.load() != 0 ? hobby::kOn : Colour (0xff6b6a72));
        g.fillEllipse (r.reduced (5.0f));
    }

    void mouseDown (const MouseEvent&) override { engine.tapFromUi(); repaint(); }

private:
    flow::Engine& engine;
};

//==============================================================================
// Οθόνη loops: μία λωρίδα ανά block, άξονας 4/8/16 μέτρα, λευκή κεφαλή αναπαραγωγής.
class LoopsView : public Component, public TooltipClient
{
public:
    LoopsView (flow::Engine& e, const int& sel, Typeface::Ptr face)
        : engine (e), selected (sel), typeface (face), tempoBox (e, face), tapDot (e), list (face)
    {
        addChildComponent (list);
        addChildComponent (tempoBox);
        addChildComponent (tapDot);
        tempoBox.setTooltip (gr ("Tempo (40-240): click to type, or drag up/down"));
        tapDot.setTooltip (gr ("Tap tempo: click or the last key on the right"));
    }

    std::function<void (int)> onSelect;
    TempoBox& getTempoBox() { return tempoBox; }
    PluginListView& getList() { return list; }

    std::function<void()> onMetroChange;

    // Orb: 16 αναθέσεις (4 γωνίες × 4 γραμμές). Η οθόνη ανοίγει με το κουμπί ORB.
    struct KnobState { bool bound = false; float value = 0.0f; String name; };
    std::array<KnobState, 16> knobs;
    static constexpr int knobAreaH = 0;
    static constexpr int stripY = 16;                 // λωρίδα λαμπακιών κάτω από την progress bar
    bool orbMode = false;
    bool qrMode = false;          // μέσα στην οθόνη orb: δείχνει το QR για το iPad αντί για τον orb
    String qrUrl;
    int qrClients = 0;
    void setQr (bool on, const String& url) { qrMode = on; qrUrl = url; repaint(); }
    Image orbImage;
    std::function<void (int)> onOrbAssign, onOrbClear;      // θέση 0..15
    std::function<void (int, Point<float>)> onOrbMove;      // γωνία, θέση (-1..1) μέσα στον orb
    std::function<void (int, bool)> onOrbTouch;
    std::function<void()> onOrbClearMotion, onOrbImage, onOrbImageClear;
    void setOrbMode (bool on)
    {
        if (on == orbMode) return;
        orbMode = on;
        if (on) { list.close(); openMenu (false); orbDrag = -1; orbHover = -1; }
        resized(); repaint();
        if (onOrbMode) onOrbMode();
    }
    std::function<void()> onOrbMode;
    void setOrb (const std::array<KnobState, 16>& k, const std::array<Point<float>, 4>& pos, int learning, bool rec, bool motion)
    {
        for (int c = 0; c < 4; ++c)
            orbLift[(size_t) c] += ((c == orbDrag ? 1.0f : 0.0f) - orbLift[(size_t) c]) * 0.25f;    // ο κύκλος σηκώνεται ομαλά όταν τον κρατάς
        bool changed = pos != orbPos || learning != orbLearning || rec != orbRec || motion != orbMotion || learning >= 0;
        for (size_t i = 0; i < knobs.size() && ! changed; ++i)
            changed = knobs[i].bound != k[i].bound;
        knobs = k; orbPos = pos; orbLearning = learning; orbRec = rec; orbMotion = motion;
        if (changed) repaint (orbRect());
        if (orbExternal) orbExternal->repaint();
    }
    void setOrbPos (const std::array<Point<float>, 4>& p) { orbPos = p; repaint (orbRect()); }
    std::function<bool()> metroIsCustom;   // έχει φορτωθεί δικός ήχος μετρονόμου;
    std::function<void (int)> onMetroSample;   // 1 = διάλεξε ήχο, 2 = αρχικός ήχος

    // φωτάκια εργαλείων του επιλεγμένου κουμπιού (VST ⇄ ✕, MIDI FX ⇄ ✕, FX + ✕): δίπλα στο CPU, χωρίς μενού
    struct ToolLamp { String glyph; Colour colour; bool enabled = true; bool armed = false; std::function<void()> fn; String tip; };
    void setToolLamps (std::vector<ToolLamp> l)
    {
        bool same = l.size() == tools.size();
        for (size_t i = 0; same && i < l.size(); ++i)
            same = l[i].glyph == tools[i].glyph && l[i].enabled == tools[i].enabled && l[i].armed == tools[i].armed && l[i].colour == tools[i].colour;
        tools = std::move (l);
        if (! same) repaint();
    }

    // λωρίδα μετρονόμου (κάτω): tempo, tap, ένταση, έξοδος. Ανοίγει με το πρώτο πάτημα του κουμπιού μετρονόμου.
    bool metroStrip = false;
    void setMetroStrip (bool on)
    {
        if (on == metroStrip) return;
        metroStrip = on;
        resized();
        repaint();
    }

    // μενού μέσα στην οθόνη (κουμπί φακέλου): αριστερά επιλογές, δεξιά πληροφορίες
    struct MenuEntry { String glyph, text; bool enabled = true, checked = false, isSets = false; int symbol = -1; std::function<void()> fn; std::vector<MenuEntry> sub; };
    bool menuOpen = false;
    struct SetItem { String name, date; Image thumb; bool current = false; std::function<void()> fn; };
    void setSetItems (std::vector<SetItem> s) { setItems = std::move (s); if (menuOpen) repaint(); }
    void mouseWheelMove (const MouseEvent&, const MouseWheelDetails& wd) override
    {
        if (! menuOpen || ! setsOpen) return;
        setsScroll = jmax (0, setsScroll + (wd.deltaY > 0 ? -1 : 1));
        repaint();
    }
    std::function<void()> onMenuOpen;   // το Main γεμίζει τις επιλογές και τις πληροφορίες
    void setMenu (std::vector<MenuEntry> items) { menuItems = std::move (items); if (menuOpen) repaint(); }
    void setMenuInfo (std::vector<std::pair<String, String>> info)
    {
        if (info == menuInfo) return;
        menuInfo = std::move (info);
        if (menuOpen) repaint();
    }
    void openMenu (bool on)
    {
        if (on == menuOpen) return;
        menuOpen = on; menuSub = -1; menuHover = -1; setsOpen = false; setsScroll = 0;
        if (on) { list.close(); if (onMenuOpen) onMenuOpen(); }
        repaint();
    }

    void paintOverChildren (Graphics& g) override
    {
        menuHot.clear(); menuPanel = {};
        if (! menuOpen) return;

        const float w = (float) getWidth();
        const Rectangle<float> panel (14.0f, 42.0f, w - 28.0f, (float) (getHeight() - knobAreaH - 6) - 42.0f);
        menuPanel = panel;
        g.setColour (Colour (0xff1e1b19));
        g.fillRoundedRectangle (panel, 10.0f);
        g.setColour (Colour (0xff3a3430));
        g.drawRoundedRectangle (panel.reduced (0.5f), 10.0f, 1.0f);

        const float split = panel.getX() + 52.0f;
        g.setColour (Colour (0xff3a3430));
        g.fillRect (split, panel.getY() + 12.0f, 1.0f, panel.getHeight() - 24.0f);

        const Colour dim (0xff6b625a), txt (0xfff3f4f6);
        g.setFont (hobby::makeFont (typeface, 11.0f));
        g.setColour (dim);
        if (! setsOpen) g.drawText ("INFO", (int) split + 14, (int) panel.getY() + 8, 120, 16, Justification::centredLeft, false);

        // αριστερή στήλη: μικροί κύκλοι με σύμβολο, το όνομα φαίνεται στο hover
        const int n = jmax (1, (int) menuItems.size());
        const float step = (panel.getHeight() - 20.0f) / (float) n;
        const float rad = jlimit (8.0f, 13.0f, step * 0.5f - 2.0f);
        const float cx = panel.getX() + 26.0f;
        std::function<void()> closeFn = [this] { openMenu (false); };
        std::vector<Rectangle<float>> circles;
        for (int i = 0; i < (int) menuItems.size(); ++i)
        {
            const auto& it = menuItems[(size_t) i];
            const float cy = panel.getY() + 10.0f + step * ((float) i + 0.5f);
            const Rectangle<float> c (cx - rad, cy - rad, 2.0f * rad, 2.0f * rad);
            circles.push_back (c);
            const Rectangle<float> r = c.expanded (4.0f, jmax (0.0f, (step - 2.0f * rad) * 0.5f));
            const bool hot_ = i == menuHover || i == menuSub || (it.isSets && setsOpen);
            g.setColour (hot_ && it.enabled ? Colour (0xff54493f) : Colour (0xff3a332e));
            g.fillEllipse (c);
            g.setColour (it.checked || (it.isSets && setsOpen) ? hobby::kOn : Colour (0xff4a423b));
            g.drawEllipse (c.reduced (0.5f), it.checked ? 1.6f : 1.0f);
            const Colour ic = it.enabled ? (it.glyph == "!" ? Colour (0xffff7a6e) : hobby::kOn) : dim;
            if (it.symbol >= 0) drawSymbol (g, (Symbol) it.symbol, c.getCentre(), rad * 1.15f, ic);
            else
            {
                g.setColour (ic);
                g.setFont (hobby::makeFont (typeface, rad * 1.05f));
                g.drawText (it.glyph, c.toNearestInt(), Justification::centred, false);
            }
            if (it.enabled)
            {
                if (it.isSets)        menuHot.push_back ({ r, [this] { setsOpen = ! setsOpen; setsScroll = 0; menuSub = -1; }, i });
                else if (! it.sub.empty()) menuHot.push_back ({ r, [this, i] { setsOpen = false; menuSub = menuSub == i ? -1 : i; }, i });
                else                  menuHot.push_back ({ r, [this, fn = it.fn, closeFn] { auto f = fn; closeFn(); if (f) f(); }, i });
            }
        }

        // όνομα στο hover
        if (menuHover >= 0 && menuHover < (int) menuItems.size() && menuHover != menuSub)
        {
            const auto& it = menuItems[(size_t) menuHover];
            const auto c = circles[(size_t) menuHover];
            g.setFont (hobby::makeFont (typeface, 12.0f));
            const float tw = textWidth (g.getCurrentFont(), it.text) + 18.0f;
            const Rectangle<float> tr (c.getRight() + 10.0f, c.getCentreY() - 11.0f, tw, 22.0f);
            g.setColour (Colour (0xff2f2a27)); g.fillRoundedRectangle (tr, 7.0f);
            g.setColour (Colour (0xff4a423b)); g.drawRoundedRectangle (tr.reduced (0.5f), 7.0f, 1.0f);
            g.setColour (it.enabled ? txt : dim);
            g.drawText (it.text, tr.toNearestInt(), Justification::centred, false);
        }

        // υπομενού (flyout) δίπλα στον κύκλο
        if (menuSub >= 0 && menuSub < (int) menuItems.size() && ! menuItems[(size_t) menuSub].sub.empty())
        {
            const auto& subs = menuItems[(size_t) menuSub].sub;
            const float subH = jlimit (20.0f, 28.0f, step);
            const float fw = 260.0f, fh = subH * (float) subs.size() + 12.0f;
            const auto c = circles[(size_t) menuSub];
            const float fx = c.getRight() + 12.0f;
            float fy = c.getCentreY() - subH * 0.5f - 6.0f;
            fy = jmax (panel.getY() + 4.0f, jmin (fy, panel.getBottom() - fh - 6.0f));
            const Rectangle<float> fr (fx, fy, fw, fh);
            g.setColour (Colour (0xff2f2a27)); g.fillRoundedRectangle (fr, 8.0f);
            g.setColour (Colour (0xff4a423b)); g.drawRoundedRectangle (fr.reduced (0.5f), 8.0f, 1.0f);
            menuHot.push_back ({ fr, [] {}, -1 });   // κλικ μέσα στο flyout δεν το κλείνει
            for (int j = 0; j < (int) subs.size(); ++j)
            {
                const auto& sj = subs[(size_t) j];
                const Rectangle<float> rr (fr.getX() + 6.0f, fr.getY() + 6.0f + subH * (float) j, fr.getWidth() - 12.0f, subH);
                g.setFont (hobby::makeFont (typeface, 13.0f));
                g.setColour (sj.enabled ? txt : dim);
                g.drawText (sj.text, (int) rr.getX() + 8, (int) rr.getY(), (int) rr.getWidth() - 34, (int) rr.getHeight(), Justification::centredLeft, true);
                if (sj.checked) { g.setColour (hobby::kOn); g.drawText (String::fromUTF8 ("\xe2\x9c\x93"), (int) rr.getRight() - 24, (int) rr.getY(), 20, (int) rr.getHeight(), Justification::centred, false); }
                if (sj.enabled) menuHot.push_back ({ rr, [this, fn = sj.fn, closeFn] { auto f = fn; closeFn(); if (f) f(); }, -1 });
            }
        }

        // sets με μικρογραφία
        if (setsOpen)
        {
            g.setColour (dim); g.setFont (hobby::makeFont (typeface, 11.0f));
            g.drawText ("MY SETS", (int) split + 14, (int) panel.getY() + 8, 120, 16, Justification::centredLeft, false);
            const float areaX = split + 14.0f, areaY = panel.getY() + 30.0f, areaW = panel.getRight() - areaX - 14.0f, areaH = panel.getBottom() - areaY - 8.0f;
            const int cols = areaW > 520.0f ? 2 : 1;
            const float cw = areaW / (float) cols, sz = 32.0f, rowStep = 42.0f;
            const int visRows = jmax (1, (int) (areaH / rowStep));
            const int totalRows = ((int) setItems.size() + cols - 1) / cols;
            setsScroll = jlimit (0, jmax (0, totalRows - visRows), setsScroll);
            if (setItems.empty())
            {
                g.setColour (dim); g.setFont (hobby::makeFont (typeface, 13.0f));
                g.drawText (String::fromUTF8 ("\xe2\x80\x93"), (int) areaX, (int) areaY, 200, 24, Justification::centredLeft, false);
            }
            for (int k = 0; k < (int) setItems.size(); ++k)
            {
                const int row = k / cols - setsScroll, col = k % cols;
                if (row < 0 || row >= visRows) continue;
                const auto& si = setItems[(size_t) k];
                const Rectangle<float> rr (areaX + cw * (float) col, areaY + rowStep * (float) row, cw - 12.0f, sz + 6.0f);
                if (menuHover == 1000 + k) { g.setColour (Colour (0xff3a332e)); g.fillRoundedRectangle (rr, 8.0f); }
                const Rectangle<float> tc (rr.getX() + 4.0f, rr.getY() + 3.0f, sz, sz);
                g.setColour (Colour (0xff2e2a27)); g.fillEllipse (tc);
                if (si.thumb.isValid())
                {
                    Graphics::ScopedSaveState ss (g);
                    Path clip; clip.addEllipse (tc);
                    g.reduceClipRegion (clip);
                    g.drawImage (si.thumb, tc, RectanglePlacement::stretchToFit);
                }
                else
                {
                    g.setColour (Colour (0xff6e645c));
                    for (int a = 0; a < 12; ++a)
                    {
                        const float ang = MathConstants<float>::twoPi * (float) a / 12.0f;
                        g.fillEllipse (tc.getCentreX() + (sz * 0.5f - 4.0f) * std::cos (ang) - 0.9f, tc.getCentreY() + (sz * 0.5f - 4.0f) * std::sin (ang) - 0.9f, 1.8f, 1.8f);
                    }
                    g.fillEllipse (tc.getCentreX() - 2.0f, tc.getCentreY() - 2.0f, 4.0f, 4.0f);
                }
                g.setColour (si.current ? hobby::kOn : Colour (0xff4a423b));
                g.drawEllipse (tc.reduced (0.5f), si.current ? 1.6f : 1.0f);
                g.setColour (txt); g.setFont (hobby::makeFont (typeface, 14.0f));
                g.drawText (si.name, (int) (tc.getRight() + 10.0f), (int) rr.getY() + 2, (int) (rr.getRight() - tc.getRight() - 14.0f), 18, Justification::centredLeft, true);
                g.setColour (dim); g.setFont (hobby::makeFont (typeface, 10.5f));
                g.drawText (si.date, (int) (tc.getRight() + 10.0f), (int) rr.getY() + 20, 160, 14, Justification::centredLeft, false);
                menuHot.push_back ({ rr, [this, fn = si.fn, closeFn] { auto f = fn; closeFn(); if (f) f(); }, 1000 + k });
            }
            return;
        }

        // πληροφορίες
        const float rowH = jlimit (18.0f, 28.0f, (panel.getHeight() - 38.0f) / (float) jmax (1, (int) menuInfo.size()));
        const float y0 = panel.getY() + 30.0f;
        for (int i = 0; i < (int) menuInfo.size(); ++i)
        {
            const float y = y0 + rowH * (float) i;
            g.setFont (hobby::makeFont (typeface, 12.0f));
            g.setColour (dim);
            g.drawText (menuInfo[(size_t) i].first, (int) split + 14, (int) y, 100, (int) rowH, Justification::centredLeft, true);
            g.setFont (hobby::makeFont (typeface, 13.0f));
            g.setColour (txt);
            g.drawText (menuInfo[(size_t) i].second, (int) split + 120, (int) y, (int) (panel.getRight() - split - 130.0f), (int) rowH, Justification::centredLeft, true);
        }
    }

    void mouseMove (const MouseEvent& e) override
    {
        if (orbMode)
        {
            int h = -1;
            for (int slot = 0; slot < 16; ++slot) if (orbRowRect (slot).contains (e.position)) h = slot;
            if (h != orbHover) { orbHover = h; repaint(); }
            return;
        }
        if (! menuOpen) return;
        int h = -1;
        for (auto& z : menuHot) if (z.idx >= 0 && z.r.contains (e.position)) h = z.idx;
        if (h != menuHover) { menuHover = h; repaint (menuPanel.toNearestInt()); }
    }

    // ενδείξεις κάτω (χωρίς πλαίσιο): block, συγχορδία, μήνυμα ή CPU, τελεία MIDI
    bool midiActive = false;
    String title, chordName, notice;
    Colour noticeColour { Colour (0xffe8837a) };
    float cpu = 0.0f;

    void resized() override
    {
        list.setBounds (0, 44, getWidth(), jmax (60, getHeight() - 44 - knobAreaH - 4));
        const int tw = getWidth(), ts = 22;
        tapDot.setBounds (tw - 14 - ts, 12, ts, ts);
        tempoBox.setBounds (tapDot.getX() - 8 - 100, 12, 100, 22);
        tempoBox.setVisible (false);   // το tempo βγήκε από την κύρια οθόνη (tap = πλήκτρο B0)
        tapDot.setVisible (false);
    }

    void paint (Graphics& g) override
    {
        PaintTimer pt ("loops", g.getClipBounds().getWidth(), g.getClipBounds().getHeight());
        if (orbMode) { hot.clear(); tipZones.clear(); if (qrMode) paintQr (g); else paintOrb (g); return; }
        paintHome (g);
        return;
        const int w = getWidth();
        const int axisBars = engine.axisBarsOut.load();
        const double axisBeats = (double) axisBars * flow::beatsPerBar;
        const double now = engine.beatPosOut.load();
        double cycleBeats = 0.0;   // ο μεγαλύτερος κύκλος λούπας που παίζει (0 = καμία)
        for (int b = 0; b < flow::numBlocks; ++b)
        {
            const int st = engine.loopStateOut[(size_t) b].load();
            if (engine.active[(size_t) b].load() != 0 && (st == flow::loopPlay || st == flow::loopOver))
                cycleBeats = jmax (cycleBeats, engine.loopBeatsOut[(size_t) b].load());
        }

        int recording = -1;
        bool overdubbing = false;
        for (int b = 0; b < flow::numBlocks; ++b)
        {
            if (engine.active[(size_t) b].load() == 0) continue;
            const int st = engine.loopStateOut[(size_t) b].load();
            if (st == flow::loopRec || st == flow::loopOver) { recording = b; overdubbing = st == flow::loopOver; break; }
        }

        // ενδείξεις εγγραφής: εμφανίζονται κάτω, στη λωρίδα, μόνο όσο γράφει κάτι
        const auto tapeState = engine.recorder.getState();
        const bool taping = tapeState == RecorderEngine::State::Recording;
        String audioText = tapeState == RecorderEngine::State::Error ? String ("ERROR")
                           : clockText (engine.recorder.getElapsedSeconds());
        if (engine.recorder.isZipping()) audioText = "...";

        // λωρίδες
        const auto list = activeBlocks (engine);
        const int count = list.size();
        const int top = 48;
        const int room = jmax (40, getHeight() - top - 12 - knobAreaH);   // το ύψος της οθόνης loops αλλάζει με το plugin
        const int step = count > 0 ? jmin (44, room / count) : 44;
        const int laneH = jmax (6, step - 6);
        const float laneX = 14.0f, laneW = (float) (w - 14) - laneX;

        stepOut = step; topOut = top;

        g.setFont (hobby::makeFont (typeface, jlimit (10.0f, 16.0f, (float) laneH * 0.6f)));
        for (int i = 0; i < count; ++i)
        {
            const int b = list[i].getIntValue();
            const int y = top + i * step;
            g.setColour (Colour (0xff2d2926));
            g.fillRoundedRectangle (laneX, (float) y, laneW, (float) laneH, jmin (6.0f, (float) laneH * 0.3f));
        }

        const int gridBottom = top + jmax (1, count) * step + 4;

        // μπάρα tempo: δουλεύει μόνο όταν ο μετρονόμος είναι ανοιχτός. Αναβοσβήνει σε κάθε beat (έντονα στην αρχή του κύκλου, μέτρια στο τελευταίο beat, αχνά στα υπόλοιπα)
        {
            const float by = 6.0f, bh = 5.0f;
            g.setColour (Colour (0xff3a3430));
            g.fillRoundedRectangle (laneX, by, laneW, bh, bh * 0.5f);
            if (engine.metroOn.load())
            {
                const double cyc = jmax (1.0, engine.metroCycleOut.load());
                const double bt = std::floor (jmax (0.0, now) + 1.0e-9);
                const double frac = jmax (0.0, now - bt);
                const double pc = std::fmod (bt, cyc);
                const float level = (pc < 1.0e-6) ? 1.0f : (std::abs (pc - (cyc - 1.0)) < 1.0e-6 ? 0.65f : 0.35f);
                const float decay = jlimit (0.0f, 1.0f, 1.0f - (float) frac * 2.8f);
                const float a = level * decay * decay;
                g.setColour (Colour (0xff3a3430).interpolatedWith (Colour (0xfff0ebe2), a));
                g.fillRoundedRectangle (laneX, by, laneW, bh, bh * 0.5f);
            }
        }
        for (int i = 0; i <= axisBars; ++i)
        {
            const float x = laneX + laneW * (float) i / (float) axisBars;
            const bool strong = i % 4 == 0;
            g.setColour (strong ? hobby::kGridLine.brighter (0.15f) : hobby::kGridLine);
            g.fillRect (x - 0.5f, (float) (top - 6), 1.0f, (float) (gridBottom - top + 6));
        }

        for (int i = 0; i < count; ++i)
        {
            const int b = list[i].getIntValue();
            const float y = (float) (top + i * step);
            const float lh = (float) laneH;
            const float corner = jmin (6.0f, lh * 0.3f);
            const int state = engine.loopStateOut[(size_t) b].load();

            if (state == flow::loopPlay || state == flow::loopOver)
            {
                const double L = jmax (0.25, engine.loopBeatsOut[(size_t) b].load());
                const int layers = engine.loopLayersOut[(size_t) b].load();
                const unsigned allMask = layers <= 0 ? 0u : (layers >= 32 ? 0xffffffffu : ((1u << layers) - 1u));
                const bool allMuted = (layers > 0 && ((unsigned) engine.loopMuteOut[(size_t) b].load() & allMask) == allMask)
                                      || engine.loopStoppedOut[(size_t) b].load() != 0;

                // οι λούπες είναι αγκυρωμένες στο πλέγμα (beat 0) και ο κύκλος είναι η μεγαλύτερη λούπα:
                // η λωρίδα σχεδιάζεται μόνο μέσα στον κύκλο, όπως κινείται και το playhead
                const double drawEnd = jmin (axisBeats, jmax (L, cycleBeats));
                for (double k = 0.0; k < drawEnd - 1.0e-6; k += L)
                {
                    const double len = jmin (L, drawEnd - k);
                    const Rectangle<float> seg (laneX + laneW * (float) (k / axisBeats) + 0.002f * laneW, y,
                                                laneW * (float) (len / axisBeats) - 0.004f * laneW, lh);
                    const bool dimSeg = allMuted || engine.blockOff[(size_t) b].load() != 0;
                    const Colour segCol = dimSeg ? Colour (0xff3a3d46) : blockLight (b);
                    if (! dimSeg)   // λάμψη οθόνης γύρω από τη λωρίδα
                    {
                        g.setColour (segCol.withAlpha (0.07f));
                        g.fillRoundedRectangle (seg.expanded (6.0f), corner + 6.0f);
                        g.setColour (segCol.withAlpha (0.10f));
                        g.fillRoundedRectangle (seg.expanded (3.0f), corner + 3.0f);
                    }
                    if (dimSeg) g.setColour (segCol);   // ντεγκραντέ: 100% αριστερά, 70% δεξιά
                    else g.setGradientFill (ColourGradient (segCol, seg.getX(), 0.0f, segCol.interpolatedWith (hobby::kScreen, 0.3f), seg.getRight(), 0.0f, false));
                    g.fillRoundedRectangle (seg, corner);
                    if (! dimSeg && seg.getWidth() > 4.0f * corner)   // φωτεινή κορυφή
                    {
                        g.setColour (Colours::white.withAlpha (0.28f));
                        g.fillRect (seg.getX() + corner, seg.getY() + 0.5f, seg.getWidth() - 2.0f * corner, 1.0f);
                    }
                    if (state == flow::loopOver)
                    {
                        g.setColour (hobby::kPeach);
                        g.drawRoundedRectangle (seg.expanded (1.0f), corner + 1.0f, 1.5f);
                    }
                }
            }
            else if (state == flow::loopRec)
            {
                const double start = engine.recStartBeatOut[(size_t) b].load();
                const double elapsed = jlimit (0.0, axisBeats, now - start);
                if (elapsed > 0.0)
                {
                    const double p0 = std::fmod (start, 2.0 * flow::beatsPerBar);   // ο κύκλος 2 μέτρων ξεκινά στο τέρμα αριστερά (ο άξονας είναι 4+ μέτρα)
                    auto piece = [&] (double from, double len)
                    {
                        const float x = laneX + laneW * (float) (from / axisBeats);
                        const float ww = laneW * (float) (len / axisBeats);
                        g.setColour (hobby::kRed.withAlpha (0.35f));
                        g.fillRoundedRectangle (x - 2.0f, y - 2.0f, ww + 4.0f, lh + 4.0f, corner + 1.0f);
                        g.setColour (hobby::kCoral);
                        g.fillRoundedRectangle (x, y, jmax (2.0f, ww), lh, corner);
                    };
                    const double firstLen = jmin (elapsed, axisBeats - p0);
                    piece (p0, firstLen);
                    if (elapsed > firstLen) piece (0.0, elapsed - firstLen);
                }
            }
        }

        bool anyLoop = recording >= 0;
        for (int i = 0; i < count && ! anyLoop; ++i)
            if (engine.loopStateOut[(size_t) list[i].getIntValue()].load() != flow::loopOff) anyLoop = true;
        // χωρίς λούπα το playhead περιμένει στην αρχή: ξεκινά όταν πατηθεί το Rec MIDI loop
        const double headBeats = (recording < 0 && cycleBeats > 0.0) ? jmin (axisBeats, cycleBeats) : axisBeats;
        const float hx = laneX + (anyLoop ? laneW * (float) (std::fmod (now, headBeats) / axisBeats) : 0.0f);
        g.setColour (anyLoop ? Colours::white : Colours::white.withAlpha (0.25f));
        g.fillRect (hx - 1.0f, (float) (top - 8), 2.0f, (float) (gridBottom - top + 8));

        // κάτω λωρίδα ενδείξεων
        {
            const float sh = 22.0f, sy = (float) stripY;
            const float cx = (float) w * 0.5f;
            const bool showRec = recording >= 0 || taping || engine.recorder.isZipping() || tapeState == RecorderEngine::State::Error;
            hot.clear();
            tipZones.clear();
            const float titleW = 0.0f;   // ο τίτλος του block είναι πια πάνω από τα ποτενσιόμετρα
            float chordX = 14.0f;
            // διάταξη: τα σύμβολα σπρώχνουν το ένα το άλλο. Αν δεν χωράνε όλα, πρώτα φεύγουν οι τιμές δεξιά, μετά το CPU.
            if (chordName.isNotEmpty()) lastChord = chordName;   // η συγχορδία μένει μέχρι να αλλάξει
            const float chordW = lastChord.isNotEmpty() ? std::ceil (textWidth (hobby::makeFont (typeface, 16.0f), lastChord)) + 14.0f : 0.0f;
            const float toolsW = (float) tools.size() * 26.0f;
            auto needed = [&] (bool cpuOn, bool valsOn)
            {
                const float right = 18.0f + 28.0f + 44.0f; juce::ignoreUnused (valsOn);
                return 14.0f + titleW + chordW + 16.0f + (cpuOn ? 76.0f + 16.0f : 0.0f) + toolsW + 16.0f + right;
            };
            const bool cpuAllowed = ! metroStrip && ! showRec;
            bool layoutCpu = true, layoutVals = true;
            if (needed (true, true) > (float) w) layoutVals = false;
            if (needed (true, false) > (float) w) layoutCpu = false;
            const bool showCpu = cpuAllowed && layoutCpu;
            if (showCpu)   // CPU: μικρός κουκκιδωτός δίσκος που γεμίζει (σαν τα ποτενσιόμετρα) και το % δίπλα
            {
                const float load = jlimit (0.0f, 1.0f, cpu);
                const Colour cc = load < 0.6f ? hobby::kOn : load < 0.85f ? Colour (0xfff5de8a) : hobby::kRed;
                const float R = 10.0f;
                const float cpuW = 2.0f * R + 46.0f, groupW = cpuW + (toolsW > 0.0f ? 16.0f + toolsW : 0.0f);
                const float c0 = cx - groupW * 0.5f;   // ο δείκτης CPU στη μέση, δίπλα στα εργαλεία (αν υπάρχουν)
                const Rectangle<float> disc (c0, sy + sh * 0.5f - R, 2.0f * R, 2.0f * R);
                Path circle; circle.addEllipse (disc);
                g.setColour (Colour (0xff3a3430));
                g.fillPath (circle);
                if (load > 0.005f)
                {
                    Path pie;
                    const float a0 = MathConstants<float>::pi * -0.75f;
                    pie.addPieSegment (disc, a0, a0 + MathConstants<float>::pi * 1.5f * load, 0.0f);
                    g.setColour (cc);
                    g.fillPath (pie);
                }
                g.saveState();
                g.reduceClipRegion (circle);
                g.setTiledImageFill (dotGaps(), 0, 0, 1.0f);
                g.fillRect (disc);
                g.restoreState();
                g.setFont (hobby::makeFont (typeface, 13.0f));
                g.setColour (load >= 0.85f ? hobby::kRed : hobby::kIconGrey);
                g.drawText (String ((int) std::lround (load * 100.0f)) + "%", Rectangle<float> (c0 + 2.0f * R + 6.0f, sy, 40.0f, sh), Justification::centredLeft, false);
                tipZones.push_back ({ Rectangle<float> (c0, sy, 2.0f * R + 46.0f, sh), String::fromUTF8 ("Processor load (CPU)") });
            }
            if (lastChord.isNotEmpty())
            {
                g.setFont (hobby::makeFont (typeface, 16.0f));
                g.setColour (hobby::kLilac);
                g.drawText (lastChord, (int) chordX, (int) sy, (int) chordW + 4, (int) sh, Justification::centredLeft, false);
                chordX += chordW;
            }
            if (metroStrip)
            {
                // tempo και tap είναι τα δύο components αριστερά. Ένταση: 25 50 75 100. Έξοδος: ⇨ 1 2 3 4
                const float ly = sy + sh * 0.5f, R = 9.0f;
                auto lampAt = [&] (float x, bool on, const String& txt, std::function<void()> fn)
                {
                    const Rectangle<float> r (x - R, ly - R, 2.0f * R, 2.0f * R);
                    g.setColour (on ? hobby::kOn : Colour (0xff6b625a));   // σκέτο σύμβολο που φωτίζει ή σβήνει
                    g.setFont (hobby::makeFont (typeface, 16.0f));
                    g.drawText (txt, r, Justification::centred, false);
                    hot.push_back ({ r.expanded (3.0f), fn });
                };
                // τρία κουμπιά δίπλα στο tap: ένταση (κάθε πάτημα αλλάζει επίπεδο), ήχος (δικός σου ⇄ αρχικός). Η έξοδος του μετρονόμου (ακουστικά) μένει σταθερή, δεν έχει κουμπί
                float x = (float) tapDot.getRight() + 26.0f;
                const bool custom = metroIsCustom && metroIsCustom();
                tipZones.push_back ({ Rectangle<float> (x - 12.0f, ly - 12.0f, 24.0f, 24.0f), custom ? String::fromUTF8 ("Back to the default metronome sound") : String::fromUTF8 ("Choose your own metronome sound") });
                lampAt (x, custom, custom ? String::fromUTF8 ("\xe2\x86\xba") : String::fromUTF8 ("\xe2\x99\xaa"),
                        [this, custom] { if (onMetroSample) onMetroSample (custom ? 2 : 1); });   // ♪ διάλεξε δικό σου ήχο, ↺ γύρνα στον αρχικό
            }
            else if (showRec)
            {
                const float pl = (float) hobby::pulse (Time::getMillisecondCounterHiRes() * 0.001 * 1.5);
                float x0 = cx - 150.0f;
                g.setFont (hobby::makeFont (typeface, 14.0f));
                if (recording >= 0)
                {
                    const String t = String (overdubbing ? "OVERDUB " : "REC LOOP ") + String (recording + 1);
                    g.setColour (hobby::kRed.withAlpha (0.4f + 0.6f * pl));
                    g.fillEllipse (x0, sy + sh * 0.5f - 4.0f, 8.0f, 8.0f);
                    g.setColour (overdubbing ? hobby::kPeach : hobby::kRed);
                    g.drawText (t, Rectangle<float> (x0 + 14.0f, sy, 110.0f, sh), Justification::centredLeft, false);
                    x0 += 130.0f;
                }
                if (taping || engine.recorder.isZipping() || tapeState == RecorderEngine::State::Error)
                {
                    g.setColour (hobby::kRed.withAlpha (taping ? 0.4f + 0.6f * pl : 1.0f));
                    g.fillEllipse (x0, sy + sh * 0.5f - 4.0f, 8.0f, 8.0f);
                    g.setColour (hobby::kRed);
                    g.drawText (audioText, Rectangle<float> (x0 + 14.0f, sy, 170.0f, sh), Justification::centredLeft, false);
                }
            }
            else
            {
                // λάμπες εργαλείων στη μέση: σκέτα σύμβολα, φωτίζουν ή σβήνουν. Το μπαλούν λέει τι κάνει η καθεμία.
                const float leftEnd = 14.0f + titleW + chordW;
                const float rightStart = (float) w - 44.0f - 18.0f - 28.0f;
                const float cpuBlock = showCpu ? 2.0f * 10.0f + 46.0f + 16.0f : 0.0f;
                const float groupW = cpuBlock + toolsW;
                float tx = jmin (jmax (cx - groupW * 0.5f, leftEnd + 16.0f), rightStart - 16.0f - groupW) + cpuBlock + 13.0f;
                for (auto& tl : tools)
                {
                    const Rectangle<float> r (tx - 12.0f, sy + sh * 0.5f - 11.0f, 24.0f, 22.0f);
                    const Colour c = tl.armed ? hobby::kRed : tl.colour;
                    g.setColour (tl.enabled ? c : Colour (0xff4a433d));
                    g.setFont (hobby::makeFont (typeface, 16.0f));
                    g.drawText (tl.glyph, r, Justification::centred, false);
                    if (tl.enabled) this->hot.push_back ({ r.expanded (1.0f), tl.fn });
                    if (tl.tip.isNotEmpty()) tipZones.push_back ({ r, tl.tip });
                    tx += 26.0f;
                }
            }
            // τα A (Auto loop), ≈ και # μεταφέρθηκαν στις ρυθμίσεις (Loop recording)
            if (notice.isNotEmpty())   // μηνύματα: γκρι γράμματα κάτω αριστερά στην οθόνη της λούπας, χωρίς φόντο
            {
                g.setFont (hobby::makeFont (typeface, 13.0f));
                g.setColour (hobby::kIconGrey);
                g.drawText (notice, w / 5, getHeight() - 28, w * 3 / 5, 18, Justification::centred, true);
            }
            g.setColour (midiActive ? hobby::kOn : Colour (0xff4a433d));
            g.fillEllipse ((float) w - 24.0f, sy + sh * 0.5f - 3.5f, 7.0f, 7.0f);
        }

    }

    // κουκκιδωτή υφή: φωτεινά 2x2 pixel με σκοτεινά κενά
    static const Image& dotGaps()
    {
        static Image tex = [] { Image im (Image::ARGB, 3, 3, true);
                                for (int y = 0; y < 3; ++y) for (int x = 0; x < 3; ++x)
                                    if (x == 2 || y == 2) im.setPixelAt (x, y, Colours::black.withAlpha (0.42f));
                                return im; }();
        return tex;
    }

    // ---------------------------------------------------------------- Orb
    struct OrbGeom { float cx, cy, R; };
    OrbGeom orbGeom() const
    {
        if (orbWo == 0) { const auto hg = homeGeom(); return { hg.ocx, hg.cy, hg.orbR }; }
        const float w = (float) orbW(), h = (float) orbH();
        return { w * 0.5f, h * 0.5f + 6.0f, jmax (40.0f, jmin ((h - 70.0f) * 0.5f, w * 0.2f)) };
    }
    static Colour orbColour (int c)
    {
        static const Colour col[4] { Colour (0xffe8a06a), Colour (0xff7cc97f), Colour (0xffe578b8), Colour (0xff6f8fe8) };
        return col[c & 3];
    }
    static bool orbLeft (int c) { return c == 0 || c == 3; }
    Point<float> orbListOrigin (int c) const     // γωνία κειμένου της λίστας (αριστερά: δεξιό άκρο, δεξιά: αριστερό άκρο)
    {
        const auto gm = orbGeom();
        return { orbLeft (c) ? gm.cx - gm.R - 22.0f : gm.cx + gm.R + 22.0f, c < 2 ? gm.cy - gm.R + 12.0f : gm.cy + gm.R - 64.0f };
    }
    Rectangle<float> orbRowRect (int slot) const
    {
        const int c = slot / 4, r = slot % 4;
        const auto o = orbListOrigin (c);
        const float y = o.y + 16.0f * (float) r - 8.0f;
        return orbLeft (c) ? Rectangle<float> (o.x - 170.0f, y, 170.0f, 16.0f) : Rectangle<float> (o.x, y, 170.0f, 16.0f);
    }
    Rectangle<float> orbDotRect (int c) const
    {
        const auto o = orbListOrigin (c);
        return Rectangle<float> (0, 0, 22.0f, 22.0f).withCentre ({ orbLeft (c) ? o.x - 6.0f : o.x + 6.0f, o.y - 22.0f });
    }
    Point<float> orbDotPoint (int c) const
    {
        const auto gm = orbGeom();
        const float k = gm.R - 12.0f;
        return { gm.cx + orbPos[(size_t) c].x * k, gm.cy + orbPos[(size_t) c].y * k };
    }
    Rectangle<float> orbBottomRect() const   // "✕ motion"
    {
        const auto gm = orbGeom();
        if (orbWo == 0) return Rectangle<float> (gm.cx - 60.0f, gm.cy + gm.R + 8.0f + 58.0f * homeGeom().s, 120.0f, 16.0f);
        return Rectangle<float> (gm.cx + 60.0f, gm.cy + gm.R + 24.0f, 90.0f, 16.0f);
    }
    Rectangle<float> orbImageClearRect() const
    {
        const auto gm = orbGeom();
        return Rectangle<float> (0, 0, 24.0f, 24.0f).withCentre ({ gm.cx, gm.cy + gm.R * 0.82f });
    }

    // ---------------------------------------------------------------- Νέα κύρια οθόνη: τρεις κύκλοι (loop του block, orb, Smart + Follow)
    // ---- επιλεκτικό repaint: ξαναζωγραφίζεται μόνο ό,τι άλλαξε (πριν ξαναζωγραφιζόταν όλη η οθόνη 30 φορές το δευτερόλεπτο)
    Rectangle<int> leftRect() const  { const auto hg = homeGeom(); const float e = hg.lr + hg.lw + 40.0f * hg.s; return Rectangle<float> (hg.lcx - e, hg.cy - e, 2.0f * e, 2.0f * e).getSmallestIntegerContainer().getIntersection (getLocalBounds()); }
    Rectangle<int> rightRect() const { const auto hg = homeGeom(); const float e = hg.lr + hg.lw + 40.0f * hg.s; return Rectangle<float> (hg.rcx - e, hg.cy - e, 2.0f * e, 2.0f * e).getSmallestIntegerContainer().getIntersection (getLocalBounds()); }
    Rectangle<int> orbRect() const   { const auto hg = homeGeom(); const float e = hg.orbR * 1.45f + 30.0f * hg.s; return Rectangle<float> (hg.ocx - e, hg.cy - e, 2.0f * e, 2.0f * e).getSmallestIntegerContainer().getIntersection (getLocalBounds()); }
    Rectangle<int> orbTextRect() const { const auto hg = homeGeom(); return Rectangle<float> (hg.ocx - 150.0f * hg.s, hg.cy + hg.orbR, 300.0f * hg.s, 90.0f * hg.s).getSmallestIntegerContainer().getIntersection (getLocalBounds()); }
    Rectangle<int> topRect() const   { return Rectangle<int> (0, 0, getWidth(), (int) (44.0f * homeGeom().s)); }
    int64 rightSig = 0, topSig = 0;

    void repaintDynamic()
    {
        if (menuOpen || orbMode || list.isVisible()) { repaint(); return; }
        const int sel = selected;
        const bool valid = sel >= 0 && sel < flow::numBlocks && engine.active[(size_t) sel].load() != 0;
        auto q = [] (float v) { return (int64) std::lround (v * 200.0f); };
        int64 h = (int64) engine.smartOn.load() * 7 + engine.smartAuto.load() * 13 + q (engine.smartAccept.load()) + (valid ? 1 : 0) + sel * 31;
        if (valid)
        {
            const auto b = (size_t) sel;
            for (int64 v : { (int64) engine.smartType[b].load(), q (engine.smartLow[b].load()), q (engine.smartMid[b].load()), q (engine.smartHigh[b].load()), q (engine.smartWidth[b].load()),
                             q (engine.smartNeedLow[b].load()), q (engine.smartNeedWidth[b].load()), q (engine.smartAppLow[b].load()), q (engine.smartAppWidth[b].load()) })
                h = h * 1000003 + v;
        }
        if (h != rightSig) { rightSig = h; repaint (rightRect()); }
        int64 t = (int64) lastChord.hashCode64() * 31 + (int64) std::lround (jlimit (0.0f, 1.0f, cpu) * 100.0f) * 7 + (midiActive ? 1 : 0) + (int64) notice.hashCode64() * 3 + chordName.hashCode64();
        if (t != topSig) { topSig = t; repaint (topRect()); repaint (getLocalBounds().removeFromBottom (40)); }
        repaint (leftRect());                       // ο κύκλος του loop κινείται συνέχεια
        repaint (orbTextRect());                    // μπάρα 16 μέτρων και ενδείξεις Rec
        if (orbSpinV > 1.0e-4f) repaint (orbRect());    // ο orb γυρίζει όσο γράφει Rec Audio
    }

    struct HomeGeom { float s, cy, lcx, lr, lw, ocx, orbR, rcx; };
    HomeGeom homeGeom() const
    {
        const float w = (float) getWidth(), h = (float) getHeight();
        HomeGeom hg;
        hg.s = jlimit (0.55f, 1.5f, jmin (w / 1065.0f, h / 500.0f));
        hg.cy = h * 0.47f;
        hg.ocx = w * 0.5f;
        hg.orbR = 190.0f * hg.s;                                 // μεγάλος orb: κυρίαρχο στοιχείο
        hg.lr = 96.0f * hg.s; hg.lw = 14.0f * hg.s;              // οι πλαϊνοί κύκλοι μικραίνουν
        hg.lcx = w * 0.155f; hg.rcx = w * (1.0f - 0.155f);   // συμμετρικοί γύρω από τον orb
        return hg;
    }
    bool homeStyle() const { return orbWo == 0; }
    Point<float> orbRingCentre (int c) const
    {
        const auto gm = orbGeom();
        const float k = gm.R * 0.91f;
        static const float sx[4] { -1.0f, 1.0f, 1.0f, -1.0f }, sy[4] { -1.0f, -1.0f, 1.0f, 1.0f };
        return { gm.cx + sx[c & 3] * k, gm.cy + sy[c & 3] * k };
    }

    std::function<void()> onSmartCycle, onFollowToggle;     // Off → Ask → Auto, Follow on/off
    std::function<void (int)> onSmartType, onSmartApply;    // block
    float orbSpin = 0.0f, orbSpinV = 0.0f;
    double lastSpinMs = 0.0;

    static String chordWord (int root, int type)
    {
        static const char* nm[12] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        return String (nm[((root % 12) + 12) % 12]) + (type == 1 ? "m" : "");
    }

    void paintHome (Graphics& g)
    {
        tipZones.clear(); hot.clear(); metroSliderRect = {};
        const auto hg = homeGeom();
        const float s = hg.s, w = (float) getWidth(), h = (float) getHeight();
        const double now = engine.beatPosOut.load();
        const Colour txtC (0xffe6e0d8), dimC (0xff968e86), trackC (0xff343130);
        const int sel = selected;
        const bool valid = sel >= 0 && sel < flow::numBlocks && engine.active[(size_t) sel].load() != 0;
        const auto tapeState = engine.recorder.getState();
        const bool taping = tapeState == RecorderEngine::State::Recording;

        // ---- γωνίες: συγχορδία, CPU, tempo (το tempo και το tap είναι components πάνω δεξιά), μήνυμα, ένδειξη MIDI
        if (chordName.isNotEmpty()) lastChord = chordName;
        if (lastChord.isNotEmpty())
        {
            g.setFont (hobby::makeFont (typeface, 18.0f * s)); g.setColour (hobby::kLilac);
            g.drawText (lastChord, Rectangle<float> (30.0f * s, 10.0f * s, 160.0f * s, 26.0f * s), Justification::centredLeft, false);
        }
        {
            const float load = jlimit (0.0f, 1.0f, cpu);
            const float R = 9.0f * s, cx0 = w * 0.5f, cy0 = 22.0f * s;
            g.setColour (Colour (0xff3a3430)); g.fillEllipse (cx0 - R, cy0 - R, 2.0f * R, 2.0f * R);
            if (load > 0.005f)
            {
                Path pie; pie.addPieSegment (Rectangle<float> (cx0 - R, cy0 - R, 2.0f * R, 2.0f * R), MathConstants<float>::pi * -0.75f, MathConstants<float>::pi * -0.75f + MathConstants<float>::pi * 1.5f * load, 0.0f);
                g.setColour (load >= 0.85f ? hobby::kRed : dimC); g.fillPath (pie);
            }
            tipZones.push_back ({ Rectangle<float> (cx0 - R, cy0 - R, 2.0f * R, 2.0f * R), String ("Processor load (CPU)") });
        }
        if (notice.isNotEmpty())
        {
            g.setFont (hobby::makeFont (typeface, 13.0f * s)); g.setColour (hobby::kIconGrey);
            g.drawText (notice, Rectangle<float> (w * 0.2f, h - 28.0f * s, w * 0.6f, 20.0f * s), Justification::centred, true);
        }
        g.setColour (midiActive ? hobby::kOn : Colour (0xff4a433d));
        g.fillEllipse (w - 14.0f * s - 7.0f, h - 14.0f * s - 7.0f, 7.0f, 7.0f);
        if (metroStrip)   // ♪ / ↺: δικός σου ήχος μετρονόμου ⇄ αρχικός
        {
            const bool custom = metroIsCustom && metroIsCustom();
            const Rectangle<float> r (w - 34.0f * s - 11.0f, 40.0f * s, 22.0f, 22.0f);
            g.setColour (custom ? hobby::kOn : Colour (0xff6b625a)); g.setFont (hobby::makeFont (typeface, 16.0f));
            g.drawText (custom ? String::fromUTF8 ("\xe2\x86\xba") : String::fromUTF8 ("\xe2\x99\xaa"), r, Justification::centred, false);
            hot.push_back ({ r.expanded (3.0f), [this, custom] { if (onMetroSample) onMetroSample (custom ? 2 : 1); } });
            tipZones.push_back ({ r, custom ? String ("Back to the default metronome sound") : String ("Choose your own metronome sound") });
        }

        // ---- αριστερός κύκλος: το loop του επιλεγμένου block στον κοινό άξονα (4 / 8 / 16 μέτρα)
        {
            const float cx = hg.lcx, cy = hg.cy, r = hg.lr, lw = hg.lw;
            const int axisBars = jmax (1, engine.axisBarsOut.load());
            const double axisBeats = (double) axisBars * flow::beatsPerBar;
            const int st = valid ? engine.loopStateOut[(size_t) sel].load() : flow::loopOff;
            const double L = valid ? jmax (0.0, engine.loopBeatsOut[(size_t) sel].load()) : 0.0;
            const int layers = valid ? engine.loopLayersOut[(size_t) sel].load() : 0;
            const bool playing = st == flow::loopPlay || st == flow::loopOver;
            const bool recLoop = st == flow::loopRec, over = st == flow::loopOver;
            const bool muted = valid && (engine.blockOff[(size_t) sel].load() != 0 || engine.loopStoppedOut[(size_t) sel].load() != 0);
            Colour base = ! valid ? trackC : muted ? Colour (0xff4a4a52) : recLoop ? hobby::kRed : over ? Colour (0xffe8883a) : blockLight (sel);
            // παλμός με το tempo: ο πρώτος χτύπος κάθε μέτρου πιο έντονος
            const double bt = std::floor (jmax (0.0, now) + 1.0e-9), frac = jmax (0.0, now - bt);
            const bool barStart = std::fmod (bt, flow::beatsPerBar) < 1.0e-6;
            const float decay = jlimit (0.0f, 1.0f, 1.0f - (float) frac * 2.8f);
            const float pulse = (valid && (playing || recLoop)) ? decay * decay * (barStart ? 0.9f : 0.35f) : 0.0f;

            Path track; track.addCentredArc (cx, cy, r, r, 0.0f, 0.0f, MathConstants<float>::twoPi, true);
            g.setColour (trackC.interpolatedWith (Colour (0xfff0ebe2), pulse * 0.45f));
            g.strokePath (track, PathStrokeType (lw));
            g.setColour (hobby::kScreen);
            for (int i = 0; i < axisBars; ++i)   // ένα σημάδι ανά μέτρο
            {
                const float a = MathConstants<float>::twoPi * (float) i / (float) axisBars;
                const float dx = std::sin (a), dy = -std::cos (a);
                g.drawLine (cx + dx * (r - lw * 0.5f - 1.0f), cy + dy * (r - lw * 0.5f - 1.0f), cx + dx * (r + lw * 0.5f + 1.0f), cy + dy * (r + lw * 0.5f + 1.0f), jmax (1.5f, 2.0f * s));
            }
            double span = 0.0;
            if (playing) span = jmin (axisBeats, jmax (0.25, L));
            else if (recLoop)
            {
                const double elapsed = jmax (0.0, now - engine.recStartBeatOut[(size_t) sel].load());
                span = jmin (axisBeats, elapsed);
            }
            if (span > 0.0)
            {
                const float f = (float) (span / axisBeats);
                Path arc; arc.addCentredArc (cx, cy, r, r, 0.0f, 0.0f, MathConstants<float>::twoPi * jmin (0.9999f, f), true);
                g.setColour (base.interpolatedWith (Colours::white, pulse));
                g.strokePath (arc, PathStrokeType (lw));
                if (f > 0.0f)   // επανάληψη του κύκλου μέσα στον άξονα: αχνά τμήματα
                    for (double k = span; playing && k < axisBeats - 1.0e-6; k += span)
                    {
                        const float a0 = MathConstants<float>::twoPi * (float) (k / axisBeats);
                        const float a1 = MathConstants<float>::twoPi * (float) (jmin (axisBeats, k + span) / axisBeats);
                        Path rp; rp.addCentredArc (cx, cy, r, r, 0.0f, a0, jmin (a1, a0 + MathConstants<float>::twoPi * 0.9999f), true);
                        g.setColour (base.withAlpha (0.28f));
                        g.strokePath (rp, PathStrokeType (lw));
                    }
            }
            if (valid && (playing || recLoop))   // playhead
            {
                const double headBeats = playing ? axisBeats : axisBeats;
                const float a = MathConstants<float>::twoPi * (float) (std::fmod (jmax (0.0, now), headBeats) / axisBeats);
                const float dx = std::sin (a), dy = -std::cos (a);
                g.setColour (Colours::white);
                g.drawLine (cx + dx * (r - lw * 0.5f - 5.0f * s), cy + dy * (r - lw * 0.5f - 5.0f * s), cx + dx * (r + lw * 0.5f + 5.0f * s), cy + dy * (r + lw * 0.5f + 5.0f * s), jmax (2.0f, 3.0f * s));
            }
            // κέντρο: μήκος και στρώματα
            g.setColour (valid ? txtC : dimC);
            if (playing && L > 0.0)
            {
                const int bars = jmax (1, (int) std::lround (L / flow::beatsPerBar));
                g.setFont (hobby::makeFont (typeface, 24.0f * s).boldened());
                g.drawText (String (bars) + (bars == 1 ? " bar" : " bars"), Rectangle<float> (cx - r, cy - 26.0f * s, 2.0f * r, 30.0f * s), Justification::centred, false);
                g.setFont (hobby::makeFont (typeface, 13.0f * s)); g.setColour (dimC);
                g.drawText (String (layers) + (layers == 1 ? " layer" : " layers"), Rectangle<float> (cx - r, cy + 6.0f * s, 2.0f * r, 20.0f * s), Justification::centred, false);
            }
            else
            {
                g.setFont (hobby::makeFont (typeface, 20.0f * s).boldened());
                g.setColour (recLoop ? hobby::kRed : dimC);
                g.drawText (! valid ? String ("-") : recLoop ? String (over ? "Overdub" : "Rec") : String ("ready"), Rectangle<float> (cx - r, cy - 16.0f * s, 2.0f * r, 32.0f * s), Justification::centred, false);
            }
            g.setColour (dimC); g.setFont (hobby::makeFont (typeface, 12.0f * s).boldened());
            if (valid) g.drawText ("Block " + String (sel + 1), Rectangle<float> (cx - r, cy - r - lw - 26.0f * s, 2.0f * r, 18.0f * s), Justification::centred, false);
            g.setFont (hobby::makeFont (typeface, 11.0f * s));
            g.drawText ("axis " + String (axisBars) + " bars", Rectangle<float> (cx - r, cy + r + lw * 0.5f + 8.0f * s, 2.0f * r, 16.0f * s), Justification::centred, false);
            if (recLoop || over)
            {
                g.setColour (over ? hobby::kPeach : hobby::kRed);
                g.drawText (over ? "Overdub (Rec MIDI loop)" : "Rec MIDI loop", Rectangle<float> (cx - r, cy + r + lw * 0.5f + 26.0f * s, 2.0f * r, 16.0f * s), Justification::centred, false);
            }
        }

        // ---- κεντρικός κύκλος: orb (πρώτο πλάνο) + 4 δαχτυλίδια γύρω του (τρίτο πλάνο)
        {
            const double tn = Time::getMillisecondCounterHiRes();
            const float dt = lastSpinMs > 0.0 ? jlimit (0.0f, 0.2f, (float) ((tn - lastSpinMs) * 0.001)) : 0.0f;
            lastSpinMs = tn;
            orbSpinV += ((taping ? 0.25f : 0.0f) - orbSpinV) * jmin (1.0f, dt * 1.5f);   // περίπου μία στροφή ανά 25 δλ, ομαλή επιτάχυνση και επιβράδυνση
            orbSpin = std::fmod (orbSpin + orbSpinV * dt, MathConstants<float>::twoPi);
            paintOrb (g);
            const float rr = 14.0f * s;
            for (int c = 0; c < 4; ++c)
            {
                bool any = false;
                for (int r = 0; r < 4; ++r) any = any || knobs[(size_t) (c * 4 + r)].bound;
                const auto pc = orbRingCentre (c);
                const bool learningHere = orbLearning >= c * 4 && orbLearning < c * 4 + 4;
                Path ring; ring.addEllipse (pc.x - rr, pc.y - rr, 2.0f * rr, 2.0f * rr);
                Colour rc = any ? Colour (0xff48443f) : Colour (0xff35312e);
                if (c == orbDrag) rc = Colour (0xff9a948b);
                if (learningHere) rc = hobby::kOn.withAlpha (0.35f + 0.65f * (float) hobby::pulse (tn * 0.001 * 2.0));
                g.setColour (rc);
                g.strokePath (ring, PathStrokeType (jmax (2.5f, 4.0f * s)));
            }
            // ενδείξεις κάτω από τον orb: Rec Audio και Rec motion
            float ty = gmBottom();
            if (orbRec)
            {   // μπάρα θέσης στον κύκλο των 16 μέτρων: εμφανίζεται μόνο όσο γράφει το Rec motion
                const double cyc = 64.0;
                const float ph = (float) (std::fmod (jmax (0.0, engine.beatPosOut.load()), cyc) / cyc);
                const float bw = 240.0f * s, bx = hg.ocx - bw * 0.5f, by = ty + 16.0f * s, bh = jmax (3.0f, 4.0f * s);
                g.setColour (Colour (0xff3a3430)); g.fillRoundedRectangle (bx, by, bw, bh, bh * 0.5f);
                g.setColour (orbRec ? hobby::kCoral : Colour (0xffc9c3ba)); g.fillRoundedRectangle (bx, by, jmax (bh, bw * ph), bh, bh * 0.5f);
                g.setFont (hobby::makeFont (typeface, 11.0f * s)); g.setColour (dimC);
                g.drawText (String (jlimit (1, 16, (int) (ph * 16.0f) + 1)) + " / 16", Rectangle<float> (bx - 62.0f * s, by - 6.0f * s, 56.0f * s, 16.0f * s), Justification::centredRight, false);
                ty += 28.0f * s;
            }
            g.setFont (hobby::makeFont (typeface, 13.0f * s));
            if (taping || engine.recorder.isZipping() || tapeState == RecorderEngine::State::Error)
            {
                String t = tapeState == RecorderEngine::State::Error ? String ("ERROR") : clockText (engine.recorder.getElapsedSeconds());
                if (engine.recorder.isZipping()) t = "...";
                g.setColour (hobby::kRed);
                g.drawText (t, Rectangle<float> (hg.ocx - 120.0f * s, ty, 240.0f * s, 18.0f * s), Justification::centred, false);
                ty += 18.0f * s;
            }
            if (orbRec)
            {
                g.setColour (hobby::kCoral);
                g.drawText ("Rec motion", Rectangle<float> (hg.ocx - 120.0f * s, ty, 240.0f * s, 18.0f * s), Justification::centred, false);
                ty += 18.0f * s;
            }
            if (orbMotion)
            {
                g.setColour (dimC);
                g.drawText (String::fromUTF8 ("\xe2\x9c\x95 motion"), orbBottomRect(), Justification::centred, false);
            }
        }

        // ---- δεξιός κύκλος: Smart master (δαχτυλίδια Low / Mid / High + κύκλος stereo) και Follow
        paintSmartCircle (g, hg, valid);
    }

    float gmBottom() const { const auto gm = orbGeom(); return gm.cy + gm.R + 8.0f * homeGeom().s; }

    void paintSmartCircle (Graphics& g, const HomeGeom& hg, bool valid)
    {
        const float s = hg.s, cx = hg.rcx, cy = hg.cy, r = hg.lr, lw = hg.lw;
        const Colour txtC (0xffe6e0d8), dimC (0xff968e86), trackC (0xff343130), okC = hobby::kOn, amber (0xffebb45a);
        auto back = [] (Colour c) { return c.interpolatedWith (hobby::kScreen, 0.45f); };
        const bool on = engine.smartOn.load() != 0;
        const int mode = engine.smartAuto.load();
        const float acc = engine.smartAccept.load();
        const int sel = selected;
        const int type = valid ? engine.smartType[(size_t) sel].load() : 0;
        static const char* typeNames[] { "Other", "Bass", "Keys", "Pad", "Lead", "Strings", "Drums", "FX" };
        const Colour bc = valid ? blockLight (sel) : Colour (0xff6b625a);
        const bool active = on && type != 0;
        const Colour cols[3] { back (bc.interpolatedWith (Colours::black, 0.25f)), back (bc), back (bc.interpolatedWith (Colours::white, 0.45f)) };
        float lim[4]; flow::smartLimits (type, lim);
        const float vals[3] { valid ? engine.smartLow[(size_t) sel].load() : 0.0f, valid ? engine.smartMid[(size_t) sel].load() : 0.0f, valid ? engine.smartHigh[(size_t) sel].load() : 0.0f };
        const float width = valid ? engine.smartWidth[(size_t) sel].load() : 0.0f;
        const float full = degreesToRadians (270.0f), start = degreesToRadians (-135.0f);
        for (int k = 0; k < 3; ++k)
        {
            const float rad = r - (float) k * (lw + 4.0f * s) + 0.0f;
            const bool isOver = active && vals[k] > lim[k];
            Path tp; tp.addCentredArc (cx, cy, rad, rad, 0.0f, start, start + full, true);
            g.setColour (trackC); g.strokePath (tp, PathStrokeType (lw));
            if (vals[k] > 0.003f)
            {
                Path vp; vp.addCentredArc (cx, cy, rad, rad, 0.0f, start, start + full * jlimit (0.0f, 1.0f, vals[k]), true);
                g.setColour (isOver ? back (hobby::kRed) : cols[k]); g.strokePath (vp, PathStrokeType (lw));
            }
            if (active && lim[k] < 0.999f)
            {
                const float a = start + full * lim[k];
                const float dx = std::sin (a), dy = -std::cos (a);
                g.setColour (txtC.withAlpha (0.8f));
                g.drawLine (cx + dx * (rad - lw * 0.5f - 2.0f), cy + dy * (rad - lw * 0.5f - 2.0f), cx + dx * (rad + lw * 0.5f + 2.0f), cy + dy * (rad + lw * 0.5f + 2.0f), jmax (1.4f, 1.6f * s));
            }
        }
        // κέντρο: κύκλος του πλάτους stereo. Πάτημα = Off → Ask → Auto
        {
            const float sr = 26.0f * s;
            g.setColour (Colour (0xff4a4642)); g.drawEllipse (cx - sr, cy - sr, 2.0f * sr, 2.0f * sr, 2.0f);
            const float ew = jmax (1.5f, width * sr);
            const bool wOver = active && width > lim[3];
            g.setColour (! on ? Colour (0xff4a4642) : wOver ? back (hobby::kRed) : cols[1]);
            g.fillEllipse (cx - ew, cy - sr + 3.0f, 2.0f * ew, 2.0f * sr - 6.0f);
            if (on)
            {
                g.setColour (mode == 1 ? okC : dimC);
                g.setFont (hobby::makeFont (typeface, 10.0f * s).boldened());
                g.drawText (mode == 1 ? "Auto" : "Ask", Rectangle<float> (cx - 30.0f * s, cy + sr + 3.0f * s, 60.0f * s, 14.0f * s), Justification::centred, false);
            }
            const Rectangle<float> hit (cx - sr - 8.0f, cy - sr - 8.0f, 2.0f * sr + 16.0f, 2.0f * sr + 16.0f);
            hot.push_back ({ hit, [this] { if (onSmartCycle) onSmartCycle(); } });
            tipZones.push_back ({ hit, String (on ? (mode == 1 ? "Smart master: Auto (press = Off)" : "Smart master: Ask (press = Auto)") : "Smart master: Off (press = Ask)") });
        }
        // τύπος ήχου: πάτημα = επόμενος τύπος
        {
            const Rectangle<float> tr (cx - 70.0f * s, cy - r - lw * 0.5f - 34.0f * s, 140.0f * s, 24.0f * s);
            g.setColour (type != 0 ? txtC : dimC); g.setFont (hobby::makeFont (typeface, 16.0f * s).boldened());
            g.drawText (valid ? String (typeNames[jlimit (0, 7, type)]) : String ("-"), tr, Justification::centred, false);
            if (valid) { hot.push_back ({ tr, [this, b = selected] { if (onSmartType) onSmartType (b); } }); tipZones.push_back ({ tr, String ("Sound type of the block (press = next)") }); }
        }
        // κάτω: διακόπτης Follow, πρόταση, ✓
        {
            const float yb = cy + r + lw * 0.5f + 28.0f * s, fr = 13.0f * s;

            const float needLow = valid ? engine.smartNeedLow[(size_t) sel].load() : 0.0f, needW = valid ? engine.smartNeedWidth[(size_t) sel].load() : 1.0f;
            const float appLow = valid ? engine.smartAppLow[(size_t) sel].load() : 0.0f, appW = valid ? engine.smartAppWidth[(size_t) sel].load() : 1.0f;
            const bool nL = active && needLow > 0.5f, nW = active && needW < 0.97f, applied = appLow > 0.2f || appW < 0.98f, need = nL || nW;
            String msg;
            if (! valid) msg = "";
            else if (! on) msg = "Smart off";
            else if (type == 0) msg = "Pick a sound type";
            else if (need)
            {
                if (nL) msg = "Lows -" + String (needLow * acc, 1) + " dB";
                if (nW) msg += String (msg.isEmpty() ? "" : "  ") + "Width " + String ((int) std::round (100.0f - (1.0f - needW) * acc * 100.0f)) + " %";
            }
            else msg = applied ? "Correcting" : "In range";
            g.setColour (need ? amber : dimC); g.setFont (hobby::makeFont (typeface, 12.0f * s).boldened());
            g.drawText (msg, Rectangle<float> (cx - 60.0f * s, yb - 9.0f * s, 150.0f * s, 18.0f * s), Justification::centred, true);
            if (valid && on && mode == 0 && type != 0 && (need || applied) && acc > 0.0f)
            {
                const Point<float> kc (cx + 96.0f * s, yb);
                const float kr = 15.0f * s;
                g.setColour (applied ? Colour (0xff3c5a46) : Colour (0xff35493c)); g.fillEllipse (kc.x - kr, kc.y - kr, 2.0f * kr, 2.0f * kr);
                g.setColour (okC); g.drawEllipse (kc.x - kr, kc.y - kr, 2.0f * kr, 2.0f * kr, applied ? 2.4f : 1.6f);
                Path ck; ck.startNewSubPath (kc.x - 6.0f * s, kc.y); ck.lineTo (kc.x - 2.0f * s, kc.y + 5.0f * s); ck.lineTo (kc.x + 7.0f * s, kc.y - 5.0f * s);
                g.setColour (txtC); g.strokePath (ck, PathStrokeType (2.6f, PathStrokeType::curved, PathStrokeType::rounded));
                hot.push_back ({ Rectangle<float> (kc.x - kr - 4.0f, kc.y - kr - 4.0f, 2.0f * kr + 8.0f, 2.0f * kr + 8.0f), [this, b = selected] { if (onSmartApply) onSmartApply (b); } });
            }
        }
    }

    void paintQr (Graphics& g)
    {
        const int w = getWidth(), h = getHeight();
        g.setFont (hobby::makeFont (typeface, 13.0f));
        g.setColour (Colour (0xffc9c3ba));
        g.drawText ("ORB \xc2\xb7 iPad", 40, 8, w - 80, 22, Justification::centred, true);
        const auto code = qr::encode (qrUrl.toStdString());
        if (code.size <= 0) return;
        const int quiet = 3, avail = jmax (60, h - 90);
        const int px = jmax (2, avail / (code.size + 2 * quiet));
        const int side = px * (code.size + 2 * quiet);
        const int textW = 300;
        const int x0 = jmax (14, (w - side - 28 - textW) / 2), y0 = jmax (40, (h - side) / 2 + 8);
        g.setColour (Colours::white);
        g.fillRoundedRectangle ((float) x0, (float) y0, (float) side, (float) side, 10.0f);
        g.setColour (Colours::black);
        for (int y = 0; y < code.size; ++y)
            for (int x = 0; x < code.size; ++x)
                if (code.dark (x, y)) g.fillRect (x0 + (x + quiet) * px, y0 + (y + quiet) * px, px, px);
        const int tx = x0 + side + 28;
        g.setColour (Colour (0xffe6e0d8)); g.setFont (hobby::makeFont (typeface, 16.0f).boldened());
        g.drawText (CharPointer_UTF8 ("\xce\xa3\xce\xba\xce\xac\xce\xbd\xce\xb1\xcf\x81\xce\xb5 \xce\xbc\xce\xb5 \xcf\x84\xce\xb7\xce\xbd \xce\xba\xce\xac\xce\xbc\xce\xb5\xcf\x81\xce\xb1"), tx, y0 + 8, textW, 22, Justification::centredLeft, false);
        g.drawText ("of the iPad", tx, y0 + 30, textW, 22, Justification::centredLeft, false);
        g.setColour (Colour (0xff968e86)); g.setFont (hobby::makeFont (typeface, 12.0f));
        g.drawFittedText (qrUrl, tx, y0 + 70, textW, 36, Justification::topLeft, 2);
        g.setColour (qrClients > 0 ? hobby::kOn : Colour (0xff968e86)); g.setFont (hobby::makeFont (typeface, 13.0f));
        g.drawText ("iPad connected: " + String (qrClients), tx, y0 + 118, textW, 20, Justification::centredLeft, false);
        g.setColour (Colour (0xff786f67)); g.setFont (hobby::makeFont (typeface, 11.0f));
        g.drawFittedText ("Same Wi-Fi as the Mac. Press ORB again to close.", tx, y0 + 150, textW, 32, Justification::topLeft, 2);
    }

    void paintOrb (Graphics& g)
    {
        const auto gm = orbGeom();
        const float cx = gm.cx, cy = gm.cy, R = gm.R;
        const double nowMs = Time::getMillisecondCounterHiRes();

        const bool home = orbWo == 0 && ! orbMode;
        if (! home) { g.setFont (hobby::makeFont (typeface, 13.0f));
        g.setColour (Colour (0xffc9c3ba));
        g.drawText (title == "FLOW" ? String ("ORB") : title, 40, 8, orbW() - 80, 22, Justification::centred, true);   // χωρίς επιλεγμένο block γράφει ORB
        }

        // ο orb: δική σου εικόνα (σώζεται με το set), αλλιώς ουδέτερη κουκκιδωτή επιφάνεια
        if (home)
        {
            // στην κύρια οθόνη ο δίσκος φτιάχνεται μία φορά σε εικόνα (cache) και μετά απλώς αντιγράφεται: χωρίς ανακλίμακα εικόνας σε κάθε καρέ
            const float scale = (float) g.getInternalContext().getPhysicalPixelScaleFactor();
            const int side = jmax (8, roundToInt (2.0f * R * scale));
            const void* key = orbImage.isValid() ? (const void*) orbImage.getPixelData().get() : nullptr;
            if (! orbCache.isValid() || orbCache.getWidth() != side || orbCacheKey != key)
            {
                orbCacheKey = key;
                orbCache = Image (Image::ARGB, side, side, true);
                Graphics cg (orbCache);
                cg.addTransform (AffineTransform::scale ((float) side / (2.0f * R)));
                Path cc; cc.addEllipse (0.0f, 0.0f, 2.0f * R, 2.0f * R);
                cg.setColour (Colour (0xff2e2a27)); cg.fillPath (cc);
                cg.reduceClipRegion (cc);
                if (orbImage.isValid())
                    cg.drawImage (orbImage, Rectangle<float> (0.0f, 0.0f, 2.0f * R, 2.0f * R), RectanglePlacement::fillDestination | RectanglePlacement::centred);
                else
                {
                    cg.setTiledImageFill (dotGaps(), 0, 0, 1.0f);
                    cg.fillRect (Rectangle<float> (0.0f, 0.0f, 2.0f * R, 2.0f * R));
                }
            }
            if (std::abs (orbSpin) > 1.0e-4f)
                g.drawImageTransformed (orbCache, AffineTransform::scale (2.0f * R / (float) side).translated (-R, -R).rotated (orbSpin).translated (cx, cy));
            else
                g.drawImage (orbCache, Rectangle<float> (cx - R, cy - R, 2.0f * R, 2.0f * R), RectanglePlacement::stretchToFit);
        }
        else
        {
        Path circle; circle.addEllipse (cx - R, cy - R, 2.0f * R, 2.0f * R);
        g.setColour (Colour (0xff2e2a27));
        g.fillPath (circle);
        g.saveState();
        g.reduceClipRegion (circle);
        if (orbImage.isValid())
            g.drawImage (orbImage, Rectangle<float> (cx - R, cy - R, 2.0f * R, 2.0f * R), RectanglePlacement::fillDestination | RectanglePlacement::centred);
        else
        {
            g.setTiledImageFill (dotGaps(), 0, 0, 1.0f);
            g.fillRect (Rectangle<float> (cx - R, cy - R, 2.0f * R, 2.0f * R));
        }
        g.restoreState();
        }
        g.setColour (Colour (0xff4a423c));
        g.drawEllipse (cx - R, cy - R, 2.0f * R, 2.0f * R, 1.4f);
        g.setFont (hobby::makeFont (typeface, 11.0f));
        if (home) {}
        else if (! orbImage.isValid())
        {
            g.setColour (Colour (0xff78706a));
            g.drawText ("+ image", Rectangle<float> (cx - 50.0f, cy + R * 0.7f, 100.0f, 18.0f), Justification::centred, false);
        }
        else
        {
            g.setColour (Colours::white.withAlpha (0.55f));
            g.drawText (String::fromUTF8 ("\xe2\x9c\x95"), orbImageClearRect(), Justification::centred, false);
        }

        // κύκλοι που αιωρούνται: σκούρα μαλακή σκιά από κάτω. Όταν κρατάς έναν, σηκώνεται και η σκιά του απομακρύνεται και θολώνει.
        for (int c = 0; c < 4; ++c)
        {
            const auto p = orbDotPoint (c);
            const float r = c == orbDrag ? 12.0f : 10.5f;
            const float lift = orbLift[(size_t) c];
            const float bob = 0.5f + 0.5f * (float) std::sin (nowMs * 0.0018 + (double) c * 1.7);
            const float h = 4.0f + 1.5f * bob * (1.0f - lift) + 9.0f * lift;     // «ύψος» σε εικονοστοιχεία

            {
                // στρογγυλή, πολύ θολή σκιά ακριβώς κάτω από την τελεία· όταν την κρατάς απομακρύνεται και θολώνει λίγο
                const float cxs = p.x, cys = p.y + 18.0f + 1.5f * bob * (1.0f - lift) + 8.0f * lift;
                const float R = (r + 22.0f) * (1.0f + 0.35f * lift);
                const float a = 0.155f * (1.0f - 0.25f * lift);
                ColourGradient gr (Colours::black.withAlpha (a), cxs, cys, Colours::transparentBlack, cxs + R, cys, true);
                gr.addColour (0.35, Colours::black.withAlpha (a * 0.78f));
                gr.addColour (0.70, Colours::black.withAlpha (a * 0.22f));
                g.setGradientFill (gr);
                g.fillEllipse (cxs - R, cys - R, 2.0f * R, 2.0f * R);
            }

            const float py = p.y - h * 0.45f;
            g.setColour (Colour (0xfffaf8f4));      // άσπρη τελεία (τα χρώματα μένουν στις λίστες)
            g.fillEllipse (p.x - r, py - r, 2.0f * r, 2.0f * r);
        }

        // λίστες αναθέσεων στις 4 γωνίες
        for (int c = 0; c < 4 && ! home; ++c)
        {
            const auto col = orbColour (c);
            const bool left = orbLeft (c);
            const auto o = orbListOrigin (c);
            const auto dr = orbDotRect (c);
            const bool learningHere = orbLearning >= c * 4 && orbLearning < c * 4 + 4;
            if (learningHere)
            {
                g.setColour (hobby::kOn.withAlpha (0.35f + 0.65f * (float) hobby::pulse (nowMs * 0.001 * 2.0)));
                g.drawEllipse (dr.reduced (3.0f), 2.0f);
            }
            g.setColour (col);
            g.fillEllipse (dr.withSizeKeepingCentre (10.0f, 10.0f));
            for (int r = 0; r < 4; ++r)
            {
                const int slot = c * 4 + r;
                const auto& ks = knobs[(size_t) slot];
                const float y = o.y + 16.0f * (float) r - 8.0f;
                const bool hov = slot == orbHover && ks.bound;
                if (ks.bound)
                {
                    g.setFont (hobby::makeFont (typeface, 12.0f));
                    g.setColour (Colour (0xffe6e0d8));
                    const String pct = String (roundToInt (jlimit (0.0f, 1.0f, ks.value) * 100.0f)) + "%";
                    g.setFont (hobby::makeFont (typeface, 10.0f));
                    g.setColour (hov ? hobby::kRed : Colour (0xff9a8f84));
                    const String nm = hov ? String::fromUTF8 ("\xe2\x9c\x95 ") + ks.name : ks.name;
                    if (left)
                    {
                        g.setFont (hobby::makeFont (typeface, 12.0f)); g.setColour (Colour (0xffe6e0d8));
                        g.drawText (pct, Rectangle<float> (o.x - 40.0f, y, 40.0f, 16.0f), Justification::centredRight, false);
                        g.setFont (hobby::makeFont (typeface, 10.0f)); g.setColour (hov ? hobby::kRed : Colour (0xff9a8f84));
                        g.drawText (nm, Rectangle<float> (o.x - 170.0f, y, 126.0f, 16.0f), Justification::centredRight, true);
                    }
                    else
                    {
                        g.setFont (hobby::makeFont (typeface, 12.0f)); g.setColour (Colour (0xffe6e0d8));
                        g.drawText (pct, Rectangle<float> (o.x, y, 40.0f, 16.0f), Justification::centredLeft, false);
                        g.setFont (hobby::makeFont (typeface, 10.0f)); g.setColour (hov ? hobby::kRed : Colour (0xff9a8f84));
                        g.drawText (nm, Rectangle<float> (o.x + 44.0f, y, 126.0f, 16.0f), Justification::centredLeft, true);
                    }
                }
                else
                {
                    const bool waiting = slot == orbLearning;
                    g.setFont (hobby::makeFont (typeface, 10.0f));
                    g.setColour (waiting ? hobby::kOn : Colour (0xff5f5852));
                    g.drawText (waiting ? String (String::fromUTF8 ("touch a parameter\xe2\x80\xa6")) : String (String::fromUTF8 ("\xe2\x80\x94")),
                                left ? Rectangle<float> (o.x - 170.0f, y, 170.0f, 16.0f) : Rectangle<float> (o.x, y, 170.0f, 16.0f),
                                left ? Justification::centredRight : Justification::centredLeft, false);
                }
            }
        }

        // μπάρα θέσης στον κύκλο + ένδειξη εγγραφής κίνησης
        if (! home)
        {
            const double cyc = 64.0;   // 16 μέτρα
            const float ph = (float) (std::fmod (jmax (0.0, engine.beatPosOut.load()), cyc) / cyc);
            const float bw = 240.0f, bx = cx - bw * 0.5f, by = cy + R + 12.0f;
            g.setColour (Colour (0xff3a3430));
            g.fillRoundedRectangle (bx, by, bw, 3.0f, 1.5f);
            g.setColour (Colour (0xffc9c3ba));
            g.fillRoundedRectangle (bx, by, bw * ph, 3.0f, 1.5f);
            g.setFont (hobby::makeFont (typeface, 10.0f));
            if (orbRec)
            {
                g.setColour (hobby::kCoral);
                g.drawText ("REC motion", Rectangle<float> (bx, by + 8.0f, bw, 16.0f), Justification::centred, false);
            }
            g.setColour (Colour (0xff9a8f84));
            g.drawText (String (jlimit (1, 16, (int) (ph * 16.0f) + 1)) + " / 16", Rectangle<float> (cx - bw * 0.5f - 60.0f, by - 7.0f, 54.0f, 16.0f), Justification::centredRight, false);
            if (orbMotion)
            {
                g.setColour (Colour (0xff9a8f84));
                g.drawText (String::fromUTF8 ("\xe2\x9c\x95 motion"), orbBottomRect(), Justification::centredRight, false);
            }
        }
    }

    void orbMouseDown (Point<float> p)
    {
        for (int c = 0; c < 4; ++c)
            if (orbDotRect (c).contains (p))
            {
                for (int r = 0; r < 4; ++r)
                    if (! knobs[(size_t) (c * 4 + r)].bound) { if (onOrbAssign) onOrbAssign (c * 4 + r); return; }
                return;
            }
        for (int slot = 0; slot < 16; ++slot)
            if (orbRowRect (slot).contains (p))
            {
                if (knobs[(size_t) slot].bound) { if (onOrbClear) onOrbClear (slot); }
                else if (onOrbAssign) onOrbAssign (slot);
                return;
            }
        for (int c = 0; c < 4; ++c)
            if (orbDotPoint (c).getDistanceFrom (p) <= 18.0f)
            {
                orbDrag = c;
                if (onOrbTouch) onOrbTouch (c, true);
                orbDragTo (p);
                return;
            }
        if (orbMotion && orbBottomRect().contains (p)) { if (onOrbClearMotion) onOrbClearMotion(); return; }
        if (orbImage.isValid()) { if (orbImageClearRect().contains (p) && onOrbImageClear) onOrbImageClear(); return; }
        const auto gm = orbGeom();
        if (p.getDistanceFrom ({ gm.cx, gm.cy }) <= gm.R && onOrbImage) onOrbImage();
    }

    void orbDragTo (Point<float> p)
    {
        if (orbDrag < 0) return;
        const auto gm = orbGeom();
        const float k = gm.R - 12.0f;
        if (onOrbMove) onOrbMove (orbDrag, { (p.x - gm.cx) / k, (p.y - gm.cy) / k });
    }

    // Αποσπασμένος orb (δικό του παράθυρο, π.χ. στο iPad): το OrbPanel ζωγραφίζει και στέλνει τα κλικ εδώ
    Image orbCache; const void* orbCacheKey = nullptr;
    Component* orbExternal = nullptr;
    int orbWo = 0, orbHo = 0;
    int orbW() const { return orbWo > 0 ? orbWo : getWidth(); }
    int orbH() const { return orbHo > 0 ? orbHo : getHeight(); }
    void extPaint (Graphics& g, int w, int h) { orbWo = w; orbHo = h; paintOrb (g); orbWo = orbHo = 0; }
    void extMouseDown (Point<float> p, int w, int h) { orbWo = w; orbHo = h; orbMouseDown (p); orbWo = orbHo = 0; }
    void extMouseDrag (Point<float> p, int w, int h) { orbWo = w; orbHo = h; orbDragTo (p); orbWo = orbHo = 0; }
    void extMouseUp()
    {
        if (orbDrag >= 0) { if (onOrbTouch) onOrbTouch (orbDrag, false); orbDrag = -1; if (orbExternal) orbExternal->repaint(); }
    }
    void extMouseMove (Point<float> p, int w, int h)
    {
        orbWo = w; orbHo = h;
        int hv = -1;
        for (int slot = 0; slot < 16; ++slot) if (orbRowRect (slot).contains (p)) hv = slot;
        orbWo = orbHo = 0;
        if (hv != orbHover) { orbHover = hv; if (orbExternal) orbExternal->repaint(); }
    }

    void mouseDown (const MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) return;
        if (orbMode) { if (! qrMode) orbMouseDown (e.position); return; }
        if (menuOpen)
        {
            for (auto& z : menuHot)
                if (z.r.contains (e.position)) { auto fn = z.fn; fn(); repaint(); return; }
            if (menuPanel.contains (e.position)) return;
            openMenu (false);    // κλικ έξω από το πάνελ: κλείνει και συνεχίζει κανονικά
        }
        if (metroStrip && ! metroSliderRect.isEmpty() && metroSliderRect.expanded (8.0f, 10.0f).contains (e.position))
        {
            draggingMetro = true;
            setMetroLevelFromX (e.position.x);
            return;
        }
        for (auto& h : hot)
            if (h.first.contains (e.position)) { auto fn = h.second; fn(); repaint(); return; }
        // δαχτυλίδια γύρω από τον orb: πάτημα = learn στην πρώτη ελεύθερη ανάθεση της γωνίας
        for (int c = 0; c < 4; ++c)
            if (orbRingCentre (c).getDistanceFrom (e.position) <= 14.0f * homeGeom().s + 8.0f)
            {
                for (int r = 0; r < 4; ++r)
                    if (! knobs[(size_t) (c * 4 + r)].bound) { if (onOrbAssign) onOrbAssign (c * 4 + r); return; }
                return;
            }
        for (int c = 0; c < 4; ++c)
            if (orbDotPoint (c).getDistanceFrom (e.position) <= 18.0f)
            {
                orbDrag = c;
                if (onOrbTouch) onOrbTouch (c, true);
                orbDragTo (e.position);
                return;
            }
        if (orbMotion && orbBottomRect().contains (e.position)) { if (onOrbClearMotion) onOrbClearMotion(); return; }
    }

    String getTooltip() override
    {
        const auto p = getMouseXYRelative().toFloat();
        for (auto& z : tipZones)
            if (z.first.contains (p)) return z.second;
        return {};
    }

    void mouseDrag (const MouseEvent& e) override
    {
        if (orbMode) { if (! qrMode) orbDragTo (e.position); return; }
        if (orbDrag >= 0) orbDragTo (e.position);
        if (draggingMetro) setMetroLevelFromX (e.position.x);
    }
    void mouseUp (const MouseEvent&) override { if (orbDrag >= 0) { if (onOrbTouch) onOrbTouch (orbDrag, false); orbDrag = -1; repaint(); } if (draggingMetro) { draggingMetro = false; if (onMetroChange) onMetroChange(); } }

private:
    void setMetroLevelFromX (float px)
    {
        engine.metroLevel = jlimit (0.0f, 1.0f, (px - metroSliderRect.getX()) / metroSliderRect.getWidth());
        repaint();
    }

    Rectangle<float> metroSliderRect;
    bool draggingMetro = false;
    String lastChord;
    int orbDrag = -1, orbHover = -1, orbLearning = -1;
    bool orbRec = false, orbMotion = false;
    std::array<Point<float>, 4> orbPos {};
    std::array<float, 4> orbLift { { 0.0f, 0.0f, 0.0f, 0.0f } };
    std::vector<std::pair<Rectangle<float>, String>> tipZones;   // μπαλούν βοήθειας ανά φωτάκι
    flow::Engine& engine;
    const int& selected;
    Typeface::Ptr typeface;
    TempoBox tempoBox;
    TapDot tapDot;
    PluginListView list;
    int stepOut = 0, topOut = 48;
    std::vector<std::pair<Rectangle<float>, std::function<void()>>> hot;   // κλικ-ζώνες φωτακιών στη λωρίδα μετρονόμου
    std::vector<ToolLamp> tools;

    struct MenuZone { Rectangle<float> r; std::function<void()> fn; int idx; };
    std::vector<MenuZone> menuHot;
    std::vector<MenuEntry> menuItems;
    std::vector<std::pair<String, String>> menuInfo;
    Rectangle<float> menuPanel;
    int menuSub = -1, menuHover = -1;
    bool setsOpen = false;
    int setsScroll = 0;
    std::vector<SetItem> setItems;
};

//==============================================================================
// Πληκτρολόγιο 88 πλήκτρων. Πάνω από κάθε ζώνη: κυκλάκια (αριθμός, hold, καμπύλη έντασης, transpose) και γραμμή έντασης.
class KeyboardView : public Component
{
public:
    KeyboardView (flow::Engine& e, const int& sel, const String& msg, Typeface::Ptr face)
        : engine (e), selected (sel), message (msg), typeface (face) {}

    static String chordNameFor (int mask, int bass) { return detectChord (mask, bass); }

    std::function<void (int)> onSelect, onRedoZone, onCurve, onVolumeMenu, onHold;
    std::function<void (int)> onRemoveBlock;
    std::function<void (int)> onToggleOff;   // (δεν χρησιμοποιείται πια από τον αριθμό)
    std::function<void (int)> onSwapInstrument, onToggleSpecialBlock;   // ⇄ και SP στη σειρά του block
    std::function<void (int)> onToggleSilent;   // κυκλάκι ♪: το live του block μένει σιωπηλό όσο παίζει η λούπα του
    std::function<void (int, int)> onChordType;   // (block, +1 / -1 / 0 = maj ή νέα τονική)
    std::function<void (int, float)> onVolume;
    std::function<void (int, int, bool)> onSpecialZone;   // ειδικό block: νέα ζώνη από σύρσιμο (lo, hi, τέλος σύρσιμου)
    std::function<void (int, int)> onTranspose;   // (block, +1 / -1 / 0 = μηδενισμός)

    Colour keyLight (int b) const { return engine.blockOff[(size_t) b].load() != 0 ? Colour (0xff7a7168) : blockLight (b); }
    Colour keyDeep (int b) const  { return engine.blockOff[(size_t) b].load() != 0 ? Colour (0xff4f4944) : blockDeep (b); }

    void paint (Graphics& g) override
    {
        const int sel = selected;
        const double t = Time::getMillisecondCounterHiRes() * 0.001;
        const float blink = hobby::pulse (t / 1.6);   // τα ελεύθερα πλήκτρα (χωρίς block) αναβοσβήνουν σιγά σιγά
        const Colour freeWhite = Colour (0xff141210).interpolatedWith (Colour (0xff3d3732), blink);
        const Colour freeBlack = Colour (0xff0c0b0a).interpolatedWith (Colour (0xff2a2623), blink);
        const int cb = engine.pendingBlock();
        const bool waiting = cb >= 0;
        const int firstKey = waiting && engine.zoneState[(size_t) cb].load() == 2 ? engine.zoneFirst[(size_t) cb].load() : -1;

        const float x0 = 14.0f;
        kw = (float) getWidth() - 28.0f;
        const float ww = kw / 52.0f;
        labelY = 12.0f;
        keysY = labelY + 39.0f;   // 8 px παραπάνω: χώρος για τη λωρίδα του ειδικού block

        for (auto& s : spans) s = Span();
        struct CtlDot { Point<float> p; float r; int n; };
        std::vector<CtlDot> ctlDots;
        std::vector<Rectangle<float>> blackRects;
        std::vector<Colour> blackCols;
        std::vector<float> seps;

        int wi = 0, prev = -1;
        for (int n = flow::lowestNote; n <= flow::highestNote; ++n)
        {
            const int b = engine.blockForNote (n);
            const bool black = flow::isBlackKey (n);
            const float l = x0 + (black ? ((float) wi - 0.3f) * ww : (float) wi * ww);
            const float r = x0 + (black ? ((float) wi + 0.3f) * ww : (float) (wi + 1) * ww);

            if (b >= 0 && prev >= 0 && b != prev)
                seps.push_back (x0 + (float) wi * ww);

            if (b >= 0)
            {
                auto& s = spans[(size_t) b];
                s.used = true;
                s.l = jmin (s.l, l);
                s.r = jmax (s.r, r);
            }

            noteL[(size_t) n] = l; noteR[(size_t) n] = r;
            if (! black) whiteNote[(size_t) jmin (wi, 51)] = n;
            const bool isFirst = n == firstKey;
            const float alpha = 1.0f;
            const bool switchKey = b >= 0 && ((engine.swBits[(size_t) b * 2 + (size_t) (n >> 6)].load() >> (n & 63)) & 1) != 0;
            const Colour switchOrange (0xffe8883a);
            const bool latchedKey = b >= 0 && engine.blockOff[(size_t) b].load() == 0
                                    && ((engine.latchBits[(size_t) b * 2 + (size_t) (n >> 6)].load() >> (n & 63)) & 1) != 0;
            const bool pressedKey = ((engine.pressBits[(size_t) (n >> 6)].load() >> (n & 63)) & 1) != 0;   // το πλήκτρο που πατάς φαίνεται λευκό
            const bool litKey = latchedKey || pressedKey;

            if (black)
            {
                blackRects.push_back ({ l, keysY, r - l, 34.0f });
                const bool ctl = flow::isControlKey (n);
                if (ctl) ctlDots.push_back ({ { (l + r) * 0.5f, keysY + 17.0f }, jmin ((r - l) * 0.32f, 6.0f), n });
                Colour c = ctl ? hobby::kScreen.brighter (0.12f) : litKey ? Colours::white : switchKey ? switchOrange.darker (0.25f) : (b >= 0 ? keyDeep (b) : (isFirst ? blockDeep (cb) : freeBlack));
                blackCols.push_back (c.withMultipliedAlpha (alpha));
            }
            else
            {
                const Rectangle<float> kr (l, keysY, r - l, 56.0f);
                const bool ctlKey = flow::isControlKey (n);   // A0 (tap), A#0 (Rec Audio), B0 (Follow): σκούρα πλήκτρα ελέγχου
                Colour c = ctlKey ? hobby::kScreen.brighter (0.12f) : litKey ? Colours::white : switchKey ? switchOrange : (b >= 0 ? keyLight (b) : (isFirst ? blockLight (cb) : freeWhite));
                const Colour cc = c.withMultipliedAlpha (alpha);
                if (b >= 0 && ! litKey && ! switchKey && ! ctlKey)   // ντεγκραντέ: 100% πάνω, 70% κάτω
                    g.setGradientFill (ColourGradient (cc, 0.0f, kr.getY(), cc.interpolatedWith (hobby::kScreen, 0.3f), 0.0f, kr.getBottom(), false));
                else
                    g.setColour (cc);
                g.fillRoundedRectangle (kr, 3.0f);
                g.setColour (hobby::kScreen);
                g.drawRoundedRectangle (kr.reduced (0.5f), 3.0f, 1.0f);
                if (ctlKey)
                    ctlDots.push_back ({ { kr.getCentreX(), kr.getBottom() - 11.0f }, jmin (kr.getWidth() * 0.32f, 7.0f), n });
                ++wi;
            }
            prev = b;
        }

        for (size_t i = 0; i < blackRects.size(); ++i)
        {
            g.setGradientFill (ColourGradient (blackCols[i], 0.0f, blackRects[i].getY(),
                                               blackCols[i].interpolatedWith (hobby::kScreen, 0.3f), 0.0f, blackRects[i].getBottom(), false));
            g.fillRoundedRectangle (blackRects[i], 2.0f);
            g.setColour (hobby::kScreen);
            g.drawRoundedRectangle (blackRects[i].reduced (0.5f), 2.0f, 1.0f);
        }
        for (auto& cd : ctlDots)   // γκρι όταν δεν δουλεύουν· tap: κόκκινο όσο πατιέται και πράσινο όταν πάρει tempo· Rec Audio: κόκκινο όσο γράφει· Follow: πορτοκαλί όταν δουλεύει
        {
            const bool held = ((engine.ctlHeld.load() >> (cd.n - flow::lowestNote)) & 1u) != 0;
            Colour col (0xff4f4a46);
            if (cd.n == flow::tapKeyNote)
                col = held ? hobby::kRed : engine.tapActiveOut.load() != 0 ? hobby::kOn : col;
            else if (cd.n == flow::recAudioKeyNote)
                col = engine.recorder.getState() == RecorderEngine::State::Recording ? hobby::kRed : col;
            else if (cd.n == flow::followKeyNote)
                col = engine.followOn.load() != 0 ? Colour (0xffe8883a) : col;
            g.setColour (col);
            g.fillEllipse (cd.p.x - cd.r, cd.p.y - cd.r, cd.r * 2.0f, cd.r * 2.0f);
        }

        g.setColour (hobby::kScreen);
        for (auto sx : seps)
            g.fillRect (sx - 1.0f, keysY, 2.0f, 56.0f);

        // ειδικό block: άσπρη λωρίδα πάνω από τα πλήκτρα της ζώνης του, και λευκό περίγραμμα στο πρώτο και στο τελευταίο πλήκτρο
        specialRect = {};
        {
            const int sb = engine.specialBlock.load();
            if (sb >= 0 && engine.active[(size_t) sb].load() != 0 && engine.zoneState[(size_t) sb].load() == 0)
            {
                const int a = engine.lo[(size_t) sb].load(), c = engine.hi[(size_t) sb].load();
                if (a >= flow::lowestNote && c <= flow::highestNote && a <= c)
                {
                    const float al = engine.blockOff[(size_t) sb].load() != 0 ? 0.3f : (sb == sel ? 1.0f : 0.8f);
                    specialRect = { noteL[(size_t) a], keysY - 13.0f, noteR[(size_t) c] - noteL[(size_t) a], 8.0f };
                    auto& ss = spans[(size_t) sb];   // το ειδικό block έχει δικό του κυκλάκι ★ (και τις λειτουργίες του) αριστερά στη λωρίδα
                    ss.used = true; ss.l = specialRect.getX(); ss.r = specialRect.getRight();
                    g.setColour (Colours::white.withAlpha (al));
                    g.fillRoundedRectangle (specialRect, 4.0f);
                    for (int n : { a, c })
                    {
                        const bool blk = flow::isBlackKey (n);
                        g.drawRoundedRectangle (Rectangle<float> (noteL[(size_t) n], keysY, noteR[(size_t) n] - noteL[(size_t) n], blk ? 34.0f : 56.0f).reduced (1.0f), 3.0f, 2.0f);
                    }
                }
            }
        }

        // πάνω από κάθε ζώνη: κυκλάκια. Από κάτω τους: γραμμή έντασης.
        constexpr float D = 22.0f, gap = 4.0f;
        const auto numFont = hobby::makeFont (typeface, 13.0f);
        const auto smallFont = hobby::makeFont (typeface, 11.0f);

        if (sel != lastSelSeen)   // όταν επιλέγεται άλλο block, κλείνει το μενού του ανοιχτού
        {
            lastSelSeen = sel;
            if (expanded >= 0 && expanded != sel) expanded = -1;
        }

        std::array<int, flow::numBlocks> order {};
        {
            int k = 0;
            for (int b = 0; b < flow::numBlocks; ++b) if (b != expanded) order[(size_t) k++] = b;
            if (expanded >= 0 && expanded < flow::numBlocks) order[(size_t) k++] = expanded;
            while (k < flow::numBlocks) order[(size_t) k++] = -1;
        }

        for (int oi = 0; oi < flow::numBlocks; ++oi)
        {
            const int b = order[(size_t) oi];
            if (b < 0) continue;
            auto& s = spans[(size_t) b];
            silentRect[(size_t) b] = {};
            for (auto* r : { &numRect[(size_t) b], &holdRect[(size_t) b], &curveRect[(size_t) b], &trRect[(size_t) b], &chordRect[(size_t) b], &barRect[(size_t) b], &xRect[(size_t) b], &swapRect[(size_t) b], &spRect[(size_t) b] })
                *r = {};
            if (! s.used) continue;

            const float width = s.r - s.l;
            const bool isSpecial = b == engine.specialBlock.load();
            const Colour col = isSpecial ? Colours::white.withMultipliedAlpha (engine.blockOff[(size_t) b].load() != 0 ? 0.4f : 1.0f)
                                         : (b == sel ? Colour (0xfffaf8f4) : keyLight (b));   // το επιλεγμένο block (και όταν επιλέγεται αυτόματα από το παίξιμο) είναι λευκό
            const float alpha = b == sel ? 1.0f : 0.7f;
            const int hold = engine.holdMode[(size_t) b].load();
            const int tr = engine.transpose[(size_t) b].load();

            const int cmode = engine.chordMode[(size_t) b].load();
            // κλικ στον αριθμό ανοίγει τις υπόλοιπες λειτουργίες: hold, καμπύλη, transpose, ♪ (live σιωπηλό όσο παίζει λούπα), ✕
            const bool open = b == expanded;
            const bool showChord = false;   // το Chord δεν χρησιμοποιείται πια: δεν υπάρχει κυκλάκι
            const bool showCurve = open, showHold = open, showTr = open, showSilent = open && ! isSpecial, showX = open, showSwap = open, showSp = open;
            const int shown = 1 + (showHold ? 1 : 0) + (showCurve ? 1 : 0) + (showTr ? 1 : 0) + (showSilent ? 1 : 0) + (showSwap ? 1 : 0) + (showSp ? 1 : 0) + (showX ? 1 : 0);
            const float total = (float) shown * D + (float) (shown - 1) * gap;
            float x = isSpecial ? s.l : s.l + (width - total) * 0.5f;   // το ειδικό block: τα κυκλάκια τέρμα αριστερά στη λωρίδα του
            if (open)
            {
                x = jlimit (14.0f, (float) getWidth() - 14.0f - total, x);
            }
            const float cy = labelY;

            // αριθμός
            numRect[(size_t) b] = { x, cy, D, D };
            g.setColour (col.withMultipliedAlpha (alpha));
            g.fillEllipse (numRect[(size_t) b]);
            g.setColour (hobby::kScreen);
            g.setFont (isSpecial ? hobby::makeFont (typeface, 10.5f) : numFont);
            g.drawText (isSpecial ? String ("SP") : String (b + 1), numRect[(size_t) b], Justification::centred, false);
            x += D + gap;

            if (showChord)   // τύπος συγχορδίας (Chord) ή κλίμακα (Scale)
            {
                chordRect[(size_t) b] = { x, cy, D, D };
                const auto cr = chordRect[(size_t) b];
                g.setColour (col.withMultipliedAlpha (alpha * 0.4f));
                g.fillEllipse (cr);
                g.setColour (col.withMultipliedAlpha (alpha));
                static const char* names[] = { "maj", "min", "7", "maj7", "m7", "sus4", "dim" };
                String txt;
                if (cmode == 1) txt = names[jlimit (0, flow::numChordTypes - 1, engine.chordType[(size_t) b].load())];
                else if (engine.chordTonic[(size_t) b].load() < 0) txt = "?";
                else txt = engine.chordMinor[(size_t) b].load() != 0 ? "min" : "maj";
                g.setFont (txt.length() > 3 ? hobby::makeFont (typeface, 9.0f) : smallFont);
                g.drawText (txt, cr, Justification::centred, false);
                x += D + gap;
            }

            if (showHold)    // hold: LM / LP, κενό όταν είναι Off
            {
                holdRect[(size_t) b] = { x, cy, D, D };
                const auto hr = holdRect[(size_t) b];
                if (open) { g.setColour (hobby::kScreen); g.fillEllipse (hr); }   // καπακώνει ό,τι είναι από κάτω, χωρίς φόντο στο μενού
                g.setColour (col.withMultipliedAlpha (alpha * (hold != 0 ? 0.4f : 0.16f)));
                g.fillEllipse (hr);
                Colour holdCol = col;
                if (hold == 3)   // AL Capture: αναβοσβήνει πορτοκαλί
                    holdCol = col.interpolatedWith (Colour (0xffe8883a), hobby::pulse (Time::getMillisecondCounterHiRes() * 0.001 * 1.5));
                else if (hold == 4)
                    holdCol = Colour (0xffe8883a);
                g.setColour (holdCol.withMultipliedAlpha (hold != 0 ? alpha : 0.5f));
                g.setFont (smallFont);
                g.drawText (hold == 1 ? "LM" : hold == 2 ? "LP" : hold >= 3 ? "AL" : "H", hr, Justification::centred, false);
                x += D + gap;
            }

            if (showCurve)   // κουμπί καμπύλης έντασης
            {
                curveRect[(size_t) b] = { x, cy, D, D };
                const auto br = curveRect[(size_t) b];
                g.setGradientFill (ColourGradient (Colour (0xff46403b), 0.0f, br.getY(), Colour (0xff2a2623), 0.0f, br.getBottom(), false));
                g.fillEllipse (br);
                g.setColour (col);
                g.strokePath (curvePath (engine.curveMode[(size_t) b].load(), br), PathStrokeType (1.6f, PathStrokeType::curved, PathStrokeType::rounded));
                x += D + gap;
            }

            if (showTr)      // transpose
            {
                trRect[(size_t) b] = { x, cy, D, D };
                const auto tr2 = trRect[(size_t) b];
                if (open) { g.setColour (hobby::kScreen); g.fillEllipse (tr2); }   // καπακώνει ό,τι είναι από κάτω, χωρίς φόντο στο μενού
                g.setColour (col.withMultipliedAlpha (alpha * (tr != 0 ? 0.4f : 0.16f)));
                g.fillEllipse (tr2);
                g.setColour (col.withMultipliedAlpha (tr != 0 ? alpha : 0.5f));
                g.setFont (smallFont);
                g.drawText (tr > 0 ? "+" + String (tr / 12) : String (tr / 12), tr2, Justification::centred, false);
                x += D + gap;
            }

            if (showSilent)  // ♪: με κάθε πάτημα το live του block μένει σιωπηλό ή όχι όσο παίζει η λούπα του
            {
                silentRect[(size_t) b] = { x, cy, D, D };
                const auto sr2 = silentRect[(size_t) b];
                if (open) { g.setColour (hobby::kScreen); g.fillEllipse (sr2); }   // καπακώνει ό,τι είναι από κάτω, χωρίς φόντο στο μενού
                const bool silent = engine.liveSilent[(size_t) b].load() != 0;
                g.setColour (col.withMultipliedAlpha (alpha * (silent ? 0.4f : 0.16f)));
                g.fillEllipse (sr2);
                g.setColour (col.withMultipliedAlpha (silent ? alpha : 0.7f));
                g.setFont (smallFont);
                g.drawText (String::fromUTF8 ("\xe2\x99\xaa"), sr2, Justification::centred, false);
                if (silent)
                    g.drawLine (sr2.getX() + 5.0f, sr2.getBottom() - 5.0f, sr2.getRight() - 5.0f, sr2.getY() + 5.0f, 1.6f);
                x += D + gap;
            }

            if (showSwap)    // ⇄: αλλαγή οργάνου (VST) του block
            {
                swapRect[(size_t) b] = { x, cy, D, D };
                const auto wr = swapRect[(size_t) b];
                if (open) { g.setColour (hobby::kScreen); g.fillEllipse (wr); }   // καπακώνει ό,τι είναι από κάτω, χωρίς φόντο στο μενού
                g.setColour (col.withMultipliedAlpha (alpha * 0.16f));
                g.fillEllipse (wr);
                g.setColour (col.withMultipliedAlpha (0.7f));
                g.setFont (smallFont);
                g.drawText (String::fromUTF8 ("\xe2\x87\x84"), wr, Justification::centred, false);
                x += D + gap;
            }

            if (showSp)      // SP: ειδικό block (πάνω από τα άλλα, χωρίς λούπα, παίζει πάντα)
            {
                spRect[(size_t) b] = { x, cy, D, D };
                const auto pr = spRect[(size_t) b];
                if (open) { g.setColour (hobby::kScreen); g.fillEllipse (pr); }   // καπακώνει ό,τι είναι από κάτω, χωρίς φόντο στο μενού
                g.setColour (col.withMultipliedAlpha (alpha * (isSpecial ? 0.4f : 0.16f)));
                g.fillEllipse (pr);
                g.setColour (col.withMultipliedAlpha (isSpecial ? alpha : 0.7f));
                g.setFont (hobby::makeFont (typeface, 10.0f));
                g.drawText ("SP", pr, Justification::centred, false);
                x += D + gap;
            }

            if (showX)       // σβήσιμο block: δύο πατήματα (το πρώτο κοκκινίζει για 3 δευτερόλεπτα)
            {
                xRect[(size_t) b] = { x, cy, D, D };
                const auto xr = xRect[(size_t) b];
                if (open) { g.setColour (hobby::kScreen); g.fillEllipse (xr); }   // καπακώνει ό,τι είναι από κάτω, χωρίς φόντο στο μενού
                const bool armed = armedBlock == b && Time::getMillisecondCounter() < armedUntil;
                const Colour xc = armed ? hobby::kRed : col.withMultipliedAlpha (0.4f);
                g.setColour (xc.withAlpha (armed ? 0.3f : 0.14f));
                g.fillEllipse (xr);
                g.setColour (xc);
                Path xp;
                xp.startNewSubPath (xr.getCentreX() - 3.5f, xr.getCentreY() - 3.5f); xp.lineTo (xr.getCentreX() + 3.5f, xr.getCentreY() + 3.5f);
                xp.startNewSubPath (xr.getCentreX() + 3.5f, xr.getCentreY() - 3.5f); xp.lineTo (xr.getCentreX() - 3.5f, xr.getCentreY() + 3.5f);
                g.strokePath (xp, PathStrokeType (1.5f, PathStrokeType::curved, PathStrokeType::rounded));
            }

            const Rectangle<float> bar (s.l + 0.006f * kw, keysY - 4.0f, width - 0.012f * kw, 3.0f);
            barRect[(size_t) b] = bar;
            g.setColour (Colour (0xff3a3430));
            g.fillRoundedRectangle (bar, 1.5f);
            g.setColour (engine.blockOff[(size_t) b].load() != 0 ? Colour (0xff7a7168) : Colour (0xfff2c14e));
            g.fillRoundedRectangle (bar.withWidth (jmax (0.0f, bar.getWidth() * engine.gainOut[(size_t) b].load())), 1.5f);
        }
    }

    void mouseDown (const MouseEvent& e) override
    {
        const auto p = e.position;
        dragBlock = -1;
        spMode = 0;

        // το μενού του block ανοίγει και κλείνει μόνο με τον αριθμό του (όχι με κλικ αλλού)

        // ο αριθμός ανοίγει/κλείνει το μενού του block, ανεξάρτητα από γειτονικά blocks που μπορεί να επικαλύπτονται
        if (! e.mods.isPopupMenu())
            for (int b = 0; b < flow::numBlocks; ++b)
                if (spans[(size_t) b].used && numRect[(size_t) b].expanded (2.0f).contains (p))
                {
                    expanded = expanded == b ? -1 : b;
                    repaint();
                    return;
                }

        // το ✕ του ανοιχτού block ελέγχεται πρώτο: τα κυκλάκια μπορεί να πέφτουν πάνω στην περιοχή γειτονικού block
        if (! e.mods.isPopupMenu())
            for (int b = 0; b < flow::numBlocks; ++b)
                if (spans[(size_t) b].used && xRect[(size_t) b].expanded (2.0f).contains (p))
                {
                    if (armedBlock == b && Time::getMillisecondCounter() < armedUntil) { armedBlock = -1; repaint(); if (onRemoveBlock) onRemoveBlock (b); }
                    else { armedBlock = b; armedUntil = Time::getMillisecondCounter() + 3000; repaint(); Component::SafePointer<KeyboardView> sp (this); Timer::callAfterDelay (3100, [sp] { if (sp != nullptr) sp->repaint(); }); }
                    return;
                }

        // τα κυκλάκια του ανοιχτού μενού ελέγχονται πρώτα, ανεξάρτητα από το πλάτος της ζώνης του block (μικρά blocks: τα κυκλάκια πέφτουν έξω από τη ζώνη)
        if (! e.mods.isPopupMenu() && expanded >= 0 && expanded < flow::numBlocks && spans[(size_t) expanded].used)
        {
            const int b = expanded;
            if (swapRect[(size_t) b].expanded (2.0f).contains (p))   { if (onSwapInstrument) onSwapInstrument (b); repaint(); return; }
            if (spRect[(size_t) b].expanded (2.0f).contains (p))     { if (onToggleSpecialBlock) onToggleSpecialBlock (b); repaint(); return; }
            if (silentRect[(size_t) b].expanded (2.0f).contains (p)) { if (onToggleSilent) onToggleSilent (b); repaint(); return; }
            if (holdRect[(size_t) b].expanded (2.0f).contains (p))   { if (onHold) onHold (b); return; }
            if (curveRect[(size_t) b].expanded (2.0f).contains (p))  { if (onCurve) onCurve (b); return; }
            if (trRect[(size_t) b].expanded (2.0f).contains (p))     { if (onTranspose) onTranspose (b, e.getNumberOfClicks() >= 2 ? 0 : 1); return; }
        }

        bool onCircle = false;   // κλικ σε κυκλάκι κανονικού block μένει στο block, ακόμα κι αν η λωρίδα του ειδικού περνά από πάνω
        for (int b = 0; b < flow::numBlocks && ! onCircle; ++b)
            if (spans[(size_t) b].used)
                for (const auto* r : { &holdRect[(size_t) b], &curveRect[(size_t) b], &trRect[(size_t) b], &chordRect[(size_t) b], &silentRect[(size_t) b], &swapRect[(size_t) b], &spRect[(size_t) b] })
                    if (r->expanded (2.0f).contains (p)) { onCircle = true; break; }

        if (! onCircle && ! specialRect.isEmpty() && specialRect.expanded (3.0f, 4.0f).contains (p))
        {
            const int sb = engine.specialBlock.load();
            if (e.mods.isPopupMenu()) { if (onVolumeMenu) onVolumeMenu (sb); return; }
            spLo0 = engine.lo[(size_t) sb].load(); spHi0 = engine.hi[(size_t) sb].load();
            spStartNote = noteAtX (p.x);
            spMoved = false;
            spMode = p.x < specialRect.getX() + 10.0f ? 1 : p.x > specialRect.getRight() - 10.0f ? 2 : 3;
            return;
        }

        for (int b = 0; b < flow::numBlocks; ++b)
        {
            const auto& s = spans[(size_t) b];
            if (! s.used || p.x < s.l || p.x > s.r || p.y < labelY - 2.0f || p.y > keysY + 56.0f) continue;
            if (b == engine.specialBlock.load() && p.y >= keysY - 13.0f) continue;   // κάτω από τα κυκλάκια το ειδικό block δεν παίρνει κλικ (τα πλήκτρα ανήκουν στα κανονικά blocks)

            const bool onBar = barRect[(size_t) b].expanded (0.0f, 6.0f).contains (p);
            const bool onTr = trRect[(size_t) b].expanded (2.0f).contains (p);
            const bool onChord = chordRect[(size_t) b].expanded (2.0f).contains (p);

            if (e.mods.isPopupMenu())
            {
                if (onTr) { if (onTranspose) onTranspose (b, -1); }
                else if (onChord) { if (onChordType) onChordType (b, -1); }
                else if (onBar) { if (onVolumeMenu) onVolumeMenu (b); }
                else if (p.y < keysY) { if (onRedoZone) onRedoZone (b); }
                return;
            }

            if (onTr)
            {
                if (onTranspose) onTranspose (b, e.getNumberOfClicks() >= 2 ? 0 : 1);
                return;
            }
            if (numRect[(size_t) b].expanded (2.0f).contains (p)) { expanded = expanded == b ? -1 : b; repaint(); return; }   // ο αριθμός δεν επιλέγει: ανοίγει τις λειτουργίες
            if (silentRect[(size_t) b].expanded (2.0f).contains (p)) { if (onToggleSilent) onToggleSilent (b); repaint(); return; }
            if (onChord) { if (onChordType) onChordType (b, e.getNumberOfClicks() >= 2 ? 0 : 1); return; }
            if (holdRect[(size_t) b].expanded (2.0f).contains (p)) { if (onHold) onHold (b); return; }
            if (curveRect[(size_t) b].expanded (2.0f).contains (p)) { if (onCurve) onCurve (b); return; }

            if (onBar)
            {
                dragBlock = b;
                setVolumeFromX (b, p.x);
                return;
            }

            if (onSelect) onSelect (b);
            return;
        }
    }

    void mouseDrag (const MouseEvent& e) override
    {
        if (dragBlock >= 0) setVolumeFromX (dragBlock, e.position.x);

        if (spMode > 0 && onSpecialZone)
        {
            const int n = noteAtX (e.position.x);
            int a = spLo0, c = spHi0;
            if (spMode == 1)      a = jmin (n, spHi0);
            else if (spMode == 2) c = jmax (n, spLo0);
            else
            {
                const int w = spHi0 - spLo0;
                a = jlimit (flow::lowestNote, flow::highestNote - w, spLo0 + (n - spStartNote));
                c = a + w;
            }
            if (a != engine.lo[(size_t) engine.specialBlock.load()].load() || c != engine.hi[(size_t) engine.specialBlock.load()].load())
            {
                spMoved = true;
                onSpecialZone (a, c, false);
            }
        }
    }

    void mouseUp (const MouseEvent&) override
    {
        dragBlock = -1;
        if (spMode > 0)
        {
            const int sb = engine.specialBlock.load();
            if (spMoved) { if (onSpecialZone && sb >= 0) onSpecialZone (engine.lo[(size_t) sb].load(), engine.hi[(size_t) sb].load(), true); }
            else if (onSelect && sb >= 0) onSelect (sb);
            spMode = 0;
        }
    }

private:
    struct Span { float l = 1e9f, r = -1e9f; bool used = false; };

    // Εκτίμηση ονόματος συγχορδίας από τα pitch classes που ακούγονται. Δεν είναι πάντα μοναδική (C6 / Am7): προτιμά το μπάσο.
    static float textWidth (const Font& f, const String& t)
    {
        GlyphArrangement ga;
        ga.addLineOfText (f, t, 0.0f, 0.0f);
        return ga.getBoundingBox (0, -1, true).getWidth();
    }

    static String detectChord (int mask, int bass)
    {
        int count = 0;
        for (int i = 0; i < 12; ++i) count += (mask >> i) & 1;
        if (count < 3) return {};

        struct T { const char* name; int pcs; };
        static const T types[] = { { "", 0x091 }, { "m", 0x089 }, { "7", 0x491 }, { "maj7", 0x891 }, { "m7", 0x489 }, { "sus4", 0x0A1 },
                                   { "sus2", 0x085 }, { "dim", 0x049 }, { "aug", 0x111 }, { "6", 0x291 }, { "m7b5", 0x449 } };
        static const char* nn[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

        auto rot = [] (int v, int r) { return ((v << r) | (v >> (12 - r))) & 0xfff; };
        String best; int bestScore = -1;
        for (int r = 0; r < 12; ++r)
            for (auto& t : types)
                if (rot (t.pcs, r) == mask)
                {
                    const int score = (r == bass ? 2 : 0);
                    if (score > bestScore) { bestScore = score; best = String (nn[r]) + t.name; }
                }
        return best;
    }

    static Path curvePath (int c, Rectangle<float> r)
    {
        Path p;
        switch (c)
        {
            case 1:  p.startNewSubPath (4.0f, 11.0f); p.cubicTo (12.0f, 11.0f, 16.0f, 8.0f, 18.0f, 3.0f); break;
            case 2:  p.startNewSubPath (4.0f, 11.0f); p.cubicTo (6.0f, 6.0f, 10.0f, 3.0f, 18.0f, 3.0f); break;
            case 3:  p.startNewSubPath (4.0f, 3.0f);  p.lineTo (18.0f, 11.0f); break;
            case 4:  p.startNewSubPath (4.0f, 11.0f); p.cubicTo (10.0f, 11.0f, 12.0f, 3.0f, 18.0f, 3.0f); break;
            case 5:  p.startNewSubPath (4.0f, 11.0f); p.cubicTo (14.0f, 11.0f, 17.0f, 9.0f, 18.0f, 3.0f); break;
            default: p.startNewSubPath (4.0f, 11.0f); p.lineTo (18.0f, 3.0f); break;
        }
        p.applyTransform (AffineTransform::translation (r.getCentreX() - 11.0f, r.getCentreY() - 7.0f));
        return p;
    }

    int noteAtX (float x) const
    {
        const float ww = kw / 52.0f;
        const int i = jlimit (0, 51, (int) std::floor ((x - 14.0f) / ww));
        const int n = whiteNote[(size_t) i];
        return n >= flow::lowestNote ? n : flow::lowestNote;
    }

    void setVolumeFromX (int b, float x)
    {
        const auto bar = barRect[(size_t) b];
        if (bar.getWidth() <= 0.0f || ! onVolume) return;
        onVolume (b, jlimit (0.0f, 1.0f, (x - bar.getX()) / bar.getWidth()));
    }

    flow::Engine& engine;
    const int& selected;
    const String& message;
    Typeface::Ptr typeface;
    std::array<Span, flow::numBlocks> spans;
    int expanded = -1;
    int lastSelSeen = -2;
    std::array<Rectangle<float>, flow::numBlocks> silentRect;
    std::array<Rectangle<float>, flow::numBlocks> numRect, holdRect, curveRect, trRect, chordRect, barRect, xRect, swapRect, spRect;
    int armedBlock = -1;
    uint32 armedUntil = 0;
    float kw = 1.0f, labelY = 46.0f, keysY = 77.0f;
    int dragBlock = -1;
    std::array<float, 128> noteL {}, noteR {};
    std::array<int, 52> whiteNote {};
    Rectangle<float> specialRect;
    int spMode = 0, spLo0 = 0, spHi0 = 0, spStartNote = 0;
    bool spMoved = false;
};

//==============================================================================
// Κεφαλίδα του πάνελ plugin: τίτλος, CPU, κυκλάκια αλυσίδας FX και "M" για το master FX.
class PanelHeader : public Component
{
public:
    explicit PanelHeader (Typeface::Ptr face) : typeface (face) {}

    // δεδομένα που ορίζει το MainComponent
    bool midiActive = false;   // τελεία: το Flow ακούει MIDI
    String title, chordName, notice;
    Colour noticeColour { Colour (0xffe8837a) };
    Colour accent { Colour (0xff9ad4a8) };
    int numCircles = 0;       // πόσα FX έχει η αλυσίδα που φαίνεται
    int activeCircle = -1;    // ποιο δείχνει ο editor
    bool masterMode = false;
    bool canAdd = true;
    float cpu = 0.0f;
    bool showActions = false;          // κουμπιά αλλαγής / αφαίρεσης του plugin που φαίνεται
    int64 armedUntilMs = 0;            // το κουμπί αφαίρεσης περιμένει δεύτερο πάτημα μέχρι αυτή τη στιγμή

    std::function<void (int)> onCircle, onCircleMenu;
    std::function<void()> onPlus, onMaster, onReplace, onRemove;

    void paint (Graphics& g) override
    {
        const float h = (float) getHeight();

        // δεξιά: M | + 3 2 1  (η σειρά: 1 2 3 + | M)
        constexpr float D = 22.0f, gap = 5.0f;
        const int n = numCircles;
        const float total = (float) n * (D + gap) + (canAdd ? D : 0.0f) + 12.0f + 1.0f + 12.0f + D;
        float x = (float) getWidth() - 12.0f - total;
        const float y = (h - D) * 0.5f;

        circles.clear();
        auto disc = [&] (Rectangle<float> r, Colour fill, const String& txt, Colour tc)
        {
            g.setColour (fill);
            g.fillEllipse (r);
            g.setColour (tc);
            g.setFont (hobby::makeFont (typeface, 13.0f));
            g.drawText (txt, r, Justification::centred, false);
        };
        for (int i = 0; i < n; ++i)
        {
            const Rectangle<float> r (x, y, D, D);
            circles.push_back (r);
            const bool on = i == activeCircle;
            const Colour col = masterMode ? hobby::kPeach : accent;
            disc (r, on ? col.darker (0.45f) : col.withAlpha (0.35f), String (i + 1), on ? Colours::white : col.darker (0.6f));
            x += D + gap;
        }

        plusRect = {};
        if (canAdd)
        {
            plusRect = { x, y, D, D };
            g.setColour (Colour (0xffd9d0c4));
            g.fillEllipse (plusRect);
            g.setColour (Colour (0xff6b625a));
            g.fillRect (plusRect.getCentreX() - 5.0f, plusRect.getCentreY() - 0.9f, 10.0f, 1.8f);
            g.fillRect (plusRect.getCentreX() - 0.9f, plusRect.getCentreY() - 5.0f, 1.8f, 10.0f);
            x += D;
        }

        x += 12.0f;
        g.setColour (Colour (0xffc9bfb3));
        g.fillRect (x, y + 2.0f, 1.0f, D - 4.0f);
        x += 13.0f;

        masterRect = { x, y, D, D };
        disc (masterRect, masterMode ? hobby::kPeach.darker (0.3f) : hobby::kPeach.withAlpha (0.55f), "M", masterMode ? Colours::white : Colour (0xff8a4f1c));
    }

    // μόνο τα κυκλάκια, το + και το M πιάνουν κλικ: το υπόλοιπο (π, Flow) περνά στο πάνω παράθυρο
    bool hitTest (int x, int y) override
    {
        const Point<float> p ((float) x, (float) y);
        if (masterRect.expanded (3.0f).contains (p) || plusRect.expanded (3.0f).contains (p)) return true;
        for (const auto& c : circles)
            if (c.expanded (3.0f).contains (p)) return true;
        return false;
    }

    void mouseDown (const MouseEvent& e) override
    {
        const auto p = e.position;
        if (masterRect.expanded (3.0f).contains (p)) { if (onMaster) onMaster(); return; }
        if (plusRect.expanded (3.0f).contains (p)) { if (onPlus) onPlus(); return; }
        for (size_t i = 0; i < circles.size(); ++i)
            if (circles[i].expanded (3.0f).contains (p))
            {
                if (e.mods.isPopupMenu()) { if (onCircleMenu) onCircleMenu ((int) i); }
                else if (onCircle) onCircle ((int) i);
                return;
            }
    }

private:
    Typeface::Ptr typeface;
    std::vector<Rectangle<float>> circles;
    Rectangle<float> plusRect, masterRect, replaceRect, removeRect;
};


// Ο orb σε δικό του παράθυρο (σύρεται σε άλλη οθόνη, π.χ. iPad με Sidecar/Duet)
class OrbPanel : public Component
{
public:
    explicit OrbPanel (LoopsView& l) : lv (l) { lv.orbExternal = this; }
    ~OrbPanel() override { if (lv.orbExternal == this) lv.orbExternal = nullptr; }
    void paint (Graphics& g) override
    {
        g.fillAll (hobby::kScreen);
        lv.extPaint (g, getWidth(), getHeight());
    }
    void mouseDown (const MouseEvent& e) override { lv.extMouseDown (e.position, getWidth(), getHeight()); repaint(); }
    void mouseDrag (const MouseEvent& e) override { lv.extMouseDrag (e.position, getWidth(), getHeight()); }
    void mouseUp (const MouseEvent&) override { lv.extMouseUp(); }
    void mouseMove (const MouseEvent& e) override { lv.extMouseMove (e.position, getWidth(), getHeight()); }
private:
    LoopsView& lv;
};

class OrbWindow : public DocumentWindow
{
public:
    OrbWindow (LoopsView& l, std::function<void()> onClose)
        : DocumentWindow ("Flow orb", hobby::kScreen, DocumentWindow::closeButton | DocumentWindow::minimiseButton | DocumentWindow::maximiseButton),
          closed (std::move (onClose))
    {
        setUsingNativeTitleBar (true);
        panel = new OrbPanel (l);
        setContentOwned (panel, false);
        setResizable (true, false);
        setResizeLimits (360, 300, 4000, 3000);
        // αν υπάρχει δεύτερη οθόνη (π.χ. iPad), ο orb ανοίγει εκεί, αλλιώς σε κανονικό μέγεθος στην κύρια
        const auto& ds = Desktop::getInstance().getDisplays();
        const Displays::Display* other = nullptr;
        for (auto& d : ds.displays) if (! d.isMain) { other = &d; break; }
        if (other != nullptr) setBounds (other->userArea);
        else centreWithSize (720, 560);
        setVisible (true);
        toFront (false);
    }
    void closeButtonPressed() override { if (closed) closed(); }
private:
    OrbPanel* panel = nullptr;
    std::function<void()> closed;
};
