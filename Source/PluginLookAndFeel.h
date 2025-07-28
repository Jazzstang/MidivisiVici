#pragma once
#include <JuceHeader.h>
#include "PluginColours.h"

/**
 * Custom LookAndFeel for a macOS 26 dark mode-inspired, highly rounded UI.
 * Use getCornerRadius() for all rounded control drawing.
 */
class PluginLookAndFeel : public juce::LookAndFeel_V4,
                          private juce::Timer
{
public:
    struct CornerRadii
    {
        bool topLeft = false;
        bool topRight = false;
        bool bottomLeft = false;
        bool bottomRight = false;
        float radius = 4.0f;
    };
    /**
     * Default corner radius for macOS 26 style rounding.
     * Use getCornerRadius() in your drawing code for consistent rounding across the UI.
     */
    static constexpr float defaultCornerRadius = 8.0f;

    enum class JostWeight
    {
        Thin,
        ExtraLight,
        Light,
        Regular,
        Medium,
        SemiBold,
        Bold,
        ExtraBold,
        Black
    };

    PluginLookAndFeel();
    ~PluginLookAndFeel() override;

    /// Set the preferred corner radius. Call this before UI is drawn.
    void setCornerRadius(float newRadius) { cornerRadius = newRadius; }
    /// Get the current corner radius for all rounded controls.
    float getCornerRadius() const { return cornerRadius; }

    // === Couleur animée ===
    juce::Colour getPrimaryColour(float globalY, float totalHeight) const;

    // === Boutons custom ===
    void drawMidiMonitor(juce::Graphics& g,
                         juce::Rectangle<int> area,
                         const juce::StringArray& messages);

    void drawTwoLineToggleButton(juce::Graphics&, juce::ToggleButton&,
                                 bool isHighlighted, bool isDown,
                                 const juce::String& top, const juce::String& bottom);

    /** Uses getCornerRadius() for macOS-like rounding. */
    void drawCustomButtonBackground(juce::Graphics& g, juce::Component& button,
                                    bool isActive, bool isHighlighted, bool isDown,
                                    float cornerSize);

    /** Uses getCornerRadius() for macOS-like rounding. */
    void drawToggleButton(juce::Graphics&, juce::ToggleButton&,
                          bool isMouseOverButton, bool isButtonDown) override;

    /** Uses getCornerRadius() for macOS-like rounding. */
    void drawRoundedToggleButton(juce::Graphics&, juce::ToggleButton&,
                                 bool isMouseOverButton, bool isButtonDown);

    /** Uses getCornerRadius() for macOS-like rounding. */
    void drawSquareToggleButton(juce::Graphics&, juce::ToggleButton&,
                                bool isMouseOverButton, bool isButtonDown,
                                float fontSize = 24.0f);

    /** Custom version with individually rounded corners */
    void drawSquareToggleButton(juce::Graphics&, juce::ToggleButton&,
                                bool isMouseOverButton, bool isButtonDown,
                                const CornerRadii&, float fontSize = 24.0f);

    /** Update Input Monitor states used for conditional rounding */
    void setMonitorStates(bool notes, bool controls, bool clock, bool events);

    // === Sliders ===
    /** Uses getCornerRadius() for macOS-like rounding. */
    void drawLinearSlider(juce::Graphics&, int x, int y, int width, int height,
                          float sliderPos, float minSliderPos, float maxSliderPos,
                          const juce::Slider::SliderStyle, juce::Slider&) override;
    
    /** Uses getCornerRadius() for macOS-like rounding. */
    void drawLinearSliderThumb(juce::Graphics&, int x, int y, int width, int height,
                               float sliderPos, float minSliderPos, float maxSliderPos,
                               const juce::Slider::SliderStyle, juce::Slider&) override;

    /** Uses getCornerRadius() for macOS-like rounding. */
    void drawRotarySlider(juce::Graphics&, int x, int y, int width, int height,
                          float sliderPosProportional, float rotaryStartAngle, float rotaryEndAngle,
                          juce::Slider&) override;

    // === Labels ===
    /** Uses getCornerRadius() for macOS-like rounding. */
    void drawLabel(juce::Graphics&, juce::Label&) override;
    juce::Font getLabelFont(juce::Label&) override;

    // === ComboBox ===
    /** Uses getCornerRadius() for macOS-like rounding. */
    void drawComboBox(juce::Graphics&, int width, int height, bool isButtonDown,
                      int buttonX, int buttonY, int buttonW, int buttonH,
                      juce::ComboBox&) override;

    // === Popup Menu ===
    /** Uses getCornerRadius() for macOS-like rounding. */
    void drawPopupMenuBackground(juce::Graphics&, int width, int height) override;
    /** Uses getCornerRadius() for macOS-like rounding. */
    void drawPopupMenuItem(juce::Graphics&, const juce::Rectangle<int>& area,
                           bool isSeparator, bool isActive, bool isHighlighted,
                           bool isTicked, bool hasSubMenu, const juce::String& text,
                           const juce::String& shortcutKeyText,
                           const juce::Drawable* icon,
                           const juce::Colour* textColourToUse) override;

    // === Accès polices Jost ===
    juce::Font getJostFont(float size,
                           JostWeight weight = JostWeight::Regular,
                           bool italic = false);

private:
    CornerRadii getCornerRadiiForButton(const juce::String& buttonId) const;

    bool notesState = false;
    bool controlsState = false;
    bool clockState = false;
    bool eventsState = false;

    // Polices Jost
    juce::Typeface::Ptr jostThin, jostThinItalic;
    juce::Typeface::Ptr jostExtraLight, jostExtraLightItalic;
    juce::Typeface::Ptr jostLight, jostLightItalic;
    juce::Typeface::Ptr jostRegular, jostItalic;
    juce::Typeface::Ptr jostMedium, jostMediumItalic;
    juce::Typeface::Ptr jostSemiBold, jostSemiBoldItalic;
    juce::Typeface::Ptr jostBold, jostBoldItalic;
    juce::Typeface::Ptr jostExtraBold, jostExtraBoldItalic;
    juce::Typeface::Ptr jostBlack, jostBlackItalic;

    float cornerRadius = defaultCornerRadius;
    float animationOffset = 0.0f;
    void timerCallback() override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginLookAndFeel)
};

