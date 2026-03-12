/*
==============================================================================
FlatComboBoxParameterAttachment.cpp
------------------------------------------------------------------------------
Role du fichier:
- Adaptateur entre un FlatComboBox custom et un RangedAudioParameter JUCE.

Concept de design:
- Pattern "Adapter":
  - Sens UI -> parametre: changement combo => ecriture normalisee APVTS.
  - Sens parametre -> UI: callback ParameterAttachment => MAJ combo.

Invariants importants:
- Eviter les boucles de callback avec ignoreCallbacks.
- Conserver le callback utilisateur existant (userCallback) et le restaurer
  en destruction pour ne pas casser la chaine d interaction UI.

Threading:
- Message thread pour la vue.
- Notifications parametre gerees par JUCE ParameterAttachment.
- Aucune logique RT ici.
==============================================================================
*/

#include "FlatComboBoxParameterAttachment.h"

FlatComboBoxParameterAttachment::FlatComboBoxParameterAttachment(juce::RangedAudioParameter& param,
                                                                 FlatComboBox& c,
                                                                 juce::UndoManager* um)
    : comboBox(c),
      storedParameter(param),
      attachment(param, [this](float f) { setValue(f); }, um)
{
    // Synchronise la vue avec la valeur courante du parametre avant de
    // brancher les callbacks utilisateur.
    sendInitialUpdate();
    userCallback = comboBox.onChange;
    comboBox.onChange = [this]()
    {
        if (! ignoreCallbacks)
            comboBoxChanged();
        if (userCallback)
            userCallback();
    };
}

FlatComboBoxParameterAttachment::~FlatComboBoxParameterAttachment()
{
    comboBox.onChange = userCallback;
}

void FlatComboBoxParameterAttachment::sendInitialUpdate()
{
    attachment.sendInitialUpdate();
}

void FlatComboBoxParameterAttachment::setValue(float newValue)
{
    // Mapping parametre -> combo:
    // - convertTo0to1 donne la position normalisee.
    // - getNumSteps fixe le nombre de choix discrets.
    auto rangeSteps = storedParameter.getNumSteps();
    auto normValue = storedParameter.convertTo0to1(newValue);
    auto index = static_cast<int>(juce::roundToInt(normValue * (rangeSteps - 1)));
    int id = index + 1;

    if (id == comboBox.getSelectedId())
        return;

    const juce::ScopedValueSetter<bool> svs(ignoreCallbacks, true);
    comboBox.setSelectedId(id, juce::sendNotificationSync);
}

void FlatComboBoxParameterAttachment::comboBoxChanged()
{
    // Mapping combo -> parametre:
    // - selectedId est 1-based cote UI.
    // - APVTS attend une valeur normalisee [0..1].
    auto numItems = storedParameter.getNumSteps();
    auto selected = static_cast<float>(comboBox.getSelectedId() - 1);
    auto newValue = numItems > 1 ? selected / static_cast<float>(numItems - 1)
                                 : 0.0f;

    attachment.setValueAsCompleteGesture(storedParameter.convertFrom0to1(newValue));
}
