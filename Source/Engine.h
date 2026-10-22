#pragma once
// Flow engine: μέχρι 16 blocks (ένα ανά instrument), καθένα με δική του ζώνη, hold, ένταση, transpose, MIDI loop,
// MIDI FX και αλυσίδα FX. Στο τέλος: master FX, metronome, Rec Audio με stems.
// Όλη η μουσική λογική τρέχει μέσα στο audio callback, άρα ο χρόνος ακολουθεί το ρολόι του ήχου.

#include <juce_audio_utils/juce_audio_utils.h>
#include "RecorderEngine.h"
#include "LinkBridge.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <cmath>
#include <cstdint>
#include <vector>

namespace flow
{
    constexpr int numBlocks   = 16;
    constexpr int lowestNote  = 21;     // A0
    constexpr int highestNote = 108;    // C8
    constexpr int maxLoopBars = 16;     // μέγιστο μήκος MIDI loop
    constexpr double beatsPerBar = 4.0;

    enum LoopState { loopOff = 0, loopRec = 1, loopPlay = 2, loopOver = 3 };
    constexpr int maxLayers = 32;
    // κουμπιά που ανατίθενται σε πετάλι ή πλήκτρο (MIDI learn)
    enum Control { ctlHold = 0, ctlRec, ctlUndo, ctlStop, ctlTape, ctlMetro, ctlPanic, ctlScenePrev, ctlSceneNext, ctlMute, numControls };

    // καμπύλες έντασης: 0 διαγώνια, 1 βαθιά (αργή αρχή), 2 ανάποδη βαθιά (γρήγορη αρχή), 3 διαγώνια προς τα κάτω
    constexpr int numCurves = 6;   // 4 = S-καμπύλη, 5 = πολύ βαθιά (κυβική)
    constexpr int numChordTypes = 7;   // maj, min, 7, maj7, m7, sus4, dim
    inline float curveGain (int curve, float x)
    {
        x = juce::jlimit (0.0f, 1.0f, x);
        switch (curve)
        {
            case 1:  return x * x;
            case 2:  return 1.0f - (1.0f - x) * (1.0f - x);
            case 3:  return 1.0f - x;
            case 4:  return x * x * (3.0f - 2.0f * x);
            case 5:  return x * x * x;
            default: return x;
        }
    }

    // αντίστροφη καμπύλη: ποια θέση πετάλιού δίνει ένταση g (για σύρσιμο με το ποντίκι)
    inline float curveInverse (int curve, float g)
    {
        g = juce::jlimit (0.0f, 1.0f, g);
        switch (curve)
        {
            case 1:  return std::sqrt (g);
            case 2:  return 1.0f - std::sqrt (1.0f - g);
            case 3:  return 1.0f - g;
            case 4:  return 0.5f - std::sin (std::asin (1.0f - 2.0f * g) / 3.0f);
            case 5:  return std::cbrt (g);
            default: return g;
        }
    }

    inline bool isBlackKey (int n) { const int m = n % 12; return m == 1 || m == 3 || m == 6 || m == 8 || m == 10; }

    // γεγονός loop: phase σε beats μέσα στο loop (στην εγγραφή: απόλυτη θέση beat)
    struct LoopEvent { double phase; bool on; int note; int vel; bool isRaw = false; int d2 = 0; };   // isRaw: note = status, vel = data1, d2 = data2 (CC, pitch bend, aftertouch)

    struct LoopSnapshot
    {
        double beats = 0.0;
        std::vector<double> layerBeats;   // μήκος κάθε layer (το loop είναι το μεγαλύτερο, τα άλλα επαναλαμβάνονται)
        unsigned mute = 0;
        std::vector<std::vector<LoopEvent>> layers;
    };

    using PluginPtr = std::unique_ptr<juce::AudioPluginInstance>;

    class Engine : public juce::AudioIODeviceCallback
    {
    public:
        Engine()
        {
            for (int b = 0; b < numBlocks; ++b)
            {
                active[b] = 0; zoneState[b] = 0; zoneFirst[b] = 0; zoneSeq[b] = 0;
                lo[b] = lowestNote; hi[b] = lowestNote;
                holdMode[b] = 0; latchBits[(size_t) b * 2] = 0; latchBits[(size_t) b * 2 + 1] = 0;
                blockOff[b] = 0; chordMode[b] = 0; chordType[b] = 0; chordMinor[b] = 0; chordTonic[b] = -1; pcMaskOut[b] = 0; bassOut[b] = -1;
                arpMode[b] = 0;
                curveMode[b] = 0;
                transpose[b] = 0;
                volPos[b] = 1.0f; gainOut[b] = 1.0f;
                volMap[b] = 0;
                loopStateOut[b] = loopOff;
                loopBeatsOut[b] = 0.0;
                recStartBeatOut[b] = 0.0;
                loopLayersOut[b] = 0;
                loopMuteOut[b] = 0;
                loopTapsOut[b] = 0;
                cmdMain[b] = 0; cmdUndo[b] = 0; cmdStop[b] = 0; cmdMute[b] = 0; loopStoppedOut[b] = 0;
            }
            for (int i = 0; i < numControls; ++i) mapCode[i] = 0;
            owner.fill (-1);
            incoming.reset (44100.0);
        }

        //==================== controls and read-outs (message thread) ====================
        std::array<std::atomic<uint64_t>, numBlocks * 2> latchBits;   // πλήκτρα που κρατιούνται από το Latch (και νότες συγχορδίας), για την οθόνη
        std::array<std::atomic<int>, numBlocks> active, zoneState, zoneFirst, zoneSeq, lo, hi, holdMode, arpMode, curveMode, transpose, blockOff, chordMode, chordType, chordMinor, chordTonic, pcMaskOut, bassOut,
                                                volMap, loopStateOut, loopLayersOut, loopMuteOut, loopTapsOut,
                                                cmdMain, cmdUndo, cmdStop, cmdMute, loopStoppedOut;
        std::array<std::atomic<double>, numBlocks> loopBeatsOut, recStartBeatOut;
        std::array<std::atomic<float>, numBlocks> volPos, gainOut;
        std::array<std::atomic<int>, numControls> mapCode;   // MIDI learn πεταλιών / πλήκτρων
        std::atomic<int> mapLearnTarget { -1 }, learnedCount { 0 }, learnedTarget { -1 }, learnedCode { 0 };
        std::atomic<int> volLearn { -1 }, volLearnedCount { 0 }, volLearnedBlock { -1 }, volLearnedCode { 0 };
        std::atomic<int> sceneRequest { -1 };   // από πετάλι: -2 = προηγούμενη, -3 = επόμενη (το διαβάζει το UI)
        std::atomic<int> selectedBlock { 0 };   // το block στο οποίο δρουν Hold και looper
        std::atomic<int> changeCount { 0 };     // ανεβαίνει όταν αλλάζει κάτι που πρέπει να αποθηκευτεί
        std::atomic<double> beatPosOut { 0.0 };
        std::atomic<int> axisBarsOut { 4 };

        // tempo και quantize (global)
        std::atomic<double> tempo { 120.0 };
        std::atomic<int> startQ { 0 }, lengthQ { 0 }, snapQ { 0 };   // startQ/lengthQ: 0 Off, 1 Beat, 2 Bar. snapQ: 0 Off, 1 = 1/8, 2 = 1/16
        std::atomic<int> tapRequest { 0 };
        std::atomic<int> afterFirst { 0 };      // μετά το πρώτο loop: 0 = Play, 1 = Overdub
        std::atomic<int> midiCount { 0 };       // μετρά τα μηνύματα MIDI που έρχονται (για την ένδειξη στην κεφαλίδα)
        std::atomic<int> tapActiveOut { 0 };

        // metronome
        std::atomic<bool> metroOn { false };
        std::atomic<float> metroLevel { 0.8f };
        std::atomic<int> metroChannel { 2 };      // έξοδος 3 (0-based 2)
        std::atomic<int> numOutChannels { 2 };

        // panic και Link
        std::atomic<int> panicCount { 0 };
        std::atomic<int> linkPeersOut { 0 };

        RecorderEngine recorder;   // Rec Audio
        LinkBridge link;

        juce::MidiMessageCollector incoming;  // register this as the MIDI input callback

        // εντολές looper για ένα block (από κουμπί ή πετάλι)
        void pressMain (int b) { ++cmdMain[(size_t) b]; }
        void pressUndo (int b) { ++cmdUndo[(size_t) b]; }
        void pressStop (int b) { ++cmdStop[(size_t) b]; }
        void pressMute (int b) { ++cmdMute[(size_t) b]; }
        void tapFromUi()       { ++tapRequest; }
        void panic()           { ++panicCount; }

        void setTempo (double bpm)
        {
            bpm = juce::jlimit (40.0, 240.0, bpm);
            tempo = bpm;
            if (link.isEnabled()) link.setTempo (bpm);
            ++changeCount;
        }

