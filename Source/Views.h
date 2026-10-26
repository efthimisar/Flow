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
        if (engine.active[(size_t) b].load() != 0)
            list.add (String (b));
    return list;
}

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
class LoopsView : public Component
{
public:
    LoopsView (flow::Engine& e, const int& sel, Typeface::Ptr face)
        : engine (e), selected (sel), typeface (face), tempoBox (e, face), tapDot (e)
    {
        addAndMakeVisible (tempoBox);
        addAndMakeVisible (tapDot);
        tempoBox.setTooltip (gr ("Tempo (40-240): κλικ για να πληκτρολογήσεις, ή σύρε πάνω/κάτω"));
        tapDot.setTooltip (gr ("Tap tempo: κλικ ή δύο ελεύθερα μαύρα πλήκτρα μαζί"));
    }

    std::function<void (int)> onSelect;
    TempoBox& getTempoBox() { return tempoBox; }

    // ενδείξεις κάτω (χωρίς πλαίσιο): block, συγχορδία, μήνυμα ή CPU, τελεία MIDI
    bool midiActive = false;
    String title, chordName, notice;
    Colour noticeColour { Colour (0xffe8837a) };
    float cpu = 0.0f;

    void resized() override
    {
        tempoBox.setBounds (getWidth() / 2 - 58, 15, 116, 28);
        tapDot.setBounds (tempoBox.getRight() + 8, 18, 22, 22);
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

        // επάνω: αριστερά REC Loop / n, δεξιά REC Audio / χρόνος
        const auto f = hobby::makeFont (typeface, 19.0f);
        g.setFont (f);

        const String loopWord = overdubbing ? "OVERDUB" : "REC";
        g.setColour (recording >= 0 ? (overdubbing ? hobby::kPeach : hobby::kRed) : hobby::kIconGrey);
        g.drawText (loopWord, 14, 15, 120, 28, Justification::centredLeft, false);
        g.setColour (hobby::kText);
        g.drawText (" Loop / " + (recording >= 0 ? String (recording + 1) : String ("-")),
                    14 + (int) std::ceil (textWidth (f, loopWord)), 15, 200, 28, Justification::centredLeft, false);

        const auto tapeState = engine.recorder.getState();
        const bool taping = tapeState == RecorderEngine::State::Recording;
        String audioRest = tapeState == RecorderEngine::State::Error ? String (" Audio / ERROR")
                           : " Audio / " + clockText (engine.recorder.getElapsedSeconds());
        if (engine.recorder.isZipping()) audioRest = " Audio / ZIP...";
        const float restW = textWidth (f, audioRest), recW = textWidth (f, "REC");
        g.setColour (hobby::kText);
        g.drawText (audioRest, w - 14 - (int) std::ceil (restW) - 4, 15, (int) std::ceil (restW) + 4, 28, Justification::centredRight, false);
        g.setColour (taping ? hobby::kRed : hobby::kIconGrey);
        g.drawText ("REC", w - 14 - (int) std::ceil (restW) - (int) std::ceil (recW) - 4, 15, (int) std::ceil (recW) + 4, 28,
                    Justification::centredRight, false);

        // λωρίδες
        const auto list = activeBlocks (engine);
        const int count = list.size();
        const int top = 48;
        const int room = jmax (40, getHeight() - top - 12 - 26);   // το ύψος της οθόνης loops αλλάζει με το plugin
        const int step = count > 0 ? jmin (44, room / count) : 44;
        const int laneH = jmax (6, step - 6);
        const float laneX = 42.0f, laneW = (float) (w - 14) - laneX;

        stepOut = step; topOut = top;

        g.setFont (hobby::makeFont (typeface, jlimit (10.0f, 16.0f, (float) laneH * 0.6f)));
        for (int i = 0; i < count; ++i)
        {
            const int b = list[i].getIntValue();
            const int y = top + i * step;
            g.setColour (b == selected ? hobby::kText : hobby::kIconGrey);
            g.drawText (String (b + 1), 14, y, 24, laneH, Justification::centredRight, false);
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
            g.setColour (hobby::kRed);
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
                    g.setColour ((allMuted || engine.blockOff[(size_t) b].load() != 0) ? Colour (0xff3a3d46) : blockLight (b));
                    g.fillRoundedRectangle (seg, corner);
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
                    const double p0 = std::fmod (start, axisBeats);
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
            const float sh = 22.0f, sy = (float) getHeight() - sh - 6.0f;
            g.setFont (hobby::makeFont (typeface, 16.0f));
            g.setColour (hobby::kText);
            g.drawText (title, 14, (int) sy, w / 2 - 150, (int) sh, Justification::centredLeft, false);
            if (chordName.isNotEmpty())
            {
                g.setColour (hobby::kLilac);
                g.drawText (chordName, 14 + (int) std::ceil (textWidth (g.getCurrentFont(), title)) + 14, (int) sy, 140, (int) sh, Justification::centredLeft, false);
            }
            const float cx = (float) w * 0.5f;
            if (notice.isNotEmpty())
            {
                g.setColour (noticeColour);
                g.drawText (notice, (int) cx - 140, (int) sy, 165, (int) sh, Justification::centred, true);
            }
            else
            {
                const bool hot = cpu >= 0.85f;
                g.setFont (hobby::makeFont (typeface, 13.0f));
                g.setColour (hobby::kIconGrey);
                g.drawText ("CPU", Rectangle<float> (cx - 88.0f, sy, 32.0f, sh), Justification::centredRight, false);
                const Rectangle<float> bar (cx - 50.0f, sy + sh * 0.5f - 2.0f, 70.0f, 4.0f);
                g.setColour (Colour (0xff3a3430));
                g.fillRoundedRectangle (bar, 2.0f);
                g.setColour (hot ? hobby::kRed : hobby::kOn);
                g.fillRoundedRectangle (bar.withWidth (jmax (2.0f, bar.getWidth() * jlimit (0.0f, 1.0f, cpu))), 2.0f);
                g.setColour (hot ? hobby::kRed : hobby::kIconGrey);
                g.drawText (String ((int) std::lround (jlimit (0.0f, 1.0f, cpu) * 100.0f)) + "%", Rectangle<float> (cx + 26.0f, sy, 40.0f, sh), Justification::centredLeft, false);
            }
            // φωτάκια ρυθμίσεων looper (δεξιά, μικρά): A = ανίχνευση, ≈ = αναμονή πατημάτων, # = snap νοτών
            {
                const float ly = sy + sh * 0.5f;
                const float R = 9.0f;                         // ακτίνα φωτακιού
                const bool showVals = w >= 800;
                const float valW = showVals ? 30.0f : 0.0f;
                const int sel = jlimit (0, flow::numBlocks - 1, selected);
                const int mode = engine.autoLoop.load();
                const bool paused = engine.autoPausedOut[(size_t) sel].load() != 0;
                const bool ready = mode != 0 && ! paused && engine.autoReadyOut[(size_t) sel].load() != 0;
                const int snap = engine.snapQ.load();

                // x από δεξιά: [A] [≈ val] [# val] ... τελεία MIDI
                const float total = 2.0f * R + 14.0f + (2.0f * R + valW) + 14.0f + (2.0f * R + valW);
                float lx = (float) w - 44.0f - total + R;

                auto lamp = [&] (float x, bool on, Colour c, std::function<void (Rectangle<float>, Colour)> glyph)
                {
                    const Rectangle<float> r (x - R, ly - R, 2.0f * R, 2.0f * R);
                    g.setColour (on ? c : Colour (0xff3a3430));
                    g.fillEllipse (r);
                    glyph (r, on ? Colour (0xff262220) : Colour (0xff6b625a));
                };

                const bool aOn = mode != 0 && ! paused;
                const double pul = ready ? hobby::pulse (Time::getMillisecondCounterHiRes() * 0.001 * 1.5) : 0.0;
                lamp (lx, aOn, mode == 2 ? hobby::mix (hobby::kLilac, Colours::white, (float) pul * 0.6f) : hobby::kOn,
                      [&] (Rectangle<float> r, Colour c)
                      {
                          g.setColour (c);
                          g.setFont (hobby::makeFont (typeface, 12.0f));
                          g.drawText ("A", r, Justification::centred, false);
                      });
                lx += 2.0f * R + 14.0f;

                lamp (lx, true, hobby::kPeach,
                      [&] (Rectangle<float> r, Colour c)
                      {
                          g.setColour (c);
                          for (float dy : { -2.4f, 2.4f })
                          {
                              Path p;
                              p.startNewSubPath (r.getCentreX() - 5.0f, r.getCentreY() + dy);
                              p.cubicTo (r.getCentreX() - 2.5f, r.getCentreY() + dy - 2.5f, r.getCentreX() - 1.2f, r.getCentreY() + dy - 2.5f, r.getCentreX(), r.getCentreY() + dy);
                              p.cubicTo (r.getCentreX() + 1.2f, r.getCentreY() + dy + 2.5f, r.getCentreX() + 2.5f, r.getCentreY() + dy + 2.5f, r.getCentreX() + 5.0f, r.getCentreY() + dy);
                              g.strokePath (p, PathStrokeType (1.4f, PathStrokeType::curved, PathStrokeType::rounded));
                          }
                      });
                if (showVals)
                {
                    g.setColour (hobby::kIconGrey);
                    g.setFont (hobby::makeFont (typeface, 11.0f));
                    g.drawText (String ((double) engine.tapMs.load() / 1000.0, 1) + "s", (int) (lx + R + 3.0f), (int) sy, (int) valW, (int) sh, Justification::centredLeft, false);
                }
                lx += 2.0f * R + valW + 14.0f;

                lamp (lx, snap != 0, hobby::kLilac,
                      [&] (Rectangle<float> r, Colour c)
                      {
                          g.setColour (c);
                          const float a = 2.6f, b = 5.0f;
                          g.fillRect (r.getCentreX() - a - 0.7f, r.getCentreY() - b, 1.4f, 2.0f * b);
                          g.fillRect (r.getCentreX() + a - 0.7f, r.getCentreY() - b, 1.4f, 2.0f * b);
                          g.fillRect (r.getCentreX() - b, r.getCentreY() - a - 0.7f, 2.0f * b, 1.4f);
                          g.fillRect (r.getCentreX() - b, r.getCentreY() + a - 0.7f, 2.0f * b, 1.4f);
                      });
                if (showVals)
                {
                    g.setColour (hobby::kIconGrey);
                    g.setFont (hobby::makeFont (typeface, 11.0f));
                    g.drawText (snap == 0 ? String ("Off") : snap == 1 ? String ("1/8") : String ("1/16"), (int) (lx + R + 3.0f), (int) sy, (int) valW, (int) sh, Justification::centredLeft, false);
                }
            }
            g.setColour (midiActive ? hobby::kOn : Colour (0xff4a433d));
            g.fillEllipse ((float) w - 24.0f, sy + sh * 0.5f - 3.5f, 7.0f, 7.0f);
        }
    }

