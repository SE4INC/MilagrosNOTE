#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    float medianOf (std::vector<float> v)
    {
        if (v.empty())
            return 0.0f;
        std::sort (v.begin(), v.end());
        return v[v.size() / 2];
    }
}

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout MilagrosNoteProcessor::createLayout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> params;

    StringArray keys;
    for (int i = 0; i < 12; ++i)
        keys.add (afina::noteNames (i));

    auto pctAttr = AudioParameterFloatAttributes()
        .withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v * 100.0f)) + " %"; })
        .withValueFromStringFunction ([] (const String& t) { return t.getFloatValue() / 100.0f; });
    auto msAttr = AudioParameterFloatAttributes()
        .withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v)) + " ms"; })
        .withValueFromStringFunction ([] (const String& t) { return t.getFloatValue(); });
    auto hzAttr = AudioParameterFloatAttributes()
        .withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " Hz"; })
        .withValueFromStringFunction ([] (const String& t) { return t.getFloatValue(); });
    auto dbAttr = AudioParameterFloatAttributes()
        .withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v)) + " dB"; })
        .withValueFromStringFunction ([] (const String& t) { return t.getFloatValue(); });

    params.push_back (std::make_unique<AudioParameterChoice> (
        ParameterID { "mode", 1 }, es ("Modo"),
        StringArray { es ("Voz (editar notas)"), es ("Instrumental (analizar acordes)") }, 0));
    params.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "key", 1 }, es ("Tonalidad"), keys, 0));
    params.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "scale", 1 }, es ("Escala"), afina::scaleNames(), 1));
    params.push_back (std::make_unique<AudioParameterChoice> (
        ParameterID { "guide", 1 }, es ("Guía"),
        StringArray { es ("Acordes del beat + escala"), es ("Solo la escala") }, 0));
    params.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "stabilize", 1 }, es ("Estabilizar"),
                                                             NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.35f, pctAttr));
    params.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "transition", 1 }, es ("Transición"),
                                                             NormalisableRange<float> (1.0f, 100.0f, 1.0f), 25.0f, msAttr));
    params.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "refA", 1 }, es ("Afinación base (La)"),
                                                             NormalisableRange<float> (430.0f, 450.0f, 0.1f), 440.0f, hzAttr));
    params.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "thresh", 1 }, es ("Umbral de voz"),
                                                             NormalisableRange<float> (-70.0f, -30.0f, 1.0f), -50.0f, dbAttr));
    params.push_back (std::make_unique<AudioParameterBool> (ParameterID { "live", 1 }, es ("Escucharme afinado al cantar"), true));

    return { params.begin(), params.end() };
}

//==============================================================================
MilagrosNoteProcessor::MilagrosNoteProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",     juce::AudioChannelSet::stereo(), true)
                          .withInput  ("Sidechain", juce::AudioChannelSet::stereo(), false)
                          .withOutput ("Output",    juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createLayout())
{
    modeP       = apvts.getRawParameterValue ("mode");
    keyP        = apvts.getRawParameterValue ("key");
    scaleP      = apvts.getRawParameterValue ("scale");
    guideP      = apvts.getRawParameterValue ("guide");
    stabilizeP  = apvts.getRawParameterValue ("stabilize");
    transitionP = apvts.getRawParameterValue ("transition");
    refAP       = apvts.getRawParameterValue ("refA");
    threshP     = apvts.getRawParameterValue ("thresh");
    liveP       = apvts.getRawParameterValue ("live");

    f0Frames.reset (new std::atomic<float>[(size_t) maxFrames]);
    shiftFrames.reset (new std::atomic<float>[(size_t) maxFrames]);
    for (int i = 0; i < maxFrames; ++i)
    {
        f0Frames[(size_t) i].store (0.0f);
        shiftFrames[(size_t) i].store (0.0f);
    }
}

bool MilagrosNoteProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    if (layouts.getMainInputChannelSet() != out)
        return false;

    const auto& sc = layouts.getChannelSet (true, 1);
    return sc.isDisabled() || sc == juce::AudioChannelSet::mono() || sc == juce::AudioChannelSet::stereo();
}

