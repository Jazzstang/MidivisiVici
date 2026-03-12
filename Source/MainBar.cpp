/*
==============================================================================
MainBar.cpp
------------------------------------------------------------------------------
Role in architecture:
- UI module for CC sequencing and morph controller management.
- Left section edits the currently selected morph page (16 value knobs + 16 rate
  steps).
- Right section hosts dynamic controller modules (8 morph knobs per module).

Data flow:
- UI state lives in morphPages (single source of truth in editor).
- pushPageToProcessor()/pushAllPagesToProcessor() publish runtime page configs.
- Processor reports playback page/step for lightweight visual highlight.

Threading:
- Message thread only.
- No direct audio-thread access.
==============================================================================
*/

#include "MainBar.h"

#include "PluginProcessor.h"
#include "UiMetrics.h"
#include "4-arpeggiator/StepToggle.h"
#include "0-component/ModulePresetStore.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace
{
    static const juce::Identifier kMainBarStateNodeID           { "MAINBAR_STATE" };
    static const juce::Identifier kMainBarSelectedPagePropID    { "selectedPageIndex" };
    static const juce::Identifier kMainBarModuleNodeID          { "MAINBAR_MODULE" };
    static const juce::Identifier kMainBarModuleIndexPropID     { "index" };
    static const juce::Identifier kMainBarModuleCollapsedPropID { "collapsed" };
    static const juce::Identifier kMainBarModuleTitlePropID     { "title" };
    static const juce::Identifier kMainBarModuleColourPropID    { "colourIndex" };
    static const juce::Identifier kMainBarModuleUserNamedPropID { "userNamed" };
    static const juce::Identifier kMainBarMorphNodeID           { "MAINBAR_MORPH" };
    static const juce::Identifier kMainBarMorphIndexPropID      { "index" };
    static const juce::Identifier kMainBarMorphNamePropID       { "name" };
    static const juce::Identifier kMainBarMorphChannelPropID    { "channel" };
    static const juce::Identifier kMainBarMorphCcPropID         { "cc" };
    static const juce::Identifier kMainBarMorphValuePropID      { "morphValue" };
    static const juce::Identifier kMainBarModulePresetNodeID    { "MAINBAR_MODULE_PRESET" };

    static juce::Identifier makeStepValuePropID(int step)
    {
        return juce::Identifier("stepValue" + juce::String(step));
    }

    static juce::Identifier makeStepRatePropID(int step)
    {
        return juce::Identifier("stepRate" + juce::String(step));
    }

    static std::pair<juce::Colour, juce::Colour> getModuleNameColours(int colourIndex)
    {
        if (colourIndex < 0 || colourIndex >= 16)
            return { PluginColours::primary, PluginColours::onPrimary };

        return PluginColours::getIndexedNameColours(colourIndex);
    }

    static const char* moduleColourLabel(int idx)
    {
        static constexpr const char* labels[16] =
        {
            "Lay 1", "Lay 2", "Lay 3", "Lay 4", "Lay 5", "Lay 6", "Lay 7", "Lay 8",
            "OnLay 1", "OnLay 2", "OnLay 3", "OnLay 4", "OnLay 5", "OnLay 6", "OnLay 7", "OnLay 8"
        };
        return labels[(size_t) juce::jlimit(0, 15, idx)];
    }
}

class MainBar::MiniRoundButton : public juce::Button
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

    void paintButton(juce::Graphics& g, bool isMouseOver, bool isButtonDown) override
    {
        auto bounds = getLocalBounds().toFloat().reduced(1.0f);
        const auto side = juce::jmin(bounds.getWidth(), bounds.getHeight());
        bounds = bounds.withSizeKeepingCentre(side, side);

        auto bg = isEnabled() ? PluginColours::primary : PluginColours::disabled;
        auto fg = isEnabled() ? PluginColours::onPrimary : PluginColours::onDisabled;

        if (isMouseOver && isEnabled())
            bg = bg.brighter(0.08f);
        if (isButtonDown && isEnabled())
            bg = PluginColours::pressed;

        g.setColour(bg);
        g.fillEllipse(bounds);

        g.setColour(fg);
        g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
        g.drawFittedText(glyph, getLocalBounds(), juce::Justification::centred, 1);
    }

private:
    juce::String glyph;
};

class MainBar::NamePill : public juce::Component
{
public:
    std::function<void()> onContext;

    void setText(const juce::String& t)
    {
        const juce::String normalized = t.isNotEmpty() ? t : "Untitled";
        if (text == normalized)
            return;

        text = normalized;
        repaint();
    }

    void setColours(juce::Colour bg, juce::Colour fg)
    {
        if (background == bg && foreground == fg)
            return;

        background = bg;
        foreground = fg;
        repaint();
    }

    void setVertical(bool shouldBeVertical)
    {
        if (vertical == shouldBeVertical)
            return;

        vertical = shouldBeVertical;
        repaint();
    }

    void setSelected(bool shouldBeSelected)
    {
        if (selected == shouldBeSelected)
            return;

        selected = shouldBeSelected;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        const auto local = getLocalBounds();
        const auto b = local.toFloat().reduced(1.0f);

        g.setColour(background);
        g.fillRoundedRectangle(b, juce::jmin(b.getWidth(), b.getHeight()) * 0.5f);

        if (selected)
        {
            g.setColour(PluginColours::onPrimary.withAlpha(0.9f));
            g.drawRoundedRectangle(b.reduced(0.8f), juce::jmin(b.getWidth(), b.getHeight()) * 0.5f, 1.2f);
        }

        g.setColour(foreground);
        g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));

        if (vertical)
        {
            juce::Graphics::ScopedSaveState s(g);
            const auto c = b.getCentre();
            g.addTransform(juce::AffineTransform::rotation(-juce::MathConstants<float>::halfPi, c.x, c.y));

            juce::Rectangle<int> rotated(0, 0, local.getHeight(), local.getWidth());
            rotated.setCentre(local.getCentre());
            g.drawFittedText(text, rotated.reduced(4, 2), juce::Justification::centred, 1);
            return;
        }

        g.drawFittedText(text, local.reduced(5, 0), juce::Justification::centred, 1);
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
    bool vertical = false;
    bool selected = false;
};

class MainBar::MorphSlider : public juce::Slider
{
public:
    std::function<void()> onSelect;

    MorphSlider()
        : juce::Slider(juce::Slider::RotaryHorizontalVerticalDrag,
                       juce::Slider::NoTextBox)
    {
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
        setPaintingIsUnclipped(true);
    }

    void setDisplayValueOverride(float visualValue)
    {
        const float clamped = juce::jlimit((float) getMinimum(),
                                           (float) getMaximum(),
                                           visualValue);
        if (hasDisplayValueOverride && std::abs(displayValueOverride - clamped) < 0.0001f)
            return;

        hasDisplayValueOverride = true;
        displayValueOverride = clamped;
        repaint();
    }

    void clearDisplayValueOverride()
    {
        if (!hasDisplayValueOverride)
            return;

        hasDisplayValueOverride = false;
        repaint();
    }

    void setVisualColours(juce::Colour activeArcColour,
                          juce::Colour baseArcColour,
                          juce::Colour coreFillColour = PluginColours::onPrimary)
    {
        if (activeArc == activeArcColour
            && baseArc == baseArcColour
            && coreFill == coreFillColour)
            return;

        activeArc = activeArcColour;
        baseArc = baseArcColour;
        coreFill = coreFillColour;
        repaint();
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (onSelect)
            onSelect();

        juce::Slider::mouseDown(e);
    }

