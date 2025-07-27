#pragma once
#include <JuceHeader.h>

class DivisiSplit : public juce::Component
{
public:
    explicit DivisiSplit(juce::AudioProcessorValueTreeState& state);
    ~DivisiSplit() override;

    void resized() override;

private:
    juce::AudioProcessorValueTreeState& parameters;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DivisiSplit)
};
