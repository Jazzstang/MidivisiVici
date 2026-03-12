/**
 * @file PluginParameters.h
 * @brief IDs canoniques, helpers lane et declaration des builders de layout.
 *
 * Role:
 * - Centraliser la source de verite des IDs parametres.
 * - Fournir des helpers robustes pour suffixes lane et steps.
 * - Decrire les interfaces de construction APVTS.
 *
 * Threading:
 * - Definitions/builders: init-time ou message thread.
 * - Runtime: preferer des pointeurs caches, eviter string building en RT.
 */

#pragma once

#include <JuceHeader.h>

#include <memory>
#include <vector>

//==============================================================================
// 1) Systeme de lanes (A..P)
//==============================================================================

namespace Lanes
{
    /** @brief Identifiants de lane utilises par UI et processing. */
    enum class Lane : int
    {
        A = 0,  B,  C,  D,
        E,      F,  G,  H,
        I,      J,  K,  L,
        M,      N,  O,  P
    };

    static constexpr int kNumLanes = 16;

    inline const char* laneSuffix(Lane lane) noexcept
    {
        static constexpr const char* kLaneNames[kNumLanes] =
        {
            "A", "B", "C", "D",
            "E", "F", "G", "H",
            "I", "J", "K", "L",
            "M", "N", "O", "P"
        };

        const auto idx = juce::jlimit(0, kNumLanes - 1, (int) lane);
        return kLaneNames[idx];
    }

    inline Lane fromIndex(int i) noexcept
    {
        const auto idx = juce::jlimit(0, kNumLanes - 1, i);
        return static_cast<Lane>(idx);
    }
}

//==============================================================================
// 2) IDs de parametres
//------------------------------------------------------------------------------
// IMPORTANT:
//   - ParamIDs::Base: noms canoniques sans suffixe lane.
//   - ParamIDs::Monitor: IDs single-instance sans suffixe lane.
//   - ParamIDs::* aliases legacy: compatibilite code existant.
//==============================================================================

namespace ParamIDs
{
    //--------------------------------------------------------------------------
    // 2.1) Monitor (single instance)
    //--------------------------------------------------------------------------
    namespace Monitor
    {
        inline constexpr const char* inputMonitorEnable        = "inputMonitorEnable";
        inline constexpr const char* inputMonitorFilterNote    = "inputMonitorFilterNote";
        inline constexpr const char* inputMonitorFilterControl = "inputMonitorFilterControl";
        inline constexpr const char* inputMonitorFilterClock   = "inputMonitorFilterClock";
        inline constexpr const char* inputMonitorFilterEvent   = "inputMonitorFilterEvent";

        inline constexpr const char* outputMonitorEnable        = "outputMonitorEnable";
        inline constexpr const char* outputMonitorFilterNote    = "outputMonitorFilterNote";
        inline constexpr const char* outputMonitorFilterControl = "outputMonitorFilterControl";
        inline constexpr const char* outputMonitorFilterClock   = "outputMonitorFilterClock";
        inline constexpr const char* outputMonitorFilterEvent   = "outputMonitorFilterEvent";
    }

    //--------------------------------------------------------------------------
    // 2.1.b) Global controls (single instance, no lane suffix)
    //--------------------------------------------------------------------------
    namespace Global
    {
        // Global harmonizer tonality/mode, applied in every harmonizer lane.
        inline constexpr const char* harmGlobalKey              = "harmGlobalKey";
        inline constexpr const char* harmGlobalScale            = "harmGlobalScale";
        // Global keyboard remap modes (MainMenu W/B).
        inline constexpr const char* whiteInputModeToggle       = "whiteInputModeToggle";
        inline constexpr const char* blackInputModeToggle       = "blackInputModeToggle";
    }

    //--------------------------------------------------------------------------
    // 2.2) Base IDs (lane-based modules)
    //--------------------------------------------------------------------------
    namespace Base
    {
        // InputFilter
        inline constexpr const char* inputFilterEnable       = "inputFilterEnable";

