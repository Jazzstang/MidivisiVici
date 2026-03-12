// PluginLookAndFeel.cpp
#include "PluginLookAndFeel.h"
#include "MainMenu.h"
#include "BinaryData.h"
#include <cmath>

// ========================================================================
// Utilitaire : Pas de majuscules sauf exceptions (CC, PC, notes MIDI)
// ========================================================================
juce::String PluginLookAndFeel::normaliseText(const juce::String& text)
{
    if (text.isEmpty())
        return text;

    if (text.equalsIgnoreCase("CC") || text.equalsIgnoreCase("PC"))
        return text.toUpperCase();

    if (text.length() <= 3 && juce::CharacterFunctions::isLetter(text[0]))
        return text;

    return text.toLowerCase();
}

// ========================================================================
// Rectangle avec coins arrondis sélectifs
// ========================================================================
static void addSelectiveRoundedRect(juce::Path& path,
                                    juce::Rectangle<float> area,
                                    float radius,
                                    bool topLeft, bool topRight,
                                    bool bottomLeft, bool bottomRight)
{
    const float x = area.getX();
    const float y = area.getY();
    const float w = area.getWidth();
    const float h = area.getHeight();
    const float rMax = juce::jmin(w, h) * 0.5f;
    const float r    = (radius < 0.0f) ? rMax : juce::jlimit(0.0f, rMax, radius);

    path.startNewSubPath(x + (topLeft ? r : 0), y);

    if (topRight)
    {
        path.lineTo(x + w - r, y);
        path.quadraticTo(x + w, y, x + w, y + r);
    }
    else
        path.lineTo(x + w, y);

    if (bottomRight)
    {
        path.lineTo(x + w, y + h - r);
        path.quadraticTo(x + w, y + h, x + w - r, y + h);
    }
    else
        path.lineTo(x + w, y + h);

    if (bottomLeft)
    {
        path.lineTo(x + r, y + h);
        path.quadraticTo(x, y + h, x, y + h - r);
    }
    else
        path.lineTo(x, y + h);

    if (topLeft)
    {
        path.lineTo(x, y + r);
        path.quadraticTo(x, y, x + r, y);
    }
    else
        path.lineTo(x, y);

    path.closeSubPath();
}

std::unique_ptr<juce::Drawable> PluginLookAndFeel::createTintedIcon(const void* data,
                                                                     int dataSize,
                                                                     juce::Colour colour)
{
    auto icon = juce::Drawable::createFromImageData(data, dataSize);
    if (icon == nullptr)
        return nullptr;

    icon->replaceColour(juce::Colours::black, colour);
    return icon;
}

void PluginLookAndFeel::loadCachedIconSet(CachedIconSet& target, const void* data, int dataSize)
{
    target.variants[(size_t) CachedIconTone::Primary] =
        createTintedIcon(data, dataSize, PluginColours::mainMenuPrimary);
    target.variants[(size_t) CachedIconTone::OnBackground] =
        createTintedIcon(data, dataSize, PluginColours::onBackground);
    target.variants[(size_t) CachedIconTone::Background] =
        createTintedIcon(data, dataSize, PluginColours::background);
    target.variants[(size_t) CachedIconTone::Disabled] =
        createTintedIcon(data, dataSize, PluginColours::onDisabled);
}

const juce::Drawable* PluginLookAndFeel::getCachedIconVariant(const CachedIconSet& set,
                                                               CachedIconTone tone) noexcept
{
    return set.variants[(size_t) tone].get();
}

float PluginLookAndFeel::resolveToggleFontSize(juce::ToggleButton& button)
{
    static const juce::Identifier kLargeTitleId { "lfLargeTitle" };
    auto& props = button.getProperties();

    if (props.contains(kLargeTitleId))
        return (bool) props[kLargeTitleId] ? 24.0f : 16.0f;

    const auto text = button.getButtonText();
    const bool isLarge =
        text.containsIgnoreCase("Input Filter")
        || text.containsIgnoreCase("Input Monitor")
        || text.containsIgnoreCase("Output Monitor")
        || text.containsIgnoreCase("Transform")
        || text.containsIgnoreCase("Divisi")
        || text.containsIgnoreCase("Harmonizer")
        || text.containsIgnoreCase("Arpeggiator")
        || text.containsIgnoreCase("range splitter")
        || text.containsIgnoreCase("round robin");

    props.set(kLargeTitleId, isLarge);
    return isLarge ? 24.0f : 16.0f;
}

// ========================================================================
// Constructeur
// ========================================================================
PluginLookAndFeel::PluginLookAndFeel()
{
    setColourScheme(juce::LookAndFeel_V4::getDarkColourScheme());

    setColour(juce::Label::backgroundColourId, PluginColours::surface);
    setColour(juce::Label::textColourId, PluginColours::onSurface);
    setColour(juce::Label::outlineColourId, PluginColours::divider);
    setColour(juce::ComboBox::textColourId, PluginColours::onSurface);
    // Default slider palette so controls keep a visible lane background.
    setColour(juce::Slider::backgroundColourId, PluginColours::background);
    setColour(juce::Slider::trackColourId, PluginColours::primary);
    setColour(juce::Slider::thumbColourId, PluginColours::onPrimary);

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

    // Cache tinted icon variants once to avoid SVG parse/copy during paint.
    loadCachedIconSet(saveIconSet, BinaryData::save_svg, BinaryData::save_svgSize);
    loadCachedIconSet(copyIconSet, BinaryData::copy_svg, BinaryData::copy_svgSize);
    loadCachedIconSet(pasteIconSet, BinaryData::paste_svg, BinaryData::paste_svgSize);
    loadCachedIconSet(deleteIconSet, BinaryData::delete_svg, BinaryData::delete_svgSize);
}

