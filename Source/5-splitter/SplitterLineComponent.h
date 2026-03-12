/**
 * @file SplitterLineComponent.h
 * @brief One configurable branch row used by the Splitter module.
 *
 * Threading:
 * - UI thread only.
 * - Not RT-safe.
 */
#pragma once

#include <JuceHeader.h>
#include "../0-component/LabeledSlider.h"
#include "../0-component/FlatComboBox.h"
#include "../0-component/ShadowComponent.h"

/**
 * @brief Branch row exposing per-mode controls and target MIDI channel.
 */
class SplitterLineComponent : public juce::Component
{
public:
    SplitterLineComponent();

    // Propage l’index de layer vers tous les sliders
    void setLayerIndex(int idx)
    {
        layerIndex = idx;
        rangeSlider.setLayerIndex   (idx);
        voiceSlider.setLayerIndex   (idx);
        // palette appliquée via setLayerIndex() → pas besoin d’autre chose ici
    }

    void setSplitIndex(int idx)
    {
        splitIndex = idx;
        // Defaults for lines 2..5: channels 2..5.
        comboBox.setSelectedId(juce::jlimit(2, 17, idx + 3), juce::dontSendNotification);
    }
    bool hitTest (int x, int y) override;

    /** Applique la visibilité/enabled et la palette (normal ou grisé) */
    void configure(bool isActive, bool isRoundRobin);

    void setVerticalCenterRef(int y) { toggleCenterY = y; }

    /** Positionne la FlatComboBox sur un rect (coordonnées locales à la ligne). */
    void setComboBounds(juce::Rectangle<int> r)
    {
        comboOverride = r;
        hasComboOverride = true;

        if (comboShadow)
        {
            comboShadow->setBounds(r.expanded(10));
            comboBox.setBounds(comboShadow->getShadowArea());
        }
        else
        {
            comboBox.setBounds(r);
        }
    }

    /** Bounds de la combo en coordonnées du parent (Splitter). */
    juce::Rectangle<int> getComboBoundsInParent() const
    {
        auto r = comboBox.getBounds();
        if (auto* p = comboBox.getParentComponent())
            if (p != this)
                r.translate(p->getX(), p->getY());
        r.translate(getX(), getY());
        return r;
    }

    std::function<void()> onComboPreOpen;

    void resized() override;
    void paint(juce::Graphics& g) override;

    LabeledSlider rangeSlider;
    LabeledSlider voiceSlider;
    FlatComboBox  comboBox;

private:
    std::unique_ptr<ShadowComponent> comboShadow;

    int  layerIndex = -1; // 1..4 pour L2..L5 ; -1 = contrast
    int  splitIndex = -1; // 0..3 pour L2..L5

    bool roundRobinMode = true;
    bool active = false;

    int  toggleCenterY = 0; // coord Y absolue (parent Splitter)

    bool hasComboOverride = false;
    juce::Rectangle<int> comboOverride;
};
