/**
 * @file PresetManager.h
 * @brief Persistent bank/preset/snapshot manager with RT-safe selection bridge.
 *
 * Threading:
 * - ValueTree authoring on message thread.
 * - RT API uses atomics only and never touches ValueTree.
 *
 * Architecture intent:
 * - ValueTree = persistence/authoring source of truth (message thread domain).
 * - snapshots mirror = fast in-memory cache for capture/recall convenience.
 * - LayoutTables = immutable clamp map for RT-safe program addressing.
 */

#pragma once

#include <JuceHeader.h>

#include <atomic>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

// Forward decl to avoid include cycles
class MorphParameterManager;

//==============================================================================
/**
 * =============================================================================
 * PresetManager
 * =============================================================================
 *
 * Encapsulates the full snapshot system:
 *
 *   BANKS -> PRESETS -> SNAPSHOTS -> VARIANTS (X/Y)
 *
 * - A snapshot is a full plugin parameter state (parameters.state subtree).
 * - Each snapshot stores 2 variants:
 *     X : main state
 *     Y : alternate state (used for morph via global Z)
 *
 * Threading model:
 * - Message thread (non-RT):
 *     ValueTree authoring, import/export, rename, capture, load, persistence.
 * - Audio thread (RT):
 *     Atomics only, reads immutable layout tables (published by pointer swap).
 *
 * Pending contract:
 * - The UI Save button becomes "Pending" when the user modifies parameters.
 * - Operations that APPLY a snapshot (replace parameters.state) must be guarded
 *   by isRestoringOrSaving to avoid spurious "Pending" triggers.
 *
 * NOTE:
 * - This class listens to parameters.state changes to detect user edits.
 * - It must not do heavy work from ValueTree callbacks.
 *
 * Selection sync contract (RT -> UI):
 * - RT selection changes set selectionDirty=true then call triggerAsyncUpdate().
 * - AsyncUpdater coalesces events and runs handleAsyncUpdate() on message thread.
 * - handleAsyncUpdate() persists clamped selection and notifies UI callbacks.
 *
 * Persistence boundary:
 * - "Snapshot content" = APVTS parameter subtree + optional UI metadata.
 * - "Current selection" = bank/preset/snapshot/variant properties on root.
 * - loadState() always normalizes structure before publishing it to RT tables.
 * =============================================================================
 */
/**
 * @brief Owns preset hierarchy and snapshot capture/recall lifecycle.
 *
 * Pattern:
 * - Pattern: Repository + Observer
 * - Problem solved: provide a single source of truth for preset data while
 *   broadcasting selection/pending changes to UI and processor.
 * - Participants: `PresetManager`, `juce::ValueTree`, APVTS, UI callbacks.
 * - Flow: user/host selection -> manager state update -> optional snapshot load.
 * - Pitfalls: strict split between RT atomics and non-RT ValueTree operations.
 */