// ========================================================================
// States bouton InputMonitor
// ========================================================================
PluginLookAndFeel::CornerRadii PluginLookAndFeel::getCornerRadiiForButton(const juce::String& buttonId) const
{
    CornerRadii c;

    // Rayon dynamique basé sur la taille → pilule complète
    // (on récupère la taille directement au moment du dessin dans drawSquareToggleButton)
    // Ici, on ne connaît pas les bounds du bouton, donc on laisse radius = -1 pour indiquer "auto"
    c.radius = -1.0f;

    if (buttonId == "IM_Notes")
    {
        c.topLeft = c.bottomLeft = true;
        c.topRight = c.bottomRight = !imControls;
    }
    else if (buttonId == "IM_Controls")
    {
        c.topLeft  = c.bottomLeft  = !imNotes;
        c.topRight = c.bottomRight = !imClock;
    }
    else if (buttonId == "IM_Clock")
    {
        c.topLeft  = c.bottomLeft  = !imControls;
        c.topRight = c.bottomRight = !imEvents;
    }
    else if (buttonId == "IM_Event")
    {
        c.topLeft  = c.bottomLeft  = !imClock;
        c.topRight = c.bottomRight = true;
    }
    else if (buttonId == "OM_Notes")
    {
        c.topLeft = c.bottomLeft = true;
        c.topRight = c.bottomRight = !omControls;
    }
    else if (buttonId == "OM_Controls")
    {
        c.topLeft  = c.bottomLeft  = !omNotes;
        c.topRight = c.bottomRight = !omClock;
    }
    else if (buttonId == "OM_Clock")
    {
        c.topLeft  = c.bottomLeft  = !omControls;
        c.topRight = c.bottomRight = !omEvents;
    }
    else if (buttonId == "OM_Event")
    {
        c.topLeft  = c.bottomLeft  = !omClock;
        c.topRight = c.bottomRight = true;
    }
    else
    {
        c.topLeft = c.topRight = c.bottomLeft = c.bottomRight = true;
    }

    return c;
}

void PluginLookAndFeel::setInputMonitorStates(bool notes, bool controls, bool clock, bool events)
{
    imNotes = notes; imControls = controls;
    imClock = clock; imEvents = events;
}

void PluginLookAndFeel::setOutputMonitorStates(bool notes, bool controls, bool clock, bool events)
{
    omNotes = notes; omControls = controls;
    omClock = clock; omEvents = events;
}

