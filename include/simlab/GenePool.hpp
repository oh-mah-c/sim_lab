#pragma once
// CL-1 Phase 4 — Persistent Gene Pool ("di sản tiến hóa xuyên ván chơi").
//
// A viewer/meta-layer Hall of Fame: the top-k historically fittest
// individuals, each carrying their BrainGenome (slow weights +
// hyper-genes) and body Genome (species loci such as organ_lung_gain /
// organ_gill_gain). Persisted to a binary file (default
// saves/gene_pool.bin) so knowledge survives total population wipeouts.
//
// Determinism contract: the pool is READ-ONLY world input. derive() is a
// pure function of (world_seed, entity id, pool contents) — it consumes no
// RNG state and mutates nothing, so state_hash() stays a pure function of
// (world inputs, pool snapshot). Player "bless" writes change the pool
// file, which intentionally changes FUTURE spawns (a god-tool, like the
// existing beacon/fear powers — never an in-run mutation of World Truth).
#include <cstdint>
#include <string>
#include <vector>

#include "simlab/Brain.hpp"
#include "simlab/Entity.hpp"

namespace simlab {

class PropertyRegistry;

struct GenePoolEntry {
    BrainGenome brain;          // must be valid to be stored/injected
    Genome body;                // species loci snapshot (display/future use)
    std::string kind;           // "sapient" etc — sanity + UI label
    std::int64_t fitness = 0;   // caller-scored: age + descendants + wealth
    std::uint64_t seen_tick = 0;
};

class GenePool {
public:
    static constexpr std::size_t kMaxEntries = 32;
    static constexpr std::uint64_t kDeriveDenominator = 10;  // 70% draw rate

    // entries sorted by fitness descending
    const std::vector<GenePoolEntry>& entries() const { return entries_; }
    std::size_t size() const { return entries_.size(); }
    bool empty() const { return entries_.empty(); }

    // Insert if the entry beats the current worst (or pool not full).
    // Entries with invalid brains are ignored.
    void consider(GenePoolEntry e);
    // Player "Bless": force to the top (fitness = current best + 1).
    void bless(GenePoolEntry e);
    // Remove entries whose slow block is byte-identical to `slow`.
    void dedupe_slow(const std::vector<std::int8_t>& slow);

    // Deterministic injection for a fresh brain slot: with probability
    // 70% (hash of world_seed+npc) derives a child genome by block
    // crossover of two pool donors + mutation at the child's mut_scale.
    // Returns false -> caller falls back to hash-init (the 30% diversity
    // draw, or an empty pool).
    bool derive(std::uint64_t world_seed, EntityId npc,
                BrainGenome& out) const;

    // Binary persistence. Body-genome prop names resolve through `props`;
    // unknown names are skipped on load (forward-compatible).
    bool load(const std::string& path, const PropertyRegistry& props);
    bool save(const std::string& path, const PropertyRegistry& props) const;

private:
    std::vector<GenePoolEntry> entries_;   // fitness desc, capped at kMax
};

} // namespace simlab
