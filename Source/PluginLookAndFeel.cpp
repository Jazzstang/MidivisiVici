#include "PluginLookAndFeel.h"
#include "InputFilter.h"
#include "BinaryData.h"

// ======================
// Constructeur / Timer
// ======================
PluginLookAndFeel::PluginLookAndFeel()
{
    setColourScheme(juce::LookAndFeel_V4::getDarkColourScheme());

    setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::Label::textColourId, PluginColours::onSurface);
    setColour(juce::Label::outlineColourId, PluginColours::divider);
    setColour(juce::ComboBox::textColourId, juce::Colours::transparentBlack);

    // === Chargement des polices Jost (fixes, toutes graisses) ===
    jostThin        = juce::Typeface::createSystemTypefaceFor(BinaryData::JostThin_ttf, BinaryData::JostThin_ttfSize);
    jostThinItalic  = juce::Typeface::createSystemTypefaceFor(BinaryData::JostThinItalic_ttf, BinaryData::JostThinItalic_ttfSize);
    jostExtraLight  = juce::Typeface::createSystemTypefaceFor(BinaryData::JostExtraLight_ttf, BinaryData::JostExtraLight_ttfSize);
    jostExtraLightItalic = juce::Typeface::createSystemTypefaceFor(BinaryData::JostExtraLightItalic_ttf, BinaryData::JostExtraLightItalic_ttfSize);
    jostLight       = juce::Typeface::createSystemTypefaceFor(BinaryData::JostLight_ttf, BinaryData::JostLight_ttfSize);
    jostLightItalic = juce::Typeface::createSystemTypefaceFor(BinaryData::JostLightItalic_ttf, BinaryData::JostLightItalic_ttfSize);
    jostRegular     = juce::Typeface::createSystemTypefaceFor(BinaryData::JostRegular_ttf, BinaryData::JostRegular_ttfSize);
    jostItalic      = juce::Typeface::createSystemTypefaceFor(BinaryData::JostItalic_ttf, BinaryData::JostItalic_ttfSize);
    jostMedium      = juce::Typeface::createSystemTypefaceFor(BinaryData::JostMedium_ttf, BinaryData::JostMedium_ttfSize);
    jostMediumItalic= juce::Typeface::createSystemTypefaceFor(BinaryData::JostMediumItalic_ttf, BinaryData::JostMediumItalic_ttfSize);
    jostSemiBold    = juce::Typeface::createSystemTypefaceFor(BinaryData::JostSemiBold_ttf, BinaryData::JostSemiBold_ttfSize);
    jostSemiBoldItalic = juce::Typeface::createSystemTypefaceFor(BinaryData::JostSemiBoldItalic_ttf, BinaryData::JostSemiBoldItalic_ttfSize);
    jostBold        = juce::Typeface::createSystemTypefaceFor(BinaryData::JostBold_ttf, BinaryData::JostBold_ttfSize);
    jostBoldItalic  = juce::Typeface::createSystemTypefaceFor(BinaryData::JostBoldItalic_ttf, BinaryData::JostBoldItalic_ttfSize);
    jostExtraBold   = juce::Typeface::createSystemTypefaceFor(BinaryData::JostExtraBold_ttf, BinaryData::JostExtraBold_ttfSize);
    jostExtraBoldItalic = juce::Typeface::createSystemTypefaceFor(BinaryData::JostExtraBoldItalic_ttf, BinaryData::JostExtraBoldItalic_ttfSize);
    jostBlack       = juce::Typeface::createSystemTypefaceFor(BinaryData::JostBlack_ttf, BinaryData::JostBlack_ttfSize);
    jostBlackItalic = juce::Typeface::createSystemTypefaceFor(BinaryData::JostBlackItalic_ttf, BinaryData::JostBlackItalic_ttfSize);

    startTimerHz(30); // Animation du dégradé
}

PluginLookAndFeel::~PluginLookAndFeel()
{
    stopTimer();
}

