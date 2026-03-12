/*
==============================================================================
InputMonitor.cpp
------------------------------------------------------------------------------
Role in architecture:
- UI facade for the input monitor panel (header toggle + filter buttons + list).
- Bridges APVTS bool params to UI widgets.
- Pulls MIDI events from a bounded FIFO on UI ticks and forwards accepted events
  to MidiMonitor (virtualized list/ring buffer).

Threading model:
- Message thread only for this file.
- No RT constraints here, but it must stay lightweight because timer ticks run
  frequently and share the message thread with the whole editor.

Performance contract:
- Never read unbounded FIFO amounts in one tick.
- Keep drawing simple (no heavy allocations in paint/resized loops).
==============================================================================
*/

#include "InputMonitor.h"
#include "../PluginParameters.h"
#include "../PluginColours.h"
#include "../PluginLookAndFeel.h"
#include "../UiMetrics.h"

//------------------------------------------------------------------------------
// Construction and parameter wiring
//------------------------------------------------------------------------------
InputMonitor::InputMonitor(juce::AudioProcessorValueTreeState& vts)
    : parameters(vts), monitor()
{
    monitorEnabledRaw = parameters.getRawParameterValue(ParamIDs::inputMonitorEnable);
    filterNoteRaw = parameters.getRawParameterValue(ParamIDs::inputMonitorFilterNote);
    filterControlRaw = parameters.getRawParameterValue(ParamIDs::inputMonitorFilterControl);
    filterClockRaw = parameters.getRawParameterValue(ParamIDs::inputMonitorFilterClock);
    filterEventRaw = parameters.getRawParameterValue(ParamIDs::inputMonitorFilterEvent);

    // === Header ===
    inputMonitorTitleButton.setButtonText("Input Monitor");
    inputMonitorTitleButton.setClickingTogglesState(true);
    inputMonitorTitleButton.setToggleState(true, juce::dontSendNotification);

    inputMonitorTitleButtonShadow = std::make_unique<ShadowComponent>(
        juce::DropShadow(juce::Colours::black, 0, { 0, 0 }),
        8.0f, true, true, true, true, 10);
    inputMonitorTitleButtonShadow->setInterceptsMouseClicks(false, false);
    inputMonitorTitleButtonShadow->addAndMakeVisible(inputMonitorTitleButton);
    addAndMakeVisible(*inputMonitorTitleButtonShadow);

    // === Boutons ===
    inputMonitorNotesButton.setButtonText("Notes");
    inputMonitorNotesButton.setComponentID("IM_Notes");

    inputMonitorControlsButton.setButtonText("Controls");
    inputMonitorControlsButton.setComponentID("IM_Controls");

    inputMonitorClockButton.setButtonText("Clock");
    inputMonitorClockButton.setComponentID("IM_Clock");

    inputMonitorEventButton.setButtonText("Event");
    inputMonitorEventButton.setComponentID("IM_Event");

    addAndMakeVisible(inputMonitorNotesButton);
    addAndMakeVisible(inputMonitorControlsButton);
    addAndMakeVisible(inputMonitorClockButton);
    addAndMakeVisible(inputMonitorEventButton);

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
                lf->setInputMonitorStates(
                    inputMonitorNotesButton.getToggleState(),
                    inputMonitorControlsButton.getToggleState(),
                    inputMonitorClockButton.getToggleState(),
                    inputMonitorEventButton.getToggleState());
            }
        };
    };

    // init + bind
    setToggleFromParam(inputMonitorTitleButton, ParamIDs::inputMonitorEnable);
    setToggleFromParam(inputMonitorNotesButton,   ParamIDs::inputMonitorFilterNote);
    setToggleFromParam(inputMonitorControlsButton,ParamIDs::inputMonitorFilterControl);
    setToggleFromParam(inputMonitorClockButton,   ParamIDs::inputMonitorFilterClock);
    setToggleFromParam(inputMonitorEventButton,   ParamIDs::inputMonitorFilterEvent);

    // Zone centrale
    monitor.setOpaque(true);   // fond surface géré dans MidiMonitor
    addAndMakeVisible(monitor);
}

//------------------------------------------------------------------------------
// Shared UI scheduler entry point
//------------------------------------------------------------------------------
void InputMonitor::uiTimerTick()
{
    performTimerTick();
}

//------------------------------------------------------------------------------
// Layout:
// - top: title toggle
// - middle: MidiMonitor list area
// - bottom: 4 filter toggles
//------------------------------------------------------------------------------
void InputMonitor::resized()
{
    constexpr int contentInset = UiMetrics::kModuleOuterMargin + UiMetrics::kModuleInnerMargin;
    auto area = getLocalBounds().reduced(contentInset);

    // Header
    auto titleArea = area.removeFromTop(24);
    inputMonitorTitleButtonShadow->setBounds(titleArea.expanded(10));
    inputMonitorTitleButton.setBounds(inputMonitorTitleButtonShadow->getShadowArea());

    area.removeFromTop(UiMetrics::kModuleInnerMargin);

    // Footer
    auto footerContainer = area.removeFromBottom(24 + UiMetrics::kModuleInnerMargin);
    auto footerArea = footerContainer.removeFromTop(24).reduced(UiMetrics::kModuleInnerMargin, 0);
    filterBackgroundArea = footerArea;

    const int w = footerArea.getWidth() / 4;
    inputMonitorNotesButton.setBounds(footerArea.removeFromLeft(w));
    inputMonitorControlsButton.setBounds(footerArea.removeFromLeft(w));
    inputMonitorClockButton.setBounds(footerArea.removeFromLeft(w));
    inputMonitorEventButton.setBounds(footerArea);

    // État LookAndFeel
    if (auto* lf = dynamic_cast<PluginLookAndFeel*>(&getLookAndFeel()))
    {
        lf->setInputMonitorStates(
            inputMonitorNotesButton.getToggleState(),
            inputMonitorControlsButton.getToggleState(),
            inputMonitorClockButton.getToggleState(),
            inputMonitorEventButton.getToggleState());
    }

    monitor.setBounds(area);
}

//------------------------------------------------------------------------------
// Paint static panel surfaces.
// The list body is punched out from the rounded module background so the
// embedded MidiMonitor owns that drawing area.
//------------------------------------------------------------------------------
void InputMonitor::paint(juce::Graphics& g)
{
    const auto backgroundArea = getLocalBounds().toFloat().reduced((float) UiMetrics::kModuleOuterMargin);

    // Path principal (fond arrondi) avec trou monitor
    juce::Path backgroundPath;
    backgroundPath.addRoundedRectangle(backgroundArea, UiMetrics::kModuleCornerRadius);
    juce::Path hole;
    hole.addRectangle(monitor.getBounds().toFloat());
    backgroundPath.addPath(hole);
    backgroundPath.setUsingNonZeroWinding(false);

    g.setColour(PluginColours::surface);
    g.fillPath(backgroundPath);

    // Fond pilule boutons
    if (!filterBackgroundArea.isEmpty())
    {
        juce::Path pillPath;
        float radius = PluginLookAndFeel::getDynamicCornerRadius(filterBackgroundArea);
        pillPath.addRoundedRectangle(filterBackgroundArea.toFloat(), radius);

        g.setColour(PluginColours::surface);
        g.fillPath(pillPath);
    }
}