        // NEW: consume routing (lane-based)
        // Meaning:
        //   - If ON for lane A: lane B input becomes remainder(A).
        //   - If OFF for lane A: lane B input follows lane A source (serial/parallel topology).
        // This is currently implemented at processor routing level.
        inline constexpr const char* inputFilterConsume      = "inputFilterConsume";
        // Direct source override (lane-based):
        //   - If ON for lane X: lane X source is rawPlayableScratch for this block.
        //   - If OFF for lane X: lane X follows normal consume topology.
        inline constexpr const char* inputFilterDirect       = "inputFilterDirect";
        // Hold mode (lane-based):
        //   - If ON: accepted notes remain held until replaced or transport STOP.
        //   - START retriggers held notes.
        inline constexpr const char* inputFilterHold         = "inputFilterHold";
        // Black transformed-note intake (lane-based):
        //   - UI/behavior is only active when global B mode is ON.
        //   - If ON: lane receives transformed black-note events.
        //   - If OFF: transformed black-note NOTE ON / poly aftertouch are blocked
        //     for this lane (NOTE OFF still propagated for safety).
        inline constexpr const char* inputFilterBlackNotes   = "inputFilterBlackNotes";

        inline constexpr const char* inputMuteToggle         = "inputMuteToggle";
        inline constexpr const char* channelFilter           = "channelFilter";
        inline constexpr const char* whiteKeyModeToggle      = "whiteKeyModeToggle";

        inline constexpr const char* noteFilterToggle        = "noteFilterToggle";
        inline constexpr const char* noteMin                 = "noteMin";
        inline constexpr const char* noteMax                 = "noteMax";

        inline constexpr const char* velocityFilterToggle    = "velocityFilterToggle";
        inline constexpr const char* velocityMin             = "velocityMin";
        inline constexpr const char* velocityMax             = "velocityMax";

        inline constexpr const char* stepFilterToggle        = "stepFilterToggle";
        inline constexpr const char* stepFilterNumerator     = "stepFilterNumerator";
        inline constexpr const char* stepFilterDenominator   = "stepFilterDenominator";

        inline constexpr const char* voiceLimitToggle        = "voiceLimitToggle";
        inline constexpr const char* voiceLimit              = "voiceLimit";
        inline constexpr const char* priority                = "priority";

        // Harmonizer
        inline constexpr const char* harmonizerEnable        = "harmonizerEnable";
        inline constexpr const char* harmVoice2              = "harmVoice2";
        inline constexpr const char* harmVoice3              = "harmVoice3";
        inline constexpr const char* harmVoice4              = "harmVoice4";
        inline constexpr const char* harmVoice5              = "harmVoice5";
        inline constexpr const char* harmVoice2VelMod        = "harmVoice2VelMod";
        inline constexpr const char* harmVoice3VelMod        = "harmVoice3VelMod";
        inline constexpr const char* harmVoice4VelMod        = "harmVoice4VelMod";
        inline constexpr const char* harmVoice5VelMod        = "harmVoice5VelMod";
        inline constexpr const char* harmKey                 = "harmKey";
        inline constexpr const char* harmScale               = "harmScale";

        // IMPORTANT:
        // These are numeric ranges in current design (INT 0..8).
        inline constexpr const char* harmOctavePlusRandom     = "harmOctavePlusRandom";
        inline constexpr const char* harmOctaveMinusRandom    = "harmOctaveMinusRandom";

        inline constexpr const char* harmPitchCorrector       = "harmPitchCorrector";

        // Arpeggiator (morph + link)
        inline constexpr const char* arpeggiatorEnable        = "arpeggiatorEnable";
        inline constexpr const char* arpMode                  = "arpMode";

        inline constexpr const char* arpRateMorph             = "arpRateMorph";
        inline constexpr const char* arpDirectionMorph        = "arpDirectionMorph";
        inline constexpr const char* arpPatternMorph          = "arpPatternMorph";
        inline constexpr const char* arpRangeMorph            = "arpRangeMorph";
        inline constexpr const char* arpVelocityMorph         = "arpVelocityMorph";
        inline constexpr const char* arpGrooveMorph           = "arpGrooveMorph";
        inline constexpr const char* arpGateMorph             = "arpGateMorph";
        inline constexpr const char* arpAccentMorph           = "arpAccentMorph";

