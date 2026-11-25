#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <map>
#include <vector>
#include "Types.hpp"
#include "Sensor.hpp"

// CL-1 cognitive brain substrate (blueprint: ai_architecture_blueprint.html).
// Phase 0 foundation ONLY — arena, kernels, LUTs, hash RNG. No agent
// behavior changes; nothing here is wired into World/Driver yet.
//
// Hard rules (AGENTS.md):
//  - integer-only, no floats anywhere in state or math;
//  - deterministic across build modes: dot products accumulate in int32
//    in a fixed order, so AVX2/scalar produce bit-identical results;
//  - no wall-clock or global RNG: every random draw derives from
//    (world_seed, entity, tick, counter) via Rng::hash / stream_seed.
//
// Quantization convention:
//  - weights: int8 in [-127,127], zero = 0;
//  - activations/pre-activations: int8 in [-128,127];
//  - dense layer: y[o] = act( clamp_i8( (dot(w_row_o, x) >> shift) + bias[o] ) )
//    where `shift` is chosen by the caller so acc>>shift lands in int8 range.

namespace simlab {

class World;

// ---- activation LUTs -------------------------------------------------------
// 256-entry tables indexed by the raw int8 bit pattern (index = (uint8_t)v).
// Built once, lazily, with integer fixed-point only — bit-identical on
// every platform and build mode.
enum class BrainAct : std::int32_t { kLinear = 0, kTanh = 1, kSigmoid = 2 };
const std::int8_t* brain_act_lut(BrainAct act);   // never null, 256 entries

// Phase 3: the world-model output layer is fixed at 25 x 96 regardless of
// the genome-chosen hidden size H. fast/anchor vectors always have this
// length (BrainSlot invariant, also used by save/load).
constexpr std::int32_t kBrainWmHidden = 96;
constexpr std::size_t kBrainFastLen =
    static_cast<std::size_t>(kLabelChannels) * kBrainWmHidden;

// ---- kernels ----------------------------------------------------------------
inline std::int8_t brain_clamp_i8(std::int64_t v) {
    if (v > 127) return 127;
    if (v < -128) return -128;
    return static_cast<std::int8_t>(v);
}

// Raw int8 dot product, sequential order — the ONLY accumulation order
// used anywhere, so results are SIMD-width independent.
std::int32_t brain_dot_i8(const std::int8_t* a, const std::int8_t* b,
                          std::int32_t n);

// Dense layer forward. w is row-major [rows][cols] (unpadded cols OK —
// padding is a LAYOUT concern, see brain_padded_cols). bias may be null.
// x and y are int8[cols]/int8[rows]. act selects the LUT applied to the
// clamped pre-activation.
void brain_dense_i8(const std::int8_t* w, std::int32_t rows, std::int32_t cols,
                    const std::int8_t* x, const std::int16_t* bias,
                    std::int32_t shift, BrainAct act, std::int8_t* y);

// ---- layout helpers ----------------------------------------------------------
// Weight rows are padded to a multiple of 16 columns so future SIMD kernels
// can iterate fixed-width fragments; the pad bytes are always zero.
inline std::int32_t brain_padded_cols(std::int32_t cols) {
    return (cols + 15) & ~15;
}
// Bytes for one dense layer's weights (int8) — rows * padded_cols.
inline std::int64_t brain_dense_weight_bytes(std::int32_t rows,
                                             std::int32_t cols) {
    return static_cast<std::int64_t>(rows) * brain_padded_cols(cols);
}
// Bytes for biases (int16 per row).
inline std::int64_t brain_dense_bias_bytes(std::int32_t rows) {
    return static_cast<std::int64_t>(rows) * 2;
}

// ---- hash-driven init / mutation ----------------------------------------------
// Deterministic weight fill: w[i] in [-kInitSpread, +kInitSpread] derived
// from (seed, salt, index). Used for random BrainGenome init and tests.
std::int8_t brain_rand_weight(std::uint64_t seed, std::uint64_t salt,
                              std::uint64_t index, std::int32_t spread);
// One mutation draw: returns a signed step in [-step, +step] (0 allowed),
// derived from (seed, entity, tick, block) — replay-identical forever.
std::int32_t brain_mutation_step(std::uint64_t seed, std::uint64_t entity,
                                 std::uint64_t tick, std::uint64_t block,
                                 std::int32_t step);
// Apply sparse-block mutation to a weight row range in place.
// Each 64-weight block is mutated with probability mut_permille/1000;
// a mutated block's weights each get +-step (clamped to int8 range).
void brain_mutate_blocks(std::int8_t* w, std::int64_t n_weights,
                         std::uint64_t seed, std::uint64_t entity,
                         std::uint64_t tick, std::int32_t step,
                         std::int32_t mut_permille);

// ---- BrainArena ---------------------------------------------------------------
// Deterministic bump+free-list allocator over one slab, carved into
// 64-byte cells (blueprint DoD: row-major int8 weights, 16-wide padding,
// 64-byte alignment). Allocations are whole cells; sizes round UP to cells.
// Free lists are per-cell-count vectors used LIFO — allocation order is a
// pure function of the alloc/free call sequence, so replays are identical.
// Slab memory is NOT part of state_hash; only its deterministic CONTENTS
// (weights) are hashed in later phases.
class BrainArena {
public:
    static constexpr std::int32_t kCell = 64;      // bytes per cell

