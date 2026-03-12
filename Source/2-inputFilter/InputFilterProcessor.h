/**
 * @file InputFilterProcessor.h
 * @brief Lane-level MIDI input filtering processor.
 *
 * Threading:
 * - Audio thread for `process`.
 * - No locks, allocations, or blocking I/O in RT path.
 */
//==============================================================================
// InputFilterProcessor.h
// -----------------------------------------------------------------------------
// Module DSP/MIDI: InputFilterProcessor (lane-based)
//
// PEDAGOGICAL OVERVIEW
// --------------------
// This processor implements the "Input Filter" stage for one lane (A..P).
//
// It is responsible for:
//   - Filtering incoming MIDI Note events in a strict order.
//   - Tracking "held notes" (notes that passed all filters at NoteOn time).
//   - Deriving an "active voices" subset from held notes (voice limit + priority).
//   - Emitting a deterministic diff (NoteOff then NoteOn) when active voices change.
//
// IMPORTANT: This processor can *remove* and *re-emit* note events.
// If the internal bookkeeping is wrong, the result is typically "stuck notes".
// Therefore, the core contract is:
//   - Every NoteOn that is allowed into heldNotes must be matched by a NoteOff
//     that removes it (channel change safety is handled explicitly).
//
// STRICT FILTER ORDER (requested)
//   1) Channel filter (NoteOn only, strict)
//   2) Mute (NoteOn only)
//   3) Note range (NoteOn only)
//   4) Velocity range (NoteOn only)
//   5) Step filter (NoteOn only, last gate)
//   6) Voice limit + priority reconciliation (derived from heldNotes)
//
// VOICE LIMIT INVARIANT
//   - heldNotes is the authoritative logical set.
//   - activeVoices is always derived from heldNotes after applying:
//       * voiceLimitToggle / voiceLimit
//       * priority mode (Last / Lowest / Highest)
//   - Any activeVoices transition is emitted as deterministic diff:
//       * NoteOff first (removed voices)
//       * NoteOn after (added voices, unless explicitly disabled)
//
// STEP FILTER INVARIANT
//   - Step filter is a NoteOn-only gate. It never directly emits NoteOff.
//   - NoteOff lifetime safety remains driven by heldNotes/activeVoices model.
//   - Editing step parameters at runtime resets step phase/counter only.
//   - In hold mode, step edits are part of "input-filter changed" policy and
//     therefore can trigger a full held/active reset (by design).
//
// NOTE SEMANTICS (critical for correctness)
//   - JUCE velocity API differs by version:
//       * getVelocity() may return uint8 [0..127] or float [0..1]
//       * conversion helpers below normalize both cases
//   - Some devices send "NoteOn velocity=0" to mean NoteOff.
//   - Our .cpp treats NoteOn vel==0 as NoteOff.
//
// VELOCITY FILTER INVARIANT
//   - Velocity gating is evaluated at accepted NoteOn time and stored in HeldNote.
//   - Runtime parameter changes (velocity min/max, toggle) can only prune currently
//     held notes from that stored state; they do not recreate notes that were never
//     accepted by the lane filter pipeline.
//
// TRANSPORT INTEGRATION (requested)
//   - onPlaybackStarted(): resets counters/guards and arms held-note retrigger
//     on next process block (clean START when notes are already held).
//   - onPlaybackStopped(): arms a single one-shot flush + held-state reset
//     (cuts hold-latched notes on STOP).
//
// Channel-change safety (requested)
//   - NoteOn is filtered by selected channel (strict).
//   - NoteOff is accepted if it matches an existing held note,
//     even if its channel does not match the currently selected channel.
//   - Additionally, if the NoteOff arrives on the "new" channel but the note
//     is held on an "old" channel, we still accept it by matching note number
//     across any held channel (best-effort fix for real-world controllers).
//
// Consume routing support (project topology)
//   - ParamIDs::Base::inputFilterConsume is lane-based.
//   - ParamIDs::Base::inputFilterDirect is lane-based and handled by router.
//   - Effective policy:
//       consume is forced OFF while stepFilterToggle is ON for the same lane.
//   - This processor exposes isConsumeEnabled() for the router.
//   - This processor itself does NOT perform routing; PluginProcessor does.
//   - On routing source edges (direct topology changes), PluginProcessor
//     resynchronizes lane held snapshots and asks this processor to reconcile.
//
// Hold mode support
//   - ParamIDs::Base::inputFilterHold is lane-based.
//   - If ON, NoteOff events do not release held notes.
//   - Held notes are internally split between:
//       * physically-held (real key still down for this lane)
//       * latched-only (kept alive by hold after key release)
//   - On the next accepted physical NoteOn while hold is ON, latched-only
//     notes are dropped so the new physical gesture takes ownership.
//   - Held notes are explicitly cleared when hold is disabled or when another
//     InputFilter parameter changes while hold is ON.
//
// RT SAFETY
//   - No allocations in process().
//   - We reuse scratch buffers reserved in ctor.
//   - Only cached APVTS atomic pointers are read on the audio thread.
//==============================================================================

