//==============================================================================
// PluginProcessor.cpp
//------------------------------------------------------------------------------
// MidivisiVici - Implementation du processor principal (MIDI Effect)
//
// Objectifs de cette implementation:
//   1) Garder un routage lane coherent avec la topologie consume/direct.
//   2) Ajouter un dedup NOTE deterministe et RT-safe au point de convergence:
//
//        (merge sorties lanes) -> DEDUP -> sortie host
//
// Pourquoi le dedup:
//   - Quand des lanes convergent apres Splitter, des NOTE identiques peuvent
//     etre emises plusieurs fois (meme channel/note/on-off au meme sample).
//   - La sortie host ne doit pas contenir ces doublons.
//
// Politique dedup (deterministe, RT-safe):
//   - Concerne uniquement les NOTE events:
//       NoteOn vel>0, NoteOff, NoteOn vel==0 traite comme NoteOff.
//   - Cle = (samplePos, channel, noteNumber, isOn, velocity pour NoteOn).
//   - Garder la premiere occurrence de la cle dans le bloc.
//   - Supprimer les occurrences suivantes.
//   - Les messages non-note sont toujours preserves.
//   - Capacite limitee: si depassee, fail-open (conserver le reste).
//
// Note JUCE (evitement warning compilateur):
//   - Eviter range-for direct sur juce::MidiBuffer avec copie temporaire.
//   - Preferer iterateurs explicites.
//==============================================================================

#include "PluginProcessor.h"
#include "PluginEditor.h"

#include "PluginParameters.h"
#include "DebugConfig.h"
#include "BuildTimestamp.h"

#include <cmath>       // std::abs
#include <limits>      // std::numeric_limits
#include <memory>      // std::unique_ptr
#include <type_traits> // std::is_floating_point_v
#include <utility>     // std::move

//==============================================================================
// Petit helper pour eviter les pieges de concat String.
//==============================================================================

#if LOGS_ENABLED
static inline juce::String laneTag(Lanes::Lane lane)
{
    return juce::String(Lanes::laneSuffix(lane));
}
#endif

//==============================================================================
// Helpers consume + dedup (RT-safe, pas d alloc heap)
//==============================================================================

namespace
{
    //--------------------------------------------------------------------------
    // NOTE KEY
    //--------------------------------------------------------------------------
    // Identite minimale d un NOTE event dans un bloc, utilisee par:
    //   - routage consume (retirer les notes deja consommees)
    //   - dedup final (supprimer doublons au merge)
    //--------------------------------------------------------------------------
    struct NoteKey
    {
        int  samplePos = 0;
        int  channel   = 1;   // 1..16
        int  note      = 0;   // 0..127
        bool isOn      = false;
        int  velocity  = 0;   // 0 for NoteOff, 1..127 for NoteOn
    };

    struct NoteOnInject
    {
        int samplePos = 0;
        int channel   = 1;   // 1..16
        int note      = 0;   // 0..127
        int velocity  = 100; // 1..127
    };

    //--------------------------------------------------------------------------
    // Detection NOTE
    //--------------------------------------------------------------------------
    // JUCE peut coder un NoteOn velocity=0. On le traite comme NoteOff.
    // Retourne true si message note-related et remplit isOnOut.
    //--------------------------------------------------------------------------
    static inline bool isNoteMsg(const juce::MidiMessage& m, bool& isOnOut) noexcept
    {
        if (m.isNoteOn() && m.getVelocity() > 0.0f)
        {
            isOnOut = true;
            return true;
        }

        if (m.isNoteOff() || (m.isNoteOn() && m.getVelocity() == 0.0f))
        {
            isOnOut = false;
            return true;
        }

        return false;
    }

    static inline bool matchKey(const NoteKey& k,
                                int samplePos, int ch, int note, bool isOn) noexcept
    {
        return (k.samplePos == samplePos &&
                k.channel   == ch &&
                k.note      == note &&
                k.isOn      == isOn);
    }

    static inline bool matchKeyWithVelocity(const NoteKey& a,
                                            const NoteKey& b) noexcept
    {
        if (a.samplePos != b.samplePos
            || a.channel != b.channel
            || a.note != b.note
            || a.isOn != b.isOn)
            return false;

        if (!a.isOn)
            return true;

        return a.velocity == b.velocity;
    }

    // JUCE API compatibility:
    // - some versions expose getVelocity() as uint8 [0..127]
    // - older versions expose getVelocity() as float [0..1]
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

        // Saturated stack: drop oldest to keep most-recent history.
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

    template <size_t N>
    static bool appendUniqueNoteKey(const NoteKey& key,
                                    std::array<NoteKey, N>& keys,
                                    int& count) noexcept
    {
        for (int i = 0; i < count; ++i)
        {
            if (matchKey(keys[(size_t) i], key.samplePos, key.channel, key.note, key.isOn))
                return true;
        }

        if (count >= (int) N)
            return false;

        keys[(size_t) count++] = key;
        return true;
    }

    template <size_t N>
    static inline bool containsNoteKey(const std::array<NoteKey, N>& keys,
                                       int count,
                                       const NoteKey& key) noexcept
    {
        for (int i = 0; i < count; ++i)
        {
            if (matchKey(keys[(size_t) i], key.samplePos, key.channel, key.note, key.isOn))
                return true;
        }

        return false;
    }

    // Build a unique NOTE-key index for a MidiBuffer.
    // overflow=true means the index is truncated and callers must fallback to
    // direct buffer scans to preserve exact behavior.
    template <size_t N>
    static int collectUniqueNoteKeys(const juce::MidiBuffer& in,
                                     std::array<NoteKey, N>& outKeys,
                                     bool& overflow) noexcept
    {
        int count = 0;
        overflow = false;

        for (auto it = in.begin(); it != in.end(); ++it)
        {
            const auto meta = *it;
            const auto& msg = meta.getMessage();

            bool isOn = false;
            if (!isNoteMsg(msg, isOn))
                continue;

            const NoteKey key
            {
                meta.samplePosition,
                msg.getChannel(),
                msg.getNoteNumber(),
                isOn
            };

            if (containsNoteKey(outKeys, count, key))
                continue;

            if (count >= (int) N)
            {
                overflow = true;
                continue;
            }

            outKeys[(size_t) count++] = key;
        }

        return count;
    }

    template <size_t N>
    static int collectNoteKeyMultiplicity(const juce::MidiBuffer& in,
                                          std::array<NoteKey, N>& outKeys,
                                          std::array<uint16_t, N>& outCounts,
                                          bool& overflow) noexcept
    {
        int count = 0;
        overflow = false;
        outCounts.fill(0);

        for (auto it = in.begin(); it != in.end(); ++it)
        {
            const auto meta = *it;
            const auto& msg = meta.getMessage();

            bool isOn = false;
            if (!isNoteMsg(msg, isOn))
                continue;

            const NoteKey key
            {
                meta.samplePosition,
                msg.getChannel(),
                msg.getNoteNumber(),
                isOn,
                isOn ? velocityFloatToMidi127(msg.getVelocity()) : 0
            };

            int existing = -1;
            for (int i = 0; i < count; ++i)
            {
                if (matchKeyWithVelocity(outKeys[(size_t) i], key))
                {
                    existing = i;
                    break;
                }
            }

            if (existing >= 0)
            {
                auto& c = outCounts[(size_t) existing];
                c = (uint16_t) juce::jmin(65535, (int) c + 1);
                continue;
            }

            if (count >= (int) N)
            {
                overflow = true;
                continue;
            }

            outKeys[(size_t) count] = key;
            outCounts[(size_t) count] = 1;
            ++count;
        }

        return count;
    }

    template <size_t N>
    static int findNoteKeyWithVelocityIndex(const std::array<NoteKey, N>& keys,
                                            int count,
                                            const NoteKey& key) noexcept
    {
        for (int i = 0; i < count; ++i)
        {
            if (matchKeyWithVelocity(keys[(size_t) i], key))
                return i;
        }

        return -1;
    }

    static bool bufferHasNoteKey(const juce::MidiBuffer& buffer,
                                 const NoteKey& key) noexcept
    {
        for (auto it = buffer.begin(); it != buffer.end(); ++it)
        {
            const auto meta = *it;
            const auto& msg = meta.getMessage();

            bool isOn = false;
            if (!isNoteMsg(msg, isOn))
                continue;

            if (matchKey(key, meta.samplePosition, msg.getChannel(), msg.getNoteNumber(), isOn))
                return true;
        }

        return false;
    }

    static bool bufferHasPolyAftertouch(const juce::MidiBuffer& buffer,
                                        int samplePos,
                                        int channel,
                                        int note) noexcept
    {
        for (auto it = buffer.begin(); it != buffer.end(); ++it)
        {
            const auto meta = *it;
            const auto& msg = meta.getMessage();

            if (!msg.isAftertouch())
                continue;

            if (meta.samplePosition == samplePos
                && msg.getChannel() == channel
                && msg.getNoteNumber() == note)
            {
                return true;
            }
        }

        return false;
    }

    static bool hasAnyHeldRawNote(const std::array<std::array<int, 128>, 16>& heldCounts) noexcept
    {
        for (const auto& byNote : heldCounts)
            for (const int c : byNote)
                if (c > 0)
                    return true;

        return false;
    }

    // Returns true if the current incoming block contains at least one
    // NOTE ON event (velocity > 0).
    static bool hasAnyRawNoteOnInIncoming(const juce::MidiBuffer& incoming) noexcept
    {
        for (auto it = incoming.begin(); it != incoming.end(); ++it)
        {
            const auto meta = *it;
            const auto& msg = meta.getMessage();

            bool isOn = false;
            if (!isNoteMsg(msg, isOn))
                continue;
            if (isOn)
                return true;
        }

        return false;
    }

    static constexpr std::array<std::array<int, 7>, 10> kModeDegreePatterns
    {{
        {{ 0, 2, 4, 5, 7, 9, 11 }}, // Major / Ionian
        {{ 0, 2, 3, 5, 7, 8, 10 }}, // Minor / Aeolian
        {{ 0, 2, 3, 5, 7, 9, 10 }}, // Dorian
        {{ 0, 1, 3, 5, 7, 8, 10 }}, // Phrygian
        {{ 0, 2, 4, 6, 7, 9, 11 }}, // Lydian
        {{ 0, 2, 4, 5, 7, 9, 10 }}, // Mixolydian
        {{ 0, 1, 3, 5, 6, 8, 10 }}, // Locrian
        {{ 0, 2, 3, 5, 7, 8, 11 }}, // Harmonic minor
        {{ 0, 2, 3, 5, 7, 9, 11 }}, // Melodic minor
        {{ 0, 1, 2, 3, 4, 5, 6 }}   // Chromatic fallback (degree-order)
    }};

    static inline int whiteDegreeIndexForPitchClass(int pitchClass) noexcept
    {
        switch (pitchClass % 12)
        {
            case 0:  return 0; // C
            case 2:  return 1; // D
            case 4:  return 2; // E
            case 5:  return 3; // F
            case 7:  return 4; // G
            case 9:  return 5; // A
            case 11: return 6; // B
            default: break;
        }

        return -1;
    }

    static inline bool isBlackPitchClass(int pitchClass) noexcept
    {
        switch (pitchClass % 12)
        {
            case 1:  // C#
            case 3:  // D#
            case 6:  // F#
            case 8:  // G#
            case 10: // A#
                return true;
            default:
                break;
        }

        return false;
    }

    static inline int blackKeySourceIndexForPitchClass(int pitchClass) noexcept
    {
        switch (pitchClass % 12)
        {
            case 1:  return 0; // C#
            case 3:  return 1; // D#
            case 6:  return 2; // F#
            case 8:  return 3; // G#
            case 10: return 4; // A#
            default: break;
        }

        return -1;
    }

    static inline uint16_t buildScaleMaskFromMode(int tonicPc, int modeIndex) noexcept
    {
        const int wrappedTonic = ((tonicPc % 12) + 12) % 12;
        const auto& pattern = kModeDegreePatterns[(size_t) juce::jlimit(0, 9, modeIndex)];

        uint16_t mask = 0;
        for (int d : pattern)
        {
            const int pc = (wrappedTonic + d) % 12;
            mask = (uint16_t) (mask | (uint16_t) (1u << pc));
        }

        return mask;
    }

    enum class BlackChordQuality : uint8_t
    {
        Unknown = 0,
        Maj,
        Min,
        Dim,
        Aug,
        Power5,
        Sus2,
        Sus4,
        Dom7,
        Maj7,
        Min7,
        MinMaj7,
        Min7b5,
        Dim7
    };

    struct BlackChordInfo
    {
        bool valid = false;
        int rootPc = 0;
        BlackChordQuality quality = BlackChordQuality::Unknown;
    };

    static inline bool hasIntervalBit(uint16_t intervalMask, int semitone) noexcept
    {
        const int wrapped = ((semitone % 12) + 12) % 12;
        return (intervalMask & (uint16_t) (1u << wrapped)) != 0;
    }

    static uint16_t buildIntervalsFromRootMask(uint16_t pitchClassMask, int rootPc) noexcept
    {
        uint16_t iv = 0;
        for (int pc = 0; pc < 12; ++pc)
        {
            if ((pitchClassMask & (uint16_t) (1u << pc)) == 0)
                continue;

            const int rel = (pc - rootPc + 12) % 12;
            iv = (uint16_t) (iv | (uint16_t) (1u << rel));
        }
        return iv;
    }

    static BlackChordInfo detectBlackChordInfo(uint16_t pitchClassMask, int bassPc) noexcept
    {
        BlackChordInfo best {};
        int bestScore = std::numeric_limits<int>::min();

        auto tryCandidate = [&](int rootPc, BlackChordQuality q, int baseScore)
        {
            int score = baseScore;
            if (bassPc >= 0)
            {
                if (rootPc == bassPc)
                    score += 10;
                else
                    score += 1;
            }

            if (score > bestScore
                || (score == bestScore && rootPc == bassPc && best.rootPc != bassPc)
                || (score == bestScore && rootPc < best.rootPc))
            {
                bestScore = score;
                best.valid = true;
                best.rootPc = rootPc;
                best.quality = q;
            }
        };

        for (int rootPc = 0; rootPc < 12; ++rootPc)
        {
            if ((pitchClassMask & (uint16_t) (1u << rootPc)) == 0)
                continue;

            const uint16_t iv = buildIntervalsFromRootMask(pitchClassMask, rootPc);
            const bool has2 = hasIntervalBit(iv, 2);
            const bool has3 = hasIntervalBit(iv, 3);
            const bool has4 = hasIntervalBit(iv, 4);
            const bool has5 = hasIntervalBit(iv, 5);
            const bool has6 = hasIntervalBit(iv, 6);
            const bool has7 = hasIntervalBit(iv, 7);
            const bool has8 = hasIntervalBit(iv, 8);
            const bool has9 = hasIntervalBit(iv, 9);
            const bool has10 = hasIntervalBit(iv, 10);
            const bool has11 = hasIntervalBit(iv, 11);

            if (has4 && has7)
            {
                if (has11) tryCandidate(rootPc, BlackChordQuality::Maj7, 120);
                if (has10) tryCandidate(rootPc, BlackChordQuality::Dom7, 118);
                if (has8)  tryCandidate(rootPc, BlackChordQuality::Aug, 106);
                tryCandidate(rootPc, BlackChordQuality::Maj, 104);
                continue;
            }

            if (has3 && has7)
            {
                if (has10) tryCandidate(rootPc, BlackChordQuality::Min7, 116);
                if (has11) tryCandidate(rootPc, BlackChordQuality::MinMaj7, 114);
                tryCandidate(rootPc, BlackChordQuality::Min, 102);
                continue;
            }

            if (has3 && has6)
            {
                if (has9)  tryCandidate(rootPc, BlackChordQuality::Dim7, 112);
                if (has10) tryCandidate(rootPc, BlackChordQuality::Min7b5, 110);
                tryCandidate(rootPc, BlackChordQuality::Dim, 101);
                continue;
            }

            if (has2 && has7 && !has3 && !has4)
            {
                tryCandidate(rootPc, BlackChordQuality::Sus2, 96);
                continue;
            }

            if (has5 && has7 && !has3 && !has4)
            {
                tryCandidate(rootPc, BlackChordQuality::Sus4, 96);
                continue;
            }

            if (has7 && !has2 && !has3 && !has4 && !has5)
            {
                tryCandidate(rootPc, BlackChordQuality::Power5, 90);
                continue;
            }

            // Loose fallback for incomplete voicings.
            if (has4 && !has3)
                tryCandidate(rootPc, BlackChordQuality::Maj, 70);
            else if (has3 && !has4)
                tryCandidate(rootPc, BlackChordQuality::Min, 70);
            else
                tryCandidate(rootPc, BlackChordQuality::Unknown, 50);
        }

        if (!best.valid)
        {
            best.valid = true;
            best.rootPc = (bassPc >= 0 ? bassPc : 0);
            best.quality = BlackChordQuality::Unknown;
        }

        return best;
    }

    static inline int getModeColorDegree(int modeIndex) noexcept
    {
        switch (juce::jlimit(0, 9, modeIndex))
        {
            case 0:  return -1; // Ionian
            case 1:  return 8;  // Aeolian b6
            case 2:  return 9;  // Dorian 6
            case 3:  return 1;  // Phrygian b2
            case 4:  return 6;  // Lydian #4
            case 5:  return 10; // Mixolydian b7
            case 6:  return 6;  // Locrian b5
            case 7:  return 11; // Harmonic minor maj7 color
            case 8:  return 9;  // Melodic minor 6
            case 9:  return -1; // Chromatic
            default: break;
        }
        return -1;
    }

    static inline bool appendUniquePc(std::array<int, 5>& out, int& count, int pc) noexcept
    {
        const int wrapped = ((pc % 12) + 12) % 12;
        for (int i = 0; i < count; ++i)
            if (out[(size_t) i] == wrapped)
                return false;

        if (count >= 5)
            return false;

        out[(size_t) count++] = wrapped;
        return true;
    }

    static inline bool inScaleMask(uint16_t scaleMask, int pc) noexcept
    {
        const int wrapped = ((pc % 12) + 12) % 12;
        return (scaleMask & (uint16_t) (1u << wrapped)) != 0;
    }

    static std::array<int, 6> stableExtensionPriority(BlackChordQuality quality, int modeIndex) noexcept
    {
        using Q = BlackChordQuality;
        const int m = juce::jlimit(0, 9, modeIndex);

        if (quality == Q::Maj || quality == Q::Maj7)
        {
            if (m == 4) return { 6, 2, 9, 5, 10, 1 }; // Lydian
            if (m == 5) return { 10, 2, 9, 5, 6, 1 }; // Mixolydian
            return { 2, 9, 5, 6, 10, 1 };
        }

        if (quality == Q::Dom7)
            return { 2, 5, 9, 1, 8, 6 };

        if (quality == Q::Min || quality == Q::Min7 || quality == Q::MinMaj7)
        {
            if (m == 2) return { 9, 2, 5, 10, 8, 1 }; // Dorian
            if (m == 3) return { 1, 5, 10, 8, 2, 9 }; // Phrygian
            if (m == 1) return { 10, 5, 8, 2, 9, 1 }; // Aeolian
            return { 2, 5, 10, 9, 8, 1 };
        }

        if (quality == Q::Min7b5 || quality == Q::Dim || quality == Q::Dim7)
            return { 1, 5, 8, 2, 9, 10 };

        if (quality == Q::Sus2 || quality == Q::Sus4)
            return { 2, 5, 9, 10, 4, 6 };

        return { 2, 5, 9, 10, 1, 8 };
    }

    static inline bool isMajorFamily(BlackChordQuality q) noexcept
    {
        using Q = BlackChordQuality;
        return q == Q::Maj || q == Q::Maj7 || q == Q::Dom7 || q == Q::Aug;
    }

    static inline bool isMinorFamily(BlackChordQuality q) noexcept
    {
        using Q = BlackChordQuality;
        return q == Q::Min || q == Q::Min7 || q == Q::MinMaj7
            || q == Q::Min7b5 || q == Q::Dim || q == Q::Dim7;
    }

    static std::array<int, 5> buildModalFallbackPenta(int tonicPc, int modeIndex) noexcept
    {
        // Deterministic modal pentatonic fallbacks.
        static constexpr std::array<std::array<int, 5>, 10> kModalPentaDegrees
        {{
            {{ 0, 2, 4, 7, 9 }},   // Ionian
            {{ 0, 3, 5, 7, 10 }},  // Aeolian
            {{ 0, 3, 5, 7, 9 }},   // Dorian
            {{ 0, 1, 3, 7, 10 }},  // Phrygian
            {{ 0, 2, 4, 6, 9 }},   // Lydian
            {{ 0, 2, 4, 7, 10 }},  // Mixolydian
            {{ 0, 1, 3, 6, 10 }},  // Locrian
            {{ 0, 3, 5, 7, 11 }},  // Harmonic minor
            {{ 0, 2, 3, 7, 9 }},   // Melodic minor
            {{ 0, 2, 4, 7, 9 }}    // Chromatic fallback
        }};

        std::array<int, 5> out {};
        const auto& rel = kModalPentaDegrees[(size_t) juce::jlimit(0, 9, modeIndex)];
        for (int i = 0; i < 5; ++i)
            out[(size_t) i] = ((tonicPc + rel[(size_t) i]) % 12 + 12) % 12;
        return out;
    }

    static std::array<int, 5> sortFiveSetForBlackMapping(const std::array<int, 5>& set,
                                                          int anchorPc) noexcept
    {
        std::array<std::pair<int, int>, 5> rel {};
        for (int i = 0; i < 5; ++i)
        {
            const int pc = ((set[(size_t) i] % 12) + 12) % 12;
            const int r = (pc - anchorPc + 12) % 12;
            rel[(size_t) i] = { r, pc };
        }

        std::sort(rel.begin(), rel.end(), [](const auto& a, const auto& b)
        {
            if (a.first != b.first)
                return a.first < b.first;
            return a.second < b.second;
        });

        std::array<int, 5> out {};
        for (int i = 0; i < 5; ++i)
            out[(size_t) i] = rel[(size_t) i].second;
        return out;
    }

    static std::array<int, 5> buildBlackFiveClassSetDeterministic(uint16_t whiteMask,
                                                                   int whiteBassPc,
                                                                   int tonicPc,
                                                                   int modeIndex) noexcept
    {
        const int wrappedTonic = ((tonicPc % 12) + 12) % 12;
        const uint16_t scaleMask = buildScaleMaskFromMode(wrappedTonic, modeIndex);

        if ((whiteMask & 0x0FFFu) == 0)
            return buildModalFallbackPenta(wrappedTonic, modeIndex);

        const BlackChordInfo chord = detectBlackChordInfo(whiteMask & 0x0FFFu, whiteBassPc);
        const int root = ((chord.rootPc % 12) + 12) % 12;

        // Level 1: chord-driven + modal color + extension fill.
        std::array<int, 5> selected {};
        int selectedCount = 0;

        auto appendIfInScale = [&](int relDeg)
        {
            const int pc = ((root + relDeg) % 12 + 12) % 12;
            if (inScaleMask(scaleMask, pc))
                appendUniquePc(selected, selectedCount, pc);
        };

        using Q = BlackChordQuality;
        switch (chord.quality)
        {
            case Q::Maj:      appendIfInScale(0); appendIfInScale(4); appendIfInScale(7); break;
            case Q::Maj7:     appendIfInScale(0); appendIfInScale(4); appendIfInScale(7); appendIfInScale(11); break;
            case Q::Dom7:     appendIfInScale(0); appendIfInScale(4); appendIfInScale(7); appendIfInScale(10); break;
            case Q::Min:      appendIfInScale(0); appendIfInScale(3); appendIfInScale(7); break;
            case Q::Min7:     appendIfInScale(0); appendIfInScale(3); appendIfInScale(7); appendIfInScale(10); break;
            case Q::MinMaj7:  appendIfInScale(0); appendIfInScale(3); appendIfInScale(7); appendIfInScale(11); break;
            case Q::Min7b5:   appendIfInScale(0); appendIfInScale(3); appendIfInScale(6); appendIfInScale(10); break;
            case Q::Dim:      appendIfInScale(0); appendIfInScale(3); appendIfInScale(6); break;
            case Q::Dim7:     appendIfInScale(0); appendIfInScale(3); appendIfInScale(6); appendIfInScale(9); break;
            case Q::Aug:      appendIfInScale(0); appendIfInScale(4); appendIfInScale(8); break;
            case Q::Sus2:     appendIfInScale(0); appendIfInScale(2); appendIfInScale(7); break;
            case Q::Sus4:     appendIfInScale(0); appendIfInScale(5); appendIfInScale(7); break;
            case Q::Power5:   appendIfInScale(0); appendIfInScale(7); break;
            case Q::Unknown:  appendIfInScale(0); break;
        }

        const int colorDeg = getModeColorDegree(modeIndex);
        if (colorDeg >= 0)
            appendIfInScale(colorDeg);

        const auto extPriority = stableExtensionPriority(chord.quality, modeIndex);
        for (int rel : extPriority)
        {
            if (selectedCount >= 5)
                break;
            appendIfInScale(rel);
        }

        if (selectedCount == 5)
            return sortFiveSetForBlackMapping(selected, root);

        // Level 2: chord pentatonic fallback (only if full 5 notes in scale).
        std::array<int, 5> candidate {};
        bool chordPentaOk = true;
        const std::array<int, 5> majorPenta { 0, 2, 4, 7, 9 };
        const std::array<int, 5> minorPenta { 0, 3, 5, 7, 10 };
        const auto& relPenta = isMajorFamily(chord.quality)
            ? majorPenta
            : (isMinorFamily(chord.quality) ? minorPenta : majorPenta);

        for (int i = 0; i < 5; ++i)
        {
            const int pc = ((root + relPenta[(size_t) i]) % 12 + 12) % 12;
            if (!inScaleMask(scaleMask, pc))
            {
                chordPentaOk = false;
                break;
            }
            candidate[(size_t) i] = pc;
        }

        if (chordPentaOk)
            return sortFiveSetForBlackMapping(candidate, root);

        // Level 3: modal key fallback.
        return sortFiveSetForBlackMapping(buildModalFallbackPenta(wrappedTonic, modeIndex),
                                          wrappedTonic);
    }

    static inline uint32_t makeWhiteChordSignature(uint16_t pitchClassMask,
                                                   int bassPc,
                                                   int tonicPc,
                                                   int modeIndex) noexcept
    {
        const uint32_t mask12 = (uint32_t) (pitchClassMask & 0x0FFFu);
        const uint32_t bass4 = (uint32_t) (bassPc >= 0 ? (bassPc & 0x0F) : 0x0F);
        const uint32_t tonic4 = (uint32_t) (((tonicPc % 12) + 12) % 12);
        const uint32_t mode4 = (uint32_t) juce::jlimit(0, 9, modeIndex);

        return mask12 | (bass4 << 12) | (tonic4 << 16) | (mode4 << 20);
    }

    static inline int computeMappedBlackOutputNote(int inputNote,
                                                   const std::array<int, 5>& set) noexcept
    {
        const int clampedInput = juce::jlimit(0, 127, inputNote);
        const int inputPc = clampedInput % 12;
        const int idx = blackKeySourceIndexForPitchClass(inputPc);
        if (idx < 0)
            return clampedInput;

        // Build an ascending 5-note class list from set[0], preserving octave
        // carry across wrapped pitch classes (ex: 7,9,11,2,4 -> 7,9,11,14,16).
        std::array<int, 5> ascendingTargets {};
        ascendingTargets[0] = ((set[0] % 12) + 12) % 12;
        for (int i = 1; i < 5; ++i)
        {
            int v = ((set[(size_t) i] % 12) + 12) % 12;
            while (v < ascendingTargets[(size_t) (i - 1)])
                v += 12;
            ascendingTargets[(size_t) i] = v;
        }

        const int octaveBase = clampedInput - inputPc;
        const int mapped = octaveBase + ascendingTargets[(size_t) idx];
        return juce::jlimit(0, 127, mapped);
    }

