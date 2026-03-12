//==============================================================================
// Arpeggiator.cpp
//------------------------------------------------------------------------------
// Vue d ensemble:
// - Implmente la logique UI du module arpeggiator pour une lane.
// - Fait le pont entre APVTS (etat persistant), interactions utilisateur
//   et visualisation de lecture synchronisee transport.
//
// Flux global:
// 1) Chargement APVTS -> baseLayerValues + etat des controles.
// 2) Morph knobs appliquent une projection d affichage sur les pas.
// 3) Timer UI lit le transport et avance un playhead visuel.
// 4) Edition utilisateur -> ecriture APVTS -> re-projection.
//
// Limite volontaire:
// - Ce fichier ne genere pas le MIDI final temps reel.
// - Le moteur autoritaire est ArpeggiatorProcessor.
//==============================================================================

#include "Arpeggiator.h"

#include "../PluginColours.h"
#include "../PluginParameters.h"
#include "../PluginProcessor.h"
#include "../UiMetrics.h"
#include "../0-component/ModulePresetStore.h"
#include <cmath>
#include <limits>

namespace
{
    //--------------------------------------------------------------------------
    // Convention de couches:
    // - Les indices ci-dessous sont le contrat commun UI <-> params <-> moteur.
    // - Ne pas changer ces valeurs sans mise a jour coordonnee.
    //--------------------------------------------------------------------------
    constexpr int kNumLayers = 8;
    constexpr int kNumSteps = 32;

    constexpr int kLayerRate      = 0;
    constexpr int kLayerGate      = 1;
    constexpr int kLayerGroove    = 2;
    constexpr int kLayerVelocity  = 3;
    constexpr int kLayerDirection = 4;
    constexpr int kLayerPattern   = 5;
    constexpr int kLayerRange     = 6;
    constexpr int kLayerAccent    = 7;
    // Compatibility note:
    // - "Pattern/Range" are historical storage aliases.
    // - UI semantics:
    //   Arp  => Jump / Octave
    //   Drum => Velo Env / Tim Env

    constexpr int kRateTypeCount = 6;
    constexpr int kRateRatiosPerType = 7;
    constexpr int kRateChoiceCount = kRateTypeCount * kRateRatiosPerType; // 42
    constexpr int kRateRndIndex = kRateChoiceCount + 1; // 43
    constexpr int kDirectionTopIndex = 1;
    constexpr int kDirectionUpIndex = 2;
    constexpr int kDirectionDownIndex = 4;
    constexpr int kDirectionBottomIndex = 5;
    constexpr int kDirectionChordAllIndex = 6;
    constexpr int kDirectionUpPairIndex = 7;
    constexpr int kDirectionDownPairIndex = 8;
    constexpr int kDirectionRndIndex = 10;
    constexpr int kDirectionChordRndIndex = 23;
    constexpr int kPatternRndIndex = 9;
    constexpr int kRangeRndIndex = 10;
    constexpr int kAccentRndIndex = 9;
    constexpr int kVelocityChoiceFirst = 1;   // ppp
    constexpr int kVelocityChoiceEqual = 4;   // "=" (fixed neutral, not morphed)
    constexpr int kVelocityChoiceMf = 5;      // "mf" (neutral, morphed)
    constexpr int kVelocityChoiceLast = 8;    // fff
    constexpr std::array<int, 8> kVelocityPercentValues {
        -100, -66, -33, 0, 0, 33, 66, 100
    };
    constexpr int kGateTimedChoiceFirst = 1;
    constexpr int kGateTimedChoiceLast = 8;
    constexpr int kGrooveChoiceFirst = 1;
    constexpr int kGrooveChoiceEqual = 8;
    constexpr int kGrooveChoiceLast = 15;
    constexpr std::array<int, 15> kGroovePercentValues {
        -75, -66, -50, -33, -25, -10, -5, 0, 5, 10, 25, 33, 50, 66, 75
    };
    constexpr int kNumMorphKnobs = 4;
    constexpr int kLinkModeLink = 0;
    constexpr int kLinkModeF = 1;
    constexpr int kLinkModeUnlink = 2;

    static constexpr std::array<const char*, kNumMorphKnobs> kMorphBaseParamIds {
        ParamIDs::arpRateMorph,
        ParamIDs::arpGateMorph,
        ParamIDs::arpGrooveMorph,
        ParamIDs::arpVelocityMorph
    };

    // Mapping fixe des 4 boutons de page vers les couches affichees.
    static const std::array<int, 4> kPageLayers { kLayerDirection, kLayerPattern, kLayerRange, kLayerAccent };

    static const std::array<juce::String, 4> kArpeggiatorPageLabels {
        "Strum", "Jump", "Octave", "Retrig"
    };

    static const std::array<juce::String, 4> kDrumMachinePageLabels {
        "Hit", "Velo Env", "Tim Env", "Retrig"
    };

    static const juce::StringArray kDrumHitChoices {
        "Skip", "X", "O", "Flam", "Drag"
    };

    static const juce::StringArray kDrumEnvChoices {
        "Skip", "0", "lin", "log", "rlin", "rlog", "rnd"
    };

    // Affiche 8 toggles: Rythm global puis les 7 couches restantes.
    static const std::array<int, 8> kVisibleLinkLayers {
        kLayerRate,
        kLayerGate,
        kLayerGroove,
        kLayerVelocity,
        kLayerDirection,
        kLayerPattern,
        kLayerRange,
        kLayerAccent
    };

    static inline int clampChoice(int value, int maxIndex) noexcept
    {
        return juce::jlimit(0, juce::jmax(0, maxIndex), value);
    }

    static inline bool isRateRegularChoice(int choiceIndex) noexcept
    {
        return choiceIndex >= 1 && choiceIndex <= kRateChoiceCount;
    }

    static inline int rateChoiceType(int choiceIndex) noexcept
    {
        return juce::jlimit(0, kRateTypeCount - 1, (choiceIndex - 1) / kRateRatiosPerType);
    }

    static inline int rateChoiceSlot1Based(int choiceIndex) noexcept
    {
        return juce::jlimit(1, kRateRatiosPerType, ((choiceIndex - 1) % kRateRatiosPerType) + 1);
    }

    static inline int makeRateChoiceIndex(int type, int slot1Based) noexcept
    {
        const int safeType = juce::jlimit(0, kRateTypeCount - 1, type);
        const int safeSlot = juce::jlimit(1, kRateRatiosPerType, slot1Based);
        return (safeType * kRateRatiosPerType) + safeSlot;
    }

    static inline bool isVelocityFixedChoice(int choiceIndex) noexcept
    {
        return choiceIndex >= kVelocityChoiceFirst && choiceIndex <= kVelocityChoiceLast;
    }

    static inline int velocityPercentFromChoiceIndex(int choiceIndex) noexcept
    {
        if (!isVelocityFixedChoice(choiceIndex))
            return 0;

        return kVelocityPercentValues[(size_t) (choiceIndex - kVelocityChoiceFirst)];
    }

    static inline int choiceIndexFromVelocityPercent(double percent, bool allowEqual) noexcept
    {
        int bestChoice = allowEqual ? kVelocityChoiceEqual : kVelocityChoiceMf;
        double bestDistance = std::numeric_limits<double>::max();

        for (int choice = kVelocityChoiceFirst; choice <= kVelocityChoiceLast; ++choice)
        {
            if (!allowEqual && choice == kVelocityChoiceEqual)
                continue;

            const double d = std::abs(percent - (double) velocityPercentFromChoiceIndex(choice));
            if (d < bestDistance)
            {
                bestDistance = d;
                bestChoice = choice;
            }
        }

        return bestChoice;
    }

    static inline bool isGateTimedChoice(int choiceIndex) noexcept
    {
        return choiceIndex >= kGateTimedChoiceFirst && choiceIndex <= kGateTimedChoiceLast;
    }

    static inline bool isGrooveFixedChoice(int choiceIndex) noexcept
    {
        return choiceIndex >= kGrooveChoiceFirst && choiceIndex <= kGrooveChoiceLast;
    }

    static inline int groovePercentFromChoice(int choiceIndex) noexcept
    {
        if (!isGrooveFixedChoice(choiceIndex))
            return 0;

        return kGroovePercentValues[(size_t) (choiceIndex - kGrooveChoiceFirst)];
    }

    static inline int grooveChoiceFromPercent(double percent, bool allowEqual) noexcept
    {
        int bestChoice = allowEqual ? kGrooveChoiceEqual : 7; // fallback on -5 when "=" excluded
        double bestDistance = std::numeric_limits<double>::max();

        for (int choice = kGrooveChoiceFirst; choice <= kGrooveChoiceLast; ++choice)
        {
            if (!allowEqual && choice == kGrooveChoiceEqual)
                continue;

            const double d = std::abs(percent - (double) groovePercentFromChoice(choice));
            if (d < bestDistance)
            {
                bestDistance = d;
                bestChoice = choice;
            }
        }

        return bestChoice;
    }

    // Invariant global UX:
    // le premier pas de chaque sequenceur ne peut jamais rester Skip.
    // Cette fonction centralise la valeur par defaut du pas 1 par couche/mode.
    static inline int firstStepDefaultChoiceForLayerInMode(int layer, int modeIndex) noexcept
    {
        switch (layer)
        {
            case kLayerRate:      return 5;    // 1/16
            case kLayerGate:      return 8;    // 99%
            case kLayerGroove:    return 8;    // "="
            case kLayerVelocity:  return 4;    // "="
            case kLayerDirection: return (modeIndex == 0 ? 2 : 1); // Arp: Up / Drum: Hit X
            case kLayerPattern:   return 1;    // Arp: Jump 1 / Drum: Velo Env 0
            case kLayerRange:     return (modeIndex == 0 ? 5 : 1); // Arp: Octave 0 / Drum: Tim Env 0
            case kLayerAccent:    return 1;    // Retrig x1
            default:              return 1;
        }
    }

    struct ScopedApplyingFromState
    {
        explicit ScopedApplyingFromState(std::atomic<bool>& flagIn) : flag(flagIn)
        {
            flag.store(true);
        }

        ~ScopedApplyingFromState()
        {
            flag.store(false);
        }

        std::atomic<bool>& flag;
    };

}

