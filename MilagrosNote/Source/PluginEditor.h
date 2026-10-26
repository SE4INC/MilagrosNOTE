#pragma once

#include "PluginProcessor.h"

class MilagrosNoteEditor : public juce::AudioProcessorEditor,
                           private juce::Timer
{
public:
    explicit MilagrosNoteEditor (MilagrosNoteProcessor&);
    ~MilagrosNoteEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    class PianoRoll;
    class MainPage;
    class AdvisorPage;

    MilagrosNoteProcessor& proc;
    int tick = 0;
    std::unique_ptr<MainPage> mainPage;
    std::unique_ptr<AdvisorPage> advisorPage;
    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MilagrosNoteEditor)
};
