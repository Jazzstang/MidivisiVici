#pragma once

#include <JuceHeader.h>
#include "MidiMonitor.h"
#include "PluginParameters.h"

class OutputMonitor : public juce::Component
{
public:
    explicit OutputMonitor(juce::AudioProcessorValueTreeState& vts);

    void resized() override;

    /** Ajoute un message au moniteur */
    void addMessage(const juce::MidiMessage& message)
    {
        monitor.addMessage(message);
    }

    /** Lit la FIFO sortie et met à jour le moniteur */
    template <typename Fifo, typename Buffer>
    void updateFromFifo(Fifo& fifo, Buffer& buffer)
    {
        int start1, size1, start2, size2;
        fifo.prepareToRead(512, start1, size1, start2, size2);
        if (size1 > 0)
        {
            for (int i = 0; i < size1; ++i)
                addMessage(buffer[start1 + i]);
        }
        fifo.finishedRead(size1);
    }

private:
    juce::AudioProcessorValueTreeState& parameters;

    MidiMonitor monitor;

    juce::ToggleButton titleButton;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> titleAttachment;

    juce::ToggleButton notesButton, controlsButton, clockButton, eventButton;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> notesAttachment, controlsAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> clockAttachment, eventAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OutputMonitor)
};
