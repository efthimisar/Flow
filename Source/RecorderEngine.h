#pragma once
// Μηχανή εγγραφής (Rec Audio). Δεν εξαρτάται από GUI/plugin.
//  - Ο audio thread μόνο αντιγράφει το σήμα σε lock-free FIFO και μετρά peaks.
//  - Ένας background thread κρατά το pre-roll, ανιχνεύει ήχο, γράφει το WAV (24-bit, στερεοφωνικό)
//    και ταυτόχρονα ένα WAV ανά block (stems). Στο τέλος όλα μπαίνουν σε ένα zip.
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>

class RecorderEngine : private juce::Thread
{
public:
    enum class State { Armed, Recording, Stopped, Error };

    static constexpr int    kStems = 16;                  // ένα stem ανά block
    static constexpr int    kChannels = 2 + 2 * kStems;   // master L/R + stems L/R
    static constexpr double kThresholdDb = -40.0;
    static constexpr double kPreRollSeconds = 0.5;
    static constexpr double kMaxTakeSeconds = 12.0 * 60.0;
    static constexpr int    kSyncSeconds = 2;             // κάθε πόσα δευτερόλεπτα ενημερώνεται η κεφαλίδα του WAV

    RecorderEngine();
    ~RecorderEngine() override;

    // μόνο από prepareToPlay (όχι audio thread)
    void prepare (double sampleRate);
    void release();

    // audio thread: δεν αλλάζει τα buffers. stems: 2*kStems κανάλια (L/R ανά block), stemMask: ποια blocks έχουν ήχο
    void process (const juce::AudioBuffer<float>& master, const juce::AudioBuffer<float>& stems, uint32_t stemMask);

    // message thread
    void setFolder (const juce::File& f);
    juce::File getFolder() const;
    void pressButton();                                  // stop ή νέο take
    State  getState() const noexcept        { return (State) state.load(); }
    bool   isTapeFull() const noexcept      { return tapeFull.load(); }
    double getElapsedSeconds() const noexcept { return (double) framesWritten.load() / juce::jmax (1.0, sampleRate.load()); }
    float  getPeakL() noexcept              { return peakL.exchange (0.0f); }
    float  getPeakR() noexcept              { return peakR.exchange (0.0f); }
    juce::File getCurrentFile() const;
    int    getDroppedFrames() const noexcept { return dropped.load(); }
    bool   isZipping() const noexcept       { return zipping.load(); }

    static juce::File makeTakeBase (const juce::File& folder, juce::Time t);

private:
    class WavWriter;

    void run() override;
    void workerStep();
    void startTake();
    void finishTake (bool full);
    void enterError();
    void resetArmed();
    void startWorker();
    void stopWorker();
    bool writeRange (const juce::AudioBuffer<float>& src, int start, int n);
    void closeWriters (bool& ok);
    void packZip();

    // FIFO audio → worker
    juce::AbstractFifo fifo { 1 };
    juce::AudioBuffer<float> fifoBuf;
    std::atomic<int> dropped { 0 };
    std::atomic<uint32_t> stemMaskNow { 0 };

    // pre-roll (μόνο worker)
    juce::AudioBuffer<float> preRoll;
    int preRollLen = 0, preRollPos = 0, preRollFilled = 0;

    std::unique_ptr<WavWriter> writer;
    std::unique_ptr<WavWriter> stemWriter[kStems];
    juce::File masterFile, stemFile[kStems], zipFile;
    uint32_t takeMask = 0;
    juce::File currentFile;
    mutable juce::CriticalSection fileLock;
    juce::File folder;

    std::atomic<double> sampleRate { 48000.0 };
    std::atomic<int> state { (int) State::Armed };
    std::atomic<bool> tapeFull { false };
    std::atomic<bool> zipping { false };
    std::atomic<int64_t> framesWritten { 0 };
    std::atomic<bool> pressRequested { false };
    std::atomic<bool> prepared { false };
    std::atomic<float> peakL { 0.0f }, peakR { 0.0f };

    int64_t maxFrames = 0, framesSinceSync = 0;
    juce::AudioBuffer<float> chunk;
    float thresholdLin = 0.01f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RecorderEngine)
};
