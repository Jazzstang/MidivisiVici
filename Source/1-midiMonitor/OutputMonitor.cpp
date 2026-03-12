/*
==============================================================================
OutputMonitor.cpp
------------------------------------------------------------------------------
Role in architecture:
- UI facade for the output monitor panel.
- Mirrors output monitor APVTS toggles (enable + Notes/Controls/Clock/Event).
- Pulls events from the output FIFO on UI ticks and forwards filtered messages
  to MidiMonitor.

Threading model:
- Message thread only.
- Must remain lightweight to avoid UI stalls while MIDI traffic is high.

Note:
- Filtering policy is intentionally parallel to InputMonitor, but parameter IDs
  are output-specific and must never be mixed with input monitor IDs.
==============================================================================
*/

#include "OutputMonitor.h"
#include "../PluginParameters.h"
#include "../PluginColours.h"
#include "../PluginLookAndFeel.h"
#include "../UiMetrics.h"

//------------------------------------------------------------------------------
// Construction and APVTS binding
//------------------------------------------------------------------------------
OutputMonitor::OutputMonitor(juce::AudioProcessorValueTreeState& vts,
                             const juce::String& title)
    : parameters(vts), monitor()
{
    monitorEnabledRaw = parameters.getRawParameterValue(ParamIDs::outputMonitorEnable);
    filterNoteRaw = parameters.getRawParameterValue(ParamIDs::outputMonitorFilterNote);
    filterControlRaw = parameters.getRawParameterValue(ParamIDs::outputMonitorFilterControl);
    filterClockRaw = parameters.getRawParameterValue(ParamIDs::outputMonitorFilterClock);
    filterEventRaw = parameters.getRawParameterValue(ParamIDs::outputMonitorFilterEvent);

    // === Header ===
    outputMonitorTitleButton.setButtonText(title);
    outputMonitorTitleButton.setClickingTogglesState(true);
    outputMonitorTitleButton.setToggleState(true, juce::dontSendNotification);

    outputMonitorTitleButtonShadow = std::make_unique<ShadowComponent>(
        juce::DropShadow(juce::Colours::black, 0, { 0, 0 }),
        8.0f, true, true, true, true, 10);
    outputMonitorTitleButtonShadow->setInterceptsMouseClicks(false, false);
    outputMonitorTitleButtonShadow->addAndMakeVisible(outputMonitorTitleButton);
    addAndMakeVisible(*outputMonitorTitleButtonShadow);

    // === Boutons ===
    outputMonitorNotesButton.setButtonText("Notes");
    outputMonitorNotesButton.setComponentID("OM_Notes");

    outputMonitorControlsButton.setButtonText("Controls");
    outputMonitorControlsButton.setComponentID("OM_Controls");

    outputMonitorClockButton.setButtonText("Clock");
    outputMonitorClockButton.setComponentID("OM_Clock");

    outputMonitorEventButton.setButtonText("Event");
    outputMonitorEventButton.setComponentID("OM_Event");

    addAndMakeVisible(outputMonitorNotesButton);
    addAndMakeVisible(outputMonitorControlsButton);
    addAndMakeVisible(outputMonitorClockButton);
    addAndMakeVisible(outputMonitorEventButton);

    // Attachments
    auto setToggleFromParam = [this](juce::ToggleButton& b, const juce::String& id)
    {
        if (auto* p = parameters.getParameter(id))
            b.setToggleState(p->getValue() > 0.5f, juce::dontSendNotification);

        b.onClick = [this, &b, id]
        {
            if (auto* p = parameters.getParameter(id))
            {
                const float v = b.getToggleState() ? 1.0f : 0.0f;
                p->beginChangeGesture();
                p->setValueNotifyingHost(v);
                p->endChangeGesture();
            }

            if (auto* lf = dynamic_cast<PluginLookAndFeel*>(&getLookAndFeel()))
            {
                lf->setOutputMonitorStates(
                    outputMonitorNotesButton.getToggleState(),
                    outputMonitorControlsButton.getToggleState(),
                    outputMonitorClockButton.getToggleState(),
                    outputMonitorEventButton.getToggleState());
            }
        };
    };

    setToggleFromParam(outputMonitorTitleButton,  ParamIDs::outputMonitorEnable);
    setToggleFromParam(outputMonitorNotesButton,  ParamIDs::outputMonitorFilterNote);
    setToggleFromParam(outputMonitorControlsButton, ParamIDs::outputMonitorFilterControl);
    setToggleFromParam(outputMonitorClockButton,  ParamIDs::outputMonitorFilterClock);
    setToggleFromParam(outputMonitorEventButton,  ParamIDs::outputMonitorFilterEvent);

    monitor.setOpaque(true);
    addAndMakeVisible(monitor);
}

