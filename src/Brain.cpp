#include "simlab/Brain.hpp"
#include "simlab/GenePool.hpp"
#include "simlab/Rng.hpp"
#include "simlab/World.hpp"
#include "simlab/Cognition.hpp"
#include "simlab/Causal.hpp"
#include "simlab/Laws.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define SIMLAB_BRAIN_X86 1
#endif

namespace simlab {

namespace {

// ---- fixed-point helpers (Q20 throughout the LUT builders) -------------------
// u is represented Q20: real = raw / 2^20. All math in int64 -> identical
// on every platform.
constexpr std::int64_t kQ20 = 1ll << 20;

// tan nuclear-free rational Padé: tanh(u) ~= u*(27+u^2)/(27+9u^2),
// accurate to <0.3% for |u| <= 2 — plenty for a saturated unit.
std::int8_t tanh_q8(std::int64_t u_q20) {
    const std::int64_t u2 = (u_q20 * u_q20) / kQ20;              // Q20
    const std::int64_t num = u_q20 * (27 * kQ20 + u2);          // Q40
    const std::int64_t den = 27 * kQ20 + 9 * u2;                // Q20
    const std::int64_t t = den != 0 ? num / den : 0;            // Q20 in (-1,1)
    // map (-1,1) -> [-128,127]
    std::int64_t o = (t * 128) / kQ20;
    return brain_clamp_i8(o);
}

const std::array<std::int8_t, 256>& lut_tanh() {
    static const std::array<std::int8_t, 256> t = [] {
        std::array<std::int8_t, 256> a{};
        for (int i = 0; i < 256; ++i) {
            const std::int64_t v = i - 128;                    // int8 value
            a[static_cast<std::uint8_t>(static_cast<std::int8_t>(v))] =
                static_cast<std::int8_t>(tanh_q8(v * 16384));   // u = v/64 Q20
        }
        return a;
    }();
    return t;
}

const std::array<std::int8_t, 256>& lut_sigmoid() {
    // sigma(u) = (1 + tanh(u/2)) / 2  ->  as int8 in [-128,127] this is
    // exactly tanh(u/2)*128, i.e. the tanh LUT sampled at half the input.
    static const std::array<std::int8_t, 256> t = [] {
        std::array<std::int8_t, 256> a{};
        for (int i = 0; i < 256; ++i) {
            const std::int32_t v = i - 128;
            const std::int32_t half = v / 2;                    // u/2, int div
            a[static_cast<std::uint8_t>(static_cast<std::int8_t>(v))] =
                lut_tanh()[static_cast<std::uint8_t>(
                    static_cast<std::int8_t>(half))];
        }
        return a;
    }();
    return t;
}

const std::array<std::int8_t, 256>& lut_linear() {
    static const std::array<std::int8_t, 256> t = [] {
        std::array<std::int8_t, 256> a{};
        for (int v = -128; v <= 127; ++v)
            a[static_cast<std::uint8_t>(static_cast<std::int8_t>(v))] =
                static_cast<std::int8_t>(v);              // identity
        return a;
    }();
    return t;
}

}  // namespace

const std::int8_t* brain_act_lut(BrainAct act) {
    switch (act) {
        case BrainAct::kTanh:    return lut_tanh().data();
        case BrainAct::kSigmoid: return lut_sigmoid().data();
        case BrainAct::kLinear:
        default:                 return lut_linear().data();
    }
}

#if SIMLAB_BRAIN_X86
// AVX2 int8 dot: widen to i16, madd pairs -> i32 lanes, accumulate in
// vectors. int32 lane sums are reassociated vs the scalar loop, but mod-2^32
// addition is fully associative/commutative -> bit-identical result.
__attribute__((target("avx2")))
std::int32_t brain_dot_i8_avx2(const std::int8_t* a, const std::int8_t* b,
                               std::int32_t n) {
    __m256i acc0 = _mm256_setzero_si256();
    __m256i acc1 = _mm256_setzero_si256();
    std::int32_t i = 0;
    for (; i + 32 <= n; i += 32) {
        const __m256i a8 =
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i));
        const __m256i b8 =
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + i));
        acc0 = _mm256_add_epi32(acc0, _mm256_madd_epi16(
            _mm256_cvtepi8_epi16(_mm256_castsi256_si128(a8)),
            _mm256_cvtepi8_epi16(_mm256_castsi256_si128(b8))));
        acc1 = _mm256_add_epi32(acc1, _mm256_madd_epi16(
            _mm256_cvtepi8_epi16(_mm256_extracti128_si256(a8, 1)),
            _mm256_cvtepi8_epi16(_mm256_extracti128_si256(b8, 1))));
    }
    acc0 = _mm256_add_epi32(acc0, acc1);
    __m128i s = _mm_add_epi32(_mm256_castsi256_si128(acc0),
                              _mm256_extracti128_si256(acc0, 1));
    s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0x4E));
    s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0xB1));
    std::int32_t acc = _mm_cvtsi128_si32(s);
    for (; i < n; ++i)
        acc += static_cast<std::int32_t>(a[i]) * b[i];
    return acc;
}
#endif

std::int32_t brain_dot_i8(const std::int8_t* a, const std::int8_t* b,
                          std::int32_t n) {
#if SIMLAB_BRAIN_X86
    static const bool have_avx2 = __builtin_cpu_supports("avx2");
    if (have_avx2) return brain_dot_i8_avx2(a, b, n);
#endif
    std::int32_t acc = 0;
    for (std::int32_t i = 0; i < n; ++i)
        acc += static_cast<std::int32_t>(a[i]) * b[i];
    return acc;
}

void brain_dense_i8(const std::int8_t* w, std::int32_t rows, std::int32_t cols,
                    const std::int8_t* x, const std::int16_t* bias,
                    std::int32_t shift, BrainAct act, std::int8_t* y) {
    const std::int8_t* lut = brain_act_lut(act);
    for (std::int32_t o = 0; o < rows; ++o) {
        const std::int8_t* row = w + static_cast<std::int64_t>(o) * cols;
        std::int64_t acc = brain_dot_i8(row, x, cols) >> shift;
        if (bias) acc += bias[o];
        const std::int8_t pre = brain_clamp_i8(acc);
        y[o] = (act == BrainAct::kLinear)
                   ? pre
                   : lut[static_cast<std::uint8_t>(pre)];
    }
}

// ---- hash-driven init / mutation ---------------------------------------------

std::int8_t brain_rand_weight(std::uint64_t seed, std::uint64_t salt,
                              std::uint64_t index, std::int32_t spread) {
    // two hash rounds keep low bits of index well mixed
    std::uint64_t h = Rng::hash(seed ^ Rng::hash(salt + index * 0x9E3779B97F4A7C15ull));
    h ^= h >> 29; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 32;
    const std::int64_t span = 2 * static_cast<std::int64_t>(spread) + 1;
    return static_cast<std::int8_t>(
        static_cast<std::int64_t>(h % static_cast<std::uint64_t>(span)) - spread);
}

std::int32_t brain_mutation_step(std::uint64_t seed, std::uint64_t entity,
                                 std::uint64_t tick, std::uint64_t block,
                                 std::int32_t step) {
    std::uint64_t h = seed;
    h ^= Rng::hash(entity + block * 0x9E3779B97F4A7C15ull);
    h ^= Rng::hash(tick + block * 0xBF58476D1CE4E5B9ull);
    h ^= h >> 29; h *= 0x94D049BB133111EBull; h ^= h >> 32;
    const std::int64_t span = 2 * static_cast<std::int64_t>(step) + 1;
    return static_cast<std::int32_t>(
        static_cast<std::int64_t>(h % static_cast<std::uint64_t>(span)) - step);
}

