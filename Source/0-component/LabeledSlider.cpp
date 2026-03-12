/*
==============================================================================
LabeledSlider.cpp
------------------------------------------------------------------------------
Role du fichier:
- Slider lineaire avec labels auxiliaires (titre, bornes, ratio, valeur).

Place dans l architecture:
- Composant UI reutilisable pour InputFilter et modules annexes.
- Le composant ne connait pas APVTS directement.

Design:
- Un seul slider interne, plusieurs modes d affichage de labels:
  - Note, Range, Ratio, Simple.
- Palette derivee d un layer index pour coherence visuelle inter-modules.

Threading:
- UI thread uniquement.
==============================================================================
*/

#include "LabeledSlider.h"

LabeledSlider::LabeledSlider()
{
    titleLabel.setFont(juce::Font(juce::FontOptions(14.0f)));
    titleLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(titleLabel);

    slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    slider.setSliderStyle(juce::Slider::LinearHorizontal);
    slider.setPaintingIsUnclipped(true);
    addAndMakeVisible(slider);

    leftLabel .setJustificationType(juce::Justification::centredLeft);
    leftLabel .setName("value_num_min");  addAndMakeVisible(leftLabel);

    rightLabel.setJustificationType(juce::Justification::centredRight);
    rightLabel.setName("value_num_max");  addAndMakeVisible(rightLabel);

    centerLabel.setJustificationType(juce::Justification::centred);
    centerLabel.setName("value_ratio");   addAndMakeVisible(centerLabel);

    applyPaletteFromLayer(); // palette par défaut (contrast)
}

void LabeledSlider::setTitle(const juce::String& text)           { titleLabel.setText(text, juce::dontSendNotification); }
void LabeledSlider::setSliderStyle(juce::Slider::SliderStyle s)   { slider.setSliderStyle(s); }
void LabeledSlider::setRange(double min, double max, double step) { slider.setRange(min, max, step); }

void LabeledSlider::setLayerIndex (int idx)
{
    if (layerIndex != idx)
    {
        layerIndex = idx;
        refreshPalette();
    }
}

void LabeledSlider::refreshPalette()
{
    applyPaletteFromLayer();
    repaint();
}

void LabeledSlider::applyPaletteFromLayer()
{
    if (layerIndex < 0)
    {
        // Palette "contrast"
        setTrackColour      (PluginColours::contrast);
        setBackgroundColour (PluginColours::background);
        setThumbColour      (PluginColours::onContrast);
        setTextColour       (PluginColours::onSurface);
        return;
    }

    // Palette layer:
    // background = couleur de piste, text = contraste associe.
    const auto lc = PluginColours::getLayerColours(layerIndex);
    setTrackColour      (lc.background);
    setBackgroundColour (PluginColours::background);
    setThumbColour      (lc.text);
    setTextColour       (PluginColours::onSurface);
}

void LabeledSlider::setDisplayMode(DisplayMode m)
{
    // Bascule la visibilite des labels selon le mode.
    mode = m;
    leftLabel  .setVisible(mode == DisplayMode::Note || mode == DisplayMode::Range);
    rightLabel .setVisible(mode == DisplayMode::Note || mode == DisplayMode::Range);
    centerLabel.setVisible(mode == DisplayMode::Ratio || mode == DisplayMode::Simple);
    updateDisplay();
}

void LabeledSlider::updateDisplay()
{
    // Important:
    // Cette methode transforme la valeur numerique en texte contextuel.
    // Le slider reste la source de verite; les labels ne sont qu une projection.
    if (mode == DisplayMode::Note)
    {
        leftLabel .setText(midiNoteName((int)slider.getMinValue()), juce::dontSendNotification);
        rightLabel.setText(midiNoteName((int)slider.getMaxValue()), juce::dontSendNotification);
    }
    else if (mode == DisplayMode::Range)
    {
        leftLabel .setText("min: " + juce::String((int)slider.getMinValue()), juce::dontSendNotification);
        rightLabel.setText("max: " + juce::String((int)slider.getMaxValue()), juce::dontSendNotification);
    }
    else if (mode == DisplayMode::Ratio)
    {
        centerLabel.setText(juce::String((int)slider.getMinValue()) + "/" + juce::String((int)slider.getMaxValue()), juce::dontSendNotification);
    }
    else if (mode == DisplayMode::Simple)
    {
        centerLabel.setText(juce::String((int)slider.getValue()), juce::dontSendNotification);
    }
}

void LabeledSlider::setTrackColour(juce::Colour c)      { slider.setColour(juce::Slider::trackColourId, c); }
void LabeledSlider::setBackgroundColour(juce::Colour c) { slider.setColour(juce::Slider::backgroundColourId, c); }
void LabeledSlider::setThumbColour(juce::Colour c)      { slider.setColour(juce::Slider::thumbColourId, c); }
void LabeledSlider::setTextColour(juce::Colour c)
{
    titleLabel .setColour(juce::Label::textColourId, c);
    leftLabel  .setColour(juce::Label::textColourId, c);
    rightLabel .setColour(juce::Label::textColourId, c);
    centerLabel.setColour(juce::Label::textColourId, c);
}

void LabeledSlider::resized()
{
    auto area = getLocalBounds();

    const int titleHeight = 12;
    const int labelHeight = 12;
    const int spacing     = 0;

    titleLabel.setBounds(area.removeFromTop(titleHeight));

    const int sliderHeight = 14;
    slider.setBounds(area.removeFromTop(sliderHeight));

    if (mode == DisplayMode::Note || mode == DisplayMode::Range)
    {
        // Labels gauche/droite alignes sur la meme baseline.
        const int labelY = slider.getBottom() + spacing;
        const auto sb = slider.getBounds();

        leftLabel .setBounds(sb.getX(),            labelY, 60, labelHeight);
        rightLabel.setBounds(sb.getRight() - 60,   labelY, 60, labelHeight);
    }
    else if (mode == DisplayMode::Ratio || mode == DisplayMode::Simple)
    {
        centerLabel.setBounds(slider.getX(), slider.getBottom() + spacing, slider.getWidth(), labelHeight);
    }
}

int LabeledSlider::getTrackCenterY() const
{
    return slider.getY() + slider.getHeight() / 2;
}

void LabeledSlider::paint(juce::Graphics& g)
{
    juce::ignoreUnused(g);
    // Rafraichit le texte au paint pour reflater les changements externes.
    updateDisplay();
}

juce::String LabeledSlider::midiNoteName(int value) const
{
    static const juce::StringArray names = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
    const int octave = (value / 12) - 1;
    const int index  = value % 12;
    return names[index] + juce::String(octave);
}

void LabeledSlider::updateEnabledState()
{
    const auto textColour = isEnabled() ? PluginColours::onSurface
                                        : PluginColours::onDisabled;

    setTextColour(textColour); // applique à tous les labels
}

void LabeledSlider::setEnabled(bool shouldBeEnabled)
{
    juce::Component::setEnabled(shouldBeEnabled);
    updateEnabledState();
}
