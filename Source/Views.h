#pragma once
// Οθόνες του Flow: loops, πληκτρολόγιο, κεφαλίδα πάνελ plugin, tempo και tap.
#include <juce_audio_utils/juce_audio_utils.h>
#include "Engine.h"
#include "HobbyStyle.h"

using namespace juce;

static inline String gr (const char* utf8Text) { return String::fromUTF8 (utf8Text); }

static inline float textWidthOf (const Font& f, const String& t)
{
    GlyphArrangement ga;
    ga.addLineOfText (f, t, 0.0f, 0.0f);
    return ga.getBoundingBox (0, -1, true).getWidth();
}

// Χρώματα των blocks: ανοιχτό για τα λευκά πλήκτρα, πιο βαθύ για τα μαύρα.
static inline Colour blockLight (int b)
{
    static const Colour c[7] { Colour (0xff9ad4a8), Colour (0xffc4aef0), Colour (0xfff5de8a), Colour (0xfff4a7b9),
                               Colour (0xff8fc8f0), Colour (0xfff5b88a), Colour (0xffff8f84) };
    return c[b % 7];
}

static inline Colour blockDeep (int b)
{
    static const Colour c[7] { Colour (0xff5b9a70), Colour (0xff8670c4), Colour (0xffc2a63f), Colour (0xffc76f87),
                               Colour (0xff4f93c4), Colour (0xffc98550), Colour (0xffd6594c) };
    return c[b % 7];
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
        search.setTextToShowWhenEmpty (String::fromUTF8 ("\xce\xb1\xce\xbd\xce\xb1\xce\xb6\xce\xae\xcf\x84\xce\xb7\xcf\x83\xce\xb7\xe2\x80\xa6"), hobby::kIconGrey);   // αναζήτηση…
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

    void resized() override { search.setBounds (jmax (260, getWidth() - 94 - 14 - 190 - 10), 8, 190, 28); }

    void open (const String& t, const String& s, Colour c, StringArray names, StringArray formats,
               std::function<void (int)> pick, std::function<void()> rescan, std::function<void()> file)
    {
        title = t; sub = s; colour = c; items = names; fmts = formats;
        onPick = pick; onRescan = rescan; onFile = file;
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
        g.drawText (sub, 26 + (int) std::ceil (textWidth (hobby::makeFont (typeface, 17.0f), title)) + 12, 8, 200, 28, Justification::centredLeft, false);

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

        const int cols = w >= 640 ? 2 : 1;
        const float gapX = 20.0f, left = 26.0f, rowH = 34.0f, top = 58.0f;
        const float colW = ((float) w - 2.0f * left - gapX * (float) (cols - 1)) / (float) cols;
        const int rowsVisible = jmax (1, (int) ((float) (getHeight() - 58 - 8) / rowH));
        std::vector<int> vis;   // ευρετήριο των plugins που ταιριάζουν στην αναζήτηση
        const String q = search.getText().trim();
        for (int i = 0; i < items.size(); ++i)
            if (q.isEmpty() || items[i].containsIgnoreCase (q)) vis.push_back (i);
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
                g.setColour (hobby::kText);
                g.setFont (hobby::makeFont (typeface, 15.0f));
                g.drawText (items[idx], rr.withTrimmedLeft (14.0f).withTrimmedRight (60.0f), Justification::centredLeft, true);
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

    void mouseWheelMove (const MouseEvent&, const MouseWheelDetails& wd) override
    {
        scroll += wd.deltaY > 0 ? -1 : 1;
        repaint();
    }

private:
    Typeface::Ptr typeface;
    String title, sub;
    Colour colour { Colours::white };
    StringArray items, fmts;
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
        tempoBox.setTooltip (gr ("Tempo (40-240): κλικ για να πληκτρολογήσεις, ή σύρε πάνω/κάτω"));
        tapDot.setTooltip (gr ("Tap tempo: κλικ ή δύο ελεύθερα μαύρα πλήκτρα μαζί"));
    }

    std::function<void (int)> onSelect;
    TempoBox& getTempoBox() { return tempoBox; }
    PluginListView& getList() { return list; }

    std::function<void()> onMetroChange;

    // 8 ποτενσιόμετρα κάτω στην οθόνη (ανάθεση σε παραμέτρους του επιλεγμένου block)
    struct KnobState { bool bound = false; float value = 0.0f; };
    std::array<KnobState, 8> knobs;
    int knobLearning = -1;      // θέση που περιμένει learn (αναβοσβήνει)
    int learnVisual = 0;        // 0 κανονικά, 1 πράσινο (ανάθεση), 2 κόκκινο (σβήσιμο)
    std::function<bool (int)> onKnobClick;            // true = το κλικ το πήρε το learn
    std::function<void (int, float)> onKnobDrag;      // νέα τιμή 0..1 από το ποντίκι
    std::function<String (int)> knobTip;
    static constexpr int knobAreaH = 104;
    static constexpr int stripY = 16;                 // λωρίδα λαμπακιών κάτω από την progress bar
    void setKnobs (const std::array<KnobState, 8>& k, int learning, int visual)
    {
        bool same = learning == knobLearning && visual == learnVisual;
        for (size_t i = 0; same && i < 8; ++i)
            same = k[i].bound == knobs[i].bound && std::abs (k[i].value - knobs[i].value) < 0.004f;
        if (same) return;
        knobs = k; knobLearning = learning; learnVisual = visual;
        repaint (0, getHeight() - knobAreaH, getWidth(), knobAreaH);
    }
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
        tempoBox.setVisible (on);
        tapDot.setVisible (on);
        resized();
        repaint();
    }

    // ενδείξεις κάτω (χωρίς πλαίσιο): block, συγχορδία, μήνυμα ή CPU, τελεία MIDI
    bool midiActive = false;
    String title, chordName, notice;
    Colour noticeColour { Colour (0xffe8837a) };
    float cpu = 0.0f;

    void resized() override
    {
        list.setBounds (0, 44, getWidth(), jmax (60, getHeight() - 44 - knobAreaH - 4));
        const int by = stripY;
        tempoBox.setBounds (getWidth() / 2 - 150, by, 100, 22);
        tapDot.setBounds (tempoBox.getRight() + 8, by, 22, 22);
        tempoBox.setVisible (metroStrip);
        tapDot.setVisible (metroStrip);
    }

    void paint (Graphics& g) override
    {
        const int w = getWidth();
        const int axisBars = engine.axisBarsOut.load();
        const double axisBeats = (double) axisBars * flow::beatsPerBar;
        const double now = engine.beatPosOut.load();

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
        String audioText = tapeState == RecorderEngine::State::Error ? String ("REC AUDIO ERROR")
                           : "REC AUDIO " + clockText (engine.recorder.getElapsedSeconds());
        if (engine.recorder.isZipping()) audioText = "REC AUDIO ZIP...";

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

        // κόκκινη μπάρα 2 μέτρων: προχωρά συνεχόμενα και ξαναρχίζει. Μόνο ένδειξη για να παίζεις μέσα στο μέτρο, δεν καθορίζει το μήκος της λούπας
        {
            const double cycle = 2.0 * flow::beatsPerBar;
            const float p = (float) (std::fmod (jmax (0.0, now), cycle) / cycle);
            const float by = 6.0f, bh = 5.0f;
            g.setColour (Colour (0xff3a3430));
            g.fillRoundedRectangle (laneX, by, laneW, bh, bh * 0.5f);
            g.setColour (Colour (0xffc9c3ba));   // φωτεινό γκρι (όχι κόκκινο)
            g.fillRoundedRectangle (laneX, by, jmax (bh, laneW * p), bh, bh * 0.5f);
            g.setColour (Colour (0xff6b625a));
            g.fillRect (laneX + laneW * 0.5f - 0.5f, by - 1.0f, 1.0f, bh + 2.0f);
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

                for (double k = 0.0; k < axisBeats - 1.0e-6; k += L)
                {
                    const double len = jmin (L, axisBeats - k);
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
                    g.setColour (segCol);
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
        const float hx = laneX + (anyLoop ? laneW * (float) (std::fmod (now, axisBeats) / axisBeats) : 0.0f);
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
            const float chordW = 0.0f;   // η συγχορδία γράφεται δίπλα στον τίτλο του block
            const float toolsW = (float) tools.size() * 26.0f;
            auto needed = [&] (bool cpuOn, bool valsOn)
            {
                const float right = 3.0f * 18.0f + 28.0f + (valsOn ? 60.0f : 0.0f) + 44.0f;
                return 14.0f + titleW + chordW + (cpuOn ? 150.0f : 0.0f) + 16.0f + toolsW + 16.0f + right;
            };
            const bool cpuAllowed = ! metroStrip && notice.isEmpty() && ! showRec;
            bool layoutCpu = true, layoutVals = true;
            if (needed (true, true) > (float) w) layoutVals = false;
            if (needed (true, false) > (float) w) layoutCpu = false;
            const bool showCpu = cpuAllowed && layoutCpu;
            if (showCpu)   // CPU δίπλα από το όνομα του block
            {
                const bool hotCpu = cpu >= 0.85f;
                const float c0 = chordX;
                g.setFont (hobby::makeFont (typeface, 13.0f));
                g.setColour (hobby::kIconGrey);
                g.drawText ("CPU", Rectangle<float> (c0, sy, 30.0f, sh), Justification::centredLeft, false);
                const Rectangle<float> bar (c0 + 34.0f, sy + sh * 0.5f - 2.0f, 60.0f, 4.0f);
                g.setColour (Colour (0xff3a3430));
                g.fillRoundedRectangle (bar, 2.0f);
                g.setColour (hotCpu ? hobby::kRed : hobby::kOn);
                g.fillRoundedRectangle (bar.withWidth (jmax (2.0f, bar.getWidth() * jlimit (0.0f, 1.0f, cpu))), 2.0f);
                g.setColour (hotCpu ? hobby::kRed : hobby::kIconGrey);
                g.drawText (String ((int) std::lround (jlimit (0.0f, 1.0f, cpu) * 100.0f)) + "%", Rectangle<float> (c0 + 100.0f, sy, 40.0f, sh), Justification::centredLeft, false);
                tipZones.push_back ({ Rectangle<float> (c0, sy, 140.0f, sh), String::fromUTF8 ("Φόρτος επεξεργαστή (CPU)") });
                chordX = c0 + 150.0f;
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
                // ένταση: μπάρα από 0 έως 100%, σέρνεται
                {
                    const float bw = 96.0f, bh = 6.0f;
                    metroSliderRect = { x, ly - bh * 0.5f, bw, bh };
                    const float lv = jlimit (0.0f, 1.0f, engine.metroLevel.load());
                    g.setColour (Colour (0xff3a3430));
                    g.fillRoundedRectangle (metroSliderRect, 3.0f);
                    g.setColour (hobby::kOn);
                    g.fillRoundedRectangle (metroSliderRect.withWidth (jmax (3.0f, bw * lv)), 3.0f);
                    g.setColour (Colour (0xfff3f4f6));
                    g.fillEllipse (metroSliderRect.getX() + bw * lv - 6.0f, ly - 6.0f, 12.0f, 12.0f);
                    tipZones.push_back ({ metroSliderRect.expanded (8.0f, 10.0f), String::fromUTF8 ("Ένταση μετρονόμου (0–100%)") });
                    x += bw + 22.0f;
                }
                const bool custom = metroIsCustom && metroIsCustom();
                tipZones.push_back ({ Rectangle<float> (x - 12.0f, ly - 12.0f, 24.0f, 24.0f), custom ? String::fromUTF8 ("Επαναφορά στον αρχικό ήχο μετρονόμου") : String::fromUTF8 ("Διάλεξε δικό σου ήχο για το μετρονόμο") });
                lampAt (x, custom, custom ? String::fromUTF8 ("\xe2\x86\xba") : String::fromUTF8 ("\xe2\x99\xaa"),
                        [this, custom] { if (onMetroSample) onMetroSample (custom ? 2 : 1); });   // ♪ διάλεξε δικό σου ήχο, ↺ γύρνα στον αρχικό
            }
            else if (notice.isNotEmpty())
            {
                g.setColour (noticeColour);
                g.drawText (notice, (int) cx - 140, (int) sy, 165, (int) sh, Justification::centred, true);
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
                const float leftEnd = 14.0f + titleW + chordW + (showCpu ? 150.0f : 0.0f);
                const float rightStart = (float) w - 44.0f - (3.0f * 18.0f + 28.0f + (layoutVals ? 60.0f : 0.0f));
                float tx = jmin (jmax (cx - toolsW * 0.5f, leftEnd + 16.0f), rightStart - 16.0f - toolsW) + 13.0f;
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
            // φωτάκια ρυθμίσεων looper (δεξιά, μικρά): A = ανίχνευση, ≈ = αναμονή πατημάτων, # = snap νοτών
            if (! metroStrip)
            {
                const float ly = sy + sh * 0.5f;
                const float R = 9.0f;                         // ακτίνα φωτακιού
                const bool showVals = layoutVals;
                const float valW = showVals ? 30.0f : 0.0f;
                const int sel = jlimit (0, flow::numBlocks - 1, selected);
                const int mode = engine.autoLoop.load();
                const bool paused = engine.autoPausedOut[(size_t) sel].load() != 0;
                const bool ready = mode != 0 && ! paused && engine.autoReadyOut[(size_t) sel].load() != 0;
                const int snap = engine.snapQ.load();

                // x από δεξιά: [A] [≈ val] [# val] ... τελεία MIDI
                const float total = 2.0f * R + 14.0f + (2.0f * R + valW) + 14.0f + (2.0f * R + valW);
                float lx = (float) w - 44.0f - total + R;

                // σκέτα σύμβολα (χωρίς κυκλάκι): φωτίζουν όταν είναι ενεργά. Κάθε πάτημα αλλάζει επίπεδο.
                auto zone = [&] (float x, const String& tip, std::function<void()> fn)
                {
                    const Rectangle<float> r (x - R, ly - R - 2.0f, 2.0f * R + valW, 2.0f * R + 4.0f);
                    hot.push_back ({ r, fn });
                    tipZones.push_back ({ r, tip });
                };
                const bool aOn = mode != 0 && ! paused;
                const double pul = ready ? hobby::pulse (Time::getMillisecondCounterHiRes() * 0.001 * 1.5) : 0.0;

                {   // A: ανίχνευση επανάληψης
                    const Colour c = aOn ? (mode == 2 ? hobby::mix (hobby::kLilac, Colours::white, (float) pul * 0.6f) : hobby::kOn) : Colour (0xff6b625a);
                    g.setColour (c);
                    g.setFont (hobby::makeFont (typeface, 16.0f));
                    g.drawText ("A", Rectangle<float> (lx - R, ly - R, 2.0f * R, 2.0f * R), Justification::centred, false);
                    zone (lx, String::fromUTF8 ("Αυτόματη ανίχνευση επανάληψης: Off → Auto (ξεκινά μόνη) → με πάτημα Rec MIDI loop"),
                          [this] { engine.autoLoop = (engine.autoLoop.load() + 1) % 3; if (onMetroChange) onMetroChange(); });
                }
                lx += 2.0f * R + 14.0f;

                {   // ≈: αναμονή πατημάτων πεταλιού
                    const Rectangle<float> r (lx - R, ly - R, 2.0f * R, 2.0f * R);
                    g.setColour (hobby::kPeach);
                    for (float dy : { -2.4f, 2.4f })
                    {
                        Path p;
                        p.startNewSubPath (r.getCentreX() - 6.0f, r.getCentreY() + dy);
                        p.cubicTo (r.getCentreX() - 3.0f, r.getCentreY() + dy - 3.0f, r.getCentreX() - 1.4f, r.getCentreY() + dy - 3.0f, r.getCentreX(), r.getCentreY() + dy);
                        p.cubicTo (r.getCentreX() + 1.4f, r.getCentreY() + dy + 3.0f, r.getCentreX() + 3.0f, r.getCentreY() + dy + 3.0f, r.getCentreX() + 6.0f, r.getCentreY() + dy);
                        g.strokePath (p, PathStrokeType (1.6f, PathStrokeType::curved, PathStrokeType::rounded));
                    }
                    if (showVals)
                    {
                        g.setColour (hobby::kIconGrey);
                        g.setFont (hobby::makeFont (typeface, 11.0f));
                        g.drawText (String ((double) engine.tapMs.load() / 1000.0, 1) + "s", (int) (lx + R + 3.0f), (int) sy, (int) valW, (int) sh, Justification::centredLeft, false);
                    }
                    zone (lx, String::fromUTF8 ("Αναμονή πατημάτων πεταλιού (0.5 / 0.7 / 1.0 δευτ.)"),
                          [this] { const int t = engine.tapMs.load(); engine.tapMs = t < 600 ? 700 : t < 900 ? 1000 : 500; if (onMetroChange) onMetroChange(); });
                }
                lx += 2.0f * R + valW + 14.0f;

                {   // #: quantize νοτών
                    const Rectangle<float> r (lx - R, ly - R, 2.0f * R, 2.0f * R);
                    g.setColour (snap != 0 ? hobby::kLilac : Colour (0xff6b625a));
                    const float a = 2.8f, b = 6.0f;
                    g.fillRect (r.getCentreX() - a - 0.8f, r.getCentreY() - b, 1.6f, 2.0f * b);
                    g.fillRect (r.getCentreX() + a - 0.8f, r.getCentreY() - b, 1.6f, 2.0f * b);
                    g.fillRect (r.getCentreX() - b, r.getCentreY() - a - 0.8f, 2.0f * b, 1.6f);
                    g.fillRect (r.getCentreX() - b, r.getCentreY() + a - 0.8f, 2.0f * b, 1.6f);
                    if (showVals)
                    {
                        g.setColour (hobby::kIconGrey);
                        g.setFont (hobby::makeFont (typeface, 11.0f));
                        g.drawText (snap == 0 ? String ("Off") : snap == 1 ? String ("1/8") : String ("1/16"), (int) (lx + R + 3.0f), (int) sy, (int) valW, (int) sh, Justification::centredLeft, false);
                    }
                    zone (lx, String::fromUTF8 ("Quantize νοτών στη λούπα: Off → 1/8 → 1/16"),
                          [this] { engine.snapQ = (engine.snapQ.load() + 1) % 3; if (onMetroChange) onMetroChange(); });
                }
            }
            g.setColour (midiActive ? hobby::kOn : Colour (0xff4a433d));
            g.fillEllipse ((float) w - 24.0f, sy + sh * 0.5f - 3.5f, 7.0f, 7.0f);
        }

        paintKnobs (g);
    }

    Point<float> knobCentre (int k) const
    {
        const float cw = (float) (getWidth() - 28) / 8.0f;
        return { 14.0f + cw * ((float) k + 0.5f), (float) getHeight() - 14.0f - knobR };
    }

    int knobAt (Point<float> p) const
    {
        for (int k = 0; k < 8; ++k)
            if (p.getDistanceFrom (knobCentre (k)) <= knobR + 4.0f) return k;
        return -1;
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

    void paintKnobs (Graphics& g)
    {
        const int w = getWidth(), h = getHeight();
        const float sepY = (float) (h - knobAreaH);
        g.setColour (Colour (0xff3a3430));
        g.fillRect (14.0f, sepY, (float) (w - 28), 1.0f);

        // τίτλος block και συγχορδία πάνω από τα ποτενσιόμετρα
        g.setFont (hobby::makeFont (typeface, 16.0f));
        g.setColour (hobby::kText);
        const float titleW = std::ceil (textWidth (g.getCurrentFont(), title));
        g.drawText (title, 14, (int) sepY + 6, (int) titleW + 4, 22, Justification::centredLeft, false);
        if (chordName.isNotEmpty())
        {
            g.setColour (hobby::kLilac);
            g.drawText (chordName, 14 + (int) titleW + 18, (int) sepY + 6, 160, 22, Justification::centredLeft, false);
        }

        const Colour dash = blockLight (jlimit (0, flow::numBlocks - 1, selected));
        const double nowMs = Time::getMillisecondCounterHiRes();
        for (int k = 0; k < 8; ++k)
        {
            const auto c = knobCentre (k);
            const float R = knobR;
            const auto& ks = knobs[(size_t) k];
            const float v = jlimit (0.0f, 1.0f, ks.value);
            const Rectangle<float> disc (c.x - R, c.y - R, 2.0f * R, 2.0f * R);

            Path circle; circle.addEllipse (disc);
            g.setColour (Colour (0xff3a3430).withAlpha (ks.bound ? 1.0f : 0.55f));
            g.fillPath (circle);

            if (ks.bound && v > 0.001f)
            {
                Path pie;
                const float a0 = MathConstants<float>::pi * -0.75f;
                pie.addPieSegment (disc, a0, a0 + MathConstants<float>::pi * 1.5f * v, 0.0f);
                g.setColour (Colour (0xff6b625a).interpolatedWith (Colour (0xffcfc9c0), v));
                g.fillPath (pie);
            }
            g.saveState();
            g.reduceClipRegion (circle);
            g.setTiledImageFill (dotGaps(), 0, 0, 1.0f);
            g.fillRect (disc);
            g.restoreState();

            if (ks.bound)
            {
                const float ang = MathConstants<float>::pi * (-0.75f + 1.5f * v) - MathConstants<float>::halfPi;
                const Point<float> p0 (c.x + R * 0.55f * std::cos (ang), c.y + R * 0.55f * std::sin (ang));
                const Point<float> p1 (c.x + (R - 1.0f) * std::cos (ang), c.y + (R - 1.0f) * std::sin (ang));
                g.setColour (dash);
                g.drawLine (Line<float> (p0, p1), 3.0f);
            }

            // learn: δαχτυλίδι γύρω από κάθε ποτενσιόμετρο (πράσινο ανάθεση, κόκκινο σβήσιμο), αναβοσβήνει εκείνο που περιμένει
            if (learnVisual != 0 || k == knobLearning)
            {
                const bool waiting = k == knobLearning;
                const float a = waiting ? 0.35f + 0.65f * (float) hobby::pulse (nowMs * 0.001 * 2.0) : 0.55f;
                g.setColour ((learnVisual == 2 ? hobby::kRed : hobby::kOn).withAlpha (a));
                g.drawEllipse (disc.expanded (3.0f), 1.6f);
            }
            if (knobTip)
            {
                const auto tip = knobTip (k);
                if (tip.isNotEmpty()) tipZones.push_back ({ disc.expanded (3.0f), tip });
            }
        }
    }

    void mouseDown (const MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) return;
        if (metroStrip && ! metroSliderRect.isEmpty() && metroSliderRect.expanded (8.0f, 10.0f).contains (e.position))
        {
            draggingMetro = true;
            setMetroLevelFromX (e.position.x);
            return;
        }
        for (auto& h : hot)
            if (h.first.contains (e.position)) { h.second(); repaint(); return; }
        {
            const int k = knobAt (e.position);
            if (k >= 0)
            {
                if (onKnobClick && onKnobClick (k)) { repaint(); return; }
                if (knobs[(size_t) k].bound) { dragKnob = k; dragStartY = e.position.y; dragStartVal = knobs[(size_t) k].value; }
                return;
            }
        }
        if (stepOut <= 0) return;
        const auto list = activeBlocks (engine);
        const int i = (e.y - topOut) / stepOut;
        if (e.y >= topOut && i >= 0 && i < list.size() && onSelect)
            onSelect (list[i].getIntValue());
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
        if (draggingMetro) setMetroLevelFromX (e.position.x);
        else if (dragKnob >= 0 && onKnobDrag)
        {
            const float nv = jlimit (0.0f, 1.0f, dragStartVal + (dragStartY - e.position.y) / 160.0f);
            knobs[(size_t) dragKnob].value = nv;
            onKnobDrag (dragKnob, nv);
            repaint (0, getHeight() - knobAreaH, getWidth(), knobAreaH);
        }
    }
    void mouseUp (const MouseEvent&) override { dragKnob = -1; if (draggingMetro) { draggingMetro = false; if (onMetroChange) onMetroChange(); } }

private:
    void setMetroLevelFromX (float px)
    {
        engine.metroLevel = jlimit (0.0f, 1.0f, (px - metroSliderRect.getX()) / metroSliderRect.getWidth());
        repaint();
    }

    Rectangle<float> metroSliderRect;
    bool draggingMetro = false;
    int dragKnob = -1;
    float dragStartY = 0.0f, dragStartVal = 0.0f;
    static constexpr float knobR = 24.0f;
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

            if (black)
            {
                blackRects.push_back ({ l, keysY, r - l, 34.0f });
                Colour c = latchedKey ? Colours::white : switchKey ? switchOrange.darker (0.25f) : (b >= 0 ? keyDeep (b) : (isFirst ? blockDeep (cb) : freeBlack));
                blackCols.push_back (c.withMultipliedAlpha (alpha));
            }
            else
            {
                const Rectangle<float> kr (l, keysY, r - l, 56.0f);
                Colour c = latchedKey ? Colours::white : switchKey ? switchOrange : (b >= 0 ? keyLight (b) : (isFirst ? blockLight (cb) : freeWhite));
                g.setColour (c.withMultipliedAlpha (alpha));
                g.fillRoundedRectangle (kr, 3.0f);
                g.setColour (hobby::kScreen);
                g.drawRoundedRectangle (kr.reduced (0.5f), 3.0f, 1.0f);
                ++wi;
            }
            prev = b;
        }

        for (size_t i = 0; i < blackRects.size(); ++i)
        {
            g.setColour (blackCols[i]);
            g.fillRoundedRectangle (blackRects[i], 2.0f);
            g.setColour (hobby::kScreen);
            g.drawRoundedRectangle (blackRects[i].reduced (0.5f), 2.0f, 1.0f);
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
            for (auto* r : { &numRect[(size_t) b], &holdRect[(size_t) b], &curveRect[(size_t) b], &trRect[(size_t) b], &chordRect[(size_t) b], &barRect[(size_t) b], &xRect[(size_t) b] })
                *r = {};
            if (! s.used) continue;

            const float width = s.r - s.l;
            const bool isSpecial = b == engine.specialBlock.load();
            const Colour col = isSpecial ? Colours::white.withMultipliedAlpha (engine.blockOff[(size_t) b].load() != 0 ? 0.4f : 1.0f) : keyLight (b);
            const float alpha = b == sel ? 1.0f : 0.7f;
            const int hold = engine.holdMode[(size_t) b].load();
            const int tr = engine.transpose[(size_t) b].load();

            const int cmode = engine.chordMode[(size_t) b].load();
            // κλικ στον αριθμό ανοίγει τις υπόλοιπες λειτουργίες: hold, καμπύλη, transpose, ♪ (live σιωπηλό όσο παίζει λούπα), ✕
            const bool open = b == expanded;
            const bool showChord = false;   // το Chord δεν χρησιμοποιείται πια: δεν υπάρχει κυκλάκι
            const bool showCurve = open, showHold = open, showTr = open, showSilent = open && ! isSpecial, showX = open;
            const int shown = 1 + (showHold ? 1 : 0) + (showCurve ? 1 : 0) + (showTr ? 1 : 0) + (showSilent ? 1 : 0) + (showX ? 1 : 0);
            const float total = (float) shown * D + (float) (shown - 1) * gap;
            float x = isSpecial ? s.l : s.l + (width - total) * 0.5f;   // το ειδικό block: τα κυκλάκια τέρμα αριστερά στη λωρίδα του
            if (open)
            {
                x = jlimit (14.0f, (float) getWidth() - 14.0f - total, x);
                g.setColour (hobby::kScreen);
                g.fillRoundedRectangle (x - 5.0f, labelY - 3.0f, total + 10.0f, D + 6.0f, 9.0f);
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

            if (showX)       // σβήσιμο block: δύο πατήματα (το πρώτο κοκκινίζει για 3 δευτερόλεπτα)
            {
                xRect[(size_t) b] = { x, cy, D, D };
                const auto xr = xRect[(size_t) b];
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

        bool onCircle = false;   // κλικ σε κυκλάκι κανονικού block μένει στο block, ακόμα κι αν η λωρίδα του ειδικού περνά από πάνω
        for (int b = 0; b < flow::numBlocks && ! onCircle; ++b)
            if (spans[(size_t) b].used)
                for (const auto* r : { &holdRect[(size_t) b], &curveRect[(size_t) b], &trRect[(size_t) b], &chordRect[(size_t) b], &silentRect[(size_t) b] })
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
    std::array<Rectangle<float>, flow::numBlocks> numRect, holdRect, curveRect, trRect, chordRect, barRect, xRect;
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
