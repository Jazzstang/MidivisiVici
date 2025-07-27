#pragma once

#include <JuceHeader.h>
#include "PluginLookAndFeel.h"

class MidiMonitor : public juce::Component,
                    private juce::Timer
{
public:
    MidiMonitor()
    {
        setOpaque(true);
        startTimerHz(30);
    }

    void clear()
    {
        juce::ScopedLock lock(bufferLock);
        messages.clear();
        repaint();
    }

    void addMessage(const juce::MidiMessage& m)
    {
        juce::ScopedLock lock(bufferLock);
        if (messages.size() > 200)
            messages.remove(0);

        messages.add(m.getDescription());
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(PluginColours::surface);
        g.setColour(PluginColours::onSurface);

        // --- Utilisation de la police Jost du LookAndFeel si dispo ---
        if (auto* lf = dynamic_cast<PluginLookAndFeel*>(&getLookAndFeel()))
            g.setFont(lf->getJostFont(12.0f, PluginLookAndFeel::JostWeight::Light, false));
        else
            g.setFont(juce::Font(13.0f));

        auto bounds = getLocalBounds().reduced(4);
        const int lineHeight = 14;
        const int maxLines = bounds.getHeight() / lineHeight;
        const int startIdx = juce::jmax(0, messages.size() - maxLines);

        int y = bounds.getY();
        for (int i = startIdx; i < messages.size(); ++i)
        {
            g.drawText(messages[i], bounds.getX(), y, bounds.getWidth(), lineHeight,
                       juce::Justification::left);
            y += lineHeight;
        }
    }
    
private:
    void timerCallback() override { repaint(); }

    juce::StringArray messages;
    juce::CriticalSection bufferLock;
};
