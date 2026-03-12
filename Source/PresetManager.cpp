/*
  ==============================================================================
    PresetManager.cpp
  ------------------------------------------------------------------------------
    MidivisiVici - Preset / bank / snapshot system implementation.

    Key goals:
    - Persistent snapshot storage (ValueTree) + memory mirror for fast access
    - Strict RT rules: audio thread never touches ValueTree
    - Clean Save/Pending contract for the UI Save button
    - Robust DAW save/restore integration

    PROJECT RULES:
    - ASCII only for comments/logs (no accents / no emoji).
    - RT safety: no allocations, no disk I/O, no locks, no heavy logs in audio thread.
  ==============================================================================
*/

#include "PresetManager.h"

#include "0-component/MorphParameterManager.h"
#include "DebugConfig.h"
#include "PluginParameters.h"

#include <algorithm>

//==============================================================================
// 0) Local helpers (file scope)
//------------------------------------------------------------------------------
namespace
{
    // Clamp MSB/LSB/PC values to safe ranges without assuming a fixed bank size.
    // The actual clamp is done by LayoutTables on RT side, but we keep inputs sane.
    int clampMidi7Bit(int v) noexcept { return juce::jlimit(0, 127, v); }

    constexpr const char* kPresetLibrarySubdir      = "MidivisiVici";
    constexpr const char* kPresetLibraryRootDir     = "PresetLibrary";
    constexpr const char* kPresetLibraryIndexFile   = "LibraryIndex.xml";
    constexpr const char* kPresetLibraryLegacyFile  = "PresetLibrary.xml";
    constexpr const char* kPresetLibraryIndexTag    = "PRESET_LIBRARY_INDEX";
    constexpr const char* kPresetLibraryBankMetaTag = "BANK_META";
    constexpr const char* kPresetLibraryPresetTag   = "PRESET_META";
    constexpr const char* kMetaFormatVersion        = "formatVersion";
    constexpr const char* kMetaIndex                = "index";
    constexpr const char* kMetaName                 = "name";
    constexpr const char* kMetaSnapshotCount        = "snapshotCount";
    constexpr int kPresetLibraryFormatVersion       = 2;

    constexpr const char* kApvtsParamIdProp = "id";
    constexpr const char* kApvtsValueProp   = "value";

    static bool setParameterNormalizedRecursive(juce::ValueTree& node,
                                                const juce::String& parameterId,
                                                float normalized) noexcept
    {
        // Non-RT helper used during preset default shaping only.
        // This walks the serialized APVTS tree and rewrites one parameter
        // normalized value by id.
        if (!node.isValid())
            return false;

        bool changed = false;
        if (node.hasProperty(kApvtsParamIdProp)
            && node.getProperty(kApvtsParamIdProp).toString() == parameterId)
        {
            node.setProperty(kApvtsValueProp, juce::jlimit(0.0f, 1.0f, normalized), nullptr);
            changed = true;
        }

        for (int i = 0; i < node.getNumChildren(); ++i)
        {
            auto child = node.getChild(i);
            if (setParameterNormalizedRecursive(child, parameterId, normalized))
                changed = true;
        }

        return changed;
    }

    static void enforceArpSharedDefaultShapesInState(juce::AudioProcessorValueTreeState& vts,
                                                     juce::ValueTree& state)
    {
        if (!state.isValid())
            return;

        // Project rule:
        // - Shared Arp/Drum sequencers have deterministic "factory" first-step
        //   values so lane creation/reset behaves predictably across snapshots.
        for (int laneIndex = 0; laneIndex < Lanes::kNumLanes; ++laneIndex)
        {
            const auto lane = Lanes::fromIndex(laneIndex);
            for (int step = 1; step <= 32; ++step)
            {
                const auto applyChoice = [&](const juce::String& stepParamId, int targetChoice)
                {
                    auto* param = dynamic_cast<juce::AudioParameterChoice*>(vts.getParameter(stepParamId));
                    if (param == nullptr)
                        return;

                    const int maxChoice = juce::jmax(0, param->choices.size() - 1);
                    const int clampedChoice = juce::jlimit(0, maxChoice, targetChoice);
                    const float normalized = param->convertTo0to1((float) clampedChoice);
                    setParameterNormalizedRecursive(state, stepParamId, normalized);
                };

                const int directionTarget = (step == 1 ? 2 : 0); // Up, then Skip.
                const int gateTarget = (step == 1 ? 8 : 0);      // 99%, then Skip.
                const int grooveTarget = (step == 1 ? 8 : 0);    // "=", then Skip.
                const int velocityTarget = (step == 1 ? 4 : 0);  // "=", then Skip.

                applyChoice(ParamIDs::laneStep(ParamIDs::Base::arpDirectionSeqPrefix, step, lane), directionTarget);
                applyChoice(ParamIDs::laneStep(ParamIDs::Base::arpGateSeqPrefix, step, lane), gateTarget);
                applyChoice(ParamIDs::laneStep(ParamIDs::Base::arpGrooveSeqPrefix, step, lane), grooveTarget);
                applyChoice(ParamIDs::laneStep(ParamIDs::Base::arpVelocitySeqPrefix, step, lane), velocityTarget);
            }
        }
    }
}

//==============================================================================
// 1) Ctor / dtor
//------------------------------------------------------------------------------
PresetManager::PresetManager(juce::AudioProcessorValueTreeState& vts)
    : parameters(vts)
{
    //--------------------------------------------------------------------------
    // 1.1) Capture factory defaults (must happen before any DAW restore)
    //--------------------------------------------------------------------------
    factoryDefaultState = parameters.copyState();

    //--------------------------------------------------------------------------
    // 1.2) Build default preset tree and mirror
    //--------------------------------------------------------------------------
    root = juce::ValueTree(IDs::root);
    initialiseDefaultBanks();

    //--------------------------------------------------------------------------
    // 1.3) Optional global library restore from disk.
    //--------------------------------------------------------------------------
    const bool loadedFromDisk = loadUserLibraryFromDisk();

    //--------------------------------------------------------------------------
    // 1.4) Default selection (RT-safe, clamps use layout tables already published)
    //--------------------------------------------------------------------------
    if (!loadedFromDisk)
    {
        setCurrentBankRT(0);
        setCurrentPresetRT(0);
        setCurrentSnapshotRT(0);
        setCurrentVariantRT(Variant::X);

        // Persist selection into root properties for DAW save/restore.
        persistSelectionToTree();
        saveUserLibraryToDiskIfPossible();
    }

    //--------------------------------------------------------------------------
    // 1.5) Listen to parameters.state to detect "Pending" (user modifications)
    //--------------------------------------------------------------------------
    parameters.state.addListener(this);

    //--------------------------------------------------------------------------
    // 1.6) Push initial selection to UI (message thread)
    //--------------------------------------------------------------------------
    notifySelectionChangeIfNeeded();

   #if LOGS_ENABLED
    DBG_LOG("INIT", "PRESETMANAGER", "CTOR", "#000#",
            "Constructed. Default banks created, selection initialized, listener attached.");
   #endif
}

PresetManager::~PresetManager()
{
    // Cancel pending async updater callback first.
    cancelPendingUpdate();

    // Remove listener first (avoid callbacks during destruction).
    parameters.state.removeListener(this);

    // Safety: audio thread must never see a dangling pointer.
    layoutRT.store(nullptr, std::memory_order_release);

    // Layouts are owned by this instance and destroyed here (message thread).
    retiredLayouts.clear();
    layoutOwned.reset();

   #if LOGS_ENABLED
    DBG_LOG("LIFECYCLE", "PRESETMANAGER", "DTOR", "#001#",
            "Destroyed. Listener removed, async updates canceled, RT layout pointer cleared.");
   #endif
}

//==============================================================================
// 2) AsyncUpdater bridge (RT -> message thread selection notifications)
//------------------------------------------------------------------------------
void PresetManager::triggerSelectionAsyncUpdate() noexcept
{
    // Safe from any thread in JUCE.
    triggerAsyncUpdate();
}

void PresetManager::handleAsyncUpdate()
{
    // Message thread only.
    // Consume selectionDirty and notify UI if needed.
    if (!consumeSelectionDirty())
        return;

    // Keep persisted selection coherent for DAW save/restore.
    persistSelectionToTree();

    // Notify UI listeners (MainMenu etc).
    notifySelectionChangeIfNeeded();

   #if LOGS_ENABLED
    DBG_LOG("ASYNC", "PRESETMANAGER", "SELECTION", "#005#",
            "Async selection update -> "
            "B=" + juce::String(getCurrentBank()) +
            " P=" + juce::String(getCurrentPreset()) +
            " S=" + juce::String(getCurrentSnapshot()) +
            " V=" + (getCurrentVariant() == Variant::X ? "X" : "Y"));
   #endif
}

//==============================================================================
// 3) Global persistent state (DAW save/restore)
//------------------------------------------------------------------------------
juce::ValueTree PresetManager::getState() const
{
   #if LOGS_ENABLED
    DBG_LOG("STATE", "PRESETMANAGER", "GETSTATE", "#010#",
            "Returning deep copy of root preset tree.");
   #endif

    return root.createCopy();
}