    static void remapInputFromWhiteKeyboardMode(const juce::MidiBuffer& in,
                                                juce::MidiBuffer& out,
                                                juce::MidiBuffer& whiteOriginOut,
                                                juce::MidiBuffer& blackOriginOut,
                                                bool blackModeEnabled,
                                                int tonicPc,
                                                int modeIndex,
                                                std::array<std::array<int, 128>, 16>& whiteHeldCounts,
                                                std::array<std::array<std::array<uint8_t, 16>, 128>, 16>& whiteMappedNoteStack,
                                                std::array<std::array<std::array<uint8_t, 16>, 128>, 16>& whiteMappedVelocityStack,
                                                std::array<std::array<uint8_t, 128>, 16>& whiteMappedNoteStackSize,
                                                std::array<std::array<uint8_t, 128>, 16>& whitePhysicalHeldCount,
                                                std::array<std::array<uint8_t, 128>, 16>& whitePhysicalLastVelocity,
                                                uint32_t& whiteChordSignature,
                                                bool& whiteChordSignatureValid,
                                                std::array<int, 5>& blackFiveClassSet,
                                                std::array<std::array<std::array<uint8_t, 16>, 128>, 16>& blackMappedNoteStack,
                                                std::array<std::array<std::array<uint8_t, 16>, 128>, 16>& blackMappedVelocityStack,
                                                std::array<std::array<uint8_t, 128>, 16>& blackMappedNoteStackSize,
                                                std::array<std::array<uint8_t, 128>, 16>& blackPhysicalHeldCount,
                                                std::array<std::array<uint8_t, 128>, 16>& blackPhysicalLastVelocity,
                                                std::array<std::array<uint8_t, 128>, 16>& blackDeferredOffCount,
                                                bool applyDeferredBlackSetUpdate,
                                                const std::array<int, 5>& deferredBlackFiveClassSet,
                                                int deferredUpdateSamplePos)
    {
        out.clear();
        whiteOriginOut.clear();
        blackOriginOut.clear();
        const int maxWhiteStackDepth = (int) whiteMappedNoteStack[0][0].size();
        const int maxBlackStackDepth = (int) blackMappedNoteStack[0][0].size();

        const int wrappedTonic = ((tonicPc % 12) + 12) % 12;
        const auto& degreePattern =
            kModeDegreePatterns[(size_t) juce::jlimit(0, 9, modeIndex)];

        auto adjustWhiteHeldCount = [&](int channel1, int note, int delta) noexcept
        {
            const int chIndex = juce::jlimit(1, 16, channel1) - 1;
            const int noteIndex = juce::jlimit(0, 127, note);
            auto& c = whiteHeldCounts[(size_t) chIndex][(size_t) noteIndex];
            c = juce::jmax(0, c + delta);
        };

        auto countBlackMappedRefsForNote = [&](int chIndex0, int mappedNote) noexcept
        {
            if (!juce::isPositiveAndBelow(chIndex0, 16))
                return 0;

            const int noteIndex = juce::jlimit(0, 127, mappedNote);
            int refs = 0;
            for (int inputNote = 0; inputNote < 128; ++inputNote)
            {
                if (!isBlackPitchClass(inputNote % 12))
                    continue;

                const auto& mappedStack = blackMappedNoteStack[(size_t) chIndex0][(size_t) inputNote];
                const auto stackSize = blackMappedNoteStackSize[(size_t) chIndex0][(size_t) inputNote];
                for (uint8_t depth = 0; depth < stackSize; ++depth)
                {
                    if ((int) mappedStack[(size_t) depth] == noteIndex)
                        ++refs;
                }
            }
            return refs;
        };

        auto deferOrEmitBlackOff = [&](int chIndex0, int mappedNote, int samplePos)
        {
            const int safeCh = juce::jlimit(0, 15, chIndex0);
            const int safeNote = juce::jlimit(0, 127, mappedNote);
            const int channel1 = safeCh + 1;

            // If a white-origin note is currently held on the same mapped key,
            // defer black NOTE OFF to avoid stealing that sustained white note.
            if (whiteHeldCounts[(size_t) safeCh][(size_t) safeNote] > 0)
            {
                auto& pending = blackDeferredOffCount[(size_t) safeCh][(size_t) safeNote];
                pending = (uint8_t) juce::jmin(255, (int) pending + 1);
                return;
            }

            out.addEvent(juce::MidiMessage::noteOff(channel1, safeNote), samplePos);
            blackOriginOut.addEvent(juce::MidiMessage::noteOff(channel1, safeNote), samplePos);
        };

        auto drainDeferredBlackOffsIfSafe = [&](int samplePos)
        {
            for (int ch = 0; ch < 16; ++ch)
            {
                for (int mappedNote = 0; mappedNote < 128; ++mappedNote)
                {
                    auto& pending = blackDeferredOffCount[(size_t) ch][(size_t) mappedNote];
                    if (pending == 0)
                        continue;

                    if (whiteHeldCounts[(size_t) ch][(size_t) mappedNote] > 0)
                        continue;

                    if (countBlackMappedRefsForNote(ch, mappedNote) > 0)
                        continue;

                    for (uint8_t i = 0; i < pending; ++i)
                    {
                        out.addEvent(juce::MidiMessage::noteOff(ch + 1, mappedNote), samplePos);
                        blackOriginOut.addEvent(juce::MidiMessage::noteOff(ch + 1, mappedNote), samplePos);
                    }
                    pending = 0;
                }
            }
        };

        auto applyBlackSetWithRetrig = [&](const std::array<int, 5>& newSet, int samplePos)
        {
            bool setChanged = false;
            for (int i = 0; i < 5; ++i)
            {
                if (blackFiveClassSet[(size_t) i] != newSet[(size_t) i])
                {
                    setChanged = true;
                    break;
                }
            }

            bool staleHeldMapping = false;
            if (!setChanged)
            {
                for (int ch = 0; ch < 16 && !staleHeldMapping; ++ch)
                {
                    for (int inputNote = 0; inputNote < 128; ++inputNote)
                    {
                        if (!isBlackPitchClass(inputNote % 12))
                            continue;

                        const auto& mappedStack =
                            blackMappedNoteStack[(size_t) ch][(size_t) inputNote];
                        const auto stackSize =
                            blackMappedNoteStackSize[(size_t) ch][(size_t) inputNote];
                        if (stackSize == 0)
                            continue;

                        const int expectedMappedNote =
                            computeMappedBlackOutputNote(inputNote, newSet);
                        for (uint8_t depth = 0; depth < stackSize; ++depth)
                        {
                            if ((int) mappedStack[(size_t) depth] != expectedMappedNote)
                            {
                                staleHeldMapping = true;
                                break;
                            }
                        }

                        if (staleHeldMapping)
                            break;
                    }
                }
            }

            if (!setChanged && !staleHeldMapping)
                return;

            for (int ch = 0; ch < 16; ++ch)
            {
                for (int inputNote = 0; inputNote < 128; ++inputNote)
                {
                    if (!isBlackPitchClass(inputNote % 12))
                        continue;

                    auto& mappedStack = blackMappedNoteStack[(size_t) ch][(size_t) inputNote];
                    auto& velStack = blackMappedVelocityStack[(size_t) ch][(size_t) inputNote];
                    auto& stackSize = blackMappedNoteStackSize[(size_t) ch][(size_t) inputNote];
                    if (stackSize == 0)
                        continue;

                    const int newMappedNote = computeMappedBlackOutputNote(inputNote, newSet);
                    for (uint8_t depth = 0; depth < stackSize; ++depth)
                    {
                        const int oldMappedNote = (int) mappedStack[(size_t) depth];
                        const int onVelocity = juce::jlimit(1, 127, (int) velStack[(size_t) depth]);

                        if (oldMappedNote == newMappedNote)
                            continue;

                        deferOrEmitBlackOff(ch, oldMappedNote, samplePos);

                        out.addEvent(juce::MidiMessage::noteOn(ch + 1, newMappedNote, (juce::uint8) onVelocity),
                                     samplePos);
                        blackOriginOut.addEvent(juce::MidiMessage::noteOn(ch + 1, newMappedNote, (juce::uint8) onVelocity),
                                                samplePos);

                        mappedStack[(size_t) depth] = (uint8_t) newMappedNote;
                    }
                }
            }

            blackFiveClassSet = newSet;
        };

        auto recomputeBlackSetFromWhiteHeld = [&](int samplePos, bool retrigHeldBlackNotes)
        {
            uint16_t whiteMask = 0;
            int whiteBassPc = -1;

            for (int note = 0; note < 128; ++note)
            {
                bool held = false;
                for (int ch = 0; ch < 16; ++ch)
                {
                    if (whiteHeldCounts[(size_t) ch][(size_t) note] > 0)
                    {
                        held = true;
                        break;
                    }
                }

                if (!held)
                    continue;

                const int pc = note % 12;
                whiteMask = (uint16_t) (whiteMask | (uint16_t) (1u << pc));
                if (whiteBassPc < 0)
                    whiteBassPc = pc;
            }

            const uint32_t newSignature =
                makeWhiteChordSignature(whiteMask, whiteBassPc, wrappedTonic, modeIndex);

            if (!whiteChordSignatureValid || newSignature != whiteChordSignature)
            {
                const auto computedSet = buildBlackFiveClassSetDeterministic(whiteMask,
                                                                              whiteBassPc,
                                                                              wrappedTonic,
                                                                              modeIndex);
                if (retrigHeldBlackNotes)
                    applyBlackSetWithRetrig(computedSet, samplePos);
                else
                    blackFiveClassSet = computedSet;
                whiteChordSignature = newSignature;
                whiteChordSignatureValid = true;
            }
        };

        auto flushHeldWhiteStackForChannel = [&](int chIndex0, int samplePos)
        {
            if (!juce::isPositiveAndBelow(chIndex0, 16))
                return;

            for (int inputNote = 0; inputNote < 128; ++inputNote)
            {
                if (whiteDegreeIndexForPitchClass(inputNote % 12) < 0)
                    continue;

                auto& mappedStack = whiteMappedNoteStack[(size_t) chIndex0][(size_t) inputNote];
                auto& velStack = whiteMappedVelocityStack[(size_t) chIndex0][(size_t) inputNote];
                auto& stackSize = whiteMappedNoteStackSize[(size_t) chIndex0][(size_t) inputNote];
                if (stackSize == 0)
                    continue;

                while (stackSize > 0)
                {
                    const uint8_t depth = (uint8_t) (stackSize - 1);
                    const int mappedNote = juce::jlimit(0, 127, (int) mappedStack[(size_t) depth]);
                    out.addEvent(juce::MidiMessage::noteOff(chIndex0 + 1, mappedNote), samplePos);
                    whiteOriginOut.addEvent(juce::MidiMessage::noteOff(chIndex0 + 1, mappedNote), samplePos);
                    adjustWhiteHeldCount(chIndex0 + 1, mappedNote, -1);
                    mappedStack[(size_t) depth] = 0;
                    velStack[(size_t) depth] = 0;
                    --stackSize;
                }
            }

            whitePhysicalHeldCount[(size_t) chIndex0].fill(0);
            whitePhysicalLastVelocity[(size_t) chIndex0].fill(0);
        };

        auto flushHeldBlackStackForChannel = [&](int chIndex0, int samplePos)
        {
            if (!juce::isPositiveAndBelow(chIndex0, 16))
                return;

            for (int inputNote = 0; inputNote < 128; ++inputNote)
            {
                if (!isBlackPitchClass(inputNote % 12))
                    continue;

                auto& mappedStack = blackMappedNoteStack[(size_t) chIndex0][(size_t) inputNote];
                auto& velStack = blackMappedVelocityStack[(size_t) chIndex0][(size_t) inputNote];
                auto& stackSize = blackMappedNoteStackSize[(size_t) chIndex0][(size_t) inputNote];
                if (stackSize == 0)
                    continue;

                while (stackSize > 0)
                {
                    const uint8_t depth = (uint8_t) (stackSize - 1);
                    const int mappedNote = juce::jlimit(0, 127, (int) mappedStack[(size_t) depth]);
                    out.addEvent(juce::MidiMessage::noteOff(chIndex0 + 1, mappedNote), samplePos);
                    blackOriginOut.addEvent(juce::MidiMessage::noteOff(chIndex0 + 1, mappedNote), samplePos);
                    mappedStack[(size_t) depth] = 0;
                    velStack[(size_t) depth] = 0;
                    --stackSize;
                }
            }

            blackPhysicalHeldCount[(size_t) chIndex0].fill(0);
            blackPhysicalLastVelocity[(size_t) chIndex0].fill(0);
            blackDeferredOffCount[(size_t) chIndex0].fill(0);
        };

        // Keep deterministic init on first run only.
        // Do not recompute silently on every block, otherwise white NOTE OFF-only
        // changes can mutate the mapping without retrig on held black notes.
        if (!whiteChordSignatureValid)
            recomputeBlackSetFromWhiteHeld(0, false);

        if (blackModeEnabled && applyDeferredBlackSetUpdate)
            applyBlackSetWithRetrig(deferredBlackFiveClassSet, juce::jmax(0, deferredUpdateSamplePos));

        int healSamplePos = 0;
        for (auto it = in.begin(); it != in.end(); ++it)
        {
            const auto meta = *it;
            const auto& msg = meta.getMessage();
            healSamplePos = juce::jmax(healSamplePos, meta.samplePosition);

            const bool noteOn = msg.isNoteOn() && msg.getVelocity() > 0.0f;
            const bool noteOff = msg.isNoteOff() || (msg.isNoteOn() && msg.getVelocity() == 0.0f);
            const bool polyAftertouch = msg.isAftertouch();
            const bool noteScopedEvent = noteOn || noteOff || polyAftertouch;

            const bool isAllNotesOffMessage =
                msg.isAllNotesOff()
                || msg.isAllSoundOff()
                || (msg.isController()
                    && (msg.getControllerNumber() == 123 || msg.getControllerNumber() == 120));

            if (isAllNotesOffMessage)
            {
                const int chIndex = juce::jlimit(1, 16, msg.getChannel()) - 1;
                flushHeldWhiteStackForChannel(chIndex, meta.samplePosition);
                flushHeldBlackStackForChannel(chIndex, meta.samplePosition);
                whiteHeldCounts[(size_t) chIndex].fill(0);
                whiteChordSignatureValid = false;
                out.addEvent(msg, meta.samplePosition);
                continue;
            }

            if (!noteScopedEvent)
            {
                out.addEvent(msg, meta.samplePosition);
                continue;
            }

            const int inputNote = juce::jlimit(0, 127, msg.getNoteNumber());
            const int pc = inputNote % 12;
            const int degreeIndex = whiteDegreeIndexForPitchClass(pc);

            if (degreeIndex >= 0)
            {
                const int chIndex = juce::jlimit(1, 16, msg.getChannel()) - 1;
                const int inputIndex = juce::jlimit(0, 127, inputNote);
                auto& physicalCount = whitePhysicalHeldCount[(size_t) chIndex][(size_t) inputIndex];
                auto& physicalLastVel = whitePhysicalLastVelocity[(size_t) chIndex][(size_t) inputIndex];
                auto& mappedStack = whiteMappedNoteStack[(size_t) chIndex][(size_t) inputIndex];
                auto& velStack = whiteMappedVelocityStack[(size_t) chIndex][(size_t) inputIndex];
                auto& mappedStackSize = whiteMappedNoteStackSize[(size_t) chIndex][(size_t) inputIndex];

                const int octaveBase = inputNote - pc;
                // Keep white-key order monotonic by preserving octave carry
                // from (tonic + modal degree), instead of reducing to pitch-class
                // before composing the MIDI note number.
                const int mappedAbs = wrappedTonic + degreePattern[(size_t) degreeIndex];
                int mappedNote = juce::jlimit(0, 127, octaveBase + mappedAbs);

                if (noteOn)
                {
                    physicalLastVel = (uint8_t) velocityFloatToMidi127(msg.getVelocity());
                    physicalCount = (uint8_t) juce::jmin((int) physicalCount + 1, maxWhiteStackDepth);

                    if (mappedStackSize < maxWhiteStackDepth)
                    {
                        mappedStack[(size_t) mappedStackSize] = (uint8_t) mappedNote;
                        velStack[(size_t) mappedStackSize] = (uint8_t) velocityFloatToMidi127(msg.getVelocity());
                        ++mappedStackSize;
                    }
                    else
                    {
                        // Saturated white remap stack: retire oldest mapped note
                        // with explicit NOTE OFF to avoid leaked/stuck note state.
                        const int droppedMappedNote = juce::jlimit(0, 127, (int) mappedStack[0]);
                        out.addEvent(juce::MidiMessage::noteOff(msg.getChannel(), droppedMappedNote),
                                     meta.samplePosition);
                        whiteOriginOut.addEvent(juce::MidiMessage::noteOff(msg.getChannel(), droppedMappedNote),
                                                meta.samplePosition);
                        adjustWhiteHeldCount(msg.getChannel(), droppedMappedNote, -1);

                        for (size_t iStack = 1; iStack < mappedStack.size(); ++iStack)
                        {
                            mappedStack[iStack - 1] = mappedStack[iStack];
                            velStack[iStack - 1] = velStack[iStack];
                        }
                        mappedStack[mappedStack.size() - 1] = (uint8_t) mappedNote;
                        velStack[velStack.size() - 1] = (uint8_t) velocityFloatToMidi127(msg.getVelocity());
                    }
                }
                else if (noteOff)
                {
                    physicalCount = (uint8_t) juce::jmax(0, (int) physicalCount - 1);

                    if (mappedStackSize > 0)
                    {
                        mappedNote = (int) mappedStack[(size_t) (mappedStackSize - 1)];
                        velStack[(size_t) (mappedStackSize - 1)] = 0;
                        --mappedStackSize;
                    }
                }
                else if (polyAftertouch)
                {
                    if (mappedStackSize > 0)
                        mappedNote = (int) mappedStack[(size_t) (mappedStackSize - 1)];
                }

                juce::MidiMessage mappedMsg;
                if (noteOn)
                    mappedMsg = juce::MidiMessage::noteOn(msg.getChannel(), mappedNote, msg.getVelocity());
                else if (noteOff)
                    mappedMsg = juce::MidiMessage::noteOff(msg.getChannel(), mappedNote);
                else
                    mappedMsg = juce::MidiMessage::aftertouchChange(msg.getChannel(), mappedNote, msg.getAfterTouchValue());

                out.addEvent(mappedMsg, meta.samplePosition);
                whiteOriginOut.addEvent(mappedMsg, meta.samplePosition);

                if (noteOn || noteOff)
                {
                    adjustWhiteHeldCount(mappedMsg.getChannel(),
                                         mappedMsg.getNoteNumber(),
                                         noteOn ? 1 : -1);
                    // Rule: white-note releases do not retrigger black-set recompute.
                    // Only white NOTE ON causes an immediate black remap update.
                    if (noteOn)
                        recomputeBlackSetFromWhiteHeld(meta.samplePosition, true);
                }
                continue;
            }

            if (!isBlackPitchClass(pc))
            {
                out.addEvent(msg, meta.samplePosition);
                continue;
            }

            if (!blackModeEnabled)
            {
                // B inactive: black keys are ignored in W mode, but NOTE OFF is
                // forwarded unchanged to remain resilient against stale host events.
                if (noteOff)
                    out.addEvent(msg, meta.samplePosition);
                continue;
            }

            const int chIndex = juce::jlimit(1, 16, msg.getChannel()) - 1;
            const int inputIndex = juce::jlimit(0, 127, inputNote);
            auto& physicalCount = blackPhysicalHeldCount[(size_t) chIndex][(size_t) inputIndex];
            auto& physicalLastVel = blackPhysicalLastVelocity[(size_t) chIndex][(size_t) inputIndex];

            if (noteOn)
            {
                physicalLastVel = (uint8_t) velocityFloatToMidi127(msg.getVelocity());
                physicalCount = (uint8_t) juce::jmin((int) physicalCount + 1, maxBlackStackDepth);
            }
            else if (noteOff)
            {
                physicalCount = (uint8_t) juce::jmax(0, (int) physicalCount - 1);
            }

            auto& mappedStack = blackMappedNoteStack[(size_t) chIndex][(size_t) inputIndex];
            auto& velStack = blackMappedVelocityStack[(size_t) chIndex][(size_t) inputIndex];
            auto& mappedStackSize = blackMappedNoteStackSize[(size_t) chIndex][(size_t) inputIndex];

            int mappedNote = computeMappedBlackOutputNote(inputNote, blackFiveClassSet);

            if (noteOn)
            {
                if (mappedStackSize < 16)
                {
                    mappedStack[(size_t) mappedStackSize++] = (uint8_t) mappedNote;
                    velStack[(size_t) (mappedStackSize - 1)] =
                        (uint8_t) velocityFloatToMidi127(msg.getVelocity());
                }
                else
                {
                    // Defensive RT self-heal:
                    // if an input stream overflows per-key depth, retire the oldest
                    // mapped note with an explicit NOTE OFF before dropping it.
                    // This avoids leaked/stuck mapped notes after long sessions.
                    const int droppedMappedNote = juce::jlimit(0, 127, (int) mappedStack[0]);
                    deferOrEmitBlackOff(msg.getChannel() - 1, droppedMappedNote, meta.samplePosition);

                    for (size_t iStack = 1; iStack < mappedStack.size(); ++iStack)
                    {
                        mappedStack[iStack - 1] = mappedStack[iStack];
                        velStack[iStack - 1] = velStack[iStack];
                    }
                    mappedStack[mappedStack.size() - 1] = (uint8_t) mappedNote;
                    velStack[velStack.size() - 1] =
                        (uint8_t) velocityFloatToMidi127(msg.getVelocity());
                }
            }
            else if (noteOff)
            {
                if (mappedStackSize > 0)
                {
                    mappedNote = (int) mappedStack[(size_t) (mappedStackSize - 1)];
                    velStack[(size_t) (mappedStackSize - 1)] = 0;
                    --mappedStackSize;
                }
            }
            else if (polyAftertouch)
            {
                if (mappedStackSize > 0)
                    mappedNote = (int) mappedStack[(size_t) (mappedStackSize - 1)];
            }

            juce::MidiMessage mappedMsg;
            if (noteOn)
                mappedMsg = juce::MidiMessage::noteOn(msg.getChannel(), mappedNote, msg.getVelocity());
            else if (noteOff)
                mappedMsg = juce::MidiMessage::noteOff(msg.getChannel(), mappedNote);
            else
                mappedMsg = juce::MidiMessage::aftertouchChange(msg.getChannel(), mappedNote, msg.getAfterTouchValue());

            out.addEvent(mappedMsg, meta.samplePosition);
            blackOriginOut.addEvent(mappedMsg, meta.samplePosition);
        }

        // Self-heal white remap state from physical input state.
        // This mirrors black remap healing and closes NOTE ON/OFF pairing gaps
        // when dense passages or host ordering glitches occur.
        for (int ch = 0; ch < 16; ++ch)
        {
            for (int inputNote = 0; inputNote < 128; ++inputNote)
            {
                const int inputPc = inputNote % 12;
                const int degreeIdx = whiteDegreeIndexForPitchClass(inputPc);
                if (degreeIdx < 0)
                    continue;

                auto& mappedStack = whiteMappedNoteStack[(size_t) ch][(size_t) inputNote];
                auto& velStack = whiteMappedVelocityStack[(size_t) ch][(size_t) inputNote];
                auto& stackSize = whiteMappedNoteStackSize[(size_t) ch][(size_t) inputNote];
                const int expectedCount = juce::jlimit(
                    0,
                    maxWhiteStackDepth,
                    (int) whitePhysicalHeldCount[(size_t) ch][(size_t) inputNote]);
                const int octaveBase = inputNote - inputPc;
                const int expectedMappedNote = juce::jlimit(
                    0,
                    127,
                    octaveBase + wrappedTonic + degreePattern[(size_t) degreeIdx]);
                const int fallbackVelocity = juce::jlimit(
                    1, 127,
                    juce::jmax(1, (int) whitePhysicalLastVelocity[(size_t) ch][(size_t) inputNote]));

                while ((int) stackSize > expectedCount)
                {
                    const uint8_t depth = (uint8_t) (stackSize - 1);
                    const int oldMappedNote = juce::jlimit(0, 127, (int) mappedStack[(size_t) depth]);
                    out.addEvent(juce::MidiMessage::noteOff(ch + 1, oldMappedNote), healSamplePos);
                    whiteOriginOut.addEvent(juce::MidiMessage::noteOff(ch + 1, oldMappedNote), healSamplePos);
                    adjustWhiteHeldCount(ch + 1, oldMappedNote, -1);
                    mappedStack[(size_t) depth] = 0;
                    velStack[(size_t) depth] = 0;
                    --stackSize;
                }

                while ((int) stackSize < expectedCount)
                {
                    const uint8_t depth = stackSize;
                    mappedStack[(size_t) depth] = (uint8_t) expectedMappedNote;
                    velStack[(size_t) depth] = (uint8_t) fallbackVelocity;
                    ++stackSize;
                    out.addEvent(
                        juce::MidiMessage::noteOn(ch + 1,
                                                  expectedMappedNote,
                                                  (juce::uint8) fallbackVelocity),
                        healSamplePos);
                    whiteOriginOut.addEvent(
                        juce::MidiMessage::noteOn(ch + 1,
                                                  expectedMappedNote,
                                                  (juce::uint8) fallbackVelocity),
                        healSamplePos);
                    adjustWhiteHeldCount(ch + 1, expectedMappedNote, 1);
                }

                for (uint8_t depth = 0; depth < stackSize; ++depth)
                {
                    const int oldMappedNote = juce::jlimit(0, 127, (int) mappedStack[(size_t) depth]);
                    if (oldMappedNote == expectedMappedNote)
                        continue;

                    const int retrigVelocity = juce::jlimit(
                        1, 127,
                        (int) (velStack[(size_t) depth] > 0
                                   ? velStack[(size_t) depth]
                                   : (uint8_t) fallbackVelocity));

                    out.addEvent(juce::MidiMessage::noteOff(ch + 1, oldMappedNote), healSamplePos);
                    whiteOriginOut.addEvent(juce::MidiMessage::noteOff(ch + 1, oldMappedNote), healSamplePos);
                    adjustWhiteHeldCount(ch + 1, oldMappedNote, -1);
                    out.addEvent(
                        juce::MidiMessage::noteOn(ch + 1,
                                                  expectedMappedNote,
                                                  (juce::uint8) retrigVelocity),
                        healSamplePos);
                    whiteOriginOut.addEvent(
                        juce::MidiMessage::noteOn(ch + 1,
                                                  expectedMappedNote,
                                                  (juce::uint8) retrigVelocity),
                        healSamplePos);
                    adjustWhiteHeldCount(ch + 1, expectedMappedNote, 1);

                    mappedStack[(size_t) depth] = (uint8_t) expectedMappedNote;
                    velStack[(size_t) depth] = (uint8_t) retrigVelocity;
                }
            }
        }

        // Self-heal black remap state from physical input state.
        // This keeps black-key mapping robust under long sessions and rapid
        // white-chord transitions, even if an earlier block lost pairing.
        if (blackModeEnabled)
        {
            for (int ch = 0; ch < 16; ++ch)
            {
                for (int inputNote = 0; inputNote < 128; ++inputNote)
                {
                    if (!isBlackPitchClass(inputNote % 12))
                        continue;

                    auto& mappedStack = blackMappedNoteStack[(size_t) ch][(size_t) inputNote];
                    auto& velStack = blackMappedVelocityStack[(size_t) ch][(size_t) inputNote];
                    auto& stackSize = blackMappedNoteStackSize[(size_t) ch][(size_t) inputNote];
                    const int expectedCount = juce::jlimit(
                        0,
                        maxBlackStackDepth,
                        (int) blackPhysicalHeldCount[(size_t) ch][(size_t) inputNote]);
                    const int expectedMappedNote =
                        computeMappedBlackOutputNote(inputNote, blackFiveClassSet);
                    const int fallbackVelocity = juce::jlimit(
                        1, 127,
                        juce::jmax(1, (int) blackPhysicalLastVelocity[(size_t) ch][(size_t) inputNote]));

                    while ((int) stackSize > expectedCount)
                    {
                        const uint8_t depth = (uint8_t) (stackSize - 1);
                        const int oldMappedNote = juce::jlimit(0, 127, (int) mappedStack[(size_t) depth]);
                        deferOrEmitBlackOff(ch, oldMappedNote, healSamplePos);
                        mappedStack[(size_t) depth] = 0;
                        velStack[(size_t) depth] = 0;
                        --stackSize;
                    }

                    while ((int) stackSize < expectedCount)
                    {
                        const uint8_t depth = stackSize;
                        mappedStack[(size_t) depth] = (uint8_t) expectedMappedNote;
                        velStack[(size_t) depth] = (uint8_t) fallbackVelocity;
                        ++stackSize;
                        out.addEvent(
                            juce::MidiMessage::noteOn(ch + 1,
                                                      expectedMappedNote,
                                                      (juce::uint8) fallbackVelocity),
                            healSamplePos);
                        blackOriginOut.addEvent(
                            juce::MidiMessage::noteOn(ch + 1,
                                                      expectedMappedNote,
                                                      (juce::uint8) fallbackVelocity),
                            healSamplePos);
                    }

                    for (uint8_t depth = 0; depth < stackSize; ++depth)
                    {
                        const int oldMappedNote = juce::jlimit(0, 127, (int) mappedStack[(size_t) depth]);
                        if (oldMappedNote == expectedMappedNote)
                            continue;

                        const int retrigVelocity = juce::jlimit(
                            1, 127,
                            (int) (velStack[(size_t) depth] > 0
                                       ? velStack[(size_t) depth]
                                       : (uint8_t) fallbackVelocity));

                        deferOrEmitBlackOff(ch, oldMappedNote, healSamplePos);
                        out.addEvent(
                            juce::MidiMessage::noteOn(ch + 1,
                                                      expectedMappedNote,
                                                      (juce::uint8) retrigVelocity),
                            healSamplePos);
                        blackOriginOut.addEvent(
                            juce::MidiMessage::noteOn(ch + 1,
                                                      expectedMappedNote,
                                                      (juce::uint8) retrigVelocity),
                            healSamplePos);

                        mappedStack[(size_t) depth] = (uint8_t) expectedMappedNote;
                        velStack[(size_t) depth] = (uint8_t) retrigVelocity;
                    }
                }
            }
        }

        // Flush deferred black NOTE OFF only when safe:
        // - no white-held ownership on the mapped note
        // - no black mapped stack still referencing that mapped note
        // This prevents white-note steals while preserving eventual NOTE OFF
        // symmetry for retargeted black notes.
        drainDeferredBlackOffsIfSafe(healSamplePos);
    }

