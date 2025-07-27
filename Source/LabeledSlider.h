// LabeledSlider.h
#pragma once

#include <JuceHeader.h>
#include "PluginColours.h"

class LabeledSlider : public juce::Component
{
public:
    enum class DisplayMode { None, Note, Range, Ratio, Simple };

    LabeledSlider();
    void resized() override;
    void paint(juce::Graphics& g) override;

    void setTitle(const juce::String& text);
    void setSliderStyle(juce::Slider::SliderStyle style);
    void setRange(double min, double max, double interval);
    void setDisplayMode(DisplayMode mode);
    void updateDisplay();

    juce::Slider& getSlider() { return slider; }

private:
    juce::Label titleLabel;
    juce::Slider slider;

    juce::Label leftLabel;
    juce::Label rightLabel;
    juce::Label centerLabel;

    DisplayMode mode = DisplayMode::None;

    juce::String midiNoteName(int value) const;
};
