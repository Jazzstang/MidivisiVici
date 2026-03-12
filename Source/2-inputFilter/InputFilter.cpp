//==============================================================================
// InputFilter.cpp
//
// MidivisiVici - UI Module: InputFilter
//
// This file is lane-aware (A..P). Each instance reads/writes a dedicated
// set of parameters suffixed with _<lane>.
//
// IMPORTANT:
// - No Attachments. We push values manually via setValueNotifyingHost().
// - We listen to APVTS changes (automation/snapshots/MIDI) and refresh the UI.
// - parameterChanged() can be called from non-message threads: do NOT touch UI.
//
// Routing toggles:
// - ParamIDs::Base::inputFilterConsume (bool) is lane-based.
// - ParamIDs::Base::inputFilterDirect  (bool) is lane-based.
// - ParamIDs::Base::inputFilterHold    (bool) is lane-based.
// - UI controls are placed on a dedicated routing row (label + tick box).
// - Routing behavior is handled elsewhere (PluginProcessor/router).
//==============================================================================

#include "InputFilter.h"

#include "../PluginParameters.h"
#include "../PluginColours.h"
#include "../DebugConfig.h"
#include "../UiMetrics.h"

namespace
{
    // Petit garde RAII pour eviter de laisser applyingFromState a true
    // en cas de retour anticipe futur dans refreshUIFromParameters().
    struct ScopedAtomicBoolFlag
    {
        explicit ScopedAtomicBoolFlag(std::atomic<bool>& f) : flag(f) { flag.store(true); }
        ~ScopedAtomicBoolFlag() { flag.store(false); }

        std::atomic<bool>& flag;
    };
}

juce::String InputFilter::laneParamID(const char* baseID) const
{
    return ParamIDs::lane(baseID, lane);
}

//==============================================================================
// Utility: add a component with shadow into the UI
//==============================================================================

template<typename Comp>
void InputFilter::addWithShadow(std::unique_ptr<ShadowComponent>& shadowPtr,
                                Comp& comp,
                                const juce::DropShadow& shadow)
{
    shadowPtr = std::make_unique<ShadowComponent>(shadow, -1.0f, true, true, true, true, 10);
    shadowPtr->addAndMakeVisible(comp);
    addAndMakeVisible(*shadowPtr);
}

//==============================================================================
// APVTS Listener registration
//==============================================================================

void InputFilter::registerParameterListeners()
{
    parameters.addParameterListener(idInputFilterEnable, this);

    // Routing
    parameters.addParameterListener(idInputFilterConsume, this);
    parameters.addParameterListener(idInputFilterDirect, this);
    parameters.addParameterListener(idInputFilterHold, this);
    parameters.addParameterListener(idInputFilterBlackNotes, this);
    parameters.addParameterListener(idGlobalBlackInputMode, this);

    parameters.addParameterListener(idNoteFilterToggle, this);
    parameters.addParameterListener(idNoteMin, this);
    parameters.addParameterListener(idNoteMax, this);

    parameters.addParameterListener(idVelocityFilterToggle, this);
    parameters.addParameterListener(idVelocityMin, this);
    parameters.addParameterListener(idVelocityMax, this);

    parameters.addParameterListener(idStepFilterToggle, this);
    parameters.addParameterListener(idStepFilterNumerator, this);
    parameters.addParameterListener(idStepFilterDenominator, this);

    parameters.addParameterListener(idVoiceLimitToggle, this);
    parameters.addParameterListener(idVoiceLimit, this);
    parameters.addParameterListener(idPriority, this);
}

void InputFilter::unregisterParameterListeners()
{
    parameters.removeParameterListener(idInputFilterEnable, this);

    // Routing
    parameters.removeParameterListener(idInputFilterConsume, this);
    parameters.removeParameterListener(idInputFilterDirect, this);
    parameters.removeParameterListener(idInputFilterHold, this);
    parameters.removeParameterListener(idInputFilterBlackNotes, this);
    parameters.removeParameterListener(idGlobalBlackInputMode, this);

    parameters.removeParameterListener(idNoteFilterToggle, this);
    parameters.removeParameterListener(idNoteMin, this);
    parameters.removeParameterListener(idNoteMax, this);

    parameters.removeParameterListener(idVelocityFilterToggle, this);
    parameters.removeParameterListener(idVelocityMin, this);
    parameters.removeParameterListener(idVelocityMax, this);

    parameters.removeParameterListener(idStepFilterToggle, this);
    parameters.removeParameterListener(idStepFilterNumerator, this);
    parameters.removeParameterListener(idStepFilterDenominator, this);

    parameters.removeParameterListener(idVoiceLimitToggle, this);
    parameters.removeParameterListener(idVoiceLimit, this);
    parameters.removeParameterListener(idPriority, this);
}

//==============================================================================
// APVTS Listener callback (NOT message thread)
//==============================================================================