void brain_mutate_blocks(std::int8_t* w, std::int64_t n_weights,
                         std::uint64_t seed, std::uint64_t entity,
                         std::uint64_t tick, std::int32_t step,
                         std::int32_t mut_permille) {
    const std::int64_t nblocks = (n_weights + 63) / 64;
    for (std::int64_t b = 0; b < nblocks; ++b) {
        // per-block fire check: hash -> permille
        std::uint64_t h = seed ^ Rng::hash(entity * 1315423911ull
                            + static_cast<std::uint64_t>(tick) * 2654435761ull
                            + static_cast<std::uint64_t>(b) * 0x9E3779B97F4A7C15ull);
        h ^= h >> 29; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 32;
        if (static_cast<std::int64_t>(h % 1000) >= mut_permille) continue;
        const std::int64_t lo = b * 64;
        const std::int64_t hi = std::min<std::int64_t>(n_weights, lo + 64);
        for (std::int64_t i = lo; i < hi; ++i) {
            const std::int32_t d = brain_mutation_step(
                seed, entity, tick, static_cast<std::uint64_t>(i), step);
            const std::int64_t v =
                static_cast<std::int64_t>(w[i]) + d;
            w[i] = brain_clamp_i8(v > 127 ? 127 : v);
        }
    }
}

// ---- BrainArena ----------------------------------------------------------------

void BrainArena::reserve(std::uint64_t cells) {
    if (cells <= slab_cells_) return;
    // Cell is alignas(64): data() is always 64B-aligned, so cell 0 sits at
    // byte offset 0 and every previously handed-out offset stays valid
    // across realloc. (+1 spare cell replaces the old alignment slack.)
    slab_.resize(cells + 1);
    slab_base_off_ = 0;
    slab_cells_ = cells;
}

std::uint64_t BrainArena::alloc(std::uint64_t bytes) {
    std::uint64_t cells = (bytes + kCell - 1) / kCell;
    if (cells == 0) cells = 1;
    if (cells > slab_cells_) return kInvalid;
    if (cells >= free_.size()) free_.resize(cells + 1);
    auto& fl = free_[cells];
    std::uint64_t cell;
    if (!fl.empty()) {
        cell = fl.back(); fl.pop_back();
    } else {
        if (high_water_ + cells > slab_cells_) return kInvalid;
        cell = high_water_;
        high_water_ += cells;
    }
    live_bytes_ += cells * kCell;
    // offsets are cell-grid relative (cell * kCell); slab_base_off_ is added
    // by w8()/free() so realloc-moving reserve() never invalidates them.
    return cell * kCell;
}

void BrainArena::free(std::uint64_t offset, std::uint64_t bytes) {
    if (offset == kInvalid || offset % kCell != 0) return;
    const std::uint64_t cell = offset / kCell;
    const std::uint64_t cells = (bytes + kCell - 1) / kCell;
    if (cells == 0 || cell + cells > slab_cells_) return;
    if (cells >= free_.size()) free_.resize(cells + 1);
    free_[cells].push_back(cell);
    live_bytes_ = live_bytes_ >= cells * kCell
                      ? live_bytes_ - cells * kCell : 0;
}

// ================= Phase 1: network layout, forward, shadow learning =============

namespace {

// Fixed-point shifts chosen so (dot >> shift) + bias lands in int8 range
// for the init spread (|w| <= 24) and typical activations.
constexpr std::int32_t kShEmbed = 11, kShGru = 11, kShWm1 = 11, kShWm2 = 12;
constexpr std::int32_t kWmHidden = kBrainWmHidden;
constexpr std::int32_t kHid = 64;

struct Off {
    std::uint32_t w = 0;   // weight offset (int8, rows x padded cols)
    std::uint32_t b = 0;   // bias offset   (int16, rows)
};

// Build the layout for hidden size H: every layer's weight block then its
// bias block, sequentially. All column counts are padded to multiples of
// 16 so stride == padded cols (worker input H+8 -> pad16, WM input H+16).
static BrainLayout layout_for(std::int32_t H) {
    BrainLayout l{};
    std::uint64_t o = 0;
    auto layer = [&](std::uint32_t rows, std::uint32_t cols,
                     std::uint32_t& w, std::uint32_t& b) {
        w = static_cast<std::uint32_t>(o);
        o += static_cast<std::uint64_t>(rows) * cols;
        b = static_cast<std::uint32_t>(o);
        o += static_cast<std::uint64_t>(rows) * 2;
    };
    const std::uint32_t wc = static_cast<std::uint32_t>(
        (H + 8 + 15) & ~15);    // worker input: h(H) ++ drive(8), padded
    const std::uint32_t mc = static_cast<std::uint32_t>(
        (H + kActionVerbs + 15) & ~15);   // WM input: h(H) ++ action(32), padded
    const std::uint32_t Hu = static_cast<std::uint32_t>(H);
    layer(Hu, kSensorChannels, l.embed_w, l.embed_b); // sensor -> hidden H
    layer(Hu, Hu,  l.zw,     l.zb);         // GRU update gate
    layer(Hu, Hu,  l.hw,     l.hb);         // GRU candidate
    layer(32, Hu,  l.m1w,    l.m1b);        // manager H -> 32
    layer(8,  32,  l.m2w,    l.m2b);        // manager 32 -> 8 drives
    layer(64, wc,  l.w1w,    l.w1b);        // worker (H+8) -> 64
    layer(kActionVerbs, 64, l.w2w, l.w2b);  // worker 64 -> 32 verbs
    layer(16, Hu,  l.aqw,    l.aqb);        // worker attention queries
    layer(96, mc,  l.wm1w,   l.wm1b);       // WM (H+32) -> 96
    layer(25, 96,  l.wm2w,   l.wm2b);       // WM 96 -> 25 outputs
    layer(1,  Hu,  l.valw,   l.valb);       // value H -> 1
    l.total_bytes = o;
    return l;
}

// hidden sizes are genome-quantized to {32,48,64,96}; anything else falls
// back to the nearest supported size (deterministic).
static std::int32_t snap_hidden(std::int32_t h) {
    if (h <= 32) return 32;
    if (h <= 48) return 48;
    if (h <= 64) return 64;
    if (h <= 96) return 96;
    return 96;
}

// dense forward with int16 bias: y[o] = act(clamp((dot>>shift)+bias[o]))
void dense16(const std::int8_t* w, std::int32_t rows, std::int32_t cols,
             const std::int8_t* x, const std::int16_t* bias, std::int32_t shift,
             BrainAct act, std::int8_t* y) {
    const std::int8_t* lut = brain_act_lut(act);
    for (std::int32_t o = 0; o < rows; ++o) {
        const std::int8_t* row = w + static_cast<std::int64_t>(o) * cols;
        std::int64_t acc = brain_dot_i8(row, x, cols) >> shift;
        if (bias) acc += bias[o];
        const std::int8_t pre = brain_clamp_i8(acc);
        y[o] = (act == BrainAct::kLinear) ? pre : lut[static_cast<std::uint8_t>(pre)];
    }
}

// integer "huber-lite" for the WM output delta: linear near zero, quarter
// slope far out — keeps one huge surprise from dominating a step.
std::int32_t huber_i32(std::int32_t e) {
    const std::int32_t a = e < 0 ? -e : e;
    if (a <= 12) return e;
    const std::int32_t s = e < 0 ? -1 : 1;
    return s * (12 + (a - 12) / 4);
}

}  // namespace

const BrainLayout& brain_layout() { return brain_layout_h(kHid); }

const BrainLayout& brain_layout_h(std::int32_t hidden) {
    static BrainLayout cache[7];   // index = H/16 - 2  ({32..96} -> 0..4)
    static bool init[7] = {};
    const std::int32_t H = snap_hidden(hidden);
    const int idx = H / 16 - 2;
    if (!init[idx]) { cache[idx] = layout_for(H); init[idx] = true; }
    return cache[idx];
}

// read a brain_* hyper-gene locus (integer allele) with a default
static std::int64_t brain_locus(const World& w, const Entity* e,
                                const char* name, std::int64_t dflt) {
    if (!e || e->genome.loci.empty()) return dflt;
    const PropertyDef* d = w.props.find_name(name);
    if (!d) return dflt;
    const std::int64_t* v = e->genome.get(d->id);
    return v ? *v : dflt;
}

