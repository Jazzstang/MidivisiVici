/**
 * @file MorphParameterRT.h
 * @brief Atomic mirror used by the audio thread for morph interpolation.
 *
 * Threading:
 * - RT-safe by design (atomics only).
 * - No allocations, no locks.
 */

#pragma once
#include <atomic>
#include <JuceHeader.h>

/**
 * @brief RT-side atomics for one morphable parameter.
 */
struct MorphParameterRT
{
    std::atomic<float> x { 0.0f };
    std::atomic<float> y { 0.0f };
    std::atomic<float> current { 0.0f };
    std::atomic<bool>  morphable { true };

    /**
     * @brief Return interpolated value for a MIDI-style Z position (0..127).
     *
     * Thread/RT:
     * - RT-safe: atomics only.
     */
    inline float getValueForZ(int zValue) const noexcept
    {
        const float xVal = x.load(std::memory_order_relaxed);
        const float yVal = y.load(std::memory_order_relaxed);
        const bool morph = morphable.load(std::memory_order_relaxed);

        if (morph)
            return juce::jmap((float) zValue, 0.0f, 127.0f, xVal, yVal);
        else
            return (zValue < 64) ? xVal : yVal;
    }
};
