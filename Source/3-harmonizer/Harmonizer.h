/**
 * @file Harmonizer.h
 * @brief Lane-aware Harmonizer UI module.
 *
 * Threading:
 * - UI widgets on message thread.
 * - APVTS callbacks may occur off message thread and are bridged through
 *   `AsyncUpdater`.
 * - Not RT-safe.
 */

#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <memory>
#include <vector>

#include "../PluginParameters.h"
#include "../0-component/ShadowComponent.h"
#include "../0-component/LeftClickToggleButton.h"
#include "../0-component/CustomRotaryWithCombo.h"

class MidivisiViciAudioProcessor;

//==============================================================================
// Harmonizer (UI)
//==============================================================================

/**
 * @brief Harmonizer controls for one lane.
 *
 * Pattern:
 * - Pattern: Observer + Async boundary
 * - Problem solved: keep UI in sync with automation/snapshot updates safely.
 * - Participants: APVTS listener, `AsyncUpdater`, lane-local controls.
 * - Flow: parameter callback -> async trigger -> pull state and repaint.
 * - Pitfalls: enforce lane ID mapping and avoid UI writes in audio-thread callbacks.
 */
class Harmonizer : public juce::Component,
                   private juce::AudioProcessorValueTreeState::Listener,
                   private juce::AsyncUpdater,
                   private juce::Timer,
                   private juce::Slider::Listener
{
public:
    //==========================================================================
    // Construction / destruction
    //==========================================================================
    Harmonizer(juce::AudioProcessorValueTreeState& state,
               MidivisiViciAudioProcessor& processorIn,
               Lanes::Lane laneIn);
    ~Harmonizer() override;

    //==========================================================================
    // JUCE overrides
    //==========================================================================
    void resized() override;
    void paint(juce::Graphics& g) override;
    void uiTimerTick();

    //==========================================================================
    // Explicit refresh (optional)
    //
    // The editor can call this after creating the UI, or after operations that
    // may not trigger APVTS listeners (depending on project architecture).
    //==========================================================================
    void forceRefreshFromParameters();

private:
    //==========================================================================
    // Parameter ID helper (lane-suffixed)
    //
    // Single source of truth: this MUST match the DSP / PluginParameters.
    //==========================================================================
    juce::String laneId(const char* baseId) const
    {
        return ParamIDs::lane(baseId, lane);
    }

    //==========================================================================
    // APVTS listener callback
    //
    // IMPORTANT:
    // This can be called from the audio thread. Never touch UI objects here.
    //==========================================================================
    void parameterChanged(const juce::String& parameterID, float newValue) override;

    //==========================================================================
    // AsyncUpdater (message thread)
    //==========================================================================
    void handleAsyncUpdate() override;

    //==========================================================================
    // Timer (optional smoothing)
    //==========================================================================
    void timerCallback() override;

    //==========================================================================
    // Slider listener (uniqueness enforcement)
    //
    // Slider::Listener is used only to enforce the uniqueness rule. Actual
    // APVTS writes are done via each slider's onValueChange callback in .cpp.
    //==========================================================================
    void sliderValueChanged(juce::Slider* slider) override;
    void enforceUniqueValues(juce::Slider* changedSlider);
    void showModulePresetMenu();
    void showSaveModulePresetDialog();
    bool saveLoadedModulePreset();
    void showEditLoadedPresetDialog();
    [[nodiscard]] juce::ValueTree captureModulePresetState() const;
    void applyModulePresetState(const juce::ValueTree& state);

    //==========================================================================
    // APVTS listener registration helpers
    //==========================================================================
    void registerParameterListeners();
    void unregisterParameterListeners();

    //==========================================================================
    // APVTS -> UI synchronization
    //
    // Pulls APVTS values and updates widgets with dontSendNotification.
    // This function runs on the message thread only.
    //==========================================================================
    void refreshUIFromParameters();

    //==========================================================================
    // Bypass gating (robust bypass)
    //==========================================================================
    void applyGlobalUiBypass(bool bypassed);

    //==========================================================================
    // Visual helper: wrap a component inside a ShadowComponent container
    //==========================================================================
    template<typename Comp>
    void addWithShadow(std::unique_ptr<ShadowComponent>& shadowPtr,
                       Comp& comp,
                       const juce::DropShadow& shadow)
    {
        auto shadowComp = std::make_unique<ShadowComponent>(
            shadow, -1.0f, true, true, true, true, 10);

        shadowComp->addAndMakeVisible(comp);
        addAndMakeVisible(*shadowComp);
        shadowPtr = std::move(shadowComp);
    }

    //==========================================================================
    // References / guards
    //==========================================================================
    juce::AudioProcessorValueTreeState& parameters;
    MidivisiViciAudioProcessor& processor;
    const Lanes::Lane lane;

    // Cached lane IDs (built once in ctor to avoid repeated string allocations).
    juce::String idHarmonizerEnable;
    juce::String idPitchCorrector;
    juce::String idOctavePlus;
    juce::String idOctaveMinus;
    juce::String idVoice2;
    juce::String idVoice3;
    juce::String idVoice4;
    juce::String idVoice5;
    juce::String idVoice2VelMod;
    juce::String idVoice3VelMod;
    juce::String idVoice4VelMod;
    juce::String idVoice5VelMod;

    // Cached parameter pointers (owned by APVTS, valid for plugin lifetime).
    juce::RangedAudioParameter* pHarmonizerEnable = nullptr;
    juce::RangedAudioParameter* pPitchCorrector = nullptr;
    juce::RangedAudioParameter* pOctavePlus = nullptr;
    juce::RangedAudioParameter* pOctaveMinus = nullptr;
    juce::RangedAudioParameter* pVoice2 = nullptr;
    juce::RangedAudioParameter* pVoice3 = nullptr;
    juce::RangedAudioParameter* pVoice4 = nullptr;
    juce::RangedAudioParameter* pVoice5 = nullptr;
    juce::RangedAudioParameter* pVoice2VelMod = nullptr;
    juce::RangedAudioParameter* pVoice3VelMod = nullptr;
    juce::RangedAudioParameter* pVoice4VelMod = nullptr;
    juce::RangedAudioParameter* pVoice5VelMod = nullptr;

    // Prevent feedback loops when UI is updated from APVTS.
    std::atomic<bool> applyingFromState { false };
    juce::File loadedModulePresetFile;
    juce::String loadedModulePresetName;

    //==========================================================================
    // Header (Enable)
    //==========================================================================
    LeftClickToggleButton harmonizerTitleButton { "Harmonizer" };
    std::unique_ptr<ShadowComponent> harmonizerTitleButtonShadow;

    //==========================================================================
    // Rotaries
    //==========================================================================
    std::unique_ptr<CustomRotaryWithCombo> pitchCorrector;

    juce::Label octaveRandomizerLabel;
    std::unique_ptr<CustomRotaryWithCombo> octaveRandomizerPlus;
    std::unique_ptr<CustomRotaryWithCombo> octaveRandomizerMinus;

    std::unique_ptr<CustomRotaryWithCombo> voice2VelocityMod;
    std::unique_ptr<CustomRotaryWithCombo> voice3VelocityMod;
    std::unique_ptr<CustomRotaryWithCombo> voice4VelocityMod;
    std::unique_ptr<CustomRotaryWithCombo> voice5VelocityMod;
    juce::Label additionalVoicesLabel;
    juce::Label velocityModificatorLabel;

    //==========================================================================
    // Vertical sliders (voice offsets) + shadows
    //==========================================================================
    juce::Slider voice2Slider { juce::Slider::LinearVertical, juce::Slider::NoTextBox };
    juce::Slider voice3Slider { juce::Slider::LinearVertical, juce::Slider::NoTextBox };
    juce::Slider voice4Slider { juce::Slider::LinearVertical, juce::Slider::NoTextBox };
    juce::Slider voice5Slider { juce::Slider::LinearVertical, juce::Slider::NoTextBox };

    std::unique_ptr<ShadowComponent> voice2Shadow;
    std::unique_ptr<ShadowComponent> voice3Shadow;
    std::unique_ptr<ShadowComponent> voice4Shadow;
    std::unique_ptr<ShadowComponent> voice5Shadow;

    // Smoothing targets (semitones).
    int targetVoice2 = 0;
    int targetVoice3 = 0;
    int targetVoice4 = 0;
    int targetVoice5 = 0;

    // Used to perform a "hard set" on first refresh, then allow Timer smoothing.
    bool voicesInitialised = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Harmonizer)
};
