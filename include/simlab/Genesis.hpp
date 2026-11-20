#pragma once
// Primordial Genesis — the "true playable" starting state (SDL-free).
//
// A naked world at tick 0: terrain, rivers, wild flora/fauna, ore outcrops,
// pathogens — everything worldgen derives from data — plus a handful of
// founders of the `founder_species` (default "sapient") in 1..N bands placed
// far apart. Founders carry NO language, NO lexicon, NO structures, NO tools:
// their minds know only the species' `innate` verbs (data/species.txt).
// Everything else (verbs, words, shelters, law, money, war) must emerge from
// the ordinary causal pipeline — see DriverState::curiosity.
//
// Kept separate from every test scenario: nothing here runs unless called.
#include <cstdint>
#include <string>
#include <vector>
#include "simlab/Types.hpp"

namespace simlab {

class World;

struct PrimordialParams {
    std::uint64_t seed = 1;
    int grid = 64;                        // cells per side
    int tribes = 2;                       // 1..4 bands
    int per_tribe = 6;                    // founders per band (3..12)
    std::string founder_species = "sapient";
    // false: a local valley (legacy analytic fields). true: a closed planet
    // `grid` x `grid` cells with latitude climate belts, ocean rim and
    // mountain spines (WorldGenerator::planet_seed); bands start in warm,
    // watered lowlands spread east-west, isolated from each other.
    bool planet = false;
    // Planet scale (2:1 equirectangular, e.g. 2048 x 1024 cells). 0 = the
    // planet IS the grid (grid x grid). When set, the planet stays analytic
    // (pure field functions of the worldgen seed — no RAM) and only a home
    // REGION of `grid` x `grid*2/3` cells on the warm belt is materialized
    // and simulated; the rest of the planet is dormant until visited.
    int planet_w = 0, planet_h = 0;
};

struct PrimordialWorld {
    std::vector<Vec2> homes;              // band camp centres
    std::vector<std::int64_t> tribe_lang; // speech-community id per band (no words yet)
    std::vector<EntityId> founders;
    std::uint64_t gen_seed = 0;           // worldgen seed (planet-coded when planet)
    int planet_w = 0, planet_h = 0;       // analytic planet size in cells (0 = valley)
    int region_x = 0, region_y = 0, region_w = 0, region_h = 0;   // materialized cells
};

// Fill an EMPTY world (content loaded, no entities) — deterministic in seed.
PrimordialWorld build_primordial(World& w, const PrimordialParams& p);

// Count of pre-made culture at tick 0 that must not exist (enclosures,
// inscriptions, lexical entries, concepts, debts, composites). 0 = clean.
std::size_t primordial_violations(const World& w);

}  // namespace simlab
