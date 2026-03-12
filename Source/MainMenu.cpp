/*
==============================================================================
MainMenu.cpp
==============================================================================
MainMenu - Main UI surface (preset navigation + global controls).

Base secure goals:
  - Bank/Preset/Snapshot navigation immediately loads the corresponding state.
  - PresetManager emits onPendingChange(true) when VTS changes (except during
    restore/save guarded operations).
  - We translate that callback into UI updates via MessageManager::callAsync.
  - Avoid use-after-free: callbacks are cleared in the dtor.
  - Avoid unstable state: while SaveState::Saving, ignore pending=true events.

Clipboard (Copy/Paste/Delete):
  - Copy arms the clipboard, Paste applies clipboard source to current selection,
    Delete resets current selection to factory defaults.
  - These operations are "store-only" inside PresetManager (must NOT touch VTS),
    then we explicitly APPLY by loading the current snapshot to make runtime match.
  - We call presetManager.markSaved() after an operation that results in
    "runtime == stored snapshot" to clear the latch and set UI state to Idle.

IMPORTANT:
  - Comments/logs are ASCII only (project rule).
  - No Timer usage here (project rule: MainMenu does not use Timer).
  - All UI work is message-thread only.
==============================================================================
*/

#include "MainMenu.h"
#include "UiMetrics.h"
#include <cmath>

//==============================================================================
// 0) Local helpers
//------------------------------------------------------------------------------
namespace
{
    constexpr int kGlobalKeyLastIndex         = 12; // C..B + Off
    constexpr int kGlobalKeyOffIndex          = 12; // Off
    constexpr int kGlobalScaleLastIndex       = 9;  // Major..Chromatic
    constexpr int kGlobalScaleChromaticIndex  = 9;  // Chromatic
    // MainMenu module height follows the runtime component height so its
    // background matches the LFO module background height exactly.
    static inline int getMainMenuModuleHeightFor(const juce::Component& c) noexcept
    {
        return juce::jmax(1, c.getHeight() - (2 * UiMetrics::kModuleOuterMargin));
    }

    juce::String formatBpmFooterText(double bpm)
    {
        const double safe = (std::isfinite(bpm) && bpm > 0.0) ? bpm : 120.0;
        return juce::String(safe, 2) + " BPM";
    }

    juce::String formatMainMenuHeaderText(const juce::String& bankName,
                                          const juce::String& presetName,
                                          int snapshotNumberOneBased,
                                          const juce::String& snapshotName)
    {
        return bankName.toUpperCase()
             + " | "
             + presetName.toUpperCase()
             + " : "
             + juce::String(snapshotNumberOneBased)
             + " - "
             + snapshotName.toUpperCase();
    }

    juce::String sanitiseUserNameForCompare(const juce::String& input)
    {
        juce::String out;

        for (auto c : input)
            if (juce::CharacterFunctions::isLetterOrDigit(c) || c == ' ')
                out += c;

        return out.trim();
    }

    void showRenameDialogAsync(const juce::String& title,
                               const juce::String& currentName,
                               std::function<void(juce::String)> onSubmit)
    {
        auto* dialog = new juce::AlertWindow(title, "Enter a new name.", juce::AlertWindow::NoIcon);
        dialog->addTextEditor("name", currentName, "Name:");
        dialog->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        dialog->addButton("Rename", 1, juce::KeyPress(juce::KeyPress::returnKey));

        auto dialogSafe = juce::Component::SafePointer<juce::AlertWindow>(dialog);
        dialog->enterModalState(true,
                                juce::ModalCallbackFunction::create(
                                    [dialogSafe, onSubmit = std::move(onSubmit)](int result) mutable
                                    {
                                        if (result != 1 || dialogSafe == nullptr || !onSubmit)
                                            return;

                                        if (auto* textEditor = dialogSafe->getTextEditor("name"))
                                            onSubmit(textEditor->getText());
                                        else
                                            onSubmit({});
                                    }),
                                true);
    }
}

static juce::DropShadow makeShadow()
{
    return juce::DropShadow(juce::Colours::black, 0, { 0, 0 });
}

// Load a SVG from BinaryData (Projucer) and return a Drawable.
// Expected BinaryData symbols:
//   - BinaryData::copy_svg   / BinaryData::copy_svgSize
//   - BinaryData::paste_svg  / BinaryData::paste_svgSize
//   - BinaryData::delete_svg / BinaryData::delete_svgSize
static std::unique_ptr<juce::Drawable> loadSvgFromBinaryData(const void* data, int size)
{
    if (data == nullptr || size <= 0)
        return {};

    auto xml = juce::parseXML(juce::String::fromUTF8((const char*)data, size));
    if (xml == nullptr)
        return {};

    return juce::Drawable::createFromSVG(*xml);
}

// Apply a Drawable icon to a DrawableButton.
// Note: DrawableButton keeps references internally; keep the Drawable alive.
static void setButtonIcon(juce::DrawableButton& button, juce::Drawable* icon)
{
    jassert(icon != nullptr);
    button.setImages(icon, icon, icon);
}

//==============================================================================
// 1) Clipboard helpers
//------------------------------------------------------------------------------
MainMenu::SnapshotAddress MainMenu::getCurrentSelectionSafe() const
{
    SnapshotAddress a;
    a.bank     = presetManager.getCurrentBank();
    a.preset   = presetManager.getCurrentPreset();
    a.snapshot = presetManager.getCurrentSnapshot();
    return a;
}

void MainMenu::setClipboardActive(bool shouldBeActive)
{
    if (clipboard.active == shouldBeActive)
        return;

    clipboard.active = shouldBeActive;

    if (!clipboard.active)
        clipboard.source = {};

    updateClipboardButtonsUI(saveState == SaveState::Saving);
}

void MainMenu::updateClipboardButtonsUI(bool lockEdits)
{
    // Header actions are disabled while Saving (UI lock).
    learnButton.setEnabled(!lockEdits);
    copyButton.setEnabled(!lockEdits);
    deleteButton.setEnabled(!lockEdits);

    // Paste requires clipboard armed.
    const bool pasteEnabled = (!lockEdits && clipboard.active);
    pasteButton.setEnabled(pasteEnabled);

    // Visual: paste button stays visually "armed" while clipboard is active.
    // (pasteButton is a DrawableButton but it still supports toggle visuals
    //  if your LookAndFeel uses it; safe to set anyway).
    pasteButton.setToggleState(clipboard.active, juce::dontSendNotification);

    learnButton.repaint();
    copyButton.repaint();
    pasteButton.repaint();
    deleteButton.repaint();
    saveToggle.repaint();
}

void MainMenu::triggerCopyAction()
{
    if (copyButton.onClick)
        copyButton.onClick();
}

void MainMenu::triggerPasteAction()
{
    if (pasteButton.onClick)
        pasteButton.onClick();
}

void MainMenu::triggerDeleteAction()
{
    if (deleteButton.onClick)
        deleteButton.onClick();
}

void MainMenu::triggerSaveAction()
{
    if (saveToggle.onClick)
        saveToggle.onClick();
}

void MainMenu::triggerLearnAction()
{
    if (learnButton.onClick)
        learnButton.onClick();
}

void MainMenu::setDisplayedBpm(double bpm)
{
    const double safe = (std::isfinite(bpm) && bpm > 0.0) ? bpm : 120.0;
    const double rounded = std::round(safe * 100.0) / 100.0;

    if (std::abs(rounded - lastDisplayedBpm) <= 0.0001)
        return;

    lastDisplayedBpm = rounded;
    bpmFooterLabel.setText(formatBpmFooterText(rounded), juce::dontSendNotification);
}