    void paint(juce::Graphics& g) override
    {
        auto boundsF = getLocalBounds().toFloat().reduced(2.0f);
        if (boundsF.getWidth() <= 0.0f || boundsF.getHeight() <= 0.0f)
            return;
        const bool enabled = isEnabled();

        const float radius = juce::jmax(2.0f, juce::jmin(boundsF.getWidth(), boundsF.getHeight()) * 0.5f - 3.0f);
        const auto center = boundsF.getCentre();

        const auto baseFillColour = coreFill;
        const auto baseArcColour = enabled ? baseArc : PluginColours::background;
        const auto activeArcColour = enabled ? activeArc : PluginColours::surface;

        if (enabled)
        {
            g.setColour(baseFillColour);
            g.fillEllipse(center.x - radius, center.y - radius, radius * 2.0f, radius * 2.0f);
        }

        const float innerRadius = radius + 2.0f;
        constexpr float thickness = 4.0f;
        constexpr float arcLength = juce::MathConstants<float>::pi * 1.5f;
        constexpr float startAngleBase = juce::MathConstants<float>::pi * 1.25f;
        const float centerAngle = startAngleBase + arcLength * 0.5f;

        juce::Path baseArc;
        baseArc.addCentredArc(center.x, center.y, innerRadius, innerRadius,
                              0.0f, startAngleBase, startAngleBase + arcLength, true);
        g.setColour(baseArcColour);
        g.strokePath(baseArc, juce::PathStrokeType(thickness));

        const float valueRange = (float) juce::jmax(1.0, getMaximum() - getMinimum());
        const float visualValue = hasDisplayValueOverride ? displayValueOverride : (float) getValue();
        const float norm = juce::jlimit(0.0f, 1.0f, (visualValue - (float) getMinimum()) / valueRange);
        const float endAngle = centerAngle + (norm - 0.5f) * arcLength;

        juce::Path valueArc;
        valueArc.addCentredArc(center.x, center.y, innerRadius, innerRadius,
                               0.0f, centerAngle, endAngle, true);
        g.setColour(activeArcColour);
        g.strokePath(valueArc, juce::PathStrokeType(thickness));

        constexpr float cursorLength = 4.0f;
        constexpr float cursorWidth = 2.0f;
        const float edgeX = center.x + innerRadius * std::cos(endAngle - juce::MathConstants<float>::halfPi);
        const float edgeY = center.y + innerRadius * std::sin(endAngle - juce::MathConstants<float>::halfPi);

        juce::Path cursorPath;
        cursorPath.addRectangle(-cursorLength * 0.5f, -cursorWidth * 0.5f, cursorLength, cursorWidth);
        cursorPath.applyTransform(juce::AffineTransform::rotation(endAngle + juce::MathConstants<float>::halfPi)
                                                 .translated(edgeX, edgeY));
        if (enabled)
            g.fillPath(cursorPath);
    }

private:
    juce::Colour activeArc { PluginColours::contrast };
    juce::Colour baseArc { PluginColours::background };
    juce::Colour coreFill { PluginColours::onPrimary };
    bool hasDisplayValueOverride = false;
    float displayValueOverride = 0.0f;
};

MainBar::RightModule::~RightModule() = default;

MainBar::MainBar(MidivisiViciAudioProcessor& processorIn)
    : processor(processorIn)
{
    // Build left-side sequencer widgets first (always present).
    for (int i = 0; i < kStepsPerPage; ++i)
    {
        auto knob = std::make_unique<MorphSlider>();
        knob->setRange(0.0, 127.0, 1.0);
        knob->setDoubleClickReturnValue(true, 64.0);
        knob->setValue(64.0, juce::dontSendNotification);
        knob->setMouseCursor(juce::MouseCursor::PointingHandCursor);
        knob->onValueChange = [this, i]
        {
            if (suppressUiCallbacks || !juce::isPositiveAndBelow(selectedPageIndex, (int) morphPages.size()))
                return;

            morphPages[(size_t) selectedPageIndex].stepValues[(size_t) i] =
                juce::jlimit(0, 127, juce::roundToInt(stepValueKnobs[(size_t) i]->getValue()));
            pushSelectedPageToProcessor();
        };

        stepValueKnobs[(size_t) i] = std::move(knob);
        addAndMakeVisible(*stepValueKnobs[(size_t) i]);

        auto step = std::make_unique<StepToggle>();
        step->setLayer(0);
        step->setChoiceLabels(&arpStepLayerChoices[0]);
        step->setMinimumChoiceIndex(i == 0 ? 1 : 0);
        step->setValue(i == 0 ? 5 : 0, false);

        step->onValueChanged = [this, i](int newValue)
        {
            if (suppressUiCallbacks || !juce::isPositiveAndBelow(selectedPageIndex, (int) morphPages.size()))
                return;

            int clamped = juce::jlimit(0, 43, newValue);
            if (i == 0 && clamped == 0)
                clamped = 5;

            morphPages[(size_t) selectedPageIndex].stepRates[(size_t) i] = clamped;
            updateStepValueEnablement();
            pushSelectedPageToProcessor();
        };

        step->onLeftClick = [this, i]
        {
            if (!juce::isPositiveAndBelow(selectedPageIndex, (int) morphPages.size()))
                return;

            if (i == 0)
                return;

            auto& page = morphPages[(size_t) selectedPageIndex];
            const int current = page.stepRates[(size_t) i];

            if (current != 0)
            {
                page.stepRates[(size_t) i] = 0;
                if (rateSteps[(size_t) i] != nullptr)
                    rateSteps[(size_t) i]->setValue(0, true);
                return;
            }

            int copied = 0;
            for (int prev = i - 1; prev >= 0; --prev)
            {
                const int candidate = page.stepRates[(size_t) prev];
                if (candidate != 0)
                {
                    copied = candidate;
                    break;
                }
            }

            if (copied == 0)
                copied = 5;

            page.stepRates[(size_t) i] = copied;
            if (rateSteps[(size_t) i] != nullptr)
                rateSteps[(size_t) i]->setValue(copied, true);
        };

        rateSteps[(size_t) i] = std::move(step);
        addAndMakeVisible(*rateSteps[(size_t) i]);
    }

    ensureDefaultRightModules();
    refreshRightModuleCallbacks();
    syncSelectedPageToControls();
    pushAllPagesToProcessor();
    notifyDestinationNamesChanged();
}

MainBar::~MainBar() = default;

void MainBar::setOnDestinationNamesChanged(std::function<void()> cb)
{
    onDestinationNamesChanged = std::move(cb);
}

juce::String MainBar::getLfoDestinationLabelForPage(int pageIndex) const
{
    if (!juce::isPositiveAndBelow(pageIndex, (int) morphPages.size()))
        return {};

    const int moduleIndex = pageIndex / kMorphPerModule;
    const auto fallbackController = "Controler " + juce::String(moduleIndex + 1);
    const auto fallbackMorph = "Morph " + juce::String((pageIndex % kMorphPerModule) + 1);

    juce::String controllerName = fallbackController;
    if (juce::isPositiveAndBelow(moduleIndex, (int) rightModules.size()) &&
        rightModules[(size_t) moduleIndex] != nullptr)
    {
        const auto title = rightModules[(size_t) moduleIndex]->title.trim();
        if (title.isNotEmpty())
            controllerName = title;
    }

    juce::String morphName = fallbackMorph;
    const auto pageName = morphPages[(size_t) pageIndex].name.trim();
    if (pageName.isNotEmpty())
        morphName = pageName;

    return juce::String(moduleIndex + 1) + " " + controllerName + " " + morphName;
}

std::pair<juce::Colour, juce::Colour> MainBar::getLfoDestinationColoursForPage(int pageIndex) const
{
    if (!juce::isPositiveAndBelow(pageIndex, (int) morphPages.size()))
        return { PluginColours::primary, PluginColours::onPrimary };

    const int moduleIndex = pageIndex / kMorphPerModule;
    if (juce::isPositiveAndBelow(moduleIndex, (int) rightModules.size()) &&
        rightModules[(size_t) moduleIndex] != nullptr)
    {
        return getModuleNameColours(rightModules[(size_t) moduleIndex]->colourIndex);
    }

    return PluginColours::getIndexedNameColours(moduleIndex);
}

void MainBar::notifyDestinationNamesChanged()
{
    if (onDestinationNamesChanged != nullptr)
        onDestinationNamesChanged();
}

std::unique_ptr<MainBar::RightModule> MainBar::createRightModule(int moduleIndex)
{
    // Build one controller module (actions + 8 morph controls).
    auto module = std::make_unique<RightModule>();

    module->collapseButton = std::make_unique<MiniRoundButton>("-");
    module->closeButton = std::make_unique<MiniRoundButton>("X");
    module->addButton = std::make_unique<MiniRoundButton>("+");
    module->duplicateButton = std::make_unique<MiniRoundButton>("D");
    module->titlePill = std::make_unique<NamePill>();
    module->titlePill->setVertical(true);

    addAndMakeVisible(*module->collapseButton);
    addAndMakeVisible(*module->closeButton);
    addAndMakeVisible(*module->addButton);
    addAndMakeVisible(*module->duplicateButton);
    addAndMakeVisible(*module->titlePill);

    for (int morph = 0; morph < kMorphPerModule; ++morph)
    {
        auto& control = module->morphControls[(size_t) morph];

        control.knob = std::make_unique<MorphSlider>();
        control.knob->setRange(0.0, 100.0, 1.0);
        control.knob->setDoubleClickReturnValue(true, 50.0);
        control.knob->setValue(50.0, juce::dontSendNotification);

        control.labelPill = std::make_unique<NamePill>();
        control.labelPill->setVertical(false);

        addAndMakeVisible(*control.knob);
        addAndMakeVisible(*control.labelPill);
    }

    module->title = "Controler " + juce::String(moduleIndex + 1);
    module->colourIndex = pickUniqueRandomModuleColour(moduleIndex);
    module->userNamed = false;

    return module;
}

void MainBar::ensureDefaultRightModules()
{
    if (!rightModules.empty())
        return;

    rightModules.reserve(kDefaultRightModuleCount);
    morphPages.reserve((size_t) kDefaultRightModuleCount * kMorphPerModule);

    for (int moduleIndex = 0; moduleIndex < kDefaultRightModuleCount; ++moduleIndex)
    {
        rightModules.push_back(createRightModule(moduleIndex));

        for (int morph = 0; morph < kMorphPerModule; ++morph)
        {
            MorphPageState page;
            page.name = "Morph " + juce::String(morph + 1);
            page.channel = juce::jlimit(1, 16, moduleIndex + 1);
            page.ccNumber = juce::jlimit(0, 127, morph + 1);
            page.morphValue = 50;
            page.stepValues.fill(64);
            page.stepRates.fill(0);
            page.stepRates[0] = 5;
            morphPages.push_back(page);
        }
    }

    selectedPageIndex = 0;
}

