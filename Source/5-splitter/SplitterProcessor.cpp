/*
==============================================================================
SplitterProcessor.cpp
------------------------------------------------------------------------------
Role in architecture:
- Audio-thread routing engine placed at the end of each lane chain.
- Converts held source notes into branch-specific desired outputs according to:
  - splitter enable,
  - mode (round robin / range split),
  - per-branch active flag, target channel, limits, and ranges.
- Applies a strict desired->output diff to avoid stuck notes:
  - NoteOff for obsolete outputs first,
  - then NoteOn for new outputs.

Core invariants:
- All note lifetime decisions are made from sourceHeld* state, never from
  transient per-block ordering.
- Every sounding output note is represented in outputActive/outputChannel.
- Any config or topology change is reconciled through applyDesiredDiff().

RT contract:
- No locks.
- No heap allocations in process hot path (buffers are preallocated/reset).
==============================================================================
*/

#include "SplitterProcessor.h"

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace
{
    // JUCE velocity API compatibility helper (float or uint8 variants).
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
}

bool SplitterProcessor::Config::operator==(const Config& other) const noexcept
{
    if (enabled != other.enabled)
        return false;

    if (mode != other.mode)
        return false;

    for (size_t i = 0; i < active.size(); ++i)
    {
        if (active[i]     != other.active[i])     return false;
        if (channel[i]    != other.channel[i])    return false;
        if (voiceLimit[i] != other.voiceLimit[i]) return false;
        if (noteMin[i]    != other.noteMin[i])    return false;
        if (noteMax[i]    != other.noteMax[i])    return false;
    }

    return true;
}

bool SplitterProcessor::isNoteOnMessage(const juce::MidiMessage& msg) noexcept
{
    return msg.isNoteOn() && msg.getVelocity() > 0.0f;
}

bool SplitterProcessor::isNoteOffMessage(const juce::MidiMessage& msg) noexcept
{
    return msg.isNoteOff() || (msg.isNoteOn() && msg.getVelocity() == 0.0f);
}

SplitterProcessor::SplitterProcessor(juce::AudioProcessorValueTreeState& vts,
                                     Lanes::Lane laneIn)
    : parameters(vts)
    , lane(laneIn)
{
    const auto id = [&](const char* baseId) { return ParamIDs::lane(baseId, lane); };

    splitterEnableRaw = parameters.getRawParameterValue(id(ParamIDs::Base::splitterEnable));

    splitterModeChoice = dynamic_cast<juce::AudioParameterChoice*>(
        parameters.getParameter(id(ParamIDs::Base::splitterMode)));

    lineActive[0] = parameters.getRawParameterValue(id(ParamIDs::Base::splitLineActive01));
    line1ChannelChoice = dynamic_cast<juce::AudioParameterChoice*>(
        parameters.getParameter(id(ParamIDs::Base::splitLine1Channel)));

    static constexpr const char* kActiveIds[kNumExtraBranches] =
    {
        ParamIDs::Base::splitLineActive02,
        ParamIDs::Base::splitLineActive03,
        ParamIDs::Base::splitLineActive04,
        ParamIDs::Base::splitLineActive05
    };

    static constexpr const char* kVoiceLimitIds[kNumExtraBranches] =
    {
        ParamIDs::Base::splitLine2VoiceLimit,
        ParamIDs::Base::splitLine3VoiceLimit,
        ParamIDs::Base::splitLine4VoiceLimit,
        ParamIDs::Base::splitLine5VoiceLimit
    };

    static constexpr const char* kNoteMinIds[kNumExtraBranches] =
    {
        ParamIDs::Base::splitLine2NoteMin,
        ParamIDs::Base::splitLine3NoteMin,
        ParamIDs::Base::splitLine4NoteMin,
        ParamIDs::Base::splitLine5NoteMin
    };

    static constexpr const char* kNoteMaxIds[kNumExtraBranches] =
    {
        ParamIDs::Base::splitLine2NoteMax,
        ParamIDs::Base::splitLine3NoteMax,
        ParamIDs::Base::splitLine4NoteMax,
        ParamIDs::Base::splitLine5NoteMax
    };

    static constexpr const char* kChannelIds[kNumExtraBranches] =
    {
        ParamIDs::Base::splitLine2Channel,
        ParamIDs::Base::splitLine3Channel,
        ParamIDs::Base::splitLine4Channel,
        ParamIDs::Base::splitLine5Channel
    };

    for (int i = 0; i < kNumExtraBranches; ++i)
    {
        lineActive[i + 1] = parameters.getRawParameterValue(id(kActiveIds[i]));
        lineExtraVoiceLimit[i] = parameters.getRawParameterValue(id(kVoiceLimitIds[i]));
        lineExtraVoiceLimitParam[i] = dynamic_cast<juce::AudioParameterInt*>(
            parameters.getParameter(id(kVoiceLimitIds[i])));
        lineExtraNoteMin[i] = parameters.getRawParameterValue(id(kNoteMinIds[i]));
        lineExtraNoteMinParam[i] = dynamic_cast<juce::AudioParameterInt*>(
            parameters.getParameter(id(kNoteMinIds[i])));
        lineExtraNoteMax[i] = parameters.getRawParameterValue(id(kNoteMaxIds[i]));
        lineExtraNoteMaxParam[i] = dynamic_cast<juce::AudioParameterInt*>(
            parameters.getParameter(id(kNoteMaxIds[i])));
        lineExtraChannel[i] = parameters.getRawParameterValue(id(kChannelIds[i]));
        lineExtraChannelParam[i] = dynamic_cast<juce::AudioParameterInt*>(
            parameters.getParameter(id(kChannelIds[i])));
    }

    outScratch.clear();
    outScratch.ensureSize(2048);

    resetAllTrackingRT();
}

