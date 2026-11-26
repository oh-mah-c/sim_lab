#include "simlab/BrainNet.hpp"
#include "simlab/Rng.hpp"
#include <algorithm>
#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace simlab {

using M = BrainModule;
static constexpr std::uint8_t BM(BrainModule m) {
    return static_cast<std::uint8_t>(m);
}

// Data-driven tract table (spec §7). src/dst are BrainModule indices.
// gain: base |weight| 0..255; density: permille of dst pop connected per
// pre neuron; delay: base ticks; inh_permille: fraction forced inhibitory.
const BrainTract kBrainTracts[kBrainTractCount] = {
    // sensory / attention loops
    {BM(M::kSensoryCortex), BM(M::kThalamus),        180, 400, 1, 50},
    {BM(M::kThalamus),       BM(M::kSensoryCortex),  160, 300, 1, 80},
    {BM(M::kThalamus),       BM(M::kPrefrontal),     200, 500, 2, 30},
    {BM(M::kThalamus),       BM(M::kAssociation),   150, 300, 2, 60},
    // fast defensive route + neuromodulatory loop
    {BM(M::kSensoryCortex), BM(M::kAmygdala),       220, 500, 1, 40},
    {BM(M::kAmygdala),      BM(M::kHypothalamus),   200, 400, 1, 60},
    {BM(M::kAmygdala),      BM(M::kPrefrontal),     150, 300, 2, 70},
    {BM(M::kAmygdala),      BM(M::kHippocampus),    140, 250, 2, 80},
    // homeostasis -> motivation -> action
    {BM(M::kHypothalamus),  BM(M::kBasalGanglia),   190, 500, 1, 40},
    {BM(M::kHypothalamus),  BM(M::kBrainstem),      160, 300, 1, 70},
    {BM(M::kBrainstem),     BM(M::kHypothalamus),   140, 300, 1, 60},
    // executive / action selection / motor
    {BM(M::kPrefrontal),    BM(M::kBasalGanglia),   200, 500, 2, 30},
    {BM(M::kBasalGanglia),  BM(M::kMotorCortex),    220, 600, 1, 30},
    {BM(M::kPrefrontal),    BM(M::kMotorCortex),    160, 300, 2, 60},
    {BM(M::kCerebellum),    BM(M::kMotorCortex),    200, 500, 1, 40},
    {BM(M::kMotorCortex),   BM(M::kCerebellum),     180, 400, 2, 70},
    // memory loops
    {BM(M::kHippocampus),   BM(M::kPrefrontal),     180, 400, 3, 50},
    {BM(M::kPrefrontal),    BM(M::kHippocampus),    170, 400, 2, 50},
    {BM(M::kSensoryCortex), BM(M::kHippocampus),    150, 300, 3, 70},
    {BM(M::kHippocampus),   BM(M::kAssociation),    140, 300, 3, 60},
    // association / language
    {BM(M::kSensoryCortex), BM(M::kAssociation),    160, 400, 2, 50},
    {BM(M::kAssociation),   BM(M::kPrefrontal),     180, 400, 2, 50},
    {BM(M::kAssociation),   BM(M::kLanguage),       150, 300, 3, 70},
    {BM(M::kLanguage),      BM(M::kMotorCortex),    170, 300, 2, 60},
};

// Multi-rate scheduler periods (neural ticks). Brainstem/sensory/motor run
// every tick; association/PFC/amygdala/hypothalamus/cerebellum at 2;
// hippocampus at 4; language at 8 — speculation/decision layers tick slower.
static const std::uint32_t kModulePeriod[kBrainModuleCount] = {
    1, 1, 2, 1, 1, 2, 4, 2, 1, 1, 2, 8
};

// ---- deterministic hashing ----------------------------------------------------
static std::uint64_t nhash(std::uint64_t a, std::uint64_t b,
                           std::uint64_t c) {
    std::uint64_t h = a ^ (b * 0x9E3779B97F4A7C15ull) ^
                      (c * 0xBF58476D1CE4E5B9ull);
    h ^= h >> 30; h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 27; h *= 0x94D049BB133111EBull;
    h ^= h >> 31;
    return h;
}

