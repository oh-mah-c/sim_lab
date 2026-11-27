#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <map>
#include <vector>
#include "Types.hpp"
#include "Sensor.hpp"
#include "BrainNet.hpp"

// CL-1 cognitive brain — bio-inspired modular spiking connectome
// (brain.txt spec, save format v29). Replaces the legacy monolithic
// MLP/minGRU block: BrainSlot now owns a BrainNet (12 LIF modules, CSR
// synapses, delay rings, multi-rate scheduler) plus the same cognitive
// memory buffers (replay ring / tried combos) that AgentMind and the
// archaeology pipeline already rely on.
//
// Hard rules (AGENTS.md):
//  - integer-only, no floats anywhere in state or math;
//  - deterministic across build modes and thread counts: module order,
//    neuron order, synapse order and kick-ring insertion order are fixed;
//  - no wall-clock or global RNG: connectomes derive from
//    (world_seed, entity, genome) via hash only;
//  - no heap in neural hot paths: BrainNet vectors are sized once at
//    init/load and never resized inside tick()/decide().
//
// Heritability: the connectome is NOT stored as edges — BrainGenome::slow
// is a compact integer parameter block (kConnectomeBytes) that encodes
// module populations, LIF baselines, E/I ratio, recurrence density and
// per-tract gains. Crossover/mutation operate on those bytes; the actual
// synapses are re-derived deterministically at birth, so two agents with
// the same genome+seed always grow the same brain.

namespace simlab {

class World;
struct BrainGenome;

// ---- connectome parameter block ----------------------------------------------
// BrainGenome::slow is exactly kConnectomeBytes int8 lanes:
//   [0..11]   module populations (0..100 neurons; 0 = module disabled)
//   [12]      threshold code  = threshold/8      (40..160 -> 320..1280)
//   [13]      reset code      = (reset+512)/16   (0..64  -> -512..+512)
//   [14]      leak_shift      (3..8)
//   [15]      refractory      (1..6)
//   [16]      ei_permille/8   (0..127 -> 0..1016 inhibitory neurons)
//   [17]      density_permille/8 (0..127 -> local recurrence density)
//   [18..41]  per-tract gain/2 (0..127 -> 0..254; 0 = table default)
//   [42]      stdp_window ticks (0 -> default 6)
//   [43]      plastic_rate /4 permille
//   [44]      reward_rate /4 permille
//   [45]      neuromodulator sensitivity /8 permille (0..127 -> 0..1016)
//   [46]      rollout budget /8 permille (0..127 -> planning frequency)
//   [47]      population profile (0=min 0.5x, 1=std 1x, 2=full 1.5x)
//   [48]      empathy/social sensitivity /8 permille (0..127 -> 0..1016);
//             scales the negative RPE of unprovoked conspecific harm
//   [49..95]  reserved
constexpr std::size_t kConnectomeBytes = 96;

// Decoded + sanitized connectome parameters (what BrainNet::init consumes).
struct BrainNetParams {
    std::array<std::uint16_t, kBrainModuleCount> pops{};
    std::int16_t threshold = 800;
    std::int16_t reset = -200;
    std::uint8_t leak_shift = 5;
    std::uint8_t refractory = 2;
    std::uint8_t ei_permille = 200;
    std::uint16_t density_permille = 120;
    std::array<std::int16_t, kBrainTractCount> tract_gain{};
    // Phase 6 plasticity (genome slow[42..44], heritable):
    std::uint8_t stdp_window = 6;      // causal window, neural ticks
    std::int32_t plastic_rate = 400;   // permille STDP step scale
    std::int32_t reward_rate = 400;    // permille reward-mod step scale
    // Phase 9 heritable cognitive/genetic params:
    std::int32_t mod_sens_pm = 1000;   // neuromodulator rate sensitivity
    std::int32_t rollout_budget_pm = 1024;  // planning frequency permille
    std::int32_t profile = 1;          // 0=min,1=std,2=full population
};

// Decode genome.slow into params, clamping every lane to its domain.
// Invalid/short genomes yield the default parameter set.
BrainNetParams brainnet_params_of(const BrainGenome& g);
// Clamp every lane of a connectome parameter block in place.
void brainnet_sanitize(std::vector<std::int8_t>& slow);

// ---- hash-driven mutation (kept: generic int8 block tools) --------------------
std::int8_t brain_rand_weight(std::uint64_t seed, std::uint64_t salt,
                              std::uint64_t index, std::int32_t spread);
std::int32_t brain_mutation_step(std::uint64_t seed, std::uint64_t entity,
                                 std::uint64_t tick, std::uint64_t block,
                                 std::int32_t step);
void brain_mutate_blocks(std::int8_t* w, std::int64_t n_weights,
                         std::uint64_t seed, std::uint64_t entity,
                         std::uint64_t tick, std::int32_t step,
                         std::int32_t mut_permille);

// ---- BrainArena ---------------------------------------------------------------
// Deterministic bump+free-list allocator over one slab, carved into
// 64-byte cells. Retained for BrainStore/save compatibility; the v29
// connectome keeps its SoA state in BrainSlot::net (preallocated vectors)
// rather than arena offsets.
class BrainArena {
public:
    static constexpr std::int32_t kCell = 64;

