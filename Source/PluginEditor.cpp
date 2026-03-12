//==============================================================================
// PluginEditor.cpp
//
// MidivisiVici - Main Editor
//
// PEDAGOGICAL OVERVIEW
// --------------------
// This file implements the top-level JUCE editor that hosts and arranges all UI
// modules (menus, monitors, and dynamic lane processor rows).
//
// The editor also draws the global background (vignette + subtle noise overlay)
// and a vector logo.
//
// RUNTIME MODEL
// -------------
// The editor itself does not process audio/MIDI. It is a pure UI object living
// on the message thread.
//
// A Timer tick at 30 Hz is used for "meter-like" UI that needs periodic refresh
// (monitors, animation, etc.). To keep CPU usage reasonable, repaint() is only
// called when something actually changed (monitor filter states or noise cache).
//
// PARAMETER ACCESS
// ----------------
// For fast UI polling of a few booleans, we read APVTS parameters with
// getRawParameterValue(), which returns an atomic<float>*.
// This is safe to read from the message thread without locks.
//
// IMPORTANT: getRawParameterValue() returns the *current value as float*.
// For AudioParameterBool this is 0/1, which is perfect for "load() > 0.5".
// For other parameter types (int/choice), prefer parameter mapping helpers
// (convertFrom0to1 / convertTo0to1) like we do in module UIs.
//
//==============================================================================

#include "PluginEditor.h"
#include "UiMetrics.h"
#include <cmath>

namespace
{
    //--------------------------------------------------------------------------
    // Drawable helpers
    //
    // The logo is loaded as a Drawable from embedded SVG data. We:
    //   - extract a path outline (for shadows),
    //   - tint it to match the global theme.
    //--------------------------------------------------------------------------

    static void tintDrawable(juce::Drawable& d, juce::Colour c)
    {
        if (auto* dp = dynamic_cast<juce::DrawablePath*>(&d))
        {
            dp->setFill(juce::FillType(c));

            if (dp->getStrokeType().getStrokeThickness() > 0.0f)
                dp->setStrokeFill(juce::FillType(c));

            return;
        }

        if (auto* comp = dynamic_cast<juce::DrawableComposite*>(&d))
        {
            const int n = comp->getNumChildComponents();
            for (int i = 0; i < n; ++i)
            {
                if (auto* childComp = comp->getChildComponent(i))
                {
                    if (auto* child = dynamic_cast<juce::Drawable*>(childComp))
                        tintDrawable(*child, c);
                }
            }
            return;
        }
    }

    static std::unique_ptr<juce::Drawable> loadSvgFromBinaryData(const void* data, int size)
    {
        if (data == nullptr || size <= 0)
            return {};

        auto xml = juce::parseXML(juce::String::fromUTF8((const char*) data, size));
        if (xml == nullptr)
            return {};

        return juce::Drawable::createFromSVG(*xml);
    }

    //--------------------------------------------------------------------------
    // Layout constants
    //--------------------------------------------------------------------------

    constexpr int kColumnWidth = UiMetrics::kModuleWidth; // module columns
    constexpr int kLaneMenuWidth = 36;           // lane menu/top-actions band (reduced side margins)
    constexpr int kModuleInstanceHeight = 350;
    constexpr int kCollapsedLaneHeight = 34;
    constexpr int kHeaderHeight = 152;
    constexpr int kFooterHeight = kModuleInstanceHeight / 3; // 116
    constexpr int kLaneModuleColumns = 4; // inputFilter/harmonizer/arpeggiator/splitter
    constexpr int kSharedOutputColumns = 1; // shared output monitor
    constexpr int kEditorWidth = kColumnWidth + kLaneMenuWidth
                               + ((kLaneModuleColumns + kSharedOutputColumns) * kColumnWidth);

    //--------------------------------------------------------------------------
    // Safe bool param read (APVTS -> atomic<float>)
    //--------------------------------------------------------------------------

    static bool readRawBool(const std::atomic<float>* raw, bool fallback = false) noexcept
    {
        return raw != nullptr ? (raw->load() > 0.5f) : fallback;
    }

    static std::pair<juce::Colour, juce::Colour> getLaneNameColours(int colourIndex)
    {
        if (colourIndex < 0 || colourIndex >= 16)
            return { PluginColours::primary, PluginColours::onPrimary };

        return PluginColours::getIndexedNameColours(colourIndex);
    }

    static inline void setParameterNormalized(juce::RangedAudioParameter& p, float normalizedValue)
    {
        const float clamped = juce::jlimit(0.0f, 1.0f, normalizedValue);
        if (std::abs(p.getValue() - clamped) <= 1.0e-6f)
            return;

        p.beginChangeGesture();
        p.setValueNotifyingHost(clamped);
        p.endChangeGesture();
    }

    static constexpr int kLaneMenuActionCount = 5; // up/down/delete/add/duplicate

    static const juce::Identifier kUiStateNodeID          { "UI_STATE" };
    static const juce::Identifier kUiActiveLaneCountPropID{ "activeLaneCount" };
    static const juce::Identifier kUiLanesNodeID          { "LANES" };
    static const juce::Identifier kUiLaneNodeID           { "LANE" };
    static const juce::Identifier kUiLaneIndexPropID      { "index" };
    static const juce::Identifier kUiLaneNamePropID       { "laneName" };
    static const juce::Identifier kUiLaneColourPropID     { "laneColourIndex" };
    static const juce::Identifier kUiLaneCollapsedPropID  { "laneCollapsed" };

    static const char* laneColourLabel(int idx)
    {
        static constexpr const char* labels[16] =
        {
            "Lay 1", "Lay 2", "Lay 3", "Lay 4", "Lay 5", "Lay 6", "Lay 7", "Lay 8",
            "OnLay 1", "OnLay 2", "OnLay 3", "OnLay 4", "OnLay 5", "OnLay 6", "OnLay 7", "OnLay 8"
        };
        return labels[(size_t) juce::jlimit(0, 15, idx)];
    }
}

class MidivisiViciAudioProcessorEditor::LaneMenuComponent : public juce::Component
{
public:
    struct Callbacks
    {
        std::function<void()> onDuplicate;
        std::function<void()> onInsert;
        std::function<void()> onDelete;
        std::function<void()> onMoveDown;
        std::function<void()> onMoveUp;
        std::function<void()> onToggleCollapse;
        std::function<void()> onNameContext;
    };

    LaneMenuComponent()
        : collapseButton("-"),
          duplicateButton("D"),
          insertButton("+"),
          deleteButton("X"),
          downButton("v"),
          upButton("^")
    {
        addAndMakeVisible(collapseButton);
        addAndMakeVisible(duplicateButton);
        addAndMakeVisible(insertButton);
        addAndMakeVisible(deleteButton);
        addAndMakeVisible(downButton);
        addAndMakeVisible(upButton);
        addAndMakeVisible(namePill);

        collapseButton.onClick = [this] { if (callbacks.onToggleCollapse) callbacks.onToggleCollapse(); };
        duplicateButton.onClick = [this] { if (callbacks.onDuplicate) callbacks.onDuplicate(); };
        insertButton.onClick = [this] { if (callbacks.onInsert) callbacks.onInsert(); };
        deleteButton.onClick = [this] { if (callbacks.onDelete) callbacks.onDelete(); };
        downButton.onClick = [this] { if (callbacks.onMoveDown) callbacks.onMoveDown(); };
        upButton.onClick = [this] { if (callbacks.onMoveUp) callbacks.onMoveUp(); };
        namePill.onContext = [this] { if (callbacks.onNameContext) callbacks.onNameContext(); };
    }

