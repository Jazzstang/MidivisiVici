//==============================================================================
// InputFilterProcessor.cpp
// -----------------------------------------------------------------------------
// Module DSP/MIDI: InputFilterProcessor (lane-based)
//
// PEDAGOGICAL OVERVIEW
// --------------------
// This processor filters a MIDI stream in a deterministic, RT-safe way.
//
// STRICT FILTERING ORDER (as requested)
//   1) Channel filter
//   2) Mute
//   3) Note range
//   4) Velocity range
//   5) Step Filter (only if all previous checks pass)
//   6) Voice Limit + Priority reconciliation (from held notes)
//
// INTERNAL STATE MODEL (important to avoid stuck notes)
//   - heldNotes:
//       "logical/physical" notes that passed all filters at NoteOn time.
//       This is what the user is currently holding, as seen by this lane.
//
//   - activeVoices:
//       subset of heldNotes that are currently audible after voice limit + priority.
//       activeVoices is derived from heldNotes.
//
//   - Whenever activeVoices changes, we emit a diff:
//       * NoteOff first for voices removed
//       * NoteOn then for voices added
//
// STOP / TRANSPORT BEHAVIOR (requested)
//   - On DAW STOP, emit a one-shot flush and fully reset held/active state.
//   - This explicitly cuts hold-latched notes on STOP.
//
// NOTE OFF EXCEPTION (channel change safety)
//   - NoteOn is strictly filtered by selectedChannel.
//   - NoteOff must be accepted if it matches an existing held note,
//     EVEN IF its channel does not match the current selectedChannel.
//
// IMPORTANT JUCE MIDI DETAILS (critical bugfix)
//   - juce::MidiMessage::getVelocity() API differs by JUCE version
//     (uint8 0..127 or float 0..1): always normalize through helper.
//   - NoteOn with velocity=0 must be treated as NoteOff.
//
// RT SAFETY
//   - No heap allocations in process().
//   - Scratch buffers are reserved in ctor.
//==============================================================================

#include "InputFilterProcessor.h"
#include "../PluginProcessor.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

//==============================================================================
// Tiny helpers
//==============================================================================

// Convert JUCE velocity to MIDI int [1..127].
// Note: We clamp to [1..127] for NoteOn vel>0. (vel==0 is handled as NoteOff)
template <typename V>
static inline int velocityFloatToMidi127(V v) noexcept
{
    if constexpr (std::is_floating_point_v<V>)
    {
        const int v127 = (int) std::lround((double) v * 127.0);
        return juce::jlimit(1, 127, v127);
    }
    else
    {
        return juce::jlimit(1, 127, (int) v);
    }
}

static inline bool passesVelocityGate(const bool velocityFilterEnabled,
                                      const int velocity,
                                      const int velocityMin,
                                      const int velocityMax) noexcept
{
    if (!velocityFilterEnabled)
        return true;

    return velocity >= velocityMin && velocity <= velocityMax;
}

// Fast equality check for small active-voice sets.
// We compare only musical identity (channel,note), not velocity/timestamp:
// - velocity updates for already-active voices should not retrigger NoteOn.
// - timestamp is an ordering aid for priority, not playback identity.
static inline bool sameVoiceKeySetSmall(const std::vector<HeldNote>& a,
                                        const std::vector<HeldNote>& b) noexcept
{
    if (a.size() != b.size())
        return false;

    constexpr size_t kSmallSetThreshold = 24;
    if (a.size() > kSmallSetThreshold)
        return false;

    for (const auto& va : a)
    {
        bool found = false;
        for (const auto& vb : b)
        {
            if (va.channel == vb.channel && va.note == vb.note)
            {
                found = true;
                break;
            }
        }

        if (!found)
            return false;
    }

    return true;
}

//==============================================================================
// Constructor
//==============================================================================

InputFilterProcessor::InputFilterProcessor(MidivisiViciAudioProcessor& processorRef,
                                           juce::AudioProcessorValueTreeState& vts,
                                           Lanes::Lane laneIn)
    : processor(processorRef)
    , parameters(vts)
    , lane(laneIn)
{
    //--------------------------------------------------------------------------
    // Cache lane-suffixed parameter pointers (APVTS atomics).
    //--------------------------------------------------------------------------
    channel = parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::channelFilter, lane));
    mute    = parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::inputMuteToggle, lane));

    // Routing-level toggle (read here for visibility; routing is done in PluginProcessor).
    consume = parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::inputFilterConsume, lane));
    direct  = parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::inputFilterDirect, lane));
    hold    = parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::inputFilterHold, lane));

    noteFilterEnabled = parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::noteFilterToggle, lane));
    noteMin           = parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::noteMin, lane));
    noteMax           = parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::noteMax, lane));

    velocityFilterEnabled = parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::velocityFilterToggle, lane));
    velocityMin           = parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::velocityMin, lane));
    velocityMax           = parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::velocityMax, lane));

    stepFilterEnabled     = parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::stepFilterToggle, lane));
    stepFilterNumerator   = parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::stepFilterNumerator, lane));
    stepFilterDenominator = parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::stepFilterDenominator, lane));

    voiceLimitEnabled = parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::voiceLimitToggle, lane));
    voiceLimit        = parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::voiceLimit, lane));
    priorityRawValue  = parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::priority, lane));

    priorityChoice = dynamic_cast<juce::AudioParameterChoice*>(
        parameters.getParameter(ParamIDs::lane(ParamIDs::Base::priority, lane)));

   #if LOGS_ENABLED && LOG_INPUTFILTER_PROCESSOR
    if (!priorityChoice)
    {
        DBG_LOG("ERROR", "INPUTFILTER", "INIT", "#E01#",
                juce::String("priorityChoice parameter missing for lane ")
                + juce::String(Lanes::laneSuffix(lane)) + "!");
    }

    if (!consume)
    {
        DBG_LOG("ERROR", "INPUTFILTER", "INIT", "#E02#",
                juce::String("consume parameter missing for lane ")
                + juce::String(Lanes::laneSuffix(lane))
                + " (ParamIDs::Base::inputFilterConsume)!");
    }

    if (!hold)
    {
        DBG_LOG("ERROR", "INPUTFILTER", "INIT", "#E03#",
                juce::String("hold parameter missing for lane ")
                + juce::String(Lanes::laneSuffix(lane))
                + " (ParamIDs::Base::inputFilterHold)!");
    }
   #endif

    jassert(priorityChoice != nullptr);
    jassert(priorityRawValue != nullptr);
    jassert(consume != nullptr);
    jassert(hold != nullptr);

    //--------------------------------------------------------------------------
    // Reserve internal buffers to minimize RT allocations.
    //--------------------------------------------------------------------------
    // heldNotes:
    // - uniqueness is enforced on (channel,note)
    // - NoteOn is strict-channel filtered in this module
    // => practical upper bound is 128 logical keys for one lane snapshot.
    heldNotes.reserve(128);

    // activeVoices is bounded by voice limit (1..16).
    activeVoices.reserve(16);

    oldVoicesScratch.reserve(16);
    newVoicesScratch.reserve(128);

    outScratch.clear();
    // Reserve enough room for dense MIDI bursts to avoid RT growth in process().
    // MidiBuffer::ensureSize is expressed in bytes, not event count.
    // A larger upfront reservation reduces allocator spikes under heavy CC/note bursts.
    outScratch.ensureSize(65536);

    resetAllTrackingRT();
}