// ======================
// getJostFont centralisé
// ======================
juce::Font PluginLookAndFeel::getJostFont(float size, JostWeight weight, bool italic)
{
    juce::Typeface::Ptr tf;

    switch (weight)
    {
        case JostWeight::Thin:        tf = italic ? jostThinItalic : jostThin; break;
        case JostWeight::ExtraLight:  tf = italic ? jostExtraLightItalic : jostExtraLight; break;
        case JostWeight::Light:       tf = italic ? jostLightItalic : jostLight; break;
        case JostWeight::Regular:     tf = italic ? jostItalic : jostRegular; break;
        case JostWeight::Medium:      tf = italic ? jostMediumItalic : jostMedium; break;
        case JostWeight::SemiBold:    tf = italic ? jostSemiBoldItalic : jostSemiBold; break;
        case JostWeight::Bold:        tf = italic ? jostBoldItalic : jostBold; break;
        case JostWeight::ExtraBold:   tf = italic ? jostExtraBoldItalic : jostExtraBold; break;
        case JostWeight::Black:       tf = italic ? jostBlackItalic : jostBlack; break;
    }

    juce::Font font(tf);
    font.setHeight(size);
    return font;
}

// ======================
// Cutom Button
// ======================

void PluginLookAndFeel::drawCustomButtonBackground(juce::Graphics& g, juce::Component& button,
                                                   bool isActive, bool isHighlighted, bool isDown,
                                                   float cornerSize)
{
    auto bounds = button.getLocalBounds().toFloat();

    // Couleur dynamique
    juce::Colour baseColour;
    if (auto* lf = dynamic_cast<PluginLookAndFeel*>(&button.getLookAndFeel()))
    {
        auto window = button.getTopLevelComponent();
        auto totalHeight = window ? window->getHeight() : 1;
        auto globalY = button.getScreenBounds().getCentreY();
        baseColour = isActive ? lf->getPrimaryColour((float)globalY, (float)totalHeight).withAlpha(0.85f)
                              : PluginColours::surface;
    }
    else
    {
        baseColour = isActive ? PluginColours::primary.withAlpha(0.85f)
                              : PluginColours::surface;
    }

    if (isDown)
        baseColour = baseColour.darker(0.2f);
    else if (isHighlighted)
        baseColour = baseColour.brighter(0.1f);

    // Ombre portée
    g.setColour(juce::Colours::black.withAlpha(0.25f));
    g.fillRoundedRectangle(bounds.translated(0, 1.5f), cornerSize);

    // Fond principal
    g.setColour(baseColour);
    g.fillRoundedRectangle(bounds, cornerSize);

    // Ombre interne haute (désactivé)
    if (!isActive)
    {
        juce::ColourGradient innerShadow(juce::Colours::black.withAlpha(0.15f),
                                         bounds.getCentreX(), bounds.getY(),
                                         juce::Colours::transparentBlack,
                                         bounds.getCentreX(), bounds.getBottom(), false);
        g.setGradientFill(innerShadow);
        g.fillRoundedRectangle(bounds, cornerSize);
    }

    // Lueur interne haute (actif)
    if (isActive)
    {
        const float glowHeight = bounds.getHeight() * 0.2f;
        juce::ColourGradient innerGlow(juce::Colours::white.withAlpha(0.12f),
                                       bounds.getCentreX(), bounds.getY(),
                                       juce::Colours::transparentBlack,
                                       bounds.getCentreX(), bounds.getY() + glowHeight,
                                       false);
        g.setGradientFill(innerGlow);
        g.fillRoundedRectangle(bounds, cornerSize);
    }
}

// ======================
// Animation du dégradé
// ======================
void PluginLookAndFeel::timerCallback()
{
    animationOffset += 0.002f;
    if (animationOffset > 1.0f)
        animationOffset -= 1.0f;
}

// ======================
// Couleur "primary" animée avec gradient spatial
// ======================
juce::Colour PluginLookAndFeel::getPrimaryColour(float globalY, float totalHeight) const
{
    float pos = juce::jlimit(0.0f, 1.0f, globalY / totalHeight);

    // Gradient spatial
    juce::Colour base = PluginColours::primary;
    juce::Colour topColour    = base.darker(0.3f).withRotatedHue(-0.05f);
    juce::Colour bottomColour = base.brighter(0.4f).withRotatedHue(0.04f);
    juce::Colour spatialColour = topColour.interpolatedWith(bottomColour, pos);

    // Animation organique
    float wave = std::sin((pos * 3.0f + animationOffset * juce::MathConstants<float>::twoPi) * 0.7f);
    float hueShift = 0.02f * wave;
    float satMod   = 0.05f * wave;

    return spatialColour.withRotatedHue(hueShift).withSaturation(spatialColour.getSaturation() + satMod);
}

