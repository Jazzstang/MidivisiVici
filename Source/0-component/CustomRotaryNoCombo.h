/**
 * @file CustomRotaryNoCombo.h
 * @brief Rotary-only variant of `CustomRotaryWithCombo`.
 *
 * Threading:
 * - UI thread only.
 * - Not RT-safe.
 */
#pragma once
#include "CustomRotaryWithCombo.h"

/**
 * @brief Thin adapter that disables the embedded combo box.
 */
class CustomRotaryNoCombo : public CustomRotaryWithCombo
{
public:
    /**
     * @brief Create a rotary control without combo selector.
     */
    CustomRotaryNoCombo(const juce::String& labelText,
                        int minVal, int maxVal, int defaultVal,
                        bool centeredRange = false,
                        ArcMode mode = ArcMode::LeftToCursor)
        : CustomRotaryWithCombo(labelText, minVal, maxVal, defaultVal,
                                centeredRange, mode, false)
    {}

    /** @brief Layout override tuned for rotary-only rendering. */
    void resized() override;
};
