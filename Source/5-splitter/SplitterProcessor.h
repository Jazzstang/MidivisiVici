/**
 * @file SplitterProcessor.h
 * @brief Lane-level splitter routing processor.
 *
 * Threading:
 * - Audio thread for `process`.
 * - RT-safe hot path (no locks/allocations).
 */
//==============================================================================
// SplitterProcessor.h
//------------------------------------------------------------------------------
// Lane-level MIDI splitter engine.
//
// Responsibilities:
// - Route lane notes to up to 5 output branches.
// - Support Round Robin and Range Split modes.
// - Enforce branch channel targets and voice limits.
// - Keep output state coherent on live config changes (no stuck notes).
//
// Real-time contract:
// - process() runs on the audio thread.
// - No locks, no heap allocations in the hot path.
//==============================================================================

#pragma once

#include <JuceHeader.h>

#include <array>
#include <atomic>
#include <cstdint>

#include "../PluginParameters.h"

/**
 * @brief Per-lane splitter processor running on the audio thread.
 *
 * Pattern:
 * - Pattern: Strategy + Diff Engine
 * - Problem solved: dynamic routing with strict output-note coherence.
 * - Participants:
 *   - SplitterProcessor: route decision and output state reconciliation.
 *   - Config snapshot: active branches, channels, limits, ranges, mode.
 *   - desired/output state tables: compute and apply minimal NoteOff/NoteOn diff.
 * - Flow:
 *   1. Read current splitter config.
 *   2. Track source held notes from incoming MIDI.
 *   3. Build desired branch state from mode/rules.
 *   4. Diff desired vs sounding state and emit updates.
 * - Pitfalls:
 *   - Every reroute must emit matching NoteOff for old target before NoteOn.
 *   - Disabled splitter with all branches inactive must produce no note output.
 */
class SplitterProcessor
{
public:
    /**
     * @brief Build one lane splitter processor.
     *
     * Thread: message thread during construction.
     * RT-safe: not required (init only).
     */
    explicit SplitterProcessor(juce::AudioProcessorValueTreeState& vts,
                               Lanes::Lane laneIn);

    ~SplitterProcessor() = default;

    /**
     * @brief Reset all tracked source/desired/output note states.
     *
     * Thread: audio thread or prepare/reset path.
     * RT-safe: yes.
     */
    void resetAllTrackingRT() noexcept;

    /**
     * @brief Process one lane MIDI block in-place.
     *
     * Thread: audio thread.
     * RT-safe: yes (no allocations, no locks, no I/O).
     * Behavior:
     * - Enabled: notes are routed by active splitter branches and target channels.
     * - Disabled: note stream is bypassed (source channel/velocity preserved).
     * - Non-note events are always passed through unchanged.
     *
     * @param midi In/out lane MIDI block.
     */
    void process(juce::MidiBuffer& midi);

private:
    static constexpr int kNumMidiChannels   = 16;
    static constexpr int kNumNotes          = 128;
    static constexpr int kNumBranches       = 5;  // line 1..5
    static constexpr int kNumExtraBranches  = 4;  // line 2..5
    static constexpr int kVelocityStackDepth = 16;
    static constexpr int kNumKeys           = kNumMidiChannels * kNumNotes;

    struct Config
    {
        bool enabled = true;
        int mode = 0; // 0=RoundRobin, 1=RangeSplit

        // Branch index mapping:
        //   0 -> line 1 (direct out)
        //   1 -> line 2
        //   2 -> line 3
        //   3 -> line 4
        //   4 -> line 5
        std::array<bool, kNumBranches> active     { true, false, false, false, false };
        std::array<int,  kNumBranches> channel    { 1, 2, 3, 4, 5 }; // line1 may be 0 (Mute)
        std::array<int,  kNumBranches> voiceLimit { 16, 16, 16, 16, 16 };
        std::array<int,  kNumBranches> noteMin    { 0, 0, 0, 0, 0 };
        std::array<int,  kNumBranches> noteMax    { 127, 127, 127, 127, 127 };

        bool operator==(const Config& other) const noexcept;
        bool operator!=(const Config& other) const noexcept { return !(*this == other); }
    };

    // Helpers
    static inline int keyIndex(int ch0, int note) noexcept
    {
        return ch0 * kNumNotes + note;
    }

    static inline int channelFromKey(int key) noexcept
    {
        return key / kNumNotes;
    }

    static inline int noteFromKey(int key) noexcept
    {
        return key % kNumNotes;
    }

    static bool isNoteOnMessage(const juce::MidiMessage& msg) noexcept;
    static bool isNoteOffMessage(const juce::MidiMessage& msg) noexcept;

    // Read and normalize current APVTS config snapshot for this block.
    Config readConfigRT() const noexcept;

    void handleSourceNoteOn(const Config& cfg,
                            int channel1,
                            int note,
                            uint8_t velocity) noexcept;

    bool handleSourceNoteOff(int channel1, int note) noexcept;

    void ensureRoundRobinAssignments(const Config& cfg);
    int  selectNextRoundRobinBranch(const Config& cfg) noexcept;

