// Offline checks for the suppressor engine.
#include "SpectralSuppressor.h"
#include <cstdio>
#include <random>

namespace
{
    constexpr double sr = 48000.0;
    constexpr int blockSize = 480;
    int failures = 0;

    void check (bool ok, const char* what)
    {
        std::printf ("[%s] %s\n", ok ? "PASS" : "FAIL", what);
        if (! ok) ++failures;
    }

    // Run a stereo buffer through the engine in blocks
    void run (SpectralSuppressor& e, juce::AudioBuffer<float>& buf, const SuppressorParams& p)
    {
        for (int start = 0; start < buf.getNumSamples(); start += blockSize)
        {
            const int n = juce::jmin (blockSize, buf.getNumSamples() - start);
            juce::AudioBuffer<float> view (buf.getArrayOfWritePointers(), buf.getNumChannels(), start, n);
            e.process (view, p);
        }
    }

    // Amplitude of a sinusoid at freq over [from, to)
    double toneAmplitude (const float* x, int from, int to, double freq)
    {
        double c = 0.0, s = 0.0;
        for (int i = from; i < to; ++i)
        {
            const double ph = juce::MathConstants<double>::twoPi * freq * i / sr;
            c += x[i] * std::cos (ph);
            s += x[i] * std::sin (ph);
        }
        return 2.0 * std::sqrt (c * c + s * s) / (to - from);
    }

    double rms (const float* x, int from, int to)
    {
        double a = 0.0;
        for (int i = from; i < to; ++i) a += (double) x[i] * x[i];
        return std::sqrt (a / (to - from));
    }

    juce::AudioBuffer<float> makeSignal (int len, double toneFreq, float toneAmp, unsigned seed)
    {
        juce::AudioBuffer<float> b (2, len);
        std::mt19937 rng (seed);
        std::normal_distribution<float> nd (0.0f, 0.05f);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < len; ++i)
                b.setSample (ch, i, nd (rng) + toneAmp * (float) std::sin (juce::MathConstants<double>::twoPi * toneFreq * i / sr));
        return b;
    }

    // dB reduction of a 3 kHz resonance over noise with the given settings
    double resonanceReduction (const SuppressorParams& p)
    {
        const int len = (int) sr * 4;
        SpectralSuppressor e;
        e.prepare (sr, 2);
        const int lat = e.getLatencySamples();
        auto in = makeSignal (len, 3000.0, 0.3f, 7);
        juce::AudioBuffer<float> out (in);
        run (e, out, p);
        const double before = toneAmplitude (in.getReadPointer (0), len / 2 - lat, len - lat, 3000.0);
        const double after  = toneAmplitude (out.getReadPointer (0), len / 2, len, 3000.0);
        return 20.0 * std::log10 (before / after);
    }
}

