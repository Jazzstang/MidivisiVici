/**
 * @file TwoLineToggleButton.h
 * @brief ToggleButton that renders a two-line label using project LookAndFeel.
 *
 * Threading:
 * - UI thread only.
 * - Not RT-safe.
 */
#pragma once
#include <JuceHeader.h>
#include "../PluginColours.h"
#include "../PluginLookAndFeel.h"

/** @brief Two-line toggle used by module sections and filter blocks. */
class TwoLineToggleButton : public juce::ToggleButton
{
public:
    TwoLineToggleButton(const juce::String& top, const juce::String& bottom)
        : topText(top), bottomText(bottom) {}

    void paintButton(juce::Graphics& g, bool shouldDrawButtonAsHighlighted,
                     bool shouldDrawButtonAsDown) override
    {
        const bool isEnabledNow = isEnabled(); // important

        if (auto* lf = dynamic_cast<PluginLookAndFeel*>(&getLookAndFeel()))
        {
            lf->drawTwoLineToggleButton(g, *this,
                                        shouldDrawButtonAsHighlighted,
                                        shouldDrawButtonAsDown,
                                        topText, bottomText,
                                        isEnabledNow); // <--- on transmet l'état
        }
        else
        {
            // Fallback simple avec désactivation visible
            auto bgColor = isEnabledNow ? juce::Colours::grey : juce::Colours::darkgrey;
            auto textColor = isEnabledNow ? juce::Colours::white : juce::Colours::lightgrey;

            g.fillAll(bgColor);
            g.setColour(textColor);
            g.setFont(juce::Font(juce::FontOptions(14.0f)));
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
