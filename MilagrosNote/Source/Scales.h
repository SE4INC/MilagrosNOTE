// Notas y escalas de MilagrosAfina (AGPL-3.0)
#pragma once

#include <juce_core/juce_core.h>
#include <cmath>

namespace afina
{
    inline constexpr int numScales = 7;

    inline const char* noteNames (int pc)
    {
        static const char* const n[12] = { "Do", "Do#", "Re", "Re#", "Mi", "Fa",
                                           "Fa#", "Sol", "Sol#", "La", "La#", "Si" };
        return n[((pc % 12) + 12) % 12];
    }

    inline juce::String noteWithOctave (int midi)
    {
        return juce::String (noteNames (midi)) + juce::String (midi / 12 - 1);
    }

    inline juce::StringArray scaleNames()
    {
        return { juce::String::fromUTF8 ("Crom\xC3\xA1tica (todas las notas)"),
                 "Mayor",
                 "Menor",
                 juce::String::fromUTF8 ("Menor arm\xC3\xB3nica"),
                 juce::String::fromUTF8 ("Pentat\xC3\xB3nica mayor"),
                 juce::String::fromUTF8 ("Pentat\xC3\xB3nica menor"),
                 "Blues" };
    }

    // Cada escala es una máscara de 12 bits (bit 0 = la tónica)
    inline bool inScale (int scaleType, int root, int pc)
    {
        static const int masks[numScales] = { 4095,  // cromática
                                              2741,  // mayor        0 2 4 5 7 9 11
                                              1453,  // menor        0 2 3 5 7 8 10
                                              2477,  // menor armónica 0 2 3 5 7 8 11
                                              661,   // pentatónica mayor 0 2 4 7 9
                                              1193,  // pentatónica menor 0 3 5 7 10
                                              1257 };// blues        0 3 5 6 7 10
        const int rel = (((pc - root) % 12) + 12) % 12;
        return ((masks[juce::jlimit (0, numScales - 1, scaleType)] >> rel) & 1) != 0;
    }

    // Nota MIDI permitida más cercana a un valor de nota con decimales
    inline int nearestAllowed (float midi, int scaleType, int root)
    {
        const int base = (int) std::floor (midi);
        int best = base;
        float bestD = 1.0e9f;
        for (int n = base - 3; n <= base + 4; ++n)
        {
            if (inScale (scaleType, root, ((n % 12) + 12) % 12))
            {
                const float d = std::abs (midi - (float) n);
                if (d < bestD)
                {
                    bestD = d;
                    best = n;
                }
            }
        }
        return best;
    }
}
