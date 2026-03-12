//==============================================================================
// StepToggle.cpp
//------------------------------------------------------------------------------
// Role du fichier:
// - Definir la vue editable d un pas (cellule) pour tous les layers du
//   sequenceur arpeggiator.
//
// Place dans l architecture:
// - Composant UI pur, sans logique audio.
// - Utilise par StepSequencerGrid qui pilote la coherence globale des steps.
//
// Flux principal:
// 1) La grille fixe layer/labels/valeur.
// 2) L utilisateur interagit (drag/click/wheel/right-click).
// 3) StepToggle valide/clamp la valeur locale.
// 4) StepToggle notifie la grille via callbacks.
//
// Point de vigilance:
// - Les indices de choices definis ici doivent rester synchronises avec
//   PluginParameters + ArpeggiatorProcessor. Une desynchronisation produit des
//   comportements musicaux faux sans erreur de compilation.
//==============================================================================

#include "StepToggle.h"
#include "../PluginLookAndFeel.h"
#include "../PluginColours.h"
#include <BinaryData.h>
#include <array>
#include <cmath>

//==============================================================================
// Layer choices (doivent matcher PluginParameters exactement)
//------------------------------------------------------------------------------
// Convention:
// - Chaque entree est un indice discret (AudioParameterChoice).
// - Les indices sont utilises plus loin par le moteur pour decoder la logique.
// - Les labels sont UI; l ordre des indices est contractuel.
//
// Consequence:
// - Changer un ordre ou inserer une valeur "au milieu" impose une mise a jour
//   coordonnee de:
//   1) PluginParameters (liste des choices APVTS)
//   2) ArpeggiatorProcessor (decode runtime des indices)
//   3) Arpeggiator UI (preview/etat visuel eventuel)
//==============================================================================

const juce::StringArray arpStepLayerChoices[8] = {
    // 0) Rate
    juce::StringArray{
        "Skip",
        // Binaries (1:1)
        "1/1", "1/2", "1/4", "1/8", "1/16", "1/32", "1/64",
        // Dotted (3:2)
        "3/2", "3/4", "3/8", "3/16", "3/32", "3/64", "3/128",
        // Double dotted (7:4)
        "7/4", "7/8", "7/16", "7/32", "7/64", "7/128", "7/256",
        // Triolet (3:2)
        "2/3", "1/3", "1/6", "1/12", "1/24", "1/48", "1/96",
        // Quintolet (5:4)
        "4/5", "2/5", "1/5", "1/10", "1/20", "1/40", "1/80",
        // Septolet (7:4)
        "4/7", "2/7", "1/7", "1/14", "1/28", "1/56", "1/112",
        "Rnd"
    },

    // 1) Strum
    [] {
        juce::StringArray arr;
        const auto up = juce::String::fromUTF8("\xE2\x86\x91");          // ↑
        const auto down = juce::String::fromUTF8("\xE2\x86\x93");        // ↓
        const auto top = juce::String::fromUTF8("\xE2\xA4\x92");         // ⤒ (up to bar)
        const auto bottom = juce::String::fromUTF8("\xE2\xA4\x93");      // ⤓ (down to bar)

        arr.add("Skip");
        arr.add(top);
        arr.add(up);
        arr.add("=");
        arr.add(down);
        arr.add(bottom);
        arr.add("Chord");
        arr.add(up + " + " + up);
        arr.add(down + " + " + down);
        arr.add("Mute");
        arr.add("Rnd");

        arr.add("Chord " + up + " 1/4");
        arr.add("Chord " + up + " 1/3");
        arr.add("Chord " + up + " 1/2");
        arr.add("Chord " + up + " 2/3");
        arr.add("Chord " + up + " 3/4");
        arr.add("Chord " + up + " 1/1");
        arr.add("Chord " + down + " 1/4");
        arr.add("Chord " + down + " 1/3");
        arr.add("Chord " + down + " 1/2");
        arr.add("Chord " + down + " 2/3");
        arr.add("Chord " + down + " 3/4");
        arr.add("Chord " + down + " 1/1");
        arr.add("Chord Rnd");
        return arr;
    }(),

    // 2) Jump
    juce::StringArray{
        "Skip",
        "1", "2", "3", "4", "5", "6", "7", "8",
        "Rnd"
    },

    // 3) Octave
    juce::StringArray{
        "Skip",
        "-4", "-3", "-2", "-1",
        "0",
        "+1", "+2", "+3", "+4",
        "Rnd"
    },

    // 4) Velocity
    juce::StringArray{
        "Skip",
        "ppp", "pp", "p",
        "=",
        "mf",
        "f", "ff", "fff"
    },

    // 5) Groove
    [] {
        juce::StringArray arr {
            "Skip",
            "-75",
            "-66",
            "-50",
            "-33",
            "-25%",
            "-10%",
            "-5%",
            "=",
            "+5%",
            "+10%",
            "+25%",
            "+33%",
            "+50%",
            "+66%",
            "+75%"
        };
        return arr;
    }(),

    // 6) Gate
    juce::StringArray{
        "Skip",
        "5%",
        "10%",
        "25%",
        "33%",
        "50%",
        "66%",
        "75%",
        "99%",
        "Tie"
    },

    // 7) Retrig
    juce::StringArray{
        "Skip",
        "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8",
        "Rnd"
    }
};