void SplitterProcessor::resetAllTrackingRT() noexcept
{
    // Full runtime reset used on prepare/panic/topology hard reset.
    // We clear source, desired, and currently sounding output states.
    for (auto& ch : sourceHeldCount)          ch.fill(0);
    for (auto& ch : sourceHeldVelocity)       ch.fill(0);
    for (auto& ch : sourceVelocityStack)
        for (auto& stack : ch)
            stack.fill(0);
    for (auto& ch : sourceVelocityStackSize)  ch.fill(0);
    for (auto& ch : sourceTimestamp)          ch.fill(0);
    for (auto& ch : sourceAssignedBranch)     ch.fill(0);

    for (auto& br : desiredStamp)
        for (auto& ch : br)
            ch.fill(0);
    for (auto& br : desiredVelocity)
        for (auto& ch : br)
            ch.fill(0);
    for (auto& br : desiredChannel)
        for (auto& ch : br)
            ch.fill(0);

    for (auto& br : outputActive)
        for (auto& ch : br)
            ch.fill(0);
    for (auto& br : outputVelocity)
        for (auto& ch : br)
            ch.fill(0);
    for (auto& br : outputChannel)
        for (auto& ch : br)
            ch.fill(0);

    desiredBuildGeneration = 1;
    desiredKeyCount.fill(0);
    outputKeyCount.fill(0);
    outputKeyCacheValid = true;

    timestampCounter = 0;
    roundRobinCursor = 0;
    hasLastConfig = false;

    outScratch.clear();
}

void SplitterProcessor::beginDesiredBuild() noexcept
{
    auto nextGeneration = (uint16_t) (desiredBuildGeneration + 1);

    if (nextGeneration == 0)
    {
        for (auto& br : desiredStamp)
            for (auto& ch : br)
                ch.fill(0);

        nextGeneration = 1;
    }

    desiredBuildGeneration = nextGeneration;
    desiredKeyCount.fill(0);
}

bool SplitterProcessor::isDesiredActiveForCurrentBuild(int branch, int ch0, int note) const noexcept
{
    return desiredStamp[(size_t) branch][(size_t) ch0][(size_t) note] == desiredBuildGeneration;
}

void SplitterProcessor::setDesiredNoteForCurrentBuild(int branch,
                                                      int ch0,
                                                      int note,
                                                      uint8_t velocity,
                                                      uint8_t outChannel) noexcept
{
    auto& stamp = desiredStamp[(size_t) branch][(size_t) ch0][(size_t) note];
    const bool wasAlreadyMarked = (stamp == desiredBuildGeneration);
    stamp = desiredBuildGeneration;

    desiredVelocity[(size_t) branch][(size_t) ch0][(size_t) note] =
        (uint8_t) juce::jlimit(1, 127, (int) velocity);
    desiredChannel[(size_t) branch][(size_t) ch0][(size_t) note] =
        (uint8_t) juce::jlimit(1, 16, (int) outChannel);

    if (wasAlreadyMarked)
        return;

    auto& count = desiredKeyCount[(size_t) branch];
    if (count >= kNumKeys)
        return;

    desiredKeys[(size_t) branch][(size_t) count++] = keyIndex(ch0, note);
}

