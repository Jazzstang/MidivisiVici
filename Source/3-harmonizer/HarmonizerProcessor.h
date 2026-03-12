/**
 * @file HarmonizerProcessor.h
 * @brief Lane-level MIDI harmonizer processor.
 *
 * Threading:
 * - Audio thread for `process`.
 * - RT-safe hot path (no locks/allocations).
 */
//==============================================================================
// HarmonizerProcessor.h
// -----------------------------------------------------------------------------
// MidivisiVici - MIDI Processor: HarmonizerProcessor (lane-based)
//
// Role:
//   This processor applies a deterministic 4-stage transformation pipeline to
//   incoming MIDI notes:
//
//     1) Pitch Corrector
//     2) Additional Voices (up to 4 clones, dropped if offset == 0)
//     3) Octave Randomizer (range: -minus .. +plus, shuffle-bag per note start)
//        - Each produced note (base + each extra voice) gets its own octave pick.
//        - Picks are pseudo-random without repetition until the bag is exhausted.
//     4) Key/Scale Quantize (nearest, tie -> up)
//
// Critical constraints:
//   - Robust NoteOff handling (no stuck notes), even if parameters change while
//     notes are held (automation/snapshots/MIDI control).
//   - Stacking: repeated NoteOn for same (channel,note) stacks.
//     NoteOff is emitted only when stack count returns to 0.
//   - Drop voices if the resulting note is outside 0..127.
//   - Preserve original velocity for NoteOn and NoteOff messages.
//
// Project rules:
//   - ASCII only for comments/logs.
//   - RT-safe: no allocations in process().
//
// Lane behavior:
//   - One HarmonizerProcessor instance per lane (A/B/C).
//   - All parameter IDs used here MUST be lane-suffixed via ParamIDs::lane().
//
// IMPORTANT JUCE MIDI DETAIL (critical bugfix):
//   - juce::MidiMessage::getVelocity() API differs by JUCE version:
//       * uint8 [0..127] on recent versions
//       * float [0..1] on older versions
//     Conversion helpers must support both.
//==============================================================================

#pragma once

#include <JuceHeader.h>

#include <array>
#include <atomic>
#include <cstdint>

#include "../PluginParameters.h"
#include "../DebugConfig.h"

/**
 * @brief Per-lane harmonizer processor running on the audio thread.
 *
 * Pattern:
 * - Pattern: Transformation Pipeline + Frozen Mapping State
 * - Problem solved: apply pitch/voice/octave/quantize transformations while
 *   preserving exact NoteOff matching under parameter automation.
 * - Participants:
 *   - HarmonizerProcessor: lane engine and note lifecycle authority.
 *   - Params snapshot: read once per block from APVTS atomics.
 *   - KeyState table: frozen per-key output mapping until final NoteOff.
 * - Flow:
 *   1. Read params for block.
 *   2. Handle bypass edges and optional retune of held notes.
 *   3. Process source NoteOn/NoteOff with stack tracking.
 *   4. Emit deterministic NoteOff/NoteOn diffs when mapping changes.
 * - Pitfalls:
 *   - Mismatch between frozen mapping and current params can create stuck notes.
 *   - Velocity conversion must handle JUCE API differences.
 */
class HarmonizerProcessor
{
public:
    struct ExternalLfoModulations
    {
        std::array<float, 4> voiceOffsetNormDeltas {};
        std::array<float, 4> voiceVelocityNormDeltas {};
    };

    /**
     * @brief Build one lane harmonizer processor.
     *
     * Thread: message thread during construction.
     * RT-safe: not required (init only).
     */
    explicit HarmonizerProcessor(juce::AudioProcessorValueTreeState& vts,
                                 Lanes::Lane laneIn);

    ~HarmonizerProcessor() = default;

    /**
     * @brief Reset all runtime note tracking tables.
     *
     * Thread: audio thread or prepare/reset path.
     * RT-safe: yes.
     *
     * Call at prepareToPlay() or after a global panic.
     */
    void resetAllTrackingRT() noexcept;

    /**
     * @brief Process one lane MIDI block in-place.
     *
     * Thread: audio thread.
     * RT-safe: yes (no allocations, no locks, no I/O).
     * Order:
     * - Pitch Corrector -> Voices -> Octave randomizer -> Quantize.
     * - If mapping-relevant params changed while keys are held, old mapped
     *   notes are released before new mapped notes are emitted.
     *
     * @param midi In/out lane MIDI block.
     */
    void process(juce::MidiBuffer& midi);