// ========================================================================
// Toggle Buttons
// ========================================================================
void PluginLookAndFeel::drawToggleButton(juce::Graphics& g, juce::ToggleButton& button,
                                         bool isMouseOver, bool isButtonDown)
{
    const auto id = button.getComponentID();
    const bool isMainMenu = (getContextStyle() == ComponentStyle::MainMenu);

    // === 1. Bouton Sauvegarde avec icône ===
    if (id == "MM_Toggle_Save")
    {
        drawIconToggleButton(g, button, isMouseOver, isButtonDown);
        return;
    }

    // === 2. Checkbox WhiteKey (forme circulaire) ===
    if (id.startsWithIgnoreCase("MM_Toggle_WhiteKey"))
    {
        auto r = button.getLocalBounds().toFloat();

        // Cercle
        const float boxSize = r.getHeight();
        auto circleArea = juce::Rectangle<float>(r.getX(), r.getY(), boxSize, boxSize);

        juce::Colour base = isMainMenu ? PluginColours::mainMenuSurface : PluginColours::surface;
        if (isButtonDown)     base = base.darker(0.20f);
        else if (isMouseOver) base = base.brighter(0.10f);

        g.setColour(base);
        g.fillEllipse(circleArea);

        // Dot central si actif
        if (button.getToggleState())
        {
            const float dot = boxSize * 0.36f;
            auto dotArea = circleArea.withSizeKeepingCentre(dot, dot);
            g.setColour(button.isEnabled()
                            ? (isMainMenu ? PluginColours::onBackground : PluginColours::onSurface)
                            : PluginColours::onDisabled);
            g.fillEllipse(dotArea);
        }

        // Texte à droite
        auto labelArea = r.withX(circleArea.getRight() + 8.0f).toNearestInt();
        g.setFont(getJostFont(12.0f, JostWeight::Light));

        juce::Colour textColour = PluginColours::onSurface;
        if (!button.isEnabled())  textColour = PluginColours::onDisabled;
        else if (isMainMenu)      textColour = PluginColours::onBackground;

        g.setColour(textColour);
        g.drawFittedText(normaliseText(button.getButtonText()),
                         labelArea, juce::Justification::centredLeft, 2);
        return;
    }

    // === 2.b Compact tick boxes used by InputFilter routing row ===
    if (id.startsWith("IF_Tick_"))
    {
        auto bounds = button.getLocalBounds().toFloat().reduced(1.0f);
        const float side = juce::jmin(bounds.getWidth(), bounds.getHeight());
        auto box = bounds.withSizeKeepingCentre(side, side);

        juce::Colour fill = button.getToggleState() ? PluginColours::primary : PluginColours::secondary;
        juce::Colour stroke = button.getToggleState() ? PluginColours::primary : PluginColours::onSurface;
        juce::Colour tick = PluginColours::onPrimary;

        if (!button.isEnabled())
        {
            fill = PluginColours::disabled;
            stroke = PluginColours::onDisabled;
            tick = PluginColours::onDisabled;
        }
        else if (isButtonDown)
        {
            fill = fill.darker(0.2f);
        }
        else if (isMouseOver)
        {
            fill = fill.brighter(0.08f);
        }

        g.setColour(fill);
        g.fillRoundedRectangle(box, 3.0f);

        g.setColour(stroke);
        g.drawRoundedRectangle(box, 3.0f, 1.0f);

        if (button.getToggleState())
        {
            juce::Path mark;
            const auto x = box.getX();
            const auto y = box.getY();
            const auto w = box.getWidth();
            const auto h = box.getHeight();
            mark.startNewSubPath(x + 0.22f * w, y + 0.54f * h);
            mark.lineTo(x + 0.44f * w, y + 0.74f * h);
            mark.lineTo(x + 0.78f * w, y + 0.30f * h);
            g.setColour(tick);
            g.strokePath(mark, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        return;
    }

    // === 3. Tous les autres boutons (snapshot, mute, modules...) ===

    // Radius dynamique
    auto radii = getCornerRadiiForButton(id);

    // Radius spécial (bouton arrondi complet)
    const bool isMute = (id == "MM_Toggle_Mute");
    if (isMute || id == "stepFilterToggle")
        radii.radius = -1.0f;
    
    // Font dynamique : 24 si titre de module, sinon 16.
    // Cached in component properties to avoid repeated string scans in paint.
    const float fontSize = resolveToggleFontSize(button);

    // Dessin principal : fond + texte
    drawSquareToggleButton(g, button, isMouseOver, isButtonDown, radii, fontSize);
}

void PluginLookAndFeel::drawSquareToggleButton(juce::Graphics& g,
                                               juce::ToggleButton& button,
                                               bool isMouseOver, bool isButtonDown,
                                               const CornerRadii& corners,
                                               float fontSize)
{
    auto bounds = button.getLocalBounds().toFloat();

    const juce::String id = button.getComponentID();
    const bool isSnapshot   = id.startsWithIgnoreCase("MM_Toggle_Snapshot");
    const bool isMute       = id == "MM_Toggle_Mute";
    const bool isMainMenu   = (getContextStyle() == ComponentStyle::MainMenu);

    // === Détection filtre type IM_* ou OM_* ===
    const bool isFilterToggle =
        id.startsWith("IM_") || id.startsWith("OM_");

    const float effectiveRadius = corners.radius > 0.0f
        ? corners.radius
        : getDynamicCornerRadius(button.getLocalBounds());
    const bool allCornersRounded = corners.topLeft && corners.topRight
                                && corners.bottomLeft && corners.bottomRight;

    // === Couleur de fond ===
    juce::Colour baseColour;

    if (isFilterToggle && !button.getToggleState())
    {
        baseColour = PluginColours::secondary;
    }
    else if (isSnapshot)
    {
        baseColour = button.getToggleState()
            ? PluginColours::onBackground
            : PluginColours::mainMenuSurface;
    }
    else if (isMute)
    {
        baseColour = button.getToggleState()
            ? PluginColours::onBackground
            : PluginColours::mainMenuSurface;
    }
    else if (isMainMenu)
    {
        baseColour = button.getToggleState()
            ? PluginColours::mainMenuPrimary
            : PluginColours::mainMenuSurface;
    }
    else
    {
        baseColour = button.getToggleState()
            ? PluginColours::primary
            : PluginColours::secondary;
    }

    if (isButtonDown)
        baseColour = baseColour.darker(0.2f);
    else if (isMouseOver)
        baseColour = baseColour.brighter(0.1f);

    g.setColour(baseColour);
    if (allCornersRounded)
    {
        g.fillRoundedRectangle(bounds, effectiveRadius);
    }
    else
    {
        juce::Path p;
        addSelectiveRoundedRect(p, bounds, effectiveRadius,
                                corners.topLeft, corners.topRight,
                                corners.bottomLeft, corners.bottomRight);
        g.fillPath(p);
    }

    // === Couleur de texte ===
    juce::Colour textColour;

    if (!button.isEnabled())
    {
        textColour = PluginColours::onDisabled;
    }
    else if (isSnapshot)
    {
        textColour = button.getToggleState()
            ? PluginColours::background
            : PluginColours::onBackground;
    }
    else if (isMute)
    {
        textColour = button.getToggleState()
            ? PluginColours::background
            : PluginColours::onBackground;
    }
    else if (isMainMenu)
    {
        textColour = button.getToggleState()
            ? PluginColours::onBackground
            : PluginColours::onSurface;
    }
    else
    {
        textColour = button.getToggleState()
            ? PluginColours::onSurface
            : PluginColours::onDisabled;
    }

    g.setColour(textColour);

    // === Police ===
    if (fontSize <= 0.0f)
        fontSize = resolveToggleFontSize(button);

    g.setFont(getJostFont(fontSize, JostWeight::Light));
    g.drawText(normaliseText(button.getButtonText()),
               bounds.toNearestInt(),
               juce::Justification::centred);
}

void PluginLookAndFeel::drawCustomButtonBackground(juce::Graphics& g, juce::Component& button,
                                                   bool isActive, bool isHighlighted, bool isDown,
                                                   float cornerSize)
{
    auto bounds = button.getLocalBounds().toFloat();

    // Couleur de base
    bool isMainMenu = (getContextStyle() == ComponentStyle::MainMenu);

    juce::Colour baseColour;
    if (isMainMenu)
    {
        baseColour = isActive ? PluginColours::mainMenuPrimary
                              : PluginColours::mainMenuSurface;
    }
    else
    {
        baseColour = isActive ? PluginColours::primary
                              : PluginColours::secondary;
    }

    if (isDown)
        baseColour = baseColour.darker(0.2f);
    else if (isHighlighted)
        baseColour = baseColour.brighter(0.1f);

    g.setColour(baseColour);
    g.fillRoundedRectangle(bounds, cornerSize);
}

void PluginLookAndFeel::drawIconToggleButton(juce::Graphics& g,
                                             juce::ToggleButton& button,
                                             bool isMouseOver,
                                             bool isButtonDown)
{
    auto bounds = button.getLocalBounds().toFloat();

    // ==== Recuperation robuste de l'etat (parent = ShadowComponent) ====
    SaveState state = SaveState::Idle;
    if (auto* mm = button.findParentComponentOfClass<MainMenu>())
        state = mm->getSaveState();

    // ==== Couleurs spec ====
    juce::Colour baseColour;
    CachedIconTone iconTone = CachedIconTone::Primary;

    switch (state)
    {
        case SaveState::Idle:
            baseColour = PluginColours::mainMenuSurface;
            iconTone = CachedIconTone::Primary;
            break;

        case SaveState::Pending:
            baseColour = PluginColours::onBackground;
            iconTone = CachedIconTone::Background;
            break;

        case SaveState::Saving:
            baseColour = PluginColours::background;
            iconTone = CachedIconTone::OnBackground;
            break;
    }

    // ==== Feedback hover/down (optionnel mais agreable) ====
    if (button.isEnabled())
    {
        if (isButtonDown)      baseColour = baseColour.darker(0.20f);
        else if (isMouseOver)  baseColour = baseColour.brighter(0.10f);
    }
    else
    {
        // Un tout petit "fade" quand disabled (Idle/Saving)
        baseColour = PluginColours::disabled;
        iconTone = CachedIconTone::Disabled;
    }

    // ==== Fond ====
    const float r = 6.0f;
    g.setColour(baseColour);
    g.fillRoundedRectangle(bounds, r);

    // ==== Icone SVG ====
    if (const auto* icon = getCachedIconVariant(saveIconSet, iconTone))
    {
        const float iconSize = std::min(bounds.getWidth(), bounds.getHeight()) * 0.75f;
        auto iconBounds = bounds.withSizeKeepingCentre(iconSize, iconSize);
        icon->drawWithin(g, iconBounds, juce::RectanglePlacement::centred, 1.0f);
    }
}

void PluginLookAndFeel::drawDrawableButton(juce::Graphics& g,
                                           juce::DrawableButton& button,
                                           bool isMouseOver,
                                           bool isButtonDown)
{
    const auto id = button.getComponentID();

    const bool isMainMenuIconButton =
        (id == "MM_Button_Copy" || id == "MM_Button_Paste"
         || id == "MM_Button_Delete" || id == "MM_Button_Save");

    if (!isMainMenuIconButton)
    {
        juce::LookAndFeel_V4::drawDrawableButton(g, button, isMouseOver, isButtonDown);
        return;
    }

    auto bounds = button.getLocalBounds().toFloat();

    // ---- Etats (robustes meme hors MainMenu: editor top-actions)
    bool clipboardActive = button.getToggleState();
    SaveState saveState = SaveState::Idle;

    if (auto* mm = button.findParentComponentOfClass<MainMenu>())
    {
        clipboardActive = mm->isClipboardActive();
        saveState = mm->getSaveState();
    }
    else if (id == "MM_Button_Save" && button.isEnabled())
    {
        // Top action save: enabled <=> pending.
        saveState = SaveState::Pending;
    }

    const bool isPaste = (id == "MM_Button_Paste");
    const bool isCopy  = (id == "MM_Button_Copy");
    const bool isSave  = (id == "MM_Button_Save");

    // ---- Couleurs par defaut (comme Save Idle)
    juce::Colour baseColour = PluginColours::mainMenuSurface;
    CachedIconTone iconTone = CachedIconTone::Primary;

    if (isSave)
    {
        switch (saveState)
        {
            case SaveState::Idle:
                baseColour = PluginColours::mainMenuSurface;
                iconTone = CachedIconTone::Primary;
                break;
            case SaveState::Pending:
                baseColour = PluginColours::onBackground;
                iconTone = CachedIconTone::Background;
                break;
            case SaveState::Saving:
                baseColour = PluginColours::background;
                iconTone = CachedIconTone::OnBackground;
                break;
        }
    }
    else
    {
        // ---- Etat visuel actif pour copy/paste
        if (isCopy && clipboardActive)
        {
            baseColour = PluginColours::mainMenuSurface;
            iconTone = CachedIconTone::OnBackground;
        }

        if (isPaste && clipboardActive)
        {
            baseColour = PluginColours::onBackground;
            iconTone = CachedIconTone::Background;
        }
    }

    // ---- Hover / down / disabled
    if (button.isEnabled())
    {
        if (isButtonDown)      baseColour = baseColour.darker(0.20f);
        else if (isMouseOver)  baseColour = baseColour.brighter(0.10f);
    }
    else
    {
        baseColour = PluginColours::disabled;
        iconTone = CachedIconTone::Disabled;
    }

    // ---- Fond (round button)
    const float side = juce::jmin(bounds.getWidth(), bounds.getHeight());
    const auto circle = bounds.withSizeKeepingCentre(side, side);
    g.setColour(baseColour);
    g.fillEllipse(circle);

    // ---- Icone recoloree noir -> iconColour
    const CachedIconSet* iconSet = nullptr;
    if (id == "MM_Button_Copy")      iconSet = &copyIconSet;
    else if (id == "MM_Button_Paste") iconSet = &pasteIconSet;
    else if (id == "MM_Button_Delete") iconSet = &deleteIconSet;
    else if (id == "MM_Button_Save") iconSet = &saveIconSet;

    if (iconSet != nullptr)
    {
        if (const auto* icon = getCachedIconVariant(*iconSet, iconTone))
        {
            const float iconSize = side * 0.75f;
            auto iconBounds = circle.withSizeKeepingCentre(iconSize, iconSize);
            icon->drawWithin(g, iconBounds, juce::RectanglePlacement::centred, 1.0f);
            return;
        }
    }

    // Fallback if icon cache failed for any reason.
    if (auto* img = button.getNormalImage())
    {
        img->drawWithin(g,
                        circle.withSizeKeepingCentre(side * 0.75f,
                                                     side * 0.75f),
                        juce::RectanglePlacement::centred,
                        1.0f);
    }
}

//==============================================================================
// Poignée Sliders
//==============================================================================
namespace
{
constexpr const char* kLfoOffsetSliderId = "LFO_OffsetSlider";

bool isLfoOffsetSlider(const juce::Slider& slider) noexcept
{
    return slider.getComponentID() == kLfoOffsetSliderId;
}
}

void PluginLookAndFeel::drawLinearSliderThumb(juce::Graphics& g, int x, int y, int width, int height,
                                              float sliderPos, float, float,
                                              const juce::Slider::SliderStyle style, juce::Slider& slider)
{
    juce::ignoreUnused(style);

    const bool horizontal = slider.isHorizontal();
    const bool disabled = !slider.isEnabled();
    const bool isOffsetSlider = horizontal && isLfoOffsetSlider(slider);

    float thumbWidth = horizontal ? 11.0f : 14.0f;
    float thumbHeight = horizontal ? juce::jmin(11.0f, (float) height - 2.0f) : 12.0f;

    if (isOffsetSlider)
    {
        // Keep the thumb exactly 2x the slider lane thickness for Offset.
        const float laneHeight = juce::jmax(6.0f, (float) height * 0.26f);
        thumbHeight = juce::jlimit(8.0f, juce::jmax(8.0f, (float) height - 2.0f), laneHeight * 2.0f);
        const float maxThumbWidth = juce::jmax(12.0f, (float) width - 2.0f);
        thumbWidth = juce::jlimit(12.0f, maxThumbWidth, juce::jmax(24.0f, thumbHeight * 2.0f));
    }

    const float thumbX = horizontal ? sliderPos - thumbWidth * 0.5f
                                    : x + width * 0.5f - thumbWidth * 0.5f;
    const float thumbY = horizontal ? y + height * 0.5f - thumbHeight * 0.5f
                                    : sliderPos - thumbHeight * 0.5f;

    juce::Rectangle<float> thumbBounds(thumbX, thumbY, thumbWidth, thumbHeight);
    const float radius = juce::jmin(4.0f, juce::jmin(thumbBounds.getWidth(), thumbBounds.getHeight()) * 0.5f);

    auto accent = slider.findColour(juce::Slider::thumbColourId);
    if (accent.isTransparent())
        accent = PluginColours::contrast;

    auto border = slider.findColour(juce::Slider::trackColourId);
    if (border.isTransparent())
        border = PluginColours::contrast;

    g.setColour(disabled ? PluginColours::surface : PluginColours::onPrimary);
    g.fillRoundedRectangle(thumbBounds, radius);

    g.setColour(disabled ? PluginColours::background : border);
    g.drawRoundedRectangle(thumbBounds.reduced(0.5f), radius, 1.0f);

    if (isOffsetSlider)
    {
        // Offset specific thumb: integer value centered in the handle.
        const int value = juce::roundToInt(slider.getValue());
        g.setColour(disabled ? PluginColours::onDisabled : PluginColours::background);
        g.setFont(getJostFont(9.0f, JostWeight::SemiBold, false));
        g.drawFittedText(juce::String(value), thumbBounds.toNearestInt(), juce::Justification::centred, 1);
    }
    else if (horizontal)
    {
        const float barWidth = 1.6f;
        const float barHeight = juce::jmax(4.0f, thumbHeight - 4.0f);
        const float barX = thumbBounds.getCentreX() - barWidth * 0.5f;
        const float barY = thumbBounds.getCentreY() - barHeight * 0.5f;
        g.setColour(disabled ? PluginColours::background : accent);
        g.fillRoundedRectangle(barX, barY, barWidth, barHeight, barWidth * 0.5f);
    }
    else
    {
        const float barHeight = 1.6f;
        const float barWidth = juce::jmax(4.0f, thumbWidth - 4.0f);
        const float barX = thumbBounds.getCentreX() - barWidth * 0.5f;
        const float barY = thumbBounds.getCentreY() - barHeight * 0.5f;
        g.setColour(disabled ? PluginColours::background : accent);
        g.fillRoundedRectangle(barX, barY, barWidth, barHeight, barHeight * 0.5f);
    }
}

//==============================================================================
// Sliders Horizontaux
//==============================================================================
void PluginLookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
                                         float sliderPos, float minSliderPos, float maxSliderPos,
                                         const juce::Slider::SliderStyle style, juce::Slider& slider)
{
    if (slider.getComponentID() == "VerticalSlider")
    {
        drawVerticalCentreSlider(g, slider, x, y, width, height, sliderPos);
        return;
    }

    auto bounds = juce::Rectangle<float>((float) x, (float) y, (float) width, (float) height).reduced(2.0f, 0.0f);
    const bool disabled = !slider.isEnabled();

    auto laneColour = slider.findColour(juce::Slider::backgroundColourId);
    if (laneColour.isTransparent())
        laneColour = PluginColours::background;

    auto activeColour = slider.findColour(juce::Slider::trackColourId);
    if (activeColour.isTransparent())
        activeColour = PluginColours::primary;

    if (disabled)
    {
        laneColour = PluginColours::background;
        activeColour = PluginColours::surface;
    }

    const float laneHeight = juce::jmax(6.0f, bounds.getHeight() * 0.26f);
    auto lane = juce::Rectangle<float>(bounds.getX(),
                                       bounds.getCentreY() - laneHeight * 0.5f,
                                       bounds.getWidth(),
                                       laneHeight);
    const float laneRadius = laneHeight * 0.5f;

    g.setColour(laneColour);
    g.fillRoundedRectangle(lane, laneRadius);

    g.setColour(disabled ? PluginColours::background : PluginColours::divider);
    g.drawRoundedRectangle(lane.reduced(0.4f), laneRadius, 0.8f);

    const double minValue = slider.getMinimum();
    const double maxValue = slider.getMaximum();
    const bool bipolar = (minValue < 0.0 && maxValue > 0.0);

    if (bipolar)
    {
        const float zeroPos = slider.getPositionOfValue(0.0);
        g.setColour(disabled ? PluginColours::surface : PluginColours::divider);
        g.drawLine(zeroPos,
                   lane.getY() - 1.0f,
                   zeroPos,
                   lane.getBottom() + 1.0f,
                   1.0f);
    }

    if (style == juce::Slider::TwoValueHorizontal)
    {
        const float minPos = slider.getPositionOfValue(slider.getMinValue());
        const float maxPos = slider.getPositionOfValue(slider.getMaxValue());
        const float left = juce::jmin(minPos, maxPos);
        const float right = juce::jmax(minPos, maxPos);

        if (right > left)
        {
            g.setColour(activeColour);
            g.fillRoundedRectangle(juce::Rectangle<float>(left,
                                                          lane.getY(),
                                                          right - left,
                                                          lane.getHeight()),
                                   laneRadius);
        }

        drawLinearSliderThumb(g, x, y, width, height, minPos, minSliderPos, maxSliderPos, style, slider);
        drawLinearSliderThumb(g, x, y, width, height, maxPos, minSliderPos, maxSliderPos, style, slider);
        return;
    }

    float fillStart = lane.getX();
    float fillEnd = juce::jlimit(lane.getX(), lane.getRight(), sliderPos);

    if (bipolar)
    {
        const float zeroPos = juce::jlimit(lane.getX(), lane.getRight(), slider.getPositionOfValue(0.0));
        fillStart = juce::jmin(zeroPos, fillEnd);
        fillEnd = juce::jmax(zeroPos, fillEnd);
    }

    if (fillEnd > fillStart)
    {
        g.setColour(activeColour);
        g.fillRoundedRectangle(juce::Rectangle<float>(fillStart,
                                                      lane.getY(),
                                                      fillEnd - fillStart,
                                                      lane.getHeight()),
                               laneRadius);
    }

    drawLinearSliderThumb(g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
}

//==============================================================================
// Sliders Verticaux
//==============================================================================

void PluginLookAndFeel::drawVerticalCentreSlider(juce::Graphics& g,
                                                 juce::Slider& slider,
                                                 int x, int y, int width, int height,
                                                 float sliderPos)
{
    auto bounds = juce::Rectangle<float>((float) x, (float) y, (float) width, (float) height).reduced(2.0f, 0.0f);
    const bool isNeutral = (std::abs(slider.getValue()) < 0.05);
    const bool disabled = !slider.isEnabled();

    // Narrow center lane only (no pill background).
    const float laneWidth = juce::jmax(4.0f, bounds.getWidth() * 0.20f);
    auto lane = juce::Rectangle<float>(0.0f, bounds.getY() + 2.0f, laneWidth, bounds.getHeight() - 4.0f)
                    .withCentre(bounds.getCentre());
    const float laneRadius = laneWidth * 0.5f;

    auto laneColour = slider.findColour(juce::Slider::backgroundColourId);
    if (laneColour.isTransparent())
        laneColour = PluginColours::background;

    g.setColour(disabled ? PluginColours::disabled : laneColour);
    g.fillRoundedRectangle(lane, laneRadius);

    const float centerY = lane.getCentreY();
    g.setColour(disabled ? PluginColours::onDisabled : PluginColours::contrast);
    g.drawLine(lane.getX() - 2.0f, centerY, lane.getRight() + 2.0f, centerY, 1.0f);

    // Active center fill
    const float thumbHeight = 13.0f;
    const float thumbY = juce::jlimit(lane.getY(),
                                      lane.getBottom() - thumbHeight,
                                      sliderPos - thumbHeight * 0.5f);
    const float thumbCenterY = thumbY + thumbHeight * 0.5f;
    const float fillTop = juce::jmin(centerY, thumbCenterY);
    const float fillBottom = juce::jmax(centerY, thumbCenterY);

    juce::Rectangle<float> activeFill(lane.getX(), fillTop, lane.getWidth(), fillBottom - fillTop);
    if (activeFill.getHeight() > 0.0f)
    {
        g.setColour(disabled
            ? PluginColours::onDisabled
            : (isNeutral ? PluginColours::divider : PluginColours::contrast));
        g.fillRoundedRectangle(activeFill, laneRadius);
    }

    // Thumb
    auto thumb = juce::Rectangle<float>(bounds.getX() + 1.0f, thumbY, bounds.getWidth() - 2.0f, thumbHeight);

    g.setColour(disabled ? PluginColours::disabled : PluginColours::onPrimary);
    g.fillRoundedRectangle(thumb, 4.0f);

    g.setColour(disabled ? PluginColours::onDisabled : PluginColours::contrast);
    g.drawRoundedRectangle(thumb.reduced(0.5f), 4.0f, 1.0f);

    // Value label inside thumb
    const int intVal = (int) slider.getValue();
    juce::String valueText = (intVal > 0 ? "+" + juce::String(intVal) : juce::String(intVal));
    g.setColour(disabled ? PluginColours::onDisabled : PluginColours::background);
    g.setFont(getJostFont(9.0f, JostWeight::SemiBold, false));
    g.drawFittedText(valueText, thumb.toNearestInt(), juce::Justification::centred, 1);
}

//==============================================================================
// Rotary
//==============================================================================
void PluginLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                                         float sliderPosProportional, float rotaryStartAngle, float rotaryEndAngle,
                                         juce::Slider& slider)
{
    const float radius   = juce::jmin(width / 2.0f, height / 2.0f) - 4.0f;
    const float centreX  = (float)x + width * 0.5f;
    const float centreY  = (float)y + height * 0.5f;
    const float angle    = rotaryStartAngle + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);
    const bool enabled = slider.isEnabled();

    // === Cercle de fond ===
    g.setColour(enabled ? PluginColours::divider : PluginColours::background);
    g.drawEllipse(centreX - radius, centreY - radius, radius * 2.0f, radius * 2.0f, 2.0f);

    // === Arc dynamique (contrast) ===
    g.setColour(enabled ? PluginColours::contrast : PluginColours::surface);
    juce::Path valueArc;
    valueArc.addCentredArc(centreX, centreY, radius, radius, 0.0f,
                           rotaryStartAngle, angle, true);
    g.strokePath(valueArc, juce::PathStrokeType(3.0f));

    // === Curseur tangentiellement orienté ===
    const float cursorLength = 4.0f;     // longueur du curseur
    const float cursorWidth  = 2.0f;    // épaisseur du curseur

    // position du point sur le cercle
    const float edgeX = centreX + radius * std::cos(angle - juce::MathConstants<float>::halfPi);
    const float edgeY = centreY + radius * std::sin(angle - juce::MathConstants<float>::halfPi);

    // direction tangentielle (perpendiculaire au rayon)
    const float tangentAngle = angle;  // pour une ligne alignée avec la tangente

    // construire un petit rectangle centré sur (edgeX, edgeY), orienté tangentiellement
    juce::Path cursorPath;
    juce::Rectangle<float> cursorRect(-cursorLength * 0.5f, -cursorWidth * 0.5f,
                                      cursorLength, cursorWidth);
    cursorPath.addRectangle(cursorRect);
    cursorPath.applyTransform(juce::AffineTransform::rotation(tangentAngle)
                                                     .translated(edgeX, edgeY));

    if (enabled)
    {
        g.setColour(PluginColours::contrast);
        g.fillPath(cursorPath);
    }
}