void PresetManager::loadState(const juce::ValueTree& vt)
{
    // loadState() is the normalization gateway for all persisted data:
    // - DAW chunk restore
    // - XML import
    // - disk library load/migration
    //
    // Invariants after this function:
    // 1) root has baseline BANK/PRESET structure.
    // 2) snapshots mirror matches normalized structure.
    // 3) RT layout tables are republished from normalized structure.
    // 4) atomics contain clamped selection values.

    //--------------------------------------------------------------------------
    // Validate input
    //--------------------------------------------------------------------------
    if (!vt.isValid() || !vt.hasType(IDs::root))
    {
       #if LOGS_ENABLED
        DBG_LOG("ERROR", "PRESETMANAGER", "LOADSTATE", "#011E#",
                "Invalid ValueTree passed to loadState().");
       #endif
        return;
    }

    //--------------------------------------------------------------------------
    // During loadState, we must NOT trigger Pending (this is a restore/apply path).
    //--------------------------------------------------------------------------
    const juce::ScopedValueSetter<bool> restoringGuard(isRestoringOrSaving, true);

    //--------------------------------------------------------------------------
    // Copy tree (deep copy to detach from caller)
    //--------------------------------------------------------------------------
    root = vt.createCopy();

    // Ensure baseline capacity is always present:
    // - kNumBanksDefault banks
    // - kNumPresetsDefault presets per bank
    // - kNumSnapshotsDefault snapshots addressable per preset (sparse storage)
    while (root.getNumChildren() > kNumBanksDefault)
        root.removeChild(root.getNumChildren() - 1, nullptr);

    for (int b = root.getNumChildren(); b < kNumBanksDefault; ++b)
        root.addChild(createDefaultBankNode(getBankDirectoryName(b), kNumPresetsDefault), -1, nullptr);

    const int banksToNormalize = juce::jmin(root.getNumChildren(), kNumBanksDefault);
    for (int b = 0; b < banksToNormalize; ++b)
    {
        juce::ValueTree bankNode = root.getChild(b);
        if (!bankNode.isValid())
            continue;

        if (!bankNode.hasType(IDs::bank))
            continue;

        while (bankNode.getNumChildren() > kNumPresetsDefault)
            bankNode.removeChild(bankNode.getNumChildren() - 1, nullptr);

        for (int p = bankNode.getNumChildren(); p < kNumPresetsDefault; ++p)
        {
            const juce::String presetLabel = "Preset " + juce::String(p + 1).paddedLeft('0', 3);
            bankNode.addChild(createDefaultPresetNode(presetLabel), -1, nullptr);
        }

        const int presetsToNormalize = juce::jmin(bankNode.getNumChildren(), kNumPresetsDefault);
        for (int p = 0; p < presetsToNormalize; ++p)
        {
            juce::ValueTree presetNode = bankNode.getChild(p);
            if (!presetNode.isValid() || !presetNode.hasType(IDs::preset))
                continue;

            for (int c = presetNode.getNumChildren() - 1; c >= 0; --c)
            {
                juce::ValueTree snapshotNode = presetNode.getChild(c);
                if (!snapshotNode.isValid() || !snapshotNode.hasType(IDs::snapshot))
                {
                    presetNode.removeChild(c, nullptr);
                    continue;
                }

                const int snapIdx = (int) snapshotNode.getProperty(IDs::snapshotIndex, c);
                if (snapIdx < 0 || snapIdx >= kNumSnapshotsDefault)
                {
                    presetNode.removeChild(c, nullptr);
                    continue;
                }

                for (Variant v : { Variant::X, Variant::Y })
                {
                    const juce::Identifier variantId = (v == Variant::X ? IDs::snapshotX : IDs::snapshotY);
                    juce::ValueTree variantNode = snapshotNode.getChildWithName(variantId);
                    if (!variantNode.isValid())
                    {
                        variantNode = juce::ValueTree(variantId);
                        snapshotNode.addChild(variantNode, -1, nullptr);
                    }

                    if (!variantNode.hasProperty(IDs::name))
                        variantNode.setProperty(IDs::name, getSnapshotDisplayName(snapIdx, v), nullptr);

                    if (!variantNode.hasProperty(IDs::activeLaneCount))
                        variantNode.setProperty(IDs::activeLaneCount, 1, nullptr);
                }
            }

            presetNode.setProperty(IDs::snapshotCount, kNumSnapshotsDefault, nullptr);
        }
    }

    //--------------------------------------------------------------------------
    // Resize mirror to match imported structure (banks/presets/snapshots)
    //--------------------------------------------------------------------------
    {
        snapshots.clear();
        snapshots.resize((size_t) juce::jmax(0, root.getNumChildren()));

        const int numBanksRaw = root.getNumChildren();

        for (int b = 0; b < numBanksRaw; ++b)
        {
            const juce::ValueTree bankNode = root.getChild(b);

            if (!bankNode.isValid() || !bankNode.hasType(IDs::bank))
            {
                snapshots[(size_t) b].clear();
                continue;
            }

            const int numPresetsRaw = bankNode.getNumChildren();
            snapshots[(size_t) b].resize((size_t) juce::jmax(0, numPresetsRaw));

            for (int p = 0; p < numPresetsRaw; ++p)
            {
                juce::ValueTree presetNode = bankNode.getChild(p);

                if (!presetNode.isValid() || !presetNode.hasType(IDs::preset))
                {
                    snapshots[(size_t) b][(size_t) p].clear();
                    continue;
                }

                int numSnapsRaw = (int) presetNode.getProperty(IDs::snapshotCount, kNumSnapshotsDefault);
                numSnapsRaw = juce::jlimit(1, kNumSnapshotsDefault, numSnapsRaw);
                presetNode.setProperty(IDs::snapshotCount, numSnapsRaw, nullptr);
                snapshots[(size_t) b][(size_t) p].resize((size_t) juce::jmax(0, numSnapsRaw));
            }
        }
    }

    //--------------------------------------------------------------------------
    // Resync mirror from tree (with fallback to factory defaults)
    //--------------------------------------------------------------------------
    resyncSlotsFromTree();

    //--------------------------------------------------------------------------
    // Publish new layout tables (RT clamps)
    //--------------------------------------------------------------------------
    rebuildAndPublishLayoutTables();

    //--------------------------------------------------------------------------
    // Restore persisted selection (with safe defaults)
    //--------------------------------------------------------------------------
    const int b = (int) root.getProperty(IDs::currentBank, 0);
    const int p = (int) root.getProperty(IDs::currentPreset, 0);
    const int s = (int) root.getProperty(IDs::currentSnapshot, 0);
    const int v = (int) root.getProperty(IDs::currentVariant, variantToInt(Variant::X));

    // Re-clamp using RT tables and store into atomics
    setCurrentBankRT(b);
    setCurrentPresetRT(p);
    setCurrentSnapshotRT(s);
    setCurrentVariantRT(intToVariant(v));

    // Persist clamped selection back to tree to keep it consistent
    persistSelectionToTree();

    // This restore result is considered saved/idle.
    markSaved();

    // Force UI refresh even if lastNotified* matches old values.
    lastNotifiedBank     = -1;
    lastNotifiedPreset   = -1;
    lastNotifiedSnapshot = -1;
    lastNotifiedVariant  = -1;

    notifySelectionChangeIfNeeded();

   #if LOGS_ENABLED
    DBG_LOG("STATE", "PRESETMANAGER", "LOADSTATE", "#012#",
            "Loaded state -> "
            "B=" + juce::String(getCurrentBank()) +
            " P=" + juce::String(getCurrentPreset()) +
            " S=" + juce::String(getCurrentSnapshot()) +
            " V=" + (getCurrentVariant() == Variant::X ? "X" : "Y"));
   #endif
}

//==============================================================================
// 4) ValueTree <-> mirror sync helpers (non-RT)
//------------------------------------------------------------------------------
juce::ValueTree PresetManager::getVariantNode(int bank, int preset, int snap, Variant which) const
{
    if (!isValid(bank, preset, snap))
        return {};

    const juce::ValueTree bankNode = root.getChild(bank);
    const juce::ValueTree presetNode = bankNode.getChild(preset);

    juce::ValueTree snapshotNode;
    for (int i = 0; i < presetNode.getNumChildren(); ++i)
    {
        const juce::ValueTree child = presetNode.getChild(i);
        if (!child.isValid() || !child.hasType(IDs::snapshot))
            continue;

        if ((int) child.getProperty(IDs::snapshotIndex, i) == snap)
        {
            snapshotNode = child;
            break;
        }
    }

    if (!snapshotNode.isValid())
        return {};

    const juce::Identifier variantId = (which == Variant::X ? IDs::snapshotX : IDs::snapshotY);
    return snapshotNode.getChildWithName(variantId);
}

juce::ValueTree PresetManager::getOrCreateVariantNode(int bank, int preset, int snap, Variant which)
{
    if (!isValid(bank, preset, snap))
        return {};

    juce::ValueTree bankNode = root.getChild(bank);
    juce::ValueTree presetNode = bankNode.getChild(preset);

    juce::ValueTree snapshotNode;
    for (int i = 0; i < presetNode.getNumChildren(); ++i)
    {
        const juce::ValueTree child = presetNode.getChild(i);
        if (!child.isValid() || !child.hasType(IDs::snapshot))
            continue;

        if ((int) child.getProperty(IDs::snapshotIndex, i) == snap)
        {
            snapshotNode = child;
            break;
        }
    }

    if (!snapshotNode.isValid())
    {
        snapshotNode = createDefaultSnapshotNode(snap);
        presetNode.addChild(snapshotNode, -1, nullptr);
    }

    const int currentCount = juce::jlimit(1, kNumSnapshotsDefault,
                                          (int) presetNode.getProperty(IDs::snapshotCount, kNumSnapshotsDefault));
    if (snap >= currentCount)
        presetNode.setProperty(IDs::snapshotCount, juce::jlimit(1, kNumSnapshotsDefault, snap + 1), nullptr);

    const juce::Identifier variantId = (which == Variant::X ? IDs::snapshotX : IDs::snapshotY);
    juce::ValueTree varNode = snapshotNode.getChildWithName(variantId);
    if (!varNode.isValid())
    {
        varNode = juce::ValueTree(variantId);
        snapshotNode.addChild(varNode, -1, nullptr);
    }

    return varNode;
}

void PresetManager::writeStateIntoTree(int bank, int preset, int snap,
                                       Variant which, const juce::ValueTree& st)
{
    if (!isValid(bank, preset, snap) || !st.isValid())
        return;

    juce::ValueTree varNode = getOrCreateVariantNode(bank, preset, snap, which);
    if (!varNode.isValid())
        return;

    // Replace the previous stored parameters subtree (if any)
    if (juce::ValueTree oldState = varNode.getChildWithName(parameters.state.getType()); oldState.isValid())
        varNode.removeChild(oldState, nullptr);

    varNode.addChild(st.createCopy(), -1, nullptr);

   #if LOGS_ENABLED
    DBG_LOG("SYNC", "PRESETMANAGER", "WRITE", "#020#",
            "Write snapshot state -> B=" + juce::String(bank) +
            " P=" + juce::String(preset) +
            " S=" + juce::String(snap) +
            " V=" + (which == Variant::X ? "X" : "Y"));
   #endif
}

