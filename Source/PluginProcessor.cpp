#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
MidivisiViciAudioProcessor::MidivisiViciAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
     : AudioProcessor (BusesProperties()
                      #if ! JucePlugin_IsMidiEffect
                       #if ! JucePlugin_IsSynth
                        .withInput  ("Input", juce::AudioChannelSet::stereo(), true)
                       #endif
                        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                      #endif
                       ),
       parameters (*this, nullptr, juce::Identifier ("MidivisiViciParams"), createParameterLayout())
#endif
{
}

MidivisiViciAudioProcessor::~MidivisiViciAudioProcessor() {}

const juce::String MidivisiViciAudioProcessor::getName() const { return JucePlugin_Name; }

bool MidivisiViciAudioProcessor::acceptsMidi() const { return true; }
bool MidivisiViciAudioProcessor::producesMidi() const { return true; }
bool MidivisiViciAudioProcessor::isMidiEffect() const { return true; }
double MidivisiViciAudioProcessor::getTailLengthSeconds() const { return 0.0; }

int MidivisiViciAudioProcessor::getNumPrograms() { return 1; }
int MidivisiViciAudioProcessor::getCurrentProgram() { return 0; }
void MidivisiViciAudioProcessor::setCurrentProgram (int index) {}
const juce::String MidivisiViciAudioProcessor::getProgramName (int index) { return {}; }
void MidivisiViciAudioProcessor::changeProgramName (int index, const juce::String& newName) {}

void MidivisiViciAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock) {}
void MidivisiViciAudioProcessor::releaseResources() {}

#ifndef JucePlugin_PreferredChannelConfigurations
bool MidivisiViciAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return true;
}
#endif

void MidivisiViciAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    buffer.clear();
    auto inputEnable = parameters.getRawParameterValue("inputEnable")->load() > 0.5f;

    if (!inputEnable)
    {
        midiMessages.clear();
        return;
    }

    // Ici viendra la logique des filtres et du traitement MIDI
}

void MidivisiViciAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = parameters.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void MidivisiViciAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));
    if (xml.get() != nullptr)
        if (xml->hasTagName (parameters.state.getType()))
            parameters.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MidivisiViciAudioProcessor();
}

//==============================================================================

juce::AudioProcessorValueTreeState::ParameterLayout MidivisiViciAudioProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    // Input Enable
    params.push_back (std::make_unique<juce::AudioParameterBool>("inputEnable", "Input Enable", true));
    
    params.push_back (std::make_unique<juce::AudioParameterBool>("showMonitor", "Show Monitor", true));

    // Input Monitor Filters (Note, Control, Clock, Event)
    params.push_back (std::make_unique<juce::AudioParameterBool>("filterNote", "Filter Note", true));
    params.push_back (std::make_unique<juce::AudioParameterBool>("filterControl", "Filter Control", true));
    params.push_back (std::make_unique<juce::AudioParameterBool>("filterClock", "Filter Clock", true));
    params.push_back (std::make_unique<juce::AudioParameterBool>("filterEvent", "Filter Event", true));

    return { params.begin(), params.end() };
}

bool MidivisiViciAudioProcessor::hasEditor() const
{
    return true;
}

juce::AudioProcessorEditor* MidivisiViciAudioProcessor::createEditor()
{
    return new MidivisiViciAudioProcessorEditor(*this);
}