    struct PitchClassSnapshot
    {
        uint16_t pitchClassMask = 0;
        int bassPitchClass = -1;
    };

    static PitchClassSnapshot buildPitchClassSnapshotFromHeldCounts(
        const std::array<std::array<int, 128>, 16>& heldCounts) noexcept
    {
        PitchClassSnapshot snap;

        for (int note = 0; note < 128; ++note)
        {
            bool held = false;
            for (int ch = 0; ch < 16; ++ch)
            {
                if (heldCounts[(size_t) ch][(size_t) note] > 0)
                {
                    held = true;
                    break;
                }
            }

            if (!held)
                continue;

            const int pc = note % 12;
            snap.pitchClassMask = (uint16_t) (snap.pitchClassMask | (uint16_t) (1u << pc));
            if (snap.bassPitchClass < 0)
                snap.bassPitchClass = pc;
        }

        return snap;
    }

    static inline int readRoundedParamValue(const std::atomic<float>* raw,
                                            int fallback = 0) noexcept
    {
        if (raw == nullptr)
            return fallback;
        return juce::roundToInt(raw->load(std::memory_order_relaxed));
    }

    static inline uint64_t hashMix64(uint64_t hash, uint64_t value) noexcept
    {
        hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
        return hash;
    }

    enum class ArpEqScalarCompareMode : uint8_t
    {
        BoolSwitch = 0,  // compared as > 0.5f
        MorphInt,        // compared as jlimit(-100,100, roundToInt())
        ChoiceIndex      // compared as rounded choice index
    };

    static constexpr int kArpEqScalarIndexArpMode = 1;

    static constexpr std::array<ArpEqScalarCompareMode, 25> kArpEqScalarCompareModes =
    {{
        ArpEqScalarCompareMode::BoolSwitch, // arpeggiatorEnable
        ArpEqScalarCompareMode::ChoiceIndex, // arpMode
        ArpEqScalarCompareMode::MorphInt,   // arpRateMorph
        ArpEqScalarCompareMode::MorphInt,   // arpDirectionMorph
        ArpEqScalarCompareMode::MorphInt,   // arpPatternMorph
        ArpEqScalarCompareMode::MorphInt,   // arpRangeMorph
        ArpEqScalarCompareMode::MorphInt,   // arpVelocityMorph
        ArpEqScalarCompareMode::MorphInt,   // arpGrooveMorph
        ArpEqScalarCompareMode::MorphInt,   // arpGateMorph
        ArpEqScalarCompareMode::MorphInt,   // arpAccentMorph
        ArpEqScalarCompareMode::ChoiceIndex, // arpRateLink
        ArpEqScalarCompareMode::ChoiceIndex, // arpDirectionLink
        ArpEqScalarCompareMode::ChoiceIndex, // arpPatternLink
        ArpEqScalarCompareMode::ChoiceIndex, // arpRangeLink
        ArpEqScalarCompareMode::ChoiceIndex, // arpVelocityLink
        ArpEqScalarCompareMode::ChoiceIndex, // arpGrooveLink
        ArpEqScalarCompareMode::ChoiceIndex, // arpGateLink
        ArpEqScalarCompareMode::ChoiceIndex, // arpAccentLink
        ArpEqScalarCompareMode::ChoiceIndex, // arpDirectionUnlinkRate
        ArpEqScalarCompareMode::ChoiceIndex, // arpPatternUnlinkRate
        ArpEqScalarCompareMode::ChoiceIndex, // arpRangeUnlinkRate
        ArpEqScalarCompareMode::ChoiceIndex, // arpVelocityUnlinkRate
        ArpEqScalarCompareMode::ChoiceIndex, // arpGrooveUnlinkRate
        ArpEqScalarCompareMode::ChoiceIndex, // arpGateUnlinkRate
        ArpEqScalarCompareMode::ChoiceIndex  // arpAccentUnlinkRate
    }};

    static inline bool equalArpScalarEffective(float a,
                                               float b,
                                               ArpEqScalarCompareMode mode) noexcept
    {
        switch (mode)
        {
            case ArpEqScalarCompareMode::BoolSwitch:
                return (a > 0.5f) == (b > 0.5f);

            case ArpEqScalarCompareMode::MorphInt:
                return juce::jlimit(-100, 100, juce::roundToInt(a))
                    == juce::jlimit(-100, 100, juce::roundToInt(b));

            case ArpEqScalarCompareMode::ChoiceIndex:
                return juce::roundToInt(a) == juce::roundToInt(b);
        }

        return false;
    }

    static inline int quantizeEffectiveArpMorphForPhaseLock(const std::atomic<float>* morphRaw,
                                                            float lfoNormDelta) noexcept
    {
        const float baseMorph = (morphRaw != nullptr) ? morphRaw->load() : 0.0f;
        const float baseNorm = juce::jlimit(0.0f, 1.0f, (baseMorph + 100.0f) / 200.0f);
        const float effectiveNorm =
            juce::jlimit(0.0f, 1.0f, baseNorm + juce::jlimit(-1.0f, 1.0f, lfoNormDelta));
        const float effectiveMorph = (effectiveNorm * 200.0f) - 100.0f;
        return juce::jlimit(-100, 100, juce::roundToInt(effectiveMorph));
    }

    static constexpr std::array<double, 19> kLfoRateWholeNotes =
    {{
        4.0,        // 4/1
        3.0,        // 3/1
        2.0,        // 2/1
        1.0,        // 1/1
        0.5,        // 1/2
        0.75,       // 1/2.
        1.0 / 3.0,  // 1/2t
        0.25,       // 1/4
        0.375,      // 1/4.
        1.0 / 6.0,  // 1/4t
        0.125,      // 1/8
        0.1875,     // 1/8.
        1.0 / 12.0, // 1/8t
        0.0625,     // 1/16
        0.09375,    // 1/16.
        1.0 / 24.0, // 1/16t
        0.03125,    // 1/32
        0.046875,   // 1/32.
        1.0 / 48.0  // 1/32t
    }};

    // Snapshot UI fallback identifiers (kept in sync with PluginEditor/LFOGroup).
    static const juce::Identifier kUiStateNodeID           { "UI_STATE" };
    static const juce::Identifier kUiActiveLaneCountPropID { "activeLaneCount" };
    static const juce::Identifier kUiLfoStateNodeID        { "LFO_STATE" };
    static const juce::Identifier kUiLfoRowNodeID          { "LFO_ROW" };
    static const juce::Identifier kUiLfoCountPropID        { "count" };
    static const juce::Identifier kUiLfoRowIndexPropID     { "index" };
    static const juce::Identifier kUiLfoRatePropID         { "rate" };
    static const juce::Identifier kUiLfoDepthPropID        { "depth" };
    static const juce::Identifier kUiLfoDestinationPropID  { "destinationId" };
    static const juce::Identifier kUiLfoDestinationsPropID { "destinationIds" };
    static const juce::Identifier kUiLfoOffsetPropID       { "offset" };
    static const juce::Identifier kUiLfoWavePropID         { "wave" };

    static juce::String serializeDestinationIds(const std::array<int, MidivisiViciAudioProcessor::kMaxLfoDestinationsPerRow>& ids,
                                                int count)
    {
        juce::StringArray parts;
        const int safeCount = juce::jlimit(0, MidivisiViciAudioProcessor::kMaxLfoDestinationsPerRow, count);
        parts.ensureStorageAllocated(safeCount);

        for (int i = 0; i < safeCount; ++i)
            parts.add(juce::String(ids[(size_t) i]));

        return parts.joinIntoString(",");
    }

    static juce::Array<int> parseDestinationIds(const juce::String& csv)
    {
        juce::Array<int> ids;
        const auto tokens = juce::StringArray::fromTokens(csv, ",", {});
        ids.ensureStorageAllocated(tokens.size());

        for (const auto& token : tokens)
        {
            const int id = juce::jmax(MidivisiViciAudioProcessor::LfoDestinationIds::kNone,
                                      token.trim().getIntValue());
            ids.addIfNotAlreadyThere(id);
        }

        return ids;
    }

    static inline float wrap01(float value) noexcept
    {
        float wrapped = std::fmod(value, 1.0f);
        if (wrapped < 0.0f)
            wrapped += 1.0f;
        return wrapped;
    }

    static inline float randomUnit(float x) noexcept
    {
        const float v = std::sin(x * 12.9898f) * 43758.5453f;
        return v - std::floor(v);
    }

    static inline float evaluateLfoWaveSample(int waveShapeIndex,
                                              float phase01,
                                              float offsetCycles) noexcept
    {
        const float t = wrap01(phase01 + offsetCycles);

        switch (juce::jlimit(0, 4, waveShapeIndex))
        {
            case 0: return std::sin(t * juce::MathConstants<float>::twoPi); // Sine
            case 1: return 1.0f - 4.0f * std::abs(t - 0.5f);                // Triangle
            case 2: return 2.0f * (t - 0.5f);                               // Saw
            case 3: return (t < 0.5f) ? 1.0f : -1.0f;                       // Square
            case 4:
            default:
            {
                constexpr float kStepsPerCycle = 16.0f;
                const float stepIndex = std::floor(t * kStepsPerCycle);
                return (randomUnit(stepIndex + 17.0f) * 2.0f) - 1.0f;       // Random S&H
            }
        }
    }

    static inline bool decodeLfoDestinationStableId(int stableId,
                                                    int& outLaneIndex,
                                                    int& outSlot) noexcept
    {
        if (stableId >= MidivisiViciAudioProcessor::LfoDestinationIds::kMainBarMorphBase)
            return false;

        if (stableId < MidivisiViciAudioProcessor::LfoDestinationIds::kBase)
            return false;

        const int normalized = stableId - MidivisiViciAudioProcessor::LfoDestinationIds::kBase;
        const int laneIndex = normalized / MidivisiViciAudioProcessor::LfoDestinationIds::kLaneStride;
        const int slot = normalized % MidivisiViciAudioProcessor::LfoDestinationIds::kLaneStride;

        if (!juce::isPositiveAndBelow(laneIndex, Lanes::kNumLanes))
            return false;

        outLaneIndex = laneIndex;
        outSlot = slot;
        return true;
    }

    static inline bool decodeLfoMainBarMorphDestinationStableId(int stableId,
                                                                int& outPageIndex) noexcept
    {
        if (!MidivisiViciAudioProcessor::LfoDestinationIds::isMainBarMorph(stableId))
            return false;

        const int pageIndex =
            MidivisiViciAudioProcessor::LfoDestinationIds::getMainBarMorphPageIndex(stableId);

        if (!juce::isPositiveAndBelow(pageIndex, MidivisiViciAudioProcessor::kMainBarMaxPages))
            return false;

        outPageIndex = pageIndex;
        return true;
    }

    static constexpr int kMainBarRateTypeCount = 6;
    static constexpr int kMainBarRateRatiosPerType = 7;
    static constexpr int kMainBarRateChoiceCount =
        kMainBarRateTypeCount * kMainBarRateRatiosPerType; // 42
    static constexpr int kMainBarRateRndChoice = kMainBarRateChoiceCount + 1; // 43

    static inline bool isMainBarRateRegularChoice(int choiceIndex) noexcept
    {
        return choiceIndex >= 1 && choiceIndex <= kMainBarRateChoiceCount;
    }

    static inline int mainBarRateChoiceType(int choiceIndex) noexcept
    {
        return juce::jlimit(0, kMainBarRateTypeCount - 1,
                            (choiceIndex - 1) / kMainBarRateRatiosPerType);
    }

    static inline int mainBarRateChoiceSlot1Based(int choiceIndex) noexcept
    {
        return juce::jlimit(1, kMainBarRateRatiosPerType,
                            ((choiceIndex - 1) % kMainBarRateRatiosPerType) + 1);
    }

    static inline double beatsForMainBarRateChoice(int choiceIndex) noexcept
    {
        if (!isMainBarRateRegularChoice(choiceIndex))
            return 0.25;

        const int type = mainBarRateChoiceType(choiceIndex);
        const int slot = mainBarRateChoiceSlot1Based(choiceIndex) - 1; // 0..6

        static constexpr std::array<std::array<double, 2>, kMainBarRateRatiosPerType> binaries =
        {{
            {1.0, 1.0}, {1.0, 2.0}, {1.0, 4.0}, {1.0, 8.0},
            {1.0, 16.0}, {1.0, 32.0}, {1.0, 64.0}
        }};

        static constexpr std::array<std::array<double, 2>, kMainBarRateRatiosPerType> dotted =
        {{
            {3.0, 2.0}, {3.0, 4.0}, {3.0, 8.0}, {3.0, 16.0},
            {3.0, 32.0}, {3.0, 64.0}, {3.0, 128.0}
        }};

        static constexpr std::array<std::array<double, 2>, kMainBarRateRatiosPerType> doubleDotted =
        {{
            {7.0, 4.0}, {7.0, 8.0}, {7.0, 16.0}, {7.0, 32.0},
            {7.0, 64.0}, {7.0, 128.0}, {7.0, 256.0}
        }};

        static constexpr std::array<std::array<double, 2>, kMainBarRateRatiosPerType> triplet =
        {{
            {2.0, 3.0}, {1.0, 3.0}, {1.0, 6.0}, {1.0, 12.0},
            {1.0, 24.0}, {1.0, 48.0}, {1.0, 96.0}
        }};

        static constexpr std::array<std::array<double, 2>, kMainBarRateRatiosPerType> quintolet =
        {{
            {4.0, 5.0}, {2.0, 5.0}, {1.0, 5.0}, {1.0, 10.0},
            {1.0, 20.0}, {1.0, 40.0}, {1.0, 80.0}
        }};

        static constexpr std::array<std::array<double, 2>, kMainBarRateRatiosPerType> septolet =
        {{
            {4.0, 7.0}, {2.0, 7.0}, {1.0, 7.0}, {1.0, 14.0},
            {1.0, 28.0}, {1.0, 56.0}, {1.0, 112.0}
        }};

        const auto* table = &binaries;
        switch (type)
        {
            case 0: table = &binaries; break;
            case 1: table = &dotted; break;
            case 2: table = &doubleDotted; break;
            case 3: table = &triplet; break;
            case 4: table = &quintolet; break;
            case 5: table = &septolet; break;
            default: break;
        }

        const double numerator = (*table)[(size_t) slot][0];
        const double denominator = (*table)[(size_t) slot][1];
        if (denominator <= 0.0)
            return 0.25;

        return 4.0 * (numerator / denominator);
    }

    static inline int applyMainBarMorphToValue(int baseValue, int morphPercent) noexcept
    {
        const float base = (float) juce::jlimit(0, 127, baseValue);
        const float m = juce::jlimit(0.0f, 100.0f, (float) morphPercent) / 100.0f;

        if (m <= 0.5f)
        {
            const float t = m / 0.5f;
            return juce::jlimit(0, 127, juce::roundToInt(base * t));
        }

        const float t = (m - 0.5f) / 0.5f;
        return juce::jlimit(0, 127, juce::roundToInt(base + ((127.0f - base) * t)));
    }

    static inline int firstStepDefaultChoiceForArpLayer(int layer) noexcept
    {
        switch (layer)
        {
            case 0: return 5;    // Rate: 1/16
            case 1: return 2;    // Arp Direction/Strum: Up
            case 2: return 1;    // Jump: 1
            case 3: return 5;    // Octave: 0
            case 4: return 4;    // Velocity: "="
            case 5: return 8;    // Groove: "="
            // IMPORTANT:
            // le runtime ArpeggiatorProcessor force le pas 1 Gate a 99%
            // (index discret 8 dans la nouvelle table Gate).
            // Conserver la meme valeur ici garantit une equivalence inter-lanes
            // coherente pour le phase-lock.
            case 6: return 8;    // Gate: 99%
            case 7: return 1;    // Retrig: x1
            case 8: return 1;    // Drum Hit: X
            case 9: return 1;    // Drum Velo Env: 0
            case 10: return 1;   // Drum Tim Env: 0
            default: break;
        }

        return 1;
    }

    static inline bool isArpEqSeqLayerRelevantForMode(int layer, int modeIndex) noexcept
    {
        // Shared layers between Arp and Drum.
        if (layer == 0 || layer == 4 || layer == 5 || layer == 6 || layer == 7)
            return true;

        const bool drumMode = (modeIndex == 1);
        if (drumMode)
            return (layer == 8 || layer == 9 || layer == 10); // Hit/VeloEnv/TimEnv

        return (layer == 1 || layer == 2 || layer == 3); // Direction/Pattern/Range
    }

    static inline int normalizeArpStepChoiceForEquivalence(int layer,
                                                           int step,
                                                           int choice) noexcept
    {
        // Runtime behavior: first step cannot remain Skip.
        // Arp engine substitutes Skip->default for step 1 on each layer.
        if (step == 0 && choice == 0)
            return firstStepDefaultChoiceForArpLayer(layer);

        return choice;
    }

    static inline bool matchNoteOnInjectKey(const NoteOnInject& e,
                                            int samplePos, int ch, int note) noexcept
    {
        return (e.samplePos == samplePos &&
                e.channel   == ch &&
                e.note      == note);
    }

    template <size_t N>
    static bool appendUniqueNoteOnInject(const NoteOnInject& ev,
                                         std::array<NoteOnInject, N>& events,
                                         int& count) noexcept
    {
        for (int i = 0; i < count; ++i)
        {
            if (matchNoteOnInjectKey(events[(size_t) i], ev.samplePos, ev.channel, ev.note))
                return true;
        }

        if (count >= (int) N)
            return false;

        events[(size_t) count++] = ev;
        return true;
    }

    //--------------------------------------------------------------------------
    // CONSUME: Collect NOTE keys that survived InputFilter (post-filter snapshot).
    // These keys are considered "used/consumed" by the lane.
    //--------------------------------------------------------------------------
    template <size_t N>
    static int collectConsumedNotes(const juce::MidiBuffer& laneAfterFilter,
                                    std::array<NoteKey, N>& outKeys,
                                    bool& overflow) noexcept
    {
        return collectUniqueNoteKeys(laneAfterFilter, outKeys, overflow);
    }

    //--------------------------------------------------------------------------
    // CONSUME: Build remainder = laneInput - consumed NOTE events.
    // Non-note messages are always preserved.
    //--------------------------------------------------------------------------
    template <size_t N>
    static void buildRemainder(const juce::MidiBuffer& laneInput,
                               const std::array<NoteKey, N>& consumedKeys,
                               int consumedCount,
                               bool consumedKeysOverflow,
                               const juce::MidiBuffer& consumedFallbackBuffer,
                               juce::MidiBuffer& outRemainder)
    {
        outRemainder.clear();

        for (auto it = laneInput.begin(); it != laneInput.end(); ++it)
        {
            const auto meta = *it;
            const auto& msg = meta.getMessage();
            const int samplePos = meta.samplePosition;

            bool isOn = false;
            if (!isNoteMsg(msg, isOn))
            {
                outRemainder.addEvent(msg, samplePos);
                continue;
            }

            const int ch = msg.getChannel();
            const int nt = msg.getNoteNumber();

            const NoteKey key { samplePos, ch, nt, isOn };
            bool consumed = containsNoteKey(consumedKeys, consumedCount, key);

            // Rare overflow fallback: preserve exact consume behavior even if
            // key-index capacity is exceeded under extreme bursts.
            if (!consumed && consumedKeysOverflow)
                consumed = bufferHasNoteKey(consumedFallbackBuffer, key);

            if (!consumed)
                outRemainder.addEvent(msg, samplePos);
        }
    }

    //--------------------------------------------------------------------------
    // CONSUME: Detect synthetic NOTE ON events produced by InputFilter that were
    // not present in laneInput. Those correspond to "stolen" voices when
    // priority/voice-limit reassigns ownership.
    //--------------------------------------------------------------------------
    template <size_t N, size_t M>
    static int collectSyntheticConsumedNoteOns(const juce::MidiBuffer& laneInput,
                                               const std::array<NoteKey, M>& laneInputKeys,
                                               int laneInputKeyCount,
                                               bool laneInputKeysOverflow,
                                               const juce::MidiBuffer& laneAfterFilter,
                                               std::array<NoteKey, N>& outKeys) noexcept
    {
        int count = 0;

        for (auto it = laneAfterFilter.begin(); it != laneAfterFilter.end(); ++it)
        {
            const auto meta = *it;
            const auto& msg = meta.getMessage();

            bool isOn = false;
            if (!isNoteMsg(msg, isOn) || !isOn)
                continue;

            const NoteKey key
            {
                meta.samplePosition,
                msg.getChannel(),
                msg.getNoteNumber(),
                true
            };

            bool existsInInput = containsNoteKey(laneInputKeys, laneInputKeyCount, key);
            if (!existsInInput && laneInputKeysOverflow)
                existsInInput = bufferHasNoteKey(laneInput, key);

            if (existsInInput)
                continue;

            if (!appendUniqueNoteKey(key, outKeys, count))
                break;
        }

        return count;
    }

    //--------------------------------------------------------------------------
    // CONSUME: Detect synthetic NOTE OFF events produced by InputFilter that were
    // not present in laneInput. Those correspond to internally released voices
    // that may need to be handed off to downstream lanes.
    //--------------------------------------------------------------------------
    template <size_t N, size_t M>
    static int collectSyntheticConsumedNoteOffs(const juce::MidiBuffer& laneInput,
                                                const std::array<NoteKey, M>& laneInputKeys,
                                                int laneInputKeyCount,
                                                bool laneInputKeysOverflow,
                                                const juce::MidiBuffer& laneAfterFilter,
                                                std::array<NoteKey, N>& outKeys) noexcept
    {
        int count = 0;

        for (auto it = laneAfterFilter.begin(); it != laneAfterFilter.end(); ++it)
        {
            const auto meta = *it;
            const auto& msg = meta.getMessage();

            bool isOn = false;
            if (!isNoteMsg(msg, isOn) || isOn)
                continue;

            const NoteKey key
            {
                meta.samplePosition,
                msg.getChannel(),
                msg.getNoteNumber(),
                false
            };

            bool existsInInput = containsNoteKey(laneInputKeys, laneInputKeyCount, key);
            if (!existsInInput && laneInputKeysOverflow)
                existsInInput = bufferHasNoteKey(laneInput, key);

            if (existsInInput)
                continue;

            if (!appendUniqueNoteKey(key, outKeys, count))
                break;
        }

        return count;
    }

    //--------------------------------------------------------------------------
    // Utility: append only non-note events from in to out.
    //--------------------------------------------------------------------------
    static void appendNonNoteEvents(const juce::MidiBuffer& in, juce::MidiBuffer& out)
    {
        for (auto it = in.begin(); it != in.end(); ++it)
        {
            const auto meta = *it;
            const auto& msg = meta.getMessage();

            bool isOn = false;
            if (isNoteMsg(msg, isOn))
                continue;

            out.addEvent(msg, meta.samplePosition);
        }
    }

    template <size_t Nch, size_t Nnt, size_t NvelDepth>
    static void updateHeldSnapshotFromBuffer(
        const juce::MidiBuffer& in,
        std::array<std::array<int, Nnt>, Nch>& heldCount,
        std::array<std::array<int, Nnt>, Nch>& heldVelocity,
        std::array<std::array<std::array<uint8_t, NvelDepth>, Nnt>, Nch>& heldVelocityStack,
        std::array<std::array<uint8_t, Nnt>, Nch>& heldVelocityStackSize,
        std::array<std::array<uint64_t, Nnt>, Nch>& heldOrder,
        uint64_t& orderCounter) noexcept
    {
        for (auto it = in.begin(); it != in.end(); ++it)
        {
            const auto meta = *it;
            const auto& msg = meta.getMessage();

            bool isOn = false;
            if (!isNoteMsg(msg, isOn))
                continue;

            const int chIndex = juce::jlimit(1, (int) Nch, msg.getChannel()) - 1;
            const int ntIndex = juce::jlimit(0, (int) Nnt - 1, msg.getNoteNumber());
            auto& velStack = heldVelocityStack[(size_t) chIndex][(size_t) ntIndex];
            auto& velStackSize = heldVelocityStackSize[(size_t) chIndex][(size_t) ntIndex];

            if (isOn)
            {
                const uint8_t vel = (uint8_t) velocityFloatToMidi127(msg.getVelocity());
                pushVelocityStack(velStack, velStackSize, vel);

                heldVelocity[(size_t) chIndex][(size_t) ntIndex] =
                    (int) topVelocityStack(velStack, velStackSize, vel);
                heldCount[(size_t) chIndex][(size_t) ntIndex] =
                    juce::jmax(0, heldCount[(size_t) chIndex][(size_t) ntIndex] + 1);
                heldOrder[(size_t) chIndex][(size_t) ntIndex] = ++orderCounter;
            }
            else
            {
                const int prev = heldCount[(size_t) chIndex][(size_t) ntIndex];
                const int next = juce::jmax(0, prev - 1);
                heldCount[(size_t) chIndex][(size_t) ntIndex] = next;

                if (velStackSize > 0)
                    --velStackSize;

                if (next == 0)
                {
                    heldVelocity[(size_t) chIndex][(size_t) ntIndex] = 0;
                    velStackSize = 0;
                    heldOrder[(size_t) chIndex][(size_t) ntIndex] = 0;
                }
                else if (velStackSize > 0)
                {
                    heldVelocity[(size_t) chIndex][(size_t) ntIndex] =
                        (int) velStack[(size_t) (velStackSize - 1)];
                }
            }
        }
    }

    template <size_t Nch, size_t Nnt>
    static bool updateHeldCountSnapshotFromBuffer(
        const juce::MidiBuffer& in,
        std::array<std::array<int, Nnt>, Nch>& heldCount) noexcept
    {
        bool sawNoteEvent = false;

        for (auto it = in.begin(); it != in.end(); ++it)
        {
            const auto meta = *it;
            const auto& msg = meta.getMessage();

            bool isOn = false;
            if (!isNoteMsg(msg, isOn))
                continue;

            sawNoteEvent = true;
            const int chIndex = juce::jlimit(1, (int) Nch, msg.getChannel()) - 1;
            const int ntIndex = juce::jlimit(0, (int) Nnt - 1, msg.getNoteNumber());

            if (isOn)
            {
                heldCount[(size_t) chIndex][(size_t) ntIndex] =
                    juce::jmax(0, heldCount[(size_t) chIndex][(size_t) ntIndex] + 1);
            }
            else
            {
                heldCount[(size_t) chIndex][(size_t) ntIndex] =
                    juce::jmax(0, heldCount[(size_t) chIndex][(size_t) ntIndex] - 1);
            }
        }

        return sawNoteEvent;
    }

    //--------------------------------------------------------------------------
    // DEDUP: Remove duplicated NOTE events within the block at the lane junction.
    //
    // Behavior:
    //   - Always keep non-note messages.
    //   - For note messages, keep the first occurrence of each NoteKey; drop repeats.
    //   - IMPORTANT anti-stuck rule:
    //       dropped duplicate NOTE ON still increments heldCounts so NOTE OFF
    //       release accounting can never underflow when quantization collapses
    //       several source notes onto one output note.
    //   - Track up to N unique NoteKeys; if exceeded, fail-open for the rest.
    //--------------------------------------------------------------------------
    template <size_t N, size_t Nch, size_t Nnt>
    static void dedupNoteEvents(const juce::MidiBuffer& in,
                               juce::MidiBuffer& out,
                               std::array<NoteKey, N>& seenKeys,
                               int& seenCount,
                               std::array<std::array<int, Nnt>, Nch>& heldCounts) noexcept
    {
        out.clear();
        seenCount = 0;

        bool trackingEnabled = true;

        auto updateHeld = [&](int ch1, int nt, bool isOn) noexcept
        {
            const int chIndex = juce::jlimit(1, (int) Nch, ch1) - 1;
            const int ntIndex = juce::jlimit(0, (int) Nnt - 1, nt);
            auto& count = heldCounts[(size_t) chIndex][(size_t) ntIndex];

            if (isOn)
                count = juce::jmax(0, count + 1);
            else
                count = juce::jmax(0, count - 1);
        };

        for (auto it = in.begin(); it != in.end(); ++it)
        {
            const auto meta = *it;
            const auto& msg = meta.getMessage();
            const int samplePos = meta.samplePosition;

            bool isOn = false;
            if (!isNoteMsg(msg, isOn))
            {
                out.addEvent(msg, samplePos);
                continue;
            }

            if (!trackingEnabled)
            {
                // Fail-open: we do not drop anything when tracking is disabled.
                out.addEvent(msg, samplePos);
                updateHeld(msg.getChannel(), msg.getNoteNumber(), isOn);
                continue;
            }

            const int ch = msg.getChannel();
            const int nt = msg.getNoteNumber();
            const int vel = isOn ? velocityFloatToMidi127(msg.getVelocity()) : 0;

            // Safety-first policy:
            // Never dedup NOTE OFF at the lane junction.
            // Redundant NOTE OFF is harmless; dropped NOTE OFF can create stuck
            // notes if upstream ownership/counters temporarily diverge.
            if (!isOn)
            {
                out.addEvent(msg, samplePos);
                updateHeld(ch, nt, false);
                continue;
            }

            bool alreadySeen = false;
            for (int i = 0; i < seenCount; ++i)
            {
                const auto& k = seenKeys[(size_t) i];
                if (k.samplePos == samplePos &&
                    k.channel   == ch &&
                    k.note      == nt &&
                    k.isOn      == isOn &&
                    (!isOn || k.velocity == vel))
                {
                    alreadySeen = true;
                    break;
                }
            }

            if (alreadySeen)
            {
                // Logical hold count is preserved even when dedup drops this
                // NOTE ON at the physical output.
                updateHeld(ch, nt, true);
                continue;
            }

            // Keep first occurrence.
            out.addEvent(msg, samplePos);
            updateHeld(ch, nt, isOn);

            // Track key if possible; otherwise disable tracking (fail-open).
            if (seenCount < (int) N)
            {
                seenKeys[(size_t) seenCount++] = NoteKey { samplePos, ch, nt, isOn, vel };
            }
            else
            {
                trackingEnabled = false;
            }
        }
    }