juce::ValueTree PresetManager::readStateFromTree(int bank, int preset, int snap, Variant which) const
{
    if (!isValid(bank, preset, snap))
        return {};

    const juce::ValueTree varNode = getVariantNode(bank, preset, snap, which);
    if (!varNode.isValid())
        return {};

    const juce::ValueTree child = varNode.getChildWithName(parameters.state.getType());
    if (child.isValid())
        return child;

    return {};
}

void PresetManager::writeUiStateIntoTree(int bank, int preset, int snap, Variant which, const juce::ValueTree& uiState)
{
    if (!uiState.isValid())
        return;

    juce::ValueTree varNode = getOrCreateVariantNode(bank, preset, snap, which);
    if (!varNode.isValid())
        return;

    if (juce::ValueTree oldUiState = varNode.getChildWithName(IDs::uiState); oldUiState.isValid())
        varNode.removeChild(oldUiState, nullptr);

    varNode.addChild(uiState.createCopy(), -1, nullptr);
}

juce::ValueTree PresetManager::readUiStateFromTree(int bank, int preset, int snap, Variant which) const
{
    const juce::ValueTree varNode = getVariantNode(bank, preset, snap, which);
    if (!varNode.isValid())
        return {};

    return varNode.getChildWithName(IDs::uiState);
}

void PresetManager::clearUiStateInTree(int bank, int preset, int snap, Variant which)
{
    juce::ValueTree varNode = getOrCreateVariantNode(bank, preset, snap, which);
    if (!varNode.isValid())
        return;

    if (juce::ValueTree oldUiState = varNode.getChildWithName(IDs::uiState); oldUiState.isValid())
        varNode.removeChild(oldUiState, nullptr);
}

void PresetManager::writeActiveLaneCountIntoTree(int bank, int preset, int snap, Variant which, int laneCount)
{
    juce::ValueTree varNode = getOrCreateVariantNode(bank, preset, snap, which);
    if (!varNode.isValid())
        return;

    varNode.setProperty(IDs::activeLaneCount, juce::jlimit(1, 16, laneCount), nullptr);
}

int PresetManager::readActiveLaneCountFromTree(int bank, int preset, int snap, Variant which, int fallback) const
{
    const juce::ValueTree varNode = getVariantNode(bank, preset, snap, which);
    if (!varNode.isValid() || !varNode.hasProperty(IDs::activeLaneCount))
        return fallback;

    return juce::jlimit(1, 16, (int) varNode.getProperty(IDs::activeLaneCount, fallback));
}

void PresetManager::clearActiveLaneCountInTree(int bank, int preset, int snap, Variant which)
{
    juce::ValueTree varNode = getOrCreateVariantNode(bank, preset, snap, which);
    if (!varNode.isValid())
        return;

    varNode.removeProperty(IDs::activeLaneCount, nullptr);
}

void PresetManager::resyncSlotsFromTree()
{
    // Sparse strategy:
    // - Clear mirror slots to "empty" (invalid ValueTree).
    // - Rehydrate only snapshots explicitly stored in the tree.
    // - Missing snapshots are interpreted as factory-default on recall.
    //
    // This keeps serialized state compact while preserving deterministic recall.
    for (int b = 0; b < (int) snapshots.size(); ++b)
    {
        for (int p = 0; p < (int) snapshots[(size_t) b].size(); ++p)
        {
            for (int s = 0; s < (int) snapshots[(size_t) b][(size_t) p].size(); ++s)
            {
                auto& slot = snapshots[(size_t) b][(size_t) p][(size_t) s];
                slot.stateX = {};
                slot.stateY = {};
            }
        }
    }

    const int banksToSave = juce::jmin(root.getNumChildren(), kNumBanksDefault);
    for (int b = 0; b < banksToSave; ++b)
    {
        const juce::ValueTree bankNode = root.getChild(b);
        if (!bankNode.isValid() || !bankNode.hasType(IDs::bank))
            continue;

        const int presetsToSave = juce::jmin(bankNode.getNumChildren(), kNumPresetsDefault);
        for (int p = 0; p < presetsToSave; ++p)
        {
            const juce::ValueTree presetNode = bankNode.getChild(p);
            if (!presetNode.isValid() || !presetNode.hasType(IDs::preset))
                continue;

            for (int c = 0; c < presetNode.getNumChildren(); ++c)
            {
                const juce::ValueTree snapshotNode = presetNode.getChild(c);
                if (!snapshotNode.isValid() || !snapshotNode.hasType(IDs::snapshot))
                    continue;

                const int s = juce::jlimit(0,
                                           juce::jmax(0, (int) snapshots[(size_t) b][(size_t) p].size() - 1),
                                           (int) snapshotNode.getProperty(IDs::snapshotIndex, c));

                if (!isValid(b, p, s))
                    continue;

                juce::ValueTree stX = readStateFromTree(b, p, s, Variant::X);
                juce::ValueTree stY = readStateFromTree(b, p, s, Variant::Y);

                auto& slot = snapshots[(size_t) b][(size_t) p][(size_t) s];
                if (stX.isValid())
                    slot.stateX = stX.createCopy();
                if (stY.isValid())
                    slot.stateY = stY.createCopy();
            }
        }
    }

   #if LOGS_ENABLED
    DBG_LOG("SYNC", "PRESETMANAGER", "RESYNC", "#021#",
            "Mirror resynced from tree (sparse).");
   #endif
}

void PresetManager::persistSelectionToTree()
{
    root.setProperty(IDs::currentBank,     getCurrentBank(),     nullptr);
    root.setProperty(IDs::currentPreset,   getCurrentPreset(),   nullptr);
    root.setProperty(IDs::currentSnapshot, getCurrentSnapshot(), nullptr);
    root.setProperty(IDs::currentVariant,  variantToInt(getCurrentVariant()), nullptr);

   #if LOGS_ENABLED
    DBG_LOG("STATE", "PRESETMANAGER", "PERSIST", "#022#",
            "Persist selection -> "
            "B=" + juce::String(getCurrentBank()) +
            " P=" + juce::String(getCurrentPreset()) +
            " S=" + juce::String(getCurrentSnapshot()) +
            " V=" + (getCurrentVariant() == Variant::X ? "X" : "Y"));
   #endif
}

void PresetManager::replaceParametersStateGuarded(const juce::ValueTree& newState)
{
    if (!newState.isValid())
        return;

    // IMPORTANT:
    // - This is an APPLY operation (recall/restore).
    // - It must NEVER share the same ValueTree instance with stored snapshots,
    //   otherwise editing parameters would mutate the stored snapshot too.
    const juce::ScopedValueSetter<bool> pendingGuard(isRestoringOrSaving, true);

    const juce::ValueTree safeCopy = newState.createCopy();

    // Listener is temporarily detached to avoid self-triggering pending state
    // while we are performing an explicit restore/apply.
    parameters.state.removeListener(this);
    parameters.replaceState(safeCopy);
    parameters.state.addListener(this);

    // After an APPLY, runtime matches the loaded state -> not pending.
    markSaved();
}

void PresetManager::requestGlobalAllNotesOffIfAvailable()
{
    if (!onGlobalAllNotesOff)
        return;

    // We want the callback to run on message thread (UI world),
    // but it will only set an atomic flag in the processor anyway.
    if (juce::MessageManager::getInstance()->isThisTheMessageThread())
        onGlobalAllNotesOff();
    else
        juce::MessageManager::callAsync([cb = onGlobalAllNotesOff] { cb(); });
}

//==============================================================================
// 5) Capture / load snapshots (non-RT)
//------------------------------------------------------------------------------
void PresetManager::captureSnapshot(int bank, int preset, int snap, Variant which)
{
    if (!isValid(bank, preset, snap))
        return;
    
    // Safety: prevent stuck notes when changing snapshot.
    requestGlobalAllNotesOffIfAvailable();

    // Capture a copy of current plugin parameters
    const juce::ValueTree currentState = parameters.copyState().createCopy();

    auto& slot = snapshots[(size_t) bank][(size_t) preset][(size_t) snap];

    if (which == Variant::X)
    {
        slot.stateX = currentState;

        // If Y has never been captured, make it follow X by default,
        // but as a deep copy (no shared instance).
        if (!slot.stateY.isValid())
            slot.stateY = currentState.createCopy();
    }
    else
    {
        slot.stateY = currentState;
    }

    // Persist into root tree storage
    writeStateIntoTree(bank, preset, snap, which, currentState);

    if (onSnapshotLaneCountCapture)
        writeActiveLaneCountIntoTree(bank, preset, snap, which, onSnapshotLaneCountCapture());

    if (onSnapshotUiCapture)
    {
        const juce::ValueTree uiState = onSnapshotUiCapture();
        if (uiState.isValid())
            writeUiStateIntoTree(bank, preset, snap, which, uiState);
    }

    // Capture makes storage match runtime -> clear pending latch (Idle)
    if (!isRestoringOrSaving)
        markSaved();

    saveUserLibraryToDiskIfPossible();

   #if LOGS_ENABLED
    DBG_LOG("ACTION", "PRESETMANAGER", "CAPTURE", "#030#",
            "Captured snapshot -> "
            "B=" + juce::String(bank) +
            " P=" + juce::String(preset) +
            " S=" + juce::String(snap) +
            " V=" + (which == Variant::X ? "X" : "Y"));
   #endif
}