void MainMenu::setDetectedChordFlow(const juce::String& inputChordName,
                                    const juce::String& outputChordName)
{
    const juce::String safeIn = inputChordName.isNotEmpty() ? inputChordName : "NoChord";
    const juce::String safeOut = outputChordName.isNotEmpty() ? outputChordName : "NoChord";
    const juce::String arrow = juce::String::fromUTF8("\xE2\x86\x92");
    const juce::String display = safeIn + " " + arrow + " " + safeOut;

    if (display == lastChordFlowDisplay)
        return;

    lastChordFlowDisplay = display;
    chordFooterLabel.setText(lastChordFlowDisplay, juce::dontSendNotification);
}

//==============================================================================
// 2) Ctor / dtor
//------------------------------------------------------------------------------
MainMenu::MainMenu(juce::AudioProcessorValueTreeState& vts, PresetManager& presetMgr)
    : parameters(vts)
    , presetManager(presetMgr)
{
    //==========================================================================
    // 2.1) LookAndFeel
    //==========================================================================
    mainMenuLF.setContextStyle(PluginLookAndFeel::ComponentStyle::MainMenu);
    setLookAndFeel(&mainMenuLF);

    //==========================================================================
    // 2.2) Header: Mute (VTS -> button, manual host notification)
    //==========================================================================
    muteButton.setComponentID("MM_Toggle_Mute");
    muteButton.setLookAndFeel(&mainMenuLF);
    muteButton.setClickingTogglesState(true);

    // Init from VTS.
    if (auto* p = parameters.getRawParameterValue(ParamIDs::inputMuteToggle))
    {
        const bool muteState = (p->load() > 0.5f);
        muteButton.setToggleState(muteState, juce::dontSendNotification);
        lastMuteState = muteState;
    }

    muteButton.onClick = [this]
    {
        if (saveState == SaveState::Saving)
            return;

        const bool newState = muteButton.getToggleState();

        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(parameters.getParameter(ParamIDs::inputMuteToggle)))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost(newState ? 1.0f : 0.0f);
            p->endChangeGesture();
        }

       #if LOGS_ENABLED
        if (newState != lastMuteState)
        {
            DBG_LOG("STATE", "MAINMENU", "MUTE", "#000#", newState ? "ON" : "OFF");
            lastMuteState = newState;
        }
       #endif
    };

    addWithShadow(muteButtonShadow, muteButton, makeShadow());

    //==========================================================================
    // 2.3) Header: MIDI channel selector
    //==========================================================================
    midiChannelLabel.setText("MIDI\nChannel", juce::dontSendNotification);
    midiChannelLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(midiChannelLabel);

    for (int i = 1; i <= 16; ++i)
        channelModeSelector.addItem(juce::String(i), i);

    if (auto* p = dynamic_cast<juce::AudioParameterInt*>(parameters.getParameter(ParamIDs::channelFilter)))
    {
        const int chan = p->get();
        channelModeSelector.setSelectedId(chan, juce::dontSendNotification);
        lastChannel = chan;
    }

    channelModeSelector.onChange = [this]
    {
        if (saveState == SaveState::Saving)
            return;

        const int sel = channelModeSelector.getSelectedId();
        if (sel <= 0)
            return;

        if (auto* p = dynamic_cast<juce::AudioParameterInt*>(parameters.getParameter(ParamIDs::channelFilter)))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost(p->convertTo0to1(sel));
            p->endChangeGesture();

           #if LOGS_ENABLED
            if (sel != lastChannel)
            {
                DBG_LOG("STATE", "INPUTFILTER", "CHANNEL FILTER", "#002#",
                        "Channel " + juce::String(sel));
                lastChannel = sel;
            }
           #endif
        }
    };

    addWithShadow(channelModeSelectorShadow, channelModeSelector, makeShadow());

    // Header simplification:
    // - Mute and Channel controls are kept internally but hidden from this row.
    if (muteButtonShadow)          muteButtonShadow->setVisible(false);
    if (channelModeSelectorShadow) channelModeSelectorShadow->setVisible(false);
    muteButton.setVisible(false);
    channelModeSelector.setVisible(false);
    midiChannelLabel.setVisible(false);

    // Top title: BANK | PRESET : SNAP - SNAPNAME (single composite pill).
    snapshotNameButton.setName("MM_HeaderComposite_SnapshotName");
    snapshotNameButton.setComponentID("MM_Label_SnapshotName");
    snapshotNameButton.setText("BANK 1 | PRESET 1 : 1 - SNAPSHOT 1", juce::dontSendNotification);
    snapshotNameButton.setJustificationType(juce::Justification::centred);
    snapshotNameButton.setEditable(false, false, false);
    snapshotNameButton.setWantsKeyboardFocus(false);
    snapshotNameButton.setColour(juce::Label::backgroundColourId, PluginColours::onBackground);
    snapshotNameButton.setColour(juce::Label::outlineColourId, PluginColours::onBackground);
    snapshotNameButton.setColour(juce::Label::textColourId, PluginColours::background);
    snapshotNameButton.getProperties().set("skipTextNormalise", true);
    snapshotNameButton.getProperties().set("useExplicitTextColour", true);
    snapshotNameButton.setTooltip("Bank | Preset | Snapshot (right click to rename)");
    snapshotNameButton.onRightClick = [this](const juce::MouseEvent&)
    {
        if (saveState == SaveState::Saving)
            return;

        juce::PopupMenu menu;
        menu.addItem(1, "Rename Bank...");
        menu.addItem(2, "Rename Preset...");
        menu.addItem(3, "Rename Snapshot...");
        menu.setLookAndFeel(&getLookAndFeel());

        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&snapshotNameButton),
                           [sp = juce::Component::SafePointer(this)](int result)
                           {
                               if (sp == nullptr)
                                   return;

                               if (result == 1)
                                   sp->requestRenameCurrentBank();
                               else if (result == 2)
                                   sp->requestRenameCurrentPreset();
                               else if (result == 3)
                                   sp->requestRenameCurrentSnapshot();
                           });
    };
    addAndMakeVisible(snapshotNameButton);

    // Footer modules: chord detector placeholder + BPM + theme mode (W/B).
    chordFooterLabel.setName("MM_ChordTitle");
    chordFooterLabel.setComponentID("MM_Label_ChordTitle");
    chordFooterLabel.setText("NOCHORD \xE2\x86\x92 NOCHORD", juce::dontSendNotification);
    chordFooterLabel.setJustificationType(juce::Justification::centred);
    chordFooterLabel.setEditable(false, false, false);
    chordFooterLabel.setWantsKeyboardFocus(false);
    chordFooterLabel.setColour(juce::Label::backgroundColourId, PluginColours::onBackground);
    chordFooterLabel.setColour(juce::Label::outlineColourId, PluginColours::onBackground);
    chordFooterLabel.setColour(juce::Label::textColourId, PluginColours::background);
    chordFooterLabel.getProperties().set("skipTextNormalise", true);
    chordFooterLabel.getProperties().set("useExplicitTextColour", true);
    chordFooterLabel.setTooltip("Input chord -> output chord");
    addAndMakeVisible(chordFooterLabel);
    lastChordFlowDisplay = chordFooterLabel.getText();

    bpmFooterLabel.setName("MM_BpmTitle");
    bpmFooterLabel.setComponentID("MM_Label_BpmTitle");
    bpmFooterLabel.setText(formatBpmFooterText(lastDisplayedBpm), juce::dontSendNotification);
    bpmFooterLabel.setJustificationType(juce::Justification::centred);
    bpmFooterLabel.setEditable(false, false, false);
    bpmFooterLabel.setWantsKeyboardFocus(false);
    bpmFooterLabel.setColour(juce::Label::backgroundColourId, PluginColours::mainMenuPrimary);
    bpmFooterLabel.setColour(juce::Label::outlineColourId, PluginColours::mainMenuPrimary);
    bpmFooterLabel.setColour(juce::Label::textColourId, PluginColours::onBackground);
    bpmFooterLabel.getProperties().set("skipTextNormalise", true);
    bpmFooterLabel.getProperties().set("useExplicitTextColour", true);
    bpmFooterLabel.setTooltip("Current tempo (host when available, otherwise last known BPM)");
    addAndMakeVisible(bpmFooterLabel);

    const auto setBoolParamFromUi = [this](const char* paramId, bool newValue)
    {
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(parameters.getParameter(paramId)))
        {
            if (p->get() == newValue)
                return;

            p->beginChangeGesture();
            p->setValueNotifyingHost(newValue ? 1.0f : 0.0f);
            p->endChangeGesture();
        }
    };

    whiteModeButton.setComponentID("MM_Toggle_ThemeWhite");
    whiteModeButton.setLookAndFeel(&mainMenuLF);
    whiteModeButton.setClickingTogglesState(true);
    whiteModeButton.setToggleState(false, juce::dontSendNotification);
    whiteModeButton.setTooltip("White mode: remap white keys to selected key/mode");
    whiteModeButton.onClick = [this, setBoolParamFromUi]
    {
        if (saveState == SaveState::Saving)
            return;

        const bool whiteEnabled = whiteModeButton.getToggleState();
        setBoolParamFromUi(ParamIDs::Global::whiteInputModeToggle, whiteEnabled);

        // Interlock rule: B can never stay ON while W is OFF.
        if (!whiteEnabled)
            setBoolParamFromUi(ParamIDs::Global::blackInputModeToggle, false);

        syncInputRemapModesFromVTS();
    };
    addAndMakeVisible(whiteModeButton);

    blackModeButton.setComponentID("MM_Toggle_ThemeBlack");
    blackModeButton.setLookAndFeel(&mainMenuLF);
    blackModeButton.setClickingTogglesState(true);
    blackModeButton.setToggleState(false, juce::dontSendNotification);
    blackModeButton.setTooltip("Black mode (available only when White mode is ON)");
    blackModeButton.onClick = [this, setBoolParamFromUi]
    {
        if (saveState == SaveState::Saving)
            return;

        if (!whiteModeButton.getToggleState())
        {
            blackModeButton.setToggleState(false, juce::dontSendNotification);
            applyInputRemapModeInterlock();
            return;
        }

        setBoolParamFromUi(ParamIDs::Global::blackInputModeToggle,
                           blackModeButton.getToggleState());
        syncInputRemapModesFromVTS();
    };
    addAndMakeVisible(blackModeButton);

    //==========================================================================
    // 2.3.b) Global Tonality / Mode (shared harmonizer key/scale)
    //==========================================================================
    globalKeyLabel.setText("Key", juce::dontSendNotification);
    globalKeyLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(globalKeyLabel);

    globalKeySelector.addItemList({
        "C", "C#", "D", "D#", "E", "F",
        "F#", "G", "G#", "A", "A#", "B", "Off"
    }, 1);
    addWithShadow(globalKeySelectorShadow, globalKeySelector, makeShadow());

    if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(parameters.getParameter(ParamIDs::Global::harmGlobalKey)))
    {
        const int idx = juce::jlimit(0, kGlobalKeyLastIndex, p->getIndex());
        globalKeySelector.setSelectedId(idx + 1, juce::dontSendNotification);
        lastGlobalKeyIndex = idx;
    }

    globalKeySelector.onChange = [this]
    {
        if (saveState == SaveState::Saving)
            return;

        const int idx = juce::jlimit(0, kGlobalKeyLastIndex, globalKeySelector.getSelectedId() - 1);

        if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(parameters.getParameter(ParamIDs::Global::harmGlobalKey)))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost(p->convertTo0to1((float) idx));
            p->endChangeGesture();
            lastGlobalKeyIndex = idx;
        }

        applyGlobalTonalityLock(true);
    };

    globalModeLabel.setText("Mode", juce::dontSendNotification);
    globalModeLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(globalModeLabel);

    globalModeSelector.addItemList({
        "Major", "Minor", "Dorian", "Phrygian", "Lydian",
        "Mixolydian", "Locrian", "Harmonic Minor", "Melodic Minor", "Chromatic"
    }, 1);
    addWithShadow(globalModeSelectorShadow, globalModeSelector, makeShadow());

    if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(parameters.getParameter(ParamIDs::Global::harmGlobalScale)))
    {
        const int idx = juce::jlimit(0, kGlobalScaleLastIndex, p->getIndex());
        globalModeSelector.setSelectedId(idx + 1, juce::dontSendNotification);
        lastGlobalModeIndex = idx;
    }

    globalModeSelector.onChange = [this]
    {
        if (saveState == SaveState::Saving)
            return;

        if (isGlobalKeyOffSelected())
        {
            applyGlobalTonalityLock(true);
            return;
        }

        const int idx = juce::jlimit(0, kGlobalScaleLastIndex, globalModeSelector.getSelectedId() - 1);

        if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(parameters.getParameter(ParamIDs::Global::harmGlobalScale)))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost(p->convertTo0to1((float) idx));
            p->endChangeGesture();
            lastGlobalModeIndex = idx;
        }
    };

    applyGlobalTonalityLock(false);

    //==========================================================================
    // 2.4) Header: Learn / Copy / Paste / Delete / Save
    //==========================================================================
    learnButton.setLookAndFeel(&mainMenuLF);
    copyButton.setLookAndFeel(&mainMenuLF);
    pasteButton.setLookAndFeel(&mainMenuLF);
    deleteButton.setLookAndFeel(&mainMenuLF);
    saveToggle.setLookAndFeel(&mainMenuLF);

    learnButton.setComponentID("MM_Toggle_Learn");
    copyButton.setComponentID("MM_Button_Copy");
    pasteButton.setComponentID("MM_Button_Paste");
    deleteButton.setComponentID("MM_Button_Delete");
    saveToggle.setComponentID("MM_Toggle_Save");

    // Header buttons: no keyboard focus.
    learnButton.setButtonText("L");
    copyButton.setButtonText("");
    pasteButton.setButtonText("");
    deleteButton.setButtonText("");
    saveToggle.setButtonText("");

    learnButton.setWantsKeyboardFocus(false);
    copyButton.setWantsKeyboardFocus(false);
    pasteButton.setWantsKeyboardFocus(false);
    deleteButton.setWantsKeyboardFocus(false);
    saveToggle.setWantsKeyboardFocus(false);

    // Behavior:
    // - learnButton: placeholder action (future MIDI learn)
    // - copyButton: toggle = "clipboard armed"
    // - paste/delete/save: momentary actions (clickingTogglesState=false)
    learnButton.setClickingTogglesState(false);
    copyButton.setClickingTogglesState(true);
    pasteButton.setClickingTogglesState(false);
    deleteButton.setClickingTogglesState(false);
    saveToggle.setClickingTogglesState(false);

    // Tooltips (ASCII only).
    learnButton.setTooltip("Learn (coming soon)");
    copyButton.setTooltip("Copy current snapshot");
    pasteButton.setTooltip("Paste into current snapshot");
    deleteButton.setTooltip("Reset current snapshot to defaults");
    saveToggle.setTooltip("Save current snapshot");

    // Load icons from BinaryData.
    copyIconDrawable   = loadSvgFromBinaryData(BinaryData::copy_svg,   BinaryData::copy_svgSize);
    pasteIconDrawable  = loadSvgFromBinaryData(BinaryData::paste_svg,  BinaryData::paste_svgSize);
    deleteIconDrawable = loadSvgFromBinaryData(BinaryData::delete_svg, BinaryData::delete_svgSize);

    if (copyIconDrawable)   setButtonIcon(copyButton,   copyIconDrawable.get());
    if (pasteIconDrawable)  setButtonIcon(pasteButton,  pasteIconDrawable.get());
    if (deleteIconDrawable) setButtonIcon(deleteButton, deleteIconDrawable.get());

    // Shadows
    addWithShadow(learnButtonShadow,  learnButton,  makeShadow());
    addWithShadow(copyButtonShadow,   copyButton,   makeShadow());
    addWithShadow(pasteButtonShadow,  pasteButton,  makeShadow());
    addWithShadow(deleteButtonShadow, deleteButton, makeShadow());
    addWithShadow(saveToggleShadow,   saveToggle,   makeShadow());

    // Buttons moved to PluginEditor top action column.
    learnButton.setVisible(false);
    copyButton.setVisible(false);
    pasteButton.setVisible(false);
    deleteButton.setVisible(false);
    saveToggle.setVisible(false);
    if (learnButtonShadow)  learnButtonShadow->setVisible(false);
    if (copyButtonShadow)   copyButtonShadow->setVisible(false);
    if (pasteButtonShadow)  pasteButtonShadow->setVisible(false);
    if (deleteButtonShadow) deleteButtonShadow->setVisible(false);
    if (saveToggleShadow)   saveToggleShadow->setVisible(false);

    // Clipboard starts disabled.
    clipboard.active = false;
    updateClipboardButtonsUI(false);
    
    // Placeholder (MIDI Learn to be implemented later).
    learnButton.onClick = [] {};

    //==========================================================================
    // 2.5) Copy behavior
    //==========================================================================
    copyButton.onClick = [this]
    {
        if (saveState == SaveState::Saving)
            return;

        if (!clipboard.active)
        {
            clipboard.source = getCurrentSelectionSafe();
            setClipboardActive(true);

           #if LOGS_ENABLED
            DBG_LOG("STATE", "MAINMENU", "SNAPSHOT CLIPBOARD", "#010#",
                    "COPY ON src=("
                    + juce::String(clipboard.source.bank) + ","
                    + juce::String(clipboard.source.preset) + ","
                    + juce::String(clipboard.source.snapshot) + ")");
           #endif
        }
        else
        {
            setClipboardActive(false);

           #if LOGS_ENABLED
            DBG_LOG("STATE", "MAINMENU", "SNAPSHOT CLIPBOARD", "#011#",
                    "COPY OFF clipboard cleared");
           #endif
        }
    };

    //==========================================================================
    // 2.6) Paste behavior
    //==========================================================================
    pasteButton.onClick = [this]
    {
        // Keep visual toggle consistent (paste is a momentary action).
        pasteButton.setToggleState(clipboard.active, juce::dontSendNotification);

        if (saveState == SaveState::Saving)
            return;

        if (!clipboard.active)
            return;

        const auto dst = getCurrentSelectionSafe();
        const auto src = clipboard.source;

        // Store-only: copy snapshot data in PresetManager storage.
        presetManager.copySnapshot(src.bank, src.preset, src.snapshot,
                                   dst.bank, dst.preset, dst.snapshot,
                                   PresetManager::Variant::X);

        presetManager.copySnapshot(src.bank, src.preset, src.snapshot,
                                   dst.bank, dst.preset, dst.snapshot,
                                   PresetManager::Variant::Y);

        // APPLY: load current snapshot so runtime matches stored snapshot.
        presetManager.loadCurrentSnapshot(presetManager.getCurrentVariant());

        // Now runtime == stored => mark saved (clear latch + set UI idle).
        presetManager.markSaved();

       #if LOGS_ENABLED
        DBG_LOG("ACTION", "PRESET", "SNAPSHOT PASTE", "#010#",
                "src=(" + juce::String(src.bank) + "," + juce::String(src.preset) + "," + juce::String(src.snapshot) + ")"
                " -> dst=(" + juce::String(dst.bank) + "," + juce::String(dst.preset) + "," + juce::String(dst.snapshot) + ")");
       #endif
    };

    //==========================================================================
    // 2.7) Delete behavior
    //==========================================================================
    deleteButton.onClick = [this]
    {
        if (saveState == SaveState::Saving)
            return;

        const auto dst = getCurrentSelectionSafe();

        // Store-only: reset snapshot content to defaults.
        presetManager.resetSnapshotToDefaults(dst.bank, dst.preset, dst.snapshot, PresetManager::Variant::X);
        presetManager.resetSnapshotToDefaults(dst.bank, dst.preset, dst.snapshot, PresetManager::Variant::Y);

        // APPLY: load current snapshot so runtime matches stored snapshot.
        presetManager.loadCurrentSnapshot(presetManager.getCurrentVariant());

        // Now runtime == stored => mark saved.
        presetManager.markSaved();

       #if LOGS_ENABLED
        DBG_LOG("ACTION", "PRESET", "SNAPSHOT DELETE", "#011#",
                "dst=(" + juce::String(dst.bank) + "," + juce::String(dst.preset) + "," + juce::String(dst.snapshot) + ") -> DEFAULTS");
       #endif
    };

    //==========================================================================
    // 2.8) Save behavior
    //==========================================================================
    // Save is synchronous UI action: capture current VTS state into selected snapshot.
    saveToggle.onClick = [this]
    {
        if (saveState != SaveState::Pending)
            return;

        // UI lock.
        setSaveState(SaveState::Saving);

        const auto dst = getCurrentSelectionSafe();
        const auto v   = presetManager.getCurrentVariant();

        // Store-only: capture VTS state into snapshot storage.
        presetManager.captureSnapshot(dst.bank, dst.preset, dst.snapshot, v);

        // Clear latch and return UI to Idle.
        presetManager.markSaved();
        setSaveState(SaveState::Idle);

       #if LOGS_ENABLED
        DBG_LOG("ACTION", "PRESET", "SNAPSHOT SAVE", "#004#",
                "dst=(" + juce::String(dst.bank) + "," + juce::String(dst.preset) + "," + juce::String(dst.snapshot) + ") saved");
       #endif
    };

    //==========================================================================
    // 2.9) Bank selector (base secure: after change -> preset=0 snapshot=0 and APPLY)
    //==========================================================================
    bankLabel.setText("Bank", juce::dontSendNotification);
    bankLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(bankLabel);

    bankSelector.onChange = [this]
    {
        if (saveState == SaveState::Saving)
            return;

        const int bank = bankSelector.getSelectedId() - 1;
        if (bank < 0 || bank == lastBankIndex)
            return;

        presetManager.setCurrentBank(bank);

        // Refresh preset list to match new bank.
        refreshPresetNames(bank);

        // Base secure: force preset=0 snapshot=0.
        presetManager.setCurrentPreset(0);
        presetSelector.setSelectedId(1, juce::dontSendNotification);

        presetManager.setCurrentSnapshot(0);

        updateSnapshotButtons(bank, presetManager.getCurrentPreset());
        refreshSnapshotNameDisplay(bank, 0, 0);

        // APPLY
        presetManager.loadCurrentSnapshot(presetManager.getCurrentVariant());

       #if LOGS_ENABLED
        DBG_LOG("STATE", "PRESET", "BANK SELECT", "#001#",
                "Bank " + juce::String(bank));
       #endif

        lastBankIndex     = bank;
        lastPresetIndex   = 0;
        lastSnapshotIndex = 0;
    };
    
    bankSelector.onRightClick = [this](const juce::MouseEvent&)
    {
        if (saveState == SaveState::Saving)
            return;

        showBankContextMenu();
    };

    addWithShadow(bankSelectorShadow, bankSelector, makeShadow());
    refreshBankNames();

    //==========================================================================
    // 2.10) Preset selector (base secure: after change -> snapshot=0 and APPLY)
    //==========================================================================
    presetLabel.setText("Preset", juce::dontSendNotification);
    presetLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(presetLabel);

    presetSelector.onChange = [this]
    {
        if (saveState == SaveState::Saving)
            return;

        const int bank   = presetManager.getCurrentBank();
        const int preset = presetSelector.getSelectedId() - 1;

        if (preset < 0 || preset == lastPresetIndex)
            return;

        presetManager.setCurrentPreset(preset);
        presetManager.setCurrentSnapshot(0);

        updateSnapshotButtons(bank, preset);
        refreshSnapshotNameDisplay(bank, preset, 0);

        // APPLY
        presetManager.loadCurrentSnapshot(presetManager.getCurrentVariant());

       #if LOGS_ENABLED
        DBG_LOG("STATE", "PRESET", "PRESET SELECT", "#002#",
                "Preset " + juce::String(preset));
       #endif

        lastPresetIndex   = preset;
        lastSnapshotIndex = 0;
    };
    
    presetSelector.onRightClick = [this](const juce::MouseEvent&)
    {
        if (saveState == SaveState::Saving)
            return;

        showPresetContextMenu();
    };

    addWithShadow(presetSelectorShadow, presetSelector, makeShadow());
    refreshPresetNames(presetManager.getCurrentBank());
    refreshSnapshotNameDisplay(presetManager.getCurrentBank(),
                               presetManager.getCurrentPreset(),
                               presetManager.getCurrentSnapshot());

    //==========================================================================
    // 2.11) Snapshot buttons (8)
    //==========================================================================
    snapshotButtons.clear();
    snapshotShadows.clear();

    for (int i = 0; i < 8; ++i)
    {
        auto btn = std::make_unique<juce::ToggleButton>("Snap" + juce::String(i + 1));
        btn->setButtonText(juce::String(i + 1));
        btn->setRadioGroupId(800);
        btn->setClickingTogglesState(false);
        btn->setLookAndFeel(&mainMenuLF);
        btn->setComponentID("MM_Toggle_Snapshot" + juce::String(i + 1));
        btn->setColour(juce::ToggleButton::textColourId, PluginColours::onBackground);

        juce::ToggleButton* btnPtr = btn.get();

        btn->onClick = [this, btnPtr, i]
        {
            if (saveState == SaveState::Saving)
                return;

            if (!btnPtr->isVisible() || !btnPtr->isEnabled())
                return;

            // Enforce radio manually (clickingTogglesState=false).
            for (int j = 0; j < (int)snapshotButtons.size(); ++j)
            {
                const bool isThis = (snapshotButtons[(size_t)j].get() == btnPtr);
                snapshotButtons[(size_t)j]->setToggleState(isThis, juce::dontSendNotification);
            }

            presetManager.setCurrentSnapshot(i);

            // APPLY
            presetManager.loadCurrentSnapshot(presetManager.getCurrentVariant());
            lastSnapshotIndex = i;
            refreshSnapshotNameDisplay(presetManager.getCurrentBank(),
                                       presetManager.getCurrentPreset(),
                                       i);

           #if LOGS_ENABLED
            DBG_LOG("STATE", "PRESET", "SNAPSHOT SELECT", "#003#",
                    "Snapshot " + juce::String(i));
           #endif
        };

        auto shadow = std::make_unique<ShadowComponent>(
            makeShadow(),
            0.0f,
            true, true, true, true,
            10);

        shadow->addAndMakeVisible(*btn);
        addAndMakeVisible(*shadow);

        snapshotShadows.push_back(std::move(shadow));
        snapshotButtons.push_back(std::move(btn));
    }

    updateSnapshotButtons(presetManager.getCurrentBank(), presetManager.getCurrentPreset());

    //==========================================================================
    // 2.12) PresetManager -> UI selection sync
    //==========================================================================
    presetManager.setOnSelectionChange([this](int bank, int preset, int snapshot, int /*variant*/)
    {
        juce::MessageManager::callAsync(
            [sp = juce::Component::SafePointer(this), bank, preset, snapshot]
            {
                if (sp == nullptr)
                    return;

                // Bank
                if (bank != sp->lastBankIndex)
                {
                    sp->bankSelector.setSelectedId(bank + 1, juce::dontSendNotification);
                    sp->refreshPresetNames(bank);
                    sp->lastBankIndex = bank;
                }

                // Preset
                if (preset != sp->lastPresetIndex)
                {
                    sp->presetSelector.setSelectedId(preset + 1, juce::dontSendNotification);
                    sp->lastPresetIndex = preset;
                }

                // Snapshots: visibility + current toggle
                sp->updateSnapshotButtons(bank, preset);
                sp->lastSnapshotIndex = snapshot;
                sp->refreshSnapshotNameDisplay(bank, preset, snapshot);
                
                // keep manual-bound UI in sync with VTS.
                sp->syncUIFromVTS();

            });
    });

    //==========================================================================
    // 2.13) PresetManager -> UI pending sync
    //==========================================================================
    presetManager.setOnPendingChange([this](bool pending)
    {
        juce::MessageManager::callAsync(
            [sp = juce::Component::SafePointer(this), pending]
            {
                if (sp == nullptr)
                    return;

                // Rule: while Saving, ignore pending=true (do not flip back to Pending).
                if (pending && sp->saveState == SaveState::Saving)
                    return;

               #if LOGS_ENABLED
                DBG_LOG("STATE", "MAINMENU", "SAVE STATE", "#100#",
                        pending ? "Pending=true -> Save=Pending"
                                : "Pending=false -> Save=Idle");
               #endif

                sp->setSaveState(pending ? SaveState::Pending : SaveState::Idle);

                // Refresh manual-bound controls after state replacement.
                if (!pending)
                    sp->syncUIFromVTS();
            });
    });

    // Initial UI state
    setSaveState(SaveState::Idle);
    syncUIFromVTS();
}

