/*
==============================================================================
LFOComponent.cpp
------------------------------------------------------------------------------
Role du fichier:
- Rendu visuel d un LFO individuel dans l UI.

Modes de fonctionnement:
1) Mode ancre (drawFullCycleFromZero = true)
   - La courbe complete est fixe.
   - Un curseur vertical se deplace.
   - Utilise un cache image pour limiter le cout repaint.
2) Mode non ancre
   - La courbe est recalculee avec phase interne.
   - Animation interne possible via uiTimerTick().

Pilotage:
- Le scheduler central (PluginEditor/LFOGroup) pilote principalement la phase.
- Ce composant reste purement UI et ne traite aucun evenement audio.

Contraintes:
- Message thread uniquement.
- Pas de dependance RT.
==============================================================================
*/

#include "LFOComponent.h"
#include <cmath>

LFOComponent::LFOComponent()
{
    lastTime = juce::Time::getMillisecondCounterHiRes() * 0.001;
    // Pas de timer local permanent: l animation est orchestree par le scheduler UI.
}

void LFOComponent::setActive(bool shouldAnimate)
{
    if (isActive == shouldAnimate)
        return;

    isActive = shouldAnimate;

    // En mode "ancré", on n'anime rien en interne : pas de timer, ce sont
    // les appels à setPhaseCursor(...) qui déclenchent repaint().
    if (drawFullCycleFromZero || externalPhaseDriven)
        return;

    if (isActive && rateHz > 0.0f)
        lastTime = juce::Time::getMillisecondCounterHiRes() * 0.001;
}

void LFOComponent::setExternalPhaseDriven(bool shouldUseExternalPhase)
{
    if (externalPhaseDriven == shouldUseExternalPhase)
        return;

    externalPhaseDriven = shouldUseExternalPhase;

    if (externalPhaseDriven)
        return;

    // Retour en mode phase interne: rebase l horloge pour eviter un saut visuel.
    if (!drawFullCycleFromZero && isActive && rateHz > 0.0f)
        lastTime = juce::Time::getMillisecondCounterHiRes() * 0.001;
}

void LFOComponent::setPaletteColours(juce::Colour background, juce::Colour foreground)
{
    if (backgroundColour == background && foregroundColour == foreground)
        return;

    backgroundColour = background;
    foregroundColour = foreground;
    invalidateStaticBackground();
    repaint();
}

void LFOComponent::setShape(Shape newShape)
{
    if (shape == newShape)
        return;

    shape = newShape;
    invalidateStaticBackground();
    repaint();
}

void LFOComponent::setRateFromDivision(float divisionFactor, double bpm)
{
    // Compat: ce mode sert surtout quand la phase n est pas drivee de l exterieur.
    if (divisionFactor <= 0.0f || bpm <= 0.0)
    {
        rateHz = 0.0f;
        return;
    }

    // Conversion musicale:
    // - divisionFactor exprime une fraction de ronde.
    // - 1 ronde = 4 beats.
    const double periodBeats = (double) divisionFactor * 4.0;
    const double beatsPerSec = bpm / 60.0;

    const float newRate = (float) (beatsPerSec / periodBeats); // cycles/s
    if (std::abs(rateHz - newRate) < 0.001f)
        return;

    rateHz = newRate;
}

void LFOComponent::setDepth(float newDepth)
{
    newDepth = juce::jlimit(-1.0f, 1.0f, newDepth);
    if (std::abs(depth - newDepth) < 0.001f)
        return;

    depth = newDepth;
    invalidateStaticBackground();
    repaint();
}

void LFOComponent::setPhaseOffset(float offset)
{
    float wrapped = std::fmod(offset, 1.0f);
    if (wrapped < 0.0f)
        wrapped += 1.0f;

    if (std::abs(phaseOffset - wrapped) < 0.0001f)
        return;

    phaseOffset = wrapped;
    invalidateStaticBackground();
    repaint();
}