void MilagrosNoteProcessor::prepareToPlay (double sampleRate, int)
{
    sr = sampleRate;
    detector.prepare (sr);
    shifter[0].prepare (sr);
    shifter[1].prepare (sr);
    chordAnalyzer.prepare (sr);

    latency = shifter[0].latencySamples();
    setLatencySamples (latency);
    frameSize = detector.hopSamples();
    hopSeconds = detector.hopSeconds();
    detectDelay = (int) std::lround (0.016 * sr);

    ratioSm = 1.0f;
    periodSm = (float) (sr / 150.0);
    cachedPeriod = periodSm;
    lastFi = -1;
    f0Count = f0Pos = jumpCount = 0;
    liveUnvoiced = 100;
    liveTargetValid = false;
    liveVoiced = false;
    liveCorr = 0.0f;
    liveRatio = 1.0f;
    fallbackPos = 0;
}

//==============================================================================
float MilagrosNoteProcessor::medianF0() const
{
    std::array<float, 5> tmp {};
    const int n = juce::jlimit (1, 5, f0Count);
    for (int i = 0; i < n; ++i)
        tmp[(size_t) i] = f0Ring[(size_t) i];
    std::sort (tmp.begin(), tmp.begin() + n);
    return tmp[(size_t) (n / 2)];
}

void MilagrosNoteProcessor::onDetect (juce::int64 s, bool write)
{
    const auto& r = detector.getResult();

    // 1) Guardar la nota detectada en la línea de tiempo (captura)
    if (write)
    {
        const juce::int64 center = s - detectDelay;
        if (center >= 0)
        {
            const int f = (int) (center / frameSize);
            if (f < maxFrames)
            {
                f0Frames[(size_t) f].store (r.voiced ? r.f0 : 0.0f);
                shiftFrames[(size_t) f].store (0.0f);
                if (f + 1 > capturedFrames.load())
                    capturedFrames.store (f + 1);
            }
        }
    }

    // 2) Corrección en vivo con la escala (para escucharte afinado)
    const int root = juce::jlimit (0, 11, (int) keyP->load());
    const int scale = juce::jlimit (0, afina::numScales - 1, (int) scaleP->load());
    const float refA = refAP->load();

    if (r.voiced)
    {
        if (f0Count >= 3)
        {
            const float pm = medianF0();
            if (r.f0 > pm * 1.3f || r.f0 < pm / 1.3f)
            {
                if (++jumpCount < 3)
                    return;
                f0Count = f0Pos = 0;
                jumpCount = 0;
            }
            else
            {
                jumpCount = 0;
            }
        }
        f0Ring[(size_t) f0Pos] = r.f0;
        f0Pos = (f0Pos + 1) % 5;
        f0Count = juce::jmin (f0Count + 1, 5);
        const float f0 = medianF0();

        liveUnvoiced = 0;
        const float midi = 69.0f + 12.0f * std::log2 (f0 / refA);
        const int nearest = afina::nearestAllowed (midi, scale, root);
        if (! liveTargetValid || ! afina::inScale (scale, root, ((liveTarget % 12) + 12) % 12))
        {
            liveTarget = nearest;
            liveTargetValid = true;
        }
        else if (nearest != liveTarget && std::abs (midi - (float) nearest) + 0.2f < std::abs (midi - (float) liveTarget))
        {
            liveTarget = nearest;
        }

        const float err = ((float) liveTarget - midi) * 100.0f;
        const float alpha = 1.0f - std::exp (-(float) hopSeconds / 0.04f);
        liveCorr += alpha * (err * 0.9f - liveCorr);
        liveRatio = std::pow (2.0f, liveCorr / 1200.0f);
        livePeriod = (float) sr / f0;
        liveVoiced = true;
    }
    else
    {
        ++liveUnvoiced;
        if (liveUnvoiced > 6)
        {
            liveCorr *= 0.9f;
            liveRatio = std::pow (2.0f, liveCorr / 1200.0f);
        }
        if (liveUnvoiced > 40)
        {
            liveTargetValid = false;
            f0Count = f0Pos = 0;
        }
        liveVoiced = liveUnvoiced < 12;
    }
}

