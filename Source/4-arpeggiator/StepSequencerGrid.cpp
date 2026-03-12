//==============================================================================
// StepSequencerGrid.cpp
//==============================================================================

#include "StepSequencerGrid.h"

namespace
{
    constexpr int kNumLayers = 8;
    constexpr int kCanonicalStepCount = 32;

    // IMPORTANT:
    // L ordre ici suit l ordre des layers utilises par StepSequencerGrid/UI:
    // 0=Rate, 1=Gate, 2=Groove, 3=Velocity, 4=Strum, 5=Jump, 6=Octave, 7=Retrig.
    // Ne pas confondre avec certains mappings internes du processor.
    static inline int defaultChoiceForLayer(int layerIndex0Based) noexcept
    {
        static constexpr std::array<int, kNumLayers> kDefaults {
            5,    // Rate: 1/16
            8,    // Gate: 99%
            8,    // Groove: "="
            4,    // Velocity: "="
            2,    // Strum: Up
            1,    // Jump: 1
            5,    // Octave: 0
            1     // Retrig: x1
        };

        const int clamped = juce::jlimit(0, kNumLayers - 1, layerIndex0Based);
        return kDefaults[(size_t) clamped];
    }

    static inline const juce::StringArray& choiceLabelsForLayer(
        int layerIndex0Based,
        const std::array<const juce::StringArray*, kNumLayers>& overrides) noexcept
    {
        const int clamped = juce::jlimit(0, kNumLayers - 1, layerIndex0Based);
        if (const auto* ptr = overrides[(size_t) clamped])
            return *ptr;

        return arpStepLayerChoices[clamped];
    }

    static inline int minChoiceForStep(int stepIndex0Based) noexcept
    {
        return (stepIndex0Based == 0 ? 1 : 0);
    }

    static inline int clampChoiceForStep(int stepIndex0Based, int maxChoice, int value) noexcept
    {
        const int clampedMax = juce::jmax(0, maxChoice);
        const int clampedMin = juce::jlimit(0, clampedMax, minChoiceForStep(stepIndex0Based));
        return juce::jlimit(clampedMin, juce::jmax(clampedMin, clampedMax), value);
    }
}

StepSequencerGrid::StepSequencerGrid()
    : StepSequencerGrid(1, 32)
{
}

StepSequencerGrid::StepSequencerGrid(int numRows, int numCols)
{
    // Borner la geometrie supportee.
    rows = juce::jlimit(1, 4, numRows);
    cols = juce::jlimit(1, 32, numCols);

    // Initialiser le modele canonique a Skip partout.
    for (auto& layer : layerValues)
        layer.fill(0);

    buildSteps();
    setActiveLayer(0);
}

//==============================================================================
// Internal helpers
//==============================================================================

int StepSequencerGrid::clampLayerIndex(int layer) const noexcept
{
    return juce::jlimit(0, kNumLayers - 1, layer);
}

bool StepSequencerGrid::isStepIndexValid(int stepIndex0Based) const noexcept
{
    return juce::isPositiveAndBelow(stepIndex0Based, kCanonicalStepCount);
}

void StepSequencerGrid::applyActiveLayerToVisibleSteps()
{
    // Projeter les valeurs canoniques du layer actif vers les cellules visibles.
    const int visibleCount = (int)steps.size();
    const auto* choiceLabels = layerChoiceLabels[(size_t) activeLayer];
    const auto& choices = choiceLabelsForLayer(activeLayer, layerChoiceLabels);

    for (int i = 0; i < visibleCount; ++i)
    {
        // Les cellules visibles peuvent etre < 32 (si cols < 32).
        // Le modele reste 32, donc mapping direct i -> i.
        if (!isStepIndexValid(i))
            break;

        auto& cell = *steps[(size_t)i];
        cell.setMinimumChoiceIndex(minChoiceForStep(i));
        cell.setChoiceLabels(choiceLabels);
        cell.setLayer(activeLayer);

        const int clampedValue = clampChoiceForStep(i, choices.size() - 1, layerValues[(size_t) activeLayer][(size_t) i]);
        layerValues[(size_t) activeLayer][(size_t) i] = clampedValue;
        cell.setValue(clampedValue, false);
        cell.setPlayhead(i == playbackStep);
    }
}

//==============================================================================
// Build
//==============================================================================

