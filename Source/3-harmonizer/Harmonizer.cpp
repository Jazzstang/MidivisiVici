//==============================================================================
// Harmonizer.cpp
// -----------------------------------------------------------------------------
// MidivisiVici - UI Module: Harmonizer (lane-based)
//
// This component is the UI front-end for the Harmonizer MIDI module.
//
// The UI is fully manual-bound to the APVTS (no attachments).
// Manual binding gives full control, but it must respect JUCE parameter mapping.
//
// THREAD MODEL
// ------------
// APVTS can call parameterChanged() from a non-message thread (sometimes audio).
// We therefore use a 2-step sync:
//
//   1) parameterChanged(): does NOT touch UI, only triggers AsyncUpdater.
//   2) handleAsyncUpdate(): runs on the message thread and updates UI controls
//      using dontSendNotification.
//
// PARAMETER MAPPING
// -----------------
// setValueNotifyingHost() expects a normalized value in [0..1].
// To stay correct for int/choice/float parameters, always use:
//
//   read : real = param->convertFrom0to1(param->getValue())
//   write: norm = param->convertTo0to1(real)
//
// LANE MODEL
// ----------
// One Harmonizer UI exists per lane (A/B/C). Parameter IDs are derived via
// ParamIDs::lane(baseId, lane) so UI and DSP always target the same parameters.
//
// ROBUST BYPASS
// -------------
// The title toggle is always clickable.
// When bypassed, other controls are disabled and the whole module is dimmed.
//
// VOICE UNIQUENESS (USER ACTION ONLY)
// -----------------------------------
// Voice sliders represent semitone offsets. Multiple sliders can be 0.
// Any non-zero value must be unique across Voice2..Voice5.
// Enforced only on user edits (Slider::Listener).
//
// SMOOTHING
// ---------
// A Timer smoothly converges voice sliders toward their APVTS target values
// after automation / snapshot recall, without fighting user drags.
//==============================================================================

#include "Harmonizer.h"
#include "../PluginColours.h"
#include "../PluginParameters.h"
#include "../PluginProcessor.h"
#include "../UiMetrics.h"
#include "../0-component/ModulePresetStore.h"

#include <algorithm>
#include <cmath>

//==============================================================================
// Local helpers (parameter access + mapping)
//==============================================================================

namespace
{
    constexpr int kVelocityModMinStep = -10;
    constexpr int kVelocityModMaxStep = 10;
    constexpr int kVelocityModCenterIndex = 10;

    static inline juce::RangedAudioParameter* getParam(juce::AudioProcessorValueTreeState& apvts,
                                                       const juce::String& pid) noexcept
    {
        return apvts.getParameter(pid);
    }

    static inline void writeParamBool(juce::RangedAudioParameter* p, bool state)
    {
        if (!p)
            return;

        const bool current = (p->getValue() > 0.5f);
        if (current == state)
            return;

        p->beginChangeGesture();
        p->setValueNotifyingHost(state ? 1.0f : 0.0f);
        p->endChangeGesture();
    }

    static inline int readParamInt(juce::RangedAudioParameter* p, int fallback) noexcept
    {
        if (!p)
            return fallback;

        const float real = p->convertFrom0to1(p->getValue());
        return juce::roundToInt(real);
    }

    static inline void writeParamInt(juce::RangedAudioParameter* p, int realValue)
    {
        if (!p)
            return;

        const int currentReal = juce::roundToInt(p->convertFrom0to1(p->getValue()));
        if (currentReal == realValue)
            return;

        const float norm = p->convertTo0to1((float) realValue);

        p->beginChangeGesture();
        p->setValueNotifyingHost(norm);
        p->endChangeGesture();
    }

    static inline int velocityModStepToIndex(int step) noexcept
    {
        return juce::jlimit(kVelocityModMinStep, kVelocityModMaxStep, step) + kVelocityModCenterIndex;
    }

    static inline int velocityModIndexToStep(int index) noexcept
    {
        return juce::jlimit(0, 2 * kVelocityModCenterIndex, index) - kVelocityModCenterIndex;
    }

    static std::vector<juce::String> makeVelocityModPercentLabels()
    {
        std::vector<juce::String> labels;
        labels.reserve(2 * kVelocityModCenterIndex + 1);

        for (int step = kVelocityModMinStep; step <= kVelocityModMaxStep; ++step)
        {
            const int pct = step * 10;
            juce::String text;
            if (pct > 0)
                text << "+" << pct << "%";
            else
                text << pct << "%";

            labels.push_back(text);
        }

        return labels;
    }

}

//==============================================================================
// Listener registration
//==============================================================================

void Harmonizer::registerParameterListeners()
{
    parameters.addParameterListener(idHarmonizerEnable, this);

    parameters.addParameterListener(idPitchCorrector, this);
    parameters.addParameterListener(idOctavePlus, this);
    parameters.addParameterListener(idOctaveMinus, this);

    parameters.addParameterListener(idVoice2, this);
    parameters.addParameterListener(idVoice3, this);
    parameters.addParameterListener(idVoice4, this);
    parameters.addParameterListener(idVoice5, this);
    parameters.addParameterListener(idVoice2VelMod, this);
    parameters.addParameterListener(idVoice3VelMod, this);
    parameters.addParameterListener(idVoice4VelMod, this);
    parameters.addParameterListener(idVoice5VelMod, this);
}