// ======================
// Labels
// ======================
void PluginLookAndFeel::drawLabel(juce::Graphics& g, juce::Label& label)
{
    auto bounds = label.getLocalBounds();
    auto& props = label.getProperties();
    const bool skipTextNormalise =
        (bool) props.getWithDefault("skipTextNormalise", false);
    const bool useExplicitTextColour =
        (bool) props.getWithDefault("useExplicitTextColour", false);

    // Fill only when explicitly opaque to avoid redundant dark frames on surface areas.
    if (label.isOpaque())
    {
        g.setColour(label.findColour(juce::Label::backgroundColourId));
        g.fillRect(bounds);
    }

    g.setFont(getLabelFont(label));

    auto id = label.getName();

    const bool isSliderValue =
        (id == "value_note" || id == "value_num_min" || id == "value_num_max" ||
         id == "value_ratio" || id == "value_simple");

    const bool isMainMenuContext = (getContextStyle() == ComponentStyle::MainMenu);

    // ==== Choix des couleurs ====
    if (useExplicitTextColour)
    {
        g.setColour(label.findColour(juce::Label::textColourId));
    }
    else if (isSliderValue)
    {
        // texte spécial "valeur slider" : primaire si enabled, sinon onDisabled (fix)
        g.setColour(label.isEnabled() ? PluginColours::primary : PluginColours::onDisabled);
    }
    else if (id == "ccStatus")
    {
        g.setColour(PluginColours::onSurface);
    }
    else if (isMainMenuContext)
    {
        // dans MainMenu : le texte doit être onBackground
        g.setColour(label.isEnabled() ? PluginColours::onBackground : PluginColours::onDisabled);
    }
    else
    {
        g.setColour(label.isEnabled() ? PluginColours::onSurface : PluginColours::onDisabled);
    }

    const bool isSingleHandle =
        (id == "value_note" || id == "value_ratio" || id == "value_simple");

    auto justification = isSingleHandle ? juce::Justification::centred
                                        : label.getJustificationType();

    const auto text = skipTextNormalise ? label.getText()
                                        : normaliseText(label.getText());
    g.drawFittedText(text, bounds.reduced(2), justification, 1);
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

    const bool isMainMenu = (getContextStyle() == ComponentStyle::MainMenu);

    // === Fond du texte (gauche) ===
    juce::Colour bgColour = isMainMenu ? PluginColours::mainMenuPrimary : PluginColours::primary;
    if (!box.isEnabled())
        bgColour = PluginColours::disabled;
    g.setColour(bgColour);
    g.fillRect(textArea);

    // === Fond pilule à droite (flèche) ===
    juce::Path arrowBackground;
    const float radius = height * 0.5f;
    addSelectiveRoundedRect(arrowBackground, arrowArea, radius, false, true, false, true);
    g.setColour(isMainMenu ? PluginColours::mainMenuSurface : PluginColours::primary);
    g.fillPath(arrowBackground);

    // === Flèche ===
    const float arrowSize = 4.0f;
    juce::Path arrowPath;
    arrowPath.startNewSubPath(arrowArea.getCentreX() - arrowSize * 0.5f, arrowArea.getCentreY() - 2.0f);
    arrowPath.lineTo(arrowArea.getCentreX(), arrowArea.getCentreY() + 2.0f);
    arrowPath.lineTo(arrowArea.getCentreX() + arrowSize * 0.5f, arrowArea.getCentreY() - 2.0f);

    g.setColour(isMainMenu ? PluginColours::onBackground : PluginColours::onSurface);
    g.strokePath(arrowPath, juce::PathStrokeType(1.5f));

    // === Texte ===
    g.setColour(box.isEnabled()
                    ? (isMainMenu ? PluginColours::onBackground : PluginColours::onSurface)
                    : PluginColours::onDisabled);

    g.setFont(getJostFont(12.0f, JostWeight::Regular, false));
    g.drawFittedText(normaliseText(box.getText()), textArea.toNearestInt(), juce::Justification::centredLeft, 1);
}

