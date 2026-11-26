#include "simlab/Brain.hpp"
#include "simlab/GenePool.hpp"
#include "simlab/Rng.hpp"
#include "simlab/World.hpp"
#include "simlab/Cognition.hpp"
#include "simlab/Causal.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <climits>

namespace simlab {

// ================= hash-driven mutation (kept tools) ==========================

std::int8_t brain_rand_weight(std::uint64_t seed, std::uint64_t salt,
                              std::uint64_t index, std::int32_t spread) {
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
            w[i] = static_cast<std::int8_t>(
                v > 127 ? 127 : (v < -128 ? -128 : v));
        }
    }
}

// ---- BrainArena ----------------------------------------------------------------

void BrainArena::reserve(std::uint64_t cells) {
    if (cells <= slab_cells_) return;
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

// ================= connectome genome params ====================================

namespace {

inline std::int32_t clampi(std::int64_t v, std::int64_t lo,
                           std::int64_t hi) {
    return static_cast<std::int32_t>(v < lo ? lo : (v > hi ? hi : v));
}

// canonical Standard-profile populations (BrainModule order)
const std::array<std::int8_t, kBrainModuleCount> kDefaultPops = {
    24, 24, 16, 24, 24, 16, 48, 48, 80, 64, 48, 24
};

}  // namespace

BrainGenome brain_default_genome() {
    BrainGenome g;
    g.valid = true;
    g.slow.assign(kConnectomeBytes, 0);
    for (int m = 0; m < kBrainModuleCount; ++m)
        g.slow[m] = kDefaultPops[m];
    g.slow[12] = 100;    // threshold 800
    g.slow[13] = 19;     // reset -200  ((19*16)-512+8)
    g.slow[14] = 5;      // leak_shift
    g.slow[15] = 2;      // refractory
    g.slow[16] = 25;     // ei 200 permille
    g.slow[17] = 15;     // density 120 permille
    // tract gains stay 0 -> kBrainTracts table defaults
    return g;
}

void brainnet_sanitize(std::vector<std::int8_t>& slow) {
    if (slow.size() < kConnectomeBytes) slow.resize(kConnectomeBytes, 0);
    auto cl = [](std::int8_t& v, std::int32_t lo, std::int32_t hi) {
        v = static_cast<std::int8_t>(clampi(v, lo, hi));
    };
    for (int m = 0; m < kBrainModuleCount; ++m) cl(slow[m], 0, 100);
    cl(slow[12], 40, 160);   // threshold code
    cl(slow[13], 0, 64);     // reset code
    cl(slow[14], 3, 8);      // leak
    cl(slow[15], 1, 6);      // refractory
    cl(slow[16], 0, 127);    // ei
    cl(slow[17], 0, 127);    // density
    for (std::size_t t = 18; t < 42; ++t) cl(slow[t], 0, 127);
    // 42..95 reserved: keep raw int8 (future parameters)
}

BrainNetParams brainnet_params_of(const BrainGenome& g) {
    BrainNetParams p;
    if (!g.valid || g.slow.size() < kConnectomeBytes) {
        const BrainGenome d = brain_default_genome();
        return brainnet_params_of(d);
    }
    for (int m = 0; m < kBrainModuleCount; ++m)
        p.pops[m] = static_cast<std::uint16_t>(
            clampi(g.slow[m], 0, 100));
    p.threshold = static_cast<std::int16_t>(
        clampi(g.slow[12], 40, 160) * 8);
    p.reset = static_cast<std::int16_t>(
        clampi(g.slow[13], 0, 64) * 16 - 512 + 8);
    p.leak_shift = static_cast<std::uint8_t>(clampi(g.slow[14], 3, 8));
    p.refractory = static_cast<std::uint8_t>(clampi(g.slow[15], 1, 6));
    p.ei_permille = static_cast<std::uint8_t>(clampi(g.slow[16], 0, 127) * 8);
    p.density_permille =
        static_cast<std::uint16_t>(clampi(g.slow[17], 0, 127) * 8);
    for (std::size_t t = 0; t < kBrainTractCount; ++t)
        p.tract_gain[t] = g.slow[18 + t] > 0
                              ? static_cast<std::int16_t>(g.slow[18 + t] * 2)
                              : 0;
    return p;
}

// ================= invention helpers (kept) ====================================

namespace {

inline bool invent_verb(int v) {
    return v == 9 || v == 10 || v == 24 || v == 28;
}
std::uint64_t invent_key(int verb, MaterialId a, MaterialId b) {
    std::uint64_t x = a, y = b;
    if (x > y) std::swap(x, y);
    std::uint64_t h = Rng::hash(static_cast<std::uint64_t>(verb));
    h = Rng::hash(h ^ x);
    h = Rng::hash(h ^ (y << 1));
    return h | 1;
}
bool invent_tried(const BrainSlot& s, std::uint64_t key) {
    for (std::uint64_t k : s.tried) if (k == key) return true;
    return false;
}
void invent_mark(BrainSlot& s, std::uint64_t key) {
    if (invent_tried(s, key)) return;
    s.tried[s.tried_next & 63] = key;
    s.tried_next = (s.tried_next + 1) & 63;
}

// stratified replay push shared by brain_post / brain_assimilate
void push_replay(BrainSlot& s, const Transition& t) {
    if (s.replay_count < 16) {
        s.replay[s.replay_next] = t;
        s.replay_next = (s.replay_next + 1) & 15;
        s.replay_count += 1;
    } else {
        std::int64_t best = -1;
        std::int32_t worst = 0;
        for (int i = 0; i < 16; ++i) {
            const std::int64_t sc =
                static_cast<std::int64_t>(s.replay[i].charge) +
                s.replay[i].sigma;
            if (best < 0 || sc < best) { best = sc; worst = i; }
        }
        s.replay[worst] = t;
    }
}

// engram pattern snapshot: wh[96] = hippocampus(48)+prefrontal(48)
// membrane readout, w1h[64] = sensory cortex perceptual trace. Membrane
// int16 -> int8 via /256. Modules smaller than their share contribute
// zeros for the missing tail — deterministic.
void engram_snapshot(const BrainNet& net, std::array<std::int8_t, 96>& wh,
                     std::array<std::int8_t, 64>& w1h) {
    wh.fill(0); w1h.fill(0);
    auto q = [](std::int16_t m) -> std::int8_t {
        const std::int32_t v = m / 256;
        return static_cast<std::int8_t>(clampi(v, -128, 127));
    };
    const BrainModuleState& hip =
        net.modules[static_cast<int>(BrainModule::kHippocampus)];
    const BrainModuleState& pfc =
        net.modules[static_cast<int>(BrainModule::kPrefrontal)];
    for (std::uint32_t i = 0; i < hip.count && i < 48; ++i)
        wh[i] = q(net.st[hip.first + i].membrane);
    for (std::uint32_t i = 0; i < pfc.count && i < 48; ++i)
        wh[48 + i] = q(net.st[pfc.first + i].membrane);
    const BrainModuleState& sen =
        net.modules[static_cast<int>(BrainModule::kSensoryCortex)];
    for (std::uint32_t i = 0; i < sen.count && i < 64; ++i)
        w1h[i] = q(net.st[sen.first + i].membrane);
}

// ---- Phase 3-5: vital state (neuromod / emotion / homeostasis) ----------------
// All integer, all deterministic. Called once per brain_decide / brain_post.

inline std::int16_t clampi16(std::int64_t v) {
    return static_cast<std::int16_t>(clampi(v, -1000, 1000));
}

// modality group for a sensor channel (Sensor.hpp layout)
inline int chan_group(int c) {
    if (c < 24) return kBGrpSomatic;      // 0..15 intero, 16..23 neuro
    if (c < 52) return kBGrpVisual;       // 24..51 entity slots
    if (c < 68) return kBGrpSomatic;      // 52..67 material focus
    if (c < 72) return kBGrpSocial;       // 68..71 social
    if (c < 80) return kBGrpVisual;       // 72..79 environment
    if (c < 112) return kBGrpMotor;       // 80..111 efference copy
    return kBGrpVisual;                   // 112..116 conditioning tail
}

// amygdala context key: material + coarse position bucket — learned
// associations only, no hard-coded threat list (spec §5).
std::uint64_t fear_ctx_of(const Entity* me, MaterialId m) {
    std::uint64_t h = Rng::hash(static_cast<std::uint64_t>(m) + 0xFEA201);
    if (me) {
        h = Rng::hash(h ^ (static_cast<std::uint64_t>(
                     (me->pos.x / 64 + 4096) & 0xFFFF) << 20));
        h = Rng::hash(h ^ static_cast<std::uint64_t>(
                     (me->pos.y / 64 + 4096) & 0xFFFF));
    }
    return h | 1;
}

void brain_vital_tick(World& w, BrainSlot& s, const Entity* me,
                      const AgentMind* mind) {
    BrainVital& v = s.vital;
    static thread_local SeqPidCache vc_;
    SeqPid vp(vc_, w.props);
    auto prop = [&](const char* n) -> std::int64_t {
        const PropId p = vp(n);
        return (p != kNoProp && me) ? w.prop_int(*me, p, 0) : 0;
    };
    const std::int64_t hunger = prop("hunger"), thirst = prop("thirst"),
                      fatigue = prop("fatigue");
    const std::int64_t panic = mind ? std::abs(mind->neuro.panic) : 0;
    const std::int64_t rage = mind ? std::abs(mind->neuro.rage) : 0;
    const std::int64_t grief = mind ? std::abs(mind->neuro.grief) : 0;
    std::int64_t tempstress = 0;
    if (me && w.terrain.contains(me->pos)) {
        const std::int32_t t = w.terrain.temp_at(me->pos);
        tempstress = std::abs(static_cast<std::int64_t>(t) - 500) / 4;
    }
    v.drive_hunger = clampi16(hunger);
    v.drive_thirst = clampi16(thirst);
    v.drive_fatigue = clampi16(fatigue);
    v.drive_temp = static_cast<std::int16_t>(clampi(tempstress, 0, 1000));
    v.drive_stress = static_cast<std::int16_t>(clampi(
        (panic + grief) * 2 / 3 + v.drive_temp / 4, 0, 1000));
    v.drive_repro = 0;   // no dedicated reproductive-drive prop yet
    // sleep pressure tracks fatigue (decays on rest in brain_post)
    v.sleep_pressure = static_cast<std::int16_t>(clampi(
        (static_cast<std::int64_t>(v.drive_fatigue) * 3 +
         v.sleep_pressure) / 4, 0, 1000));

    // ---- neuromodulators: leak toward targets at distinct rates -------------
    // shifts = update rate class (smaller = faster). dopa/norepi fast,
    // sero/gaba/glut intermediate, cortisol slow endocrine (spec §14.1).
    const std::int64_t stress = panic + grief;
    const std::int64_t satiety = 1000 - std::max(hunger, thirst);
    const std::int64_t tgt[kBModCount] = {
        clampi(v.last_reward * 2, 0, 1000),                    // dopa: RPE
        clampi(satiety, 0, 1000),                              // sero: satiety
        clampi(v.last_sigma + (panic + rage) / 2, 0, 1000),    // norepi: arousal
        clampi(v.last_sigma / 2 + stress / 8, 0, 1000),        // achi: encoding
        clampi(fatigue, 0, 1000),                              // gaba: fatigue
        clampi(v.last_sigma, 0, 1000),                         // glut: learning
        clampi(stress, 0, 1000)                                // cortisol: stress
    };
    static const std::uint8_t kModShift[kBModCount] = {3, 5, 3, 4, 5, 4, 6};
    for (int i = 0; i < kBModCount; ++i) {
        const std::int64_t d = tgt[i] - v.mod[i];
        v.mod[i] = static_cast<std::int16_t>(
            clampi(static_cast<std::int64_t>(v.mod[i]) + (d >> kModShift[i]),
                   0, 1000));
    }

    // ---- emotional dimensions (emerge from drives+neuro+memory) --------------
    std::int64_t fear = panic;
    const std::uint64_t ctx = me ? fear_ctx_of(me, kNoMaterial) : 0;
    for (const BrainVital::FearTrace& f : v.fears)
        if (f.ctx == ctx && f.ctx != 0)
            fear = std::max(fear,
                static_cast<std::int64_t>(std::abs(f.valence)) *
                    f.strength / 1000);
    const std::int64_t novelty =
        mind ? static_cast<std::int64_t>(mind->discoveries.size()) * 50 : 0;
    v.emo[kBEmoFear] = clampi16(fear);
    v.emo[kBEmoAnger] = clampi16(rage);
    v.emo[kBEmoCuriosity] =
        clampi16(std::min<std::int64_t>(1000, novelty + v.last_sigma / 4));
    v.emo[kBEmoSadness] = clampi16(grief);
    v.emo[kBEmoJoy] = clampi16(std::max<std::int64_t>(0, v.last_reward));
    v.emo[kBEmoDisgust] = 0;   // toxin-driven; no toxin valence channel yet
    v.emo[kBEmoSurprise] = clampi16(v.last_sigma);
    v.emo[kBEmoTrust] = 0;
    v.emo[kBEmoAttachment] = 0;
}

// thalamic gating: attention-weighted sensory gain per modality group (Q8).
// Arousal (norepi) + attention (achi) amplify everything; fear amplifies
// visual+somatic (threat monitoring); homeostatic urgency amplifies
// somatic; social channel boosted under stress.
void brain_attn_update(BrainVital& v) {
    const std::int64_t ne = v.mod[kBModNorepi], ac = v.mod[kBModAchi];
    const std::int64_t fear = std::abs(v.emo[kBEmoFear]);
    const std::int64_t urg =
        std::max({v.drive_hunger, v.drive_thirst, v.drive_fatigue,
                  v.drive_temp, v.drive_stress});
    const std::int64_t base = 256 + (ne + ac) / 8;
    const std::int64_t g[kBGrpCount] = {
        base + fear / 4 + urg / 8,          // visual
        base + v.drive_stress / 4,          // social/auditory
        base + fear / 2 + urg / 4,          // somatic
        base                               // motor (efference copy)
    };
    for (int i = 0; i < kBGrpCount; ++i)
        v.attn_gain[i] = static_cast<std::int16_t>(clampi(g[i], 64, 1024));
}

// fear conditioning write: aversive event -> context-keyed trace.
// valence negative; strength saturates at 1000; bounded 8-slot table,
// evicts the weakest trace when full (deterministic order).
void brain_fear_upsert(BrainVital& v, std::uint64_t ctx,
                       std::int64_t aversity, std::uint64_t tick) {
    if (ctx == 0 || aversity <= 0) return;
    const std::int16_t val =
        static_cast<std::int16_t>(-clampi(aversity, 100, 1000));
    std::int16_t add = static_cast<std::int16_t>(
        clampi(200 + aversity / 4, 200, 500));
    int slot = -1, weakest = 0;
    std::int64_t wscore = -1;
    for (int i = 0; i < 8; ++i) {
        BrainVital::FearTrace& f = v.fears[i];
        if (f.ctx == ctx) { slot = i; break; }
        if (f.ctx == 0) { slot = i; break; }
        const std::int64_t sc =
            static_cast<std::int64_t>(std::abs(f.valence)) * f.strength;
        if (wscore < 0 || sc < wscore) { wscore = sc; weakest = i; }
    }
    if (slot < 0) slot = weakest;
    BrainVital::FearTrace& f = v.fears[slot];
    if (f.ctx != ctx) { f.ctx = ctx; f.valence = val; f.strength = 0; }
    f.valence = static_cast<std::int16_t>(std::min<std::int64_t>(
        f.valence, val));
    f.strength = static_cast<std::int16_t>(
        clampi(f.strength + add, 0, 1000));
    f.last = tick;
}

// sleep consolidation (spec §12): replay top-charge transitions back into
// hippocampus/PFC, strengthen them, apply deterministic forgetting to the
// rest. Only recombines EXISTING replay entries — never invents facts.
void brain_consolidate(BrainSlot& s, std::int64_t comp_permille) {
    comp_permille = std::clamp<std::int64_t>(comp_permille, 0, 1000);
    if (comp_permille == 0 || s.replay_count == 0) return;
    BrainNet& net = s.net;
    // rank replay entries by charge+sigma; top 2 rehearse, rest decay
    int top[2] = {-1, -1};
    std::int64_t bsc[2] = {-1, -1};
    for (int i = 0; i < 16; ++i) {
        const Transition& t = s.replay[i];
        if (!t.valid) continue;
        const std::int64_t sc =
            static_cast<std::int64_t>(t.charge) + t.sigma;
        for (int k = 0; k < 2; ++k)
            if (sc > bsc[k]) {
                for (int j = 1; j > k; --j) { bsc[j] = bsc[j-1]; top[j] = top[j-1]; }
                bsc[k] = sc; top[k] = i; break;
            }
    }
    const BrainModuleState& hip =
        net.modules[static_cast<int>(BrainModule::kHippocampus)];
    const BrainModuleState& pfc =
        net.modules[static_cast<int>(BrainModule::kPrefrontal)];
    auto q = [](std::int8_t v, std::int64_t comp) -> std::int32_t {
        return static_cast<std::int32_t>(v) * 4 *
               static_cast<std::int32_t>(comp) / 1000;
    };
    for (int k = 0; k < 2; ++k) {
        const int i = top[k];
        if (i < 0) continue;
        Transition& t = s.replay[i];
        for (std::uint32_t u = 0; u < hip.count && u < 96; ++u)
            net.inject(static_cast<std::uint8_t>(BrainModule::kHippocampus),
                       static_cast<std::int32_t>(u), q(t.wh[u], comp_permille));
        for (std::uint32_t u = 0; u < pfc.count && u < 64; ++u)
            net.inject(static_cast<std::uint8_t>(BrainModule::kPrefrontal),
                       static_cast<std::int32_t>(u), q(t.w1h[u], comp_permille));
        t.charge = static_cast<std::uint16_t>(
            clampi(static_cast<std::int64_t>(t.charge) + 40, 0, 1000));
    }
    for (Transition& t : s.replay) {
        if (!t.valid) continue;
        const bool rehearsed =
            (top[0] >= 0 && &t == &s.replay[top[0]]) ||
            (top[1] >= 0 && &t == &s.replay[top[1]]);
        if (!rehearsed) {
            // deterministic forgetting: unrehearsed traces fade; below
            // threshold they are pruned (never random deletion)
            t.charge = static_cast<std::uint16_t>(
                t.charge > 0 ? t.charge - 1 : 0);
            if (t.charge < 20 && t.sigma < 100) t.valid = false;
        }
    }
    for (int k = 0; k < 2; ++k) net.step();
}

// read a brain_* hyper-gene locus (integer allele) with a default
std::int64_t brain_locus(const World& w, const Entity* e,
                         const char* name, std::int64_t dflt) {
    if (!e || e->genome.loci.empty()) return dflt;
    const PropertyDef* d = w.props.find_name(name);
    if (!d) return dflt;
    const std::int64_t* v = e->genome.get(d->id);
    return v ? *v : dflt;
}

}  // namespace

// ================= slot lifecycle ==============================================

BrainSlot& brain_slot(World& w, BrainStore& store, EntityId npc,
                      const BrainGenome* genome) {
    auto it = store.slots.find(npc);
    if (it != store.slots.end()) return it->second;

    BrainGenome g;
    if (genome && genome->valid) {
        g = *genome;
    } else {
        BrainGenome poolg;
        if (w.gene_pool && w.gene_pool->derive(w.world_seed, npc, poolg)) {
            g = poolg;
        } else {
            g = brain_default_genome();
            const Entity* ge = w.find(npc);
            g.lr_permille = static_cast<std::int32_t>(
                brain_locus(w, ge, "brain_lr", 400));
            g.mut_scale = static_cast<std::int32_t>(
                brain_locus(w, ge, "brain_mut_scale", 500));
            g.trace_decay = static_cast<std::int32_t>(
                brain_locus(w, ge, "brain_trace_decay", 900));
        }
    }
    brainnet_sanitize(g.slow);
    const BrainNetParams p = brainnet_params_of(g);

    BrainSlot slot;
    slot.genome = g;
    slot.vital.mod.fill(384);          // baseline tonic modulators
    slot.vital.attn_gain.fill(256);    // unity thalamic gain (Q8)
    slot.net.init(w.world_seed, npc, p.pops, p.threshold, p.reset,
                  p.leak_shift, p.refractory, p.ei_permille,
                  p.density_permille, p.tract_gain);
    auto [ins, ok] = store.slots.emplace(npc, std::move(slot));
    (void)ok;
    return ins->second;
}

void brain_free_slot(BrainStore& store, EntityId npc) {
    store.slots.erase(npc);
}

void brain_gc(World& w, BrainStore& store) {
    if (w.speculative) return;
    std::vector<EntityId> dead;
    for (const auto& [id, slot] : store.slots) {
        (void)slot;
        const Entity* e = w.find(id);
        if (!e || !e->alive) dead.push_back(id);
    }
    for (EntityId id : dead) brain_free_slot(store, id);
}

// ================= pre / post ==================================================

void brain_pre(World& w, EntityId npc, const InteractionContext& ctx) {
    if (!w.brain_shadow || w.speculative) return;
    if (!w.find_mind(npc)) return;
    if (!w.brains_) w.brains_ = std::make_shared<BrainStore>();
    BrainSlot& s = brain_slot(w, *w.brains_, npc);
    BrainPending& p = s.pending;
    p.s = sensor_encode(w, npc, &s.last_action);
    p.a = sensor_action(ctx);
    p.pm_a = kNoMaterial; p.pm_b = kNoMaterial;
    if (const Entity* me = w.find(npc)) p.pm_pos = me->pos;
    for (EntityId tid : ctx.targets) {
        if (tid == ctx.actor) continue;
        const Entity* t = w.find(tid);
        if (!t || !t->alive || t->material == kNoMaterial) continue;
        if (p.pm_a == kNoMaterial) p.pm_a = t->material;
        else { p.pm_b = t->material; break; }
    }
    p.tick = w.clock_.tick();
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

    // surprise proxy: outcome magnitude (the SNN has no separate predictor
    // yet — Phase 6 plasticity will give it one). Same 0..1000 scale.
    std::int64_t l1 = 0;
    for (int i = 0; i < kLabelChannels; ++i)
        l1 += std::abs(static_cast<std::int32_t>(y[i]));
    const std::int64_t sigma = std::min<std::int64_t>(1000, l1 / 8);
    const std::int64_t charge =
        mind ? std::min<std::int64_t>(1000,
               std::abs(mind->neuro.panic) + std::abs(mind->neuro.rage) +
               std::abs(mind->neuro.grief)) / 3 : 0;

    store->sigma_sum += sigma;
    store->sigma_n += 1;
    if (sigma >= 500) store->surprise_events += 1;

    Transition t;
    t.s = p.s; t.a = p.a; t.y = y;
    engram_snapshot(s.net, t.wh, t.w1h);
    t.sigma = static_cast<std::uint16_t>(sigma);
    t.charge = static_cast<std::uint16_t>(charge);
    t.event_id = ev.id;
    t.valid = true;
    push_replay(s, t);
    s.last_action = p.a;

    // ---- Phase 5: phasic teaching signals + amygdala conditioning ------------
    s.vital.last_reward = static_cast<std::int16_t>(clampi(y[24], -1000, 1000));
    s.vital.last_sigma = static_cast<std::int16_t>(sigma);
    s.vital.last_charge = static_cast<std::int16_t>(charge);
    {
        const Entity* me = w.find(npc);
        const bool aversive =
            ev.failed || sigma >= 600 || charge >= 600;
        if (aversive && me) {
            const std::uint64_t ctx = fear_ctx_of(me, p.pm_a);
            brain_fear_upsert(s.vital, ctx,
                              sigma + charge / 2, w.clock_.tick());
            // cortisol surge: slow endocrine stress channel
            s.vital.mod[kBModCortisol] = static_cast<std::int16_t>(
                clampi(static_cast<std::int64_t>(s.vital.mod[kBModCortisol]) +
                           120, 0, 1000));
        }
        // cerebellar motor error: move verbs get prediction-error current
        int verb_idx = -1;
        for (int i = 0; i < kActionVerbs; ++i)
            if (p.a[i] == 100) { verb_idx = i; break; }
        if (verb_idx >= 0 && me) {
            const char* vn = sensor_verb_table()[verb_idx];
            if (std::strcmp(vn, "move") == 0) {
                const std::int32_t dx = me->pos.x - p.pm_pos.x;
                const std::int32_t dy = me->pos.y - p.pm_pos.y;
                const std::int64_t moved2 =
                    static_cast<std::int64_t>(dx) * dx +
                    static_cast<std::int64_t>(dy) * dy;
                // prediction error proxy: surprise + collision/failure
                std::int64_t perr = sigma - 200;
                if (ev.failed) perr += 300;
                if (moved2 == 0 && !ev.failed) perr += 150;  // stalled
                perr = clampi(perr, -400, 800);
                BrainNet& net = s.net;
                const BrainModuleState& cb =
                    net.modules[static_cast<int>(BrainModule::kCerebellum)];
                const BrainModuleState& mc =
                    net.modules[static_cast<int>(BrainModule::kMotorCortex)];
                for (std::uint32_t i = 0; i < cb.count; ++i)
                    net.inject(static_cast<std::uint8_t>(
                                   BrainModule::kCerebellum),
                               static_cast<std::int32_t>(i),
                               static_cast<std::int32_t>(perr) / 4);
                for (std::uint32_t i = 0; i < mc.count; ++i)
                    net.inject(static_cast<std::uint8_t>(
                                   BrainModule::kMotorCortex),
                               static_cast<std::int32_t>(i),
                               static_cast<std::int32_t>(perr) / 8);
                net.step();
            }
            // working memory: last committed action (spec §10.2)
            s.vital.wm_verb = verb_idx;
            s.vital.wm_conf = static_cast<std::int16_t>(
                clampi(1000 - sigma, 0, 1000));
        }
        // sleep consolidation on rest (spec §12)
        if (verb_idx >= 0 &&
            std::strcmp(sensor_verb_table()[verb_idx], "rest") == 0) {
            s.vital.sleep_pressure = static_cast<std::int16_t>(std::max(
                0, static_cast<int>(s.vital.sleep_pressure) - 250));
            brain_consolidate(s, 500);
        }
    }

    // ---- outcome teaching signal -------------------------------------------
    // Inject the homeostatic reward (label ch24) as a modulatory current
    // into Hypothalamus (drive) and Basal Ganglia (action valuation), then
    // let the network settle for a few ticks. Phase 6 replaces this with
    // proper dopamine/eligibility plasticity.
    {
        BrainNet& net = s.net;
        const std::int32_t rew = static_cast<std::int32_t>(y[24]);
        const BrainModuleState& hm =
            net.modules[static_cast<int>(BrainModule::kHypothalamus)];
        const BrainModuleState& bg =
            net.modules[static_cast<int>(BrainModule::kBasalGanglia)];
        for (std::uint32_t i = 0; i < hm.count; ++i)
            net.inject(static_cast<std::uint8_t>(
                           BrainModule::kHypothalamus),
                       static_cast<std::int32_t>(i), rew);
        for (std::uint32_t i = 0; i < bg.count; ++i)
            net.inject(static_cast<std::uint8_t>(
                           BrainModule::kBasalGanglia),
                       static_cast<std::int32_t>(i), rew * 2);
        for (int k = 0; k < 4; ++k) net.step();
    }

    // ---- open-ended invention: mark tried combos (curiosity bookkeeping) ----
    {
        int verb = -1;
        for (int i = 0; i < kActionVerbs; ++i)
            if (p.a[i] == 100) { verb = i; break; }
        if (verb >= 0 && invent_verb(verb) && p.pm_a != kNoMaterial) {
            const std::uint64_t ikey = invent_key(verb, p.pm_a, p.pm_b);
            const bool fresh = !invent_tried(s, ikey);
            invent_mark(s, ikey);
            // novelty dopamine: fresh safe transform -> extra BG current
            if (fresh && sigma >= 400) {
                const Entity* me = w.find(npc);
                std::int64_t safety = 0;
                if (me) {
                    const PropId hp = SIMLAB_PID(w.props, "hunger"),
                                   tp = SIMLAB_PID(w.props, "thirst");
                    const std::int64_t hunger =
                        hp != kNoProp ? w.prop_int(*me, hp, 0) : 0;
                    const std::int64_t thirst =
                        tp != kNoProp ? w.prop_int(*me, tp, 0) : 0;
                    const std::int64_t panic =
                        mind ? mind->neuro.panic : 0;
                    safety = std::max<std::int64_t>(
                        0, 1000 - std::max(hunger, thirst) - panic);
                }
                if (safety > 500) {
                    BrainNet& net = s.net;
                    const BrainModuleState& bg =
                        net.modules[static_cast<int>(
                            BrainModule::kBasalGanglia)];
                    for (std::uint32_t i = 0; i < bg.count; ++i)
                        net.inject(static_cast<std::uint8_t>(
                                       BrainModule::kBasalGanglia),
                                   static_cast<std::int32_t>(i), 120);
                    net.step();
                }
            }
        }
    }
}

// ================= decision =====================================================

BrainChoice brain_decide(World& w, EntityId npc) {
    BrainChoice out;
    if (w.speculative) return out;
    const Entity* me = w.find(npc);
    const AgentMind* mind = w.find_mind(npc);
    if (!me || !me->alive || !mind) return out;
    if (!w.brains_) w.brains_ = std::make_shared<BrainStore>();
    BrainStore& store = *w.brains_;
    BrainSlot& s = brain_slot(w, store, npc);
    BrainNet& net = s.net;

    // ---- Phase 3-5: homeostasis, neuromodulation, attention ------------------
    brain_vital_tick(w, s, me, mind);
    brain_attn_update(s.vital);

    SensorVec sv = sensor_encode(w, npc, &s.last_action);
    // perceptual uncertainty (spec §9.2): low acetylcholine -> deterministic
    // hash noise on a quarter of active channels. Never reads hidden state.
    if (s.vital.mod[kBModAchi] < 400) {
        const std::int32_t noise_span = 400 - s.vital.mod[kBModAchi];
        for (int c = 0; c < kSensorChannels; ++c) {
            if (sv[c] == 0) continue;
            const std::uint64_t h =
                Rng::hash(w.world_seed ^ (npc * 0x9E3779B97F4A7C15ull) ^
                          (w.clock_.tick() * 0xBF58476D1CE4E5B9ull) ^
                          static_cast<std::uint64_t>(c));
            if ((h & 3) != 0) continue;
            const std::int32_t j =
                static_cast<std::int32_t>(h % (2 * noise_span + 1)) -
                noise_span;
            sv[c] = static_cast<std::int8_t>(clampi(
                static_cast<std::int64_t>(sv[c]) + j / 16, -128, 127));
        }
    }

    // ---- sensory encoding -----------------------------------------------------
    // Channel c -> Sensory Cortex neuron (c*count)/kSensorChannels, current
    // = channel * 6 (int8 -> ~+-768) scaled by the thalamic attention gain
    // for its modality group (spec §9.1/9.3 — gated, not pass-through).
    // Intero channels 0..15 additionally drive Hypothalamus (homeostatic
    // motivation); last_action re-enters via Cerebellum (efference copy).
    {
        const BrainModuleState& sm =
            net.modules[static_cast<int>(BrainModule::kSensoryCortex)];
        for (int c = 0; c < kSensorChannels; ++c) {
            if (sv[c] == 0 || sm.count == 0) continue;
            const std::int32_t idx =
                static_cast<std::int32_t>(
                    static_cast<std::int64_t>(c) * sm.count /
                    kSensorChannels);
            const std::int32_t gain =
                s.vital.attn_gain[chan_group(c)];
            net.inject(static_cast<std::uint8_t>(
                           BrainModule::kSensoryCortex), idx,
                       static_cast<std::int32_t>(sv[c]) * 6 * gain / 256);
        }
        const BrainModuleState& hm =
            net.modules[static_cast<int>(BrainModule::kHypothalamus)];
        for (int c = 0; c < 16 && hm.count; ++c) {
            if (sv[c] == 0) continue;
            const std::int32_t idx = static_cast<std::int32_t>(
                static_cast<std::int64_t>(c) * hm.count / 16);
            net.inject(static_cast<std::uint8_t>(
                           BrainModule::kHypothalamus), idx,
                       static_cast<std::int32_t>(sv[c]) * 8);
        }
        const BrainModuleState& cb =
            net.modules[static_cast<int>(BrainModule::kCerebellum)];
        if (cb.count) {
            for (int i = 0; i < kActionVerbs; ++i) {
                if (s.last_action[i] == 0) continue;
                const std::int32_t idx = static_cast<std::int32_t>(
                    static_cast<std::int64_t>(i) * cb.count / kActionVerbs);
                net.inject(static_cast<std::uint8_t>(
                               BrainModule::kCerebellum), idx,
                           static_cast<std::int32_t>(s.last_action[i]) * 4);
            }
        }
    }

    // ---- neural burst ----------------------------------------------------------
    // 8 neural ticks: every module fires at least once (language period=8),
    // signals propagate through the delay rings.
    constexpr int kTicks = 8;
    for (int k = 0; k < kTicks; ++k) net.step();

    // ---- readout ----------------------------------------------------------------
    // Verb logits: first 32 Motor Cortex neurons' membranes.
    std::int32_t vlogit[kActionVerbs] = {};
    {
        const BrainModuleState& mm =
            net.modules[static_cast<int>(BrainModule::kMotorCortex)];
        const std::uint32_t nv =
            std::min<std::uint32_t>(kActionVerbs, mm.count);
        for (std::uint32_t v = 0; v < nv; ++v)
            vlogit[v] = net.st[mm.first + v].membrane;
    }
    // Entity saliency: last 4 Association Cortex neurons.
    std::int32_t entsal[kEntitySlots] = {};
    {
        const BrainModuleState& am =
            net.modules[static_cast<int>(BrainModule::kAssociation)];
        const std::uint32_t navail =
            std::min<std::uint32_t>(kEntitySlots, am.count);
        for (std::uint32_t t = 0; t < navail; ++t) {
            const std::uint32_t idx =
                am.count - navail + t;
            entsal[t] = net.st[am.first + idx].membrane;
        }
    }

    // ---- verb mask: known_actions + innate move/rest (unchanged policy) --------
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

    // ---- nearest remembered entities (same bounded insertion as before) -------
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

    // ---- candidate generation ---------------------------------------------------
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
        consider(vlogit[v] / 16, v, -1);
        for (int t = 0; t < nnear; ++t)
            consider(vlogit[v] / 16 + entsal[t] / 16, v, t);
    }
    if (ncand == 0) return out;

    // ---- score candidates: neural base + mind-level biases ----------------------
    const NeuroState& ns = mind->neuro;
    const PropId hp_p = SIMLAB_PID(w.props, "hunger"),
                   tp_p = SIMLAB_PID(w.props, "thirst");
    const std::int64_t hunger_v =
        hp_p != kNoProp ? w.prop_int(*me, hp_p, 0) : 0;
    const std::int64_t thirst_v =
        tp_p != kNoProp ? w.prop_int(*me, tp_p, 0) : 0;
    const std::int64_t safety = std::max<std::int64_t>(0, std::min<std::int64_t>(
        1000, 1000 - std::max(hunger_v, thirst_v) - ns.panic));
    const bool curious = safety > 500;

    // two nearest remembered material-bearing entities (invention bonus)
    EntityId mat_ids[2] = {kNoEntity, kNoEntity};
    MaterialId mat_m[2] = {kNoMaterial, kNoMaterial};
    {
        std::int64_t md2[2] = {0, 0};
        int nm = 0;
        for (const auto& [kid, snap] : mind->memory) {
            if (kid == npc || snap.material == kNoMaterial) continue;
            const std::int64_t dx = snap.pos.x - me->pos.x;
            const std::int64_t dy = snap.pos.y - me->pos.y;
            const std::int64_t d2 = dx * dx + dy * dy;
            if (nm == 2 && d2 >= md2[1]) continue;
            int p = nm < 2 ? nm++ : 1;
            while (p > 0 && md2[p - 1] > d2) {
                md2[p] = md2[p - 1]; mat_ids[p] = mat_ids[p - 1];
                mat_m[p] = mat_m[p - 1]; --p;
            }
            md2[p] = d2; mat_ids[p] = kid; mat_m[p] = snap.material;
        }
    }

    std::int64_t best_total = INT64_MIN;
    int best_c = 0;
    for (int c = 0; c < ncand; ++c) {
        const Cand& cd = cands[c];
        std::int64_t total = cd.score;
        // rage approaches hated memories; panic avoids trauma-tagged ones
        if (cd.tgt >= 0 && cd.tgt < nnear) {
            const auto mit = mind->memory.find(near[cd.tgt].id);
            if (mit != mind->memory.end() &&
                mit->second.engram.emotional_charge < 0) {
                const std::int64_t chg = -mit->second.engram.emotional_charge;
                total += ns.rage * chg / 3072 - ns.panic * chg / 4096;
            }
        }
        // epistemic bonus: untried material transform while safe
        if (curious && invent_verb(cd.verb) &&
            cd.tgt >= 0 && cd.tgt < nnear) {
            const auto mit = mind->memory.find(near[cd.tgt].id);
            if (mit != mind->memory.end() &&
                mit->second.material != kNoMaterial) {
                const MaterialId ma = mit->second.material;
                const MaterialId mb =
                    (cd.verb == 10)
                        ? (near[cd.tgt].id == mat_ids[0] ? mat_m[1] : mat_m[0])
                        : kNoMaterial;
                if (!invent_tried(s, invent_key(cd.verb, ma, mb)))
                    total += 192;
            }
        }
        // tribal doctrine bias (doctrine-free agents: cheap no-op)
        {
            const Entity* tgt_e = nullptr;
            if (cd.tgt >= 0 && cd.tgt < nnear)
                tgt_e = w.find(near[cd.tgt].id);
            total += doctrine_bias_for(w, *mind, *me, cd.verb, tgt_e);
        }
        // deterministic exploration jitter (genome-scaled)
        const std::uint64_t jit =
            Rng::hash(w.world_seed ^ (npc * 0x9E3779B97F4A7C15ull) ^
                      (w.clock_.tick() * 0xBF58476D1CE4E5B9ull) ^
                      (static_cast<std::uint64_t>(c) << 40));
        total += static_cast<std::int64_t>(jit %
            static_cast<std::uint64_t>(1 + s.genome.mut_scale / 100)) -
            (1 + s.genome.mut_scale / 100) / 2;
        // amygdala defensive bias: learned fear amplifies fight/flight
        // verbs (spec §5) — never a hard-coded threat list.
        if (s.vital.emo[kBEmoFear] > 400) {
            const char* vn = sensor_verb_table()[cd.verb];
            if (std::strcmp(vn, "strike") == 0 ||
                std::strcmp(vn, "move") == 0)
                total += s.vital.emo[kBEmoFear] / 3;
        }
        if (total > best_total) { best_total = total; best_c = c; }
    }

    // ---- metabolism: bill neural work -------------------------------------------
    {
        const std::int64_t macs =
            static_cast<std::int64_t>(net.neuron_count) * kTicks * 2 +
            static_cast<std::int64_t>(net.syn_count) / 8;
        s.mac_pending += macs;
    }

    const Cand& chosen = cands[best_c];
    const char* verb = sensor_verb_table()[chosen.verb];
    out.verb = verb;
    out.score = best_total;
    out.target = (chosen.tgt >= 0 && chosen.tgt < nnear)
                     ? near[chosen.tgt].id : kNoEntity;
    if (std::strcmp(verb, "move") == 0) {
        out.has_move = true;
        if (out.target != kNoEntity)
            out.move_to = near[chosen.tgt].pos;
        else {
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

// ================= engram assimilation ==========================================

void brain_assimilate(World& w, EntityId npc, int verb,
                      std::int64_t charge, std::int64_t sigma,
                      std::uint64_t salt,
                      const std::int8_t* wh, const std::int8_t* w1h,
                      const std::int8_t* y, std::int64_t comp_permille) {
    if (w.speculative || !w.brain_shadow || !w.brains_) return;
    BrainStore& store = *w.brains_;
    BrainSlot& s = brain_slot(w, store, npc);
    comp_permille = std::clamp<std::int64_t>(comp_permille, 0, 1000);
    if (comp_permille == 0) return;

    Transition t{};
    if (verb >= 0 && verb < kActionVerbs) t.a[verb] = 100;
    t.sigma = static_cast<std::uint16_t>(
        std::clamp<std::int64_t>(sigma, 0, 1000));
    t.charge = static_cast<std::uint16_t>(
        std::clamp<std::int64_t>(charge, 0, 1000));
    std::copy(wh, wh + t.wh.size(), t.wh.begin());
    std::copy(w1h, w1h + t.w1h.size(), t.w1h.begin());
    std::copy(y, y + t.y.size(), t.y.begin());
    t.event_id = salt;
    t.valid = true;
    push_replay(s, t);

    // neural imprint: drive the engram pattern into hippocampus (wh) and
    // prefrontal (w1h) as scaled currents, reward channel into basal
    // ganglia; a few ticks let the pattern settle into the network.
    BrainNet& net = s.net;
    const BrainModuleState& hip =
        net.modules[static_cast<int>(BrainModule::kHippocampus)];
    const BrainModuleState& pfc =
        net.modules[static_cast<int>(BrainModule::kPrefrontal)];
    const BrainModuleState& bg =
        net.modules[static_cast<int>(BrainModule::kBasalGanglia)];
    for (std::uint32_t i = 0; i < hip.count && i < 96; ++i)
        net.inject(static_cast<std::uint8_t>(BrainModule::kHippocampus),
                   static_cast<std::int32_t>(i),
                   static_cast<std::int32_t>(wh[i]) * 4 *
                       static_cast<std::int32_t>(comp_permille) / 1000);
    for (std::uint32_t i = 0; i < pfc.count && i < 64; ++i)
        net.inject(static_cast<std::uint8_t>(BrainModule::kPrefrontal),
                   static_cast<std::int32_t>(i),
                   static_cast<std::int32_t>(w1h[i]) * 4 *
                       static_cast<std::int32_t>(comp_permille) / 1000);
    if (bg.count)
        net.inject(static_cast<std::uint8_t>(BrainModule::kBasalGanglia),
                   0, static_cast<std::int32_t>(y[24]) * 2 *
                          static_cast<std::int32_t>(comp_permille) / 1000);
    for (int k = 0; k < 4; ++k) net.step();
}

// ================= fingerprint ==================================================

std::uint64_t brain_state_hash(const BrainStore& store) {
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&](std::uint64_t v) {
        h ^= v; h *= 1099511628211ull;
    };
    mix(store.slots.size());
    for (const auto& [id, s] : store.slots) {
        mix(static_cast<std::uint64_t>(id));
        // genome params (connectome is derived from these + seeds)
        mix(static_cast<std::uint32_t>(s.genome.hidden));
        mix(static_cast<std::uint32_t>(s.genome.lr_permille));
        mix(static_cast<std::uint32_t>(s.genome.trace_decay));
        mix(static_cast<std::uint32_t>(s.genome.mut_scale));
        for (std::int8_t v : s.genome.slow)
            mix(static_cast<std::uint8_t>(v));
        // neural dynamic state (synapses/weights are derived -> not hashed)
        const BrainNet& n = s.net;
        mix(n.neuron_count); mix(n.syn_count); mix(n.tick);
        mix(n.pending_mask); mix(n.dropped_kicks); mix(n.spikes);
        for (const BrainNeuronState& ns : n.st) {
            mix(static_cast<std::uint16_t>(ns.membrane));
            mix(static_cast<std::uint32_t>(ns.syn_input));
            mix(ns.refractory_left); mix(ns.flags);
        }
        for (const BrainModuleState& ms : n.modules) {
            mix(ms.first); mix(ms.count); mix(ms.period); mix(ms.phase);
            mix(ms.last_run); mix(ms.event_accum);
            mix(static_cast<std::uint32_t>(ms.scalar));
        }
        for (const auto& r : n.ring) {
            mix(static_cast<std::uint32_t>(r.size()));
            for (std::uint32_t idx : r) mix(idx);
        }
        for (std::int8_t v : s.last_action) mix(static_cast<std::uint8_t>(v));
        mix(s.pending.valid ? 1 : 0);
        mix(static_cast<std::uint32_t>(s.pending.pm_pos.x));
        mix(static_cast<std::uint32_t>(s.pending.pm_pos.y));
        mix(s.replay_count); mix(s.replay_next);
        mix(static_cast<std::uint64_t>(s.mac_pending));
        mix(static_cast<std::uint64_t>(s.mac_frac));
        mix(static_cast<std::uint32_t>(s.tried_next));
        for (std::uint64_t k : s.tried) mix(k);
        // Phase 3-5 vital state: modulators, emotion, drives, conditioning
        const BrainVital& v = s.vital;
        for (std::int16_t m : v.mod) mix(static_cast<std::uint16_t>(m));
        for (std::int16_t e : v.emo) mix(static_cast<std::uint16_t>(e));
        mix(static_cast<std::uint16_t>(v.drive_hunger));
        mix(static_cast<std::uint16_t>(v.drive_thirst));
        mix(static_cast<std::uint16_t>(v.drive_fatigue));
        mix(static_cast<std::uint16_t>(v.drive_temp));
        mix(static_cast<std::uint16_t>(v.drive_stress));
        mix(static_cast<std::uint16_t>(v.drive_repro));
        for (const BrainVital::FearTrace& f : v.fears) {
            mix(f.ctx); mix(static_cast<std::uint16_t>(f.valence));
            mix(static_cast<std::uint16_t>(f.strength)); mix(f.last);
        }
        for (std::int16_t g : v.attn_gain)
            mix(static_cast<std::uint16_t>(g));
        mix(v.attn_last); mix(static_cast<std::uint16_t>(v.sleep_pressure));
        mix(v.mod_accum);
        mix(static_cast<std::uint32_t>(v.wm_verb));
        mix(static_cast<std::uint64_t>(v.wm_target));
        mix(static_cast<std::uint16_t>(v.wm_conf));
        mix(static_cast<std::uint16_t>(v.last_reward));
        mix(static_cast<std::uint16_t>(v.last_sigma));
        mix(static_cast<std::uint16_t>(v.last_charge));
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

// ================= genome / inheritance =========================================

BrainGenome brain_genome_of(const World& w, const BrainStore& store,
                            EntityId npc) {
    (void)w;
    const auto it = store.slots.find(npc);
    if (it == store.slots.end()) return BrainGenome{};
    return it->second.genome;
}

std::int64_t brain_genetic_distance(const BrainGenome& a,
                                    const BrainGenome& b) {
    if (!a.valid || !b.valid || a.slow.size() != b.slow.size() ||
        a.slow.empty())
        return 1000000;
    std::int64_t sum = 0;
    for (std::size_t i = 0; i < a.slow.size(); ++i)
        sum += std::abs(static_cast<std::int32_t>(a.slow[i]) -
                        static_cast<std::int32_t>(b.slow[i]));
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
    // block crossover on the connectome parameter block (64-byte blocks)
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
    brain_mutate_blocks(g.slow.data(), static_cast<std::int64_t>(n),
                        w.world_seed, a, tick, 3, g.mut_scale);
    if (g.slow.size() != kConnectomeBytes)
        g.slow.resize(kConnectomeBytes, 0);
    brainnet_sanitize(g.slow);
    return g;
}

void brain_install_spawn(World& w, EntityId nid, const BrainGenome& g) {
    if (w.speculative || !g.valid) return;
    if (!w.brains_) w.brains_ = std::make_shared<BrainStore>();
    brain_slot(w, *w.brains_, nid, &g);
}

}  // namespace simlab
