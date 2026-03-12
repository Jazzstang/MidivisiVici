/**
 * @file MorphParameterManager.h
 * @brief Synchronizes morph parameter snapshots between APVTS and RT mirrors.
 *
 * Threading:
 * - Non-RT methods run on message thread.
 * - RT consumers read atomics through `MorphParameterRT`.
 */

#pragma once
#include "MorphParameterRT.h"
#include <unordered_map>
#include "../DebugConfig.h"
/**
 * @brief Owns all morph parameter mirrors and pushes interpolated RT values.
 */
class MorphParameterManager
{
public:
    /** @brief Construct manager bound to APVTS. */
    MorphParameterManager(juce::AudioProcessorValueTreeState& vts)
        : parameters(vts) {}

    /**
     * @brief Register one morphable parameter family.
     * @param baseID Base parameter ID (without `_X/_Y/_Mode` suffixes).
     */
    void registerParameter(const juce::String& baseID)
    {
        morphParams.try_emplace(baseID);
        paramIDs[baseID + "_X"]    = baseID;
        paramIDs[baseID + "_Y"]    = baseID;
        paramIDs[baseID + "_Mode"] = baseID;
    }

    /**
     * @brief Pull X/Y/morphable values from APVTS into RT atomics.
     *
     * Thread/RT:
     * - Message thread / non-RT only.
     */
    void syncFromValueTree()
    {
        for (auto& [baseID, morphParam] : morphParams)
        {
            if (auto* pX = parameters.getRawParameterValue(baseID + "_X"))
                morphParam.x.store(pX->load(), std::memory_order_relaxed);

            if (auto* pY = parameters.getRawParameterValue(baseID + "_Y"))
                morphParam.y.store(pY->load(), std::memory_order_relaxed);

            auto* pMode = parameters.getRawParameterValue(baseID + "_Mode");
            if (!pMode) pMode = parameters.getRawParameterValue(baseID + "_Morph");

            if (pMode)
                morphParam.morphable.store(pMode->load() > 0.5f, std::memory_order_relaxed);
        }

       #if LOGS_ENABLED
        DBG_LOG("SYNC", "MORPHMANAGER", "SYNC_VTS", "#010#",
                "Synced morph parameters from ValueTreeState.");
       #endif
    }

    /**
     * @brief Return RT mirror for one base parameter ID.
     * @return Pointer to RT mirror, or nullptr if ID is unknown.
     */
    const MorphParameterRT* getRT(const juce::String& baseID) const noexcept
    {
        auto it = morphParams.find(baseID);
        return (it != morphParams.end()) ? &it->second : nullptr;
    }

    /**
     * @brief Rebase X/Y mirrors from current parameter values after recall.
     *
     * Thread/RT:
     * - Message thread / non-RT only.
     */
    void resyncAllFromParameters() noexcept
    {
       #if LOGS_ENABLED
        DBG_LOG("SYNC", "MORPHMANAGER", "RESYNC_ALL", "#000#",
                "Resynchronizing all morph parameters from AVTS state...");
       #endif

        for (auto& [paramID, morphParam] : morphParams)
        {
            if (auto* p = parameters.getParameter(paramID))
            {
                const float v = p->getValue();
                morphParam.x.store(v, std::memory_order_relaxed);
                morphParam.y.store(v, std::memory_order_relaxed);
            }
        }

       #if LOGS_ENABLED
        DBG_LOG("SYNC", "MORPHMANAGER", "RESYNC_ALL", "#001#",
                "All morph parameters resynced successfully.");
       #endif
    }
    
    /**
     * @brief Compute current morphed value for each registered parameter.
     * @param zNorm Morph position in normalized range [0..1].
     *
     * Thread/RT:
     * - RT-safe: atomics only, no locks, no allocations.
     */
    void applyMorph(float zNorm) noexcept
    {
        const float z = juce::jlimit(0.0f, 1.0f, zNorm);
        for (auto& [baseID, morphParam] : morphParams)
        {
            const float xVal = morphParam.x.load(std::memory_order_relaxed);
            const float yVal = morphParam.y.load(std::memory_order_relaxed);
            const bool  isMorphable = morphParam.morphable.load(std::memory_order_relaxed);
            const float morphedValue = isMorphable
                ? juce::jmap(z, 0.0f, 1.0f, xVal, yVal)
                : xVal;
            morphParam.current.store(morphedValue, std::memory_order_relaxed);
        }

       #if LOGS_ENABLED
        DBG_LOG("RT", "MORPHMANAGER", "APPLY_MORPH", "#020#",
                "Applied morph at Z=" + juce::String(zNorm, 2));
       #endif
    }

private:
    /** @brief Non-RT APVTS reference used for synchronization. */
    juce::AudioProcessorValueTreeState& parameters;
    /** @brief RT mirrors keyed by base parameter ID. */
    std::unordered_map<juce::String, MorphParameterRT> morphParams;
    /** @brief Mapping cache from suffixed IDs to base IDs (non-RT helper). */
    std::unordered_map<juce::String, juce::String> paramIDs;
};