#pragma once

#include <JuceHeader.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

#include "../PluginParameters.h"
#include "../DebugConfig.h"

class MidivisiViciAudioProcessor;

//==============================================================================
// HeldNote
//------------------------------------------------------------------------------
// A note currently held by the user (logical view).
//
// Notes:
//  - channel is 1..16 (JUCE uses 1-based channels).
//  - velocity is MIDI 0..127 (we store the integer form, not JUCE's float).
//  - timestampOn is a monotonic counter used for "Last" priority ordering.
//==============================================================================
/**
 * @brief One held source note tracked by InputFilterProcessor.
 *
 * Invariant:
 * - channel is 1..16 (JUCE channel numbering).
 * - velocity is MIDI integer 0..127.
 * - timestampOn is monotonic per lane and used by "Last" priority.
 */
struct HeldNote
{
    int      channel     = 1;   // 1..16
    int      note        = 0;   // 0..127
    int      velocity    = 0;   // 0..127 (MIDI integer)
    uint64_t timestampOn = 0;   // monotonic "arrival" ordering
};

//==============================================================================
// InputFilterProcessor
//==============================================================================

/**
 * @brief Per-lane input filter processor running on the audio thread.
 *
 * Pattern:
 * - Pattern: Pipeline + State Reconciler
 * - Problem solved: apply ordered note filters while keeping note lifetime
 *   coherent when parameters change in real time.
 * - Participants:
 *   - InputFilterProcessor: filtering, held state, active voice diff emission.
 *   - APVTS atomics: lane parameter snapshot for this block.
 *   - heldNotes/activeVoices: runtime ownership for note lifetime.
 * - Flow:
 *   1. Apply ordered NoteOn gates (channel -> mute -> ranges -> step).
 *   2. Track held notes independent from transient block ordering.
 *   3. Recompute active voices (limit + priority) and emit NoteOff/NoteOn diff.
 * - Pitfalls:
 *   - NoteOff must always release previously emitted notes (channel safety).
 *   - Parameter changes must prune/reconcile state without leaving stuck notes.
 */
class InputFilterProcessor
{
public:
    /**
     * @brief Build one lane input-filter processor.
     *
     * Thread: message thread during construction.
     * RT-safe: not required (init only).
     */
    InputFilterProcessor(MidivisiViciAudioProcessor& processorRef,
                         juce::AudioProcessorValueTreeState& vts,
                         Lanes::Lane laneIn);

    /**
     * @brief Process one lane MIDI block in-place.
     *
     * Thread: audio thread.
     * RT-safe: yes (no allocations, no locks, no I/O).
     * Order:
     * - Applies the strict NoteOn filter order documented in this file header.
     * - NoteOff path prioritizes note lifetime correctness over current channel
     *   filter to avoid stale held notes.
     *
     * @param midiMessages In/out lane MIDI block.
     */
    void process(juce::MidiBuffer& midiMessages);