    void setCallbacks(Callbacks cb)
    {
        callbacks = std::move(cb);
    }

    void setLaneNumber(int n)
    {
        laneNumber = juce::jmax(1, n);
        repaint();
    }

    void setCollapsed(bool shouldCollapse)
    {
        collapsed = shouldCollapse;
        collapseButton.setSquareMode(false);
        collapseButton.setGlyph("-");
        namePill.setVertical(!collapsed);
        resized();
        repaint();
    }

    void setLaneName(const juce::String& n)
    {
        namePill.setText(n);
    }

    void setNameColours(juce::Colour bg, juce::Colour fg)
    {
        namePill.setColours(bg, fg);
    }

    void setCanMoveUp(bool canMove)
    {
        upButton.setEnabled(canMove);
    }

    void setCanMoveDown(bool canMove)
    {
        downButton.setEnabled(canMove);
    }

    void setCanDelete(bool canDelete)
    {
        deleteButton.setEnabled(canDelete);
    }

    void setCanInsertOrDuplicate(bool canInsert)
    {
        insertButton.setVisible(canInsert);
        insertButton.setEnabled(canInsert);
        duplicateButton.setEnabled(canInsert);
    }

    void paint(juce::Graphics& g) override
    {
        const auto badgeArea = numberBadgeBounds.toFloat().reduced(1.5f);
        g.setColour(PluginColours::secondary);
        g.fillEllipse(badgeArea);
        g.setColour(PluginColours::onSecondary);
        g.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
        g.drawFittedText(juce::String(laneNumber), numberBadgeBounds, juce::Justification::centred, 1);
    }

    void resized() override
    {
        constexpr int kButtonSize = 24;
        constexpr int kGap = 4;
        constexpr int kSideMargin = 0;
        constexpr int kCollapsedTopBottomMargin = UiMetrics::kModuleInnerMargin * 2;
        constexpr int kExpandedTopBottomMargin = UiMetrics::kModuleInnerMargin * 2;

        auto area = getLocalBounds();

        if (collapsed)
        {
            area = area.withTrimmedTop(kCollapsedTopBottomMargin)
                       .withTrimmedBottom(kCollapsedTopBottomMargin)
                       .withTrimmedLeft(kSideMargin)
                       .withTrimmedRight(kSideMargin);
            auto row = area;

            auto placeSquareFromLeft = [&](juce::Rectangle<int> slot, RoundButton& button)
            {
                button.setBounds(slot.withSizeKeepingCentre(kButtonSize, kButtonSize));
            };

            auto collapseSlot = row.removeFromLeft(kButtonSize);
            placeSquareFromLeft(collapseSlot, collapseButton);
            row.removeFromLeft(kGap);

            auto numberSlot = row.removeFromLeft(kButtonSize);
            numberBadgeBounds = numberSlot.withSizeKeepingCentre(kButtonSize, kButtonSize);
            row.removeFromLeft(kGap);

            const int actionWidth = (kLaneMenuActionCount * kButtonSize) + ((kLaneMenuActionCount - 1) * kGap);
            auto actions = row.removeFromRight(actionWidth);

            auto upSlot = actions.removeFromLeft(kButtonSize);
            placeSquareFromLeft(upSlot, upButton);
            actions.removeFromLeft(kGap);

            auto downSlot = actions.removeFromLeft(kButtonSize);
            placeSquareFromLeft(downSlot, downButton);
            actions.removeFromLeft(kGap);

            auto deleteSlot = actions.removeFromLeft(kButtonSize);
            placeSquareFromLeft(deleteSlot, deleteButton);
            actions.removeFromLeft(kGap);

            auto insertSlot = actions.removeFromLeft(kButtonSize);
            placeSquareFromLeft(insertSlot, insertButton);
            actions.removeFromLeft(kGap);

            auto duplicateSlot = actions.removeFromLeft(kButtonSize);
            placeSquareFromLeft(duplicateSlot, duplicateButton);

            namePill.setBounds(row.reduced(0, 1));
            return;
        }

        area = area.withTrimmedTop(kExpandedTopBottomMargin)
                   .withTrimmedBottom(kExpandedTopBottomMargin)
                   .withTrimmedLeft(kSideMargin)
                   .withTrimmedRight(kSideMargin);

        auto collapseStrip = area.removeFromTop(kButtonSize);
        collapseButton.setBounds(collapseStrip.withSizeKeepingCentre(kButtonSize, kButtonSize));
        area.removeFromTop(kGap);

        auto bottomStack = area.removeFromBottom((6 * kButtonSize) + (5 * kGap));

        auto numberStrip = bottomStack.removeFromTop(kButtonSize);
        numberBadgeBounds = numberStrip.withSizeKeepingCentre(kButtonSize, kButtonSize);
        bottomStack.removeFromTop(kGap);

        auto upStrip = bottomStack.removeFromTop(kButtonSize);
        upButton.setBounds(upStrip.withSizeKeepingCentre(kButtonSize, kButtonSize));
        bottomStack.removeFromTop(kGap);

        auto downStrip = bottomStack.removeFromTop(kButtonSize);
        downButton.setBounds(downStrip.withSizeKeepingCentre(kButtonSize, kButtonSize));
        bottomStack.removeFromTop(kGap);

        auto deleteStrip = bottomStack.removeFromTop(kButtonSize);
        deleteButton.setBounds(deleteStrip.withSizeKeepingCentre(kButtonSize, kButtonSize));
        bottomStack.removeFromTop(kGap);

        auto insertStrip = bottomStack.removeFromTop(kButtonSize);
        insertButton.setBounds(insertStrip.withSizeKeepingCentre(kButtonSize, kButtonSize));
        bottomStack.removeFromTop(kGap);

        auto duplicateStrip = bottomStack.removeFromTop(kButtonSize);
        duplicateButton.setBounds(duplicateStrip.withSizeKeepingCentre(kButtonSize, kButtonSize));

        const int verticalNameHeight = juce::jmax(1, area.getHeight() - 4);
        namePill.setBounds(area.withSizeKeepingCentre(kButtonSize, verticalNameHeight));
    }

private:
    class RoundButton : public juce::Button
    {
    public:
        explicit RoundButton(juce::String glyphText)
            : juce::Button(glyphText), glyph(std::move(glyphText))
        {
            setMouseCursor(juce::MouseCursor::PointingHandCursor);
        }

        void setGlyph(juce::String glyphText)
        {
            glyph = std::move(glyphText);
            repaint();
        }

        void setSquareMode(bool enabled)
        {
            squareMode = enabled;
            repaint();
        }

        void paintButton(juce::Graphics& g, bool isMouseOver, bool isButtonDown) override
        {
            auto bounds = getLocalBounds().toFloat().reduced(1.0f);
            const auto side = juce::jmin(bounds.getWidth(), bounds.getHeight());
            bounds = bounds.withSizeKeepingCentre(side, side);
            auto bg = isEnabled() ? PluginColours::primary : PluginColours::disabled;
            auto fg = isEnabled() ? PluginColours::onPrimary : PluginColours::onDisabled;

            if (isMouseOver && isEnabled())
                bg = bg.brighter(0.06f);
            if (isButtonDown && isEnabled())
                bg = PluginColours::pressed;

            g.setColour(bg);
            const float corner = squareMode ? 6.0f : (bounds.getHeight() * 0.5f);
            g.fillRoundedRectangle(bounds, corner);

            g.setColour(fg);
            g.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
            g.drawFittedText(glyph, getLocalBounds(), juce::Justification::centred, 1);
        }

