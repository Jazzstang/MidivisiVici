/**
 * @file PluginLookAndFeel.h
 * @brief Shared LookAndFeel implementation for all custom UI widgets.
 */
//==============================================================================
// PluginLookAndFeel.h
//------------------------------------------------------------------------------
// Central LookAndFeel for all plugin UI components.
//
// Responsibilities:
// - Provide unified color/shape/typography rendering.
// - Draw custom controls (module buttons, step cells, monitor toggles, etc.).
// - Cache reusable icon variants to reduce paint-time overhead.
//
// Thread model:
// - Message thread only (JUCE paint/layout callbacks).
// - Not RT-safe and never used from audio thread.
//==============================================================================

#pragma once
#include <JuceHeader.h>
#include "PluginColours.h"
#include <array>

/**
 * @brief Shared LookAndFeel used by the whole editor tree.
 *
 * Pattern:
 * - Pattern: Flyweight + Theme Facade
 * - Problem solved: avoid duplicated paint logic and ensure consistent visuals.
 * - Participants:
 *   - PluginLookAndFeel: draw methods + cached resources.
 *   - PluginColours: color tokens.
 *   - UI components: call JUCE virtual draw methods through LAF.
 * - Flow:
 *   1. Constructor loads fonts and icon variants.
 *   2. Components request draw* methods during paint.
 *   3. Cached assets are reused across paints.
 * - Pitfalls:
 *   - Expensive operations in draw* paths can freeze UI under MIDI bursts.
 *   - Paint logic must stay deterministic and allocation-light.
 */
class PluginLookAndFeel : public juce::LookAndFeel_V4
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
    
    struct ComponentStyle
    {
        enum Type
        {
            Default,
            MainMenu
        };
    };

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
    
    /** @brief Set contextual style variant used by draw methods. */
    void setContextStyle(ComponentStyle::Type style) { currentStyle = style; }
    ComponentStyle::Type getContextStyle() const { return currentStyle; }
    
    /** @brief Normalize displayed text while preserving known abbreviations. */
    static juce::String normaliseText(const juce::String& text);
    
    /**
     * Calcule un rayon dynamique pour un look "pilule".
     * Utilise la moitié de la plus petite dimension (largeur ou hauteur).
     */
    static float getDynamicCornerRadius(const juce::Rectangle<int>& bounds)
    {
        return std::min(bounds.getHeight() / 2.0f, bounds.getWidth() / 2.0f);
    }

    /** @brief Construct LAF and preload fonts/icon caches. */
    PluginLookAndFeel();
    ~PluginLookAndFeel() override;

    void setCornerRadius(float newRadius) { cornerRadius = newRadius; }
    float getCornerRadius() const { return cornerRadius; }

    // === Boutons custom ===
    void drawMidiMonitor(juce::Graphics& g,
                         juce::Rectangle<int> area,
                         const juce::StringArray& messages);

    void drawTwoLineToggleButton(juce::Graphics& g,
                                 juce::ToggleButton& button,
                                 bool isHighlighted,
                                 bool isDown,
                                 const juce::String& topText,
                                 const juce::String& bottomText,
                                 bool isEnabled);

    void drawCustomButtonBackground(juce::Graphics& g, juce::Component& button,
                                    bool isActive, bool isHighlighted, bool isDown,
                                    float cornerSize);

    void drawToggleButton(juce::Graphics&, juce::ToggleButton&,
                          bool isMouseOverButton, bool isButtonDown) override;
    
    void drawDrawableButton(juce::Graphics& g,
                            juce::DrawableButton& button,
                            bool isMouseOverButton,
                            bool isButtonDown) override;

    /** Version carré avec coins paramétrables */
    void drawSquareToggleButton(juce::Graphics&, juce::ToggleButton&,
                                bool isMouseOverButton, bool isButtonDown,
                                const CornerRadii& corners,
                                float fontSize);

    /** Update Monitor states used for conditional rounding */
    void setInputMonitorStates(bool notes, bool controls, bool clock, bool events);
    void setOutputMonitorStates(bool notes, bool controls, bool clock, bool events);
    
    // === StepToggle (StepSequencer) ===
    void drawStepToggle(juce::Graphics& g,
                        juce::Rectangle<int> bounds,
                        bool isSelected,
                        int layerIndex,
                        const juce::String& label,
                        bool isEnabled,
                        bool isHovered,
                        bool isPlayhead);
    
    // === Sliders ===
    void drawLinearSlider(juce::Graphics&, int x, int y, int width, int height,
                          float sliderPos, float minSliderPos, float maxSliderPos,
                          const juce::Slider::SliderStyle, juce::Slider&) override;
    
    void drawLinearSliderThumb(juce::Graphics&, int x, int y, int width, int height,
                               float sliderPos, float minSliderPos, float maxSliderPos,
                               const juce::Slider::SliderStyle, juce::Slider&) override;

    void drawRotarySlider(juce::Graphics&, int x, int y, int width, int height,
                          float sliderPosProportional, float rotaryStartAngle, float rotaryEndAngle,
                          juce::Slider&) override;
    
    void drawVerticalCentreSlider(juce::Graphics& g,
                                          juce::Slider& slider,
                                          int x, int y, int width, int height,
                                          float sliderPos);

    // === Labels ===
    void drawLabel(juce::Graphics&, juce::Label&) override;
    juce::Font getLabelFont(juce::Label&) override;

    // === ComboBox ===
    void drawComboBox(juce::Graphics&, int width, int height, bool isButtonDown,
                      int buttonX, int buttonY, int buttonW, int buttonH,
                      juce::ComboBox&) override;

    // === Save Me ===
    void drawIconToggleButton(juce::Graphics& g,
                              juce::ToggleButton& button,
                              bool isMouseOver,
                              bool isButtonDown);
    
    // === Popup Menu ===
    void drawPopupMenuBackground(juce::Graphics&, int width, int height) override;
    void drawPopupMenuItem(juce::Graphics&, const juce::Rectangle<int>& area,
                           bool isSeparator, bool isActive, bool isHighlighted,
                           bool isTicked, bool hasSubMenu, const juce::String& text,
                           const juce::String& shortcutKeyText,
                           const juce::Drawable* icon,
                           const juce::Colour* textColourToUse) override;

    // === Polices Jost ===
    juce::Font getJostFont(float size,
                           JostWeight weight = JostWeight::Regular,
                           bool italic = false);

private:
    enum class CachedIconTone : uint8_t
    {
        Primary = 0,
        OnBackground,
        Background,
        Disabled
    };

    struct CachedIconSet
    {
        std::array<std::unique_ptr<juce::Drawable>, 4> variants {};
    };

    ComponentStyle::Type currentStyle = ComponentStyle::Default;
    
    CornerRadii getCornerRadiiForButton(const juce::String& buttonId) const;

    bool imNotes = false, imControls = false, imClock = false, imEvents = false;
    bool omNotes = false, omControls = false, omClock = false, omEvents = false;
    
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

    CachedIconSet saveIconSet;
    CachedIconSet copyIconSet;
    CachedIconSet pasteIconSet;
    CachedIconSet deleteIconSet;

    float cornerRadius = defaultCornerRadius;

    static std::unique_ptr<juce::Drawable> createTintedIcon(const void* data, int dataSize, juce::Colour colour);
    static void loadCachedIconSet(CachedIconSet& target, const void* data, int dataSize);
    static const juce::Drawable* getCachedIconVariant(const CachedIconSet& set, CachedIconTone tone) noexcept;
    static float resolveToggleFontSize(juce::ToggleButton& button);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginLookAndFeel)
};