void Harmonizer::unregisterParameterListeners()
{
    parameters.removeParameterListener(idHarmonizerEnable, this);

    parameters.removeParameterListener(idPitchCorrector, this);
    parameters.removeParameterListener(idOctavePlus, this);
    parameters.removeParameterListener(idOctaveMinus, this);

    parameters.removeParameterListener(idVoice2, this);
    parameters.removeParameterListener(idVoice3, this);
    parameters.removeParameterListener(idVoice4, this);
    parameters.removeParameterListener(idVoice5, this);
    parameters.removeParameterListener(idVoice2VelMod, this);
    parameters.removeParameterListener(idVoice3VelMod, this);
    parameters.removeParameterListener(idVoice4VelMod, this);
    parameters.removeParameterListener(idVoice5VelMod, this);
}

//==============================================================================
// APVTS callback (may be audio thread)
//==============================================================================

void Harmonizer::parameterChanged(const juce::String& /*parameterID*/, float /*newValue*/)
{
    // Never touch UI here. Schedule a refresh on the message thread.
    triggerAsyncUpdate();
}

//==============================================================================
// AsyncUpdater (message thread)
//==============================================================================

void Harmonizer::handleAsyncUpdate()
{
    refreshUIFromParameters();
}

void Harmonizer::forceRefreshFromParameters()
{
    triggerAsyncUpdate();
}

//==============================================================================
// Robust bypass gating
//==============================================================================

void Harmonizer::applyGlobalUiBypass(bool bypassed)
{
    // Title must remain clickable even when bypassed.
    if (harmonizerTitleButtonShadow)
        harmonizerTitleButtonShadow->setEnabled(true);

    harmonizerTitleButton.setEnabled(true);

    const bool enableChildren = !bypassed;

    if (pitchCorrector)        pitchCorrector->setEnabled(enableChildren);
    if (octaveRandomizerPlus)  octaveRandomizerPlus->setEnabled(enableChildren);
    if (octaveRandomizerMinus) octaveRandomizerMinus->setEnabled(enableChildren);
    if (voice2VelocityMod)     voice2VelocityMod->setEnabled(enableChildren);
    if (voice3VelocityMod)     voice3VelocityMod->setEnabled(enableChildren);
    if (voice4VelocityMod)     voice4VelocityMod->setEnabled(enableChildren);
    if (voice5VelocityMod)     voice5VelocityMod->setEnabled(enableChildren);
    additionalVoicesLabel.setEnabled(enableChildren);
    velocityModificatorLabel.setEnabled(enableChildren);

    octaveRandomizerLabel.setEnabled(enableChildren);

    voice2Slider.setEnabled(enableChildren);
    voice3Slider.setEnabled(enableChildren);
    voice4Slider.setEnabled(enableChildren);
    voice5Slider.setEnabled(enableChildren);

    // Visual "disabled" hint for the whole module.
    setAlpha(bypassed ? 0.55f : 1.0f);
}

//==============================================================================
// APVTS -> UI sync (message thread only, no notifications)
//==============================================================================

void Harmonizer::refreshUIFromParameters()
{
    applyingFromState.store(true);

    //--------------------------------------------------------------------------
    // 1) Enable / bypass
    //--------------------------------------------------------------------------
    const bool enabled = (pHarmonizerEnable != nullptr) ? (pHarmonizerEnable->getValue() > 0.5f) : true;

    harmonizerTitleButton.setToggleState(enabled, juce::dontSendNotification);
    applyGlobalUiBypass(!enabled);

    //--------------------------------------------------------------------------
    // 2) Pitch Corrector (-12..+12)
    //--------------------------------------------------------------------------
    if (pitchCorrector)
    {
        const int v = juce::jlimit(-12, 12, readParamInt(pPitchCorrector, 0));
        if ((int) juce::roundToInt(pitchCorrector->getValue()) != v)
            pitchCorrector->setValue(v, juce::dontSendNotification);
    }

    //--------------------------------------------------------------------------
    // 3) Octave randomizer ranges
    //
    // Stored params:
    //   plus  : 0..8
    //   minus : 0..8
    //
    // UI display:
    //   plus  :  0..8
    //   minus : -8..0 (visual only)
    //--------------------------------------------------------------------------
    if (octaveRandomizerPlus)
    {
        const int v = juce::jlimit(0, 8, readParamInt(pOctavePlus, 0));
        if ((int) juce::roundToInt(octaveRandomizerPlus->getValue()) != v)
            octaveRandomizerPlus->setValue(v, juce::dontSendNotification);
    }

    if (octaveRandomizerMinus)
    {
        const int stored = juce::jlimit(0, 8, readParamInt(pOctaveMinus, 0));
        const int uiValue = -stored;
        if ((int) juce::roundToInt(octaveRandomizerMinus->getValue()) != uiValue)
            octaveRandomizerMinus->setValue(uiValue, juce::dontSendNotification);
    }

    //--------------------------------------------------------------------------
    // 4) Additional voice velocity modifiers (-100..+100%, step 10)
    //--------------------------------------------------------------------------
    auto syncVelocityMod = [&](std::unique_ptr<CustomRotaryWithCombo>& rotary,
                               juce::RangedAudioParameter* param)
    {
        if (!rotary)
            return;

        const int step = juce::jlimit(kVelocityModMinStep, kVelocityModMaxStep, readParamInt(param, 0));
        const int index = velocityModStepToIndex(step);
        if ((int) juce::roundToInt(rotary->getValue()) != index)
            rotary->setValue(index, juce::dontSendNotification);
    };

    syncVelocityMod(voice2VelocityMod, pVoice2VelMod);
    syncVelocityMod(voice3VelocityMod, pVoice3VelMod);
    syncVelocityMod(voice4VelocityMod, pVoice4VelMod);
    syncVelocityMod(voice5VelocityMod, pVoice5VelMod);

    //--------------------------------------------------------------------------
    // 5) Voice sliders (-24..+24)
    //
    // We keep "targetVoiceX" as a mirror used by the Timer smoothing.
    // On first initialization, we hard-set the slider value.
    //--------------------------------------------------------------------------
    auto syncVoice = [&](juce::Slider& slider, juce::RangedAudioParameter* param, int& target)
    {
        const int v = juce::jlimit(-24, 24, readParamInt(param, 0));
        target = v;

        if (!voicesInitialised)
            slider.setValue((double) v, juce::dontSendNotification);
    };

    syncVoice(voice2Slider, pVoice2, targetVoice2);
    syncVoice(voice3Slider, pVoice3, targetVoice3);
    syncVoice(voice4Slider, pVoice4, targetVoice4);
    syncVoice(voice5Slider, pVoice5, targetVoice5);

    voicesInitialised = true;

    // No full-component repaint here:
    // child controls repaint themselves on value updates, which keeps
    // message-thread load lower during dense automation updates.
    applyingFromState.store(false);
}

