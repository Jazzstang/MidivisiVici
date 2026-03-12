//==============================================================================
// HarmonizerProcessor.cpp
// -----------------------------------------------------------------------------
// Key fixes / upgrades in this version:
//   1) Correct JUCE velocity handling across API variants
//      (uint8 0..127 or float 0..1 -> MIDI 1..127).
//   2) Robust NoteOff semantics (NoteOn with vel==0 treated as NoteOff).
//   3) Retune on mapping parameter change while keys are held:
//        - NoteOff old pitches
//        - Recompute mapping
//        - NoteOn new pitches
//      Uses per-voice diff to minimize retriggers.
//   4) Keeps RT safety: no allocations in process().
//==============================================================================

#include "HarmonizerProcessor.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

namespace
{
    constexpr int kGlobalKeyOffIndex         = 12; // C..B + Off
    constexpr int kGlobalScaleChromaticIndex = 9;  // Major..Melodic Minor + Chromatic
}

//==============================================================================
// Tiny helpers
//==============================================================================

// Convert JUCE velocity to MIDI uint8 [1..127].
// Note: vel==0 is handled separately as NoteOff.
template <typename V>
static inline uint8_t velocityFloatToMidi127(V v) noexcept
{
    if constexpr (std::is_floating_point_v<V>)
    {
        const int v127 = (int) std::lround((double) v * 127.0);
        return (uint8_t) juce::jlimit(1, 127, v127);
    }
    else
    {
        return (uint8_t) juce::jlimit(1, 127, (int) v);
    }
}

template <size_t N>
static inline void pushVelocityStack(std::array<uint8_t, N>& stack,
                                     uint8_t& size,
                                     uint8_t velocity) noexcept
{
    if (size < (uint8_t) N)
    {
        stack[(size_t) size++] = velocity;
        return;
    }

    // Saturated stack: keep most recent velocity history.
    for (size_t i = 1; i < N; ++i)
        stack[i - 1] = stack[i];

    stack[N - 1] = velocity;
}

template <size_t N>
static inline uint8_t topVelocityStack(const std::array<uint8_t, N>& stack,
                                       uint8_t size,
                                       uint8_t fallback = 100) noexcept
{
    if (size == 0)
        return fallback;

    return stack[(size_t) (size - 1)];
}

//==============================================================================
// 1) Ctor / reset
//==============================================================================

HarmonizerProcessor::HarmonizerProcessor(juce::AudioProcessorValueTreeState& vts,
                                         Lanes::Lane laneIn)
    : parameters(vts)
    , lane(laneIn)
{
    // Reserve once outside audio processing to reduce chances of MidiBuffer
    // growth on the RT path when additional voices multiply events.
    // Keep extra headroom to reduce chance of MidiBuffer growth in RT when
    // dense NoteOn/NoteOff bursts and retune diffs occur in the same block.
    outScratch.ensureSize(256 * 1024);

    refreshParameterCacheRT();
    resetAllTrackingRT();

    // Seed can be refined later (time-based, host-provided, etc.).
    rngState = 0x12345678u;
    octaveBag.reset(rngState);

    // Read once so we have a consistent initial state.
    readParamsForBlockRT();
    lastEnabled = params.enabled;

    lastMappingParams = params;
    hasLastMappingParams = true;
}

void HarmonizerProcessor::resetAllTrackingRT() noexcept
{
    for (auto& ch : keyStates)
        for (auto& ks : ch)
            ks = KeyState{};

    for (auto& ch : bypassHeldCount) ch.fill(0);
    for (auto& ch : bypassHeldVel)   ch.fill(0);
    for (auto& ch : bypassHeldVelocityStack)
        for (auto& stack : ch)
            stack.fill(0);
    for (auto& ch : bypassHeldVelocityStackSize)
        ch.fill(0);

    outScratch.clear();
    activeSourceKeyCount = 0;

    // Reset mapping snapshot guards too.
    hasLastMappingParams = false;
    lastMappingParams = Params{};
}

void HarmonizerProcessor::setExternalLfoModulations(const ExternalLfoModulations& mod) noexcept
{
    externalLfoModulations = mod;
}

void HarmonizerProcessor::setRuntimeForceBypass(bool shouldBypass) noexcept
{
    runtimeForceBypass = shouldBypass;
}

void HarmonizerProcessor::refreshParameterCacheRT() noexcept
{
    // Lane-scoped
    pEnabled        = parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::harmonizerEnable, lane));
    pPitchCorrector = parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::harmPitchCorrector, lane));
    pOctavePlus     = parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::harmOctavePlusRandom, lane));
    pOctaveMinus    = parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::harmOctaveMinusRandom, lane));
    pVoiceOffsets[0] = parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::harmVoice2, lane));
    pVoiceOffsets[1] = parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::harmVoice3, lane));
    pVoiceOffsets[2] = parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::harmVoice4, lane));
    pVoiceOffsets[3] = parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::harmVoice5, lane));
    pVoiceVelocityMods[0] = parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::harmVoice2VelMod, lane));
    pVoiceVelocityMods[1] = parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::harmVoice3VelMod, lane));
    pVoiceVelocityMods[2] = parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::harmVoice4VelMod, lane));
    pVoiceVelocityMods[3] = parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::harmVoice5VelMod, lane));

    // Global (shared by all lanes)
    pGlobalKey   = parameters.getRawParameterValue(ParamIDs::Global::harmGlobalKey);
    pGlobalScale = parameters.getRawParameterValue(ParamIDs::Global::harmGlobalScale);
}

