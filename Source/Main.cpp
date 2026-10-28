// Flow - master keyboard host για VST3 / AU plugins.
// Prisma Audio

#include "Views.h"
#include "KeyLights.h"
#include "WebOrb.h"
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
          motionTile (Symbol::Orb, typeface),
          recTile (Symbol::PadRecord, typeface),
          undoTile (Symbol::Cancel, typeface),
          stopTile (Symbol::Undo, typeface),
          tapeTile (Symbol::Tape, typeface),
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

        blockTile.setTooltip (gr ("Block: loads a new plugin into a new block. Right-click: ★ special block (the selected block always plays live on top of the others, without a loop. White strip above the keys: drag = position, ends = range) and on/off pedal"));
        vstTile.setTooltip (gr ("VST: opens / closes the instrument window (green = the block has a VST). Right-click: replace, remove"));
        mfxTile.setTooltip (gr ("MIDI FX: MIDI effect (e.g. Stepic) before the instrument. If there is no plugin, opens a file chooser. Right-click: replace, remove"));
        fxTile.setTooltip (gr ("FX: chain of audio effects after the instrument. The circles 1, 2, 3 and the + are in the panel header. Right-click: add"));
        folderTile.setTooltip (gr ("Menu: sets, audio and MIDI settings, master volume, Rec Audio folder, Link, Panic"));
        recTile.setTooltip ("Rec MIDI loop: " + gr ("records the loop of the selected block (the number inside the circle). Press again = end. If the block already has a loop, it does nothing: use re-record or delete. Right-click: quantize and pedal"));
        undoTile.setTooltip (gr ("Delete loop: deletes the loop of the selected block. Pedal: presses = block number (two presses = block 2)"));
        stopTile.setTooltip ("Rec MIDI loop: " + gr ("re-record. Deletes the loop of the selected block and starts a new recording at once. Right-click: mute pedal"));
        tapeTile.setTooltip ("Rec Audio: " + gr ("stop or new take (master and stems per block, in a zip)"));
        metroTile.setTooltip (gr ("Metronome (M key). Plays in mono on a separate output. Right-click: sound, output, key/pedal"));

        addAndMakeVisible (loopsView);
        addAndMakeVisible (keyboardView);
        addAndMakeVisible (header);

        blockTile.onClick = [this] { if (learnClick (flow::ctlSpecial)) return; chooseAndLoadPlugin(); };
        vstTile.onClick = [this] { onVstClick(); };
        mfxTile.onClick = [this] { onMidiFxClick(); };
        fxTile.onClick = [this] { onFxClick(); };

        holdTile.onClick = [this] { cycleHold (selected); };
        chordTile.onClick = [this] { if (learnClick (flow::ctlHold)) return; cycleAl (selected); };
        motionTile.onClick = [this] { toggleOrbRec(); };
        motionTile.setTooltip (gr ("Rec motion: press, then drag a circle in the orb (click flow, top left). The movement is recorded over 16 bars and loops; you can record over it again."));
        recTile.onClick  = [this]
        {
            if (learnClick (flow::ctlRec)) return;
            const int b = engine.recTarget (false);
            if (b < 0) return;
            if (engine.loopStateOut[(size_t) b].load() == flow::loopPlay) return;   // το block έχει ήδη λούπα: ξαναγράφω ή σβήσιμο
            engine.pressMain (b);
        };
        undoTile.onClick = [this] { if (learnClick (flow::ctlUndo)) return; if (isActive (selected)) engine.pressUndo (selected); };
        stopTile.onClick = [this] { if (learnClick (flow::ctlStop)) return; if (isActive (selected)) engine.pressReRec (selected); };
        tapeTile.onClick = [this] { if (learnClick (flow::ctlTape)) return; engine.recorder.pressButton(); };
        // μετρονόμος: 1ο πάτημα = ανοίγει η λωρίδα (tempo, ένταση, έξοδος), 2ο = ξεκινά ο ήχος και αναβοσβήνει, 3ο = σταματά. Το επόμενο ξαναρχίζει τον κύκλο.
        metroTile.onClick = [this]
        {
            if (learnClick (flow::ctlMetro)) return;
            if (engine.metroOn.load())
            {
                toggleMetronome();           // σταματά
                loopsView.setMetroStrip (false);
            }
            else if (! loopsView.metroStrip)
            {
                loopsView.setMetroStrip (true);
            }
            else
            {
                loopsView.setMetroStrip (false);
                toggleMetronome();           // ξεκινά
            }
        };
        loopsView.onMetroChange = [this] { saveNow(); };
        loopsView.metroIsCustom = [this] { return metroSamplePath.isNotEmpty(); };
        loopsView.onMetroSample = [this] (int what)
        {
            if (what == 1) chooseMetroSample();
            else { metroSamplePath = {}; engine.setMetroSample (AudioBuffer<float>()); saveNow(); }
        };
        folderTile.onClick = [this] { loopsView.openMenu (! loopsView.menuOpen); };
        loopsView.onMenuOpen = [this] { buildScreenMenu(); loopsView.setMenuInfo (menuInfoRows()); };

        loopsView.onSelect = [this] (int b) { selectBlock (b); };
        loopsView.getTempoBox().onChange = [this] { dirty = true; };
        keyboardView.onSelect = [this] (int b) { selectBlock (b); };
        keyboardView.onRedoZone = [this] (int b) { showZoneMenu (b); };
        keyboardView.onHold = [this] (int b) { selectBlock (b); cycleHold (b); };
        keyboardView.onRemoveBlock = [this] (int b) { removeBlock (b); };
        keyboardView.onToggleSilent = [this] (int b) { engine.liveSilent[(size_t) b] = engine.liveSilent[(size_t) b].load() != 0 ? 0 : 1; saveNow(); };
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
        keyboardView.onSpecialZone = [this] (int a, int c, bool fin)
        {
            const int sb = engine.specialBlock.load();
            if (sb < 0) return;
            engine.setZone (sb, a, c);
            if (fin) saveNow();
            keyboardView.repaint();
        };
        blockTile.onRightClick = [this] { showBlockMenu(); };

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

        knobBlock.fill (-1); knobParamIdx.fill (-1);
        if (std::unique_ptr<XmlElement> lib { settings->getXmlValue ("orbLibrary") })
            for (auto* pe : lib->getChildWithTagNameIterator ("Plugin"))
                readKnobs (*pe, knobLibrary[pe->getStringAttribute ("key")]);
        for (int k = 0; k < 8; ++k)
        {
            engine.knobCc[(size_t) k] = settings->getIntValue ("knobcc" + String (k), 0);
            engine.knobIn[(size_t) k] = -1.0f;
        }
        for (int b = 0; b < flow::numBlocks; ++b)
            for (int c = 0; c < 4; ++c) orbPos[(size_t) b][(size_t) c] = orbDefault (c);
        loopsView.onOrbAssign = [this] (int slot) { startKnobLearn (slot); };
        loopsView.onOrbClear = [this] (int slot) { clearKnobSlot (slot); };
        loopsView.onOrbMove = [this] (int corner, Point<float> p) { orbMoved (corner, p); };
        loopsView.onOrbTouch = [this] (int corner, bool down) { orbTouch (corner, down); };
        loopsView.onOrbClearMotion = [this] { orbClearMotion(); };
        loopsView.onOrbImage = [this] { pickOrbImage(); };
        loopsView.onOrbMode = [this] { repaint(); };
        loopsView.onOrbImageClear = [this] { setOrbImage ({}); };

        orbFolderPath = settings->getValue ("orbFolder");
        autoOrbImage();   // νέο (άδειο) set: τυχαία εικόνα από το φάκελο Music/Flow/Orb, αν υπάρχει
        webOrbToken = settings->getValue ("webOrbToken");
        if (webOrbToken.isEmpty()) webOrbToken = String::toHexString (Random::getSystemRandom().nextInt64()).paddedLeft ('0', 8).substring (0, 8);
        if (settings->getBoolValue ("webOrbOn", false)) startWebOrb();

        const String savedFolder = settings->getValue ("tapeFolder");
        engine.recorder.setFolder (savedFolder.isNotEmpty() ? File (savedFolder)
                                                            : File::getSpecialLocation (File::userMusicDirectory).getChildFile ("Flow"));

        engine.metroLevel = (float) jlimit (0.0, 1.0, settings->getDoubleValue ("metroLevel", 0.8));
        engine.metroChannel = jmax (0, settings->getIntValue ("metroChannel", 2));
        favPlugins.addLines (settings->getValue ("favPlugins"));
        favPlugins.removeEmptyStrings();
        engine.masterPos = (float) jlimit (0.0, 1.0, settings->getDoubleValue ("masterPos", 1.0));
        engine.limiterOn = settings->getIntValue ("limiterOn", 1) != 0 ? 1 : 0;
        engine.masterCc = settings->getIntValue ("masterCc", 0);
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
        lightsWanted = true;   // τα φώτα πλήκτρων ανάβουν μόνα τους όταν βρεθεί Komplete Kontrol MK2

        refreshSize();
        restoreSession();
        autoOrbImage();   // σε κάθε εκκίνηση: νέα τυχαία εικόνα από το φάκελο (αν έχει εικόνες)· σώζεται με το set
        refreshAll();
        lastChange = engine.changeCount.load();
        startTimerHz (30);
        scanTimer.f = [this] { scanStep(); };
        loadKnownPlugins();
        dockTimer.f = [this] { updateDock(); };
        dockTimer.startTimerHz (60);
    }

    ~MainComponent() override
    {
        webServer.stop();
        orbWindow.reset();
        stopKnobLearn();
        lightsPump.stopTimer();
        stopTimer();
        dockTimer.stopTimer();
        scanTimer.stopTimer();
        if (dockedTop != nullptr) dockedTop->removeComponentListener (this);
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

    void paintOverChildren (Graphics& g) override
    {
        hobby::drawGlass (g, loopsBezel.reduced (5));
        hobby::drawGlass (g, keysBezel.reduced (5));
    }

    //==========================================================================
    void paint (Graphics& g) override
    {
        hobby::drawBody (g, getLocalBounds());
        hobby::drawBrandMark (g, typeface, getWidth());
        hobby::drawEmbossedText (g, typeface, orbStage != 0 ? "orb" : "flow", (float) sideM + 8.0f);

        hobby::drawBezel (g, loopsBezel.toFloat(), loopsBezel.reduced (5).toFloat(), hobby::kScreen, 18.0f);
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
        if (e.y >= pluginBezel.getY() && e.y < pluginBezel.getBottom() && std::abs (e.x - getWidth() / 2) < 40 && ! e.mouseWasDraggedSinceMouseDown())
            showPiMenu();
        // κλικ στη λέξη "flow" (πάνω αριστερά): οθόνη orb
        else if (e.x >= sideM && e.x < sideM + 70 && e.y >= (int) hobby::kBrandY - 6 && e.y < (int) hobby::kBrandY + 24 && ! e.mouseWasDraggedSinceMouseDown())
            cycleOrb();
    }

    bool keyPressed (const KeyPress& k) override
    {
        if (k.getModifiers().isAnyModifierKeyDown()) return false;
        const auto c = k.getTextCharacter();
        if (c == 'm' || c == 'M') { toggleMetronome(); return true; }
        if (k == KeyPress::escapeKey) { if (orbStage != 0) { closeOrb(); return true; } doPanic(); return true; }
        return false;
    }

private:
    //==========================================================================
    std::array<ValueTile*, 12> tiles()
    {
        return { &blockTile, &vstTile, &mfxTile, &fxTile, &chordTile, &motionTile, &recTile, &undoTile, &stopTile, &tapeTile, &metroTile, &folderTile };
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
        menu.addItem (1, gr ("\xe2\x97\x8e  \xe2\x96\xb6"));
        menu.addItem (2, gr ("\xe2\x97\x8e  \xe2\x97\x80"));
        menu.addItem (3, gr ("\xe2\x9c\x95"), engine.mapCode[flow::ctlSceneNext].load() != 0 || engine.mapCode[flow::ctlScenePrev].load() != 0);
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
        const int h = engine.holdMode[(size_t) b].load();
        engine.holdMode[(size_t) b] = h >= 3 ? 0 : (h + 1) % 3;   // Off → LM → LP → Off (το AL έχει δικό του κουμπί)
        saveNow();
        refreshAll();
    }

    // κουμπί AL: Capture (ανάθεση συγχορδιών σε πλήκτρα) → AL (παίζει) → Off
    void cycleAl (int b)
    {
        if (! isActive (b)) return;
        const int h = engine.holdMode[(size_t) b].load();
        engine.holdMode[(size_t) b] = h == 3 ? 4 : h == 4 ? 0 : 3;
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
        if (panelMode == panelMaster) return gr ("Master FX: press + to load an effect on the final mix");
        if (! isActive (selected))    return gr ("Press Block to load the first plugin");
        return gr ("This plugin has no editor");
    }

    //==========================================================================
    // γεωμετρία παραθύρου: μία στήλη (κεφαλίδα, οθόνη loops, κουμπιά, πλήκτρα). Τα plugin ανοίγουν σε δικά τους παράθυρα.
    static constexpr int topH = 14, botH = 14, gapPx = 8, sideM = 11, headerBarH = 34;
    static constexpr int minLoopsH = 230, stripH = 72, keysH = 120, minContentW = 760, defaultLoopsH = 340;
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

        header.setBounds (pluginBezel.getX() + 5, pluginBezel.getY(), pluginBezel.getWidth() - 10, headerH);
        hobby::kBrandY = (float) pluginBezel.getY() + (float) headerH * 0.5f - 8.0f;

        // τρεις ομάδες: αριστερά VST, MIDI FX, FX, Hold, Chord. Στη μέση τα 4 στρογγυλά. Δεξιά metronome και φάκελος.
        const auto strip = stripBezel.reduced (5);
        const int ty = strip.getY() + 5;
        const int leftX = strip.getX() + 14;
        const int midW = 4 * 52 + 3 * 8;
        const int rightX = strip.getRight() - 14 - 52;
        const int leftEnd = leftX + 6 * 60 - 8, rightStart = rightX - 60;
        const int midX = leftEnd + jmax (4, (rightStart - leftEnd - midW) / 2);

        int x = leftX;
        for (auto* t : { &blockTile, &vstTile, &mfxTile, &fxTile, &chordTile, &motionTile })
        {
            t->setBounds (x, ty, 52, 52);
            x += 60;
        }
        x = midX;
        for (auto* t : { &recTile, &stopTile, &undoTile, &tapeTile })
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

    void parentHierarchyChanged() override
    {
        refreshSize();
        // το μαγνητικό παράθυρο ακολουθεί το Flow αμέσως σε κάθε κίνηση, χωρίς να περιμένει το timer
        auto* top = getTopLevelComponent();
        if (top != dockedTop)
        {
            if (dockedTop != nullptr) dockedTop->removeComponentListener (this);
            dockedTop = top;
            if (dockedTop != nullptr) dockedTop->addComponentListener (this);
        }
    }

    void componentMovedOrResized (Component& c, bool, bool) override
    {
        if (&c == dockedTop) updateDock();
    }

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

    // μαγνητικό παράθυρο: κολλάει δεξιά του Flow, ευθυγραμμισμένο με την κορυφή του, και ακολουθεί όταν μετακινείται το Flow.
    // Αν το τραβήξεις με το χέρι ξεκολλάει. Το επόμενο plugin που ανοίγει κολλάει ξανά.
    Point<int> dockPos (const Component& w) const
    {
        Point<int> at { 40, 60 };
        if (auto* top = getTopLevelComponent())
            at = { top->getRight(), top->getY() };
        const auto total = Desktop::getInstance().getDisplays().getTotalBounds (true);
        at.x = jlimit (total.getX(), jmax (total.getX(), total.getRight() - w.getWidth()), at.x);
        at.y = jlimit (total.getY(), jmax (total.getY(), total.getBottom() - w.getHeight()), at.y);
        return at;
    }

    void updateDock()
    {
        if (pluginWindows.empty()) { docked = true; return; }
        auto* top = getTopLevelComponent();
        if (top == nullptr) return;
        auto* w = pluginWindows.begin()->second.get();
        if (! docked) return;
        docked = false;   // τα παράθυρα plugin είναι ελεύθερα: δεν ακολουθούν το Flow
        return;

        const bool mouseDown = ModifierKeys::currentModifiers.isLeftButtonDown();

        // το τράβηξες με το χέρι: κουνήθηκε το plugin ενώ το ποντίκι είναι πατημένο και το Flow έμεινε
        if (mouseDown && top->getBounds() == lastTopBounds && (w->getPosition() - lastWinPos).getDistanceFromOrigin() > 4)
        {
            docked = false;
            flow::flowLog ("dock: undocked by hand at " + String (w->getX()) + "," + String (w->getY()));
            return;
        }

        // αλλιώς: ακολουθεί το Flow (μετακίνηση ή αλλαγή μεγέθους)
        lastTopBounds = top->getBounds();
        const auto expected = dockPos (*w);
        if ((w->getPosition() - expected).getDistanceFromOrigin() > 1)
        {
            w->setTopLeftPosition (expected);
            lastWinPos = w->getPosition();
        }
        else
        {
            lastWinPos = w->getPosition();
        }
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

        // ένα παράθυρο plugin τη φορά: το νέο αντικαθιστά το προηγούμενο και κολλάει στη δεξιά πλευρά του Flow
        for (auto& kv : pluginWindows)
            rememberWindowPos (*kv.second);
        pluginWindows.clear();

        const String key = "pwpos_" + p->getName();
        auto w = std::make_unique<PluginWindow> (*this, p, windowTitle (p), e, key);

        docked = false;   // ελεύθερο παράθυρο: ανοίγει εκεί που ήταν την τελευταία φορά, αλλιώς δεξιά από το Flow
        {
            const String saved = properties.getUserSettings()->getValue (key);
            const auto parts = StringArray::fromTokens (saved, " ", "");
            Point<int> at = dockPos (*w);
            if (parts.size() == 2)
            {
                at = { parts[0].getIntValue(), parts[1].getIntValue() };
                const auto total = Desktop::getInstance().getDisplays().getTotalBounds (true);
                if (! total.contains (at)) at = dockPos (*w);
            }
            w->setTopLeftPosition (at);
        }
        w->setVisible (true);
        w->toFront (true);
        lastWinPos = w->getPosition();
        if (auto* top = getTopLevelComponent()) lastTopBounds = top->getBounds();
        flow::flowLog ("dock: opened at " + String (lastWinPos.x) + "," + String (lastWinPos.y) + " (Flow " + String (lastTopBounds.getX()) + "," + String (lastTopBounds.getY()) + " " + String (lastTopBounds.getWidth()) + "x" + String (lastTopBounds.getHeight()) + ")");
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
        if (! isActive (selected)) { setMessage (gr ("Load an instrument (VST) first")); return; }
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
        if (! isActive (selected)) { setMessage (gr ("Load an instrument (VST) first")); return; }
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

    // φωτάκια εργαλείων στη λωρίδα κάτω, ανάλογα με το κουμπί που δείχνει τώρα: VST ⇄ ✕, MIDI FX ⇄ ✕, FX + ✕. Το ✕ θέλει δύο πατήματα.
    // ◎ φωτάκι: πετάλι. 1ο πάτημα = ανάθεση (πράσινο): πάτα ένα κουμπί της εφαρμογής και μετά το πετάλι. 2ο = διαγραφή ανάθεσης (κόκκινο). 3ο = κλείνει.
    // ---- φώτα πλήκτρων Komplete Kontrol MK2: κάθε block παίρνει το χρώμα του, το ειδικό block ανάβει άσπρο στο πρώτο και στο τελευταίο πλήκτρο
    KeyLights keyLights;
    // Το Komplete Kontrol plugin γράφει κι αυτό φώτα. Γρήγορο ρολόι (200 Hz) ξαναστέλνει τα δικά μας μόλις έρθει νότα και για ~1 s μετά.
    CriticalSection lightsLock;
    std::array<uint8_t, 88> lightsBuf {};
    struct LightsPump : public HighResolutionTimer
    {
        std::function<void()> f;
        void hiResTimerCallback() override { if (f) f(); }
    } lightsPump;
    int pumpMc = 0;
    uint32 pumpBurstUntil = 0, pumpLast = 0;
    void pumpLights()
    {
        const int mc = engine.midiCount.load();
        const uint32 now = Time::getMillisecondCounter();
        if (mc != pumpMc) { pumpMc = mc; pumpBurstUntil = now + 1200; }
        if (now < pumpBurstUntil || now - pumpLast >= 100)
        {
            pumpLast = now;
            const ScopedLock sl (lightsLock);
            if (keyLights.isOn()) keyLights.sendKeys (lightsBuf.data(), 88);
        }
    }
    bool lightsWanted = false;
    int lightsTick = 0;
    std::array<uint8_t, 88> lastLights {};

    void setKeyLights (bool on)
    {
        lightsWanted = on;
        if (on)
        {
            bool ok;
            { const ScopedLock sl (lightsLock); ok = keyLights.start(); }
            flow::flowLog ("key lights: " + keyLights.getStatus());
            if (ok) setMessage (gr ("Key lights: ") + keyLights.getStatus());
            if (ok)
            {
                lastLights.fill (0xff);
                updateLights (true);
                lightsPump.f = [this] { pumpLights(); };
                lightsPump.startTimer (5);
            }
        }
        else
        {
            lightsPump.stopTimer();
            { const ScopedLock sl (lightsLock); keyLights.stop(); }
            flow::flowLog ("key lights: off");
        }
        saveNow();
        updateToolLamps();
    }

    // το φωτάκι του block στην παλέτα του MK2: byte = (απόσταση απόχρωσης + 1) * 4 + φωτεινότητα 0..3. 17 = άσπρο.
    static uint8_t lightByte (int hueIdx, int brightness)
    {
        return (uint8_t) ((jlimit (0, 15, hueIdx) + 1) * 4 + jlimit (0, 3, brightness));
    }

    void updateLights (bool force)
    {
        if (! keyLights.isOn()) return;

        std::array<uint8_t, 88> buf {};
        const int first = keyLights.getFirstNote();
        const int count = keyLights.getKeyCount();
        for (int n = flow::lowestNote; n <= flow::highestNote; ++n)
        {
            const int idx = n - first;
            if (idx < 0 || idx >= count || idx >= 88) continue;
            const int b = engine.blockForNote (n);
            if (b < 0) continue;
            if (engine.blockOff[(size_t) b].load() != 0) continue;
            buf[(size_t) idx] = lightByte (blockHue (b), b == selected ? 3 : 1);
        }

        // ό,τι κρατιέται με Hold (και οι νότες συγχορδίας του) είναι λευκό και στο hardware, ακόμα κι αν είναι έξω από τη ζώνη
        for (int b = 0; b < flow::numBlocks; ++b)
        {
            if (! isActive (b) || engine.blockOff[(size_t) b].load() != 0) continue;
            for (int n = flow::lowestNote; n <= flow::highestNote; ++n)
            {
                const int idx = n - first;
                if (idx < 0 || idx >= count || idx >= 88) continue;
                if (((engine.latchBits[(size_t) b * 2 + (size_t) (n >> 6)].load() >> (n & 63)) & 1) != 0)
                    buf[(size_t) idx] = (uint8_t) (17 * 4 + 3);
            }
        }

        const int sb = engine.specialBlock.load();
        if (sb >= 0 && isActive (sb) && engine.blockOff[(size_t) sb].load() == 0 && engine.zoneState[(size_t) sb].load() == 0)
            for (int n : { engine.lo[(size_t) sb].load(), engine.hi[(size_t) sb].load() })
            {
                const int idx = n - first;
                if (idx >= 0 && idx < count && idx < 88) buf[(size_t) idx] = (uint8_t) (17 * 4 + 3);   // άσπρο
            }

        if (force || buf != lastLights || ++lightsTick >= 4)
        {
            lightsTick = 0;
            lastLights = buf;
            bool ok;
            {
                const ScopedLock sl (lightsLock);
                lightsBuf = buf;
                ok = keyLights.sendKeys (buf.data(), 88);
            }
            if (! ok)
            {
                flow::flowLog ("key lights: " + keyLights.getStatus());
                lightsPump.stopTimer();
                { const ScopedLock sl (lightsLock); keyLights.stop(); }
                lightsWanted = true;
                updateToolLamps();
            }
        }
    }

    int statTicks = 0, statWindows = 0, lastXRuns = -1;
    bool lightsTried = false;
    int learnMode = 0;

    // Orb: 4 γωνίες × 4 αναθέσεις = 16 θέσεις (slot = γωνία * 4 + γραμμή). Κάθε θέση δείχνει σε μία παράμετρο του plugin του επιλεγμένου block.
    static constexpr int kSlots = 16;
    std::array<int, kSlots> knobBlock, knobParamIdx;     // block και δείκτης παραμέτρου (-1 = ελεύθερο)
    int knobStage = 0, knobSlot = -1;   // 1 = περιμένει να κουνηθεί παράμετρος σε plugin
    uint32 knobStageStart = 0;
    std::atomic<int> knobTouched { -1 };   // (block << 20) | δείκτης

    struct ParamWatch : AudioProcessorParameter::Listener
    {
        AudioPluginInstance* plugin = nullptr;
        std::atomic<int>* sink = nullptr;
        int block = 0;
        uint32 ignoreUntil = 0;
        void attach (AudioPluginInstance* p, int b, std::atomic<int>* s)
        {
            detach();
            plugin = p; block = b; sink = s; ignoreUntil = Time::getMillisecondCounter() + 400;
            for (auto* prm : p->getParameters()) prm->addListener (this);
        }
        void detach()
        {
            if (plugin != nullptr) for (auto* prm : plugin->getParameters()) prm->removeListener (this);
            plugin = nullptr;
        }
        void parameterValueChanged (int i, float) override { hit (i); }
        void parameterGestureChanged (int i, bool starting) override { if (starting) hit (i); }
        void hit (int i)
        {
            if (sink != nullptr && Time::getMillisecondCounter() >= ignoreUntil && sink->load() < 0)
                *sink = (block << 20) | (i & 0xfffff);
        }
        ~ParamWatch() override { detach(); }
    };
    std::array<ParamWatch, flow::numBlocks> watches;

    // Οι αναθέσεις ανήκουν στο plugin κάθε block (σώζονται μέσα στο set) και θυμούνται και σε μια βιβλιοθήκη ανά plugin για νέα blocks.
    // Τα CC του hardware (knobCc) είναι ξεχωριστά και μένουν πάντα. Τα knobBlock/knobParamIdx είναι η τρέχουσα όψη για το επιλεγμένο block.
    struct KnobAssign { int idx = -1; String name; };
    std::array<std::array<KnobAssign, kSlots>, (size_t) flow::numBlocks> blockKnobs;
    std::map<String, std::array<KnobAssign, kSlots>> knobLibrary;

    String knobKey (int block)
    {
        if (auto* p = engine.getPlugin (block)) return p->getPluginDescription().createIdentifierString();
        return {};
    }

    void syncKnobView()
    {
        const int sel = isActive (selected) ? selected : -1;
        for (int k = 0; k < kSlots; ++k)
        {
            const int idx = sel >= 0 ? blockKnobs[(size_t) sel][(size_t) k].idx : -1;
            knobBlock[(size_t) k] = idx >= 0 ? sel : -1;
            knobParamIdx[(size_t) k] = idx;
        }
    }

    void validateKnobs (int block)   // μετά από αλλαγή έκδοσης plugin ο δείκτης μπορεί να άλλαξε: ψάχνει την παράμετρο με το ίδιο όνομα
    {
        auto* p = engine.getPlugin (block);
        for (auto& a : blockKnobs[(size_t) block])
        {
            if (a.idx < 0) continue;
            if (p == nullptr) { a.idx = -1; continue; }
            auto& params = p->getParameters();
            if (a.name.isEmpty()) { if (a.idx >= params.size()) a.idx = -1; continue; }
            if (a.idx < params.size() && params[a.idx]->getName (64) == a.name) continue;
            int found = -1;
            for (int i = 0; i < params.size(); ++i)
                if (params[i]->getName (64) == a.name) { found = i; break; }
            a.idx = found;
        }
    }

    void rememberKnobs (int block)
    {
        const auto key = knobKey (block);
        if (key.isNotEmpty()) knobLibrary[key] = blockKnobs[(size_t) block];
    }

    void ensureKnobs (int block)   // νέο block ή set χωρίς αναθέσεις: παίρνει όσα θυμάται το Flow για αυτό το plugin
    {
        bool any = false;
        for (auto& a : blockKnobs[(size_t) block]) if (a.idx >= 0) any = true;
        if (! any)
        {
            auto it = knobLibrary.find (knobKey (block));
            if (it != knobLibrary.end()) blockKnobs[(size_t) block] = it->second;
        }
        validateKnobs (block);
    }

    void clearKnobsOf (int block)
    {
        for (auto& a : blockKnobs[(size_t) block]) a = KnobAssign();
        for (auto& l : orbLane[(size_t) block]) l = OrbLane();
    }

    static void writeKnobs (XmlElement& parent, const std::array<KnobAssign, kSlots>& arr)
    {
        for (int k = 0; k < kSlots; ++k)
            if (arr[(size_t) k].idx >= 0)
            {
                auto* ki = parent.createNewChildElement ("K");
                ki->setAttribute ("i", k);
                ki->setAttribute ("p", arr[(size_t) k].idx);
                ki->setAttribute ("n", arr[(size_t) k].name);
            }
    }

    static void readKnobs (const XmlElement& parent, std::array<KnobAssign, kSlots>& arr)
    {
        for (auto& a : arr) a = KnobAssign();
        for (auto* ki : parent.getChildWithTagNameIterator ("K"))
        {
            const int k = ki->getIntAttribute ("i", -1), idx = ki->getIntAttribute ("p", -1);
            if (k >= 0 && k < kSlots && idx >= 0) arr[(size_t) k] = { idx, ki->getStringAttribute ("n") };
        }
    }

    AudioProcessorParameter* knobTarget (int k)
    {
        const int b = knobBlock[(size_t) k], idx = knobParamIdx[(size_t) k];
        if (b < 0 || idx < 0 || ! isActive (b)) return nullptr;
        auto* p = engine.getPlugin (b);
        if (p == nullptr || idx >= p->getParameters().size()) return nullptr;
        return p->getParameters()[idx];
    }

    void stopKnobLearn()
    {
        for (auto& w : watches) w.detach();
        if (knobStage != 0) engine.knobLearnSlot = -1;
        knobStage = 0; knobSlot = -1;
    }

    // ---- Orb: αναθέσεις (learn) ----
    void startKnobLearn (int k)
    {
        stopKnobLearn();
        engine.cancelLearns();
        knobTouched = -1;
        // η ανάθεση γίνεται για το plugin του επιλεγμένου block
        auto* p = isActive (selected) ? engine.getPlugin (selected) : nullptr;
        if (p == nullptr || p->getParameters().isEmpty()) { setMessage (gr ("The selected block has no parameters")); return; }
        watches[(size_t) selected].attach (p, selected, &knobTouched);
        knobStage = 1; knobSlot = k; knobStageStart = Time::getMillisecondCounter();
        setMessage (gr ("Move a parameter in a plugin window"));
    }

    void clearKnobSlot (int k)
    {
        if (! isActive (selected)) return;
        blockKnobs[(size_t) selected][(size_t) k] = KnobAssign();
        rememberKnobs (selected);
        stopKnobLearn();
        saveNow();
    }

    // ---- Orb: οθόνη (πάτημα στη λέξη flow): 1ο πάτημα = στην οθόνη του Flow, 2ο = σε δικό του παράθυρο (π.χ. iPad), 3ο = κλείνει ----
    int orbStage = 0;   // 0 κλειστός, 1 στην οθόνη του Flow, 2 σε ξεχωριστό παράθυρο
    std::unique_ptr<OrbWindow> orbWindow;

    void cycleOrb()
    {
        if (orbStage == 0) { orbStage = 1; loopsView.setOrbMode (true); }
        else if (orbStage == 1)
        {
            orbStage = 2;
            loopsView.setOrbMode (false);
            orbWindow = std::make_unique<OrbWindow> (loopsView, [this] { MessageManager::callAsync ([safe = SafePointer<MainComponent> (this)] { if (safe != nullptr) safe->closeOrb(); }); });
        }
        else closeOrb();
        repaint();
    }

    void closeOrb()
    {
        orbStage = 0;
        loopsView.setOrbMode (false);
        orbWindow.reset();
        repaint();
    }

    // ---- Orb στο Safari του iPad (web server μέσα στο Flow) ----
    webOrb::Server webServer;
    bool webOrbOn = false;
    String webOrbToken;
    CriticalSection webImgLock;
    MemoryBlock webImgJpeg;
    int webImgVer = 0;
    String lastWebState;

    void updateWebImage()
    {
        MemoryBlock mb;
        if (orbImage.isValid())
        {
            MemoryOutputStream mo (mb, false);
            JPEGImageFormat jf; jf.setQuality (0.82f);
            jf.writeImageToStream (orbImage, mo);
        }
        const ScopedLock sl (webImgLock);
        webImgJpeg = mb;
        ++webImgVer;
    }

    void startWebOrb()
    {
        if (webServer.port != 0) return;
        webServer.token = webOrbToken;
        webServer.getImage = [this] { const ScopedLock sl (webImgLock); return webImgJpeg; };
        webServer.onMessage = [safe = SafePointer<MainComponent> (this)] (const String& msg)
        {
            MessageManager::callAsync ([safe, msg] { if (safe != nullptr) safe->webOrbMessage (msg); });
        };
        webOrbOn = webServer.start (8765);
        if (! webOrbOn) setMessage (gr ("iPad orb: could not start the server"));
        lastWebState = {};
    }

    void stopWebOrb()
    {
        webServer.stop();
        webServer.port = 0;
        webOrbOn = false;
    }

    void webOrbMessage (const String& msg)
    {
        const var v = JSON::parse (msg);
        const String t = v.getProperty ("t", {}).toString();
        const int c = (int) v.getProperty ("c", -1);
        if (t == "rec") { toggleOrbRec(); return; }
        if (t == "clear") { orbClearMotion(); return; }
        if (c < 0 || c > 3) return;
        if (t == "touch") orbTouch (c, (int) v.getProperty ("d", 0) != 0);
        else if (t == "move")
        {
            const float x = (float) (double) v.getProperty ("x", 0.0), y = (float) (double) v.getProperty ("y", 0.0);
            orbMoved (c, { x, y });
        }
    }

    void pushWebState (const std::array<LoopsView::KnobState, kSlots>& st, const std::array<Point<float>, 4>& pos)
    {
        if (! webOrbOn) return;
        auto* o = new DynamicObject();
        Array<var> p, k;
        for (int c = 0; c < 4; ++c) p.add (var (Array<var> { var ((double) pos[(size_t) c].x), var ((double) pos[(size_t) c].y) }));
        for (int i = 0; i < kSlots; ++i)
            k.add (var (Array<var> { var (st[(size_t) i].bound ? 1 : 0), var (std::round (st[(size_t) i].value * 100.0)), var (st[(size_t) i].name) }));
        o->setProperty ("p", p);
        o->setProperty ("k", k);
        o->setProperty ("r", orbRecArmed ? 1 : 0);
        o->setProperty ("m", anyOrbMotion() ? 1 : 0);
        o->setProperty ("ph", std::round (orbPhase() * 1000.0) / 1000.0);
        o->setProperty ("t", loopsView.title);
        { const ScopedLock sl (webImgLock); o->setProperty ("iv", webImgVer); }
        const String js = JSON::toString (var (o), true);
        if (js != lastWebState) { lastWebState = js; webServer.setState (js); }
    }

    String webOrbUrl() const
    {
        String ip;
        for (auto& a : IPAddress::getAllAddresses (false))
            if (! a.isIPv6 && a.toString() != "127.0.0.1" && ! a.toString().startsWith ("169.254")) { ip = a.toString(); break; }
        if (ip.isEmpty()) ip = "<ip>";
        return "http://" + ip + ":" + String (webServer.port) + "/?k=" + webOrbToken;
    }

    // ---- Orb: εικόνα (σώζεται με το set) ----
    Image orbImage;
    std::unique_ptr<FileChooser> orbChooser;

    void setOrbImage (Image img, bool save = true)
    {
        if (img.isValid() && (img.getWidth() > 512 || img.getHeight() > 512))
        {
            const float k = 512.0f / (float) jmax (img.getWidth(), img.getHeight());
            img = img.rescaled (jmax (1, roundToInt ((float) img.getWidth() * k)), jmax (1, roundToInt ((float) img.getHeight() * k)), Graphics::highResamplingQuality);
        }
        orbImage = img;
        updateWebImage();
        loopsView.orbImage = img;
        loopsView.repaint();
        if (save) saveNow();
    }

    // ---- Orb: αυτόματη εικόνα από τον φάκελο Music/Flow/Orb, με τυχαία παραλλαγή ----
    String orbFolderPath;       // ρύθμιση χρήστη· κενό = Music/Flow/Orb
    std::unique_ptr<FileChooser> orbFolderChooser;

    File orbImagesDir() const
    {
        if (orbFolderPath.isNotEmpty()) return File (orbFolderPath);
        return File::getSpecialLocation (File::userMusicDirectory).getChildFile ("Flow").getChildFile ("Orb");
    }

    void chooseOrbFolder()
    {
        orbFolderChooser = std::make_unique<FileChooser> (gr ("Folder with images for the orb"), orbImagesDir().getParentDirectory());
        orbFolderChooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectDirectories, [this] (const FileChooser& fc)
        {
            const auto f = fc.getResult();
            if (f == File() || ! f.isDirectory()) return;
            orbFolderPath = f.getFullPathName();
            saveNow();
            setMessage (gr ("Orb images folder: ") + f.getFileName());
        });
    }

    static Image orbVariation (const Image& src, Random& rnd)
    {
        constexpr int N = 512;
        const int s0 = jmin (src.getWidth(), src.getHeight());
        const int s = jmax (16, (int) ((float) s0 / (1.0f + rnd.nextFloat() * 0.8f)));      // ζουμ 1x έως 1.8x
        const Image crop = src.getClippedImage ({ rnd.nextInt (jmax (1, src.getWidth() - s + 1)), rnd.nextInt (jmax (1, src.getHeight() - s + 1)), s, s });

        Image out (Image::ARGB, N, N, true);
        {
            Graphics g (out);
            g.setImageResamplingQuality (Graphics::highResamplingQuality);
            const float angle = (float) rnd.nextInt (4) * MathConstants<float>::halfPi + (rnd.nextFloat() - 0.5f) * 0.5f;
            const float flip = rnd.nextBool() ? -1.0f : 1.0f;
            const float big = 1.5f * (float) N;   // μεγαλύτερο, ώστε η περιστροφή να μην αφήνει κενά στις γωνίες
            g.addTransform (AffineTransform::scale (flip, 1.0f).rotated (angle).translated ((float) N * 0.5f, (float) N * 0.5f));
            g.drawImage (crop, Rectangle<float> (-big * 0.5f, -big * 0.5f, big, big));
        }

        // χρώμα: μετατόπιση απόχρωσης, κορεσμός, φωτεινότητα
        const float hueShift = rnd.nextFloat(), satMul = 0.7f + rnd.nextFloat() * 0.6f, briMul = 0.8f + rnd.nextFloat() * 0.35f;
        {
            Image::BitmapData bd (out, Image::BitmapData::readWrite);
            for (int y = 0; y < N; ++y)
                for (int x = 0; x < N; ++x)
                {
                    const auto c = bd.getPixelColour (x, y);
                    float h = 0, sa = 0, v = 0;
                    c.getHSB (h, sa, v);
                    bd.setPixelColour (x, y, Colour::fromHSV (std::fmod (h + hueShift, 1.0f), jlimit (0.0f, 1.0f, sa * satMul), jlimit (0.0f, 1.0f, v * briMul), c.getFloatAlpha()));
                }
        }

        return out;
    }

    void autoOrbImage()
    {
        auto dir = orbImagesDir();
        dir.createDirectory();
        const auto files = dir.findChildFiles (File::findFiles, false, "*.jpg;*.jpeg;*.png");
        if (files.isEmpty()) return;
        Random rnd;
        const auto src = ImageFileFormat::loadFrom (files[rnd.nextInt (files.size())]);
        if (src.isValid()) setOrbImage (orbVariation (src, rnd), false);
    }

    void pickOrbImage()
    {
        orbChooser = std::make_unique<FileChooser> (gr ("Choose an image for the orb"), File::getSpecialLocation (File::userPicturesDirectory), "*.png;*.jpg;*.jpeg");
        orbChooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles, [this] (const FileChooser& fc)
        {
            const auto f = fc.getResult();
            if (f == File()) return;
            auto img = ImageFileFormat::loadFrom (f);
            if (img.isValid()) setOrbImage (img); else setMessage (gr ("Could not read the image"));
        });
    }

    // ---- Orb: θέση κύκλων, macro, κίνηση ----
    static constexpr int orbBars = 16;                  // διάρκεια της κίνησης: 16 μέτρα (4/4)
    static constexpr int orbPerBeat = 16;                // δείγματα ανά beat
    static constexpr int orbN = orbBars * 4 * orbPerBeat;   // 1024 δείγματα
    struct OrbLane { std::vector<Point<float>> s; std::vector<char> ok; bool has = false, recording = false; };
    std::array<std::array<Point<float>, 4>, (size_t) flow::numBlocks> orbPos;
    std::array<std::array<OrbLane, 4>, (size_t) flow::numBlocks> orbLane;
    std::array<bool, 4> orbTouching { { false, false, false, false } };
    bool orbRecArmed = false;

    static Point<float> orbClamp (int, Point<float> p)   // ελεύθερη κίνηση σε όλον τον orb (όριο: ο κύκλος)
    {
        const float len = p.getDistanceFromOrigin();
        return len > 1.0f ? Point<float> (p.x / len, p.y / len) : p;
    }
    static Point<float> orbDefault (int corner) { return orbClamp (corner, { ((corner == 1 || corner == 2) ? 1.0f : -1.0f) * 0.3536f, (corner >= 2 ? 1.0f : -1.0f) * 0.3536f }); }
    static float orbValue (Point<float> p) { return jlimit (0.0f, 1.0f, p.getDistanceFromOrigin()); }

    double orbPhase() const   // 0..1 μέσα στα 16 μέτρα
    {
        constexpr double cyc = (double) (orbBars * 4);
        return std::fmod (jmax (0.0, engine.beatPosOut.load()), cyc) / cyc;
    }

    AudioProcessorParameter* orbTarget (int block, int slot)
    {
        if (block < 0 || block >= flow::numBlocks || ! isActive (block)) return nullptr;
        const int idx = blockKnobs[(size_t) block][(size_t) slot].idx;
        auto* p = engine.getPlugin (block);
        if (idx < 0 || p == nullptr || idx >= p->getParameters().size()) return nullptr;
        return p->getParameters()[idx];
    }

    // Οι 4 γραμμές της λίστας είναι οι 4 κατευθύνσεις: πάνω, δεξιά, κάτω, αριστερά.
    // Κάθε παράμετρος παίρνει 0% στο κέντρο και 100% όταν ο κύκλος φτάσει στην άκρη προς τη δική της κατεύθυνση (οι διαγώνιες κινούν δύο).
    void orbApply (int block, int corner, Point<float> p)
    {
        const float v[4] { jlimit (0.0f, 1.0f, -p.y), jlimit (0.0f, 1.0f, p.x), jlimit (0.0f, 1.0f, p.y), jlimit (0.0f, 1.0f, -p.x) };
        for (int r = 0; r < 4; ++r)
            if (auto* prm = orbTarget (block, corner * 4 + r))
                if (std::abs (prm->getValue() - v[r]) > 0.002f) prm->setValueNotifyingHost (v[r]);
    }

    void orbFinishLane (OrbLane& l)
    {
        l.recording = false;
        int first = -1;
        for (int i = 0; i < orbN; ++i) if (l.ok[(size_t) i]) { first = i; break; }
        if (first < 0) { l.has = false; l.s.clear(); l.ok.clear(); return; }
        Point<float> last = l.s[(size_t) first];
        for (int n = 0; n < orbN; ++n)   // κρατά την τελευταία θέση στα κενά (κυκλικά)
        {
            const int i = (first + n) % orbN;
            if (l.ok[(size_t) i]) last = l.s[(size_t) i]; else l.s[(size_t) i] = last;
        }
        l.has = true;
    }

    void orbTouch (int corner, bool down)
    {
        orbTouching[(size_t) corner] = down;
        if (! isActive (selected)) return;
        auto& l = orbLane[(size_t) selected][(size_t) corner];
        if (down && orbRecArmed)   // overdub: γράφει μόνο εκεί που αγγίζεις, τα υπόλοιπα μένουν
        {
            if (l.s.empty()) { l.s.assign ((size_t) orbN, {}); l.ok.assign ((size_t) orbN, 0); }
            l.recording = true;
        }
        else if (! down && l.recording) orbFinishLane (l);
    }

    void orbMoved (int corner, Point<float> p)
    {
        if (selected < 0 || selected >= flow::numBlocks) return;   // οι κύκλοι κινούνται και σε κουμπί χωρίς όργανο
        p = orbClamp (corner, p);
        orbPos[(size_t) selected][(size_t) corner] = p;
        orbApply (selected, corner, p);
    }

    void orbClearMotion()
    {
        if (! isActive (selected)) return;
        for (auto& l : orbLane[(size_t) selected]) { l = OrbLane(); }
        setMessage (gr ("Motion cleared"));
    }

    void toggleOrbRec()
    {
        orbRecArmed = ! orbRecArmed;
        if (! orbRecArmed)
            for (auto& bl : orbLane) for (auto& l : bl) if (l.recording) orbFinishLane (l);
        setMessage (orbRecArmed ? gr ("Rec motion (16 bars): drag a circle in the orb") : gr ("Rec motion off"));
        motionTile.setIconColour (orbRecArmed ? hobby::kRed : hobby::kIconGrey);
    }

    void orbTick()   // εγγραφή και αναπαραγωγή κίνησης (30 φορές το δευτερόλεπτο)
    {
        const double ph = orbPhase() * orbN;
        const int i = jlimit (0, orbN - 1, (int) ph);
        const int i2 = (i + 1) % orbN;
        const float fr = (float) (ph - std::floor (ph));
        for (int b = 0; b < flow::numBlocks; ++b)
        {
            if (! isActive (b)) continue;
            for (int c = 0; c < 4; ++c)
            {
                auto& l = orbLane[(size_t) b][(size_t) c];
                const bool touching = b == selected && orbTouching[(size_t) c];
                if (touching)
                {
                    if (l.recording) { l.s[(size_t) i] = orbPos[(size_t) b][(size_t) c]; l.ok[(size_t) i] = 1; }
                }
                else if (l.has)
                {
                    const auto p = l.s[(size_t) i] + (l.s[(size_t) i2] - l.s[(size_t) i]) * fr;   // ομαλή μετάβαση ανάμεσα στα δείγματα
                    orbPos[(size_t) b][(size_t) c] = p;
                    orbApply (b, c, p);
                }
            }
        }
    }

    bool anyOrbMotion() const
    {
        if (! isActive (selected)) return false;
        for (auto& l : orbLane[(size_t) selected]) if (l.has) return true;
        return false;
    }

    void knobTick()
    {
        syncKnobView();
        if (loopsView.menuOpen) loopsView.setMenuInfo (menuInfoRows());
        if (engine.masterLearned.exchange (0) > 0)
        {
            setMessage (gr ("Master volume ") + String::fromUTF8 ("\xe2\x86\x90") + " " + pedalCodeText (engine.masterCc.load()));
            saveNow();
        }
        if (knobStage != 0)
        {
            if (Time::getMillisecondCounter() - knobStageStart > 30000)
            {
                stopKnobLearn(); setMessage (gr ("Assignment cancelled"));
            }
            else if (knobStage == 1 && knobTouched.load() >= 0)
            {
                const int t = knobTouched.load();
                for (auto& w : watches) w.detach();
                const int tb = t >> 20, ti = t & 0xfffff;
                if (tb >= 0 && tb < flow::numBlocks)
                    if (auto* tp = engine.getPlugin (tb))
                        if (ti < tp->getParameters().size())
                        {
                            blockKnobs[(size_t) tb][(size_t) knobSlot] = { ti, tp->getParameters()[ti]->getName (64) };
                            rememberKnobs (tb);
                            setMessage (tp->getParameters()[ti]->getName (40));
                        }
                knobStage = 0; knobSlot = -1;
                saveNow();
            }
        }

        orbTick();

        std::array<LoopsView::KnobState, kSlots> st;
        for (int k = 0; k < kSlots; ++k)
            if (auto* prm = knobTarget (k))
            {
                st[(size_t) k].bound = true;
                st[(size_t) k].value = prm->getValue();
                st[(size_t) k].name = prm->getName (24);
            }
        std::array<Point<float>, 4> pos;
        for (int c = 0; c < 4; ++c) pos[(size_t) c] = (selected >= 0 && selected < flow::numBlocks) ? orbPos[(size_t) selected][(size_t) c] : orbDefault (c);
        loopsView.setOrb (st, pos, knobStage != 0 ? knobSlot : -1, orbRecArmed, anyOrbMotion());
        pushWebState (st, pos);
    }
    bool learnClick (int ctl)
    {
        if (learnMode == 0) return false;
        if (learnMode == 1) startPedalLearn (ctl);
        else
        {
            engine.mapCode[(size_t) ctl] = 0;
            setMessage (String (controlName (ctl)) + " " + String::fromUTF8 ("\xe2\x9c\x95"));
            saveNow();
        }
        learnMode = 0;
        updateToolLamps();
        return true;
    }

    uint32 armedAt[3] = { 0, 0, 0 };
    bool armedNow (int i) const { return Time::getMillisecondCounter() < armedAt[i]; }
    void pressDelete (int i, std::function<void()> doIt)
    {
        if (armedNow (i)) { armedAt[i] = 0; doIt(); }
        else { armedAt[i] = Time::getMillisecondCounter() + 3000; }
        updateToolLamps();
    }

    void updateToolLamps()
    {
        std::vector<LoopsView::ToolLamp> l;
        const bool v = isActive (selected);
        if (v && panelMode == panelInstrument)
        {
            l.push_back ({ String::fromUTF8 ("\xe2\x87\x84"), hobby::kOn, true, false, [this] { replaceInstrument(); }, gr ("Change the instrument (VST) of the block") });
        }
        else if (v && panelMode == panelMidiFx)
        {
            const bool has = engine.getMidiFx (selected) != nullptr;
            l.push_back ({ String::fromUTF8 ("\xe2\x87\x84"), hobby::kLilac, true, false, [this] { loadMidiFxInto (selected); }, gr ("Load MIDI FX in the block") });
            l.push_back ({ String::fromUTF8 ("\xe2\x9c\x95"), hobby::kLilac, has, armedNow (1), [this] { pressDelete (1, [this] { removeMidiFx (selected); }); }, gr ("Remove MIDI FX (two presses)") });
        }
        else if (v && panelMode == panelFx)
        {
            l.push_back ({ "+", hobby::kPeach, engine.numFx (selected) < maxFxPerChain, false, [this] { addFxTo (selected); }, gr ("Add an effect (FX) to the block") });
            l.push_back ({ String::fromUTF8 ("\xe2\x9c\x95"), hobby::kPeach, engine.numFx (selected) > 0, armedNow (2), [this] { pressDelete (2, [this] { clearFx (selected); }); }, gr ("Remove all effects of the block (two presses)") });
        }
        if (v)   // ★ ειδικό block (πάνω από τα άλλα, χωρίς λούπα)
            l.push_back ({ String ("SP"), Colours::white, true, false, [this] { toggleSpecial(); }, gr ("SP = special block: on top of the others, no loop, always plays") });
        // quantize: κάθε πάτημα αλλάζει επίπεδο και το μήνυμα δείχνει το νέο. Σβηστό (γκρι) = Off.
        auto q3 = [this] (std::atomic<int>& q, const String& name, const char* l0, const char* l1, const char* l2)
        {
            q = (q.load() + 1) % 3;
            setMessage (name + ": " + String (q.load() == 0 ? l0 : q.load() == 1 ? l1 : l2));
            saveNow();
            updateToolLamps();
        };
        l.push_back ({ String::fromUTF8 ("\xe2\x96\xb6|"), engine.startQ.load() != 0 ? hobby::kOn : hobby::kIconGrey, true, false,
                       [this, q3] { q3 (engine.startQ, "Start quantize", "Off", "beat", "bar"); }, gr ("Start quantize: where the loop starts (Off / beat / bar)") });
        l.push_back ({ String::fromUTF8 ("\xe2\x86\x94"), engine.lengthQ.load() != 0 ? hobby::kOn : hobby::kIconGrey, true, false,
                       [this, q3] { q3 (engine.lengthQ, "Length quantize", "Off", "beat", "bar (4/4)"); }, gr ("Length quantize: loop length in whole beats or bars") });
        l.push_back ({ String::fromUTF8 ("\xe2\x97\x8e"), learnMode == 0 ? hobby::kIconGrey : learnMode == 1 ? hobby::kOn : hobby::kRed, true, false,
                       [this] { learnMode = (learnMode + 1) % 3; stopKnobLearn(); if (learnMode == 1) setMessage (gr ("Press the button or knob you want to assign")); updateToolLamps(); },
                       gr ("MIDI learn: green = assign pedal, red = remove assignment") });
        loopsView.setToolLamps (std::move (l));
    }

    void showVstMenu()
    {
        PopupMenu menu;
        const bool v = isActive (selected);
        menu.addItem (2, gr ("\xe2\x87\x84"), v);
        menu.addItem (3, gr ("\xe2\x9c\x95"), v);

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
        menu.addItem (1, gr ("\xe2\x87\x84"), v);
        menu.addItem (2, gr ("\xe2\x9c\x95"), has);

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
        menu.addItem (1, gr ("+"), v && engine.numFx (selected) < maxFxPerChain);
        menu.addItem (2, gr ("\xe2\x9c\x95"), v && engine.numFx (selected) > 0);

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
        menu.addItem (1, gr ("\xe2\x9c\x95"));
        menu.addItem (2, gr ("\xe2\x97\x80"), i > 0);
        menu.addItem (3, gr ("\xe2\x96\xb6"), i < n - 1);

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
    // λίστα plugins από τη σάρωση των φακέλων (VST3, AU). kind: 0 = instruments, 1 = effects, 2 = MIDI FX (effects)
    File pluginListFile() { return properties.getUserSettings()->getFile().getSiblingFile ("Flow-plugins.xml"); }
    File pluginScanCrashFile() { return properties.getUserSettings()->getFile().getSiblingFile ("Flow-scan-crash.txt"); }

    void loadKnownPlugins()
    {
        if (auto xml = parseXML (pluginListFile()))
            knownPlugins.recreateFromXml (*xml);
    }

    void saveKnownPlugins()
    {
        if (auto xml = knownPlugins.createXml())
            xml->writeTo (pluginListFile());
    }

    void startPluginScan()
    {
        if (scanning) return;
        scanning = true;
        scanFormatIndex = 0;
        scanner.reset();
        setMessage (gr ("Scanning plugins…"));
        scanTimer.startTimer (40);
    }

    void scanStep()
    {
        if (scanner == nullptr)
        {
            if (scanFormatIndex >= formatManager.getNumFormats())
            {
                scanTimer.stopTimer();
                scanning = false;
                saveKnownPlugins();
                setMessage (gr ("Found ") + String (knownPlugins.getNumTypes()) + gr (" plugins"));
                return;
            }
            auto* fmt = formatManager.getFormat (scanFormatIndex);
            scanner = std::make_unique<PluginDirectoryScanner> (knownPlugins, *fmt, fmt->getDefaultLocationsToSearch(), true, pluginScanCrashFile());
        }
        String name;
        if (! scanner->scanNextFile (true, name))
        {
            scanner.reset();
            ++scanFormatIndex;
        }
        else
        {
            setMessage (gr ("Scanning: ") + name);
        }
    }

    StringArray favPlugins;   // favorites (σήμα f): κλειδιά plugins, κοινά για instruments και effects

    void pickPlugin (const String& title, int kind, std::function<void (const PluginDescription&)> chosen)
    {
        if (scanning)
        {
            setMessage (gr ("Plugin scan is still running… try again in a moment"));
            return;
        }
        if (knownPlugins.getNumTypes() == 0)
        {
            startPluginScan();
            return;
        }

        std::vector<PluginDescription> list;
        for (const auto& d : knownPlugins.getTypes())
            if (kind == 0 ? d.isInstrument : ! d.isInstrument)
                list.push_back (d);
        std::sort (list.begin(), list.end(), [] (const PluginDescription& a, const PluginDescription& b)
                   { return a.name.compareIgnoreCase (b.name) < 0; });


        // η λίστα εμφανίζεται μέσα στην οθόνη των loops (στη θέση τους): διάλεξε όνομα, ↻ νέα σάρωση, … αρχείο, ✕ κλείσιμο
        StringArray names, formats, keys;
        for (const auto& d : list)
        {
            names.add (d.name);
            formats.add (d.pluginFormatName);
            keys.add (d.createIdentifierString());
        }

        const Colour kc = kind == 0 ? hobby::kOn : kind == 1 ? hobby::kPeach : hobby::kLilac;
        const String kt = kind == 0 ? "VST" : kind == 1 ? "FX" : "MIDI FX";
        const String ks = kind == 0 ? "instruments" : "effects";

        loopsView.getList().open (kt, ks, kc, names, formats,
                                  [safe = SafePointer<MainComponent> (this), list, chosen] (int i)
                                  {
                                      if (safe != nullptr && i >= 0 && i < (int) list.size()) chosen (list[(size_t) i]);
                                  },
                                  [safe = SafePointer<MainComponent> (this)]
                                  {
                                      if (safe == nullptr) return;
                                      safe->knownPlugins.clear();
                                      safe->startPluginScan();
                                  },
                                  [safe = SafePointer<MainComponent> (this), title, chosen]
                                  {
                                      if (safe != nullptr) safe->pickPluginFile (title, chosen);
                                  },
                                  keys, favPlugins,
                                  [safe = SafePointer<MainComponent> (this)] (const StringArray& f)
                                  {
                                      if (safe == nullptr) return;
                                      safe->favPlugins = f;
                                      safe->saveNow();
                                  });
    }

    void pickPluginFile (const String& title, std::function<void (const PluginDescription&)> chosen)
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
                setMessage (gr ("No plugin found in this file."));
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
            setMessage (gr ("No free block (maximum 9)."));
            return;
        }
        pickPlugin (gr ("Choose a plugin"), 0, [this, slot] (const PluginDescription& d)
        {
            loadPlugin (slot, d, MemoryBlock(), true, true, nullptr);
        });
    }

    void replaceInstrument()
    {
        if (! isActive (selected)) return;
        const int slot = selected;
        pickPlugin (gr ("New instrument for block ") + String (slot + 1), 0, [this, slot] (const PluginDescription& d)
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
            setMessage (gr ("Load failed: ") + error);
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
            clearKnobsOf (block);
        }

        stopKnobLearn();
        auto old = engine.swapPlugin (block, std::move (instance), captureZone);
        if (old != nullptr) stateCache.erase (old.get());
        old.reset();

        if (state.getSize() > 0)
            if (auto* p = engine.getPlugin (block))
                p->setStateInformation (state.getData(), (int) state.getSize());

        ensureKnobs (block);

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
                    safe->setMessage (gr ("Load failed: ") + error);
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
        pickPlugin (gr ("Choose a MIDI effect"), 2, [this, block] (const PluginDescription& d)
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

    // target: block 0..8 ή -1 για master
    void addFxTo (int target)
    {
        if (target >= 0 && ! isActive (target)) { setMessage (gr ("Load an instrument (VST) first")); return; }
        if (engine.numFx (target) >= maxFxPerChain) { setMessage (gr ("Maximum 8 effects per chain")); return; }

        pickPlugin (target < 0 ? gr ("Master FX: choose an effect") : gr ("FX: choose an effect"), 1, [this, target] (const PluginDescription& d)
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
        stopKnobLearn();

        auto extras = engine.takeExtras (block);
        for (auto& p : extras) stateCache.erase (p.get());
        extras.clear();

        auto old = engine.swapPlugin (block, nullptr);
        if (old != nullptr) stateCache.erase (old.get());
        old.reset();

        if (engine.specialBlock.load() == block) engine.specialBlock = -1;
        engine.liveSilent[(size_t) block] = 1;
        engine.holdMode[(size_t) block] = 0;
        engine.chordMode[(size_t) block] = 0; engine.chordTonic[(size_t) block] = -1; engine.blockOff[(size_t) block] = 0;
        engine.curveMode[(size_t) block] = 0;
        engine.volPos[(size_t) block] = 1.0f;
        engine.volMap[(size_t) block] = 0;
        engine.transpose[(size_t) block] = 0;
        clearKnobsOf (block);

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
        menu.addItem (1, gr ("\xe2\x9f\xb2"));
        menu.addItem (2, gr ("\xe2\x96\xb2  +1"));
        menu.addItem (3, gr ("\xe2\x96\xbc  -1"));
        menu.addItem (4, gr ("0"));
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

    void toggleSpecial()
    {
        if (! isActive (selected)) return;
        auto& en = engine;
        const int s = selected;
        if (en.specialBlock.load() == s) en.specialBlock = -1;
        else
        {
            en.pressUndo (s);   // το ειδικό block δεν έχει λούπα: αν είχε, σβήνεται
            en.specialBlock = s;
        }
        saveNow();
        refreshAll();
        keyboardView.repaint();
    }

    // (δεν χρησιμοποιείται πια) δεξί κλικ στο Block: ειδικό block του επιλεγμένου block και πετάλι on/off του
    void showBlockMenu()
    {
        const int sb = engine.specialBlock.load();
        const bool ok = isActive (selected);
        const int code = engine.mapCode[(size_t) flow::ctlSpecial].load();

        PopupMenu menu;
        menu.addItem (1, String::fromUTF8 ("\xe2\x98\x85"), ok, sb >= 0 && sb == selected);
        menu.addItem (2, String::fromUTF8 ("\xe2\x97\x8e  \xe2\x98\x85"), sb >= 0);
        menu.addItem (3, String::fromUTF8 ("\xe2\x9c\x95  \xe2\x98\x85"), code != 0);
        if (code != 0)
            menu.addItem (4, gr ("Now: ") + pedalCodeText (code), false);

        menu.showMenuAsync (PopupMenu::Options().withTargetComponent (&blockTile),
                            [safe = SafePointer<MainComponent> (this)] (int result)
        {
            if (safe == nullptr) return;
            if (result == 1)
            {
                auto& en = safe->engine;
                const int s = safe->selected;
                if (en.specialBlock.load() == s) en.specialBlock = -1;
                else
                {
                    en.pressUndo (s);   // το ειδικό block δεν έχει λούπα: αν είχε, σβήνεται
                    en.specialBlock = s;
                }
                safe->saveNow();
                safe->refreshAll();
                safe->keyboardView.repaint();
            }
            if (result == 2) safe->startPedalLearn (flow::ctlSpecial);
            if (result == 3) { safe->engine.mapCode[(size_t) flow::ctlSpecial] = 0; safe->saveNow(); }
        });
    }

    void showVolumeMenu (int b)
    {
        const int code = engine.volMap[(size_t) b].load();

        PopupMenu menu;
        menu.addItem (1, gr ("\xe2\x97\x8e"));
        menu.addItem (2, gr ("\xe2\x9c\x95"), code != 0);
        if (code != 0)
            menu.addItem (3, gr ("Now: ") + pedalCodeText (code), false);

        menu.showMenuAsync (PopupMenu::Options().withTargetComponent (&keyboardView),
                            [safe = SafePointer<MainComponent> (this), b] (int result)
        {
            if (safe == nullptr) return;
            if (result == 1)
            {
                safe->engine.startVolumeLearn (b);
                safe->setMessage (gr ("Move the volume pedal for block ") + String (b + 1));
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
        startMenu.addItem (10, gr ("\xe2\x8a\x98"), true, sq == 0);
        startMenu.addItem (11, gr ("\xe2\x99\xa9"), true, sq == 1);
        startMenu.addItem (12, gr ("\xe2\x99\xa9\xe2\x99\xa9\xe2\x99\xa9\xe2\x99\xa9"), true, sq == 2);
        lenMenu.addItem (20, gr ("\xe2\x8a\x98"), true, lq == 0);
        lenMenu.addItem (21, gr ("\xe2\x99\xa9"), true, lq == 1);
        lenMenu.addItem (22, gr ("\xe2\x99\xa9\xe2\x99\xa9\xe2\x99\xa9\xe2\x99\xa9"), true, lq == 2);
        snapMenu.addItem (30, gr ("\xe2\x8a\x98"), true, nq == 0);
        snapMenu.addItem (31, "1/8", true, nq == 1);
        snapMenu.addItem (32, "1/16", true, nq == 2);

        PopupMenu menu;
        menu.addSubMenu (gr ("\xe2\x96\xb6|"), startMenu);
        menu.addSubMenu (gr ("\xe2\x86\x94"), lenMenu);
        menu.addSubMenu (gr ("\xe2\x99\xaf"), snapMenu);
        PopupMenu autoMenu;
        autoMenu.addItem (70, gr ("\xe2\x8a\x98"), true, engine.autoLoop.load() == 0);
        autoMenu.addItem (71, gr ("A"), true, engine.autoLoop.load() == 1);
        autoMenu.addItem (72, gr ("A  \xe2\x97\x8f"), true, engine.autoLoop.load() == 2);
        menu.addSubMenu (gr ("A"), autoMenu);
        PopupMenu tapMenu;
        tapMenu.addItem (60, "0.5 s", true, engine.tapMs.load() == 500);
        tapMenu.addItem (61, "0.7 s", true, engine.tapMs.load() == 700);
        tapMenu.addItem (62, "1.0 s", true, engine.tapMs.load() == 1000);
        menu.addSubMenu (gr ("\xe2\x89\x88"), tapMenu);
        menu.addSeparator();
        menu.addItem (40, gr ("\xe2\x97\x8e"));
        menu.addItem (41, gr ("\xe2\x9c\x95"), code != 0);
        if (code != 0) menu.addItem (42, gr ("Now: ") + pedalCodeText (code), false);

        menu.showMenuAsync (PopupMenu::Options().withTargetComponent (&recTile),
                            [safe = SafePointer<MainComponent> (this)] (int r)
        {
            if (safe == nullptr || r == 0) return;
            auto& e = safe->engine;
            if (r >= 10 && r <= 12) e.startQ = r - 10;
            if (r >= 20 && r <= 22) e.lengthQ = r - 20;
            if (r >= 30 && r <= 32) e.snapQ = r - 30;
            if (r >= 70 && r <= 72) e.autoLoop = r - 70;
            if (r >= 60 && r <= 62) e.tapMs = r == 60 ? 500 : r == 61 ? 700 : 1000;
            if (r == 40) safe->startPedalLearn (flow::ctlRec);
            if (r == 41) e.mapCode[flow::ctlRec] = 0;
            safe->saveNow();
        });
    }

    void showMetroMenu()
    {
        const int code = engine.mapCode[flow::ctlMetro].load();
        const int outs = engine.numOutChannels.load();
        const int cur = engine.metroChannel.load();

        PopupMenu outMenu;
        for (int c = 0; c < outs; ++c)
            outMenu.addItem (100 + c, gr ("Output ") + String (c + 1), true, c == cur);

        PopupMenu menu;
        menu.addItem (1, gr ("\xe2\x99\xaa"));
        menu.addItem (2, gr ("\xe2\x86\xba"), metroSamplePath.isNotEmpty());
        menu.addSubMenu (gr ("\xe2\x87\xa8"), outMenu);
        menu.addSeparator();
        menu.addItem (40, gr ("\xe2\x97\x8e"));
        menu.addItem (41, gr ("\xe2\x9c\x95"), code != 0);
        if (code != 0) menu.addItem (42, gr ("Now: ") + pedalCodeText (code), false);

        menu.showMenuAsync (PopupMenu::Options().withTargetComponent (&metroTile),
                            [safe = SafePointer<MainComponent> (this)] (int r)
        {
            if (safe == nullptr || r == 0) return;
            auto& e = safe->engine;
            if (r == 1) safe->chooseMetroSample();
            if (r == 2)
            {
                safe->metroSamplePath = {};
                e.setMetroSample (AudioBuffer<float>());
            }
            if (r >= 100) e.metroChannel = r - 100;
            if (r == 40) safe->startPedalLearn (flow::ctlMetro);
            if (r == 41) e.mapCode[flow::ctlMetro] = 0;
            safe->saveNow();
        });
    }

    // μενού μέσα στην οθόνη (κουμπί φακέλου)
    String currentSetName;

    // μικρογραφία set: η εικόνα του orb μέσα στο αρχείο, με cache ανά αρχείο και ώρα τροποποίησης
    std::map<String, Image> setThumbCache;
    Image setThumbnail (const File& f)
    {
        const String key = f.getFullPathName() + "|" + String (f.getLastModificationTime().toMilliseconds());
        auto it = setThumbCache.find (key);
        if (it != setThumbCache.end()) return it->second;
        Image img;
        if (std::unique_ptr<XmlElement> xml { XmlDocument::parse (f) })
            if (auto* oe = xml->getChildByName ("OrbImage"))
            {
                MemoryOutputStream mo;
                if (Base64::convertFromBase64 (mo, oe->getAllSubText().trim()))
                {
                    auto full = ImageFileFormat::loadFrom (mo.getData(), mo.getDataSize());
                    if (full.isValid())
                    {
                        const int s = jmin (full.getWidth(), full.getHeight());
                        img = full.getClippedImage ({ (full.getWidth() - s) / 2, (full.getHeight() - s) / 2, s, s })
                                  .rescaled (96, 96, Graphics::highResamplingQuality);
                    }
                }
            }
        if (setThumbCache.size() > 200) setThumbCache.clear();
        setThumbCache[key] = img;
        return img;
    }

    void buildScreenMenu()
    {
        auto act = [this] (std::function<void()> f) { return std::function<void()> ([this, f] { f(); saveNow(); }); };
        std::vector<LoopsView::MenuEntry> items;
        auto add = [&items] (const String& glyph, const String& text, std::function<void()> fn, bool enabled = true, bool checked = false)
        {
            LoopsView::MenuEntry m; m.glyph = glyph; m.text = text; m.fn = std::move (fn); m.enabled = enabled; m.checked = checked;
            items.push_back (std::move (m));
        };

        add (String::fromUTF8 ("\xe2\x99\xaa"), gr ("Audio & MIDI settings\xe2\x80\xa6"), act ([this] { openAudioSettings(); }));
        add (String::fromUTF8 ("\xe2\x86\x93"), gr ("Save set\xe2\x80\xa6"), act ([this] { saveSetToFile(); }));
        add (String::fromUTF8 ("\xe2\x86\x91"), gr ("Load set\xe2\x80\xa6"), act ([this] { loadSetFromFile(); }));

        add (String::fromUTF8 ("\xe2\x9f\xb2"), gr ("Restore last session"), act ([this] { restoreLastSession(); }), hasSessionBackup());
        add (String::fromUTF8 ("\xe2\x96\xad"), gr ("Rec Audio folder\xe2\x80\xa6"), act ([this] { chooseTapeFolder(); }));

        {
            LoopsView::MenuEntry m; m.glyph = "v"; m.symbol = (int) Symbol::Speaker; m.text = gr ("Master volume");
            const float pos = engine.masterPos.load();
            const float steps[4] { 1.0f, 0.85f, 0.7f, 0.5f };
            for (int i = 0; i < 4; ++i)
            {
                LoopsView::MenuEntry sj; sj.text = String ((int) (steps[i] * 100)) + "%"; sj.checked = std::abs (pos - steps[i]) < 0.05f;
                const float v = steps[i];
                sj.fn = [this, v] { engine.masterPos = v; saveNow(); };
                m.sub.push_back (std::move (sj));
            }
            { LoopsView::MenuEntry sj; sj.text = gr ("Safety limiter"); sj.checked = engine.limiterOn.load() != 0;
              sj.fn = [this] { engine.limiterOn = engine.limiterOn.load() != 0 ? 0 : 1; saveNow(); }; m.sub.push_back (std::move (sj)); }
            { LoopsView::MenuEntry sj; sj.text = gr ("Assign knob (MIDI learn)");
              sj.fn = [this] { engine.cancelLearns(); engine.masterLearn = 1; setMessage (gr ("Move the knob for the master volume")); }; m.sub.push_back (std::move (sj)); }
            { LoopsView::MenuEntry sj; sj.text = gr ("Remove assignment"); sj.enabled = engine.masterCc.load() != 0;
              sj.fn = [this] { engine.masterCc = 0; saveNow(); }; m.sub.push_back (std::move (sj)); }
            items.push_back (std::move (m));
        }

        add (String::fromUTF8 ("\xe2\x88\x9e"), "Ableton Link" + (LinkBridge::isAvailable() ? String() : gr (" (not included in this build)")),
             act ([this]
             {
                 const bool on = ! engine.isLinkEnabled();
                 engine.setLinkEnabled (on);
                 setMessage (on ? gr ("Ableton Link: on") : gr ("Ableton Link: off"));
             }), LinkBridge::isAvailable(), engine.isLinkEnabled());
        add (String::fromUTF8 ("\xe2\x97\x89"), gr ("iPad orb (Safari)"),
             act ([this]
             {
                 if (webOrbOn) stopWebOrb(); else startWebOrb();
                 setMessage (webOrbOn ? gr ("iPad orb: on") : gr ("iPad orb: off"));
             }), true, webOrbOn);
        add (String::fromUTF8 ("\xe2\x96\xa3"), gr ("Orb images folder\xe2\x80\xa6"), act ([this] { chooseOrbFolder(); }));
        add ("!", "Panic", act ([this] { doPanic(); }));

        {
            Array<File> files = setsDir().findChildFiles (File::findFiles, false, "*.flowset");
            struct Newest { static int compareElements (const File& a, const File& b)
            { return a.getLastModificationTime() < b.getLastModificationTime() ? 1 : (b.getLastModificationTime() < a.getLastModificationTime() ? -1 : 0); } };
            Newest sorter;
            files.sort (sorter);
            LoopsView::MenuEntry m; m.glyph = String::fromUTF8 ("\xe2\x96\xa4"); m.text = gr ("My sets"); m.isSets = true;
            items.insert (items.begin(), std::move (m));

            std::vector<LoopsView::SetItem> setItems;
            for (auto& f : files)
            {
                LoopsView::SetItem si;
                si.name = f.getFileNameWithoutExtension();
                si.date = f.getLastModificationTime().formatted ("%d %b %Y");
                si.current = si.name == currentSetName;
                si.thumb = setThumbnail (f);
                si.fn = [this, f] { loadSetFile (f); saveNow(); };
                setItems.push_back (std::move (si));
            }
            loopsView.setSetItems (std::move (setItems));
        }

        loopsView.setMenu (std::move (items));
    }

    std::vector<std::pair<String, String>> menuInfoRows()
    {
        std::vector<std::pair<String, String>> r;
        r.push_back ({ "Set", currentSetName.isNotEmpty() ? currentSetName : String::fromUTF8 ("\xe2\x80\x93") });
        r.push_back ({ "Tempo", String ((int) std::lround (engine.tempo.load())) + " BPM" });
        if (auto* dev = deviceManager.getCurrentAudioDevice())
        {
            const double rate = dev->getCurrentSampleRate();
            r.push_back ({ "Audio", dev->getName() });
            r.push_back ({ {}, String (rate / 1000.0, 1) + " kHz  \xc2\xb7  " + String (dev->getCurrentBufferSizeSamples()) + " samples  \xc2\xb7  "
                                 + String (1000.0 * (double) dev->getOutputLatencyInSamples() / jmax (1.0, rate), 1) + " ms" });
        }
        else r.push_back ({ "Audio", gr ("none") });
        {
            StringArray names;
            for (auto& d : MidiInput::getAvailableDevices())
                if (deviceManager.isMidiInputDeviceEnabled (d.identifier)) names.add (d.name);
            r.push_back ({ "MIDI", names.isEmpty() ? gr ("none") : names.joinIntoString (", ") });
        }
        r.push_back ({ "Master", String ((int) std::lround (engine.masterPos.load() * 100.0f)) + "%  \xc2\xb7  limiter " + (engine.limiterOn.load() != 0 ? "on" : "off") });
        r.push_back ({ "Link", engine.isLinkEnabled() ? "on" : "off" });
        {
            auto folder = engine.recorder.getFolder().getFullPathName();
            const auto home = File::getSpecialLocation (File::userHomeDirectory).getFullPathName();
            if (home.isNotEmpty() && folder.startsWith (home)) folder = "~" + folder.substring (home.length());
            r.push_back ({ "Rec Audio", folder });
        }
        r.push_back ({ "Key lights", keyLights.isOn() ? "on" : "off" });
        if (webOrbOn) r.push_back ({ "iPad orb", webOrbUrl() + "  \xc2\xb7  " + String (webServer.getNumClients()) + " connected" });
        return r;
    }

    void showPiMenu()
    {
        const int pcode = engine.mapCode[flow::ctlPanic].load();
        PopupMenu menu;
        menu.addItem (1, gr ("Audio & MIDI settings…"));
        menu.addSeparator();
        menu.addItem (2, gr ("Save set…"));
        menu.addItem (3, gr ("Load set…"));
        menu.addItem (9, gr ("Restore last session"), hasSessionBackup());

        // λίστα με τα αποθηκευμένα sets (νεότερα πρώτα)
        Array<File> setFiles = setsDir().findChildFiles (File::findFiles, false, "*.flowset");
        struct NewestFirst { static int compareElements (const File& a, const File& b)
        { return a.getLastModificationTime() < b.getLastModificationTime() ? 1 : (b.getLastModificationTime() < a.getLastModificationTime() ? -1 : 0); } };
        NewestFirst sorter;
        setFiles.sort (sorter);
        {
            PopupMenu setsMenu;
            for (int i = 0; i < setFiles.size() && i < 40; ++i)
                setsMenu.addItem (1000 + i, setFiles[i].getFileNameWithoutExtension());
            if (setFiles.isEmpty()) setsMenu.addItem (999, gr ("(none)"), false);
            menu.addSubMenu (gr ("My sets"), setsMenu);
        }

        // master: ένταση, limiter
        {
            const float pos = engine.masterPos.load();
            const float stepsPos[4] { 1.0f, 0.85f, 0.7f, 0.5f };
            PopupMenu mm;
            for (int i = 0; i < 4; ++i)
                mm.addItem (20 + i, String ((int) (stepsPos[i] * 100)) + "%", true, std::abs (pos - stepsPos[i]) < 0.05f);
            mm.addSeparator();
            mm.addItem (26, gr ("Safety limiter"), true, engine.limiterOn.load() != 0);
            mm.addItem (24, gr ("Assign knob (MIDI learn)"));
            mm.addItem (25, gr ("Remove assignment"), engine.masterCc.load() != 0);
            if (engine.masterCc.load() != 0) mm.addItem (27, gr ("Now: ") + pedalCodeText (engine.masterCc.load()), false);
            menu.addSubMenu (gr ("Master volume"), mm);
        }
        menu.addSeparator();
        menu.addItem (4, gr ("Panic: silence stuck notes (Esc)"));
        menu.addItem (5, gr ("Panic: assign pedal or key (MIDI learn)"));
        menu.addItem (6, gr ("Panic: remove assignment"), pcode != 0);
        if (pcode != 0) menu.addItem (7, gr ("Panic now: ") + pedalCodeText (pcode), false);
        menu.addSeparator();
        menu.addItem (8, "Ableton Link" + (LinkBridge::isAvailable() ? String() : gr (" (not included in this build)")),
                      LinkBridge::isAvailable(), engine.isLinkEnabled());

        const Rectangle<int> area (getScreenX() + getWidth() / 2 - 40, getScreenY() + 46, 80, 28);
        menu.showMenuAsync (PopupMenu::Options().withTargetScreenArea (area),
                            [safe = SafePointer<MainComponent> (this), setFiles] (int r)
        {
            if (safe == nullptr || r == 0) return;
            if (r >= 1000 && r - 1000 < setFiles.size()) safe->loadSetFile (setFiles[r - 1000]);
            if (r >= 20 && r <= 23) { const float v[4] { 1.0f, 0.85f, 0.7f, 0.5f }; safe->engine.masterPos = v[r - 20]; }
            if (r == 26) safe->engine.limiterOn = safe->engine.limiterOn.load() != 0 ? 0 : 1;
            if (r == 24) { safe->engine.cancelLearns(); safe->engine.masterLearn = 1; safe->setMessage (gr ("Move the knob for the master volume")); }
            if (r == 25) safe->engine.masterCc = 0;
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
                safe->setMessage (on ? gr ("Ableton Link: on") : gr ("Ableton Link: off"));
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
            setMessage (gr ("Metronome: output ") + String (engine.metroChannel.load() + 1)
                        + gr (" does not exist. Open π / - and enable more outputs"));
            return;
        }
        engine.metroOn = ! engine.metroOn.load();
    }

    void chooseMetroSample()
    {
        chooser = std::make_unique<FileChooser> (gr ("Metronome sound (wav, aiff, mp3...)"),
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
            if (report) setMessage (gr ("Could not read this audio file"));
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
        options.dialogTitle = gr ("Audio & MIDI settings");
        options.componentToCentreAround = this;
        options.dialogBackgroundColour = Colours::darkgrey;
        options.escapeKeyTriggersCloseButton = true;
        options.useNativeTitleBar = true;
        options.resizable = false;
        options.launchAsync();
    }

    void chooseTapeFolder()
    {
        chooser = std::make_unique<FileChooser> (gr ("Folder for Rec Audio (zip)"), engine.recorder.getFolder());

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
        root->setAttribute ("version", 2);   // 1: 16 blocks (έως 0.36), 2: 9 blocks
        root->setAttribute ("tempo", engine.tempo.load());
        root->setAttribute ("startQ", engine.startQ.load());
        root->setAttribute ("lengthQ", engine.lengthQ.load());
        root->setAttribute ("snapQ", engine.snapQ.load());
        root->setAttribute ("tapMs", engine.tapMs.load());
        root->setAttribute ("autoLoop", engine.autoLoop.load());
        root->setAttribute ("tempoManual", engine.tempoUserSet.load() ? 1 : 0);
        root->setAttribute ("selected", selected);
        if (orbImage.isValid())   // η εικόνα του orb σώζεται μέσα στο set (μικρογραφία JPEG)
        {
            MemoryOutputStream mo;
            JPEGImageFormat jf; jf.setQuality (0.82f);
            if (jf.writeImageToStream (orbImage, mo))
                root->createNewChildElement ("OrbImage")->addTextElement (Base64::toBase64 (mo.getData(), mo.getDataSize()));
        }
        for (int b = 0; b < flow::numBlocks; ++b)
        {
            if (! isActive (b)) continue;
            auto* e = root->createNewChildElement ("Block");
            {
                auto* oe = e->createNewChildElement ("Orb");
                writeKnobs (*oe, blockKnobs[(size_t) b]);
                // Rec motion: 1024 δείγματα ανά κύκλο, x και y ως int16 (±1 → ±32767), base64
                for (int c = 0; c < 4; ++c)
                {
                    const auto& l = orbLane[(size_t) b][(size_t) c];
                    if (! l.has || l.recording || (int) l.s.size() != orbN) continue;
                    MemoryBlock mb ((size_t) orbN * 4);
                    auto* q = static_cast<int16*> (mb.getData());
                    for (int i = 0; i < orbN; ++i)
                    {
                        q[2 * i]     = (int16) std::lround (jlimit (-1.0f, 1.0f, l.s[(size_t) i].x) * 32767.0f);
                        q[2 * i + 1] = (int16) std::lround (jlimit (-1.0f, 1.0f, l.s[(size_t) i].y) * 32767.0f);
                    }
                    auto* me = oe->createNewChildElement ("Motion");
                    me->setAttribute ("c", c);
                    me->addTextElement (Base64::toBase64 (mb.getData(), mb.getSize()));
                }
            }
            e->setAttribute ("slot", b);
            e->setAttribute ("lo", engine.lo[(size_t) b].load());
            e->setAttribute ("hi", engine.hi[(size_t) b].load());
            e->setAttribute ("seq", engine.zoneSeq[(size_t) b].load());
            e->setAttribute ("zone", engine.zoneState[(size_t) b].load() == 0 ? 1 : 0);
            e->setAttribute ("hold", engine.holdMode[(size_t) b].load());
            e->setAttribute ("al", engine.getAlText (b));
            e->setAttribute ("curve", engine.curveMode[(size_t) b].load());
            e->setAttribute ("off", engine.blockOff[(size_t) b].load());
            e->setAttribute ("special", engine.specialBlock.load() == b ? 1 : 0);
            e->setAttribute ("lsilent", engine.liveSilent[(size_t) b].load());
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

    int setMoved = 0, setDropped = 0, setNewer = 0;   // αποτέλεσμα του τελευταίου applySet

    void applySet (const XmlElement& root, const StringArray& crashed)
    {
        clearEverything();
        {
            Image img;
            if (auto* oe = root.getChildByName ("OrbImage"))
            {
                MemoryOutputStream mo;
                if (Base64::convertFromBase64 (mo, oe->getAllSubText().trim()))
                    img = ImageFileFormat::loadFrom (mo.getData(), mo.getDataSize());
            }
            if (img.isValid()) setOrbImage (img, false); else { setOrbImage ({}, false); autoOrbImage(); }
        }

        // Έκδοση αρχείου: τα παλιά sets (16 blocks) μετατρέπονται, όσα blocks είναι σε θέσεις πάνω από το 9 πάνε στις ελεύθερες θέσεις
        setNewer = root.getIntAttribute ("version", 1) > 2 ? 1 : 0;
        setMoved = setDropped = 0;
        std::map<int, int> slotMap;
        {
            std::array<bool, (size_t) flow::numBlocks> used {};
            for (auto* e : root.getChildWithTagNameIterator ("Block"))
            {
                const int sl = e->getIntAttribute ("slot", -1);
                if (sl >= 0 && sl < flow::numBlocks) used[(size_t) sl] = true;
            }
            for (auto* e : root.getChildWithTagNameIterator ("Block"))
            {
                const int sl = e->getIntAttribute ("slot", -1);
                if (sl < flow::numBlocks) continue;
                int freeSlot = -1;
                for (int i = 0; i < flow::numBlocks; ++i)
                    if (! used[(size_t) i]) { freeSlot = i; break; }
                if (freeSlot < 0) { ++setDropped; continue; }
                used[(size_t) freeSlot] = true;
                slotMap[sl] = freeSlot;
                ++setMoved;
            }
        }
        auto mapSlot = [&slotMap] (int sl)
        {
            if (sl >= 0 && sl < flow::numBlocks) return sl;
            auto it = slotMap.find (sl);
            return it != slotMap.end() ? it->second : -1;
        };

        engine.tempo = jlimit (40.0, 240.0, root.getDoubleAttribute ("tempo", 110.0));
        engine.startQ = jlimit (0, 2, root.getIntAttribute ("startQ", 1));
        engine.lengthQ = jlimit (0, 2, root.getIntAttribute ("lengthQ", 2));
        engine.snapQ = jlimit (0, 2, root.getIntAttribute ("snapQ", 2));
        engine.tapMs = jlimit (300, 1500, root.getIntAttribute ("tapMs", 500));
        engine.autoLoop = jlimit (0, 2, root.getIntAttribute ("autoLoop", 0));
        engine.tempoUserSet = root.getIntAttribute ("tempoManual", engine.tempo.load() != 110.0 ? 1 : 0) != 0;
        pendingSelect = jmax (0, mapSlot (root.getIntAttribute ("selected", 0)));

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
                    const int bs = mapSlot (be->getIntAttribute ("s", -1));
                    if (bs < 0) continue;
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

        engine.specialBlock = -1;
        for (auto* e : root.getChildWithTagNameIterator ("Block"))
        {
            const int origSlot = e->getIntAttribute ("slot", -1);
            const int slot = mapSlot (origSlot);
            if (slot < 0) continue;

            auto inst = readPluginEntry (e->getChildByName ("Instrument"));
            if (! inst.ok) continue;

            if (crashed.contains ("b" + String (origSlot)))
            {
                setMessage (gr ("A plugin did not load correctly last time and was skipped."));
                continue;
            }

            if (auto* ke = e->getChildByName ("Orb"))
            {
                readKnobs (*ke, blockKnobs[(size_t) slot]);
                for (auto* me : ke->getChildWithTagNameIterator ("Motion"))
                {
                    const int c = me->getIntAttribute ("c", -1);
                    if (c < 0 || c > 3) continue;
                    MemoryOutputStream mo;
                    if (! Base64::convertFromBase64 (mo, me->getAllSubText().trim()) || mo.getDataSize() != (size_t) orbN * 4) continue;
                    const auto* q = static_cast<const int16*> (mo.getData());
                    auto& l = orbLane[(size_t) slot][(size_t) c];
                    l.s.assign ((size_t) orbN, {});
                    l.ok.assign ((size_t) orbN, 1);
                    for (int i = 0; i < orbN; ++i) l.s[(size_t) i] = { (float) q[2 * i] / 32767.0f, (float) q[2 * i + 1] / 32767.0f };
                    l.has = true; l.recording = false;
                }
            }

            engine.holdMode[(size_t) slot]  = jlimit (0, 4, e->getIntAttribute ("hold", 0));
            engine.setAlText (slot, e->getStringAttribute ("al"));
            engine.curveMode[(size_t) slot] = jlimit (0, flow::numCurves - 1, e->getIntAttribute ("curve", 0));
            engine.blockOff[(size_t) slot]   = jlimit (0, 1, e->getIntAttribute ("off", 0));
            if (e->getIntAttribute ("special", 0) != 0) engine.specialBlock = slot;
            engine.liveSilent[(size_t) slot] = jlimit (0, 1, e->getIntAttribute ("lsilent", 1));
            engine.chordMode[(size_t) slot]  = 0;   // το Chord δεν χρησιμοποιείται πια
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
        chooser = std::make_unique<FileChooser> (gr ("Save set"), dir.getChildFile ("Set.flowset"), "*.flowset");
        chooser->launchAsync (FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles | FileBrowserComponent::warnAboutOverwriting,
                              [this] (const FileChooser& fc)
        {
            auto f = fc.getResult();
            if (f == File()) return;
            f = f.withFileExtension ("flowset");
            auto xml = createSetXml (true, true);
            if (xml->writeTo (f)) { currentSetName = f.getFileNameWithoutExtension(); setMessage (gr ("Saved: ") + f.getFileName()); }
            else                  setMessage (gr ("Could not write the file"));
        });
    }

    File setsDir() const
    {
        return File::getSpecialLocation (File::userMusicDirectory).getChildFile ("Flow").getChildFile ("Sets");
    }

    void loadSetFile (const File& f)
    {
        std::unique_ptr<XmlElement> xml (XmlDocument::parse (f));
        if (xml == nullptr || ! xml->hasTagName ("FlowSet"))
        {
            setMessage (gr ("This is not a Flow set"));
            return;
        }
        applySet (*xml, {});
        currentSetName = f.getFileNameWithoutExtension();
        if (setNewer != 0)         setMessage (gr ("This set is from a newer Flow, some things may be missing"));
        else if (setDropped > 0)   setMessage (gr ("Loaded, but ") + String (setDropped) + gr (" blocks did not fit (maximum 9)"));
        else if (setMoved > 0)     setMessage (gr ("Loaded: ") + String (setMoved) + gr (" blocks were moved to free slots"));
        else                       setMessage (gr ("Loaded: ") + f.getFileName());
    }

    void loadSetFromFile()
    {
        auto dir = setsDir();
        chooser = std::make_unique<FileChooser> (gr ("Load set"), dir, "*.flowset");
        chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles, [this] (const FileChooser& fc)
        {
            auto f = fc.getResult();
            if (f == File()) return;
            loadSetFile (f);
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
        settings->setValue ("favPlugins", favPlugins.joinIntoString ("\n"));
        settings->setValue ("masterPos", (double) engine.masterPos.load());
        settings->setValue ("limiterOn", engine.limiterOn.load());
        settings->setValue ("masterCc", engine.masterCc.load());
        settings->setValue ("metroChannel", engine.metroChannel.load());
        settings->setValue ("metroSample", metroSamplePath);
        settings->setValue ("link", engine.isLinkEnabled());
        settings->setValue ("keyLights", lightsWanted);
        settings->setValue ("orbFolder", orbFolderPath);
        settings->setValue ("webOrbOn", webOrbOn);
        settings->setValue ("webOrbToken", webOrbToken);

        for (int i = 0; i < flow::numControls; ++i)
            settings->setValue ("ctl" + String (i), engine.mapCode[(size_t) i].load());
        for (int k = 0; k < 8; ++k)
            settings->setValue ("knobcc" + String (k), engine.knobCc[(size_t) k].load());
        {
            XmlElement lib ("OrbLib");
            for (auto& kv : knobLibrary)
            {
                auto* pe = lib.createNewChildElement ("Plugin");
                pe->setAttribute ("key", kv.first);
                writeKnobs (*pe, kv.second);
            }
            settings->setValue ("orbLibrary", &lib);
        }

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
        engine.tempo = jlimit (40.0, 240.0, xml->getDoubleAttribute ("tempo", 110.0));
        saveNow();
    }

    //==========================================================================
    static const char* controlName (int i)
    {
        static const char* const names[flow::numControls] = { "Hold", "Rec MIDI loop", "Delete loop", "Mute loop", "Rec Audio", "Metronome", "Panic", "Previous scene", "Next scene", "Mute", "Special block on/off" };
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
        menu.addItem (1, gr ("\xe2\x97\x8e"));
        menu.addItem (2, gr ("\xe2\x9c\x95"), code != 0);
        if (code != 0)
            menu.addItem (3, gr ("Now: ") + pedalCodeText (code), false);

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
            case flow::ctlSpecial: return &blockTile;
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
            setMessage (gr ("Press a free key or pedal for: ") + controlName (i));
        else
            setMessage (gr ("Press the pedal for: ") + controlName (i));
    }

    void setMessage (const String& m) { message = m; messageTicks = 60; }

    void refreshAll()
    {
        const int sel = selected;
        const bool valid = isActive (sel);
        const double t = Time::getMillisecondCounterHiRes() * 0.001;

        // κουμπιά plugin: πράσινο = αυτό που δείχνει τώρα ο editor. Τελείες: υπάρχει plugin / πόσα FX
        const bool hasMfx = valid && engine.getMidiFx (sel) != nullptr;
        const int nfx = valid ? engine.numFx (sel) : 0;
        // πράσινο = υπάρχει (block / VST / MIDI FX / FX στο επιλεγμένο block), άσπρο = δεν υπάρχει
        const Colour tileWhite (0xfff3f4f6);
        blockTile.setIconColour (firstActive() >= 0 ? hobby::kOn : tileWhite);
        vstTile.setIconColour (valid ? hobby::kOn : tileWhite);
        mfxTile.setIconColour (hasMfx ? hobby::kOn : tileWhite);
        mfxTile.setDots (hasMfx ? 1 : 0);
        fxTile.setIconColour (nfx > 0 ? hobby::kOn : tileWhite);
        fxTile.setDots (jmin (nfx, 3));

        const int hold = valid ? engine.holdMode[(size_t) sel].load() : 0;
        chordTile.setDots (0);
        chordTile.setIconColour (hold == 3 ? hobby::mix (hobby::kIconGrey, Colour (0xffe8883a), hobby::pulse (t * 1.5))
                                           : hold == 4 ? Colour (0xffe8883a) : hobby::kIconGrey);
        chordTile.setTooltip (gr ("AL (selected block): press = Capture (play a chord, then a free key: the key becomes a key switch, orange, and you can make several assignments) / press again = AL plays / again = Off. Now: ")
                              + (hold == 3 ? "Capture" : hold == 4 ? "AL" : "Off"));

        holdTile.setDots (hold);
        holdTile.setIconColour (hold == 0 ? hobby::kIconGrey : hobby::kOn);
        holdTile.setTooltip (gr ("Hold (selected block): press = Off / LM Mono Latch (one note) / LP Poly Latch (chord). Now: ")
                             + (hold == 0 ? "Off" : hold == 1 ? "LM" : hold == 2 ? "LP" : hold == 3 ? "AL Capture" : "AL"));

        const int loop = valid ? engine.loopStateOut[(size_t) sel].load() : (int) flow::loopOff;
        const int mute = valid ? engine.loopMuteOut[(size_t) sel].load() : 0;
        const int taps = engine.tapsMuteOut.load();

        const int recBlock = valid ? sel + 1 : 0;
        Colour recColour = hobby::kIconGrey;
        bool recFilled = false;
        if (loop == flow::loopRec)       { recColour = hobby::mix (hobby::kIconGrey, hobby::kRed, hobby::pulse (t * 1.5)); recFilled = true; }
        else if (loop == flow::loopPlay) { recColour = hobby::kOn; recFilled = true; }
        else if (loop == flow::loopOver) { recColour = hobby::mix (hobby::kIconGrey, hobby::kPeach, hobby::pulse (t * 1.5)); recFilled = true; }
        recTile.setNumber (recBlock, recFilled);
        recTile.setIconColour (recColour);

        undoTile.setIconColour (engine.tapsDelOut.load() > 0 ? hobby::kCoral : loop != flow::loopOff ? hobby::kBlue : hobby::kIconGrey);

        stopTile.setIconColour (loop != flow::loopOff ? hobby::kBlue : hobby::kIconGrey);
        juce::ignoreUnused (mute, taps);

        switch (engine.recorder.getState())
        {
            case RecorderEngine::State::Recording: tapeTile.setIconColour (hobby::mix (hobby::kIconGrey, hobby::kRed, 0.5f + 0.5f * hobby::pulse (t * 1.0))); break;
            case RecorderEngine::State::Stopped:   tapeTile.setIconColour (engine.recorder.isTapeFull() ? hobby::kCoral : hobby::kPeach); break;
            case RecorderEngine::State::Error:     tapeTile.setIconColour (hobby::kCoral); break;
            default:                               tapeTile.setIconColour (hobby::kIconGrey); break;
        }

        {
            // ο μετρονόμος αναβοσβήνει σε κάθε beat όταν ο ήχος του είναι ανοιχτός
            const double ph = engine.beatPosOut.load();
            const float flash = jlimit (0.0f, 1.0f, 1.0f - (float) (ph - std::floor (ph)) * 4.0f);
            metroTile.setIconColour (engine.metroOn.load() ? hobby::mix (hobby::kIconGrey, hobby::kOn, 0.3f + 0.7f * flash) : hobby::kIconGrey);
        }

        // κεφαλίδα πάνελ
        const bool master = panelMode == panelMaster;
        String title;
        if (master)            title = "MASTER FX";
        else if (! valid)      title = "FLOW";
        else                   title = "BLOCK " + String (sel + 1);
        header.title = title;
        header.chordName = (valid && ! master) ? KeyboardView::chordNameFor (engine.pcMaskOut[(size_t) sel].load(), engine.bassOut[(size_t) sel].load()) : String();
        const int stopTaps = engine.tapsMuteOut.load();
        const int delTaps = engine.tapsDelOut.load();
        if (message.isNotEmpty())  { header.notice = message; header.noticeColour = hobby::kCoral; }
        else if (stopTaps > 0)     { header.notice = stopTaps == 1 ? String ("LOOP OFF") : String ("LOOP ON"); header.noticeColour = hobby::kLilac; }
        else if (delTaps > 0)      { header.notice = delTaps == 1 ? String ("RE-REC") : String ("DEL"); header.noticeColour = hobby::kCoral; }
        else if (valid && engine.autoLoop.load() == 2 && engine.autoReadyOut[(size_t) sel].load() != 0) { header.notice = "REC = LOOP"; header.noticeColour = hobby::kLilac; }
        else                       header.notice = String();
        header.accent = valid ? blockLight (sel) : hobby::kIconGrey;
        header.masterMode = master;
        header.numCircles = master ? engine.numFx (-1) : (valid ? engine.numFx (sel) : 0);
        header.activeCircle = (master || panelMode == panelFx) ? panelIndex : -1;
        header.canAdd = (master || valid) && header.numCircles < maxFxPerChain;
        cpuSmooth = 0.85f * cpuSmooth + 0.15f * (float) deviceManager.getCpuUsage();

        // μετρήσεις ήχου: γράφονται στο Flow-log (σημαντικά γεγονότα αμέσως, αλλιώς σύνοψη κάθε 60 s)
        if (++statTicks >= 300)
        {
            statTicks = 0;
            ++statWindows;
            const int calls = engine.statCalls.exchange (0);
            const int over = engine.statOver.exchange (0), full = engine.statOverFull.exchange (0);
            const float maxCb = engine.statMaxCb.exchange (0.0f), maxLock = engine.statMaxLock.exchange (0.0f);
            const float budget = engine.statBudget.load();
            int xr = -1;
            if (auto* dev = deviceManager.getCurrentAudioDevice()) xr = dev->getXRunCount();
            const int xrDelta = (xr >= 0 && lastXRuns >= 0) ? xr - lastXRuns : 0;
            lastXRuns = xr;
            const bool notable = full > 0 || xrDelta > 0 || maxLock > 0.002f || maxCb > 0.6f * budget;
            if (calls > 0 && (notable || statWindows % 6 == 0))
                flow::flowLog (String::formatted ("audio: buffer %d, callbacks %d, max %.2f ms of %.2f ms, >80%%: %d, >100%%: %d, max lock wait %.2f ms, xruns +%d",
                                                  currentBufferSize(), calls, maxCb * 1000.0f, budget * 1000.0f, over, full, maxLock * 1000.0f, xrDelta));
        }
        header.cpu = cpuSmooth;
        header.repaint();
        loopsView.title = header.title; loopsView.chordName = header.chordName; loopsView.notice = header.notice;
        loopsView.noticeColour = header.noticeColour; loopsView.cpu = cpuSmooth;
        updateToolLamps();
        knobTick();

        loopsView.repaint();
        keyboardView.repaint();
    }

    int lastFollowed = -1;
    void timerCallback() override
    {
        // focus: το block που έπαιξες τελευταίο γίνεται αυτόματα το επιλεγμένο (οθόνη, κουμπιά λούπας, πετάλια)
        {
            const int lp = engine.lastPlayed.load();
            if (lp != lastFollowed)
            {
                lastFollowed = lp;
                if (lp >= 0 && lp != selected && isActive (lp) && lp != engine.specialBlock.load())
                {
                    selected = lp;
                    engine.selectedBlock = lp;
                    updateEditor();
                    refreshAll();
                }
            }
        }

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

        if (lightsWanted && ! keyLights.isOn() && ! lightsTried) { lightsTried = true; setKeyLights (true); }
        updateLights (false);

        const int mc = engine.midiCount.load();
        if (mc != lastMidiCount) { lastMidiCount = mc; lastMidiMs = Time::getMillisecondCounter(); }
        header.midiActive = Time::getMillisecondCounter() - lastMidiMs < 250;
        loopsView.midiActive = header.midiActive;

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
    KnownPluginList knownPlugins;
    std::unique_ptr<PluginDirectoryScanner> scanner;
    int scanFormatIndex = 0;
    bool scanning = false;
    struct ScanTimer : public Timer
    {
        std::function<void()> f;
        void timerCallback() override { if (f) f(); }
    } scanTimer;
    bool docked = true;
    Component* dockedTop = nullptr;
    Point<int> lastWinPos;
    Rectangle<int> lastTopBounds;
    struct DockTimer : public Timer
    {
        std::function<void()> f;
        void timerCallback() override { if (f) f(); }
    } dockTimer;
    std::unique_ptr<FileChooser> chooser;

    LoopsView loopsView;
    KeyboardView keyboardView;
    PanelHeader header;
    ValueTile blockTile, vstTile, mfxTile, fxTile, holdTile, chordTile, motionTile, recTile, undoTile, stopTile, tapeTile, metroTile, folderTile;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

//==============================================================================
class FlowApplication : public JUCEApplication
{
public:
    const String getApplicationName() override    { return "Flow"; }
    const String getApplicationVersion() override { return "0.48.5"; }
    bool moreThanOneInstanceAllowed() override    { return false; }

    void initialise (const String&) override
    {
        flow::flowLog ("Flow " + getApplicationVersion() + " started");
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
