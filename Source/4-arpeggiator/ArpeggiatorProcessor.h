/**
 * @file ArpeggiatorProcessor.h
 * @brief Moteur arpeggiator d une lane (thread audio).
 *
 * Role:
 * - Transformer les notes tenues en flux MIDI genere selon les couches
 *   du step sequencer (rythm, strum/hit, jump/velo-env, octave/tim-env,
 *   velocity, groove, gate, retrig).
 * - Garantir un ordre temporel deterministe, y compris sur les cas cross-block.
 *
 * Threading:
 * - `process` est appelee sur audio thread.
 * - Hot path RT-safe: pas de lock, pas d allocation, pas d I/O.
 */
//==============================================================================
// ArpeggiatorProcessor.h
//------------------------------------------------------------------------------
// Moteur MIDI arpeggiator par lane.
//
// Responsabilites:
// - Consommer les notes source tenues.
// - Appliquer les couches de pas et leurs interactions.
// - Produire des note on/off avec timing stable en PPQ.
// - Gerer une file differree pour les evenements futurs (groove/retrig/mute).
//
// Contrat temps reel:
// - Audio thread only for process().
// - No locks, no heap allocations in process().
// - Deterministic ordering for events at the same musical time.
//==============================================================================

#pragma once

#include <JuceHeader.h>

#include <array>
#include <atomic>
#include <cstdint>

#include "../PluginParameters.h"

/**
 * @brief Processor arpeggiator par lane, execute sur thread audio.
 *
 * Pattern utilise:
 * - Pattern: machine d etat + strategie + file differree.
 * - Probleme resolu: conserver un timing musical stable malgre les interactions
 *   complexes entre couches et offsets cross-block.
 * - Participants:
 *   - ArpeggiatorProcessor: orchestration globale.
 *   - APVTS atomics/choices: lecture de configuration.
 *   - Deferred queue: report d evenements futurs en PPQ.
 * - Flux:
 *   1) Lecture contexte host + params caches.
 *   2) Avance des curseurs musicaux.
 *   3) Emission des offs planifies + evenements differes dus.
 *   4) Generation des ticks atteints.
 *   5) Merge final dans le buffer lane, ordre stable.
 *
 * Invariants critiques:
 * - Pas d allocation dans process.
 * - Evenements differes tries par (ppq, sequence).
 * - Sync inter-lanes copie curseurs ET file differree.
 */
class ArpeggiatorProcessor
{
public:
    // IMPORTANT naming note:
    // - Internal layer aliases Pattern/Range are kept for param-ID compatibility.
    // - UI semantics:
    //   Arp  => Jump / Octave
    //   Drum => Velo Env / Tim Env
    // Runtime sequencer layers exposed by current Arp/Drum engine:
    // 0=Rythm, 1=Strum/Hit, 2=Jump/VeloEnv, 3=Octave/TimEnv,
    // 4=Velocity, 5=Groove, 6=Gate, 7=Retrig.
    static constexpr int kNumSequencerLayers = 8;
    static constexpr int kMaxDeferredEvents = 8192;
    static constexpr int kNumMorphLayers = 8;

    struct ExternalMorphModulations
    {
        // Deltas normalises [-1..+1] appliques aux 8 morph layers.
        // Cette modulation est additive a la valeur de base APVTS.
        std::array<float, kNumMorphLayers> normDeltas {};
    };

    /**
     * @brief Snapshot transport pour un bloc audio.
     *
     * Origine:
     * - Fourni par PluginProcessor avant appel process().
     *
     * Remarque:
     * - `ppqAtBlockStart` est la reference temporelle de conversion
     *   sample <-> ppq sur ce bloc.
     */
    struct ProcessContext
    {
        double sampleRate = 44100.0;
        double bpm = 120.0;
        double ppqAtBlockStart = 0.0;
        bool isPlaying = false;
        bool justStarted = false;
        int numSamples = 0;
    };