StepToggle::StepToggle() = default;

namespace
{
    //--------------------------------------------------------------------------
    // Indices de direction:
    // Ces constantes codent le "contrat d index" pour le layer Strum.
    // Elles sont reutilisees par le menu contextuel specialise afin d offrir
    // une presentation claire (sous-menu Chord) sans casser le mapping interne.
    //--------------------------------------------------------------------------
    constexpr int kRateFirstIndex = 1;
    constexpr int kRateTypeCount = 6;
    constexpr int kRateRatiosPerType = 7;
    constexpr int kRateLastRegularIndex = kRateFirstIndex + (kRateTypeCount * kRateRatiosPerType) - 1; // 42
    constexpr int kRateRndIndex = kRateLastRegularIndex + 1; // 43
    constexpr float kRateMenuTupletSize = 8.5f;
    constexpr float kRateStepTupletSize = 8.5f;
    // UI layer routing (StepSequencerGrid order):
    // 0=Rate, 1=Gate, 2=Groove, 3=Velocity, 4=Strum, 5=Jump, 6=Octave, 7=Retrig.
    constexpr int kUiLayerRateIndex = 0;
    constexpr int kUiLayerGateIndex = 1;
    constexpr int kUiLayerStrumIndex = 4;

    // Choice table order (arpStepLayerChoices):
    // 0=Rate, 1=Strum, 2=Jump, 3=Octave, 4=Velocity, 5=Groove, 6=Gate, 7=Retrig.
    constexpr int kChoiceLayerStrumIndex = 1;

    constexpr int kDirectionSkipIndex = 0;
    constexpr int kDirectionTopIndex = 1;
    constexpr int kDirectionUpIndex = 2;
    constexpr int kDirectionEqualIndex = 3;
    constexpr int kDirectionDownIndex = 4;
    constexpr int kDirectionBottomIndex = 5;
    constexpr int kDirectionChordAllIndex = 6;
    constexpr int kDirectionUpPairIndex = 7;
    constexpr int kDirectionDownPairIndex = 8;
    constexpr int kDirectionMuteIndex = 9;
    constexpr int kDirectionRndIndex = 10;
    constexpr int kDirectionChordUpQuarterIndex = 11;
    constexpr int kDirectionChordUpWholeIndex = 16;
    constexpr int kDirectionChordDownQuarterIndex = 17;
    constexpr int kDirectionChordDownWholeIndex = 22;
    constexpr int kDirectionChordRndIndex = 23;

    static const std::array<const char*, kRateTypeCount> kRateTypeLabels {
        "Binaries (1:1)",
        "Dotted (3:2)",
        "Double dotted (7:4)",
        "Triolet (3:2)",
        "Quintolet (5:4)",
        "Septolet (7:4)"
    };

    struct RateVisual
    {
        juce::String glyph;
        int dotCount = 0;
        int tuplet = 0;
        bool valid = false;
    };

