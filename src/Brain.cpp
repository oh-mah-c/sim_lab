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
    g.slow[42] = 6;      // stdp_window ticks
    g.slow[43] = 100;    // plastic_rate 400 permille
    g.slow[44] = 100;    // reward_rate 400 permille
    g.slow[45] = 125;    // mod_sens 1000 permille (/8)
    g.slow[46] = 127;    // rollout_budget 1016 permille (/8, ~always)
    g.slow[47] = 1;      // population profile: 0=min 1=std 2=full
    g.slow[48] = 100;    // empathy/social sensitivity 800 permille (/8)
    return g;
}

// Founder diversity (ALife): generation-0 connectome parameters vary
// deterministically per entity so natural selection has phenotypic
// variance to act on from the first generation. Module populations stay
// canonical (anatomy is fixed); thresholds, leak, tract gains, plasticity,
// neuromodulator sensitivity, rollout budget and empathy all jitter.
// Pure hash of (world_seed, npc) — no RNG state, replay-safe.
BrainGenome brain_founder_genome(std::uint64_t world_seed, EntityId npc) {
    BrainGenome g = brain_default_genome();
    const std::uint64_t idh =
        static_cast<std::uint64_t>(npc) * 0x9E3779B97F4A7C15ull;
    auto draw = [&](std::uint64_t salt, std::int64_t span) {
        const std::uint64_t h =
            Rng::hash(world_seed ^ idh ^ (salt * 0xBF58476D1CE4E5B9ull));
        return static_cast<std::int64_t>(
            h % static_cast<std::uint64_t>(2 * span + 1)) - span;
    };
    auto set8 = [&](std::size_t i, std::int64_t v, std::int64_t lo,
                    std::int64_t hi) {
        g.slow[i] = static_cast<std::int8_t>(clampi(v, lo, hi));
    };
    set8(12, 100 + draw(0x12, 20), 40, 160);    // threshold code
    set8(13, 19 + draw(0x13, 4), 0, 64);        // reset code
    set8(14, 5 + draw(0x14, 1), 3, 8);          // leak_shift
    set8(15, 2 + draw(0x15, 1), 1, 6);          // refractory
    set8(16, 25 + draw(0x16, 8), 0, 127);       // ei permille
    set8(17, 15 + draw(0x17, 6), 0, 127);       // density permille
    // tract gains: perturb around each kBrainTracts table default
    // (slow[t] > 0 overrides gain = slow*2, so centre = gain/2).
    for (std::size_t t = 0; t < kBrainTractCount; ++t) {
        const std::int64_t c = kBrainTracts[t].gain / 2;
        set8(18 + t, c + draw(0x300 + t, c / 4), 1, 127);
    }
    // ALife bootstrap priors: the interoceptive->action and
    // sensory->executive routes get WIDER heritable variance than the
    // generic tracts, so founder populations naturally contain
    // "wired-for-survival" phenotypes (strong Hypothalamus->BG and
    // Sensory->Thalamus->PFC->Motor coupling). Symmetric spread —
    // selection, not the prior, decides which direction wins.
    //   0 Sens->Thal, 2 Thal->PFC, 5 Amyg->Hypo, 8 Hypo->BG,
    //   10 BG->Motor, 11 PFC->BG, 12 PFC->Motor
    static const std::size_t kPriorTracts[] = {0, 2, 5, 8, 10, 11, 12};
    for (std::size_t t : kPriorTracts) {
        const std::int64_t c = kBrainTracts[t].gain / 2;
        set8(18 + t, c + draw(0x700 + t, c * 3 / 8), 1, 127);
    }
    set8(42, 6 + draw(0x42, 3), 0, 16);         // stdp_window
    set8(43, 100 + draw(0x43, 40), 0, 255);     // plastic_rate
    set8(44, 100 + draw(0x44, 40), 0, 255);     // reward_rate
    set8(45, 125 + draw(0x45, 40), 0, 127);     // mod_sens
    set8(46, 127 + draw(0x46, 24), 0, 127);     // rollout_budget
    // slow[47] population profile stays Standard: Minimal/Full rescale
    // every module pop and would break the canonical anatomy contract.
    set8(48, 100 + draw(0x48, 48), 0, 127);     // empathy /8
    // hyper-genes jitter too (locus overrides still win afterwards)
    g.lr_permille = static_cast<std::int32_t>(clampi(
        400 + draw(0x60, 120), 100, 900));
    g.trace_decay = static_cast<std::int32_t>(clampi(
        900 + draw(0x61, 200), 400, 1600));
    g.mut_scale = static_cast<std::int32_t>(clampi(
        500 + draw(0x62, 200), 100, 1200));
    brainnet_sanitize(g.slow);
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
    // Phase 6 plasticity params (heritable, spec §13.1)
    cl(slow[42], 0, 16);    // stdp_window (0 -> default 6)
    cl(slow[43], 0, 255);   // plastic_rate /4 permille
    cl(slow[44], 0, 255);   // reward_rate /4 permille
    // Phase 9 developmental params (heritable, spec §23)
    cl(slow[45], 0, 127);   // mod_sens /8 permille
    cl(slow[46], 0, 127);   // rollout_budget /8 permille
    cl(slow[47], 0, 2);     // population profile enum
    cl(slow[48], 0, 127);   // empathy /8 permille
    // 49..95 reserved: keep raw int8 (future parameters)
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
    // Phase 11: population profile scales module populations
    // (0 = Minimal 0.5x, 1 = Standard 1x, 2 = Full 1.5x), floor 4.
    p.profile = static_cast<std::uint8_t>(clampi(g.slow[47], 0, 2));
    {
        const std::int32_t sc = p.profile == 0 ? 1 : (p.profile == 2 ? 3 : 2);
        for (int m = 0; m < kBrainModuleCount; ++m)
            p.pops[m] = static_cast<std::uint16_t>(
                clampi(static_cast<std::int64_t>(p.pops[m]) * sc / 2, 4, 200));
    }
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
    p.stdp_window = static_cast<std::uint8_t>(
        g.slow[42] == 0 ? 6 : clampi(g.slow[42], 2, 16));
    p.plastic_rate = clampi(g.slow[43], 0, 255) * 4;
    p.reward_rate = clampi(g.slow[44], 0, 255) * 4;
    p.mod_sens_pm = clampi(g.slow[45], 0, 127) * 8;
    p.rollout_budget_pm = clampi(g.slow[46], 0, 127) * 8;
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
        clampi(384 + v.last_rpe / 2, 0, 1000),               // dopa: RPE
        clampi(satiety, 0, 1000),                              // sero: satiety
        clampi(v.last_sigma + (panic + rage) / 2, 0, 1000),    // norepi: arousal
        clampi(v.last_sigma / 2 + stress / 8, 0, 1000),        // achi: encoding
        clampi(fatigue, 0, 1000),                              // gaba: fatigue
        clampi(v.last_sigma, 0, 1000),                         // glut: learning
        clampi(stress, 0, 1000)                                // cortisol: stress
    };
    static const std::uint8_t kModShift[kBModCount] = {3, 5, 3, 4, 5, 4, 6};
    // Phase 9: genome-tunable neuromodulator sensitivity scales the
    // per-tick pull toward each modulator's target (spec §23).
    const std::int64_t sens = s.net.mod_sens_pm;
    for (int i = 0; i < kBModCount; ++i) {
        const std::int64_t step =
            (tgt[i] - v.mod[i]) >> kModShift[i];
        v.mod[i] = static_cast<std::int16_t>(
            clampi(static_cast<std::int64_t>(v.mod[i]) +
                       step * sens / 1000, 0, 1000));
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
    // attachment emerges from social cognition: mean familiarity/trust of
    // remembered conspecifics (0 while alone) — feeds the social-proximity
    // drive in brain_decide (spec §21).
    {
        std::int64_t acc = 0;
        int nsoc = 0;
        for (const BrainSocial& so : s.cog.social) {
            if (so.key == 0) continue;
            acc += std::max<std::int64_t>(0, so.familiarity) +
                   std::max<std::int64_t>(0, so.trust);
            ++nsoc;
        }
        v.emo[kBEmoAttachment] = nsoc
            ? static_cast<std::int16_t>(clampi(acc / nsoc / 2, 0, 1000))
            : static_cast<std::int16_t>(0);
    }
    v.emo[kBEmoTrust] = 0;
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

// ---- Phase 6: reward-modulated plasticity -------------------------------------
// Eligibility = recently delivered plastic kicks still inside the reward
// window. RPE sign decides direction: positive -> potentiate eligible
// synapses, negative -> depress. Aversive events force a negative floor so
// failure always teaches (spec §13.3).
void brain_reward_plasticity(BrainSlot& s, std::int64_t rpe,
                             bool aversive) {
    BrainNet& net = s.net;
    std::int64_t eff = rpe;
    if (aversive && eff > -200) eff = -200;
    if (eff == 0) return;
    const std::int32_t rate =
        net.reward_rate * net.plastic_scale_pm / 1000;
    const std::int32_t gate = std::max<std::int32_t>(
        64, std::min<std::int32_t>(2500, net.stdp_gate));
    // dw delta: |rpe| 1000 -> ~rate*gate/100000*8 dw units; clamp sane.
    std::int32_t delta = static_cast<std::int32_t>(
        std::max<std::int64_t>(1, std::abs(eff) * rate * gate /
                                     (1000 * 100000) * 8));
    delta = std::min(delta, 24);
    if (eff < 0) delta = -delta;
    const std::uint32_t ewin = net.stdp_window * 6;  // reward window wider
    for (std::uint32_t u = 0; u < net.neuron_count; ++u) {
        for (std::uint64_t e : net.kick_hist[u]) {
            if (e == ~0ull) continue;
            if (net.tick - (e >> 24) > ewin) continue;
            const std::uint32_t k =
                static_cast<std::uint32_t>(e & 0xFFFFFFull);
            if (k >= net.syn_count || !(net.syn[k].flags & 1)) continue;
            std::int32_t d = net.dw[k] + delta;
            net.dw[k] = static_cast<std::int8_t>(
                std::max(-BrainNet::kDwMax, std::min(BrainNet::kDwMax, d)));
        }
    }
}

// ---- Phase 7: goal management -------------------------------------------------
// Goals derive from homeostatic drives + emotional state each decide — they
// bias candidates but NEVER become actions by themselves (spec §17).
// origin codes: 0=homeostatic, 1=survival, 2=exploration, 3=social,
//               4=resource, 5=learned, 6=long-term.
std::uint64_t social_key_of(EntityId id) {
    return Rng::hash(static_cast<std::uint64_t>(id) ^
                     0x5DEECE66DULL);
}
// Doctrine triggers evaluated against the mind's own memory snapshot —
// the observable props/material/kind the engram actually recorded, never
// live entity state (pure ALife sensory honesty). Mirrors
// doctrine_trigger_hits in Cognition.cpp but reads MemorySnapshot.
bool snap_trigger_hits(const World& w,
                       const AgentMind::MemorySnapshot& s,
                       const AgentMind::DoctrineSlot& d) {
    switch (d.trig_kind) {
    case 0: {   // remembered prop threshold
        if (d.trig_prop == kNoProp) return false;
        const auto it = s.props.find(d.trig_prop);
        return it != s.props.end() &&
               it->second.type == ValType::Integer &&
               it->second.i >= d.trig_thr;
    }
    case 1:     // remembered material
        return static_cast<std::uint64_t>(s.material) == d.trig_id;
    case 2: {   // synthesized recipe: remembered material must be that synth
        if (s.material == kNoMaterial) return false;
        for (const World::SynthRecord& r : w.synthesized_)
            if (r.synth_id == d.trig_id &&
                static_cast<std::uint64_t>(r.material) ==
                    static_cast<std::uint64_t>(s.material))
                return true;
        return false;
    }
    case 3:     // remembered kind name
        return s.kind_name == d.trig_text;
    }
    return false;
}

// doctrine_bias_for re-grounded on memory: same valence/calm math as the
// legacy planner's version, but the trigger reads the snapshot and the
// starvation override reads the brain's own hunger drive.
std::int64_t snap_doctrine_bias(const World& w, const AgentMind& mind,
                                const AgentMind::MemorySnapshot& snap,
                                int verb, std::int64_t drive_hunger) {
    if (mind.doctrines.empty()) return 0;
    if (drive_hunger > 900) return 0;   // starving — dogma yields to gut
    std::int64_t bias = 0;
    for (const AgentMind::DoctrineSlot& d : mind.doctrines) {
        if (d.verb >= 0 && d.verb != verb) continue;
        if (!snap_trigger_hits(w, snap, d)) continue;
        const std::int64_t calm =
            1000 - std::min<std::int64_t>(mind.neuro.panic, 1000);
        bias += d.valence * calm / 4000;
    }
    return bias;
}

BrainSocial* social_find(BrainCognition& c, std::uint64_t key) {
    BrainSocial* empty = nullptr;
    for (BrainSocial& s : c.social) {
        if (s.key == key) return &s;
        if (s.key == 0 && !empty) empty = &s;
    }
    return empty;
}
void social_upsert(BrainCognition& c, std::uint64_t key, std::int64_t dtrust,
                   std::int64_t tick) {
    BrainSocial* s = social_find(c, key);
    if (!s) return;
    if (s->key == 0) s->key = key;
    s->trust = static_cast<std::int16_t>(clampi(
        static_cast<std::int64_t>(s->trust) + dtrust, -1000, 1000));
    s->familiarity = static_cast<std::int16_t>(clampi(
        static_cast<std::int64_t>(s->familiarity) + 40, 0, 1000));
    s->last = static_cast<std::uint64_t>(tick);
}
void brain_goals_update(World& w, BrainSlot& s,
                        const AgentMind* mind) {
    BrainVital& v = s.vital;
    BrainCognition& c = s.cog;
    const std::uint64_t tick = w.clock_.tick();
    // Pure ALife: no drive->verb translation table. Hypothalamic drives
    // reach the Basal Ganglia through the connectome's own tracts
    // (Hypothalamus->BG). Goals record ONLY interoceptive salience —
    // which homeostatic pressure is high and how strongly — never a
    // suggested action (verb stays -1, so the goal-bias term below can
    // never fire). BG verb selection remains purely neural.
    static const struct { std::uint8_t origin; std::int16_t BrainVital::*drv; } kDrv[] = {
        {1, &BrainVital::drive_hunger}, {2, &BrainVital::drive_thirst},
        {3, &BrainVital::drive_fatigue}, {4, &BrainVital::drive_temp},
        {5, &BrainVital::drive_stress},  {6, &BrainVital::drive_repro},
    };
    for (const auto& d : kDrv) {
        const std::int64_t drive = v.*(d.drv);
        if (drive <= 500) continue;
        BrainGoal* g = nullptr;
        for (BrainGoal& cand : c.goals)
            if (cand.active && cand.origin == d.origin) { g = &cand; break; }
        if (!g) {
            for (BrainGoal& cand : c.goals)
                if (!cand.active) { g = &cand; break; }
        }
        if (!g) continue;
        g->verb = -1;
        g->target = 0;
        g->priority = static_cast<std::int16_t>(
            std::min<std::int64_t>(drive, 1000));
        g->origin = d.origin;
        g->last = tick;
        g->active = true;
    }
    (void)mind;
    // deactivate stale goals (not refreshed for 4000 ticks)
    for (BrainGoal& g : c.goals)
        if (g.active && tick - g.last > 4000) g.active = false;
    // conflict: two active goals with close priority -> PFC must arbitrate
    std::int64_t best = -1, second = -1;
    for (const BrainGoal& g : c.goals) {
        if (!g.active) continue;
        if (g.priority > best) { second = best; best = g.priority; }
        else if (g.priority > second) second = g.priority;
    }
    c.conflict = (best > 0 && second > 0 && best - second < 120)
                     ? static_cast<std::int16_t>(
                           120 - (best - second)) : 0;
}

// ---- Phase 7: bounded mental rollout (spec §19) --------------------------------
// Neural simulation, NOT a world clone: inject the candidate's efference
// copy, run `depth` ticks, measure the Motor Cortex readout delta +
// Hypothalamic relief as the predicted value. Read-only w.r.t. the world;
// mutates only this agent's net (deterministic, billed as MACs).
std::int64_t brain_rollout_score(BrainSlot& s, int verb,
                                 std::int64_t depth) {
    BrainNet& net = s.net;
    const BrainModuleState& mc =
        net.modules[static_cast<int>(BrainModule::kMotorCortex)];
    const BrainModuleState& hm =
        net.modules[static_cast<int>(BrainModule::kHypothalamus)];
    std::int64_t before = 0;
    for (std::uint32_t i = 0; i < mc.count && i < kActionVerbs; ++i)
        before += net.st[mc.first + i].membrane;
    // efference copy: candidate verb injects into Motor + Cerebellum
    for (std::uint32_t i = 0; i < mc.count; ++i)
        net.inject(static_cast<std::uint8_t>(BrainModule::kMotorCortex),
                   static_cast<std::int32_t>(i), 60);
    const BrainModuleState& cb =
        net.modules[static_cast<int>(BrainModule::kCerebellum)];
    for (std::uint32_t i = 0; i < cb.count; ++i)
        net.inject(static_cast<std::uint8_t>(BrainModule::kCerebellum),
                   static_cast<std::int32_t>(i), 30);
    for (std::int64_t d = 0; d < depth; ++d) net.step();
    std::int64_t after = 0;
    for (std::uint32_t i = 0; i < mc.count && i < kActionVerbs; ++i)
        after += net.st[mc.first + i].membrane;
    // predicted value: motor engagement delta + homeostatic scalar drift
    std::int64_t hm_d = 0;
    for (std::uint32_t i = 0; i < hm.count; ++i)
        hm_d += net.st[hm.first + i].membrane;
    return (after - before) / 8 + hm_d / 64 +
           s.vital.exp_rew[verb < kActionVerbs ? verb : 0] / 8;
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
            // ALife: founders get a deterministic per-entity genome so
            // generation-0 carries selectable phenotypic variance instead
            // of 12 identical Standard connectomes.
            g = brain_founder_genome(w.world_seed, npc);
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
                  p.density_permille, p.tract_gain,
                  p.stdp_window, p.plastic_rate, p.reward_rate,
                  p.mod_sens_pm, p.rollout_budget_pm);
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
    p.pm_target = kNoEntity;
    if (const Entity* me = w.find(npc)) p.pm_pos = me->pos;
    for (EntityId tid : ctx.targets) {
        if (tid == ctx.actor) continue;
        if (p.pm_target == kNoEntity) p.pm_target = tid;
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
    int verb_idx = -1;
    for (int i = 0; i < kActionVerbs; ++i)
        if (p.a[i] == 100) { verb_idx = i; break; }
    const bool aversive = ev.failed || sigma >= 600 || charge >= 600;
    {
        const Entity* me = w.find(npc);
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

    // ---- Phase 6: reward prediction error + plasticity + social ---------------
    // RPE = actual outcome - expected outcome for THIS verb (EMA per-verb,
    // spec §13.2). The RPE sets last_rpe (dopamine target next vital_tick)
    // and gates synaptic plasticity on currently-eligible synapses.
    {
        // Pure ALife RPE: `actual` is the measured outcome label only.
        // No flat failure constants and no hardcoded moral lookups —
        // conspecific harm is aversive only when the BODY pays for it
        // (pain, lost energy, cortisol), which y[24]/charge already
        // encode through the somatic channels.
        std::int64_t actual = clampi(y[24], -1000, 1000);
        if (verb_idx >= 0) {
            std::int16_t& exp = s.vital.exp_rew[verb_idx];
            const std::int64_t rpe = actual - exp;
            s.vital.last_rpe = static_cast<std::int16_t>(
                clampi(rpe, -2000, 2000));
            // EMA update: exp += rpe/8 (defined division, deterministic)
            exp = static_cast<std::int16_t>(clampi(
                static_cast<std::int64_t>(exp) + rpe / 8, -1000, 1000));
            // dopamine gate for all plasticity this post
            s.net.stdp_gate = static_cast<std::int32_t>(clampi(
                200 + s.vital.mod[kBModDopa] * 2, 64, 2200));
            brain_reward_plasticity(s, rpe, aversive);
        }
        // failure / high-surprise -> next decide triggers bounded planning
        if (aversive || sigma >= 700)
            s.cog.plan_trigger = 1;
        else if (sigma < 300 && !ev.failed)
            s.cog.plan_trigger = 0;

        // ---- Phase 8: social cognition -------------------------------------
        // Own experience with a conspecific target: familiarity + trust.
        // Communicated claims update trust at half rate (hearsay discount).
        if (p.pm_target != kNoEntity) {
            const Entity* tgt = w.find(p.pm_target);
            if (tgt && tgt->alive && w.find_mind(p.pm_target)) {
                const std::uint64_t key = social_key_of(p.pm_target);
                // outcome valence: reward-positive -> trust+, aversive -> trust-
                std::int64_t dv = 0;
                if (!ev.failed) {
                    if (actual > 200) dv = 10;
                    else if (actual < -200) dv = -12;
                    else if (sigma < 300) dv = 4;
                } else dv = -15;
                if (verb_idx >= 0 &&
                    std::strcmp(sensor_verb_table()[verb_idx],
                                "communicate") == 0) {
                    // communication reliability ledger
                    s.cog.comm_rel = static_cast<std::int16_t>(clampi(
                        static_cast<std::int64_t>(s.cog.comm_rel) +
                            (sigma < 500 && !ev.failed ? 12 : -15),
                        -1000, 1000));
                    dv /= 2;   // hearsay discount on trust move
                }
                social_upsert(s.cog, key, dv, w.clock_.tick());
            }
        }
        // language production success -> strengthen language tracts a touch
        // (vocabulary/sequence associations, spec §20): Assoc->Lang and
        // Lang->MotorCortex plastic synapses get a small LTP bump.
        if (verb_idx >= 0 && !ev.failed &&
            std::strcmp(sensor_verb_table()[verb_idx],
                        "communicate") == 0) {
            BrainNet& net = s.net;
            for (std::uint32_t k = 0; k < net.syn_count; ++k) {
                if (!(net.syn[k].flags & 1)) continue;
                // plastic synapses targeting Language = lexical integration
                const std::uint8_t tm =
                    net.module_of[net.syn[k].target];
                if (tm == static_cast<std::uint8_t>(
                        BrainModule::kLanguage)) {
                    std::int32_t d = net.dw[k] + 1;
                    net.dw[k] = static_cast<std::int8_t>(std::min(
                        BrainNet::kDwMax, d));
                }
            }
            // Lang->MC: source in Language module — find pre via CSR scan is
            // costly; approximate by scanning rows of language neurons.
            const BrainModuleState& lm =
                net.modules[static_cast<int>(BrainModule::kLanguage)];
            const std::uint8_t mc_idx =
                static_cast<std::uint8_t>(BrainModule::kMotorCortex);
            for (std::uint32_t u = lm.first; u < lm.first + lm.count; ++u) {
                for (std::uint32_t k = net.row_ptr[u];
                     k < net.row_ptr[u + 1]; ++k) {
                    if (!(net.syn[k].flags & 1)) continue;
                    if (net.module_of[net.syn[k].target] != mc_idx)
                        continue;
                    std::int32_t d = net.dw[k] + 1;
                    net.dw[k] = static_cast<std::int8_t>(std::min(
                        BrainNet::kDwMax, d));
                }
            }
        }
    }

    // ---- outcome teaching signal -------------------------------------------
    // Inject the homeostatic reward (label ch24) as a modulatory current
    // into Hypothalamus (drive) and Basal Ganglia (action valuation), then
    // let the network settle for a few ticks under the dopamine gate set
    // above — STDP + reward eligibility do the actual weight updates.
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
    // Pure ALife: no injected novelty dopamine — a fresh transform earns
    // whatever the outcome label says it earned.
    {
        int verb = -1;
        for (int i = 0; i < kActionVerbs; ++i)
            if (p.a[i] == 100) { verb = i; break; }
        if (verb >= 0 && invent_verb(verb) && p.pm_a != kNoMaterial)
            invent_mark(s, invent_key(verb, p.pm_a, p.pm_b));
    }
}

// ================= decision =====================================================

static const bool brain_debug =
    std::getenv("SIMLAB_BRAIN_DEBUG") != nullptr;
static EntityId debug_npc = [] {
    const char* e = std::getenv("SIMLAB_BRAIN_DEBUG_NPC");
    return e ? static_cast<EntityId>(std::strtoull(e, nullptr, 10))
             : static_cast<EntityId>(0);
}();
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
    // Phase 7: hierarchical goal management + conflict detection (spec §17)
    brain_goals_update(w, s, mind);
    // dopamine gate for this burst's STDP (spec §13.2)
    net.stdp_gate = static_cast<std::int32_t>(clampi(
        200 + s.vital.mod[kBModDopa] * 2, 64, 2200));

    // ---- Phase 9: age-dependent development (spec §23) ------------------------
    // Juveniles (< adult_age) are hyper-plastic but plan less; adults keep
    // baseline plasticity with full rollout capacity (motor refinement).
    bool juvenile = false;
    {
        static thread_local SeqPidCache ac_;
        SeqPid ap(ac_, w.props);
        const PropId age_p = ap("age"), adult_p = ap("adult_age");
        const std::int64_t age =
            age_p != kNoProp ? w.prop_int(*me, age_p, 0) : 0;
        const std::int64_t adult =
            adult_p != kNoProp ? w.prop_int(*me, adult_p, 200) : 200;
        juvenile = adult > 0 && age >= 0 && age < adult;
    }
    net.plastic_scale_pm = juvenile ? 1300 : 1000;

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
    // PFC functional subdivisions (spec §4.7): dlPFC = first third
    // (working memory / rule maintenance), vmPFC = middle (value
    // integration), OFC = last third (expected outcome / switching).
    std::int32_t pfc_dlp = 0, pfc_vmp = 0, pfc_ofc = 0;
    {
        const BrainModuleState& pm =
            net.modules[static_cast<int>(BrainModule::kPrefrontal)];
        const std::uint32_t third = std::max<std::uint32_t>(1, pm.count / 3);
        for (std::uint32_t i = 0; i < pm.count; ++i) {
            const std::int32_t m = net.st[pm.first + i].membrane;
            if (i < third) pfc_dlp += m;
            else if (i < 2 * third) pfc_vmp += m;
            else pfc_ofc += m;
        }
        pfc_dlp /= static_cast<std::int32_t>(third);
        pfc_vmp /= static_cast<std::int32_t>(third);
        pfc_ofc /= static_cast<std::int32_t>(std::max<std::uint32_t>(
            1, pm.count - 2 * third));
    }
    // Language Network readout: mean of last 8 Language neurons =
    // communicative intent (spec §4.11/§20).
    std::int32_t lang_intent = 0;
    {
        const BrainModuleState& lm =
            net.modules[static_cast<int>(BrainModule::kLanguage)];
        const std::uint32_t n =
            std::min<std::uint32_t>(8, lm.count);
        if (n) {
            for (std::uint32_t i = lm.count - n; i < lm.count; ++i)
                lang_intent += net.st[lm.first + i].membrane;
            lang_intent /= static_cast<std::int32_t>(n);
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
    // Memory recall bandwidth: the live sensor has 4 entity channels, but
    // action candidacy may recall deeper — a brain can only act on what it
    // remembers, and 4 slots loses most of the engram store. kMemNear is a
    // recall-depth parameter, not a world-truth peek: every entry is the
    // agent's own MemorySnapshot.
    constexpr int kMemNear = 12;
    struct NearEnt { std::int64_t d2; EntityId id; Vec2 pos;
                     std::int64_t memsal; std::int64_t key; };
    NearEnt near[kMemNear];
    int nnear = 0;
    for (const auto& [kid, snap] : mind->memory) {
        if (kid == npc) continue;
        const std::int64_t dx = snap.pos.x - me->pos.x;
        const std::int64_t dy = snap.pos.y - me->pos.y;
        const std::int64_t d2 = dx * dx + dy * dy;
        // Affective recall: rank by engram emotional charge (encoded at
        // perception time from the observer's own needs — a drink seen
        // while desperate IS a flashbulb memory) blended with proximity.
        // Neutral memories fall back to nearest-first; trauma-signed
        // engrams sink. This is the memory system's own retrieval order,
        // not a world-truth lookup.
        const std::int64_t chg =
            std::clamp<std::int64_t>(snap.engram.emotional_charge,
                                     -4000, 4000);
        const std::int64_t key = chg * 32 - d2 / 64;
        if (nnear == kMemNear && key <= near[kMemNear - 1].key) continue;
        int p = nnear < kMemNear ? nnear++ : kMemNear - 1;
        while (p > 0 && near[p - 1].key < key) { near[p] = near[p - 1]; --p; }
        near[p] = {d2, kid, snap.pos, chg, key};
    }

    // ---- candidate generation ---------------------------------------------------
    constexpr int kC = 14;
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
        // consume with no target is a guaranteed no-op (the law iterates
        // ctx.targets) — never worth a candidacy slot
        if (v != 0) consider(vlogit[v] / 16, v, -1);
        for (int t = 0; t < nnear; ++t) {
            // live-percept channels (4) carry cortical saliency; deeper
            // recall ranks by the engram's own emotional charge — the
            // amygdala-modulated memory trace, not a world-truth read.
            const std::int32_t msal = t < kEntitySlots
                ? entsal[t] / 16
                : static_cast<std::int32_t>(
                      std::clamp<std::int64_t>(near[t].memsal, -4000, 4000)
                      / 64);
            consider(vlogit[v] / 16 + msal, v, t);
        }
    }

    // Pure ALife: candidate generation ends here. Every candidate is a
    // (verb, remembered-entity) pair scored by neural readout — no
    // synthetic destinations, no force() overrides, no drive->target
    // steering code. Survival, roaming and social approach must emerge
    // from the connectome dynamics + memory + plasticity.
    if (ncand == 0) return out;

    // ---- score candidates: neural base + mind-level biases ----------------------
    const NeuroState& ns = mind->neuro;

    std::int64_t best_total = INT64_MIN;
    int best_c = 0;
    std::int64_t totals[kC] = {};
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
        // tribal doctrine bias — evaluated against the mind's own memory
        // snapshot (remembered props/material/kind), never live entity
        // state; doctrine-free agents: cheap no-op.
        if (cd.tgt >= 0 && cd.tgt < nnear) {
            const auto tit = mind->memory.find(near[cd.tgt].id);
            if (tit != mind->memory.end())
                total += snap_doctrine_bias(
                    w, *mind, tit->second, cd.verb,
                    s.vital.drive_hunger);
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
        // ---- Phase 7: goal relevance + PFC subdivision integration ----------
        // Active goals bias their suggested verb (goals never auto-act).
        for (const BrainGoal& g : s.cog.goals)
            if (g.active && g.verb == cd.verb)
                total += g.priority / 8;
        // dlPFC working-memory maintenance: confident recent action repeats
        if (s.vital.wm_conf > 500 && cd.verb == s.vital.wm_verb)
            total += 24;
        // vmPFC value integration: expected outcome for this verb
        total += s.vital.exp_rew[cd.verb] / 16;
        // OFC behavioral switching: high |OFC| + goal conflict -> the PFC
        // readout itself must break the tie (no bonus injected here).
        // ---- Phase 8: social cognition -------------------------------------
        // trust/familiarity bias on social verbs toward remembered
        // conspecifics; comm intent from Language readout.
        if (cd.tgt >= 0 && cd.tgt < nnear) {
            const EntityId tid = near[cd.tgt].id;
            // conspecific = remembered entity whose kind is a species —
            // judged from the memory snapshot, not live world state.
            const auto tit2 = mind->memory.find(tid);
            const bool conspecific =
                tit2 != mind->memory.end() &&
                w.species.find_name(tit2->second.kind_name) != nullptr;
            if (tid != npc && conspecific) {
                const BrainSocial* soc = nullptr;
                const std::uint64_t key = social_key_of(tid);
                for (const BrainSocial& x : s.cog.social)
                    if (x.key == key) { soc = &x; break; }
                // trust is LEARNED ONLY (own experience + observation);
                // no relationship-graph fallback — the brain trusts what
                // it has lived, not what the world asserts.
                const std::int64_t trust = soc ? soc->trust : 0;
                const char* vn = sensor_verb_table()[cd.verb];
                const bool social_v =
                    std::strcmp(vn, "share") == 0 ||
                    std::strcmp(vn, "communicate") == 0 ||
                    std::strcmp(vn, "trade") == 0 ||
                    std::strcmp(vn, "strike") == 0 ||
                    std::strcmp(vn, "steal") == 0;
                if (social_v) {
                    // trusted partners: pro-social verbs up, harm down
                    if (trust > 0 &&
                        (std::strcmp(vn, "share") == 0 ||
                         std::strcmp(vn, "communicate") == 0 ||
                         std::strcmp(vn, "trade") == 0))
                        total += trust / 12;
                    if (trust < 0 &&
                        (std::strcmp(vn, "strike") == 0 ||
                         std::strcmp(vn, "steal") == 0))
                        total += trust / 12;   // trust<0 -> negative
                    if (trust < 0 &&
                        (std::strcmp(vn, "share") == 0 ||
                         std::strcmp(vn, "communicate") == 0))
                        total -= trust / 12;   // distrusted -> avoid help
                    // heard-say suspicion: low comm_rel discounts incoming
                    // social verbs (source-tagged reliability, spec §21)
                    if (s.cog.comm_rel < -200 &&
                        std::strcmp(vn, "communicate") == 0)
                        total -= 64;
                }
                // communicative intent -> communicate bonus
                if (lang_intent > 200 &&
                    std::strcmp(vn, "communicate") == 0)
                    total += lang_intent / 16;
            }
        }
        // Pure ALife: no homeostatic scoring hacks. `consume`/`move`
        // toward remembered entities compete on neural logits + learned
        // biases alone — hunger/thirst reach the action selection purely
        // through Hypothalamus->BasalGanglia dynamics and exp_rew.
        totals[c] = total;
        if (total > best_total) { best_total = total; best_c = c; }
    }

    // ---- Phase 7: bounded mental rollout (spec §19) ---------------------------
    // Triggered by goal conflict, post failure/surprise, or high uncertainty.
    // Simple reflexes (panic/rage override) bypass planning entirely.
    // Read-only w.r.t. the world; mutates only this agent's net.
    {
        const bool reflex = ns.panic > 700 || ns.rage > 700;
        const bool uncertain = s.vital.mod[kBModAchi] < 350;
        // Phase 9: genome rollout budget (permille) gates planning
        // frequency deterministically per (world, npc, tick); juveniles
        // get 60% of the budget (spec §23).
        const std::int64_t rb =
            net.rollout_budget_pm * (juvenile ? 600 : 1000) / 1000;
        const bool afford = rb >= 1024 ||
            (static_cast<std::int64_t>(Rng::hash(
                 w.world_seed ^ (npc * 0x9E3779B97F4A7C15ull) ^
                 (w.clock_.tick() * 0xBF58476D1CE4E5B9ull))) % 1024) < rb;
        const bool want_plan =
            !reflex && ncand > 1 && afford &&
            (s.cog.plan_trigger || s.cog.conflict > 0 ||
             s.vital.last_sigma > 750 || uncertain);
        if (want_plan) {
            // configurable depth (mind aptitude) & candidate budget (2)
            const std::int64_t horizon =
                mind_params(w, *me).plan_horizon;
            const std::int64_t depth = std::max<std::int64_t>(
                1, std::min<std::int64_t>(4, horizon));
            const int budget = std::min(ncand, 2);
            std::int64_t rscore[kC] = {};
            for (int c = 0; c < budget; ++c) {
                rscore[c] = brain_rollout_score(
                    s, cands[c].verb, depth);
                s.cog.plan_ops +=
                    static_cast<std::uint64_t>(net.neuron_count) * depth;
            }
            s.cog.plan_trigger = 0;   // consumed
            // PFC arbitration: if a rollout predicts a clearly better
            // outcome for a non-greedy candidate, it overrides BG habit.
            // (only when the greedy candidate itself was simulated)
            if (best_c < budget) {
                const std::int64_t ref = rscore[best_c];
                for (int c = 0; c < budget; ++c) {
                    if (c == best_c) continue;
                    if (rscore[c] - ref > 400)
                        totals[c] += 160;   // bounded override bonus
                }
            }
            // re-pick argmax over adjusted totals
            best_total = INT64_MIN;
            for (int c = 0; c < ncand; ++c)
                if (totals[c] > best_total) {
                    best_total = totals[c]; best_c = c;
                }
        }
    }

    // ---- metabolism: bill neural work -------------------------------------------
    {
        const std::int64_t macs =
            static_cast<std::int64_t>(net.neuron_count) * kTicks * 2 +
            static_cast<std::int64_t>(net.syn_count) / 8;
        s.mac_pending += macs;
    }

    const Cand& chosen = cands[best_c];
    if (brain_debug && npc == debug_npc) {
        fprintf(stderr, "[braindbg] npc=%llu ncand=%d chosen v=%d t=%d tot=%lld\n",
                (unsigned long long)npc, ncand, chosen.verb, chosen.tgt,
                (long long)best_total);
        for (int c = 0; c < ncand; ++c)
            fprintf(stderr, "  cand%d v=%d t=%d base=%d tot=%lld\n", c,
                    cands[c].verb, cands[c].tgt, cands[c].score,
                    (long long)totals[c]);
    }
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

// ================= observational learning + social cognition ==================

void brain_observe(World& w, EntityId observer, EntityId actor, int verb,
                   std::int64_t outcome_valence, bool via_comm,
                   EntityId /*target*/) {
    if (w.speculative || !w.brains_) return;
    auto it = w.brains_->slots.find(observer);
    if (it == w.brains_->slots.end()) return;   // no brain -> no neural learn
    BrainSlot& s = it->second;
    BrainNet& net = s.net;
    if (verb < 0 || verb >= kActionVerbs) return;

    // Phase 8 social bookkeeping: familiarity always; trust moves at full
    // rate for direct observation, half rate for communicated hearsay.
    {
        const std::uint64_t key = social_key_of(actor);
        std::int64_t dv = 0;
        if (outcome_valence > 200) dv = via_comm ? 5 : 10;
        else if (outcome_valence < -200) dv = via_comm ? -6 : -12;
        else dv = via_comm ? 1 : 3;
        social_upsert(s.cog, key, dv, w.clock_.tick());
        // Pure ALife: no scripted "violence is bad" rule. Observers learn
        // from the observed outcome_valence like any other event —
        // retaliation/avoidance must emerge from their own RPE, fear
        // conditioning and social valuation, not a hardcoded penalty.
        if (via_comm) {
            // source-tagged reliability: the observer tracks how much to
            // trust THIS source's claims (spec §21 hearsay discount)
            s.cog.comm_rel = static_cast<std::int16_t>(clampi(
                static_cast<std::int64_t>(s.cog.comm_rel) +
                    (outcome_valence >= 0 ? 6 : -10), -1000, 1000));
        }
    }

    // Phase 6 observational learning (spec §13.4): inject the observed
    // (actor-context, verb) pattern into Hippocampus + Association under a
    // dopamine gate scaled by valence — associations form, but the small
    // current means observation alone never grants skill mastery.
    const BrainModuleState& hip =
        net.modules[static_cast<int>(BrainModule::kHippocampus)];
    const BrainModuleState& asc =
        net.modules[static_cast<int>(BrainModule::kAssociation)];
    const std::uint64_t h = Rng::hash(
        w.world_seed ^ (actor * 0x9E3779B97F4A7C15ull) ^
        (static_cast<std::uint64_t>(verb) << 32));
    const std::int32_t amp = static_cast<std::int32_t>(clampi(
        40 + outcome_valence / 8, 20, 120));
    for (std::uint32_t i = 0; i < hip.count && i < 48; ++i)
        net.inject(static_cast<std::uint8_t>(BrainModule::kHippocampus),
                   static_cast<std::int32_t>(i),
                   amp * static_cast<std::int32_t>(
                       ((h >> (i % 24)) & 1) ? 1 : -1) / 4);
    for (std::uint32_t i = 0; i < asc.count && i < 48; ++i)
        net.inject(static_cast<std::uint8_t>(BrainModule::kAssociation),
                   static_cast<std::int32_t>(i),
                   amp * static_cast<std::int32_t>(
                       ((h >> ((i + 13) % 24)) & 1) ? 1 : -1) / 4);
    // dopamine bump: salient observations are remembered better
    s.vital.mod[kBModDopa] = static_cast<std::int16_t>(clampi(
        static_cast<std::int64_t>(s.vital.mod[kBModDopa]) +
            (outcome_valence >= 0 ? 60 : 20), 0, 1000));
    net.stdp_gate = static_cast<std::int32_t>(clampi(
        200 + s.vital.mod[kBModDopa] * 2, 64, 2200));
    for (int k = 0; k < 2; ++k) net.step();
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
        // Phase 6: per-verb expected outcomes + last RPE
        for (std::int16_t e : v.exp_rew)
            mix(static_cast<std::uint16_t>(e));
        mix(static_cast<std::uint16_t>(v.last_rpe));
        // Phase 6: learned synaptic deltas ARE state (persist + hash)
        for (std::int8_t d : n.dw) mix(static_cast<std::uint8_t>(d));
        // Phase 7-8 cognition: goals, social memory, comm reliability
        const BrainCognition& cg = s.cog;
        for (const BrainGoal& g : cg.goals) {
            mix(static_cast<std::uint32_t>(g.verb));
            mix(static_cast<std::uint64_t>(g.target));
            mix(static_cast<std::uint16_t>(g.priority));
            mix(g.origin); mix(g.last); mix(g.active ? 1 : 0);
        }
        for (const BrainSocial& so : cg.social) {
            mix(so.key); mix(static_cast<std::uint16_t>(so.trust));
            mix(static_cast<std::uint16_t>(so.familiarity)); mix(so.last);
        }
        mix(static_cast<std::uint16_t>(cg.comm_rel));
        mix(static_cast<std::uint16_t>(cg.conflict));
        mix(static_cast<std::uint16_t>(cg.plan_trigger));
        mix(cg.plan_ops);
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