void MainBar::setLeftModuleWidth(int widthPx)
{
    const int clamped = juce::jmax(1, widthPx);
    if (leftModuleWidthPx == clamped)
        return;

    leftModuleWidthPx = clamped;
    resized();
    repaint();
}

int MainBar::getPageIndexFor(int moduleIndex, int morphIndex) const noexcept
{
    if (!juce::isPositiveAndBelow(moduleIndex, (int) rightModules.size()))
        return -1;
    if (!juce::isPositiveAndBelow(morphIndex, kMorphPerModule))
        return -1;

    const int pageIndex = moduleIndex * kMorphPerModule + morphIndex;
    if (!juce::isPositiveAndBelow(pageIndex, (int) morphPages.size()))
        return -1;

    return pageIndex;
}

void MainBar::setSelectedPageIndex(int newIndex, bool syncUi)
{
    // Page selection drives both right-side highlight and left-side sequencer data.
    if (morphPages.empty())
        return;

    const int clamped = juce::jlimit(0, (int) morphPages.size() - 1, newIndex);
    if (selectedPageIndex == clamped)
        return;

    selectedPageIndex = clamped;
    lastPlaybackPageIndex = -1;
    lastPlaybackStepIndex = -1;
    for (auto& step : rateSteps)
        if (step != nullptr)
            step->setPlayhead(false);

    refreshRightModuleCallbacks();
    updateCcSequencerKnobColours();

    if (syncUi)
        syncSelectedPageToControls();
}

void MainBar::syncSelectedPageToControls()
{
    if (!juce::isPositiveAndBelow(selectedPageIndex, (int) morphPages.size()))
        return;

    suppressUiCallbacks = true;

    auto& page = morphPages[(size_t) selectedPageIndex];
    if (page.stepRates[0] == 0)
        page.stepRates[0] = 5;

    for (int i = 0; i < kStepsPerPage; ++i)
    {
        if (stepValueKnobs[(size_t) i] != nullptr)
            stepValueKnobs[(size_t) i]->setValue(page.stepValues[(size_t) i], juce::dontSendNotification);

        if (rateSteps[(size_t) i] != nullptr)
        {
            rateSteps[(size_t) i]->setLayer(0);
            rateSteps[(size_t) i]->setChoiceLabels(&arpStepLayerChoices[0]);
            rateSteps[(size_t) i]->setMinimumChoiceIndex(i == 0 ? 1 : 0);
            int value = juce::jlimit(0, 43, page.stepRates[(size_t) i]);
            if (i == 0 && value == 0)
                value = 5;
            rateSteps[(size_t) i]->setValue(value, false);
        }
    }

    suppressUiCallbacks = false;
    updateCcSequencerKnobColours();
    updateStepValueEnablement();
}

void MainBar::updateCcSequencerKnobColours()
{
    juce::Colour activeArc = PluginColours::contrast;
    juce::Colour baseArc = PluginColours::background;

    if (juce::isPositiveAndBelow(selectedPageIndex, (int) morphPages.size()))
    {
        const int moduleIndex = selectedPageIndex / kMorphPerModule;
        if (juce::isPositiveAndBelow(moduleIndex, (int) rightModules.size()))
        {
            const int colourIndex = rightModules[(size_t) moduleIndex] != nullptr
                                        ? rightModules[(size_t) moduleIndex]->colourIndex
                                        : -1;
            const auto [moduleBg, moduleFg] = getModuleNameColours(colourIndex);
            activeArc = moduleBg;
            baseArc = moduleFg;
        }
    }

    for (auto& knob : stepValueKnobs)
    {
        if (knob != nullptr)
            knob->setVisualColours(activeArc, baseArc, PluginColours::onPrimary);
    }
}

void MainBar::updateStepValueEnablement()
{
    if (!juce::isPositiveAndBelow(selectedPageIndex, (int) morphPages.size()))
        return;

    const auto& page = morphPages[(size_t) selectedPageIndex];
    for (int i = 0; i < kStepsPerPage; ++i)
    {
        if (stepValueKnobs[(size_t) i] == nullptr)
            continue;

        const bool isSkip = (i > 0 && page.stepRates[(size_t) i] == 0);
        stepValueKnobs[(size_t) i]->setEnabled(!isSkip);
        stepValueKnobs[(size_t) i]->setAlpha(isSkip ? 0.35f : 1.0f);
    }
}

void MainBar::pushPageToProcessor(int pageIndex)
{
    // Publish one page runtime snapshot to the processor.
    if (!juce::isPositiveAndBelow(pageIndex, (int) morphPages.size()))
        return;

    MidivisiViciAudioProcessor::MainBarPageRtConfig cfg;
    auto& page = morphPages[(size_t) pageIndex];
    if (page.stepRates[0] == 0)
        page.stepRates[0] = 5;

    cfg.channel = page.channel;
    cfg.ccNumber = page.ccNumber;
    cfg.morphValue = page.morphValue;
    cfg.stepValues = page.stepValues;
    cfg.stepRates = page.stepRates;

    // Single UI writer -> RT atomic mirror in processor.
    processor.setMainBarPageConfigFromUI(pageIndex, cfg);
}

void MainBar::pushSelectedPageToProcessor()
{
    pushPageToProcessor(selectedPageIndex);
}

void MainBar::pushAllPagesToProcessor()
{
    // Keep page count and payload publication grouped to avoid transient
    // out-of-range reads on audio thread during topology edits.
    processor.setMainBarPageCountFromUI((int) morphPages.size());
    for (int i = 0; i < (int) morphPages.size(); ++i)
        pushPageToProcessor(i);
}

void MainBar::refreshRightModuleCallbacks()
{
    // Rebind callbacks after topology edits so captured indices stay valid.
    const bool canRemove = ((int) rightModules.size() > kMinRightModuleCount);
    const bool canGrow = ((int) rightModules.size() < kMaxRightModuleCount);

    for (int moduleIndex = 0; moduleIndex < (int) rightModules.size(); ++moduleIndex)
    {
        auto& module = *rightModules[(size_t) moduleIndex];

        if (module.collapseButton != nullptr)
        {
            module.collapseButton->setGlyph(module.collapsed ? "+" : "-");
            module.collapseButton->onClick = [this, moduleIndex] { toggleModuleCollapsed(moduleIndex); };
        }

        if (module.closeButton != nullptr)
        {
            module.closeButton->setEnabled(canRemove);
            module.closeButton->onClick = [this, moduleIndex] { removeModule(moduleIndex); };
        }

        if (module.addButton != nullptr)
        {
            module.addButton->setEnabled(canGrow);
            module.addButton->onClick = [this, moduleIndex] { insertModuleAfter(moduleIndex, false); };
        }

        if (module.duplicateButton != nullptr)
        {
            module.duplicateButton->setEnabled(canGrow);
            module.duplicateButton->onClick = [this, moduleIndex] { insertModuleAfter(moduleIndex, true); };
        }

        if (!module.userNamed)
            module.title = "Controler " + juce::String(moduleIndex + 1);
        else if (module.title.isEmpty())
            module.title = "Controler " + juce::String(moduleIndex + 1);

        if (!juce::isPositiveAndBelow(module.colourIndex, 16))
            module.colourIndex = pickUniqueRandomModuleColour(moduleIndex);

        const auto [moduleBg, moduleFg] = getModuleNameColours(module.colourIndex);

        if (module.titlePill != nullptr)
        {
            module.titlePill->setText(module.title);
            module.titlePill->setColours(moduleBg, moduleFg);
            module.titlePill->setVertical(true);
            module.titlePill->onContext = [this, moduleIndex]
            {
                if (!juce::isPositiveAndBelow(moduleIndex, (int) rightModules.size()))
                    return;

                auto& mod = *rightModules[(size_t) moduleIndex];
                if (mod.titlePill != nullptr)
                    showModuleNameContextMenu(moduleIndex, *mod.titlePill);
            };
        }

        for (int morph = 0; morph < kMorphPerModule; ++morph)
        {
            auto& control = module.morphControls[(size_t) morph];
            const int pageIndex = getPageIndexFor(moduleIndex, morph);
            if (!juce::isPositiveAndBelow(pageIndex, (int) morphPages.size()))
                continue;

            auto& page = morphPages[(size_t) pageIndex];

            if (control.knob != nullptr)
            {
                control.knob->setValue(page.morphValue, juce::dontSendNotification);
                control.knob->setVisualColours(moduleBg, moduleFg, PluginColours::onPrimary);
                control.knob->onSelect = [this, pageIndex]
                {
                    setSelectedPageIndex(pageIndex, true);
                };

                control.knob->onValueChange = [this, pageIndex]
                {
                    if (suppressUiCallbacks || !juce::isPositiveAndBelow(pageIndex, (int) morphPages.size()))
                        return;

                    for (int moduleIdx = 0; moduleIdx < (int) rightModules.size(); ++moduleIdx)
                    {
                        const int base = moduleIdx * kMorphPerModule;
                        const int local = pageIndex - base;
                        if (!juce::isPositiveAndBelow(local, kMorphPerModule))
                            continue;

                        auto& knob = rightModules[(size_t) moduleIdx]->morphControls[(size_t) local].knob;
                        if (knob != nullptr)
                        {
                            morphPages[(size_t) pageIndex].morphValue =
                                juce::jlimit(0, 100, juce::roundToInt(knob->getValue()));
                            pushPageToProcessor(pageIndex);
                        }
                        break;
                    }
                };
            }

            if (control.labelPill != nullptr)
            {
                control.labelPill->setText(page.name);
                control.labelPill->setColours(moduleBg, moduleFg);
                control.labelPill->setVertical(false);
                control.labelPill->setSelected(pageIndex == selectedPageIndex);
                control.labelPill->onContext = [this, pageIndex]
                {
                    if (!juce::isPositiveAndBelow(pageIndex, (int) morphPages.size()))
                        return;

                    for (int moduleIdx = 0; moduleIdx < (int) rightModules.size(); ++moduleIdx)
                    {
                        const int base = moduleIdx * kMorphPerModule;
                        const int local = pageIndex - base;
                        if (!juce::isPositiveAndBelow(local, kMorphPerModule))
                            continue;

                        auto& pill = rightModules[(size_t) moduleIdx]->morphControls[(size_t) local].labelPill;
                        if (pill != nullptr)
                            showMorphContextMenu(pageIndex, *pill);
                        break;
                    }
                };
            }
        }
    }

    updateCcSequencerKnobColours();
}

