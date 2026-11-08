// Flow - master keyboard host για VST3 / AU plugins.
// Prisma Audio

#include "Views.h"
#include "KeyLights.h"
#include "KeyScreens.h"
#include "WadaPalettes.h"
#include "FlowLogo.h"
#include "WindowSnap.h"
#include <algorithm>
#include "AudioPanel.h"
#include "SettingsScreen.h"
#include "PluginTypes.h"
#include "RoleCard.h"
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
          undoTile (Symbol::Cancel, typeface),
          stopTile (Symbol::Stop, typeface),
          tapeTile (Symbol::Tape, typeface),
          metroTile (Symbol::Tempo, typeface),
          folderTile (Symbol::Menu, typeface),
          smartTile (Symbol::Rings, typeface),
          motionTile (Symbol::Orb, typeface)
    {
        PropertiesFile::Options options;
        options.applicationName     = "Flow";
        options.filenameSuffix      = ".settings";
        options.osxLibrarySubFolder = "Application Support";
        properties.setStorageParameters (options);
        if (auto* us = properties.getUserSettings())
        {
            helpOn = us->getBoolValue ("helpOn", true);
            if (! helpOn) tooltipWindow.setMillisecondsBeforeTipAppears (2000000000);
        }

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
        folderTile.setTooltip (gr ("Settings: sets, audio and MIDI, master volume, Rec Audio folder, Link, Panic"));
        recTile.setTooltip ("Rec MIDI loop: " + gr ("records the loop of the selected block (the number inside the circle). Press again = end. If the block already has a loop, it starts an overdub (up to 2 layers); press again to end it. Right-click: quantize and pedal"));
        undoTile.setTooltip (gr ("Delete loop: one press deletes the loop of the selected block, a quick double press deletes only the last overdub"));
        stopTile.setTooltip ("Rec MIDI loop: " + gr ("stop / play the loop of the selected block. Right-click: mute pedal"));
        tapeTile.setTooltip (gr ("Play all loops. Quick double press = stop all. Rec Audio: key A#0"));
        metroTile.setTooltip (gr ("Metronome (M key). Plays in mono on a separate output. Right-click: sound, output, key/pedal"));

        addAndMakeVisible (loopsView);
        addAndMakeVisible (keyboardView);
        addAndMakeVisible (header);
        addChildComponent (audioPanel);
        audioPanel.onChanged = [this] { saveNow(); };
        audioPanel.onMessage = [this] (const String& m) { setMessage (m); };
        audioPanel.onClose = [this]
        {
            if (audioFromSettings) { audioFromSettings = false; settingsScreen.setBounds (loopsBezel.reduced (5)); settingsScreen.open(); settingsScreen.grabKeyboardFocus(); }
            repaint();
        };
        addChildComponent (settingsScreen);
        setupSettingsScreen();

        // Sound Palette: ζάρι (Palette, έρχεται αργότερα) και κάμερα (κράτα τον ήχο του επιλεγμένου block)
        addAndMakeVisible (paletteTile);
        addChildComponent (roleCard);
        paletteTile.diceTip = gr ("Sound Palette: first press fills blocks 1-7 from the sounds you kept; one press = new sound in the selected block; double press = new sounds in all blocks not kept");
        paletteTile.cameraTip = gr ("Keep sound: saves the sound of the selected block (instrument and MIDI FX, without audio FX) with a label");
        paletteTile.onDice = [this] { onPaletteDice(); };
        paletteTile.onCamera = [this] { onPaletteCamera(); };
        roleCard.onPick = [this] (int role) { roleCard.close(); saveSnapshot (snapshotBlock, role); grabKeyboardFocus(); };
        roleCard.onCancel = [this] { roleCard.close(); grabKeyboardFocus(); };
        soundCount = countSounds();
        paletteTile.setDiceOn (soundCount > 0);

        blockTile.onClick = [this] { if (learnClick (flow::ctlSpecial)) return; startZoneFirst(); };   // zones first: το Block ορίζει πάντα πρώτα τη ζώνη, μετά το VST βάζει plugin στη ζώνη
        vstTile.onClick = [this] { onVstClick(); };
        mfxTile.onClick = [this] { onMidiFxClick(); };
        fxTile.onClick = [this] { onFxClick(); };

        holdTile.onClick = [this] { cycleHold (selected); };
        motionTile.onClick = [this] { toggleOrbRec(); };
        motionTile.setTooltip (gr ("Rec motion: press, then drag a circle in the orb. The movement is recorded over 16 bars and loops; you can record over it again."));
        recTile.onClick  = [this]
        {
            if (learnClick (flow::ctlRec)) return;
            const int b = engine.recTarget (false);
            if (b < 0) return;
            if (engine.loopStateOut[(size_t) b].load() == flow::loopPlay && engine.loopLayersOut[(size_t) b].load() >= flow::maxOverdubLayers) return;   // μέχρι 3 στρώματα
            engine.pressMain (b);
        };
        undoTile.onClick = [this] { if (learnClick (flow::ctlUndo)) return; if (isActive (selected)) engine.pressUndo (selected); };
        stopTile.onClick = [this] { if (learnClick (flow::ctlStop)) return; if (isActive (selected)) engine.pressStop (selected); };
        tapeTile.onClick = [this] { if (learnClick (flow::ctlTape)) return; engine.pressPlayStop(); };
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
        folderTile.onClick = [this] { if (settingsScreen.isVisible()) settingsScreen.close(); else openSettingsScreen(); refreshSmartTile(); };
        smartTile.setTooltip (gr ("Smart master: level of six bands and stereo width of every block, with suggestions by sound type (third screen)"));
        smartTile.onClick = [this]
        {
            if (settingsScreen.isSmartDirect()) { settingsScreen.close(); return; }
            loopsView.openMenu (false);
            if (audioPanel.isVisible()) audioPanel.close();
            settingsScreen.setBounds (loopsBezel.reduced (5));
            settingsScreen.openSmart();
            settingsScreen.grabKeyboardFocus();
            refreshSmartTile();
        };
        loopsView.onMenuOpen = [this] { buildScreenMenu(); loopsView.setMenuInfo (menuInfoRows()); };

        loopsView.onSelect = [this] (int b) { selectBlock (b); };
        loopsView.onSmartCycle = [this]
        {
            engine.smartOn = engine.smartOn.load() == 0 ? 1 : 0;   // Off ↔ On (πάντα Auto)
            saveNow(); refreshSmartTile(); loopsView.repaint();
        };
        loopsView.onSmartType = [this] (int b)
        {
            engine.smartType[(size_t) b] = (engine.smartType[(size_t) b].load() + 1) % 9;
            if (engine.smartType[(size_t) b].load() == 6) engine.followBlock[(size_t) b] = 0;   // Drums: δεν ακολουθούν (μπορείς να το ξανανάψεις στις ρυθμίσεις)
            engine.smartResetApp (b); rememberSmartType (b, engine.smartType[(size_t) b].load());
            saveNow(); loopsView.repaint();
        };
        loopsView.onFollowToggle = [this] { engine.toggleFollow(); saveNow(); refreshSmartTile(); loopsView.repaint(); };
        loopsView.getTempoBox().onChange = [this] { dirty = true; };
        keyboardView.onSelect = [this] (int b) { selectBlock (b); };
        keyboardView.onRedoZone = [this] (int b) { selectBlock (b); engine.startZone (b); refreshAll(); };   // ⟲ στη σειρά του block: ξαναορίζει τη ζώνη
        keyboardView.onToggleFollowBlock = [this] (int b)
        {
            // κύκλος: off → on → poly (πολυφωνική μετάθεση) → off
            if (engine.followBlock[(size_t) b].load() == 0) { engine.followBlock[(size_t) b] = 1; engine.followPolyBlock[(size_t) b] = 0; }
            else if (engine.followPolyBlock[(size_t) b].load() == 0) engine.followPolyBlock[(size_t) b] = 1;
            else { engine.followBlock[(size_t) b] = 0; engine.followPolyBlock[(size_t) b] = 0; }
            saveNow(); keyboardView.repaint();
        };
        keyboardView.onToggleMf = [this] (int b) { engine.mfBlock = engine.mfBlock.load() == b ? -1 : b; saveNow(); refreshAll(); keyboardView.repaint(); };
        keyboardView.onHold = [this] (int b) { selectBlock (b); cycleHold (b); };
        keyboardView.onRemoveBlock = [this] (int b) { removeBlock (b); };
        keyboardView.onSwapInstrument = [this] (int b) { selectBlock (b); replaceInstrument(); };
        keyboardView.onToggleSpecialBlock = [this] (int b) { selectBlock (b); toggleSpecial(); };
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
        keyboardView.onSpecialZone = [this] (int sb, int a, int c, bool fin)
        {
            if (! engine.isSp (sb)) return;
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
        auOn = settings->getBoolValue ("auOn", false);
        pluginOff = StringArray::fromLines (settings->getValue ("pluginOff"));
        for (int i = 0; i < flow::Engine::numPedals; ++i)
            engine.pedalCc[(size_t) i] = settings->getIntValue ("pedalcc" + String (i), 20 + i);
        for (int b = 0; b < flow::numBlocks; ++b)
            for (int c = 0; c < 4; ++c) { orbPos[(size_t) b][(size_t) c] = orbDefault (c); orbAutoPat[(size_t) b][(size_t) c] = -1; }
        loopsView.onOrbAssign = [this] (int slot) { startKnobLearn (slot); };
        loopsView.onOrbRing = [this] (int c) { orbAutoPress (c); };
        loopsView.onOrbClear = [this] (int slot) { clearKnobSlot (slot); };
        loopsView.onOrbMove = [this] (int corner, Point<float> p) { orbMoved (corner, p); };
        loopsView.onOrbTouch = [this] (int corner, bool down) { orbTouch (corner, down); };
        loopsView.onOrbClearMotion = [this] { orbClearMotion(); };
        loopsView.onOrbImage = [this] { pickOrbImage(); };
        loopsView.onOrbMode = [this] { repaint(); };
        loopsView.onOrbImageClear = [this] { setOrbImage ({}); };

        windowSizeMode = jlimit (0, 3, settings->getIntValue ("windowSize", 1));
        autoOrbImage();   // νέο (άδειο) set: τυχαία εικόνα από το φάκελο Music/Flow/Orb, αν υπάρχει
        loopsView.setOrbMode (false);   // η κύρια οθόνη δείχνει πάντα: loop του block, orb, Smart + Follow

        const String savedFolder = settings->getValue ("tapeFolder");
        engine.recorder.setFolder (savedFolder.isNotEmpty() ? File (savedFolder)
                                                            : File::getSpecialLocation (File::userMusicDirectory).getChildFile ("Flow"));

        engine.metroOn = false;
        engine.metroLevel = (float) jlimit (0.0, 1.0, settings->getDoubleValue ("metroLevel", 0.8));
        engine.metroChannel = jmax (0, settings->getIntValue ("metroChannel", 2));
        favPlugins.addLines (settings->getValue ("favPlugins"));
        favPlugins.removeEmptyStrings();
        engine.masterPos = 1.0f;   // το master volume είναι πάντα 100%
        engine.followSetScale (settings->getIntValue ("followRoot", 9), settings->getIntValue ("followType", 1));
        engine.followAuto = settings->getIntValue ("followAuto", 1);
        engine.smartOn = settings->getIntValue ("smartOn", 0) != 0 ? 1 : 0;
        engine.smartAccept = jlimit (0.0f, 1.0f, (float) settings->getDoubleValue ("smartAccept", 0.6));
        engine.masterCc = 0;
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
        orbTimer.f = [this] { orbPhysics(); };
        orbTimer.startTimerHz (60);
    }

    ~MainComponent() override
    {
        if (keyScreens.isOn()) submitS88Logo();   // το Flow κλείνει: μένει το λογότυπο στις οθόνες (το stop() της keyScreens το στέλνει πριν κλείσει)
        stopKnobLearn();
        lightsPump.stopTimer();
        stopTimer();
        dockTimer.stopTimer();
        orbTimer.stopTimer();
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
        hobby::drawEmbossedText (g, typeface, "flow", (float) sideM + 8.0f);

        hobby::drawBezel (g, loopsBezel.toFloat(), loopsBezel.reduced (5).toFloat(), hobby::kScreen, 18.0f);
        hobby::drawBezel (g, keysBezel.toFloat(), keysBezel.reduced (5).toFloat(), hobby::kScreen, 18.0f);

        for (auto* t : tiles())
        {
            auto r = t->getBounds().toFloat();
            if (t == &recTile || t == &undoTile || t == &stopTile || t == &tapeTile)
            {
                Path p;
                p.addEllipse (r);
                hobby::drawShadowCached (g, DropShadow (Colours::black.withAlpha (0.55f), 4, { 0, 3 }), p, 2);
            }
            else
            {
                hobby::drawTileShadow (g, r);
            }
        }
        hobby::drawTileShadow (g, paletteTile.getBounds().toFloat());
    }

    void resized() override
    {
        if (getWidth() <= 0 || getHeight() <= 0) return;
        layoutFor (getWidth(), getHeight());
        placeChildren();
    }

    void mouseDown (const MouseEvent&) override { grabKeyboardFocus(); }

    bool keyPressed (const KeyPress& k) override
    {
        if (k.getModifiers().isAnyModifierKeyDown()) return false;
        const auto c = k.getTextCharacter();
        if (k == KeyPress::escapeKey) { if (knobStage != 0) { stopKnobLearn(); setMessage (gr ("Assignment cancelled")); flow::flowLog ("orb learn: cancelled (Esc)"); if (settingsScreen.isVisible()) settingsScreen.repaint(); return true; } if (learnMode != 0) { learnMode = 0; stopKnobLearn(); setMessage (gr ("MIDI learn cancelled")); updateToolLamps(); return true; } if (settingsScreen.isVisible()) { settingsScreen.goBack(); return true; } if (audioPanel.isVisible()) { audioPanel.close(); return true; } doPanic(); return true; }
        return false;
    }

private:
    //==========================================================================
    std::array<ValueTile*, 10> tiles()   // ο μετρονόμος δεν υπάρχει πια: το δαχτυλίδι του loop δείχνει τον ρυθμό
    {
        return { &blockTile, &vstTile, &mfxTile, &fxTile, &motionTile, &recTile, &undoTile, &stopTile, &tapeTile, &folderTile };
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
        if (b != -1 && ! isActive (b) && ! isEmptyZone (b)) return;
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
    static constexpr int minLoopsH = 230, stripH = 72, keysH = 120, minContentW = 800, defaultLoopsH = 340;
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
        audioPanel.setBounds (loopsBezel.reduced (5));
        settingsScreen.setBounds (loopsBezel.reduced (5));

        header.setBounds (pluginBezel.getX() + 5, pluginBezel.getY(), pluginBezel.getWidth() - 10, headerH);
        hobby::kBrandY = (float) pluginBezel.getY() + (float) headerH * 0.5f - 8.0f;

        // τρεις ομάδες: αριστερά 5 τετράγωνα (Block, VST, MIDI FX, FX, Rec motion). Στη μέση τα 4 στρογγυλά (στο κέντρο του ελεύθερου χώρου). Δεξιά μετρονόμος και ρυθμίσεις.
        const auto strip = stripBezel.reduced (5);
        const int ty = strip.getY() + 5;
        const int leftX = strip.getX() + 14;
        const int midW = 4 * 52 + 3 * 8;
        const int rightX = strip.getRight() - 14 - 52;
        const int palW = 2 * 52 + 8;   // κουμπί Sound Palette: δύο μισά
        const int leftEnd = leftX + 5 * 60, rightStart = rightX - 8 - palW;
        const int midX = leftEnd + jmax (4, (rightStart - leftEnd - midW) / 2);

        int x = leftX;
        for (auto* t : { &blockTile, &vstTile, &mfxTile, &fxTile, &motionTile })
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
        folderTile.setBounds (rightX, ty, 52, 52);
        paletteTile.setBounds (rightX - 8 - palW, ty, palW, 52);
        roleCard.setBounds (loopsBezel.reduced (5));
    }

    void refreshSize()
    {
        if (getWidth() <= 0 || getHeight() <= 0)
        {
            // προεπιλεγμένο μέγεθος: 1097 x 782 (όπως το ρύθμισε ο χρήστης), μικρότερο μόνο αν δεν χωράει στην οθόνη
            int w = 1097, h = 782;
            if (auto* d = Desktop::getInstance().getDisplays().getPrimaryDisplay())
            {
                w = jmin (w, d->userArea.getWidth() - 40);
                h = jmin (h, d->userArea.getHeight() - 28 - 40);
            }
            setSize (jmax (minWinW(), w), jmax (defaultWinH(), h));
        }

        applyWindowSize (false);

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

        // δεύτερη οθόνη: το παράθυρο μένει στο κέντρο της οθόνης ενώ το plugin αλλάζει μέγεθος (τα πρώτα δευτερόλεπτα)
        void keepCentredOn (Point<int> centre) { centreOn = centre; centreUntil = Time::getMillisecondCounter() + 4000; }
        void resized() override
        {
            DocumentWindow::resized();
            if (centreUntil != 0 && Time::getMillisecondCounter() < centreUntil && ! inRecentre)
            {
                inRecentre = true;
                setCentrePosition (centreOn);
                inRecentre = false;
            }
        }
        Point<int> centreOn;
        uint32 centreUntil = 0;
        bool inRecentre = false;

        MainComponent& owner;
        AudioPluginInstance* plugin;
        String posKey;
    };

    // μικρογραφίες plugin: αποθηκεύονται όταν ανοίγει το παράθυρό του (Music/Flow/thumbs) και φαίνονται στη λίστα VST
    static File thumbFile (const String& pluginName)
    {
        return File::getSpecialLocation (File::userMusicDirectory).getChildFile ("Flow").getChildFile ("thumbs")
                   .getChildFile (File::createLegalFileName (pluginName) + ".png");
    }
    std::map<String, Image> thumbCache;
    // μαύρη (ή σχεδόν μαύρη) εικόνα = αποτυχημένο στιγμιότυπο
    static bool isBlankImage (const Image& im)
    {
        if (! im.isValid()) return true;
        double sum = 0.0; int n = 0;
        for (int y = 2; y < im.getHeight(); y += 4)
            for (int x = 2; x < im.getWidth(); x += 4)
            {
                const auto c = im.getPixelAt (x, y);
                sum += (double) (c.getRed() + c.getGreen() + c.getBlue()) / 3.0; ++n;
            }
        return n == 0 || sum / (double) n < 10.0;
    }
    Image thumbFor (const String& name)
    {
        auto it = thumbCache.find (name);
        if (it != thumbCache.end()) return it->second;
        Image im;
        const auto f = thumbFile (name);
        if (f.existsAsFile()) im = ImageFileFormat::loadFrom (f);
        if (isBlankImage (im)) im = Image();   // παλιά μαύρα αρχεία αγνοούνται (και ξαναγράφονται)
        thumbCache[name] = im;
        return im;
    }
    void saveThumbOf (AudioPluginInstance* p, int attempt = 0)
    {
        auto it = pluginWindows.find (p);
        if (it == pluginWindows.end() || it->second == nullptr || ! it->second->isShowing()) return;
        const auto f = thumbFile (p->getName());
        if (f.existsAsFile()) { const auto old = ImageFileFormat::loadFrom (f); if (! isBlankImage (old) && old.getWidth() >= 480) return; }   // μία φορά ανά plugin· τα παλιά μικρά (288 px) ξανασώζονται σε 480 px για τις οθόνες του S88
        auto* content = it->second->getContentComponent();
        if (content == nullptr || content->getWidth() < 40 || content->getHeight() < 40) return;

        // 1) στιγμιότυπο του παραθύρου από το σύστημα (βλέπει το native GUI του plugin)· 2) αλλιώς της JUCE
        Image full;
        {
            Image win = flowCaptureWindow (it->second->getName());
            if (win.isValid() && it->second->getWidth() > 0)
            {
                const float sc = (float) win.getWidth() / (float) it->second->getWidth();
                const int ch = jmin (win.getHeight(), (int) std::lround ((float) content->getHeight() * sc));
                full = win.getClippedImage ({ 0, win.getHeight() - ch, win.getWidth(), ch });   // το περιεχόμενο είναι κάτω από τη γραμμή τίτλου
            }
        }
        if (isBlankImage (full)) full = content->createComponentSnapshot (content->getLocalBounds(), true, 1.0f);

        if (isBlankImage (full))   // το GUI δεν έχει ζωγραφιστεί ακόμη: ξαναπροσπαθεί λίγο αργότερα
        {
            if (attempt < 4)
                Timer::callAfterDelay (3000, [safe = SafePointer<MainComponent> (this), p, attempt] { if (safe != nullptr) safe->saveThumbOf (p, attempt + 1); });
            flow::flowLog ("thumb: blank snapshot of " + p->getName() + " (try " + String (attempt) + ")");
            return;
        }
        const int tw = 480, th = 320;   // 3:2, όπως η θέση στη λίστα· 480 px ώστε να είναι καθαρό στις οθόνες του S88
        const float want = (float) tw / (float) th;
        int cw = full.getWidth(), chh = (int) std::lround ((float) cw / want);
        if (chh > full.getHeight()) { chh = full.getHeight(); cw = (int) std::lround ((float) chh * want); }
        Image sq = full.getClippedImage ({ (full.getWidth() - cw) / 2, 0, cw, chh }).rescaled (tw, th, Graphics::highResamplingQuality);
        f.getParentDirectory().createDirectory();
        FileOutputStream os (f);
        if (os.openedOk()) { os.setPosition (0); os.truncate(); PNGImageFormat().writeImageToStream (sq, os); }
        thumbCache.erase (p->getName());
        flow::flowLog ("thumb: saved " + p->getName());
    }

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
        flow::flowLog ("openWindowFor: hasEditor=" + String (p->hasEditor() ? 1 : 0) + ", creating the editor");
        AudioProcessorEditor* e = p->hasEditor() ? p->createEditorIfNeeded() : nullptr;
        flow::flowLog ("openWindowFor: editor " + String (e != nullptr ? "created" : "not available (generic editor)"));
        if (e == nullptr)
            e = new GenericAudioProcessorEditor (*p);

        // ένα παράθυρο plugin τη φορά: το νέο αντικαθιστά το προηγούμενο και κολλάει στη δεξιά πλευρά του Flow
        for (auto& kv : pluginWindows)
            rememberWindowPos (*kv.second);
        pluginWindows.clear();

        const String key = "pwpos_" + p->getName();
        flow::flowLog ("openWindowFor: creating the window");
        auto w = std::make_unique<PluginWindow> (*this, p, windowTitle (p), e, key);
        flow::flowLog ("openWindowFor: window created");

        docked = false;   // ελεύθερο παράθυρο
        {
            // Αν υπάρχει δεύτερη οθόνη (Sidecar / iPad), ανοίγει centered εκεί
            const auto& displays = Desktop::getInstance().getDisplays();
            const Displays::Display* secondary = nullptr;
            for (int i = 0; i < displays.displays.size(); ++i)
            {
                const auto& d = displays.displays.getReference (i);
                if (! d.isMain) { secondary = &d; break; }
            }
            if (secondary != nullptr)
            {
                // Centered στη δεύτερη οθόνη (χωρίς αποθηκευμένη θέση)
                auto area = secondary->userArea;
                int cx = area.getCentreX() - w->getWidth()  / 2;
                int cy = area.getCentreY() - w->getHeight() / 2;
                cx = jlimit (area.getX(), jmax (area.getX(), area.getRight()  - w->getWidth()),  cx);
                cy = jlimit (area.getY(), jmax (area.getY(), area.getBottom() - w->getHeight()), cy);
                w->setTopLeftPosition (cx, cy);
                w->keepCentredOn (area.getCentre());
                flow::flowLog ("openWindowFor: secondary display at " + String (cx) + "," + String (cy));
            }
            else
            {
                // Μία οθόνη: θέση από προηγούμενη χρήση ή δεξιά από το Flow
                const String saved = properties.getUserSettings()->getValue (key);
                const auto parts = StringArray::fromTokens (saved, " ", "");
                Point<int> at = dockPos (*w);
                if (parts.size() == 2)
                {
                    at = { parts[0].getIntValue(), parts[1].getIntValue() };
                    const auto total = displays.getTotalBounds (true);
                    if (! total.contains (at)) at = dockPos (*w);
                }
                w->setTopLeftPosition (at);
            }
        }
        flow::flowLog ("openWindowFor: showing the window");
        w->setVisible (true);
        w->toFront (true);
        if (w->centreUntil != 0) w->setCentrePosition (w->centreOn);
        lastWinPos = w->getPosition();
        if (auto* top = getTopLevelComponent()) lastTopBounds = top->getBounds();
        flow::flowLog ("dock: opened at " + String (lastWinPos.x) + "," + String (lastWinPos.y) + " (Flow " + String (lastTopBounds.getX()) + "," + String (lastTopBounds.getY()) + " " + String (lastTopBounds.getWidth()) + "x" + String (lastTopBounds.getHeight()) + ")");
        pluginWindows[p] = std::move (w);
        Timer::callAfterDelay (2500, [safe = SafePointer<MainComponent> (this), p] { if (safe != nullptr) safe->saveThumbOf (p); });
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
            chooseAndLoadPlugin (true);
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
    KeyScreens keyScreens;
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
        const int mc = engine.midiCount.load() + engine.loopEvtCount.load();
        const uint32 now = Time::getMillisecondCounter();
        if (mc != pumpMc) { pumpMc = mc; pumpBurstUntil = now + 1200; }
        // όσο παίζει ή γράφεται λούπα σε special block, τα φώτα ξαναγράφονται συνεχώς (το plugin του μπορεί να ανάβει τα δικά του)
        for (int sb : { engine.specialBlock.load(), engine.specialBlock2.load() })
            if (sb >= 0 && sb < flow::numBlocks && engine.loopStateOut[(size_t) sb].load() != flow::loopOff) { pumpBurstUntil = now + 300; break; }
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

    // Χειριστήρια του S88 MK2 (καταγραφή HID). 35–42 = τα 8 κουμπιά πάνω από την οθόνη → τα κουμπιά του Flow (5 αριστερά + 3 δεξιά).
    // Encoder στη λίστα plugins: περιστροφή = γραμμή, κλίση πάνω/κάτω = instruments/favorites, αριστερά/δεξιά = στήλη, πάτημα = επιλογή.
    void handleS88Control (int ctl, int v)
    {
        if (ctl >= 35 && ctl <= 42 && v != 0) flow::flowLog ("S88 button " + String (ctl) + " pressed (pending block " + String (engine.pendingBlock()) + ", selected " + String (selected) + ")");
        auto& list = loopsView.getList();
        const bool listOpen = list.isVisible();
        if (ctl == 100)   // περιστροφή: + = δεξιά
        {
            if (listOpen) list.encoderStep (v > 0 ? 1 : -1);
            else if (s88RingsOpen) { s88Ring = jlimit (0, 3, s88Ring + (v > 0 ? 1 : -1)); s88Slot = 0; }
            else if (engine.pendingBlock() < 0 && s88BlockVisible() && ! s88MacrosOpen)
            {
                if (s88AsEdit) { engine.setAsPct (selected, jlimit (0, 100, engine.asPct[(size_t) selected].load() + (v > 0 ? 5 : -5))); saveNow(); refreshAll(); }
                else s88MenuSel = jlimit (0, 12, s88MenuSel + (v > 0 ? 1 : -1));
                s88BlockTouch();
            }   // μενού block: περιστροφή = επόμενο κυκλάκι   // Orb Rings: περιστροφή = άλλο ring, από την πρώτη θέση
            return;
        }
        if (v == 0) return;   // μόνο πάτημα
        if (ctl == 10) { keyScreens.reclaim(); return; }   // κουμπί MIDI του S88: ξαναπαίρνει τις οθόνες αν τις έχει πάρει το Komplete Kontrol
        if (listOpen)
        {
            switch (ctl)
            {
                case 1: list.encoderPush(); return;                 // πάτημα encoder
                case 2: list.encoderShowAll (true); return;         // πάνω = instruments
                case 3: list.encoderShowAll (false); return;        // κάτω = favorites
                case 4: list.encoderStep (-3); return;              // αριστερά = 5 πίσω (γρήγορη μετακίνηση)
                case 5: list.encoderStep (3); return;               // δεξιά = 5 μπροστά
                default: break;
            }
        }
        if (s88RingsOpen && ! listOpen)
        {
            switch (ctl)
            {
                case 1: { const int k = s88Ring * 4 + s88Slot; if (knobTarget (k) != nullptr) clearKnobSlot (k); else startKnobLearn (k); return; }   // πάτημα: learn σε άδεια θέση, σβήσιμο σε κατειλημμένη
                case 2: s88Slot = jmax (0, s88Slot - 1); return;
                case 3: s88Slot = jmin (3, s88Slot + 1); return;
                default: break;
            }
        }
        if (! listOpen && ! s88RingsOpen && ! s88MacrosOpen && engine.pendingBlock() < 0 && isActive (selected))
        {
            if (s88BlockVisible())
            {
                switch (ctl)
                {
                    case 1: s88MenuActivate(); s88BlockTouch(); return;
                    case 2: if (s88MenuSel == 1) s88StepCurve (selected, 1); else if (s88MenuSel == 7) s88StepFollow (selected, 1); else if (s88MenuSel == 8) s88StepScale (1, false); else if (s88MenuSel == 9) s88StepRepeat (selected, 1, false); else if (s88MenuSel == 10) s88StepAs (selected, 1); else if (s88MenuSel == 2 && keyboardView.onTranspose) keyboardView.onTranspose (selected, 1); s88BlockTouch(); return;    // transpose επιλεγμένο: κλίση πάνω = +1 οκτάβα
                    case 3: if (s88MenuSel == 1) s88StepCurve (selected, -1); else if (s88MenuSel == 7) s88StepFollow (selected, -1); else if (s88MenuSel == 8) s88StepScale (-1, false); else if (s88MenuSel == 9) s88StepRepeat (selected, -1, false); else if (s88MenuSel == 10) s88StepAs (selected, -1); else if (s88MenuSel == 2 && keyboardView.onTranspose) keyboardView.onTranspose (selected, -1); s88BlockTouch(); return;   // κλίση κάτω = −1 οκτάβα
                    case 4: s88AsEdit = false; s88MenuSel = jmax (0, s88MenuSel - 1); s88BlockTouch(); return;
                    case 5: s88AsEdit = false; s88MenuSel = jmin (12, s88MenuSel + 1); s88BlockTouch(); return;
                    default: break;
                }
            }
            else if (ctl == 1) { s88BlockTouch(); return; }   // πάτημα encoder: ανοίγει την οθόνη του block
        }
        if (s88RingsOpen && ctl >= 35 && ctl <= 40 && ctl != 39)   // άλλο κουμπί (Block, VST, MIDI FX, FX, Settings) κλείνει την οθόνη Orb Rings
        {
            s88RingsOpen = false;
            if (knobStage != 0) stopKnobLearn();
        }
        if (s88MacrosOpen && ctl >= 35 && ctl <= 39) s88MacrosOpen = false;   // άλλο κουμπί (Block, VST, MIDI FX, FX, Orb Rings) κλείνει την οθόνη Macros
        switch (ctl)
        {
            case 35: blockTile.triggerClick(); break;
            case 36: vstTile.triggerClick(); break;
            case 37: mfxTile.triggerClick(); break;
            case 38: fxTile.triggerClick(); break;
            case 39: if (! s88RingsOpen) { s88RingsOpen = true; s88Ring = 0; s88Slot = 0; } else motionTile.triggerClick(); break;   // 1ο πάτημα: οθόνη Orb Rings· 2ο: Rec motion (η οθόνη μένει)
            case 40:   // Macros: αριστερά η οθόνη του block, δεξιά τα 8 macros του VST (κουμπιά 1–8). Ξανά πάτημα ή Block τα κλείνει
                if (s88MacrosOpen) { s88MacrosOpen = false; s88BlockTouch(); }
                else if (isActive (selected) && engine.pendingBlock() < 0) { ensureKnobs (selected); syncKnobView(); s88MacrosOpen = true; s88BlockTouch(); }
                break;
            case 41: paletteTile.onDice(); break;
            case 42: paletteTile.onCamera(); break;
            default: break;
        }
    }

    // ---- Οθόνες του S88 MK2 (πείραμα): μετατροπή εικόνας σε 16 bit big-endian (στο S88 το κόκκινο είναι στα ψηλά bits: δοκιμάστηκε από τον χρήστη, το BGR της τεκμηρίωσης έβγαζε κόκκινο↔μπλε ανάποδα) και δοκιμαστικό καρέ με τρεις μπάρες χρώματος για έλεγχο χρωμάτων
    static void toBgr565 (const Image& im, std::vector<uint8_t>& out)
    {
        out.assign ((size_t) KeyScreens::frameBytes, 0);
        Image::BitmapData bd (im, Image::BitmapData::readOnly);
        for (int y = 0; y < KeyScreens::height; ++y)
            for (int x = 0; x < KeyScreens::width; ++x)
            {
                uint8 r8, g8, b8;
                if (bd.pixelFormat == Image::RGB) { const auto* px = reinterpret_cast<const PixelRGB*> (bd.getPixelPointer (x, y)); r8 = px->getRed(); g8 = px->getGreen(); b8 = px->getBlue(); }   // γρήγορη ανάγνωση: η getPixelColour ανά pixel έκανε τα καρέ αργά
                else { const auto c = bd.getPixelColour (x, y); r8 = c.getRed(); g8 = c.getGreen(); b8 = c.getBlue(); }
                const uint16_t v = (uint16_t) (((r8 >> 3) << 11) | ((g8 >> 2) << 5) | (b8 >> 3));
                const size_t i = ((size_t) y * KeyScreens::width + (size_t) x) * 2;
                out[i] = (uint8_t) (v >> 8); out[i + 1] = (uint8_t) (v & 0xff);
            }
    }

    void sendScreenTest()
    {
        const char* labels[2] = { "LEFT screen", "RIGHT screen" };
        for (int s = 0; s < 2; ++s)
        {
            Image im (Image::RGB, KeyScreens::width, KeyScreens::height, true);
            Graphics g (im);
            g.fillAll (Colour (0xff262220));
            const Colour cols[3] = { Colours::red, Colours::green, Colours::blue };
            const char* names[3] = { "RED", "GREEN", "BLUE" };
            for (int i = 0; i < 3; ++i)
            {
                const Rectangle<int> r (20 + i * 150, 90, 140, 110);
                g.setColour (cols[i]); g.fillRoundedRectangle (r.toFloat(), 8.0f);
                g.setColour (Colours::white); g.setFont (Font (FontOptions (22.0f))); g.drawText (names[i], r, Justification::centred);
            }
            g.setColour (Colours::white); g.setFont (Font (FontOptions (30.0f)));
            g.drawText (String ("Flow  -  ") + labels[s], Rectangle<int> (20, 20, 440, 50), Justification::centredLeft);
            g.setFont (Font (FontOptions (16.0f)));
            g.drawText ("S88 MK2 screen test: do the bar colours match their names?", Rectangle<int> (20, 220, 440, 30), Justification::centredLeft);
            std::vector<uint8_t> buf;
            toBgr565 (im, buf);
            keyScreens.submit (s, buf.data());
        }
    }

    // ---- Οθόνες του S88 MK2 (2 × 480×272). Όλες εκτός από το λογότυπο έχουν πάνω μια λεπτή λωρίδα με τις λέξεις των 8 κουμπιών (35–42).
    //  1  λογότυπο: όταν δεν έχει ανοίξει το Flow (στην έναρξη και στο κλείσιμο)
    //  2  σχέδιο (γεννήτρια Bauhaus + Wada), συνεχόμενο στις δύο οθόνες: το Flow είναι ανοιχτό και δεν υπάρχει ακόμη λούπα
    //  3  λίστα plugins ανοιχτή: αριστερά «Block n» και μικρογραφία του block, δεξιά τοίχος 3×2 με μικρογραφίες των VST
    //  4  λούπες: 8 δαχτυλίδια, ένα ανά block (1–8), μόνο στο κάτω μέρος
    static constexpr int s88StripH = 40;
    static constexpr int s88WordShift = -15;   // οι λέξεις πιο αριστερά, ώστε να συμπίπτουν με τα πραγματικά κουμπιά
    Image s88Art;                 // το σχέδιο της οθόνης 2 (960 × 246), νέο σε κάθε σύνδεση των οθονών
    uint32 s88SplashUntil = 0;    // μέχρι πότε μένει το λογότυπο μετά τη σύνδεση

    // κοινά σχήματα των δύο γεννητριών: το σχήμα μπαίνει σε τετράγωνο (x, y, s), σχεδιασμένο σε μία θέση και περιστρεφόμενο κατά r·90°
    // 0 τεταρτοκύκλιο, 1 ημικύκλιο, 2 κύκλος, 3 δαχτυλίδι (η τρύπα παίρνει το χρώμα hole), 4 τρίγωνο, 5 κορυφή, 6 αψίδα, 7 φακός, 8 μπάρα, 9 τετράγωνο
    static bool orbKindIsCurved (int k) { return k <= 3 || k == 6 || k == 7; }

    static int orbPickKind (Random& rnd, int last)
    {
        static const int wt[10] = { 16, 14, 12, 10, 12, 6, 9, 6, 5, 3 };
        for (;;)
        {
            int roll = rnd.nextInt (93), k = 0;
            while (k < 9 && roll >= wt[k]) { roll -= wt[k]; ++k; }
            if (k != last) return k;
        }
    }

    static void orbShape (Graphics& g, int kind, float x, float y, float s, Colour col, Colour hole, int r)
    {
        const float hp = MathConstants<float>::halfPi, pi = MathConstants<float>::pi;
        Path p;
        switch (kind)
        {
            case 0: p.addPieSegment (x - s, y - s, 2.0f * s, 2.0f * s, hp, pi, 0.0f); break;                  // γωνία πάνω-αριστερά
            case 1: p.addPieSegment (x, y, s, s, 0.0f, pi, 0.0f); break;                                      // δεξί μισό του κύκλου
            case 2: p.addEllipse (x, y, s, s); break;
            case 3: p.addEllipse (x, y, s, s); break;                                                         // η τρύπα ζωγραφίζεται μετά
            case 4: p.addTriangle (x, y, x + s, y, x, y + s); break;
            case 5: p.addTriangle (x, y + s, x + s, y + s, x + s * 0.5f, y); break;
            case 6: { const float w = s * 0.6f, x0 = x + (s - w) * 0.5f;
                      p.addRectangle (x0, y + w * 0.5f, w, s - w * 0.5f);
                      p.addPieSegment (x0, y, w, w, -hp, hp, 0.0f); break; }
            case 7: break;                                                                                    // φακός: με clip, παρακάτω
            case 8: p.addRectangle (x + s * 0.36f, y, s * 0.28f, s); break;
            default: p.addRectangle (x, y, s, s); break;
        }
        const auto rot = AffineTransform::rotation ((float) r * hp, x + s * 0.5f, y + s * 0.5f);
        g.setColour (col);
        if (kind == 7)
        {
            Path a, b;
            a.addPieSegment (x - s, y - s, 2.0f * s, 2.0f * s, hp, pi, 0.0f);
            b.addPieSegment (x, y, 2.0f * s, 2.0f * s, 1.5f * pi, 2.0f * pi, 0.0f);
            a.applyTransform (rot); b.applyTransform (rot);
            g.saveState(); g.reduceClipRegion (a); g.fillPath (b); g.restoreState();
            return;
        }
        if (kind != 2 && kind != 3 && kind != 9) p.applyTransform (rot);
        g.fillPath (p);
        if (kind == 3) { g.setColour (hole); const float m = s * 0.28f; g.fillEllipse (x + m, y + m, s - 2.0f * m, s - 2.0f * m); }
    }

    // πλατιά εκδοχή της γεννήτριας για τις οθόνες του hardware: μαύρο φόντο, 2 χρώματα Wada, 2-3 μεγάλα σχήματα σε 4 θέσεις κατά πλάτος
    static Image orbGeneratedWide (Random& rnd, int w, int h)
    {
        int cand[348], nc = 0;
        for (int i = 0; i < 348; ++i) if (wada::kCombos[i].n >= 3) cand[nc++] = i;
        const auto& combo = wada::kCombos[cand[rnd.nextInt (nc)]];
        const int np = combo.n;
        Colour pal[4];
        for (int i = 0; i < np; ++i) pal[i] = Colour ((uint32) (0xff000000u | wada::kColours[combo.idx[i]]));
        const int c0 = rnd.nextInt (np), c1 = (c0 + 1 + rnd.nextInt (np - 1)) % np;
        const Colour use[2] = { pal[c0], pal[c1] };
        Image out (Image::RGB, w, h, true);
        Graphics g (out);
        g.fillAll (Colours::black);
        int slot[4] = { 0, 1, 2, 3 };
        for (int i = 3; i > 0; --i) std::swap (slot[i], slot[rnd.nextInt (i + 1)]);
        const int n = 2 + rnd.nextInt (2);
        const float sw = (float) w / 4.0f;
        int last = -1;
        for (int i = 0; i < n; ++i)
        {
            const int kind = orbPickKind (rnd, last); last = kind;
            const float sz = (float) h * (rnd.nextInt (3) == 2 ? 0.75f : 1.0f);
            const float x = (float) slot[i] * sw + (sw - sz) * 0.5f + (rnd.nextFloat() - 0.5f) * 60.0f;
            const float y = ((float) h - sz) * 0.5f + (rnd.nextFloat() - 0.5f) * 20.0f;
            orbShape (g, kind, x, y, sz, use[rnd.nextInt (2)], Colours::black, rnd.nextInt (4));
        }
        return out;
    }

    void drawS88Strip (Graphics& g, int screen)
    {
        static const char* words[8] = { "Block", "VST", "MIDI FX", "FX", "Orb Rings", "Macros", "Dice", "Keep" };
        g.setColour (Colour (0xff1c1a19).withAlpha (0.75f));   // 75% αδιαφάνεια: το σχέδιο φαίνεται αχνά από πίσω
        g.fillRect (0, 0, KeyScreens::width, s88StripH);
        g.setFont (hobby::makeFont (typeface, 22.0f).boldened());
        for (int i = 0; i < 4; ++i)
        {
            g.setColour (screen * 4 + i == 4 && s88Blink ? hobby::kRed : Colour (0xffe8e4e0));   // Rec motion οπλισμένο: η λέξη Orb Rings αναβοσβήνει κόκκινη
            g.drawText (words[screen * 4 + i], Rectangle<int> (i * 120 + s88WordShift, 0, 120, s88StripH), Justification::centred, false);
            if (screen * 4 + i == 5 && s88MacrosOpen)   // οθόνη Macros ενεργή: πλαίσιο γύρω από τη λέξη
            {
                g.setColour (Colours::white);
                g.drawRoundedRectangle ((float) (i * 120 + s88WordShift + 8), 6.0f, 104.0f, (float) s88StripH - 12.0f, 8.0f, 2.0f);
            }
        }
    }

    // 1: λογότυπο αριστερά, κυκλάκι δεξιά, καθαρό μαύρο
    void drawS88Logo (Graphics& gl, Graphics& gRt)
    {
        gl.fillAll (Colours::black); gRt.fillAll (Colours::black);
        static Image logo = ImageFileFormat::loadFrom (flowlogo::kPng, (size_t) flowlogo::kPngSize);
        if (logo.isValid()) gl.drawImageAt (logo, 240 - logo.getWidth() / 2, 131 - logo.getHeight() / 2);
        gRt.setColour (Colours::white);
        gRt.fillEllipse (240.0f - 23.0f, 130.0f - 23.0f, 46.0f, 46.0f);
    }

    void submitS88Logo()
    {
        Image L (Image::RGB, KeyScreens::width, KeyScreens::height, true), R (Image::RGB, KeyScreens::width, KeyScreens::height, true);
        { Graphics gl (L), gRt (R); drawS88Logo (gl, gRt); }
        std::vector<uint8_t> buf;
        toBgr565 (L, buf); keyScreens.submit (0, buf.data());
        toBgr565 (R, buf); keyScreens.submit (1, buf.data());
    }

    // 2: το σχέδιο συνεχίζεται από την αριστερή οθόνη στη δεξιά (οθαλμαπάτη χωρίς διακοπή)
    void drawS88Art (Graphics& gl, Graphics& gRt)
    {
        gl.fillAll (Colours::black);   // χωρίς σχέδια και χρώματα: καθαρό μαύρο (η λωρίδα με τις λέξεις είναι από πάνω)
        gRt.fillAll (Colours::black);
    }

    // μαύρο φόντο με λεπτό κόκκο (±5, όπως στο Orb): υπόστρωμα για τις οθόνες Set zone, λίστα VST και block
    Image s88ArtDim;
    void drawS88DimArt (Graphics& gl, Graphics& gRt)
    {
        if (! s88ArtDim.isValid())
        {
            s88ArtDim = Image (Image::RGB, 2 * KeyScreens::width, KeyScreens::height, true);   // μαύρο φόντο με λεπτό κόκκο, χωρίς σχέδια
            { Graphics g (s88ArtDim); g.fillAll (Colours::black); }
            Random rnd;
            Image::BitmapData bd (s88ArtDim, Image::BitmapData::readWrite);
            for (int y = 0; y < bd.height; ++y)
                for (int x = 0; x < bd.width; ++x)
                {
                    const auto c = bd.getPixelColour (x, y);
                    const int dn = rnd.nextInt (11) - 5;
                    bd.setPixelColour (x, y, Colour ((uint8) jlimit (0, 255, (int) c.getRed() + dn), (uint8) jlimit (0, 255, (int) c.getGreen() + dn), (uint8) jlimit (0, 255, (int) c.getBlue() + dn)));
                }
        }
        gl.drawImageAt (s88ArtDim, 0, 0);
        gRt.drawImageAt (s88ArtDim, -KeyScreens::width, 0);
    }

    // 3 (αριστερά): όπως η οθόνη Set zone: «Block n» και «Choose a VST» κάτω αριστερά, κάτω δεξιά το snapshot του VST που είναι επιλεγμένο στη λίστα (κενό σκούρο ορθογώνιο αν δεν έχει)
    void drawS88ListLeft (Graphics& g, int b, const Image& snapshot)
    {
        g.setColour (blockLight (b)); g.setFont (hobby::makeFont (typeface, 32.0f).boldened());
        g.drawText ("Block " + String (b + 1), Rectangle<float> (18.0f, 182.0f, 220.0f, 40.0f), Justification::centredLeft, false);
        g.setColour (Colours::white); g.setFont (hobby::makeFont (typeface, 22.0f).boldened());
        g.drawText ("Choose a VST", Rectangle<float> (18.0f, 222.0f, 220.0f, 30.0f), Justification::centredLeft, false);
        const Rectangle<float> tr (240.0f, 136.0f, 226.0f, 122.0f);
        Path clip; clip.addRoundedRectangle (tr, 10.0f);
        g.saveState(); g.reduceClipRegion (clip, AffineTransform());
        g.setColour (Colour (0xff3a3430)); g.fillRect (tr);
        if (snapshot.isValid()) { g.setColour (Colours::white); g.setImageResamplingQuality (Graphics::highResamplingQuality); g.drawImage (snapshot, tr, RectanglePlacement::centred); }
        g.restoreState();
    }

    // 3 (δεξιά): ρολό από πλακίδια με ονόματα, ευθυγραμμισμένα δεξιά· το επιλεγμένο μεγάλο με λευκό περίγραμμα και την κουκκίδα του block, τα γειτονικά στενότερα και πιο αχνά
    void drawS88ListRight (Graphics& g, int b, const std::vector<int>& vis, const StringArray& items, int selK, bool showAll)
    {
        const int total = (int) vis.size();
        const float R = (float) KeyScreens::width - 14.0f, cy = 156.0f;
        struct Sp { float w, h, off, fs, fill, txt; };
        static const Sp sp[3] = { { 300.0f, 58.0f, 0.0f, 32.0f, 1.0f, 1.0f }, { 272.0f, 42.0f, 54.0f, 18.0f, 0.67f, 0.8f }, { 244.0f, 32.0f, 98.0f, 15.0f, 0.39f, 0.55f } };
        for (int d = 2; d >= 0; --d)
            for (int sgn = -1; sgn <= 1; sgn += 2)
            {
                if (d == 0 && sgn > 0) continue;
                const int k = selK + sgn * d;
                if (k < 0 || k >= total) continue;
                const auto& q = sp[d];
                const float yc = cy + (float) sgn * q.off, x0 = R - q.w;
                const Rectangle<float> tile (x0, yc - q.h * 0.5f, q.w, q.h);
                const String& name = items[vis[(size_t) k]];
                if (d == 0)
                {
                    g.setColour (Colour (0xff3a3430)); g.fillRoundedRectangle (tile, 14.0f);
                    g.setColour (Colours::white); g.drawRoundedRectangle (tile.reduced (1.5f), 14.0f, 3.0f);
                    g.setColour (blockLight (b)); g.fillEllipse (x0 + 16.0f, yc - 8.0f, 16.0f, 16.0f);
                    g.setColour (Colours::white); g.setFont (hobby::makeFont (typeface, q.fs).boldened());
                    g.drawText (name, Rectangle<float> (x0 + 44.0f, tile.getY(), q.w - 54.0f, q.h), Justification::centredLeft, true);
                }
                else
                {
                    g.setColour (Colour (0xff3a3430).withAlpha (q.fill)); g.fillRoundedRectangle (tile, 10.0f);
                    g.setColour (Colours::white.withAlpha (q.txt)); g.setFont (hobby::makeFont (typeface, q.fs));
                    g.drawText (name, Rectangle<float> (x0 + 16.0f, tile.getY(), q.w - 26.0f, q.h), Justification::centredLeft, true);
                }
            }
        g.setColour (Colour (0xffb7bbc6)); g.setFont (hobby::makeFont (typeface, 13.0f));
        g.drawText (String (showAll ? "instruments" : "favorites") + String::fromUTF8 ("  \xc2\xb7  ") + String (selK + 1) + " / " + String (total),
                    Rectangle<float> (18.0f, 244.0f, 220.0f, 18.0f), Justification::centredLeft, false);
    }

    String s88Sig;
    bool s88RingsOpen = false, s88Blink = false, s88MacrosOpen = false;
    int s88Ring = 0, s88Slot = 0;   // επιλεγμένο ring (0..3) και θέση παραμέτρου (0..3)
    int s88WallTop = 0;   // πρώτη ορατή σειρά του τοίχου

    // ---- Οθόνη του block (S88): αριστερά αριθμός/range/volume/πλήκτρα, δεξιά το μενού του block με 10 κυκλάκια ----
    int s88MenuSel = 0, s88LastSelKey = -2;
    bool s88AsEdit = false;   // AS: πατημένο encoder → η περιστροφή αλλάζει το ποσοστό ±5%
    uint32 s88BlockUntil = 0, s88RemoveArmUntil = 0;
    bool s88BlockVisible() const { return Time::getMillisecondCounter() < s88BlockUntil && isActive (selected); }
    void s88BlockTouch() { s88BlockUntil = 0xffffffffu; }   // μόνιμη οθόνη: μένει μέχρι να πατηθεί άλλο κουμπί (Orb Rings, VST, MIDI FX, FX…)

    String s88MenuLabel (int i, int b) const
    {
        const bool armed = Time::getMillisecondCounter() < s88RemoveArmUntil;
        const int tr = engine.transpose[(size_t) b].load() / 12;
        switch (i)
        {
            case 0: { const int h = engine.holdMode[(size_t) b].load(); return "Hold " + String::fromUTF8 ("\xc2\xb7 ") + (h == 0 ? "off" : h == 1 ? "LM" : h == 2 ? "LP" : "AL"); }
            case 1: return "Volume curve " + String::fromUTF8 ("\xc2\xb7 ") + String (engine.curveMode[(size_t) b].load() + 1) + "/" + String (flow::numCurves) + "  (tilt up / down)";
            case 2: return "Transpose " + String::fromUTF8 ("\xc2\xb7 ") + (tr > 0 ? "+" : "") + String (tr) + "  (tilt up / down)";
            case 3: return "Live " + String::fromUTF8 ("\xc2\xb7 ") + (engine.liveSilent[(size_t) b].load() != 0 ? "silent" : "sounding");
            case 4: return "Change instrument";
            case 5: return "Special " + String::fromUTF8 ("\xc2\xb7 ") + (engine.isSp (b) ? "on" : "off");
            case 6: return "Master flow " + String::fromUTF8 ("\xc2\xb7 ") + (engine.mfBlock.load() == b ? "on" : "off");
            case 7: return "Follow " + String::fromUTF8 ("\xc2\xb7 ") + (engine.followBlock[(size_t) b].load() == 0 ? "off" : engine.followPolyBlock[(size_t) b].load() != 0 ? "poly" : "on") + "  (tilt up / down)";
            case 8: return "Scale " + String::fromUTF8 ("\xc2\xb7 ") + (engine.followAuto.load() != 0 ? (engine.followAutoFound.load() != 0 ? String ("auto: ") + String (juce::StringArray { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" }[engine.followRoot.load()]) + " " : String ("auto: listening ")) : String()) + (engine.followAuto.load() != 0 && engine.followAutoFound.load() == 0 ? String() : String (engine.followType.load() == 0 ? "major" : "minor")) + "  (all blocks)";
            case 9: { const int rm = engine.repeatMode[(size_t) b].load(); return "Repeat " + String::fromUTF8 ("\xc2\xb7 ") + (rm == 0 ? "loop" : rm == 1 ? "once" : "hold at end") + "  (tilt up / down)"; }
            case 10: return engine.isAs (b) ? "Assistant " + String::fromUTF8 ("\xc2\xb7 ") + (engine.asMode[(size_t) b].load() == 2 ? "style " : "rules ") + String::fromUTF8 ("\xc2\xb7 ") + String (engine.asPct[(size_t) b].load()) + "%" + (s88AsEdit ? "  (rotate: amount, press: done)" : "  (tilt: off / rules / style, press: amount)") : String ("Assistant ") + String::fromUTF8 ("\xc2\xb7 ") + "off  (tilt up: rules)";
            case 11: return "Set the zone again";
            default: return armed ? "Press again to delete" : "Delete block";
        }
    }
    bool s88MenuOn (int i, int b) const
    {
        switch (i)
        {
            case 0: return engine.holdMode[(size_t) b].load() != 0;
            case 2: return engine.transpose[(size_t) b].load() != 0;
            case 3: return engine.liveSilent[(size_t) b].load() != 0;
            case 5: return engine.isSp (b);
            case 6: return engine.mfBlock.load() == b;
            case 7: return engine.followBlock[(size_t) b].load() != 0;
            case 9: return engine.repeatMode[(size_t) b].load() != 0;
            case 10: return engine.isAs (b);
            default: return false;
        }
    }

    // Follow ανά block με κλίση: off ← → on ← → poly (χωρίς περιτύλιγμα)
    void s88StepFollow (int b, int dir)
    {
        if (! isActive (b)) return;
        const int cur = engine.followBlock[(size_t) b].load() == 0 ? 0 : engine.followPolyBlock[(size_t) b].load() != 0 ? 2 : 1;
        const int n = jlimit (0, 2, cur + dir);
        engine.followBlock[(size_t) b] = n == 0 ? 0 : 1;
        engine.followPolyBlock[(size_t) b] = n == 2 ? 1 : 0;
        saveNow();
        refreshAll();
    }

    // κλίμακα των λουπών που ηχογραφείς (Auto / Major / Minor): ισχύει για όλα τα blocks
    void s88StepScale (int dir, bool wrap)
    {
        // καταστάσεις: 0 Auto, 1 major, 2 minor
        const int cur = engine.followAuto.load() != 0 ? 0 : 1 + engine.followType.load();
        const int n = wrap ? (cur + dir + 3) % 3 : jlimit (0, 2, cur + dir);
        engine.followAuto = n == 0 ? 1 : 0;
        if (n == 0) { engine.followAutoFound = 0; engine.keyHist.fill (0.0f); engine.keyHistN = 0.0f; engine.keyHistT = 0.0; }
        else engine.followSetScale (engine.followRoot.load(), n - 1);
        saveNow();
        refreshAll();
    }

    // AS (Assistant): κλίση πάνω = rules, κάτω = off (το style έρχεται στο επόμενο build)
    void s88StepAs (int b, int dir)
    {
        if (! isActive (b)) return;
        engine.setAsMode (b, jlimit (0, 2, engine.asMode[(size_t) b].load() + dir));
        if (! engine.isAs (b)) s88AsEdit = false;
        saveNow();
        refreshAll();
    }

    // τρόπος επανάληψης του επόμενου layer: loop / once / hold at end
    void s88StepRepeat (int b, int dir, bool wrap)
    {
        if (! isActive (b)) return;
        const int cur = engine.repeatMode[(size_t) b].load();
        engine.repeatMode[(size_t) b] = wrap ? (cur + dir + 3) % 3 : jlimit (0, 2, cur + dir);
        saveNow();
        refreshAll();
    }

    void s88StepCurve (int b, int dir)
    {
        if (! isActive (b)) return;
        engine.curveMode[(size_t) b] = (engine.curveMode[(size_t) b].load() + dir + flow::numCurves) % flow::numCurves;
        saveNow();
        refreshAll();
    }

    void s88MenuActivate()
    {
        const int b = selected;
        if (! isActive (b)) return;
        switch (s88MenuSel)
        {
            case 0: if (keyboardView.onHold) keyboardView.onHold (b); break;
            case 1: engine.curveMode[(size_t) b] = 0; saveNow(); refreshAll(); break;   // πάτημα: πρώτη καμπύλη (+/− με κλίση πάνω/κάτω)
            case 2: if (keyboardView.onTranspose) keyboardView.onTranspose (b, 0); break;   // πάτημα: transpose στο 0 (+/− με κλίση πάνω/κάτω)
            case 3: if (keyboardView.onToggleSilent) keyboardView.onToggleSilent (b); break;
            case 4: if (keyboardView.onSwapInstrument) keyboardView.onSwapInstrument (b); break;
            case 5: if (keyboardView.onToggleSpecialBlock) keyboardView.onToggleSpecialBlock (b); break;
            case 6: if (keyboardView.onToggleMf) keyboardView.onToggleMf (b); break;
            case 7: if (keyboardView.onToggleFollowBlock) keyboardView.onToggleFollowBlock (b); break;
            case 8: s88StepScale (1, true); break;   // πάτημα: επόμενη κλίμακα (και με κλίση πάνω/κάτω)
            case 9: s88StepRepeat (b, 1, true); break;   // πάτημα: επόμενος τρόπος (και με κλίση πάνω/κάτω)
            case 10:   // πάτημα: αν ο βοηθός είναι off ανοίγει σε rules, αλλιώς μπαίνει/βγαίνει από τη ρύθμιση ποσοστού
                if (! engine.isAs (b)) { engine.setAsMode (b, 1); saveNow(); refreshAll(); }
                else s88AsEdit = ! s88AsEdit;
                break;
            case 11: if (keyboardView.onRedoZone && ! engine.isAs (b)) keyboardView.onRedoZone (b); break;
            default:
                if (Time::getMillisecondCounter() < s88RemoveArmUntil) { s88RemoveArmUntil = 0; removeBlock (b); }   // σβήσιμο: δύο πατήματα, όπως στον Mac
                else { s88RemoveArmUntil = Time::getMillisecondCounter() + 3000; setMessage (gr ("Press again to delete the block")); }
                break;
        }
    }

    String s88BlockSig()
    {
        const int b = selected;
        String pn; bool hasSnap = false;
        if (auto* pl = engine.getPlugin (b)) { pn = pl->getName(); hasSnap = thumbFor (pn).isValid(); }
        String s = "B" + String (b) + "," + String (s88MenuSel) + "," + String (engine.lo[(size_t) b].load()) + "," + String (engine.hi[(size_t) b].load()) + ","
                   + String ((int) (engine.volPos[(size_t) b].load() * 200.0f)) + "," + pn + "," + String ((int) hasSnap) + ",";
        for (int i = 0; i < 13; ++i) s += s88MenuLabel (i, b) + "|";
        s += String (engine.asPct[(size_t) b].load()) + (engine.isAs (b) ? "A" : "a") + (s88AsEdit ? "e" : "");
        return s;
    }

    // 8 macros του VST (θέσεις 1–8 των κουμπιών): δαχτυλίδι με την τιμή, από κάτω το όνομα. Κενή θέση = παύλα
    String s88MacrosSig()
    {
        const int b = selected;
        String pn; bool hasSnap = false;
        if (auto* pl = engine.getPlugin (b)) { pn = pl->getName(); hasSnap = thumbFor (pn).isValid(); }
        String s = "BM" + String (b) + "," + String (engine.lo[(size_t) b].load()) + "," + String (engine.hi[(size_t) b].load()) + ","
                   + String ((int) (engine.volPos[(size_t) b].load() * 200.0f)) + "," + pn + "," + String ((int) hasSnap) + ",";
        for (int k = 0; k < 8; ++k)
        {
            if (auto* p = knobTarget (k)) s += p->getName (16) + String (roundToInt (jlimit (0.0f, 1.0f, p->getValue()) * 100.0f)) + "|";
            else s += "-|";
        }
        return s;
    }

    void drawS88MacroPanel (Graphics& g, int b, Colour bcol)
    {
        const Colour white (0xffffffff), track (0xff3a3430), none (0xff6a645e), grey (0xffb7bbc6);
        for (int k = 0; k < 8; ++k)
        {
            const float cx = 60.0f + (float) (k % 4) * 120.0f, cy = k < 4 ? 94.0f : 196.0f, r = 27.0f;
            Path tr; tr.addCentredArc (cx, cy, r, r, 0.0f, -2.356f, 2.356f, true);
            g.setColour (track); g.strokePath (tr, PathStrokeType (6.0f, PathStrokeType::curved, PathStrokeType::rounded));
            if (auto* p = knobTarget (k))
            {
                const float v = jlimit (0.0f, 1.0f, p->getValue());
                if (v > 0.004f)
                {
                    Path a; a.addCentredArc (cx, cy, r, r, 0.0f, -2.356f, -2.356f + 4.712f * v, true);
                    g.setColour (bcol); g.strokePath (a, PathStrokeType (6.0f, PathStrokeType::curved, PathStrokeType::rounded));
                }
                g.setColour (white); g.setFont (hobby::makeFont (typeface, 19.0f).boldened());
                g.drawText (String (roundToInt (v * 100.0f)), Rectangle<float> (cx - r, cy - 12.0f, 2.0f * r, 24.0f), Justification::centred, false);
                g.setFont (hobby::makeFont (typeface, 15.0f).boldened());
                g.drawText (p->getName (14), Rectangle<float> (cx - 56.0f, cy + r + 4.0f, 112.0f, 20.0f), Justification::centred, true);
            }
            else
            {
                g.setColour (none); g.setFont (hobby::makeFont (typeface, 19.0f));
                g.drawText (String::fromUTF8 ("\xe2\x80\x94"), Rectangle<float> (cx - r, cy - 12.0f, 2.0f * r, 24.0f), Justification::centred, false);
            }
        }
        g.setColour (grey); g.setFont (hobby::makeFont (typeface, 14.0f));
        String pn; if (auto* pl = engine.getPlugin (b)) pn = pl->getName();
        g.drawText ("Macros " + String::fromUTF8 ("\xc2\xb7 ") + pn + "  (knobs 1" + String::fromUTF8 ("\xe2\x80\x93") + "8)", Rectangle<float> (14.0f, 246.0f, 452.0f, 18.0f), Justification::centred, false);
    }

    void drawS88Block (Graphics& gl, Graphics& gRt)
    {
        drawS88DimArt (gl, gRt);
        const int b = jlimit (0, flow::numBlocks - 1, selected);
        const Colour bcol = blockLight (b), white (0xffffffff);
        // ---- αριστερά: μεγάλο snapshot του VST, αριθμός πάνω αριστερά, range (κάτω νότα από πάνω, πάνω νότα από κάτω) πάνω δεξιά, γραμμή έντασης κάτω ----
        {
            const Rectangle<float> tr (100.0f, 50.0f, 280.0f, 187.0f);
            Path clip; clip.addRoundedRectangle (tr, 10.0f);
            gl.saveState(); gl.reduceClipRegion (clip, AffineTransform());
            gl.setColour (Colour (0xff3a3430)); gl.fillRect (tr);
            Image snap;
            if (auto* pl = engine.getPlugin (b)) snap = thumbFor (pl->getName());
            if (snap.isValid()) { gl.setColour (Colours::white); gl.setImageResamplingQuality (Graphics::highResamplingQuality); gl.drawImage (snap, tr, RectanglePlacement::centred); }
            gl.restoreState();
        }
        gl.setColour (bcol); gl.fillEllipse (26.0f, 54.0f, 44.0f, 44.0f);
        gl.setColour (Colour (0xff1c1a19)); gl.setFont (hobby::makeFont (typeface, 26.0f).boldened());
        gl.drawText (String (b + 1), Rectangle<float> (26.0f, 54.0f, 44.0f, 44.0f), Justification::centred, false);
        const int lo = engine.lo[(size_t) b].load(), hi = engine.hi[(size_t) b].load();
        gl.setColour (white); gl.setFont (hobby::makeFont (typeface, 26.0f).boldened());
        if (engine.isAs (b))
        {
            gl.drawText ("AS", Rectangle<float> (380.0f, 50.0f, 74.0f, 32.0f), Justification::centredRight, false);
            gl.drawText (String (engine.asPct[(size_t) b].load()) + "%", Rectangle<float> (380.0f, 86.0f, 74.0f, 32.0f), Justification::centredRight, false);
        }
        else
        {
            gl.drawText (noteName (lo), Rectangle<float> (380.0f, 50.0f, 74.0f, 32.0f), Justification::centredRight, false);
            gl.drawText (noteName (hi), Rectangle<float> (380.0f, 86.0f, 74.0f, 32.0f), Justification::centredRight, false);
        }
        const float kx0 = 28.0f, kw = 424.0f;
        gl.setColour (Colour (0xff3a3430)); gl.fillRoundedRectangle (kx0, 252.0f, kw, 6.0f, 3.0f);
        gl.setColour (Colour (0xffe8b94a)); gl.fillRoundedRectangle (kx0, 252.0f, jmax (6.0f, kw * jlimit (0.0f, 1.0f, engine.volPos[(size_t) b].load())), 6.0f, 3.0f);   // γραμμή volume: κινείται με το πετάλι

        if (s88MacrosOpen) { drawS88MacroPanel (gRt, b, bcol); return; }

        // ---- δεξιά: 10 κυκλάκια, 5×2 ----
        const float D = 54.0f, gap = 8.0f, gx = 14.0f + (452.0f - (7.0f * D + 6.0f * gap)) * 0.5f;
        for (int i = 0; i < 13; ++i)
        {
            const float cx = gx + (float) (i < 7 ? i : i - 7) * (D + gap), cy = 62.0f + (float) (i < 7 ? 0 : 1) * (D + 12.0f);
            gRt.setColour (Colours::black); gRt.fillEllipse (cx, cy, D, D);
            if (s88MenuOn (i, b)) { gRt.setColour (bcol); gRt.drawEllipse (cx + 1.5f, cy + 1.5f, D - 3.0f, D - 3.0f, 3.0f); }
            if (i == s88MenuSel) { gRt.setColour (white); gRt.drawEllipse (cx - 3.0f, cy - 3.0f, D + 6.0f, D + 6.0f, 3.0f); }
            gRt.setColour (white);
            const Rectangle<float> r (cx, cy, D, D);
            const int h = engine.holdMode[(size_t) b].load(), tr = engine.transpose[(size_t) b].load() / 12;
            gRt.setFont (hobby::makeFont (typeface, 22.0f).boldened());
            switch (i)
            {
                case 0: gRt.drawText (h == 1 ? "LM" : h == 2 ? "LP" : h >= 3 ? "AL" : "H", r, Justification::centred, false); break;
                case 1:   // η καμπύλη έντασης του block, όπως είναι τώρα
                {
                    const int cm = engine.curveMode[(size_t) b].load();
                    Path cp;
                    for (int k = 0; k <= 24; ++k)
                    {
                        const float xx = (float) k / 24.0f;
                        const float px = cx + 16.0f + xx * (D - 32.0f), py = cy + D - 16.0f - flow::curveGain (cm, xx) * (D - 32.0f);
                        if (k == 0) cp.startNewSubPath (px, py); else cp.lineTo (px, py);
                    }
                    gRt.strokePath (cp, PathStrokeType (3.0f));
                    break;
                }
                case 2: gRt.drawText (tr > 0 ? "+" + String (tr) : String (tr), r, Justification::centred, false); break;
                case 3:
                    gRt.drawText (String::fromUTF8 ("\xe2\x99\xaa"), r, Justification::centred, false);
                    if (engine.liveSilent[(size_t) b].load() != 0) gRt.drawLine (cx + 16.0f, cy + D - 16.0f, cx + D - 16.0f, cy + 16.0f, 3.0f);
                    break;
                case 4: gRt.drawText (String::fromUTF8 ("\xe2\x87\x84"), r, Justification::centred, false); break;
                case 5: gRt.drawText ("SP", r, Justification::centred, false); break;
                case 6: gRt.drawText ("MF", r, Justification::centred, false); break;
                case 7: gRt.drawText ("f", r, Justification::centred, false); break;
                case 8:   // κλίμακα: τρίγωνο = major, παύλα = minor, τελεία = αυτόματη
                {
                    const int ft = engine.followType.load();
                    const float mx = cx + D * 0.5f, my = cy + D * 0.5f;
                    if (ft == 0) { Path tri; tri.addTriangle (mx, my - 12.0f, mx - 12.0f, my + 9.0f, mx + 12.0f, my + 9.0f); gRt.strokePath (tri, PathStrokeType (3.0f)); }
                    else gRt.drawLine (mx - 13.0f, my, mx + 13.0f, my, 3.5f);
                    if (engine.followAuto.load() != 0) gRt.fillEllipse (cx + D - 16.0f, cy + 8.0f, 7.0f, 7.0f);   // τελεία = αυτόματη εύρεση
                    break;
                }
                case 9:   // Repeat: ∞ = loop, →| = once, →‖ = hold at end
                {
                    const int rm = engine.repeatMode[(size_t) b].load();
                    const float mx = cx + D * 0.5f, my = cy + D * 0.5f;
                    if (rm == 0) gRt.drawText (String::fromUTF8 ("\xe2\x88\x9e"), r, Justification::centred, false);
                    else
                    {
                        const float x0 = rm == 1 ? mx - 15.0f : mx - 18.0f, x1 = rm == 1 ? mx + 8.0f : mx + 2.0f;
                        gRt.drawLine (x0, my, x1, my, 3.0f);
                        Path ar; ar.addTriangle (x1, my - 6.0f, x1 + 7.0f, my, x1, my + 6.0f); gRt.fillPath (ar);
                        if (rm == 1) gRt.drawLine (mx + 19.0f, my - 10.0f, mx + 19.0f, my + 10.0f, 3.0f);
                        else { gRt.drawLine (mx + 14.0f, my - 10.0f, mx + 14.0f, my + 10.0f, 3.0f); gRt.drawLine (mx + 20.0f, my - 10.0f, mx + 20.0f, my + 10.0f, 3.0f); }
                    }
                    break;
                }
                case 10:
                    gRt.drawText ("AS", r, Justification::centred, false);
                    if (engine.asMode[(size_t) b].load() == 2)   // style: κυματιστή γραμμή κάτω από το AS
                    {
                        Path wv;
                        for (int k = 0; k <= 20; ++k) { const float px = cx + 14.0f + (float) k / 20.0f * (D - 28.0f), py = cy + D - 11.0f + 3.0f * std::sin ((float) k / 20.0f * 9.42f); if (k == 0) wv.startNewSubPath (px, py); else wv.lineTo (px, py); }
                        gRt.strokePath (wv, PathStrokeType (2.0f));
                    }
                    break;
                case 11: gRt.drawText (String::fromUTF8 ("\xe2\x9f\xb2"), r, Justification::centred, false); break;
                default:
                    gRt.setColour (Time::getMillisecondCounter() < s88RemoveArmUntil ? hobby::kRed : white);
                    gRt.drawLine (cx + 18.0f, cy + 18.0f, cx + D - 18.0f, cy + D - 18.0f, 3.0f);
                    gRt.drawLine (cx + 18.0f, cy + D - 18.0f, cx + D - 18.0f, cy + 18.0f, 3.0f);
                    break;
            }
        }
        gRt.setColour (white); gRt.setFont (hobby::makeFont (typeface, 18.0f).boldened());
        gRt.drawText (s88MenuLabel (s88MenuSel, b), Rectangle<float> (14.0f, 222.0f, 452.0f, 28.0f), Justification::centred, false);
    }

    // Set zone (και «ζώνη ορισμένη, χωρίς VST ακόμη»): φόντο 75% σκούρο με κόκκο. Αριστερά κάτω «Block n» και η επόμενη ενέργεια, κάτω δεξιά το snapshot του VST όσο φορτώνει.
    // Δεξιά: κλαβιέ 2 οκτάβων (ίδιο ύψος με το snapshot), το range ακριβώς από πάνω δεξιά. Το παράθυρο ακολουθεί τα πλήκτρα που πατάς.
    void drawS88SetZone (Graphics& gl, Graphics& gRt)
    {
        drawS88DimArt (gl, gRt);
        const int pb = engine.pendingBlock();
        const int b = jlimit (0, flow::numBlocks - 1, pb >= 0 ? pb : selected);
        const bool done = pb < 0;   // η ζώνη έχει οριστεί, το block δεν έχει ακόμη plugin
        const int first = (! done && engine.zoneState[(size_t) b].load() == 2) ? engine.zoneFirst[(size_t) b].load() : -1;
        const int lo = engine.lo[(size_t) b].load(), hi = engine.hi[(size_t) b].load();
        const bool loading = loadingSlots.contains (b);
        const Colour bcol = blockLight (b), white (0xffffffff);

        gl.setColour (bcol); gl.setFont (hobby::makeFont (typeface, 32.0f).boldened());
        gl.drawText ("Block " + String (b + 1), Rectangle<float> (18.0f, 182.0f, 220.0f, 40.0f), Justification::centredLeft, false);
        const String line2 = loading ? String() : done ? String ("Choose a VST") : String ("Set the zone");
        gl.setColour (white); gl.setFont (hobby::makeFont (typeface, 22.0f).boldened());
        gl.drawText (line2, Rectangle<float> (18.0f, 222.0f, 220.0f, 30.0f), Justification::centredLeft, false);
        if (loading)   // snapshot του VST: η πάνω αριστερή γωνία στο κέντρο της οθόνης
        {
            const Rectangle<float> tr (240.0f, 136.0f, 226.0f, 122.0f);
            Path clip; clip.addRoundedRectangle (tr, 10.0f);
            gl.saveState(); gl.reduceClipRegion (clip, AffineTransform());
            Image im = thumbFor (loadingName[(size_t) b]);
            gl.setColour (Colour (0xff3a3430)); gl.fillRect (tr);
            if (im.isValid()) { gl.setColour (Colours::white); gl.setImageResamplingQuality (Graphics::highResamplingQuality); gl.drawImage (im, tr, RectanglePlacement::centred); }
            gl.restoreState();
        }

        // δεξιά: παράθυρο 2 οκτάβων (14 λευκά), ευθυγραμμισμένο σε ντο
        const int anchor = done ? lo : (first >= 0 ? first : 60);
        int start = (anchor / 12) * 12;
        if (done && hi > start + 23) start = jmax (start, (hi / 12 - 1) * 12);   // μεγάλη ζώνη: δείχνει το άκρο της (το range από πάνω δείχνει όλη τη ζώνη)
        start = jlimit (12, 84, start);
        const float kx0 = 34.0f, ww = 412.0f / 14.0f, ky = 136.0f, kh = 122.0f;
        auto inZone = [&] (int n) { return (done && n >= lo && n <= hi) || n == first; };
        int wi = 0;
        std::array<int, 24> whiteIx {};
        for (int n = start; n < start + 24; ++n)
        {
            whiteIx[(size_t) (n - start)] = wi;
            if (flow::isBlackKey (n)) continue;
            if (n >= flow::lowestNote && n <= flow::highestNote)
            {
                gRt.setColour (inZone (n) ? bcol : Colour (0xfff0ece6));
                gRt.fillRect (kx0 + (float) wi * ww, ky, ww - 2.0f, kh);
            }
            ++wi;
        }
        for (int n = start; n < start + 24; ++n)
            if (flow::isBlackKey (n) && n >= flow::lowestNote && n <= flow::highestNote)
            {
                const float x = kx0 + (float) whiteIx[(size_t) (n - start)] * ww;   // το μαύρο μπαίνει στο όριο ανάμεσα στο προηγούμενο και το επόμενο λευκό
                gRt.setColour (inZone (n) ? bcol.darker (0.45f) : Colour (0xff15130f));
                gRt.fillRect (x - ww * 0.3f, ky, ww * 0.6f, kh * 0.62f);
            }
        if (done || first >= 0)
        {
            gRt.setColour (white); gRt.setFont (hobby::makeFont (typeface, 34.0f).boldened());
            gRt.drawText (done ? noteName (lo) + String::fromUTF8 (" \xe2\x80\x93 ") + noteName (hi) : noteName (first) + String::fromUTF8 (" \xe2\x80\x93"),
                          Rectangle<float> (150.0f, ky - 50.0f, 296.0f, 42.0f), Justification::centredRight, false);
        }
    }

    // Orb Rings: υπογραφή περιεχομένου (επιλογή, learn, ονόματα/τιμές, θέση στα 16 μέτρα): ξαναστέλνεται μόνο αν άλλαξε κάτι
    String s88RingsSig()
    {
        String s = "O" + String (s88Ring) + "," + String (s88Slot) + "," + String (knobStage != 0 ? knobSlot : -1) + "," + String (selected) + "," + String ((int) orbRecArmed) + String ((int) s88Blink) + ",";
        for (int k = 0; k < kSlots; ++k)
        {
            if (auto* p = knobTarget (k)) s += p->getName (24) + String (roundToInt (jlimit (0.0f, 1.0f, p->getValue()) * 100.0f)) + "|";
            else s += "-|";
        }
        s += String ((int) (std::fmod (jmax (0.0, engine.beatPosOut.load()), 64.0) / 64.0 * 240.0));
        return s;
    }

    // Orb Rings: 2 ring ανά οθόνη, 4 θέσεις παραμέτρων το καθένα. Λευκό περίγραμμα στην επιλεγμένη θέση, κάτω γραμμή = τα 16 μέτρα (κόκκινη όσο γράφει το Rec motion).
    void drawS88OrbRings (Graphics& gl, Graphics& gRt)
    {
        drawS88Art (gl, gRt);
        static const Colour rc[4] { Colour (0xffd96b4a), Colour (0xffe8b94a), Colour (0xff4a9ab8), Colour (0xffa86a9a) };
        const Colour white (0xffffffff), dim (0xffb0aaa2), none (0xff6a645e), warm (0xffe8b94a);
        const float ph = (float) (std::fmod (jmax (0.0, engine.beatPosOut.load()), 64.0) / 64.0);
        Graphics* gs[2] = { &gl, &gRt };
        for (int sc = 0; sc < 2; ++sc)
        {
            auto& g = *gs[sc];
            g.setColour (Colour (0xff1c1a19).withAlpha (0.8f));
            g.fillRoundedRectangle (14.0f, 50.0f, 452.0f, 208.0f, 14.0f);
            for (int i = 0; i < 2; ++i)
            {
                const int r = sc * 2 + i;
                const float x0 = i == 0 ? 26.0f : 246.0f;
                g.setColour (rc[r]); g.fillEllipse (x0, 62.0f, 22.0f, 22.0f);
                g.setColour (white); g.setFont (hobby::makeFont (typeface, 20.0f).boldened());
                g.drawText ("Ring " + String (r + 1), Rectangle<float> (x0 + 32.0f, 59.0f, 150.0f, 26.0f), Justification::centredLeft, false);
                for (int k = 0; k < 4; ++k)
                {
                    const int slot = r * 4 + k;
                    const float y = 98.0f + (float) k * 40.0f;
                    if (s88Ring == r && s88Slot == k) { g.setColour (white); g.drawRoundedRectangle (x0, y, 208.0f, 34.0f, 10.0f, 2.0f); }
                    const Rectangle<float> tr (x0 + 10.0f, y + 4.0f, 150.0f, 26.0f), pr (x0 + 148.0f, y + 4.0f, 50.0f, 26.0f);
                    if (auto* p = knobTarget (slot))
                    {
                        g.setFont (hobby::makeFont (typeface, 16.0f)); g.setColour (white);
                        g.drawText (p->getName (24), Rectangle<float> (x0 + 10.0f, y + 4.0f, 136.0f, 26.0f), Justification::centredLeft, true);
                        g.setFont (hobby::makeFont (typeface, 16.0f).boldened()); g.setColour (dim);
                        g.drawText (String (roundToInt (jlimit (0.0f, 1.0f, p->getValue()) * 100.0f)) + "%", pr, Justification::centredRight, false);
                    }
                    else if (knobStage != 0 && knobSlot == slot)
                    {
                        g.setFont (hobby::makeFont (typeface, 15.0f)); g.setColour (warm);
                        g.drawText (String (String::fromUTF8 ("touch a parameter\xe2\x80\xa6")), tr, Justification::centredLeft, false);
                    }
                    else
                    {
                        g.setFont (hobby::makeFont (typeface, 16.0f)); g.setColour (none);
                        g.drawText (String::fromUTF8 ("\xe2\x80\x94"), tr, Justification::centredLeft, false);
                    }
                }
            }
            g.setColour (Colour (0xff3a3430)); g.fillRect (0, 266, KeyScreens::width, 6);
            const float fw = jlimit (0.0f, (float) KeyScreens::width, ph * 2.0f * (float) KeyScreens::width - (float) (sc * KeyScreens::width));
            g.setColour (orbRecArmed ? hobby::kCoral : Colour (0xffc9c3ba)); g.fillRect (0.0f, 266.0f, fw, 6.0f);
        }
    }

    void updateS88Screens()
    {
        if (! keyScreens.isOn()) return;
        auto& list = loopsView.getList();
        const bool open = list.isVisible();
        list.alwaysShowSel = true;
        s88Blink = orbRecArmed && ((Time::getMillisecondCounter() / 400) & 1) != 0;
        {   // όταν αλλάζει το επιλεγμένο block (π.χ. παίζοντας) ή φορτώνεται plugin, η οθόνη του block εμφανίζεται για λίγο
            const int selKey = selected * 2 + (isActive (selected) ? 1 : 0);
            if (selKey != s88LastSelKey) { s88LastSelKey = selKey; if (isActive (selected)) s88BlockTouch(); }
        }
        const bool splash = Time::getMillisecondCounter() < s88SplashUntil;
        std::vector<int> vis; int cols = 1, rows = 1;
        if (open)
        {
            list.buildVis (vis, cols, rows);
            const String sig = "L" + String (list.getSelK()) + "/" + String ((int) vis.size()) + "/" + String ((int) list.getShowAll()) + "/" + list.getSearchText() + "/" + String (cols) + "/" + String (selected);
            if (sig == s88Sig) return;
            s88Sig = sig;
        }
        else if (splash)
        {
            if (s88Sig == "LOGO") return;
            s88Sig = "LOGO";
        }
        else if (engine.pendingBlock() >= 0)   // το block περιμένει ζώνη (zones first ή μετά από φόρτωση plugin)
        {
            const int zb = engine.pendingBlock();
            const String sig = "Z" + String (zb) + "," + String (engine.zoneState[(size_t) zb].load()) + "," + String (engine.zoneFirst[(size_t) zb].load()) + "," + String ((int) s88Blink) + "," + String ((int) loadingSlots.contains (zb));
            if (sig == s88Sig) return;
            s88Sig = sig;
        }
        else if (s88RingsOpen)
        {
            const String sig = s88RingsSig();
            if (sig == s88Sig) return;
            s88Sig = sig;
        }
        else if (isEmptyZone (selected))   // ζώνη ορισμένη, χωρίς plugin ακόμη: μένουμε στην οθόνη του block (όχι στο άδειο σχέδιο)
        {
            const String sig = "Z" + String (selected) + ",done," + String (engine.lo[(size_t) selected].load()) + "," + String (engine.hi[(size_t) selected].load()) + "," + String ((int) loadingSlots.contains (selected));
            if (sig == s88Sig) return;
            s88Sig = sig;
        }
        else if (s88MacrosOpen && isActive (selected))
        {
            const String sig = s88MacrosSig();
            if (sig == s88Sig) return;
            s88Sig = sig;
        }
        else if (s88BlockVisible())
        {
            const String sig = s88BlockSig();
            if (sig == s88Sig) return;
            s88Sig = sig;
        }
        else
        {
            const String sg = "ART" + String ((int) s88Blink);
            if (s88Sig == sg) return;
            s88Sig = sg;
        }

        const Colour bg (0xff000000), thumbBg (0xff3a3430), white (0xfff3f4f6), grey (0xffb7bbc6), dim (0xff6a6c73);
        Image L (Image::RGB, KeyScreens::width, KeyScreens::height, true), R (Image::RGB, KeyScreens::width, KeyScreens::height, true);
        Graphics gl (L), gRt (R);
        gl.fillAll (bg); gRt.fillAll (bg);

        if (! open && splash) drawS88Logo (gl, gRt);
        else
        {
            if (! open && s88Sig.startsWith ("ART")) drawS88Art (gl, gRt);
            if (! open && s88Sig.startsWith ("O")) drawS88OrbRings (gl, gRt);
            if (! open && s88Sig.startsWith ("Z")) drawS88SetZone (gl, gRt);
            if (! open && s88Sig.startsWith ("B")) drawS88Block (gl, gRt);
            if (open)
            {
                drawS88DimArt (gl, gRt);
                const Colour dimTxt (0xff6a6c73);
                if (vis.empty())
                {
                    gRt.setColour (dimTxt); gRt.setFont (hobby::makeFont (typeface, 20.0f)); gRt.drawText ("No results", Rectangle<int> (0, 0, KeyScreens::width, KeyScreens::height), Justification::centred);
                    drawS88ListLeft (gl, jlimit (0, flow::numBlocks - 1, selected), Image());
                }
                else
                {
                    const int total = (int) vis.size();
                    const int selK = jlimit (0, total - 1, list.getSelK());
                    const auto& items = list.getItems();
                    Image snap = list.thumbFn ? list.thumbFn (items[vis[(size_t) selK]]) : Image();
                    drawS88ListLeft (gl, jlimit (0, flow::numBlocks - 1, selected), snap);
                    drawS88ListRight (gRt, jlimit (0, flow::numBlocks - 1, selected), vis, items, selK, list.getShowAll());
                }
            }
            drawS88Strip (gl, 0); drawS88Strip (gRt, 1);
        }
        std::vector<uint8_t> buf;
        toBgr565 (L, buf); keyScreens.submit (0, buf.data());
        toBgr565 (R, buf); keyScreens.submit (1, buf.data());
    }

    int midiDiagR = 0, midiDiagTotal = 0;
    uint32_t midiDiagLast[256] = {};
    uint32_t midiDiagLastMs[256] = {};

    // Διαγνωστικό: γράφει στο log τα MIDI μηνύματα που δεν είναι νότες (CC, pitch bend, program change, SysEx…), ένα ανά (status, data1) το πολύ κάθε 150 ms
    void drainMidiDiag()
    {
        const int w = engine.midiDiagW.load (std::memory_order_acquire);
        if (w - midiDiagR > 256) midiDiagR = w - 256;
        while (midiDiagR < w)
        {
            const uint32_t v = engine.midiDiag[(size_t) (midiDiagR & 255)].load (std::memory_order_relaxed);
            ++midiDiagR;
            const uint32_t status = (v >> 16) & 0xff, d1 = (v >> 8) & 0xff, d2 = v & 0xff, size = v >> 24;
            const size_t key = (size_t) (((status & 0x0f) * 31 + d1 * 7 + (status >> 4)) & 255);
            const auto now = Time::getMillisecondCounter();
            if (midiDiagTotal >= 1500) continue;
            if (midiDiagLast[key] == (v & 0xffffff) || (now - midiDiagLastMs[key] < 150)) continue;
            midiDiagLast[key] = v & 0xffffff; midiDiagLastMs[key] = now; ++midiDiagTotal;
            flow::flowLog ("midi in: status=0x" + String::toHexString ((int) status) + " d1=" + String ((int) d1) + " d2=" + String ((int) d2) + " size=" + String ((int) size));
        }
    }

    void setKeyLights (bool on)
    {
        lightsWanted = on;
        if (on)
        {
            keyLights.setInputLogger ([] (const String& s) { flow::flowLog (s); });
            keyLights.onControl = [this] (int ctl, int v) { handleS88Control (ctl, v); };
            bool ok;
            { const ScopedLock sl (lightsLock); ok = keyLights.start(); }
            flow::flowLog ("key lights: " + keyLights.getStatus());
            if (ok) setMessage (gr ("Key lights: ") + keyLights.getStatus());
            if (ok && ! keyScreens.isOn())   // οθόνες S88 (πείραμα): δοκιμαστικό καρέ με μπάρες χρώματος
            {
                keyScreens.logger = [] (const String& s) { flow::flowLog (s); };
                if (keyScreens.start()) { s88Sig = String(); s88Art = Image(); s88ArtDim = Image(); s88SplashUntil = Time::getMillisecondCounter() + 2500; submitS88Logo(); }   // πρώτα το λογότυπο, μετά το σχέδιο
            }
            if (ok)
            {
                lastLights.fill (0xff);
                updateLights (true);
                lightsPump.f = [this] { pumpLights(); };
                lightsPump.startTimer (5);
            }
            else lightsRetryAt = Time::getMillisecondCounter() + 2000;
        }
        else
        {
            lightsRetryAt = 0;
            lightsPump.stopTimer();
            { const ScopedLock sl (lightsLock); keyLights.stop(); }
            if (keyScreens.isOn()) submitS88Logo();   // το Flow κλείνει: μένει το λογότυπο στις οθόνες
            keyScreens.stop();
            loopsView.getList().alwaysShowSel = false;
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
            const int b = engine.blockForNote (n, true, true);
            if (b < 0) continue;
            if (engine.blockOff[(size_t) b].load() != 0) continue;
            buf[(size_t) idx] = lightByte (blockHue (b), b == selected ? (blockHue (b) == 3 ? 2 : 3) : 1);   // κίτρινο (3) στη φωτεινότητα 3 φαίνεται λευκό στο MK2
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

        for (int sb : { engine.specialBlock.load(), engine.specialBlock2.load() })
            if (sb >= 0 && isActive (sb) && engine.blockOff[(size_t) sb].load() == 0 && engine.zoneState[(size_t) sb].load() == 0)
                for (int n : { engine.lo[(size_t) sb].load(), engine.hi[(size_t) sb].load() })
                {
                    const int idx = n - first;
                    if (idx >= 0 && idx < count && idx < 88) buf[(size_t) idx] = (uint8_t) (17 * 4 + 3);   // άσπρο
                }

        for (int n : { flow::tapKeyNote, flow::recAudioKeyNote, flow::followKeyNote })   // πλήκτρα ελέγχου (tap tempo, Rec Audio, Follow): άσπρα και στο hardware
        {
            const int idx = n - first;
            if (idx >= 0 && idx < count && idx < 88) buf[(size_t) idx] = (uint8_t) (17 * 4 + 3);
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
                lightsRetryAt = Time::getMillisecondCounter() + 2000;   // θα ξαναδοκιμάσει μόνο του (π.χ. όταν το Maschine αφήσει το πληκτρολόγιο)
                updateToolLamps();
            }
        }
    }

    uint32 lightsRetryAt = 0;
    int lightsRetryCount = 0;

    void retryLightsIfNeeded()
    {
        if (! lightsWanted || keyLights.isOn() || lightsRetryAt == 0) return;
        if ((int32) (Time::getMillisecondCounter() - lightsRetryAt) < 0) return;
        bool ok;
        { const ScopedLock sl (lightsLock); ok = keyLights.start(); }
        if (ok)
        {
            flow::flowLog ("key lights: reconnected after " + String (lightsRetryCount) + " tries");
            lightsRetryAt = 0; lightsRetryCount = 0;
            lastLights.fill (0xff);
            updateLights (true);
            lightsPump.f = [this] { pumpLights(); };
            lightsPump.startTimer (5);
            updateToolLamps();
        }
        else
        {
            ++lightsRetryCount;
            lightsRetryAt = Time::getMillisecondCounter() + 2000;
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
        std::atomic<int> fallback { -1 };      // πρώτη αλλαγή τιμής χωρίς gesture (για plugins που δεν στέλνουν gestures)
        std::atomic<int> gestureCount { 0 }, ignoredCount { 0 };
        std::vector<char> ignore;              // παράμετροι που οδηγεί το ίδιο το Flow (ήδη αναθέσεις)
        int block = 0;
        uint32 ignoreUntil = 0;
        void attach (AudioPluginInstance* p, int b, std::atomic<int>* s, std::vector<char> skip)
        {
            detach();
            plugin = p; block = b; sink = s; ignore = std::move (skip);
            fallback = -1; gestureCount = 0; ignoredCount = 0;
            ignoreUntil = Time::getMillisecondCounter() + 400;
            for (auto* prm : p->getParameters()) prm->addListener (this);
        }
        void detach()
        {
            if (plugin != nullptr) for (auto* prm : plugin->getParameters()) prm->removeListener (this);
            plugin = nullptr;
        }
        bool skipped (int i) const { return i >= 0 && (size_t) i < ignore.size() && ignore[(size_t) i] != 0; }
        // μόνο αληθινή κίνηση από τον χρήστη (gesture) γίνεται δεκτή αμέσως
        void parameterGestureChanged (int i, bool starting) override
        {
            if (! starting || Time::getMillisecondCounter() < ignoreUntil) return;
            if (skipped (i)) { ++ignoredCount; return; }
            ++gestureCount;
            int expected = -1;
            if (sink != nullptr) sink->compare_exchange_strong (expected, (block << 20) | (i & 0xfffff));
        }
        void parameterValueChanged (int i, float) override
        {
            if (Time::getMillisecondCounter() < ignoreUntil) return;
            if (skipped (i)) { ++ignoredCount; return; }
            int expected = -1;
            fallback.compare_exchange_strong (expected, i);
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
            else autoAssignMacros (block);
        }
        validateKnobs (block);
    }

    // Plugin χωρίς αναθέσεις που δεν έχει ξαναδεί το Flow: οι παράμετροι που λέγονται «Macro …» πάνε στα πρώτα κουμπιά με τη σειρά του αριθμού τους (έως 8).
    // Δουλεύει μόνο όσο το plugin τις δηλώνει με τέτοιο όνομα· αλλιώς δεν αναθέτει τίποτα (το log δείχνει τη λίστα των παραμέτρων κάθε plugin).
    void autoAssignMacros (int block)
    {
        auto* p = engine.getPlugin (block);
        if (p == nullptr) return;
        std::vector<std::pair<int, int>> found;   // (σειρά, δείκτης παραμέτρου)
        auto& params = p->getParameters();
        for (int i = 0; i < params.size(); ++i)
        {
            const auto nm = params[i]->getName (64);
            const int at = nm.toLowerCase().indexOf ("macro");
            if (at < 0) continue;
            found.push_back ({ i, i });   // με τη σειρά που τα δηλώνει το plugin (το Massive X έχει ονόματα σαν «Macro A1», οπότε ο αριθμός μόνος του δεν αρκεί)
        }
        if (found.empty()) return;
        std::sort (found.begin(), found.end());
        for (size_t k = 0; k < found.size() && k < 8; ++k)
            blockKnobs[(size_t) block][k] = { found[k].second, params[found[k].second]->getName (64) };
        rememberKnobs (block);
        flow::flowLog ("macros: block " + String (block + 1) + " " + p->getName() + ": " + String ((int) juce::jmin ((size_t) 8, found.size())) + " assigned automatically");
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
        std::vector<char> skip ((size_t) p->getParameters().size(), 0);
        for (int j = 0; j < kSlots; ++j)
        {
            const int ai = blockKnobs[(size_t) selected][(size_t) j].idx;
            if (j != k && ai >= 0 && (size_t) ai < skip.size()) skip[(size_t) ai] = 1;
        }
        watches[(size_t) selected].attach (p, selected, &knobTouched, std::move (skip));
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
        loopsView.orbImage = img;
        loopsView.repaint();
        if (save) saveNow();
    }

    // ---- Orb: εικόνα από τη γεννήτρια (Bauhaus + Sanzo Wada) ----
    int windowSizeMode = 1;     // 0 Compact, 1 Default, 2 Large, 3 Free
    // χωρίς zoom: η μεγάλη πλευρά της εικόνας = η διάμετρος του κύκλου· τετράγωνη εικόνα γεμίζει τον κύκλο και οι γωνίες κόβονται
    static Image orbFit (const Image& src, bool flipIt = false, int quarterTurns = 0)
    {
        constexpr int N = 512;
        Image out (Image::ARGB, N, N, true);
        Graphics g (out);
        g.setImageResamplingQuality (Graphics::highResamplingQuality);
        const float w = (float) src.getWidth(), h = (float) src.getHeight();
        const float k = (float) N / jmax (w, h);   // η μεγάλη πλευρά = η διάμετρος· ο κύκλος κόβει τις γωνίες
        g.addTransform (AffineTransform::translation (-w * 0.5f, -h * 0.5f).scaled (flipIt ? -k : k, k)
                            .rotated ((float) quarterTurns * MathConstants<float>::halfPi).translated ((float) N * 0.5f, (float) N * 0.5f));
        g.drawImageAt (src, 0, 0);
        return out;
    }

    // Γεννήτρια: γεωμετρική σύνθεση σε στυλ Bauhaus με συνδυασμό χρωμάτων Wada (μόνο συνδυασμοί με 3-4 χρώματα). Τρεις πυκνότητες: λίγα και μεγάλα (πλέγμα 3x3, 3-5 σχήματα), μεσαία (4x4, 7-10), πολλά και μικρά (5x5, 12-18).
    // Σχήματα (orbShape): τεταρτοκύκλιο, ημικύκλιο, κύκλος, δαχτυλίδι, τρίγωνο, κορυφή, αψίδα, φακός, μπάρα και λίγα τετράγωνα, σε 4 περιστροφές, πάντα με τουλάχιστον ένα καμπύλο. Δεν υπάρχει «στόχος» (δαχτυλίδι με τελεία μέσα). Έως 8 δοκιμές: κρατιέται η πρώτη όπου κανένα χρώμα δεν πιάνει πάνω από ~42-50% και κάθε χρώμα φαίνεται τουλάχιστον 8%.
    // Όλες οι ευθείες ακμές πέφτουν σε ακέραια pixel (χωρίς εξομάλυνση ακμών), ώστε να μη φαίνονται λεπτές γραμμές στα όρια των κελιών· μόνο οι καμπύλες εξομαλύνονται. Στο τέλος λεπτός κόκκος.
    static Image orbGenerated (Random& rnd)
    {
        constexpr int N = 512;
        int cand[348], nc = 0;
        for (int i = 0; i < 348; ++i) if (wada::kCombos[i].n >= 3) cand[nc++] = i;
        const auto& combo = wada::kCombos[cand[rnd.nextInt (nc)]];
        const int np = combo.n;
        Colour pal[4];
        for (int i = 0; i < np; ++i) pal[i] = Colour ((uint32) (0xff000000u | wada::kColours[combo.idx[i]]));

        const int dens = rnd.nextInt (3);                     // 0 λίγα και μεγάλα, 1 μεσαία, 2 πολλά και μικρά
        const int grid = dens == 0 ? 3 : dens == 1 ? 4 : 5;
        const int smin = dens == 0 ? 3 : dens == 1 ? 7 : 12, smax = dens == 0 ? 5 : dens == 1 ? 10 : 18;
        int e[6];
        for (int i = 0; i <= grid; ++i) e[i] = (int) std::lround ((double) i * N / (double) grid);

        auto compose = [&] (Image& out)
        {
            Graphics g (out);
            g.fillAll (pal[0]);   // αδιαφανές από την αρχή: καμία ημιδιαφανής γραμμή
            int cell[5][5] = {};
            int lastKind = -1;
            for (int r = 0; r < grid; ++r)
                for (int c = 0; c < grid; ++c)
                {
                    int k = rnd.nextInt (np), tries = 0;
                    while (tries++ < 20 && ((c > 0 && k == cell[r][c - 1]) || (r > 0 && k == cell[r - 1][c]))) k = rnd.nextInt (np);
                    cell[r][c] = k;
                    g.setColour (pal[k]);
                    g.fillRect (Rectangle<int> (e[c], e[r], e[c + 1] - e[c], e[r + 1] - e[r]));
                }
            int last = -1, arcs = 0;
            const int nShapes = smin + rnd.nextInt (smax - smin + 1);
            for (int i = 0; i <= nShapes; ++i)   // το τελευταίο (i == nShapes) μπαίνει μόνο αν λείπει καμπύλο σχήμα
            {
                if (i == nShapes && arcs > 0) break;
                int k = rnd.nextInt (np);
                if (k == last) k = (k + 1 + rnd.nextInt (np - 1)) % np;
                last = k;
                const int sc = (dens != 2 && rnd.nextFloat() < 0.3f) ? 2 : 1;   // πλάτος 1 ή 2 κελιά
                const int ix = rnd.nextInt (grid - sc + 1), iy = rnd.nextInt (grid - sc + 1);
                const int kind = i == nShapes ? 0 : orbPickKind (rnd, lastKind);
                lastKind = kind;
                const float s = (float) (e[ix + sc] - e[ix]);
                orbShape (g, kind, (float) e[ix], (float) e[iy], s, pal[k], pal[cell[iy][ix]], rnd.nextInt (4));
                if (orbKindIsCurved (kind)) ++arcs;
            }
        };

        Image best; float bestScore = 1.0e9f;
        for (int attempt = 0; attempt < 8; ++attempt)
        {
            Image cand1 (Image::ARGB, N, N, true);
            compose (cand1);
            int cnt[4] = {}, tot = 0;   // κάλυψη ανά χρώμα: δειγματοληψία μέσα στον κύκλο
            {
                Image::BitmapData bd (cand1, Image::BitmapData::readOnly);
                for (int y = 4; y < N; y += 8)
                    for (int x = 4; x < N; x += 8)
                    {
                        const float dx = (float) x - N * 0.5f, dy = (float) y - N * 0.5f;
                        if (dx * dx + dy * dy > (N * 0.5f) * (N * 0.5f)) continue;
                        const auto c = bd.getPixelColour (x, y);
                        int bi = 0; float bd2 = 1.0e9f;
                        for (int i = 0; i < np; ++i)
                        {
                            const float dr = c.getFloatRed() - pal[i].getFloatRed(), dg = c.getFloatGreen() - pal[i].getFloatGreen(), db = c.getFloatBlue() - pal[i].getFloatBlue();
                            const float d2 = dr * dr + dg * dg + db * db;
                            if (d2 < bd2) { bd2 = d2; bi = i; }
                        }
                        ++cnt[bi]; ++tot;
                    }
            }
            float mx = 0.0f, mn = 1.0f;
            for (int i = 0; i < np; ++i) { const float f = tot > 0 ? (float) cnt[i] / (float) tot : 0.0f; mx = jmax (mx, f); mn = jmin (mn, f); }
            const float score = mx + (mn < 0.08f ? 0.3f : 0.0f);
            if (! best.isValid() || score < bestScore) { best = cand1; bestScore = score; }
            if (mx <= (np == 3 ? 0.5f : 0.42f) && mn >= 0.08f) break;
        }

        {   // λεπτός κόκκος
            Image::BitmapData bd (best, Image::BitmapData::readWrite);
            for (int y = 0; y < N; ++y)
                for (int x = 0; x < N; ++x)
                {
                    const auto c = bd.getPixelColour (x, y);
                    const int dn = rnd.nextInt (11) - 5;
                    bd.setPixelColour (x, y, Colour ((uint8) jlimit (0, 255, (int) c.getRed() + dn), (uint8) jlimit (0, 255, (int) c.getGreen() + dn), (uint8) jlimit (0, 255, (int) c.getBlue() + dn), (uint8) 255));
                }
        }
        return best;
    }

    // Εικόνα από τον φάκελο του χρήστη (Music/Flow/Orb): τυχαίο αρχείο, κεντραρισμένο τετράγωνο, βαμμένο ολόκληρο με χρώματα Wada.
    // Το φωτεινό περιεχόμενο της εικόνας (luma, με αυτόματη επέκταση 2%–98%) αντιστοιχίζεται σε διαβάθμιση ανάμεσα στα 3-4 χρώματα ενός συνδυασμού Wada, ταξινομημένα από σκούρο σε ανοιχτό. Στο τέλος ο ίδιος λεπτός κόκκος με τη γεννήτρια.
    // Αν ο φάκελος δεν υπάρχει ή δεν έχει εικόνες, επιστρέφει κενή εικόνα (τότε δουλεύει η γεννήτρια).
    static Image orbFromFolder (Random& rnd, StringArray& used)
    {
        constexpr int N = 512;
        const auto dir = File::getSpecialLocation (File::userMusicDirectory).getChildFile ("Flow").getChildFile ("Orb");
        if (! dir.isDirectory()) return {};
        Array<File> files;
        for (const auto& ext : { "*.png", "*.jpg", "*.jpeg" }) dir.findChildFiles (files, File::findFiles, false, ext);
        if (files.isEmpty()) return {};
        // Χωρίς επανάληψη: διαλέγει μόνο από όσες δεν έχουν χρησιμοποιηθεί ακόμα· όταν τελειώσουν, ξαναρχίζει (χωρίς να βάλει πρώτη την τελευταία)
        auto unused = [&] { Array<File> u; for (const auto& f : files) if (! used.contains (f.getFileName())) u.add (f); return u; };
        auto pool = unused();
        if (pool.isEmpty())
        {
            const String last = used.isEmpty() ? String() : used[used.size() - 1];
            used.clear();
            for (const auto& f : files) if (files.size() < 2 || f.getFileName() != last) pool.add (f);
        }
        const File chosen = pool[rnd.nextInt (pool.size())];
        used.add (chosen.getFileName());
        const Image src = ImageFileFormat::loadFrom (chosen);
        if (! src.isValid() || src.getWidth() < 2 || src.getHeight() < 2) return {};

        Image sq (Image::RGB, N, N, true);   // κεντραρισμένο τετράγωνο, διάφανες περιοχές πάνω σε λευκό
        {
            Graphics g (sq);
            g.fillAll (Colours::white);
            g.setImageResamplingQuality (Graphics::highResamplingQuality);
            const int side = jmin (src.getWidth(), src.getHeight());
            const Image crop = src.getClippedImage (Rectangle<int> ((src.getWidth() - side) / 2, (src.getHeight() - side) / 2, side, side));
            g.drawImage (crop, Rectangle<float> (0.0f, 0.0f, (float) N, (float) N));
        }

        int cand[348], nc = 0;
        for (int i = 0; i < 348; ++i) if (wada::kCombos[i].n >= 3) cand[nc++] = i;
        const auto& combo = wada::kCombos[cand[rnd.nextInt (nc)]];
        const int np = combo.n;
        Colour pal[4];
        for (int i = 0; i < np; ++i) pal[i] = Colour ((uint32) (0xff000000u | wada::kColours[combo.idx[i]]));
        auto luma = [] (Colour c) { return 0.299f * c.getFloatRed() + 0.587f * c.getFloatGreen() + 0.114f * c.getFloatBlue(); };
        for (int i = 1; i < np; ++i)   // ταξινόμηση από σκούρο σε ανοιχτό
            for (int j = i; j > 0 && luma (pal[j]) < luma (pal[j - 1]); --j) std::swap (pal[j], pal[j - 1]);

        std::vector<float> L ((size_t) (N * N));
        int hist[256] = {};
        {
            Image::BitmapData bd (sq, Image::BitmapData::readOnly);
            for (int y = 0; y < N; ++y)
                for (int x = 0; x < N; ++x)
                {
                    const float l = luma (bd.getPixelColour (x, y));
                    L[(size_t) (y * N + x)] = l;
                    ++hist[jlimit (0, 255, (int) (l * 255.0f))];
                }
        }
        auto percentile = [&] (float f) { int acc = 0; const int target = (int) (f * (float) (N * N)); for (int i = 0; i < 256; ++i) { acc += hist[i]; if (acc >= target) return (float) i / 255.0f; } return 1.0f; };
        const float lo = percentile (0.02f), hi = jmax (lo + 0.05f, percentile (0.98f));

        Image out (Image::RGB, N, N, false);
        Image::BitmapData bd (out, Image::BitmapData::writeOnly);
        for (int y = 0; y < N; ++y)
            for (int x = 0; x < N; ++x)
            {
                const float t = jlimit (0.0f, 1.0f, (L[(size_t) (y * N + x)] - lo) / (hi - lo)) * (float) (np - 1);
                const int i0 = jmin (np - 2, (int) t);
                const Colour c = pal[i0].interpolatedWith (pal[i0 + 1], t - (float) i0);
                const int dn = rnd.nextInt (11) - 5;
                bd.setPixelColour (x, y, Colour ((uint8) jlimit (0, 255, (int) c.getRed() + dn), (uint8) jlimit (0, 255, (int) c.getGreen() + dn), (uint8) jlimit (0, 255, (int) c.getBlue() + dn)));
            }
        return out;
    }

    void autoOrbImage()
    {
        Random rnd;
        auto* st = properties.getUserSettings();
        StringArray used = StringArray::fromLines (st->getValue ("orbUsed"));
        Image im = orbFromFolder (rnd, used);
        st->setValue ("orbUsed", used.joinIntoString ("\n"));                    // 1) δική σου εικόνα από Music/Flow/Orb, βαμμένη με Wada
        if (! im.isValid()) im = orbGenerated (rnd);       // 2) αλλιώς η γεννήτρια
        setOrbImage (im, false);
    }

    void pickOrbImage()
    {
        orbChooser = std::make_unique<FileChooser> (gr ("Choose an image for the orb"), File::getSpecialLocation (File::userPicturesDirectory), "*.png;*.jpg;*.jpeg");
        orbChooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles, [this] (const FileChooser& fc)
        {
            const auto f = fc.getResult();
            if (f == File()) return;
            auto img = ImageFileFormat::loadFrom (f);
            if (img.isValid()) setOrbImage (orbFit (img)); else setMessage (gr ("Could not read the image"));
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
    // ελαστική κίνηση: η τελεία ακολουθεί το δάχτυλο σαν μάζα σε ελατήριο, αργά και ομαλά (60 φορές το δευτερόλεπτο)
    // κίνηση ανά block: κάθε block συνεχίζει τα σχέδιά του (Motion 1..7) και όταν επιλέγεις άλλο block
    std::array<std::array<Point<float>, 4>, (size_t) flow::numBlocks> orbGoal, orbVel;   // [block][γωνία]
    std::array<std::array<bool, 4>, (size_t) flow::numBlocks> orbSettle {};
    bool orbRecArmed = false;

    // κινήσεις: κάθε πάτημα στον δακτύλιο της γωνίας δίνει την επόμενη (1..7), το όγδοο σβήνει· ο στόχος κινείται σε μοτίβο και η τελεία τον ακολουθεί ελαστικά
    static constexpr int orbPatterns = 7;   // κύκλος, οκτώ, σάρωση, σπείρα, παλμός, περιπλάνηση, άλμα και επιστροφή
    static constexpr double orbAutoPeriod = 8.0;   // δευτερόλεπτα ανά κύκλο
    std::array<std::array<int, 4>, (size_t) flow::numBlocks> orbAutoPat;          // [block][γωνία], -1 = καμία κίνηση
    std::array<std::array<double, 4>, (size_t) flow::numBlocks> orbAutoT0 {};
    std::array<std::array<std::array<float, 4>, 4>, (size_t) flow::numBlocks> orbAutoPh {};
    std::array<std::array<std::array<Point<float>, 4>, 4>, (size_t) flow::numBlocks> orbAutoTg {};

    void orbAutoStop (int b, int c)
    {
        if (b < 0 || b >= flow::numBlocks || orbAutoPat[(size_t) b][(size_t) c] < 0) return;
        orbAutoPat[(size_t) b][(size_t) c] = -1;
        if (isActive (b))
        {
            auto& l = orbLane[(size_t) b][(size_t) c];
            if (l.recording) orbFinishLane (l);
        }
    }

    void orbAutoPress (int c)
    {
        if (selected < 0 || selected >= flow::numBlocks) return;
        const size_t b = (size_t) selected;
        auto& rnd = Random::getSystemRandom();
        const int next = orbAutoPat[b][(size_t) c] + 1;   // κάθε πάτημα: επόμενη κίνηση· μετά την τελευταία σβήνει
        if (next >= orbPatterns) { orbAutoStop (selected, c); syncOrbAuto(); setMessage (gr ("Motion: off")); return; }
        orbAutoPat[b][(size_t) c] = next;
        orbAutoT0[b][(size_t) c] = Time::getMillisecondCounterHiRes() * 0.001;
        for (auto& v : orbAutoPh[b][(size_t) c]) v = rnd.nextFloat() * MathConstants<float>::twoPi;
        for (auto& t : orbAutoTg[b][(size_t) c]) t = { rnd.nextFloat() * 1.4f - 0.7f, rnd.nextFloat() * 1.4f - 0.7f };
        orbGoal[b][(size_t) c] = orbPos[b][(size_t) c];
        orbSettle[b][(size_t) c] = true;
        if (orbRecArmed && isActive (selected))   // Rec motion: γράφεται και η αυτόματη κίνηση
        {
            auto& l = orbLane[b][(size_t) c];
            if (l.s.empty()) { l.s.assign ((size_t) orbN, {}); l.ok.assign ((size_t) orbN, 0); }
            l.recording = true;
        }
        setMessage ("Motion " + String (orbAutoPat[b][(size_t) c] + 1) + " / " + String (orbPatterns));
    }

    Point<float> orbAutoGoal (int b, int c) const
    {
        const double now = Time::getMillisecondCounterHiRes() * 0.001;
        const float u = (float) std::fmod ((now - orbAutoT0[(size_t) b][(size_t) c]) / orbAutoPeriod, 1.0);
        const float T = MathConstants<float>::twoPi * u;
        static const float baseA[4] { -MathConstants<float>::halfPi, 0.0f, MathConstants<float>::halfPi, MathConstants<float>::pi };
        const float a = baseA[c];
        const auto& ph = orbAutoPh[(size_t) b][(size_t) c];
        switch (orbAutoPat[(size_t) b][(size_t) c])
        {
            case 0: return { 0.55f * std::cos (T + a), 0.55f * std::sin (T + a) };
            case 1: { const float x = 0.65f * std::sin (T), y = 0.4f * std::sin (2.0f * T);
                      return { x * std::cos (a) - y * std::sin (a), x * std::sin (a) + y * std::cos (a) }; }
            case 2: { const float r = 0.85f * std::sin (T); return { r * std::cos (a), r * std::sin (a) }; }
            case 3: { const float r = 0.15f + 0.7f * (0.5f - 0.5f * std::cos (T)); return { r * std::cos (2.0f * T + a), r * std::sin (2.0f * T + a) }; }
            case 4: { const float r = 0.45f + 0.4f * std::sin (2.0f * T); return { r * std::cos (a), r * std::sin (a) }; }
            case 5: return { 0.6f * std::sin (T + ph[0]) + 0.2f * std::sin (3.0f * T + ph[1]),
                             0.6f * std::sin (T + ph[2] + 1.3f) + 0.2f * std::sin (2.0f * T + ph[3]) };
            default: { const int st = (int) (u * 4.0f) % 4;
                       return st % 2 ? orbDefault (c) : orbAutoTg[(size_t) b][(size_t) c][(size_t) st]; }
        }
    }

    void syncOrbAuto()
    {
        const bool ok = selected >= 0 && selected < flow::numBlocks;
        const auto& pt = orbAutoPat[(size_t) (ok ? selected : 0)];
        loopsView.setOrbAuto ({ { ok && pt[0] >= 0, ok && pt[1] >= 0, ok && pt[2] >= 0, ok && pt[3] >= 0 } });
    }

    static Point<float> orbClamp (int, Point<float> p)   // ελεύθερη κίνηση σε όλον τον orb (όριο: ο κύκλος)
    {
        const float len = p.getDistanceFromOrigin();
        return len > 1.0f ? Point<float> (p.x / len, p.y / len) : p;
    }
    // αρχική θέση: ρόμβος (πάνω, δεξιά, κάτω, αριστερά), στην ίδια απόσταση από το κέντρο όπως πριν (0.5)
    static Point<float> orbDefault (int corner)
    {
        static const Point<float> d[4] { { 0.0f, -0.5f }, { 0.5f, 0.0f }, { 0.0f, 0.5f }, { -0.5f, 0.0f } };
        return d[jlimit (0, 3, corner)];
    }
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
        if (down) orbAutoStop (selected, corner);   // πιάνεις την τελεία: σταματά η τυχαία κίνηση
        orbTouching[(size_t) corner] = down;
        if (down && selected >= 0 && selected < flow::numBlocks)
        {
            orbGoal[(size_t) selected][(size_t) corner] = orbPos[(size_t) selected][(size_t) corner];   // χωρίς άλμα στο πάτημα
            orbVel[(size_t) selected][(size_t) corner] = {};
            orbSettle[(size_t) selected][(size_t) corner] = true;
        }
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
        orbGoal[(size_t) selected][(size_t) corner] = orbClamp (corner, p);   // στόχος: η θέση του δαχτύλου· η τελεία πάει εκεί ελαστικά
        orbSettle[(size_t) selected][(size_t) corner] = true;
    }

    void orbPhysics()
    {
        const float dt = 1.0f / 60.0f, omega = 6.0f, zeta = 0.62f;   // ω: ταχύτητα, ζ < 1: λίγη ελαστικότητα
        std::array<Point<float>, 4> selPos;
        bool anySel = false;
        for (int b = 0; b < flow::numBlocks; ++b)      // όλα τα block κινούνται, όχι μόνο το επιλεγμένο
        {
            const size_t bb = (size_t) b;
            for (int c = 0; c < 4; ++c)
            {
                const size_t cc = (size_t) c;
                auto& pp = orbPos[bb][cc];
                const bool touching = b == selected && orbTouching[cc];
                if (orbAutoPat[bb][cc] >= 0 && ! touching && isActive (b)) { orbGoal[bb][cc] = orbClamp (c, orbAutoGoal (b, c)); orbSettle[bb][cc] = true; }
                if (orbSettle[bb][cc])
                {
                    const auto acc = (orbGoal[bb][cc] - pp) * (omega * omega) - orbVel[bb][cc] * (2.0f * zeta * omega);
                    orbVel[bb][cc] += acc * dt;
                    pp = orbClamp (c, pp + orbVel[bb][cc] * dt);
                    if (isActive (b)) orbApply (b, c, pp);
                    if (b == selected) anySel = true;
                    if (! touching && orbAutoPat[bb][cc] < 0 && (orbGoal[bb][cc] - pp).getDistanceFromOrigin() < 0.002f && orbVel[bb][cc].getDistanceFromOrigin() < 0.01f)
                        orbSettle[bb][cc] = false;
                }
                if (b == selected) selPos[cc] = pp;
            }
        }
        if (anySel) loopsView.setOrbPos (selPos);
        syncOrbAuto();
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
                const bool touching = (b == selected && orbTouching[(size_t) c]) || orbSettle[(size_t) b][(size_t) c];
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
            else
            {
                int t = knobTouched.load();
                bool viaFallback = false;
                if (t < 0 && Time::getMillisecondCounter() - knobStageStart > 5000)   // plugin χωρίς gestures: δέχεται την πρώτη αλλαγή τιμής
                    for (auto& w : watches)
                        if (w.plugin != nullptr && w.gestureCount.load() == 0 && w.fallback.load() >= 0)
                        { t = (w.block << 20) | w.fallback.load(); viaFallback = true; }
                if (t >= 0)
                {
                    int ign = 0;
                    for (auto& w : watches) if (w.plugin != nullptr) ign += w.ignoredCount.load();
                    for (auto& w : watches) w.detach();
                    const int tb = t >> 20, ti = t & 0xfffff;
                    if (tb >= 0 && tb < flow::numBlocks)
                        if (auto* tp = engine.getPlugin (tb))
                            if (ti < tp->getParameters().size())
                            {
                                const auto nm = tp->getParameters()[ti]->getName (64);
                                blockKnobs[(size_t) tb][(size_t) knobSlot] = { ti, nm };
                                rememberKnobs (tb);
                                setMessage (nm.substring (0, 40));
                                flow::flowLog ("orb learn: slot " + String (knobSlot) + " block " + String (tb + 1) + " -> param " + String (ti) + " '" + nm + "' ("
                                               + (viaFallback ? "no gesture, first value change" : "user gesture") + "), ignored changes of assigned params: " + String (ign));
                            }
                    knobStage = 0; knobSlot = -1;
                    saveNow();
                    if (settingsScreen.isVisible()) settingsScreen.repaint();
                }
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
        // τα Start quantize, Length quantize και MIDI learn είναι πια στις ρυθμίσεις (Loop recording, MIDI learn)
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

    // «My plugins»: τα AU κρύβονται όταν είναι off (προεπιλογή), και κάθε plugin μπορεί να σβηστεί μόνο του (τα νέα είναι πάντα επιλεγμένα)
    static String pluginKey (const PluginDescription& d) { return d.name + "|" + d.pluginFormatName; }
    bool pluginAllowed (const PluginDescription& d) const
    {
        if (d.pluginFormatName == "AudioUnit" && ! auOn) return false;
        return ! pluginOff.contains (pluginKey (d));
    }
    StringArray pluginOff;
    bool auOn = false;

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

        // Κάθε λίστα δείχνει ΟΛΑ τα plugins: πρώτα αυτά που ταιριάζουν στο κουμπί, μετά τα υπόλοιπα με την ένδειξη «other»
        // (έτσι τίποτα δεν κρύβεται αν ένα plugin δηλώνει λάθος τον τύπο του).
        //   VST: instruments με έξοδο ήχου · FX: effects ήχου · MIDI FX: χωρίς έξοδο ήχου ή με κατηγορία midi
        auto fits = [kind] (const PluginDescription& d)
        {
            const bool noOut = d.numOutputChannels == 0, midiCat = d.category.containsIgnoreCase ("midi");
            if (kind == 0) return d.isInstrument && ! noOut;
            if (kind == 1) return ! d.isInstrument && ! noOut && ! midiCat;
            return noOut || midiCat;
        };
        std::vector<PluginDescription> primary, others;
        for (const auto& d : knownPlugins.getTypes())
        {
            flow::flowLog ("plugin: " + d.name + " [" + d.pluginFormatName + "] instrument=" + String ((int) d.isInstrument) + " in=" + String (d.numInputChannels)
                           + " out=" + String (d.numOutputChannels) + " category=" + d.category);
            if (! pluginAllowed (d)) continue;
            const int ut = flow::pluginTypeByName (d.name);   // τύπος που όρισε ο χρήστης: μόνο στη λίστα του· χωρίς τύπο: φαίνεται σε όλες
            if (ut >= 0 && ut != kind) continue;
            (ut == kind || (ut < 0 && fits (d)) ? primary : others).push_back (d);
        }
        auto byName = [] (const PluginDescription& a, const PluginDescription& b) { return a.name.compareIgnoreCase (b.name) < 0; };
        std::sort (primary.begin(), primary.end(), byName);
        std::sort (others.begin(), others.end(), byName);
        std::vector<PluginDescription> list = primary;
        list.insert (list.end(), others.begin(), others.end());

        // η λίστα εμφανίζεται μέσα στην οθόνη των loops (στη θέση τους): διάλεξε όνομα, ↻ νέα σάρωση, … αρχείο, ✕ κλείσιμο
        StringArray names, formats, keys;
        for (size_t i = 0; i < list.size(); ++i)
        {
            const auto& d = list[i];
            names.add (d.name);
            formats.add (d.pluginFormatName + (i >= primary.size() ? String::fromUTF8 (" \xc2\xb7 other") : String()));
            keys.add (d.createIdentifierString());
        }

        const Colour kc = kind == 0 ? hobby::kOn : kind == 1 ? hobby::kPeach : hobby::kLilac;
        const String kt = kind == 0 ? "VST" : kind == 1 ? "FX" : "MIDI FX";
        const String ks = kind == 0 ? "instruments" : "effects";

        loopsView.getList().thumbFn = [this] (const String& n) { return thumbFor (n); };
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

    bool isEmptyZone (int b) const { return b >= 0 && b < flow::numBlocks && engine.emptyZone[(size_t) b].load() != 0; }

    // zones first: το Block ετοιμάζει ένα νέο block που περιμένει ζώνη (πρώτο και τελευταίο πλήκτρο), χωρίς plugin
    void startZoneFirst()
    {
        if (engine.pendingBlock() >= 0) { setMessage (gr ("Set the zone: press the first and the last key")); return; }
        const int slot = freeSlot();
        if (slot < 0) { setMessage (gr ("No free block (maximum 9).")); return; }
        engine.reserveZone (slot);
        selected = slot;
        engine.selectedBlock = slot;
        refreshAll();
        setMessage (gr ("Set the zone: press the first and the last key"));
    }

    int freeSlot() const
    {
        for (int b = 0; b < flow::numBlocks; ++b)
            if (! isActive (b) && ! isEmptyZone (b) && ! loadingSlots.contains (b))
                return b;
        return -1;
    }

    // intoZone: το plugin μπαίνει σε block που έχει ήδη ζώνη (zones first), χωρίς νέο ορισμό ζώνης
    void chooseAndLoadPlugin (bool intoZone = false)
    {
        int zoneSlot = -1;
        if (intoZone)
        {
            if (isEmptyZone (selected) && engine.zoneState[(size_t) selected].load() == 0) zoneSlot = selected;
            else for (int b = 0; b < flow::numBlocks && zoneSlot < 0; ++b)
                if (isEmptyZone (b) && engine.zoneState[(size_t) b].load() == 0) zoneSlot = b;
        }
        if (zoneSlot >= 0)
        {
            pickPlugin (gr ("Choose a plugin"), 0, [this, zoneSlot] (const PluginDescription& d)
            {
                loadPlugin (zoneSlot, d, MemoryBlock(), true, false, nullptr);
            });
            return;
        }
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

    // σημάδι ασφαλείας (crash): μικρό ξεχωριστό αρχείο, ώστε να μη ξαναγράφεται το μεγάλο αρχείο ρυθμίσεων σε κάθε φόρτωση plugin
    File loadingFile() { return properties.getUserSettings()->getFile().getSiblingFile ("Flow-loading.txt"); }

    void setLoading (const String& token, bool loading)
    {
        loadingTokens.removeString (token);
        if (loading) loadingTokens.add (token);
        loadingFile().replaceWithText (loadingTokens.joinIntoString (","));
    }

    // μία αποθήκευση όταν τελειώσουν όλες οι φορτώσεις (όχι μία ανά plugin)
    void saveAfterLoad()
    {
        if (pendingLoads > 0) return;
        if (setLoadT0 != 0)
        {
            flow::flowLog ("set loaded in " + String ((int) (Time::getMillisecondCounter() - setLoadT0)) + " ms");
            setLoadT0 = 0;
        }
        saveNow (true);
    }

    // χρόνος δημιουργίας plugin: τα plugins δημιουργούνται ένα-ένα στο message thread, άρα ο δικός του χρόνος είναι από την προηγούμενη ολοκλήρωση
    void logLoadTime (const String& token, const String& name)
    {
        const auto now = Time::getMillisecondCounter();
        const auto start = loadStartMs.count (token) ? loadStartMs[token] : now;
        const auto own = now - jmax (start, lastLoadDoneMs);
        lastLoadDoneMs = now;
        loadStartMs.erase (token);
        flow::flowLog ("load " + token + " " + name + ": " + String ((int) own) + " ms (waited " + String ((int) (now - start)) + " ms)");
    }

    // userLoad: το plugin φορτώθηκε τώρα από τον χρήστη -> νέο block, επιλέγεται και περιμένει ζώνη
    void loadPlugin (int block, const PluginDescription& description, const MemoryBlock& state, bool userLoad, bool captureZone,
                     std::function<void()> after)
    {
        const String token = "b" + String (block);
        loadStartMs[token] = Time::getMillisecondCounter();
        if (! userLoad) setMessage (gr ("Loading: ") + description.name);
        setLoading (token, true);   // σημάδι ασφαλείας: αν κρασάρει, δεν ξαναφορτώνεται μόνο του
        loadingSlots.addIfNotAlreadyThere (block);
        if (block >= 0 && block < flow::numBlocks) loadingName[(size_t) block] = description.name;
        ++pendingLoads;

        SafePointer<MainComponent> safeThis (this);

        formatManager.createPluginInstanceAsync (description, engine.getSampleRate(), currentBufferSize(),
            [safeThis, block, token, state, userLoad, captureZone, after] (std::unique_ptr<AudioPluginInstance> instance, const String& error)
            {
                if (safeThis != nullptr)
                    safeThis->pluginLoaded (block, token, std::move (instance), error, state, userLoad, captureZone, after);
            });
    }

    // ο τύπος ήχου που διάλεξες για ένα όργανο (π.χ. Serum 2) θυμάται και μπαίνει μόνος του όταν το ξαναφορτώσεις
    int rememberedSmartType (const String& plugin) const
    {
        auto* settings = const_cast<juce::ApplicationProperties&> (properties).getUserSettings();
        if (plugin.isEmpty() || settings == nullptr) return 0;
        return jlimit (0, 8, settings->getIntValue ("stype:" + plugin, 0));
    }
    void rememberSmartType (int b, int t)
    {
        auto* settings = properties.getUserSettings();
        if (settings == nullptr) return;
        if (auto* pl = engine.getPlugin (b)) settings->setValue ("stype:" + pl->getName(), jlimit (0, 8, t));
    }
    void pluginLoaded (int block, const String& token, std::unique_ptr<AudioPluginInstance> instance, const String& error,
                       const MemoryBlock& state, bool userLoad, bool captureZone, std::function<void()> after)
    {
        setLoading (token, false);
        loadingSlots.removeFirstMatchingValue (block);
        --pendingLoads;
        const String instName = instance != nullptr ? instance->getName() : String();
        logLoadTime (token, instance != nullptr ? instance->getName() : String ("(failed)"));

        if (instance == nullptr)
        {
            loadFailed = true;
            setMessage (gr ("Load failed: ") + error);
            saveAfterLoad();
            return;
        }

        closeEditor();

        if (userLoad)    // νέο block με καθαρές ρυθμίσεις
        {
            engine.holdMode[(size_t) block] = 0;
            engine.chordMode[(size_t) block] = 0; engine.chordTonic[(size_t) block] = -1; engine.blockOff[(size_t) block] = 0;
            engine.curveMode[(size_t) block] = 0;
            engine.followBlock[(size_t) block] = 1; engine.followPolyBlock[(size_t) block] = 0; engine.smartType[(size_t) block] = rememberedSmartType (instName); if (engine.smartType[(size_t) block].load() == 6) engine.followBlock[(size_t) block] = 0; engine.smartResetApp (block); for (int oc = 0; oc < 4; ++oc) { orbAutoPat[(size_t) block][(size_t) oc] = -1; orbSettle[(size_t) block][(size_t) oc] = false; }
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
        saveAfterLoad();
    }

    // aux plugin (MIDI FX / FX): φορτώνεται και παραδίδεται στο done
    void loadAux (const PluginDescription& d, const MemoryBlock& state, const String& token, std::function<void (PluginPtr)> done)
    {
        flow::flowLog ("loadAux: " + d.name + " (" + d.pluginFormatName + ") token=" + token);
        loadStartMs[token] = Time::getMillisecondCounter();
        setLoading (token, true);
        ++pendingLoads;
        SafePointer<MainComponent> safe (this);

        formatManager.createPluginInstanceAsync (d, engine.getSampleRate(), currentBufferSize(),
            [safe, token, state, done] (std::unique_ptr<AudioPluginInstance> inst, const String& error)
            {
                if (safe == nullptr) return;
                safe->setLoading (token, false);
                --safe->pendingLoads;
                safe->logLoadTime (token, inst != nullptr ? inst->getName() : String ("(failed)"));
                if (inst == nullptr)
                {
                    safe->loadFailed = true;
                    safe->setMessage (gr ("Load failed: ") + error);
                    safe->saveAfterLoad();
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
                saveAfterLoad();
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
                saveAfterLoad();
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
        if (block >= 0 && block < flow::numBlocks) { paletteKept[(size_t) block] = false; paletteFile[(size_t) block] = String(); }
        if (isEmptyZone (block))   // zones first: σβήνει τη ζώνη που δεν έχει plugin
        {
            engine.clearEmptyZone (block);
            if (selected == block) { selected = firstActive(); engine.selectedBlock = selected; }
            if (save) { updateEditor(); refreshAll(); saveNow(); }
            return;
        }
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
        if (engine.specialBlock2.load() == block) engine.specialBlock2 = -1;
        if (engine.mfBlock.load() == block) engine.mfBlock = -1;
        engine.liveSilent[(size_t) block] = 1;
        engine.holdMode[(size_t) block] = 0;
        engine.chordMode[(size_t) block] = 0; engine.chordTonic[(size_t) block] = -1; engine.blockOff[(size_t) block] = 0;
        engine.curveMode[(size_t) block] = 0;
        engine.followBlock[(size_t) block] = 1; engine.followPolyBlock[(size_t) block] = 0; engine.smartType[(size_t) block] = 0; engine.smartResetApp (block); for (int oc = 0; oc < 4; ++oc) { orbAutoPat[(size_t) block][(size_t) oc] = -1; orbSettle[(size_t) block][(size_t) oc] = false; }
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

    void toggleSpecial()
    {
        if (! isActive (selected)) return;
        auto& en = engine;
        const int s = selected;
        if (en.specialBlock.load() == s) en.specialBlock = -1;
        else if (en.specialBlock2.load() == s) en.specialBlock2 = -1;
        else if (en.specialBlock.load() < 0) en.specialBlock = s;
        else if (en.specialBlock2.load() < 0) en.specialBlock2 = s;
        else { setMessage (gr ("Two special blocks are already set. Turn one off first")); return; }
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
            if (r >= 70 && r <= 71) e.autoLoop = r - 70;
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

        add (String::fromUTF8 ("\xe2\x88\x9e"), "Ableton Link" + (LinkBridge::isAvailable() ? String() : gr (" (not included in this build)")),
             act ([this]
             {
                 const bool on = ! engine.isLinkEnabled();
                 engine.setLinkEnabled (on);
                 setMessage (on ? gr ("Ableton Link: on") : gr ("Ableton Link: off"));
             }), LinkBridge::isAvailable(), engine.isLinkEnabled());
        {
            LoopsView::MenuEntry m; m.glyph = "!"; m.text = "Panic";
            { LoopsView::MenuEntry sj; sj.text = gr ("Silence stuck notes (Esc)"); sj.fn = [this] { doPanic(); }; m.sub.push_back (std::move (sj)); }
            { LoopsView::MenuEntry sj; sj.text = gr ("Assign pedal or key (MIDI learn)"); sj.fn = [this] { startPedalLearn (flow::ctlPanic); }; m.sub.push_back (std::move (sj)); }
            { LoopsView::MenuEntry sj; sj.text = gr ("Remove assignment"); sj.enabled = engine.mapCode[flow::ctlPanic].load() != 0;
              sj.fn = [this] { engine.mapCode[flow::ctlPanic] = 0; saveNow(); }; m.sub.push_back (std::move (sj)); }
            items.push_back (std::move (m));
        }

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
        r.push_back ({ "Flow", JUCEApplication::getInstance() != nullptr ? JUCEApplication::getInstance()->getApplicationVersion() : String() });
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
        r.push_back ({ "Link", engine.isLinkEnabled() ? "on" : "off" });
        {
            auto folder = engine.recorder.getFolder().getFullPathName();
            const auto home = File::getSpecialLocation (File::userHomeDirectory).getFullPathName();
            if (home.isNotEmpty() && folder.startsWith (home)) folder = "~" + folder.substring (home.length());
            r.push_back ({ "Rec Audio", folder });
        }
        r.push_back ({ "Key lights", keyLights.isOn() ? "on" : "off" });
        return r;
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
        engine.metroOn = false;   // ο ήχος μετρονόμου αφαιρέθηκε
        return;
        if (! engine.metroOn.load() && engine.numOutChannels.load() <= engine.metroChannel.load())
        {
            setMessage (gr ("Metronome: output ") + String (engine.metroChannel.load() + 1)
                        + gr (" does not exist. Open the settings (sliders button) and enable more outputs"));
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
    void refreshSmartTile()
    {
        const Colour white (0xfff3f4f6), dim = hobby::kIconGrey.withAlpha (0.4f);
        motionTile.setIconColour (orbRecArmed ? hobby::kRed : dim);
        folderTile.setIconColour (settingsScreen.isVisible() ? white : dim);
        smartTile.setIconColour (settingsScreen.isSmartDirect() ? white : dim);
    }

    void openSettingsScreen()
    {
        loopsView.openMenu (false);
        if (audioPanel.isVisible()) audioPanel.close();
        settingsScreen.setBounds (loopsBezel.reduced (5));
        settingsScreen.open();
        settingsScreen.grabKeyboardFocus();
    }

    void setupSettingsScreen()
    {
        auto& h = settingsScreen.host;
        settingsScreen.setWantsKeyboardFocus (true);
        settingsScreen.onChanged = [this] { saveNow(); };
        settingsScreen.onClose = [this] { grabKeyboardFocus(); refreshSmartTile(); repaint(); };
        h.openAudio = [this] { settingsScreen.hideForPanel(); audioFromSettings = true; openAudioSettings(); };
        h.audioSummary = [this] { auto* d = deviceManager.getCurrentAudioDevice(); return d != nullptr ? d->getName() : String(); };
        h.currentSetName = [this] { return currentSetName; };
        h.listSets = [this]
        {
            std::vector<SettingsScreen::SetInfo> out;
            Array<File> files = setsDir().findChildFiles (File::findFiles, false, "*.flowset");
            struct Newest { static int compareElements (const File& a, const File& b)
            { return a.getLastModificationTime() < b.getLastModificationTime() ? 1 : (b.getLastModificationTime() < a.getLastModificationTime() ? -1 : 0); } };
            Newest sorter; files.sort (sorter);
            for (auto& f : files)
            {
                SettingsScreen::SetInfo si;
                si.name = f.getFileNameWithoutExtension();
                si.date = f.getLastModificationTime().formatted ("%d %b %Y");
                si.current = si.name == currentSetName;
                si.thumb = setThumbnail (f);
                si.file = f;
                out.push_back (std::move (si));
            }
            return out;
        };
        h.saveSetNamed = [this] (const String& name)
        {
            auto dir = setsDir();
            dir.createDirectory();
            auto f = dir.getChildFile (File::createLegalFileName (name)).withFileExtension ("flowset");
            auto xml = createSetXml (true, true);
            if (xml->writeTo (f)) { currentSetName = f.getFileNameWithoutExtension(); setMessage (gr ("Saved: ") + f.getFileName()); }
            else                  setMessage (gr ("Could not write the file"));
        };
        h.saveElsewhere = [this] { saveSetToFile(); };
        h.loadElsewhere = [this] { loadSetFromFile(); };
        h.loadSet = [this] (const File& f) { loadSetFile (f); saveNow(); };
        h.deleteSet = [this] (const File& f)
        {
            const auto name = f.getFileNameWithoutExtension();
            if (f.moveToTrash() || f.deleteFile())
            {
                if (name == currentSetName) currentSetName = {};
                setMessage (gr ("Deleted (in the Trash): ") + name);
            }
            else setMessage (gr ("Could not delete the set"));
        };
        h.orbImage = [this] { return orbImage; };
        h.sessionInfo = [this]() -> String
        {
            std::unique_ptr<XmlElement> xml (properties.getUserSettings()->getXmlValue ("sessionBackup"));
            if (xml == nullptr || ! xml->hasTagName ("FlowSet") || xml->getChildByName ("Block") == nullptr) return {};
            int n = 0;
            for (auto* b : xml->getChildWithTagNameIterator ("Block")) { (void) b; ++n; }
            return String (n) + (n == 1 ? " block" : " blocks") + "  " + String ((int) std::lround (xml->getDoubleAttribute ("tempo", 110.0))) + " BPM";
        };
        h.restoreSession = [this] { restoreLastSession(); };
        h.recFolder = [this] { return engine.recorder.getFolder(); };
        h.setRecFolder = [this] (const File& f) { f.createDirectory(); engine.recorder.setFolder (f); saveNow(); };
        h.chooseRecFolder = [this] { chooseTapeFolder(); };
        h.scanPlugins = [this] { if (scanning) return; knownPlugins.clear(); startPluginScan(); };
        h.scanBusy = [this] { return scanning; };
        h.plugins = [this]
        {
            std::vector<SettingsScreen::PluginRow> rows;
            StringArray seen;
            for (const auto& d : knownPlugins.getTypes())
            {
                if (d.pluginFormatName == "AudioUnit" && ! auOn) continue;
                const String key = pluginKey (d);
                if (seen.contains (key)) continue;
                seen.add (key);
                const int t = flow::pluginTypeByName (d.name);
                rows.push_back ({ d.name, d.pluginFormatName, key, t, ! pluginOff.contains (key) });
            }
            std::sort (rows.begin(), rows.end(), [] (const auto& a, const auto& b) { return a.name.compareIgnoreCase (b.name) < 0; });
            return rows;
        };
        h.auGet = [this] { return auOn; };
        h.auSet = [this] (bool on) { auOn = on; saveNow(); };
        h.pluginOnSet = [this] (const String& key, bool on)
        {
            if (on) pluginOff.removeString (key); else pluginOff.addIfNotAlreadyThere (key);
            saveNow();
        };
        h.pluginAllSet = [this] (bool on)
        {
            for (const auto& d : knownPlugins.getTypes())
            {
                if (d.pluginFormatName == "AudioUnit" && ! auOn) continue;
                if (on) pluginOff.removeString (pluginKey (d)); else pluginOff.addIfNotAlreadyThere (pluginKey (d));
            }
            saveNow();
        };
        h.pedalCcGet = [this] (int i) { return engine.pedalCc[(size_t) i].load(); };
        h.pedalLearning = [this] { return engine.pedalLearn.load(); };
        h.pedalLearn = [this] (int i)
        {
            if (engine.pedalLearn.load() == i) { engine.cancelLearns(); return; }
            engine.cancelLearns();
            engine.startPedalLearn (i);
            setMessage (gr ("Move pedal ") + String (i + 1));
        };
        h.pedalClear = [this] (int i) { engine.cancelLearns(); engine.pedalCc[(size_t) i] = -1; saveNow(); };
        h.masterGet = [this] { return engine.masterPos.load(); };
        h.masterSet = [this] (float v) { engine.masterPos = v; };
        h.masterCcText = [this]() -> String { const int c = engine.masterCc.load(); return c != 0 ? pedalCodeText (c) : String(); };
        h.masterAssign = [this] { engine.cancelLearns(); engine.masterLearn = 1; setMessage (gr ("Move the knob for the master volume")); };
        h.masterRemove = [this] { engine.masterCc = 0; saveNow(); };
        h.linkAvailable = [] { return LinkBridge::isAvailable(); };
        h.linkGet = [this] { return engine.isLinkEnabled(); };
        h.linkToggle = [this]
        {
            const bool on = ! engine.isLinkEnabled();
            engine.setLinkEnabled (on);
            setMessage (on ? gr ("Ableton Link: on") : gr ("Ableton Link: off"));
        };
        h.helpGet = [this] { return helpOn; };
        h.helpToggle = [this]
        {
            helpOn = ! helpOn;
            tooltipWindow.setMillisecondsBeforeTipAppears (helpOn ? 600 : 2000000000);
            if (auto* us = properties.getUserSettings()) { us->setValue ("helpOn", helpOn); us->saveIfNeeded(); }
            setMessage (helpOn ? String ("Help: on") : String ("Help: off"));
        };
        h.panicNow = [this] { doPanic(); };
        h.panicAssign = [this] { startPedalLearn (flow::ctlPanic); };
        h.panicRemove = [this] { engine.mapCode[flow::ctlPanic] = 0; saveNow(); };
        h.panicCodeText = [this]() -> String { const int c = engine.mapCode[flow::ctlPanic].load(); return c != 0 ? pedalCodeText (c) : String(); };
        h.startQGet = [this] { return engine.startQ.load(); };
        h.startQSet = [this] (int v) { engine.startQ = v; saveNow(); };
        h.lengthQGet = [this] { return engine.lengthQ.load(); };
        h.lengthQSet = [this] (int v) { engine.lengthQ = v; saveNow(); };
        h.loopFitGet = [this] { return engine.loopFit.load(); };
        h.loopFitSet = [this] (int v) { engine.loopFit = v; saveNow(); };
        h.tapMsGet = [this] { return engine.tapMs.load(); };
        h.tapMsSet = [this] (int v) { engine.tapMs = v; saveNow(); };
        h.smartBlocks = [this]
        {
            std::vector<SettingsScreen::SmartBlock> v ((size_t) flow::numBlocks);
            for (int b = 0; b < flow::numBlocks; ++b)
            {
                auto& x = v[(size_t) b];
                if (auto* p = engine.getPlugin (b)) { x.active = true; x.name = p->getName(); }
                x.type = engine.smartType[(size_t) b].load();
                x.autoType = engine.smartAutoT[(size_t) b].load();
                for (int k = 0; k < flow::smartBands; ++k)
                {
                    x.band[k] = engine.smartBand[(size_t) b][(size_t) k].load();
                    x.need[k] = engine.smartNeed[(size_t) b][(size_t) k].load();
                    x.app[k]  = engine.smartApp[(size_t) b][(size_t) k].load();
                }
                x.colour = blockLight (b);
                x.width = engine.smartWidth[(size_t) b].load();
                flow::smartLimits (x.type != 0 ? x.type : x.autoType, x.lim);
                x.needW = engine.smartNeedWidth[(size_t) b].load(); x.appW = engine.smartAppWidth[(size_t) b].load();
            }
            return v;
        };
        h.smartOnGet = [this] { return engine.smartOn.load(); };
        h.smartOnSet = [this] (int v) { engine.smartOn = v; saveNow(); };
        h.smartAcceptGet = [this] { return engine.smartAccept.load(); };
        h.smartAcceptSet = [this] (float v) { engine.smartAccept = v; saveNow(); };
        h.smartTypeSet = [this] (int b, int t)
        {
            engine.smartType[(size_t) b] = jlimit (0, 8, t);
            if (jlimit (0, 8, t) == 6) engine.followBlock[(size_t) b] = 0;   // Drums: αποκλείονται από το Follow
            engine.smartResetApp (b);   // νέος τύπος: καθαρή αρχή
            rememberSmartType (b, t);
            saveNow();
        };
        h.windowSizeGet = [this] { return windowSizeMode; };
        h.windowSizeSet = [this] (int v) { windowSizeMode = jlimit (0, 3, v); applyWindowSize (true); saveNow(); };
        h.windowSizeFits = [this] (int v) { return windowSizeFits (v); };
        h.autoLoopGet = [this] { return engine.autoLoop.load(); };
        h.autoLoopSet = [this] (int v) { engine.autoLoop = v; saveNow(); };
        h.snapQGet = [this] { return engine.snapQ.load(); };
        h.snapQSet = [this] (int v) { engine.snapQ = v; saveNow(); };
        h.tools = [this]
        {
            std::vector<SettingsScreen::ToolItem> l;
            if (! isActive (selected)) return l;
            const bool hasFx = engine.getMidiFx (selected) != nullptr;
            const String swap = String::fromUTF8 ("\xe2\x87\x84"), cross = String::fromUTF8 ("\xe2\x9c\x95");
            l.push_back ({ swap, "Instrument (VST)", {}, hobby::kOn, true, false, [this] { replaceInstrument(); } });
            l.push_back ({ swap, "MIDI FX: load", {}, hobby::kLilac, true, false, [this] { loadMidiFxInto (selected); } });
            l.push_back ({ cross, "MIDI FX: remove", {}, hobby::kLilac, hasFx, armedNow (1), [this] { pressDelete (1, [this] { removeMidiFx (selected); }); } });
            l.push_back ({ "+", "FX: add", {}, hobby::kPeach, engine.numFx (selected) < maxFxPerChain, false, [this] { addFxTo (selected); } });
            l.push_back ({ cross, "FX: clear all", {}, hobby::kPeach, engine.numFx (selected) > 0, armedNow (2), [this] { pressDelete (2, [this] { clearFx (selected); }); } });
            l.push_back ({ "SP", "Special block", {}, Colours::white, true, false, [this] { toggleSpecial(); } });
            l.push_back ({ cross, "Remove block", {}, hobby::kRed, true, armedNow (0), [this] { pressDelete (0, [this] { removeBlock (selected); }); } });
            return l;
        };
        h.orbKnobs = [this]
        {
            std::vector<SettingsScreen::OrbKnob> v;
            for (auto& k : loopsView.knobs) v.push_back ({ k.bound, k.name, k.value });
            return v;
        };
        h.orbAssign = [this] (int slot) { startKnobLearn (slot); };
        h.orbLearnSlot = [this] { return knobStage != 0 ? knobSlot : -1; };
        h.orbClear = [this] (int slot) { clearKnobSlot (slot); };
        h.learnStart = [this] (int mode)
        {
            learnMode = mode;
            stopKnobLearn();
            setMessage (mode == 1 ? gr ("Press the button or knob you want to assign") : gr ("Press the button or knob to remove its assignment"));
            updateToolLamps();
        };
    }

public:
    int getWindowSizeMode() const { return windowSizeMode; }
    static Point<int> windowSizeFor (int mode)
    {
        return mode == 0 ? Point<int> (880, 690) : mode == 2 ? Point<int> (1280, 940) : Point<int> (1097, 810);
    }
    static Rectangle<int> windowArea()
    {
        if (auto* d = Desktop::getInstance().getDisplays().getPrimaryDisplay()) return d->userArea;
        return { 0, 0, 1920, 1080 };
    }
    bool windowSizeFits (int mode) const
    {
        if (mode == 3) return true;
        const auto s = windowSizeFor (mode);
        const auto a = windowArea();
        return s.x <= a.getWidth() && s.y <= a.getHeight();
    }
    // εφαρμόζει το επιλεγμένο μέγεθος στο παράθυρο (resize = true: αλλάζει και το τρέχον μέγεθος)
    void applyWindowSize (bool resize)
    {
        auto* w = dynamic_cast<DocumentWindow*> (getTopLevelComponent());
        if (w == nullptr) return;
        if (windowSizeMode == 3)
        {
            w->setResizable (true, false);
            w->setResizeLimits (minWinW(), minWinH() + 28, 4000, 3000);
            return;
        }
        const auto a = windowArea();
        auto s = windowSizeFor (windowSizeMode);
        s.x = jmin (s.x, a.getWidth());
        s.y = jmin (s.y, a.getHeight());
        w->setResizeLimits (s.x, s.y, s.x, s.y);
        w->setResizable (false, false);
        if (resize) w->centreWithSize (s.x, s.y);
    }
private:

    void openAudioSettings()   // οι ρυθμίσεις ήχου και MIDI ανοίγουν μέσα στην οθόνη του Flow
    {
        loopsView.openMenu (false);
        audioPanel.setBounds (loopsBezel.reduced (5));
        audioPanel.open();
        audioPanel.grabKeyboardFocus();
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
        root->setAttribute ("loopFit", engine.loopFit.load());
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
            e->setAttribute ("curve", engine.curveMode[(size_t) b].load());
            e->setAttribute ("stype", engine.smartType[(size_t) b].load());
            e->setAttribute ("nofollow", engine.followBlock[(size_t) b].load() == 0 ? 1 : 0);
            e->setAttribute ("fpoly", engine.followPolyBlock[(size_t) b].load());
            e->setAttribute ("rep", engine.repeatMode[(size_t) b].load());
            e->setAttribute ("as", engine.asMode[(size_t) b].load());
            e->setAttribute ("aspct", engine.asPct[(size_t) b].load());
            e->setAttribute ("off", engine.blockOff[(size_t) b].load());
            e->setAttribute ("special", engine.specialBlock.load() == b ? 1 : engine.specialBlock2.load() == b ? 2 : 0);
            e->setAttribute ("mf", engine.mfBlock.load() == b ? 1 : 0);
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
                l->setAttribute ("anchor", snap.anchor);
                {
                    StringArray lb;
                    for (auto v : snap.layerBeats) lb.add (String (v, 4));
                    l->setAttribute ("layerBeats", lb.joinIntoString (" "));
                    StringArray lm;
                    for (auto v : snap.layerMode) lm.add (String (v));
                    l->setAttribute ("layerMode", lm.joinIntoString (" "));
                }
                l->setAttribute ("mute", (int) snap.mute);
                for (auto& layer : snap.layers)
                    l->createNewChildElement ("Layer")->addTextElement (loopToText (layer));
            }
        }

        for (int b = 0; b < flow::numBlocks; ++b)   // zones first: ζώνες block χωρίς plugin
            if (isEmptyZone (b) && engine.zoneState[(size_t) b].load() == 0)
            {
                auto* ze = root->createNewChildElement ("Zone");
                ze->setAttribute ("slot", b);
                ze->setAttribute ("lo", engine.lo[(size_t) b].load());
                ze->setAttribute ("hi", engine.hi[(size_t) b].load());
                ze->setAttribute ("seq", engine.zoneSeq[(size_t) b].load());
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
        paletteOn = false;
        panelMode = panelInstrument;
        panelIndex = 0;
    }

    int setMoved = 0, setDropped = 0, setNewer = 0;   // αποτέλεσμα του τελευταίου applySet

    void applySet (const XmlElement& root, const StringArray& crashed)
    {
        setLoadT0 = Time::getMillisecondCounter();
        lastLoadDoneMs = setLoadT0;
        clearEverything();
        flow::flowLog ("set: cleared in " + String ((int) (Time::getMillisecondCounter() - setLoadT0)) + " ms");
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
        engine.loopFit = jlimit (0, 1, root.getIntAttribute ("loopFit", 0));
        engine.tapMs = jlimit (300, 1500, root.getIntAttribute ("tapMs", 500));
        engine.autoLoop = jlimit (0, 1, root.getIntAttribute ("autoLoop", 0));   // 2 (On Rec, παλιό) → 1
        // το tempo του set ισχύει: δεν το αλλάζει η αυτόματη ανίχνευση και το Link παίρνει το tempo του set
        {
            const bool manual = root.getIntAttribute ("tempoManual", 1) != 0 || std::abs (engine.tempo.load() - 110.0) > 0.01;
            if (manual) engine.setTempo (engine.tempo.load());
            else engine.tempoUserSet = false;
        }
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

        engine.specialBlock = -1; engine.specialBlock2 = -1;
        engine.mfBlock = -1;
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

            engine.holdMode[(size_t) slot]  = jlimit (0, 2, e->getIntAttribute ("hold", 0));
            engine.curveMode[(size_t) slot] = jlimit (0, flow::numCurves - 1, e->getIntAttribute ("curve", 0));
            engine.smartType[(size_t) slot] = jlimit (0, 8, e->getIntAttribute ("stype", 0));
            // παλιό attribute "follow" αγνοείται: όλα ακολουθούν, εκτός από όσα το έχουν αποκλείσει ή είναι Drums
            engine.followBlock[(size_t) slot] = e->hasAttribute ("nofollow") ? (e->getIntAttribute ("nofollow", 0) != 0 ? 0 : 1)
                                                                              : (engine.smartType[(size_t) slot].load() == 6 ? 0 : 1);
            engine.followPolyBlock[(size_t) slot] = engine.followBlock[(size_t) slot].load() != 0 ? jlimit (0, 1, e->getIntAttribute ("fpoly", 0)) : 0;
            engine.repeatMode[(size_t) slot] = jlimit (0, 2, e->getIntAttribute ("rep", 0));
            engine.smartResetApp (slot);
            engine.blockOff[(size_t) slot]   = jlimit (0, 1, e->getIntAttribute ("off", 0));
            { const int spv = e->getIntAttribute ("special", 0); if (spv == 1) engine.specialBlock = slot; else if (spv >= 2) engine.specialBlock2 = slot; }
            if (e->getIntAttribute ("mf", 0) != 0) engine.mfBlock = slot;
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
            engine.asPct[(size_t) slot] = jlimit (0, 100, e->getIntAttribute ("aspct", 60));
            engine.asLo[(size_t) slot] = engine.lo[(size_t) slot].load(); engine.asHi[(size_t) slot] = engine.hi[(size_t) slot].load();
            engine.asMode[(size_t) slot] = jlimit (0, 2, e->getIntAttribute ("as", 0));

            auto copy = std::make_shared<XmlElement> (*e);
            auto crashedCopy = crashed;
            loadPlugin (slot, inst.desc, inst.state, false, ! hasZone,
                        [this, slot, copy, crashedCopy] { loadExtras (slot, *copy, crashedCopy); });
        }

        for (auto* ze : root.getChildWithTagNameIterator ("Zone"))   // zones first
        {
            const int sl = ze->getIntAttribute ("slot", -1);
            if (sl < 0 || sl >= flow::numBlocks || isActive (sl) || loadingSlots.contains (sl)) continue;
            engine.setZone (sl, ze->getIntAttribute ("lo", flow::lowestNote), ze->getIntAttribute ("hi", flow::lowestNote), ze->getIntAttribute ("seq", 0));
            engine.emptyZone[(size_t) sl] = 1;
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
        if (pendingLoads == 0) setLoadT0 = 0;   // set χωρίς plugins: δεν υπάρχει φόρτωση να μετρηθεί
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
                saveAfterLoad();
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
            snap.anchor = l->getDoubleAttribute ("anchor", 0.0);
            snap.mute = (unsigned) l->getIntAttribute ("mute", 0);
            for (auto* layer : l->getChildWithTagNameIterator ("Layer"))
                snap.layers.push_back (loopFromText (layer->getAllSubText()));
            {
                StringArray lb;
                lb.addTokens (l->getStringAttribute ("layerBeats"), " ", "");
                lb.removeEmptyStrings();
                for (auto& s : lb) snap.layerBeats.push_back (s.getDoubleValue());
                StringArray lm;
                lm.addTokens (l->getStringAttribute ("layerMode"), " ", "");
                lm.removeEmptyStrings();
                for (auto& s : lm) snap.layerMode.push_back (s.getIntValue());
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
            loadFxSequence (target, items, i + 1);   // πρώτα η επόμενη φόρτωση, ώστε να αποθηκεύεται μόνο μετά την τελευταία
            saveAfterLoad();
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

    //==========================================================================
    // Sound Palette: κάμερα = κράτα τον ήχο του επιλεγμένου block (instrument + MIDI FX + ρυθμίσεις του block, χωρίς audio FX)
    File soundsDir() const
    {
        return File::getSpecialLocation (File::userMusicDirectory).getChildFile ("Flow").getChildFile ("Sounds");
    }

    int countSounds() const
    {
        return soundsDir().findChildFiles (File::findFiles, false, "*.flowsound").size();
    }

    // Palette: τα blocks 1-7 παίρνουν ρόλους με τη σειρά BASS, ARP, OTHER, PADS/STRINGS, KEYS, LEAD, FX.
    // Πρώτο πάτημα: γεμίζει και τα 7. Μετά: ένα πάτημα = νέος ήχος στο επιλεγμένο block, διπλό πάτημα = νέοι ήχοι σε όσα δεν έχουν κρατηθεί.
    void onPaletteDice()
    {
        if (roleCard.isVisible() || settingsScreen.isVisible() || audioPanel.isVisible()) return;
        soundCount = countSounds();
        paletteTile.setDiceOn (soundCount > 0);
        if (soundCount == 0) { setMessage (gr ("No sounds kept yet: use the camera on a block you like")); return; }
        if (pendingLoads > 0) { setMessage (gr ("Wait until the plugins have loaded")); return; }

        if (! paletteOn) { paletteRun (0); return; }

        if (paletteTimer.isTimerRunning())    // δεύτερο πάτημα μέσα στα 300 ms
        {
            paletteTimer.stopTimer();
            paletteRun (1);
            return;
        }
        paletteTimer.f = [this] { paletteTimer.stopTimer(); paletteRun (2); };
        paletteTimer.startTimer (300);
    }

    // mode 0: γέμισμα και των 7, 1: νέοι ήχοι σε όσα δεν είναι κρατημένα, 2: νέος ήχος στο επιλεγμένο block
    void paletteRun (int mode)
    {
        if (pendingLoads > 0) { setMessage (gr ("Wait until the plugins have loaded")); return; }

        std::vector<int> targets;
        if (mode == 2)
        {
            if (selected < 0 || selected >= RoleCard::numRoles) { setMessage (gr ("Select one of the blocks 1-7")); return; }
            targets.push_back (selected);
        }
        else
            for (int b = 0; b < RoleCard::numRoles; ++b)
                if (mode == 0 || ! paletteKept[(size_t) b]) targets.push_back (b);
        if (targets.empty()) { setMessage (gr ("All 7 sounds are kept")); return; }

        // τα αρχεία ανά ρόλο
        std::array<std::vector<File>, RoleCard::numRoles> byRole;
        for (auto& f : soundsDir().findChildFiles (File::findFiles, false, "*.flowsound"))
            if (auto x = XmlDocument::parse (f))
                if (x->hasTagName ("FlowSound"))
                    for (int r = 0; r < RoleCard::numRoles; ++r)
                        if (x->getStringAttribute ("role") == RoleCard::name (r)) byRole[(size_t) r].push_back (f);

        struct Pick { int block; File file; };
        std::vector<Pick> picks;
        StringArray missing;
        for (int b : targets)
        {
            std::vector<File> cand;
            for (auto& f : byRole[(size_t) b])
                if (f.getFullPathName() != paletteFile[(size_t) b]) cand.push_back (f);
            if (cand.empty())
            {
                if (byRole[(size_t) b].empty() || mode == 2) missing.add (RoleCard::name (b));
                continue;
            }
            picks.push_back ({ b, cand[(size_t) Random::getSystemRandom().nextInt ((int) cand.size())] });
        }

        if (picks.empty())
        {
            setMessage (gr ("No other sound for: ") + missing.joinIntoString (", "));
            return;
        }

        if (mode == 0) backupBeforePalette();
        paletteOn = true;
        paletteTimer.stopTimer();

        int started = 0;
        for (auto& p : picks)
            if (paletteLoad (p.block, p.file)) ++started;

        String msg = gr ("Palette: ") + String (started) + gr (started == 1 ? " sound" : " sounds");
        if (missing.size() > 0) msg << String::fromUTF8 (" \xc2\xb7 ") << gr ("none for ") << missing.joinIntoString (", ");
        setMessage (msg);
    }

    // η Palette αντικαθιστά τα blocks 1-7: πρώτα κρατάμε αντίγραφο του set (τα 5 τελευταία)
    void backupBeforePalette()
    {
        bool any = false;
        for (int b = 0; b < flow::numBlocks; ++b) if (isActive (b)) any = true;
        if (! any) return;
        auto dir = setsDir();
        if (! dir.createDirectory().wasOk()) return;
        auto f = dir.getChildFile ("Before Palette " + Time::getCurrentTime().formatted ("%Y-%m-%d %H%M%S")).withFileExtension ("flowset");
        auto xml = createSetXml (true, true);
        if (! xml->writeTo (f)) return;
        flow::flowLog ("palette backup: " + f.getFullPathName());
        auto olds = dir.findChildFiles (File::findFiles, false, "Before Palette *.flowset");
        std::sort (olds.begin(), olds.end(), [] (const File& a, const File& b) { return a.getFileName() > b.getFileName(); });
        for (int i = 5; i < olds.size(); ++i) olds.getReference (i).deleteFile();
    }

    // φορτώνει ένα snapshot στο block (instrument, MIDI FX, ρυθμίσεις του block). Το loop του block διατηρείται.
    bool paletteLoad (int b, const File& file)
    {
        auto xml = XmlDocument::parse (file);
        if (xml == nullptr || ! xml->hasTagName ("FlowSound")) return false;
        auto inst = readPluginEntry (xml->getChildByName ("Instrument"));
        if (! inst.ok) return false;
        flow::flowLog ("palette load: " + inst.desc.name + " state " + String ((int) inst.state.getSize()) + " bytes from " + file.getFileName());

        const bool was = isActive (b);
        flow::LoopSnapshot snap;
        const bool hadLoop = was && engine.getLoop (b, snap);
        if (! was)    // προεπιλογή (δική μου υπόθεση): μία οκτάβα ανά block, από C1
        {
            const int lo = flow::firstPlayNote + 12 * b;
            engine.setZone (b, lo, b == RoleCard::numRoles - 1 ? flow::highestNote : lo + 11);
        }
        paletteFile[(size_t) b] = file.getFullPathName();
        paletteKept[(size_t) b] = false;

        auto copy = std::make_shared<XmlElement> (*xml);
        loadPlugin (b, inst.desc, inst.state, false, false,
                    [this, b, copy, snap, hadLoop] { paletteApply (b, *copy, snap, hadLoop); });
        return true;
    }

    void paletteApply (int b, const XmlElement& e, const flow::LoopSnapshot& snap, bool hadLoop)
    {
        if (! isActive (b)) return;
        engine.smartType[(size_t) b]  = jlimit (0, 8, e.getIntAttribute ("stype", 0));
        engine.holdMode[(size_t) b]   = jlimit (0, 2, e.getIntAttribute ("hold", 0));
        engine.curveMode[(size_t) b]  = jlimit (0, flow::numCurves - 1, e.getIntAttribute ("curve", 0));
        engine.volPos[(size_t) b]     = (float) jlimit (0.0, 1.0, e.getDoubleAttribute ("vol", 1.0));
        engine.volMap[(size_t) b]     = e.getIntAttribute ("vmap", 0);
        {
            const int t = jlimit (-48, 48, e.getIntAttribute ("tr", 0));
            engine.transpose[(size_t) b] = ((t + (t >= 0 ? 6 : -6)) / 12) * 12;
        }
        engine.blockOff[(size_t) b] = 0;

        clearKnobsOf (b);
        if (auto* ke = e.getChildByName ("Orb")) readKnobs (*ke, blockKnobs[(size_t) b]);
        ensureKnobs (b);

        if (hadLoop) engine.setLoop (b, snap);

        auto mfx = readPluginEntry (e.getChildByName ("MidiFx"));
        if (mfx.ok)
        {
            loadAux (mfx.desc, mfx.state, "b" + String (b) + "m", [this, b] (PluginPtr p)
            {
                if (! isActive (b)) return;
                closeEditor();
                auto old = engine.setMidiFx (b, std::move (p));
                if (old != nullptr) stateCache.erase (old.get());
                old.reset();
                updateEditor();
                refreshAll();
                saveAfterLoad();
            });
        }
        else if (engine.getMidiFx (b) != nullptr)    // ο νέος ήχος δεν έχει MIDI FX
        {
            closeEditor();
            auto old = engine.setMidiFx (b, nullptr);
            if (old != nullptr) stateCache.erase (old.get());
            old.reset();
        }
    }

    void onPaletteCamera()
    {
        if (roleCard.isVisible() || settingsScreen.isVisible() || audioPanel.isVisible()) return;
        if (! isActive (selected) || engine.getPlugin (selected) == nullptr) { setMessage (gr ("No sound to keep")); return; }
        if (pendingLoads > 0) { setMessage (gr ("Wait until the plugins have loaded")); return; }
        const int type = engine.smartType[(size_t) selected].load();
        if (type == 6) { setMessage (gr ("Drums are not kept in the Palette")); return; }

        static const char* const typeNames[] { "Smart", "Bass", "Keys", "Pad", "Lead", "Strings", "Drums", "FX", "Arp" };
        const String dot = String::fromUTF8 (" \xc2\xb7 ");
        snapshotBlock = selected;
        roleCard.open (gr ("What is this sound?"),
                       engine.getPlugin (selected)->getName() + dot + gr ("block ") + String (selected + 1) + dot + gr ("Smart master: ") + typeNames[jlimit (0, 8, type)],
                       RoleCard::roleFromSmartType (type));
    }

    bool saveSnapshot (int b, int role)
    {
        auto* inst = isActive (b) ? engine.getPlugin (b) : nullptr;
        if (inst == nullptr) { setMessage (gr ("No sound to keep")); return false; }

        auto xml = std::make_unique<XmlElement> ("FlowSound");
        xml->setAttribute ("version", 1);
        xml->setAttribute ("role", String (RoleCard::name (role)));
        xml->setAttribute ("name", inst->getName());
        xml->setAttribute ("created", Time::getCurrentTime().toISO8601 (true));
        xml->setAttribute ("stype", engine.smartType[(size_t) b].load());
        xml->setAttribute ("hold", engine.holdMode[(size_t) b].load());
        xml->setAttribute ("curve", engine.curveMode[(size_t) b].load());
        xml->setAttribute ("vol", (double) engine.volPos[(size_t) b].load());
        xml->setAttribute ("vmap", engine.volMap[(size_t) b].load());
        xml->setAttribute ("tr", engine.transpose[(size_t) b].load());
        writeKnobs (*xml->createNewChildElement ("Orb"), blockKnobs[(size_t) b]);
        pluginElement (*xml, "Instrument", inst, true);
        {
            MemoryBlock probe; inst->getStateInformation (probe);
            flow::flowLog ("snapshot: " + inst->getName() + " state " + String ((int) probe.getSize()) + " bytes (full plugin state at this moment, not a preset reference)");
        }
        if (auto* m = engine.getMidiFx (b)) pluginElement (*xml, "MidiFx", m, true);
        // τα audio FX δεν μπαίνουν στο snapshot: ο ήχος κρατά τα FX που έχει ήδη το block

        auto dir = soundsDir();
        if (! dir.createDirectory().wasOk()) { setMessage (gr ("Could not write the file")); return false; }
        const String base = File::createLegalFileName (inst->getName()) + " " + Time::getCurrentTime().formatted ("%Y-%m-%d %H%M%S");
        auto f = dir.getChildFile (base).withFileExtension ("flowsound");
        for (int i = 2; f.exists() && i < 100; ++i)
            f = dir.getChildFile (base + " " + String (i)).withFileExtension ("flowsound");

        if (xml->writeTo (f))
        {
            paletteTile.flashCamera();
            setMessage (gr ("Sound kept: ") + inst->getName() + String::fromUTF8 (" \xc2\xb7 ") + RoleCard::name (role));
            flow::flowLog ("snapshot saved: " + f.getFullPathName() + " (" + RoleCard::name (role) + ")");
            soundCount = countSounds();
            paletteTile.setDiceOn (soundCount > 0);
            if (b < RoleCard::numRoles) { paletteKept[(size_t) b] = true; paletteFile[(size_t) b] = f.getFullPathName(); }   // κρατημένο: δεν αλλάζει με το ζάρι
            return true;
        }
        setMessage (gr ("Could not write the file"));
        return false;
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
        const auto saveT0 = Time::getMillisecondCounter();
        auto* settings = properties.getUserSettings();

        std::unique_ptr<XmlElement> audioState (deviceManager.createStateXml());
        settings->setValue ("audioDeviceState", audioState.get());
        settings->setValue ("tapeFolder", engine.recorder.getFolder().getFullPathName());
        settings->setValue ("metroLevel", (double) engine.metroLevel.load());
        settings->setValue ("favPlugins", favPlugins.joinIntoString ("\n"));
        settings->setValue ("followRoot", engine.followRoot.load());
        settings->setValue ("followType", engine.followType.load());
        settings->setValue ("followAuto", engine.followAuto.load());
        settings->setValue ("smartOn", engine.smartOn.load());
        settings->setValue ("smartAccept", (double) engine.smartAccept.load());
        settings->setValue ("metroChannel", engine.metroChannel.load());
        settings->setValue ("metroSample", metroSamplePath);
        settings->setValue ("link", engine.isLinkEnabled());
        settings->setValue ("keyLights", lightsWanted);
        settings->setValue ("windowSize", windowSizeMode);

        for (int i = 0; i < flow::numControls; ++i)
            settings->setValue ("ctl" + String (i), engine.mapCode[(size_t) i].load());
        for (int k = 0; k < 8; ++k)
            settings->setValue ("knobcc" + String (k), engine.knobCc[(size_t) k].load());
        settings->setValue ("auOn", auOn);
        settings->setValue ("pluginOff", pluginOff.joinIntoString ("\n"));
        for (int i = 0; i < flow::Engine::numPedals; ++i)
            settings->setValue ("pedalcc" + String (i), engine.pedalCc[(size_t) i].load());
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
        const auto saveMs = (int) (lastSaveMs - saveT0);
        if (saveMs >= 25)   // μέτρηση: πόσο κοστίζει μία αποθήκευση και πόσο μεγάλο είναι το αρχείο ρυθμίσεων
            flow::flowLog ("saveSettings: " + String (saveMs) + " ms (states " + String (withStates ? 1 : 0) + ", file " + String ((int) (settings->getFile().getSize() / 1024)) + " KB)");
    }

    void restoreSession()
    {
        auto* settings = properties.getUserSettings();

        StringArray crashed;
        crashed.addTokens (loadingFile().loadFileAsString() + "," + settings->getValue ("loadingTokens"), ",", "");   // το δεύτερο: παλιές εκδόσεις
        crashed.removeEmptyStrings();
        crashed.removeDuplicates (false);
        loadingTokens.clear();
        loadingFile().replaceWithText ("");
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
        static const char* const names[flow::numControls] = { "Hold", "Rec MIDI loop", "Delete loop", "Stop loop (block)", "Play / stop all loops", "Metronome", "Panic", "Previous scene", "Next scene", "Mute", "Special block on/off" };
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
        messageTicks = 450;   // 15 δευτερόλεπτα για να προλάβεις να φτάσεις στο πετάλι (πριν ήταν 2)
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
        const Colour tileWhite (0xfff3f4f6), dimGrey = hobby::kIconGrey.withAlpha (0.4f);
        const float blink = (std::fmod (t, 0.8) < 0.4) ? 1.0f : 0.0f;
        blockTile.setIconColour (valid ? tileWhite : dimGrey);   // άσπρο = λειτουργεί, απαλό γκρι = σβηστό
        vstTile.setIconColour (valid ? tileWhite : dimGrey);
        paletteTile.setCameraOn (valid && engine.getPlugin (sel) != nullptr && engine.smartType[(size_t) sel].load() != 6);
        mfxTile.setIconColour (hasMfx ? tileWhite : dimGrey);
        mfxTile.setDots (hasMfx ? 1 : 0);
        fxTile.setIconColour (nfx > 0 ? tileWhite : dimGrey);
        fxTile.setDots (jmin (nfx, 3));

        const int hold = valid ? engine.holdMode[(size_t) sel].load() : 0;
        chordTile.setDots (0);
        chordTile.setIconColour (hold == 3 ? hobby::mix (hobby::kIconGrey, Colour (0xffe8883a), hobby::pulse (t * 1.5))
                                           : hold == 4 ? Colour (0xffe8883a) : dimGrey);
        chordTile.setTooltip (gr ("AL (selected block): press = Capture (play a chord, then a free key: the key becomes a key switch, orange, and you can make several assignments) / press again = AL plays / again = Off. Now: ")
                              + (hold == 3 ? "Capture" : hold == 4 ? "AL" : "Off"));

        holdTile.setDots (hold);
        holdTile.setIconColour (hold == 0 ? dimGrey : tileWhite);
        holdTile.setTooltip (gr ("Hold (selected block): press = Off / LM Mono Latch (one note) / LP Poly Latch (chord). Now: ")
                             + (hold == 0 ? "Off" : hold == 1 ? "LM" : hold == 2 ? "LP" : hold == 3 ? "AL Capture" : "AL"));

        const int loop = valid ? engine.loopStateOut[(size_t) sel].load() : (int) flow::loopOff;
        const int mute = valid ? engine.loopMuteOut[(size_t) sel].load() : 0;
        const int taps = engine.tapsMuteOut.load();

        const int recBlock = valid ? sel + 1 : 0;
        Colour recColour = dimGrey;
        bool recFilled = false;
        if (loop == flow::loopRec)       { recColour = hobby::mix (dimGrey, hobby::kRed, 0.5f + 0.5f * hobby::pulse (t * 1.5)); recFilled = true; }
        else if (loop == flow::loopPlay) { recColour = tileWhite; recFilled = true; }
        else if (loop == flow::loopOver) { recColour = hobby::kPeach; recFilled = true; }
        recTile.setNumber (recBlock, recFilled);
        recTile.setIconColour (recColour);

        undoTile.setIconColour (engine.tapsDelOut.load() > 0 ? hobby::kRed : loop != flow::loopOff ? tileWhite : dimGrey);

        {
            const bool selStopped = valid && engine.loopStoppedOut[(size_t) selected].load() != 0;
            stopTile.setSymbol (selStopped ? Symbol::Play : Symbol::Stop);
            stopTile.setIconColour (selStopped ? hobby::mix (hobby::kPeach, tileWhite, blink) : loop != flow::loopOff ? tileWhite : dimGrey);
        }
        juce::ignoreUnused (mute, taps);

        tapeTile.setSymbol (engine.loopsStopped.load() != 0 ? Symbol::Stop : Symbol::Play);
        tapeTile.setIconColour (engine.loopsStopped.load() != 0 ? hobby::mix (hobby::kPeach, tileWhite, blink) : loop != flow::loopOff ? hobby::kBlue : dimGrey);

        {
            // ο μετρονόμος αναβοσβήνει σε κάθε beat όταν ο ήχος του είναι ανοιχτός
            const double ph = engine.beatPosOut.load();
            const float flash = jlimit (0.0f, 1.0f, 1.0f - (float) (ph - std::floor (ph)) * 4.0f);
            refreshSmartTile();
            metroTile.setIconColour (engine.metroOn.load() ? hobby::mix (dimGrey, tileWhite, 0.3f + 0.7f * flash) : dimGrey);
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
            const int jumps = engine.statJumps.exchange (0), clip = engine.statClip.exchange (0), nanN = engine.statNan.exchange (0);
            const float jumpMax = engine.statJumpMax.exchange (0.0f), peak = engine.statPeak.exchange (0.0f);
            const bool notable = full > 0 || xrDelta > 0 || maxLock > 0.002f || maxCb > 0.6f * budget || jumps > 0 || clip > 0 || nanN > 0;
            if (jumps > 0 || clip > 0 || nanN > 0)
                flow::flowLog (String::formatted ("GLITCH? output jumps(>0.3): %d, biggest jump %.2f, samples over 1.0: %d, NaN: %d, peak %.2f", jumps, jumpMax, clip, nanN, peak));
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

        loopsView.repaintDynamic();
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
                if (lp >= 0 && lp != selected && isActive (lp))
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

        const int plearned = engine.pedalLearnedCount.load();
        if (plearned != lastPedalLearned)
        {
            lastPedalLearned = plearned;
            setMessage ("Pedal " + String (engine.pedalLearnedSlot.load() + 1) + " " + String::fromUTF8 ("\xe2\x86\x90") + " CC " + String (engine.pedalLearnedNum.load()));
            saveNow();
        }

        const int vlearned = engine.volLearnedCount.load();
        if (vlearned != lastVolLearned)
        {
            lastVolLearned = vlearned;
            setMessage ("Block " + String (engine.volLearnedBlock.load() + 1) + " " + String::fromUTF8 ("\xe2\x86\x90") + " "
                        + pedalCodeText (engine.volLearnedCode.load()));
        }

        // αλλαγές από πετάλια (hold, ένταση, αναθέσεις, ζώνες, tempo) και από το ποντίκι σώζονται άμεσα (με όριο ρυθμού)
        if (engine.volMoveDirty.load() != 0 && Time::getMillisecondCounter() - engine.lastVolMoveMs.load() > 1500)
        {
            engine.volMoveDirty = 0;
            ++engine.changeCount;   // το πετάλι σταμάτησε: σώζεται μία φορά
        }
        const int cc = engine.changeCount.load();
        if (cc != lastChange) { lastChange = cc; dirty = true; }
        if (dirty && Time::getMillisecondCounter() - lastSaveMs > 5000)   // κάθε αποθήκευση γράφει ~15 MB (~160 ms στο UI): όχι πιο συχνά από ανά 5 s
            saveSettings (false);

        drainMidiDiag();
        updateS88Screens();

        if (engine.smartOn.load() != 0 && ++smartDiagTicks >= 300)   // κάθε ~10 s: στάθμες ανά block και ζώνη (dBFS) και διόρθωση, για ρύθμιση του Smart με δεδομένα
        {
            smartDiagTicks = 0;
            static const char* tn[] = { "Smart", "Bass", "Keys", "Pad", "Lead", "Strings", "Drums", "FX", "Arp" };
            String line = String ("smart ") + "context" + ":";
            float total[6] = {};
            for (int b = 0; b < flow::numBlocks; ++b)
            {
                if (engine.active[(size_t) b].load() == 0) continue;
                String lv, ap;
                for (int k = 0; k < 6; ++k)
                {
                    const float lvl = engine.smartBand[(size_t) b][(size_t) k].load();
                    total[k] += flow::smartCtxEnergy (lvl);
                    lv << String (lvl * 60.0f - 60.0f, 0) << (k < 5 ? " " : "");
                    ap << String (engine.smartApp[(size_t) b][(size_t) k].load(), 1) << (k < 5 ? " " : "");
                }
                line << " | b" << (b + 1) << " " << tn[jlimit (0, 8, engine.smartType[(size_t) b].load())];
                if (engine.smartType[(size_t) b].load() == 0)
                    line << "->" << tn[jlimit (0, 8, engine.smartAutoT[(size_t) b].load())] << " (low " << String (engine.smartFeat[(size_t) b][0].load(), 2) << " mid " << String (engine.smartFeat[(size_t) b][1].load(), 2)
                         << " high " << String (engine.smartFeat[(size_t) b][2].load(), 2) << " dbStd " << String (engine.smartFeat[(size_t) b][3].load(), 1) << ")";
                line << " dB[" << lv << "] cut[" << ap << "] w" << String (engine.smartWidth[(size_t) b].load(), 2);
            }
            float all = 0.0f; for (int k = 0; k < 6; ++k) all += total[k];
            if (all > 1.0e-12f) line << " | share% [" << String (100.0f * total[0] / all, 0) << " " << String (100.0f * total[1] / all, 0) << " " << String (100.0f * total[2] / all, 0) << " " << String (100.0f * total[3] / all, 0) << " " << String (100.0f * total[4] / all, 0) << " " << String (100.0f * total[5] / all, 0) << "]";
            flow::flowLog (line);
        }

        if (++stateTicks >= 3600)   // ~2 λεπτά: η αποθήκευση με states (32 MB) κοστίζει ~0.5 s στο UI
        {
            stateTicks = 0;
            saveSettings (true);
        }

        retryLightsIfNeeded();
        refreshAll();
    }

    //==========================================================================
    flow::Engine engine;
    ApplicationProperties properties;
    AudioDeviceManager deviceManager;
    AudioPluginFormatManager formatManager;
    TooltipWindow tooltipWindow { nullptr, 600 };
    bool helpOn = true;

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
    int lastLearned = 0, lastVolLearned = 0, lastPedalLearned = 0, lastChange = 0;
    int messageTicks = 0, stateTicks = 0, smartDiagTicks = 0;
    bool dirty = false;
    uint32 lastSaveMs = 0;
    Array<int> loadingSlots;
    std::array<String, flow::numBlocks> loadingName;   // όνομα του plugin που φορτώνει σε κάθε θέση (για το snapshot στο S88)
    int pendingLoads = 0;
    StringArray loadingTokens;
    std::map<String, uint32> loadStartMs;
    uint32 lastLoadDoneMs = 0, setLoadT0 = 0;
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
    } dockTimer, orbTimer;
    std::unique_ptr<FileChooser> chooser;

    LoopsView loopsView;
    KeyboardView keyboardView;
    PanelHeader header;
    AudioMidiPanel audioPanel { deviceManager, typeface };
    SettingsScreen settingsScreen { typeface };
    RoleCard roleCard { typeface };
    PaletteTile paletteTile;
    int snapshotBlock = -1;
    int soundCount = 0;
    bool paletteOn = false;                                  // η Palette έχει γεμίσει τα blocks (μόνο για τη συνεδρία)
    std::array<bool, flow::numBlocks> paletteKept {};        // κρατημένα blocks (αόρατο, μόνο για τη συνεδρία)
    std::array<String, flow::numBlocks> paletteFile {};
    struct PaletteTimer : public Timer
    {
        std::function<void()> f;
        void timerCallback() override { if (f) f(); }
    } paletteTimer;
    bool audioFromSettings = false;
    ValueTile blockTile, vstTile, mfxTile, fxTile, holdTile, chordTile, recTile, undoTile, stopTile, tapeTile, metroTile, folderTile, smartTile, motionTile;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

//==============================================================================
class FlowApplication : public JUCEApplication
{
public:
    const String getApplicationName() override    { return "Flow"; }
    const String getApplicationVersion() override { return "0.90.1"; }
    bool moreThanOneInstanceAllowed() override    { return false; }

    void initialise (const String&) override
    {
        flow::flowLog ("Flow " + getApplicationVersion() + " started");
        mainWindow = std::make_unique<MainWindow> (getApplicationName() + " " + getApplicationVersion());
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
            setResizeLimits (800, 640, 4000, 3000);
            if (auto* mc = dynamic_cast<MainComponent*> (getContentComponent()))
            {
                // το επιλεγμένο μέγεθος (Settings > Window size), προεπιλογή 1097 x 810
                mc->applyWindowSize (true);
                if (mc->getWindowSizeMode() == 3)
                {
                    int w = 1097, h = 810;
                    if (auto* d = Desktop::getInstance().getDisplays().getPrimaryDisplay())
                    {
                        w = jmin (w, d->userArea.getWidth() - 20);
                        h = jmin (h, d->userArea.getHeight() - 20);
                    }
                    centreWithSize (jmax (880, w), jmax (690, h));
                }
            }
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
