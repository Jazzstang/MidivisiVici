/*
==============================================================================
LFOGroup.cpp
------------------------------------------------------------------------------
Role in architecture:
- Dynamic editor-side container for global LFO cards.
- Owns card lifecycle (add/remove/duplicate/reorder/collapse).
- Rebuilds destination lists from current lane count + mainbar page count.
- Publishes per-row runtime configuration to processor caches.

Threading model:
- Message thread only.
- Timer-driven visual updates must remain lightweight to preserve UI fluidity.
==============================================================================
*/

#include "LFOGroup.h"
#include "PluginProcessor.h"
#include "UiMetrics.h"
#include "BinaryData.h"

#include <array>

namespace
{
    // Musical rate menu used by each LFO row.
    static const std::vector<std::pair<juce::String, double>> rateOptions = {
        {"4/1",   4.0},   {"3/1",   3.0},   {"2/1",   2.0},   {"1/1",   1.0},
        {"1/2",   0.5},   {"1/2.",  0.75},  {"1/2t",  1.0 / 3.0},
        {"1/4",   0.25},  {"1/4.",  0.375}, {"1/4t",  1.0 / 6.0},
        {"1/8",   0.125}, {"1/8.",  0.1875},{"1/8t",  1.0 / 12.0},
        {"1/16",  0.0625},{"1/16.", 0.09375},{"1/16t", 1.0 / 24.0},
        {"1/32",  0.03125},{"1/32.",0.046875},{"1/32t",1.0 / 48.0},
    };

    constexpr int kLaneGap = 0;
    constexpr int kCollapsedWidth = 26;
    constexpr int kTopSectionMinHeight = 78;
    // Bottom strip hosts Offset label+slider (left) and waveform buttons (right).
    // Keep enough vertical room so Offset thumb can stay 2x track thickness.
    constexpr int kBottomSectionHeight = 30;
    constexpr int kBottomSectionGap = 4;
    constexpr int kButtonGap = 3;
    constexpr int kActionButtonCount = 7;
    constexpr int kWaveButtonCount = 5;

    static const juce::Identifier kLfoStateNodeID      { "LFO_STATE" };
    static const juce::Identifier kLfoRowNodeID        { "LFO_ROW" };
    static const juce::Identifier kLfoCountPropID      { "count" };
    static const juce::Identifier kLfoRowIndexPropID   { "index" };
    static const juce::Identifier kLfoCollapsedPropID  { "collapsed" };
    static const juce::Identifier kLfoRatePropID       { "rate" };
    static const juce::Identifier kLfoDepthPropID      { "depth" };
    static const juce::Identifier kLfoDestinationPropID{ "destinationId" };
    static const juce::Identifier kLfoDestinationsPropID{ "destinationIds" };
    static const juce::Identifier kLfoOffsetPropID     { "offset" };
    static const juce::Identifier kLfoWavePropID       { "wave" };

    constexpr int kDestinationNoneId = MidivisiViciAudioProcessor::LfoDestinationIds::kNone;

    static inline float wrap01(float v) noexcept
    {
        float w = std::fmod(v, 1.0f);
        if (w < 0.0f)
            w += 1.0f;
        return w;
    }

    static inline float randomUnit(float x) noexcept
    {
        const float v = std::sin(x * 12.9898f) * 43758.5453f;
        return v - std::floor(v);
    }

    static juce::String normalizeDisplayName(const juce::String& name, const juce::String& fallback)
    {
        const auto trimmed = name.trim();
        return trimmed.isNotEmpty() ? trimmed : fallback;
    }

    static juce::String serializeDestinationIds(const std::vector<int>& ids)
    {
        juce::StringArray parts;
        parts.ensureStorageAllocated((int) ids.size());
        for (const int id : ids)
            parts.add(juce::String(id));
        return parts.joinIntoString(",");
    }

    static juce::Array<int> parseDestinationIds(const juce::String& csv)
    {
        juce::Array<int> ids;
        const auto tokens = juce::StringArray::fromTokens(csv, ",", {});
        for (const auto& token : tokens)
        {
            const int id = token.trim().getIntValue();
            if (id > 0)
                ids.addIfNotAlreadyThere(id);
        }
        return ids;
    }

    static std::unique_ptr<juce::Drawable> loadWaveIcon(int shapeIndex)
    {
        switch (shapeIndex)
        {
            case 0: return juce::Drawable::createFromImageData(BinaryData::sine_svg, BinaryData::sine_svgSize);
            case 1: return juce::Drawable::createFromImageData(BinaryData::triangle_svg, BinaryData::triangle_svgSize);
            case 2: return juce::Drawable::createFromImageData(BinaryData::saw_svg, BinaryData::saw_svgSize);
            case 3: return juce::Drawable::createFromImageData(BinaryData::square_svg, BinaryData::square_svgSize);
            case 4: return juce::Drawable::createFromImageData(BinaryData::rnd_svg, BinaryData::rnd_svgSize);
            default: break;
        }
        return {};
    }
}

class LFOGroup::MiniRoundButton : public juce::Button
{
public:
    explicit MiniRoundButton(juce::String glyphText)
        : juce::Button(glyphText), glyph(std::move(glyphText))
    {
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
    }

    void setGlyph(juce::String glyphText)
    {
        if (glyph == glyphText)
            return;

        glyph = std::move(glyphText);
        repaint();
    }

    void setSelected(bool shouldBeSelected)
    {
        if (selected == shouldBeSelected)
            return;

        selected = shouldBeSelected;
        repaint();
    }

    void setIcon(std::unique_ptr<juce::Drawable> newIcon)
    {
        iconTemplate = std::move(newIcon);
        iconTinted.reset();
        iconDirty = true;
        repaint();
    }

    void setCustomColours(juce::Colour bg, juce::Colour fg)
    {
        if (useCustomColours && customBackground == bg && customForeground == fg)
            return;

        useCustomColours = true;
        customBackground = bg;
        customForeground = fg;
        repaint();
    }

    void paintButton(juce::Graphics& g, bool isMouseOver, bool isButtonDown) override
    {
        auto bounds = getLocalBounds().toFloat().reduced(1.0f);
        const auto side = juce::jmin(bounds.getWidth(), bounds.getHeight());
        bounds = bounds.withSizeKeepingCentre(side, side);

        auto bg = PluginColours::disabled;
        auto fg = PluginColours::onDisabled;
        if (isEnabled())
        {
            if (useCustomColours)
            {
                bg = customBackground;
                fg = customForeground;
            }
            else
            {
                bg = selected ? PluginColours::onPrimary : PluginColours::primary;
                fg = selected ? PluginColours::primary : PluginColours::onPrimary;
            }
        }

        // Wave SVGs must stay high-contrast on dark "ON" state.
        if (!useCustomColours && selected && iconTemplate != nullptr && isEnabled())
            fg = juce::Colours::white;

        if (isMouseOver && isEnabled())
            bg = bg.brighter(0.08f);
        if (isButtonDown && isEnabled())
            bg = PluginColours::pressed;

        g.setColour(bg);
        g.fillEllipse(bounds);

        if (iconTemplate != nullptr)
        {
            if (iconDirty || iconTinted == nullptr || iconTintColour != fg)
            {
                iconTinted = iconTemplate->createCopy();
                if (iconTinted != nullptr)
                {
                    iconTinted->replaceColour(juce::Colours::black, fg);
                    iconTinted->replaceColour(juce::Colours::white, fg);
                }
                iconTintColour = fg;
                iconDirty = false;
            }

            if (iconTinted != nullptr)
            {
                const auto iconBounds = bounds.reduced(side * 0.22f);
                iconTinted->drawWithin(g, iconBounds, juce::RectanglePlacement::centred, 1.0f);
                return;
            }
        }

        g.setColour(fg);
        g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
        g.drawFittedText(glyph, getLocalBounds(), juce::Justification::centred, 1);
    }

private:
    juce::String glyph;
    bool selected = false;
    bool iconDirty = false;
    bool useCustomColours = false;
    juce::Colour customBackground;
    juce::Colour customForeground;
    juce::Colour iconTintColour;
    std::unique_ptr<juce::Drawable> iconTemplate;
    std::unique_ptr<juce::Drawable> iconTinted;
};

