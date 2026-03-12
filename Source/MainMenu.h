#pragma once

/*
==============================================================================
MainMenu.h
------------------------------------------------------------------------------
MidivisiVici - MainMenu UI (Preset + Transport + Header Controls)

This component is the main UI surface for:
  - Global controls (Mute, MIDI channel mode)
  - Preset navigation (Bank, Preset, Snapshot, Variant)
  - Snapshot clipboard workflow (Copy / Paste / Delete)
  - Save button with 3-state visual logic (Idle / Pending / Saving)

PROJECT RULES (important):
  - ASCII only for comments/logs (no accents / no emoji).
  - UI thread only: any ValueTree mutations happen on message thread.
  - Audio thread never touches ValueTree (PresetManager RT methods are atomics only).
  - Logs (DBG_LOG) must be stable and avoid spam (only on state change).

Dependencies:
  - PresetManager provides banks/presets/snapshots selection + persistence.
  - PluginParameters provides ParamIDs.
  - Custom components: ShadowComponent, FlatComboBox, LookAndFeel, Colours.

==============================================================================
*/
/**
 * @file MainMenu.h
 * @brief Main editor menu for preset navigation and global controls.
 *
 * Threading:
 * - UI and ValueTree mutations on message thread.
 * - Not RT-safe.
 */

#include <JuceHeader.h>

#include "0-component/ShadowComponent.h"
#include "0-component/FlatComboBox.h"

#include "PluginParameters.h"
#include "PluginColours.h"
#include "PluginLookAndFeel.h"
#include "DebugConfig.h"

#include "PresetManager.h"

/** @brief Label variant exposing right-click callback for contextual renaming. */
class ContextLabel : public juce::Label
{
public:
    using juce::Label::Label;

    std::function<void(const juce::MouseEvent&)> onRightClick;

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            if (onRightClick)
            onRightClick(e);
            return;
        }

        juce::Label::mouseDown(e);
    }
};

//==============================================================================
// SaveState
//------------------------------------------------------------------------------
// UI-only state machine for the "Save" button.
// - Idle    : nothing to save (button disabled)
// - Pending : unsaved changes detected (button enabled)
// - Saving  : transient visual state while saving is running
//------------------------------------------------------------------------------
enum class SaveState
{
    Idle = 0,
    Pending,
    Saving
};

//==============================================================================
// MainMenu
//------------------------------------------------------------------------------
/**
 * @brief Composite top-level menu for banks/presets/snapshots and globals.
 *
 * Pattern:
 * - Pattern: Facade
 * - Problem solved: centralize user flows for preset persistence and global
 *   control edits in one consistent interaction surface.
 * - Participants: `MainMenu`, `PresetManager`, APVTS-bound controls.
 * - Flow: user edits control -> write APVTS/preset selection -> refresh labels.
 * - Pitfalls: keep save/clipboard states coherent during async host updates.
 */
class MainMenu : public juce::Component
{
public:
    //==========================================================================
    // 1) Ctor / dtor
    //==========================================================================
    MainMenu(juce::AudioProcessorValueTreeState& vts, PresetManager& presetMgr);
    ~MainMenu() override;

    //==========================================================================
    // 2) JUCE overrides
    //==========================================================================
    void resized() override;
    void paint(juce::Graphics& g) override;

    //==========================================================================
    // 3) Save button state API (UI)
    //==========================================================================
    SaveState getSaveState() const noexcept { return saveState; }
    void setSaveState(SaveState newState);

    // Clipboard state (Copy/Paste armed)
    bool isClipboardActive() const noexcept { return clipboard.active; }

    // External action triggers (used by top action column in PluginEditor).
    void triggerCopyAction();
    void triggerPasteAction();
    void triggerDeleteAction();
    void triggerSaveAction();
    void triggerLearnAction();

    /**
     * @brief Update footer BPM display (host BPM when available, last known otherwise).
     *
     * Thread: message thread.
     * RT-safe: no.
     */
    void setDisplayedBpm(double bpm);

    /**
     * @brief Update chord-flow footer text as "IN -> OUT".
     *
     * Thread: message thread.
     * RT-safe: no.
     */
    void setDetectedChordFlow(const juce::String& inputChordName,
                              const juce::String& outputChordName);

private:
    //==========================================================================
    // 4) References (non-owning)
    //==========================================================================
    juce::AudioProcessorValueTreeState& parameters;
    PresetManager& presetManager;

    //==========================================================================
    // 5) Local LookAndFeel
    //==========================================================================
    PluginLookAndFeel mainMenuLF;

    //==========================================================================
    // 6) Header controls
    //==========================================================================
    juce::ToggleButton muteButton { "Mute" };
    std::unique_ptr<ShadowComponent> muteButtonShadow;

    juce::Label midiChannelLabel;
    FlatComboBox channelModeSelector;
    std::unique_ptr<ShadowComponent> channelModeSelectorShadow;
    
    ContextLabel snapshotNameButton;       // top title: BANK | PRESET : SNAP - NAME
    juce::Label chordFooterLabel;          // footer left: chord detection placeholder
    juce::Label bpmFooterLabel;            // footer center: current BPM
    juce::ToggleButton whiteModeButton { "W" };
    juce::ToggleButton blackModeButton { "B" };