static std::int32_t clamp16(std::int64_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return static_cast<std::int32_t>(v);
}
static std::int32_t sat_add(std::int32_t a, std::int32_t b) {
    const std::int64_t s = static_cast<std::int64_t>(a) + b;
    if (s > 60000) return 60000;
    if (s < -60000) return -60000;
    return static_cast<std::int32_t>(s);
}

// jitter helper: deterministic in [-spread, +spread]
static std::int32_t njit(std::uint64_t seed, std::uint64_t a,
                         std::uint64_t b, std::int32_t spread) {
    if (spread <= 0) return 0;
    return static_cast<std::int32_t>(nhash(seed, a, b) %
                                     (2ull * spread + 1)) - spread;
}

void BrainNet::init(std::uint64_t world_seed, std::uint64_t entity,
                    const std::array<std::uint16_t, kBrainModuleCount>& pops,
                    std::int16_t threshold, std::int16_t reset,
                    std::uint8_t leak_shift, std::uint8_t refractory,
                    std::uint8_t ei_permille, std::uint16_t density_permille,
                    const std::array<std::int16_t, kBrainTractCount>& tract_gain,
                    std::uint8_t stdp_window, std::int32_t plastic_rate,
                    std::int32_t reward_rate,
                    std::int32_t mod_sens_pm, std::int32_t rollout_budget_pm) {
    const std::uint64_t seed =
        world_seed ^ (entity * 0xD1B54A32D192ED03ull);
    this->stdp_window = stdp_window;
    this->plastic_rate = plastic_rate;
    this->reward_rate = reward_rate;
    this->mod_sens_pm = mod_sens_pm;
    this->rollout_budget_pm = rollout_budget_pm;
    this->stdp_gate = 512;

    // ---- module layout -------------------------------------------------------
    neuron_count = 0;
    std::uint32_t mask_words_total = 0;
    for (int m = 0; m < kBrainModuleCount; ++m) {
        BrainModuleState& ms = modules[m];
        ms.first = neuron_count;
        ms.count = pops[m];
        ms.period = kModulePeriod[m];
        ms.phase = static_cast<std::uint32_t>(m) % ms.period;
        ms.last_run = 0; ms.event_accum = 0; ms.scalar = 0;
        ms.spike_words = (ms.count + 63) / 64;
        ms.spike_off = mask_words_total;
        mask_words_total += ms.spike_words;
        neuron_count += ms.count;
    }
    spike_masks.assign(mask_words_total, 0);
    gen.resize(neuron_count);
    st.assign(neuron_count, BrainNeuronState{});
    module_of.resize(neuron_count);
    last_fire.assign(neuron_count, 0);
    kick_hist.assign(neuron_count, {~0ull, ~0ull, ~0ull, ~0ull});
    recent_pos.assign(neuron_count, 0);

    // ---- neuron parameters: genome baseline + per-neuron hash jitter ---------
    for (int m = 0; m < kBrainModuleCount; ++m) {
        const BrainModuleState& ms = modules[m];
        for (std::uint32_t i = 0; i < ms.count; ++i) {
            const std::uint32_t u = ms.first + i;
            module_of[u] = static_cast<std::uint8_t>(m);
            BrainNeuronGen& g = gen[u];
            g.threshold = static_cast<std::int16_t>(clamp16(
                threshold + njit(seed, u, 1, threshold / 8 + 1)));
            g.reset = static_cast<std::int16_t>(clamp16(
                reset + njit(seed, u, 2, 60)));
            const std::int32_t ls = leak_shift + njit(seed, u, 3, 1);
            g.leak_shift = static_cast<std::uint8_t>(
                std::max(3, std::min(8, ls)));
            const std::int32_t rf = refractory + njit(seed, u, 4, 1);
            g.refractory = static_cast<std::uint8_t>(
                std::max(1, std::min(6, rf)));
            g.plasticity = 0;
            // inhibitory fraction -> negative outgoing weight sign
            g.sign = (nhash(seed, u, 5) % 1000 < ei_permille) ? -1 : 1;
        }
    }

    // ---- synapse construction -------------------------------------------------
    // Pass 1: count outdegree per pre neuron.
    std::vector<std::uint32_t> outdeg(neuron_count, 0);
    auto bump = [&](std::uint32_t pre) { ++outdeg[pre]; };

    // (a) local recurrence inside each module: each pre -> k_local posts,
    //     weight ~ small, sign = pre sign.
    const std::uint32_t local_k_base =
        static_cast<std::uint32_t>(density_permille) / 8;  // ~0.125%/neuron
    for (int m = 0; m < kBrainModuleCount; ++m) {
        const BrainModuleState& ms = modules[m];
        if (ms.count < 2) continue;
        const std::uint32_t k_local = std::max<std::uint32_t>(
            1, std::min<std::uint32_t>(6,
                local_k_base * ms.count / 1000 + 1));
        for (std::uint32_t i = 0; i < ms.count; ++i)
            for (std::uint32_t j = 0; j < k_local; ++j) bump(ms.first + i);
    }
    // (b) inter-module tracts.
    for (std::size_t t = 0; t < kBrainTractCount; ++t) {
        const BrainTract& tr = kBrainTracts[t];
        const BrainModuleState& src = modules[tr.src];
        const BrainModuleState& dst = modules[tr.dst];
        if (!src.count || !dst.count) continue;
        const std::int32_t gain =
            tract_gain[t] > 0 ? tract_gain[t] : tr.gain;
        for (std::uint32_t i = 0; i < src.count; ++i) {
            const std::uint32_t k =
                static_cast<std::uint32_t>(
                    static_cast<std::uint64_t>(tr.density) * dst.count /
                    1000);
            for (std::uint32_t j = 0; j < k; ++j) bump(src.first + i);
        }
        (void)gain;
    }

    // CSR prefix sum.
    row_ptr.assign(neuron_count + 1, 0);
    for (std::uint32_t u = 0; u < neuron_count; ++u)
        row_ptr[u + 1] = row_ptr[u] + outdeg[u];
    syn_count = row_ptr[neuron_count];
    syn.resize(syn_count);
    dw.assign(syn_count, 0);
    std::vector<std::uint32_t> cursor(row_ptr.begin(), row_ptr.end() - 1);

    // Plasticity (spec §13): local recurrence is plastic everywhere except
    // the hard-wired autonomic cores (Brainstem, Hypothalamus); tract
    // synapses are plastic except the two autonomic brainstem loops
    // (indices 7,8 in kBrainTracts). flags bit0 = STDP-eligible.
    auto emit = [&](std::uint32_t pre, std::uint32_t post,
                    std::int32_t weight, std::uint8_t delay,
                    bool plastic) {
        const std::uint32_t p = cursor[pre]++;
        syn[p].target = post;
        syn[p].weight = static_cast<std::int16_t>(
            clamp16(weight));
        syn[p].delay = delay;
        syn[p].flags = plastic ? 1 : 0;
    };

    // (a) local recurrence
    for (int m = 0; m < kBrainModuleCount; ++m) {
        const BrainModuleState& ms = modules[m];
        if (ms.count < 2) continue;
        const std::uint32_t k_local = std::max<std::uint32_t>(
            1, std::min<std::uint32_t>(6,
                local_k_base * ms.count / 1000 + 1));
        for (std::uint32_t i = 0; i < ms.count; ++i) {
            const std::uint32_t pre = ms.first + i;
            for (std::uint32_t j = 0; j < k_local; ++j) {
                // hash-picked post within module, avoid self. Bounded:
                // a retry with the same salt would loop forever, so the
                // fallback is a deterministic +1 shift instead.
                std::uint32_t off = static_cast<std::uint32_t>(
                    nhash(seed, pre, 0x100 + j * 977 + i) % ms.count);
                if (off == i) off = (off + 1) % ms.count;
                const std::uint32_t post = ms.first + off;
                const std::int32_t w =
                    (40 + njit(seed, pre, 0x200 + j, 40)) * gen[pre].sign;
                emit(pre, post, w, 1, m != 0 && m != 2);
            }
        }
    }
    // (b) tracts
    for (std::size_t t = 0; t < kBrainTractCount; ++t) {
        const BrainTract& tr = kBrainTracts[t];
        const BrainModuleState& src = modules[tr.src];
        const BrainModuleState& dst = modules[tr.dst];
        if (!src.count || !dst.count) continue;
        const std::int32_t gain =
            tract_gain[t] > 0 ? tract_gain[t] : tr.gain;
        for (std::uint32_t i = 0; i < src.count; ++i) {
            const std::uint32_t pre = src.first + i;
            const std::uint32_t k =
                static_cast<std::uint32_t>(
                    static_cast<std::uint64_t>(tr.density) * dst.count /
                    1000);
            for (std::uint32_t j = 0; j < k; ++j) {
                const std::uint32_t off = static_cast<std::uint32_t>(
                    nhash(seed, pre, 0x3000 + t * 131 + j * 17) % dst.count);
                const std::uint32_t post = dst.first + off;
                // sign: pre neuron's type, flipped for the tract's
                // inhibitory fraction
                std::int32_t sgn = gen[pre].sign;
                if (nhash(seed, pre, 0x4000 + t * 733 + j) % 1000 <
                    tr.inh_permille)
                    sgn = -sgn;
                const std::int32_t w =
                    gain * sgn + njit(seed, pre, 0x5000 + t * 31 + j,
                                      gain / 4 + 1);
                const std::uint8_t d = static_cast<std::uint8_t>(
                    tr.delay + njit(seed, pre, 0x6000 + t, 1) / 2 +
                    (njit(seed, pre, 0x6100 + t, 1) > 0 ? 1 : 0));
                emit(pre, post, w, std::max<std::uint8_t>(1, d),
                     t != 7 && t != 8);
            }
        }
    }

    max_delay = 0;
    for (const BrainSynapse& s : syn)
        max_delay = std::max<std::uint32_t>(max_delay, s.delay);
    if (max_delay >= BrainNet::kRingLen) max_delay = BrainNet::kRingLen - 1;
    for (auto& r : ring) r.clear();
    tick = 0; pending_mask = 0; dropped_kicks = 0; spikes = 0;
}

