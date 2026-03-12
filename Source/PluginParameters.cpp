//==============================================================================
// PluginParameters.cpp
//
// MidivisiVici - Definitions centralisees des parametres
//
// Systeme de lanes:
//   - jusqu a 16 lanes (A..P) pour InputFilter/Harmonizer/Arpeggiator/Splitter.
//   - Les monitors restent single-instance.
//
// Regles projet:
//   - Commentaires/logs en ASCII.
//   - Creation de parametres uniquement en init-time (non-RT).
//
// Ce fichier definit:
//   - createParameterLayout(): aggregation complete APVTS
//   - ParameterBuilders::createXXXParameters(lane)
//   - DebugUtils::dumpParameters(state)
//
// Points importants:
//   - InputFilter expose consume/direct/hold par lane.
//   - Les IDs doivent rester strictement alignes avec ParamIDs::Base/Monitor.
//==============================================================================

#include "PluginParameters.h"
#include "DebugConfig.h"

#include <utility>

//==============================================================================
// 1) Macro helpers
//------------------------------------------------------------------------------
// Kept for future morphable triplets.
// Current arpeggiator morph params are plain floats (no triplets yet).
//------------------------------------------------------------------------------
#define ADD_MORPHABLE_PARAM(vec, id, name, range, defaultValue)                   \
    do {                                                                          \
        vec.push_back(std::make_unique<juce::AudioParameterFloat>(                \
            juce::String(id) + "_X", juce::String(name) + " X", range, defaultValue)); \
        vec.push_back(std::make_unique<juce::AudioParameterFloat>(                \
            juce::String(id) + "_Y", juce::String(name) + " Y", range, defaultValue)); \
        vec.push_back(std::make_unique<juce::AudioParameterBool>(                 \
            juce::String(id) + "_Mode", juce::String(name) + " Morph", true));    \
    } while (false)

//==============================================================================
// 2) Layout entry point
//==============================================================================

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    auto append = [&](std::vector<std::unique_ptr<juce::RangedAudioParameter>> moduleParams)
    {
        std::move(moduleParams.begin(), moduleParams.end(), std::back_inserter(params));
    };

    // Global controls (single instance)
    append(ParameterBuilders::createGlobalHarmonizerParameters());

    // Monitors (single instance)
    append(ParameterBuilders::createInputMonitorParameters());
    append(ParameterBuilders::createOutputMonitorParameters());

    // Lane-based modules (A..P)
    for (int i = 0; i < Lanes::kNumLanes; ++i)
    {
        const auto lane = Lanes::fromIndex(i);

        append(ParameterBuilders::createInputFilterParameters(lane));
        append(ParameterBuilders::createHarmonizerParameters(lane));
        append(ParameterBuilders::createArpeggiatorParameters(lane));
        append(ParameterBuilders::createVoiceSplitterParameters(lane));
    }

    return { params.begin(), params.end() };
}

//==============================================================================
// 3) Parameter builders
//==============================================================================

namespace ParameterBuilders
{
    //--------------------------------------------------------------------------
    // Global harmonizer controls (single instance)
    //--------------------------------------------------------------------------
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> createGlobalHarmonizerParameters()
    {
        std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            ParamIDs::Global::harmGlobalKey, "Global Key",
            juce::StringArray { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B", "Off" }, 0));

        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            ParamIDs::Global::harmGlobalScale, "Global Scale",
            juce::StringArray { "Major", "Minor", "Dorian", "Phrygian", "Lydian",
                                "Mixolydian", "Locrian", "Harmonic Minor", "Melodic Minor", "Chromatic" }, 0));

