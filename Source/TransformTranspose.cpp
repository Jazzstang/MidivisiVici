#include "TransformTranspose.h"

TransformTranspose::TransformTranspose(juce::AudioProcessorValueTreeState& vts)
    : parameters(vts)
{
    addAndMakeVisible(semitoneSlider);
    semitoneSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    semitoneSlider.setRange(-12.0, 12.0, 1.0);
    semitoneSlider.setTextValueSuffix(" st");
    semitoneSlider.onValueChange = [this]
    {
        parameters.getParameter("pitchShift")->setValueNotifyingHost(
            (float)(semitoneSlider.getValue() + 12.0f) / 24.0f);
    };
}

TransformTranspose::~TransformTranspose() = default;

void TransformTranspose::resized()
{
    semitoneSlider.setBounds(getLocalBounds().reduced(10));
}