//==============================================================================
// 2) Param reading (RT-safe)
// - Read once per block.
// - Avoid ValueTree edits: only read parameter objects / atomics.
//==============================================================================

void HarmonizerProcessor::readParamsForBlockRT()
{
    Params p;

    //--------------------------------------------------------------------------
    // RT lecture strategy:
    // - only cached atomic reads here (no ID string builds, no APVTS map lookup).
    // - this keeps per-block overhead deterministic even under heavy MIDI load.
    //--------------------------------------------------------------------------
    const auto readRawInt = [](const std::atomic<float>* raw,
                               int fallback,
                               int minValue,
                               int maxValue) noexcept
    {
        const float v = (raw != nullptr ? raw->load() : (float) fallback);
        return juce::jlimit(minValue, maxValue, (int) juce::roundToInt(v));
    };

    // Applies a normalized modulation delta [-1..+1] to a linear integer range.
    // This preserves the previous "baseNorm + deltaNorm -> convertFrom0to1" model.
    const auto applyNormDeltaLinearInt = [](int baseValue,
                                            int minValue,
                                            int maxValue,
                                            float normDelta) noexcept
    {
        const int clampedBase = juce::jlimit(minValue, maxValue, baseValue);
        const float span = (float) (maxValue - minValue);
        if (span <= 0.0f)
            return clampedBase;

        const float baseNorm = ((float) clampedBase - (float) minValue) / span;
        const float effectiveNorm = juce::jlimit(0.0f, 1.0f,
                                                 baseNorm + juce::jlimit(-1.0f, 1.0f, normDelta));
        const float mapped = (float) minValue + effectiveNorm * span;
        return juce::jlimit(minValue, maxValue, (int) juce::roundToInt(mapped));
    };

    p.enabled        = (pEnabled != nullptr && pEnabled->load() > 0.5f);
    if (runtimeForceBypass)
        p.enabled = false;
    p.pitchCorrector = readRawInt(pPitchCorrector, 0, -12, 12);
    p.octavePlus     = readRawInt(pOctavePlus, 0, 0, 8);
    p.octaveMinus    = readRawInt(pOctaveMinus, 0, 0, 8);

    for (int i = 0; i < 4; ++i)
    {
        const int baseOffset = readRawInt(pVoiceOffsets[(size_t) i], 0, -24, 24);
        // Additional voices are morphable by LFO in normalized space.
        // Rounded result is an integer semitone offset; 0 means voice disabled.
        p.voiceOffsets[i] = applyNormDeltaLinearInt(
            baseOffset, -24, 24, externalLfoModulations.voiceOffsetNormDeltas[(size_t) i]);

        const int baseVelMod = readRawInt(pVoiceVelocityMods[(size_t) i], 0, -10, 10);
        p.voiceVelocityMods[i] = applyNormDeltaLinearInt(
            baseVelMod, -10, 10, externalLfoModulations.voiceVelocityNormDeltas[(size_t) i]);
    }

    p.key   = readRawInt(pGlobalKey, 0, 0, kGlobalKeyOffIndex);
    p.scale = readRawInt(pGlobalScale, 0, 0, kGlobalScaleChromaticIndex);

    // Global tonality Off => force chromatic quantize (no pitch-class filtering).
    if (p.key == kGlobalKeyOffIndex)
        p.scale = kGlobalScaleChromaticIndex;

    p.scaleMask = buildScaleMaskRT(p.key, p.scale);

    params = p;
}

//==============================================================================
// 2b) Detect mapping-param changes
//==============================================================================

bool HarmonizerProcessor::didPitchMappingParamsChange(const Params& a, const Params& b) noexcept
{
    if (a.pitchCorrector != b.pitchCorrector) return true;
    if (a.octavePlus     != b.octavePlus)     return true;
    if (a.octaveMinus    != b.octaveMinus)    return true;
    if (a.key            != b.key)            return true;
    if (a.scale          != b.scale)          return true;

    for (int i = 0; i < 4; ++i)
    {
        // Any additional-voice pitch offset edit changes effective note mapping.
        // We must retune held notes to keep NoteOff/NoteOn pairing deterministic.
        if (a.voiceOffsets[i] != b.voiceOffsets[i])
            return true;
    }

    return false;
}

uint8_t HarmonizerProcessor::computeVoiceVelocityChangeMask(const Params& a, const Params& b) noexcept
{
    uint8_t mask = 0;
    for (int i = 0; i < 4; ++i)
        if (a.voiceVelocityMods[i] != b.voiceVelocityMods[i])
            mask |= (uint8_t) (1u << i);

    return mask;
}