void BrainNet::inject(std::uint8_t mod, std::int32_t neuron_in_module,
                      std::int32_t current) {
    if (mod >= kBrainModuleCount) return;
    const BrainModuleState& ms = modules[mod];
    if (neuron_in_module < 0 ||
        static_cast<std::uint32_t>(neuron_in_module) >= ms.count) return;
    BrainNeuronState& s = st[ms.first + neuron_in_module];
    s.syn_input = sat_add(s.syn_input, current);
}

// ---- AVX2 LIF module update (Phase 11) ---------------------------------------
// Bit-identical to the scalar loop below: same trunc-toward-zero leak
// division, same clamp16, same refractory/reset semantics. Modules are
// genome-bounded (<= kAvxMaxNeurons); larger ones fall back to scalar.
#if defined(__AVX2__)
static constexpr std::uint32_t kAvxMaxNeurons = 256;

// trunc division by 2^shift, matching C++ integer division exactly:
// m / 2^s == sign(m) * (|m| >> s) for s in [0,31].
static inline __m256i avx_trunc_shr(__m256i m, __m256i sh) {
    const __m256i sign = _mm256_srai_epi32(m, 31);       // 0 or -1
    const __m256i a = _mm256_sub_epi32(                   // |m|
        _mm256_xor_si256(m, sign), sign);
    const __m256i q = _mm256_srlv_epi32(a, sh);            // |m| >> sh
    return _mm256_sub_epi32(                              // sign(m) * q
        _mm256_xor_si256(q, sign), sign);
}

