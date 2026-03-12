/*
==============================================================================
Splitter.cpp
------------------------------------------------------------------------------
Role in architecture:
- UI control surface for one lane Splitter module.
- Edits lane-scoped splitter parameters in APVTS.
- Maintains coherent UI constraints (channel uniqueness, mode-dependent rows,
  activation cascade between line toggles).

Design notes:
- This file does not route MIDI directly. It only writes/reads parameters.
- Routing behavior is implemented by SplitterProcessor on audio thread.

Threading:
- Message thread only.
- Parameter reads/writes are UI-side; no RT assumptions here.
==============================================================================
*/

#include "Splitter.h"
#include "../PluginColours.h"
#include "../PluginLookAndFeel.h"
#include "../PluginParameters.h"
#include "../UiMetrics.h"
#include "../0-component/ModulePresetStore.h"

// ============================================================================
//  Hotspot cliquable pour chaque flèche (1 composant = 1 ligne)
// ============================================================================
class Splitter::ArrowHotspot : public juce::Component
{
public:
    explicit ArrowHotspot(int lineNoIn, std::function<void(int)> onClickIn)
        : lineNo(lineNoIn), onClick(std::move(onClickIn))
    {
        setInterceptsMouseClicks(true, false);
        setRepaintsOnMouseActivity(false);
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
        setOpaque(false);
    }

    void paint(juce::Graphics&) override {} // invisible

    void mouseDown(const juce::MouseEvent&) override
    {
        if (onClick) onClick(lineNo);
    }

private:
    int lineNo; // 1..5
    std::function<void(int)> onClick;
};