        void setLinkEnabled (bool on) { link.enable (on); linkWanted = on; }
        bool isLinkEnabled() const    { return linkWanted.load(); }

        // ζώνη block: το επόμενο ζευγάρι πλήκτρων που θα πατηθούν την ορίζει
        void startZone (int b)          { zoneFirst[b] = 0; zoneState[b] = 1; }
        void setZone (int b, int l, int h, int seq = -1)
        {
            lo[b] = juce::jmin (l, h);
            hi[b] = juce::jmax (l, h);
            zoneSeq[b] = seq >= 0 ? seq : ++seqCounter;
            seqCounter = juce::jmax (seqCounter.load(), zoneSeq[b].load());
            zoneState[b] = 0;
        }
        int pendingBlock() const { return captureTarget(); }   // block που περιμένει ζώνη (-1 = κανένα)
        bool capturing() const
        {
            for (int b = 0; b < numBlocks; ++b)
                if (active[b].load() != 0 && zoneState[b].load() != 0) return true;
            return false;
        }

        // ποιο block παίζει το πλήκτρο n (-1 = κανένα). Αν επικαλύπτονται ζώνες, κερδίζει η πιο πρόσφατη.
        int blockForNote (int n) const
        {
            int best = -1, bestSeq = -1;
            for (int b = 0; b < numBlocks; ++b)
                if (active[b].load() != 0 && zoneState[b].load() == 0 && n >= lo[b].load() && n <= hi[b].load()
                    && zoneSeq[b].load() > bestSeq)
                {
                    best = b;
                    bestSeq = zoneSeq[b].load();
                }
            return best;
        }

        void startVolumeLearn (int b) { mapLearnTarget = -1; volLearn = b; }
        void cancelLearns()           { mapLearnTarget = -1; volLearn = -1; }

        double getSampleRate() const { return sampleRate.load(); }

        juce::AudioPluginInstance* getPlugin (int b) { return blocks[(size_t) b].plugin.get(); }
        juce::AudioPluginInstance* getMidiFx (int b) { return blocks[(size_t) b].midiFx.get(); }
        int numFx (int b) const { return (int) chainOf (b).size(); }
        juce::AudioPluginInstance* getFx (int b, int i)
        {
            auto& c = chainOf (b);
            return i >= 0 && i < (int) c.size() ? c[(size_t) i].get() : nullptr;
        }

        // Puts a new plugin (or nullptr) into a block and returns the old one.
        PluginPtr swapPlugin (int b, PluginPtr fresh, bool captureZone = false)
        {
            if (fresh != nullptr)
            {
                prepare (*fresh);
                fresh->setPlayHead (&head);
            }

            PluginPtr old;
            {
                const juce::ScopedLock sl (lock);
                auto& B = blocks[(size_t) b];
                old = std::move (B.plugin);
                B.plugin = std::move (fresh);
                B.resetState();
                B.panicPending = true;
                if (B.plugin != nullptr)
                    B.scratch.setSize (juce::jmax (2, juce::jmax (B.plugin->getTotalNumInputChannels(),
                                                                   B.plugin->getTotalNumOutputChannels())),
                                       juce::jmax (blockSize, 4096));
                for (auto& o : owner)
                    if (o == b) o = -1;
                active[b] = B.plugin != nullptr ? 1 : 0;
                zoneFirst[b] = 0;
                zoneState[b] = (B.plugin != nullptr && captureZone) ? 1 : 0;
                B.prevGain = -1.0f;
                B.seenMain = cmdMain[(size_t) b].load();
                B.seenUndo = cmdUndo[(size_t) b].load();
                B.seenStop = cmdStop[(size_t) b].load();
                B.seenMute = cmdMute[(size_t) b].load();
                loopStateOut[b] = loopOff;
                loopBeatsOut[b] = 0.0;
                loopLayersOut[b] = 0;
                loopMuteOut[b] = 0;
                loopTapsOut[b] = 0;
            }
            return old;
        }

        // MIDI FX του block (μπαίνει πριν από το instrument)
        PluginPtr setMidiFx (int b, PluginPtr fresh)
        {
            if (fresh != nullptr)
            {
                prepare (*fresh);
                fresh->setPlayHead (&head);
            }
            const juce::ScopedLock sl (lock);
            auto& B = blocks[(size_t) b];
            PluginPtr old = std::move (B.midiFx);
            B.midiFx = std::move (fresh);
            if (B.midiFx != nullptr)
                B.mfxScratch.setSize (juce::jmax (1, juce::jmax (B.midiFx->getTotalNumInputChannels(), B.midiFx->getTotalNumOutputChannels())),
                                      juce::jmax (blockSize, 4096));
            B.panicPending = true;
            return old;
        }

        // αλυσίδα FX: b = 0..15 για block, b = -1 για το master
        void addFx (int b, PluginPtr fresh)
        {
            if (fresh == nullptr) return;
            prepareFx (*fresh);
            fresh->setPlayHead (&head);
            const juce::ScopedLock sl (lock);
            chainOf (b).push_back (std::move (fresh));
        }

        PluginPtr removeFx (int b, int i)
        {
            const juce::ScopedLock sl (lock);
            auto& c = chainOf (b);
            if (i < 0 || i >= (int) c.size()) return nullptr;
            PluginPtr old = std::move (c[(size_t) i]);
            c.erase (c.begin() + i);
            return old;
        }

        void moveFx (int b, int i, int dir)
        {
            const juce::ScopedLock sl (lock);
            auto& c = chainOf (b);
            const int j = i + dir;
            if (i < 0 || j < 0 || i >= (int) c.size() || j >= (int) c.size()) return;
            std::swap (c[(size_t) i], c[(size_t) j]);
        }

        // βγάζει MIDI FX και FX ενός block που αφαιρείται
        std::vector<PluginPtr> takeExtras (int b)
        {
            std::vector<PluginPtr> out;
            const juce::ScopedLock sl (lock);
            auto& B = blocks[(size_t) b];
            if (B.midiFx != nullptr) out.push_back (std::move (B.midiFx));
            for (auto& p : B.fx) out.push_back (std::move (p));
            B.fx.clear();
            return out;
        }

        // loops: αποθήκευση / φόρτωση νοτών
        bool getLoop (int b, LoopSnapshot& snap)
        {
            const juce::ScopedLock sl (lock);
            auto& B = blocks[(size_t) b];
            if (B.nLayers == 0 || B.loopBeats <= 0.0) return false;
            snap.beats = B.loopBeats;
            snap.mute = B.muteMask;
            snap.layers.clear();
            for (int l = 0; l < B.nLayers; ++l) { snap.layers.push_back (B.layerEv[(size_t) l]); snap.layerBeats.push_back (B.layerBeats[(size_t) l]); }
            return true;
        }

        void setLoop (int b, const LoopSnapshot& snap)
        {
            const juce::ScopedLock sl (lock);
            auto& B = blocks[(size_t) b];
            if (B.plugin == nullptr) return;
            cutLoopNotes (B);
            B.rec.clear(); B.recOpen.reset();
            for (auto& l : B.layerEv) l.clear();
            B.nLayers = juce::jmin ((int) snap.layers.size(), maxLayers);
            for (int l = 0; l < B.nLayers; ++l) B.layerEv[(size_t) l] = snap.layers[(size_t) l];
            B.loopBeats = 0.0;
            for (int l = 0; l < B.nLayers; ++l)
            {
                const double lb = (size_t) l < snap.layerBeats.size() && snap.layerBeats[(size_t) l] > 0.0 ? snap.layerBeats[(size_t) l] : snap.beats;
                B.layerBeats[(size_t) l] = lb;
                B.loopBeats = std::max (B.loopBeats, lb);
            }
            if (B.nLayers == 0) B.loopBeats = snap.beats;
            B.muteMask = snap.mute;
            B.loopState = B.nLayers > 0 ? loopPlay : loopOff;
        }

        // metronome: ο ήχος (μονοφωνικός) φορτώνεται από το UI
        void setMetroSample (juce::AudioBuffer<float>&& s)
        {
            const juce::ScopedLock sl (lock);
            metroSample = std::move (s);
            metroPos = -1;
        }

        //==================== audio ====================
        void audioDeviceAboutToStart (juce::AudioIODevice* device) override
        {
            sampleRate = device->getCurrentSampleRate();
            blockSize  = device->getCurrentBufferSizeSamples();
            outLatency = (double) device->getOutputLatencyInSamples() / juce::jmax (1.0, sampleRate.load());
            incoming.reset (sampleRate.load());
            recorder.prepare (sampleRate.load());

            const juce::ScopedLock sl (lock);
            const int cap = juce::jmax (blockSize, 4096);
            masterBuf.setSize (2, cap);
            stemBuf.setSize (2 * RecorderEngine::kStems, cap);
            metroBuf.setSize (1, cap);
            fxTmp.setSize (8, cap);

            auto reprepare = [this, cap] (juce::AudioPluginInstance& p, bool asFx)
            {
                p.releaseResources();
                if (asFx) prepareFx (p); else prepare (p);
            };

            for (auto& B : blocks)
            {
                if (B.plugin != nullptr)
                {
                    reprepare (*B.plugin, false);
                    B.scratch.setSize (juce::jmax (2, juce::jmax (B.plugin->getTotalNumInputChannels(),
                                                                   B.plugin->getTotalNumOutputChannels())), cap);
                }
                if (B.midiFx != nullptr)
                {
                    reprepare (*B.midiFx, false);
                    B.mfxScratch.setSize (juce::jmax (1, juce::jmax (B.midiFx->getTotalNumInputChannels(), B.midiFx->getTotalNumOutputChannels())), cap);
                }
                for (auto& p : B.fx) reprepare (*p, true);
            }
            for (auto& p : masterFx) reprepare (*p, true);
        }