    void mouseDown (const MouseEvent& e) override
    {
        if (e.mods.isPopupMenu() || stepOut <= 0) return;
        const auto list = activeBlocks (engine);
        const int i = (e.y - topOut) / stepOut;
        if (e.y >= topOut && i >= 0 && i < list.size() && onSelect)
            onSelect (list[i].getIntValue());
    }

private:
    flow::Engine& engine;
    const int& selected;
    Typeface::Ptr typeface;
    TempoBox tempoBox;
    TapDot tapDot;
    int stepOut = 0, topOut = 48;
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
    std::function<void (int)> onToggleOff;   // διπλό κλικ στον αριθμό: block σβηστό / αναμμένο
    std::function<void (int, int)> onChordType;   // (block, +1 / -1 / 0 = maj ή νέα τονική)
    std::function<void (int, float)> onVolume;
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
        keysY = labelY + 31.0f;

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

        // πάνω από κάθε ζώνη: κυκλάκια. Από κάτω τους: γραμμή έντασης.
        constexpr float D = 22.0f, gap = 4.0f;
        const auto numFont = hobby::makeFont (typeface, 13.0f);
        const auto smallFont = hobby::makeFont (typeface, 11.0f);

        for (int b = 0; b < flow::numBlocks; ++b)
        {
            auto& s = spans[(size_t) b];
            for (auto* r : { &numRect[(size_t) b], &holdRect[(size_t) b], &curveRect[(size_t) b], &trRect[(size_t) b], &chordRect[(size_t) b], &barRect[(size_t) b], &xRect[(size_t) b] })
                *r = {};
            if (! s.used) continue;

            const float width = s.r - s.l;
            const Colour col = keyLight (b);
            const float alpha = b == sel ? 1.0f : 0.7f;
            const int hold = engine.holdMode[(size_t) b].load();
            const int tr = engine.transpose[(size_t) b].load();

            const int cmode = engine.chordMode[(size_t) b].load();
            const int cap = jlimit (1, 5, (int) std::floor ((width + gap) / (D + gap)));
            // προτεραιότητα όταν δεν χωράνε όλα: αριθμός, chord (αν είναι ενεργό), καμπύλη, hold, transpose
            int room = cap - 1;
            const bool showChord = false;   // το Chord δεν χρησιμοποιείται πια: δεν υπάρχει κυκλάκι
            const bool showCurve = room > 0;                if (showCurve) --room;
            const bool showHold  = room > 0;                if (showHold)  --room;
            const bool showTr    = room > 0;              if (showTr)    --room;
            const bool showX     = room > 0;
            const int shown = 1 + (showChord ? 1 : 0) + (showHold ? 1 : 0) + (showCurve ? 1 : 0) + (showTr ? 1 : 0) + (showX ? 1 : 0);
            const float total = (float) shown * D + (float) (shown - 1) * gap;
            float x = s.l + (width - total) * 0.5f;
            const float cy = labelY;

            // αριθμός
            numRect[(size_t) b] = { x, cy, D, D };
            g.setColour (col.withMultipliedAlpha (alpha));
            g.fillEllipse (numRect[(size_t) b]);
            g.setColour (hobby::kScreen);
            g.setFont (numFont);
            g.drawText (String (b + 1), numRect[(size_t) b], Justification::centred, false);
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

        for (int b = 0; b < flow::numBlocks; ++b)
        {
            const auto& s = spans[(size_t) b];
            if (! s.used || p.x < s.l || p.x > s.r || p.y < labelY - 2.0f || p.y > keysY + 56.0f) continue;

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

            if (xRect[(size_t) b].expanded (2.0f).contains (p))
            {
                if (armedBlock == b && Time::getMillisecondCounter() < armedUntil) { armedBlock = -1; if (onRemoveBlock) onRemoveBlock (b); }
                else { armedBlock = b; armedUntil = Time::getMillisecondCounter() + 3000; }
                repaint();
                return;
            }

            if (onTr)
            {
                if (onTranspose) onTranspose (b, e.getNumberOfClicks() >= 2 ? 0 : 1);
                return;
            }
            if (numRect[(size_t) b].expanded (2.0f).contains (p) && e.getNumberOfClicks() >= 2) { if (onToggleOff) onToggleOff (b); return; }
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
    }

    void mouseUp (const MouseEvent&) override { dragBlock = -1; }

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
    std::array<Rectangle<float>, flow::numBlocks> numRect, holdRect, curveRect, trRect, chordRect, barRect, xRect;
    int armedBlock = -1;
    uint32 armedUntil = 0;
    float kw = 1.0f, labelY = 46.0f, keysY = 77.0f;
    int dragBlock = -1;
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
