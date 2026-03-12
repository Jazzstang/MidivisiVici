/**
 * @file InputFilter.h
 * @brief Lane-aware Input Filter UI module.
 *
 * Threading:
 * - UI owns widgets.
 * - `parameterChanged` may come from audio thread and only triggers async update.
 * - Not RT-safe.
 */

#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <memory>

#include "../0-component/TwoLineToggleButton.h"
#include "../0-component/LabeledSlider.h"
#include "../0-component/FlatComboBox.h"
#include "../0-component/LeftClickToggleButton.h"
#include "../0-component/ShadowComponent.h"
#include "../PluginParameters.h"

/**
 * @brief Input Filter user interface for one lane (A..P).
 *
 * Pattern:
 * - Pattern: Observer + Async boundary
 * - Problem solved: reflect APVTS automation safely in UI without touching UI
 *   from non-message threads.
 * - This class is UI-only: it does not emit MIDI directly, so stuck-note
 *   prevention depends on keeping parameter writes coherent for the processor.
 * - Participants: APVTS listener, `AsyncUpdater`, lane-local controls.
 * - Flow: APVTS change -> async trigger -> UI refresh on message thread.
 * - Pitfalls: keep lane ID mapping strictly consistent with parameter layout.
 */
class InputFilter : public juce::Component,
                    private juce::AudioProcessorValueTreeState::Listener,
                    private juce::AsyncUpdater
{
public:
    using Lane = Lanes::Lane;

    //==========================================================================
    // Ctor / dtor
    //==========================================================================
    explicit InputFilter(juce::AudioProcessorValueTreeState& state, Lane laneIn);
    ~InputFilter() override;

    //==========================================================================
    // JUCE
    //==========================================================================
    void resized() override;
    void paint(juce::Graphics& g) override;

    /** @brief Force APVTS-to-UI refresh on demand (message thread). */
    void forceRefreshFromParameters();

private:
    //==========================================================================
    // APVTS Listener (NOT message thread). We only trigger an async refresh here.
    //==========================================================================
    void parameterChanged(const juce::String& parameterID, float newValue) override;

    //==========================================================================
    // AsyncUpdater (message thread): apply APVTS -> UI without notifications.
    //==========================================================================
    void handleAsyncUpdate() override;

    //==========================================================================
    // Param registration
    //==========================================================================
    void registerParameterListeners();
    void unregisterParameterListeners();

    //==========================================================================
    // UI refresh: read parameters and update controls
    //==========================================================================
    void refreshUIFromParameters();

    // Robust + centralized UI gating:
    // - applyGlobalUiBypass() handles global bypass visuals + interaction.
    // - updateFilterEnabledState() handles per-section gating.
    void applyGlobalUiBypass(bool bypassed);
    void updateFilterEnabledState();
    void setBoolParameterFromUi(const juce::String& paramId, bool enabled);
    void setIntParameterFromUi(const juce::String& paramId, int value);
    void setChoiceParameterFromUi(const juce::String& paramId, int index);

    // Prevent feedback loops when we update UI from parameter state.
    std::atomic<bool> applyingFromState { false };

    //==========================================================================
    // Utility: add a component wrapped in a ShadowComponent.
    //==========================================================================
    template<typename Comp>
    void addWithShadow(std::unique_ptr<ShadowComponent>& shadowPtr,
                       Comp& comp,
                       const juce::DropShadow& shadow);

    //==========================================================================
    // Lane helpers
    //==========================================================================
    juce::String laneParamID(const char* baseID) const;

    //==========================================================================
    // APVTS
    //==========================================================================
    juce::AudioProcessorValueTreeState& parameters;
    const Lane lane;

    // Cached lane-specific parameter IDs (prebuilt to avoid repeated allocations).
    // NOTE: juce::String allocations happen in the UI thread only (constructor).
    juce::String idInputFilterEnable;

    // Routing toggles (lane-based)
    juce::String idInputFilterConsume;
    juce::String idInputFilterDirect;
    juce::String idInputFilterHold;
    juce::String idInputFilterBlackNotes;
    juce::String idGlobalBlackInputMode;

    juce::String idNoteFilterToggle;
    juce::String idNoteMin;
    juce::String idNoteMax;

    juce::String idVelocityFilterToggle;
    juce::String idVelocityMin;
    juce::String idVelocityMax;

    juce::String idStepFilterToggle;
    juce::String idStepFilterNumerator;
    juce::String idStepFilterDenominator;

    juce::String idVoiceLimitToggle;
    juce::String idVoiceLimit;
    juce::String idPriority;

    //==========================================================================
    // Header: global enable for Input Filter.
    //==========================================================================
    LeftClickToggleButton inputFilterTitleButton { "Input Filter" };
    std::unique_ptr<ShadowComponent> inputFilterTitleButtonShadow;

    //==========================================================================
    // Toggles + sliders: Note / Velocity / Step / Voice Limit
    //==========================================================================
    TwoLineToggleButton noteFilterToggle     { "Note",     "Filter" };
    TwoLineToggleButton velocityFilterToggle { "Velocity", "Filter" };
    TwoLineToggleButton stepFilterToggle     { "Step",     "Filter" };
    TwoLineToggleButton voiceLimitToggle     { "Voice",    "Limit" };

    std::unique_ptr<ShadowComponent> noteFilterToggleShadow;
    std::unique_ptr<ShadowComponent> velocityFilterToggleShadow;
    std::unique_ptr<ShadowComponent> stepFilterToggleShadow;
    std::unique_ptr<ShadowComponent> voiceLimitToggleShadow;

    LabeledSlider noteSlider;
    LabeledSlider velocitySlider;
    LabeledSlider stepSlider;
    LabeledSlider voiceSlider;

    //==========================================================================
    // Priority (Last, Lowest, Highest) - dedicated compact row
    //==========================================================================
    juce::Label priorityLabel;
    FlatComboBox priorityBox;
    std::unique_ptr<ShadowComponent> priorityBoxShadow;

    //==========================================================================
    // Routing row (Direct + Black Notes + Hold + Consume)
    //
    // Interaction:
    // - Only meaningful when global enable is ON.
    // - Direct/Hold remain available while global enable is ON.
    // - Black Notes is editable only when global B mode is ON.
    // - Consume is forcibly locked OFF while Step Filter is ON:
    //   * UI shows consume unchecked and disabled.
    //   * user click cannot re-enable consume until Step Filter is OFF.
    //==========================================================================
    juce::Label directLabel;
    juce::ToggleButton directButton;
    juce::Label blackNotesLabel;
    juce::ToggleButton blackNotesButton;
    juce::Label holdLabel;
    juce::ToggleButton holdButton;
    juce::Label consumeLabel;
    juce::ToggleButton consumeButton;

    //==========================================================================
    // Last-known UI values (used for optional log gating)
    //==========================================================================
    int lastNoteMin         = -1;
    int lastNoteMax         = -1;
    int lastVelocityMin     = -1;
    int lastVelocityMax     = -1;
    int lastStepNumerator   = -1;
    int lastStepDenominator = -1;
    int lastVoiceLimit      = -1;
    int lastPriorityIndex   = 0;

    bool lastNoteFilterState     = false;
    bool lastVelocityFilterState = false;
    bool lastStepFilterState     = false;
    bool lastVoiceLimitState     = false;
    bool lastGlobalEnableState   = false;

    // Routing cached UI state
    bool lastDirectState         = false;
    bool lastBlackNotesState     = false;
    bool lastHoldState           = false;
    bool lastConsumeState        = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(InputFilter)
};
