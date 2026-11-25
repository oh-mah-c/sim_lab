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
    std::int8_t* w8(std::uint64_t offset) {
        return reinterpret_cast<std::int8_t*>(slab_.data()) + offset;
    }
    const std::int8_t* w8(std::uint64_t offset) const {
        return reinterpret_cast<const std::int8_t*>(slab_.data()) + offset;
    }
    std::uint64_t used_cells() const { return high_water_; }
    std::uint64_t capacity_cells() const { return slab_cells_; }
    std::uint64_t live_bytes() const { return live_bytes_; }

private:
    // slab_ holds slab_cells_ * kCell bytes, 64-byte aligned via vector of
    // uint64_t (16-byte aligned on x86-64; we over-align by allocating
    // cells*8 + 64 and offsetting — see reserve())
    std::vector<std::uint64_t> slab_;
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
    std::array<std::int16_t, 64> h{};          // post-GRU hidden state Q8
};

struct BrainSlot {
    std::uint64_t woff = 0;          // arena offset of weight block
    std::uint64_t wbytes = 0;        // bytes handed to arena (for free)
    std::array<std::int16_t, 64> h{};          // GRU state, Q8 fixed-point
    ActionVec last_action{};                   // feeds sensor channels 80..95
    BrainPending pending{};
    std::array<Transition, 16> replay{};
    std::int32_t replay_next = 0;
    std::int32_t replay_count = 0;
};

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
// Layer geometry (public for tests/tools).
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

// Get-or-create the agent's brain slot (allocates + hash-inits weights).
// Deterministic per (world_seed, entity): same id -> same initial brain.
BrainSlot& brain_slot(World& w, BrainStore& store, EntityId npc);
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

}  // namespace simlab
