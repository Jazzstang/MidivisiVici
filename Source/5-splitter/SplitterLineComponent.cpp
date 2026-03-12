/*
==============================================================================
SplitterLineComponent.cpp
------------------------------------------------------------------------------
Role du fichier:
- Vue d une branche splitter (une ligne) avec controles associes:
  - Note range (mode range split),
  - Voice limit (mode round robin),
  - Canal cible (combo).

Place dans l architecture:
- Composant UI enfant de Splitter.
- Ne traite pas de MIDI; il edite des parametres APVTS via attachments dans
  le parent Splitter.

Interaction parent/enfant:
- hitTest delegue la zone des fleches au parent pour garder un drag/click
  coherent sur les connecteurs visuels.
==============================================================================
*/

#include "SplitterLineComponent.h"
#include "Splitter.h"
#include "../PluginColours.h"

// Helper cote parent pour laisser passer certains clics (zone fleches).
bool Splitter::isInAnyArrowHitbox(juce::Point<int> ptInSplitter) const
{
    for (int lineNo = 1; lineNo <= 5; ++lineNo)
        if (arrowHitboxByLine[lineNo].contains(ptInSplitter))
            return true;
    return false;
}

SplitterLineComponent::SplitterLineComponent()
{
    setInterceptsMouseClicks(false, true);

    // Sliders:
    // - rangeSlider: actif en mode range split.
    // - voiceSlider: actif en mode round robin.
    rangeSlider.setTitle("Note Range");
    rangeSlider.setSliderStyle(juce::Slider::TwoValueHorizontal);
    rangeSlider.setRange(0, 127, 1);
    rangeSlider.setDisplayMode(LabeledSlider::DisplayMode::Note);
    addAndMakeVisible(rangeSlider);

    voiceSlider.setTitle("Voice Limit");
    voiceSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    voiceSlider.setRange(1, 16, 1);
    voiceSlider.setDisplayMode(LabeledSlider::DisplayMode::Simple);
    addAndMakeVisible(voiceSlider);

    // Combo canal cible (mute + channels 1..16).
    comboBox.addItem("mute", 1);
    for (int j = 1; j <= 16; ++j) comboBox.addItem(juce::String(j), j + 1);
    comboBox.setSelectedId(3);

    comboShadow = std::make_unique<ShadowComponent>(
        juce::DropShadow(juce::Colours::black, 0, juce::Point<int>(0, 0)),
        -1.0f, true, true, true, true, 10
    );
    comboShadow->setInterceptsMouseClicks(false, true);
    comboShadow->addAndMakeVisible(comboBox);
    addAndMakeVisible(*comboShadow);

    comboBox.onClickBeforeOpen = [this]() { if (onComboPreOpen) onComboPreOpen(); };
}

void SplitterLineComponent::configure(bool isActive, bool isRoundRobin)
{
    active = isActive;
    roundRobinMode = isRoundRobin;

    const bool showRange = (!roundRobinMode && active);
    const bool showRR    = ( roundRobinMode && active);

    rangeSlider   .setVisible(showRange);
    voiceSlider   .setVisible(showRR);

    rangeSlider   .setEnabled(active);
    voiceSlider   .setEnabled(active);
    comboBox      .setEnabled(active);

    if (active)
    {
        rangeSlider   .refreshPalette();
        voiceSlider   .refreshPalette();
    }
    else
    {
        // Etat inactif: palette neutre pour signaler clairement le bypass.
        auto grey = [&](LabeledSlider& ls)
        {
            ls.setTrackColour      (PluginColours::onSurface);
            ls.setBackgroundColour (PluginColours::surface);
            ls.setThumbColour      (PluginColours::onSurface);
            ls.setTextColour       (PluginColours::onSurface);
        };
        grey(rangeSlider); grey(voiceSlider);
    }

    resized();
}

void SplitterLineComponent::resized()
{
    auto area = getLocalBounds();

    // Zone combo a droite, zone slider a gauche.
    constexpr int comboWidth = 60;
    juce::Rectangle<int> contentArea;
    juce::Rectangle<int> comboAreaLocal;

    if (hasComboOverride)
    {
        comboAreaLocal = comboOverride;
        contentArea    = area.withTrimmedRight(comboWidth);
    }
    else
    {
        contentArea    = area;
        comboAreaLocal = contentArea.removeFromRight(comboWidth);
    }

    auto box = comboAreaLocal.withSizeKeepingCentre(comboWidth, 24);

    if (comboShadow)
    {
        comboShadow->setBounds(box.expanded(10));
        comboBox.setBounds(comboShadow->getShadowArea());
    }
    else
    {
        comboBox.setBounds(box);
    }

    // Zone sliders a gauche.
    auto sliderArea = contentArea.reduced(8);

    // Alignement vertical du track avec la ligne de branche du schema.
    const int localToggleY = toggleCenterY - getY();

    // Biais fin pour aligner avec la fleche dessinee dans le parent.
    static constexpr int kTrackBias = -24;
    const int targetY = localToggleY + kTrackBias;

    if (roundRobinMode)
    {
        voiceSlider.setBounds(sliderArea);
        rangeSlider.setBounds({});

        const int voiceCY = voiceSlider.getY()    + voiceSlider.getTrackCenterY();

        voiceSlider.setBounds(voiceSlider.getBounds().translated(0, targetY - voiceCY));
    }
    else
    {
        rangeSlider.setBounds(sliderArea);
        voiceSlider.setBounds({});

        const int rangeCY = rangeSlider.getY() + rangeSlider.getTrackCenterY();
        rangeSlider.setBounds(rangeSlider.getBounds().translated(0, targetY - rangeCY));
    }
}

void SplitterLineComponent::paint(juce::Graphics& g)
{
    juce::ignoreUnused(g);
    // Le rendu principal est assure par les enfants (sliders/combo).
}

bool SplitterLineComponent::hitTest (int x, int y)
{
    // 1) Zone fleche: laisser passer au parent (Splitter).
    if (auto* parent = getParentComponent())
    {
        const juce::Point<int> ptInParent (getX() + x, getY() + y);
        if (auto* splitter = dynamic_cast<Splitter*>(parent))
            if (splitter->isInAnyArrowHitbox(ptInParent))
                return false;
    }

    // 2) Sinon, autoriser seulement les zones enfants visibles.
    for (int i = getNumChildComponents() - 1; i >= 0; --i)
        if (auto* c = getChildComponent(i))
            if (c->isVisible() && c->getBounds().contains(x, y))
                return true;

    // 3) Zone vide: delegation au parent.
    return false;
}