    BrainArena() = default;
    explicit BrainArena(std::uint64_t cells) { reserve(cells); }

    void reserve(std::uint64_t cells);

    static constexpr std::uint64_t kInvalid = ~std::uint64_t{0};
    std::uint64_t alloc(std::uint64_t bytes);
    void free(std::uint64_t offset, std::uint64_t bytes);

    std::int8_t* w8(std::uint64_t offset) {
        return reinterpret_cast<std::int8_t*>(slab_.data()) +
               slab_base_off_ + offset;
    }
    const std::int8_t* w8(std::uint64_t offset) const {
        return reinterpret_cast<const std::int8_t*>(slab_.data()) +
               slab_base_off_ + offset;
    }
    std::uint64_t used_cells() const { return high_water_; }
    std::uint64_t capacity_cells() const { return slab_cells_; }
    std::uint64_t live_bytes() const { return live_bytes_; }
    const std::vector<std::vector<std::uint64_t>>& free_lists() const {
        return free_;
    }
    void restore_state(std::uint64_t cells, std::uint64_t high_water,
                       std::uint64_t live_bytes,
                       const std::vector<std::vector<std::uint64_t>>& fl) {
        reserve(cells);
        high_water_ = high_water; live_bytes_ = live_bytes; free_ = fl;
    }

private:
    struct alignas(64) Cell { std::uint64_t v; };
    std::vector<Cell> slab_;
    std::uint64_t slab_base_off_ = 0;
    std::uint64_t slab_cells_ = 0;
    std::uint64_t high_water_ = 0;
    std::uint64_t live_bytes_ = 0;
    std::vector<std::vector<std::uint64_t>> free_;
};

// ---- experience record ---------------------------------------------------------
// One committed (s,a)->y transition. wh/w1h carry the engram pattern that
// brain_assimilate injects (first 96/64 readout neurons' activity) so the
// archaeology/engraving pipeline (EngramPayload codec, SocialLaws) keeps
// working unchanged — they are neural ensemble patterns now, not WM
// activations.
struct Transition {
    SensorVec s{};
    ActionVec a{};
    LabelVec  y{};
    std::array<std::int8_t, 96> wh{};
    std::array<std::int8_t, 64> w1h{};
    std::uint16_t sigma = 0;
    std::uint16_t charge = 0;
    std::uint64_t event_id = 0;
    bool valid = false;
};

// ---- Phase 3-5 state: neuromodulators, emotion, homeostasis, conditioning ---
// (brain.txt §§9-12, 14). All integer, all deterministic, all serialized.
enum BrainModIdx : int {
    kBModDopa = 0, kBModSero, kBModNorepi, kBModAchi,
    kBModGaba, kBModGlut, kBModCortisol, kBModCount
};
enum BrainEmoIdx : int {
    kBEmoFear = 0, kBEmoAnger, kBEmoCuriosity, kBEmoSadness,
    kBEmoJoy, kBEmoDisgust, kBEmoSurprise, kBEmoTrust,
    kBEmoAttachment, kBEmoCount
};
// Sensory modality groups for thalamic gating (channel ranges per
// Sensor.hpp layout): 0=visual/extero, 1=social/auditory, 2=somatosensory
// + intero, 3=motor-efference copy.
enum BrainModGroup : int { kBGrpVisual = 0, kBGrpSocial, kBGrpSomatic,
                           kBGrpMotor, kBGrpCount };
struct BrainVital {
    std::array<std::int16_t, kBModCount> mod{};   // 0..1000
    std::array<std::int16_t, kBEmoCount> emo{};   // -1000..1000
    // Hypothalamic drives (spec §4.5): 0..1000, never assign actions.
    std::int16_t drive_hunger = 0, drive_thirst = 0, drive_fatigue = 0;
    std::int16_t drive_temp = 0, drive_stress = 0, drive_repro = 0;
    // Amygdala fear conditioning: bounded context->valence traces.
    // ctx = hash(material, pos bucket, kind); learned, never hard-coded.
    struct FearTrace {
        std::uint64_t ctx = 0;
        std::int16_t valence = 0;    // -1000..1000
        std::int16_t strength = 0;  // 0..1000
        std::uint64_t last = 0;     // tick of last reinforcement
    };
    std::array<FearTrace, 8> fears{};
    // Thalamic attention gains, Q8 (256 = unity) per modality group.
    std::array<std::int16_t, kBGrpCount> attn_gain{};
    std::uint64_t attn_last = 0;
    std::int16_t sleep_pressure = 0;   // 0..1000
    // Slow neuromodulator accumulator (sub-tick remainder, Euclid-style).
    std::uint64_t mod_accum = 0;
    // Working memory (spec §10.2): last committed decision summary.
    std::int32_t wm_verb = -1;
    std::int64_t wm_target = 0;
    std::int16_t wm_conf = 0;
    // Phasic teaching signals (set by brain_post, consumed by vital_tick):
    // reward prediction error, surprise magnitude, emotional charge.
    std::int16_t last_reward = 0;
    std::int16_t last_sigma = 0;
    std::int16_t last_charge = 0;
    // Phase 6 reward prediction error (spec §13.2): per-verb expected
    // outcome (EMA of label ch24) + last computed RPE = actual - expected.
    std::array<std::int16_t, kActionVerbs> exp_rew{};
    std::int16_t last_rpe = 0;
};

// ---- Phase 7-8 state: goals, social cognition, planning ----------------------
// (brain.txt §§15-21). Integer, deterministic, serialized in v29.
struct BrainGoal {
    std::int32_t verb = -1;        // suggested verb index (-1 = none)
    std::int64_t target = 0;       // remembered entity or 0
    std::int16_t priority = 0;     // 0..1000
    std::uint8_t origin = 0;       // drive category code
    std::uint64_t last = 0;        // tick last refreshed
    bool active = false;
};
struct BrainSocial {
    std::uint64_t key = 0;         // hash of conspecific entity id
    std::int16_t trust = 0;        // -1000..1000 (own experience only)
    std::int16_t familiarity = 0;  // 0..1000 exposure count (saturating)
    std::uint64_t last = 0;
};
struct BrainCognition {
    std::array<BrainGoal, 8> goals{};
    std::array<BrainSocial, 8> social{};
    std::int16_t comm_rel = 0;      // communication reliability -1000..1000
    std::int16_t conflict = 0;      // last goal-conflict magnitude 0..1000
    std::int16_t plan_trigger = 0;  // set by brain_post, consumed by decide
    std::uint64_t plan_ops = 0;     // rollout ops spent (diagnostic)
};

// Pre-commit snapshot filled by brain_pre() and consumed by brain_post().
struct BrainPending {
    bool valid = false;
    SensorVec s{};
    ActionVec a{};
    std::uint64_t tick = 0;
    MaterialId pm_a = kNoMaterial;
    MaterialId pm_b = kNoMaterial;
    Vec2 pm_pos{0, 0};        // actor position at pre (cerebellar error)
    EntityId pm_target = kNoEntity;  // first interact target (social learning)
};

// The inheritable brain. `slow` is the kConnectomeBytes connectome
// parameter block; fast weights / neural state are NEVER inherited
// (Baldwin: only architecture params cross the generational boundary).
struct BrainGenome {
    bool valid = false;
    std::int32_t hidden = 64;        // legacy field, unused by v29 brain
    std::int32_t lr_permille = 400;  // future plasticity rate (Phase 6)
    std::int32_t trace_decay = 900;  // future eligibility decay (Phase 6)
    std::int32_t mut_scale = 500;    // birth mutation permille
    std::vector<std::int8_t> slow;   // connectome params, kConnectomeBytes
};

struct BrainSlot {
    BrainGenome genome;          // heritable connectome params
    BrainNet net;                // spiking state (membranes, synapses, rings)
    BrainVital vital;            // neuromod/emotion/homeostasis/conditioning
    BrainCognition cog;          // goals/social/planning (Phase 7-8)
    ActionVec last_action{};     // feeds sensor channels 80..95
    BrainPending pending{};
    std::array<Transition, 16> replay{};
    std::int32_t replay_next = 0;
    std::int32_t replay_count = 0;
    // MACs spent by brain_decide() since the last metabolize pass, plus the
    // sub-unit energy remainder (same convention as mind_brain_accum).
    std::int64_t mac_pending = 0;
    std::int64_t mac_frac = 0;
    // Open-ended invention: ring of material-combo keys already transformed.
    // Lifetime-only (Baldwin), never inherited.
    std::array<std::uint64_t, 64> tried{};
    std::int32_t tried_next = 0;
    // Pure ALife: no synthetic-target caches live here. Movement,
    // foraging and social approach must emerge from the connectome +
    // memory readout — the engine keeps no hand-computed destinations.
};

// Canonical default connectome genome (Standard profile).
BrainGenome brain_default_genome();

// Reproductive isolation threshold: mean |Δparam| over the connectome
// block, permille of the 255 int8 range.
constexpr std::int64_t kMaxBrainGeneticDistance = 180;

// Per-world brain bank. Shared via shared_ptr so speculative clones see
// the same store but MUST NOT write it (guarded by World::speculative).
struct BrainStore {
    BrainArena arena;
    std::map<EntityId, BrainSlot> slots;
    // shadow-mode observability stats (not part of state_hash)
    std::int64_t wm_err_sum = 0;
    std::int64_t wm_n = 0;
    std::int64_t sigma_sum = 0;
    std::int64_t sigma_n = 0;
    std::int64_t surprise_events = 0;
    // NOTE: per-net spike counts live in BrainNet::spikes (serialized +
    // hashed per slot). A store-level aggregate would race under
    // parallel_think's concurrent brain_decide calls — do not re-add.
};

// ---- Phase 1 API ----------------------------------------------------------------
// Get-or-create the agent's brain slot: installs `genome`'s connectome
// params when provided, else the default genome; the synapses themselves
// are always re-derived from (world_seed, entity, params) so the same
// inputs grow the same brain. Deterministic per (world_seed, entity).
BrainSlot& brain_slot(World& w, BrainStore& store, EntityId npc,
                      const BrainGenome* genome = nullptr);
void brain_free_slot(BrainStore& store, EntityId npc);
void brain_gc(World& w, BrainStore& store);

// Pre-commit: encode s_t + action a_t into pending (and feed last_action
// into the net's motor reafference). Post-commit: push the replay
// transition and inject the outcome as a teaching current. Weight
// plasticity lands in Phase 6; until then post only maintains memory.
void brain_pre(World& w, EntityId npc, const InteractionContext& ctx);
void brain_post(World& w, EntityId npc, const CausalEvent& ev);

// Engraved-artifact knowledge transfer: inject the inscribed engram
// pattern (ancestor's neural ensemble: wh/w1h/y) into the reader's
// hippocampus/PFC and record it in the replay ring so it can be
// re-transmitted. `comp_permille` scales the injected current.
void brain_assimilate(World& w, EntityId npc, int verb,
                      std::int64_t charge, std::int64_t sigma,
                      std::uint64_t salt,
                      const std::int8_t* wh, const std::int8_t* w1h,
                      const std::int8_t* y, std::int64_t comp_permille);

// Deterministic fingerprint of ALL brain state (genome params + neural
// state + replay + tried) — FNV-1a over sorted slots.
std::uint64_t brain_state_hash(const BrainStore& store);

// ---- Phase 2 API -----------------------------------------------------------------
BrainGenome brain_genome_of(const World& w, const BrainStore& store,
                            EntityId npc);
std::int64_t brain_genetic_distance(const BrainGenome& a,
                                    const BrainGenome& b);
// Per-field/byte crossover on the connectome block + hyper-genes +
// brain_mutate_blocks at the child's mut_scale + sanitize.
BrainGenome brain_child_genome(const World& w, EntityId a, EntityId b,
                               const BrainGenome& ga, const BrainGenome& gb);

// The CL-1 decision: inject current sensor state into Sensory Cortex,
// run a bounded neural burst (net.tick x k), read Motor Cortex verb
// units + Association entity-slot readouts, mask, argmax with jitter.
// Mutates ONLY this agent's net (deterministic per call sequence).
struct BrainChoice {
    bool valid = false;
    std::string verb;
    EntityId target = kNoEntity;
    Vec2 move_to{0, 0};
    bool has_move = false;
    std::int64_t score = 0;
};
BrainChoice brain_decide(World& w, EntityId npc);

// Phase 6 observational learning (spec §13.4) + Phase 8 social cognition:
// a witnessed (actor,verb,outcome) event biases the observer's associations
// WITHOUT guaranteeing skill — small dopamine-gated current injection plus
// familiarity/trust bookkeeping. `via_comm` marks hearsay (communicated
// claim): trust moves at half rate and low comm_rel breeds suspicion.
// No-op when the observer has no brain slot yet.
void brain_observe(World& w, EntityId observer, EntityId actor, int verb,
                   std::int64_t outcome_valence, bool via_comm,
                   EntityId target = kNoEntity);

void brain_install_spawn(World& w, EntityId nid, const BrainGenome& g);

}  // namespace simlab
