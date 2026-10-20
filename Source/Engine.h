#pragma once
// Flow engine: 16 blocks, each with its own plugin, key range, hold, arpeggiator and MIDI loop.
// Everything musical runs inside the audio callback, so timing follows the audio clock.

#include <juce_audio_utils/juce_audio_utils.h>
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
    constexpr double tempoBpm = 120.0;  // fixed tempo for arp and loops
    constexpr int axisBars    = 4;      // loops screen shows 4 bars

    enum LoopState { loopOff = 0, loopRec = 1, loopPlay = 2, loopOver = 3 };
    constexpr int maxLayers = 4;
    constexpr int numControls = 9;   // κουμπιά που μπορούν να ανατεθούν σε πετάλι

    // equal split of the 88 keys: the first blocks get one key more when it does not divide evenly
    inline void defaultRange (int block, int& lo, int& hi)
    {
        const int total = highestNote - lowestNote + 1;
        const int base = total / numBlocks, rem = total % numBlocks;
        int start = lowestNote;
        for (int b = 0; b < block; ++b)
            start += base + (b < rem ? 1 : 0);
        lo = start;
        hi = start + base + (block < rem ? 1 : 0) - 1;
    }

    class Engine : public juce::AudioIODeviceCallback
    {
    public:
        Engine()
        {
            for (int b = 0; b < numBlocks; ++b)
            {
                int l = 0, h = 0;
                defaultRange (b, l, h);
                lo[b] = l;
                hi[b] = h;
                holdMode[b] = 0;
                arpMode[b] = 0;
                loopStateOut[b] = loopOff;
                loopBarsOut[b] = 0;
                recStartOut[b] = 0;
                loopLayersOut[b] = 0;
                loopMuteOut[b] = 0;
                loopTapsOut[b] = 0;
                cmdMain[b] = 0; cmdUndo[b] = 0; cmdStop[b] = 0;
            }
            for (int i = 0; i < numControls; ++i) { mapCode[i] = 0; mapHits[i] = 0; }
            owner.fill (-1);
            incoming.reset (44100.0);
        }

        //==================== controls and read-outs (message thread) ====================
        std::array<std::atomic<int>, numBlocks> lo, hi, holdMode, arpMode, loopStateOut, loopBarsOut,
                                                loopLayersOut, loopMuteOut, loopTapsOut, cmdMain, cmdUndo, cmdStop;
        std::array<std::atomic<int>, numControls> mapCode, mapHits;   // MIDI learn πεταλιών
        std::atomic<int> mapLearnTarget { -1 }, learnedCount { 0 }, learnedTarget { -1 }, learnedCode { 0 };
        std::array<std::atomic<int64_t>, numBlocks> recStartOut;
        std::atomic<int64_t> globalSamples { 0 };
        std::atomic<int> learnBlock { -1 };
        std::atomic<int> learnStage { 0 };    // 0 = off, 1 = waiting for the low key, 2 = waiting for the high key

        juce::MidiMessageCollector incoming;  // register this as the MIDI input callback

        // εντολές looper για ένα block (από κουμπί ή πετάλι)
        void pressMain (int b) { ++cmdMain[(size_t) b]; }
        void pressUndo (int b) { ++cmdUndo[(size_t) b]; }
        void pressStop (int b) { ++cmdStop[(size_t) b]; }

        void startLearn (int block) { learnBlock = block; learnStage = 1; }
        void cancelLearn()          { learnBlock = -1;   learnStage = 0; }

        double getSampleRate() const { return sampleRate.load(); }
        int64_t barSamples() const   { return (int64_t) std::llround (sampleRate.load() * 60.0 / tempoBpm * 4.0); }
        int64_t axisSamples() const  { return barSamples() * axisBars; }

        juce::AudioPluginInstance* getPlugin (int b) { return blocks[(size_t) b].plugin.get(); }

        // Puts a new plugin (or nullptr) into a block and returns the old one.
        std::unique_ptr<juce::AudioPluginInstance> swapPlugin (int b, std::unique_ptr<juce::AudioPluginInstance> fresh)
        {
            if (fresh != nullptr)
                prepare (*fresh);

            std::unique_ptr<juce::AudioPluginInstance> old;
            {
                const juce::ScopedLock sl (lock);
                auto& B = blocks[(size_t) b];
                old = std::move (B.plugin);
                B.plugin = std::move (fresh);
                B.resetState();
                if (B.plugin != nullptr)
                    B.scratch.setSize (juce::jmax (1, juce::jmax (B.plugin->getTotalNumInputChannels(),
                                                                    B.plugin->getTotalNumOutputChannels())),
                                       juce::jmax (blockSize, 4096));
                for (auto& o : owner)
                    if (o == b) o = -1;
                B.seenMain = cmdMain[(size_t) b].load();
                B.seenUndo = cmdUndo[(size_t) b].load();
                B.seenStop = cmdStop[(size_t) b].load();
                loopStateOut[b] = loopOff;
                loopBarsOut[b] = 0;
                loopLayersOut[b] = 0;
                loopMuteOut[b] = 0;
                loopTapsOut[b] = 0;
            }
            return old;
        }

        //==================== audio ====================
        void audioDeviceAboutToStart (juce::AudioIODevice* device) override
        {
            sampleRate = device->getCurrentSampleRate();
            blockSize  = device->getCurrentBufferSizeSamples();
            incoming.reset (sampleRate.load());

            const juce::ScopedLock sl (lock);
            for (auto& B : blocks)
            {
                if (B.plugin == nullptr) continue;
                B.plugin->releaseResources();
                prepare (*B.plugin);
                B.scratch.setSize (juce::jmax (1, juce::jmax (B.plugin->getTotalNumInputChannels(),
                                                                B.plugin->getTotalNumOutputChannels())),
                                   juce::jmax (blockSize, 4096));
            }
        }

        void audioDeviceStopped() override
        {
            const juce::ScopedLock sl (lock);
            for (auto& B : blocks)
                if (B.plugin != nullptr)
                    B.plugin->releaseResources();
        }

        void audioDeviceIOCallbackWithContext (const float* const*, int, float* const* outputChannelData,
                                               int numOutputChannels, int numSamples,
                                               const juce::AudioIODeviceCallbackContext&) override
        {
            juce::ScopedNoDenormals noDenormals;

            for (int c = 0; c < numOutputChannels; ++c)
                juce::FloatVectorOperations::clear (outputChannelData[c], numSamples);

            const juce::ScopedLock sl (lock);

            const double sr = sampleRate.load();
            const int64_t gs = globalPos;

            dispatchIncoming (numSamples);

            for (int b = 0; b < numBlocks; ++b)
            {
                auto& B = blocks[(size_t) b];
                if (B.plugin == nullptr)
                {
                    B.seenMain = cmdMain[(size_t) b].load();
                    B.seenUndo = cmdUndo[(size_t) b].load();
                    B.seenStop = cmdStop[(size_t) b].load();
                    continue;
                }

                runBlock (b, gs, numSamples, sr);

                if (B.scratch.getNumSamples() < numSamples)
                    B.scratch.setSize (B.scratch.getNumChannels(), numSamples, false, false, true);

                B.scratch.clear();
                juce::AudioBuffer<float> view (B.scratch.getArrayOfWritePointers(), B.scratch.getNumChannels(), numSamples);
                B.plugin->processBlock (view, B.out);

                const int chans = view.getNumChannels();
                for (int c = 0; c < numOutputChannels; ++c)
                    juce::FloatVectorOperations::add (outputChannelData[c],
                                                       view.getReadPointer (juce::jmin (c, chans - 1)), numSamples);
            }

            globalPos += numSamples;
            globalSamples = globalPos;
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
        };

        struct LoopEvent { int64_t phase; bool on; int note; int vel; };

        struct Block
        {
            std::unique_ptr<juce::AudioPluginInstance> plugin;
            juce::AudioBuffer<float> scratch;
            juce::MidiBuffer out;
            std::bitset<128> pressed, latched, sounding, loopSounding, recOpen;
            std::array<std::vector<LoopEvent>, maxLayers> layerEv;
            int nLayers = 0;
            unsigned muteMask = 0;
            int seenMain = 0, seenUndo = 0, seenStop = 0, taps = 0;
            int64_t lastTap = 0;
            std::array<uint8_t, 128> vel {};
            int lastLatched = -1;
            int prevHold = 0, prevArp = 0;
            int arpIndex = 0;
            int loopState = loopOff;
            int64_t recStart = 0, loopLen = 0;
            std::vector<LoopEvent> rec;

            void resetState()
            {
                pressed.reset(); latched.reset(); sounding.reset(); loopSounding.reset(); recOpen.reset();
                vel.fill (0);
                lastLatched = -1; prevHold = 0; prevArp = 0; arpIndex = 0;
                loopState = loopOff; recStart = 0; loopLen = 0;
                rec.clear();
                for (auto& l : layerEv) l.clear();
                nLayers = 0; muteMask = 0; taps = 0; lastTap = 0;
                out.clear();
            }
        };

        juce::CriticalSection lock;
        std::array<Block, numBlocks> blocks;
        std::array<int, 128> owner;
        std::array<std::vector<Action>, numBlocks> inActions;
        std::array<std::vector<juce::MidiMessage>, numBlocks> inRaw;
        std::vector<Action> actions;
        juce::MidiBuffer midiIn;
        int learnFirst = 0;
        int64_t globalPos = 0;
        std::atomic<double> sampleRate { 44100.0 };
        int blockSize = 512;

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

        //==================== incoming MIDI -> blocks ====================
        int findBlock (int note) const
        {
            for (int b = 0; b < numBlocks; ++b)
                if (blocks[(size_t) b].plugin != nullptr && note >= lo[b].load() && note <= hi[b].load())
                    return b;
            return -1;
        }

        void dispatchIncoming (int numSamples)
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

                if (handlePedal (msg))
                    continue;

                if (msg.isNoteOn())
                {
                    const int n = msg.getNoteNumber();
                    const int lb = learnBlock.load();

                    if (lb >= 0)
                    {
                        if (learnStage.load() == 1)
                        {
                            learnFirst = n;
                            learnStage = 2;
                        }
                        else
                        {
                            lo[lb] = std::min (learnFirst, n);
                            hi[lb] = std::max (learnFirst, n);
                            learnBlock = -1;
                            learnStage = 0;
                        }
                        continue;
                    }

                    const int b = findBlock (n);
                    if (b >= 0)
                    {
                        owner[(size_t) n] = b;
                        Action a;
                        a.pos = pos; a.order = 1; a.kind = Kind::Live; a.note = n; a.vel = msg.getVelocity(); a.on = true;
                        inActions[(size_t) b].push_back (a);
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
                        if (blocks[(size_t) b].plugin != nullptr)
                            pushRaw (b, pos, msg);
                }
            }
        }

        // πετάλια: επιστρέφει true αν το μήνυμα το κατανάλωσε η ανάθεση (δεν πάει στα plugins)
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
            const int target = mapLearnTarget.load();

            if (target >= 0 && press)
            {
                for (int i = 0; i < numControls; ++i)
                    if (mapCode[i].load() == code) mapCode[i] = 0;

                mapCode[target] = code;
                learnedTarget = target;
                learnedCode = code;
                ++learnedCount;
                mapLearnTarget = -1;
                return true;
            }

            for (int i = 0; i < numControls; ++i)
            {
                if (mapCode[i].load() == code)
                {
                    if (press) ++mapHits[i];
                    return true;
                }
            }

            return false;
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
                if (s[(size_t) n]) return n;
            return -1;
        }

        void emitNote (Block& B, int pos, bool on, int note, int vel, int64_t gs)
        {
            B.out.addEvent (on ? juce::MidiMessage::noteOn (1, note, (juce::uint8) vel)
                               : juce::MidiMessage::noteOff (1, note), pos);
            B.sounding.set ((size_t) note, on);

            if (B.loopState == loopRec || B.loopState == loopOver)
            {
                B.rec.push_back ({ gs + pos - B.recStart, on, note, vel });
                B.recOpen.set ((size_t) note, on);
            }
        }

        void offAll (Block& B, int pos, int64_t gs)
        {
            for (int n = 0; n < 128; ++n)
                if (B.sounding[(size_t) n])
                    emitNote (B, pos, false, n, 0, gs);
        }

        // brings the sounding notes in line with what should sound now
        void updateOutput (Block& B, int pos, int64_t gs, int hold, int arp)
        {
            const auto& target = hold == 0 ? B.pressed : B.latched;

            if (arp == 0)
            {
                for (int n = 0; n < 128; ++n)
                    if (B.sounding[(size_t) n] && ! target[(size_t) n])
                        emitNote (B, pos, false, n, 0, gs);

                for (int n = 0; n < 128; ++n)
                    if (target[(size_t) n] && ! B.sounding[(size_t) n])
                        emitNote (B, pos, true, n, B.vel[(size_t) n], gs);
            }
            else if (target.none())
            {
                offAll (B, pos, gs);
                B.arpIndex = 0;
            }
        }

        void arpStep (Block& B, int pos, int64_t gs, int hold, int arp)
        {
            const auto& target = hold == 0 ? B.pressed : B.latched;
            if (target.none()) return;

            offAll (B, pos, gs);

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
                        emitNote (B, pos, true, n, B.vel[(size_t) n], gs);
                        break;
                    }
                }
            }
            else            // poly: all held notes together on every step
            {
                for (int n = 0; n < 128; ++n)
                    if (target[(size_t) n])
                        emitNote (B, pos, true, n, B.vel[(size_t) n], gs);
            }
        }

        void cutLoopNotes (Block& B)
        {
            for (int n = 0; n < 128; ++n)
                if (B.loopSounding[(size_t) n])
                    B.out.addEvent (juce::MidiMessage::noteOff (1, n), 0);

            B.loopSounding.reset();
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
            B.loopLen = 0;
            B.loopState = loopOff;
        }

        // κλείνει το layer που ηχογραφείται (rec) και το προσθέτει στο loop. Επιστρέφει true αν είχε νότες.
        bool commitLayer (Block& B, int64_t gs)
        {
            const int64_t elapsed = std::max<int64_t> (1, std::min (B.loopLen, gs - B.recStart));

            for (int n = 0; n < 128; ++n)    // νότες που κρατιούνται όταν τελειώνει η ηχογράφηση σταματούν στο τέλος
                if (B.recOpen[(size_t) n])
                    B.rec.push_back ({ elapsed - 1, false, n, 0 });

            auto& dst = B.layerEv[(size_t) B.nLayers];
            dst.clear();
            for (auto e : B.rec)
            {
                if (e.phase < 0 || e.phase >= B.loopLen) continue;
                e.phase = (B.recStart + e.phase) % B.loopLen;
                dst.push_back (e);
            }

            B.rec.clear();
            B.recOpen.reset();

            if (dst.empty()) return false;
            ++B.nLayers;
            return true;
        }

        void finishRecording (Block& B, int64_t gs, int64_t barLen)
        {
            const int64_t elapsed = std::max<int64_t> (1, gs - B.recStart);
            const int64_t barsUsed = (elapsed + barLen - 1) / barLen;
            const int bars = barsUsed <= 1 ? 1 : (barsUsed <= 2 ? 2 : 4);
            B.loopLen = (int64_t) bars * barLen;

            if (commitLayer (B, gs)) B.loopState = loopPlay;
            else                     stopLoop (B);
        }

        void finishOverdub (Block& B, int64_t gs)
        {
            commitLayer (B, gs);
            B.loopState = loopPlay;
        }

        void loopMain (Block& B, int64_t gs, int64_t barLen)
        {
            switch (B.loopState)
            {
                case loopOff:
                    B.recStart = (gs / barLen) * barLen;
                    B.rec.clear();
                    B.recOpen.reset();
                    B.loopState = loopRec;
                    break;
                case loopRec:  finishRecording (B, gs, barLen); break;
                case loopPlay:
                    if (B.nLayers < maxLayers)
                    {
                        B.recStart = gs;
                        B.rec.clear();
                        B.recOpen.reset();
                        B.loopState = loopOver;
                    }
                    break;
                case loopOver: finishOverdub (B, gs); break;
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
                cutLoopNotes (B);
                if (B.nLayers == 0) stopLoop (B);
            }
        }

        void runBlock (int b, int64_t gs, int numSamples, double sr)
        {
            Block& B = blocks[(size_t) b];
            B.out.clear();

            const int64_t ge = gs + numSamples;
            const int64_t barLen  = std::max<int64_t> (1, (int64_t) std::llround (sr * 60.0 / tempoBpm * 4.0));
            const int64_t stepLen = std::max<int64_t> (1, (int64_t) std::llround (sr * 60.0 / tempoBpm / 4.0));
            const int64_t gateLen = (int64_t) ((double) stepLen * 0.8);

            const int hold = holdMode[b].load();
            const int arp  = arpMode[b].load();

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
                offAll (B, 0, gs);
                B.arpIndex = 0;
                changed = true;
            }

            B.prevHold = hold;
            B.prevArp = arp;
            if (changed) updateOutput (B, 0, gs, hold, arp);

            // ---- loop: εντολές (κουμπιά ή πετάλια)
            const int64_t tapWindow = (int64_t) (sr * 0.7);

            for (int cm = cmdMain[(size_t) b].load(); B.seenMain != cm; )
            {
                ++B.seenMain;
                loopMain (B, gs, barLen);
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
                // N πατήματα STOP = σίγαση / επαναφορά του layer N
                const int layer = B.taps - 1;
                if (B.loopState == loopPlay && layer < B.nLayers)
                {
                    B.muteMask ^= (1u << layer);
                    cutLoopNotes (B);
                }
                B.taps = 0;
            }

            if (B.loopState == loopRec && (gs - B.recStart) >= (int64_t) axisBars * barLen)
                finishRecording (B, gs, barLen);
            else if (B.loopState == loopOver && (gs - B.recStart) >= B.loopLen)
                finishOverdub (B, gs);

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

            if ((B.loopState == loopPlay || B.loopState == loopOver) && B.loopLen > 0)
            {
                for (int layer = 0; layer < B.nLayers; ++layer)
                {
                    if ((B.muteMask >> layer) & 1u) continue;

                    for (const auto& e : B.layerEv[(size_t) layer])
                    {
                        for (int64_t m = gs / B.loopLen; m <= (ge - 1) / B.loopLen; ++m)
                        {
                            const int64_t p = m * B.loopLen + e.phase;
                            if (p < gs || p >= ge) continue;
                            Action a;
                            a.pos = (int) (p - gs); a.order = 3; a.kind = Kind::LoopEvt;
                            a.note = e.note; a.vel = e.vel; a.on = e.on;
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
                            B.pressed.set ((size_t) a.note);
                            B.vel[(size_t) a.note] = (uint8_t) a.vel;

                            if (hold == 2)
                            {
                                B.latched.flip ((size_t) a.note);
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
                        updateOutput (B, a.pos, gs, hold, arp);
                        break;

                    case Kind::Raw:
                        B.out.addEvent (inRaw[(size_t) b][(size_t) a.rawIndex], a.pos);
                        break;

                    case Kind::ArpStep:
                        arpStep (B, a.pos, gs, hold, arp);
                        break;

                    case Kind::GateOff:
                        offAll (B, a.pos, gs);
                        break;

                    case Kind::LoopEvt:
                        B.out.addEvent (a.on ? juce::MidiMessage::noteOn (1, a.note, (juce::uint8) a.vel)
                                             : juce::MidiMessage::noteOff (1, a.note), a.pos);
                        B.loopSounding.set ((size_t) a.note, a.on);
                        break;
                }
            }

            loopStateOut[b] = B.loopState;
            loopBarsOut[b]  = (B.loopState == loopPlay || B.loopState == loopOver) ? (int) (B.loopLen / barLen) : 0;
            loopLayersOut[b] = B.nLayers;
            loopMuteOut[b]   = (int) B.muteMask;
            loopTapsOut[b]   = B.taps;
            recStartOut[b]  = B.recStart;
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Engine)
    };
}