void SplitterProcessor::rebuildOutputKeyCache() noexcept
{
    outputKeyCount.fill(0);

    for (int branch = 0; branch < kNumBranches; ++branch)
    {
        auto& count = outputKeyCount[(size_t) branch];

        for (int ch0 = 0; ch0 < kNumMidiChannels; ++ch0)
        {
            for (int nt = 0; nt < kNumNotes; ++nt)
            {
                if (outputActive[(size_t) branch][(size_t) ch0][(size_t) nt] == 0)
                    continue;

                if (count >= kNumKeys)
                    continue;

                outputKeys[(size_t) branch][(size_t) count++] = keyIndex(ch0, nt);
            }
        }
    }

    outputKeyCacheValid = true;
}

void SplitterProcessor::ensureOutputKeyCache() noexcept
{
    if (!outputKeyCacheValid)
        rebuildOutputKeyCache();
}

SplitterProcessor::Config SplitterProcessor::readConfigRT() const noexcept
{
    // Read once per block and normalize values so downstream code can assume:
    // - channels in expected domain,
    // - note min/max ordered,
    // - voice limits clamped.
    Config cfg;

    if (splitterEnableRaw != nullptr)
        cfg.enabled = (splitterEnableRaw->load() > 0.5f);

    if (splitterModeChoice != nullptr)
        cfg.mode = juce::jlimit(0, 1, splitterModeChoice->getIndex());

    for (int i = 0; i < kNumBranches; ++i)
    {
        if (lineActive[i] != nullptr)
            cfg.active[(size_t) i] = (lineActive[i]->load() > 0.5f);
    }

    if (line1ChannelChoice != nullptr)
    {
        const int idx = juce::jlimit(0, 16, line1ChannelChoice->getIndex());
        cfg.channel[0] = (idx == 0 ? 0 : idx); // 0=Mute, 1..16 MIDI channels
    }

    for (int i = 0; i < kNumExtraBranches; ++i)
    {
        const int branch = i + 1;

        if (lineExtraVoiceLimitParam[i] != nullptr)
            cfg.voiceLimit[(size_t) branch] = juce::jlimit(1, 16, lineExtraVoiceLimitParam[i]->get());
        else if (lineExtraVoiceLimit[i] != nullptr)
            cfg.voiceLimit[(size_t) branch] = juce::jlimit(1, 16, (int) lineExtraVoiceLimit[i]->load());

        int rawMin = 0;
        if (lineExtraNoteMinParam[i] != nullptr)
            rawMin = lineExtraNoteMinParam[i]->get();
        else if (lineExtraNoteMin[i] != nullptr)
            rawMin = (int) lineExtraNoteMin[i]->load();

        int rawMax = 127;
        if (lineExtraNoteMaxParam[i] != nullptr)
            rawMax = lineExtraNoteMaxParam[i]->get();
        else if (lineExtraNoteMax[i] != nullptr)
            rawMax = (int) lineExtraNoteMax[i]->load();

        cfg.noteMin[(size_t) branch] = juce::jlimit(0, 127, std::min(rawMin, rawMax));
        cfg.noteMax[(size_t) branch] = juce::jlimit(0, 127, std::max(rawMin, rawMax));

        if (lineExtraChannelParam[i] != nullptr)
            cfg.channel[(size_t) branch] = juce::jlimit(1, 16, lineExtraChannelParam[i]->get());
        else if (lineExtraChannel[i] != nullptr)
            cfg.channel[(size_t) branch] = juce::jlimit(1, 16, (int) lineExtraChannel[i]->load());
    }

    return cfg;
}

int SplitterProcessor::selectNextRoundRobinBranch(const Config& cfg) noexcept
{
    // Cursor walks only extra branches (line2..line5).
    // Return 0 when no extra branch is active.
    for (int step = 0; step < kNumExtraBranches; ++step)
    {
        const int extraIndex = (roundRobinCursor + step) % kNumExtraBranches; // 0..3
        const int branchIndex = extraIndex + 1;                                // 1..4

        if (cfg.active[(size_t) branchIndex])
        {
            roundRobinCursor = (extraIndex + 1) % kNumExtraBranches;
            return branchIndex;
        }
    }

    return 0;
}

