// MilagrosNote - edición de notas de voz, gratuita y de código abierto (AGPL-3.0)
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <memory>
#include <vector>

#include "Scales.h"
#include "PitchDetector.h"
#include "PitchShifter.h"
#include "KeyAnalyzer.h"
#include "ChordAnalyzer.h"

inline juce::String es (const char* utf8) { return juce::String::fromUTF8 (utf8); }

class MilagrosNoteProcessor : public juce::AudioProcessor
{
public:
    static constexpr int maxFrames = 200000;   // unos 13 minutos (un cuadro cada ~4 ms)

    struct Note
    {
        int startFrame = 0, endFrame = 0;
        float medianMidi = 60.0f, targetMidi = 60.0f;
        bool edited = false;
        int chord = -1;
    };

    MilagrosNoteProcessor();
    ~MilagrosNoteProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "MilagrosNote"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // ---------- Interfaz (solo desde el hilo de la interfaz) ----------
    void setCapturing (bool on) { capturing.store (on); }
    bool isCapturing() const { return capturing.load(); }
    void clearCapture();
    void rebuildNotes();
    const std::vector<Note>& getNotes() const { return notes; }
    int getNotesVersion() const { return notesVersion; }

    void moveNote (int idx, int semitones);
    void snapNote (int idx);        // a la nota que pide el beat
    void restoreNote (int idx);
    void autoCorrectAll();          // corrección total
    void restoreAll();
    void applyAllShifts (float stabilizeOverride = -1.0f);

    int suggestTarget (const Note& n) const;
    int chordForNote (const Note& n) const;
    std::array<float, 12> getBeatChroma() const;

    int getFrameSize() const { return frameSize; }
    double getSampleRate() const { return sr; }
    int getCapturedFrames() const { return capturedFrames.load(); }
    float getF0 (int frame) const { return frame >= 0 && frame < maxFrames ? f0Frames[(size_t) frame].load() : 0.0f; }
    float getShift (int frame) const { return frame >= 0 && frame < maxFrames ? shiftFrames[(size_t) frame].load() : 0.0f; }
    float getRefA() const { return refAP->load(); }
    double getPositionSeconds() const { return posSeconds.load(); }
    bool isPlayingNow() const { return playingA.load(); }
    bool hasHostPosition() const { return havePosA.load(); }

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void onDetect (juce::int64 s, bool write);
    void onChord (juce::int64 s);
    void applyShifts (int idx, float stabilize);
    float medianF0() const;

    double sr = 44100.0;
    int frameSize = 192, latency = 600, detectDelay = 700;
    double hopSeconds = 0.004;

    PitchDetector detector;
    PitchShifter shifter[2];
    ChordAnalyzer chordAnalyzer;

    // Cuadros de la línea de tiempo (leídos por el hilo de audio, escritos por ambos)
    std::unique_ptr<std::atomic<float>[]> f0Frames, shiftFrames;
    std::atomic<int> capturedFrames { 0 };
    std::atomic<bool> capturing { false };

    // Notas (solo hilo de la interfaz)
    std::vector<Note> notes;
    int notesVersion = 0;

    // Estado de reproducción
    juce::int64 fallbackPos = 0;
    std::atomic<double> posSeconds { 0.0 };
    std::atomic<bool> playingA { false }, havePosA { false };
    float ratioSm = 1.0f, periodSm = 300.0f;
    int lastFi = -1;
    float cachedRatio = 1.0f, cachedPeriod = 300.0f;
    bool cachedVoiced = false;

    // Corrección en vivo (para escucharte afinado mientras cantas)
    std::array<float, 5> f0Ring {};
    int f0Count = 0, f0Pos = 0, jumpCount = 0, liveUnvoiced = 100, liveTarget = 60;
    bool liveTargetValid = false, liveVoiced = false;
    float liveCorr = 0.0f, liveRatio = 1.0f, livePeriod = 300.0f;

    std::atomic<float> *modeP = nullptr, *keyP = nullptr, *scaleP = nullptr, *guideP = nullptr,
                       *stabilizeP = nullptr, *transitionP = nullptr, *refAP = nullptr,
                       *threshP = nullptr, *liveP = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MilagrosNoteProcessor)
};