//==============================================================================
// Constructor / destructor
//==============================================================================

Harmonizer::Harmonizer(juce::AudioProcessorValueTreeState& state,
                       MidivisiViciAudioProcessor& processorIn,
                       Lanes::Lane laneIn)
    : parameters(state)
    , processor(processorIn)
    , lane(laneIn)
    , idHarmonizerEnable(laneId(ParamIDs::Base::harmonizerEnable))
    , idPitchCorrector(laneId(ParamIDs::Base::harmPitchCorrector))
    , idOctavePlus(laneId(ParamIDs::Base::harmOctavePlusRandom))
    , idOctaveMinus(laneId(ParamIDs::Base::harmOctaveMinusRandom))
    , idVoice2(laneId(ParamIDs::Base::harmVoice2))
    , idVoice3(laneId(ParamIDs::Base::harmVoice3))
    , idVoice4(laneId(ParamIDs::Base::harmVoice4))
    , idVoice5(laneId(ParamIDs::Base::harmVoice5))
    , idVoice2VelMod(laneId(ParamIDs::Base::harmVoice2VelMod))
    , idVoice3VelMod(laneId(ParamIDs::Base::harmVoice3VelMod))
    , idVoice4VelMod(laneId(ParamIDs::Base::harmVoice4VelMod))
    , idVoice5VelMod(laneId(ParamIDs::Base::harmVoice5VelMod))
{
    // Cache parameter pointers once. This avoids repeated APVTS lookups in
    // high-frequency UI paths (timer smoothing, automation refresh).
    pHarmonizerEnable = getParam(parameters, idHarmonizerEnable);
    pPitchCorrector   = getParam(parameters, idPitchCorrector);
    pOctavePlus       = getParam(parameters, idOctavePlus);
    pOctaveMinus      = getParam(parameters, idOctaveMinus);
    pVoice2           = getParam(parameters, idVoice2);
    pVoice3           = getParam(parameters, idVoice3);
    pVoice4           = getParam(parameters, idVoice4);
    pVoice5           = getParam(parameters, idVoice5);
    pVoice2VelMod     = getParam(parameters, idVoice2VelMod);
    pVoice3VelMod     = getParam(parameters, idVoice3VelMod);
    pVoice4VelMod     = getParam(parameters, idVoice4VelMod);
    pVoice5VelMod     = getParam(parameters, idVoice5VelMod);

    registerParameterListeners();

    //==========================================================================
    // Header: title toggle (Enable)
    //==========================================================================
    harmonizerTitleButton.setClickingTogglesState(true);

    harmonizerTitleButton.onClick = [this]
    {
        if (applyingFromState.load())
            return;

        const bool nextState = harmonizerTitleButton.getToggleState();
        writeParamBool(pHarmonizerEnable, nextState);
        applyGlobalUiBypass(!nextState);
    };
    harmonizerTitleButton.onPopupClick = [this](const juce::MouseEvent&)
    {
        showModulePresetMenu();
    };

    addWithShadow(harmonizerTitleButtonShadow, harmonizerTitleButton,
                  juce::DropShadow(juce::Colours::black, 0, { 0, 0 }));

    //==========================================================================
    // Rotaries
    //==========================================================================
    pitchCorrector = std::make_unique<CustomRotaryWithCombo>(
        "Pitch Corrector", -12, 12, 0, true, CustomRotaryWithCombo::ArcMode::CenterToCursor);
    addAndMakeVisible(*pitchCorrector);

    pitchCorrector->onValueChange = [this](int newVal)
    {
        if (applyingFromState.load())
            return;

        writeParamInt(pPitchCorrector, juce::jlimit(-12, 12, newVal));
    };

    octaveRandomizerLabel.setText("Octave\nRandomizer", juce::dontSendNotification);
    octaveRandomizerLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(octaveRandomizerLabel);

    additionalVoicesLabel.setText("Additional Voices", juce::dontSendNotification);
    additionalVoicesLabel.setJustificationType(juce::Justification::centred);
    additionalVoicesLabel.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(additionalVoicesLabel);

    velocityModificatorLabel.setText("Velocity Modificator", juce::dontSendNotification);
    velocityModificatorLabel.setJustificationType(juce::Justification::centred);
    velocityModificatorLabel.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(velocityModificatorLabel);

    octaveRandomizerPlus = std::make_unique<CustomRotaryWithCombo>(
        "+", 0, 8, 0, false, CustomRotaryWithCombo::ArcMode::LeftToCursor);
    addAndMakeVisible(*octaveRandomizerPlus);

    octaveRandomizerPlus->onValueChange = [this](int v)
    {
        if (applyingFromState.load())
            return;

        writeParamInt(pOctavePlus, juce::jlimit(0, 8, v));
    };

    octaveRandomizerMinus = std::make_unique<CustomRotaryWithCombo>(
        "-", -8, 0, 0, false, CustomRotaryWithCombo::ArcMode::RightToCursor);
    octaveRandomizerMinus->setInvertedFill(true);
    addAndMakeVisible(*octaveRandomizerMinus);

    octaveRandomizerMinus->onValueChange = [this](int neg)
    {
        if (applyingFromState.load())
            return;

        // UI is -8..0, stored parameter is 0..8 (magnitude).
        const int stored = juce::jlimit(0, 8, -neg);

        writeParamInt(pOctaveMinus, stored);
    };

    //==========================================================================
    // Additional voice velocity modifiers (voice2..voice5)
    // - Stored as steps: -10..+10 (1 step = 10%)
    // - UI display uses percent labels: -100%..+100%
    //==========================================================================
    const auto velocityModLabels = makeVelocityModPercentLabels();

    auto bindVelocityMod = [this, &velocityModLabels](std::unique_ptr<CustomRotaryWithCombo>& rotary,
                                                      const juce::String& label,
                                                      juce::RangedAudioParameter* param)
    {
        rotary = std::make_unique<CustomRotaryWithCombo>(
            label, 0, 2 * kVelocityModCenterIndex, kVelocityModCenterIndex, true,
            CustomRotaryWithCombo::ArcMode::CenterToCursor);

        rotary->setStringList(velocityModLabels);
        rotary->setValue(kVelocityModCenterIndex, juce::dontSendNotification);
        addAndMakeVisible(*rotary);

        rotary->onValueChange = [this, param](int index)
        {
            if (applyingFromState.load())
                return;

            writeParamInt(param, velocityModIndexToStep(index));
        };
    };

    bindVelocityMod(voice2VelocityMod, "", pVoice2VelMod);
    bindVelocityMod(voice3VelocityMod, "", pVoice3VelMod);
    bindVelocityMod(voice4VelocityMod, "", pVoice4VelMod);
    bindVelocityMod(voice5VelocityMod, "", pVoice5VelMod);

    //==========================================================================
    // Voice sliders (vertical)
    //==========================================================================
    voice2Slider.setRange(-24, 24, 1);
    voice3Slider.setRange(-24, 24, 1);
    voice4Slider.setRange(-24, 24, 1);
    voice5Slider.setRange(-24, 24, 1);

    // UI bootstrap at 0 for all additional voices (disabled state).
    // APVTS sync below may replace these with recalled values.
    voice2Slider.setValue(0.0, juce::dontSendNotification);
    voice3Slider.setValue(0.0, juce::dontSendNotification);
    voice4Slider.setValue(0.0, juce::dontSendNotification);
    voice5Slider.setValue(0.0, juce::dontSendNotification);

    targetVoice2 = 0;
    targetVoice3 = 0;
    targetVoice4 = 0;
    targetVoice5 = 0;

    addAndMakeVisible(voice2Slider);
    addAndMakeVisible(voice3Slider);
    addAndMakeVisible(voice4Slider);
    addAndMakeVisible(voice5Slider);

    // Slider::Listener is only used to enforce uniqueness on user action.
    voice2Slider.addListener(this);
    voice3Slider.addListener(this);
    voice4Slider.addListener(this);
    voice5Slider.addListener(this);

    // Manual binding: slider -> APVTS parameter (real -> normalized).
    auto bindVoiceSlider = [this](juce::Slider& slider, juce::RangedAudioParameter* param, int& target)
    {
        slider.onValueChange = [this, &slider, param, &target]
        {
            if (applyingFromState.load())
                return;

            writeParamInt(param, (int) slider.getValue());

            target = (int) slider.getValue();
        };
    };

    bindVoiceSlider(voice2Slider, pVoice2, targetVoice2);
    bindVoiceSlider(voice3Slider, pVoice3, targetVoice3);
    bindVoiceSlider(voice4Slider, pVoice4, targetVoice4);
    bindVoiceSlider(voice5Slider, pVoice5, targetVoice5);

    // Initial APVTS -> UI sync.
    refreshUIFromParameters();

}

