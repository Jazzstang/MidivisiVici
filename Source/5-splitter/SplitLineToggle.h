/**
 * @file SplitLineToggle.h
 * @brief Circular branch on/off toggle used by Splitter lines.
 *
 * Threading:
 * - UI thread only.
 * - Not RT-safe.
 */
#pragma once

#include <JuceHeader.h>

/** @brief Circular on/off indicator and trigger for one splitter branch. */
class SplitLineToggle : public juce::Component
{
public:
    explicit SplitLineToggle(int index);

    /** Change l'état actif/inactif */
    void setActive(bool shouldBeActive);

    /** État actuel */
    bool isActive() const noexcept { return active; }

    /** Récupère l'index de la ligne (layer) */
    int getLayerIndex() const noexcept { return layerIndex; }

    /** Callback appelé lors du changement d'état */
    std::function<void(bool)> onToggle;

    /** Dessin du bouton */
    void paint(juce::Graphics& g) override;

    /** Gestion clic souris */
    void mouseDown(const juce::MouseEvent&) override;

private:
    bool active = true;
    int layerIndex = 0;
};
