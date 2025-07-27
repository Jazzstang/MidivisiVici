#include "TransformOctaveRandomizer.h"

TransformOctaveRandomizer::TransformOctaveRandomizer(juce::AudioProcessorValueTreeState& vts)
    : parameters(vts)
{
    addAndMakeVisible(amountSlider);
    amountSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    amountSlider.setRange(0.0, 1.0, 0.01);
    amountSlider.setTextValueSuffix(" Octaves");
    amountSlider.onValueChange = [this]
    {
        parameters.getParameter("octaveRandomizerAmount")->setValueNotifyingHost(
            (float)amountSlider.getValue());
    };
}

TransformOctaveRandomizer::~TransformOctaveRandomizer() = default;

void TransformOctaveRandomizer::resized()
{
    amountSlider.setBounds(getLocalBounds().reduced(10));
}
