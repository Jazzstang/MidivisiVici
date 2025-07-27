// =============================
// InputFilter.cpp
// =============================
#include "InputFilter.h"
#include "TwoLineToggleButton.h"
#include "PluginParameters.h"

InputFilter::InputFilter(juce::AudioProcessorValueTreeState& state)
    : parameters(state),
      noteFilterToggle("Note", "Filter"),
      velocityFilterToggle("Velocity", "Filter"),
      voiceLimitToggle("Voice", "Limit")
{
    // === Ligne 1 : MIDI Channel + Mode Selector + Mute + Learn CC + Status ===
    midiChannelLabel.setText("MIDI Channel", juce::dontSendNotification);
    midiChannelLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(midiChannelLabel);

    channelModeSelector.addItem("1", 1);
    channelModeSelector.addItem("2", 2);
    channelModeSelector.addItem("3", 3);
    channelModeSelector.addItem("4", 4);
    channelModeSelector.addItem("5", 5);
    channelModeSelector.addItem("6", 6);
    channelModeSelector.addItem("7", 7);
    channelModeSelector.addItem("8", 8);
    channelModeSelector.addItem("9", 9);
    channelModeSelector.addItem("10", 10);
    channelModeSelector.addItem("11", 11);
    channelModeSelector.addItem("12", 12);
    channelModeSelector.addItem("13", 13);
    channelModeSelector.addItem("14", 14);
    channelModeSelector.addItem("15", 15);
    channelModeSelector.addItem("16", 16);
    channelModeSelector.setSelectedId(1);
    channelModeSelector.onChange = [this]{
        // callback
    };
    addAndMakeVisible(channelModeSelector);
    channelModeSelector.setSelectedId(1);

    muteButton.setButtonText("Mute");
    muteButton.setName("muteButton");
    muteButton.setClickingTogglesState(true);
    muteButton.setToggleState(false, juce::dontSendNotification);
    muteButton.onClick = [this] {
        bool newState = muteButton.getToggleState();
        parameters.getParameter(ParamIDs::inputMute)->setValueNotifyingHost(newState ? 1.0f : 0.0f);
    };
    addAndMakeVisible(muteButton);

    learnCCButton.setButtonText("Learn CC");
    learnCCButton.setName("learnCCButton");
    learnCCButton.setClickingTogglesState(true);
    learnCCButton.onClick = [this] {
        auto* learnParam = parameters.getParameter(ParamIDs::learnModeActive);
        learnParam->setValueNotifyingHost(learnCCButton.getToggleState() ? 1.0f : 0.0f);
    };
    addAndMakeVisible(learnCCButton);

    ccLearnStatusLabel.setText("none", juce::dontSendNotification);
    ccLearnStatusLabel.setJustificationType(juce::Justification::centredLeft);
    ccLearnStatusLabel.setName("ccStatus");
    addAndMakeVisible(ccLearnStatusLabel);

    inputFilterTitleButton.setButtonText("Input Filter");
    inputFilterTitleButton.setClickingTogglesState(true);
    inputFilterTitleButton.setToggleState(true, juce::dontSendNotification);
    addAndMakeVisible(inputFilterTitleButton);
    inputFilterTitleButton.onClick = [this]{
        const bool enabled = inputFilterTitleButton.getToggleState();
        noteSlider.setEnabled(enabled && noteFilterToggle.getToggleState());
        velocitySlider.setEnabled(enabled && velocityFilterToggle.getToggleState());
        stepSlider.setEnabled(enabled && stepFilterToggle.getToggleState());
        voiceSlider.setEnabled(enabled && voiceLimitToggle.getToggleState());
    };

    // === Sliders ===
    addAndMakeVisible(noteSlider);
    noteSlider.setTitle("Note Range");
    noteSlider.setSliderStyle(juce::Slider::TwoValueHorizontal);
    noteSlider.setRange(0, 127, 1);
    noteSlider.setDisplayMode(LabeledSlider::DisplayMode::Note);
    noteSlider.getSlider().onValueChange = [this] {
        parameters.getParameter(ParamIDs::noteMin)->setValueNotifyingHost(noteSlider.getSlider().getMinValue() / 127.0f);
        parameters.getParameter(ParamIDs::noteMax)->setValueNotifyingHost(noteSlider.getSlider().getMaxValue() / 127.0f);
    };

    addAndMakeVisible(velocitySlider);
    velocitySlider.setTitle("Velocity Range");
    velocitySlider.setSliderStyle(juce::Slider::TwoValueHorizontal);
    velocitySlider.setRange(0, 127, 1);
    velocitySlider.setDisplayMode(LabeledSlider::DisplayMode::Range);
    velocitySlider.getSlider().onValueChange = [this] {
        parameters.getParameter(ParamIDs::velocityMin)->setValueNotifyingHost(velocitySlider.getSlider().getMinValue() / 127.0f);
        parameters.getParameter(ParamIDs::velocityMax)->setValueNotifyingHost(velocitySlider.getSlider().getMaxValue() / 127.0f);
    };

    addAndMakeVisible(stepFilterToggle);
    stepFilterToggle.setToggleState(true, juce::dontSendNotification);
    stepFilterToggle.onClick = [this]
    {
        bool active = stepFilterToggle.getToggleState();
        stepSlider.setEnabled(active);
    };
    addAndMakeVisible(stepSlider);
    stepSlider.setTitle("Step Filter");
    stepSlider.setSliderStyle(juce::Slider::TwoValueHorizontal);
    stepSlider.setRange(1, 16, 1);
    stepSlider.setDisplayMode(LabeledSlider::DisplayMode::Ratio);
    stepSlider.getSlider().onValueChange = [this] {
        parameters.getParameter(ParamIDs::stepFilterNumerator)->setValueNotifyingHost((stepSlider.getSlider().getMinValue() - 1) / 15.0f);
        parameters.getParameter(ParamIDs::stepFilterDenominator)->setValueNotifyingHost((stepSlider.getSlider().getMaxValue() - 1) / 15.0f);
    };

    addAndMakeVisible(voiceSlider);
    voiceSlider.setTitle("Voice Limit");
    voiceSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    voiceSlider.setRange(1, 16, 1);
    voiceSlider.setDisplayMode(LabeledSlider::DisplayMode::Simple);
    voiceSlider.getSlider().onValueChange = [this] {
        parameters.getParameter(ParamIDs::voiceLimit)->setValueNotifyingHost((voiceSlider.getSlider().getValue() - 1) / 15.0f);
    };

    noteFilterToggle.setToggleState(true, juce::dontSendNotification);
    noteFilterToggle.onClick = [this]{ noteSlider.setEnabled(noteFilterToggle.getToggleState()); };
    addAndMakeVisible(noteFilterToggle);
  
    velocityFilterToggle.setToggleState(true, juce::dontSendNotification);
    velocityFilterToggle.onClick = [this]{ velocitySlider.setEnabled(velocityFilterToggle.getToggleState()); };
    addAndMakeVisible(velocityFilterToggle);
  
    voiceLimitToggle.setToggleState(true, juce::dontSendNotification);
    voiceLimitToggle.onClick = [this]{ voiceSlider.setEnabled(voiceLimitToggle.getToggleState()); };
    addAndMakeVisible(voiceLimitToggle);

    // Priority
    addAndMakeVisible(priorityBox);
    priorityBox.addItem("Last", 1);
    priorityBox.addItem("Lowest", 2);
    priorityBox.addItem("Highest", 3);
    priorityBox.onChange = [this] {
        parameters.getParameter(ParamIDs::priority)->setValueNotifyingHost((priorityBox.getSelectedId() - 1) / 2.0f);
    };
    priorityLabel.setText("Priority", juce::dontSendNotification);
    addAndMakeVisible(priorityLabel);

    startTimerHz(15);
}
void InputFilter::setLine1Bounds(juce::Rectangle<int> area)
{
    midiChannelLabel.setBounds(area.removeFromLeft(68));
    channelModeSelector.setBounds(area.removeFromLeft(48).reduced(4, 4));
    muteButton.setBounds(area.removeFromLeft(48).reduced(4, 4));
    learnCCButton.setBounds(area.removeFromLeft(48).reduced(4, 4));
    ccLearnStatusLabel.setBounds(area.removeFromLeft(68));
}
void InputFilter::resized()
{
    auto area = getLocalBounds();
    area = area.withTrimmedRight(10)
               .withTrimmedLeft(10);

    inputFilterTitleButton.setBounds(area.removeFromTop(24));
    area.removeFromTop(4); // espace comme Transform/Divisi
    
    // === Header ===
    auto line1 = area.removeFromTop(24);
    setLine1Bounds(line1);

    area.removeFromTop(14);  // Espace entre la ligne 1 et le premier slider
    
    // === Sliders ===
    auto rowH = 40;
    int sliderSpacing = 8;

    // === Note Range Filter ===
    auto noteArea = area.removeFromTop(rowH);
    noteFilterToggle.setBounds(noteArea.removeFromLeft(48).reduced(0, 2));
    noteArea.removeFromLeft(8);
    noteSlider.setBounds(noteArea);
    area.removeFromTop(sliderSpacing);

    // === Velocity Range Filter ===
    auto velocityArea = area.removeFromTop(rowH);
    velocityFilterToggle.setBounds(velocityArea.removeFromLeft(48).reduced(0, 2));
    velocityArea.removeFromLeft(8);
    velocitySlider.setBounds(velocityArea);
    area.removeFromTop(sliderSpacing);

    // === Step Filter Toggle + Slider ===
    auto stepArea = area.removeFromTop(rowH);
    stepFilterToggle.setBounds(stepArea.removeFromLeft(48).reduced(0, 2)); // largeur fixe bouton
    stepArea.removeFromLeft(8);
    stepSlider.setBounds(stepArea);
    area.removeFromTop(sliderSpacing);
    
    // === Voice Limit Filter ==
    auto voiceArea = area.removeFromTop(rowH);
    voiceLimitToggle.setBounds(voiceArea.removeFromLeft(48).reduced(0, 2));
    voiceArea.removeFromLeft(8);
    voiceSlider.setBounds(voiceArea);
    area.removeFromTop(sliderSpacing);

    // === Priority ==
    auto priorityArea = area.removeFromTop(rowH);  // hauteur de la ligne

    const int labelWidth = 48;  // largeur fixe du label
    priorityLabel.setBounds(priorityArea.removeFromLeft(labelWidth).reduced(4, 4));
    priorityBox.setBounds(priorityArea.reduced(4, 4));  // le reste pour le ComboBox
}