    static juce::Font makePopupJostFont(float size)
    {
        static juce::Typeface::Ptr jostRegular =
            juce::Typeface::createSystemTypefaceFor(BinaryData::JostRegular_ttf,
                                                    BinaryData::JostRegular_ttfSize);

        if (jostRegular != nullptr)
            return juce::Font(juce::FontOptions(jostRegular).withHeight(size));

        return juce::Font(juce::FontOptions(size));
    }

    static juce::Font makePopupNotoMusicFont(float size)
    {
        static juce::Typeface::Ptr notoMusic =
            juce::Typeface::createSystemTypefaceFor(BinaryData::NotoMusic_ttf,
                                                    BinaryData::NotoMusic_ttfSize);

        if (notoMusic != nullptr)
            return juce::Font(juce::FontOptions(notoMusic).withHeight(size));

        return juce::Font(juce::FontOptions(size));
    }

    static juce::String getRateGlyphForSlot(int slot1Based)
    {
        switch (slot1Based)
        {
            case 1: return juce::String::fromUTF8("\xF0\x9D\x85\x9D"); // whole
            case 2: return juce::String::fromUTF8("\xF0\x9D\x85\x9E"); // half
            case 3: return juce::String::fromUTF8("\xF0\x9D\x85\x9F"); // quarter
            case 4: return juce::String::fromUTF8("\xF0\x9D\x85\xA0"); // eighth
            case 5: return juce::String::fromUTF8("\xF0\x9D\x85\xA1"); // sixteenth
            case 6: return juce::String::fromUTF8("\xF0\x9D\x85\xA2"); // thirty-second
            case 7: return juce::String::fromUTF8("\xF0\x9D\x85\xA3"); // sixty-fourth
            default: break;
        }

        return {};
    }

    static RateVisual makeRateVisualForChoice(int choiceIndex)
    {
        RateVisual out;

        if (choiceIndex < kRateFirstIndex || choiceIndex > kRateLastRegularIndex)
            return out;

        const int zeroBased = choiceIndex - kRateFirstIndex;
        const int type = zeroBased / kRateRatiosPerType;          // 0..5
        const int slot1Based = (zeroBased % kRateRatiosPerType) + 1; // 1..7

        out.glyph = getRateGlyphForSlot(slot1Based);
        if (out.glyph.isEmpty())
            return out;

        if (type == 1)       out.dotCount = 1; // dotted
        else if (type == 2)  out.dotCount = 2; // double dotted

        if (type == 3)       out.tuplet = 3;   // triolet
        else if (type == 4)  out.tuplet = 5;   // quintolet
        else if (type == 5)  out.tuplet = 7;   // septolet

        out.valid = true;
        return out;
    }

    class RateMenuItem final : public juce::PopupMenu::CustomComponent
    {
    public:
        RateMenuItem(RateVisual visualIn, juce::String ratioText, bool isCurrentIn)
            : juce::PopupMenu::CustomComponent(true),
              visual(std::move(visualIn)),
              ratio(std::move(ratioText)),
              isCurrent(isCurrentIn)
        {
        }

        void getIdealSize(int& idealWidth, int& idealHeight) override
        {
            idealWidth = 172;
            idealHeight = 26;
        }

        void paint(juce::Graphics& g) override
        {
            auto bounds = getLocalBounds().toFloat();
            if (isItemHighlighted())
            {
                g.setColour(PluginColours::primary.withAlpha(0.18f));
                g.fillRoundedRectangle(bounds.reduced(1.0f), 4.0f);
            }

            const auto* item = getItem();
            const bool isEnabled = (item == nullptr || item->isEnabled);
            const bool isTicked = isCurrent;

            const juce::Colour fg = isEnabled
                ? juce::Colours::white
                : juce::Colours::white.withAlpha(0.45f);

            auto area = getLocalBounds().reduced(6, 0);

            auto tickArea = area.removeFromLeft(10).toFloat();
            if (isTicked)
            {
                g.setColour(fg);
                g.fillEllipse(tickArea.withSizeKeepingCentre(6.0f, 6.0f));
            }

            auto glyphArea = area.removeFromLeft(32);
            g.setColour(fg);
            const float menuGlyphSize = juce::jlimit(17.0f, 24.0f, (float) glyphArea.getHeight() * 0.9f);
            g.setFont(makePopupNotoMusicFont(menuGlyphSize));

            auto glyphDrawArea = glyphArea;
            if (visual.tuplet > 0)
                glyphDrawArea.removeFromTop(4);

            g.drawText(visual.glyph, glyphDrawArea, juce::Justification::centred, false);

            if (visual.dotCount > 0)
            {
                const float cy = glyphArea.getCentreY() + 1.5f;
                float cx = (float) glyphArea.getCentreX() + 6.0f;
                for (int i = 0; i < visual.dotCount; ++i)
                    g.fillEllipse(cx + (float) (i * 3), cy, 2.2f, 2.2f);
            }

            if (visual.tuplet > 0)
            {
                g.setFont(makePopupJostFont(kRateMenuTupletSize));
                g.drawText(juce::String(visual.tuplet),
                           glyphArea.withY(glyphArea.getY() - 1).withHeight(8),
                           juce::Justification::centred,
                           false);
            }

            g.setFont(makePopupJostFont(12.0f));
            g.drawText(ratio, area, juce::Justification::centredLeft, false);
        }