    /**
     * @brief Inject per-block external LFO modulation deltas.
     *
     * Thread: audio thread.
     * RT-safe: yes.
     */
    void setExternalLfoModulations(const ExternalLfoModulations& mod) noexcept;

    /**
     * @brief Force a runtime bypass independently of the APVTS enable flag.
     *
     * Thread: audio thread (set from parent processor before process()).
     * RT-safe: yes.
     */
    void setRuntimeForceBypass(bool shouldBypass) noexcept;

private:
    //==========================================================================
    // Constants
    //==========================================================================
    static constexpr int kNumMidiChannels = 16;
    static constexpr int kNumNotes        = 128;
    static constexpr int kMaxVoices       = 5;  // base + 4 clones
    static constexpr int kVelocityStackDepth = 16;

    //==========================================================================
    // Per-key state
    //
    // For each (channel,note) we store:
    //   - count   : stack count for that input key
    //   - mask    : which output voices are active (bit i = voice i)
    //   - outNotes: frozen output note numbers for each voice
    //   - lastVel : effective top velocity while held (MIDI 1..127)
    //   - velocityStack: per-key NoteOn history for stable retrigger/retune
    //
    // Freezing matters:
    //   - We compute the mapping only when count goes 0->1 (first NoteOn).
    //   - We keep it stable until count returns to 0 (final NoteOff).
    //
    // NEW:
    //   - If mapping-related parameters change while notes are held, we "retune"
    //     the currently active keys:
    //       * NoteOff old outNotes
    //       * Recompute mapping
    //       * NoteOn new outNotes
    //     This prevents mismatched NoteOff due to pitch/scale changes.
    //==========================================================================
    struct KeyState
    {
        uint8_t count = 0;                     // stack count (0..255)
        uint8_t mask  = 0;                     // active voice bits
        uint8_t outNotes[kMaxVoices] = { 0, 0, 0, 0, 0 }; // frozen mapping
        uint8_t lastVel = 100;                 // last NoteOn velocity (1..127)
        std::array<uint8_t, kVelocityStackDepth> velocityStack {};
        uint8_t velocityStackSize = 0;
    };

    std::array<std::array<KeyState, kNumNotes>, kNumMidiChannels> keyStates{};
    // Invariant runtime:
    // - activeSourceKeyCount == number of (channel,note) entries where count > 0.
    // - updated on 0->1 and 1->0 transitions only.
    //
    // Why this exists:
    // - retune/flush passes otherwise scan 16 * 128 keys each time.
    // - this counter allows early stop once all active keys were visited.
    //
    // Any logic touching KeyState::count must preserve this invariant.
    uint16_t activeSourceKeyCount = 0;

    // Held source-note state while harmonizer is bypassed.
    std::array<std::array<uint8_t, kNumNotes>, kNumMidiChannels> bypassHeldCount {};
    std::array<std::array<uint8_t, kNumNotes>, kNumMidiChannels> bypassHeldVel {};
    std::array<std::array<std::array<uint8_t, kVelocityStackDepth>, kNumNotes>, kNumMidiChannels> bypassHeldVelocityStack {};
    std::array<std::array<uint8_t, kNumNotes>, kNumMidiChannels> bypassHeldVelocityStackSize {};

    //==========================================================================
    // Parameters snapshot (read once per block)
    //==========================================================================
    struct Params
    {
        bool enabled = true;

        int pitchCorrector = 0; // -12..+12

        int octavePlus  = 0;    // 0..8
        int octaveMinus = 0;    // 0..8

        int voiceOffsets[4] = { 0, 0, 0, 0 }; // -24..+24, 0 disables voice
        int voiceVelocityMods[4] = { 0, 0, 0, 0 }; // -10..+10, 1 step = 10%

        int key   = 0;          // 0..12 (0..11 + Off)
        int scale = 0;          // 0..9  (0..8 + Chromatic)
        uint16_t scaleMask = 0x0FFFu; // cache quantize per block
    };

    juce::AudioProcessorValueTreeState& parameters;
    const Lanes::Lane lane;

    Params params;
    bool   lastEnabled = true;
    ExternalLfoModulations externalLfoModulations;
    bool runtimeForceBypass = false;

    // Last mapping snapshot (used to detect mapping-affecting changes).
    Params lastMappingParams;
    bool   hasLastMappingParams = false;

    //==========================================================================
    // Scratch output (avoid per-block allocations)
    //==========================================================================
    juce::MidiBuffer outScratch;

    //==========================================================================
    // Shuffle-bag octave randomizer (RT-safe, no alloc)
    //==========================================================================
    struct OctaveShuffleBag
    {
        static constexpr int kMax = 17; // -8..+8
        int values[kMax] {};
        int remaining = 0;