void InputFilter::timerCallback()
{
    const bool globalEnabled = inputFilterTitleButton.getToggleState();

    learnCCButton.setClickingTogglesState(true);
    
    noteSlider.getSlider().setMinValue(parameters.getParameter(ParamIDs::noteMin)->getValue() * 127.0f, juce::dontSendNotification);
    noteSlider.getSlider().setMaxValue(parameters.getParameter(ParamIDs::noteMax)->getValue() * 127.0f, juce::dontSendNotification);

    velocitySlider.getSlider().setMinValue(parameters.getParameter(ParamIDs::velocityMin)->getValue() * 127.0f, juce::dontSendNotification);
    velocitySlider.getSlider().setMaxValue(parameters.getParameter(ParamIDs::velocityMax)->getValue() * 127.0f, juce::dontSendNotification);

    stepSlider.getSlider().setMinValue(parameters.getParameter(ParamIDs::stepFilterNumerator)->getValue() * 15.0f + 1, juce::dontSendNotification);
    stepSlider.getSlider().setMaxValue(parameters.getParameter(ParamIDs::stepFilterDenominator)->getValue() * 15.0f + 1, juce::dontSendNotification);

    voiceSlider.getSlider().setValue(parameters.getParameter(ParamIDs::voiceLimit)->getValue() * 15.0f + 1, juce::dontSendNotification);

    priorityBox.setSelectedId(static_cast<int>(parameters.getParameter(ParamIDs::priority)->getValue() * 2.0f + 1), juce::dontSendNotification);
    muteButton.setToggleState(parameters.getParameter(ParamIDs::inputMute)->getValue() > 0.5f, juce::dontSendNotification);

    noteSlider.setEnabled(globalEnabled && noteFilterToggle.getToggleState());
    velocitySlider.setEnabled(globalEnabled && velocityFilterToggle.getToggleState());
    stepSlider.setEnabled(globalEnabled && stepFilterToggle.getToggleState());
    voiceSlider.setEnabled(globalEnabled && voiceLimitToggle.getToggleState());
}

InputFilter::~InputFilter() = default;
