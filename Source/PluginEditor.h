#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PluginLookAndFeel.h"
#include "PluginColours.h"

//==============================================================================
class MidivisiViciAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    explicit MidivisiViciAudioProcessorEditor (MidivisiViciAudioProcessor&);
    ~MidivisiViciAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    PluginLookAndFeel customLookAndFeel;
    MidivisiViciAudioProcessor& processor;

    // Bandeau titre (bouton)
    juce::ToggleButton monitorTitleButton;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> showMonitorAttachment;

    // Zone du moniteur MIDI
    juce::Component monitorArea;

    // Filtres en ligne
    juce::ToggleButton noteButton;
    juce::ToggleButton controlButton;
    juce::ToggleButton clockButton;
    juce::ToggleButton eventButton;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> noteAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> controlAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> clockAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> eventAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidivisiViciAudioProcessorEditor)
};