Harmonizer::~Harmonizer()
{
    cancelPendingUpdate();
    unregisterParameterListeners();
}

//==============================================================================
// Timer smoothing
//==============================================================================

void Harmonizer::timerCallback()
{
    if (applyingFromState.load())
        return;

    if (voice2Slider.isMouseButtonDown() ||
        voice3Slider.isMouseButtonDown() ||
        voice4Slider.isMouseButtonDown() ||
        voice5Slider.isMouseButtonDown())
        return;

    auto smoothMove = [](juce::Slider& slider, int target)
    {
        const double current = slider.getValue();
        const double diff = (double) target - current;

        if (std::abs(diff) > 0.01)
            slider.setValue(current + diff * 0.3, juce::dontSendNotification);
        else if (std::abs(diff) > 1.0e-6)
            slider.setValue((double) target, juce::dontSendNotification);
    };

    // Important:
    // smoothing is visual-only; it must not write parameters back.
    applyingFromState.store(true);
    smoothMove(voice2Slider, targetVoice2);
    smoothMove(voice3Slider, targetVoice3);
    smoothMove(voice4Slider, targetVoice4);
    smoothMove(voice5Slider, targetVoice5);
    applyingFromState.store(false);
}

void Harmonizer::uiTimerTick()
{
    auto tickRotary = [](std::unique_ptr<CustomRotaryWithCombo>& rotary)
    {
        if (rotary)
            rotary->uiTimerTick();
    };

    tickRotary(pitchCorrector);
    tickRotary(octaveRandomizerPlus);
    tickRotary(octaveRandomizerMinus);
    tickRotary(voice2VelocityMod);
    tickRotary(voice3VelocityMod);
    tickRotary(voice4VelocityMod);
    tickRotary(voice5VelocityMod);

    // UI-only modulation preview:
    // pull effective values (base + LFO) from processor and display them
    // without writing APVTS state.
    const int effectiveVoiceOffsets[4] =
    {
        processor.getLaneHarmonizerVoiceOffsetDisplayValueForUI(lane, 0),
        processor.getLaneHarmonizerVoiceOffsetDisplayValueForUI(lane, 1),
        processor.getLaneHarmonizerVoiceOffsetDisplayValueForUI(lane, 2),
        processor.getLaneHarmonizerVoiceOffsetDisplayValueForUI(lane, 3)
    };

    const int effectiveVelocitySteps[4] =
    {
        processor.getLaneHarmonizerVelocityModDisplayValueForUI(lane, 0),
        processor.getLaneHarmonizerVelocityModDisplayValueForUI(lane, 1),
        processor.getLaneHarmonizerVelocityModDisplayValueForUI(lane, 2),
        processor.getLaneHarmonizerVelocityModDisplayValueForUI(lane, 3)
    };

    applyingFromState.store(true);

    auto applySliderPreview = [](juce::Slider& slider, int value)
    {
        if (slider.isMouseButtonDown())
            return;

        const int clamped = juce::jlimit(-24, 24, value);
        if (juce::roundToInt(slider.getValue()) != clamped)
            slider.setValue((double) clamped, juce::dontSendNotification);
    };

    applySliderPreview(voice2Slider, effectiveVoiceOffsets[0]);
    applySliderPreview(voice3Slider, effectiveVoiceOffsets[1]);
    applySliderPreview(voice4Slider, effectiveVoiceOffsets[2]);
    applySliderPreview(voice5Slider, effectiveVoiceOffsets[3]);

    targetVoice2 = effectiveVoiceOffsets[0];
    targetVoice3 = effectiveVoiceOffsets[1];
    targetVoice4 = effectiveVoiceOffsets[2];
    targetVoice5 = effectiveVoiceOffsets[3];

    auto applyVelocityPreview = [](std::unique_ptr<CustomRotaryWithCombo>& rotary, int step)
    {
        if (!rotary)
            return;

        rotary->setDisplayValueOverride((float) velocityModStepToIndex(step));
    };

    applyVelocityPreview(voice2VelocityMod, effectiveVelocitySteps[0]);
    applyVelocityPreview(voice3VelocityMod, effectiveVelocitySteps[1]);
    applyVelocityPreview(voice4VelocityMod, effectiveVelocitySteps[2]);
    applyVelocityPreview(voice5VelocityMod, effectiveVelocitySteps[3]);

    applyingFromState.store(false);

    timerCallback();
}

