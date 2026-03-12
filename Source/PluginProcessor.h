/**
 * @file PluginProcessor.h
 * @brief Processor principal MIDI effect orchestrant les lanes et l etat global.
 */
//==============================================================================
// PluginProcessor.h
// -----------------------------------------------------------------------------
// MidivisiVici - AudioProcessor principal (MIDI Effect)
//
// RESPONSABILITES
//   - Traitement MIDI temps reel (processBlock)
//   - Synchronisation transport (Play/Stop, BPM, PPQ)
//   - Integration preset/snapshot/morph (PresetManager + MorphParameterManager)
//   - Pont sur entre moteur RT et UI (atomics, FIFOs, rappel asynchrone)
//   - Sauvegarde/restauration d etat pour projets DAW
//
// REGLES PROJET (important)
//   - Commentaires/logs en ASCII.
//   - RT safety: pas d allocations, pas d I/O disque, pas de lock, pas de logs
//     couteux dans audio thread.
//   - Acces ValueTree interdit sur audio thread.
//
// A PROPOS DES LANES (A..P, subset actif 1..16)
//   - Les modules lane-based sont instancies par lane.
//   - Le runtime traite uniquement activeLaneCount lanes.
//   - Les monitors sont single-instance (pas de suffixe lane).
//   - Les IDs lane sont construits via ParamIDs::lane(Base::<id>, lane).
//
// TOPOLOGIE CONSUME (InputFilter)
//   - InputMonitor observe le flux brut (diagnostic seulement).
//   - Le routage source est calcule lane par lane:
//       lane[0] source = raw stream
//       si direct(lane[i]) ON    -> lane[i] source = raw stream
//       si consume(lane[i-1]) ON -> lane[i] source = remainder(lane[i-1])
//       si consume(lane[i-1]) OFF-> lane[i] source = lane[i-1] source
//   - Regle de coherence:
//       si Step Filter lane[i] est ON, consume lane[i] est force OFF
//       au niveau routage effectif (meme si un automate host tente ON).
//   - Cela cree des groupes en serie via consume=ON et en parallele via
//     consume=OFF. direct=ON force explicitement la source raw.
//   - Sur edge de direct ou consume, les lanes impactees sont
//     resynchronisees/flush pour eviter des etats held stale provenant de
//     l ancienne topologie.
//
// DEDUP FINAL (apres merge post-splitter)
//   - Quand plusieurs lanes convergent, des NOTE doublonnes peuvent apparaitre.
//   - Les output monitors lane tapent avant merge.
//   - Le chemin host applique un dedup final deterministe:
//        (merge lanes) -> DEDUP -> sortie host
//   - Politique dedup:
//        garder la premiere occurrence d une cle NOTE
//        (samplePos, channel, noteNumber, isNoteOn/isNoteOff),
//        supprimer les occurrences suivantes dans le bloc.
//==============================================================================

#pragma once

#include <JuceHeader.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

//==============================================================================
// INTERNAL MODULES
//==============================================================================

#include "PresetManager.h"
#include "TransportState.h"
#include "0-component/MorphParameterManager.h"

#include "2-inputFilter/InputFilterProcessor.h"
#include "3-harmonizer/HarmonizerProcessor.h"
#include "4-arpeggiator/ArpeggiatorProcessor.h"
#include "5-splitter/SplitterProcessor.h"
#include "1-midiMonitor/MonitorSourceTag.h"

#include "DebugConfig.h"

class MidivisiViciAudioProcessorEditor;

//==============================================================================
// MidivisiViciAudioProcessor
//==============================================================================
//
// Design notes:
//   - MIDI Effect: audio buffers are always cleared.
//   - RT thread MUST NOT allocate.
//   - Monitors use AbstractFifo ring buffers of juce::MidiMessage.
//   - SnapshotRecallAsync runs on message thread only.
//==============================================================================
/**
 * @brief Facade JUCE AudioProcessor pour l ensemble du systeme.
 *
 * Pattern:
 * - Pattern: Facade + orchestrateur de pipeline + pont observer.
 * - Probleme resolu: garder un pipeline MIDI RT deterministe tout en exposant
 *   une UI riche et un systeme preset/snapshot.
 * - Participants:
 *   - MidivisiViciAudioProcessor: proprietaire du pipeline et de l etat.
 *   - Modules lane: InputFilter, Harmonizer, Arpeggiator, Splitter.
 *   - PresetManager/MorphManager: etat non-RT et mapping.
 *   - SnapshotRecallAsync: passerelle RT -> message thread.
 * - Flux:
 *   1. processBlock lit transport + params du bloc.
 *   2. Route MIDI selon topologie lane/consume/direct.
 *   3. Merge sorties lanes, dedup, monitors, sortie host.
 *   4. Effets asynchrones (persist/rappel) hors audio thread.
 * - Points sensibles:
 *   - Acces ValueTree interdit en audio thread.
 *   - Monitors/debug doivent rester RT-safe.
 *   - Dedup doit conserver l ordre deterministe.
 */

