#pragma once
// Ρυθμίσεις Audio & MIDI μέσα στην οθόνη του Flow (αντί για παράθυρο): έξοδος, sample rate, buffer, κανάλια, είσοδοι και έξοδος MIDI.
#include <juce_audio_utils/juce_audio_utils.h>
#include "HobbyStyle.h"
#include <functional>
#include <vector>

class AudioMidiPanel : public juce::Component, private juce::Timer
{
public:
    AudioMidiPanel (juce::AudioDeviceManager& d, juce::Typeface::Ptr face) : dm (d), typeface (face) {}
    ~AudioMidiPanel() override { stopTimer(); }

    std::function<void()> onClose, onChanged;
    std::function<void (const juce::String&)> onMessage;

    void open()
    {
        outOpen = midiOutOpen = false;
        setVisible (true);
        toFront (false);
        startTimerHz (2);
        repaint();
    }
    void close()
    {
        stopTimer();
        setVisible (false);
        if (onClose) onClose();
    }

    void paint (juce::Graphics& g) override
    {
        using namespace juce;
        hits.clear();
        const auto bounds = getLocalBounds();
        g.setColour (hobby::kScreen);
        g.fillRoundedRectangle (bounds.toFloat(), 14.0f);

        const Colour txt (0xffe6e0d8), dim (0xff968e86), line (0xff3e3a37), fieldBg (0xff36322f), green = hobby::kOn;
        auto font = [&] (float h, bool bold = false) { return bold ? hobby::makeFont (typeface, h).boldened() : hobby::makeFont (typeface, h); };
        auto hover = [&] (const Rectangle<int>& r) { return r.contains (mouse); };

        auto label = [&] (int x, int y, const String& t)
        {
            g.setColour (dim); g.setFont (font (12.0f, true));
            g.drawText (t, x, y, 400, 16, Justification::centredLeft, false);
        };
        auto chip = [&] (int x, int y, int w, const String& t, bool on, std::function<void()> fn)
        {
            const Rectangle<int> r (x, y, w, 28);
            g.setColour (on ? Colour (0xff284234) : (hover (r) ? Colour (0xff423d39) : Colour (0xff36322f)));
            g.fillRoundedRectangle (r.toFloat(), 14.0f);
            g.setColour (on ? green : line);
            g.drawRoundedRectangle (r.toFloat().reduced (1.0f), 14.0f, on ? 2.0f : 1.0f);
            g.setColour (on ? green : txt); g.setFont (font (12.0f));
            g.drawText (t, r, Justification::centred, false);
            hits.push_back ({ r, std::move (fn) });
        };
        auto field = [&] (int x, int y, int w, const String& t, bool open, std::function<void()> fn)
        {
            const Rectangle<int> r (x, y, w, 34);
            g.setColour (hover (r) ? Colour (0xff423d39) : fieldBg);
            g.fillRoundedRectangle (r.toFloat(), 10.0f);
            g.setColour (open ? green : line);
            g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 10.0f, 1.0f);
            g.setColour (txt); g.setFont (font (14.0f));
            g.drawText (t, r.withTrimmedLeft (16).withTrimmedRight (34), Justification::centredLeft, true);
            g.setColour (dim);
            g.drawText (open ? juce::String::fromUTF8 ("\xe2\x96\xb4") : juce::String::fromUTF8 ("\xe2\x96\xbe"), r.withTrimmedRight (14), Justification::centredRight, false);
            hits.push_back ({ r, std::move (fn) });
        };
        auto item = [&] (int x, int y, int w, const String& t, bool on, std::function<void()> fn)
        {
            const Rectangle<int> r (x, y, w, 30);
            if (hover (r)) { g.setColour (Colour (0xff423d39)); g.fillRoundedRectangle (r.toFloat(), 8.0f); }
            g.setColour (on ? green : txt); g.setFont (font (13.0f));
            g.drawText ((on ? juce::String::fromUTF8 ("\xe2\x9c\x93  ") : String ("     ")) + t, r.withTrimmedLeft (12), Justification::centredLeft, true);
            hits.push_back ({ r, std::move (fn) });
        };

        // τίτλος και κλείσιμο
        g.setColour (dim); g.setFont (font (13.0f, true));
        g.drawText ("AUDIO & MIDI", 40, 26, 300, 20, Justification::centredLeft, false);
        {
            const Rectangle<int> r (getWidth() - 40 - 26, 22, 26, 26);
            g.setColour (hover (r) ? Colour (0xff4a4440) : fieldBg);
            g.fillEllipse (r.toFloat());
            g.setColour (txt);
            g.drawLine ((float) r.getX() + 8.0f, (float) r.getY() + 8.0f, (float) r.getRight() - 8.0f, (float) r.getBottom() - 8.0f, 2.0f);
            g.drawLine ((float) r.getX() + 8.0f, (float) r.getBottom() - 8.0f, (float) r.getRight() - 8.0f, (float) r.getY() + 8.0f, 2.0f);
            hits.push_back ({ r, [this] { close(); } });
        }