//==============================================================================
// Uniqueness enforcement (user action only)
//==============================================================================

void Harmonizer::sliderValueChanged(juce::Slider* slider)
{
    if (applyingFromState.load() || slider == nullptr)
        return;

    enforceUniqueValues(slider);

    if (slider == &voice2Slider)      targetVoice2 = (int) slider->getValue();
    else if (slider == &voice3Slider) targetVoice3 = (int) slider->getValue();
    else if (slider == &voice4Slider) targetVoice4 = (int) slider->getValue();
    else if (slider == &voice5Slider) targetVoice5 = (int) slider->getValue();
}

void Harmonizer::enforceUniqueValues(juce::Slider* changedSlider)
{
    const int desiredValue = (int) changedSlider->getValue();

    // Design rule:
    // - 0 disables the additional voice and must always remain reachable.
    // - uniqueness applies only to non-zero offsets.
    if (desiredValue == 0)
        return;

    const auto isTakenByOtherVoice = [changedSlider, this](int candidate) noexcept
    {
        for (auto* s : { &voice2Slider, &voice3Slider, &voice4Slider, &voice5Slider })
        {
            if (s == changedSlider)
                continue;
            if ((int) s->getValue() == candidate)
                return true;
        }
        return false;
    };

    if (!isTakenByOtherVoice(desiredValue))
        return;

    const int minValue = (int) changedSlider->getMinimum();
    const int maxValue = (int) changedSlider->getMaximum();
    const int primaryStep = (desiredValue >= 0) ? 1 : -1;
    const int span = maxValue - minValue;

    // Search strategy:
    // 1) keep previous behavior by preferring movement away from zero
    // 2) fallback to opposite direction to avoid "stuck at range edge"
    int resolved = desiredValue;

    for (int delta = 1; delta <= span; ++delta)
    {
        const int cand = desiredValue + primaryStep * delta;
        if (cand < minValue || cand > maxValue)
            continue;
        if (!isTakenByOtherVoice(cand))
        {
            resolved = cand;
            break;
        }
    }

    if (resolved == desiredValue)
    {
        for (int delta = 1; delta <= span; ++delta)
        {
            const int cand = desiredValue - primaryStep * delta;
            if (cand < minValue || cand > maxValue)
                continue;
            if (!isTakenByOtherVoice(cand))
            {
                resolved = cand;
                break;
            }
        }
    }

    if (resolved != desiredValue)
        changedSlider->setValue((double) resolved, juce::sendNotificationSync);
}

