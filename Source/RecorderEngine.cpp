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
RecorderEngine::RecorderEngine() : juce::Thread ("FlowRecorderWriter") {}

RecorderEngine::~RecorderEngine()
{
    stopWorker();
    if (writer != nullptr) finishTake (false);
}

void RecorderEngine::startWorker() { startThread (juce::Thread::Priority::normal); }
void RecorderEngine::stopWorker()  { stopThread (30000); }

void RecorderEngine::prepare (double sr)
{
    if (prepared.load() && std::abs (sr - sampleRate.load()) < 0.5) return;   // ίδιο sample rate: συνεχίζουμε

    const bool wasPrepared = prepared.exchange (false);
    stopWorker();
    if (wasPrepared && writer != nullptr) { finishTake (false); resetArmed(); }   // άλλαξε το sample rate μέσα σε λήψη

    sampleRate = sr;
    const int cap = (int) std::ceil (4.0 * sr);
    fifo.setTotalSize (cap);
    fifo.reset();
    fifoBuf.setSize (kChannels, cap);
    fifoBuf.clear();
    preRollLen = (int) std::ceil (kPreRollSeconds * sr);
    preRoll.setSize (kChannels, preRollLen);
    preRoll.clear();
    preRollPos = preRollFilled = 0;
    chunk.setSize (kChannels, 4096);
    maxFrames = (int64_t) std::llround (kMaxTakeSeconds * sr);
    thresholdLin = (float) std::pow (10.0, kThresholdDb / 20.0);
    if (getState() == State::Armed) { framesWritten = 0; }

    prepared = true;
    startWorker();
}

void RecorderEngine::release() {}   // το αρχείο κλείνει μόνο με stop, TAPE FULL ή καταστροφή

static void raisePeak (std::atomic<float>& a, float v) { if (v > a.load (std::memory_order_relaxed)) a.store (v, std::memory_order_relaxed); }

void RecorderEngine::process (const juce::AudioBuffer<float>& buffer, const juce::AudioBuffer<float>& stems, uint32_t stemMask)
{
    if (! prepared.load (std::memory_order_acquire)) return;
    const int n = buffer.getNumSamples(), nCh = buffer.getNumChannels();
    if (n <= 0 || nCh <= 0) return;

    const int chR = nCh > 1 ? 1 : 0;
    raisePeak (peakL, buffer.getMagnitude (0, 0, n));
    raisePeak (peakR, buffer.getMagnitude (chR, 0, n));
    stemMaskNow.store (stemMask, std::memory_order_relaxed);

    int s1, n1, s2, n2;
    fifo.prepareToWrite (n, s1, n1, s2, n2);
    if (n1 + n2 < n) { dropped.fetch_add (n); return; }                     // γεμάτο FIFO (π.χ. κόλλησε ο δίσκος)

    auto put = [&] (int dstCh, const float* src)
    {
        if (n1 > 0) fifoBuf.copyFrom (dstCh, s1, src, n1);
        if (n2 > 0) fifoBuf.copyFrom (dstCh, s2, src + n1, n2);
    };
    put (0, buffer.getReadPointer (0));
    put (1, buffer.getReadPointer (chR));
    const int sc = juce::jmin (stems.getNumChannels(), 2 * kStems);
    for (int c = 0; c < 2 * kStems; ++c)
    {
        if (c < sc && stems.getNumSamples() >= n) put (2 + c, stems.getReadPointer (c));
        else
        {
            if (n1 > 0) fifoBuf.clear (2 + c, s1, n1);
            if (n2 > 0) fifoBuf.clear (2 + c, s2, n2);
        }
    }
    fifo.finishedWrite (n1 + n2);
}

void RecorderEngine::setFolder (const juce::File& f) { const juce::ScopedLock sl (fileLock); folder = f; }
juce::File RecorderEngine::getFolder() const          { const juce::ScopedLock sl (fileLock); return folder; }
juce::File RecorderEngine::getCurrentFile() const     { const juce::ScopedLock sl (fileLock); return currentFile; }
void RecorderEngine::pressButton()                    { pressRequested = true; notify(); }

juce::File RecorderEngine::makeTakeBase (const juce::File& dir, juce::Time t)
{
    const auto base = "Flow_" + t.formatted ("%Y-%m-%d_%H-%M");
    auto f = dir.getChildFile (base);
    for (int i = 2; (f.withFileExtension ("zip").exists() || f.withFileExtension ("wav").exists()) && i < 1000; ++i)
        f = dir.getChildFile (base + "_" + juce::String (i).paddedLeft ('0', 2));
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
    writer.reset();
    for (auto& w : stemWriter) w.reset();
    state = (int) State::Error;
}

void RecorderEngine::closeWriters (bool& ok)
{
    if (writer != nullptr) { ok = writer->sync() && ok; writer.reset(); }
    for (auto& w : stemWriter)
        if (w != nullptr) { w->sync(); w.reset(); }
}

