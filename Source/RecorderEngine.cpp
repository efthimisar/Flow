#include "RecorderEngine.h"
#include <cmath>

// ---------------------------------------------------------------- WAV 24-bit με κεφαλίδα που ενημερώνεται τακτικά
class RecorderEngine::WavWriter
{
public:
    WavWriter (const juce::File& f, double sr) : stream (f), sampleRate ((uint32_t) std::lround (sr))
    {
        if (stream.failedToOpen()) return;
        stream.setPosition (0);
        stream.truncate();
        writeHeader (0);
    }

    bool ok() const { return ! stream.failedToOpen() && stream.getStatus().wasOk(); }

    bool write (const float* l, const float* r, int n)
    {
        bytes.ensureSize ((size_t) n * 6, false);
        auto* p = static_cast<uint8_t*> (bytes.getData());
        auto conv = [&p] (float v)
        {
            const float c = juce::jlimit (-1.0f, 1.0f, v);
            const int32_t s = (int32_t) std::lrintf (c * 8388607.0f);
            *p++ = (uint8_t) (s & 0xff); *p++ = (uint8_t) ((s >> 8) & 0xff); *p++ = (uint8_t) ((s >> 16) & 0xff);
        };
        for (int i = 0; i < n; ++i) { conv (l[i]); conv (r[i]); }
        if (! stream.write (bytes.getData(), (size_t) n * 6)) return false;
        dataBytes += (uint64_t) n * 6;
        return true;
    }

    // γράφει τα πραγματικά μεγέθη στην κεφαλίδα (ασφαλές ακόμη κι αν το host κρασάρει μετά)
    bool sync()
    {
        const auto pos = stream.getPosition();
        const bool a = writeHeader ((uint32_t) juce::jmin<uint64_t> (dataBytes, 0xffffffffu - 36));
        const bool b = stream.setPosition (pos);
        stream.flush();
        return a && b && stream.getStatus().wasOk();
    }

private:
    bool writeHeader (uint32_t dataSize)
    {
        if (! stream.setPosition (0)) return false;
        stream.write ("RIFF", 4);  stream.writeInt (36 + (int) dataSize);
        stream.write ("WAVE", 4);  stream.write ("fmt ", 4);
        stream.writeInt (16);      stream.writeShort (1);          // PCM
        stream.writeShort (2);     stream.writeInt ((int) sampleRate);
        stream.writeInt ((int) (sampleRate * 6)); stream.writeShort (6); stream.writeShort (24);
        stream.write ("data", 4);  stream.writeInt ((int) dataSize);
        return stream.getStatus().wasOk();
    }

    juce::FileOutputStream stream;
    uint32_t sampleRate;
    uint64_t dataBytes = 0;
    juce::MemoryBlock bytes;
};

// ---------------------------------------------------------------- RecorderEngine
RecorderEngine::RecorderEngine() : juce::Thread ("TapeRecorderWriter") {}

RecorderEngine::~RecorderEngine()
{
    stopWorker();
    if (writer != nullptr) finishTake (false);
}

void RecorderEngine::startWorker() { startThread (juce::Thread::Priority::normal); }
void RecorderEngine::stopWorker()  { stopThread (3000); }

void RecorderEngine::prepare (double sr)
{
    if (prepared.load() && std::abs (sr - sampleRate.load()) < 0.5) return;   // ίδιο sample rate: συνεχίζουμε

    const bool wasPrepared = prepared.exchange (false);
    stopWorker();
    if (wasPrepared && writer != nullptr) { finishTake (false); resetArmed(); }   // άλλαξε το sample rate μέσα σε λήψη

    sampleRate = sr;
    const int cap = (int) std::ceil (8.0 * sr);
    fifo.setTotalSize (cap);
    fifo.reset();
    fifoBuf.setSize (2, cap);
    preRollLen = (int) std::ceil (kPreRollSeconds * sr);
    preRoll.setSize (2, preRollLen);
    preRoll.clear();
    preRollPos = preRollFilled = 0;
    chunk.setSize (2, 4096);
    maxFrames = (int64_t) std::llround (kMaxTakeSeconds * sr);
    thresholdLin = (float) std::pow (10.0, kThresholdDb / 20.0);
    if (getState() == State::Armed) { framesWritten = 0; }

    prepared = true;
    startWorker();
}