void Harmonizer::showSaveModulePresetDialog()
{
    auto* dialog = new juce::AlertWindow("Save As Harmonizer Preset",
                                         "Enter preset name.",
                                         juce::AlertWindow::NoIcon);
    dialog->addTextEditor("name", "Preset", "Name:");
    dialog->addButton("Save As", 1, juce::KeyPress(juce::KeyPress::returnKey));
    dialog->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    auto dialogSafe = juce::Component::SafePointer<juce::AlertWindow>(dialog);
    auto compSafe = juce::Component::SafePointer<Harmonizer>(this);

    dialog->enterModalState(true,
                            juce::ModalCallbackFunction::create(
                                [dialogSafe, compSafe](int result)
                                {
                                    if (result != 1 || dialogSafe == nullptr || compSafe == nullptr)
                                        return;

                                    const auto* textEditor = dialogSafe->getTextEditor("name");
                                    const auto presetName = (textEditor != nullptr)
                                                                ? textEditor->getText().trim()
                                                                : juce::String();
                                    if (presetName.isEmpty())
                                        return;

                                    juce::String error;
                                    if (!ModulePresetStore::savePreset("harmonizer",
                                                                       presetName,
                                                                       compSafe->captureModulePresetState(),
                                                                       &error))
                                    {
                                        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                                               "Save Harmonizer Preset",
                                                                               error);
                                    }
                                }),
                            true);
}

bool Harmonizer::saveLoadedModulePreset()
{
    if (!loadedModulePresetFile.existsAsFile())
        return false;

    juce::String error;
    if (!ModulePresetStore::savePresetInPlace(loadedModulePresetFile,
                                              "harmonizer",
                                              captureModulePresetState(),
                                              &error))
    {
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                               "Save Harmonizer Preset",
                                               error);
        return false;
    }

    return true;
}

void Harmonizer::showEditLoadedPresetDialog()
{
    if (!loadedModulePresetFile.existsAsFile())
        return;

    juce::ValueTree payload;
    juce::String presetName;
    juce::String moduleKey;
    juce::String error;
    if (!ModulePresetStore::loadPresetPayload(loadedModulePresetFile,
                                              payload,
                                              &presetName,
                                              &moduleKey,
                                              &error))
    {
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                               "Edit Harmonizer Preset",
                                               error);
        return;
    }

    auto* dialog = new juce::AlertWindow("Edit Harmonizer Preset",
                                         "Rename or delete this preset.",
                                         juce::AlertWindow::NoIcon);
    dialog->addTextEditor("name",
                          presetName.trim().isNotEmpty() ? presetName.trim()
                                                          : loadedModulePresetFile.getFileNameWithoutExtension(),
                          "Name:");
    dialog->addButton("Rename", 1, juce::KeyPress(juce::KeyPress::returnKey));
    dialog->addButton("Delete", 2);
    dialog->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    auto dialogSafe = juce::Component::SafePointer<juce::AlertWindow>(dialog);
    auto compSafe = juce::Component::SafePointer<Harmonizer>(this);
    dialog->enterModalState(true,
                            juce::ModalCallbackFunction::create(
                                [dialogSafe, compSafe](int result)
                                {
                                    if (result == 0 || dialogSafe == nullptr || compSafe == nullptr)
                                        return;

                                    if (result == 1)
                                    {
                                        const auto* te = dialogSafe->getTextEditor("name");
                                        const auto newName = te != nullptr ? te->getText().trim() : juce::String();
                                        if (newName.isEmpty())
                                            return;

                                        juce::File renamedFile;
                                        juce::String error;
                                        if (!ModulePresetStore::renamePresetFile(compSafe->loadedModulePresetFile,
                                                                                 "harmonizer",
                                                                                 newName,
                                                                                 &renamedFile,
                                                                                 &error))
                                        {
                                            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                                                   "Edit Harmonizer Preset",
                                                                                   error);
                                            return;
                                        }

                                        compSafe->loadedModulePresetFile = renamedFile;
                                        compSafe->loadedModulePresetName = newName;
                                        return;
                                    }

                                    if (result == 2)
                                    {
                                        juce::String error;
                                        if (!ModulePresetStore::deletePresetFile(compSafe->loadedModulePresetFile,
                                                                                 "harmonizer",
                                                                                 &error))
                                        {
                                            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                                                   "Edit Harmonizer Preset",
                                                                                   error);
                                            return;
                                        }

                                        compSafe->loadedModulePresetFile = juce::File();
                                        compSafe->loadedModulePresetName.clear();
                                    }
                                }),
                            true);
}

juce::ValueTree Harmonizer::captureModulePresetState() const
{
    std::vector<juce::String> paramIds;
    paramIds.reserve(12);

    paramIds.push_back(idHarmonizerEnable);
    paramIds.push_back(idPitchCorrector);
    paramIds.push_back(idOctavePlus);
    paramIds.push_back(idOctaveMinus);
    paramIds.push_back(idVoice2);
    paramIds.push_back(idVoice3);
    paramIds.push_back(idVoice4);
    paramIds.push_back(idVoice5);
    paramIds.push_back(idVoice2VelMod);
    paramIds.push_back(idVoice3VelMod);
    paramIds.push_back(idVoice4VelMod);
    paramIds.push_back(idVoice5VelMod);

    return ModulePresetStore::captureParameterState(parameters, paramIds);
}