void PresetManager::loadSnapshot(int bank, int preset, int snap, Variant which)
{
    if (!isValid(bank, preset, snap))
        return;

    auto& slot = snapshots[(size_t) bank][(size_t) preset][(size_t) snap];

    // Select source (mirror first, fallback to tree).
    // Mirror is expected hot path; tree read is recovery path.
    juce::ValueTree stateToLoad;

    if (which == Variant::X)
        stateToLoad = slot.stateX;
    else
        stateToLoad = slot.stateY.isValid() ? slot.stateY : slot.stateX;

    if (!stateToLoad.isValid())
        stateToLoad = readStateFromTree(bank, preset, snap, which);

    if (!stateToLoad.isValid())
    {
        // Fallback: empty snapshot always loads factory defaults.
        // This avoids partial/undefined runtime state after sparse restores.
        const juce::ValueTree defSrc = factoryDefaultState.isValid()
                                     ? factoryDefaultState
                                     : parameters.copyState();

        stateToLoad = defSrc;
    }

    // APPLY operation: replace runtime state (guarded)
    replaceParametersStateGuarded(stateToLoad);

    if (onSnapshotLaneCountApply)
    {
        const int activeLanes = readActiveLaneCountFromTree(bank, preset, snap, which, 1);
        onSnapshotLaneCountApply(activeLanes);
    }

    if (onSnapshotUiApply)
    {
        const juce::ValueTree uiState = readUiStateFromTree(bank, preset, snap, which);
        onSnapshotUiApply(uiState.isValid() ? uiState.createCopy() : juce::ValueTree{});
    }

    // Optional hook: ensure MorphParameterManager mirror follows new parameter values.
    if (morphManager != nullptr)
    {
        morphManager->syncFromValueTree();

       #if LOGS_ENABLED
        DBG_LOG("SYNC", "PRESETMANAGER", "MORPH_HOOK", "#031H#",
                "MorphParameterManager syncFromValueTree() after snapshot load.");
       #endif
    }
    else
    {
       #if LOGS_ENABLED
        DBG_LOG("WARN", "PRESETMANAGER", "MORPH_HOOK", "#031W#",
                "MorphParameterManager is null; skipping morph sync.");
       #endif
    }

   #if LOGS_ENABLED
    DBG_LOG("ACTION", "PRESETMANAGER", "LOAD", "#031#",
            "Loaded snapshot -> "
            "B=" + juce::String(bank) +
            " P=" + juce::String(preset) +
            " S=" + juce::String(snap) +
            " V=" + (which == Variant::X ? "X" : "Y"));
   #endif
}

void PresetManager::replaceParametersStateFromDaw(const juce::ValueTree& newState)
{
    // DAW restore is an APPLY operation and must be guarded.
    replaceParametersStateGuarded(newState);
}

void PresetManager::captureCurrent(Variant which)
{
    const int b = getCurrentBank();
    const int p = getCurrentPreset();
    const int s = getCurrentSnapshot();

    captureSnapshot(b, p, s, which);

   #if LOGS_ENABLED
    DBG_LOG("ACTION", "PRESETMANAGER", "CAPTURE_CURRENT", "#032#",
            "Captured current snapshot -> "
            "B=" + juce::String(b) +
            " P=" + juce::String(p) +
            " S=" + juce::String(s) +
            " V=" + (which == Variant::X ? "X" : "Y"));
   #endif
}

void PresetManager::loadCurrentSnapshot(Variant which)
{
    const int b = getCurrentBank();
    const int p = getCurrentPreset();
    const int s = getCurrentSnapshot();

    loadSnapshot(b, p, s, which);

   #if LOGS_ENABLED
    DBG_LOG("ACTION", "PRESETMANAGER", "LOAD_CURRENT", "#033#",
            "Loaded current snapshot -> "
            "B=" + juce::String(b) +
            " P=" + juce::String(p) +
            " S=" + juce::String(s) +
            " V=" + (which == Variant::X ? "X" : "Y"));
   #endif
}

void PresetManager::copySnapshot(int srcBank, int srcPreset, int srcSnap,
                                 int dstBank, int dstPreset, int dstSnap,
                                 Variant which)
{
    if (!isValid(srcBank, srcPreset, srcSnap) || !isValid(dstBank, dstPreset, dstSnap))
        return;

    auto& src = snapshots[(size_t) srcBank][(size_t) srcPreset][(size_t) srcSnap];
    auto& dst = snapshots[(size_t) dstBank][(size_t) dstPreset][(size_t) dstSnap];

    // Choose source (mirror first, fallback tree)
    juce::ValueTree st;

    if (which == Variant::X)
        st = src.stateX;
    else
        st = src.stateY.isValid() ? src.stateY : src.stateX;

    if (!st.isValid())
        st = readStateFromTree(srcBank, srcPreset, srcSnap, which);

    if (!st.isValid())
        return;

    // STORE-ONLY: always store a deep copy (no shared instances)
    const juce::ValueTree stCopy = st.createCopy();

    if (which == Variant::X)
        dst.stateX = stCopy;
    else
        dst.stateY = stCopy;

    writeStateIntoTree(dstBank, dstPreset, dstSnap, which, stCopy);

    const int srcLaneCount = readActiveLaneCountFromTree(srcBank, srcPreset, srcSnap, which, -1);
    if (srcLaneCount > 0)
        writeActiveLaneCountIntoTree(dstBank, dstPreset, dstSnap, which, srcLaneCount);
    else
        clearActiveLaneCountInTree(dstBank, dstPreset, dstSnap, which);

    const juce::ValueTree srcUiState = readUiStateFromTree(srcBank, srcPreset, srcSnap, which);
    if (srcUiState.isValid())
        writeUiStateIntoTree(dstBank, dstPreset, dstSnap, which, srcUiState);
    else
        clearUiStateInTree(dstBank, dstPreset, dstSnap, which);

    if (!isRestoringOrSaving)
        triggerPendingIfNeeded();

    saveUserLibraryToDiskIfPossible();

   #if LOGS_ENABLED
    DBG_LOG("ACTION", "PRESETMANAGER", "COPY_SNAPSHOT", "#034#",
            "Copied snapshot -> "
            "src=(" + juce::String(srcBank) + "," + juce::String(srcPreset) + "," + juce::String(srcSnap) + ") "
            "dst=(" + juce::String(dstBank) + "," + juce::String(dstPreset) + "," + juce::String(dstSnap) + ") "
            "V=" + (which == Variant::X ? "X" : "Y"));
   #endif
}

void PresetManager::resetSnapshotToDefaults(int bank, int preset, int snap, Variant which)
{
    if (!isValid(bank, preset, snap))
        return;
    
    requestGlobalAllNotesOffIfAvailable();

    // Factory default must be stored as a deep copy (no shared instances).
    const juce::ValueTree defSrc = factoryDefaultState.isValid()
                                 ? factoryDefaultState
                                 : parameters.copyState();

    juce::ValueTree def = defSrc.createCopy();
    enforceArpSharedDefaultShapesInState(parameters, def);

    auto& slot = snapshots[(size_t) bank][(size_t) preset][(size_t) snap];

    // STORE-ONLY: write mirror and tree, do not touch parameters.state
    if (which == Variant::X)
        slot.stateX = def;
    else
        slot.stateY = def;

    writeStateIntoTree(bank, preset, snap, which, def);
    writeActiveLaneCountIntoTree(bank, preset, snap, which, 1);
    clearUiStateInTree(bank, preset, snap, which);

    if (!isRestoringOrSaving)
        triggerPendingIfNeeded();

    saveUserLibraryToDiskIfPossible();

   #if LOGS_ENABLED
    DBG_LOG("ACTION", "PRESETMANAGER", "RESET_SNAPSHOT", "#035#",
            "Reset snapshot to defaults -> "
            "dst=(" + juce::String(bank) + "," + juce::String(preset) + "," + juce::String(snap) + ") "
            "V=" + (which == Variant::X ? "X" : "Y"));
   #endif
}

//==============================================================================
// 6) Selection (UI / message thread)
//------------------------------------------------------------------------------
void PresetManager::setCurrentBank(int idx)
{
    setCurrentBankRT(idx);
    setCurrentPresetRT(getCurrentPreset());
    setCurrentSnapshotRT(getCurrentSnapshot());

    // Ensure UI reacts even if selection changes from RT path too.
    triggerSelectionAsyncUpdate();
}

void PresetManager::setCurrentPreset(int idx)
{
    setCurrentPresetRT(idx);
    setCurrentSnapshotRT(getCurrentSnapshot());
    triggerSelectionAsyncUpdate();
}

void PresetManager::setCurrentSnapshot(int idx)
{
    setCurrentSnapshotRT(idx);
    triggerSelectionAsyncUpdate();
}

void PresetManager::setCurrentVariant(Variant v)
{
    setCurrentVariantRT(v);
    triggerSelectionAsyncUpdate();
}

int PresetManager::getCurrentBank() const noexcept
{
    return currentBank.load(std::memory_order_relaxed);
}

int PresetManager::getCurrentPreset() const noexcept
{
    return currentPreset.load(std::memory_order_relaxed);
}

int PresetManager::getCurrentSnapshot() const noexcept
{
    return currentSnapshot.load(std::memory_order_relaxed);
}

PresetManager::Variant PresetManager::getCurrentVariant() const noexcept
{
    return intToVariant(currentVariantI.load(std::memory_order_relaxed));
}

juce::ValueTree PresetManager::getSnapshotUiState(int bank, int preset, int snap, Variant which) const
{
    const juce::ValueTree state = readUiStateFromTree(bank, preset, snap, which);
    return state.isValid() ? state.createCopy() : juce::ValueTree {};
}

juce::ValueTree PresetManager::getCurrentSnapshotUiState() const
{
    return getSnapshotUiState(getCurrentBank(),
                              getCurrentPreset(),
                              getCurrentSnapshot(),
                              getCurrentVariant());
}

//==============================================================================
// 7) MIDI mapping (UI / message thread)
//------------------------------------------------------------------------------
void PresetManager::setBankFromMsb(int msb)
{
    setCurrentBank(clampMidi7Bit(msb));

   #if LOGS_ENABLED
    DBG_LOG("MIDI", "PRESETMANAGER", "BANK_MSB", "#040#",
            "Received CC#0 -> Bank=" + juce::String(msb));
   #endif
}