class MidivisiViciAudioProcessor : public juce::AudioProcessor
{
public:
    struct LfoDestinationIds
    {
        static constexpr int kNone = 1;
        static constexpr int kBase = 1000;
        static constexpr int kLaneStride = 32;
        static constexpr int kMainBarMorphBase = 100000;

        enum Slot : int
        {
            HarmPitch1 = 0,
            HarmPitch2,
            HarmPitch3,
            HarmPitch4,
            HarmVelMod1,
            HarmVelMod2,
            HarmVelMod3,
            HarmVelMod4,
            ArpRateMorph,
            ArpGateMorph,
            ArpGrooveMorph,
            ArpVelocityMorph
        };

        static constexpr int make(int laneIndex, int slot) noexcept
        {
            return kBase + (laneIndex * kLaneStride) + slot;
        }

        static constexpr int makeMainBarMorph(int pageIndex) noexcept
        {
            return kMainBarMorphBase + pageIndex;
        }

        static constexpr bool isMainBarMorph(int stableId) noexcept
        {
            return stableId >= kMainBarMorphBase;
        }

        static constexpr int getMainBarMorphPageIndex(int stableId) noexcept
        {
            return stableId - kMainBarMorphBase;
        }
    };

    static constexpr int kMaxLfoDestinationsPerRow = 64;

    struct LfoRtRowConfig
    {
        // UI -> RT snapshot payload for one LFO row.
        // Note: destinationStableIds carries multi-select routing targets.
        int rateIndex = 3;
        int depth = 0;
        int offset = 0;
        int waveShape = 0;
        int destinationCount = 0;
        std::array<int, kMaxLfoDestinationsPerRow> destinationStableIds {};
    };

    //--------------------------------------------------------------------------
    // MainBar CC sequencer RT bridge (UI -> audio thread)
    //--------------------------------------------------------------------------
    static constexpr int kMainBarStepsPerPage = 16;
    static constexpr int kMainBarMorphsPerModule = 8;
    static constexpr int kMainBarMaxPages = Lanes::kNumLanes * kMainBarMorphsPerModule;

    struct MainBarPageRtConfig
    {
        int channel = 1; // 1..16
        int ccNumber = 1; // 0..127
        int morphValue = 50; // 0..100 (%)
        std::array<int, kMainBarStepsPerPage> stepValues {};
        std::array<int, kMainBarStepsPerPage> stepRates {};
    };

    //==========================================================================
    // Lifetime
    //==========================================================================
    MidivisiViciAudioProcessor();
    ~MidivisiViciAudioProcessor() override;

    //==========================================================================
    // JUCE AudioProcessor overrides
    //==========================================================================
    /**
     * @brief Prepare structures RT pour la prochaine session de lecture.
     *
     * Thread: callback host (hors hot path processBlock).
     * RT-safe: oui; allocations acceptees ici uniquement.
     */
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    /**
     * @brief Traite un bloc audio/MIDI host.
     *
     * Thread: audio thread.
     * RT-safe: oui (pas de lock, pas d I/O disque, pas d allocation non bornee).
     * Ordre:
     * - Pipeline lane: InputFilter -> Harmonizer -> Arpeggiator -> Splitter.
     * - Dedup applique apres merge lanes et avant sortie host.
     */
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

   #ifndef JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
   #endif

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;

    double getTailLengthSeconds() const override;

    // Host compatibility stubs
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;

    //==========================================================================
    // State persistence (DAW project save/restore)
    //==========================================================================
    /**
     * @brief Serialise l etat plugin pour sauvegarde de projet host.
     *
     * Thread: message thread ou contexte host non-RT.
     * RT-safe: non.
     */
    void getStateInformation(juce::MemoryBlock& destData) override;

    /**
     * @brief Restaure l etat plugin depuis la sauvegarde projet host.
     *
     * Thread: message thread ou contexte host non-RT.
     * RT-safe: non.
     */
    void setStateInformation(const void* data, int sizeInBytes) override;

    //==========================================================================
    // Exposed helpers (UI bridge)
    //==========================================================================
    juce::AudioProcessorValueTreeState& getValueTreeState();