// ============================================================================
// Construction:
// - instantiate header/mode controls
// - build line toggles and line components
// - attach APVTS write callbacks
// - install arrow hotspots for easier hit-testing/clicking
// ============================================================================
Splitter::Splitter(juce::AudioProcessorValueTreeState& state, Lanes::Lane laneIn)
    : parameters(state)
    , lane(laneIn)
{
    // Header
    splitterTitleButton.setClickingTogglesState(true);
    splitterTitleButton.setToggleState(true, juce::dontSendNotification);
    splitterTitleButton.setButtonText(modeToggle.getMode() == SplitterModeToggle::RoundRobin
                                       ? "round robin"
                                       : "range splitter");
    splitterTitleButtonShadow = std::make_unique<ShadowComponent>(
        juce::DropShadow(juce::Colours::black, 0, juce::Point<int>(0, 0)),
        -1.0f, true, true, true, true, 10);
    splitterTitleButtonShadow->addAndMakeVisible(splitterTitleButton);
    this->addAndMakeVisible(*splitterTitleButtonShadow);

    splitterTitleButton.onClick = [this]
    {
        if (applyingFromState)
            return;

        writeBoolParam(ParamIDs::Base::splitterEnable, splitterTitleButton.getToggleState());
        updateVisibility();
    };
    splitterTitleButton.onPopupClick = [this](const juce::MouseEvent&)
    {
        showModulePresetMenu();
    };

    modeToggle.onModeChange = [this](SplitterModeToggle::Mode mode) {
        if (applyingFromState)
            return;

        writeChoiceIndexParam(ParamIDs::Base::splitterMode, (int) mode);

        updateVisibility();
        splitterTitleButton.setButtonText(mode == SplitterModeToggle::RoundRobin
                                          ? "round robin"
                                          : "range splitter");
    };
    this->addAndMakeVisible(modeToggle);

    modeToggle.parameterValueChanged(0, (float) readChoiceIndexParam(ParamIDs::Base::splitterMode, 0));

    for (int i = 0; i < 5; ++i)
    {
        auto toggle = std::make_unique<SplitLineToggle>(i);
        toggle->onToggle = [this, i](bool newState)
        {
            if (applyingFromState)
                return;

            if (newState)
            {
                if (i >= 2)
                    for (int j = 1; j < i; ++j)
                        if (!lineToggles[j]->isActive())
                            lineToggles[j]->setActive(true);
            }
            else
            {
                if (i >= 1)
                    for (int j = i + 1; j < (int) lineToggles.size(); ++j)
                        if (lineToggles[j]->isActive())
                            lineToggles[j]->setActive(false);
            }

            updateVisibility();
            pushUiStateToParameters();
        };
        this->addAndMakeVisible(*toggle);
        lineToggles.push_back(std::move(toggle));
    }

    directOutLabel.setFont(juce::Font(juce::FontOptions(14.0f)));
    directOutLabel.setJustificationType(juce::Justification::centred);
    directOutLabel.setText("Direct Out", juce::dontSendNotification);
    this->addAndMakeVisible(directOutLabel);

    line1Combo.addItem("mute", 1);
    for (int i = 1; i <= 16; ++i) line1Combo.addItem(juce::String(i), i + 1);
    line1Combo.setSelectedId(2, juce::dontSendNotification);
    line1ComboShadow = std::make_unique<ShadowComponent>(
        juce::DropShadow(juce::Colours::black, 0, juce::Point<int>(0, 0)),
        -1.0f, true, true, true, true, 10
    );
    line1ComboShadow->addAndMakeVisible(line1Combo);
    this->addAndMakeVisible(*line1ComboShadow);

    line1Combo.isItemEnabled = [this](int id)
    {
        if (id <= 1) return true;
        for (int i = 0; i < (int) splitLines.size(); ++i)
        {
            const int toggleIdx = i + 1;
            if (lineToggles[toggleIdx] && lineToggles[toggleIdx]->isActive())
                if (splitLines[i]->comboBox.getSelectedId() == id)
                    return false;
        }
        return true;
    };

    line1Combo.onClickBeforeOpen = [this]()
    {
        if (!lineToggles[0]->isActive())
        {
            lineToggles[0]->setActive(true);
            updateVisibility();
            this->repaint();
        }
    };

    line1Combo.onChange = [this]()
    {
        if (applyingFromState)
            return;

        const int selectedId = juce::jlimit(1, 17, line1Combo.getSelectedId());
        writeChoiceIndexParam(ParamIDs::Base::splitLine1Channel, selectedId - 1); // 0=mute, 1..16=channel
    };

    static constexpr const char* kLineVoiceLimitIds[4] =
    {
        ParamIDs::Base::splitLine2VoiceLimit,
        ParamIDs::Base::splitLine3VoiceLimit,
        ParamIDs::Base::splitLine4VoiceLimit,
        ParamIDs::Base::splitLine5VoiceLimit
    };

    static constexpr const char* kLineNoteMinIds[4] =
    {
        ParamIDs::Base::splitLine2NoteMin,
        ParamIDs::Base::splitLine3NoteMin,
        ParamIDs::Base::splitLine4NoteMin,
        ParamIDs::Base::splitLine5NoteMin
    };

    static constexpr const char* kLineNoteMaxIds[4] =
    {
        ParamIDs::Base::splitLine2NoteMax,
        ParamIDs::Base::splitLine3NoteMax,
        ParamIDs::Base::splitLine4NoteMax,
        ParamIDs::Base::splitLine5NoteMax
    };

    static constexpr const char* kLineChannelIds[4] =
    {
        ParamIDs::Base::splitLine2Channel,
        ParamIDs::Base::splitLine3Channel,
        ParamIDs::Base::splitLine4Channel,
        ParamIDs::Base::splitLine5Channel
    };

    for (int i = 0; i < 4; ++i)
    {
        auto line = std::make_unique<SplitterLineComponent>();
        line->setSplitIndex(i);
        line->setLayerIndex(i + 1);
        auto* linePtr = line.get();

        line->onComboPreOpen = [this, i]()
        {
            const int toggleIndex = i + 1;
            if (! lineToggles[toggleIndex]->isActive())
            {
                lineToggles[toggleIndex]->setActive(true);
                updateVisibility();
                this->repaint();
            }
        };

        line->voiceSlider.getSlider().onValueChange = [this, i, linePtr]()
        {
            if (applyingFromState)
                return;

            const int val = (int) linePtr->voiceSlider.getSlider().getValue();
            writeIntParam(kLineVoiceLimitIds[i], juce::jlimit(1, 16, val));
        };

        line->rangeSlider.getSlider().onValueChange = [this, i, linePtr]()
        {
            if (applyingFromState)
                return;

            const auto& slider = linePtr->rangeSlider.getSlider();
            const int minNote = (int) slider.getMinValue();
            const int maxNote = (int) slider.getMaxValue();
            writeIntParam(kLineNoteMinIds[i], juce::jlimit(0, 127, minNote));
            writeIntParam(kLineNoteMaxIds[i], juce::jlimit(0, 127, maxNote));
        };

        line->comboBox.isItemEnabled = [this, i](int id)
        {
            if (id <= 1) return true;

            const int requesterToggleIdx = i + 1;

            if (lineToggles[0] && lineToggles[0]->isActive())
                if (line1Combo.getSelectedId() == id)
                    return false;

            for (int j = 0; j < (int) splitLines.size(); ++j)
            {
                const int otherToggleIdx = j + 1;
                if (otherToggleIdx == requesterToggleIdx) continue;
                if (lineToggles[otherToggleIdx] && lineToggles[otherToggleIdx]->isActive())
                    if (splitLines[j]->comboBox.getSelectedId() == id)
                        return false;
            }
            return true;
        };

        line->comboBox.onChange = [this, i, linePtr]()
        {
            if (applyingFromState)
                return;

            const int selectedId = juce::jlimit(1, 17, linePtr->comboBox.getSelectedId());

            if (selectedId <= 1)
            {
                lineToggles[(size_t) (i + 1)]->setActive(false);
                return;
            }

            writeIntParam(kLineChannelIds[i], juce::jlimit(1, 16, selectedId - 1));
        };

        this->addAndMakeVisible(*line);
        splitLines.push_back(std::move(line));
    }

    for (int ln = 1; ln <= 5; ++ln)
    {
        arrowHotspots[ln] = std::make_unique<ArrowHotspot>(ln, [this](int lineNo){
            const int toggleIdx = lineNo - 1;
            if (toggleIdx >= 0 && toggleIdx < (int)lineToggles.size() && lineToggles[toggleIdx])
            {
                const bool newState = !lineToggles[toggleIdx]->isActive();
                lineToggles[toggleIdx]->setActive(newState);
                updateVisibility();
                this->repaint();
            }
        });
        this->addAndMakeVisible(*arrowHotspots[ln]);
    }

    this->addMouseListener(this, true);
    syncFromParameters();
    updateVisibility();
}

