#pragma once
#include <JuceHeader.h>

class DivisiOctaveDoubling : public juce::Component
{
public:
    explicit DivisiOctaveDoubling(juce::AudioProcessorValueTreeState& state);
    ~DivisiOctaveDoubling() override;

    void resized() override;

private:
    juce::AudioProcessorValueTreeState& parameters;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DivisiOctaveDoubling)
};