    BrainArena() = default;
    explicit BrainArena(std::uint64_t cells) { reserve(cells); }

    // Grow the slab (never shrinks). Idempotent; cells already handed out
    // keep their offsets.
    void reserve(std::uint64_t cells);

    // Allocate `bytes` (rounded up to whole cells). Returns byte offset into
    // the slab, or kInvalid on overflow. Deterministic: reuses the most
    // recently freed cell-count class, else bump-allocates.
    static constexpr std::uint64_t kInvalid = ~std::uint64_t{0};
    std::uint64_t alloc(std::uint64_t bytes);
    void free(std::uint64_t offset, std::uint64_t bytes);

    // Raw access. offset must have come from alloc() and not be freed.
    // Offsets are cell-grid relative; slab_base_off_ (alignment slack)
    // is added here so reserve() reallocs never invalidate them.
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
    // Phase 2 save/load: arena bookkeeping needed to rebuild identical
    // alloc/free behavior after a load (offsets themselves are re-derived).
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
    // 64-byte-aligned cell: operator new honors over-alignment (C++17), so
    // slab_.data() is ALWAYS 64B-aligned and slab_base_off_ stays 0 across
    // every realloc. (The old uint64_t slab could realign on growth, which
    // silently shifted every live brain's weights — a nondeterminism bug.)
    struct alignas(64) Cell { std::uint64_t v; };
    std::vector<Cell> slab_;
    std::uint64_t slab_base_off_ = 0;   // byte offset of cell 0 inside slab_
    std::uint64_t slab_cells_ = 0;
    std::uint64_t high_water_ = 0;      // bump pointer, in cells
    std::uint64_t live_bytes_ = 0;
    // free_[ncells] = stack of freed cell-offsets (in cells) of that size
    std::vector<std::vector<std::uint64_t>> free_;
};

// ================= Phase 1: per-agent brain state ==============================
// Fixed network layout (H=64 hidden). All weights int8 in ONE arena block
// per brain; biases int16 in a tail region of the same block. Only the
// trunk (embed + minGRU) and World Model are forwarded in shadow mode —
// Manager/Worker/Value weights exist for layout stability (Phase 2 adds
// their heads without re-laying-out).
//
//   embed   96 -> 64   (tanh)
//   minGRU  z = sigmoid(Wz.e), h~ = tanh(Wh.e);  h <- h + z*(h~ - h)
//           elementwise, h stored int16 Q8 — recurrent part is a single
//           multiply per unit (the "minimal" GRU of the blueprint).
//   WM      (h8 ++ a16) = 80 -> 96 (tanh) -> 25 (linear):
//           yhat[0..15] intero deltas, [16..23] outcome classes, [24] reward
//
// Replay: 16-transition ring per agent; stratified replacement keeps the
// highest (|charge| + sigma) samples — rare shocks are never overwritten
// by routine sips.

struct Transition {
    SensorVec s{};
    ActionVec a{};
    LabelVec  y{};
    std::array<std::int8_t, 96> wh{};   // WM layer-1 activation (sleep SGD)
    std::array<std::int8_t, 64> w1h{};  // worker hidden (dream w2w replay)
    std::uint16_t sigma = 0;        // surprise 0..1000
    std::uint16_t charge = 0;       // |emotional_charge| 0..1000
    std::uint64_t event_id = 0;
    bool valid = false;
};

// Pre-commit forward-pass snapshot: what the brain predicted this action
// would do, filled by brain_pre() and consumed by brain_post() once the
// causal event is committed.
struct BrainPending {
    bool valid = false;
    SensorVec s{};
    ActionVec a{};
    std::array<std::int8_t, kLabelChannels> yhat{};
    std::array<std::int8_t, 96> wm_hidden{};   // WM layer-1 activations
    std::array<std::int8_t, 64> w1hidden{};    // worker hidden (actor SGD)
    std::array<std::int16_t, 96> h{};          // post-GRU hidden state Q8
    std::array<std::int8_t, 96> embed{};       // embed output (manager/worker in)
    // open-ended invention: materials of the first two material-bearing
    // targets, resolved pre-commit so brain_post can key the tried-combo
    // set on exactly what the laws will see.
    MaterialId pm_a = kNoMaterial;
    MaterialId pm_b = kNoMaterial;
};

struct BrainSlot {
    std::uint64_t woff = 0;          // arena offset of weight block
    std::uint64_t wbytes = 0;        // bytes handed to arena (for free)
    std::int32_t hidden = 64;        // H — layer width, {32,48,64,96}
    // Baldwin hyper-genes (inherited via BrainGenome / entity genome loci)
    std::int32_t lr_permille = 400;      // brain_lr — lifetime SGD rate
    std::int32_t trace_decay = 900;     // brain_trace_decay — gamma*1024
    std::int32_t mut_scale = 500;       // brain_mut_scale — birth mutation ‰
    std::array<std::int16_t, 96> h{};   // GRU state, Q8 (first `hidden` used)
    ActionVec last_action{};                   // feeds sensor channels 80..95
    BrainPending pending{};
    std::array<Transition, 16> replay{};
    std::int32_t replay_next = 0;
    std::int32_t replay_count = 0;
    // Phase 3 metabolism: MACs spent by brain_decide() since the last
    // drift_needs() metabolize pass, plus the sub-unit energy remainder
    // (Euclid-exact like mind_brain_accum). Converted to `energy` drain
    // inside interact()'s causal event — never written outside it.
    std::int64_t mac_pending = 0;
    std::int64_t mac_frac = 0;
    // Phase 3 plasticity: FAST weights — a small int8 delta on the WM
    // output layer (25 x 96), updated online by brain_post with a
    // surprise-scaled rate, consolidated into slow weights during `rest`
    // (sleep). Never inherited (Baldwin). `anchor` is the slow-WM2
    // snapshot taken at birth — the L2-SP regularizer target that keeps
    // sleep consolidation from catastrophic forgetting.
    std::vector<std::int8_t> fast;      // kLabelChannels*kWmHidden deltas
    std::vector<std::int8_t> anchor;    // same size, birth snapshot of wm2w
    // Open-ended invention: ring of material-combo keys (verb + parent
    // material ids) this agent has already transformed. Lifetime-only —
    // never inherited (Baldwin), like fast weights. 0 = empty slot.
    std::array<std::uint64_t, 64> tried{};
    std::int32_t tried_next = 0;
};

// Phase 2: the inheritable brain. `slow` is the canonical-layout int8
// weight block (embed+GRU+manager+worker+WM+value). Fast weights are NOT
// stored — they are always zero at birth (Baldwin: only slow weights and
// hyper-genes cross the generational boundary).
struct BrainGenome {
    bool valid = false;
    std::int32_t hidden = 64;
    std::int32_t lr_permille = 400;
    std::int32_t trace_decay = 900;
    std::int32_t mut_scale = 500;
    std::vector<std::int8_t> slow;
};

// Reproductive isolation threshold: mean |Δw| over the slow block, permille
// of the 255 int8 range. Above this, reproduce() refuses the pairing.
constexpr std::int64_t kMaxBrainGeneticDistance = 180;   // ~18% mean drift

// Per-world brain bank. Shared via shared_ptr so speculative clones see
// the same store but MUST NOT write it (guarded by World::speculative).
// Deterministic: slots is a std::map (sorted EntityId), arena allocation
// order is a pure function of the call sequence.
struct BrainStore {
    BrainArena arena;
    std::map<EntityId, BrainSlot> slots;
    // shadow-mode observability stats (not part of state_hash)
    std::int64_t wm_err_sum = 0;      // sum |y - yhat| over label channels
    std::int64_t wm_n = 0;
    std::int64_t sigma_sum = 0;
    std::int64_t sigma_n = 0;
    std::int64_t surprise_events = 0; // sigma >= 500 count
};

// ---- Phase 1 API ----------------------------------------------------------------
// Layer geometry (public for tests/tools). brain_layout() is the canonical
// H=64 view; brain_layout_h(H) gives the per-hidden-size layout used by
// Phase-2 genome-variable brains ({32,48,64,96}).
struct BrainLayout {
    // offsets (bytes) inside the weight block; weights region first,
    // then the int16 bias region.
    std::uint32_t embed_w, embed_b;
    std::uint32_t zw, zb, hw, hb;              // minGRU gates
    std::uint32_t m1w, m1b, m2w, m2b;          // manager 64->32->8
    std::uint32_t w1w, w1b, w2w, w2b, aqw, aqb;// worker (64+8)->64->16 + attn
    std::uint32_t wm1w, wm1b, wm2w, wm2b;      // world model 80->96->25
    std::uint32_t valw, valb;                  // value 64->1
    std::uint64_t total_bytes = 0;
};
const BrainLayout& brain_layout();
const BrainLayout& brain_layout_h(std::int32_t hidden);

// Get-or-create the agent's brain slot (allocates + hash-inits weights,
// or installs `genome`'s slow weights + hyper-genes when provided and
// layout-compatible). Deterministic per (world_seed, entity): same id ->
// same initial brain.
BrainSlot& brain_slot(World& w, BrainStore& store, EntityId npc,
                      const BrainGenome* genome = nullptr);
void brain_free_slot(BrainStore& store, EntityId npc);
// Sweep dead/missing entities out of the store (called from decay_minds).
void brain_gc(World& w, BrainStore& store);

// Pre-commit: encode s_t, action a_t, forward trunk+WM -> pending.
// Post-commit: extract label, surprise, push replay, one integer SGD step
// on the WM output layer. Both no-op unless called under brain_shadow.
void brain_pre(World& w, EntityId npc, const InteractionContext& ctx);
void brain_post(World& w, EntityId npc, const CausalEvent& ev);

// Deterministic fingerprint of ALL brain state (weights + h + replay) —
// the Phase-1 replay check. FNV-1a over sorted slots.
std::uint64_t brain_state_hash(const BrainStore& store);

// ---- Phase 2: genome, decision, inheritance ------------------------------------
// Read the agent's current BrainGenome (slow weights + hyper-genes) —
// nullptr-equivalent `valid=false` when the agent has no slot yet.
BrainGenome brain_genome_of(const World& w, const BrainStore& store,
                            EntityId npc);
// Mean |Δw| over the slow block in permille of 255 (0 = identical).
std::int64_t brain_genetic_distance(const BrainGenome& a,
                                    const BrainGenome& b);
// Block crossover (64-weight blocks) + brain_mutate_blocks at the child's
// brain_mut_scale. Deterministic per (seed, parentA, parentB, tick).
BrainGenome brain_child_genome(const World& w, EntityId a, EntityId b,
                               const BrainGenome& ga, const BrainGenome& gb);

// The CL-1 decision: encode -> embed -> minGRU -> manager(drive) ->
// worker(verb logits + attention) -> mask -> top-C (verb,target) ->
// N-step WM imagination -> argmax. Returns the chosen (verb,target,dest).
struct BrainChoice {
    bool valid = false;
    std::string verb;
    EntityId target = kNoEntity;   // remembered entity id, kNoEntity = none
    Vec2 move_to{0, 0};            // only when verb == "move"
    bool has_move = false;
    std::int64_t score = 0;        // imagined value (diagnostics/tests)
};
BrainChoice brain_decide(World& w, EntityId npc);

// Install an inherited brain on a just-spawned entity (interact commit
// path). No-op on speculative clones or invalid genomes.
void brain_install_spawn(World& w, EntityId nid, const BrainGenome& g);

}  // namespace simlab
