/**
 * @file ShadowComponent.h
 * @brief Utility component that paints a configurable drop shadow container.
 *
 * Threading:
 * - UI thread only.
 * - Not RT-safe.
 */
#pragma once
#include <JuceHeader.h>

/**
 * @brief Paints a drop shadow behind a rounded rectangle hosting child controls.
 */
class ShadowComponent : public juce::Component
{
public:
    /**
     * @brief Construct a shadow host.
     * @param shadow Drop shadow definition.
     * @param radius Corner radius; negative value enables dynamic pill radius.
     * @param topLeftCorner Enable top-left rounding.
     * @param topRightCorner Enable top-right rounding.
     * @param bottomLeftCorner Enable bottom-left rounding.
     * @param bottomRightCorner Enable bottom-right rounding.
     * @param margin Extra bounds margin used for shadow extents.
     */
    ShadowComponent(const juce::DropShadow& shadow,
                    float radius = -1.0f,
                    bool topLeftCorner = true, bool topRightCorner = true,
                    bool bottomLeftCorner = true, bool bottomRightCorner = true,
                    int margin = 12);

    void setShadow(const juce::DropShadow& newShadow);
    void setCornerRadii(float radius,
                        bool topLeftCorner,
                        bool topRightCorner,
                        bool bottomLeftCorner,
                        bool bottomRightCorner);
    void setMargin(int newMargin);
    juce::Rectangle<int> getShadowArea() const;

    void paint(juce::Graphics& g) override;
    bool hitTest(int x, int y) override;

private:
    /**
     * @brief Build rounded rectangle path with per-corner selection.
     *
     * If `cornerRadius < 0`, pill mode uses height-based radius.
     */
    void addSelectiveRoundedRect(juce::Path& path,
                                 juce::Rectangle<float> area) const;

    juce::DropShadow shadow;
    float cornerRadius;   // <0 → mode pilule dynamique
    bool topLeft, topRight, bottomLeft, bottomRight;
    int margin;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ShadowComponent)
};
