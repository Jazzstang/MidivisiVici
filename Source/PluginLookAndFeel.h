#pragma once
#include <JuceHeader.h>
#include "PluginColours.h"

class PluginLookAndFeel : public juce::LookAndFeel_V4
{
public:
    PluginLookAndFeel()
    {
        // Tu peux aussi configurer le LookAndFeel_V4::ColourScheme si tu veux :
        setColourScheme (juce::LookAndFeel_V4::getDarkColourScheme());
    }

    // ================================================
    // === BOUTONS STANDARD
    // ================================================
    void drawButtonBackground (juce::Graphics& g, juce::Button& button,
                               const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted,
                               bool shouldDrawButtonAsDown) override
    {
        auto bounds = button.getLocalBounds().toFloat();
        auto cornerSize = 4.0f;

        juce::Colour fill;

        if (shouldDrawButtonAsDown)
            fill = PluginColours::pressed;
        else if (shouldDrawButtonAsHighlighted)
            fill = PluginColours::hover;
        else
            fill = PluginColours::surface;

        g.setColour (fill);
        g.fillRoundedRectangle (bounds, cornerSize);
    }

    void drawToggleButton (
        juce::Graphics& g,
        juce::ToggleButton& button,
        bool isMouseOverButton,
        bool isButtonDown
    ) override
    {
        auto bounds = button.getLocalBounds();

        // Fond plein
        if (button.getToggleState())
            g.setColour (PluginColours::primary);
        else if (isButtonDown)
            g.setColour (PluginColours::pressed);
        else if (isMouseOverButton)
            g.setColour (PluginColours::hover);
        else
            g.setColour (PluginColours::surface);

        g.fillRect (bounds);

        // Texte
        g.setColour (PluginColours::onSurface);
        g.setFont (14.0f);
        g.drawText (
            button.getButtonText(),
            bounds,
            juce::Justification::centred
        );
    }

    // ================================================
    // === SLIDERS
    // ================================================
    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float /*minSliderPos*/, float /*maxSliderPos*/,
                           const juce::Slider::SliderStyle style, juce::Slider& slider) override
    {
        auto trackHeight = 4.0f;
        auto trackY = y + height * 0.5f - trackHeight * 0.5f;

        // Track
        g.setColour (PluginColours::divider);
        g.fillRect (
            static_cast<float>(x),
            trackY,
            static_cast<float>(width),
            trackHeight
        );

        // Value
        g.setColour (PluginColours::primary);
        g.fillRect (
            static_cast<float>(x),
            trackY,
            sliderPos - static_cast<float>(x),
            trackHeight
        );

        // Thumb
        auto thumbRadius = 6.0f;
        g.setColour (PluginColours::primary);
        g.fillEllipse (
            sliderPos - thumbRadius,
            trackY + trackHeight * 0.5f - thumbRadius,
            thumbRadius * 2,
            thumbRadius * 2
        );
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPosProportional, float rotaryStartAngle, float rotaryEndAngle,
                           juce::Slider& slider) override
    {
        auto radius = juce::jmin (width / 2, height / 2) - 4.0f;
        auto centreX = x + width * 0.5f;
        auto centreY = y + height * 0.5f;

        auto outline = juce::Colours::darkgrey;
        auto fill = PluginColours::primary;

        auto toAngle = rotaryStartAngle + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);

        // Background arc
        g.setColour (PluginColours::divider);
        g.drawEllipse (centreX - radius, centreY - radius, radius * 2, radius * 2, 2.0f);

        // Value arc
        g.setColour (fill);
        juce::Path valueArc;
        valueArc.addCentredArc (centreX, centreY, radius, radius, 0.0f, rotaryStartAngle, toAngle, true);
        g.strokePath (valueArc, juce::PathStrokeType (3.0f));

        // Thumb
        auto thumbX = centreX + radius * std::cos (toAngle - juce::MathConstants<float>::halfPi);
        auto thumbY = centreY + radius * std::sin (toAngle - juce::MathConstants<float>::halfPi);

        g.setColour (fill);
        g.fillEllipse (thumbX - 4.0f, thumbY - 4.0f, 8.0f, 8.0f);
    }

    // ================================================
    // === LABELS
    // ================================================
    void drawLabel (juce::Graphics& g, juce::Label& label) override
    {
        g.fillAll (label.findColour (juce::Label::backgroundColourId));

        if (label.isBeingEdited())
            g.setColour (PluginColours::onBackground);
        else
            g.setColour (PluginColours::onSurface);

        g.setFont (getLabelFont (label));
        auto textArea = label.getBorderSize().subtractedFrom (label.getLocalBounds());

        g.drawFittedText (label.getText(), textArea, label.getJustificationType(), 2);
    }
};
