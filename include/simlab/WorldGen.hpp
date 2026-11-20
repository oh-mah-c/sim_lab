#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include "Subsurface.hpp"
#include "Species.hpp"
#include "World.hpp"

namespace simlab {

// ---- World generation (TODO §2 + §2.8) ----------------------------------------
// Deterministic, process-based world creation. Stages run in dependency order:
//   terrain → hydrology → climate → subsurface → resources → ecology
//
// SEED SYSTEM (TODO §2.8): every field is an ANALYTIC function of
// (world_seed, domain, global cell coordinate) — the "Global World Map" tier.
// Materialized cells + entities are the "Regional/Local Map" tier, produced by
// generate_region() for any global rectangle in ANY order with identical
// results (entity ids reflect call order; world content does not).
//
// Domain-separated seeds via worldgen_seed(); per-callsite salts keep usages
// independent inside a domain.

// Bump when the generation algorithm changes; stored on the World so saves
// can reject incompatible replays (TODO §2.8.E).
// v4: placer_site field — placer deposits now require a river cell with
//     slow flow draining a fault-bearing catchment (process-based, §2.4).
// v3: continent-scale relief, latitude temperature, orographic rain shadow,
//     lakes (basin minima), faults, erosion-exposed outcrops, data-driven
//     bedrock/soil/species, floor-division noise (valid for negative coords).
inline constexpr int kGeneratorVersion = 4;

// Domain-separated seed derivation (TODO §2.8.A): each generation domain gets
// an independent stream so changing one domain cannot perturb the others.
// Domains: "terrain" "hydrology" "climate" "atmosphere" "geology"
//          "subsurface" "resources" "ecology"
inline std::uint64_t worldgen_seed(std::uint64_t world_seed, const char* domain) {
    return Rng::stream_seed(world_seed, std::string("worldgen.") + domain);
}

struct WorldGenParams {
    int grid_w = 64;              // cells
    int grid_h = 64;
    int cell_size = 16;           // map units per cell
    std::uint64_t seed = 1;
    std::int32_t sea_level = 200; // metres; cells below are water
    // Ecology entity cap PER generate()/generate_region() call. Region-order
    // independence holds only while the cap is not reached — raise it (or use
    // a huge value) when materializing a world by regions.
    int max_trees = 400;
    // Global cell offset of this grid's origin (TODO §2.8.B).
    int origin_x = 0;
    int origin_y = 0;
    // Lazy materialization (fog-of-war, Phase 9): when true,
    // generate_region() fills ONLY terrain/subsurface fields and skips the
    // entity stages (deposits/species). Terrain fill is idempotent — a later
    // call with defer_entities=false materializes entities for cells whose
    // entity stage has not run yet (tracked in World::gen_cells_).
    bool defer_entities = false;
    // Species names the ecology stage must NOT spawn (Primordial Genesis
    // places its founders itself). Empty = every species (default).
    std::vector<std::string> skip_species;
};

class WorldGenerator {
public:
    // Full-grid generation: sizes w.terrain/w.subsurface to params and fills
    // the whole grid, then materializes entities for the full rect.
    bool generate(World& w, const WorldGenParams& params, std::string* err = nullptr);

    // Materialize a GLOBAL cell rectangle (gx0,gy0,gw,gh) of an already-sized
    // terrain grid. Field values are analytic so any subset/any order yields
    // identical world content; spawn dedup prevents duplicate entities when
    // regions overlap or are regenerated. Entities may spawn for cells outside
    // the materialized terrain bounds (fields are analytic everywhere).
    bool generate_region(World& w, const WorldGenParams& p,
                         int gx0, int gy0, int gw, int gh,
                         std::string* err = nullptr);

    // Consistency checks on a (generated or hand-built) world.
    static std::vector<std::string> validate(const World& w);

    // ---- Global Map tier: analytic fields (TODO §2.8.C) ----------------------
    // Pure functions of (seed, sea_level, global cell). Sample any coordinate —
    // no grid required. These define the continuous world; grids/entities are
    // materializations of it.
    static std::int32_t elevation_at(std::uint64_t seed, int gx, int gy);

