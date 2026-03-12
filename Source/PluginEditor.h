/**
 * @file PluginEditor.h
 * @brief Top-level plugin editor composition and UI scheduler entry point.
 */
//==============================================================================
// PluginEditor.h
//
// MidivisiVici - Main Plugin Editor
//
// PEDAGOGICAL OVERVIEW
// --------------------
// This class is the top-level UI container of the plugin.
//
// It owns and arranges:
//   - The top row (logo, MainMenu, LFOGroup),
//   - The center grid (InputMonitor + lane menu band + lane modules + shared OutputMonitor),
//   - The bottom row (MainBar),
//   - Side MIDI monitors (input + per-lane output modules).
//
// The editor also draws the global background (theme fill + vignette) and can
// overlay a subtle noise texture for a more organic look.
//
// TIMER ROLE (UI THREAD)
// ----------------------
// A Timer tick is used as a lightweight "refresh clock" for visuals that may
// evolve over time (e.g. monitors, cached noise on resize).
//
// The Timer runs on the message thread. It must remain UI-only:
//   - Do not touch audio data structures that are not thread-safe.
//   - Reading APVTS atomics for simple booleans is OK.
//
// IMPORTANT: MINIMIZE REPAINT COST
// -------------------------------
// The corresponding .cpp should avoid repainting the full editor at every tick.
// The intended pattern is:
//   - recompute or update only when something actually changed,
//   - call repaint() only when needed.
//
//==============================================================================

#pragma once

#include <JuceHeader.h>
#include <vector>
#include <array>
#include <map>

#include "PluginProcessor.h"
#include "PluginColours.h"
#include "PluginLookAndFeel.h"
#include "ChordDetection.h"
#include "BinaryData.h"

// UI top / helpers
#include "MainMenu.h"
#include "MainBar.h"
#include "LFOGroup.h"
#include "0-component/ShadowComponent.h"

// Monitors
#include "1-midiMonitor/MidiMonitor.h"
#include "1-midiMonitor/InputMonitor.h"
#include "1-midiMonitor/OutputMonitor.h"

// Modules
#include "2-inputFilter/InputFilter.h"
#include "3-harmonizer/Harmonizer.h"
#include "4-arpeggiator/Arpeggiator.h"
#include "5-splitter/Splitter.h"

//==============================================================================
// MidivisiViciAudioProcessorEditor
//==============================================================================
/**
 * @brief Top-level editor that owns and schedules all plugin UI modules.
 *
 * Pattern:
 * - Pattern: Composite + Central UI Scheduler
 * - Problem solved: keep UI responsive by driving module refresh from one
 *   message-thread clock instead of many independent timers.
 * - Participants:
 *   - MidivisiViciAudioProcessorEditor: layout + scheduler authority.
 *   - Child modules: monitors, lane UIs, menu/bar, LFO group.
 *   - PluginLookAndFeel: shared styling for all children.
 * - Flow:
 *   1. timerCallback() runs at 60 Hz base clock.
 *   2. Sub-rates (30/15 Hz) dispatch to less critical UI tasks.
 *   3. Repaint is triggered only when observed states changed.
 * - Pitfalls:
 *   - Message-thread overload if tick handler does too much per frame.
 *   - Never touch non-thread-safe audio structures from UI callbacks.
 */