class LFOGroup::RowsContent : public juce::Component
{
public:
    explicit RowsContent(LFOGroup& ownerRef) : owner(ownerRef) {}

    void paint(juce::Graphics& g) override
    {
        for (const auto& card : owner.expandedCardBounds)
        {
            g.setColour(PluginColours::surface);
            g.fillRoundedRectangle(card, 12.0f);
        }
    }

private:
    LFOGroup& owner;
};

//------------------------------------------------------------------------------
// Construction: create horizontal viewport and bootstrap default rows.
//------------------------------------------------------------------------------
LFOGroup::LFOGroup(MidivisiViciAudioProcessor& proc)
    : processor(proc)
{
    rowsContent = std::make_unique<RowsContent>(*this);
    rowsViewport.setViewedComponent(rowsContent.get(), false);
    // LFO strip scrolls horizontally: show horizontal scrollbar when needed.
    rowsViewport.setScrollBarsShown(false, true, false, false);
    addAndMakeVisible(rowsViewport);

    resetToDefaultLayout();
}

LFOGroup::~LFOGroup()
{
}

void LFOGroup::setDestinationLabelProviders(
    std::function<juce::String(int)> laneLabelProvider,
    std::function<juce::String(int)> mainBarLabelProvider,
    std::function<std::pair<juce::Colour, juce::Colour>(int)> laneColourProvider,
    std::function<std::pair<juce::Colour, juce::Colour>(int)> mainBarColourProvider)
{
    laneDestinationLabelProvider = std::move(laneLabelProvider);
    mainBarDestinationLabelProvider = std::move(mainBarLabelProvider);
    laneDestinationColourProvider = std::move(laneColourProvider);
    mainBarDestinationColourProvider = std::move(mainBarColourProvider);
    destinationLabelsDirty = true;
}

void LFOGroup::markDestinationLabelsDirty() noexcept
{
    destinationLabelsDirty = true;
}

//------------------------------------------------------------------------------
// Build one default LFO card and wire all callbacks.
//------------------------------------------------------------------------------
std::unique_ptr<LFOGroup::LfoRow> LFOGroup::createDefaultRow()
{
    auto row = std::make_unique<LfoRow>();

    std::vector<juce::String> rateLabels;
    rateLabels.reserve(rateOptions.size());
    for (const auto& opt : rateOptions)
        rateLabels.push_back(opt.first);

    row->rateKnob = std::make_unique<CustomRotaryWithCombo>(
        "Rate", 0, (int) rateLabels.size() - 1, 3, false,
        CustomRotaryWithCombo::ArcMode::LeftToCursor, true);
    row->rateKnob->setStringList(rateLabels);
    row->rateKnob->setShowPlusSign(false);
    rowsContent->addAndMakeVisible(*row->rateKnob);
    if (row->rateKnob != nullptr)
    {
        row->rateKnob->onValueChange = [this](int)
        {
            pushRtLfoConfigToProcessor();
        };
    }

    row->depthKnob = std::make_unique<CustomRotaryWithCombo>(
        "Depth", -100, 100, 0, false,
        CustomRotaryWithCombo::ArcMode::CenterToCursor, true);
    row->depthKnob->setShowPlusSign(false);
    rowsContent->addAndMakeVisible(*row->depthKnob);

    row->destinationBox = std::make_unique<FlatComboBox>();
    row->destinationBox->setMultiSelectEnabled(true);
    row->destinationBox->setShowMultiSelectActions(true);
    row->destinationBox->setReopenPopupOnMultiSelect(true);
    row->destinationBox->setMultiSelectionTextFormatter([](const juce::Array<int>& selectedIds)
    {
        if (selectedIds.size() <= 1)
            return juce::String();
        return juce::String(selectedIds.size()) + " destinations";
    });
    row->destinationBox->setSelectedId(kDestinationNoneId, juce::dontSendNotification);
    rowsContent->addAndMakeVisible(*row->destinationBox);

    row->offsetSlider = std::make_unique<juce::Slider>(juce::Slider::LinearHorizontal, juce::Slider::NoTextBox);
    row->offsetSlider->setComponentID("LFO_OffsetSlider");
    row->offsetSlider->setRange(-100.0, 100.0, 1.0);
    row->offsetSlider->setDoubleClickReturnValue(true, 0.0);
    row->offsetSlider->setValue(0.0, juce::dontSendNotification);
    row->offsetSlider->setTooltip("LFO Offset");
    row->offsetSlider->setColour(juce::Slider::trackColourId, PluginColours::primary);
    row->offsetSlider->setColour(juce::Slider::backgroundColourId, PluginColours::background);
    row->offsetSlider->setColour(juce::Slider::thumbColourId, PluginColours::onPrimary);
    rowsContent->addAndMakeVisible(*row->offsetSlider);

    row->offsetLabel = std::make_unique<juce::Label>();
    row->offsetLabel->setText("Offset", juce::dontSendNotification);
    row->offsetLabel->setJustificationType(juce::Justification::centred);
    row->offsetLabel->setColour(juce::Label::textColourId, PluginColours::onSurface);
    row->offsetLabel->setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    row->offsetLabel->setInterceptsMouseClicks(false, false);
    row->offsetLabel->setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::plain)));
    rowsContent->addAndMakeVisible(*row->offsetLabel);

    row->visual = std::make_unique<LFOComponent>();
    row->visual->setShape(LFOComponent::Shape::Sine);
    row->visual->setPhaseOffset(0.0f);
    row->visual->setDrawOneFullCycleFromZero(false);
    row->visual->setExternalPhaseDriven(true);
    row->visual->setDepth(0.0f);
    rowsContent->addAndMakeVisible(*row->visual);

    if (row->offsetSlider != nullptr)
    {
        auto* visual = row->visual.get();
        auto* slider = row->offsetSlider.get();
        row->offsetSlider->onValueChange = [this, visual, slider]
        {
            if (visual == nullptr || slider == nullptr)
                return;

            // UI-only path:
            // - updates local visual immediately for snappy feedback,
            // - mirrors the same value into processor atomics for RT use.
            const float offsetCycles = (float) (slider->getValue() / 100.0);
            visual->setPhaseOffset(offsetCycles);
            pushRtLfoConfigToProcessor();
        };
    }

    if (row->depthKnob != nullptr)
    {
        auto* visual = row->visual.get();
        auto* depthKnob = row->depthKnob.get();
        row->depthKnob->onValueChange = [this, visual, depthKnob](int)
        {
            if (visual == nullptr || depthKnob == nullptr)
                return;

            const float depth = juce::jlimit(-1.0f, 1.0f, (float) depthKnob->getValue() / 100.0f);
            visual->setDepth(depth);
            pushRtLfoConfigToProcessor();
        };
    }

    if (row->destinationBox != nullptr)
    {
        auto* rowPtr = row.get();
        row->destinationBox->onChange = [this, rowPtr]
        {
            if (rowPtr == nullptr || rowPtr->destinationBox == nullptr)
                return;

            updateRowDestinationBinding(*rowPtr, rowPtr->destinationBox->getSelectedIds());
        };
    }

    for (int i = 0; i < kWaveButtonCount; ++i)
    {
        row->waveButtons[(size_t) i] = std::make_unique<MiniRoundButton>(juce::String(i + 1));
        row->waveButtons[(size_t) i]->setIcon(loadWaveIcon(i));
        auto* rowPtr = row.get();
        row->waveButtons[(size_t) i]->setTooltip("Waveform " + juce::String(i + 1));
        row->waveButtons[(size_t) i]->onClick = [this, rowPtr, i]
        {
            if (rowPtr == nullptr)
                return;
            applyRowWaveShape(*rowPtr, i);
        };
        rowsContent->addAndMakeVisible(*row->waveButtons[(size_t) i]);
    }

    row->collapseButton = std::make_unique<MiniRoundButton>("-");
    row->orderButton = std::make_unique<MiniRoundButton>("1");
    row->moveLeftButton = std::make_unique<MiniRoundButton>("<");
    row->moveRightButton = std::make_unique<MiniRoundButton>(">");
    row->closeButton = std::make_unique<MiniRoundButton>("X");
    row->addButton = std::make_unique<MiniRoundButton>("+");
    row->duplicateButton = std::make_unique<MiniRoundButton>("D");

    row->collapseButton->setTooltip("Collapse/Expand LFO");
    row->orderButton->setTooltip("LFO order");
    row->moveLeftButton->setTooltip("Move LFO left");
    row->moveRightButton->setTooltip("Move LFO right");
    row->closeButton->setTooltip("Close LFO");
    row->addButton->setTooltip("Insert LFO after");
    row->duplicateButton->setTooltip("Duplicate LFO");
    row->orderButton->setInterceptsMouseClicks(false, false);

    rowsContent->addAndMakeVisible(*row->collapseButton);
    rowsContent->addAndMakeVisible(*row->orderButton);
    rowsContent->addAndMakeVisible(*row->moveLeftButton);
    rowsContent->addAndMakeVisible(*row->moveRightButton);
    rowsContent->addAndMakeVisible(*row->closeButton);
    rowsContent->addAndMakeVisible(*row->addButton);
    rowsContent->addAndMakeVisible(*row->duplicateButton);

    applyRowWaveShape(*row, row->waveShapeIndex);
    updateRowDestinationBinding(*row, juce::Array<int>{ kDestinationNoneId });

    return row;
}