Arpeggiator::PageDialButton::PageDialButton()
{
    titleLabel.setJustificationType(juce::Justification::centred);
    titleLabel.setInterceptsMouseClicks(false, false);
    titleLabel.setFont(juce::Font(juce::FontOptions(12.0f)));
    titleLabel.setColour(juce::Label::textColourId, PluginColours::onSurface);
    addAndMakeVisible(titleLabel);

    setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

void Arpeggiator::PageDialButton::setText(const juce::String& text)
{
    if (titleLabel.getText() == text)
        return;

    titleLabel.setText(text, juce::dontSendNotification);
    repaint();
}

void Arpeggiator::PageDialButton::setLayerIndex(int index)
{
    if (layerIndex == index)
        return;

    layerIndex = juce::jlimit(0, kNumLayers - 1, index);
    repaint();
}

void Arpeggiator::PageDialButton::setSelected(bool shouldBeSelected)
{
    if (selected == shouldBeSelected)
        return;

    selected = shouldBeSelected;
    repaint();
}

void Arpeggiator::PageDialButton::resized()
{
    auto area = getLocalBounds();

    const int labelHeight = 20;
    const int labelToDialSpace = 4;
    const int diameter = juce::jmin(area.getWidth(), area.getHeight() - labelHeight - labelToDialSpace);

    dialBounds = juce::Rectangle<int>(
        area.getCentreX() - diameter / 2,
        labelHeight + labelToDialSpace,
        diameter,
        diameter);

    titleLabel.setBounds(area.getCentreX() - 50, 0, 100, labelHeight);
}

void Arpeggiator::PageDialButton::paint(juce::Graphics& g)
{
    auto boundsF = dialBounds.toFloat();
    if (boundsF.isEmpty())
        return;

    const auto colours = PluginColours::getLayerColours(layerIndex);
    g.setColour(selected ? colours.background : PluginColours::onPrimary);
    g.fillEllipse(boundsF);

    g.setColour(selected ? colours.text : colours.background);
    g.drawEllipse(boundsF.reduced(1.2f), selected ? 2.4f : 1.6f);
}

void Arpeggiator::PageDialButton::mouseDown(const juce::MouseEvent& e)
{
    if (e.mods.isLeftButtonDown() && onClick)
        onClick();
}

void Arpeggiator::PageDialButton::mouseEnter(const juce::MouseEvent&)
{
    hovered = true;
    repaint();
}

void Arpeggiator::PageDialButton::mouseExit(const juce::MouseEvent&)
{
    hovered = false;
    repaint();
}

const std::array<juce::String, 8>& Arpeggiator::arpSeqPrefixes()
{
    static const std::array<juce::String, 8> prefixes {
        "arpRateSeq",
        "arpGateSeq",
        "arpGrooveSeq",
        "arpVelocitySeq",
        "arpDirectionSeq",
        "arpPatternSeq",
        "arpRangeSeq",
        "arpAccentSeq",
    };
    return prefixes;
}

const std::array<juce::String, 8>& Arpeggiator::drumSeqPrefixes()
{
    static const std::array<juce::String, 8> prefixes {
        "arpRateSeq",
        "arpGateSeq",
        "arpGrooveSeq",
        "arpVelocitySeq",
        "drumGraceSeq",
        "drumVeloEnvSeq",
        "drumTimEnvSeq",
        "arpAccentSeq",
    };
    return prefixes;
}

const juce::String& Arpeggiator::seqPrefixForLayerAndMode(int layer, int modeIndex)
{
    const int safeLayer = juce::jlimit(0, kNumLayers - 1, layer);
    const bool drumMode = (modeIndex == 1);
    return (drumMode ? drumSeqPrefixes() : arpSeqPrefixes())[(size_t) safeLayer];
}

const std::array<juce::String, 8>& Arpeggiator::linkBaseParamIDs()
{
    static const std::array<juce::String, 8> linkIDs {
        ParamIDs::arpRateLink,
        ParamIDs::arpGateLink,
        ParamIDs::arpGrooveLink,
        ParamIDs::arpVelocityLink,
        ParamIDs::arpDirectionLink,
        ParamIDs::arpPatternLink,
        ParamIDs::arpRangeLink,
        ParamIDs::arpAccentLink,
    };
    return linkIDs;
}

const std::array<juce::String, 8>& Arpeggiator::unlinkRateBaseParamIDs()
{
    static const std::array<juce::String, 8> unlinkRateIDs {
        "", // Rate layer has no autonomous-rate parameter.
        ParamIDs::arpGateUnlinkRate,
        ParamIDs::arpGrooveUnlinkRate,
        ParamIDs::arpVelocityUnlinkRate,
        ParamIDs::arpDirectionUnlinkRate,
        ParamIDs::arpPatternUnlinkRate,
        ParamIDs::arpRangeUnlinkRate,
        ParamIDs::arpAccentUnlinkRate
    };
    return unlinkRateIDs;
}

juce::String Arpeggiator::idForLane(const juce::String& baseID) const
{
    return ParamIDs::lane(baseID, lane);
}

int Arpeggiator::readLinkModeForLayer(int layer) const
{
    const int safeLayer = juce::jlimit(0, kNumLayers - 1, layer);

    if (auto* raw = layerLinkRawPointers[(size_t) safeLayer])
    {
        const int idx = juce::roundToInt(raw->load());
        return (safeLayer == kLayerRate)
            ? juce::jlimit(kLinkModeLink, kLinkModeF, idx)
            : juce::jlimit(kLinkModeLink, kLinkModeUnlink, idx);
    }

    if (const auto pid = idForLane(linkBaseParamIDs()[(size_t) safeLayer]);
        auto* p = dynamic_cast<juce::AudioParameterChoice*>(parameters.getParameter(pid)))
    {
        return (safeLayer == kLayerRate)
            ? juce::jlimit(kLinkModeLink, kLinkModeF, p->getIndex())
            : juce::jlimit(kLinkModeLink, kLinkModeUnlink, p->getIndex());
    }

    return kLinkModeF;
}

int Arpeggiator::readUnlinkRateChoiceForLayer(int layer) const
{
    const int safeLayer = juce::jlimit(0, kNumLayers - 1, layer);
    if (safeLayer == kLayerRate)
        return 5;

    int idx = 5;
    if (auto* raw = layerUnlinkRateRawPointers[(size_t) safeLayer])
        idx = juce::roundToInt(raw->load());
    else if (const auto& base = unlinkRateBaseParamIDs()[(size_t) safeLayer];
             base.isNotEmpty())
        if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(parameters.getParameter(idForLane(base))))
            idx = p->getIndex();

    // Skip is not valid for autonomous unlink clocks.
    if (idx <= 0)
        idx = 5;

    return juce::jlimit(1, kRateRndIndex, idx);
}

juce::AudioParameterChoice*& Arpeggiator::stepChoicePointerForLayerStepMode(int layer,
                                                                             int step0Based,
                                                                             int modeIndex) noexcept
{
    const int safeLayer = juce::jlimit(0, kNumLayers - 1, layer);
    const int safeStep = juce::jlimit(0, kNumSteps - 1, step0Based);
    const bool useDrumStore = (modeIndex == 1)
                           && (safeLayer == kLayerDirection
                               || safeLayer == kLayerPattern
                               || safeLayer == kLayerRange);

    return useDrumStore
        ? stepChoiceParamPointersDrum[(size_t) safeLayer][(size_t) safeStep]
        : stepChoiceParamPointersArp[(size_t) safeLayer][(size_t) safeStep];
}

juce::AudioParameterChoice* Arpeggiator::getStepChoiceParameterForLayerStepMode(int layer,
                                                                                 int step0Based,
                                                                                 int modeIndex)
{
    auto*& ptr = stepChoicePointerForLayerStepMode(layer, step0Based, modeIndex);
    if (ptr != nullptr)
        return ptr;

    const auto stepParamId = idForLane(makeSeqParamId(seqPrefixForLayerAndMode(layer, modeIndex),
                                                       step0Based + 1));
    ptr = dynamic_cast<juce::AudioParameterChoice*>(parameters.getParameter(stepParamId));
    return ptr;
}

juce::String Arpeggiator::unlinkRateLabelForChoice(int choiceIndex)
{
    const auto& rateChoices = arpStepLayerChoices[0];
    if (choiceIndex < 0 || choiceIndex >= rateChoices.size())
        return "1/16";

    return rateChoices[choiceIndex];
}

void Arpeggiator::openUnlinkRateMenuForLayer(int layer, juce::Component& target)
{
    const int safeLayer = juce::jlimit(0, kNumLayers - 1, layer);
    if (safeLayer == kLayerRate)
        return;

    if (readLinkModeForLayer(safeLayer) != kLinkModeUnlink)
        return;

    const auto& baseId = unlinkRateBaseParamIDs()[(size_t) safeLayer];
    if (baseId.isEmpty())
        return;

    auto* choiceParam = dynamic_cast<juce::AudioParameterChoice*>(parameters.getParameter(idForLane(baseId)));
    if (choiceParam == nullptr)
        return;

    const auto& rateChoices = arpStepLayerChoices[0];
    const int currentChoice = juce::jlimit(1, kRateRndIndex, choiceParam->getIndex());
    constexpr std::array<const char*, 6> kRateTypeLabels {
        "Binaries (1:1)",
        "Dotted (3:2)",
        "Double dotted (7:4)",
        "Triolet (3:2)",
        "Quintolet (5:4)",
        "Septolet (7:4)"
    };

    juce::PopupMenu menu;

    for (int type = 0; type < kRateTypeCount; ++type)
    {
        juce::PopupMenu subMenu;
        const int typeStart = 1 + type * kRateRatiosPerType;
        const int typeEnd = typeStart + kRateRatiosPerType - 1;

        for (int idx = typeStart; idx <= typeEnd; ++idx)
        {
            if (!juce::isPositiveAndBelow(idx, rateChoices.size()))
                continue;
            subMenu.addItem(idx + 1, rateChoices[idx], true, idx == currentChoice);
        }

        menu.addSubMenu(kRateTypeLabels[(size_t) type], subMenu);
    }

    if (juce::isPositiveAndBelow(kRateRndIndex, rateChoices.size()))
        menu.addItem(kRateRndIndex + 1, rateChoices[kRateRndIndex], true, currentChoice == kRateRndIndex);

    auto options = juce::PopupMenu::Options()
        .withTargetComponent(&target)
        .withMinimumWidth(juce::jmax(140, target.getWidth()))
        .withMaximumNumColumns(1)
        .withStandardItemHeight(18);

    menu.showMenuAsync(options,
                       [safe = juce::Component::SafePointer<Arpeggiator>(this),
                        layer = safeLayer,
                        choiceParam](int result)
    {
        if (safe == nullptr || result <= 0 || choiceParam == nullptr)
            return;

        const int selectedChoice = juce::jlimit(1, kRateRndIndex, result - 1);
        const int current = juce::jlimit(1, kRateRndIndex, choiceParam->getIndex());
        if (current == selectedChoice)
            return;

        choiceParam->beginChangeGesture();
        choiceParam->setValueNotifyingHost(choiceParam->convertTo0to1((float) selectedChoice));
        choiceParam->endChangeGesture();

        if (safe->layerLinkToggles[(size_t) layer] != nullptr)
            safe->layerLinkToggles[(size_t) layer]->setUnlinkRateLabel(unlinkRateLabelForChoice(selectedChoice));
    });
}