    /**
     * @brief Construit le moteur d une lane.
     *
     * Thread: init (hors audio hot path).
     * RT-safe: non requis a la construction.
     */
    explicit ArpeggiatorProcessor(juce::AudioProcessorValueTreeState& vts,
                                  Lanes::Lane laneIn);

    ~ArpeggiatorProcessor() = default;

    struct ClockSyncState
    {
        // Snapshot complet pour phase-lock inter-lanes.
        // Inclut timeline, etat musical derive et file differree.
        bool valid = false;
        // Fingerprint du snapshot de notes source:
        // - sert a verifier que deux lanes jouent le meme pool de notes tenues
        //   avant de partager une queue differree.
        uint64_t heldNoteFingerprint = 0;
        int heldNoteCount = 0;
        bool wasPlaying = false;
        bool hasLastBlockPpq = false;
        double lastBlockPpq = 0.0;
        double nextRateStepPpq = 0.0;
        std::array<double, kNumSequencerLayers> nextUnlinkStepPpqByLayer {};
        std::array<int, kNumSequencerLayers> playbackCursorByLayer {};
        std::array<int, kNumSequencerLayers> playbackStepByLayer {};
        std::array<uint8_t, kNumSequencerLayers> lastLinkModeByLayer {};
        int currentDirectionMode = 1;
        int currentJumpSize = 1;
        double currentStepDurationBeats = 0.25;
        int currentOctaveOffset = 0;
        bool currentIsDrumMode = false;
        // Drum Hit semantic (historical internal name kept for compatibility):
        // -1=O(silence), 0=X, 1=Flam, 2=Drag
        int currentGraceCount = 0;
        int currentDrumVeloEnvModeInt = 0;
        int currentVelocityChoice = 0;
        int currentGrooveChoice = 0;
        int currentGateChoice = 0;
        int currentRetrigCount = 1;
        // Tim Env internal state (historical internal name "Pace").
        int currentPaceModeInt = 0;
        int arpWalkIndex = -1;
        int64_t randomSeed = 0;
        int deferredEventCount = 0;
        uint64_t deferredEventSequenceCounter = 1;
        std::array<uint64_t, kMaxDeferredEvents> deferredSequence;
        std::array<double, kMaxDeferredEvents> deferredPpq;
        std::array<double, kMaxDeferredEvents> deferredOffOrExtendPpq;
        std::array<int, kMaxDeferredEvents> deferredChannel1;
        std::array<int, kMaxDeferredEvents> deferredNote;
        std::array<int, kMaxDeferredEvents> deferredVelocity;
        std::array<int, kMaxDeferredEvents> deferredGateMode;
        std::array<uint8_t, kMaxDeferredEvents> deferredType;
    };

    struct DeferredQueueTelemetry
    {
        // Outils de diagnostic: detecte surcharge de queue differree.
        uint32_t peakDepth = 0;
        uint32_t droppedInvalidPpq = 0;
        uint32_t droppedOverflowLate = 0;
        uint32_t replacedOverflow = 0;
        uint32_t prioritizedNoteOnReplacements = 0;
        bool overflowed = false;
    };

    /**
     * @brief Reset complet de l etat runtime (held/generated/deferred).
     *
     * Usage:
     * - panic, stop fort, reset transport.
     *
     * Thread: audio thread ou reset path.
     * RT-safe: oui.
     */
    void resetAllTrackingRT() noexcept;

    /**
     * @brief Traite un bloc MIDI lane in-place.
     *
     * Thread: audio thread.
     * RT-safe: oui.
     *
     * Ordre logique:
     * - Drain des evenements differes dus.
     * - Drain des note off planifiees.
     * - Avance de ticks selon rate.
     * - Generation des note on/off du tick.
     *
     * @param midi Buffer entree/sortie lane.
     * @param ctx Contexte transport du bloc.
     */
    void process(juce::MidiBuffer& midi, const ProcessContext& ctx);