void LFOGroup::applyRowWaveShape(LfoRow& row, int shapeIndex)
{
    row.waveShapeIndex = juce::jlimit(0, kWaveButtonCount - 1, shapeIndex);

    if (row.visual != nullptr)
    {
        switch (row.waveShapeIndex)
        {
            case 0: row.visual->setShape(LFOComponent::Shape::Sine); break;
            case 1: row.visual->setShape(LFOComponent::Shape::Triangle); break;
            case 2: row.visual->setShape(LFOComponent::Shape::Saw); break;
            case 3: row.visual->setShape(LFOComponent::Shape::Square); break;
            case 4: row.visual->setShape(LFOComponent::Shape::Random); break;
            default: row.visual->setShape(LFOComponent::Shape::Sine); break;
        }
    }

    for (int i = 0; i < kWaveButtonCount; ++i)
    {
        auto& button = row.waveButtons[(size_t) i];
        if (button != nullptr)
            button->setSelected(i == row.waveShapeIndex);
    }

    pushRtLfoConfigToProcessor();
}

void LFOGroup::insertDefaultRowAfter(int rowIndex)
{
    if ((int) lfoRows.size() >= kMaxLfoCount)
        return;

    const int insertIndex = juce::jlimit(0, (int) lfoRows.size(), rowIndex + 1);
    lfoRows.insert(lfoRows.begin() + insertIndex, createDefaultRow());

    rebuildDestinationChoices();
    refreshRuntimeCaches();
    refreshRowButtons();
    pushRtLfoConfigToProcessor();
    resized();
    repaint();
}

void LFOGroup::duplicateRowAfter(int rowIndex)
{
    if ((int) lfoRows.size() >= kMaxLfoCount)
        return;

    if (!juce::isPositiveAndBelow(rowIndex, (int) lfoRows.size()))
        return;

    auto duplicate = createDefaultRow();
    auto& source = *lfoRows[(size_t) rowIndex];

    if (duplicate->rateKnob != nullptr && source.rateKnob != nullptr)
        duplicate->rateKnob->setValue(source.rateKnob->getValue(), false);

    if (duplicate->depthKnob != nullptr && source.depthKnob != nullptr)
        duplicate->depthKnob->setValue(source.depthKnob->getValue(), false);

    if (duplicate->destinationBox != nullptr && source.destinationBox != nullptr)
        duplicate->destinationBox->setSelectedIds(source.destinationBox->getSelectedIds(),
                                                  juce::dontSendNotification);

    if (duplicate->offsetSlider != nullptr && source.offsetSlider != nullptr)
        duplicate->offsetSlider->setValue(source.offsetSlider->getValue(), juce::sendNotificationSync);

    applyRowWaveShape(*duplicate, source.waveShapeIndex);
    updateRowDestinationBinding(*duplicate,
                                source.destinationBox != nullptr
                                    ? source.destinationBox->getSelectedIds()
                                    : juce::Array<int>{ kDestinationNoneId });

    duplicate->collapsed = false;

    const int insertIndex = rowIndex + 1;
    lfoRows.insert(lfoRows.begin() + insertIndex, std::move(duplicate));

    rebuildDestinationChoices();
    refreshRuntimeCaches();
    refreshRowButtons();
    pushRtLfoConfigToProcessor();
    resized();
    repaint();
}

void LFOGroup::removeRow(int rowIndex)
{
    if ((int) lfoRows.size() <= kMinLfoCount)
        return;

    if (!juce::isPositiveAndBelow(rowIndex, (int) lfoRows.size()))
        return;

    lfoRows.erase(lfoRows.begin() + rowIndex);

    refreshRuntimeCaches();
    refreshRowButtons();
    pushRtLfoConfigToProcessor();
    resized();
    repaint();
}

void LFOGroup::moveRowLeft(int rowIndex)
{
    if (!juce::isPositiveAndBelow(rowIndex, (int) lfoRows.size()) || rowIndex <= 0)
        return;

    std::swap(lfoRows[(size_t) rowIndex - 1], lfoRows[(size_t) rowIndex]);

    refreshRowButtons();
    pushRtLfoConfigToProcessor();
    resized();
    repaint();
}

