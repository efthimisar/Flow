#pragma once

#include <algorithm>
#include <cmath>

// Smart master, λειτουργία «Context»: η διόρθωση ενός block εξαρτάται από τα άλλα blocks (masking), όχι από σταθερό όριο στάθμης.
// Καθαρός υπολογισμός χωρίς JUCE, ώστε να δοκιμάζεται εκτός Mac. Οι προτεραιότητες ρόλου και τα όρια είναι ΣΥΜΒΑΣΕΙΣ ΜΙΞΗΣ
// (όχι τεκμηριωμένα ευρήματα): ρυθμίζονται με το αυτί. Η ιδέα (μείωση masking με δυναμικό EQ ανά ζώνη) έχει βάση στη βιβλιογραφία
// (Hafezi & Reiss 2015, JAES), αλλά οι αριθμοί εδώ είναι δικές μας αρχικές τιμές.
namespace flow
{
    constexpr int smartCtxBands = 6;

    // ποιο block «κατέχει» κάθε ζώνη (1 = κυρίαρχο). Ζώνες: Sub, Low, Low-Mid, Mid, High-Mid, High
    inline float smartPrio (int type, int band)
    {
        static const float p[8][6] = {
            { 0.50f, 0.50f, 0.50f, 0.50f, 0.50f, 0.50f },   // Other (δεν διορθώνεται)
            { 0.80f, 1.00f, 0.60f, 0.25f, 0.15f, 0.10f },   // Bass: κατέχει τα Low· στο Sub δίνει προτεραιότητα στην μπότα (Drums)
            { 0.25f, 0.40f, 0.60f, 0.65f, 0.60f, 0.45f },   // Keys (πιάνο, arp)
            { 0.20f, 0.30f, 0.55f, 0.60f, 0.50f, 0.50f },   // Pad: στο ambient είναι το «χαλί», όχι υποχρεωτικά υποδεέστερο
            { 0.20f, 0.40f, 0.70f, 1.00f, 1.00f, 0.80f },   // Lead (σόλο): κατέχει Mid και High-Mid
            { 0.25f, 0.35f, 0.55f, 0.60f, 0.55f, 0.50f },   // Strings
            { 1.00f, 0.90f, 0.50f, 0.40f, 0.50f, 0.70f },   // Drums (μπότα στο Sub, hats ψηλά)
            { 0.10f, 0.20f, 0.30f, 0.40f, 0.50f, 0.60f } }; // FX
        return p[type < 0 || type > 7 ? 0 : type][band < 0 || band > 5 ? 0 : band];
    }

    inline float smartCtxEnergy (float level01) { return std::pow (10.0f, (level01 * 60.0f - 60.0f) / 10.0f); }

    // lvl[c][k]: στάθμη 0..1 ((dB+60)/60) του block c στη ζώνη k, ΠΡΙΝ τη διόρθωση. Επιστρέφει στο cutDb[k] τη μείωση που θέλουμε για το block b.
    //  1) masking: ανά ζώνη, όσο ισχυρότεροι ήχοι με μεγαλύτερη προτεραιότητα είναι δυνατότεροι από το b, κόβεται το 30% της διαφοράς (dB).
    //  2) «προϋπολογισμός χαμηλών»: αν Sub+Low είναι μεγάλο ποσοστό της συνολικής ενέργειας, όλοι κόβουν λίγο στα χαμηλά (το Bass τα μισά).
    inline void smartContextCut (const float (*lvl)[smartCtxBands], const int* type, const bool* active, int n, int b,
                                 float lowBudget, float* cutDb)
    {
        for (int k = 0; k < smartCtxBands; ++k) cutDb[k] = 0.0f;
        if (b < 0 || b >= n || ! active[b] || type[b] == 0) return;

        constexpr float eps = 1.0e-12f, floorLevel = 0.15f;   // 0.15 = -51 dBFS: πιο σιωπηλά δεν μετράνε
        float all = 0.0f, low = 0.0f;
        for (int c = 0; c < n; ++c)
            if (active[c])
                for (int k = 0; k < smartCtxBands; ++k)
                {
                    const float e = smartCtxEnergy (lvl[c][k]);
                    all += e;
                    if (k < 2) low += e;
                }

        for (int k = 0; k < smartCtxBands; ++k)
        {
            if (lvl[b][k] < floorLevel) continue;
            const float own = smartCtxEnergy (lvl[b][k]);
            const float myPrio = smartPrio (type[b], k);
            float maskers = 0.0f;
            for (int c = 0; c < n; ++c)
                if (c != b && active[c] && type[c] != 0 && smartPrio (type[c], k) > myPrio + 0.15f)
                    maskers += smartCtxEnergy (lvl[c][k]);
            const float overDb = 10.0f * std::log10 ((maskers + eps) / (own + eps));
            float cut = overDb > 0.0f ? 0.3f * overDb : 0.0f;
            if (k < 2 && all > eps && low / all > lowBudget)
            {
                const float excessDb = 10.0f * std::log10 ((low / all) / lowBudget);
                cut += std::min (4.0f, 0.5f * excessDb) * (type[b] == 1 ? 0.5f : 1.0f);
            }
            cutDb[k] = std::min (k < 2 ? 8.0f : 4.0f, cut);
        }
    }
}