void RecorderEngine::release() {}   // το αρχείο κλείνει μόνο με stop, TAPE FULL ή καταστροφή του plugin

static void raisePeak (std::atomic<float>& a, float v) { if (v > a.load (std::memory_order_relaxed)) a.store (v, std::memory_order_relaxed); }

void RecorderEngine::process (const juce::AudioBuffer<float>& buffer)
{
    if (! prepared.load (std::memory_order_acquire)) return;
    const int n = buffer.getNumSamples(), nCh = buffer.getNumChannels();
    if (n <= 0 || nCh <= 0) return;

    const int chR = nCh > 1 ? 1 : 0;
    raisePeak (peakL, buffer.getMagnitude (0, 0, n));
    raisePeak (peakR, buffer.getMagnitude (chR, 0, n));

    int s1, n1, s2, n2;
    fifo.prepareToWrite (n, s1, n1, s2, n2);
    if (n1 + n2 < n) { dropped.fetch_add (n); return; }                     // γεμάτο FIFO (π.χ. κόλλησε ο δίσκος)
    const float* l = buffer.getReadPointer (0);
    const float* r = buffer.getReadPointer (chR);
    if (n1 > 0) { fifoBuf.copyFrom (0, s1, l, n1);      fifoBuf.copyFrom (1, s1, r, n1); }
    if (n2 > 0) { fifoBuf.copyFrom (0, s2, l + n1, n2); fifoBuf.copyFrom (1, s2, r + n1, n2); }
    fifo.finishedWrite (n1 + n2);
}

void RecorderEngine::setFolder (const juce::File& f) { const juce::ScopedLock sl (fileLock); folder = f; }
juce::File RecorderEngine::getFolder() const          { const juce::ScopedLock sl (fileLock); return folder; }
juce::File RecorderEngine::getCurrentFile() const     { const juce::ScopedLock sl (fileLock); return currentFile; }
void RecorderEngine::pressButton()                    { pressRequested = true; notify(); }

juce::File RecorderEngine::makeTakeFile (const juce::File& dir, juce::Time t)
{
    const auto base = "Tape_" + t.formatted ("%Y-%m-%d_%H-%M");
    auto f = dir.getChildFile (base + ".wav");
    for (int i = 2; f.exists() && i < 1000; ++i)
        f = dir.getChildFile (base + "_" + juce::String (i).paddedLeft ('0', 2) + ".wav");
    return f;
}

// ---------------------------------------------------------------- worker
void RecorderEngine::run()
{
    while (! threadShouldExit())
    {
        workerStep();
        wait (8);
    }
}

void RecorderEngine::resetArmed()
{
    preRollPos = preRollFilled = 0;
    framesWritten = 0;
    tapeFull = false;
    state = (int) State::Armed;
}

void RecorderEngine::enterError()
{
    { const juce::ScopedLock sl (fileLock); writer.reset(); }
    state = (int) State::Error;
}

void RecorderEngine::finishTake (bool full)
{
    bool ok = true;
    if (writer != nullptr) { ok = writer->sync(); writer.reset(); }
    tapeFull = full;
    state = (int) (ok ? State::Stopped : State::Error);
}