void Arpeggiator::refreshLinkToggleRateLabels()
{
    for (int layer = 0; layer < (int) layerLinkToggles.size(); ++layer)
    {
        if (layerLinkToggles[(size_t) layer] == nullptr)
            continue;

        layerLinkToggles[(size_t) layer]->setModeIndex(readLinkModeForLayer(layer), juce::dontSendNotification);
        layerLinkToggles[(size_t) layer]->setUnlinkRateLabel(
            unlinkRateLabelForChoice(readUnlinkRateChoiceForLayer(layer)));
    }
}

void Arpeggiator::refreshParameterPointerCache()
{
    // Cache APVTS:
    // - reduit les recherches string + dynamic_cast dans les chemins frequents UI
    //   (drag morph, edition step, refresh async).
    // - ce cache est "best effort": chaque site d utilisation garde un fallback
    //   lookup si un pointeur n est pas resolu.
    const auto enableParamId = idForLane(ParamIDs::arpeggiatorEnable);
    arpeggiatorEnableRaw = parameters.getRawParameterValue(enableParamId);
    arpeggiatorEnableParam = dynamic_cast<juce::AudioParameterBool*>(parameters.getParameter(enableParamId));
    const auto modeParamId = idForLane(ParamIDs::arpMode);
    arpeggiatorModeRaw = parameters.getRawParameterValue(modeParamId);
    arpeggiatorModeParam = dynamic_cast<juce::AudioParameterChoice*>(parameters.getParameter(modeParamId));

    for (int i = 0; i < kNumMorphKnobs; ++i)
        morphParamPointers[(size_t) i] = parameters.getParameter(idForLane(kMorphBaseParamIds[(size_t) i]));

    const auto& linkIDs = linkBaseParamIDs();
    const auto& unlinkRateIDs = unlinkRateBaseParamIDs();
    for (int i = 0; i < (int) layerLinkRawPointers.size(); ++i)
    {
        layerLinkRawPointers[(size_t) i] = parameters.getRawParameterValue(idForLane(linkIDs[(size_t) i]));
        if (unlinkRateIDs[(size_t) i].isNotEmpty())
            layerUnlinkRateRawPointers[(size_t) i] = parameters.getRawParameterValue(idForLane(unlinkRateIDs[(size_t) i]));
        else
            layerUnlinkRateRawPointers[(size_t) i] = nullptr;
    }

    for (int layer = 0; layer < kNumLayers; ++layer)
    {
        for (int step = 1; step <= kNumSteps; ++step)
        {
            const auto arpStepParamId = idForLane(makeSeqParamId(arpSeqPrefixes()[(size_t) layer], step));
            stepChoiceParamPointersArp[(size_t) layer][(size_t) (step - 1)] =
                dynamic_cast<juce::AudioParameterChoice*>(parameters.getParameter(arpStepParamId));

            // Drum pages are independent only for Direction/Pattern/Range
            // (aka Hit/Velo Env/Tim Env in Drum mode).
            // Shared layers intentionally alias the Arp storage.
            if (layer == kLayerDirection || layer == kLayerPattern || layer == kLayerRange)
            {
                const auto drumStepParamId = idForLane(makeSeqParamId(drumSeqPrefixes()[(size_t) layer], step));
                stepChoiceParamPointersDrum[(size_t) layer][(size_t) (step - 1)] =
                    dynamic_cast<juce::AudioParameterChoice*>(parameters.getParameter(drumStepParamId));
            }
            else
            {
                stepChoiceParamPointersDrum[(size_t) layer][(size_t) (step - 1)] =
                    stepChoiceParamPointersArp[(size_t) layer][(size_t) (step - 1)];
            }
        }
    }
}

juce::String Arpeggiator::makeSeqParamId(const juce::String& prefix, int step1Based)
{
    return prefix + juce::String(step1Based).paddedLeft('0', 2);
}

juce::ValueTree Arpeggiator::captureModulePresetState() const
{
    std::vector<juce::String> paramIds;
    paramIds.reserve(300);

    const auto addBaseId = [this, &paramIds](const juce::String& baseId)
    {
        if (baseId.isNotEmpty())
            paramIds.push_back(idForLane(baseId));
    };

    addBaseId(ParamIDs::Base::arpeggiatorEnable);
    addBaseId(ParamIDs::Base::arpMode);
    addBaseId(ParamIDs::Base::arpRateMorph);
    addBaseId(ParamIDs::Base::arpGateMorph);
    addBaseId(ParamIDs::Base::arpGrooveMorph);
    addBaseId(ParamIDs::Base::arpVelocityMorph);
    addBaseId(ParamIDs::Base::arpDirectionMorph);
    addBaseId(ParamIDs::Base::arpPatternMorph);
    addBaseId(ParamIDs::Base::arpRangeMorph);
    addBaseId(ParamIDs::Base::arpAccentMorph);

    for (const auto& linkBase : linkBaseParamIDs())
        addBaseId(linkBase);
    for (const auto& unlinkBase : unlinkRateBaseParamIDs())
        addBaseId(unlinkBase);

    const auto addStepsForPrefix = [this, &paramIds](const juce::String& prefix)
    {
        for (int step = 1; step <= kNumSteps; ++step)
            paramIds.push_back(idForLane(makeSeqParamId(prefix, step)));
    };

    for (const auto& prefix : arpSeqPrefixes())
        addStepsForPrefix(prefix);
    for (const auto& prefix : drumSeqPrefixes())
        addStepsForPrefix(prefix);

    return ModulePresetStore::captureParameterState(parameters, paramIds);
}

void Arpeggiator::applyModulePresetState(const juce::ValueTree& state)
{
    if (!state.isValid())
        return;

    ModulePresetStore::applyParameterState(parameters, state);
    if (!isUpdatePending())
        triggerAsyncUpdate();
}

void Arpeggiator::showSaveModulePresetDialog()
{
    auto* dialog = new juce::AlertWindow("Save As Arpeggiator Preset",
                                         "Enter preset name.",
                                         juce::AlertWindow::NoIcon);
    dialog->addTextEditor("name", "Preset", "Name:");
    dialog->addButton("Save As", 1, juce::KeyPress(juce::KeyPress::returnKey));
    dialog->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    auto dialogSafe = juce::Component::SafePointer<juce::AlertWindow>(dialog);
    auto compSafe = juce::Component::SafePointer<Arpeggiator>(this);

    dialog->enterModalState(true,
                            juce::ModalCallbackFunction::create(
                                [dialogSafe, compSafe](int result)
                                {
                                    if (result != 1 || dialogSafe == nullptr || compSafe == nullptr)
                                        return;

                                    const auto* textEditor = dialogSafe->getTextEditor("name");
                                    const auto presetName = (textEditor != nullptr)
                                                                ? textEditor->getText().trim()
                                                                : juce::String();
                                    if (presetName.isEmpty())
                                        return;

                                    juce::String error;
                                    if (!ModulePresetStore::savePreset("arpeggiator",
                                                                       presetName,
                                                                       compSafe->captureModulePresetState(),
                                                                       &error))
                                    {
                                        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                                               "Save Arpeggiator Preset",
                                                                               error);
                                    }
                                }),
                            true);
}

bool Arpeggiator::saveLoadedModulePreset()
{
    if (!loadedModulePresetFile.existsAsFile())
        return false;

    juce::String error;
    if (!ModulePresetStore::savePresetInPlace(loadedModulePresetFile,
                                              "arpeggiator",
                                              captureModulePresetState(),
                                              &error))
    {
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                               "Save Arpeggiator Preset",
                                               error);
        return false;
    }

    return true;
}

void Arpeggiator::showEditLoadedPresetDialog()
{
    if (!loadedModulePresetFile.existsAsFile())
        return;

    juce::ValueTree payload;
    juce::String presetName;
    juce::String moduleKey;
    juce::String error;
    if (!ModulePresetStore::loadPresetPayload(loadedModulePresetFile,
                                              payload,
                                              &presetName,
                                              &moduleKey,
                                              &error))
    {
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                               "Edit Arpeggiator Preset",
                                               error);
        return;
    }

    auto* dialog = new juce::AlertWindow("Edit Arpeggiator Preset",
                                         "Rename or delete this preset.",
                                         juce::AlertWindow::NoIcon);
    dialog->addTextEditor("name",
                          presetName.trim().isNotEmpty() ? presetName.trim()
                                                          : loadedModulePresetFile.getFileNameWithoutExtension(),
                          "Name:");
    dialog->addButton("Rename", 1, juce::KeyPress(juce::KeyPress::returnKey));
    dialog->addButton("Delete", 2);
    dialog->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    auto dialogSafe = juce::Component::SafePointer<juce::AlertWindow>(dialog);
    auto compSafe = juce::Component::SafePointer<Arpeggiator>(this);
    dialog->enterModalState(true,
                            juce::ModalCallbackFunction::create(
                                [dialogSafe, compSafe](int result)
                                {
                                    if (result == 0 || dialogSafe == nullptr || compSafe == nullptr)
                                        return;

                                    if (result == 1)
                                    {
                                        const auto* te = dialogSafe->getTextEditor("name");
                                        const auto newName = te != nullptr ? te->getText().trim() : juce::String();
                                        if (newName.isEmpty())
                                            return;

                                        juce::File renamedFile;
                                        juce::String error;
                                        if (!ModulePresetStore::renamePresetFile(compSafe->loadedModulePresetFile,
                                                                                 "arpeggiator",
                                                                                 newName,
                                                                                 &renamedFile,
                                                                                 &error))
                                        {
                                            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                                                   "Edit Arpeggiator Preset",
                                                                                   error);
                                            return;
                                        }

                                        compSafe->loadedModulePresetFile = renamedFile;
                                        compSafe->loadedModulePresetName = newName;
                                        return;
                                    }

                                    if (result == 2)
                                    {
                                        juce::String error;
                                        if (!ModulePresetStore::deletePresetFile(compSafe->loadedModulePresetFile,
                                                                                 "arpeggiator",
                                                                                 &error))
                                        {
                                            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                                                   "Edit Arpeggiator Preset",
                                                                                   error);
                                            return;
                                        }

                                        compSafe->loadedModulePresetFile = juce::File();
                                        compSafe->loadedModulePresetName.clear();
                                    }
                                }),
                            true);
}