//==============================================================================
// Transport hooks
//==============================================================================

void InputFilterProcessor::onPlaybackStarted()
{
    hostIsStopped        = false;
    didFlushOnceThisStop = false;
    pendingAllNotesOff   = false;
    currentStepCount     = 0;
    currentStepPhase     = 0;
    pendingHeldRetrigOnStart = !heldNotes.empty();

   #if LOGS_ENABLED && LOG_INPUTFILTER_PROCESSOR
    DBG_LOG("STATE", "INPUTFILTER", "TRANSPORT", "#IFP20#",
            juce::String("PLAY lane=") + juce::String(Lanes::laneSuffix(lane))
            + " retrig=" + juce::String(pendingHeldRetrigOnStart ? "Y" : "N"));
   #endif
}

void InputFilterProcessor::onPlaybackStopped()
{
    hostIsStopped = true;

    // Arm a single one-shot STOP reset if there is any note state to clear.
    if (!didFlushOnceThisStop && (!activeVoices.empty() || !heldNotes.empty()))
        pendingAllNotesOff = true;

   #if LOGS_ENABLED && LOG_INPUTFILTER_PROCESSOR
    DBG_LOG("STATE", "INPUTFILTER", "TRANSPORT", "#IFP21#",
            juce::String("STOP lane=") + juce::String(Lanes::laneSuffix(lane))
            + " armed=" + juce::String(pendingAllNotesOff ? "Y" : "N"));
   #endif
}

void InputFilterProcessor::resetAllTrackingRT() noexcept
{
    heldNotes.clear();
    activeVoices.clear();
    for (auto& ch : physicalHeldCount)
        ch.fill(0);

    timestampCounter = 0;
    currentStepCount = 0;
    currentStepPhase = 0;

    pendingAllNotesOff   = false;
    hostIsStopped        = false;
    didFlushOnceThisStop = false;
    pendingHeldRetrigOnStart = false;
    hasLatchedOnlyHeldNotes = false;

    lastSnapshotValid = false;
    lastSelectedChannel = 1;
    lastMuteActive      = false;
    lastNoteF           = false;
    lastNMin            = 0;
    lastNMax            = 127;
    lastVeloF           = false;
    lastVMin            = 0;
    lastVMax            = 127;
    lastStepF           = false;
    lastStepNum         = 1;
    lastStepDen         = 1;
    lastLimitEnabled    = false;
    lastMaxVoices       = 16;
    lastPriority        = 0;
    lastConsumeRouting  = false;
    lastDirectRouting   = false;
    lastHoldEnabled     = false;
}

int InputFilterProcessor::forceAllNotesOffAndClear(juce::MidiBuffer& out,
                                                   int samplePos,
                                                   std::array<HeldNote, 16>* releasedActiveVoices) noexcept
{
    // Flush only currently audible notes.
    int releasedCount = 0;

    for (const auto& v : activeVoices)
    {
        out.addEvent(juce::MidiMessage::noteOff(v.channel, v.note), samplePos);

        if (releasedActiveVoices && releasedCount < (int) releasedActiveVoices->size())
            (*releasedActiveVoices)[(size_t) releasedCount++] = v;
    }

    heldNotes.clear();
    activeVoices.clear();
    for (auto& ch : physicalHeldCount)
        ch.fill(0);

    pendingAllNotesOff   = false;
    didFlushOnceThisStop = false;
    currentStepCount     = 0;
    currentStepPhase     = 0;
    lastSnapshotValid    = false;
    pendingHeldRetrigOnStart = false;
    hasLatchedOnlyHeldNotes = false;

    return releasedCount;
}

