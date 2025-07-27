#pragma once
#include <JuceHeader.h>

class TransformOctaveRandomizer : public juce::Component
{
public:
    explicit TransformOctaveRandomizer(juce::AudioProcessorValueTreeState& vts);
    ~TransformOctaveRandomizer() override;

    void resized() override;

private:
    juce::AudioProcessorValueTreeState& parameters;
    juce::Slider amountSlider;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TransformOctaveRandomizer)
};
