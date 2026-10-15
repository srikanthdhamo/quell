#pragma once

#include <juce_dsp/juce_dsp.h>
#include <complex>
#include <vector>

#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(_M_ARM64)
 #include <arm_neon.h>
 #define QUELL_NEON 1
#elif defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
 #include <emmintrin.h>
 #define QUELL_SSE 1
#endif

namespace quell
{
    // Dot product, n must be a multiple of 8. Unaligned loads are fine.
    inline float dot (const float* a, const float* b, int n) noexcept
    {
       #if QUELL_NEON
        float32x4_t s0 = vdupq_n_f32 (0.0f), s1 = vdupq_n_f32 (0.0f);
        for (int i = 0; i < n; i += 8)
        {
            s0 = vfmaq_f32 (s0, vld1q_f32 (a + i),     vld1q_f32 (b + i));
            s1 = vfmaq_f32 (s1, vld1q_f32 (a + i + 4), vld1q_f32 (b + i + 4));
        }
        return vaddvq_f32 (vaddq_f32 (s0, s1));
       #elif QUELL_SSE
        __m128 s0 = _mm_setzero_ps(), s1 = _mm_setzero_ps();
        for (int i = 0; i < n; i += 8)
        {
            s0 = _mm_add_ps (s0, _mm_mul_ps (_mm_loadu_ps (a + i),     _mm_loadu_ps (b + i)));
            s1 = _mm_add_ps (s1, _mm_mul_ps (_mm_loadu_ps (a + i + 4), _mm_loadu_ps (b + i + 4)));
        }
        s0 = _mm_add_ps (s0, s1);
        alignas (16) float t[4];
        _mm_store_ps (t, s0);
        return (t[0] + t[1]) + (t[2] + t[3]);
       #else
        float s[8] = {};
        for (int i = 0; i < n; i += 8)
            for (int k = 0; k < 8; ++k)
                s[k] += a[i + k] * b[i + k];
        return ((s[0] + s[1]) + (s[2] + s[3])) + ((s[4] + s[5]) + (s[6] + s[7]));
       #endif
    }
}

/*
    Zero-latency time-varying filter for one channel.

    design() turns a per-bin magnitude response into a minimum-phase FIR using the
    real cepstrum (homomorphic) method:
        c   = IDFT(log|H|)            real cepstrum
        c'  = fold c onto n >= 0      (c[0], 2c[1..N/2-1], c[N/2], 0...)
        H_m = exp(DFT(c'))            minimum-phase spectrum, same magnitude
        h   = IDFT(H_m)               truncated + tapered to the FIR length
    A minimum-phase filter has no pre-ringing and the least possible delay, so the
    audio path adds no latency. Each new filter is crossfaded in over one hop.
*/
class MinPhaseFilter
{
public:
    void prepare (int firLength, int analysisBins, int crossfadeSamples)
    {
        jassert (juce::isPowerOfTwo (firLength) && firLength >= 64);
        firLen = firLength;
        numGains = analysisBins;
        fadeLen = juce::jmax (1, crossfadeSamples);

        // Cepstrum size: twice the FIR length keeps time-aliasing low
        cepSize = firLen * 2;
        fft = std::make_unique<juce::dsp::FFT> (juce::roundToInt (std::log2 ((double) cepSize)));
        bufA.assign ((size_t) cepSize, {});
        bufB.assign ((size_t) cepSize, {});
        calibrate();

        history.assign ((size_t) firLen * 2, 0.0f);
        firCur.assign ((size_t) firLen, 0.0f);
        firPrev.assign ((size_t) firLen, 0.0f);
        firNew.assign ((size_t) firLen, 0.0f);
        reset();
    }

    void reset()
    {
        std::fill (history.begin(), history.end(), 0.0f);
        // Identity: reversed FIR, so the unit tap sits at the end
        std::fill (firCur.begin(), firCur.end(), 0.0f);
        firCur.back() = 1.0f;
        firPrev = firCur;
        histPos = 0;
        fade = 1.0f;
    }