BrainSlot& brain_slot(World& w, BrainStore& store, EntityId npc,
                      const BrainGenome* genome) {
    auto it = store.slots.find(npc);
    if (it != store.slots.end()) return it->second;
    // hyper-genes: explicit genome wins, else the entity's own loci
    const Entity* ge = w.find(npc);
    const BrainGenome* src = genome;
    BrainGenome tmp, poolg;
    if (!src || !src->valid) {
        // Phase 4 gene pool: 70% of fresh brains derive from hall-of-fame
        // donors (deterministic per world_seed+npc); the rest fall through
        // to the entity's own loci -> hash-init path.
        if (w.gene_pool && w.gene_pool->derive(w.world_seed, npc, poolg)) {
            src = &poolg;
        } else {
            tmp.hidden   = snap_hidden(static_cast<std::int32_t>(
                brain_locus(w, ge, "brain_hidden", kHid)));
            tmp.lr_permille    = static_cast<std::int32_t>(
                brain_locus(w, ge, "brain_lr", 400));
            tmp.trace_decay    = static_cast<std::int32_t>(
                brain_locus(w, ge, "brain_trace_decay", 900));
            tmp.mut_scale      = static_cast<std::int32_t>(
                brain_locus(w, ge, "brain_mut_scale", 500));
            src = &tmp;
        }
    }
    const std::int32_t H = snap_hidden(src->hidden);
    const BrainLayout& L = brain_layout_h(H);
    BrainArena& ar = store.arena;
    if (ar.capacity_cells() == 0) ar.reserve(8192);   // ~512KB initial slab
    const bool have_slow = src->slow.size() == L.total_bytes;
    const std::uint64_t bytes = L.total_bytes;
    std::uint64_t off = ar.alloc(bytes);
    while (off == BrainArena::kInvalid) {               // grow until it fits
        const std::uint64_t need = (bytes + BrainArena::kCell - 1) /
                                   BrainArena::kCell;
        ar.reserve(ar.capacity_cells() + need + 64);
        off = ar.alloc(bytes);
    }
    BrainSlot slot;
    slot.woff = off;
    slot.wbytes = bytes;
    slot.hidden = H;
    slot.lr_permille = src->lr_permille;
    slot.trace_decay = src->trace_decay;
    slot.mut_scale = src->mut_scale;
    // weights: inherited slow block verbatim (Baldwin — fast weights are
    // NOT inherited and start at the hash-init... no: spec says fast=0;
    // our layout has no separate fast region yet, so an inherited brain
    // keeps its slow block and a fresh brain hash-inits (genesis noise).
    std::int8_t* base = ar.w8(off);
    if (have_slow) {
        std::memcpy(base, src->slow.data(), L.total_bytes);
        // clamp int16 bias regions to the legal range (byte-crossover can
        // produce out-of-range halves)
        auto clamp_bias = [&](std::uint32_t boff, std::uint32_t rows) {
            std::int16_t* bp = reinterpret_cast<std::int16_t*>(base + boff);
            for (std::uint32_t i = 0; i < rows; ++i)
                bp[i] = static_cast<std::int16_t>(
                    std::max<std::int64_t>(-4096,
                        std::min<std::int64_t>(4095, bp[i])));
        };
        clamp_bias(L.embed_b, H); clamp_bias(L.zb, H); clamp_bias(L.hb, H);
        clamp_bias(L.m1b, 32);    clamp_bias(L.m2b, 8);
        clamp_bias(L.w1b, 64);    clamp_bias(L.w2b, kActionVerbs); clamp_bias(L.aqb, 16);
        clamp_bias(L.wm1b, 96);   clamp_bias(L.wm2b, 25); clamp_bias(L.valb, 1);
    } else {
        for (std::uint64_t i = 0; i < bytes; ++i)
            base[i] = brain_rand_weight(w.seed, static_cast<std::uint64_t>(npc),
                                        i, 24);
        auto zero_bias = [&](std::uint32_t boff, std::uint32_t rows) {
            std::memset(base + boff, 0, rows * 2);
        };
        zero_bias(L.embed_b, H); zero_bias(L.zb, H); zero_bias(L.hb, H);
        zero_bias(L.m1b, 32);    zero_bias(L.m2b, 8);
        zero_bias(L.w1b, 64);    zero_bias(L.w2b, kActionVerbs); zero_bias(L.aqb, 16);
        zero_bias(L.wm1b, 96);   zero_bias(L.wm2b, 25); zero_bias(L.valb, 1);
    }
    // Phase 3 plasticity: fast weights start at ZERO (Baldwin — never
    // inherited); anchor snapshots the birth slow-WM2 block for the
    // L2-SP pull during sleep consolidation.
    constexpr std::int32_t kWm2n = kLabelChannels * kWmHidden;
    slot.fast.assign(kWm2n, 0);
    slot.anchor.assign(reinterpret_cast<const std::int8_t*>(base + L.wm2w),
                       reinterpret_cast<const std::int8_t*>(base + L.wm2w) + kWm2n);
    // h and replay start at zero — fast weights / experience never inherit
    auto [ins, ok] = store.slots.emplace(npc, std::move(slot));
    (void)ok;
    return ins->second;
}

void brain_free_slot(BrainStore& store, EntityId npc) {
    auto it = store.slots.find(npc);
    if (it == store.slots.end()) return;
    store.arena.free(it->second.woff, it->second.wbytes);
    store.slots.erase(it);
}

void brain_gc(World& w, BrainStore& store) {
    if (w.speculative) return;   // clones share the store — never mutate it
    std::vector<EntityId> dead;
    for (const auto& [id, slot] : store.slots) {
        (void)slot;
        const Entity* e = w.find(id);
        if (!e || !e->alive) dead.push_back(id);
    }
    for (EntityId id : dead) brain_free_slot(store, id);
}

// WM output layer: effective weight = clamp_i8(slow + fast_delta). Merges
// into a stack buffer so the hot dot product stays a plain int8 matvec.
static void wm2_forward(const std::int8_t* wm2w, const std::int8_t* fast,
                        const std::int16_t* wm2b, const std::int8_t* x,
                        std::int8_t* y) {
    constexpr std::int32_t n = kLabelChannels * kWmHidden;
    std::array<std::int8_t, n> merged;
    for (std::int32_t i = 0; i < n; ++i)
        merged[i] = brain_clamp_i8(static_cast<std::int64_t>(wm2w[i]) + fast[i]);
    dense16(merged.data(), kLabelChannels, kWmHidden, x, wm2b,
            kShWm2, BrainAct::kLinear, y);
}

namespace { std::int32_t drive_bias(const NeuroState& nr, int d); }