// ======================
// Pop up
// ======================
void PluginLookAndFeel::drawPopupMenuBackground(juce::Graphics& g, int width, int height)
{
    const bool isMainMenu = (getContextStyle() == ComponentStyle::MainMenu);

    if (isMainMenu)
    {
        g.setColour(PluginColours::mainMenuSurface);
        g.fillRect(0, 0, width, height);
    }
    else
    {
        g.setColour(PluginColours::secondary);
        g.fillRect(0, 0, width, height);
    }
}

// ======================
// Popup Menu Item
// ======================
void PluginLookAndFeel::drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area,
                                          bool isSeparator, bool isActive, bool isHighlighted,
                                          bool isTicked, bool /*hasSubMenu*/, const juce::String& text,
                                          const juce::String& shortcutKeyText,
                                          const juce::Drawable* /*icon*/, const juce::Colour* textColourToUse)
{
    if (isSeparator)
    {
        g.setColour(PluginColours::divider);
        g.drawLine((float)area.getX(), (float)area.getCentreY(),
                   (float)area.getRight(), (float)area.getCentreY());
        return;
    }

    const bool isMainMenu = (getContextStyle() == ComponentStyle::MainMenu);

    // === Fond highlight ===
    if (isHighlighted)
    {
        g.setColour(isMainMenu ? PluginColours::mainMenuPrimary : PluginColours::primary);
        g.fillRect(area);
    }

    // === Couleur du texte ===
    juce::Colour textColour;
    if (textColourToUse != nullptr)
        textColour = *textColourToUse;
    else if (isActive)
        textColour = isMainMenu ? PluginColours::onBackground
                                : PluginColours::onSurface;
    else
        textColour = PluginColours::onDisabled;

    g.setColour(textColour);
    g.setFont(getJostFont(14.0f, JostWeight::Regular));
    g.drawText(normaliseText(text), area.reduced(4),
               juce::Justification::centredLeft);

    // === Dot si tické ===
    if (isTicked)
    {
        const float dotSize = 6.0f;
        float dotX = (float)area.getRight() - 12.0f;
        float dotY = (float)area.getCentreY() - dotSize * 0.5f;
        g.setColour(isMainMenu ? PluginColours::mainMenuPrimary
                               : PluginColours::primary);
        g.fillEllipse(dotX, dotY, dotSize, dotSize);
    }

    // === Raccourci clavier ===
    if (!shortcutKeyText.isEmpty())
    {
        g.setFont(getJostFont(12.0f, JostWeight::Thin));
        g.drawText(normaliseText(shortcutKeyText), area.reduced(4),
                   juce::Justification::centredRight);
    }
}