    private:
        juce::String glyph;
        bool squareMode = false;
    };

    class NamePill : public juce::Component
    {
    public:
        std::function<void()> onContext;

        void setText(const juce::String& t)
        {
            text = t.isNotEmpty() ? t : "Untitled";
            repaint();
        }

        void setColours(juce::Colour bg, juce::Colour fg)
        {
            background = bg;
            foreground = fg;
            repaint();
        }

        void setVertical(bool shouldBeVertical)
        {
            vertical = shouldBeVertical;
            repaint();
        }

        void paint(juce::Graphics& g) override
        {
            const auto local = getLocalBounds();
            const auto b = local.toFloat().reduced(1.0f);
            g.setColour(background);
            g.fillRoundedRectangle(b, 8.0f);

            g.setColour(foreground);
            g.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));

            if (vertical)
            {
                juce::Graphics::ScopedSaveState s(g);
                const auto c = b.getCentre();
                g.addTransform(juce::AffineTransform::rotation(-juce::MathConstants<float>::halfPi, c.x, c.y));
                auto rotatedTextBounds = juce::Rectangle<int>(0, 0, local.getHeight(), local.getWidth());
                rotatedTextBounds.setCentre(local.getCentre());
                g.drawText(text, rotatedTextBounds, juce::Justification::centred, false);
                return;
            }

            g.drawText(text, local.reduced(6, 0), juce::Justification::centredLeft, false);
        }

        void mouseUp(const juce::MouseEvent& e) override
        {
            if (e.mods.isPopupMenu() && onContext)
                onContext();
        }

    private:
        juce::String text { "Untitled" };
        juce::Colour background { PluginColours::primary };
        juce::Colour foreground { PluginColours::onPrimary };
        bool vertical = true;
    };

    Callbacks callbacks;
    int laneNumber = 1;
    bool collapsed = false;
    juce::Rectangle<int> numberBadgeBounds;

    RoundButton collapseButton;
    RoundButton duplicateButton;
    RoundButton insertButton;
    RoundButton deleteButton;
    RoundButton downButton;
    RoundButton upButton;
    NamePill namePill;
};

//==============================================================================
// Ctor / dtor
//==============================================================================

MidivisiViciAudioProcessorEditor::MidivisiViciAudioProcessorEditor(MidivisiViciAudioProcessor& p)
    : juce::AudioProcessorEditor(p)
    , processor(p)
    , mainMenu(p.getValueTreeState(), p.getPresetManager())
    , lfoGroup(p)
    , mainBar(p)
    , inputMonitor(p.getValueTreeState())
    , outputMonitor(p.getValueTreeState(), "Output Monitor")
{
    setLookAndFeel(&lookAndFeel);
    startTimerHz(60);
    addAndMakeVisible(mainMenu);
    addAndMakeVisible(lfoGroup);
    addAndMakeVisible(mainBar);
    mainMenu.setDisplayedBpm(processor.getBpm());
    lfoGroup.setDestinationLabelProviders(
        [this](int laneIndex) -> juce::String
        {
            if (!juce::isPositiveAndBelow(laneIndex, activeLaneCount))
                return "Untitled";
            return laneUiStates[(size_t) laneIndex].name;
        },
        [this](int pageIndex) -> juce::String
        {
            return mainBar.getLfoDestinationLabelForPage(pageIndex);
        },
        [this](int laneIndex) -> std::pair<juce::Colour, juce::Colour>
        {
            if (!juce::isPositiveAndBelow(laneIndex, activeLaneCount))
                return { PluginColours::primary, PluginColours::onPrimary };
            return getLaneNameColours(laneUiStates[(size_t) laneIndex].colourIndex);
        },
        [this](int pageIndex) -> std::pair<juce::Colour, juce::Colour>
        {
            return mainBar.getLfoDestinationColoursForPage(pageIndex);
        });
    mainBar.setOnDestinationNamesChanged([this]
    {
        lfoGroup.markDestinationLabelsDirty();
    });

    auto& vts = processor.getValueTreeState();
    inNotesRaw = vts.getRawParameterValue(ParamIDs::Monitor::inputMonitorFilterNote);
    inControlsRaw = vts.getRawParameterValue(ParamIDs::Monitor::inputMonitorFilterControl);
    inClockRaw = vts.getRawParameterValue(ParamIDs::Monitor::inputMonitorFilterClock);
    inEventsRaw = vts.getRawParameterValue(ParamIDs::Monitor::inputMonitorFilterEvent);
    outNotesRaw = vts.getRawParameterValue(ParamIDs::Monitor::outputMonitorFilterNote);
    outControlsRaw = vts.getRawParameterValue(ParamIDs::Monitor::outputMonitorFilterControl);
    outClockRaw = vts.getRawParameterValue(ParamIDs::Monitor::outputMonitorFilterClock);
    outEventsRaw = vts.getRawParameterValue(ParamIDs::Monitor::outputMonitorFilterEvent);
    harmGlobalKeyRaw = vts.getRawParameterValue(ParamIDs::Global::harmGlobalKey);
    harmGlobalScaleRaw = vts.getRawParameterValue(ParamIDs::Global::harmGlobalScale);

    {
        uint16_t inputMask = 0;
        uint16_t outputMask = 0;
        int inputBass = -1;
        int outputBass = -1;
        lastChordSnapshotRevision = processor.getChordPitchClassSnapshotForUI(inputMask,
                                                                               inputBass,
                                                                               outputMask,
                                                                               outputBass);

        ChordDetection::Context chordContext;
        chordContext.keyTonic = juce::jlimit(0, 11,
                                             harmGlobalKeyRaw != nullptr
                                                 ? juce::roundToInt(harmGlobalKeyRaw->load(std::memory_order_relaxed))
                                                 : 0);
        chordContext.mode = ChordDetection::modeFromScaleIndex(
            harmGlobalScaleRaw != nullptr
                ? juce::roundToInt(harmGlobalScaleRaw->load(std::memory_order_relaxed))
                : 9);

        const auto inputChord = ChordDetection::nameChordFromPitchClassMask(inputMask,
                                                                             inputBass,
                                                                             chordContext);
        const auto outputChord = ChordDetection::nameChordFromPitchClassMask(outputMask,
                                                                              outputBass,
                                                                              chordContext);
        mainMenu.setDetectedChordFlow(inputChord.bestChordName, outputChord.bestChordName);
    }

    topCopyButton.setComponentID("MM_Button_Copy");
    topPasteButton.setComponentID("MM_Button_Paste");
    topDeleteButton.setComponentID("MM_Button_Delete");
    topSaveButton.setComponentID("MM_Button_Save");

    topCopyButton.setButtonText("");
    topPasteButton.setButtonText("");
    topDeleteButton.setButtonText("");
    topSaveButton.setButtonText("");

    topCopyButton.setClickingTogglesState(false);
    topPasteButton.setClickingTogglesState(false);
    topDeleteButton.setClickingTogglesState(false);
    topSaveButton.setClickingTogglesState(false);

    topCopyButton.setWantsKeyboardFocus(false);
    topPasteButton.setWantsKeyboardFocus(false);
    topDeleteButton.setWantsKeyboardFocus(false);
    topSaveButton.setWantsKeyboardFocus(false);
    topCopyButton.setTriggeredOnMouseDown(true);
    topPasteButton.setTriggeredOnMouseDown(true);
    topDeleteButton.setTriggeredOnMouseDown(true);
    topSaveButton.setTriggeredOnMouseDown(true);
    topCopyButton.setMouseCursor(juce::MouseCursor::PointingHandCursor);
    topPasteButton.setMouseCursor(juce::MouseCursor::PointingHandCursor);
    topDeleteButton.setMouseCursor(juce::MouseCursor::PointingHandCursor);
    topSaveButton.setMouseCursor(juce::MouseCursor::PointingHandCursor);

    topCopyButton.setTooltip("Copy current snapshot");
    topPasteButton.setTooltip("Paste into current snapshot");
    topDeleteButton.setTooltip("Reset current snapshot to defaults");
    topSaveButton.setTooltip("Save current snapshot");

    // Ensure drawable buttons always have concrete SVG images in this column.
    if (auto copyIcon = loadSvgFromBinaryData(BinaryData::copy_svg, BinaryData::copy_svgSize))
        topCopyButton.setImages(copyIcon.get(), copyIcon.get(), copyIcon.get(), copyIcon.get());

    if (auto pasteIcon = loadSvgFromBinaryData(BinaryData::paste_svg, BinaryData::paste_svgSize))
        topPasteButton.setImages(pasteIcon.get(), pasteIcon.get(), pasteIcon.get(), pasteIcon.get());

    if (auto deleteIcon = loadSvgFromBinaryData(BinaryData::delete_svg, BinaryData::delete_svgSize))
        topDeleteButton.setImages(deleteIcon.get(), deleteIcon.get(), deleteIcon.get(), deleteIcon.get());

    if (auto saveIcon = loadSvgFromBinaryData(BinaryData::save_svg, BinaryData::save_svgSize))
        topSaveButton.setImages(saveIcon.get(), saveIcon.get(), saveIcon.get(), saveIcon.get());

    topCopyButton.onClick = [this]
    {
        mainMenu.triggerCopyAction();
        refreshTopActionButtons();
    };

    topPasteButton.onClick = [this]
    {
        mainMenu.triggerPasteAction();
        refreshTopActionButtons();
    };

    topDeleteButton.onClick = [this]
    {
        mainMenu.triggerDeleteAction();
        refreshTopActionButtons();
    };

    topSaveButton.onClick = [this]
    {
        mainMenu.triggerSaveAction();
        refreshTopActionButtons();
    };

    addAndMakeVisible(topCopyButton);
    addAndMakeVisible(topPasteButton);
    addAndMakeVisible(topDeleteButton);
    addAndMakeVisible(topSaveButton);

    addAndMakeVisible(inputMonitor);
    addAndMakeVisible(outputMonitor);
    inputMonitor.setFifoSource(&processor.getInputFifo(), processor.getInputFifoMessages());
    outputMonitor.setFifoSource(&processor.getOutputFifo(), processor.getOutputFifoMessages());
    outputMonitor.setFifoSourceMetadata(processor.getOutputFifoSourceKinds(),
                                        processor.getOutputFifoSourceIndices());

    lanesViewport.setViewedComponent(&lanesContent, false);
    lanesViewport.setScrollBarsShown(true, false, true, false);
    addAndMakeVisible(lanesViewport);

   #if JUCE_DEBUG
    addAndMakeVisible(logInputFilterProcessorToggle);
    addAndMakeVisible(logHarmonizerToggle);
    addAndMakeVisible(logSplitterToggle);
    addAndMakeVisible(logPluginProcessorToggle);
    addAndMakeVisible(logPluginEditorToggle);
   #endif

    //--------------------------------------------------------------------------
    // Logo (SVG embedded in BinaryData)
    //--------------------------------------------------------------------------
    if (auto d = juce::Drawable::createFromImageData(BinaryData::mdvz_svg, BinaryData::mdvz_svgSize))
        mdvzLogo = std::move(d);

    if (mdvzLogo)
        tintDrawable(*mdvzLogo, PluginColours::onBackground);

    rebuildLaneParameterMaps();
    applyActiveLaneCountFromProcessor(true, false);
    refreshTopActionButtons();

    auto& presetManager = processor.getPresetManager();
    presetManager.setOnSnapshotUiCapture([this]
    {
        auto state = createSnapshotUiStateTree();
        processor.setSnapshotUiFallbackState(state);
        return state;
    });
    presetManager.setOnSnapshotUiApply([this](const juce::ValueTree& state)
    {
        applySnapshotUiStateTree(state);
        processor.setSnapshotUiFallbackState(createSnapshotUiStateTree());
    });

    applySnapshotUiStateTree(presetManager.getCurrentSnapshotUiState());
}