void LFOGroup::moveRowRight(int rowIndex)
{
    if (!juce::isPositiveAndBelow(rowIndex, (int) lfoRows.size()) || rowIndex >= ((int) lfoRows.size() - 1))
        return;

    std::swap(lfoRows[(size_t) rowIndex], lfoRows[(size_t) rowIndex + 1]);

    refreshRowButtons();
    pushRtLfoConfigToProcessor();
    resized();
    repaint();
}

void LFOGroup::toggleRowCollapsed(int rowIndex)
{
    if (!juce::isPositiveAndBelow(rowIndex, (int) lfoRows.size()))
        return;

    auto& row = *lfoRows[(size_t) rowIndex];
    row.collapsed = !row.collapsed;

    refreshRowButtons();
    pushRtLfoConfigToProcessor();
    resized();
    repaint();
}

void LFOGroup::refreshRowButtons()
{
    const bool canGrow = ((int) lfoRows.size() < kMaxLfoCount);
    const bool canRemove = ((int) lfoRows.size() > kMinLfoCount);

    for (int i = 0; i < (int) lfoRows.size(); ++i)
    {
        auto& row = *lfoRows[(size_t) i];
        const bool canMoveLeft = (i > 0);
        const bool canMoveRight = (i < ((int) lfoRows.size() - 1));

        if (row.collapseButton != nullptr)
            row.collapseButton->onClick = [this, i] { toggleRowCollapsed(i); };

        if (row.orderButton != nullptr)
        {
            row.orderButton->setGlyph(juce::String(i + 1));
            const auto [bg, fg] = PluginColours::getIndexedNameColours(i);
            row.orderButton->setCustomColours(bg, fg);
        }

        const auto [moduleBg, moduleFg] = PluginColours::getIndexedNameColours(i);
        const int layerIndex = i % 8;

        if (row.rateKnob != nullptr)
        {
            row.rateKnob->setUseLayerColours(true);
            row.rateKnob->setLayerIndex(layerIndex);
        }

        if (row.depthKnob != nullptr)
        {
            row.depthKnob->setUseLayerColours(true);
            row.depthKnob->setLayerIndex(layerIndex);
        }

        if (row.offsetSlider != nullptr)
        {
            row.offsetSlider->setColour(juce::Slider::trackColourId, moduleBg);
            row.offsetSlider->setColour(juce::Slider::backgroundColourId, moduleFg);
            row.offsetSlider->setColour(juce::Slider::thumbColourId, moduleBg);
        }

        if (row.offsetLabel != nullptr)
            row.offsetLabel->setColour(juce::Label::textColourId, moduleBg);

        if (row.visual != nullptr)
            row.visual->setPaletteColours(moduleBg, moduleFg);

        if (row.moveLeftButton != nullptr)
            row.moveLeftButton->onClick = [this, i] { moveRowLeft(i); };

        if (row.moveRightButton != nullptr)
            row.moveRightButton->onClick = [this, i] { moveRowRight(i); };

        if (row.closeButton != nullptr)
            row.closeButton->onClick = [this, i] { removeRow(i); };

        if (row.addButton != nullptr)
            row.addButton->onClick = [this, i] { insertDefaultRowAfter(i); };

        if (row.duplicateButton != nullptr)
            row.duplicateButton->onClick = [this, i] { duplicateRowAfter(i); };

        if (row.closeButton != nullptr)
            row.closeButton->setEnabled(canRemove);

        if (row.addButton != nullptr)
            row.addButton->setEnabled(canGrow);

        if (row.duplicateButton != nullptr)
            row.duplicateButton->setEnabled(canGrow);

        if (row.moveLeftButton != nullptr)
            row.moveLeftButton->setEnabled(canMoveLeft);

        if (row.moveRightButton != nullptr)
            row.moveRightButton->setEnabled(canMoveRight);

        if (row.collapseButton != nullptr)
            row.collapseButton->setGlyph("-");
    }
}

void LFOGroup::refreshRuntimeCaches()
{
    // Keep runtime cache vectors strictly aligned with row count.
    const auto rowCount = lfoRows.size();

    phase01.resize(rowCount, 0.0);
    lastRateIndexSent.resize(rowCount, -1);
    lastDepthSent.resize(rowCount, 2.0f);
}

void LFOGroup::resetToDefaultLayout()
{
    lfoRows.clear();
    lfoRows.reserve((size_t) kDefaultLfoCount);

    for (int i = 0; i < kDefaultLfoCount; ++i)
        lfoRows.push_back(createDefaultRow());

    cachedDestinationLaneCount = -1;
    cachedDestinationMainBarPageCount = -1;
    rebuildDestinationChoices();

    refreshRuntimeCaches();
    refreshRowButtons();
    pushRtLfoConfigToProcessor();
    resized();
    repaint();
}

const LFOGroup::DestinationTarget* LFOGroup::findDestinationTarget(int stableId) const noexcept
{
    for (const auto& target : destinationTargets)
        if (target.stableId == stableId)
            return &target;

    return nullptr;
}

void LFOGroup::rebuildDestinationChoicesIfNeeded()
{
    const int activeLaneCount = juce::jlimit(1, Lanes::kNumLanes, processor.getActiveLaneCount());
    const int mainBarPageCount = juce::jlimit(0, MidivisiViciAudioProcessor::kMainBarMaxPages,
                                              processor.getMainBarPageCount());

    // Fast-path:
    // avoid rebuilding combo content every timer tick when topology and
    // naming providers did not change.
    if (!destinationLabelsDirty &&
        activeLaneCount == cachedDestinationLaneCount &&
        mainBarPageCount == cachedDestinationMainBarPageCount &&
        !destinationTargets.empty())
        return;

    cachedDestinationLaneCount = activeLaneCount;
    cachedDestinationMainBarPageCount = mainBarPageCount;
    rebuildDestinationChoices();
}