void InputFilter::parameterChanged(const juce::String& parameterID, float /*newValue*/)
{
    // This may be called from audio thread. Do NOT touch UI here.
   #if LOGS_ENABLED && LOG_INPUTFILTER
    DBG_LOG("STATE", "INPUTFILTER", "PARAM CHANGED", "IF_UI", parameterID);
   #endif

    // AsyncUpdater coalesce deja les notifications, mais eviter un re-trigger
    // inutile reduit encore la pression message-thread sous forte automation.
    if (!isUpdatePending())
        triggerAsyncUpdate();
}

//==============================================================================
// AsyncUpdater callback (message thread)
//==============================================================================

void InputFilter::handleAsyncUpdate()
{
    refreshUIFromParameters();
}

//==============================================================================
// Public helper
//==============================================================================

void InputFilter::forceRefreshFromParameters()
{
    // Evite de surcharger la queue message-thread si plusieurs demandes
    // de refresh arrivent pendant une meme tranche d automation.
    if (!isUpdatePending())
        triggerAsyncUpdate();
}

//==============================================================================
// Centralized "global bypass" behavior
//==============================================================================

void InputFilter::applyGlobalUiBypass(bool bypassed)
{
    auto setEnabledIfChanged = [](juce::Component& c, bool shouldBeEnabled)
    {
        if (c.isEnabled() != shouldBeEnabled)
            c.setEnabled(shouldBeEnabled);
    };

    // Title should always remain clickable.
    if (inputFilterTitleButtonShadow) setEnabledIfChanged(*inputFilterTitleButtonShadow, true);
    setEnabledIfChanged(inputFilterTitleButton, true);

    // Disable shadow wrappers (they are the parent for toggles / combobox).
    if (noteFilterToggleShadow)      setEnabledIfChanged(*noteFilterToggleShadow, !bypassed);
    if (velocityFilterToggleShadow)  setEnabledIfChanged(*velocityFilterToggleShadow, !bypassed);
    if (stepFilterToggleShadow)      setEnabledIfChanged(*stepFilterToggleShadow, !bypassed);
    if (voiceLimitToggleShadow)      setEnabledIfChanged(*voiceLimitToggleShadow, !bypassed);
    if (priorityBoxShadow)           setEnabledIfChanged(*priorityBoxShadow, !bypassed);

    // Also disable children (belt and suspenders).
    setEnabledIfChanged(noteFilterToggle, !bypassed);
    setEnabledIfChanged(velocityFilterToggle, !bypassed);
    setEnabledIfChanged(stepFilterToggle, !bypassed);
    setEnabledIfChanged(voiceLimitToggle, !bypassed);
    setEnabledIfChanged(priorityBox, !bypassed);

    setEnabledIfChanged(directButton, !bypassed);
    setEnabledIfChanged(blackNotesButton, !bypassed);
    setEnabledIfChanged(holdButton, !bypassed);
    setEnabledIfChanged(consumeButton, !bypassed);
    setEnabledIfChanged(priorityLabel, !bypassed);
    setEnabledIfChanged(directLabel, !bypassed);
    setEnabledIfChanged(blackNotesLabel, !bypassed);
    setEnabledIfChanged(holdLabel, !bypassed);
    setEnabledIfChanged(consumeLabel, !bypassed);

    // Sliders are not wrapped.
    setEnabledIfChanged(noteSlider, !bypassed);
    setEnabledIfChanged(velocitySlider, !bypassed);
    setEnabledIfChanged(stepSlider, !bypassed);
    setEnabledIfChanged(voiceSlider, !bypassed);

    // Visual feedback for the whole module.
    const float targetAlpha = bypassed ? 0.55f : 1.0f;
    if (getAlpha() != targetAlpha)
        setAlpha(targetAlpha);
}

void InputFilter::setBoolParameterFromUi(const juce::String& paramId, bool enabled)
{
    if (auto* p = dynamic_cast<juce::AudioParameterBool*>(parameters.getParameter(paramId)))
    {
        const float target = enabled ? 1.0f : 0.0f;
        const auto* raw = parameters.getRawParameterValue(paramId);
        const float current = (raw != nullptr ? raw->load() : p->get());

        if ((enabled && current > 0.5f) || (!enabled && current <= 0.5f))
            return;

        p->beginChangeGesture();
        p->setValueNotifyingHost(target);
        p->endChangeGesture();
    }
}

void InputFilter::setIntParameterFromUi(const juce::String& paramId, int value)
{
    if (auto* p = dynamic_cast<juce::AudioParameterInt*>(parameters.getParameter(paramId)))
    {
        const auto* raw = parameters.getRawParameterValue(paramId);
        // APVTS raw pour AudioParameterInt est deja dans l espace "reel"
        // (ex: 0..127), pas normalise 0..1.
        const int currentReal = juce::roundToInt(raw != nullptr ? raw->load() : (float) p->get());
        const auto range = p->getNormalisableRange();
        const int minValue = juce::roundToInt(range.start);
        const int maxValue = juce::roundToInt(range.end);
        const int nextReal = juce::jlimit(minValue, maxValue, juce::roundToInt((float) value));

        if (currentReal == nextReal)
            return;

        p->beginChangeGesture();
        p->setValueNotifyingHost(p->convertTo0to1((float) nextReal));
        p->endChangeGesture();
    }
}

