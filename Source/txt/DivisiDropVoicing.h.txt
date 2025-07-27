#pragma once
#include <JuceHeader.h>

class DivisiDropVoicing : public juce::Component
{
public:
    explicit DivisiDropVoicing(juce::AudioProcessorValueTreeState& state);
    ~DivisiDropVoicing() override;

    void resized() override;

private:
    juce::AudioProcessorValueTreeState& parameters;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DivisiDropVoicing)
};