    double getBpm() const noexcept { return lastKnownBpm.load(std::memory_order_relaxed); }
    void   updateBpm(double bpm) noexcept { lastKnownBpm.store(bpm, std::memory_order_relaxed); }

    // UI extrapolation helper: "PPQ now" computed from last host position + wallclock.
    double getHostPpqNowForUI() const;

    bool   getUiIsPlaying() const noexcept;

    // One-shot latch for UI (Play edge).
    bool   consumeUiJustStarted() noexcept;

    // Diagnostic SysEx helper (allocates internally; do not call in hot loops).
    void sendDebugMessage(juce::MidiBuffer& midiMessages,
                          const juce::String& message,
                          int samplePosition = 0);

    // Submodules may inject events here (merged into final midi at end of processBlock).
    juce::MidiBuffer& getMidiOutputBuffer() noexcept { return midiOutputBuffer; }

    // Global panic requested by PresetManager (or UI): executed as one-shot in processBlock.
    void requestGlobalAllNotesOff() noexcept;

    //==========================================================================
    // Monitor FIFOs (UI reads them)
    //==========================================================================
    juce::AbstractFifo& getInputFifo() noexcept { return inputFifo; }
    juce::MidiMessage*  getInputFifoMessages() noexcept { return inputFifoMessages; }

    juce::AbstractFifo& getOutputFifo() noexcept { return outputFifo; }
    juce::MidiMessage*  getOutputFifoMessages() noexcept { return outputFifoMessages; }
    std::uint8_t* getOutputFifoSourceKinds() noexcept { return outputFifoSourceKinds; }
    std::int8_t*  getOutputFifoSourceIndices() noexcept { return outputFifoSourceIndices; }

    int getActiveLaneCount() const noexcept;
    void setActiveLaneCount(int laneCount) noexcept;
    // UI -> RT bridge (lock-free atomics):
    // called from message thread, read from audio thread.
    void setLfoRowCountFromUI(int rowCount) noexcept;
    void setLfoRowConfigFromUI(int rowIndex, const LfoRtRowConfig& config) noexcept;
    void setMainBarPageCountFromUI(int pageCount) noexcept;
    int getMainBarPageCount() const noexcept;
    void setMainBarPageConfigFromUI(int pageIndex, const MainBarPageRtConfig& config) noexcept;
    int getMainBarPlaybackStepForPage(int pageIndex) const noexcept;
    int getMainBarMorphDisplayValueForUI(int pageIndex) const noexcept;
    int getLaneHarmonizerVoiceOffsetDisplayValueForUI(Lanes::Lane lane, int voiceIndex0Based) const noexcept;
    int getLaneHarmonizerVelocityModDisplayValueForUI(Lanes::Lane lane, int voiceIndex0Based) const noexcept;
    int getLaneArpMorphDisplayValueForUI(Lanes::Lane lane, int morphIndex0Based) const noexcept;
    int getLaneArpPlaybackStepForUI(Lanes::Lane lane, int layerIndex0Based) const noexcept;
    uint32_t getChordPitchClassSnapshotForUI(uint16_t& inputPitchClassMask,
                                             int& inputBassPitchClass,
                                             uint16_t& outputPitchClassMask,
                                             int& outputBassPitchClass) const noexcept;
    /**
     * @brief Cache a full editor UI snapshot tree for headless snapshot capture.
     *
     * Thread: message thread only.
     */
    void setSnapshotUiFallbackState(const juce::ValueTree& state);
    /**
     * @brief Install processor-owned snapshot UI callbacks.
     *
     * Used when the editor is closed so LFO snapshot data stays recallable.
     * Thread: message thread only.
     */
    void installSnapshotUiCallbacksFallback();

    //==========================================================================
    // Access to managers (UI + internal modules)
    //==========================================================================
    PresetManager&         getPresetManager() noexcept { return presetManager; }
    MorphParameterManager& getMorphManager() noexcept { return morphManager; }

    // Transport state helper (RT-safe tick + edges).
    TransportState transport;

    //==========================================================================
    // Central APVTS + managers (member init order matters; matches .cpp)
    //==========================================================================
    juce::AudioProcessorValueTreeState parameters;
    PresetManager presetManager;
    MorphParameterManager morphManager;

    // Global morph controller from CC#1 (0..127), applied in processBlock.
    std::atomic<int> zControllerValue { 0 };

private:
    //==========================================================================
    // SnapshotRecallAsync (message thread only)
    //==========================================================================
    class SnapshotRecallAsync : private juce::AsyncUpdater
    {
    public:
        explicit SnapshotRecallAsync(PresetManager& pm, MorphParameterManager& mm)
            : presetManager(pm), morphManager(mm) {}