    /**
     * @brief Notify transport start edge to the processor.
     *
     * Thread: audio thread.
     * RT-safe: yes.
     */
    void onPlaybackStarted();

    /**
     * @brief Notify transport stop edge to the processor.
     *
     * Thread: audio thread.
     * RT-safe: yes.
     */
    void onPlaybackStopped();

    /**
     * @brief Hard reset of held and active runtime state.
     *
     * Thread: audio thread or prepare/reset path.
     * RT-safe: yes.
     */
    void resetAllTrackingRT() noexcept;

    /**
     * @brief Emit NoteOff for active voices and clear runtime state.
     *
     * Thread: audio thread.
     * RT-safe: yes.
     *
     * @param out Destination MIDI buffer.
     * @param samplePos Sample position for emitted NoteOffs.
     * @param releasedActiveVoices Optional copy of released voices.
     * @return Number of emitted NoteOff messages.
     */
    int forceAllNotesOffAndClear(juce::MidiBuffer& out,
                                 int samplePos,
                                 std::array<HeldNote, 16>* releasedActiveVoices = nullptr) noexcept;

    /**
     * @brief Rebuild held/active state from external lane-held snapshot.
     *
     * Thread: audio thread.
     * RT-safe: yes.
     * Usage:
     * - Called when upstream routing topology changed and this lane must
     *   synchronize to externally maintained held-note truth.
     */
    void resyncFromExternalHeldState(const std::array<std::array<int, 128>, 16>& heldCounts,
                                     const std::array<std::array<int, 128>, 16>& heldVelocity,
                                     const std::array<std::array<uint64_t, 128>, 16>& heldOrder,
                                     juce::MidiBuffer& out,
                                     int samplePos);

    /**
     * @brief Return lane consume toggle state.
     *
     * Thread: audio thread or routing path.
     * RT-safe: yes (atomic read only).
     *
     * The lane router uses this to choose raw feed vs remainder feed.
     */
    bool isConsumeEnabled() const noexcept
    {
        const bool consumeRequested = (consume && consume->load() > 0.5f);
        const bool stepFilterOn = (stepFilterEnabled && stepFilterEnabled->load() > 0.5f);
        return (consumeRequested && !stepFilterOn);
    }

    /**
     * @brief Return whether a given source key is held in this lane.
     *
     * Thread: audio thread / routing path.
     * RT-safe: yes.
     */
    bool isHeldNoteForRouting(int ch, int note) const noexcept
    {
        return isHeldNote(ch, note);
    }

private:
    //==========================================================================
    // References (owned elsewhere)
    //==========================================================================
    MidivisiViciAudioProcessor&         processor;
    juce::AudioProcessorValueTreeState& parameters;

    // Lane identity (A..P)
    const Lanes::Lane lane;

    //==========================================================================
    // Cached parameter pointers (APVTS atomics)
    //
    // IMPORTANT:
    // - All IDs must be lane-suffixed (ParamIDs::lane(Base::<id>, lane)).
    //==========================================================================
    std::atomic<float>* channel               = nullptr; // 1..16 stored as float
    std::atomic<float>* mute                  = nullptr; // bool

    // Consume routing toggle (lane-based)
    std::atomic<float>* consume               = nullptr; // bool
    std::atomic<float>* direct                = nullptr; // bool (routing)
    std::atomic<float>* hold                  = nullptr; // bool (latch mode)

    std::atomic<float>* noteFilterEnabled     = nullptr; // bool
    std::atomic<float>* noteMin               = nullptr; // 0..127
    std::atomic<float>* noteMax               = nullptr; // 0..127

    std::atomic<float>* velocityFilterEnabled = nullptr; // bool
    std::atomic<float>* velocityMin           = nullptr; // 0..127
    std::atomic<float>* velocityMax           = nullptr; // 0..127