        // MainMenu keyboard remap modes:
        // - White mode: remap white-key pitch classes to selected key/mode.
        // - Black mode: reserved for future black-key mapping behavior.
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            ParamIDs::Global::whiteInputModeToggle, "White Input Mode Toggle", false));
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            ParamIDs::Global::blackInputModeToggle, "Black Input Mode Toggle", false));

        return params;
    }

    //--------------------------------------------------------------------------
    // Input Monitor (single instance)
    //--------------------------------------------------------------------------
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> createInputMonitorParameters()
    {
        std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

        params.push_back(std::make_unique<juce::AudioParameterBool>(
            ParamIDs::Monitor::inputMonitorEnable, "Input Monitor Enable", true));

        params.push_back(std::make_unique<juce::AudioParameterBool>(
            ParamIDs::Monitor::inputMonitorFilterNote, "Input Monitor Notes", true));

        params.push_back(std::make_unique<juce::AudioParameterBool>(
            ParamIDs::Monitor::inputMonitorFilterControl, "Input Monitor Controls", true));

        params.push_back(std::make_unique<juce::AudioParameterBool>(
            ParamIDs::Monitor::inputMonitorFilterClock, "Input Monitor Clock", true));

        params.push_back(std::make_unique<juce::AudioParameterBool>(
            ParamIDs::Monitor::inputMonitorFilterEvent, "Input Monitor Event", true));

        return params;
    }

    //--------------------------------------------------------------------------
    // Output Monitor (single instance)
    //--------------------------------------------------------------------------
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> createOutputMonitorParameters()
    {
        std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

        params.push_back(std::make_unique<juce::AudioParameterBool>(
            ParamIDs::Monitor::outputMonitorEnable, "Output Monitor Enable", true));

        params.push_back(std::make_unique<juce::AudioParameterBool>(
            ParamIDs::Monitor::outputMonitorFilterNote, "Output Monitor Notes", true));

        params.push_back(std::make_unique<juce::AudioParameterBool>(
            ParamIDs::Monitor::outputMonitorFilterControl, "Output Monitor Controls", true));

        params.push_back(std::make_unique<juce::AudioParameterBool>(
            ParamIDs::Monitor::outputMonitorFilterClock, "Output Monitor Clock", true));

        params.push_back(std::make_unique<juce::AudioParameterBool>(
            ParamIDs::Monitor::outputMonitorFilterEvent, "Output Monitor Event", true));

        return params;
    }

    //--------------------------------------------------------------------------
    // Input Filter (lane-based)
    //--------------------------------------------------------------------------
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> createInputFilterParameters(Lanes::Lane lane)
    {
        std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

        auto id = [&](const char* baseId) { return ParamIDs::lane(baseId, lane); };

        // Enable / routing
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            id(ParamIDs::Base::inputFilterEnable), "Input Filter Enable", true));

        // Consume toggle (routing-level behavior)
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            id(ParamIDs::Base::inputFilterConsume), "Input Filter Consume", false));

        // Direct source override (routing-level behavior)
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            id(ParamIDs::Base::inputFilterDirect), "Input Filter Direct", false));
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            id(ParamIDs::Base::inputFilterHold), "Input Filter Hold", false));
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            id(ParamIDs::Base::inputFilterBlackNotes), "Input Filter Black Notes", false));

        // Global-ish lane input shaping
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            id(ParamIDs::Base::inputMuteToggle), "Input Mute Toggle", false));

        params.push_back(std::make_unique<juce::AudioParameterInt>(
            id(ParamIDs::Base::channelFilter), "Channel Filter", 1, 16, 1));

        params.push_back(std::make_unique<juce::AudioParameterBool>(
            id(ParamIDs::Base::whiteKeyModeToggle), "White Key Mode Toggle", false));

        // Note
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            id(ParamIDs::Base::noteFilterToggle), "Note Filter Toggle", false));

        params.push_back(std::make_unique<juce::AudioParameterInt>(
            id(ParamIDs::Base::noteMin), "Note Min", 0, 127, 0));

        params.push_back(std::make_unique<juce::AudioParameterInt>(
            id(ParamIDs::Base::noteMax), "Note Max", 0, 127, 127));

        // Velocity
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            id(ParamIDs::Base::velocityFilterToggle), "Velocity Filter Toggle", false));

        params.push_back(std::make_unique<juce::AudioParameterInt>(
            id(ParamIDs::Base::velocityMin), "Velocity Min", 0, 127, 0));

        params.push_back(std::make_unique<juce::AudioParameterInt>(
            id(ParamIDs::Base::velocityMax), "Velocity Max", 0, 127, 127));

        // Step
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            id(ParamIDs::Base::stepFilterToggle), "Step Filter Toggle", false));

        params.push_back(std::make_unique<juce::AudioParameterInt>(
            id(ParamIDs::Base::stepFilterNumerator), "Step Filter Numerator", 1, 16, 1));

        params.push_back(std::make_unique<juce::AudioParameterInt>(
            id(ParamIDs::Base::stepFilterDenominator), "Step Filter Denominator", 1, 16, 1));

        // Voice limit
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            id(ParamIDs::Base::voiceLimitToggle), "Voice Limit Toggle", false));

        params.push_back(std::make_unique<juce::AudioParameterInt>(
            id(ParamIDs::Base::voiceLimit), "Voice Limit", 1, 16, 16));

        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            id(ParamIDs::Base::priority), "Voice Priority",
            juce::StringArray { "Last", "Lowest", "Highest" }, 0));

        return params;
    }

    //--------------------------------------------------------------------------
    // Harmonizer (lane-based)
    //--------------------------------------------------------------------------
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> createHarmonizerParameters(Lanes::Lane lane)
    {
        std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

        auto id = [&](const char* baseId) { return ParamIDs::lane(baseId, lane); };

        params.push_back(std::make_unique<juce::AudioParameterBool>(
            id(ParamIDs::Base::harmonizerEnable), "Enable Harmonizer", true));

        // Additional voices:
        // - range is semitone offset relative to the base note
        // - 0 means "voice disabled" and must stay available at all times
        // - default is 0 so a fresh lane starts with only the base voice active
        params.push_back(std::make_unique<juce::AudioParameterInt>(id(ParamIDs::Base::harmVoice2), "Voice 2", -24, 24, 0));
        params.push_back(std::make_unique<juce::AudioParameterInt>(id(ParamIDs::Base::harmVoice3), "Voice 3", -24, 24, 0));
        params.push_back(std::make_unique<juce::AudioParameterInt>(id(ParamIDs::Base::harmVoice4), "Voice 4", -24, 24, 0));
        params.push_back(std::make_unique<juce::AudioParameterInt>(id(ParamIDs::Base::harmVoice5), "Voice 5", -24, 24, 0));

        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            id(ParamIDs::Base::harmKey), "Key",
            juce::StringArray { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }, 0));

        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            id(ParamIDs::Base::harmScale), "Scale",
            juce::StringArray { "Major", "Minor", "Dorian", "Phrygian", "Lydian",
                                "Mixolydian", "Locrian", "Harmonic Minor", "Melodic Minor" }, 0));

        // These are numeric ranges used in HarmonizerProcessor (0..8).
        params.push_back(std::make_unique<juce::AudioParameterInt>(
            id(ParamIDs::Base::harmOctavePlusRandom), "Octave + Range", 0, 8, 0));

        params.push_back(std::make_unique<juce::AudioParameterInt>(
            id(ParamIDs::Base::harmOctaveMinusRandom), "Octave - Range", 0, 8, 0));

        params.push_back(std::make_unique<juce::AudioParameterInt>(
            id(ParamIDs::Base::harmPitchCorrector), "Pitch Corrector", -12, 12, 0));

        params.push_back(std::make_unique<juce::AudioParameterInt>(id(ParamIDs::Base::harmVoice2VelMod), "Voice 2 Velocity Mod", -10, 10, 0));
        params.push_back(std::make_unique<juce::AudioParameterInt>(id(ParamIDs::Base::harmVoice3VelMod), "Voice 3 Velocity Mod", -10, 10, 0));
        params.push_back(std::make_unique<juce::AudioParameterInt>(id(ParamIDs::Base::harmVoice4VelMod), "Voice 4 Velocity Mod", -10, 10, 0));
        params.push_back(std::make_unique<juce::AudioParameterInt>(id(ParamIDs::Base::harmVoice5VelMod), "Voice 5 Velocity Mod", -10, 10, 0));

        return params;
    }

    //--------------------------------------------------------------------------
    // Arpeggiator (lane-based)
    //--------------------------------------------------------------------------
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> createArpeggiatorParameters(Lanes::Lane lane)
    {
        std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

        auto id = [&](const char* baseId) { return ParamIDs::lane(baseId, lane); };

        // Enable
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            id(ParamIDs::Base::arpeggiatorEnable), "Enable Arpeggiator", true));

        // 2-state mode selector:
        // 0 = Arpeggiator, 1 = Drum Machine.
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            id(ParamIDs::Base::arpMode),
            "Arpeggiator Mode",
            juce::StringArray { "Arpeggiator", "Drum Machine" },
            0));

        // Morph floats
        params.push_back(std::make_unique<juce::AudioParameterFloat>(id(ParamIDs::Base::arpRateMorph),      "Rate Morph",      -100.f, 100.f, 0.f));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(id(ParamIDs::Base::arpDirectionMorph), "Strum Morph",     -100.f, 100.f, 0.f));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(id(ParamIDs::Base::arpPatternMorph),   "Jump Morph",      -100.f, 100.f, 0.f));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(id(ParamIDs::Base::arpRangeMorph),     "Octave Morph",    -100.f, 100.f, 0.f));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(id(ParamIDs::Base::arpVelocityMorph),  "Velocity Morph",  -100.f, 100.f, 0.f));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(id(ParamIDs::Base::arpGrooveMorph),    "Groove Morph",    -100.f, 100.f, 0.f));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(id(ParamIDs::Base::arpGateMorph),      "Gate Morph",      -100.f, 100.f, 0.f));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(id(ParamIDs::Base::arpAccentMorph),    "Retrig Morph",    -100.f, 100.f, 0.f));

        // Link modes (non-Rythm layers):
        // index 0 = Link (old Unlink behavior: follows rate steps, no wrap reset)
        // index 1 = F    (old Link behavior: follows rate steps, reset on rate wrap)
        // index 2 = Unlink (autonomous own-rate sequencer clock)
        //
        // Non-Rythm ordering keeps backward compatibility with old bool states:
        // - saved 0.0f -> Link
        // - saved 1.0f -> F
        //
        // Rythm is binary:
        // - R: reset/restart by held-note activity
        // - F: free-run on DAW timeline
        const juce::StringArray linkModeChoices { "Link", "F", "Unlink" };
        const juce::StringArray rythmSyncChoices { "R", "F" };
        params.push_back(std::make_unique<juce::AudioParameterChoice>(id(ParamIDs::Base::arpRateLink),      "Rythm Sync",     rythmSyncChoices, 0));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(id(ParamIDs::Base::arpDirectionLink), "Strum Link",     linkModeChoices, 1));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(id(ParamIDs::Base::arpPatternLink),   "Jump Link",      linkModeChoices, 1));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(id(ParamIDs::Base::arpRangeLink),     "Octave Link",    linkModeChoices, 1));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(id(ParamIDs::Base::arpVelocityLink),  "Velocity Link",  linkModeChoices, 1));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(id(ParamIDs::Base::arpGrooveLink),    "Groove Link",    linkModeChoices, 1));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(id(ParamIDs::Base::arpGateLink),      "Gate Link",      linkModeChoices, 1));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(id(ParamIDs::Base::arpAccentLink),    "Retrig Link",    linkModeChoices, 1));

        // Choice lists (must match StepToggle::arpStepLayerChoices exactly).
        const juce::StringArray rateChoices {
            "Skip",
            // Binaries (1:1)
            "1/1", "1/2", "1/4", "1/8", "1/16", "1/32", "1/64",
            // Dotted (3:2)
            "3/2", "3/4", "3/8", "3/16", "3/32", "3/64", "3/128",
            // Double dotted (7:4)
            "7/4", "7/8", "7/16", "7/32", "7/64", "7/128", "7/256",
            // Triolet (3:2)
            "2/3", "1/3", "1/6", "1/12", "1/24", "1/48", "1/96",
            // Quintolet (5:4)
            "4/5", "2/5", "1/5", "1/10", "1/20", "1/40", "1/80",
            // Septolet (7:4)
            "4/7", "2/7", "1/7", "1/14", "1/28", "1/56", "1/112",
            "Rnd"
        };

        // Autonomous rate used when a non-rate layer is in "Unlink" mode.
        // It shares the same menu/range as Rythm.
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            id(ParamIDs::Base::arpDirectionUnlinkRate), "Strum Unlink Rate", rateChoices, 5));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            id(ParamIDs::Base::arpPatternUnlinkRate), "Jump Unlink Rate", rateChoices, 5));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            id(ParamIDs::Base::arpRangeUnlinkRate), "Octave Unlink Rate", rateChoices, 5));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            id(ParamIDs::Base::arpVelocityUnlinkRate), "Velocity Unlink Rate", rateChoices, 5));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            id(ParamIDs::Base::arpGrooveUnlinkRate), "Groove Unlink Rate", rateChoices, 5));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            id(ParamIDs::Base::arpGateUnlinkRate), "Gate Unlink Rate", rateChoices, 5));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            id(ParamIDs::Base::arpAccentUnlinkRate), "Retrig Unlink Rate", rateChoices, 5));

        const juce::StringArray directionChoices = []()
        {
            juce::StringArray arr;
            const auto up = juce::String::fromUTF8("\xE2\x86\x91");          // up arrow glyph
            const auto down = juce::String::fromUTF8("\xE2\x86\x93");        // down arrow glyph
            const auto top = juce::String::fromUTF8("\xE2\xA4\x92");         // up arrow to bar glyph
            const auto bottom = juce::String::fromUTF8("\xE2\xA4\x93");      // down arrow to bar glyph

            arr.add("Skip");
            arr.add(top);
            arr.add(up);
            arr.add("=");
            arr.add(down);
            arr.add(bottom);
            arr.add("Chord");
            arr.add(up + " + " + up);
            arr.add(down + " + " + down);
            arr.add("Mute");
            arr.add("Rnd");

            arr.add("Chord " + up + " 1/4");
            arr.add("Chord " + up + " 1/3");
            arr.add("Chord " + up + " 1/2");
            arr.add("Chord " + up + " 2/3");
            arr.add("Chord " + up + " 3/4");
            arr.add("Chord " + up + " 1/1");
            arr.add("Chord " + down + " 1/4");
            arr.add("Chord " + down + " 1/3");
            arr.add("Chord " + down + " 1/2");
            arr.add("Chord " + down + " 2/3");
            arr.add("Chord " + down + " 3/4");
            arr.add("Chord " + down + " 1/1");
            arr.add("Chord Rnd");
            return arr;
        }();

        const juce::StringArray patternChoices {
            "Skip",
            "1", "2", "3", "4", "5", "6", "7", "8",
            "Rnd"
        };

        const juce::StringArray octaveChoices {
            "Skip",
            "-4", "-3", "-2", "-1",
            "0",
            "+1", "+2", "+3", "+4",
            "Rnd"
        };

        const juce::StringArray velocityChoices {
            "Skip",
            "ppp", // source velocity -100% (clamped to 1)
            "pp",  // source velocity -66%
            "p",   // source velocity -33%
            "=",   // fixed neutral (not morphed)
            "mf",  // neutral value (morphable)
            "f",   // source velocity +33%
            "ff",  // source velocity +66%
            "fff"  // source velocity +100% (clamped to 127)
        };

        juce::StringArray grooveChoices {
            "Skip",
            "-75",
            "-66",
            "-50",
            "-33",
            "-25%",
            "-10%",
            "-5%",
            "=",
            "+5%",
            "+10%",
            "+25%",
            "+33%",
            "+50%",
            "+66%",
            "+75%"
        };

        juce::StringArray gateChoices {
            "Skip",
            "5%",
            "10%",
            "25%",
            "33%",
            "50%",
            "66%",
            "75%",
            "99%",
            "Tie"
        };

        juce::StringArray accentChoices { "Skip" };
        for (int i = 1; i <= 8; ++i)
            accentChoices.add("x" + juce::String(i));
        accentChoices.add("Rnd");

        // Drum-machine specific pages (independent from Strum/Jump/Octave).
        const juce::StringArray drumHitChoices {
            "Skip", "X", "O", "Flam", "Drag"
        };
        const juce::StringArray drumEnvChoices {
            "Skip", "-", "lin", "log", "rlin", "rlog", "rnd"
        };

        // 32 steps per layer, lane-suffixed IDs:
        for (int step = 1; step <= 32; ++step)
        {
            params.push_back(std::make_unique<juce::AudioParameterChoice>(
                ParamIDs::laneStep(ParamIDs::Base::arpRateSeqPrefix, step, lane),
                "Rate Step " + juce::String(step),
                rateChoices,
                (step == 1 ? 5 : 0))); // first step=1/16, others Skip
        }

        for (int step = 1; step <= 32; ++step)
        {
            params.push_back(std::make_unique<juce::AudioParameterChoice>(
                ParamIDs::laneStep(ParamIDs::Base::arpDirectionSeqPrefix, step, lane),
                "Strum Step " + juce::String(step),
                directionChoices,
                (step == 1 ? 2 : 0))); // first step=Up, others Skip
        }

        for (int step = 1; step <= 32; ++step)
        {
            params.push_back(std::make_unique<juce::AudioParameterChoice>(
                ParamIDs::laneStep(ParamIDs::Base::arpPatternSeqPrefix, step, lane),
                "Jump Step " + juce::String(step),
                patternChoices,
                (step == 1 ? 1 : 0))); // first step=1, others Skip
        }

        for (int step = 1; step <= 32; ++step)
        {
            params.push_back(std::make_unique<juce::AudioParameterChoice>(
                ParamIDs::laneStep(ParamIDs::Base::arpRangeSeqPrefix, step, lane),
                "Octave Step " + juce::String(step),
                octaveChoices,
                (step == 1 ? 5 : 0))); // first step=octave 0, others Skip
        }

        for (int step = 1; step <= 32; ++step)
        {
            params.push_back(std::make_unique<juce::AudioParameterChoice>(
                ParamIDs::laneStep(ParamIDs::Base::arpVelocitySeqPrefix, step, lane),
                "Velocity Step " + juce::String(step),
                velocityChoices,
                (step == 1 ? 4 : 0))); // first step "=", others Skip
        }

        for (int step = 1; step <= 32; ++step)
        {
            params.push_back(std::make_unique<juce::AudioParameterChoice>(
                ParamIDs::laneStep(ParamIDs::Base::arpGrooveSeqPrefix, step, lane),
                "Groove Step " + juce::String(step),
                grooveChoices,
                (step == 1 ? 8 : 0))); // first step "=", others Skip
        }

        for (int step = 1; step <= 32; ++step)
        {
            params.push_back(std::make_unique<juce::AudioParameterChoice>(
                ParamIDs::laneStep(ParamIDs::Base::arpGateSeqPrefix, step, lane),
                "Gate Step " + juce::String(step),
                gateChoices,
                (step == 1 ? 8 : 0))); // first step=99%, others Skip
        }

        for (int step = 1; step <= 32; ++step)
        {
            params.push_back(std::make_unique<juce::AudioParameterChoice>(
                ParamIDs::laneStep(ParamIDs::Base::arpAccentSeqPrefix, step, lane),
                "Retrig Step " + juce::String(step),
                accentChoices,
                (step == 1 ? 1 : 0))); // first step=x1, others Skip
        }

        // Independent Drum pages:
        // - Hit page
        // - Velo Env page
        // - Tim Env page
        for (int step = 1; step <= 32; ++step)
        {
            params.push_back(std::make_unique<juce::AudioParameterChoice>(
                ParamIDs::laneStep(ParamIDs::Base::drumGraceSeqPrefix, step, lane),
                "Drum Hit Step " + juce::String(step),
                drumHitChoices,
                (step == 1 ? 1 : 0))); // first step=X, others Skip
        }

        for (int step = 1; step <= 32; ++step)
        {
            params.push_back(std::make_unique<juce::AudioParameterChoice>(
                ParamIDs::laneStep(ParamIDs::Base::drumVeloEnvSeqPrefix, step, lane),
                "Drum Velo Env Step " + juce::String(step),
                drumEnvChoices,
                (step == 1 ? 1 : 0))); // first step=0, others Skip
        }

        for (int step = 1; step <= 32; ++step)
        {
            params.push_back(std::make_unique<juce::AudioParameterChoice>(
                ParamIDs::laneStep(ParamIDs::Base::drumTimEnvSeqPrefix, step, lane),
                "Drum Tim Env Step " + juce::String(step),
                drumEnvChoices,
                (step == 1 ? 1 : 0))); // first step=0, others Skip
        }

        return params;
    }

    //--------------------------------------------------------------------------
    // Splitter (lane-based)
    //--------------------------------------------------------------------------
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> createVoiceSplitterParameters(Lanes::Lane lane)
    {
        std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

        auto id = [&](const char* baseId) { return ParamIDs::lane(baseId, lane); };

        params.push_back(std::make_unique<juce::AudioParameterBool>(
            id(ParamIDs::Base::splitterEnable),
            "Enable Splitter",
            true));

        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            id(ParamIDs::Base::splitterMode),
            "Splitter Mode",
            juce::StringArray { "RoundRobin", "RangeSplit" },
            0));

        // Line 1 (special: channel choice includes "Mute")
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            id(ParamIDs::Base::splitLineActive01),
            "Line 1 Active",
            true));

        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            id(ParamIDs::Base::splitLine1Channel),
            "Line 1 Channel",
            juce::StringArray {
                "Mute", "1", "2", "3", "4", "5", "6", "7", "8",
                "9", "10", "11", "12", "13", "14", "15", "16"
            },
            1)); // default channel 1

        // Lines 2..5
        params.push_back(std::make_unique<juce::AudioParameterBool>(id(ParamIDs::Base::splitLineActive02), "Line 2 Active", false));
        params.push_back(std::make_unique<juce::AudioParameterInt>( id(ParamIDs::Base::splitLine2VoiceLimit), "Line 2 Voice Limit", 1, 16, 16));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(id(ParamIDs::Base::splitLine2Priority), "Line 2 Priority", juce::StringArray { "Last", "Lowest", "Highest" }, 0));
        params.push_back(std::make_unique<juce::AudioParameterInt>( id(ParamIDs::Base::splitLine2NoteMin), "Line 2 Note Min", 0, 127, 0));
        params.push_back(std::make_unique<juce::AudioParameterInt>( id(ParamIDs::Base::splitLine2NoteMax), "Line 2 Note Max", 0, 127, 127));
        params.push_back(std::make_unique<juce::AudioParameterInt>( id(ParamIDs::Base::splitLine2Channel), "Line 2 Channel", 1, 16, 2));

        params.push_back(std::make_unique<juce::AudioParameterBool>(id(ParamIDs::Base::splitLineActive03), "Line 3 Active", false));
        params.push_back(std::make_unique<juce::AudioParameterInt>( id(ParamIDs::Base::splitLine3VoiceLimit), "Line 3 Voice Limit", 1, 16, 16));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(id(ParamIDs::Base::splitLine3Priority), "Line 3 Priority", juce::StringArray { "Last", "Lowest", "Highest" }, 0));
        params.push_back(std::make_unique<juce::AudioParameterInt>( id(ParamIDs::Base::splitLine3NoteMin), "Line 3 Note Min", 0, 127, 0));
        params.push_back(std::make_unique<juce::AudioParameterInt>( id(ParamIDs::Base::splitLine3NoteMax), "Line 3 Note Max", 0, 127, 127));
        params.push_back(std::make_unique<juce::AudioParameterInt>( id(ParamIDs::Base::splitLine3Channel), "Line 3 Channel", 1, 16, 3));

        params.push_back(std::make_unique<juce::AudioParameterBool>(id(ParamIDs::Base::splitLineActive04), "Line 4 Active", false));
        params.push_back(std::make_unique<juce::AudioParameterInt>( id(ParamIDs::Base::splitLine4VoiceLimit), "Line 4 Voice Limit", 1, 16, 16));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(id(ParamIDs::Base::splitLine4Priority), "Line 4 Priority", juce::StringArray { "Last", "Lowest", "Highest" }, 0));
        params.push_back(std::make_unique<juce::AudioParameterInt>( id(ParamIDs::Base::splitLine4NoteMin), "Line 4 Note Min", 0, 127, 0));
        params.push_back(std::make_unique<juce::AudioParameterInt>( id(ParamIDs::Base::splitLine4NoteMax), "Line 4 Note Max", 0, 127, 127));
        params.push_back(std::make_unique<juce::AudioParameterInt>( id(ParamIDs::Base::splitLine4Channel), "Line 4 Channel", 1, 16, 4));

        params.push_back(std::make_unique<juce::AudioParameterBool>(id(ParamIDs::Base::splitLineActive05), "Line 5 Active", false));
        params.push_back(std::make_unique<juce::AudioParameterInt>( id(ParamIDs::Base::splitLine5VoiceLimit), "Line 5 Voice Limit", 1, 16, 16));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(id(ParamIDs::Base::splitLine5Priority), "Line 5 Priority", juce::StringArray { "Last", "Lowest", "Highest" }, 0));
        params.push_back(std::make_unique<juce::AudioParameterInt>( id(ParamIDs::Base::splitLine5NoteMin), "Line 5 Note Min", 0, 127, 0));
        params.push_back(std::make_unique<juce::AudioParameterInt>( id(ParamIDs::Base::splitLine5NoteMax), "Line 5 Note Max", 0, 127, 127));
        params.push_back(std::make_unique<juce::AudioParameterInt>( id(ParamIDs::Base::splitLine5Channel), "Line 5 Channel", 1, 16, 5));

        return params;
    }
}

