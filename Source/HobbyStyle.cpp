#include "HobbyStyle.h"
#include "BinaryData.h"
#include <cmath>

namespace { enum PadSymbol { symNone = -1 }; }

namespace hobby
{
    float pulse (double phase)
    {
        const double p = phase - std::floor (phase);
        return (float) (0.5 + 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * p));
    }

    juce::Typeface::Ptr loadTypeface()
    {
        return juce::Typeface::createSystemTypefaceFor (BinaryData::CarlitoBold_ttf, (size_t) BinaryData::CarlitoBold_ttfSize);
    }

    juce::Font makeFont (const juce::Typeface::Ptr& t, float h)
    {
        if (t != nullptr) return juce::Font (juce::FontOptions (t).withHeight (h));
        return juce::Font (juce::FontOptions (h, juce::Font::bold));
    }

    void drawBody (juce::Graphics& g, juce::Rectangle<int> b)
    {
        g.setGradientFill (juce::ColourGradient (kBodyTop, 0.0f, (float) b.getY(), kBodyBottom, 0.0f, (float) b.getBottom(), false));
        g.fillRect (b);
    }

    void drawBrandMark (juce::Graphics& g, const juce::Typeface::Ptr& typeface, int windowWidth)
    {
        const auto f = makeFont (typeface, 15.0f);
        const juce::String chars[3] = { juce::String (juce::CharPointer_UTF8 ("\xcf\x80")), "/", "-" };
        const float gap = 6.0f;
        float w[3], total = gap * 2.0f;
        for (int i = 0; i < 3; ++i) { juce::GlyphArrangement ga; ga.addLineOfText (f, chars[i], 0.0f, 0.0f); w[i] = ga.getBoundingBox (0, -1, true).getWidth(); total += w[i]; }
        g.setFont (f);
        for (int pass = 0; pass < 3; ++pass)
        {
            const float off = pass == 0 ? -1.0f : 1.0f;
            float x = ((float) windowWidth - total) * 0.5f;
            for (int i = 0; i < 3; ++i)
            {
                const juce::Rectangle<float> r (x + (pass == 2 ? 0.0f : off), 9.0f + (pass == 2 ? 0.0f : off), w[i] + 2.0f, 16.0f);
                g.setColour (pass == 0 ? juce::Colours::white.withAlpha (0.75f)
                                       : pass == 1 ? juce::Colour (0xffddd3c6) : juce::Colour (0xfff1ebe2));
                g.drawText (chars[i], r, juce::Justification::centredLeft, false);
                x += w[i] + gap;
            }
        }
    }

    void drawTileShadow (juce::Graphics& g, juce::Rectangle<float> tile)
    {
        juce::Path tp;
        tp.addRoundedRectangle (tile, 9.0f);
        juce::DropShadow (juce::Colours::black.withAlpha (0.55f), 4, { 0, 3 }).drawForPath (g, tp);
    }

    // ανάγλυφο πλαίσιο με βαθουλωμένη οθόνη μέσα του
    void drawBezel (juce::Graphics& g, juce::Rectangle<float> bezel, juce::Rectangle<float> screen,
                    juce::Colour screenColour, float innerShadow)
    {
        juce::Path bp;
        bp.addRoundedRectangle (bezel, 17.0f);
        juce::DropShadow (juce::Colour (0xff503c28).withAlpha (0.28f), 6, { 0, 3 }).drawForPath (g, bp);

        g.setGradientFill (juce::ColourGradient (juce::Colour (0xfffbf8f3), bezel.getX(), bezel.getY(),
                                                 juce::Colour (0xffcfc3b4), bezel.getRight(), bezel.getBottom(), false));
        g.fillPath (bp);
        g.setColour (juce::Colours::white.withAlpha (0.9f));
        g.drawLine (bezel.getX() + 12.0f, bezel.getY() + 0.5f, bezel.getRight() - 12.0f, bezel.getY() + 0.5f, 1.0f);

        // λευκή άκρη κάτω από την οθόνη, μετά η οθόνη και η εσωτερική σκιά της
        g.setColour (juce::Colours::white.withAlpha (0.9f));
        g.fillRoundedRectangle (screen.translated (0.0f, 1.0f), 12.0f);
        g.setColour (screenColour);
        g.fillRoundedRectangle (screen, 12.0f);

        juce::Path sp;
        sp.addRoundedRectangle (screen, 12.0f);
        g.saveState();
        g.reduceClipRegion (sp);
        g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.8f), 0.0f, screen.getY(),
                                                 juce::Colours::transparentBlack, 0.0f, screen.getY() + innerShadow, false));
        g.fillRect (screen);
        g.setGradientFill (juce::ColourGradient (juce::Colours::transparentBlack, 0.0f, screen.getBottom() - 6.0f,
                                                 juce::Colours::white.withAlpha (0.08f), 0.0f, screen.getBottom(), false));
        g.fillRect (screen);
        g.restoreState();
    }
}

