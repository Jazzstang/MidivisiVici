/**
 * @file FlatComboBoxParameterAttachment.h
 * @brief APVTS attachment between `FlatComboBox` and a ranged parameter.
 *
 * Threading:
 * - UI side updates on message thread.
 * - Host/parameter notifications may originate from non-UI contexts and are
 *   handled by JUCE `ParameterAttachment`.
 */
#pragma once

#include <JuceHeader.h>
#include "FlatComboBox.h"

/**
 * @brief Connects a `FlatComboBox` to a `juce::RangedAudioParameter`.
 *
 * Pattern:
 * - Pattern: Adapter
 * - Problem solved: map custom combo callbacks to normalized parameter writes.
 * - Participants: `FlatComboBox`, `juce::RangedAudioParameter`,
 *   `juce::ParameterAttachment`.
 * - Flow: combo change -> parameter set; parameter update -> combo refresh.
 * - Pitfalls: avoid callback loops (`ignoreCallbacks` guard).
 */
class FlatComboBoxParameterAttachment
{
public:
    /**
     * @brief Construct attachment and bind callback wiring.
     */
    FlatComboBoxParameterAttachment(juce::RangedAudioParameter& parameter,
                                    FlatComboBox& combo,
                                    juce::UndoManager* undoManager = nullptr);
    ~FlatComboBoxParameterAttachment();

    /** @brief Push current parameter value into the combo. */
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