// ======================
// Redirection
// ======================
void PluginLookAndFeel::drawToggleButton(juce::Graphics& g, juce::ToggleButton& button,
                                         bool isMouseOverButton, bool isButtonDown)
{
    const auto name = button.getName();

    if (name == "muteButton" || name == "stepFilterToggle")
    {
        drawRoundedToggleButton(g, button, isMouseOverButton, isButtonDown);
    }
    else if (name == "monitorFooter") // <-- Détection de tes boutons en bas
    {
        // Utilise la version carrée mais avec une police plus petite
        drawSquareToggleButton(g, button, isMouseOverButton, isButtonDown, 16.0f);
    }
    else
    {
        drawSquareToggleButton(g, button, isMouseOverButton, isButtonDown);
    }
}

// ======================
// Bouton arrondi
// ======================
void PluginLookAndFeel::drawRoundedToggleButton(juce::Graphics& g, juce::ToggleButton& button,
                                                bool isMouseOver, bool isButtonDown)
{
    auto bounds = button.getLocalBounds().toFloat();
    float cornerSize = 6.0f;

    auto window = button.getTopLevelComponent();
    auto totalHeight = window ? window->getHeight() : 1;
    auto globalY = button.getScreenBounds().getCentreY();

    // === Ombre portée (tous états) ===
    g.setColour(juce::Colours::black.withAlpha(0.25f));
    g.fillRoundedRectangle(bounds.translated(0, 1.5f), cornerSize);

    // === Volume de base ===
    juce::Colour fill = button.getToggleState()
                        ? getPrimaryColour((float)globalY, (float)totalHeight).withAlpha(0.85f)
                        : isButtonDown  ? PluginColours::pressed
                        : isMouseOver   ? PluginColours::hover
                                        : PluginColours::surface;
    g.setColour(fill);
    g.fillRoundedRectangle(bounds, cornerSize);

    // === Ombre interne haute (désactivé) ===
    if (!button.getToggleState())
    {
        juce::ColourGradient innerShadow(juce::Colours::black.withAlpha(0.15f),
                                         bounds.getCentreX(), bounds.getY(),
                                         juce::Colours::transparentBlack,
                                         bounds.getCentreX(), bounds.getBottom(), false);
        g.setGradientFill(innerShadow);
        g.fillRoundedRectangle(bounds, cornerSize);
    }

    // === Lueur interne haute (actif) ===
    if (button.getToggleState())
    {
        const float glowHeight = bounds.getHeight() * 0.2f; // 20% de la hauteur
        juce::ColourGradient innerGlow(
            juce::Colours::white.withAlpha(0.12f),           // couleur de départ
            bounds.getCentreX(), bounds.getY(),              // point haut
            juce::Colours::transparentBlack,                 // couleur de fin
            bounds.getCentreX(), bounds.getY() + glowHeight, // fin du dégradé limitée
            false);
        g.setGradientFill(innerGlow);
        g.fillRoundedRectangle(bounds, cornerSize);
    }

    // === Contour sur hover ===
    if (isMouseOver)
    {
        g.setColour(juce::Colours::white.withAlpha(0.1f));
        g.drawRoundedRectangle(bounds, cornerSize, 1.0f);
    }

    // === Texte ===
    g.setColour(PluginColours::onSurface);
    g.setFont(getJostFont(16.0f, JostWeight::Light));
    g.drawText(button.getButtonText(), bounds.toNearestInt(), juce::Justification::centred);
}

