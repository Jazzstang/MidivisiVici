#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "PluginColours.h"
#include "PluginLookAndFeel.h"

//==============================================================================
MidivisiViciAudioProcessorEditor::MidivisiViciAudioProcessorEditor (MidivisiViciAudioProcessor& p)
    : AudioProcessorEditor (p), processor (p)
{
    setLookAndFeel (&customLookAndFeel);
    setSize (300, 300);

    auto& params = processor.parameters;

    // Titre (bouton)
    addAndMakeVisible (monitorTitleButton);
    monitorTitleButton.setButtonText ("Input Monitor");
    showMonitorAttachment.reset (new juce::AudioProcessorValueTreeState::ButtonAttachment (
        params, "showMonitor", monitorTitleButton));

    // Zone moniteur
    addAndMakeVisible (monitorArea);

    // Boutons filtres
    auto initToggle = [&] (juce::ToggleButton& btn, const juce::String& text, const juce::String& paramID, auto& attach)
    {
        addAndMakeVisible (btn);
        btn.setButtonText (text);
        attach.reset (new juce::AudioProcessorValueTreeState::ButtonAttachment (params, paramID, btn));
    };

    initToggle (noteButton, "Note", "filterNote", noteAttachment);
    initToggle (controlButton, "Control", "filterControl", controlAttachment);
    initToggle (clockButton, "Clock", "filterClock", clockAttachment);
    initToggle (eventButton, "Event", "filterEvent", eventAttachment);
}

MidivisiViciAudioProcessorEditor::~MidivisiViciAudioProcessorEditor()
{
    setLookAndFeel (nullptr);
}

void MidivisiViciAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black);
    g.setColour (juce::Colours::white);
}

void MidivisiViciAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced(10);

    // Bandeau en haut
    monitorTitleButton.setBounds(area.removeFromTop(24));

    // Ligne de filtres en bas
    auto filterRow = area.removeFromBottom(24);
    auto w = filterRow.getWidth() / 4;

    noteButton.setBounds(filterRow.removeFromLeft(w).reduced(2));
    controlButton.setBounds(filterRow.removeFromLeft(w).reduced(2));
    clockButton.setBounds(filterRow.removeFromLeft(w).reduced(2));
    eventButton.setBounds(filterRow.reduced(2));

    // Moniteur prend l'espace restant
    monitorArea.setBounds(area.reduced(0, 4));

    // Affichage ou pas du moniteur
    monitorArea.setVisible(monitorTitleButton.getToggleState());
}