MainMenu::~MainMenu()
{
    // Avoid callbacks firing after destruction.
    presetManager.setOnSelectionChange({});
    presetManager.setOnPendingChange({});

    setLookAndFeel(nullptr);
}

//==============================================================================
// 3) Save state machine
//------------------------------------------------------------------------------
void MainMenu::setSaveState(SaveState newState)
{
    if (saveState == newState)
        return;

    saveState = newState;

    const bool lockEdits = (saveState == SaveState::Saving);

    // Save button is enabled only when Pending.
    saveToggle.setEnabled(saveState == SaveState::Pending);

    // Save is an action button, not a persistent toggle.
    saveToggle.setToggleState(false, juce::dontSendNotification);

    setPresetNavigationEnabled(!lockEdits);
    updateClipboardButtonsUI(lockEdits);

    saveToggle.repaint();
}

bool MainMenu::isGlobalKeyOffSelected() const noexcept
{
    return (globalKeySelector.getSelectedId() - 1) == kGlobalKeyOffIndex;
}

void MainMenu::applyGlobalTonalityLock(bool forceScaleChromaticInParam)
{
    const bool keyOff = isGlobalKeyOffSelected();

    if (keyOff)
    {
        const int chromaticId = kGlobalScaleChromaticIndex + 1;
        if (globalModeSelector.getSelectedId() != chromaticId)
            globalModeSelector.setSelectedId(chromaticId, juce::dontSendNotification);

        lastGlobalModeIndex = kGlobalScaleChromaticIndex;

        if (forceScaleChromaticInParam)
        {
            if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(parameters.getParameter(ParamIDs::Global::harmGlobalScale)))
            {
                if (p->getIndex() != kGlobalScaleChromaticIndex)
                {
                    p->beginChangeGesture();
                    p->setValueNotifyingHost(p->convertTo0to1((float) kGlobalScaleChromaticIndex));
                    p->endChangeGesture();
                }
            }
        }
    }

    globalModeSelector.setEnabled(globalKeySelector.isEnabled() && !keyOff);
}