// ======================
// Bouton carré
// ======================
void PluginLookAndFeel::drawSquareToggleButton(juce::Graphics& g, juce::ToggleButton& button,
                                               bool isMouseOver, bool isButtonDown,
                                               float fontSize /* = 24.0f */)
{
    auto bounds = button.getLocalBounds().toFloat();

    auto window = button.getTopLevelComponent();
    auto totalHeight = window ? window->getHeight() : 1;
    auto globalY = button.getScreenBounds().getCentreY();

    // === Ombre portée (tous états) ===
    g.setColour(juce::Colours::black.withAlpha(0.25f));
    g.fillRect(bounds.translated(0, 1.5f));

    // === Volume de base ===
    juce::Colour fill = button.getToggleState()
                        ? getPrimaryColour((float)globalY, (float)totalHeight).withAlpha(0.85f)
                        : isButtonDown  ? PluginColours::pressed
                        : isMouseOver   ? PluginColours::hover
                                        : PluginColours::surface;
    g.setColour(fill);
    g.fillRect(bounds);

    // === Ombre interne haute (désactivé) ===
    if (!button.getToggleState())
    {
        juce::ColourGradient innerShadow(juce::Colours::black.withAlpha(0.15f),
                                         bounds.getCentreX(), bounds.getY(),
                                         juce::Colours::transparentBlack,
                                         bounds.getCentreX(), bounds.getBottom(), false);
        g.setGradientFill(innerShadow);
        g.fillRect(bounds);
    }

    // === Lueur interne haute (actif) ===
    if (button.getToggleState())
    {
        const float glowHeight = bounds.getHeight() * 0.2f;
        juce::ColourGradient innerGlow(
            juce::Colours::white.withAlpha(0.12f),
            bounds.getCentreX(), bounds.getY(),
            juce::Colours::transparentBlack,
            bounds.getCentreX(), bounds.getY() + glowHeight,
            false);
        g.setGradientFill(innerGlow);
        g.fillRect(bounds);
    }

    // === Contour sur hover (haut/bas uniquement) ===
    if (isMouseOver)
    {
        g.setColour(juce::Colours::white.withAlpha(0.1f));
        g.drawLine(bounds.getX(), bounds.getY(), bounds.getRight(), bounds.getY(), 1.0f);
        g.drawLine(bounds.getX(), bounds.getBottom(), bounds.getRight(), bounds.getBottom(), 1.0f);
    }

    // === Texte ===
    g.setColour(PluginColours::onSurface);
    g.setFont(getJostFont(fontSize, JostWeight::ExtraLight));
    g.drawText(button.getButtonText(), bounds.toNearestInt(), juce::Justification::centred);
}

//==============================================================================
// Poignee Sliders
//==============================================================================
void PluginLookAndFeel::drawLinearSliderThumb(juce::Graphics& g, int x, int y, int width, int height,
                                              float sliderPos, float, float,
                                              const juce::Slider::SliderStyle style, juce::Slider& slider)
{
    const float thumbWidth  = 10.0f;
    const float thumbHeight = 14.0f;
    
    const float thumbX = slider.isHorizontal() ? sliderPos - thumbWidth * 0.5f
                                               : x + width * 0.5f - thumbWidth * 0.5f;
    const float thumbY = slider.isHorizontal() ? y + height * 0.5f - thumbHeight * 0.5f
                                               : sliderPos - thumbHeight * 0.5f;

    juce::Rectangle<float> thumbBounds(thumbX, thumbY, thumbWidth, thumbHeight);

    // Ombre portée
    g.setColour(juce::Colours::black.withAlpha(0.25f));
    g.fillRoundedRectangle(thumbBounds.translated(0, 1.5f), 3.0f);

    // Corps principal
    auto bodyColour = slider.isEnabled() ? juce::Colours::white.withAlpha(0.25f)
                                         : juce::Colours::white.withAlpha(0.12f);
    g.setColour(bodyColour);
    g.fillRoundedRectangle(thumbBounds, 3.0f);

    // Contour interne clair
    g.setColour(slider.isEnabled() ? juce::Colours::white.withAlpha(0.4f)
                                   : juce::Colours::white.withAlpha(0.15f));
    g.drawRoundedRectangle(thumbBounds.reduced(0.5f), 3.0f, 1.0f);

    // Barre centrale (verticale)
    juce::Colour primary = slider.isEnabled() ? PluginColours::primary : PluginColours::disabled;
    if (slider.isEnabled())
    {
        if (auto* lf = dynamic_cast<PluginLookAndFeel*>(&slider.getLookAndFeel()))
        {
            auto window = slider.getTopLevelComponent();
            auto totalHeight = window ? window->getHeight() : 1;
            auto globalY = slider.getScreenBounds().getCentreY();
            primary = lf->getPrimaryColour((float)globalY, (float)totalHeight);
        }
    }

    g.setColour(primary);

    // Trait central vertical ajusté
    const float barWidth  = thumbWidth * 0.15f;
    const float barHeight = thumbHeight;
    const float barX = thumbBounds.getCentreX() - (barWidth * 0.5f);
    const float barY = thumbBounds.getY();
    g.fillRoundedRectangle(barX, barY, barWidth, barHeight, barWidth * 0.5f);
}

