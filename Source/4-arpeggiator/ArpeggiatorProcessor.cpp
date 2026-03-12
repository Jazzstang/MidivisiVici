//==============================================================================
// ArpeggiatorProcessor.cpp
//------------------------------------------------------------------------------
// Vue d ensemble:
// - Implementation RT du moteur arpeggiator par lane.
// - Lit APVTS (via pointeurs caches), suit les notes tenues, calcule les ticks
//   musicaux et emet des notes generees.
//
// Architecture de traitement:
// 1) Entree bloc: analyse transport + discontinuite.
// 2) Segmentation temporelle intra-bloc autour des evenements MIDI entrants.
// 3) Sur chaque segment:
//    - drain off planifies
//    - drain queue differree
//    - avance ticks rate et genere notes
// 4) Merge final dans outScratch puis swap vers buffer lane.
//
// Contraintes RT:
// - zero allocation dynamique dans process.
// - zero lock.
// - ordre stable des evenements (ppq puis sequence).
//==============================================================================

#include "ArpeggiatorProcessor.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

namespace
{
    //--------------------------------------------------------------------------
    // Helpers locaux (sans etat global) pour conversion d index et stacks.
    //--------------------------------------------------------------------------
    constexpr int kRateTypeCount = 6;
    constexpr int kRateRatiosPerType = 7;
    constexpr int kRateChoiceCount = kRateTypeCount * kRateRatiosPerType; // 42
    constexpr int kVelocityChoiceFirst = 1; // ppp
    constexpr int kVelocityChoiceEqual = 4; // "="
    constexpr int kVelocityChoiceMf = 5;    // mf
    constexpr int kVelocityChoiceLast = 8;  // fff
    constexpr std::array<int, 8> kVelocityPercentValues {
        -100, -66, -33, 0, 0, 33, 66, 100
    };
    constexpr std::array<int, 8> kGateTimedPercentValues { 5, 10, 25, 33, 50, 66, 75, 99 };
    constexpr int kGateTimedChoiceFirst = 1;
    constexpr int kGateTimedChoiceLast = kGateTimedChoiceFirst + (int) kGateTimedPercentValues.size() - 1; // 8
    constexpr int kGateTieChoice = kGateTimedChoiceLast + 1; // 9
    constexpr int kGrooveChoiceFirst = 1;
    constexpr int kGrooveChoiceEqual = 8;
    constexpr int kGrooveChoiceLast = 15;
    constexpr std::array<int, 15> kGroovePercentValues {
        -75, -66, -50, -33, -25, -10, -5, 0, 5, 10, 25, 33, 50, 66, 75
    };

    static inline bool isRateRegularChoice(int choiceIndex) noexcept
    {
        return choiceIndex >= 1 && choiceIndex <= kRateChoiceCount;
    }

    static inline int rateChoiceType(int choiceIndex) noexcept
    {
        return juce::jlimit(0, kRateTypeCount - 1, (choiceIndex - 1) / kRateRatiosPerType);
    }

    static inline int rateChoiceSlot1Based(int choiceIndex) noexcept
    {
        return juce::jlimit(1, kRateRatiosPerType, ((choiceIndex - 1) % kRateRatiosPerType) + 1);
    }

    static inline int makeRateChoiceIndex(int type, int slot1Based) noexcept
    {
        const int safeType = juce::jlimit(0, kRateTypeCount - 1, type);
        const int safeSlot = juce::jlimit(1, kRateRatiosPerType, slot1Based);
        return (safeType * kRateRatiosPerType) + safeSlot;
    }

    static inline bool isVelocityFixedChoice(int choiceIndex) noexcept
    {
        return choiceIndex >= kVelocityChoiceFirst && choiceIndex <= kVelocityChoiceLast;
    }

    static inline int velocityPercentFromChoiceIndex(int choiceIndex) noexcept
    {
        if (!isVelocityFixedChoice(choiceIndex))
            return 0;

        return kVelocityPercentValues[(size_t) (choiceIndex - kVelocityChoiceFirst)];
    }

    static inline int choiceIndexFromVelocityPercent(double percent, bool allowEqual) noexcept
    {
        int bestChoice = allowEqual ? kVelocityChoiceEqual : kVelocityChoiceMf;
        double bestDistance = std::numeric_limits<double>::max();

        for (int choice = kVelocityChoiceFirst; choice <= kVelocityChoiceLast; ++choice)
        {
            if (!allowEqual && choice == kVelocityChoiceEqual)
                continue;

            const double d = std::abs(percent - (double) velocityPercentFromChoiceIndex(choice));
            if (d < bestDistance)
            {
                bestDistance = d;
                bestChoice = choice;
            }
        }

        return bestChoice;
    }

    static inline int applyVelocityPercentToSource(int sourceVelocity, int percent) noexcept
    {
        const int src = juce::jlimit(1, 127, sourceVelocity);
        const int pct = juce::jlimit(-100, 100, percent);

        if (pct >= 0)
        {
            const double t = (double) pct / 100.0;
            const double moved = (double) src + t * (double) (127 - src);
            return juce::jlimit(1, 127, juce::roundToInt((float) moved));
        }

        const double t = (double) (-pct) / 100.0;
        const double moved = (double) src - t * (double) (src - 1);
        return juce::jlimit(1, 127, juce::roundToInt((float) moved));
    }

    static inline bool isGateTimedChoice(int choiceIndex) noexcept
    {
        return choiceIndex >= kGateTimedChoiceFirst && choiceIndex <= kGateTimedChoiceLast;
    }

    static inline int gateTimedPercentForChoice(int choiceIndex) noexcept
    {
        if (!isGateTimedChoice(choiceIndex))
            return 0;

        return kGateTimedPercentValues[(size_t) (choiceIndex - kGateTimedChoiceFirst)];
    }

    static inline bool isGrooveFixedChoice(int choiceIndex) noexcept
    {
        return choiceIndex >= kGrooveChoiceFirst && choiceIndex <= kGrooveChoiceLast;
    }

    static inline int groovePercentFromChoice(int choiceIndex) noexcept
    {
        if (!isGrooveFixedChoice(choiceIndex))
            return 0;

        return kGroovePercentValues[(size_t) (choiceIndex - kGrooveChoiceFirst)];
    }