void Arpeggiator::showModulePresetMenu()
{
    constexpr int kSaveId = 1;
    constexpr int kSaveAsId = 2;
    constexpr int kEditId = 3;
    constexpr int kLoadBaseId = 1000;

    juce::PopupMenu menu;
    const bool canSaveLoadedPreset = loadedModulePresetFile.existsAsFile();
    menu.addItem(kSaveId, "Save", canSaveLoadedPreset, false);
    menu.addItem(kSaveAsId, "Save As Preset...");
    menu.addItem(kEditId, "Edit Preset...", canSaveLoadedPreset, false);

    juce::PopupMenu loadMenu;
    const auto entries = ModulePresetStore::listPresets("arpeggiator");
    if (entries.empty())
    {
        loadMenu.addItem(999999, "(No presets)", false, false);
    }
    else
    {
        for (int i = 0; i < (int) entries.size(); ++i)
            loadMenu.addItem(kLoadBaseId + i, entries[(size_t) i].displayName);
    }
    menu.addSubMenu("Load Preset", loadMenu);

    auto compSafe = juce::Component::SafePointer<Arpeggiator>(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&arpeggiatorTitleButton),
                       [compSafe, entries, kSaveId, kSaveAsId, kEditId, kLoadBaseId](int result)
                       {
                           if (compSafe == nullptr || result == 0)
                               return;

                           if (result == kSaveId)
                           {
                               compSafe->saveLoadedModulePreset();
                               return;
                           }

                           if (result == kSaveAsId)
                           {
                               compSafe->showSaveModulePresetDialog();
                               return;
                           }

                           if (result == kEditId)
                           {
                               compSafe->showEditLoadedPresetDialog();
                               return;
                           }

                           if (result < kLoadBaseId || result >= kLoadBaseId + (int) entries.size())
                               return;

                           const auto& entry = entries[(size_t) (result - kLoadBaseId)];
                           juce::ValueTree payload;
                           juce::String error;
                           if (!ModulePresetStore::loadPresetPayload(entry.file, payload, nullptr, nullptr, &error))
                           {
                               juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                                      "Load Arpeggiator Preset",
                                                                      error);
                               return;
                           }

                           compSafe->applyModulePresetState(payload);
                           compSafe->loadedModulePresetFile = entry.file;
                           compSafe->loadedModulePresetName = entry.displayName;
                       });
}

Arpeggiator::Arpeggiator(juce::AudioProcessorValueTreeState& vts,
                         MidivisiViciAudioProcessor& processorIn,
                         Lane laneIn)
    : parameters(vts)
    , processor(processorIn)
    , lane(laneIn)
{
    // 1) Initialisation des pointeurs APVTS et listeners.
    refreshParameterPointerCache();
    registerParameterListeners();

    arpeggiatorTitleButton.setClickingTogglesState(true);
    arpeggiatorTitleButton.onClick = [this]
    {
        if (applyingFromState.load())
            return;

        if (auto* p = arpeggiatorEnableParam)
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost(arpeggiatorTitleButton.getToggleState() ? 1.0f : 0.0f);
            p->endChangeGesture();
        }

        if (!isUpdatePending())
            triggerAsyncUpdate();
    };
    arpeggiatorTitleButton.onPopupClick = [this](const juce::MouseEvent&)
    {
        showModulePresetMenu();
    };

    addWithShadow(arpeggiatorTitleButtonShadow,
                  arpeggiatorTitleButton,
                  juce::DropShadow(juce::Colours::black, 0, { 0, 0 }));

    arpeggiatorModeToggle.onModeChange = [this](SplitterModeToggle::Mode mode)
    {
        if (applyingFromState.load())
            return;

        // Reuse splitter 2-state toggle semantics:
        // RoundRobin => Arpeggiator, RangeSplit => Drum Machine.
        if (auto* p = arpeggiatorModeParam)
        {
            const int modeIndex = (mode == SplitterModeToggle::RoundRobin ? 0 : 1);
            p->beginChangeGesture();
            p->setValueNotifyingHost(p->convertTo0to1((float) modeIndex));
            p->endChangeGesture();
        }

        if (!isUpdatePending())
            triggerAsyncUpdate();
    };
    addAndMakeVisible(arpeggiatorModeToggle);

    // 2) Construction des controls "morph": ils modifient la projection
    // d affichage mais la base reste dans baseLayerValues/APVTS.
    auto makeModifierKnob = [this](std::unique_ptr<CustomRotaryNoCombo>& dst,
                                   const juce::String& title,
                                   const juce::String& baseParamID,
                                   int layerIndex,
                                   CustomRotaryWithCombo::ArcMode arcMode)
    {
        dst = std::make_unique<CustomRotaryNoCombo>(title, -100, 100, 0, true, arcMode);
        dst->setUseLayerColours(true);
        dst->setLayerIndex(layerIndex);

        dst->onValueChange = [this, layerIndex, baseParamID](int newVal)
        {
            setActiveLayer(layerIndex);

            if (applyingFromState.load())
                return;

            juce::RangedAudioParameter* parameter = nullptr;
            if (juce::isPositiveAndBelow(layerIndex, kNumMorphKnobs))
                parameter = morphParamPointers[(size_t) layerIndex];
            if (parameter == nullptr)
                parameter = parameters.getParameter(idForLane(baseParamID));

            if (parameter != nullptr)
            {
                const float real = (float) juce::jlimit(-100, 100, newVal);
                const int currentReal = juce::roundToInt(parameter->convertFrom0to1(parameter->getValue()));
                if (currentReal != (int) real)
                {
                    parameter->beginChangeGesture();
                    parameter->setValueNotifyingHost(parameter->convertTo0to1(real));
                    parameter->endChangeGesture();
                }
            }

            // Morph edit hot path:
            // only the edited layer needs immediate reprojection.
            // This keeps UI fluid while dragging knobs at high rate.
            refreshSingleLayerDisplayFromBase(layerIndex);
        };

        dst->onMouseDownLayerChange = [this](int layer)
        {
            setActiveLayer(layer);
        };

        addAndMakeVisible(*dst);
    };

    makeModifierKnob(rateKnob,     "Rythm",    ParamIDs::arpRateMorph,     kLayerRate,     CustomRotaryWithCombo::ArcMode::CenterToCursor);
    makeModifierKnob(gateKnob,     "Gate",     ParamIDs::arpGateMorph,     kLayerGate,     CustomRotaryWithCombo::ArcMode::CenterToCursor);
    makeModifierKnob(grooveKnob,   "Groove",   ParamIDs::arpGrooveMorph,   kLayerGroove,   CustomRotaryWithCombo::ArcMode::CenterToCursor);
    makeModifierKnob(velocityKnob, "Velocity", ParamIDs::arpVelocityMorph, kLayerVelocity, CustomRotaryWithCombo::ArcMode::CenterToCursor);

    for (int i = 0; i < (int) pageButtons.size(); ++i)
    {
        auto button = std::make_unique<PageDialButton>();

        button->onClick = [this, i]
        {
            setActiveLayer(activePageLayerForSlot(i));
        };

        pageButtons[(size_t) i] = std::move(button);
        addAndMakeVisible(*pageButtons[(size_t) i]);
    }

    sequencer = std::make_unique<StepSequencerGrid>(4, 8);
    sequencer->setActiveLayer(activeLayer);
    applyModeLayerChoiceLabels(0);

    sequencer->onStepChanged = [this](int stepIndex0Based, int displayedChoice)
    {
        if (applyingFromState.load())
            return;

        if (!juce::isPositiveAndBelow(activeLayer, kNumLayers) ||
            !juce::isPositiveAndBelow(stepIndex0Based, kNumSteps))
            return;

        const int modeIndex = (arpeggiatorModeRaw != nullptr)
            ? juce::jlimit(0, 1, juce::roundToInt(arpeggiatorModeRaw->load()))
            : 0;

        int baseChoice = removeModifierFromStepChoice(activeLayer, displayedChoice);
        // Hard guard: le pas 1 ne peut jamais redevenir Skip, meme si la
        // projection inverse du morph retourne 0.
        if (stepIndex0Based == 0 && baseChoice == 0)
            baseChoice = firstStepDefaultChoiceForLayerInMode(activeLayer, modeIndex);
        baseLayerValues[(size_t) activeLayer][(size_t) stepIndex0Based] = baseChoice;
        auto* choiceParam = getStepChoiceParameterForLayerStepMode(activeLayer, stepIndex0Based, modeIndex);

        if (choiceParam != nullptr)
        {
            const int clamped = clampChoice(baseChoice, (int) choiceParam->choices.size() - 1);

            choiceParam->beginChangeGesture();
            choiceParam->setValueNotifyingHost(choiceParam->convertTo0to1((float) clamped));
            choiceParam->endChangeGesture();
        }

        // Toujours re-projeter la valeur visible avec morph applique.
        if (sequencer)
            sequencer->setLayerStepValue(activeLayer,
                                         stepIndex0Based,
                                         applyModifierToStepChoice(activeLayer, baseChoice));

        if (activeLayer == kLayerRate)
            updateRateLengthOverlay();
    };

    addAndMakeVisible(*sequencer);

    for (int i = 0; i < (int) layerLinkToggles.size(); ++i)
    {
        auto t = std::make_unique<LinkToggle>(i);
        if (i == kLayerRate)
            t->setBinaryFRMode(true);

        t->onModeChange = [this, i](int newMode)
        {
            if (applyingFromState.load())
                return;

            const auto pid = idForLane(linkBaseParamIDs()[(size_t) i]);
            if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(parameters.getParameter(pid)))
            {
                const int clampedMode = (i == kLayerRate)
                    ? juce::jlimit(kLinkModeLink, kLinkModeF, newMode)
                    : juce::jlimit(kLinkModeLink, kLinkModeUnlink, newMode);
                p->beginChangeGesture();
                p->setValueNotifyingHost(p->convertTo0to1((float) clampedMode));
                p->endChangeGesture();
            }
        };

        t->onRequestRateMenu = [this, i]()
        {
            if (applyingFromState.load())
                return;
            openUnlinkRateMenuForLayer(i, *layerLinkToggles[(size_t) i]);
        };

        layerLinkToggles[(size_t) i] = std::move(t);
        addAndMakeVisible(*layerLinkToggles[(size_t) i]);
    }

    refreshUIFromParameters();
}