    void chooseRoundRobinVoicesForBranch(const Config& cfg, int branchIndex);
    void beginDesiredBuild() noexcept;
    bool isDesiredActiveForCurrentBuild(int branch, int ch0, int note) const noexcept;
    void setDesiredNoteForCurrentBuild(int branch,
                                       int ch0,
                                       int note,
                                       uint8_t velocity,
                                       uint8_t outChannel) noexcept;
    void ensureOutputKeyCache() noexcept;
    void rebuildOutputKeyCache() noexcept;
    // Build desired routing matrix from held source notes + current config.
    void buildDesiredState(const Config& cfg);
    // Emit NoteOff/NoteOn diff and update output state tables.
    void applyDesiredDiff(const Config& cfg, juce::MidiBuffer& out, int samplePos);

    // References
    juce::AudioProcessorValueTreeState& parameters;
    const Lanes::Lane lane;

    // Cached parameter pointers (lane-suffixed IDs).
    std::atomic<float>* splitterEnableRaw = nullptr;
    juce::AudioParameterChoice* splitterModeChoice = nullptr;
    std::atomic<float>* lineActive[kNumBranches] { nullptr, nullptr, nullptr, nullptr, nullptr };
    juce::AudioParameterChoice* line1ChannelChoice = nullptr;

    std::atomic<float>* lineExtraVoiceLimit[kNumExtraBranches] { nullptr, nullptr, nullptr, nullptr };
    juce::AudioParameterInt* lineExtraVoiceLimitParam[kNumExtraBranches] { nullptr, nullptr, nullptr, nullptr };
    std::atomic<float>* lineExtraNoteMin[kNumExtraBranches] { nullptr, nullptr, nullptr, nullptr };
    juce::AudioParameterInt* lineExtraNoteMinParam[kNumExtraBranches] { nullptr, nullptr, nullptr, nullptr };
    std::atomic<float>* lineExtraNoteMax[kNumExtraBranches] { nullptr, nullptr, nullptr, nullptr };
    juce::AudioParameterInt* lineExtraNoteMaxParam[kNumExtraBranches] { nullptr, nullptr, nullptr, nullptr };
    std::atomic<float>* lineExtraChannel[kNumExtraBranches] { nullptr, nullptr, nullptr, nullptr };
    juce::AudioParameterInt* lineExtraChannelParam[kNumExtraBranches] { nullptr, nullptr, nullptr, nullptr };

    // Source held-note state (input seen by splitter).
    std::array<std::array<uint8_t,  kNumNotes>, kNumMidiChannels> sourceHeldCount {};
    std::array<std::array<uint8_t,  kNumNotes>, kNumMidiChannels> sourceHeldVelocity {};
    std::array<std::array<std::array<uint8_t, kVelocityStackDepth>, kNumNotes>, kNumMidiChannels> sourceVelocityStack {};
    std::array<std::array<uint8_t,  kNumNotes>, kNumMidiChannels> sourceVelocityStackSize {};
    std::array<std::array<uint64_t, kNumNotes>, kNumMidiChannels> sourceTimestamp {};

    // Round-robin assignment per held source key (0 = none, 1..4 = line2..5).
    std::array<std::array<uint8_t, kNumNotes>, kNumMidiChannels> sourceAssignedBranch {};

    uint64_t timestampCounter = 0;
    int roundRobinCursor = 0; // next extra branch search start: 0..3

    // Desired state scratch (rebuilt on each diff pass with generation stamps).
    std::array<std::array<std::array<uint16_t, kNumNotes>, kNumMidiChannels>, kNumBranches> desiredStamp {};
    std::array<std::array<std::array<uint8_t, kNumNotes>, kNumMidiChannels>, kNumBranches> desiredVelocity {};
    std::array<std::array<std::array<uint8_t, kNumNotes>, kNumMidiChannels>, kNumBranches> desiredChannel {};
    uint16_t desiredBuildGeneration = 1;
    std::array<std::array<int, kNumKeys>, kNumBranches> desiredKeys {};
    std::array<int, kNumBranches> desiredKeyCount {};

    // Currently sounding output state.
    std::array<std::array<std::array<uint8_t, kNumNotes>, kNumMidiChannels>, kNumBranches> outputActive {};
    std::array<std::array<std::array<uint8_t, kNumNotes>, kNumMidiChannels>, kNumBranches> outputVelocity {};
    std::array<std::array<std::array<uint8_t, kNumNotes>, kNumMidiChannels>, kNumBranches> outputChannel {};
    std::array<std::array<int, kNumKeys>, kNumBranches> outputKeys {};
    std::array<int, kNumBranches> outputKeyCount {};
    std::array<std::array<int, kNumKeys>, kNumBranches> outputKeysScratch {};
    bool outputKeyCacheValid = true;

    // Scratch
    juce::MidiBuffer outScratch;
    std::array<int, kNumKeys> candidateKeys {};

    // Last block config (to detect live changes with held notes).
    Config lastConfig {};
    bool hasLastConfig = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SplitterProcessor)
};
