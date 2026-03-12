/*
==============================================================================
TransportState.cpp
------------------------------------------------------------------------------
Role du fichier:
- Implementation d un detecteur d etat transport (playing) avec edges
  one-shot START/STOP.

Usage typique:
- prime() en prepareToPlay.
- tick() en debut de processBlock.

Objectif:
- Offrir un comportement stable entre hotes, y compris quand certaines infos
  transport sont temporairement indisponibles.

Contraintes:
- RT-safe: pas de lock, pas d allocation, pas d I/O.
- Le fallback "etat precedent" est volontaire pour eviter des edges fantomes
  quand un hote fournit des infos transport intermittentes.
==============================================================================
*/

#include "TransportState.h"

//==============================================================================
// 1) Unified query helper
//------------------------------------------------------------------------------
bool TransportState::queryIsPlaying(juce::AudioPlayHead* ph, bool fallbackState) noexcept
{
    if (ph == nullptr)
        return fallbackState;

#if JUCE_MAJOR_VERSION >= 7
    // JUCE moderne: getPosition() expose des champs optionnels explicites.
    // Si aucune position n est disponible ce tick, on conserve fallbackState.
    if (auto posOpt = ph->getPosition())
        return posOpt->getIsPlaying();

    return fallbackState;
#else
    // Compat JUCE legacy.
    juce::AudioPlayHead::CurrentPositionInfo pos {};
    if (ph->getCurrentPosition(pos))
        return pos.isPlaying;

    return fallbackState;
#endif
}

//==============================================================================
// 2) Public API
//------------------------------------------------------------------------------
void TransportState::prime(juce::AudioPlayHead* ph) noexcept
{
    wasPlaying      = queryIsPlaying(ph, false);
    startedThisTick = false;
    stoppedThisTick = false;
}

void TransportState::tick(juce::AudioPlayHead* ph) noexcept
{
    startedThisTick = false;
    stoppedThisTick = false;

    const bool nowPlaying = queryIsPlaying(ph, wasPlaying);

    // Detecte les transitions STOP->PLAY et PLAY->STOP.
    // Invariant: startedThisTick et stoppedThisTick ne peuvent pas etre vrais
    // en meme temps sur un meme tick.
    if (wasPlaying && !nowPlaying)
        stoppedThisTick = true;   // PLAY -> STOP
    else if (!wasPlaying && nowPlaying)
        startedThisTick = true;   // STOP -> PLAY

    wasPlaying = nowPlaying;
}
