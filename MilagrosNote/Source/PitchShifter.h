// Cambiador de tono sincronizado con el periodo de la voz.
// Dos "cabezas" de lectura sobre una línea de retardo; cuando una se acerca al borde,
// salta exactamente un periodo de la voz (alineado por correlación) con un fundido corto.
// Así la forma de onda se mantiene continua y la voz conserva su timbre.
#pragma once

#include <juce_core/juce_core.h>
#include <cmath>
#include <cstdint>
#include <vector>

class PitchShifter
{
public:
    void prepare (double sampleRate)
    {
        sr = sampleRate;
        xfMax = juce::jmax (48, (int) std::lround (0.005 * sr));
        dLow = (float) (xfMax + 8);
        tMax = (float) std::ceil (sr / 65.0);
        dHigh = dLow + tMax;
        dHome = dLow + 0.5f * tMax;
        buf.assign (size, 0.0f);
        reset();
    }

    void reset()
    {
        std::fill (buf.begin(), buf.end(), 0.0f);
        w = 0;
        dA = dB = dHome;
        active = 0;
        xfading = false;
        xfPos = xfLen = 0;
        lastPeriod = (float) (sr / 150.0);
    }

    int latencySamples() const { return (int) std::lround (dHome); }

    // ratio: 1.0 = sin cambio, 1.0595 = un semitono arriba
    // period: periodo de la voz de entrada en muestras
    float process (float x, float ratio, float period, bool voiced)
    {
        buf[w & mask] = x;
        if (period > 20.0f && period < tMax * 1.5f)
            lastPeriod = period;

        float& dAct = (active == 0) ? dA : dB;
        float& dOth = (active == 0) ? dB : dA;

        float drift = ratio - 1.0f;
        // Sin voz: volvemos poco a poco al retardo central (latencia estable)
        if (! voiced && std::abs (drift) < 1.0e-4f)
            drift = juce::jlimit (-0.01f, 0.01f, (dAct - dHome) * 0.001f);

        dA -= drift;
        dB -= drift;

        if (! xfading)
        {
            const float margin = std::abs (drift) * (float) xfMax + 2.0f;
            if (drift > 0.0f && dAct < dLow + margin)
                startJump (+lastPeriod);
            else if (drift < 0.0f && dAct > dHigh - margin)
                startJump (-lastPeriod);
        }

        dA = juce::jlimit (6.0f, (float) (size - 16), dA);
        dB = juce::jlimit (6.0f, (float) (size - 16), dB);

        float out = readInterp (dAct);
        if (xfading)
        {
            const float yOth = readInterp (dOth);
            const float g = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * (float) xfPos / (float) xfLen);
            out += g * (yOth - out);
            if (++xfPos >= xfLen)
            {
                active ^= 1;
                xfading = false;
            }
        }

        ++w;
        return out;
    }

private:
    void startJump (float signedPeriod)
    {
        float& dAct = (active == 0) ? dA : dB;
        float& dOth = (active == 0) ? dB : dA;

        const float T = std::abs (signedPeriod);
        float cand = dAct + signedPeriod;
        int guard = 0;
        while (cand < dLow && guard++ < 8)  cand += T;
        while (cand > dHigh && guard++ < 16) cand -= T;

        xfLen = juce::jlimit (32, xfMax, (int) T);
        const int S = juce::jlimit (2, 24, (int) (T * 0.06f) + 2);
        const int c0 = (int) std::lround (cand);
        const int a0 = (int) std::lround (dAct);

        // Buscamos el desplazamiento que mejor alinea las formas de onda
        int bestDelta = 0;
        double bestScore = -1.0e30;
        for (int dl = -S; dl <= S; ++dl)
        {
            const int cd = c0 + dl;
            if (cd < xfLen + 4 || cd > (int) size - 64)
                continue;
            double num = 0.0, den = 1.0e-9;
            for (int i = 0; i < xfLen; ++i)
            {
                const float r = buf[(w - (uint32_t) a0 + (uint32_t) i) & mask];
                const float c = buf[(w - (uint32_t) cd + (uint32_t) i) & mask];
                num += (double) r * (double) c;
                den += (double) c * (double) c;
            }
            const double score = num / std::sqrt (den);
            if (score > bestScore)
            {
                bestScore = score;
                bestDelta = dl;
            }
        }

        dOth = (float) (c0 + bestDelta);
        xfPos = 0;
        xfading = true;
    }

    float readInterp (float delay) const
    {
        const int di = (int) delay;
        const float frac = delay - (float) di;
        const uint32_t idx = w - (uint32_t) di;

        const float ym1 = buf[(idx - 2u) & mask];
        const float y0  = buf[(idx - 1u) & mask];
        const float y1  = buf[idx & mask];
        const float y2  = buf[(idx + 1u) & mask];

        const float t = 1.0f - frac;
        const float c0 = y0;
        const float c1 = 0.5f * (y1 - ym1);
        const float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
        const float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
        return ((c3 * t + c2) * t + c1) * t + c0;
    }

    static constexpr uint32_t size = 16384, mask = size - 1;

    double sr = 44100.0;
    std::vector<float> buf;
    uint32_t w = 0;
    float dA = 600.0f, dB = 600.0f;
    float dLow = 248.0f, dHigh = 990.0f, dHome = 620.0f, tMax = 680.0f;
    int active = 0, xfMax = 220, xfPos = 0, xfLen = 64;
    bool xfading = false;
    float lastPeriod = 300.0f;
};