// forward trunk + world model; fills pending. Caller guarantees slot exists.
static void brain_forward(World& w, BrainStore& store, EntityId npc,
                          BrainPending& p) {
    BrainSlot& s = brain_slot(w, store, npc);
    const std::int32_t H = s.hidden;
    const BrainLayout& L = brain_layout_h(H);
    const std::int32_t wm_in = ((H + kActionVerbs + 15) & ~15);   // padded cols
    std::int8_t* base = store.arena.w8(s.woff);
    const std::int8_t* embed_w = base + L.embed_w;
    const std::int16_t* embed_b =
        reinterpret_cast<const std::int16_t*>(base + L.embed_b);
    const std::int8_t* zw = base + L.zw;
    const std::int16_t* zb =
        reinterpret_cast<const std::int16_t*>(base + L.zb);
    const std::int8_t* hw = base + L.hw;
    const std::int16_t* hb =
        reinterpret_cast<const std::int16_t*>(base + L.hb);
    const std::int8_t* wm1w = base + L.wm1w;
    const std::int16_t* wm1b =
        reinterpret_cast<const std::int16_t*>(base + L.wm1b);
    const std::int8_t* wm2w = base + L.wm2w;
    const std::int16_t* wm2b =
        reinterpret_cast<const std::int16_t*>(base + L.wm2b);

    // embed: sensor(96) -> e(H), tanh
    std::array<std::int8_t, 96> e{};
    dense16(embed_w, H, kSensorChannels, p.s.data(), embed_b,
            kShEmbed, BrainAct::kTanh, e.data());
    for (int i = 0; i < H; ++i) p.embed[i] = e[i];

    // minGRU: gates read the EMBEDDED input only (no recurrence inside the
    // gates); the recurrence is one elementwise multiply — the cheap part.
    std::array<std::int8_t, 96> z{}, hh{};
    dense16(zw, H, H, e.data(), zb, kShGru, BrainAct::kSigmoid, z.data());
    dense16(hw, H, H, e.data(), hb, kShGru, BrainAct::kTanh, hh.data());
    for (int i = 0; i < H; ++i) {
        const std::int64_t zq = static_cast<std::int64_t>(z[i]) + 128; // ~sigma*256
        const std::int64_t target = static_cast<std::int64_t>(hh[i]) * 256;
        std::int64_t h = s.h[i] + ((zq * (target - s.h[i])) >> 8);
        h = std::max<std::int64_t>(-32512, std::min<std::int64_t>(32511, h));
        s.h[i] = static_cast<std::int16_t>(h);
        p.h[i] = s.h[i];
    }

    // worker forward for actor learning: manager -> drive -> w1 hidden.
    // brain_post uses p.w1hidden to SGD the executed verb's w2w row, so
    // policy improves within a lifetime, not only across generations.
    {
        const std::int32_t wc = (H + 8 + 15) & ~15;
        const AgentMind* fm = w.find_mind(npc);
        std::array<std::int8_t, 32> m1{};
        dense16(base + L.m1w, 32, H, e.data(),
                reinterpret_cast<const std::int16_t*>(base + L.m1b),
                kShGru, BrainAct::kTanh, m1.data());
        std::array<std::int8_t, 8> m2{};
        dense16(base + L.m2w, 8, 32, m1.data(),
                reinterpret_cast<const std::int16_t*>(base + L.m2b),
                kShGru, BrainAct::kLinear, m2.data());
        std::int32_t ds[8];
        for (int d = 0; d < 8; ++d)
            ds[d] = m2[d] * 8 + (fm ? drive_bias(fm->neuro, d) : 0);
        int drive = 0;
        for (int d = 1; d < 8; ++d) if (ds[d] > ds[drive]) drive = d;
        std::array<std::int8_t, 128> wwin{};
        for (int i = 0; i < H; ++i)
            wwin[i] = static_cast<std::int8_t>(s.h[i] >> 8);
        wwin[H + drive] = 100;
        dense16(base + L.w1w, 64, wc, wwin.data(),
                reinterpret_cast<const std::int16_t*>(base + L.w1b),
                kShWm1, BrainAct::kTanh, p.w1hidden.data());
    }

    // WM input: hidden state dequantized to int8 ++ action one-hot (pad16)
    std::array<std::int8_t, 128> win{};
    for (int i = 0; i < H; ++i)
        win[i] = static_cast<std::int8_t>(s.h[i] >> 8);
    for (int i = 0; i < kActionVerbs; ++i)
        win[H + i] = p.a[i];

    // WM layer 1: (H+16) -> 96, tanh (kept for the output-layer gradient)
    dense16(wm1w, kWmHidden, wm_in, win.data(), wm1b,
            kShWm1, BrainAct::kTanh, p.wm_hidden.data());
    // WM output: 96 -> 25, linear (slow wm2w + fast delta, clamped int8)
    wm2_forward(wm2w, s.fast.data(), wm2b, p.wm_hidden.data(), p.yhat.data());
}

void brain_pre(World& w, EntityId npc, const InteractionContext& ctx) {
    if (!w.brain_shadow || w.speculative) return;
    if (!w.find_mind(npc)) return;                    // only perceived agents
    if (!w.brains_) w.brains_ = std::make_shared<BrainStore>();
    BrainStore* store = w.brains_.get();
    BrainSlot& s = brain_slot(w, *store, npc);
    BrainPending& p = s.pending;
    p.s = sensor_encode(w, npc, &s.last_action);
    p.a = sensor_action(ctx);
    brain_forward(w, *store, npc, p);
    p.valid = true;
}