//==============================================================================
// Sliders
//==============================================================================
void PluginLookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
                                         float sliderPos, float minSliderPos, float maxSliderPos,
                                         const juce::Slider::SliderStyle style, juce::Slider& slider)
{
    const float trackHeight = 4.0f;
    const float trackY = y + height * 0.5f - trackHeight * 0.5f;

    // Couleur de la piste
    auto trackColour = slider.isEnabled() ? PluginColours::divider
                                          : PluginColours::divider.withAlpha(0.3f);
    g.setColour(trackColour);
    g.fillRect((float)x, trackY, (float)width, trackHeight);

    // Couleur active
    auto window = slider.getTopLevelComponent();
    auto totalHeight = window ? window->getHeight() : 1;
    auto globalY = slider.getScreenBounds().getCentreY();
    juce::Colour primary = slider.isEnabled() ? getPrimaryColour((float)globalY, (float)totalHeight)
                                              : PluginColours::disabled;

    if (style == juce::Slider::TwoValueHorizontal)
    {
        auto minPos = slider.getPositionOfValue(slider.getMinValue());
        auto maxPos = slider.getPositionOfValue(slider.getMaxValue());

        // Remplissage actif
        g.setColour(primary);
        g.fillRect(minPos, trackY, maxPos - minPos, trackHeight);

        // Poignées double
        drawLinearSliderThumb(g, x, y, width, height, minPos, minSliderPos, maxSliderPos, style, slider);
        drawLinearSliderThumb(g, x, y, width, height, maxPos, minSliderPos, maxSliderPos, style, slider);
        return;
    }

    // Remplissage actif simple
    g.setColour(primary);
    g.fillRect((float)x, trackY, sliderPos - (float)x, trackHeight);

    // Poignée simple
    drawLinearSliderThumb(g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
}

//==============================================================================
// Rotary
//==============================================================================
void PluginLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                                         float sliderPosProportional, float rotaryStartAngle, float rotaryEndAngle,
                                         juce::Slider& slider)
{
    auto radius = juce::jmin(width / 2, height / 2) - 4.0f;
    auto centreX = x + width * 0.5f;
    auto centreY = y + height * 0.5f;
    auto toAngle = rotaryStartAngle + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);

    g.setColour(PluginColours::divider);
    g.drawEllipse(centreX - radius, centreY - radius, radius * 2, radius * 2, 2.0f);

    auto window = slider.getTopLevelComponent();
    auto totalHeight = window ? window->getHeight() : 1;
    auto globalY = slider.getScreenBounds().getCentreY();

    g.setColour(getPrimaryColour((float)globalY, (float)totalHeight));
    juce::Path valueArc;
    valueArc.addCentredArc(centreX, centreY, radius, radius, 0.0f,
                           rotaryStartAngle, toAngle, true);
    g.strokePath(valueArc, juce::PathStrokeType(3.0f));

    auto thumbX = centreX + radius * std::cos(toAngle - juce::MathConstants<float>::halfPi);
    auto thumbY = centreY + radius * std::sin(toAngle - juce::MathConstants<float>::halfPi);
    g.fillEllipse(thumbX - 4.0f, thumbY - 4.0f, 8.0f, 8.0f);
}