    std::atomic<float>* stepFilterEnabled     = nullptr; // bool
    std::atomic<float>* stepFilterNumerator   = nullptr; // 1..16
    std::atomic<float>* stepFilterDenominator = nullptr; // 1..16

    std::atomic<float>* voiceLimitEnabled     = nullptr; // bool
    std::atomic<float>* voiceLimit            = nullptr; // 1..16

    // Priority parameter:
    // - priorityRawValue is used on RT path (atomic float index).
    // - priorityChoice is kept for UI-readable text/debug naming.
    std::atomic<float>* priorityRawValue      = nullptr; // choice index 0..2
    juce::AudioParameterChoice* priorityChoice = nullptr;

    //==========================================================================
    // Runtime state (RT-owned)
    //==========================================================================
    std::vector<HeldNote> heldNotes;      // passed all filters on NoteOn
    std::vector<HeldNote> activeVoices;   // derived subset (priority + voice limit)
    std::array<std::array<uint16_t, 128>, 16> physicalHeldCount {}; // key-down tracker

    uint64_t timestampCounter = 0;        // monotonic counter for "Last" ordering
    // Step filter runtime state:
    // - currentStepCount is an absolute counter used for debug/trace readability.
    // - currentStepPhase is the modulo position in [0..stepDen-1] used in RT.
    //
    // We keep both to avoid signed overflow UB and to avoid per-event modulo
    // on a potentially large counter when processing dense MIDI bursts.
    uint64_t currentStepCount = 0;
    int      currentStepPhase = 0;

    // One-shot STOP flush behavior
    bool pendingAllNotesOff   = false;    // armed on STOP, consumed once
    bool hostIsStopped        = false;    // transport state
    bool didFlushOnceThisStop = false;    // prevents repeated flushes while stopped
    bool pendingHeldRetrigOnStart = false; // armed on START if held notes exist
    // Fast-path hint for hold mode:
    // true means heldNotes may contain latched-only notes (not physically held).
    // This avoids scanning heldNotes on every accepted NoteOn when hold is ON.
    // Implementation note:
    // - This flag is intentionally fail-safe: in ambiguous NoteOff ownership
    //   cases we may set it to true even if uncertain, because the consequence
    //   is only an extra bounded scan on next accepted NoteOn.
    bool hasLatchedOnlyHeldNotes = false;

    //==========================================================================
    // Scratch buffers (avoid per-block allocations)
    //==========================================================================
    juce::MidiBuffer outScratch;

    std::vector<HeldNote> oldVoicesScratch;
    std::vector<HeldNote> newVoicesScratch;

    // Last block snapshot for parameter-change resync logic.
    bool lastSnapshotValid = false;
    int  lastSelectedChannel = 1;
    bool lastMuteActive      = false;
    bool lastNoteF           = false;
    int  lastNMin            = 0;
    int  lastNMax            = 127;
    bool lastVeloF           = false;
    int  lastVMin            = 0;
    int  lastVMax            = 127;
    bool lastStepF           = false;
    int  lastStepNum         = 1;
    int  lastStepDen         = 1;
    bool lastLimitEnabled    = false;
    int  lastMaxVoices       = 16;
    int  lastPriority        = 0;
    // Cached effective consume routing state (already forced OFF if step filter ON).
    bool lastConsumeRouting  = false;
    bool lastDirectRouting   = false;
    bool lastHoldEnabled     = false;

    //==========================================================================
    // Internal helpers (state mutation)
    //==========================================================================
    void addToHeldNotes(int ch, int note, int vel);
    void removeFromHeldNotes(int ch, int note);

    // True if this exact (ch,note) is currently held.
    bool isHeldNote(int ch, int note) const noexcept;

    // Channel-change safety helper:
    // Try to find a held note by note number on ANY channel.
    // Returns true and writes the matched channel into outCh if found.
    bool findHeldNoteUniqueAnyChannel(int note, int& outCh) const noexcept;