void InputFilter::setChoiceParameterFromUi(const juce::String& paramId, int index)
{
    if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(parameters.getParameter(paramId)))
    {
        const int maxIndex = juce::jmax(0, (int) p->choices.size() - 1);
        const auto* raw = parameters.getRawParameterValue(paramId);
        const int currentIndex = (raw != nullptr)
            ? juce::jlimit(0, maxIndex, juce::roundToInt(raw->load()))
            : juce::jlimit(0, maxIndex, p->getIndex());
        const int nextIndex = juce::jlimit(0, maxIndex, index);

        if (currentIndex == nextIndex)
            return;

        p->beginChangeGesture();
        p->setValueNotifyingHost(p->convertTo0to1((float) nextIndex));
        p->endChangeGesture();
    }
}

//==============================================================================
// Enabled/disabled visuals and interactivity (per-section only)
//==============================================================================

void InputFilter::updateFilterEnabledState()
{
    auto setEnabledIfChanged = [](juce::Component& c, bool shouldBeEnabled)
    {
        if (c.isEnabled() != shouldBeEnabled)
            c.setEnabled(shouldBeEnabled);
    };

    const auto* pGlobal = parameters.getRawParameterValue(idInputFilterEnable);
    const bool globalEnabled = (pGlobal != nullptr && pGlobal->load() > 0.5f);

    if (!globalEnabled)
    {
        // Global bypass already applied in applyGlobalUiBypass(true).
        return;
    }

    const auto* pNoteT  = parameters.getRawParameterValue(idNoteFilterToggle);
    const auto* pVeloT  = parameters.getRawParameterValue(idVelocityFilterToggle);
    const auto* pStepT  = parameters.getRawParameterValue(idStepFilterToggle);
    const auto* pVoiceT = parameters.getRawParameterValue(idVoiceLimitToggle);

    const bool noteOn  = (pNoteT  != nullptr && pNoteT->load()  > 0.5f);
    const bool veloOn  = (pVeloT  != nullptr && pVeloT->load()  > 0.5f);
    const bool stepOn  = (pStepT  != nullptr && pStepT->load()  > 0.5f);
    const bool voiceOn = (pVoiceT != nullptr && pVoiceT->load() > 0.5f);

    // Sliders follow their section toggles.
    setEnabledIfChanged(noteSlider, noteOn);
    setEnabledIfChanged(velocitySlider, veloOn);
    setEnabledIfChanged(stepSlider, stepOn);
    setEnabledIfChanged(voiceSlider, voiceOn);

    // Priority exists only to configure Voice Limit behavior.
    const bool priorityEnabled = voiceOn;

    if (priorityBoxShadow && priorityBoxShadow->isEnabled() != priorityEnabled)
        priorityBoxShadow->setEnabled(priorityEnabled);
    setEnabledIfChanged(priorityBox, priorityEnabled);
    setEnabledIfChanged(priorityLabel, priorityEnabled);

    // Routing row policy:
    // - Direct/Hold stay available while module is enabled.
    // - Black Notes is only editable while global B mode is ON.
    // - Consume is hard-disabled while Step Filter is active.
    const bool routingEnabled = true;
    const auto* pGlobalBlackMode = parameters.getRawParameterValue(idGlobalBlackInputMode);
    const bool blackModeOn = (pGlobalBlackMode != nullptr && pGlobalBlackMode->load() > 0.5f);
    const bool consumeEnabled = !stepOn;

    setEnabledIfChanged(directButton, routingEnabled);
    setEnabledIfChanged(blackNotesButton, blackModeOn);
    setEnabledIfChanged(holdButton, routingEnabled);
    setEnabledIfChanged(directLabel, routingEnabled);
    setEnabledIfChanged(blackNotesLabel, blackModeOn);
    setEnabledIfChanged(holdLabel, routingEnabled);
    setEnabledIfChanged(consumeButton, consumeEnabled);
    setEnabledIfChanged(consumeLabel, consumeEnabled);
}

//==============================================================================
// Pull current parameter values and apply them to UI controls
//==============================================================================

