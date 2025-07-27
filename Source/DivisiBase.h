#pragma once

#include <JuceHeader.h>
#include "FlatComboBox.h"

/**
 * Composant graphique pour la gestion du module Divisi.
 * Contient :
 *  - Un header activable
 *  - Un sélecteur de mode (FlatComboBox)
 *  - Un composant dynamique pour les différents modes de division
 */
class DivisiBase : public juce::Component
{
public:
    explicit DivisiBase(juce::AudioProcessorValueTreeState& state);
    ~DivisiBase() override;

    void resized() override;

private:
    juce::AudioProcessorValueTreeState& parameters;

    // === Header ===
    juce::ToggleButton headerButton { "Divisi" };

    // === Mode Selector ===
    FlatComboBox modeSelector;

    // === Composant dynamique (différents modes) ===
    std::unique_ptr<juce::Component> activeModeComponent;

    // === Attachments ===
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> enableAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modeAttachment;

    void changeMode(int modeId);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DivisiBase)
};