        void audioDeviceStopped() override
        {
            const juce::ScopedLock sl (lock);
            for (auto& B : blocks)
            {
                if (B.plugin != nullptr) B.plugin->releaseResources();
                if (B.midiFx != nullptr) B.midiFx->releaseResources();
                for (auto& p : B.fx) p->releaseResources();
            }
            for (auto& p : masterFx) p->releaseResources();
            recorder.release();
        }

        void audioDeviceIOCallbackWithContext (const float* const*, int, float* const* outputChannelData,
                                               int numOutputChannels, int numSamples,
                                               const juce::AudioIODeviceCallbackContext&) override
        {
            juce::ScopedNoDenormals noDenormals;

            for (int c = 0; c < numOutputChannels; ++c)
                juce::FloatVectorOperations::clear (outputChannelData[c], numSamples);
            numOutChannels = numOutputChannels;

            const juce::ScopedLock sl (lock);

            const double sr = sampleRate.load();
            const int64_t gs = globalPos;

            if (masterBuf.getNumSamples() < numSamples)
            {
                masterBuf.setSize (2, numSamples, false, false, true);
                stemBuf.setSize (2 * RecorderEngine::kStems, numSamples, false, false, true);
                metroBuf.setSize (1, numSamples, false, false, true);
            }

            // ---- ρολόι: tempo και θέση beat (εσωτερικά ή από Ableton Link)
            double bpm = juce::jlimit (40.0, 240.0, tempo.load());
            if (linkWanted.load() && link.isEnabled())
            {
                double lt = bpm, lb = beatPos;
                link.capture (outLatency, lt, lb);
                if (lt >= 20.0 && lt <= 999.0) { bpm = lt; tempo = juce::jlimit (40.0, 240.0, lt); }
                beatPos = lb;
                linkPeersOut = link.numPeers();
            }
            blkBps = bpm / (60.0 * sr);
            blkBeat0 = beatPos;
            head.bpm = bpm; head.ppq = beatPos; head.samples = gs;

            // ---- tap από το κουμπί
            for (int tr = tapRequest.load(); seenTap != tr; )
            {
                ++seenTap;
                registerTap (gs, sr);
            }

            if (seenPanic != panicCount.load())
            {
                owner.fill (-1);
                seenPanic = panicCount.load();
            }

            dispatchIncoming (numSamples, gs, sr);
            tapActiveOut = (lastTapT > 0 && (double) (gs - lastTapT) < 3.0 * sr) ? 1 : 0;

            masterBuf.clear (0, numSamples);
            masterBuf.clear (1, numSamples);
            stemBuf.clear (0, numSamples);
            uint32_t stemMask = 0;

            for (int b = 0; b < numBlocks; ++b)
            {
                auto& B = blocks[(size_t) b];
                if (B.plugin == nullptr)
                {
                    B.seenMain = cmdMain[(size_t) b].load();
                    B.seenUndo = cmdUndo[(size_t) b].load();
                    B.seenStop = cmdStop[(size_t) b].load();
                    B.seenMute = cmdMute[(size_t) b].load();
                    continue;
                }

                runBlock (b, gs, numSamples, sr);
                applyChord (B, b);

                // MIDI FX: τα νότες/CC περνούν από το MIDI effect πριν φτάσουν στο instrument
                if (B.midiFx != nullptr)
                {
                    if (B.mfxScratch.getNumSamples() < numSamples)
                        B.mfxScratch.setSize (B.mfxScratch.getNumChannels(), numSamples, false, false, true);
                    B.mfxScratch.clear();
                    juce::AudioBuffer<float> mview (B.mfxScratch.getArrayOfWritePointers(), B.mfxScratch.getNumChannels(), numSamples);
                    B.midiFx->processBlock (mview, B.out);
                }

                if (B.panicPending)
                {
                    B.panicPending = false;
                    for (int ch = 1; ch <= 16; ++ch)
                    {
                        B.out.addEvent (juce::MidiMessage::allNotesOff (ch), 0);
                        B.out.addEvent (juce::MidiMessage::allSoundOff (ch), 0);
                    }
                }

                if (B.scratch.getNumSamples() < numSamples)
                    B.scratch.setSize (B.scratch.getNumChannels(), numSamples, false, false, true);

                B.scratch.clear();
                juce::AudioBuffer<float> view (B.scratch.getArrayOfWritePointers(), B.scratch.getNumChannels(), numSamples);
                B.plugin->processBlock (view, B.out);

                const int chans = view.getNumChannels();
                float* l = view.getWritePointer (0);
                float* r = view.getWritePointer (juce::jmin (1, chans - 1));
                if (chans < 2) juce::FloatVectorOperations::copy (B.scratch.getWritePointer (1), l, numSamples);

                // FX του block (μετά το instrument, πριν από την ένταση)
                for (auto& fx : B.fx)
                    processFx (*fx, B.scratch.getWritePointer (0), B.scratch.getWritePointer (1), numSamples);

                const float shown = curveGain (curveMode[(size_t) b].load(), volPos[(size_t) b].load());
                gainOut[(size_t) b] = shown;
                const float gain = blockOff[(size_t) b].load() != 0 ? 0.0f : shown;
                if (B.prevGain < 0.0f) B.prevGain = gain;
                if (B.prevGain != gain || gain != 1.0f)
                {
                    B.scratch.applyGainRamp (0, 0, numSamples, B.prevGain, gain);
                    B.scratch.applyGainRamp (1, 0, numSamples, B.prevGain, gain);
                }
                B.prevGain = gain;

                masterBuf.addFrom (0, 0, B.scratch, 0, 0, numSamples);
                masterBuf.addFrom (1, 0, B.scratch, 1, 0, numSamples);
                stemBuf.copyFrom (2 * b,     0, B.scratch, 0, 0, numSamples);
                stemBuf.copyFrom (2 * b + 1, 0, B.scratch, 1, 0, numSamples);
                stemMask |= (1u << b);
            }

            // ---- master FX (δεν μπαίνουν στα stems)
            for (auto& fx : masterFx)
                processFx (*fx, masterBuf.getWritePointer (0), masterBuf.getWritePointer (1), numSamples);

            // ---- έξοδος
            if (numOutputChannels == 1)
            {
                juce::FloatVectorOperations::addWithMultiply (outputChannelData[0], masterBuf.getReadPointer (0), 0.5f, numSamples);
                juce::FloatVectorOperations::addWithMultiply (outputChannelData[0], masterBuf.getReadPointer (1), 0.5f, numSamples);
            }
            else if (numOutputChannels >= 2)
            {
                juce::FloatVectorOperations::add (outputChannelData[0], masterBuf.getReadPointer (0), numSamples);
                juce::FloatVectorOperations::add (outputChannelData[1], masterBuf.getReadPointer (1), numSamples);
            }

            // ---- metronome: μονοφωνικός, σε ξεχωριστή έξοδο, δεν περνά από master FX ούτε από την ηχογράφηση
            const double beat1 = blkBeat0 + (double) numSamples * blkBps;
            if (metroOn.load())
            {
                renderMetronome (numSamples, blkBeat0, beat1);
                const int mc = metroChannel.load();
                if (mc >= 0 && mc < numOutputChannels)
                    juce::FloatVectorOperations::add (outputChannelData[mc], metroBuf.getReadPointer (0), numSamples);
            }
            else
            {
                metroPos = -1;
            }

            {
                juce::AudioBuffer<float> mview (masterBuf.getArrayOfWritePointers(), 2, numSamples);
                juce::AudioBuffer<float> sview (stemBuf.getArrayOfWritePointers(), 2 * RecorderEngine::kStems, numSamples);
                recorder.process (mview, sview, stemMask);
            }

            // ---- loops screen: μήκος άξονα
            double longest = 0.0;
            for (auto& B : blocks)
            {
                if (B.plugin == nullptr) continue;
                if (B.loopState == loopPlay || B.loopState == loopOver) longest = std::max (longest, B.loopBeats);
                if (B.loopState == loopRec) longest = std::max (longest, beat1 - B.recStart);
            }
            const double bars = longest / beatsPerBar;
            axisBarsOut = bars <= 4.0 + 1e-6 ? 4 : bars <= 8.0 + 1e-6 ? 8 : 16;

            beatPos = beat1;
            beatPosOut = beat1;
            globalPos += numSamples;
        }