void LFOGroup::rebuildDestinationChoices()
{
    // Regenerate destination catalog from live topology:
    // - lane-scoped harmonizer/arp morph targets
    // - mainbar morph targets
    destinationTargets.clear();
    destinationTargets.push_back({
        kDestinationNoneId,
        "None",
        PluginColours::surface,
        PluginColours::onSurface
    });
    destinationLabelsDirty = false;

    const int activeLaneCount = juce::jlimit(1, Lanes::kNumLanes, processor.getActiveLaneCount());
    const int mainBarPageCount = juce::jlimit(0, MidivisiViciAudioProcessor::kMainBarMaxPages,
                                              processor.getMainBarPageCount());
    cachedDestinationLaneCount = activeLaneCount;
    cachedDestinationMainBarPageCount = mainBarPageCount;

    for (int laneIndex = 0; laneIndex < activeLaneCount; ++laneIndex)
    {
        const auto laneName = laneDestinationLabelProvider != nullptr
                                  ? normalizeDisplayName(laneDestinationLabelProvider(laneIndex), "Untitled")
                                  : juce::String("Lane ") + juce::String(laneIndex + 1);
        const auto laneLabel = juce::String(laneIndex + 1) + " " + laneName;
        const auto [laneBg, laneFg] = laneDestinationColourProvider != nullptr
                                          ? laneDestinationColourProvider(laneIndex)
                                          : PluginColours::getIndexedNameColours(laneIndex);

        destinationTargets.push_back({ MidivisiViciAudioProcessor::LfoDestinationIds::make(
                                           laneIndex, MidivisiViciAudioProcessor::LfoDestinationIds::HarmPitch1),
                                       laneLabel + " Harm Pitch 1",
                                       laneBg,
                                       laneFg });
        destinationTargets.push_back({ MidivisiViciAudioProcessor::LfoDestinationIds::make(
                                           laneIndex, MidivisiViciAudioProcessor::LfoDestinationIds::HarmPitch2),
                                       laneLabel + " Harm Pitch 2",
                                       laneBg,
                                       laneFg });
        destinationTargets.push_back({ MidivisiViciAudioProcessor::LfoDestinationIds::make(
                                           laneIndex, MidivisiViciAudioProcessor::LfoDestinationIds::HarmPitch3),
                                       laneLabel + " Harm Pitch 3",
                                       laneBg,
                                       laneFg });
        destinationTargets.push_back({ MidivisiViciAudioProcessor::LfoDestinationIds::make(
                                           laneIndex, MidivisiViciAudioProcessor::LfoDestinationIds::HarmPitch4),
                                       laneLabel + " Harm Pitch 4",
                                       laneBg,
                                       laneFg });

        destinationTargets.push_back({ MidivisiViciAudioProcessor::LfoDestinationIds::make(
                                           laneIndex, MidivisiViciAudioProcessor::LfoDestinationIds::HarmVelMod1),
                                       laneLabel + " Harm Vel Mod 1",
                                       laneBg,
                                       laneFg });
        destinationTargets.push_back({ MidivisiViciAudioProcessor::LfoDestinationIds::make(
                                           laneIndex, MidivisiViciAudioProcessor::LfoDestinationIds::HarmVelMod2),
                                       laneLabel + " Harm Vel Mod 2",
                                       laneBg,
                                       laneFg });
        destinationTargets.push_back({ MidivisiViciAudioProcessor::LfoDestinationIds::make(
                                           laneIndex, MidivisiViciAudioProcessor::LfoDestinationIds::HarmVelMod3),
                                       laneLabel + " Harm Vel Mod 3",
                                       laneBg,
                                       laneFg });
        destinationTargets.push_back({ MidivisiViciAudioProcessor::LfoDestinationIds::make(
                                           laneIndex, MidivisiViciAudioProcessor::LfoDestinationIds::HarmVelMod4),
                                       laneLabel + " Harm Vel Mod 4",
                                       laneBg,
                                       laneFg });

        destinationTargets.push_back({ MidivisiViciAudioProcessor::LfoDestinationIds::make(
                                           laneIndex, MidivisiViciAudioProcessor::LfoDestinationIds::ArpRateMorph),
                                       laneLabel + " Arp Rate Morph",
                                       laneBg,
                                       laneFg });
        destinationTargets.push_back({ MidivisiViciAudioProcessor::LfoDestinationIds::make(
                                           laneIndex, MidivisiViciAudioProcessor::LfoDestinationIds::ArpGateMorph),
                                       laneLabel + " Arp Gate Morph",
                                       laneBg,
                                       laneFg });
        destinationTargets.push_back({ MidivisiViciAudioProcessor::LfoDestinationIds::make(
                                           laneIndex, MidivisiViciAudioProcessor::LfoDestinationIds::ArpGrooveMorph),
                                       laneLabel + " Arp Groove Morph",
                                       laneBg,
                                       laneFg });
        destinationTargets.push_back({ MidivisiViciAudioProcessor::LfoDestinationIds::make(
                                           laneIndex, MidivisiViciAudioProcessor::LfoDestinationIds::ArpVelocityMorph),
                                       laneLabel + " Arp Velocity Morph",
                                       laneBg,
                                       laneFg });
    }

    for (int pageIndex = 0; pageIndex < mainBarPageCount; ++pageIndex)
    {
        const int moduleIndex = (pageIndex / MidivisiViciAudioProcessor::kMainBarMorphsPerModule) + 1;
        const int morphIndex = (pageIndex % MidivisiViciAudioProcessor::kMainBarMorphsPerModule) + 1;
        const auto fallbackLabel = "Controller " + juce::String(moduleIndex)
                                 + " Morph " + juce::String(morphIndex);
        const auto label = mainBarDestinationLabelProvider != nullptr
                               ? normalizeDisplayName(mainBarDestinationLabelProvider(pageIndex), fallbackLabel)
                               : fallbackLabel;
        const auto [moduleBg, moduleFg] = mainBarDestinationColourProvider != nullptr
                                              ? mainBarDestinationColourProvider(pageIndex)
                                              : PluginColours::getIndexedNameColours(moduleIndex - 1);

        destinationTargets.push_back({
            MidivisiViciAudioProcessor::LfoDestinationIds::makeMainBarMorph(pageIndex),
            label,
            moduleBg,
            moduleFg
        });
    }

    for (auto& row : lfoRows)
    {
        if (row == nullptr || row->destinationBox == nullptr)
            continue;

        const auto previousStableIds = row->destinationBox->getSelectedIds();

        row->destinationBox->clear();
        row->destinationBox->clearItemColourPairs();
        for (const auto& target : destinationTargets)
        {
            row->destinationBox->addItem(target.label, target.stableId);
            row->destinationBox->setItemColourPair(target.stableId, target.background, target.foreground);
        }

        // Keep user selection stable across catalog rebuilds:
        // only drop IDs that no longer exist in the regenerated catalog.
        juce::Array<int> resolvedStableIds;
        for (const int previousId : previousStableIds)
        {
            if (findDestinationTarget(previousId) == nullptr)
                continue;
            resolvedStableIds.addIfNotAlreadyThere(previousId);
        }
        if (resolvedStableIds.isEmpty())
            resolvedStableIds.add(kDestinationNoneId);

        row->destinationBox->setSelectedIds(resolvedStableIds, juce::dontSendNotification);
        updateRowDestinationBinding(*row, resolvedStableIds);
    }

    pushRtLfoConfigToProcessor();
}

void LFOGroup::updateRowDestinationBinding(LfoRow& row, const juce::Array<int>& stableIds)
{
    juce::Array<int> resolved;
    resolved.ensureStorageAllocated(stableIds.size());

    bool hasNonNoneTarget = false;
    for (const int stableId : stableIds)
    {
        if (findDestinationTarget(stableId) == nullptr)
            continue;

        resolved.addIfNotAlreadyThere(stableId);
        if (stableId != kDestinationNoneId)
            hasNonNoneTarget = true;
    }

    // UI contract for "None":
    // - "None" is mutually exclusive with any real destination.
    // - if at least one real destination is selected, remove "None".
    if (hasNonNoneTarget)
        resolved.removeFirstMatchingValue(kDestinationNoneId);

    if (resolved.isEmpty())
        resolved.add(kDestinationNoneId);

    if (row.destinationBox != nullptr && row.destinationBox->getSelectedIds() != resolved)
        row.destinationBox->setSelectedIds(resolved, juce::dontSendNotification);

    // Persist only effective targets (exclude "None").
    row.destinationStableIds.clear();
    row.destinationStableIds.reserve((size_t) resolved.size());
    for (const int stableId : resolved)
    {
        if (stableId == kDestinationNoneId)
            continue;
        row.destinationStableIds.push_back(stableId);
    }

    pushRtLfoConfigToProcessor();
}

