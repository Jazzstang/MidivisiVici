#include "ChordDetection.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace
{
    static constexpr std::array<const char*, 12> kPitchClassNames =
    {{
        "C", "C#", "D", "D#", "E", "F",
        "F#", "G", "G#", "A", "A#", "B"
    }};

    struct Candidate
    {
        int rootPc = 0;
        int bassPc = 0;
        juce::String suffix;
        juce::String symbol;
        float score = 0.0f;
        float confidence = 0.0f;
        bool valid = false;
    };

    static inline int wrapPc(int pitchClass) noexcept
    {
        int pc = pitchClass % 12;
        if (pc < 0)
            pc += 12;
        return pc;
    }

    static inline juce::String pitchClassName(int pitchClass)
    {
        return kPitchClassNames[(size_t) wrapPc(pitchClass)];
    }

    static uint16_t buildScaleMask(int tonicPc, ChordDetection::Mode mode) noexcept
    {
        tonicPc = wrapPc(tonicPc);

        std::array<int, 7> pattern {};
        switch (mode)
        {
            case ChordDetection::Mode::Major:         pattern = { 0, 2, 4, 5, 7, 9, 11 }; break;
            case ChordDetection::Mode::Minor:         pattern = { 0, 2, 3, 5, 7, 8, 10 }; break;
            case ChordDetection::Mode::Dorian:        pattern = { 0, 2, 3, 5, 7, 9, 10 }; break;
            case ChordDetection::Mode::Phrygian:      pattern = { 0, 1, 3, 5, 7, 8, 10 }; break;
            case ChordDetection::Mode::Lydian:        pattern = { 0, 2, 4, 6, 7, 9, 11 }; break;
            case ChordDetection::Mode::Mixolydian:    pattern = { 0, 2, 4, 5, 7, 9, 10 }; break;
            case ChordDetection::Mode::Locrian:       pattern = { 0, 1, 3, 5, 6, 8, 10 }; break;
            case ChordDetection::Mode::HarmonicMinor: pattern = { 0, 2, 3, 5, 7, 8, 11 }; break;
            case ChordDetection::Mode::MelodicMinor:  pattern = { 0, 2, 3, 5, 7, 9, 11 }; break;
            case ChordDetection::Mode::Chromatic:
            default:
                return 0x0FFFu;
        }

        uint16_t mask = 0;
        for (int interval : pattern)
            mask = (uint16_t) (mask | (uint16_t) (1u << wrapPc(tonicPc + interval)));
        return mask;
    }

    static int countSetBits12(uint16_t value) noexcept
    {
        int count = 0;
        value &= 0x0FFFu;
        while (value != 0)
        {
            count += (int) (value & 1u);
            value = (uint16_t) (value >> 1u);
        }
        return count;
    }

    static int findOnlySetBit12(uint16_t value) noexcept
    {
        value &= 0x0FFFu;
        for (int bit = 0; bit < 12; ++bit)
            if ((value & (uint16_t) (1u << bit)) != 0)
                return bit;
        return -1;
    }

    static inline bool hasInterval(uint16_t intervalMask, int semitone) noexcept
    {
        return (intervalMask & (uint16_t) (1u << wrapPc(semitone))) != 0;
    }

    static uint16_t buildIntervalsFromRoot(uint16_t pitchClassMask, int rootPc) noexcept
    {
        uint16_t intervals = 0;
        for (int pc = 0; pc < 12; ++pc)
        {
            if ((pitchClassMask & (uint16_t) (1u << pc)) == 0)
                continue;

            const int interval = wrapPc(pc - rootPc);
            intervals = (uint16_t) (intervals | (uint16_t) (1u << interval));
        }
        return intervals;
    }

    static Candidate buildCandidate(uint16_t pitchClassMask,
                                    int rootPc,
                                    int bassPc,
                                    uint16_t keyMask)
    {
        Candidate c;
        c.rootPc = wrapPc(rootPc);
        c.bassPc = wrapPc(bassPc);

        const uint16_t iv = buildIntervalsFromRoot(pitchClassMask, c.rootPc);
        const bool has2 = hasInterval(iv, 2);
        const bool has3 = hasInterval(iv, 3);
        const bool has4 = hasInterval(iv, 4);
        const bool has5 = hasInterval(iv, 5);
        const bool has6 = hasInterval(iv, 6);
        const bool has7 = hasInterval(iv, 7);
        const bool has8 = hasInterval(iv, 8);
        const bool has9 = hasInterval(iv, 9);
        const bool has10 = hasInterval(iv, 10);
        const bool has11 = hasInterval(iv, 11);

        // Core quality + suffix.
        if (has4 && has7)
        {
            c.suffix = has11 ? "maj7" : (has10 ? "7" : "M");
            c.score += 3.0f;
        }
        else if (has3 && has7)
        {
            c.suffix = has11 ? "m(maj7)" : (has10 ? "m7" : "m");
            c.score += 3.0f;
        }
        else if (has3 && has6)
        {
            c.suffix = has9 ? "dim7" : (has10 ? "m7b5" : "dim");
            c.score += 2.8f;
        }
        else if (has4 && has8)
        {
            c.suffix = has11 ? "maj7#5" : (has10 ? "7#5" : "+");
            c.score += 2.7f;
        }
        else if (has2 && has7 && !has3 && !has4)
        {
            c.suffix = has10 ? "7sus2" : "sus2";
            c.score += 2.2f;
        }
        else if (has5 && has7 && !has3 && !has4)
        {
            c.suffix = has10 ? "7sus4" : "sus4";
            c.score += 2.2f;
        }
        else if (has7 && !has2 && !has3 && !has4 && !has5)
        {
            c.suffix = "5";
            c.score += 1.8f;
        }
        else
        {
            return c;
        }

        const bool hasThirdFamily = has3 || has4;
        const bool hasFifthFamily = has6 || has7 || has8;
        c.score += hasThirdFamily ? 1.2f : 0.0f;
        c.score += hasFifthFamily ? 1.0f : 0.0f;

        if (c.bassPc == c.rootPc)
            c.score += 1.5f;
        else
            c.score += 0.2f;

        if ((keyMask & (uint16_t) (1u << c.rootPc)) != 0)
            c.score += 0.6f;
        else
            c.score -= 0.2f;

        int extensionPenalty = 0;
        extensionPenalty += hasInterval(iv, 1) ? 1 : 0;
        extensionPenalty += hasInterval(iv, 6) ? 1 : 0;
        extensionPenalty += hasInterval(iv, 8) ? 1 : 0;
        c.score -= 0.15f * (float) extensionPenalty;

        c.symbol = pitchClassName(c.rootPc) + c.suffix;
        if (c.bassPc != c.rootPc)
            c.symbol += "/" + pitchClassName(c.bassPc);

        c.confidence = juce::jlimit(0.0f, 1.0f, 0.18f + (c.score / 10.0f));
        c.valid = true;
        return c;
    }

    static juce::String buildFallbackClusterName(uint16_t pitchClassMask, int bassPc)
    {
        juce::String cluster = "Cluster(";
        bool first = true;
        for (int pc = 0; pc < 12; ++pc)
        {
            if ((pitchClassMask & (uint16_t) (1u << pc)) == 0)
                continue;

            if (!first)
                cluster += ",";
            cluster += pitchClassName(pc);
            first = false;
        }
        cluster += ")";

        if (bassPc >= 0)
            cluster += "/" + pitchClassName(bassPc);

        return cluster;
    }
}

