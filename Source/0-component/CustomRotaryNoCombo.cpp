/*
==============================================================================
CustomRotaryNoCombo.cpp
------------------------------------------------------------------------------
Role du fichier:
- Variante "rotary only" du composant CustomRotaryWithCombo.

Intention:
- Conserver le meme moteur de dessin/interaction que la version complete,
  mais sans combo visible.
- Adapter le layout (label + knob) pour une presentation compacte.

Threading:
- UI thread uniquement.
==============================================================================
*/

#include "CustomRotaryNoCombo.h"

void CustomRotaryNoCombo::resized()
{
    auto area = getLocalBounds();

    const int labelHeight = 20;
    const int labelToKnobSpace = 4;
    const int diameter = juce::jmin(area.getWidth(), area.getHeight() - labelHeight - labelToKnobSpace);

    // Invariant layout:
    // - label en haut
    // - knob centre horizontalement sous le label
    rotaryBounds = juce::Rectangle<int>(
        area.getCentreX() - diameter / 2,
        labelHeight + labelToKnobSpace,
        diameter, diameter);

    // Le titre garde une largeur stable pour eviter les "jumps" visuels.
    titleLabel.setBounds(area.getCentreX() - 50, 0, 100, labelHeight);
}