void RecorderEngine::packZip()
{
    zipping = true;
    juce::ZipFile::Builder builder;
    builder.addFile (masterFile, 0, "Master.wav");
    for (int b = 0; b < kStems; ++b)
        if ((takeMask >> b) & 1u && stemFile[b].existsAsFile())
            builder.addFile (stemFile[b], 0, "Block " + juce::String (b + 1).paddedLeft ('0', 2) + ".wav");

    bool ok = false;
    {
        juce::FileOutputStream out (zipFile);
        if (out.openedOk())
        {
            out.setPosition (0);
            out.truncate();
            ok = builder.writeToStream (out, nullptr);
            out.flush();
        }
    }

    if (ok)
    {
        masterFile.deleteFile();
        for (int b = 0; b < kStems; ++b) stemFile[b].deleteFile();
    }
    {
        const juce::ScopedLock sl (fileLock);
        currentFile = ok ? zipFile : masterFile;
    }
    zipping = false;
}

void RecorderEngine::finishTake (bool full)
{
    bool ok = true;
    closeWriters (ok);
    if (ok) packZip();
    tapeFull = full;
    state = (int) (ok ? State::Stopped : State::Error);
}

void RecorderEngine::startTake()
{
    juce::File dir = getFolder();
    if (! dir.createDirectory().wasOk() || ! dir.hasWriteAccess()) { enterError(); return; }
    const auto base = makeTakeBase (dir, juce::Time::getCurrentTime());
    masterFile = base.withFileExtension ("wav");
    zipFile = base.withFileExtension ("zip");

    auto w = std::make_unique<WavWriter> (masterFile, sampleRate.load());
    if (! w->ok()) { enterError(); return; }

    takeMask = stemMaskNow.load();
    for (int b = 0; b < kStems; ++b)
    {
        if (! ((takeMask >> b) & 1u)) continue;
        stemFile[b] = dir.getChildFile (base.getFileName() + "_block" + juce::String (b + 1).paddedLeft ('0', 2) + ".wav");
        auto sw = std::make_unique<WavWriter> (stemFile[b], sampleRate.load());
        if (sw->ok()) stemWriter[b] = std::move (sw);
        else takeMask &= ~(1u << b);
    }

    { const juce::ScopedLock sl (fileLock); currentFile = masterFile; }
    writer = std::move (w);
    framesWritten = 0;
    framesSinceSync = 0;
    tapeFull = false;
    state = (int) State::Recording;
}

bool RecorderEngine::writeRange (const juce::AudioBuffer<float>& src, int start, int n)
{
    if (! writer->write (src.getReadPointer (0) + start, src.getReadPointer (1) + start, n)) return false;
    for (int b = 0; b < kStems; ++b)
        if (stemWriter[b] != nullptr)
            if (! stemWriter[b]->write (src.getReadPointer (2 + 2 * b) + start, src.getReadPointer (3 + 2 * b) + start, n))
                stemWriter[b].reset();   // ένα stem που απέτυχε δεν σταματά τη λήψη
    return true;
}

void RecorderEngine::workerStep()
{
    if (pressRequested.exchange (false))
    {
        switch (getState())
        {
            case State::Recording: finishTake (false); break;
            case State::Stopped:
            case State::Error:     resetArmed(); startTake(); break;   // νέο take: ξεκινά αμέσως στο πάτημα
            case State::Armed:     startTake();        break;          // ξεκινά αμέσως στο πάτημα, όχι με τον πρώτο ήχο
        }
    }

    for (;;)
    {
        const int ready = fifo.getNumReady();
        if (ready <= 0) break;
        const int n = juce::jmin (ready, chunk.getNumSamples());
        int s1, n1, s2, n2;
        fifo.prepareToRead (n, s1, n1, s2, n2);
        for (int c = 0; c < kChannels; ++c)
        {
            if (n1 > 0) chunk.copyFrom (c, 0, fifoBuf, c, s1, n1);
            if (n2 > 0) chunk.copyFrom (c, n1, fifoBuf, c, s2, n2);
        }
        fifo.finishedRead (n1 + n2);
        const int cnt = n1 + n2;
        const float* l = chunk.getReadPointer (0);
        const float* r = chunk.getReadPointer (1);

        switch (getState())
        {
            case State::Armed:
                break;   // δεν ξεκινά μόνο του: περιμένει το πάτημα
            case State::Recording:
            {
                if (writer == nullptr) break;
                const int64_t remaining = maxFrames - framesWritten.load();
                const int todo = (int) juce::jmin<int64_t> (cnt, remaining);
                if (todo > 0)
                {
                    if (! writeRange (chunk, 0, todo)) { enterError(); break; }
                    framesWritten += todo;
                    framesSinceSync += todo;
                }
                if (framesWritten.load() >= maxFrames) { finishTake (true); break; }     // TAPE FULL στα 12:00
                if (framesSinceSync >= (int64_t) (kSyncSeconds * sampleRate.load()))
                {
                    framesSinceSync = 0;
                    bool ok = writer->sync();
                    for (auto& w : stemWriter) if (w != nullptr) w->sync();
                    if (! ok) enterError();
                }
                break;
            }
            case State::Stopped:
            case State::Error:
                break;   // ο ήχος περνά αλλά δεν γράφεται
        }
    }
}
