//==============================================================================
// LinkToggle.cpp
//------------------------------------------------------------------------------
// Composant UI:
// - mode standard (tri-etat): clic gauche cycle F -> Link -> Unlink -> F
// - mode Rythm (binaire): clic gauche alterne F <-> R
// - clic droit: menu rate local en mode standard quand etat Unlink
//==============================================================================

#include "LinkToggle.h"

namespace
{
    static constexpr int kModeCount = 3;
}

LinkToggle::LinkToggle(int layerIndexIn)
    : layerIndex(juce::jlimit(0, 7, layerIndexIn))
{
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

void LinkToggle::setLayerIndex(int index)
{
    const int clamped = juce::jlimit(0, 7, index);
    if (layerIndex == clamped)
        return;

    layerIndex = clamped;
    repaint();
}

void LinkToggle::setModeIndex(int newMode, juce::NotificationType notification)
{
    const int clamped = binaryFRMode
        ? juce::jlimit((int) Mode::Link, (int) Mode::F, newMode)
        : juce::jlimit(0, kModeCount - 1, newMode);
    if (modeIndex == clamped)
        return;

    modeIndex = clamped;
    repaint();

    if (notification == juce::sendNotification && onModeChange)
        onModeChange(modeIndex);
}

void LinkToggle::setBinaryFRMode(bool shouldUseBinaryFR)
{
    if (binaryFRMode == shouldUseBinaryFR)
        return;

    binaryFRMode = shouldUseBinaryFR;
    setModeIndex(binaryFRMode ? juce::jlimit((int) Mode::Link, (int) Mode::F, modeIndex)
                              : juce::jlimit(0, kModeCount - 1, modeIndex),
                 juce::dontSendNotification);
    repaint();
}

void LinkToggle::setUnlinkRateLabel(const juce::String& label)
{
    if (unlinkRateLabel == label)
        return;

    unlinkRateLabel = label;
    if (modeIndex == Mode::Unlink)
        repaint();
}

juce::String LinkToggle::getDisplayText() const
{
    if (binaryFRMode)
        return (modeIndex == Mode::F ? "F" : "R");

    if (modeIndex == Mode::F)
        return "F";

    if (modeIndex == Mode::Link)
        return "Link";

    return unlinkRateLabel;
}

int LinkToggle::getNextModeFromClick() const noexcept
{
    if (binaryFRMode)
        return (modeIndex == Mode::F ? Mode::Link : Mode::F);

    // UX cycle matches requested naming:
    // F -> Link -> Unlink -> F.
    switch (modeIndex)
    {
        case Mode::F:      return Mode::Link;
        case Mode::Link:   return Mode::Unlink;
        case Mode::Unlink: return Mode::F;
        default:           return Mode::F;
    }
}

void LinkToggle::paint(juce::Graphics& g)
{
    const auto colours = PluginColours::getLayerColours(layerIndex);
    const auto bounds = getLocalBounds().toFloat();
    const float cornerSize = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.26f;

    const bool isFollowerRateMode = binaryFRMode
        ? (modeIndex == Mode::F)
        : (modeIndex == Mode::F || modeIndex == Mode::Link);
    const juce::Colour bg = isFollowerRateMode ? colours.background : colours.text;
    const juce::Colour fg = isFollowerRateMode ? colours.text : colours.background;

    g.setColour(bg);
    g.fillRoundedRectangle(bounds, cornerSize);

    g.setColour(hover ? PluginColours::onSurface : fg.withAlpha(0.35f));
    g.drawRoundedRectangle(bounds.reduced(1.0f), cornerSize, hover ? 1.25f : 1.0f);

    juce::Font font(juce::FontOptions(10.5f));
    if (modeIndex == Mode::F)
        font = font.boldened();

    g.setColour(fg);
    g.setFont(font);
    g.drawFittedText(getDisplayText(),
                     getLocalBounds().reduced(3, 1),
                     juce::Justification::centred,
                     1,
                     0.95f);
}

void LinkToggle::mouseDown(const juce::MouseEvent& e)
{
    pendingRightClick = e.mods.isRightButtonDown() || e.mods.isPopupMenu();
}

void LinkToggle::mouseUp(const juce::MouseEvent& e)
{
    const bool isRightClick =
        pendingRightClick &&
        (e.mods.isRightButtonDown() || e.mods.isPopupMenu() || e.mouseWasClicked());

    pendingRightClick = false;

    if (!isEnabled())
        return;

    if (isRightClick)
    {
        if (!binaryFRMode && modeIndex == Mode::Unlink && onRequestRateMenu)
            onRequestRateMenu();
        return;
    }

    if (!e.mods.isLeftButtonDown() && !e.mouseWasClicked())
        return;

    const int nextMode = getNextModeFromClick();
    setModeIndex(nextMode, juce::sendNotification);
}

void LinkToggle::mouseEnter(const juce::MouseEvent&)
{
    hover = true;
    repaint();
}

void LinkToggle::mouseExit(const juce::MouseEvent&)
{
    hover = false;
    repaint();
}