// ======================
// Labels
// ======================
void PluginLookAndFeel::drawLabel(juce::Graphics& g, juce::Label& label)
{
    auto bounds = label.getLocalBounds();

    g.setColour(juce::Colours::transparentBlack);
    g.fillRect(bounds);

    g.setFont(getLabelFont(label));

    auto id = label.getName();
    auto window = label.getTopLevelComponent();
    auto totalHeight = window ? window->getHeight() : 1;
    auto globalY = label.getScreenBounds().getCentreY();

    if (id == "value_note" || id == "value_num_min" || id == "value_num_max"
        || id == "value_ratio" || id == "value_simple")
    {
        g.setColour(label.isEnabled()
                        ? getPrimaryColour((float)globalY, (float)totalHeight)
                        : PluginColours::disabled);
    }
    else if (id == "ccStatus")
        g.setColour(PluginColours::onSurface);
    else
        g.setColour(label.isEnabled() ? PluginColours::onSurface : PluginColours::disabled);

    g.drawFittedText(label.getText(), bounds.reduced(2), label.getJustificationType(), 1);
}

// ======================
// Combo Box
// ======================
void PluginLookAndFeel::drawComboBox(juce::Graphics& g, int width, int height,
                                     bool isButtonDown, int, int, int, int,
                                     juce::ComboBox& box)
{
    const float arrowButtonWidth = 12.0f;

    auto textArea  = juce::Rectangle<float>(0, 0, (float)width - arrowButtonWidth, (float)height);
    auto arrowArea = juce::Rectangle<float>((float)width - arrowButtonWidth, 0, arrowButtonWidth, (float)height);

    // --- Ombre basse ---
    g.setColour(juce::Colours::black.withAlpha(0.25f));
    g.fillRect(0.0f, (float)height - 1.5f, (float)width, 1.5f);

    // --- Fond principal ---
    g.setColour(box.isMouseOver(true) ? PluginColours::surface.brighter(0.05f)
                                      : PluginColours::surface);
    g.fillRect(textArea);
    g.fillRect(arrowArea);

    // --- Ombre interne ---
    if (!box.hasKeyboardFocus(true))
    {
        juce::ColourGradient innerShadow(juce::Colours::black.withAlpha(0.12f),
                                         textArea.getCentreX(), textArea.getY(),
                                         juce::Colours::transparentBlack,
                                         textArea.getCentreX(), textArea.getBottom(), false);
        g.setGradientFill(innerShadow);
        g.fillRect(textArea);
    }

    // --- Lueur focus ---
    if (box.hasKeyboardFocus(true))
    {
        const float glowHeight = textArea.getHeight() * 0.2f;
        juce::ColourGradient innerGlow(juce::Colours::white.withAlpha(0.12f),
                                       textArea.getCentreX(), textArea.getY(),
                                       juce::Colours::transparentBlack,
                                       textArea.getCentreX(), textArea.getY() + glowHeight, false);
        g.setGradientFill(innerGlow);
        g.fillRect(textArea);
    }

    // --- Zone flèche ---
    g.setColour(box.isMouseOver(true) ? PluginColours::surface.brighter(0.05f)
                                      : PluginColours::surface.darker(0.1f));
    g.fillRect(arrowArea);

    // --- Flèche ---
    const float arrowSize = 4.0f;
    juce::Path path;
    path.startNewSubPath(arrowArea.getCentreX() - arrowSize * 0.5f, arrowArea.getCentreY() - 2.0f);
    path.lineTo(arrowArea.getCentreX(), arrowArea.getCentreY() + 2.0f);
    path.lineTo(arrowArea.getCentreX() + arrowSize * 0.5f, arrowArea.getCentreY() - 2.0f);
    g.setColour(PluginColours::onSurface);
    g.strokePath(path, juce::PathStrokeType(1.5f));

    // --- Texte sans padding ---
    g.setColour(box.findColour (juce::ComboBox::textColourId).withAlpha(1.0f));
    g.setFont(getJostFont (14.0f, JostWeight::Regular, false));
    g.drawText(box.getText(), textArea.toNearestInt(), juce::Justification::centredLeft, true);
}


// ======================
// Pop up
// ======================
void PluginLookAndFeel::drawPopupMenuBackground(juce::Graphics& g, int width, int height)
{
    // Ombre légère autour du menu
    g.setColour(juce::Colours::black.withAlpha(0.3f));
    g.fillRect(0, 0, width, height); // angles droits
    
    // Fond principal
    g.setColour(PluginColours::surface);
    g.fillRect(0, 0, width, height); // angles droits
}