MidivisiViciAudioProcessorEditor::~MidivisiViciAudioProcessorEditor()
{
    processor.setSnapshotUiFallbackState(createSnapshotUiStateTree());
    processor.installSnapshotUiCallbacksFallback();

    setLookAndFeel(nullptr);
}

void MidivisiViciAudioProcessorEditor::refreshTopActionButtons()
{
    const auto saveState = mainMenu.getSaveState();
    const bool saving = (saveState == SaveState::Saving);
    const bool pending = (saveState == SaveState::Pending);
    const bool clipboardActive = mainMenu.isClipboardActive();

    const bool saveStateChanged =
        (!topActionStateInitialized || saveState != lastTopSaveState || clipboardActive != lastTopClipboardActive);

    if (saveStateChanged)
    {
        topCopyButton.setEnabled(!saving);
        topDeleteButton.setEnabled(!saving);
        topPasteButton.setEnabled(!saving && clipboardActive);
        topSaveButton.setEnabled(pending);
    }

    if (!topActionStateInitialized || clipboardActive != lastTopClipboardActive)
    {
        topCopyButton.setToggleState(clipboardActive, juce::dontSendNotification);
        topPasteButton.setToggleState(clipboardActive, juce::dontSendNotification);
    }

    if (!topActionStateInitialized)
        topSaveButton.setToggleState(false, juce::dontSendNotification);

    lastTopSaveState = saveState;
    lastTopClipboardActive = clipboardActive;
    topActionStateInitialized = true;
}

int MidivisiViciAudioProcessorEditor::getLaneHeight(int laneIndex) const noexcept
{
    if (!juce::isPositiveAndBelow(laneIndex, Lanes::kNumLanes))
        return kModuleInstanceHeight;

    return laneUiStates[(size_t) laneIndex].collapsed ? kCollapsedLaneHeight : kModuleInstanceHeight;
}

int MidivisiViciAudioProcessorEditor::getTotalLaneContentHeight() const noexcept
{
    int total = 0;
    for (int i = 0; i < activeLaneCount; ++i)
        total += getLaneHeight(i);
    return juce::jmax(total, 1);
}

int MidivisiViciAudioProcessorEditor::getVisibleLaneAreaHeight() const noexcept
{
    return juce::jlimit(kModuleInstanceHeight,
                        3 * kModuleInstanceHeight,
                        getTotalLaneContentHeight());
}

