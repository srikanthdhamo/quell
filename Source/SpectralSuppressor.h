#pragma once

#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <vector>
#include "SensitivityCurve.h"
#include "MinPhaseFilter.h"

/*
    Dynamic resonance suppressor engine.

    Short-time Fourier transform with 75% overlap (Hann analysis + synthesis).
    For every frame and every bin:
      1. Measure the bin level in dB.
      2. Estimate a smooth "expected" spectrum by averaging the dB spectrum over a
         log-frequency window (narrower window = higher sharpness). The average is
         taken twice, the second time with peaks clipped, so resonances do not pull
         the envelope up toward themselves.
      3. Whatever sticks out above the envelope by more than the selectivity
         threshold is a resonance. It is mapped through a soft-knee curve that
         saturates at the depth setting. The EQ-style sensitivity curve scales
         depth and shifts the threshold per frequency.
      4. Reduction is optionally linked across channels, blurred slightly across
         frequency, and smoothed over time with separate attack and release.
      5. The resulting per-bin gain (mixed, or inverted for delta listening) is
         applied to the spectrum and resynthesised.

    Two modes:
      Quality      - the gains are applied to the STFT and resynthesised.
                     Latency is one FFT frame (fftSize samples).
      ZeroLatency  - the STFT only analyses. Each hop its gains become a
                     minimum-phase FIR (MinPhaseFilter) that filters the live
                     signal, so the audio path has no latency. Reduction reacts
                     slightly later than in Quality mode because there is no
                     look-ahead.
*/

struct SuppressorParams
{
    float depthDb      = 6.0f;    // max reduction, 0..24 dB
    float sharpness    = 4.0f;    // 1..10
    float selectivity  = 3.5f;    // 0..10
    float attackMs     = 5.0f;
    float releaseMs    = 60.0f;
    SensitivityCurve curve;       // EQ-style per-frequency sensitivity
    float link         = 1.0f;    // 0..1 stereo link
    float mix          = 1.0f;    // 0..1
    float outGainDb    = 0.0f;
    bool  delta        = false;   // listen to what is being removed
};

class SpectralSuppressor
{
public:
    static constexpr int displayPoints = 256;
    static constexpr float displayMinHz = 20.0f, displayMaxHz = 20000.0f;

    SpectralSuppressor()
    {
        for (auto& v : displayInputDb)     v.store (-120.0f);
        for (auto& v : displayReductionDb) v.store (0.0f);
    }

    enum class Mode { Quality, ZeroLatency };