void BrainNet::lif_module_avx2(BrainModuleState& ms) {
    // Stage SoA (module counts are small; stack buffers, no heap).
    alignas(32) std::int32_t memb[kAvxMaxNeurons];
    alignas(32) std::int32_t inp[kAvxMaxNeurons];
    alignas(32) std::int32_t refr[kAvxMaxNeurons];
    alignas(32) std::int32_t thr[kAvxMaxNeurons];
    alignas(32) std::int32_t rst[kAvxMaxNeurons];
    alignas(32) std::int32_t lsh[kAvxMaxNeurons];
    alignas(32) std::int32_t rfr[kAvxMaxNeurons];
    alignas(32) std::int32_t out_m[kAvxMaxNeurons];
    alignas(32) std::int32_t out_r[kAvxMaxNeurons];
    alignas(32) std::int32_t out_s[kAvxMaxNeurons];  // 1 = spiked
    const std::uint32_t n = ms.count;
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::uint32_t u = ms.first + i;
        memb[i] = st[u].membrane;
        inp[i]  = st[u].syn_input;
        refr[i] = st[u].refractory_left;
        thr[i]  = gen[u].threshold;
        rst[i]  = gen[u].reset;
        lsh[i]  = gen[u].leak_shift;
        rfr[i]  = gen[u].refractory;
    }
    const __m256i vmin = _mm256_set1_epi32(-32768);
    const __m256i vmax = _mm256_set1_epi32(32767);
    const __m256i zero = _mm256_setzero_si256();
    for (std::uint32_t i = 0; i < n; i += 8) {
        const std::uint32_t lane = std::min<std::uint32_t>(8, n - i);
        __m256i M  = _mm256_load_si256(reinterpret_cast<const __m256i*>(memb + i));
        __m256i I  = _mm256_load_si256(reinterpret_cast<const __m256i*>(inp + i));
        __m256i R  = _mm256_load_si256(reinterpret_cast<const __m256i*>(refr + i));
        __m256i T  = _mm256_load_si256(reinterpret_cast<const __m256i*>(thr + i));
        __m256i RS = _mm256_load_si256(reinterpret_cast<const __m256i*>(rst + i));
        __m256i LS = _mm256_load_si256(reinterpret_cast<const __m256i*>(lsh + i));
        __m256i RF = _mm256_load_si256(reinterpret_cast<const __m256i*>(rfr + i));
        const __m256i active = _mm256_cmpeq_epi32(R, zero);   // refr==0
        // leak: m -= trunc(m / 2^leak_shift)   (only meaningful if active)
        const __m256i leak = avx_trunc_shr(M, LS);
        __m256i m2 = _mm256_sub_epi32(M, leak);
        m2 = _mm256_add_epi32(m2, I);
        m2 = _mm256_max_epi32(vmin, _mm256_min_epi32(vmax, m2)); // clamp16
        const __m256i spike = _mm256_and_si256(
            _mm256_cmpgt_epi32(m2, _mm256_sub_epi32(T, _mm256_set1_epi32(1))),
            active);
        // membrane: spike -> reset; active non-spike -> m2; inactive -> reset
        const __m256i m_act = _mm256_blendv_epi8(m2, RS, spike);
        const __m256i m_out = _mm256_blendv_epi8(RS, m_act, active);
        // refractory: spike -> gen refractory; inactive -> refr-1; else 0
        const __m256i r_dec = _mm256_sub_epi32(R, _mm256_set1_epi32(1));
        const __m256i r_inact = _mm256_max_epi32(zero, r_dec);
        const __m256i r_act = _mm256_blendv_epi8(zero, RF, spike);
        const __m256i r_out = _mm256_blendv_epi8(r_inact, r_act, active);
        // spike flag
        const __m256i s_out = _mm256_and_si256(spike, _mm256_set1_epi32(1));
        _mm256_store_si256(reinterpret_cast<__m256i*>(out_m + i), m_out);
        _mm256_store_si256(reinterpret_cast<__m256i*>(out_r + i), r_out);
        _mm256_store_si256(reinterpret_cast<__m256i*>(out_s + i), s_out);
        (void)lane;
    }
    // scalar writeback: identical observable effects to the scalar loop
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::uint32_t u = ms.first + i;
        st[u].membrane = static_cast<std::int16_t>(out_m[i]);
        st[u].refractory_left = static_cast<std::uint8_t>(out_r[i]);
        st[u].syn_input = 0;
        if (out_s[i]) {
            spike_masks[ms.spike_off + i / 64] |= 1ull << (i % 64);
            ++spikes;
        }
    }
}
#endif  // __AVX2__