void brain_post(World& w, EntityId npc, const CausalEvent& ev) {
    if (!w.brain_shadow || w.speculative) return;
    BrainStore* store = w.brains_.get();
    if (!store) return;
    auto it = store->slots.find(npc);
    if (it == store->slots.end()) return;
    BrainSlot& s = it->second;
    BrainPending& p = s.pending;
    if (!p.valid) return;
    p.valid = false;

    const LabelVec y = sensor_label(w, ev, npc, p.s);
    const AgentMind* mind = w.find_mind(npc);

    // surprise: mean |y - yhat| over the 25 channels, scaled to permille
    std::int64_t l1 = 0;
    for (int i = 0; i < kLabelChannels; ++i)
        l1 += std::abs(static_cast<std::int32_t>(y[i]) - p.yhat[i]);
    const std::int64_t sigma = std::min<std::int64_t>(1000, l1 / 8);
    const std::int64_t charge =
        mind ? std::min<std::int64_t>(1000,
               std::abs(mind->neuro.panic) + std::abs(mind->neuro.rage) +
               std::abs(mind->neuro.grief)) / 3 : 0;

    store->wm_err_sum += l1;
    store->wm_n += kLabelChannels;
    store->sigma_sum += sigma;
    store->sigma_n += 1;
    if (sigma >= 500) store->surprise_events += 1;

    // replay: stratified ring — replace the least (charge+sigma) sample
    Transition t;
    t.s = p.s; t.a = p.a; t.y = y; t.wh = p.wm_hidden;
    t.w1h = p.w1hidden;             // dream replay re-drives the w2w row
    t.sigma = static_cast<std::uint16_t>(sigma);
    t.charge = static_cast<std::uint16_t>(charge);
    t.event_id = ev.id;
    t.valid = true;
    if (s.replay_count < 16) {
        s.replay[s.replay_next] = t;
        s.replay_next = (s.replay_next + 1) & 15;
        s.replay_count += 1;
    } else {
        // replace the minimum (charge+sigma) sample; ties -> lowest index
        std::int64_t best = -1; std::int32_t worst = 0;
        for (int i = 0; i < 16; ++i) {
            const std::int64_t sc =
                static_cast<std::int64_t>(s.replay[i].charge) + s.replay[i].sigma;
            if (best < 0 || sc < best) { best = sc; worst = i; }
        }
        s.replay[worst] = t;
    }
    s.last_action = p.a;

    // ---- one integer SGD step on the WM OUTPUT layer (W2 + bias) -------------
    // err = huber(y - yhat); grad ~ err * hidden; lr modulated by surprise.
    const BrainLayout& L = brain_layout_h(s.hidden);
    std::int8_t* base = store->arena.w8(s.woff);
    std::int8_t* wm2w = base + L.wm2w;
    std::int16_t* wm2b =
        reinterpret_cast<std::int16_t*>(base + L.wm2b);
    const std::int64_t eta = s.lr_permille + sigma / 2;  // genome lr + surprise
    for (int o = 0; o < kLabelChannels; ++o) {
        const std::int32_t err =
            huber_i32(static_cast<std::int32_t>(y[o]) - p.yhat[o]);
        if (err == 0) continue;
        for (int i = 0; i < kWmHidden; ++i) {
            const std::int64_t dw =
                (eta * err * p.wm_hidden[i]) >> 17;
            const std::int64_t d = std::max<std::int64_t>(-8,
                                               std::min<std::int64_t>(8, dw));
            if (d == 0) continue;
            std::int64_t v = static_cast<std::int64_t>(wm2w[o * kWmHidden + i]) + d;
            wm2w[o * kWmHidden + i] = brain_clamp_i8(v);
        }
        const std::int64_t db = (eta * err) >> 13;
        const std::int64_t db2 = std::max<std::int64_t>(-8,
                                            std::min<std::int64_t>(8, db));
        if (db2 != 0) {
            const std::int64_t bv =
                static_cast<std::int64_t>(wm2b[o]) + db2;
            wm2b[o] = static_cast<std::int16_t>(
                std::max<std::int64_t>(-4096,
                          std::min<std::int64_t>(4095, bv)));
        }
    }

    // ---- Phase 3: FAST-weight plasticity on the same WM2 error -------------
    // Faster, surprise-scaled, small deltas — the "working memory" of the
    // world model. Consolidated into slow weights on `rest` (sleep).
    if (s.fast.size() == static_cast<std::size_t>(kLabelChannels * kWmHidden)) {
        const std::int64_t eta_f = (eta * 3) / 2;   // ~1.5x slow lr
        for (int o = 0; o < kLabelChannels; ++o) {
            const std::int32_t err =
                huber_i32(static_cast<std::int32_t>(y[o]) - p.yhat[o]);
            if (err == 0) continue;
            for (int i = 0; i < kWmHidden; ++i) {
                const std::int64_t dw =
                    (eta_f * err * p.wm_hidden[i]) >> 18;
                const std::int64_t d = std::max<std::int64_t>(-6,
                                                   std::min<std::int64_t>(6, dw));
                if (d == 0) continue;
                const std::int64_t v =
                    static_cast<std::int64_t>(s.fast[o * kWmHidden + i]) + d;
                s.fast[o * kWmHidden + i] = static_cast<std::int8_t>(
                    std::max<std::int64_t>(-96,
                        std::min<std::int64_t>(96, v)));
            }
        }
    }

    // ---- Phase 4c: actor learning — w2w policy-row SGD ---------------------
    // REINFORCE-flavoured: advantage = homeostatic reward (label ch24,
    // already in channel units) + a novelty bonus for high-surprise
    // outcomes, applied ONLY to the executed verb's logit row through the
    // worker hidden stored by brain_forward. Lets coping habits, tastes
    // and pathological fixations actually be reinforced within a lifetime.
    {
        int verb = -1;
        for (int i = 0; i < kActionVerbs; ++i)
            if (p.a[i] == 100) { verb = i; break; }
        if (verb >= 0) {
            // stockpiling drive: a bounded bonus (<= ~15% of the reward
            // swing) for acting while inventory_security reads high —
            // food/tool hoarding emerges as a smooth gradient, never a rule.
            const std::int64_t adv =
                static_cast<std::int64_t>(y[24]) * 8 +
                (sigma >= 400 ? 96 : 0) - 64 +
                ((static_cast<std::int64_t>(p.s[kChanInvSecurity]) * 5) >> 2);
            if (adv != 0) {
                const std::int64_t eta_a =
                    std::max<std::int64_t>(8, s.lr_permille / 8);
                std::int8_t* w2w = base + L.w2w;
                for (int i = 0; i < 64; ++i) {
                    const std::int64_t dw =
                        (eta_a * adv * p.w1hidden[i]) >> 17;
                    const std::int64_t d = std::max<std::int64_t>(-4,
                                               std::min<std::int64_t>(4, dw));
                    if (d == 0) continue;
                    const std::int64_t v =
                        static_cast<std::int64_t>(w2w[verb * 64 + i]) + d;
                    w2w[verb * 64 + i] = brain_clamp_i8(v);
                }
            }
        }
    }

    // ---- Phase 3+deep: sleep consolidation & dream replay on `rest` --------
    // 1) dream rollout: the 4 most emotional replay samples (charge-led,
    //    sigma breaks ties) re-run WM1->WM2 on their stored hidden state —
    //    surprise/value delta consolidates into SLOW wm2 AND the w2w policy
    //    row of the dreamed verb (coping reinforced, charged failures
    //    penalized — trauma extinction without hardcoded labels).
    // 2) fast-weight decay (use-it-or-lose-it) then fast -> slow transfer.
    // 3) L2-SP pull toward the birth anchor (>>4) — anti-forgetting.
    if (p.a[4] && s.replay_count > 0 &&                       // verb "rest"
        s.fast.size() == static_cast<std::size_t>(kLabelChannels * kWmHidden)) {
        const std::int64_t eta_c = std::max<std::int64_t>(16, s.lr_permille / 2);
        // pick top-4 replay entries by emotional charge (sigma tiebreak)
        std::array<std::int32_t, 4> top{-1, -1, -1, -1};
        std::array<std::int64_t, 4> topsc{-1, -1, -1, -1};
        for (int i = 0; i < 16 && i < s.replay_count; ++i) {
            if (!s.replay[i].valid) continue;
            const std::int64_t sc =
                static_cast<std::int64_t>(s.replay[i].charge) * 4 +
                s.replay[i].sigma;
            for (int k = 0; k < 4; ++k) {
                if (sc > topsc[k]) {
                    for (int j = 3; j > k; --j) { topsc[j] = topsc[j-1]; top[j] = top[j-1]; }
                    topsc[k] = sc; top[k] = i; break;
                }
            }
        }
        for (int k = 0; k < 4; ++k) {
            const int ri = top[k];
            if (ri < 0) continue;
            const Transition& rt = s.replay[ri];
            LabelVec yh{};
            wm2_forward(wm2w, s.fast.data(), wm2b, rt.wh.data(), yh.data());
            for (int o = 0; o < kLabelChannels; ++o) {
                const std::int32_t err = huber_i32(
                    static_cast<std::int32_t>(rt.y[o]) - yh[o]);
                if (err == 0) continue;
                for (int i = 0; i < kWmHidden; ++i) {
                    const std::int64_t dw = (eta_c * err * rt.wh[i]) >> 18;
                    const std::int64_t d = std::max<std::int64_t>(-4,
                                                       std::min<std::int64_t>(4, dw));
                    if (d == 0) continue;
                    const std::int64_t v =
                        static_cast<std::int64_t>(wm2w[o * kWmHidden + i]) + d;
                    wm2w[o * kWmHidden + i] = brain_clamp_i8(v);
                }
            }
            // dream policy step: re-drive the executed verb's w2w row with
            // the stored worker hidden. Advantage = homeostatic outcome +
            // a charged-outcome term — actions that paid off under high
            // emotional charge consolidate (coping), charged failures are
            // suppressed (avoidance). Small rate: sleep is gentle.
            int dverb = -1;
            for (int i = 0; i < kActionVerbs; ++i)
                if (rt.a[i] == 100) { dverb = i; break; }
            if (dverb >= 0) {
                std::int64_t adv =
                    static_cast<std::int64_t>(rt.y[24]) * 8 - 64 +
                    (rt.sigma >= 400 ? 48 : 0);
                if (rt.charge >= 600)
                    adv += rt.y[24] > 0 ? 96 : -192;
                if (adv != 0) {
                    const std::int64_t eta_d =
                        std::max<std::int64_t>(8, s.lr_permille / 16);
                    std::int8_t* w2w_pol = base + L.w2w;   // worker policy
                    for (int i = 0; i < 64; ++i) {
                        const std::int64_t dw =
                            (eta_d * adv * rt.w1h[i]) >> 17;
                        const std::int64_t d = std::max<std::int64_t>(-3,
                                                   std::min<std::int64_t>(3, dw));
                        if (d == 0) continue;
                        const std::int64_t v =
                            static_cast<std::int64_t>(w2w_pol[dverb * 64 + i]) + d;
                        w2w_pol[dverb * 64 + i] = brain_clamp_i8(v);
                    }
                }
            }
        }
        // synaptic forgetting: fast weights shed ~1/16 per rest unless
        // re-reinforced by fresh surprise — prevents saturation drift when
        // the environment shifts (integer /16 truncates toward 0).
        for (std::int8_t& f : s.fast)
            f = static_cast<std::int8_t>(f - f / 16);
        // fast -> slow transfer + L2-SP anchor pull (one pass, in place)
        for (int i = 0; i < kLabelChannels * kWmHidden; ++i) {
            const std::int32_t f = s.fast[i];
            const std::int32_t tr = f >> 2;                 // keep remainder
            if (tr != 0) {
                const std::int64_t v =
                    static_cast<std::int64_t>(wm2w[i]) + tr;
                wm2w[i] = brain_clamp_i8(v);
                s.fast[i] = static_cast<std::int8_t>(f - (tr << 2));
            }
            const std::int64_t pull =
                (static_cast<std::int64_t>(s.anchor[i]) - wm2w[i]) >> 4;
            if (pull != 0) {
                const std::int64_t v =
                    static_cast<std::int64_t>(wm2w[i]) + pull;
                wm2w[i] = brain_clamp_i8(v);
            }
        }
    }

    // ---- active dream replay over the engram buffer + forgetting curve ----
    // The four most emotional memories are rehearsed while the agent rests
    // (clarity refresh; stability doubles at the data-set interval inside
    // rehearse_engram) and stamped as freshly recalled. Low-charge traces
    // unrehearsed beyond the brain's heritable trace_decay horizon decay
    // toward zero charge and shed stability — use it or lose it, freeing
    // capacity for new experiences. Runs on `rest` even with an empty
    // replay ring (engrams outlive transitions).
    if (p.a[4] && mind) {                                // verb "rest"
        const Entity* me = w.find(npc);
        if (me) {
            AgentMind& mm = w.mind(npc);
            const MindParams mp = mind_params(w, *me);
            const std::uint64_t now = w.clock_.tick();
            // top-4 engrams by |emotional_charge| (map order breaks ties)
            std::array<EntityId, 4> etop{kNoEntity, kNoEntity,
                                        kNoEntity, kNoEntity};
            std::array<std::int64_t, 4> esc{-1, -1, -1, -1};
            for (const auto& [kid, snap] : mm.memory) {
                const std::int64_t ch = snap.engram.emotional_charge;
                const std::int64_t ac = ch < 0 ? -ch : ch;
                for (int k = 0; k < 4; ++k) {
                    if (ac > esc[k]) {
                        for (int j = 3; j > k; --j) {
                            esc[j] = esc[j - 1]; etop[j] = etop[j - 1];
                        }
                        esc[k] = ac; etop[k] = kid; break;
                    }
                }
            }
            for (EntityId kid : etop) {
                if (kid == kNoEntity) continue;
                auto it = mm.memory.find(kid);
                if (it == mm.memory.end()) continue;
                rehearse_engram(it->second.engram, mp, now);
                it->second.engram.last_rehearsal = now;  // dream = recall
            }
            // forgetting curve: weak traces go quiet past the horizon
            const std::int64_t horizon =
                std::max<std::int64_t>(0, s.trace_decay);
            for (auto& [kid, snap] : mm.memory) {
                MemoryEngram& g = snap.engram;
                const std::int64_t ch = g.emotional_charge;
                const std::int64_t ac = ch < 0 ? -ch : ch;
                if (ac == 0 || ac >= 200) continue;      // salient survives
                if (now <= g.last_rehearsal ||
                    static_cast<std::int64_t>(now - g.last_rehearsal) <=
                        horizon)
                    continue;
                g.emotional_charge = ch - ch / 4;        // toward 0
                // sleep_consolidate doubles charged engrams on the same
                // rest, so the cut must beat x2 to actually decay: 3/8
                // leaves a net 0.75x per rest for stale weak traces.
                g.stability = std::max<std::int64_t>(
                    1, (g.stability * 3) / 8);            // evict sooner
            }
        }
    }
}