void MidivisiViciAudioProcessorEditor::rebuildLaneParameterMaps()
{
    for (auto& laneMap : laneParamMaps)
        laneMap.clear();

    for (auto* param : processor.getParameters())
    {
        auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(param);
        if (ranged == nullptr)
            continue;

        const auto pid = ranged->getParameterID();
        if (pid.length() < 3 || pid[pid.length() - 2] != '_')
            continue;

        const auto suffix = pid.substring(pid.length() - 1);
        for (int i = 0; i < Lanes::kNumLanes; ++i)
        {
            const juce::String laneSuffix = Lanes::laneSuffix(Lanes::fromIndex(i));
            if (suffix != laneSuffix)
                continue;

            laneParamMaps[(size_t) i][pid.dropLastCharacters(2)] = ranged;
            break;
        }
    }

    laneParamMapsBuilt = true;
}

void MidivisiViciAudioProcessorEditor::copyLaneParameters(int srcIndex, int dstIndex)
{
    if (!laneParamMapsBuilt)
        rebuildLaneParameterMaps();

    if (!juce::isPositiveAndBelow(srcIndex, Lanes::kNumLanes)
        || !juce::isPositiveAndBelow(dstIndex, Lanes::kNumLanes)
        || srcIndex == dstIndex)
        return;

    const auto& srcMap = laneParamMaps[(size_t) srcIndex];
    const auto& dstMap = laneParamMaps[(size_t) dstIndex];

    for (const auto& [baseId, srcParam] : srcMap)
    {
        if (srcParam == nullptr)
            continue;

        auto itDst = dstMap.find(baseId);
        if (itDst == dstMap.end() || itDst->second == nullptr)
            continue;

        setParameterNormalized(*itDst->second, srcParam->getValue());
    }
}

void MidivisiViciAudioProcessorEditor::resetLaneParametersToDefault(int laneIndex)
{
    if (!laneParamMapsBuilt)
        rebuildLaneParameterMaps();

    if (!juce::isPositiveAndBelow(laneIndex, Lanes::kNumLanes))
        return;

    auto& laneMap = laneParamMaps[(size_t) laneIndex];
    for (auto& [_, param] : laneMap)
    {
        if (param == nullptr)
            continue;
        setParameterNormalized(*param, param->getDefaultValue());
    }

    // Contract user-facing:
    // - Strum step 1 must default to Up, steps 2..32 to Skip
    // - Gate step 1 must default to 99%, steps 2..32 to Skip
    // - Groove step 1 must default to "=", steps 2..32 to Skip
    // - Velocity step 1 must default to "=", steps 2..32 to Skip
    // We force this explicitly here so "new lane" always lands on these shapes,
    // independently from potential future global default tweaks.
    const auto setStepChoice = [&](const char* seqPrefix, int step1Based, int choiceIndex)
    {
        const juce::String stepBaseId = ParamIDs::stepId(seqPrefix, step1Based);
        auto it = laneMap.find(stepBaseId);
        if (it == laneMap.end() || it->second == nullptr)
            return;

        auto* choiceParam = dynamic_cast<juce::AudioParameterChoice*>(it->second);
        if (choiceParam == nullptr)
            return;

        const int maxChoice = juce::jmax(0, choiceParam->choices.size() - 1);
        const int clampedChoice = juce::jlimit(0, maxChoice, choiceIndex);
        setParameterNormalized(*choiceParam, choiceParam->convertTo0to1((float) clampedChoice));
    };

    setStepChoice(ParamIDs::Base::arpDirectionSeqPrefix, 1, 2); // Up
    for (int step = 2; step <= 32; ++step)
        setStepChoice(ParamIDs::Base::arpDirectionSeqPrefix, step, 0); // Skip

    setStepChoice(ParamIDs::Base::arpGateSeqPrefix, 1, 8); // 99%
    for (int step = 2; step <= 32; ++step)
        setStepChoice(ParamIDs::Base::arpGateSeqPrefix, step, 0); // Skip

    setStepChoice(ParamIDs::Base::arpGrooveSeqPrefix, 1, 8); // "="
    for (int step = 2; step <= 32; ++step)
        setStepChoice(ParamIDs::Base::arpGrooveSeqPrefix, step, 0); // Skip

    setStepChoice(ParamIDs::Base::arpVelocitySeqPrefix, 1, 4); // "="
    for (int step = 2; step <= 32; ++step)
        setStepChoice(ParamIDs::Base::arpVelocitySeqPrefix, step, 0); // Skip
}

void MidivisiViciAudioProcessorEditor::swapLaneParameters(int laneA, int laneB)
{
    if (!laneParamMapsBuilt)
        rebuildLaneParameterMaps();

    if (!juce::isPositiveAndBelow(laneA, Lanes::kNumLanes)
        || !juce::isPositiveAndBelow(laneB, Lanes::kNumLanes)
        || laneA == laneB)
        return;

    auto& mapA = laneParamMaps[(size_t) laneA];
    auto& mapB = laneParamMaps[(size_t) laneB];

    for (auto& [baseId, paramA] : mapA)
    {
        auto itB = mapB.find(baseId);
        if (paramA == nullptr || itB == mapB.end() || itB->second == nullptr)
            continue;

        auto* paramB = itB->second;
        const float a = paramA->getValue();
        const float b = paramB->getValue();
        setParameterNormalized(*paramA, b);
        setParameterNormalized(*paramB, a);
    }
}

void MidivisiViciAudioProcessorEditor::insertLaneAfter(int laneIndex, bool duplicateSource)
{
    if (!juce::isPositiveAndBelow(laneIndex, activeLaneCount) || activeLaneCount >= Lanes::kNumLanes)
        return;

    const int insertAt = laneIndex + 1;
    const int oldCount = activeLaneCount;

    for (int i = oldCount - 1; i >= insertAt; --i)
    {
        copyLaneParameters(i, i + 1);
        laneUiStates[(size_t) (i + 1)] = laneUiStates[(size_t) i];
    }

    if (duplicateSource)
    {
        copyLaneParameters(laneIndex, insertAt);
        laneUiStates[(size_t) insertAt] = laneUiStates[(size_t) laneIndex];
        laneUiStates[(size_t) insertAt].collapsed = false;
    }
    else
    {
        resetLaneParametersToDefault(insertAt);
        laneUiStates[(size_t) insertAt] = LaneUiState {};
    }

    processor.setActiveLaneCount(oldCount + 1);
    applyActiveLaneCountFromProcessor(true, true);
}

void MidivisiViciAudioProcessorEditor::removeLaneAt(int laneIndex)
{
    if (!juce::isPositiveAndBelow(laneIndex, activeLaneCount) || activeLaneCount <= 1)
        return;

    const int oldCount = activeLaneCount;
    for (int i = laneIndex; i < oldCount - 1; ++i)
    {
        copyLaneParameters(i + 1, i);
        laneUiStates[(size_t) i] = laneUiStates[(size_t) (i + 1)];
    }

    resetLaneParametersToDefault(oldCount - 1);
    laneUiStates[(size_t) (oldCount - 1)] = LaneUiState {};

    processor.setActiveLaneCount(oldCount - 1);
    applyActiveLaneCountFromProcessor(true, false);
}

void MidivisiViciAudioProcessorEditor::moveLaneUp(int laneIndex)
{
    if (laneIndex <= 0 || laneIndex >= activeLaneCount)
        return;

    swapLaneParameters(laneIndex, laneIndex - 1);
    std::swap(laneUiStates[(size_t) laneIndex], laneUiStates[(size_t) (laneIndex - 1)]);
    applyActiveLaneCountFromProcessor(false, false);
}

