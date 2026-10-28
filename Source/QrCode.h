#pragma once
// Μικρός δημιουργός QR code (byte mode, διόρθωση σφαλμάτων M, εκδόσεις 1–10). Δικός μας κώδικας, χωρίς εξωτερική βιβλιοθήκη.
#include <vector>
#include <string>
#include <cstdint>
#include <algorithm>

namespace qr
{
struct Code
{
    int size = 0;
    std::vector<std::vector<bool>> m;   // m[y][x], true = σκούρο
    bool dark (int x, int y) const { return x >= 0 && y >= 0 && x < size && y < size && m[(size_t) y][(size_t) x]; }
};

namespace detail
{
    static const int ECC_PER_BLOCK_M[11] = { 0, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26 };
    static const int NUM_BLOCKS_M[11]    = { 0, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5 };

    inline int rawModules (int ver)
    {
        int r = (16 * ver + 128) * ver + 64;
        if (ver >= 2) { const int na = ver / 7 + 2; r -= (25 * na - 10) * na - 55; if (ver >= 7) r -= 36; }
        return r;
    }
    inline int dataCodewords (int ver) { return rawModules (ver) / 8 - ECC_PER_BLOCK_M[ver] * NUM_BLOCKS_M[ver]; }

    inline uint8_t gfMul (uint8_t x, uint8_t y)
    {
        int z = 0;
        for (int i = 7; i >= 0; --i) { z = (z << 1) ^ ((z >> 7) * 0x11D); z ^= ((y >> i) & 1) * x; }
        return (uint8_t) z;
    }
    inline std::vector<uint8_t> rsDivisor (int degree)
    {
        std::vector<uint8_t> r ((size_t) degree, 0);
        r[(size_t) degree - 1] = 1;
        uint8_t root = 1;
        for (int i = 0; i < degree; ++i)
        {
            for (int j = 0; j < degree; ++j)
            {
                r[(size_t) j] = gfMul (r[(size_t) j], root);
                if (j + 1 < degree) r[(size_t) j] ^= r[(size_t) j + 1];
            }
            root = gfMul (root, 0x02);
        }
        return r;
    }
    inline std::vector<uint8_t> rsRemainder (const std::vector<uint8_t>& data, const std::vector<uint8_t>& div)
    {
        std::vector<uint8_t> r (div.size(), 0);
        for (uint8_t b : data)
        {
            const uint8_t f = b ^ r[0];
            r.erase (r.begin());
            r.push_back (0);
            for (size_t i = 0; i < r.size(); ++i) r[i] ^= gfMul (div[i], f);
        }
        return r;
    }

    struct Builder
    {
        int ver, size;
        std::vector<std::vector<bool>> mod, fn;
        Builder (int v) : ver (v), size (v * 4 + 17), mod ((size_t) size, std::vector<bool> ((size_t) size, false)), fn ((size_t) size, std::vector<bool> ((size_t) size, false)) {}
        void setFn (int x, int y, bool d) { mod[(size_t) y][(size_t) x] = d; fn[(size_t) y][(size_t) x] = true; }
        static bool bit (long v, int i) { return ((v >> i) & 1) != 0; }