void MainBar::insertModuleAfter(int moduleIndex, bool duplicate)
{
    if ((int) rightModules.size() >= kMaxRightModuleCount)
        return;
    if (!juce::isPositiveAndBelow(moduleIndex, (int) rightModules.size()))
        return;

    const int insertIndex = moduleIndex + 1;
    auto module = createRightModule(insertIndex);

    std::array<MorphPageState, kMorphPerModule> insertedPages {};

    if (duplicate)
    {
        const auto& source = *rightModules[(size_t) moduleIndex];
        module->collapsed = source.collapsed;
        module->title = source.title;
        module->colourIndex = source.colourIndex;
        module->userNamed = source.userNamed;

        const int sourceBase = moduleIndex * kMorphPerModule;
        for (int i = 0; i < kMorphPerModule; ++i)
            insertedPages[(size_t) i] = morphPages[(size_t) (sourceBase + i)];
    }
    else
    {
        module->title = "Controler " + juce::String(insertIndex + 1);
        module->colourIndex = pickUniqueRandomModuleColour(insertIndex);
        module->userNamed = false;

        for (int i = 0; i < kMorphPerModule; ++i)
        {
            MorphPageState page;
            page.name = "Morph " + juce::String(i + 1);
            page.channel = juce::jlimit(1, 16, insertIndex + 1);
            page.ccNumber = juce::jlimit(0, 127, i + 1);
            page.morphValue = 50;
            page.stepValues.fill(64);
            page.stepRates.fill(0);
            page.stepRates[0] = 5;
            insertedPages[(size_t) i] = page;
        }
    }

    rightModules.insert(rightModules.begin() + insertIndex, std::move(module));

    const int insertPageIndex = insertIndex * kMorphPerModule;
    morphPages.insert(morphPages.begin() + insertPageIndex,
                      insertedPages.begin(),
                      insertedPages.end());

    selectedPageIndex = juce::jlimit(0, (int) morphPages.size() - 1, insertPageIndex);

    refreshRightModuleCallbacks();
    syncSelectedPageToControls();
    pushAllPagesToProcessor();
    notifyDestinationNamesChanged();
    resized();
    repaint();
}

void MainBar::removeModule(int moduleIndex)
{
    if ((int) rightModules.size() <= kMinRightModuleCount)
        return;
    if (!juce::isPositiveAndBelow(moduleIndex, (int) rightModules.size()))
        return;

    rightModules.erase(rightModules.begin() + moduleIndex);

    const int base = moduleIndex * kMorphPerModule;
    const int end = base + kMorphPerModule;
    if (juce::isPositiveAndBelow(base, (int) morphPages.size()))
    {
        const int safeEnd = juce::jlimit(base, (int) morphPages.size(), end);
        morphPages.erase(morphPages.begin() + base, morphPages.begin() + safeEnd);
    }

    if (morphPages.empty())
        selectedPageIndex = 0;
    else
        selectedPageIndex = juce::jlimit(0, (int) morphPages.size() - 1, selectedPageIndex);

    refreshRightModuleCallbacks();
    syncSelectedPageToControls();
    pushAllPagesToProcessor();
    notifyDestinationNamesChanged();
    resized();
    repaint();
}

void MainBar::toggleModuleCollapsed(int moduleIndex)
{
    if (!juce::isPositiveAndBelow(moduleIndex, (int) rightModules.size()))
        return;

    rightModules[(size_t) moduleIndex]->collapsed = !rightModules[(size_t) moduleIndex]->collapsed;
    refreshRightModuleCallbacks();
    resized();
    repaint();
}

int MainBar::pickUniqueRandomModuleColour(int moduleIndex) const noexcept
{
    return PluginColours::defaultPaletteIndexForOrder(moduleIndex);
}

void MainBar::showModuleRenameDialog(int moduleIndex)
{
    if (!juce::isPositiveAndBelow(moduleIndex, (int) rightModules.size()))
        return;

    auto* dialog = new juce::AlertWindow("Rename Controller",
                                         "Enter controller name.",
                                         juce::AlertWindow::NoIcon);
    dialog->addTextEditor("name", rightModules[(size_t) moduleIndex]->title, "Name:");
    dialog->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
    dialog->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    auto dialogSafe = juce::Component::SafePointer<juce::AlertWindow>(dialog);
    auto barSafe = juce::Component::SafePointer<MainBar>(this);

    dialog->enterModalState(true,
                            juce::ModalCallbackFunction::create(
                                [dialogSafe, barSafe, moduleIndex](int result)
                                {
                                    if (result != 1 || barSafe == nullptr || dialogSafe == nullptr)
                                        return;
                                    if (!juce::isPositiveAndBelow(moduleIndex, (int) barSafe->rightModules.size()))
                                        return;

                                    auto* te = dialogSafe->getTextEditor("name");
                                    if (te == nullptr)
                                        return;

                                    const auto name = te->getText().trim();
                                    auto& module = *barSafe->rightModules[(size_t) moduleIndex];
                                    module.title = name.isNotEmpty()
                                                       ? name
                                                       : ("Controler " + juce::String(moduleIndex + 1));
                                    module.userNamed = name.isNotEmpty();

                                    barSafe->refreshRightModuleCallbacks();
                                    barSafe->notifyDestinationNamesChanged();
                                    barSafe->repaint();
                                }),
                            true);
}

juce::ValueTree MainBar::captureModulePresetState(int moduleIndex) const
{
    if (!juce::isPositiveAndBelow(moduleIndex, (int) rightModules.size()))
        return {};

    const auto& module = rightModules[(size_t) moduleIndex];
    if (module == nullptr)
        return {};

    juce::ValueTree moduleNode(kMainBarModulePresetNodeID);
    moduleNode.setProperty(kMainBarModuleCollapsedPropID, module->collapsed, nullptr);
    moduleNode.setProperty(kMainBarModuleTitlePropID, module->title, nullptr);
    moduleNode.setProperty(kMainBarModuleColourPropID, module->colourIndex, nullptr);
    moduleNode.setProperty(kMainBarModuleUserNamedPropID, module->userNamed, nullptr);

    for (int morphIndex = 0; morphIndex < kMorphPerModule; ++morphIndex)
    {
        const int pageIndex = getPageIndexFor(moduleIndex, morphIndex);
        if (!juce::isPositiveAndBelow(pageIndex, (int) morphPages.size()))
            continue;

        const auto& page = morphPages[(size_t) pageIndex];

        juce::ValueTree morphNode(kMainBarMorphNodeID);
        morphNode.setProperty(kMainBarMorphIndexPropID, morphIndex, nullptr);
        morphNode.setProperty(kMainBarMorphNamePropID, page.name, nullptr);
        morphNode.setProperty(kMainBarMorphChannelPropID, page.channel, nullptr);
        morphNode.setProperty(kMainBarMorphCcPropID, page.ccNumber, nullptr);
        morphNode.setProperty(kMainBarMorphValuePropID, page.morphValue, nullptr);

        for (int step = 0; step < kStepsPerPage; ++step)
        {
            morphNode.setProperty(makeStepValuePropID(step), page.stepValues[(size_t) step], nullptr);
            morphNode.setProperty(makeStepRatePropID(step), page.stepRates[(size_t) step], nullptr);
        }

        moduleNode.addChild(morphNode, -1, nullptr);
    }

    return moduleNode;
}

