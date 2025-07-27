#pragma once
#include <JuceHeader.h>

class TransformTranspose : public juce::Component
{
public:
    explicit TransformTranspose(juce::AudioProcessorValueTreeState& vts);
    ~TransformTranspose() override;

    void resized() override;

private:
    juce::AudioProcessorValueTreeState& parameters;
    juce::Slider semitoneSlider;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TransformTranspose)
};