    private:
        //==================== data ====================
        enum class Kind { GateOff, Live, Raw, ArpStep, LoopEvt };

        struct Action
        {
            int pos = 0;
            int order = 0;
            Kind kind = Kind::Live;
            int note = 0;
            int vel = 0;
            bool on = false;
            int rawIndex = -1;
            bool rawLoop = false;
            int d2 = 0;
        };

        struct Block
        {
            PluginPtr plugin, midiFx;
            std::vector<PluginPtr> fx;
            juce::AudioBuffer<float> scratch, mfxScratch;
            juce::MidiBuffer out;
            std::bitset<128> pressed, latched, sounding, loopSounding, recOpen;
            std::array<std::vector<LoopEvent>, maxLayers> layerEv;
            std::array<double, maxLayers> layerBeats {};
            int nLayers = 0;
            unsigned muteMask = 0;
            int seenMain = 0, seenUndo = 0, seenStop = 0, seenMute = 0, seenPanic = 0, taps = 0;
            bool stopped = false, stopTapValid = false;
            int64_t lastStopTap = 0;
            unsigned loopSusMask = 0, loopBendMask = 0;
            int64_t lastTap = 0;
            std::array<uint8_t, 128> vel {};
            std::array<uint8_t, 128> outNote {}, loopOut {};
            std::array<std::array<uint8_t, 6>, 128> chordNotes {};   // νότες συγχορδίας ανά πλήκτρο (εκτός της βασικής)
            std::array<uint8_t, 128> chordCount {};
            float prevGain = -1.0f;
            int lastLatched = -1;
            int prevHold = 0, prevArp = 0;
            int arpIndex = 0;
            int loopState = loopOff;
            double recStart = 0.0, loopBeats = 0.0;
            bool panicPending = false;
            std::vector<LoopEvent> rec;

            void resetState()
            {
                pressed.reset(); latched.reset(); sounding.reset(); loopSounding.reset(); recOpen.reset();
                vel.fill (0);
                outNote.fill (0); loopOut.fill (0); chordCount.fill (0);
                lastLatched = -1; prevHold = 0; prevArp = 0; arpIndex = 0;
                loopState = loopOff; recStart = 0.0; loopBeats = 0.0;
                rec.clear();
                for (auto& l : layerEv) l.clear();
                nLayers = 0; muteMask = 0; taps = 0; lastTap = 0; stopped = false; stopTapValid = false; loopSusMask = 0; loopBendMask = 0;
                out.clear();
            }
        };

        // AudioPlayHead: tempo και θέση μεταφοράς για τα plugins
        struct Head : public juce::AudioPlayHead
        {
            double bpm = 120.0, ppq = 0.0;
            int64_t samples = 0;
            juce::Optional<PositionInfo> getPosition() const override
            {
                PositionInfo info;
                info.setBpm (bpm);
                info.setTimeSignature (juce::AudioPlayHead::TimeSignature { 4, 4 });
                info.setPpqPosition (ppq);
                info.setPpqPositionOfLastBarStart (std::floor (ppq / 4.0) * 4.0);
                info.setTimeInSamples (samples);
                info.setIsPlaying (true);
                info.setIsRecording (false);
                info.setIsLooping (false);
                return info;
            }
        };

        juce::CriticalSection lock;
        std::array<Block, numBlocks> blocks;
        std::vector<PluginPtr> masterFx;
        juce::MidiBuffer chordTmp;
        Head head;
        std::array<int, 128> owner;
        std::array<std::vector<Action>, numBlocks> inActions;
        std::array<std::vector<juce::MidiMessage>, numBlocks> inRaw;
        std::vector<Action> actions;
        juce::MidiBuffer midiIn, fxMidi;
        juce::AudioBuffer<float> masterBuf, stemBuf, metroBuf, fxTmp, metroSample;
        std::atomic<int> seqCounter { 0 };
        int64_t globalPos = 0;
        double beatPos = 0.0;
        std::atomic<double> sampleRate { 44100.0 };
        std::atomic<bool> linkWanted { false };
        double outLatency = 0.0;
        int blockSize = 512;

        // ρολόι του τρέχοντος block ήχου
        double blkBeat0 = 0.0, blkBps = 0.0;

        // tap tempo
        std::vector<int64_t> tapTimes;
        int64_t lastTapT = 0;
        int seenTap = 0, seenPanic = 0;
        int lastFreeNote = -1;
        int64_t lastFreeT = 0;

        // metronome
        int metroPos = -1;
        bool metroAccent = false;

        std::vector<PluginPtr>& chainOf (int b) { return b < 0 ? masterFx : blocks[(size_t) b].fx; }
        const std::vector<PluginPtr>& chainOf (int b) const { return b < 0 ? masterFx : blocks[(size_t) b].fx; }

        //==================== plugin set-up ====================
        void prepare (juce::AudioPluginInstance& p)
        {
            auto layout = p.getBusesLayout();

            auto instrument = layout;
            for (int i = 0; i < instrument.inputBuses.size(); ++i)
                instrument.inputBuses.getReference (i) = juce::AudioChannelSet::disabled();
            for (int i = 0; i < instrument.outputBuses.size(); ++i)
                instrument.outputBuses.getReference (i) = i == 0 ? juce::AudioChannelSet::stereo()
                                                                 : juce::AudioChannelSet::disabled();

            if (! p.setBusesLayout (instrument))
            {
                auto effect = layout;
                for (int i = 0; i < effect.inputBuses.size(); ++i)
                    effect.inputBuses.getReference (i) = i == 0 ? juce::AudioChannelSet::stereo()
                                                                : juce::AudioChannelSet::disabled();
                for (int i = 0; i < effect.outputBuses.size(); ++i)
                    effect.outputBuses.getReference (i) = i == 0 ? juce::AudioChannelSet::stereo()
                                                                 : juce::AudioChannelSet::disabled();
                p.setBusesLayout (effect);
            }

            const double sr = sampleRate.load();
            p.setPlayConfigDetails (p.getTotalNumInputChannels(), p.getTotalNumOutputChannels(), sr, blockSize);
            p.prepareToPlay (sr, blockSize);
        }

        // audio effect: στερεοφωνική είσοδος και έξοδος
        void prepareFx (juce::AudioPluginInstance& p)
        {
            auto effect = p.getBusesLayout();
            for (int i = 0; i < effect.inputBuses.size(); ++i)
                effect.inputBuses.getReference (i) = i == 0 ? juce::AudioChannelSet::stereo() : juce::AudioChannelSet::disabled();
            for (int i = 0; i < effect.outputBuses.size(); ++i)
                effect.outputBuses.getReference (i) = i == 0 ? juce::AudioChannelSet::stereo() : juce::AudioChannelSet::disabled();
            p.setBusesLayout (effect);

            const double sr = sampleRate.load();
            p.setPlayConfigDetails (p.getTotalNumInputChannels(), p.getTotalNumOutputChannels(), sr, blockSize);
            p.prepareToPlay (sr, blockSize);
        }

        void processFx (juce::AudioPluginInstance& p, float* l, float* r, int n)
        {
            const int need = juce::jmax (2, juce::jmax (p.getTotalNumInputChannels(), p.getTotalNumOutputChannels()));
            if (fxTmp.getNumChannels() < need || fxTmp.getNumSamples() < n)
                fxTmp.setSize (juce::jmax (need, fxTmp.getNumChannels()), juce::jmax (n, fxTmp.getNumSamples()), false, false, true);

            for (int c = 0; c < fxTmp.getNumChannels(); ++c) fxTmp.clear (c, 0, n);
            juce::FloatVectorOperations::copy (fxTmp.getWritePointer (0), l, n);
            juce::FloatVectorOperations::copy (fxTmp.getWritePointer (1), r, n);

            juce::AudioBuffer<float> view (fxTmp.getArrayOfWritePointers(), need, n);
            fxMidi.clear();
            p.processBlock (view, fxMidi);

            const int oc = p.getTotalNumOutputChannels();
            juce::FloatVectorOperations::copy (l, fxTmp.getReadPointer (0), n);
            juce::FloatVectorOperations::copy (r, fxTmp.getReadPointer (oc > 1 ? 1 : 0), n);
        }

        //==================== tap tempo ====================
        void registerTap (int64_t t, double sr)
        {
            if (! tapTimes.empty() && (double) (t - tapTimes.back()) > 2.0 * sr)
                tapTimes.clear();
            tapTimes.push_back (t);
            if (tapTimes.size() > 8) tapTimes.erase (tapTimes.begin());
            lastTapT = std::max<int64_t> (1, t);

            if (tapTimes.size() >= 2)
            {
                const double avg = (double) (tapTimes.back() - tapTimes.front()) / (double) (tapTimes.size() - 1) / sr;
                if (avg > 0.0)
                {
                    const double bpm = juce::jlimit (40.0, 240.0, 60.0 / avg);
                    tempo = bpm;
                    if (link.isEnabled()) link.setTempo (bpm);
                    ++changeCount;
                }
            }
        }

