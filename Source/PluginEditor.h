#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PluginColours.h"
#include "PluginLookAndFeel.h"
#include "MidiMonitor.h"
#include "InputMonitor.h"
#include "InputFilter.h"
#include "TransformBase.h"
#include "DivisiBase.h"
#include "OutputMonitor.h"

/**
 * Editeur principal
 * ------------------
 * Contient :
 * - 5 colonnes (Input Monitor, Input Filter, Transform, Divisi, Output Monitor)
 * - Les ToggleButtons de titre pour Transform et Divisi
 * - Les composants modulaires pour Input Monitor, Input Filter et Output Monitor
 */
class MidivisiViciAudioProcessorEditor
    : public juce::AudioProcessorEditor,
      private juce::Timer
{
public:
    MidivisiViciAudioProcessorEditor(MidivisiViciAudioProcessor&);
    ~MidivisiViciAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    MidivisiViciAudioProcessor& processor;

    // === LookAndFeel local ===
    PluginLookAndFeel lookAndFeel;

    bool notesState = false;
    bool controlsState = false;
    bool clockState = false;
    bool eventsState = false;

    juce::Image noiseImage;
    int noiseWidth = 0;
    int noiseHeight = 0;

    void generateNoiseImage(int width, int height);
    void timerCallback() override;

    // === Colonne 1 : Input Monitor (modulaire, avec ses filtres internes) ===
    InputMonitor inputMonitor;

    // === Colonne 2 : Input Filter (entièrement géré dans InputFilter) ===
    InputFilter inputFilterContent;

    // === Colonne 3 : Transform ===
    TransformBase transformContent;

    // === Colonne 4 : Divisi ===
    DivisiBase divisiContent;

    // === Colonne 5 : Output Monitor (modulaire, avec ses filtres internes) ===
    OutputMonitor outputMonitor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidivisiViciAudioProcessorEditor)
};