void Harmonizer::applyModulePresetState(const juce::ValueTree& state)
{
    if (!state.isValid())
        return;

    ModulePresetStore::applyParameterState(parameters, state);

    if (!isUpdatePending())
        triggerAsyncUpdate();
}

void Harmonizer::showModulePresetMenu()
{
    constexpr int kSaveId = 1;
    constexpr int kSaveAsId = 2;
    constexpr int kEditId = 3;
    constexpr int kLoadBaseId = 1000;

    juce::PopupMenu menu;
    const bool canSaveLoadedPreset = loadedModulePresetFile.existsAsFile();
    menu.addItem(kSaveId, "Save", canSaveLoadedPreset, false);
    menu.addItem(kSaveAsId, "Save As Preset...");
    menu.addItem(kEditId, "Edit Preset...", canSaveLoadedPreset, false);

    juce::PopupMenu loadMenu;
    const auto entries = ModulePresetStore::listPresets("harmonizer");
    if (entries.empty())
    {
        loadMenu.addItem(999999, "(No presets)", false, false);
    }
    else
    {
        for (int i = 0; i < (int) entries.size(); ++i)
            loadMenu.addItem(kLoadBaseId + i, entries[(size_t) i].displayName);
    }
    menu.addSubMenu("Load Preset", loadMenu);

    auto compSafe = juce::Component::SafePointer<Harmonizer>(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&harmonizerTitleButton),
                       [compSafe, entries, kSaveId, kSaveAsId, kEditId, kLoadBaseId](int result)
                       {
                           if (compSafe == nullptr || result == 0)
                               return;

                           if (result == kSaveId)
                           {
                               compSafe->saveLoadedModulePreset();
                               return;
                           }

                           if (result == kSaveAsId)
                           {
                               compSafe->showSaveModulePresetDialog();
                               return;
                           }

                           if (result == kEditId)
                           {
                               compSafe->showEditLoadedPresetDialog();
                               return;
                           }

                           if (result < kLoadBaseId || result >= kLoadBaseId + (int) entries.size())
                               return;

                           const auto& entry = entries[(size_t) (result - kLoadBaseId)];
                           juce::ValueTree payload;
                           juce::String error;
                           if (!ModulePresetStore::loadPresetPayload(entry.file, payload, nullptr, nullptr, &error))
                           {
                               juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                                      "Load Harmonizer Preset",
                                                                      error);
                               return;
                           }

                           compSafe->applyModulePresetState(payload);
                           compSafe->loadedModulePresetFile = entry.file;
                           compSafe->loadedModulePresetName = entry.displayName;
                       });
}

//==============================================================================
// JUCE Component overrides (RESTORED UI)
//==============================================================================