void SplitterProcessor::handleSourceNoteOn(const Config& cfg,
                                           int channel1,
                                           int note,
                                           uint8_t velocity) noexcept
{
    // Source state keeps stacked note-ons for robust off/retrigger behavior.
    const int ch0 = juce::jlimit(1, kNumMidiChannels, channel1) - 1;
    const int nt  = juce::jlimit(0, kNumNotes - 1, note);

    auto& count = sourceHeldCount[(size_t) ch0][(size_t) nt];
    auto& vel   = sourceHeldVelocity[(size_t) ch0][(size_t) nt];
    auto& stack = sourceVelocityStack[(size_t) ch0][(size_t) nt];
    auto& stackSize = sourceVelocityStackSize[(size_t) ch0][(size_t) nt];
    auto& ts = sourceTimestamp[(size_t) ch0][(size_t) nt];
    auto& assigned = sourceAssignedBranch[(size_t) ch0][(size_t) nt];

    const bool wasOff = (count == 0);

    if (count < 255)
        ++count;

    pushVelocityStack(stack, stackSize, velocity);
    vel = topVelocityStack(stack, stackSize, velocity);
    ts = ++timestampCounter;

    if (wasOff)
    {
        assigned = 0;

        if (cfg.mode == 0)
            assigned = (uint8_t) juce::jlimit(0, 4, selectNextRoundRobinBranch(cfg));
    }
}

bool SplitterProcessor::handleSourceNoteOff(int channel1, int note) noexcept
{
    // Returns false when there was no tracked held key for this source note.
    const int ch0 = juce::jlimit(1, kNumMidiChannels, channel1) - 1;
    const int nt  = juce::jlimit(0, kNumNotes - 1, note);

    auto& count = sourceHeldCount[(size_t) ch0][(size_t) nt];
    auto& vel   = sourceHeldVelocity[(size_t) ch0][(size_t) nt];
    auto& stack = sourceVelocityStack[(size_t) ch0][(size_t) nt];
    auto& stackSize = sourceVelocityStackSize[(size_t) ch0][(size_t) nt];
    auto& ts = sourceTimestamp[(size_t) ch0][(size_t) nt];
    auto& assigned = sourceAssignedBranch[(size_t) ch0][(size_t) nt];

    if (count == 0)
        return false;

    --count;

    if (stackSize > 0)
        --stackSize;

    if (count == 0)
    {
        vel = 0;
        ts = 0;
        assigned = 0;
        stackSize = 0;
        stack.fill(0);
    }
    else
    {
        vel = topVelocityStack(stack, stackSize, vel > 0 ? vel : (uint8_t) 100);
    }

    return true;
}

void SplitterProcessor::ensureRoundRobinAssignments(const Config& cfg)
{
    // If assignments became invalid after config edits (branch disabled, mode
    // switch, etc.), rebuild missing assignments for currently held source keys.
    if (cfg.mode != 0)
        return;

    bool anyExtraActive = false;
    for (int b = 1; b <= kNumExtraBranches; ++b)
    {
        if (cfg.active[(size_t) b])
        {
            anyExtraActive = true;
            break;
        }
    }

    int missingCount = 0;

    for (int ch0 = 0; ch0 < kNumMidiChannels; ++ch0)
    {
        for (int nt = 0; nt < kNumNotes; ++nt)
        {
            if (sourceHeldCount[(size_t) ch0][(size_t) nt] == 0)
            {
                sourceAssignedBranch[(size_t) ch0][(size_t) nt] = 0;
                continue;
            }

            auto& assigned = sourceAssignedBranch[(size_t) ch0][(size_t) nt];

            if (!anyExtraActive)
            {
                assigned = 0;
                continue;
            }

            if (assigned < 1 || assigned > 4 || !cfg.active[(size_t) assigned])
                assigned = 0;

            if (assigned == 0 && missingCount < kNumKeys)
                candidateKeys[(size_t) missingCount++] = keyIndex(ch0, nt);
        }
    }

    if (!anyExtraActive || missingCount == 0)
        return;

    std::sort(candidateKeys.begin(),
              candidateKeys.begin() + missingCount,
              [&](int a, int b)
              {
                  const int ach = channelFromKey(a);
                  const int ant = noteFromKey(a);
                  const int bch = channelFromKey(b);
                  const int bnt = noteFromKey(b);

                  const uint64_t tsa = sourceTimestamp[(size_t) ach][(size_t) ant];
                  const uint64_t tsb = sourceTimestamp[(size_t) bch][(size_t) bnt];

                  if (tsa != tsb)
                      return tsa < tsb; // oldest first

                  return a < b;
              });

    for (int i = 0; i < missingCount; ++i)
    {
        const int key = candidateKeys[(size_t) i];
        const int ch0 = channelFromKey(key);
        const int nt  = noteFromKey(key);

        sourceAssignedBranch[(size_t) ch0][(size_t) nt] =
            (uint8_t) juce::jlimit(0, 4, selectNextRoundRobinBranch(cfg));
    }
}