void LFOComponent::setDrawOneFullCycleFromZero(bool b)
{
    if (drawFullCycleFromZero == b)
        return;

    drawFullCycleFromZero = b;

    // Si on repasse en mode non ancre, on re-synchronise le temps local.
    if (!drawFullCycleFromZero && isActive && rateHz > 0.0f && !externalPhaseDriven)
        lastTime = juce::Time::getMillisecondCounterHiRes() * 0.001;

    invalidateStaticBackground();
    repaint();
}

juce::Rectangle<int> LFOComponent::cursorRepaintBounds(float phase01) const
{
    auto bounds = getLocalBounds().toFloat().reduced(4.0f);
    const float x = bounds.getX() + bounds.getWidth() * juce::jlimit(0.0f, 1.0f, phase01);

    return juce::Rectangle<float>(x - 2.0f,
                                  bounds.getY() - 1.0f,
                                  4.0f,
                                  bounds.getHeight() + 2.0f)
        .getSmallestIntegerContainer();
}

void LFOComponent::setPhaseCursor(float p01)
{
    // Utilise par LFOGroup pour phase-lock multi LFO.
    const float clamped = juce::jlimit(0.0f, 1.0f, p01);
    if (!drawFullCycleFromZero)
    {
        if (std::abs(phase - clamped) < 0.0001f)
            return;

        phase = clamped;
        repaint();
        return;
    }

    if (std::abs(phaseCursor - clamped) < 0.0001f)
        return;

    // Micro-optim UI: repaint uniquement l union ancienne/nouvelle position.
    const auto oldBounds = cursorRepaintBounds(phaseCursor);
    phaseCursor = clamped;
    const auto newBounds = cursorRepaintBounds(phaseCursor);

    repaint(oldBounds.getUnion(newBounds));
}

void LFOComponent::reset()
{
    // Reset coherent pour les deux modes (ancre et non ancre).
    phase       = 0.0f;
    phaseCursor = 0.0f;
    invalidateStaticBackground();
    repaint();
}

void LFOComponent::invalidateStaticBackground()
{
    staticBackgroundDirty = true;
}

void LFOComponent::rebuildStaticBackgroundIfNeeded()
{
    if (!drawFullCycleFromZero)
        return;

    const auto bounds = getLocalBounds();
    if (bounds.isEmpty())
    {
        staticBackground = {};
        return;
    }

    if (!staticBackgroundDirty
        && staticBackground.isValid()
        && staticBackground.getWidth() == bounds.getWidth()
        && staticBackground.getHeight() == bounds.getHeight())
    {
        return;
    }

    staticBackground = juce::Image(juce::Image::ARGB, bounds.getWidth(), bounds.getHeight(), true);
    juce::Graphics bg(staticBackground);

    auto area = bounds.toFloat().reduced(1.0f);

    // Fond + courbe precalcules en image pour eviter un redraw vectoriel complet
    // a chaque frame en mode ancre.
    bg.setColour(backgroundColour);
    bg.fillRoundedRectangle(area, 14.0f);

    // --- Courbe LFO ---
    bg.setColour(foregroundColour);
    bg.strokePath(generateLFOPath(), juce::PathStrokeType(1.5f));

    staticBackgroundDirty = false;
}

void LFOComponent::resized()
{
    invalidateStaticBackground();
}

void LFOComponent::paint(juce::Graphics& g)
{
    if (drawFullCycleFromZero)
    {
        rebuildStaticBackgroundIfNeeded();

        if (staticBackground.isValid())
            g.drawImageAt(staticBackground, 0, 0);
    }
    else
    {
        auto area = getLocalBounds().toFloat().reduced(1.0f);

        // --- Fond arrondi ---
        g.setColour(backgroundColour);
        g.fillRoundedRectangle(area, 14.0f);

        // --- Courbe LFO ---
        g.setColour(foregroundColour);
        g.strokePath(generateLFOPath(), juce::PathStrokeType(1.5f));
    }

    // Curseur visible uniquement en mode ancre.
    if (drawFullCycleFromZero)
    {
        auto bounds = getLocalBounds().toFloat().reduced(4.0f);
        const float x = bounds.getX() + bounds.getWidth() * phaseCursor;

        g.setColour(foregroundColour);
        g.drawLine(x, bounds.getY(), x, bounds.getBottom(), 1.0f);
    }
}