void MainBar::applyModulePresetState(int moduleIndex, const juce::ValueTree& state)
{
    if (!juce::isPositiveAndBelow(moduleIndex, (int) rightModules.size()))
        return;
    if (!state.isValid() || !state.hasType(kMainBarModulePresetNodeID))
        return;

    auto& module = *rightModules[(size_t) moduleIndex];
    module.collapsed = (bool) state.getProperty(kMainBarModuleCollapsedPropID, module.collapsed);

    const auto restoredTitle = state.getProperty(kMainBarModuleTitlePropID, module.title).toString().trim();
    module.title = restoredTitle.isNotEmpty()
                       ? restoredTitle
                       : ("Controler " + juce::String(moduleIndex + 1));
    module.userNamed = (bool) state.getProperty(kMainBarModuleUserNamedPropID, module.userNamed)
                       && restoredTitle.isNotEmpty();
    module.colourIndex = juce::jlimit(0, 15, (int) state.getProperty(kMainBarModuleColourPropID, module.colourIndex));

    std::array<juce::ValueTree, (size_t) kMorphPerModule> morphNodes {};
    for (int c = 0; c < state.getNumChildren(); ++c)
    {
        const auto morphNode = state.getChild(c);
        if (!morphNode.isValid() || !morphNode.hasType(kMainBarMorphNodeID))
            continue;

        const int morphIndex = juce::jlimit(0,
                                            kMorphPerModule - 1,
                                            (int) morphNode.getProperty(kMainBarMorphIndexPropID, c));
        morphNodes[(size_t) morphIndex] = morphNode;
    }

    for (int morphIndex = 0; morphIndex < kMorphPerModule; ++morphIndex)
    {
        const int pageIndex = getPageIndexFor(moduleIndex, morphIndex);
        if (!juce::isPositiveAndBelow(pageIndex, (int) morphPages.size()))
            continue;

        auto& page = morphPages[(size_t) pageIndex];
        const auto morphNode = morphNodes[(size_t) morphIndex];
        if (!morphNode.isValid())
            continue;

        const auto restoredName = morphNode.getProperty(kMainBarMorphNamePropID, page.name).toString().trim();
        page.name = restoredName.isNotEmpty() ? restoredName : page.name;
        page.channel = juce::jlimit(1, 16,
                                    (int) morphNode.getProperty(kMainBarMorphChannelPropID, page.channel));
        page.ccNumber = juce::jlimit(0, 127,
                                     (int) morphNode.getProperty(kMainBarMorphCcPropID, page.ccNumber));
        page.morphValue = juce::jlimit(0, 100,
                                       (int) morphNode.getProperty(kMainBarMorphValuePropID, page.morphValue));

        for (int step = 0; step < kStepsPerPage; ++step)
        {
            page.stepValues[(size_t) step] =
                juce::jlimit(0, 127,
                             (int) morphNode.getProperty(makeStepValuePropID(step),
                                                         page.stepValues[(size_t) step]));

            int rate = juce::jlimit(0, 43,
                                    (int) morphNode.getProperty(makeStepRatePropID(step),
                                                                page.stepRates[(size_t) step]));
            if (step == 0 && rate == 0)
                rate = 5;
            page.stepRates[(size_t) step] = rate;
        }

        pushPageToProcessor(pageIndex);
    }

    refreshRightModuleCallbacks();
    syncSelectedPageToControls();
    notifyDestinationNamesChanged();
    resized();
    repaint();
}

void MainBar::showSaveModulePresetDialog(int moduleIndex)
{
    if (!juce::isPositiveAndBelow(moduleIndex, (int) rightModules.size()))
        return;

    auto* dialog = new juce::AlertWindow("Save As Controller Preset",
                                         "Enter preset name.",
                                         juce::AlertWindow::NoIcon);
    dialog->addTextEditor("name", "Preset", "Name:");
    dialog->addButton("Save As", 1, juce::KeyPress(juce::KeyPress::returnKey));
    dialog->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    auto dialogSafe = juce::Component::SafePointer<juce::AlertWindow>(dialog);
    auto barSafe = juce::Component::SafePointer<MainBar>(this);

    dialog->enterModalState(true,
                            juce::ModalCallbackFunction::create(
                                [dialogSafe, barSafe, moduleIndex](int result)
                                {
                                    if (result != 1 || dialogSafe == nullptr || barSafe == nullptr)
                                        return;
                                    if (!juce::isPositiveAndBelow(moduleIndex, (int) barSafe->rightModules.size()))
                                        return;

                                    const auto* te = dialogSafe->getTextEditor("name");
                                    const auto presetName = (te != nullptr) ? te->getText().trim() : juce::String();
                                    if (presetName.isEmpty())
                                        return;

                                    juce::String error;
                                    if (!ModulePresetStore::savePreset("cc_controller",
                                                                       presetName,
                                                                       barSafe->captureModulePresetState(moduleIndex),
                                                                       &error))
                                    {
                                        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                                               "Save Controller Preset",
                                                                               error);
                                    }
                                }),
                            true);
}

bool MainBar::saveLoadedModulePreset(int moduleIndex)
{
    if (!juce::isPositiveAndBelow(moduleIndex, (int) rightModules.size()))
        return false;

    const auto& module = rightModules[(size_t) moduleIndex];
    if (module == nullptr || !module->loadedPresetFile.existsAsFile())
        return false;

    juce::String error;
    if (!ModulePresetStore::savePresetInPlace(module->loadedPresetFile,
                                              "cc_controller",
                                              captureModulePresetState(moduleIndex),
                                              &error))
    {
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                               "Save Controller Preset",
                                               error);
        return false;
    }

    return true;
}

void MainBar::showEditModulePresetDialog(int moduleIndex)
{
    if (!juce::isPositiveAndBelow(moduleIndex, (int) rightModules.size()))
        return;

    auto& module = *rightModules[(size_t) moduleIndex];
    if (!module.loadedPresetFile.existsAsFile())
        return;

    juce::ValueTree payload;
    juce::String presetName;
    juce::String moduleKey;
    juce::String error;
    if (!ModulePresetStore::loadPresetPayload(module.loadedPresetFile,
                                              payload,
                                              &presetName,
                                              &moduleKey,
                                              &error))
    {
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                               "Edit Controller Preset",
                                               error);
        return;
    }

    auto* dialog = new juce::AlertWindow("Edit Controller Preset",
                                         "Rename or delete this preset.",
                                         juce::AlertWindow::NoIcon);
    dialog->addTextEditor("name",
                          presetName.trim().isNotEmpty() ? presetName.trim()
                                                          : module.loadedPresetFile.getFileNameWithoutExtension(),
                          "Name:");
    dialog->addButton("Rename", 1, juce::KeyPress(juce::KeyPress::returnKey));
    dialog->addButton("Delete", 2);
    dialog->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    auto dialogSafe = juce::Component::SafePointer<juce::AlertWindow>(dialog);
    auto barSafe = juce::Component::SafePointer<MainBar>(this);
    dialog->enterModalState(true,
                            juce::ModalCallbackFunction::create(
                                [dialogSafe, barSafe, moduleIndex](int result)
                                {
                                    if (result == 0 || dialogSafe == nullptr || barSafe == nullptr)
                                        return;
                                    if (!juce::isPositiveAndBelow(moduleIndex, (int) barSafe->rightModules.size()))
                                        return;

                                    auto& targetModule = *barSafe->rightModules[(size_t) moduleIndex];
                                    if (!targetModule.loadedPresetFile.existsAsFile())
                                        return;

                                    if (result == 1)
                                    {
                                        const auto* te = dialogSafe->getTextEditor("name");
                                        const auto newName = te != nullptr ? te->getText().trim() : juce::String();
                                        if (newName.isEmpty())
                                            return;

                                        juce::File renamedFile;
                                        juce::String error;
                                        if (!ModulePresetStore::renamePresetFile(targetModule.loadedPresetFile,
                                                                                 "cc_controller",
                                                                                 newName,
                                                                                 &renamedFile,
                                                                                 &error))
                                        {
                                            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                                                   "Edit Controller Preset",
                                                                                   error);
                                            return;
                                        }

                                        targetModule.loadedPresetFile = renamedFile;
                                        targetModule.loadedPresetName = newName;
                                        return;
                                    }

                                    if (result == 2)
                                    {
                                        juce::String error;
                                        if (!ModulePresetStore::deletePresetFile(targetModule.loadedPresetFile,
                                                                                 "cc_controller",
                                                                                 &error))
                                        {
                                            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                                                   "Edit Controller Preset",
                                                                                   error);
                                            return;
                                        }

                                        targetModule.loadedPresetFile = juce::File();
                                        targetModule.loadedPresetName.clear();
                                    }
                                }),
                            true);
}