    /**
     * @brief Exporte l etat de clock + queue pour phase-lock inter-lanes.
     *
     * Thread: audio thread.
     * RT-safe: oui.
     */
    void getClockSyncState(ClockSyncState& state) const noexcept;

    /**
     * @brief Importe l etat de clock + queue pour phase-lock inter-lanes.
     *
     * Thread: audio thread.
     * RT-safe: oui.
     */
    void applyClockSyncState(const ClockSyncState& state) noexcept;

    /**
     * @brief Lit les compteurs de telemetrie de queue differree.
     *
     * Thread: audio ou polling UI.
     * RT-safe: oui.
     */
    void getDeferredQueueTelemetry(DeferredQueueTelemetry& telemetry) const noexcept;

    /**
     * @brief Remet a zero les compteurs de telemetrie de queue differree.
     *
     * Thread: audio ou reset path.
     * RT-safe: oui.
     */
    void resetDeferredQueueTelemetry() noexcept;

    /**
     * @brief Injecte des deltas morph externes pour le bloc.
     *
     * Thread: audio thread.
     * RT-safe: oui.
     */
    void setExternalMorphModulations(const ExternalMorphModulations& mod) noexcept;

    /**
     * @brief Retourne le pas de lecture courant pour une couche sequencer.
     *
     * Thread:
     * - Ecriture sur audio thread.
     * - Lecture possible depuis thread UI (best-effort visuel).
     *
     * RT-safe: oui.
     *
     * @param layerIndex0Based Index de couche [0..7] (Rate..Retrig).
     * @return Pas courant [0..31] ou -1 si inactif/non valide.
     */
    int getPlaybackStepForLayer(int layerIndex0Based) const noexcept;

private:
    static constexpr int kNumMidiChannels    = 16;
    static constexpr int kNumNotes           = 128;
    static constexpr int kVelocityStackDepth = 16;
    static constexpr int kNumLayers          = kNumSequencerLayers;
    static constexpr int kNumSteps           = 32;
    static constexpr int kMaxHeldKeys        = kNumMidiChannels * kNumNotes;

    static constexpr int kLayerRate      = 0;
    static constexpr int kLayerDirection = 1;
    static constexpr int kLayerPattern   = 2;
    static constexpr int kLayerRange     = 3;
    static constexpr int kLayerVelocity  = 4;
    static constexpr int kLayerGroove    = 5;
    static constexpr int kLayerGate      = 6;
    static constexpr int kLayerAccent    = 7;
    static constexpr int kNumDrumIndependentLayers = 3;
    static constexpr int kLinkModeLink   = 0;
    static constexpr int kLinkModeF      = 1;
    static constexpr int kLinkModeUnlink = 2;

    static constexpr int kRateTypeCount      = 6;
    static constexpr int kRateRatiosPerType  = 7;
    static constexpr int kRateChoiceCount    = kRateTypeCount * kRateRatiosPerType; // 42
    static constexpr int kRateRndIndex       = kRateChoiceCount + 1; // 43
    static constexpr int kDirectionTopIndex = 1;
    static constexpr int kDirectionUpIndex = 2;
    static constexpr int kDirectionEqualIndex = 3;
    static constexpr int kDirectionDownIndex = 4;
    static constexpr int kDirectionBottomIndex = 5;
    static constexpr int kDirectionChordAllIndex = 6;
    static constexpr int kDirectionUpPairIndex = 7;
    static constexpr int kDirectionDownPairIndex = 8;
    static constexpr int kDirectionMuteIndex = 9;
    static constexpr int kDirectionRndIndex = 10;
    static constexpr int kDirectionChordSpreadLastIndex = 22;
    static constexpr int kDirectionChordRndIndex = 23;
    static constexpr int kPatternRndIndex   = 9;
    static constexpr int kRangeChoiceFirst  = 1;   // -4 oct
    static constexpr int kRangeChoiceLast   = 9;   // +4 oct
    static constexpr int kRangeRndIndex     = 10;
    static constexpr int kDrumHitChoiceFirst   = 1; // X
    static constexpr int kDrumHitChoiceLast    = 4; // Drag
    static constexpr int kDrumHitChoiceX       = 1;
    static constexpr int kDrumHitChoiceSilence = 2; // O
    static constexpr int kDrumHitChoiceFlam    = 3;
    static constexpr int kDrumHitChoiceDrag    = 4;
    static constexpr int kDrumEnvChoiceFirst   = 1; // "0"
    static constexpr int kDrumEnvChoiceLast    = 5; // rlog
    static constexpr int kDrumEnvRndIndex      = 6; // rnd
    static constexpr int kAccentChoiceFirst = 1;   // x1
    static constexpr int kAccentChoiceLast  = 8;   // x8
    static constexpr int kAccentRndIndex    = 9;
    // GateMode: contrat de duree des notes generees.
    enum class GateMode
    {
        Skip,
        Timed,
        Legato
    };

