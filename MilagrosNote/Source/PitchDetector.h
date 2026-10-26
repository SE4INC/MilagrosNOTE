// Detector de nota (algoritmo YIN + refinamiento a la frecuencia de muestreo original)
#pragma once

#include <juce_dsp/juce_dsp.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

class PitchDetector
{
public:
    struct Result
    {
        bool voiced = false;
        float f0 = 0.0f;            // Hz
        float confidence = 0.0f;    // 0..1
        float rmsDb = -120.0f;
    };

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        decim = juce::jmax (1, (int) std::lround (sr / 12000.0));
        sd = sr / (double) decim;
        tauMin = juce::jmax (2, (int) std::floor (sd / 1000.0));   // hasta 1000 Hz
        tauMax = (int) std::ceil (sd / 65.0);                      // desde 65 Hz
        frameLen = windowD + tauMax + 4;

        hp.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighPass (sr, 70.0, 0.707f);
        const auto lpc = juce::dsp::IIR::Coefficients<float>::makeLowPass (sr, juce::jmin (3500.0, 0.4 * sd), 0.707f);
        lp1.coefficients = lpc;
        lp2.coefficients = lpc;
        hp.reset();
        lp1.reset();
        lp2.reset();

        fbuf.assign (fSize, 0.0f);
        dbuf.assign (dSize, 0.0f);
        frame.assign ((size_t) frameLen, 0.0f);
        cmnd.assign ((size_t) tauMax + 3, 1.0f);
        fIdx = dIdx = 0;
        decCount = hopCount = 0;
        result = {};
    }

    void setLevelThresholdDb (float db) { levelThresholdDb = db; }
    double hopSeconds() const { return (double) hopD * (double) decim / sr; }
    int hopSamples() const { return hopD * decim; }
    const Result& getResult() const { return result; }

    // Devuelve true cuando hay un resultado nuevo (cada ~4 ms)
    bool pushSample (float x)
    {
        x = hp.processSample (x);
        fbuf[fIdx & fMask] = x;
        ++fIdx;

        const float y = lp2.processSample (lp1.processSample (x));
        if (++decCount < decim)
            return false;
        decCount = 0;

        dbuf[dIdx & dMask] = y;
        ++dIdx;
        if (++hopCount < hopD)
            return false;
        hopCount = 0;

        analyse();
        return true;
    }

private:
    void analyse()
    {
        result.voiced = false;
        result.confidence = 0.0f;
        result.f0 = 0.0f;

        // Nivel de la señal
        {
            const int n = 1024;
            float e = 0.0f;
            for (int i = 0; i < n; ++i)
            {
                const float s = fbuf[(fIdx - 1u - (uint32_t) i) & fMask];
                e += s * s;
            }
            result.rmsDb = 10.0f * std::log10 (e / (float) n + 1.0e-12f);
        }

        if (dIdx < (uint32_t) frameLen || result.rmsDb < levelThresholdDb)
            return;

        for (int i = 0; i < frameLen; ++i)
            frame[(size_t) i] = dbuf[(dIdx - (uint32_t) frameLen + (uint32_t) i) & dMask];

        // Función de diferencia normalizada (YIN)
        const int W = windowD;
        const int base = tauMax + 4;
        float runSum = 0.0f;
        cmnd[0] = 1.0f;
        for (int tau = 1; tau <= tauMax + 1; ++tau)
        {
            float sum = 0.0f;
            for (int j = 0; j < W; ++j)
            {
                const float diff = frame[(size_t) (base + j)] - frame[(size_t) (base + j - tau)];
                sum += diff * diff;
            }
            runSum += sum;
            cmnd[(size_t) tau] = runSum > 1.0e-9f ? sum * (float) tau / runSum : 1.0f;
        }
        if (runSum < 1.0e-9f)
            return;

        int tauEst = -1;
        for (int tau = tauMin; tau <= tauMax; ++tau)
        {
            if (cmnd[(size_t) tau] < 0.15f)
            {
                while (tau + 1 <= tauMax && cmnd[(size_t) tau + 1] < cmnd[(size_t) tau])
                    ++tau;
                tauEst = tau;
                break;
            }
        }
        if (tauEst < 0)
        {
            float m = 1.0e9f;
            for (int tau = tauMin; tau <= tauMax; ++tau)
            {
                if (cmnd[(size_t) tau] < m)
                {
                    m = cmnd[(size_t) tau];
                    tauEst = tau;
                }
            }
        }
        if (tauEst < tauMin)
            return;

        const float c = cmnd[(size_t) tauEst];
        result.confidence = juce::jlimit (0.0f, 1.0f, 1.0f - c);
        if (c > 0.3f)
            return;   // sin nota clara (ruido, consonante, silencio)

        // Interpolación parabólica a la frecuencia reducida
        float tauF = (float) tauEst;
        {
            const float a = cmnd[(size_t) tauEst - 1], b = cmnd[(size_t) tauEst], cc = cmnd[(size_t) tauEst + 1];
            const float den = a - 2.0f * b + cc;
            if (std::abs (den) > 1.0e-9f)
                tauF += juce::jlimit (-1.0f, 1.0f, 0.5f * (a - cc) / den);
        }

        // Refinamiento a la frecuencia de muestreo original
        float tauFull = tauF * (float) decim;
        const int center = (int) std::lround (tauFull);
        const int Wf = juce::jlimit (512, 2048, 2 * center);
        const int range = decim + 2;

        if (fIdx > (uint32_t) (Wf + center + range + 4) && 2 * range + 1 <= (int) dv.size())
        {
            float best = 1.0e30f;
            int bestIdx = range;
            for (int t = center - range; t <= center + range; ++t)
            {
                float sum = 0.0f;
                for (int j = 0; j < Wf; ++j)
                {
                    const float a = fbuf[(fIdx - 1u - (uint32_t) j) & fMask];
                    const float b = fbuf[(fIdx - 1u - (uint32_t) j - (uint32_t) t) & fMask];
                    const float d = a - b;
                    sum += d * d;
                }
                dv[(size_t) (t - (center - range))] = sum;
                if (sum < best)
                {
                    best = sum;
                    bestIdx = t - (center - range);
                }
            }

            float refined = (float) (center - range + bestIdx);
            if (bestIdx > 0 && bestIdx < 2 * range)
            {
                const float a = dv[(size_t) bestIdx - 1], b = dv[(size_t) bestIdx], cc = dv[(size_t) bestIdx + 1];
                const float den = a - 2.0f * b + cc;
                if (std::abs (den) > 1.0e-12f)
                    refined += juce::jlimit (-1.0f, 1.0f, 0.5f * (a - cc) / den);
            }
            // Solo aceptamos el refinamiento si no se aleja del valor grueso
            if (std::abs (refined - tauFull) < (float) decim * 1.5f)
                tauFull = refined;
        }

        result.f0 = (float) sr / tauFull;
        result.voiced = true;
    }

    static constexpr uint32_t fSize = 8192, fMask = fSize - 1;
    static constexpr uint32_t dSize = 1024, dMask = dSize - 1;
    static constexpr int windowD = 256;
    static constexpr int hopD = 48;

    double sr = 44100.0, sd = 11025.0;
    int decim = 4, tauMin = 11, tauMax = 170, frameLen = 430;
    int decCount = 0, hopCount = 0;
    uint32_t fIdx = 0, dIdx = 0;
    float levelThresholdDb = -50.0f;

    juce::dsp::IIR::Filter<float> hp, lp1, lp2;
    std::vector<float> fbuf, dbuf, frame, cmnd;
    std::array<float, 64> dv {};
    Result result;
};