        //==================== metronome ====================
        void renderMetronome (int n, double beat0, double beat1)
        {
            float* out = metroBuf.getWritePointer (0);
            juce::FloatVectorOperations::clear (out, n);

            std::array<int, 8> startPos {};
            std::array<bool, 8> startAccent {};
            int starts = 0;
            for (double k = std::ceil (beat0 - 1.0e-9); k < beat1 - 1.0e-9 && starts < 8; k += 1.0)
            {
                startPos[(size_t) starts] = juce::jlimit (0, n - 1, (int) ((k - beat0) / blkBps));
                startAccent[(size_t) starts] = std::fmod (k, beatsPerBar) < 1.0e-6;
                ++starts;
            }

            const double sr = sampleRate.load();
            const float level = metroLevel.load();
            const bool useSample = metroSample.getNumSamples() > 0;
            int next = 0;

            for (int i = 0; i < n; ++i)
            {
                while (next < starts && startPos[(size_t) next] <= i)
                {
                    metroPos = 0;
                    metroAccent = startAccent[(size_t) next];
                    ++next;
                }
                if (metroPos < 0) continue;

                float v;
                if (useSample)
                {
                    if (metroPos >= metroSample.getNumSamples()) { metroPos = -1; continue; }
                    v = metroSample.getSample (0, metroPos);
                }
                else
                {
                    const double t = (double) metroPos / sr;
                    if (t > 0.06) { metroPos = -1; continue; }
                    v = (float) (std::sin (juce::MathConstants<double>::twoPi * (metroAccent ? 1500.0 : 1000.0) * t) * std::exp (-t * 70.0));
                }
                out[i] = v * level * (metroAccent ? 1.0f : 0.7f);
                ++metroPos;
            }
        }

        //==================== incoming MIDI -> blocks ====================
        // το block που περιμένει ζώνη: το επιλεγμένο αν περιμένει, αλλιώς το πρώτο που περιμένει
        int captureTarget() const
        {
            const int sel = selectedBlock.load();
            if (sel >= 0 && sel < numBlocks && active[(size_t) sel].load() != 0 && zoneState[(size_t) sel].load() != 0)
                return sel;
            for (int b = 0; b < numBlocks; ++b)
                if (active[(size_t) b].load() != 0 && zoneState[(size_t) b].load() != 0)
                    return b;
            return -1;
        }

        void dispatchIncoming (int numSamples, int64_t gs, double sr)
        {
            for (int b = 0; b < numBlocks; ++b)
            {
                inActions[(size_t) b].clear();
                inRaw[(size_t) b].clear();
            }

            midiIn.clear();
            incoming.removeNextBlockOfMessages (midiIn, numSamples);

            for (const auto metadata : midiIn)
            {
                const auto msg = metadata.getMessage();
                const int pos = juce::jlimit (0, numSamples - 1, metadata.samplePosition);
                if (! msg.isMidiClock() && ! msg.isActiveSense()) ++midiCount;

                if (handlePedal (msg))
                    continue;

                if (msg.isNoteOn())
                {
                    const int n = msg.getNoteNumber();
                    const int cb = captureTarget();

                    if (cb >= 0)    // ορισμός ζώνης: το πρώτο και το δεύτερο πλήκτρο (ίδιο πλήκτρο δύο φορές = ζώνη μίας νότας)
                    {
                        if (zoneState[(size_t) cb].load() == 1)
                        {
                            zoneFirst[(size_t) cb] = n;
                            zoneState[(size_t) cb] = 2;
                        }
                        else
                        {
                            setZone (cb, zoneFirst[(size_t) cb].load(), n);
                            ++changeCount;
                        }
                        continue;
                    }

                    const int b = blockForNote (n);
                    if (b >= 0)
                    {
                        owner[(size_t) n] = b;
                        Action a;
                        a.pos = pos; a.order = 1; a.kind = Kind::Live; a.note = n; a.vel = msg.getVelocity(); a.on = true;
                        inActions[(size_t) b].push_back (a);
                    }
                    else if (isBlackKey (n))    // ελεύθερα μαύρα πλήκτρα: δύο μαζί (σε ~50 ms) = tap tempo
                    {
                        const int64_t t = gs + pos;
                        if (lastFreeNote >= 0 && lastFreeNote != n && (double) (t - lastFreeT) <= 0.05 * sr)
                        {
                            registerTap (t, sr);
                            lastFreeNote = -1;
                        }
                        else
                        {
                            lastFreeNote = n;
                            lastFreeT = t;
                        }
                    }
                }
                else if (msg.isNoteOff())
                {
                    const int n = msg.getNoteNumber();
                    const int b = owner[(size_t) n];
                    if (b >= 0)
                    {
                        owner[(size_t) n] = -1;
                        Action a;
                        a.pos = pos; a.order = 1; a.kind = Kind::Live; a.note = n; a.on = false;
                        inActions[(size_t) b].push_back (a);
                    }
                }
                else if (msg.isAftertouch())
                {
                    const int b = owner[(size_t) msg.getNoteNumber()];
                    if (b >= 0) pushRaw (b, pos, msg);
                }
                else if (msg.isController() || msg.isPitchWheel() || msg.isChannelPressure())
                {
                    for (int b = 0; b < numBlocks; ++b)
                        if (active[(size_t) b].load() != 0)
                            pushRaw (b, pos, msg);
                }
            }
        }

        // πετάλια και πλήκτρα ελέγχου: επιστρέφει true αν το μήνυμα το κατανάλωσε η ανάθεση (δεν πάει στα plugins)
        bool handlePedal (const juce::MidiMessage& msg)
        {
            int kind = 0, num = 0;
            bool press = false;

            if (msg.isNoteOn())             { kind = 1; num = msg.getNoteNumber(); press = true; }
            else if (msg.isNoteOff())       { kind = 1; num = msg.getNoteNumber(); }
            else if (msg.isController())    { kind = 2; num = msg.getControllerNumber(); press = msg.getControllerValue() >= 64; }
            else if (msg.isProgramChange()) { kind = 3; num = msg.getProgramChangeNumber(); press = true; }

            if (kind == 0) return false;

            const int code = (kind << 16) | (msg.getChannel() << 8) | num;

            // MIDI learn πεταλιού έντασης: η πρώτη κίνηση ενός CC το αναθέτει στο block
            const int vl = volLearn.load();
            if (vl >= 0 && kind == 2)
            {
                volMap[(size_t) vl] = code;
                for (int i = 0; i < numControls; ++i)
                    if (mapCode[(size_t) i].load() == code) mapCode[(size_t) i] = 0;
                volLearnedBlock = vl;
                volLearnedCode = code;
                ++volLearnedCount;
                volLearn = -1;
                ++changeCount;
                return true;
            }

            // MIDI learn κουμπιού
            const int target = mapLearnTarget.load();
            if (target >= 0 && press)
            {
                for (int i = 0; i < numControls; ++i)
                    if (mapCode[(size_t) i].load() == code) mapCode[(size_t) i] = 0;
                for (int b = 0; b < numBlocks; ++b)
                    if (volMap[(size_t) b].load() == code) volMap[(size_t) b] = 0;

                mapCode[(size_t) target] = code;
                learnedTarget = target;
                learnedCode = code;
                ++learnedCount;
                mapLearnTarget = -1;
                ++changeCount;
                return true;
            }

            // πετάλι έντασης: ένα πετάλι μπορεί να ελέγχει πολλά blocks
            if (kind == 2)
            {
                bool hit = false;
                const float v = (float) msg.getControllerValue() / 127.0f;
                for (int b = 0; b < numBlocks; ++b)
                    if (active[(size_t) b].load() != 0 && volMap[(size_t) b].load() == code)
                    {
                        volPos[(size_t) b] = v;
                        hit = true;
                    }
                if (hit)
                {
                    ++changeCount;
                    return true;
                }
            }

            // πετάλι / πλήκτρο κουμπιού: δρα στο επιλεγμένο block
            for (int i = 0; i < numControls; ++i)
            {
                if (mapCode[(size_t) i].load() != code) continue;

                if (press)
                {
                    const int sel = selectedBlock.load();
                    const bool ok = sel >= 0 && sel < numBlocks && active[(size_t) sel].load() != 0;

                    switch (i)
                    {
                        case ctlHold:  if (ok) { holdMode[(size_t) sel] = (holdMode[(size_t) sel].load() + 1) % 3; ++changeCount; } break;
                        case ctlRec:   if (ok) ++cmdMain[(size_t) sel]; break;
                        case ctlUndo:  if (ok) ++cmdUndo[(size_t) sel]; break;
                        case ctlStop:  if (ok) ++cmdStop[(size_t) sel]; break;
                        case ctlMute:  if (ok) ++cmdMute[(size_t) sel]; break;
                        case ctlTape:  recorder.pressButton(); break;
                        case ctlMetro: metroOn = ! metroOn.load(); break;
                        case ctlPanic: ++panicCount; break;
                        case ctlScenePrev: sceneRequest = -2; break;
                        case ctlSceneNext: sceneRequest = -3; break;
                        default: break;
                    }
                }
                return true;
            }

            return false;
        }


