/**
 * @file LabeledSlider.h
 * @brief Slider wrapper with title and contextual labels.
 *
 * Threading:
 * - UI thread only.
 * - Not RT-safe.
 */
#pragma once

#include <JuceHeader.h>
#include "../PluginColours.h"
#include "ShadowComponent.h"

/**
 * @brief Slider component with configurable value display semantics.
 */
class LabeledSlider : public juce::Component
{
public:
    /** @brief Value text formatting modes for labels. */
    enum class DisplayMode { None, Note, Range, Ratio, Simple };

    /** @brief Override track color. */
    void setTrackColour(juce::Colour c);
    /** @brief Override background color. */
    void setBackgroundColour(juce::Colour c);
    /** @brief Override thumb color. */
    void setThumbColour(juce::Colour c);
    /** @brief Override text color. */
    void setTextColour(juce::Colour c);

    LabeledSlider();
    void resized() override;
    void paint(juce::Graphics& g) override;

    /** @brief Set header text. */
    void setTitle(const juce::String& text);
    /** @brief Set underlying JUCE slider style. */
    void setSliderStyle(juce::Slider::SliderStyle style);
    /** @brief Set value range and step interval. */
    void setRange(double min, double max, double interval);
    /** @brief Set display mode used for edge/center labels. */
    void setDisplayMode(DisplayMode mode);
    /** @brief Recompute labels from current slider value. */
    void updateDisplay();

    /** Retourne, en coordonnées locales du LabeledSlider, le Y du centre du track. */
    int getTrackCenterY() const;

    juce::Slider& getSlider() { return slider; }

    /** @brief Set palette layer index (-1 uses contrast palette). */
    void setLayerIndex (int idx);
    /** @brief Return active layer index. */
    int  getLayerIndex() const noexcept { return layerIndex; }
    
    /** @brief Apply enabled/disabled style and interaction state. */
    void setEnabled(bool shouldBeEnabled);

    /** @brief Re-apply palette from current layer index. */
    void refreshPalette();

private:
    juce::Label  titleLabel;
    juce::Slider slider;

    juce::Label leftLabel;
    juce::Label rightLabel;
    juce::Label centerLabel;

    DisplayMode mode = DisplayMode::None;

    juce::String midiNoteName(int value) const;

    // -1 = pas de layer => palette "contrast" ; >=0 => layX
    int  layerIndex = -1;
    void applyPaletteFromLayer();
    void updateEnabledState();
};