Splitter::~Splitter() = default;

// ============================================================================
//  Timer
// ============================================================================
void Splitter::timerCallback()
{
    syncFromParameters();
}

void Splitter::uiTimerTick()
{
    timerCallback();
}

juce::String Splitter::laneParamId(const char* baseId) const
{
    return ParamIDs::lane(baseId, lane);
}

juce::ValueTree Splitter::captureModulePresetState() const
{
    static constexpr const char* kParamBases[] =
    {
        ParamIDs::Base::splitterEnable,
        ParamIDs::Base::splitterMode,
        ParamIDs::Base::splitLineActive01,
        ParamIDs::Base::splitLine1Channel,
        ParamIDs::Base::splitLineActive02,
        ParamIDs::Base::splitLine2VoiceLimit,
        ParamIDs::Base::splitLine2Priority,
        ParamIDs::Base::splitLine2NoteMin,
        ParamIDs::Base::splitLine2NoteMax,
        ParamIDs::Base::splitLine2Channel,
        ParamIDs::Base::splitLineActive03,
        ParamIDs::Base::splitLine3VoiceLimit,
        ParamIDs::Base::splitLine3Priority,
        ParamIDs::Base::splitLine3NoteMin,
        ParamIDs::Base::splitLine3NoteMax,
        ParamIDs::Base::splitLine3Channel,
        ParamIDs::Base::splitLineActive04,
        ParamIDs::Base::splitLine4VoiceLimit,
        ParamIDs::Base::splitLine4Priority,
        ParamIDs::Base::splitLine4NoteMin,
        ParamIDs::Base::splitLine4NoteMax,
        ParamIDs::Base::splitLine4Channel,
        ParamIDs::Base::splitLineActive05,
        ParamIDs::Base::splitLine5VoiceLimit,
        ParamIDs::Base::splitLine5Priority,
        ParamIDs::Base::splitLine5NoteMin,
        ParamIDs::Base::splitLine5NoteMax,
        ParamIDs::Base::splitLine5Channel
    };

    std::vector<juce::String> ids;
    ids.reserve((int) (sizeof(kParamBases) / sizeof(kParamBases[0])));
    for (const auto* base : kParamBases)
        ids.push_back(laneParamId(base));

    return ModulePresetStore::captureParameterState(parameters, ids);
}

void Splitter::applyModulePresetState(const juce::ValueTree& state)
{
    if (!state.isValid())
        return;

    ModulePresetStore::applyParameterState(parameters, state);
    syncFromParameters();
    updateVisibility();
    repaint();
}

void Splitter::showSaveModulePresetDialog()
{
    auto* dialog = new juce::AlertWindow("Save As Splitter Preset",
                                         "Enter preset name.",
                                         juce::AlertWindow::NoIcon);
    dialog->addTextEditor("name", "Preset", "Name:");
    dialog->addButton("Save As", 1, juce::KeyPress(juce::KeyPress::returnKey));
    dialog->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    auto dialogSafe = juce::Component::SafePointer<juce::AlertWindow>(dialog);
    auto compSafe = juce::Component::SafePointer<Splitter>(this);

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
                                    if (!ModulePresetStore::savePreset("splitter",
                                                                       presetName,
                                                                       compSafe->captureModulePresetState(),
                                                                       &error))
                                    {
                                        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                                               "Save Splitter Preset",
                                                                               error);
                                    }
                                }),
                            true);
}

bool Splitter::saveLoadedModulePreset()
{
    if (!loadedModulePresetFile.existsAsFile())
        return false;

    juce::String error;
    if (!ModulePresetStore::savePresetInPlace(loadedModulePresetFile,
                                              "splitter",
                                              captureModulePresetState(),
                                              &error))
    {
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                               "Save Splitter Preset",
                                               error);
        return false;
    }

    return true;
}