void MilagrosNoteProcessor::onChord (juce::int64 s)
{
    const juce::int64 center = s - SharedChords::hopSamples;
    if (center < 0)
        return;
    const int cf = (int) (center / SharedChords::hopSamples);
    if (cf >= SharedChords::maxFrames)
        return;

    auto& sh = SharedChords::get();
    sh.chord[cf].store (chordAnalyzer.chord());
    if (cf > sh.maxFrame.load())
        sh.maxFrame.store (cf);

    const auto& c = chordAnalyzer.chroma();
    for (size_t k = 0; k < 12; ++k)
        sh.chromaSum[k].store (sh.chromaSum[k].load() + c[k]);
}

//==============================================================================
void MilagrosNoteProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    auto mainBuf = getBusBuffer (buffer, true, 0);
    auto sideBuf = getBusBuffer (buffer, true, 1);
    const int numCh = juce::jmin (mainBuf.getNumChannels(), 2);
    const int n = mainBuf.getNumSamples();
    if (numCh == 0 || n == 0)
        return;

    // Posición en la línea de tiempo del DAW
    bool playing = true, havePos = false;
    juce::int64 blockPos = fallbackPos;
    if (auto* ph = getPlayHead())
    {
        if (auto pos = ph->getPosition())
        {
            playing = pos->getIsPlaying();
            if (auto t = pos->getTimeInSamples())
            {
                blockPos = *t;
                havePos = true;
            }
        }
    }
    if (! havePos)
        fallbackPos = playing ? fallbackPos + n : 0;
    havePosA.store (havePos);
    playingA.store (playing);
    posSeconds.store ((double) blockPos / sr);

    detector.setLevelThresholdDb (threshP->load());
    const bool voiceMode = modeP->load() < 0.5f;
    const bool captureOn = capturing.load();
    const bool liveOn = liveP->load() > 0.5f;
    const bool writeFrames = voiceMode && captureOn && playing;
    const bool useLive = voiceMode && liveOn && (captureOn || ! playing);
    const float stabilizeUnused = stabilizeP->load();
    juce::ignoreUnused (stabilizeUnused);

    const float transitionMs = juce::jmax (1.0f, transitionP->load());
    const float ratioCoef = 1.0f - std::exp (-1.0f / (transitionMs * 0.001f * (float) sr));
    const int scCh = sideBuf.getNumChannels();

    float* p0 = mainBuf.getWritePointer (0);
    float* p1 = numCh > 1 ? mainBuf.getWritePointer (1) : nullptr;

    for (int i = 0; i < n; ++i)
    {
        const juce::int64 s = blockPos + i;
        const float in0 = p0[i];
        const float in1 = p1 != nullptr ? p1[i] : 0.0f;
        const float mono = p1 != nullptr ? 0.5f * (in0 + in1) : in0;

        // Detector de nota de la voz
        if (voiceMode && detector.pushSample (mono))
            onDetect (s, writeFrames);

        // Acordes: del beat (sidechain) o de este canal si está en modo Instrumental
        bool feedChord = false;
        float chordIn = 0.0f;
        if (! voiceMode)
        {
            chordIn = mono;
            feedChord = true;
        }
        else if (scCh > 0)
        {
            float t = 0.0f;
            for (int ch = 0; ch < scCh; ++ch)
                t += sideBuf.getReadPointer (ch)[i];
            chordIn = t / (float) scCh;
            feedChord = true;
        }
        if (feedChord && chordAnalyzer.pushSample (chordIn) && playing)
            onChord (s);

        // Qué corrección aplicar en este instante
        float rT = 1.0f, per = periodSm;
        bool vF = false;
        if (voiceMode)
        {
            if (useLive)
            {
                rT = liveRatio;
                vF = liveVoiced;
                per = livePeriod;
            }
            else if (playing)
            {
                const juce::int64 lp = s - latency;
                if (lp >= 0)
                {
                    const int fi = (int) (lp / frameSize);
                    if (fi != lastFi)
                    {
                        lastFi = fi;
                        cachedVoiced = false;
                        cachedRatio = 1.0f;
                        if (fi < maxFrames)
                        {
                            const float f0 = f0Frames[(size_t) fi].load();
                            if (f0 > 0.0f)
                            {
                                cachedRatio = std::pow (2.0f, shiftFrames[(size_t) fi].load() / 12.0f);
                                cachedPeriod = (float) sr / f0;
                                cachedVoiced = true;
                            }
                        }
                    }
                    rT = cachedRatio;
                    vF = cachedVoiced;
                    per = cachedPeriod;
                }
            }
        }

        ratioSm += (rT - ratioSm) * ratioCoef;
        if (vF)
            periodSm = per;

        p0[i] = shifter[0].process (in0, ratioSm, periodSm, vF);
        if (p1 != nullptr)
            p1[i] = shifter[1].process (in1, ratioSm, periodSm, vF);
    }
}