        inline constexpr const char* arpRateLink              = "arpRateLink";
        inline constexpr const char* arpDirectionLink         = "arpDirectionLink";
        inline constexpr const char* arpPatternLink           = "arpPatternLink";
        inline constexpr const char* arpRangeLink             = "arpRangeLink";
        inline constexpr const char* arpVelocityLink          = "arpVelocityLink";
        inline constexpr const char* arpGrooveLink            = "arpGrooveLink";
        inline constexpr const char* arpGateLink              = "arpGateLink";
        inline constexpr const char* arpAccentLink            = "arpAccentLink";
        inline constexpr const char* arpPaceLink              = "arpPaceLink";
        inline constexpr const char* arpDirectionUnlinkRate   = "arpDirectionUnlinkRate";
        inline constexpr const char* arpPatternUnlinkRate     = "arpPatternUnlinkRate";
        inline constexpr const char* arpRangeUnlinkRate       = "arpRangeUnlinkRate";
        inline constexpr const char* arpVelocityUnlinkRate    = "arpVelocityUnlinkRate";
        inline constexpr const char* arpGrooveUnlinkRate      = "arpGrooveUnlinkRate";
        inline constexpr const char* arpGateUnlinkRate        = "arpGateUnlinkRate";
        inline constexpr const char* arpAccentUnlinkRate      = "arpAccentUnlinkRate";

        // Arpeggiator step prefixes (no step number, no lane suffix)
        inline constexpr const char* arpRateSeqPrefix         = "arpRateSeq";
        inline constexpr const char* arpDirectionSeqPrefix    = "arpDirectionSeq";
        inline constexpr const char* arpPatternSeqPrefix      = "arpPatternSeq";
        inline constexpr const char* arpRangeSeqPrefix        = "arpRangeSeq";
        inline constexpr const char* arpVelocitySeqPrefix     = "arpVelocitySeq";
        inline constexpr const char* arpGrooveSeqPrefix       = "arpGrooveSeq";
        inline constexpr const char* arpGateSeqPrefix         = "arpGateSeq";
        inline constexpr const char* arpAccentSeqPrefix       = "arpAccentSeq";
        inline constexpr const char* arpPaceSeqPrefix         = "arpPaceSeq";
        // Drum-machine-specific step pages (independent from Strum/Jump/Octave).
        inline constexpr const char* drumGraceSeqPrefix       = "drumGraceSeq";
        inline constexpr const char* drumVeloEnvSeqPrefix     = "drumVeloEnvSeq";
        inline constexpr const char* drumTimEnvSeqPrefix      = "drumTimEnvSeq";

        // VoiceSplitter
        inline constexpr const char* splitterEnable           = "splitterEnable";
        inline constexpr const char* splitterMode             = "splitterMode";

        inline constexpr const char* splitLineActive01        = "splitLineActive01";
        inline constexpr const char* splitLine1Channel        = "splitLine1Channel";

        inline constexpr const char* splitLineActive02        = "splitLineActive02";
        inline constexpr const char* splitLine2VoiceLimit     = "splitLine2VoiceLimit";
        inline constexpr const char* splitLine2Priority       = "splitLine2Priority";
        inline constexpr const char* splitLine2NoteMin        = "splitLine2NoteMin";
        inline constexpr const char* splitLine2NoteMax        = "splitLine2NoteMax";
        inline constexpr const char* splitLine2Channel        = "splitLine2Channel";

        inline constexpr const char* splitLineActive03        = "splitLineActive03";
        inline constexpr const char* splitLine3VoiceLimit     = "splitLine3VoiceLimit";
        inline constexpr const char* splitLine3Priority       = "splitLine3Priority";
        inline constexpr const char* splitLine3NoteMin        = "splitLine3NoteMin";
        inline constexpr const char* splitLine3NoteMax        = "splitLine3NoteMax";
        inline constexpr const char* splitLine3Channel        = "splitLine3Channel";

        inline constexpr const char* splitLineActive04        = "splitLineActive04";
        inline constexpr const char* splitLine4VoiceLimit     = "splitLine4VoiceLimit";
        inline constexpr const char* splitLine4Priority       = "splitLine4Priority";
        inline constexpr const char* splitLine4NoteMin        = "splitLine4NoteMin";
        inline constexpr const char* splitLine4NoteMax        = "splitLine4NoteMax";
        inline constexpr const char* splitLine4Channel        = "splitLine4Channel";

