#include "DivisiRoundRobin.h"

DivisiRoundRobin::DivisiRoundRobin(juce::AudioProcessorValueTreeState& state)
    : parameters(state)
{
    // Initialisation des composants spécifiques
}

DivisiRoundRobin::~DivisiRoundRobin() = default;

void DivisiRoundRobin::resized()
{
    // Gestion des positions de composants si nécessaire
}
