#pragma once
#include <JuceHeader.h>
#include "PluginColours.h"
#include "PluginLookAndFeel.h"

class TwoLineToggleButton : public juce::ToggleButton
{
public:
    TwoLineToggleButton(const juce::String& top, const juce::String& bottom)
        : topText(top), bottomText(bottom) {}

    void paintButton(juce::Graphics& g, bool shouldDrawButtonAsHighlighted,
                     bool shouldDrawButtonAsDown) override
    {
        if (auto* lf = dynamic_cast<PluginLookAndFeel*>(&getLookAndFeel()))
        {
            lf->drawTwoLineToggleButton(g, *this,
                                        shouldDrawButtonAsHighlighted,
                                        shouldDrawButtonAsDown,
                                        topText, bottomText);
        }
        else
        {
            // Fallback simple
            g.fillAll(juce::Colours::grey);
            g.setColour(juce::Colours::white);
            g.setFont(14.0f);
            auto bounds = getLocalBounds();
            g.drawFittedText(topText, bounds.removeFromTop(bounds.getHeight() / 2),
                             juce::Justification::centred, 1);
            g.drawFittedText(bottomText, bounds,
                             juce::Justification::centred, 1);
        }
    }

private:
    juce::String topText, bottomText;
};
