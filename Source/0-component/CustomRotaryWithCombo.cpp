/*
==============================================================================
CustomRotaryWithCombo.cpp
------------------------------------------------------------------------------
Role du fichier:
- Composant rotary custom avec:
  - drag vertical,
  - optional combo de selection,
  - rendu piste/arc personnalise,
  - smoothing visuel non destructif.

Place dans l architecture:
- UI pur, utilise dans plusieurs modules (InputFilter/Harmonizer/Arp/MainBar).
- Peut etre relie a APVTS via attachments externes.

Invariants:
- value est la valeur discrete "source de verite" cote UI.
- displayValue est une projection lissee de value (uniquement visuelle).
- isDragging force un suivi direct (pas de smoothing pendant interaction).

Threading:
- Message thread uniquement.
==============================================================================
*/

#include "CustomRotaryWithCombo.h"
#include "../PluginLookAndFeel.h"

constexpr float dragDistanceForFullRange = 100.0f;

CustomRotaryWithCombo::CustomRotaryWithCombo(const juce::String& labelText,
                                             int minVal, int maxVal, int defaultVal,
                                             bool centeredRange,
                                             ArcMode mode,
                                             bool showCombo)
    : minValue(minVal), maxValue(maxVal), value(defaultVal),
      displayValue((float)defaultVal),
      arcMode(centeredRange ? ArcMode::CenterToCursor : mode),
      showComboBox(showCombo)
{
    titleLabel.setText(labelText, juce::dontSendNotification);
    titleLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(titleLabel);

    buildComboItems();
    combo.setSelectedId(value - minValue + 1);
    combo.setShowArrow(false);
    combo.onChange = [this] { updateFromCombo(); };

    comboShadow = std::make_unique<ShadowComponent>(
        juce::DropShadow(juce::Colours::black, 0, {0, 0}),
        -1.0f, true, true, true, true, 6);
    comboShadow->addAndMakeVisible(combo);
    addAndMakeVisible(*comboShadow);

    if (!showComboBox)
    {
        combo.setVisible(false);
        comboShadow->setVisible(false);
    }

    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    setPaintingIsUnclipped(true);
}

void CustomRotaryWithCombo::setArcMode(ArcMode mode)
{
    if (arcMode == mode)
        return;

    arcMode = mode;
    repaint();
}

void CustomRotaryWithCombo::setUseSmoothing(bool shouldSmooth)
{
    if (useSmoothing == shouldSmooth)
        return;

    useSmoothing = shouldSmooth;

    if (!useSmoothing)
    {
        // Mode direct: rendu immediat, aucun rattrapage progressif.
        displayValue = (float) value;
        smoothingActive = false;
        repaint();
        return;
    }

    smoothingActive = (!isDragging && std::abs(displayValue - (float) value) > 0.01f);
}

void CustomRotaryWithCombo::mouseDown(const juce::MouseEvent& e)
{
    isDragging = true;
    valueAtMouseDown = value;

    if (onMouseDownLayerChange && layerIndex >= 0)
        onMouseDownLayerChange(layerIndex);
}

void CustomRotaryWithCombo::setRange(int minVal, int maxVal)
{
    minValue = minVal; maxValue = maxVal;
    useStringList = false;
    buildComboItems();
    setValue(value, false);
}

void CustomRotaryWithCombo::setValue(int newVal, bool sendNotification)
{
    const float oldDisplayValue = displayValue;
    const bool oldSmoothingActive = smoothingActive;
    const int oldValue = value;

    const int clampedValue = useStringList
                               ? juce::jlimit(0, (int) stringList.size() - 1, newVal)
                               : juce::jlimit(minValue, maxValue, newVal);
    value = clampedValue;

    if (showComboBox)
    {
        const int desiredSelectedId = useStringList ? (value + 1) : (value - minValue + 1);
        if (combo.getSelectedId() != desiredSelectedId)
        {
            syncingComboSelection = true;
            combo.setSelectedId(desiredSelectedId, juce::dontSendNotification);
            syncingComboSelection = false;
        }
    }

    // Smoothing strictement visuel:
    // - desactive ou drag: displayValue suit instantanement value.
    // - sinon: uiTimerTick interpole vers la cible.
    if (!useSmoothing || isDragging)
    {
        displayValue = (float) value;
        smoothingActive = false;
    }
    else if (std::abs(displayValue - (float) value) > 0.01f)
    {
        smoothingActive = true;
    }
    else
    {
        smoothingActive = false;
    }

    const bool valueChanged = (oldValue != value);
    const bool displayChanged = (std::abs(oldDisplayValue - displayValue) > 0.001f);
    const bool smoothingChanged = (oldSmoothingActive != smoothingActive);

    if (valueChanged || displayChanged || smoothingChanged)
        repaint();

    if (sendNotification && valueChanged && onValueChange)
        onValueChange(value);
}

