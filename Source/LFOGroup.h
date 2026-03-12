/**
 * @file LFOGroup.h
 * @brief UI container for dynamic global LFO lanes (1..8).
 *
 * Architecture role:
 * - Owns editable LFO rows (rate/depth/offset/wave/destinations).
 * - Publishes a compact RT mirror to PluginProcessor atomics.
 * - Restores/saves UI-only row topology for snapshot persistence.
 *
 * Threading:
 * - UI thread only.
 * - Timer-driven visual updates on message thread.
 * - Not RT-safe.
 */
#pragma once

#include <JuceHeader.h>
#include <array>
#include <functional>
#include <utility>
#include <vector>
#include "PluginColours.h"
#include "UiMetrics.h"
#include "0-component/CustomRotaryWithCombo.h"
#include "0-component/FlatComboBox.h"
#include "0-component/LFOComponent.h"

// Forward declaration
class MidivisiViciAudioProcessor;

/**
 * @brief Editor-side group for LFO controls and animated previews.
 *
 * Pattern:
 * - Pattern: Composite
 * - Problem solved: keep each LFO lane visually and behaviorally aligned with
 *   shared host transport timing.
 * - Participants: dynamic knob pairs, destinations, and `LFOComponent` views.
 * - Flow: timer tick polls tempo/phase and pushes visual updates.
 * - Pitfalls: keep phase-lock deterministic across all lanes.
 */
class LFOGroup : public juce::Component,
                 private juce::Timer
{
public:
    explicit LFOGroup(MidivisiViciAudioProcessor& processorIn);
    ~LFOGroup() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    /** @brief Shared UI scheduler entry point for visual LFO updates. */
    void uiTimerTick();
    /**
     * @brief Inject dynamic naming providers used by LFO destination menus.
     *
     * laneLabelProvider:
     * - Input: zero-based lane index.
     * - Output: current lane display name (e.g. "Bass", "Untitled").
     *
     * mainBarLabelProvider:
     * - Input: zero-based MainBar morph page index.
     * - Output: full destination display label for this page.
     *
     * Thread: message thread only.
     *
     * Notes:
     * - Providers are intentionally UI-side only (string/color formatting).
     * - RT routing uses stable integer destination IDs, never display labels.
     */
    void setDestinationLabelProviders(std::function<juce::String(int)> laneLabelProvider,
                                      std::function<juce::String(int)> mainBarLabelProvider,
                                      std::function<std::pair<juce::Colour, juce::Colour>(int)> laneColourProvider = {},
                                      std::function<std::pair<juce::Colour, juce::Colour>(int)> mainBarColourProvider = {});
    /** @brief Mark destination labels dirty so they are rebuilt on next UI tick. */
    void markDestinationLabelsDirty() noexcept;

    // Snapshot UI persistence helpers (editor thread only).
    /** @brief Serialize LFO rows UI state for snapshots. */
    [[nodiscard]] juce::ValueTree getStateTree() const;
    /** @brief Restore LFO rows UI state from snapshots. */
    void applyStateTree(const juce::ValueTree& state);

private:
    void timerCallback() override;
    void resetToDefaultLayout();

    class MiniRoundButton;
    class RowsContent;
    class HorizontalWheelViewport : public juce::Viewport
    {
    public:
        using juce::Viewport::Viewport;

        void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
        {
            // Force horizontal wheel handling even when the target is a child component.
            if (!useMouseWheelMoveIfNeeded(e, wheel))
                juce::Viewport::mouseWheelMove(e, wheel);
        }
    };

    struct LfoRow
    {
        std::unique_ptr<CustomRotaryWithCombo> rateKnob;
        std::unique_ptr<CustomRotaryWithCombo> depthKnob;
        std::unique_ptr<FlatComboBox>          destinationBox;
        std::unique_ptr<juce::Slider>          offsetSlider;
        std::unique_ptr<juce::Label>           offsetLabel;
        std::unique_ptr<LFOComponent>          visual;
        std::array<std::unique_ptr<MiniRoundButton>, 5> waveButtons;
        int waveShapeIndex = 0;

        std::unique_ptr<MiniRoundButton> collapseButton;
        std::unique_ptr<MiniRoundButton> orderButton;
        std::unique_ptr<MiniRoundButton> moveLeftButton;
        std::unique_ptr<MiniRoundButton> moveRightButton;
        std::unique_ptr<MiniRoundButton> closeButton;
        std::unique_ptr<MiniRoundButton> addButton;
        std::unique_ptr<MiniRoundButton> duplicateButton;

        bool collapsed = false;
        juce::Rectangle<int> lastBounds;
        // Runtime payload for this row: selected stable destination IDs.
        // "None" is not stored here (empty vector means no effective target).
        std::vector<int> destinationStableIds;
    };

    struct DestinationTarget
    {
        int stableId = 1;
        juce::String label;
        juce::Colour background = PluginColours::primary;
        juce::Colour foreground = PluginColours::onPrimary;
    };

    std::unique_ptr<LfoRow> createDefaultRow();
    void insertDefaultRowAfter(int rowIndex);
    void duplicateRowAfter(int rowIndex);
    void removeRow(int rowIndex);
    void moveRowLeft(int rowIndex);
    void moveRowRight(int rowIndex);
    void toggleRowCollapsed(int rowIndex);

    void refreshRowButtons();
    // Keep phase/depth/rate cache vectors aligned with current row count.
    void refreshRuntimeCaches();
    void applyRowWaveShape(LfoRow& row, int shapeIndex);
    void rebuildDestinationChoicesIfNeeded();
    void rebuildDestinationChoices();
    const DestinationTarget* findDestinationTarget(int stableId) const noexcept;
    void updateRowDestinationBinding(LfoRow& row, const juce::Array<int>& stableIds);
    // Serialize all row configs to processor runtime cache.
    void pushRtLfoConfigToProcessor() noexcept;
    float evaluateWaveSample(const LfoRow& row, float phase01InCycle) const noexcept;
    void layoutRow(LfoRow& row, const juce::Rectangle<int>& bounds);

    MidivisiViciAudioProcessor& processor;

    static constexpr int kMinLfoCount = 1;
    static constexpr int kMaxLfoCount = 8;
    static constexpr int kDefaultLfoCount = 4;
    static constexpr int kLfoCardWidth = UiMetrics::kModuleWidth;
    std::vector<std::unique_ptr<LfoRow>> lfoRows;
    // Live destination catalog rebuilt from topology (active lanes + MainBar pages).
    std::vector<DestinationTarget> destinationTargets;
    int cachedDestinationLaneCount = -1;
    int cachedDestinationMainBarPageCount = -1;
    std::function<juce::String(int)> laneDestinationLabelProvider;
    std::function<juce::String(int)> mainBarDestinationLabelProvider;
    std::function<std::pair<juce::Colour, juce::Colour>(int)> laneDestinationColourProvider;
    std::function<std::pair<juce::Colour, juce::Colour>(int)> mainBarDestinationColourProvider;
    bool destinationLabelsDirty = true;
    std::vector<juce::Rectangle<float>> expandedCardBounds;
    std::unique_ptr<RowsContent> rowsContent;
    HorizontalWheelViewport rowsViewport;

    bool wasPlaying = false;

    // Per-row phase cache in [0..1), reused by timer to avoid recomputation.
    std::vector<double> phase01; // phase normalisee par LFO [0..1)
    std::vector<int> lastRateIndexSent;
    std::vector<float> lastDepthSent;
    double lastBpmSent = -1.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LFOGroup)
};