        auto* dev = dm.getCurrentAudioDevice();
        const auto setup = dm.getAudioDeviceSetup();
        const int colW = (getWidth() - 80 - 40) / 2;
        const int xl = 40, xr = 40 + colW + 40;

        // ---------------- αριστερά: ήχος ----------------
        int y = 70;
        label (xl, y, "Output"); y += 18;
        field (xl, y, colW, dev != nullptr ? dev->getName() : String ("none"), outOpen, [this] { outOpen = ! outOpen; repaint(); });
        y += 40;
        if (outOpen)
        {
            StringArray names;
            if (auto* type = dm.getCurrentDeviceTypeObject()) names = type->getDeviceNames (false);
            for (int i = 0; i < names.size() && i < 8; ++i)
            {
                const String n = names[i];
                item (xl, y, colW, n, dev != nullptr && dev->getName() == n, [this, n]
                {
                    auto s = dm.getAudioDeviceSetup();
                    s.outputDeviceName = n; s.inputDeviceName = {};
                    s.useDefaultOutputChannels = true; s.useDefaultInputChannels = true;
                    outOpen = false;
                    apply (s);
                });
                y += 32;
            }
            y += 4;
        }
        y += 10;

        if (dev != nullptr)
        {
            label (xl, y, "Sample rate"); y += 18;
            {
                int x = xl;
                const auto rates = dev->getAvailableSampleRates();
                for (int i = 0; i < rates.size() && i < 8; ++i)
                {
                    const double r = rates[i];
                    const String t = std::abs (r / 1000.0 - std::round (r / 1000.0)) < 0.001 ? String ((int) std::round (r / 1000.0)) + "k" : String (r / 1000.0, 1) + "k";
                    if (x + 76 > xl + colW) { x = xl; y += 34; }
                    chip (x, y, 76, t, std::abs (r - dev->getCurrentSampleRate()) < 1.0, [this, r] { auto s = dm.getAudioDeviceSetup(); s.sampleRate = r; apply (s); });
                    x += 84;
                }
                y += 42;
            }

            label (xl, y, "Buffer"); y += 18;
            {
                int x = xl;
                Array<int> sizes;
                for (int b : dev->getAvailableBufferSizes()) if (b >= 32 && b <= 2048 && (b & (b - 1)) == 0) sizes.add (b);
                if (sizes.isEmpty()) for (int b : dev->getAvailableBufferSizes()) { if (sizes.size() < 6) sizes.add (b); }
                for (int b : sizes)
                {
                    if (x + 76 > xl + colW) { x = xl; y += 34; }
                    chip (x, y, 76, String (b), b == dev->getCurrentBufferSizeSamples(), [this, b] { auto s = dm.getAudioDeviceSetup(); s.bufferSize = b; apply (s); });
                    x += 84;
                }
                y += 42;
            }

            {
                const double sr = jmax (1.0, dev->getCurrentSampleRate());
                g.setColour (dim); g.setFont (font (12.0f));
                g.drawText ("Latency  " + String (1000.0 * (double) dev->getOutputLatencyInSamples() / sr, 1) + " ms   " + juce::String::fromUTF8 ("\xc2\xb7") + "   CPU " + String (roundToInt (dm.getCpuUsage() * 100.0)) + "%",
                            xl, y, colW, 16, Justification::centredLeft, false);
                y += 34;
            }

            const auto names = dev->getOutputChannelNames();
            if (names.size() > 2)
            {
                label (xl, y, "Output channels"); y += 18;
                const auto active = dev->getActiveOutputChannels();
                int x = xl;
                for (int i = 0; i < names.size() && i < 16; i += 2)
                {
                    const bool pair = i + 1 < names.size();
                    const String t = pair ? String (i + 1) + "-" + String (i + 2) : String (i + 1);
                    const bool on = active[i] && (! pair || active[i + 1]);
                    if (x + 76 > xl + colW) { x = xl; y += 34; }
                    chip (x, y, 76, t, on, [this, i, pair, active]
                    {
                        auto s = dm.getAudioDeviceSetup();
                        BigInteger bits = active;
                        const bool turnOn = ! (active[i] && (! pair || active[i + 1]));
                        bits.setBit (i, turnOn);
                        if (pair) bits.setBit (i + 1, turnOn);
                        if (bits.isZero()) return;              // πάντα τουλάχιστον ένα ζεύγος
                        s.useDefaultOutputChannels = false;
                        s.outputChannels = bits;
                        apply (s);
                    });
                    x += 84;
                }
            }
        }