    // Tim Env repartition for retrigs.
    // Internal enum name "PaceMode" is kept to avoid broad churn.
    enum class PaceMode
    {
        Equal,
        Lin,
        Log,
        RevLin,
        RevLog
    };

    enum class DrumVeloEnvMode
    {
        Flat,
        Lin,
        Log,
        RevLin,
        RevLog
    };

    // Type d evenement stocke en file differree.
    // Current engine only enqueues NoteOn and Mute.
    enum class DeferredEventType : uint8_t
    {
        NoteOn = 0,
        Mute
    };

    struct DeferredEvent
    {
        // Cle de tri:
        // - ppq d abord
        // - sequence ensuite pour stabiliser l ordre a ppq egal.
        double ppq = 0.0;
        double offOrExtendPpq = 0.0;
        uint64_t sequence = 0;
        int channel1 = 1;
        int note = 0;
        int velocity = 100;
        GateMode gateMode = GateMode::Skip;
        DeferredEventType type = DeferredEventType::NoteOn;
    };

    //--- lecture params --------------------------------------------------------
    static bool isNoteOnMessage(const juce::MidiMessage& msg) noexcept;
    static bool isNoteOffMessage(const juce::MidiMessage& msg) noexcept;

    bool readEnabledRT() const noexcept;
    int readModeRT() const noexcept;
    int readStepChoiceForModeRT(int modeIndex,
                                int layer,
                                int step0Based) const noexcept;
    juce::AudioParameterChoice* getStepChoiceParamRT(int modeIndex,
                                                      int layer,
                                                      int step0Based) const noexcept;
    int readStepChoiceRT(int layer, int step0Based) const noexcept;
    int readLinkModeRT(int layer) const noexcept;
    int readUnlinkRateChoiceRT(int layer) const noexcept;
    int readMorphRT(int layer) const noexcept;
    //--- etat interne ----------------------------------------------------------
    // Lecture O(1) de l etat "au moins une note tenue".
    // Evite un scan complet 16x128 dans le hot path.
    bool hasAnyHeldNotesRT() const noexcept;
    void resetLinkedSequencersForIdle(double ppqNow) noexcept;
    // Reagit aux changements de link en live (UI/thread message) sans casser
    // la coherence musicale du tick courant.
    // Point sensible Jump:
    // - Jump utilise arpWalkIndex comme memoire de parcours.
    // - toute transition link/unlink de la couche Jump doit re-primer ce curseur
    //   pour eviter des sauts non predictibles.
    void reconcileLinkTransitions(double ppqNow) noexcept;

    void handleTrackedNoteOn(int channel1, int note, uint8_t velocity) noexcept;
    void handleTrackedNoteOff(int channel1, int note) noexcept;
    // Cache compact des touches tenues, trie note asc puis channel asc.
    // Objectif: eviter un scan complet 16x128 a chaque tick.
    void insertHeldKeySortedRT(int ch0, int note) noexcept;
    void removeHeldKeySortedRT(int ch0, int note) noexcept;
    void rebuildHeldKeyCacheFromMatrixRT() noexcept;