void Harmonizer::resized()
{
    // This layout is kept identical to the previously stable module version.
    // It assumes a fixed-ish module height in the global editor grid.

    constexpr int contentInset = UiMetrics::kModuleOuterMargin + UiMetrics::kModuleInnerMargin;
    auto area = getLocalBounds().reduced(contentInset);

    //--------------------------------------------------------------------------
    // Header (title)
    //--------------------------------------------------------------------------
    auto titleArea = area.removeFromTop(24);

    if (harmonizerTitleButtonShadow)
    {
        harmonizerTitleButtonShadow->setBounds(titleArea.expanded(10));
        harmonizerTitleButton.setBounds(harmonizerTitleButtonShadow->getShadowArea());
    }
    else
    {
        harmonizerTitleButton.setBounds(titleArea);
    }

    constexpr int voiceSliderWidth = 30;
    constexpr int voiceSpacing = 24;
    const int voiceColumnsWidth = voiceSliderWidth * 4 + voiceSpacing * 3;
    const int voiceColumnsStartX = area.getCentreX() - voiceColumnsWidth / 2;

    //--------------------------------------------------------------------------
    // Row 1: Rotaries (Pitch + Octave +/-)
    //--------------------------------------------------------------------------
    auto rotaryArea = area.removeFromTop(82);

    int potWidth = juce::jmin(rotaryArea.getHeight(), rotaryArea.getWidth() / 4);
    const int gapSmall = juce::jmax(6, potWidth / 5);
    const int gapLarge = juce::jmax(12, potWidth / 3);

    int totalWidth = 3 * potWidth + gapLarge + gapSmall;
    if (totalWidth > rotaryArea.getWidth())
    {
        potWidth = juce::jmax(24, (rotaryArea.getWidth() - gapLarge - gapSmall) / 3);
        totalWidth = 3 * potWidth + gapLarge + gapSmall;
    }

    const int y = rotaryArea.getY();
    const int h = rotaryArea.getHeight();

    int x = rotaryArea.getCentreX() - totalWidth / 2;

    if (pitchCorrector)
        pitchCorrector->setBounds(x, y, potWidth, h);

    x += potWidth + gapLarge;

    if (octaveRandomizerPlus)
        octaveRandomizerPlus->setBounds(x, y, potWidth, h);

    x += potWidth + gapSmall;

    if (octaveRandomizerMinus)
        octaveRandomizerMinus->setBounds(x, y, potWidth, h);

    // Center label above +/- rotaries
    if (octaveRandomizerPlus && octaveRandomizerMinus)
    {
        auto plusLabelBounds  = octaveRandomizerPlus->getBounds().withHeight(20);
        auto minusLabelBounds = octaveRandomizerMinus->getBounds().withHeight(20);

        const int labelHeight = 28;
        const int labelWidth  = minusLabelBounds.getRight() - plusLabelBounds.getX();
        const int labelY      = plusLabelBounds.getY() + 6;

        octaveRandomizerLabel.setBounds(plusLabelBounds.getX(), labelY, labelWidth, labelHeight);
    }

    //--------------------------------------------------------------------------
    // Row 2: 4 vertical sliders (voices)
    //--------------------------------------------------------------------------
    constexpr int firstSeparatorTopGap = 8;
    constexpr int lastBlockBottomGap = 8;
    constexpr int velocityModsBlockHeight = 78;
    constexpr int separatorHeight = 8;
    constexpr int velocityTopCompensation = 5;
    constexpr int separatorSideMargin = 15;

    area.removeFromTop(firstSeparatorTopGap);
    area.removeFromBottom(lastBlockBottomGap);

    auto velocityModsArea = area.removeFromBottom(velocityModsBlockHeight);
    auto velocityModTitleArea = velocityModsArea.removeFromTop(separatorHeight);
    const int separatorX = separatorSideMargin;
    const int separatorW = juce::jmax(1, getWidth() - separatorSideMargin * 2);
    velocityModificatorLabel.setBounds(separatorX, velocityModTitleArea.getY(), separatorW, velocityModTitleArea.getHeight());

    auto verticalArea = area;
    auto additionalVoicesTitleArea = verticalArea.removeFromTop(separatorHeight);
    additionalVoicesLabel.setBounds(separatorX, additionalVoicesTitleArea.getY(), separatorW, additionalVoicesTitleArea.getHeight());
    const int sliderHeight = verticalArea.getHeight();

    auto makeSeparatorText = [](juce::Label& label, const juce::String& centerText) -> juce::String
    {
        const auto font = label.getLookAndFeel().getLabelFont(label);
        const int totalW = label.getWidth();

        auto measureTextWidth = [&font](const juce::String& text) -> int
        {
            juce::AttributedString attributed(text);
            attributed.setFont(font);

            juce::TextLayout layout;
            layout.createLayout(attributed, 10000.0f);
            return juce::jmax(0, juce::roundToInt(layout.getWidth()));
        };

        const int textW = measureTextWidth(centerText);
        const int dashW = juce::jmax(1, measureTextWidth("-"));
        const int gapW = measureTextWidth(" ");

        const int sideCount = juce::jmax(2, (totalW - textW - gapW * 2) / (dashW * 2));
        juce::String side;
        for (int i = 0; i < sideCount; ++i)
            side << "-";
        return side + " " + centerText + " " + side;
    };

    additionalVoicesLabel.setText(makeSeparatorText(additionalVoicesLabel, "Additional Voices"),
                                  juce::dontSendNotification);
    velocityModificatorLabel.setText(makeSeparatorText(velocityModificatorLabel, "Velocity Modificator"),
                                     juce::dontSendNotification);

    auto addVerticalSlider = [&](juce::Slider& slider,
                                 std::unique_ptr<ShadowComponent>& shadow,
                                 int offsetIndex)
    {
        const int offsetX = offsetIndex * (voiceSliderWidth + voiceSpacing);
        juce::Rectangle<int> sliderBounds(voiceColumnsStartX + offsetX, verticalArea.getY(), voiceSliderWidth, sliderHeight);

        if (shadow)
        {
            shadow->setBounds(sliderBounds.expanded(4, 0));
            slider.setBounds(shadow->getShadowArea());
        }
        else
        {
            slider.setBounds(sliderBounds);
        }

        slider.setComponentID("VerticalSlider");
        slider.setSliderStyle(juce::Slider::LinearVertical);
        slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    };

    addVerticalSlider(voice2Slider, voice2Shadow, 0);
    addVerticalSlider(voice3Slider, voice3Shadow, 1);
    addVerticalSlider(voice4Slider, voice4Shadow, 2);
    addVerticalSlider(voice5Slider, voice5Shadow, 3);

    //--------------------------------------------------------------------------
    // Row 3: Velocity modifiers (rotary + combo, below each voice slider)
    //--------------------------------------------------------------------------
    const int velocityRotaryWidth = juce::jlimit(
        54, 76, juce::jmin(68, velocityModsArea.getWidth() / 4 - 8));

    auto placeVelocityRotary = [&](std::unique_ptr<CustomRotaryWithCombo>& rotary, int offsetIndex)
    {
        if (!rotary)
            return;

        const int centerX = voiceColumnsStartX + offsetIndex * (voiceSliderWidth + voiceSpacing)
                            + voiceSliderWidth / 2;
        rotary->setBounds(centerX - velocityRotaryWidth / 2,
                          velocityModsArea.getY() - velocityTopCompensation,
                          velocityRotaryWidth,
                          velocityModsArea.getHeight() + velocityTopCompensation);
    };

    placeVelocityRotary(voice2VelocityMod, 0);
    placeVelocityRotary(voice3VelocityMod, 1);
    placeVelocityRotary(voice4VelocityMod, 2);
    placeVelocityRotary(voice5VelocityMod, 3);

}

void Harmonizer::paint(juce::Graphics& g)
{
    const auto backgroundArea = getLocalBounds().toFloat().reduced((float) UiMetrics::kModuleOuterMargin);

    juce::Path backgroundPath;
    backgroundPath.addRoundedRectangle(backgroundArea, UiMetrics::kModuleCornerRadius);

    g.setColour(PluginColours::surface);
    g.fillPath(backgroundPath);
}