void InputFilterProcessor::resyncFromExternalHeldState(
    const std::array<std::array<int, 128>, 16>& heldCounts,
    const std::array<std::array<int, 128>, 16>& heldVelocity,
    const std::array<std::array<uint64_t, 128>, 16>& heldOrder,
    juce::MidiBuffer& out,
    int samplePos)
{
    const int  selectedChannel = juce::jlimit(1, 16, channel ? (int) channel->load() : 1);
    const bool muteActive      = (mute && mute->load() > 0.5f);

    const bool noteF = (noteFilterEnabled     && noteFilterEnabled->load()     > 0.5f);
    const bool veloF = (velocityFilterEnabled && velocityFilterEnabled->load() > 0.5f);

    const int nA = (int) (noteMin     ? noteMin->load()     : 0);
    const int nB = (int) (noteMax     ? noteMax->load()     : 127);
    const int vA = (int) (velocityMin ? velocityMin->load() : 0);
    const int vB = (int) (velocityMax ? velocityMax->load() : 127);

    const int nMin = std::min(nA, nB);
    const int nMax = std::max(nA, nB);
    const int vMin = std::min(vA, vB);
    const int vMax = std::max(vA, vB);

    oldVoicesScratch.assign(activeVoices.begin(), activeVoices.end());
    heldNotes.clear();
    for (auto& ch : physicalHeldCount)
        ch.fill(0);
    hasLatchedOnlyHeldNotes = false;

    uint64_t maxTs = timestampCounter;

    for (int ch0 = 0; ch0 < 16; ++ch0)
    {
        const int ch1 = ch0 + 1;

        for (int nt = 0; nt < 128; ++nt)
        {
            if (heldCounts[(size_t) ch0][(size_t) nt] <= 0)
                continue;

            if (muteActive)
                continue;

            if (ch1 != selectedChannel)
                continue;

            const int vel = juce::jlimit(1, 127, heldVelocity[(size_t) ch0][(size_t) nt] > 0
                                                 ? heldVelocity[(size_t) ch0][(size_t) nt]
                                                 : 100);

            if (noteF && (nt < nMin || nt > nMax))
                continue;

            if (veloF && (vel < vMin || vel > vMax))
                continue;

            uint64_t ts = heldOrder[(size_t) ch0][(size_t) nt];
            if (ts == 0)
                ts = ++maxTs;
            else
                maxTs = std::max(maxTs, ts);

            heldNotes.push_back(HeldNote { ch1, nt, vel, ts });

            // Physical tracker is intentionally boolean-like:
            // when resync says "held", we mark true; otherwise false.
            // This avoids counter drift from synthetic retriggers and keeps
            // NOTE OFF release logic conservative against stuck notes.
            physicalHeldCount[(size_t) (ch1 - 1)][(size_t) nt] =
                (heldCounts[(size_t) ch0][(size_t) nt] > 0) ? (uint16_t) 1 : (uint16_t) 0;
        }
    }

    timestampCounter = maxTs;
    recalculateActiveVoices(out, samplePos);
}

//==============================================================================
// Held notes helpers
//==============================================================================

void InputFilterProcessor::addToHeldNotes(int ch, int note, int vel)
{
    // Enforce uniqueness for (ch, note):
    // - If the key already exists, refresh velocity + timestamp in place.
    // - If it does not exist, append once.
    //
    // This avoids erase/remove churn in the common re-trigger case while keeping
    // the "Last" priority model deterministic (timestamp is always refreshed).
    const auto it = std::find_if(heldNotes.begin(), heldNotes.end(),
                                 [&](const HeldNote& h)
                                 {
                                     return (h.channel == ch && h.note == note);
                                 });

    if (it != heldNotes.end())
    {
        it->velocity = vel;
        it->timestampOn = ++timestampCounter;
    }
    else
    {
        heldNotes.push_back(HeldNote { ch, note, vel, ++timestampCounter });
    }

   #if LOGS_ENABLED && LOG_INPUTFILTER_PROCESSOR
    DBG_LOG("ACTION", "HELD", "ADD", "#H01#",
            juce::String("lane=") + juce::String(Lanes::laneSuffix(lane))
            + " ch=" + juce::String(ch)
            + " note=" + juce::String(note)
            + " vel=" + juce::String(vel));
    DBG_LOG("STATE", "HELD", "LIST", "#H02#", dumpHeldNotes());
   #endif
}

void InputFilterProcessor::removeFromHeldNotes(int ch, int note)
{
    // Keys are unique by (ch, note), so we can erase a single iterator.
    const auto it = std::find_if(heldNotes.begin(), heldNotes.end(),
                                 [&](const HeldNote& h)
                                 {
                                     return (h.channel == ch && h.note == note);
                                 });
    if (it != heldNotes.end())
        heldNotes.erase(it);

   #if LOGS_ENABLED && LOG_INPUTFILTER_PROCESSOR
    DBG_LOG("ACTION", "HELD", "REMOVE", "#H03#",
            juce::String("lane=") + juce::String(Lanes::laneSuffix(lane))
            + " ch=" + juce::String(ch)
            + " note=" + juce::String(note));
    DBG_LOG("STATE", "HELD", "LIST", "#H04#", dumpHeldNotes());
   #endif
}

bool InputFilterProcessor::isHeldNote(int ch, int note) const noexcept
{
    for (const auto& h : heldNotes)
        if (h.channel == ch && h.note == note)
            return true;

    return false;
}

void InputFilterProcessor::markPhysicalNoteOn(int ch, int note) noexcept
{
    if (ch < 1 || ch > 16 || note < 0 || note > 127)
        return;

    // Boolean-like tracker by design.
    physicalHeldCount[(size_t) (ch - 1)][(size_t) note] = 1;
}

void InputFilterProcessor::markPhysicalNoteOff(int ch, int note) noexcept
{
    if (ch < 1 || ch > 16 || note < 0 || note > 127)
        return;

    // Boolean-like tracker by design.
    physicalHeldCount[(size_t) (ch - 1)][(size_t) note] = 0;
}

bool InputFilterProcessor::isPhysicallyHeld(int ch, int note) const noexcept
{
    if (ch < 1 || ch > 16 || note < 0 || note > 127)
        return false;

    return physicalHeldCount[(size_t) (ch - 1)][(size_t) note] > 0;
}

bool InputFilterProcessor::findPhysicalNoteUniqueAnyChannel(int note, int& outCh) const noexcept
{
    if (note < 0 || note > 127)
    {
        outCh = -1;
        return false;
    }

    int foundCh = -1;
    int foundCount = 0;

    for (int ch = 1; ch <= 16; ++ch)
    {
        if (!isPhysicallyHeld(ch, note))
            continue;

        foundCh = ch;
        ++foundCount;
        if (foundCount > 1)
            break;
    }

    if (foundCount == 1)
    {
        outCh = foundCh;
        return true;
    }

    outCh = -1;
    return false;
}

