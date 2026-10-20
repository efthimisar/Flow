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

//==============================================================================
// Οθόνη loops: μία λωρίδα ανά block, 4 μέτρα, λευκή κεφαλή αναπαραγωγής.
class LoopsView : public Component
{
public:
    LoopsView (flow::Engine& e, const int& sel, const String& msg, Typeface::Ptr face) : engine (e), selected (sel), message (msg), typeface (face)
    {
        setInterceptsMouseClicks (false, false);
    }

    void paint (Graphics& g) override
    {
        const int w = getWidth();
        const int64_t axis = jmax<int64_t> (1, engine.axisSamples());
        const int64_t now = engine.globalSamples.load();

        int recording = -1;
        bool overdubbing = false;
        for (int b = 0; b < flow::numBlocks; ++b)
        {
            const int st = engine.loopStateOut[(size_t) b].load();
            if (st == flow::loopRec || st == flow::loopOver) { recording = b; overdubbing = st == flow::loopOver; break; }
        }

        g.setFont (hobby::makeFont (typeface, 19.0f));
        g.setColour (hobby::kText);
        String head = recording >= 0 ? String (overdubbing ? "OVERDUB " : "REC ") + String (recording + 1)
                                     : String ((int) flow::tempoBpm) + " BPM";
        head << "  |  loop " << flow::axisBars << " bars";
        if (const int taps = engine.loopTapsOut[(size_t) selected].load(); taps > 0)
            head << "  |  STOP x" << taps;
        g.drawText (head, 14, 12, w - 28, 28, Justification::centredLeft, false);
        if (message.isNotEmpty())
        {
            g.setColour (hobby::kCoral);
            g.drawText (message, 14, 12, w - 28, 28, Justification::centredRight, true);
        }

        const int top = 48, step = 11, laneH = 9;
        const float laneX = 42.0f, laneW = (float) (w - 14) - laneX;

        g.setFont (hobby::makeFont (typeface, 10.0f));
        for (int b = 0; b < flow::numBlocks; ++b)
        {
            g.setColour (b == selected ? hobby::kText : hobby::kIconGrey);
            g.drawText (String (b + 1), 14, top + b * step - 1, 24, 11, Justification::centredRight, false);
            g.setColour (Colour (0xff2d2926));
            g.fillRoundedRectangle (laneX, (float) (top + b * step), laneW, (float) laneH, 4.0f);
        }

        g.setColour (hobby::kGridLine);
        for (int i = 0; i <= 4; ++i)
        {
            const float x = laneX + laneW * (float) i / 4.0f;
            g.fillRect (x - 0.5f, (float) (top - 6), 1.0f, (float) (16 * step + 10));
        }

        for (int b = 0; b < flow::numBlocks; ++b)
        {
            const float y = (float) (top + b * step);
            const int state = engine.loopStateOut[(size_t) b].load();

            if (state == flow::loopPlay || state == flow::loopOver)
            {
                const int bars = jmax (1, engine.loopBarsOut[(size_t) b].load());
                const int n = jmax (1, flow::axisBars / bars);
                const float len = laneW / (float) n;
                const int layers = engine.loopLayersOut[(size_t) b].load();
                const unsigned allMask = layers > 0 ? ((1u << layers) - 1u) : 0u;
                const bool allMuted = layers > 0 && ((unsigned) engine.loopMuteOut[(size_t) b].load() & allMask) == allMask;

                for (int i = 0; i < n; ++i)
                {
                    const Rectangle<float> seg (laneX + (float) i * len + 0.002f * laneW, y, len - 0.004f * laneW, (float) laneH);
                    g.setColour (allMuted ? Colour (0xff3a3d46) : blockLight (b));
                    g.fillRoundedRectangle (seg, 4.0f);
                    if (state == flow::loopOver)
                    {
                        g.setColour (hobby::kPeach);
                        g.drawRoundedRectangle (seg.expanded (1.0f), 5.0f, 1.5f);
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
                    g.fillRoundedRectangle (x - 2.0f, y - 2.0f, ww + 4.0f, (float) laneH + 4.0f, 5.0f);
                    g.setColour (hobby::kCoral);
                    g.fillRoundedRectangle (x, y, jmax (2.0f, ww), (float) laneH, 4.0f);
                };
                const int64_t firstLen = jmin (elapsed, axis - p0);
                piece (p0, firstLen);
                if (elapsed > firstLen) piece (0, elapsed - firstLen);
            }
        }

        const float hx = laneX + laneW * (float) (now % axis) / (float) axis;
        g.setColour (Colours::white);
        g.fillRect (hx - 1.0f, (float) (top - 8), 2.0f, (float) (16 * step + 12));
    }

private:
    flow::Engine& engine;
    const int& selected;
    const String& message;
    Typeface::Ptr typeface;
};

//==============================================================================
// Πληκτρολόγιο 88 πλήκτρων: χρωματίζονται τα πλήκτρα του επιλεγμένου block.
class KeyboardView : public Component
{
public:
    KeyboardView (flow::Engine& e, const int& sel, Typeface::Ptr face) : engine (e), selected (sel), typeface (face)
    {
        setInterceptsMouseClicks (false, false);
    }

