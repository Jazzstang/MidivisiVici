/**
 * @file StepSequencerGrid.h
 * @brief Grille de pas editable avec stockage canonique multi-layer.
 *
 * Vue d ensemble:
 * - Ce composant maintient 8 layers x 32 pas dans un modele interne stable.
 * - Une seule couche est affichee a la fois, mais toutes les couches gardent
 *   leurs valeurs meme hors ecran.
 *
 * Place dans l architecture:
 * - UI intermediaire entre StepToggle (cellule) et Arpeggiator (module).
 * - Arpeggiator decide quel layer est actif, puis pousse/recupere les valeurs.
 *
 * Flux:
 * 1) APVTS -> Arpeggiator -> setLayerStepValue (mise a jour modele).
 * 2) setActiveLayer projette ce modele vers les cellules visibles.
 * 3) Utilisateur edite une cellule -> onStepChanged remonte vers Arpeggiator.
 *
 * Threading:
 * - UI thread uniquement.
 * - Non RT-safe.
 */

#pragma once

#include <JuceHeader.h>
#include <array>
#include <vector>
#include <memory>

#include "StepToggle.h"

/**
 * @brief Gere projection view/model des pas de sequenceur.
 *
 * Design concept:
 * - Separation des responsabilites:
 *   - modele: layerValues (canonique)
 *   - vue: steps (StepToggle visibles)
 * - Cette separation evite de perdre des etats lors d un changement de page.
 *
 * Invariants:
 * - layerValues contient toujours 8x32 valeurs.
 * - Le pas 1 (index 0) d un layer ne peut pas etre Skip (minChoiceForStep).
 * - Une modification utilisateur ne doit notifier onStepChanged qu une fois.
 */
class StepSequencerGrid : public juce::Component
{
public:
    StepSequencerGrid();                 // defaults to 1 x 32
    StepSequencerGrid(int numRows, int numCols);

    //==========================================================================
    // JUCE
    //==========================================================================
    void resized() override;

    //==========================================================================
    // External control
    //==========================================================================
    // Change la couche visible (UI thread). Ne modifie pas les autres couches.
    void setActiveLayer(int layer);
    // Fournit une table de labels custom pour une couche (peut etre null -> labels par defaut).
    void setLayerChoiceLabels(int layer, const juce::StringArray* labels);

    // Mise a jour modele d un pas dans un layer cible.
    // Utilise dans le flux "APVTS -> UI" (pas de notification user ici).
    void setLayerStepValue(int layer, int stepIndex0Based, int choiceIndex);

    // Mise a jour d un pas du layer actif.
    // Utilise surtout dans le flux interaction utilisateur (peut notifier).
    void setStepValue(int stepIndex0Based, int choiceIndex, bool notify);

    // Visual playback cursor on currently visible row.
    // Valeur -1 efface le playhead.
    void setPlaybackStep(int stepIndex0Based);

    int getActiveLayer() const noexcept { return activeLayer; }
    int getStepCount() const noexcept { return rows * cols; }

    // Emis quand un pas est edite depuis la vue.
    // stepIndex0Based est toujours dans [0..31] pour le modele canonique.
    std::function<void(int stepIndex0Based, int newChoiceIndex)> onStepChanged;

private:
    //==========================================================================
    // Internal
    //==========================================================================
    void buildSteps();
    void applyActiveLayerToVisibleSteps();

    bool isStepIndexValid(int stepIndex0Based) const noexcept;
    int  clampLayerIndex(int layer) const noexcept;

    //==========================================================================
    // Layout config
    //==========================================================================
    int rows = 1;
    int cols = 32;

    //==========================================================================
    // State
    //==========================================================================
    // -1 au demarrage pour forcer la premiere projection explicite via setActiveLayer().
    int activeLayer = -1;
    int selectedStep = -1;
    int playbackStep = -1;

    // Store canonique independant de la geometrie visible.
    // Meme si la grille visible est 4x8, le modele reste 32 pas par layer.
    std::array<std::array<int, 32>, 8> layerValues {{ }};
    std::array<const juce::StringArray*, 8> layerChoiceLabels {{ }};

    // Visible cells: size == rows * cols.
    std::vector<std::unique_ptr<StepToggle>> steps;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StepSequencerGrid)
};
