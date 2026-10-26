// Detección de tonalidad: perfil de notas (chroma) + perfiles de Krumhansl-Schmuckler
#pragma once

#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <cmath>

class KeyAnalyzer
{
public:
    struct Key
    {
        int root = -1;        // 0 = Do ... 11 = Si
        bool minor = false;
        float score = 0.0f;   // correlación (-1..1)
    };

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        for (int i = 0; i < fftSize; ++i)
            window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) fftSize);
        reset();
    }

    void reset()
    {
        fifo.fill (0.0f);
        idx = 0;
        count = 0;
        for (auto& c : chroma)
            c.store (0.0f);
        frames.store (0);
    }

    // Una muestra mono de la instrumental
    void pushSample (float x)
    {
        fifo[(size_t) idx] = x;
        idx = (idx + 1) & (fftSize - 1);
        if (++count >= hop)
        {
            count = 0;
            analyse();
        }
    }

    int getFrames() const { return frames.load(); }

    std::array<float, 12> getChroma() const
    {
        std::array<float, 12> r {};
        for (size_t i = 0; i < 12; ++i)
            r[i] = chroma[i].load();
        return r;
    }

    static Key estimate (const std::array<float, 12>& c)
    {
        static const float major[12] = { 6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f, 2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f };
        static const float minorP[12] = { 6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f, 2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f };

        float mean = 0.0f;
        for (float v : c)
            mean += v;
        mean /= 12.0f;

        float norm = 0.0f;
        for (float v : c)
            norm += (v - mean) * (v - mean);

        Key best;
        if (norm < 1.0e-9f)
            return best;

        best.score = -2.0f;
        for (int mode = 0; mode < 2; ++mode)
        {
            const float* prof = mode == 0 ? major : minorP;
            float pm = 0.0f;
            for (int i = 0; i < 12; ++i)
                pm += prof[i];
            pm /= 12.0f;
            float pn = 0.0f;
            for (int i = 0; i < 12; ++i)
                pn += (prof[i] - pm) * (prof[i] - pm);

            for (int r = 0; r < 12; ++r)
            {
                float num = 0.0f;
                for (int i = 0; i < 12; ++i)
                    num += (c[(size_t) ((i + r) % 12)] - mean) * (prof[i] - pm);
                const float s = num / std::sqrt (norm * pn);
                if (s > best.score)
                {
                    best.score = s;
                    best.root = r;
                    best.minor = (mode == 1);
                }
            }
        }
        return best;
    }

private:
    void analyse()
    {
        float energy = 0.0f;
        for (int i = 0; i < fftSize; ++i)
        {
            const float s = fifo[(size_t) ((idx + i) & (fftSize - 1))];
            energy += s * s;
            data[(size_t) i] = s * window[(size_t) i];
        }
        if (std::sqrt (energy / (float) fftSize) < 0.001f)
            return;   // casi silencio
        std::fill (data.begin() + fftSize, data.end(), 0.0f);

        fft.performFrequencyOnlyForwardTransform (data.data());

        const int kLo = (int) std::ceil (100.0 * fftSize / sr);
        const int kHi = juce::jmin ((int) (2000.0 * fftSize / sr), fftSize / 2 - 1);

        float maxMag = 1.0e-9f;
        for (int k = kLo; k <= kHi; ++k)
            maxMag = juce::jmax (maxMag, data[(size_t) k]);

        std::array<float, 12> add {};
        for (int k = kLo; k <= kHi; ++k)
        {
            const float f = (float) ((double) k * sr / (double) fftSize);
            const float midiF = 69.0f + 12.0f * std::log2 (f / 440.0f);
            const int n = (int) std::lround (midiF);
            if (std::abs (midiF - (float) n) < 0.35f)
                add[(size_t) (((n % 12) + 12) % 12)] += data[(size_t) k] / maxMag;
        }

        for (size_t i = 0; i < 12; ++i)
            chroma[i].store (chroma[i].load() * 0.995f + add[i]);
        frames.fetch_add (1);
    }

    static constexpr int fftOrder = 13;
    static constexpr int fftSize = 1 << fftOrder;
    static constexpr int hop = fftSize / 2;

    double sr = 44100.0;
    juce::dsp::FFT fft { fftOrder };
    std::array<float, fftSize> window {}, fifo {};
    std::array<float, fftSize * 2> data {};
    int idx = 0, count = 0;
    std::atomic<float> chroma[12];
    std::atomic<int> frames { 0 };
};
