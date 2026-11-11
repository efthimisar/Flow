#pragma once
#include <juce_graphics/juce_graphics.h>
#include <cmath>

// Σχέδια κίνησης του Orb. Ένα ring = μία ανάθεση: η τιμή της παραμέτρου είναι η ΚΑΤΑΚΟΡΥΦΗ θέση της τελείας (κέντρο = 50%, πάνω = 100%).
// Το ίδιο σημείο χρησιμοποιείται και για τη ζωγραφική του συμβόλου στο ring (οθόνη Flow και S88).
namespace orbshapes
{
    constexpr int count = 17;       // 0..15 αυτόματα σχέδια, 16 = U (user: η κίνηση που έγραψε ο χρήστης με Rec motion)
    constexpr int userShape = 16;
    constexpr int speedSteps = 5;   // 0 = αργότερο ... 4 = ταχύτερο
    inline double periodSeconds (int speed)   // δευτερόλεπτα ανά κύκλο
    {
        static const double p[speedSteps] { 16.0, 8.0, 4.0, 2.0, 1.0 };
        return p[juce::jlimit (0, speedSteps - 1, speed)];
    }
    constexpr int defaultSpeed = 1;   // 8 δλ, όπως πριν

    inline float tri (float x) { x -= std::floor (x); return 4.0f * std::abs (x - 0.5f) - 1.0f; }   // τριγωνικό κύμα -1..1

    // u: 0..1 μέσα στον κύκλο. ph: 4 τυχαίες φάσεις (περιπλάνηση), tg: 4 τυχαίοι στόχοι (άλμα). Επιστρέφει θέση μέσα στον μοναδιαίο κύκλο.
    inline juce::Point<float> point (int pat, float u, const float* ph, const juce::Point<float>* tg)
    {
        using juce::Point;
        const float pi = juce::MathConstants<float>::pi, T = 2.0f * pi * u, a = -0.5f * pi;
        switch (pat)
        {
            case 0:  return { 0.55f * std::cos (T + a), 0.55f * std::sin (T + a) };                                   // κύκλος
            case 1:  return { 0.4f * std::sin (2.0f * T), 0.65f * std::sin (T) };                                     // οκτώ (κάθετο)
            case 2:  return { 0.0f, 0.85f * std::sin (T) };                                                           // σάρωση
            case 3:  { const float r = 0.15f + 0.7f * (0.5f - 0.5f * std::cos (T)); return { r * std::cos (2.0f * T + a), r * std::sin (2.0f * T + a) }; }   // σπείρα
            case 4:  { const float r = 0.45f + 0.4f * std::sin (2.0f * T); return { 0.0f, -r }; }                      // παλμός
            case 5:  return { 0.6f * std::sin (T + ph[0]) + 0.2f * std::sin (3.0f * T + ph[1]),
                              0.6f * std::sin (T + ph[2] + 1.3f) + 0.2f * std::sin (2.0f * T + ph[3]) };               // περιπλάνηση
            case 6:  { const int st = (int) (u * 4.0f) % 4; return st % 2 ? Point<float> (0.0f, 0.0f) : tg[st]; }      // άλμα και επιστροφή
            case 7:  { const Point<float> v[3] { { 0.0f, -0.85f }, { 0.8f, 0.6f }, { -0.8f, 0.6f } };                   // τρίγωνο
                       const float s = u * 3.0f; const int i = (int) s % 3, j = (i + 1) % 3; const float f = s - std::floor (s);
                       return { v[i].x * (1.0f - f) + v[j].x * f, v[i].y * (1.0f - f) + v[j].y * f }; }
            case 8:  { const Point<float> v[4] { { 0.0f, -0.85f }, { 0.85f, 0.0f }, { 0.0f, 0.85f }, { -0.85f, 0.0f } };   // ρόμβος
                       const float s = u * 4.0f; const int i = (int) s % 4, j = (i + 1) % 4; const float f = s - std::floor (s);
                       return { v[i].x * (1.0f - f) + v[j].x * f, v[i].y * (1.0f - f) + v[j].y * f }; }
            case 9:  return { 0.85f * std::sin (3.0f * T), 0.85f * std::sin (2.0f * T) };                             // Lissajous 3:2
            case 10: { const float r = 0.85f * std::cos (3.0f * T); return { r * std::cos (T), r * std::sin (T) }; }  // λουλούδι
            case 11: { const float s = 0.9f * std::sin (T); return { 0.7f * s, s }; }                                  // εκκρεμές
            case 12: return { 0.75f * std::sin (T + 0.3f) + 0.15f * std::sin (5.0f * T), 0.6f * std::sin (2.0f * T + 1.0f) };   // drift
            case 13: return { 0.85f * tri (u * 2.0f), 0.85f * tri (u * 3.0f + 0.2f) };                                 // αναπήδηση
            case 14: { const float s1 = std::max (0.0f, std::sin (2.0f * T)), s2 = std::max (0.0f, std::sin (2.0f * T - 1.2f));
                       return { 0.0f, -(0.8f * s1 * s1 + 0.45f * s2 * s2) }; }                                         // καρδιά
            case 15: { const float f = u * 2.0f - std::floor (u * 2.0f); return { 0.0f, -0.9f * std::exp (-5.0f * f) }; }   // σταγόνα
            default: return { 0.0f, 0.0f };
        }
    }

    // Ζωγραφίζει το σύμβολο (η διαδρομή του σχεδίου) μέσα σε κύκλο ακτίνας r γύρω από το c. Το U ζωγραφίζεται ως γράμμα.
    inline void drawSymbol (juce::Graphics& g, int pat, juce::Point<float> c, float r, juce::Colour col, const juce::Font& font)
    {
        if (pat < 0) return;
        if (pat == userShape)
        {
            g.setColour (col); g.setFont (font.withHeight (r * 1.5f).boldened());
            g.drawText ("U", juce::Rectangle<float> (c.x - r, c.y - r, 2.0f * r, 2.0f * r), juce::Justification::centred, false);
            return;
        }
        static const float ph[4] { 0.3f, 1.1f, 2.0f, 0.7f };
        static const juce::Point<float> tg[4] { { 0.7f, -0.5f }, { 0.0f, 0.0f }, { -0.6f, 0.6f }, { 0.0f, 0.0f } };
        juce::Path p;
        for (int i = 0; i <= 96; ++i)
        {
            const auto q = point (pat, (float) i / 96.0f, ph, tg);
            const float x = c.x + q.x * r, y = c.y + q.y * r;
            if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
        }
        g.setColour (col);
        g.strokePath (p, juce::PathStrokeType (juce::jmax (1.2f, r * 0.12f), juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
}