//------------------------------------------------------------------------------
// Shared UI scheduler entry point
//------------------------------------------------------------------------------
void OutputMonitor::uiTimerTick()
{
    performTimerTick();
}

//------------------------------------------------------------------------------
// Pull raw APVTS values into toggle widgets.
// This guards against host automation changes done outside direct UI clicks.
//------------------------------------------------------------------------------
void OutputMonitor::syncTogglesFromParameters()
{
    auto sync = [](juce::ToggleButton& button, const std::atomic<float>* raw)
    {
        if (raw == nullptr)
            return;

        const bool state = (raw->load() > 0.5f);
        if (button.getToggleState() != state)
            button.setToggleState(state, juce::dontSendNotification);
    };

    sync(outputMonitorTitleButton, monitorEnabledRaw);
    sync(outputMonitorNotesButton, filterNoteRaw);
    sync(outputMonitorControlsButton, filterControlRaw);
    sync(outputMonitorClockButton, filterClockRaw);
    sync(outputMonitorEventButton, filterEventRaw);
}

//------------------------------------------------------------------------------
// Layout:
// - top: title toggle
// - middle: MidiMonitor list area
// - bottom: 4 filter toggles
//------------------------------------------------------------------------------
void OutputMonitor::resized()
{
    constexpr int contentInset = UiMetrics::kModuleOuterMargin + UiMetrics::kModuleInnerMargin;
    auto area = getLocalBounds().reduced(contentInset);
    
    auto titleArea = area.removeFromTop(24);
    outputMonitorTitleButtonShadow->setBounds(titleArea.expanded(10));
    outputMonitorTitleButton.setBounds(outputMonitorTitleButtonShadow->getShadowArea());

    area.removeFromTop(UiMetrics::kModuleInnerMargin);

    auto footerContainer = area.removeFromBottom(24 + UiMetrics::kModuleInnerMargin);
    auto footerArea = footerContainer.removeFromTop(24).reduced(UiMetrics::kModuleInnerMargin, 0);
    filterBackgroundArea = footerArea;

    const int w = footerArea.getWidth() / 4;
    outputMonitorNotesButton.setBounds(footerArea.removeFromLeft(w));
    outputMonitorControlsButton.setBounds(footerArea.removeFromLeft(w));
    outputMonitorClockButton.setBounds(footerArea.removeFromLeft(w));
    outputMonitorEventButton.setBounds(footerArea);

    if (auto* lf = dynamic_cast<PluginLookAndFeel*>(&getLookAndFeel()))
    {
        lf->setOutputMonitorStates(
            outputMonitorNotesButton.getToggleState(),
            outputMonitorControlsButton.getToggleState(),
            outputMonitorClockButton.getToggleState(),
            outputMonitorEventButton.getToggleState());
    }

    monitor.setBounds(area);
}

//------------------------------------------------------------------------------
// Paint static panel surfaces (rounded module + footer pill).
//------------------------------------------------------------------------------
void OutputMonitor::paint(juce::Graphics& g)
{
    const auto backgroundArea = getLocalBounds().toFloat().reduced((float) UiMetrics::kModuleOuterMargin);

    juce::Path backgroundPath;
    backgroundPath.addRoundedRectangle(backgroundArea, UiMetrics::kModuleCornerRadius);
    juce::Path hole;
    hole.addRectangle(monitor.getBounds().toFloat());
    backgroundPath.addPath(hole);
    backgroundPath.setUsingNonZeroWinding(false);

    g.setColour(PluginColours::surface);
    g.fillPath(backgroundPath);

    if (!filterBackgroundArea.isEmpty())
    {
        juce::Path pillPath;
        float radius = PluginLookAndFeel::getDynamicCornerRadius(filterBackgroundArea);
        pillPath.addRoundedRectangle(filterBackgroundArea.toFloat(), radius);

        g.setColour(PluginColours::surface);
        g.fillPath(pillPath);
    }
}