void PresetManager::setPresetFromLsb(int lsb)
{
    setCurrentPreset(clampMidi7Bit(lsb));

   #if LOGS_ENABLED
    DBG_LOG("MIDI", "PRESETMANAGER", "PRESET_LSB", "#041#",
            "Received CC#32 -> Preset=" + juce::String(lsb));
   #endif
}

void PresetManager::selectSnapshotFromProgramChange(int pc)
{
    const int requested = juce::jmax(0, pc - 1);

    setCurrentSnapshot(requested);

    // APPLY on current variant (message thread). In the full architecture,
    // this may be delegated to SnapshotRecallAsync from the processor.
    loadCurrentSnapshot(getCurrentVariant());

   #if LOGS_ENABLED
    DBG_LOG("MIDI", "PRESETMANAGER", "SNAPSHOT_PC", "#042#",
            "Received Program Change -> pc=" + juce::String(pc) +
            " idx=" + juce::String(getCurrentSnapshot()));
   #endif
}

//==============================================================================
// 7.1) Selection change callback plumbing
//------------------------------------------------------------------------------
void PresetManager::setOnSelectionChange(SelectionChangeFn fn)
{
    onSelectionChange = std::move(fn);

    // Force first push even if values match lastNotified* defaults.
    lastNotifiedBank     = -1;
    lastNotifiedPreset   = -1;
    lastNotifiedSnapshot = -1;
    lastNotifiedVariant  = -1;

    notifySelectionChangeIfNeeded();
}

void PresetManager::notifySelectionChangeUI()
{
    lastNotifiedBank     = -1;
    lastNotifiedPreset   = -1;
    lastNotifiedSnapshot = -1;
    lastNotifiedVariant  = -1;

    notifySelectionChangeIfNeeded();
}

void PresetManager::notifySelectionChangeIfNeeded()
{
    const int b = getCurrentBank();
    const int p = getCurrentPreset();
    const int s = getCurrentSnapshot();
    const int v = variantToInt(getCurrentVariant());

    if (b == lastNotifiedBank &&
        p == lastNotifiedPreset &&
        s == lastNotifiedSnapshot &&
        v == lastNotifiedVariant)
        return;

    lastNotifiedBank     = b;
    lastNotifiedPreset   = p;
    lastNotifiedSnapshot = s;
    lastNotifiedVariant  = v;

    if (onSelectionChange)
        onSelectionChange(b, p, s, v);
}

//==============================================================================
// 8) Export / import XML (non-RT)
//------------------------------------------------------------------------------
juce::Result PresetManager::exportToFile(const juce::File& targetFile) const
{
   #if LOGS_ENABLED
    DBG_LOG("ACTION", "PRESETMANAGER", "EXPORT", "#200#",
            "Export -> " + targetFile.getFullPathName());
   #endif

    const juce::File parentDir = targetFile.getParentDirectory();
    if (!parentDir.createDirectory().wasOk())
        return juce::Result::fail("Unable to create destination directory.");

    if (auto xml = getState().createXml())
    {
        if (!xml->writeTo(targetFile))
        {
           #if LOGS_ENABLED
            DBG_LOG("ERROR", "PRESETMANAGER", "EXPORT_FAIL", "#200E#",
                    "XML write failed -> " + targetFile.getFileName());
           #endif
            return juce::Result::fail("XML write failed.");
        }

       #if LOGS_ENABLED
        DBG_LOG("SUCCESS", "PRESETMANAGER", "EXPORT_OK", "#201#",
                "Export OK -> " + targetFile.getFileName());
       #endif

        return juce::Result::ok();
    }

   #if LOGS_ENABLED
    DBG_LOG("ERROR", "PRESETMANAGER", "EXPORT_FAIL", "#202E#",
            "ValueTree->XML conversion failed.");
   #endif

    return juce::Result::fail("ValueTree->XML conversion failed.");
}

juce::Result PresetManager::importFromFile(const juce::File& sourceFile)
{
   #if LOGS_ENABLED
    DBG_LOG("ACTION", "PRESETMANAGER", "IMPORT", "#210#",
            "Import -> " + sourceFile.getFullPathName());
   #endif

    if (!sourceFile.existsAsFile())
        return juce::Result::fail("File not found.");

    std::unique_ptr<juce::XmlElement> xml = juce::XmlDocument::parse(sourceFile);
    if (!xml || !xml->hasTagName(IDs::root))
        return juce::Result::fail("Invalid XML structure (wrong root tag).");

    const juce::ValueTree vt = juce::ValueTree::fromXml(*xml);
    if (!vt.isValid())
        return juce::Result::fail("XML->ValueTree conversion failed.");

    loadState(vt);
    saveUserLibraryToDiskIfPossible();

   #if LOGS_ENABLED
    DBG_LOG("SUCCESS", "PRESETMANAGER", "IMPORT_OK", "#211#",
            "Import OK. Banks=" + juce::String(getNumBanks()));
   #endif

    return juce::Result::ok();
}

//==============================================================================
// 9) Default structure creation (non-RT)
//------------------------------------------------------------------------------
void PresetManager::initialiseDefaultBanks()
{
    snapshots.clear();
    snapshots.resize((size_t) kNumBanksDefault);

    for (int b = 0; b < kNumBanksDefault; ++b)
    {
        snapshots[(size_t) b].resize((size_t) kNumPresetsDefault);

        for (int p = 0; p < kNumPresetsDefault; ++p)
            snapshots[(size_t) b][(size_t) p].resize((size_t) kNumSnapshotsDefault);
    }

    root.removeAllChildren(nullptr);

    for (int b = 0; b < kNumBanksDefault; ++b)
    {
        const juce::String name = getBankDirectoryName(b);
        root.addChild(createDefaultBankNode(name, kNumPresetsDefault), -1, nullptr);
    }

    rebuildAndPublishLayoutTables();

   #if LOGS_ENABLED
    DBG_LOG("INIT", "PRESETMANAGER", "DEFAULTS", "#300#",
            "Default structure created and snapshots prefilled.");
   #endif
}

juce::ValueTree PresetManager::createDefaultSnapshotNode(const int snapshotIndex)
{
    juce::ValueTree snapshot(IDs::snapshot);
    snapshot.setProperty(IDs::snapshotIndex, snapshotIndex, nullptr);

    for (Variant v : { Variant::X, Variant::Y })
    {
        const juce::Identifier varID = (v == Variant::X ? IDs::snapshotX : IDs::snapshotY);

        juce::ValueTree variantNode(varID);
        variantNode.setProperty(IDs::name, getSnapshotDisplayName(snapshotIndex, v), nullptr);
        variantNode.setProperty(IDs::activeLaneCount, 1, nullptr);
        snapshot.addChild(variantNode, -1, nullptr);
    }

    return snapshot;
}

juce::ValueTree PresetManager::createDefaultPresetNode(const juce::String& presetName)
{
    juce::ValueTree preset(IDs::preset);
    preset.setProperty(IDs::presetName, presetName, nullptr);
    preset.setProperty(IDs::snapshotCount, kNumSnapshotsDefault, nullptr);

    return preset;
}

juce::ValueTree PresetManager::createDefaultBankNode(const juce::String& bankName, int numPresets)
{
    juce::ValueTree bank(IDs::bank);
    bank.setProperty(IDs::bankName, bankName, nullptr);

    for (int i = 0; i < numPresets; ++i)
    {
        const juce::String presetLabel = "Preset " + juce::String(i + 1).paddedLeft('0', 3);
        bank.addChild(createDefaultPresetNode(presetLabel), -1, nullptr);
    }

    return bank;
}

//==============================================================================
// 10) Helpers / validation (UI)
//------------------------------------------------------------------------------
bool PresetManager::isValid(const int b, const int p, const int s) const
{
    return (b >= 0 && b < (int) snapshots.size())
        && (p >= 0 && p < (int) snapshots[(size_t) b].size())
        && (s >= 0 && s < (int) snapshots[(size_t) b][(size_t) p].size());
}

juce::String PresetManager::sanitiseName(const juce::String& input) const
{
    // ASCII policy: keep only letters/digits/spaces
    juce::String out;

    for (auto c : input)
        if (juce::CharacterFunctions::isLetterOrDigit(c) || c == ' ')
            out += c;

    return out.trim();
}

int PresetManager::getNumBanks() const
{
    return juce::jmin(root.getNumChildren(), kNumBanksDefault);
}

int PresetManager::getNumPresets(int bankIndex) const
{
    if (bankIndex < 0 || bankIndex >= getNumBanks())
        return 0;

    return juce::jmin(root.getChild(bankIndex).getNumChildren(), kNumPresetsDefault);
}

int PresetManager::getNumSnapshots(int bankIndex, int presetIndex) const
{
    if (bankIndex < 0 || bankIndex >= getNumBanks())
        return 0;

    const juce::ValueTree bankNode = root.getChild(bankIndex);

    if (presetIndex < 0 || presetIndex >= juce::jmin(bankNode.getNumChildren(), kNumPresetsDefault))
        return 0;

    const juce::ValueTree presetNode = bankNode.getChild(presetIndex);
    const int snapshotCount = (int) presetNode.getProperty(IDs::snapshotCount, presetNode.getNumChildren());
    return juce::jlimit(1, kNumSnapshotsDefault, snapshotCount);
}

int PresetManager::getNumProgramEntriesRTSafe() const noexcept
{
    const LayoutTables* lt = getLayoutRT();
    if (lt == nullptr)
        return 1;

    int total = 0;
    for (const int snaps : lt->snapsPerPreset)
        total += juce::jmax(0, snaps);

    return juce::jmax(1, total);
}