//==============================================================================
// Edición de notas
void MilagrosNoteProcessor::clearCapture()
{
    capturing.store (false);
    const int n = capturedFrames.load();
    for (int i = 0; i < n && i < maxFrames; ++i)
    {
        f0Frames[(size_t) i].store (0.0f);
        shiftFrames[(size_t) i].store (0.0f);
    }
    capturedFrames.store (0);
    notes.clear();
    ++notesVersion;
}

void MilagrosNoteProcessor::rebuildNotes()
{
    notes.clear();
    ++notesVersion;

    const int N = capturedFrames.load();
    if (N <= 0)
        return;

    const float refA = refAP->load();
    std::vector<float> midi ((size_t) N, 0.0f);
    for (int f = 0; f < N; ++f)
    {
        const float f0 = f0Frames[(size_t) f].load();
        midi[(size_t) f] = f0 > 0.0f ? 69.0f + 12.0f * std::log2 (f0 / refA) : 0.0f;
    }

    // Mediana de 5 cuadros: quita picos sueltos
    std::vector<float> sm = midi;
    for (int f = 2; f < N - 2; ++f)
    {
        if (midi[(size_t) f] <= 0.0f)
            continue;
        float v[5];
        int c = 0;
        for (int k = -2; k <= 2; ++k)
            if (midi[(size_t) (f + k)] > 0.0f)
                v[c++] = midi[(size_t) (f + k)];
        if (c >= 3)
        {
            std::sort (v, v + c);
            sm[(size_t) f] = v[c / 2];
        }
    }

    // Separar en notas: una nota termina con un silencio o con un cambio de altura sostenido
    bool inNote = false;
    int start = 0, lastVoiced = 0, gap = 0, pendCount = 0, pendStart = 0;
    float ref = 0.0f;
    std::vector<float> vals, pend;

    auto closeNote = [&] (int endFrame)
    {
        if ((int) vals.size() >= 8)
        {
            Note nt;
            nt.startFrame = start;
            nt.endFrame = juce::jmax (start, endFrame);
            nt.medianMidi = medianOf (vals);
            nt.targetMidi = nt.medianMidi;
            nt.chord = chordForNote (nt);
            notes.push_back (nt);
        }
    };

    for (int f = 0; f < N; ++f)
    {
        const float m = sm[(size_t) f];
        if (m > 0.0f)
        {
            gap = 0;
            lastVoiced = f;
            if (! inNote)
            {
                start = f;
                vals.clear();
                vals.push_back (m);
                ref = m;
                pend.clear();
                pendCount = 0;
                inNote = true;
            }
            else if (std::abs (m - ref) > 0.8f)
            {
                if (pendCount == 0)
                    pendStart = f;
                ++pendCount;
                pend.push_back (m);
                if (pendCount >= 5)
                {
                    closeNote (pendStart - 1);
                    start = pendStart;
                    vals = pend;
                    ref = medianOf (pend);
                    pend.clear();
                    pendCount = 0;
                }
            }
            else
            {
                pend.clear();
                pendCount = 0;
                vals.push_back (m);
                ref += 0.1f * (m - ref);
            }
        }
        else if (inNote)
        {
            if (++gap > 6)
            {
                closeNote (lastVoiced);
                inNote = false;
                pend.clear();
                pendCount = 0;
            }
        }
    }
    if (inNote)
        closeNote (lastVoiced);
}

