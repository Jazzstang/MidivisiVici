#include "TransformStrummer.h"

TransformStrummer::TransformStrummer(juce::AudioProcessorValueTreeState& vts)
    : parameters(vts)
{
    addAndMakeVisible(directionBox);
    directionBox.addItem("Up", 1);
    directionBox.addItem("Down", 2);
    directionBox.onChange = [this]
    {
        parameters.getParameter("strummerDirection")->setValueNotifyingHost(
            (float)(directionBox.getSelectedId() - 1));
    };

    addAndMakeVisible(speedSlider);
    speedSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    speedSlider.setRange(1.0, 500.0, 1.0);
    speedSlider.setTextValueSuffix(" ms");
    speedSlider.onValueChange = [this]
    {
        parameters.getParameter("strummerSpeed")->setValueNotifyingHost(
            (float)speedSlider.getValue() / 500.0f);
    };
}

TransformStrummer::~TransformStrummer() = default;

void TransformStrummer::resized()
{
    auto area = getLocalBounds().reduced(10);
    directionBox.setBounds(area.removeFromTop(24));
    area.removeFromTop(8);
    speedSlider.setBounds(area.removeFromTop(40));
}