void MainMenu::setPresetNavigationEnabled(bool shouldBeEnabled)
{
    // Only lock the UI during Saving.
    muteButton.setEnabled(shouldBeEnabled);
    channelModeSelector.setEnabled(shouldBeEnabled);
    globalKeySelector.setEnabled(shouldBeEnabled);
    applyGlobalTonalityLock(false);

    snapshotNameButton.setEnabled(shouldBeEnabled);
    whiteModeButton.setEnabled(shouldBeEnabled);
    applyInputRemapModeInterlock();
    chordFooterLabel.setEnabled(true);
    bpmFooterLabel.setEnabled(true);
    learnButton.setEnabled(shouldBeEnabled);

    bankSelector.setEnabled(shouldBeEnabled);
    presetSelector.setEnabled(shouldBeEnabled);

    for (auto& b : snapshotButtons)
        if (b != nullptr)
            b->setEnabled(shouldBeEnabled);
}

void MainMenu::refreshSnapshotNameDisplay(int bankIndex, int presetIndex, int snapshotIndex)
{
    const int safeBankIndex = juce::jlimit(0, juce::jmax(0, presetManager.getNumBanks() - 1), bankIndex);
    const int safePresetIndex =
        juce::jlimit(0, juce::jmax(0, presetManager.getNumPresets(safeBankIndex) - 1), presetIndex);
    const int safeSnapshotIndex =
        juce::jlimit(0, juce::jmax(0, presetManager.getNumSnapshots(safeBankIndex, safePresetIndex) - 1), snapshotIndex);

    const juce::String bankName = presetManager.getBankName(safeBankIndex);
    const juce::String presetName = presetManager.getPresetName(safeBankIndex, safePresetIndex);
    const juce::String snapshotName =
        presetManager.getSnapshotName(safeBankIndex,
                                      safePresetIndex,
                                      safeSnapshotIndex,
                                      presetManager.getCurrentVariant());

    snapshotNameButton.setText(formatMainMenuHeaderText(bankName,
                                                        presetName,
                                                        safeSnapshotIndex + 1,
                                                        snapshotName),
                               juce::dontSendNotification);
}