class MidivisiViciAudioProcessorEditor
    : public juce::AudioProcessorEditor,
      private juce::Timer
{
public:
    explicit MidivisiViciAudioProcessorEditor(MidivisiViciAudioProcessor&);
    ~MidivisiViciAudioProcessorEditor() override;

    //==========================================================================
    // JUCE overrides
    //==========================================================================
    /**
     * @brief Paint the root editor background and static visuals.
     *
     * Thread: message thread.
     * RT-safe: not applicable.
     */
    void paint(juce::Graphics&) override;

    /**
     * @brief Layout all child components (top row, grid, footer).
     *
     * Thread: message thread.
     * RT-safe: not applicable.
     */
    void resized() override;

private:
    //==========================================================================
    // Processor reference
    //
    // The editor needs access to:
    //   - APVTS (for monitors state polling),
    //   - FIFO sources wiring for the MIDI monitors,
    //   - global objects exposed by the processor.
    //==========================================================================
    MidivisiViciAudioProcessor& processor;

    //==========================================================================
    // Global LookAndFeel
    //
    // Owned by the editor so all children can inherit consistent styling.
    //==========================================================================
    PluginLookAndFeel lookAndFeel;

    //==========================================================================
    // Background visuals (logo)
    //==========================================================================
    std::unique_ptr<juce::Drawable> mdvzLogo;
    juce::Rectangle<int> logoBounds;

    /**
     * @brief Central UI scheduler tick.
     *
     * Thread: message thread.
     * RT-safe: not applicable.
     *
     * Dispatches module uiTimerTick() at 60/30/15 Hz tiers.
     */
    void timerCallback() override;
    uint32_t uiSchedulerTickCounter = 0;

    // Cache for monitor filter states (to avoid redundant LookAndFeel updates).
    bool lastInNotes    = false;
    bool lastInControls = false;
    bool lastInClock    = false;
    bool lastInEvents   = false;

    bool lastOutNotes    = false;
    bool lastOutControls = false;
    bool lastOutClock    = false;
    bool lastOutEvents   = false;

    // Cached APVTS raw pointers for monitor bool polling.
    std::atomic<float>* inNotesRaw = nullptr;
    std::atomic<float>* inControlsRaw = nullptr;
    std::atomic<float>* inClockRaw = nullptr;
    std::atomic<float>* inEventsRaw = nullptr;
    std::atomic<float>* outNotesRaw = nullptr;
    std::atomic<float>* outControlsRaw = nullptr;
    std::atomic<float>* outClockRaw = nullptr;
    std::atomic<float>* outEventsRaw = nullptr;
    std::atomic<float>* harmGlobalKeyRaw = nullptr;
    std::atomic<float>* harmGlobalScaleRaw = nullptr;
    uint32_t lastChordSnapshotRevision = 0;

    // Top action column state cache to avoid redundant button updates.
    bool topActionStateInitialized = false;
    SaveState lastTopSaveState = SaveState::Idle;
    bool lastTopClipboardActive = false;

    //==========================================================================
    // Top / Bottom UI
    //==========================================================================
    MainMenu mainMenu;
    LFOGroup lfoGroup;
    MainBar  mainBar;

    // Top action column (between logo and MainMenu), aligned to lane menu width.
    juce::DrawableButton topCopyButton   { "TopCopy", juce::DrawableButton::ImageFitted };
    juce::DrawableButton topPasteButton  { "TopPaste", juce::DrawableButton::ImageFitted };
    juce::DrawableButton topDeleteButton { "TopDelete", juce::DrawableButton::ImageFitted };
    juce::DrawableButton topSaveButton   { "TopSave", juce::DrawableButton::ImageFitted };
    void refreshTopActionButtons();

    //==========================================================================
    // Side monitor
    //==========================================================================
    InputMonitor  inputMonitor;
    OutputMonitor outputMonitor;

    //==========================================================================
    // Dynamic lanes UI (1..16)
    //==========================================================================
    class LaneMenuComponent;

    struct LaneComponents
    {
        std::unique_ptr<InputFilter> inputFilter;
        std::unique_ptr<Harmonizer> harmonizer;
        std::unique_ptr<Arpeggiator> arpeggiator;
        std::unique_ptr<Splitter> splitter;
    };

    struct LaneUiState
    {
        juce::String name { "Untitled" };
        int colourIndex = -1; // -1: auto-assign unique random, 0..15: explicit palette
        bool collapsed = false;
    };

    juce::Viewport lanesViewport;
    juce::Component lanesContent;

    std::vector<LaneComponents> laneComponents;
    std::vector<std::unique_ptr<LaneMenuComponent>> laneMenus;
    std::array<LaneUiState, Lanes::kNumLanes> laneUiStates {};

    std::array<std::map<juce::String, juce::RangedAudioParameter*>, Lanes::kNumLanes> laneParamMaps {};
    bool laneParamMapsBuilt = false;

    int activeLaneCount = 1;

    void rebuildLaneParameterMaps();
    void copyLaneParameters(int srcIndex, int dstIndex);
    void resetLaneParametersToDefault(int laneIndex);
    void swapLaneParameters(int laneA, int laneB);

    void insertLaneAfter(int laneIndex, bool duplicateSource);
    void removeLaneAt(int laneIndex);
    void moveLaneUp(int laneIndex);
    void moveLaneDown(int laneIndex);

    void showLaneNameContextMenu(int laneIndex, juce::Component& target);
    void showLaneRenameDialog(int laneIndex);
    int pickUniqueRandomLaneColour(int laneIndex) const noexcept;

    int getLaneHeight(int laneIndex) const noexcept;
    int getTotalLaneContentHeight() const noexcept;
    int getVisibleLaneAreaHeight() const noexcept;

    void ensureLaneComponentsCreated(int laneCount);
    void applyActiveLaneCountFromProcessor(bool resizeEditor, bool scrollToBottom = false);

    // Snapshot UI persistence (lanes + LFO layout/meta).
    [[nodiscard]] juce::ValueTree createSnapshotUiStateTree() const;
    void applySnapshotUiStateTree(const juce::ValueTree& state);

   #if JUCE_DEBUG
    //==========================================================================
    // Debug-only UI toggles
    //
    // These are editor-side toggles intended to enable/disable DBG_LOG groups.
    // They should not allocate or do heavy work in callbacks.
    //==========================================================================
    juce::ToggleButton logInputFilterProcessorToggle { "InputFilterProcessor" };
    juce::ToggleButton logHarmonizerToggle          { "Harmonizer" };
    juce::ToggleButton logSplitterToggle            { "Splitter" };
    juce::ToggleButton logPluginProcessorToggle     { "PluginProcessor" };
    juce::ToggleButton logPluginEditorToggle        { "PluginEditor" };
   #endif

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidivisiViciAudioProcessorEditor)
};