void InputFilter::refreshUIFromParameters()
{
    ScopedAtomicBoolFlag stateGuard { applyingFromState };

    // UI write de-duplication:
    // avoid repaint/listener churn when APVTS pushes unchanged values
    // (common under dense automation and multi-lane updates).
    const auto setToggleIfChanged = [](juce::ToggleButton& b, bool state)
    {
        if (b.getToggleState() != state)
            b.setToggleState(state, juce::dontSendNotification);
    };

    const auto setSliderMinIfChanged = [](juce::Slider& s, int value)
    {
        if ((int) s.getMinValue() != value)
            s.setMinValue(value, juce::dontSendNotification);
    };

    const auto setSliderMaxIfChanged = [](juce::Slider& s, int value)
    {
        if ((int) s.getMaxValue() != value)
            s.setMaxValue(value, juce::dontSendNotification);
    };

    const auto setSliderValueIfChanged = [](juce::Slider& s, int value)
    {
        if ((int) juce::roundToInt(s.getValue()) != value)
            s.setValue(value, juce::dontSendNotification);
    };

    // --- Global enable ---
    const auto* pGlobal = parameters.getRawParameterValue(idInputFilterEnable);
    const bool globalEnabled = (pGlobal != nullptr && pGlobal->load() > 0.5f);

    setToggleIfChanged(inputFilterTitleButton, globalEnabled);

    // Apply bypass FIRST (blocks interaction + visuals)
    applyGlobalUiBypass(!globalEnabled);

    // --- Step toggle (needed early for consume-lock policy) ---
    const auto* pStepT   = parameters.getRawParameterValue(idStepFilterToggle);
    const bool stepOn = (pStepT != nullptr && pStepT->load() > 0.5f);

    // --- Routing toggles ---
    const auto* pConsume = parameters.getRawParameterValue(idInputFilterConsume);
    const auto* pDirect = parameters.getRawParameterValue(idInputFilterDirect);
    const auto* pHold = parameters.getRawParameterValue(idInputFilterHold);
    const auto* pBlackNotes = parameters.getRawParameterValue(idInputFilterBlackNotes);
    const bool consumeOnRaw = (pConsume != nullptr && pConsume->load() > 0.5f);
    const bool directOn = (pDirect != nullptr && pDirect->load() > 0.5f);
    const bool holdOn = (pHold != nullptr && pHold->load() > 0.5f);
    const bool blackNotesOn = (pBlackNotes != nullptr && pBlackNotes->load() > 0.5f);

    // StepFilter->Consume lock:
    // - Tant que Step est ON, la source de verite force consume a OFF.
    // - Le processor applique deja consumeEffectif = consume && !stepOn;
    //   ici on maintient aussi la coherence de la valeur persistante APVTS.
    if (stepOn && consumeOnRaw)
        setBoolParameterFromUi(idInputFilterConsume, false);

    const bool consumeOn = stepOn ? false : consumeOnRaw;

    setToggleIfChanged(directButton, directOn);
    setToggleIfChanged(blackNotesButton, blackNotesOn);
    setToggleIfChanged(holdButton, holdOn);
    setToggleIfChanged(consumeButton, consumeOn);

    // --- Note filter toggle + range ---
    const auto* pNoteT   = parameters.getRawParameterValue(idNoteFilterToggle);
    const auto* pNoteMin = parameters.getRawParameterValue(idNoteMin);
    const auto* pNoteMax = parameters.getRawParameterValue(idNoteMax);

    const bool noteOn = (pNoteT != nullptr && pNoteT->load() > 0.5f);

    const int noteMinVal = juce::jlimit(0, 127, (int)(pNoteMin != nullptr ? pNoteMin->load() : 0.0f));
    const int noteMaxVal = juce::jlimit(0, 127, (int)(pNoteMax != nullptr ? pNoteMax->load() : 127.0f));

    setToggleIfChanged(noteFilterToggle, noteOn);
    setSliderMinIfChanged(noteSlider.getSlider(), noteMinVal);
    setSliderMaxIfChanged(noteSlider.getSlider(), noteMaxVal);

    // --- Velocity filter toggle + range ---
    const auto* pVeloT   = parameters.getRawParameterValue(idVelocityFilterToggle);
    const auto* pVeloMin = parameters.getRawParameterValue(idVelocityMin);
    const auto* pVeloMax = parameters.getRawParameterValue(idVelocityMax);

    const bool veloOn = (pVeloT != nullptr && pVeloT->load() > 0.5f);

    const int veloMinVal = juce::jlimit(0, 127, (int)(pVeloMin != nullptr ? pVeloMin->load() : 0.0f));
    const int veloMaxVal = juce::jlimit(0, 127, (int)(pVeloMax != nullptr ? pVeloMax->load() : 127.0f));

    setToggleIfChanged(velocityFilterToggle, veloOn);
    setSliderMinIfChanged(velocitySlider.getSlider(), veloMinVal);
    setSliderMaxIfChanged(velocitySlider.getSlider(), veloMaxVal);

    // --- Step filter toggle + ratio (numerator/denominator) ---
    const auto* pStepNum = parameters.getRawParameterValue(idStepFilterNumerator);
    const auto* pStepDen = parameters.getRawParameterValue(idStepFilterDenominator);

    const int stepNumVal = juce::jlimit(1, 16, (int)(pStepNum != nullptr ? pStepNum->load() : 1.0f));
    const int stepDenVal = juce::jlimit(1, 16, (int)(pStepDen != nullptr ? pStepDen->load() : 1.0f));

    setToggleIfChanged(stepFilterToggle, stepOn);
    setSliderMinIfChanged(stepSlider.getSlider(), stepNumVal);
    setSliderMaxIfChanged(stepSlider.getSlider(), stepDenVal);

    // --- Voice limit toggle + value ---
    const auto* pVoiceT = parameters.getRawParameterValue(idVoiceLimitToggle);
    const auto* pVoice  = parameters.getRawParameterValue(idVoiceLimit);

    const bool voiceOn = (pVoiceT != nullptr && pVoiceT->load() > 0.5f);
    const int voiceVal = juce::jlimit(1, 16, (int)(pVoice != nullptr ? pVoice->load() : 16.0f));

    setToggleIfChanged(voiceLimitToggle, voiceOn);
    setSliderValueIfChanged(voiceSlider.getSlider(), voiceVal);

    // --- Priority choice ---
    const auto* pPriorityRaw = parameters.getRawParameterValue(idPriority);
    const int priorityIndex = juce::jlimit(0, 2, (int) (pPriorityRaw != nullptr ? pPriorityRaw->load() : 0.0f));
    const int selectedId = priorityIndex + 1;
    if (priorityBox.getSelectedId() != selectedId)
        priorityBox.setSelectedId(selectedId, juce::dontSendNotification);

    // Per-section gating (sliders, priority, direct/consume)
    updateFilterEnabledState();

    // Cache last-known values (optional, used for log gating consistency)
    lastGlobalEnableState    = globalEnabled;
    lastDirectState          = directOn;
    lastBlackNotesState      = blackNotesOn;
    lastHoldState            = holdOn;
    lastConsumeState         = consumeOn;

    lastNoteFilterState      = noteOn;
    lastNoteMin              = noteMinVal;
    lastNoteMax              = noteMaxVal;

    lastVelocityFilterState  = veloOn;
    lastVelocityMin          = veloMinVal;
    lastVelocityMax          = veloMaxVal;

    lastStepFilterState      = stepOn;
    lastStepNumerator        = stepNumVal;
    lastStepDenominator      = stepDenVal;

    lastVoiceLimitState      = voiceOn;
    lastVoiceLimit           = voiceVal;

    lastPriorityIndex        = juce::jlimit(0, 2, selectedId - 1);

    // No unconditional repaint here:
    // child controls already repaint on real value/state changes.
    // Avoiding forced repaints reduces message-thread bursts under automation.
}