        int lastMinus = -1;
        int lastPlus  = -1;

        uint32_t rng = 0x12345678u;

        static inline uint32_t xorshift32(uint32_t& s) noexcept
        {
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s << 5;
            return s;
        }

        inline void reset(uint32_t seed) noexcept
        {
            rng = (seed != 0u ? seed : 0x12345678u);
            remaining = 0;
            lastMinus = -1;
            lastPlus  = -1;
        }

        inline void refill(int minus, int plus) noexcept
        {
            remaining = 0;
            for (int o = -minus; o <= plus; ++o)
                values[remaining++] = o;

            // Fisher-Yates shuffle
            for (int i = remaining - 1; i > 0; --i)
            {
                const uint32_t r = xorshift32(rng);
                const int j = (int) (r % (uint32_t) (i + 1));
                std::swap(values[i], values[j]);
            }
        }

        inline int next(int minus, int plus) noexcept
        {
            minus = juce::jlimit(0, 8, minus);
            plus  = juce::jlimit(0, 8, plus);

            // Fast-path: pas de randomisation octave active.
            // Evite du travail inutile sur le thread audio (shuffle + RNG),
            // tout en conservant exactement le resultat musical attendu.
            if (minus == 0 && plus == 0)
            {
                lastMinus = 0;
                lastPlus  = 0;
                remaining = 0;
                return 0;
            }

            // If range changed, restart the bag (safest: avoids partial bag issues).
            if (minus != lastMinus || plus != lastPlus)
            {
                lastMinus = minus;
                lastPlus  = plus;
                remaining = 0;
            }

            if (remaining <= 0)
                refill(minus, plus);

            return values[--remaining];
        }
    };

    OctaveShuffleBag octaveBag;
    uint32_t rngState = 0x12345678u;

    // Cached APVTS raw pointers (resolved once, read per block in RT).
    // This removes repeated string ID builds + map lookups in the audio hot path.
    std::atomic<float>* pEnabled = nullptr;
    std::atomic<float>* pPitchCorrector = nullptr;
    std::atomic<float>* pOctavePlus = nullptr;
    std::atomic<float>* pOctaveMinus = nullptr;
    std::array<std::atomic<float>*, 4> pVoiceOffsets {};
    std::array<std::atomic<float>*, 4> pVoiceVelocityMods {};
    std::atomic<float>* pGlobalKey = nullptr;
    std::atomic<float>* pGlobalScale = nullptr;

    //==========================================================================
    // Internal helpers
    //==========================================================================
    void refreshParameterCacheRT() noexcept;
    void readParamsForBlockRT();

    // Detect pitch-mapping changes (notes can move).
    static bool didPitchMappingParamsChange(const Params& a, const Params& b) noexcept;
    // Detect velocity-only changes (notes stay, velocities can change).
    // Returns a bitmask over additional voices 2..5 (bits 0..3).
    static uint8_t computeVoiceVelocityChangeMask(const Params& a, const Params& b) noexcept;

    // Retune all currently active keys:
    //   - diff old vs new mapping per key/voice
    //   - emit NoteOff/NoteOn only for changed voices
    void retuneAllActiveKeysRT(juce::MidiBuffer& out, int samplePos);
    // Velocity-only retune:
    //   - same pitches, refresh NoteOn velocity only when needed
    //   - changedAdditionalVoiceMask bits map to additional voices 2..5
    void retuneActiveVoiceVelocitiesRT(juce::MidiBuffer& out,
                                       int samplePos,
                                       uint8_t changedAdditionalVoiceMask);

    // Re-enable edge: convert bypass-held source notes to harmonized mapping
    // with minimal diff (no retrigger for unchanged notes).
    void activateFromBypassHeldRT(juce::MidiBuffer& out, int samplePos);

    // Flush all active notes (used when disabling to avoid stuck notes)
    void flushAllActiveNotesRT(juce::MidiBuffer& out, int samplePos, bool emitBypassSourceOn);

    // Compute frozen outNotes[] + mask for (channel0,inNote) when count goes 0->1
    void computeOutNotesForKeyRT(int channel0, int inNote, KeyState& ks);

    static inline bool isValidMidiNote(int note) noexcept { return (note >= 0 && note <= 127); }

    int  applyQuantizeRT(int note) const noexcept;     // returns -1 if drop
    uint16_t buildScaleMaskRT(int key, int scaleIndex) const noexcept;
    static uint8_t velocityForVoiceRT(uint8_t baseVelocity, int voiceIndex, const Params& p) noexcept;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HarmonizerProcessor)
};