bool PresetManager::getProgramAddressForIndexRTSafe(int programIndex,
                                                    int& outBank,
                                                    int& outPreset,
                                                    int& outSnapshot) const noexcept
{
    const LayoutTables* lt = getLayoutRT();
    if (lt == nullptr || lt->numBanks <= 0)
    {
        outBank = 0;
        outPreset = 0;
        outSnapshot = 0;
        return false;
    }

    int idx = juce::jmax(0, programIndex);
    const int totalPrograms = getNumProgramEntriesRTSafe();
    if (totalPrograms > 0)
        idx = juce::jlimit(0, totalPrograms - 1, idx);

    for (int b = 0; b < lt->numBanks; ++b)
    {
        const int presets = (b >= 0 && b < (int) lt->presetsPerBank.size())
                          ? lt->presetsPerBank[(size_t) b]
                          : 0;

        const int base = (b >= 0 && b < (int) lt->bankPresetBase.size())
                       ? lt->bankPresetBase[(size_t) b]
                       : 0;

        for (int p = 0; p < presets; ++p)
        {
            const int flatPreset = base + p;
            const int snaps = (flatPreset >= 0 && flatPreset < (int) lt->snapsPerPreset.size())
                            ? juce::jmax(0, lt->snapsPerPreset[(size_t) flatPreset])
                            : 0;

            if (snaps <= 0)
                continue;

            if (idx < snaps)
            {
                outBank = b;
                outPreset = p;
                outSnapshot = idx;
                return true;
            }

            idx -= snaps;
        }
    }

    outBank = 0;
    outPreset = 0;
    outSnapshot = 0;
    return false;
}

int PresetManager::getProgramIndexForAddressRTSafe(int bank, int preset, int snapshot) const noexcept
{
    const LayoutTables* lt = getLayoutRT();
    if (lt == nullptr || lt->numBanks <= 0)
        return 0;

    const int b = clampBankRT(bank, lt);
    const int p = clampPresetRT(b, preset, lt);
    const int s = clampSnapshotRT(b, p, snapshot, lt);

    int idx = 0;

    for (int bb = 0; bb < b; ++bb)
    {
        const int presets = (bb >= 0 && bb < (int) lt->presetsPerBank.size())
                          ? lt->presetsPerBank[(size_t) bb]
                          : 0;
        const int base = (bb >= 0 && bb < (int) lt->bankPresetBase.size())
                       ? lt->bankPresetBase[(size_t) bb]
                       : 0;

        for (int pp = 0; pp < presets; ++pp)
        {
            const int flat = base + pp;
            if (flat >= 0 && flat < (int) lt->snapsPerPreset.size())
                idx += juce::jmax(0, lt->snapsPerPreset[(size_t) flat]);
        }
    }

    const int baseB = (b >= 0 && b < (int) lt->bankPresetBase.size())
                    ? lt->bankPresetBase[(size_t) b]
                    : 0;

    for (int pp = 0; pp < p; ++pp)
    {
        const int flat = baseB + pp;
        if (flat >= 0 && flat < (int) lt->snapsPerPreset.size())
            idx += juce::jmax(0, lt->snapsPerPreset[(size_t) flat]);
    }

    idx += juce::jmax(0, s);

    const int totalPrograms = getNumProgramEntriesRTSafe();
    return (totalPrograms > 0) ? juce::jlimit(0, totalPrograms - 1, idx) : 0;
}

juce::String PresetManager::getSnapshotDisplayName(int snapIndex, Variant which)
{
    juce::ignoreUnused(which);
    return "Snapshot " + juce::String(snapIndex + 1);
}

juce::File PresetManager::getUserLibraryBaseDirectory() const
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile(kPresetLibrarySubdir)
        .getChildFile(kPresetLibraryRootDir);
}

juce::File PresetManager::getUserLibraryIndexFilePath() const
{
    return getUserLibraryBaseDirectory().getChildFile(kPresetLibraryIndexFile);
}

juce::File PresetManager::getLegacyUserLibraryFilePath() const
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile(kPresetLibrarySubdir)
        .getChildFile(kPresetLibraryLegacyFile);
}

juce::String PresetManager::getBankDirectoryName(int bankIndex) const
{
    if (bankIndex >= 0 && bankIndex < 26)
        return "Bank " + juce::String::charToString((juce::juce_wchar) ('A' + bankIndex));

    return "Bank " + juce::String(bankIndex + 1).paddedLeft('0', 2);
}

juce::String PresetManager::getPresetDirectoryName(int presetIndex) const
{
    return "Preset " + juce::String(presetIndex + 1).paddedLeft('0', 3);
}

juce::String PresetManager::getSnapshotFileName(int snapshotIndex) const
{
    return "Snapshot " + juce::String(snapshotIndex + 1).paddedLeft('0', 3) + ".xml";
}

juce::File PresetManager::getBankDirectoryPath(const juce::File& baseDir, int bankIndex) const
{
    return baseDir.getChildFile(getBankDirectoryName(bankIndex));
}

juce::File PresetManager::getPresetDirectoryPath(const juce::File& baseDir, int bankIndex, int presetIndex) const
{
    return getBankDirectoryPath(baseDir, bankIndex).getChildFile(getPresetDirectoryName(presetIndex));
}

juce::File PresetManager::getSnapshotFilePath(const juce::File& baseDir,
                                              int bankIndex,
                                              int presetIndex,
                                              int snapshotIndex) const
{
    return getPresetDirectoryPath(baseDir, bankIndex, presetIndex)
        .getChildFile(getSnapshotFileName(snapshotIndex));
}

bool PresetManager::loadUserLibraryFromDisk()
{
    const juce::File baseDir = getUserLibraryBaseDirectory();
    const juce::File indexFile = getUserLibraryIndexFilePath();

    // New persistent layout:
    //   PresetLibrary/
    //     LibraryIndex.xml
    //     Bank A/Preset 001/Snapshot 001.xml
    //     ...
    //
    // Design note:
    // - Index file stores names + selection + snapshotCount.
    // - Snapshot files store the actual payload nodes.
    // - This split avoids rewriting one giant XML file on each edit.
    if (indexFile.existsAsFile())
    {
        std::unique_ptr<juce::XmlElement> indexXml = juce::XmlDocument::parse(indexFile);
        if (indexXml && indexXml->hasTagName(kPresetLibraryIndexTag))
        {
            const juce::ValueTree indexTree = juce::ValueTree::fromXml(*indexXml);
            if (indexTree.isValid())
            {
                juce::ValueTree loadedRoot(IDs::root);
                loadedRoot.setProperty(IDs::currentBank,
                                       (int) indexTree.getProperty(IDs::currentBank, 0),
                                       nullptr);
                loadedRoot.setProperty(IDs::currentPreset,
                                       (int) indexTree.getProperty(IDs::currentPreset, 0),
                                       nullptr);
                loadedRoot.setProperty(IDs::currentSnapshot,
                                       (int) indexTree.getProperty(IDs::currentSnapshot, 0),
                                       nullptr);
                loadedRoot.setProperty(IDs::currentVariant,
                                       (int) indexTree.getProperty(IDs::currentVariant, variantToInt(Variant::X)),
                                       nullptr);

                int bankOrdinal = 0;

                for (int bMetaIdx = 0; bMetaIdx < indexTree.getNumChildren(); ++bMetaIdx)
                {
                    const juce::ValueTree bankMeta = indexTree.getChild(bMetaIdx);
                    if (!bankMeta.isValid() || !bankMeta.hasType(kPresetLibraryBankMetaTag))
                        continue;

                    if (bankOrdinal >= kNumBanksDefault)
                        break;

                    const int b = bankOrdinal++;

                    juce::ValueTree bankNode(IDs::bank);
                    bankNode.setProperty(IDs::bankName,
                                         bankMeta.getProperty(kMetaName, getBankDirectoryName(b)).toString(),
                                         nullptr);

                    int presetOrdinal = 0;

                    for (int pMetaIdx = 0; pMetaIdx < bankMeta.getNumChildren(); ++pMetaIdx)
                    {
                        const juce::ValueTree presetMeta = bankMeta.getChild(pMetaIdx);
                        if (!presetMeta.isValid() || !presetMeta.hasType(kPresetLibraryPresetTag))
                            continue;

                        if (presetOrdinal >= kNumPresetsDefault)
                            break;

                        const int p = presetOrdinal++;
                        const int snapshotCount = juce::jlimit(
                            1,
                            kNumSnapshotsDefault,
                            (int) presetMeta.getProperty(kMetaSnapshotCount, kNumSnapshotsDefault));

                        juce::ValueTree presetNode(IDs::preset);
                        presetNode.setProperty(IDs::presetName,
                                               presetMeta.getProperty(kMetaName, getPresetDirectoryName(p)).toString(),
                                               nullptr);
                        presetNode.setProperty(IDs::snapshotCount, snapshotCount, nullptr);

                        const juce::File presetDir = getPresetDirectoryPath(baseDir, b, p);
                        if (presetDir.exists())
                        {
                            juce::Array<juce::File> snapshotFiles;
                            presetDir.findChildFiles(snapshotFiles,
                                                     juce::File::findFiles,
                                                     false,
                                                     "Snapshot *.xml");

                            for (const auto& snapshotFile : snapshotFiles)
                            {
                                const juce::String filename = snapshotFile.getFileNameWithoutExtension().trim();
                                const int spacePos = filename.lastIndexOfChar(' ');
                                if (spacePos < 0)
                                    continue;

                                const juce::String indexText = filename.substring(spacePos + 1).trim();
                                const int parsed = indexText.getIntValue();
                                if (parsed <= 0)
                                    continue;

                                const int s = juce::jlimit(0, snapshotCount - 1, parsed - 1);

                                std::unique_ptr<juce::XmlElement> snapshotXml = juce::XmlDocument::parse(snapshotFile);
                                if (!snapshotXml || !snapshotXml->hasTagName(IDs::snapshot.toString()))
                                    continue;

                                juce::ValueTree snapshotNode = juce::ValueTree::fromXml(*snapshotXml);
                                if (!snapshotNode.isValid() || !snapshotNode.hasType(IDs::snapshot))
                                    continue;

                                snapshotNode.setProperty(IDs::snapshotIndex, s, nullptr);

                                for (Variant v : { Variant::X, Variant::Y })
                                {
                                    const juce::Identifier varId = (v == Variant::X ? IDs::snapshotX : IDs::snapshotY);
                                    juce::ValueTree varNode = snapshotNode.getChildWithName(varId);
                                    if (!varNode.isValid())
                                    {
                                        varNode = juce::ValueTree(varId);
                                        snapshotNode.addChild(varNode, -1, nullptr);
                                    }

                                    if (!varNode.hasProperty(IDs::name))
                                        varNode.setProperty(IDs::name, getSnapshotDisplayName(s, v), nullptr);

                                    if (!varNode.hasProperty(IDs::activeLaneCount))
                                        varNode.setProperty(IDs::activeLaneCount, 1, nullptr);
                                }

                                presetNode.addChild(snapshotNode, -1, nullptr);
                            }
                        }

                        bankNode.addChild(presetNode, -1, nullptr);
                    }

                    loadedRoot.addChild(bankNode, -1, nullptr);
                }

                if (loadedRoot.getNumChildren() > 0)
                {
                    loadState(loadedRoot);

                   #if LOGS_ENABLED
                    DBG_LOG("STATE", "PRESETMANAGER", "USER_LOAD", "#012U#",
                            "Loaded user preset library from disk folder: " + baseDir.getFullPathName());
                   #endif

                    return true;
                }
            }
        }
    }

    // Backward-compatible migration path (legacy single-file XML).
    // Migration is one-way: load old format, normalize through loadState(),
    // then immediately write the folder-based format.
    const juce::File legacyFile = getLegacyUserLibraryFilePath();
    if (!legacyFile.existsAsFile())
        return false;

    std::unique_ptr<juce::XmlElement> xml = juce::XmlDocument::parse(legacyFile);
    if (!xml || !xml->hasTagName(IDs::root))
        return false;

    const juce::ValueTree vt = juce::ValueTree::fromXml(*xml);
    if (!vt.isValid())
        return false;

    loadState(vt);

    // Write immediately in the new folder-based format.
    saveUserLibraryToDiskIfPossible();

   #if LOGS_ENABLED
    DBG_LOG("STATE", "PRESETMANAGER", "USER_LOAD_LEGACY", "#012L#",
            "Loaded legacy preset library and migrated: " + legacyFile.getFullPathName());
   #endif

    return true;
}

