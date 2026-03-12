/*
==============================================================================
SplitterModeToggle.cpp
------------------------------------------------------------------------------
Role du fichier:
- Toggle de mode binaire du splitter:
  - RoundRobin
  - RangeSplit

Comportement:
- Clic utilisateur: alterne le mode et notifie onModeChange.
- Changement parametre externe (APVTS): parameterValueChanged met a jour la vue.

Invariants:
- currentMode est la source de verite visuelle locale.
- setMode() ne fait que setter + repaint, sans side effect metier.
==============================================================================
*/

#include "SplitterModeToggle.h"
#include "../PluginColours.h"
#include "../PluginLookAndFeel.h"

SplitterModeToggle::SplitterModeToggle()
{
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    setInterceptsMouseClicks(true, true);
}

void SplitterModeToggle::mouseDown(const juce::MouseEvent&)
{
    // Toggle local immediat + propagation callback.
    currentMode = (currentMode == RoundRobin) ? RangeSplit : RoundRobin;
    if (onModeChange) onModeChange(currentMode);
    repaint();
}

void SplitterModeToggle::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    float radius = PluginLookAndFeel::getDynamicCornerRadius(getLocalBounds());

    // Fond pilule.
    g.setColour(PluginColours::secondary);
    g.fillRoundedRectangle(bounds, radius);

    // Curseur circulaire gauche/droite selon mode.
    float padding = 4.0f;
    float circleSize = bounds.getHeight() - 2 * padding;
    float x = (currentMode == RoundRobin) ? bounds.getX() + padding
                                          : bounds.getRight() - padding - circleSize;

    juce::Rectangle<float> circleArea(x, bounds.getY() + padding, circleSize, circleSize);
    
    g.setColour(PluginColours::primary);
    g.fillEllipse(circleArea);
}

void SplitterModeToggle::parameterValueChanged(int, float newValue)
{
    // Mapping float APVTS -> enum binaire.
    const auto newMode = (newValue < 0.5f) ? RoundRobin : RangeSplit;

    if (newMode != currentMode)
    {
        setMode(newMode);
        if (onModeChange) onModeChange(newMode);
        repaint();
    }
}

void SplitterModeToggle::setMode(Mode newMode)
{
    currentMode = newMode;
    repaint();
}
