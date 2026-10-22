// Flow - master keyboard host για VST3 / AU plugins.
// Prisma Audio

#include "Views.h"
#include <map>

namespace
{
    enum PanelMode { panelInstrument = 0, panelMidiFx, panelFx, panelMaster };
    constexpr int maxFxPerChain = 8;
    using PluginPtr = flow::PluginPtr;
}

//==============================================================================
class MainComponent : public Component,
                      private Timer,
                      private ComponentListener
{
public:
    MainComponent()
        : typeface (hobby::loadTypeface()),
          loopsView (engine, selected, typeface),
          keyboardView (engine, selected, message, typeface),
          header (typeface),
          blockTile (Symbol::Block, typeface),
          vstTile (Symbol::Vst, typeface),
          mfxTile (Symbol::Thru, typeface),
          fxTile (Symbol::Fx, typeface),
          holdTile (Symbol::Hold, typeface),
          chordTile (Symbol::Chord, typeface),
          recTile (Symbol::PadRecord, typeface),
          undoTile (Symbol::Undo, typeface),
          stopTile (Symbol::Stop, typeface),
          tapeTile (Symbol::Stop, typeface),
          metroTile (Symbol::Tempo, typeface),
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

        setWantsKeyboardFocus (true);

        for (auto* t : tiles())
            addAndMakeVisible (t);
        for (auto* t : { &recTile, &undoTile, &stopTile, &tapeTile })
            t->setRound (true);

        blockTile.setTooltip (gr ("Block: φορτώνει νέο plugin σε νέο block"));
        vstTile.setTooltip (gr ("VST: ανοίγει / κλείνει το παράθυρο του instrument (πράσινο = το block έχει VST). Δεξί κλικ: αντικατάσταση, αφαίρεση"));
        mfxTile.setTooltip (gr ("MIDI FX: MIDI effect (π.χ. Stepic) πριν από το instrument. Αν δεν έχει plugin, ανοίγει επιλογή αρχείου. Δεξί κλικ: αντικατάσταση, αφαίρεση"));
        fxTile.setTooltip (gr ("FX: αλυσίδα audio effects μετά το instrument. Τα κυκλάκια 1, 2, 3 και το + είναι στην κεφαλίδα του πάνελ. Δεξί κλικ: προσθήκη"));
        folderTile.setTooltip (gr ("Φάκελος αποθήκευσης των Rec Audio (zip με master και stems)"));
        recTile.setTooltip ("Rec MIDI loop / Play / Overdub: " + gr ("ηχογράφηση, αναπαραγωγή, overdub του MIDI loop. Δεξί κλικ: quantize και πετάλι"));
        undoTile.setTooltip ("Undo: " + gr ("αφαιρεί το τελευταίο στρώμα του MIDI loop"));
        stopTile.setTooltip ("Stop: " + gr ("πάτα N φορές γρήγορα για σίγαση ή επαναφορά του στρώματος N"));
        tapeTile.setTooltip ("Rec Audio: " + gr ("σταμάτημα ή νέο take (master και stems ανά block, σε zip)"));
        metroTile.setTooltip (gr ("Metronome (πλήκτρο M). Ακούγεται μονοφωνικά σε ξεχωριστή έξοδο. Δεξί κλικ: ήχος, ένταση, έξοδος, πλήκτρο/πετάλι"));

        addAndMakeVisible (loopsView);
        addAndMakeVisible (keyboardView);
        addAndMakeVisible (header);

        blockTile.onClick = [this] { chooseAndLoadPlugin(); };
        vstTile.onClick = [this] { onVstClick(); };
        vstTile.onRightClick = [this] { showVstMenu(); };
        mfxTile.onClick = [this] { onMidiFxClick(); };
        mfxTile.onRightClick = [this] { showMidiFxMenu(); };
        fxTile.onClick = [this] { onFxClick(); };
        fxTile.onRightClick = [this] { showFxMenu(); };

        holdTile.onClick = [this] { cycleHold (selected); };
        chordTile.onClick = [this] { cycleChordMode (selected); };
        recTile.onClick  = [this] { if (isActive (selected)) engine.pressMain (selected); };
        undoTile.onClick = [this] { if (isActive (selected)) engine.pressUndo (selected); };
        stopTile.onClick = [this] { if (isActive (selected)) engine.pressStop (selected); };
        tapeTile.onClick = [this] { engine.recorder.pressButton(); };
        metroTile.onClick = [this] { toggleMetronome(); };
        folderTile.onClick = [this] { chooseTapeFolder(); };

        // δεξί κλικ: ανάθεση σε πετάλι / ρυθμίσεις
        holdTile.onRightClick = [this] { showPedalMenu (flow::ctlHold); };
        recTile.onRightClick  = [this] { showRecMenu(); };
        undoTile.onRightClick = [this] { showPedalMenu (flow::ctlUndo); };
        stopTile.onRightClick = [this] { showPedalMenu (flow::ctlStop); };
        tapeTile.onRightClick = [this] { showPedalMenu (flow::ctlTape); };
        metroTile.onRightClick = [this] { showMetroMenu(); };

        loopsView.onSelect = [this] (int b) { selectBlock (b); };
        loopsView.getTempoBox().onChange = [this] { dirty = true; };
        keyboardView.onSelect = [this] (int b) { selectBlock (b); };
        keyboardView.onRedoZone = [this] (int b) { showZoneMenu (b); };
        keyboardView.onHold = [this] (int b) { selectBlock (b); cycleHold (b); };
        keyboardView.onRemoveBlock = [this] (int b) { removeBlock (b); };
        keyboardView.onToggleOff = [this] (int b) { toggleBlockOff (b); };
        keyboardView.onChordType = [this] (int b, int dir) { selectBlock (b); stepChordType (b, dir); };
        keyboardView.onCurve = [this] (int b)
        {
            engine.curveMode[(size_t) b] = (engine.curveMode[(size_t) b].load() + 1) % flow::numCurves;
            saveNow();
            refreshAll();
        };
        keyboardView.onTranspose = [this] (int b, int dir)
        {
            auto& t = engine.transpose[(size_t) b];
            t = dir == 0 ? 0 : jlimit (-48, 48, t.load() + dir * 12);   // ανά οκτάβα, ±4
            saveNow();
            refreshAll();
        };
        keyboardView.onVolume = [this] (int b, float gainValue)
        {
            engine.volPos[(size_t) b] = flow::curveInverse (engine.curveMode[(size_t) b].load(), gainValue);
            dirty = true;
        };
        keyboardView.onVolumeMenu = [this] (int b) { showVolumeMenu (b); };

        header.onCircle = [this] (int i)
        {
            const bool same = (panelMode == panelFx || panelMode == panelMaster) && panelIndex == i;
            if (panelMode != panelMaster) panelMode = panelFx;
            panelIndex = i;
            updateEditor();
            toggleWindowForPanel (same);
        };
        header.onCircleMenu = [this] (int i) { showCircleMenu (i); };
        header.onPlus = [this] { addFxTo (panelMode == panelMaster ? -1 : selected); };
        header.onMaster = [this]
        {
            const bool toMaster = panelMode != panelMaster;
            panelMode = toMaster ? panelMaster : panelInstrument;
            panelIndex = 0;
            updateEditor();
            if (toMaster) toggleWindowForPanel (false);
            refreshAll();
        };

        auto* settings = properties.getUserSettings();

        for (int i = 0; i < flow::numControls; ++i)
            engine.mapCode[(size_t) i] = settings->getIntValue ("ctl" + String (i), 0);

        const String savedFolder = settings->getValue ("tapeFolder");
        engine.recorder.setFolder (savedFolder.isNotEmpty() ? File (savedFolder)
                                                            : File::getSpecialLocation (File::userMusicDirectory).getChildFile ("Flow"));

        engine.metroLevel = (float) jlimit (0.0, 1.0, settings->getDoubleValue ("metroLevel", 0.8));
        engine.metroChannel = jmax (0, settings->getIntValue ("metroChannel", 2));
        metroSamplePath = settings->getValue ("metroSample");

        std::unique_ptr<XmlElement> savedAudio (settings->getXmlValue ("audioDeviceState"));
        deviceManager.initialise (0, 16, savedAudio.get(), true);

        for (auto& device : MidiInput::getAvailableDevices())
        {
            deviceManager.setMidiInputDeviceEnabled (device.identifier, true);
            knownMidi.add (device.identifier);
        }

        deviceManager.addAudioCallback (&engine);
        deviceManager.addMidiInputDeviceCallback (String(), &engine.incoming);

        if (metroSamplePath.isNotEmpty()) loadMetroSample (File (metroSamplePath), false);
        if (settings->getBoolValue ("link", false) && LinkBridge::isAvailable()) engine.setLinkEnabled (true);

        refreshSize();
        restoreSession();
        refreshAll();
        lastChange = engine.changeCount.load();
        startTimerHz (30);
    }

    ~MainComponent() override
    {
        stopTimer();
        if (pendingLoads == 0) saveSettings (true);
        deviceManager.removeMidiInputDeviceCallback (String(), &engine.incoming);
        deviceManager.removeAudioCallback (&engine);
        closeEditor();
        for (int b = 0; b < flow::numBlocks; ++b)
        {
            auto extras = engine.takeExtras (b);
            extras.clear();
            engine.swapPlugin (b, nullptr).reset();
        }
        while (engine.numFx (-1) > 0)
            engine.removeFx (-1, 0).reset();
    }

    //==========================================================================
    void paint (Graphics& g) override
    {
        hobby::drawBody (g, getLocalBounds());
        hobby::drawBrandMark (g, typeface, getWidth());

        hobby::drawBezel (g, loopsBezel.toFloat(), loopsBezel.reduced (5).toFloat(), hobby::kScreen, 18.0f);
        hobby::drawBezel (g, pluginBezel.toFloat(), pluginBezel.reduced (5).toFloat(), hobby::kScreen, 14.0f);
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

    void resized() override
    {
        if (getWidth() <= 0 || getHeight() <= 0) return;
        layoutFor (getWidth(), getHeight());
        placeChildren();
    }

    void mouseDown (const MouseEvent&) override { grabKeyboardFocus(); }

    void mouseUp (const MouseEvent& e) override
    {
        // κλικ στο "π / -": μενού (ρυθμίσεις ήχου, set, panic, Link)
        if (e.y < 30 && std::abs (e.x - getWidth() / 2) < 40 && ! e.mouseWasDraggedSinceMouseDown())
            showPiMenu();
    }

    bool keyPressed (const KeyPress& k) override
    {
        if (k.getModifiers().isAnyModifierKeyDown()) return false;
        const auto c = k.getTextCharacter();
        if (c == 'm' || c == 'M') { toggleMetronome(); return true; }
        if (k == KeyPress::escapeKey) { doPanic(); return true; }
        return false;
    }

private:
    //==========================================================================
    std::array<ValueTile*, 11> tiles()
    {
        return { &blockTile, &vstTile, &mfxTile, &fxTile, &chordTile, &recTile, &undoTile, &stopTile, &tapeTile, &metroTile, &folderTile };
    }

    bool isActive (int b) const { return b >= 0 && b < flow::numBlocks && engine.active[(size_t) b].load() != 0; }

    int currentBufferSize() const
    {
        if (auto* device = deviceManager.getCurrentAudioDevice())
            return device->getCurrentBufferSizeSamples();
        return 512;
    }

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
            if (isActive (b))
                return b;
        return -1;
    }

    //==========================================================================
    // σκηνές A-D: ποια blocks παίζουν και, ανά block, ένταση, hold, chord, transpose, καμπύλη. Plugins, ζώνες και loops δεν αλλάζουν.
    struct SceneBlock { int off = 0, hold = 0, cmode = 0, ctype = 0, cminor = 0, ctonic = -1, tr = 0, curve = 0; float vol = 1.0f; };
    struct Scene { bool saved = false; std::array<SceneBlock, flow::numBlocks> b; };
    std::array<Scene, 4> scenes;
    int currentScene = 0;

    void captureScene (int i)
    {
        auto& sc = scenes[(size_t) i];
        for (int b = 0; b < flow::numBlocks; ++b)
        {
            auto& x = sc.b[(size_t) b];
            x.off    = engine.blockOff[(size_t) b].load();
            x.hold   = engine.holdMode[(size_t) b].load();
            x.cmode  = engine.chordMode[(size_t) b].load();
            x.ctype  = engine.chordType[(size_t) b].load();
            x.cminor = engine.chordMinor[(size_t) b].load();
            x.ctonic = engine.chordTonic[(size_t) b].load();
            x.tr     = engine.transpose[(size_t) b].load();
            x.curve  = engine.curveMode[(size_t) b].load();
            x.vol    = engine.volPos[(size_t) b].load();
        }
        sc.saved = true;
    }

    void recallScene (int i)
    {
        const auto& sc = scenes[(size_t) i];
        for (int b = 0; b < flow::numBlocks; ++b)
        {
            if (! isActive (b)) continue;   // σκηνή σε block που δεν υπάρχει πια: αγνοείται
            const auto& x = sc.b[(size_t) b];
            engine.blockOff[(size_t) b]  = x.off;
            engine.holdMode[(size_t) b]  = x.hold;
            engine.chordMode[(size_t) b] = x.cmode;
            engine.chordType[(size_t) b] = x.ctype;
            engine.chordMinor[(size_t) b] = x.cminor;
            engine.chordTonic[(size_t) b] = x.ctonic;
            engine.transpose[(size_t) b] = x.tr;
            engine.curveMode[(size_t) b] = x.curve;
            engine.volPos[(size_t) b]    = x.vol;
        }
    }

    // η σκηνή που φεύγεις θυμάται την τελευταία της κατάσταση. Άδεια σκηνή: αντιγράφει την τωρινή και ανοίγει.
    void gotoScene (int i)
    {
        if (i < 0 || i > 3 || i == currentScene) return;
        captureScene (currentScene);
        if (scenes[(size_t) i].saved) recallScene (i);
        else                          captureScene (i);
        currentScene = i;
        ++engine.changeCount;
        saveNow();
        refreshAll();
    }

    void stepScene (int dir)
    {
        int n = currentScene;
        for (int k = 0; k < 4; ++k)
        {
            n = (n + dir + 4) % 4;
            if (scenes[(size_t) n].saved) { gotoScene (n); return; }
        }
    }

    void toggleBlockOff (int b)
    {
        if (! isActive (b)) return;
        engine.blockOff[(size_t) b] = engine.blockOff[(size_t) b].load() != 0 ? 0 : 1;
        saveNow();
        refreshAll();
    }

    void showSceneMenu()
    {
        PopupMenu menu;
        menu.addItem (1, gr ("Επόμενη σκηνή: ανάθεση πεταλιού"));
        menu.addItem (2, gr ("Προηγούμενη σκηνή: ανάθεση πεταλιού"));
        menu.addItem (3, gr ("Αφαίρεση αναθέσεων"), engine.mapCode[flow::ctlSceneNext].load() != 0 || engine.mapCode[flow::ctlScenePrev].load() != 0);
        menu.showMenuAsync (PopupMenu::Options().withTargetComponent (&keyboardView),
                            [safe = SafePointer<MainComponent> (this)] (int r)
        {
            if (safe == nullptr) return;
            if (r == 1) safe->startPedalLearn (flow::ctlSceneNext);
            if (r == 2) safe->startPedalLearn (flow::ctlScenePrev);
            if (r == 3) { safe->engine.mapCode[flow::ctlSceneNext] = 0; safe->engine.mapCode[flow::ctlScenePrev] = 0; safe->saveNow(); }
        });
    }

    void cycleChordMode (int b)
    {
        if (! isActive (b)) return;
        const int m = (engine.chordMode[(size_t) b].load() + 1) % 3;
        engine.chordMode[(size_t) b] = m;
        engine.chordTonic[(size_t) b] = -1;   // Scale: το πρώτο πλήκτρο που θα πατηθεί γίνεται η τονική
        saveNow();
        refreshAll();
    }

    // κύκλος πάνω από το block: Chord = επόμενος/προηγούμενος τύπος (dir 0 = maj). Scale = major / minor (dir 0 = νέα τονική)
    void stepChordType (int b, int dir)
    {
        if (! isActive (b)) return;
        const int mode = engine.chordMode[(size_t) b].load();
        if (mode == 1)
        {
            const int n = flow::numChordTypes;
            const int cur = engine.chordType[(size_t) b].load();
            engine.chordType[(size_t) b] = dir == 0 ? 0 : (cur + dir + n) % n;
        }
        else if (mode == 2)
        {
            if (dir == 0) engine.chordTonic[(size_t) b] = -1;
            else engine.chordMinor[(size_t) b] = engine.chordMinor[(size_t) b].load() != 0 ? 0 : 1;
        }
        saveNow();
        refreshAll();
    }

    void cycleHold (int b)
    {
        if (! isActive (b)) return;
        engine.holdMode[(size_t) b] = (engine.holdMode[(size_t) b].load() + 1) % 5;
        saveNow();
        refreshAll();
    }

    //==========================================================================
    // πάνελ plugin: ποιο plugin δείχνει ο editor
    juce::AudioPluginInstance* panelPlugin()
    {
        if (panelMode == panelMaster)
            return engine.getFx (-1, panelIndex);
        if (! isActive (selected)) return nullptr;
        switch (panelMode)
        {
            case panelMidiFx: return engine.getMidiFx (selected);
            case panelFx:     return engine.getFx (selected, panelIndex);
            default:          return engine.getPlugin (selected);
        }
    }

    void normalisePanel()
    {
        if (panelMode == panelMaster)
        {
            const int n = engine.numFx (-1);
            panelIndex = n > 0 ? jlimit (0, n - 1, panelIndex) : 0;
            return;
        }
        if (! isActive (selected)) { panelMode = panelInstrument; return; }
        if (panelMode == panelMidiFx && engine.getMidiFx (selected) == nullptr) panelMode = panelInstrument;
        if (panelMode == panelFx)
        {
            const int n = engine.numFx (selected);
            if (n == 0) panelMode = panelInstrument;
            else        panelIndex = jlimit (0, n - 1, panelIndex);
        }
    }

    String emptyPanelText() const
    {
        if (panelMode == panelMaster) return gr ("Master FX: πάτα το + για να φορτώσεις effect στο τελικό μίγμα");
        if (! isActive (selected))    return gr ("Πάτα Block για να φορτώσεις το πρώτο plugin");
        return gr ("Αυτό το plugin δεν έχει editor");
    }

    //==========================================================================
    // γεωμετρία παραθύρου: μία στήλη (κεφαλίδα, οθόνη loops, κουμπιά, πλήκτρα). Τα plugin ανοίγουν σε δικά τους παράθυρα.
    static constexpr int topH = 36, botH = 14, gapPx = 8, sideM = 11, headerBarH = 44;
    static constexpr int minLoopsH = 160, stripH = 72, keysH = 120, minContentW = 724, defaultLoopsH = 250;
    static constexpr int minContentH() { return minLoopsH + gapPx + stripH + gapPx + keysH; }
    static constexpr int chromeH() { return topH + headerBarH + gapPx + botH; }
    static constexpr int minWinW() { return minContentW + 2 * sideM; }
    static constexpr int minWinH() { return chromeH() + minContentH(); }
    static constexpr int defaultWinH() { return chromeH() + defaultLoopsH + gapPx + stripH + gapPx + keysH; }

    void layoutFor (int W, int H)
    {
        const int cw = jmax (minContentW, W - 2 * sideM);
        const int contentH = jmax (minContentH(), H - chromeH());
        const int y0 = topH + headerBarH + gapPx;

        pluginBezel = { sideM, topH, cw, headerBarH };   // η κεφαλίδα
        loopsBezel  = { sideM, y0, cw, contentH - (stripH + keysH + 2 * gapPx) };
        stripBezel  = { sideM, y0 + contentH - keysH - gapPx - stripH, cw, stripH };
        keysBezel   = { sideM, y0 + contentH - keysH, cw, keysH };
    }

    void placeChildren()
    {
        loopsView.setBounds (loopsBezel.reduced (5));
        keyboardView.setBounds (keysBezel.reduced (5));

        const auto panel = pluginBezel.reduced (5);
        header.setBounds (panel.getX(), panel.getY(), panel.getWidth(), headerH);

        // τρεις ομάδες: αριστερά VST, MIDI FX, FX, Hold, Chord. Στη μέση τα 4 στρογγυλά. Δεξιά metronome και φάκελος.
        const auto strip = stripBezel.reduced (5);
        const int ty = strip.getY() + 5;
        const int leftX = strip.getX() + 14;
        const int midW = 4 * 52 + 3 * 8;
        const int rightX = strip.getRight() - 14 - 52;
        const int leftEnd = leftX + 5 * 60 - 8, rightStart = rightX - 60;
        const int midX = leftEnd + jmax (4, (rightStart - leftEnd - midW) / 2);

        int x = leftX;
        for (auto* t : { &blockTile, &vstTile, &mfxTile, &fxTile, &chordTile })
        {
            t->setBounds (x, ty, 52, 52);
            x += 60;
        }
        x = midX;
        for (auto* t : { &recTile, &undoTile, &stopTile, &tapeTile })
        {
            t->setBounds (x, ty, 52, 52);
            x += 60;
        }
        metroTile.setBounds (rightX - 60, ty, 52, 52);
        folderTile.setBounds (rightX, ty, 52, 52);
    }

    void refreshSize()
    {
        if (getWidth() <= 0 || getHeight() <= 0)
            setSize (minWinW(), defaultWinH());

        if (auto* w = findParentComponentOfClass<DocumentWindow>())
            w->setResizeLimits (minWinW(), minWinH() + 28, 4000, 3000);

        layoutFor (getWidth(), getHeight());
        placeChildren();
        repaint();
    }

    void parentHierarchyChanged() override { refreshSize(); }

    //==========================================================================
    // κάθε plugin (instrument, MIDI FX, FX, master FX) ανοίγει σε δικό του ελεύθερο παράθυρο, στο δικό του μέγεθος
    struct PluginWindow : public DocumentWindow
    {
        PluginWindow (MainComponent& o, AudioPluginInstance* p, const String& title, AudioProcessorEditor* e, const String& key)
            : DocumentWindow (title, hobby::kBodyBottom, DocumentWindow::closeButton | DocumentWindow::minimiseButton),
              owner (o), plugin (p), posKey (key)
        {
            setUsingNativeTitleBar (true);
            const bool canResize = e->isResizable();
            setContentOwned (e, true);
            setResizable (canResize, false);
        }

        void closeButtonPressed() override { owner.closePluginWindowAsync (plugin); }

        MainComponent& owner;
        AudioPluginInstance* plugin;
        String posKey;
    };

    String windowTitle (AudioPluginInstance* p) const
    {
        const String name = p->getName();
        if (panelMode == panelMaster) return "MASTER FX " + String (panelIndex + 1) + String::fromUTF8 (" \xc2\xb7 ") + name;
        const String blk = "BLOCK " + String (selected + 1) + String::fromUTF8 (" \xc2\xb7 ");
        if (panelMode == panelMidiFx) return blk + String::fromUTF8 ("MIDI FX \xc2\xb7 ") + name;
        if (panelMode == panelFx)     return blk + "FX " + String (panelIndex + 1) + String::fromUTF8 (" \xc2\xb7 ") + name;
        return blk + name;
    }

    bool hasWindow (AudioPluginInstance* p) const { return p != nullptr && pluginWindows.count (p) > 0; }

    void rememberWindowPos (PluginWindow& w)
    {
        properties.getUserSettings()->setValue (w.posKey, String (w.getX()) + " " + String (w.getY()));
    }

    void openWindowFor (AudioPluginInstance* p)
    {
        flow::flowLog ("openWindowFor: " + p->getName());
        AudioProcessorEditor* e = p->hasEditor() ? p->createEditorIfNeeded() : nullptr;
        if (e == nullptr)
            e = new GenericAudioProcessorEditor (*p);

        const String key = "pwpos_" + p->getName();
        auto w = std::make_unique<PluginWindow> (*this, p, windowTitle (p), e, key);

        Point<int> at;
        const String saved = properties.getUserSettings()->getValue (key);
        if (saved.containsChar (' '))
        {
            at = { saved.upToFirstOccurrenceOf (" ", false, false).getIntValue(),
                   saved.fromFirstOccurrenceOf (" ", false, false).getIntValue() };
        }
        else
        {
            const int n = (int) pluginWindows.size();
            if (auto* top = getTopLevelComponent())
                at = { top->getRight() + 12 + 28 * n, top->getY() + 28 * n };
            else
                at = { 40 + 28 * n, 60 + 28 * n };
        }

        const auto total = Desktop::getInstance().getDisplays().getTotalBounds (true);
        at.x = jlimit (total.getX(), jmax (total.getX(), total.getRight() - w->getWidth()), at.x);
        at.y = jlimit (total.getY(), jmax (total.getY(), total.getBottom() - w->getHeight()), at.y);
        w->setTopLeftPosition (at);
        w->setVisible (true);
        w->toFront (true);
        pluginWindows[p] = std::move (w);
    }

    // κουμπί: ανοίγει το παράθυρο του plugin που δείχνει το πάνελ. Αν είναι ήδη ανοιχτό: το κλείνει (closeIfOpen) ή το φέρνει μπροστά.
    void toggleWindowForPanel (bool closeIfOpen)
    {
        if (auto* p = panelPlugin())
        {
            auto it = pluginWindows.find (p);
            if (it == pluginWindows.end())
            {
                openWindowFor (p);
            }
            else if (closeIfOpen)
            {
                rememberWindowPos (*it->second);
                pluginWindows.erase (it);
            }
            else
            {
                it->second->toFront (true);
            }
        }
        refreshAll();
    }

    void closePluginWindowAsync (AudioPluginInstance* p)
    {
        MessageManager::callAsync ([safe = SafePointer<MainComponent> (this), p]
        {
            if (safe == nullptr) return;
            auto it = safe->pluginWindows.find (p);
            if (it != safe->pluginWindows.end())
            {
                safe->rememberWindowPos (*it->second);
                safe->pluginWindows.erase (it);
            }
            safe->refreshAll();
        });
    }

    // καλείται πριν αφαιρεθεί ή αντικατασταθεί οποιοδήποτε plugin: κλείνει όλα τα παράθυρα plugin
    void closeEditor()
    {
        for (auto& kv : pluginWindows)
            rememberWindowPos (*kv.second);
        pluginWindows.clear();
    }

    void updateEditor() { normalisePanel(); }

    //==========================================================================
    // κουμπιά VST / MIDI FX / FX
    void onVstClick()
    {
        if (! isActive (selected))
        {
            chooseAndLoadPlugin();
            return;
        }
        const bool same = panelMode == panelInstrument;
        if (panelMode == panelMaster) header.onMaster();   // επιστροφή από το master
        panelMode = panelInstrument;
        updateEditor();
        toggleWindowForPanel (same);
    }

    void onMidiFxClick()
    {
        if (! isActive (selected)) { setMessage (gr ("Φόρτωσε πρώτα ένα instrument (VST)")); return; }
        if (engine.getMidiFx (selected) != nullptr)
        {
            const bool same = panelMode == panelMidiFx;
            panelMode = panelMidiFx;
            updateEditor();
            toggleWindowForPanel (same);
        }
        else
        {
            loadMidiFxInto (selected);
        }
    }

    void onFxClick()
    {
        if (! isActive (selected)) { setMessage (gr ("Φόρτωσε πρώτα ένα instrument (VST)")); return; }
        if (engine.numFx (selected) > 0)
        {
            const bool same = panelMode == panelFx;
            panelMode = panelFx;
            panelIndex = jlimit (0, engine.numFx (selected) - 1, panelIndex);
            updateEditor();
            toggleWindowForPanel (same);
        }
        else
        {
            addFxTo (selected);
        }
    }

    void showVstMenu()
    {
        PopupMenu menu;
        const bool v = isActive (selected);
        menu.addItem (2, v ? gr ("Αντικατάσταση instrument του block ") + String (selected + 1) + "…" : gr ("Αντικατάσταση instrument"), v);
        menu.addItem (3, v ? gr ("Αφαίρεση block ") + String (selected + 1) : gr ("Αφαίρεση block"), v);

        menu.showMenuAsync (PopupMenu::Options().withTargetComponent (&vstTile),
                            [safe = SafePointer<MainComponent> (this)] (int result)
        {
            if (safe == nullptr) return;
            if (result == 2) safe->replaceInstrument();
            if (result == 3) safe->removeBlock (safe->selected);
        });
    }

    void showMidiFxMenu()
    {
        const bool v = isActive (selected);
        const bool has = v && engine.getMidiFx (selected) != nullptr;
        PopupMenu menu;
        menu.addItem (1, has ? gr ("Αντικατάσταση MIDI FX…") : gr ("Φόρτωση MIDI FX…"), v);
        menu.addItem (2, gr ("Αφαίρεση MIDI FX"), has);

        menu.showMenuAsync (PopupMenu::Options().withTargetComponent (&mfxTile),
                            [safe = SafePointer<MainComponent> (this)] (int result)
        {
            if (safe == nullptr) return;
            if (result == 1) safe->loadMidiFxInto (safe->selected);
            if (result == 2) safe->removeMidiFx (safe->selected);
        });
    }

    void showFxMenu()
    {
        const bool v = isActive (selected);
        PopupMenu menu;
        menu.addItem (1, gr ("Προσθήκη FX στο block…"), v && engine.numFx (selected) < maxFxPerChain);
        menu.addItem (2, gr ("Αφαίρεση όλων των FX του block"), v && engine.numFx (selected) > 0);

        menu.showMenuAsync (PopupMenu::Options().withTargetComponent (&fxTile),
                            [safe = SafePointer<MainComponent> (this)] (int result)
        {
            if (safe == nullptr) return;
            if (result == 1) safe->addFxTo (safe->selected);
            if (result == 2) safe->clearFx (safe->selected);
        });
    }

    void showCircleMenu (int i)
    {
        const int target = panelMode == panelMaster ? -1 : selected;
        const int n = engine.numFx (target);
        PopupMenu menu;
        menu.addItem (1, gr ("Αφαίρεση"));
        menu.addItem (2, gr ("Μετακίνηση αριστερά"), i > 0);
        menu.addItem (3, gr ("Μετακίνηση δεξιά"), i < n - 1);

        menu.showMenuAsync (PopupMenu::Options().withTargetComponent (&header),
                            [safe = SafePointer<MainComponent> (this), i, target] (int result)
        {
            if (safe == nullptr) return;
            if (result == 1) safe->removeFxAt (target, i);
            if (result == 2) safe->moveFxAt (target, i, -1);
            if (result == 3) safe->moveFxAt (target, i, +1);
        });
    }

    //==========================================================================
    // φόρτωση plugins
    void pickPlugin (const String& title, std::function<void (const PluginDescription&)> chosen)
    {
        chooser = std::make_unique<FileChooser> (title, File ("/Library/Audio/Plug-Ins"), "*.vst3;*.component");

        auto flags = FileBrowserComponent::openMode
                   | FileBrowserComponent::canSelectFiles
                   | FileBrowserComponent::canSelectDirectories;

        chooser->launchAsync (flags, [this, chosen] (const FileChooser& fc)
        {
            auto file = fc.getResult();
            if (file == File()) return;

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
            chosen (*found.getFirst());
        });
    }

    int freeSlot() const
    {
        for (int b = 0; b < flow::numBlocks; ++b)
            if (! isActive (b) && ! loadingSlots.contains (b))
                return b;
        return -1;
    }

    void chooseAndLoadPlugin()
    {
        const int slot = freeSlot();
        if (slot < 0)
        {
            setMessage (gr ("Δεν υπάρχει ελεύθερο block (μέγιστο 16)."));
            return;
        }
        pickPlugin (gr ("Διάλεξε plugin (.vst3 ή .component)"), [this, slot] (const PluginDescription& d)
        {
            loadPlugin (slot, d, MemoryBlock(), true, true, nullptr);
        });
    }

    void replaceInstrument()
    {
        if (! isActive (selected)) return;
        const int slot = selected;
        pickPlugin (gr ("Νέο instrument για το block ") + String (slot + 1), [this, slot] (const PluginDescription& d)
        {
            loadPlugin (slot, d, MemoryBlock(), false, false, nullptr);
        });
    }

    void setLoading (const String& token, bool loading)
    {
        auto* settings = properties.getUserSettings();
        StringArray list;
        list.addTokens (settings->getValue ("loadingTokens"), ",", "");
        list.removeEmptyStrings();
        list.removeString (token);
        if (loading) list.add (token);
        settings->setValue ("loadingTokens", list.joinIntoString (","));
        properties.saveIfNeeded();
    }

    // userLoad: το plugin φορτώθηκε τώρα από τον χρήστη -> νέο block, επιλέγεται και περιμένει ζώνη
    void loadPlugin (int block, const PluginDescription& description, const MemoryBlock& state, bool userLoad, bool captureZone,
                     std::function<void()> after)
    {
        const String token = "b" + String (block);
        setLoading (token, true);   // σημάδι ασφαλείας: αν κρασάρει, δεν ξαναφορτώνεται μόνο του
        loadingSlots.addIfNotAlreadyThere (block);
        ++pendingLoads;

        SafePointer<MainComponent> safeThis (this);

        formatManager.createPluginInstanceAsync (description, engine.getSampleRate(), currentBufferSize(),
            [safeThis, block, token, state, userLoad, captureZone, after] (std::unique_ptr<AudioPluginInstance> instance, const String& error)
            {
                if (safeThis != nullptr)
                    safeThis->pluginLoaded (block, token, std::move (instance), error, state, userLoad, captureZone, after);
            });
    }

    void pluginLoaded (int block, const String& token, std::unique_ptr<AudioPluginInstance> instance, const String& error,
                       const MemoryBlock& state, bool userLoad, bool captureZone, std::function<void()> after)
    {
        setLoading (token, false);
        loadingSlots.removeFirstMatchingValue (block);
        --pendingLoads;

        if (instance == nullptr)
        {
            loadFailed = true;
            setMessage (gr ("Αποτυχία φόρτωσης: ") + error);
            return;
        }

        closeEditor();

        if (userLoad)    // νέο block με καθαρές ρυθμίσεις
        {
            engine.holdMode[(size_t) block] = 0;
            engine.chordMode[(size_t) block] = 0; engine.chordTonic[(size_t) block] = -1; engine.blockOff[(size_t) block] = 0;
            engine.curveMode[(size_t) block] = 0;
            engine.volPos[(size_t) block] = 1.0f;
            engine.volMap[(size_t) block] = 0;
            engine.transpose[(size_t) block] = 0;
        }

        auto old = engine.swapPlugin (block, std::move (instance), captureZone);
        if (old != nullptr) stateCache.erase (old.get());
        old.reset();

        if (state.getSize() > 0)
            if (auto* p = engine.getPlugin (block))
                p->setStateInformation (state.getData(), (int) state.getSize());

        if (userLoad || ! isActive (selected) || block == pendingSelect)
        {
            selected = block;
            engine.selectedBlock = block;
            if (userLoad) panelMode = panelInstrument;
        }
        if (block == pendingSelect) pendingSelect = -1;

        if (after) after();

        updateEditor();
        if (userLoad) toggleWindowForPanel (false);
        refreshAll();
        saveNow (true);
    }

    // aux plugin (MIDI FX / FX): φορτώνεται και παραδίδεται στο done
    void loadAux (const PluginDescription& d, const MemoryBlock& state, const String& token, std::function<void (PluginPtr)> done)
    {
        flow::flowLog ("loadAux: " + d.name + " (" + d.pluginFormatName + ") token=" + token);
        setLoading (token, true);
        ++pendingLoads;
        SafePointer<MainComponent> safe (this);

        formatManager.createPluginInstanceAsync (d, engine.getSampleRate(), currentBufferSize(),
            [safe, token, state, done] (std::unique_ptr<AudioPluginInstance> inst, const String& error)
            {
                if (safe == nullptr) return;
                safe->setLoading (token, false);
                --safe->pendingLoads;
                if (inst == nullptr)
                {
                    safe->loadFailed = true;
                    safe->setMessage (gr ("Αποτυχία φόρτωσης: ") + error);
                    return;
                }
                flow::flowLog ("loadAux: instance created: " + inst->getName());
                if (state.getSize() > 0)
                    inst->setStateInformation (state.getData(), (int) state.getSize());
                flow::flowLog ("loadAux: handing over to engine");
                done (std::move (inst));
            });
    }

    void loadMidiFxInto (int block)
    {
        if (! isActive (block)) return;
        pickPlugin (gr ("Διάλεξε MIDI effect (.vst3 ή .component)"), [this, block] (const PluginDescription& d)
        {
            loadAux (d, MemoryBlock(), "b" + String (block) + "m", [this, block] (PluginPtr p)
            {
                if (! isActive (block)) return;
                flow::flowLog ("loadMidiFxInto: closeEditor");
                closeEditor();
                flow::flowLog ("loadMidiFxInto: setMidiFx");
                auto old = engine.setMidiFx (block, std::move (p));
                flow::flowLog ("loadMidiFxInto: installed, opening window");
                if (old != nullptr) stateCache.erase (old.get());
                old.reset();
                selected = block;
                engine.selectedBlock = block;
                panelMode = panelMidiFx;
                updateEditor();
                toggleWindowForPanel (false);
                refreshAll();
                saveNow (true);
            });
        });
    }

    void removeMidiFx (int block)
    {
        if (! isActive (block) || engine.getMidiFx (block) == nullptr) return;
        closeEditor();
        auto old = engine.setMidiFx (block, nullptr);
        if (old != nullptr) stateCache.erase (old.get());
        old.reset();
        updateEditor();
        refreshAll();
        saveNow (true);
    }

    // target: block 0..15 ή -1 για master
    void addFxTo (int target)
    {
        if (target >= 0 && ! isActive (target)) { setMessage (gr ("Φόρτωσε πρώτα ένα instrument (VST)")); return; }
        if (engine.numFx (target) >= maxFxPerChain) { setMessage (gr ("Μέγιστο 8 effects ανά αλυσίδα")); return; }

        pickPlugin (target < 0 ? gr ("Master FX: διάλεξε effect") : gr ("FX: διάλεξε effect"), [this, target] (const PluginDescription& d)
        {
            loadAux (d, MemoryBlock(), "f" + String (target) + "_" + String (engine.numFx (target)), [this, target] (PluginPtr p)
            {
                if (target >= 0 && ! isActive (target)) return;
                closeEditor();
                engine.addFx (target, std::move (p));
                if (target < 0) panelMode = panelMaster;
                else { panelMode = panelFx; selected = target; engine.selectedBlock = target; }
                panelIndex = engine.numFx (target) - 1;
                updateEditor();
                toggleWindowForPanel (false);
                refreshAll();
                saveNow (true);
            });
        });
    }

    void removeFxAt (int target, int i)
    {
        closeEditor();
        auto old = engine.removeFx (target, i);
        if (old != nullptr) stateCache.erase (old.get());
        old.reset();
        updateEditor();
        refreshAll();
        saveNow (true);
    }

    void clearFx (int target)
    {
        closeEditor();
        while (engine.numFx (target) > 0)
        {
            auto old = engine.removeFx (target, 0);
            if (old != nullptr) stateCache.erase (old.get());
        }
        updateEditor();
        refreshAll();
        saveNow (true);
    }

    void moveFxAt (int target, int i, int dir)
    {
        closeEditor();
        engine.moveFx (target, i, dir);
        panelIndex = jlimit (0, juce::jmax (0, engine.numFx (target) - 1), i + dir);
        updateEditor();
        refreshAll();
        saveNow (true);
    }

    void removeBlock (int block, bool save = true)
    {
        if (! isActive (block)) return;

        closeEditor();

        auto extras = engine.takeExtras (block);
        for (auto& p : extras) stateCache.erase (p.get());
        extras.clear();

        auto old = engine.swapPlugin (block, nullptr);
        if (old != nullptr) stateCache.erase (old.get());
        old.reset();

        engine.holdMode[(size_t) block] = 0;
        engine.chordMode[(size_t) block] = 0; engine.chordTonic[(size_t) block] = -1; engine.blockOff[(size_t) block] = 0;
        engine.curveMode[(size_t) block] = 0;
        engine.volPos[(size_t) block] = 1.0f;
        engine.volMap[(size_t) block] = 0;
        engine.transpose[(size_t) block] = 0;

        if (selected == block)
        {
            selected = firstActive();
            engine.selectedBlock = selected;
        }

        if (save)
        {
            updateEditor();
            refreshAll();
            saveNow();
        }
    }

    void showZoneMenu (int b)
    {
        PopupMenu menu;
        menu.addItem (1, gr ("Ορισμός ζώνης ξανά"));
        menu.addItem (2, gr ("Transpose +1 οκτάβα"));
        menu.addItem (3, gr ("Transpose -1 οκτάβα"));
        menu.addItem (4, gr ("Transpose 0"));
        menu.showMenuAsync (PopupMenu::Options().withTargetComponent (&keyboardView),
                            [safe = SafePointer<MainComponent> (this), b] (int result)
        {
            if (safe == nullptr || result == 0) return;
            if (result == 1)
            {
                safe->selectBlock (b);
                safe->engine.startZone (b);
            }
            else
            {
                auto& t = safe->engine.transpose[(size_t) b];
                t = result == 2 ? jlimit (-48, 48, t.load() + 12) : result == 3 ? jlimit (-48, 48, t.load() - 12) : 0;
                safe->saveNow();
            }
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
    // μενού Rec MIDI loop (quantize), metronome, π / -
    void showRecMenu()
    {
        const int code = engine.mapCode[flow::ctlRec].load();
        const int sq = engine.startQ.load(), lq = engine.lengthQ.load(), nq = engine.snapQ.load();

        PopupMenu startMenu, lenMenu, snapMenu;
        startMenu.addItem (10, "Off", true, sq == 0);
        startMenu.addItem (11, "Beat", true, sq == 1);
        startMenu.addItem (12, "Bar", true, sq == 2);
        lenMenu.addItem (20, "Off", true, lq == 0);
        lenMenu.addItem (21, "Beat", true, lq == 1);
        lenMenu.addItem (22, "Bar", true, lq == 2);
        snapMenu.addItem (30, "Off", true, nq == 0);
        snapMenu.addItem (31, "1/8", true, nq == 1);
        snapMenu.addItem (32, "1/16", true, nq == 2);

        PopupMenu menu;
        menu.addSubMenu (gr ("Start quantize (η αρχή του Rec MIDI loop)"), startMenu);
        menu.addSubMenu (gr ("Length quantize (το μήκος του loop)"), lenMenu);
        menu.addSubMenu (gr ("Snap νοτών"), snapMenu);
        PopupMenu afterMenu;
        afterMenu.addItem (50, "Play", true, engine.afterFirst.load() == 0);
        afterMenu.addItem (51, "Overdub", true, engine.afterFirst.load() == 1);
        menu.addSubMenu (gr ("Μετά το πρώτο loop"), afterMenu);
        menu.addSeparator();
        menu.addItem (40, gr ("Ανάθεση πεταλιού (MIDI learn)"));
        menu.addItem (41, gr ("Αφαίρεση ανάθεσης"), code != 0);
        if (code != 0) menu.addItem (42, gr ("Τώρα: ") + pedalCodeText (code), false);

        menu.showMenuAsync (PopupMenu::Options().withTargetComponent (&recTile),
                            [safe = SafePointer<MainComponent> (this)] (int r)
        {
            if (safe == nullptr || r == 0) return;
            auto& e = safe->engine;
            if (r >= 10 && r <= 12) e.startQ = r - 10;
            if (r >= 20 && r <= 22) e.lengthQ = r - 20;
            if (r >= 30 && r <= 32) e.snapQ = r - 30;
            if (r == 50 || r == 51) e.afterFirst = r - 50;
            if (r == 40) safe->startPedalLearn (flow::ctlRec);
            if (r == 41) e.mapCode[flow::ctlRec] = 0;
            safe->saveNow();
        });
    }

    void showMetroMenu()
    {
        const int code = engine.mapCode[flow::ctlMetro].load();
        const float level = engine.metroLevel.load();
        const int outs = engine.numOutChannels.load();
        const int cur = engine.metroChannel.load();

        PopupMenu levelMenu, outMenu;
        const float levels[4] { 0.25f, 0.5f, 0.75f, 1.0f };
        for (int i = 0; i < 4; ++i)
            levelMenu.addItem (10 + i, String ((int) (levels[i] * 100)) + "%", true, std::abs (level - levels[i]) < 0.13f && (i == 3 || std::abs (level - levels[i]) <= std::abs (level - levels[i + 1])));
        for (int c = 0; c < outs; ++c)
            outMenu.addItem (100 + c, gr ("Έξοδος ") + String (c + 1), true, c == cur);

        PopupMenu menu;
        menu.addItem (1, gr ("Διάλεξε δικό μου ήχο (sample)…"));
        menu.addItem (2, gr ("Προεπιλεγμένος ήχος"), metroSamplePath.isNotEmpty());
        menu.addSubMenu (gr ("Ένταση"), levelMenu);
        menu.addSubMenu (gr ("Έξοδος ακουστικών"), outMenu);
        menu.addSeparator();
        menu.addItem (40, gr ("Ανάθεση πλήκτρου ή πεταλιού (MIDI learn)"));
        menu.addItem (41, gr ("Αφαίρεση ανάθεσης"), code != 0);
        if (code != 0) menu.addItem (42, gr ("Τώρα: ") + pedalCodeText (code), false);

        menu.showMenuAsync (PopupMenu::Options().withTargetComponent (&metroTile),
                            [safe = SafePointer<MainComponent> (this), levels] (int r)
        {
            if (safe == nullptr || r == 0) return;
            auto& e = safe->engine;
            if (r == 1) safe->chooseMetroSample();
            if (r == 2)
            {
                safe->metroSamplePath = {};
                e.setMetroSample (AudioBuffer<float>());
            }
            if (r >= 10 && r <= 13) e.metroLevel = levels[r - 10];
            if (r >= 100) e.metroChannel = r - 100;
            if (r == 40) safe->startPedalLearn (flow::ctlMetro);
            if (r == 41) e.mapCode[flow::ctlMetro] = 0;
            safe->saveNow();
        });
    }

    void showPiMenu()
    {
        const int pcode = engine.mapCode[flow::ctlPanic].load();
        PopupMenu menu;
        menu.addItem (1, gr ("Ρυθμίσεις ήχου και MIDI…"));
        menu.addSeparator();
        menu.addItem (2, gr ("Αποθήκευση set…"));
        menu.addItem (3, gr ("Φόρτωση set…"));
        menu.addItem (9, gr ("Επαναφορά τελευταίας συνεδρίας"), hasSessionBackup());
        menu.addSeparator();
        menu.addItem (4, gr ("Panic: σίγαση κολλημένων νοτών (Esc)"));
        menu.addItem (5, gr ("Panic: ανάθεση πεταλιού ή πλήκτρου (MIDI learn)"));
        menu.addItem (6, gr ("Panic: αφαίρεση ανάθεσης"), pcode != 0);
        if (pcode != 0) menu.addItem (7, gr ("Panic τώρα: ") + pedalCodeText (pcode), false);
        menu.addSeparator();
        menu.addItem (8, "Ableton Link" + (LinkBridge::isAvailable() ? String() : gr (" (δεν περιλαμβάνεται σε αυτό το build)")),
                      LinkBridge::isAvailable(), engine.isLinkEnabled());

        const Rectangle<int> area (getScreenX() + getWidth() / 2 - 40, getScreenY() + 4, 80, 28);
        menu.showMenuAsync (PopupMenu::Options().withTargetScreenArea (area),
                            [safe = SafePointer<MainComponent> (this)] (int r)
        {
            if (safe == nullptr || r == 0) return;
            if (r == 1) safe->openAudioSettings();
            if (r == 2) safe->saveSetToFile();
            if (r == 3) safe->loadSetFromFile();
            if (r == 9) safe->restoreLastSession();
            if (r == 4) safe->doPanic();
            if (r == 5) safe->startPedalLearn (flow::ctlPanic);
            if (r == 6) safe->engine.mapCode[flow::ctlPanic] = 0;
            if (r == 8)
            {
                const bool on = ! safe->engine.isLinkEnabled();
                safe->engine.setLinkEnabled (on);
                safe->setMessage (on ? gr ("Ableton Link: ενεργό") : gr ("Ableton Link: ανενεργό"));
            }
            safe->saveNow();
        });
    }

    void doPanic()
    {
        engine.panic();
        setMessage ("Panic");
    }

    //==========================================================================
    // metronome
    void toggleMetronome()
    {
        if (! engine.metroOn.load() && engine.numOutChannels.load() <= engine.metroChannel.load())
        {
            setMessage (gr ("Metronome: η έξοδος ") + String (engine.metroChannel.load() + 1)
                        + gr (" δεν υπάρχει. Άνοιξε το π / - και ενεργοποίησε περισσότερες εξόδους"));
            return;
        }
        engine.metroOn = ! engine.metroOn.load();
    }

    void chooseMetroSample()
    {
        chooser = std::make_unique<FileChooser> (gr ("Ήχος για το metronome (wav, aiff, mp3...)"),
                                                 File::getSpecialLocation (File::userMusicDirectory), "*.wav;*.aif;*.aiff;*.mp3;*.flac;*.m4a");
        chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles, [this] (const FileChooser& fc)
        {
            auto f = fc.getResult();
            if (f != File()) { loadMetroSample (f, true); saveNow(); }
        });
    }

    void loadMetroSample (const File& f, bool report)
    {
        AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<AudioFormatReader> reader (fm.createReaderFor (f));
        if (reader == nullptr || reader->lengthInSamples <= 0)
        {
            if (report) setMessage (gr ("Δεν μπόρεσα να διαβάσω αυτό το αρχείο ήχου"));
            return;
        }

        const int maxLen = (int) jmin<int64> (reader->lengthInSamples, (int64) (reader->sampleRate * 4.0));
        AudioBuffer<float> raw ((int) reader->numChannels, maxLen);
        reader->read (&raw, 0, maxLen, 0, true, true);

        AudioBuffer<float> mono (1, maxLen);
        mono.clear();
        for (int c = 0; c < raw.getNumChannels(); ++c)
            mono.addFrom (0, 0, raw, c, 0, maxLen, 1.0f / (float) raw.getNumChannels());

        const double ratio = reader->sampleRate / jmax (1.0, engine.getSampleRate());
        const int outLen = jmax (1, (int) std::floor ((double) maxLen / ratio));
        AudioBuffer<float> out (1, outLen);
        for (int i = 0; i < outLen; ++i)
        {
            const double pos = (double) i * ratio;
            const int i0 = jmin (maxLen - 1, (int) pos);
            const int i1 = jmin (maxLen - 1, i0 + 1);
            const float frac = (float) (pos - (double) i0);
            out.setSample (0, i, mono.getSample (0, i0) * (1.0f - frac) + mono.getSample (0, i1) * frac);
        }

        metroSamplePath = f.getFullPathName();
        engine.setMetroSample (std::move (out));
        if (report) setMessage (gr ("Metronome: ") + f.getFileName());
    }

    //==========================================================================
    void openAudioSettings()
    {
        auto* selector = new AudioDeviceSelectorComponent (deviceManager, 0, 0, 0, 16, true, false, false, false);
        selector->setSize (520, 520);

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
        chooser = std::make_unique<FileChooser> (gr ("Φάκελος για τα Rec Audio (zip)"), engine.recorder.getFolder());

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
    // set: όλα τα blocks (plugins με state, ζώνες, hold, καμπύλες, εντάσεις, transpose, MIDI FX, FX, νότες των loops) + master FX
    String stateString (AudioPluginInstance* p, bool fresh)
    {
        auto it = stateCache.find (p);
        if (! fresh && it != stateCache.end()) return it->second;
        MemoryBlock state;
        p->getStateInformation (state);
        const auto s = state.toBase64Encoding();
        stateCache[p] = s;
        return s;
    }

    XmlElement* pluginElement (XmlElement& parent, const String& tag, AudioPluginInstance* p, bool fresh)
    {
        auto* wrapper = parent.createNewChildElement (tag);
        if (auto d = p->getPluginDescription().createXml())
        {
            d->setAttribute ("state", stateString (p, fresh));
            wrapper->addChildElement (d.release());
        }
        return wrapper;
    }

    static String loopToText (const std::vector<flow::LoopEvent>& ev)
    {
        StringArray tokens;
        for (auto& e : ev)
            tokens.add (e.isRaw ? String (e.phase, 6) + ":2:" + String (e.note) + ":" + String (e.vel) + ":" + String (e.d2)
                                : String (e.phase, 6) + ":" + String (e.on ? 1 : 0) + ":" + String (e.note) + ":" + String (e.vel));
        return tokens.joinIntoString (" ");
    }

    static std::vector<flow::LoopEvent> loopFromText (const String& text)
    {
        std::vector<flow::LoopEvent> ev;
        StringArray tokens;
        tokens.addTokens (text, " ", "");
        for (auto& t : tokens)
        {
            StringArray f;
            f.addTokens (t, ":", "");
            if (f.size() == 5 && f[1].getIntValue() == 2)
                ev.push_back ({ f[0].getDoubleValue(), true, jlimit (0, 255, f[2].getIntValue()), jlimit (0, 127, f[3].getIntValue()), true, jlimit (0, 127, f[4].getIntValue()) });
            else if (f.size() == 4)
                ev.push_back ({ f[0].getDoubleValue(), f[1].getIntValue() != 0, jlimit (0, 127, f[2].getIntValue()), jlimit (0, 127, f[3].getIntValue()) });
        }
        return ev;
    }

    std::unique_ptr<XmlElement> createSetXml (bool fresh, bool withLoops)
    {
        auto root = std::make_unique<XmlElement> ("FlowSet");
        root->setAttribute ("version", 1);
        root->setAttribute ("tempo", engine.tempo.load());
        root->setAttribute ("startQ", engine.startQ.load());
        root->setAttribute ("lengthQ", engine.lengthQ.load());
        root->setAttribute ("snapQ", engine.snapQ.load());
        root->setAttribute ("afterFirst", engine.afterFirst.load());
        root->setAttribute ("selected", selected);

        for (int b = 0; b < flow::numBlocks; ++b)
        {
            if (! isActive (b)) continue;
            auto* e = root->createNewChildElement ("Block");
            e->setAttribute ("slot", b);
            e->setAttribute ("lo", engine.lo[(size_t) b].load());
            e->setAttribute ("hi", engine.hi[(size_t) b].load());
            e->setAttribute ("seq", engine.zoneSeq[(size_t) b].load());
            e->setAttribute ("zone", engine.zoneState[(size_t) b].load() == 0 ? 1 : 0);
            e->setAttribute ("hold", engine.holdMode[(size_t) b].load());
            e->setAttribute ("al", engine.getAlText (b));
            e->setAttribute ("curve", engine.curveMode[(size_t) b].load());
            e->setAttribute ("off", engine.blockOff[(size_t) b].load());
            e->setAttribute ("chord", engine.chordMode[(size_t) b].load());
            e->setAttribute ("ctype", engine.chordType[(size_t) b].load());
            e->setAttribute ("cminor", engine.chordMinor[(size_t) b].load());
            e->setAttribute ("ctonic", engine.chordTonic[(size_t) b].load());
            e->setAttribute ("vol", (double) engine.volPos[(size_t) b].load());
            e->setAttribute ("vmap", engine.volMap[(size_t) b].load());
            e->setAttribute ("tr", engine.transpose[(size_t) b].load());

            pluginElement (*e, "Instrument", engine.getPlugin (b), fresh);
            if (auto* m = engine.getMidiFx (b)) pluginElement (*e, "MidiFx", m, fresh);
            for (int k = 0; k < engine.numFx (b); ++k) pluginElement (*e, "Fx", engine.getFx (b, k), fresh);

            flow::LoopSnapshot snap;
            if (withLoops && engine.getLoop (b, snap))
            {
                auto* l = e->createNewChildElement ("Loop");
                l->setAttribute ("beats", snap.beats);
                {
                    StringArray lb;
                    for (auto v : snap.layerBeats) lb.add (String (v, 4));
                    l->setAttribute ("layerBeats", lb.joinIntoString (" "));
                }
                l->setAttribute ("mute", (int) snap.mute);
                for (auto& layer : snap.layers)
                    l->createNewChildElement ("Layer")->addTextElement (loopToText (layer));
            }
        }

        {
            captureScene (currentScene);
            auto* sx = root->createNewChildElement ("Scenes");
            sx->setAttribute ("current", currentScene);
            for (int i = 0; i < 4; ++i)
            {
                if (! scenes[(size_t) i].saved) continue;
                auto* si = sx->createNewChildElement ("Scene");
                si->setAttribute ("idx", i);
                for (int b = 0; b < flow::numBlocks; ++b)
                {
                    const auto& x = scenes[(size_t) i].b[(size_t) b];
                    auto* be = si->createNewChildElement ("B");
                    be->setAttribute ("s", b);
                    be->setAttribute ("off", x.off);
                    be->setAttribute ("hold", x.hold);
                    be->setAttribute ("cm", x.cmode);
                    be->setAttribute ("ct", x.ctype);
                    be->setAttribute ("cn", x.cminor);
                    be->setAttribute ("cto", x.ctonic);
                    be->setAttribute ("tr", x.tr);
                    be->setAttribute ("cv", x.curve);
                    be->setAttribute ("vol", (double) x.vol);
                }
            }
        }

        auto* master = root->createNewChildElement ("Master");
        for (int k = 0; k < engine.numFx (-1); ++k)
            pluginElement (*master, "Fx", engine.getFx (-1, k), fresh);

        return root;
    }

    struct PluginEntry { PluginDescription desc; MemoryBlock state; bool ok = false; };

    static PluginEntry readPluginEntry (const XmlElement* wrapper)
    {
        PluginEntry pe;
        if (wrapper == nullptr) return pe;
        if (auto* d = wrapper->getFirstChildElement())
        {
            pe.ok = pe.desc.loadFromXml (*d);
            pe.state.fromBase64Encoding (d->getStringAttribute ("state"));
        }
        return pe;
    }

    void clearEverything()
    {
        closeEditor();
        for (int b = 0; b < flow::numBlocks; ++b)
            removeBlock (b, false);
        clearFx (-1);
        panelMode = panelInstrument;
        panelIndex = 0;
    }

    void applySet (const XmlElement& root, const StringArray& crashed)
    {
        clearEverything();

        engine.tempo = jlimit (40.0, 240.0, root.getDoubleAttribute ("tempo", 120.0));
        engine.startQ = jlimit (0, 2, root.getIntAttribute ("startQ", 0));
        engine.lengthQ = jlimit (0, 2, root.getIntAttribute ("lengthQ", 0));
        engine.snapQ = jlimit (0, 2, root.getIntAttribute ("snapQ", 0));
        engine.afterFirst = jlimit (0, 1, root.getIntAttribute ("afterFirst", 0));
        pendingSelect = root.getIntAttribute ("selected", 0);

        for (auto& sc : scenes) sc = Scene();
        currentScene = 0;
        if (auto* sx = root.getChildByName ("Scenes"))
        {
            currentScene = jlimit (0, 3, sx->getIntAttribute ("current", 0));
            for (auto* si : sx->getChildWithTagNameIterator ("Scene"))
            {
                const int idx = si->getIntAttribute ("idx", -1);
                if (idx < 0 || idx > 3) continue;
                auto& sc = scenes[(size_t) idx];
                sc.saved = true;
                for (auto* be : si->getChildWithTagNameIterator ("B"))
                {
                    const int bs = be->getIntAttribute ("s", -1);
                    if (bs < 0 || bs >= flow::numBlocks) continue;
                    auto& x = sc.b[(size_t) bs];
                    x.off = jlimit (0, 1, be->getIntAttribute ("off", 0));
                    x.hold = jlimit (0, 4, be->getIntAttribute ("hold", 0));
                    x.cmode = jlimit (0, 2, be->getIntAttribute ("cm", 0));
                    x.ctype = jlimit (0, flow::numChordTypes - 1, be->getIntAttribute ("ct", 0));
                    x.cminor = jlimit (0, 1, be->getIntAttribute ("cn", 0));
                    x.ctonic = jlimit (-1, 11, be->getIntAttribute ("cto", -1));
                    x.tr = jlimit (-48, 48, be->getIntAttribute ("tr", 0));
                    x.curve = jlimit (0, flow::numCurves - 1, be->getIntAttribute ("cv", 0));
                    x.vol = (float) jlimit (0.0, 1.0, be->getDoubleAttribute ("vol", 1.0));
                }
            }
        }

        for (auto* e : root.getChildWithTagNameIterator ("Block"))
        {
            const int slot = e->getIntAttribute ("slot", -1);
            if (slot < 0 || slot >= flow::numBlocks) continue;

            auto inst = readPluginEntry (e->getChildByName ("Instrument"));
            if (! inst.ok) continue;

            if (crashed.contains ("b" + String (slot)))
            {
                setMessage (gr ("Ένα plugin δεν φορτώθηκε σωστά την τελευταία φορά και παραλείφθηκε."));
                continue;
            }

            engine.holdMode[(size_t) slot]  = jlimit (0, 4, e->getIntAttribute ("hold", 0));
            engine.setAlText (slot, e->getStringAttribute ("al"));
            engine.curveMode[(size_t) slot] = jlimit (0, flow::numCurves - 1, e->getIntAttribute ("curve", 0));
            engine.blockOff[(size_t) slot]   = jlimit (0, 1, e->getIntAttribute ("off", 0));
            engine.chordMode[(size_t) slot]  = jlimit (0, 2, e->getIntAttribute ("chord", 0));
            engine.chordType[(size_t) slot]  = jlimit (0, flow::numChordTypes - 1, e->getIntAttribute ("ctype", 0));
            engine.chordMinor[(size_t) slot] = jlimit (0, 1, e->getIntAttribute ("cminor", 0));
            engine.chordTonic[(size_t) slot] = jlimit (-1, 11, e->getIntAttribute ("ctonic", -1));
            engine.volPos[(size_t) slot]    = (float) jlimit (0.0, 1.0, e->getDoubleAttribute ("vol", 1.0));
            engine.volMap[(size_t) slot]    = e->getIntAttribute ("vmap", 0);
            {
                const int trLoaded = jlimit (-48, 48, e->getIntAttribute ("tr", 0));
                engine.transpose[(size_t) slot] = ((trLoaded + (trLoaded >= 0 ? 6 : -6)) / 12) * 12;   // παλιά sets: στρογγυλοποίηση στην πλησιέστερη οκτάβα
            }

            const bool hasZone = e->getIntAttribute ("zone", 0) != 0;
            engine.setZone (slot, e->getIntAttribute ("lo", flow::lowestNote), e->getIntAttribute ("hi", flow::lowestNote),
                            e->getIntAttribute ("seq", 0));

            auto copy = std::make_shared<XmlElement> (*e);
            auto crashedCopy = crashed;
            loadPlugin (slot, inst.desc, inst.state, false, ! hasZone,
                        [this, slot, copy, crashedCopy] { loadExtras (slot, *copy, crashedCopy); });
        }

        if (auto* m = root.getChildByName ("Master"))
        {
            auto items = std::make_shared<std::vector<PluginEntry>>();
            for (auto* f : m->getChildWithTagNameIterator ("Fx"))
            {
                auto pe = readPluginEntry (f);
                if (pe.ok && ! crashed.contains ("f-1_" + String ((int) items->size()))) items->push_back (std::move (pe));
            }
            loadFxSequence (-1, items, 0);
        }
    }

    void loadExtras (int slot, const XmlElement& e, const StringArray& crashed)
    {
        if (! isActive (slot)) return;

        auto mfx = readPluginEntry (e.getChildByName ("MidiFx"));
        if (mfx.ok && ! crashed.contains ("b" + String (slot) + "m"))
        {
            loadAux (mfx.desc, mfx.state, "b" + String (slot) + "m", [this, slot] (PluginPtr p)
            {
                if (! isActive (slot)) return;
                closeEditor();
                engine.setMidiFx (slot, std::move (p)).reset();
                updateEditor();
                refreshAll();
                saveNow (true);
            });
        }

        auto items = std::make_shared<std::vector<PluginEntry>>();
        for (auto* f : e.getChildWithTagNameIterator ("Fx"))
        {
            auto pe = readPluginEntry (f);
            if (pe.ok && ! crashed.contains ("f" + String (slot) + "_" + String ((int) items->size()))) items->push_back (std::move (pe));
        }
        loadFxSequence (slot, items, 0);

        if (auto* l = e.getChildByName ("Loop"))
        {
            flow::LoopSnapshot snap;
            snap.beats = l->getDoubleAttribute ("beats", 0.0);
            snap.mute = (unsigned) l->getIntAttribute ("mute", 0);
            for (auto* layer : l->getChildWithTagNameIterator ("Layer"))
                snap.layers.push_back (loopFromText (layer->getAllSubText()));
            {
                StringArray lb;
                lb.addTokens (l->getStringAttribute ("layerBeats"), " ", "");
                lb.removeEmptyStrings();
                for (auto& s : lb) snap.layerBeats.push_back (s.getDoubleValue());
            }
            if (snap.beats > 0.0 && ! snap.layers.empty())
                engine.setLoop (slot, snap);
        }
    }

    void loadFxSequence (int target, std::shared_ptr<std::vector<PluginEntry>> items, size_t i)
    {
        if (i >= items->size()) return;
        if (target >= 0 && ! isActive (target)) return;

        auto& pe = (*items)[i];
        loadAux (pe.desc, pe.state, "f" + String (target) + "_" + String ((int) i), [this, target, items, i] (PluginPtr p)
        {
            if (target >= 0 && ! isActive (target)) return;
            closeEditor();
            engine.addFx (target, std::move (p));
            updateEditor();
            refreshAll();
            saveNow (true);
            loadFxSequence (target, items, i + 1);
        });
    }

    void saveSetToFile()
    {
        auto dir = File::getSpecialLocation (File::userMusicDirectory).getChildFile ("Flow").getChildFile ("Sets");
        dir.createDirectory();
        chooser = std::make_unique<FileChooser> (gr ("Αποθήκευση set"), dir.getChildFile ("Set.flowset"), "*.flowset");
        chooser->launchAsync (FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles | FileBrowserComponent::warnAboutOverwriting,
                              [this] (const FileChooser& fc)
        {
            auto f = fc.getResult();
            if (f == File()) return;
            f = f.withFileExtension ("flowset");
            auto xml = createSetXml (true, true);
            if (xml->writeTo (f)) setMessage (gr ("Αποθηκεύτηκε: ") + f.getFileName());
            else                  setMessage (gr ("Δεν μπόρεσα να γράψω το αρχείο"));
        });
    }

    void loadSetFromFile()
    {
        auto dir = File::getSpecialLocation (File::userMusicDirectory).getChildFile ("Flow").getChildFile ("Sets");
        chooser = std::make_unique<FileChooser> (gr ("Φόρτωση set"), dir, "*.flowset");
        chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles, [this] (const FileChooser& fc)
        {
            auto f = fc.getResult();
            if (f == File()) return;
            std::unique_ptr<XmlElement> xml (XmlDocument::parse (f));
            if (xml == nullptr || ! xml->hasTagName ("FlowSet"))
            {
                setMessage (gr ("Αυτό δεν είναι set του Flow"));
                return;
            }
            applySet (*xml, {});
            setMessage (gr ("Φορτώθηκε: ") + f.getFileName());
        });
    }

    //==========================================================================
    // αυτόματη αποθήκευση της τελευταίας κατάστασης
    void saveNow (bool withStates = false) { saveSettings (withStates); }

    void saveSettings (bool withStates)
    {
        auto* settings = properties.getUserSettings();

        std::unique_ptr<XmlElement> audioState (deviceManager.createStateXml());
        settings->setValue ("audioDeviceState", audioState.get());
        settings->setValue ("tapeFolder", engine.recorder.getFolder().getFullPathName());
        settings->setValue ("metroLevel", (double) engine.metroLevel.load());
        settings->setValue ("metroChannel", engine.metroChannel.load());
        settings->setValue ("metroSample", metroSamplePath);
        settings->setValue ("link", engine.isLinkEnabled());

        for (int i = 0; i < flow::numControls; ++i)
            settings->setValue ("ctl" + String (i), engine.mapCode[(size_t) i].load());

        if (pendingLoads == 0)    // όσο φορτώνονται plugins δεν αντικαθιστούμε το session (θα χανόταν ό,τι δεν έχει φορτώσει ακόμη)
        {
            auto xml = createSetXml (withStates, false);
            settings->setValue ("session", xml.get());
        }

        properties.saveIfNeeded();
        dirty = false;
        lastSaveMs = Time::getMillisecondCounter();
    }

    void restoreSession()
    {
        auto* settings = properties.getUserSettings();

        StringArray crashed;
        crashed.addTokens (settings->getValue ("loadingTokens"), ",", "");
        crashed.removeEmptyStrings();
        settings->setValue ("loadingTokens", "");
        properties.saveIfNeeded();

        startupCrashed = crashed;

        // Το app ανοίγει πάντα άδειο. Η τελευταία συνεδρία (αν είχε blocks) φυλάγεται ως αντίγραφο και ξαναφορτώνεται μόνο από το "π / -".
        std::unique_ptr<XmlElement> xml (settings->getXmlValue ("session"));
        if (xml == nullptr || ! xml->hasTagName ("FlowSet") || xml->getChildByName ("Block") == nullptr) return;

        settings->setValue ("sessionBackup", xml.get());
        properties.saveIfNeeded();
    }

    bool hasSessionBackup()
    {
        std::unique_ptr<XmlElement> xml (properties.getUserSettings()->getXmlValue ("sessionBackup"));
        return xml != nullptr && xml->hasTagName ("FlowSet") && xml->getChildByName ("Block") != nullptr;
    }

    void restoreLastSession()
    {
        std::unique_ptr<XmlElement> xml (properties.getUserSettings()->getXmlValue ("sessionBackup"));
        if (xml == nullptr || ! xml->hasTagName ("FlowSet")) return;
        applySet (*xml, startupCrashed);
        engine.tempo = jlimit (40.0, 240.0, xml->getDoubleAttribute ("tempo", 120.0));
        saveNow();
    }

    //==========================================================================
    static const char* controlName (int i)
    {
        static const char* const names[flow::numControls] = { "Hold", "Rec MIDI loop / Play / Overdub", "Undo", "Stop", "Rec Audio", "Metronome", "Panic", "Προηγούμενη σκηνή", "Επόμενη σκηνή" };
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
            case flow::ctlHold:  return &holdTile;
            case flow::ctlRec:   return &recTile;
            case flow::ctlUndo:  return &undoTile;
            case flow::ctlStop:  return &stopTile;
            case flow::ctlMetro: return &metroTile;
            case flow::ctlScenePrev:
            case flow::ctlSceneNext: return &metroTile;
            default:             return &tapeTile;
        }
    }

    void startPedalLearn (int i)
    {
        engine.cancelLearns();
        engine.mapLearnTarget = i;
        if (i == flow::ctlMetro || i == flow::ctlPanic)
            setMessage (gr ("Πάτησε ένα ελεύθερο πλήκτρο ή πετάλι για: ") + controlName (i));
        else
            setMessage (gr ("Πάτησε το πετάλι για: ") + controlName (i));
    }

    void setMessage (const String& m) { message = m; messageTicks = 300; }

    void refreshAll()
    {
        const int sel = selected;
        const bool valid = isActive (sel);
        const double t = Time::getMillisecondCounterHiRes() * 0.001;

        // κουμπιά plugin: πράσινο = αυτό που δείχνει τώρα ο editor. Τελείες: υπάρχει plugin / πόσα FX
        const bool hasMfx = valid && engine.getMidiFx (sel) != nullptr;
        const int nfx = valid ? engine.numFx (sel) : 0;
        vstTile.setIconColour (valid ? hobby::kOn : hobby::kIconGrey);   // πράσινο = το block έχει VST
        mfxTile.setIconColour (hasMfx && hasWindow (engine.getMidiFx (sel)) ? hobby::kOn : hobby::kIconGrey);
        mfxTile.setDots (hasMfx ? 1 : 0);
        bool anyFxOpen = false;
        for (int i = 0; i < nfx; ++i) anyFxOpen = anyFxOpen || hasWindow (engine.getFx (sel, i));
        fxTile.setIconColour (anyFxOpen ? hobby::kOn : hobby::kIconGrey);
        fxTile.setDots (jmin (nfx, 3));

        const int cm = valid ? engine.chordMode[(size_t) sel].load() : 0;
        chordTile.setDots (cm);
        chordTile.setIconColour (cm == 0 ? hobby::kIconGrey : hobby::kOn);
        chordTile.setTooltip (gr ("Chord (επιλεγμένο block): πάτημα = Off / Chord (μία νότα δίνει συγχορδία του τύπου που διαλέγεις) / Scale (συγχορδία της κλίμακας: το πρώτο πλήκτρο που πατάς είναι η τονική). Ο κύκλος πάνω από το block αλλάζει τύπο. Τώρα: ")
                              + (cm == 0 ? "Off" : cm == 1 ? "Chord" : "Scale"));

        const int hold = valid ? engine.holdMode[(size_t) sel].load() : 0;
        holdTile.setDots (hold);
        holdTile.setIconColour (hold == 0 ? hobby::kIconGrey : hobby::kOn);
        holdTile.setTooltip (gr ("Hold (επιλεγμένο block): πάτημα = Off / LM Mono Latch (μία νότα) / LP Poly Latch (συγχορδία). Τώρα: ")
                             + (hold == 0 ? "Off" : hold == 1 ? "LM" : hold == 2 ? "LP" : hold == 3 ? "AL Capture" : "AL"));

        const int loop = valid ? engine.loopStateOut[(size_t) sel].load() : (int) flow::loopOff;
        const int mute = valid ? engine.loopMuteOut[(size_t) sel].load() : 0;
        const int taps = valid ? engine.loopTapsOut[(size_t) sel].load() : 0;

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

        metroTile.setIconColour (engine.metroOn.load() ? hobby::kOn : hobby::kIconGrey);

        // κεφαλίδα πάνελ
        const bool master = panelMode == panelMaster;
        String title;
        if (master)            title = "MASTER FX";
        else if (! valid)      title = "FLOW";
        else                   title = "BLOCK " + String (sel + 1);
        header.title = title;
        header.chordName = (valid && ! master) ? KeyboardView::chordNameFor (engine.pcMaskOut[(size_t) sel].load(), engine.bassOut[(size_t) sel].load()) : String();
        const int stopTaps = valid ? engine.loopTapsOut[(size_t) sel].load() : 0;
        if (message.isNotEmpty())  { header.notice = message; header.noticeColour = hobby::kCoral; }
        else if (stopTaps > 0)     { header.notice = "STOP x" + String (stopTaps); header.noticeColour = hobby::kLilac; }
        else                       header.notice = String();
        header.accent = valid ? blockLight (sel) : hobby::kIconGrey;
        header.masterMode = master;
        header.numCircles = master ? engine.numFx (-1) : (valid ? engine.numFx (sel) : 0);
        header.activeCircle = (master || panelMode == panelFx) ? panelIndex : -1;
        header.canAdd = (master || valid) && header.numCircles < maxFxPerChain;
        cpuSmooth = 0.85f * cpuSmooth + 0.15f * (float) deviceManager.getCpuUsage();
        header.cpu = cpuSmooth;
        header.repaint();

        loopsView.repaint();
        keyboardView.repaint();
    }

    void timerCallback() override
    {
        // νέες συσκευές MIDI (π.χ. Bluetooth πετάλι που συνδέθηκε μετά την εκκίνηση) ενεργοποιούνται μόνες τους
        if (++midiPollTicks >= 30)
        {
            midiPollTicks = 0;
            StringArray present;
            for (auto& device : MidiInput::getAvailableDevices())
            {
                present.add (device.identifier);
                if (! knownMidi.contains (device.identifier))
                {
                    knownMidi.add (device.identifier);
                    deviceManager.setMidiInputDeviceEnabled (device.identifier, true);
                }
            }
            for (int i = knownMidi.size(); --i >= 0;)
                if (! present.contains (knownMidi[i]))
                    knownMidi.remove (i);
        }

        const int mc = engine.midiCount.load();
        if (mc != lastMidiCount) { lastMidiCount = mc; lastMidiMs = Time::getMillisecondCounter(); }
        header.midiActive = Time::getMillisecondCounter() - lastMidiMs < 250;

        if (messageTicks > 0 && --messageTicks == 0)
        {
            message.clear();
            engine.cancelLearns();
        }

        engine.sceneRequest.exchange (-1);

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

        // αλλαγές από πετάλια (hold, ένταση, αναθέσεις, ζώνες, tempo) και από το ποντίκι σώζονται άμεσα (με όριο ρυθμού)
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
    }

    //==========================================================================
    flow::Engine engine;
    ApplicationProperties properties;
    AudioDeviceManager deviceManager;
    AudioPluginFormatManager formatManager;
    TooltipWindow tooltipWindow { nullptr, 600 };

    Typeface::Ptr typeface;
    int selected = 0;
    int panelMode = panelInstrument, panelIndex = 0;
    bool layouting = false, inRefresh = false;
    StringArray knownMidi;
    int midiPollTicks = 0, lastMidiCount = 0;
    uint32 lastMidiMs = 0;
    StringArray startupCrashed;
    static constexpr int headerH = 34;
    int winW = 992, winH = 700, lastPrefW = 0, lastPrefH = 0;
    float editorScale = 1.0f;
    Rectangle<int> loopsBezel, pluginBezel, stripBezel, keysBezel;
    String message;
    int lastLearned = 0, lastVolLearned = 0, lastChange = 0;
    int messageTicks = 0, stateTicks = 0;
    bool dirty = false;
    uint32 lastSaveMs = 0;
    Array<int> loadingSlots;
    int pendingLoads = 0;
    bool loadFailed = false;
    int pendingSelect = -1;
    float cpuSmooth = 0.0f;
    String metroSamplePath;
    std::map<const AudioPluginInstance*, String> stateCache;

    std::map<AudioPluginInstance*, std::unique_ptr<PluginWindow>> pluginWindows;
    std::unique_ptr<FileChooser> chooser;

    LoopsView loopsView;
    KeyboardView keyboardView;
    PanelHeader header;
    ValueTile blockTile, vstTile, mfxTile, fxTile, holdTile, chordTile, recTile, undoTile, stopTile, tapeTile, metroTile, folderTile;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

//==============================================================================
class FlowApplication : public JUCEApplication
{
public:
    const String getApplicationName() override    { return "Flow"; }
    const String getApplicationVersion() override { return "0.13.1"; }
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
            setResizable (true, false);
            setResizeLimits (746, 620, 4000, 3000);
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