        // Called from audio thread (RT-safe: atomics + triggerAsyncUpdate).
        void requestRecall(int bank, int preset, int snapshot, int variant)
        {
            pendingBank.store(bank, std::memory_order_relaxed);
            pendingPreset.store(preset, std::memory_order_relaxed);
            pendingSnapshot.store(snapshot, std::memory_order_relaxed);
            pendingVariant.store(variant, std::memory_order_relaxed);

            recallPending.store(true, std::memory_order_release);
            triggerAsyncUpdate();
        }

        // Called from audio thread (RT-safe).
        void triggerPersistOnly()
        {
            triggerAsyncUpdate();
        }

    private:
        void handleAsyncUpdate() override
        {
            bool didSomething = false;

            // 1) Persist selection (bank/preset) if needed
            if (presetManager.consumeSelectionDirty())
            {
                presetManager.persistSelectionToTree();
                didSomething = true;

               #if LOGS_ENABLED
                DBG_LOG("ASYNC", "PLUGINPROCESSOR", "PERSIST", "#210#",
                        "Persisted selection to ValueTree.");
               #endif
            }

            // 2) Snapshot recall if pending
            if (recallPending.exchange(false, std::memory_order_acq_rel))
            {
                const int b = pendingBank.load(std::memory_order_relaxed);
                const int p = pendingPreset.load(std::memory_order_relaxed);
                const int s = pendingSnapshot.load(std::memory_order_relaxed);
                const auto v = (PresetManager::Variant) pendingVariant.load(std::memory_order_relaxed);

                presetManager.loadSnapshot(b, p, s, v);
                didSomething = true;

               #if LOGS_ENABLED
                DBG_LOG("ASYNC", "PLUGINPROCESSOR", "RECALL", "#211#",
                        juce::String("Snapshot recalled -> B=") + juce::String(b) +
                        " P=" + juce::String(p) +
                        " S=" + juce::String(s) +
                        " V=" + juce::String((int) v));
               #endif

                // After snapshot load, sync morph mirror from VTS.
                morphManager.syncFromValueTree();
            }

            // 3) Notify UI if anything changed
            if (didSomething)
                presetManager.notifySelectionChangeUI();
        }

        PresetManager& presetManager;
        MorphParameterManager& morphManager;

        std::atomic<bool> recallPending { false };
        std::atomic<int>  pendingBank { 0 };
        std::atomic<int>  pendingPreset { 0 };
        std::atomic<int>  pendingSnapshot { 0 };
        std::atomic<int>  pendingVariant { 0 };
    };

    std::unique_ptr<SnapshotRecallAsync> recallAsync;

    //==========================================================================
    // MIDI monitors: FIFOs + storage
    // - Input monitor: single shared FIFO
    // - Output monitor: single shared FIFO for all lanes + MainBar CC
    //==========================================================================
    static constexpr int kMonitorFifoCapacity = 512;

    juce::AbstractFifo inputFifo  { kMonitorFifoCapacity };
    juce::MidiMessage  inputFifoMessages[kMonitorFifoCapacity];

    juce::AbstractFifo outputFifo { kMonitorFifoCapacity };
    juce::MidiMessage outputFifoMessages[kMonitorFifoCapacity];
    std::uint8_t outputFifoSourceKinds[kMonitorFifoCapacity] {};
    std::int8_t outputFifoSourceIndices[kMonitorFifoCapacity] {};
    std::atomic<float>* inputMonitorEnableRaw = nullptr;
    std::atomic<float>* inputMonitorFilterNoteRaw = nullptr;
    std::atomic<float>* inputMonitorFilterControlRaw = nullptr;
    std::atomic<float>* inputMonitorFilterClockRaw = nullptr;
    std::atomic<float>* inputMonitorFilterEventRaw = nullptr;
    std::atomic<float>* outputMonitorEnableRaw = nullptr;
    std::atomic<float>* outputMonitorFilterNoteRaw = nullptr;
    std::atomic<float>* outputMonitorFilterControlRaw = nullptr;
    std::atomic<float>* outputMonitorFilterClockRaw = nullptr;
    std::atomic<float>* outputMonitorFilterEventRaw = nullptr;
    std::atomic<float>* whiteInputModeRaw = nullptr;
    std::atomic<float>* blackInputModeRaw = nullptr;