// ---------------------------------------------------------------- σύμβολα
void drawSymbol (juce::Graphics& g, Symbol sym, juce::Point<float> c, float size, juce::Colour col)
{
    const float k = size / 22.0f;
    const auto tf = juce::AffineTransform::scale (k).translated (c.x, c.y);

    auto stroke = [&] (juce::Path p, float w, const juce::AffineTransform& extra = {})
    {
        p.applyTransform (extra);
        p.applyTransform (tf);
        g.setColour (col);
        g.strokePath (p, juce::PathStrokeType (w * k, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    };
    auto fill = [&] (juce::Path p, juce::Colour colour)
    {
        p.applyTransform (tf);
        g.setColour (colour);
        g.fillPath (p);
    };
    auto line = [] (float x1, float y1, float x2, float y2) { juce::Path p; p.startNewSubPath (x1, y1); p.lineTo (x2, y2); return p; };

    switch (sym)
    {
        case Symbol::PadRecord:
        {
            juce::Path p; p.addEllipse (-8.0f, -8.0f, 16.0f, 16.0f);
            fill (p, col);
            break;
        }
        case Symbol::PadOverdub:
        {
            juce::Path ring; ring.addEllipse (-9.0f, -9.0f, 18.0f, 18.0f);
            stroke (ring, 2.4f);
            juce::Path dot; dot.addEllipse (-4.2f, -4.2f, 8.4f, 8.4f);
            fill (dot, col);
            break;
        }
        case Symbol::Play:
        {
            juce::Path p; p.addTriangle (-6.0f, -8.0f, -6.0f, 8.0f, 8.0f, 0.0f);
            fill (p, col);
            break;
        }
        case Symbol::Stop:
        {
            juce::Path p; p.addRoundedRectangle (-8.0f, -8.0f, 16.0f, 16.0f, 2.5f);
            fill (p, col);
            break;
        }
        case Symbol::Cancel:
        {
            juce::Path p;
            p.startNewSubPath (-7.0f, -7.0f); p.lineTo (7.0f, 7.0f);
            p.startNewSubPath (-7.0f, 7.0f);  p.lineTo (7.0f, -7.0f);
            stroke (p, 2.4f);
            break;
        }
        case Symbol::Undo:
        {
            const float h = 11.0f, r = h * 0.8f, a0 = 0.9f, a1 = 5.6f;   // γωνίες από τις 12 η ώρα, δεξιόστροφα
            juce::Path arc;
            arc.addCentredArc (0.0f, 0.0f, r, r, 0.0f, a0, a1, true);
            const juce::Point<float> e (r * std::sin (a1), -r * std::cos (a1));
            const juce::Point<float> d (std::cos (a1), std::sin (a1));
            const juce::Point<float> n (-d.y, d.x);
            juce::Path head;
            head.addTriangle (e + d * (h * 0.75f), e + n * (h * 0.55f), e - n * (h * 0.55f));
            const auto mirror = juce::AffineTransform::scale (-1.0f, 1.0f);
            head.applyTransform (mirror);
            stroke (arc, 2.2f, mirror);
            fill (head, col);
            break;
        }
        case Symbol::StartQ:
        {
            stroke (line (-8.0f, -7.0f, -8.0f, 7.0f), 1.7f);
            juce::Path p; p.addTriangle (-3.0f, -6.0f, -3.0f, 6.0f, 8.0f, 0.0f);
            fill (p, col);
            break;
        }
        case Symbol::LengthQ:
        {
            juce::Path p;
            p.startNewSubPath (-9.0f, -6.0f); p.lineTo (-9.0f, 6.0f);
            p.startNewSubPath (9.0f, -6.0f);  p.lineTo (9.0f, 6.0f);
            p.startNewSubPath (-6.0f, 0.0f);  p.lineTo (6.0f, 0.0f);
            p.startNewSubPath (-3.0f, -3.0f); p.lineTo (-6.0f, 0.0f); p.lineTo (-3.0f, 3.0f);
            p.startNewSubPath (3.0f, -3.0f);  p.lineTo (6.0f, 0.0f);  p.lineTo (3.0f, 3.0f);
            stroke (p, 1.7f);
            break;
        }
        case Symbol::Snap:
        {
            juce::Path p;
            p.startNewSubPath (-9.0f, -3.0f); p.lineTo (9.0f, -3.0f);
            p.startNewSubPath (-9.0f, 3.0f);  p.lineTo (9.0f, 3.0f);
            p.startNewSubPath (-3.0f, -9.0f); p.lineTo (-3.0f, 9.0f);
            p.startNewSubPath (3.0f, -9.0f);  p.lineTo (3.0f, 9.0f);
            stroke (p, 1.7f);
            break;
        }
        case Symbol::Loop:
        {
            juce::Path p;
            p.addCentredArc (0.0f, 0.0f, 7.0f, 7.0f, 0.0f, 1.816f, 7.449f, true);
            p.startNewSubPath (8.0f, -9.0f); p.lineTo (8.0f, -3.0f); p.lineTo (2.0f, -3.0f);
            stroke (p, 1.7f);
            break;
        }
        case Symbol::Sync:
        {
            juce::Path p;
            p.addRoundedRectangle (-10.0f, -4.0f, 11.0f, 8.0f, 4.0f);
            p.addRoundedRectangle (-1.0f, -4.0f, 11.0f, 8.0f, 4.0f);
            stroke (p, 1.7f);
            break;
        }
        case Symbol::Tempo:
        {
            juce::Path p;
            p.startNewSubPath (-5.0f, 9.0f); p.lineTo (-3.0f, -8.0f); p.lineTo (3.0f, -8.0f); p.lineTo (5.0f, 9.0f); p.closeSubPath();
            p.startNewSubPath (0.0f, 4.0f);  p.lineTo (5.0f, -6.0f);
            stroke (p, 1.7f);
            break;
        }
        case Symbol::Block:
        {
            juce::Path p;
            p.addRoundedRectangle (-8.0f, -8.0f, 6.5f, 6.5f, 1.5f);
            p.addRoundedRectangle (1.5f, -8.0f, 6.5f, 6.5f, 1.5f);
            p.addRoundedRectangle (-8.0f, 1.5f, 6.5f, 6.5f, 1.5f);
            p.addRoundedRectangle (1.5f, 1.5f, 6.5f, 6.5f, 1.5f);
            stroke (p, 1.7f);
            break;
        }
        case Symbol::Learn:
        {
            juce::Path ring; ring.addEllipse (-5.5f, -5.5f, 11.0f, 11.0f);
            stroke (ring, 1.7f);
            juce::Path dot; dot.addEllipse (-1.6f, -1.6f, 3.2f, 3.2f);
            fill (dot, col);
            juce::Path ticks;
            ticks.startNewSubPath (0.0f, -10.0f); ticks.lineTo (0.0f, -7.0f);
            ticks.startNewSubPath (0.0f, 7.0f);   ticks.lineTo (0.0f, 10.0f);
            ticks.startNewSubPath (-10.0f, 0.0f); ticks.lineTo (-7.0f, 0.0f);
            ticks.startNewSubPath (7.0f, 0.0f);   ticks.lineTo (10.0f, 0.0f);
            stroke (ticks, 1.7f);
            break;
        }
        case Symbol::Vst:
        {
            juce::Path p;
            p.addRoundedRectangle (-7.0f, -6.0f, 14.0f, 10.0f, 2.0f);
            p.startNewSubPath (-3.5f, 4.0f); p.lineTo (-3.5f, 9.0f);
            p.startNewSubPath (3.5f, 4.0f);  p.lineTo (3.5f, 9.0f);
            stroke (p, 1.7f);
            break;
        }
        case Symbol::Window:
        {
            juce::Path p;
            p.addRoundedRectangle (-9.0f, -7.0f, 18.0f, 14.0f, 2.5f);
            p.startNewSubPath (-9.0f, -3.0f); p.lineTo (9.0f, -3.0f);
            stroke (p, 1.7f);
            break;
        }
        case Symbol::Hold:
        {
            juce::Path a; a.addRoundedRectangle (-6.0f, -7.0f, 4.0f, 14.0f, 1.0f);
            juce::Path b; b.addRoundedRectangle (2.0f, -7.0f, 4.0f, 14.0f, 1.0f);
            fill (a, col);
            fill (b, col);
            break;
        }
        case Symbol::Arp:
        {
            juce::Path p;
            p.startNewSubPath (-9.0f, 8.0f);
            p.lineTo (-9.0f, 3.0f); p.lineTo (-3.0f, 3.0f); p.lineTo (-3.0f, -2.0f);
            p.lineTo (3.0f, -2.0f); p.lineTo (3.0f, -7.0f); p.lineTo (9.0f, -7.0f);
            stroke (p, 1.7f);
            break;
        }
        case Symbol::Thru:
        {
            juce::Path p;
            p.startNewSubPath (-10.0f, 0.0f); p.lineTo (-4.0f, 0.0f);
            p.startNewSubPath (4.0f, 0.0f);   p.lineTo (10.0f, 0.0f);
            p.startNewSubPath (7.0f, -3.0f);  p.lineTo (10.0f, 0.0f); p.lineTo (7.0f, 3.0f);
            p.addRoundedRectangle (-4.0f, -5.0f, 8.0f, 10.0f, 2.0f);
            stroke (p, 1.7f);
            break;
        }
    }
}

// ---------------------------------------------------------------- εμφάνιση
HobbyLookAndFeel::HobbyLookAndFeel()
{
    setColour (juce::TooltipWindow::backgroundColourId, juce::Colour (0xff38322e));
    setColour (juce::TooltipWindow::textColourId, hobby::kText);
    setColour (juce::TooltipWindow::outlineColourId, juce::Colour (0xff50473f));
}

void HobbyLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour& colour,
                                              bool isHighlighted, bool isDown)
{
    if (button.getComponentID() != "footswitch")
    {
        LookAndFeel_V4::drawButtonBackground (g, button, colour, isHighlighted, isDown);
        return;
    }

    // επίπεδο πετάλι: φωτεινός δακτύλιος 92 px και λευκό πρόσωπο 80 px, χωρίς σκιές
    const auto centre = button.getLocalBounds().toFloat().getCentre();
    const auto ringArgb = (juce::uint32) (int) button.getProperties().getWithDefault ("ring", (int) hobby::kRingOff.getARGB());

    g.setColour (juce::Colour (ringArgb));
    g.fillEllipse (juce::Rectangle<float> (92.0f, 92.0f).withCentre (centre));
    g.setColour (juce::Colour (isDown ? 0xfff1ece5 : 0xfffbf9f5));
    g.fillEllipse (juce::Rectangle<float> (80.0f, 80.0f).withCentre (centre));
}

void HobbyLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& button, bool isHighlighted, bool isDown)
{
    if (button.getComponentID() != "footswitch")
    {
        LookAndFeel_V4::drawButtonText (g, button, isHighlighted, isDown);
        return;
    }

    const int sym = (int) button.getProperties().getWithDefault ("symbol", (int) symNone);
    if (sym == symNone) return;
    drawSymbol (g, (Symbol) sym, button.getLocalBounds().toFloat().getCentre(), 21.0f, hobby::kInk);
}

