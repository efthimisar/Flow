// Simple Host - ένας πολύ απλός host για VST3 / AU plugins.
// Prisma Audio

#include <juce_audio_utils/juce_audio_utils.h>

using namespace juce;

static String gr (const char* utf8Text) { return String::fromUTF8 (utf8Text); }

//==============================================================================
// Παράθυρο που δείχνει το γραφικό περιβάλλον του plugin.
class PluginWindow : public DocumentWindow
{
public:
    PluginWindow (AudioProcessorEditor* editor, const String& name)
        : DocumentWindow (name, Colours::darkgrey,
                          DocumentWindow::closeButton | DocumentWindow::minimiseButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (editor, true);
        setResizable (editor->isResizable(), false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    // Το κλείσιμο απλώς κρύβει το παράθυρο. Ξανανοίγει από το κουμπί.
    void closeButtonPressed() override { setVisible (false); }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginWindow)
};

//==============================================================================
class MainComponent : public Component
{
public:
    MainComponent()
    {
        PropertiesFile::Options options;
        options.applicationName     = "SimpleHost";
        options.filenameSuffix      = ".settings";
        options.osxLibrarySubFolder = "Application Support";
        properties.setStorageParameters (options);

        formatManager.addDefaultFormats();

        addAndMakeVisible (loadButton);
        addAndMakeVisible (showButton);
        addAndMakeVisible (audioButton);
        addAndMakeVisible (statusLabel);

        loadButton.setButtonText (gr ("Φόρτωση plugin…"));
        showButton.setButtonText (gr ("Εμφάνιση παραθύρου plugin"));
        audioButton.setButtonText (gr ("Ρυθμίσεις ήχου και MIDI…"));
        statusLabel.setJustificationType (Justification::centred);
        statusLabel.setText (gr ("Δεν έχει φορτωθεί plugin."), dontSendNotification);
        showButton.setEnabled (false);

        loadButton.onClick  = [this] { chooseAndLoadPlugin(); };
        showButton.onClick  = [this] { showPluginWindow(); };
        audioButton.onClick = [this] { openAudioSettings(); };

        auto* settings = properties.getUserSettings();

        std::unique_ptr<XmlElement> savedAudio (settings->getXmlValue ("audioDeviceState"));
        deviceManager.initialise (2, 2, savedAudio.get(), true);

        for (auto& device : MidiInput::getAvailableDevices())
            deviceManager.setMidiInputDeviceEnabled (device.identifier, true);

        deviceManager.addAudioCallback (&player);
        deviceManager.addMidiInputDeviceCallback (String(), &player);

        setSize (420, 220);

        restoreLastPlugin();
    }

    ~MainComponent() override
    {
        saveSettings();
        deviceManager.removeMidiInputDeviceCallback (String(), &player);
        deviceManager.removeAudioCallback (&player);
        unloadPlugin();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (20);
        loadButton.setBounds  (area.removeFromTop (36));
        area.removeFromTop (10);
        showButton.setBounds  (area.removeFromTop (36));
        area.removeFromTop (10);
        audioButton.setBounds (area.removeFromTop (36));
        area.removeFromTop (14);
        statusLabel.setBounds (area);
    }

    void paint (Graphics& g) override
    {
        g.fillAll (getLookAndFeel().findColour (ResizableWindow::backgroundColourId));
    }

private:
    //==========================================================================
    void chooseAndLoadPlugin()
    {
        chooser = std::make_unique<FileChooser> (gr ("Διάλεξε plugin (.vst3 ή .component)"),
                                                 File ("/Library/Audio/Plug-Ins"),
                                                 "*.vst3;*.component");

        auto flags = FileBrowserComponent::openMode
                   | FileBrowserComponent::canSelectFiles
                   | FileBrowserComponent::canSelectDirectories;

        chooser->launchAsync (flags, [this] (const FileChooser& fc)
        {
            auto file = fc.getResult();

            if (file != File())
                loadFromFile (file);
        });
    }

    void loadFromFile (const File& file)
    {
        OwnedArray<PluginDescription> found;
        auto path = file.getFullPathName();

        for (auto* format : formatManager.getFormats())
            if (format->fileMightContainThisPluginType (path))
                format->findAllTypesForFile (found, path);

        if (found.isEmpty())
        {
            statusLabel.setText (gr ("Δεν βρέθηκε plugin σε αυτό το αρχείο."), dontSendNotification);
            return;
        }

        loadPlugin (*found.getFirst(), MemoryBlock());
    }