        // ---------------- δεξιά: MIDI ----------------
        y = 70;
        label (xr, y, "MIDI inputs"); y += 18;
        {
            const auto devs = MidiInput::getAvailableDevices();
            if (devs.isEmpty())
            {
                g.setColour (dim); g.setFont (font (13.0f));
                g.drawText ("none found", xr, y, colW, 30, Justification::centredLeft, false);
                y += 46;
            }
            for (int i = 0; i < devs.size() && i < 8; ++i)
            {
                const auto& d = devs.getReference (i);
                const bool on = dm.isMidiInputDeviceEnabled (d.identifier);
                const Rectangle<int> r (xr, y, colW, 38);
                g.setColour (hover (r) ? Colour (0xff3a3532) : Colour (0xff2e2a28));
                g.fillRoundedRectangle (r.toFloat(), 10.0f);
                g.setColour (line); g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 10.0f, 1.0f);
                g.setColour (txt); g.setFont (font (13.0f));
                g.drawText (d.name, r.withTrimmedLeft (16).withTrimmedRight (70), Justification::centredLeft, true);
                const Rectangle<float> sw ((float) r.getRight() - 52.0f, (float) r.getY() + 9.0f, 38.0f, 20.0f);
                g.setColour (on ? Colour (0xff284234) : Colour (0xff3e3a37));
                g.fillRoundedRectangle (sw, 10.0f);
                g.setColour (on ? green : dim);
                g.fillEllipse (on ? sw.getRight() - 18.0f : sw.getX() + 2.0f, sw.getY() + 2.0f, 16.0f, 16.0f);
                const String id = d.identifier;
                hits.push_back ({ r, [this, id, on] { dm.setMidiInputDeviceEnabled (id, ! on); if (onChanged) onChanged(); repaint(); } });
                y += 46;
            }
        }
        y += 14;
        label (xr, y, "MIDI output"); y += 18;
        {
            const auto outs = MidiOutput::getAvailableDevices();
            const String cur = dm.getDefaultMidiOutputIdentifier();
            String curName = "none";
            for (auto& o : outs) if (o.identifier == cur) curName = o.name;
            field (xr, y, colW, curName, midiOutOpen, [this] { midiOutOpen = ! midiOutOpen; repaint(); });
            y += 40;
            if (midiOutOpen)
            {
                item (xr, y, colW, "none", cur.isEmpty(), [this] { dm.setDefaultMidiOutputDevice ({}); midiOutOpen = false; if (onChanged) onChanged(); repaint(); });
                y += 32;
                for (int i = 0; i < outs.size() && i < 8; ++i)
                {
                    const String id = outs[i].identifier;
                    item (xr, y, colW, outs[i].name, id == cur, [this, id] { dm.setDefaultMidiOutputDevice (id); midiOutOpen = false; if (onChanged) onChanged(); repaint(); });
                    y += 32;
                }
            }
        }

        g.setColour (Colour (0xff786f67)); g.setFont (font (11.0f));
        g.drawText (juce::String::fromUTF8 ("Changes apply immediately and are saved."), 40, getHeight() - 30, 500, 16, Justification::centredLeft, false);
    }

    void mouseMove (const juce::MouseEvent& e) override { mouse = e.getPosition(); repaint(); }
    void mouseExit (const juce::MouseEvent&) override { mouse = { -1, -1 }; repaint(); }
    void mouseDown (const juce::MouseEvent& e) override { mouse = e.getPosition(); }
    void mouseUp (const juce::MouseEvent& e) override
    {
        for (int i = (int) hits.size(); --i >= 0;)
            if (hits[(size_t) i].r.contains (e.getPosition()))
            {
                auto fn = hits[(size_t) i].fn;   // αντίγραφο: το fn αλλάζει την κατάσταση και ξαναζωγραφίζει
                if (fn) fn();
                return;
            }
    }
    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::escapeKey) { close(); return true; }
        return false;
    }

private:
    struct Hit { juce::Rectangle<int> r; std::function<void()> fn; };

    void apply (const juce::AudioDeviceManager::AudioDeviceSetup& s)
    {
        const auto err = dm.setAudioDeviceSetup (s, true);
        if (err.isNotEmpty() && onMessage) onMessage (err);
        if (onChanged) onChanged();
        repaint();
    }
    void timerCallback() override { if (isVisible()) repaint(); }

    juce::AudioDeviceManager& dm;
    juce::Typeface::Ptr typeface;
    std::vector<Hit> hits;
    juce::Point<int> mouse { -1, -1 };
    bool outOpen = false, midiOutOpen = false;
};