Arpeggiator::~Arpeggiator()
{
    cancelPendingUpdate();
    unregisterParameterListeners();
}

void Arpeggiator::registerParameterListeners()
{
    // Important:
    // - Ecouter uniquement les params relies a CE module lane.
    // - Toute notif est reroutee vers AsyncUpdater avant acces UI.
    parameters.addParameterListener(idForLane(ParamIDs::arpeggiatorEnable), this);
    parameters.addParameterListener(idForLane(ParamIDs::arpMode), this);

    // Row 1 modifiers only.
    parameters.addParameterListener(idForLane(ParamIDs::arpRateMorph), this);
    parameters.addParameterListener(idForLane(ParamIDs::arpGateMorph), this);
    parameters.addParameterListener(idForLane(ParamIDs::arpGrooveMorph), this);
    parameters.addParameterListener(idForLane(ParamIDs::arpVelocityMorph), this);

    for (const auto& baseID : linkBaseParamIDs())
        parameters.addParameterListener(idForLane(baseID), this);
    for (const auto& baseID : unlinkRateBaseParamIDs())
        if (baseID.isNotEmpty())
            parameters.addParameterListener(idForLane(baseID), this);

    for (int layer = 0; layer < kNumLayers; ++layer)
    {
        for (int step = 1; step <= kNumSteps; ++step)
            parameters.addParameterListener(idForLane(makeSeqParamId(arpSeqPrefixes()[(size_t) layer], step)), this);
    }

    // Independent Drum pages.
    for (int layer : { kLayerDirection, kLayerPattern, kLayerRange })
    {
        for (int step = 1; step <= kNumSteps; ++step)
            parameters.addParameterListener(idForLane(makeSeqParamId(drumSeqPrefixes()[(size_t) layer], step)), this);
    }
}

void Arpeggiator::unregisterParameterListeners()
{
    parameters.removeParameterListener(idForLane(ParamIDs::arpeggiatorEnable), this);
    parameters.removeParameterListener(idForLane(ParamIDs::arpMode), this);

    parameters.removeParameterListener(idForLane(ParamIDs::arpRateMorph), this);
    parameters.removeParameterListener(idForLane(ParamIDs::arpGateMorph), this);
    parameters.removeParameterListener(idForLane(ParamIDs::arpGrooveMorph), this);
    parameters.removeParameterListener(idForLane(ParamIDs::arpVelocityMorph), this);

    for (const auto& baseID : linkBaseParamIDs())
        parameters.removeParameterListener(idForLane(baseID), this);
    for (const auto& baseID : unlinkRateBaseParamIDs())
        if (baseID.isNotEmpty())
            parameters.removeParameterListener(idForLane(baseID), this);

    for (int layer = 0; layer < kNumLayers; ++layer)
    {
        for (int step = 1; step <= kNumSteps; ++step)
            parameters.removeParameterListener(idForLane(makeSeqParamId(arpSeqPrefixes()[(size_t) layer], step)), this);
    }

    for (int layer : { kLayerDirection, kLayerPattern, kLayerRange })
    {
        for (int step = 1; step <= kNumSteps; ++step)
            parameters.removeParameterListener(idForLane(makeSeqParamId(drumSeqPrefixes()[(size_t) layer], step)), this);
    }
}

void Arpeggiator::parameterChanged(const juce::String&, float)
{
    // Coalescing explicite pour eviter de saturer la queue message-thread
    // quand plusieurs params bougent a haute frequence.
    if (!isUpdatePending())
        triggerAsyncUpdate();
}

void Arpeggiator::handleAsyncUpdate()
{
    refreshUIFromParameters();
}

void Arpeggiator::forceRefreshFromParameters()
{
    if (!isUpdatePending())
        triggerAsyncUpdate();
}

void Arpeggiator::applyGlobalUiBypass(bool bypassed)
{
    // Le bypass du module desactive les enfants mais laisse le titre actif
    // pour pouvoir re-activer le module facilement.
    if (arpeggiatorTitleButtonShadow) arpeggiatorTitleButtonShadow->setEnabled(true);
    arpeggiatorTitleButton.setEnabled(true);

    const bool enableChildren = !bypassed;
    arpeggiatorModeToggle.setEnabled(enableChildren);

    if (rateKnob)     rateKnob->setEnabled(enableChildren);
    if (gateKnob)     gateKnob->setEnabled(enableChildren);
    if (grooveKnob)   grooveKnob->setEnabled(enableChildren);
    if (velocityKnob) velocityKnob->setEnabled(enableChildren);

    for (auto& b : pageButtons)
        if (b) b->setEnabled(enableChildren);

    if (sequencer) sequencer->setEnabled(enableChildren);

    for (auto& t : layerLinkToggles)
        if (t) t->setEnabled(enableChildren);

    const float targetAlpha = bypassed ? 0.55f : 1.0f;
    if (getAlpha() != targetAlpha)
        setAlpha(targetAlpha);
}

void Arpeggiator::applyModeLayerChoiceLabels(int modeIndex)
{
    if (sequencer == nullptr)
        return;

    // Couches partagees Arpeggiator/Drum Machine:
    // - meme table de choices
    // - meme contenu de pas (meme parametres APVTS)
    // => un switch de mode ne doit ni reinitialiser ni remapper ces pages.
    sequencer->setLayerChoiceLabels(kLayerRate,      &arpStepLayerChoices[0]); // Rythm
    sequencer->setLayerChoiceLabels(kLayerGate,      &arpStepLayerChoices[6]); // Gate
    sequencer->setLayerChoiceLabels(kLayerGroove,    &arpStepLayerChoices[5]); // Groove
    sequencer->setLayerChoiceLabels(kLayerVelocity,  &arpStepLayerChoices[4]); // Velocity
    sequencer->setLayerChoiceLabels(kLayerAccent,    &arpStepLayerChoices[7]); // Retrig

    // Couches mode-specifiques.
    if (modeIndex == 0)
    {
        sequencer->setLayerChoiceLabels(kLayerDirection, &arpStepLayerChoices[1]); // Strum
        sequencer->setLayerChoiceLabels(kLayerPattern,   &arpStepLayerChoices[2]); // Jump
        sequencer->setLayerChoiceLabels(kLayerRange,     &arpStepLayerChoices[3]); // Octave
    }
    else
    {
        sequencer->setLayerChoiceLabels(kLayerDirection, &kDrumHitChoices); // Hit
        sequencer->setLayerChoiceLabels(kLayerPattern,   &kDrumEnvChoices);   // Velo Env
        sequencer->setLayerChoiceLabels(kLayerRange,     &kDrumEnvChoices);   // Tim Env
    }
}

int Arpeggiator::activePageLayerForSlot(int slot) const
{
    if (!juce::isPositiveAndBelow(slot, (int) kPageLayers.size()))
        return kLayerDirection;

    return kPageLayers[(size_t) slot];
}

juce::String Arpeggiator::activePageLabelForSlot(int slot) const
{
    if (!juce::isPositiveAndBelow(slot, (int) kArpeggiatorPageLabels.size()))
        return {};

    const int modeIndex = (arpeggiatorModeRaw != nullptr
                               ? juce::jlimit(0, 1, juce::roundToInt(arpeggiatorModeRaw->load()))
                               : 0);

    return (modeIndex == 0
                ? kArpeggiatorPageLabels[(size_t) slot]
                : kDrumMachinePageLabels[(size_t) slot]);
}

void Arpeggiator::refreshPageButtons()
{
    for (int i = 0; i < (int) pageButtons.size(); ++i)
    {
        auto* button = pageButtons[(size_t) i].get();
        if (button == nullptr)
            continue;

        const int layer = activePageLayerForSlot(i);
        const bool selected = (layer == activeLayer);

        button->setLayerIndex(layer);
        button->setText(activePageLabelForSlot(i));
        button->setSelected(selected);
    }
}

juce::String Arpeggiator::formatRateLengthDecimal(double beats)
{
    const double quantized = std::round(beats * 100.0) / 100.0;
    juce::String s(quantized, 2);

    while (s.containsChar('.') && s.endsWithChar('0'))
        s = s.dropLastCharacters(1);

    if (s.endsWithChar('.'))
        s = s.dropLastCharacters(1);

    return s;
}

void Arpeggiator::updateRateLengthOverlay()
{
    if (!rateKnob)
        return;

    const int safeRateLength = juce::jmax(1, effectiveSequenceLengthForLayer(kLayerRate));
    double totalBeats = 0.0;
    double rndMeanBeats = 0.0;

    for (int choice = 1; choice <= kRateChoiceCount; ++choice)
        rndMeanBeats += beatsForRateChoice(choice);
    rndMeanBeats /= (double) juce::jmax(1, kRateChoiceCount);

    for (int step = 0; step < safeRateLength; ++step)
    {
        int choice = applyModifierToStepChoice(kLayerRate, baseLayerValues[(size_t) kLayerRate][(size_t) step]);
        if (choice == 0)
            continue;

        if (choice == kRateRndIndex)
        {
            // Rnd n a pas de duree fixe; on affiche une moyenne attendue
            // pour conserver une indication stable pour l utilisateur.
            totalBeats += rndMeanBeats;
            continue;
        }

        totalBeats += beatsForRateChoice(choice);
    }

    rateKnob->setCenterOverlayColour(juce::Colours::white);
    rateKnob->setCenterOverlayText(formatRateLengthDecimal(totalBeats));
}

