// Detección de acordes del beat (mayores y menores) y línea de tiempo compartida
// entre las copias del plugin (así una copia en el beat puede avisar a la copia de la voz).
#pragma once

#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <cmath>

namespace notechords
{
    // id: -2 = sin datos, -1 = sin acorde claro, 0..11 = mayor, 12..23 = menor
    inline juce::String chordName (int id)
    {
        if (id < 0)
            return "-";
        static const char* const n[12] = { "Do", "Do#", "Re", "Re#", "Mi", "Fa", "Fa#", "Sol", "Sol#", "La", "La#", "Si" };
        return juce::String (n[id % 12]) + (id >= 12 ? "m" : "");
    }

    inline bool isChordTone (int chordId, int pc)
    {
        if (chordId < 0)
            return false;
        const int root = chordId % 12;
        const bool minor = chordId >= 12;
        const int rel = (((pc - root) % 12) + 12) % 12;
        return rel == 0 || rel == 7 || rel == (minor ? 3 : 4);
    }
}

struct SharedChords
{
    static constexpr int maxFrames = 16384;       // ~ 23 minutos a 48 kHz
    static constexpr int hopSamples = 4096;

    std::atomic<int> chord[maxFrames];
    std::atomic<int> maxFrame { -1 };
    std::atomic<float> chromaSum[12];

    SharedChords() { clear(); }

    void clear()
    {
        for (auto& c : chord)
            c.store (-2);
        maxFrame.store (-1);
        for (auto& s : chromaSum)
            s.store (0.0f);
    }

    static SharedChords& get()
    {
        static SharedChords instance;
        return instance;
    }
};

class ChordAnalyzer
{
public:
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
        chromaSm.fill (0.0f);
        idx = count = 0;
        lastChord = -1;
    }

    // Devuelve true cuando hay un acorde nuevo (cada 4096 muestras)
    bool pushSample (float x)
    {
        fifo[(size_t) idx] = x;
        idx = (idx + 1) & (fftSize - 1);
        if (++count >= SharedChords::hopSamples)
        {
            count = 0;
            analyse();
            return true;
        }
        return false;
    }

    int chord() const { return lastChord; }
    const std::array<float, 12>& chroma() const { return chromaSm; }

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
        {
            lastChord = -1;
            chromaSm.fill (0.0f);
            return;
        }
        std::fill (data.begin() + fftSize, data.end(), 0.0f);
        fft.performFrequencyOnlyForwardTransform (data.data());

        const int kLo = (int) std::ceil (100.0 * fftSize / sr);
        const int kHi = juce::jmin ((int) (2000.0 * fftSize / sr), fftSize / 2 - 1);

        std::array<float, 12> cur {};
        for (int k = kLo; k <= kHi; ++k)
        {
            const float f = (float) ((double) k * sr / (double) fftSize);
            const float midiF = 69.0f + 12.0f * std::log2 (f / 440.0f);
            const int n = (int) std::lround (midiF);
            if (std::abs (midiF - (float) n) < 0.35f)
                cur[(size_t) (((n % 12) + 12) % 12)] += data[(size_t) k];
        }

        float mx = 1.0e-9f;
        for (float v : cur)
            mx = juce::jmax (mx, v);
        for (size_t i = 0; i < 12; ++i)
            chromaSm[i] = 0.5f * chromaSm[i] + 0.5f * (cur[i] / mx);

        float norm = 0.0f;
        for (float v : chromaSm)
            norm += v * v;
        norm = std::sqrt (norm);
        if (norm < 1.0e-6f)
        {
            lastChord = -1;
            return;
        }

        int best = -1;
        float bestScore = 0.0f;
        for (int root = 0; root < 12; ++root)
        {
            for (int q = 0; q < 2; ++q)
            {
                const size_t third = (size_t) ((root + (q == 1 ? 3 : 4)) % 12);
                const size_t fifth = (size_t) ((root + 7) % 12);
                const float score = (1.2f * chromaSm[(size_t) root] + 1.0f * chromaSm[third] + 0.9f * chromaSm[fifth])
                                    / (norm * 1.803f);
                if (score > bestScore)
                {
                    bestScore = score;
                    best = root + 12 * q;
                }
            }
        }
        lastChord = bestScore >= 0.62f ? best : -1;
    }

    static constexpr int fftOrder = 13;
    static constexpr int fftSize = 1 << fftOrder;

    double sr = 44100.0;
    juce::dsp::FFT fft { fftOrder };
    std::array<float, fftSize> window {}, fifo {};
    std::array<float, fftSize * 2> data {};
    std::array<float, 12> chromaSm {};
    int idx = 0, count = 0, lastChord = -1;
};