    //==========================================================================
    // Lane processors (A..P)
    //==========================================================================
    static constexpr int kNumLanes = Lanes::kNumLanes;
    static constexpr int kVelocityStackDepth = 16;
    static constexpr int kMaxLfoRows = 8;
    std::array<std::atomic<float>*, kNumLanes> laneInputFilterEnableRaw {};
    std::array<std::atomic<float>*, kNumLanes> laneInputFilterConsumeRaw {};
    std::array<std::atomic<float>*, kNumLanes> laneInputFilterDirectRaw {};
    std::array<std::atomic<float>*, kNumLanes> laneInputFilterBlackNotesRaw {};
    std::array<std::atomic<float>*, kNumLanes> laneInputFilterStepFilterRaw {};
    std::array<std::atomic<float>*, kNumLanes> laneHarmonizerEnableRaw {};
    std::array<std::atomic<float>*, kNumLanes> laneHarmonizerPitchCorrectRaw {};
    std::array<std::atomic<float>*, kNumLanes> laneHarmonizerOctavePlusRaw {};
    std::array<std::atomic<float>*, kNumLanes> laneHarmonizerOctaveMinusRaw {};
    // Cached APVTS raw pointers used to compute UI-only modulation previews.
    std::array<std::array<std::atomic<float>*, 4>, kNumLanes> laneHarmonizerVoiceOffsetRaw {};
    std::array<std::array<std::atomic<float>*, 4>, kNumLanes> laneHarmonizerVelocityModRaw {};
    std::atomic<float>* harmGlobalKeyRaw = nullptr;
    std::atomic<float>* harmGlobalScaleRaw = nullptr;
    // Arp morph order: 0=Rate, 1=Gate, 2=Groove, 3=Velocity.
    std::array<std::array<std::atomic<float>*, 4>, kNumLanes> laneArpMorphRaw {};

    std::array<std::unique_ptr<InputFilterProcessor>, kNumLanes> inputFilterProcessors;
    std::array<std::unique_ptr<HarmonizerProcessor>,  kNumLanes> harmonizerProcessors;
    std::array<std::unique_ptr<ArpeggiatorProcessor>, kNumLanes> arpeggiatorProcessors;
    std::array<std::unique_ptr<SplitterProcessor>,    kNumLanes> splitterProcessors;
    std::array<bool, kNumLanes> lastInputFilterEnabledState {};
    static constexpr int kArpEqScalarCount = 25;
    static constexpr int kArpEqSeqLayerCount = 11;
    static constexpr int kArpEqSeqStepCount = 32;
    std::array<std::array<std::atomic<float>*, kArpEqScalarCount>, kNumLanes> arpEqScalarRaw {};
    std::array<std::array<std::array<juce::AudioParameterChoice*, kArpEqSeqStepCount>, kArpEqSeqLayerCount>, kNumLanes> arpEqStepChoices {};
    bool arpEqCacheReady = false;
    std::array<ArpeggiatorProcessor::ClockSyncState, kNumLanes> preBlockArpSyncScratch {};
    std::array<bool, kNumLanes> preBlockArpSyncValid {};

    struct LfoRtRowState
    {
        std::atomic<int> rateIndex { 3 };
        std::atomic<int> depth { 0 };
        std::atomic<int> offset { 0 };
        std::atomic<int> waveShape { 0 };
        std::atomic<int> destinationCount { 0 };
        std::array<std::atomic<int>, kMaxLfoDestinationsPerRow> destinationStableIds {};
    };

    struct LfoLaneModulationState
    {
        std::array<float, 4> harmPitchNormDeltas {};
        std::array<float, 4> harmVelocityNormDeltas {};
        float arpRateMorphNormDelta = 0.0f;
        float arpGateMorphNormDelta = 0.0f;
        float arpGrooveMorphNormDelta = 0.0f;
        float arpVelocityMorphNormDelta = 0.0f;
    };

    // RT mirror of editor LFO rows.
    // Default 4 rows keeps behavior aligned with snapshot fallback defaults.
    std::atomic<int> lfoRtRowCount { 4 };
    std::array<LfoRtRowState, kMaxLfoRows> lfoRtRows {};
    std::array<LfoLaneModulationState, kNumLanes> lfoLaneModScratch {};
    // UI-only "effective after LFO" snapshots (written audio thread, read UI thread).
    std::array<std::array<std::atomic<int>, 4>, kNumLanes> laneHarmVoiceOffsetDisplay {};
    std::array<std::array<std::atomic<int>, 4>, kNumLanes> laneHarmVelocityModDisplay {};
    std::array<std::array<std::atomic<int>, 4>, kNumLanes> laneArpMorphDisplay {};
    std::array<std::array<std::atomic<int>, 8>, kNumLanes> laneArpPlaybackStepDisplay {};

