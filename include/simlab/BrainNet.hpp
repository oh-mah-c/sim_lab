#pragma once
#include <array>
#include <cstdint>
#include <vector>
#include "Types.hpp"

// Bio-inspired modular spiking brain (brain.txt spec, save v29).
// Replaces the legacy monolithic MLP/minGRU CL-1 block inside BrainSlot.
//
// Hard invariants (same as the rest of the engine):
//  - integer-only: membranes/weights/traces are int16/int8/int32; decay uses
//    defined division (C++ >> on negatives is implementation-defined, so all
//    signed shifts go through sar() / division);
//  - deterministic: module order, neuron order, synapse order and kick-ring
//    insertion order are all fixed; no unordered iteration, no wall clock,
//    no floats;
//  - sparse: firing is a uint64_t bitmask per 64-neuron group, iterated with
//    ctzll; synaptic work happens only for spikes that actually occur;
//  - no heap in hot paths: every vector is sized once at init/load and never
//    resized inside tick()/decide().

namespace simlab {

// ---- anatomical modules (spec Part II) --------------------------------------
enum class BrainModule : std::uint8_t {
    kBrainstem = 0, kThalamus, kHypothalamus, kCerebellum,
    kBasalGanglia, kAmygdala, kHippocampus, kPrefrontal,
    kSensoryCortex, kMotorCortex, kAssociation, kLanguage,
    kModuleCount
};
constexpr int kBrainModuleCount =
    static_cast<int>(BrainModule::kModuleCount);

// ---- neuron model (spec §5): integer LIF ------------------------------------
// Heritable parameters (BrainGenome) + dynamic state (BrainNet).
struct BrainNeuronGen {
    std::int16_t threshold = 800;    // firing threshold (membrane units)
    std::int16_t reset = -200;       // post-spike reset potential
    std::uint8_t leak_shift = 5;     // leak: m -= m / (1<<leak_shift) per tick
    std::uint8_t refractory = 2;     // ticks spent at reset after a spike
    std::uint8_t plasticity = 0;     // class (STDP eligibility later)
    std::int8_t sign = 1;            // +1 excitatory / -1 inhibitory weight sign
};

struct BrainNeuronState {
    std::int16_t membrane = 0;
    std::int32_t syn_input = 0;      // accumulated weighted kicks (saturating)
    std::uint8_t refractory_left = 0;
    std::uint8_t flags = 0;          // bit0: modulatory target, bit1: readout
    bool operator==(const BrainNeuronState&) const = default;
};

// ---- synapses (spec §6): CSR adjacency ---------------------------------------
struct BrainSynapse {
    std::uint32_t target;      // global neuron index
    std::int16_t weight;       // signed, already includes pre-sign
    std::uint8_t delay;        // ticks until delivery (>=1)
    std::uint8_t flags;        // bit0 plastic, bit1 modulatory
    bool operator==(const BrainSynapse&) const = default;
};

// ---- connectome tracts (spec §7): data-driven, not hard-coded ---------------
// One row per directed pathway; expanded into synapses at init from
// (world_seed, entity, genome) so the SAME genome always yields the SAME
// connectome — weights are never serialized, only regenerated.
struct BrainTract {
    std::uint8_t src, dst;     // BrainModule indices
    std::int16_t gain;         // base weight magnitude 0..255
    std::uint16_t density;     // permille of dst population connected per pre
    std::uint8_t delay;        // base delay ticks
    std::uint8_t inh_permille; // fraction of this tract's synapses inhibitory
};

// Required primary pathways (spec §7 table) + local recurrence handled
// separately. Order is fixed -> deterministic expansion.
constexpr std::size_t kBrainTractCount = 24;
extern const BrainTract kBrainTracts[kBrainTractCount];

// ---- genome (spec §23): heritable neural architecture ------------------------
// Lives in Brain.hpp's BrainGenome (kept there for GenePool/save compat) —
// the numeric fields the connectome builder consumes:
//   module_neurons[m]  population size per module (0 = module disabled)
//   threshold/reset/leak/refractory  global LIF baselines (per-neuron jitter
//                      is hash-derived, genome-scaled)
//   ei_permille        global inhibitory fraction for local recurrence
//   density_permille   local recurrence density
//   tract_gain[24]     per-tract weight override (0 = use kBrainTracts gain)
//   lr/mut equivalents kept for API compat (plasticity Phase 6)

// ---- module runtime state ----------------------------------------------------
struct BrainModuleState {
    std::uint32_t first = 0;        // first global neuron index
    std::uint32_t count = 0;
    std::uint32_t period = 1;       // scheduler: run every `period` ticks
    std::uint32_t phase = 0;        // scheduler: offset within period
    std::uint64_t last_run = 0;     // last scheduler tick it executed
    std::uint64_t event_accum = 0;  // coalesced pending-event counter
    std::int32_t scalar = 0;        // module-specific accumulator (neuromod etc.)
    std::uint32_t spike_off = 0;    // word offset into BrainNet::spike_masks
    std::uint32_t spike_words = 0;  // ceil(count/64)
};

// ---- the net -----------------------------------------------------------------
// All SoA storage, sized once at init. `tick` counts neural steps since
// birth; world_tick alignment is the caller's concern (brain_decide bursts).
struct BrainNet {
    // ---- genome-derived layout ---------------------------------------------
    std::array<BrainModuleState, kBrainModuleCount> modules{};
    std::uint32_t neuron_count = 0;
    std::uint32_t syn_count = 0;
    std::uint32_t max_delay = 1;