    void prepare (double newSampleRate, int newNumChannels, Mode newMode = Mode::Quality)
    {
        sampleRate  = newSampleRate;
        numChannels = juce::jmax (1, newNumChannels);
        mode        = newMode;

        // Frame ~43 ms in Quality mode. Zero-latency mode analyses with a ~21 ms frame
        // (faster reaction) and uses an FIR of the same length.
        const int order = (sampleRate <= 50000.0 ? 11 : (sampleRate <= 100000.0 ? 12 : 13))
                        - (mode == Mode::ZeroLatency ? 1 : 0);
        fftSize = 1 << order;
        hopSize = fftSize / 4;
        numBins = fftSize / 2 + 1;
        fft = std::make_unique<juce::dsp::FFT> (order);

        window.resize ((size_t) fftSize);
        for (int i = 0; i < fftSize; ++i)
            window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) fftSize);

        // Hann^2 at 75% overlap sums to 1.5
        olaScale = 1.0f / 1.5f;

        auto resizeAll = [this] (std::vector<std::vector<float>>& v, size_t n)
        {
            v.assign ((size_t) numChannels, std::vector<float> (n, 0.0f));
        };
        resizeAll (inFifo,  (size_t) fftSize);
        resizeAll (outFifo, (size_t) fftSize);
        resizeAll (fftData, (size_t) fftSize * 2);
        resizeAll (binDb,   (size_t) numBins);
        resizeAll (target,  (size_t) numBins);
        resizeAll (smoothed,(size_t) numBins);
        resizeAll (detectPower, (size_t) numBins);
        resizeAll (finalGain, (size_t) numBins);

        filters.clear();
        if (mode == Mode::ZeroLatency)
        {
            filters.resize ((size_t) numChannels);
            for (auto& f : filters)
                f.prepare (fftSize, numBins, hopSize);
        }

        // ~20 ms detection smoothing
        detectCoef = 1.0f - std::exp (-((float) hopSize / (float) sampleRate) / 0.020f);

        prefix.assign ((size_t) numBins + 1, 0.0);
        envelope.assign ((size_t) numBins, 0.0f);
        clipped.assign ((size_t) numBins, 0.0f);
        scratch.assign ((size_t) numBins, 0.0f);
        winLo.assign ((size_t) numBins, 0);
        winHi.assign ((size_t) numBins, 0);
        weight.assign ((size_t) numBins, 0.0f);
        thrShift.assign ((size_t) numBins, 0.0f);
        displayInputState.assign ((size_t) displayPoints, -120.0f);

        // Normalise so a full-scale sine reads ~0 dB
        dbOffset = -20.0f * std::log10 ((float) fftSize * 0.25f);

        cachedSharpness = -1.0e9f;
        curveValid = false;
        reset();
    }

    void reset()
    {
        for (auto* set : { &inFifo, &outFifo, &smoothed, &target, &detectPower })
            for (auto& ch : *set)
                std::fill (ch.begin(), ch.end(), 0.0f);
        fifoPos = 0;
        hopCounter = 0;
        for (auto& f : filters)
            f.reset();
    }

    int getLatencySamples() const noexcept { return mode == Mode::Quality ? fftSize : 0; }
    Mode getMode() const noexcept { return mode; }
    const MinPhaseFilter* getFilter (int ch) const { return (size_t) ch < filters.size() ? &filters[(size_t) ch] : nullptr; }

    void process (juce::AudioBuffer<float>& buffer, const SuppressorParams& p)
    {
        const int chans = juce::jmin (buffer.getNumChannels(), numChannels);
        const int n = buffer.getNumSamples();
        const int mask = fftSize - 1;
        activeChannels = chans;

        if (mode == Mode::ZeroLatency)
        {
            processZeroLatency (buffer, p, chans);
            return;
        }

        for (int i = 0; i < n; ++i)
        {
            for (int ch = 0; ch < chans; ++ch)
            {
                auto* d = buffer.getWritePointer (ch);
                inFifo[(size_t) ch][(size_t) fifoPos] = d[i];
                d[i] = outFifo[(size_t) ch][(size_t) fifoPos];
                outFifo[(size_t) ch][(size_t) fifoPos] = 0.0f;
            }

            fifoPos = (fifoPos + 1) & mask;

            if (++hopCounter >= hopSize)
            {
                hopCounter = 0;
                processFrame (p, chans);
            }
        }
    }

    // GUI data (written by the audio thread, read by the editor)
    std::array<std::atomic<float>, displayPoints> displayInputDb;
    std::array<std::atomic<float>, displayPoints> displayReductionDb;

    static float displayFrequency (int i) noexcept
    {
        return displayMinHz * std::pow (displayMaxHz / displayMinHz, (float) i / (float) (displayPoints - 1));
    }