void MidivisiViciAudioProcessorEditor::moveLaneDown(int laneIndex)
{
    if (laneIndex < 0 || laneIndex >= (activeLaneCount - 1))
        return;

    swapLaneParameters(laneIndex, laneIndex + 1);
    std::swap(laneUiStates[(size_t) laneIndex], laneUiStates[(size_t) (laneIndex + 1)]);
    applyActiveLaneCountFromProcessor(false, false);
}

void MidivisiViciAudioProcessorEditor::showLaneRenameDialog(int laneIndex)
{
    if (!juce::isPositiveAndBelow(laneIndex, activeLaneCount))
        return;

    auto* dialog = new juce::AlertWindow("Rename Lane",
                                         "Enter lane name.",
                                         juce::AlertWindow::NoIcon);
    dialog->addTextEditor("name", laneUiStates[(size_t) laneIndex].name, "Name:");
    dialog->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
    dialog->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    auto dialogSafe = juce::Component::SafePointer<juce::AlertWindow>(dialog);
    auto editorSafe = juce::Component::SafePointer<MidivisiViciAudioProcessorEditor>(this);

    dialog->enterModalState(true,
                            juce::ModalCallbackFunction::create(
                                [dialogSafe, editorSafe, laneIndex](int result)
                                {
                                    if (result != 1 || editorSafe == nullptr || dialogSafe == nullptr)
                                        return;

                                    auto* te = dialogSafe->getTextEditor("name");
                                    if (te == nullptr)
                                        return;

                                    const auto newName = te->getText().trim();
                                    editorSafe->laneUiStates[(size_t) laneIndex].name =
                                        newName.isNotEmpty() ? newName : "Untitled";
                                    editorSafe->applyActiveLaneCountFromProcessor(false, false);
                                }),
                            true);
}

int MidivisiViciAudioProcessorEditor::pickUniqueRandomLaneColour(int laneIndex) const noexcept
{
    return PluginColours::defaultPaletteIndexForOrder(laneIndex);
}

void MidivisiViciAudioProcessorEditor::showLaneNameContextMenu(int laneIndex, juce::Component& target)
{
    if (!juce::isPositiveAndBelow(laneIndex, activeLaneCount))
        return;

    juce::PopupMenu menu;
    menu.addItem(1, "Rename...");

    juce::PopupMenu colours;
    for (int i = 0; i < 16; ++i)
        colours.addItem(100 + i, laneColourLabel(i), true, laneUiStates[(size_t) laneIndex].colourIndex == i);

    menu.addSubMenu("Colour", colours);
    menu.addSeparator();
    menu.addItem(2, "Auto colour", true, laneUiStates[(size_t) laneIndex].colourIndex < 0);

    auto editorSafe = juce::Component::SafePointer<MidivisiViciAudioProcessorEditor>(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&target),
                       [editorSafe, laneIndex](int result)
                       {
                           if (editorSafe == nullptr || result == 0)
                               return;

                           if (result == 1)
                           {
                               editorSafe->showLaneRenameDialog(laneIndex);
                               return;
                           }

                           if (result == 2)
                           {
                               editorSafe->laneUiStates[(size_t) laneIndex].colourIndex =
                                   editorSafe->pickUniqueRandomLaneColour(laneIndex);
                               editorSafe->applyActiveLaneCountFromProcessor(false, false);
                               return;
                           }

                           if (result >= 100 && result < 116)
                           {
                               editorSafe->laneUiStates[(size_t) laneIndex].colourIndex = result - 100;
                               editorSafe->applyActiveLaneCountFromProcessor(false, false);
                           }
                       });
}

void MidivisiViciAudioProcessorEditor::ensureLaneComponentsCreated(int laneCount)
{
    const auto targetCount = juce::jlimit(1, Lanes::kNumLanes, laneCount);
    auto& vts = processor.getValueTreeState();

    while ((int) laneComponents.size() < targetCount)
    {
        const int laneIndex = (int) laneComponents.size();
        const auto lane = Lanes::fromIndex(laneIndex);

        LaneComponents row;
        row.inputFilter = std::make_unique<InputFilter>(vts, lane);
        row.harmonizer = std::make_unique<Harmonizer>(vts, processor, lane);
        row.arpeggiator = std::make_unique<Arpeggiator>(vts, processor, lane);
        row.splitter = std::make_unique<Splitter>(vts, lane);

        auto menu = std::make_unique<LaneMenuComponent>();
        menu->setCallbacks(
            LaneMenuComponent::Callbacks{
                [this, laneIndex] { insertLaneAfter(laneIndex, true);  },
                [this, laneIndex] { insertLaneAfter(laneIndex, false); },
                [this, laneIndex] { removeLaneAt(laneIndex);           },
                [this, laneIndex] { moveLaneDown(laneIndex);           },
                [this, laneIndex] { moveLaneUp(laneIndex);             },
                [this, laneIndex]
                {
                    if (!juce::isPositiveAndBelow(laneIndex, activeLaneCount))
                        return;
                    laneUiStates[(size_t) laneIndex].collapsed = !laneUiStates[(size_t) laneIndex].collapsed;
                    // Recompute editor/monitor height on collapse/expand with the same
                    // min/max clamp policy (1..3 module heights).
                    applyActiveLaneCountFromProcessor(true, false);
                },
                [this, laneIndex]
                {
                    if (juce::isPositiveAndBelow(laneIndex, (int) laneMenus.size()) && laneMenus[(size_t) laneIndex] != nullptr)
                        showLaneNameContextMenu(laneIndex, *laneMenus[(size_t) laneIndex]);
                }
            });

        lanesContent.addAndMakeVisible(*row.inputFilter);
        lanesContent.addAndMakeVisible(*row.harmonizer);
        lanesContent.addAndMakeVisible(*row.arpeggiator);
        lanesContent.addAndMakeVisible(*row.splitter);
        lanesContent.addAndMakeVisible(*menu);

        laneComponents.push_back(std::move(row));
        laneMenus.push_back(std::move(menu));
    }
}

void MidivisiViciAudioProcessorEditor::applyActiveLaneCountFromProcessor(bool resizeEditor, bool scrollToBottom)
{
    activeLaneCount = juce::jlimit(1, Lanes::kNumLanes, processor.getActiveLaneCount());
    lfoGroup.markDestinationLabelsDirty();
    ensureLaneComponentsCreated(activeLaneCount);

    for (int i = 0; i < (int) laneComponents.size(); ++i)
    {
        const bool visible = (i < activeLaneCount);
        if (visible && !juce::isPositiveAndBelow(laneUiStates[(size_t) i].colourIndex, 16))
            laneUiStates[(size_t) i].colourIndex = pickUniqueRandomLaneColour(i);

        const bool collapsed = visible && laneUiStates[(size_t) i].collapsed;
        laneComponents[(size_t) i].inputFilter->setVisible(visible && !collapsed);
        laneComponents[(size_t) i].harmonizer->setVisible(visible && !collapsed);
        laneComponents[(size_t) i].arpeggiator->setVisible(visible && !collapsed);
        laneComponents[(size_t) i].splitter->setVisible(visible && !collapsed);

        if (i < (int) laneMenus.size() && laneMenus[(size_t) i] != nullptr)
        {
            auto& menu = *laneMenus[(size_t) i];
            menu.setVisible(visible);
            menu.setLaneNumber(i + 1);
            menu.setCollapsed(laneUiStates[(size_t) i].collapsed);
            menu.setCanMoveUp(i > 0);
            menu.setCanMoveDown(i < (activeLaneCount - 1));
            menu.setCanDelete(activeLaneCount > 1);
            menu.setCanInsertOrDuplicate(activeLaneCount < Lanes::kNumLanes);
            menu.setLaneName(laneUiStates[(size_t) i].name);
            const auto [bg, fg] = getLaneNameColours(laneUiStates[(size_t) i].colourIndex);
            menu.setNameColours(bg, fg);
        }
    }

    const int width = kEditorWidth;
    const int height = kHeaderHeight + getVisibleLaneAreaHeight() + kFooterHeight;

    if (resizeEditor && (getWidth() != width || getHeight() != height))
    {
        setSize(width, height);
    }
    else
    {
        resized();
    }

    if (scrollToBottom)
    {
        const int contentHeight = getTotalLaneContentHeight();
        const int viewportHeight = lanesViewport.getHeight();

        if (contentHeight > viewportHeight)
            lanesViewport.setViewPosition(0, juce::jmax(0, contentHeight - viewportHeight));
    }
}