bool PresetManager::saveUserLibraryToDisk()
{
    // Message-thread only by design in this project.
    const juce::File baseDir = getUserLibraryBaseDirectory();
    if (!baseDir.createDirectory().wasOk())
        return false;

    // Keep serialized selection coherent.
    persistSelectionToTree();

    juce::ValueTree indexTree(kPresetLibraryIndexTag);
    indexTree.setProperty(kMetaFormatVersion, kPresetLibraryFormatVersion, nullptr);
    indexTree.setProperty(IDs::currentBank, getCurrentBank(), nullptr);
    indexTree.setProperty(IDs::currentPreset, getCurrentPreset(), nullptr);
    indexTree.setProperty(IDs::currentSnapshot, getCurrentSnapshot(), nullptr);
    indexTree.setProperty(IDs::currentVariant, variantToInt(getCurrentVariant()), nullptr);

    for (int b = 0; b < root.getNumChildren(); ++b)
    {
        const juce::ValueTree bankNode = root.getChild(b);
        if (!bankNode.isValid() || !bankNode.hasType(IDs::bank))
            continue;

        juce::ValueTree bankMeta(kPresetLibraryBankMetaTag);
        bankMeta.setProperty(kMetaIndex, b, nullptr);
        bankMeta.setProperty(IDs::bankName,
                             bankNode.getProperty(IDs::bankName, getBankDirectoryName(b)).toString(),
                             nullptr);
        bankMeta.setProperty(kMetaName,
                             bankNode.getProperty(IDs::bankName, getBankDirectoryName(b)).toString(),
                             nullptr);

        for (int p = 0; p < bankNode.getNumChildren(); ++p)
        {
            const juce::ValueTree presetNode = bankNode.getChild(p);
            if (!presetNode.isValid() || !presetNode.hasType(IDs::preset))
                continue;

            juce::ValueTree presetMeta(kPresetLibraryPresetTag);
            presetMeta.setProperty(kMetaIndex, p, nullptr);
            presetMeta.setProperty(IDs::presetName,
                                   presetNode.getProperty(IDs::presetName, getPresetDirectoryName(p)).toString(),
                                   nullptr);
            presetMeta.setProperty(kMetaName,
                                   presetNode.getProperty(IDs::presetName, getPresetDirectoryName(p)).toString(),
                                   nullptr);
            presetMeta.setProperty(
                kMetaSnapshotCount,
                juce::jlimit(1, kNumSnapshotsDefault,
                             (int) presetNode.getProperty(IDs::snapshotCount, kNumSnapshotsDefault)),
                nullptr);

            bool createdPresetDir = false;

            for (int c = 0; c < presetNode.getNumChildren(); ++c)
            {
                juce::ValueTree snapshotNode = presetNode.getChild(c);
                if (!snapshotNode.isValid() || !snapshotNode.hasType(IDs::snapshot))
                    continue;

                const int s = (int) snapshotNode.getProperty(IDs::snapshotIndex, c);
                if (s < 0 || s >= kNumSnapshotsDefault)
                    continue;
                snapshotNode.setProperty(IDs::snapshotIndex, s, nullptr);

                const juce::File presetDir = getPresetDirectoryPath(baseDir, b, p);
                if (!createdPresetDir)
                {
                    if (!presetDir.createDirectory().wasOk())
                        return false;
                    createdPresetDir = true;
                }

                std::unique_ptr<juce::XmlElement> snapshotXml = snapshotNode.createXml();
                if (!snapshotXml)
                    return false;

                const juce::File snapshotFile = getSnapshotFilePath(baseDir, b, p, s);
                if (!snapshotXml->writeTo(snapshotFile))
                    return false;
            }

            bankMeta.addChild(presetMeta, -1, nullptr);
        }

        indexTree.addChild(bankMeta, -1, nullptr);
    }

    std::unique_ptr<juce::XmlElement> indexXml = indexTree.createXml();
    if (!indexXml)
        return false;

    const juce::File indexFile = getUserLibraryIndexFilePath();
    const bool ok = indexXml->writeTo(indexFile);

   #if LOGS_ENABLED
    DBG_LOG(ok ? "STATE" : "ERROR",
            "PRESETMANAGER",
            ok ? "USER_SAVE" : "USER_SAVE_FAIL",
            ok ? "#022U#" : "#022E#",
            (ok ? "Saved" : "Failed to save") + juce::String(" user preset library folder: ")
            + baseDir.getFullPathName());
   #endif

    return ok;
}

void PresetManager::saveUserLibraryToDiskIfPossible()
{
    juce::ignoreUnused(saveUserLibraryToDisk());
}

//==============================================================================
// 11) Rename helpers (STORE-ONLY)
//------------------------------------------------------------------------------
bool PresetManager::renameBank(int bankIndex, const juce::String& newName)
{
    if (bankIndex < 0 || bankIndex >= root.getNumChildren())
        return false;

    const juce::String safe = sanitiseName(newName);
    if (safe.isEmpty())
        return false;

    const juce::String currentSafe = sanitiseName(getBankName(bankIndex));
    if (currentSafe.compareIgnoreCase(safe) == 0)
        return true;

    const int numBanks = getNumBanks();
    for (int i = 0; i < numBanks; ++i)
    {
        if (i == bankIndex)
            continue;

        const juce::String existingSafe = sanitiseName(getBankName(i));
        if (existingSafe.compareIgnoreCase(safe) == 0)
            return false;
    }

    juce::ValueTree bankNode = root.getChild(bankIndex);
    bankNode.setProperty(IDs::bankName, safe, nullptr);

    if (!isRestoringOrSaving)
        triggerPendingIfNeeded();

    saveUserLibraryToDiskIfPossible();

    return true;
}

bool PresetManager::renamePreset(int bankIndex, int presetIndex, const juce::String& newName)
{
    if (bankIndex < 0 || bankIndex >= root.getNumChildren())
        return false;

    juce::ValueTree bankNode = root.getChild(bankIndex);

    if (presetIndex < 0 || presetIndex >= bankNode.getNumChildren())
        return false;

    const juce::String safe = sanitiseName(newName);
    if (safe.isEmpty())
        return false;

    const juce::String currentSafe = sanitiseName(getPresetName(bankIndex, presetIndex));
    if (currentSafe.compareIgnoreCase(safe) == 0)
        return true;

    const int numPresets = getNumPresets(bankIndex);
    for (int i = 0; i < numPresets; ++i)
    {
        if (i == presetIndex)
            continue;

        const juce::String existingSafe = sanitiseName(getPresetName(bankIndex, i));
        if (existingSafe.compareIgnoreCase(safe) == 0)
            return false;
    }

    juce::ValueTree presetNode = bankNode.getChild(presetIndex);
    presetNode.setProperty(IDs::presetName, safe, nullptr);

    if (!isRestoringOrSaving)
        triggerPendingIfNeeded();

    saveUserLibraryToDiskIfPossible();

    return true;
}

bool PresetManager::renameSnapshot(int bankIndex,
                                   int presetIndex,
                                   int snapshotIndex,
                                   const juce::String& newName)
{
    if (!isValid(bankIndex, presetIndex, snapshotIndex))
        return false;

    const juce::String safe = sanitiseName(newName);
    if (safe.isEmpty())
        return false;

    juce::ValueTree bankNode = root.getChild(bankIndex);
    if (!bankNode.isValid())
        return false;

    juce::ValueTree presetNode = bankNode.getChild(presetIndex);
    if (!presetNode.isValid())
        return false;

    juce::ValueTree snapshotNode;
    for (int i = 0; i < presetNode.getNumChildren(); ++i)
    {
        const juce::ValueTree child = presetNode.getChild(i);
        if (!child.isValid() || !child.hasType(IDs::snapshot))
            continue;

        if ((int) child.getProperty(IDs::snapshotIndex, i) == snapshotIndex)
        {
            snapshotNode = child;
            break;
        }
    }

    if (!snapshotNode.isValid())
    {
        snapshotNode = createDefaultSnapshotNode(snapshotIndex);
        presetNode.addChild(snapshotNode, -1, nullptr);
    }

    for (Variant v : { Variant::X, Variant::Y })
    {
        const juce::Identifier variantId = (v == Variant::X ? IDs::snapshotX : IDs::snapshotY);
        juce::ValueTree varNode = snapshotNode.getChildWithName(variantId);
        if (!varNode.isValid())
        {
            varNode = juce::ValueTree(variantId);
            snapshotNode.addChild(varNode, -1, nullptr);
        }

        varNode.setProperty(IDs::name, safe, nullptr);
        if (!varNode.hasProperty(IDs::activeLaneCount))
            varNode.setProperty(IDs::activeLaneCount, 1, nullptr);
    }

    if (!isRestoringOrSaving)
        triggerPendingIfNeeded();

    saveUserLibraryToDiskIfPossible();

    return true;
}