// run one scheduler pass over a module's neurons; fire into delay rings.
void BrainNet::step() {
    // dopamine-gated plasticity step (spec §13.2): gate is set by the
    // caller from the agent's dopamine level before each burst.
    const std::int32_t pstep = std::max<std::int32_t>(
        1, plastic_rate * stdp_gate / 100000 * plastic_scale_pm / 1000);
    const std::uint32_t win = stdp_window;

    // 1) deliver kicks scheduled for this tick (insertion order preserved).
    const std::uint32_t slot =
        static_cast<std::uint32_t>(tick % kRingLen);
    for (const std::uint32_t idx : ring[slot]) {
        const BrainSynapse& s = syn[idx];
        // effective weight = baseline + learned delta (clamped §13.1)
        std::int32_t w = s.weight;
        if (dw[idx]) {
            w = clamp16(static_cast<std::int64_t>(w) +
                        static_cast<std::int64_t>(dw[idx]) * 8);
        }
        BrainNeuronState& ts = st[s.target];
        ts.syn_input = sat_add(ts.syn_input, w);
        // eligibility: record this delivered kick on the post neuron so a
        // near-future post spike can potentiate it (causal STDP).
        if (s.flags & 1) {
            const std::uint32_t pos = recent_pos[s.target];
            kick_hist[s.target][pos] =
                (tick << 24) | static_cast<std::uint64_t>(idx);
            recent_pos[s.target] = static_cast<std::uint8_t>((pos + 1) & 3);
        }
    }
    ring[slot].clear();

    // 2) due modules in fixed id order; each runs at most once per tick.
    const std::uint64_t pend = pending_mask;
    for (int m = 0; m < kBrainModuleCount; ++m) {
        BrainModuleState& ms = modules[m];
        if (!ms.count) continue;
        const bool due =
            (tick % ms.period == ms.phase % ms.period) ||
            ((pend >> m) & 1ull);
        if (!due) continue;
        // consume the event request whether or not it was the trigger
        pending_mask &= ~(1ull << m);

        // LIF update over the module's neuron range.
        std::fill_n(spike_masks.begin() + ms.spike_off, ms.spike_words, 0);
#if defined(__AVX2__)
        if (avx2 && ms.count <= 256 && ms.count >= 8) {
            lif_module_avx2(ms);
        } else
#endif
        for (std::uint32_t i = 0; i < ms.count; ++i) {
            const std::uint32_t u = ms.first + i;
            BrainNeuronState& s = st[u];
            const BrainNeuronGen& g = gen[u];
            if (s.refractory_left) {
                --s.refractory_left;
                s.membrane = g.reset;
                s.syn_input = 0;
                continue;
            }
            std::int32_t m32 = s.membrane;
            // leak toward 0: defined division (truncation), dt-exact
            m32 -= m32 / (1 << g.leak_shift);
            m32 = clamp16(static_cast<std::int64_t>(m32) + s.syn_input);
            s.syn_input = 0;
            if (m32 >= g.threshold) {
                s.membrane = g.reset;
                s.refractory_left = g.refractory;
                spike_masks[ms.spike_off + i / 64] |=
                    1ull << (i % 64);
                ++spikes;
            } else {
                s.membrane = static_cast<std::int16_t>(m32);
            }
        }
        // 3) propagate spikes through CSR rows into delay rings + STDP.
        for (std::uint32_t w = 0; w < ms.spike_words; ++w) {
            const std::uint64_t word = spike_masks[ms.spike_off + w];
            if (!word) continue;
            const std::uint32_t base = ms.first + w * 64;
            for_each_spike(word, base, [&](std::uint32_t u) {
                last_fire[u] = tick + 1;
                // causal LTP: kicks this neuron just received within the
                // STDP window get potentiated (pre-before-post).
                for (std::uint64_t e : kick_hist[u]) {
                    if (e == ~0ull) continue;
                    if (tick - (e >> 24) > win) continue;
                    const std::uint32_t k =
                        static_cast<std::uint32_t>(e & 0xFFFFFFull);
                    if (k < syn_count && (syn[k].flags & 1)) {
                        std::int32_t d = dw[k] + pstep;
                        dw[k] = static_cast<std::int8_t>(
                            std::max(-kDwMax, std::min(kDwMax, d)));
                    }
                }
                const std::uint32_t b = row_ptr[u], e = row_ptr[u + 1];
                for (std::uint32_t k = b; k < e; ++k) {
                    // anti-causal LTD: post fired within the window BEFORE
                    // this pre spike -> depress (post-before-pre).
                    if ((syn[k].flags & 1)) {
                        const std::uint64_t lf = last_fire[syn[k].target];
                        if (lf != 0 && tick + 1 - lf <= win) {
                            std::int32_t d = dw[k] - pstep;
                            dw[k] = static_cast<std::int8_t>(
                                std::max(-kDwMax, std::min(kDwMax, d)));
                        }
                    }
                    const std::uint8_t d = syn[k].delay;
                    const std::uint32_t at =
                        static_cast<std::uint32_t>(tick + d) % kRingLen;
                    if (ring[at].size() < 8192)
                        ring[at].push_back(k);
                    else
                        ++dropped_kicks;
                }
            });
        }
        ms.last_run = tick;
        ++ms.event_accum;
    }
    ++tick;
}

}  // namespace simlab