//==============================================================================
// 4) Debug dump
//==============================================================================

namespace DebugUtils
{
#if LOGS_ENABLED
    static void dumpOne(const juce::AudioProcessorValueTreeState& state, const juce::String& id)
    {
        if (auto* p = state.getParameter(id))
            DBG(id + " = " + p->getCurrentValueAsText());
    }
#endif

    void dumpParameters(const juce::AudioProcessorValueTreeState& state)
    {
      #if LOGS_ENABLED
        DBG("=========================================");
        DBG("PLUGIN INIT - PARAMETERS SNAPSHOT");
        DBG("=========================================");

       #if LOG_INPUT_MONITOR
        DBG("--- Input Monitor ---");
        dumpOne(state, ParamIDs::Monitor::inputMonitorEnable);
        dumpOne(state, ParamIDs::Monitor::inputMonitorFilterNote);
        dumpOne(state, ParamIDs::Monitor::inputMonitorFilterControl);
        dumpOne(state, ParamIDs::Monitor::inputMonitorFilterClock);
        dumpOne(state, ParamIDs::Monitor::inputMonitorFilterEvent);
       #endif

       #if LOG_OUTPUT_MONITOR
        DBG("--- Output Monitor ---");
        dumpOne(state, ParamIDs::Monitor::outputMonitorEnable);
        dumpOne(state, ParamIDs::Monitor::outputMonitorFilterNote);
        dumpOne(state, ParamIDs::Monitor::outputMonitorFilterControl);
        dumpOne(state, ParamIDs::Monitor::outputMonitorFilterClock);
        dumpOne(state, ParamIDs::Monitor::outputMonitorFilterEvent);
       #endif

       #if LOG_HARMONIZER
        DBG("--- Harmonizer Global ---");
        dumpOne(state, ParamIDs::Global::harmGlobalKey);
        dumpOne(state, ParamIDs::Global::harmGlobalScale);
        dumpOne(state, ParamIDs::Global::whiteInputModeToggle);
        dumpOne(state, ParamIDs::Global::blackInputModeToggle);
       #endif

        for (int i = 0; i < Lanes::kNumLanes; ++i)
        {
            const auto lane = Lanes::fromIndex(i);
            const juce::String laneTag = juce::String("Lane ") + Lanes::laneSuffix(lane);

           #if LOG_INPUTFILTER
            DBG("--- Input Filter " + laneTag + " ---");
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::inputFilterEnable, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::inputFilterConsume, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::inputFilterDirect, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::inputFilterHold, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::inputFilterBlackNotes, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::inputMuteToggle, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::channelFilter, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::whiteKeyModeToggle, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::noteFilterToggle, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::noteMin, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::noteMax, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::velocityFilterToggle, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::velocityMin, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::velocityMax, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::stepFilterToggle, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::stepFilterNumerator, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::stepFilterDenominator, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::voiceLimitToggle, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::voiceLimit, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::priority, lane));
           #endif