// ======================
// Menu Item
// ======================
void PluginLookAndFeel::drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area,
                                          bool isSeparator, bool isActive, bool isHighlighted,
                                          bool isTicked, bool hasSubMenu, const juce::String& text,
                                          const juce::String& shortcutKeyText,
                                          const juce::Drawable* icon, const juce::Colour* textColourToUse)
{
    if (isSeparator)
    {
        g.setColour(PluginColours::divider);
        g.drawLine((float)area.getX(), (float)area.getCentreY(), (float)area.getRight(), (float)area.getCentreY());
        return;
    }

    auto highlight = isHighlighted ? PluginColours::primary.withAlpha(0.15f) : juce::Colours::transparentBlack;
    g.setColour(highlight);
    g.fillRect(area);

    auto textColour = textColourToUse != nullptr ? *textColourToUse
                                                 : (isActive ? PluginColours::onSurface
                                                             : PluginColours::disabled);

    g.setColour(textColour);
    g.setFont(getJostFont(14.0f, JostWeight::Regular));
    g.drawText(text, area.reduced(4), juce::Justification::centredLeft);

    if (isTicked)
    {
        const float dotSize = 6.0f;

        // Zone à droite du menu
        float dotX = (float)area.getRight() - 12.0f;
        float dotY = (float)area.getCentreY() - dotSize * 0.5f;

        // Ombre interne (cercle un peu plus grand, semi-transparent)
        g.setColour(juce::Colours::black.withAlpha(0.25f));
        g.fillEllipse(dotX - 1.0f, dotY - 1.0f, dotSize + 2.0f, dotSize + 2.0f);

        // Point principal (couleur primaire)
        g.setColour(PluginColours::primary);
        g.fillEllipse(dotX, dotY, dotSize, dotSize);
    }
    
    if (!shortcutKeyText.isEmpty())
    {
        g.setFont(getJostFont(12.0f, JostWeight::Thin));
        g.drawText(shortcutKeyText, area.reduced(4), juce::Justification::centredRight);
    }
}

// ======================
// Affichage Montitor
// ======================
void PluginLookAndFeel::drawMidiMonitor(juce::Graphics& g,
                                        juce::Rectangle<int> area,
                                        const juce::StringArray& messages)
{
    // Fond uniforme
    g.fillAll(PluginColours::surface);

    // Texte
    g.setColour(PluginColours::onSurface);
    g.setFont(getJostFont(13.0f, JostWeight::Light, false));

    const int lineHeight = 14;
    const int maxLines = area.getHeight() / lineHeight;
    const int startIdx = juce::jmax(0, messages.size() - maxLines);

    int y = area.getY();
    for (int i = startIdx; i < messages.size(); ++i)
    {
        g.drawText(messages[i], area.getX(), y, area.getWidth(), lineHeight,
                   juce::Justification::left);
        y += lineHeight;
    }
}

// ======================
// Custom Two Line Toggle
// ======================
void PluginLookAndFeel::drawTwoLineToggleButton(juce::Graphics& g,
                                                juce::ToggleButton& button,
                                                bool isHighlighted, bool isDown,
                                                const juce::String& topText,
                                                const juce::String& bottomText)
{
    drawCustomButtonBackground(g, button, button.getToggleState(), isHighlighted, isDown, 4.0f);

    auto bounds = button.getLocalBounds().toFloat();
    const int halfHeight = bounds.getHeight() / 2;
    const int gap = -4;

    g.setColour(button.findColour(button.getToggleState()
                                  ? juce::TextButton::textColourOnId
                                  : juce::TextButton::textColourOffId));
    g.setFont(getJostFont(14.0f, JostWeight::Light, false)); // ou Light si tu veux

    g.drawText(topText, 0, 0, (int)bounds.getWidth(), halfHeight - gap,
               juce::Justification::centred);
    g.drawText(bottomText, 0, halfHeight + gap, (int)bounds.getWidth(), halfHeight - gap,
               juce::Justification::centred);
}

juce::Font PluginLookAndFeel::getLabelFont(juce::Label& label)
{
    bool isItalic = label.getName().containsIgnoreCase("italic");
    bool isBold   = label.getName().containsIgnoreCase("bold");

    auto weight = isBold ? JostWeight::Bold : JostWeight::Regular;
    return getJostFont(13.0f, weight, isItalic);
}