        void finder (int cx, int cy)
        {
            for (int dy = -4; dy <= 4; ++dy)
                for (int dx = -4; dx <= 4; ++dx)
                {
                    const int dist = std::max (std::abs (dx), std::abs (dy)), x = cx + dx, y = cy + dy;
                    if (x >= 0 && x < size && y >= 0 && y < size) setFn (x, y, dist != 2 && dist != 4);
                }
        }
        void align (int cx, int cy)
        {
            for (int dy = -2; dy <= 2; ++dy) for (int dx = -2; dx <= 2; ++dx) setFn (cx + dx, cy + dy, std::max (std::abs (dx), std::abs (dy)) != 1);
        }
        std::vector<int> alignPositions() const
        {
            if (ver == 1) return {};
            const int na = ver / 7 + 2;
            const int step = (ver * 4 + na * 2 + 1) / (na * 2 - 2) * 2;
            std::vector<int> r ((size_t) na);
            r[0] = 6;
            for (int i = na - 1, pos = size - 7; i >= 1; --i, pos -= step) r[(size_t) i] = pos;
            return r;
        }
        void formatBits (int mask)
        {
            const int data = (0 << 3) | mask;   // M = 0
            int rem = data;
            for (int i = 0; i < 10; ++i) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
            const int bits = ((data << 10) | rem) ^ 0x5412;
            for (int i = 0; i <= 5; ++i) setFn (8, i, bit (bits, i));
            setFn (8, 7, bit (bits, 6)); setFn (8, 8, bit (bits, 7)); setFn (7, 8, bit (bits, 8));
            for (int i = 9; i < 15; ++i) setFn (14 - i, 8, bit (bits, i));
            for (int i = 0; i < 8; ++i) setFn (size - 1 - i, 8, bit (bits, i));
            for (int i = 8; i < 15; ++i) setFn (8, size - 15 + i, bit (bits, i));
            setFn (8, size - 8, true);
        }
        void versionBits()
        {
            if (ver < 7) return;
            int rem = ver;
            for (int i = 0; i < 12; ++i) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
            const long bits = ((long) ver << 12) | rem;
            for (int i = 0; i < 18; ++i)
            {
                const bool b = bit (bits, i);
                const int a = size - 11 + i % 3, c = i / 3;
                setFn (a, c, b); setFn (c, a, b);
            }
        }
        void functionPatterns()
        {
            for (int i = 0; i < size; ++i) { setFn (6, i, i % 2 == 0); setFn (i, 6, i % 2 == 0); }
            finder (3, 3); finder (size - 4, 3); finder (3, size - 4);
            const auto ap = alignPositions(); const int n = (int) ap.size();
            for (int i = 0; i < n; ++i)
                for (int j = 0; j < n; ++j)
                    if (! ((i == 0 && j == 0) || (i == 0 && j == n - 1) || (i == n - 1 && j == 0))) align (ap[(size_t) i], ap[(size_t) j]);
            formatBits (0); versionBits();
        }
        void drawCodewords (const std::vector<uint8_t>& data)
        {
            size_t i = 0;
            for (int right = size - 1; right >= 1; right -= 2)
            {
                if (right == 6) right = 5;
                for (int vert = 0; vert < size; ++vert)
                    for (int j = 0; j < 2; ++j)
                    {
                        const int x = right - j;
                        const bool upward = ((right + 1) & 2) == 0;
                        const int y = upward ? size - 1 - vert : vert;
                        if (! fn[(size_t) y][(size_t) x] && i < data.size() * 8)
                        {
                            mod[(size_t) y][(size_t) x] = bit (data[i >> 3], 7 - (int) (i & 7));
                            ++i;
                        }
                    }
            }
        }
        void applyMask (int mask)
        {
            for (int y = 0; y < size; ++y)
                for (int x = 0; x < size; ++x)
                {
                    bool inv = false;
                    switch (mask)
                    {
                        case 0: inv = (x + y) % 2 == 0; break;
                        case 1: inv = y % 2 == 0; break;
                        case 2: inv = x % 3 == 0; break;
                        case 3: inv = (x + y) % 3 == 0; break;
                        case 4: inv = (x / 3 + y / 2) % 2 == 0; break;
                        case 5: inv = x * y % 2 + x * y % 3 == 0; break;
                        case 6: inv = (x * y % 2 + x * y % 3) % 2 == 0; break;
                        default: inv = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
                    }
                    if (! fn[(size_t) y][(size_t) x] && inv) mod[(size_t) y][(size_t) x] = ! mod[(size_t) y][(size_t) x];
                }
        }
        int penalty() const
        {
            int p = 0;
            auto line = [&] (bool horiz)
            {
                for (int a = 0; a < size; ++a)
                {
                    int run = 1; bool prev = horiz ? mod[(size_t) a][0] : mod[0][(size_t) a];
                    std::vector<bool> s;
                    for (int b = 0; b < size; ++b)
                    {
                        const bool v = horiz ? mod[(size_t) a][(size_t) b] : mod[(size_t) b][(size_t) a];
                        s.push_back (v);
                        if (b > 0) { if (v == prev) { ++run; if (run == 5) p += 3; else if (run > 5) p += 1; } else { run = 1; prev = v; } }
                    }
                    for (int b = 0; b + 10 < size + 0 && b + 10 <= size - 1 + 0; ++b)
                    {
                        static const bool P1[11] = { 1,0,1,1,1,0,1,0,0,0,0 }, P2[11] = { 0,0,0,0,1,0,1,1,1,0,1 };
                        bool m1 = true, m2 = true;
                        for (int k = 0; k < 11; ++k) { if (s[(size_t) (b + k)] != (bool) P1[k]) m1 = false; if (s[(size_t) (b + k)] != (bool) P2[k]) m2 = false; }
                        if (m1) p += 40;
                        if (m2) p += 40;
                    }
                }
            };
            line (true); line (false);
            int dark = 0;
            for (int y = 0; y < size; ++y)
                for (int x = 0; x < size; ++x)
                {
                    if (mod[(size_t) y][(size_t) x]) ++dark;
                    if (x + 1 < size && y + 1 < size && mod[(size_t) y][(size_t) x] == mod[(size_t) y][(size_t) x + 1] && mod[(size_t) y][(size_t) x] == mod[(size_t) y + 1][(size_t) x] && mod[(size_t) y][(size_t) x] == mod[(size_t) y + 1][(size_t) x + 1]) p += 3;
                }
            const int total = size * size;
            const int k = (std::abs (dark * 20 - total * 10) + total - 1) / total - 1;
            return p + std::max (0, k) * 10;
        }
    };
}

// Επιστρέφει QR για το κείμενο (UTF-8). size == 0 αν δεν χωράει (πάνω από ~150 byte).
inline Code encode (const std::string& text)
{
    using namespace detail;
    int ver = 0;
    for (int v = 1; v <= 10; ++v)
    {
        const int capBits = dataCodewords (v) * 8;
        const int used = 4 + (v < 10 ? 8 : 16) + (int) text.size() * 8;
        if (used <= capBits) { ver = v; break; }
    }
    if (ver == 0) return {};

    // bits δεδομένων
    std::vector<bool> bits;
    auto put = [&] (unsigned val, int n) { for (int i = n - 1; i >= 0; --i) bits.push_back (((val >> i) & 1) != 0); };
    put (0x4, 4);
    put ((unsigned) text.size(), ver < 10 ? 8 : 16);
    for (unsigned char c : text) put (c, 8);
    const size_t capBits = (size_t) dataCodewords (ver) * 8;
    for (int i = 0; i < 4 && bits.size() < capBits; ++i) bits.push_back (false);
    while (bits.size() % 8 != 0) bits.push_back (false);
    std::vector<uint8_t> data (capBits / 8, 0);
    for (size_t i = 0; i < bits.size(); ++i) if (bits[i]) data[i >> 3] |= (uint8_t) (1 << (7 - (i & 7)));
    for (size_t i = bits.size() / 8, pad = 0xEC; i < data.size(); ++i, pad ^= 0xEC ^ 0x11) data[i] = (uint8_t) pad;

    // μπλοκ + Reed-Solomon + συνένωση
    const int nb = NUM_BLOCKS_M[ver], eccLen = ECC_PER_BLOCK_M[ver];
    const int raw = rawModules (ver) / 8, shortBlocks = nb - raw % nb, shortLen = raw / nb;
    const auto div = rsDivisor (eccLen);
    std::vector<std::vector<uint8_t>> blocks;
    for (int i = 0, k = 0; i < nb; ++i)
    {
        const int len = shortLen - eccLen + (i < shortBlocks ? 0 : 1);
        std::vector<uint8_t> dat (data.begin() + k, data.begin() + k + len); k += len;
        const auto ecc = rsRemainder (dat, div);
        if (i < shortBlocks) dat.push_back (0);
        dat.insert (dat.end(), ecc.begin(), ecc.end());
        blocks.push_back (dat);
    }
    std::vector<uint8_t> all;
    for (size_t i = 0; i < blocks[0].size(); ++i)
        for (int j = 0; j < nb; ++j)
            if (i != (size_t) (shortLen - eccLen) || j >= shortBlocks) all.push_back (blocks[(size_t) j][i]);

    Builder b (ver);
    b.functionPatterns();
    b.drawCodewords (all);
    int best = 0, bestPen = 1 << 30;
    for (int m = 0; m < 8; ++m)
    {
        b.applyMask (m); b.formatBits (m);
        const int p = b.penalty();
        if (p < bestPen) { bestPen = p; best = m; }
        b.applyMask (m);   // αναίρεση
    }
    b.applyMask (best); b.formatBits (best);
    Code c; c.size = b.size; c.m = b.mod;
    return c;
}
}
