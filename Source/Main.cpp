// Flow - master keyboard host για VST3 / AU plugins.
// Prisma Audio

#include <juce_audio_utils/juce_audio_utils.h>
#include "Engine.h"
#include "HobbyStyle.h"

using namespace juce;

static String gr (const char* utf8Text) { return String::fromUTF8 (utf8Text); }

//==============================================================================
// Χρώματα των blocks: ανοιχτό για τα λευκά πλήκτρα, πιο βαθύ για τα μαύρα.
static Colour blockLight (int b)
{
    static const Colour c[7] { Colour (0xff9ad4a8), Colour (0xffc4aef0), Colour (0xfff5de8a), Colour (0xfff4a7b9),
                               Colour (0xff8fc8f0), Colour (0xfff5b88a), Colour (0xffff8f84) };
    return c[b % 7];
}

static Colour blockDeep (int b)
{
    static const Colour c[7] { Colour (0xff5b9a70), Colour (0xff8670c4), Colour (0xffc2a63f), Colour (0xffc76f87),
                               Colour (0xff4f93c4), Colour (0xffc98550), Colour (0xffd6594c) };
    return c[b % 7];
}

static String noteName (int n) { return MidiMessage::getMidiNoteName (n, true, true, 4); }

static float textWidth (const Font& f, const String& s)
{
    GlyphArrangement ga;
    ga.addLineOfText (f, s, 0.0f, 0.0f);
    return ga.getBoundingBox (0, -1, true).getWidth();
}

static String clockText (double seconds)
{
    const int total = (int) seconds;
    return String::formatted ("%02d:%02d:%02d", total / 3600, (total / 60) % 60, total % 60);
}

static StringArray activeBlocks (const flow::Engine& engine)
{
    StringArray list;
    for (int b = 0; b < flow::numBlocks; ++b)
        if (engine.active[(size_t) b].load() != 0)
            list.add (String (b));
    return list;
}

//==============================================================================
// Οθόνη loops: μία λωρίδα ανά block, 4 μέτρα, λευκή κεφαλή αναπαραγωγής.
class LoopsView : public Component
{
public:
    LoopsView (flow::Engine& e, const int& sel, const String& msg, Typeface::Ptr face) : engine (e), selected (sel), message (msg), typeface (face) {}

    std::function<void (int)> onSelect;

    void paint (Graphics& g) override
    {
        const int w = getWidth();
        const int64_t axis = jmax<int64_t> (1, engine.axisSamples());
        const int64_t now = engine.globalSamples.load();

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
        g.drawText (loopWord, 14, 12, 120, 28, Justification::centredLeft, false);
        g.setColour (hobby::kText);
        g.drawText (" Loop / " + (recording >= 0 ? String (recording + 1) : String ("-")),
                    14 + (int) std::ceil (textWidth (f, loopWord)), 12, 200, 28, Justification::centredLeft, false);

        const auto tapeState = engine.recorder.getState();
        const bool taping = tapeState == RecorderEngine::State::Recording;
        const String audioRest = tapeState == RecorderEngine::State::Error ? String (" Audio / ERROR")
                                 : " Audio / " + clockText (engine.recorder.getElapsedSeconds());
        const float restW = textWidth (f, audioRest), recW = textWidth (f, "REC");
        g.setColour (hobby::kText);
        g.drawText (audioRest, w - 14 - (int) std::ceil (restW) - 4, 12, (int) std::ceil (restW) + 4, 28, Justification::centredRight, false);
        g.setColour (taping ? hobby::kRed : hobby::kIconGrey);
        g.drawText ("REC", w - 14 - (int) std::ceil (restW) - (int) std::ceil (recW) - 4, 12, (int) std::ceil (recW) + 4, 28,
                    Justification::centredRight, false);

        const int taps = selected >= 0 ? engine.loopTapsOut[(size_t) selected].load() : 0;
        if (message.isNotEmpty())
        {
            g.setColour (hobby::kCoral);
            g.drawText (message, 260, 12, w - 520, 28, Justification::centred, true);
        }
        else if (taps > 0)
        {
            g.setColour (hobby::kLilac);
            g.drawText ("STOP x" + String (taps), 260, 12, w - 520, 28, Justification::centred, false);
        }

        // λωρίδες
        const auto list = activeBlocks (engine);
        const int count = list.size();
        const int top = 48;
        const int step = count > 0 ? jmin (44, 176 / count) : 44;
        const int laneH = jmax (6, step - 6);
        const float laneX = 42.0f, laneW = (float) (w - 14) - laneX;

        stepOut = step; laneHOut = laneH; topOut = top;

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
        g.setColour (hobby::kGridLine);
        for (int i = 0; i <= 4; ++i)
        {
            const float x = laneX + laneW * (float) i / 4.0f;
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
                const int bars = jmax (1, engine.loopBarsOut[(size_t) b].load());
                const int n = jmax (1, flow::axisBars / bars);
                const float len = laneW / (float) n;
                const int layers = engine.loopLayersOut[(size_t) b].load();
                const unsigned allMask = layers > 0 ? ((1u << layers) - 1u) : 0u;
                const bool allMuted = layers > 0 && ((unsigned) engine.loopMuteOut[(size_t) b].load() & allMask) == allMask;

                for (int k = 0; k < n; ++k)
                {
                    const Rectangle<float> seg (laneX + (float) k * len + 0.002f * laneW, y, len - 0.004f * laneW, lh);
                    g.setColour (allMuted ? Colour (0xff3a3d46) : blockLight (b));
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
                const int64_t start = engine.recStartOut[(size_t) b].load();
                const int64_t elapsed = jlimit<int64_t> (0, axis, now - start);
                const int64_t p0 = start % axis;
                auto piece = [&] (int64_t from, int64_t len)
                {
                    const float x = laneX + laneW * (float) from / (float) axis;
                    const float ww = laneW * (float) len / (float) axis;
                    g.setColour (hobby::kRed.withAlpha (0.35f));
                    g.fillRoundedRectangle (x - 2.0f, y - 2.0f, ww + 4.0f, lh + 4.0f, corner + 1.0f);
                    g.setColour (hobby::kCoral);
                    g.fillRoundedRectangle (x, y, jmax (2.0f, ww), lh, corner);
                };
                const int64_t firstLen = jmin (elapsed, axis - p0);
                piece (p0, firstLen);
                if (elapsed > firstLen) piece (0, elapsed - firstLen);
            }
        }

        const float hx = laneX + laneW * (float) (now % axis) / (float) axis;
        g.setColour (Colours::white);
        g.fillRect (hx - 1.0f, (float) (top - 8), 2.0f, (float) (gridBottom - top + 8));
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
    const String& message;
    Typeface::Ptr typeface;
    int stepOut = 0, laneHOut = 0, topOut = 48;
};

//==============================================================================
// Πληκτρολόγιο 88 πλήκτρων: κάθε block έχει δικό του χρώμα, αριθμό, καμπύλη έντασης και γραμμή έντασης.
class KeyboardView : public Component
{
public:
    KeyboardView (flow::Engine& e, const int& sel, Typeface::Ptr face) : engine (e), selected (sel), typeface (face) {}

