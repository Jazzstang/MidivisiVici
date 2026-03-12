/**
 * @file OutputMonitor.h
 * @brief Output-side MIDI monitor module with filter toggles.
 *
 * Threading:
 * - UI thread owns component state.
 * - FIFO reading is performed from UI ticks (`uiTimerTick`).
 * - Not RT-safe.
 */
#pragma once

#include <JuceHeader.h>
#include <cstdint>
#include "MidiMonitor.h"
#include "MonitorSourceTag.h"
#include "../PluginParameters.h"
#include "../0-component/LeftClickToggleButton.h"
#include "../0-component/ShadowComponent.h"

/**
 * @brief Output monitor panel (enable + type filters + event list).
 *
 * Pattern:
 * - Pattern: Facade
 * - Problem solved: centralize output FIFO pull/filter/render behavior.
 * - Participants: `OutputMonitor`, `MidiMonitor`, APVTS filter params.
 * - Flow: periodic tick -> sync toggles -> read FIFO -> filter -> display.
 * - Pitfalls: keep per-tick FIFO limit bounded to preserve UI responsiveness.
 */
class OutputMonitor : public juce::Component,
                      private juce::Timer
{
public:
    /** @brief Create monitor bound to parameter state and panel title. */
    explicit OutputMonitor(juce::AudioProcessorValueTreeState& vts,
                           const juce::String& title = "Output Monitor");

    void resized() override;
    void paint(juce::Graphics& g) override;
    void uiTimerTick();

    /** @brief Add one message directly (bypasses FIFO filtering). */
    void addMessage(const juce::MidiMessage& message)
    {
        monitor.addMessage(message);
    }

    /** @brief Bind FIFO + backing buffer used during timer ticks. */
    void setFifoSource(juce::AbstractFifo* fifo, juce::MidiMessage* buffer)
    {
        fifoPtr = fifo;
        bufferPtr = buffer;
    }

    /** @brief Bind per-event source metadata arrays aligned with FIFO slots. */
    void setFifoSourceMetadata(std::uint8_t* sourceKinds, std::int8_t* sourceIndices)
    {
        sourceKindPtr = sourceKinds;
        sourceIndexPtr = sourceIndices;
    }

    /**
     * @brief Read a FIFO chunk, apply UI filters, and append accepted messages.
     */
    template <typename Fifo, typename Buffer>
    void updateFromFifo(Fifo& fifo, Buffer& buffer)
    {
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;

        int ready = fifo.getNumReady();
        if (ready <= 0)
            return;

        if (ready > kBacklogDropThreshold)
        {
            const int toDrop = ready - kBacklogKeepAfterDrop;
            drainFifoItems(fifo, juce::jmax(0, toDrop));
            ready = fifo.getNumReady();
        }

        const int toRead = juce::jmin(kMaxMessagesPerTick, ready);
        if (toRead <= 0)
            return;

        fifo.prepareToRead(toRead, start1, size1, start2, size2);

        // ⚠️ IDs de l'OUTPUT monitor (pas ceux de l'INPUT)
        const bool showNotes    = readRawBool(filterNoteRaw, true);
        const bool showControls = readRawBool(filterControlRaw, true);
        const bool showClock    = readRawBool(filterClockRaw, true);
        const bool showEvents   = readRawBool(filterEventRaw, true);

        auto handleRange = [&](int start, int size)
        {
            for (int i = 0; i < size; ++i)
            {
                const int slot = start + i;
                const auto& msg = buffer[slot];
                const std::uint8_t sourceKind = (sourceKindPtr != nullptr)
                                                    ? sourceKindPtr[slot]
                                                    : (std::uint8_t) MonitorSourceKind::Unknown;
                const std::int8_t sourceIndex = (sourceIndexPtr != nullptr)
                                                    ? sourceIndexPtr[slot]
                                                    : (std::int8_t) -1;
                const juce::Colour lineColour =
                    resolveSourceLineColour(sourceKind, sourceIndex);

                // Notes
                if (msg.isNoteOnOrOff())
                {
                    if (showNotes)
                        monitor.addMessage(msg, lineColour);
                    continue;
                }

                // Contrôleurs
                if (msg.isController())
                {
                    if (showControls)
                        monitor.addMessage(msg, lineColour);
                    continue;
                }

                // Clock
                if (msg.isMidiClock())
                {
                    if (showClock)
                        monitor.addMessage(msg, lineColour);
                    continue;
                }

                // Events (y compris Meta Text Events pour debug)
                if (showEvents)
                {
                    monitor.addMessage(msg, lineColour);
                }
            }
        };

        // Traite les DEUX fenêtres
        handleRange(start1, size1);
        handleRange(start2, size2);

        // Marque la lecture totale
        fifo.finishedRead(size1 + size2);
    }

    /** @brief Drain FIFO without rendering events (monitor disabled path). */
    template <typename Fifo>
    void discardFromFifo(Fifo& fifo)
    {
        drainFifoItems(fifo, juce::jmin(kDisabledDrainPerTick, fifo.getNumReady()));
    }
    
private:
    static bool readRawBool(const std::atomic<float>* raw, bool fallback = false) noexcept
    {
        return raw != nullptr ? (raw->load() > 0.5f) : fallback;
    }

    static juce::Colour resolveSourceLineColour(std::uint8_t sourceKindRaw,
                                                std::int8_t sourceIndex) noexcept
    {
        const auto sourceKind = static_cast<MonitorSourceKind>(sourceKindRaw);
        if ((sourceKind == MonitorSourceKind::Lane
             || sourceKind == MonitorSourceKind::MainBarController)
            && sourceIndex >= 0)
        {
            const auto [bg, fg] = PluginColours::getIndexedNameColours((int) sourceIndex);
            juce::ignoreUnused(fg);
            return bg;
        }

        return PluginColours::onSurface;
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

    void syncTogglesFromParameters();

    void timerCallback() override
    {
        uiTimerTick();
    }

    void performTimerTick()
    {
        syncTogglesFromParameters();

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

    MidiMonitor monitor;

    // Header
    std::unique_ptr<ShadowComponent> outputMonitorTitleButtonShadow;

    // Footer
    juce::Rectangle<int> filterBackgroundArea;

    // Boutons
    LeftClickToggleButton outputMonitorTitleButton;
    juce::ToggleButton outputMonitorNotesButton;
    juce::ToggleButton outputMonitorControlsButton;
    juce::ToggleButton outputMonitorClockButton;
    juce::ToggleButton outputMonitorEventButton;

    // FIFO
    juce::AbstractFifo* fifoPtr = nullptr;
    juce::MidiMessage* bufferPtr = nullptr;
    std::uint8_t* sourceKindPtr = nullptr;
    std::int8_t* sourceIndexPtr = nullptr;
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OutputMonitor)
};