void SplitterProcessor::chooseRoundRobinVoicesForBranch(const Config& cfg, int branchIndex)
{
    // "Last" priority policy:
    // - gather held keys assigned to this branch,
    // - sort by newest timestamp first,
    // - keep up to voiceLimit keys.
    if (branchIndex < 1 || branchIndex > 4)
        return;

    int count = 0;

    for (int ch0 = 0; ch0 < kNumMidiChannels; ++ch0)
    {
        for (int nt = 0; nt < kNumNotes; ++nt)
        {
            if (sourceHeldCount[(size_t) ch0][(size_t) nt] == 0)
                continue;

            if (sourceAssignedBranch[(size_t) ch0][(size_t) nt] != (uint8_t) branchIndex)
                continue;

            if (count < kNumKeys)
                candidateKeys[(size_t) count++] = keyIndex(ch0, nt);
        }
    }

    if (count <= 0)
        return;

    std::sort(candidateKeys.begin(),
              candidateKeys.begin() + count,
              [&](int a, int b)
              {
                  const int ach = channelFromKey(a);
                  const int ant = noteFromKey(a);
                  const int bch = channelFromKey(b);
                  const int bnt = noteFromKey(b);

                  const auto tsa = sourceTimestamp[(size_t) ach][(size_t) ant];
                  const auto tsb = sourceTimestamp[(size_t) bch][(size_t) bnt];

                  // Fixed strategy: Last
                  if (tsa != tsb) return tsa > tsb;
                  if (ant != bnt) return ant < bnt;
                  return a < b;
              });

    const int keep = juce::jmin(count, juce::jlimit(1, 16, cfg.voiceLimit[(size_t) branchIndex]));
    const int outChannel = juce::jlimit(1, 16, cfg.channel[(size_t) branchIndex]);

    for (int i = 0; i < keep; ++i)
    {
        const int key = candidateKeys[(size_t) i];
        const int ch0 = channelFromKey(key);
        const int nt  = noteFromKey(key);

        const uint8_t vel = (uint8_t) juce::jlimit(1, 127,
            (int) (sourceHeldVelocity[(size_t) ch0][(size_t) nt] > 0
                ? sourceHeldVelocity[(size_t) ch0][(size_t) nt]
                : 100));

        setDesiredNoteForCurrentBuild(branchIndex, ch0, nt, vel, (uint8_t) outChannel);
    }
}