void Splitter::showEditLoadedPresetDialog()
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
                                               "Edit Splitter Preset",
                                               error);
        return;
    }

    auto* dialog = new juce::AlertWindow("Edit Splitter Preset",
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
    auto compSafe = juce::Component::SafePointer<Splitter>(this);
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
                                                                                 "splitter",
                                                                                 newName,
                                                                                 &renamedFile,
                                                                                 &error))
                                        {
                                            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                                                   "Edit Splitter Preset",
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
                                                                                 "splitter",
                                                                                 &error))
                                        {
                                            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                                                   "Edit Splitter Preset",
                                                                                   error);
                                            return;
                                        }

                                        compSafe->loadedModulePresetFile = juce::File();
                                        compSafe->loadedModulePresetName.clear();
                                    }
                                }),
                            true);
}

void Splitter::showModulePresetMenu()
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
    const auto entries = ModulePresetStore::listPresets("splitter");
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

    auto compSafe = juce::Component::SafePointer<Splitter>(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&splitterTitleButton),
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
                                                                      "Load Splitter Preset",
                                                                      error);
                               return;
                           }

                           compSafe->applyModulePresetState(payload);
                           compSafe->loadedModulePresetFile = entry.file;
                           compSafe->loadedModulePresetName = entry.displayName;
                       });
}

void Splitter::writeBoolParam(const char* baseId, bool value)
{
    // UI -> APVTS helper (host automation aware).
    if (auto* p = dynamic_cast<juce::AudioParameterBool*>(parameters.getParameter(laneParamId(baseId))))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost(value ? 1.0f : 0.0f);
        p->endChangeGesture();
    }
}

void Splitter::writeIntParam(const char* baseId, int value)
{
    // AudioParameterInt expects normalized value when writing through host path.
    if (auto* p = dynamic_cast<juce::AudioParameterInt*>(parameters.getParameter(laneParamId(baseId))))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost(p->convertTo0to1((float) value));
        p->endChangeGesture();
    }
}

void Splitter::writeChoiceIndexParam(const char* baseId, int index)
{
    // Choice index is written in normalized domain for host compatibility.
    if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(parameters.getParameter(laneParamId(baseId))))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost(p->convertTo0to1((float) index));
        p->endChangeGesture();
    }
}

bool Splitter::readBoolParam(const char* baseId, bool fallback) const
{
    if (auto* p = parameters.getRawParameterValue(laneParamId(baseId)))
        return (p->load() > 0.5f);

    return fallback;
}

int Splitter::readIntParam(const char* baseId, int fallback) const
{
    if (auto* p = dynamic_cast<juce::AudioParameterInt*>(parameters.getParameter(laneParamId(baseId))))
        return p->get();

    if (auto* raw = parameters.getRawParameterValue(laneParamId(baseId)))
        return (int) raw->load();

    return fallback;
}

int Splitter::readChoiceIndexParam(const char* baseId, int fallback) const
{
    if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(parameters.getParameter(laneParamId(baseId))))
        return p->getIndex();

    return fallback;
}

void Splitter::pushUiStateToParameters()
{
    // Commit the whole UI topology atomically from user point of view.
    // Processor side will pick this snapshot and reconcile active notes.
    if (applyingFromState)
        return;

    static constexpr const char* kActiveIds[5] =
    {
        ParamIDs::Base::splitLineActive01,
        ParamIDs::Base::splitLineActive02,
        ParamIDs::Base::splitLineActive03,
        ParamIDs::Base::splitLineActive04,
        ParamIDs::Base::splitLineActive05
    };

    for (int i = 0; i < 5; ++i)
    {
        if (lineToggles[(size_t) i] != nullptr)
            writeBoolParam(kActiveIds[i], lineToggles[(size_t) i]->isActive());
    }

    for (int i = 0; i < 5; ++i)
        prevActive[(size_t) i] = (lineToggles[(size_t) i] != nullptr && lineToggles[(size_t) i]->isActive());
}

