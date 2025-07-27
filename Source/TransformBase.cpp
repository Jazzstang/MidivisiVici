#include "TransformBase.h"
#include "PluginParameters.h"
#include "TransformTranspose.h"
#include "TransformOctaveRandomizer.h"
#include "TransformVelocityFormer.h"
#include "TransformStrummer.h"
#include "FlatComboBoxParameterAttachment.h"

TransformBase::TransformBase(juce::AudioProcessorValueTreeState& state)
    : parameters(state)
{
    // === Header ===
    addAndMakeVisible(headerButton);
    enableAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        parameters, ParamIDs::transformEnable, headerButton);

    // === Mode Selector ===
    modeSelector.addItem("Bypass", 1);
    modeSelector.addItem("Transpose", 2);
    modeSelector.addItem("Octave Randomizer", 3);
    modeSelector.addItem("Velocity Former", 4);
    modeSelector.addItem("Strummer", 5);
    addAndMakeVisible(modeSelector);

    // === Callback changement mode ===
    modeSelector.onChange = [this]
    {
        changeMode(modeSelector.getSelectedId());
    };

    modeAttachment = std::make_unique<FlatComboBoxParameterAttachment>(
        *parameters.getParameter(ParamIDs::transformMode), modeSelector);

    // Initialisation du composant actif
    changeMode(modeSelector.getSelectedId());
}

TransformBase::~TransformBase() = default;

void TransformBase::changeMode(int modeId)
{
    // Supprime le composant précédent proprement
    activeModeComponent.reset();

    // Crée un nouveau composant selon le mode choisi
    switch (modeId)
    {
        case 2: activeModeComponent = std::make_unique<TransformTranspose>(parameters); break;
        case 3: activeModeComponent = std::make_unique<TransformOctaveRandomizer>(parameters); break;
        case 4: activeModeComponent = std::make_unique<TransformVelocityFormer>(parameters); break;
        case 5: activeModeComponent = std::make_unique<TransformStrummer>(parameters); break;
        default: break; // Mode Bypass = aucun composant
    }

    // Ajoute le nouveau composant si présent
    if (activeModeComponent)
        addAndMakeVisible(*activeModeComponent);

    resized(); // repositionnement automatique
}

void TransformBase::resized()
{
    auto area = getLocalBounds();

    // === Header ===
    headerButton.setBounds(area.removeFromTop(24));
    area.removeFromTop(4);

    // === Mode Selector ===
    modeSelector.setBounds(area.removeFromTop(24));
    area.removeFromTop(8);

    // === Zone dynamique ===
    if (activeModeComponent)
        activeModeComponent->setBounds(area);
}
