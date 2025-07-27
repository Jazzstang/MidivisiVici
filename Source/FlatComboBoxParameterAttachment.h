#pragma once

#include <JuceHeader.h>
#include "FlatComboBox.h"

/**
 * Attachment to connect a FlatComboBox with a RangedAudioParameter.
 * Mirrors ComboBoxParameterAttachment but listens to FlatComboBox::onChange.
 */
class FlatComboBoxParameterAttachment
{
public:
    FlatComboBoxParameterAttachment(juce::RangedAudioParameter& parameter,
                                    FlatComboBox& combo,
                                    juce::UndoManager* undoManager = nullptr);
    ~FlatComboBoxParameterAttachment();

    /// Call this after setting up your combo box if more setup is required
    void sendInitialUpdate();

private:
    void setValue(float newValue);
    void comboBoxChanged();

    FlatComboBox& comboBox;
    juce::RangedAudioParameter& storedParameter;
    juce::ParameterAttachment attachment;
    bool ignoreCallbacks = false;
    std::function<void()> userCallback;
};