void MainMenu::showSnapshotContextMenu()
{
    juce::PopupMenu menu;
    menu.addItem(1, "Rename...");
    menu.setLookAndFeel(&getLookAndFeel());

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&snapshotNameButton),
                       [sp = juce::Component::SafePointer(this)](int result)
                       {
                           if (sp == nullptr || result != 1)
                               return;

                           sp->requestRenameCurrentSnapshot();
                       });
}

void MainMenu::requestRenameCurrentSnapshot()
{
    if (saveState == SaveState::Saving)
        return;

    const int bankIndex = presetManager.getCurrentBank();
    const int presetIndex = presetManager.getCurrentPreset();
    const int snapshotIndex = presetManager.getCurrentSnapshot();

    if (bankIndex < 0 || bankIndex >= presetManager.getNumBanks())
        return;
    if (presetIndex < 0 || presetIndex >= presetManager.getNumPresets(bankIndex))
        return;
    if (snapshotIndex < 0 || snapshotIndex >= presetManager.getNumSnapshots(bankIndex, presetIndex))
        return;

    showRenameDialogAsync("Rename Snapshot",
                          presetManager.getSnapshotName(bankIndex, presetIndex, snapshotIndex, presetManager.getCurrentVariant()),
                          [sp = juce::Component::SafePointer(this), bankIndex, presetIndex, snapshotIndex](juce::String requestedName)
                          {
                              if (sp == nullptr)
                                  return;
                              if (sp->saveState == SaveState::Saving)
                                  return;

                              const juce::String safeName = sanitiseUserNameForCompare(requestedName);
                              if (safeName.isEmpty())
                              {
                                  juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                                         "Rename Snapshot",
                                                                         "Invalid name. Use letters, numbers and spaces.");
                                  return;
                              }

                              if (!sp->presetManager.renameSnapshot(bankIndex, presetIndex, snapshotIndex, safeName))
                              {
                                  juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                                         "Rename Snapshot",
                                                                         "Rename failed.");
                                  return;
                              }

                              sp->refreshSnapshotNameDisplay(bankIndex, presetIndex, snapshotIndex);
                          });
}