           #if LOG_HARMONIZER
            DBG("--- Harmonizer " + laneTag + " ---");
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::harmonizerEnable, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::harmVoice2, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::harmVoice3, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::harmVoice4, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::harmVoice5, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::harmVoice2VelMod, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::harmVoice3VelMod, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::harmVoice4VelMod, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::harmVoice5VelMod, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::harmKey, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::harmScale, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::harmOctavePlusRandom, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::harmOctaveMinusRandom, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::harmPitchCorrector, lane));
           #endif

           #if LOG_ARPEGGIATOR
            DBG("--- Arpeggiator " + laneTag + " ---");
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::arpeggiatorEnable, lane));
            // Keep dump small: morph + link only by default.
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::arpRateMorph, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::arpDirectionMorph, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::arpPatternMorph, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::arpRangeMorph, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::arpVelocityMorph, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::arpGrooveMorph, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::arpGateMorph, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::arpAccentMorph, lane));

            dumpOne(state, ParamIDs::lane(ParamIDs::Base::arpRateLink, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::arpDirectionLink, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::arpPatternLink, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::arpRangeLink, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::arpVelocityLink, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::arpGrooveLink, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::arpGateLink, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::arpAccentLink, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::arpDirectionUnlinkRate, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::arpPatternUnlinkRate, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::arpRangeUnlinkRate, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::arpVelocityUnlinkRate, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::arpGrooveUnlinkRate, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::arpGateUnlinkRate, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::arpAccentUnlinkRate, lane));
           #endif

           #if LOG_SPLITTER
            DBG("--- Splitter " + laneTag + " ---");
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::splitterEnable, lane));
            dumpOne(state, ParamIDs::lane(ParamIDs::Base::splitterMode, lane));
           #endif
        }

        DBG("=========================================");
      #endif
    }
}