void InputFilterProcessor::dropLatchedOnlyHeldNotes() noexcept
{
    if (!hasLatchedOnlyHeldNotes)
        return;

    heldNotes.erase(std::remove_if(heldNotes.begin(), heldNotes.end(),
                                   [&](const HeldNote& h)
                                   {
                                       return !isPhysicallyHeld(h.channel, h.note);
                                   }),
                    heldNotes.end());

    hasLatchedOnlyHeldNotes = false;
}

//------------------------------------------------------------------------------
// SAFE NoteOff fallback:
//   We do NOT want "stealing" by releasing a random channel when multiple
//   identical note numbers are held on different channels.
//
// Rule:
//   - If exactly ONE held note exists with this note number (any channel),
//     we may accept the NoteOff and use that unique channel.
//   - If 0 or >1 matches exist, we ignore (no guessing).
//------------------------------------------------------------------------------
bool InputFilterProcessor::findHeldNoteUniqueAnyChannel(int note, int& outCh) const noexcept
{
    int foundCh = -1;
    int foundCount = 0;

    for (const auto& h : heldNotes)
    {
        if (h.note != note)
            continue;

        foundCh = h.channel;
        ++foundCount;

        if (foundCount > 1)
            break;
    }

    if (foundCount == 1)
    {
        outCh = foundCh;
        return true;
    }

    outCh = -1;
    return false;
}

//==============================================================================
// Diff: emit NoteOff/NoteOn for changes in the active voice set
//==============================================================================

void InputFilterProcessor::diffAndEmit(const std::vector<HeldNote>& oldVoices,
                                       const std::vector<HeldNote>& newVoices,
                                       juce::MidiBuffer& out,
                                       int samplePos,
                                       bool emitAddedVoices)
{
    // Fast paths for common cases (no set overlap checks needed).
    if (oldVoices.empty() && newVoices.empty())
        return;

    // Most frequent RT case under dense note streams:
    // active set unchanged (same channel/note keys), only metadata differs.
    // In that case, there is nothing to emit.
    if (sameVoiceKeySetSmall(oldVoices, newVoices))
        return;

    if (newVoices.empty())
    {
        for (const auto& v : oldVoices)
            out.addEvent(juce::MidiMessage::noteOff(v.channel, v.note), samplePos);
        return;
    }

    if (oldVoices.empty())
    {
        if (emitAddedVoices)
        {
            for (const auto& v : newVoices)
                out.addEvent(juce::MidiMessage::noteOn(v.channel, v.note, (juce::uint8) v.velocity), samplePos);
        }
        return;
    }

    // Membership maps avoid O(n^2) nested scans when held sets are dense.
    std::array<std::array<uint8_t, 128>, 16> oldMask {};
    std::array<std::array<uint8_t, 128>, 16> newMask {};

    for (const auto& v : oldVoices)
    {
        const int ch = juce::jlimit(1, 16, v.channel) - 1;
        const int nt = juce::jlimit(0, 127, v.note);
        oldMask[(size_t) ch][(size_t) nt] = 1;
    }

    for (const auto& v : newVoices)
    {
        const int ch = juce::jlimit(1, 16, v.channel) - 1;
        const int nt = juce::jlimit(0, 127, v.note);
        newMask[(size_t) ch][(size_t) nt] = 1;
    }

    // 1) NOTE OFF for voices that disappeared
    for (const auto& v : oldVoices)
    {
        const int ch = juce::jlimit(1, 16, v.channel) - 1;
        const int nt = juce::jlimit(0, 127, v.note);
        if (newMask[(size_t) ch][(size_t) nt] == 0)
        {
            out.addEvent(juce::MidiMessage::noteOff(v.channel, v.note), samplePos);

           #if LOGS_ENABLED && LOG_INPUTFILTER_PROCESSOR
            DBG_LOG("ACTION", "VOICE", "STOP", "#VOFF#",
                    juce::String("lane=") + juce::String(Lanes::laneSuffix(lane))
                    + " ch=" + juce::String(v.channel)
                    + " note=" + juce::String(v.note));
           #endif
        }
    }

    // 2) NOTE ON for newly activated voices
    if (emitAddedVoices)
    {
        for (const auto& v : newVoices)
        {
            const int ch = juce::jlimit(1, 16, v.channel) - 1;
            const int nt = juce::jlimit(0, 127, v.note);
            if (oldMask[(size_t) ch][(size_t) nt] == 0)
            {
                out.addEvent(juce::MidiMessage::noteOn(v.channel, v.note, (juce::uint8) v.velocity), samplePos);

               #if LOGS_ENABLED && LOG_INPUTFILTER_PROCESSOR
                DBG_LOG("ACTION", "VOICE", "START", "#VON#",
                        juce::String("lane=") + juce::String(Lanes::laneSuffix(lane))
                        + " ch=" + juce::String(v.channel)
                        + " note=" + juce::String(v.note)
                        + " vel=" + juce::String(v.velocity));
               #endif
            }
        }
    }
}

//==============================================================================
// Recompute activeVoices from heldNotes (voice limit + priority) and emit diff
//==============================================================================