    int applyModifierToStepChoice(int layer, int choiceIndex) const noexcept;
    // Resolve the next playable (non-Skip) step from a given cursor.
    // Used to ensure Skip steps are traversed immediately between 2 musical steps.
    int findNextNonSkipStepForLayerForModeRT(int modeIndex,
                                             int layer,
                                             int startStep0Based,
                                             int safeLayerLength) const noexcept;
    // Calcule la longueur effective d une couche pour un mode explicite.
    // Evite de relire le mode arp/drum dans les scans O(steps) du hot path.
    int effectiveSequenceLengthForLayerForModeRT(int layer, int modeIndex) const noexcept;
    int effectiveSequenceLengthForLayerRT(int layer) const noexcept;
    void computeEffectiveSequenceLengthsForModeRT(int modeIndex,
                                                  std::array<int, kNumLayers>& outLengths) const noexcept;
    void sanitizePlaybackCursorsToLengthsRT() noexcept;
    double beatsForRateChoice(int choiceIndex) const noexcept;

    //--- timeline et steps -----------------------------------------------------
    void clearPlaybackState() noexcept;
    void resetPlaybackState(double ppqNow) noexcept;
    bool triggerOneRateStep() noexcept;
    bool triggerOneUnlinkLayerStep(int layer) noexcept;
    void resolveMusicalStateForCurrentTick() noexcept;

    //--- generation MIDI -------------------------------------------------------
    void flushAllGeneratedNotes(juce::MidiBuffer& out, int samplePos) noexcept;
    void emitBypassHeldSnapshot(juce::MidiBuffer& out, int samplePos) noexcept;
    void emitBypassHeldNoteOffSnapshot(juce::MidiBuffer& out, int samplePos) noexcept;

    void emitDueScheduledOffs(juce::MidiBuffer& out,
                              const ProcessContext& ctx,
                              int sampleEndExclusive) noexcept;
    void runTicksUntil(juce::MidiBuffer& out,
                       const ProcessContext& ctx,
                       int sampleEndExclusive) noexcept;

    int gatherHeldKeysSorted(int* outKeys, int maxKeys) const noexcept;
    uint64_t computeHeldNoteFingerprint() const noexcept;
    // Determine the seed note index used when arpWalkIndex is uninitialized.
    // Rule:
    // - scan Direction sequence from step 1 to effective length
    // - ignore non-significant choices ("=", chord modes, mute, skip)
    // - first significant choice defines the seed boundary/random start
    // - fallback with no significant step: highest note
    int resolveInitialArpWalkIndexForSequence(int heldCount) noexcept;
    // Selectionne le rang de note source pour Top/Up/=/Down/Bottom.
    // Invariant:
    // - wrap deterministe via modulo.
    // - le saut est borne a [1..8] et derive de Jump.
    // - si la pool change de taille, le curseur est re-synchronise proprement.
    int chooseArpKeyIndex(int heldCount) noexcept;
    double paceWeightForSlot(int slotIndex,
                             int slotCount,
                             PaceMode mode) const noexcept;
    int buildPacedOffsets(int repeatCount,
                          double stepDurationBeats,
                          bool includeEndPoint,
                          PaceMode mode,
                          double* outOffsets,
                          int maxOffsets) const noexcept;
    int applyDrumVeloEnvelopeToVelocity(int baseVelocity,
                                        int retrigIndex,
                                        int retrigCount,
                                        DrumVeloEnvMode mode) const noexcept;

    bool resolveGrooveOffsetBeats(int grooveChoice,
                                  double stepDurationBeats,
                                  double& offsetBeats) noexcept;
    bool resolveGateForStep(int gateChoice,
                            double stepDurationBeats,
                            GateMode& gateMode,
                            double& gateDurationBeats) noexcept;