int Arpeggiator::modifierForLayer(int layer) const
{
    switch (layer)
    {
        case kLayerRate:     return rateKnob     ? rateKnob->getValue()     : 0;
        case kLayerGate:     return gateKnob     ? gateKnob->getValue()     : 0;
        case kLayerGroove:   return grooveKnob   ? grooveKnob->getValue()   : 0;
        case kLayerVelocity: return velocityKnob ? velocityKnob->getValue() : 0;
        default:             return 0;
    }
}

int Arpeggiator::applyModifierToStepChoice(int layer, int choiceIndex) const
{
    const int mod = modifierForLayer(layer);
    if (mod == 0)
        return choiceIndex;

    switch (layer)
    {
        case kLayerRate:
        {
            if (!isRateRegularChoice(choiceIndex))
                return choiceIndex;

            // Le morph de rate reste borne au type courant (binaries, dotted...).
            // Cela evite des "sauts de famille" non voulus en live.
            const int type = rateChoiceType(choiceIndex);
            const int slot = rateChoiceSlot1Based(choiceIndex); // 1..7

            if (mod > 0)
            {
                const double t = (double) mod / 100.0;
                const double movedSlot = (double) slot + t * (double) (kRateRatiosPerType - slot);
                return makeRateChoiceIndex(type,
                                           juce::jlimit(1, kRateRatiosPerType, juce::roundToInt((float) movedSlot)));
            }

            const double t = (double) (-mod) / 100.0;
            const double movedSlot = (double) slot - t * (double) (slot - 1);
            return makeRateChoiceIndex(type,
                                       juce::jlimit(1, kRateRatiosPerType, juce::roundToInt((float) movedSlot)));
        }

        case kLayerGate:
        {
            if (!isGateTimedChoice(choiceIndex))
                return choiceIndex;

            // Comportement "pas comme centre":
            // - 0  : inchange
            // - +100: tend vers la valeur gate maximale (99%)
            // - -100: tend vers la valeur gate minimale (5%)
            // Skip/Tie restent stables.
            const int slot = choiceIndex - kGateTimedChoiceFirst; // 0..7
            constexpr int kLastSlot = (kGateTimedChoiceLast - kGateTimedChoiceFirst);
            if (mod > 0)
            {
                const double t = (double) mod / 100.0;
                const double moved = (double) slot + t * (double) (kLastSlot - slot);
                return kGateTimedChoiceFirst
                    + juce::jlimit(0, kLastSlot, juce::roundToInt((float) moved));
            }

            const double t = (double) (-mod) / 100.0;
            const double moved = (double) slot - t * (double) slot;
            return kGateTimedChoiceFirst
                + juce::jlimit(0, kLastSlot, juce::roundToInt((float) moved));
        }

        case kLayerGroove:
        {
            if (!isGrooveFixedChoice(choiceIndex))
                return choiceIndex;

            // Skip (0) and "=" (8) are not morphed.
            if (choiceIndex == kGrooveChoiceEqual)
                return choiceIndex;

            const int groovePercent = groovePercentFromChoice(choiceIndex);

            // Meme principe de centre pour groove.
            if (mod > 0)
            {
                const double t = (double) mod / 100.0;
                const double moved = (double) groovePercent + t * (double) (-75 - groovePercent);
                return grooveChoiceFromPercent(moved, false);
            }

            const double t = (double) (-mod) / 100.0;
            const double moved = (double) groovePercent + t * (double) (75 - groovePercent);
            return grooveChoiceFromPercent(moved, false);
        }

        case kLayerVelocity:
        {
            if (!isVelocityFixedChoice(choiceIndex))
                return choiceIndex;

            // Skip (0) and "=" (4) are not morphed.
            if (choiceIndex == kVelocityChoiceEqual)
                return choiceIndex;

            const int basePercent = velocityPercentFromChoiceIndex(choiceIndex);

            // Velocity est un modificateur autour de la velocite source:
            // -100% -> vers 1, 0% -> inchange, +100% -> vers 127.
            if (mod > 0)
            {
                const double t = (double) mod / 100.0;
                const double moved = (double) basePercent + t * (double) (100 - basePercent);
                return choiceIndexFromVelocityPercent(moved, false);
            }

            const double t = (double) (-mod) / 100.0;
            const double moved = (double) basePercent + t * (double) (-100 - basePercent);
            return choiceIndexFromVelocityPercent(moved, false);
        }

        default:
            return choiceIndex;
    }
}

int Arpeggiator::removeModifierFromStepChoice(int layer, int choiceIndex) const
{
    const int mod = modifierForLayer(layer);
    if (mod == 0)
        return choiceIndex;

    switch (layer)
    {
        case kLayerRate:
        {
            if (!isRateRegularChoice(choiceIndex))
                return choiceIndex;

            const int type = rateChoiceType(choiceIndex);
            const int slot = rateChoiceSlot1Based(choiceIndex); // 1..7

            // Inversion de projection morph pour retrouver la base APVTS.
            if (mod >= 100 || mod <= -100)
                return choiceIndex;

            if (mod > 0)
            {
                const double t = (double) mod / 100.0;
                const double denom = juce::jmax(1.0e-6, 1.0 - t);
                const double baseSlot = ((double) slot - ((double) kRateRatiosPerType * t)) / denom;
                return makeRateChoiceIndex(type,
                                           juce::jlimit(1, kRateRatiosPerType, juce::roundToInt((float) baseSlot)));
            }

            const double t = (double) (-mod) / 100.0;
            const double denom = juce::jmax(1.0e-6, 1.0 - t);
            const double baseSlot = ((double) slot - t) / denom;
            return makeRateChoiceIndex(type,
                                       juce::jlimit(1, kRateRatiosPerType, juce::roundToInt((float) baseSlot)));
        }

        case kLayerGate:
        {
            if (!isGateTimedChoice(choiceIndex))
                return choiceIndex;

            // Inversion du mapping proportionnel.
            // A |mod|=100, inversion non bijective: on garde la valeur visible.
            if (mod >= 100 || mod <= -100)
                return choiceIndex;

            const int slot = choiceIndex - kGateTimedChoiceFirst; // 0..7
            constexpr int kLastSlot = (kGateTimedChoiceLast - kGateTimedChoiceFirst);
            if (mod > 0)
            {
                const double t = (double) mod / 100.0;
                const double denom = juce::jmax(1.0e-6, 1.0 - t);
                const double baseSlot = ((double) slot - ((double) kLastSlot * t)) / denom;
                return kGateTimedChoiceFirst
                    + juce::jlimit(0, kLastSlot, juce::roundToInt((float) baseSlot));
            }

            const double t = (double) (-mod) / 100.0;
            const double denom = juce::jmax(1.0e-6, 1.0 - t);
            const double baseSlot = (double) slot / denom;
            return kGateTimedChoiceFirst
                + juce::jlimit(0, kLastSlot, juce::roundToInt((float) baseSlot));
        }

        case kLayerGroove:
        {
            if (!isGrooveFixedChoice(choiceIndex))
                return choiceIndex;

            // Skip (0) and "=" (8) are not morphed.
            if (choiceIndex == kGrooveChoiceEqual)
                return choiceIndex;

            // Inversion du mapping proportionnel.
            // A |mod|=100, inversion non bijective: on garde la valeur visible.
            if (mod >= 100 || mod <= -100)
                return choiceIndex;

            const int groovePercent = groovePercentFromChoice(choiceIndex);

            if (mod > 0)
            {
                const double t = (double) mod / 100.0;
                const double denom = juce::jmax(1.0e-6, 1.0 - t);
                const double base = ((double) groovePercent + (75.0 * t)) / denom;
                return grooveChoiceFromPercent(base, false);
            }

            const double t = (double) (-mod) / 100.0;
            const double denom = juce::jmax(1.0e-6, 1.0 - t);
            const double base = ((double) groovePercent - (75.0 * t)) / denom;
            return grooveChoiceFromPercent(base, false);
        }

        case kLayerVelocity:
        {
            if (!isVelocityFixedChoice(choiceIndex))
                return choiceIndex;

            // Skip (0) and "=" (4) are not morphed.
            if (choiceIndex == kVelocityChoiceEqual)
                return choiceIndex;

            // Inversion du mapping proportionnel.
            // A |mod|=100, inversion non bijective: on garde la valeur visible.
            if (mod >= 100 || mod <= -100)
                return choiceIndex;

            const int displayedPercent = velocityPercentFromChoiceIndex(choiceIndex);

            if (mod > 0)
            {
                const double t = (double) mod / 100.0;
                const double denom = juce::jmax(1.0e-6, 1.0 - t);
                const double base = ((double) displayedPercent - (100.0 * t)) / denom;
                return choiceIndexFromVelocityPercent(base, false);
            }

            const double t = (double) (-mod) / 100.0;
            const double denom = juce::jmax(1.0e-6, 1.0 - t);
            const double base = ((double) displayedPercent + (100.0 * t)) / denom;
            return choiceIndexFromVelocityPercent(base, false);
        }

        default:
            return choiceIndex;
    }
}

void Arpeggiator::refreshStepDisplayFromBase()
{
    if (!sequencer)
        return;

    for (int layer = 0; layer < kNumLayers; ++layer)
    {
        for (int step = 0; step < kNumSteps; ++step)
        {
            const int baseChoice = baseLayerValues[(size_t) layer][(size_t) step];
            sequencer->setLayerStepValue(layer,
                                         step,
                                         applyModifierToStepChoice(layer, baseChoice));
        }
    }

    sequencer->setActiveLayer(activeLayer);
    sequencer->setPlaybackStep(playbackStepByLayer[(size_t) activeLayer]);
    updateRateLengthOverlay();
}