void InputFilterProcessor::recalculateActiveVoicesWithPolicy(juce::MidiBuffer& out,
                                                             int samplePos,
                                                             bool emitAddedVoices,
                                                             bool limitEnabled,
                                                             int maxVoices,
                                                             int prio)
{
    // Copy previous voices into scratch (reuse capacity, avoid allocations).
    oldVoicesScratch.assign(activeVoices.begin(), activeVoices.end());

    // Start from all held notes.
    newVoicesScratch.assign(heldNotes.begin(), heldNotes.end());

    // Priority ordering only matters when a truncation can happen.
    // If all held notes fit in the active set, sorting is pure CPU overhead.
    const bool needsPriorityTrim = limitEnabled && ((int) newVoicesScratch.size() > maxVoices);

    if (needsPriorityTrim)
    {
        // Sort by priority BEFORE truncation.
        switch (prio)
        {
            case 0: // Last: keep most recent first
                std::sort(newVoicesScratch.begin(), newVoicesScratch.end(),
                          [](const HeldNote& a, const HeldNote& b)
                          {
                              return a.timestampOn > b.timestampOn;
                          });
                break;

            case 1: // Lowest: keep lowest notes first
                std::sort(newVoicesScratch.begin(), newVoicesScratch.end(),
                          [](const HeldNote& a, const HeldNote& b)
                          {
                              return a.note < b.note;
                          });
                break;

            case 2: // Highest: keep highest notes first
            default:
                std::sort(newVoicesScratch.begin(), newVoicesScratch.end(),
                          [](const HeldNote& a, const HeldNote& b)
                          {
                              return a.note > b.note;
                          });
                break;
        }
    }

   #if LOGS_ENABLED && LOG_INPUTFILTER_PROCESSOR
    DBG_LOG("STATE", "INPUTFILTER", "VOICE PRIORITY", "#IFP19#",
            juce::String("lane=") + juce::String(Lanes::laneSuffix(lane))
            + " mode=" + getPriorityText()
            + " limit=" + juce::String(limitEnabled ? "ON" : "OFF")
            + " max=" + juce::String(maxVoices));
   #endif

    // Truncate to respect voice limit.
    if (needsPriorityTrim)
        newVoicesScratch.resize((size_t) maxVoices);

    // Replace active voices (use assign to keep capacity stable).
    activeVoices.assign(newVoicesScratch.begin(), newVoicesScratch.end());

    // Emit diff: NoteOff removed, NoteOn added (or off-only when requested).
    diffAndEmit(oldVoicesScratch, activeVoices, out, samplePos, emitAddedVoices);

   #if LOGS_ENABLED && LOG_INPUTFILTER_PROCESSOR
    DBG_LOG("STATE", "INPUTFILTER", "ACTIVE VOICES", "#IFP26#",
            juce::String("lane=") + juce::String(Lanes::laneSuffix(lane))
            + " voices=" + dumpVoices(activeVoices));
   #endif
}

void InputFilterProcessor::recalculateActiveVoices(juce::MidiBuffer& out,
                                                   int samplePos,
                                                   bool emitAddedVoices)
{
    const bool limitEnabledLocal = (voiceLimitEnabled && voiceLimitEnabled->load() > 0.5f);

    int maxVoicesLocal = 16;
    if (limitEnabledLocal && voiceLimit)
        maxVoicesLocal = juce::jlimit(1, 16, (int) voiceLimit->load());

    const int prioLocal = getPriorityIndex();

    recalculateActiveVoicesWithPolicy(out,
                                      samplePos,
                                      emitAddedVoices,
                                      limitEnabledLocal,
                                      maxVoicesLocal,
                                      prioLocal);
}

void InputFilterProcessor::emitActiveVoicesRetrigger(juce::MidiBuffer& out, int samplePos)
{
    if (activeVoices.empty())
        return;

    // Order is intentional: release old state first, then attack again.
    for (const auto& v : activeVoices)
        out.addEvent(juce::MidiMessage::noteOff(v.channel, v.note), samplePos);

    for (const auto& v : activeVoices)
        out.addEvent(juce::MidiMessage::noteOn(v.channel, v.note,
                                               (juce::uint8) juce::jlimit(1, 127, v.velocity)),
                     samplePos);
}

bool InputFilterProcessor::pruneHeldNotesForCurrentGate(int selectedChannel,
                                                        bool muteActive,
                                                        bool noteF,
                                                        int nMin,
                                                        int nMax,
                                                        bool veloF,
                                                        int vMin,
                                                        int vMax) noexcept
{
    const auto beforeSize = heldNotes.size();

    heldNotes.erase(std::remove_if(heldNotes.begin(), heldNotes.end(),
                                   [&](const HeldNote& h)
                                   {
                                       if (muteActive)
                                           return true;

                                       if (h.channel != selectedChannel)
                                           return true;

                                       if (noteF && (h.note < nMin || h.note > nMax))
                                           return true;

                                       if (!passesVelocityGate(veloF, h.velocity, vMin, vMax))
                                           return true;

                                       return false;
                                   }),
                    heldNotes.end());

    return heldNotes.size() != beforeSize;
}

bool InputFilterProcessor::shouldPassStepFilterAndAdvance(int stepNum, int stepDen) noexcept
{
    // Defensive normalization:
    // stepNum comes from 1-based UI ("num of den"), stepDen must stay >= 1.
    // Clamping here keeps runtime robust against malformed state restoration.
    stepDen = juce::jmax(1, stepDen);
    stepNum = juce::jlimit(1, stepDen, stepNum);

    // Saturating wrap guard for extremely long sessions.
    // We reset only the absolute counter used for logs; phase remains continuous.
    if (currentStepCount == std::numeric_limits<uint64_t>::max())
        currentStepCount = 0;

    ++currentStepCount;

    if (currentStepPhase < 0 || currentStepPhase >= stepDen)
        currentStepPhase = 0;

    const bool pass = (currentStepPhase == (stepNum - 1));

    ++currentStepPhase;
    if (currentStepPhase >= stepDen)
        currentStepPhase = 0;

    return pass;
}

//==============================================================================
// Main process (RT)
//==============================================================================