    void scheduleGeneratedOff(int channel1, int note, double offPpq) noexcept;
    void emitGeneratedNoteOn(juce::MidiBuffer& out,
                             int samplePos,
                             int channel1,
                             int note,
                             int velocity,
                             GateMode gateMode,
                             double offPpq) noexcept;
    void processTickAtPpq(juce::MidiBuffer& out,
                          const ProcessContext& ctx,
                          double tickPpq,
                          int tickSample) noexcept;
    //--- queue differree -------------------------------------------------------
    void emitDueDeferredEvents(juce::MidiBuffer& out,
                               const ProcessContext& ctx,
                               int sampleEndExclusive) noexcept;
    static bool deferredEventComesBefore(const DeferredEvent& a,
                                         const DeferredEvent& b) noexcept;
    static bool deferredEventHeapCompare(const DeferredEvent& a,
                                         const DeferredEvent& b) noexcept;
    void rebuildDeferredEventHeap() noexcept;
    void enqueueDeferredEvent(const DeferredEvent& event) noexcept;
    void enqueueDeferredNoteOn(double onsetPpq,
                               int channel1,
                               int note,
                               int velocity,
                               GateMode gateMode,
                               double offPpq) noexcept;
    void enqueueDeferredMute(double atPpq) noexcept;
    void clearDeferredEvents() noexcept;
    uint16_t beginSelectionStampGeneration() noexcept;

    //--- conversion temps ------------------------------------------------------
    int ppqToSampleClamped(const ProcessContext& ctx, double ppq) const noexcept;
    double sampleToPpq(const ProcessContext& ctx, int samplePos) const noexcept;

    // References externes
    juce::AudioProcessorValueTreeState& parameters;
    const Lanes::Lane lane;

    // Cache de pointeurs params:
    // evite lookup string repetes dans le hot path.
    std::atomic<float>* arpeggiatorEnableRaw = nullptr;
    std::atomic<float>* arpeggiatorModeRaw = nullptr;
    std::array<std::atomic<float>*, kNumMorphLayers> arpMorphRaw {};
    ExternalMorphModulations externalMorphModulations;
    std::array<std::atomic<float>*, kNumLayers> linkRaw {};
    std::array<std::atomic<float>*, kNumLayers> unlinkRateRaw {};
    std::array<uint8_t, kNumLayers> lastLinkState {};
    std::array<std::array<juce::AudioParameterChoice*, kNumSteps>, kNumLayers> stepChoices {};
    std::array<std::array<juce::AudioParameterChoice*, kNumSteps>, kNumDrumIndependentLayers> drumStepChoices {};
    bool lastEnabled = true;
    bool wasPlaying = false;
    // Mode R idle-reset latch:
    // - true once resetLinkedSequencersForIdle() has been applied in current
    //   idle phase (no held notes).
    // - prevents repeated timeline rebases at every segment boundary.
    bool rateIdleResetLatched = false;
    bool hasLastBlockPpq = false;
    double lastBlockPpq = 0.0;

    // Held note tracking (source)
    // Invariant: heldCount[ch][note] > 0 => source active.
    std::array<std::array<uint8_t,  kNumNotes>, kNumMidiChannels> heldCount {};
    std::array<std::array<uint8_t,  kNumNotes>, kNumMidiChannels> heldVelocity {};
    std::array<std::array<std::array<uint8_t, kVelocityStackDepth>, kNumNotes>, kNumMidiChannels> heldVelocityStack {};
    std::array<std::array<uint8_t,  kNumNotes>, kNumMidiChannels> heldVelocityStackSize {};
    std::array<std::array<uint64_t, kNumNotes>, kNumMidiChannels> heldTimestamp {};
    // Cache O(nHeld) utilise par gatherHeldKeysSorted().
    // Chaque entree encode (channel,note) sous forme key = ch0 * 128 + note.
    std::array<int, kMaxHeldKeys> heldKeysSorted {};
    int heldKeysSortedCount = 0;
    // Compteur global des touches actives (channel,note) avec heldCount > 0.
    // Maintenu sur les transitions 0->1 et 1->0 pour rester O(1) en lecture.
    int heldActiveNoteCount = 0;
    uint64_t timestampCounter = 0;