        //==================== Chord ====================
        // Το πλήκτρο που πατάς δίνει μία νότα. Εδώ, μετά το hold/loop/transpose και πριν από το MIDI FX, γίνεται συγχορδία.
        // Έτσι το loop γράφει μόνο τη μία νότα και αν αλλάξει ο τύπος, αλλάζει και το loop.
        static int chordIntervals (int type, int* out)
        {
            static const int T[][5] = { { 0, 4, 7, -1, -1 }, { 0, 3, 7, -1, -1 }, { 0, 4, 7, 10, -1 }, { 0, 4, 7, 11, -1 },
                                        { 0, 3, 7, 10, -1 }, { 0, 5, 7, -1, -1 }, { 0, 3, 6, -1, -1 } };
            const int* t = T[juce::jlimit (0, numChordTypes - 1, type)];
            int n = 0;
            for (int i = 0; i < 5 && t[i] >= 0; ++i) out[n++] = t[i];
            return n;
        }

        int buildChord (int b, int note, int* out) const
        {
            const int mode = chordMode[(size_t) b].load();
            if (mode == 1) return chordIntervals (chordType[(size_t) b].load(), out);

            if (mode == 2)
            {
                const int tonic = chordTonic[(size_t) b].load();
                if (tonic < 0) { out[0] = 0; return 1; }
                static const int MAJ[7] = { 0, 2, 4, 5, 7, 9, 11 }, MIN[7] = { 0, 2, 3, 5, 7, 8, 10 };
                const int* sc = chordMinor[(size_t) b].load() != 0 ? MIN : MAJ;
                const int rel = ((note - tonic) % 12 + 12) % 12;
                int d = -1;
                for (int i = 0; i < 7; ++i) if (sc[i] == rel) d = i;
                if (d < 0) { out[0] = 0; return 1; }
                int n = 0;
                for (int k : { 0, 2, 4 })
                {
                    const int idx = d + k;
                    out[n++] = sc[idx % 7] + 12 * (idx / 7) - sc[d];
                }
                return n;
            }

            out[0] = 0;
            return 1;
        }

        void applyChord (Block& B, int b)
        {
            const int mode = chordMode[(size_t) b].load();

            // πρώτο πλήκτρο σε Scale χωρίς τονική: γίνεται η τονική
            if (mode == 2 && chordTonic[(size_t) b].load() < 0)
                for (const auto meta : B.out)
                {
                    const auto m = meta.getMessage();
                    if (m.isNoteOn()) { chordTonic[(size_t) b] = m.getNoteNumber() % 12; break; }
                }

            if (mode != 0 || B.out.getNumEvents() > 0)
            {
                bool any = mode != 0;
                if (! any)
                    for (int n = 0; n < 128 && ! any; ++n) any = B.chordCount[(size_t) n] != 0;

                if (any)
                {
                    chordTmp.clear();
                    for (const auto meta : B.out)
                    {
                        const auto m = meta.getMessage();
                        const int pos = meta.samplePosition;
                        if (m.isNoteOn())
                        {
                            const int root = m.getNoteNumber();
                            if (B.chordCount[(size_t) root] != 0)
                            {
                                for (int i = 0; i < B.chordCount[(size_t) root]; ++i)
                                    chordTmp.addEvent (juce::MidiMessage::noteOff (m.getChannel(), B.chordNotes[(size_t) root][(size_t) i]), pos);
                                B.chordCount[(size_t) root] = 0;
                            }
                            chordTmp.addEvent (m, pos);
                            int iv[8];
                            const int cnt = mode != 0 ? buildChord (b, root, iv) : 1;
                            int stored = 0;
                            for (int i = 1; i < cnt && stored < 6; ++i)
                            {
                                const int nn = root + iv[i];
                                if (nn < 0 || nn > 127) continue;
                                B.chordNotes[(size_t) root][(size_t) stored++] = (uint8_t) nn;
                                chordTmp.addEvent (juce::MidiMessage::noteOn (m.getChannel(), nn, m.getVelocity()), pos);
                            }
                            B.chordCount[(size_t) root] = (uint8_t) stored;
                        }
                        else if (m.isNoteOff())
                        {
                            const int root = m.getNoteNumber();
                            chordTmp.addEvent (m, pos);
                            for (int i = 0; i < B.chordCount[(size_t) root]; ++i)
                                chordTmp.addEvent (juce::MidiMessage::noteOff (m.getChannel(), B.chordNotes[(size_t) root][(size_t) i]), pos);
                            B.chordCount[(size_t) root] = 0;
                        }
                        else
                            chordTmp.addEvent (m, pos);
                    }
                    B.out.swapWith (chordTmp);
                }
            }

            // ανάγνωση για την ανίχνευση ονόματος συγχορδίας (pitch classes + μπάσο των νοτών που ακούγονται, με transpose)
            int mask = 0, bass = -1;
            for (int n = 0; n < 128; ++n)
            {
                if (! B.sounding[(size_t) n]) continue;
                const int on = B.outNote[(size_t) n];
                if (bass < 0 || on < bass) bass = on;
                mask |= 1 << (on % 12);
                for (int i = 0; i < B.chordCount[(size_t) on]; ++i)
                    mask |= 1 << (B.chordNotes[(size_t) on][(size_t) i] % 12);
            }
            pcMaskOut[(size_t) b] = mask;
            bassOut[(size_t) b] = bass < 0 ? -1 : bass % 12;

            uint64_t lb[2] { 0, 0 };
            if (holdMode[b].load() != 0)
            {
                auto setb = [&] (int n) { if (n >= 0 && n < 128) lb[n >> 6] |= (uint64_t) 1 << (n & 63); };
                for (int n = 0; n < 128; ++n)
                {
                    if (! B.latched[(size_t) n]) continue;
                    setb (n);
                    if (B.sounding[(size_t) n])
                    {
                        const int on = B.outNote[(size_t) n];
                        for (int i = 0; i < B.chordCount[(size_t) on]; ++i)
                            setb (B.chordNotes[(size_t) on][(size_t) i]);
                    }
                }
            }
            latchBits[(size_t) b * 2] = lb[0];
            latchBits[(size_t) b * 2 + 1] = lb[1];
        }

        void pushRaw (int b, int pos, const juce::MidiMessage& msg)
        {
            Action a;
            a.pos = pos; a.order = 1; a.kind = Kind::Raw;
            a.rawIndex = (int) inRaw[(size_t) b].size();
            inRaw[(size_t) b].push_back (msg);
            inActions[(size_t) b].push_back (a);
        }

        //==================== per-block logic ====================
        static int highestSet (const std::bitset<128>& s)
        {
            for (int n = 127; n >= 0; --n)
                if (s[(size_t) n])
                    return n;
            return -1;
        }

        // pos: θέση μέσα στο block ήχου. Το transpose εφαρμόζεται εδώ, πριν από το MIDI FX και το instrument.
        void emitNote (Block& B, int b, int pos, bool on, int note, int vel)
        {
            if (on)
            {
                B.outNote[(size_t) note] = (uint8_t) juce::jlimit (0, 127, note + transpose[(size_t) b].load());
                B.out.addEvent (juce::MidiMessage::noteOn (1, B.outNote[(size_t) note], (juce::uint8) vel), pos);
            }
            else
            {
                B.out.addEvent (juce::MidiMessage::noteOff (1, B.outNote[(size_t) note]), pos);
            }
            B.sounding.set ((size_t) note, on);

            if (B.loopState == loopRec || B.loopState == loopOver)
            {
                B.rec.push_back ({ blkBeat0 + (double) pos * blkBps, on, note, vel });
                B.recOpen.set ((size_t) note, on);
            }
        }

        void offAll (Block& B, int b, int pos)
        {
            for (int n = 0; n < 128; ++n)
                if (B.sounding[(size_t) n])
                    emitNote (B, b, pos, false, n, 0);
        }

        // brings the sounding notes in line with what should sound now
        void updateOutput (Block& B, int b, int pos, int hold, int arp)
        {
            const auto& target = hold == 0 ? B.pressed : B.latched;

            if (arp == 0)
            {
                for (int n = 0; n < 128; ++n)
                    if (B.sounding[(size_t) n] && ! target[(size_t) n])
                        emitNote (B, b, pos, false, n, 0);

                for (int n = 0; n < 128; ++n)
                    if (target[(size_t) n] && ! B.sounding[(size_t) n])
                        emitNote (B, b, pos, true, n, B.vel[(size_t) n]);
            }
            else if (target.none())
            {
                offAll (B, b, pos);
                B.arpIndex = 0;
            }
        }