juce::ValueTree MidivisiViciAudioProcessorEditor::createSnapshotUiStateTree() const
{
    juce::ValueTree state(kUiStateNodeID);
    state.setProperty(kUiActiveLaneCountPropID, activeLaneCount, nullptr);

    juce::ValueTree lanesNode(kUiLanesNodeID);
    for (int i = 0; i < activeLaneCount; ++i)
    {
        juce::ValueTree laneNode(kUiLaneNodeID);
        laneNode.setProperty(kUiLaneIndexPropID, i, nullptr);
        laneNode.setProperty(kUiLaneNamePropID, laneUiStates[(size_t) i].name, nullptr);
        laneNode.setProperty(kUiLaneColourPropID, laneUiStates[(size_t) i].colourIndex, nullptr);
        laneNode.setProperty(kUiLaneCollapsedPropID, laneUiStates[(size_t) i].collapsed, nullptr);
        lanesNode.addChild(laneNode, -1, nullptr);
    }
    state.addChild(lanesNode, -1, nullptr);

    state.addChild(lfoGroup.getStateTree(), -1, nullptr);
    state.addChild(mainBar.getStateTree(), -1, nullptr);
    return state;
}

void MidivisiViciAudioProcessorEditor::applySnapshotUiStateTree(const juce::ValueTree& state)
{
    std::array<LaneUiState, Lanes::kNumLanes> restoredStates {};
    const bool hasUiState = state.isValid() && state.hasType(kUiStateNodeID);

    int restoredLaneCount = juce::jlimit(1, Lanes::kNumLanes, processor.getActiveLaneCount());
    if (hasUiState)
    {
        restoredLaneCount = juce::jlimit(1,
                                         Lanes::kNumLanes,
                                         (int) state.getProperty(kUiActiveLaneCountPropID,
                                                                  restoredLaneCount));
    }

    if (hasUiState)
    {
        if (const auto lanesNode = state.getChildWithName(kUiLanesNodeID); lanesNode.isValid())
        {
            for (int i = 0; i < lanesNode.getNumChildren(); ++i)
            {
                const auto laneNode = lanesNode.getChild(i);
                if (!laneNode.isValid() || !laneNode.hasType(kUiLaneNodeID))
                    continue;

                const int laneIndex = juce::jlimit(0,
                                                   restoredLaneCount - 1,
                                                   (int) laneNode.getProperty(kUiLaneIndexPropID, i));

                auto& laneState = restoredStates[(size_t) laneIndex];
                const juce::String restoredName = laneNode.getProperty(kUiLaneNamePropID, laneState.name).toString().trim();
                laneState.name = restoredName.isNotEmpty() ? restoredName : "Untitled";
                laneState.colourIndex = juce::jlimit(-1, 15, (int) laneNode.getProperty(kUiLaneColourPropID, laneState.colourIndex));
                laneState.collapsed = (bool) laneNode.getProperty(kUiLaneCollapsedPropID, laneState.collapsed);
            }
        }
    }

    laneUiStates = restoredStates;

    processor.setActiveLaneCount(restoredLaneCount);
    applyActiveLaneCountFromProcessor(true, false);

    if (hasUiState)
        lfoGroup.applyStateTree(state.getChildWithName("LFO_STATE"));
    else
        lfoGroup.applyStateTree({});

    if (hasUiState)
        mainBar.applyStateTree(state.getChildWithName("MAINBAR_STATE"));
    else
        mainBar.applyStateTree({});
}

//==============================================================================
// Paint
//==============================================================================

void MidivisiViciAudioProcessorEditor::paint(juce::Graphics& g)
{
    // Base background fill
    g.fillAll(PluginColours::background);

    // Vector logo (tinted once, but re-tinted here for safety if LAF changes it)
    if (mdvzLogo)
    {
        const int placementFlags =
            juce::RectanglePlacement::xLeft |
            juce::RectanglePlacement::yMid  |
            juce::RectanglePlacement::onlyReduceInSize;

        tintDrawable(*mdvzLogo, PluginColours::onBackground);

        juce::Graphics::ScopedSaveState sss(g);
        mdvzLogo->drawWithin(g, logoBounds.toFloat(), placementFlags, 1.0f);
    }
}

//==============================================================================
// Resized
//==============================================================================

void MidivisiViciAudioProcessorEditor::resized()
{
    auto area = getLocalBounds();

    auto header = area.removeFromTop(kHeaderHeight);

    logoBounds = header.removeFromLeft(kColumnWidth).reduced(20, 12);

    auto topActions = header.removeFromLeft(kLaneMenuWidth);
    mainMenu.setBounds(header.removeFromLeft(kColumnWidth));
    auto lfoBounds = header.removeFromLeft(juce::jmin(header.getWidth(), 4 * kColumnWidth));
    lfoGroup.setBounds(lfoBounds);

    {
        constexpr int kButtonSize = 24; // Match LaneMenu round button diameter.
        constexpr int kTopBottomMargin = UiMetrics::kModuleInnerMargin * 2;
        constexpr int kSideMargin = 0;

        auto a = topActions.withTrimmedTop(kTopBottomMargin)
                           .withTrimmedBottom(kTopBottomMargin)
                           .withTrimmedLeft(kSideMargin)
                           .withTrimmedRight(kSideMargin);

        const int totalButtonsHeight = 4 * kButtonSize;
        const int gap = juce::jmax(0, (a.getHeight() - totalButtonsHeight) / 3);

        int y = a.getY();
        auto placeButton = [&](juce::DrawableButton& button)
        {
            auto slot = juce::Rectangle<int>(a.getX(), y, a.getWidth(), kButtonSize);
            button.setBounds(slot.withSizeKeepingCentre(kButtonSize, kButtonSize));
            y += kButtonSize + gap;
        };

        placeButton(topCopyButton);
        placeButton(topPasteButton);
        placeButton(topDeleteButton);
        placeButton(topSaveButton);
    }

    // Keep top action buttons above sibling components in case of overlap.
    topCopyButton.toFront(false);
    topPasteButton.toFront(false);
    topDeleteButton.toFront(false);
    topSaveButton.toFront(false);

    auto footer = area.removeFromBottom(kFooterHeight);
    // Left footer segment spans:
    //   [InputMonitor column] + [Lane menu band] + [InputFilter column].
    // MainBar applies module outer margins internally, so we keep full slot width here.
    const int leftMainBarWidth = (2 * kColumnWidth) + kLaneMenuWidth;
    mainBar.setLeftModuleWidth(juce::jmax(1, leftMainBarWidth));
    mainBar.setBounds(footer);

    auto center = area;
    inputMonitor.setBounds(center.removeFromLeft(kColumnWidth));
    outputMonitor.setBounds(center.removeFromRight(kColumnWidth));
    lanesViewport.setBounds(center);

    const int contentWidth = lanesViewport.getWidth();
    const int contentHeight = juce::jmax(lanesViewport.getHeight(), getTotalLaneContentHeight());
    lanesContent.setBounds(0, 0, contentWidth, contentHeight);

    int y = 0;
    for (int laneIndex = 0; laneIndex < activeLaneCount && laneIndex < (int) laneComponents.size(); ++laneIndex)
    {
        const int laneHeight = getLaneHeight(laneIndex);
        auto row = juce::Rectangle<int>(0, y, contentWidth, laneHeight);
        y += laneHeight;

        const bool collapsed = laneUiStates[(size_t) laneIndex].collapsed;

        if (laneIndex < (int) laneMenus.size() && laneMenus[(size_t) laneIndex] != nullptr)
        {
            juce::Rectangle<int> laneMenuBounds;
            if (collapsed)
                laneMenuBounds = row.withTrimmedLeft(UiMetrics::kModuleOuterMargin)
                                    .withTrimmedRight(UiMetrics::kModuleOuterMargin);
            else
                laneMenuBounds = row.removeFromLeft(kLaneMenuWidth);

            laneMenus[(size_t) laneIndex]->setBounds(laneMenuBounds);
        }

        if (collapsed)
        {
            laneComponents[(size_t) laneIndex].inputFilter->setBounds({});
            laneComponents[(size_t) laneIndex].harmonizer->setBounds({});
            laneComponents[(size_t) laneIndex].arpeggiator->setBounds({});
            laneComponents[(size_t) laneIndex].splitter->setBounds({});
            continue;
        }

        laneComponents[(size_t) laneIndex].inputFilter->setBounds(row.removeFromLeft(kColumnWidth));
        laneComponents[(size_t) laneIndex].harmonizer->setBounds(row.removeFromLeft(kColumnWidth));
        laneComponents[(size_t) laneIndex].arpeggiator->setBounds(row.removeFromLeft(kColumnWidth));
        laneComponents[(size_t) laneIndex].splitter->setBounds(row.removeFromLeft(kColumnWidth));
    }
}