int MilagrosNoteProcessor::chordForNote (const Note& n) const
{
    auto& sh = SharedChords::get();
    const int hop = SharedChords::hopSamples;
    const int c0 = (int) (((juce::int64) n.startFrame * frameSize) / hop);
    const int c1 = (int) (((juce::int64) n.endFrame * frameSize) / hop);

    std::array<int, 24> votes {};
    for (int c = juce::jmax (0, c0); c <= c1 && c < SharedChords::maxFrames; ++c)
    {
        const int id = sh.chord[c].load();
        if (id >= 0 && id < 24)
            ++votes[(size_t) id];
    }
    int best = -1, bestVotes = 0;
    for (int i = 0; i < 24; ++i)
    {
        if (votes[(size_t) i] > bestVotes)
        {
            bestVotes = votes[(size_t) i];
            best = i;
        }
    }
    return best;
}

int MilagrosNoteProcessor::suggestTarget (const Note& n) const
{
    const int root = juce::jlimit (0, 11, (int) keyP->load());
    const int scale = juce::jlimit (0, afina::numScales - 1, (int) scaleP->load());
    const int chord = guideP->load() < 0.5f ? chordForNote (n) : -1;

    const float m = n.medianMidi;
    const int base = (int) std::floor (m);
    int best = (int) std::lround (m);
    float bestCost = 1.0e9f;

    for (int c = base - 3; c <= base + 4; ++c)
    {
        const int pc = ((c % 12) + 12) % 12;
        const bool chordTone = chord >= 0 && notechords::isChordTone (chord, pc);
        if (! afina::inScale (scale, root, pc) && ! chordTone)
            continue;
        float cost = std::abs (m - (float) c);
        if (chord >= 0 && ! chordTone)
            cost += 0.6f;     // preferimos las notas del acorde
        if (cost < bestCost)
        {
            bestCost = cost;
            best = c;
        }
    }
    return best;
}

void MilagrosNoteProcessor::applyShifts (int idx, float stabilize)
{
    if (idx < 0 || idx >= (int) notes.size())
        return;
    const auto& nt = notes[(size_t) idx];
    const float refA = refAP->load();

    for (int f = nt.startFrame; f <= nt.endFrame && f < maxFrames; ++f)
    {
        float sh = 0.0f;
        if (nt.edited)
        {
            sh = nt.targetMidi - nt.medianMidi;
            const float f0 = f0Frames[(size_t) f].load();
            if (f0 > 0.0f)
            {
                const float m = 69.0f + 12.0f * std::log2 (f0 / refA);
                sh -= stabilize * (m - nt.medianMidi);   // aplana parte del vibrato
            }
        }
        shiftFrames[(size_t) f].store (sh);
    }
}

void MilagrosNoteProcessor::applyAllShifts (float stabilizeOverride)
{
    const float st = stabilizeOverride >= 0.0f ? stabilizeOverride : stabilizeP->load();
    for (int i = 0; i < (int) notes.size(); ++i)
        applyShifts (i, st);
}

void MilagrosNoteProcessor::moveNote (int idx, int semitones)
{
    if (idx < 0 || idx >= (int) notes.size())
        return;
    auto& nt = notes[(size_t) idx];
    const float cur = nt.edited ? nt.targetMidi : (float) std::lround (nt.medianMidi);
    nt.targetMidi = std::round (cur) + (float) semitones;
    nt.edited = true;
    applyShifts (idx, stabilizeP->load());
}

void MilagrosNoteProcessor::snapNote (int idx)
{
    if (idx < 0 || idx >= (int) notes.size())
        return;
    auto& nt = notes[(size_t) idx];
    nt.chord = chordForNote (nt);
    nt.targetMidi = (float) suggestTarget (nt);
    nt.edited = true;
    applyShifts (idx, stabilizeP->load());
}

void MilagrosNoteProcessor::restoreNote (int idx)
{
    if (idx < 0 || idx >= (int) notes.size())
        return;
    auto& nt = notes[(size_t) idx];
    nt.edited = false;
    nt.targetMidi = nt.medianMidi;
    applyShifts (idx, stabilizeP->load());
}

void MilagrosNoteProcessor::autoCorrectAll()
{
    for (int i = 0; i < (int) notes.size(); ++i)
        snapNote (i);
}