// ======================
// Affichage Monitor
// ======================
void PluginLookAndFeel::drawMidiMonitor(juce::Graphics& g,
                                        juce::Rectangle<int> area,
                                        const juce::StringArray& messages)
{
    g.setColour(PluginColours::surface);
    g.fillRect(area);

    g.setColour(PluginColours::onSurface);
    g.setFont(getJostFont(13.0f, JostWeight::Light, false));

    const int lineHeight = 14;
    const int maxLines = area.getHeight() / lineHeight;
    const int startIdx = juce::jmax(0, messages.size() - maxLines);

    int y = area.getY();
    for (int i = startIdx; i < messages.size(); ++i)
    {
        g.drawText(normaliseText(messages[i]),
                   area.getX(), y,
                   area.getWidth(), lineHeight,
                   juce::Justification::left);
        y += lineHeight;
    }
}

// ======================
// Custom Two Line Toggle
// ======================
void PluginLookAndFeel::drawTwoLineToggleButton(juce::Graphics& g,
                                                juce::ToggleButton& button,
                                                bool isHighlighted,
                                                bool isDown,
                                                const juce::String& topText,
                                                const juce::String& bottomText,
                                                bool isEnabled)
{
    auto bounds = button.getLocalBounds().toFloat();
    auto dynamicRadius = getDynamicCornerRadius(button.getLocalBounds());

    // ==== Construction path pilule ====
    juce::Path background;
    addSelectiveRoundedRect(background, bounds, dynamicRadius, true, true, true, true);

    // ==== Couleur de fond ====
    bool isMainMenu = (getContextStyle() == ComponentStyle::MainMenu);

    juce::Colour baseColour;
    if (!isEnabled)
    {
        baseColour = PluginColours::disabled; // fond désactivé
    }
    else if (isMainMenu)
    {
        baseColour = button.getToggleState()
                       ? PluginColours::mainMenuPrimary
                       : PluginColours::mainMenuSurface;
    }
    else
    {
        baseColour = button.getToggleState()
                       ? PluginColours::primary
                       : PluginColours::secondary;
    }

    if (isDown)
        baseColour = baseColour.darker(0.2f);
    else if (isHighlighted)
        baseColour = baseColour.brighter(0.1f);

    g.setColour(baseColour);
    g.fillPath(background);

    // ==== Texte ====
    juce::Colour textColour;
    if (!button.isEnabled())
    {
        textColour = PluginColours::onDisabled;
    }
    else if (isMainMenu)
    {
        // ON -> onBackground, OFF -> background
        textColour = button.getToggleState()
            ? PluginColours::onBackground
            : PluginColours::onSurface;
    }
    else
    {
        // Hors MainMenu : texte toujours lisible sur primary/secondary
        textColour = button.getToggleState()
            ? PluginColours::onSurface
            : PluginColours::onSurface;
    }

    g.setColour(textColour);
    g.setFont(getJostFont(14.0f, JostWeight::Light, false));

    // ==== Dessin des deux lignes ====
    const int halfHeight = bounds.getHeight() / 2;
    const int gap = -4;

    g.drawText(normaliseText(topText), 0, 0,
               (int)bounds.getWidth(), halfHeight - gap,
               juce::Justification::centred);

    g.drawText(normaliseText(bottomText), 0, halfHeight + gap,
               (int)bounds.getWidth(), halfHeight - gap,
               juce::Justification::centred);
}