    private:
        RateVisual visual;
        juce::String ratio;
        bool isCurrent = false;
    };
}

const juce::StringArray& StepToggle::getChoices() const
{
    if (choiceLabelsOverride != nullptr)
        return *choiceLabelsOverride;

    return arpStepLayerChoices[currentLayer];
}

//==============================================================================
// Paint
//==============================================================================

void StepToggle::paint(juce::Graphics& g)
{
    if (auto* lnf = dynamic_cast<PluginLookAndFeel*>(&getLookAndFeel()))
    {
        const bool isActive = (value > 0);
        const auto rateVisual = (currentLayer == 0 ? makeRateVisualForChoice(value) : RateVisual{});
        juce::String labelToDraw = getLabelForCurrentValue();
        if (rateVisual.valid)
            labelToDraw.clear();

        lnf->drawStepToggle(g,
                            getLocalBounds(),
                            isActive,
                            currentLayer,
                            labelToDraw,
                            isEnabled(),
                            isMouseOver(),
                            isPlayhead);

        if (rateVisual.valid)
        {
            juce::Colour textColour = PluginColours::background;
            if (isEnabled())
            {
                const auto layerColours = PluginColours::getLayerColours(currentLayer);
                textColour = isActive ? layerColours.text : layerColours.background;
            }
            auto area = getLocalBounds();
            g.setColour(textColour);
            const float stepGlyphSize = juce::jlimit(17.0f, 26.0f, (float) area.getHeight() * 0.92f);
            g.setFont(makePopupNotoMusicFont(stepGlyphSize));

            auto glyphArea = area;
            if (rateVisual.tuplet > 0)
                glyphArea.removeFromTop(5);

            g.drawText(rateVisual.glyph, glyphArea, juce::Justification::centred, false);

            if (rateVisual.dotCount > 0)
            {
                const float cy = (float) area.getCentreY() + 2.3f;
                float cx = (float) area.getCentreX() + 7.0f;
                for (int i = 0; i < rateVisual.dotCount; ++i)
                    g.fillEllipse(cx + (float) (i * 2), cy, 2.2f, 2.2f);
            }

            if (rateVisual.tuplet > 0)
            {
                g.setFont(makePopupJostFont(kRateStepTupletSize));
                g.drawText(juce::String(rateVisual.tuplet),
                           area.withY(area.getY() + 1).withHeight(8),
                           juce::Justification::centred,
                           false);
            }
        }

        // Selection highlight can be implemented later in LookAndFeel.
        // "isSelected" is kept for future.
    }
    else
    {
        g.setColour(value > 0 ? juce::Colours::skyblue : juce::Colours::darkgrey);
        g.fillRect(getLocalBounds());
        g.setColour(juce::Colours::white);
        g.setFont(juce::Font(juce::FontOptions(12.0f)));
        g.drawFittedText(getLabelForCurrentValue(), getLocalBounds(), juce::Justification::centred, 1);
    }
}