void Splitter::syncFromParameters()
{
    // Pull APVTS into UI widgets.
    // Guarded by applyingFromState to avoid callback feedback loops.
    if (applyingFromState)
        return;

    applyingFromState = true;
    bool changed = false;

    // Module enable
    {
        const bool enabled = readBoolParam(ParamIDs::Base::splitterEnable, true);
        if (splitterTitleButton.getToggleState() != enabled)
        {
            splitterTitleButton.setToggleState(enabled, juce::dontSendNotification);
            changed = true;
        }
    }

    // Mode
    {
        const int modeIdx = juce::jlimit(0, 1, readChoiceIndexParam(ParamIDs::Base::splitterMode, 0));
        const auto targetMode = (modeIdx == 0 ? SplitterModeToggle::RoundRobin : SplitterModeToggle::RangeSplit);
        const auto targetTitle = (modeIdx == 0 ? juce::String("round robin") : juce::String("range splitter"));

        if (modeToggle.getMode() != targetMode)
        {
            modeToggle.setMode(targetMode);
            changed = true;
        }

        if (splitterTitleButton.getButtonText() != targetTitle)
        {
            splitterTitleButton.setButtonText(targetTitle);
            changed = true;
        }
    }

    // Active lines
    static constexpr const char* kActiveIds[5] =
    {
        ParamIDs::Base::splitLineActive01,
        ParamIDs::Base::splitLineActive02,
        ParamIDs::Base::splitLineActive03,
        ParamIDs::Base::splitLineActive04,
        ParamIDs::Base::splitLineActive05
    };

    for (int i = 0; i < 5; ++i)
    {
        const bool active = readBoolParam(kActiveIds[i], i == 0);
        if (lineToggles[(size_t) i] != nullptr)
        {
            if (lineToggles[(size_t) i]->isActive() != active)
            {
                lineToggles[(size_t) i]->setActive(active);
                changed = true;
            }
        }
        prevActive[(size_t) i] = (splitterTitleButton.getToggleState() && active);
    }

    // Line 1 channel choice (0=mute, 1..16 channel)
    {
        const int channelIdx = juce::jlimit(0, 16, readChoiceIndexParam(ParamIDs::Base::splitLine1Channel, 1));
        const int selectedId = channelIdx + 1;
        if (line1Combo.getSelectedId() != selectedId)
        {
            line1Combo.setSelectedId(selectedId, juce::dontSendNotification);
            changed = true;
        }
    }

    static constexpr const char* kLineVoiceLimitIds[4] =
    {
        ParamIDs::Base::splitLine2VoiceLimit,
        ParamIDs::Base::splitLine3VoiceLimit,
        ParamIDs::Base::splitLine4VoiceLimit,
        ParamIDs::Base::splitLine5VoiceLimit
    };

    static constexpr const char* kLineNoteMinIds[4] =
    {
        ParamIDs::Base::splitLine2NoteMin,
        ParamIDs::Base::splitLine3NoteMin,
        ParamIDs::Base::splitLine4NoteMin,
        ParamIDs::Base::splitLine5NoteMin
    };

    static constexpr const char* kLineNoteMaxIds[4] =
    {
        ParamIDs::Base::splitLine2NoteMax,
        ParamIDs::Base::splitLine3NoteMax,
        ParamIDs::Base::splitLine4NoteMax,
        ParamIDs::Base::splitLine5NoteMax
    };

    static constexpr const char* kLineChannelIds[4] =
    {
        ParamIDs::Base::splitLine2Channel,
        ParamIDs::Base::splitLine3Channel,
        ParamIDs::Base::splitLine4Channel,
        ParamIDs::Base::splitLine5Channel
    };

    static constexpr int kDefaultChannels[4] = { 2, 3, 4, 5 };

    for (int i = 0; i < 4; ++i)
    {
        if (splitLines[(size_t) i] == nullptr)
            continue;

        const int voiceLimit = juce::jlimit(1, 16, readIntParam(kLineVoiceLimitIds[i], 16));
        const int noteMin    = juce::jlimit(0, 127, readIntParam(kLineNoteMinIds[i], 0));
        const int noteMax    = juce::jlimit(0, 127, readIntParam(kLineNoteMaxIds[i], 127));
        const int channel    = juce::jlimit(1, 16, readIntParam(kLineChannelIds[i], kDefaultChannels[i]));

        auto& voice = splitLines[(size_t) i]->voiceSlider.getSlider();
        auto& range = splitLines[(size_t) i]->rangeSlider.getSlider();
        auto& combo = splitLines[(size_t) i]->comboBox;

        if ((int) voice.getValue() != voiceLimit)
        {
            voice.setValue(voiceLimit, juce::dontSendNotification);
            changed = true;
        }

        const int sortedMin = std::min(noteMin, noteMax);
        const int sortedMax = std::max(noteMin, noteMax);
        if ((int) range.getMinValue() != sortedMin)
        {
            range.setMinValue(sortedMin, juce::dontSendNotification);
            changed = true;
        }
        if ((int) range.getMaxValue() != sortedMax)
        {
            range.setMaxValue(sortedMax, juce::dontSendNotification);
            changed = true;
        }

        const int selectedId = channel + 1; // 2..17
        if (combo.getSelectedId() != selectedId)
        {
            combo.setSelectedId(selectedId, juce::dontSendNotification);
            changed = true;
        }
    }

    if (changed)
        updateVisibility();

    applyingFromState = false;
}

// ============================================================================
//  Helpers IDs/Canaux
// ============================================================================
bool Splitter::isInLineArrowHitbox(int splitIndex, juce::Point<int> ptInSplitter) const
{
    const int lineNo = splitIndex + 2;            // 0..3 -> 2..5
    if (lineNo < 1 || lineNo > 5) return false;
    return arrowHitboxByLine[lineNo].contains(ptInSplitter);
}

bool Splitter::isIdAllowedFor(int requesterIndex, int itemId) const
{
    if (itemId <= 1) return true; // 1 = mute

    if (requesterIndex != 0)
        if (lineToggles[0] && lineToggles[0]->isActive())
            if (line1Combo.getSelectedId() == itemId)
                return false;

    for (int i = 0; i < (int) splitLines.size(); ++i)
    {
        const int toggleIdx = i + 1; // 1..4
        if (requesterIndex == toggleIdx) continue;

        if (lineToggles[toggleIdx] && lineToggles[toggleIdx]->isActive())
            if (splitLines[i]->comboBox.getSelectedId() == itemId)
                return false;
    }
    return true;
}