//==============================================================================
// Constructor / Destructor
//==============================================================================

InputFilter::InputFilter(juce::AudioProcessorValueTreeState& state, Lane laneIn)
    : parameters(state),
      lane(laneIn),

      // Prebuild lane parameter IDs once (UI thread, ctor)
      idInputFilterEnable(laneParamID(ParamIDs::inputFilterEnable)),

      // Routing
      idInputFilterConsume(laneParamID(ParamIDs::inputFilterConsume)),
      idInputFilterDirect(laneParamID(ParamIDs::inputFilterDirect)),
      idInputFilterHold(laneParamID(ParamIDs::inputFilterHold)),
      idInputFilterBlackNotes(laneParamID(ParamIDs::inputFilterBlackNotes)),
      idGlobalBlackInputMode(ParamIDs::blackInputModeToggle),

      idNoteFilterToggle(laneParamID(ParamIDs::noteFilterToggle)),
      idNoteMin(laneParamID(ParamIDs::noteMin)),
      idNoteMax(laneParamID(ParamIDs::noteMax)),

      idVelocityFilterToggle(laneParamID(ParamIDs::velocityFilterToggle)),
      idVelocityMin(laneParamID(ParamIDs::velocityMin)),
      idVelocityMax(laneParamID(ParamIDs::velocityMax)),

      idStepFilterToggle(laneParamID(ParamIDs::stepFilterToggle)),
      idStepFilterNumerator(laneParamID(ParamIDs::stepFilterNumerator)),
      idStepFilterDenominator(laneParamID(ParamIDs::stepFilterDenominator)),

      idVoiceLimitToggle(laneParamID(ParamIDs::voiceLimitToggle)),
      idVoiceLimit(laneParamID(ParamIDs::voiceLimit)),
      idPriority(laneParamID(ParamIDs::priority))
{
    // Register listeners FIRST so snapshot recalls will refresh this UI.
    registerParameterListeners();

    auto setupInlineLabel = [](juce::Label& label, const juce::String& text)
    {
        label.setText(text, juce::dontSendNotification);
        label.setJustificationType(juce::Justification::centredLeft);
        label.setColour(juce::Label::textColourId, PluginColours::onSurface);
        label.setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        label.setInterceptsMouseClicks(false, false);
        label.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
    };

    setupInlineLabel(priorityLabel, "priority");
    setupInlineLabel(directLabel, "direct");
    setupInlineLabel(blackNotesLabel, "black notes");
    setupInlineLabel(holdLabel, "hold");
    setupInlineLabel(consumeLabel, "consume");
    addAndMakeVisible(priorityLabel);
    addAndMakeVisible(directLabel);
    addAndMakeVisible(blackNotesLabel);
    addAndMakeVisible(holdLabel);
    addAndMakeVisible(consumeLabel);

    //--------------------------------------------------------------------------
    // Header (global enable)
    //--------------------------------------------------------------------------
    inputFilterTitleButton.setClickingTogglesState(true);

    inputFilterTitleButton.onClick = [this]
    {
        if (applyingFromState.load())
            return;

        const bool newState = inputFilterTitleButton.getToggleState();
        setBoolParameterFromUi(idInputFilterEnable, newState);
        applyGlobalUiBypass(!newState);
        updateFilterEnabledState();

       #if LOGS_ENABLED && LOG_INPUTFILTER
        if (newState != lastGlobalEnableState)
        {
            DBG_LOG("STATE", "INPUTFILTER", "GLOBAL ENABLE", "IF000", newState ? "ON" : "OFF");
            lastGlobalEnableState = newState;
        }
       #endif
    };

    addWithShadow(inputFilterTitleButtonShadow, inputFilterTitleButton,
                  juce::DropShadow(juce::Colours::black, 0, { 0, 0 }));

    //--------------------------------------------------------------------------
    // NOTE slider
    //--------------------------------------------------------------------------
    noteSlider.setTitle("Note Range");
    noteSlider.setSliderStyle(juce::Slider::TwoValueHorizontal);
    noteSlider.setRange(0, 127, 1);
    noteSlider.setDisplayMode(LabeledSlider::DisplayMode::Note);

    noteSlider.getSlider().onValueChange = [this]
    {
        if (applyingFromState.load())
            return;

        const int minVal = (int) noteSlider.getSlider().getMinValue();
        const int maxVal = (int) noteSlider.getSlider().getMaxValue();

        setIntParameterFromUi(idNoteMin, minVal);
        setIntParameterFromUi(idNoteMax, maxVal);
    };

    addAndMakeVisible(noteSlider);

    // NOTE toggle
    addWithShadow(noteFilterToggleShadow, noteFilterToggle,
                  juce::DropShadow(juce::Colours::black, 0, { 0, 0 }));

    noteFilterToggle.onClick = [this]
    {
        if (applyingFromState.load())
            return;

        const bool newState = noteFilterToggle.getToggleState();
        setBoolParameterFromUi(idNoteFilterToggle, newState);
        updateFilterEnabledState();
    };

    //--------------------------------------------------------------------------
    // VELOCITY slider
    //--------------------------------------------------------------------------
    velocitySlider.setTitle("Velocity Range");
    velocitySlider.setSliderStyle(juce::Slider::TwoValueHorizontal);
    velocitySlider.setRange(0, 127, 1);
    velocitySlider.setDisplayMode(LabeledSlider::DisplayMode::Range);

    velocitySlider.getSlider().onValueChange = [this]
    {
        if (applyingFromState.load())
            return;

        const int minVal = (int) velocitySlider.getSlider().getMinValue();
        const int maxVal = (int) velocitySlider.getSlider().getMaxValue();

        setIntParameterFromUi(idVelocityMin, minVal);
        setIntParameterFromUi(idVelocityMax, maxVal);
    };

    addAndMakeVisible(velocitySlider);

    // VELOCITY toggle
    addWithShadow(velocityFilterToggleShadow, velocityFilterToggle,
                  juce::DropShadow(juce::Colours::black, 0, { 0, 0 }));

    velocityFilterToggle.onClick = [this]
    {
        if (applyingFromState.load())
            return;

        const bool newState = velocityFilterToggle.getToggleState();
        setBoolParameterFromUi(idVelocityFilterToggle, newState);
        updateFilterEnabledState();
    };

    //--------------------------------------------------------------------------
    // VOICE slider
    //--------------------------------------------------------------------------
    voiceSlider.setTitle("Voice Limit");
    voiceSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    voiceSlider.setRange(1, 16, 1);
    voiceSlider.setDisplayMode(LabeledSlider::DisplayMode::Simple);

    voiceSlider.getSlider().onValueChange = [this]
    {
        if (applyingFromState.load())
            return;

        const int val = (int) voiceSlider.getSlider().getValue();
        setIntParameterFromUi(idVoiceLimit, val);
    };

    addAndMakeVisible(voiceSlider);

    // VOICE toggle
    addWithShadow(voiceLimitToggleShadow, voiceLimitToggle,
                  juce::DropShadow(juce::Colours::black, 0, { 0, 0 }));

    voiceLimitToggle.onClick = [this]
    {
        if (applyingFromState.load())
            return;

        const bool enabled = voiceLimitToggle.getToggleState();
        setBoolParameterFromUi(idVoiceLimitToggle, enabled);
        updateFilterEnabledState();
    };

    //--------------------------------------------------------------------------
    // PRIORITY (choice)
    //--------------------------------------------------------------------------
    priorityBox.addItemList({ "Last", "Lowest", "Highest" }, 1);
    priorityBox.onChange = [this]
    {
        if (applyingFromState.load())
            return;

        const int sel   = priorityBox.getSelectedId(); // 1..3
        const int index = sel - 1;
        setChoiceParameterFromUi(idPriority, index);
    };

    addWithShadow(priorityBoxShadow, priorityBox,
                  juce::DropShadow(juce::Colours::black, 0, { 0, 0 }));

    //--------------------------------------------------------------------------
    // STEP slider (numerator/denominator)
    //--------------------------------------------------------------------------
    stepSlider.setTitle("Step Filter");
    stepSlider.setSliderStyle(juce::Slider::TwoValueHorizontal);
    stepSlider.setRange(1, 16, 1);
    stepSlider.setDisplayMode(LabeledSlider::DisplayMode::Ratio);

    stepSlider.getSlider().onValueChange = [this]
    {
        if (applyingFromState.load())
            return;

        const int num = (int) stepSlider.getSlider().getMinValue();
        const int den = (int) stepSlider.getSlider().getMaxValue();

        setIntParameterFromUi(idStepFilterNumerator, num);
        setIntParameterFromUi(idStepFilterDenominator, den);
    };

    addAndMakeVisible(stepSlider);

    // STEP toggle
    addWithShadow(stepFilterToggleShadow, stepFilterToggle,
                  juce::DropShadow(juce::Colours::black, 0, { 0, 0 }));

    stepFilterToggle.onClick = [this]
    {
        if (applyingFromState.load())
            return;

        const bool enabled = stepFilterToggle.getToggleState();
        setBoolParameterFromUi(idStepFilterToggle, enabled);

        // Policy: Step ON forces consume OFF immediately.
        if (enabled)
        {
            consumeButton.setToggleState(false, juce::dontSendNotification);
            setBoolParameterFromUi(idInputFilterConsume, false);
        }

        // Le gating final (dont consume lock) reste centralise dans
        // updateFilterEnabledState() pour garder une seule source de verite.
        updateFilterEnabledState();
    };

    //--------------------------------------------------------------------------
    // ROUTING (Direct + Hold + Consume)
    //--------------------------------------------------------------------------
    directButton.setButtonText("");
    directButton.setComponentID("IF_Tick_Direct");
    directButton.setClickingTogglesState(true);
    directButton.setToggleState(false, juce::dontSendNotification);
    addAndMakeVisible(directButton);
    directButton.onClick = [this]
    {
        if (applyingFromState.load())
            return;

        setBoolParameterFromUi(idInputFilterDirect, directButton.getToggleState());
    };

    consumeButton.setButtonText("");
    consumeButton.setComponentID("IF_Tick_Consume");
    consumeButton.setClickingTogglesState(true);
    addAndMakeVisible(consumeButton);
    consumeButton.onClick = [this]
    {
        if (applyingFromState.load())
            return;

        const auto* pStepToggle = parameters.getRawParameterValue(idStepFilterToggle);
        const bool stepOn = (pStepToggle != nullptr && pStepToggle->load() > 0.5f);

        // Step ON => consume locked OFF.
        if (stepOn)
        {
            consumeButton.setToggleState(false, juce::dontSendNotification);
            setBoolParameterFromUi(idInputFilterConsume, false);
            updateFilterEnabledState();
            return;
        }

        setBoolParameterFromUi(idInputFilterConsume, consumeButton.getToggleState());

       #if LOGS_ENABLED && LOG_INPUTFILTER
        const bool enabled = consumeButton.getToggleState();
        if (enabled != lastConsumeState)
        {
            DBG_LOG("STATE", "INPUTFILTER", "CONSUME", "IF010", enabled ? "ON" : "OFF");
            lastConsumeState = enabled;
        }
       #endif
    };

    holdButton.setButtonText("");
    holdButton.setComponentID("IF_Tick_Hold");
    holdButton.setClickingTogglesState(true);
    addAndMakeVisible(holdButton);
    holdButton.onClick = [this]
    {
        if (applyingFromState.load())
            return;

        setBoolParameterFromUi(idInputFilterHold, holdButton.getToggleState());
    };

    blackNotesButton.setButtonText("");
    blackNotesButton.setComponentID("IF_Tick_BlackNotes");
    blackNotesButton.setClickingTogglesState(true);
    addAndMakeVisible(blackNotesButton);
    blackNotesButton.onClick = [this]
    {
        if (applyingFromState.load())
            return;

        setBoolParameterFromUi(idInputFilterBlackNotes, blackNotesButton.getToggleState());
    };

    // First sync
    refreshUIFromParameters();
}

