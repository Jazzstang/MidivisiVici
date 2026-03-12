/**
 * @file Splitter.h
 * @brief Lane-aware Splitter UI module (direct out + 4 configurable branches).
 *
 * Threading:
 * - UI thread only.
 * - Parameter polling/sync done from timer on message thread.
 * - Not RT-safe.
 */
#pragma once

#include <JuceHeader.h>
#include <array>
#include <memory>
#include <vector>

#include "../PluginColours.h"
#include "../PluginParameters.h"
#include "SplitterModeToggle.h"
#include "SplitLineToggle.h"
#include "SplitterLineComponent.h"
#include "../0-component/FlatComboBox.h"
#include "../0-component/LeftClickToggleButton.h"
#include "../0-component/ShadowComponent.h"

/**
 * @brief Splitter panel controlling one lane routing topology.
 *
 * Pattern:
 * - Pattern: Facade + composition
 * - Problem solved: present one coherent UI over multiple branch controls,
 *   mode switching, and channel uniqueness constraints.
 * - Participants: `Splitter`, `SplitterLineComponent`, `SplitLineToggle`,
 *   `SplitterModeToggle`.
 * - Flow: UI edits -> lane param writes -> processor enforces routing policy.
 * - Pitfalls: keep mode-dependent visibility and channel availability consistent.
 */
class Splitter : public juce::Component,
                 private juce::Timer
{
public:
    explicit Splitter(juce::AudioProcessorValueTreeState& state, Lanes::Lane laneIn);
    ~Splitter() override;

    static constexpr int numLines = 5; // 1 ligne pass-thru + 4 splits

    // Souris (globales) — les flèches sont gérées par les hotspots internes.
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    void mouseDown(const juce::MouseEvent& e) override;

    void resized() override;
    void paint(juce::Graphics& g) override;
    void uiTimerTick();

    // Utilitaires (encore utilisés ailleurs)
    // splitIndex: 0..3 (correspond aux lignes 2..5)
    bool isInLineArrowHitbox(int splitIndex, juce::Point<int> ptInSplitter) const;

    // Requis par SplitterLineComponent::hitTest
    bool isInAnyArrowHitbox(juce::Point<int> ptInSplitter) const;

private:
    // Timer
    void timerCallback() override;

    // Hotspot cliquable invisible pour une flèche (1 composant = 1 ligne)
    class ArrowHotspot;

    // 1..5 utilisés (index 0 inutilisé par simplicité)
    std::array<std::unique_ptr<ArrowHotspot>, 6> arrowHotspots{};

    // Debug/outil : renvoie 1..5 si 'pt' est dans une hitbox de flèche, sinon -1
    int lineNoAtPoint(juce::Point<int> ptInSplitter) const;

    // IDs/Canaux : vérification et recherche
    bool isIdAllowedFor(int requesterIndex, int itemId) const;
    int  findNextAvailableId(int requesterIndex, int preferredStartId) const;

    // États d’activation précédents (L1..L5) pour corriger à l’activation
    std::array<bool, 5> prevActive{};

    void updateVisibility();
    void applyGlobalUiBypass(bool bypassed);
    void syncFromParameters();
    void writeBoolParam(const char* baseId, bool value);
    void writeIntParam(const char* baseId, int value);
    void writeChoiceIndexParam(const char* baseId, int index);
    bool readBoolParam(const char* baseId, bool fallback = false) const;
    int  readIntParam(const char* baseId, int fallback = 0) const;
    int  readChoiceIndexParam(const char* baseId, int fallback = 0) const;
    void pushUiStateToParameters();
    juce::String laneParamId(const char* baseId) const;
    void showModulePresetMenu();
    void showSaveModulePresetDialog();
    bool saveLoadedModulePreset();
    void showEditLoadedPresetDialog();
    [[nodiscard]] juce::ValueTree captureModulePresetState() const;
    void applyModulePresetState(const juce::ValueTree& state);

    // === State / VTS ===
    juce::AudioProcessorValueTreeState& parameters;
    const Lanes::Lane lane;
    bool applyingFromState = false;
    juce::File loadedModulePresetFile;
    juce::String loadedModulePresetName;

    // === Header ===
    LeftClickToggleButton           splitterTitleButton { "Voice Splitter" };
    std::unique_ptr<ShadowComponent> splitterTitleButtonShadow;
    SplitterModeToggle               modeToggle;

    // === Colonne verticale de toggles (L1..L5) ===
    std::vector<std::unique_ptr<SplitLineToggle>> lineToggles;

    // === Ligne 1 : ComboBox seule ===
    FlatComboBox                                 line1Combo;
    std::unique_ptr<ShadowComponent>             line1ComboShadow;
    juce::Label                                  directOutLabel;
    juce::Rectangle<int>                         directOutArrowHitbox;

    // === Lignes 2 à 5 : composants complets ===
    std::vector<std::unique_ptr<SplitterLineComponent>> splitLines;

    // Hitboxes indexées par numéro de ligne : [1..5] utilisés (0 inutilisé)
    std::array<juce::Rectangle<int>, 6> arrowHitboxByLine{};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Splitter)
};