int Splitter::findNextAvailableId(int requesterIndex, int preferredStartId) const
{
    int start = juce::jlimit(1, 17, preferredStartId);

    // Cherche d'abord au-dessus
    {
        int id = start + 1;
        if (id < 2) id = 2;
        for (; id <= 17; ++id)
            if (isIdAllowedFor(requesterIndex, id))
                return id;
    }
    // Puis wrap
    {
        for (int id = 2; id <= start; ++id)
            if (isIdAllowedFor(requesterIndex, id))
                return id;
    }
    return 1; // mute en dernier recours
}

// ============================================================================
//  Visibilité / états
// ============================================================================
void Splitter::applyGlobalUiBypass(bool bypassed)
{
    // Keep title clickable so module can always be re-enabled quickly.
    if (splitterTitleButtonShadow)
        splitterTitleButtonShadow->setEnabled(true);
    splitterTitleButton.setEnabled(true);

    const bool enableChildren = !bypassed;

    modeToggle.setEnabled(enableChildren);
    directOutLabel.setEnabled(enableChildren);

    if (line1ComboShadow)
        line1ComboShadow->setEnabled(enableChildren);
    line1Combo.setEnabled(enableChildren && lineToggles[0] != nullptr && lineToggles[0]->isActive());

    for (int i = 0; i < (int) lineToggles.size(); ++i)
        if (lineToggles[(size_t) i] != nullptr)
            lineToggles[(size_t) i]->setEnabled(enableChildren);

    for (int i = 0; i < (int) splitLines.size(); ++i)
        if (splitLines[(size_t) i] != nullptr)
            splitLines[(size_t) i]->setEnabled(enableChildren);

    // Match other modules visual bypass feedback.
    setAlpha(bypassed ? 0.55f : 1.0f);
}

void Splitter::updateVisibility()
{
    // Enforce visual hierarchy rules:
    // - line activation cascade,
    // - mode-dependent controls per branch,
    // - title text follows splitter mode.
    const bool moduleEnabled = splitterTitleButton.getToggleState();
    const bool isRoundRobin = (modeToggle.getMode() == SplitterModeToggle::RoundRobin);

    applyGlobalUiBypass(!moduleEnabled);

    // Lignes 2..5
    for (int i = 0; i < 4; ++i)
    {
        const int requesterIndex = i + 1; // 1..4
        const bool activeNow = lineToggles[requesterIndex]->isActive();
        const bool wasActive = prevActive[requesterIndex];

        splitLines[i]->configure(activeNow, isRoundRobin);
        splitLines[i]->setEnabled(moduleEnabled && activeNow);

        if (moduleEnabled && activeNow && !wasActive)
        {
            const int currentId = splitLines[i]->comboBox.getSelectedId();
            if (!isIdAllowedFor(requesterIndex, currentId))
                splitLines[i]->comboBox.setSelectedId(findNextAvailableId(requesterIndex, currentId),
                                                      juce::dontSendNotification);
        }
    }

    // Ligne 1
    const bool activeL1Now = lineToggles[0]->isActive();
    const bool wasActiveL1 = prevActive[0];
    line1Combo.setEnabled(moduleEnabled && activeL1Now);

    if (moduleEnabled && activeL1Now && !wasActiveL1)
    {
        const int currentId = line1Combo.getSelectedId();
        if (!isIdAllowedFor(0, currentId))
            line1Combo.setSelectedId(findNextAvailableId(0, currentId), juce::dontSendNotification);
    }

    for (int i = 0; i < (int) lineToggles.size(); ++i)
        prevActive[i] = (moduleEnabled && lineToggles[i]->isActive());

    repaint();
}