        inline constexpr const char* splitLineActive05        = "splitLineActive05";
        inline constexpr const char* splitLine5VoiceLimit     = "splitLine5VoiceLimit";
        inline constexpr const char* splitLine5Priority       = "splitLine5Priority";
        inline constexpr const char* splitLine5NoteMin        = "splitLine5NoteMin";
        inline constexpr const char* splitLine5NoteMax        = "splitLine5NoteMax";
        inline constexpr const char* splitLine5Channel        = "splitLine5Channel";
    }

    //--------------------------------------------------------------------------
    // 2.3) Safe ID builders (lane suffix and step numbers)
    //--------------------------------------------------------------------------
    inline juce::String lane(const juce::String& baseId, Lanes::Lane laneIn)
    {
        // Format: "<baseId>_<laneSuffix>"
        return baseId + "_" + Lanes::laneSuffix(laneIn);
    }

    inline juce::String lane(const char* baseId, Lanes::Lane laneIn)
    {
        return lane(juce::String(baseId), laneIn);
    }

    inline juce::String stepId(const char* prefix, int step1Based)
    {
        // Format: "<prefix>01" .. "<prefix>32"
        const auto s = juce::String(step1Based).paddedLeft('0', 2);
        return juce::String(prefix) + s;
    }

    inline juce::String laneStep(const char* prefix, int step1Based, Lanes::Lane laneIn)
    {
        // Format: "<prefix>01_<laneSuffix>"
        return lane(stepId(prefix, step1Based), laneIn);
    }

    //--------------------------------------------------------------------------
    // 2.4) Legacy aliases (temporary)
    //--------------------------------------------------------------------------
    // Purpose:
    //   Older modules still use ParamIDs::<id> directly.
    //   These aliases map to the new canonical layout without changing every file.
    //
    // Rule:
    //   New code should use ParamIDs::Base::* or ParamIDs::Monitor::* explicitly.
    //--------------------------------------------------------------------------
    inline constexpr const char* inputMonitorEnable        = Monitor::inputMonitorEnable;
    inline constexpr const char* inputMonitorFilterNote    = Monitor::inputMonitorFilterNote;
    inline constexpr const char* inputMonitorFilterControl = Monitor::inputMonitorFilterControl;
    inline constexpr const char* inputMonitorFilterClock   = Monitor::inputMonitorFilterClock;
    inline constexpr const char* inputMonitorFilterEvent   = Monitor::inputMonitorFilterEvent;

    inline constexpr const char* outputMonitorEnable        = Monitor::outputMonitorEnable;
    inline constexpr const char* outputMonitorFilterNote    = Monitor::outputMonitorFilterNote;
    inline constexpr const char* outputMonitorFilterControl = Monitor::outputMonitorFilterControl;
    inline constexpr const char* outputMonitorFilterClock   = Monitor::outputMonitorFilterClock;
    inline constexpr const char* outputMonitorFilterEvent   = Monitor::outputMonitorFilterEvent;

    inline constexpr const char* harmGlobalKey              = Global::harmGlobalKey;
    inline constexpr const char* harmGlobalScale            = Global::harmGlobalScale;
    inline constexpr const char* whiteInputModeToggle       = Global::whiteInputModeToggle;
    inline constexpr const char* blackInputModeToggle       = Global::blackInputModeToggle;

    inline constexpr const char* inputFilterEnable         = Base::inputFilterEnable;
    inline constexpr const char* inputFilterConsume        = Base::inputFilterConsume; // NEW legacy alias
    inline constexpr const char* inputFilterDirect         = Base::inputFilterDirect;  // NEW legacy alias
    inline constexpr const char* inputFilterHold           = Base::inputFilterHold;    // NEW legacy alias
    inline constexpr const char* inputFilterBlackNotes     = Base::inputFilterBlackNotes;

    inline constexpr const char* inputMuteToggle           = Base::inputMuteToggle;
    inline constexpr const char* channelFilter             = Base::channelFilter;
    inline constexpr const char* whiteKeyModeToggle        = Base::whiteKeyModeToggle;

    inline constexpr const char* noteFilterToggle          = Base::noteFilterToggle;
    inline constexpr const char* noteMin                   = Base::noteMin;
    inline constexpr const char* noteMax                   = Base::noteMax;

