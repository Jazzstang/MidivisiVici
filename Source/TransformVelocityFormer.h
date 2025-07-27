#pragma once
#include <JuceHeader.h>

class TransformVelocityFormer : public juce::Component
{
public:
    explicit TransformVelocityFormer(juce::AudioProcessorValueTreeState& vts);
    ~TransformVelocityFormer() override;

    void resized() override;

private:
    juce::AudioProcessorValueTreeState& parameters;
    juce::Slider scaleSlider;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TransformVelocityFormer)
};
