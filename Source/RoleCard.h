#pragma once
// RoleCard - καρτέλα μέσα στην οθόνη των loops: "Τι είναι αυτός ο ήχος;". Επτά στρογγυλά πλακίδια, ένα ανά ρόλο.
// Το πλακίδιο που προέρχεται από το Smart master του block είναι πράσινο. Δεν είναι παράθυρο.
#include "HobbyStyle.h"

class RoleCard : public juce::Component
{
public:
    static constexpr int numRoles = 7;

    // η σειρά είναι και η σειρά των blocks του Palette από αριστερά προς τα δεξιά
    static const char* name (int i)
    {
        static const char* const n[numRoles] = { "BASS", "ARP", "OTHER", "PADS/STRINGS", "KEYS", "LEAD", "FX" };
        return n[juce::jlimit (0, numRoles - 1, i)];
    }

    // Smart master: 0 Other, 1 Bass, 2 Keys, 3 Pad, 4 Lead, 5 Strings, 6 Drums, 7 FX. Το Drums δεν έχει ρόλο (-1).
    static int roleFromSmartType (int t)
    {
        static const int m[8] { 2, 0, 4, 3, 5, 3, -1, 6 };
        return m[juce::jlimit (0, 7, t)];
    }

    explicit RoleCard (juce::Typeface::Ptr face) : typeface (face)
    {
        setWantsKeyboardFocus (true);
        setVisible (false);
    }

    std::function<void (int)> onPick;
    std::function<void()> onCancel;

    void open (const juce::String& t, const juce::String& sub, int pre)
    {
        title = t; subtitle = sub; preselect = pre; pressed = -1;
        setVisible (true);
        toFront (false);
        grabKeyboardFocus();
        repaint();
    }

    void close() { setVisible (false); pressed = -1; }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (hobby::kScreen);
        const float s = scale();
        const float w = (float) getWidth();

        g.setColour (hobby::kText);
        g.setFont (hobby::makeFont (typeface, 20.0f * s));
        g.drawText (title, juce::Rectangle<float> (24.0f * s, 22.0f * s, w - 120.0f * s, 26.0f * s), juce::Justification::centredLeft, false);
        g.setColour (hobby::kIconGrey);
        g.setFont (hobby::makeFont (typeface, 14.0f * s));
        g.drawText (subtitle, juce::Rectangle<float> (24.0f * s, 52.0f * s, w - 120.0f * s, 20.0f * s), juce::Justification::centredLeft, false);

        // ✕ πάνω δεξιά: ακύρωση
        {
            const auto c = closeBounds();
            const auto m = c.getCentre();
            juce::Path x;
            x.startNewSubPath (m.x - 7.0f * s, m.y - 7.0f * s); x.lineTo (m.x + 7.0f * s, m.y + 7.0f * s);
            x.startNewSubPath (m.x + 7.0f * s, m.y - 7.0f * s); x.lineTo (m.x - 7.0f * s, m.y + 7.0f * s);
            g.setColour (hobby::kIconGrey);
            g.strokePath (x, juce::PathStrokeType (2.0f * s, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        for (int i = 0; i < numRoles; ++i)
        {
            const auto r = tileBounds (i);
            const bool down = (i == pressed);
            juce::Path ep;
            ep.addEllipse (r);
            juce::DropShadow (juce::Colours::black.withAlpha (0.55f), 4, { 0, 3 }).drawForPath (g, ep);
            g.setGradientFill (juce::ColourGradient (juce::Colour (down ? 0xff3d3733 : 0xff46403b), 0.0f, r.getY(),
                                                     juce::Colour (down ? 0xff24211f : 0xff2a2623), 0.0f, r.getBottom(), false));
            g.fillEllipse (r);
            g.setColour (juce::Colours::white.withAlpha (down ? 0.08f : 0.18f));
            g.drawLine (r.getX() + r.getWidth() * 0.22f, r.getY() + 1.5f, r.getRight() - r.getWidth() * 0.22f, r.getY() + 1.5f, 1.0f);

            const bool on = (i == preselect);
            if (on)
            {
                g.setColour (hobby::kOn);
                g.drawEllipse (r.reduced (3.0f), 2.5f);
            }

            const juce::String text (name (i));
            float fs = 17.0f * s;
            juce::Font f = hobby::makeFont (typeface, fs);
            while (fs > 9.0f && juce::GlyphArrangement::getStringWidth (f, text) > r.getWidth() * 0.78f)
            {
                fs -= 0.5f;
                f = hobby::makeFont (typeface, fs);
            }
            g.setFont (f);
            g.setColour (on ? hobby::kOn : hobby::kIconGrey);
            g.drawText (text, r.translated (0.0f, down ? 1.0f
                                                       : 0.0f), juce::Justification::centred, false);
        }

        g.setColour (hobby::kIconGrey);
        g.setFont (hobby::makeFont (typeface, 14.0f * s));
        const float by = (float) getHeight() * 0.5f + diameter() * 0.5f + 46.0f * s;
        g.drawText (juce::String::fromUTF8 ("press a tile: the sound is kept with that label"),
                    juce::Rectangle<float> (0.0f, by, w, 20.0f * s), juce::Justification::centred, false);
        g.setFont (hobby::makeFont (typeface, 12.0f * s));
        g.drawText (juce::String::fromUTF8 ("green: taken from the Smart master type of the block"),
                    juce::Rectangle<float> (0.0f, by + 24.0f * s, w, 18.0f * s), juce::Justification::centred, false);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        pressed = -1;
        if (closeBounds().contains (e.position)) { closePressed = true; return; }
        closePressed = false;
        for (int i = 0; i < numRoles; ++i)
            if (hitEllipse (tileBounds (i), e.position)) { pressed = i; break; }
        repaint();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        const int p = pressed;
        const bool cp = closePressed;
        pressed = -1;
        closePressed = false;
        repaint();
        if (cp && closeBounds().contains (e.position)) { if (onCancel) onCancel(); return; }
        if (p >= 0 && hitEllipse (tileBounds (p), e.position) && onPick) onPick (p);
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::escapeKey) { if (onCancel) onCancel(); return true; }
        return false;
    }

private:
    float scale() const { return juce::jlimit (0.6f, 1.3f, juce::jmin ((float) getWidth() / 1055.0f, (float) getHeight() / 490.0f)); }
    float diameter() const
    {
        const float w = (float) getWidth();
        const float d = juce::jmin (w * 0.108f, (w - 80.0f) / ((float) numRoles + 6.0f * 0.18f));
        return juce::jlimit (50.0f, 140.0f, d);
    }
    juce::Rectangle<float> closeBounds() const { const float s = scale(); return { (float) getWidth() - 56.0f * s, 18.0f * s, 36.0f * s, 36.0f * s }; }
    juce::Rectangle<float> tileBounds (int i) const
    {
        const float d = diameter(), gap = d * 0.18f;
        const float total = (float) numRoles * d + (float) (numRoles - 1) * gap;
        const float x0 = ((float) getWidth() - total) * 0.5f;
        return { x0 + (float) i * (d + gap), (float) getHeight() * 0.5f - d * 0.5f, d, d };
    }
    static bool hitEllipse (juce::Rectangle<float> r, juce::Point<float> p)
    {
        const auto c = r.getCentre();
        const float rx = r.getWidth() * 0.5f;
        return (p.x - c.x) * (p.x - c.x) + (p.y - c.y) * (p.y - c.y) <= rx * rx;
    }

    juce::Typeface::Ptr typeface;
    juce::String title, subtitle;
    int preselect = -1, pressed = -1;
    bool closePressed = false;
};