    struct MainBarPageRtState
    {
        std::atomic<int> channel { 1 };
        std::atomic<int> ccNumber { 1 };
        std::atomic<int> morphValue { 50 };
        std::array<std::atomic<int>, kMainBarStepsPerPage> stepValues {};
        std::array<std::atomic<int>, kMainBarStepsPerPage> stepRates {};
    };

    std::atomic<int> mainBarRtPageCount { 32 };
    std::array<MainBarPageRtState, kMainBarMaxPages> mainBarPages {};
    std::array<int, kMainBarMaxPages> mainBarStepCursor {};
    std::array<double, kMainBarMaxPages> mainBarNextStepPpq {};
    std::array<juce::Random, kMainBarMaxPages> mainBarPageRandom {};
    std::array<std::atomic<int>, kMainBarMaxPages> mainBarPlaybackStep {};
    std::array<std::atomic<int>, kMainBarMaxPages> mainBarMorphDisplay {};
    // Last emitted CC value cache for MainBar sequencer.
    // Indexing: [channel-1][ccNumber], value in 0..127, -1 means "unknown/not sent yet".
    std::array<std::array<int, 128>, 16> mainBarLastSentCcValue {};
    bool mainBarWasPlaying = false;
    // Last known full UI snapshot tree (editor-provided when available).
    // Message-thread only.
    juce::ValueTree snapshotUiFallbackState;

    // Per-lane held snapshot of the lane input stream (post consume-routing input).
    // Used when re-enabling InputFilter to rebuild notes without blind retrigger.
    std::array<std::array<std::array<int, 128>, 16>, kNumLanes> laneInputHeldCount {};
    std::array<std::array<std::array<int, 128>, 16>, kNumLanes> laneInputHeldVelocity {};
    std::array<std::array<std::array<std::array<uint8_t, kVelocityStackDepth>, 128>, 16>, kNumLanes> laneInputHeldVelocityStack {};
    std::array<std::array<std::array<uint8_t, 128>, 16>, kNumLanes> laneInputHeldVelocityStackSize {};
    std::array<std::array<std::array<uint64_t, 128>, 16>, kNumLanes> laneInputHeldOrder {};
    std::array<uint64_t, kNumLanes> laneInputHeldOrderCounter {};
    // Last routing topology snapshot used to detect source-boundary changes.
    // When direct/consume edges toggle, affected lanes must resync/flush to
    // avoid stale held-note ownership from the previous topology.
    std::array<bool, kNumLanes> lastDirectRoutingState {};
    std::array<bool, kNumLanes> lastConsumeRoutingState {};
    std::array<bool, kNumLanes> lastBlackNotesRoutingState {};
    bool lastDirectRoutingStateValid = false;
    bool lastWhiteInputModeState = false;
    bool lastBlackInputModeState = false;
    bool lastInputRemapStateValid = false;

    //==========================================================================
    // Internal MIDI buffers (RT reuse, pre-sized in prepareToPlay / restore)
    //==========================================================================
    juce::MidiBuffer midiOutputBuffer;   // submodules can inject events (panic, etc.)
    juce::MidiBuffer swallowScratch;     // removes plugin-control msgs (CC0/CC32/PC)