int main()
{
    const int len = (int) sr * 4;

    // 1. Depth 0 -> output is the input delayed by the latency
    {
        SpectralSuppressor e;
        e.prepare (sr, 2);
        const int lat = e.getLatencySamples();
        auto in = makeSignal (len, 3000.0, 0.3f, 1);
        juce::AudioBuffer<float> out (in);
        SuppressorParams p; p.depthDb = 0.0f;
        run (e, out, p);

        float maxErr = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = lat + 4096; i < len; ++i)
                maxErr = juce::jmax (maxErr, std::abs (out.getSample (ch, i) - in.getSample (ch, i - lat)));
        std::printf ("    latency %d samples, max error %.2e\n", lat, maxErr);
        check (maxErr < 1.0e-4f, "transparent at depth 0 (latency-aligned null)");
    }

    // 2. A loud narrow resonance over noise is pulled down; noise mostly untouched
    {
        SpectralSuppressor e;
        e.prepare (sr, 2);
        const int lat = e.getLatencySamples();
        auto in = makeSignal (len, 3000.0, 0.3f, 2);
        juce::AudioBuffer<float> out (in);
        SuppressorParams p; p.depthDb = 12.0f;
        run (e, out, p);

        const int from = len / 2, to = len;
        const double before = toneAmplitude (in.getReadPointer (0), from - lat, to - lat, 3000.0);
        const double after  = toneAmplitude (out.getReadPointer (0), from, to, 3000.0);
        const double redDb  = 20.0 * std::log10 (before / after);
        std::printf ("    3 kHz resonance reduced by %.2f dB\n", redDb);
        check (redDb > 6.0, "resonance reduced by more than 6 dB at depth 12");

        // Noise-only comparison: remove the tone from both and compare residual RMS
        auto noiseOnly = makeSignal (len, 3000.0, 0.0f, 2);
        juce::AudioBuffer<float> noiseOut (noiseOnly);
        SpectralSuppressor e2; e2.prepare (sr, 2);
        run (e2, noiseOut, p);
        const double nIn  = rms (noiseOnly.getReadPointer (0), from - lat, to - lat);
        const double nOut = rms (noiseOut.getReadPointer (0), from, to);
        const double noiseChange = 20.0 * std::log10 (nOut / nIn);
        std::printf ("    white noise level change %.2f dB\n", noiseChange);
        check (noiseChange > -1.5, "broadband noise is mostly left alone (< 1.5 dB change)");
    }

    // 3. Delta + processed = dry
    {
        auto in = makeSignal (len, 1500.0, 0.4f, 3);
        juce::AudioBuffer<float> wet (in), del (in);
        SpectralSuppressor a, b;
        a.prepare (sr, 2); b.prepare (sr, 2);
        SuppressorParams p; p.depthDb = 10.0f;
        run (a, wet, p);
        p.delta = true;
        run (b, del, p);
        const int lat = a.getLatencySamples();
        float maxErr = 0.0f;
        for (int i = lat + 4096; i < len; ++i)
            maxErr = juce::jmax (maxErr, std::abs (wet.getSample (0, i) + del.getSample (0, i) - in.getSample (0, i - lat)));
        std::printf ("    wet + delta vs dry max error %.2e\n", maxErr);
        check (maxErr < 1.0e-4f, "delta output is exactly what was removed");
    }

    // 4. The sensitivity curve steers where processing happens
    {
        SuppressorParams p; p.depthDb = 8.0f;
        const double flat = resonanceReduction (p);

        SuppressorParams up = p;
        up.curve.bands[SensitivityCurve::Bell2] = { 3000.0f, 12.0f, 1.0f };
        const double boosted = resonanceReduction (up);

        SuppressorParams down = p;
        down.curve.bands[SensitivityCurve::Bell2] = { 3000.0f, -12.0f, 1.0f };
        const double reduced = resonanceReduction (down);

        SuppressorParams cut = p;
        cut.curve.bands[SensitivityCurve::LowCut].freq = 10000.0f;
        const double outside = resonanceReduction (cut);

        std::printf ("    3 kHz reduction: flat %.2f, bell +12 %.2f, bell -12 %.2f, low cut @10k %.2f dB\n",
                     flat, boosted, reduced, outside);
        check (boosted > flat + 2.0, "raising a sensitivity bell increases reduction there");
        check (reduced < flat * 0.5, "lowering a sensitivity bell decreases reduction there");
        check (outside < 0.5, "a low cut above the resonance stops processing it");
    }

    // 5. Zero-latency mode (the plugin's LOW LATENCY button)
    for (double rate : { 48000.0, 96000.0 })
    {
        using Mode = SpectralSuppressor::Mode;
        const int n = (int) rate * 4;
        std::printf ("  zero-latency mode @ %.0f kHz\n", rate / 1000.0);

        auto signal = [rate, n] (double toneAmp, unsigned seed)
        {
            juce::AudioBuffer<float> b (2, n);
            std::mt19937 rng (seed);
            std::normal_distribution<float> nd (0.0f, 0.05f);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < n; ++i)
                    b.setSample (ch, i, nd (rng) + (float) (toneAmp * std::sin (juce::MathConstants<double>::twoPi * 3000.0 * i / rate)));
            return b;
        };
        auto runAt = [rate] (SpectralSuppressor& e, juce::AudioBuffer<float>& buf, const SuppressorParams& p)
        {
            e.prepare (rate, 2, Mode::ZeroLatency);
            const int bs = (int) (rate / 100.0);
            for (int start = 0; start < buf.getNumSamples(); start += bs)
            {
                juce::AudioBuffer<float> view (buf.getArrayOfWritePointers(), 2, start, juce::jmin (bs, buf.getNumSamples() - start));
                e.process (view, p);
            }
        };
        auto toneAmp = [rate] (const float* x, int from, int to)
        {
            double c = 0.0, s = 0.0;
            for (int i = from; i < to; ++i)
            {
                const double ph = juce::MathConstants<double>::twoPi * 3000.0 * i / rate;
                c += x[i] * std::cos (ph);
                s += x[i] * std::sin (ph);
            }
            return 2.0 * std::sqrt (c * c + s * s) / (to - from);
        };

        SpectralSuppressor e;
        e.prepare (rate, 2, Mode::ZeroLatency);
        const int lat = e.getLatencySamples();
        std::printf ("    latency %d samples (%.2f ms)\n", lat, 1000.0 * lat / rate);
        check (1000.0 * lat / rate <= 1.0, "low-latency mode latency <= 1 ms");

        // Depth 0 -> output identical to input, no delay
        {
            auto in = signal (0.3, 11);
            juce::AudioBuffer<float> out (in);
            SuppressorParams p; p.depthDb = 0.0f;
            runAt (e, out, p);
            float maxErr = 0.0f;
            for (int i = 0; i < n; ++i)
                maxErr = juce::jmax (maxErr, std::abs (out.getSample (0, i) - in.getSample (0, i)));
            std::printf ("    depth 0 max error vs undelayed input %.2e\n", maxErr);
            check (maxErr < 1.0e-4f, "zero-latency mode is transparent at depth 0 (no delay)");
        }

        // Resonance reduction, noise untouched, filter is minimum-phase
        {
            auto in = signal (0.3, 12);
            juce::AudioBuffer<float> out (in);
            SuppressorParams p; p.depthDb = 12.0f;
            SpectralSuppressor z;
            runAt (z, out, p);
            const double red = 20.0 * std::log10 (toneAmp (in.getReadPointer (0), n / 2, n) / toneAmp (out.getReadPointer (0), n / 2, n));

            const auto ir = z.getFilter (0)->getImpulseResponse();
            double total = 0.0, early = 0.0;
            for (size_t k = 0; k < ir.size(); ++k)
            {
                total += (double) ir[k] * ir[k];
                if (k < ir.size() / 8) early += (double) ir[k] * ir[k];
            }

            auto noise = signal (0.0, 12);
            juce::AudioBuffer<float> nOut (noise);
            SpectralSuppressor z2;
            runAt (z2, nOut, p);
            const double nChange = 20.0 * std::log10 (rms (nOut.getReadPointer (0), n / 2, n) / rms (noise.getReadPointer (0), n / 2, n));

            std::printf ("    resonance -%.2f dB, white noise %.2f dB, IR energy in first 1/8: %.1f%%\n",
                         red, nChange, 100.0 * early / total);
            check (red > 6.0, "zero-latency mode reduces the resonance by > 6 dB");
            check (nChange > -1.5, "zero-latency mode leaves broadband noise mostly alone");
            check (early / total > 0.95, "filter is minimum-phase (energy at the start, no pre-ringing)");
        }

        // Wet + delta = dry, sample-aligned
        {
            auto in = signal (0.4, 13);
            juce::AudioBuffer<float> wet (in), del (in);
            SuppressorParams p; p.depthDb = 10.0f;
            SpectralSuppressor a, b;
            runAt (a, wet, p);
            p.delta = true;
            runAt (b, del, p);
            float maxErr = 0.0f;
            for (int i = 0; i < n; ++i)
                maxErr = juce::jmax (maxErr, std::abs (wet.getSample (0, i) + del.getSample (0, i) - in.getSample (0, i)));
            check (maxErr < 1.0e-4f, "zero-latency delta is exactly what was removed");
        }
    }

    // 6. CPU cost (stereo, 10 s of audio, 128-sample blocks)
    for (auto mode : { SpectralSuppressor::Mode::Quality, SpectralSuppressor::Mode::ZeroLatency })
        for (double rate : { 48000.0, 96000.0 })
        {
            SpectralSuppressor e;
            e.prepare (rate, 2, mode);
            juce::AudioBuffer<float> buf (2, 128);
            std::mt19937 rng (5);
            std::normal_distribution<float> nd (0.0f, 0.1f);
            SuppressorParams p; p.depthDb = 10.0f;
            const int blocks = (int) (rate * 10.0 / 128.0);
            const auto t0 = juce::Time::getHighResolutionTicks();
            for (int b = 0; b < blocks; ++b)
            {
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < 128; ++i)
                        buf.setSample (ch, i, nd (rng));
                e.process (buf, p);
            }
            const double secs = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);
            const double load = 100.0 * secs / 10.0;
            std::printf ("    %s @ %.0f kHz: %.1f%% of one core\n",
                         mode == SpectralSuppressor::Mode::Quality ? "quality     " : "zero-latency", rate / 1000.0, load);
            check (load < 25.0, "runs comfortably in real time (< 25% of one core)");
        }

    std::printf (failures == 0 ? "\nAll tests passed.\n" : "\n%d test(s) failed.\n", failures);
    return failures == 0 ? 0 : 1;
}
