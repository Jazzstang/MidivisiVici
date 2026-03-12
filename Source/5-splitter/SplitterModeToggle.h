/**
 * @file SplitterModeToggle.h
 * @brief Compact UI toggle switching Splitter mode (Round Robin / Range).
 *
 * Threading:
 * - UI thread only.
 * - Not RT-safe.
 */
#pragma once
#include <JuceHeader.h>

/** @brief Two-state mode toggle used in Splitter header. */
class SplitterModeToggle : public juce::Component,
                           public juce::AudioProcessorParameter::Listener
{
public:
    /** @brief JUCE parameter listener hook. */
    void parameterValueChanged(int parameterIndex, float newValue) override;

    /** @brief Unused listener hook required by interface. */
    void parameterGestureChanged(int /*parameterIndex*/, bool /*gestureIsStarting*/) override {}

    /** @brief Splitter routing modes. */
    enum Mode { RoundRobin, RangeSplit };

    SplitterModeToggle();

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent&) override;
    void setMode(Mode mode);
    
    Mode getMode() const { return currentMode; }
    std::function<void(Mode)> onModeChange;

private:
    Mode currentMode = RoundRobin;
};
