#include "FlatComboBoxParameterAttachment.h"

FlatComboBoxParameterAttachment::FlatComboBoxParameterAttachment(juce::RangedAudioParameter& param,
                                                                 FlatComboBox& c,
                                                                 juce::UndoManager* um)
    : comboBox(c),
      storedParameter(param),
      attachment(param, [this](float f) { setValue(f); }, um)
{
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
    auto numItems = storedParameter.getNumSteps();
    auto selected = static_cast<float>(comboBox.getSelectedId() - 1);
    auto newValue = numItems > 1 ? selected / static_cast<float>(numItems - 1)
                                 : 0.0f;

    attachment.setValueAsCompleteGesture(storedParameter.convertFrom0to1(newValue));
}