   #if LOGS_ENABLED
    //--------------------------------------------------------------------------
    // Utility: count events in a MidiBuffer (iterator-based, safe).
    //--------------------------------------------------------------------------
    static int countMidiEvents(const juce::MidiBuffer& buf) noexcept
    {
        int c = 0;
        for (auto it = buf.begin(); it != buf.end(); ++it)
            ++c;
        return c;
    }
   #endif
}

//==============================================================================
// Internal helper: monitors (Input/Output) -> FIFO
//==============================================================================

namespace
{
    // Keep monitor tapping intentionally tiny in RT:
    // UI can lag slightly, MIDI timing cannot.
    static constexpr int kInputMonitorWriteBudgetPerBlock = 8;
    static constexpr int kInputMonitorScanBudgetPerBlock = 32;
    static constexpr int kOutputMonitorWriteBudgetPerBlock = 12;
    static constexpr int kOutputMonitorScanBudgetPerLanePerBlock = 48;
    static constexpr int kMainBarMonitorTapMaxEventsPerBlock = 512;

    static inline bool pushMonitorMessage(juce::AbstractFifo& fifo,
                                          juce::MidiMessage* storage,
                                          const juce::MidiMessage& msg) noexcept
    {
        if (storage == nullptr)
            return false;

        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        fifo.prepareToWrite(1, start1, size1, start2, size2);

        if (size1 > 0)
        {
            storage[start1] = msg;
            fifo.finishedWrite(1);
            return true;
        }

        if (size2 > 0)
        {
            storage[start2] = msg;
            fifo.finishedWrite(1);
            return true;
        }

        fifo.finishedWrite(0);
        return false;
    }

    static inline bool pushMonitorMessage(juce::AbstractFifo& fifo,
                                          juce::MidiMessage* storage,
                                          std::uint8_t* sourceKinds,
                                          std::int8_t* sourceIndices,
                                          const juce::MidiMessage& msg,
                                          MonitorSourceKind sourceKind,
                                          int sourceIndex) noexcept
    {
        if (storage == nullptr || sourceKinds == nullptr || sourceIndices == nullptr)
            return false;

        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        fifo.prepareToWrite(1, start1, size1, start2, size2);

        if (size1 > 0)
        {
            storage[start1] = msg;
            sourceKinds[start1] = (std::uint8_t) sourceKind;
            sourceIndices[start1] = (std::int8_t) juce::jlimit(-1, 127, sourceIndex);
            fifo.finishedWrite(1);
            return true;
        }

        if (size2 > 0)
        {
            storage[start2] = msg;
            sourceKinds[start2] = (std::uint8_t) sourceKind;
            sourceIndices[start2] = (std::int8_t) juce::jlimit(-1, 127, sourceIndex);
            fifo.finishedWrite(1);
            return true;
        }

        fifo.finishedWrite(0);
        return false;
    }
}

//==============================================================================
// Ctor / dtor
//==============================================================================

MidivisiViciAudioProcessor::MidivisiViciAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
    : AudioProcessor(BusesProperties()
    #if !JucePlugin_IsMidiEffect
     #if !JucePlugin_IsSynth
        .withInput ("Input",  juce::AudioChannelSet::stereo(), true)
     #endif
        .withOutput("Output", juce::AudioChannelSet::stereo(), true)
    #endif
    )
#endif
    , parameters(*this,
                 nullptr,
                 juce::Identifier("PARAMETERS"),
                 createParameterLayout())
    , presetManager(parameters)
    , morphManager(parameters)
{
    presetManager.setMorphManager(&morphManager);

    presetManager.setOnGlobalAllNotesOff([this]
    {
        requestGlobalAllNotesOff();
    });

    presetManager.setOnSnapshotLaneCountCapture([this]
    {
        return getActiveLaneCount();
    });

    presetManager.setOnSnapshotLaneCountApply([this](int laneCount)
    {
        setActiveLaneCount(laneCount);
    });

    for (int i = 0; i < kNumLanes; ++i)
    {
        const auto lane = Lanes::fromIndex(i);
        inputFilterProcessors[i] = std::make_unique<InputFilterProcessor>(*this, parameters, lane);
        harmonizerProcessors[i]  = std::make_unique<HarmonizerProcessor>(parameters, lane);
        arpeggiatorProcessors[i] = std::make_unique<ArpeggiatorProcessor>(parameters, lane);
        splitterProcessors[i]    = std::make_unique<SplitterProcessor>(parameters, lane);

        for (int voice = 0; voice < 4; ++voice)
        {
            laneHarmVoiceOffsetDisplay[(size_t) i][(size_t) voice].store(0, std::memory_order_relaxed);
            laneHarmVelocityModDisplay[(size_t) i][(size_t) voice].store(0, std::memory_order_relaxed);
        }

        for (int morph = 0; morph < 4; ++morph)
            laneArpMorphDisplay[(size_t) i][(size_t) morph].store(0, std::memory_order_relaxed);

        for (int layer = 0; layer < 8; ++layer)
            laneArpPlaybackStepDisplay[(size_t) i][(size_t) layer].store(-1, std::memory_order_relaxed);
    }

    for (int page = 0; page < kMainBarMaxPages; ++page)
    {
        const int moduleIndex = page / kMainBarMorphsPerModule;
        const int morphIndex = page % kMainBarMorphsPerModule;

        auto& rtPage = mainBarPages[(size_t) page];
        rtPage.channel.store(juce::jlimit(1, 16, moduleIndex + 1), std::memory_order_relaxed);
        rtPage.ccNumber.store(juce::jlimit(0, 127, morphIndex + 1), std::memory_order_relaxed);
        rtPage.morphValue.store(50, std::memory_order_relaxed);

        for (int step = 0; step < kMainBarStepsPerPage; ++step)
        {
            rtPage.stepValues[(size_t) step].store(64, std::memory_order_relaxed);
            rtPage.stepRates[(size_t) step].store(step == 0 ? 5 : 0, std::memory_order_relaxed);
        }

        mainBarStepCursor[(size_t) page] = 0;
        mainBarNextStepPpq[(size_t) page] = 0.0;
        mainBarPageRandom[(size_t) page].setSeed((int64_t) (page + 1) * (int64_t) 0x4f1bbcdcbfa53e0bULL);
        mainBarPlaybackStep[(size_t) page].store(-1, std::memory_order_relaxed);
        mainBarMorphDisplay[(size_t) page].store(50, std::memory_order_relaxed);
    }

    for (auto& byCc : mainBarLastSentCcValue)
        byCc.fill(-1);

    mainBarRtPageCount.store(4 * kMainBarMorphsPerModule, std::memory_order_relaxed);
    mainBarWasPlaying = false;
    installSnapshotUiCallbacksFallback();

    refreshRtParameterPointerCache();
    buildArpeggiatorEquivalenceCache();
    clearChordSnapshotRt();

    recallAsync = std::make_unique<SnapshotRecallAsync>(presetManager, morphManager);

    syncMorphMirrorFromVTS();

   #if LOGS_ENABLED
    DBG_LOG("INIT", "PLUGINPROCESSOR", "CTOR", "#000#",
            juce::String("Processor constructed. Build timestamp: ")
            + juce::String(BUILD_TIMESTAMP));
   #endif

   #if JUCE_DEBUG
    DBG(juce::String("Build timestamp: ") + juce::String(BUILD_TIMESTAMP));
   #endif
}

MidivisiViciAudioProcessor::~MidivisiViciAudioProcessor() = default;

void MidivisiViciAudioProcessor::refreshRtParameterPointerCache() noexcept
{
    inputMonitorEnableRaw = parameters.getRawParameterValue(ParamIDs::Monitor::inputMonitorEnable);
    inputMonitorFilterNoteRaw = parameters.getRawParameterValue(ParamIDs::Monitor::inputMonitorFilterNote);
    inputMonitorFilterControlRaw = parameters.getRawParameterValue(ParamIDs::Monitor::inputMonitorFilterControl);
    inputMonitorFilterClockRaw = parameters.getRawParameterValue(ParamIDs::Monitor::inputMonitorFilterClock);
    inputMonitorFilterEventRaw = parameters.getRawParameterValue(ParamIDs::Monitor::inputMonitorFilterEvent);

    outputMonitorEnableRaw = parameters.getRawParameterValue(ParamIDs::Monitor::outputMonitorEnable);
    outputMonitorFilterNoteRaw = parameters.getRawParameterValue(ParamIDs::Monitor::outputMonitorFilterNote);
    outputMonitorFilterControlRaw = parameters.getRawParameterValue(ParamIDs::Monitor::outputMonitorFilterControl);
    outputMonitorFilterClockRaw = parameters.getRawParameterValue(ParamIDs::Monitor::outputMonitorFilterClock);
    outputMonitorFilterEventRaw = parameters.getRawParameterValue(ParamIDs::Monitor::outputMonitorFilterEvent);
    harmGlobalKeyRaw = parameters.getRawParameterValue(ParamIDs::Global::harmGlobalKey);
    harmGlobalScaleRaw = parameters.getRawParameterValue(ParamIDs::Global::harmGlobalScale);
    whiteInputModeRaw = parameters.getRawParameterValue(ParamIDs::Global::whiteInputModeToggle);
    blackInputModeRaw = parameters.getRawParameterValue(ParamIDs::Global::blackInputModeToggle);

    for (int i = 0; i < kNumLanes; ++i)
    {
        const auto lane = Lanes::fromIndex(i);
        laneInputFilterEnableRaw[(size_t) i] =
            parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::inputFilterEnable, lane));
        laneInputFilterConsumeRaw[(size_t) i] =
            parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::inputFilterConsume, lane));
        laneInputFilterDirectRaw[(size_t) i] =
            parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::inputFilterDirect, lane));
        laneInputFilterBlackNotesRaw[(size_t) i] =
            parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::inputFilterBlackNotes, lane));
        laneInputFilterStepFilterRaw[(size_t) i] =
            parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::stepFilterToggle, lane));
        laneHarmonizerEnableRaw[(size_t) i] =
            parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::harmonizerEnable, lane));
        laneHarmonizerPitchCorrectRaw[(size_t) i] =
            parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::harmPitchCorrector, lane));
        laneHarmonizerOctavePlusRaw[(size_t) i] =
            parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::harmOctavePlusRandom, lane));
        laneHarmonizerOctaveMinusRaw[(size_t) i] =
            parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::harmOctaveMinusRandom, lane));

        laneHarmonizerVoiceOffsetRaw[(size_t) i][0] =
            parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::harmVoice2, lane));
        laneHarmonizerVoiceOffsetRaw[(size_t) i][1] =
            parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::harmVoice3, lane));
        laneHarmonizerVoiceOffsetRaw[(size_t) i][2] =
            parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::harmVoice4, lane));
        laneHarmonizerVoiceOffsetRaw[(size_t) i][3] =
            parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::harmVoice5, lane));

        laneHarmonizerVelocityModRaw[(size_t) i][0] =
            parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::harmVoice2VelMod, lane));
        laneHarmonizerVelocityModRaw[(size_t) i][1] =
            parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::harmVoice3VelMod, lane));
        laneHarmonizerVelocityModRaw[(size_t) i][2] =
            parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::harmVoice4VelMod, lane));
        laneHarmonizerVelocityModRaw[(size_t) i][3] =
            parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::harmVoice5VelMod, lane));

        laneArpMorphRaw[(size_t) i][0] =
            parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::arpRateMorph, lane));
        laneArpMorphRaw[(size_t) i][1] =
            parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::arpGateMorph, lane));
        laneArpMorphRaw[(size_t) i][2] =
            parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::arpGrooveMorph, lane));
        laneArpMorphRaw[(size_t) i][3] =
            parameters.getRawParameterValue(ParamIDs::lane(ParamIDs::Base::arpVelocityMorph, lane));
    }
}

void MidivisiViciAudioProcessor::clearChordSnapshotRt() noexcept
{
    chordInputPitchClassMaskRt = 0;
    chordInputBassPitchClassRt = -1;
    chordOutputPitchClassMaskRt = 0;
    chordOutputBassPitchClassRt = -1;
    chordHarmonizerParamHashRt = 0;
    chordHarmonizerParamHashValidRt = false;

    uint32_t beginSeq = chordSnapshotRevisionRt + 1u;
    if ((beginSeq & 1u) == 0u)
        ++beginSeq;
    chordSnapshotRevisionRt = beginSeq;
    uiChordSnapshotRevision.store(beginSeq, std::memory_order_release);

    uiInputChordPitchClassMask.store(0, std::memory_order_relaxed);
    uiInputChordBassPitchClass.store(-1, std::memory_order_relaxed);
    uiOutputChordPitchClassMask.store(0, std::memory_order_relaxed);
    uiOutputChordBassPitchClass.store(-1, std::memory_order_relaxed);

    uint32_t endSeq = beginSeq + 1u;
    if ((endSeq & 1u) != 0u)
        ++endSeq;
    chordSnapshotRevisionRt = endSeq;
    uiChordSnapshotRevision.store(endSeq, std::memory_order_release);
}

//==============================================================================
// Metadata
//==============================================================================

const juce::String MidivisiViciAudioProcessor::getName() const { return JucePlugin_Name; }
bool MidivisiViciAudioProcessor::acceptsMidi()  const { return true; }
bool MidivisiViciAudioProcessor::producesMidi() const { return true; }
bool MidivisiViciAudioProcessor::isMidiEffect() const { return true; }
double MidivisiViciAudioProcessor::getTailLengthSeconds() const { return 0.0; }

int MidivisiViciAudioProcessor::getNumPrograms()
{
    return presetManager.getNumProgramEntriesRTSafe();
}
int MidivisiViciAudioProcessor::getCurrentProgram()
{
    return presetManager.getProgramIndexForAddressRTSafe(
        presetManager.getCurrentBank(),
        presetManager.getCurrentPreset(),
        presetManager.getCurrentSnapshot());
}

void MidivisiViciAudioProcessor::setCurrentProgram(int index)
{
    int bank = 0;
    int preset = 0;
    int snapshot = 0;

    if (!presetManager.getProgramAddressForIndexRTSafe(index, bank, preset, snapshot))
        return;

    const auto variant = presetManager.getCurrentVariant();

    // RT-safe selection updates.
    presetManager.setCurrentBankRT(bank);
    presetManager.setCurrentPresetRT(preset);
    presetManager.setCurrentSnapshotRT(snapshot);
    presetManager.setCurrentVariantRT(variant);

    // Program recall should not leave previous notes hanging.
    requestGlobalAllNotesOff();

    // Apply state on message thread via async recall bridge.
    if (recallAsync)
        recallAsync->requestRecall(bank, preset, snapshot, (int) variant);
}

const juce::String MidivisiViciAudioProcessor::getProgramName(int index)
{
    int bank = 0;
    int preset = 0;
    int snapshot = 0;

    if (!presetManager.getProgramAddressForIndexRTSafe(index, bank, preset, snapshot))
        return "Program 1";

    juce::String snapshotName = PresetManager::getSnapshotDisplayName(snapshot, PresetManager::Variant::X);

    // ValueTree-based names are queried only on message thread.
    if (auto* mm = juce::MessageManager::getInstanceWithoutCreating();
        mm != nullptr && mm->isThisTheMessageThread())
    {
        snapshotName = presetManager.getSnapshotName(bank, preset, snapshot, PresetManager::Variant::X);
    }

    return "B" + juce::String(bank + 1)
         + " P" + juce::String(preset + 1)
         + " S" + juce::String(snapshot + 1)
         + " : " + snapshotName;
}

void MidivisiViciAudioProcessor::changeProgramName(int index, const juce::String& newName)
{
    int bank = 0;
    int preset = 0;
    int snapshot = 0;

    if (!presetManager.getProgramAddressForIndexRTSafe(index, bank, preset, snapshot))
        return;

    // Program rename maps to snapshot rename (per program entry).
    juce::ignoreUnused(presetManager.renameSnapshot(bank, preset, snapshot, newName));
}

//==============================================================================
// Prepare / release
//==============================================================================

void MidivisiViciAudioProcessor::prepareToPlay(double /*sampleRate*/, int /*samplesPerBlock*/)
{
    refreshRtParameterPointerCache();

    // Monitors are diagnostic UI features.
    inputFifo.reset();
    outputFifo.reset();

    // Prime transport from host playhead state (no heavy work).
    transport.prime(getPlayHead());

    fallbackPpqAtNextBlock = 0.0;
    fallbackPpqValid = false;
    localClockWasActive = false;
    localClockPpqAtNextBlock = 0.0;
    lfoPhaseAnchorValid = false;
    lfoPhaseAnchorPpq = 0.0;
    mainBarWasPlaying = false;
    lastProcessedActiveLaneCount =
        juce::jlimit(1, kNumLanes, activeLaneCount.load(std::memory_order_relaxed));

    // Sync morph mirror with current VTS state (message thread safe here).
    syncMorphMirrorFromVTS();

    // Pre-allocate buffers to reduce RT growth risk.
    midiOutputBuffer.clear();      midiOutputBuffer.ensureSize(2048);
    swallowScratch.clear();        swallowScratch.ensureSize(2048);

    rawPlayableScratch.clear();    rawPlayableScratch.ensureSize(2048);
    whiteKeyRemapScratch.clear();  whiteKeyRemapScratch.ensureSize(2048);
    whiteInputOriginScratch.clear(); whiteInputOriginScratch.ensureSize(2048);
    blackInputOriginScratch.clear(); blackInputOriginScratch.ensureSize(2048);

    // Merge + dedup at the lane junction.
    mergedLanesScratch.clear();    mergedLanesScratch.ensureSize(4096);
    dedupedMergedScratch.clear();  dedupedMergedScratch.ensureSize(4096);

    for (int i = 0; i < kNumLanes; ++i)
    {
        laneSourceScratch[i].clear();      laneSourceScratch[i].ensureSize(2048);
        laneInputScratch[i].clear();       laneInputScratch[i].ensureSize(2048);
        laneAfterFilterScratch[i].clear(); laneAfterFilterScratch[i].ensureSize(2048);
        laneOutputScratch[i].clear();      laneOutputScratch[i].ensureSize(2048);
        laneRemainderScratch[i].clear();   laneRemainderScratch[i].ensureSize(2048);
        lastInputFilterEnabledState[(size_t) i] = false;
        lastDirectRoutingState[(size_t) i] = false;
        lastConsumeRoutingState[(size_t) i] = false;
        lastBlackNotesRoutingState[(size_t) i] = false;

        if (inputFilterProcessors[i])
            inputFilterProcessors[i]->resetAllTrackingRT();
        if (harmonizerProcessors[i])
            harmonizerProcessors[i]->resetAllTrackingRT();
        if (arpeggiatorProcessors[i])
            arpeggiatorProcessors[i]->resetAllTrackingRT();
        if (splitterProcessors[i])
            splitterProcessors[i]->resetAllTrackingRT();

        for (auto& byNote : laneInputHeldCount[(size_t) i])    byNote.fill(0);
        for (auto& byNote : laneInputHeldVelocity[(size_t) i]) byNote.fill(0);
        for (auto& byNote : laneInputHeldVelocityStack[(size_t) i])
            for (auto& byVel : byNote)
                byVel.fill(0);
        for (auto& byNote : laneInputHeldVelocityStackSize[(size_t) i]) byNote.fill(0);
        for (auto& byNote : laneInputHeldOrder[(size_t) i])    byNote.fill(0);
        laneInputHeldOrderCounter[(size_t) i] = 0;
    }

    for (auto& byNote : rawHeldCount)
        byNote.fill(0);
    for (auto& byNote : rawHeldVelocity)
        byNote.fill(0);
    for (auto& byNote : rawHeldVelocityStack)
        for (auto& byVel : byNote)
            byVel.fill(0);
    for (auto& byNote : rawHeldVelocityStackSize)
        byNote.fill(0);
    for (auto& byNote : rawHeldOrder)
        byNote.fill(0);
    rawHeldOrderCounter = 0;
    for (auto& byNote : outputHeldCount)
        byNote.fill(0);
    for (auto& byLane : harmonizerHeldCountByLane)
        for (auto& byNote : byLane)
            byNote.fill(0);
    for (auto& byNote : whiteModeHeldCount)
        byNote.fill(0);
    for (auto& byCh : whiteModeMappedNoteStack)
        for (auto& byNote : byCh)
            byNote.fill(0);
    for (auto& byCh : whiteModeMappedVelocityStack)
        for (auto& byNote : byCh)
            byNote.fill(0);
    for (auto& byCh : whiteModeMappedNoteStackSize)
        byCh.fill(0);
    for (auto& byCh : whiteModePhysicalHeldCount)
        byCh.fill(0);
    for (auto& byCh : whiteModePhysicalLastVelocity)
        byCh.fill(0);
    for (auto& byCh : blackModeMappedNoteStack)
        for (auto& byNote : byCh)
            byNote.fill(0);
    for (auto& byCh : blackModeMappedVelocityStack)
        for (auto& byNote : byCh)
            byNote.fill(0);
    for (auto& byCh : blackModeMappedNoteStackSize)
        byCh.fill(0);
    for (auto& byCh : blackModePhysicalHeldCount)
        byCh.fill(0);
    for (auto& byCh : blackModePhysicalLastVelocity)
        byCh.fill(0);
    for (auto& byCh : blackModeDeferredOffCount)
        byCh.fill(0);
    whiteModeChordSignature = 0;
    whiteModeChordSignatureValid = false;
    blackModeFiveClassSet = { 0, 2, 4, 7, 9 };
    blackModePendingSetUpdate = false;
    blackModePendingFiveClassSet = { 0, 2, 4, 7, 9 };
    lastDirectRoutingStateValid = false;
    lastWhiteInputModeState = false;
    lastBlackInputModeState = false;
    lastInputRemapStateValid = false;
    clearChordSnapshotRt();

    for (int page = 0; page < kMainBarMaxPages; ++page)
    {
        mainBarStepCursor[(size_t) page] = 0;
        mainBarNextStepPpq[(size_t) page] = 0.0;
        mainBarPageRandom[(size_t) page].setSeed((int64_t) (page + 1) * (int64_t) 0x4f1bbcdcbfa53e0bULL);
        mainBarPlaybackStep[(size_t) page].store(-1, std::memory_order_relaxed);
    }
    for (auto& byCc : mainBarLastSentCcValue)
        byCc.fill(-1);

   #if LOGS_ENABLED
    DBG_LOG("LIFECYCLE", "PLUGINPROCESSOR", "PREPARE", "#100#",
            "prepareToPlay: FIFOs reset, transport primed, morph synced, buffers reserved.");
   #endif
}

void MidivisiViciAudioProcessor::releaseResources()
{
   #if LOGS_ENABLED
    DBG_LOG("LIFECYCLE", "PLUGINPROCESSOR", "RELEASE", "#101#",
            "releaseResources: no manual cleanup required.");
   #endif
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool MidivisiViciAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
   #if JucePlugin_IsMidiEffect
    juce::ignoreUnused(layouts);
    return true;
   #else
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
   #endif
}
#endif

//==============================================================================
// processBlock (RT)
//==============================================================================

void MidivisiViciAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                             juce::MidiBuffer& midiMessages)
{
    //--------------------------------------------------------------------------
    // 0) MIDI-only effect: always clear audio.
    //--------------------------------------------------------------------------
    buffer.clear();

    //--------------------------------------------------------------------------
    // 0.1) One-shot global panic (requested by UI/message thread)
    //--------------------------------------------------------------------------
    if (pendingGlobalAllNotesOff.exchange(false, std::memory_order_acq_rel))
    {
        emitGlobalAllNotesOff(midiOutputBuffer, 0);
        resetAllNoteTrackingAfterPanic();
    }

    //--------------------------------------------------------------------------
    // 0.2) Global keyboard remap (MainMenu W/B), before monitor tap and routing
    //--------------------------------------------------------------------------
    const bool whiteModeEnabled =
        (whiteInputModeRaw != nullptr && whiteInputModeRaw->load(std::memory_order_relaxed) > 0.5f);
    bool blackModeEnabled =
        (blackInputModeRaw != nullptr && blackInputModeRaw->load(std::memory_order_relaxed) > 0.5f);

    if (!whiteModeEnabled)
        blackModeEnabled = false;

    int currentTonicPc = 0;
    if (harmGlobalKeyRaw != nullptr)
    {
        const int globalKey = juce::roundToInt(harmGlobalKeyRaw->load(std::memory_order_relaxed));
        currentTonicPc = (globalKey >= 0 && globalKey <= 11) ? globalKey : 0;
    }
    const int currentModeIndex = juce::jlimit(0, 9,
                                              harmGlobalScaleRaw != nullptr
                                                  ? juce::roundToInt(harmGlobalScaleRaw->load(std::memory_order_relaxed))
                                                  : 0);

    // Safety: remap topology change can invalidate pending note-off mapping
    // (eg W ON remap then W OFF before key release). Flush once on mode edges
    // to guarantee no stuck notes.
    if (lastInputRemapStateValid)
    {
        if (whiteModeEnabled != lastWhiteInputModeState
            || blackModeEnabled != lastBlackInputModeState)
        {
            emitGlobalAllNotesOff(midiOutputBuffer, 0);
            resetAllNoteTrackingAfterPanic();
        }
    }

    lastWhiteInputModeState = whiteModeEnabled;
    lastBlackInputModeState = blackModeEnabled;
    lastInputRemapStateValid = true;

    if (whiteModeEnabled)
    {
        const bool applyDeferredBlackSetUpdate =
            (blackModeEnabled && blackModePendingSetUpdate);
        const auto deferredBlackSet = blackModePendingFiveClassSet;
        if (applyDeferredBlackSetUpdate)
            blackModePendingSetUpdate = false;

        remapInputFromWhiteKeyboardMode(midiMessages,
                                        whiteKeyRemapScratch,
                                        whiteInputOriginScratch,
                                        blackInputOriginScratch,
                                        blackModeEnabled,
                                        currentTonicPc,
                                        currentModeIndex,
                                        whiteModeHeldCount,
                                        whiteModeMappedNoteStack,
                                        whiteModeMappedVelocityStack,
                                        whiteModeMappedNoteStackSize,
                                        whiteModePhysicalHeldCount,
                                        whiteModePhysicalLastVelocity,
                                        whiteModeChordSignature,
                                        whiteModeChordSignatureValid,
                                        blackModeFiveClassSet,
                                        blackModeMappedNoteStack,
                                        blackModeMappedVelocityStack,
                                        blackModeMappedNoteStackSize,
                                        blackModePhysicalHeldCount,
                                        blackModePhysicalLastVelocity,
                                        blackModeDeferredOffCount,
                                        applyDeferredBlackSetUpdate,
                                        deferredBlackSet,
                                        0);
        midiMessages.swapWith(whiteKeyRemapScratch);
    }
    else
    {
        whiteInputOriginScratch.clear();
        blackInputOriginScratch.clear();
        for (auto& byNote : whiteModeHeldCount)
            byNote.fill(0);
        for (auto& byCh : whiteModeMappedNoteStack)
            for (auto& byNote : byCh)
                byNote.fill(0);
        for (auto& byCh : whiteModeMappedVelocityStack)
            for (auto& byNote : byCh)
                byNote.fill(0);
        for (auto& byCh : whiteModeMappedNoteStackSize)
            byCh.fill(0);
        for (auto& byCh : whiteModePhysicalHeldCount)
            byCh.fill(0);
        for (auto& byCh : whiteModePhysicalLastVelocity)
            byCh.fill(0);
        for (auto& byCh : blackModeMappedNoteStack)
            for (auto& byNote : byCh)
                byNote.fill(0);
        for (auto& byCh : blackModeMappedVelocityStack)
            for (auto& byNote : byCh)
                byNote.fill(0);
        for (auto& byCh : blackModeMappedNoteStackSize)
            byCh.fill(0);
        for (auto& byCh : blackModePhysicalHeldCount)
            byCh.fill(0);
        for (auto& byCh : blackModePhysicalLastVelocity)
            byCh.fill(0);
        for (auto& byCh : blackModeDeferredOffCount)
            byCh.fill(0);
        whiteModeChordSignature = 0;
        whiteModeChordSignatureValid = false;
        blackModeFiveClassSet = { 0, 2, 4, 7, 9 };
        blackModePendingSetUpdate = false;
        blackModePendingFiveClassSet = { 0, 2, 4, 7, 9 };
    }

    //--------------------------------------------------------------------------
    // 1) Transport tick (RT-safe)
    //--------------------------------------------------------------------------
    transport.tick(getPlayHead());

    //--------------------------------------------------------------------------
    // 2) Host position -> atomics for UI extrapolation
    // Source priority:
    // - JUCE7 getPosition() optional fields when available
    // - transport edge helper for play/stop transitions
    // - local PPQ fallback while host omits position in running state
    //--------------------------------------------------------------------------
    double ppq = 0.0;
    double bpm = lastKnownBpm.load(std::memory_order_relaxed);
    bool playing = false;
    bool hasPpq = false;
    bool hasBpm = false;
    bool hasPlaying = false;

    if (auto* playHead = getPlayHead())
    {
       #if JUCE_MAJOR_VERSION >= 7
        if (auto posOpt = playHead->getPosition())
        {
            const auto& pos = *posOpt;

            if (auto ppqOpt = pos.getPpqPosition())
            {
                ppq = *ppqOpt;
                hasPpq = true;
            }

            if (auto bpmOpt = pos.getBpm())
            {
                bpm = *bpmOpt;
                hasBpm = true;
            }

            playing = pos.getIsPlaying();
            hasPlaying = true;
        }
       #else
        juce::AudioPlayHead::CurrentPositionInfo legacy {};
        if (playHead->getCurrentPosition(legacy))
        {
            ppq = legacy.ppqPosition;
            hasPpq = true;

            if (legacy.bpm > 0.0)
            {
                bpm = legacy.bpm;
                hasBpm = true;
            }

            playing = legacy.isPlaying;
            hasPlaying = true;
        }
       #endif
    }

    const bool transportPlaying = transport.isPlaying();
    const bool resolvedPlaying = (hasPlaying ? playing : transportPlaying);
    const bool hostJustStarted = transport.justStarted();
    const bool hostJustStopped = transport.justStopped();

    // Local note-driven clock while host transport is stopped:
    // - active when at least one raw input note is already held at block start,
    //   or when this block contains a new NOTE ON,
    // - fully disabled as soon as host transport is playing.
    // This avoids stopping a whole block too early when NOTE OFFs arrive later
    // within the same buffer.
    const bool rawHeldAtBlockStart = hasAnyHeldRawNote(rawHeldCount);
    const bool incomingHasRawNoteOn = hasAnyRawNoteOnInIncoming(midiMessages);
    const bool localClockActive = (!resolvedPlaying && (rawHeldAtBlockStart || incomingHasRawNoteOn));
    const bool localClockJustStarted = (localClockActive && !localClockWasActive);
    const bool localClockJustStopped = (!localClockActive && localClockWasActive);
    const bool effectivePlaying = resolvedPlaying || localClockActive;
    const bool effectiveJustStarted = hostJustStarted || localClockJustStarted;

    // Keep a sticky "last known BPM" for UI modules when host transport fields
    // are partially unavailable.
    if (hasBpm && bpm > 0.0)
        lastKnownBpm.store(bpm, std::memory_order_relaxed);

    const double safeBpmForTimeline = juce::jmax(1.0, bpm);
    const double safeRateForTimeline = juce::jmax(1.0, getSampleRate());
    const int blockSamples = juce::jmax(0, buffer.getNumSamples());
    const double blockBeats =
        (safeBpmForTimeline / 60.0) * ((double) blockSamples / safeRateForTimeline);

    if (hasPpq)
    {
        if (resolvedPlaying)
        {
            fallbackPpqAtNextBlock = ppq + blockBeats;
            fallbackPpqValid = true;
        }
        else
        {
            fallbackPpqValid = false;
        }
    }
    else if (resolvedPlaying)
    {
        if (fallbackPpqValid)
        {
            ppq = fallbackPpqAtNextBlock;
        }
        else
        {
            // Bootstrap from last UI-published position if host position is absent.
            ppq = uiPpqAtBlockStart.load(std::memory_order_relaxed);
            fallbackPpqValid = true;
        }

        fallbackPpqAtNextBlock = ppq + blockBeats;
    }
    else
    {
        fallbackPpqValid = false;
    }

    double effectivePpqAtBlockStart = ppq;
    if (resolvedPlaying)
    {
        localClockPpqAtNextBlock = 0.0;
    }
    else if (localClockActive)
    {
        if (localClockJustStarted)
            localClockPpqAtNextBlock = 0.0;

        effectivePpqAtBlockStart = localClockPpqAtNextBlock;
        localClockPpqAtNextBlock += blockBeats;
    }
    else
    {
        localClockPpqAtNextBlock = 0.0;
    }

    if (effectivePlaying)
    {
        uiPpqAtBlockStart.store(effectivePpqAtBlockStart, std::memory_order_relaxed);
        uiBeatsPerSec.store(safeBpmForTimeline / 60.0, std::memory_order_relaxed);
        uiBlockWallclock.store(juce::Time::getMillisecondCounterHiRes() * 0.001,
                               std::memory_order_relaxed);
    }

    uiIsPlaying.store(effectivePlaying, std::memory_order_relaxed);

    if (effectiveJustStarted || !lfoPhaseAnchorValid)
    {
        lfoPhaseAnchorPpq = effectivePpqAtBlockStart;
        lfoPhaseAnchorValid = true;
    }

    const double lfoPhasePpq = effectivePpqAtBlockStart - lfoPhaseAnchorPpq;

    const int activeLanes =
        juce::jlimit(1, kNumLanes, activeLaneCount.load(std::memory_order_relaxed));
    const int previousActiveLanes =
        juce::jlimit(1, kNumLanes, lastProcessedActiveLaneCount);
    const int mainBarPageCount =
        juce::jlimit(0, kMainBarMaxPages, mainBarRtPageCount.load(std::memory_order_relaxed));

    //--------------------------------------------------------------------------
    // 2.0) Active lane-count edges (dynamic add/remove lanes)
    //
    // Why:
    // - If lane count shrinks while notes are active, removed lanes are no longer
    //   processed and could leave stale note ownership unless flushed explicitly.
    // - If lane count grows, newly activated lanes must start from a clean state.
    //
    // RT contract:
    // - This branch executes only on lane-count changes (rare).
    // - Work is bounded by kNumLanes and uses preallocated storage only.
    //--------------------------------------------------------------------------
    if (activeLanes != previousActiveLanes)
    {
        const int laneResetFrom = juce::jmin(activeLanes, previousActiveLanes);
        const int laneResetTo   = juce::jmax(activeLanes, previousActiveLanes);

        // Removed lanes:
        // - Flush the full removed-lane chain once (InputFilter -> Harmonizer
        //   -> Arpeggiator -> Splitter) before resetting states.
        // - This is safer than flushing InputFilter alone because downstream
        //   modules can hold transformed/generated notes that need matching
        //   NOTE OFF on the effective output pitch/channel.
        if (activeLanes < previousActiveLanes)
        {
            ArpeggiatorProcessor::ProcessContext removedLaneArpFlushCtx;
            removedLaneArpFlushCtx.sampleRate = juce::jmax(1.0, getSampleRate());
            removedLaneArpFlushCtx.bpm = juce::jmax(1.0, bpm);
            removedLaneArpFlushCtx.ppqAtBlockStart = effectivePpqAtBlockStart;
            removedLaneArpFlushCtx.isPlaying = false;
            removedLaneArpFlushCtx.justStarted = false;
            removedLaneArpFlushCtx.numSamples = juce::jmax(0, buffer.getNumSamples());

            for (int i = activeLanes; i < previousActiveLanes; ++i)
            {
                laneOutputScratch[(size_t) i].clear();

                if (inputFilterProcessors[(size_t) i] != nullptr)
                    inputFilterProcessors[(size_t) i]->forceAllNotesOffAndClear(
                        laneOutputScratch[(size_t) i], 0, nullptr);

                if (harmonizerProcessors[(size_t) i] != nullptr)
                    harmonizerProcessors[(size_t) i]->process(laneOutputScratch[(size_t) i]);

                if (arpeggiatorProcessors[(size_t) i] != nullptr)
                    arpeggiatorProcessors[(size_t) i]->process(
                        laneOutputScratch[(size_t) i], removedLaneArpFlushCtx);

                if (splitterProcessors[(size_t) i] != nullptr)
                    splitterProcessors[(size_t) i]->process(laneOutputScratch[(size_t) i]);

                midiOutputBuffer.addEvents(laneOutputScratch[(size_t) i], 0, -1, 0);
            }
        }

        for (int i = laneResetFrom; i < laneResetTo; ++i)
        {
            if (inputFilterProcessors[(size_t) i] != nullptr)
                inputFilterProcessors[(size_t) i]->resetAllTrackingRT();
            if (harmonizerProcessors[(size_t) i] != nullptr)
                harmonizerProcessors[(size_t) i]->resetAllTrackingRT();
            if (arpeggiatorProcessors[(size_t) i] != nullptr)
                arpeggiatorProcessors[(size_t) i]->resetAllTrackingRT();
            if (splitterProcessors[(size_t) i] != nullptr)
                splitterProcessors[(size_t) i]->resetAllTrackingRT();

            laneSourceScratch[(size_t) i].clear();
            laneInputScratch[(size_t) i].clear();
            laneAfterFilterScratch[(size_t) i].clear();
            laneOutputScratch[(size_t) i].clear();
            laneRemainderScratch[(size_t) i].clear();

            for (auto& byNote : laneInputHeldCount[(size_t) i])    byNote.fill(0);
            for (auto& byNote : laneInputHeldVelocity[(size_t) i]) byNote.fill(0);
            for (auto& byNote : laneInputHeldVelocityStack[(size_t) i])
                for (auto& byVel : byNote)
                    byVel.fill(0);
            for (auto& byNote : laneInputHeldVelocityStackSize[(size_t) i]) byNote.fill(0);
            for (auto& byNote : laneInputHeldOrder[(size_t) i])    byNote.fill(0);
            laneInputHeldOrderCounter[(size_t) i] = 0;
            for (auto& byNote : harmonizerHeldCountByLane[(size_t) i]) byNote.fill(0);

            lastInputFilterEnabledState[(size_t) i] = false;
            lastDirectRoutingState[(size_t) i] = false;
            lastConsumeRoutingState[(size_t) i] = false;
        }

        // Force routing-topology edge recomputation on next stage.
        lastDirectRoutingStateValid = false;
        lastWhiteInputModeState = false;
        lastBlackInputModeState = false;
        lastInputRemapStateValid = false;
    }

    lastProcessedActiveLaneCount = activeLanes;

    //--------------------------------------------------------------------------
    // 2.1) Global LFO modulation (UI-configured, evaluated on audio thread)
    //
    // Contract:
    // - LFO settings are written by UI via atomics.
    // - Processor evaluates current waveform from block PPQ and injects
    //   normalized deltas into lane processors (no APVTS writeback).
    //--------------------------------------------------------------------------
    for (auto& laneMods : lfoLaneModScratch)
        laneMods = {};
    std::array<float, kMainBarMaxPages> lfoMainBarMorphNormScratch {};

    // UI preview helper:
    // apply a normalized modulation delta [-1..+1] on a linear integer range.
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
        return juce::jlimit(minValue, maxValue, juce::roundToInt(mapped));
    };

    const int lfoRowCount = juce::jlimit(0, kMaxLfoRows, lfoRtRowCount.load(std::memory_order_relaxed));

    if (effectivePlaying)
    {
        for (int rowIndex = 0; rowIndex < lfoRowCount; ++rowIndex)
        {
            const auto& row = lfoRtRows[(size_t) rowIndex];
            const int destinationCount = juce::jlimit(
                0, kMaxLfoDestinationsPerRow,
                row.destinationCount.load(std::memory_order_relaxed));
            if (destinationCount <= 0)
                continue;

            const int rateIndex = row.rateIndex.load(std::memory_order_relaxed);
            if (!juce::isPositiveAndBelow(rateIndex, (int) kLfoRateWholeNotes.size()))
                continue;

            const float depthNorm =
                juce::jlimit(-1.0f, 1.0f, (float) row.depth.load(std::memory_order_relaxed) / 100.0f);
            if (std::abs(depthNorm) <= 1.0e-6f)
                continue;

            // Offset UI [-100..100] -> phase relative [-1..1] cycle.
            // Lecture atomique lock-free: aucun lock/allocation sur le thread audio.
            const float offsetCycles =
                juce::jlimit(-1.0f, 1.0f, (float) row.offset.load(std::memory_order_relaxed) / 100.0f);
            const int waveShape = row.waveShape.load(std::memory_order_relaxed);

            const double factorWhole = kLfoRateWholeNotes[(size_t) rateIndex];
            const double periodBeats = factorWhole * 4.0;
            if (periodBeats <= 0.0)
                continue;

            double phase01 = std::fmod(lfoPhasePpq / periodBeats, 1.0);
            if (phase01 < 0.0)
                phase01 += 1.0;

            const float waveSample =
                evaluateLfoWaveSample(waveShape, (float) phase01, offsetCycles);
            const float modulationNorm = juce::jlimit(-1.0f, 1.0f, depthNorm * waveSample);

            for (int destinationIndex = 0; destinationIndex < destinationCount; ++destinationIndex)
            {
                const int destinationStableId =
                    row.destinationStableIds[(size_t) destinationIndex].load(std::memory_order_relaxed);
                if (destinationStableId == LfoDestinationIds::kNone)
                    continue;

                int mainBarPageIndex = -1;
                if (decodeLfoMainBarMorphDestinationStableId(destinationStableId, mainBarPageIndex))
                {
                    if (juce::isPositiveAndBelow(mainBarPageIndex, mainBarPageCount))
                        lfoMainBarMorphNormScratch[(size_t) mainBarPageIndex] += modulationNorm;
                    continue;
                }

                int laneIndex = -1;
                int slot = -1;
                if (!decodeLfoDestinationStableId(destinationStableId, laneIndex, slot))
                    continue;

                if (!juce::isPositiveAndBelow(laneIndex, activeLanes))
                    continue;

                auto& laneMods = lfoLaneModScratch[(size_t) laneIndex];

                switch (slot)
                {
                    case LfoDestinationIds::HarmPitch1:
                    case LfoDestinationIds::HarmPitch2:
                    case LfoDestinationIds::HarmPitch3:
                    case LfoDestinationIds::HarmPitch4:
                    {
                        const int voiceIndex = slot - LfoDestinationIds::HarmPitch1;
                        laneMods.harmPitchNormDeltas[(size_t) voiceIndex] += modulationNorm;
                        break;
                    }

                    case LfoDestinationIds::HarmVelMod1:
                    case LfoDestinationIds::HarmVelMod2:
                    case LfoDestinationIds::HarmVelMod3:
                    case LfoDestinationIds::HarmVelMod4:
                    {
                        const int voiceIndex = slot - LfoDestinationIds::HarmVelMod1;
                        laneMods.harmVelocityNormDeltas[(size_t) voiceIndex] += modulationNorm;
                        break;
                    }

                    case LfoDestinationIds::ArpRateMorph:
                        laneMods.arpRateMorphNormDelta += modulationNorm;
                        break;

                    case LfoDestinationIds::ArpGateMorph:
                        laneMods.arpGateMorphNormDelta += modulationNorm;
                        break;

                    case LfoDestinationIds::ArpGrooveMorph:
                        laneMods.arpGrooveMorphNormDelta += modulationNorm;
                        break;

                    case LfoDestinationIds::ArpVelocityMorph:
                        laneMods.arpVelocityMorphNormDelta += modulationNorm;
                        break;

                    default:
                        break;
                }
            }
        }
    }

    for (int laneIndex = 0; laneIndex < kNumLanes; ++laneIndex)
    {
        auto& laneMods = lfoLaneModScratch[(size_t) laneIndex];

        for (float& d : laneMods.harmPitchNormDeltas)
            d = juce::jlimit(-1.0f, 1.0f, d);
        for (float& d : laneMods.harmVelocityNormDeltas)
            d = juce::jlimit(-1.0f, 1.0f, d);

        laneMods.arpRateMorphNormDelta = juce::jlimit(-1.0f, 1.0f, laneMods.arpRateMorphNormDelta);
        laneMods.arpGateMorphNormDelta = juce::jlimit(-1.0f, 1.0f, laneMods.arpGateMorphNormDelta);
        laneMods.arpGrooveMorphNormDelta = juce::jlimit(-1.0f, 1.0f, laneMods.arpGrooveMorphNormDelta);
        laneMods.arpVelocityMorphNormDelta = juce::jlimit(-1.0f, 1.0f, laneMods.arpVelocityMorphNormDelta);

        // UI-only effective values (base parameter + current LFO block modulation).
        for (int voice = 0; voice < 4; ++voice)
        {
            const auto* rawVoiceOffset = laneHarmonizerVoiceOffsetRaw[(size_t) laneIndex][(size_t) voice];
            const int baseVoiceOffset = juce::jlimit(-24, 24,
                                                     juce::roundToInt(rawVoiceOffset != nullptr
                                                                          ? rawVoiceOffset->load(std::memory_order_relaxed)
                                                                          : 0.0f));
            const int effectiveVoiceOffset = applyNormDeltaLinearInt(baseVoiceOffset,
                                                                      -24, 24,
                                                                      laneMods.harmPitchNormDeltas[(size_t) voice]);
            laneHarmVoiceOffsetDisplay[(size_t) laneIndex][(size_t) voice]
                .store(effectiveVoiceOffset, std::memory_order_relaxed);

            const auto* rawVelocityMod = laneHarmonizerVelocityModRaw[(size_t) laneIndex][(size_t) voice];
            const int baseVelocityMod = juce::jlimit(-10, 10,
                                                     juce::roundToInt(rawVelocityMod != nullptr
                                                                          ? rawVelocityMod->load(std::memory_order_relaxed)
                                                                          : 0.0f));
            const int effectiveVelocityMod = applyNormDeltaLinearInt(baseVelocityMod,
                                                                      -10, 10,
                                                                      laneMods.harmVelocityNormDeltas[(size_t) voice]);
            laneHarmVelocityModDisplay[(size_t) laneIndex][(size_t) voice]
                .store(effectiveVelocityMod, std::memory_order_relaxed);
        }

        const float arpMorphDeltas[4] =
        {
            laneMods.arpRateMorphNormDelta,     // Rate
            laneMods.arpGateMorphNormDelta,     // Gate
            laneMods.arpGrooveMorphNormDelta,   // Groove
            laneMods.arpVelocityMorphNormDelta  // Velocity
        };

        for (int morph = 0; morph < 4; ++morph)
        {
            const auto* rawMorph = laneArpMorphRaw[(size_t) laneIndex][(size_t) morph];
            const int baseMorph = juce::jlimit(-100, 100,
                                               juce::roundToInt(rawMorph != nullptr
                                                                    ? rawMorph->load(std::memory_order_relaxed)
                                                                    : 0.0f));
            const int effectiveMorph = applyNormDeltaLinearInt(baseMorph,
                                                                -100, 100,
                                                                arpMorphDeltas[(size_t) morph]);
            laneArpMorphDisplay[(size_t) laneIndex][(size_t) morph]
                .store(effectiveMorph, std::memory_order_relaxed);
        }

        for (int layer = 0; layer < 8; ++layer)
            laneArpPlaybackStepDisplay[(size_t) laneIndex][(size_t) layer]
                .store(-1, std::memory_order_relaxed);

        if (harmonizerProcessors[(size_t) laneIndex] != nullptr)
        {
            HarmonizerProcessor::ExternalLfoModulations harmMod;
            harmMod.voiceOffsetNormDeltas = laneMods.harmPitchNormDeltas;
            harmMod.voiceVelocityNormDeltas = laneMods.harmVelocityNormDeltas;
            harmonizerProcessors[(size_t) laneIndex]->setExternalLfoModulations(harmMod);
        }

        if (arpeggiatorProcessors[(size_t) laneIndex] != nullptr)
        {
            ArpeggiatorProcessor::ExternalMorphModulations arpMod;
            arpMod.normDeltas[0] = laneMods.arpRateMorphNormDelta;     // arpRateMorph
            arpMod.normDeltas[4] = laneMods.arpVelocityMorphNormDelta; // arpVelocityMorph
            arpMod.normDeltas[5] = laneMods.arpGrooveMorphNormDelta;   // arpGrooveMorph
            arpMod.normDeltas[6] = laneMods.arpGateMorphNormDelta;     // arpGateMorph
            arpeggiatorProcessors[(size_t) laneIndex]->setExternalMorphModulations(arpMod);
        }
    }

    for (int page = 0; page < mainBarPageCount; ++page)
        lfoMainBarMorphNormScratch[(size_t) page] =
            juce::jlimit(-1.0f, 1.0f, lfoMainBarMorphNormScratch[(size_t) page]);

    //--------------------------------------------------------------------------
    // 2.1) Transport edges -> notify lane processors (RT-safe contract)
    //--------------------------------------------------------------------------
    if (hostJustStarted)
    {
        for (int i = 0; i < activeLanes; ++i)
            if (inputFilterProcessors[i])
                inputFilterProcessors[i]->onPlaybackStarted();
    }
    else if (hostJustStopped)
    {
        for (int i = 0; i < activeLanes; ++i)
            if (inputFilterProcessors[i])
                inputFilterProcessors[i]->onPlaybackStopped();
    }

    if (effectiveJustStarted)
        uiPlayJustStarted.store(true, std::memory_order_release);

    // Local note-driven clock just stopped (all keys released while DAW stopped):
    // emit a strict safety All Notes Off.
    if (localClockJustStopped && !resolvedPlaying)
        emitGlobalAllNotesOff(midiOutputBuffer, 0);

    //--------------------------------------------------------------------------
    // 2.2) MainBar centralized CC sequencer (RT-safe)
    //--------------------------------------------------------------------------
    const double safeMainBarRate = juce::jmax(1.0, getSampleRate());
    const double safeMainBarBpm = juce::jmax(1.0, bpm);
    const double mainBarBeatsPerSample =
        juce::jmax(1.0e-12, (safeMainBarBpm / 60.0) / safeMainBarRate);
    const double mainBarBlockStartPpq = effectivePpqAtBlockStart;
    const double mainBarBlockEndPpq =
        mainBarBlockStartPpq + ((double) blockSamples * mainBarBeatsPerSample);
    struct MainBarMonitorTapEvent
    {
        juce::MidiMessage message;
        int controllerModuleIndex = -1;
    };
    std::array<MainBarMonitorTapEvent, kMainBarMonitorTapMaxEventsPerBlock> mainBarMonitorTapEvents {};
    int mainBarMonitorTapCount = 0;

    auto runMainBarCcSequencerRt = [&]()
    {
        if (!effectivePlaying)
        {
            mainBarWasPlaying = false;
            mainBarMonitorTapCount = 0;
            for (int page = 0; page < mainBarPageCount; ++page)
            {
                mainBarPlaybackStep[(size_t) page].store(-1, std::memory_order_relaxed);
                const int baseMorphValue = juce::jlimit(0, 100,
                                                        mainBarPages[(size_t) page].morphValue.load(std::memory_order_relaxed));
                mainBarMorphDisplay[(size_t) page].store(baseMorphValue, std::memory_order_relaxed);
            }
            for (auto& byCc : mainBarLastSentCcValue)
                byCc.fill(-1);
            for (int page = mainBarPageCount; page < kMainBarMaxPages; ++page)
                mainBarMorphDisplay[(size_t) page].store(50, std::memory_order_relaxed);
            return;
        }

        if (effectiveJustStarted || !mainBarWasPlaying)
        {
            for (int page = 0; page < mainBarPageCount; ++page)
            {
                mainBarStepCursor[(size_t) page] = 0;
                mainBarNextStepPpq[(size_t) page] = mainBarBlockStartPpq;
                mainBarPlaybackStep[(size_t) page].store(-1, std::memory_order_relaxed);
            }
        }

        mainBarWasPlaying = true;

        for (int page = 0; page < mainBarPageCount; ++page)
        {
            auto& rtPage = mainBarPages[(size_t) page];
            const int baseMorphValue =
                juce::jlimit(0, 100, rtPage.morphValue.load(std::memory_order_relaxed));
            const float baseMorphNorm =
                juce::jlimit(0.0f, 1.0f, (float) baseMorphValue / 100.0f);
            const float lfoDeltaNorm = lfoMainBarMorphNormScratch[(size_t) page];
            const int effectiveMorphValue = juce::jlimit(
                0, 100, juce::roundToInt((baseMorphNorm + lfoDeltaNorm) * 100.0f));
            mainBarMorphDisplay[(size_t) page].store(effectiveMorphValue, std::memory_order_relaxed);

            // Keep sequence in-range after large transport jumps.
            if (mainBarNextStepPpq[(size_t) page] < (mainBarBlockStartPpq - 32.0))
                mainBarNextStepPpq[(size_t) page] = mainBarBlockStartPpq;

            int iterationBudget = 256;
            while (mainBarNextStepPpq[(size_t) page] < mainBarBlockEndPpq && --iterationBudget > 0)
            {
                int stepIndex = mainBarStepCursor[(size_t) page];
                if (!juce::isPositiveAndBelow(stepIndex, kMainBarStepsPerPage))
                    stepIndex = 0;

                int rateChoice = rtPage.stepRates[(size_t) stepIndex].load(std::memory_order_relaxed);
                if (stepIndex == 0 && rateChoice == 0)
                    rateChoice = 5;

                // Skip has no duration:
                // jump directly to the next non-skip step in this page.
                if (rateChoice == 0)
                {
                    int scanned = 0;
                    int probe = stepIndex;
                    int probeChoice = 0;
                    while (scanned < kMainBarStepsPerPage)
                    {
                        probe = (probe + 1) % kMainBarStepsPerPage;
                        probeChoice = rtPage.stepRates[(size_t) probe].load(std::memory_order_relaxed);
                        if (probe == 0 && probeChoice == 0)
                            probeChoice = 5;
                        ++scanned;
                        if (probeChoice != 0)
                            break;
                    }

                    // Defensive fallback if all steps are effectively skip:
                    // keep clock progressing so the page cannot hard-stall.
                    if (probeChoice == 0)
                    {
                        mainBarNextStepPpq[(size_t) page] += 0.25; // quarter-note fallback
                        mainBarPlaybackStep[(size_t) page].store(-1, std::memory_order_relaxed);
                        mainBarStepCursor[(size_t) page] = 0;
                        continue;
                    }

                    // Resume this same logical tick on the next non-skip step.
                    stepIndex = probe;
                    rateChoice = probeChoice;
                    mainBarStepCursor[(size_t) page] = stepIndex;
                }

                int resolvedRateChoice = rateChoice;
                if (resolvedRateChoice == kMainBarRateRndChoice)
                    resolvedRateChoice = 1 + mainBarPageRandom[(size_t) page].nextInt(kMainBarRateChoiceCount);

                const double stepDurationBeats = juce::jmax(1.0e-6,
                                                            beatsForMainBarRateChoice(resolvedRateChoice));
                const double eventPpq = mainBarNextStepPpq[(size_t) page];

                if (eventPpq >= mainBarBlockStartPpq && eventPpq < mainBarBlockEndPpq)
                {
                    const int channel =
                        juce::jlimit(1, 16, rtPage.channel.load(std::memory_order_relaxed));
                    const int ccNumber =
                        juce::jlimit(0, 127, rtPage.ccNumber.load(std::memory_order_relaxed));
                    const int stepValue =
                        juce::jlimit(0, 127, rtPage.stepValues[(size_t) stepIndex].load(std::memory_order_relaxed));
                    const int outValue = applyMainBarMorphToValue(stepValue, effectiveMorphValue);

                    int samplePos = juce::roundToInt((eventPpq - mainBarBlockStartPpq) / mainBarBeatsPerSample);
                    if (blockSamples > 0)
                        samplePos = juce::jlimit(0, blockSamples - 1, samplePos);
                    else
                        samplePos = 0;

                    auto& lastByCc = mainBarLastSentCcValue[(size_t) (channel - 1)];
                    if (lastByCc[(size_t) ccNumber] != outValue)
                    {
                        const juce::MidiMessage ccMsg =
                            juce::MidiMessage::controllerEvent(channel, ccNumber, outValue);
                        midiOutputBuffer.addEvent(ccMsg, samplePos);
                        lastByCc[(size_t) ccNumber] = outValue;

                        if (mainBarMonitorTapCount < kMainBarMonitorTapMaxEventsPerBlock)
                        {
                            auto& tap = mainBarMonitorTapEvents[(size_t) mainBarMonitorTapCount++];
                            tap.message = ccMsg;
                            tap.controllerModuleIndex =
                                juce::jlimit(0, Lanes::kNumLanes - 1, page / kMainBarMorphsPerModule);
                        }
                    }

                    mainBarPlaybackStep[(size_t) page].store(stepIndex, std::memory_order_relaxed);
                }

                mainBarNextStepPpq[(size_t) page] = eventPpq + stepDurationBeats;
                mainBarStepCursor[(size_t) page] = (stepIndex + 1) % kMainBarStepsPerPage;
            }

            // Safety: if a page hit the iteration limit, move it to block end
            // so it cannot spin forever across blocks.
            if (iterationBudget <= 0 && mainBarNextStepPpq[(size_t) page] < mainBarBlockEndPpq)
                mainBarNextStepPpq[(size_t) page] = mainBarBlockEndPpq;
        }

        for (int page = mainBarPageCount; page < kMainBarMaxPages; ++page)
            mainBarMorphDisplay[(size_t) page].store(50, std::memory_order_relaxed);
    };

    //--------------------------------------------------------------------------
    // 3) Preset control parsing (RT) + async recall/persist
    //--------------------------------------------------------------------------
    bool postedPersist = false;

    for (auto it = midiMessages.begin(); it != midiMessages.end(); ++it)
    {
        const auto meta = *it;
        const auto m = meta.getMessage();

        if (m.isController())
        {
            const int cc  = m.getControllerNumber();
            const int val = m.getControllerValue();

            if (cc == 0)
            {
                presetManager.setBankFromMsbRT(val);
                postedPersist = true;
            }
            else if (cc == 32)
            {
                presetManager.setPresetFromLsbRT(val);
                postedPersist = true;
            }
            else if (cc == 1)
            {
                zControllerValue.store(val, std::memory_order_relaxed);
            }
        }
        else if (m.isProgramChange())
        {
            const int pc0 = m.getProgramChangeNumber();
            const int pc1 = pc0 + 1;

            presetManager.selectSnapshotFromProgramChangeRT(pc1);

            const int b = presetManager.getCurrentBank();
            const int p = presetManager.getCurrentPreset();
            const int s = presetManager.getCurrentSnapshot();
            const int v = (int) presetManager.getCurrentVariant();

            if (recallAsync)
                recallAsync->requestRecall(b, p, s, v);

            postedPersist = false;
        }
    }

    if (postedPersist && recallAsync)
        recallAsync->triggerPersistOnly();

    //--------------------------------------------------------------------------
    // 4) Apply morph from global Z (RT-safe)
    //--------------------------------------------------------------------------
    const int zValue = zControllerValue.load(std::memory_order_relaxed);
    const float zNorm = (float) zValue / 127.0f;
    morphManager.applyMorph(zNorm);

    //--------------------------------------------------------------------------
    // 5) Swallow plugin-control messages BEFORE lane processing
    //   - CC#0, CC#32, ProgramChange are consumed by plugin (not forwarded)
    //--------------------------------------------------------------------------
    swallowScratch.clear();

    for (auto it = midiMessages.begin(); it != midiMessages.end(); ++it)
    {
        const auto meta = *it;
        const auto m = meta.getMessage();

        const bool swallow =
            (m.isController() && (m.getControllerNumber() == 0 || m.getControllerNumber() == 32)) ||
            m.isProgramChange();

        if (!swallow)
            swallowScratch.addEvent(m, meta.samplePosition);
    }

    // rawPlayableScratch is the canonical playable stream for this block.
    rawPlayableScratch.swapWith(swallowScratch);

    // Track physical note state from raw playable stream.
    bool chordInputNoteEventSeen = false;
    bool chordInputNoteOnSeen = false;
    for (auto it = rawPlayableScratch.begin(); it != rawPlayableScratch.end(); ++it)
    {
        const auto meta = *it;
        const auto& msg = meta.getMessage();

        const int ch = juce::jlimit(1, 16, msg.getChannel()) - 1;
        const int nt = juce::jlimit(0, 127, msg.getNoteNumber());

        const bool isNoteOn = msg.isNoteOn() && msg.getVelocity() > 0.0f;
        const bool isNoteOff = msg.isNoteOff() || (msg.isNoteOn() && msg.getVelocity() == 0.0f);

        if (isNoteOn)
        {
            chordInputNoteEventSeen = true;
            chordInputNoteOnSeen = true;
            const uint8_t vel = (uint8_t) velocityFloatToMidi127(msg.getVelocity());
            auto& velStack = rawHeldVelocityStack[(size_t) ch][(size_t) nt];
            auto& velStackSize = rawHeldVelocityStackSize[(size_t) ch][(size_t) nt];
            pushVelocityStack(velStack, velStackSize, vel);

            rawHeldVelocity[(size_t) ch][(size_t) nt] =
                (int) topVelocityStack(velStack, velStackSize, vel);
            rawHeldCount[(size_t) ch][(size_t) nt] += 1;
            rawHeldOrder[(size_t) ch][(size_t) nt] = ++rawHeldOrderCounter;
        }
        else if (isNoteOff)
        {
            chordInputNoteEventSeen = true;
            const int prev = rawHeldCount[(size_t) ch][(size_t) nt];
            const int next = juce::jmax(0, prev - 1);
            rawHeldCount[(size_t) ch][(size_t) nt] = next;

            auto& velStack = rawHeldVelocityStack[(size_t) ch][(size_t) nt];
            auto& velStackSize = rawHeldVelocityStackSize[(size_t) ch][(size_t) nt];

            if (velStackSize > 0)
                --velStackSize;

            if (next == 0)
            {
                rawHeldVelocity[(size_t) ch][(size_t) nt] = 0;
                velStackSize = 0;
                rawHeldOrder[(size_t) ch][(size_t) nt] = 0;
            }
            else if (velStackSize > 0)
            {
                rawHeldVelocity[(size_t) ch][(size_t) nt] =
                    (int) velStack[(size_t) (velStackSize - 1)];
            }
        }
    }

    //--------------------------------------------------------------------------
    // 6) Lane routing with optional consume
    //
    // Effective consume policy:
    // - consume parameter is ignored while step filter is ON for the same lane.
    // - This mirrors UI lock semantics and prevents ambiguous routing states
    //   when users/automation edit parameters live.
    //--------------------------------------------------------------------------
    mergedLanesScratch.clear();

    bool consumeEnabled[kNumLanes] {};
    bool directEnabled[kNumLanes] {};
    bool blackNotesEnabled[kNumLanes] {};
    bool laneFilterEnabled[kNumLanes] {};
    bool routingSourceResetNeeded[kNumLanes] {};

    constexpr int kBlackOriginKeyCapacity = 1024;
    std::array<NoteKey, kBlackOriginKeyCapacity> blackOriginKeys {};
    std::array<uint16_t, kBlackOriginKeyCapacity> blackOriginKeyMultiplicity {};
    bool blackOriginKeysOverflow = false;
    int blackOriginKeyCount = 0;
    const bool laneBlackGateActive = whiteModeEnabled && blackModeEnabled;

    if (laneBlackGateActive)
    {
        blackOriginKeyCount =
            collectNoteKeyMultiplicity(blackInputOriginScratch,
                                       blackOriginKeys,
                                       blackOriginKeyMultiplicity,
                                       blackOriginKeysOverflow);
    }

    for (int i = 0; i < activeLanes; ++i)
    {
        const auto* pEnable = laneInputFilterEnableRaw[(size_t) i];
        const auto* pConsume = laneInputFilterConsumeRaw[(size_t) i];
        const auto* pDirect = laneInputFilterDirectRaw[(size_t) i];
        const auto* pBlackNotes = laneInputFilterBlackNotesRaw[(size_t) i];
        const auto* pStepFilter = laneInputFilterStepFilterRaw[(size_t) i];

        laneFilterEnabled[i] = (pEnable != nullptr && pEnable->load() > 0.5f);
        const bool stepFilterOn = (pStepFilter != nullptr && pStepFilter->load() > 0.5f);
        const bool consumeRequested = (pConsume != nullptr && pConsume->load() > 0.5f);
        consumeEnabled[i] = (consumeRequested && !stepFilterOn);
        directEnabled[i] = (pDirect != nullptr && pDirect->load() > 0.5f);
        blackNotesEnabled[i] = (pBlackNotes != nullptr && pBlackNotes->load() > 0.5f);
    }

    // Detect routing topology edges and mark impacted lanes.
    // Impact rules:
    // - A direct toggle at lane i can change source identity for lane i and
    //   downstream lanes until the next explicit direct boundary.
    // - A consume toggle at lane i can change source identity from lane i+1
    //   and downstream lanes until the next explicit direct boundary.
    // - A black-notes source toggle at lane i changes lane i source selection
    //   (white-only vs black-only) and can affect downstream lanes.
    if (lastDirectRoutingStateValid)
    {
        for (int i = 0; i < activeLanes; ++i)
        {
            if (lastDirectRoutingState[(size_t) i] != directEnabled[i])
            {
                for (int j = i; j < activeLanes; ++j)
                {
                    if (j > i && directEnabled[j])
                        break;

                    routingSourceResetNeeded[j] = true;
                }
            }

            if (lastConsumeRoutingState[(size_t) i] != consumeEnabled[i])
            {
                for (int j = i + 1; j < activeLanes; ++j)
                {
                    if (directEnabled[j])
                        break;

                    routingSourceResetNeeded[j] = true;
                }
            }

            const bool blackSourceEnabled = laneBlackGateActive && blackNotesEnabled[i];
            if (lastBlackNotesRoutingState[(size_t) i] != blackSourceEnabled)
            {
                for (int j = i; j < activeLanes; ++j)
                {
                    if (j > i && directEnabled[j])
                        break;

                    routingSourceResetNeeded[j] = true;
                }
            }
        }
    }

    for (int i = 0; i < kNumLanes; ++i)
    {
        lastDirectRoutingState[(size_t) i] = (i < activeLanes) ? directEnabled[i] : false;
        lastConsumeRoutingState[(size_t) i] = (i < activeLanes) ? consumeEnabled[i] : false;
        lastBlackNotesRoutingState[(size_t) i] =
            (i < activeLanes) ? (laneBlackGateActive && blackNotesEnabled[i]) : false;
    }

    lastDirectRoutingStateValid = true;

    auto clearLaneInputHeldSnapshot = [this](int laneIndex) noexcept
    {
        for (auto& byNote : laneInputHeldCount[(size_t) laneIndex])    byNote.fill(0);
        for (auto& byNote : laneInputHeldVelocity[(size_t) laneIndex]) byNote.fill(0);
        for (auto& byNote : laneInputHeldVelocityStack[(size_t) laneIndex])
            for (auto& byVel : byNote)
                byVel.fill(0);
        for (auto& byNote : laneInputHeldVelocityStackSize[(size_t) laneIndex]) byNote.fill(0);
        for (auto& byNote : laneInputHeldOrder[(size_t) laneIndex])    byNote.fill(0);
        laneInputHeldOrderCounter[(size_t) laneIndex] = 0;
    };

    auto copyRawHeldSnapshotToLane = [this](int laneIndex) noexcept
    {
        laneInputHeldCount[(size_t) laneIndex] = rawHeldCount;
        laneInputHeldVelocity[(size_t) laneIndex] = rawHeldVelocity;
        laneInputHeldVelocityStack[(size_t) laneIndex] = rawHeldVelocityStack;
        laneInputHeldVelocityStackSize[(size_t) laneIndex] = rawHeldVelocityStackSize;
        laneInputHeldOrder[(size_t) laneIndex] = rawHeldOrder;
        laneInputHeldOrderCounter[(size_t) laneIndex] = rawHeldOrderCounter;
    };

    auto clearHarmonizerHeldSnapshotForLane = [this](int laneIndex) noexcept
    {
        for (auto& byNote : harmonizerHeldCountByLane[(size_t) laneIndex])
            byNote.fill(0);
    };

    // Housekeeping NOTE OFF keys to inject into downstream lanes when an upstream
    // lane steals a note internally (voice-limit/priority reassignment).
    constexpr int kHousekeepingKeyCapacity = 512;
    std::array<std::array<NoteKey, kHousekeepingKeyCapacity>, kNumLanes> pendingStealOffKeys {};
    int pendingStealOffCount[kNumLanes] {};

    // Handoff NOTE ON events injected when an upstream lane releases an internal
    // voice that is still physically held (consume chain re-assignment).
    constexpr int kHandoffOnCapacity = 512;
    std::array<std::array<NoteOnInject, kHandoffOnCapacity>, kNumLanes> pendingHandoffOns {};
    int pendingHandoffOnCount[kNumLanes] {};

    // Capture each lane arpeggiator clock state at block start. If two lanes
    // have identical arpeggiator settings, we can phase-lock their clocks
    // from this snapshot to prevent unintentional drift.
    preBlockArpSyncValid.fill(false);

    for (int i = 0; i < activeLanes; ++i)
    {
        if (arpeggiatorProcessors[i] == nullptr)
            continue;

        arpeggiatorProcessors[i]->getClockSyncState(preBlockArpSyncScratch[(size_t) i]);
        preBlockArpSyncValid[(size_t) i] = preBlockArpSyncScratch[(size_t) i].valid;
    }

    bool chordOutputNoteEventSeen = false;

    for (int i = 0; i < activeLanes; ++i)
    {
        const auto lane = Lanes::fromIndex(i);
        const bool laneUsesBlackSource = laneBlackGateActive && blackNotesEnabled[i];

        const bool laneInputFilterEnabled = laneFilterEnabled[i];
        const bool sourceResetThisLane = routingSourceResetNeeded[i];
        const bool sourceResetToRawDirect = sourceResetThisLane && directEnabled[i];

        //----------------------------------------------------------------------
        // 7.1) Resolve lane source according to consume topology
        //
        // Rule:
        //   - lane 0 source = raw
        //   - lane i direct ON -> lane i source = raw
        //   - prev consume ON  -> source = prev remainder (serial)
        //   - prev consume OFF -> source = prev source (parallel)
        //----------------------------------------------------------------------
        laneSourceScratch[i].clear();

        if (i == 0 || directEnabled[i])
        {
            laneSourceScratch[i].addEvents(rawPlayableScratch, 0, -1, 0);
        }
        else if (consumeEnabled[i - 1])
        {
            laneSourceScratch[i].addEvents(laneRemainderScratch[i - 1], 0, -1, 0);
        }
        else
        {
            laneSourceScratch[i].addEvents(laneSourceScratch[i - 1], 0, -1, 0);
        }

        // laneInputScratch starts from source, then receives lane-specific
        // housekeeping injections.
        laneInputScratch[i].clear();
        if (!laneBlackGateActive)
        {
            laneInputScratch[i].addEvents(laneSourceScratch[i], 0, -1, 0);
        }
        else
        {
            const bool blackSourceOnly = blackNotesEnabled[i];
            auto blackOriginRemaining = blackOriginKeyMultiplicity;
            auto laneGateHeld = laneInputHeldCount[(size_t) i];
            for (auto it = laneSourceScratch[i].begin(); it != laneSourceScratch[i].end(); ++it)
            {
                const auto meta = *it;
                const auto& msg = meta.getMessage();
                const int samplePos = meta.samplePosition;

                bool isOn = false;
                if (isNoteMsg(msg, isOn))
                {
                    const NoteKey key
                    {
                        samplePos,
                        msg.getChannel(),
                        msg.getNoteNumber(),
                        isOn,
                        isOn ? velocityFloatToMidi127(msg.getVelocity()) : 0
                    };

                    bool isBlackOrigin = false;
                    const int originIndex =
                        findNoteKeyWithVelocityIndex(blackOriginKeys, blackOriginKeyCount, key);
                    if (originIndex >= 0)
                    {
                        auto& remaining = blackOriginRemaining[(size_t) originIndex];
                        if (remaining > 0)
                        {
                            isBlackOrigin = true;
                            --remaining;
                        }
                    }

                    if (!isBlackOrigin && blackOriginKeysOverflow)
                        isBlackOrigin = bufferHasNoteKey(blackInputOriginScratch, key);

                    const bool isOffEvent = !isOn;
                    const int chIndex = juce::jlimit(1, 16, msg.getChannel()) - 1;
                    const int noteIndex = juce::jlimit(0, 127, msg.getNoteNumber());
                    const bool laneAlreadyHoldsNote =
                        laneGateHeld[(size_t) chIndex][(size_t) noteIndex] > 0;

                    bool keepEvent = true;

                    if (blackSourceOnly)
                    {
                        // Fail-safe anti-stuck:
                        // if this lane already holds the note, always allow NOTE OFF
                        // even when origin matching is ambiguous in dense passages.
                        if (!isBlackOrigin && !(isOffEvent && laneAlreadyHoldsNote))
                            keepEvent = false;
                    }
                    else
                    {
                        // Symmetric fail-safe for white-source lanes: do not block
                        // NOTE OFF of an already-held note on ambiguous origin.
                        if (isBlackOrigin && !(isOffEvent && laneAlreadyHoldsNote))
                            keepEvent = false;
                    }

                    if (!keepEvent)
                        continue;

                    if (isOn)
                    {
                        laneGateHeld[(size_t) chIndex][(size_t) noteIndex] =
                            juce::jmax(0, laneGateHeld[(size_t) chIndex][(size_t) noteIndex] + 1);
                    }
                    else
                    {
                        laneGateHeld[(size_t) chIndex][(size_t) noteIndex] =
                            juce::jmax(0, laneGateHeld[(size_t) chIndex][(size_t) noteIndex] - 1);
                    }
                }
                else if (msg.isAftertouch())
                {
                    const bool isBlackOrigin =
                        bufferHasPolyAftertouch(blackInputOriginScratch,
                                                samplePos,
                                                msg.getChannel(),
                                                msg.getNoteNumber());

                    if (blackSourceOnly ? !isBlackOrigin : isBlackOrigin)
                        continue;
                }

                laneInputScratch[i].addEvent(msg, samplePos);
            }
        }

        // Build an indexed view of lane input NOTE keys lazily.
        // We only materialize this index when consume-housekeeping actually
        // needs NOTE membership checks (pending off/on injections or
        // downstream synthetic consume propagation).
        //
        // This avoids an unconditional O(events) scan on every lane/block.
        constexpr int kLaneInputKeyIndexCapacity = 1024;
        std::array<NoteKey, kLaneInputKeyIndexCapacity> laneInputKeyIndex {};
        bool laneInputKeyIndexOverflow = false;
        bool laneInputKeyIndexBuilt = false;
        int laneInputKeyCount = 0;

        auto ensureLaneInputKeyIndexBuilt = [&]() noexcept
        {
            if (laneInputKeyIndexBuilt)
                return;

            laneInputKeyCount =
                collectUniqueNoteKeys(laneInputScratch[i], laneInputKeyIndex, laneInputKeyIndexOverflow);
            laneInputKeyIndexBuilt = true;
        };

        auto laneInputContainsKey = [&](const NoteKey& key) noexcept
        {
            ensureLaneInputKeyIndexBuilt();

            if (containsNoteKey(laneInputKeyIndex, laneInputKeyCount, key))
                return true;

            if (laneInputKeyIndexOverflow)
                return bufferHasNoteKey(laneInputScratch[i], key);

            return false;
        };

        auto laneInputAppendKey = [&](const NoteKey& key) noexcept
        {
            ensureLaneInputKeyIndexBuilt();

            if (laneInputKeyCount >= kLaneInputKeyIndexCapacity)
            {
                laneInputKeyIndexOverflow = true;
                return;
            }

            if (!containsNoteKey(laneInputKeyIndex, laneInputKeyCount, key))
                laneInputKeyIndex[(size_t) laneInputKeyCount++] = key;
        };

        //----------------------------------------------------------------------
        // 7.1.b) Apply pending housekeeping NOTE OFF (internal consume sync).
        // 7.1.c) Apply pending handoff NOTE ON from upstream internal releases.
        //
        // Direct lane is a hard boundary: it always starts from raw and ignores
        // upstream consume housekeeping/handoffs.
        //----------------------------------------------------------------------
        if (!directEnabled[i])
        {
            // We only inject a key if that exact NOTE OFF is not already present
            // in lane input.
            for (int k = 0; k < pendingStealOffCount[i]; ++k)
            {
                const auto offKey = pendingStealOffKeys[i][(size_t) k];

                if (inputFilterProcessors[i] &&
                    !inputFilterProcessors[i]->isHeldNoteForRouting(offKey.channel, offKey.note))
                    continue;

                if (laneInputContainsKey(offKey))
                    continue;

                laneInputScratch[i].addEvent(juce::MidiMessage::noteOff(offKey.channel, offKey.note),
                                             offKey.samplePos);
                laneInputAppendKey(offKey);
            }

            for (int k = 0; k < pendingHandoffOnCount[i]; ++k)
            {
                const auto onEv = pendingHandoffOns[i][(size_t) k];
                const NoteKey onKey { onEv.samplePos, onEv.channel, onEv.note, true };

                if (inputFilterProcessors[i] &&
                    inputFilterProcessors[i]->isHeldNoteForRouting(onEv.channel, onEv.note))
                    continue;

                if (laneInputContainsKey(onKey))
                    continue;

                laneInputScratch[i].addEvent(
                    juce::MidiMessage::noteOn(onEv.channel,
                                              onEv.note,
                                              (juce::uint8) juce::jlimit(1, 127, onEv.velocity)),
                    onEv.samplePos);
                laneInputAppendKey(onKey);
            }
        }

        // Routing-source topology changed for this lane:
        // - direct target: rebuild external held snapshot from raw stream.
        // - non-direct target: clear external held snapshot (previous source is stale).
        if (sourceResetToRawDirect)
        {
            // Raw-held snapshot is already up to date for this block. Do not
            // feed laneInputScratch incrementally here, otherwise this block
            // note events would be counted twice.
            copyRawHeldSnapshotToLane(i);
        }
        else
        {
            if (sourceResetThisLane)
                clearLaneInputHeldSnapshot(i);

            // Keep an external held snapshot of this lane input stream.
            updateHeldSnapshotFromBuffer(
                laneInputScratch[i],
                laneInputHeldCount[(size_t) i],
                laneInputHeldVelocity[(size_t) i],
                laneInputHeldVelocityStack[(size_t) i],
                laneInputHeldVelocityStackSize[(size_t) i],
                laneInputHeldOrder[(size_t) i],
                laneInputHeldOrderCounter[(size_t) i]);
        }

        //----------------------------------------------------------------------
        // 7.2) Run lane chain
        //   We keep three buffers per lane:
        //     - laneInputScratch       : what this lane received
        //     - laneAfterFilterScratch : after InputFilter only (consume basis)
        //     - laneOutputScratch      : final lane output after lane modules
        //                                (IF + Harm + Arp + Splitter)
        //----------------------------------------------------------------------

        laneAfterFilterScratch[i].clear();
        laneOutputScratch[i].clear();

        //----- InputFilter -----
        const bool wasInputFilterEnabled = lastInputFilterEnabledState[(size_t) i];

        if (laneInputFilterEnabled && inputFilterProcessors[i])
        {
            // InputFilter ON: lane receives/outputs normal note flow.
            laneOutputScratch[i].addEvents(laneInputScratch[i], 0, -1, 0);

            inputFilterProcessors[i]->process(laneOutputScratch[i]);

            if (!wasInputFilterEnabled)
            {
                // Re-enable edge: rebuild from currently held lane-input snapshot.
                inputFilterProcessors[i]->resyncFromExternalHeldState(
                    laneInputHeldCount[(size_t) i],
                    laneInputHeldVelocity[(size_t) i],
                    laneInputHeldOrder[(size_t) i],
                    laneOutputScratch[i],
                    0);
            }
            else if (sourceResetThisLane)
            {
                // Source-topology edge (including direct):
                // reconcile held/active state against external lane-input snapshot.
                inputFilterProcessors[i]->resyncFromExternalHeldState(
                    laneInputHeldCount[(size_t) i],
                    laneInputHeldVelocity[(size_t) i],
                    laneInputHeldOrder[(size_t) i],
                    laneOutputScratch[i],
                    0);
            }
        }
        else
        {
            // InputFilter OFF: lane is muted for notes, keep non-notes only.
            appendNonNoteEvents(laneInputScratch[i], laneOutputScratch[i]);

            if (inputFilterProcessors[i])
            {
                std::array<HeldNote, 16> releasedActive {};
                int releasedCount = 0;

                if (wasInputFilterEnabled)
                {
                    // Bypass edge: flush lingering voices and collect released notes.
                    releasedCount = inputFilterProcessors[i]->forceAllNotesOffAndClear(
                        laneOutputScratch[i], 0, &releasedActive);
                }

                // Redistribute released notes to downstream lane(s) when consume was active.
                if (wasInputFilterEnabled && consumeEnabled[i] && (i + 1) < activeLanes)
                {
                    for (int r = 0; r < releasedCount; ++r)
                    {
                        const auto& rel = releasedActive[(size_t) r];
                        const int chIndex = juce::jlimit(1, 16, rel.channel) - 1;
                        const int ntIndex = juce::jlimit(0, 127, rel.note);

                        if (rawHeldCount[(size_t) chIndex][(size_t) ntIndex] <= 0)
                            continue;

                        const int vel = juce::jlimit(1, 127, rawHeldVelocity[(size_t) chIndex][(size_t) ntIndex]);
                        const NoteOnInject onEv { 0, rel.channel, rel.note, vel };

                        for (int j = i + 1; j < activeLanes; ++j)
                        {
                            if (directEnabled[j])
                                break;

                            if (j > (i + 1) && consumeEnabled[j - 1])
                                break;

                            if (!laneFilterEnabled[j] || !inputFilterProcessors[j])
                                continue;

                            if (inputFilterProcessors[j]->isHeldNoteForRouting(onEv.channel, onEv.note))
                                break;

                            appendUniqueNoteOnInject(onEv, pendingHandoffOns[(size_t) j], pendingHandoffOnCount[j]);
                            break;
                        }
                    }
                }
            }
        }

        lastInputFilterEnabledState[(size_t) i] = laneInputFilterEnabled;

        // Snapshot "post-filter" state only when consume logic is active on this lane.
        // This avoids an unconditional full-buffer copy for lanes that do not consume.
        const bool consumeTrackingThisLane =
            (consumeEnabled[i] && laneInputFilterEnabled && inputFilterProcessors[i] != nullptr);
        if (consumeTrackingThisLane)
            laneAfterFilterScratch[i].addEvents(laneOutputScratch[i], 0, -1, 0);
	
        //----- Harmonizer -----
        // Always call process() so the processor can handle ON/OFF transitions
        // (flush harmonized notes and retrigger source notes on bypass).
        if (sourceResetThisLane || !laneInputFilterEnabled)
            clearHarmonizerHeldSnapshotForLane(i);

        if (harmonizerProcessors[i])
        {
            harmonizerProcessors[i]->setRuntimeForceBypass(false);
            harmonizerProcessors[i]->process(laneOutputScratch[i]);
        }

        const bool laneOutputNoteEventSeen =
            updateHeldCountSnapshotFromBuffer(laneOutputScratch[i],
                                              harmonizerHeldCountByLane[(size_t) i]);
        if (!laneUsesBlackSource && (laneOutputNoteEventSeen || sourceResetThisLane))
            chordOutputNoteEventSeen = true;

        //----- Arpeggiator -----
        // Always call process() to keep module transitions deterministic.
        if (arpeggiatorProcessors[i])
        {
            // Phase-lock this lane to a previous lane that has identical arp settings.
            bool laneClockLocked = false;
            for (int j = 0; j < i; ++j)
            {
                if (!preBlockArpSyncValid[(size_t) j] || arpeggiatorProcessors[j] == nullptr)
                    continue;

                const auto laneJ = Lanes::fromIndex(j);
                if (!areArpeggiatorSettingsEquivalentCached(lane, laneJ))
                    continue;

                // Queue-sync safety:
                // une queue differree contient des NoteOn/controle derives du pool
                // de notes tenues. On ne partage donc un snapshot inter-lane que
                // si le pool de notes source est strictement equivalent.
                // Sinon on risquerait d injecter des differes "etrangers" (stuck,
                // doubles trig ou desync musicale).
                if (preBlockArpSyncScratch[(size_t) i].heldNoteFingerprint
                    != preBlockArpSyncScratch[(size_t) j].heldNoteFingerprint
                    || preBlockArpSyncScratch[(size_t) i].heldNoteCount
                        != preBlockArpSyncScratch[(size_t) j].heldNoteCount)
                {
                    continue;
                }

                const auto& laneModA = lfoLaneModScratch[(size_t) i];
                const auto& laneModB = lfoLaneModScratch[(size_t) j];
                static constexpr int kIdxRateMorph = 1;
                static constexpr int kIdxVelocityMorph = 5;
                static constexpr int kIdxGrooveMorph = 6;
                static constexpr int kIdxGateMorph = 7;
                const bool hasSameMorphLfo =
                    quantizeEffectiveArpMorphForPhaseLock(
                        arpEqScalarRaw[(size_t) i][(size_t) kIdxRateMorph], laneModA.arpRateMorphNormDelta)
                        ==
                    quantizeEffectiveArpMorphForPhaseLock(
                        arpEqScalarRaw[(size_t) j][(size_t) kIdxRateMorph], laneModB.arpRateMorphNormDelta)
                    &&
                    quantizeEffectiveArpMorphForPhaseLock(
                        arpEqScalarRaw[(size_t) i][(size_t) kIdxGateMorph], laneModA.arpGateMorphNormDelta)
                        ==
                    quantizeEffectiveArpMorphForPhaseLock(
                        arpEqScalarRaw[(size_t) j][(size_t) kIdxGateMorph], laneModB.arpGateMorphNormDelta)
                    &&
                    quantizeEffectiveArpMorphForPhaseLock(
                        arpEqScalarRaw[(size_t) i][(size_t) kIdxGrooveMorph], laneModA.arpGrooveMorphNormDelta)
                        ==
                    quantizeEffectiveArpMorphForPhaseLock(
                        arpEqScalarRaw[(size_t) j][(size_t) kIdxGrooveMorph], laneModB.arpGrooveMorphNormDelta)
                    &&
                    quantizeEffectiveArpMorphForPhaseLock(
                        arpEqScalarRaw[(size_t) i][(size_t) kIdxVelocityMorph], laneModA.arpVelocityMorphNormDelta)
                        ==
                    quantizeEffectiveArpMorphForPhaseLock(
                        arpEqScalarRaw[(size_t) j][(size_t) kIdxVelocityMorph], laneModB.arpVelocityMorphNormDelta);

                if (!hasSameMorphLfo)
                    continue;

                arpeggiatorProcessors[i]->applyClockSyncState(preBlockArpSyncScratch[(size_t) j]);
                // Propagate the effective lock-source snapshot so downstream lanes
                // can lock on the same canonical timeline in this block.
                preBlockArpSyncScratch[(size_t) i] = preBlockArpSyncScratch[(size_t) j];
                preBlockArpSyncValid[(size_t) i] = preBlockArpSyncValid[(size_t) j];
                laneClockLocked = true;
                break;
            }

            if (!laneClockLocked)
                preBlockArpSyncValid[(size_t) i] = preBlockArpSyncScratch[(size_t) i].valid;

            ArpeggiatorProcessor::ProcessContext arpCtx;
            arpCtx.sampleRate = juce::jmax(1.0, getSampleRate());
            arpCtx.bpm = juce::jmax(1.0, bpm);
            arpCtx.ppqAtBlockStart = effectivePpqAtBlockStart;
            arpCtx.isPlaying = effectivePlaying;
            arpCtx.justStarted = effectiveJustStarted;
            arpCtx.numSamples = buffer.getNumSamples();
            arpeggiatorProcessors[i]->process(laneOutputScratch[i], arpCtx);

            for (int layer = 0; layer < 8; ++layer)
            {
                const int playbackStep =
                    arpeggiatorProcessors[i]->getPlaybackStepForLayer(layer);
                laneArpPlaybackStepDisplay[(size_t) i][(size_t) layer]
                    .store(playbackStep, std::memory_order_relaxed);
            }
        }

        //----- Splitter -----
        if (splitterProcessors[i])
            splitterProcessors[i]->process(laneOutputScratch[i]);

        //----------------------------------------------------------------------
        // 7.3) Build remainder for next lane (if consume ON)
        //
        // Consume definition (safe + deterministic):
        //   - Only NOTE events that survived InputFilter are considered consumed.
        //   - Only NOTE events are removed from the next lane input.
        //   - Non-note messages are never consumed.
        //----------------------------------------------------------------------

        laneRemainderScratch[i].clear();

        if (consumeEnabled[i] && laneInputFilterEnabled && inputFilterProcessors[i])
        {
            std::array<NoteKey, 512> consumedKeys {};
            bool consumedKeysOverflow = false;
            const int consumedCount =
                collectConsumedNotes(laneAfterFilterScratch[i], consumedKeys, consumedKeysOverflow);

            // Fast path: no NOTE survived InputFilter, so consume removes nothing.
            // Keep lane input as-is without per-event filtering work.
            if (consumedCount == 0 && !consumedKeysOverflow)
            {
                laneRemainderScratch[i].addEvents(laneInputScratch[i], 0, -1, 0);
            }
            else
            {
                buildRemainder(laneInputScratch[i],
                               consumedKeys,
                               consumedCount,
                               consumedKeysOverflow,
                               laneAfterFilterScratch[i],
                               laneRemainderScratch[i]);
            }

           #if LOGS_ENABLED
            DBG_LOG("PROCESS", "PLUGINPROCESSOR", "CONSUME", "#C01#",
                    juce::String("Lane ") + laneTag(lane)
                    + " consume=ON consumedNotes=" + juce::String(consumedCount));
           #endif
        }
        else if (consumeEnabled[i])
        {
            // Consume requested but not active at InputFilter level:
            // remainder must mirror lane input so downstream serial routing stays coherent.
            laneRemainderScratch[i].addEvents(laneInputScratch[i], 0, -1, 0);
        }
        else
        {
            // Consume OFF: remainder is not used by downstream lanes.
        }

        //----------------------------------------------------------------------
        // 7.3.b) Propagate internal consume transitions to downstream lanes.
        //
        // - Synthetic NOTE ON (not present in lane input):
        //     note became active internally in this lane -> downstream lanes must
        //     receive housekeeping NOTE OFF for that key.
        //
        // - Synthetic NOTE OFF (not present in lane input):
        //     note was released internally in this lane but may still be held
        //     physically -> downstream lanes must receive NOTE ON handoff using
        //     original input velocity.
        //----------------------------------------------------------------------
        if (consumeEnabled[i] && laneInputFilterEnabled && inputFilterProcessors[i] && (i + 1) < activeLanes)
        {
            ensureLaneInputKeyIndexBuilt();

            std::array<NoteKey, 256> syntheticOns {};
            const int syntheticOnCount =
                collectSyntheticConsumedNoteOns(laneInputScratch[i],
                                                laneInputKeyIndex,
                                                laneInputKeyCount,
                                                laneInputKeyIndexOverflow,
                                                laneAfterFilterScratch[i],
                                                syntheticOns);

            for (int s = 0; s < syntheticOnCount; ++s)
            {
                const auto onKey = syntheticOns[(size_t) s];
                const NoteKey offKey { onKey.samplePos, onKey.channel, onKey.note, false };

                for (int j = i + 1; j < activeLanes; ++j)
                {
                    if (directEnabled[j])
                        break;

                    if (j > (i + 1) && consumeEnabled[j - 1])
                        break;

                    if (!laneFilterEnabled[j] || !inputFilterProcessors[j])
                        continue;

                    if (!inputFilterProcessors[j]->isHeldNoteForRouting(offKey.channel, offKey.note))
                        continue;

                    appendUniqueNoteKey(offKey, pendingStealOffKeys[(size_t) j], pendingStealOffCount[j]);
                }
            }

            std::array<NoteKey, 256> syntheticOffs {};
            const int syntheticOffCount =
                collectSyntheticConsumedNoteOffs(laneInputScratch[i],
                                                 laneInputKeyIndex,
                                                 laneInputKeyCount,
                                                 laneInputKeyIndexOverflow,
                                                 laneAfterFilterScratch[i],
                                                 syntheticOffs);

            for (int s = 0; s < syntheticOffCount; ++s)
            {
                const auto offKey = syntheticOffs[(size_t) s];
                const int chIndex = juce::jlimit(1, 16, offKey.channel) - 1;
                const int ntIndex = juce::jlimit(0, 127, offKey.note);

                // Only handoff if this note is still physically held in raw input state.
                if (rawHeldCount[(size_t) chIndex][(size_t) ntIndex] <= 0)
                    continue;

                const int vel = juce::jlimit(1, 127, rawHeldVelocity[(size_t) chIndex][(size_t) ntIndex]);
                const NoteOnInject onEv { offKey.samplePos, offKey.channel, offKey.note, vel };

                for (int j = i + 1; j < activeLanes; ++j)
                {
                    if (directEnabled[j])
                        break;

                    if (j > (i + 1) && consumeEnabled[j - 1])
                        break;

                    if (!laneFilterEnabled[j] || !inputFilterProcessors[j])
                        continue;

                    if (inputFilterProcessors[j]->isHeldNoteForRouting(onEv.channel, onEv.note))
                        break;

                    appendUniqueNoteOnInject(onEv, pendingHandoffOns[(size_t) j], pendingHandoffOnCount[j]);
                    break; // Handoff goes to the first eligible downstream lane only.
                }
            }
        }

        //----------------------------------------------------------------------
        // 6.4) Merge lane output into the junction stream (pre-dedup)
        //----------------------------------------------------------------------
        mergedLanesScratch.addEvents(laneOutputScratch[i], 0, -1, 0);
    }

    //--------------------------------------------------------------------------
    // 6.5) DEDUP at the lane junction (post-merge, pre-host)
    //--------------------------------------------------------------------------
    {
        // Capacity note:
        // - 1024 unique NOTE keys is typically plenty for a MIDI block.
        // - If exceeded, the algorithm fails open (keeps remaining NOTE events).
        std::array<NoteKey, 1024> seenKeys {};
        int seenCount = 0;

        dedupNoteEvents(mergedLanesScratch, dedupedMergedScratch, seenKeys, seenCount, outputHeldCount);

       #if LOGS_ENABLED
        // Lightweight diagnostic: only log if something was removed.
        const int inCount  = countMidiEvents(mergedLanesScratch);
        const int outCount = countMidiEvents(dedupedMergedScratch);

        if (outCount < inCount)
        {
            DBG_LOG("PROCESS", "PLUGINPROCESSOR", "DEDUP", "#D00#",
                    juce::String("Dropped ") + juce::String(inCount - outCount)
                    + " duplicate NOTE events at junction.");
        }
       #endif
    }

    const auto inputChordSnapshot = buildPitchClassSnapshotFromHeldCounts(
        whiteModeEnabled ? whiteModeHeldCount : rawHeldCount);

    std::array<std::array<int, 128>, 16> chordOutputHeldCount {};
    for (int laneIndex = 0; laneIndex < activeLanes; ++laneIndex)
    {
        const bool laneUsesBlackSource = laneBlackGateActive && blackNotesEnabled[laneIndex];
        if (laneUsesBlackSource)
            continue;

        const auto& laneHeld = harmonizerHeldCountByLane[(size_t) laneIndex];
        for (int ch = 0; ch < 16; ++ch)
            for (int note = 0; note < 128; ++note)
                chordOutputHeldCount[(size_t) ch][(size_t) note] += laneHeld[(size_t) ch][(size_t) note];
    }
    const auto outputChordSnapshot = buildPitchClassSnapshotFromHeldCounts(chordOutputHeldCount);

    uint64_t harmonizerParamHash = 1469598103934665603ULL;
    harmonizerParamHash = hashMix64(harmonizerParamHash, (uint64_t) activeLanes);
    harmonizerParamHash = hashMix64(harmonizerParamHash,
                                    (uint64_t) juce::jlimit(0, 12, readRoundedParamValue(harmGlobalKeyRaw, 0)));
    harmonizerParamHash = hashMix64(harmonizerParamHash,
                                    (uint64_t) juce::jlimit(0, 9, readRoundedParamValue(harmGlobalScaleRaw, 0)));

    for (int laneIndex = 0; laneIndex < activeLanes; ++laneIndex)
    {
        harmonizerParamHash = hashMix64(
            harmonizerParamHash,
            (uint64_t) juce::jlimit(0, 1, readRoundedParamValue(laneHarmonizerEnableRaw[(size_t) laneIndex], 1)));
        harmonizerParamHash = hashMix64(
            harmonizerParamHash,
            (uint64_t) (readRoundedParamValue(laneHarmonizerPitchCorrectRaw[(size_t) laneIndex], 0) + 128));
        harmonizerParamHash = hashMix64(
            harmonizerParamHash,
            (uint64_t) readRoundedParamValue(laneHarmonizerOctavePlusRaw[(size_t) laneIndex], 0));
        harmonizerParamHash = hashMix64(
            harmonizerParamHash,
            (uint64_t) readRoundedParamValue(laneHarmonizerOctaveMinusRaw[(size_t) laneIndex], 0));

        for (int voice = 0; voice < 4; ++voice)
        {
            harmonizerParamHash = hashMix64(
                harmonizerParamHash,
                (uint64_t) (readRoundedParamValue(
                    laneHarmonizerVoiceOffsetRaw[(size_t) laneIndex][(size_t) voice], 0) + 256));
            harmonizerParamHash = hashMix64(
                harmonizerParamHash,
                (uint64_t) (readRoundedParamValue(
                    laneHarmonizerVelocityModRaw[(size_t) laneIndex][(size_t) voice], 0) + 128));
        }
    }

    const bool harmonizerParamsChanged =
        (!chordHarmonizerParamHashValidRt || harmonizerParamHash != chordHarmonizerParamHashRt);
    chordHarmonizerParamHashRt = harmonizerParamHash;
    chordHarmonizerParamHashValidRt = true;

    const bool chordStateChanged =
        (inputChordSnapshot.pitchClassMask != chordInputPitchClassMaskRt)
        || (inputChordSnapshot.bassPitchClass != chordInputBassPitchClassRt)
        || (outputChordSnapshot.pitchClassMask != chordOutputPitchClassMaskRt)
        || (outputChordSnapshot.bassPitchClass != chordOutputBassPitchClassRt);

    if (whiteModeEnabled && blackModeEnabled)
    {
        // Re-target black mapping:
        // - on effective chord-state move (input or post-harmonizer),
        // - or on harmonizer parameter edits.
        //
        // This avoids stale black-note mapping during fast legato transitions
        // where NOTE ON and NOTE OFF overlap across blocks.
        const bool blackRetargetTrigger = (chordStateChanged || harmonizerParamsChanged);
        if (blackRetargetTrigger)
        {
            const auto desiredBlackSet =
                buildBlackFiveClassSetDeterministic(outputChordSnapshot.pitchClassMask,
                                                    outputChordSnapshot.bassPitchClass,
                                                    currentTonicPc,
                                                    currentModeIndex);

            bool setChanged = false;
            for (int i = 0; i < 5; ++i)
            {
                if (blackModeFiveClassSet[(size_t) i] != desiredBlackSet[(size_t) i])
                {
                    setChanged = true;
                    break;
                }
            }

            if (setChanged)
            {
                blackModePendingFiveClassSet = desiredBlackSet;
                blackModePendingSetUpdate = true;
            }
        }
    }
    else
    {
        blackModePendingSetUpdate = false;
    }

    if (chordStateChanged || chordInputNoteEventSeen || chordOutputNoteEventSeen || harmonizerParamsChanged)
    {
        chordInputPitchClassMaskRt = inputChordSnapshot.pitchClassMask;
        chordInputBassPitchClassRt = inputChordSnapshot.bassPitchClass;
        chordOutputPitchClassMaskRt = outputChordSnapshot.pitchClassMask;
        chordOutputBassPitchClassRt = outputChordSnapshot.bassPitchClass;

        uint32_t beginSeq = chordSnapshotRevisionRt + 1u;
        if ((beginSeq & 1u) == 0u)
            ++beginSeq;
        chordSnapshotRevisionRt = beginSeq;
        uiChordSnapshotRevision.store(beginSeq, std::memory_order_release);

        uiInputChordPitchClassMask.store(chordInputPitchClassMaskRt, std::memory_order_relaxed);
        uiInputChordBassPitchClass.store(chordInputBassPitchClassRt, std::memory_order_relaxed);
        uiOutputChordPitchClassMask.store(chordOutputPitchClassMaskRt, std::memory_order_relaxed);
        uiOutputChordBassPitchClass.store(chordOutputBassPitchClassRt, std::memory_order_relaxed);

        uint32_t endSeq = beginSeq + 1u;
        if ((endSeq & 1u) != 0u)
            ++endSeq;
        chordSnapshotRevisionRt = endSeq;
        uiChordSnapshotRevision.store(endSeq, std::memory_order_release);
    }

    //--------------------------------------------------------------------------
    // 7) MainBar centralized CC sequencer (run after note pipeline)
    //--------------------------------------------------------------------------
    runMainBarCcSequencerRt();

    //--------------------------------------------------------------------------
    // 8) Deferred monitor updates (lowest priority in RT path)
    //--------------------------------------------------------------------------
    {
        // 8.1) Input monitor feed (after global W/B remap, before swallow)
        const bool inputMonitorEnabled = (inputMonitorEnableRaw != nullptr && inputMonitorEnableRaw->load() > 0.5f);

        if (inputMonitorEnabled)
        {
            int writesRemaining =
                juce::jmin(kInputMonitorWriteBudgetPerBlock, inputFifo.getFreeSpace());
            if (writesRemaining > 0)
            {
                const bool showNotes = (inputMonitorFilterNoteRaw != nullptr
                                        && inputMonitorFilterNoteRaw->load() > 0.5f);
                const bool showControls = (inputMonitorFilterControlRaw != nullptr
                                           && inputMonitorFilterControlRaw->load() > 0.5f);
                const bool showClock = (inputMonitorFilterClockRaw != nullptr
                                        && inputMonitorFilterClockRaw->load() > 0.5f);
                const bool showEvents = (inputMonitorFilterEventRaw != nullptr
                                         && inputMonitorFilterEventRaw->load() > 0.5f);
                const bool acceptAll = showNotes && showControls && showClock && showEvents;
                int scanned = 0;

                for (auto it = midiMessages.begin(); it != midiMessages.end(); ++it)
                {
                    if (writesRemaining <= 0 || scanned >= kInputMonitorScanBudgetPerBlock)
                        break;

                    ++scanned;

                    const auto meta = *it;
                    const auto& msg = meta.getMessage();

                    if (!acceptAll)
                    {
                        bool keep = true;

                        if (!showNotes && (msg.isNoteOn() || msg.isNoteOff()))
                            keep = false;

                        if (!showControls && (msg.isController() || msg.isProgramChange()))
                            keep = false;

                        if (!showClock && (msg.isMidiClock() || msg.isMidiStart() || msg.isMidiContinue()
                                           || msg.isMidiStop() || msg.isQuarterFrame()
                                           || msg.isSongPositionPointer()
                                           || (msg.getRawDataSize() > 0
                                               && (msg.getRawData()[0] == 0xF1 || msg.getRawData()[0] == 0xFE))))
                        {
                            keep = false;
                        }

                        if (!showEvents && (msg.isPitchWheel() || msg.isAftertouch()
                                            || msg.isChannelPressure() || msg.isSysEx()))
                        {
                            keep = false;
                        }

                        if (!keep)
                            continue;
                    }

                    if (!pushMonitorMessage(inputFifo, inputFifoMessages, msg))
                        break;

                    --writesRemaining;
                }
            }
        }

        // 8.2) Shared output monitor feed (all lanes + MainBar CC source tags).
        const bool outputMonitorEnabled = (outputMonitorEnableRaw != nullptr && outputMonitorEnableRaw->load() > 0.5f);

        if (outputMonitorEnabled)
        {
            int writesRemainingTotal =
                juce::jmin(kOutputMonitorWriteBudgetPerBlock, outputFifo.getFreeSpace());
            const bool showNotesOut = (outputMonitorFilterNoteRaw != nullptr
                                       && outputMonitorFilterNoteRaw->load() > 0.5f);
            const bool showControlsOut = (outputMonitorFilterControlRaw != nullptr
                                          && outputMonitorFilterControlRaw->load() > 0.5f);
            const bool showClockOut = (outputMonitorFilterClockRaw != nullptr
                                       && outputMonitorFilterClockRaw->load() > 0.5f);
            const bool showEventsOut = (outputMonitorFilterEventRaw != nullptr
                                        && outputMonitorFilterEventRaw->load() > 0.5f);
            const bool acceptAllOut = showNotesOut && showControlsOut && showClockOut && showEventsOut;

            if (writesRemainingTotal > 0)
            {
                for (int i = 0; i < activeLanes; ++i)
                {
                    int writesRemaining = writesRemainingTotal;
                    int scanned = 0;

                    for (auto it = laneOutputScratch[i].begin(); it != laneOutputScratch[i].end(); ++it)
                    {
                        if (writesRemaining <= 0
                            || writesRemainingTotal <= 0
                            || scanned >= kOutputMonitorScanBudgetPerLanePerBlock)
                            break;

                        ++scanned;

                        const auto meta = *it;
                        const auto& msg = meta.getMessage();

                        if (!acceptAllOut)
                        {
                            bool keep = true;

                            if (!showNotesOut && (msg.isNoteOn() || msg.isNoteOff()))
                                keep = false;

                            if (!showControlsOut && (msg.isController() || msg.isProgramChange()))
                                keep = false;

                            if (!showClockOut && (msg.isMidiClock() || msg.isMidiStart() || msg.isMidiContinue()
                                                  || msg.isMidiStop() || msg.isQuarterFrame()
                                                  || msg.isSongPositionPointer()
                                                  || (msg.getRawDataSize() > 0
                                                      && (msg.getRawData()[0] == 0xF1
                                                          || msg.getRawData()[0] == 0xFE))))
                            {
                                keep = false;
                            }

                            if (!showEventsOut && (msg.isPitchWheel() || msg.isAftertouch()
                                                   || msg.isChannelPressure() || msg.isSysEx()))
                            {
                                keep = false;
                            }

                            if (!keep)
                                continue;
                        }

                        if (!pushMonitorMessage(outputFifo,
                                                outputFifoMessages,
                                                outputFifoSourceKinds,
                                                outputFifoSourceIndices,
                                                msg,
                                                MonitorSourceKind::Lane,
                                                i))
                        {
                            break;
                        }

                        --writesRemainingTotal;
                        --writesRemaining;
                    }

                    if (writesRemainingTotal <= 0)
                        break;
                }
            }

            if (writesRemainingTotal > 0)
            {
                for (int i = 0; i < mainBarMonitorTapCount; ++i)
                {
                    if (writesRemainingTotal <= 0)
                        break;

                    const auto& tap = mainBarMonitorTapEvents[(size_t) i];
                    if (!pushMonitorMessage(outputFifo,
                                            outputFifoMessages,
                                            outputFifoSourceKinds,
                                            outputFifoSourceIndices,
                                            tap.message,
                                            MonitorSourceKind::MainBarController,
                                            tap.controllerModuleIndex))
                    {
                        break;
                    }

                    --writesRemainingTotal;
                }
            }
        }
    }

    //--------------------------------------------------------------------------
    // 9) Publish result to host (post-dedup)
    //--------------------------------------------------------------------------
    midiMessages.swapWith(dedupedMergedScratch);

    // Merge internal output injections (panic, debug, etc.)
    midiMessages.addEvents(midiOutputBuffer, 0, -1, 0);
    midiOutputBuffer.clear();

    localClockWasActive = localClockActive;
}

