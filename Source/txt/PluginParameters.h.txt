#pragma once

namespace ParamIDs
{
    // === Input Monitor ===
    constexpr const char* inputMonitorEnable         = "inputMonitorEnable";
    constexpr const char* inputMonitorFilterNote     = "inputMonitorFilterNote";
    constexpr const char* inputMonitorFilterControl  = "inputMonitorFilterControl";
    constexpr const char* inputMonitorFilterClock    = "inputMonitorFilterClock";
    constexpr const char* inputMonitorFilterEvent    = "inputMonitorFilterEvent";

    // === Input Filter ===
    constexpr const char* inputEnable                = "inputEnable";
    constexpr const char* inputMute                  = "inputMute";
    constexpr const char* learnModeActive            = "learnModeActive";
    constexpr const char* learnedCC                  = "learnedCC";
    constexpr const char* learnedChannel             = "learnedChannel";
    constexpr const char* noteMin                    = "noteMin";
    constexpr const char* noteMax                    = "noteMax";
    constexpr const char* velocityMin                = "velocityMin";
    constexpr const char* velocityMax                = "velocityMax";
    constexpr const char* stepFilterNumerator        = "stepFilterNumerator";
    constexpr const char* stepFilterDenominator      = "stepFilterDenominator";
    constexpr const char* voiceLimit                 = "voiceLimit";
    constexpr const char* priority                   = "priority";
    constexpr const char* channel                    = "channel";

    // === Transform ===
    constexpr const char* transformEnable            = "transformEnable";
    constexpr const char* transformMode              = "transformMode";
    constexpr const char* pitchShift                 = "pitchShift";

    // === Divisi ===
    constexpr const char* divisiEnable               = "divisiEnable";
    constexpr const char* divisiMode                 = "divisiMode";

    // === Output Monitor ===
    constexpr const char* outputMonitorEnable        = "outputMonitorEnable";
    constexpr const char* outputMonitorFilterNote    = "outputMonitorFilterNote";
    constexpr const char* outputMonitorFilterControl = "outputMonitorFilterControl";
    constexpr const char* outputMonitorFilterClock   = "outputMonitorFilterClock";
    constexpr const char* outputMonitorFilterEvent   = "outputMonitorFilterEvent";
}