private:
    void processZeroLatency (juce::AudioBuffer<float>& buffer, const SuppressorParams& p, int chans)
    {
        const int n = buffer.getNumSamples();
        const int mask = fftSize - 1;
        const float outGain = juce::Decibels::decibelsToGain (p.outGainDb);

        for (int i = 0; i < n; ++i)
        {
            for (int ch = 0; ch < chans; ++ch)
            {
                auto* d = buffer.getWritePointer (ch);
                const float x = d[i];
                inFifo[(size_t) ch][(size_t) fifoPos] = x;
                const float y = filters[(size_t) ch].processSample (x);   // wet (mix included)
                d[i] = (p.delta ? x - y : y) * outGain;
            }

            fifoPos = (fifoPos + 1) & mask;

            if (++hopCounter >= hopSize)
            {
                hopCounter = 0;
                processFrame (p, chans);
            }
        }
    }

    void updateAnalysisWindows (float sharpness)
    {
        if (sharpness == cachedSharpness)
            return;
        cachedSharpness = sharpness;

        // Envelope width: ~1.5 octaves at sharpness 1 down to ~1/8 octave at 10
        const float t = juce::jlimit (0.0f, 1.0f, (sharpness - 1.0f) / 9.0f);
        const float octaves = std::pow (2.0f, juce::jmap (t, std::log2 (1.5f), std::log2 (0.125f)));
        const float half = std::pow (2.0f, octaves * 0.5f);

        for (int b = 0; b < numBins; ++b)
        {
            int lo = (int) std::floor ((float) b / half);
            int hi = (int) std::ceil  ((float) b * half);
            // Always look at least a few bins either side so single-bin peaks are visible
            lo = juce::jmin (lo, b - 3);
            hi = juce::jmax (hi, b + 3);
            winLo[(size_t) b] = juce::jlimit (1, numBins - 1, lo);
            winHi[(size_t) b] = juce::jlimit (1, numBins - 1, hi);
        }

        blurPasses = juce::roundToInt (juce::jmap (t, 3.0f, 0.0f));
    }

    void updateWeights (const SensitivityCurve& curve)
    {
        if (curveValid && curve == cachedCurve)
            return;
        cachedCurve = curve;
        curveValid = true;

        for (int b = 0; b < numBins; ++b)
        {
            const float f = juce::jmax (1.0f, (float) b * (float) sampleRate / (float) fftSize);
            weight[(size_t) b]   = (b == 0) ? 0.0f : curve.depthMultiplier (f);
            thrShift[(size_t) b] = curve.thresholdShiftDb (f);
        }
    }

    void boxAverage (const float* src, float* dst)
    {
        prefix[0] = 0.0;
        for (int b = 0; b < numBins; ++b)
            prefix[(size_t) b + 1] = prefix[(size_t) b] + (double) src[b];

        for (int b = 0; b < numBins; ++b)
        {
            const int lo = winLo[(size_t) b], hi = winHi[(size_t) b];
            dst[b] = (float) ((prefix[(size_t) hi + 1] - prefix[(size_t) lo]) / (double) (hi - lo + 1));
        }
    }

    void computeTargetReduction (int ch, const SuppressorParams& p)
    {
        auto& db  = binDb[(size_t) ch];
        auto& tgt = target[(size_t) ch];
        auto& det = detectPower[(size_t) ch];
        const auto* X = fftData[(size_t) ch].data();

        for (int b = 0; b < numBins; ++b)
        {
            const float re = X[2 * b], im = X[2 * b + 1];
            scratch[(size_t) b] = re * re + im * im;
        }

        // Detection signal: power smoothed over neighbouring bins and a couple of
        // frames, so random noise fluctuations are not mistaken for resonances.
        for (int b = 0; b < numBins; ++b)
        {
            const float l = scratch[(size_t) juce::jmax (0, b - 1)];
            const float r = scratch[(size_t) juce::jmin (numBins - 1, b + 1)];
            const float pw = 0.25f * l + 0.5f * scratch[(size_t) b] + 0.25f * r;
            det[(size_t) b] += detectCoef * (pw - det[(size_t) b]);
            db[(size_t) b] = 10.0f * std::log10 (det[(size_t) b] + 1.0e-20f) + dbOffset;
        }

        // Pass 1: rough envelope. Pass 2: clip peaks to it and average again.
        boxAverage (db.data(), envelope.data());
        for (int b = 0; b < numBins; ++b)
            clipped[(size_t) b] = juce::jmin (db[(size_t) b], envelope[(size_t) b] + 1.5f);
        boxAverage (clipped.data(), envelope.data());

        const float depth = p.depthDb;
        const float threshold = p.selectivity * 1.2f;

        for (int b = 0; b < numBins; ++b)
        {
            const float level = db[(size_t) b];
            const float excess = level - envelope[(size_t) b] - juce::jmax (0.0f, threshold + thrShift[(size_t) b]);
            float r = 0.0f;

            if (excess > 0.0f && depth > 0.0f)
            {
                // Fade out detection for near-silent bins
                const float gate = juce::jlimit (0.0f, 1.0f, (level + 90.0f) / 20.0f);
                const float d = juce::jmin (40.0f, depth * weight[(size_t) b] * gate);
                if (d > 0.0f)
                    r = d * (1.0f - std::exp (-excess / d));
            }
            tgt[(size_t) b] = r;
        }
    }

    void processFrame (const SuppressorParams& p, int chans)
    {
        const int mask = fftSize - 1;

        updateAnalysisWindows (p.sharpness);
        updateWeights (p.curve);

        for (int ch = 0; ch < chans; ++ch)
        {
            auto& buf = fftData[(size_t) ch];
            const auto& in = inFifo[(size_t) ch];
            for (int k = 0; k < fftSize; ++k)
                buf[(size_t) k] = in[(size_t) ((fifoPos + k) & mask)] * window[(size_t) k];
            std::fill (buf.begin() + fftSize, buf.end(), 0.0f);

            fft->performRealOnlyForwardTransform (buf.data());
            computeTargetReduction (ch, p);
        }

        // Stereo link: pull each channel toward the loudest reduction
        if (chans > 1 && p.link > 0.0f)
        {
            for (int b = 0; b < numBins; ++b)
            {
                float m = 0.0f;
                for (int ch = 0; ch < chans; ++ch)
                    m = juce::jmax (m, target[(size_t) ch][(size_t) b]);
                for (int ch = 0; ch < chans; ++ch)
                {
                    auto& t = target[(size_t) ch][(size_t) b];
                    t += p.link * (m - t);
                }
            }
        }

        const float frameSec = (float) hopSize / (float) sampleRate;
        const float aCoef = 1.0f - std::exp (-frameSec / juce::jmax (1.0e-4f, p.attackMs  * 0.001f));
        const float rCoef = 1.0f - std::exp (-frameSec / juce::jmax (1.0e-4f, p.releaseMs * 0.001f));
        const float mix = juce::jlimit (0.0f, 1.0f, p.mix);
        const float outGain = juce::Decibels::decibelsToGain (p.outGainDb);

        for (int ch = 0; ch < chans; ++ch)
        {
            auto& t = target[(size_t) ch];

            // Light blur across frequency, wider at low sharpness
            for (int pass = 0; pass < blurPasses; ++pass)
            {
                scratch[0] = t[0];
                scratch[(size_t) numBins - 1] = t[(size_t) numBins - 1];
                for (int b = 1; b < numBins - 1; ++b)
                    scratch[(size_t) b] = 0.25f * t[(size_t) b - 1] + 0.5f * t[(size_t) b] + 0.25f * t[(size_t) b + 1];
                std::copy (scratch.begin(), scratch.end(), t.begin());
            }

            auto& s = smoothed[(size_t) ch];
            auto& fg = finalGain[(size_t) ch];

            for (int b = 0; b < numBins; ++b)
            {
                const float tb = t[(size_t) b];
                float& sb = s[(size_t) b];
                sb += (tb > sb ? aCoef : rCoef) * (tb - sb);

                const float g = juce::Decibels::decibelsToGain (-sb);
                fg[(size_t) b] = 1.0f - mix * (1.0f - g);   // wet gain
            }

            if (mode == Mode::ZeroLatency)
            {
                // Delta and output gain are applied per sample in processZeroLatency
                filters[(size_t) ch].design (fg.data());
                continue;
            }

            auto* X = fftData[(size_t) ch].data();
            for (int b = 0; b < numBins; ++b)
            {
                const float wet = fg[(size_t) b];
                const float gain = (p.delta ? (1.0f - wet) : wet) * outGain;

                X[2 * b]     *= gain;
                X[2 * b + 1] *= gain;
                if (b > 0 && b < fftSize / 2)
                {
                    X[2 * (fftSize - b)]     *= gain;
                    X[2 * (fftSize - b) + 1] *= gain;
                }
            }

            fft->performRealOnlyInverseTransform (X);

            auto& out = outFifo[(size_t) ch];
            for (int k = 0; k < fftSize; ++k)
                out[(size_t) ((fifoPos + k) & mask)] += X[k] * window[(size_t) k] * olaScale;
        }

        updateDisplay (chans);
    }

    void updateDisplay (int chans)
    {
        for (int i = 0; i < displayPoints; ++i)
        {
            const float f = displayFrequency (i);
            const int b = juce::jlimit (1, numBins - 1, juce::roundToInt (f * (float) fftSize / (float) sampleRate));

            float level = -120.0f, red = 0.0f;
            for (int ch = 0; ch < chans; ++ch)
            {
                level = juce::jmax (level, binDb[(size_t) ch][(size_t) b]);
                red   = juce::jmax (red, smoothed[(size_t) ch][(size_t) b]);
            }

            auto& st = displayInputState[(size_t) i];
            st = juce::jmax (level, st * 0.85f + level * 0.15f);  // fast rise, slow fall
            st = juce::jmax (st, -120.0f);
            displayInputDb[(size_t) i].store (st, std::memory_order_relaxed);
            displayReductionDb[(size_t) i].store (red, std::memory_order_relaxed);
        }
    }

    Mode mode = Mode::Quality;
    double sampleRate = 44100.0;
    int numChannels = 2, activeChannels = 2;
    int fftSize = 2048, hopSize = 512, numBins = 1025;
    int fifoPos = 0, hopCounter = 0, blurPasses = 1;
    float olaScale = 1.0f, dbOffset = 0.0f, detectCoef = 1.0f;
    float cachedSharpness = -1.0f;
    SensitivityCurve cachedCurve;
    bool curveValid = false;

    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> window, envelope, clipped, scratch, weight, thrShift, displayInputState;
    std::vector<double> prefix;
    std::vector<int> winLo, winHi;
    std::vector<std::vector<float>> inFifo, outFifo, fftData, binDb, target, smoothed, detectPower, finalGain;
    std::vector<MinPhaseFilter> filters;
};
