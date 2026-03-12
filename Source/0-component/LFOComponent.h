/**
 * @file LFOComponent.h
 * @brief Lightweight LFO waveform visualizer with optional user interaction.
 *
 * Threading:
 * - UI thread only.
 * - Not RT-safe.
 */
#pragma once
#include <JuceHeader.h>
#include "../PluginColours.h"

/**
 * @brief Displays one animated LFO curve for the editor.
 *
 * Pattern:
 * - Pattern: Strategy (shape enum)
 * - Problem solved: one renderer supports multiple waveform shapes.
 * - Participants: `LFOComponent`, `Shape`.
 * - Flow: host/UI updates parameters -> component caches state -> paint uses
 *   state to draw waveform and phase cursor.
 * - Pitfalls: keep animation work in UI scheduler to avoid message-thread spikes.
 */
class LFOComponent : public juce::Component
{
public:
    /** @brief Supported waveform shapes. */
    enum class Shape { Sine, Triangle, Saw, Square, Random };

    LFOComponent();

    /** @brief Enable/disable UI animation updates. */
    void setActive(bool shouldAnimate);
    
    // === Interaction utilisateur ===
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;

    /** @brief Set waveform shape. */
    void setShape(Shape newShape);
    /**
     * @brief Convert musical division + tempo to internal rate.
     * @param divisionFactor Beat fraction for one cycle.
     * @param bpm Current host tempo.
     */
    void setRateFromDivision(float divisionFactor, double bpm);
    /** @brief Set vertical modulation depth. */
    void setDepth(float newDepth);
    /** @brief Set free-run phase offset when not externally driven. */
    void setPhaseOffset(float offset);
    /** @brief Enable external phase driving mode. */
    void setExternalPhaseDriven(bool shouldUseExternalPhase);
    /** @brief Set visual palette colors (background + waveform/cursor). */
    void setPaletteColours(juce::Colour background, juce::Colour foreground);

    /** @brief Draw one fixed cycle and move only the phase cursor. */
    void setDrawOneFullCycleFromZero(bool b);
    /** @brief Set cursor phase in normalized range [0..1). */
    void setPhaseCursor(float p01);

    /** @brief Reset internal visual state. */
    void reset();
    /** @brief UI scheduler tick hook (called by shared UI timer). */
    void uiTimerTick();

    // JUCE
    void resized() override;
    void paint(juce::Graphics& g) override;

private:

    // Dessin de la forme
    juce::Path generateLFOPath() const;
    void invalidateStaticBackground();
    void rebuildStaticBackgroundIfNeeded();
    juce::Rectangle<int> cursorRepaintBounds(float phase01) const;

    // Etat
    Shape  shape        = Shape::Sine;
    bool   isActive     = false;

    // Quand true : on dessine toujours 1 cycle 0..1 ancré.
    // La forme ne "glisse" plus, seul un curseur vertical bouge.
    bool   drawFullCycleFromZero = true;

    // Vitesse (Hz) si on utilise l'animation interne (mode non-ancré)
    float  rateHz       = 0.0f;

    // -1..1 (tu limites côté UI), 0 = ligne horizontale
    float  depth        = 0.0f;

    // Décalage de phase (seulement utile si non-ancré / mode libre)
    float  phaseOffset  = 0.0f;
    bool isDraggingOffset = false;
    juce::Point<float> dragStart;
    float dragStartOffset = 0.0f;

    // Phase interne (si non-ancré)
    float  phase        = 0.0f;

    // Curseur de phase [0..1) (utilisé quand drawFullCycleFromZero = true)
    float  phaseCursor  = 0.0f;
    bool   externalPhaseDriven = false;

    // Cache du fond + forme pour le mode ancré.
    juce::Image staticBackground;
    bool staticBackgroundDirty = true;
    juce::Colour backgroundColour = PluginColours::background;
    juce::Colour foregroundColour = PluginColours::onBackground;

    // Horloge interne pour timer quand non-ancré
    double lastTime     = 0.0;
};