    // gains: linear magnitude for analysis bins 0..numGains-1 (DC..Nyquist)
    void design (const float* gains)
    {
        const int half = cepSize / 2;
        const float binScale = (float) (numGains - 1) / (float) half;

        // log|H| on the cepstrum grid (interpolated from the analysis bins)
        for (int j = 0; j <= half; ++j)
        {
            const float pos = (float) j * binScale;
            const int i0 = juce::jmin ((int) pos, numGains - 2);
            const float fr = pos - (float) i0;
            const float g = gains[i0] + fr * (gains[i0 + 1] - gains[i0]);
            const float lg = std::log (juce::jmax (g, 1.0e-4f));
            bufA[(size_t) j] = { lg, 0.0f };
            if (j > 0 && j < half)
                bufA[(size_t) (cepSize - j)] = { lg, 0.0f };
        }

        inverse (bufA.data(), bufB.data());      // real cepstrum in bufB

        bufA[0] = { bufB[0].real(), 0.0f };
        for (int n = 1; n < half; ++n)
            bufA[(size_t) n] = { 2.0f * bufB[(size_t) n].real(), 0.0f };
        bufA[(size_t) half] = { bufB[(size_t) half].real(), 0.0f };
        for (int n = half + 1; n < cepSize; ++n)
            bufA[(size_t) n] = {};

        forward (bufA.data(), bufB.data());      // log of min-phase spectrum

        for (int j = 0; j < cepSize; ++j)
        {
            const auto z = bufB[(size_t) j];
            bufA[(size_t) j] = std::polar (std::exp (z.real()), z.imag());
        }

        inverse (bufA.data(), bufB.data());      // impulse response

        // Truncate with a half-cosine taper over the last quarter, store reversed
        const int taperStart = firLen - firLen / 4;
        for (int n = 0; n < firLen; ++n)
        {
            float w = 1.0f;
            if (n >= taperStart)
                w = 0.5f + 0.5f * std::cos (juce::MathConstants<float>::pi * (float) (n - taperStart) / (float) (firLen - taperStart));
            firNew[(size_t) (firLen - 1 - n)] = bufB[(size_t) n].real() * w;
        }

        // Start a crossfade from whatever is currently audible
        std::swap (firPrev, firCur);
        std::swap (firCur, firNew);
        fade = 0.0f;
    }

    float processSample (float x) noexcept
    {
        history[(size_t) histPos] = x;
        history[(size_t) (histPos + firLen)] = x;
        histPos = (histPos + 1) & (firLen - 1);

        // Oldest..newest of the last firLen samples, contiguous thanks to the doubled buffer
        const float* win = history.data() + histPos;
        const float yCur = quell::dot (win, firCur.data(), firLen);

        if (fade >= 1.0f)
            return yCur;

        const float yPrev = quell::dot (win, firPrev.data(), firLen);
        const float y = yPrev + fade * (yCur - yPrev);
        fade = juce::jmin (1.0f, fade + 1.0f / (float) fadeLen);
        return y;
    }

    // Impulse response in normal order (for tests)
    std::vector<float> getImpulseResponse() const
    {
        return { firCur.rbegin(), firCur.rend() };
    }

private:
    using Complex = std::complex<float>;

    // JUCE's FFT backends (fallback, vDSP, IPP, FFTW) differ in sign and scaling
    // conventions, so measure them once and normalise to:
    //   forward: X[k] = sum x[n] e^{-i 2pi kn/N},  inverse: x[n] = 1/N sum X[k] e^{+i 2pi kn/N}
    void calibrate()
    {
        std::fill (bufA.begin(), bufA.end(), Complex {});
        bufA[1] = { 1.0f, 0.0f };
        fft->perform (bufA.data(), bufB.data(), false);
        flipSign = bufB[1].imag() > 0.0f;   // JUCE "forward" is e^{+i}: swap directions

        std::fill (bufA.begin(), bufA.end(), Complex {});
        bufA[0] = { 1.0f, 0.0f };
        fft->perform (bufA.data(), bufB.data(), ! flipSign);   // true inverse
        inverseScale = std::abs (bufB[0].real() - 1.0f) < 1.0e-3f ? 1.0f / (float) cepSize : 1.0f;
    }

    void forward (const Complex* in, Complex* out) { fft->perform (in, out, flipSign); }

    void inverse (const Complex* in, Complex* out)
    {
        fft->perform (in, out, ! flipSign);
        if (inverseScale != 1.0f)
            for (int i = 0; i < cepSize; ++i)
                out[i] *= inverseScale;
    }

    int firLen = 1024, numGains = 513, fadeLen = 256, cepSize = 2048;
    int histPos = 0;
    float fade = 1.0f, inverseScale = 1.0f;
    bool flipSign = false;

    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<Complex> bufA, bufB;
    std::vector<float> history, firCur, firPrev, firNew;
};