void MainBar::showModuleNameContextMenu(int moduleIndex, juce::Component& target)
{
    if (!juce::isPositiveAndBelow(moduleIndex, (int) rightModules.size()))
        return;

    juce::PopupMenu menu;
    constexpr int kRenameId = 1;
    constexpr int kSavePresetId = 2;
    constexpr int kSaveAsPresetId = 3;
    constexpr int kEditPresetId = 4;
    constexpr int kAutoColourId = 5;
    constexpr int kLoadPresetBaseId = 1000;
    constexpr int kColourBaseId = 100;

    const bool canSaveLoadedPreset = rightModules[(size_t) moduleIndex]->loadedPresetFile.existsAsFile();

    menu.addItem(kRenameId, "Rename...");
    menu.addItem(kSavePresetId, "Save", canSaveLoadedPreset, false);
    menu.addItem(kSaveAsPresetId, "Save As Preset...");
    menu.addItem(kEditPresetId, "Edit Preset...", canSaveLoadedPreset, false);

    juce::PopupMenu loadPresets;
    const auto presetEntries = ModulePresetStore::listPresets("cc_controller");
    if (presetEntries.empty())
    {
        loadPresets.addItem(999999, "(No presets)", false, false);
    }
    else
    {
        for (int i = 0; i < (int) presetEntries.size(); ++i)
            loadPresets.addItem(kLoadPresetBaseId + i, presetEntries[(size_t) i].displayName);
    }
    menu.addSubMenu("Load Preset", loadPresets);
    menu.addSeparator();

    juce::PopupMenu colours;
    for (int i = 0; i < 16; ++i)
    {
        const bool selected = rightModules[(size_t) moduleIndex]->colourIndex == i;
        colours.addItem(kColourBaseId + i, moduleColourLabel(i), true, selected);
    }

    menu.addSubMenu("Colour", colours);
    menu.addSeparator();
    menu.addItem(kAutoColourId, "Auto colour", true, rightModules[(size_t) moduleIndex]->colourIndex < 0);

    auto barSafe = juce::Component::SafePointer<MainBar>(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&target),
                       [barSafe, moduleIndex, presetEntries, kRenameId, kSavePresetId, kSaveAsPresetId, kEditPresetId, kAutoColourId, kLoadPresetBaseId, kColourBaseId](int result)
                       {
                           if (barSafe == nullptr || result == 0)
                               return;
                           if (!juce::isPositiveAndBelow(moduleIndex, (int) barSafe->rightModules.size()))
                               return;

                           auto& module = *barSafe->rightModules[(size_t) moduleIndex];

                           if (result == kRenameId)
                           {
                               barSafe->showModuleRenameDialog(moduleIndex);
                               return;
                           }

                           if (result == kSavePresetId)
                           {
                               barSafe->saveLoadedModulePreset(moduleIndex);
                               return;
                           }

                           if (result == kSaveAsPresetId)
                           {
                               barSafe->showSaveModulePresetDialog(moduleIndex);
                               return;
                           }

                           if (result == kEditPresetId)
                           {
                               barSafe->showEditModulePresetDialog(moduleIndex);
                               return;
                           }

                           if (result >= kLoadPresetBaseId && result < kLoadPresetBaseId + (int) presetEntries.size())
                           {
                               const auto& entry = presetEntries[(size_t) (result - kLoadPresetBaseId)];
                               juce::ValueTree payload;
                               juce::String error;
                               if (!ModulePresetStore::loadPresetPayload(entry.file, payload, nullptr, nullptr, &error))
                               {
                                   juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                                          "Load Controller Preset",
                                                                          error);
                                   return;
                               }

                               barSafe->applyModulePresetState(moduleIndex, payload);
                               auto& targetModule = *barSafe->rightModules[(size_t) moduleIndex];
                               targetModule.loadedPresetFile = entry.file;
                               targetModule.loadedPresetName = entry.displayName;
                               return;
                           }

                           if (result == kAutoColourId)
                           {
                               module.colourIndex = barSafe->pickUniqueRandomModuleColour(moduleIndex);
                               barSafe->refreshRightModuleCallbacks();
                               barSafe->repaint();
                               return;
                           }

                           if (result >= kColourBaseId && result < kColourBaseId + 16)
                           {
                               module.colourIndex = result - kColourBaseId;
                               barSafe->refreshRightModuleCallbacks();
                               barSafe->repaint();
                           }
                       });
}

void MainBar::showMorphEditDialog(int pageIndex)
{
    if (!juce::isPositiveAndBelow(pageIndex, (int) morphPages.size()))
        return;

    auto& page = morphPages[(size_t) pageIndex];

    auto* dialog = new juce::AlertWindow("Edit Morph", "Edit morph mapping.", juce::AlertWindow::NoIcon);
    dialog->addTextEditor("name", page.name, "Name:");
    dialog->addTextEditor("channel", juce::String(page.channel), "Channel:");
    dialog->addTextEditor("cc", juce::String(page.ccNumber), "CC:");
    dialog->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
    dialog->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    auto dialogSafe = juce::Component::SafePointer<juce::AlertWindow>(dialog);
    auto barSafe = juce::Component::SafePointer<MainBar>(this);

    dialog->enterModalState(true,
                            juce::ModalCallbackFunction::create(
                                [dialogSafe, barSafe, pageIndex](int result)
                                {
                                    if (result != 1 || barSafe == nullptr || dialogSafe == nullptr)
                                        return;
                                    if (!juce::isPositiveAndBelow(pageIndex, (int) barSafe->morphPages.size()))
                                        return;

                                    auto& pageRef = barSafe->morphPages[(size_t) pageIndex];

                                    const auto name = dialogSafe->getTextEditor("name") != nullptr
                                                          ? dialogSafe->getTextEditor("name")->getText().trim()
                                                          : juce::String();
                                    const int ch = dialogSafe->getTextEditor("channel") != nullptr
                                                       ? dialogSafe->getTextEditor("channel")->getText().getIntValue()
                                                       : pageRef.channel;
                                    const int cc = dialogSafe->getTextEditor("cc") != nullptr
                                                       ? dialogSafe->getTextEditor("cc")->getText().getIntValue()
                                                       : pageRef.ccNumber;

                                    pageRef.name = name.isNotEmpty() ? name : "Morph";
                                    pageRef.channel = juce::jlimit(1, 16, ch);
                                    pageRef.ccNumber = juce::jlimit(0, 127, cc);

                                    barSafe->refreshRightModuleCallbacks();
                                    barSafe->pushPageToProcessor(pageIndex);
                                    barSafe->notifyDestinationNamesChanged();
                                    barSafe->repaint();
                                }),
                            true);
}

void MainBar::showMorphContextMenu(int pageIndex, juce::Component& target)
{
    if (!juce::isPositiveAndBelow(pageIndex, (int) morphPages.size()))
        return;

    juce::PopupMenu menu;
    menu.addItem(1, "Edit...");

    auto barSafe = juce::Component::SafePointer<MainBar>(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&target),
                       [barSafe, pageIndex](int result)
                       {
                           if (barSafe == nullptr || result == 0)
                               return;

                           if (result == 1)
                               barSafe->showMorphEditDialog(pageIndex);
                       });
}

void MainBar::paint(juce::Graphics& g)
{
    g.fillAll(PluginColours::background);

    g.setColour(PluginColours::surface);
    if (!leftModuleBounds.isEmpty())
    {
        juce::Path p;
        p.addRoundedRectangle(leftModuleBounds.toFloat(), UiMetrics::kModuleCornerRadius);
        g.fillPath(p);
    }

    for (const auto& bounds : rightModuleBounds)
    {
        if (bounds.isEmpty())
            continue;

        juce::Path p;
        p.addRoundedRectangle(bounds.toFloat(), UiMetrics::kModuleCornerRadius);
        g.fillPath(p);
    }
}

void MainBar::layoutLeftModule()
{
    // Left section contract:
    // - top row: 16 value knobs
    // - bottom row: 16 rate steps
    // Index alignment between rows is strict.
    auto hideAll = [&]()
    {
        for (auto& knob : stepValueKnobs)
            if (knob != nullptr)
                knob->setBounds({});

        for (auto& step : rateSteps)
            if (step != nullptr)
                step->setBounds({});
    };

    if (leftModuleBounds.isEmpty())
    {
        hideAll();
        return;
    }

    auto area = leftModuleBounds.reduced(UiMetrics::kModuleInnerMargin);
    if (area.getWidth() <= 0 || area.getHeight() <= 0)
    {
        hideAll();
        return;
    }

    constexpr int kColGap = 4;
    constexpr int kRowsGap = 6;

    const int totalGap = (kStepsPerPage - 1) * kColGap;
    const int colWidth = juce::jmax(12, (area.getWidth() - totalGap) / kStepsPerPage);
    const int usedWidth = (colWidth * kStepsPerPage) + totalGap;
    const int startX = area.getX() + juce::jmax(0, (area.getWidth() - usedWidth) / 2);

    const int stepRowHeight = juce::jlimit(20, 34, area.getHeight() / 2);
    const int topRowHeight = juce::jmax(14, area.getHeight() - stepRowHeight - kRowsGap);

    for (int i = 0; i < kStepsPerPage; ++i)
    {
        const int x = startX + i * (colWidth + kColGap);
        const juce::Rectangle<int> topCell(x, area.getY(), colWidth, topRowHeight);
        const juce::Rectangle<int> stepCell(x, area.getBottom() - stepRowHeight, colWidth, stepRowHeight);

        if (stepValueKnobs[(size_t) i] != nullptr)
        {
            const int knobSide = juce::jmax(10, juce::jmin(topCell.getWidth(), topCell.getHeight()));
            stepValueKnobs[(size_t) i]->setBounds(topCell.withSizeKeepingCentre(knobSide, knobSide));
        }

        if (rateSteps[(size_t) i] != nullptr)
            rateSteps[(size_t) i]->setBounds(stepCell);
    }
}