    // Physical key-state tracker helpers.
    void markPhysicalNoteOn(int ch, int note) noexcept;
    void markPhysicalNoteOff(int ch, int note) noexcept;
    bool isPhysicallyHeld(int ch, int note) const noexcept;
    bool findPhysicalNoteUniqueAnyChannel(int note, int& outCh) const noexcept;

    // Remove hold-latched notes that are no longer physically held.
    void dropLatchedOnlyHeldNotes() noexcept;

    // Recompute active voices from heldNotes and emit diffs.
    // If emitAddedVoices is false, only NoteOff for removed voices is emitted.
    // This mode is used to keep stop-state edits deterministic without creating
    // fresh note attacks while transport is stopped.
    void recalculateActiveVoices(juce::MidiBuffer& out,
                                 int samplePos,
                                 bool emitAddedVoices = true);

    // Internal policy version used by process() to reuse block-snapshot values
    // of voice-limit parameters and avoid repeated atomic reads per event.
    void recalculateActiveVoicesWithPolicy(juce::MidiBuffer& out,
                                           int samplePos,
                                           bool emitAddedVoices,
                                           bool limitEnabled,
                                           int maxVoices,
                                           int priorityIndex);

    // Emit an explicit retrigger for currently active voices (NoteOff then NoteOn).
    // Used on transport START when notes were already held before start.
    void emitActiveVoicesRetrigger(juce::MidiBuffer& out, int samplePos);

    // Prune heldNotes against current "gate" parameters (mute/channel/ranges).
    // Step filter is intentionally excluded (NoteOn-only gate).
    // Returns true if the held set was modified.
    bool pruneHeldNotesForCurrentGate(int selectedChannel,
                                      bool muteActive,
                                      bool noteF,
                                      int nMin,
                                      int nMax,
                                      bool veloF,
                                      int vMin,
                                      int vMax) noexcept;

    // Step-filter decision helper (RT-safe):
    // - Advances step state for each candidate NoteOn that reached step gate.
    // - Returns true when this step is accepted, false when skipped.
    // - stepNum is 1-based (UI model), stepDen is >= 1.
    bool shouldPassStepFilterAndAdvance(int stepNum, int stepDen) noexcept;

    // Emit NoteOffs for removed voices, then optionally NoteOns for newly activated ones.
    void diffAndEmit(const std::vector<HeldNote>& oldVoices,
                     const std::vector<HeldNote>& newVoices,
                     juce::MidiBuffer& out,
                     int samplePos,
                     bool emitAddedVoices = true);

    //==========================================================================
    // Priority helpers
    //==========================================================================
    int getPriorityIndex() const noexcept
    {
        if (priorityRawValue != nullptr)
            return juce::jlimit(0, 2, (int) priorityRawValue->load());

        return priorityChoice ? juce::jlimit(0, 2, priorityChoice->getIndex()) : 0;
    }

    juce::String getPriorityText() const
    {
        return priorityChoice ? priorityChoice->getCurrentChoiceName() : "Last";
    }

    //==========================================================================
    // Debug helpers (string dumps for logs)
    //==========================================================================
    juce::String dumpHeldNotes() const
    {
        juce::String s;
        for (const auto& hn : heldNotes)
        {
            s += "[ch=" + juce::String(hn.channel) +
                 " note=" + juce::String(hn.note) +
                 " vel=" + juce::String(hn.velocity) +
                 " ts="  + juce::String((juce::int64) hn.timestampOn) + "] ";
        }
        return s.isEmpty() ? "<empty>" : s;
    }

    static juce::String dumpVoices(const std::vector<HeldNote>& v)
    {
        juce::String s;
        for (const auto& e : v)
        {
            s += "[ch=" + juce::String(e.channel) +
                 " note=" + juce::String(e.note) +
                 " vel=" + juce::String(e.velocity) +
                 " ts="  + juce::String((juce::int64) e.timestampOn) + "] ";
        }
        return s.isEmpty() ? "<empty>" : s;
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(InputFilterProcessor)
};