//==============================================================================
// Mouse handling
//------------------------------------------------------------------------------
// Strategie d interaction:
// - mouseDown: memorise le contexte de geste.
// - mouseDrag: edition continue (fine/coarse selon cardinalite des choices).
// - mouseUp:
//   - click droit: menu specialise selon layer.
//   - click gauche sans drag: delegation a onLeftClick (logique de grille).
//------------------------------------------------------------------------------
// Important:
// StepToggle ne prend pas de decision metier "de sequenceur" ici. Il expose
// juste des intentions utilisateur. La grille et le code APVTS decident du
// sens final (copy previous, skip, etc).
//==============================================================================

void StepToggle::mouseDown(const juce::MouseEvent& e)
{
    pendingRightClick = e.mods.isRightButtonDown() || e.mods.isPopupMenu();
    wasDragged = false;

    dragStartY = e.getPosition().y;
    dragStartValue = value;

    // If right click: do not start drag.
    if (pendingRightClick)
        return;
}

void StepToggle::mouseDrag(const juce::MouseEvent& e)
{
    if (pendingRightClick)
        return;

    wasDragged = true;

    const auto& choices = getChoices();
    const int maxValue = choices.size() - 1;
    if (maxValue <= 0)
        return;

    // Map drag distance to choice index delta.
    // Bigger choice set -> smaller per-step delta.
    const float ratio = 200.0f / (float)juce::jmax(1, choices.size());
    const int delta = (int)((dragStartY - e.getPosition().y) / ratio);

    const int newVal = juce::jlimit(0, maxValue, dragStartValue + delta);

    if (newVal != value)
        setValue(newVal, true);
}

void StepToggle::mouseUp(const juce::MouseEvent& e)
{
    const bool isRight =
        pendingRightClick &&
        (e.mods.isRightButtonDown() || e.mods.isPopupMenu() || e.mouseWasClicked());

    if (isRight)
    {
        // IMPORTANT:
        // layer routing uses UI order from StepSequencerGrid:
        // 0=Rate, 1=Gate, 4=Strum.
        if (currentLayer == kUiLayerRateIndex)
            showRateChoiceMenu();
        else if (currentLayer == kUiLayerStrumIndex)
            showDirectionChoiceMenu();
        else if (currentLayer == kUiLayerGateIndex)
            showGateChoiceMenu();
        else
            showCompactChoiceMenu();
        pendingRightClick = false;
        return;
    }

    // If user dragged, value already committed via setValue(..., true).
    if (wasDragged)
        return;

    if (onLeftClick)
        onLeftClick();
}

void StepToggle::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    juce::ignoreUnused(e);

    const auto& choices = getChoices();
    const int maxValue = choices.size() - 1;
    if (maxValue <= 0)
        return;

    float delta = 0.0f;
    if (std::abs(wheel.deltaY) >= std::abs(wheel.deltaX))
        delta = wheel.deltaY;
    else
        delta = wheel.deltaX;

    if (std::abs(delta) <= 1.0e-6f)
        return;

    const int direction = (delta > 0.0f ? +1 : -1);
    int targetValue = value + direction;

    // Rate layer:
    // navigation molette contrainte au type courant pour eviter les sauts
    // transverses non souhaites (binaries -> triolet, etc).
    if (currentLayer == 0 &&
        value >= kRateFirstIndex &&
        value <= kRateLastRegularIndex)
    {
        const int currentType = (value - kRateFirstIndex) / kRateRatiosPerType;
        const int typeStart = kRateFirstIndex + currentType * kRateRatiosPerType;
        const int typeEnd = typeStart + kRateRatiosPerType - 1;

        const int minWithinType = juce::jmax(minChoiceIndex, typeStart);
        const int maxWithinType = juce::jmin(maxValue, typeEnd);
        targetValue = juce::jlimit(minWithinType, juce::jmax(minWithinType, maxWithinType), targetValue);
    }
    else
    {
        targetValue = juce::jlimit(0, maxValue, targetValue);
    }

    setValue(targetValue, true);
}

//==============================================================================
// State setters
//------------------------------------------------------------------------------
// Tous les setters font un clamp defensif, car les labels peuvent varier selon
// le layer/override. Ce clamp local evite toute lecture hors borne a l affichage.
//==============================================================================

