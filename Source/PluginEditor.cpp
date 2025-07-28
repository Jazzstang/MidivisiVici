#include "PluginEditor.h"

//==============================================================================
MidivisiViciAudioProcessorEditor::MidivisiViciAudioProcessorEditor(MidivisiViciAudioProcessor& p)
    : AudioProcessorEditor(p),
      processor(p),
      inputMonitor(p.parameters),
      inputFilterContent(p.parameters),
      transformContent(p.parameters),
      divisiContent(p.parameters),
      outputMonitor(p.parameters)
{
    setLookAndFeel(&lookAndFeel);
    startTimerHz(30);

    // === Colonne 1 : Input Monitor ===
    addAndMakeVisible(inputMonitor);

    // === Colonne 2 : Input Filter ===
    addAndMakeVisible(inputFilterContent);

    // === Colonne 3 : Transform ===
    addAndMakeVisible(transformContent);
    
    // === Colonne 4 : Divisi ===
    addAndMakeVisible(divisiContent);

    // === Colonne 5 : Output Monitor ===
    addAndMakeVisible(outputMonitor);

    setSize(5 * 300, 300);
}

MidivisiViciAudioProcessorEditor::~MidivisiViciAudioProcessorEditor()
{
    setLookAndFeel(nullptr);
}

void MidivisiViciAudioProcessorEditor::paint(juce::Graphics& g)
{
    // --- Dégradé radial ---
    juce::ColourGradient radial(juce::Colours::transparentBlack,
                                getWidth() / 2.0f, getHeight() / 2.0f,
                                juce::Colours::black.withAlpha(0.4f),
                                0, 0, true);
    g.setGradientFill(radial);
    g.fillAll();

    // --- Bruit ---
    if (noiseImage.isValid())
        g.drawImageAt(noiseImage, 0, 0);

    g.fillAll(PluginColours::background);
}

void MidivisiViciAudioProcessorEditor::resized()
{
    auto area = getLocalBounds();
    int columnWidth = 280;

    auto col1 = area.removeFromLeft(columnWidth).reduced(10);
    auto col2 = area.removeFromLeft(columnWidth).reduced(10);
    auto col3 = area.removeFromLeft(columnWidth).reduced(10);
    auto col4 = area.removeFromLeft(columnWidth).reduced(10);
    auto col5 = area.removeFromLeft(columnWidth).reduced(10);

    inputMonitor.setBounds(col1);
    inputFilterContent.setBounds(col2);
    transformContent.setBounds(col3);
    divisiContent.setBounds(col4);
    outputMonitor.setBounds(col5);

    generateNoiseImage(getWidth(), getHeight());
}

void MidivisiViciAudioProcessorEditor::timerCallback()
{
    // --- Mise à jour via les méthodes internes des moniteurs ---
    inputMonitor.updateFromFifo(processor.inputFifo, processor.inputFifoMessages);
    outputMonitor.updateFromFifo(processor.outputFifo, processor.outputFifoMessages);

    notesState = processor.parameters.getRawParameterValue(ParamIDs::inputMonitorFilterNote)->load() > 0.5f;
    controlsState = processor.parameters.getRawParameterValue(ParamIDs::inputMonitorFilterControl)->load() > 0.5f;
    clockState = processor.parameters.getRawParameterValue(ParamIDs::inputMonitorFilterClock)->load() > 0.5f;
    eventsState = processor.parameters.getRawParameterValue(ParamIDs::inputMonitorFilterEvent)->load() > 0.5f;

    lookAndFeel.setMonitorStates(notesState, controlsState, clockState, eventsState);

    repaint();
}

void MidivisiViciAudioProcessorEditor::generateNoiseImage(int width, int height)
{
    if (width == noiseWidth && height == noiseHeight && noiseImage.isValid())
        return; // rien à faire si la taille n’a pas changé

    noiseWidth = width;
    noiseHeight = height;

    noiseImage = juce::Image(juce::Image::ARGB, width, height, true);
    juce::Graphics g(noiseImage);

    juce::Random rand;
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            if (rand.nextFloat() < 0.02f)
            {
                noiseImage.setPixelAt(x, y,
                                      juce::Colours::white.withAlpha(0.03f)); // alpha en [0,1]
            }
        }
    }
}
