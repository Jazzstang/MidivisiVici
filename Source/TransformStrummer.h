#pragma once
#include <JuceHeader.h>

class TransformStrummer : public juce::Component
{
public:
    explicit TransformStrummer(juce::AudioProcessorValueTreeState& vts);
    ~TransformStrummer() override;

    void resized() override;

private:
    juce::AudioProcessorValueTreeState& parameters;
    juce::ComboBox directionBox;
    juce::Slider speedSlider;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TransformStrummer)
};
