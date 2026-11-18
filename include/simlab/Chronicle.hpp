#pragma once
// World History analyzer (Phase 9): reads World Truth + the causal ledger
// and answers the historian's questions — census, genetic drift, tech and
// cultural lineage, conservation audit — without replaying raw events.
// Pure read: analyze() never mutates the world.
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include "Types.hpp"
#include "World.hpp"

namespace simlab {

struct CensusRow {
    std::string kind;
    std::int64_t alive = 0;
    std::int64_t dead = 0;
    // Heuristic split of the dead (World-Truth props at death, not intent):
    // starved = a need prop still pinned near its ceiling; other = anything
    // else (violence, processing, structural failure).
    std::int64_t starved = 0;
    std::int64_t other_dead = 0;
};

struct Chronicle {
    std::uint64_t tick = 0;
    std::int64_t entities_alive = 0;
    std::int64_t entities_dead = 0;
    std::int64_t minds = 0;
    std::vector<CensusRow> census;                    // sorted by kind name

    // Genetic drift: alive genome carriers, distinct locus-set signatures
    // (speciation measure), and per-prop allele sums/counts so callers can
    // compare founding vs current generations: mean = sum / count.
    std::int64_t genome_carriers = 0;
    std::int64_t genome_lineages = 0;
    std::map<std::string, std::pair<std::int64_t, std::int64_t>> allele_sums;

    // Tech & cultural lineage.
    std::vector<std::string> verified_methods;        // sorted unique MethodKeys
    std::int64_t composites = 0;                      // assembled structures
    std::int64_t inscribed = 0;                       // entities carrying `inscription`
    std::int64_t collectives = 0;
    EntityId top_leader = kNoEntity;                  // highest summed authority
    std::int64_t top_authority = 0;
    std::string leader_kind;

    // Conservation audit (hạch toán): material inventory + ledger-derived
    // energy spent/gained. Nothing here asserts a law — it REPORTS the books.
    std::map<MaterialId, std::int64_t> inventory;
    std::pair<std::int64_t, std::int64_t> energy{0, 0};   // {spent, gained}
};

Chronicle analyze_world(const World& w);

}  // namespace simlab