    // Raw playable stream after swallow, kept unchanged for consume routing.
    juce::MidiBuffer rawPlayableScratch;
    // Pre-routing global input remap (MainMenu W/B).
    juce::MidiBuffer whiteKeyRemapScratch;
    // White-origin remapped note events (used for white-only chord analysis).
    juce::MidiBuffer whiteInputOriginScratch;
    // NOTE events in the remapped stream that originated from black-key input
    // while global B mode is enabled. Used for per-lane InputFilter black-note
    // gating without changing the canonical rawPlayableScratch stream.
    juce::MidiBuffer blackInputOriginScratch;
    // White-only held-note state used to derive deterministic black-key mapping
    // when W mode is enabled (black mode chord source excludes black keys).
    std::array<std::array<int, 128>, 16> whiteModeHeldCount {};
    // Per-(channel,input note) stack for white-key remap pairing.
    // This guarantees NOTE OFF symmetry even under dense passages and mode
    // updates while keys are held.
    std::array<std::array<std::array<uint8_t, kVelocityStackDepth>, 128>, 16> whiteModeMappedNoteStack {};
    std::array<std::array<std::array<uint8_t, kVelocityStackDepth>, 128>, 16> whiteModeMappedVelocityStack {};
    std::array<std::array<uint8_t, 128>, 16> whiteModeMappedNoteStackSize {};
    std::array<std::array<uint8_t, 128>, 16> whiteModePhysicalHeldCount {};
    std::array<std::array<uint8_t, 128>, 16> whiteModePhysicalLastVelocity {};
    // Signature of the currently active white-only chord context.
    // Format (low to high bits):
    // - [0..11]   : white pitch-class mask
    // - [12..15]  : white bass pitch class (0..11, 15 = none)
    // - [16..19]  : global tonic pitch class
    // - [20..23]  : global mode index
    uint32_t whiteModeChordSignature = 0;
    bool whiteModeChordSignatureValid = false;
    // Current deterministic target set for black-key mapping:
    // C#->set[0], D#->set[1], F#->set[2], G#->set[3], A#->set[4].
    std::array<int, 5> blackModeFiveClassSet { 0, 2, 4, 7, 9 };
    // Per-(channel,input note) stack that remembers mapped NOTE ON target
    // notes for black-key inputs so NOTE OFF keeps exact pairing when the
    // five-class set changes while notes are held.
    std::array<std::array<std::array<uint8_t, kVelocityStackDepth>, 128>, 16> blackModeMappedNoteStack {};
    // Parallel velocity stack (same indexing/size as blackModeMappedNoteStack).
    // Used to retrig held black notes with stable velocity when the active
    // five-class set is recomputed.
    std::array<std::array<std::array<uint8_t, kVelocityStackDepth>, 128>, 16> blackModeMappedVelocityStack {};
    std::array<std::array<uint8_t, 128>, 16> blackModeMappedNoteStackSize {};
    // Deferred NOTE OFF counters for black-origin remap retargets.
    // Keyed by final mapped output note [channel][note].
    //
    // Why this exists:
    // - In W+B mode, a black held note can be retargeted when white chord changes.
    // - If the previous black mapped note is still held by white origin, sending
    //   NOTE OFF immediately can steal the white-held note.
    // - We defer that OFF and flush it later only when the target note is no
    //   longer owned by white origin and no black mapping still references it.
    //
    // This keeps NOTE ON/OFF parity robust while preventing audible steals.
    std::array<std::array<uint8_t, 128>, 16> blackModeDeferredOffCount {};
    // Physical held state for incoming black keys (before remap target changes).
    // This is the source of truth used by RT self-heal to keep mapped stacks
    // coherent under rapid chord transitions.
    std::array<std::array<uint8_t, 128>, 16> blackModePhysicalHeldCount {};
    // Last physical velocity seen per incoming black key. Used as fallback
    // when self-heal needs to reconstruct missing mapped NOTE ON events.
    std::array<std::array<uint8_t, 128>, 16> blackModePhysicalLastVelocity {};
    // Deferred set update request produced after lane processing (post-chord).
    // Applied at next block start in the remap stage.
    bool blackModePendingSetUpdate = false;
    std::array<int, 5> blackModePendingFiveClassSet { 0, 2, 4, 7, 9 };

    // Per-lane working buffers:
    //   laneSourceScratch[i]      : resolved lane source before housekeeping
    //                               (raw, propagated parallel source, or remainder)
    //   laneInputScratch[i]       : exact input seen by lane i after source
    //                               resolution + housekeeping
    //   laneAfterFilterScratch[i] : buffer state after InputFilter only (consume basis)
    //   laneOutputScratch[i]      : lane output after its chain (IF + Harm + Arp + Splitter)
    //   laneRemainderScratch[i]   : remainder passed to next lane when consume is ON
    std::array<juce::MidiBuffer, kNumLanes> laneSourceScratch;
    std::array<juce::MidiBuffer, kNumLanes> laneInputScratch;
    std::array<juce::MidiBuffer, kNumLanes> laneAfterFilterScratch;
    std::array<juce::MidiBuffer, kNumLanes> laneOutputScratch;
    std::array<juce::MidiBuffer, kNumLanes> laneRemainderScratch;

    // Merge buffer (post-lane processing; post Splitter).
    juce::MidiBuffer mergedLanesScratch;

    // Dedup buffer at the lane junction before monitoring + host output.
    juce::MidiBuffer dedupedMergedScratch;

    // Physical held-note snapshot from raw input stream (ch/note).
    // Used by consume handoff logic to preserve original velocity when notes
    // move from one lane to another due to internal voice reallocation.
    std::array<std::array<int, 128>, 16> rawHeldCount {};
    std::array<std::array<int, 128>, 16> rawHeldVelocity {};
    std::array<std::array<std::array<uint8_t, kVelocityStackDepth>, 128>, 16> rawHeldVelocityStack {};
    std::array<std::array<uint8_t, 128>, 16> rawHeldVelocityStackSize {};
    std::array<std::array<uint64_t, 128>, 16> rawHeldOrder {};
    uint64_t rawHeldOrderCounter = 0;