std::uint64_t brain_state_hash(const BrainStore& store) {
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&](std::uint64_t v) {
        h ^= v; h *= 1099511628211ull;
    };
    mix(store.slots.size());
    for (const auto& [id, s] : store.slots) {
        mix(static_cast<std::uint64_t>(id));
        mix(s.wbytes);   // woff is arena bookkeeping (heap-alignment
                         // dependent) — NOT brain state, never hashed
        mix(static_cast<std::uint32_t>(s.hidden));
        mix(static_cast<std::uint32_t>(s.lr_permille));
        mix(static_cast<std::uint32_t>(s.trace_decay));
        mix(static_cast<std::uint32_t>(s.mut_scale));
        for (std::int16_t v : s.h) mix(static_cast<std::uint16_t>(v));
        for (std::int8_t v : s.last_action) mix(static_cast<std::uint8_t>(v));
        // weights: hash the arena bytes of this brain's block
        const std::int8_t* wb = store.arena.w8(s.woff);
        for (std::uint64_t i = 0; i < s.wbytes; ++i)
            mix(static_cast<std::uint8_t>(wb[i]));
        mix(s.replay_count); mix(s.replay_next);
        mix(static_cast<std::uint64_t>(s.mac_pending));
        mix(static_cast<std::uint64_t>(s.mac_frac));
        for (std::int8_t v : s.fast) mix(static_cast<std::uint8_t>(v));
        for (std::int8_t v : s.anchor) mix(static_cast<std::uint8_t>(v));
        for (const Transition& t : s.replay) {
            mix(t.valid ? 1 : 0);
            if (!t.valid) continue;
            mix(t.event_id); mix(t.sigma); mix(t.charge);
            for (std::int8_t v : t.s) mix(static_cast<std::uint8_t>(v));
            for (std::int8_t v : t.a) mix(static_cast<std::uint8_t>(v));
            for (std::int8_t v : t.y) mix(static_cast<std::uint8_t>(v));
            for (std::int8_t v : t.wh) mix(static_cast<std::uint8_t>(v));
            for (std::int8_t v : t.w1h) mix(static_cast<std::uint8_t>(v));
        }
    }
    mix(store.wm_err_sum); mix(store.wm_n);
    mix(store.sigma_sum); mix(store.sigma_n);
    mix(store.surprise_events);
    return h;
}

// ================= Phase 2: genome, inheritance, CL-1 decision =================

BrainGenome brain_genome_of(const World& w, const BrainStore& store,
                            EntityId npc) {
    (void)w;
    BrainGenome g;
    const auto it = store.slots.find(npc);
    if (it == store.slots.end()) return g;
    const BrainSlot& s = it->second;
    const std::int8_t* wb = store.arena.w8(s.woff);
    g.valid = true;
    g.hidden = s.hidden;
    g.lr_permille = s.lr_permille;
    g.trace_decay = s.trace_decay;
    g.mut_scale = s.mut_scale;
    g.slow.assign(wb, wb + s.wbytes);
    return g;
}

std::int64_t brain_genetic_distance(const BrainGenome& a,
                                    const BrainGenome& b) {
    if (!a.valid || !b.valid || a.hidden != b.hidden ||
        a.slow.size() != b.slow.size() || a.slow.empty())
        return 1000000;
    std::int64_t sum = 0;
    for (std::size_t i = 0; i < a.slow.size(); ++i)
        sum += std::abs(static_cast<std::int32_t>(a.slow[i]) -
                        static_cast<std::int32_t>(b.slow[i]));
    // mean |dw| as permille of the 255 int8 range
    return sum * 1000 / (static_cast<std::int64_t>(a.slow.size()) * 255);
}