    inline constexpr const char* velocityFilterToggle      = Base::velocityFilterToggle;
    inline constexpr const char* velocityMin               = Base::velocityMin;
    inline constexpr const char* velocityMax               = Base::velocityMax;

    inline constexpr const char* stepFilterToggle          = Base::stepFilterToggle;
    inline constexpr const char* stepFilterNumerator       = Base::stepFilterNumerator;
    inline constexpr const char* stepFilterDenominator     = Base::stepFilterDenominator;

    inline constexpr const char* voiceLimitToggle          = Base::voiceLimitToggle;
    inline constexpr const char* voiceLimit                = Base::voiceLimit;
    inline constexpr const char* priority                  = Base::priority;

    inline constexpr const char* harmonizerEnable          = Base::harmonizerEnable;
    inline constexpr const char* harmVoice2                = Base::harmVoice2;
    inline constexpr const char* harmVoice3                = Base::harmVoice3;
    inline constexpr const char* harmVoice4                = Base::harmVoice4;
    inline constexpr const char* harmVoice5                = Base::harmVoice5;
    inline constexpr const char* harmVoice2VelMod          = Base::harmVoice2VelMod;
    inline constexpr const char* harmVoice3VelMod          = Base::harmVoice3VelMod;
    inline constexpr const char* harmVoice4VelMod          = Base::harmVoice4VelMod;
    inline constexpr const char* harmVoice5VelMod          = Base::harmVoice5VelMod;
    inline constexpr const char* harmKey                   = Base::harmKey;
    inline constexpr const char* harmScale                 = Base::harmScale;
    inline constexpr const char* harmOctavePlusRandom       = Base::harmOctavePlusRandom;
    inline constexpr const char* harmOctaveMinusRandom      = Base::harmOctaveMinusRandom;
    inline constexpr const char* harmPitchCorrector         = Base::harmPitchCorrector;

    inline constexpr const char* arpeggiatorEnable          = Base::arpeggiatorEnable;
    inline constexpr const char* arpMode                    = Base::arpMode;
    inline constexpr const char* arpRateMorph               = Base::arpRateMorph;
    inline constexpr const char* arpDirectionMorph          = Base::arpDirectionMorph;
    inline constexpr const char* arpPatternMorph            = Base::arpPatternMorph;
    inline constexpr const char* arpRangeMorph              = Base::arpRangeMorph;
    inline constexpr const char* arpVelocityMorph           = Base::arpVelocityMorph;
    inline constexpr const char* arpGrooveMorph             = Base::arpGrooveMorph;
    inline constexpr const char* arpGateMorph               = Base::arpGateMorph;
    inline constexpr const char* arpAccentMorph             = Base::arpAccentMorph;

    inline constexpr const char* arpRateLink                = Base::arpRateLink;
    inline constexpr const char* arpDirectionLink           = Base::arpDirectionLink;
    inline constexpr const char* arpPatternLink             = Base::arpPatternLink;
    inline constexpr const char* arpRangeLink               = Base::arpRangeLink;
    inline constexpr const char* arpVelocityLink            = Base::arpVelocityLink;
    inline constexpr const char* arpGrooveLink              = Base::arpGrooveLink;
    inline constexpr const char* arpGateLink                = Base::arpGateLink;
    inline constexpr const char* arpAccentLink              = Base::arpAccentLink;
    inline constexpr const char* arpPaceLink                = Base::arpPaceLink;
    inline constexpr const char* arpDirectionUnlinkRate     = Base::arpDirectionUnlinkRate;
    inline constexpr const char* arpPatternUnlinkRate       = Base::arpPatternUnlinkRate;
    inline constexpr const char* arpRangeUnlinkRate         = Base::arpRangeUnlinkRate;
    inline constexpr const char* arpVelocityUnlinkRate      = Base::arpVelocityUnlinkRate;
    inline constexpr const char* arpGrooveUnlinkRate        = Base::arpGrooveUnlinkRate;
    inline constexpr const char* arpGateUnlinkRate          = Base::arpGateUnlinkRate;
    inline constexpr const char* arpAccentUnlinkRate        = Base::arpAccentUnlinkRate;