    // Generated note tracking (sortie arpeggiator).
    std::array<std::array<uint8_t,  kNumNotes>, kNumMidiChannels> generatedActive {};
    std::array<std::array<uint8_t,  kNumNotes>, kNumMidiChannels> generatedVelocity {};
    std::array<std::array<uint8_t,  kNumNotes>, kNumMidiChannels> scheduledOffActive {};
    std::array<std::array<double,   kNumNotes>, kNumMidiChannels> scheduledOffPpq {};
    // Compteurs auxiliaires pour court-circuiter des scans complets 16x128
    // quand aucun etat n est actif.
    // - generatedActiveCount: nombre de (ch,note) actuellement actifs en sortie arp.
    // - scheduledOffActiveCount: nombre de note-off planifies en attente.
    int generatedActiveCount = 0;
    int scheduledOffActiveCount = 0;

    std::array<int, kNumLayers> playbackCursorByLayer {};
    std::array<int, kNumLayers> playbackStepByLayer {};
    double nextRateStepPpq = 0.0;
    std::array<double, kNumLayers> nextUnlinkStepPpqByLayer {};

    int currentDirectionMode = 1;
    int currentJumpSize = 1;
    double currentStepDurationBeats = 0.25;
    int currentOctaveOffset = 0;
    bool currentIsDrumMode = false;
    // Drum Hit semantic (historical internal name kept for compatibility):
    // -1=O(silence), 0=X, 1=Flam, 2=Drag
    int currentGraceCount = 0;
    DrumVeloEnvMode currentDrumVeloEnvMode = DrumVeloEnvMode::Flat;
    // Tick-resolved per-layer choices.
    // Stored once in resolveMusicalStateForCurrentTick() to keep processTickAtPpq()
    // deterministic and cheaper under heavy realtime load.
    int currentVelocityChoice = 0;
    int currentGrooveChoice = 0;
    int currentGateChoice = 0;
    // Retrig desire (x1..x8) issu de la couche Retrig.
    // IMPORTANT: au rendu, un pas chord force toujours le retrig effectif a x1.
    int currentRetrigCount = 1;
    // Tim Env runtime state (internal historical name "Pace").
    PaceMode currentPaceMode = PaceMode::Equal;

    int arpWalkIndex = -1;

    juce::Random playbackRandom { 0x51A4C31u };
    // Scratch buffers reutilises sur audio thread pour eviter de gros tableaux
    // automatiques par tick (reduction de pression cache/stack).
    std::array<int, kMaxHeldKeys> keyScratch {};
    std::array<int, kMaxHeldKeys> selectedSrcKeysScratch {};
    std::array<int, kMaxHeldKeys> selectedOutNotesScratch {};
    std::array<int, kMaxHeldKeys> selectedSourceVelocitiesScratch {};
    std::array<std::array<uint16_t, kNumNotes>, kNumMidiChannels> selectedSeenStamp {};
    uint16_t selectedSeenGeneration = 1;

    // Queue differree:
    // - capacite fixe (pas d allocation RT).
    // - maintenue en min-heap sur (ppq, sequence).
    std::array<DeferredEvent, kMaxDeferredEvents> deferredEvents {};
    int deferredEventCount = 0;
    uint64_t deferredEventSequenceCounter = 1;
    DeferredEvent deferredWorstEvent {};
    bool deferredWorstValid = false;
    DeferredQueueTelemetry deferredTelemetry {};

    juce::MidiBuffer outScratch;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ArpeggiatorProcessor)
};
