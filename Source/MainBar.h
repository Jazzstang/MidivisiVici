/**
 * @file MainBar.h
 * @brief MainBar with centralized CC sequencer (left) and morph modules (right).
 *
 * Architecture role:
 * - UI source-of-truth for MainBar pages (`morphPages`).
 * - Publishes compact runtime page snapshots to processor atomics.
 * - Exposes dynamic destination labels/colours for LFO multi-destination menus.
 *
 * Threading:
 * - UI thread only for this component.
 * - Sends RT-safe config snapshots to processor via atomics.
 */
#pragma once

#include <JuceHeader.h>

#include <array>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "PluginColours.h"

class MidivisiViciAudioProcessor;
class StepToggle;

class MainBar : public juce::Component
{
public:
    explicit MainBar(MidivisiViciAudioProcessor& processorIn);
    ~MainBar() override;

    /**
     * @brief Set explicit width of left sequencer module.
     *
     * The remaining width is used by right modules viewport-like strip.
     */
    void setLeftModuleWidth(int widthPx);

    /** @brief Shared UI scheduler entry point. */
    void uiTimerTick();
    /** @brief Set callback fired when destination naming/topology changes for LFO menus. */
    void setOnDestinationNamesChanged(std::function<void()> cb);
    /**
     * @brief Build dynamic LFO destination label for one MainBar morph page.
     *
     * Format: "<controllerIndex> <controllerName> <morphName>"
     * Example: "2 BassCtrl Cutoff".
     */
    [[nodiscard]] juce::String getLfoDestinationLabelForPage(int pageIndex) const;
    /** @brief Build destination color pair for one MainBar morph page. */
    [[nodiscard]] std::pair<juce::Colour, juce::Colour> getLfoDestinationColoursForPage(int pageIndex) const;

    /** @brief Serialize MainBar UI model for snapshot persistence. */
    [[nodiscard]] juce::ValueTree getStateTree() const;

    /**
     * @brief Restore MainBar UI model from snapshot state.
     *
     * Supports partial/older trees by filling missing pages with defaults.
     */
    void applyStateTree(const juce::ValueTree& state);

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    class MiniRoundButton;
    class NamePill;
    class MorphSlider;

    static constexpr int kMinRightModuleCount = 1;
    static constexpr int kDefaultRightModuleCount = 4;
    static constexpr int kMaxRightModuleCount = 16;
    static constexpr int kMorphPerModule = 8;
    static constexpr int kStepsPerPage = 16;

    struct MorphControl
    {
        std::unique_ptr<MorphSlider> knob;
        std::unique_ptr<NamePill> labelPill;
    };

    struct RightModule
    {
        std::unique_ptr<MiniRoundButton> collapseButton;
        std::unique_ptr<MiniRoundButton> closeButton;
        std::unique_ptr<MiniRoundButton> addButton;
        std::unique_ptr<MiniRoundButton> duplicateButton;
        std::unique_ptr<NamePill> titlePill;
        std::array<MorphControl, kMorphPerModule> morphControls;

        bool collapsed = false;
        juce::String title;
        int colourIndex = -1;
        bool userNamed = false;
        juce::File loadedPresetFile;
        juce::String loadedPresetName;

        ~RightModule();
    };

    struct MorphPageState
    {
        // Editable morph metadata shown on right modules.
        juce::String name;
        int channel = 1;
        int ccNumber = 1;
        int morphValue = 50;

        // Left-side sequencer payload for this morph page.
        // Step 0 must remain non-skip (rate default 1/16 index = 5).
        std::array<int, kStepsPerPage> stepValues {};
        std::array<int, kStepsPerPage> stepRates {};
    };

    std::unique_ptr<RightModule> createRightModule(int moduleIndex);
    void ensureDefaultRightModules();
    void refreshRightModuleCallbacks();
    void rebuildLayoutCache();
    void layoutLeftModule();
    void layoutRightModule(RightModule& module,
                           int moduleIndex,
                           const juce::Rectangle<int>& slotBounds,
                           const juce::Rectangle<int>& contentBounds);

    int getPageIndexFor(int moduleIndex, int morphIndex) const noexcept;
    void setSelectedPageIndex(int newIndex, bool syncUi);
    void syncSelectedPageToControls();
    void updateStepValueEnablement();
    void updateCcSequencerKnobColours();

    // Publish all pages after topology load/reset.
    // UI thread only; processor consumes via lock-free atomics.
    void pushAllPagesToProcessor();
    void pushPageToProcessor(int pageIndex);
    void pushSelectedPageToProcessor();

    void insertModuleAfter(int moduleIndex, bool duplicate);
    void removeModule(int moduleIndex);
    void toggleModuleCollapsed(int moduleIndex);
    void showModuleRenameDialog(int moduleIndex);
    void showModuleNameContextMenu(int moduleIndex, juce::Component& target);
    void showSaveModulePresetDialog(int moduleIndex);
    bool saveLoadedModulePreset(int moduleIndex);
    void showEditModulePresetDialog(int moduleIndex);
    [[nodiscard]] juce::ValueTree captureModulePresetState(int moduleIndex) const;
    void applyModulePresetState(int moduleIndex, const juce::ValueTree& state);
    void showMorphEditDialog(int pageIndex);
    void showMorphContextMenu(int pageIndex, juce::Component& target);
    int pickUniqueRandomModuleColour(int moduleIndex) const noexcept;
    void notifyDestinationNamesChanged();

    MidivisiViciAudioProcessor& processor;

    int leftModuleWidthPx = 0;
    juce::Rectangle<int> leftSlotBounds;
    juce::Rectangle<int> leftModuleBounds;

    std::vector<std::unique_ptr<RightModule>> rightModules;
    std::vector<juce::Rectangle<int>> rightModuleBounds;
    std::vector<MorphPageState> morphPages;

    // Currently edited morph page.
    int selectedPageIndex = 0;
    int lastPlaybackPageIndex = -1;
    int lastPlaybackStepIndex = -1;
    bool suppressUiCallbacks = false;

    std::array<std::unique_ptr<MorphSlider>, kStepsPerPage> stepValueKnobs;
    std::array<std::unique_ptr<StepToggle>, kStepsPerPage> rateSteps;
    std::function<void()> onDestinationNamesChanged;
};