    std::function<void (int)> onSelect, onRedoZone, onCurve, onVolumeMenu;
    std::function<void (int, float)> onVolume;

    void paint (Graphics& g) override
    {
        const int sel = selected;
        const double t = Time::getMillisecondCounterHiRes() * 0.001;
        const float freeAlpha = 0.08f + 0.92f * hobby::pulse (t / 3.0);   // τα ελεύθερα πλήκτρα σβήνουν και ανάβουν αργά όσο περιμένει ζώνη
        const int cb = engine.pendingBlock();
        const bool waiting = cb >= 0;
        const int firstKey = waiting && engine.zoneState[(size_t) cb].load() == 2 ? engine.zoneFirst[(size_t) cb].load() : -1;

        // επικεφαλίδα: BLOCK n αριστερά, περιοχή νοτών δεξιά
        g.setFont (hobby::makeFont (typeface, 19.0f));
        g.setColour (hobby::kText);
        const bool selValid = sel >= 0 && sel < flow::numBlocks && engine.active[(size_t) sel].load() != 0;
        if (selValid)
        {
            g.drawText ("BLOCK " + String (sel + 1), 14, 12, getWidth() - 28, 28, Justification::centredLeft, false);
            if (engine.zoneState[(size_t) sel].load() == 0)
                g.drawText (noteName (engine.lo[(size_t) sel].load()) + " " + String::fromUTF8 ("\xe2\x80\x93") + " " + noteName (engine.hi[(size_t) sel].load()),
                            14, 12, getWidth() - 28, 28, Justification::centredRight, false);
        }

        const float x0 = 14.0f;
        kw = (float) getWidth() - 28.0f;
        const float ww = kw / 52.0f;
        labelY = 12.0f + 28.0f + 6.0f;
        keysY = labelY + 22.0f;

        auto isBlack = [] (int n) { const int m = n % 12; return m == 1 || m == 3 || m == 6 || m == 8 || m == 10; };

        for (auto& s : spans) s = Span();
        std::vector<Rectangle<float>> blackRects;
        std::vector<Colour> blackCols;
        std::vector<float> seps;

        int wi = 0, prev = -1;
        for (int n = flow::lowestNote; n <= flow::highestNote; ++n)
        {
            const int b = engine.blockForNote (n);
            const bool black = isBlack (n);
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
            const float alpha = (b < 0 && waiting && ! isFirst) ? freeAlpha : 1.0f;

            if (black)
            {
                blackRects.push_back ({ l, keysY, r - l, 34.0f });
                Colour c = b >= 0 ? blockDeep (b) : (isFirst ? blockDeep (cb) : Colour (0xff4a4038));
                blackCols.push_back (c.withMultipliedAlpha (alpha));
            }
            else
            {
                const Rectangle<float> kr (l, keysY, r - l, 56.0f);
                Colour c = b >= 0 ? blockLight (b) : (isFirst ? blockLight (cb) : Colour (0xffece7df));
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

        // πάνω από κάθε ζώνη: αριθμός, ένδειξη hold, κουμπί καμπύλης. Από κάτω: γραμμή έντασης.
        const auto numFont = hobby::makeFont (typeface, 13.0f);
        const auto holdFont = hobby::makeFont (typeface, 11.0f);
        for (int b = 0; b < flow::numBlocks; ++b)
        {
            auto& s = spans[(size_t) b];
            btn[(size_t) b] = {};
            barRect[(size_t) b] = {};
            if (! s.used) continue;

            const float width = s.r - s.l;
            const Colour col = blockLight (b);
            const String num (b + 1);
            const int hold = engine.holdMode[(size_t) b].load();
            const String holdText = hold == 1 ? "LM" : hold == 2 ? "LP" : String();

            const float numW = textWidth (numFont, num);
            const float holdW = holdText.isEmpty() ? 0.0f : textWidth (holdFont, holdText);
            float total = numW + 6.0f + 24.0f;
            bool showBtn = true, showHold = holdText.isNotEmpty();
            if (showHold && total + 6.0f + holdW > width) showHold = false;
            if (showHold) total += 6.0f + holdW;
            if (total > width) { showBtn = false; total = numW + (showHold ? 6.0f + holdW : 0.0f); }

            float x = s.l + (width - total) * 0.5f;
            const float alpha = b == sel ? 1.0f : 0.65f;
            g.setFont (numFont);
            g.setColour (col.withMultipliedAlpha (alpha));
            g.drawText (num, Rectangle<float> (x, labelY, numW + 2.0f, 16.0f), Justification::centredLeft, false);
            x += numW + 6.0f;
            if (showHold)
            {
                g.setFont (holdFont);
                g.drawText (holdText, Rectangle<float> (x, labelY, holdW + 2.0f, 16.0f), Justification::centredLeft, false);
                x += holdW + 6.0f;
            }
            if (showBtn)
            {
                const Rectangle<float> br (x, labelY, 24.0f, 16.0f);
                btn[(size_t) b] = br;
                g.setGradientFill (ColourGradient (Colour (0xff46403b), 0.0f, br.getY(), Colour (0xff2a2623), 0.0f, br.getBottom(), false));
                g.fillRoundedRectangle (br, 5.0f);
                g.setColour (Colour (0xff1c1917));
                g.drawRoundedRectangle (br.reduced (0.5f), 5.0f, 1.0f);
                g.setColour (col);
                g.strokePath (curvePath (engine.curveMode[(size_t) b].load(), br), PathStrokeType (1.6f, PathStrokeType::curved, PathStrokeType::rounded));
            }

            const Rectangle<float> bar (s.l + 0.006f * kw, keysY - 4.0f, width - 0.012f * kw, 3.0f);
            barRect[(size_t) b] = bar;
            g.setColour (Colour (0xff3a3430));
            g.fillRoundedRectangle (bar, 1.5f);
            g.setColour (col);
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

            if (e.mods.isPopupMenu())
            {
                if (onBar) { if (onVolumeMenu) onVolumeMenu (b); }
                else if (p.y < keysY) { if (onRedoZone) onRedoZone (b); }
                return;
            }

            if (btn[(size_t) b].expanded (2.0f).contains (p)) { if (onCurve) onCurve (b); return; }

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

    static Path curvePath (int c, Rectangle<float> r)
    {
        Path p;
        switch (c)
        {
            case 1:  p.startNewSubPath (4.0f, 11.0f); p.cubicTo (12.0f, 11.0f, 16.0f, 8.0f, 18.0f, 3.0f); break;
            case 2:  p.startNewSubPath (4.0f, 11.0f); p.cubicTo (6.0f, 6.0f, 10.0f, 3.0f, 18.0f, 3.0f); break;
            case 3:  p.startNewSubPath (4.0f, 3.0f);  p.lineTo (18.0f, 11.0f); break;
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
    Typeface::Ptr typeface;
    std::array<Span, flow::numBlocks> spans;
    std::array<Rectangle<float>, flow::numBlocks> btn, barRect;
    float kw = 1.0f, labelY = 46.0f, keysY = 68.0f;
    int dragBlock = -1;
};

//==============================================================================
class MainComponent : public Component,
                      private Timer,
                      private ComponentListener
{
public:
    MainComponent()
        : typeface (hobby::loadTypeface()),
          loopsView (engine, selected, message, typeface),
          keyboardView (engine, selected, typeface),
          vstTile (Symbol::Vst, typeface),
          holdTile (Symbol::Hold, typeface),
          recTile (Symbol::PadRecord, typeface),
          undoTile (Symbol::Undo, typeface),
          stopTile (Symbol::Stop, typeface),
          tapeTile (Symbol::Stop, typeface),
          folderTile (Symbol::Folder, typeface)
    {
        PropertiesFile::Options options;
        options.applicationName     = "Flow";
        options.filenameSuffix      = ".settings";
        options.osxLibrarySubFolder = "Application Support";
        properties.setStorageParameters (options);

        formatManager.addFormat (std::make_unique<VST3PluginFormat>());
       #if JUCE_MAC
        formatManager.addFormat (std::make_unique<AudioUnitPluginFormat>());
       #endif

        for (auto* t : tiles())
            addAndMakeVisible (t);
        for (auto* t : { &recTile, &undoTile, &stopTile, &tapeTile })
            t->setRound (true);

        vstTile.setTooltip (gr ("VST: φόρτωση plugin (δημιουργεί νέο block). Δεξί κλικ: αφαίρεση του επιλεγμένου block"));
        folderTile.setTooltip (gr ("Φάκελος αποθήκευσης των WAV του Tape Recorder"));
        recTile.setTooltip ("Rec / Play / Overdub: " + gr ("ηχογράφηση, αναπαραγωγή, overdub του loop"));
        undoTile.setTooltip ("Undo: " + gr ("αφαιρεί το τελευταίο στρώμα του loop"));
        stopTile.setTooltip ("Stop: " + gr ("πάτα N φορές για σίγαση ή επαναφορά του στρώματος N"));
        tapeTile.setTooltip ("Tape Recorder: " + gr ("σταμάτημα ή νέο take"));

        addAndMakeVisible (loopsView);
        addAndMakeVisible (keyboardView);

        vstTile.onClick = [this] { chooseAndLoadPlugin(); };
        vstTile.onRightClick = [this] { showVstMenu(); };

        holdTile.onClick = [this]
        {
            if (! isActive (selected)) return;
            engine.holdMode[(size_t) selected] = (engine.holdMode[(size_t) selected].load() + 1) % 3;
            saveNow();
            refreshAll();
        };

        recTile.onClick  = [this] { if (isActive (selected)) engine.pressMain (selected); };
        undoTile.onClick = [this] { if (isActive (selected)) engine.pressUndo (selected); };
        stopTile.onClick = [this] { if (isActive (selected)) engine.pressStop (selected); };
        tapeTile.onClick = [this] { engine.recorder.pressButton(); };
        folderTile.onClick = [this] { chooseTapeFolder(); };

        // δεξί κλικ: ανάθεση σε πετάλι
        holdTile.onRightClick = [this] { showPedalMenu (flow::ctlHold); };
        recTile.onRightClick  = [this] { showPedalMenu (flow::ctlRec); };
        undoTile.onRightClick = [this] { showPedalMenu (flow::ctlUndo); };
        stopTile.onRightClick = [this] { showPedalMenu (flow::ctlStop); };
        tapeTile.onRightClick = [this] { showPedalMenu (flow::ctlTape); };

        loopsView.onSelect = [this] (int b) { selectBlock (b); };
        keyboardView.onSelect = [this] (int b) { selectBlock (b); };
        keyboardView.onRedoZone = [this] (int b) { showZoneMenu (b); };
        keyboardView.onCurve = [this] (int b)
        {
            engine.curveMode[(size_t) b] = (engine.curveMode[(size_t) b].load() + 1) % flow::numCurves;
            saveNow();
            refreshAll();
        };
        keyboardView.onVolume = [this] (int b, float gainValue)
        {
            engine.volPos[(size_t) b] = flow::curveInverse (engine.curveMode[(size_t) b].load(), gainValue);
            dirty = true;
        };
        keyboardView.onVolumeMenu = [this] (int b) { showVolumeMenu (b); };

        auto* settings = properties.getUserSettings();

        for (int i = 0; i < flow::numControls; ++i)
            engine.mapCode[(size_t) i] = settings->getIntValue ("ctl" + String (i), 0);

        const String savedFolder = settings->getValue ("tapeFolder");
        engine.recorder.setFolder (savedFolder.isNotEmpty() ? File (savedFolder)
                                                            : File::getSpecialLocation (File::userMusicDirectory).getChildFile ("Flow"));

        selected = settings->getIntValue ("selectedBlock", 0);
        engine.selectedBlock = selected;

        std::unique_ptr<XmlElement> savedAudio (settings->getXmlValue ("audioDeviceState"));
        deviceManager.initialise (0, 2, savedAudio.get(), true);

        for (auto& device : MidiInput::getAvailableDevices())
            deviceManager.setMidiInputDeviceEnabled (device.identifier, true);

        deviceManager.addAudioCallback (&engine);
        deviceManager.addMidiInputDeviceCallback (String(), &engine.incoming);

        refreshSize();
        restorePlugins();
        refreshAll();
        lastChange = engine.changeCount.load();
        startTimerHz (30);
    }

    ~MainComponent() override
    {
        stopTimer();
        saveSettings (true);
        deviceManager.removeMidiInputDeviceCallback (String(), &engine.incoming);
        deviceManager.removeAudioCallback (&engine);
        closeEditor();
        for (int b = 0; b < flow::numBlocks; ++b)
            engine.swapPlugin (b, nullptr).reset();
    }

    //==========================================================================
    void paint (Graphics& g) override
    {
        hobby::drawBody (g, getLocalBounds());
        hobby::drawBrandMark (g, typeface, getWidth());

        hobby::drawBezel (g, loopsBezel.toFloat(), loopsBezel.reduced (5).toFloat(), hobby::kScreen, 18.0f);
        if (! pluginBezel.isEmpty())
            hobby::drawBezel (g, pluginBezel.toFloat(), pluginBezel.reduced (5).toFloat(), hobby::kScreen, 1.0f);
        hobby::drawBezel (g, stripBezel.toFloat(), stripBezel.reduced (5).toFloat(), hobby::kStripScreen, 15.0f);
        hobby::drawBezel (g, keysBezel.toFloat(), keysBezel.reduced (5).toFloat(), hobby::kScreen, 18.0f);

        for (auto* t : tiles())
        {
            auto r = t->getBounds().toFloat();
            if (t == &recTile || t == &undoTile || t == &stopTile || t == &tapeTile)
            {
                Path p;
                p.addEllipse (r);
                DropShadow (Colours::black.withAlpha (0.55f), 4, { 0, 3 }).drawForPath (g, p);
            }
            else
            {
                hobby::drawTileShadow (g, r);
            }
        }
    }

    void resized() override { placeChildren(); }

    void mouseUp (const MouseEvent& e) override
    {
        // κλικ στο "π / -" ανοίγει τις ρυθμίσεις ήχου και MIDI
        if (e.y < 30 && std::abs (e.x - getWidth() / 2) < 40 && ! e.mouseWasDraggedSinceMouseDown())
            openAudioSettings();
    }

private:
    //==========================================================================
    std::array<ValueTile*, 7> tiles() { return { &vstTile, &holdTile, &recTile, &undoTile, &stopTile, &tapeTile, &folderTile }; }

    bool isActive (int b) const { return b >= 0 && b < flow::numBlocks && engine.active[(size_t) b].load() != 0; }

    void selectBlock (int b)
    {
        if (b != -1 && ! isActive (b)) return;
        selected = b;
        engine.selectedBlock = b;
        updateEditor();
        refreshAll();
        saveNow();
    }

    int firstActive() const
    {
        for (int b = 0; b < flow::numBlocks; ++b)
            if (isActive (b)) return b;
        return -1;
    }

    // γεωμετρία παραθύρου
    void computeLayout()
    {
        const bool shown = editor != nullptr;
        const int ew = shown ? editor->getWidth()  : 0;
        const int eh = shown ? editor->getHeight() : 0;

        const int fixedH = 36 + 246 + 8 + 72 + 8 + 152 + 14;
        int avail = 100000;
        if (auto* d = Desktop::getInstance().getDisplays().getPrimaryDisplay())
            avail = d->userArea.getHeight() - 30;

        editorScale = 1.0f;
        if (shown && eh > 0)
        {
            const int room = avail - fixedH - 18;
            if (eh > room) editorScale = jlimit (0.4f, 1.0f, (float) jmax (1, room) / (float) eh);
        }

        const int seh = shown ? (int) std::ceil ((float) eh * editorScale) : 0;
        const int sew = shown ? (int) std::ceil ((float) ew * editorScale) : 0;

        winW = jmax (992, sew + 32);
        winH = fixedH + (shown ? seh + 10 + 8 : 0);

        int y = 36;
        loopsBezel = { 11, y, winW - 22, 246 };
        y += 246 + 8;
        if (shown)
        {
            pluginBezel = { 11, y, winW - 22, seh + 10 };
            y += seh + 10 + 8;
        }
        else
        {
            pluginBezel = {};
        }
        stripBezel = { 11, y, winW - 22, 72 };
        y += 72 + 8;
        keysBezel = { 11, y, winW - 22, 152 };
    }

    void placeChildren()
    {
        loopsView.setBounds (loopsBezel.reduced (5));
        keyboardView.setBounds (keysBezel.reduced (5));

        // τρεις ομάδες με ίσες αποστάσεις: αριστερά VST + Hold, στη μέση τα 4 στρογγυλά, δεξιά ο φάκελος
        const auto strip = stripBezel.reduced (5);
        const int ty = strip.getY() + 5;
        const int leftX = strip.getX() + 14;
        const int leftEnd = leftX + 52 + 8 + 52;
        const int rightX = strip.getRight() - 14 - 52;
        const int midW = 4 * 52 + 3 * 8;
        const int gap = jmax (8, (rightX - leftEnd - midW) / 2);
        const int midX = leftEnd + gap;

        vstTile.setBounds (leftX, ty, 52, 52);
        holdTile.setBounds (leftX + 60, ty, 52, 52);
        int x = midX;
        for (auto* t : { &recTile, &undoTile, &stopTile, &tapeTile })
        {
            t->setBounds (x, ty, 52, 52);
            x += 60;
        }
        folderTile.setBounds (rightX, ty, 52, 52);

        if (editor != nullptr)
        {
            const auto area = pluginBezel.reduced (5);
            const int sew = (int) std::ceil ((float) editor->getWidth() * editorScale);
            layouting = true;
            editor->setTransform (AffineTransform::scale (editorScale));
            editor->setTopLeftPosition (area.getCentreX() - sew / 2, area.getY());
            layouting = false;
        }
    }

    void refreshSize()
    {
        computeLayout();

        if (auto* w = findParentComponentOfClass<DocumentWindow>())
        {
            if (w->getContentComponent() == this)
            {
                if (getWidth() != winW || getHeight() != winH)
                    w->setContentComponentSize (winW, winH);
            }
        }
        else if (getWidth() != winW || getHeight() != winH)
        {
            setSize (winW, winH);
        }

        placeChildren();
        repaint();
    }

    void parentHierarchyChanged() override { refreshSize(); }

    //==========================================================================
    // plugin editor μέσα στο παράθυρο (πάντα για το επιλεγμένο block)
    void closeEditor()
    {
        if (editor != nullptr)
        {
            editor->removeComponentListener (this);
            removeChildComponent (editor.get());
            editor.reset();
        }
        editorBlock = -1;
    }

    void updateEditor()
    {
        auto* plugin = isActive (selected) ? engine.getPlugin (selected) : nullptr;

        if (plugin == nullptr)
        {
            closeEditor();
        }
        else if (editor == nullptr || editorBlock != selected)
        {
            closeEditor();

            AudioProcessorEditor* e = plugin->hasEditor() ? plugin->createEditorIfNeeded() : nullptr;
            if (e == nullptr)
                e = new GenericAudioProcessorEditor (*plugin);

            editor.reset (e);
            editorBlock = selected;
            addAndMakeVisible (editor.get());
            editor->addComponentListener (this);
        }

        refreshSize();
    }

    void componentMovedOrResized (Component& c, bool, bool wasResized) override
    {
        if (&c == editor.get() && wasResized && ! layouting)
            refreshSize();
    }

    //==========================================================================
    // φόρτωση plugin: κάθε plugin δημιουργεί ένα νέο block
    void showVstMenu()
    {
        PopupMenu menu;
        menu.addItem (1, gr ("Φόρτωση plugin (νέο block)…"));
        menu.addItem (2, isActive (selected) ? gr ("Αφαίρεση block ") + String (selected + 1) : gr ("Αφαίρεση block"), isActive (selected));

        menu.showMenuAsync (PopupMenu::Options().withTargetComponent (&vstTile),
                            [safe = SafePointer<MainComponent> (this)] (int result)
        {
            if (safe == nullptr) return;
            if (result == 1) safe->chooseAndLoadPlugin();
            if (result == 2) safe->removeBlock (safe->selected);
        });
    }

    void chooseAndLoadPlugin()
    {
        int slot = -1;
        for (int b = 0; b < flow::numBlocks && slot < 0; ++b)
            if (! isActive (b) && ! loadingSlots.contains (b))
                slot = b;

        if (slot < 0)
        {
            setMessage (gr ("Δεν υπάρχει ελεύθερο block (μέγιστο 16)."));
            return;
        }

        chooser = std::make_unique<FileChooser> (gr ("Διάλεξε plugin (.vst3 ή .component)"),
                                                 File ("/Library/Audio/Plug-Ins"),
                                                 "*.vst3;*.component");

        auto flags = FileBrowserComponent::openMode
                   | FileBrowserComponent::canSelectFiles
                   | FileBrowserComponent::canSelectDirectories;

        chooser->launchAsync (flags, [this, slot] (const FileChooser& fc)
        {
            auto file = fc.getResult();
            if (file != File())
                loadFromFile (slot, file);
        });
    }

    void loadFromFile (int block, const File& file)
    {
        OwnedArray<PluginDescription> found;
        auto path = file.getFullPathName();

        for (auto* format : formatManager.getFormats())
            if (format->fileMightContainThisPluginType (path))
                format->findAllTypesForFile (found, path);

        if (found.isEmpty())
        {
            setMessage (gr ("Δεν βρέθηκε plugin σε αυτό το αρχείο."));
            return;
        }

        loadPlugin (block, *found.getFirst(), MemoryBlock(), true, true);
    }

    void setLoading (int block, bool loading)
    {
        auto* settings = properties.getUserSettings();
        StringArray list;
        list.addTokens (settings->getValue ("loadingBlocks"), ",", "");
        list.removeEmptyStrings();
        list.removeString (String (block));
        if (loading) list.add (String (block));
        settings->setValue ("loadingBlocks", list.joinIntoString (","));
        properties.saveIfNeeded();

        if (loading) loadingSlots.addIfNotAlreadyThere (block);
        else         loadingSlots.removeFirstMatchingValue (block);
    }

    // userLoad: το plugin φορτώθηκε τώρα από τον χρήστη -> νέο block, επιλέγεται και περιμένει ζώνη
    void loadPlugin (int block, const PluginDescription& description, const MemoryBlock& state, bool userLoad, bool captureZone)
    {
        setLoading (block, true);   // σημάδι ασφαλείας: αν κρασάρει, δεν ξαναφορτώνεται μόνο του

        double sampleRate = engine.getSampleRate();
        int bufferSize = 512;
        if (auto* device = deviceManager.getCurrentAudioDevice())
            bufferSize = device->getCurrentBufferSizeSamples();

        SafePointer<MainComponent> safeThis (this);

        formatManager.createPluginInstanceAsync (description, sampleRate, bufferSize,
            [safeThis, block, description, state, userLoad, captureZone] (std::unique_ptr<AudioPluginInstance> instance, const String& error)
            {
                if (safeThis != nullptr)
                    safeThis->pluginLoaded (block, std::move (instance), error, description, state, userLoad, captureZone);
            });
    }

    void pluginLoaded (int block, std::unique_ptr<AudioPluginInstance> instance, const String& error,
                       const PluginDescription& description, const MemoryBlock& state, bool userLoad, bool captureZone)
    {
        setLoading (block, false);

        if (instance == nullptr)
        {
            setMessage (gr ("Αποτυχία φόρτωσης: ") + error);
            return;
        }

        if (editorBlock == block)
            closeEditor();

        if (userLoad)    // νέο block με καθαρές ρυθμίσεις
        {
            engine.holdMode[(size_t) block] = 0;
            engine.curveMode[(size_t) block] = 0;
            engine.volPos[(size_t) block] = 1.0f;
            engine.volMap[(size_t) block] = 0;
        }

        descriptions[(size_t) block] = description;
        auto old = engine.swapPlugin (block, std::move (instance), captureZone);
        old.reset();

        if (state.getSize() > 0)
            if (auto* p = engine.getPlugin (block))
                p->setStateInformation (state.getData(), (int) state.getSize());

        if (userLoad || ! isActive (selected))
        {
            selected = block;
            engine.selectedBlock = block;
        }

        updateEditor();
        refreshAll();
        saveNow (true);
    }

    void removeBlock (int block)
    {
        if (! isActive (block)) return;

        if (editorBlock == block)
            closeEditor();

        auto old = engine.swapPlugin (block, nullptr);
        old.reset();
        descriptions[(size_t) block] = PluginDescription();

        engine.holdMode[(size_t) block] = 0;
        engine.curveMode[(size_t) block] = 0;
        engine.volPos[(size_t) block] = 1.0f;
        engine.volMap[(size_t) block] = 0;

        auto* settings = properties.getUserSettings();
        for (auto key : { "desc", "state", "lo", "hi", "seq", "zone", "hold", "curve", "vol", "vmap" })
            settings->removeValue ("b" + String (block) + "_" + key);

        if (selected == block)
        {
            selected = firstActive();
            engine.selectedBlock = selected;
        }

        updateEditor();
        refreshAll();
        saveNow();
    }

    void showZoneMenu (int b)
    {
        PopupMenu menu;
        menu.addItem (1, gr ("Ορισμός ζώνης ξανά"));
        menu.showMenuAsync (PopupMenu::Options().withTargetComponent (&keyboardView),
                            [safe = SafePointer<MainComponent> (this), b] (int result)
        {
            if (safe == nullptr || result != 1) return;
            safe->selectBlock (b);
            safe->engine.startZone (b);
            safe->refreshAll();
        });
    }

    void showVolumeMenu (int b)
    {
        const int code = engine.volMap[(size_t) b].load();

        PopupMenu menu;
        menu.addItem (1, gr ("Ανάθεση πεταλιού έντασης (MIDI learn)"));
        menu.addItem (2, gr ("Αφαίρεση ανάθεσης"), code != 0);
        if (code != 0)
            menu.addItem (3, gr ("Τώρα: ") + pedalCodeText (code), false);

        menu.showMenuAsync (PopupMenu::Options().withTargetComponent (&keyboardView),
                            [safe = SafePointer<MainComponent> (this), b] (int result)
        {
            if (safe == nullptr) return;
            if (result == 1)
            {
                safe->engine.startVolumeLearn (b);
                safe->setMessage (gr ("Κούνησε το πετάλι έντασης για το block ") + String (b + 1));
            }
            if (result == 2)
            {
                safe->engine.volMap[(size_t) b] = 0;
                safe->saveNow();
            }
        });
    }

    //==========================================================================
    void openAudioSettings()
    {
        auto* selector = new AudioDeviceSelectorComponent (deviceManager, 0, 0, 0, 2, true, false, true, false);
        selector->setSize (500, 450);

        DialogWindow::LaunchOptions options;
        options.content.setOwned (selector);
        options.dialogTitle = gr ("Ρυθμίσεις ήχου και MIDI");
        options.componentToCentreAround = this;
        options.dialogBackgroundColour = Colours::darkgrey;
        options.escapeKeyTriggersCloseButton = true;
        options.useNativeTitleBar = true;
        options.resizable = false;
        options.launchAsync();
    }

    void chooseTapeFolder()
    {
        chooser = std::make_unique<FileChooser> (gr ("Φάκελος για τα WAV του Tape Recorder"), engine.recorder.getFolder());

        chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectDirectories,
                              [this] (const FileChooser& fc)
        {
            auto dir = fc.getResult();
            if (dir != File())
            {
                engine.recorder.setFolder (dir);
                saveNow();
            }
        });
    }

    //==========================================================================
    // αποθήκευση: όλες οι αναθέσεις, ζώνες, καμπύλες, εντάσεις και plugins σώζονται μόλις αλλάξουν.
    // Η κατάσταση (preset) των plugins σώζεται όταν φορτώνεται, κάθε 20 δευτερόλεπτα και στο κλείσιμο.
    void saveNow (bool withStates = false)
    {
        saveSettings (withStates);
    }

    void saveSettings (bool withStates)
    {
        auto* settings = properties.getUserSettings();

        std::unique_ptr<XmlElement> audioState (deviceManager.createStateXml());
        settings->setValue ("audioDeviceState", audioState.get());
        settings->setValue ("selectedBlock", selected);
        settings->setValue ("tapeFolder", engine.recorder.getFolder().getFullPathName());

        for (int i = 0; i < flow::numControls; ++i)
            settings->setValue ("ctl" + String (i), engine.mapCode[(size_t) i].load());

        for (int b = 0; b < flow::numBlocks; ++b)
        {
            if (! isActive (b)) continue;
            const String n = "b" + String (b) + "_";

            settings->setValue (n + "desc", descriptions[(size_t) b].createXml().get());
            settings->setValue (n + "lo", engine.lo[(size_t) b].load());
            settings->setValue (n + "hi", engine.hi[(size_t) b].load());
            settings->setValue (n + "seq", engine.zoneSeq[(size_t) b].load());
            settings->setValue (n + "zone", engine.zoneState[(size_t) b].load() == 0 ? 1 : 0);
            settings->setValue (n + "hold", engine.holdMode[(size_t) b].load());
            settings->setValue (n + "curve", engine.curveMode[(size_t) b].load());
            settings->setValue (n + "vol", (double) engine.volPos[(size_t) b].load());
            settings->setValue (n + "vmap", engine.volMap[(size_t) b].load());

            if (withStates)
                if (auto* p = engine.getPlugin (b))
                {
                    MemoryBlock state;
                    p->getStateInformation (state);
                    settings->setValue (n + "state", state.toBase64Encoding());
                }
        }

        properties.saveIfNeeded();
        dirty = false;
        lastSaveMs = Time::getMillisecondCounter();
    }

    void restorePlugins()
    {
        auto* settings = properties.getUserSettings();

        StringArray crashed;
        crashed.addTokens (settings->getValue ("loadingBlocks"), ",", "");
        crashed.removeEmptyStrings();
        settings->setValue ("loadingBlocks", "");
        properties.saveIfNeeded();

        for (int b = 0; b < flow::numBlocks; ++b)
        {
            const String n = "b" + String (b) + "_";
            if (! settings->containsKey (n + "desc")) continue;

            if (crashed.contains (String (b)))
            {
                setMessage (gr ("Ένα plugin δεν φορτώθηκε σωστά την τελευταία φορά και παραλείφθηκε."));
                settings->removeValue (n + "desc");
                continue;
            }

            std::unique_ptr<XmlElement> xml (settings->getXmlValue (n + "desc"));
            if (xml == nullptr) continue;

            PluginDescription description;
            if (! description.loadFromXml (*xml)) continue;

            MemoryBlock state;
            state.fromBase64Encoding (settings->getValue (n + "state"));

            engine.holdMode[(size_t) b]  = jlimit (0, 2, settings->getIntValue (n + "hold", 0));
            engine.curveMode[(size_t) b] = jlimit (0, flow::numCurves - 1, settings->getIntValue (n + "curve", 0));
            engine.volPos[(size_t) b]    = (float) jlimit (0.0, 1.0, settings->getDoubleValue (n + "vol", 1.0));
            engine.volMap[(size_t) b]    = settings->getIntValue (n + "vmap", 0);

            const bool hasZone = settings->getIntValue (n + "zone", 0) != 0;
            engine.setZone (b, settings->getIntValue (n + "lo", flow::lowestNote), settings->getIntValue (n + "hi", flow::lowestNote),
                            settings->getIntValue (n + "seq", 0));

            loadPlugin (b, description, state, false, ! hasZone);
        }
    }

    //==========================================================================
    static const char* controlName (int i)
    {
        static const char* const names[flow::numControls] = { "Hold", "Rec / Play / Overdub", "Undo", "Stop", "Tape Recorder" };
        return names[i];
    }

    static String pedalCodeText (int code)
    {
        const int kind = code >> 16, ch = (code >> 8) & 0xff, num = code & 0xff;
        const String type = kind == 1 ? "Note" : kind == 2 ? "CC" : "Program";
        return type + " " + String (num) + " (ch " + String (ch) + ")";
    }

    void showPedalMenu (int i)
    {
        const int code = engine.mapCode[(size_t) i].load();

        PopupMenu menu;
        menu.addItem (1, gr ("Ανάθεση πεταλιού (MIDI learn)"));
        menu.addItem (2, gr ("Αφαίρεση ανάθεσης"), code != 0);
        if (code != 0)
            menu.addItem (3, gr ("Τώρα: ") + pedalCodeText (code), false);

        menu.showMenuAsync (PopupMenu::Options().withTargetComponent (pedalTile (i)),
                            [safe = SafePointer<MainComponent> (this), i] (int result)
        {
            if (safe == nullptr) return;
            if (result == 1) safe->startPedalLearn (i);
            if (result == 2) { safe->engine.mapCode[(size_t) i] = 0; safe->saveNow(); }
        });
    }

    ValueTile* pedalTile (int i)
    {
        switch (i)
        {
            case flow::ctlHold: return &holdTile;
            case flow::ctlRec:  return &recTile;
            case flow::ctlUndo: return &undoTile;
            case flow::ctlStop: return &stopTile;
            default:            return &tapeTile;
        }
    }

    void startPedalLearn (int i)
    {
        engine.cancelLearns();
        engine.mapLearnTarget = i;
        setMessage (gr ("Πάτησε το πετάλι για: ") + controlName (i));
    }

    void setMessage (const String& m) { message = m; messageTicks = 300; }

    void refreshAll()
    {
        const int sel = selected;
        const bool valid = isActive (sel);

        vstTile.setIconColour (valid ? hobby::kOn : hobby::kIconGrey);

        const int hold = valid ? engine.holdMode[(size_t) sel].load() : 0;
        holdTile.setDots (hold);
        holdTile.setIconColour (hold == 0 ? hobby::kIconGrey : hobby::kOn);
        holdTile.setTooltip (gr ("Hold (επιλεγμένο block): πάτημα = Off / LM Mono Latch (μία νότα) / LP Poly Latch (συγχορδία). Τώρα: ")
                             + (hold == 0 ? "Off" : hold == 1 ? "LM" : "LP"));

        const int loop = valid ? engine.loopStateOut[(size_t) sel].load() : (int) flow::loopOff;
        const int mute = valid ? engine.loopMuteOut[(size_t) sel].load() : 0;
        const int taps = valid ? engine.loopTapsOut[(size_t) sel].load() : 0;
        const double t = Time::getMillisecondCounterHiRes() * 0.001;

        Symbol recSymbol = Symbol::PadRecord;
        Colour recColour = hobby::kIconGrey;
        if (loop == flow::loopRec)       { recSymbol = Symbol::Play;       recColour = hobby::mix (hobby::kIconGrey, hobby::kRed,   hobby::pulse (t * 1.5)); }
        else if (loop == flow::loopPlay) { recSymbol = Symbol::PadOverdub; recColour = hobby::kOn; }
        else if (loop == flow::loopOver) { recSymbol = Symbol::Play;       recColour = hobby::mix (hobby::kIconGrey, hobby::kPeach, hobby::pulse (t * 1.5)); }
        recTile.setSymbol (recSymbol);
        recTile.setIconColour (recColour);

        undoTile.setIconColour (loop != flow::loopOff ? hobby::kBlue : hobby::kIconGrey);

        stopTile.setIconColour (taps > 0 ? hobby::kLilac
                                         : mute != 0 ? hobby::mix (hobby::kIconGrey, hobby::kLilac, 0.4f + 0.6f * hobby::pulse (t * 0.5))
                                                     : hobby::kIconGrey);

        switch (engine.recorder.getState())
        {
            case RecorderEngine::State::Recording: tapeTile.setIconColour (hobby::mix (hobby::kIconGrey, hobby::kRed, 0.5f + 0.5f * hobby::pulse (t * 1.0))); break;
            case RecorderEngine::State::Stopped:   tapeTile.setIconColour (engine.recorder.isTapeFull() ? hobby::kCoral : hobby::kPeach); break;
            case RecorderEngine::State::Error:     tapeTile.setIconColour (hobby::kCoral); break;
            default:                               tapeTile.setIconColour (hobby::kIconGrey); break;
        }

        loopsView.repaint();
        keyboardView.repaint();
    }

    void timerCallback() override
    {
        if (messageTicks > 0 && --messageTicks == 0)
        {
            message.clear();
            engine.cancelLearns();
        }

        const int learned = engine.learnedCount.load();
        if (learned != lastLearned)
        {
            lastLearned = learned;
            const int target = engine.learnedTarget.load();
            if (target >= 0 && target < flow::numControls)
                setMessage (String (controlName (target)) + " " + String::fromUTF8 ("\xe2\x86\x90") + " " + pedalCodeText (engine.learnedCode.load()));
        }

        const int vlearned = engine.volLearnedCount.load();
        if (vlearned != lastVolLearned)
        {
            lastVolLearned = vlearned;
            setMessage ("Block " + String (engine.volLearnedBlock.load() + 1) + " " + String::fromUTF8 ("\xe2\x86\x90") + " "
                        + pedalCodeText (engine.volLearnedCode.load()));
        }

        // αλλαγές από πετάλια (hold, ένταση, αναθέσεις, ζώνες) και από το ποντίκι σώζονται άμεσα (με όριο ρυθμού)
        const int cc = engine.changeCount.load();
        if (cc != lastChange) { lastChange = cc; dirty = true; }
        if (dirty && Time::getMillisecondCounter() - lastSaveMs > 400)
            saveSettings (false);

        if (++stateTicks >= 600)
        {
            stateTicks = 0;
            saveSettings (true);
        }

        refreshAll();

        if (editor == nullptr && isActive (selected))
            updateEditor();
    }

    //==========================================================================
    flow::Engine engine;
    ApplicationProperties properties;
    AudioDeviceManager deviceManager;
    AudioPluginFormatManager formatManager;
    TooltipWindow tooltipWindow { nullptr, 600 };

    Typeface::Ptr typeface;
    int selected = 0;
    bool layouting = false;
    int winW = 992, winH = 700;
    float editorScale = 1.0f;
    Rectangle<int> loopsBezel, pluginBezel, stripBezel, keysBezel;
    String message;
    int lastLearned = 0, lastVolLearned = 0, lastChange = 0;
    int messageTicks = 0, stateTicks = 0;
    bool dirty = false;
    uint32 lastSaveMs = 0;
    Array<int> loadingSlots;

    std::array<PluginDescription, flow::numBlocks> descriptions;
    std::unique_ptr<AudioProcessorEditor> editor;
    int editorBlock = -1;
    std::unique_ptr<FileChooser> chooser;

    LoopsView loopsView;
    KeyboardView keyboardView;
    ValueTile vstTile, holdTile, recTile, undoTile, stopTile, tapeTile, folderTile;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

//==============================================================================
class FlowApplication : public JUCEApplication
{
public:
    const String getApplicationName() override    { return "Flow"; }
    const String getApplicationVersion() override { return "0.3.0"; }
    bool moreThanOneInstanceAllowed() override    { return false; }

    void initialise (const String&) override
    {
        mainWindow = std::make_unique<MainWindow> (getApplicationName());
    }

    void shutdown() override { mainWindow.reset(); }

    void systemRequestedQuit() override { quit(); }

    class MainWindow : public DocumentWindow
    {
    public:
        explicit MainWindow (const String& name)
            : DocumentWindow (name, hobby::kBodyBottom, DocumentWindow::closeButton | DocumentWindow::minimiseButton)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent(), true);
            setResizable (false, false);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override
        {
            JUCEApplication::getInstance()->systemRequestedQuit();
        }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

private:
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (FlowApplication)