    // ---- neuron SoA ---------------------------------------------------------
    std::vector<BrainNeuronGen> gen;        // [neuron_count]
    std::vector<BrainNeuronState> st;       // [neuron_count]
    std::vector<std::uint8_t> module_of;    // [neuron_count] -> module idx

    // ---- CSR synapses --------------------------------------------------------
    std::vector<std::uint32_t> row_ptr;     // [neuron_count + 1]
    std::vector<BrainSynapse> syn;          // [syn_count]

    // ---- spike groups: ceil(count/64) masks per module run -------------------
    // transient scratch — recomputed each tick, never serialized
    std::vector<std::uint64_t> spike_masks;

    // ---- delayed kick rings --------------------------------------------------
    // ring[d] holds synapse indices scheduled for delivery at
    // (deliver_tick mod ring_len). Insertion order = firing order =
    // deterministic. Bounded: kicks beyond capacity are dropped in a
    // deterministic lowest-index-first overflow (counted, hashed).
    static constexpr std::uint32_t kRingLen = 16;
    std::array<std::vector<std::uint32_t>, kRingLen> ring;
    std::uint64_t tick = 0;
    std::uint64_t dropped_kicks = 0;
    std::uint64_t spikes = 0;            // cumulative spikes emitted (diag)

    // ---- scheduler ------------------------------------------------------------
    std::uint64_t pending_mask = 0;         // bit per module: event pending
    std::uint64_t cascade_left = 0;         // bounded propagation budget

    // ---- readout scratch ------------------------------------------------------
    // verb readout units live at the head of kMotorCortex when count>=32
    std::int32_t verb_logits[32] = {};

    void init(std::uint64_t world_seed, std::uint64_t entity,
              const std::array<std::uint16_t, kBrainModuleCount>& pops,
              std::int16_t threshold, std::int16_t reset,
              std::uint8_t leak_shift, std::uint8_t refractory,
              std::uint8_t ei_permille, std::uint16_t density_permille,
              const std::array<std::int16_t, 24>& tract_gain);
    // Deterministic re-derivation after load: same args as init; dynamic
    // state (membranes/rings/clocks) is overlaid by deserialize() after.

    // inject current (signed, -128..127) into neurons [first, first+count)
    // of a module — used for sensory encoding and neuromodulation later.
    void inject(std::uint8_t mod, std::int32_t neuron_in_module,
                std::int32_t current);

    // run one neural tick: deliver due kicks, update due modules in
    // module-id order, fire spikes into delay rings. Bounded cascade.
    void step();

    // mark a module for event-driven activation next tick (coalesced).
    void request(std::uint8_t mod) { pending_mask |= 1ull << mod; }

    // sparse spike iteration helper: calls fn(global_neuron_index) for each
    // set bit of mask word `word` at base offset `base`.
    template <class F>
    static inline void for_each_spike(std::uint64_t word, std::uint32_t base,
                                      F&& fn) {
        while (word) {
            const std::uint32_t b =
                static_cast<std::uint32_t>(__builtin_ctzll(word));
            word &= word - 1;
            fn(base + b);
        }
    }
};

// defined signed shift (spec §5.1): arithmetic shift right, floor semantics
// for negatives — identical on every platform.
inline std::int32_t brain_sar(std::int32_t v, std::uint32_t k) {
    return v >= 0 ? (v >> k) : -(((-v) + (1 << k) - 1) >> k);
}

}  // namespace simlab
