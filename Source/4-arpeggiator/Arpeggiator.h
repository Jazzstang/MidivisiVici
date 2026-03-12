/**
 * @file Arpeggiator.h
 * @brief UI du module Arpeggiator/Drum (edition des pas + visualisation de lecture).
 *
 * Role du fichier:
 * - Definir la facade UI d une lane arpeggiator.
 * - Projeter deux vues metier (Arp et Drum) sur une base de parametres partagee.
 * - Synchroniser l affichage avec APVTS (etat persistant) sans toucher
 *   directement au flux audio RT.
 *
 * Place dans l architecture:
 * - UI: ce composant.
 * - Etat: APVTS (PluginParameters).
 * - Moteur temps reel: ArpeggiatorProcessor (autre classe, autre thread).
 *
 * Threading:
 * - UI thread pour rendu et interactions.
 * - parameterChanged peut venir hors UI thread via APVTS, donc ce composant
 *   rebasculle via AsyncUpdater avant de toucher aux widgets JUCE.
 * - Non RT-safe.
 */

#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <memory>

#include "../0-component/CustomRotaryNoCombo.h"
#include "../0-component/LeftClickToggleButton.h"
#include "../0-component/ShadowComponent.h"
#include "../PluginParameters.h"

#include "LinkToggle.h"
#include "StepSequencerGrid.h"
#include "../5-splitter/SplitterModeToggle.h"

class MidivisiViciAudioProcessor;

/**
 * @brief Panneau complet d une lane arpeggiator.
 *
 * Pattern utilise:
 * - Pattern: Observer + projection model/view.
 * - Probleme resolu: garder une UI editable et reactive pendant que l etat
 *   reel vit dans APVTS et que le moteur audio tourne ailleurs.
 * - Participants:
 *   - APVTS listener: detecte les changements de parametres.
 *   - StepSequencerGrid: projection des pas visibles.
 *   - Timer UI: animation du playhead local synchronise au transport.
 * - Piege classique:
 *   - Le playhead UI est une visualisation, pas la source autoritaire du timing
 *     audio (source autoritaire = ArpeggiatorProcessor).
 */