void LFOComponent::uiTimerTick()
{
    // Animation interne uniquement en mode non ancre.
    // En mode phase externe, ce composant ne doit pas avancer tout seul.
    if (drawFullCycleFromZero || externalPhaseDriven || !isActive || rateHz <= 0.0f || std::abs(depth) <= 0.0f)
        return;

    const double currentTime = juce::Time::getMillisecondCounterHiRes() * 0.001;
    double delta = currentTime - lastTime;
    lastTime = currentTime;

    if (delta < 0.0)
        delta = 0.0;

    phase += (float) (rateHz * delta);
    while (phase >= 1.0f)
        phase -= 1.0f;

    repaint();
}

juce::Path LFOComponent::generateLFOPath() const
{
    juce::Path path;
    auto bounds = getLocalBounds().toFloat().reduced(4.0f);

    const int   numPoints = juce::jmax(2, (int) std::round(bounds.getWidth()));
    const float width     = bounds.getWidth();
    const float height    = bounds.getHeight();
    const float midY      = bounds.getCentreY();

    // Profondeur nulle: ligne horizontale au milieu.
    if (std::abs(depth) <= 0.0f || width <= 1.0f)
    {
        path.startNewSubPath(bounds.getX(), midY);
        path.lineTo(bounds.getRight(), midY);
        return path;
    }

    auto waveform = [&](float t01) -> float
    {
        // Mode ancre: pas de phase glissante, seulement offset utilisateur.
        float basePhase = drawFullCycleFromZero ? 0.0f : phase;
        float t = t01 + basePhase + phaseOffset; // phaseOffset est ton offset utilisateur
        t -= std::floor(t);

        auto randomUnit = [](float x) -> float
        {
            const float v = std::sin(x * 12.9898f) * 43758.5453f;
            return v - std::floor(v);
        };

        switch (shape)
        {
            case Shape::Sine:     return std::sin(t * juce::MathConstants<float>::twoPi);
            case Shape::Triangle: return 1.0f - 4.0f * std::abs(t - 0.5f);
            case Shape::Saw:      return 2.0f * (t - 0.5f);
            case Shape::Square:   return t < 0.5f ? 1.0f : -1.0f;
            case Shape::Random:
            {
                // Random deterministe par step: rendu stable d un repaint a l autre.
                constexpr float kStepsPerCycle = 16.0f;
                const float stepIndex = std::floor(t * kStepsPerCycle);
                const float u = randomUnit(stepIndex + 17.0f);
                return (u * 2.0f) - 1.0f;
            }
        }
        return 0.0f;
    };

    for (int x = 0; x < numPoints; ++x)
    {
        const float t01 = width > 1.0f ? (float) x / width : 0.0f;
        const float y   = midY - 0.45f * height * depth * waveform(t01);

        if (x == 0)
            path.startNewSubPath(bounds.getX() + x, y);
        else
            path.lineTo(bounds.getX() + x, y);
    }

    return path;
}

void LFOComponent::mouseDown(const juce::MouseEvent& e)
{
    // === Double-clic -> reset offset ===
    if (e.getNumberOfClicks() == 2)
    {
        setPhaseOffset(0.0f);
        return;
    }

    // === Sinon : début de drag ===
    isDraggingOffset = true;
    dragStart = e.position;
    dragStartOffset = phaseOffset;
}

void LFOComponent::mouseDrag(const juce::MouseEvent& e)
{
    if (!isDraggingOffset)
        return;

    const float deltaX = (e.position.x - dragStart.x);
    const float sensitivity = 1.0f / 150.0f; // ajustable
    const float newOffset = dragStartOffset + deltaX * sensitivity;
    setPhaseOffset(newOffset);
}

void LFOComponent::mouseUp(const juce::MouseEvent&)
{
    isDraggingOffset = false;
}

void LFOComponent::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    const float delta = -wheel.deltaY * 0.05f; // sens et vitesse du scroll
    setPhaseOffset(phaseOffset + delta);
}