float LFOGroup::evaluateWaveSample(const LfoRow& row, float phase01InCycle) const noexcept
{
    const float offset = (row.offsetSlider != nullptr) ? (float) (row.offsetSlider->getValue() / 100.0) : 0.0f;
    const float t = wrap01(phase01InCycle + offset);

    switch (juce::jlimit(0, kWaveButtonCount - 1, row.waveShapeIndex))
    {
        case 0: return std::sin(t * juce::MathConstants<float>::twoPi);                   // Sine
        case 1: return 1.0f - 4.0f * std::abs(t - 0.5f);                                  // Triangle
        case 2: return 2.0f * (t - 0.5f);                                                  // Saw
        case 3: return (t < 0.5f) ? 1.0f : -1.0f;                                          // Square
        case 4:
        default:
        {
            constexpr float kStepsPerCycle = 16.0f;
            const float stepIndex = std::floor(t * kStepsPerCycle);
            const float u = randomUnit(stepIndex + 17.0f);
            return (u * 2.0f) - 1.0f;                                                     // Random S&H
        }
    }
}

void LFOGroup::pushRtLfoConfigToProcessor() noexcept
{
    // Point unique UI -> processor:
    // - UI thread lit les widgets,
    // - ecrit des valeurs simples (int) via atomics dans le processor,
    // - aucune mutation APVTS ici, donc pas de feedback loop UI.
    const int rowCount = juce::jlimit(0, kMaxLfoCount, (int) lfoRows.size());
    processor.setLfoRowCountFromUI(rowCount);

    for (size_t i = 0; i < lfoRows.size(); ++i)
    {
        const auto& row = lfoRows[i];
        if (row == nullptr)
            continue;

        MidivisiViciAudioProcessor::LfoRtRowConfig config;
        config.rateIndex = (row->rateKnob != nullptr) ? row->rateKnob->getValue() : 3;
        config.depth = (row->depthKnob != nullptr) ? row->depthKnob->getValue() : 0;
        config.offset = (row->offsetSlider != nullptr) ? juce::roundToInt(row->offsetSlider->getValue()) : 0;
        config.waveShape = row->waveShapeIndex;
        config.destinationCount = juce::jlimit(0,
                                               MidivisiViciAudioProcessor::kMaxLfoDestinationsPerRow,
                                               (int) row->destinationStableIds.size());
        for (int destinationIndex = 0; destinationIndex < config.destinationCount; ++destinationIndex)
            config.destinationStableIds[(size_t) destinationIndex] =
                row->destinationStableIds[(size_t) destinationIndex];

        // Single writer (UI thread) -> lock-free reader (audio thread).
        // Values are scalar atomics, so publication is deterministic and bounded.
        processor.setLfoRowConfigFromUI((int) i, config);
    }
}

void LFOGroup::layoutRow(LfoRow& row, const juce::Rectangle<int>& bounds)
{
    // Per-card layout contract:
    // - keep module-level margins consistent with other modules,
    // - keep left action column fixed,
    // - collapse mode hides all controls except action column.
    row.lastBounds = bounds;

    auto area = bounds.withTrimmedLeft(UiMetrics::kModuleInnerMargin)
                      .withTrimmedRight(UiMetrics::kModuleInnerMargin)
                      .reduced(0, UiMetrics::kModuleInnerMargin);
    const int buttonSize = juce::jlimit(10, 18,
                                        (area.getHeight() - ((kActionButtonCount - 1) * kButtonGap))
                                            / kActionButtonCount);

    auto actionColumn = area.removeFromLeft(buttonSize);
    int y = actionColumn.getY();

    auto placeButton = [&](std::unique_ptr<MiniRoundButton>& button)
    {
        if (button == nullptr)
            return;

        button->setVisible(true);
        button->setBounds(actionColumn.getX(), y, buttonSize, buttonSize);
        y += buttonSize + kButtonGap;
    };

    placeButton(row.collapseButton);
    placeButton(row.orderButton);
    placeButton(row.moveLeftButton);
    placeButton(row.moveRightButton);
    placeButton(row.closeButton);
    placeButton(row.addButton);
    placeButton(row.duplicateButton);

    if (row.collapsed)
    {
        if (row.rateKnob != nullptr)        row.rateKnob->setVisible(false);
        if (row.depthKnob != nullptr)       row.depthKnob->setVisible(false);
        if (row.destinationBox != nullptr)  row.destinationBox->setVisible(false);
        if (row.offsetSlider != nullptr)    row.offsetSlider->setVisible(false);
        if (row.offsetLabel != nullptr)     row.offsetLabel->setVisible(false);
        if (row.visual != nullptr)          row.visual->setVisible(false);
        for (auto& button : row.waveButtons)
            if (button != nullptr)
                button->setVisible(false);

        if (row.rateKnob != nullptr)        row.rateKnob->setBounds({});
        if (row.depthKnob != nullptr)       row.depthKnob->setBounds({});
        if (row.destinationBox != nullptr)  row.destinationBox->setBounds({});
        if (row.offsetSlider != nullptr)    row.offsetSlider->setBounds({});
        if (row.offsetLabel != nullptr)     row.offsetLabel->setBounds({});
        if (row.visual != nullptr)          row.visual->setBounds({});
        for (auto& button : row.waveButtons)
            if (button != nullptr)
                button->setBounds({});
        return;
    }

    if (row.rateKnob != nullptr)        row.rateKnob->setVisible(true);
    if (row.depthKnob != nullptr)       row.depthKnob->setVisible(true);
    if (row.destinationBox != nullptr)  row.destinationBox->setVisible(true);
    if (row.offsetSlider != nullptr)    row.offsetSlider->setVisible(true);
    if (row.offsetLabel != nullptr)     row.offsetLabel->setVisible(true);
    if (row.visual != nullptr)          row.visual->setVisible(true);
    for (auto& button : row.waveButtons)
        if (button != nullptr)
            button->setVisible(true);

    area.removeFromLeft(UiMetrics::kModuleInnerMargin);

    const int totalH = area.getHeight();
    const int topSectionHeight = juce::jmax(kTopSectionMinHeight, totalH - (kBottomSectionHeight + kBottomSectionGap));
    auto topSection = area.removeFromTop(topSectionHeight);
    area.removeFromTop(kBottomSectionGap);
    auto bottomSection = area.removeFromTop(kBottomSectionHeight);

    const int splitGap = 6;
    const int leftWidth = juce::jlimit(122, 146, topSection.getWidth() / 2);
    auto leftTop = topSection.removeFromLeft(leftWidth);
    topSection.removeFromLeft(splitGap);
    auto rightTop = topSection;

    const int rotaryGap = 2;
    const int rotaryWidth = juce::jmax(44, (leftTop.getWidth() - rotaryGap) / 2);
    if (row.rateKnob != nullptr)
        row.rateKnob->setBounds(leftTop.removeFromLeft(rotaryWidth));
    leftTop.removeFromLeft(rotaryGap);
    if (row.depthKnob != nullptr)
        row.depthKnob->setBounds(leftTop.removeFromLeft(rotaryWidth));

    const int destinationHeight = 18;
    const int visualGap = 2;
    if (rightTop.getHeight() > (destinationHeight + visualGap))
    {
        auto destinationArea = rightTop.removeFromBottom(destinationHeight);
        rightTop.removeFromBottom(visualGap);
        auto visualArea = rightTop.withTrimmedLeft(UiMetrics::kModuleInnerMargin)
                                  .withTrimmedTop(UiMetrics::kModuleInnerMargin)
                                  .withTrimmedBottom(UiMetrics::kModuleInnerMargin);

        if (row.destinationBox != nullptr)
        {
            row.destinationBox->setVisible(true);
            row.destinationBox->setBounds(destinationArea);
        }

        if (row.visual != nullptr)
            row.visual->setBounds(visualArea);
    }
    else
    {
        auto visualArea = rightTop.withTrimmedLeft(UiMetrics::kModuleInnerMargin)
                                  .withTrimmedTop(UiMetrics::kModuleInnerMargin)
                                  .withTrimmedBottom(UiMetrics::kModuleInnerMargin);
        if (row.visual != nullptr)
            row.visual->setBounds(visualArea);

        if (row.destinationBox != nullptr)
        {
            row.destinationBox->setVisible(false);
            row.destinationBox->setBounds({});
        }
    }

    auto leftBottom = bottomSection.removeFromLeft(leftWidth);
    bottomSection.removeFromLeft(splitGap);
    auto rightBottom = bottomSection;

    const int offsetLabelHeight = juce::jlimit(10, 14, leftBottom.getHeight() / 2);
    const int offsetLabelGap = 2;
    auto offsetLabelArea = leftBottom.removeFromTop(offsetLabelHeight);
    leftBottom.removeFromTop(offsetLabelGap);
    const auto offsetSliderArea = leftBottom;

    if (row.offsetLabel != nullptr)
        row.offsetLabel->setBounds(offsetLabelArea.withX(offsetSliderArea.getX())
                                                 .withWidth(offsetSliderArea.getWidth()));

    if (row.offsetSlider != nullptr)
        row.offsetSlider->setBounds(offsetSliderArea);

    const int waveGap = 2;
    const int availableW = juce::jmax(0, rightBottom.getWidth());
    const int idealSide = (availableW - (waveGap * (kWaveButtonCount - 1))) / kWaveButtonCount;
    const int waveSide = juce::jlimit(8, juce::jmax(8, rightBottom.getHeight()), idealSide);

    if (availableW >= (kWaveButtonCount * 8 + waveGap * (kWaveButtonCount - 1)) && waveSide >= 8)
    {
        const int totalW = (kWaveButtonCount * waveSide) + (waveGap * (kWaveButtonCount - 1));
        int bx = rightBottom.getX() + juce::jmax(0, (rightBottom.getWidth() - totalW) / 2);
        const int by = rightBottom.getY() + juce::jmax(0, (rightBottom.getHeight() - waveSide) / 2);

        for (int i = 0; i < kWaveButtonCount; ++i)
        {
            auto& button = row.waveButtons[(size_t) i];
            if (button == nullptr)
                continue;

            button->setVisible(true);
            button->setBounds(bx, by, waveSide, waveSide);
            bx += waveSide + waveGap;
        }
    }
    else
    {
        for (auto& button : row.waveButtons)
        {
            if (button == nullptr)
                continue;
            button->setVisible(false);
            button->setBounds({});
        }
    }
}