class Arpeggiator : public juce::Component,
                    private juce::AudioProcessorValueTreeState::Listener,
                    private juce::AsyncUpdater,
                    private juce::Timer
{
public:
    using Lane = Lanes::Lane;

    Arpeggiator(juce::AudioProcessorValueTreeState& vts,
                MidivisiViciAudioProcessor& processorIn,
                Lane lane);
    ~Arpeggiator() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void uiTimerTick();

    void setActiveLayer(int newLayer);
    void forceRefreshFromParameters();

private:
    /**
     * @brief Bouton rond de selection de page (mode dependant).
     *
     * Ce bouton est volontairement "dumb UI":
     * - affiche un etat selectionne/non-selectionne,
     * - remonte un onClick,
     * - ne connait pas APVTS directement.
     */
    class PageDialButton : public juce::Component
    {
    public:
        PageDialButton();

        void setText(const juce::String& text);
        void setLayerIndex(int index);
        void setSelected(bool shouldBeSelected);

        std::function<void()> onClick;

        void resized() override;
        void paint(juce::Graphics& g) override;
        void mouseDown(const juce::MouseEvent& e) override;
        void mouseEnter(const juce::MouseEvent& e) override;
        void mouseExit(const juce::MouseEvent& e) override;

    private:
        juce::Label titleLabel;
        juce::Rectangle<int> dialBounds;
        int layerIndex = 0;
        bool selected = false;
        bool hovered = false;
    };

    // APVTS listener: entree asynchrone vers la synchro UI.
    void parameterChanged(const juce::String& parameterID, float newValue) override;
    void handleAsyncUpdate() override;

    // Timer UI: visualisation transport/playhead.
    void timerCallback() override;

    void registerParameterListeners();
    void unregisterParameterListeners();
    void showModulePresetMenu();
    void showSaveModulePresetDialog();
    bool saveLoadedModulePreset();
    void showEditLoadedPresetDialog();
    [[nodiscard]] juce::ValueTree captureModulePresetState() const;
    void applyModulePresetState(const juce::ValueTree& state);

    void refreshUIFromParameters();
    void applyGlobalUiBypass(bool bypassed);

    juce::String idForLane(const juce::String& baseID) const;

    template <typename Comp>
    void addWithShadow(std::unique_ptr<ShadowComponent>& shadowPtr,
                       Comp& comp,
                       const juce::DropShadow& shadow)
    {
        shadowPtr = std::make_unique<ShadowComponent>(shadow, -1.0f, true, true, true, true, 10);
        shadowPtr->addAndMakeVisible(comp);
        addAndMakeVisible(*shadowPtr);
    }

    static juce::String makeSeqParamId(const juce::String& prefix, int step1Based);
    static const std::array<juce::String, 8>& arpSeqPrefixes();
    static const std::array<juce::String, 8>& drumSeqPrefixes();
    static const juce::String& seqPrefixForLayerAndMode(int layer, int modeIndex);
    static const std::array<juce::String, 8>& linkBaseParamIDs();
    static const std::array<juce::String, 8>& unlinkRateBaseParamIDs();
    static juce::String unlinkRateLabelForChoice(int choiceIndex);

    void refreshPageButtons();
    void applyModeLayerChoiceLabels(int modeIndex);
    void updateRateLengthOverlay();
    static juce::String formatRateLengthDecimal(double beats);
    int readLinkModeForLayer(int layer) const;
    int readUnlinkRateChoiceForLayer(int layer) const;
    juce::AudioParameterChoice*& stepChoicePointerForLayerStepMode(int layer, int step0Based, int modeIndex) noexcept;
    juce::AudioParameterChoice* getStepChoiceParameterForLayerStepMode(int layer, int step0Based, int modeIndex);
    void openUnlinkRateMenuForLayer(int layer, juce::Component& target);
    void refreshLinkToggleRateLabels();

    // Reproject only one layer from canonical base values to sequencer view.
    // Use this on fast morph edits to reduce UI work and avoid micro-freezes.
    void refreshSingleLayerDisplayFromBase(int layer);
    void refreshStepDisplayFromBase();
    int  applyModifierToStepChoice(int layer, int choiceIndex) const;
    int  removeModifierFromStepChoice(int layer, int choiceIndex) const;
    int  modifierForLayer(int layer) const;

    void resetPlaybackState(double ppqNow);
    void clearPlaybackState();
    bool advancePlaybackTo(double ppqNow);
    bool triggerOneRateStep();
    int  effectiveSequenceLengthForLayer(int layer) const;
    void resolveMusicalStateForCurrentTick();
    double beatsForRateChoice(int choiceIndex) const;
    void refreshParameterPointerCache();

    int activePageLayerForSlot(int slot) const;
    juce::String activePageLabelForSlot(int slot) const;

    juce::AudioProcessorValueTreeState& parameters;
    MidivisiViciAudioProcessor& processor;
    const Lane lane;

    std::atomic<bool> applyingFromState { false };

    LeftClickToggleButton arpeggiatorTitleButton { "Arpeggiator" };
    std::unique_ptr<ShadowComponent> arpeggiatorTitleButtonShadow;
    SplitterModeToggle arpeggiatorModeToggle;

    int spaceHeaderToRow1 = 8;
    int spaceRow1ToRow2 = 7;
    int spaceRow2ToSeq = 8;

    // Row 1: morph modifiers qui deforment l affichage de pas
    // sans changer la base stockee (baseLayerValues).
    std::unique_ptr<CustomRotaryNoCombo> rateKnob;
    std::unique_ptr<CustomRotaryNoCombo> gateKnob;
    std::unique_ptr<CustomRotaryNoCombo> grooveKnob;
    std::unique_ptr<CustomRotaryNoCombo> velocityKnob;

    // Row 2: selecteurs de pages (couches "musicales").
    // Nommage interne:
    // - kLayerPattern/kLayerRange restent les IDs historiques.
    // - Semantique metier:
    //   Arp  => Jump / Octave
    //   Drum => Velo Env / Tim Env
    std::array<std::unique_ptr<PageDialButton>, 4> pageButtons;

    std::unique_ptr<StepSequencerGrid> sequencer;
    int activeLayer = 0;

    // Valeurs canoniques (base APVTS) par layer/step.
    // Important: ces valeurs restent la reference, les morph knobs appliquent
    // une projection visuelle temporaire.
    std::array<std::array<int, 32>, 8> baseLayerValues {};

    std::array<std::unique_ptr<LinkToggle>, 8> layerLinkToggles;

    // Etat de lecture UI (visualisation uniquement).
    // Doit rester coherent avec transport mais n est pas l horloge audio.
    bool wasPlaying = false;
    bool hasLastUiPpq = false;
    double lastUiPpq = 0.0;
    double nextRateStepPpq = 0.0;
    std::array<int, 8> playbackCursorByLayer { 0, 0, 0, 0, 0, 0, 0, 0 };
    std::array<int, 8> playbackStepByLayer   { -1, -1, -1, -1, -1, -1, -1, -1 };
    juce::Random playbackRandom { 0x51A4C31u };

    // Etat musical derive de la position de lecture courante.
    // Utilise pour afficher/anticiper la logique, pas pour produire le MIDI final.
    int currentDirectionMode = 1;
    int currentJumpSize      = 1; // 1..8
    double currentStepDurationBeats = 0.25;
    int currentOctaveOffset  = 0; // -4..+4
    int currentRetrigCount   = 1; // 1..8

    // Cache de pointeurs APVTS pour eviter des recherches string a chaque tick UI.
    // Tous ces pointeurs sont resolves sur le thread UI et utilises uniquement
    // dans le composant UI (jamais sur l audio thread).
    std::atomic<float>* arpeggiatorEnableRaw = nullptr;
    juce::AudioParameterBool* arpeggiatorEnableParam = nullptr;
    std::atomic<float>* arpeggiatorModeRaw = nullptr;
    juce::AudioParameterChoice* arpeggiatorModeParam = nullptr;
    std::array<juce::RangedAudioParameter*, 4> morphParamPointers {};
    std::array<std::atomic<float>*, 8> layerLinkRawPointers {};
    std::array<std::atomic<float>*, 8> layerUnlinkRateRawPointers {};
    // Arp and Drum have independent pages for layers Direction/Pattern/Range.
    // Note: "Pattern/Range" = alias de stockage historique (Jump/Octave ou
    // Velo Env/Tim Env selon le mode).
    // Shared layers (Rythm/Gate/Groove/Velocity/Retrig) point to the same
    // parameters in both modes.
    std::array<std::array<juce::AudioParameterChoice*, 32>, 8> stepChoiceParamPointersArp {};
    std::array<std::array<juce::AudioParameterChoice*, 32>, 8> stepChoiceParamPointersDrum {};
    juce::File loadedModulePresetFile;
    juce::String loadedModulePresetName;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Arpeggiator)
};