void CustomRotaryWithCombo::buildComboItems()
{
    // Liste numerique par defaut. La variante string passe par setStringList().
    if (!showComboBox || useStringList) return;

    combo.clear();
    for (int v = minValue; v <= maxValue; ++v)
    {
        juce::String label = (showPlusSign && v > 0) ? "+" + juce::String(v) : juce::String(v);
        combo.addItem(label, v - minValue + 1);
    }
}

void CustomRotaryWithCombo::setStringList(const std::vector<juce::String>& items)
{
    stringList = items;
    useStringList = true;

    combo.clear();
    for (size_t i = 0; i < items.size(); ++i)
        combo.addItem(items[i], (int)i + 1);

    value = juce::jlimit(0, (int)items.size() - 1, value);
    syncingComboSelection = true;
    combo.setSelectedId(value + 1, juce::dontSendNotification);
    syncingComboSelection = false;
    repaint();
}

void CustomRotaryWithCombo::setCenterOverlayText(const juce::String& text)
{
    if (centerOverlayText == text)
        return;

    centerOverlayText = text;
    repaint();
}

void CustomRotaryWithCombo::setCenterOverlayColour(juce::Colour colour)
{
    if (centerOverlayColour == colour)
        return;

    centerOverlayColour = colour;
    repaint();
}

void CustomRotaryWithCombo::setDisplayValueOverride(float visualValue)
{
    const float clamped = useStringList
                              ? juce::jlimit(0.0f,
                                             (float) juce::jmax(0, (int) stringList.size() - 1),
                                             visualValue)
                              : juce::jlimit((float) minValue, (float) maxValue, visualValue);

    if (hasDisplayValueOverride && std::abs(displayValueOverride - clamped) < 0.0001f)
        return;

    hasDisplayValueOverride = true;
    displayValueOverride = clamped;
    repaint();
}

void CustomRotaryWithCombo::clearDisplayValueOverride()
{
    if (!hasDisplayValueOverride)
        return;

    hasDisplayValueOverride = false;
    repaint();
}

juce::String CustomRotaryWithCombo::getSelectedString() const
{
    if (useStringList && value >= 0 && value < (int)stringList.size())
        return stringList[(size_t)value];
    return {};
}

void CustomRotaryWithCombo::updateFromCombo()
{
    if (!showComboBox || syncingComboSelection)
        return;

    // Mapping 1-based combo id -> index/valeur interne.
    if (useStringList)
        setValue(combo.getSelectedId() - 1);
    else
        setValue(minValue + combo.getSelectedId() - 1);
}

void CustomRotaryWithCombo::mouseDrag(const juce::MouseEvent& e)
{
    if (isDragging)
    {
        // Drag vertical normalise sur une distance de reference.
        float pixelDelta = (float)(e.position.getY() - e.mouseDownPosition.getY());
        float normDelta  = -pixelDelta / dragDistanceForFullRange;

        int range = useStringList ? (int)stringList.size() - 1 : (maxValue - minValue);
        int newValue = juce::roundToInt((float)valueAtMouseDown + normDelta * (float)range);
        setValue(newValue);
    }
}

void CustomRotaryWithCombo::mouseUp(const juce::MouseEvent& e)
{
    juce::ignoreUnused(e);
    isDragging = false;

    // After drag, allow smoothing again (for automation/snapshots).
    // But we keep displayValue aligned right now to avoid a jump.
    displayValue = (float) value;
    smoothingActive = false;
}

void CustomRotaryWithCombo::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    if (wheel.deltaY != 0.0f)
    {
        int step = (wheel.deltaY > 0.0f ? 1 : -1);
        setValue(value + step);
    }
}

void CustomRotaryWithCombo::uiTimerTick()
{
    // Jamais de smoothing pendant drag pour garder un retour immediat.
    if (!smoothingActive || !isShowing())
        return;

    if (!useSmoothing || isDragging)
    {
        smoothingActive = false;
        return;
    }

    const float target = (float) value;
    if (std::abs(displayValue - target) > 0.01f)
    {
        displayValue += (target - displayValue) * 0.2f;
        repaint();
        return;
    }

    displayValue = target;
    smoothingActive = false;
}