void SplitterProcessor::buildDesiredState(const Config& cfg)
{
    // Routing policy matrix:
    // - splitter disabled: global bypass (source notes on source channels).
    // - splitter enabled: line1 direct out + mode-specific extra branches.
    // - if all branches inactive, desired state remains empty -> full note off.
    beginDesiredBuild();

    if (!cfg.enabled)
    {
        // Splitter bypass: keep source notes on their original channel.
        for (int ch0 = 0; ch0 < kNumMidiChannels; ++ch0)
        {
            for (int nt = 0; nt < kNumNotes; ++nt)
            {
                if (sourceHeldCount[(size_t) ch0][(size_t) nt] == 0)
                    continue;

                const uint8_t vel = (uint8_t) juce::jlimit(1, 127,
                    (int) (sourceHeldVelocity[(size_t) ch0][(size_t) nt] > 0
                        ? sourceHeldVelocity[(size_t) ch0][(size_t) nt]
                        : 100));

                setDesiredNoteForCurrentBuild(0, ch0, nt, vel, (uint8_t) (ch0 + 1));
            }
        }

        return;
    }

    // Line 1: direct out of all held notes to selected channel.
    const bool line1Active = cfg.active[0];
    const int line1Channel = cfg.channel[0]; // 0 = Mute

    if (line1Active && line1Channel >= 1 && line1Channel <= 16)
    {
        for (int ch0 = 0; ch0 < kNumMidiChannels; ++ch0)
        {
            for (int nt = 0; nt < kNumNotes; ++nt)
            {
                if (sourceHeldCount[(size_t) ch0][(size_t) nt] == 0)
                    continue;

                const uint8_t vel = (uint8_t) juce::jlimit(1, 127,
                    (int) (sourceHeldVelocity[(size_t) ch0][(size_t) nt] > 0
                        ? sourceHeldVelocity[(size_t) ch0][(size_t) nt]
                        : 100));

                setDesiredNoteForCurrentBuild(0, ch0, nt, vel, (uint8_t) line1Channel);
            }
        }
    }

    if (cfg.mode == 0) // RoundRobin
    {
        ensureRoundRobinAssignments(cfg);

        for (int branch = 1; branch <= kNumExtraBranches; ++branch)
        {
            if (!cfg.active[(size_t) branch])
                continue;

            chooseRoundRobinVoicesForBranch(cfg, branch);
        }

        return;
    }

    // Range split
    for (int branch = 1; branch <= kNumExtraBranches; ++branch)
    {
        if (!cfg.active[(size_t) branch])
            continue;

        const int outChannel = juce::jlimit(1, 16, cfg.channel[(size_t) branch]);
        const int nMin = juce::jlimit(0, 127, std::min(cfg.noteMin[(size_t) branch], cfg.noteMax[(size_t) branch]));
        const int nMax = juce::jlimit(0, 127, std::max(cfg.noteMin[(size_t) branch], cfg.noteMax[(size_t) branch]));

        for (int ch0 = 0; ch0 < kNumMidiChannels; ++ch0)
        {
            for (int nt = 0; nt < kNumNotes; ++nt)
            {
                if (sourceHeldCount[(size_t) ch0][(size_t) nt] == 0)
                    continue;

                if (nt < nMin || nt > nMax)
                    continue;

                const uint8_t vel = (uint8_t) juce::jlimit(1, 127,
                    (int) (sourceHeldVelocity[(size_t) ch0][(size_t) nt] > 0
                        ? sourceHeldVelocity[(size_t) ch0][(size_t) nt]
                        : 100));

                setDesiredNoteForCurrentBuild(branch, ch0, nt, vel, (uint8_t) outChannel);
            }
        }
    }
}