//==============================================================================
// Editor (UI)
//==============================================================================

juce::AudioProcessorEditor* MidivisiViciAudioProcessor::createEditor()
{
   #if LOGS_ENABLED
    DBG_LOG("UI", "PLUGINPROCESSOR", "EDITOR", "#600#",
            "Creating Editor instance (MidivisiViciAudioProcessorEditor).");
   #endif

    return new MidivisiViciAudioProcessorEditor(*this);
}

bool MidivisiViciAudioProcessor::hasEditor() const
{
   #if LOGS_ENABLED
    static bool firstCheck = true;
    if (firstCheck)
    {
        DBG_LOG("UI", "PLUGINPROCESSOR", "EDITOR", "#601#", "hasEditor() -> true");
        firstCheck = false;
    }
   #endif

    return true;
}

//==============================================================================
// State save / restore (stable + buffer reservation on restore)
//==============================================================================

void MidivisiViciAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
   #if LOGS_ENABLED
    DBG_LOG("STATE", "PLUGINPROCESSOR", "SAVE", "#700#",
            "Serializing plugin state (DAW save).");
   #endif

    juce::ValueTree pluginState("PLUGIN_STATE");

    pluginState.setProperty("lastKnownBpm",
                            lastKnownBpm.load(std::memory_order_relaxed),
                            nullptr);

    if (auto xml = parameters.copyState().createXml())
        pluginState.addChild(juce::ValueTree::fromXml(*xml), -1, nullptr);

    pluginState.addChild(presetManager.getState(), -1, nullptr);

    if (auto xml = pluginState.createXml())
        copyXmlToBinary(*xml, destData);
}

void MidivisiViciAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
   #if LOGS_ENABLED
    DBG_LOG("STATE", "PLUGINPROCESSOR", "RESTORE", "#710#",
            "Deserializing plugin state (DAW restore).");
   #endif

    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));

    if (xml && xml->hasTagName("PLUGIN_STATE"))
    {
        juce::ValueTree pluginState = juce::ValueTree::fromXml(*xml);

        const double restoredBpm =
            (double) pluginState.getProperty("lastKnownBpm", 120.0);
        if (restoredBpm > 0.0)
            lastKnownBpm.store(restoredBpm, std::memory_order_relaxed);

        if (auto paramsNode = pluginState.getChildWithName(parameters.state.getType());
            paramsNode.isValid())
        {
            presetManager.replaceParametersStateFromDaw(paramsNode);
        }

        if (auto presetNode = pluginState.getChildWithName(PresetManager::IDs::root);
            presetNode.isValid())
        {
            presetManager.loadState(presetNode);
            applyLfoUiStateToRuntime(presetManager.getCurrentSnapshotUiState());
        }

        // After restore, resync morph mirror from VTS.
        syncMorphMirrorFromVTS();

        // Non-RT: reserve again to reduce RT growth after restore.
        midiOutputBuffer.clear();      midiOutputBuffer.ensureSize(2048);
        swallowScratch.clear();        swallowScratch.ensureSize(2048);
        rawPlayableScratch.clear();    rawPlayableScratch.ensureSize(2048);
        whiteKeyRemapScratch.clear();  whiteKeyRemapScratch.ensureSize(2048);

        mergedLanesScratch.clear();    mergedLanesScratch.ensureSize(4096);
        dedupedMergedScratch.clear();  dedupedMergedScratch.ensureSize(4096);

        for (int i = 0; i < kNumLanes; ++i)
        {
            laneSourceScratch[i].clear();      laneSourceScratch[i].ensureSize(2048);
            laneInputScratch[i].clear();       laneInputScratch[i].ensureSize(2048);
            laneAfterFilterScratch[i].clear(); laneAfterFilterScratch[i].ensureSize(2048);
            laneOutputScratch[i].clear();      laneOutputScratch[i].ensureSize(2048);
            laneRemainderScratch[i].clear();   laneRemainderScratch[i].ensureSize(2048);
            lastInputFilterEnabledState[(size_t) i] = false;
            lastDirectRoutingState[(size_t) i] = false;
            lastConsumeRoutingState[(size_t) i] = false;

            for (auto& byNote : laneInputHeldCount[(size_t) i])    byNote.fill(0);
            for (auto& byNote : laneInputHeldVelocity[(size_t) i]) byNote.fill(0);
            for (auto& byNote : laneInputHeldVelocityStack[(size_t) i])
                for (auto& byVel : byNote)
                    byVel.fill(0);
            for (auto& byNote : laneInputHeldVelocityStackSize[(size_t) i]) byNote.fill(0);
            for (auto& byNote : laneInputHeldOrder[(size_t) i])    byNote.fill(0);
            laneInputHeldOrderCounter[(size_t) i] = 0;
        }

        for (auto& byNote : rawHeldCount)
            byNote.fill(0);
        for (auto& byNote : rawHeldVelocity)
            byNote.fill(0);
        for (auto& byNote : rawHeldVelocityStack)
            for (auto& byVel : byNote)
                byVel.fill(0);
        for (auto& byNote : rawHeldVelocityStackSize)
            byNote.fill(0);
        for (auto& byNote : rawHeldOrder)
            byNote.fill(0);
        rawHeldOrderCounter = 0;
        for (auto& byNote : outputHeldCount)
            byNote.fill(0);
        for (auto& byLane : harmonizerHeldCountByLane)
            for (auto& byNote : byLane)
                byNote.fill(0);
        lastDirectRoutingStateValid = false;
        lastWhiteInputModeState = false;
        lastBlackInputModeState = false;
        lastInputRemapStateValid = false;
        clearChordSnapshotRt();
        lastProcessedActiveLaneCount =
            juce::jlimit(1, kNumLanes, activeLaneCount.load(std::memory_order_relaxed));
    }
}

//==============================================================================
// UI helpers / internal sync
//==============================================================================

double MidivisiViciAudioProcessor::getHostPpqNowForUI() const
{
    const double ppq0 = uiPpqAtBlockStart.load(std::memory_order_relaxed);
    const bool isPlaying = getUiIsPlaying();

    if (!isPlaying)
        return ppq0;

    const double t0 = uiBlockWallclock.load(std::memory_order_relaxed);
    const double tNow = juce::Time::getMillisecondCounterHiRes() * 0.001;

    const double dt = std::max(0.0, tNow - t0);
    return ppq0 + uiBeatsPerSec.load(std::memory_order_relaxed) * dt;
}

bool MidivisiViciAudioProcessor::getUiIsPlaying() const noexcept
{
    const bool isPlaying = uiIsPlaying.load(std::memory_order_relaxed);
    if (!isPlaying)
        return false;

    // Some hosts stop calling processBlock while transport is stopped.
    // In that case, the last "playing" UI atomics can become stale and keep
    // sequencer/LFO visuals running. Consider playback inactive if no block
    // timestamp refresh happened recently.
    constexpr double kUiPlayFreshnessSec = 0.25;
    const double t0 = uiBlockWallclock.load(std::memory_order_relaxed);
    if (t0 <= 0.0)
        return false;

    const double tNow = juce::Time::getMillisecondCounterHiRes() * 0.001;
    return (tNow - t0) <= kUiPlayFreshnessSec;
}

int MidivisiViciAudioProcessor::getActiveLaneCount() const noexcept
{
    return juce::jlimit(1, kNumLanes, activeLaneCount.load(std::memory_order_relaxed));
}

void MidivisiViciAudioProcessor::setActiveLaneCount(int laneCount) noexcept
{
    activeLaneCount.store(juce::jlimit(1, kNumLanes, laneCount), std::memory_order_relaxed);
}