void Arpeggiator::refreshSingleLayerDisplayFromBase(int layer)
{
    if (!sequencer)
        return;

    const int safeLayer = juce::jlimit(0, kNumLayers - 1, layer);

    // Update canonical projection for one layer only.
    for (int step = 0; step < kNumSteps; ++step)
    {
        const int baseChoice = baseLayerValues[(size_t) safeLayer][(size_t) step];
        sequencer->setLayerStepValue(safeLayer,
                                     step,
                                     applyModifierToStepChoice(safeLayer, baseChoice));
    }

    // Keep visible state coherent if this layer is currently shown.
    if (safeLayer == activeLayer)
    {
        sequencer->setActiveLayer(activeLayer);
        sequencer->setPlaybackStep(playbackStepByLayer[(size_t) activeLayer]);
    }

    if (safeLayer == kLayerRate)
        updateRateLengthOverlay();
}

void Arpeggiator::refreshUIFromParameters()
{
    // Point d entree unique de synchro APVTS -> widgets.
    // applyingFromState bloque les boucles de feedback UI->APVTS->UI.
    ScopedApplyingFromState stateGuard { applyingFromState };

    bool enabled = true;
    if (auto* p = arpeggiatorEnableRaw)
        enabled = (p->load() > 0.5f);

    int modeIndex = 0;
    if (auto* p = arpeggiatorModeRaw)
        modeIndex = juce::jlimit(0, 1, juce::roundToInt(p->load()));

    const auto uiMode = (modeIndex == 0 ? SplitterModeToggle::RoundRobin
                                        : SplitterModeToggle::RangeSplit);
    arpeggiatorModeToggle.setMode(uiMode);
    applyModeLayerChoiceLabels(modeIndex);

    arpeggiatorTitleButton.setToggleState(enabled, juce::dontSendNotification);
    applyGlobalUiBypass(!enabled);
    arpeggiatorTitleButton.setButtonText(modeIndex == 0 ? "arpeggiator" : "drum machine");
    refreshPageButtons();

    auto syncKnob = [this](CustomRotaryNoCombo* knob, int morphIndex, const juce::String& baseParamID)
    {
        if (knob == nullptr)
            return;

        juce::RangedAudioParameter* parameter = nullptr;
        if (juce::isPositiveAndBelow(morphIndex, kNumMorphKnobs))
            parameter = morphParamPointers[(size_t) morphIndex];
        if (parameter == nullptr)
            parameter = parameters.getParameter(idForLane(baseParamID));

        if (parameter != nullptr)
            knob->setValue(juce::jlimit(-100, 100, juce::roundToInt(parameter->convertFrom0to1(parameter->getValue()))),
                           juce::dontSendNotification);
        else
            knob->setValue(0, juce::dontSendNotification);
    };

    syncKnob(rateKnob.get(), 0, ParamIDs::arpRateMorph);
    syncKnob(gateKnob.get(), 1, ParamIDs::arpGateMorph);
    syncKnob(grooveKnob.get(), 2, ParamIDs::arpGrooveMorph);
    syncKnob(velocityKnob.get(), 3, ParamIDs::arpVelocityMorph);

    refreshLinkToggleRateLabels();

    for (int layer = 0; layer < kNumLayers; ++layer)
    {
        const int firstStepDefault = firstStepDefaultChoiceForLayerInMode(layer, modeIndex);

        for (int step = 1; step <= kNumSteps; ++step)
        {
            int idx = 0;
            auto* p = getStepChoiceParameterForLayerStepMode(layer, step - 1, modeIndex);
            if (p != nullptr)
                idx = p->getIndex();

            if (step == 1 && idx == 0)
                idx = firstStepDefault;

            baseLayerValues[(size_t) layer][(size_t) (step - 1)] = idx;
        }
    }

    refreshStepDisplayFromBase();
}

void Arpeggiator::setActiveLayer(int newLayer)
{
    const int clamped = juce::jlimit(0, kNumLayers - 1, newLayer);

    if (clamped == activeLayer)
        return;

    activeLayer = clamped;

    if (sequencer)
    {
        sequencer->setActiveLayer(activeLayer);
        sequencer->setPlaybackStep(playbackStepByLayer[(size_t) activeLayer]);
    }

    refreshPageButtons();
}

void Arpeggiator::paint(juce::Graphics& g)
{
    const auto backgroundArea = getLocalBounds().toFloat().reduced((float) UiMetrics::kModuleOuterMargin);

    juce::Path backgroundPath;
    backgroundPath.addRoundedRectangle(backgroundArea, UiMetrics::kModuleCornerRadius);

    g.setColour(PluginColours::surface);
    g.fillPath(backgroundPath);
}

void Arpeggiator::resized()
{
    // Layout deterministic:
    // - Header titre
    // - Ligne morph knobs
    // - Ligne selecteurs de page
    // - Grille pas + ligne link toggles
    constexpr int contentInset = UiMetrics::kModuleOuterMargin + UiMetrics::kModuleInnerMargin;
    auto area = getLocalBounds().reduced(contentInset);

    {
        auto titleArea = area.removeFromTop(24);
        arpeggiatorTitleButtonShadow->setBounds(titleArea.expanded(10));
        arpeggiatorTitleButton.setBounds(arpeggiatorTitleButtonShadow->getShadowArea());
        // Keep the toggle inside the title pill, like Splitter mode toggle.
        arpeggiatorModeToggle.setBounds(titleArea.removeFromRight(32).reduced(4));
    }

    area.removeFromTop(spaceHeaderToRow1);

    const int knobHeight = 56;
    const int knobWidth = 70;

    {
        auto row1 = area.removeFromTop(knobHeight);
        auto content = row1.withSizeKeepingCentre(knobWidth * 4, knobHeight);

        if (rateKnob)     rateKnob->setBounds(content.removeFromLeft(knobWidth).reduced(4, 0));
        if (gateKnob)     gateKnob->setBounds(content.removeFromLeft(knobWidth).reduced(4, 0));
        if (grooveKnob)   grooveKnob->setBounds(content.removeFromLeft(knobWidth).reduced(4, 0));
        if (velocityKnob) velocityKnob->setBounds(content.removeFromLeft(knobWidth).reduced(4, 0));
    }

    area.removeFromTop(spaceRow1ToRow2);

    {
        auto row2 = area.removeFromTop(knobHeight);
        constexpr int pageButtonWidth = 56;
        auto content = row2.withSizeKeepingCentre(pageButtonWidth * (int) pageButtons.size(), knobHeight);

        for (int i = 0; i < (int) pageButtons.size(); ++i)
        {
            if (pageButtons[(size_t) i])
                pageButtons[(size_t) i]->setBounds(content.removeFromLeft(pageButtonWidth).reduced(3, 0));
        }
    }

    area.removeFromTop(spaceRow2ToSeq);

    auto footerContainer = area.removeFromBottom(28 + UiMetrics::kModuleInnerMargin);
    auto footerArea = footerContainer.removeFromTop(28);

    constexpr int kSequencerWidth = 270;
    constexpr int kSequencerHeight = 130;
    const auto sequencerBounds = area.withSizeKeepingCentre(kSequencerWidth, kSequencerHeight);

    auto toggleTrackArea = footerArea.withX(sequencerBounds.getX())
                                     .withWidth(sequencerBounds.getWidth());

    const int toggleHeight = juce::jmin(20, toggleTrackArea.getHeight());
    const int spacing = UiMetrics::kModuleInnerMargin;
    const int toggleCount = (int) kVisibleLinkLayers.size();
    const int totalSpacing = spacing * juce::jmax(0, toggleCount - 1);
    const int toggleWidth = juce::jmax(18, (toggleTrackArea.getWidth() - totalSpacing) / juce::jmax(1, toggleCount));
    const int usedWidth = (toggleWidth * toggleCount) + totalSpacing;
    const int toggleStartX = toggleTrackArea.getX() + juce::jmax(0, (toggleTrackArea.getWidth() - usedWidth) / 2);
    const int toggleY = toggleTrackArea.getCentreY() - (toggleHeight / 2);

    for (int i = 0; i < toggleCount; ++i)
    {
        const int layerIndex = kVisibleLinkLayers[(size_t) i];
        if (layerLinkToggles[(size_t) layerIndex])
        {
            layerLinkToggles[(size_t) layerIndex]->setVisible(true);
            layerLinkToggles[(size_t) layerIndex]->setBounds(toggleStartX + i * (toggleWidth + spacing),
                                                             toggleY,
                                                             toggleWidth,
                                                             toggleHeight);
        }
    }

    if (sequencer)
        sequencer->setBounds(sequencerBounds);
}

void Arpeggiator::clearPlaybackState()
{
    playbackStepByLayer.fill(-1);
    currentDirectionMode = 1;
    currentJumpSize = 1;
    currentStepDurationBeats = 0.25;
    currentOctaveOffset = 0;
    currentRetrigCount = 1;
    hasLastUiPpq = false;
    lastUiPpq = 0.0;

    if (sequencer)
        sequencer->setPlaybackStep(-1);
}

void Arpeggiator::resetPlaybackState(double ppqNow)
{
    nextRateStepPpq = ppqNow;
    playbackCursorByLayer.fill(0);
    playbackStepByLayer.fill(-1);
    currentDirectionMode = 1;
    currentJumpSize = 1;
    currentStepDurationBeats = 0.25;
    currentOctaveOffset = 0;
    currentRetrigCount = 1;

    if (sequencer)
        sequencer->setPlaybackStep(-1);
}

bool Arpeggiator::advancePlaybackTo(double ppqNow)
{
    if (ppqNow + 1.0e-9 < nextRateStepPpq)
        return false;

    bool moved = false;
    int guard = 0;

    while (ppqNow + 1.0e-9 >= nextRateStepPpq && guard < 64)
    {
        moved |= triggerOneRateStep();
        ++guard;
    }

    return moved;
}