// ---------------------------------------------------------- τετράγωνο κουμπί τιμής
void ValueTile::paintButton (juce::Graphics& g, bool, bool isDown)
{
    const float dy = isDown ? 1.0f : 0.0f;
    const auto r = getLocalBounds().toFloat().reduced (0.5f);

    g.setGradientFill (juce::ColourGradient (juce::Colour (isDown ? 0xff3d3733 : 0xff46403b), 0.0f, r.getY(),
                                             juce::Colour (isDown ? 0xff24211f : 0xff2a2623), 0.0f, r.getBottom(), false));
    g.fillRoundedRectangle (r, 9.0f);
    g.setColour (juce::Colour (0xff1c1917));
    g.drawRoundedRectangle (r, 9.0f, 1.0f);
    g.setColour (juce::Colours::white.withAlpha (isDown ? 0.08f : 0.18f));
    g.drawLine (r.getX() + 7.0f, r.getY() + 1.5f, r.getRight() - 7.0f, r.getY() + 1.5f, 1.0f);

    juce::Colour iconColour = hobby::kIconGrey;
    if (isOnOff) iconColour = index == 1 ? hobby::kOn : hobby::kOff;
    if (hasOverride) iconColour = iconOverride;
    drawSymbol (g, symbol, { r.getCentreX(), 17.0f + dy }, 22.0f, iconColour);

    const juce::Font f (typeface != nullptr ? juce::FontOptions (typeface).withHeight (14.0f)
                                            : juce::FontOptions (14.0f, juce::Font::bold));
    g.setFont (f);
    g.setColour (juce::Colour (0xffeceef2));
    g.drawFittedText (labels[juce::jlimit (0, juce::jmax (0, labels.size() - 1), index)],
                      juce::Rectangle<int> (1, 32 + (int) dy, getWidth() - 2, 14), juce::Justification::centred, 1, 0.7f);
}