// ============================================================================
//  Layout
// ============================================================================
void Splitter::resized()
{
    // Geometry pass:
    // - allocate title/mode row,
    // - place vertical toggles column,
    // - place line1 direct row + lines2..5 components.
    constexpr int contentInset = UiMetrics::kModuleOuterMargin + UiMetrics::kModuleInnerMargin;
    auto area = getLocalBounds().reduced(contentInset, contentInset);

    // Header
    auto titleArea = area.removeFromTop(24);
    splitterTitleButtonShadow->setBounds(titleArea.expanded(10));
    splitterTitleButton.setBounds(splitterTitleButtonShadow->getShadowArea());
    modeToggle.setBounds(titleArea.removeFromRight(32).reduced(4));

    // 5 lignes
    const int numRows   = 5;
    const int rowHeight = area.getHeight() / numRows;
    constexpr int comboWidth = 60;

    for (int i = 0; i < numRows; ++i)
    {
        const int rowTop = i * rowHeight;
        const auto fullRow = juce::Rectangle<int>(area.getX(), area.getY() + rowTop, area.getWidth(), rowHeight);

        // Colonne toggles
        auto rowArea   = fullRow;
        auto toggleCol = rowArea.removeFromLeft(32);
        lineToggles[i]->setBounds(toggleCol.getCentreX() - 8, rowArea.getCentreY() - 8, 16, 16);

        if (i > 0) // lignes 2..5
            splitLines[i - 1]->setVerticalCenterRef(rowArea.getCentreY());

        // Rect combo commun (aligné à droite)
        auto comboRectGlobal = rowArea.withX(rowArea.getRight() - comboWidth)
                                      .withWidth(comboWidth)
                                      .withSizeKeepingCentre(comboWidth, 24);

        if (i == 0)
        {
            // Ligne 1
            line1ComboShadow->setBounds(comboRectGlobal.expanded(10));
            line1Combo.setBounds(line1ComboShadow->getShadowArea());

            auto toggleCenter = lineToggles[0]->getBounds().getCentre();
            int labelX = toggleCenter.x + 14;
            int labelY = toggleCenter.y - 24;
            int labelW = comboRectGlobal.getX() - labelX - 14;
            directOutLabel.setBounds(labelX, labelY, labelW, 24);
        }
        else
        {
            // Headroom pour labels internes des SplitterLineComponent
            constexpr int kTopHeadroom = 24;
            auto rowWithHeadroom = rowArea.withY(rowArea.getY() - kTopHeadroom)
                                          .withHeight(rowArea.getHeight() + kTopHeadroom);

            splitLines[i - 1]->setBounds(rowWithHeadroom);

            auto comboRectLocal = comboRectGlobal.translated(
                -rowWithHeadroom.getX(), -rowWithHeadroom.getY()
            );
            splitLines[i - 1]->setComboBounds(comboRectLocal);
        }
    }
}

