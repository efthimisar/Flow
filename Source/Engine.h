#pragma once
#include <map>
// Flow engine: μέχρι 9 blocks (ένα ανά instrument), καθένα με δική του ζώνη, hold, ένταση, transpose, MIDI loop,
// MIDI FX και αλυσίδα FX. Στο τέλος: master FX, metronome, Rec Audio με stems.
// Όλη η μουσική λογική τρέχει μέσα στο audio callback, άρα ο χρόνος ακολουθεί το ρολόι του ήχου.

#include <juce_audio_utils/juce_audio_utils.h>
#include "SmartCore.h"
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
    constexpr int numBlocks   = 9;
    constexpr int lowestNote  = 21;     // A0
    constexpr int highestNote = 108;    // C8
    constexpr int tapKeyNote    = 21;   // A0: tap tempo, δεν παίζει ήχο
    constexpr int recAudioKeyNote = 22; // A#0 (μαύρο): Rec Audio (σταματά ή νέο take), δεν παίζει ήχο
    constexpr int followKeyNote = 23;   // B0: ενεργοποιεί/σβήνει το Follow, δεν παίζει ήχο
    constexpr int firstPlayNote = 24;   // C1: το πρώτο πλήκτρο που παίζει ήχο
    inline bool isControlKey (int n) { return n >= lowestNote && n < firstPlayNote; }
    // καταγραφή βημάτων σε αρχείο (Music/Flow/Flow-log.txt) για να φαίνεται πού σταματά ένα crash
    inline void flowLog (const juce::String& s)
    {
        auto dir = juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("Flow");
        dir.createDirectory();
        dir.getChildFile ("Flow-log.txt").appendText (juce::Time::getCurrentTime().formatted ("%H:%M:%S ") + s + "\n");
    }

    constexpr int maxLoopBars = 16;     // μέγιστο μήκος MIDI loop
    constexpr double beatsPerBar = 4.0;

    enum LoopState { loopOff = 0, loopRec = 1, loopPlay = 2, loopOver = 3 };
    constexpr int maxLayers = 32;
    constexpr int maxOverdubLayers = 3;   // όριο: 3 στρώματα ανά λούπα (το 1ο + 2 overdub), το καθένα με δικό του μήκος και τρόπο επανάληψης
    // κουμπιά που ανατίθενται σε πετάλι ή πλήκτρο (MIDI learn)
    enum Control { ctlHold = 0, ctlRec, ctlUndo, ctlStop, ctlTape, ctlMetro, ctlPanic, ctlScenePrev, ctlSceneNext, ctlMute, ctlSpecial, numControls };

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
        std::vector<int> layerMode;       // τρόπος επανάληψης κάθε layer: 0 loop, 1 once, 2 hold at end
        double beats = 0.0;
        double anchor = 0.0;              // αρχή της λούπας (beat mod μήκος): από εκεί ξαναρχίζει όταν τελειώνει
        std::vector<double> layerBeats;   // μήκος κάθε layer (το loop είναι το μεγαλύτερο, τα άλλα επαναλαμβάνονται)
        unsigned mute = 0;
        std::vector<std::vector<LoopEvent>> layers;
    };

    using PluginPtr = std::unique_ptr<juce::AudioPluginInstance>;

    // Smart master: αρχικά όρια ανά τύπο ήχου {Low, Mid, High, πλάτος}, κλίμακα 0..1 (στάθμη: -60..0 dB RMS).
    // Είναι σύμβαση μίξης, όχι τεκμηριωμένα ευρήματα: ρυθμίζονται με το αυτί. 1.0 = χωρίς όριο.
    // 6 ζώνες: Sub <60, Low 60-200, Low-Mid 200-500, Mid 500-2k, High-Mid 2k-6k, High >6k (Hz)
    constexpr int smartBands = 6;
    constexpr float smartCross[5] = { 60.0f, 200.0f, 500.0f, 2000.0f, 6000.0f };
    inline float smartMaxDb (int band) { return band < 2 ? 12.0f : 6.0f; }   // μέγιστη μείωση ανά ζώνη

    // όρια ανά τύπο ήχου: out[0..5] ζώνες, out[6] πλάτος. Αρχικές τιμές από τα παλιά 3 όρια (Low, Mid, High):
    // σύμβαση μίξης, όχι τεκμηριωμένα ευρήματα. Ρυθμίζονται με το αυτί. 1.0 = χωρίς όριο.
    inline void smartLimits (int type, float* out)
    {
        static const float t[9][4] = { { 1.0f, 1.0f, 1.0f, 1.0f },     // Other
                                       { 0.85f, 0.60f, 0.35f, 0.20f }, // Bass
                                       { 0.80f, 0.85f, 0.70f, 0.60f }, // Keys
                                       { 0.60f, 0.75f, 0.60f, 1.00f }, // Pad
                                       { 0.60f, 0.85f, 0.80f, 0.70f }, // Lead
                                       { 0.70f, 0.85f, 0.75f, 0.90f },  // Strings
                                       { 0.90f, 0.80f, 0.85f, 0.80f },  // Drums
                                       { 0.50f, 0.80f, 0.80f, 1.00f },   // FX
                                       { 0.50f, 0.85f, 0.85f, 0.80f } }; // Arp
        const int i = type < 0 || type > 8 ? 0 : type;
        const float lo = t[i][0], mi = t[i][1], hi = t[i][2];
        out[0] = lo < 0.999f ? lo * 0.85f : 1.0f;
        out[1] = lo;
        out[2] = mi;
        out[3] = mi;
        out[4] = hi;
        out[5] = hi < 0.999f ? hi * 0.9f : 1.0f;
        out[6] = t[i][3];
    }

    class Engine : public juce::AudioIODeviceCallback
    {
    public:
        Engine()
        {
            for (int b = 0; b < numBlocks; ++b)
            {
                active[b] = 0; emptyZone[b] = 0; zoneState[b] = 0; zoneFirst[b] = 0; zoneSeq[b] = 0;
                lo[b] = firstPlayNote; hi[b] = firstPlayNote;
                holdMode[b] = 0; latchBits[(size_t) b * 2] = 0; latchBits[(size_t) b * 2 + 1] = 0; swBits[(size_t) b * 2] = 0; swBits[(size_t) b * 2 + 1] = 0; autoReadyOut[(size_t) b] = 0; autoPausedOut[(size_t) b] = 0;
                liveSilent[b] = 1;
                blockOff[b] = 0; chordMode[b] = 0; chordType[b] = 0; chordMinor[b] = 0; chordTonic[b] = -1; pcMaskOut[b] = 0; bassOut[b] = -1;
                arpMode[b] = 0;
                curveMode[b] = 0;
                transpose[b] = 0;
                volPos[b] = 1.0f; gainOut[b] = 1.0f;
                volMap[b] = 0;
                if (b < numPedals) pedalCc[(size_t) b] = 20 + b;
                followBlock[b] = 1; followPolyBlock[b] = 0; repeatMode[b] = 0; asMode[b] = 0; asPct[b] = 60; asLo[b] = firstPlayNote; asHi[b] = firstPlayNote;
                smartType[b] = 0; smartWidth[b] = 0.0f; smartNeedWidth[b] = 1.0f;
                smartAutoT[b] = 0; for (int q = 0; q < 4; ++q) smartFeat[b][(size_t) q] = 0.0f;
                for (int k = 0; k < smartBands; ++k) { smartBand[b][(size_t) k] = 0.0f; smartNeed[b][(size_t) k] = 0.0f; }
                smartResetApp ((int) b);
                loopStateOut[b] = loopOff;
                loopBeatsOut[b] = 0.0;
                loopAnchorOut[b] = 0.0;
                recStartBeatOut[b] = 0.0;
                loopLayersOut[b] = 0;
                loopMuteOut[b] = 0;
                loopTapsOut[b] = 0;
                cmdMain[b] = 0; cmdUndo[b] = 0; cmdStop[b] = 0; cmdMute[b] = 0; rerecReq[b] = 0; loopStoppedOut[b] = 0;
            }
            for (int i = 0; i < numControls; ++i) mapCode[i] = 0;
            owner.fill (-1);
            ownerSp.fill (-1);
            incoming.reset (44100.0);
        }

        //==================== controls and read-outs (message thread) ====================
        std::array<std::atomic<uint64_t>, numBlocks * 2> swBits;      // AL: πλήκτρα που είναι key switch (πορτοκαλί), για την οθόνη
        std::array<std::atomic<uint64_t>, 2> pressBits {};              // πλήκτρα που πατιούνται τώρα (φυσικά), για την οθόνη
        std::array<std::atomic<uint64_t>, numBlocks * 2> latchBits;   // πλήκτρα που κρατιούνται από το Latch (και νότες συγχορδίας), για την οθόνη
        std::array<std::atomic<int>, numBlocks> active, emptyZone, zoneState, zoneFirst, zoneSeq, lo, hi, holdMode, arpMode, curveMode, transpose, blockOff, chordMode, chordType, chordMinor, chordTonic, pcMaskOut, bassOut,
                                                volMap, loopStateOut, loopLayersOut, loopMuteOut, loopTapsOut,
                                                cmdMain, cmdUndo, cmdStop, cmdMute, rerecReq, loopStoppedOut;
        std::array<std::atomic<double>, numBlocks> loopBeatsOut, recStartBeatOut, loopAnchorOut;
        std::atomic<double> cycleAnchorOut { 0.0 };   // αρχή του κύκλου (της μεγαλύτερης λούπας) για την οθόνη
        std::array<std::atomic<float>, numBlocks> volPos, gainOut;
        std::array<std::atomic<int>, numControls> mapCode;   // MIDI learn πεταλιών / πλήκτρων
        std::atomic<int> mapLearnTarget { -1 }, learnedCount { 0 }, learnedTarget { -1 }, learnedCode { 0 };
        std::atomic<int> volLearn { -1 }, volLearnedCount { 0 }, volLearnedBlock { -1 }, volLearnedCode { 0 };
        // 6 πετάλια έντασης (standard): πετάλι N ελέγχει πάντα το block N. Αριθμός CC (οποιοδήποτε κανάλι)· -1 = δεν έχει οριστεί. Προεπιλογή CC 20..25.
        static constexpr int numPedals = 6;
        std::array<std::atomic<int>, numPedals> pedalCc {};
        std::atomic<int> pedalLearn { -1 }, pedalLearnedCount { 0 }, pedalLearnedSlot { -1 }, pedalLearnedNum { 0 };
        // 8 φυσικά ποτενσιόμετρα (CC): κωδικός ανά θέση (0 = καμία ανάθεση), τιμή που περιμένει το UI (-1 = τίποτα), learn
        std::array<std::atomic<int>, 8> knobCc {};
        std::array<std::atomic<float>, 8> knobIn {};
        std::atomic<int> knobLearnSlot { -1 }, knobLearnedSlot { -1 };
        // master: ένταση (θέση 0..1, κέρδος = θέση²), ασφαλιστικός limiter, CC για ένταση
        std::atomic<float> masterPos { 1.0f };
        std::atomic<int> limiterOn { 0 }, masterCc { 0 }, masterLearn { 0 }, masterLearned { 0 };
        // Follow: οι λούπες των block που το έχουν ανάψει ακολουθούν τη συγχορδία που παίζεις (μεταφορά με κλίμακα)
        std::atomic<int> followOn { 0 };                                  // γενικός διακόπτης (πλήκτρο B7 / οθόνη Follow)
        std::atomic<int> followRoot { 9 }, followType { 1 };             // κλίμακα των λουπών που ηχογράφησες: ρίζα 0..11, τύπος 0..5
        std::atomic<int> followTRoot { 9 }, followTType { 1 };
        // Πολυφωνική μετάθεση: κάθε νότα που κρατάς (έως 3, οι χαμηλότερες) παίζει τη λούπα μεταφερμένη στη δική της ρίζα, με μειωμένη ένταση (1/√n)
        static constexpr int maxPolyVoices = 3;
        std::atomic<int> followVoiceN { 0 };
        std::array<std::atomic<int>, maxPolyVoices> followVoiceRoot {};
        std::atomic<int> followHomeRoot { 9 }, followHomeType { 1 }, followHomeSet { 0 };   // τελευταία συγχορδία με σβηστό Follow (τονικότητα πηγής)           // κλίμακα-στόχος (από τη συγχορδία)
        // Αυτόματη εύρεση κλίμακας από ό,τι παίζεις ζωντανά (με σβηστό Follow): ιστόγραμμα 12 νοτών με εκθετική απομείωση,
        // σύγκριση (συσχέτιση Pearson) με τα 24 προφίλ Krumhansl–Kessler (Krumhansl–Schmuckler) και υστέρηση. Μόνο major / minor.
        // [ΧΡΕΙΑΖΕΤΑΙ ΠΗΓΗ] Οι τιμές των προφίλ είναι από δευτερογενή σελίδα· να ελεγχθούν με Krumhansl (1990).
        std::atomic<int> followAuto { 1 };                               // 1 = η κλίμακα βρίσκεται αυτόματα, 0 = χειροκίνητη
        std::atomic<int> followAutoFound { 0 };                          // 1 = έχει βρεθεί κλίμακα αυτόματα
        std::array<float, 12> keyHist {};
        double keyHistT = 0.0;
        float keyHistN = 0.0f;
        uint64_t prevPressLo = 0, prevPressHi = 0;
        static constexpr float keyHalfLifeSec = 12.0f;                   // ρύθμιση, όχι από μελέτη
        static constexpr float keyMinNotes = 8.0f;                       // κάτω από αυτό δεν αποφασίζει
        static constexpr float keySwitchMargin = 0.05f;                  // υστέρηση: η νέα κλίμακα πρέπει να κερδίζει κατά τόσο (συσχέτιση)
        static float keyCorr (const std::array<float, 12>& h, const float* prof, int root)
        {
            float mh = 0.0f, mp = 0.0f;
            for (int i = 0; i < 12; ++i) { mh += h[(size_t) i]; mp += prof[i]; }
            mh /= 12.0f; mp /= 12.0f;
            float sxy = 0.0f, sxx = 0.0f, syy = 0.0f;
            for (int i = 0; i < 12; ++i)
            {
                const float x = h[(size_t) ((root + i) % 12)] - mh, y = prof[i] - mp;
                sxy += x * y; sxx += x * x; syy += y * y;
            }
            return (sxx > 1.0e-9f && syy > 1.0e-9f) ? sxy / std::sqrt (sxx * syy) : 0.0f;
        }
        void keyAutoUpdate()
        {
            static const float maj[12] = { 6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f, 2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f };
            static const float mnr[12] = { 6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f, 2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f };
            const uint64_t lo = pressBits[0].load(), hi = pressBits[1].load();
            const uint64_t nlo = lo & ~prevPressLo, nhi = hi & ~prevPressHi;
            prevPressLo = lo; prevPressHi = hi;
            if (followOn.load() != 0 || followAuto.load() == 0) return;     // μαθαίνει μόνο με σβηστό Follow και αυτόματη κλίμακα
            if (nlo == 0 && nhi == 0) return;
            const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
            if (keyHistT > 0.0)
            {
                const float f = std::pow (0.5f, (float) (now - keyHistT) / keyHalfLifeSec);
                for (auto& v : keyHist) v *= f;
                keyHistN *= f;
            }
            keyHistT = now;
            for (int n = 0; n < 128; ++n)
            {
                const bool nw = n < 64 ? ((nlo >> n) & 1u) != 0 : ((nhi >> (n - 64)) & 1u) != 0;
                if (nw && (mfBlock.load() < 0 || noteBelongsToMf (n))) { keyHist[(size_t) (n % 12)] += 1.0f; keyHistN += 1.0f; }
            }
            if (keyHistN < keyMinNotes) return;
            float best = -2.0f; int bR = 0, bT = 0;
            for (int r = 0; r < 12; ++r)
                for (int t = 0; t < 2; ++t)
                {
                    const float c = keyCorr (keyHist, t == 0 ? maj : mnr, r);
                    if (c > best) { best = c; bR = r; bT = t; }
                }
            const int curT = followType.load();
            const float cur = followAutoFound.load() != 0 ? keyCorr (keyHist, curT == 0 ? maj : mnr, followRoot.load()) : -2.0f;
            if (bR == followRoot.load() && bT == curT && followAutoFound.load() != 0) return;
            if (cur > -2.0f && best < cur + keySwitchMargin) return;
            followSetScale (bR, bT);
            followAutoFound = 1;
            ++changeCount;
        }
        std::array<std::atomic<int>, numBlocks> followBlock;             // 1 = το block ακολουθεί
        std::array<std::atomic<int>, numBlocks> repeatMode;               // τρόπος επανάληψης του ΕΠΟΜΕΝΟΥ layer που θα ηχογραφηθεί: 0 loop, 1 once (single shot), 2 hold at end
        std::atomic<int> chordGen { 0 };                                  // αυξάνεται όταν αλλάζει η συγχορδία (νέο πάτημα με άλλη ρίζα ή τύπο): ξαναπαίζουν τα hold-at-end
        int lastChordKey = -1;
        std::array<std::atomic<int>, numBlocks> followPolyBlock;         // 1 = το block ακολουθεί πολυφωνικά (μόνο με followBlock = 1)
        // AS (Assistant): το block γίνεται δεύτερος παίκτης. 0 = off, 1 = rules, 2 = style (δεύτερο build)
        // Η ζώνη του φυλάσσεται (asLo/asHi) και επανέρχεται όταν κλείσει. Δεν ακούει τα πλήκτρα του παίκτη.
        std::array<std::atomic<int>, numBlocks> asMode, asPct, asLo, asHi;
        bool isAs (int b) const { return b >= 0 && b < numBlocks && asMode[(size_t) b].load() != 0; }
        void setAsMode (int b, int m)
        {
            if (b < 0 || b >= numBlocks) return;
            m = juce::jlimit (0, 3, m);   // 1 rules, 2 style (Markov), 3 orch (συνοδεία συγχορδιών)
            const int old = asMode[(size_t) b].load();
            if (m == old) return;
            if (old == 0 && m != 0) { asLo[(size_t) b] = lo[(size_t) b].load(); asHi[(size_t) b] = hi[(size_t) b].load(); }
            if (old != 0 && m == 0) { lo[(size_t) b] = asLo[(size_t) b].load(); hi[(size_t) b] = asHi[(size_t) b].load(); }
            asMode[(size_t) b] = m;
            asClear = true;
            bool any = false; for (int i = 0; i < numBlocks; ++i) if (i == b ? m != 0 : asMode[(size_t) i].load() != 0) any = true;
            if (! any) asClearModel = true;   // κανένας βοηθός: το μοντέλο σβήνει
            ++changeCount;
        }
        void setAsPct (int b, int pct) { if (b >= 0 && b < numBlocks) asPct[(size_t) b] = juce::jlimit (0, 100, pct); }
        bool anyFollowPoly() const { for (int i = 0; i < numBlocks; ++i) if (followBlock[(size_t) i].load() != 0 && followPolyBlock[(size_t) i].load() != 0) return true; return false; }
        // Follow on/off (πλήκτρο B0 ή οθόνη): όταν ανάβει, η τελευταία συγχορδία που κράτησες με σβηστό Follow γίνεται η τονικότητα πηγής
        void toggleFollow() { setFollow (followOn.load() != 0 ? 0 : 1); }
        // Το κράτημα (Hold) μένει ενεργό: οι κρατημένες νότες μεταφέρονται στη νέα κλίμακα όταν αλλάζει η συγχορδία (βλ. followRetune)
        void setFollow (int nowOn)
        {
            if (nowOn == (followOn.load() != 0 ? 1 : 0)) return;
            if (nowOn != 0 && followHomeSet.load() != 0 && ! (followAuto.load() != 0 && followAutoFound.load() != 0))
                followSetScale (followHomeRoot.load(), followHomeType.load());
            followOn = nowOn;
            ++changeCount;
        }
        // μόνο Major (0) και Minor (1): οι παλιές κλίμακες γίνονται η πιο κοντινή (Dorian, Phrygian → Minor, Mixolydian, Lydian → Major)
        static int followTypeNorm (int t) { return juce::jlimit (0, 1, t); }   // 0 Major, 1 Minor (παλιά αποθηκευμένη Harmonic minor γίνεται Minor)
        void followSetScale (int root, int type)
        {
            followRoot = juce::jlimit (0, 11, root); followType = followTypeNorm (type);
            followTRoot = followRoot.load(); followTType = followType.load();
        }
        // Smart master: μέτρηση ανά block (3 ζώνες + πλάτος stereo) και διόρθωση με βάση τον τύπο ήχου
        std::atomic<int> smartContext { 1 };                       // Smart: η διόρθωση εξαρτάται από τα άλλα blocks (SmartCore.h). Η ένταση του εφέ = «accept» (0..100%)
        std::atomic<int> smartOn { 0 }, smartAuto { 1 };            // On/Off. Η διόρθωση είναι πάντα Auto (το smartAuto δεν χρησιμοποιείται πια)
        std::atomic<float> smartAccept { 0.6f };                    // πόσο από την αναγκαία διόρθωση εφαρμόζεται (0..1)
        std::array<std::atomic<int>, numBlocks> smartAutoT;         // τύπος που βρήκε το Smart μόνο του (όταν ο τύπος είναι «Smart»=0): 0 = δεν είναι σίγουρο
        std::array<std::array<std::atomic<float>, 4>, numBlocks> smartFeat;   // χαρακτηριστικά που χρησιμοποιήθηκαν: χαμηλά, μεσαία, υψηλά, dbStd
        std::array<std::atomic<int>, numBlocks> smartType;          // 0 Smart (αυτόματος),  1 Bass, 2 Keys, 3 Pad, 4 Lead, 5 Strings, 6 Drums, 7 FX
        std::array<std::array<std::atomic<float>, smartBands>, numBlocks> smartBand, smartNeed, smartApp;   // ανά ζώνη: μέτρηση 0..1 (πριν τη διόρθωση), αναγκαία και εφαρμοσμένη μείωση (dB)
        std::array<std::atomic<float>, numBlocks> smartWidth, smartNeedWidth, smartAppWidth;                // πλάτος 0..1, αναγκαία και εφαρμοσμένη κλίμακα πλάτους
        void smartResetApp (int b)
        {
            for (int k = 0; k < smartBands; ++k) smartApp[(size_t) b][(size_t) k] = 0.0f;
            smartAppWidth[(size_t) b] = 1.0f;
            smartAutoT[(size_t) b] = 0;
            auto& B = blocks[(size_t) b];
            for (int k = 0; k < 6; ++k) B.smLong[k] = 0.0f;
            B.smDbMean = -60.0f; B.smDbVar = 0.0f; B.smSoundSec = 0.0f;
        }
        int smartEffType (int b) const { const int t = smartType[(size_t) b].load(); return t != 0 ? t : smartAutoT[(size_t) b].load(); }
        std::atomic<int> sceneRequest { -1 };   // από πετάλι: -2 = προηγούμενη, -3 = επόμενη (το διαβάζει το UI)
        std::atomic<int> selectedBlock { 0 };   // το block στο οποίο δρουν Hold και looper
        std::atomic<int> lastPlayed { -1 };     // το block που έπαιξες τελευταίο: εκεί γράφει το Rec MIDI loop από πετάλι
        std::atomic<int> pedalMute { 0 }, pedalDel { 0 };       // πατήματα πεταλιού (mute / σβήσιμο): ο αριθμός πατημάτων = αριθμός block
        std::atomic<int> tapsMuteOut { 0 }, tapsDelOut { 0 };   // πατήματα που μετρήθηκαν τώρα (για την κεφαλίδα)
        std::atomic<int> tapMs { 500 };                         // αναμονή μετά το τελευταίο πάτημα
        std::atomic<int> autoLoop { 0 };                        // ανίχνευση επανάληψης: 0 = Off, 1 = Auto (ξεκινά μόνη)
        std::atomic<bool> tempoUserSet { false };               // το tempo το όρισες εσύ (κουτί, tap, Link, set): η ανίχνευση δεν το αλλάζει
        std::array<std::atomic<int>, numBlocks> autoPausedOut;   // 1 = η αυτόματη ανίχνευση του block είναι σε παύση (μετά από διαγραφή)
        std::array<std::atomic<int>, numBlocks> autoReadyOut;   // 1 = το block είναι έτοιμο για λούπα (για την κεφαλίδα)
        std::atomic<int> changeCount { 0 };     // ανεβαίνει όταν αλλάζει κάτι που πρέπει να αποθηκευτεί
        std::atomic<double> beatPosOut { 0.0 };
        std::atomic<int> axisBarsOut { 4 };

        // tempo και quantize (global)
        std::atomic<double> tempo { 110.0 };
        std::atomic<int> loopFit { 0 };   // 0 Free: κάθε λούπα κρατά το μήκος που παίχτηκε. 1 Fit: χωράει ακέραιες φορές στον κύκλο (μεγαλύτερη λούπα)
        std::atomic<int> startQ { 1 }, lengthQ { 2 }, snapQ { 2 };   // startQ/lengthQ: 0 Off, 1 Beat, 2 Bar. snapQ: 0 Off, 1 = 1/8, 2 = 1/16
        std::atomic<int> tapRequest { 0 };
        std::atomic<int> afterFirst { 0 };      // μετά το πρώτο loop: 0 = Play, 1 = Overdub
        std::atomic<int> statCalls { 0 }, statOver { 0 }, statOverFull { 0 };   // μετρήσεις callback ήχου (διαβάζονται κάθε λίγα δευτερόλεπτα από το Main)
        std::atomic<float> statMaxCb { 0.0f }, statMaxLock { 0.0f }, statBudget { 0.0f };
        // ανίχνευση «κρακελαρίσματος» στην τελική έξοδο: απότομα άλματα δείγματος, κορεσμός (>1.0), NaN/άπειρο
        std::atomic<int> statJumps { 0 }, statClip { 0 }, statNan { 0 };
        std::atomic<float> statJumpMax { 0.0f }, statPeak { 0.0f };
        std::atomic<int> loopEvtCount { 0 };    // νότες που παίζουν οι λούπες (για τα φώτα του hardware)
        // Διαγνωστικό: τα μηνύματα MIDI που δεν είναι νότες/clock (CC, pitch bend, program, SysEx κ.λπ.) περνούν σε μικρό ring για καταγραφή στο log (όχι στο νήμα ήχου)
        std::atomic<uint32_t> midiDiag[256] {};
        std::atomic<int> midiDiagW { 0 };
        std::atomic<int> midiCount { 0 };       // μετρά τα μηνύματα MIDI που έρχονται (για την ένδειξη στην κεφαλίδα)
        std::atomic<int> tapActiveOut { 0 };
        std::atomic<uint32_t> ctlHeld { 0 };   // πατημένα πλήκτρα ελέγχου (bit 0 = A0)

        // metronome
        std::atomic<bool> metroOn { false };
        std::atomic<double> metroCycleOut { 4.0 };   // κύκλος σε beats: ο μεγαλύτερος κύκλος λούπας ή ένα μέτρο (για το κλικ και την μπάρα)
        std::array<std::atomic<int>, numBlocks> liveSilent;   // 1 = όσο παίζει η λούπα του block, το live του block μένει σιωπηλό (default). 0 = ακούγεται και το live.
        std::atomic<juce::uint32> lastVolMoveMs { 0 };
        std::atomic<int> volMoveDirty { 0 };
        std::atomic<int> shiftHeld { 0 };       // sustain CC64 πατημένο: το ειδικό block παίρνει τα πλήκτρα της ζώνης του όσο το κρατάς
        std::atomic<int> specialBlock { -1 }, specialBlock2 { -1 };   // δύο ειδικά block (το καθένα με δική του ζώνη)
        bool isSp (int b) const { return b >= 0 && (b == specialBlock.load() || b == specialBlock2.load()); }
        //   // ειδικό block: πάνω από τα άλλα, παίζει στη ζώνη του όσο κρατιέται το sustain, έχει λούπα και εφέ (-1 = κανένα)
        std::atomic<int> mfBlock { -1 };        // MF (master flow): το block που ορίζει τις συγχορδίες που ακολουθούν τα άλλα στο Follow (-1 = όλα τα πλήκτρα)
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
        // ποιο block γράφει το Rec MIDI loop: όποιο ηχογραφεί ήδη, αλλιώς (πετάλι) το τελευταίο που έπαιξες, αλλιώς το επιλεγμένο
        int recTarget (bool fromPedal) const
        {
            for (int b = 0; b < numBlocks; ++b)
                if (active[(size_t) b].load() != 0 && loopStateOut[(size_t) b].load() == loopRec)
                    return b;
            const int lp = lastPlayed.load();
            if (fromPedal && lp >= 0 && lp < numBlocks && active[(size_t) lp].load() != 0) return lp;
            const int sel = selectedBlock.load();
            return sel >= 0 && sel < numBlocks && active[(size_t) sel].load() != 0 ? sel : -1;
        }

        void pressMain (int b) { if (b >= 0) ++cmdMain[(size_t) b]; }
        void pressUndo (int b) { ++cmdUndo[(size_t) b]; }
        void pressStop (int b) { ++cmdStop[(size_t) b]; }
        void pressMute (int b) { ++cmdMute[(size_t) b]; }
        // Play όλων των λουπών με ένα πάτημα, γρήγορο διπλό πάτημα = Stop όλων (ξαναπαίζουν συγχρονισμένες στη φάση τους)
        void pressPlayStop()
        {
            const auto now = juce::Time::getMillisecondCounter();
            const bool quick = now - lastPlayPressMs < (juce::uint32) tapMs.load();
            lastPlayPressMs = now;
            loopsStopped = quick ? 1 : 0;
            if (! quick) ++playAllGen;   // το Play ξαναπαίζει και τις λούπες που είχαν σταματήσει μεμονωμένα
            ++changeCount;
        }
        juce::uint32 lastPlayPressMs = 0;
        std::atomic<int> playAllGen { 0 };
        std::atomic<int> loopsStopped { 0 };
        void pressOverdub (int b) { if (b >= 0) rerecReq[(size_t) b] = 1; }   // overdub (μέχρι 2 στρώματα) ή κλείσιμό του
        void pressReRec (int b) { if (b >= 0) rerecReq[(size_t) b] = 1; }   // ξαναγράφω: σβήνει τη λούπα του block και ξεκινά νέα εγγραφή
        void tapFromUi()       { ++tapRequest; }
        void panic()           { ++panicCount; }

        void setTempo (double bpm)
        {
            bpm = juce::jlimit (40.0, 240.0, bpm);
            tempo = bpm;
            tempoUserSet = true;
            if (link.isEnabled()) link.setTempo (bpm);
            ++changeCount;
        }

        void setLinkEnabled (bool on) { link.enable (on); linkWanted = on; }
        bool isLinkEnabled() const    { return linkWanted.load(); }

        // ζώνη block: το επόμενο ζευγάρι πλήκτρων που θα πατηθούν την ορίζει
        void startZone (int b)          { zoneFirst[b] = 0; zoneState[b] = 1; }
        // zones first: block με ζώνη αλλά χωρίς plugin ακόμα (δεν παίζει ήχο, φαίνεται μόνο στα πλήκτρα)
        void reserveZone (int b)        { emptyZone[b] = 1; zoneFirst[b] = 0; zoneState[b] = 1; }
        void clearEmptyZone (int b)     { emptyZone[b] = 0; zoneFirst[b] = 0; zoneState[b] = 0; }
        // AL: συγχορδίες key switch ως κείμενο "πλήκτρο=νότα,νότα,νότα;..." (για αποθήκευση στο set)
        juce::String getAlText (int b)
        {
            const juce::ScopedLock sl (lock);
            const auto& B = blocks[(size_t) b];
            juce::String out;
            for (int k = 0; k < 128; ++k)
            {
                if (! B.alSwitch[(size_t) k]) continue;
                juce::StringArray notes;
                for (int n = 0; n < 128; ++n)
                    if (B.alChord[(size_t) k][(size_t) n]) notes.add (juce::String (n));
                if (out.isNotEmpty()) out << ";";
                out << k << "=" << notes.joinIntoString (",");
            }
            return out;
        }

        void setAlText (int b, const juce::String& text)
        {
            const juce::ScopedLock sl (lock);
            auto& B = blocks[(size_t) b];
            for (auto& c : B.alChord) c.reset();
            B.alSwitch.reset();
            for (const auto& item : juce::StringArray::fromTokens (text, ";", ""))
            {
                const int eq = item.indexOfChar ('=');
                if (eq < 1) continue;
                const int k = item.substring (0, eq).getIntValue();
                if (k < 0 || k > 127) continue;
                for (const auto& t : juce::StringArray::fromTokens (item.substring (eq + 1), ",", ""))
                {
                    const int n = t.getIntValue();
                    if (n >= 0 && n < 128) B.alChord[(size_t) k].set ((size_t) n);
                }
                if (B.alChord[(size_t) k].any()) B.alSwitch.set ((size_t) k);
            }
        }

        void setZone (int b, int l, int h, int seq = -1)
        {
            lo[b] = juce::jmax (firstPlayNote, juce::jmin (l, h));
            hi[b] = juce::jmax (lo[b].load(), juce::jmax (l, h));
            zoneSeq[b] = seq >= 0 ? seq : ++seqCounter;
            seqCounter = juce::jmax (seqCounter.load(), zoneSeq[b].load());
            zoneState[b] = 0;
        }
        int pendingBlock() const { return captureTarget(); }   // block που περιμένει ζώνη (-1 = κανένα)
        bool capturing() const
        {
            for (int b = 0; b < numBlocks; ++b)
                if ((active[b].load() != 0 || emptyZone[b].load() != 0) && zoneState[b].load() != 0) return true;
            return false;
        }

        // ποιο block παίζει το πλήκτρο n (-1 = κανένα). Αν επικαλύπτονται ζώνες, κερδίζει η πιο πρόσφατη.
        // MF: οι νότες του master block. Για το ειδικό block (SP) μετράει η ζώνη του και χωρίς πετάλι, ώστε οι συγχορδίες του να οδηγούν τα άλλα block
        bool noteBelongsToMf (int n) const
        {
            const int mf = mfBlock.load();
            if (mf >= 0 && isSp (mf))
                return mf < numBlocks && active[(size_t) mf].load() != 0 && zoneState[(size_t) mf].load() == 0
                       && n >= lo[(size_t) mf].load() && n <= hi[(size_t) mf].load();
            return blockForNote (n) == mf;
        }
        int blockForNote (int n, bool ignorePedal = false, bool includeEmpty = false) const   // ignorePedal: για τα φωτάκια, να μην αλλάζουν με το sustain
        {
            if (! ignorePedal && shiftHeld.load() != 0 && n >= firstPlayNote)
            {
                int spBest = -1, spSeq = -1;
                for (int sb : { specialBlock.load(), specialBlock2.load() })
                    if (sb >= 0 && sb < numBlocks && active[(size_t) sb].load() != 0 && blockOff[(size_t) sb].load() == 0
                        && zoneState[(size_t) sb].load() == 0 && n >= lo[(size_t) sb].load() && n <= hi[(size_t) sb].load()   // μόνο στη ζώνη του
                        && zoneSeq[(size_t) sb].load() > spSeq)
                    { spBest = sb; spSeq = zoneSeq[(size_t) sb].load(); }
                if (spBest >= 0) return spBest;
            }
            int best = -1, bestSeq = -1;
            for (int b = 0; b < numBlocks; ++b)
                if (! isSp (b) && asMode[b].load() == 0 && (active[b].load() != 0 || (includeEmpty && emptyZone[b].load() != 0)) && zoneState[b].load() == 0 && n >= lo[b].load() && n <= hi[b].load()
                    && zoneSeq[b].load() > bestSeq)
                {
                    best = b;
                    bestSeq = zoneSeq[b].load();
                }
            return best;
        }

        // το ειδικό block παίζει το πλήκτρο n (εφόσον υπάρχει, είναι αναμμένο και το πλήκτρο είναι στη ζώνη του)
        // (το ειδικό block παίζει μόνο τα πλήκτρα της ζώνης του και μόνο όσο κρατιέται το sustain, βλ. blockForNote)
        int specialForNote (int) const { return -1; }

        void startVolumeLearn (int b) { mapLearnTarget = -1; volLearn = b; }
        void startPedalLearn (int i)  { mapLearnTarget = -1; volLearn = -1; knobLearnSlot = -1; masterLearn = 0; pedalLearn = i; }
        void cancelLearns()           { mapLearnTarget = -1; volLearn = -1; knobLearnSlot = -1; masterLearn = 0; pedalLearn = -1; }

        double getSampleRate() const { return sampleRate.load(); }

        juce::AudioPluginInstance* getPlugin (int b) { return blocks[(size_t) b].plugin.get(); }
        juce::AudioPluginInstance* getMidiFx (int b) { return blocks[(size_t) b].midiFx.get(); }
        int numFx (int b) const { return (int) chainOf (b).size(); }
        juce::AudioPluginInstance* getFx (int b, int i)
        {
            auto& c = chainOf (b);
            return i >= 0 && i < (int) c.size() ? c[(size_t) i].get() : nullptr;
        }

        // Προθέρμανση: το νέο plugin επεξεργάζεται λίγα blocks σιωπής ΠΡΙΝ μπει στη ροή, ώστε οι αργές πρώτες αρχικοποιήσεις (δέσμευση μνήμης, φόρτωση δειγμάτων)
        // να γίνουν εκτός του ηχητικού νήματος. Τρέχει στο νήμα που φορτώνει, πριν το κλείδωμα.
        void warmUp (juce::AudioPluginInstance& p, const juce::String& what)
        {
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            const int ch = juce::jmax (2, juce::jmax (p.getTotalNumInputChannels(), p.getTotalNumOutputChannels()));
            juce::AudioBuffer<float> buf (ch, juce::jmax (64, blockSize));
            juce::MidiBuffer midi;
            for (int i = 0; i < 6; ++i)
            {
                buf.clear();
                midi.clear();
                p.processBlock (buf, midi);
            }
            flowLog ("warm-up " + what + ": " + juce::String (juce::Time::getMillisecondCounterHiRes() - t0, 1) + " ms");

            // λίστα παραμέτρων (όπως τις δηλώνει το plugin): δείχνει ποιες μπορούν να ανατεθούν αυτόματα στα κουμπιά του hardware
            const auto& prms = p.getParameters();
            juce::String line;
            for (int i = 0; i < prms.size() && i < 200; ++i)
            {
                line << i << "=" << prms[i]->getName (28) << (prms[i]->isAutomatable() ? "" : "(na)") << "; ";
                if ((i + 1) % 16 == 0 || i + 1 == prms.size() || i == 199) { flowLog ("params " + what + " (" + juce::String (prms.size()) + " total): " + line); line.clear(); }
            }
        }

        // Puts a new plugin (or nullptr) into a block and returns the old one.
        PluginPtr swapPlugin (int b, PluginPtr fresh, bool captureZone = false)
        {
            if (fresh != nullptr)
            {
                prepare (*fresh);
                fresh->setPlayHead (&head);
                warmUp (*fresh, fresh->getName());
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
                if (B.plugin != nullptr) emptyZone[b] = 0;
                zoneFirst[b] = 0;
                zoneState[b] = (B.plugin != nullptr && captureZone) ? 1 : 0;
                B.prevGain = -1.0f;
                B.seenMain = cmdMain[(size_t) b].load();
                B.seenUndo = cmdUndo[(size_t) b].load();
                if (pedalAct[b] != 0)
            {
                const int act = pedalAct[b];
                pedalAct[b] = 0;
                if ((act == 1 || act == 2) && B.loopState == loopPlay && B.nLayers > 0)
                {
                    if (act == 1) B.muteMask |= 1u; else B.muteMask &= ~1u;
                    cutLoopNotes (B);
                }
                else if (act == 3 || act == 4)
                {
                    loopUndo (B);
                    B.hist.clear(); B.autoReady = false; B.lastDetect = 0;
                    B.autoPaused = act == 4;
                }
            }
            B.seenStop = cmdStop[(size_t) b].load();
                B.seenMute = cmdMute[(size_t) b].load(); B.seenChord = chordGen.load();
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
                flowLog ("setMidiFx: prepare start, in=" + juce::String (fresh->getTotalNumInputChannels()) + " out=" + juce::String (fresh->getTotalNumOutputChannels())
                         + " buses in/out=" + juce::String (fresh->getBusCount (true)) + "/" + juce::String (fresh->getBusCount (false)));
                prepare (*fresh);
                flowLog ("setMidiFx: prepare done, in=" + juce::String (fresh->getTotalNumInputChannels()) + " out=" + juce::String (fresh->getTotalNumOutputChannels()));
                fresh->setPlayHead (&head);
                warmUp (*fresh, fresh->getName());
            }
            const juce::ScopedLock sl (lock);
            auto& B = blocks[(size_t) b];
            PluginPtr old = std::move (B.midiFx);
            B.midiFx = std::move (fresh);
            if (B.midiFx != nullptr)
                B.mfxScratch.setSize (juce::jmax (1, juce::jmax (B.midiFx->getTotalNumInputChannels(), B.midiFx->getTotalNumOutputChannels())),
                                      juce::jmax (blockSize, 4096));
            B.mfxLogged = false;
            B.panicPending = true;
            flowLog ("setMidiFx: installed");
            return old;
        }

        // αλυσίδα FX: b = 0..8 για block, b = -1 για το master
        void addFx (int b, PluginPtr fresh)
        {
            if (fresh == nullptr) return;
            prepareFx (*fresh);
            fresh->setPlayHead (&head);
            warmUp (*fresh, fresh->getName());
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
            snap.anchor = std::fmod (std::fmod (B.loopAnchor, B.loopBeats) + B.loopBeats, B.loopBeats);
            snap.mute = B.muteMask;
            snap.layers.clear(); snap.layerBeats.clear(); snap.layerMode.clear();
            for (int l = 0; l < B.nLayers; ++l) { snap.layers.push_back (B.layerEv[(size_t) l]); snap.layerBeats.push_back (B.layerBeats[(size_t) l]); snap.layerMode.push_back (B.layerMode[(size_t) l]); }
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
            for (int l = 0; l < maxLayers; ++l)
            {
                B.layerMode[(size_t) l] = l < B.nLayers && (size_t) l < snap.layerMode.size() ? juce::jlimit (0, 2, snap.layerMode[(size_t) l]) : 0;
                B.layerStart[(size_t) l] = -1.0;
                B.layerArm[(size_t) l] = (B.layerMode[(size_t) l] != 0) ? -2.0 : -1.0;   // once/hold ξεκινούν στο επόμενο μέτρο
            }
            B.muteMask = snap.mute;
            B.loopAnchor = snap.anchor;
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

            // μετρήσεις για τις διακοπές ήχου: συνολικός χρόνος του callback και πόσο περιμένει το κλείδωμα (π.χ. όταν η οθόνη το κρατά)
            struct Probe
            {
                Engine& en; int n; float* const* out = nullptr; int nch = 0; const juce::int64 t0 = juce::Time::getHighResolutionTicks(); float lockWait = 0.0f;
                Probe (Engine& e, int ns) : en (e), n (ns) {}
                ~Probe()
                {
                    const double total = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);
                    const double budget = (double) n / std::max (1.0, en.sampleRate.load());
                    en.statCalls.fetch_add (1);
                    if ((float) total > en.statMaxCb.load()) en.statMaxCb = (float) total;
                    if (lockWait > en.statMaxLock.load()) en.statMaxLock = lockWait;
                    if (total > 0.8 * budget) en.statOver.fetch_add (1);
                    if (total > budget) en.statOverFull.fetch_add (1);
                    en.statBudget = (float) budget;
                    int jumps = 0, clip = 0, nan = 0; float jmax = 0.0f, pk = 0.0f;
                    for (int c = 0; c < juce::jmin (2, nch); ++c)
                    {
                        const float* d = out[c];
                        for (int i = 0; i < n; ++i)
                        {
                            const float x = d[i];
                            if (! std::isfinite (x)) { ++nan; continue; }
                            const float a = std::abs (x);
                            if (a > pk) pk = a;
                            if (a > 1.0f) ++clip;
                            if (i > 0 && std::isfinite (d[i - 1])) { const float j = std::abs (x - d[i - 1]); if (j > 0.3f) ++jumps; if (j > jmax) jmax = j; }
                        }
                    }
                    if (jumps > 0) en.statJumps.fetch_add (jumps);
                    if (clip > 0) en.statClip.fetch_add (clip);
                    if (nan > 0) en.statNan.fetch_add (nan);
                    if (jmax > en.statJumpMax.load()) en.statJumpMax = jmax;
                    if (pk > en.statPeak.load()) en.statPeak = pk;
                }
            } probe (*this, numSamples);
            probe.out = outputChannelData; probe.nch = numOutputChannels;

            for (int c = 0; c < numOutputChannels; ++c)
                juce::FloatVectorOperations::clear (outputChannelData[c], numSamples);
            numOutChannels = numOutputChannels;

            const auto tLock0 = juce::Time::getHighResolutionTicks();
            const juce::ScopedLock sl (lock);
            probe.lockWait = (float) juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - tLock0);

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
                ownerSp.fill (-1);
                seenPanic = panicCount.load();
            }

            dispatchIncoming (numSamples, gs, sr);
            keyAutoUpdate();
            followUpdateTarget();
            processPedalTaps (gs, sr);
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
                    B.seenMute = cmdMute[(size_t) b].load(); B.seenChord = chordGen.load();
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
                    if (! B.mfxLogged) flowLog ("midiFx first processBlock: start, channels=" + juce::String (mview.getNumChannels()) + " samples=" + juce::String (numSamples));
                    B.midiFx->processBlock (mview, B.out);
                    if (! B.mfxLogged) { B.mfxLogged = true; flowLog ("midiFx first processBlock: done"); }
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

                if (smartOn.load() != 0) smartProcess (B, b, numSamples, sr);
                else if (smartBand[(size_t) b][1].load() != 0.0f) for (int k = 0; k < smartBands; ++k) smartBand[(size_t) b][(size_t) k] = 0.0f;

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

            // ---- master ένταση και ασφαλιστικός limiter (ό,τι ακούγεται είναι και ό,τι ηχογραφείται)
            {
                float* mL = masterBuf.getWritePointer (0);
                float* mR = masterBuf.getWritePointer (1);
                const float pos = masterPos.load();
                const float g1 = pos * pos;
                const float g0 = masterGainPrev;
                masterGainPrev = g1;
                const bool lim = limiterOn.load() != 0;
                float outPk = 0.0f;
                const float ceilV = 0.97f;
                const float rel = 1.0f - std::exp (-1.0f / (0.12f * (float) juce::jmax (8000.0, sr)));
                for (int i = 0; i < numSamples; ++i)
                {
                    const float g = g0 + (g1 - g0) * (float) (i + 1) / (float) numSamples;
                    float l = mL[i] * g, r = mR[i] * g;
                    if (lim)
                    {
                        const float pk = std::max (std::abs (l), std::abs (r));
                        const float need = pk > ceilV ? ceilV / pk : 1.0f;
                        limEnv = need < limEnv ? need : std::min (need, limEnv + (1.0f - limEnv) * rel);
                        l *= limEnv; r *= limEnv;
                    }
                    mL[i] = l; mR[i] = r;
                    outPk = std::max (outPk, std::max (std::abs (l), std::abs (r)));
                }
                // ένταση μετρονόμου: 50% της έντασης που ακούγεται (γρήγορη άνοδος, αργή πτώση ~8 s, με ελάχιστο ώστε να ακούγεται και στις σιωπές)
                musicEnv = std::max (outPk, musicEnv * std::exp (-(float) numSamples / (8.0f * (float) juce::jmax (8000.0, sr))));
                metroAutoLevel = 0.5f * std::max (musicEnv, 0.1f);
            }

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
            metroCycleOut = playCycle();
            cycleAnchorOut = playCycleAnchor();
            const double bars = longest / beatsPerBar;
            axisBarsOut = bars <= 4.0 + 1e-6 ? 4 : bars <= 8.0 + 1e-6 ? 8 : 16;

            beatPos = beat1 + pendingBeatShift;
            pendingBeatShift = 0.0;
            beatPosOut = beatPos;
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

        struct HistEv { int64_t t; bool on; int note; int vel; };
        struct AutoHit { double D = 0.0; int64_t tStart = 0; };

        struct Block
        {
            PluginPtr plugin, midiFx;
            std::vector<PluginPtr> fx;
            juce::AudioBuffer<float> scratch, mfxScratch;
            juce::MidiBuffer out;
            int followKey = -1;   // κατάσταση Follow που ισχύει για τις νότες που ηχούν (αλλάζει → οι κρατημένες νότες ξαναπαίζονται)
            std::bitset<128> pressed, latched, sounding, loopSounding, recOpen;
            std::bitset<128> lpGroup, lpSingle;   // LP: νότες που μαζεύονται στο παράθυρο συγχορδίας / μονές νότες που παίζουν όσο πατιούνται
            bool lpOpen = false;
            int64_t lpEnd = 0;
            // AL (Advanced Latch): key switches → συγχορδίες
            std::array<std::bitset<128>, 128> alChord;
            std::bitset<128> alSwitch, alGroup, alPending, alMute;
            bool alOpen = false, alHavePending = false;
            int64_t alEnd = 0;
            std::vector<HistEv> hist;       // πρόσφατες νότες για την ανίχνευση επανάληψης
            int64_t lastDetect = 0;
            bool autoReady = false;
            bool autoPaused = false;   // μετά από διαγραφή: καμία ανίχνευση μέχρι νέο re-record
            AutoHit autoHit;
            std::array<std::vector<LoopEvent>, maxLayers> layerEv;
            std::array<double, maxLayers> layerBeats {};
            std::array<int, maxLayers> layerMode {};        // 0 loop, 1 once, 2 hold at end
            std::array<double, maxLayers> layerStart {};    // once/hold: beat που ξεκίνησε το τρέχον παίξιμο (<0 = δεν παίζει)
            std::array<double, maxLayers> layerArm {};      // once/hold: προγραμματισμένη εκκίνηση (beat), -2 = στο επόμενο μέτρο, <0 = καμία
            int curMode = 0, wantMode = 0, seenChord = 0;
            int nLayers = 0;
            unsigned muteMask = 0;
            int seenMain = 0, seenUndo = 0, seenStop = 0, seenMute = 0, seenPanic = 0, taps = 0;
            bool stopped = false, stopTapValid = false, mfxLogged = false, userStopped = false;
            int undoTaps = 0, seenPlayAll = 0; int64_t undoLastT = 0;
            int64_t lastStopTap = 0;
            unsigned loopSusMask = 0, loopBendMask = 0;
            int64_t lastTap = 0;
            std::array<uint8_t, 128> vel {};
            std::array<uint8_t, 128> outNote {}, loopOut {}, loopVoiceN {};
            std::array<std::array<uint8_t, 3>, 128> loopOutV {};   // πολυφωνική μετάθεση: οι έξοδοι κάθε νότας της λούπας
            std::array<std::array<uint8_t, 6>, 128> chordNotes {};   // νότες συγχορδίας ανά πλήκτρο (εκτός της βασικής)
            std::array<uint8_t, 128> chordCount {};
            float prevGain = -1.0f;
            // Smart master: φίλτρα μέτρησης και διόρθωσης, εξομαλυμένες ενέργειες
            float smLp[5] {};                                    // μέτρηση: μονοφωνικό άθροισμα, 5 φίλτρα (όρια των 6 ζωνών)
            float smCorrL[5] {}, smCorrR[5] {};                  // διόρθωση: τα ίδια φίλτρα ανά κανάλι
            float smEn[6] {}, smEnM = 0.0f, smEnS = 0.0f;
            float smLong[6] {}, smDbMean = -60.0f, smDbVar = 0.0f, smSoundSec = 0.0f;   // Smart αυτόματος τύπος: μακροπρόθεσμο φάσμα και διακύμανση στάθμης
            float smPrevG[6] { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
            float smPrevW = 1.0f;
            int lastLatched = -1;
            int prevHold = 0, prevArp = 0;
            int arpIndex = 0;
            int loopState = loopOff;
            double recStart = 0.0, loopBeats = 0.0;
            double loopAnchor = 0.0;   // beat όπου ξεκινά η λούπα: η εγγραφή ξεκίνησε εκεί και το τέλος της οδηγεί πάλι εκεί
            bool panicPending = false;
            std::vector<LoopEvent> rec;
            double stopAt = -1.0;            // το τέλος της εγγραφής περιμένει αυτή τη γραμμή μέτρου (-1 = καμία)
            std::vector<LoopEvent> preRec;   // τελευταίες νότες πριν ξεκινήσει η εγγραφή: μπαίνουν στη λούπα όταν η αρχή κουμπώνει πίσω στον κύκλο

            void resetState()
            {
                pressed.reset(); latched.reset(); sounding.reset(); loopSounding.reset(); recOpen.reset();
                lpGroup.reset(); lpSingle.reset(); lpOpen = false; lpEnd = 0;
                for (auto& c : alChord) c.reset();
                alSwitch.reset(); alGroup.reset(); alPending.reset(); alMute.reset(); alOpen = false; alHavePending = false; alEnd = 0;
                hist.clear(); lastDetect = 0; autoReady = false; autoPaused = false; autoHit = AutoHit();
                vel.fill (0);
                outNote.fill (0); loopOut.fill (0); loopVoiceN.fill (0); chordCount.fill (0);
                lastLatched = -1; prevHold = 0; prevArp = 0; arpIndex = 0;
                loopState = loopOff; recStart = 0.0; loopBeats = 0.0; loopAnchor = 0.0;
                rec.clear();
                for (auto& l : layerEv) l.clear();
                layerMode.fill (0); layerStart.fill (-1.0); layerArm.fill (-1.0); curMode = 0;
                nLayers = 0; muteMask = 0; taps = 0; lastTap = 0; stopped = false; userStopped = false; undoTaps = 0; stopTapValid = false; loopSusMask = 0; loopBendMask = 0;
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

        static const int* followScale (int type)
        {
            static const int t[3][7] = { { 0, 2, 4, 5, 7, 9, 11 },    // Major
                                         { 0, 2, 3, 5, 7, 8, 10 },    // Minor (natural)
                                         { 0, 2, 3, 5, 7, 8, 11 } };  // Harmonic minor
            return t[juce::jlimit (0, 2, type)];
        }

        // η νότα της λούπας μεταφέρεται από την κλίμακα που ηχογράφησες στην κλίμακα της συγχορδίας, βαθμίδα προς βαθμίδα
        int followMapNote (int b, int note) const
        {
            if (followOn.load() == 0 || followBlock[(size_t) b].load() == 0 || b == mfBlock.load()) return note;   // το MF block είναι η πηγή: δεν ακολουθεί
            return followMapNoteTo (note, followRoot.load(), followType.load(), followTRoot.load(), followTType.load());
        }

        static int followMapNoteTo (int note, int sR, int sT, int tR, int tT)
        {
            if (sR == tR && sT == tT) return note;
            const int* sI = followScale (sT);
            const int* tI = followScale (tT);
            const int rel = ((note - sR) % 12 + 12) % 12;
            const int base = note - rel;
            int d = 0;
            for (int i = 0; i < 7; ++i) if (sI[i] <= rel) d = i;
            const int leftover = rel - sI[d];                       // νότα εκτός κλίμακας: κρατά την απόσταση από τη βαθμίδα
            const int shift = ((tR - sR + 6 + 120) % 12) - 6;      // η ρίζα μετακινείται στο κοντινότερο σημείο
            return juce::jlimit (0, 127, base + shift + tI[d] + leftover);
        }

        // η συγχορδία που κρατάς (τουλάχιστον 3 διαφορετικές νότες) ορίζει τη νέα ρίζα και τον τύπο (μείζονα / ελάσσονα)
        uint64_t tgtPrevLo = 0, tgtPrevHi = 0;
        void followUpdateTarget()
        {
            const uint64_t lo = pressBits[0].load(), hi = pressBits[1].load();
            const bool newPress = ((lo & ~tgtPrevLo) | (hi & ~tgtPrevHi)) != 0;
            tgtPrevLo = lo; tgtPrevHi = hi;
            // Ο στόχος αλλάζει μόνο όταν πατηθεί νέο πλήκτρο. Όταν αφήνεις τη συγχορδία, τα πλήκτρα δεν σηκώνονται ταυτόχρονα
            // και η «χαμηλότερη νότα που έμεινε» θα έσερνε τη ρίζα σε άλλη νότα (τα latched blocks γύριζαν σε λάθος νότα).
            if (! newPress) return;
            int mask = 0, lowest = -1;
            for (int n = 0; n < 128; ++n)
            {
                const bool p = n < 64 ? ((lo >> n) & 1u) != 0 : ((hi >> (n - 64)) & 1u) != 0;
                if (p && (mfBlock.load() < 0 || noteBelongsToMf (n))) { mask |= 1 << (n % 12); if (lowest < 0) lowest = n; }   // με MF: μόνο οι νότες του MF block
            }
            const int pcs = (int) std::bitset<12> ((unsigned) mask).count();
            if (anyFollowPoly() && followOn.load() != 0)
            {
                // πολυφωνικά (λούπες): οι (έως 3) χαμηλότερες διαφορετικές νότες που κρατάς γίνονται οι φωνές. Όταν τις αφήσεις, μένουν οι τελευταίες.
                int cnt = 0, seen = 0;
                for (int n = 0; n < 128 && cnt < maxPolyVoices; ++n)
                {
                    const bool p = n < 64 ? ((lo >> n) & 1u) != 0 : ((hi >> (n - 64)) & 1u) != 0;
                    if (! p || (mfBlock.load() >= 0 && ! noteBelongsToMf (n)) || ((seen >> (n % 12)) & 1) != 0) continue;
                    seen |= 1 << (n % 12);
                    followVoiceRoot[(size_t) cnt++] = n % 12;
                }
                if (cnt > 0)
                {
                    followVoiceN = cnt;
                    if (pcs < 3) { followTRoot = followVoiceRoot[0].load(); followTType = followType.load(); }
                }
                // με 3+ νότες συνεχίζει παρακάτω: ρίζα και τύπος της συγχορδίας για τα live (latched) blocks
            }
            if (pcs < 3) return;
            const int root = lowest % 12;
            const bool m3 = ((mask >> ((root + 3) % 12)) & 1) != 0, M3 = ((mask >> ((root + 4) % 12)) & 1) != 0;
            const int type = (m3 && ! M3) ? 1 : (M3 && ! m3) ? 0 : followType.load();
            if (root * 2 + type != lastChordKey) { lastChordKey = root * 2 + type; ++chordGen; }   // αλλαγή συγχορδίας: ξαναπαίζουν τα hold-at-end
            if (followOn.load() == 0)    // με σβηστό Follow: μαθαίνει την τονικότητα πηγής από την τελευταία συγχορδία
            {
                followHomeRoot = root; followHomeType = type; followHomeSet = 1;
                return;
            }
            followTRoot = root;
            followTType = type;
        }

        static float smartLevel (float meanSquare)
        {
            const float db = 10.0f * std::log10 (meanSquare + 1.0e-12f);
            return juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
        }

        // μετράει το block (πριν τη διόρθωση) σε 6 ζώνες και εφαρμόζει τη διόρθωση (πάντα Auto, στο ποσοστό αποδοχής)
        void smartProcess (Block& B, int b, int n, double sr)
        {
            float* L = B.scratch.getWritePointer (0);
            float* R = B.scratch.getWritePointer (1);
            float a[5];
            for (int k = 0; k < 5; ++k) a[k] = 1.0f - std::exp (-6.2831853f * smartCross[k] / (float) sr);
            double sE[6] = {}, sM = 0, sS = 0;
            for (int i = 0; i < n; ++i)
            {
                const float m = 0.5f * (L[i] + R[i]), s = 0.5f * (L[i] - R[i]);
                for (int k = 0; k < 5; ++k) B.smLp[k] += a[k] * (m - B.smLp[k]);
                float band[6];
                band[0] = B.smLp[0];
                for (int k = 1; k < 5; ++k) band[k] = B.smLp[k] - B.smLp[k - 1];
                band[5] = m - B.smLp[4];
                for (int k = 0; k < 6; ++k) sE[k] += (double) band[k] * band[k];
                sM += (double) m * m; sS += (double) s * s;
            }
            const float inv = 1.0f / (float) juce::jmax (1, n);
            const float kk0 = 1.0f - std::exp (-(float) n / (0.3f * (float) sr));
            for (int k = 0; k < 6; ++k)
            {
                B.smEn[k] += kk0 * ((float) sE[k] * inv - B.smEn[k]);
                smartBand[(size_t) b][(size_t) k] = smartLevel (B.smEn[k]);
            }
            {   // Smart αυτόματος τύπος: αργή μέση ενέργεια ανά ζώνη και διακύμανση της στάθμης, μόνο όσο το block ηχεί
                double totE = 0.0; for (int k = 0; k < 6; ++k) totE += sE[k] * inv;
                if (totE > 1.0e-9)
                {
                    const float kL = 1.0f - std::exp (-(float) n / (4.0f * (float) sr));
                    for (int k = 0; k < 6; ++k) B.smLong[k] += kL * ((float) (sE[k] * inv) - B.smLong[k]);
                    const float dbNow = 10.0f * std::log10 ((float) totE + 1.0e-12f);
                    B.smDbMean += kL * (dbNow - B.smDbMean);
                    const float d = dbNow - B.smDbMean;
                    B.smDbVar += kL * (d * d - B.smDbVar);
                    B.smSoundSec = juce::jmin (60.0f, B.smSoundSec + (float) n / (float) sr);
                }
                float feat[4];
                smartAutoT[(size_t) b] = smartAutoClassify (B.smLong, std::sqrt (juce::jmax (0.0f, B.smDbVar)), B.smSoundSec, feat);
                for (int q = 0; q < 4; ++q) smartFeat[(size_t) b][(size_t) q] = feat[q];
            }
            B.smEnM += kk0 * ((float) sM * inv - B.smEnM);
            B.smEnS += kk0 * ((float) sS * inv - B.smEnS);
            float width = smartWidth[(size_t) b].load();
            if (B.smEnM + B.smEnS > 1.0e-9f)   // σε σιωπή κρατιέται η τελευταία τιμή
            {
                const float rm = std::sqrt (B.smEnM), rs = std::sqrt (B.smEnS);
                width = juce::jmin (1.0f, 2.0f * rs / (rm + rs));
                smartWidth[(size_t) b] = width;
            }

            // αναγκαία διόρθωση ως προς τα όρια του τύπου
            const int type = smartEffType (b);   // «Smart» (0) = ο τύπος που βρήκε το Smart μόνο του
            float lim[7]; smartLimits (type, lim);
            const float accept = smartAccept.load();
            const float kk = 1.0f - std::exp (-(float) n / (0.5f * (float) sr));
            const bool ctx = smartContext.load() != 0;
            float ctxCut[6] = {};
            if (ctx && type != 0)   // Context: η μείωση εξαρτάται από τα άλλα blocks (τα επίπεδα όλων, πριν τη διόρθωση)
            {
                float lv[numBlocks][smartBands]; int ty[numBlocks]; bool ac[numBlocks];
                for (int c = 0; c < numBlocks; ++c)
                {
                    ac[c] = active[(size_t) c].load() != 0;
                    ty[c] = smartEffType (c);
                    for (int k = 0; k < smartBands; ++k) lv[c][k] = smartBand[(size_t) c][(size_t) k].load();
                }
                smartContextCut (lv, ty, ac, numBlocks, b, 0.5f, ctxCut);
            }
            const float kkAtt = 1.0f - std::exp (-(float) n / (0.15f * (float) sr));   // Context: γρήγορο attack, αργό release
            float g[6];
            for (int k = 0; k < 6; ++k)
            {
                const float need = type == 0 ? 0.0f : ctx ? ctxCut[k] : juce::jlimit (0.0f, smartMaxDb (k), (smartBand[(size_t) b][(size_t) k].load() - lim[k]) * 60.0f);
                smartNeed[(size_t) b][(size_t) k] = need;
                float app = smartApp[(size_t) b][(size_t) k].load();
                if (type == 0) app = 0.0f;
                else app += (ctx && need * accept > app ? kkAtt : kk) * (need * accept - app);
                smartApp[(size_t) b][(size_t) k] = app;
                g[k] = std::pow (10.0f, -app / 20.0f);
            }
            const float needW = type == 0 || width <= lim[6] ? 1.0f : juce::jmax (0.3f, lim[6] / width);
            smartNeedWidth[(size_t) b] = needW;
            float appW = smartAppWidth[(size_t) b].load();
            if (type == 0) appW = 1.0f;
            else appW += kk * ((1.0f - (1.0f - needW) * accept) - appW);
            smartAppWidth[(size_t) b] = appW;

            // εφαρμογή: κάθε ζώνη παίρνει το κέρδος της (οι ζώνες είναι διαφορές φίλτρων, άθροισμα = αρχικό σήμα) και το πλάτος (κλίμακα του side)
            bool act = appW < 0.999f || B.smPrevW < 0.999f;
            for (int k = 0; k < 6; ++k) if (g[k] < 0.999f || B.smPrevG[k] < 0.999f) act = true;
            if (act)
            {
                for (int i = 0; i < n; ++i)
                {
                    const float t = (float) (i + 1) / (float) n;
                    float gk[6];
                    for (int k = 0; k < 6; ++k) gk[k] = B.smPrevG[k] + (g[k] - B.smPrevG[k]) * t;
                    const float w = B.smPrevW + (appW - B.smPrevW) * t;
                    for (int k = 0; k < 5; ++k)
                    {
                        B.smCorrL[k] += a[k] * (L[i] - B.smCorrL[k]);
                        B.smCorrR[k] += a[k] * (R[i] - B.smCorrR[k]);
                    }
                    float l = gk[0] * B.smCorrL[0], r = gk[0] * B.smCorrR[0];
                    for (int k = 1; k < 5; ++k)
                    {
                        l += gk[k] * (B.smCorrL[k] - B.smCorrL[k - 1]);
                        r += gk[k] * (B.smCorrR[k] - B.smCorrR[k - 1]);
                    }
                    l += gk[5] * (L[i] - B.smCorrL[4]);
                    r += gk[5] * (R[i] - B.smCorrR[4]);
                    const float m = 0.5f * (l + r), s = 0.5f * (l - r) * w;
                    L[i] = m + s; R[i] = m - s;
                }
            }
            for (int k = 0; k < 6; ++k) B.smPrevG[k] = g[k];
            B.smPrevW = appW;
        }

        juce::CriticalSection lock;
        std::array<Block, numBlocks> blocks;
        std::vector<PluginPtr> masterFx;
        float masterGainPrev = 1.0f, limEnv = 1.0f, musicEnv = 0.0f, metroAutoLevel = 0.25f;
        juce::MidiBuffer chordTmp;
        Head head;
        std::array<int, 128> owner;
        std::array<int, 128> ownerSp;   // ειδικό block: δεύτερος ιδιοκτήτης του πλήκτρου (παίζει μαζί με το κανονικό block)
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
                    tempoUserSet = true;
                    if (link.isEnabled()) link.setTempo (bpm);
                    ++changeCount;
                }
            }
        }

        //==================== metronome ====================
        // κύκλος σε beats: ο μεγαλύτερος κύκλος λούπας που παίζει, αλλιώς ένα μέτρο
        double playCycle() const
        {
            double c = 0.0;
            for (const auto& B : blocks)
                if (B.plugin != nullptr && (B.loopState == loopPlay || B.loopState == loopOver))
                    c = std::max (c, B.loopBeats);
            return c >= beatsPerBar - 1.0e-6 ? c : (double) beatsPerBar;
        }

        double playCycleAnchor() const
        {
            double c = 0.0, a = 0.0;
            for (const auto& B : blocks)
                if (B.plugin != nullptr && (B.loopState == loopPlay || B.loopState == loopOver) && B.loopBeats > c) { c = B.loopBeats; a = B.loopAnchor; }
            return a;
        }

        void renderMetronome (int n, double beat0, double beat1)
        {
            float* out = metroBuf.getWritePointer (0);
            juce::FloatVectorOperations::clear (out, n);

            // κλικ μόνο στην αρχή του κύκλου (έντονο) και στο τελευταίο beat του (πιο χαμηλά): σκαλοπάτι που δείχνει πότε τελειώνει/αρχίζει
            const double cyc = playCycle(), cycAnc = playCycleAnchor();
            std::array<int, 8> startPos {};
            std::array<bool, 8> startAccent {};
            int starts = 0;
            for (double k = std::ceil (beat0 - 1.0e-9); k < beat1 - 1.0e-9 && starts < 8; k += 1.0)
            {
                if (k < 0.0) continue;
                double pc = std::fmod (k - cycAnc, cyc);
                if (pc < 0.0) pc += cyc;
                const bool strong = pc < 1.0e-6 || pc > cyc - 1.0e-6;
                const bool weak = ! strong && std::abs (pc - (cyc - 1.0)) < 1.0e-6;
                if (! strong && ! weak) continue;
                startPos[(size_t) starts] = juce::jlimit (0, n - 1, (int) ((k - beat0) / blkBps));
                startAccent[(size_t) starts] = strong;
                ++starts;
            }

            const double sr = sampleRate.load();
            const float level = metroAutoLevel;   // 50% της έντασης των ήχων, χωρίς χειροκίνητο volume
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
                out[i] = v * level * (metroAccent ? 1.0f : 0.5f);
                ++metroPos;
            }
        }

        //==================== incoming MIDI -> blocks ====================
        // το block που περιμένει ζώνη: το επιλεγμένο αν περιμένει, αλλιώς το πρώτο που περιμένει
        int captureTarget() const
        {
            const int sel = selectedBlock.load();
            if (sel >= 0 && sel < numBlocks && (active[(size_t) sel].load() != 0 || emptyZone[(size_t) sel].load() != 0) && zoneState[(size_t) sel].load() != 0)
                return sel;
            for (int b = 0; b < numBlocks; ++b)
                if ((active[(size_t) b].load() != 0 || emptyZone[(size_t) b].load() != 0) && zoneState[(size_t) b].load() != 0)
                    return b;
            return -1;
        }

        //==================== AS (Assistant, rules) ====================
        struct AsEv { double beat; int note; int vel; bool on; };
        std::array<std::vector<AsEv>, numBlocks> asPend;
        std::vector<int> asHist;                 // πρόσφατες νότες του παίκτη
        double asLastPlayer = -1.0e9;
        std::atomic<bool> asClear { false }, asClearModel { false };
        // style: Markov μεταβλητής τάξης (Continuator-like). Κατάσταση = νότα*8 + απόσταση από την προηγούμενη νότα σε βήματα 1/8 (0..7)
        std::map<uint64_t, std::vector<uint16_t>> asModel;
        std::vector<uint16_t> asSeq, asGenSeq;   // τελευταίες καταστάσεις του παίκτη / της γενιάς του βοηθού
        static constexpr int asMaxOrder = 4;
        // orch: κρατημένες συγχορδίες που ακολουθούν την αρμονία του παίκτη
        std::bitset<128> asHeld;                              // πλήκτρα που κρατά τώρα ο παίκτης
        std::array<double, 128> asOnBeat {};                  // πότε πατήθηκε κάθε πλήκτρο (beat)
        std::array<std::vector<int>, numBlocks> asOrchNotes;  // νότες που κρατά τώρα ο βοηθός
        std::array<uint32_t, numBlocks> asOrchKey {};         // τρέχουσα συγχορδία (bass pc, μάσκα pcs), 0 = καμία
        juce::Random asRng { 7 };
        static constexpr double asGrid = 0.5;    // 1/8 σε beats: ρολόι του Flow, δουλεύει και χωρίς μετρονόμο

        int asScaleStep (int n, int steps) const   // μετακίνηση κατά βαθμίδες της κλίμακας Follow (major / minor)
        {
            static const int maj[7] { 0, 2, 4, 5, 7, 9, 11 }, mnr[7] { 0, 2, 3, 5, 7, 8, 10 };
            const int* sc = followType.load() == 0 ? maj : mnr;
            const int root = followRoot.load() % 12;
            int deg = -1, pcRel = ((n - root) % 12 + 12) % 12;
            for (int i = 0; i < 7; ++i) if (sc[i] == pcRel) deg = i;
            if (deg < 0) return n;   // εκτός κλίμακας: αμετάβλητη
            const int oct = (n - root - pcRel) / 12;
            int d = deg + steps, o = oct;
            while (d >= 7) { d -= 7; ++o; }
            while (d < 0)  { d += 7; --o; }
            return root + o * 12 + sc[d];
        }

        void asSchedule (int b, double beat, int note, int vel, double len)
        {
            note = juce::jlimit (21, 108, note);
            auto& v = asPend[(size_t) b];
            if (v.size() > 256) return;
            v.push_back ({ beat, note, juce::jlimit (1, 127, vel), true });
            v.push_back ({ beat + len, note, 0, false });
        }

        static uint64_t asKey (const std::vector<uint16_t>& q, int k)   // κλειδί: τελευταίες k καταστάσεις + η τάξη
        {
            uint64_t h = 1469598103934665603ull ^ (uint64_t) k;
            for (size_t i = q.size() - (size_t) k; i < q.size(); ++i) h = (h ^ q[i]) * 1099511628211ull;
            return h;
        }
        void asLearn (uint16_t next)   // asSeq = ιστορικό πριν την next
        {
            for (int k = 1; k <= asMaxOrder && (size_t) k <= asSeq.size(); ++k)
            {
                auto& v = asModel[asKey (asSeq, k)];
                v.push_back (next);
                if (v.size() > 32) v.erase (v.begin());
            }
            if (asModel.size() > 20000) asModel.clear();
        }
        int asSample (const std::vector<uint16_t>& ctx)   // -1 = δεν υπάρχει συνέχεια
        {
            for (int k = juce::jmin (asMaxOrder, (int) ctx.size()); k >= 1; --k)
            {
                const auto it = asModel.find (asKey (ctx, k));
                if (it != asModel.end() && ! it->second.empty())
                    return it->second[(size_t) asRng.nextInt ((int) it->second.size())];
            }
            return -1;
        }
        int asMeanPitch() const
        {
            if (asHist.empty()) return 60;
            int sum = 0; for (int n : asHist) sum += n;
            return sum / (int) asHist.size();
        }
        // μία νότα του βοηθού σε οκτάβα κάτω από το μέσο ύψος του παίκτη
        int asPlace (int note) const
        {
            const int target = asMeanPitch() - 12;
            while (note > target + 6) note -= 12;
            while (note < target - 6) note += 12;
            return juce::jlimit (21, 108, note);
        }
        // φράση 1–3 νοτών από το μοντέλο, ξεκινά στο t
        void asPhrase (int b, double t, int vel)
        {
            if (asGenSeq.empty()) asGenSeq = asSeq;
            if (asGenSeq.empty()) return;
            const int len = 1 + asRng.nextInt (3);
            for (int i = 0; i < len; ++i)
            {
                const int st = asSample (asGenSeq);
                if (st < 0) { asGenSeq = asSeq; return; }
                asGenSeq.push_back ((uint16_t) st);
                if ((int) asGenSeq.size() > asMaxOrder) asGenSeq.erase (asGenSeq.begin());
                const int gap = st & 7;
                if (i > 0) t += (double) juce::jmax (1, gap) * asGrid;
                asSchedule (b, t, asPlace (st >> 3), vel, asGrid * 0.9);
            }
        }

        // ο παίκτης πάτησε νότα (beat): ο βοηθός απαντά με παράγωγο, κουμπωμένο στο επόμενο 1/8
        void asObserve (double beat, int n, int vel)
        {
            asHeld.set ((size_t) n & 127); asOnBeat[(size_t) n & 127] = beat;
            bool anyAs = false;
            for (int b = 0; b < numBlocks; ++b) if (asMode[(size_t) b].load() != 0) anyAs = true;
            if (anyAs)
            {
                const double gapB = beat - asLastPlayer;
                if (gapB > 8.0) { asSeq.clear(); asGenSeq.clear(); }
                const int gs = gapB > 8.0 ? 0 : juce::jlimit (0, 7, (int) std::lround (gapB / asGrid));
                const uint16_t st = (uint16_t) (n * 8 + gs);
                asLearn (st);
                asSeq.push_back (st);
                if ((int) asSeq.size() > asMaxOrder) asSeq.erase (asSeq.begin());
                asGenSeq.clear();   // η νέα φράση του παίκτη ξαναρχίζει το πλαίσιο της γενιάς
            }
            asLastPlayer = beat;
            asHist.push_back (n);
            if (asHist.size() > 16) asHist.erase (asHist.begin());
            for (int b = 0; b < numBlocks; ++b)
            {
                if (asMode[(size_t) b].load() == 0 || asMode[(size_t) b].load() == 3) continue;
                const int pct = asPct[(size_t) b].load();
                if (asRng.nextInt (100) >= pct) continue;
                const double t = std::ceil ((beat + 0.02) / asGrid) * asGrid;
                if (asMode[(size_t) b].load() == 2) { asPhrase (b, t, (int) ((float) vel * 0.7f)); continue; }
                static const int pick[8] { 0, 1, 1, 2, 2, 3, 0, 1 };
                int out = n;
                switch (pick[asRng.nextInt (8)])
                {
                    case 0: out = n - 12; break;               // οκτάβα κάτω
                    case 1: out = asScaleStep (n, 2); break;   // τρίτη πάνω
                    case 2: out = asScaleStep (n, 4); break;   // πέμπτη πάνω
                    default: out = n + 12; break;              // οκτάβα πάνω
                }
                asSchedule (b, t, out, (int) ((float) vel * 0.7f), asGrid * 0.9);
            }
        }

        void asEmit (int b, double beat, int note, int vel, bool on)
        {
            asPend[(size_t) b].push_back ({ beat, note, vel, on });
        }
        // συγχορδία του παίκτη τώρα: κρατημένα πλήκτρα + όσα πατήθηκαν το τελευταίο beat. Επιστρέφει κλειδί (0 = λιγότερες από 3 φωνές)
        uint32_t asChordNow (double beat, int& bassNote, int& vel) const
        {
            uint32_t mask = 0; int low = 128, vs = 0, vn = 0;
            for (int n = 21; n <= 108; ++n)
                if (asHeld[(size_t) n] || beat - asOnBeat[(size_t) n] < 1.0 && beat >= asOnBeat[(size_t) n])
                {
                    mask |= 1u << (n % 12);
                    if (n < low) low = n;
                    if (asHeld[(size_t) n]) { vs += 1; vn += 0; }
                }
            juce::ignoreUnused (vs, vn);
            if (low == 128 || __builtin_popcount (mask) < 3) return 0;
            bassNote = low; vel = 70;
            return ((uint32_t) (low % 12 + 1) << 16) | mask;
        }
        // φωνοθέτηση: μπάσο χαμηλά, οι νότες της συγχορδίας ανοιχτά πάνω, διπλή η ρίζα μία οκτάβα πάνω
        std::vector<int> asVoicing (uint32_t key, int bassNote) const
        {
            std::vector<int> v;
            const uint32_t mask = key & 0xFFFFu;
            const int bpc = (int) (key >> 16) - 1;
            int bass = 36 + bpc; if (bass > 47) bass -= 12;
            v.push_back (bass);
            int prev = bass + 7;
            for (int i = 0; i < 12; ++i)
            {
                const int pc = (bpc + i) % 12;
                if (! ((mask >> pc) & 1u)) continue;
                int n = 48 + ((pc - 48) % 12 + 12) % 12;
                while (n < prev + 3) n += 12;
                if (n > 84) n -= 12;
                if (n <= 108) { v.push_back (n); prev = n; }
            }
            juce::ignoreUnused (bassNote);
            return v;
        }
        void asOrchStep (int b, double g)   // κάθε beat: αλλαγή συγχορδίας ή λήξη σε μεγάλη παύση
        {
            auto& on = asOrchNotes[(size_t) b];
            auto release = [&] { for (int n : on) asEmit (b, g, n, 0, false); on.clear(); asOrchKey[(size_t) b] = 0; };
            if (g - asLastPlayer > 6.0) { if (! on.empty()) release(); return; }
            int bass = 0, vel = 70;
            const uint32_t k = asChordNow (g, bass, vel);
            if (k == 0 || k == asOrchKey[(size_t) b]) return;
            if (! on.empty()) release();
            asOrchKey[(size_t) b] = k;
            if (asRng.nextInt (100) >= asPct[(size_t) b].load()) return;   // ποσοστό: πόσο συχνά παίζει η ορχήστρα
            on = asVoicing (k, bass);
            for (int n : on) asEmit (b, g, n, 80, true);
        }

        // γέμισμα παύσεων + παράδοση των προγραμματισμένων νοτών στο block
        void asProcess (double beat0, double beat1)
        {
            if (asClear.exchange (false))
            {
                for (auto& v : asPend) v.clear();
                for (int b = 0; b < numBlocks; ++b)
                {
                    for (int n : asOrchNotes[(size_t) b]) asEmit (b, beat0, n, 0, false);
                    asOrchNotes[(size_t) b].clear(); asOrchKey[(size_t) b] = 0;
                }
            }
            if (asClearModel.exchange (false)) { asModel.clear(); asSeq.clear(); asGenSeq.clear(); }
            bool any = false;
            for (int b = 0; b < numBlocks; ++b) if (asMode[(size_t) b].load() != 0) any = true;
            if (! any) return;

            for (double g = std::ceil (beat0); g < beat1; g += 1.0)
                for (int b = 0; b < numBlocks; ++b)
                    if (asMode[(size_t) b].load() == 3) asOrchStep (b, g);
            if (! asHist.empty() && beat0 - asLastPlayer > 1.0 && beat0 - asLastPlayer < 24.0)
                for (double g = std::ceil (beat0 / asGrid) * asGrid; g < beat1; g += asGrid)
                    for (int b = 0; b < numBlocks; ++b)
                    {
                        if (asMode[(size_t) b].load() == 0 || asMode[(size_t) b].load() == 3) continue;
                        if (asRng.nextFloat() >= 0.35f * (float) asPct[(size_t) b].load() / 100.0f) continue;
                        if (asMode[(size_t) b].load() == 2) { if (asPend[(size_t) b].empty()) asPhrase (b, g, 60); continue; }
                        const int base = asHist[(size_t) asRng.nextInt ((int) asHist.size())];
                        const int out = asRng.nextBool() ? asScaleStep (base, 2) : base;
                        asSchedule (b, g, out, 60, asGrid * 0.9);
                    }

            for (int b = 0; b < numBlocks; ++b)
            {
                auto& v = asPend[(size_t) b];
                for (size_t i = 0; i < v.size();)
                {
                    if (v[i].beat >= beat1) { ++i; continue; }
                    Action a;
                    a.pos = juce::jlimit (0, 1 << 20, (int) ((v[i].beat - beat0) / blkBps));
                    a.order = v[i].on ? 1 : 0;
                    a.kind = Kind::Live; a.note = v[i].note; a.vel = v[i].vel; a.on = v[i].on;
                    inActions[(size_t) b].push_back (a);
                    v.erase (v.begin() + (long) i);
                }
            }
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
                if (! msg.isMidiClock() && ! msg.isActiveSense() && ! msg.isNoteOnOrOff() && msg.getRawDataSize() >= 1)
                {
                    const auto* d = msg.getRawData();
                    const int w = midiDiagW.load (std::memory_order_relaxed);
                    midiDiag[(size_t) (w & 255)].store (((uint32_t) d[0] << 16) | ((msg.getRawDataSize() > 1 ? (uint32_t) d[1] : 0u) << 8) | (msg.getRawDataSize() > 2 ? (uint32_t) d[2] : 0u) | ((uint32_t) juce::jmin (msg.getRawDataSize(), 15) << 24), std::memory_order_relaxed);
                    midiDiagW.store (w + 1, std::memory_order_release);
                }

                if (msg.isController() && msg.getControllerNumber() == 64)   // sustain = shift για το ειδικό block (momentary)
                {
                    const bool has = (specialBlock.load() >= 0 && specialBlock.load() < numBlocks && active[(size_t) specialBlock.load()].load() != 0)
                                  || (specialBlock2.load() >= 0 && specialBlock2.load() < numBlocks && active[(size_t) specialBlock2.load()].load() != 0);
                    if (has || shiftHeld.load() != 0)
                    {
                        shiftHeld = (has && msg.getControllerValue() >= 64) ? 1 : 0;
                        if (has) continue;
                    }
                }
                if (handlePedal (msg))
                    continue;

                if ((msg.isNoteOn() || msg.isNoteOff()) && ! isControlKey (msg.getNoteNumber()))
                {
                    const int pn = msg.getNoteNumber() & 127;
                    if (msg.isNoteOn()) pressBits[(size_t) (pn >> 6)].fetch_or ((uint64_t) 1 << (pn & 63));
                    else                pressBits[(size_t) (pn >> 6)].fetch_and (~((uint64_t) 1 << (pn & 63)));
                }
                else if ((msg.isNoteOn() || msg.isNoteOff()) && msg.getNoteNumber() >= lowestNote)   // πλήκτρα ελέγχου: ανάβει η τελεία όσο είναι πατημένα
                {
                    const uint32_t bit = 1u << (msg.getNoteNumber() - lowestNote);
                    if (msg.isNoteOn()) ctlHeld.fetch_or (bit); else ctlHeld.fetch_and (~bit);
                }

                if (msg.isNoteOn())
                {
                    const int n = msg.getNoteNumber();
                    if (n == followKeyNote)   // B0 = Follow on/off, δεν παίζει ήχο
                    {
                        toggleFollow();
                        continue;
                    }
                    if (n == tapKeyNote)      // A0 = tap tempo, δεν παίζει ήχο
                    {
                        registerTap (gs + pos, sr);
                        continue;
                    }
                    if (n == recAudioKeyNote) // A#0 = Rec Audio (σταματά ή νέο take), δεν παίζει ήχο
                    {
                        recorder.pressButton();
                        continue;
                    }
                    if (isControlKey (n)) continue;   // A#0: σιωπηλό
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
                    const int sp = specialForNote (n);
                    if (sp >= 0)
                    {
                        ownerSp[(size_t) n] = sp;
                        Action a;
                        a.pos = pos; a.order = 1; a.kind = Kind::Live; a.note = n; a.vel = msg.getVelocity(); a.on = true;
                        inActions[(size_t) sp].push_back (a);
                    }
                    if (b >= 0 || sp >= 0) asObserve (blkBeat0 + (double) pos * blkBps, n, (int) msg.getVelocity());
                    if (b >= 0)
                    {
                        owner[(size_t) n] = b;
                        lastPlayed = b;
                        Action a;
                        a.pos = pos; a.order = 1; a.kind = Kind::Live; a.note = n; a.vel = msg.getVelocity(); a.on = true;
                        inActions[(size_t) b].push_back (a);
                    }
                }
                else if (msg.isNoteOff())
                {
                    const int n = msg.getNoteNumber();
                    const int sp = ownerSp[(size_t) n];
                    if (sp >= 0 || owner[(size_t) n] >= 0) asHeld.reset ((size_t) n & 127);
                    if (sp >= 0)
                    {
                        ownerSp[(size_t) n] = -1;
                        Action a;
                        a.pos = pos; a.order = 1; a.kind = Kind::Live; a.note = n; a.on = false;
                        inActions[(size_t) sp].push_back (a);
                    }
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
                    const int sp = ownerSp[(size_t) msg.getNoteNumber()];
                    if (sp >= 0 && sp != b) pushRaw (sp, pos, msg);
                }
                else if (msg.isController() || msg.isPitchWheel() || msg.isChannelPressure())
                {
                    for (int b = 0; b < numBlocks; ++b)
                        if (active[(size_t) b].load() != 0)
                            pushRaw (b, pos, msg);
                }
            }
            asProcess (blkBeat0, blkBeat0 + (double) numSamples * blkBps);
        }

        // πετάλια και πλήκτρα ελέγχου: επιστρέφει true αν το μήνυμα το κατανάλωσε η ανάθεση (δεν πάει στα plugins)
        // ένα πετάλι θεωρείται πατημένο όσο στέλνει μηνύματα (ροή τιμών) και μέχρι να σταματήσει για 300 ms ή να σταλεί η αποδέσμευση
        std::array<int, 32> pedalDown {};
        std::array<juce::uint32, 32> pedalDownTime {};
        int pedalDownN = 0;
        juce::uint32 learnQuietUntil = 0;
        bool pedalIsDown (int code) const
        {
            const auto now = juce::Time::getMillisecondCounter();
            for (int i = 0; i < pedalDownN; ++i)
                if (pedalDown[(size_t) i] == code) return now - pedalDownTime[(size_t) i] < 300;
            return false;
        }
        void pedalSetDown (int code, bool down)
        {
            const auto now = juce::Time::getMillisecondCounter();
            for (int i = 0; i < pedalDownN; ++i)
                if (pedalDown[(size_t) i] == code)
                {
                    if (! down)
                    {
                        pedalDown[(size_t) i] = pedalDown[(size_t) (pedalDownN - 1)];
                        pedalDownTime[(size_t) i] = pedalDownTime[(size_t) (pedalDownN - 1)];
                        --pedalDownN;
                    }
                    else pedalDownTime[(size_t) i] = now;
                    return;
                }
            if (down)
            {
                if (pedalDownN >= (int) pedalDown.size()) pedalDownN = 0;
                pedalDown[(size_t) pedalDownN] = code;
                pedalDownTime[(size_t) pedalDownN++] = now;
            }
        }

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

            // τα 6 standard πετάλια έντασης: ένα CC (οποιοδήποτε κανάλι) = ένα block. Έχουν προτεραιότητα έναντι knobs/κουμπιών με το ίδιο CC.
            if (kind == 2)
            {
                const auto sameCc = [num] (int c) { return (c >> 16) == 2 && (c & 0xff) == num; };
                const int pl = pedalLearn.load();
                if (pl >= 0 && pl < numPedals)
                {
                    for (int j = 0; j < numPedals; ++j)
                        if (pedalCc[(size_t) j].load() == num) pedalCc[(size_t) j] = -1;
                    for (int i = 0; i < numControls; ++i)
                        if (sameCc (mapCode[(size_t) i].load())) mapCode[(size_t) i] = 0;
                    for (int k = 0; k < 8; ++k)
                        if (sameCc (knobCc[(size_t) k].load())) knobCc[(size_t) k] = 0;
                    for (int b = 0; b < numBlocks; ++b)
                        if (sameCc (volMap[(size_t) b].load())) volMap[(size_t) b] = 0;
                    if (sameCc (masterCc.load())) masterCc = 0;
                    pedalCc[(size_t) pl] = num;
                    pedalLearnedSlot = pl;
                    pedalLearnedNum = num;
                    ++pedalLearnedCount;
                    pedalLearn = -1;
                    ++changeCount;
                    return true;
                }
                for (int i = 0; i < numPedals; ++i)
                    if (pedalCc[(size_t) i].load() == num)
                    {
                        if (active[(size_t) i].load() != 0)
                        {
                            volPos[(size_t) i] = (float) msg.getControllerValue() / 127.0f;
                            lastVolMoveMs = juce::Time::getMillisecondCounter();
                            volMoveDirty = 1;
                        }
                        return true;
                    }
            }

            // MIDI learn πεταλιού έντασης (πρώτο, ώστε να μην το καταναλώσει knob ή master): η πρώτη κίνηση ενός CC το αναθέτει στο block.
            // Το ίδιο CC σβήνεται από knobs, master και κουμπιά: ένα CC ανήκει σε ένα μόνο χειριστήριο.
            const int vl = volLearn.load();
            if (vl >= 0 && kind == 2)
            {
                volMap[(size_t) vl] = code;
                for (int i = 0; i < numControls; ++i)
                    if (mapCode[(size_t) i].load() == code) mapCode[(size_t) i] = 0;
                for (int k = 0; k < 8; ++k)
                    if (knobCc[(size_t) k].load() == code) knobCc[(size_t) k] = 0;
                if (masterCc.load() == code) masterCc = 0;
                volLearnedBlock = vl;
                volLearnedCode = code;
                ++volLearnedCount;
                volLearn = -1;
                ++changeCount;
                return true;
            }

            // ποτενσιόμετρα: learn (πρώτο CC που κινείται) και κανονική λειτουργία (η τιμή περνά στο UI, το plugin δεν παίρνει το CC)
            // (όσο περιμένει ανάθεση κουμπιού, το CC δεν το καταναλώνει knob που είχε το ίδιο CC)
            if (kind == 2 && mapLearnTarget.load() < 0)
            {
                const float kv = (float) msg.getControllerValue() / 127.0f;
                if (masterLearn.load() != 0)
                {
                    for (int k = 0; k < 8; ++k)
                        if (knobCc[(size_t) k].load() == code) knobCc[(size_t) k] = 0;
                    for (int b = 0; b < numBlocks; ++b)
                        if (volMap[(size_t) b].load() == code) volMap[(size_t) b] = 0;
                    masterCc = code;
                    masterPos = kv;
                    masterLearn = 0;
                    ++masterLearned;
                    ++changeCount;
                    return true;
                }
                if (masterCc.load() == code) { masterPos = kv; return true; }
                const int ks = knobLearnSlot.load();
                if (ks >= 0 && ks < 8)
                {
                    for (int k = 0; k < 8; ++k)
                        if (knobCc[(size_t) k].load() == code) knobCc[(size_t) k] = 0;
                    for (int b = 0; b < numBlocks; ++b)
                        if (volMap[(size_t) b].load() == code) volMap[(size_t) b] = 0;
                    if (masterCc.load() == code) masterCc = 0;
                    knobCc[(size_t) ks] = code;
                    knobIn[(size_t) ks] = kv;
                    knobLearnedSlot = ks;
                    knobLearnSlot = -1;
                    ++changeCount;
                    return true;
                }
                for (int k = 0; k < 8; ++k)
                    if (knobCc[(size_t) k].load() == code) { knobIn[(size_t) k] = kv; return true; }
            }

            // MIDI learn κουμπιού
            const int target = mapLearnTarget.load();
            if (target >= 0 && press)
            {
                for (int i = 0; i < numControls; ++i)
                    if (mapCode[(size_t) i].load() == code) mapCode[(size_t) i] = 0;
                for (int b = 0; b < numBlocks; ++b)
                    if (volMap[(size_t) b].load() == code) volMap[(size_t) b] = 0;
                for (int k = 0; k < 8; ++k)
                    if (knobCc[(size_t) k].load() == code) knobCc[(size_t) k] = 0;
                if (masterCc.load() == code) masterCc = 0;

                mapCode[(size_t) target] = code;
                pedalSetDown (code, true);   // το πάτημα της ανάθεσης δεν δίνει τη λειτουργία: μόνο το επόμενο
                learnQuietUntil = juce::Time::getMillisecondCounter() + 500;
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
                    // η κίνηση του πεταλιού δεν σώζεται σε κάθε μήνυμα (έγραφε αρχείο στο UI thread και κόλλαγαν τα γραφικά): το Main σώζει όταν σταματήσει
                    lastVolMoveMs = juce::Time::getMillisecondCounter();
                    volMoveDirty = 1;
                    return true;
                }
            }

            // πετάλι / πλήκτρο κουμπιού: δρα στο επιλεγμένο block
            const bool wasDown = pedalIsDown (code);
            pedalSetDown (code, press);
            const bool edge = press && (kind == 3 || ! wasDown);   // program change: κάθε μήνυμα είναι νέο πάτημα
            const bool quiet = code == learnedCode.load() && juce::Time::getMillisecondCounter() < learnQuietUntil;

            for (int i = 0; i < numControls; ++i)
            {
                if (mapCode[(size_t) i].load() != code) continue;

                if (edge && ! quiet)
                {
                    flowLog ("pedal: control " + juce::String (i) + " code " + juce::String (code));
                    const int sel = selectedBlock.load();
                    const bool ok = sel >= 0 && sel < numBlocks && active[(size_t) sel].load() != 0;

                    switch (i)
                    {
                        case ctlHold:  if (ok) { const int h = holdMode[(size_t) sel].load(); holdMode[(size_t) sel] = h >= 3 ? 0 : (h + 1) % 3; ++changeCount; } break;
                        case ctlRec:   { const int t = recTarget (true); if (t >= 0) ++cmdMain[(size_t) t]; } break;
                        case ctlUndo:  { const int t = recTarget (true); if (t >= 0) pressUndo (t); } break;     // ✕ σβήσιμο λούπας, όπως το κουμπί της οθόνης
                        case ctlStop:  { const int t = recTarget (true); if (t >= 0) pressStop (t); } break;    // στοπ / play της λούπας του block, όπως το κουμπί της οθόνης
                        case ctlMute:  ++pedalMute; break;
                        case ctlSpecial: break;   // αντικαταστάθηκε από το shift (sustain CC64)
                        case ctlTape:  pressPlayStop(); break;    // Play/Stop όλων των λουπών (το Rec Audio μένει στο πλήκτρο A#0)
                        case ctlMetro: break;   // ο μετρονόμος αφαιρέθηκε
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

            uint64_t sb[2] { 0, 0 };
            for (int n = 0; n < 128; ++n)
                if (B.alSwitch[(size_t) n]) sb[n >> 6] |= (uint64_t) 1 << (n & 63);
            swBits[(size_t) b * 2] = sb[0];
            swBits[(size_t) b * 2 + 1] = sb[1];
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
            // όσο υπάρχει λούπα, το block μένει σιωπηλό στο live: ακούγεται μόνο η λούπα
            if (B.loopState == loopPlay && B.nLayers > 0 && liveSilent[(size_t) b].load() != 0)
            {
                if (on || ! B.sounding[(size_t) note]) return;
            }

            if (on)
            {
                // Follow: ένα block χωρίς λούπα που παίζει (π.χ. με arpeggiator) ακολουθεί κι αυτό τη συγχορδία
                const bool looped = B.loopState == loopPlay && B.nLayers > 0;
                const int fn = looped ? note : followMapNote (b, note);
                B.outNote[(size_t) note] = (uint8_t) juce::jlimit (0, 127, fn + transpose[(size_t) b].load());
                B.out.addEvent (juce::MidiMessage::noteOn (1, B.outNote[(size_t) note], (juce::uint8) vel), pos);
            }
            else
            {
                B.out.addEvent (juce::MidiMessage::noteOff (1, B.outNote[(size_t) note]), pos);
            }
            B.sounding.set ((size_t) note, on);

            if (autoLoop.load() != 0 && B.loopState == loopOff)
            {
                B.hist.push_back ({ curGs + pos, on, note, vel });
                if (B.hist.size() > 3000) B.hist.erase (B.hist.begin(), B.hist.begin() + 1000);
            }

            if (B.loopState == loopRec || B.loopState == loopOver)
            {
                B.rec.push_back ({ blkBeat0 + (double) pos * blkBps, on, note, vel });
                B.recOpen.set ((size_t) note, on);
            }
            else if (B.loopState == loopOff)
            {
                const double bt = blkBeat0 + (double) pos * blkBps;
                B.preRec.push_back ({ bt, on, note, vel });
                size_t cut = 0;
                while (cut < B.preRec.size() && B.preRec[cut].phase < bt - 4.0 * beatsPerBar) ++cut;   // κρατάμε μέχρι 4 μέτρα πίσω
                if (cut > 0) B.preRec.erase (B.preRec.begin(), B.preRec.begin() + (long) cut);
            }
        }

        bool isSpecialIdx (int b) const { return isSp (b); }

        void offAll (Block& B, int b, int pos)
        {
            for (int n = 0; n < 128; ++n)
                if (B.sounding[(size_t) n])
                    emitNote (B, b, pos, false, n, 0);
        }

        // Follow + Hold: όταν αλλάζει η κλίμακα-στόχος (ή ανάβει/σβήνει το Follow), οι κρατημένες νότες χωρίς arpeggiator ξαναπαίζονται στη νέα κλίμακα
        void followRetune (Block& B, int b, int hold, int arp)
        {
            const bool fol = followOn.load() != 0 && followBlock[(size_t) b].load() != 0 && ! (B.loopState == loopPlay && B.nLayers > 0);
            const int key = fol ? (followTRoot.load() | (followTType.load() << 4) | (followRoot.load() << 8) | (followType.load() << 12)) : -2;
            if (key == B.followKey) return;
            B.followKey = key;
            if (hold == 0 || arp != 0) return;      // με arpeggiator κάθε βήμα περνά ήδη από τη μεταφορά
            for (int n = 0; n < 128; ++n)
            {
                if (! B.sounding[(size_t) n] || ! B.latched[(size_t) n]) continue;
                const int want = juce::jlimit (0, 127, followMapNote (b, n) + transpose[(size_t) b].load());
                if (want == B.outNote[(size_t) n]) continue;
                const int vel = B.vel[(size_t) n] > 0 ? (int) B.vel[(size_t) n] : 100;
                emitNote (B, b, 0, false, n, 0);
                emitNote (B, b, 0, true, n, vel);
            }
        }

        // brings the sounding notes in line with what should sound now
        static std::bitset<128> targetSet (const Block& B, int hold)
        {
            if (hold == 0) return B.pressed;
            if (hold == 2) return B.latched | (B.lpSingle & B.pressed);
            if (hold == 3)   // AL Capture: ακούς ό,τι παίζεις (εκτός από το πλήκτρο που μπορεί να γίνει key switch) και τη συγχορδία που μόλις ανατέθηκε
            {
                std::bitset<128> p = B.pressed & ~B.alMute;
                if (B.alOpen && B.alHavePending) p &= ~B.alGroup;
                return p | B.latched;
            }
            if (hold == 4) return B.latched | (B.pressed & ~B.alSwitch);   // AL: τα key switches δεν ηχούν οι ίδιοι, τα άλλα πλήκτρα παίζουν κανονικά
            return B.latched;
        }

        // AL Capture: κλείνει το παράθυρο. 2+ νότες = συγχορδία που περιμένει, 1 νότα μετά από συγχορδία = key switch
        void alClose (Block& B, int b, int pos, int hold, int arp)
        {
            B.alOpen = false;
            const std::bitset<128> g = B.alGroup;
            B.alGroup.reset();
            if (g.count() >= 2)
            {
                B.alPending = g;
                B.alHavePending = true;
            }
            else if (g.count() == 1 && B.alHavePending)
            {
                const int sw = highestSet (g);
                B.alChord[(size_t) sw] = B.alPending;
                B.alSwitch.set ((size_t) sw);
                B.alMute.set ((size_t) sw);
                B.alHavePending = false;
                flowLog ("AL: block " + juce::String (b + 1) + " key " + juce::String (sw) + " = chord of " + juce::String ((int) B.alPending.count()) + " notes, assignments: " + juce::String ((int) B.alSwitch.count()));
                B.latched = B.alChord[(size_t) sw];
                B.lastLatched = highestSet (B.latched);
                for (int n = 0; n < 128; ++n)
                    if (B.latched[(size_t) n]) B.vel[(size_t) n] = B.vel[(size_t) sw] > 0 ? B.vel[(size_t) sw] : (uint8_t) 100;
            }
            updateOutput (B, b, pos, hold, arp);
        }

        // LP: κλείνει το παράθυρο συγχορδίας. 2+ νότες = νέα συγχορδία (ίδιες νότες με την κρατημένη = σβήνει),
        // 1 νότα = παίζει όσο πατιέται, χωρίς να κρατιέται.
        void lpClose (Block& B, int b, int pos, int hold, int arp)
        {
            B.lpOpen = false;
            if (B.lpGroup.count() >= 2)
            {
                if (B.lpGroup == B.latched)
                {
                    B.latched.reset();
                    B.lastLatched = -1;
                }
                else
                {
                    B.latched = B.lpGroup;
                    B.lastLatched = highestSet (B.lpGroup);
                }
            }
            else
            {
                B.lpSingle |= B.lpGroup;
            }
            B.lpGroup.reset();
            updateOutput (B, b, pos, hold, arp);
        }

        void updateOutput (Block& B, int b, int pos, int hold, int arp)
        {
            const std::bitset<128> target = targetSet (B, hold);

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
            const std::bitset<128> target = targetSet (B, hold);
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
                {
                    if (B.loopVoiceN[(size_t) n] > 0)
                        for (int v = 0; v < B.loopVoiceN[(size_t) n]; ++v)
                            B.out.addEvent (juce::MidiMessage::noteOff (1, B.loopOutV[(size_t) n][(size_t) v]), 0);
                    else
                        B.out.addEvent (juce::MidiMessage::noteOff (1, B.loopOut[(size_t) n]), 0);
                    B.loopVoiceN[(size_t) n] = 0;
                }

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
            B.stopAt = -1.0;
            cutLoopNotes (B);
            B.recOpen.reset();
            B.rec.clear();
            for (auto& l : B.layerEv) l.clear();
            B.nLayers = 0;
            B.layerMode.fill (0); B.layerStart.fill (-1.0); B.layerArm.fill (-1.0);
            B.muteMask = 0;
            B.taps = 0;
            B.stopped = false;
            B.loopBeats = 0.0;
            B.loopAnchor = 0.0;
            B.loopState = loopOff;
        }

        // κλείνει το layer που ηχογραφείται (rec) και το προσθέτει στο loop. Επιστρέφει true αν είχε νότες.
        bool commitLayer (Block& B, double beatNow, double len)
        {
            // loop: η φάση μετριέται από την αρχή της λούπας· once / hold at end: από την αρχή της δικής τους εγγραφής (παίζουν από εκεί που ξεκίνησες)
            const double anchor = (B.nLayers > 0 && B.curMode == 0) ? B.loopAnchor : B.recStart;
            const double rawElapsed = std::max (1.0e-6, beatNow - B.recStart);
            const double elapsed = std::max (1.0e-6, std::min (len, rawElapsed));
            // το μήκος στρογγυλεύτηκε στο πλησιέστερο μέτρο: αν η διαφορά είναι μέχρι ένα beat, οι νότες προσαρμόζονται αναλογικά
            // χωρίς τέντωμα: οι νότες μένουν εκεί που τις έπαιξες. Αν σταμάτησες πριν το τέλος του μέτρου, η λούπα συμπληρώνεται με παύση.
            const double warp = 1.0; juce::ignoreUnused (rawElapsed);

            for (int n = 0; n < 128; ++n)    // νότες που κρατιούνται όταν τελειώνει η ηχογράφηση σταματούν στο τέλος
                if (B.recOpen[(size_t) n])
                    B.rec.push_back ({ B.recStart + elapsed - 1.0e-4, false, n, 0 });

            const int sq = snapQ.load();
            const double grid = forceGridBeats > 0.0 ? forceGridBeats : (sq == 1 ? 0.5 : sq == 2 ? 0.25 : 0.0);
            std::array<double, 128> shift {};
            std::bitset<128> hasShift;

            auto& dst = B.layerEv[(size_t) B.nLayers];
            dst.clear();
            for (const auto& e : B.rec)
            {
                const double ph = (e.phase - B.recStart) * warp;   // νότες λίγο πριν το μέτρο (αρνητικές) πάνε στο τέλος της λούπας
                if (ph >= len) continue;
                double a = B.recStart + ph;

                if (e.isRaw)    // CC, pitch bend, aftertouch: χωρίς snap
                {
                    double rp = std::fmod (a - anchor, len);   // η λούπα ξεκινά πάντα στην αρχή του πλέγματος
                    if (rp < 0.0) rp += len;
                    dst.push_back ({ rp, true, e.note, e.vel, true, e.d2 });
                    continue;
                }

                if (grid > 0.0)
                {
                    if (e.on)
                    {
                        const double s = B.recStart + std::round ((a - B.recStart) / grid) * grid;
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
                        a = B.recStart + std::round ((a - B.recStart) / grid) * grid;
                    }
                }

                double p = std::fmod (a - anchor, len);   // φάση μέσα στη λούπα, από την αρχή της
                if (p < 0.0) p += len;
                dst.push_back ({ p, e.on, e.note, e.vel });
            }

            // Η ίδια νότα που παίζεται γρήγορα: μετά το snap το off της πρώτης μπορεί να πέσει μετά το on της δεύτερης και να την κόψει.
            // Κάθε νότα λήγει πριν ξεκινήσει η επόμενη του ίδιου ύψους.
            {
                struct Pair { int on, off; };
                std::array<int, 128> openIdx;
                openIdx.fill (-1);
                std::array<std::vector<Pair>, 128> byNote;
                for (int i = 0; i < (int) dst.size(); ++i)
                {
                    const auto& ev = dst[(size_t) i];
                    if (ev.isRaw || ev.note < 0 || ev.note > 127) continue;
                    if (ev.on) openIdx[(size_t) ev.note] = i;
                    else if (openIdx[(size_t) ev.note] >= 0) { byNote[(size_t) ev.note].push_back ({ openIdx[(size_t) ev.note], i }); openIdx[(size_t) ev.note] = -1; }
                }
                for (auto& list : byNote)
                {
                    const size_t m = list.size();
                    if (m == 0) continue;
                    std::sort (list.begin(), list.end(), [&] (const Pair& a, const Pair& b) { return dst[(size_t) a.on].phase < dst[(size_t) b.on].phase; });
                    for (size_t k = 0; k < m; ++k)
                    {
                        const double on = dst[(size_t) list[k].on].phase;
                        double gap = m == 1 ? len : std::fmod (dst[(size_t) list[(k + 1) % m].on].phase - on + len, len);
                        if (gap < 1.0e-6) gap = len;
                        double d = std::fmod (dst[(size_t) list[k].off].phase - on + len, len);
                        if (d > gap - 0.004) d = std::max (0.004, gap - 0.004);
                        dst[(size_t) list[k].off].phase = std::fmod (on + d, len);
                    }
                }
            }

            B.rec.clear();
            B.recOpen.reset();

            if (dst.empty()) return false;
            if (B.nLayers == 0) B.loopAnchor = B.recStart;
            B.layerBeats[(size_t) B.nLayers] = len;
            B.layerMode[(size_t) B.nLayers] = B.curMode;
            B.layerStart[(size_t) B.nLayers] = B.curMode != 0 ? beatNow : -1.0;   // το once/hold που μόλις ηχογράφησες παίζει αμέσως από την αρχή
            B.layerArm[(size_t) B.nLayers] = -1.0;
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

        // μήκος νέας λούπας: όλες οι λούπες πρέπει να χωράνε ακέραιες φορές στον κύκλο (τον μεγαλύτερο)
        double loopLengthFor (const Block& self, double elapsed) const
        {
            double cycle = 0.0;
            for (const auto& o : blocks)
                if (&o != &self && o.nLayers > 0)
                    cycle = std::max (cycle, o.loopBeats);

            if (cycle <= 0.0 || loopFit.load() == 0) return quantizedLength (elapsed);   // πρώτη λούπα ορίζει το πλέγμα· Free: κρατά το μήκος που παίχτηκε

            const int cBars = std::max (1, (int) std::lround (cycle / beatsPerBar));
            const double barsE = elapsed / beatsPerBar;

            if (barsE >= (double) cBars)       // μεγαλύτερη από τον κύκλο: ακέραιο πολλαπλάσιο του
            {
                const int maxN = std::max (1, maxLoopBars / cBars);
                const int n = juce::jlimit (1, maxN, (int) std::lround (barsE / (double) cBars));
                return (double) (n * cBars) * beatsPerBar;
            }

            int best = cBars;                  // μικρότερη: το κοντινότερο μήκος που χωράει ακριβώς στον κύκλο
            double bestD = 1.0e9;
            for (int d = cBars; d >= 1; --d)
            {
                if (cBars % d != 0) continue;
                const double dd = std::abs ((double) d - barsE);
                if (dd < bestD - 1.0e-9) { bestD = dd; best = d; }
            }
            return (double) best * beatsPerBar;
        }

        // όταν τελειώνει η εγγραφή λούπας, το latch (LM / LP / AL) του block σβήνει: παίζει μόνο η λούπα
        void holdOffAfterLoop (Block& B)
        {
            const int idx = (int) (&B - &blocks[0]);
            if (idx >= 0 && idx < numBlocks)
                holdMode[(size_t) idx] = 0;
        }

        void finishRecording (Block& B, double beatNow, bool allowOverdub = true)
        {
            // η λούπα ξεκινά στην αρχή του grid (εκεί που ξεκίνησε η εγγραφή): η σιωπή πριν την πρώτη νότα μένει και το τέλος της οδηγεί πάλι στην αρχή

            const double elapsed = beatNow - B.recStart;
            if (elapsed <= 0.0) { stopLoop (B); return; }   // ακυρώθηκε πριν αρχίσει (start quantize)

            const double beats = loopLengthFor (B, elapsed);
            juce::ignoreUnused (allowOverdub);

            if (commitLayer (B, beatNow, beats))
            {
                B.loopState = loopPlay;
                holdOffAfterLoop (B);
            }
            else
            {
                stopLoop (B);
            }
        }

        // κάθε layer έχει δικό του ελεύθερο μήκος: αν είναι μεγαλύτερο από το loop, το loop μεγαλώνει και τα άλλα επαναλαμβάνονται
        void finishOverdub (Block& B, double beatNow)
        {
            // κάθε στρώμα έχει το δικό του μήκος (π.χ. 1ο = 2 μέτρα, 2ο = 4 μέτρα), όπως και η πρώτη λούπα. Το μεγαλύτερο ορίζει τον κύκλο του block.
            const double elapsed = std::max (1.0e-6, beatNow - B.recStart);
            commitLayer (B, beatNow, loopLengthFor (B, elapsed));
            B.stopAt = -1.0;
            B.loopState = loopPlay;
        }

        void loopMain (Block& B, double beat)
        {
            switch (B.loopState)
            {
                case loopOff:
                {
                    // η εγγραφή ξεκινά ακριβώς στο πάτημα (Rec MIDI loop): εκεί είναι η αρχή της λούπας και το τέλος της οδηγεί πάλι εκεί
                    B.curMode = B.wantMode;
                    B.recStart = beat;
                    B.rec.clear();
                    B.recOpen.reset();
                    for (const auto& pe : B.preRec)   // ό,τι έπαιξες από την αρχή του κύκλου μπαίνει στη λούπα
                        if (pe.phase >= B.recStart - 1.0e-9 && pe.phase < beat)
                        {
                            B.rec.push_back (pe);
                            B.recOpen.set ((size_t) pe.note, pe.on);
                        }
                    B.preRec.clear();
                    B.loopState = loopRec;
                    break;
                }
                case loopRec:
                {
                    // το τέλος περιμένει την επόμενη γραμμή μέτρου. Αν πατήσεις λίγο μετά τη γραμμή (μέχρι μισό beat), κλείνει στη γραμμή που μόλις πέρασε.
                    const double prev = B.recStart + std::floor ((beat - B.recStart) / beatsPerBar + 1.0e-9) * beatsPerBar;   // μέτρα από την αρχή της λούπας
                    if (B.stopAt >= 0.0 || beat - prev <= 0.5) { B.stopAt = -1.0; finishRecording (B, beat); }
                    else B.stopAt = prev + beatsPerBar;
                    break;
                }
                case loopPlay:
                    if (B.nLayers < maxOverdubLayers)   // overdub: μέχρι 3 στρώματα συνολικά, το καθένα με δικό του μήκος και τρόπο επανάληψης
                    {
                        B.curMode = B.wantMode;
                        B.recStart = beat;
                        B.rec.clear();
                        B.recOpen.reset();
                        B.loopState = loopOver;
                    }
                    break;
                case loopOver:
                {
                    // όπως στην πρώτη λούπα: το τέλος περιμένει την επόμενη γραμμή μέτρου (πάτημα λίγο μετά τη γραμμή, μέχρι μισό beat, κλείνει σε αυτήν)
                    const double prev = B.recStart + std::floor ((beat - B.recStart) / beatsPerBar + 1.0e-9) * beatsPerBar;
                    if (B.stopAt >= 0.0 || beat - prev <= 0.5) { B.stopAt = -1.0; finishOverdub (B, beat); }
                    else B.stopAt = prev + beatsPerBar;
                    break;
                }
                default: break;
            }
        }

        // σβήσιμο της λούπας του block (κάθε block έχει μία λούπα)
        void loopUndo (Block& B)
        {
            if (B.loopState == loopOff) return;
            cutLoopNotes (B);
            stopLoop (B);
            bool any = false;
            for (const auto& o : blocks) if (o.nLayers > 0) any = true;
            if (! any) tempoAdopted = false;   // χωρίς λούπες, το επόμενο loop μπορεί να ορίσει νέο πλέγμα
        }

        int64_t curGs = 0;
        double pendingBeatShift = 0.0;
        double forceGridBeats = 0.0;
        bool tempoAdopted = false;

        int pedalAct[numBlocks] = {};   // 1 σίγαση, 2 ενεργοποίηση, 3 re-record, 4 διαγραφή (από πετάλι, στο τελευταίο block που έπαιξες)
        int seenPM = 0, seenPD = 0, tapsM = 0, tapsD = 0;
        int64_t lastTapM = 0, lastTapD = 0;

        // πετάλι mute / σβήσιμο: N γρήγορα πατήματα = block N. Δρα όταν περάσει η αναμονή από το τελευταίο πάτημα.
        void processPedalTaps (int64_t gs, double sr)
        {
            const int64_t win = (int64_t) (sr * 0.001 * (double) tapMs.load());

            auto handle = [&] (std::atomic<int>& counter, int& seen, int& taps, int64_t& lastTap, bool del, std::atomic<int>& out)
            {
                for (int c = counter.load(); seen != c;)
                {
                    ++seen;
                    if (gs - lastTap > win) taps = 0;
                    ++taps;
                    lastTap = gs;
                }
                if (taps > 0 && gs - lastTap > win)
                {
                    int blk = lastPlayed.load();
                    if (blk < 0 || blk >= numBlocks || active[(size_t) blk].load() == 0) blk = selectedBlock.load();
                    if (blk >= 0 && blk < numBlocks && active[(size_t) blk].load() != 0)
                    {
                        if (del)
                            pedalAct[blk] = taps == 1 ? 3 : 4;
                        else if (loopStateOut[(size_t) blk].load() != loopOff)
                            pedalAct[blk] = taps == 1 ? 1 : 2;
                    }
                    taps = 0;
                }
                out = taps;
            };

            handle (pedalMute, seenPM, tapsM, lastTapM, false, tapsMuteOut);
            handle (pedalDel,  seenPD, tapsD, lastTapD, true,  tapsDelOut);
        }


        //==================== ανίχνευση επανάληψης (αυτόματη λούπα) ====================
        static bool hasNear (const std::vector<double>& v, double t, double tol)
        {
            for (double x : v)
                if (std::abs (x - t) <= tol) return true;
            return false;
        }

        // Ψάχνει αν οι τελευταίες νότες του block επαναλαμβάνονται σε σταθερό "μέτρο" D για 4 μέτρα.
        // Συγκρίνει μόνο τους χρόνους των νοτών (όχι το ύψος): μπάσο LM που αλλάζει νότα κάθε μέτρο είναι σταθερός ρυθμός.
        bool detectLoop (Block& B, int64_t nowS, double sr, AutoHit& out)
        {
            std::vector<double> on;
            for (const auto& e : B.hist)
            {
                if (! e.on) continue;
                const double t = (double) e.t / sr;
                if (on.empty() || t - on.back() > 0.035) on.push_back (t);   // συγχορδία = ένα onset
            }
            if (on.size() < 4) return false;

            const double now = (double) nowS / sr;
            const double tL = on.back();
            if (now - tL > 1.5) return false;                                // δεν παίζεις πια

            std::vector<double> iois;
            for (size_t i = 1; i < on.size(); ++i) iois.push_back (on[i] - on[i - 1]);
            std::sort (iois.begin(), iois.end());
            const double medIoi = iois[iois.size() / 2];

            // Αρχή του παραθύρου 4 μέτρων: τα όρια των μέτρων πέφτουν στο μεγαλύτερο κενό ανάμεσα στις νότες (όχι πάνω σε νότα),
            // ώστε μια νότα που επαναλαμβάνεται σε κάθε μέτρο να μην πέφτει πότε στο ένα και πότε στο διπλανό μέτρο.
            auto windowStart = [&] (double D) -> double
            {
                std::vector<double> ph;
                for (double t : on)
                    if (t >= tL - 5.0 * D) { double p = std::fmod (tL - t, D); if (p < 0.0) p += D; ph.push_back (p); }
                std::sort (ph.begin(), ph.end());
                double c = 0.5 * D, bestGap = -1.0;
                for (size_t i = 0; i < ph.size(); ++i)
                {
                    const double a = ph[i], b = i + 1 < ph.size() ? ph[i + 1] : ph[0] + D;
                    if (b - a > bestGap) { bestGap = b - a; c = std::fmod (0.5 * (a + b), D); }
                }
                return tL - c - 4.0 * D;   // τέλος παραθύρου = το τελευταίο όριο μέτρου πριν την τελευταία νότα
            };

            const bool linkActive = linkWanted.load() && link.isEnabled();
            const bool known = tempoUserSet.load() || tempoAdopted || linkActive;
            const double D0 = 240.0 / juce::jlimit (40.0, 240.0, tempo.load());
            const double Dref = known ? D0 : 2.0;

            std::vector<double> cands;
            if (known)
            {
                for (double m : { 0.5, 1.0, 2.0 })
                    for (int i = -8; i <= 8; ++i)
                        cands.push_back (D0 * m * (1.0 + 0.015 * (double) i));
            }
            else
            {
                for (double D = 0.9; D <= 6.0; D *= 1.015) cands.push_back (D);
            }

            std::vector<char> pass (cands.size(), 0);

            for (size_t ci = 0; ci < cands.size(); ++ci)
            {
                const double D = cands[ci];
                const double tol = std::min (0.06 * D + 0.03, std::max (0.03, 0.4 * medIoi));
                if (on.front() > tL - 3.0 * D + tol) continue;               // δεν υπάρχουν δεδομένα για 4 μέτρα

                const double ws = windowStart (D);
                std::array<std::vector<double>, 4> seg;
                for (double t : on)
                {
                    if (t < ws - tol || t >= ws + 4.0 * D - tol) continue;
                    seg[(size_t) juce::jlimit (0, 3, (int) std::floor ((t - ws) / D))].push_back (t);
                }
                if (seg[0].empty() || seg[1].empty() || seg[2].empty() || seg[3].empty()) continue;

                // μόνο γρήγορο παίξιμο: τυπικό διάστημα ανάμεσα στις νότες το πολύ 1/8 (D/8), με μικρή ανοχή
                {
                    std::vector<double> w;
                    for (double t : on) if (t >= ws - tol && t <= tL) w.push_back (t);
                    std::vector<double> gaps;
                    for (size_t i = 1; i < w.size(); ++i) gaps.push_back (w[i] - w[i - 1]);
                    if (gaps.empty()) continue;
                    std::sort (gaps.begin(), gaps.end());
                    if (gaps[gaps.size() / 2] > D / 8.0 * 1.3) continue;
                }

                int fT = 0, fOk = 0, bT = 0, bOk = 0;
                for (size_t k = 1; k < 4; ++k)
                    for (double t : seg[k]) { ++fT; if (hasNear (seg[k - 1], t - D, tol)) ++fOk; }
                for (size_t k = 0; k < 3; ++k)
                    for (double t : seg[k]) { ++bT; if (hasNear (seg[k + 1], t + D, tol)) ++bOk; }

                const double score = std::min ((double) fOk / (double) std::max (1, fT), (double) bOk / (double) std::max (1, bT));
                pass[ci] = score >= 0.85 ? 1 : 0;
            }

            // ομάδες διαδοχικών υποψηφίων που περνούν: κρατάμε την κοντινότερη στο αναμενόμενο μέτρο
            double bestD = 0.0, bestDist = 1.0e9;
            for (size_t i = 0; i < cands.size();)
            {
                if (! pass[i]) { ++i; continue; }
                size_t j = i;
                double sum = 0.0;
                while (j < cands.size() && pass[j] && (j == i || std::abs (cands[j] / cands[j - 1] - 1.0) < 0.03)) { sum += cands[j]; ++j; }
                const double centre = sum / (double) (j - i);
                const double dist = std::abs (std::log (centre / Dref));
                if (dist < bestDist) { bestDist = dist; bestD = centre; }
                i = j;
            }
            if (bestD <= 0.0) return false;

            // πρώτο πλήκτρο του μέτρου: η φάση μετά το μεγαλύτερο κενό
            const double D = bestD;
            const double tol = std::min (0.06 * D + 0.03, std::max (0.03, 0.4 * medIoi));
            const double ws = windowStart (D);
            std::vector<double> ph;
            for (double t : on)
            {
                if (t < ws - tol || t >= ws + 4.0 * D - tol) continue;
                double p = std::fmod (t - ws, D);
                if (p < 0.0) p += D;
                ph.push_back (p / D);
            }
            if (ph.empty()) return false;
            std::sort (ph.begin(), ph.end());
            double bestGap = ph.front() + 1.0 - ph.back();
            double dbPhase = ph.front();
            for (size_t i = 1; i < ph.size(); ++i)
                if (ph[i] - ph[i - 1] > bestGap) { bestGap = ph[i] - ph[i - 1]; dbPhase = ph[i]; }

            const double tDown = ws + dbPhase * D;
            const int k = (int) std::floor ((now + 0.06 - 4.0 * D - tDown) / D);
            if (k < 0) return false;

            out.D = D;
            out.tStart = (int64_t) std::llround ((tDown + (double) k * D) * sr);
            return true;
        }

        // φτιάχνει τη λούπα από τις τελευταίες 4 μέρες νοτών. Αν το tempo είναι ελεύθερο, το tempo και το πλέγμα προσαρμόζονται στο παίξιμό σου (μία φορά).
        void commitAuto (Block& B, int b, const AutoHit& hit, int64_t nowS, double sr, double beatAtNow)
        {
            const double D = hit.D;
            bool otherLoops = false;
            for (const auto& o : blocks)
                if (&o != &B && o.nLayers > 0) otherLoops = true;

            const bool linkActive = linkWanted.load() && link.isEnabled();
            const bool adopt = ! tempoUserSet.load() && ! tempoAdopted && ! otherLoops && ! linkActive;

            double clockBps = blkBps;   // beats ανά sample του ρολογιού μετά την όποια αλλαγή
            if (adopt)
            {
                const double bpm = juce::jlimit (40.0, 240.0, 240.0 / D);
                tempo = bpm;
                tempoAdopted = true;
                ++changeCount;
                clockBps = bpm / (60.0 * sr);
            }

            // beat στο οποίο ξεκίνησε το μέτρο που διαλέξαμε
            double bStart = beatAtNow - (double) (nowS - hit.tStart) * clockBps;
            const double bAligned = std::round (bStart / beatsPerBar) * beatsPerBar;
            const double delta = bAligned - bStart;
            if (adopt) pendingBeatShift += delta;   // το ρολόι ευθυγραμμίζεται στο πρώτο σου πλήκτρο
            const double bStartFinal = bAligned;

            const double len = loopLengthFor (B, 4.0 * beatsPerBar);
            const double winSec = (len / (4.0 * beatsPerBar)) * 4.0 * D;
            const double tStartSec = (double) hit.tStart / sr;
            const double beatsPerSec = (4.0 * beatsPerBar) / (4.0 * D);   // 16 beats ανά 4D
            const double edge = 0.05;

            B.rec.clear();
            B.recOpen.reset();
            std::bitset<128> open;
            for (const auto& e : B.hist)
            {
                const double rel = (double) e.t / sr - tStartSec;
                if (e.on)
                {
                    if (rel < -edge || rel >= winSec - edge) continue;
                    B.rec.push_back ({ bStartFinal + rel * beatsPerSec, true, e.note, e.vel });
                    open.set ((size_t) e.note);
                }
                else if (open[(size_t) e.note] && rel <= winSec)
                {
                    B.rec.push_back ({ bStartFinal + rel * beatsPerSec, false, e.note, 0 });
                    open.reset ((size_t) e.note);
                }
            }
            B.recOpen = open;
            B.recStart = bStartFinal;

            const int sq = snapQ.load();
            forceGridBeats = sq == 1 ? 0.5 : 0.25;
            const bool ok = commitLayer (B, bStartFinal + len, len);
            forceGridBeats = 0.0;

            offAll (B, b, 0);   // οι νότες που ακούγονται στο block σβήνουν: από εδώ και πέρα παίζει μόνο η λούπα
            B.hist.clear();
            B.autoReady = false;
            autoReadyOut[(size_t) b] = 0;
            if (ok)
            {
                B.loopState = loopPlay;
                holdOffAfterLoop (B);
            }
            else
                stopLoop (B);

            flowLog ("auto loop: block " + juce::String (b + 1) + " D=" + juce::String (D, 3) + " s, adopt=" + juce::String (adopt ? 1 : 0)
                     + ", len=" + juce::String (len, 1) + " beats, ok=" + juce::String (ok ? 1 : 0));
        }

        void autoLoopStep (Block& B, int b, int64_t nowS, double sr, double beatAtNow)
        {
            if (nowS - B.lastDetect < (int64_t) (0.1 * sr)) return;
            B.lastDetect = nowS;

            const int64_t oldest = nowS - (int64_t) (25.0 * sr);
            size_t cut = 0;
            while (cut < B.hist.size() && B.hist[cut].t < oldest) ++cut;
            if (cut > 0) B.hist.erase (B.hist.begin(), B.hist.begin() + (long) cut);

            AutoHit hit;
            const bool found = detectLoop (B, nowS, sr, hit);
            B.autoReady = found;
            autoReadyOut[(size_t) b] = found ? 1 : 0;
            if (found)
            {
                B.autoHit = hit;
                if (autoLoop.load() == 1)
                    commitAuto (B, b, hit, nowS, sr, beatAtNow);
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
            B.lpGroup.reset(); B.lpSingle.reset(); B.lpOpen = false;
            B.lastLatched = -1;
            B.arpIndex = 0;
            B.panicPending = true;
            juce::ignoreUnused (b);
        }

        void runBlock (int b, int64_t gs, int numSamples, double sr)
        {
            Block& B = blocks[(size_t) b];
            B.out.clear();
            curGs = gs;

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

            if (hold != 2 && (B.lpOpen || B.lpSingle.any()))
            {
                B.lpOpen = false; B.lpGroup.reset(); B.lpSingle.reset();
            }
            if (hold != 3 && (B.alOpen || B.alHavePending || B.alMute.any()))
            {
                B.alOpen = false; B.alGroup.reset(); B.alHavePending = false; B.alPending.reset(); B.alMute.reset();
            }

            if (hold != B.prevHold)
            {
                if (hold == 0)
                {
                    B.latched.reset();
                    B.lastLatched = -1;
                }
                else if (hold >= 3)
                {
                    B.latched.reset();   // AL: ξεκινά χωρίς κρατημένες νότες
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
            followRetune (B, b, hold, arp);
            if (changed) updateOutput (B, b, 0, hold, arp);

            // ---- loop: εντολές (κουμπιά ή πετάλια)
            const int64_t tapWindow = (int64_t) (sr * 0.7);

            if (B.stopAt >= 0.0 && (B.loopState == loopRec || B.loopState == loopOver) && beat1 >= B.stopAt - 1.0e-9)
            {
                const double t = B.stopAt;
                B.stopAt = -1.0;
                if (B.loopState == loopRec) finishRecording (B, t);
                else finishOverdub (B, t);
            }

            rerecReq[(size_t) b] = 0;

            for (int cm = cmdMain[(size_t) b].load(); B.seenMain != cm; )
            {
                ++B.seenMain;
                if (B.autoPaused) { B.autoPaused = false; B.hist.clear(); B.lastDetect = 0; }
                B.wantMode = juce::jlimit (0, 2, repeatMode[(size_t) b].load());
                loopMain (B, beat0);
            }

            {   // ✕: ένα πάτημα = σβήσιμο της λούπας, γρήγορο διπλό πάτημα = σβήσιμο μόνο του overdub (του τελευταίου στρώματος)
                const int64_t uwin = (int64_t) (sr * 0.001 * (double) tapMs.load());
                for (int cu = cmdUndo[(size_t) b].load(); B.seenUndo != cu; )
                {
                    ++B.seenUndo;
                    if (gs - B.undoLastT > uwin) B.undoTaps = 0;
                    ++B.undoTaps;
                    B.undoLastT = gs;
                }
                if (B.undoTaps > 0 && gs - B.undoLastT > uwin)
                {
                    if (B.undoTaps >= 2 && B.loopState == loopPlay && B.nLayers > 1)
                    {
                        cutLoopNotes (B);
                        --B.nLayers;
                        B.layerEv[(size_t) B.nLayers].clear();
                        B.layerBeats[(size_t) B.nLayers] = 0.0;
                        B.layerMode[(size_t) B.nLayers] = 0; B.layerStart[(size_t) B.nLayers] = -1.0; B.layerArm[(size_t) B.nLayers] = -1.0;
                        B.muteMask &= ~(1u << B.nLayers);
                        B.loopBeats = 0.0;
                        for (int l = 0; l < B.nLayers; ++l) B.loopBeats = std::max (B.loopBeats, B.layerBeats[(size_t) l]);
                    }
                    else if (B.loopState == loopOver)   // σβήσιμο όσο γράφεται overdub: το αναιρεί, η λούπα συνεχίζει
                    {
                        B.rec.clear(); B.recOpen.reset(); B.loopState = loopPlay;
                    }
                    else
                    {
                        loopUndo (B);    // σβήσιμο της λούπας του block
                        B.hist.clear(); B.autoReady = false; B.autoPaused = true;
                    }
                    B.undoTaps = 0;
                }
            }

            for (int cm = cmdMute[(size_t) b].load(); B.seenMute != cm; )
            {
                ++B.seenMute;
                if (B.loopState == loopPlay && B.nLayers > 0)
                {
                    B.muteMask ^= 1u;    // mute / ενεργοποίηση: η λούπα τρέχει σιωπηλά και γυρίζει συγχρονισμένη
                    cutLoopNotes (B);
                }
            }
            if (B.seenPlayAll != playAllGen.load()) { B.seenPlayAll = playAllGen.load(); B.userStopped = false; }
            for (int cs = cmdStop[(size_t) b].load(); B.seenStop != cs; ) { ++B.seenStop; B.userStopped = ! B.userStopped; }   // στοπ / play της λούπας του block
            {   // σταματούν με κομμένες νότες και ξαναμπαίνουν στη φάση τους (κουμπί του block ή Play/Stop όλων)
                const bool st = B.userStopped || loopsStopped.load() != 0;
                if (st != B.stopped)
                {
                    B.stopped = st;
                    if (st) cutLoopNotes (B);
                    for (int l = 0; l < B.nLayers; ++l)   // once / hold: στο στοπ σταματούν· στο play ξαναπαίζουν από την αρχή στο επόμενο μέτρο
                        if (B.layerMode[(size_t) l] != 0) { B.layerStart[(size_t) l] = -1.0; B.layerArm[(size_t) l] = st ? -1.0 : -2.0; }
                }
            }
            juce::ignoreUnused (tapWindow);

            if (B.loopState == loopRec && (beat0 - B.recStart) >= maxLoopBars * beatsPerBar)
                finishRecording (B, beat0);
            else if (B.loopState == loopOver && (beat0 - B.recStart) >= (double) maxLoopBars * beatsPerBar)
                finishOverdub (B, beat0);

            {   // once / hold at end: εκκινήσεις (στο επόμενο μέτρο) και αλλαγή συγχορδίας
                const int cg = chordGen.load();
                const bool chordChanged = cg != B.seenChord;
                B.seenChord = cg;
                if ((B.loopState == loopPlay || B.loopState == loopOver) && B.loopBeats > 0.0 && ! B.stopped)
                {
                    bool cut = false;
                    for (int l = 0; l < B.nLayers; ++l)
                    {
                        const auto ul = (size_t) l;
                        if (B.layerMode[ul] == 0) continue;
                        if (chordChanged && B.layerMode[ul] == 2 && ((B.muteMask >> l) & 1u) == 0) B.layerArm[ul] = -2.0;
                        if (B.layerArm[ul] == -2.0)
                        {
                            const double k = std::ceil ((beat0 - B.loopAnchor) / beatsPerBar - 1.0e-9);
                            B.layerArm[ul] = B.loopAnchor + k * beatsPerBar;
                        }
                        if (B.layerArm[ul] >= 0.0 && B.layerArm[ul] < beat1)
                        {
                            if (B.layerStart[ul] >= 0.0) cut = true;   // ξαναρχίζει ενώ έπαιζε: κόβονται οι νότες που ηχούν
                            B.layerStart[ul] = B.layerArm[ul];
                            B.layerArm[ul] = -1.0;
                        }
                    }
                    if (cut) cutLoopNotes (B);
                }
            }

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
                    const auto ul = (size_t) layer;
                    const double L = B.layerBeats[ul] > 0.0 ? B.layerBeats[ul] : L0;
                    const int age = B.nLayers - 1 - layer;                         // 0 = το νεότερο layer
                    const float lgain = age == 0 ? 1.0f : age == 1 ? 0.75f : 0.55f;   // το πιο πρόσφατο ακούγεται περισσότερο, τα παλιότερα πιο πίσω
                    auto push = [&] (double x, const LoopEvent& e)
                    {
                        Action a;
                        a.pos = juce::jlimit (0, numSamples - 1, (int) ((x - beat0) / blkBps));
                        a.order = 3; a.kind = Kind::LoopEvt;
                        a.note = e.note; a.on = e.on; a.rawLoop = e.isRaw; a.d2 = e.d2;
                        a.vel = (e.on && ! e.isRaw) ? juce::jmax (1, (int) std::lround ((float) e.vel * lgain)) : e.vel;
                        actions.push_back (a);
                    };

                    if (B.layerMode[ul] == 0)   // loop: συνεχής επανάληψη
                    {
                        for (const auto& e : B.layerEv[ul])
                            for (double m = std::floor ((beat0 - B.loopAnchor) / L); m <= std::floor ((beat1 - 1.0e-9 - B.loopAnchor) / L); m += 1.0)
                            {
                                const double x = B.loopAnchor + m * L + e.phase;
                                if (x >= beat0 && x < beat1) push (x, e);
                            }
                    }
                    else if (B.layerStart[ul] >= 0.0)   // once / hold at end: παίζει μία φορά από την αρχή του και μετά περιμένει
                    {
                        for (const auto& e : B.layerEv[ul])
                        {
                            const double x = B.layerStart[ul] + e.phase;
                            if (x >= beat0 && x < beat1) push (x, e);
                        }
                        if (beat1 >= B.layerStart[ul] + L - 1.0e-9) B.layerStart[ul] = -1.0;   // τελείωσε: σιωπή μέχρι νέα εκκίνηση
                    }
                }
            }

            std::stable_sort (actions.begin(), actions.end(), [] (const Action& x, const Action& y)
            {
                return x.pos != y.pos ? x.pos < y.pos : x.order < y.order;
            });

            // ---- run them in time order
            const int64_t lpWindow = (int64_t) (sr * 0.030);

            for (const auto& a : actions)
            {
                // LP: αν το παράθυρο συγχορδίας έχει λήξει πριν από αυτό το γεγονός, κλείνει πρώτα
                if (hold == 2 && B.lpOpen && gs + a.pos >= B.lpEnd)
                    lpClose (B, b, (int) std::max<int64_t> (0, B.lpEnd - gs), hold, arp);
                if (hold == 3 && B.alOpen && gs + a.pos >= B.alEnd)
                    alClose (B, b, (int) std::max<int64_t> (0, B.alEnd - gs), hold, arp);

                switch (a.kind)
                {
                    case Kind::Live:
                        if (a.on)
                        {
                            B.pressed.set ((size_t) a.note);
                            B.vel[(size_t) a.note] = (uint8_t) a.vel;

                            if (hold == 2)   // LP: οι νότες που πατιούνται μέσα σε 30 ms γίνονται μία συγχορδία (2+ νότες), η παλιά μένει ώσπου να είναι έτοιμη η νέα
                            {
                                if (! B.lpOpen)
                                {
                                    B.lpOpen = true;
                                    B.lpGroup.reset();
                                    B.lpEnd = gs + a.pos + lpWindow;
                                }
                                B.lpGroup.set ((size_t) a.note);
                                B.lpSingle.reset ((size_t) a.note);
                            }
                            else if (hold == 3)   // AL Capture
                            {
                                if (! B.alOpen)
                                {
                                    B.alOpen = true;
                                    B.alGroup.reset();
                                    B.alEnd = gs + a.pos + lpWindow;
                                    B.latched.reset();   // τέλος της "ακρόασης" της προηγούμενης ανάθεσης
                                }
                                B.alGroup.set ((size_t) a.note);
                            }
                            else if (hold == 4 && B.alSwitch[(size_t) a.note])   // AL: key switch
                            {
                                const auto& chord = B.alChord[(size_t) a.note];
                                if (B.latched == chord)
                                {
                                    B.latched.reset();
                                    B.lastLatched = -1;
                                }
                                else
                                {
                                    B.latched = chord;
                                    B.lastLatched = highestSet (chord);
                                    for (int n = 0; n < 128; ++n)
                                        if (chord[(size_t) n]) B.vel[(size_t) n] = (uint8_t) a.vel;
                                }
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
                            B.alMute.reset ((size_t) a.note);
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
                        ++loopEvtCount;   // η λούπα παίζει νότες: το Komplete Kontrol plugin γράφει φώτα, το Flow τα ξαναστέλνει
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
                            const bool poly = followPolyBlock[(size_t) b].load() != 0 && followOn.load() != 0 && followBlock[(size_t) b].load() != 0
                                              && b != mfBlock.load() && followVoiceN.load() > 0;
                            if (poly)
                            {
                                const int nv = juce::jlimit (1, maxPolyVoices, followVoiceN.load());
                                const int vel = juce::jlimit (1, 127, (int) std::lround ((float) a.vel / std::sqrt ((float) nv)));
                                int cnt = 0;
                                for (int v = 0; v < nv; ++v)
                                {
                                    const int o = juce::jlimit (0, 127, followMapNoteTo (a.note, followRoot.load(), followType.load(), followVoiceRoot[(size_t) v].load(), followType.load()) + transpose[(size_t) b].load());
                                    bool dup = false;
                                    for (int k = 0; k < cnt; ++k) dup = dup || B.loopOutV[(size_t) a.note][(size_t) k] == o;
                                    if (dup) continue;
                                    B.loopOutV[(size_t) a.note][(size_t) cnt++] = (uint8_t) o;
                                    B.out.addEvent (juce::MidiMessage::noteOn (1, o, (juce::uint8) vel), a.pos);
                                }
                                B.loopVoiceN[(size_t) a.note] = (uint8_t) cnt;
                            }
                            else
                            {
                                B.loopVoiceN[(size_t) a.note] = 0;
                                B.loopOut[(size_t) a.note] = (uint8_t) juce::jlimit (0, 127, followMapNote (b, a.note) + transpose[(size_t) b].load());
                                B.out.addEvent (juce::MidiMessage::noteOn (1, B.loopOut[(size_t) a.note], (juce::uint8) a.vel), a.pos);
                            }
                        }
                        else
                        {
                            if (B.loopVoiceN[(size_t) a.note] > 0)
                            {
                                for (int v = 0; v < B.loopVoiceN[(size_t) a.note]; ++v)
                                    B.out.addEvent (juce::MidiMessage::noteOff (1, B.loopOutV[(size_t) a.note][(size_t) v]), a.pos);
                                B.loopVoiceN[(size_t) a.note] = 0;
                            }
                            else
                                B.out.addEvent (juce::MidiMessage::noteOff (1, B.loopOut[(size_t) a.note]), a.pos);
                        }
                        B.loopSounding.set ((size_t) a.note, a.on);
                        break;
                }
            }

            if (hold == 2 && B.lpOpen && B.lpEnd < ge)
                lpClose (B, b, (int) juce::jlimit<int64_t> (0, numSamples - 1, B.lpEnd - gs), hold, arp);
            if (hold == 3 && B.alOpen && B.alEnd < ge)
                alClose (B, b, (int) juce::jlimit<int64_t> (0, numSamples - 1, B.alEnd - gs), hold, arp);

            if (autoLoop.load() != 0 && B.loopState == loopOff && arp == 0 && ! B.autoPaused)
                autoLoopStep (B, b, gs + numSamples, sr, beat1);
            else
            {
                if (! B.hist.empty()) B.hist.clear();
                B.autoReady = false;
                autoReadyOut[(size_t) b] = 0;
            }

            autoPausedOut[(size_t) b] = B.autoPaused ? 1 : 0;
            loopStateOut[b] = B.loopState;
            loopBeatsOut[b] = (B.loopState == loopPlay || B.loopState == loopOver) ? B.loopBeats : 0.0;
            loopAnchorOut[b] = B.loopAnchor;
            loopLayersOut[b] = B.nLayers;
            loopMuteOut[b]   = (int) B.muteMask;
            loopTapsOut[b]   = B.taps;
            loopStoppedOut[b] = B.stopped ? 1 : 0;
            recStartBeatOut[b] = B.recStart;

        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Engine)
    };
}