        void arpStep (Block& B, int b, int pos, int hold, int arp)
        {
            const auto& target = hold == 0 ? B.pressed : B.latched;
            if (target.none()) return;

            offAll (B, b, pos);

            if (arp == 1)   // mono: one note per step, going up through the held notes
            {
                const int count = (int) target.count();
                int pick = B.arpIndex % count;
                B.arpIndex = (B.arpIndex + 1) % 4096;

                for (int n = 0; n < 128; ++n)
                {
                    if (! target[(size_t) n]) continue;
                    if (pick-- == 0)
                    {
                        emitNote (B, b, pos, true, n, B.vel[(size_t) n]);
                        break;
                    }
                }
            }
            else            // poly: all held notes together on every step
            {
                for (int n = 0; n < 128; ++n)
                    if (target[(size_t) n])
                        emitNote (B, b, pos, true, n, B.vel[(size_t) n]);
            }
        }

        void cutLoopNotes (Block& B)
        {
            for (int n = 0; n < 128; ++n)
                if (B.loopSounding[(size_t) n])
                    B.out.addEvent (juce::MidiMessage::noteOff (1, B.loopOut[(size_t) n]), 0);

            B.loopSounding.reset();

            for (int ch = 0; ch < 16; ++ch)
            {
                if ((B.loopSusMask >> ch) & 1u)  B.out.addEvent (juce::MidiMessage::controllerEvent (ch + 1, 64, 0), 0);
                if ((B.loopBendMask >> ch) & 1u) B.out.addEvent (juce::MidiMessage::pitchWheel (ch + 1, 8192), 0);
            }
            B.loopSusMask = 0;
            B.loopBendMask = 0;
        }

        void stopLoop (Block& B)
        {
            cutLoopNotes (B);
            B.recOpen.reset();
            B.rec.clear();
            for (auto& l : B.layerEv) l.clear();
            B.nLayers = 0;
            B.muteMask = 0;
            B.taps = 0;
            B.stopped = false;
            B.loopBeats = 0.0;
            B.loopState = loopOff;
        }

        // κλείνει το layer που ηχογραφείται (rec) και το προσθέτει στο loop. Επιστρέφει true αν είχε νότες.
        bool commitLayer (Block& B, double beatNow, double len)
        {
            const double elapsed = std::max (1.0e-6, std::min (len, beatNow - B.recStart));

            for (int n = 0; n < 128; ++n)    // νότες που κρατιούνται όταν τελειώνει η ηχογράφηση σταματούν στο τέλος
                if (B.recOpen[(size_t) n])
                    B.rec.push_back ({ B.recStart + elapsed - 1.0e-4, false, n, 0 });

            const int sq = snapQ.load();
            const double grid = sq == 1 ? 0.5 : sq == 2 ? 0.25 : 0.0;
            std::array<double, 128> shift {};
            std::bitset<128> hasShift;

            auto& dst = B.layerEv[(size_t) B.nLayers];
            dst.clear();
            for (const auto& e : B.rec)
            {
                const double ph = std::max (0.0, e.phase - B.recStart);
                if (ph >= len) continue;
                double a = B.recStart + ph;

                if (e.isRaw)    // CC, pitch bend, aftertouch: χωρίς snap
                {
                    double rp = std::fmod (a, len);
                    if (rp < 0.0) rp += len;
                    dst.push_back ({ rp, true, e.note, e.vel, true, e.d2 });
                    continue;
                }

                if (grid > 0.0)
                {
                    if (e.on)
                    {
                        const double s = std::round (a / grid) * grid;
                        shift[(size_t) e.note] = s - a;
                        hasShift.set ((size_t) e.note);
                        a = s;
                    }
                    else if (hasShift[(size_t) e.note])
                    {
                        a += shift[(size_t) e.note];
                        hasShift.reset ((size_t) e.note);
                    }
                    else
                    {
                        a = std::round (a / grid) * grid;
                    }
                }

                double p = std::fmod (a, len);
                if (p < 0.0) p += len;
                dst.push_back ({ p, e.on, e.note, e.vel });
            }

            B.rec.clear();
            B.recOpen.reset();

            if (dst.empty()) return false;
            B.layerBeats[(size_t) B.nLayers] = len;
            ++B.nLayers;
            B.loopBeats = 0.0;
            for (int l = 0; l < B.nLayers; ++l) B.loopBeats = std::max (B.loopBeats, B.layerBeats[(size_t) l]);
            return true;
        }

        // μήκος ενός layer από τον χρόνο που ηχογραφήθηκε, με το Length quantize
        double quantizedLength (double elapsed) const
        {
            const int lq = lengthQ.load();
            double beats;
            if (lq == 1)      beats = std::max (1.0, std::round (elapsed));
            else if (lq == 2) beats = std::max (1.0, std::round (elapsed / beatsPerBar)) * beatsPerBar;
            else              beats = std::max (1.0, std::ceil (elapsed / beatsPerBar - 1.0e-9)) * beatsPerBar;
            return std::min (beats, maxLoopBars * beatsPerBar);
        }

        void finishRecording (Block& B, double beatNow, bool allowOverdub = true)
        {
            const double elapsed = beatNow - B.recStart;
            if (elapsed <= 0.0) { stopLoop (B); return; }   // ακυρώθηκε πριν αρχίσει (start quantize)

            const double beats = quantizedLength (elapsed);

            if (commitLayer (B, beatNow, beats))
            {
                B.loopState = loopPlay;
                if (allowOverdub && afterFirst.load() == 1)    // μετά το πρώτο loop: κατευθείαν overdub
                {
                    B.recStart = beatNow;
                    B.rec.clear();
                    B.recOpen.reset();
                    B.loopState = loopOver;
                }
            }
            else
            {
                stopLoop (B);
            }
        }

        // κάθε layer έχει δικό του ελεύθερο μήκος: αν είναι μεγαλύτερο από το loop, το loop μεγαλώνει και τα άλλα επαναλαμβάνονται
        void finishOverdub (Block& B, double beatNow)
        {
            commitLayer (B, beatNow, quantizedLength (std::max (0.0, beatNow - B.recStart)));
            B.loopState = loopPlay;
        }

        void loopMain (Block& B, double beat)
        {
            switch (B.loopState)
            {
                case loopOff:
                {
                    const int sq = startQ.load();
                    // η ηχογράφηση ξεκινά αμέσως στο πάτημα. Το Start quantize κουμπώνει την αρχή προς τα πίσω.
                    if (sq == 1)      B.recStart = std::floor (beat + 1.0e-9);
                    else              B.recStart = std::floor (beat / beatsPerBar + 1.0e-9) * beatsPerBar;   // από την αρχή του μέτρου που πατήθηκε
                    B.rec.clear();
                    B.recOpen.reset();
                    B.loopState = loopRec;
                    break;
                }
                case loopRec:  finishRecording (B, beat); break;
                case loopPlay:
                    if (B.nLayers < maxLayers)
                    {
                        const int sq2 = startQ.load();
                        B.recStart = sq2 == 1 ? std::floor (beat + 1.0e-9)
                                   : sq2 == 2 ? std::floor (beat / beatsPerBar + 1.0e-9) * beatsPerBar
                                              : beat;
                        B.rec.clear();
                        B.recOpen.reset();
                        B.loopState = loopOver;
                    }
                    break;
                case loopOver: finishOverdub (B, beat); break;
                default: break;
            }
        }

        void loopUndo (Block& B)
        {
            if (B.loopState == loopRec)
            {
                stopLoop (B);
            }
            else if (B.loopState == loopOver)
            {
                B.rec.clear();
                B.recOpen.reset();
                B.loopState = loopPlay;
            }
            else if (B.loopState == loopPlay && B.nLayers > 0)
            {
                --B.nLayers;
                B.layerEv[(size_t) B.nLayers].clear();
                B.muteMask &= ~(1u << B.nLayers);
                B.loopBeats = 0.0;                        // το μήκος γυρίζει σε αυτό του μεγαλύτερου layer που έμεινε
                for (int l = 0; l < B.nLayers; ++l) B.loopBeats = std::max (B.loopBeats, B.layerBeats[(size_t) l]);
                cutLoopNotes (B);
                if (B.nLayers == 0) stopLoop (B);
            }
        }

        void doPanic (Block& B, int b)
        {
            for (int n = 0; n < 128; ++n)
                if (B.sounding[(size_t) n])
                    B.out.addEvent (juce::MidiMessage::noteOff (1, B.outNote[(size_t) n]), 0);
            cutLoopNotes (B);
            B.sounding.reset();
            B.pressed.reset();
            B.latched.reset();
            B.lastLatched = -1;
            B.arpIndex = 0;
            B.panicPending = true;
            juce::ignoreUnused (b);
        }

