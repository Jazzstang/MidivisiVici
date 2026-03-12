/**
 * @file RotatedTextButton.h
 * @brief TextButton variant rendering its label rotated 90 degrees.
 *
 * Threading:
 * - UI thread only.
 * - Not RT-safe.
 */
#pragma once
#include <JuceHeader.h>

/** @brief Button that paints text rotated counter-clockwise by 90 degrees. */
class RotatedTextButton : public juce::TextButton {
public:
    using juce::TextButton::TextButton;

    /** @brief Paint rotated button text while keeping JUCE button behavior. */
    void paintButton(juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override {
        juce::Graphics::ScopedSaveState state(g);
        auto b = getLocalBounds().toFloat();
        g.addTransform(juce::AffineTransform::rotation(-juce::MathConstants<float>::halfPi, b.getCentreX(), b.getCentreY()));
        juce::TextButton::paintButton(g, shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);
    }
};