//==============================================================================
// 2c) Retune active keys on mapping change (diff per voice)
//==============================================================================

void HarmonizerProcessor::retuneAllActiveKeysRT(juce::MidiBuffer& out, int samplePos)
{
    // For each active input key (count>0), recompute mapping and diff.
    // This keeps NoteOff/NoteOn consistent even under automation.
    //
    // RT optimization:
    // - activeSourceKeyCount tracks exactly the number of held source keys.
    // - stop scanning as soon as all active keys were visited.
    uint16_t remainingActive = activeSourceKeyCount;
    if (remainingActive == 0)
        return;

    for (int ch0 = 0; ch0 < kNumMidiChannels; ++ch0)
    {
        if (remainingActive == 0)
            break;

        const int ch1 = ch0 + 1;

        for (int inNote = 0; inNote < kNumNotes; ++inNote)
        {
            if (remainingActive == 0)
                break;

            auto& ks = keyStates[(size_t) ch0][(size_t) inNote];

            // Ne pas ignorer les cles count>0 avec mask==0.
            // Cas critique octave +/-:
            // une note peut devenir temporairement silencieuse, puis redevenir valide
            // apres edition des parametres. Il faut donc toujours tenter le retune.
            if (ks.count == 0)
                continue;

            --remainingActive;

            // Snapshot old mapping
            KeyState old = ks;

            // Recompute mapping in-place (keeps count and lastVel).
            computeOutNotesForKeyRT(ch0, inNote, ks);

            // Fast path: identical voice-level mapping and velocities, nothing to emit.
            bool unchanged = (old.mask == ks.mask);
            if (unchanged)
            {
                for (int i = 0; i < kMaxVoices; ++i)
                {
                    const uint8_t bit = (uint8_t) (1u << i);
                    if ((old.mask & bit) == 0)
                        continue;

                    if (old.outNotes[i] != ks.outNotes[i])
                    {
                        unchanged = false;
                        break;
                    }

                    const uint8_t oldVel = velocityForVoiceRT(old.lastVel, i, lastMappingParams);
                    const uint8_t newVel = velocityForVoiceRT(ks.lastVel, i, params);
                    if (oldVel != newVel)
                    {
                        unchanged = false;
                        break;
                    }
                }
            }

            if (unchanged)
                continue;

            struct NoteVel
            {
                int note = -1;
                uint8_t vel = 100;
            };

            auto collect = [&](const KeyState& s,
                               const Params& mapParams,
                               std::array<NoteVel, kMaxVoices>& outNotes,
                               int& outCount)
            {
                outCount = 0;

                for (int i = 0; i < kMaxVoices; ++i)
                {
                    if ((s.mask & (uint8_t) (1u << i)) == 0)
                        continue;

                    const int note = (int) s.outNotes[i];
                    if (!isValidMidiNote(note))
                        continue;

                    if (outCount < kMaxVoices)
                        outNotes[(size_t) outCount++] = { note, velocityForVoiceRT(s.lastVel, i, mapParams) };
                }
            };

            auto findByNote = [](const std::array<NoteVel, kMaxVoices>& notes, int count, int note) -> int
            {
                for (int i = 0; i < count; ++i)
                    if (notes[(size_t) i].note == note)
                        return i;

                return -1;
            };

            std::array<NoteVel, kMaxVoices> oldNotes {};
            std::array<NoteVel, kMaxVoices> newNotes {};
            int oldCountN = 0;
            int newCountN = 0;

            collect(old, lastMappingParams, oldNotes, oldCountN);
            collect(ks, params, newNotes, newCountN);

            for (int i = 0; i < oldCountN; ++i)
            {
                const auto& oldEntry = oldNotes[(size_t) i];
                const int newIndex = findByNote(newNotes, newCountN, oldEntry.note);

                if (newIndex < 0 || newNotes[(size_t) newIndex].vel != oldEntry.vel)
                    out.addEvent(juce::MidiMessage::noteOff(ch1, oldEntry.note, (juce::uint8) 0), samplePos);
            }

            for (int i = 0; i < newCountN; ++i)
            {
                const auto& newEntry = newNotes[(size_t) i];
                const int oldIndex = findByNote(oldNotes, oldCountN, newEntry.note);

                if (oldIndex < 0 || oldNotes[(size_t) oldIndex].vel != newEntry.vel)
                    out.addEvent(juce::MidiMessage::noteOn(ch1, newEntry.note, (juce::uint8) newEntry.vel), samplePos);
            }
        }
    }

}