void StepSequencerGrid::buildSteps()
{
    steps.clear();
    steps.reserve((size_t)(rows * cols));

    const int visibleCount = rows * cols;

    for (int i = 0; i < visibleCount; ++i)
    {
        auto step = std::make_unique<StepToggle>();

        // Initialiser avec le layer actif courant (valeur sure).
        step->setLayer(activeLayer);

        // StepToggle -> modele canonique:
        // utiliser UNIQUEMENT onValueChanged pour eviter les doublons.
        step->onValueChanged = [this, i](int newVal)
        {
            if (!juce::isPositiveAndBelow(activeLayer, kNumLayers))
                return;

            if (!isStepIndexValid(i))
                return;

            const auto& choices = choiceLabelsForLayer(activeLayer, layerChoiceLabels);
            const int clamped = clampChoiceForStep(i, choices.size() - 1, newVal);
            layerValues[(size_t)activeLayer][(size_t)i] = clamped;

            // Defensif: conserver la cellule et le modele dans le meme domaine
            // meme si une source exterieure envoie une valeur hors plage.
            if (clamped != newVal && juce::isPositiveAndBelow(i, (int) steps.size()))
                steps[(size_t) i]->setValue(clamped, false);

            if (onStepChanged)
                onStepChanged(i, clamped);
        };

        // Clic gauche:
        // - pas 1: no-op (jamais Skip, ne copie pas).
        // - si Skip: copie le precedent non-Skip.
        // - sinon: passe en Skip.
        step->onLeftClick = [this, i]()
        {
            selectedStep = i;

            // Le pas 1 reste fixe pour garder un comportement defini.
            if (i == 0)
                return;

            const int layer = juce::jlimit(0, kNumLayers - 1, activeLayer);
            const int currentValue = layerValues[(size_t) layer][(size_t) i];

            // Non-Skip -> Skip.
            if (currentValue != 0)
            {
                setStepValue(i, 0, true);
                return;
            }

            // Skip -> copier le precedent non-Skip le plus proche.
            int targetValue = 0;
            for (int previous = i - 1; previous >= 0; --previous)
            {
                const int candidate = layerValues[(size_t) layer][(size_t) previous];
                if (candidate != 0)
                {
                    targetValue = candidate;
                    break;
                }
            }

            // Secours defensif (normalement inutile car pas 1 non-Skip).
            if (targetValue == 0)
                targetValue = defaultChoiceForLayer(layer);

            setStepValue(i, targetValue, true);
        };

        // Clic droit:
        // l edition est geree dans StepToggle, la grille garde juste la selection.
        step->onRightClick = [this, i]()
        {
            selectedStep = i;
        };

        steps.push_back(std::move(step));
        addAndMakeVisible(*steps.back());
    }
}

//==============================================================================
// Layout
//==============================================================================

void StepSequencerGrid::resized()
{
    auto area = getLocalBounds();

    // Taille fixe des cellules pour garder une grille stable.
    const int stepWidth  = 28;
    const int stepHeight = 28;
    const int spacing    = 4;

    const int totalStepsWidth = cols * stepWidth + (cols - 1) * spacing;

    // Centrer horizontalement sans toucher la hauteur.
    area = area.withSizeKeepingCentre(totalStepsWidth, area.getHeight());

    for (int y = 0; y < rows; ++y)
    {
        for (int x = 0; x < cols; ++x)
        {
            const int index = y * cols + x;

            if (!juce::isPositiveAndBelow(index, (int)steps.size()))
                continue;

            const int xPos = x * (stepWidth + spacing);
            const int yPos = y * (stepHeight + spacing);

            steps[(size_t)index]->setBounds(area.getX() + xPos, area.getY() + yPos, stepWidth, stepHeight);
        }
    }
}

//==============================================================================
// External control
//==============================================================================

void StepSequencerGrid::setActiveLayer(int layer)
{
    const int clampedLayer = clampLayerIndex(layer);
    if (activeLayer == clampedLayer)
        return;

    activeLayer = clampedLayer;
    applyActiveLayerToVisibleSteps();
}

void StepSequencerGrid::setLayerChoiceLabels(int layer, const juce::StringArray* labels)
{
    const int clampedLayer = clampLayerIndex(layer);
    layerChoiceLabels[(size_t) clampedLayer] = labels;

    if (clampedLayer == activeLayer)
        applyActiveLayerToVisibleSteps();
}

void StepSequencerGrid::setLayerStepValue(int layer, int stepIndex0Based, int choiceIndex)
{
    layer = clampLayerIndex(layer);

    if (!isStepIndexValid(stepIndex0Based))
        return;

    const auto& choices = choiceLabelsForLayer(layer, layerChoiceLabels);
    const int clampedValue = clampChoiceForStep(stepIndex0Based, choices.size() - 1, choiceIndex);
    layerValues[(size_t)layer][(size_t)stepIndex0Based] = clampedValue;

    // Si layer actif, rafraichir la cellule visible sans notifier.
    if (layer == activeLayer && juce::isPositiveAndBelow(stepIndex0Based, (int)steps.size()))
        steps[(size_t)stepIndex0Based]->setValue(clampedValue, false);
}

void StepSequencerGrid::setStepValue(int stepIndex0Based, int choiceIndex, bool notify)
{
    if (!juce::isPositiveAndBelow(activeLayer, kNumLayers))
        return;

    if (!isStepIndexValid(stepIndex0Based))
        return;

    const auto& choices = choiceLabelsForLayer(activeLayer, layerChoiceLabels);
    const int clampedValue = clampChoiceForStep(stepIndex0Based, choices.size() - 1, choiceIndex);
    layerValues[(size_t)activeLayer][(size_t)stepIndex0Based] = clampedValue;

    // notify=true -> StepToggle appelle onValueChanged -> onStepChanged.
    // C est voulu pour une action utilisateur.
    if (juce::isPositiveAndBelow(stepIndex0Based, (int)steps.size()))
        steps[(size_t)stepIndex0Based]->setValue(clampedValue, notify);

    // IMPORTANT:
    // ne pas appeler onStepChanged ici directement pour eviter les doublons.
}

void StepSequencerGrid::setPlaybackStep(int stepIndex0Based)
{
    const int clamped = isStepIndexValid(stepIndex0Based) ? stepIndex0Based : -1;

    if (playbackStep == clamped)
        return;

    const int previous = playbackStep;
    playbackStep = clamped;

    if (juce::isPositiveAndBelow(previous, (int)steps.size()))
        steps[(size_t)previous]->setPlayhead(false);

    if (juce::isPositiveAndBelow(playbackStep, (int)steps.size()))
        steps[(size_t)playbackStep]->setPlayhead(true);
}