    // Global tonality/mode (shared by all harmonizer lanes)
    juce::Label globalKeyLabel;
    FlatComboBox globalKeySelector;
    std::unique_ptr<ShadowComponent> globalKeySelectorShadow;

    juce::Label globalModeLabel;
    FlatComboBox globalModeSelector;
    std::unique_ptr<ShadowComponent> globalModeSelectorShadow;

    // Copy / Paste / Delete (left of Save)
    juce::ToggleButton learnButton { "L" };
    juce::DrawableButton copyButton   { "Copy",   juce::DrawableButton::ImageFitted };
    juce::DrawableButton pasteButton  { "Paste",  juce::DrawableButton::ImageFitted };
    juce::DrawableButton deleteButton { "Delete", juce::DrawableButton::ImageFitted };

    std::unique_ptr<ShadowComponent> learnButtonShadow;
    std::unique_ptr<ShadowComponent> copyButtonShadow;
    std::unique_ptr<ShadowComponent> pasteButtonShadow;
    std::unique_ptr<ShadowComponent> deleteButtonShadow;

    std::unique_ptr<juce::Drawable> copyIconDrawable;
    std::unique_ptr<juce::Drawable> pasteIconDrawable;
    std::unique_ptr<juce::Drawable> deleteIconDrawable;

    // Save (3-state visual logic)
    juce::ToggleButton saveToggle;
    std::unique_ptr<ShadowComponent> saveToggleShadow;

    SaveState saveState = SaveState::Idle;

    //==========================================================================
    // 7) Snapshot clipboard (Copy/Paste source tracking)
    //==========================================================================
    struct SnapshotAddress
    {
        int bank     = 0;
        int preset   = 0;
        int snapshot = 0;
    };

    struct SnapshotClipboard
    {
        bool active = false;     // true <=> Copy armed (Paste enabled)
        SnapshotAddress source;  // source snapshot address
    };

    SnapshotClipboard clipboard;

    // Clipboard helpers (UI-safe)
    SnapshotAddress getCurrentSelectionSafe() const;
    void setClipboardActive(bool shouldBeActive);
    void updateClipboardButtonsUI(bool lockEdits);

    //==========================================================================
    // 8) Preset navigation UI
    //==========================================================================
    juce::Label bankLabel;
    FlatComboBox bankSelector;
    std::unique_ptr<ShadowComponent> bankSelectorShadow;

    juce::Label presetLabel;
    FlatComboBox presetSelector;
    std::unique_ptr<ShadowComponent> presetSelectorShadow;

    // Snapshot buttons (typically 8 or 16 depending on your design)
    std::vector<std::unique_ptr<juce::ToggleButton>> snapshotButtons;
    std::vector<std::unique_ptr<ShadowComponent>> snapshotShadows;

    // Background area used for paint/layout
    juce::Rectangle<int> headerInfoPillArea;
    juce::Rectangle<int> snapshotBackgroundArea;

    // Enables/disables preset navigation controls (eg while saving or during modal ops)
    void setPresetNavigationEnabled(bool shouldBeEnabled);

    //==========================================================================
    // 9) State trackers (for non-spam logs)
    //==========================================================================
    bool lastMuteState = false;
    int  lastChannel   = -1;
    int  lastGlobalKeyIndex   = -1;
    int  lastGlobalModeIndex  = -1;

    int lastBankIndex     = -1;
    int lastPresetIndex   = -1;
    int lastSnapshotIndex = -1;
    double lastDisplayedBpm = -1.0;
    juce::String lastChordFlowDisplay;

    //==========================================================================
    // 10) Utility methods (UI)
    //==========================================================================
    void refreshBankNames();
    void refreshPresetNames(int bankIndex);
    void updateSnapshotButtons(int bankIndex, int presetIndex);
    void refreshSnapshotNameDisplay(int bankIndex, int presetIndex, int snapshotIndex);
    void showSnapshotContextMenu();
    void requestRenameCurrentSnapshot();
    void showBankContextMenu();
    void showPresetContextMenu();
    void requestRenameCurrentBank();
    void requestRenameCurrentPreset();

    // manual-bound controls must be refreshed explicitly.
    void syncUIFromVTS();
    void syncHeaderFromVTS();   // mute + channel
    void syncTonalityFromVTS(); // global key + mode
    void syncInputRemapModesFromVTS(); // W/B input remap modes
    void applyInputRemapModeInterlock();
    bool isGlobalKeyOffSelected() const noexcept;
    void applyGlobalTonalityLock(bool forceScaleChromaticInParam);
    
    // Adds a component wrapped inside a ShadowComponent (consistent UI style).
    template<typename Comp>
    void addWithShadow(std::unique_ptr<ShadowComponent>& shadowPtr,
                       Comp& comp,
                       const juce::DropShadow& shadow)
    {
        shadowPtr = std::make_unique<ShadowComponent>(shadow,
                                                      -1.0f,
                                                      true, true, true, true,
                                                      10);

        shadowPtr->addAndMakeVisible(comp);
        addAndMakeVisible(*shadowPtr);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainMenu)
};
