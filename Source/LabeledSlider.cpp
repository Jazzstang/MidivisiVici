// LabeledSlider.cpp
#include "LabeledSlider.h"

LabeledSlider::LabeledSlider()
{
    titleLabel.setFont(14.0f);
    titleLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(titleLabel);

    slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    slider.setSliderStyle(juce::Slider::LinearHorizontal);
    addAndMakeVisible(slider);

    leftLabel.setJustificationType(juce::Justification::centredLeft);
    leftLabel.setName("value_num_min");
    addAndMakeVisible(leftLabel);

    rightLabel.setJustificationType(juce::Justification::centredRight);
    rightLabel.setName("value_num_max");
    addAndMakeVisible(rightLabel);

    centerLabel.setJustificationType(juce::Justification::centred);
    centerLabel.setName("value_ratio");
    addAndMakeVisible(centerLabel);
}

void LabeledSlider::setTitle(const juce::String& text)
{
    titleLabel.setText(text, juce::dontSendNotification);
}

void LabeledSlider::setSliderStyle(juce::Slider::SliderStyle style)
{
    slider.setSliderStyle(style);
}

void LabeledSlider::setRange(double min, double max, double interval)
{
    slider.setRange(min, max, interval);
}

void LabeledSlider::setDisplayMode(DisplayMode m)
{
    mode = m;
    leftLabel.setVisible(mode == DisplayMode::Note || mode == DisplayMode::Range);
    rightLabel.setVisible(mode == DisplayMode::Note || mode == DisplayMode::Range);
    centerLabel.setVisible(mode == DisplayMode::Ratio || mode == DisplayMode::Simple);
    updateDisplay();
}

void LabeledSlider::updateDisplay()
{
    if (mode == DisplayMode::Note)
    {
        leftLabel.setText(midiNoteName((int)slider.getMinValue()), juce::dontSendNotification);
        rightLabel.setText(midiNoteName((int)slider.getMaxValue()), juce::dontSendNotification);
    }
    else if (mode == DisplayMode::Range)
    {
        leftLabel.setText("min: " + juce::String((int)slider.getMinValue()), juce::dontSendNotification);
        rightLabel.setText("max: " + juce::String((int)slider.getMaxValue()), juce::dontSendNotification);
    }
    else if (mode == DisplayMode::Ratio)
    {
        centerLabel.setText(juce::String((int)slider.getMinValue()) + "/" + juce::String((int)slider.getMaxValue()), juce::dontSendNotification);
    }
    else if (mode == DisplayMode::Simple)
    {
        centerLabel.setText(juce::String((int)slider.getValue()), juce::dontSendNotification);
    }
}

void LabeledSlider::resized()
{
    auto area = getLocalBounds();

    auto titleHeight = 12;                                  // Titre plus lisible
    auto labelHeight = 12;                                  // Labels compacts
    auto spacing = 0;                                       // Ajoute un peu d'espace vertical

    titleLabel.setBounds(area.removeFromTop(titleHeight));

    auto sliderHeight = 14;  // +2px
    slider.setBounds(area.removeFromTop(sliderHeight));

    if (mode == DisplayMode::Note || mode == DisplayMode::Range)
    {
        leftLabel.setBounds(slider.getX(), slider.getBottom() + spacing, 60, labelHeight);
        rightLabel.setBounds(slider.getRight() - 60, slider.getBottom() + spacing, 60, labelHeight);
    }
    else if (mode == DisplayMode::Ratio || mode == DisplayMode::Simple)
    {
        centerLabel.setBounds(slider.getX(), slider.getBottom() + spacing, slider.getWidth(), labelHeight);
    }
}

void LabeledSlider::paint(juce::Graphics& g)
{
    updateDisplay();
}

juce::String LabeledSlider::midiNoteName(int value) const
{
    static const juce::StringArray names = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    int octave = (value / 12) - 1;
    int index = value % 12;
    return names[index] + juce::String(octave);
}