void HarmonizerProcessor::retuneActiveVoiceVelocitiesRT(juce::MidiBuffer& out,
                                                        int samplePos,
                                                        uint8_t changedAdditionalVoiceMask)
{
    constexpr uint8_t offVel = 0;
    const uint8_t changedMask = (uint8_t) (changedAdditionalVoiceMask & 0x0Fu);
    if (changedMask == 0)
        return;

    // Align 4-bit additional-voice mask (voices 2..5) with KeyState::mask bits:
    // changed bit0 -> voice bit1, ..., changed bit3 -> voice bit4.
    const uint8_t changedVoiceBits = (uint8_t) ((changedMask << 1) & 0x1Eu);

    uint16_t remainingActive = activeSourceKeyCount;
    if (remainingActive == 0)
        return;

    for (int ch0 = 0; ch0 < kNumMidiChannels; ++ch0)
    {
        if (remainingActive == 0)
            break;

        const int ch1 = ch0 + 1;
        for (int inNote = 0; inNote < kNumNotes; ++inNote)
        {
            if (remainingActive == 0)
                break;

            auto& ks = keyStates[(size_t) ch0][(size_t) inNote];
            if (ks.count == 0)
                continue;

            --remainingActive;

            // Voice 0 (base voice) is unaffected by velocity modifiers.
            // Skip early when no changed additional voice is currently active.
            const uint8_t activeChangedVoices = (uint8_t) (ks.mask & changedVoiceBits);
            if (activeChangedVoices == 0)
                continue;

            // Only process changed additional voices 2..5.
            for (int i = 1; i < kMaxVoices; ++i)
            {
                if ((activeChangedVoices & (uint8_t) (1u << i)) == 0)
                    continue;

                const int outNote = (int) ks.outNotes[i];
                if (!isValidMidiNote(outNote))
                    continue;

                const uint8_t oldVel = velocityForVoiceRT(ks.lastVel, i, lastMappingParams);
                const uint8_t newVel = velocityForVoiceRT(ks.lastVel, i, params);
                if (oldVel == newVel)
                    continue;

                out.addEvent(juce::MidiMessage::noteOff(ch1, outNote, (juce::uint8) offVel), samplePos);
                out.addEvent(juce::MidiMessage::noteOn(ch1, outNote, (juce::uint8) newVel), samplePos);
            }
        }
    }
}

void HarmonizerProcessor::activateFromBypassHeldRT(juce::MidiBuffer& out, int samplePos)
{
    constexpr uint8_t offVel = 0;
    activeSourceKeyCount = 0;

    for (int ch0 = 0; ch0 < kNumMidiChannels; ++ch0)
    {
        const int ch1 = ch0 + 1;

        for (int inNote = 0; inNote < kNumNotes; ++inNote)
        {
            const uint8_t heldCount = bypassHeldCount[(size_t) ch0][(size_t) inNote];
            const uint8_t heldVel   = bypassHeldVel[(size_t) ch0][(size_t) inNote];
            auto& heldStack = bypassHeldVelocityStack[(size_t) ch0][(size_t) inNote];
            auto& heldStackSize = bypassHeldVelocityStackSize[(size_t) ch0][(size_t) inNote];

            if (heldCount == 0)
                continue;

            KeyState ks;
            ks.count   = heldCount;
            ks.velocityStackSize = (uint8_t) juce::jlimit(0, kVelocityStackDepth, (int) heldStackSize);
            ks.lastVel = (uint8_t) juce::jlimit(1, 127, (int) (heldVel > 0 ? heldVel : 100));

            for (int i = 0; i < kVelocityStackDepth; ++i)
                ks.velocityStack[(size_t) i] = heldStack[(size_t) i];

            if (ks.velocityStackSize > 0)
                ks.lastVel = topVelocityStack(ks.velocityStack, ks.velocityStackSize, ks.lastVel);

            computeOutNotesForKeyRT(ch0, inNote, ks);

            bool sourceStillPresent = false;
            if (isValidMidiNote(inNote) && ks.mask != 0)
            {
                for (int i = 0; i < kMaxVoices; ++i)
                {
                    if ((ks.mask & (uint8_t) (1u << i)) == 0)
                        continue;
                    if ((int) ks.outNotes[i] == inNote)
                    {
                        sourceStillPresent = true;
                        break;
                    }
                }
            }

            // Source note was sounding during bypass.
            // If not part of new harmonized mapping, stop it.
            if (isValidMidiNote(inNote) && !sourceStillPresent)
                out.addEvent(juce::MidiMessage::noteOff(ch1, inNote, (juce::uint8) offVel), samplePos);

            // Start only voices that are not already sounding as source.
            for (int i = 0; i < kMaxVoices; ++i)
            {
                if ((ks.mask & (uint8_t) (1u << i)) == 0)
                    continue;

                const int outNote = (int) ks.outNotes[i];
                if (!isValidMidiNote(outNote))
                    continue;

                if (sourceStillPresent && outNote == inNote)
                    continue;

                const uint8_t outVel = velocityForVoiceRT(ks.lastVel, i, params);
                out.addEvent(juce::MidiMessage::noteOn(ch1, outNote, (juce::uint8) outVel), samplePos);
            }

            keyStates[(size_t) ch0][(size_t) inNote] = ks;
            if (ks.count > 0 && activeSourceKeyCount < std::numeric_limits<uint16_t>::max())
                ++activeSourceKeyCount;
            bypassHeldCount[(size_t) ch0][(size_t) inNote] = 0;
            bypassHeldVel[(size_t) ch0][(size_t) inNote] = 0;
            heldStackSize = 0;
            heldStack.fill(0);
        }
    }
}

