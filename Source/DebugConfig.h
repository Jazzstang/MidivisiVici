/**
 * @file DebugConfig.h
 * @brief Project logging switches and hybrid logger declaration.
 *
 * Threading:
 * - Logging API may be called from multiple threads.
 * - Not RT-safe for high-frequency audio-thread usage.
 */
#pragma once
#include <JuceHeader.h>
#include <os/log.h>
#include <iostream>

// =====================================
// Global log switch
// =====================================
#define LOGS_ENABLED 0  // 0 = disable all logs, 1 = enable per-module switches

// =====================================
// Per-module log switches
// =====================================

// MIDI & Debug
#define LOG_DEBUG_SYSEX              1

// Components
#define LOG_CUSTOM_ROTARY_NOCOMBO    1
#define LOG_CUSTOM_ROTARY_WITHCOMBO  1
#define LOG_FLAT_COMBOBOX            1
#define LOG_LABELED_SLIDER           1
#define LOG_LFO_COMPONENT            1
#define LOG_NOTE_OFF_TRACKER         1
#define LOG_SHADOW_COMPONENT         1

// Monitors
#define LOG_INPUT_MONITOR            1
#define LOG_OUTPUT_MONITOR           1
#define LOG_MIDI_MONITOR             1

// Filters & Processors
#define LOG_INPUTFILTER              1
#define LOG_INPUTFILTER_PROCESSOR    1
#define LOG_HARMONIZER               1
#define LOG_ARPEGGIATOR              1

// Step & Sequencer
#define LOG_STEP_TOGGLE              1
#define LOG_LINK_TOGGLE              1
#define LOG_STEP_SEQUENCER_GRID      1

// Splitter
#define LOG_SPLIT_LINE_TOGGLE        1
#define LOG_SPLITTER                 1
#define LOG_SPLITTER_LINE_COMPONENT  1
#define LOG_SPLITTER_MODE_TOGGLE     1

// LFO & Main bar
#define LOG_LFO_GROUP                1
#define LOG_MAIN_BAR                 1

// Plugin Core
#define LOG_PLUGIN_PROCESSOR         1
#define LOG_PLUGIN_EDITOR            1
#define LOG_PRESETMANAGER           1
#define LOG_SNAPSHOT_RECALL_ASYNC   1

// =====================================
// HybridLogger declaration
// =====================================
/** @brief Logger that mirrors output to JUCE logger and optional file stream. */
struct HybridLogger : public juce::Logger
{
    HybridLogger();
    void logMessage(const juce::String& message) override;

    static void logFormatted(const juce::String& type,
                             const juce::String& module,
                             const juce::String& context,
                             const juce::String& id,
                             const juce::String& message);

private:
    juce::File logFile;
    std::unique_ptr<juce::FileOutputStream> fileStream;
};

#if JUCE_DEBUG
#undef DBG
#define DBG(msg) juce::Logger::writeToLog(msg)

#define DBG_LOG(type, module, context, id, message) \
    HybridLogger::logFormatted(type, module, context, id, message)
#endif
