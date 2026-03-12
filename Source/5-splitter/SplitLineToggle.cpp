/*
==============================================================================
SplitLineToggle.cpp
------------------------------------------------------------------------------
Role du fichier:
- Toggle visuel compact pour activer/desactiver une branche du splitter.

Comportement:
- Clic gauche inverse etat actif.
- Emission callback onToggle(newState) apres changement.
- Le rendu colore encode etat + index de branche.

Point de vigilance:
- La branche 0 (direct out) suit une convention visuelle specifique.
  Elle ne doit pas etre confondue avec les branches additionnelles.
==============================================================================
*/

#include "SplitLineToggle.h"
#include "../PluginColours.h"

SplitLineToggle::SplitLineToggle(int index)
    : layerIndex(index)
{
    // Taille fixe pour garantir un cercle net sans clipping.
    setSize(32, 32);
    setInterceptsMouseClicks(true, true);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

void SplitLineToggle::setActive(bool shouldBeActive)
{
    if (active != shouldBeActive)
    {
        active = shouldBeActive;
        repaint();

        if (onToggle)
            onToggle(active);
    }
}

void SplitLineToggle::mouseDown(const juce::MouseEvent&)
{
    setActive(!active);
}

void SplitLineToggle::paint(juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat();
    float outerRadius = juce::jmin(area.getWidth(), area.getHeight()) * 0.5f;
    juce::Point<float> center = area.getCentre();

    // Cercle exterieur: code principal ON/OFF.
    // Branche 0: convention visuelle stable (direct out).
    if (layerIndex == 0)
        g.setColour(PluginColours::primary);
    else
        g.setColour(active ? PluginColours::primary
                           : PluginColours::secondary);

    g.fillEllipse(area);

    // Cercle interieur: inversion de contraste pour lisibilite.
    float innerRadius = outerRadius * 0.5f;
    juce::Rectangle<float> inner = juce::Rectangle<float>(
        center.x - innerRadius, center.y - innerRadius,
        innerRadius * 2.0f, innerRadius * 2.0f
    );

    // Actif -> couleur layer, inactif -> texte layer.
    g.setColour(active
        ? PluginColours::getLayerColours(layerIndex).background
        : PluginColours::getLayerColours(layerIndex).text
    );
    g.fillEllipse(inner);
}