void SplitterProcessor::applyDesiredDiff(const Config& cfg, juce::MidiBuffer& out, int samplePos)
{
    // Reconcile desired routing with currently sounding outputs.
    // Order is intentional:
    // 1) release old notes first,
    // 2) start new notes next.
    buildDesiredState(cfg);
    ensureOutputKeyCache();

    for (int branch = 0; branch < kNumBranches; ++branch)
    {
        auto& activeKeys = outputKeys[(size_t) branch];
        const int activeKeyCount = outputKeyCount[(size_t) branch];
        auto& nextKeys = outputKeysScratch[(size_t) branch];
        int nextCount = 0;

        for (int i = 0; i < activeKeyCount; ++i)
        {
            const int key = activeKeys[(size_t) i];
            const int ch0 = channelFromKey(key);
            const int nt = noteFromKey(key);

            const int curChannel = (int) outputChannel[(size_t) branch][(size_t) ch0][(size_t) nt];
            const int curVelocity = (int) outputVelocity[(size_t) branch][(size_t) ch0][(size_t) nt];

            const bool desActive = isDesiredActiveForCurrentBuild(branch, ch0, nt);

            if (!desActive)
            {
                if (curChannel >= 1 && curChannel <= 16)
                    out.addEvent(juce::MidiMessage::noteOff(curChannel, nt), samplePos);

                outputActive[(size_t) branch][(size_t) ch0][(size_t) nt] = 0;
                outputChannel[(size_t) branch][(size_t) ch0][(size_t) nt] = 0;
                outputVelocity[(size_t) branch][(size_t) ch0][(size_t) nt] = 0;
                continue;
            }

            const int desChannel = (int) desiredChannel[(size_t) branch][(size_t) ch0][(size_t) nt];
            const int desVelocity = (int) desiredVelocity[(size_t) branch][(size_t) ch0][(size_t) nt];
            const bool changedWhileActive = (curChannel != desChannel || curVelocity != desVelocity);

            if (changedWhileActive)
            {
                if (curChannel >= 1 && curChannel <= 16)
                    out.addEvent(juce::MidiMessage::noteOff(curChannel, nt), samplePos);

                if (desChannel >= 1 && desChannel <= 16)
                {
                    out.addEvent(juce::MidiMessage::noteOn(desChannel,
                                                           nt,
                                                           (juce::uint8) juce::jlimit(1, 127, desVelocity)),
                                 samplePos);
                }
            }

            outputActive[(size_t) branch][(size_t) ch0][(size_t) nt] = 1;
            outputChannel[(size_t) branch][(size_t) ch0][(size_t) nt] = (uint8_t) juce::jlimit(1, 16, desChannel);
            outputVelocity[(size_t) branch][(size_t) ch0][(size_t) nt] = (uint8_t) juce::jlimit(1, 127, desVelocity);

            if (nextCount < kNumKeys)
                nextKeys[(size_t) nextCount++] = key;
        }

        const auto& branchDesiredKeys = desiredKeys[(size_t) branch];
        const int branchDesiredCount = desiredKeyCount[(size_t) branch];

        for (int i = 0; i < branchDesiredCount; ++i)
        {
            const int key = branchDesiredKeys[(size_t) i];
            const int ch0 = channelFromKey(key);
            const int nt = noteFromKey(key);

            if (outputActive[(size_t) branch][(size_t) ch0][(size_t) nt] != 0)
                continue;

            const int desChannel = (int) desiredChannel[(size_t) branch][(size_t) ch0][(size_t) nt];
            const int desVelocity = (int) desiredVelocity[(size_t) branch][(size_t) ch0][(size_t) nt];

            if (desChannel >= 1 && desChannel <= 16)
            {
                out.addEvent(juce::MidiMessage::noteOn(desChannel,
                                                       nt,
                                                       (juce::uint8) juce::jlimit(1, 127, desVelocity)),
                             samplePos);
            }

            outputActive[(size_t) branch][(size_t) ch0][(size_t) nt] = 1;
            outputChannel[(size_t) branch][(size_t) ch0][(size_t) nt] = (uint8_t) juce::jlimit(1, 16, desChannel);
            outputVelocity[(size_t) branch][(size_t) ch0][(size_t) nt] = (uint8_t) juce::jlimit(1, 127, desVelocity);

            if (nextCount < kNumKeys)
                nextKeys[(size_t) nextCount++] = key;
        }

        outputKeyCount[(size_t) branch] = nextCount;
        if (nextCount > 0)
            std::copy_n(nextKeys.begin(), (size_t) nextCount, activeKeys.begin());
    }

    outputKeyCacheValid = true;
}