BrainGenome brain_child_genome(const World& w, EntityId a, EntityId b,
                               const BrainGenome& ga, const BrainGenome& gb) {
    BrainGenome g;
    g.valid = true;
    g.hidden = ga.hidden;
    const std::uint64_t tick = w.clock_.tick();
    auto pick = [&](std::int32_t x, std::int32_t y, std::uint64_t f) {
        return (Rng::hash(w.world_seed ^ (a * 0x9E3779B97F4A7C15ull) ^
                          (b * 0xBF58476D1CE4E5B9ull) ^
                          (tick * 0x94D049BB133111EBull) ^ (f << 32)) & 1)
                   ? y : x;
    };
    g.lr_permille = pick(ga.lr_permille, gb.lr_permille, 1);
    g.trace_decay = pick(ga.trace_decay, gb.trace_decay, 2);
    g.mut_scale = pick(ga.mut_scale, gb.mut_scale, 3);
    // block crossover: 64-byte blocks (one arena cell) from either parent
    const std::size_t n = std::min(ga.slow.size(), gb.slow.size());
    g.slow.assign(n, 0);
    for (std::size_t blk = 0; blk * 64 < n; ++blk) {
        const BrainGenome& src =
            (Rng::hash(w.world_seed ^ (a * 0x9E3779B97F4A7C15ull) ^
                       (b * 0xBF58476D1CE4E5B9ull) ^
                       (tick * 0x94D049BB133111EBull) ^
                       ((blk + 100) << 32)) & 1) ? gb : ga;
        const std::size_t cnt = std::min<std::size_t>(64, n - blk * 64);
        std::memcpy(g.slow.data() + blk * 64, src.slow.data() + blk * 64, cnt);
    }
    // mutation at the child's own mut_scale (permille), step 3
    brain_mutate_blocks(g.slow.data(), static_cast<std::int64_t>(n),
                        w.world_seed, a, tick, 3, g.mut_scale);
    // clamp int16 bias halves back into range (byte-level crossover can
    // produce out-of-range pairs)
    const BrainLayout& L = brain_layout_h(g.hidden);
    auto clamp_bias = [&](std::uint32_t boff, std::uint32_t rows) {
        std::int16_t* bp = reinterpret_cast<std::int16_t*>(g.slow.data() + boff);
        for (std::uint32_t i = 0; i < rows; ++i)
            bp[i] = static_cast<std::int16_t>(
                std::max<std::int64_t>(-4096, std::min<std::int64_t>(4095, bp[i])));
    };
    const std::int32_t H = g.hidden;
    clamp_bias(L.embed_b, H); clamp_bias(L.zb, H); clamp_bias(L.hb, H);
    clamp_bias(L.m1b, 32);    clamp_bias(L.m2b, 8);
    clamp_bias(L.w1b, 64);    clamp_bias(L.w2b, kActionVerbs); clamp_bias(L.aqb, 16);
    clamp_bias(L.wm1b, 96);   clamp_bias(L.wm2b, 25); clamp_bias(L.valb, 1);
    return g;
}

// install an inherited brain on a freshly spawned entity (interact commit)
void brain_install_spawn(World& w, EntityId nid, const BrainGenome& g) {
    if (w.speculative || !g.valid) return;
    if (!w.brains_) w.brains_ = std::make_shared<BrainStore>();
    brain_slot(w, *w.brains_, nid, &g);
}

namespace {

// 8 drive logits get a deterministic tilt from the emotion axes — the
// manager's output is modulated, never replaced, by personality.
std::int32_t drive_bias(const NeuroState& nr, int d) {
    static const std::int32_t kSign[8] = {3, -2, 1, -1, 2, -3, 0, 1};
    const std::int64_t base =
        nr.rage + nr.grief / 2 - nr.panic / 2;
    return static_cast<std::int32_t>((base * kSign[d]) >> 6);
}

}  // namespace