bool Arpeggiator::triggerOneRateStep()
{
    // Le rate est la timeline maitre:
    // - Skip en rate: on avance juste le curseur rate.
    // - Pas valide: on echantillonne toutes les couches au meme "tick logique".
    std::array<int, kNumLayers> safeLengthByLayer {};
    for (int layer = 0; layer < kNumLayers; ++layer)
        safeLengthByLayer[(size_t) layer] = juce::jmax(1, effectiveSequenceLengthForLayer(layer));

    // Cache link UI pour ce tick visuel:
    // evite des loads atomiques repetes pendant la recherche.
    std::array<uint8_t, kNumLayers> linkStateByLayer {};
    linkStateByLayer[(size_t) kLayerRate] = 1;
    for (int layer = 1; layer < kNumLayers; ++layer)
    {
        bool linked = true;
        if (auto* p = layerLinkRawPointers[(size_t) layer])
            linked = (p->load() > 0.5f);
        linkStateByLayer[(size_t) layer] = linked ? (uint8_t) 1 : (uint8_t) 0;
    }

    const int safeRateLength = safeLengthByLayer[(size_t) kLayerRate];

    const auto findNextValidTick = [this, safeRateLength, &safeLengthByLayer, &linkStateByLayer](const std::array<int, kNumLayers>& cursorsIn,
                                                                                                   std::array<int, kNumLayers>& outStepByLayer,
                                                                                                   std::array<int, kNumLayers>& outCursorsAfter,
                                                                                                   int& outRateChoiceRaw) -> bool
    {
        auto cursors = cursorsIn;

        for (int safety = 0; safety < kNumSteps; ++safety)
        {
            const int rateStep = juce::jlimit(0, safeRateLength - 1, cursors[(size_t) kLayerRate]);
            const int rateChoice = applyModifierToStepChoice(kLayerRate,
                                                             baseLayerValues[(size_t) kLayerRate][(size_t) rateStep]);

            const int nextRateCursor = (rateStep + 1) % safeRateLength;
            const bool didRateWrap = (nextRateCursor == 0);

            if (rateChoice == 0)
            {
                cursors[(size_t) kLayerRate] = nextRateCursor;
                continue;
            }

            outStepByLayer.fill(-1);
            outStepByLayer[(size_t) kLayerRate] = rateStep;

            outCursorsAfter = cursors;
            outCursorsAfter[(size_t) kLayerRate] = nextRateCursor;

            for (int layer = 1; layer < kNumLayers; ++layer)
            {
                const int safeLayerLength = safeLengthByLayer[(size_t) layer];
                const int step = juce::jlimit(0, safeLayerLength - 1, cursors[(size_t) layer]);
                outStepByLayer[(size_t) layer] = step;

                int nextStep = (step + 1) % safeLayerLength;
                if (linkStateByLayer[(size_t) layer] != 0 && didRateWrap)
                    nextStep = 0;

                outCursorsAfter[(size_t) layer] = nextStep;
            }

            outRateChoiceRaw = rateChoice;
            return true;
        }

        return false;
    };

    std::array<int, kNumLayers> firstTickSteps {};
    std::array<int, kNumLayers> cursorsAfterFirstTick {};
    int firstRateChoiceRaw = 0;

    if (!findNextValidTick(playbackCursorByLayer, firstTickSteps, cursorsAfterFirstTick, firstRateChoiceRaw))
    {
        nextRateStepPpq += 0.25;
        playbackStepByLayer.fill(-1);
        return false;
    }

    int firstRateChoice = firstRateChoiceRaw;
    if (firstRateChoice == kRateRndIndex)
        firstRateChoice = 1 + playbackRandom.nextInt(kRateChoiceCount);

    currentStepDurationBeats = beatsForRateChoice(firstRateChoice);
    playbackStepByLayer = firstTickSteps;
    playbackCursorByLayer = cursorsAfterFirstTick;

    resolveMusicalStateForCurrentTick();
    nextRateStepPpq += currentStepDurationBeats;
    return true;
}

int Arpeggiator::effectiveSequenceLengthForLayer(int layer) const
{
    if (!juce::isPositiveAndBelow(layer, kNumLayers))
        return kNumSteps;

    // Scan inverse:
    // plus efficace quand la fin de sequence contient majoritairement des Skip.
    for (int step = kNumSteps - 1; step >= 0; --step)
    {
        if (baseLayerValues[(size_t) layer][(size_t) step] != 0)
            return step + 1;
    }

    // Si tout est Skip, garder 1 pas minimum pour eviter les modulo 0.
    return 1;
}

void Arpeggiator::resolveMusicalStateForCurrentTick()
{
    const auto readChoiceAtCurrentStep = [this](int layer) -> int
    {
        if (!juce::isPositiveAndBelow(layer, kNumLayers))
            return 0;

        const int step = playbackStepByLayer[(size_t) layer];
        if (!juce::isPositiveAndBelow(step, kNumSteps))
            return 0;

        const int baseChoice = baseLayerValues[(size_t) layer][(size_t) step];
        return applyModifierToStepChoice(layer, baseChoice);
    };

    int directionChoice = readChoiceAtCurrentStep(kLayerDirection);
    // 1=Top, 2=Up, 3="=", 4=Down, 5=Bottom, 6=Chord All,
    // 7=Up+Up, 8=Down+Down, 9=Mute, 10=Rnd(sans "=", sans Mute, sans Chord),
    // 11..22=Chord fractions, 23=Chord Rnd.
    if (directionChoice == kDirectionRndIndex)
    {
        static constexpr std::array<int, 6> kMainRndChoices {
            kDirectionTopIndex,
            kDirectionUpIndex,
            kDirectionDownIndex,
            kDirectionBottomIndex,
            kDirectionUpPairIndex,
            kDirectionDownPairIndex
        };
        directionChoice = kMainRndChoices[(size_t) playbackRandom.nextInt((int) kMainRndChoices.size())];
    }
    else if (directionChoice == kDirectionChordRndIndex)
    {
        static constexpr std::array<int, 13> kChordRndChoices {
            kDirectionChordAllIndex,
            11, 12, 13, 14, 15, 16,
            17, 18, 19, 20, 21, 22
        };
        directionChoice = kChordRndChoices[(size_t) playbackRandom.nextInt((int) kChordRndChoices.size())];
    }

    if (directionChoice >= 1 && directionChoice <= kDirectionChordRndIndex)
        currentDirectionMode = directionChoice;

    int patternChoice = readChoiceAtCurrentStep(kLayerPattern);
    if (patternChoice == kPatternRndIndex)
        patternChoice = 1 + playbackRandom.nextInt(8);
    if (patternChoice >= 1 && patternChoice <= 8)
        currentJumpSize = patternChoice;

    int octaveChoice = readChoiceAtCurrentStep(kLayerRange);
    if (octaveChoice == kRangeRndIndex)
        currentOctaveOffset = playbackRandom.nextInt(9) - 4;
    else if (octaveChoice >= 1 && octaveChoice <= 9)
        currentOctaveOffset = octaveChoice - 5;

    int accentChoice = readChoiceAtCurrentStep(kLayerAccent);
    if (accentChoice == kAccentRndIndex)
        currentRetrigCount = 1 + playbackRandom.nextInt(8);
    else if (accentChoice >= 1 && accentChoice <= 8)
        currentRetrigCount = accentChoice;
}

double Arpeggiator::beatsForRateChoice(int choiceIndex) const
{
    if (!isRateRegularChoice(choiceIndex))
        return 0.25; // secours: 1/16

    const int type = rateChoiceType(choiceIndex);
    const int slot = rateChoiceSlot1Based(choiceIndex) - 1; // 0..6

    double ratioNumerator = 1.0;
    double ratioDenominator = 1.0;

    switch (type)
    {
        case 0: // Binaries
            ratioNumerator = 1.0;
            ratioDenominator = (double) (1u << slot);
            break;

        case 1: // Dotted
            ratioNumerator = 3.0;
            ratioDenominator = (double) (1u << (slot + 1));
            break;

        case 2: // Double dotted
            ratioNumerator = 7.0;
            ratioDenominator = (double) (1u << (slot + 2));
            break;

        case 3: // Triolet
            ratioNumerator = 2.0;
            ratioDenominator = 3.0 * (double) (1u << slot);
            break;

        case 4: // Quintolet
            ratioNumerator = 4.0;
            ratioDenominator = 5.0 * (double) (1u << slot);
            break;

        case 5: // Septolet
            ratioNumerator = 4.0;
            ratioDenominator = 7.0 * (double) (1u << slot);
            break;

        default:
            return 0.25;
    }

    return 4.0 * (ratioNumerator / ratioDenominator); // duree en noires
}

void Arpeggiator::timerCallback()
{
    // Playhead UI now follows processor-authoritative playback steps.
    // This avoids drift between local UI extrapolation and RT timeline.
    bool enabled = true;
    if (auto* p = arpeggiatorEnableRaw)
        enabled = (p->load() > 0.5f);

    if (!enabled)
    {
        bool hadAnyStep = false;
        for (int layer = 0; layer < 8; ++layer)
            hadAnyStep = hadAnyStep || (playbackStepByLayer[(size_t) layer] >= 0);

        if (hadAnyStep || wasPlaying)
            clearPlaybackState();

        wasPlaying = false;
        return;
    }

    bool changed = false;
    bool anyStepActive = false;
    for (int layer = 0; layer < 8; ++layer)
    {
        const int step = processor.getLaneArpPlaybackStepForUI(lane, layer);
        anyStepActive = anyStepActive || juce::isPositiveAndBelow(step, 32);
        if (playbackStepByLayer[(size_t) layer] != step)
        {
            playbackStepByLayer[(size_t) layer] = step;
            changed = true;
        }
    }

    if (!anyStepActive)
    {
        // Keep deterministic idle visual state when transport/local clock is inactive.
        playbackCursorByLayer.fill(0);
    }

    if (changed && sequencer)
        sequencer->setPlaybackStep(playbackStepByLayer[(size_t) activeLayer]);

    wasPlaying = anyStepActive;
}

void Arpeggiator::uiTimerTick()
{
    if (rateKnob)
        rateKnob->setDisplayValueOverride((float) processor.getLaneArpMorphDisplayValueForUI(lane, 0));
    if (gateKnob)
        gateKnob->setDisplayValueOverride((float) processor.getLaneArpMorphDisplayValueForUI(lane, 1));
    if (grooveKnob)
        grooveKnob->setDisplayValueOverride((float) processor.getLaneArpMorphDisplayValueForUI(lane, 2));
    if (velocityKnob)
        velocityKnob->setDisplayValueOverride((float) processor.getLaneArpMorphDisplayValueForUI(lane, 3));

    if (rateKnob)     rateKnob->uiTimerTick();
    if (gateKnob)     gateKnob->uiTimerTick();
    if (grooveKnob)   grooveKnob->uiTimerTick();
    if (velocityKnob) velocityKnob->uiTimerTick();

    timerCallback();
}