void MainMenu::showBankContextMenu()
{
    juce::PopupMenu menu;
    menu.addItem(1, "Rename...");
    menu.setLookAndFeel(&getLookAndFeel());

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&bankSelector),
                       [sp = juce::Component::SafePointer(this)](int result)
                       {
                           if (sp == nullptr || result != 1)
                               return;

                           sp->requestRenameCurrentBank();
                       });
}

void MainMenu::showPresetContextMenu()
{
    juce::PopupMenu menu;
    menu.addItem(1, "Rename...");
    menu.setLookAndFeel(&getLookAndFeel());

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&presetSelector),
                       [sp = juce::Component::SafePointer(this)](int result)
                       {
                           if (sp == nullptr || result != 1)
                               return;

                           sp->requestRenameCurrentPreset();
                       });
}

void MainMenu::requestRenameCurrentBank()
{
    if (saveState == SaveState::Saving)
        return;

    const int bankIndex = presetManager.getCurrentBank();
    if (bankIndex < 0 || bankIndex >= presetManager.getNumBanks())
        return;
    
    showRenameDialogAsync("Rename Bank",
                          presetManager.getBankName(bankIndex),
                          [sp = juce::Component::SafePointer(this), bankIndex](juce::String requestedName)
                          {
                              if (sp == nullptr)
                                  return;
                              if (sp->saveState == SaveState::Saving)
                                  return;
                              if (bankIndex < 0 || bankIndex >= sp->presetManager.getNumBanks())
                                  return;

                              const juce::String safeName = sanitiseUserNameForCompare(requestedName);
                              if (safeName.isEmpty())
                              {
                                  juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                                         "Rename Bank",
                                                                         "Invalid name. Use letters, numbers and spaces.");
                                  return;
                              }

                              const int numBanks = sp->presetManager.getNumBanks();
                              for (int i = 0; i < numBanks; ++i)
                              {
                                  if (i == bankIndex)
                                      continue;

                                  const juce::String existing = sanitiseUserNameForCompare(sp->presetManager.getBankName(i));
                                  if (existing.compareIgnoreCase(safeName) == 0)
                                  {
                                      juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                                             "Rename Bank",
                                                                             "This bank name already exists.");
                                      return;
                                  }
                              }

                              if (!sp->presetManager.renameBank(bankIndex, safeName))
                              {
                                  juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                                         "Rename Bank",
                                                                         "Rename failed. Name may be invalid or already used.");
                                  return;
                              }

                              sp->refreshBankNames();
                              sp->bankSelector.setSelectedId(bankIndex + 1, juce::dontSendNotification);
                              sp->refreshPresetNames(bankIndex);
                              sp->refreshSnapshotNameDisplay(bankIndex,
                                                             sp->presetManager.getCurrentPreset(),
                                                             sp->presetManager.getCurrentSnapshot());
                          });
}

