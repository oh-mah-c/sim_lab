#include "simlab/Brain.hpp"
#include "simlab/Rng.hpp"
#include "simlab/World.hpp"
#include "simlab/Cognition.hpp"
#include "simlab/Causal.hpp"
#include "simlab/Laws.hpp"
#include <algorithm>
#include <array>
#include <cstring>

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

std::int32_t brain_dot_i8(const std::int8_t* a, const std::int8_t* b,
                          std::int32_t n) {
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
    const std::uint64_t need_bytes =
        cells * kCell + kCell;   // +1 cell slack for 64B alignment
    const std::uint64_t need_u64 = (need_bytes + 7) / 8;
    slab_.resize(need_u64, 0);
    // find first 64-byte-aligned offset inside the allocation
    const std::uintptr_t base =
        reinterpret_cast<std::uintptr_t>(slab_.data());
    const std::uint64_t mis = (64 - (base & 63)) & 63;
    slab_base_off_ = mis;
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
    return slab_base_off_ + cell * kCell;
}

void BrainArena::free(std::uint64_t offset, std::uint64_t bytes) {
    if (offset == kInvalid || offset < slab_base_off_) return;
    const std::uint64_t rel = offset - slab_base_off_;
    if (rel % kCell != 0) return;                    // not cell-aligned
    const std::uint64_t cell = rel / kCell;
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
constexpr std::int32_t kWmHidden = 96;
constexpr std::int32_t kWmIn = 80;      // h(64) ++ action(16)
constexpr std::int32_t kHid = 64;

struct Off {
    std::uint32_t w = 0;   // weight offset (int8, rows x padded cols)
    std::uint32_t b = 0;   // bias offset   (int16, rows)
};

// Build the layout once: every layer's weight block then its bias block,
// sequentially. All column counts are multiples of 16 so stride == cols.
const BrainLayout& layout_impl() {
    static const BrainLayout L = [] {
        BrainLayout l{};
        std::uint64_t o = 0;
        auto layer = [&](std::uint32_t rows, std::uint32_t cols,
                         std::uint32_t& w, std::uint32_t& b) {
            w = static_cast<std::uint32_t>(o);
            o += static_cast<std::uint64_t>(rows) * cols;   // cols % 16 == 0
            b = static_cast<std::uint32_t>(o);
            o += static_cast<std::uint64_t>(rows) * 2;
        };
        layer(64, 96,  l.embed_w, l.embed_b);   // sensor 96 -> hidden 64
        layer(64, 64,  l.zw,     l.zb);         // GRU update gate
        layer(64, 64,  l.hw,     l.hb);         // GRU candidate
        layer(32, 64,  l.m1w,    l.m1b);        // manager 64 -> 32
        layer(8,  32,  l.m2w,    l.m2b);        // manager 32 -> 8 drives
        layer(64, 80,  l.w1w,    l.w1b);        // worker (64+8) -> 64
        layer(16, 64,  l.w2w,    l.w2b);        // worker 64 -> 16 verbs
        layer(16, 64,  l.aqw,    l.aqb);        // worker attention queries
        layer(96, 80,  l.wm1w,   l.wm1b);       // WM 80 -> 96
        layer(25, 96,  l.wm2w,   l.wm2b);       // WM 96 -> 25 outputs
        layer(1,  64,  l.valw,   l.valb);       // value 64 -> 1
        l.total_bytes = o;
        return l;
    }();
    return L;
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

const BrainLayout& brain_layout() { return layout_impl(); }

BrainSlot& brain_slot(World& w, BrainStore& store, EntityId npc) {
    auto it = store.slots.find(npc);
    if (it != store.slots.end()) return it->second;
    const BrainLayout& L = brain_layout();
    BrainArena& ar = store.arena;
    if (ar.capacity_cells() == 0) ar.reserve(8192);   // ~512KB initial slab
    const std::uint64_t off = ar.alloc(L.total_bytes);
    BrainSlot slot;
    slot.woff = off;
    slot.wbytes = L.total_bytes;
    // deterministic init: every weight from (world_seed, entity, index)
    std::int8_t* base = ar.w8(off);
    for (std::uint64_t i = 0; i < L.total_bytes; ++i) {
        // bias region is the tail — detect by offset parity is fragile;
        // instead fill EVERYTHING with small random int8 then zero biases
        // via the layout below.
        base[i] = brain_rand_weight(w.seed, static_cast<std::uint64_t>(npc),
                                    i, 24);
    }
    // zero all bias regions (they live interleaved; walk the layout)
    const BrainLayout& l = L;
    auto zero_bias = [&](std::uint32_t boff, std::uint32_t rows) {
        std::memset(base + boff, 0, rows * 2);
    };
    zero_bias(l.embed_b, 64); zero_bias(l.zb, 64); zero_bias(l.hb, 64);
    zero_bias(l.m1b, 32);    zero_bias(l.m2b, 8);
    zero_bias(l.w1b, 64);    zero_bias(l.w2b, 16); zero_bias(l.aqb, 16);
    zero_bias(l.wm1b, 96);   zero_bias(l.wm2b, 25); zero_bias(l.valb, 1);
    // int8-fill only touched weight bytes? brain_rand_weight filled biases
    // too then memset zeroed them — weight bytes keep their draws. But the
    // rand stream index `i` ran over total_bytes INCLUDING bias bytes, so
    // weight draws depend on layout — deterministic regardless. Fine.
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

// forward trunk + world model; fills pending. Caller guarantees slot exists.
static void brain_forward(World& w, BrainStore& store, EntityId npc,
                          BrainPending& p) {
    BrainSlot& s = brain_slot(w, store, npc);
    const BrainLayout& L = brain_layout();
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

    // embed: sensor(96) -> e(64), tanh
    std::array<std::int8_t, kHid> e{};
    dense16(embed_w, kHid, kSensorChannels, p.s.data(), embed_b,
            kShEmbed, BrainAct::kTanh, e.data());

    // minGRU: gates read the EMBEDDED input only (no recurrence inside the
    // gates); the recurrence is one elementwise multiply — the cheap part.
    std::array<std::int8_t, kHid> z{}, hh{};
    dense16(zw, kHid, kHid, e.data(), zb, kShGru, BrainAct::kSigmoid, z.data());
    dense16(hw, kHid, kHid, e.data(), hb, kShGru, BrainAct::kTanh, hh.data());
    for (int i = 0; i < kHid; ++i) {
        const std::int64_t zq = static_cast<std::int64_t>(z[i]) + 128; // ~sigma*256
        const std::int64_t target = static_cast<std::int64_t>(hh[i]) * 256;
        std::int64_t h = s.h[i] + ((zq * (target - s.h[i])) >> 8);
        h = std::max<std::int64_t>(-32512, std::min<std::int64_t>(32511, h));
        s.h[i] = static_cast<std::int16_t>(h);
        p.h[i] = s.h[i];
    }

    // WM input: hidden state dequantized to int8 ++ action one-hot (80)
    std::array<std::int8_t, kWmIn> win{};
    for (int i = 0; i < kHid; ++i)
        win[i] = static_cast<std::int8_t>(s.h[i] >> 8);
    for (int i = 0; i < kActionVerbs; ++i)
        win[kHid + i] = p.a[i];

    // WM layer 1: 80 -> 96, tanh (kept for the output-layer gradient)
    dense16(wm1w, kWmHidden, kWmIn, win.data(), wm1b,
            kShWm1, BrainAct::kTanh, p.wm_hidden.data());
    // WM output: 96 -> 25, linear
    dense16(wm2w, kLabelChannels, kWmHidden, p.wm_hidden.data(), wm2b,
            kShWm2, BrainAct::kLinear, p.yhat.data());
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
    t.s = p.s; t.a = p.a; t.y = y;
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
    const BrainLayout& L = brain_layout();
    std::int8_t* base = store->arena.w8(s.woff);
    std::int8_t* wm2w = base + L.wm2w;
    std::int16_t* wm2b =
        reinterpret_cast<std::int16_t*>(base + L.wm2b);
    const std::int64_t eta = 384 + sigma / 2;      // permille-ish lr 384..884
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
        for (std::int16_t v : s.h) mix(static_cast<std::uint16_t>(v));
        for (std::int8_t v : s.last_action) mix(static_cast<std::uint8_t>(v));
        // weights: hash the arena bytes of this brain's block
        const std::int8_t* wb = store.arena.w8(s.woff);
        for (std::uint64_t i = 0; i < s.wbytes; ++i)
            mix(static_cast<std::uint8_t>(wb[i]));
        mix(s.replay_count); mix(s.replay_next);
        for (const Transition& t : s.replay) {
            mix(t.valid ? 1 : 0);
            if (!t.valid) continue;
            mix(t.event_id); mix(t.sigma); mix(t.charge);
            for (std::int8_t v : t.s) mix(static_cast<std::uint8_t>(v));
            for (std::int8_t v : t.a) mix(static_cast<std::uint8_t>(v));
            for (std::int8_t v : t.y) mix(static_cast<std::uint8_t>(v));
        }
    }
    mix(store.wm_err_sum); mix(store.wm_n);
    mix(store.sigma_sum); mix(store.sigma_n);
    mix(store.surprise_events);
    return h;
}

}  // namespace simlab