void MidivisiViciAudioProcessor::setLfoRowCountFromUI(int rowCount) noexcept
{
    lfoRtRowCount.store(juce::jlimit(0, kMaxLfoRows, rowCount), std::memory_order_relaxed);
}

void MidivisiViciAudioProcessor::setLfoRowConfigFromUI(int rowIndex,
                                                        const LfoRtRowConfig& config) noexcept
{
    if (!juce::isPositiveAndBelow(rowIndex, kMaxLfoRows))
        return;

    auto& row = lfoRtRows[(size_t) rowIndex];
    row.rateIndex.store(juce::jlimit(0, (int) kLfoRateWholeNotes.size() - 1, config.rateIndex),
                        std::memory_order_relaxed);
    row.depth.store(juce::jlimit(-100, 100, config.depth), std::memory_order_relaxed);
    row.offset.store(juce::jlimit(-100, 100, config.offset), std::memory_order_relaxed);
    row.waveShape.store(juce::jlimit(0, 4, config.waveShape), std::memory_order_relaxed);

    // Clamp once, then publish contiguous IDs [0..destinationCount).
    // Remaining slots are explicitly reset to kNone for deterministic reads.
    const int destinationCount = juce::jlimit(0,
                                              kMaxLfoDestinationsPerRow,
                                              config.destinationCount);
    row.destinationCount.store(destinationCount, std::memory_order_relaxed);

    for (int i = 0; i < destinationCount; ++i)
    {
        row.destinationStableIds[(size_t) i].store(
            juce::jmax(LfoDestinationIds::kNone, config.destinationStableIds[(size_t) i]),
            std::memory_order_relaxed);
    }

    for (int i = destinationCount; i < kMaxLfoDestinationsPerRow; ++i)
        row.destinationStableIds[(size_t) i].store(LfoDestinationIds::kNone, std::memory_order_relaxed);
}

void MidivisiViciAudioProcessor::setSnapshotUiFallbackState(const juce::ValueTree& state)
{
    if (!state.isValid() || !state.hasType(kUiStateNodeID))
        return;

    snapshotUiFallbackState = state.createCopy();
}

juce::ValueTree MidivisiViciAudioProcessor::buildLfoUiStateFromRuntime() const
{
    juce::ValueTree lfoState(kUiLfoStateNodeID);
    const int rowCount = juce::jlimit(0, kMaxLfoRows, lfoRtRowCount.load(std::memory_order_relaxed));
    lfoState.setProperty(kUiLfoCountPropID, rowCount, nullptr);

    for (int rowIndex = 0; rowIndex < rowCount; ++rowIndex)
    {
        const auto& row = lfoRtRows[(size_t) rowIndex];
        juce::ValueTree rowNode(kUiLfoRowNodeID);
        rowNode.setProperty(kUiLfoRowIndexPropID, rowIndex, nullptr);
        rowNode.setProperty(kUiLfoRatePropID,
                            juce::jlimit(0, (int) kLfoRateWholeNotes.size() - 1,
                                         row.rateIndex.load(std::memory_order_relaxed)),
                            nullptr);
        rowNode.setProperty(kUiLfoDepthPropID,
                            juce::jlimit(-100, 100, row.depth.load(std::memory_order_relaxed)),
                            nullptr);
        rowNode.setProperty(kUiLfoOffsetPropID,
                            juce::jlimit(-100, 100, row.offset.load(std::memory_order_relaxed)),
                            nullptr);
        rowNode.setProperty(kUiLfoWavePropID,
                            juce::jlimit(0, 4, row.waveShape.load(std::memory_order_relaxed)),
                            nullptr);

        const int destinationCount = juce::jlimit(0,
                                                  kMaxLfoDestinationsPerRow,
                                                  row.destinationCount.load(std::memory_order_relaxed));
        std::array<int, kMaxLfoDestinationsPerRow> destinationIds {};
        for (int i = 0; i < destinationCount; ++i)
        {
            destinationIds[(size_t) i] = juce::jmax(
                LfoDestinationIds::kNone,
                row.destinationStableIds[(size_t) i].load(std::memory_order_relaxed));
        }

        const int firstDestination =
            destinationCount > 0 ? destinationIds[0] : LfoDestinationIds::kNone;
        rowNode.setProperty(kUiLfoDestinationPropID, firstDestination, nullptr);
        rowNode.setProperty(kUiLfoDestinationsPropID,
                            serializeDestinationIds(destinationIds, destinationCount),
                            nullptr);

        lfoState.addChild(rowNode, -1, nullptr);
    }

    return lfoState;
}

juce::ValueTree MidivisiViciAudioProcessor::buildDefaultSnapshotUiStateFromRuntime() const
{
    juce::ValueTree uiState(kUiStateNodeID);
    uiState.setProperty(kUiActiveLaneCountPropID, getActiveLaneCount(), nullptr);
    uiState.addChild(buildLfoUiStateFromRuntime(), -1, nullptr);
    return uiState;
}

void MidivisiViciAudioProcessor::applyLfoUiStateToRuntime(const juce::ValueTree& state) noexcept
{
    juce::ValueTree lfoState;
    if (state.isValid() && state.hasType(kUiLfoStateNodeID))
        lfoState = state;
    else if (state.isValid() && state.hasType(kUiStateNodeID))
        lfoState = state.getChildWithName(kUiLfoStateNodeID);

    if (!lfoState.isValid() || !lfoState.hasType(kUiLfoStateNodeID))
    {
        // Keep behavior consistent with UI fallback reset: 4 default LFO rows.
        setLfoRowCountFromUI(4);

        LfoRtRowConfig cfg;
        cfg.rateIndex = 3;
        cfg.depth = 0;
        cfg.offset = 0;
        cfg.waveShape = 0;
        cfg.destinationCount = 1;
        cfg.destinationStableIds[0] = LfoDestinationIds::kNone;

        for (int i = 0; i < 4; ++i)
            setLfoRowConfigFromUI(i, cfg);

        return;
    }

    const int requestedCount = juce::jlimit(1,
                                            kMaxLfoRows,
                                            (int) lfoState.getProperty(kUiLfoCountPropID, 4));

    std::array<juce::ValueTree, (size_t) kMaxLfoRows> indexedRows {};
    for (int c = 0; c < lfoState.getNumChildren(); ++c)
    {
        const auto child = lfoState.getChild(c);
        if (!child.isValid() || !child.hasType(kUiLfoRowNodeID))
            continue;

        const int idx = juce::jlimit(0, requestedCount - 1,
                                     (int) child.getProperty(kUiLfoRowIndexPropID, c));
        indexedRows[(size_t) idx] = child;
    }

    setLfoRowCountFromUI(requestedCount);

    for (int rowIndex = 0; rowIndex < requestedCount; ++rowIndex)
    {
        LfoRtRowConfig cfg;
        cfg.rateIndex = 3;
        cfg.depth = 0;
        cfg.offset = 0;
        cfg.waveShape = 0;
        cfg.destinationCount = 1;
        cfg.destinationStableIds[0] = LfoDestinationIds::kNone;

        const auto rowNode = indexedRows[(size_t) rowIndex];
        if (rowNode.isValid())
        {
            cfg.rateIndex = juce::jlimit(0,
                                         (int) kLfoRateWholeNotes.size() - 1,
                                         (int) rowNode.getProperty(kUiLfoRatePropID, cfg.rateIndex));
            cfg.depth = juce::jlimit(-100, 100,
                                     (int) rowNode.getProperty(kUiLfoDepthPropID, cfg.depth));
            cfg.offset = juce::jlimit(-100, 100,
                                      (int) rowNode.getProperty(kUiLfoOffsetPropID, cfg.offset));
            cfg.waveShape = juce::jlimit(0, 4,
                                         (int) rowNode.getProperty(kUiLfoWavePropID, cfg.waveShape));

            juce::Array<int> destinationIds;
            if (rowNode.hasProperty(kUiLfoDestinationsPropID))
            {
                destinationIds = parseDestinationIds(
                    rowNode.getProperty(kUiLfoDestinationsPropID, juce::String()).toString());
            }

            if (destinationIds.isEmpty())
            {
                // Backward compatibility:
                // older snapshots had a single destination scalar field.
                const int legacyDestination =
                    juce::jmax(LfoDestinationIds::kNone,
                               (int) rowNode.getProperty(kUiLfoDestinationPropID,
                                                         LfoDestinationIds::kNone));
                destinationIds.add(legacyDestination);
            }

            cfg.destinationCount = juce::jlimit(1, kMaxLfoDestinationsPerRow, destinationIds.size());
            for (int i = 0; i < cfg.destinationCount; ++i)
                cfg.destinationStableIds[(size_t) i] = juce::jmax(LfoDestinationIds::kNone, destinationIds[i]);
        }

        setLfoRowConfigFromUI(rowIndex, cfg);
    }
}

void MidivisiViciAudioProcessor::installSnapshotUiCallbacksFallback()
{
    snapshotUiFallbackState = buildDefaultSnapshotUiStateFromRuntime();

    presetManager.setOnSnapshotUiCapture([this]
    {
        juce::ValueTree state =
            (snapshotUiFallbackState.isValid() && snapshotUiFallbackState.hasType(kUiStateNodeID))
                ? snapshotUiFallbackState.createCopy()
                : buildDefaultSnapshotUiStateFromRuntime();

        // Always refresh LFO payload from current runtime cache.
        if (auto lfoNode = state.getChildWithName(kUiLfoStateNodeID); lfoNode.isValid())
            state.removeChild(lfoNode, nullptr);
        state.addChild(buildLfoUiStateFromRuntime(), -1, nullptr);

        snapshotUiFallbackState = state.createCopy();
        return state;
    });

    presetManager.setOnSnapshotUiApply([this](const juce::ValueTree& state)
    {
        applyLfoUiStateToRuntime(state);

        if (state.isValid() && state.hasType(kUiStateNodeID))
            snapshotUiFallbackState = state.createCopy();
        else
            snapshotUiFallbackState = buildDefaultSnapshotUiStateFromRuntime();
    });
}

void MidivisiViciAudioProcessor::setMainBarPageCountFromUI(int pageCount) noexcept
{
    mainBarRtPageCount.store(juce::jlimit(0, kMainBarMaxPages, pageCount), std::memory_order_relaxed);
}

int MidivisiViciAudioProcessor::getMainBarPageCount() const noexcept
{
    return juce::jlimit(0, kMainBarMaxPages, mainBarRtPageCount.load(std::memory_order_relaxed));
}

void MidivisiViciAudioProcessor::setMainBarPageConfigFromUI(int pageIndex,
                                                             const MainBarPageRtConfig& config) noexcept
{
    if (!juce::isPositiveAndBelow(pageIndex, kMainBarMaxPages))
        return;

    auto& page = mainBarPages[(size_t) pageIndex];
    page.channel.store(juce::jlimit(1, 16, config.channel), std::memory_order_relaxed);
    page.ccNumber.store(juce::jlimit(0, 127, config.ccNumber), std::memory_order_relaxed);
    page.morphValue.store(juce::jlimit(0, 100, config.morphValue), std::memory_order_relaxed);

    for (int step = 0; step < kMainBarStepsPerPage; ++step)
    {
        page.stepValues[(size_t) step].store(
            juce::jlimit(0, 127, config.stepValues[(size_t) step]),
            std::memory_order_relaxed);

        int rate = juce::jlimit(0, kMainBarRateRndChoice, config.stepRates[(size_t) step]);
        if (step == 0 && rate == 0)
            rate = 5;

        page.stepRates[(size_t) step].store(rate, std::memory_order_relaxed);
    }
}

int MidivisiViciAudioProcessor::getMainBarPlaybackStepForPage(int pageIndex) const noexcept
{
    if (!juce::isPositiveAndBelow(pageIndex, kMainBarMaxPages))
        return -1;

    return mainBarPlaybackStep[(size_t) pageIndex].load(std::memory_order_relaxed);
}

int MidivisiViciAudioProcessor::getMainBarMorphDisplayValueForUI(int pageIndex) const noexcept
{
    if (!juce::isPositiveAndBelow(pageIndex, kMainBarMaxPages))
        return 50;

    return mainBarMorphDisplay[(size_t) pageIndex].load(std::memory_order_relaxed);
}

int MidivisiViciAudioProcessor::getLaneHarmonizerVoiceOffsetDisplayValueForUI(Lanes::Lane lane,
                                                                               int voiceIndex0Based) const noexcept
{
    const int laneIndex = juce::jlimit(0, kNumLanes - 1, (int) lane);
    if (!juce::isPositiveAndBelow(voiceIndex0Based, 4))
        return 0;

    return laneHarmVoiceOffsetDisplay[(size_t) laneIndex][(size_t) voiceIndex0Based]
        .load(std::memory_order_relaxed);
}

int MidivisiViciAudioProcessor::getLaneHarmonizerVelocityModDisplayValueForUI(Lanes::Lane lane,
                                                                               int voiceIndex0Based) const noexcept
{
    const int laneIndex = juce::jlimit(0, kNumLanes - 1, (int) lane);
    if (!juce::isPositiveAndBelow(voiceIndex0Based, 4))
        return 0;

    return laneHarmVelocityModDisplay[(size_t) laneIndex][(size_t) voiceIndex0Based]
        .load(std::memory_order_relaxed);
}

int MidivisiViciAudioProcessor::getLaneArpMorphDisplayValueForUI(Lanes::Lane lane,
                                                                  int morphIndex0Based) const noexcept
{
    const int laneIndex = juce::jlimit(0, kNumLanes - 1, (int) lane);
    if (!juce::isPositiveAndBelow(morphIndex0Based, 4))
        return 0;

    return laneArpMorphDisplay[(size_t) laneIndex][(size_t) morphIndex0Based]
        .load(std::memory_order_relaxed);
}

int MidivisiViciAudioProcessor::getLaneArpPlaybackStepForUI(Lanes::Lane lane,
                                                             int layerIndex0Based) const noexcept
{
    const int laneIndex = juce::jlimit(0, kNumLanes - 1, (int) lane);
    if (!juce::isPositiveAndBelow(layerIndex0Based, 8))
        return -1;

    // UI layer order:
    // 0=Rythm, 1=Gate, 2=Groove, 3=Velocity, 4=Strum/Hit, 5=Jump/VeloEnv, 6=Octave/TimEnv, 7=Retrig
    //
    // Processor layer order:
    // 0=Rythm, 1=Strum/Hit, 2=Jump/VeloEnv, 3=Octave/TimEnv, 4=Velocity, 5=Groove, 6=Gate, 7=Retrig
    const auto uiToProcessorLayer = [](int uiLayer) noexcept
    {
        switch (uiLayer)
        {
            case 0: return 0; // Rythm
            case 1: return 6; // Gate
            case 2: return 5; // Groove
            case 3: return 4; // Velocity
            case 4: return 1; // Strum / Hit
            case 5: return 2; // Jump / Velo Env
            case 6: return 3; // Octave / Tim Env
            case 7: return 7; // Retrig
            default: break;
        }

        return 0;
    };

    const int processorLayer = uiToProcessorLayer(layerIndex0Based);
    return laneArpPlaybackStepDisplay[(size_t) laneIndex][(size_t) processorLayer]
        .load(std::memory_order_relaxed);
}

uint32_t MidivisiViciAudioProcessor::getChordPitchClassSnapshotForUI(
    uint16_t& inputPitchClassMask,
    int& inputBassPitchClass,
    uint16_t& outputPitchClassMask,
    int& outputBassPitchClass) const noexcept
{
    for (;;)
    {
        const uint32_t beginSeq = uiChordSnapshotRevision.load(std::memory_order_acquire);
        if ((beginSeq & 1u) != 0u)
            continue;

        inputPitchClassMask = uiInputChordPitchClassMask.load(std::memory_order_relaxed);
        inputBassPitchClass = uiInputChordBassPitchClass.load(std::memory_order_relaxed);
        outputPitchClassMask = uiOutputChordPitchClassMask.load(std::memory_order_relaxed);
        outputBassPitchClass = uiOutputChordBassPitchClass.load(std::memory_order_relaxed);

        const uint32_t endSeq = uiChordSnapshotRevision.load(std::memory_order_acquire);
        if (beginSeq == endSeq && (endSeq & 1u) == 0u)
            return endSeq;
    }
}

bool MidivisiViciAudioProcessor::consumeUiJustStarted() noexcept
{
    return uiPlayJustStarted.exchange(false, std::memory_order_acq_rel);
}

void MidivisiViciAudioProcessor::buildArpeggiatorEquivalenceCache() noexcept
{
    arpEqCacheReady = false;

    for (auto& laneScalars : arpEqScalarRaw)
        laneScalars.fill(nullptr);

    for (auto& laneLayers : arpEqStepChoices)
        for (auto& layerSteps : laneLayers)
            layerSteps.fill(nullptr);

    static constexpr const char* kScalarIds[] =
    {
        ParamIDs::Base::arpeggiatorEnable,
        ParamIDs::Base::arpMode,
        ParamIDs::Base::arpRateMorph,
        ParamIDs::Base::arpDirectionMorph,
        ParamIDs::Base::arpPatternMorph,
        ParamIDs::Base::arpRangeMorph,
        ParamIDs::Base::arpVelocityMorph,
        ParamIDs::Base::arpGrooveMorph,
        ParamIDs::Base::arpGateMorph,
        ParamIDs::Base::arpAccentMorph,
        ParamIDs::Base::arpRateLink,
        ParamIDs::Base::arpDirectionLink,
        ParamIDs::Base::arpPatternLink,
        ParamIDs::Base::arpRangeLink,
        ParamIDs::Base::arpVelocityLink,
        ParamIDs::Base::arpGrooveLink,
        ParamIDs::Base::arpGateLink,
        ParamIDs::Base::arpAccentLink,
        ParamIDs::Base::arpDirectionUnlinkRate,
        ParamIDs::Base::arpPatternUnlinkRate,
        ParamIDs::Base::arpRangeUnlinkRate,
        ParamIDs::Base::arpVelocityUnlinkRate,
        ParamIDs::Base::arpGrooveUnlinkRate,
        ParamIDs::Base::arpGateUnlinkRate,
        ParamIDs::Base::arpAccentUnlinkRate
    };
    static_assert((int) std::size(kScalarIds) == kArpEqScalarCount,
                  "Arp equivalence scalar cache size mismatch");
    static_assert((int) std::size(kArpEqScalarCompareModes) == kArpEqScalarCount,
                  "Arp equivalence scalar compare-mode size mismatch");

    static constexpr const char* kSeqPrefixes[] =
    {
        ParamIDs::Base::arpRateSeqPrefix,
        ParamIDs::Base::arpDirectionSeqPrefix,
        ParamIDs::Base::arpPatternSeqPrefix,
        ParamIDs::Base::arpRangeSeqPrefix,
        ParamIDs::Base::arpVelocitySeqPrefix,
        ParamIDs::Base::arpGrooveSeqPrefix,
        ParamIDs::Base::arpGateSeqPrefix,
        ParamIDs::Base::arpAccentSeqPrefix,
        ParamIDs::Base::drumGraceSeqPrefix,
        ParamIDs::Base::drumVeloEnvSeqPrefix,
        ParamIDs::Base::drumTimEnvSeqPrefix
    };
    static_assert((int) std::size(kSeqPrefixes) == kArpEqSeqLayerCount,
                  "Arp equivalence sequencer cache size mismatch");

    for (int i = 0; i < kNumLanes; ++i)
    {
        const auto lane = Lanes::fromIndex(i);

        for (int s = 0; s < kArpEqScalarCount; ++s)
        {
            const auto pid = ParamIDs::lane(kScalarIds[s], lane);
            arpEqScalarRaw[(size_t) i][(size_t) s] = parameters.getRawParameterValue(pid);
        }

        for (int layer = 0; layer < kArpEqSeqLayerCount; ++layer)
        {
            for (int step = 0; step < kArpEqSeqStepCount; ++step)
            {
                const auto sid = ParamIDs::laneStep(kSeqPrefixes[layer], step + 1, lane);
                arpEqStepChoices[(size_t) i][(size_t) layer][(size_t) step] =
                    dynamic_cast<juce::AudioParameterChoice*>(parameters.getParameter(sid));
            }
        }
    }

    arpEqCacheReady = true;
}

bool MidivisiViciAudioProcessor::areArpeggiatorSettingsEquivalentCached(Lanes::Lane laneA,
                                                                         Lanes::Lane laneB) const noexcept
{
    if (!arpEqCacheReady)
        return false;

    const int idxA = (int) laneA;
    const int idxB = (int) laneB;
    if (!juce::isPositiveAndBelow(idxA, kNumLanes) || !juce::isPositiveAndBelow(idxB, kNumLanes))
        return false;

    for (int s = 0; s < kArpEqScalarCount; ++s)
    {
        const auto* pA = arpEqScalarRaw[(size_t) idxA][(size_t) s];
        const auto* pB = arpEqScalarRaw[(size_t) idxB][(size_t) s];
        if (pA == nullptr || pB == nullptr)
            return false;

        if (!equalArpScalarEffective(pA->load(),
                                     pB->load(),
                                     kArpEqScalarCompareModes[(size_t) s]))
            return false;
    }

    const auto* modeRawA = arpEqScalarRaw[(size_t) idxA][(size_t) kArpEqScalarIndexArpMode];
    if (modeRawA == nullptr)
        return false;
    const int modeIndex = juce::jlimit(0, 1, juce::roundToInt(modeRawA->load()));

    for (int layer = 0; layer < kArpEqSeqLayerCount; ++layer)
    {
        if (!isArpEqSeqLayerRelevantForMode(layer, modeIndex))
            continue;

        for (int step = 0; step < kArpEqSeqStepCount; ++step)
        {
            const auto* pA = arpEqStepChoices[(size_t) idxA][(size_t) layer][(size_t) step];
            const auto* pB = arpEqStepChoices[(size_t) idxB][(size_t) layer][(size_t) step];
            if (pA == nullptr || pB == nullptr)
                return false;

            const int aIdx = normalizeArpStepChoiceForEquivalence(layer, step, pA->getIndex());
            const int bIdx = normalizeArpStepChoiceForEquivalence(layer, step, pB->getIndex());
            if (aIdx != bIdx)
                return false;
        }
    }

    return true;
}

void MidivisiViciAudioProcessor::syncMorphMirrorFromVTS()
{
   #if LOGS_ENABLED
    DBG_LOG("SYNC", "PLUGINPROCESSOR", "MORPH_SYNC", "#810#",
            "Syncing MorphParameterManager from ValueTreeState...");
   #endif

    morphManager.syncFromValueTree();

   #if LOGS_ENABLED
    DBG_LOG("SYNC", "PLUGINPROCESSOR", "MORPH_SYNC", "#811#",
            "MorphParameterManager sync completed.");
   #endif
}

juce::AudioProcessorValueTreeState& MidivisiViciAudioProcessor::getValueTreeState()
{
    return parameters;
}

void MidivisiViciAudioProcessor::sendDebugMessage(juce::MidiBuffer& midiMessages,
                                                 const juce::String& text,
                                                 int samplePosition)
{
    // NOTE: allocates MemoryBlock -> use sparingly (not inside hot RT loops)
    juce::MemoryBlock data(text.toRawUTF8(), (size_t) text.getNumBytesAsUTF8());
    juce::MidiMessage sysEx = juce::MidiMessage::createSysExMessage(
        data.getData(), static_cast<int>(data.getSize()));

    midiMessages.addEvent(sysEx, samplePosition);
}

//==============================================================================
// Global panic helpers (RT-safe usage: write into midiOutputBuffer only)
//==============================================================================

void MidivisiViciAudioProcessor::requestGlobalAllNotesOff() noexcept
{
    pendingGlobalAllNotesOff.store(true, std::memory_order_release);
}

void MidivisiViciAudioProcessor::emitGlobalAllNotesOff(juce::MidiBuffer& midi, int samplePos) noexcept
{
    for (int ch = 1; ch <= 16; ++ch)
    {
        midi.addEvent(juce::MidiMessage::controllerEvent(ch, 120, 0), samplePos); // All Sound Off
        midi.addEvent(juce::MidiMessage::controllerEvent(ch, 123, 0), samplePos); // All Notes Off
        midi.addEvent(juce::MidiMessage::controllerEvent(ch, 64,  0), samplePos); // Sustain Off
        midi.addEvent(juce::MidiMessage::controllerEvent(ch, 66,  0), samplePos); // Sostenuto Off
        midi.addEvent(juce::MidiMessage::controllerEvent(ch, 67,  0), samplePos); // Soft Pedal Off
    }
}

void MidivisiViciAudioProcessor::resetAllNoteTrackingAfterPanic() noexcept
{
    for (auto& byNote : rawHeldCount)
        byNote.fill(0);
    for (auto& byNote : rawHeldVelocity)
        byNote.fill(0);
    for (auto& byNote : rawHeldVelocityStack)
        for (auto& byVel : byNote)
            byVel.fill(0);
    for (auto& byNote : rawHeldVelocityStackSize)
        byNote.fill(0);
    for (auto& byNote : rawHeldOrder)
        byNote.fill(0);
    rawHeldOrderCounter = 0;
    for (auto& byNote : outputHeldCount)
        byNote.fill(0);
    for (auto& byLane : harmonizerHeldCountByLane)
        for (auto& byNote : byLane)
            byNote.fill(0);

    for (int i = 0; i < kNumLanes; ++i)
    {
        if (inputFilterProcessors[i])
            inputFilterProcessors[i]->resetAllTrackingRT();
        if (harmonizerProcessors[i])
            harmonizerProcessors[i]->resetAllTrackingRT();
        if (arpeggiatorProcessors[i])
            arpeggiatorProcessors[i]->resetAllTrackingRT();
        if (splitterProcessors[i])
            splitterProcessors[i]->resetAllTrackingRT();

        for (auto& byNote : laneInputHeldCount[(size_t) i])    byNote.fill(0);
        for (auto& byNote : laneInputHeldVelocity[(size_t) i]) byNote.fill(0);
        for (auto& byNote : laneInputHeldVelocityStack[(size_t) i])
            for (auto& byVel : byNote)
                byVel.fill(0);
        for (auto& byNote : laneInputHeldVelocityStackSize[(size_t) i]) byNote.fill(0);
        for (auto& byNote : laneInputHeldOrder[(size_t) i])    byNote.fill(0);
        laneInputHeldOrderCounter[(size_t) i] = 0;
        lastInputFilterEnabledState[(size_t) i] = false;
        lastDirectRoutingState[(size_t) i] = false;
        lastConsumeRoutingState[(size_t) i] = false;
        lastBlackNotesRoutingState[(size_t) i] = false;
    }
    lastDirectRoutingStateValid = false;
    lastWhiteInputModeState = false;
    lastBlackInputModeState = false;
    lastInputRemapStateValid = false;
    clearChordSnapshotRt();

    localClockWasActive = false;
    localClockPpqAtNextBlock = 0.0;
    lfoPhaseAnchorValid = false;
    lfoPhaseAnchorPpq = 0.0;
    mainBarWasPlaying = false;
    lastProcessedActiveLaneCount =
        juce::jlimit(1, kNumLanes, activeLaneCount.load(std::memory_order_relaxed));

    for (int page = 0; page < kMainBarMaxPages; ++page)
    {
        mainBarStepCursor[(size_t) page] = 0;
        mainBarNextStepPpq[(size_t) page] = 0.0;
        mainBarPlaybackStep[(size_t) page].store(-1, std::memory_order_relaxed);
    }
    for (auto& byCc : mainBarLastSentCcValue)
        byCc.fill(-1);

    for (auto& byNote : whiteModeHeldCount)
        byNote.fill(0);
    for (auto& byCh : whiteModeMappedNoteStack)
        for (auto& byNote : byCh)
            byNote.fill(0);
    for (auto& byCh : whiteModeMappedVelocityStack)
        for (auto& byNote : byCh)
            byNote.fill(0);
    for (auto& byCh : whiteModeMappedNoteStackSize)
        byCh.fill(0);
    for (auto& byCh : whiteModePhysicalHeldCount)
        byCh.fill(0);
    for (auto& byCh : whiteModePhysicalLastVelocity)
        byCh.fill(0);
    for (auto& byCh : blackModeMappedNoteStack)
        for (auto& byNote : byCh)
            byNote.fill(0);
    for (auto& byCh : blackModeMappedVelocityStack)
        for (auto& byNote : byCh)
            byNote.fill(0);
    for (auto& byCh : blackModeMappedNoteStackSize)
        byCh.fill(0);
    for (auto& byCh : blackModePhysicalHeldCount)
        byCh.fill(0);
    for (auto& byCh : blackModePhysicalLastVelocity)
        byCh.fill(0);
    for (auto& byCh : blackModeDeferredOffCount)
        byCh.fill(0);
    whiteModeChordSignature = 0;
    whiteModeChordSignatureValid = false;
    blackModeFiveClassSet = { 0, 2, 4, 7, 9 };
    blackModePendingSetUpdate = false;
    blackModePendingFiveClassSet = { 0, 2, 4, 7, 9 };
}

//==============================================================================
// Plugin entry point
//==============================================================================

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
   #if LOGS_ENABLED
    DBG_LOG("ENTRY", "PLUGINPROCESSOR", "CREATE_PLUGIN", "#902#",
            "createPluginFilter(): new processor instance requested.");
   #endif

    return new MidivisiViciAudioProcessor();
}
