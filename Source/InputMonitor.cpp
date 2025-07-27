#include "InputMonitor.h"
#include "PluginParameters.h"

InputMonitor::InputMonitor(juce::AudioProcessorValueTreeState& vts)
    : parameters(vts), monitor()
{
    // --- Bouton titre ---
    titleButton.setButtonText("Input Monitor");
    addAndMakeVisible(titleButton);
    titleAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        parameters, ParamIDs::inputMonitorEnable, titleButton);

    // --- Filtres ---
    notesButton.setButtonText("Notes");
    notesButton.setName("monitorFooter");
    addAndMakeVisible(notesButton);
    notesAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        parameters, ParamIDs::inputMonitorFilterNote, notesButton);

    controlsButton.setButtonText("Controls");
    controlsButton.setName("monitorFooter");
    addAndMakeVisible(controlsButton);
    controlsAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        parameters, ParamIDs::inputMonitorFilterControl, controlsButton);

    clockButton.setButtonText("Clock");
    clockButton.setName("monitorFooter");
    addAndMakeVisible(clockButton);
    clockAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        parameters, ParamIDs::inputMonitorFilterClock, clockButton);

    eventButton.setButtonText("Event");
    eventButton.setName("monitorFooter");
    addAndMakeVisible(eventButton);
    eventAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        parameters, ParamIDs::inputMonitorFilterEvent, eventButton);
    
    addAndMakeVisible(monitor);
}

void InputMonitor::resized()
{
    auto area = getLocalBounds();
    auto titleArea = area.removeFromTop(24);
    titleButton.setBounds(titleArea);

    auto footerArea = area.removeFromBottom(20);
    const int w = footerArea.getWidth() / 4;
    notesButton.setBounds(footerArea.removeFromLeft(w));
    controlsButton.setBounds(footerArea.removeFromLeft(w));
    clockButton.setBounds(footerArea.removeFromLeft(w));
    eventButton.setBounds(footerArea);

    monitor.setBounds(area);
}
