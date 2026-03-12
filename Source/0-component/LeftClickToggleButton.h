/**
 * @file LeftClickToggleButton.h
 * @brief ToggleButton variant that ignores popup/right-click toggling.
 *
 * Use this for module title toggles: right-click must never flip the state.
 */
#pragma once

#include <JuceHeader.h>

class LeftClickToggleButton : public juce::ToggleButton
{
public:
    using juce::ToggleButton::ToggleButton;
    std::function<void(const juce::MouseEvent&)> onPopupClick;

private:
    static bool isPopupLikeClick(const juce::MouseEvent& e) noexcept
    {
        return e.mods.isPopupMenu() || e.mods.isRightButtonDown();
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (isPopupLikeClick(e))
            return;

        juce::ToggleButton::mouseDown(e);
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (isPopupLikeClick(e))
        {
            if (onPopupClick)
                onPopupClick(e);
            return;
        }

        juce::ToggleButton::mouseUp(e);
    }
};