// ==========================
// Step Toggle Pour Sequencer
// ==========================
void PluginLookAndFeel::drawStepToggle(juce::Graphics& g,
                                       juce::Rectangle<int> bounds,
                                       bool isSelected,
                                       int layerIndex,
                                       const juce::String& label,
                                       bool isEnabled,
                                       bool isHovered,
                                       bool isPlayhead)
{
    auto area = bounds.toFloat();
    auto colours = PluginColours::getLayerColours(layerIndex);
    float cornerSize = juce::jmin(area.getWidth(), area.getHeight()) * 0.25f;

    if (!isEnabled)
    {
        const auto normalised = normaliseText(label);
        const bool isSkip = (normalised.compareIgnoreCase("skip") == 0);
        const bool nonSkip = isSelected && !isSkip;

        // Disabled style requested:
        // - non-skip: background outline + surface fill + background text
        // - skip: background fill + background text
        g.setColour(nonSkip ? PluginColours::surface : PluginColours::background);
        g.fillRoundedRectangle(area, cornerSize);

        if (nonSkip)
        {
            g.setColour(PluginColours::background);
            g.drawRoundedRectangle(area.reduced(0.8f), cornerSize, 1.2f);
        }

        g.setColour(PluginColours::background);
        g.setFont(getJostFont(10.5f, isSkip ? JostWeight::Light : JostWeight::Bold));
        g.drawText(normalised, bounds, juce::Justification::centred, 1);
        return;
    }

    // === Fond ===
    g.setColour(isSelected ? colours.background : colours.text);
    g.fillRoundedRectangle(area, cornerSize);

    // === Bordure si survol ===
    if (isHovered)
    {
        g.setColour(PluginColours::onSurface);
        g.drawRoundedRectangle(area.reduced(1.0f), cornerSize, 1.0f);
    }

    if (isPlayhead)
    {
        g.setColour(PluginColours::primary);
        g.fillRoundedRectangle(area.reduced(1.2f), cornerSize);

        g.setColour(PluginColours::onPrimary);
        g.drawRoundedRectangle(area.reduced(0.85f), cornerSize, 2.4f);
    }

    // === Label texte ===
    auto textColour = isSelected ? colours.text : colours.background;
    g.setColour(textColour);

    auto normalised = normaliseText(label);
    auto isSkip = (normalised.compareIgnoreCase("skip") == 0);

    g.setFont(getJostFont(10.5f, isSkip ? JostWeight::Light : JostWeight::Bold));
    g.drawText(normalised, bounds, juce::Justification::centred, 1);
}

// ======================
// Font utilitaire pour labels
// ======================
juce::Font PluginLookAndFeel::getLabelFont(juce::Label& label)
{
    if (label.getName().containsIgnoreCase("snapshotname"))
        return getJostFont(15.0f, JostWeight::Bold, false);

    bool isItalic = label.getName().containsIgnoreCase("italic");
    bool isBold   = label.getName().containsIgnoreCase("bold");
    auto weight   = isBold ? JostWeight::Bold : JostWeight::Regular;

    return getJostFont(13.0f, weight, isItalic);
}

// ======================
// getJostFont
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
    return juce::Font(juce::FontOptions(tf).withHeight(size));
}

// ========================================================================
// Destructeur
// ========================================================================
PluginLookAndFeel::~PluginLookAndFeel() = default;