namespace ChordDetection
{
    Result nameChordFromPitchClassMask(uint16_t pitchClassMask,
                                       int bassPitchClass,
                                       const Context& context)
    {
        Result out;

        if ((pitchClassMask & 0x0FFFu) == 0)
            return out;

        pitchClassMask &= 0x0FFFu;
        bassPitchClass = (bassPitchClass >= 0) ? wrapPc(bassPitchClass) : -1;

        const int pcCount = countSetBits12(pitchClassMask);
        if (pcCount == 1)
        {
            const int pc = findOnlySetBit12(pitchClassMask);
            out.bestChordName = pitchClassName(pc);
            out.confidence = 0.15f;
            out.alternatives[0] = { out.bestChordName, 0.5f, out.confidence };
            out.alternativeCount = 1;
            return out;
        }

        const uint16_t keyMask = buildScaleMask(context.keyTonic, context.mode);
        std::vector<Candidate> candidates;
        candidates.reserve(12);

        for (int rootPc = 0; rootPc < 12; ++rootPc)
        {
            if ((pitchClassMask & (uint16_t) (1u << rootPc)) == 0)
                continue;

            auto candidate = buildCandidate(pitchClassMask,
                                            rootPc,
                                            (bassPitchClass >= 0 ? bassPitchClass : rootPc),
                                            keyMask);
            if (candidate.valid)
                candidates.push_back(std::move(candidate));
        }

        if (candidates.empty())
        {
            out.bestChordName = buildFallbackClusterName(pitchClassMask, bassPitchClass);
            out.confidence = 0.15f;
            out.alternatives[0] = { out.bestChordName, 0.15f, out.confidence };
            out.alternativeCount = 1;
            return out;
        }

        std::sort(candidates.begin(),
                  candidates.end(),
                  [](const Candidate& a, const Candidate& b)
                  {
                      if (a.score != b.score)
                          return a.score > b.score;
                      if (a.confidence != b.confidence)
                          return a.confidence > b.confidence;
                      if (a.symbol.length() != b.symbol.length())
                          return a.symbol.length() < b.symbol.length();
                      return a.symbol.compareNatural(b.symbol) < 0;
                  });

        out.bestChordName = candidates.front().symbol;
        out.confidence = candidates.front().confidence;

        const int altCount = juce::jlimit(0, 3, (int) candidates.size());
        for (int i = 0; i < altCount; ++i)
        {
            out.alternatives[(size_t) i] =
            {
                candidates[(size_t) i].symbol,
                candidates[(size_t) i].score,
                candidates[(size_t) i].confidence
            };
        }
        out.alternativeCount = altCount;
        return out;
    }

    Mode modeFromScaleIndex(int scaleIndex) noexcept
    {
        switch (juce::jlimit(0, 9, scaleIndex))
        {
            case 0:  return Mode::Major;
            case 1:  return Mode::Minor;
            case 2:  return Mode::Dorian;
            case 3:  return Mode::Phrygian;
            case 4:  return Mode::Lydian;
            case 5:  return Mode::Mixolydian;
            case 6:  return Mode::Locrian;
            case 7:  return Mode::HarmonicMinor;
            case 8:  return Mode::MelodicMinor;
            case 9:  return Mode::Chromatic;
            default: break;
        }

        return Mode::Chromatic;
    }

    juce::String formatChordFlowForUi(const juce::String& inputChordName,
                                      const juce::String& outputChordName)
    {
        const juce::String safeIn = inputChordName.isNotEmpty() ? inputChordName : "NoChord";
        const juce::String safeOut = outputChordName.isNotEmpty() ? outputChordName : "NoChord";
        const juce::String arrow = juce::String::fromUTF8("\xE2\x86\x92");
        return safeIn + " " + arrow + " " + safeOut;
    }
}