    inline constexpr const char* arpRateSeqPrefix           = Base::arpRateSeqPrefix;
    inline constexpr const char* arpDirectionSeqPrefix      = Base::arpDirectionSeqPrefix;
    inline constexpr const char* arpPatternSeqPrefix        = Base::arpPatternSeqPrefix;
    inline constexpr const char* arpRangeSeqPrefix          = Base::arpRangeSeqPrefix;
    inline constexpr const char* arpVelocitySeqPrefix       = Base::arpVelocitySeqPrefix;
    inline constexpr const char* arpGrooveSeqPrefix         = Base::arpGrooveSeqPrefix;
    inline constexpr const char* arpGateSeqPrefix           = Base::arpGateSeqPrefix;
    inline constexpr const char* arpAccentSeqPrefix         = Base::arpAccentSeqPrefix;
    inline constexpr const char* arpPaceSeqPrefix           = Base::arpPaceSeqPrefix;
    inline constexpr const char* drumGraceSeqPrefix         = Base::drumGraceSeqPrefix;
    inline constexpr const char* drumVeloEnvSeqPrefix       = Base::drumVeloEnvSeqPrefix;
    inline constexpr const char* drumTimEnvSeqPrefix        = Base::drumTimEnvSeqPrefix;

    inline constexpr const char* splitterEnable             = Base::splitterEnable;
    inline constexpr const char* splitterMode               = Base::splitterMode;
    inline constexpr const char* splitLineActive01          = Base::splitLineActive01;
    inline constexpr const char* splitLine1Channel          = Base::splitLine1Channel;
    inline constexpr const char* splitLineActive02          = Base::splitLineActive02;
    inline constexpr const char* splitLine2VoiceLimit       = Base::splitLine2VoiceLimit;
    inline constexpr const char* splitLine2Priority         = Base::splitLine2Priority;
    inline constexpr const char* splitLine2NoteMin          = Base::splitLine2NoteMin;
    inline constexpr const char* splitLine2NoteMax          = Base::splitLine2NoteMax;
    inline constexpr const char* splitLine2Channel          = Base::splitLine2Channel;
    inline constexpr const char* splitLineActive03          = Base::splitLineActive03;
    inline constexpr const char* splitLine3VoiceLimit       = Base::splitLine3VoiceLimit;
    inline constexpr const char* splitLine3Priority         = Base::splitLine3Priority;
    inline constexpr const char* splitLine3NoteMin          = Base::splitLine3NoteMin;
    inline constexpr const char* splitLine3NoteMax          = Base::splitLine3NoteMax;
    inline constexpr const char* splitLine3Channel          = Base::splitLine3Channel;
    inline constexpr const char* splitLineActive04          = Base::splitLineActive04;
    inline constexpr const char* splitLine4VoiceLimit       = Base::splitLine4VoiceLimit;
    inline constexpr const char* splitLine4Priority         = Base::splitLine4Priority;
    inline constexpr const char* splitLine4NoteMin          = Base::splitLine4NoteMin;
    inline constexpr const char* splitLine4NoteMax          = Base::splitLine4NoteMax;
    inline constexpr const char* splitLine4Channel          = Base::splitLine4Channel;
    inline constexpr const char* splitLineActive05          = Base::splitLineActive05;
    inline constexpr const char* splitLine5VoiceLimit       = Base::splitLine5VoiceLimit;
    inline constexpr const char* splitLine5Priority         = Base::splitLine5Priority;
    inline constexpr const char* splitLine5NoteMin          = Base::splitLine5NoteMin;
    inline constexpr const char* splitLine5NoteMax          = Base::splitLine5NoteMax;
    inline constexpr const char* splitLine5Channel          = Base::splitLine5Channel;
}

//==============================================================================
// 3) Parameter layout entry point
//==============================================================================

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

//==============================================================================
// 4) Parameter builders
//==============================================================================

namespace ParameterBuilders
{
    // Single instance
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> createGlobalHarmonizerParameters();
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> createInputMonitorParameters();
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> createOutputMonitorParameters();

    // Lane-based
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> createInputFilterParameters(Lanes::Lane lane);
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> createHarmonizerParameters(Lanes::Lane lane);
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> createArpeggiatorParameters(Lanes::Lane lane);
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> createVoiceSplitterParameters(Lanes::Lane lane);
}

//==============================================================================
// 5) Debug utilities
//==============================================================================

namespace DebugUtils
{
    void dumpParameters(const juce::AudioProcessorValueTreeState& state);
}
