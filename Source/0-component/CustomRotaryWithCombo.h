/**
 * @file CustomRotaryWithCombo.h
 * @brief Rotary UI control with optional compact combo selector.
 *
 * Threading:
 * - UI thread only.
 * - Not RT-safe (uses JUCE UI state, repaint, callbacks).
 */
#pragma once

#include <JuceHeader.h>
#include "FlatComboBox.h"
#include "ShadowComponent.h"
#include "../PluginColours.h"

/**
 * @brief Combined rotary + combo control used across modules.
 *
 * Pattern:
 * - Pattern: Strategy + Observer
 * - Problem solved: expose one control with multiple value presentation modes
 *   (arc strategy + numeric/string list) and user callbacks.
 * - Participants: `CustomRotaryWithCombo` (subject), `ArcMode` (strategy),
 *   external callbacks (`onValueChange`, `onMouseDownLayerChange`) as observers.
 * - Flow: user interaction -> local value update -> optional combo sync ->
 *   callback notification.
 * - Pitfalls: UI-only component, no access from audio thread.
 */
class CustomRotaryWithCombo : public juce::Component
{
public:
    /** @brief Arc rendering strategy for the progress segment. */
    enum class ArcMode { LeftToCursor, RightToCursor, CenterToCursor };

    std::function<void(int)> onMouseDownLayerChange;

    juce::Label titleLabel;

    /**
     * @brief Create a rotary control.
     * @param labelText Display label.
     * @param minVal Minimum value.
     * @param maxVal Maximum value.
     * @param defaultVal Initial value.
     * @param centeredRange True when value semantics are centered around zero.
     * @param mode Arc rendering mode.
     * @param showComboBox True to display the combo selector under the dial.
     */
    CustomRotaryWithCombo(const juce::String& labelText,
                          int minVal, int maxVal, int defaultVal,
                          bool centeredRange = false,
                          ArcMode mode = ArcMode::LeftToCursor,
                          bool showComboBox = true);

    void setArcMode(ArcMode mode);
    void setRange(int minVal, int maxVal);
    void setValue(int newVal, bool sendNotification = true);
    int  getValue() const noexcept { return value; }

    /** @brief Enable/disable inverted arc fill direction. */
    void setInvertedFill(bool shouldInvert)   { invertedFill = shouldInvert; }
    /** @brief Return true if arc fill is inverted. */
    bool getInvertedFill() const noexcept     { return invertedFill; }
    /** @brief Enable/disable smoothed display interpolation. */
    void setUseSmoothing(bool shouldSmooth);

    /** @brief Enable per-layer color palette. */
    void setUseLayerColours(bool shouldUse)
    {
        if (useLayerColours == shouldUse)
            return;

        useLayerColours = shouldUse;
        repaint();
    }
    /** @brief Set layer index used by palette lookup. */
    void setLayerIndex(int index)
    {
        if (layerIndex == index)
            return;

        layerIndex = index;
        repaint();
    }
    /** @brief Return active layer index. */
    int  getLayerIndex() const noexcept       { return layerIndex; }

    /** @brief Replace combo content with explicit labels. */
    void setStringList(const std::vector<juce::String>& items);
    /** @brief Return currently selected label when string mode is enabled. */
    juce::String getSelectedString() const;
    /** @brief Show plus sign for positive values in numeric mode. */
    void setShowPlusSign(bool shouldShow)
    {
        if (showPlusSign == shouldShow)
            return;

        showPlusSign = shouldShow;

        if (!useStringList && showComboBox)
        {
            buildComboItems();
            const int selectedId = value - minValue + 1;
            syncingComboSelection = true;
            combo.setSelectedId(selectedId, juce::dontSendNotification);
            syncingComboSelection = false;
        }

        repaint();
    }
    /** @brief Set text rendered at rotary center (empty string disables it). */
    void setCenterOverlayText(const juce::String& text);
    /** @brief Set color used for center overlay text. */
    void setCenterOverlayColour(juce::Colour colour);
    /**
     * @brief Set UI-only display override value (does not change underlying value).
     *
     * This is used for visual modulation previews (e.g. LFO) without touching
     * APVTS/base state.
     */
    void setDisplayValueOverride(float visualValue);
    /** @brief Clear UI-only display override and return to regular displayValue path. */
    void clearDisplayValueOverride();
    /** @brief UI scheduler tick hook (called by shared UI timer). */
    void uiTimerTick();

    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    void resized() override;
    void paint(juce::Graphics& g) override;

    std::function<void(int)> onValueChange;
    
    juce::Rectangle<int> rotaryBounds;

private:
    void buildComboItems();
    void updateFromCombo();

    FlatComboBox combo;
    std::unique_ptr<ShadowComponent> comboShadow;

    int minValue, maxValue, value;
    int valueAtMouseDown = 0;
    float displayValue = 0.0f;
    ArcMode arcMode;

    bool isDragging = false;
    bool useSmoothing = true;
    bool smoothingActive = false;
    bool invertedFill = false;
    bool showComboBox = true;

    bool useLayerColours = false;
    int layerIndex = -1;

    // === Ajouts stringList ===
    std::vector<juce::String> stringList;
    bool useStringList = false;
    bool showPlusSign = true;
    juce::String centerOverlayText;
    juce::Colour centerOverlayColour = juce::Colours::white;
    bool syncingComboSelection = false;
    bool hasDisplayValueOverride = false;
    float displayValueOverride = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CustomRotaryWithCombo)
};
