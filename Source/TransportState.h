/**
 * @file TransportState.h
 * @brief RT-safe host transport state/edge tracker.
 */
//==============================================================================
// TransportState.h
//------------------------------------------------------------------------------
// Lightweight RT transport edge detector.
//
// Responsibilities:
// - Track current host playing state.
// - Expose one-tick edges (justStarted / justStopped).
// - Stay lock-free and allocation-free for audio thread usage.
//==============================================================================

#pragma once
#include <JuceHeader.h>

/*
==============================================================================
TransportState
------------------------------------------------------------------------------
Role:
- Petit adaptateur RT entre AudioPlayHead et le moteur MIDI.
- Convertit un etat "isPlaying" potentiellement instable selon l hote
  en edges one-shot predicibles pour le reste du pipeline.

Sorties exposees:
- justStarted(): edge STOP -> PLAY (vrai un seul tick)
- justStopped(): edge PLAY -> STOP (vrai un seul tick)
- isPlaying(): dernier etat connu

Regles:
- RT-safe strict: pas d allocation, pas de lock, pas de ValueTree, pas d I/O.
- JUCE 7+: source principale = getPosition().
- JUCE <7: chemin legacy = getCurrentPosition().
- Si l hote ne donne aucune info sur un tick, on conserve l etat precedent
  (pas de reset implicite vers STOP).

Usage:
- prime(getPlayHead()) dans prepareToPlay.
- tick(getPlayHead()) en debut de processBlock, avant la logique dependante
  de start/stop.
==============================================================================
*/

/**
 * @brief Minimal transport state helper for processBlock edge logic.
 *
 * Pattern:
 * - Pattern: State + Edge Detector
 * - Problem solved: convert raw host play-state into stable one-shot edges.
 * - Participants:
 *   - prime(): initializes baseline.
 *   - tick(): updates current state and computes edges.
 * - Pitfalls:
 *   - If host data is missing on one tick, we keep previous state to avoid
 *     false edges due to transient host gaps.
 */
class TransportState
{
public:
    /**
     * @brief Capture initial transport state.
     *
     * Thread: audio thread (prepareToPlay path).
     * RT-safe: yes.
     */
    void prime(juce::AudioPlayHead* ph) noexcept;

    /**
     * @brief Update state and compute edge flags for this block.
     *
     * Thread: audio thread.
     * RT-safe: yes.
     */
    void tick(juce::AudioPlayHead* ph) noexcept;

    bool isPlaying()   const noexcept { return wasPlaying; }
    bool justStarted() const noexcept { return startedThisTick; } // STOP -> PLAY
    bool justStopped() const noexcept { return stoppedThisTick; } // PLAY -> STOP

private:
    /**
     * @brief Query helper with explicit fallback contract.
     *
     * fallbackState est l etat precedent du detecteur. Il est renvoye si
     * l hote ne peut pas fournir de position pour ce tick.
     */
    static bool queryIsPlaying(juce::AudioPlayHead* ph, bool fallbackState) noexcept;

    // Last known state.
    bool wasPlaying      = false;

    // One-shot edges for current tick.
    bool startedThisTick = false;
    bool stoppedThisTick = false;
};