void MainMenu::requestRenameCurrentPreset()
{
    if (saveState == SaveState::Saving)
        return;

    const int bankIndex = presetManager.getCurrentBank();
    const int presetIndex = presetManager.getCurrentPreset();

    if (bankIndex < 0 || bankIndex >= presetManager.getNumBanks())
        return;
    if (presetIndex < 0 || presetIndex >= presetManager.getNumPresets(bankIndex))
        return;
    
    showRenameDialogAsync("Rename Preset",
                          presetManager.getPresetName(bankIndex, presetIndex),
                          [sp = juce::Component::SafePointer(this), bankIndex, presetIndex](juce::String requestedName)
                          {
                              if (sp == nullptr)
                                  return;
                              if (sp->saveState == SaveState::Saving)
                                  return;
                              if (bankIndex < 0 || bankIndex >= sp->presetManager.getNumBanks())
                                  return;
                              if (presetIndex < 0 || presetIndex >= sp->presetManager.getNumPresets(bankIndex))
                                  return;

                              const juce::String safeName = sanitiseUserNameForCompare(requestedName);
                              if (safeName.isEmpty())
                              {
                                  juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                                         "Rename Preset",
                                                                         "Invalid name. Use letters, numbers and spaces.");
                                  return;
                              }

                              const int numPresets = sp->presetManager.getNumPresets(bankIndex);
                              for (int i = 0; i < numPresets; ++i)
                              {
                                  if (i == presetIndex)
                                      continue;

                                  const juce::String existing = sanitiseUserNameForCompare(
                                      sp->presetManager.getPresetName(bankIndex, i));
                                  if (existing.compareIgnoreCase(safeName) == 0)
                                  {
                                      juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                                             "Rename Preset",
                                                                             "This preset name already exists in this bank.");
                                      return;
                                  }
                              }

                              if (!sp->presetManager.renamePreset(bankIndex, presetIndex, safeName))
                              {
                                  juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                                         "Rename Preset",
                                                                         "Rename failed. Name may be invalid or already used.");
                                  return;
                              }

                              sp->refreshPresetNames(bankIndex);
                              sp->presetSelector.setSelectedId(presetIndex + 1, juce::dontSendNotification);
                              sp->refreshSnapshotNameDisplay(bankIndex,
                                                             presetIndex,
                                                             sp->presetManager.getCurrentSnapshot());
                          });
}

//==============================================================================
// 4) Layout
//------------------------------------------------------------------------------
void MainMenu::resized()
{
    constexpr int shadowGap                    = 10;
    constexpr int headerRowHeight              = 20;
    constexpr int tonalityRowHeight            = 20;
    constexpr int presetRowHeight              = 20;
    constexpr int snapshotRowHeight            = 20;
    constexpr int namesRowHeight               = 20;
    constexpr int gapYBetweenHeaderAndTonality = 4;
    constexpr int gapYBetweenTonalityAndPreset = 4;
    constexpr int gapYBetweenPresetAndSnapshot = 4;
    constexpr int gapYBetweenSnapshotAndNames  = 4;
    constexpr int gap                          = 6;

    const float topY         = (float) UiMetrics::kModuleOuterMargin;
    const float leftX        = (float) UiMetrics::kModuleOuterMargin;
    const float contentWidth = (float) getWidth() - (2.0f * (float) UiMetrics::kModuleOuterMargin);
    const int moduleHeight   = getMainMenuModuleHeightFor(*this);

    auto contentArea = juce::Rectangle<int>((int)leftX, (int)topY, (int)contentWidth, moduleHeight);
    auto bounds      = contentArea.reduced(UiMetrics::kModuleInnerMargin);

    //--------------------------------------------------------------------------
    // Header row: Snapshot name | Copy Paste Delete | Save
    //--------------------------------------------------------------------------
    auto headerRow = bounds.removeFromTop(headerRowHeight);
    headerInfoPillArea = headerRow;

    const int comboLabelW   = 34;
    snapshotNameButton.setBounds(headerRow);

    bounds.removeFromTop(gapYBetweenHeaderAndTonality);

    //--------------------------------------------------------------------------
    // Tonality row: Key / Mode (global harmonizer mapping)
    //--------------------------------------------------------------------------
    auto tonalityRow = bounds.removeFromTop(tonalityRowHeight);

    auto keySection = tonalityRow.removeFromLeft(tonalityRow.proportionOfWidth(0.47f));
    globalKeyLabel.setBounds(keySection.removeFromLeft(comboLabelW));
    keySection.removeFromLeft(gap);

    if (globalKeySelectorShadow) globalKeySelectorShadow->setBounds(keySection.expanded(shadowGap));
    if (globalKeySelectorShadow) globalKeySelector.setBounds(globalKeySelectorShadow->getShadowArea());

    tonalityRow.removeFromLeft(gap);

    globalModeLabel.setBounds(tonalityRow.removeFromLeft(comboLabelW));
    tonalityRow.removeFromLeft(gap);

    if (globalModeSelectorShadow) globalModeSelectorShadow->setBounds(tonalityRow.expanded(shadowGap));
    if (globalModeSelectorShadow) globalModeSelector.setBounds(globalModeSelectorShadow->getShadowArea());

    bounds.removeFromTop(gapYBetweenTonalityAndPreset);

    //--------------------------------------------------------------------------
    // Preset row: Bank / Preset
    //--------------------------------------------------------------------------
    auto presetRow = bounds.removeFromTop(presetRowHeight);

    auto bankSection = presetRow.removeFromLeft(presetRow.proportionOfWidth(0.47f));
    bankLabel.setBounds(bankSection.removeFromLeft(comboLabelW));
    bankSection.removeFromLeft(gap);

    if (bankSelectorShadow) bankSelectorShadow->setBounds(bankSection.expanded(shadowGap));
    if (bankSelectorShadow) bankSelector.setBounds(bankSelectorShadow->getShadowArea());

    presetRow.removeFromLeft(gap);

    presetLabel.setBounds(presetRow.removeFromLeft(comboLabelW));
    presetRow.removeFromLeft(gap);

    if (presetSelectorShadow) presetSelectorShadow->setBounds(presetRow.expanded(shadowGap));
    if (presetSelectorShadow) presetSelector.setBounds(presetSelectorShadow->getShadowArea());

    bounds.removeFromTop(gapYBetweenPresetAndSnapshot);

    //--------------------------------------------------------------------------
    // Snapshots row
    //--------------------------------------------------------------------------
    auto snapshotRow = bounds.removeFromTop(snapshotRowHeight);
    snapshotBackgroundArea = snapshotRow;

    const int btnWidth = (snapshotRow.getWidth() / 8);

    for (int i = 0; i < 8; ++i)
    {
        auto area = snapshotRow.removeFromLeft(btnWidth);

        if (i < (int)snapshotShadows.size() && snapshotShadows[(size_t)i])
        {
            snapshotShadows[(size_t)i]->setBounds(area.expanded(8));

            if (i < (int)snapshotButtons.size() && snapshotButtons[(size_t)i])
                snapshotButtons[(size_t)i]->setBounds(snapshotShadows[(size_t)i]->getShadowArea());
        }
    }

    bounds.removeFromTop(gapYBetweenSnapshotAndNames);

    //--------------------------------------------------------------------------
    // Footer modules: chord detector + BPM + W/B mode
    //--------------------------------------------------------------------------
    auto namesRow = bounds.removeFromTop(namesRowHeight);
    const int footerGap = gap;
    const int availableWidth = juce::jmax(0, namesRow.getWidth() - (2 * footerGap));
    const int partWidth = availableWidth / 3;

    auto chordArea = namesRow.removeFromLeft(partWidth);
    namesRow.removeFromLeft(footerGap);
    auto bpmArea = namesRow.removeFromLeft(partWidth);
    namesRow.removeFromLeft(footerGap);
    auto modeArea = namesRow;

    chordFooterLabel.setBounds(chordArea);
    bpmFooterLabel.setBounds(bpmArea);

    const int modeGap = juce::jmax(2, gap / 2);
    const int modeButtonWidth = juce::jmax(1, (modeArea.getWidth() - modeGap) / 2);
    auto whiteArea = modeArea.removeFromLeft(modeButtonWidth);
    modeArea.removeFromLeft(modeGap);
    auto blackArea = modeArea.removeFromLeft(modeButtonWidth);

    whiteModeButton.setBounds(whiteArea);
    blackModeButton.setBounds(blackArea);
}