void RecorderEngine::startTake()
{
    juce::File dir = getFolder();
    if (! dir.createDirectory().wasOk() || ! dir.hasWriteAccess()) { enterError(); return; }
    const auto f = makeTakeFile (dir, juce::Time::getCurrentTime());
    auto w = std::make_unique<WavWriter> (f, sampleRate.load());
    if (! w->ok()) { enterError(); return; }
    { const juce::ScopedLock sl (fileLock); currentFile = f; }
    writer = std::move (w);
    framesWritten = 0;
    framesSinceSync = 0;
    tapeFull = false;
    state = (int) State::Recording;
}

void RecorderEngine::workerStep()
{
    if (pressRequested.exchange (false))
    {
        switch (getState())
        {
            case State::Recording: finishTake (false); break;
            case State::Stopped:
            case State::Error:     resetArmed();       break;   // νέο take: περιμένει ήχο
            case State::Armed:     break;
        }
    }

    for (;;)
    {
        const int ready = fifo.getNumReady();
        if (ready <= 0) break;
        const int n = juce::jmin (ready, chunk.getNumSamples());
        int s1, n1, s2, n2;
        fifo.prepareToRead (n, s1, n1, s2, n2);
        if (n1 > 0) { chunk.copyFrom (0, 0, fifoBuf, 0, s1, n1);  chunk.copyFrom (1, 0, fifoBuf, 1, s1, n1); }
        if (n2 > 0) { chunk.copyFrom (0, n1, fifoBuf, 0, s2, n2); chunk.copyFrom (1, n1, fifoBuf, 1, s2, n2); }
        fifo.finishedRead (n1 + n2);
        const int cnt = n1 + n2;
        const float* l = chunk.getReadPointer (0);
        const float* r = chunk.getReadPointer (1);

        switch (getState())
        {
            case State::Armed:
            {
                bool loud = false;
                for (int i = 0; i < cnt && ! loud; ++i) loud = std::abs (l[i]) > thresholdLin || std::abs (r[i]) > thresholdLin;
                if (! loud)
                {
                    for (int i = 0; i < cnt; ++i)       // pre-roll: κυκλικό buffer 0.5 s
                    {
                        preRoll.setSample (0, preRollPos, l[i]); preRoll.setSample (1, preRollPos, r[i]);
                        preRollPos = (preRollPos + 1) % preRollLen;
                        preRollFilled = juce::jmin (preRollFilled + 1, preRollLen);
                    }
                    break;
                }
                startTake();
                if (getState() != State::Recording) break;
                // πρώτα το pre-roll με χρονολογική σειρά
                if (preRollFilled > 0)
                {
                    const int start = (preRollPos - preRollFilled + preRollLen) % preRollLen;
                    const int firstLen = juce::jmin (preRollFilled, preRollLen - start);
                    bool ok = writer->write (preRoll.getReadPointer (0) + start, preRoll.getReadPointer (1) + start, firstLen);
                    if (ok && preRollFilled > firstLen)
                        ok = writer->write (preRoll.getReadPointer (0), preRoll.getReadPointer (1), preRollFilled - firstLen);
                    if (! ok) { enterError(); break; }
                    framesWritten += preRollFilled;
                    framesSinceSync += preRollFilled;
                    preRollFilled = preRollPos = 0;
                }
                [[fallthrough]];
            }
            case State::Recording:
            {
                if (writer == nullptr) break;
                const int64_t remaining = maxFrames - framesWritten.load();
                const int todo = (int) juce::jmin<int64_t> (cnt, remaining);
                if (todo > 0)
                {
                    if (! writer->write (l, r, todo)) { enterError(); break; }
                    framesWritten += todo;
                    framesSinceSync += todo;
                }
                if (framesWritten.load() >= maxFrames) { finishTake (true); break; }     // TAPE FULL στα 12:00
                if (framesSinceSync >= (int64_t) (kSyncSeconds * sampleRate.load()))
                {
                    framesSinceSync = 0;
                    if (! writer->sync()) enterError();
                }
                break;
            }
            case State::Stopped:
            case State::Error:
                break;   // ο ήχος περνά αλλά δεν γράφεται
        }
    }
}