void MainBar::layoutRightModule(RightModule& module,
                                int moduleIndex,
                                const juce::Rectangle<int>& slotBounds,
                                const juce::Rectangle<int>& contentBounds)
{
    // Right module contract:
    // - action buttons on left,
    // - title pill on right,
    // - 2x4 morph controls in center (unless collapsed).
    juce::ignoreUnused(slotBounds, moduleIndex);

    auto hideMorphControls = [&]()
    {
        for (auto& control : module.morphControls)
        {
            if (control.knob != nullptr)
                control.knob->setBounds({});
            if (control.labelPill != nullptr)
                control.labelPill->setBounds({});
        }
    };

    auto hideAll = [&]()
    {
        if (module.collapseButton != nullptr) module.collapseButton->setBounds({});
        if (module.closeButton != nullptr) module.closeButton->setBounds({});
        if (module.addButton != nullptr) module.addButton->setBounds({});
        if (module.duplicateButton != nullptr) module.duplicateButton->setBounds({});
        if (module.titlePill != nullptr) module.titlePill->setBounds({});
        hideMorphControls();
    };

    if (contentBounds.isEmpty())
    {
        hideAll();
        return;
    }

    auto area = contentBounds.reduced(UiMetrics::kModuleInnerMargin);
    if (area.getWidth() <= 0 || area.getHeight() <= 0)
    {
        hideAll();
        return;
    }

    constexpr int kActionButtonCount = 4;
    constexpr int kButtonGap = 3;

    const int buttonSize = juce::jlimit(10, 18,
                                        (area.getHeight() - ((kActionButtonCount - 1) * kButtonGap))
                                            / kActionButtonCount);

    auto actionColumn = area.removeFromLeft(buttonSize);
    int y = actionColumn.getY();

    auto placeButton = [&](std::unique_ptr<MiniRoundButton>& button)
    {
        if (button == nullptr)
            return;

        button->setBounds(actionColumn.getX(), y, buttonSize, buttonSize);
        y += buttonSize + kButtonGap;
    };

    placeButton(module.collapseButton);
    placeButton(module.closeButton);
    placeButton(module.addButton);
    placeButton(module.duplicateButton);

    if (module.collapsed)
    {
        hideMorphControls();

        if (module.titlePill != nullptr && area.getWidth() >= buttonSize && area.getHeight() >= buttonSize)
            module.titlePill->setBounds(area.withWidth(buttonSize).withX(area.getRight() - buttonSize));
        else if (module.titlePill != nullptr)
            module.titlePill->setBounds({});

        return;
    }

    if (module.titlePill != nullptr && area.getWidth() >= buttonSize)
    {
        module.titlePill->setBounds(area.removeFromRight(buttonSize));
        area.removeFromRight(3);
    }
    else if (module.titlePill != nullptr)
    {
        module.titlePill->setBounds({});
    }

    constexpr int kCols = 4;
    constexpr int kRows = 2;
    constexpr int kColGap = 4;
    constexpr int kRowGap = 6;
    constexpr int kLabelGap = 3;

    const int cellW = (area.getWidth() - ((kCols - 1) * kColGap)) / kCols;
    const int cellH = (area.getHeight() - ((kRows - 1) * kRowGap)) / kRows;

    const int labelHeight = juce::jlimit(12, 20, cellH / 3);
    const int knobSide = juce::jmax(8, juce::jmin(cellW, cellH - labelHeight - kLabelGap));

    if (cellW <= 0 || cellH <= 0 || knobSide <= 0)
    {
        hideMorphControls();
        return;
    }

    for (int row = 0; row < kRows; ++row)
    {
        for (int col = 0; col < kCols; ++col)
        {
            const int morphIndex = (row * kCols) + col;
            auto& control = module.morphControls[(size_t) morphIndex];

            const int cellX = area.getX() + col * (cellW + kColGap);
            const int cellY = area.getY() + row * (cellH + kRowGap);
            juce::Rectangle<int> cell(cellX, cellY, cellW, cellH);

            if (control.labelPill != nullptr)
                control.labelPill->setBounds(cell.removeFromTop(labelHeight));

            cell.removeFromTop(kLabelGap);

            if (control.knob != nullptr)
                control.knob->setBounds(cell.withSizeKeepingCentre(knobSide, knobSide));
        }
    }
}

void MainBar::rebuildLayoutCache()
{
    // Compute visible slots for left block and dynamic right modules.
    // Layout is width-constrained; modules outside visible width are hidden.
    ensureDefaultRightModules();

    rightModuleBounds.clear();
    rightModuleBounds.resize(rightModules.size());

    auto row = getLocalBounds();
    if (row.isEmpty())
        return;

    const int requestedLeftWidth = leftModuleWidthPx > 0 ? leftModuleWidthPx : UiMetrics::kModuleWidth;
    const int firstWidth = juce::jlimit(1, row.getWidth(), requestedLeftWidth);

    leftSlotBounds = row.removeFromLeft(firstWidth);
    leftModuleBounds = leftSlotBounds.reduced(UiMetrics::kModuleOuterMargin);
    layoutLeftModule();

    int x = row.getX();
    const int rightLimit = row.getRight();
    const int h = row.getHeight();

    constexpr int kCollapsedWidth = 26;

    for (size_t i = 0; i < rightModules.size(); ++i)
    {
        auto& module = *rightModules[i];
        const int slotWidth = module.collapsed ? kCollapsedWidth : UiMetrics::kModuleWidth;

        if (x >= rightLimit || slotWidth <= 0)
        {
            rightModuleBounds[i] = {};
            layoutRightModule(module, (int) i, {}, {});
            continue;
        }

        const int clampedWidth = juce::jmin(slotWidth, rightLimit - x);
        const juce::Rectangle<int> slotBounds(x, row.getY(), clampedWidth, h);
        const juce::Rectangle<int> contentBounds = slotBounds.reduced(UiMetrics::kModuleOuterMargin);

        rightModuleBounds[i] = contentBounds;
        layoutRightModule(module, (int) i, slotBounds, contentBounds);
        x += slotWidth;
    }
}

void MainBar::resized()
{
    if (getLocalBounds().isEmpty())
        return;

    rebuildLayoutCache();
}

void MainBar::uiTimerTick()
{
    // Apply UI-only morph preview values (base + LFO) to right-module knobs.
    for (int moduleIndex = 0; moduleIndex < (int) rightModules.size(); ++moduleIndex)
    {
        auto* module = rightModules[(size_t) moduleIndex].get();
        if (module == nullptr)
            continue;

        for (int morphIndex = 0; morphIndex < kMorphPerModule; ++morphIndex)
        {
            auto& control = module->morphControls[(size_t) morphIndex];
            if (control.knob == nullptr)
                continue;

            const int pageIndex = getPageIndexFor(moduleIndex, morphIndex);
            if (!juce::isPositiveAndBelow(pageIndex, (int) morphPages.size()))
            {
                control.knob->clearDisplayValueOverride();
                continue;
            }

            const int effectiveMorphValue =
                processor.getMainBarMorphDisplayValueForUI(pageIndex);
            control.knob->setDisplayValueOverride((float) effectiveMorphValue);
        }
    }

    // Playback cursor UI update with minimal invalidation.
    // Only toggles playhead flags when index changed.
    if (!juce::isPositiveAndBelow(selectedPageIndex, (int) morphPages.size()))
    {
        if (juce::isPositiveAndBelow(lastPlaybackStepIndex, kStepsPerPage) &&
            rateSteps[(size_t) lastPlaybackStepIndex] != nullptr)
            rateSteps[(size_t) lastPlaybackStepIndex]->setPlayhead(false);

        lastPlaybackPageIndex = -1;
        lastPlaybackStepIndex = -1;
        return;
    }

    const int playbackStep = processor.getMainBarPlaybackStepForPage(selectedPageIndex);

    if (lastPlaybackPageIndex != selectedPageIndex)
    {
        if (juce::isPositiveAndBelow(lastPlaybackStepIndex, kStepsPerPage) &&
            rateSteps[(size_t) lastPlaybackStepIndex] != nullptr)
            rateSteps[(size_t) lastPlaybackStepIndex]->setPlayhead(false);

        lastPlaybackPageIndex = selectedPageIndex;
        lastPlaybackStepIndex = -1;
    }

    if (playbackStep == lastPlaybackStepIndex)
        return;

    if (juce::isPositiveAndBelow(lastPlaybackStepIndex, kStepsPerPage) &&
        rateSteps[(size_t) lastPlaybackStepIndex] != nullptr)
        rateSteps[(size_t) lastPlaybackStepIndex]->setPlayhead(false);

    if (juce::isPositiveAndBelow(playbackStep, kStepsPerPage) &&
        rateSteps[(size_t) playbackStep] != nullptr)
        rateSteps[(size_t) playbackStep]->setPlayhead(true);

    lastPlaybackStepIndex = juce::isPositiveAndBelow(playbackStep, kStepsPerPage) ? playbackStep : -1;
}