void LFOGroup::resized()
{
    // Viewport host + horizontal strip layout.
    // Cards keep fixed module width; overflow is handled by horizontal scrolling.
    expandedCardBounds.clear();

    if (lfoRows.empty())
        return;

    rowsViewport.setBounds(getLocalBounds());

    auto area = rowsViewport.getLocalBounds();
    if (area.getWidth() <= 0 || area.getHeight() <= 0 || rowsContent == nullptr)
        return;

    const int contentHeight = juce::jmax(1, rowsViewport.getHeight());
    const int totalWidth = ((int) lfoRows.size() * kLfoCardWidth)
                         + (juce::jmax(0, (int) lfoRows.size() - 1) * kLaneGap);
    rowsContent->setBounds(0, 0, juce::jmax(rowsViewport.getWidth(), totalWidth), contentHeight);

    auto contentArea = rowsContent->getLocalBounds();

    int x = contentArea.getX();

    for (auto& row : lfoRows)
    {
        if (row == nullptr)
            continue;

        const int width = row->collapsed ? kCollapsedWidth : kLfoCardWidth;
        auto rowBounds = juce::Rectangle<int>(x, contentArea.getY(), width, contentArea.getHeight());
        auto rowContentBounds = rowBounds.reduced(UiMetrics::kModuleOuterMargin);

        layoutRow(*row, rowContentBounds);

        if (!row->collapsed)
            expandedCardBounds.push_back(rowContentBounds.toFloat());

        x += width + kLaneGap;
    }

    rowsContent->repaint();
}

void LFOGroup::paint(juce::Graphics& g)
{
    g.fillAll(PluginColours::background);
}

void LFOGroup::timerCallback()
{
    // Main visual scheduler:
    // - keep controls refreshed,
    // - keep destinations synchronized with topology,
    // - update phase-locked waveform cursors from host transport.
    if (!juce::MessageManager::getInstance()->isThisTheMessageThread())
        return;

    rebuildDestinationChoicesIfNeeded();

    for (auto& row : lfoRows)
    {
        if (row == nullptr)
            continue;

        if (row->rateKnob != nullptr)
            row->rateKnob->uiTimerTick();

        if (row->depthKnob != nullptr)
            row->depthKnob->uiTimerTick();

        if (row->visual != nullptr)
            row->visual->uiTimerTick();
    }

    const double bpm       = processor.getBpm();
    const bool isPlaying   = processor.getUiIsPlaying();
    const double ppqNow    = processor.getHostPpqNowForUI();

    if (isPlaying != wasPlaying)
    {
        for (auto& row : lfoRows)
            if (row != nullptr && row->visual != nullptr)
                row->visual->setActive(isPlaying);

        wasPlaying = isPlaying;
    }

    // Keep waveform amplitude display in sync even when transport is stopped.
    for (size_t i = 0; i < lfoRows.size(); ++i)
    {
        auto& row = lfoRows[i];
        if (row == nullptr || row->visual == nullptr || row->depthKnob == nullptr)
            continue;

        const float depth = juce::jlimit(-1.0f, 1.0f, (float) row->depthKnob->getValue() / 100.0f);
        if (std::abs(lastDepthSent[i] - depth) > 0.0001f)
        {
            row->visual->setDepth(depth);
            lastDepthSent[i] = depth;
        }
    }

    if (!isPlaying || bpm <= 0.0)
        return;

    const bool bpmChanged = std::abs(lastBpmSent - bpm) > 0.0001;

    std::array<double, 64> lockedPhaseByRate {};
    std::array<bool, 64> hasLockedPhaseByRate {};

    for (size_t i = 0; i < lfoRows.size(); ++i)
    {
        auto& row = lfoRows[i];
        if (row == nullptr || row->visual == nullptr || row->rateKnob == nullptr)
            continue;

        const int rateIndex = row->rateKnob->getValue();
        if ((unsigned) rateIndex >= rateOptions.size())
            continue;

        double phase = 0.0;
        const bool canLockByRate = rateIndex >= 0 && rateIndex < (int) lockedPhaseByRate.size();
        if (canLockByRate && hasLockedPhaseByRate[(size_t) rateIndex])
        {
            phase = lockedPhaseByRate[(size_t) rateIndex];
        }
        else
        {
            const double factorWhole = rateOptions[(size_t) rateIndex].second;
            const double periodBeats = factorWhole * 4.0;
            if (periodBeats <= 0.0)
                continue;

            phase = std::fmod(ppqNow / periodBeats, 1.0);
            if (phase < 0.0)
                phase += 1.0;

            if (canLockByRate)
            {
                lockedPhaseByRate[(size_t) rateIndex] = phase;
                hasLockedPhaseByRate[(size_t) rateIndex] = true;
            }
        }

        phase01[i] = phase;
    }

    for (size_t i = 0; i < lfoRows.size(); ++i)
    {
        auto& row = lfoRows[i];
        if (row == nullptr || row->visual == nullptr || row->depthKnob == nullptr || row->rateKnob == nullptr)
            continue;

        const int rateIndex = row->rateKnob->getValue();
        if ((unsigned) rateIndex >= rateOptions.size())
            continue;

        const double factorWhole = rateOptions[(size_t) rateIndex].second;
        row->visual->setPhaseCursor((float) phase01[i]);

        if (bpmChanged || lastRateIndexSent[i] != rateIndex)
        {
            row->visual->setRateFromDivision((float) factorWhole, bpm);
            lastRateIndexSent[i] = rateIndex;
        }
    }

    lastBpmSent = bpm;
}

