#pragma once

#include <array>
#include <cmath>

/*
    EQ-style sensitivity curve. It is not applied to the audio. It shapes how hard
    the suppressor works at each frequency.

      +dB  -> more depth and a lower detection threshold (more aggressive)
      -dB  -> less depth and a higher threshold (gentler)
      cuts -> processing fades out beyond the cut frequency
*/
struct SensitivityCurve
{
    enum Band { LowCut, LowShelf, Bell1, Bell2, Bell3, HighShelf, HighCut, NumBands };

    struct BandInfo
    {
        const char* id;
        const char* name;
        const char* shortName;
        bool hasGain, hasQ;
        float defaultFreq;
    };

    static constexpr float minFreq = 20.0f, maxFreq = 20000.0f;
    static constexpr float maxGainDb = 12.0f;
    static constexpr float minQ = 0.3f, maxQ = 8.0f;

    static constexpr std::array<BandInfo, NumBands> info {{
        { "lc", "Low Cut",    "LC", false, false, 20.0f },
        { "ls", "Low Shelf",  "LS", true,  false, 120.0f },
        { "b1", "Bell 1",     "1",  true,  true,  400.0f },
        { "b2", "Bell 2",     "2",  true,  true,  2500.0f },
        { "b3", "Bell 3",     "3",  true,  true,  6500.0f },
        { "hs", "High Shelf", "HS", true,  false, 10000.0f },
        { "hc", "High Cut",   "HC", false, false, 20000.0f },
    }};

    struct BandValues { float freq = 1000.0f, gainDb = 0.0f, q = 1.0f; };
    std::array<BandValues, NumBands> bands;

    SensitivityCurve()
    {
        for (int i = 0; i < NumBands; ++i)
            bands[(size_t) i].freq = info[(size_t) i].defaultFreq;
    }

    bool operator== (const SensitivityCurve& o) const noexcept
    {
        for (int i = 0; i < NumBands; ++i)
        {
            const auto& a = bands[(size_t) i];
            const auto& b = o.bands[(size_t) i];
            if (a.freq != b.freq || a.gainDb != b.gainDb || a.q != b.q)
                return false;
        }
        return true;
    }
    bool operator!= (const SensitivityCurve& o) const noexcept { return ! (*this == o); }

    // Shelves + bells, in dB
    float eqDb (float f) const noexcept
    {
        auto oct = [f] (float fc) { return std::log2 (f / fc); };
        float db = 0.0f;

        const auto& ls = bands[LowShelf];
        db += ls.gainDb / (1.0f + std::exp2 (3.0f * oct (ls.freq)));

        const auto& hs = bands[HighShelf];
        db += hs.gainDb / (1.0f + std::exp2 (-3.0f * oct (hs.freq)));

        for (int b : { Bell1, Bell2, Bell3 })
        {
            const auto& bell = bands[(size_t) b];
            const float sigma = 0.6f / bell.q;   // octaves
            const float x = oct (bell.freq) / sigma;
            db += bell.gainDb * std::exp (-0.5f * x * x);
        }
        return db;
    }

    // 0..1 fade from the cut bands. A cut parked at the edge of the range is off.
    float cutWeight (float f) const noexcept
    {
        float w = 1.0f;
        const float lc = bands[LowCut].freq, hc = bands[HighCut].freq;
        if (lc > minFreq * 1.02f)
            w *= 1.0f / (1.0f + std::pow (lc / f, 4.0f));
        if (hc < maxFreq * 0.98f)
            w *= 1.0f / (1.0f + std::pow (f / hc, 4.0f));
        return w;
    }

    // What the display draws: EQ part plus the cuts expressed in dB
    float displayDb (float f) const noexcept
    {
        return eqDb (f) + 20.0f * std::log10 (std::fmax (cutWeight (f), 1.0e-4f));
    }

    float depthMultiplier (float f) const noexcept { return std::exp2 (eqDb (f) / 6.0f) * cutWeight (f); }
    float thresholdShiftDb (float f) const noexcept { return -0.5f * eqDb (f); }
};