    static inline int grooveChoiceFromPercent(double percent, bool allowEqual) noexcept
    {
        int bestChoice = allowEqual ? kGrooveChoiceEqual : 7; // fallback on -5 when "=" excluded
        double bestDistance = std::numeric_limits<double>::max();

        for (int choice = kGrooveChoiceFirst; choice <= kGrooveChoiceLast; ++choice)
        {
            if (!allowEqual && choice == kGrooveChoiceEqual)
                continue;

            const double d = std::abs(percent - (double) groovePercentFromChoice(choice));
            if (d < bestDistance)
            {
                bestDistance = d;
                bestChoice = choice;
            }
        }

        return bestChoice;
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

    template <typename V>
    static inline uint8_t velocityFloatToMidi127(V v) noexcept
    {
        if constexpr (std::is_floating_point_v<V>)
        {
            const int v127 = (int) std::lround((double) v * 127.0);
            return (uint8_t) juce::jlimit(1, 127, v127);
        }

        return (uint8_t) juce::jlimit(1, 127, (int) v);
    }

    static inline int firstStepDefaultChoiceForLayer(int layer) noexcept
    {
        switch (layer)
        {
            case 0: return 5;    // Rate: 1/16
            case 1: return 2;    // Strum: Up
            case 2: return 1;    // Jump: 1
            case 3: return 5;    // Octave: 0
            case 4: return 4;    // Velocity: "="
            case 5: return 8;    // Groove: "="
            case 6: return 8;    // Gate: 99%
            case 7: return 1;    // Retrig: x1
            default: break;
        }

        return 1;
    }

    static inline bool decodeChordSpreadMode(int directionMode,
                                             bool& downDirection,
                                             double& spanFraction) noexcept
    {
        downDirection = false;
        spanFraction = 0.0;

        switch (directionMode)
        {
            case 11: downDirection = false; spanFraction = 1.0 / 4.0; return true;
            case 12: downDirection = false; spanFraction = 1.0 / 3.0; return true;
            case 13: downDirection = false; spanFraction = 1.0 / 2.0; return true;
            case 14: downDirection = false; spanFraction = 2.0 / 3.0; return true;
            case 15: downDirection = false; spanFraction = 3.0 / 4.0; return true;
            case 16: downDirection = false; spanFraction = 1.0;       return true;
            case 17: downDirection = true;  spanFraction = 1.0 / 4.0; return true;
            case 18: downDirection = true;  spanFraction = 1.0 / 3.0; return true;
            case 19: downDirection = true;  spanFraction = 1.0 / 2.0; return true;
            case 20: downDirection = true;  spanFraction = 2.0 / 3.0; return true;
            case 21: downDirection = true;  spanFraction = 3.0 / 4.0; return true;
            case 22: downDirection = true;  spanFraction = 1.0;       return true;
            default: break;
        }

        return false;
    }
}

bool ArpeggiatorProcessor::isNoteOnMessage(const juce::MidiMessage& msg) noexcept
{
    return msg.isNoteOn() && msg.getVelocity() > 0.0f;
}

bool ArpeggiatorProcessor::isNoteOffMessage(const juce::MidiMessage& msg) noexcept
{
    return msg.isNoteOff() || (msg.isNoteOn() && msg.getVelocity() == 0.0f);
}

ArpeggiatorProcessor::ArpeggiatorProcessor(juce::AudioProcessorValueTreeState& vts,
                                           Lanes::Lane laneIn)
    : parameters(vts)
    , lane(laneIn)
{
    const auto laneId = [this](const char* baseId)
    {
        return ParamIDs::lane(baseId, lane);
    };

    arpeggiatorEnableRaw = parameters.getRawParameterValue(laneId(ParamIDs::Base::arpeggiatorEnable));
    arpeggiatorModeRaw = parameters.getRawParameterValue(laneId(ParamIDs::Base::arpMode));

    static constexpr const char* kMorphIds[kNumMorphLayers] =
    {
        ParamIDs::Base::arpRateMorph,
        ParamIDs::Base::arpDirectionMorph,
        ParamIDs::Base::arpPatternMorph,
        ParamIDs::Base::arpRangeMorph,
        ParamIDs::Base::arpVelocityMorph,
        ParamIDs::Base::arpGrooveMorph,
        ParamIDs::Base::arpGateMorph,
        ParamIDs::Base::arpAccentMorph
    };

    static constexpr const char* kLinkIds[kNumLayers] =
    {
        ParamIDs::Base::arpRateLink,
        ParamIDs::Base::arpDirectionLink,
        ParamIDs::Base::arpPatternLink,
        ParamIDs::Base::arpRangeLink,
        ParamIDs::Base::arpVelocityLink,
        ParamIDs::Base::arpGrooveLink,
        ParamIDs::Base::arpGateLink,
        ParamIDs::Base::arpAccentLink
    };

    static constexpr const char* kUnlinkRateIds[kNumLayers] =
    {
        nullptr, // Rate layer has no autonomous-rate parameter.
        ParamIDs::Base::arpDirectionUnlinkRate,
        ParamIDs::Base::arpPatternUnlinkRate,
        ParamIDs::Base::arpRangeUnlinkRate,
        ParamIDs::Base::arpVelocityUnlinkRate,
        ParamIDs::Base::arpGrooveUnlinkRate,
        ParamIDs::Base::arpGateUnlinkRate,
        ParamIDs::Base::arpAccentUnlinkRate
    };

    static constexpr const char* kStepPrefixes[kNumLayers] =
    {
        ParamIDs::Base::arpRateSeqPrefix,
        ParamIDs::Base::arpDirectionSeqPrefix,
        ParamIDs::Base::arpPatternSeqPrefix,
        ParamIDs::Base::arpRangeSeqPrefix,
        ParamIDs::Base::arpVelocitySeqPrefix,
        ParamIDs::Base::arpGrooveSeqPrefix,
        ParamIDs::Base::arpGateSeqPrefix,
        ParamIDs::Base::arpAccentSeqPrefix
    };

    static constexpr const char* kDrumStepPrefixes[kNumDrumIndependentLayers] =
    {
        ParamIDs::Base::drumGraceSeqPrefix,   // kLayerDirection
        ParamIDs::Base::drumVeloEnvSeqPrefix, // kLayerPattern
        ParamIDs::Base::drumTimEnvSeqPrefix   // kLayerRange
    };

    for (int i = 0; i < kNumLayers; ++i)
    {
        if (juce::isPositiveAndBelow(i, kNumMorphLayers))
            arpMorphRaw[(size_t) i] = parameters.getRawParameterValue(laneId(kMorphIds[i]));
        linkRaw[(size_t) i] = parameters.getRawParameterValue(laneId(kLinkIds[i]));
        if (kUnlinkRateIds[i] != nullptr)
            unlinkRateRaw[(size_t) i] = parameters.getRawParameterValue(laneId(kUnlinkRateIds[i]));
        else
            unlinkRateRaw[(size_t) i] = nullptr;

        for (int s = 0; s < kNumSteps; ++s)
        {
            const auto sid = ParamIDs::laneStep(kStepPrefixes[i], s + 1, lane);
            stepChoices[(size_t) i][(size_t) s] =
                dynamic_cast<juce::AudioParameterChoice*>(parameters.getParameter(sid));
        }
    }

    for (int i = 0; i < kNumDrumIndependentLayers; ++i)
    {
        for (int s = 0; s < kNumSteps; ++s)
        {
            const auto sid = ParamIDs::laneStep(kDrumStepPrefixes[i], s + 1, lane);
            drumStepChoices[(size_t) i][(size_t) s] =
                dynamic_cast<juce::AudioParameterChoice*>(parameters.getParameter(sid));
        }
    }

    for (int i = 0; i < kNumLayers; ++i)
        lastLinkState[(size_t) i] = (uint8_t) readLinkModeRT(i);

    lastEnabled = readEnabledRT();

    outScratch.clear();
    // Capacite volontairement large pour limiter les reallocs en rafales
    // (pas denses + retrig + chord) dans le hot path.
    outScratch.ensureSize(16384);

    resetAllTrackingRT();
}

void ArpeggiatorProcessor::getClockSyncState(ClockSyncState& s) const noexcept
{
    s.valid = true;
    s.heldNoteFingerprint = computeHeldNoteFingerprint();
    s.heldNoteCount = heldActiveNoteCount;
    s.wasPlaying = wasPlaying;
    s.hasLastBlockPpq = hasLastBlockPpq;
    s.lastBlockPpq = lastBlockPpq;
    s.nextRateStepPpq = nextRateStepPpq;
    s.nextUnlinkStepPpqByLayer = nextUnlinkStepPpqByLayer;
    s.playbackCursorByLayer = playbackCursorByLayer;
    s.playbackStepByLayer = playbackStepByLayer;
    s.lastLinkModeByLayer = lastLinkState;
    s.currentDirectionMode = currentDirectionMode;
    s.currentJumpSize = currentJumpSize;
    s.currentStepDurationBeats = currentStepDurationBeats;
    s.currentOctaveOffset = currentOctaveOffset;
    s.currentIsDrumMode = currentIsDrumMode;
    s.currentGraceCount = currentGraceCount;
    s.currentDrumVeloEnvModeInt = (int) currentDrumVeloEnvMode;
    s.currentVelocityChoice = currentVelocityChoice;
    s.currentGrooveChoice = currentGrooveChoice;
    s.currentGateChoice = currentGateChoice;
    s.currentRetrigCount = currentRetrigCount;
    s.currentPaceModeInt = (int) currentPaceMode;
    s.arpWalkIndex = arpWalkIndex;
    s.randomSeed = playbackRandom.getSeed();
    s.deferredEventCount = juce::jlimit(0, kMaxDeferredEvents, deferredEventCount);
    s.deferredEventSequenceCounter = deferredEventSequenceCounter;

    for (int i = 0; i < s.deferredEventCount; ++i)
    {
        const auto& ev = deferredEvents[(size_t) i];
        s.deferredSequence[(size_t) i] = ev.sequence;
        s.deferredPpq[(size_t) i] = ev.ppq;
        s.deferredOffOrExtendPpq[(size_t) i] = ev.offOrExtendPpq;
        s.deferredChannel1[(size_t) i] = ev.channel1;
        s.deferredNote[(size_t) i] = ev.note;
        s.deferredVelocity[(size_t) i] = ev.velocity;
        s.deferredGateMode[(size_t) i] = (int) ev.gateMode;
        s.deferredType[(size_t) i] = (uint8_t) ev.type;
    }
}

void ArpeggiatorProcessor::applyClockSyncState(const ClockSyncState& s) noexcept
{
    if (!s.valid)
        return;

    wasPlaying = s.wasPlaying;
    hasLastBlockPpq = s.hasLastBlockPpq;
    lastBlockPpq = s.lastBlockPpq;
    nextRateStepPpq = s.nextRateStepPpq;
    nextUnlinkStepPpqByLayer = s.nextUnlinkStepPpqByLayer;
    playbackCursorByLayer = s.playbackCursorByLayer;
    playbackStepByLayer = s.playbackStepByLayer;
    lastLinkState = s.lastLinkModeByLayer;
    currentDirectionMode = s.currentDirectionMode;
    currentJumpSize = s.currentJumpSize;
    currentStepDurationBeats = s.currentStepDurationBeats;
    currentOctaveOffset = s.currentOctaveOffset;
    currentIsDrumMode = s.currentIsDrumMode;
    currentGraceCount = juce::jlimit(-1, 2, s.currentGraceCount);
    currentDrumVeloEnvMode = (DrumVeloEnvMode) juce::jlimit(0, 4, s.currentDrumVeloEnvModeInt);
    currentVelocityChoice = s.currentVelocityChoice;
    currentGrooveChoice = s.currentGrooveChoice;
    currentGateChoice = s.currentGateChoice;
    currentRetrigCount = juce::jlimit(kAccentChoiceFirst, kAccentChoiceLast, s.currentRetrigCount);
    currentPaceMode = (PaceMode) juce::jlimit(0, 4, s.currentPaceModeInt);
    arpWalkIndex = s.arpWalkIndex;
    playbackRandom.setSeed(s.randomSeed);

    clearDeferredEvents();
    deferredEventSequenceCounter = juce::jmax((uint64_t) 1, s.deferredEventSequenceCounter);
    const int syncedCount = juce::jlimit(0, kMaxDeferredEvents, s.deferredEventCount);
    uint64_t maxSyncedSequence = 0;

    for (int i = 0; i < syncedCount; ++i)
    {
        const double ppq = s.deferredPpq[(size_t) i];
        if (!std::isfinite(ppq))
            continue;

        DeferredEvent ev;
        ev.ppq = ppq;
        ev.offOrExtendPpq = s.deferredOffOrExtendPpq[(size_t) i];
        ev.sequence = juce::jmax((uint64_t) 1, s.deferredSequence[(size_t) i]);
        maxSyncedSequence = juce::jmax(maxSyncedSequence, ev.sequence);
        ev.channel1 = juce::jlimit(1, kNumMidiChannels, s.deferredChannel1[(size_t) i]);
        ev.note = juce::jlimit(0, kNumNotes - 1, s.deferredNote[(size_t) i]);
        ev.velocity = juce::jlimit(1, 127, s.deferredVelocity[(size_t) i]);
        ev.gateMode = (GateMode) juce::jlimit(0, 2, s.deferredGateMode[(size_t) i]);
        ev.type = (DeferredEventType) juce::jlimit(0, 2, (int) s.deferredType[(size_t) i]);

        deferredEvents[(size_t) deferredEventCount] = ev;
        ++deferredEventCount;
    }

    deferredEventSequenceCounter = juce::jmax(deferredEventSequenceCounter, maxSyncedSequence + 1);
    rebuildDeferredEventHeap();
}

int ArpeggiatorProcessor::getPlaybackStepForLayer(int layerIndex0Based) const noexcept
{
    if (!juce::isPositiveAndBelow(layerIndex0Based, kNumLayers))
        return -1;

    const int step = playbackStepByLayer[(size_t) layerIndex0Based];
    return juce::isPositiveAndBelow(step, kNumSteps) ? step : -1;
}

void ArpeggiatorProcessor::getDeferredQueueTelemetry(DeferredQueueTelemetry& telemetry) const noexcept
{
    telemetry = deferredTelemetry;
}

void ArpeggiatorProcessor::resetDeferredQueueTelemetry() noexcept
{
    deferredTelemetry = {};
}

void ArpeggiatorProcessor::resetAllTrackingRT() noexcept
{
    for (auto& byNote : heldCount) byNote.fill(0);
    for (auto& byNote : heldVelocity) byNote.fill(0);
    for (auto& byNote : heldVelocityStack)
        for (auto& byVel : byNote)
            byVel.fill(0);
    for (auto& byNote : heldVelocityStackSize) byNote.fill(0);
    for (auto& byNote : heldTimestamp) byNote.fill(0);
    heldKeysSortedCount = 0;
    heldActiveNoteCount = 0;

    for (auto& byNote : generatedActive) byNote.fill(0);
    for (auto& byNote : generatedVelocity) byNote.fill(0);
    for (auto& byNote : scheduledOffActive) byNote.fill(0);
    for (auto& byNote : scheduledOffPpq) byNote.fill(0.0);
    generatedActiveCount = 0;
    scheduledOffActiveCount = 0;

    timestampCounter = 0;
    for (auto& byNote : selectedSeenStamp) byNote.fill(0);
    selectedSeenGeneration = 1;
    clearPlaybackState();
    arpWalkIndex = -1;
    wasPlaying = false;
    hasLastBlockPpq = false;
    lastBlockPpq = 0.0;
    resetDeferredQueueTelemetry();

    outScratch.clear();
}

void ArpeggiatorProcessor::setExternalMorphModulations(const ExternalMorphModulations& mod) noexcept
{
    externalMorphModulations = mod;
}

bool ArpeggiatorProcessor::readEnabledRT() const noexcept
{
    if (arpeggiatorEnableRaw != nullptr)
        return arpeggiatorEnableRaw->load() > 0.5f;

    return true;
}

int ArpeggiatorProcessor::readModeRT() const noexcept
{
    if (arpeggiatorModeRaw != nullptr)
        return juce::jlimit(0, 1, juce::roundToInt(arpeggiatorModeRaw->load()));

    return 0;
}

juce::AudioParameterChoice* ArpeggiatorProcessor::getStepChoiceParamRT(int modeIndex,
                                                                        int layer,
                                                                        int step0Based) const noexcept
{
    if (!juce::isPositiveAndBelow(layer, kNumLayers) || !juce::isPositiveAndBelow(step0Based, kNumSteps))
        return nullptr;

    const bool drumMode = (modeIndex == 1);
    if (drumMode)
    {
        if (layer == kLayerDirection)
            if (auto* p = drumStepChoices[0][(size_t) step0Based])
                return p;
        if (layer == kLayerPattern)
            if (auto* p = drumStepChoices[1][(size_t) step0Based])
                return p;
        if (layer == kLayerRange)
            if (auto* p = drumStepChoices[2][(size_t) step0Based])
                return p;

        // Robust fallback:
        // If a drum-specific page parameter is temporarily unavailable
        // (host restore edge / stale layout), keep engine deterministic by
        // reading the shared arp page instead of returning null.
    }

    return stepChoices[(size_t) layer][(size_t) step0Based];
}

int ArpeggiatorProcessor::readStepChoiceForModeRT(int modeIndex,
                                                  int layer,
                                                  int step0Based) const noexcept
{
    if (!juce::isPositiveAndBelow(layer, kNumLayers) || !juce::isPositiveAndBelow(step0Based, kNumSteps))
        return 0;

    int idx = 0;
    if (auto* p = getStepChoiceParamRT(modeIndex, layer, step0Based))
        idx = p->getIndex();

    // Hard invariant:
    // step 1 must always have a valid musical value, never Skip.
    //
    // Important: keep this invariant even if parameter lookup failed
    // (idx==0 because param pointer is null), otherwise the Rate layer can
    // remain stuck in Skip forever and the sequencer never ticks.
    if (step0Based == 0 && idx == 0)
    {
        if (modeIndex == 1
            && (layer == kLayerDirection || layer == kLayerPattern || layer == kLayerRange))
        {
            idx = 1; // Drum mode: Hit/Velo Env/Tim Env defaults to "X"/"-"/"-".
        }
        else
        {
            idx = firstStepDefaultChoiceForLayer(layer);
        }
    }

    return idx;
}

int ArpeggiatorProcessor::readStepChoiceRT(int layer, int step0Based) const noexcept
{
    const int modeIndex = readModeRT();
    return readStepChoiceForModeRT(modeIndex, layer, step0Based);
}

int ArpeggiatorProcessor::readLinkModeRT(int layer) const noexcept
{
    if (!juce::isPositiveAndBelow(layer, kNumLayers))
        return kLinkModeF;

    // Rythm (layer rate) is binary:
    // 0 = R (reset/restart on held-note activity), 1 = F (free-run on DAW timeline).
    if (layer == kLayerRate)
    {
        if (auto* p = linkRaw[(size_t) layer])
            return juce::jlimit(kLinkModeLink, kLinkModeF, juce::roundToInt(p->load()));

        return kLinkModeF;
    }

    if (auto* p = linkRaw[(size_t) layer])
        return juce::jlimit(kLinkModeLink, kLinkModeUnlink, juce::roundToInt(p->load()));

    return kLinkModeF;
}

int ArpeggiatorProcessor::readUnlinkRateChoiceRT(int layer) const noexcept
{
    if (!juce::isPositiveAndBelow(layer, kNumLayers))
        return 5;

    if (layer == kLayerRate)
        return 5;

    if (auto* p = unlinkRateRaw[(size_t) layer])
    {
        int idx = juce::roundToInt(p->load());
        if (idx <= 0) // Skip is invalid for autonomous rate clocks.
            idx = 5;
        return juce::jlimit(1, kRateRndIndex, idx);
    }

    return 5;
}

int ArpeggiatorProcessor::readMorphRT(int layer) const noexcept
{
    if (!juce::isPositiveAndBelow(layer, kNumMorphLayers))
        return 0;

    const float normDelta =
        juce::jlimit(-1.0f, 1.0f, externalMorphModulations.normDeltas[(size_t) layer]);

    if (auto* p = arpMorphRaw[(size_t) layer])
    {
        const float base = p->load();
        const float baseNorm = juce::jlimit(0.0f, 1.0f, (base + 100.0f) / 200.0f);
        const float effectiveNorm = juce::jlimit(0.0f, 1.0f, baseNorm + normDelta);
        const float effective = (effectiveNorm * 200.0f) - 100.0f;
        return juce::jlimit(-100, 100, juce::roundToInt(effective));
    }

    return 0;
}

bool ArpeggiatorProcessor::hasAnyHeldNotesRT() const noexcept
{
    return heldActiveNoteCount > 0;
}

void ArpeggiatorProcessor::resetLinkedSequencersForIdle(double ppqNow) noexcept
{
    // Global R mode contract:
    // when no held notes remain, all sequencers return to step 1.
    // Next note-on restarts deterministically from this state.
    playbackCursorByLayer[(size_t) kLayerRate] = 0;
    playbackStepByLayer[(size_t) kLayerRate] = -1;
    nextUnlinkStepPpqByLayer[(size_t) kLayerRate] = ppqNow;

    for (int layer = 1; layer < kNumLayers; ++layer)
    {
        playbackCursorByLayer[(size_t) layer] = 0;
        playbackStepByLayer[(size_t) layer] = -1;
        nextUnlinkStepPpqByLayer[(size_t) layer] = ppqNow;
    }

    // Rebase de l etat derive pour garantir un redemarrage deterministe.
    arpWalkIndex = -1;
    currentDirectionMode = 1;
    currentJumpSize = 1;
    currentStepDurationBeats = 0.25;
    currentOctaveOffset = 0;
    currentIsDrumMode = false;
    currentGraceCount = 0;
    currentDrumVeloEnvMode = DrumVeloEnvMode::Flat;
    currentVelocityChoice = 0;
    currentGrooveChoice = 0;
    currentGateChoice = 0;
    currentRetrigCount = 1;
    currentPaceMode = PaceMode::Equal;

    nextRateStepPpq = ppqNow;
}

void ArpeggiatorProcessor::reconcileLinkTransitions(double ppqNow) noexcept
{
    bool anyChange = false;
    bool jumpLayerLinkChanged = false;
    bool rythmModeChanged = false;
    bool anyLayerChangedToF = false;
    std::array<uint8_t, kNumLayers> changedToF {};
    std::array<uint8_t, kNumLayers> changedToUnlink {};

    for (int layer = 0; layer < kNumLayers; ++layer)
    {
        const int current = readLinkModeRT(layer);
        const int previous = (int) lastLinkState[(size_t) layer];
        if (current == previous)
            continue;

        anyChange = true;
        if (layer == kLayerRate)
            rythmModeChanged = true;
        if (layer == kLayerPattern)
            jumpLayerLinkChanged = true;
        lastLinkState[(size_t) layer] = (uint8_t) current;

        if (current == kLinkModeF)
        {
            changedToF[(size_t) layer] = 1;
            anyLayerChangedToF = true;
        }
        else if (current == kLinkModeUnlink)
        {
            changedToUnlink[(size_t) layer] = 1;
        }
    }

    if (!anyChange)
        return;

    const int modeIndex = readModeRT();
    std::array<int, kNumLayers> safeLengthByLayer {};
    computeEffectiveSequenceLengthsForModeRT(modeIndex, safeLengthByLayer);

    const bool rythmResetMode = (readLinkModeRT(kLayerRate) == kLinkModeLink); // Link index == "R".
    const bool anyHeld = hasAnyHeldNotesRT();
    if (rythmModeChanged && rythmResetMode && !anyHeld)
        resetLinkedSequencersForIdle(ppqNow);
    else if (anyLayerChangedToF && !anyHeld && rythmResetMode)
        resetLinkedSequencersForIdle(ppqNow);
    else if (anyLayerChangedToF)
    {
        const int safeRateLength = safeLengthByLayer[(size_t) kLayerRate];
        const int safeRateCursor = juce::jlimit(0, safeRateLength - 1, playbackCursorByLayer[(size_t) kLayerRate]);

        for (int layer = 1; layer < kNumLayers; ++layer)
        {
            if (changedToF[(size_t) layer] == 0)
                continue;

            const int safeLayerLength = safeLengthByLayer[(size_t) layer];
            int mapped = 0;

            if (safeRateLength > 1 && safeLayerLength > 1)
            {
                const double phase01 = (double) safeRateCursor / (double) safeRateLength;
                mapped = juce::jlimit(0, safeLayerLength - 1,
                                      juce::roundToInt((float) (phase01 * (double) safeLayerLength)));
            }

            playbackCursorByLayer[(size_t) layer] = mapped;
            playbackStepByLayer[(size_t) layer] = -1;
            nextUnlinkStepPpqByLayer[(size_t) layer] = ppqNow;
        }
    }

    for (int layer = 1; layer < kNumLayers; ++layer)
    {
        if (changedToUnlink[(size_t) layer] == 0)
            continue;

        const int safeLayerLength = safeLengthByLayer[(size_t) layer];
        playbackCursorByLayer[(size_t) layer] =
            juce::jlimit(0, safeLayerLength - 1, playbackCursorByLayer[(size_t) layer]);

        if (!juce::isPositiveAndBelow(playbackStepByLayer[(size_t) layer], safeLayerLength))
            playbackStepByLayer[(size_t) layer] = playbackCursorByLayer[(size_t) layer];

        nextUnlinkStepPpqByLayer[(size_t) layer] = ppqNow;
    }

    if (jumpLayerLinkChanged)
        arpWalkIndex = -1;

    nextRateStepPpq = juce::jmax(nextRateStepPpq, ppqNow);
    sanitizePlaybackCursorsToLengthsRT();
}

void ArpeggiatorProcessor::handleTrackedNoteOn(int channel1, int note, uint8_t velocity) noexcept
{
    const int ch0 = juce::jlimit(1, kNumMidiChannels, channel1) - 1;
    const int nt  = juce::jlimit(0, kNumNotes - 1, note);

    auto& count = heldCount[(size_t) ch0][(size_t) nt];
    auto& vel = heldVelocity[(size_t) ch0][(size_t) nt];
    auto& stack = heldVelocityStack[(size_t) ch0][(size_t) nt];
    auto& stackSize = heldVelocityStackSize[(size_t) ch0][(size_t) nt];

    const bool becameActive = (count == 0);
    if (becameActive && heldActiveNoteCount < (kNumMidiChannels * kNumNotes))
        ++heldActiveNoteCount;

    if (count < 255)
        ++count;

    pushVelocityStack(stack, stackSize, velocity);
    vel = topVelocityStack(stack, stackSize, velocity);
    heldTimestamp[(size_t) ch0][(size_t) nt] = ++timestampCounter;

    if (becameActive)
        insertHeldKeySortedRT(ch0, nt);
}

void ArpeggiatorProcessor::handleTrackedNoteOff(int channel1, int note) noexcept
{
    const int preferredCh0 = juce::jlimit(1, kNumMidiChannels, channel1) - 1;
    const int nt = juce::jlimit(0, kNumNotes - 1, note);

    int releaseCh0 = preferredCh0;
    if (heldCount[(size_t) releaseCh0][(size_t) nt] == 0)
    {
        // Defensive cross-channel fallback:
        // if NOTE OFF arrives on a different channel than NOTE ON, release only
        // when ownership is unique for this pitch class.
        int matched = -1;
        for (int ch0 = 0; ch0 < kNumMidiChannels; ++ch0)
        {
            if (heldCount[(size_t) ch0][(size_t) nt] == 0)
                continue;

            if (matched >= 0)
                return; // Ambiguous ownership, ignore to avoid accidental release.

            matched = ch0;
        }

        if (matched < 0)
            return;

        releaseCh0 = matched;
    }

    auto& count = heldCount[(size_t) releaseCh0][(size_t) nt];
    auto& vel = heldVelocity[(size_t) releaseCh0][(size_t) nt];
    auto& stack = heldVelocityStack[(size_t) releaseCh0][(size_t) nt];
    auto& stackSize = heldVelocityStackSize[(size_t) releaseCh0][(size_t) nt];

    if (count == 0)
        return;

    --count;

    if (stackSize > 0)
        --stackSize;

    if (count == 0)
    {
        if (heldActiveNoteCount > 0)
            --heldActiveNoteCount;
        removeHeldKeySortedRT(releaseCh0, nt);

        vel = 0;
        stackSize = 0;
        stack.fill(0);
        heldTimestamp[(size_t) releaseCh0][(size_t) nt] = 0;
    }
    else
    {
        vel = topVelocityStack(stack, stackSize, vel > 0 ? vel : (uint8_t) 100);
    }
}

void ArpeggiatorProcessor::insertHeldKeySortedRT(int ch0, int note) noexcept
{
    if (heldKeysSortedCount >= kMaxHeldKeys)
    {
        // Etat improbable (cache plein) -> rebuild defensif pour rester coherent.
        rebuildHeldKeyCacheFromMatrixRT();
        if (heldKeysSortedCount >= kMaxHeldKeys)
            return;
    }

    const int key = ch0 * kNumNotes + note;
    int insertAt = heldKeysSortedCount;

    for (int i = 0; i < heldKeysSortedCount; ++i)
    {
        const int existingKey = heldKeysSorted[(size_t) i];
        const int existingCh0 = existingKey / kNumNotes;
        const int existingNote = existingKey % kNumNotes;

        if (note < existingNote || (note == existingNote && ch0 < existingCh0))
        {
            insertAt = i;
            break;
        }
    }

    for (int i = heldKeysSortedCount; i > insertAt; --i)
        heldKeysSorted[(size_t) i] = heldKeysSorted[(size_t) (i - 1)];

    heldKeysSorted[(size_t) insertAt] = key;
    ++heldKeysSortedCount;
}

void ArpeggiatorProcessor::removeHeldKeySortedRT(int ch0, int note) noexcept
{
    const int key = ch0 * kNumNotes + note;
    int index = -1;

    for (int i = 0; i < heldKeysSortedCount; ++i)
    {
        if (heldKeysSorted[(size_t) i] == key)
        {
            index = i;
            break;
        }
    }

    if (index < 0)
    {
        // Desync rare (editions/restore) -> rebuild pour retrouver un cache sain.
        rebuildHeldKeyCacheFromMatrixRT();
        return;
    }

    for (int i = index; i + 1 < heldKeysSortedCount; ++i)
        heldKeysSorted[(size_t) i] = heldKeysSorted[(size_t) (i + 1)];

    if (heldKeysSortedCount > 0)
        --heldKeysSortedCount;
}

void ArpeggiatorProcessor::rebuildHeldKeyCacheFromMatrixRT() noexcept
{
    int dst = 0;
    for (int nt = 0; nt < kNumNotes && dst < kMaxHeldKeys; ++nt)
    {
        for (int ch0 = 0; ch0 < kNumMidiChannels && dst < kMaxHeldKeys; ++ch0)
        {
            if (heldCount[(size_t) ch0][(size_t) nt] == 0)
                continue;

            heldKeysSorted[(size_t) dst++] = ch0 * kNumNotes + nt;
        }
    }

    heldKeysSortedCount = dst;
    heldActiveNoteCount = dst;
}

int ArpeggiatorProcessor::applyModifierToStepChoice(int layer, int choiceIndex) const noexcept
{
    const int mod = readMorphRT(layer);
    if (mod == 0)
        return choiceIndex;

    switch (layer)
    {
        case kLayerRate:
        {
            if (!isRateRegularChoice(choiceIndex))
                return choiceIndex;

            // Invariant musical important:
            // le morph rate ne change jamais de "type" (binaries/dotted/...).
            // Il ne deplace que le slot vertical a l interieur du type courant.
            const int type = rateChoiceType(choiceIndex);
            const int slot = rateChoiceSlot1Based(choiceIndex); // 1..7

            if (mod > 0)
            {
                const double t = (double) mod / 100.0;
                const double movedSlot = (double) slot + t * (double) (kRateRatiosPerType - slot);
                return makeRateChoiceIndex(type,
                                           juce::jlimit(1, kRateRatiosPerType, juce::roundToInt((float) movedSlot)));
            }

            const double t = (double) (-mod) / 100.0;
            const double movedSlot = (double) slot - t * (double) (slot - 1);
            return makeRateChoiceIndex(type,
                                       juce::jlimit(1, kRateRatiosPerType, juce::roundToInt((float) movedSlot)));
        }

        case kLayerGate:
        {
            // Gate morph acts on the discrete timed gate set.
            // Skip/Tie remain stable for predictable musical behavior.
            if (!isGateTimedChoice(choiceIndex))
                return choiceIndex;

            const int slot = choiceIndex - kGateTimedChoiceFirst; // 0..7
            constexpr int kLastSlot = (kGateTimedChoiceLast - kGateTimedChoiceFirst);
            if (mod > 0)
            {
                const double t = (double) mod / 100.0;
                const double moved = (double) slot + t * (double) (kLastSlot - slot);
                return kGateTimedChoiceFirst
                    + juce::jlimit(0, kLastSlot, juce::roundToInt((float) moved));
            }

            const double t = (double) (-mod) / 100.0;
            const double moved = (double) slot - t * (double) slot;
            return kGateTimedChoiceFirst
                + juce::jlimit(0, kLastSlot, juce::roundToInt((float) moved));
        }

        case kLayerGroove:
        {
            if (!isGrooveFixedChoice(choiceIndex))
                return choiceIndex;

            // Skip (0) and "=" (8) are not morphed.
            if (choiceIndex == kGrooveChoiceEqual)
                return choiceIndex;

            const int groovePercent = groovePercentFromChoice(choiceIndex);

            if (mod > 0)
            {
                const double t = (double) mod / 100.0;
                const double moved = (double) groovePercent + t * (double) (-75 - groovePercent);
                return grooveChoiceFromPercent(moved, false);
            }

            const double t = (double) (-mod) / 100.0;
            const double moved = (double) groovePercent + t * (double) (75 - groovePercent);
            return grooveChoiceFromPercent(moved, false);
        }

        case kLayerRange:
        {
            // Octave morph policy:
            // - applique uniquement sur les choix fixes -4..+4 (1..9)
            // - Skip(0) et Rnd(10) restent inchanges
            // - mod positif pousse vers +4, mod negatif vers -4
            if (choiceIndex < kRangeChoiceFirst || choiceIndex > kRangeChoiceLast)
                return choiceIndex;

            const int baseOffset = choiceIndex - 5; // 1..9 -> -4..+4

            if (mod > 0)
            {
                const double t = (double) mod / 100.0;
                const double moved = (double) baseOffset + t * (double) (4 - baseOffset);
                return juce::jlimit(kRangeChoiceFirst,
                                    kRangeChoiceLast,
                                    juce::roundToInt((float) moved) + 5);
            }

            const double t = (double) (-mod) / 100.0;
            const double moved = (double) baseOffset + t * (double) (-4 - baseOffset);
            return juce::jlimit(kRangeChoiceFirst,
                                kRangeChoiceLast,
                                juce::roundToInt((float) moved) + 5);
        }

        case kLayerAccent:
        {
            // Retrig morph policy:
            // - applique uniquement sur x1..x8
            // - Skip(0) et Rnd(9) restent inchanges
            // - mod positif pousse vers x8, mod negatif vers x1
            if (choiceIndex < kAccentChoiceFirst || choiceIndex > kAccentChoiceLast)
                return choiceIndex;

            const int baseRetrig = choiceIndex; // 1..8

            if (mod > 0)
            {
                const double t = (double) mod / 100.0;
                const double moved = (double) baseRetrig
                                   + t * (double) (kAccentChoiceLast - baseRetrig);
                return juce::jlimit(kAccentChoiceFirst,
                                    kAccentChoiceLast,
                                    juce::roundToInt((float) moved));
            }

            const double t = (double) (-mod) / 100.0;
            const double moved = (double) baseRetrig
                               + t * (double) (kAccentChoiceFirst - baseRetrig);
            return juce::jlimit(kAccentChoiceFirst,
                                kAccentChoiceLast,
                                juce::roundToInt((float) moved));
        }

        case kLayerVelocity:
        {
            if (!isVelocityFixedChoice(choiceIndex))
                return choiceIndex;

            // Skip (0) and "=" (4) are not morphed.
            if (choiceIndex == kVelocityChoiceEqual)
                return choiceIndex;

            const int basePercent = velocityPercentFromChoiceIndex(choiceIndex);
            // Velocity morph policy:
            // - positive morph pulls toward +100%
            // - negative morph pulls toward -100%
            // - Skip/"=" remain unchanged.

            if (mod > 0)
            {
                const double t = (double) mod / 100.0;
                const double moved = (double) basePercent + t * (double) (100 - basePercent);
                return choiceIndexFromVelocityPercent(moved, false);
            }

            const double t = (double) (-mod) / 100.0;
            const double moved = (double) basePercent + t * (double) (-100 - basePercent);
            return choiceIndexFromVelocityPercent(moved, false);
        }

        default:
            break;
    }

    return choiceIndex;
}

int ArpeggiatorProcessor::findNextNonSkipStepForLayerForModeRT(int modeIndex,
                                                                int layer,
                                                                int startStep0Based,
                                                                int safeLayerLength) const noexcept
{
    if (!juce::isPositiveAndBelow(layer, kNumLayers))
        return 0;

    const int safeLength = juce::jmax(1, safeLayerLength);
    int step = juce::jlimit(0, safeLength - 1, startStep0Based);

    for (int scan = 0; scan < safeLength; ++scan)
    {
        const int choice = applyModifierToStepChoice(layer,
                                                     readStepChoiceForModeRT(modeIndex, layer, step));
        if (choice != 0)
            return step;

        step = (step + 1) % safeLength;
    }

    // Defensive fallback: if all steps resolve to Skip, keep start cursor.
    return juce::jlimit(0, safeLength - 1, startStep0Based);
}

int ArpeggiatorProcessor::effectiveSequenceLengthForLayerRT(int layer) const noexcept
{
    return effectiveSequenceLengthForLayerForModeRT(layer, readModeRT());
}

int ArpeggiatorProcessor::effectiveSequenceLengthForLayerForModeRT(int layer,
                                                                   int modeIndex) const noexcept
{
    if (!juce::isPositiveAndBelow(layer, kNumLayers))
        return kNumSteps;

    // Scan inverse:
    // dans le cas nominal, la fin de sequence contient beaucoup de Skip.
    // On trouve la longueur effective en sortant des qu on touche un pas actif.
    for (int step = kNumSteps - 1; step >= 0; --step)
    {
        if (readStepChoiceForModeRT(modeIndex, layer, step) != 0)
            return step + 1;
    }

    // Le pas 1 est force non-skip par readStepChoiceRT, mais on garde un fallback
    // defensif pour rester robuste si l invariant change.
    return 1;
}

void ArpeggiatorProcessor::computeEffectiveSequenceLengthsForModeRT(int modeIndex,
                                                                    std::array<int, kNumLayers>& outLengths) const noexcept
{
    for (int layer = 0; layer < kNumLayers; ++layer)
        outLengths[(size_t) layer] = juce::jmax(1, effectiveSequenceLengthForLayerForModeRT(layer, modeIndex));
}

void ArpeggiatorProcessor::sanitizePlaybackCursorsToLengthsRT() noexcept
{
    const int modeIndex = readModeRT();
    std::array<int, kNumLayers> safeLengthByLayer {};
    computeEffectiveSequenceLengthsForModeRT(modeIndex, safeLengthByLayer);

    for (int layer = 0; layer < kNumLayers; ++layer)
    {
        const int safeLength = safeLengthByLayer[(size_t) layer];

        auto& cursor = playbackCursorByLayer[(size_t) layer];
        if (!juce::isPositiveAndBelow(cursor, safeLength))
            cursor = 0;

        const int step = playbackStepByLayer[(size_t) layer];
        if (!juce::isPositiveAndBelow(step, safeLength))
            playbackStepByLayer[(size_t) layer] = -1;
    }
}

double ArpeggiatorProcessor::beatsForRateChoice(int choiceIndex) const noexcept
{
    if (!isRateRegularChoice(choiceIndex))
        return 0.25; // fallback: 1/16

    const int type = rateChoiceType(choiceIndex);
    const int slot = rateChoiceSlot1Based(choiceIndex) - 1; // 0..6

    double ratioNumerator = 1.0;
    double ratioDenominator = 1.0;

    switch (type)
    {
        case 0: // Binaries
            ratioNumerator = 1.0;
            ratioDenominator = (double) (1u << slot);
            break;

        case 1: // Dotted
            ratioNumerator = 3.0;
            ratioDenominator = (double) (1u << (slot + 1));
            break;

        case 2: // Double dotted
            ratioNumerator = 7.0;
            ratioDenominator = (double) (1u << (slot + 2));
            break;

        case 3: // Triolet
            ratioNumerator = 2.0;
            ratioDenominator = 3.0 * (double) (1u << slot);
            break;

        case 4: // Quintolet
            ratioNumerator = 4.0;
            ratioDenominator = 5.0 * (double) (1u << slot);
            break;

        case 5: // Septolet
            ratioNumerator = 4.0;
            ratioDenominator = 7.0 * (double) (1u << slot);
            break;

        default:
            return 0.25;
    }

    return 4.0 * (ratioNumerator / ratioDenominator); // quarter-note beats
}

void ArpeggiatorProcessor::clearPlaybackState() noexcept
{
    playbackStepByLayer.fill(-1);
    playbackCursorByLayer.fill(0);
    nextRateStepPpq = 0.0;
    nextUnlinkStepPpqByLayer.fill(0.0);
    currentDirectionMode = 1;
    currentJumpSize = 1;
    currentStepDurationBeats = 0.25;
    currentOctaveOffset = 0;
    currentIsDrumMode = false;
    currentGraceCount = 0;
    currentDrumVeloEnvMode = DrumVeloEnvMode::Flat;
    currentVelocityChoice = 0;
    currentGrooveChoice = 0;
    currentGateChoice = 0;
    currentRetrigCount = 1;
    currentPaceMode = PaceMode::Equal;
    arpWalkIndex = -1;
    rateIdleResetLatched = false;
    clearDeferredEvents();
}

void ArpeggiatorProcessor::resetPlaybackState(double ppqNow) noexcept
{
    clearPlaybackState();
    nextRateStepPpq = ppqNow;
    for (int layer = 0; layer < kNumLayers; ++layer)
        nextUnlinkStepPpqByLayer[(size_t) layer] = ppqNow;
}

bool ArpeggiatorProcessor::triggerOneRateStep() noexcept
{
    // Cache local par tick:
    // - mode lu une seule fois (evite melange Arp/Drum dans ce tick)
    // - longueurs effectives calculees une fois pour toutes les couches
    // Ceci reduit les rescans O(steps) sous forte activite.
    const int modeIndex = readModeRT();
    std::array<int, kNumLayers> safeLengthByLayer {};
    computeEffectiveSequenceLengthsForModeRT(modeIndex, safeLengthByLayer);

    // Cache local des modes de sync:
    // - F: reset sur wrap Rythm.
    // - Link: suit Rythm sans reset.
    // - Unlink: horloge autonome (non avance dans triggerOneRateStep).
    std::array<uint8_t, kNumLayers> linkModeByLayer {};
    for (int layer = 0; layer < kNumLayers; ++layer)
        linkModeByLayer[(size_t) layer] = (uint8_t) readLinkModeRT(layer);

    const int safeRateLength = safeLengthByLayer[(size_t) kLayerRate];

    const auto findNextValidTick = [this, modeIndex, safeRateLength, &safeLengthByLayer, &linkModeByLayer](const std::array<int, kNumLayers>& cursorsIn,
                                                                                                              std::array<int, kNumLayers>& outStepByLayer,
                                                                                                              std::array<int, kNumLayers>& outCursorsAfter,
                                                                                                              int& outRateChoiceRaw) -> bool
    {
        auto cursors = cursorsIn;

        for (int safety = 0; safety < kNumSteps; ++safety)
        {
            const int rateStep = juce::jlimit(0, safeRateLength - 1, cursors[(size_t) kLayerRate]);
            const int rateChoice = applyModifierToStepChoice(kLayerRate,
                                                             readStepChoiceForModeRT(modeIndex, kLayerRate, rateStep));

            const int nextRateCursor = (rateStep + 1) % safeRateLength;
            const bool didRateWrap = (nextRateCursor == 0);

            if (rateChoice == 0)
            {
                // Skip rate step: only rate cursor advances.
                cursors[(size_t) kLayerRate] = nextRateCursor;
                continue;
            }

            outStepByLayer.fill(-1);
            outStepByLayer[(size_t) kLayerRate] = rateStep;

            outCursorsAfter = cursors;
            outCursorsAfter[(size_t) kLayerRate] = nextRateCursor;

            for (int layer = 1; layer < kNumLayers; ++layer)
            {
                const int safeLayerLength = safeLengthByLayer[(size_t) layer];
                const int mode = (int) linkModeByLayer[(size_t) layer];

                if (mode == kLinkModeUnlink)
                {
                    const int liveStep = playbackStepByLayer[(size_t) layer];
                    if (juce::isPositiveAndBelow(liveStep, safeLayerLength))
                    {
                        const int liveChoice = applyModifierToStepChoice(layer,
                                                                         readStepChoiceForModeRT(modeIndex,
                                                                                                 layer,
                                                                                                 liveStep));
                        if (liveChoice != 0)
                            outStepByLayer[(size_t) layer] = liveStep;
                        else
                            outStepByLayer[(size_t) layer] =
                                findNextNonSkipStepForLayerForModeRT(modeIndex,
                                                                      layer,
                                                                      playbackCursorByLayer[(size_t) layer],
                                                                      safeLayerLength);
                    }
                    else
                        outStepByLayer[(size_t) layer] =
                            findNextNonSkipStepForLayerForModeRT(modeIndex,
                                                                  layer,
                                                                  playbackCursorByLayer[(size_t) layer],
                                                                  safeLayerLength);

                    outCursorsAfter[(size_t) layer] =
                        juce::jlimit(0, safeLayerLength - 1, playbackCursorByLayer[(size_t) layer]);
                    continue;
                }

                int startStep = juce::jlimit(0, safeLayerLength - 1, cursors[(size_t) layer]);
                if (mode == kLinkModeF && didRateWrap)
                    startStep = 0;
                const int step = findNextNonSkipStepForLayerForModeRT(modeIndex,
                                                                       layer,
                                                                       startStep,
                                                                       safeLayerLength);
                outStepByLayer[(size_t) layer] = step;

                int nextStep = (step + 1) % safeLayerLength;

                outCursorsAfter[(size_t) layer] = nextStep;
            }

            outRateChoiceRaw = rateChoice;
            return true;
        }

        return false;
    };

    std::array<int, kNumLayers> firstTickSteps {};
    std::array<int, kNumLayers> cursorsAfterFirstTick {};
    int firstRateChoiceRaw = 0;

    if (!findNextValidTick(playbackCursorByLayer, firstTickSteps, cursorsAfterFirstTick, firstRateChoiceRaw))
    {
        // Emergency anti-stall path:
        // If a corrupted or stale state makes all scanned rate steps appear as Skip,
        // keep the sequencer timeline alive instead of freezing forever.
        std::array<int, kNumLayers> emergencySteps {};
        std::array<int, kNumLayers> emergencyCursors {};
        emergencySteps.fill(-1);
        emergencyCursors = playbackCursorByLayer;

        const int rateStep = juce::jlimit(0,
                                          safeRateLength - 1,
                                          playbackCursorByLayer[(size_t) kLayerRate]);
        const int nextRateCursor = (rateStep + 1) % safeRateLength;
        const bool didRateWrap = (nextRateCursor == 0);

        emergencySteps[(size_t) kLayerRate] = rateStep;
        emergencyCursors[(size_t) kLayerRate] = nextRateCursor;

        for (int layer = 1; layer < kNumLayers; ++layer)
        {
            const int safeLayerLength = safeLengthByLayer[(size_t) layer];
            const int mode = (int) linkModeByLayer[(size_t) layer];

            if (mode == kLinkModeUnlink)
            {
                const int liveStep = playbackStepByLayer[(size_t) layer];
                if (juce::isPositiveAndBelow(liveStep, safeLayerLength))
                {
                    const int liveChoice = applyModifierToStepChoice(layer,
                                                                     readStepChoiceForModeRT(modeIndex,
                                                                                             layer,
                                                                                             liveStep));
                    if (liveChoice != 0)
                        emergencySteps[(size_t) layer] = liveStep;
                    else
                        emergencySteps[(size_t) layer] =
                            findNextNonSkipStepForLayerForModeRT(modeIndex,
                                                                  layer,
                                                                  playbackCursorByLayer[(size_t) layer],
                                                                  safeLayerLength);
                }
                else
                    emergencySteps[(size_t) layer] =
                        findNextNonSkipStepForLayerForModeRT(modeIndex,
                                                              layer,
                                                              playbackCursorByLayer[(size_t) layer],
                                                              safeLayerLength);

                emergencyCursors[(size_t) layer] =
                    juce::jlimit(0, safeLayerLength - 1, playbackCursorByLayer[(size_t) layer]);
                continue;
            }

            int startStep = juce::jlimit(0, safeLayerLength - 1, playbackCursorByLayer[(size_t) layer]);
            if (mode == kLinkModeF && didRateWrap)
                startStep = 0;
            const int step = findNextNonSkipStepForLayerForModeRT(modeIndex,
                                                                   layer,
                                                                   startStep,
                                                                   safeLayerLength);
            emergencySteps[(size_t) layer] = step;

            int nextStep = (step + 1) % safeLayerLength;

            emergencyCursors[(size_t) layer] = nextStep;
        }

        playbackStepByLayer = emergencySteps;
        playbackCursorByLayer = emergencyCursors;
        currentStepDurationBeats = 0.25; // 1/16 safety step.
        resolveMusicalStateForCurrentTick();
        nextRateStepPpq += currentStepDurationBeats;
        return true;
    }

    int firstRateChoice = firstRateChoiceRaw;
    if (firstRateChoice == kRateRndIndex)
        firstRateChoice = 1 + playbackRandom.nextInt(kRateChoiceCount);

    currentStepDurationBeats = beatsForRateChoice(firstRateChoice);
    playbackStepByLayer = firstTickSteps;
    playbackCursorByLayer = cursorsAfterFirstTick;

    resolveMusicalStateForCurrentTick();
    nextRateStepPpq += currentStepDurationBeats;
    return true;
}

bool ArpeggiatorProcessor::triggerOneUnlinkLayerStep(int layer) noexcept
{
    if (!juce::isPositiveAndBelow(layer, kNumLayers) || layer == kLayerRate)
        return false;

    if (readLinkModeRT(layer) != kLinkModeUnlink)
        return false;

    const int modeIndex = readModeRT();
    const int safeLayerLength =
        juce::jmax(1, effectiveSequenceLengthForLayerForModeRT(layer, modeIndex));
    const int step = findNextNonSkipStepForLayerForModeRT(modeIndex,
                                                           layer,
                                                           playbackCursorByLayer[(size_t) layer],
                                                           safeLayerLength);

    playbackStepByLayer[(size_t) layer] = step;
    playbackCursorByLayer[(size_t) layer] = (step + 1) % safeLayerLength;

    int rateChoice = readUnlinkRateChoiceRT(layer);
    if (rateChoice == kRateRndIndex)
        rateChoice = 1 + playbackRandom.nextInt(kRateChoiceCount);

    const double stepBeats = juce::jmax(1.0e-6, beatsForRateChoice(rateChoice));
    double& nextTickPpq = nextUnlinkStepPpqByLayer[(size_t) layer];
    if (!std::isfinite(nextTickPpq))
        nextTickPpq = nextRateStepPpq;
    nextTickPpq += stepBeats;
    return true;
}

void ArpeggiatorProcessor::resolveMusicalStateForCurrentTick() noexcept
{
    // Interaction matrix Arp vs Drum (single source of truth for this tick):
    // - Shared layers: Rythm/Rate, Gate, Groove, Velocity, Retrig
    // - Arp-only: Direction(=Strum), Pattern(=Jump), Range(=Octave)
    // - Drum-only: Direction(=Hit), Pattern(=Velo Env), Range(=Tim Env)
    //
    // Rule:
    // - Drum-only controls never affect Arp behavior.
    // - Arp-only semantics are ignored in Drum mode.

    // Snapshot mode once per tick:
    // avoids cross-mode inconsistencies when UI changes Arp/Drum mid-block.
    const int modeIndex = readModeRT();
    const auto readChoiceAtCurrentStep = [this, modeIndex](int layer) -> int
    {
        if (!juce::isPositiveAndBelow(layer, kNumLayers))
            return 0;

        const int step = playbackStepByLayer[(size_t) layer];
        if (!juce::isPositiveAndBelow(step, kNumSteps))
            return 0;

        return applyModifierToStepChoice(layer, readStepChoiceForModeRT(modeIndex, layer, step));
    };

    // Cache tick-local choices once.
    // This avoids re-reading APVTS choices in processTickAtPpq() and keeps all
    // layer interactions coherent for the same logical tick.
    currentVelocityChoice = readChoiceAtCurrentStep(kLayerVelocity);
    currentGrooveChoice = readChoiceAtCurrentStep(kLayerGroove);
    currentGateChoice = readChoiceAtCurrentStep(kLayerGate);
    currentIsDrumMode = (modeIndex == 1);

    if (currentIsDrumMode)
    {
        // Drum mode mapping:
        // - Direction layer -> Hit (X/O/Flam/Drag)
        // - Pattern layer   -> Velo Env (0/lin/log/rlin/rlog/rnd)
        // - Range layer     -> Tim Env  (0/lin/log/rlin/rlog/rnd)
        // Les couches partagees (Rate/Gate/Groove/Velocity/Retrig) gardent
        // exactement leur comportement arpeggiator.
        const int hitChoice = readChoiceAtCurrentStep(kLayerDirection);
        // Drum contract:
        // - Trigger choices always play as ALL (single chordal hit policy).
        // - Silent choices (Skip, O, legacy Mute) never trigger any note.
        // - No state carry-over from previous step: each step is resolved
        //   independently for deterministic drum behavior.
        bool drumStepTriggers = true;
        currentGraceCount = 0;
        switch (hitChoice)
        {
            case 0: // Skip
            case kDrumHitChoiceSilence:
            case kDirectionMuteIndex:
                drumStepTriggers = false;
                break;

            case kDrumHitChoiceX:
                currentGraceCount = 0;
                break;

            case kDrumHitChoiceFlam:
                currentGraceCount = 1;
                break;

            case kDrumHitChoiceDrag:
                currentGraceCount = 2;
                break;

            default:
                // Legacy/non-hit values are treated as generic triggers.
                // They still use ALL note selection in drum mode.
                currentGraceCount = 0;
                break;
        }

        int veloEnvChoice = readChoiceAtCurrentStep(kLayerPattern);
        if (veloEnvChoice == kDrumEnvRndIndex)
            veloEnvChoice = kDrumEnvChoiceFirst + playbackRandom.nextInt(kDrumEnvChoiceLast - kDrumEnvChoiceFirst + 1);
        else if (veloEnvChoice > kDrumEnvChoiceLast)
            veloEnvChoice = kDrumEnvChoiceLast;

        if (veloEnvChoice >= kDrumEnvChoiceFirst && veloEnvChoice <= kDrumEnvChoiceLast)
            currentDrumVeloEnvMode = (DrumVeloEnvMode) (veloEnvChoice - kDrumEnvChoiceFirst);

        int timEnvChoice = readChoiceAtCurrentStep(kLayerRange);
        if (timEnvChoice == kDrumEnvRndIndex)
            timEnvChoice = kDrumEnvChoiceFirst + playbackRandom.nextInt(kDrumEnvChoiceLast - kDrumEnvChoiceFirst + 1);
        else if (timEnvChoice > kDrumEnvChoiceLast)
            timEnvChoice = kDrumEnvChoiceLast;

        switch (timEnvChoice)
        {
            case 1: currentPaceMode = PaceMode::Equal;  break; // "-"
            case 2: currentPaceMode = PaceMode::Lin;    break; // lin
            case 3: currentPaceMode = PaceMode::Log;    break; // log
            case 4: currentPaceMode = PaceMode::RevLin; break; // rlin
            case 5: currentPaceMode = PaceMode::RevLog; break; // rlog
            default: break; // Skip garde la valeur precedente.
        }

        // En drum mode, toute etape trigger joue le pool en ALL.
        // Les etapes silencieuses (Skip/O/Mute) sont forcees en Mute.
        currentDirectionMode = (drumStepTriggers ? kDirectionChordAllIndex : kDirectionMuteIndex);
        currentJumpSize = 1;
        currentOctaveOffset = 0;
    }
    else
    {
        // Drum-only states must not leak into Arp behavior.
        currentGraceCount = 0;
        currentDrumVeloEnvMode = DrumVeloEnvMode::Flat;

        int directionChoice = readChoiceAtCurrentStep(kLayerDirection);
        // Direction/Strum:
        // 1=Top, 2=Up, 3="=", 4=Down, 5=Bottom, 6=Chord All,
        // 7=Up+Up, 8=Down+Down, 9=Mute, 10=Rnd(main, sans "="/Mute/Chord),
        // 11..16=Chord Up fractions, 17..22=Chord Down fractions, 23=Chord Rnd.
        if (directionChoice == kDirectionRndIndex)
        {
            static constexpr std::array<int, 6> kMainRndChoices {
                kDirectionTopIndex,
                kDirectionUpIndex,
                kDirectionDownIndex,
                kDirectionBottomIndex,
                kDirectionUpPairIndex,
                kDirectionDownPairIndex
            };
            directionChoice = kMainRndChoices[(size_t) playbackRandom.nextInt((int) kMainRndChoices.size())];
        }
        else if (directionChoice == kDirectionChordRndIndex)
        {
            static constexpr std::array<int, 13> kChordRndChoices {
                kDirectionChordAllIndex,
                11, 12, 13, 14, 15, 16,
                17, 18, 19, 20, 21, 22
            };
            directionChoice = kChordRndChoices[(size_t) playbackRandom.nextInt((int) kChordRndChoices.size())];
        }

        if (directionChoice >= kDirectionTopIndex && directionChoice <= kDirectionChordRndIndex)
            currentDirectionMode = directionChoice;

        int patternChoice = readChoiceAtCurrentStep(kLayerPattern);
        // Jump layer (pattern):
        // - Skip garde currentJumpSize (continuite musicale stateful).
        // - Rnd choisit 1..8 a chaque tick.
        // - 1..8 fixe directement l ecart de saut.
        if (patternChoice == kPatternRndIndex)
            patternChoice = 1 + playbackRandom.nextInt(8);
        if (patternChoice >= 1 && patternChoice <= 8)
            currentJumpSize = patternChoice;

        int octaveChoice = readChoiceAtCurrentStep(kLayerRange);
        // Octave layer (range):
        // - Skip garde currentOctaveOffset (continuite musicale stateful).
        // - Rnd choisit une octave dans [-4..+4] a chaque tick.
        // - choix fixes 1..9 mappent sur [-4..+4] avec 5 comme centre (0 octave).
        if (octaveChoice == kRangeRndIndex)
            currentOctaveOffset = playbackRandom.nextInt(9) - 4;
        else if (octaveChoice >= kRangeChoiceFirst && octaveChoice <= kRangeChoiceLast)
            currentOctaveOffset = octaveChoice - 5;

        // Drum-only contract:
        // Hit / Velo Env / Tim Env must have zero impact in Arp mode.
        // Therefore, Arp retrigs always use equal spacing.
        currentPaceMode = PaceMode::Equal;
    }

    int accentChoice = readChoiceAtCurrentStep(kLayerAccent);
    // Retrig layer (accent):
    // - Skip garde currentRetrigCount (continuite musicale stateful).
    // - Rnd choisit x1..x8 a chaque tick.
    // - x1..x8 fixe directement le nombre de trigs.
    if (accentChoice == kAccentRndIndex)
        currentRetrigCount = kAccentChoiceFirst + playbackRandom.nextInt(kAccentChoiceLast);
    else if (accentChoice >= kAccentChoiceFirst && accentChoice <= kAccentChoiceLast)
        currentRetrigCount = accentChoice;
}

void ArpeggiatorProcessor::flushAllGeneratedNotes(juce::MidiBuffer& out, int samplePos) noexcept
{
    // Fast-path: no active generated note and no pending off.
    if (generatedActiveCount <= 0 && scheduledOffActiveCount <= 0)
        return;

    for (int ch0 = 0; ch0 < kNumMidiChannels; ++ch0)
    {
        for (int nt = 0; nt < kNumNotes; ++nt)
        {
            if (generatedActive[(size_t) ch0][(size_t) nt] != 0)
                out.addEvent(juce::MidiMessage::noteOff(ch0 + 1, nt), samplePos);

            generatedActive[(size_t) ch0][(size_t) nt] = 0;
            generatedVelocity[(size_t) ch0][(size_t) nt] = 0;
            scheduledOffActive[(size_t) ch0][(size_t) nt] = 0;
            scheduledOffPpq[(size_t) ch0][(size_t) nt] = 0.0;
        }
    }

    generatedActiveCount = 0;
    scheduledOffActiveCount = 0;
}

void ArpeggiatorProcessor::emitBypassHeldSnapshot(juce::MidiBuffer& out, int samplePos) noexcept
{
    for (int ch0 = 0; ch0 < kNumMidiChannels; ++ch0)
    {
        for (int nt = 0; nt < kNumNotes; ++nt)
        {
            if (heldCount[(size_t) ch0][(size_t) nt] == 0)
                continue;

            const int vel = (int) (heldVelocity[(size_t) ch0][(size_t) nt] > 0
                                    ? heldVelocity[(size_t) ch0][(size_t) nt]
                                    : (uint8_t) 100);

            out.addEvent(juce::MidiMessage::noteOn(ch0 + 1,
                                                   nt,
                                                   (juce::uint8) juce::jlimit(1, 127, vel)),
                         samplePos);
        }
    }
}

void ArpeggiatorProcessor::emitBypassHeldNoteOffSnapshot(juce::MidiBuffer& out, int samplePos) noexcept
{
    for (int ch0 = 0; ch0 < kNumMidiChannels; ++ch0)
    {
        for (int nt = 0; nt < kNumNotes; ++nt)
        {
            if (heldCount[(size_t) ch0][(size_t) nt] == 0)
                continue;

            out.addEvent(juce::MidiMessage::noteOff(ch0 + 1, nt), samplePos);
        }
    }
}

int ArpeggiatorProcessor::ppqToSampleClamped(const ProcessContext& ctx, double ppq) const noexcept
{
    if (ctx.numSamples <= 0)
        return 0;

    const double safeRate = juce::jmax(1.0, ctx.sampleRate);
    const double safeBpm = juce::jmax(1.0, ctx.bpm);
    const double beatsPerSample = juce::jmax(1.0e-12, (safeBpm / 60.0) / safeRate);
    const double deltaSamples = (ppq - ctx.ppqAtBlockStart) / beatsPerSample;
    const int s = (int) std::floor(deltaSamples + 1.0e-9);
    return juce::jlimit(0, juce::jmax(0, ctx.numSamples - 1), s);
}

double ArpeggiatorProcessor::sampleToPpq(const ProcessContext& ctx, int samplePos) const noexcept
{
    const double safeRate = juce::jmax(1.0, ctx.sampleRate);
    const double safeBpm = juce::jmax(1.0, ctx.bpm);
    const double beatsPerSample = juce::jmax(1.0e-12, (safeBpm / 60.0) / safeRate);
    return ctx.ppqAtBlockStart + (double) juce::jmax(0, samplePos) * beatsPerSample;
}

void ArpeggiatorProcessor::emitDueScheduledOffs(juce::MidiBuffer& out,
                                                const ProcessContext& ctx,
                                                int sampleEndExclusive) noexcept
{
    // Hot-path guard:
    // - beaucoup de segments n ont aucun off en attente.
    // - evite un scan complet 16x128 quand la table est vide.
    if (sampleEndExclusive <= 0 || scheduledOffActiveCount <= 0)
        return;

    const double endPpq = sampleToPpq(ctx, sampleEndExclusive);

    for (int ch0 = 0; ch0 < kNumMidiChannels; ++ch0)
    {
        for (int nt = 0; nt < kNumNotes; ++nt)
        {
            if (scheduledOffActive[(size_t) ch0][(size_t) nt] == 0)
                continue;

            const double offPpq = scheduledOffPpq[(size_t) ch0][(size_t) nt];

            // N emettre que les offs situes dans ce segment.
            // Evite de compresser des offs futurs en fin de bloc courant.
            if (offPpq >= endPpq - 1.0e-9)
                continue;

            const int offSample = ppqToSampleClamped(ctx, offPpq);
            if (offSample >= sampleEndExclusive)
                continue;

            if (generatedActive[(size_t) ch0][(size_t) nt] != 0)
            {
                out.addEvent(juce::MidiMessage::noteOff(ch0 + 1, nt), offSample);
                if (generatedActiveCount > 0)
                    --generatedActiveCount;
            }

            generatedActive[(size_t) ch0][(size_t) nt] = 0;
            generatedVelocity[(size_t) ch0][(size_t) nt] = 0;
            if (scheduledOffActive[(size_t) ch0][(size_t) nt] != 0)
            {
                scheduledOffActive[(size_t) ch0][(size_t) nt] = 0;
                scheduledOffPpq[(size_t) ch0][(size_t) nt] = 0.0;
                if (scheduledOffActiveCount > 0)
                    --scheduledOffActiveCount;
            }
        }
    }
}

int ArpeggiatorProcessor::gatherHeldKeysSorted(int* outKeys, int maxKeys) const noexcept
{
    if (outKeys == nullptr || maxKeys <= 0)
        return 0;

    if (heldActiveNoteCount <= 0 || heldKeysSortedCount <= 0)
        return 0;

    const int available = juce::jmin(maxKeys, juce::jmin(heldKeysSortedCount, heldActiveNoteCount));
    for (int i = 0; i < available; ++i)
        outKeys[(size_t) i] = heldKeysSorted[(size_t) i];

    return available;
}

int ArpeggiatorProcessor::resolveInitialArpWalkIndexForSequence(int heldCountIn) noexcept
{
    if (heldCountIn <= 0)
        return -1;

    const int modeIndex = readModeRT();
    const int safeDirLength =
        juce::jmax(1, effectiveSequenceLengthForLayerForModeRT(kLayerDirection, modeIndex));

    for (int step = 0; step < safeDirLength; ++step)
    {
        int choice = readStepChoiceForModeRT(modeIndex, kLayerDirection, step);
        choice = applyModifierToStepChoice(kLayerDirection, choice);

        if (choice == 0) // Skip
            continue;

        if (choice == kDirectionRndIndex)
            return juce::jlimit(0, heldCountIn - 1, playbackRandom.nextInt(heldCountIn));

        if (choice == kDirectionUpIndex
            || choice == kDirectionUpPairIndex
            || choice == kDirectionTopIndex)
            return heldCountIn - 1;

        if (choice == kDirectionDownIndex
            || choice == kDirectionDownPairIndex
            || choice == kDirectionBottomIndex)
            return 0;

        // "=" / chord modes / mute are not significant for the start seed.
        const bool isChordMode =
            (choice == kDirectionChordAllIndex)
            || (choice >= 11 && choice <= kDirectionChordSpreadLastIndex)
            || (choice == kDirectionChordRndIndex);
        if (choice == kDirectionEqualIndex || choice == kDirectionMuteIndex || isChordMode)
            continue;
    }

    // No significant step in the sequence: default to highest.
    return heldCountIn - 1;
}

uint64_t ArpeggiatorProcessor::computeHeldNoteFingerprint() const noexcept
{
    // Fingerprint RT:
    // - stable pour un meme set de notes tenues (ordre note asc, channel asc).
    // - suffisamment leger pour etre calcule une fois par bloc/lane.
    // - utilise pour eviter de copier une queue differree entre lanes dont les
    //   notes source differrent.
    constexpr uint64_t kFnvOffset = 1469598103934665603ull;
    constexpr uint64_t kFnvPrime = 1099511628211ull;

    uint64_t hash = kFnvOffset;
    auto mix = [&](uint64_t value) noexcept
    {
        hash ^= value;
        hash *= kFnvPrime;
    };

    const int available = juce::jmax(0, juce::jmin(heldKeysSortedCount, heldActiveNoteCount));
    mix((uint64_t) available);

    for (int i = 0; i < available; ++i)
    {
        const int key = heldKeysSorted[(size_t) i];
        const int ch0 = juce::jlimit(0, kNumMidiChannels - 1, key / kNumNotes);
        const int nt = juce::jlimit(0, kNumNotes - 1, key % kNumNotes);
        const int count = juce::jlimit(0, 255, (int) heldCount[(size_t) ch0][(size_t) nt]);
        const int vel = juce::jlimit(0, 127, (int) heldVelocity[(size_t) ch0][(size_t) nt]);

        // pack = [ch0:8][note:8][count:8][vel:8]
        const uint64_t packed =
            (((uint64_t) ch0 & 0xffull) << 24)
            | (((uint64_t) nt & 0xffull) << 16)
            | (((uint64_t) count & 0xffull) << 8)
            | ((uint64_t) vel & 0xffull);
        mix(packed);
    }

    return hash;
}

int ArpeggiatorProcessor::chooseArpKeyIndex(int heldCountIn) noexcept
{
    if (heldCountIn <= 0)
    {
        arpWalkIndex = -1;
        return -1;
    }

    const int jump = juce::jlimit(1, 8, currentJumpSize);

    // First note after reset: seed from first significant Direction step.
    if (arpWalkIndex < 0)
        arpWalkIndex = resolveInitialArpWalkIndexForSequence(heldCountIn);

    // If held pool shrinks and current index becomes out-of-range,
    // restart from deterministic sequence-derived seed.
    if (arpWalkIndex >= heldCountIn)
        arpWalkIndex = resolveInitialArpWalkIndexForSequence(heldCountIn);

    if (!juce::isPositiveAndBelow(arpWalkIndex, heldCountIn))
        return -1;

    // Jump semantics:
    // - modulo sur la taille de pool pour garder un wrap deterministic.
    // - si jump est multiple exact de heldCount, step=0 et la lecture reste sur
    //   le meme rang, ce qui est intentionnel.
    const int step = (heldCountIn > 0 ? (jump % heldCountIn) : 0);

    switch (currentDirectionMode)
    {
        case kDirectionDownIndex: // Down
            arpWalkIndex = (arpWalkIndex - step + heldCountIn) % heldCountIn;
            break;
        case kDirectionBottomIndex: // Bottom
            arpWalkIndex = 0;
            break;
        case kDirectionTopIndex: // Top
            arpWalkIndex = heldCountIn - 1;
            break;
        case kDirectionEqualIndex: // "="
            // keep previous note index as-is
            break;
        case kDirectionUpIndex: // Up
        default:
            arpWalkIndex = (arpWalkIndex + step) % heldCountIn;
            break;
    }

    return juce::jlimit(0, heldCountIn - 1, arpWalkIndex);
}

double ArpeggiatorProcessor::paceWeightForSlot(int slotIndex,
                                               int slotCount,
                                               PaceMode mode) const noexcept
{
    const int safeIndex = juce::jlimit(0, juce::jmax(0, slotCount - 1), slotIndex);
    const int safeCount = juce::jmax(1, slotCount);

    switch (mode)
    {
        case PaceMode::Lin:
            return (double) (safeCount - safeIndex);

        case PaceMode::Log:
            return std::log((double) (safeCount - safeIndex) + 1.0);

        case PaceMode::RevLin:
            return (double) (safeIndex + 1);

        case PaceMode::RevLog:
            return std::log((double) (safeIndex + 2));

        case PaceMode::Equal:
        default:
            return 1.0;
    }
}

int ArpeggiatorProcessor::buildPacedOffsets(int repeatCount,
                                            double stepDurationBeats,
                                            bool includeEndPoint,
                                            PaceMode mode,
                                            double* outOffsets,
                                            int maxOffsets) const noexcept
{
    if (outOffsets == nullptr || maxOffsets <= 0 || repeatCount <= 0)
        return 0;

    const int count = juce::jlimit(1, maxOffsets, repeatCount);
    const double safeDuration = juce::jmax(0.0, stepDurationBeats);

    outOffsets[0] = 0.0;
    for (int i = 1; i < count; ++i)
        outOffsets[i] = 0.0;

    if (count <= 1 || safeDuration <= 1.0e-12)
        return count;

    // RT optimization:
    // Precomputed normalized offsets for retrig counts [1..8] and tim-env modes.
    // This avoids per-tick weight/log computations in the hot path.
    if (!includeEndPoint && count <= 8)
    {
        static const auto kNormalizedOffsets = []()
        {
            std::array<std::array<std::array<double, 8>, 8>, 5> table {};

            const auto weightFor = [](int modeIndex, int slotIndex, int slotCount) noexcept
            {
                const int safeIndex = juce::jlimit(0, juce::jmax(0, slotCount - 1), slotIndex);
                const int safeCount = juce::jmax(1, slotCount);

                switch (modeIndex)
                {
                    case 1: // Lin: intervals shrink over time (faster arrivals)
                        return (double) (safeCount - safeIndex);
                    case 2: // Log: stronger "faster arrivals" curvature
                        return std::log((double) (safeCount - safeIndex) + 1.0);
                    case 3: // RevLin: intervals expand over time (slower arrivals)
                        return (double) (safeIndex + 1);
                    case 4: // RevLog: stronger "slower arrivals" curvature
                        return std::log((double) (safeIndex + 2));
                    case 0: // Equal
                    default:
                        return 1.0;
                }
            };

            for (int modeIndex = 0; modeIndex < 5; ++modeIndex)
            {
                for (int retrigCount = 1; retrigCount <= 8; ++retrigCount)
                {
                    auto& offsets = table[(size_t) modeIndex][(size_t) (retrigCount - 1)];
                    offsets.fill(0.0);

                    if (retrigCount <= 1)
                        continue;

                    const int slotCount = retrigCount;
                    double sum = 0.0;
                    std::array<double, 8> weights {};

                    for (int slot = 0; slot < slotCount; ++slot)
                    {
                        const double w = juce::jmax(1.0e-6, weightFor(modeIndex, slot, slotCount));
                        weights[(size_t) slot] = w;
                        sum += w;
                    }

                    if (sum <= 1.0e-12)
                        sum = (double) slotCount;

                    double acc = 0.0;
                    for (int r = 1; r < retrigCount; ++r)
                    {
                        acc += weights[(size_t) (r - 1)] / sum;
                        offsets[(size_t) r] = acc;
                    }
                }
            }

            return table;
        }();

        int modeIndex = 0;
        switch (mode)
        {
            case PaceMode::Lin:    modeIndex = 1; break;
            case PaceMode::Log:    modeIndex = 2; break;
            case PaceMode::RevLin: modeIndex = 3; break;
            case PaceMode::RevLog: modeIndex = 4; break;
            case PaceMode::Equal:
            default:               modeIndex = 0; break;
        }

        const auto& normalized = kNormalizedOffsets[(size_t) modeIndex][(size_t) (count - 1)];
        outOffsets[0] = 0.0;
        for (int i = 1; i < count; ++i)
            outOffsets[i] = normalized[(size_t) i] * safeDuration;

        return count;
    }

    const int slotCount = includeEndPoint ? (count - 1) : count;
    if (slotCount <= 0)
        return count;

    std::array<double, 16> slotWeights {};
    const int usableSlots = juce::jmin(slotCount, (int) slotWeights.size());

    double weightSum = 0.0;
    for (int slot = 0; slot < usableSlots; ++slot)
    {
        const double w = juce::jmax(1.0e-6, paceWeightForSlot(slot, usableSlots, mode));
        slotWeights[(size_t) slot] = w;
        weightSum += w;
    }

    if (weightSum <= 1.0e-12)
    {
        for (int slot = 0; slot < usableSlots; ++slot)
            slotWeights[(size_t) slot] = 1.0;
        weightSum = (double) usableSlots;
    }

    const double scale = safeDuration / weightSum;

    double acc = 0.0;
    for (int repeat = 1; repeat < count; ++repeat)
    {
        const int slot = juce::jlimit(0, usableSlots - 1, repeat - 1);
        acc += slotWeights[(size_t) slot] * scale;
        outOffsets[repeat] = acc;
    }

    return count;
}

int ArpeggiatorProcessor::applyDrumVeloEnvelopeToVelocity(int baseVelocity,
                                                          int retrigIndex,
                                                          int retrigCount,
                                                          DrumVeloEnvMode mode) const noexcept
{
    const int base = juce::jlimit(1, 127, baseVelocity);
    if (retrigCount <= 1 || mode == DrumVeloEnvMode::Flat)
        return base;

    const double t = juce::jlimit(0.0, 1.0, (double) retrigIndex / (double) juce::jmax(1, retrigCount - 1));
    // Modifier convention:
    // - mode Lin   : -50% -> +50% (linear)
    // - mode RevLin: +50% -> -50% (linear)
    // - mode Log   : -50% -> +50% (log curve)
    // - mode RevLog: +50% -> -50% (log curve)
    // - mode Flat  : 0% (no change)
    double modifier = 0.0;

    switch (mode)
    {
        case DrumVeloEnvMode::Lin:
            modifier = -0.5 + t; // -50% .. +50%
            break;

        case DrumVeloEnvMode::RevLin:
            modifier = 0.5 - t; // +50% .. -50%
            break;

        case DrumVeloEnvMode::Log:
        {
            // Normalized logarithmic ramp in [0..1].
            static constexpr double kLogBase = 9.0;
            const double u = std::log1p(kLogBase * t) / std::log1p(kLogBase);
            modifier = -0.5 + u; // -50% .. +50%
            break;
        }

        case DrumVeloEnvMode::RevLog:
        {
            static constexpr double kLogBase = 9.0;
            const double u = std::log1p(kLogBase * t) / std::log1p(kLogBase);
            modifier = 0.5 - u; // +50% .. -50%
            break;
        }

        case DrumVeloEnvMode::Flat:
        default:
            modifier = 0.0;
            break;
    }

    const double gain = juce::jlimit(0.0, 2.0, 1.0 + modifier);
    return juce::jlimit(1, 127, juce::roundToInt((float) ((double) base * gain)));
}

bool ArpeggiatorProcessor::resolveGrooveOffsetBeats(int grooveChoice,
                                                    double stepDurationBeats,
                                                    double& offsetBeats) noexcept
{
    offsetBeats = 0.0;

    if (grooveChoice == 0)
        return true;

    int groovePercent = 0;
    if (isGrooveFixedChoice(grooveChoice))
        groovePercent = groovePercentFromChoice(grooveChoice);

    offsetBeats = stepDurationBeats * ((double) groovePercent / 100.0);
    return true;
}

bool ArpeggiatorProcessor::resolveGateForStep(int gateChoice,
                                              double stepDurationBeats,
                                              GateMode& gateMode,
                                              double& gateDurationBeats) noexcept
{
    // Contrat Gate:
    // - 0     : Skip -> aucun trig sur ce pas.
    // - 1..8  : Timed (5/10/25/33/50/66/75/99%).
    // - 9     : Tie -> note off juste apres le prochain note on.
    //
    // Objectif musical:
    // garder une relation stable entre Gate et Rate, meme en edition a la volee.
    gateMode = GateMode::Skip;
    gateDurationBeats = 0.0;

    if (gateChoice == 0)
        return false;

    if (gateChoice == kGateTieChoice)
    {
        gateMode = GateMode::Legato;
        gateDurationBeats = juce::jmax(0.0, stepDurationBeats);
        return true;
    }

    if (!isGateTimedChoice(gateChoice))
        return false;

    gateMode = GateMode::Timed;
    gateDurationBeats = juce::jmax(0.0, stepDurationBeats)
        * ((double) gateTimedPercentForChoice(gateChoice) / 100.0);
    return gateDurationBeats > 0.0;
}

void ArpeggiatorProcessor::scheduleGeneratedOff(int channel1, int note, double offPpq) noexcept
{
    const int ch0 = juce::jlimit(1, kNumMidiChannels, channel1) - 1;
    const int nt = juce::jlimit(0, kNumNotes - 1, note);

    if (scheduledOffActive[(size_t) ch0][(size_t) nt] == 0)
    {
        scheduledOffActive[(size_t) ch0][(size_t) nt] = 1;
        scheduledOffPpq[(size_t) ch0][(size_t) nt] = offPpq;
        ++scheduledOffActiveCount;
        return;
    }

    scheduledOffPpq[(size_t) ch0][(size_t) nt] =
        juce::jmax(scheduledOffPpq[(size_t) ch0][(size_t) nt], offPpq);
}

bool ArpeggiatorProcessor::deferredEventComesBefore(const DeferredEvent& a,
                                                    const DeferredEvent& b) noexcept
{
    if (a.ppq != b.ppq)
        return a.ppq < b.ppq;
    return a.sequence < b.sequence;
}

bool ArpeggiatorProcessor::deferredEventHeapCompare(const DeferredEvent& a,
                                                    const DeferredEvent& b) noexcept
{
    // Comparateur min-heap:
    // l evenement le plus tot doit rester en racine.
    return deferredEventComesBefore(b, a);
}

void ArpeggiatorProcessor::rebuildDeferredEventHeap() noexcept
{
    std::make_heap(deferredEvents.begin(),
                   deferredEvents.begin() + deferredEventCount,
                   deferredEventHeapCompare);
}

void ArpeggiatorProcessor::clearDeferredEvents() noexcept
{
    deferredEventCount = 0;
    deferredWorstValid = false;
}

uint16_t ArpeggiatorProcessor::beginSelectionStampGeneration() noexcept
{
    ++selectedSeenGeneration;
    if (selectedSeenGeneration == 0)
    {
        // Rare wrap: reset complet des stamps puis repartir a 1.
        for (auto& byNote : selectedSeenStamp)
            byNote.fill(0);
        selectedSeenGeneration = 1;
    }

    return selectedSeenGeneration;
}

void ArpeggiatorProcessor::enqueueDeferredEvent(const DeferredEvent& eventIn) noexcept
{
    if (!std::isfinite(eventIn.ppq))
    {
        ++deferredTelemetry.droppedInvalidPpq;
        return;
    }

    DeferredEvent event = eventIn;
    event.sequence = deferredEventSequenceCounter++;
    if (deferredEventSequenceCounter == 0)
        deferredEventSequenceCounter = 1;

    if (deferredEventCount < kMaxDeferredEvents)
    {
        deferredEvents[(size_t) deferredEventCount] = event;
        ++deferredEventCount;
        std::push_heap(deferredEvents.begin(),
                       deferredEvents.begin() + deferredEventCount,
                       deferredEventHeapCompare);

        if (!deferredWorstValid || deferredEventComesBefore(deferredWorstEvent, event))
        {
            deferredWorstEvent = event;
            deferredWorstValid = true;
        }

        const auto depth = (uint32_t) deferredEventCount;
        if (depth > deferredTelemetry.peakDepth)
            deferredTelemetry.peakDepth = depth;
        return;
    }

    deferredTelemetry.overflowed = true;

    // Strategie saturation:
    // conserver les evenements les plus tot, jeter les plus tardifs.
    if (deferredWorstValid && !deferredEventComesBefore(event, deferredWorstEvent))
    {
        ++deferredTelemetry.droppedOverflowLate;
        return;
    }

    int replacementIndex = -1;
    int worstOverall = -1;

    // Optimisation du chemin de saturation:
    // une seule passe lineaire collecte:
    // - l evenement le plus tardif global (fallback)
    // - le Mute le plus tardif (cible preferee quand on tente de garder
    //   les NoteOn).
    int worstMute = -1;
    for (int i = 0; i < deferredEventCount; ++i)
    {
        if (worstOverall < 0 || deferredEventComesBefore(deferredEvents[(size_t) worstOverall], deferredEvents[(size_t) i]))
            worstOverall = i;

        const auto type = deferredEvents[(size_t) i].type;
        if (type == DeferredEventType::Mute)
        {
            if (worstMute < 0 || deferredEventComesBefore(deferredEvents[(size_t) worstMute], deferredEvents[(size_t) i]))
                worstMute = i;
        }
    }

    // Priorite musicale en saturation:
    // preferer garder NOTE ON et remplacer un evenement de controle tardif.
    if (event.type == DeferredEventType::NoteOn)
        replacementIndex = worstMute;

    if (replacementIndex >= 0 && !deferredEventComesBefore(event, deferredEvents[(size_t) replacementIndex]))
        replacementIndex = -1;

    if (replacementIndex < 0)
    {
        replacementIndex = worstOverall;
        deferredWorstValid = false;
    }

    if (replacementIndex < 0)
    {
        ++deferredTelemetry.droppedOverflowLate;
        return;
    }

    if (!deferredEventComesBefore(event, deferredEvents[(size_t) replacementIndex]))
    {
        ++deferredTelemetry.droppedOverflowLate;
        return;
    }

    if (event.type == DeferredEventType::NoteOn
        && deferredEvents[(size_t) replacementIndex].type != DeferredEventType::NoteOn)
    {
        ++deferredTelemetry.prioritizedNoteOnReplacements;
    }

    deferredEvents[(size_t) replacementIndex] = event;
    rebuildDeferredEventHeap();
    deferredWorstValid = false;
    ++deferredTelemetry.replacedOverflow;
}

void ArpeggiatorProcessor::enqueueDeferredNoteOn(double onsetPpq,
                                                 int channel1,
                                                 int note,
                                                 int velocity,
                                                 GateMode gateMode,
                                                 double offPpq) noexcept
{
    DeferredEvent ev;
    ev.type = DeferredEventType::NoteOn;
    ev.ppq = onsetPpq;
    ev.offOrExtendPpq = offPpq;
    ev.channel1 = juce::jlimit(1, kNumMidiChannels, channel1);
    ev.note = juce::jlimit(0, kNumNotes - 1, note);
    ev.velocity = juce::jlimit(1, 127, velocity);
    ev.gateMode = gateMode;
    enqueueDeferredEvent(ev);
}

void ArpeggiatorProcessor::enqueueDeferredMute(double atPpq) noexcept
{
    DeferredEvent ev;
    ev.type = DeferredEventType::Mute;
    ev.ppq = atPpq;
    enqueueDeferredEvent(ev);
}

void ArpeggiatorProcessor::emitGeneratedNoteOn(juce::MidiBuffer& out,
                                               int samplePos,
                                               int channel1,
                                               int note,
                                               int velocity,
                                               GateMode gateMode,
                                               double offPpq) noexcept
{
    const int ch = juce::jlimit(1, kNumMidiChannels, channel1);
    const int ch0 = ch - 1;
    const int nt = juce::jlimit(0, kNumNotes - 1, note);
    const int vel = juce::jlimit(1, 127, velocity);

    if (generatedActive[(size_t) ch0][(size_t) nt] != 0)
    {
        // Contrat anti-stuck:
        // toujours fermer la note precedente du meme (ch,note)
        // avant d emettre le nouveau Note On.
        out.addEvent(juce::MidiMessage::noteOff(ch, nt), samplePos);
        generatedActive[(size_t) ch0][(size_t) nt] = 0;
        if (generatedActiveCount > 0)
            --generatedActiveCount;

        if (scheduledOffActive[(size_t) ch0][(size_t) nt] != 0)
        {
            scheduledOffActive[(size_t) ch0][(size_t) nt] = 0;
            scheduledOffPpq[(size_t) ch0][(size_t) nt] = 0.0;
            if (scheduledOffActiveCount > 0)
                --scheduledOffActiveCount;
        }
    }

    out.addEvent(juce::MidiMessage::noteOn(ch, nt, (juce::uint8) vel), samplePos);
    if (generatedActive[(size_t) ch0][(size_t) nt] == 0)
    {
        generatedActive[(size_t) ch0][(size_t) nt] = 1;
        ++generatedActiveCount;
    }
    generatedVelocity[(size_t) ch0][(size_t) nt] = (uint8_t) vel;

    if (gateMode != GateMode::Skip)
        scheduleGeneratedOff(ch, nt, offPpq);
}

void ArpeggiatorProcessor::processTickAtPpq(juce::MidiBuffer& out,
                                            const ProcessContext& ctx,
                                            double tickPpq,
                                            int tickSample) noexcept
{
    juce::ignoreUnused(tickSample);

    if (playbackStepByLayer[(size_t) kLayerRate] < 0)
        return;

    const int velChoice = currentVelocityChoice;
    const int gateChoice = currentGateChoice;
    const int grooveChoice = currentGrooveChoice;

    double grooveOffsetBeats = 0.0;
    resolveGrooveOffsetBeats(grooveChoice, currentStepDurationBeats, grooveOffsetBeats);

    const double blockStartPpq = ctx.ppqAtBlockStart;
    const double blockEndPpq = sampleToPpq(ctx, ctx.numSamples);
    const double onsetBasePpq = juce::jmax(blockStartPpq, tickPpq + grooveOffsetBeats);

    if (currentDirectionMode == kDirectionMuteIndex)
    {
        if (onsetBasePpq >= blockEndPpq - 1.0e-9)
        {
            enqueueDeferredMute(onsetBasePpq);
            return;
        }

        flushAllGeneratedNotes(out, ppqToSampleClamped(ctx, onsetBasePpq));
        return;
    }

    GateMode gateMode = GateMode::Skip;
    double gateDurationBeats = 0.0;
    if (!resolveGateForStep(gateChoice, currentStepDurationBeats, gateMode, gateDurationBeats))
        return;

    const int heldCountNow = gatherHeldKeysSorted(keyScratch.data(), kMaxHeldKeys);
    if (heldCountNow <= 0)
        return;

    const int poolCount = heldCountNow;

    // Velocity step policy for this tick:
    // 0     -> keep source note velocity (Skip)
    // 1..8  -> fixed symbolic modifier (ppp..fff, with "=" and "mf" at 0%)
    //
    // The step policy is frozen once per tick for deterministic sequencing.
    const bool velocityPercentDefined = isVelocityFixedChoice(velChoice);
    const int velocityPercentForTick = (velocityPercentDefined
        ? velocityPercentFromChoiceIndex(velChoice)
        : 0);

    // Drum mode remplace Octave par Tim Env: pas de transposition.
    const int octaveSemitoneOffset = (currentIsDrumMode
        ? 0
        : juce::jlimit(-4, 4, currentOctaveOffset) * 12);

    bool chordSpreadDownDirection = false;
    double chordSpreadFraction = 0.0;
    const bool chordSpreadMode = (!currentIsDrumMode
                                  && decodeChordSpreadMode(currentDirectionMode,
                                                           chordSpreadDownDirection,
                                                           chordSpreadFraction));
    const bool chordStepUsesSingleTrigger = (!currentIsDrumMode
                                             && (currentDirectionMode == kDirectionChordAllIndex
                                                 || chordSpreadMode));

    // Interaction critique:
    // - Un pas chord doit declencher un seul "groupe chord" par tick.
    // - Le parametre retrig est donc ignore pour ce pas.
    // - Effet: pas de multiplications On/Off inutiles, moins de risque de doublons
    //   et moins de pression sur la file differree en edition live.
    const int requestedRetrigs = (chordStepUsesSingleTrigger
                                      ? 1
                                      : juce::jlimit(kAccentChoiceFirst, kAccentChoiceLast, currentRetrigCount));
    // Drum Hit is independent from Retrig:
    // - X    : no pre-hit
    // - O    : silence (handled upstream via Direction=Mute)
    // - Flam : one pre-hit at -1/32 step, -50% velocity
    // - Drag : two pre-hits at -2/32 and -1/32 step, -50% velocity
    const int graceCount = (currentIsDrumMode ? juce::jlimit(0, 2, currentGraceCount) : 0);
    const int effectiveRetrigs = juce::jlimit(1, 8, requestedRetrigs);
    const double gateRatio = (currentStepDurationBeats > 1.0e-12
        ? juce::jmax(0.0, gateDurationBeats / currentStepDurationBeats)
        : 0.0);
    const double legatoOffNudge = [&]() noexcept
    {
        if (gateMode != GateMode::Legato)
            return 0.0;

        const double safeRate = juce::jmax(1.0, ctx.sampleRate);
        const double safeBpm = juce::jmax(1.0, ctx.bpm);
        return juce::jmax(1.0e-12, (safeBpm / 60.0) / safeRate);
    }();

    std::array<double, 8> retrigOffsets {};
    int pacedRetrigs = 1;
    retrigOffsets[0] = 0.0;

    // Fast-path x1:
    // pas de table de repartition a calculer, ce qui reduit legerement le cout
    // CPU dans le cas le plus frequent.
    if (effectiveRetrigs > 1)
    {
        pacedRetrigs = buildPacedOffsets(effectiveRetrigs,
                                         currentStepDurationBeats,
                                         false,
                                         currentPaceMode,
                                         retrigOffsets.data(),
                                         (int) retrigOffsets.size());
    }

    // Construire le set de notes de ce tick selon le mode direction courant:
    // - Arp  : strum/navigation dans le pool de notes.
    // - Drum : hit applique sur un jeu chordal.
    // Dedup (channel,note) pour eviter des doublons sur pair/chord.
    //
    // Contrainte RT:
    // - on reutilise des scratch buffers membres (pas de gros tableaux locaux
    //   reinitialises a chaque tick).
    // - selectedSeenStamp + generation evitent un memset 16x128 par tick.
    auto& selectedSrcKeys = selectedSrcKeysScratch;
    auto& selectedOutNotes = selectedOutNotesScratch;
    auto& selectedSourceVelocities = selectedSourceVelocitiesScratch;
    const uint16_t selectionGeneration = beginSelectionStampGeneration();
    int selectedCount = 0;

    auto appendSelectedByIndex = [&](int poolIndex)
    {
        if (!juce::isPositiveAndBelow(poolIndex, poolCount))
            return;

        const int srcKeyLocal = keyScratch[(size_t) poolIndex];
        const int srcCh0Local = juce::jlimit(0, kNumMidiChannels - 1, srcKeyLocal / kNumNotes);
        const int srcNoteLocal = juce::jlimit(0, kNumNotes - 1, srcKeyLocal % kNumNotes);
        const int outNoteLocal = srcNoteLocal + octaveSemitoneOffset;
        if (!juce::isPositiveAndBelow(outNoteLocal, kNumNotes))
            return;

        if (selectedSeenStamp[(size_t) srcCh0Local][(size_t) outNoteLocal] == selectionGeneration)
            return;

        if (selectedCount >= kMaxHeldKeys)
            return;

        selectedSeenStamp[(size_t) srcCh0Local][(size_t) outNoteLocal] = selectionGeneration;
        selectedSrcKeys[(size_t) selectedCount] = srcKeyLocal;
        selectedOutNotes[(size_t) selectedCount] = outNoteLocal;

        selectedSourceVelocities[(size_t) selectedCount] =
            (int) (heldVelocity[(size_t) srcCh0Local][(size_t) srcNoteLocal] > 0
                       ? heldVelocity[(size_t) srcCh0Local][(size_t) srcNoteLocal]
                       : (uint8_t) 100);

        ++selectedCount;
    };

    if (currentIsDrumMode || currentDirectionMode == kDirectionChordAllIndex || chordSpreadMode)
    {
        if (chordSpreadMode && !chordSpreadDownDirection)
        {
            for (int i = poolCount - 1; i >= 0; --i)
                appendSelectedByIndex(i);
        }
        else
        {
            for (int i = 0; i < poolCount; ++i)
                appendSelectedByIndex(i);
        }
    }
    else if (currentDirectionMode == kDirectionUpPairIndex
             || currentDirectionMode == kDirectionDownPairIndex)
    {
        const bool downPair = (currentDirectionMode == kDirectionDownPairIndex);
        const int safeCount = juce::jmax(1, poolCount);
        const int jump = juce::jlimit(1, 8, currentJumpSize);
        const int singleStep = jump % safeCount;

        if (arpWalkIndex < 0 || arpWalkIndex >= safeCount)
            arpWalkIndex = resolveInitialArpWalkIndexForSequence(safeCount);

        if (!juce::isPositiveAndBelow(arpWalkIndex, safeCount))
            arpWalkIndex = downPair ? 0 : (safeCount - 1);

        // Pair semantics: two successive single jumps in the current direction.
        const int firstIndex = downPair
            ? ((arpWalkIndex - singleStep + safeCount) % safeCount)
            : ((arpWalkIndex + singleStep) % safeCount);
        const int secondIndex = downPair
            ? ((firstIndex - singleStep + safeCount) % safeCount)
            : ((firstIndex + singleStep) % safeCount);
        arpWalkIndex = secondIndex;

        appendSelectedByIndex(firstIndex);
        appendSelectedByIndex(secondIndex);
    }
    else
    {
        const int keyIndex = chooseArpKeyIndex(poolCount);
        appendSelectedByIndex(keyIndex);
    }

    if (selectedCount <= 0)
        return;

    const double graceSpacingBeats = juce::jmax(1.0e-6, currentStepDurationBeats / 32.0);
    const double stepEndPpq = onsetBasePpq + juce::jmax(0.0, currentStepDurationBeats);
    // Timed gate safety:
    // keep all generated note-offs strictly inside the current step so the last
    // chord-strum/retrig event cannot overlap the next step onset.
    const double timedGateMaxOffPpq = juce::jmax(onsetBasePpq, stepEndPpq - 1.0e-9);
    const auto clampTimedGateOffPpq = [&](double eventOnsetPpq, double desiredOffPpq) noexcept
    {
        if (gateMode != GateMode::Timed)
            return desiredOffPpq;

        const double clamped = juce::jmin(desiredOffPpq, timedGateMaxOffPpq);
        return juce::jmax(eventOnsetPpq, clamped);
    };

    for (int r = 0; r < pacedRetrigs; ++r)
    {
        const double retrigOffset = retrigOffsets[(size_t) r];
        const double nextRetrigOffset = (r + 1 < pacedRetrigs
                                             ? retrigOffsets[(size_t) (r + 1)]
                                             : juce::jmax(0.0, currentStepDurationBeats));
        const double retrigSpan = juce::jmax(0.0, nextRetrigOffset - retrigOffset);
        const double gateDurationPerRetrig = (pacedRetrigs > 1
            ? retrigSpan * gateRatio
            : gateDurationBeats);
        const double chordStrumSpanBeats = (chordSpreadMode
            ? juce::jmax(0.0, retrigSpan) * juce::jlimit(0.0, 1.0, chordSpreadFraction)
            : 0.0);

        for (int i = 0; i < selectedCount; ++i)
        {
            const int srcKey = selectedSrcKeys[(size_t) i];
            const int outNote = selectedOutNotes[(size_t) i];
            const int srcCh0 = srcKey / kNumNotes;

            const int sourceVelocity = juce::jlimit(1, 127, selectedSourceVelocities[(size_t) i]);
            int outVel = applyVelocityPercentToSource(sourceVelocity, velocityPercentForTick);
            if (currentIsDrumMode)
                outVel = applyDrumVeloEnvelopeToVelocity(outVel, r, pacedRetrigs, currentDrumVeloEnvMode);

            const double perNoteOffsetBeats = (chordSpreadMode && selectedCount > 1)
                ? chordStrumSpanBeats * ((double) i / (double) (selectedCount - 1))
                : 0.0;
            const double onsetPpq = onsetBasePpq + retrigOffset + perNoteOffsetBeats;
            const double offPpq = clampTimedGateOffPpq(onsetPpq,
                                                       onsetPpq + gateDurationPerRetrig + legatoOffNudge);
            const int channel1 = srcCh0 + 1;

            const auto emitOrQueueNote = [&](double eventOnsetPpq,
                                             double eventOffPpq,
                                             int eventVelocity) noexcept
            {
                // A pre-hit that lands before this block cannot be emitted in the past.
                // We intentionally drop it at block boundaries to keep strict RT ordering.
                if (eventOnsetPpq < blockStartPpq - 1.0e-9)
                    return;

                if (eventOnsetPpq >= blockEndPpq - 1.0e-9)
                {
                    enqueueDeferredNoteOn(eventOnsetPpq,
                                          channel1,
                                          outNote,
                                          eventVelocity,
                                          gateMode,
                                          eventOffPpq);
                    return;
                }

                const int eventSample = ppqToSampleClamped(ctx, eventOnsetPpq);
                emitGeneratedNoteOn(out,
                                    eventSample,
                                    channel1,
                                    outNote,
                                    eventVelocity,
                                    gateMode,
                                    eventOffPpq);
            };

            if (graceCount > 0)
            {
                const int graceVelocity = juce::jlimit(1, 127, juce::roundToInt((float) outVel * 0.5f));

                for (int g = graceCount; g >= 1; --g)
                {
                    const double graceOffset = graceSpacingBeats * (double) g;
                    const double graceOnsetPpq = onsetPpq - graceOffset;
                    const double graceOffPpq = clampTimedGateOffPpq(
                        graceOnsetPpq,
                        graceOnsetPpq + gateDurationPerRetrig + legatoOffNudge);
                    emitOrQueueNote(graceOnsetPpq, graceOffPpq, graceVelocity);
                }
            }

            emitOrQueueNote(onsetPpq, offPpq, outVel);
        }
    }
}

void ArpeggiatorProcessor::emitDueDeferredEvents(juce::MidiBuffer& out,
                                                 const ProcessContext& ctx,
                                                 int sampleEndExclusive) noexcept
{
    if (sampleEndExclusive <= 0 || deferredEventCount <= 0)
        return;

    const double endPpq = sampleToPpq(ctx, sampleEndExclusive);
    bool poppedAny = false;

    while (deferredEventCount > 0)
    {
        const auto ev = deferredEvents[0];

        // Strictement dans ce segment.
        if (ev.ppq >= endPpq - 1.0e-9)
            break;

        const int samplePos = ppqToSampleClamped(ctx, ev.ppq);
        if (samplePos >= sampleEndExclusive)
            break;

        switch (ev.type)
        {
            case DeferredEventType::NoteOn:
                emitGeneratedNoteOn(out,
                                    samplePos,
                                    ev.channel1,
                                    ev.note,
                                    ev.velocity,
                                    ev.gateMode,
                                    ev.offOrExtendPpq);
                break;

            case DeferredEventType::Mute:
                flushAllGeneratedNotes(out, samplePos);
                break;
        }

        std::pop_heap(deferredEvents.begin(),
                      deferredEvents.begin() + deferredEventCount,
                      deferredEventHeapCompare);
        --deferredEventCount;
        poppedAny = true;
    }

    if (poppedAny)
        deferredWorstValid = false;
}

void ArpeggiatorProcessor::runTicksUntil(juce::MidiBuffer& out,
                                         const ProcessContext& ctx,
                                         int sampleEndExclusive) noexcept
{
    if (!ctx.isPlaying || ctx.numSamples <= 0 || sampleEndExclusive <= 0)
        return;

    const double ppqEnd = sampleToPpq(ctx, sampleEndExclusive);
    int guard = 0;

    struct RatePreviewState
    {
        std::array<int, kNumLayers> playbackCursorByLayer {};
        std::array<int, kNumLayers> playbackStepByLayer {};
        double nextRateStepPpq = 0.0;
        int currentDirectionMode = 1;
        int currentJumpSize = 1;
        double currentStepDurationBeats = 0.25;
        int currentOctaveOffset = 0;
        bool currentIsDrumMode = false;
        int currentGraceCount = 0;
        DrumVeloEnvMode currentDrumVeloEnvMode = DrumVeloEnvMode::Flat;
        int currentVelocityChoice = 0;
        int currentGrooveChoice = 0;
        int currentGateChoice = 0;
        int currentRetrigCount = 1;
        PaceMode currentPaceMode = PaceMode::Equal;
        int arpWalkIndex = -1;
        int64_t randomSeed = 0;
    };

    const auto captureRatePreviewState = [this]() noexcept
    {
        RatePreviewState s;
        s.playbackCursorByLayer = playbackCursorByLayer;
        s.playbackStepByLayer = playbackStepByLayer;
        s.nextRateStepPpq = nextRateStepPpq;
        s.currentDirectionMode = currentDirectionMode;
        s.currentJumpSize = currentJumpSize;
        s.currentStepDurationBeats = currentStepDurationBeats;
        s.currentOctaveOffset = currentOctaveOffset;
        s.currentIsDrumMode = currentIsDrumMode;
        s.currentGraceCount = currentGraceCount;
        s.currentDrumVeloEnvMode = currentDrumVeloEnvMode;
        s.currentVelocityChoice = currentVelocityChoice;
        s.currentGrooveChoice = currentGrooveChoice;
        s.currentGateChoice = currentGateChoice;
        s.currentRetrigCount = currentRetrigCount;
        s.currentPaceMode = currentPaceMode;
        s.arpWalkIndex = arpWalkIndex;
        s.randomSeed = playbackRandom.getSeed();
        return s;
    };

    const auto restoreRatePreviewState = [this](const RatePreviewState& s) noexcept
    {
        playbackCursorByLayer = s.playbackCursorByLayer;
        playbackStepByLayer = s.playbackStepByLayer;
        nextRateStepPpq = s.nextRateStepPpq;
        currentDirectionMode = s.currentDirectionMode;
        currentJumpSize = s.currentJumpSize;
        currentStepDurationBeats = s.currentStepDurationBeats;
        currentOctaveOffset = s.currentOctaveOffset;
        currentIsDrumMode = s.currentIsDrumMode;
        currentGraceCount = s.currentGraceCount;
        currentDrumVeloEnvMode = s.currentDrumVeloEnvMode;
        currentVelocityChoice = s.currentVelocityChoice;
        currentGrooveChoice = s.currentGrooveChoice;
        currentGateChoice = s.currentGateChoice;
        currentRetrigCount = s.currentRetrigCount;
        currentPaceMode = s.currentPaceMode;
        arpWalkIndex = s.arpWalkIndex;
        playbackRandom.setSeed(s.randomSeed);
    };

    while (guard < 1024)
    {
        const bool forceFirstRateTick = (playbackStepByLayer[(size_t) kLayerRate] < 0);

        int unlinkLayerToTick = -1;
        double bestUnlinkPpq = std::numeric_limits<double>::infinity();

        for (int layer = 1; layer < kNumLayers; ++layer)
        {
            if (readLinkModeRT(layer) != kLinkModeUnlink)
                continue;

            double& unlinkPpq = nextUnlinkStepPpqByLayer[(size_t) layer];
            if (unlinkPpq < ctx.ppqAtBlockStart - 32.0)
                unlinkPpq = ctx.ppqAtBlockStart;

            if (unlinkLayerToTick < 0 || unlinkPpq < bestUnlinkPpq - 1.0e-9)
            {
                bestUnlinkPpq = unlinkPpq;
                unlinkLayerToTick = layer;
            }
        }

        bool processRateTick = false;
        double nextEventPpq = 0.0;
        double rateTickNominalPpq = nextRateStepPpq;
        double rateTickProcessPpq = nextRateStepPpq;
        bool ratePreviewActive = false;
        bool ratePreviewValid = false;
        RatePreviewState previewSnapshot;

        if (std::isfinite(nextRateStepPpq))
        {
            previewSnapshot = captureRatePreviewState();
            ratePreviewActive = true;
            rateTickNominalPpq = nextRateStepPpq;

            if (triggerOneRateStep())
            {
                // Groove contract for scheduler:
                // - Negative groove anticipates the tick processing instant.
                // - Positive groove never delays the sequencing clock.
                //   (delay is still applied musically inside processTickAtPpq()).
                double grooveOffsetBeats = 0.0;
                resolveGrooveOffsetBeats(currentGrooveChoice,
                                         currentStepDurationBeats,
                                         grooveOffsetBeats);
                rateTickProcessPpq = rateTickNominalPpq + juce::jmin(0.0, grooveOffsetBeats);
                ratePreviewValid = true;
            }
            else
            {
                restoreRatePreviewState(previewSnapshot);
                ratePreviewActive = false;
            }
        }

        if (ratePreviewValid)
        {
            // Deterministic tie-break:
            // - The sequencing clock is always the nominal Rythm tick.
            // - Groove can only advance processing for negative offsets
            //   (anticipation), never delay it.
            // This keeps progression robust while enabling per-step anticipation.
            if (forceFirstRateTick
                || unlinkLayerToTick < 0
                || rateTickProcessPpq <= bestUnlinkPpq + 1.0e-9)
            {
                processRateTick = true;
                nextEventPpq = rateTickProcessPpq;
            }
            else
            {
                nextEventPpq = bestUnlinkPpq;
            }
        }
        else if (unlinkLayerToTick >= 0)
        {
            nextEventPpq = bestUnlinkPpq;
        }
        else
        {
            break;
        }

        // Un tick exactement en fin appartient au segment suivant.
        if (nextEventPpq >= ppqEnd - 1.0e-9)
        {
            if (ratePreviewActive)
                restoreRatePreviewState(previewSnapshot);
            break;
        }

        const int tickSample = ppqToSampleClamped(ctx, nextEventPpq);
        if (tickSample >= sampleEndExclusive)
        {
            if (ratePreviewActive)
                restoreRatePreviewState(previewSnapshot);
            break;
        }

        if (!processRateTick
            && unlinkLayerToTick >= 1
            && std::abs(nextUnlinkStepPpqByLayer[(size_t) unlinkLayerToTick] - nextEventPpq) <= 1.0e-9)
        {
            if (ratePreviewActive)
                restoreRatePreviewState(previewSnapshot);

            if (!triggerOneUnlinkLayerStep(unlinkLayerToTick))
                nextUnlinkStepPpqByLayer[(size_t) unlinkLayerToTick] = juce::jmax(nextRateStepPpq, nextEventPpq + 1.0e-6);
        }
        else
        {
            // Keep previewed trigger state (already advanced by triggerOneRateStep()).
            processTickAtPpq(out, ctx, rateTickNominalPpq, tickSample);
        }

        ++guard;
    }
}

void ArpeggiatorProcessor::process(juce::MidiBuffer& midi, const ProcessContext& ctx)
{
    // Sequence de traitement par bloc:
    // A) gerer transitions enable/disable
    // B) gerer mode bypass ou transport stop
    // C) gerer discontinuite transport
    // D) segmenter le bloc autour des messages entrants
    // E) pour chaque segment: offs -> differes -> ticks -> differes -> offs
    // F) merger outScratch -> midi
    outScratch.clear();

    // Garde-fou de coherence:
    // si un restore/sync externe desynchronise le cache compact des touches tenues,
    // on le reconstruit depuis la matrice source avant de traiter le bloc.
    if (heldKeysSortedCount != heldActiveNoteCount)
        rebuildHeldKeyCacheFromMatrixRT();

    const bool enabled = readEnabledRT();

    if (lastEnabled && !enabled)
    {
        flushAllGeneratedNotes(outScratch, 0);
        emitBypassHeldSnapshot(outScratch, 0);
        clearPlaybackState();
    }
    else if (!lastEnabled && enabled)
    {
        emitBypassHeldNoteOffSnapshot(outScratch, 0);
        clearPlaybackState();
    }

    if (!enabled)
    {
        for (auto it = midi.begin(); it != midi.end(); ++it)
        {
            const auto meta = *it;
            const auto& msg = meta.getMessage();
            const int samplePos = juce::jmax(0, meta.samplePosition);

            if (isNoteOnMessage(msg))
            {
                handleTrackedNoteOn(msg.getChannel(),
                                    msg.getNoteNumber(),
                                    velocityFloatToMidi127(msg.getVelocity()));
            }
            else if (isNoteOffMessage(msg))
            {
                handleTrackedNoteOff(msg.getChannel(), msg.getNoteNumber());
            }

            outScratch.addEvent(msg, samplePos);
        }

        lastEnabled = enabled;
        wasPlaying = ctx.isPlaying;
        hasLastBlockPpq = ctx.isPlaying;
        lastBlockPpq = ctx.ppqAtBlockStart;
        rateIdleResetLatched = false;
        midi.swapWith(outScratch);
        return;
    }

    if (!ctx.isPlaying)
    {
        if (wasPlaying)
            flushAllGeneratedNotes(outScratch, 0);

        clearPlaybackState();

        for (auto it = midi.begin(); it != midi.end(); ++it)
        {
            const auto meta = *it;
            const auto& msg = meta.getMessage();
            const int samplePos = juce::jmax(0, meta.samplePosition);

            if (isNoteOnMessage(msg))
            {
                handleTrackedNoteOn(msg.getChannel(),
                                    msg.getNoteNumber(),
                                    velocityFloatToMidi127(msg.getVelocity()));
                continue;
            }

            if (isNoteOffMessage(msg))
            {
                handleTrackedNoteOff(msg.getChannel(), msg.getNoteNumber());
                continue;
            }

            outScratch.addEvent(msg, samplePos);
        }

        wasPlaying = false;
        hasLastBlockPpq = false;
        lastEnabled = enabled;
        rateIdleResetLatched = false;
        midi.swapWith(outScratch);
        return;
    }

    const double safeRate = juce::jmax(1.0, ctx.sampleRate);
    const double safeBpm = juce::jmax(1.0, ctx.bpm);
    const double beatsPerSample = juce::jmax(1.0e-12, (safeBpm / 60.0) / safeRate);
    const double expectedPpq = hasLastBlockPpq
        ? (lastBlockPpq + (double) juce::jmax(0, ctx.numSamples) * beatsPerSample)
        : ctx.ppqAtBlockStart;

    const double ppqDrift = ctx.ppqAtBlockStart - expectedPpq;
    const bool jumpedBackwards =
        hasLastBlockPpq && (ctx.ppqAtBlockStart + 1.0e-9 < lastBlockPpq);
    const bool hugeDiscontinuity =
        hasLastBlockPpq && (std::abs(ppqDrift) > 8.0);
    const bool ppqJumped =
        (!hasLastBlockPpq) || jumpedBackwards || hugeDiscontinuity;

    if (ctx.justStarted || !wasPlaying || ppqJumped)
    {
        flushAllGeneratedNotes(outScratch, 0);
        resetPlaybackState(ctx.ppqAtBlockStart);
    }
    else if (nextRateStepPpq + 32.0 < ctx.ppqAtBlockStart)
    {
        nextRateStepPpq = ctx.ppqAtBlockStart;
    }

    // En edition live, la longueur effective peut diminuer.
    // Re-clamp des curseurs pour rester deterministe et eviter des indices stale.
    sanitizePlaybackCursorsToLengthsRT();

    const int blockSamples = juce::jmax(0, ctx.numSamples);
    const bool hasIncomingEvents = (midi.begin() != midi.end());

    // Link/unlink doit rester deterministe meme si change en plein playback.
    reconcileLinkTransitions(ctx.ppqAtBlockStart);

    // Robustesse temps reel:
    // si une horloge de couche est en retard (jitter host, reprise apres retard,
    // edition live dense), on la rebase sur le debut de bloc au lieu de rattraper
    // potentiellement des dizaines de ticks a sample 0.
    //
    // Compromis assume:
    // - on privilegie la stabilite RT (pas de burst CPU/audio crackle)
    //   plutot qu un rattrapage historique strict.
    if (nextRateStepPpq < ctx.ppqAtBlockStart - 1.0e-9 || !std::isfinite(nextRateStepPpq))
        nextRateStepPpq = ctx.ppqAtBlockStart;

    for (int layer = 1; layer < kNumLayers; ++layer)
    {
        if (readLinkModeRT(layer) != kLinkModeUnlink)
            continue;

        double& unlinkPpq = nextUnlinkStepPpqByLayer[(size_t) layer];
        if (unlinkPpq < ctx.ppqAtBlockStart - 1.0e-9 || !std::isfinite(unlinkPpq))
            unlinkPpq = ctx.ppqAtBlockStart;
    }

    const bool rateLinkControlsIdleReset = (readLinkModeRT(kLayerRate) == kLinkModeLink); // "R" mode.
    bool anyHeldNotes = hasAnyHeldNotesRT();
    bool primedSampleZeroHeldState = false;

    // Prime etat note a sample 0 avant premier tick.
    // Permet un demarrage propre quand un clip commence exactement au debut.
    if (hasIncomingEvents)
    {
        for (auto it = midi.begin(); it != midi.end(); ++it)
        {
            const auto meta = *it;
            if (meta.samplePosition > 0)
                break;

            if (meta.samplePosition < 0)
                continue;

            const auto& msg = meta.getMessage();
            if (isNoteOnMessage(msg))
            {
                handleTrackedNoteOn(msg.getChannel(),
                                    msg.getNoteNumber(),
                                    velocityFloatToMidi127(msg.getVelocity()));
                primedSampleZeroHeldState = true;
                anyHeldNotes = true;
                continue;
            }

            if (isNoteOffMessage(msg))
            {
                handleTrackedNoteOff(msg.getChannel(), msg.getNoteNumber());
                primedSampleZeroHeldState = true;
                anyHeldNotes = hasAnyHeldNotesRT();
                continue;
            }
        }
    }

    int cursorSample = 0;

    auto applyIdleResetIfNeeded = [&](double atPpq) -> bool
    {
        if (!(rateLinkControlsIdleReset && !anyHeldNotes))
        {
            rateIdleResetLatched = false;
            return false;
        }

        if (!rateIdleResetLatched)
        {
            resetLinkedSequencersForIdle(atPpq);
            rateIdleResetLatched = true;
        }

        return true;
    };

    auto processSegmentUntil = [&](int segmentEndExclusive)
    {
        if (segmentEndExclusive <= cursorSample)
            return;

        // Ordre volontaire dans chaque segment:
        // 1) OFF deja planifies
        // 2) evenements differes (NoteOn/Mute)
        // 3) OFF a nouveau pour couvrir les offs declenches par (2)
        // 4) ticks arpeggio
        // 5) second drain differes + offs pour stabiliser la fin de segment
        //
        // Cet ordre limite les inversions NoteOff/NoteOn aux memes timestamps.
        emitDueScheduledOffs(outScratch, ctx, segmentEndExclusive);
        emitDueDeferredEvents(outScratch, ctx, segmentEndExclusive);
        emitDueScheduledOffs(outScratch, ctx, segmentEndExclusive);

        // Rate Link ("R") special behavior:
        // when no note is held, keep sequencers reset/frozen.
        // Reset is latched to avoid rebasing timeline at every segment boundary.
        if (!applyIdleResetIfNeeded(sampleToPpq(ctx, segmentEndExclusive)))
            runTicksUntil(outScratch, ctx, segmentEndExclusive);

        emitDueDeferredEvents(outScratch, ctx, segmentEndExclusive);
        emitDueScheduledOffs(outScratch, ctx, segmentEndExclusive);
        cursorSample = segmentEndExclusive;
    };

    // Drain initial [0,1):
    // applique les differes/off a sample 0 avant scan des MIDI entrants.
    const int initialSegmentEnd = (blockSamples > 0 || hasIncomingEvents) ? 1 : 0;
    processSegmentUntil(initialSegmentEnd);

    for (auto it = midi.begin(); it != midi.end(); ++it)
    {
        const auto meta = *it;
        const auto& msg = meta.getMessage();
        const int samplePos = juce::jlimit(0, juce::jmax(0, ctx.numSamples), meta.samplePosition);

        processSegmentUntil(samplePos);

        if (isNoteOnMessage(msg))
        {
            if (primedSampleZeroHeldState && samplePos == 0)
                continue;

            if (rateLinkControlsIdleReset && !anyHeldNotes)
            {
                resetLinkedSequencersForIdle(sampleToPpq(ctx, samplePos));
                rateIdleResetLatched = true;
            }

            handleTrackedNoteOn(msg.getChannel(),
                                msg.getNoteNumber(),
                                velocityFloatToMidi127(msg.getVelocity()));
            anyHeldNotes = true;
            rateIdleResetLatched = false;
            continue;
        }

        if (isNoteOffMessage(msg))
        {
            if (primedSampleZeroHeldState && samplePos == 0)
                continue;

            handleTrackedNoteOff(msg.getChannel(), msg.getNoteNumber());
            anyHeldNotes = hasAnyHeldNotesRT();

            if (rateLinkControlsIdleReset && !anyHeldNotes)
            {
                resetLinkedSequencersForIdle(sampleToPpq(ctx, samplePos));
                rateIdleResetLatched = true;
            }
            else if (anyHeldNotes)
            {
                rateIdleResetLatched = false;
            }
            continue;
        }

        outScratch.addEvent(msg, samplePos);
    }

    processSegmentUntil(blockSamples);

    wasPlaying = true;
    hasLastBlockPpq = true;
    lastBlockPpq = ctx.ppqAtBlockStart;
    lastEnabled = enabled;

    midi.swapWith(outScratch);
}