InputFilter::~InputFilter()
{
    cancelPendingUpdate();
    unregisterParameterListeners();
}

//==============================================================================
// Layout / paint (lane does not affect layout)
//==============================================================================

void InputFilter::resized()
{
    constexpr int contentInset = UiMetrics::kModuleOuterMargin + UiMetrics::kModuleInnerMargin;
    auto area = getLocalBounds().reduced(contentInset);

    {
        auto titleArea = area.removeFromTop(24);
        if (inputFilterTitleButtonShadow)
        {
            inputFilterTitleButtonShadow->setBounds(titleArea.expanded(10));
            inputFilterTitleButton.setBounds(inputFilterTitleButtonShadow->getShadowArea());
        }
    }

    area.removeFromTop(14);

    auto slidersArea = area;
    constexpr int fullRowH = 40;
    constexpr int halfRowH = 20;
    constexpr int rowSideMargin = UiMetrics::kModuleInnerMargin;
    constexpr int rowGapCount = 7; // top + between 6 rows + bottom
    constexpr int contentRowsHeight = (4 * fullRowH) + (2 * halfRowH);
    const int spacing = juce::jmax(0, (slidersArea.getHeight() - contentRowsHeight) / rowGapCount);

    auto nextRow = [&](int rowHeight)
    {
        slidersArea.removeFromTop(spacing);
        return slidersArea.removeFromTop(rowHeight).reduced(rowSideMargin, 0);
    };

    auto placeFilterRow = [&](auto& toggleShadow, auto& toggle, auto& slider)
    {
        auto rowArea = nextRow(fullRowH);
        auto toggleArea = rowArea.removeFromLeft(48);

        if (toggleShadow)
        {
            toggleShadow->setBounds(toggleArea.expanded(10));
            toggle.setBounds(toggleShadow->getShadowArea());
        }

        rowArea.removeFromLeft(8);
        slider.setBounds(rowArea);
    };

    // Row 1: Note
    placeFilterRow(noteFilterToggleShadow, noteFilterToggle, noteSlider);

    // Row 2: Velocity
    placeFilterRow(velocityFilterToggleShadow, velocityFilterToggle, velocitySlider);

    // Row 3: Voice Limit (full width)
    placeFilterRow(voiceLimitToggleShadow, voiceLimitToggle, voiceSlider);

    // Row 4: Priority (compact row, label + combo)
    {
        auto rowArea = nextRow(halfRowH);
        auto labelArea = rowArea.removeFromLeft(60);
        priorityLabel.setBounds(labelArea);
        rowArea.removeFromLeft(8);

        auto comboArea = rowArea.withSizeKeepingCentre(rowArea.getWidth(), halfRowH);
        if (priorityBoxShadow)
        {
            priorityBoxShadow->setBounds(comboArea.expanded(10));
            priorityBox.setBounds(priorityBoxShadow->getShadowArea());
        }
        else
        {
            priorityBox.setBounds(comboArea);
        }
    }

    // Row 5: Step Filter (full width)
    placeFilterRow(stepFilterToggleShadow, stepFilterToggle, stepSlider);

    // Row 6: Routing (compact row, direct + black notes + hold + consume)
    {
        auto rowArea = nextRow(halfRowH);
        constexpr int groupGap = 12;
        const int groupWidth = juce::jmax(0, (rowArea.getWidth() - (3 * groupGap)) / 4);
        auto directArea = rowArea.removeFromLeft(groupWidth);
        rowArea.removeFromLeft(groupGap);
        auto blackNotesArea = rowArea.removeFromLeft(groupWidth);
        rowArea.removeFromLeft(groupGap);
        auto holdArea = rowArea.removeFromLeft(groupWidth);
        rowArea.removeFromLeft(groupGap);
        auto consumeArea = rowArea;

        auto placeTickRow = [](juce::Rectangle<int> groupArea, juce::ToggleButton& button, juce::Label& label)
        {
            auto tickArea = groupArea.removeFromLeft(18).withSizeKeepingCentre(18, 18);
            button.setBounds(tickArea);
            groupArea.removeFromLeft(6);
            label.setBounds(groupArea);
        };

        placeTickRow(directArea, directButton, directLabel);
        placeTickRow(blackNotesArea, blackNotesButton, blackNotesLabel);
        placeTickRow(holdArea, holdButton, holdLabel);
        placeTickRow(consumeArea, consumeButton, consumeLabel);
    }

    slidersArea.removeFromTop(spacing);
}

void InputFilter::paint(juce::Graphics& g)
{
    const auto backgroundArea = getLocalBounds().toFloat().reduced((float) UiMetrics::kModuleOuterMargin);

    juce::Path backgroundPath;
    backgroundPath.addRoundedRectangle(backgroundArea, UiMetrics::kModuleCornerRadius);

    g.setColour(PluginColours::surface);
    g.fillPath(backgroundPath);
}