    // Output held-note counters used by dedup to avoid dropping required
    // NoteOff events when several internal voices collapse to the same note.
    std::array<std::array<int, 128>, 16> outputHeldCount {};

    // Held-note counters captured right after the Harmonizer stage, per lane.
    // Used to build the "output chord" snapshot while optionally excluding
    // lanes that use the black-notes source.
    //
    // Layout:
    //   [lane][channel][note] => held count
    std::array<std::array<std::array<int, 128>, 16>, kNumLanes> harmonizerHeldCountByLane {};

    //==========================================================================
    // Global panic latch (one-shot consumed in processBlock)
    //==========================================================================
    std::atomic<bool> pendingGlobalAllNotesOff { false };

    // Emits panic messages into a MIDI buffer at samplePos (RT-safe, no heap).
    static void emitGlobalAllNotesOff(juce::MidiBuffer& midi, int samplePos) noexcept;

    // Optional hook to reset internal held-note trackers after panic.
    void resetAllNoteTrackingAfterPanic() noexcept;

    //==========================================================================
    // UI sync atomics (written in processBlock, read on UI thread)
    //==========================================================================
    std::atomic<double> lastKnownBpm { 120.0 };

    std::atomic<bool>   uiIsPlaying { false };
    std::atomic<bool>   uiPlayJustStarted { false };

    std::atomic<double> uiPpqAtBlockStart { 0.0 };
    std::atomic<double> uiBeatsPerSec { 0.0 };
    std::atomic<double> uiBlockWallclock { 0.0 };
    std::atomic<uint16_t> uiInputChordPitchClassMask { 0 };
    std::atomic<int> uiInputChordBassPitchClass { -1 };
    std::atomic<uint16_t> uiOutputChordPitchClassMask { 0 };
    std::atomic<int> uiOutputChordBassPitchClass { -1 };
    std::atomic<uint32_t> uiChordSnapshotRevision { 0 };

    // Number of active lanes in the runtime pipeline/UI (1..kNumLanes).
    std::atomic<int> activeLaneCount { 1 };
    // Audio-thread shadow of lane count seen at previous processBlock.
    // Used when lane count changes dynamically (add/remove lanes):
    // - shrinking lane count: flush removed lanes through the full lane chain
    //   (InputFilter -> Harmonizer -> Arpeggiator -> Splitter) before reset
    //   so transformed/generated notes get proper NOTE OFF.
    // - growing lane count: initialize new lanes from a clean state.
    int lastProcessedActiveLaneCount = 1;

    // Fallback PPQ timeline for modules when host transport position is
    // temporarily unavailable. Audio-thread only.
    double fallbackPpqAtNextBlock = 0.0;
    bool fallbackPpqValid = false;
    uint16_t chordInputPitchClassMaskRt = 0;
    int chordInputBassPitchClassRt = -1;
    uint16_t chordOutputPitchClassMaskRt = 0;
    int chordOutputBassPitchClassRt = -1;
    uint64_t chordHarmonizerParamHashRt = 0;
    bool chordHarmonizerParamHashValidRt = false;
    uint32_t chordSnapshotRevisionRt = 0;

    // Local note-driven playback clock (used while DAW transport is stopped).
    // Audio-thread only.
    bool localClockWasActive = false;
    double localClockPpqAtNextBlock = 0.0;

    // LFO phase anchor (PPQ origin) used to restart LFOs cleanly at each
    // playback start edge (DAW or local note-driven start).
    // Audio-thread only.
    bool lfoPhaseAnchorValid = false;
    double lfoPhaseAnchorPpq = 0.0;

    // Sync morph mirror from VTS (called from non-RT contexts).
    void syncMorphMirrorFromVTS();
    void refreshRtParameterPointerCache() noexcept;
    void clearChordSnapshotRt() noexcept;
    void buildArpeggiatorEquivalenceCache() noexcept;
    juce::ValueTree buildLfoUiStateFromRuntime() const;
    juce::ValueTree buildDefaultSnapshotUiStateFromRuntime() const;
    void applyLfoUiStateToRuntime(const juce::ValueTree& state) noexcept;
    bool areArpeggiatorSettingsEquivalentCached(Lanes::Lane laneA,
                                                Lanes::Lane laneB) const noexcept;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidivisiViciAudioProcessor)
};
