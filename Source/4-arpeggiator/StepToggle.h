/**
 * @file StepToggle.h
 * @brief Cellule d edition d un pas de sequenceur.
 *
 * Vue d ensemble:
 * - Ce composant represente un seul "step" visible dans la grille.
 * - Il ne contient pas la verite globale du sequenceur: la source d etat
 *   reste StepSequencerGrid (modele canonique par layer).
 * - StepToggle expose une API minimale pour afficher/editer une valeur
 *   d indice (choice index) et delegue les effets metier via callbacks.
 *
 * Place dans l architecture:
 * - UI locale: StepToggle (interaction souris + rendu d une case).
 * - UI de niveau superieur: StepSequencerGrid (projection d un layer actif).
 * - Etat persistant/runtime: APVTS + processors (hors de ce composant).
 *
 * Flux de donnees:
 * 1) StepSequencerGrid pousse layer + labels + valeur.
 * 2) Utilisateur edite (click, drag, wheel, menu contextuel).
 * 3) StepToggle emet onValueChanged / onLeftClick / onRightClick.
 * 4) StepSequencerGrid decide comment ecrire dans son modele canonique.
 *
 * Threading:
 * - UI thread uniquement.
 * - Non RT-safe.
 */

#pragma once

#include <JuceHeader.h>

// Important:
// L ordre des choices doit rester strictement coherent avec:
// - PluginParameters (AudioParameterChoice)
// - Arpeggiator / ArpeggiatorProcessor (interpretation des indices)
// Toute divergence ici casse la signification musicale des pas.
//
// Mapping contractuel des layers:
// 0=Rate, 1=Strum, 2=Jump, 3=Octave, 4=Velocity, 5=Groove, 6=Gate, 7=Retrig.
extern const juce::StringArray arpStepLayerChoices[8];

/**
 * @brief Cellule interactive avec edition click/drag/wheel/menu.
 *
 * Invariants:
 * - value est un indice valide pour le layer courant.
 * - minChoiceIndex force un plancher (ex: step 1 ne peut pas etre Skip).
 * - StepToggle ne modifie jamais directement APVTS ou un processor.
 */
class StepToggle : public juce::Component
{
public:
    StepToggle();

    //==========================================================================
    // JUCE overrides
    //==========================================================================
    void paint(juce::Graphics& g) override;
    void resized() override {}

    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;

    //==========================================================================
    // State API
    //--------------------------------------------------------------------------
    // Ces setters sont appeles par la grille pour projeter le modele canonique
    // vers la vue. Le composant reste volontairement ignorant du contexte lane
    // ou des params host.
    //==========================================================================
    void setValue(int newValue, bool notify = false);
    void setLayer(int layerIndex);
    void setChoiceLabels(const juce::StringArray* choices);
    void setMinimumChoiceIndex(int minimumChoiceIndex);
    void setSelected(bool selected);
    void setPlayhead(bool playhead);

    int getValue() const noexcept { return value; }
    int getLayer() const noexcept { return currentLayer; }

    //==========================================================================
    // Callbacks
    //--------------------------------------------------------------------------
    // onLeftClick:
    // - Intention utilisateur "edition rapide" (logique decidee par la grille).
    //
    // onRightClick:
    // - Notifie une edition via menu contextuel terminee.
    //
    // onValueChanged:
    // - Emis des que la valeur de pas change effectivement.
    // - Point d entree principal pour mettre a jour le modele canonique.
    //==========================================================================
    std::function<void()> onLeftClick;
    std::function<void()> onRightClick;
    std::function<void(int newValue)> onValueChanged;

private:
    //==========================================================================
    // Internal helpers
    //--------------------------------------------------------------------------
    // showRateChoiceMenu / showGateChoiceMenu / showDirectionChoiceMenu:
    // - Menus specialises quand la semantique du layer demande une UX dediee
    //   (hierarchie de sous-menus, ordre contraint, etc).
    //
    // showCompactChoiceMenu:
    // - Fallback generique pour layers sans UX speciale.
    //==========================================================================
    const juce::StringArray& getChoices() const;
    juce::String getLabelForCurrentValue() const;
    void showCompactChoiceMenu();
    void showDirectionChoiceMenu();
    void showGateChoiceMenu();
    void showRateChoiceMenu();

    //==========================================================================
    // State
    //--------------------------------------------------------------------------
    // value:
    // - indice de choix courant pour le layer visible.
    //
    // dragStartValue / dragStartY:
    // - reference de geste pour convertir un drag vertical en variation d index.
    //
    // currentLayer:
    // - index logique du layer UI actif dans la cellule.
    //
    // choiceLabelsOverride:
    // - table optionnelle injectee par la grille pour decoupler ordre UI et
    //   ordre physique de arpStepLayerChoices.
    //
    // minChoiceIndex:
    // - contrainte de borne basse (souvent 1 pour le premier pas).
    //
    // pendingRightClick / wasDragged:
    // - garde-fous UX pour eviter les ambiguities click vs drag vs menu.
    //==========================================================================
    int value = 0;              // choice index pour le layer courant
    int dragStartValue = 0;
    int dragStartY = 0;

    int currentLayer = 0;
    const juce::StringArray* choiceLabelsOverride = nullptr;
    int minChoiceIndex = 0;
    bool isSelected = false;
    bool isPlayhead = false;

    bool pendingRightClick = false;
    bool wasDragged = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StepToggle)
};