juce::String PresetManager::getBankName(int bankIndex) const
{
    if (bankIndex < 0 || bankIndex >= root.getNumChildren())
        return "Invalid Bank";

    const juce::ValueTree bankNode = root.getChild(bankIndex);
    return bankNode.getProperty(IDs::bankName, "Bank " + juce::String(bankIndex + 1)).toString();
}

juce::String PresetManager::getPresetName(int bankIndex, int presetIndex) const
{
    if (bankIndex < 0 || bankIndex >= root.getNumChildren())
        return "Invalid Bank";

    const juce::ValueTree bankNode = root.getChild(bankIndex);

    if (presetIndex < 0 || presetIndex >= bankNode.getNumChildren())
        return "Invalid Preset";

    const juce::ValueTree presetNode = bankNode.getChild(presetIndex);
    return presetNode.getProperty(IDs::presetName, "Preset " + juce::String(presetIndex + 1)).toString();
}

juce::String PresetManager::getSnapshotName(int bankIndex,
                                            int presetIndex,
                                            int snapshotIndex,
                                            Variant which) const
{
    if (!isValid(bankIndex, presetIndex, snapshotIndex))
        return getSnapshotDisplayName(snapshotIndex, which);

    const juce::ValueTree bankNode = root.getChild(bankIndex);
    if (!bankNode.isValid())
        return getSnapshotDisplayName(snapshotIndex, which);

    const juce::ValueTree presetNode = bankNode.getChild(presetIndex);
    if (!presetNode.isValid())
        return getSnapshotDisplayName(snapshotIndex, which);

    juce::ValueTree snapshotNode;
    for (int i = 0; i < presetNode.getNumChildren(); ++i)
    {
        const juce::ValueTree child = presetNode.getChild(i);
        if (!child.isValid() || !child.hasType(IDs::snapshot))
            continue;

        if ((int) child.getProperty(IDs::snapshotIndex, i) == snapshotIndex)
        {
            snapshotNode = child;
            break;
        }
    }

    if (!snapshotNode.isValid())
        return getSnapshotDisplayName(snapshotIndex, which);

    const juce::Identifier variantId = (which == Variant::X ? IDs::snapshotX : IDs::snapshotY);
    juce::ValueTree varNode = snapshotNode.getChildWithName(variantId);

    if (!varNode.isValid())
    {
        const juce::Identifier fallbackId = (which == Variant::X ? IDs::snapshotY : IDs::snapshotX);
        varNode = snapshotNode.getChildWithName(fallbackId);
    }

    if (!varNode.isValid())
        return getSnapshotDisplayName(snapshotIndex, which);

    const juce::String fallback = getSnapshotDisplayName(snapshotIndex, which);
    const juce::String fromTree = sanitiseName(varNode.getProperty(IDs::name, fallback).toString());
    if (fromTree.isEmpty())
        return fallback;

    const juce::String legacyX = "S" + juce::String(snapshotIndex + 1) + "X";
    const juce::String legacyY = "S" + juce::String(snapshotIndex + 1) + "Y";
    if (fromTree.compareIgnoreCase(legacyX) == 0 || fromTree.compareIgnoreCase(legacyY) == 0)
        return fallback;

    return fromTree;
}

//==============================================================================
// 12) RT-safe setters (audio thread)
//------------------------------------------------------------------------------
void PresetManager::setCurrentBankRT(int idx)
{
    const LayoutTables* lt = getLayoutRT();
    const int b = clampBankRT(idx, lt);

    currentBank.store(b, std::memory_order_relaxed);
    selectionDirty.store(true, std::memory_order_release);

    // RT -> UI bridge. AsyncUpdater coalesces multiple calls per message turn.
    triggerSelectionAsyncUpdate();
}

void PresetManager::setCurrentPresetRT(int idx)
{
    const LayoutTables* lt = getLayoutRT();
    const int b = currentBank.load(std::memory_order_relaxed);
    const int p = clampPresetRT(b, idx, lt);

    currentPreset.store(p, std::memory_order_relaxed);
    selectionDirty.store(true, std::memory_order_release);

    triggerSelectionAsyncUpdate();
}

void PresetManager::setCurrentSnapshotRT(int idx)
{
    const LayoutTables* lt = getLayoutRT();
    const int b = currentBank.load(std::memory_order_relaxed);
    const int p = currentPreset.load(std::memory_order_relaxed);
    const int s = clampSnapshotRT(b, p, idx, lt);

    currentSnapshot.store(s, std::memory_order_relaxed);
    selectionDirty.store(true, std::memory_order_release);

    triggerSelectionAsyncUpdate();
}

void PresetManager::setCurrentVariantRT(Variant v)
{
    currentVariantI.store(variantToInt(v), std::memory_order_relaxed);
    selectionDirty.store(true, std::memory_order_release);

    triggerSelectionAsyncUpdate();
}

void PresetManager::setBankFromMsbRT(int msb)
{
    setCurrentBankRT(clampMidi7Bit(msb));
}

void PresetManager::setPresetFromLsbRT(int lsb)
{
    setCurrentPresetRT(clampMidi7Bit(lsb));
}

void PresetManager::selectSnapshotFromProgramChangeRT(int pc)
{
    const LayoutTables* lt = getLayoutRT();

    const int b = currentBank.load(std::memory_order_relaxed);
    const int p = currentPreset.load(std::memory_order_relaxed);

    // Convention: PC is 1-based (1..N) -> 0-based index.
    const int requested = juce::jmax(0, pc - 1);
    const int s = clampSnapshotRT(b, p, requested, lt);

    currentSnapshot.store(s, std::memory_order_relaxed);
    selectionDirty.store(true, std::memory_order_release);

    triggerSelectionAsyncUpdate();
}

//==============================================================================
// 13) RT flags / helpers
//------------------------------------------------------------------------------
bool PresetManager::consumeSelectionDirty() noexcept
{
    return selectionDirty.exchange(false, std::memory_order_acq_rel);
}

//==============================================================================
// 14) ValueTree listener (Pending detection)
//------------------------------------------------------------------------------
void PresetManager::onParametersTreeMutation(juce::ValueTree& tree) noexcept
{
    if (isRestoringOrSaving)
        return;

    // Only care about mutations in parameters.state subtree.
    const bool isParametersTree = (tree == parameters.state) || tree.isAChildOf(parameters.state);
    if (!isParametersTree)
        return;

    triggerPendingIfNeeded();
}

void PresetManager::valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier&)
{
    onParametersTreeMutation(tree);
}

void PresetManager::valueTreeChildAdded(juce::ValueTree& parentTree, juce::ValueTree&)
{
    onParametersTreeMutation(parentTree);
}

void PresetManager::valueTreeChildRemoved(juce::ValueTree& parentTree, juce::ValueTree&, int)
{
    onParametersTreeMutation(parentTree);
}

void PresetManager::valueTreeChildOrderChanged(juce::ValueTree& parentTree, int, int)
{
    onParametersTreeMutation(parentTree);
}

void PresetManager::valueTreeParentChanged(juce::ValueTree& tree)
{
    onParametersTreeMutation(tree);
}

void PresetManager::valueTreeRedirected(juce::ValueTree& tree)
{
    onParametersTreeMutation(tree);
}

//==============================================================================
// 15) Layout tables (message thread only)
//------------------------------------------------------------------------------
void PresetManager::rebuildAndPublishLayoutTables()
{
    auto lt = std::make_unique<LayoutTables>();

    const int banks = getNumBanks();
    lt->numBanks = banks;

    lt->presetsPerBank.resize((size_t) banks);
    lt->bankPresetBase.resize((size_t) banks);

    int totalPresets = 0;

    for (int b = 0; b < banks; ++b)
    {
        const int presets = getNumPresets(b);

        lt->presetsPerBank[(size_t) b] = presets;
        lt->bankPresetBase[(size_t) b] = totalPresets;

        totalPresets += presets;
    }

    lt->snapsPerPreset.resize((size_t) totalPresets);

    for (int b = 0; b < banks; ++b)
    {
        const int presets = lt->presetsPerBank[(size_t) b];
        const int base    = lt->bankPresetBase[(size_t) b];

        for (int p = 0; p < presets; ++p)
        {
            const int snaps = getNumSnapshots(b, p);

            lt->snapsPerPreset[(size_t) (base + p)] = snaps;
        }
    }

    publishLayoutTables(std::move(lt));
}

void PresetManager::publishLayoutTables(std::unique_ptr<LayoutTables> newLayout)
{
    const LayoutTables* newPtr = newLayout.get();

    // Publish to RT first (atomic pointer swap).
    // Returned old pointer is intentionally ignored because ownership/lifetime
    // is managed by layoutOwned/retiredLayouts below.
    layoutRT.exchange(newPtr, std::memory_order_acq_rel);

    // Retire old layoutOwned (keep it alive to avoid dangling pointer on RT reads).
    if (layoutOwned)
        retiredLayouts.push_back(std::move(layoutOwned));

    // New layout becomes the owned one.
    layoutOwned = std::move(newLayout);

    // Note:
    // retiredLayouts growth is a deliberate safety tradeoff (no dangling RT read).
    // If a cap is added later, it must include an explicit epoch/RCU style
    // mechanism proving old pointers are no longer visible on audio thread.
}