//==============================================================================
// Timer
//==============================================================================

void MidivisiViciAudioProcessorEditor::timerCallback()
{
    ++uiSchedulerTickCounter;

    refreshTopActionButtons();
    mainMenu.setDisplayedBpm(processor.getBpm());

    // Chord name refresh is driven by a lock-free revision snapshot published
    // by the processor. We only run detection when the revision changes, so the
    // message thread cost stays bounded under dense MIDI traffic.
    {
        uint16_t inputMask = 0;
        uint16_t outputMask = 0;
        int inputBass = -1;
        int outputBass = -1;
        const uint32_t chordRevision = processor.getChordPitchClassSnapshotForUI(inputMask,
                                                                                  inputBass,
                                                                                  outputMask,
                                                                                  outputBass);
        if (chordRevision != lastChordSnapshotRevision)
        {
            lastChordSnapshotRevision = chordRevision;

            ChordDetection::Context chordContext;
            chordContext.keyTonic = juce::jlimit(0, 11,
                                                 harmGlobalKeyRaw != nullptr
                                                     ? juce::roundToInt(harmGlobalKeyRaw->load(std::memory_order_relaxed))
                                                     : 0);
            chordContext.mode = ChordDetection::modeFromScaleIndex(
                harmGlobalScaleRaw != nullptr
                    ? juce::roundToInt(harmGlobalScaleRaw->load(std::memory_order_relaxed))
                    : 9);

            const auto inputChord = ChordDetection::nameChordFromPitchClassMask(inputMask,
                                                                                 inputBass,
                                                                                 chordContext);
            const auto outputChord = ChordDetection::nameChordFromPitchClassMask(outputMask,
                                                                                  outputBass,
                                                                                  chordContext);
            mainMenu.setDetectedChordFlow(inputChord.bestChordName, outputChord.bestChordName);
        }
    }

    if (activeLaneCount != processor.getActiveLaneCount())
        applyActiveLaneCountFromProcessor(true, false);

    const bool run30Hz = ((uiSchedulerTickCounter & 0x1u) == 0u);
    const bool run15Hz = ((uiSchedulerTickCounter & 0x3u) == 0u);

    lfoGroup.uiTimerTick();
    mainBar.uiTimerTick();
    for (int i = 0; i < activeLaneCount && i < (int) laneComponents.size(); ++i)
    {
        if (laneUiStates[(size_t) i].collapsed)
            continue;
        laneComponents[(size_t) i].arpeggiator->uiTimerTick();
    }

    if (run30Hz)
    {
        inputMonitor.uiTimerTick();
        outputMonitor.uiTimerTick();

        for (int i = 0; i < activeLaneCount && i < (int) laneComponents.size(); ++i)
        {
            if (laneUiStates[(size_t) i].collapsed)
                continue;

            laneComponents[(size_t) i].harmonizer->uiTimerTick();
        }
    }

    if (run15Hz)
    {
        for (int i = 0; i < activeLaneCount && i < (int) laneComponents.size(); ++i)
        {
            if (laneUiStates[(size_t) i].collapsed)
                continue;
            laneComponents[(size_t) i].splitter->uiTimerTick();
        }
    }

    if (!run15Hz)
        return;

    bool needsRepaint = false;

    //--------------------------------------------------------------------------
    // 1) Monitor filter state -> LookAndFeel
    //
    // The monitors themselves may repaint based on FIFO content,
    // but the filter buttons/state used by the LookAndFeel is tied to parameters.
    //
    // Because these are bool parameters, reading raw atomic<float> values is
    // appropriate (0/1).
    //--------------------------------------------------------------------------
    const bool inNotes    = readRawBool(inNotesRaw, true);
    const bool inControls = readRawBool(inControlsRaw, true);
    const bool inClock    = readRawBool(inClockRaw, true);
    const bool inEvents   = readRawBool(inEventsRaw, true);

    const bool outNotes    = readRawBool(outNotesRaw, true);
    const bool outControls = readRawBool(outControlsRaw, true);
    const bool outClock    = readRawBool(outClockRaw, true);
    const bool outEvents   = readRawBool(outEventsRaw, true);

    // Store last values as editor members (not static locals), so behavior is
    // well-defined if multiple editor instances ever exist.
    if (inNotes != lastInNotes || inControls != lastInControls || inClock != lastInClock || inEvents != lastInEvents)
    {
        lookAndFeel.setInputMonitorStates(inNotes, inControls, inClock, inEvents);

        lastInNotes    = inNotes;
        lastInControls = inControls;
        lastInClock    = inClock;
        lastInEvents   = inEvents;

        needsRepaint = true;
    }

    if (outNotes != lastOutNotes || outControls != lastOutControls || outClock != lastOutClock || outEvents != lastOutEvents)
    {
        lookAndFeel.setOutputMonitorStates(outNotes, outControls, outClock, outEvents);

        lastOutNotes    = outNotes;
        lastOutControls = outControls;
        lastOutClock    = outClock;
        lastOutEvents   = outEvents;

        needsRepaint = true;
    }

    //--------------------------------------------------------------------------
    // 2) Repaint only when needed
    //--------------------------------------------------------------------------
    if (needsRepaint)
        repaint();
}