// ============================================================================
//  Dessin + flèches + hotspots
// ============================================================================
void Splitter::paint(juce::Graphics& g)
{
    // Draw static background blocks only.
    // Interactive elements are painted by child controls.
    // Reset hitboxes
    directOutArrowHitbox = {};
    for (auto& r : arrowHitboxByLine) r = {};

    // Fond + ombre
    const auto backgroundArea = getLocalBounds().toFloat().reduced((float) UiMetrics::kModuleOuterMargin);

    juce::Path backgroundPath; backgroundPath.addRoundedRectangle(backgroundArea, UiMetrics::kModuleCornerRadius);
    g.setColour(PluginColours::surface);
    g.fillPath(backgroundPath);

    // Liens entre toggles (derrière tout)
    const float linkThickness = 6.0f;
    const juce::PathStrokeType linkStroke(linkThickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    auto drawLink = [&](juce::Point<float> from, juce::Point<float> to, bool active)
    {
        juce::Path p; p.startNewSubPath(from); p.lineTo(to);
        g.setColour(active ? PluginColours::primary : PluginColours::surface);
        g.strokePath(p, linkStroke);
    };
    for (int i = 1; i < (int) lineToggles.size(); ++i)
    {
        if (!lineToggles[i-1] || !lineToggles[i]) continue;
        auto bPrev = lineToggles[i-1]->getBounds();
        auto bCur  = lineToggles[i]->getBounds();
        drawLink({ (float)bPrev.getCentreX(), (float)bPrev.getCentreY() },
                 { (float)bCur .getCentreX(), (float)bCur .getCentreY() },
                 lineToggles[i]->isActive());
    }

    // Constantes flèches
    static constexpr float kArrowThickness = 6.0f;
    static constexpr float kArrowLen       = 10.0f;
    static constexpr float kArrowHalfH     = 6.0f;
    static constexpr float kStartOffsetX   = 10.0f;
    static constexpr float kAirBeforeTip   = 6.0f;
    static constexpr float kMinSegDX       = 12.0f;

    auto getComboRectOnThis = [this]() -> juce::Rectangle<int>
    {
        if (auto* p = line1Combo.getParentComponent(); p && p != this)
            return line1Combo.getBounds().translated(p->getX(), p->getY());
        return line1Combo.getBounds();
    };

    auto drawArrow = [&](juce::Point<float> from,
                         const juce::Rectangle<int>& comboRect,
                         bool active,
                         juce::Rectangle<int>* outHitbox)
    {
        const float targetY = std::floor((float)comboRect.getCentreY()) + 0.5f;
        float startX = from.x + kStartOffsetX;
        float endX   = (float)comboRect.getX() - kAirBeforeTip - kArrowLen;
        float safeEndX = juce::jmax(endX, startX + kMinSegDX);

        const float comboLeftX = (float)comboRect.getX();
        if (safeEndX + kArrowLen >= comboLeftX)
            safeEndX = comboLeftX - kArrowLen - 1.0f;

        g.setColour(active ? PluginColours::primary : PluginColours::surface);

        juce::Path line; line.startNewSubPath(startX, targetY); line.lineTo(safeEndX, targetY);
        g.strokePath(line, juce::PathStrokeType(kArrowThickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        juce::Path tip;
        tip.addTriangle(safeEndX,            targetY - kArrowHalfH,
                        safeEndX + kArrowLen, targetY,
                        safeEndX,            targetY + kArrowHalfH);
        g.fillPath(tip);

        // Hitbox brute (avant réduction)
        const float hitStartX = startX;
        const float hitEndX   = safeEndX + kArrowLen;

        int hx = (int) std::floor(std::min(hitStartX, hitEndX));
        int hw = (int) std::ceil(std::abs(hitEndX - hitStartX));
        int hy = (int) std::floor(targetY) - 8;
        int hh = 16;
        if (hw < 4) hw = 4;

        if (outHitbox) *outHitbox = { hx, hy, hw, hh };
    };

        auto trimToTip = [&](juce::Rectangle<int>& r, int comboLeftX)
    {
        const int tipWidth = 200;
        int newRight = juce::jmin(r.getRight(), comboLeftX);
        int newLeft  = juce::jmax(r.getX(), newRight - tipWidth);
        if (newLeft >= newRight) newLeft = newRight - 8; // min 8 px

        r.setX(newLeft);
        r.setWidth(newRight - newLeft);

        const int tipHalfH = 9;
        const int cy = r.getCentreY();
        r.setY(cy - tipHalfH);
        r.setHeight(tipHalfH * 2);
    };

    // Flèche L1 (Direct Out)
    if (!lineToggles.empty() && lineToggles[0])
    {
        const bool activeLine1 = lineToggles[0]->isActive();
        const auto t0 = lineToggles[0]->getBounds();
        const juce::Point<float> from((float)t0.getCentreX(), (float)t0.getCentreY());
        const auto c1OnThis = getComboRectOnThis();

        drawArrow(from, c1OnThis, activeLine1, &directOutArrowHitbox);

        // Trim à la pointe uniquement (évite recouvrement sliders)
        trimToTip(directOutArrowHitbox, c1OnThis.getX());

        arrowHitboxByLine[1] = directOutArrowHitbox;
    }

    // Flèches L2..L5
    for (int li = 1; li < (int) lineToggles.size(); ++li) // li = 1..4 (L2..L5)
    {
        if (!lineToggles[li]) continue;

        const int splitIdx = li - 1;   // 0..3
        const int lineNo   = li + 1;   // 2..5

        if (splitIdx < 0 || splitIdx >= (int) splitLines.size() || !splitLines[splitIdx])
        {
            arrowHitboxByLine[lineNo] = {};
            continue;
        }
        if (!splitLines[splitIdx]->isVisible())
        {
            arrowHitboxByLine[lineNo] = {};
            continue;
        }

        auto tB = lineToggles[li]->getBounds();
        juce::Point<float> from((float) tB.getCentreX(), (float) tB.getCentreY());
        auto comboRectOnThis = splitLines[splitIdx]->getComboBoundsInParent();
        const bool active = lineToggles[li]->isActive();

        juce::Rectangle<int> hb;
        drawArrow(from, comboRectOnThis, active, &hb);

        // Trim à la pointe uniquement (évite recouvrement sliders)
        trimToTip(hb, comboRectOnThis.getX());

        arrowHitboxByLine[lineNo] = hb;
    }

    // Positionne les hotspots exactement sur les hitboxes (et derrière tout)
    for (int ln = 1; ln <= 5; ++ln)
    {
        if (arrowHotspots[ln])
        {
            arrowHotspots[ln]->setBounds(arrowHitboxByLine[ln]);
            arrowHotspots[ln]->toBack(); // garantit que sliders/combo passent avant
        }
    }
}

// ============================================================================
//  Utilitaire de debug
// ============================================================================
int Splitter::lineNoAtPoint(juce::Point<int> pt) const
{
    for (int lineNo = 1; lineNo <= 5; ++lineNo)
        if (arrowHitboxByLine[lineNo].contains(pt))
            return lineNo;
    return -1;
}

// ============================================================================
//  Souris globale (les flèches ne dépendent plus de ces handlers)
// ============================================================================
void Splitter::mouseMove(const juce::MouseEvent& e)
{
    const auto pos  = e.getEventRelativeTo(this).getPosition();
    const bool over = (lineNoAtPoint(pos) != -1);
    setMouseCursor(over ? juce::MouseCursor::PointingHandCursor
                        : juce::MouseCursor::NormalCursor);
}

void Splitter::mouseExit(const juce::MouseEvent&)
{
    setMouseCursor(juce::MouseCursor::NormalCursor);
}

void Splitter::mouseDown(const juce::MouseEvent& e)
{
    juce::ignoreUnused(e);
    // Plus besoin de gérer les flèches ici : ArrowHotspot s’en charge.
}