void MilagrosNoteProcessor::restoreAll()
{
    for (int i = 0; i < (int) notes.size(); ++i)
        restoreNote (i);
}

std::array<float, 12> MilagrosNoteProcessor::getBeatChroma() const
{
    std::array<float, 12> r {};
    auto& sh = SharedChords::get();
    for (size_t i = 0; i < 12; ++i)
        r[i] = sh.chromaSum[i].load();
    return r;
}

//==============================================================================
juce::AudioProcessorEditor* MilagrosNoteProcessor::createEditor()
{
    return new MilagrosNoteEditor (*this);
}

void MilagrosNoteProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    if (auto old = state.getChildWithName ("NOTEDATA"); old.isValid())
        state.removeChild (old, nullptr);

    juce::ValueTree data ("NOTEDATA");
    const int N = juce::jmin (capturedFrames.load(), maxFrames);
    data.setProperty ("frames", N, nullptr);

    if (N > 0)
    {
        std::vector<float> f0v ((size_t) N);
        for (int i = 0; i < N; ++i)
            f0v[(size_t) i] = f0Frames[(size_t) i].load();
        data.setProperty ("f0", juce::Base64::toBase64 (f0v.data(), f0v.size() * sizeof (float)), nullptr);

        std::vector<float> tg;
        for (const auto& nt : notes)
        {
            tg.push_back (nt.edited ? 1.0f : 0.0f);
            tg.push_back (nt.targetMidi);
        }
        if (! tg.empty())
            data.setProperty ("targets", juce::Base64::toBase64 (tg.data(), tg.size() * sizeof (float)), nullptr);
    }

    auto& sh = SharedChords::get();
    const int mf = juce::jmin (sh.maxFrame.load(), SharedChords::maxFrames - 1);
    if (mf >= 0)
    {
        std::vector<juce::int8> cv ((size_t) mf + 1);
        for (int i = 0; i <= mf; ++i)
            cv[(size_t) i] = (juce::int8) sh.chord[i].load();
        data.setProperty ("chords", juce::Base64::toBase64 (cv.data(), cv.size()), nullptr);
    }

    state.appendChild (data, nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void MilagrosNoteProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    apvts.replaceState (juce::ValueTree::fromXml (*xml));

    auto nd = apvts.state.getChildWithName ("NOTEDATA");
    if (! nd.isValid())
        return;
    apvts.state.removeChild (nd, nullptr);

    const int N = juce::jmin ((int) nd.getProperty ("frames", 0), maxFrames);
    if (N > 0)
    {
        juce::MemoryOutputStream mo;
        if (juce::Base64::convertFromBase64 (mo, nd.getProperty ("f0").toString())
            && mo.getDataSize() >= (size_t) N * sizeof (float))
        {
            const auto* p = static_cast<const float*> (mo.getData());
            for (int i = 0; i < N; ++i)
            {
                f0Frames[(size_t) i].store (p[i]);
                shiftFrames[(size_t) i].store (0.0f);
            }
            capturedFrames.store (N);
            rebuildNotes();

            juce::MemoryOutputStream mt;
            if (juce::Base64::convertFromBase64 (mt, nd.getProperty ("targets").toString()))
            {
                const auto* t = static_cast<const float*> (mt.getData());
                const size_t pairs = mt.getDataSize() / (2 * sizeof (float));
                for (size_t i = 0; i < pairs && i < notes.size(); ++i)
                {
                    notes[i].edited = t[2 * i] > 0.5f;
                    notes[i].targetMidi = t[2 * i + 1];
                }
            }
            applyAllShifts();
        }
    }

    juce::MemoryOutputStream mc;
    if (juce::Base64::convertFromBase64 (mc, nd.getProperty ("chords").toString()))
    {
        auto& sh = SharedChords::get();
        const auto* c = static_cast<const juce::int8*> (mc.getData());
        const int cnt = (int) juce::jmin ((size_t) SharedChords::maxFrames, mc.getDataSize());
        for (int i = 0; i < cnt; ++i)
            sh.chord[i].store ((int) c[i]);
        if (cnt > 0)
            sh.maxFrame.store (cnt - 1);
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MilagrosNoteProcessor();
}