        void runBlock (int b, int64_t gs, int numSamples, double sr)
        {
            Block& B = blocks[(size_t) b];
            B.out.clear();

            const double beat0 = blkBeat0;
            const double beat1 = beat0 + (double) numSamples * blkBps;
            const double bpm = blkBps * 60.0 * sr;
            const int64_t ge = gs + numSamples;
            const int64_t stepLen = std::max<int64_t> (1, (int64_t) std::llround (sr * 60.0 / bpm / 4.0));
            const int64_t gateLen = (int64_t) ((double) stepLen * 0.8);

            const int hold = holdMode[b].load();
            const int arp  = arpMode[b].load();

            // ---- panic
            if (B.seenPanic != panicCount.load())
            {
                B.seenPanic = panicCount.load();
                doPanic (B, b);
            }

            // ---- mode changes
            bool changed = false;

            if (hold != B.prevHold)
            {
                if (hold == 0)
                {
                    B.latched.reset();
                    B.lastLatched = -1;
                }
                else if (B.prevHold == 0)
                {
                    B.latched = B.pressed;
                    B.lastLatched = highestSet (B.pressed);
                }

                if (hold == 1 && B.latched.count() > 1)
                {
                    const int keep = B.lastLatched >= 0 ? B.lastLatched : highestSet (B.latched);
                    B.latched.reset();
                    if (keep >= 0) B.latched.set ((size_t) keep);
                    B.lastLatched = keep;
                }
                changed = true;
            }

            if (arp != B.prevArp)
            {
                offAll (B, b, 0);
                B.arpIndex = 0;
                changed = true;
            }

            B.prevHold = hold;
            B.prevArp = arp;
            if (changed) updateOutput (B, b, 0, hold, arp);

            // ---- loop: εντολές (κουμπιά ή πετάλια)
            const int64_t tapWindow = (int64_t) (sr * 0.7);

            for (int cm = cmdMain[(size_t) b].load(); B.seenMain != cm; )
            {
                ++B.seenMain;
                loopMain (B, beat0);
            }

            for (int cu = cmdUndo[(size_t) b].load(); B.seenUndo != cu; )
            {
                ++B.seenUndo;
                loopUndo (B);
            }

            for (int cs = cmdStop[(size_t) b].load(); B.seenStop != cs; )
            {
                ++B.seenStop;
                if (gs - B.lastTap > tapWindow) B.taps = 0;
                ++B.taps;
                B.lastTap = gs;
            }

            if (B.taps > 0 && gs - B.lastTap > tapWindow)
            {
                // N πατήματα MUTE = σίγαση / επαναφορά του layer N
                const int layer = B.taps - 1;
                if (B.loopState == loopPlay && layer < B.nLayers)
                {
                    B.muteMask ^= (1u << layer);
                    cutLoopNotes (B);
                }
                B.taps = 0;
            }

            if (B.loopState == loopRec && (beat0 - B.recStart) >= maxLoopBars * beatsPerBar)
                finishRecording (B, beat0);
            else if (B.loopState == loopOver && (beat0 - B.recStart) >= maxLoopBars * beatsPerBar)
                finishOverdub (B, beat0);

            // ---- collect everything that happens in this block
            actions.clear();
            for (const auto& a : inActions[(size_t) b])
                actions.push_back (a);

            if (arp != 0)
            {
                for (int64_t k = (gs + stepLen - 1) / stepLen; k * stepLen < ge; ++k)
                {
                    Action a;
                    a.pos = (int) (k * stepLen - gs); a.order = 2; a.kind = Kind::ArpStep;
                    actions.push_back (a);
                }

                const int64_t g0 = gs - gateLen;
                for (int64_t k = g0 <= 0 ? 0 : (g0 + stepLen - 1) / stepLen; k * stepLen + gateLen < ge; ++k)
                {
                    const int64_t p = k * stepLen + gateLen - gs;
                    if (p < 0) continue;
                    Action a;
                    a.pos = (int) p; a.order = 0; a.kind = Kind::GateOff;
                    actions.push_back (a);
                }
            }

            if ((B.loopState == loopPlay || B.loopState == loopOver) && B.loopBeats > 0.0 && ! B.stopped)
            {
                const double L0 = B.loopBeats;
                for (int layer = 0; layer < B.nLayers; ++layer)
                {
                    if ((B.muteMask >> layer) & 1u) continue;
                    const double L = B.layerBeats[(size_t) layer] > 0.0 ? B.layerBeats[(size_t) layer] : L0;

                    for (const auto& e : B.layerEv[(size_t) layer])
                    {
                        for (double m = std::floor (beat0 / L); m <= std::floor ((beat1 - 1.0e-9) / L); m += 1.0)
                        {
                            const double x = m * L + e.phase;
                            if (x < beat0 || x >= beat1) continue;
                            Action a;
                            a.pos = juce::jlimit (0, numSamples - 1, (int) ((x - beat0) / blkBps));
                            a.order = 3; a.kind = Kind::LoopEvt;
                            a.note = e.note; a.vel = e.vel; a.on = e.on; a.rawLoop = e.isRaw; a.d2 = e.d2;
                            actions.push_back (a);
                        }
                    }
                }
            }

            std::stable_sort (actions.begin(), actions.end(), [] (const Action& x, const Action& y)
            {
                return x.pos != y.pos ? x.pos < y.pos : x.order < y.order;
            });

            // ---- run them in time order
            for (const auto& a : actions)
            {
                switch (a.kind)
                {
                    case Kind::Live:
                        if (a.on)
                        {
                            const bool firstFinger = B.pressed.none();
                            B.pressed.set ((size_t) a.note);
                            B.vel[(size_t) a.note] = (uint8_t) a.vel;

                            if (hold == 2)   // LP: κρατιέται όποια συγχορδία παίξεις, νέα συγχορδία ξεπατάει την προηγούμενη
                            {
                                if (firstFinger) B.latched.reset();
                                B.latched.set ((size_t) a.note);
                                B.lastLatched = a.note;
                            }
                            else if (hold == 1)
                            {
                                const bool onlyThis = B.latched.count() == 1 && B.latched[(size_t) a.note];
                                B.latched.reset();
                                if (! onlyThis) B.latched.set ((size_t) a.note);
                                B.lastLatched = a.note;
                            }
                        }
                        else
                        {
                            B.pressed.reset ((size_t) a.note);
                        }
                        updateOutput (B, b, a.pos, hold, arp);
                        break;

                    case Kind::Raw:
                    {
                        const auto& rm = inRaw[(size_t) b][(size_t) a.rawIndex];
                        B.out.addEvent (rm, a.pos);
                        if ((B.loopState == loopRec || B.loopState == loopOver) && ! (rm.isController() && rm.getControllerNumber() >= 120))
                        {
                            const auto* d = rm.getRawData();
                            const int sz = rm.getRawDataSize();
                            if (sz >= 2)
                                B.rec.push_back ({ blkBeat0 + (double) a.pos * blkBps, true, (int) d[0], (int) d[1], true, sz > 2 ? (int) d[2] : 0 });
                        }
                        break;
                    }

                    case Kind::ArpStep:
                        arpStep (B, b, a.pos, hold, arp);
                        break;

                    case Kind::GateOff:
                        offAll (B, b, a.pos);
                        break;

                    case Kind::LoopEvt:
                        if (a.rawLoop)
                        {
                            const int st = a.note & 0xF0, ch = a.note & 0x0F;
                            if (st == 0xD0 || st == 0xC0)
                                B.out.addEvent (juce::MidiMessage (a.note, a.vel, 0.0), a.pos);
                            else
                                B.out.addEvent (juce::MidiMessage (a.note, a.vel, a.d2, 0.0), a.pos);
                            if (st == 0xB0 && a.vel == 64) { if (a.d2 >= 64) B.loopSusMask |= 1u << ch; else B.loopSusMask &= ~(1u << ch); }
                            if (st == 0xE0) B.loopBendMask |= 1u << ch;
                            break;
                        }
                        if (a.on)
                        {
                            B.loopOut[(size_t) a.note] = (uint8_t) juce::jlimit (0, 127, a.note + transpose[(size_t) b].load());
                            B.out.addEvent (juce::MidiMessage::noteOn (1, B.loopOut[(size_t) a.note], (juce::uint8) a.vel), a.pos);
                        }
                        else
                        {
                            B.out.addEvent (juce::MidiMessage::noteOff (1, B.loopOut[(size_t) a.note]), a.pos);
                        }
                        B.loopSounding.set ((size_t) a.note, a.on);
                        break;
                }
            }

            loopStateOut[b] = B.loopState;
            loopBeatsOut[b] = (B.loopState == loopPlay || B.loopState == loopOver) ? B.loopBeats : 0.0;
            loopLayersOut[b] = B.nLayers;
            loopMuteOut[b]   = (int) B.muteMask;
            loopTapsOut[b]   = B.taps;
            loopStoppedOut[b] = B.stopped ? 1 : 0;
            recStartBeatOut[b] = B.recStart;

        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Engine)
    };
}