void StepToggle::setValue(int newValue, bool notify)
{
    const auto& choices = getChoices();
    const int maxValue = choices.size() - 1;

    const int minValue = juce::jlimit(0, juce::jmax(0, maxValue), minChoiceIndex);
    const int clamped = juce::jlimit(minValue, juce::jmax(minValue, maxValue), newValue);
    if (value == clamped)
        return;

    value = clamped;

    if (notify && onValueChanged)
        onValueChanged(value);

    repaint();
}

void StepToggle::setLayer(int layerIndex)
{
    constexpr int kLastLayerIndex = (int) std::size(arpStepLayerChoices) - 1;
    const int clamped = juce::jlimit(0, kLastLayerIndex, layerIndex);
    if (currentLayer == clamped)
        return;

    currentLayer = clamped;

    // Clamp current value to new layer range.
    const auto& choices = getChoices();
    const int maxValue = juce::jmax(0, choices.size() - 1);
    const int minValue = juce::jlimit(0, maxValue, minChoiceIndex);
    value = juce::jlimit(minValue, juce::jmax(minValue, maxValue), value);

    repaint();
}

void StepToggle::setChoiceLabels(const juce::StringArray* choices)
{
    if (choiceLabelsOverride == choices)
        return;

    choiceLabelsOverride = choices;

    const auto& effectiveChoices = getChoices();
    const int maxValue = juce::jmax(0, effectiveChoices.size() - 1);
    const int minValue = juce::jlimit(0, maxValue, minChoiceIndex);
    value = juce::jlimit(minValue, juce::jmax(minValue, maxValue), value);
    repaint();
}

void StepToggle::setMinimumChoiceIndex(int minimumChoiceIndex)
{
    const int clamped = juce::jmax(0, minimumChoiceIndex);
    if (minChoiceIndex == clamped)
        return;

    minChoiceIndex = clamped;

    const auto& effectiveChoices = getChoices();
    const int maxValue = juce::jmax(0, effectiveChoices.size() - 1);
    const int minValue = juce::jlimit(0, maxValue, minChoiceIndex);
    value = juce::jlimit(minValue, juce::jmax(minValue, maxValue), value);
    repaint();
}

void StepToggle::setSelected(bool selected)
{
    if (isSelected == selected)
        return;

    isSelected = selected;
    repaint();
}

void StepToggle::setPlayhead(bool playhead)
{
    if (isPlayhead == playhead)
        return;

    isPlayhead = playhead;
    repaint();
}

juce::String StepToggle::getLabelForCurrentValue() const
{
    const auto& choices = getChoices();

    if (value >= 0 && value < choices.size())
        return choices[value];

    return juce::String(value);
}

void StepToggle::showCompactChoiceMenu()
{
    // Menu generique:
    // utile quand aucun rendu hierarchique n est necessaire.
    const auto& choices = getChoices();
    const int maxValue = choices.size() - 1;
    if (maxValue < 0)
        return;

    const int minValue = juce::jlimit(0, maxValue, minChoiceIndex);

    juce::PopupMenu menu;
    for (int i = minValue; i <= maxValue; ++i)
        menu.addItem(i + 1, choices[i], true, i == value);

    auto options = juce::PopupMenu::Options()
        .withTargetComponent(this)
        .withMinimumWidth(juce::jmax(96, getWidth()))
        .withMaximumNumColumns(1)
        .withStandardItemHeight(18);

    menu.showMenuAsync(options, [safe = juce::Component::SafePointer<StepToggle>(this)](int result)
    {
        if (safe == nullptr || result <= 0)
            return;

        safe->setValue(result - 1, true);

        if (safe->onRightClick)
            safe->onRightClick();
    });
}