//==============================================================================
// 3) Processing entry
//==============================================================================

void HarmonizerProcessor::process(juce::MidiBuffer& midi)
{
    outScratch.clear();

    // Read params once per block.
    readParamsForBlockRT();

    //--------------------------------------------------------------------------
    // 1) Handle bypass transitions
    //--------------------------------------------------------------------------
    if (lastEnabled && !params.enabled)
        flushAllActiveNotesRT(outScratch, 0, true);

    if (!lastEnabled && params.enabled)
        activateFromBypassHeldRT(outScratch, 0);

    // Disabled: pass-through (after possible flush above).
    if (!params.enabled)
    {
        for (auto it = midi.begin(); it != midi.end(); ++it)
        {
            const auto meta = *it;
            const auto& msg = meta.getMessage();
            outScratch.addEvent(msg, meta.samplePosition);

            const bool isNoteOn  = msg.isNoteOn()  && msg.getVelocity() > 0.0f;
            const bool isNoteOff = msg.isNoteOff() || (msg.isNoteOn() && msg.getVelocity() == 0.0f);
            if (!isNoteOn && !isNoteOff)
                continue;

            const int ch0 = msg.getChannel() - 1;
            const int inNote = msg.getNoteNumber();
            if (!juce::isPositiveAndBelow(ch0, kNumMidiChannels) || !isValidMidiNote(inNote))
                continue;

            auto& c = bypassHeldCount[(size_t) ch0][(size_t) inNote];
            auto& v = bypassHeldVel[(size_t) ch0][(size_t) inNote];
            auto& velStack = bypassHeldVelocityStack[(size_t) ch0][(size_t) inNote];
            auto& velStackSize = bypassHeldVelocityStackSize[(size_t) ch0][(size_t) inNote];

            if (isNoteOn)
            {
                const uint8_t vel = velocityFloatToMidi127(msg.getVelocity());
                pushVelocityStack(velStack, velStackSize, vel);

                if (c < 255) ++c;
                v = (uint8_t) juce::jlimit(1, 127, (int) topVelocityStack(velStack, velStackSize, vel));
            }
            else
            {
                if (c > 0) --c;

                if (velStackSize > 0)
                    --velStackSize;

                if (c == 0)
                {
                    v = 0;
                    velStackSize = 0;
                }
                else if (velStackSize > 0)
                {
                    v = velStack[(size_t) (velStackSize - 1)];
                }
            }
        }

        lastEnabled = params.enabled;
        hasLastMappingParams = false;
        midi.swapWith(outScratch);
        return;
    }

    //--------------------------------------------------------------------------
    // 2) If enabled and mapping params changed while notes are held: retune
    //
    // Why this matters for Pitch Corrector:
    // - pitchCorrector is stage-1 in the mapping pipeline.
    // - a live edit changes NOTE ON destination for already-held keys.
    // - we therefore emit OFF(old) then ON(new) at block start to keep
    //   deterministic pairing and avoid stuck notes.
    //
    // RT note:
    // - this path performs only bounded loops on fixed-size arrays.
    // - no lock/allocation even under fast automation.
    //--------------------------------------------------------------------------
    if (hasLastMappingParams)
    {
        const bool pitchMappingChanged = didPitchMappingParamsChange(lastMappingParams, params);
        const uint8_t velocityChangedMask = computeVoiceVelocityChangeMask(lastMappingParams, params);

        // Optimisation RT: si aucune cle source n'est active, on saute la phase
        // de retune complete (parcours canal x note), sans changer le resultat MIDI.
        if (pitchMappingChanged && activeSourceKeyCount > 0)
        {
            // Full retune may re-map pitches (pitch corrector, octave randomizer +/-, scale, voices).
            retuneAllActiveKeysRT(outScratch, 0);
        }
        else if (velocityChangedMask != 0 && activeSourceKeyCount > 0)
        {
            // Avoid unnecessary octave re-randomization when only velocity modifiers changed.
            // This path keeps pitches stable and updates only per-voice velocity.
            retuneActiveVoiceVelocitiesRT(outScratch, 0, velocityChangedMask);
        }
    }

    lastMappingParams = params;
    hasLastMappingParams = true;
    lastEnabled = params.enabled;

    //--------------------------------------------------------------------------
    // 3) Transform note messages, pass-through others.
    //
    // Normalize note semantics:
    //   - NoteOn vel>0  => NOTE ON
    //   - NoteOff       => NOTE OFF
    //   - NoteOn vel==0 => NOTE OFF (MIDI compatibility)
    //--------------------------------------------------------------------------
    for (auto it = midi.begin(); it != midi.end(); ++it)
    {
        const auto meta = *it;
        const auto& msg = meta.getMessage();
        const int samplePos = meta.samplePosition;

        const bool isNoteOn  = msg.isNoteOn()  && msg.getVelocity() > 0.0f;
        const bool isNoteOff = msg.isNoteOff() || (msg.isNoteOn() && msg.getVelocity() == 0.0f);

        //--------------------------------------------------------------------------
        // NOTE ON
        //--------------------------------------------------------------------------
        if (isNoteOn)
        {
            const int ch1 = msg.getChannel();  // 1..16
            const int ch0 = ch1 - 1;           // 0..15
            const int inNote = msg.getNoteNumber();
            const uint8_t vel = velocityFloatToMidi127(msg.getVelocity());

            if (!juce::isPositiveAndBelow(ch0, kNumMidiChannels) || !isValidMidiNote(inNote))
            {
                outScratch.addEvent(msg, samplePos);
                continue;
            }

            auto& ks = keyStates[(size_t) ch0][(size_t) inNote];
            const bool wasInactive = (ks.count == 0);

            // Compute outputs only on first stack entry.
            if (wasInactive)
                computeOutNotesForKeyRT(ch0, inNote, ks);

            // Keep per-key velocity history so retune/retrigger uses the
            // currently effective held-note velocity.
            pushVelocityStack(ks.velocityStack, ks.velocityStackSize, vel);
            ks.lastVel = topVelocityStack(ks.velocityStack, ks.velocityStackSize, vel);

            // Stack.
            if (ks.count < 255)
                ++ks.count;

            if (wasInactive && ks.count > 0 && activeSourceKeyCount < std::numeric_limits<uint16_t>::max())
                ++activeSourceKeyCount;

            // Emit NoteOn for each active voice.
            for (int i = 0; i < kMaxVoices; ++i)
            {
                if ((ks.mask & (uint8_t) (1u << i)) == 0)
                    continue;

                const int outNote = (int) ks.outNotes[i];
                if (!isValidMidiNote(outNote))
                    continue;

                const uint8_t outVel = velocityForVoiceRT(vel, i, params);
                outScratch.addEvent(juce::MidiMessage::noteOn(ch1, outNote, (juce::uint8) outVel), samplePos);
            }

            continue;
        }

        //--------------------------------------------------------------------------
        // NOTE OFF (including NoteOn with velocity 0)
        //--------------------------------------------------------------------------
        if (isNoteOff)
        {
            const int inCh1 = msg.getChannel();
            const int inCh0 = inCh1 - 1;
            const int inNote = msg.getNoteNumber();

            if (!juce::isPositiveAndBelow(inCh0, kNumMidiChannels) || !isValidMidiNote(inNote))
            {
                outScratch.addEvent(msg, samplePos);
                continue;
            }

            int releaseCh1 = inCh1;
            int releaseCh0 = inCh0;
            auto* releaseKs = &keyStates[(size_t) releaseCh0][(size_t) inNote];

            if (releaseKs->count == 0)
            {
                // Defensive cross-channel fallback:
                // if NOTE OFF channel does not match NOTE ON channel, accept it
                // only when note ownership is unique across channels.
                int matchedCh0 = -1;
                for (int ch0 = 0; ch0 < kNumMidiChannels; ++ch0)
                {
                    if (keyStates[(size_t) ch0][(size_t) inNote].count == 0)
                        continue;

                    if (matchedCh0 >= 0)
                    {
                        matchedCh0 = -2; // ambiguous
                        break;
                    }

                    matchedCh0 = ch0;
                }

                if (matchedCh0 < 0)
                {
                    // Ambiguous or orphan NOTE OFF: keep passthrough.
                    outScratch.addEvent(msg, samplePos);
                    continue;
                }

                releaseCh0 = matchedCh0;
                releaseCh1 = matchedCh0 + 1;
                releaseKs = &keyStates[(size_t) releaseCh0][(size_t) inNote];
                if (releaseKs->count == 0)
                {
                    outScratch.addEvent(msg, samplePos);
                    continue;
                }
            }

            // Unstack.
            --releaseKs->count;

            if (releaseKs->velocityStackSize > 0)
                --releaseKs->velocityStackSize;

            // Only emit NoteOff when count returns to zero.
            if (releaseKs->count == 0)
            {
                // Symetrique du 0->1 sur NoteOn: ce compteur sert seulement
                // a court-circuiter les retunes lorsqu'aucune cle n'est tenue.
                if (activeSourceKeyCount > 0)
                    --activeSourceKeyCount;

                // Use velocity 0 for NoteOff (safe). Preserving incoming vel for
                // NoteOff is optional and inconsistent across devices anyway.
                constexpr uint8_t offVel = 0;

                for (int i = 0; i < kMaxVoices; ++i)
                {
                    if ((releaseKs->mask & (uint8_t) (1u << i)) == 0)
                        continue;

                    const int outNote = (int) releaseKs->outNotes[i];
                    if (!isValidMidiNote(outNote))
                        continue;

                    outScratch.addEvent(juce::MidiMessage::noteOff(releaseCh1, outNote, (juce::uint8) offVel), samplePos);
                }

                *releaseKs = KeyState{};
            }
            else
            {
                releaseKs->lastVel = topVelocityStack(releaseKs->velocityStack,
                                                      releaseKs->velocityStackSize,
                                                      releaseKs->lastVel);
            }

            continue;
        }

        //--------------------------------------------------------------------------
        // Other MIDI: pass-through.
        //--------------------------------------------------------------------------
        outScratch.addEvent(msg, samplePos);
    }

    midi.swapWith(outScratch);
}