BrainChoice brain_decide(World& w, EntityId npc) {
    BrainChoice out;
    if (w.speculative) return out;
    const Entity* me = w.find(npc);
    const AgentMind* mind = w.find_mind(npc);
    if (!me || !me->alive || !mind) return out;
    if (!w.brains_) w.brains_ = std::make_shared<BrainStore>();
    BrainStore& store = *w.brains_;
    BrainSlot& s = brain_slot(w, store, npc);
    const std::int32_t H = s.hidden;
    const BrainLayout& L = brain_layout_h(H);
    const std::int32_t wc = (H + 8 + 15) & ~15;
    const std::int32_t mc = (H + kActionVerbs + 15) & ~15;
    const std::int8_t* base = store.arena.w8(s.woff);
    auto W = [&](std::uint32_t o) { return base + o; };
    auto B = [&](std::uint32_t o) {
        return reinterpret_cast<const std::int16_t*>(base + o);
    };

    // ---- 1-2. encode -> embed -> minGRU (on a COPY of h — decide must not
    // commit recurrent state; brain_pre does that at interact time) --------
    const SensorVec sv = sensor_encode(w, npc, &s.last_action);
    std::array<std::int8_t, 96> e{}, z{}, hh{};
    dense16(W(L.embed_w), H, kSensorChannels, sv.data(), B(L.embed_b),
            kShEmbed, BrainAct::kTanh, e.data());
    dense16(W(L.zw), H, H, e.data(), B(L.zb), kShGru, BrainAct::kSigmoid,
            z.data());
    dense16(W(L.hw), H, H, e.data(), B(L.hb), kShGru, BrainAct::kTanh,
            hh.data());
    std::array<std::int8_t, 96> h8{};
    for (int i = 0; i < H; ++i) {
        const std::int64_t zq = static_cast<std::int64_t>(z[i]) + 128;
        const std::int64_t target = static_cast<std::int64_t>(hh[i]) * 256;
        std::int64_t h = s.h[i] + ((zq * (target - s.h[i])) >> 8);
        h = std::max<std::int64_t>(-32512, std::min<std::int64_t>(32511, h));
        h8[i] = static_cast<std::int8_t>(h >> 8);
    }

    // ---- 3. manager: e -> 32 -> 8 drive logits + personality bias -----------
    std::array<std::int8_t, 32> m1{};
    dense16(W(L.m1w), 32, H, e.data(), B(L.m1b), kShGru, BrainAct::kTanh,
            m1.data());
    std::array<std::int8_t, 8> m2{};
    dense16(W(L.m2w), 8, 32, m1.data(), B(L.m2b), kShGru, BrainAct::kLinear,
            m2.data());
    std::int32_t drive_score[8];
    int drive = 0;
    for (int d = 0; d < 8; ++d) {
        drive_score[d] = m2[d] * 8 + drive_bias(mind->neuro, d);
        if (drive_score[d] > drive_score[drive]) drive = d;
    }

    // ---- worker: (h8 ++ drive) -> 64 -> 16 verb logits + 16 attn query -----
    std::array<std::int8_t, 128> win{};
    for (int i = 0; i < H; ++i) win[i] = h8[i];
    win[H + drive] = 100;
    std::array<std::int8_t, 64> w1{};
    dense16(W(L.w1w), 64, wc, win.data(), B(L.w1b), kShWm1, BrainAct::kTanh,
            w1.data());
    std::array<std::int8_t, kActionVerbs> vlogit{};
    dense16(W(L.w2w), kActionVerbs, 64, w1.data(), B(L.w2b), kShWm2,
            BrainAct::kLinear, vlogit.data());
    std::array<std::int8_t, 16> query{};
    dense16(W(L.aqw), 16, H, h8.data(), B(L.aqb), kShGru, BrainAct::kLinear,
            query.data());

    // mask: only verbs the mind has learned (known_actions) plus the
    // innate locomotion/rest pair — an agent cannot plan what it never saw.
    // One pass over the (small) known set: no per-verb string temporaries.
    bool allowed[kActionVerbs] = {};
    for (const std::string& kv : mind->known_actions)
        for (int i = 0; i < kActionVerbs; ++i)
            if (kv == sensor_verb_table()[i]) { allowed[i] = true; break; }
    for (int i = 0; i < kActionVerbs; ++i) {
        const char* v = sensor_verb_table()[i];
        if (std::strcmp(v, "move") == 0 || std::strcmp(v, "rest") == 0)
            allowed[i] = true;
    }
    int nallowed = 0;
    for (int i = 0; i < kActionVerbs; ++i) if (allowed[i]) ++nallowed;
    if (nallowed == 0) {
        for (int i = 0; i < kActionVerbs; ++i)
            if (std::strcmp(sensor_verb_table()[i], "move") == 0 ||
                std::strcmp(sensor_verb_table()[i], "rest") == 0)
                allowed[i] = true;
        for (int i = 0; i < kActionVerbs; ++i) if (allowed[i]) ++nallowed;
    }
    if (nallowed == 0) return out;

    // ---- attention targets: the 6 nearest remembered entities --------------
    // Bounded insertion into a stack array — no heap alloc per decide.
    // Iterates mind->memory (sorted map -> deterministic order); equal
    // distances keep the earlier (map-order) entry.
    struct NearEnt { std::int64_t d2; EntityId id; Vec2 pos; };
    NearEnt near[kEntitySlots];
    int nnear = 0;
    for (const auto& [kid, snap] : mind->memory) {
        if (kid == npc) continue;
        const std::int64_t dx = snap.pos.x - me->pos.x;
        const std::int64_t dy = snap.pos.y - me->pos.y;
        const std::int64_t d2 = dx * dx + dy * dy;
        if (nnear == kEntitySlots && d2 >= near[kEntitySlots - 1].d2) continue;
        int p = nnear < kEntitySlots ? nnear++ : kEntitySlots - 1;
        while (p > 0 && near[p - 1].d2 > d2) { near[p] = near[p - 1]; --p; }
        near[p] = {d2, kid, snap.pos};
    }
    // attention score per remembered slot: query[0..6] . sensor-feat[0..6]
    std::int32_t attn[kEntitySlots] = {};
    for (int t = 0; t < nnear; ++t) {
        const std::int8_t* f = &sv[24 + t * kEntityFeats];
        std::int32_t sc = 0;
        for (int k = 0; k < kEntityFeats; ++k) sc += query[k] * f[k];
        attn[t] = sc;
    }

    // ---- candidate generation: top-C (verb, target) pairs -------------------
    constexpr int kC = 5;
    struct Cand { std::int32_t score; int verb; int tgt; };
    std::array<Cand, kC> cands{};
    int ncand = 0;
    auto consider = [&](std::int32_t score, int verb, int tgt) {
        if (ncand < kC) {
            cands[ncand++] = {score, verb, tgt};
            if (ncand == kC)
                std::sort(cands.begin(), cands.end(),
                          [](const Cand& a, const Cand& b) {
                              return a.score > b.score; });
            return;
        }
        if (score > cands[kC - 1].score) {
            cands[kC - 1] = {score, verb, tgt};
            std::sort(cands.begin(), cands.end(),
                      [](const Cand& a, const Cand& b) {
                          return a.score > b.score; });
        }
    };
    for (int v = 0; v < kActionVerbs; ++v) {
        if (!allowed[v]) continue;
        consider(vlogit[v], v, -1);                    // no-entity variant
        for (int t = 0; t < nnear; ++t)
            consider(vlogit[v] + attn[t] / 8, v, t);
    }
    if (ncand == 0) return out;                        // nothing plannable

    // ---- 4. imagination: N-step WM rollout per candidate ---------------------
    const std::int64_t N = std::clamp<std::int64_t>(
        mind_params(w, *me).plan_horizon, 1, 6);
    const std::int64_t gamma =
        std::clamp<std::int64_t>(s.trace_decay, 0, 1024);   // Q10 discount
    // ---- affect shaping (Phase 4c): NeuroState bends imagined futures ----
    // grief/anhedonia blunts the homeostatic pull (depression -> anorexia,
    // withdrawal emerge naturally); panic taxes imagined failure and
    // trauma-tagged targets; rage rewards approaching hated ones. All
    // integer arithmetic over NeuroState — no disease labels anywhere.
    const NeuroState& ns = mind->neuro;
    const std::int64_t homeo_q =
        1024 - std::min<std::int64_t>(768, ns.grief * 3 / 4);
    const std::int64_t panic_q = ns.panic;
    const std::int64_t rage_q = ns.rage;
    std::array<std::int8_t, kLabelChannels> yhat{};
    std::array<std::int8_t, kWmHidden> wh{};
    std::int64_t best_total = INT64_MIN;
    int best_c = 0;
    for (int c = 0; c < ncand; ++c) {
        const Cand& cd = cands[c];
        std::int64_t total = 0;
        for (std::int64_t step = 0; step < N; ++step) {
            // reflex continuation: step 0 = candidate verb, later steps a
            // deterministic rotation through the allowed verbs
            int vstep = cd.verb;
            if (step > 0) {
                int k = 0;
                for (int i = 0; i < kActionVerbs; ++i) {
                    if (!allowed[i]) continue;
                    if (k == static_cast<int>((cd.verb + step) % nallowed)) {
                        vstep = i; break;
                    }
                    ++k;
                }
            }
            std::array<std::int8_t, 128> aw{};
            for (int i = 0; i < H; ++i) aw[i] = h8[i];
            aw[H + vstep] = 100;
            dense16(W(L.wm1w), kWmHidden, mc, aw.data(), B(L.wm1b),
                    kShWm1, BrainAct::kTanh, wh.data());
            wm2_forward(W(L.wm2w), s.fast.data(), B(L.wm2b),
                        wh.data(), yhat.data());
            std::int32_t r =
                yhat[24] + (yhat[16] > 0 ? 24 : yhat[16] < 0 ? -24 : 0);
            // grief dampens reward pull; panic taxes predicted failure
            r = static_cast<std::int32_t>(
                (static_cast<std::int64_t>(r) * homeo_q) >> 10);
            r -= static_cast<std::int32_t>(
                (panic_q * (127 - yhat[16])) >> 11);
            total = (total * gamma + r * 1024) >> 10;
        }
        // terminal value of the imagined state
        std::array<std::int8_t, 1> vv{};
        dense16(W(L.valw), 1, H, h8.data(), B(L.valb), kShGru,
                BrainAct::kLinear, vv.data());
        total += static_cast<std::int64_t>(vv[0]) * 256;
        // rage approaches hated memories; panic avoids trauma-tagged ones
        if (cd.tgt >= 0 && cd.tgt < nnear) {
            const auto mit = mind->memory.find(near[cd.tgt].id);
            if (mit != mind->memory.end() &&
                mit->second.engram.emotional_charge < 0) {
                const std::int64_t chg = -mit->second.engram.emotional_charge;
                total += rage_q * chg / 3072 - panic_q * chg / 4096;
            }
        }
        // small deterministic exploration jitter (genome-scaled)
        const std::uint64_t jit =
            Rng::hash(w.world_seed ^ (npc * 0x9E3779B97F4A7C15ull) ^
                      (w.clock_.tick() * 0xBF58476D1CE4E5B9ull) ^
                      (static_cast<std::uint64_t>(c) << 40));
        total += static_cast<std::int64_t>(jit %
            static_cast<std::uint64_t>(1 + s.mut_scale / 100)) -
            (1 + s.mut_scale / 100) / 2;
        if (total > best_total) { best_total = total; best_c = c; }
    }

    // ---- 5. map winner to an action ------------------------------------------
    const Cand& chosen = cands[best_c];
    // Phase 3 metabolism: bill the MACs this decision actually spent.
    // embed 96*H + GRU 2*H*H + manager H*32+32*8 + worker wc*64+64*16+
    // H*16(attn) + WM per imagined step mc*96+96*25 (x C x N) + value H.
    {
        const std::int64_t macs =
            96LL * H + 2LL * H * H + H * 32 + 32 * 8 +
            static_cast<std::int64_t>(wc) * 64 + 64 * 16 + H * 16 +
            static_cast<std::int64_t>(ncand > 0 ? ncand : 1) * N *
                (static_cast<std::int64_t>(mc) * 96 + 96 * 25) + H;
        s.mac_pending += macs;
    }
    const char* verb = sensor_verb_table()[chosen.verb];
    out.verb = verb;
    out.score = best_total;
    out.target = (chosen.tgt >= 0 && chosen.tgt < nnear)
                     ? near[chosen.tgt].id : kNoEntity;
    if (std::strcmp(verb, "move") == 0) {
        out.has_move = true;
        if (out.target != kNoEntity)
            out.move_to = near[chosen.tgt].pos;   // walk to the mark
        else {
            // unmarked wander: deterministic offset within sight range
            const std::uint64_t j =
                Rng::hash(w.world_seed ^ (npc * 0x2545F4914F6CDD1Dull) ^
                          (w.clock_.tick() * 0xFF51AFD7ED558CCDull));
            out.move_to = {me->pos.x + static_cast<std::int32_t>(
                               static_cast<std::int64_t>(j % 161) - 80),
                           me->pos.y + static_cast<std::int32_t>(
                               ((j >> 16) % 161) - 80)};
        }
    }
    out.valid = true;
    return out;
}

}  // namespace simlab