    // ---- Planetary mode (Primordial Genesis) ----------------------------------
    // A closed planet is encoded IN the worldgen seed (bit 63 set, width in
    // bits 32..46, height in bits 47..61, user seed in the low 32 bits), so
    // every pure field function above stays a function of (seed, gx, gy) and
    // legacy seeds (bit 63 clear) are untouched. In planet mode:
    //   * climate belts by latitude: hot wet equator (centre row) -> temperate
    //     -> frozen poles (top/bottom rows), minus an altitude lapse rate;
    //   * the map rim is ocean (east/west seas, a closed world, no void);
    //   * ridged mountain ranges + several continental masses isolate basins.
    static std::uint64_t planet_seed(std::uint32_t user_seed, int w, int h);
    static bool planet_dims(std::uint64_t seed, int& w, int& h);
    // latitude in permille: 0 = equator, 1000 = pole (planet seeds only)
    static std::int32_t planet_latitude(std::uint64_t seed, int gy);
    static bool is_sea(std::uint64_t seed, int gx, int gy, std::int32_t sea_level);
    static bool is_river(std::uint64_t seed, int gx, int gy, std::int32_t sea_level);
    static std::int32_t moisture_at(std::uint64_t seed, int gx, int gy,
                                    std::int32_t sea_level);
    static std::int32_t temperature_at(std::uint64_t seed, int gx, int gy,
                                       std::int32_t sea_level);
    static std::uint8_t water_at(std::uint64_t seed, int gx, int gy,
                                 std::int32_t sea_level);   // 0 land 1 sea 2 river
    static std::int32_t vegetation_at(std::uint64_t seed, int gx, int gy,
                                      std::int32_t sea_level);
    static void wind_at(std::uint64_t seed, int gx, int gy,
                        std::int32_t& wx, std::int32_t& wy);
    // Geological/hydrological process fields (all analytic):
    static bool is_lake(std::uint64_t seed, int gx, int gy, std::int32_t sea_level);
    static bool is_fault(std::uint64_t seed, int gx, int gy);   // fault trace (hydrothermal pathway)
    static std::int32_t latitude_term(int gy);                  // deci-°C offset by global latitude
    static std::int32_t rain_shadow_at(std::uint64_t seed, int gx, int gy);  // permille moisture lost
    static std::int32_t slope_at(std::uint64_t seed, int gx, int gy);
    // Metres of cover stripped by uplift+erosion; a buried deposit shallower
    // than this outcrops (starts exposed).
    static std::int32_t erosion_depth_at(std::uint64_t seed, int gx, int gy);
    // Placer deposition site (TODO §2.4 gold story): a river cell where flow
    // has slowed (low slope) AND the upstream catchment contains a fault —
    // the hydrothermal source whose eroded material the stream transports
    // and drops. Pure analytic function of (seed, coords): deterministic and
    // region-order independent like every other field.
    static bool placer_site_at(std::uint64_t seed, int gx, int gy,
                               std::int32_t sea_level);
    // Parent-rock class (lithology prop value 1..3) of the bedrock at a cell.
    static std::int32_t bedrock_class_at(std::uint64_t seed, int gx, int gy);
    static std::int32_t water_table_at(std::uint64_t seed, int gx, int gy,
                                       std::int32_t sea_level);
    // Strata column for a global cell (needs material registry for ids).
    static std::vector<Stratum> column_at(const World& w, std::uint64_t seed,
                                          int gx, int gy, std::int32_t sea_level);

    // Text preview of the materialized terrain (world-map inspection tool).
    static std::string preview_ascii(const World& w, int max_cols = 96);

private:
    // integer value-noise / fbm helpers (deterministic, no floating point)
    static std::uint32_t hash2(std::int32_t x, std::int32_t y, std::uint64_t seed);
    static std::int32_t vnoise(std::int32_t x, std::int32_t y, int scale,
                               std::uint64_t seed);
    static std::int32_t fbm(std::int32_t x, std::int32_t y, std::uint64_t seed);

    // steepest-descent trace from (gx,gy); `out` receives the LAND cells
    // visited (sea terminus excluded). `reached_water` is set when the path
    // terminates at standing water (sea) rather than a local minimum/step cap.
    static void trace_descent(std::uint64_t seed, int gx, int gy,
                              std::int32_t sea_level,
                              std::vector<std::pair<int,int>>& out,
                              bool* reached_water = nullptr);

    // Read-only evaluation context for analytic fields (also used by validate()).
    struct EvalCtx {
        const World& w;
        std::uint64_t seed;
        std::int32_t sea_level;
        std::map<std::pair<int,int>, std::uint8_t> water;   // memo: pure-function cache
        EvalCtx(const World& ww, std::uint64_t s, std::int32_t sl) : w(ww), seed(s), sea_level(sl) {}
    };
    // Generation context: EvalCtx + mutable world + params.
    struct GenCtx : EvalCtx {
        World& wm;
        const WorldGenParams& p;
        GenCtx(World& ww, const WorldGenParams& pp)
            : EvalCtx(ww, pp.seed, pp.sea_level), wm(ww), p(pp) {}
    };

    static std::uint8_t water_ctx(EvalCtx& ctx, int gx, int gy);
    static std::int32_t veg_value(std::int32_t temp, std::int32_t moist);
    // Value of a named condition field at a global cell (false if unknown).
    static bool field_value(EvalCtx& ctx, const std::string& field, int gx, int gy,
                            std::int64_t depth, std::int64_t& out);
    static bool conditions_hold(EvalCtx& ctx, const std::vector<FormationCondition>& cs,
                                int gx, int gy, std::int64_t depth,
                                std::string* failed = nullptr);
    static void fill_terrain_cell(GenCtx& ctx, int gx, int gy);
    static void fill_subsurface_cell(GenCtx& ctx, int gx, int gy);

    void stage_resources(GenCtx& ctx, int gx0, int gy0, int gw, int gh);
    void stage_ecology(GenCtx& ctx, int gx0, int gy0, int gw, int gh);
};

}  // namespace simlab