//==============================================================================
// 4) Flush active notes on bypass disable
//==============================================================================

void HarmonizerProcessor::flushAllActiveNotesRT(juce::MidiBuffer& out,
                                                int samplePos,
                                                bool emitBypassSourceOn)
{
    // Emit NoteOff for any active key state, then clear.
    // Velocity chosen as 0 (safe).
    constexpr uint8_t offVel = 0;

    uint16_t remainingActive = activeSourceKeyCount;
    if (remainingActive == 0)
    {
        activeSourceKeyCount = 0;
        return;
    }

    for (int ch0 = 0; ch0 < kNumMidiChannels; ++ch0)
    {
        if (remainingActive == 0)
            break;

        const int ch1 = ch0 + 1;

        for (int inNote = 0; inNote < kNumNotes; ++inNote)
        {
            if (remainingActive == 0)
                break;

            auto& ks = keyStates[(size_t) ch0][(size_t) inNote];
            if (ks.count == 0)
                continue;

            --remainingActive;

            auto& bypassStack = bypassHeldVelocityStack[(size_t) ch0][(size_t) inNote];
            auto& bypassStackSize = bypassHeldVelocityStackSize[(size_t) ch0][(size_t) inNote];

            if (ks.mask != 0)
            {
                for (int i = 0; i < kMaxVoices; ++i)
                {
                    if ((ks.mask & (uint8_t) (1u << i)) == 0)
                        continue;

                    const int outNote = (int) ks.outNotes[i];
                    if (!isValidMidiNote(outNote))
                        continue;

                    out.addEvent(juce::MidiMessage::noteOff(ch1, outNote, (juce::uint8) offVel), samplePos);
                }
            }

            // On bypass, hand the maintained source note back immediately.
            if (emitBypassSourceOn && isValidMidiNote(inNote))
            {
                const uint8_t sourceVel = (uint8_t) juce::jlimit(
                    1, 127, (int) topVelocityStack(ks.velocityStack, ks.velocityStackSize, ks.lastVel));

                out.addEvent(juce::MidiMessage::noteOn(ch1, inNote, (juce::uint8) sourceVel), samplePos);
                bypassHeldCount[(size_t) ch0][(size_t) inNote] = ks.count;
                bypassHeldVel[(size_t) ch0][(size_t) inNote]   = sourceVel;

                bypassStackSize = (uint8_t) juce::jlimit(0, kVelocityStackDepth, (int) ks.velocityStackSize);
                for (int i = 0; i < kVelocityStackDepth; ++i)
                    bypassStack[(size_t) i] = (i < (int) bypassStackSize) ? ks.velocityStack[(size_t) i] : 0;

                if (bypassStackSize == 0 && ks.count > 0)
                {
                    bypassStack[(size_t) 0] = sourceVel;
                    bypassStackSize = 1;
                }
            }
            else
            {
                bypassHeldCount[(size_t) ch0][(size_t) inNote] = 0;
                bypassHeldVel[(size_t) ch0][(size_t) inNote]   = 0;
                bypassStackSize = 0;
                bypassStack.fill(0);
            }

            ks = KeyState{};
        }
    }

    activeSourceKeyCount = 0;
}

