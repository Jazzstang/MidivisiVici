#pragma once
#include <JuceHeader.h>

class DivisiRoundRobin : public juce::Component
{
public:
    explicit DivisiRoundRobin(juce::AudioProcessorValueTreeState& state);
    ~DivisiRoundRobin() override;

    void resized() override;

private:
    juce::AudioProcessorValueTreeState& parameters;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DivisiRoundRobin)
};
