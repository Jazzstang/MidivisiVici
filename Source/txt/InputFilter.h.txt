// =============================
// InputFilter.h
// =============================
#pragma once

#include <JuceHeader.h>
#include "TwoLineToggleButton.h"
#include "LabeledSlider.h"
#include "FlatComboBox.h"

/**
 * Composant graphique représentant le module Input Filter.
 * Dispose d’un ensemble de sliders, toggles et menus permettant de filtrer les messages MIDI entrants :
 * - Mute
 * - Plage de notes
 * - Plage de vélocité
 * - Canal MIDI
 * - Step Filter (N sur M)
 * - Voice Limit
 * - Priorité
 */

class InputFilter : public juce::Component,
                    private juce::Timer
{
public:
    InputFilter(juce::AudioProcessorValueTreeState& state);
    ~InputFilter() override;

    void resized() override;
    void setLine1Bounds(juce::Rectangle<int> area);

    bool learnModeActive = false;
    int learnedCC = -1;
    int learnedChannel = -1;

private:
    void timerCallback() override;

    juce::AudioProcessorValueTreeState& parameters;

    // === Header Toggle ===
    juce::ToggleButton inputFilterTitleButton { "Input Filter" };

    // === Toggles ===
    TwoLineToggleButton noteFilterToggle { "Note", "Filter" };
    TwoLineToggleButton velocityFilterToggle { "Velocity", "Filter" };
    TwoLineToggleButton voiceLimitToggle { "Voice", "Limit" };
    TwoLineToggleButton stepFilterToggle { "Step", "Filter" };

    // === Ligne 1 ===
    juce::Label midiChannelLabel;
    FlatComboBox channelModeSelector;
    juce::ToggleButton muteButton;
    juce::TextButton learnCCButton;
    juce::Label ccLearnStatusLabel;

    // === Sliders ===
    LabeledSlider noteSlider;
    LabeledSlider velocitySlider;
    LabeledSlider stepSlider;
    LabeledSlider voiceSlider;

    // === Labels / Combos ===
    juce::Label stepFilterLabel;
    juce::Label voiceLimitLabel;
    juce::Label priorityLabel;
    FlatComboBox priorityBox;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(InputFilter)
};