juce::ValueTree MainBar::getStateTree() const
{
    // Snapshot serialization for MainBar topology and page data.
    juce::ValueTree state(kMainBarStateNodeID);
    state.setProperty(kMainBarSelectedPagePropID, selectedPageIndex, nullptr);

    for (int moduleIndex = 0; moduleIndex < (int) rightModules.size(); ++moduleIndex)
    {
        const auto& module = rightModules[(size_t) moduleIndex];
        if (module == nullptr)
            continue;

        juce::ValueTree moduleNode(kMainBarModuleNodeID);
        moduleNode.setProperty(kMainBarModuleIndexPropID, moduleIndex, nullptr);
        moduleNode.setProperty(kMainBarModuleCollapsedPropID, module->collapsed, nullptr);
        moduleNode.setProperty(kMainBarModuleTitlePropID, module->title, nullptr);
        moduleNode.setProperty(kMainBarModuleColourPropID, module->colourIndex, nullptr);
        moduleNode.setProperty(kMainBarModuleUserNamedPropID, module->userNamed, nullptr);

        for (int morphIndex = 0; morphIndex < kMorphPerModule; ++morphIndex)
        {
            const int pageIndex = getPageIndexFor(moduleIndex, morphIndex);
            if (!juce::isPositiveAndBelow(pageIndex, (int) morphPages.size()))
                continue;

            const auto& page = morphPages[(size_t) pageIndex];

            juce::ValueTree morphNode(kMainBarMorphNodeID);
            morphNode.setProperty(kMainBarMorphIndexPropID, morphIndex, nullptr);
            morphNode.setProperty(kMainBarMorphNamePropID, page.name, nullptr);
            morphNode.setProperty(kMainBarMorphChannelPropID, page.channel, nullptr);
            morphNode.setProperty(kMainBarMorphCcPropID, page.ccNumber, nullptr);
            morphNode.setProperty(kMainBarMorphValuePropID, page.morphValue, nullptr);

            for (int step = 0; step < kStepsPerPage; ++step)
            {
                morphNode.setProperty(makeStepValuePropID(step), page.stepValues[(size_t) step], nullptr);
                morphNode.setProperty(makeStepRatePropID(step), page.stepRates[(size_t) step], nullptr);
            }

            moduleNode.addChild(morphNode, -1, nullptr);
        }

        state.addChild(moduleNode, -1, nullptr);
    }

    return state;
}

void MainBar::applyStateTree(const juce::ValueTree& state)
{
    // Snapshot restore path.
    // Rebuild modules/pages defensively to stay compatible with partial trees
    // and older snapshots missing module/morph nodes.
    rightModules.clear();
    morphPages.clear();

    const bool hasState = state.isValid() && state.hasType(kMainBarStateNodeID);

    if (!hasState)
    {
        ensureDefaultRightModules();
    }
    else
    {
        auto makeDefaultPage = [](int moduleIndex, int morphIndex)
        {
            MorphPageState page;
            page.name = "Morph " + juce::String(morphIndex + 1);
            page.channel = juce::jlimit(1, 16, moduleIndex + 1);
            page.ccNumber = juce::jlimit(0, 127, morphIndex + 1);
            page.morphValue = 50;
            page.stepValues.fill(64);
            page.stepRates.fill(0);
            page.stepRates[0] = 5; // invariant: first step cannot be Skip
            return page;
        };

        std::vector<juce::ValueTree> moduleNodes;
        moduleNodes.reserve((size_t) state.getNumChildren());

        for (int childIndex = 0; childIndex < state.getNumChildren(); ++childIndex)
        {
            const auto moduleNode = state.getChild(childIndex);
            if (moduleNode.isValid() && moduleNode.hasType(kMainBarModuleNodeID))
                moduleNodes.push_back(moduleNode);
        }

        std::sort(moduleNodes.begin(), moduleNodes.end(),
                  [](const juce::ValueTree& a, const juce::ValueTree& b)
                  {
                      const int ai = (int) a.getProperty(kMainBarModuleIndexPropID, 0);
                      const int bi = (int) b.getProperty(kMainBarModuleIndexPropID, 0);
                      return ai < bi;
                  });

        if (moduleNodes.empty())
        {
            ensureDefaultRightModules();
        }
        else
        {
            const int moduleCount = juce::jlimit(kMinRightModuleCount,
                                                 kMaxRightModuleCount,
                                                 (int) moduleNodes.size());

            rightModules.reserve((size_t) moduleCount);
            morphPages.reserve((size_t) moduleCount * kMorphPerModule);

            for (int moduleIndex = 0; moduleIndex < moduleCount; ++moduleIndex)
            {
                auto module = createRightModule(moduleIndex);

                const auto moduleNode = moduleNodes[(size_t) moduleIndex];
                module->collapsed = (bool) moduleNode.getProperty(kMainBarModuleCollapsedPropID, false);
                const auto restoredTitle = moduleNode.getProperty(kMainBarModuleTitlePropID, juce::String()).toString().trim();
                module->userNamed = (bool) moduleNode.getProperty(kMainBarModuleUserNamedPropID, false);
                module->title = restoredTitle.isNotEmpty()
                                    ? restoredTitle
                                    : ("Controler " + juce::String(moduleIndex + 1));
                if (restoredTitle.isEmpty())
                    module->userNamed = false;

                int colour = (int) moduleNode.getProperty(kMainBarModuleColourPropID, module->colourIndex);
                if (!juce::isPositiveAndBelow(colour, 16))
                    colour = pickUniqueRandomModuleColour(moduleIndex);
                module->colourIndex = colour;

                std::array<juce::ValueTree, (size_t) kMorphPerModule> morphNodes {};
                for (int c = 0; c < moduleNode.getNumChildren(); ++c)
                {
                    const auto morphNode = moduleNode.getChild(c);
                    if (!morphNode.isValid() || !morphNode.hasType(kMainBarMorphNodeID))
                        continue;

                    const int morphIndex = juce::jlimit(0,
                                                        kMorphPerModule - 1,
                                                        (int) morphNode.getProperty(kMainBarMorphIndexPropID, c));
                    morphNodes[(size_t) morphIndex] = morphNode;
                }

                for (int morphIndex = 0; morphIndex < kMorphPerModule; ++morphIndex)
                {
                    auto page = makeDefaultPage(moduleIndex, morphIndex);
                    const auto morphNode = morphNodes[(size_t) morphIndex];

                    if (morphNode.isValid())
                    {
                        const auto restoredName = morphNode.getProperty(kMainBarMorphNamePropID, page.name).toString().trim();
                        page.name = restoredName.isNotEmpty() ? restoredName : page.name;
                        page.channel = juce::jlimit(1, 16,
                                                    (int) morphNode.getProperty(kMainBarMorphChannelPropID, page.channel));
                        page.ccNumber = juce::jlimit(0, 127,
                                                     (int) morphNode.getProperty(kMainBarMorphCcPropID, page.ccNumber));
                        page.morphValue = juce::jlimit(0, 100,
                                                       (int) morphNode.getProperty(kMainBarMorphValuePropID, page.morphValue));

                        for (int step = 0; step < kStepsPerPage; ++step)
                        {
                            page.stepValues[(size_t) step] =
                                juce::jlimit(0, 127,
                                             (int) morphNode.getProperty(makeStepValuePropID(step),
                                                                         page.stepValues[(size_t) step]));

                            int rate = juce::jlimit(0, 43,
                                                    (int) morphNode.getProperty(makeStepRatePropID(step),
                                                                                page.stepRates[(size_t) step]));
                            if (step == 0 && rate == 0)
                                rate = 5;
                            page.stepRates[(size_t) step] = rate;
                        }
                    }

                    morphPages.push_back(page);
                }

                rightModules.push_back(std::move(module));
            }

            if (rightModules.empty() || morphPages.empty())
                ensureDefaultRightModules();
        }
    }

    if (morphPages.empty())
        selectedPageIndex = 0;
    else if (hasState)
        selectedPageIndex = juce::jlimit(0,
                                         (int) morphPages.size() - 1,
                                         (int) state.getProperty(kMainBarSelectedPagePropID, 0));
    else
        selectedPageIndex = 0;

    refreshRightModuleCallbacks();
    // Post-restore order matters:
    // 1) refresh visible controls from selected page
    // 2) publish full runtime mirror
    // 3) notify LFO destination providers (names/topology may have changed)
    syncSelectedPageToControls();
    pushAllPagesToProcessor();
    notifyDestinationNamesChanged();
    resized();
    repaint();
}