//==============================================================================
// 5) Compute out notes for a key (freeze mapping until count returns to 0)
//==============================================================================

void HarmonizerProcessor::computeOutNotesForKeyRT(int /*channel0*/, int inNote, KeyState& ks)
{
    ks.mask = 0;
    for (int i = 0; i < kMaxVoices; ++i)
        ks.outNotes[i] = 0;

    //--------------------------------------------------------------------------
    // Stage 1: pitch corrector
    //--------------------------------------------------------------------------
    const int base0 = inNote + params.pitchCorrector;

    //--------------------------------------------------------------------------
    // Helper: attempt to add one produced voice
    //
    // noteCandidateNoOctave is the output of stage 1 + stage 2 (clone offset),
    // before stage 3 octave random and stage 4 quantize.
    //--------------------------------------------------------------------------
    auto addProduced = [&](int voiceIndex, int noteCandidateNoOctave)
    {
        // Stage 3: octave randomizer (shuffle bag), applied per produced note
        const int octavePick = octaveBag.next(params.octaveMinus, params.octavePlus); // -minus..+plus
        const int withOctave = noteCandidateNoOctave + octavePick * 12;

        // Les sorties hors 0..127 sont volontairement "drop".
        // Invariant: une voix non valide ne doit jamais entrer dans ks.mask/outNotes,
        // sinon les couples NoteOn/NoteOff ne seraient plus fiables.
        if (!isValidMidiNote(withOctave))
            return;

        // Stage 4: quantize
        const int q = applyQuantizeRT(withOctave);
        if (!isValidMidiNote(q))
            return;

        // Quantization can collapse multiple voices onto the same pitch.
        // Keep one voice per pitch to guarantee stable NoteOn/NoteOff pairing.
        for (int i = 0; i < kMaxVoices; ++i)
        {
            if ((ks.mask & (uint8_t) (1u << i)) == 0)
                continue;

            if ((int) ks.outNotes[i] == q)
                return;
        }

        ks.outNotes[voiceIndex] = (uint8_t) q;
        ks.mask |= (uint8_t) (1u << voiceIndex);
    };

    //--------------------------------------------------------------------------
    // Stage 2: additional voices (base + clones)
    //--------------------------------------------------------------------------
    addProduced(0, base0);

    // Voices 1..4 map to voiceOffsets[0..3]
    for (int i = 0; i < 4; ++i)
    {
        const int offset = params.voiceOffsets[i];
        if (offset == 0)
            continue;

        addProduced(1 + i, base0 + offset);
    }
}