void StepToggle::showDirectionChoiceMenu()
{
    // Menu specialise Strum:
    // - Modes de base en premier
    // - Sous-menu Chord pour garder une liste principale concise
    // - Rnd principal reste hors sous-menu
    const auto& choices = getChoices();
    const int maxValue = choices.size() - 1;
    if (maxValue < 0)
        return;

    // Le menu specialise Strum (avec sous-menu Chord) n est valable que pour
    // le jeu de labels arpeggiator standard.
    // En mode drum machine, la couche "direction" est reinterpretee en Hit
    // (X / O / Flam / Drag), donc on bascule sur le menu generique.
    if (&choices != &arpStepLayerChoices[kChoiceLayerStrumIndex])
    {
        showCompactChoiceMenu();
        return;
    }

    const int minValue = juce::jlimit(0, maxValue, minChoiceIndex);

    juce::PopupMenu menu;

    const auto addItemIfAllowed = [&](juce::PopupMenu& target,
                                      int choiceIndex,
                                      const juce::String& overrideLabel = {})
    {
        if (choiceIndex < minValue || choiceIndex > maxValue)
            return;

        if (!juce::isPositiveAndBelow(choiceIndex, choices.size()))
            return;

        const auto label = overrideLabel.isNotEmpty() ? overrideLabel : choices[choiceIndex];

        target.addItem(choiceIndex + 1, label, true, choiceIndex == value);
    };

    addItemIfAllowed(menu, kDirectionSkipIndex);
    addItemIfAllowed(menu, kDirectionTopIndex, juce::String::fromUTF8("\xE2\xA4\x92"));
    addItemIfAllowed(menu, kDirectionUpIndex);
    addItemIfAllowed(menu, kDirectionEqualIndex);
    addItemIfAllowed(menu, kDirectionDownIndex);
    addItemIfAllowed(menu, kDirectionBottomIndex, juce::String::fromUTF8("\xE2\xA4\x93"));

    juce::PopupMenu chordMenu;
    addItemIfAllowed(chordMenu, kDirectionChordAllIndex, "All");
    addItemIfAllowed(chordMenu, kDirectionChordUpQuarterIndex + 0, juce::String::fromUTF8("\xE2\x86\x91 1/4"));
    addItemIfAllowed(chordMenu, kDirectionChordUpQuarterIndex + 1, juce::String::fromUTF8("\xE2\x86\x91 1/3"));
    addItemIfAllowed(chordMenu, kDirectionChordUpQuarterIndex + 2, juce::String::fromUTF8("\xE2\x86\x91 1/2"));
    addItemIfAllowed(chordMenu, kDirectionChordUpQuarterIndex + 3, juce::String::fromUTF8("\xE2\x86\x91 2/3"));
    addItemIfAllowed(chordMenu, kDirectionChordUpQuarterIndex + 4, juce::String::fromUTF8("\xE2\x86\x91 3/4"));
    addItemIfAllowed(chordMenu, kDirectionChordUpWholeIndex,      juce::String::fromUTF8("\xE2\x86\x91 1/1"));
    addItemIfAllowed(chordMenu, kDirectionChordDownQuarterIndex + 0, juce::String::fromUTF8("\xE2\x86\x93 1/4"));
    addItemIfAllowed(chordMenu, kDirectionChordDownQuarterIndex + 1, juce::String::fromUTF8("\xE2\x86\x93 1/3"));
    addItemIfAllowed(chordMenu, kDirectionChordDownQuarterIndex + 2, juce::String::fromUTF8("\xE2\x86\x93 1/2"));
    addItemIfAllowed(chordMenu, kDirectionChordDownQuarterIndex + 3, juce::String::fromUTF8("\xE2\x86\x93 2/3"));
    addItemIfAllowed(chordMenu, kDirectionChordDownQuarterIndex + 4, juce::String::fromUTF8("\xE2\x86\x93 3/4"));
    addItemIfAllowed(chordMenu, kDirectionChordDownWholeIndex,      juce::String::fromUTF8("\xE2\x86\x93 1/1"));
    addItemIfAllowed(chordMenu, kDirectionChordRndIndex, "Rnd");

    if (chordMenu.getNumItems() > 0)
        menu.addSubMenu("Chord", chordMenu);

    addItemIfAllowed(menu, kDirectionUpPairIndex);
    addItemIfAllowed(menu, kDirectionDownPairIndex);
    addItemIfAllowed(menu, kDirectionMuteIndex);
    addItemIfAllowed(menu, kDirectionRndIndex);

    auto options = juce::PopupMenu::Options()
        .withTargetComponent(this)
        .withMinimumWidth(juce::jmax(120, getWidth()))
        .withMaximumNumColumns(1)
        .withStandardItemHeight(18);

    menu.showMenuAsync(options, [safe = juce::Component::SafePointer<StepToggle>(this)](int result)
    {
        if (safe == nullptr || result <= 0)
            return;

        safe->setValue(result - 1, true);

        if (safe->onRightClick)
            safe->onRightClick();
    });
}

