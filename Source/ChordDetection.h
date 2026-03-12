#pragma once

#include <JuceHeader.h>
#include <array>

namespace ChordDetection
{
    enum class Mode : uint8_t
    {
        Major = 0,
        Minor,
        Dorian,
        Phrygian,
        Lydian,
        Mixolydian,
        Locrian,
        HarmonicMinor,
        MelodicMinor,
        Chromatic
    };

    struct Context
    {
        int keyTonic = 0; // 0..11
        Mode mode = Mode::Chromatic;
    };

    struct Alternative
    {
        juce::String symbol;
        float score = 0.0f;
        float confidence = 0.0f;
    };

    struct Result
    {
        juce::String bestChordName { "NoChord" };
        float confidence = 0.0f;
        std::array<Alternative, 3> alternatives {};
        int alternativeCount = 0;
    };

    // Names one chord from pitch classes (bitmask) + bass pitch class.
    Result nameChordFromPitchClassMask(uint16_t pitchClassMask,
                                       int bassPitchClass,
                                       const Context& context = {});

    // Maps APVTS global scale index to the detector mode enum.
    Mode modeFromScaleIndex(int scaleIndex) noexcept;

    // Formats "incoming -> outgoing" string for the MainMenu footer.
    juce::String formatChordFlowForUi(const juce::String& inputChordName,
                                      const juce::String& outputChordName);
}