void CustomRotaryWithCombo::resized()
{
    auto area = getLocalBounds();

    int availableHeight = showComboBox ? area.getHeight() - 20 : area.getHeight();
    int diameter = juce::jmin(area.getWidth() / 2, availableHeight / 2);
    rotaryBounds = juce::Rectangle<int>(
        area.getCentreX() - diameter / 2,
        area.getCentreY() - diameter / 2,
        diameter, diameter);

    int labelHeight = 20;
    titleLabel.setBounds(area.getCentreX() - 50,
                         rotaryBounds.getY() - labelHeight - 4,
                         100, labelHeight);

    if (showComboBox)
    {
        int comboHeight = 20;
        int comboWidth = juce::jmin(juce::roundToInt(area.getWidth() / 1.5), area.getWidth());
        auto comboArea = juce::Rectangle<int>(
            area.getCentreX() - comboWidth / 2,
            rotaryBounds.getBottom() + 4,
            comboWidth, comboHeight);
        comboShadow->setBounds(comboArea.expanded(4));
        combo.setBounds(comboShadow->getShadowArea());
    }
}

void CustomRotaryWithCombo::paint(juce::Graphics& g)
{
    auto boundsF = rotaryBounds.toFloat();
    float radius = boundsF.getWidth() * 0.5f;
    auto center = boundsF.getCentre();
    const bool enabled = isEnabled();

    // Disabled rotary style (uniform):
    // - draw only track background + position arc
    // - no core disk, no cursor, no center text
    if (enabled)
    {
        // Keep the rotary core anthracite (original look).
        g.setColour(PluginColours::onPrimary);
        g.fillEllipse(boundsF);
    }

    float innerRadius = radius + 2.0f;
    float thickness   = 4.0f;
    float arcLength   = juce::MathConstants<float>::pi * 1.5f;
    float startAngleBase = juce::MathConstants<float>::pi * 1.25f;

    juce::Colour baseArcColour = PluginColours::background;
    juce::Colour activeArcColour = PluginColours::surface;

    if (enabled)
    {
        activeArcColour = PluginColours::contrast;

        if (useLayerColours && layerIndex >= 0)
        {
            auto layerColours = PluginColours::getLayerColours(layerIndex);
            baseArcColour = layerColours.text;
            activeArcColour = layerColours.background;
        }
    }

    juce::Path baseArc;
    baseArc.addCentredArc(center.x, center.y, innerRadius, innerRadius,
                          0.0f, startAngleBase, startAngleBase + arcLength, true);
    g.setColour(baseArcColour);
    g.strokePath(baseArc, juce::PathStrokeType(thickness));

    const float visualValue = hasDisplayValueOverride ? displayValueOverride : displayValue;
    float norm = 0.0f;
    if (useStringList)
        norm = visualValue / (float)(juce::jmax((int)stringList.size() - 1, 1));
    else
        norm = (float)(visualValue - (float)minValue) / (float)(maxValue - minValue);

    if (invertedFill)
        norm = 1.0f - norm;

    float startAngleDynamic, endAngle;
    if (arcMode == ArcMode::LeftToCursor)
    {
        startAngleDynamic = startAngleBase;
        endAngle = startAngleDynamic + norm * arcLength;
    }
    else if (arcMode == ArcMode::RightToCursor)
    {
        startAngleDynamic = startAngleBase + arcLength;
        endAngle = startAngleDynamic - norm * arcLength;
    }
    else
    {
        startAngleDynamic = startAngleBase + arcLength * 0.5f;
        endAngle = startAngleDynamic + (norm - 0.5f) * arcLength;
    }

    juce::Path valueArc;
    valueArc.addCentredArc(center.x, center.y, innerRadius, innerRadius,
                           0.0f, startAngleDynamic, endAngle, true);
    g.setColour(activeArcColour);
    g.strokePath(valueArc, juce::PathStrokeType(thickness));

    const float cursorLength = 4.0f, cursorWidth = 2.0f;
    const float edgeX = center.x + innerRadius * std::cos(endAngle - juce::MathConstants<float>::halfPi);
    const float edgeY = center.y + innerRadius * std::sin(endAngle - juce::MathConstants<float>::halfPi);

    juce::Path cursorPath;
    cursorPath.addRectangle(-cursorLength * 0.5f, -cursorWidth * 0.5f, cursorLength, cursorWidth);
    cursorPath.applyTransform(juce::AffineTransform::rotation(endAngle + juce::MathConstants<float>::halfPi)
                                                     .translated(edgeX, edgeY));
    if (enabled)
    {
        g.setColour(activeArcColour);
        g.fillPath(cursorPath);
    }

    if (enabled && centerOverlayText.isNotEmpty())
    {
        g.setColour(centerOverlayColour);

        const float fontSize = juce::jlimit(9.0f, 14.0f, boundsF.getWidth() * 0.22f);
        g.setFont(juce::Font(juce::FontOptions(fontSize, juce::Font::plain)));

        const auto textBounds = rotaryBounds.reduced(6, 6);
        // No ellipsis for center overlay; user requested raw value display.
        g.drawText(centerOverlayText, textBounds, juce::Justification::centred, false);
    }
}