void StepToggle::showGateChoiceMenu()
{
    // Menu specialise Gate:
    // ordre impose pour la lisibilite musicale:
    // - valeurs temporelles discretes
    // - Tie
    // Le premier pas est deja protege par minChoiceIndex cote grille.
    const auto& choices = getChoices();
    const int maxValue = choices.size() - 1;
    if (maxValue < 0)
        return;

    const int minValue = juce::jlimit(0, maxValue, minChoiceIndex);

    juce::PopupMenu menu;

    if (minValue == 0)
        menu.addItem(1, choices[0], true, value == 0); // Skip

    constexpr int kGateFirstTimedIndex = 1;
    constexpr int kGateLastTimedIndex = 8;
    constexpr int kGateTieIndex = 9;

    const int firstTimed = juce::jmax(kGateFirstTimedIndex, minValue);
    const int lastTimed = juce::jmin(kGateLastTimedIndex, maxValue);
    for (int i = firstTimed; i <= lastTimed; ++i)
        menu.addItem(i + 1, choices[i], true, i == value);

    if (kGateTieIndex >= minValue && kGateTieIndex <= maxValue)
        menu.addItem(kGateTieIndex + 1, choices[kGateTieIndex], true, value == kGateTieIndex);

    auto options = juce::PopupMenu::Options()
        .withTargetComponent(this)
        .withMinimumWidth(juce::jmax(96, getWidth()))
        .withMaximumNumColumns(1)
        .withStandardItemHeight(18);

    menu.showMenuAsync(options, [safe = juce::Component::SafePointer<StepToggle>(this)](int result)
    {
        if (safe == nullptr || result <= 0)
            return;

        safe->setValue(result - 1, true);

        if (safe->onRightClick)
            safe->onRightClick();
    });
}

void StepToggle::showRateChoiceMenu()
{
    // Menu specialise Rate:
    // - Organisation en sous-menus par type rythmique.
    // - Affichage custom note/glyphe pour lecture musicale plus directe.
    // - Le moteur continue de raisonner sur les indices discrets.
    const auto& choices = getChoices();
    const int maxValue = choices.size() - 1;
    if (maxValue < 0)
        return;

    const int minValue = juce::jlimit(0, maxValue, minChoiceIndex);

    juce::PopupMenu menu;

    if (minValue == 0)
        menu.addItem(1, choices[0], true, value == 0); // Skip

    for (int type = 0; type < kRateTypeCount; ++type)
    {
        juce::PopupMenu subMenu;
        const int typeStart = kRateFirstIndex + (type * kRateRatiosPerType);
        const int typeEnd = typeStart + kRateRatiosPerType - 1;

        for (int idx = typeStart; idx <= typeEnd; ++idx)
        {
            if (idx < minValue || idx > maxValue)
                continue;

            const auto visual = makeRateVisualForChoice(idx);
            if (visual.valid)
            {
                subMenu.addCustomItem(idx + 1,
                                      std::make_unique<RateMenuItem>(visual, choices[idx], idx == value),
                                      nullptr,
                                      choices[idx]);
                continue;
            }

            subMenu.addItem(idx + 1, choices[idx], true, idx == value);
        }

        if (subMenu.getNumItems() > 0)
            menu.addSubMenu(kRateTypeLabels[(size_t) type], subMenu);
    }

    if (kRateRndIndex >= minValue && kRateRndIndex <= maxValue)
        menu.addItem(kRateRndIndex + 1, choices[kRateRndIndex], true, value == kRateRndIndex);

    auto options = juce::PopupMenu::Options()
        .withTargetComponent(this)
        .withMinimumWidth(juce::jmax(140, getWidth()))
        .withMaximumNumColumns(1)
        .withStandardItemHeight(18);

    menu.showMenuAsync(options, [safe = juce::Component::SafePointer<StepToggle>(this)](int result)
    {
        if (safe == nullptr || result <= 0)
            return;

        safe->setValue(result - 1, true);

        if (safe->onRightClick)
            safe->onRightClick();
    });
}