//==============================================================================
// 5) Data refresh (Bank/Preset/Snapshot lists)
//------------------------------------------------------------------------------
void MainMenu::refreshBankNames()
{
    bankSelector.clear();

    const int numBanks = presetManager.getNumBanks();
    for (int i = 0; i < numBanks; ++i)
        bankSelector.addItem(presetManager.getBankName(i), i + 1);

    bankSelector.setSelectedId(presetManager.getCurrentBank() + 1, juce::dontSendNotification);
}

void MainMenu::refreshPresetNames(int bankIndex)
{
    presetSelector.clear();

    const int numPresets = presetManager.getNumPresets(bankIndex);
    for (int i = 0; i < numPresets; ++i)
        presetSelector.addItem(presetManager.getPresetName(bankIndex, i), i + 1);

    presetSelector.setSelectedId(presetManager.getCurrentPreset() + 1, juce::dontSendNotification);
}

void MainMenu::updateSnapshotButtons(int bankIndex, int presetIndex)
{
    const int count      = presetManager.getNumSnapshots(bankIndex, presetIndex);
    const int snapIndex  = presetManager.getCurrentSnapshot();
    const bool lockEdits = (saveState == SaveState::Saving);

    for (int i = 0; i < (int)snapshotButtons.size(); ++i)
    {
        const bool exists = (i < count);

        if (snapshotButtons[(size_t)i])
        {
            snapshotButtons[(size_t)i]->setVisible(exists);
            snapshotButtons[(size_t)i]->setEnabled(exists && !lockEdits);

            if (exists)
                snapshotButtons[(size_t)i]->setToggleState(i == snapIndex, juce::dontSendNotification);
            else
                snapshotButtons[(size_t)i]->setToggleState(false, juce::dontSendNotification);
        }

        if (snapshotShadows[(size_t)i])
            snapshotShadows[(size_t)i]->setVisible(exists);
    }

    lastSnapshotIndex = snapIndex;
    refreshSnapshotNameDisplay(bankIndex, presetIndex, snapIndex);
}

void MainMenu::syncHeaderFromVTS()
{
    // Mute
    if (auto* p = parameters.getRawParameterValue(ParamIDs::inputMuteToggle))
    {
        const bool muteState = (p->load() > 0.5f);
        muteButton.setToggleState(muteState, juce::dontSendNotification);
        lastMuteState = muteState;
    }

    // Channel
    if (auto* p = dynamic_cast<juce::AudioParameterInt*>(parameters.getParameter(ParamIDs::channelFilter)))
    {
        const int chan = p->get();
        channelModeSelector.setSelectedId(chan, juce::dontSendNotification);
        lastChannel = chan;
    }
}

void MainMenu::syncTonalityFromVTS()
{
    if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(parameters.getParameter(ParamIDs::Global::harmGlobalKey)))
    {
        const int idx = juce::jlimit(0, kGlobalKeyLastIndex, p->getIndex());
        globalKeySelector.setSelectedId(idx + 1, juce::dontSendNotification);
        lastGlobalKeyIndex = idx;
    }

    if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(parameters.getParameter(ParamIDs::Global::harmGlobalScale)))
    {
        int idx = juce::jlimit(0, kGlobalScaleLastIndex, p->getIndex());
        if (isGlobalKeyOffSelected())
            idx = kGlobalScaleChromaticIndex;

        globalModeSelector.setSelectedId(idx + 1, juce::dontSendNotification);
        lastGlobalModeIndex = idx;
    }

    applyGlobalTonalityLock(false);
}

void MainMenu::syncInputRemapModesFromVTS()
{
    bool whiteEnabled = false;
    bool blackEnabled = false;

    if (auto* p = parameters.getRawParameterValue(ParamIDs::Global::whiteInputModeToggle))
        whiteEnabled = (p->load() > 0.5f);

    if (auto* p = parameters.getRawParameterValue(ParamIDs::Global::blackInputModeToggle))
        blackEnabled = (p->load() > 0.5f);

    if (!whiteEnabled)
        blackEnabled = false;

    whiteModeButton.setToggleState(whiteEnabled, juce::dontSendNotification);
    blackModeButton.setToggleState(blackEnabled, juce::dontSendNotification);
    applyInputRemapModeInterlock();
}

void MainMenu::applyInputRemapModeInterlock()
{
    const bool whiteEnabled = whiteModeButton.getToggleState();
    const bool canEditBlack = whiteEnabled && whiteModeButton.isEnabled();

    if (!whiteEnabled && blackModeButton.getToggleState())
        blackModeButton.setToggleState(false, juce::dontSendNotification);

    blackModeButton.setEnabled(canEditBlack);
}

void MainMenu::syncUIFromVTS()
{
    syncHeaderFromVTS();
    syncTonalityFromVTS();
    syncInputRemapModesFromVTS();

    // If your LAF uses internal cached paints, this ensures visuals update.
    repaint();
}

//==============================================================================
// 6) Paint
//------------------------------------------------------------------------------
void MainMenu::paint(juce::Graphics& g)
{
    constexpr float radius       = UiMetrics::kModuleCornerRadius;
    constexpr float topY         = (float) UiMetrics::kModuleOuterMargin;
    const float moduleHeight     = (float) getMainMenuModuleHeightFor(*this);

    juce::Rectangle<float> backgroundArea =
    {
        (float) UiMetrics::kModuleOuterMargin,
        topY,
        (float) getWidth() - 2.0f * (float) UiMetrics::kModuleOuterMargin,
        moduleHeight
    };

    juce::Path p;
    p.addRoundedRectangle(backgroundArea, radius);
    g.setColour(PluginColours::mainMenuSurface);
    g.fillPath(p);

    // Header pill: BANK | PRESET : SNAP - NAME
    if (!headerInfoPillArea.isEmpty())
    {
        juce::Path headerPill;
        const float headerRadius = PluginLookAndFeel::getDynamicCornerRadius(headerInfoPillArea);
        headerPill.addRoundedRectangle(headerInfoPillArea.toFloat(), headerRadius);
        g.setColour(PluginColours::onBackground);
        g.fillPath(headerPill);
    }

    // Footer pill behind snapshot buttons
    if (!snapshotBackgroundArea.isEmpty())
    {
        juce::Path pillPath;

        const float dynRadius = PluginLookAndFeel::getDynamicCornerRadius(snapshotBackgroundArea);
        pillPath.addRoundedRectangle(snapshotBackgroundArea.toFloat(), dynRadius);

        g.setColour(PluginColours::mainMenuSurface);
        g.fillPath(pillPath);
    }

    auto drawFooterPill = [&g](juce::Rectangle<int> area, juce::Colour fillColour)
    {
        if (area.isEmpty())
            return;

        juce::Path pillPath;
        const float dynRadius = PluginLookAndFeel::getDynamicCornerRadius(area);
        pillPath.addRoundedRectangle(area.toFloat(), dynRadius);
        g.setColour(fillColour);
        g.fillPath(pillPath);
    };

    drawFooterPill(chordFooterLabel.getBounds(), PluginColours::onBackground);
    drawFooterPill(bpmFooterLabel.getBounds(), PluginColours::mainMenuPrimary);
    drawFooterPill(whiteModeButton.getBounds().getUnion(blackModeButton.getBounds()),
                   PluginColours::mainMenuPrimary);
}