uint8_t HarmonizerProcessor::velocityForVoiceRT(uint8_t baseVelocity, int voiceIndex, const Params& p) noexcept
{
    const int base = juce::jlimit(1, 127, (int) baseVelocity);

    // Voice 0 is the base note. Velocity modifiers apply only to additional voices.
    if (voiceIndex <= 0 || voiceIndex >= kMaxVoices)
        return (uint8_t) base;

    const int modSteps = p.voiceVelocityMods[voiceIndex - 1];
    const int modPct = juce::jlimit(-10, 10, modSteps) * 10;
    int modulated = base;

    if (modPct >= 0)
    {
        // Positive side interpolates from current velocity to max velocity.
        // +100% always reaches 127.
        const float t = (float) modPct / 100.0f;
        modulated = base + juce::roundToInt((127.0f - (float) base) * t);
    }
    else
    {
        // Negative side interpolates from current velocity to min velocity.
        // -100% always reaches 1.
        const float t = (float) modPct / 100.0f; // negative
        modulated = base + juce::roundToInt(((float) base - 1.0f) * t);
    }

    return (uint8_t) juce::jlimit(1, 127, modulated);
}

//==============================================================================
// 6) Quantize (nearest, tie -> up), drop if impossible
//==============================================================================

int HarmonizerProcessor::applyQuantizeRT(int note) const noexcept
{
    if (!isValidMidiNote(note))
        return -1;

    auto inScale = [&](int n) -> bool
    {
        const int pc = (n % 12 + 12) % 12;
        return (params.scaleMask & (uint16_t) (1u << pc)) != 0;
    };

    if (inScale(note))
        return note;

    int bestDown = -1;
    int bestUp   = -1;

    for (int d = 1; d <= 11; ++d)
    {
        const int dn = note - d;
        if (dn >= 0 && inScale(dn))
        {
            bestDown = dn;
            break;
        }
    }

    for (int d = 1; d <= 11; ++d)
    {
        const int up = note + d;
        if (up <= 127 && inScale(up))
        {
            bestUp = up;
            break;
        }
    }

    if (bestDown < 0 && bestUp < 0)
        return -1;

    if (bestDown < 0)
        return bestUp;

    if (bestUp < 0)
        return bestDown;

    const int distDown = note - bestDown;
    const int distUp   = bestUp - note;

    // Nearest; tie -> up
    if (distUp < distDown)
        return bestUp;

    if (distDown < distUp)
        return bestDown;

    return bestUp;
}

//==============================================================================
// 7) Scale mask builder (12-bit pitch class mask)
//==============================================================================

uint16_t HarmonizerProcessor::buildScaleMaskRT(int key, int scaleIndex) const noexcept
{
    scaleIndex = juce::jlimit(0, kGlobalScaleChromaticIndex, scaleIndex);

    // Chromatic: all pitch classes are valid.
    if (scaleIndex == kGlobalScaleChromaticIndex)
        return 0x0FFFu;

    key = juce::jlimit(0, 11, key);

    // Scale intervals in semitones from tonic (0..11)
    // 0 Major, 1 Minor (natural), 2 Dorian, 3 Phrygian, 4 Lydian,
    // 5 Mixolydian, 6 Locrian, 7 Harmonic Minor, 8 Melodic Minor
    // 9 Chromatic is handled above (all notes enabled).
    static constexpr int kScales[9][7] =
    {
        { 0, 2, 4, 5, 7, 9, 11 }, // Major
        { 0, 2, 3, 5, 7, 8, 10 }, // Natural minor
        { 0, 2, 3, 5, 7, 9, 10 }, // Dorian
        { 0, 1, 3, 5, 7, 8, 10 }, // Phrygian
        { 0, 2, 4, 6, 7, 9, 11 }, // Lydian
        { 0, 2, 4, 5, 7, 9, 10 }, // Mixolydian
        { 0, 1, 3, 5, 6, 8, 10 }, // Locrian
        { 0, 2, 3, 5, 7, 8, 11 }, // Harmonic minor
        { 0, 2, 3, 5, 7, 9, 11 }  // Melodic minor (ascending)
    };

    uint16_t mask = 0;

    for (int i = 0; i < 7; ++i)
    {
        const int pc = (key + kScales[scaleIndex][i]) % 12;
        mask |= (uint16_t) (1u << pc);
    }

    return mask;
}
