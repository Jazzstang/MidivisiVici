#include "TransformVelocityFormer.h"

TransformVelocityFormer::TransformVelocityFormer(juce::AudioProcessorValueTreeState& vts)
    : parameters(vts)
{
    addAndMakeVisible(scaleSlider);
    scaleSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    scaleSlider.setRange(0.5, 2.0, 0.01);
    scaleSlider.setTextValueSuffix(" x");
    scaleSlider.onValueChange = [this]
    {
        parameters.getParameter("velocityScale")->setValueNotifyingHost(
            (float)((scaleSlider.getValue() - 0.5) / 1.5f));
    };
}

TransformVelocityFormer::~TransformVelocityFormer() = default;

void TransformVelocityFormer::resized()
{
    scaleSlider.setBounds(getLocalBounds().reduced(10));
}