    void loadPlugin (const PluginDescription& description, const MemoryBlock& state)
    {
        unloadPlugin();
        statusLabel.setText (gr ("Φόρτωση: ") + description.name + "…", dontSendNotification);

        // Σημάδι ασφαλείας: αν το plugin κρασάρει, δεν θα ξαναφορτωθεί αυτόματα.
        properties.getUserSettings()->setValue ("loadingPlugin", true);
        properties.saveIfNeeded();

        double sampleRate = 44100.0;
        int bufferSize = 512;

        if (auto* device = deviceManager.getCurrentAudioDevice())
        {
            sampleRate = device->getCurrentSampleRate();
            bufferSize = device->getCurrentBufferSizeSamples();
        }

        SafePointer<MainComponent> safeThis (this);

        formatManager.createPluginInstanceAsync (description, sampleRate, bufferSize,
            [safeThis, description, state] (std::unique_ptr<AudioPluginInstance> instance,
                                            const String& error)
            {
                if (safeThis != nullptr)
                    safeThis->pluginLoaded (std::move (instance), error, description, state);
            });
    }

    void pluginLoaded (std::unique_ptr<AudioPluginInstance> instance,
                       const String& error,
                       const PluginDescription& description,
                       const MemoryBlock& state)
    {
        auto* settings = properties.getUserSettings();
        settings->setValue ("loadingPlugin", false);

        if (instance == nullptr)
        {
            statusLabel.setText (gr ("Αποτυχία φόρτωσης: ") + error, dontSendNotification);
            properties.saveIfNeeded();
            return;
        }

        plugin = std::move (instance);
        currentDescription = description;

        if (state.getSize() > 0)
            plugin->setStateInformation (state.getData(), (int) state.getSize());

        player.setProcessor (plugin.get());

        AudioProcessorEditor* editor = plugin->hasEditor() ? plugin->createEditorIfNeeded() : nullptr;

        if (editor == nullptr)
            editor = new GenericAudioProcessorEditor (*plugin);

        pluginWindow = std::make_unique<PluginWindow> (editor, plugin->getName());

        showButton.setEnabled (true);
        statusLabel.setText (gr ("Φορτώθηκε: ") + plugin->getName(), dontSendNotification);

        settings->setValue ("pluginDescription", description.createXml().get());
        properties.saveIfNeeded();
    }

    void unloadPlugin()
    {
        // Πρώτα το παράθυρο, μετά ο επεξεργαστής, μετά το plugin.
        pluginWindow.reset();
        player.setProcessor (nullptr);
        plugin.reset();
        showButton.setEnabled (false);
    }

    void showPluginWindow()
    {
        if (pluginWindow != nullptr)
        {
            pluginWindow->setVisible (true);
            pluginWindow->toFront (true);
        }
    }

    //==========================================================================
    void openAudioSettings()
    {
        auto* selector = new AudioDeviceSelectorComponent (deviceManager,
                                                           0, 2, 0, 2,
                                                           true, false, true, false);
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

        if (plugin != nullptr)
        {
            MemoryBlock state;
            plugin->getStateInformation (state);
            settings->setValue ("pluginState", state.toBase64Encoding());
            settings->setValue ("pluginDescription", currentDescription.createXml().get());
        }

        properties.saveIfNeeded();
    }

    void restoreLastPlugin()
    {
        auto* settings = properties.getUserSettings();

        if (settings->getBoolValue ("loadingPlugin", false))
        {
            // Η προηγούμενη φόρτωση δεν ολοκληρώθηκε (πιθανό crash). Δεν ξαναδοκιμάζουμε.
            settings->setValue ("loadingPlugin", false);
            properties.saveIfNeeded();
            statusLabel.setText (gr ("Το τελευταίο plugin δεν φορτώθηκε σωστά. Διάλεξε ξανά."),
                                 dontSendNotification);
            return;
        }

        std::unique_ptr<XmlElement> xml (settings->getXmlValue ("pluginDescription"));

        if (xml == nullptr)
            return;

        PluginDescription description;

        if (! description.loadFromXml (*xml))
            return;

        MemoryBlock state;
        state.fromBase64Encoding (settings->getValue ("pluginState"));

        loadPlugin (description, state);
    }

    //==========================================================================
    ApplicationProperties properties;
    AudioDeviceManager deviceManager;
    AudioProcessorPlayer player;
    AudioPluginFormatManager formatManager;

    std::unique_ptr<AudioPluginInstance> plugin;
    PluginDescription currentDescription;
    std::unique_ptr<PluginWindow> pluginWindow;
    std::unique_ptr<FileChooser> chooser;

    TextButton loadButton, showButton, audioButton;
    Label statusLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

//==============================================================================
class SimpleHostApplication : public JUCEApplication
{
public:
    const String getApplicationName() override    { return "Simple Host"; }
    const String getApplicationVersion() override { return "0.1.0"; }
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
            : DocumentWindow (name,
                              Desktop::getInstance().getDefaultLookAndFeel()
                                  .findColour (ResizableWindow::backgroundColourId),
                              DocumentWindow::closeButton | DocumentWindow::minimiseButton)
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

START_JUCE_APPLICATION (SimpleHostApplication)
