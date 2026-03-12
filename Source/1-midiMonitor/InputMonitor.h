/**
 * @file InputMonitor.h
 * @brief Input-side MIDI monitor module with filter toggles.
 *
 * Threading:
 * - UI thread owns component state.
 * - FIFO reading is performed from UI ticks (`uiTimerTick`).
 * - Not RT-safe.
 */
#pragma once

#include <JuceHeader.h>
#include "MidiMonitor.h"
#include "../PluginParameters.h"
#include "../0-component/LeftClickToggleButton.h"
#include "../0-component/ShadowComponent.h"

/**
 * @brief Input monitor panel (enable + type filters + event list).
 *
 * Pattern:
 * - Pattern: Facade
 * - Problem solved: centralize FIFO pull, filter policy, and monitor display.
 * - Participants: `InputMonitor`, `MidiMonitor`, APVTS filter params.
 * - Flow: periodic tick -> read FIFO chunk -> filter by toggles -> append rows.
 * - Pitfalls: do not read unbounded FIFO batches on message thread.
 */
class InputMonitor : public juce::Component,
                     private juce::Timer
{
public:
    /** @brief Create monitor bound to plugin parameter state. */
    explicit InputMonitor(juce::AudioProcessorValueTreeState& vts);

    void resized() override;
    void paint(juce::Graphics& g) override;
    void uiTimerTick();

    /** @brief Add one message directly (bypasses FIFO filtering). */
    void addMessage(const juce::MidiMessage& message)
    {
        monitor.addMessage(message);
    }

    /**
     * @brief Read a FIFO chunk, apply UI filters, and append accepted messages.
     */
    template <typename Fifo, typename Buffer>
    void updateFromFifo(Fifo& fifo, Buffer& buffer)
    {
        int ready = fifo.getNumReady();
        if (ready <= 0)
            return; // rien à lire

        if (ready > kBacklogDropThreshold)
        {
            const int toDrop = ready - kBacklogKeepAfterDrop;
            drainFifoItems(fifo, juce::jmax(0, toDrop));
            ready = fifo.getNumReady();
        }

        const int toRead = juce::jmin(kMaxMessagesPerTick, ready);
        if (toRead <= 0)
            return; // rien à lire

        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        fifo.prepareToRead(toRead, start1, size1, start2, size2);

        const bool showNotes    = readRawBool(filterNoteRaw, true);
        const bool showControls = readRawBool(filterControlRaw, true);
        const bool showClock    = readRawBool(filterClockRaw, true);
        const bool showEvents   = readRawBool(filterEventRaw, true);

        auto handle = [&](int start, int size)
        {
            for (int i = 0; i < size; ++i)
            {
                const auto& msg = buffer[start + i];

                if (msg.isNoteOnOrOff() && !showNotes) continue;
                if (msg.isController()  && !showControls) continue;
                if (msg.isMidiClock()   && !showClock) continue;
                if (!(msg.isNoteOnOrOff() || msg.isController() || msg.isMidiClock()) && !showEvents) continue;

                monitor.addMessage(msg);
            }
        };

        handle(start1, size1);
        handle(start2, size2);

        fifo.finishedRead(size1 + size2);
    }

    /** @brief Drain FIFO without rendering events (monitor disabled path). */
    template <typename Fifo>
    void discardFromFifo(Fifo& fifo)
    {
        drainFifoItems(fifo, juce::jmin(kDisabledDrainPerTick, fifo.getNumReady()));
    }

    /** @brief Bind FIFO + backing buffer used during timer ticks. */
    void setFifoSource(juce::AbstractFifo* fifo, juce::MidiMessage* buffer)
    {
        fifoPtr = fifo;
        bufferPtr = buffer;
    }

private:
    static bool readRawBool(const std::atomic<float>* raw, bool fallback = false) noexcept
    {
        return raw != nullptr ? (raw->load() > 0.5f) : fallback;
    }

    template <typename Fifo>
    static void drainFifoItems(Fifo& fifo, int itemsToDrain)
    {
        int remaining = juce::jmax(0, itemsToDrain);
        while (remaining > 0)
        {
            int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
            const int chunk = juce::jmin(kFifoDrainChunk, remaining);
            fifo.prepareToRead(chunk, start1, size1, start2, size2);

            const int drained = size1 + size2;
            fifo.finishedRead(drained);

            if (drained <= 0)
                break;

            remaining -= drained;
        }
    }

    // --- Timer ---
    void timerCallback() override
    {
        uiTimerTick();
    }

    void performTimerTick()
    {
        if (fifoPtr != nullptr && bufferPtr != nullptr)
        {
            if (readRawBool(monitorEnabledRaw, true))
                updateFromFifo(*fifoPtr, bufferPtr);
            else
                discardFromFifo(*fifoPtr);
        }

        // Flush texte au plus une fois par tick, pour éviter les rebuilds coûteux
        // à chaque message.
        if (readRawBool(monitorEnabledRaw, true))
            monitor.flushPendingDisplay();
    }

    juce::AudioProcessorValueTreeState& parameters;

    // Zone centrale affichant les messages MIDI
    MidiMonitor monitor;

    // Header (avec bouton enfant)
    std::unique_ptr<ShadowComponent> inputMonitorTitleButtonShadow;

    // Footer (fond pilule derrière les filtres)
    juce::Rectangle<int> filterBackgroundArea;

    // Boutons
    LeftClickToggleButton inputMonitorTitleButton;
    juce::ToggleButton inputMonitorNotesButton;
    juce::ToggleButton inputMonitorControlsButton;
    juce::ToggleButton inputMonitorClockButton;
    juce::ToggleButton inputMonitorEventButton;

    // Référence vers la FIFO source
    juce::AbstractFifo* fifoPtr = nullptr;
    juce::MidiMessage* bufferPtr = nullptr;
    std::atomic<float>* monitorEnabledRaw = nullptr;
    std::atomic<float>* filterNoteRaw = nullptr;
    std::atomic<float>* filterControlRaw = nullptr;
    std::atomic<float>* filterClockRaw = nullptr;
    std::atomic<float>* filterEventRaw = nullptr;

    static constexpr int kMaxMessagesPerTick = 12;
    static constexpr int kBacklogDropThreshold = kMaxMessagesPerTick * 6;
    static constexpr int kBacklogKeepAfterDrop = kMaxMessagesPerTick * 2;
    static constexpr int kDisabledDrainPerTick = 256;
    static constexpr int kFifoDrainChunk = 64;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(InputMonitor)
};