void LFOGroup::uiTimerTick()
{
    // Entry point used by the editor-level centralized UI scheduler.
    timerCallback();
}

juce::ValueTree LFOGroup::getStateTree() const
{
    juce::ValueTree state(kLfoStateNodeID);
    state.setProperty(kLfoCountPropID, (int) lfoRows.size(), nullptr);

    for (int i = 0; i < (int) lfoRows.size(); ++i)
    {
        const auto& row = lfoRows[(size_t) i];
        if (row == nullptr)
            continue;

        juce::ValueTree rowNode(kLfoRowNodeID);
        rowNode.setProperty(kLfoRowIndexPropID, i, nullptr);
        rowNode.setProperty(kLfoCollapsedPropID, row->collapsed, nullptr);
        rowNode.setProperty(kLfoRatePropID,
                            row->rateKnob != nullptr ? row->rateKnob->getValue() : 3,
                            nullptr);
        rowNode.setProperty(kLfoDepthPropID,
                            row->depthKnob != nullptr ? row->depthKnob->getValue() : 0,
                            nullptr);
        rowNode.setProperty(kLfoDestinationPropID,
                            row->destinationBox != nullptr ? row->destinationBox->getSelectedId() : kDestinationNoneId,
                            nullptr);
        rowNode.setProperty(kLfoDestinationsPropID,
                            serializeDestinationIds(row->destinationStableIds),
                            nullptr);
        rowNode.setProperty(kLfoOffsetPropID,
                            row->offsetSlider != nullptr ? juce::roundToInt(row->offsetSlider->getValue()) : 0,
                            nullptr);
        rowNode.setProperty(kLfoWavePropID, row->waveShapeIndex, nullptr);
        state.addChild(rowNode, -1, nullptr);
    }

    return state;
}

void LFOGroup::applyStateTree(const juce::ValueTree& state)
{
    if (!state.isValid() || !state.hasType(kLfoStateNodeID))
    {
        resetToDefaultLayout();
        return;
    }

    const int requestedCount = juce::jlimit(kMinLfoCount,
                                            kMaxLfoCount,
                                            (int) state.getProperty(kLfoCountPropID, (int) lfoRows.size()));

    while ((int) lfoRows.size() < requestedCount)
        lfoRows.push_back(createDefaultRow());

    while ((int) lfoRows.size() > requestedCount)
    {
        lfoRows.pop_back();
    }

    rebuildDestinationChoicesIfNeeded();

    std::array<juce::ValueTree, (size_t) kMaxLfoCount> indexedRows {};
    for (int c = 0; c < state.getNumChildren(); ++c)
    {
        const juce::ValueTree child = state.getChild(c);
        if (!child.isValid() || !child.hasType(kLfoRowNodeID))
            continue;

        const int idx = juce::jlimit(0, requestedCount - 1,
                                     (int) child.getProperty(kLfoRowIndexPropID, c));
        indexedRows[(size_t) idx] = child;
    }

    for (int i = 0; i < requestedCount; ++i)
    {
        auto& row = lfoRows[(size_t) i];
        if (row == nullptr)
            continue;

        const juce::ValueTree rowNode = indexedRows[(size_t) i];
        if (!rowNode.isValid())
            continue;

        const int rate = juce::jlimit(0, (int) rateOptions.size() - 1,
                                      (int) rowNode.getProperty(kLfoRatePropID,
                                                                 row->rateKnob != nullptr ? row->rateKnob->getValue() : 3));
        const int depth = juce::jlimit(-100, 100,
                                       (int) rowNode.getProperty(kLfoDepthPropID,
                                                                  row->depthKnob != nullptr ? row->depthKnob->getValue() : 0));
        juce::Array<int> destinationIds;
        if (rowNode.hasProperty(kLfoDestinationsPropID))
        {
            destinationIds = parseDestinationIds(
                rowNode.getProperty(kLfoDestinationsPropID, juce::String()).toString());
        }
        if (destinationIds.isEmpty())
        {
            // Backward compatibility:
            // old snapshots stored only one destination in kLfoDestinationPropID.
            const int legacyDestinationId = juce::jmax(
                kDestinationNoneId,
                (int) rowNode.getProperty(kLfoDestinationPropID,
                                          row->destinationBox != nullptr ? row->destinationBox->getSelectedId()
                                                                         : kDestinationNoneId));
            destinationIds.add(legacyDestinationId);
        }
        const int offset = juce::jlimit(-100, 100,
                                        (int) rowNode.getProperty(kLfoOffsetPropID,
                                                                   row->offsetSlider != nullptr ? juce::roundToInt(row->offsetSlider->getValue()) : 0));
        const int wave = juce::jlimit(0, kWaveButtonCount - 1,
                                      (int) rowNode.getProperty(kLfoWavePropID, row->waveShapeIndex));

        if (row->rateKnob != nullptr)
            row->rateKnob->setValue(rate, false);

        if (row->depthKnob != nullptr)
            row->depthKnob->setValue(depth, false);

        if (row->destinationBox != nullptr)
        {
            juce::Array<int> resolvedDestinationIds;
            for (const int destinationId : destinationIds)
            {
                if (findDestinationTarget(destinationId) == nullptr)
                    continue;
                resolvedDestinationIds.addIfNotAlreadyThere(destinationId);
            }
            if (resolvedDestinationIds.isEmpty())
                resolvedDestinationIds.add(kDestinationNoneId);

            row->destinationBox->setSelectedIds(resolvedDestinationIds, juce::dontSendNotification);
            updateRowDestinationBinding(*row, resolvedDestinationIds);
        }

        if (row->offsetSlider != nullptr)
            row->offsetSlider->setValue((double) offset, juce::sendNotificationSync);

        applyRowWaveShape(*row, wave);
        row->collapsed = (bool) rowNode.getProperty(kLfoCollapsedPropID, row->collapsed);
    }

    refreshRuntimeCaches();
    refreshRowButtons();
    pushRtLfoConfigToProcessor();
    resized();
    repaint();
}