class PresetManager : public juce::ValueTree::Listener,
                      private juce::AsyncUpdater
{
public:
    //==========================================================================
    // ValueTree identifiers (public so other modules can find nodes reliably)
    //==========================================================================
    struct IDs
    {
        // Root / hierarchy
        static const inline juce::Identifier root     { "PRESETS" };
        static const inline juce::Identifier bank     { "BANK" };
        static const inline juce::Identifier preset   { "PRESET" };
        static const inline juce::Identifier snapshot { "SNAPSHOT" };

        // Metadata
        static const inline juce::Identifier bankName      { "bankName" };
        static const inline juce::Identifier presetName    { "presetName" };
        static const inline juce::Identifier snapshotIndex { "snapshotIndex" };
        static const inline juce::Identifier snapshotCount { "snapshotCount" };
        static const inline juce::Identifier name          { "name" };

        // Variant nodes inside a SNAPSHOT
        static const inline juce::Identifier snapshotX { "X" };
        static const inline juce::Identifier snapshotY { "Y" };

        // Per-variant non-parameter metadata
        static const inline juce::Identifier uiState         { "UI_STATE" };
        static const inline juce::Identifier activeLaneCount { "activeLaneCount" };

        // Persisted selection (stored under root node)
        static const inline juce::Identifier currentBank     { "currentBank" };
        static const inline juce::Identifier currentPreset   { "currentPreset" };
        static const inline juce::Identifier currentSnapshot { "currentSnapshot" };
        static const inline juce::Identifier currentVariant  { "currentVariant" };
    };

    //==========================================================================
    // Variant selection
    //==========================================================================
    enum class Variant : int { X = 0, Y = 1 };

    static constexpr int variantToInt(Variant v) noexcept { return static_cast<int>(v); }
    static constexpr Variant intToVariant(int i) noexcept { return (i == 1) ? Variant::Y : Variant::X; }

    //==========================================================================
    // SnapshotSlot (memory mirror)
    //==========================================================================
    struct SnapshotSlot
    {
        juce::ValueTree stateX;
        juce::ValueTree stateY;
    };

    //==========================================================================
    // 1) Ctor / dtor
    //==========================================================================
    explicit PresetManager(juce::AudioProcessorValueTreeState& vts);
    ~PresetManager() override;

    PresetManager(const PresetManager&) = delete;
    PresetManager& operator=(const PresetManager&) = delete;
    PresetManager(PresetManager&&) = delete;
    PresetManager& operator=(PresetManager&&) = delete;

    //==========================================================================
    // 2) Global state (DAW persistence)
    //==========================================================================
    [[nodiscard]] juce::ValueTree getState() const;
    void loadState(const juce::ValueTree& newState);

    //==========================================================================
    // 3) Snapshot capture / recall (non-RT, message thread only)
    //==========================================================================
    void captureSnapshot(int bank, int preset, int snap, Variant which);
    void loadSnapshot   (int bank, int preset, int snap, Variant which);

    void captureCurrent(Variant which);
    void loadCurrentSnapshot(Variant which);

    void copySnapshot(int srcBank, int srcPreset, int srcSnap,
                      int dstBank, int dstPreset, int dstSnap,
                      Variant which);

    void resetSnapshotToDefaults(int bank, int preset, int snap, Variant which);

    //==========================================================================
    // 4) Current selection (UI / message thread)
    //==========================================================================
    void setCurrentBank(int idx);
    void setCurrentPreset(int idx);
    void setCurrentSnapshot(int idx);
    void setCurrentVariant(Variant v);

    [[nodiscard]] int getCurrentBank() const noexcept;
    [[nodiscard]] int getCurrentPreset() const noexcept;
    [[nodiscard]] int getCurrentSnapshot() const noexcept;
    [[nodiscard]] Variant getCurrentVariant() const noexcept;

    //==========================================================================
    // 5) MIDI mapping (UI / message thread)
    //==========================================================================
    void setBankFromMsb(int msb);
    void setPresetFromLsb(int lsb);
    void selectSnapshotFromProgramChange(int pc);

    //==========================================================================
    // 6) Names and counts (UI helpers)
    //==========================================================================
    bool renameBank(int bankIndex, const juce::String& newName);
    bool renamePreset(int bankIndex, int presetIndex, const juce::String& newName);
    bool renameSnapshot(int bankIndex, int presetIndex, int snapshotIndex, const juce::String& newName);

    juce::String getBankName(int bankIndex) const;
    juce::String getPresetName(int bankIndex, int presetIndex) const;
    juce::String getSnapshotName(int bankIndex,
                                 int presetIndex,
                                 int snapshotIndex,
                                 Variant which = Variant::X) const;

    int getNumBanks() const;
    int getNumPresets(int bankIndex) const;
    int getNumSnapshots(int bankIndex, int presetIndex) const;

    // Host program mapping (RT-safe, no ValueTree access).
    // Program index is flattened as:
    //   bank -> preset -> snapshot
    int getNumProgramEntriesRTSafe() const noexcept;
    bool getProgramAddressForIndexRTSafe(int programIndex,
                                         int& outBank,
                                         int& outPreset,
                                         int& outSnapshot) const noexcept;
    int getProgramIndexForAddressRTSafe(int bank, int preset, int snapshot) const noexcept;

    //==========================================================================
    // 7) Import / export XML (non-RT)
    //==========================================================================
    juce::Result exportToFile(const juce::File& targetFile) const;
    juce::Result importFromFile(const juce::File& sourceFile);

    //==========================================================================
    // 8) UI helpers
    //==========================================================================
    static juce::String getSnapshotDisplayName(int snapIndex, Variant which);

    void markSaved()
    {
        pendingLatched.store(false, std::memory_order_release);

        if (onPendingChange)
        {
            if (juce::MessageManager::getInstance()->isThisTheMessageThread())
                onPendingChange(false);
            else
                juce::MessageManager::callAsync([cb = onPendingChange] { cb(false); });
        }
    }

    void clearPendingLatch() noexcept
    {
        pendingLatched.store(false, std::memory_order_release);
    }

    //==========================================================================
    // 9) RT-safe API (audio thread)
    //==========================================================================
    void setCurrentBankRT(int idx);
    void setCurrentPresetRT(int idx);
    void setCurrentSnapshotRT(int idx);
    void setCurrentVariantRT(Variant v);

    void setBankFromMsbRT(int msb);
    void setPresetFromLsbRT(int lsb);
    void selectSnapshotFromProgramChangeRT(int pc);

    //==========================================================================
    // 10) Persistence helpers
    //==========================================================================
    void persistSelectionToTree();
    bool consumeSelectionDirty() noexcept;

    void replaceParametersStateFromDaw(const juce::ValueTree& newState);

    //==========================================================================
    // 11) Morph integration (non-owner)
    //==========================================================================
    void setMorphManager(MorphParameterManager* manager) noexcept { morphManager = manager; }

    //==========================================================================
    // 12) UI callbacks
    //==========================================================================
    using PendingChangeFn = std::function<void(bool)>;
    void setOnPendingChange(PendingChangeFn callback) { onPendingChange = std::move(callback); }

    using SelectionChangeFn = std::function<void(int bank, int preset, int snapshot, int variant)>;
    void setOnSelectionChange(SelectionChangeFn fn);

    void notifySelectionChangeUI();
    
    using GlobalAllNotesOffFn = std::function<void()>;
    void setOnGlobalAllNotesOff(GlobalAllNotesOffFn fn) { onGlobalAllNotesOff = std::move(fn); }

    // Snapshot metadata callbacks:
    // - Lane count is runtime-relevant (processor side).
    // - UI state is editor-only (lane names/collapse, LFO layout, etc.).
    // Contracts:
    // - Capture callbacks are called during snapshot capture (message thread).
    // - Apply callbacks are called during snapshot load (message thread).
    // - Callbacks must not call back into PresetManager with recursive loads.
    using SnapshotLaneCountCaptureFn = std::function<int()>;
    using SnapshotLaneCountApplyFn = std::function<void(int)>;
    void setOnSnapshotLaneCountCapture(SnapshotLaneCountCaptureFn fn) { onSnapshotLaneCountCapture = std::move(fn); }
    void setOnSnapshotLaneCountApply(SnapshotLaneCountApplyFn fn) { onSnapshotLaneCountApply = std::move(fn); }

    using SnapshotUiCaptureFn = std::function<juce::ValueTree()>;
    using SnapshotUiApplyFn = std::function<void(const juce::ValueTree&)>;
    void setOnSnapshotUiCapture(SnapshotUiCaptureFn fn) { onSnapshotUiCapture = std::move(fn); }
    void setOnSnapshotUiApply(SnapshotUiApplyFn fn) { onSnapshotUiApply = std::move(fn); }

    [[nodiscard]] juce::ValueTree getSnapshotUiState(int bank, int preset, int snap, Variant which) const;
    [[nodiscard]] juce::ValueTree getCurrentSnapshotUiState() const;

private:
    //==========================================================================
    // AsyncUpdater (RT -> UI selection sync)
    //
    // IMPORTANT:
    // - triggerAsyncUpdate() is safe from any thread in JUCE.
    // - handleAsyncUpdate() runs on message thread.
    // - We only use this to translate selectionDirty into UI notifications.
    //==========================================================================
    void triggerSelectionAsyncUpdate() noexcept;
    void handleAsyncUpdate() override;

    //==========================================================================
    // Internal data
    //==========================================================================
    juce::AudioProcessorValueTreeState& parameters;

    juce::ValueTree root;
    juce::ValueTree factoryDefaultState;

    //==========================================================================
    // Flags / latches
    //==========================================================================
    std::atomic<bool> selectionDirty { false };
    std::atomic<bool> pendingLatched { false };

    void triggerPendingIfNeeded()
    {
        if (pendingLatched.exchange(true, std::memory_order_acq_rel))
            return;

        if (onPendingChange)
        {
            if (juce::MessageManager::getInstance()->isThisTheMessageThread())
                onPendingChange(true);
            else
                juce::MessageManager::callAsync([cb = onPendingChange] { cb(true); });
        }
    }

    //==========================================================================
    // Memory mirror: snapshots[bank][preset][snap]
    //==========================================================================
    std::vector<std::vector<std::vector<SnapshotSlot>>> snapshots;

    static constexpr int kNumBanksDefault     = 16;
    static constexpr int kNumPresetsDefault   = 16;
    static constexpr int kNumSnapshotsDefault = 8;

    //==========================================================================
    // Current selection (RT-friendly atomics)
    //==========================================================================
    std::atomic<int> currentBank     { 0 };
    std::atomic<int> currentPreset   { 0 };
    std::atomic<int> currentSnapshot { 0 };
    std::atomic<int> currentVariantI { variantToInt(Variant::X) };

    //==========================================================================
    // RT layout tables (immutable, published by atomic pointer swap)
    //
    // Why immutable:
    // - Audio thread reads through raw pointer without lock.
    // - Message thread publishes a new instance and retires the old one.
    // - Old instances remain alive in retiredLayouts to prevent dangling reads.
    //==========================================================================
    struct LayoutTables
    {
        int numBanks = 0;
        std::vector<int> presetsPerBank;
        std::vector<int> bankPresetBase;
        std::vector<int> snapsPerPreset;
    };

    std::atomic<const LayoutTables*> layoutRT { nullptr };

    const LayoutTables* getLayoutRT() const noexcept
    {
        return layoutRT.load(std::memory_order_acquire);
    }

    int clampBankRT(int b, const LayoutTables* lt) const noexcept
    {
        if (lt == nullptr || lt->numBanks <= 0)
            return 0;

        return juce::jlimit(0, lt->numBanks - 1, b);
    }

    int clampPresetRT(int bank, int preset, const LayoutTables* lt) const noexcept
    {
        if (lt == nullptr || lt->numBanks <= 0)
            return 0;

        const int b = clampBankRT(bank, lt);
        const int nPresets = (b >= 0 && b < (int) lt->presetsPerBank.size())
                           ? lt->presetsPerBank[(size_t) b]
                           : 0;

        if (nPresets <= 0)
            return 0;

        return juce::jlimit(0, nPresets - 1, preset);
    }

    int clampSnapshotRT(int bank, int preset, int snap, const LayoutTables* lt) const noexcept
    {
        if (lt == nullptr || lt->numBanks <= 0)
            return 0;

        const int b = clampBankRT(bank, lt);
        const int p = clampPresetRT(b, preset, lt);

        if (b < 0 || b >= (int) lt->bankPresetBase.size())
            return 0;

        const int base = lt->bankPresetBase[(size_t) b];
        const int flatIndex = base + p;

        if (flatIndex < 0 || flatIndex >= (int) lt->snapsPerPreset.size())
            return 0;

        const int nSnaps = lt->snapsPerPreset[(size_t) flatIndex];

        if (nSnaps <= 0)
            return 0;

        return juce::jlimit(0, nSnaps - 1, snap);
    }

    // layoutOwned = currently published layout.
    // retiredLayouts = previous published layouts kept alive for RT safety.
    std::unique_ptr<LayoutTables> layoutOwned;
    std::vector<std::unique_ptr<LayoutTables>> retiredLayouts;

    void rebuildAndPublishLayoutTables();
    void publishLayoutTables(std::unique_ptr<LayoutTables> newLayout);

    //==========================================================================
    // Optional global on-disk library persistence (message thread only)
    //==========================================================================
    juce::File getUserLibraryBaseDirectory() const;
    juce::File getUserLibraryIndexFilePath() const;
    juce::File getLegacyUserLibraryFilePath() const;
    juce::File getBankDirectoryPath(const juce::File& baseDir, int bankIndex) const;
    juce::File getPresetDirectoryPath(const juce::File& baseDir, int bankIndex, int presetIndex) const;
    juce::File getSnapshotFilePath(const juce::File& baseDir, int bankIndex, int presetIndex, int snapshotIndex) const;
    juce::String getBankDirectoryName(int bankIndex) const;
    juce::String getPresetDirectoryName(int presetIndex) const;
    juce::String getSnapshotFileName(int snapshotIndex) const;
    bool loadUserLibraryFromDisk();
    bool saveUserLibraryToDisk();
    void saveUserLibraryToDiskIfPossible();

    //==========================================================================
    // APPLY helper (non-RT, message thread only)
    //==========================================================================
    void replaceParametersStateGuarded(const juce::ValueTree& newState);

    //==========================================================================
    // Internal helpers (non-RT)
    //==========================================================================
    void initialiseDefaultBanks();
    juce::ValueTree createDefaultSnapshotNode(int snapshotIndex);
    juce::ValueTree createDefaultBankNode(const juce::String& name, int numPresets);
    juce::ValueTree createDefaultPresetNode(const juce::String& name);

    bool isValid(int bank, int preset, int snap) const;
    juce::String sanitiseName(const juce::String& input) const;

    void writeStateIntoTree(int bank, int preset, int snap, Variant which, const juce::ValueTree& st);
    juce::ValueTree readStateFromTree(int bank, int preset, int snap, Variant which) const;
    juce::ValueTree getVariantNode(int bank, int preset, int snap, Variant which) const;
    juce::ValueTree getOrCreateVariantNode(int bank, int preset, int snap, Variant which);

    void writeUiStateIntoTree(int bank, int preset, int snap, Variant which, const juce::ValueTree& uiState);
    juce::ValueTree readUiStateFromTree(int bank, int preset, int snap, Variant which) const;
    void clearUiStateInTree(int bank, int preset, int snap, Variant which);

    void writeActiveLaneCountIntoTree(int bank, int preset, int snap, Variant which, int laneCount);
    int readActiveLaneCountFromTree(int bank, int preset, int snap, Variant which, int fallback = -1) const;
    void clearActiveLaneCountInTree(int bank, int preset, int snap, Variant which);

    void resyncSlotsFromTree();
    
    void requestGlobalAllNotesOffIfAvailable();
    GlobalAllNotesOffFn onGlobalAllNotesOff;

    //==========================================================================
    // Save/pending detection
    //==========================================================================
    bool isRestoringOrSaving = false;

    //==========================================================================
    // Selection callbacks
    //==========================================================================
    SelectionChangeFn onSelectionChange;

    void notifySelectionChangeIfNeeded();

    int lastNotifiedBank     = -1;
    int lastNotifiedPreset   = -1;
    int lastNotifiedSnapshot = -1;
    int lastNotifiedVariant  = -1;

    //==========================================================================
    // ValueTree::Listener overrides (parameters.state listening)
    //==========================================================================
    void onParametersTreeMutation(juce::ValueTree& tree) noexcept;

    void valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier&) override;
    void valueTreeChildAdded(juce::ValueTree& parentTree, juce::ValueTree& childWhichHasBeenAdded) override;
    void valueTreeChildRemoved(juce::ValueTree& parentTree,
                               juce::ValueTree& childWhichHasBeenRemoved,
                               int indexFromWhichChildWasRemoved) override;
    void valueTreeChildOrderChanged(juce::ValueTree& parentTree, int oldIndex, int newIndex) override;
    void valueTreeParentChanged(juce::ValueTree& treeWhoseParentHasChanged) override;
    void valueTreeRedirected(juce::ValueTree& treeWhichHasBeenChanged) override;

    //==========================================================================
    // Morph integration (non-owner)
    //==========================================================================
    MorphParameterManager* morphManager { nullptr };

    //==========================================================================
    // UI callbacks storage
    //==========================================================================
    PendingChangeFn onPendingChange;
    SnapshotLaneCountCaptureFn onSnapshotLaneCountCapture;
    SnapshotLaneCountApplyFn onSnapshotLaneCountApply;
    SnapshotUiCaptureFn onSnapshotUiCapture;
    SnapshotUiApplyFn onSnapshotUiApply;
};