    void paint (Graphics& g) override
    {
        const int sel = selected;
        const int selLo = engine.lo[(size_t) sel].load();
        const int selHi = engine.hi[(size_t) sel].load();

        // status
        String status = "BLOCK " + String (sel + 1);
        const int stage = engine.learnStage.load();
        if (engine.learnBlock.load() == sel && stage == 1)
            status << "  |  " << gr ("Πάτησε το πρώτο πλήκτρο");
        else if (engine.learnBlock.load() == sel && stage == 2)
            status << "  |  " << gr ("Πάτησε το τελευταίο πλήκτρο");
        else
            status << "  |  " << noteName (selLo) << " " << String::fromUTF8 ("\xe2\x80\x93") << " " << noteName (selHi)
                   << "  |  " << String (selHi - selLo + 1) << " " << gr ("πλήκτρα");

        g.setFont (hobby::makeFont (typeface, 19.0f));
        g.setColour (hobby::kText);
        g.drawText (status, 14, 12, getWidth() - 28, 28, Justification::centredLeft, false);

        // ιδιοκτήτης κάθε νότας για διαχωριστικά και αριθμούς (το πρώτο block που τη περιέχει)
        int ownerOf[128];
        for (int n = 0; n < 128; ++n)
        {
            ownerOf[n] = -1;
            for (int b = 0; b < flow::numBlocks; ++b)
                if (n >= engine.lo[(size_t) b].load() && n <= engine.hi[(size_t) b].load()) { ownerOf[n] = b; break; }
        }

        const float x0 = 14.0f;
        const float kw = (float) getWidth() - 28.0f;
        const float ww = kw / 52.0f;
        const float labelY = 12.0f + 28.0f + 6.0f;
        const float keysY = labelY + 22.0f;

        auto isBlack = [] (int n) { const int m = n % 12; return m == 1 || m == 3 || m == 6 || m == 8 || m == 10; };

        struct Span { float l = 1e9f, r = -1e9f; bool used = false; };
        Span spans[flow::numBlocks];
        std::vector<Rectangle<float>> blackRects;
        std::vector<Colour> blackCols;
        std::vector<float> seps;

        int wi = 0, prev = -1;
        for (int n = flow::lowestNote; n <= flow::highestNote; ++n)
        {
            const int b = ownerOf[n];
            const bool black = isBlack (n);
            const float l = x0 + (black ? ((float) wi - 0.3f) * ww : (float) wi * ww);
            const float r = x0 + (black ? ((float) wi + 0.3f) * ww : (float) (wi + 1) * ww);
            const bool inSel = n >= selLo && n <= selHi;

            if (b >= 0 && prev >= 0 && b != prev)
                seps.push_back (x0 + (float) wi * ww);

            if (b >= 0)
            {
                auto& s = spans[b];
                s.used = true;
                s.l = jmin (s.l, l);
                s.r = jmax (s.r, r);
            }

            if (black)
            {
                blackRects.push_back ({ l, keysY, r - l, 34.0f });
                blackCols.push_back (inSel ? blockDeep (sel) : (b >= 0 ? Colour (0xff4a4038) : Colour (0xff332e2a)));
            }
            else
            {
                const Rectangle<float> kr (l, keysY, r - l, 56.0f);
                g.setColour (inSel ? blockLight (sel) : (b >= 0 ? Colour (0xffece7df) : Colour (0xff5a544e)));
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

        // αριθμοί blocks και μπάρα του επιλεγμένου
        g.setFont (hobby::makeFont (typeface, 13.0f));
        for (int b = 0; b < flow::numBlocks; ++b)
        {
            if (! spans[b].used) continue;
            g.setColour (b == sel ? blockLight (b) : hobby::kIconGrey);
            g.drawText (String (b + 1), Rectangle<float> (spans[b].l, labelY, spans[b].r - spans[b].l, 16.0f),
                        Justification::centred, false);
        }

        if (spans[sel].used)
        {
            g.setColour (blockLight (sel));
            const float bl = spans[sel].l + 0.006f * kw;
            g.fillRoundedRectangle (bl, labelY + 19.0f, spans[sel].r - spans[sel].l - 0.012f * kw, 3.0f, 2.0f);
        }
    }

private:
    flow::Engine& engine;
    const int& selected;
    Typeface::Ptr typeface;
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
          blockTile (Symbol::Block, typeface),
          learnTile (Symbol::Learn, typeface),
          vstTile (Symbol::Vst, typeface),
          windowTile (Symbol::Window, typeface),
          holdTile (Symbol::Hold, typeface),
          arpTile (Symbol::Arp, typeface),
          recTile (Symbol::PadRecord, typeface),
          undoTile (Symbol::Undo, typeface),
          stopTile (Symbol::Stop, typeface)
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

        StringArray blockNames;
        for (int i = 1; i <= flow::numBlocks; ++i) blockNames.add (String (i));
        blockTile.setValues (blockNames, false);
        learnTile.setValues ({ "Learn" }, false);
        vstTile.setValues ({ "VST" }, false);
        windowTile.setValues ({ "Window" }, false);
        holdTile.setValues ({ "Off", "Mono", "Poly" }, false);
        arpTile.setValues ({ "Off", "Mono", "Poly" }, false);
        recTile.setValues ({ "Rec" }, false);
        undoTile.setValues ({ "Undo" }, false);
        stopTile.setValues ({ "Stop" }, false);
        for (auto* t : { &recTile, &undoTile, &stopTile })
            t->setRound (true);

        for (auto* t : tiles())
            addAndMakeVisible (t);

        addAndMakeVisible (loopsView);
        addAndMakeVisible (keyboardView);

        blockTile.onClick = [this]
        {
            if (engine.learnBlock.load() >= 0) engine.cancelLearn();
            selected = (selected + 1) % flow::numBlocks;
            blockTile.setIndex (selected);
            updateEditor();
            refreshAll();
        };

        learnTile.onClick = [this]
        {
            if (engine.learnBlock.load() == selected) engine.cancelLearn();
            else engine.startLearn (selected);
            refreshAll();
        };

        vstTile.onClick = [this] { showVstMenu(); };

        windowTile.onClick = [this]
        {
            showEditor = ! showEditor;
            updateEditor();
            refreshAll();
        };

        holdTile.onClick = [this]
        {
            engine.holdMode[(size_t) selected] = (engine.holdMode[(size_t) selected].load() + 1) % 3;
            refreshAll();
        };

        arpTile.onClick = [this]
        {
            engine.arpMode[(size_t) selected] = (engine.arpMode[(size_t) selected].load() + 1) % 3;
            refreshAll();
        };

        recTile.onClick  = [this] { engine.pressMain (selected); };
        undoTile.onClick = [this] { engine.pressUndo (selected); };
        stopTile.onClick = [this] { engine.pressStop (selected); };

        // δεξί κλικ: ανάθεση σε πετάλι (όλα εκτός από το VST)
        for (int i = 0; i < flow::numControls; ++i)
            if (i != 2)
                tileAt (i)->onRightClick = [this, i] { showPedalMenu (i); };

        auto* settings = properties.getUserSettings();

        selected = jlimit (0, flow::numBlocks - 1, settings->getIntValue ("selectedBlock", 0));
        showEditor = settings->getBoolValue ("showEditor", false);
        for (int b = 0; b < flow::numBlocks; ++b)
        {
            if (settings->containsKey ("lo" + String (b)))
            {
                engine.lo[(size_t) b] = settings->getIntValue ("lo" + String (b), engine.lo[(size_t) b].load());
                engine.hi[(size_t) b] = settings->getIntValue ("hi" + String (b), engine.hi[(size_t) b].load());
            }
            engine.holdMode[(size_t) b] = jlimit (0, 2, settings->getIntValue ("hold" + String (b), 0));
            engine.arpMode[(size_t) b]  = jlimit (0, 2, settings->getIntValue ("arp" + String (b), 0));
        }
        for (int i = 0; i < flow::numControls; ++i)
            engine.mapCode[(size_t) i] = settings->getIntValue ("pmap" + String (i), 0);

        std::unique_ptr<XmlElement> savedAudio (settings->getXmlValue ("audioDeviceState"));
        deviceManager.initialise (0, 2, savedAudio.get(), true);

        for (auto& device : MidiInput::getAvailableDevices())
            deviceManager.setMidiInputDeviceEnabled (device.identifier, true);

        deviceManager.addAudioCallback (&engine);
        deviceManager.addMidiInputDeviceCallback (String(), &engine.incoming);

        refreshSize();
        restorePlugins();
        refreshAll();
        startTimerHz (30);
    }

    ~MainComponent() override
    {
        stopTimer();
        saveSettings();
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

        for (int i = 0; i < flow::numControls; ++i)
        {
            auto r = tileAt (i)->getBounds().toFloat();
            if (i < 6)
            {
                hobby::drawTileShadow (g, r);
            }
            else
            {
                Path p;
                p.addEllipse (r);
                DropShadow (Colours::black.withAlpha (0.55f), 4, { 0, 3 }).drawForPath (g, p);
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
    std::array<ValueTile*, 9> tiles() { return { &blockTile, &learnTile, &vstTile, &windowTile, &holdTile, &arpTile, &recTile, &undoTile, &stopTile }; }
    ValueTile* tileAt (int i) { return tiles()[(size_t) i]; }

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

        const auto strip = stripBezel.reduced (5);
        const int total = 9 * 52 + 8 * 8 + 22;
        int x = strip.getCentreX() - total / 2;
        int index = 0;
        for (auto* t : tiles())
        {
            t->setBounds (x, strip.getY() + 5, 52, 52);
            x += 60 + (index == 5 ? 22 : 0);
            ++index;
        }

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
    // plugin editor μέσα στο παράθυρο
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
        auto* plugin = engine.getPlugin (selected);
        const bool want = showEditor && plugin != nullptr;

        if (! want)
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
    // φόρτωση plugin σε block
    void showVstMenu()
    {
        PopupMenu menu;
        menu.addItem (1, gr ("Φόρτωση plugin…"));
        menu.addItem (2, gr ("Αφαίρεση plugin"), engine.getPlugin (selected) != nullptr);

        menu.showMenuAsync (PopupMenu::Options().withTargetComponent (&vstTile),
                            [safe = SafePointer<MainComponent> (this)] (int result)
        {
            if (safe == nullptr) return;
            if (result == 1) safe->chooseAndLoadPlugin();
            if (result == 2) safe->removePlugin (safe->selected);
        });
    }

    void chooseAndLoadPlugin()
    {
        chooser = std::make_unique<FileChooser> (gr ("Διάλεξε plugin (.vst3 ή .component)"),
                                                 File ("/Library/Audio/Plug-Ins"),
                                                 "*.vst3;*.component");

        auto flags = FileBrowserComponent::openMode
                   | FileBrowserComponent::canSelectFiles
                   | FileBrowserComponent::canSelectDirectories;

        const int block = selected;

        chooser->launchAsync (flags, [this, block] (const FileChooser& fc)
        {
            auto file = fc.getResult();
            if (file != File())
                loadFromFile (block, file);
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

        loadPlugin (block, *found.getFirst(), MemoryBlock());
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
    }

    void loadPlugin (int block, const PluginDescription& description, const MemoryBlock& state)
    {
        setLoading (block, true);   // σημάδι ασφαλείας: αν κρασάρει, δεν ξαναφορτώνεται μόνο του

        double sampleRate = engine.getSampleRate();
        int bufferSize = 512;
        if (auto* device = deviceManager.getCurrentAudioDevice())
            bufferSize = device->getCurrentBufferSizeSamples();

        SafePointer<MainComponent> safeThis (this);

        formatManager.createPluginInstanceAsync (description, sampleRate, bufferSize,
            [safeThis, block, description, state] (std::unique_ptr<AudioPluginInstance> instance, const String& error)
            {
                if (safeThis != nullptr)
                    safeThis->pluginLoaded (block, std::move (instance), error, description, state);
            });
    }

    void pluginLoaded (int block, std::unique_ptr<AudioPluginInstance> instance, const String& error,
                       const PluginDescription& description, const MemoryBlock& state)
    {
        setLoading (block, false);

        if (instance == nullptr)
        {
            setMessage (gr ("Αποτυχία φόρτωσης: ") + error);
            return;
        }

        if (editorBlock == block)
            closeEditor();

        descriptions[(size_t) block] = description;
        auto old = engine.swapPlugin (block, std::move (instance));
        old.reset();

        if (state.getSize() > 0)
            if (auto* p = engine.getPlugin (block))
                p->setStateInformation (state.getData(), (int) state.getSize());

        updateEditor();
        refreshAll();
    }

    void removePlugin (int block)
    {
        if (editorBlock == block)
            closeEditor();

        auto old = engine.swapPlugin (block, nullptr);
        old.reset();
        descriptions[(size_t) block] = PluginDescription();
        updateEditor();
        refreshAll();
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

    //==========================================================================
    void saveSettings()
    {
        auto* settings = properties.getUserSettings();

        std::unique_ptr<XmlElement> audioState (deviceManager.createStateXml());
        settings->setValue ("audioDeviceState", audioState.get());
        settings->setValue ("selectedBlock", selected);
        settings->setValue ("showEditor", showEditor);

        for (int b = 0; b < flow::numBlocks; ++b)
        {
            const String n (b);
            settings->setValue ("lo" + n, engine.lo[(size_t) b].load());
            settings->setValue ("hi" + n, engine.hi[(size_t) b].load());
            settings->setValue ("hold" + n, engine.holdMode[(size_t) b].load());
            settings->setValue ("arp" + n, engine.arpMode[(size_t) b].load());

            if (auto* p = engine.getPlugin (b))
            {
                MemoryBlock state;
                p->getStateInformation (state);
                settings->setValue ("state" + n, state.toBase64Encoding());
                settings->setValue ("desc" + n, descriptions[(size_t) b].createXml().get());
            }
            else
            {
                settings->removeValue ("state" + n);
                settings->removeValue ("desc" + n);
            }
        }

        for (int i = 0; i < flow::numControls; ++i)
            settings->setValue ("pmap" + String (i), engine.mapCode[(size_t) i].load());

        properties.saveIfNeeded();
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
            if (crashed.contains (String (b)))
            {
                setMessage (gr ("Ένα plugin δεν φορτώθηκε σωστά την τελευταία φορά και παραλείφθηκε."));
                continue;
            }

            std::unique_ptr<XmlElement> xml (settings->getXmlValue ("desc" + String (b)));
            if (xml == nullptr) continue;

            PluginDescription description;
            if (! description.loadFromXml (*xml)) continue;

            MemoryBlock state;
            state.fromBase64Encoding (settings->getValue ("state" + String (b)));

            loadPlugin (b, description, state);
        }
    }

    //==========================================================================
    static const char* controlName (int i)
    {
        static const char* const names[flow::numControls] = { "Block", "Learn", "VST", "Window", "Hold", "Arp", "Rec / Play / Overdub", "Undo", "Stop" };
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

        menu.showMenuAsync (PopupMenu::Options().withTargetComponent (tileAt (i)),
                            [safe = SafePointer<MainComponent> (this), i] (int result)
        {
            if (safe == nullptr) return;
            if (result == 1) safe->startPedalLearn (i);
            if (result == 2) safe->engine.mapCode[(size_t) i] = 0;
        });
    }

    void startPedalLearn (int i)
    {
        engine.cancelLearn();
        engine.mapLearnTarget = i;
        setMessage (gr ("Πάτησε το πετάλι για: ") + controlName (i));
    }

    void setMessage (const String& m) { message = m; messageTicks = 300; }

    void refreshAll()
    {
        const int sel = selected;
        const bool hasPlugin = engine.getPlugin (sel) != nullptr;

        blockTile.setIndex (sel);

        learnTile.setIconColour (engine.learnBlock.load() == sel ? hobby::kCoral : hobby::kIconGrey);

        vstTile.setIconColour (hasPlugin ? hobby::kOn : hobby::kIconGrey);
        const String name = hasPlugin ? engine.getPlugin (sel)->getName() : String ("VST");
        if (name != lastVstName)
        {
            lastVstName = name;
            vstTile.setValues ({ name }, false);
        }

        windowTile.setIconColour (showEditor && hasPlugin ? hobby::kOn : hobby::kIconGrey);

        const int hold = engine.holdMode[(size_t) sel].load();
        holdTile.setIndex (hold);
        holdTile.setIconColour (hold == 0 ? hobby::kIconGrey : hobby::kOn);

        const int arp = engine.arpMode[(size_t) sel].load();
        arpTile.setIndex (arp);
        arpTile.setIconColour (arp == 0 ? hobby::kIconGrey : hobby::kOn);

        const int loop = engine.loopStateOut[(size_t) sel].load();
        const int layers = engine.loopLayersOut[(size_t) sel].load();
        const int mute = engine.loopMuteOut[(size_t) sel].load();
        const int taps = engine.loopTapsOut[(size_t) sel].load();
        const double t = Time::getMillisecondCounterHiRes() * 0.001;

        String recLabel = "Rec";
        Symbol recSymbol = Symbol::PadRecord;
        Colour recColour = hobby::kIconGrey;
        if (loop == flow::loopRec)       { recSymbol = Symbol::Play;       recColour = hobby::mix (hobby::kIconGrey, hobby::kRed,   hobby::pulse (t * 1.5)); }
        else if (loop == flow::loopPlay) { recSymbol = Symbol::PadOverdub; recColour = hobby::kOn; recLabel = "Over"; }
        else if (loop == flow::loopOver) { recSymbol = Symbol::Play;       recColour = hobby::mix (hobby::kIconGrey, hobby::kPeach, hobby::pulse (t * 1.5)); recLabel = "Over"; }
        recTile.setSymbol (recSymbol);
        recTile.setIconColour (recColour);
        if (recLabel != lastRecLabel) { lastRecLabel = recLabel; recTile.setValues ({ recLabel }, false); }

        undoTile.setIconColour (loop != flow::loopOff ? hobby::kBlue : hobby::kIconGrey);

        const String stopLabel = taps > 0 ? "x" + String (taps) : String ("Stop");
        if (stopLabel != lastStopLabel) { lastStopLabel = stopLabel; stopTile.setValues ({ stopLabel }, false); }
        stopTile.setIconColour (taps > 0 ? hobby::kLilac
                                         : mute != 0 ? hobby::mix (hobby::kIconGrey, hobby::kLilac, 0.4f + 0.6f * hobby::pulse (t * 0.5))
                                                     : hobby::kIconGrey);
        juce::ignoreUnused (layers);

        loopsView.repaint();
        keyboardView.repaint();
    }

    void timerCallback() override
    {
        if (messageTicks > 0 && --messageTicks == 0)
        {
            message.clear();
            engine.mapLearnTarget = -1;
        }

        // πετάλια: κάθε πάτημα κάνει ό,τι και το αντίστοιχο κουμπί
        for (int i = 0; i < flow::numControls; ++i)
        {
            const int hits = engine.mapHits[(size_t) i].load();
            while (lastHits[(size_t) i] != hits)
            {
                ++lastHits[(size_t) i];
                if (auto* tile = tileAt (i); tile->onClick) tile->onClick();
            }
        }

        const int learned = engine.learnedCount.load();
        if (learned != lastLearned)
        {
            lastLearned = learned;
            const int target = engine.learnedTarget.load();
            if (target >= 0 && target < flow::numControls)
                setMessage (String (controlName (target)) + " " + String::fromUTF8 ("\xe2\x86\x90") + " " + pedalCodeText (engine.learnedCode.load()));
        }

        refreshAll();

        // αν το block με το ανοιχτό plugin άλλαξε plugin από αλλού
        if (showEditor && editor == nullptr && engine.getPlugin (selected) != nullptr)
            updateEditor();
    }

    //==========================================================================
    flow::Engine engine;
    ApplicationProperties properties;
    AudioDeviceManager deviceManager;
    AudioPluginFormatManager formatManager;

    Typeface::Ptr typeface;
    int selected = 0;
    bool showEditor = false;
    bool layouting = false;
    int winW = 992, winH = 700;
    float editorScale = 1.0f;
    Rectangle<int> loopsBezel, pluginBezel, stripBezel, keysBezel;
    String message, lastVstName, lastRecLabel, lastStopLabel;
    std::array<int, flow::numControls> lastHits {};
    int lastLearned = 0;
    int messageTicks = 0;

    std::array<PluginDescription, flow::numBlocks> descriptions;
    std::unique_ptr<AudioProcessorEditor> editor;
    int editorBlock = -1;
    std::unique_ptr<FileChooser> chooser;

    LoopsView loopsView;
    KeyboardView keyboardView;
    ValueTile blockTile, learnTile, vstTile, windowTile, holdTile, arpTile, recTile, undoTile, stopTile;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

//==============================================================================
class FlowApplication : public JUCEApplication
{
public:
    const String getApplicationName() override    { return "Flow"; }
    const String getApplicationVersion() override { return "0.2.0"; }
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