void SplitterProcessor::process(juce::MidiBuffer& midi)
{
    // Block flow:
    // - consume source note events into held state,
    // - keep non-note events passthrough,
    // - emit only routing diffs required at each event boundary.
    outScratch.clear();

    const Config cfg = readConfigRT();
    const bool wasEnabled = (hasLastConfig && lastConfig.enabled);

    // Fast bypass path:
    // - When splitter is disabled, route is identity (pass-through).
    // - We still track source held notes so re-enabling can rebuild correctly.
    // - Heavy desired/output matrix diff is skipped for per-event updates.
    if (!cfg.enabled)
    {
        // Enabled -> disabled edge:
        // keep continuity by reconciling previously sounding routed notes once.
        if (wasEnabled)
            applyDesiredDiff(cfg, outScratch, 0);

        for (auto it = midi.begin(); it != midi.end(); ++it)
        {
            const auto meta = *it;
            const auto& msg = meta.getMessage();
            const int samplePos = meta.samplePosition;

            if (isNoteOnMessage(msg))
            {
                const int ch = juce::jlimit(1, 16, msg.getChannel());
                const int nt = juce::jlimit(0, 127, msg.getNoteNumber());
                const uint8_t vel = velocityFloatToMidi127(msg.getVelocity());

                handleSourceNoteOn(cfg, ch, nt, vel);

                const int ch0 = ch - 1;
                outputActive[0][(size_t) ch0][(size_t) nt] = 1;
                outputVelocity[0][(size_t) ch0][(size_t) nt] = vel;
                outputChannel[0][(size_t) ch0][(size_t) nt] = (uint8_t) ch;
                outputKeyCacheValid = false;
            }
            else if (isNoteOffMessage(msg))
            {
                const int ch = juce::jlimit(1, 16, msg.getChannel());
                const int nt = juce::jlimit(0, 127, msg.getNoteNumber());
                const int ch0 = ch - 1;

                if (handleSourceNoteOff(ch, nt))
                {
                    outputActive[0][(size_t) ch0][(size_t) nt] = 0;
                    outputVelocity[0][(size_t) ch0][(size_t) nt] = 0;
                    outputChannel[0][(size_t) ch0][(size_t) nt] = 0;
                    outputKeyCacheValid = false;
                }
                else if (outputActive[0][(size_t) ch0][(size_t) nt] != 0)
                {
                    // Defensive stale-state cleanup in disabled mode:
                    // passthrough NOTE OFF still goes out, but internal output
                    // tracking must also drop the orphaned key.
                    outputActive[0][(size_t) ch0][(size_t) nt] = 0;
                    outputVelocity[0][(size_t) ch0][(size_t) nt] = 0;
                    outputChannel[0][(size_t) ch0][(size_t) nt] = 0;
                    outputKeyCacheValid = false;
                }
            }

            // Disabled splitter contract: pass all events through unchanged.
            outScratch.addEvent(msg, samplePos);
        }

        lastConfig = cfg;
        hasLastConfig = true;
        midi.swapWith(outScratch);
        return;
    }

    // Parameter edge (mode/active/channels/ranges/voice strategy) while notes are held:
    // recompute and emit only the required NoteOff/NoteOn diff at block start.
    if (!hasLastConfig || cfg != lastConfig)
    {
        applyDesiredDiff(cfg, outScratch, 0);
        lastConfig = cfg;
        hasLastConfig = true;
    }

    int pendingDiffSample = -1;
    auto flushPendingDiff = [&]() noexcept
    {
        if (pendingDiffSample < 0)
            return;

        applyDesiredDiff(cfg, outScratch, pendingDiffSample);
        pendingDiffSample = -1;
    };

    for (auto it = midi.begin(); it != midi.end(); ++it)
    {
        const auto meta = *it;
        const auto& msg = meta.getMessage();
        const int samplePos = meta.samplePosition;

        if (isNoteOnMessage(msg))
        {
            const int ch = juce::jlimit(1, 16, msg.getChannel());
            const int nt = juce::jlimit(0, 127, msg.getNoteNumber());
            const uint8_t vel = velocityFloatToMidi127(msg.getVelocity());

            if (pendingDiffSample >= 0 && pendingDiffSample != samplePos)
                flushPendingDiff();

            handleSourceNoteOn(cfg, ch, nt, vel);
            pendingDiffSample = samplePos;
            continue;
        }

        if (isNoteOffMessage(msg))
        {
            const int ch = juce::jlimit(1, 16, msg.getChannel());
            const int nt = juce::jlimit(0, 127, msg.getNoteNumber());
            const int ch0 = ch - 1;

            if (pendingDiffSample >= 0 && pendingDiffSample != samplePos)
                flushPendingDiff();

            if (handleSourceNoteOff(ch, nt))
            {
                pendingDiffSample = samplePos;
            }
            else
            {
                // Defensive orphan NOTE OFF handling:
                // if source state missed this key but a routed output voice is
                // still active for (channel,note), force-release it now.
                bool releasedAny = false;
                for (int branch = 0; branch < kNumBranches; ++branch)
                {
                    if (outputActive[(size_t) branch][(size_t) ch0][(size_t) nt] == 0)
                        continue;

                    const int outCh = juce::jlimit(1,
                                                   16,
                                                   (int) outputChannel[(size_t) branch][(size_t) ch0][(size_t) nt]);
                    outScratch.addEvent(juce::MidiMessage::noteOff(outCh, nt), samplePos);

                    outputActive[(size_t) branch][(size_t) ch0][(size_t) nt] = 0;
                    outputVelocity[(size_t) branch][(size_t) ch0][(size_t) nt] = 0;
                    outputChannel[(size_t) branch][(size_t) ch0][(size_t) nt] = 0;
                    releasedAny = true;
                }

                if (releasedAny)
                {
                    outputKeyCacheValid = false;
                    pendingDiffSample = samplePos;
                }
            }

            continue;
        }

        // Preserve ordering around sample boundaries: flush note diffs before
        // forwarding non-note messages at this position.
        flushPendingDiff();

        // Non-note messages are not split; pass through as-is.
        outScratch.addEvent(msg, samplePos);
    }

    flushPendingDiff();

    midi.swapWith(outScratch);
}
