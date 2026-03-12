/**
 * @file LinkToggle.h
 * @brief Bouton de synchronisation sequencer.
 *
 * Modes supportes:
 * - mode standard (tri-etat): Link / F / Unlink.
 * - mode Rythm (binaire): R / F.
 *
 * Threading:
 * - UI thread uniquement.
 */

#pragma once

#include <JuceHeader.h>

#include "../PluginColours.h"

class LinkToggle : public juce::Component
{
public:
    enum Mode
    {
        Link = 0,
        F = 1,
        Unlink = 2
    };

    explicit LinkToggle(int layerIndexIn);

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseEnter(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;

    void setLayerIndex(int index);
    int  getLayerIndex() const noexcept { return layerIndex; }

    void setModeIndex(int newMode, juce::NotificationType notification = juce::dontSendNotification);
    int  getModeIndex() const noexcept { return modeIndex; }

    void setBinaryFRMode(bool shouldUseBinaryFR);
    bool isBinaryFRMode() const noexcept { return binaryFRMode; }

    void setUnlinkRateLabel(const juce::String& label);

    std::function<void(int newMode)> onModeChange;
    std::function<void()> onRequestRateMenu;

private:
    juce::String getDisplayText() const;
    int getNextModeFromClick() const noexcept;

    int layerIndex = 0;
    int modeIndex = Mode::F;
    bool binaryFRMode = false;
    juce::String unlinkRateLabel { "1/16" };
    bool hover = false;
    bool pendingRightClick = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LinkToggle)
};