void InputFilterProcessor::process(juce::MidiBuffer& midiMessages)
{
    outScratch.clear();

    //--------------------------------------------------------------------------
    // 0) Detect if any *real* NoteOn (vel>0) exists in this block
    //    only when START retrigger could be applied.
    //--------------------------------------------------------------------------
    bool hasIncomingNoteOn = false;
    if (pendingHeldRetrigOnStart && !hostIsStopped)
    {
        for (auto it = midiMessages.begin(); it != midiMessages.end(); ++it)
        {
            const auto meta = *it;
            const auto& m = meta.getMessage();

            if (m.isNoteOn() && m.getVelocity() > 0.0f)
            {
                hasIncomingNoteOn = true;
                break;
            }
        }
    }

    //--------------------------------------------------------------------------
    // 1) One-shot STOP flush
    //--------------------------------------------------------------------------
    const bool shouldFlush =
        pendingAllNotesOff &&
        hostIsStopped &&
        (!activeVoices.empty() || !heldNotes.empty());

    if (shouldFlush)
    {
       #if LOGS_ENABLED && LOG_INPUTFILTER_PROCESSOR
        DBG_LOG("ACTION", "INPUTFILTER", "FLUSH", "#ALLOFF_INIT#",
                juce::String("lane=") + juce::String(Lanes::laneSuffix(lane))
                + " voices=" + juce::String((int) activeVoices.size()));
       #endif

        // STOP semantics: cut audible voices and clear held/latched state.
        forceAllNotesOffAndClear(outScratch, 0, nullptr);
        didFlushOnceThisStop = true;

       #if LOGS_ENABLED && LOG_INPUTFILTER_PROCESSOR
        DBG_LOG("ACTION", "INPUTFILTER", "FLUSH", "#ALLOFF_RESET#",
                juce::String("lane=") + juce::String(Lanes::laneSuffix(lane))
                + " hold/state reset on STOP");
       #endif
    }

    //--------------------------------------------------------------------------
    // 2) Snapshot parameters (atomic loads) for this block
    //--------------------------------------------------------------------------
    const int  selectedChannel = juce::jlimit(1, 16, channel ? (int) channel->load() : 1);
    const bool muteActive      = (mute && mute->load() > 0.5f);

    // Routing-level toggles (routing is handled in PluginProcessor).
    // consume effective semantics are aligned with router:
    // consume is forced OFF when step filter is ON.
    const bool consumeRequested = (consume && consume->load() > 0.5f);
    const bool directActive     = (direct && direct->load() > 0.5f);
    const bool holdActive       = (hold && hold->load() > 0.5f);

    const bool noteF = (noteFilterEnabled     && noteFilterEnabled->load()     > 0.5f);
    const bool veloF = (velocityFilterEnabled && velocityFilterEnabled->load() > 0.5f);
    const bool stepF = (stepFilterEnabled     && stepFilterEnabled->load()     > 0.5f);
    const bool consumeActive = (consumeRequested && !stepF);

    // Normalize min/max (UI may allow min > max).
    const int nA = (int) (noteMin     ? noteMin->load()     : 0);
    const int nB = (int) (noteMax     ? noteMax->load()     : 127);
    const int vA = (int) (velocityMin ? velocityMin->load() : 0);
    const int vB = (int) (velocityMax ? velocityMax->load() : 127);

    const int nMin = std::min(nA, nB);
    const int nMax = std::max(nA, nB);
    const int vMin = std::min(vA, vB);
    const int vMax = std::max(vA, vB);
    const int stepDen = juce::jlimit(1, 16, (int) (stepFilterDenominator ? stepFilterDenominator->load() : 1));
    const int stepNumRaw = juce::jlimit(1, 16, (int) (stepFilterNumerator ? stepFilterNumerator->load() : 1));
    // "num of den" is normalized so runtime always has a valid target step.
    const int stepNum = juce::jlimit(1, stepDen, stepNumRaw);

    const bool limitEnabled = (voiceLimitEnabled && voiceLimitEnabled->load() > 0.5f);
    int maxVoices = 16;
    if (limitEnabled && voiceLimit)
        maxVoices = juce::jlimit(1, 16, (int) voiceLimit->load());

    const int prio = getPriorityIndex();

   #if LOGS_ENABLED && LOG_INPUTFILTER_PROCESSOR
    DBG_LOG("STATE", "INPUTFILTER", "BLOCK START", "#IFP00#",
            juce::String("lane=") + juce::String(Lanes::laneSuffix(lane))
            + " ch=" + juce::String(selectedChannel)
            + " mute=" + juce::String(muteActive ? "ON" : "OFF")
            + " consumeReq=" + juce::String(consumeRequested ? "ON" : "OFF")
            + " consumeEff=" + juce::String(consumeActive ? "ON" : "OFF")
            + " direct=" + juce::String(directActive ? "ON" : "OFF")
            + " hold=" + juce::String(holdActive ? "ON" : "OFF")
            + " noteF=" + juce::String(noteF ? "ON" : "OFF")
            + " veloF=" + juce::String(veloF ? "ON" : "OFF")
            + " stepF=" + juce::String(stepF ? "ON" : "OFF")
            + " limit=" + juce::String(limitEnabled ? "ON" : "OFF")
            + " max=" + juce::String(maxVoices)
            + " prio=" + juce::String(prio)
            + " n=[" + juce::String(nMin) + ".." + juce::String(nMax) + "]"
            + " v=[" + juce::String(vMin) + ".." + juce::String(vMax) + "]");
   #endif

    //--------------------------------------------------------------------------
    // 2.b) Parameter-change resync (no incoming note required)
    //
    // This prevents stale active voices ("stuck" notes) when users tweak
    // InputFilter parameters while notes are held.
    //--------------------------------------------------------------------------
    if (lastSnapshotValid)
    {
        bool holdForcedReset = false;

        const bool gateParamsChanged =
            (selectedChannel != lastSelectedChannel) ||
            (muteActive      != lastMuteActive)      ||
            (noteF           != lastNoteF)           ||
            (nMin            != lastNMin)            ||
            (nMax            != lastNMax)            ||
            (veloF           != lastVeloF)           ||
            (vMin            != lastVMin)            ||
            (vMax            != lastVMax);

        // Voice-limit recalculation is only needed when the policy can actually
        // change the selected active subset.
        const int heldCountNow = (int) heldNotes.size();
        const bool wasRestrictive = lastLimitEnabled && (heldCountNow > lastMaxVoices);
        const bool isRestrictive  = limitEnabled && (heldCountNow > maxVoices);

        const bool limitStateChanged = (limitEnabled != lastLimitEnabled) ||
                                       (maxVoices    != lastMaxVoices);
        const bool priorityChanged = (prio != lastPriority);

        const bool voiceParamsChanged =
            (limitStateChanged && (wasRestrictive || isRestrictive)) ||
            (priorityChanged   && (wasRestrictive || isRestrictive));

        const bool stepParamsChanged =
            (stepF   != lastStepF)   ||
            (stepNum != lastStepNum) ||
            (stepDen != lastStepDen);
        const bool routingParamsChanged =
            (consumeActive != lastConsumeRouting) ||
            (directActive  != lastDirectRouting);
        const bool holdToggledOff = (!holdActive && lastHoldEnabled);
        const bool nonHoldParamsChanged =
            gateParamsChanged || voiceParamsChanged || stepParamsChanged || routingParamsChanged;

        const bool allowFullRecalc = !hostIsStopped;

        // Step-gate reconfiguration reanchors sequence phase to a deterministic
        // origin. This avoids non-reproducible "mid-cycle" behavior when users
        // automate numerator/denominator/toggle while notes are flowing.
        if (stepParamsChanged || holdToggledOff)
        {
            currentStepCount = 0;
            currentStepPhase = 0;
        }

        // Hold policy:
        // - disabling hold must cut all latched notes immediately.
        // - while hold is ON, changing any other InputFilter setting also
        //   cuts/clears the held set to avoid stale/lurking latched notes.
        if (holdToggledOff || (holdActive && nonHoldParamsChanged))
        {
            forceAllNotesOffAndClear(outScratch, 0, nullptr);
            holdForcedReset = true;
        }

        if (!holdForcedReset && gateParamsChanged)
        {
            const bool heldSetChanged =
                pruneHeldNotesForCurrentGate(selectedChannel, muteActive, noteF, nMin, nMax,
                                             veloF, vMin, vMax);

            // Optimization: gate tweaks that do not alter held notes should not
            // force a full active-voice rebuild on every automation tick.
            //
            // While stopped, parameter edits still remove stale notes, but never
            // generate fresh NoteOn events.
            if (heldSetChanged)
                recalculateActiveVoicesWithPolicy(outScratch,
                                                  0,
                                                  allowFullRecalc,
                                                  limitEnabled,
                                                  maxVoices,
                                                  prio);
        }
        else if (!holdForcedReset && voiceParamsChanged)
        {
            recalculateActiveVoicesWithPolicy(outScratch,
                                              0,
                                              allowFullRecalc,
                                              limitEnabled,
                                              maxVoices,
                                              prio);
        }
    }
    else
    {
        // First block: initialize counters from current config.
        currentStepCount = 0;
        currentStepPhase = 0;
        lastSnapshotValid = true;
    }

    // Save snapshot for next block.
    lastSelectedChannel = selectedChannel;
    lastMuteActive      = muteActive;
    lastNoteF           = noteF;
    lastNMin            = nMin;
    lastNMax            = nMax;
    lastVeloF           = veloF;
    lastVMin            = vMin;
    lastVMax            = vMax;
    lastStepF           = stepF;
    lastStepNum         = stepNum;
    lastStepDen         = stepDen;
    lastLimitEnabled    = limitEnabled;
    lastMaxVoices       = maxVoices;
    lastPriority        = prio;
    lastConsumeRouting  = consumeActive;
    lastDirectRouting   = directActive;
    lastHoldEnabled     = holdActive;

    //--------------------------------------------------------------------------
    // 3) Process each incoming MIDI event
    //--------------------------------------------------------------------------
    // START retrigger:
    // If notes were held before transport start, re-emit the currently active
    // voice set once at sample 0 so downstream modules (arp/harmonizer/synth)
    // restart from a clean state.
    //
    // If real NoteOn messages are already present in this block, skip this
    // synthetic retrigger to avoid duplicate attacks.
    if (pendingHeldRetrigOnStart && !hostIsStopped)
    {
        if (!hasIncomingNoteOn)
            emitActiveVoicesRetrigger(outScratch, 0);

        pendingHeldRetrigOnStart = false;
    }

    for (auto it = midiMessages.begin(); it != midiMessages.end(); ++it)
    {
        const auto meta = *it;
        const auto& msg = meta.getMessage();
        const int sample = meta.samplePosition;

        const bool isNoteOn  = msg.isNoteOn()  && msg.getVelocity() > 0.0f;
        const bool isNoteOff = msg.isNoteOff() || (msg.isNoteOn() && msg.getVelocity() == 0.0f);

        //======================================================================
        // NOTE ON (vel > 0)
        //======================================================================
        if (isNoteOn)
        {
            const int ch  = msg.getChannel();
            const int nt  = msg.getNoteNumber();
            const int vel = velocityFloatToMidi127(msg.getVelocity());

            // 1) Channel filter (strict)
            if (ch != selectedChannel)
                continue;

            // 2) Mute
            if (muteActive)
                continue;

            // 3) Note range
            if (noteF && (nt < nMin || nt > nMax))
                continue;

            // 4) Velocity range
            if (!passesVelocityGate(veloF, vel, vMin, vMax))
                continue;

            // 5) Step filter (only after all previous checks pass)
            if (stepF)
            {
                // Step filter is NoteOn-only:
                // every candidate NoteOn advances step state exactly once.
                const bool pass = shouldPassStepFilterAndAdvance(stepNum, stepDen);
                if (!pass)
                {
                   #if LOGS_ENABLED && LOG_INPUTFILTER_PROCESSOR
                    DBG_LOG("ACTION", "STEPFILTER", "SKIP", "#STEP#",
                            juce::String("lane=") + juce::String(Lanes::laneSuffix(lane))
                            + " step=" + juce::String(currentStepCount)
                            + " num=" + juce::String(stepNum)
                            + " den=" + juce::String(stepDen)
                            + " ch=" + juce::String(ch)
                            + " note=" + juce::String(nt));
                   #endif
                    continue;
                }
            }

            // Passed all filters:
            // - this is a physical key-down for this lane
            // - in hold mode, physical gestures always replace stale latched-only
            //   notes (notes that are no longer physically held).
            markPhysicalNoteOn(ch, nt);
            if (holdActive)
                dropLatchedOnlyHeldNotes();

            // Add to held and recompute active voices.
            addToHeldNotes(ch, nt, vel);
            recalculateActiveVoicesWithPolicy(outScratch,
                                              sample,
                                              true,
                                              limitEnabled,
                                              maxVoices,
                                              prio);
        }
        //======================================================================
        // NOTE OFF (including NoteOn vel==0)
        //======================================================================
        else if (isNoteOff)
        {
            const int ch = msg.getChannel();
            const int nt = msg.getNoteNumber();
            int physicalReleaseChannel = -1;
            auto isPhysicallyHeldAnyChannel = [&](int note) noexcept
            {
                if (note < 0 || note > 127)
                    return false;

                for (int c = 1; c <= 16; ++c)
                    if (isPhysicallyHeld(c, note))
                        return true;

                return false;
            };
            auto releaseAllHeldChannelsForNote = [&](int note) noexcept
            {
                bool removedAny = false;
                for (int c = 1; c <= 16; ++c)
                {
                    if (!isHeldNote(c, note))
                        continue;

                    removeFromHeldNotes(c, note);
                    removedAny = true;
                }

                if (removedAny)
                {
                    recalculateActiveVoicesWithPolicy(outScratch,
                                                      sample,
                                                      true,
                                                      limitEnabled,
                                                      maxVoices,
                                                      prio);
                }
            };

            // Physical key-up tracking always runs, even when hold is ON.
            if (isPhysicallyHeld(ch, nt))
            {
                markPhysicalNoteOff(ch, nt);
                physicalReleaseChannel = ch;
            }
            else
            {
                int matchedPhysicalChannel = -1;
                if (findPhysicalNoteUniqueAnyChannel(nt, matchedPhysicalChannel))
                {
                    markPhysicalNoteOff(matchedPhysicalChannel, nt);
                    physicalReleaseChannel = matchedPhysicalChannel;
                }
            }

            // Hold ON: keep logical held notes latched.
            if (holdActive)
            {
                // Mark possible latched-only state so the next accepted NoteOn
                // can drop stale held notes deterministically.
                //
                // We intentionally fail-safe toward "maybe latched" in ambiguous
                // channel cases (same note on multiple channels): this only costs
                // one later scan in dropLatchedOnlyHeldNotes(), but avoids missed
                // cleanup paths that could leave stale held ownership.
                bool maybeLatched = false;

                const int probeChannel = (physicalReleaseChannel > 0) ? physicalReleaseChannel : ch;
                if (isHeldNote(probeChannel, nt) && !isPhysicallyHeld(probeChannel, nt))
                {
                    maybeLatched = true;
                }
                else
                {
                    int matchedHeldChannel = -1;
                    if (findHeldNoteUniqueAnyChannel(nt, matchedHeldChannel))
                    {
                        if (!isPhysicallyHeld(matchedHeldChannel, nt))
                            maybeLatched = true;
                    }
                    else if (physicalReleaseChannel > 0)
                    {
                        // Ambiguous logical ownership but we did observe a real
                        // physical release in this block.
                        maybeLatched = true;
                    }
                }

                if (maybeLatched)
                    hasLatchedOnlyHeldNotes = true;

                continue;
            }

            // Channel-change safety:
            //  - Prefer exact (ch,note)
            //  - Otherwise: ONLY accept if there is a UNIQUE held note with this
            //    note number on ANY channel.
            //    This avoids uncontrolled release when the same note number is
            //    held on multiple channels.
            int logicalReleaseChannel = -1;
            if (isHeldNote(ch, nt))
            {
                logicalReleaseChannel = ch;
            }
            else
            {
                int matchedChannel = -1;
                if (findHeldNoteUniqueAnyChannel(nt, matchedChannel))
                    logicalReleaseChannel = matchedChannel;
            }

            if (logicalReleaseChannel > 0)
            {
                // Keep logical note while it is still physically held.
                if (isPhysicallyHeld(logicalReleaseChannel, nt))
                    continue;

                removeFromHeldNotes(logicalReleaseChannel, nt);
                recalculateActiveVoicesWithPolicy(outScratch,
                                                  sample,
                                                  true,
                                                  limitEnabled,
                                                  maxVoices,
                                                  prio);
                continue;
            }

            // Conservative anti-stuck fallback:
            // if no physical key is currently down for this note number, but
            // logical held state still contains it (cross-channel ambiguity),
            // release all logical owners for this note.
            if (!isPhysicallyHeldAnyChannel(nt))
                releaseAllHeldChannelsForNote(nt);
        }
        //======================================================================
        // Everything else: pass-through
        //======================================================================
        else
        {
            outScratch.addEvent(msg, sample);
        }
    }

   #if LOGS_ENABLED && LOG_INPUTFILTER_PROCESSOR
    DBG_LOG("STATE", "INPUTFILTER", "BLOCK END", "#IFP99#",
            juce::String("lane=") + juce::String(Lanes::laneSuffix(lane))
            + " held=" + dumpHeldNotes()
            + " active=" + dumpVoices(activeVoices));
   #endif

    midiMessages.swapWith(outScratch);
}
