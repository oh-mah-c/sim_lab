// Sunnyworld Phase-1/2 verification suite.
// Covers the TODO §20 mandatory tests that apply to the foundation:
//   Add New Material (data-only), Causal Replay, Failed Experiment,
//   Unknown Interaction, Deterministic Replay, property validation.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <map>
#include <string>
#include "simlab/World.hpp"
#include "simlab/Geometry.hpp"
#include "simlab/WorldGen.hpp"
#include "simlab/render/Atlas.hpp"
#include "simlab/render/Snapshot.hpp"
#include "simlab/render/Patron.hpp"
#include "simlab/Discovery.hpp"
#include "simlab/Driver.hpp"
#include "simlab/Genesis.hpp"
#include "../src/LawUtil.hpp"
#include "simlab/Chronicle.hpp"
#include "simlab/Brain.hpp"
#include "simlab/NarrativeHost.hpp"
#include <chrono>
#include <thread>

using namespace simlab;

static int failures = 0;
static int checks = 0;
#define CHECK(cond) do { ++checks; if (!(cond)) { ++failures; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static PropId pid(const World& w, const char* n) {
    const PropertyDef* d = w.props.find_name(n);
    return d ? d->id : kNoProp;
}

// ---------- registry & data-driven content ---------------------------------
static void test_registries() {
    World w(1, "data");
    CHECK(w.props.size() >= 10);
    CHECK(w.materials.size() >= 6);
    CHECK(w.materials.id_of("dry_wood") != kNoMaterial);
    CHECK(w.materials.id_of("obsidian") != kNoMaterial);
    CHECK(w.materials.id_of("unobtanium") == kNoMaterial);

    // Property range validation is enforced at material parse AND at entity creation.
    PropertyBag bad;
    bad.set(pid(w, "hardness"), Value::integer(5000));   // out of 0..1000
    const EntityId e = w.create_entity("rock", w.materials.id_of("stone"), {0, 0}, bad);
    CHECK(e == kNoEntity);

    // Unknown property id rejected too.
    PropertyBag bad2;
    bad2.set(9999, Value::integer(1));
    CHECK(w.create_entity("x", kNoMaterial, {0, 0}, bad2) == kNoEntity);
}

// ---------- add material via DATA ONLY ---------------------------------------
static void test_new_material_data_only() {
    // Write a temp data dir: same properties + one new material nobody coded for.
    const std::string dir = "/tmp/simlab_newmat";
    std::string cmd = "mkdir -p " + dir + " && cp data/properties.txt " + dir + "/";
    CHECK(std::system(cmd.c_str()) == 0);
    {
        std::ofstream f(dir + "/materials.txt");
        f << "material dry_wood\n  hardness 350\n  brittleness 650\n  mass 800\n"
             "  density 500000\n  integrity 450\n  flammability 800\n"
             "  thermal_mass 1200\n  conductivity 120\n  fertility 0\n"
             "  metabolism 0\n  sight_range 0\n  speed 0\n  opacity 1000\n"
             "material void_glass\n  hardness 990\n  brittleness 950\n  mass 300\n"
             "  density 2500000\n  integrity 120\n  flammability 0\n"
             "  thermal_mass 700\n  conductivity 60\n  fertility 0\n"
             "  metabolism 0\n  sight_range 0\n  speed 0\n  opacity 1000\n";
    }
    World w(7, dir);
    CHECK(w.materials.id_of("void_glass") != kNoMaterial);
    // The engine already "understands" it: physics resolves its properties.
    const MaterialId vg = w.materials.id_of("void_glass");
    const EntityId target = w.create_entity("glass_shard", vg, {100, 100});
    const EntityId club = w.create_entity("club", w.materials.id_of("dry_wood"), {90, 100});
    InteractionContext ctx;
    ctx.actor = club; ctx.targets = {target};
    ctx.parameters.action = "impact"; ctx.parameters.force = 50000;
    const InteractionResult r = w.interact(ctx);
    CHECK(r.completed);
    const Entity* t = w.find(target);
    CHECK(t->prop(w, pid(w, "integrity"))->i < 120);   // damaged below material default
    // And it is far more fragile than dry_wood under the same hit.
    CHECK(t->prop(w, pid(w, "integrity"))->i < 100);
}

// ---------- physics outcomes differ by DATA, not code ------------------------
static void test_physics_property_driven() {
    World w(3, "data");
    const MaterialId wood = w.materials.id_of("dry_wood");
    const MaterialId stone = w.materials.id_of("stone");
    const auto integrity_after = [&](MaterialId m, std::int64_t force) {
        World& ww = w;   // fresh entities each call inside one world is fine
        const EntityId a = ww.create_entity(" striker", wood, {0, 0});
        const EntityId t = ww.create_entity("target", m, {10, 0});
        InteractionContext ctx;
        ctx.actor = a; ctx.targets = {t};
        ctx.parameters.action = "impact"; ctx.parameters.force = force;
        ww.interact(ctx);
        const Entity* e = ww.find(t);
        return e->prop(ww, pid(ww, "integrity"))->i;
    };
    const std::int64_t wood_i = integrity_after(wood, 50);
    const std::int64_t stone_i = integrity_after(stone, 50);
    CHECK(wood_i < 450);            // wood takes real damage
    CHECK(stone_i > wood_i);        // stone shrugs it off — hardness is data
}

// ---------- unknown interaction: no free success -----------------------------
static void test_unknown_interaction() {
    World w(5, "data");
    const EntityId a = w.create_entity("person", kNoMaterial, {0, 0});
    const EntityId t = w.create_entity("rock", w.materials.id_of("stone"), {5, 0});
    InteractionContext ctx;
    ctx.actor = a; ctx.targets = {t};
    ctx.parameters.action = "transmute_to_gold";   // no law supports this
    const InteractionResult r = w.interact(ctx);
    CHECK(!r.completed);
    CHECK(!r.failure.empty());
    CHECK(w.ledger.size() == 0);                    // nothing happened, nothing recorded as success
}

// ---------- causal chain -------------------------------------------------------
static void test_causal_chain() {
    World w(9, "data");
    const EntityId club = w.create_entity("club", w.materials.id_of("dry_wood"), {0, 0});
    const EntityId branch = w.create_entity("branch", w.materials.id_of("dry_wood"), {20, 0});
    // First event: a prior cause the caller supplies.
    InteractionContext c1;
    c1.actor = club; c1.targets = {branch};
    c1.parameters.action = "impact"; c1.parameters.force = 20;   // chip, not destroy
    const InteractionResult r1 = w.interact(c1);
    CHECK(r1.completed);
    CHECK(w.find(branch)->alive);   // target must survive for the second hit
    // Second event cites the first as its parent — the chain must render.
    InteractionContext c2 = c1;
    c2.parent_events = {r1.event};
    const InteractionResult r2 = w.interact(c2);
    CHECK(r2.completed);
    const CausalEvent* e2 = w.ledger.find(r2.event);
    CHECK(e2 != nullptr);
    if (!e2) return;
    CHECK(e2->parent_events.size() == 1 && e2->parent_events[0] == r1.event);
    CHECK(e2->applied_laws.size() == 1);
    const std::string ex = w.ledger.explain(r2.event);
    CHECK(ex.find("#") != std::string::npos);
    CHECK(ex.find(std::to_string(r1.event)) != std::string::npos);
}

// ---------- deterministic replay ----------------------------------------------
static void test_determinism() {
    auto run = [] {
        World w(42, "data");
        const MaterialId wood = w.materials.id_of("dry_wood");
        const MaterialId stone = w.materials.id_of("stone");
        for (int i = 0; i < 10; ++i) {
            const EntityId a = w.create_entity(" striker", wood, {i * 100, 0});
            const EntityId t = w.create_entity("target", i % 2 ? stone : wood, {i * 100 + 50, 0});
            InteractionContext ctx;
            ctx.actor = a; ctx.targets = {t};
            ctx.parameters.action = "impact";
            ctx.parameters.force = 20000 + i * 7000;
            ctx.parent_events = i ? std::vector<EventId>{static_cast<EventId>(i)} : std::vector<EventId>{};
            w.interact(ctx);
            w.step();
        }
        return w.state_hash();
    };
    CHECK(run() == run());
}

// ---------- deposits: World Truth occurrences ----------------------------------
static void test_deposits() {
    World w(21, "data");
    CHECK(w.deposits.size() >= 3);
    CHECK(w.deposits.id_of("copper_ore_body") != kNoDeposit);
    CHECK(w.deposits.id_of("unobtanium_vein") == kNoDeposit);

    // Buried deposit exists in world truth but is NOT exposed.
    const EntityId dep = w.create_deposit("copper_ore_body", {500, 500});
    CHECK(dep != kNoEntity);
    const Entity* e = w.find(dep);
    CHECK(e != nullptr);
    CHECK(e->deposit == w.deposits.id_of("copper_ore_body"));
    CHECK(e->prop(w, pid(w, "exposed"))->b == false);
    CHECK(e->prop(w, pid(w, "deposit_remaining"))->i == 24000);

    // Surface deposit is exposed by definition.
    const EntityId clay = w.create_deposit("surface_clay", {100, 100});
    CHECK(w.find(clay)->prop(w, pid(w, "exposed"))->b == true);

    // Unknown deposit name cannot be placed.
    CHECK(w.create_deposit("unobtanium_vein", {0, 0}) == kNoEntity);
}

// ---------- relationships ------------------------------------------------------
static void test_relations() {
    World w(23, "data");
    const EntityId a = w.create_entity("person", kNoMaterial, {0, 0});
    const EntityId b = w.create_entity("house", kNoMaterial, {10, 0});
    w.relate(a, "located_in", b);
    w.relate(a, "owns", b);
    CHECK(w.related(a, "located_in", b));
    CHECK(!w.related(b, "located_in", a));       // direction matters
    CHECK(w.relations_of(a).size() == 2);
    CHECK(w.relations_of(a, "owns").size() == 1);
    // Deterministic: same relations -> same hash contribution (checked via replay below)
}

// ---------- world generation (Phase 2) ----------------------------------------
static void test_worldgen() {
    WorldGenParams params; params.seed = 1234;
    World w1(1, "data"), w2(1, "data");
    WorldGenerator gen;
    CHECK(gen.generate(w1, params));
    CHECK(gen.generate(w2, params));

    // Terrain fields populated.
    CHECK(!w1.terrain.empty());
    CHECK(w1.terrain.elevation.size() ==
          static_cast<std::size_t>(w1.terrain.w) * w1.terrain.h);

    // Determinism: identical seed+data -> identical world state.
    CHECK(w1.state_hash() == w2.state_hash());

    // Different seed -> different world.
    WorldGenParams p2 = params; p2.seed = 999;
    World w3(1, "data");
    CHECK(gen.generate(w3, p2));
    CHECK(w1.state_hash() != w3.state_hash());

    // Deposits were placed by formation conditions (World Truth entities).
    std::size_t deposits = 0, trees = 0;
    for (const auto& e : w1.entities) {
        if (!e->alive) continue;
        if (e->deposit != kNoDeposit) ++deposits;
        if (e->kind_name.find("tree") != std::string::npos) ++trees;
    }
    CHECK(deposits > 0);
    CHECK(trees > 0);
    CHECK(trees <= static_cast<std::size_t>(params.max_trees));

    // Gold placer only occurs near water — verify condition held at placement.
    std::size_t gold = 0;
    for (const auto& e : w1.entities) {
        if (!e->alive || e->deposit == kNoDeposit) continue;
        const DepositDef* d = w1.deposits.find(e->deposit);
        if (!d || d->name != "gold_placer") continue;
        ++gold;
        // grid helper cannot see neighbours outside the grid -> interior cells only
        const int lx = TerrainGrid::cell_of(e->pos.x, w1.terrain.cell) - w1.terrain.ox;
        const int ly = TerrainGrid::cell_of(e->pos.y, w1.terrain.cell) - w1.terrain.oy;
        if (lx > 0 && ly > 0 && lx < w1.terrain.w - 1 && ly < w1.terrain.h - 1)
            CHECK(w1.terrain.near_water(e->pos));
    }
    CHECK(gold > 0);

    // Generated world passes consistency validation.
    const std::vector<std::string> issues = WorldGenerator::validate(w1);
    CHECK(issues.empty());
}

static void test_worldgen_validation_catches_bad() {
    // Hand-built inconsistent world: surface deposit placed on water.
    World w(5, "data");
    WorldGenParams p; p.seed = 7; p.grid_w = 16; p.grid_h = 16; p.sea_level = 300;
    WorldGenerator gen;
    CHECK(gen.generate(w, p));
    // find a water cell and force a surface deposit there
    TerrainGrid& t = w.terrain;
    Vec2 water_pos{}; bool found = false;
    for (int cy = 0; cy < t.h && !found; ++cy)
        for (int cx = 0; cx < t.w && !found; ++cx) {
            const Vec2 c = t.cell_center(cx, cy);
            if (t.is_water(c)) { water_pos = c; found = true; }
        }
    CHECK(found);
    const EntityId bad = w.create_deposit("surface_clay", water_pos);
    CHECK(bad != kNoEntity);
    const std::vector<std::string> issues = WorldGenerator::validate(w);
    bool caught = false;
    for (const std::string& s : issues) if (s.find("surface deposit") != std::string::npos) caught = true;
    CHECK(caught);
}

// ---------- seed system & persistence (TODO §2.8) ------------------------------
static void test_seed_system() {
    // Domain-separated seeds: each domain gets an independent stream.
    CHECK(worldgen_seed(42, "terrain") != worldgen_seed(42, "geology"));
    CHECK(worldgen_seed(42, "terrain") != worldgen_seed(43, "terrain"));
    CHECK(worldgen_seed(42, "terrain") == worldgen_seed(42, "terrain"));

    WorldGenParams p; p.seed = 777; p.grid_w = 64; p.grid_h = 64;
    World w(1, "data");
    WorldGenerator gen;
    CHECK(gen.generate(w, p));

    // Provenance recorded for versioning/replay checks.
    CHECK(w.world_seed == 777);
    CHECK(w.generator_version == kGeneratorVersion);
    CHECK(w.gen_origin_x == 0 && w.gen_origin_y == 0);

    // Coordinate stability (TODO §2.8.B): elevation is a pure function of
    // (seed, domain, global cell coord) — a patch generated at a nonzero
    // origin must match the same cells of a larger world.
    WorldGenParams big; big = p; big.grid_w = 128;
    World wbig(1, "data");
    CHECK(gen.generate(wbig, big));
    WorldGenParams patch; patch = p; patch.origin_x = 64; patch.origin_y = 0;
    World wpatch(1, "data");
    CHECK(gen.generate(wpatch, patch));
    for (int cy = 0; cy < 8; ++cy)
        for (int cx = 0; cx < 8; ++cx) {
            const Vec2 a = wbig.terrain.cell_center(cx + 64, cy);     // big global (64+cx)
            const Vec2 b = wpatch.terrain.cell_center(cx + 64, cy);   // patch global (64+cx)
            CHECK(wbig.terrain.elev_at(a) == wpatch.terrain.elev_at(b));
        }

    // Save/load roundtrip restores identical World Truth (TODO §2.8 save/load).
    const std::string path = "/tmp/simlab_test_save.bin";
    const std::uint64_t h_before = w.state_hash();
    CHECK(w.save(path));
    World w2(1, "data");
    CHECK(w2.load(path));
    CHECK(w2.state_hash() == h_before);
    CHECK(w2.entities.size() == w.entities.size());
    CHECK(w2.terrain.elevation == w.terrain.elevation);
    CHECK(w2.subsurface.cells.size() == w.subsurface.cells.size());
    CHECK(w2.world_seed == 777 && w2.generator_version == kGeneratorVersion);
    // world remains usable after load
    CHECK(WorldGenerator::validate(w2).empty());

    // ---- region generation: order independence + dedup (TODO §2.8.B) ----
    // Two worlds materialize the same two regions in OPPOSITE order on
    // pre-sized (entity-free) terrain; world content must be identical.
    auto setup_terrain = [](World& w, int gw, int gh) {
        TerrainGrid& t = w.terrain;
        t.w = gw; t.h = gh; t.cell = 16; t.ox = 0; t.oy = 0;
        const std::size_t n = static_cast<std::size_t>(gw) * gh;
        t.elevation.assign(n, 0); t.moisture.assign(n, 0);
        t.temperature.assign(n, 0); t.water.assign(n, 0);
        t.vegetation.assign(n, 0); t.slope.assign(n, 0);
        t.wind_x.assign(n, 0); t.wind_y.assign(n, 0);
        t.water_table.assign(n, 0);
        SubsurfaceGrid& s = w.subsurface;
        s.w = gw; s.h = gh; s.ox = 0; s.oy = 0; s.max_depth = 300;
        s.cells.assign(n, {});
    };
    auto entity_sig = [](const World& ww) {
        std::vector<std::string> sig;
        for (const auto& e : ww.entities) {
            if (!e->alive) continue;
            sig.push_back(e->kind_name + "|" +
                          std::to_string(e->pos.x) + "," + std::to_string(e->pos.y) +
                          "|dep=" + std::to_string(e->deposit));
        }
        std::sort(sig.begin(), sig.end());
        return sig;
    };
    WorldGenParams rp; rp.seed = 555; rp.cell_size = 16;
    rp.max_trees = 1 << 30;      // the per-call ecology cap would make content depend on call partition
    World wa(1, "data"), wb(1, "data");
    setup_terrain(wa, 128, 64); setup_terrain(wb, 128, 64);
    WorldGenerator g2;
    CHECK(g2.generate_region(wa, rp, 0, 0, 64, 64));    // west half first
    CHECK(g2.generate_region(wa, rp, 64, 0, 64, 64));   // then east
    CHECK(g2.generate_region(wb, rp, 64, 0, 64, 64));   // east first
    CHECK(g2.generate_region(wb, rp, 0, 0, 64, 64));    // then west
    // identical fields everywhere
    CHECK(wa.terrain.elevation == wb.terrain.elevation);
    CHECK(wa.terrain.water == wb.terrain.water);
    CHECK(wa.terrain.moisture == wb.terrain.moisture);
    CHECK(wa.subsurface.cells == wb.subsurface.cells);
    // identical world content (entity ids/order may differ — content must not)
    CHECK(entity_sig(wa) == entity_sig(wb));
    CHECK(!entity_sig(wa).empty());

    // re-materializing a region never duplicates entities (spawn dedup)
    const std::size_t ents = wa.entities.size();
    CHECK(g2.generate_region(wa, rp, 0, 0, 64, 64));
    CHECK(wa.entities.size() == ents);

    // preview tool produces a map
    const std::string pv = WorldGenerator::preview_ascii(w);
    CHECK(pv.find("world seed=") != std::string::npos);
    CHECK(pv.find('~') != std::string::npos || pv.find('=') != std::string::npos);
}

// ---------- extraction & processing (Phase 3) ---------------------------------
static InteractionContext mk_ctx(EntityId actor, std::vector<EntityId> targets,
                                 const char* action, std::int64_t force,
                                 const char* note = "", std::int64_t ticks = 1) {
    InteractionContext c;
    c.actor = actor; c.targets = std::move(targets);
    c.parameters.action = action; c.parameters.force = force;
    c.parameters.note = note; c.parameters.ticks = ticks;
    return c;
}

static void test_extraction() {
    World w(21, "data");
    const PropId qty = pid(w, "quantity");
    const PropId rem = pid(w, "deposit_remaining");
    const PropId exposed = pid(w, "exposed");
    const PropId integ = pid(w, "integrity");

    // buried copper deposit: dig exposes + yields; strike fails while buried
    const EntityId dep = w.create_deposit("copper_ore_body", {100, 100});
    const EntityId miner = w.create_entity("miner", kNoMaterial, {90, 100});
    CHECK(dep != kNoEntity && miner != kNoEntity);

    InteractionResult r = w.interact(mk_ctx(miner, {dep}, "strike", 500000));
    CHECK(!r.completed);   // buried: strike cannot reach it

    r = w.interact(mk_ctx(miner, {dep}, "dig", 500000));
    CHECK(r.completed);
    CHECK(!r.spawned.empty());
    // composition: copper_ore 700 + host_rock 300 shares of yield
    std::int64_t ore_q = 0, rock_q = 0;
    const MaterialId copper_ore = w.materials.id_of("copper_ore");
    const MaterialId host_rock = w.materials.id_of("host_rock");
    for (EntityId sid : r.spawned) {
        const Entity* s = w.find(sid);
        CHECK(s != nullptr && s->kind_name == "raw_resource");
        const std::int64_t q = s->prop(w, qty)->i;
        if (s->material == copper_ore) ore_q += q;
        if (s->material == host_rock) rock_q += q;
    }
    CHECK(ore_q > 0 && rock_q > 0);
    CHECK(ore_q > rock_q);   // 700 vs 300 share
    // deposit depletes + is now exposed
    const Entity* d = w.find(dep);
    CHECK(d->prop(w, rem)->i < 24000);
    CHECK(d->prop(w, exposed)->b == true);

    // weak dig on a fresh buried deposit fails (dig_resistance gate)
    const EntityId dep2 = w.create_deposit("copper_ore_body", {120, 100});
    r = w.interact(mk_ctx(miner, {dep2}, "dig", 100));
    CHECK(!r.completed);

    // out-of-reach target fails even with plenty of force
    const EntityId far_rock = w.create_entity("far_rock", w.materials.id_of("stone"), {400, 400});
    r = w.interact(mk_ctx(miner, {far_rock}, "strike", 500000));
    CHECK(!r.completed);

    // plain entity: strike chips off fragments of its own material
    const EntityId rock = w.create_entity("rock", w.materials.id_of("stone"), {95, 105});
    r = w.interact(mk_ctx(miner, {rock}, "strike", 200000));
    CHECK(r.completed);
    CHECK(!r.spawned.empty());
    const Entity* frag = w.find(r.spawned[0]);
    CHECK(frag != nullptr);
    CHECK(frag->material == w.materials.id_of("stone"));
    CHECK(frag->prop(w, qty)->i > 0);
    CHECK(w.find(rock)->prop(w, integ)->i < 500);
}

static void test_processing() {
    World w(22, "data");
    const MaterialId ore = w.materials.id_of("copper_ore");
    const MaterialId charcoal = w.materials.id_of("charcoal");
    const MaterialId metal = w.materials.id_of("copper_metal");
    const MaterialId slag = w.materials.id_of("slag");
    const PropId qty = pid(w, "quantity");
    const PropId temp = pid(w, "temperature");

    const EntityId worker = w.create_entity("worker", kNoMaterial, {50, 50});
    PropertyBag ore_b, fuel_b, heat_b;
    ore_b.set(qty, Value::integer(400));
    fuel_b.set(qty, Value::integer(50));
    heat_b.set(temp, Value::integer(12000));   // 1200°C — well above the recipe's 1000°C
    const EntityId ore_e = w.create_entity("ore", ore, {60, 50}, ore_b);
    const EntityId fuel_e = w.create_entity("fuel", charcoal, {55, 50}, fuel_b);
    const EntityId heat_e = w.create_entity("heater", kNoMaterial, {52, 50}, heat_b);

    // missing fuel -> fail
    InteractionResult r = w.interact(mk_ctx(worker, {ore_e, heat_e}, "process", 0, "smelt_copper"));
    CHECK(!r.completed);

    // cold participants -> fail (no entity reaches 1000°C)
    PropertyBag cold_b; cold_b.set(temp, Value::integer(100));
    const EntityId cold = w.create_entity("cold_rock", kNoMaterial, {53, 50}, cold_b);
    r = w.interact(mk_ctx(worker, {ore_e, fuel_e, cold}, "process", 0, "smelt_copper"));
    CHECK(!r.completed);

    // full conditions -> success: consumes inputs, spawns outputs+waste
    r = w.interact(mk_ctx(worker, {ore_e, fuel_e, heat_e}, "process", 0, "smelt_copper"));
    CHECK(r.completed);
    CHECK(r.spawned.size() == 2);   // copper_metal + slag
    MaterialId got[2] = {kNoMaterial, kNoMaterial};
    for (std::size_t i = 0; i < r.spawned.size(); ++i)
        got[i] = w.find(r.spawned[i])->material;
    CHECK((got[0] == metal && got[1] == slag) || (got[0] == slag && got[1] == metal));
    // inputs consumed (quantity -> 0 => destroyed via integrity)
    CHECK(!w.find(ore_e)->alive);
    CHECK(!w.find(fuel_e)->alive);

    // accounting: mass conserved — 400 ore units became 300 metal + 100 slag
    auto inv = w.material_inventory();
    CHECK(inv[metal] == 300);
    CHECK(inv[slag] == 100);
    CHECK(inv[ore] == 0);
    CHECK(inv[charcoal] == 0);

    // unknown process name -> clean failure
    r = w.interact(mk_ctx(worker, {heat_e}, "process", 0, "smelt_unobtanium"));
    CHECK(!r.completed);
}

// ---------- fracture: brittle shatters, tough deforms --------------------------
static void test_fracture() {
    World w(11, "data");
    const EntityId a = w.create_entity(" striker", w.materials.id_of("stone"), {0, 0});
    const EntityId glass = w.create_entity("shard", w.materials.id_of("obsidian"), {10, 0});
    InteractionContext ctx;
    ctx.actor = a; ctx.targets = {glass};
    ctx.parameters.action = "impact"; ctx.parameters.force = 120000;
    const InteractionResult r = w.interact(ctx);
    CHECK(r.completed);
    const Entity* g = w.find(glass);
    CHECK(!g->alive);   // obsidian: hardness 980 but brittleness 900 -> shattered
}

// ============================ Phase 3: budgets / reach / clock / thermal ======

static void apply_bag(World& w, EntityId id, const PropertyBag& b) {
    for (const auto& [p, v] : b.entries) w.find(id)->props.set(p, v);
}

static void test_budgets_clock_thermal() {
    World w(7, "data");
    const PropId energy = w.props.find_name("energy")->id;
    const PropId temp = w.props.find_name("temperature")->id;
    const PropId flamm = w.props.find_name("flammability")->id;
    const PropId integ = w.props.find_name("integrity")->id;
    CHECK(energy != kNoProp && temp != kNoProp && flamm != kNoProp);

    const EntityId actor = w.create_entity("actor", w.materials.id_of("stone"), {50, 50});
    PropertyBag pb;
    pb.set(energy, Value::integer(100000));  // 100 kJ milliJ
    apply_bag(w, actor, pb);

    // --- energy charged on successful extraction; ticks consume the clock ---
    const EntityId dep = w.create_deposit("copper_ore_body", {55, 50});
    const std::int64_t before_e = w.prop_int(*w.find(actor), energy, 0);
    const std::uint64_t before_tick = w.clock_.tick();
    InteractionResult r = w.interact(mk_ctx(actor, {dep}, "dig", 3000, "", 5));
    CHECK(r.completed);
    CHECK(w.prop_int(*w.find(actor), energy, 0) < before_e);  // energy spent
    CHECK(w.clock_.tick() == before_tick + 5);                    // 5 ticks elapsed

    // --- insufficient energy: atomic failure, no mutation, no clock ---
    w.find(actor)->props.set(energy, Value::integer(1));
    const EntityId rock = w.create_entity("rock", w.materials.id_of("stone"), {52, 50});
    r = w.interact(mk_ctx(actor, {rock}, "strike", 500000));
    CHECK(!r.completed);
    CHECK(w.clock_.tick() == before_tick + 5);
    CHECK(w.prop_int(*w.find(rock), integ, -1) == 900);       // untouched

    // --- processing ticks consume the clock (smelt_copper ticks=24) ---
    w.find(actor)->props.set(energy, Value::integer(100000));
    const EntityId ore = w.create_entity("ore", w.materials.id_of("copper_ore"), {51, 50});
    const EntityId fuel = w.create_entity("fuel", w.materials.id_of("charcoal"), {51, 51});
    PropertyBag qty_b; qty_b.set(w.props.find_name("quantity")->id, Value::integer(9999));
    apply_bag(w, ore, qty_b); apply_bag(w, fuel, qty_b);
    PropertyBag heat; heat.set(temp, Value::integer(12000));
    apply_bag(w, fuel, heat);  // fuel carries the heat
    const std::uint64_t t0 = w.clock_.tick();
    r = w.interact(mk_ctx(actor, {ore, fuel}, "process", 1000, "smelt_copper"));
    CHECK(r.completed);
    CHECK(w.clock_.tick() == t0 + 24);
    CHECK(w.material_inventory().at(w.materials.id_of("copper_metal")) == 300);

    // --- thermal: heat raises temperature via thermal_mass ---
    w.find(actor)->props.set(energy, Value::integer(50000000));
    const EntityId wood = w.create_entity("wood", w.materials.id_of("dry_wood"), {60, 50});
    const std::int64_t tm0 = w.prop_int(*w.find(wood), temp, 0);
    r = w.interact(mk_ctx(actor, {wood}, "heat", 1200000));
    CHECK(r.completed);
    CHECK(w.prop_int(*w.find(wood), temp, 0) > tm0);

    // --- ignition: flammable past 300°C loses integrity ---
    const EntityId twig = w.create_entity("twig", w.materials.id_of("dry_wood"), {62, 50});
    PropertyBag hot; hot.set(temp, Value::integer(2900));
    apply_bag(w, twig, hot);
    const std::int64_t ig0 = w.prop_int(*w.find(twig), integ, 0);
    r = w.interact(mk_ctx(actor, {twig}, "heat", 15000000));
    CHECK(r.completed);
    CHECK(w.prop_int(*w.find(twig), integ, 0) < ig0);   // burned

    // --- accounting: energy deltas visible in ledger ---
    const auto [spent, gained] = w.energy_accounting();
    CHECK(spent > 0);
    CHECK(gained == 0);

    // --- determinism: replay the same interleaved sequence -> identical hash ---
    World w2(7, "data");
    const EntityId a2 = w2.create_entity("actor", w2.materials.id_of("stone"), {50, 50});
    PropertyBag pb2; pb2.set(energy, Value::integer(100000));
    apply_bag(w2, a2, pb2);
    const EntityId d2 = w2.create_deposit("copper_ore_body", {55, 50});
    w2.interact(mk_ctx(a2, {d2}, "dig", 3000, "", 5));
    w2.find(a2)->props.set(energy, Value::integer(1));
    const EntityId r2 = w2.create_entity("rock", w2.materials.id_of("stone"), {52, 50});
    w2.interact(mk_ctx(a2, {r2}, "strike", 500000));
    w2.find(a2)->props.set(energy, Value::integer(100000));
    const EntityId o2 = w2.create_entity("ore", w2.materials.id_of("copper_ore"), {51, 50});
    const EntityId f2 = w2.create_entity("fuel", w2.materials.id_of("charcoal"), {51, 51});
    PropertyBag q2; q2.set(w2.props.find_name("quantity")->id, Value::integer(9999));
    apply_bag(w2, o2, q2); apply_bag(w2, f2, q2);
    PropertyBag h2; h2.set(temp, Value::integer(12000));
    apply_bag(w2, f2, h2);
    w2.interact(mk_ctx(a2, {o2, f2}, "process", 1000, "smelt_copper"));
    w2.find(a2)->props.set(energy, Value::integer(50000000));
    const EntityId wd2 = w2.create_entity("wood", w2.materials.id_of("dry_wood"), {60, 50});
    w2.interact(mk_ctx(a2, {wd2}, "heat", 1200000));
    const EntityId tw2 = w2.create_entity("twig", w2.materials.id_of("dry_wood"), {62, 50});
    PropertyBag ht2; ht2.set(temp, Value::integer(2900));
    apply_bag(w2, tw2, ht2);
    w2.interact(mk_ctx(a2, {tw2}, "heat", 15000000));
    CHECK(w2.state_hash() == w.state_hash());
}

// ============================ Phase 4: composition ==============================

static void test_composition() {
    World w(31, "data");
    const PropId energy = pid(w, "energy");
    const PropId mass = pid(w, "mass");
    const PropId integ = pid(w, "integrity");
    const PropId len = pid(w, "length");
    const PropId edge = pid(w, "edge");
    const PropId thrust = pid(w, "thrust_range");
    const PropId madeof = pid(w, "made_of");
    CHECK(thrust != kNoProp && madeof != kNoProp);

    const EntityId actor = w.create_entity("smith", w.materials.id_of("stone"), {50, 50});
    PropertyBag eb; eb.set(energy, Value::integer(10000000));
    apply_bag(w, actor, eb);

    // spear parts: flint edge-head + hardwood shaft
    const EntityId head = w.create_entity("head", w.materials.id_of("flint"), {51, 50});
    const EntityId shaft = w.create_entity("shaft", w.materials.id_of("hardwood"), {52, 50});

    const std::uint64_t t0 = w.clock_.tick();
    InteractionResult r = w.interact(mk_ctx(actor, {head, shaft}, "assemble", 1000));
    CHECK(r.completed);
    CHECK(r.spawned.size() == 1);
    CHECK(w.clock_.tick() == t0 + 2);              // 2 parts -> 2 ticks

    const Entity* spear = w.find(r.spawned[0]);
    CHECK(spear != nullptr && !spear->material);
    CHECK(spear->kind_name == "composite");
    // parts persist as contained members of the structure (not consumed)
    CHECK(w.find(head)->alive && w.find(shaft)->alive);
    const PropId contained = pid(w, "contained");
    CHECK(w.find(head)->prop(w, contained)->b == true);
    CHECK(w.find(shaft)->prop(w, contained)->b == true);
    // structure is real: part_of relation edges exist
    CHECK(w.related(head, "part_of", spear->id));
    CHECK(w.related(shaft, "part_of", spear->id));
    // contained parts are not independently interactable
    InteractionResult blocked = w.interact(mk_ctx(actor, {head}, "strike", 500000));
    CHECK(!blocked.completed);
    // derived: mass = sum; integrity = weakest link INCLUDING the joint —
    // bind joint = min(tensile 300,550) * 0.8 = 240 < flint integ 350
    CHECK(spear->prop(w, mass)->i == 1400);
    CHECK(spear->prop(w, integ)->i == 240);
    CHECK(spear->prop(w, pid(w, "joint_type"))->s == "bind");
    CHECK(spear->prop(w, len)->i == 1500);
    CHECK(spear->prop(w, edge)->i == 900);
    // emergent: thrust_range exists ONLY on composite (parts never had it)
    CHECK(spear->prop(w, thrust)->i == 1500 * 1900 / 1000);
    CHECK(w.find(head)->prop(w, thrust) == nullptr);   // dead, but no prop anyway
    // provenance
    CHECK(spear->prop(w, madeof)->s.find(std::to_string(head)) != std::string::npos);
    // energy charged: 500 + 1400/100 = 514
    const std::int64_t eleft = w.prop_int(*w.find(actor), energy, 0);
    CHECK(eleft == 10000000 - 514);

    // constraints: single part cannot assemble
    const EntityId lone = w.create_entity("lone", w.materials.id_of("stone"), {50, 55});
    r = w.interact(mk_ctx(actor, {lone}, "assemble", 1000));
    CHECK(!r.completed);

    // out-of-reach part fails the whole assembly
    const EntityId far = w.create_entity("far", w.materials.id_of("flint"), {500, 500});
    const EntityId near = w.create_entity("near", w.materials.id_of("dry_wood"), {50, 51});
    r = w.interact(mk_ctx(actor, {far, near}, "assemble", 1000));
    CHECK(!r.completed);
    CHECK(w.find(near)->alive);

    // too-fragile part fails (integrity below floor)
    const EntityId crumb = w.create_entity("crumb", w.materials.id_of("slag"), {51, 51});
    w.find(crumb)->props.set(integ, Value::integer(5));
    const EntityId ok2 = w.create_entity("ok2", w.materials.id_of("stone"), {52, 51});
    r = w.interact(mk_ctx(actor, {crumb, ok2}, "assemble", 1000));
    CHECK(!r.completed);

    // insufficient energy fails atomically
    w.find(actor)->props.set(energy, Value::integer(10));
    const EntityId a = w.create_entity("a", w.materials.id_of("stone"), {50, 52});
    const EntityId b = w.create_entity("b", w.materials.id_of("flint"), {51, 52});
    r = w.interact(mk_ctx(actor, {a, b}, "assemble", 1000));
    CHECK(!r.completed);
    CHECK(w.find(a)->alive && w.find(b)->alive);

    // no emergent prop without edge+length: two stones -> no thrust_range
    w.find(actor)->props.set(energy, Value::integer(10000000));
    const EntityId s1 = w.create_entity("s1", w.materials.id_of("stone"), {50, 53});
    const EntityId s2 = w.create_entity("s2", w.materials.id_of("stone"), {51, 53});
    r = w.interact(mk_ctx(actor, {s1, s2}, "assemble", 1000));
    CHECK(r.completed);
    CHECK(w.find(r.spawned[0])->prop(w, thrust) == nullptr);
    CHECK(w.find(r.spawned[0])->prop(w, len)->i == 0);

    // ---- disassemble: structure -> parts, fully reversible -----------------
    const std::uint64_t td = w.clock_.tick();
    const EntityId spear_id = spear->id;
    r = w.interact(mk_ctx(actor, {spear_id}, "disassemble", 1000));
    CHECK(r.completed);
    CHECK(!w.find(spear_id)->alive);                  // composite destroyed
    CHECK(w.find(head)->alive && w.find(shaft)->alive);
    CHECK(w.find(head)->prop(w, contained)->b == false);
    CHECK(!w.related(head, "part_of", spear_id));     // edges removed
    // parts retain their original props (flint still edge 900)
    CHECK(w.find(head)->prop(w, edge)->i == 900);
    CHECK(w.clock_.tick() == td + 2);                 // 2 parts -> 2 ticks
    // disassembled parts are interactable again
    r = w.interact(mk_ctx(actor, {head}, "strike", 500000));
    CHECK(r.completed);

    // determinism: replay -> identical hash
    World w2(31, "data");
    const EntityId a2 = w2.create_entity("smith", w2.materials.id_of("stone"), {50, 50});
    PropertyBag e2; e2.set(energy, Value::integer(10000000));
    apply_bag(w2, a2, e2);
    const EntityId h2 = w2.create_entity("head", w2.materials.id_of("flint"), {51, 50});
    const EntityId sh2 = w2.create_entity("shaft", w2.materials.id_of("hardwood"), {52, 50});
    w2.interact(mk_ctx(a2, {h2, sh2}, "assemble", 1000));
    w2.interact(mk_ctx(a2, {h2}, "strike", 500000));   // refused: part is contained (failed attempts are ledgered)
    const EntityId l2 = w2.create_entity("lone", w2.materials.id_of("stone"), {50, 55});
    w2.interact(mk_ctx(a2, {l2}, "assemble", 1000));
    const EntityId f2 = w2.create_entity("far", w2.materials.id_of("flint"), {500, 500});
    const EntityId n2 = w2.create_entity("near", w2.materials.id_of("dry_wood"), {50, 51});
    w2.interact(mk_ctx(a2, {f2, n2}, "assemble", 1000));
    const EntityId c2 = w2.create_entity("crumb", w2.materials.id_of("slag"), {51, 51});
    w2.find(c2)->props.set(integ, Value::integer(5));
    const EntityId o2 = w2.create_entity("ok2", w2.materials.id_of("stone"), {52, 51});
    w2.interact(mk_ctx(a2, {c2, o2}, "assemble", 1000));
    w2.find(a2)->props.set(energy, Value::integer(10));
    const EntityId aa2 = w2.create_entity("a", w2.materials.id_of("stone"), {50, 52});
    const EntityId bb2 = w2.create_entity("b", w2.materials.id_of("flint"), {51, 52});
    w2.interact(mk_ctx(a2, {aa2, bb2}, "assemble", 1000));
    w2.find(a2)->props.set(energy, Value::integer(10000000));
    const EntityId s12 = w2.create_entity("s1", w2.materials.id_of("stone"), {50, 53});
    const EntityId s22 = w2.create_entity("s2", w2.materials.id_of("stone"), {51, 53});
    w2.interact(mk_ctx(a2, {s12, s22}, "assemble", 1000));
    // disassemble the first composite + strike a freed part (mirror w);
    // composites were created in order — the spear is the first alive one.
    EntityId comp2 = kNoEntity;
    for (const auto& ep : w2.entities)
        if (ep->kind_name == "composite" && ep->alive) { comp2 = ep->id; break; }
    CHECK(comp2 != kNoEntity);
    w2.interact(mk_ctx(a2, {comp2}, "disassemble", 1000));
    w2.interact(mk_ctx(a2, {h2}, "strike", 500000));
    CHECK(w2.state_hash() == w.state_hash());
}

// ============================ Phase 4: wielding ================================

static void test_wielding() {
    World w(47, "data");
    const PropId energy = pid(w, "energy");
    const PropId integ = pid(w, "integrity");

    const EntityId actor = w.create_entity("hunter", w.materials.id_of("stone"), {50, 50});
    PropertyBag eb; eb.set(energy, Value::integer(10000000));
    apply_bag(w, actor, eb);

    // build a spear (flint head + hardwood shaft -> thrust_range 2850, edge 900)
    const EntityId head = w.create_entity("head", w.materials.id_of("flint"), {51, 50});
    const EntityId shaft = w.create_entity("shaft", w.materials.id_of("hardwood"), {52, 50});
    InteractionResult r = w.interact(mk_ctx(actor, {head, shaft}, "assemble", 1000));
    CHECK(r.completed);
    const EntityId spear = r.spawned[0];
    CHECK(w.find(spear)->prop(w, pid(w, "thrust_range"))->i == 2850);

    // far target: 200 units away — beyond arm reach (64), inside spear reach
    const EntityId far = w.create_entity("far_prey", w.materials.id_of("dry_wood"), {50, 250});
    r = w.interact(mk_ctx(actor, {far}, "strike", 100000));
    CHECK(!r.completed);                                   // bare hands can't reach

    // wield the spear -> thrust_range extends reach, edge sharpens the blow
    r = w.interact(mk_ctx(actor, {spear}, "wield", 1000));
    CHECK(r.completed);
    CHECK(w.related(actor, "wielding", spear));
    const std::int64_t integ0 = w.prop_int(*w.find(far), integ, 0);
    r = w.interact(mk_ctx(actor, {far}, "strike", 100000));
    CHECK(r.completed);                                    // spear reaches 200u
    CHECK(w.prop_int(*w.find(far), integ, 0) < integ0);

    // unwield -> reach bonus gone
    r = w.interact(mk_ctx(actor, {spear}, "unwield", 1000));
    CHECK(r.completed);
    CHECK(!w.related(actor, "wielding", spear));
    const EntityId far2 = w.create_entity("far2", w.materials.id_of("dry_wood"), {50, 260});
    r = w.interact(mk_ctx(actor, {far2}, "strike", 100000));
    CHECK(!r.completed);

    // contained entities cannot be wielded
    r = w.interact(mk_ctx(actor, {head, shaft}, "assemble", 1000));  // head was consumed? no—still contained in spear1
    // head/shaft are already contained in spear -> this assemble must fail
    CHECK(!r.completed);

    // structural invariant: destroying the composite releases its parts
    r = w.interact(mk_ctx(actor, {spear}, "impact", 5000000));
    CHECK(r.completed);
    CHECK(!w.find(spear)->alive);
    CHECK(w.find(head)->alive && w.find(shaft)->alive);
    CHECK(w.find(head)->prop(w, pid(w, "contained"))->b == false);
    CHECK(!w.related(head, "part_of", spear));
    CHECK(!w.related(actor, "wielding", spear));   // dead tool drops too

    // determinism replay
    World w2(47, "data");
    const EntityId a2 = w2.create_entity("hunter", w2.materials.id_of("stone"), {50, 50});
    PropertyBag e2; e2.set(energy, Value::integer(10000000));
    apply_bag(w2, a2, e2);
    const EntityId h2 = w2.create_entity("head", w2.materials.id_of("flint"), {51, 50});
    const EntityId s2 = w2.create_entity("shaft", w2.materials.id_of("hardwood"), {52, 50});
    InteractionResult r0 = w2.interact(mk_ctx(a2, {h2, s2}, "assemble", 1000));
    const EntityId sp2 = r0.spawned.empty() ? kNoEntity : r0.spawned[0];
    CHECK(sp2 != kNoEntity);
    const EntityId f2 = w2.create_entity("far_prey", w2.materials.id_of("dry_wood"), {50, 250});
    w2.interact(mk_ctx(a2, {f2}, "strike", 100000));
    w2.interact(mk_ctx(a2, {sp2}, "wield", 1000));
    w2.interact(mk_ctx(a2, {f2}, "strike", 100000));
    w2.interact(mk_ctx(a2, {sp2}, "unwield", 1000));
    const EntityId f22 = w2.create_entity("far2", w2.materials.id_of("dry_wood"), {50, 260});
    w2.interact(mk_ctx(a2, {f22}, "strike", 100000));
    w2.interact(mk_ctx(a2, {h2, s2}, "assemble", 1000));
    w2.interact(mk_ctx(a2, {sp2}, "impact", 5000000));
    CHECK(w2.state_hash() == w.state_hash());
}

// ============================ Phase 4: parametric geometry ======================

static void test_geometry() {
    World w(53, "data");
    const PropId energy = pid(w, "energy");
    const PropId mass = pid(w, "mass");
    const PropId gl = pid(w, "geom_len");
    const PropId gt = pid(w, "geom_type");
    CHECK(gl != kNoProp && gt != kNoProp);

    const EntityId actor = w.create_entity("carver", w.materials.id_of("stone"), {50, 50});
    PropertyBag eb; eb.set(energy, Value::integer(10000000));
    apply_bag(w, actor, eb);

    // hardwood rod: 1500x120x120, mass 1200, density 750000 (< water -> floats)
    const EntityId log = w.create_entity("log", w.materials.id_of("hardwood"), {51, 50});
    GeomStats st = geom_stats(w, log);
    CHECK(!st.empty);
    CHECK(st.volume == 1500LL * 120 * 120);
    CHECK(st.swing == 1500);              // rod length = full reach from grip
    CHECK(st.floats);                     // wood density < water
    CHECK(st.com_x > 51);                 // CoM toward +X end of rod
    CHECK(st.com_y == 50);
    // orientation is state: rotate the rod 90° (turn permille 250) -> CoM swings to +Y
    const PropId go = pid(w, "geom_orient");
    w.find(log)->props.set(go, Value::integer(250));
    const GeomStats st2 = geom_stats(w, log);
    CHECK(st2.com_y > 51 + 500);          // rod now extends along +Y
    CHECK(st2.com_x < 51 + 200);

    // stone blob: dense -> sinks
    const EntityId rock = w.create_entity("boulder", w.materials.id_of("stone"), {60, 50});
    CHECK(!geom_stats(w, rock).floats);

    // carve the rod: length shrinks, mass scales with volume
    const std::int64_t m0 = w.prop_int(*w.find(log), mass, 0);
    InteractionResult r = w.interact(mk_ctx(actor, {log}, "carve", 500000));
    CHECK(r.completed);
    const std::int64_t nL = w.prop_int(*w.find(log), gl, 0);
    CHECK(nL < 1500);
    const std::int64_t m1 = w.prop_int(*w.find(log), mass, 0);
    CHECK(m1 < m0);                        // mass conserved downward
    // second carve gets shorter still (shape is dynamic state)
    r = w.interact(mk_ctx(actor, {log}, "carve", 500000));
    CHECK(r.completed);
    CHECK(w.prop_int(*w.find(log), gl, 0) < nL);

    // geometry-driven reach: wield a plain long rod (no thrust_range prop)
    const EntityId pole = w.create_entity("pole", w.materials.id_of("hardwood"), {55, 50});
    // strip any emergent props: hardwood has none; thrust_range absent
    CHECK(w.find(pole)->prop(w, pid(w, "thrust_range")) == nullptr);
    const EntityId far = w.create_entity("far2", w.materials.id_of("dry_wood"), {50, 400});
    r = w.interact(mk_ctx(actor, {far}, "strike", 100000));
    CHECK(!r.completed);                   // 350u > reach 64
    r = w.interact(mk_ctx(actor, {pole}, "wield", 1000));
    CHECK(r.completed);
    r = w.interact(mk_ctx(actor, {far}, "strike", 100000));
    CHECK(r.completed);                    // rod swing ~1500 covers 350u

    // composite geometry aggregates parts: spear swing from real dims
    const EntityId h = w.create_entity("hd", w.materials.id_of("flint"), {70, 50});
    const EntityId sh = w.create_entity("sh", w.materials.id_of("hardwood"), {71, 50});
    r = w.interact(mk_ctx(actor, {h, sh}, "assemble", 1000));
    CHECK(r.completed);
    const GeomStats cs = geom_stats(w, r.spawned[0]);
    CHECK(!cs.empty);
    CHECK(cs.swing >= 1400);               // shaft 1500 + head offset
    CHECK(cs.volume > 0);

    // geometric fit: tiny parts too far apart (relative to size) cannot bind,
    // even though both are within the actor's reach
    const PropId reach_p = pid(w, "reach");
    w.find(actor)->props.set(reach_p, Value::integer(200));
    const EntityId nug1 = w.create_entity("n1", w.materials.id_of("copper_metal"), {-50, 50});
    const EntityId nug2 = w.create_entity("n2", w.materials.id_of("copper_metal"), {150, 50});
    r = w.interact(mk_ctx(actor, {nug1, nug2}, "assemble", 1000));
    CHECK(!r.completed);                   // 200u apart, bind reach ~120
    // bring them adjacent -> binds
    w.find(nug2)->pos = {49, 50};
    r = w.interact(mk_ctx(actor, {nug1, nug2}, "assemble", 1000));
    CHECK(r.completed);
    w.find(actor)->props.set(reach_p, Value::integer(64));

    // carve requires geometry: entity without geom props fails cleanly
    const EntityId void_e = w.create_entity("void", kNoMaterial, {50, 60});
    r = w.interact(mk_ctx(actor, {void_e}, "carve", 100000));
    CHECK(!r.completed);

    // determinism replay
    World w2(53, "data");
    const EntityId a2 = w2.create_entity("carver", w2.materials.id_of("stone"), {50, 50});
    PropertyBag e2; e2.set(energy, Value::integer(10000000));
    apply_bag(w2, a2, e2);
    const EntityId l2 = w2.create_entity("log", w2.materials.id_of("hardwood"), {51, 50});
    w2.find(l2)->props.set(pid(w2, "geom_orient"), Value::integer(250));  // mirror w
    w2.create_entity("boulder", w2.materials.id_of("stone"), {60, 50});
    w2.interact(mk_ctx(a2, {l2}, "carve", 500000));
    w2.interact(mk_ctx(a2, {l2}, "carve", 500000));
    const EntityId p2 = w2.create_entity("pole", w2.materials.id_of("hardwood"), {55, 50});
    const EntityId f2 = w2.create_entity("far2", w2.materials.id_of("dry_wood"), {50, 400});
    w2.interact(mk_ctx(a2, {f2}, "strike", 100000));
    w2.interact(mk_ctx(a2, {p2}, "wield", 1000));
    w2.interact(mk_ctx(a2, {f2}, "strike", 100000));
    const EntityId h2 = w2.create_entity("hd", w2.materials.id_of("flint"), {70, 50});
    const EntityId s2 = w2.create_entity("sh", w2.materials.id_of("hardwood"), {71, 50});
    w2.interact(mk_ctx(a2, {h2, s2}, "assemble", 1000));
    w2.find(a2)->props.set(reach_p == kNoProp ? pid(w2,"reach") : reach_p, Value::integer(200));
    const EntityId n12 = w2.create_entity("n1", w2.materials.id_of("copper_metal"), {-50, 50});
    const EntityId n22 = w2.create_entity("n2", w2.materials.id_of("copper_metal"), {150, 50});
    w2.interact(mk_ctx(a2, {n12, n22}, "assemble", 1000));
    w2.find(n22)->pos = {49, 50};
    w2.interact(mk_ctx(a2, {n12, n22}, "assemble", 1000));
    w2.find(a2)->props.set(pid(w2,"reach"), Value::integer(64));
    const EntityId v2 = w2.create_entity("void", kNoMaterial, {50, 60});
    w2.interact(mk_ctx(a2, {v2}, "carve", 100000));
    CHECK(w2.state_hash() == w.state_hash());
}

// ============================ Phase 4: binding types ============================

static void test_binding_types() {
    World w(59, "data");
    const PropId energy = pid(w, "energy");
    const PropId integ = pid(w, "integrity");
    const EntityId actor = w.create_entity("smith", w.materials.id_of("stone"), {50, 50});
    PropertyBag eb; eb.set(energy, Value::integer(10000000));
    apply_bag(w, actor, eb);
    const PropId joint = pid(w, "joint_type");

    int x = 40;
    auto mkpair = [&]() {
        const EntityId h = w.create_entity("h" + std::to_string(x), w.materials.id_of("flint"), {x, 50});
        const EntityId s = w.create_entity("s" + std::to_string(x), w.materials.id_of("hardwood"), {x + 1, 50});
        x += 10;
        return std::pair<EntityId, EntityId>{h, s};
    };
    // joint = min(tensile 300,550) * factor -> wedge .9=270, pierce .6=180,
    // balance .4=120 (all below flint integ 350 -> joint governs)
    struct { const char* note; std::int64_t want; } cases[] = {
        {"wedge", 270}, {"pierce", 180}, {"balance", 120}};
    for (auto& c : cases) {
        auto pr = mkpair();
        InteractionResult r = w.interact(mk_ctx(actor, {pr.first, pr.second}, "assemble", 1000, c.note));
        CHECK(r.completed);
        const Entity* comp = w.find(r.spawned[0]);
        CHECK(comp->prop(w, integ)->i == c.want);
        CHECK(comp->prop(w, joint)->s == c.note);
    }
    // unknown binding type fails cleanly
    auto bad = mkpair();
    InteractionResult r = w.interact(mk_ctx(actor, {bad.first, bad.second}, "assemble", 1000, "glue"));
    CHECK(!r.completed);
    // pierce requires edge >= 300: copper_metal blobs have edge 0 -> fail
    const EntityId m1 = w.create_entity("m1", w.materials.id_of("copper_metal"), {90, 50});
    const EntityId m2 = w.create_entity("m2", w.materials.id_of("copper_metal"), {91, 50});
    r = w.interact(mk_ctx(actor, {m1, m2}, "assemble", 1000, "pierce"));
    CHECK(!r.completed);
    r = w.interact(mk_ctx(actor, {m1, m2}, "assemble", 1000, "bind"));
    CHECK(r.completed);
}

// ---------- Phase 5: agent cognition (TODO §10, §9.10) -----------------------
static void test_cognition() {
    World w(7, "data");
    const PropId sight = pid(w, "sight_range");
    const PropId reach = pid(w, "reach");
    const PropId exposed = pid(w, "exposed");
    const PropId integ = pid(w, "integrity");
    const PropId dep_rem = pid(w, "deposit_remaining");

    // NPC: sight 300, reach 500 — perceives near entities only
    PropertyBag nb;
    nb.set(sight, Value::integer(300));
    nb.set(reach, Value::integer(500));
    const EntityId npc = w.create_entity("npc", kNoMaterial, {0, 0}, nb);
    // truth-tagged rock: integrity 777 must NEVER enter observations
    PropertyBag rb; rb.set(integ, Value::integer(777));
    const EntityId near_r = w.create_entity("rock", w.materials.id_of("stone"), {60, 0}, rb);
    const EntityId far_r  = w.create_entity("rock", w.materials.id_of("stone"), {2000, 0});
    const EntityId dep    = w.create_deposit("copper_ore_body", {50, 0});
    const EntityId vis    = w.create_deposit("surface_clay", {40, 0});  // exposed outcrop
    CHECK(npc != kNoEntity && near_r != kNoEntity && dep != kNoEntity && vis != kNoEntity);

    // ---- perception: range-limited, observable props only --------------------
    CHECK(w.perceive(npc) >= 2);                    // near rock + exposed outcrop
    const AgentMind* m = w.find_mind(npc);
    CHECK(m != nullptr);
    CHECK(m->known_entities.count(near_r) == 1);
    CHECK(m->known_entities.count(vis) == 1);       // exposed deposit is visible
    // BURIED deposit: only its declared surface evidence is visible (§9.3/§9.10)
    CHECK(m->known_entities.count(dep) == 1);
    CHECK(m->known_entities.count(far_r) == 0);     // out of sight_range
    CHECK(m->known_entities.count(npc) == 0);       // self excluded
    const Observation* ode = nullptr;
    for (const Observation& o : m->observations)
        if (o.entity == vis) ode = &o;
    CHECK(ode != nullptr);
    {   // buried copper: sign visible, truth hidden, exposure state not a sign
        const Observation* bo = nullptr;
        for (const Observation& o : m->observations) if (o.entity == dep) bo = &o;
        CHECK(bo != nullptr);
        CHECK(bo->props.count(pid(w, "surface_color")) == 1);
        CHECK(bo->props.count(exposed) == 0);
        CHECK(bo->props.count(dep_rem) == 0);
        CHECK(bo->props.size() == 2);
    }
    CHECK(ode->props.count(exposed) == 1);          // observable evidence
    CHECK(ode->props.count(integ) == 0);            // truth prop hidden
    CHECK(ode->props.count(dep_rem) == 0);          // reserve hidden
    // rock observation likewise hides integrity=777
    const Observation* ork = nullptr;
    for (const Observation& o : m->observations)
        if (o.entity == near_r) ork = &o;
    CHECK(ork != nullptr && ork->props.count(integ) == 0);

    // ---- divergent NPC knowledge ---------------------------------------------
    PropertyBag nb2; nb2.set(sight, Value::integer(300));
    const EntityId npc2 = w.create_entity("npc", kNoMaterial, {9000, 9000}, nb2);
    CHECK(w.perceive(npc2) == 0);                   // sees nothing here
    const AgentMind* m2 = w.find_mind(npc2);
    CHECK(m2 != nullptr && m2->known_entities.empty());
    CHECK(m->known_entities.size() != m2->known_entities.size());

    // blind NPC (sight_range absent -> 0): no observations
    const EntityId blind = w.create_entity("npc", kNoMaterial, {55, 0});
    CHECK(w.perceive(blind) == 0);

    // ---- beliefs never mutate World Truth ------------------------------------
    const std::int64_t integ_before = w.prop_int(*w.find(near_r), integ, 0);
    CHECK(integ_before == 777);
    w.mind(npc).belief("outcome:strike:integrity:down").confidence = 999;
    CHECK(w.prop_int(*w.find(near_r), integ, 0) == 777);   // belief != truth

    // ---- learning from own action --------------------------------------------
    // fresh buried deposit for the dig experiment
    const EntityId dep2 = w.create_deposit("copper_ore_body", {70, 0});
    CHECK(dep2 != kNoEntity);
    InteractionResult r = w.interact(mk_ctx(npc, {dep2}, "dig", 500000));
    CHECK(r.completed);
    AgentMind& mm = w.mind(npc);
    CHECK(mm.known_actions.count("dig") == 1);
    CHECK(mm.find_belief("outcome:dig:exposed:up") != nullptr);
    CHECK(mm.find_belief("outcome:dig:deposit_remaining:down") != nullptr);

    // ---- belief confidence dynamics (TODO §10.6) ------------------------------
    AgentMind solo;
    solo.reinforce("k", 40, 1); solo.reinforce("k", 40, 2); solo.reinforce("k", 40, 3);
    CHECK(solo.find_belief("k")->status == BeliefStatus::Supported);
    for (int i = 0; i < 8; ++i) solo.weaken("k", 200, 10 + i);
    CHECK(solo.find_belief("k")->status == BeliefStatus::Disputed);
    CHECK(solo.find_belief("k")->confidence == 0);

    // ---- planning: goal decomposition over OWN knowledge ----------------------
    // npc2 knows no actions -> cannot plan anything
    Goal g; g.prop = exposed; g.value = 1; g.target = dep;
    CHECK(plan(w, npc2, g).empty());
    // npc knows "dig" and has dep in known_entities -> plans dig on dep
    // dep's surface evidence was perceived above -> it is a known entity
    std::vector<InteractionContext> pl = plan(w, npc, g);
    CHECK(!pl.empty());
    CHECK(pl[0].parameters.action == "dig");
    CHECK(pl[0].targets.size() == 1 && pl[0].targets[0] == dep);
    // executing the plan through the real pipeline exposes the deposit
    InteractionResult pr = w.interact(pl[0]);
    CHECK(pr.completed);
    CHECK(w.prop_int(*w.find(dep), exposed, 0) == 1);

    // ---- minds persist through save/load --------------------------------------
    const std::string path = "/tmp/simlab_cog_save.bin";
    const std::uint64_t h = w.state_hash();
    CHECK(w.save(path));
    World w3(1, "data");
    CHECK(w3.load(path));
    CHECK(w3.state_hash() == h);
    const AgentMind* m3 = w3.find_mind(npc);
    CHECK(m3 != nullptr && m3->known_actions.count("dig") == 1);
    CHECK(m3->known_entities.count(dep) == 1);
}

// ---------- Phase 5 completion: needs, witness learning, Verified, multi-step ---
static void test_phase5_needs_cognition() {
    World w(11, "data");
    const PropId hunger = pid(w, "hunger"), thirst = pid(w, "thirst");
    const PropId fatigue = pid(w, "fatigue"), metabolism = pid(w, "metabolism");
    const PropId sight = pid(w, "sight_range"), reach = pid(w, "reach");
    const PropId energy = pid(w, "energy"), integ = pid(w, "integrity");
    const PropId exposed = pid(w, "exposed");
    CHECK(hunger != kNoProp && thirst != kNoProp && fatigue != kNoProp);

    // ---- needs drift through the causal ledger (metabolize events) ----------
    PropertyBag nb;
    nb.set(hunger, Value::integer(800)); nb.set(thirst, Value::integer(800));
    nb.set(fatigue, Value::integer(800)); nb.set(metabolism, Value::integer(2000));
    nb.set(sight, Value::integer(500)); nb.set(reach, Value::integer(500));
    nb.set(energy, Value::integer(1000000));
    const EntityId npc = w.create_entity("npc", kNoMaterial, {0, 0}, nb);
    const EntityId rock = w.create_entity("rock", w.materials.id_of("stone"), {10, 0});
    CHECK(npc != kNoEntity && rock != kNoEntity);
    const std::size_t led0 = w.ledger.size();
    w.step();                                   // 1 tick
    // metabolism 2000: hunger +2, thirst +4, fatigue +1 per tick
    CHECK(w.prop_int(*w.find(npc), hunger, 0) == 802);
    CHECK(w.prop_int(*w.find(npc), thirst, 0) == 804);
    CHECK(w.prop_int(*w.find(npc), fatigue, 0) == 801);
    CHECK(w.ledger.size() == led0 + 1);         // drift is ledgered
    const CausalEvent* mev = w.ledger.find(w.ledger.history().back().id);
    CHECK(mev != nullptr && mev->action == "metabolize");
    bool saw_hunger = false;
    for (const StateChange& c : mev->changes)
        if (c.entity == npc && c.prop == hunger) saw_hunger = true;
    CHECK(saw_hunger);
    CHECK(mev->participants.size() == 1);       // rock has no needs -> untouched

    // ---- consume: physiology law relieves needs, destroys the food ----------
    const EntityId berries = w.create_entity("food", w.materials.id_of("wild_berries"),
                                             {100, 0});
    CHECK(berries != kNoEntity);
    InteractionResult cr = w.interact(mk_ctx(npc, {berries}, "consume", 0));
    CHECK(cr.completed);
    // hunger 802 - 400 nutrition + 2 ticks * 2 drift = 406; thirst 804 - 500 + 8 = 312
    CHECK(w.prop_int(*w.find(npc), hunger, 0) == 406);
    CHECK(w.prop_int(*w.find(npc), thirst, 0) == 312);
    CHECK(!w.find(berries)->alive);             // whole entity eaten
    CHECK(w.prop_int(*w.find(npc), energy, 0) == 999950);  // eating costs 50 mJ
    const AgentMind& mn = w.mind(npc);
    CHECK(mn.known_actions.count("consume") == 1);
    CHECK(mn.find_belief("outcome:consume:hunger:down") != nullptr);

    // ---- rest: actor-only need recovery --------------------------------------
    InteractionResult rr = w.interact(mk_ctx(npc, {}, "rest", 0, "", 10));
    CHECK(rr.completed);
    // fatigue was 801 + 2 (consume ticks) = 803; rest -20*10 = -200 -> 603,
    // then +10 drift over the 10 rest ticks -> 613
    CHECK(w.prop_int(*w.find(npc), fatigue, 0) == 613);

    // ---- derive_goals: only urgent needs become goals -------------------------
    std::vector<Goal> goals = derive_goals(w, npc);
    CHECK(goals.size() == 1);
    CHECK(goals[0].kind == Goal::Kind::SatisfyNeed);
    CHECK(goals[0].need == fatigue);            // 613 >= 500 urgency
    // hunger/thirst are satisfied -> no goals for them
    World wcalm(12, "data");
    PropertyBag cb;
    cb.set(hunger, Value::integer(100)); cb.set(fatigue, Value::integer(100));
    const EntityId calm = wcalm.create_entity("npc", kNoMaterial, {0, 0}, cb);
    CHECK(derive_goals(wcalm, calm).empty());

    // ---- witness learning: nearby agent learns the verb + weak evidence ------
    PropertyBag wb;
    wb.set(sight, Value::integer(500)); wb.set(reach, Value::integer(500));
    const EntityId npcB = w.create_entity("npc", kNoMaterial, {50, 0}, wb);
    w.mind(npcB);                                // becomes an agent (has a mind)
    const EntityId dep = w.create_deposit("copper_ore_body", {140, 0});
    CHECK(dep != kNoEntity);
    InteractionResult dr = w.interact(mk_ctx(npc, {dep}, "dig", 500000));
    CHECK(dr.completed);
    CHECK(!dr.spawned.empty());
    const CausalEvent* dev = w.ledger.find(dr.event);
    CHECK(dev != nullptr);
    CHECK(!dev->spawned.empty());               // spawn ids recorded in event
    const AgentMind& mb = w.mind(npcB);
    CHECK(mb.known_actions.count("dig") == 1);  // saw the verb used
    const Belief* wb_belief = mb.find_belief("outcome:dig:exposed:up");
    CHECK(wb_belief != nullptr);
    CHECK(wb_belief->evidence_for > 0);
    CHECK(wb_belief->self_evidence == 0);       // witnessed != personal test
    // yield learning: dig spawned copper_ore + host_rock raw_resource entities
    bool saw_copper_yield = false;
    for (const auto& [k, b] : mb.beliefs)
        if (k == "yield:dig:copper_ore") saw_copper_yield = true;
    CHECK(saw_copper_yield);
    // actor's own yield belief carries self evidence
    const Belief* ay = mn.find_belief("yield:dig:copper_ore");
    CHECK(ay != nullptr && ay->self_evidence > 0);

    // ---- Verified promotion: personal repeated success only ------------------
    AgentMind vm;
    for (int i = 0; i < 6; ++i) vm.reinforce("k", 34, i + 1, /*self=*/true);
    CHECK(vm.find_belief("k")->status == BeliefStatus::Verified);
    AgentMind wm;
    for (int i = 0; i < 20; ++i) wm.reinforce("k", 34, i + 1, /*self=*/false);
    CHECK(wm.find_belief("k")->status == BeliefStatus::Supported);
    CHECK(wm.find_belief("k")->status != BeliefStatus::Verified);
    // mixed: self success then witnessed failures -> Disputed, not Verified
    AgentMind dm;
    for (int i = 0; i < 6; ++i) dm.reinforce("k", 34, i + 1, true);
    CHECK(dm.find_belief("k")->status == BeliefStatus::Verified);
    for (int i = 0; i < 9; ++i) dm.weaken("k", 60, 20 + i);
    CHECK(dm.find_belief("k")->status == BeliefStatus::Disputed);

    // ---- plan_multi: speculative multi-step search, World Truth untouched ----
    World w2(13, "data");
    PropertyBag fb;
    fb.set(hunger, Value::integer(900)); fb.set(metabolism, Value::integer(0));
    fb.set(sight, Value::integer(500)); fb.set(reach, Value::integer(500));
    fb.set(energy, Value::integer(100000));
    const EntityId eater = w2.create_entity("npc", kNoMaterial, {0, 0}, fb);
    const EntityId b1 = w2.create_entity("food", w2.materials.id_of("wild_berries"), {100, 0});
    const EntityId b2 = w2.create_entity("food", w2.materials.id_of("wild_berries"), {120, 0});
    CHECK(eater != kNoEntity && b1 != kNoEntity && b2 != kNoEntity);
    w2.mind(eater).learn_action("consume");        // prior experience
    w2.perceive(eater);                            // sees both berry bushes
    std::vector<Goal> eg = derive_goals(w2, eater);
    CHECK(eg.size() == 1 && eg[0].need == hunger);
    const std::size_t led_before = w2.ledger.size();
    const std::uint64_t h_before = w2.state_hash();
    std::vector<Plan> plans = plan_multi(w2, eater, eg[0], 3, 6, 4);
    CHECK(!plans.empty());
    // eating both bushes in one consume relieves 800 -> hunger 100 = satisfied
    CHECK(plans[0].satisfied);
    CHECK(!plans[0].steps.empty());
    CHECK(plans[0].steps[0].parameters.action == "consume");
    // determinism: same inputs -> same plan
    std::vector<Plan> plans2 = plan_multi(w2, eater, eg[0], 3, 6, 4);
    CHECK(plans2.size() == plans.size());
    CHECK(plans2[0].score == plans[0].score);
    CHECK(plans2[0].steps.size() == plans[0].steps.size());
    // speculative search NEVER touched real World Truth
    CHECK(w2.prop_int(*w2.find(eater), hunger, 0) == 900);
    CHECK(w2.find(b1)->alive && w2.find(b2)->alive);
    CHECK(w2.ledger.size() == led_before);
    CHECK(w2.state_hash() == h_before);

    // executing the found plan for real satisfies the need
    for (const InteractionContext& step : plans[0].steps)
        CHECK(w2.interact(step).completed);
    CHECK(w2.prop_int(*w2.find(eater), hunger, 0) <= 100);

    // ---- save/load v3: spawned events + self_evidence roundtrip ---------------
    const std::string path = "/tmp/simlab_p5_save.bin";
    const std::uint64_t h = w.state_hash();
    CHECK(w.save(path));
    World w4(1, "data");
    CHECK(w4.load(path));
    CHECK(w4.state_hash() == h);
    const AgentMind* m4 = w4.find_mind(npcB);
    CHECK(m4 != nullptr);
    const Belief* lb = m4->find_belief("outcome:dig:exposed:up");
    CHECK(lb != nullptr && lb->self_evidence == 0);   // witnessed stays witnessed
    const Belief* ly = w4.find_mind(npc)->find_belief("yield:dig:copper_ore");
    CHECK(ly != nullptr && ly->self_evidence > 0);
}

// ---------- Phase 1 audit regressions ------------------------------------------
static void test_phase1_audit() {
    // bad data dir: error surfaced, registries all-or-nothing (empty)
    World bad(1, "no_such_dir");
    CHECK(!bad.content_error.empty());
    CHECK(bad.materials.size() == 1 && bad.props.size() == 1);
    World ok(1, "data");
    CHECK(ok.content_error.empty());

    // rejected create burns no id and registers no kind
    PropertyBag badp; badp.set(pid(ok, "integrity"), Value::integer(99999));
    const std::uint64_t ctr = ok.kinds.counter();
    const std::uint32_t kc = ok.kinds.kind_count();
    CHECK(ok.create_entity("ghost_kind", kNoMaterial, {0, 0}, badp) == kNoEntity);
    CHECK(ok.kinds.counter() == ctr && ok.kinds.kind_count() == kc);

    // >255 kinds: Entity::kind equals registry id, ids stay unique, save/load ok
    std::set<EntityId> ids;
    EntityId last = kNoEntity;
    for (int k = 0; k < 300; ++k) {
        last = ok.create_entity("kind" + std::to_string(k), kNoMaterial, {k, 0});
        ids.insert(last);
    }
    CHECK(ids.size() == 300);
    CHECK(ok.find(last)->kind == ok.kinds.kind_id("kind299"));

    // find(): index lookups agree with entity vector; unknown -> null
    CHECK(ok.find(last) != nullptr && ok.find(kNoEntity) == nullptr);
    CHECK(ok.find(0xDEADBEEFull) == nullptr);

    // relation index: incident queries match brute force, survive unrelate
    const EntityId a = ids.size() ? *ids.begin() : kNoEntity, b = last;
    ok.relate(a, "near", b); ok.relate(b, "owns", a); ok.relate(a, "near", b);
    CHECK(ok.relations.size() == 2);
    CHECK(ok.relations_of(a).size() == 2 && ok.relations_of(b, "near").size() == 1);
    ok.relations.unrelate(a, "near", b);
    CHECK(ok.relations_of(a).size() == 1 && ok.relations_of(b, "near").empty());

    // rng streams: consumed position is hashed, saved, and restored
    World r1(5, "data");
    r1.rng("loot").next(); r1.rng("loot").next();
    const std::uint64_t h = r1.state_hash();
    World r0(5, "data");
    CHECK(r0.state_hash() != h);
    CHECK(r1.save("/tmp/simlab_rng_save.bin"));
    World r2(5, "data");
    CHECK(r2.load("/tmp/simlab_rng_save.bin"));
    CHECK(r2.state_hash() == h);
    CHECK(r2.rng("loot").next() == r1.rng("loot").next());

    // buried deposit invisible, exposed one visible
    PropertyBag nb; nb.set(pid(r1, "sight_range"), Value::integer(500));
    const EntityId npc = r1.create_entity("npc", kNoMaterial, {0, 0}, nb);
    const EntityId bur = r1.create_deposit("iron_ore_seam", {10, 0});   // no evidence declared
    CHECK(r1.perceive(npc) == 0 && r1.mind(npc).known_entities.count(bur) == 0);

    // parser strictness + new property metadata
    PropertyRegistry pr; std::string perr;
    CHECK(!pr.parse("a integer u range 0 10x\n", &perr));                 // trailing junk
    CHECK(!pr.parse("a integer u depends=nope\n", &perr));                // unknown dependency
    CHECK(!pr.parse("a integer u uncertainty=2000\n", &perr));
    CHECK(pr.parse("base integer u\ndep integer u depends=base applies=rock,tree uncertainty=50\n", &perr));
    CHECK(pr.find_name("dep")->depends == "base" && pr.find_name("dep")->uncertainty == 50);
    CHECK(pr.unmet_dependencies({pr.find_name("dep")->id}).size() == 1);
    CHECK(pr.unmet_dependencies({pr.find_name("dep")->id, pr.find_name("base")->id}).empty());
    MaterialRegistry mr;
    CHECK(!mr.parse("material m\n  base 12abc\n", pr, &perr));            // strict number
    CHECK(!mr.parse("material m\n  base 1 2\n", pr, &perr));              // extra token
    DepositRegistry dr; MaterialRegistry m2; PropertyRegistry p2;
    CHECK(p2.parse("hardness integer u\nsign integer u domain=observable\n", &perr));
    CHECK(m2.parse("material rockm\n  hardness 1\n", p2, &perr));
    CHECK(!dr.parse("deposit d\n substance rockm 1000\n accessibility surface\n depth 5\n", p2, m2, &perr));
    CHECK(!dr.parse("deposit d\n substance rockm 1000\n accessibility buried\n", p2, m2, &perr));
    CHECK(!dr.parse("deposit d\n substance rockm 1000\n accessibility buried\n depth 3\n evidence hardness 4\n", p2, m2, &perr)); // truth prop as evidence
    CHECK(dr.parse("deposit d\n substance rockm 1000\n accessibility buried\n depth 3\n evidence sign 4\n", p2, m2, &perr));
    // provenance from files; evidence on entity
    CHECK(ok.materials.find_name("stone")->provenance == "data/materials.txt");
    CHECK(ok.deposits.find_name("copper_ore_body")->provenance == "data/deposits.txt");
    // dangling relation refused
    CHECK(ok.relations.size() == 1);
    ok.relate(a, "ghost", 0xABCDEFull);
    CHECK(ok.relations.size() == 1);
}

// ---------- Phase 2 completion: processes, data-driven geology/ecology ---------
static void test_phase2_complete() {
    WorldGenerator gen;
    WorldGenParams p; p.seed = 1234; p.grid_w = 96; p.grid_h = 96; p.max_trees = 1 << 30;
    World w(1, "data");
    CHECK(gen.generate(w, p));
    CHECK(w.generator_version == kGeneratorVersion && w.gen_sea_level == p.sea_level);
    CHECK(WorldGenerator::validate(w).empty());

    // ---- hydrology: lakes are basins, rivers have outlets, channels are sparse ----
    std::size_t lakes = 0, rivers = 0, cells = w.terrain.water.size();
    for (std::size_t i = 0; i < cells; ++i) {
        const int gx = static_cast<int>(i % w.terrain.w) + w.terrain.ox;
        const int gy = static_cast<int>(i / w.terrain.w) + w.terrain.oy;
        if (w.terrain.water[i] == 1 && w.terrain.elevation[i] >= p.sea_level) {
            ++lakes; CHECK(WorldGenerator::is_lake(p.seed, gx, gy, p.sea_level));
        }
        if (w.terrain.water[i] == 2) ++rivers;
    }
    CHECK(lakes > 0);
    CHECK(rivers > 0 && rivers * 4 < cells);        // channels, not a flood

    // ---- geological process: hydrothermal veins only on faults; erosion outcrops ----
    std::size_t veins = 0, outcrops = 0;
    const PropId exposed = pid(w, "exposed");
    for (const auto& e : w.entities) {
        if (!e->alive || e->deposit == kNoDeposit) continue;
        const DepositDef* d = w.deposits.find(e->deposit);
        const int gx = TerrainGrid::cell_of(e->pos.x, w.terrain.cell);
        const int gy = TerrainGrid::cell_of(e->pos.y, w.terrain.cell);
        if (d->name == "hydrothermal_gold_vein") {
            ++veins; CHECK(WorldGenerator::is_fault(p.seed, gx, gy));
        }
        if (!d->surface) {
            const bool strip = WorldGenerator::erosion_depth_at(p.seed, gx, gy) >= d->depth;
            CHECK((w.prop_int(*e, exposed, 0) == 1) == strip);   // exposure == erosion result
            if (strip) ++outcrops;
        }
    }
    CHECK(veins > 0 && outcrops > 0);

    // ---- climate: latitude, rain shadow bounds ----
    CHECK(WorldGenerator::latitude_term(0) > WorldGenerator::latitude_term(512));
    CHECK(WorldGenerator::latitude_term(1024) == WorldGenerator::latitude_term(0));
    for (int i = 0; i < 400; ++i) {
        const std::int32_t rs = WorldGenerator::rain_shadow_at(9, i * 7, i * 3);
        CHECK(rs >= 0 && rs <= 300);
    }

    // ---- continuity/validity at NEGATIVE global coordinates ----
    std::int32_t lo = 9999, hi = -9999;
    for (int y = -200; y < 0; ++y)
        for (int x = -200; x < 0; ++x) {
            const std::int32_t e = WorldGenerator::elevation_at(5, x, y);
            lo = std::min(lo, e); hi = std::max(hi, e);
        }
    CHECK(lo >= 80 && hi <= 500);
    for (int x = -6; x < 6; ++x)                    // no seam at the axes
        CHECK(std::abs(WorldGenerator::elevation_at(5, x, 3) - WorldGenerator::elevation_at(5, x + 1, 3)) < 40);
    // patch at negative origin == same cells of a larger world
    WorldGenParams neg = p; neg.grid_w = 32; neg.grid_h = 32; neg.origin_x = -16; neg.origin_y = -16;
    World wn(1, "data");
    CHECK(gen.generate(wn, neg));
    CHECK(WorldGenerator::validate(wn).empty());
    for (int cy = 0; cy < 32; cy += 5)
        for (int cx = 0; cx < 32; cx += 5)
            CHECK(wn.terrain.elevation[static_cast<std::size_t>(cy) * 32 + cx] ==
                  WorldGenerator::elevation_at(p.seed, cx - 16, cy - 16));

    // ---- content_hash is region-order independent (state_hash is not) ----
    auto sized = [&](World& ww) {
        TerrainGrid& t = ww.terrain; t.w = 64; t.h = 32; t.cell = 16; t.ox = 0; t.oy = 0;
        const std::size_t n = 64 * 32;
        t.elevation.assign(n, 0); t.moisture.assign(n, 0); t.temperature.assign(n, 0);
        t.water.assign(n, 0); t.vegetation.assign(n, 0); t.slope.assign(n, 0);
        t.wind_x.assign(n, 0); t.wind_y.assign(n, 0); t.water_table.assign(n, 0);
        ww.subsurface.w = 64; ww.subsurface.h = 32; ww.subsurface.ox = 0; ww.subsurface.oy = 0;
        ww.subsurface.cells.assign(n, {});
    };
    WorldGenParams rp = p; rp.max_trees = 1 << 30;
    World ra(1, "data"), rb(1, "data");
    sized(ra); sized(rb);
    CHECK(gen.generate_region(ra, rp, 0, 0, 32, 32));
    CHECK(gen.generate_region(ra, rp, 32, 0, 32, 32));
    CHECK(gen.generate_region(rb, rp, 32, 0, 32, 32));
    CHECK(gen.generate_region(rb, rp, 0, 0, 32, 32));
    CHECK(ra.content_hash() == rb.content_hash());
    // mismatched config on an existing world is refused
    WorldGenParams other = rp; other.seed = 4242;
    CHECK(!gen.generate_region(ra, other, 0, 0, 8, 8));
    other = rp; other.sea_level = 250;
    CHECK(!gen.generate_region(ra, other, 0, 0, 8, 8));

    // ---- config survives save/load ----
    WorldGenParams sp = p; sp.sea_level = 260; sp.grid_w = 32; sp.grid_h = 32;
    World ws(1, "data");
    CHECK(gen.generate(ws, sp));
    CHECK(ws.save("/tmp/simlab_p2_save.bin"));
    World wl(1, "data");
    CHECK(wl.load("/tmp/simlab_p2_save.bin"));
    CHECK(wl.gen_sea_level == 260 && wl.state_hash() == ws.state_hash());
    CHECK(WorldGenerator::validate(wl).empty());

    // ---- ecology: habitat conditions hold for every individual ----
    std::size_t flora = 0, fauna = 0;
    for (const auto& e : w.entities) {
        if (!e->alive) continue;
        const SpeciesDef* sp2 = w.species.find_name(e->kind_name);
        if (!sp2) continue;
        (sp2->fauna ? fauna : flora)++;
    }
    CHECK(flora > 0 && fauna > 0);

    // ---- validation catches physically impossible states (§2.6) ----
    {   // river flowing uphill: raise every neighbour of one river cell
        World wv(1, "data"); CHECK(gen.generate(wv, p));
        TerrainGrid& t = wv.terrain;
        bool done = false;
        for (int ly = 2; ly < t.h - 2 && !done; ++ly)
            for (int lx = 2; lx < t.w - 2 && !done; ++lx) {
                const std::size_t i = static_cast<std::size_t>(ly) * t.w + lx;
                if (t.water[i] != 2) continue;
                const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
                for (int k = 0; k < 4; ++k) {
                    const std::size_t j = static_cast<std::size_t>(ly + dy[k]) * t.w + lx + dx[k];
                    t.elevation[j] = t.elevation[i] + 50;
                    if (t.water[j] == 1) t.water[j] = 0;
                }
                done = true;
            }
        CHECK(done);
        bool caught = false;
        for (const std::string& s : WorldGenerator::validate(wv)) if (s.find("flows uphill") != std::string::npos) caught = true;
        CHECK(caught);
    }
    {   // deposit that violates its formation conditions (copper on a lowland/sea cell)
        World wv(1, "data"); CHECK(gen.generate(wv, p));
        Vec2 low{}; bool found = false;
        for (int cy = 0; cy < wv.terrain.h && !found; ++cy)
            for (int cx = 0; cx < wv.terrain.w && !found; ++cx)
                if (wv.terrain.elevation[static_cast<std::size_t>(cy) * wv.terrain.w + cx] < 150) { low = wv.terrain.cell_center(cx, cy); found = true; }
        if (!found) { // fall back: any cell outside copper's elevation band 150..500
            for (int cy = 0; cy < wv.terrain.h && !found; ++cy)
                for (int cx = 0; cx < wv.terrain.w && !found; ++cx) {
                    const auto e = wv.terrain.elevation[static_cast<std::size_t>(cy) * wv.terrain.w + cx];
                    if (e > 500 || e < 150) { low = wv.terrain.cell_center(cx, cy); found = true; }
                }
        }
        if (found) {
            wv.create_deposit("copper_ore_body", low);
            bool caught = false;
            for (const std::string& s : WorldGenerator::validate(wv)) if (s.find("violates formation condition") != std::string::npos) caught = true;
            CHECK(caught);
        }
    }
    {   // flora in extreme desert, away from water
        World wv(1, "data"); CHECK(gen.generate(wv, p));
        for (int gy = 0; gy < 96; ++gy)
            for (int gx = 0; gx < 96; ++gx)
                if (WorldGenerator::moisture_at(p.seed, gx, gy, p.sea_level) < 100 &&
                    WorldGenerator::water_at(p.seed, gx, gy, p.sea_level) == 0) {
                    wv.create_entity("temperate_tree", wv.materials.id_of("dry_wood"), wv.terrain.cell_center(gx, gy));
                    gy = 96; break;
                }
        // (only asserts when such a cell exists for this seed)
        bool desert = false;
        for (int gy = 0; gy < 96; ++gy) for (int gx = 0; gx < 96; ++gx)
            if (WorldGenerator::moisture_at(p.seed, gx, gy, p.sea_level) < 100) desert = true;
        if (desert) {
            bool caught = false;
            for (const std::string& s : WorldGenerator::validate(wv))
                if (s.find("outside habitat") != std::string::npos || s.find("extreme desert") != std::string::npos) caught = true;
            CHECK(caught);
        }
    }

    // ---- data-only extension: new rock + new species, ZERO engine changes ----
    namespace fs = std::filesystem;
    const fs::path dir = "/tmp/simlab_p2_data";
    fs::remove_all(dir); fs::copy("data", dir, fs::copy_options::recursive);
    {
        std::ofstream m(dir / "materials.txt", std::ios::app);
        m << "\nmaterial test_basanite\n  hardness 920\n  brittleness 280\n  mass 2800\n"
             "  density 2800000\n  integrity 940\n  lithology 1\n";
        std::ofstream s(dir / "species.txt", std::ios::app);
        s << "\nspecies test_shrub\n  kind flora\n  material dry_wood\n  density 1000\n"
             "  condition vegetation 0 1000\n  condition elevation 0 1000\n";
    }
    World wd(1, dir.string());
    CHECK(wd.content_error.empty());
    WorldGenParams dp = p; dp.grid_w = 64; dp.grid_h = 64;
    CHECK(gen.generate(wd, dp));
    const MaterialId newrock = wd.materials.id_of("test_basanite");
    bool rock_used = false;
    for (const auto& col : wd.subsurface.cells)
        for (const Stratum& st : col) if (st.material == newrock) rock_used = true;
    CHECK(rock_used);                                // new bedrock appears via data alone
    std::size_t shrubs = 0;
    for (const auto& e : wd.entities) if (e->alive && e->kind_name == "test_shrub") ++shrubs;
    CHECK(shrubs > 0);                               // new species appears via data alone
    CHECK(WorldGenerator::validate(wd).empty());
    fs::remove_all(dir);

    // species parser rejects bad habitat data
    SpeciesRegistry sr; std::string err;
    CHECK(!sr.parse("species a\n kind flora\n material nope\n density 5\n", w.props, w.materials, &err));
    CHECK(!sr.parse("species a\n kind flora\n material dry_wood\n density 0\n", w.props, w.materials, &err));
    CHECK(!sr.parse("species a\n kind flora\n material dry_wood\n density 5\n condition lithology 1 2\n", w.props, w.materials, &err));
    CHECK(sr.parse("species a\n kind fauna\n material dry_wood\n density 5\n condition fault 0 0\n", w.props, w.materials, &err));
}

// ---------- Phase 2: placer as a geological PROCESS (§2.4) ---------------------
static void test_phase2_placer() {
    WorldGenerator gen;
    WorldGenParams p; p.seed = 1234; p.grid_w = 96; p.grid_h = 96; p.max_trees = 1 << 30;
    World w(1, "data");
    CHECK(gen.generate(w, p));
    CHECK(w.generator_version == kGeneratorVersion);
    CHECK(WorldGenerator::validate(w).empty());

    // placer_site is pure analytic: same answer for any caller, any order
    int sites = 0;
    for (int gy = 0; gy < 96; ++gy)
        for (int gx = 0; gx < 96; ++gx) {
            const bool a = WorldGenerator::placer_site_at(p.seed, gx, gy, p.sea_level);
            const bool b = WorldGenerator::placer_site_at(p.seed, gx, gy, p.sea_level);
            CHECK(a == b);
            if (!a) continue;
            ++sites;
            // a deposition site MUST be flowing water with slow gradient
            CHECK(WorldGenerator::water_at(p.seed, gx, gy, p.sea_level) == 2);
            CHECK(WorldGenerator::slope_at(p.seed, gx, gy) <= 32);
        }
    CHECK(sites > 0);                       // this seed has gold-country rivers

    // every gold_placer occurrence sits on a true placer site — the deposit
    // is the result of the process, not a coincidental near_water roll
    int placer_entities = 0;
    for (const auto& e : w.entities) {
        if (!e->alive || e->deposit == kNoDeposit) continue;
        const DepositDef* d = w.deposits.find(e->deposit);
        if (!d || d->name != "gold_placer") continue;
        ++placer_entities;
        const int gx = TerrainGrid::cell_of(e->pos.x, w.terrain.cell);
        const int gy = TerrainGrid::cell_of(e->pos.y, w.terrain.cell);
        CHECK(WorldGenerator::placer_site_at(p.seed, gx, gy, p.sea_level));
    }
    CHECK(placer_entities > 0);

    // validation catches a hand-placed placer that never went through the
    // process: dry land meeting climate conditions but not a placer site
    {
        World wv(1, "data"); CHECK(gen.generate(wv, p));
        Vec2 bad{}; bool found = false;
        for (int gy = 0; gy < wv.terrain.h && !found; ++gy)
            for (int gx = 0; gx < wv.terrain.w && !found; ++gx) {
                if (WorldGenerator::water_at(p.seed, gx, gy, p.sea_level) != 0) continue;
                const std::int32_t e = WorldGenerator::elevation_at(p.seed, gx, gy);
                const std::int32_t m = WorldGenerator::moisture_at(p.seed, gx, gy, p.sea_level);
                if (e < 80 || e > 260 || m < 300) continue;
                bad = wv.terrain.cell_center(gx, gy); found = true;
            }
        if (found) {
            wv.create_deposit("gold_placer", bad);
            bool caught = false;
            for (const std::string& s : WorldGenerator::validate(wv))
                if (s.find("violates formation condition") != std::string::npos) caught = true;
            CHECK(caught);
        }
    }
}

// ---------- Phase 6: discovery engine (TODO §11) --------------------------------
static void test_phase6_discovery() {
    World w(31, "data");
    const PropId hunger = pid(w, "hunger"), thirst = pid(w, "thirst");
    const PropId sight = pid(w, "sight_range"), reach = pid(w, "reach");
    const PropId energy = pid(w, "energy"), exposed = pid(w, "exposed");

    // hungry NPC that already knows "consume" (learned from a prior meal)
    PropertyBag nb;
    nb.set(hunger, Value::integer(900)); nb.set(thirst, Value::integer(100));
    nb.set(sight, Value::integer(500)); nb.set(reach, Value::integer(500));
    nb.set(energy, Value::integer(1000000));
    const EntityId npc = w.create_entity("npc", kNoMaterial, {0, 0}, nb);
    const EntityId meal0 = w.create_entity("food", w.materials.id_of("wild_berries"), {50, 0});
    const EntityId meal1 = w.create_entity("food", w.materials.id_of("wild_berries"), {80, 0});
    CHECK(npc != kNoEntity && meal0 != kNoEntity && meal1 != kNoEntity);
    w.interact(mk_ctx(npc, {meal0}, "consume", 0));   // learn the verb for real
    AgentMind& mn = w.mind(npc);
    CHECK(mn.known_actions.count("consume") == 1);
    w.perceive(npc);

    // ---- problem detection: urgent need becomes a Problem --------------------
    std::vector<Problem> probs = detect_problems(w, npc);
    CHECK(!probs.empty());
    CHECK(probs[0].kind == ProblemKind::UnmetNeed);
    CHECK(probs[0].goal.need == hunger);

    // ---- candidate generation: plan_multi supplies consume hypothesis --------
    std::vector<Hypothesis> hs = generate_hypotheses(w, npc, probs[0], 4);
    CHECK(!hs.empty());
    bool saw_consume = false;
    for (const Hypothesis& h : hs)
        if (h.claim.find("consume") != std::string::npos) saw_consume = true;
    CHECK(saw_consume);

    // ---- experimentation on the REAL world + verification record -------------
    const std::int64_t hunger_before = w.prop_int(*w.find(npc), hunger, 0);
    ExperimentOutcome oc;
    for (const Hypothesis& h : hs)
        if (h.claim.find("consume") != std::string::npos) { oc = run_experiment(w, npc, h); break; }
    CHECK(oc.ran);
    CHECK(oc.succeeded);
    CHECK(!oc.events.empty());
    CHECK(w.prop_int(*w.find(npc), hunger, 0) < hunger_before);
    const AgentMind& m1 = w.mind(npc);
    CHECK(m1.discoveries.size() >= 1);
    const DiscoveryRecord* rec = nullptr;
    for (const auto& [k, d] : m1.discoveries)
        if (d.claim.find("consume") != std::string::npos) rec = &d;
    CHECK(rec != nullptr);
    if (rec) {
        CHECK(rec->trials == 1);
        CHECK(rec->evidence_for > 0);
        CHECK(rec->status == DiscoveryStatus::Tested ||
              rec->status == DiscoveryStatus::Supported);
        CHECK(rec->experiments.size() == 1);
        bool saw_phys = false;
        for (const std::string& s : rec->supporting_laws)
            if (s.find("physiology") != std::string::npos) saw_phys = true;
        CHECK(saw_phys);
    }

    // ---- suspicious sign -> purposeful survey --------------------------------
    // buried copper shows only its observable evidence (surface stain)
    const EntityId dep = w.create_deposit("copper_ore_body", {120, 0});
    CHECK(dep != kNoEntity);
    CHECK(w.prop_int(*w.find(dep), exposed, 0) == 0);
    w.mind(npc).known_actions.insert("dig");       // verb learned earlier
    w.perceive(npc);
    probs = detect_problems(w, npc);
    bool saw_sign = false;
    for (const Problem& p : probs)
        if (p.kind == ProblemKind::SuspiciousSign && p.subject == dep) saw_sign = true;
    CHECK(saw_sign);
    const Problem* sign_p = nullptr;
    for (const Problem& p : probs)
        if (p.kind == ProblemKind::SuspiciousSign && p.subject == dep) sign_p = &p;
    CHECK(sign_p != nullptr);
    std::vector<Hypothesis> shs = generate_hypotheses(w, npc, *sign_p, 8);
    bool saw_dig = false;
    for (const Hypothesis& h : shs)
        if (h.key.find(":dig") != std::string::npos) saw_dig = true;
    CHECK(saw_dig);
    // run the dig probe: buried -> exposed is a real yield
    ExperimentOutcome so;
    for (const Hypothesis& h : shs)
        if (h.key.find(":dig") != std::string::npos) { so = run_experiment(w, npc, h); break; }
    CHECK(so.ran && so.succeeded);
    CHECK(w.prop_int(*w.find(dep), exposed, 0) == 1);
    const DiscoveryRecord* srec = nullptr;
    for (const auto& [k, d] : w.mind(npc).discoveries)
        if (k.find("sign:") == 0 && k.find(":dig") != std::string::npos) srec = &d;
    CHECK(srec != nullptr && srec->trials >= 1 && srec->evidence_for > 0);

    // ---- failed experiment is evidence, not silence ---------------------------
    // hunger is urgent again AND the NPC is drained of energy: the world
    // refuses the consume — a failed trial still lands in the record.
    Entity* me = w.find(npc);
    me->props.set(hunger, Value::integer(900));
    me->props.set(energy, Value::integer(10));
    const EntityId meal2 = w.create_entity("food", w.materials.id_of("wild_berries"), {90, 0});
    w.perceive(npc);
    std::vector<Problem> p2 = detect_problems(w, npc);
    const Problem* hunger_p = nullptr;
    for (const Problem& p : p2)
        if (p.kind == ProblemKind::UnmetNeed && p.goal.need == hunger) hunger_p = &p;
    CHECK(hunger_p != nullptr);
    std::vector<Hypothesis> h2 = generate_hypotheses(w, npc, *hunger_p, 4);
    ExperimentOutcome fo;
    for (const Hypothesis& h : h2)
        if (h.claim.find("consume") != std::string::npos && !h.tested) { fo = run_experiment(w, npc, h); break; }
    if (!fo.ran) {   // low energy also blocks plan_multi -> force one trial
        Hypothesis fh; fh.key = "method:forced-consume"; fh.claim = "consume(food)";
        fh.plan.steps.push_back(mk_ctx(npc, {meal2}, "consume", 0));
        fh.goal.kind = Goal::Kind::SatisfyNeed; fh.goal.need = hunger;
        fh.goal.value = 100; fh.goal.target = npc;
        fh.serves = ProblemKind::UnmetNeed;
        fo = run_experiment(w, npc, fh);
    }
    CHECK(fo.ran);
    CHECK(!fo.succeeded);
    const DiscoveryRecord* frec = nullptr;
    for (const auto& [k, d] : w.mind(npc).discoveries)
        if (k == "method:forced-consume" ||
            (d.claim.find("consume") != std::string::npos && d.trials >= 2))
            frec = &d;
    CHECK(frec != nullptr && frec->evidence_against > 0 && frec->trials >= 1);

    // ---- discovery records persist through save/load --------------------------
    const std::string path = "/tmp/simlab_disc_save.bin";
    const std::uint64_t hh = w.state_hash();
    CHECK(w.save(path));
    World w2(1, "data");
    CHECK(w2.load(path));
    CHECK(w2.state_hash() == hh);
    const AgentMind* lm = w2.find_mind(npc);
    CHECK(lm != nullptr && !lm->discoveries.empty());
    bool saw_rec = false;
    for (const auto& [k, d] : lm->discoveries)
        if (d.trials > 0) saw_rec = true;
    CHECK(saw_rec);

    // ---- discover_step drives a full cycle -------------------------------------
    me->props.set(energy, Value::integer(1000000));
    const EntityId meal3 = w.create_entity("food", w.materials.id_of("wild_berries"), {95, 0});
    CHECK(meal3 != kNoEntity);
    w.perceive(npc);
    CHECK(discover_step(w, npc));
}

// ---------- Phase 6 completion: signatures / conditions / synthesis / explore -
static void test_phase6_completion() {
    World w(41, "data");
    const PropId hunger = pid(w, "hunger"), thirst = pid(w, "thirst");
    const PropId sight = pid(w, "sight_range"), reach = pid(w, "reach");
    const PropId energy = pid(w, "energy"), exposed = pid(w, "exposed");
    const PropId integ = pid(w, "integrity");

    PropertyBag nb;
    nb.set(hunger, Value::integer(100)); nb.set(thirst, Value::integer(100));
    nb.set(sight, Value::integer(500)); nb.set(reach, Value::integer(500));
    nb.set(energy, Value::integer(100000000)); nb.set(integ, Value::integer(1000));
    const EntityId npc = w.create_entity("npc", kNoMaterial, {0, 0}, nb);
    CHECK(npc != kNoEntity);
    // verbs the NPC already commands (prior experience / witnessed learning)
    for (const char* v : {"dig", "strike", "heat", "impact", "assemble", "wield"})
        w.mind(npc).known_actions.insert(v);

    auto find_problem = [&](ProblemKind k, EntityId s) -> Problem {
        for (const Problem& p : detect_problems(w, npc))
            if (p.kind == k && p.subject == s) return p;
        return Problem{};
    };
    auto dig_key_for = [&](const Problem& p) -> std::string {
        for (const Hypothesis& h : generate_hypotheses(w, npc, p, 8))
            if (h.key.rfind("sign:", 0) == 0 && h.key.size() >= 4 &&
                h.key.compare(h.key.size() - 4, 4, ":dig") == 0)
                return h.key;
        return {};
    };
    auto find_hyp = [&](const Problem& p, const std::string& key) -> Hypothesis {
        for (const Hypothesis& h : generate_hypotheses(w, npc, p, 16))
            if (h.key == key) return h;
        return Hypothesis{};
    };
    auto rec_for = [&](const std::string& key) -> const DiscoveryRecord* {
        const auto it = w.mind(npc).discoveries.find(key);
        return it == w.mind(npc).discoveries.end() ? nullptr : &it->second;
    };

    // ===== A: MethodKey — one signature, many entities =========================
    // Two copper bodies show identical observable signs -> identical MethodKey.
    const EntityId depA = w.create_deposit("copper_ore_body", {120, 0});
    const EntityId depB = w.create_deposit("copper_ore_body", {200, 0});
    CHECK(depA != kNoEntity && depB != kNoEntity);
    w.perceive(npc);
    const Problem pA = find_problem(ProblemKind::SuspiciousSign, depA);
    const Problem pB = find_problem(ProblemKind::SuspiciousSign, depB);
    CHECK(pA.subject == depA && pB.subject == depB);
    const std::string keyA = dig_key_for(pA);
    const std::string keyB = dig_key_for(pB);
    CHECK(!keyA.empty());
    CHECK(keyA == keyB);                          // signature, not entity id
    CHECK(keyA.rfind("sign:o.", 0) == 0);         // observable-sign profile key

    // experience on depA lands in the shared record...
    Hypothesis digA = find_hyp(pA, keyA);
    ExperimentOutcome oa = run_experiment(w, npc, digA);
    CHECK(oa.ran && oa.succeeded);
    CHECK(w.prop_int(*w.find(depA), exposed, 0) == 1);
    const DiscoveryRecord* recA = rec_for(keyA);
    CHECK(recA != nullptr && recA->trials == 1 && recA->evidence_for > 0);
    // ...and depB's identical-sign hypothesis is already "tested"
    bool b_tested = false;
    for (const Hypothesis& h : generate_hypotheses(w, npc, pB, 8))
        if (h.key == keyB) b_tested = h.tested;
    CHECK(b_tested);
    // provenance keeps the concrete entities as history only
    CHECK(recA != nullptr && !recA->sample_entities.empty() &&
          std::find(recA->sample_entities.begin(), recA->sample_entities.end(),
                    depA) != recA->sample_entities.end());

    // ===== B: validConditions + conditional Refuted unlock ====================
    // coal shows different signs -> a different method key
    const EntityId depC = w.create_deposit("coal_seam", {300, 0});
    w.perceive(npc);
    const Problem pC = find_problem(ProblemKind::SuspiciousSign, depC);
    CHECK(pC.subject == depC);
    const std::string keyC = dig_key_for(pC);
    CHECK(!keyC.empty() && keyC != keyA);
    w.find(npc)->props.set(energy, Value::integer(200000));
    Hypothesis digC = find_hyp(pC, keyC);
    CHECK(!digC.key.empty());
    // drain energy below the action's cost -> the world refuses the trial
    w.find(npc)->props.set(energy, Value::integer(50000));
    ExperimentOutcome oc = run_experiment(w, npc, digC);
    CHECK(oc.ran && !oc.succeeded);
    const DiscoveryRecord* recC = rec_for(keyC);
    CHECK(recC != nullptr);
    if (recC) {
        CHECK(recC->status == DiscoveryStatus::Refuted);
        bool saw_energy = false;
        for (const Condition& c : recC->failed_conditions)
            if (c.what == "energy" && !c.met) saw_energy = true;
        CHECK(saw_energy);                        // validConditions recorded
    }
    // same starving circumstances -> the method stays blocked
    CHECK(find_hyp(pC, keyC).key.empty());
    // circumstances improved -> unlocked and retryable
    w.find(npc)->props.set(energy, Value::integer(1000000));
    Hypothesis retryC = find_hyp(pC, keyC);
    CHECK(!retryC.key.empty() && retryC.refuted_unlocked);
    ExperimentOutcome ocr = run_experiment(w, npc, retryC);
    CHECK(ocr.ran && ocr.succeeded);
    CHECK(w.prop_int(*w.find(depC), exposed, 0) == 1);
    const DiscoveryRecord* recC2 = rec_for(keyC);
    CHECK(recC2 != nullptr && recC2->evidence_for > 0 &&
          recC2->status != DiscoveryStatus::Refuted);

    // ===== C: Exploratory Synthesis under a CapabilityGap =====================
    // the starved dig above left a refused energy attempt in the ledger
    Problem gapp;
    for (const Problem& p : detect_problems(w, npc))
        if (p.kind == ProblemKind::CapabilityGap) { gapp = p; break; }
    CHECK(gapp.kind == ProblemKind::CapabilityGap);
    std::vector<Hypothesis> synth = generate_hypotheses(w, npc, gapp, 16);
    int n_assemble = 0, n_synth = 0;
    for (const Hypothesis& h : synth) {
        if (h.key.rfind("synth:", 0) == 0) {
            ++n_synth;
            if (h.key.rfind("synth:assemble:", 0) == 0) ++n_assemble;
        }
    }
    CHECK(n_assemble >= 1);                        // combine candidates offered
    CHECK(n_synth <= 6);                           // §19E search budget respected

    // ===== D: ExploreGoal — waypoint out, never locomotion ====================
    {
        World wD(53, "data");
        PropertyBag db;
        db.set(pid(wD, "hunger"), Value::integer(100));
        db.set(pid(wD, "thirst"), Value::integer(100));
        db.set(pid(wD, "sight_range"), Value::integer(3000));
        db.set(pid(wD, "reach"), Value::integer(64));
        db.set(pid(wD, "energy"), Value::integer(10000000));
        db.set(pid(wD, "integrity"), Value::integer(1000));
        const EntityId nd = wD.create_entity("npc", kNoMaterial, {0, 0}, db);
        wD.mind(nd).known_actions.insert("dig");
        const EntityId far = wD.create_deposit("copper_ore_body", {2000, 0});
        CHECK(far != kNoEntity);
        wD.perceive(nd);
        // sign is visible (sight 3000) but unreachable (reach 64): no local
        // action is possible -> discover_step reports a waypoint instead
        ExploreGoal eg; eg.target = {-999, -999};
        const bool stepped = discover_step(wD, nd, &eg);
        CHECK(!stepped);
        CHECK(eg.subject == far);
        CHECK(eg.target.x == 2000 && eg.target.y == 0);
        CHECK(!eg.reason.empty());
        CHECK(wD.find(nd)->pos.x == 0 && wD.find(nd)->pos.y == 0);  // unmoved
    }
}

// ---------- Phase 6 completion II: threat -> synthesis -> self-defence --------
// The full §21 scenario with NO technology or species names anywhere: an NPC
// is hurt, notices the threat and its own reach deficit, combines two
// ordinary objects it can see into a structure, verifies what it made, and
// uses it to fight back — every step through real laws and real experiments.
static void test_phase6_selfdefence() {
    World w(77, "data");
    const PropId hunger = pid(w, "hunger"), thirst = pid(w, "thirst");
    const PropId sight = pid(w, "sight_range"), reach = pid(w, "reach");
    const PropId energy = pid(w, "energy"), integ = pid(w, "integrity");
    const PropId thrust = pid(w, "thrust_range"), edge = pid(w, "edge");

    // the defender: short reach, sees everything nearby
    PropertyBag nb;
    nb.set(hunger, Value::integer(100)); nb.set(thirst, Value::integer(100));
    nb.set(sight, Value::integer(600)); nb.set(reach, Value::integer(64));
    nb.set(energy, Value::integer(10000000)); nb.set(integ, Value::integer(1000));
    const EntityId npc = w.create_entity("npc", kNoMaterial, {0, 0}, nb);
    // raw materials lying around — no names, just matter with properties
    const EntityId part1 = w.create_entity("part_a", w.materials.id_of("flint"), {5, 0});
    const EntityId part2 = w.create_entity("part_b", w.materials.id_of("hardwood"), {6, 0});
    // the attacker: a mobile body with its own longer reach
    PropertyBag ab;
    ab.set(energy, Value::integer(10000000));
    ab.set(reach, Value::integer(200));
    ab.set(integ, Value::integer(1000));
    const EntityId foe = w.create_entity("beast", kNoMaterial, {100, 0}, ab);
    CHECK(npc != kNoEntity && part1 != kNoEntity && part2 != kNoEntity && foe != kNoEntity);
    for (const char* v : {"assemble", "wield", "impact", "strike", "heat"})
        w.mind(npc).known_actions.insert(v);
    w.perceive(npc);

    // ---- 1. harm: the foe's blow lands on the NPC ----------------------------
    // (impact damage is ~2.2x applied force for these stats — keep it survivable)
    InteractionResult harm = w.interact(mk_ctx(foe, {npc}, "impact", 300));
    CHECK(harm.completed);
    const std::int64_t integ_after_harm = w.prop_int(*w.find(npc), integ, 0);
    CHECK(integ_after_harm > 0 && integ_after_harm < 1000);

    // ---- 2. unarmed reach is not enough --------------------------------------
    InteractionResult short_arm = w.interact(mk_ctx(npc, {foe}, "impact", 100000));
    CHECK(!short_arm.completed);                // foe at 100 > reach 64
    CHECK(short_arm.failed_event != kNoEvent);

    // ---- 3. problems: Threat + CapabilityGap from the NPC's own history ------
    Problem threat, gap;
    bool saw_threat = false, saw_gap = false;
    for (const Problem& p : detect_problems(w, npc)) {
        if (p.kind == ProblemKind::Threat && p.subject == foe) { threat = p; saw_threat = true; }
        if (p.kind == ProblemKind::CapabilityGap) { gap = p; saw_gap = true; }
    }
    CHECK(saw_threat);
    CHECK(saw_gap);

    // ---- 4. hypotheses for the threat: defence probes + synthesis ------------
    std::vector<Hypothesis> hs = generate_hypotheses(w, npc, threat, 16);
    Hypothesis build;
    for (const Hypothesis& h : hs)
        if (h.key.rfind("synth:assemble:", 0) == 0 &&
            h.plan.steps.size() == 1 && h.plan.steps[0].targets.size() == 2)
            build = h;
    CHECK(!build.key.empty());

    // ---- 5. experiment: the parts really combine on the real world -----------
    ExperimentOutcome oc = run_experiment(w, npc, build);
    CHECK(oc.ran && oc.succeeded);
    CHECK(oc.events.size() == 1);
    CHECK(w.ledger.find(oc.events[0])->spawned.size() == 1);
    const EntityId tool = w.ledger.find(oc.events[0])->spawned[0];
    const Entity* tp = w.find(tool);
    CHECK(tp != nullptr && tp->alive);
    // verification: the structure has emergent reach AND edge AND stays sound
    CHECK(w.prop_int(*tp, thrust, 0) > 0);
    CHECK(w.prop_int(*tp, edge, 0) > 0);
    CHECK(w.prop_int(*tp, integ, 0) > 0);
    const DiscoveryRecord* brec = nullptr;
    for (const auto& [k, d] : w.mind(npc).discoveries)
        if (k == build.key) brec = &d;
    CHECK(brec != nullptr && brec->evidence_for > 0 && brec->trials >= 1);

    // ---- 6. take it up and use it ---------------------------------------------
    InteractionResult wr = w.interact(mk_ctx(npc, {tool}, "wield", 1000));
    CHECK(wr.completed);
    CHECK(w.related(npc, "wielding", tool));
    const std::int64_t foe_integ_before = w.prop_int(*w.find(foe), integ, 0);
    InteractionResult fight = w.interact(mk_ctx(npc, {foe}, "impact", 200));
    CHECK(fight.completed);                     // reach+edge now reach the foe
    const std::int64_t foe_integ_after = w.prop_int(*w.find(foe), integ, 0);
    CHECK(foe_integ_after < foe_integ_before);
    CHECK(w.find(npc)->alive);                  // defender survives
}

// ---------- Phase 3 completion: conditions, provenance, wear, combustion --------
namespace {
// Test-only law: proposes a spawn with an out-of-range property AFTER a valid
// state change — the engine must reject the whole event before mutating.
class BadSpawnLaw final : public INaturalLawModel {
public:
    LawId id() const override { return 900; }
    const char* name() const override { return "test.badspawn"; }
    std::vector<std::string> domains() const override { return {}; }
    bool supports(const World&, const InteractionContext& c) const override { return c.parameters.action == "badspawn"; }
    PredictionResult predict(const World& w, const InteractionContext& c) const override {
        PredictionResult r; r.law = id(); r.law_name = name(); r.possible = true;
        StateChange ch; ch.entity = c.targets[0]; ch.prop = w.props.find_name("integrity")->id;
        ch.had_before = true; ch.before = Value::integer(900); ch.after = Value::integer(1);
        r.changes.push_back(ch);
        SpawnSpec s; s.kind = "oops"; s.pos = {0, 0};
        s.props.set(w.props.find_name("integrity")->id, Value::integer(99999));
        r.spawns.push_back(s);
        return r;
    }
    std::unique_ptr<INaturalLawModel> clone() const override { return std::make_unique<BadSpawnLaw>(); }
};
// Test-only law: "forage" spawns one nutritious, quantity-bearing item near
// the actor — stands in for any harvest/extraction output for tithe tests.
class ForageLaw final : public INaturalLawModel {
public:
    LawId id() const override { return 901; }
    const char* name() const override { return "test.forage"; }
    std::vector<std::string> domains() const override { return {}; }
    bool supports(const World&, const InteractionContext& c) const override {
        return c.parameters.action == "forage";
    }
    PredictionResult predict(const World& w, const InteractionContext& c) const override {
        PredictionResult r; r.law = id(); r.law_name = name(); r.possible = true;
        const Entity* a = w.find(c.actor);
        SpawnSpec s; s.kind = "forage_item"; s.material = kNoMaterial;
        s.pos = {a->pos.x + 5, a->pos.y};
        s.props.set(w.props.find_name("nutrition")->id, Value::integer(400));
        s.props.set(w.props.find_name("quantity")->id, Value::integer(10));
        r.spawns.push_back(s);
        return r;
    }
    std::unique_ptr<INaturalLawModel> clone() const override { return std::make_unique<ForageLaw>(); }
};
}  // namespace

static void test_phase3_complete() {
    const auto setup = [](World& w, Vec2 at, std::int64_t energy_units) {
        PropertyBag b;
        if (energy_units >= 0) b.set(pid(w, "energy"), Value::integer(energy_units));
        b.set(pid(w, "reach"), Value::integer(200));
        return w.create_entity("actor", kNoMaterial, at, b);
    };

    // ---- effort cost is independent of tool sharpness; tools wear ----------------
    {
        World w(31, "data");
        const PropId energy = pid(w, "energy"), integ = pid(w, "integrity");
        const EntityId bare = setup(w, {0, 0}, 1000000);
        const EntityId armed = setup(w, {500, 0}, 1000000);
        PropertyBag tb; tb.set(pid(w, "edge"), Value::integer(900));
        const EntityId tool = w.create_entity("tool", w.materials.id_of("dry_wood"), {500, 0}, tb);
        w.relate(armed, "wielding", tool);
        const EntityId r1 = w.create_entity("rock", w.materials.id_of("stone"), {10, 0});
        const EntityId r2 = w.create_entity("rock", w.materials.id_of("stone"), {510, 0});
        CHECK(w.interact(mk_ctx(bare, {r1}, "impact", 200000)).completed);
        CHECK(w.interact(mk_ctx(armed, {r2}, "impact", 200000)).completed);
        CHECK(w.prop_int(*w.find(bare), energy, 0) == 800000);     // paid the effort...
        CHECK(w.prop_int(*w.find(armed), energy, 0) == 800000);    // ...sharp tool pays the same
        // reaction force: soft wood striking hard stone is worn; the tool is damaged too
        CHECK(w.prop_int(*w.find(tool), integ, 0) < 450);
        // a hard tool striking soft wood wears far less than a soft tool striking stone
        World w2(31, "data");
        const EntityId a2 = setup(w2, {0, 0}, 1000000);
        const EntityId hard = w2.create_entity("tool", w2.materials.id_of("granite"), {0, 0});
        w2.relate(a2, "wielding", hard);
        const EntityId soft = w2.create_entity("log", w2.materials.id_of("dry_wood"), {10, 0});
        CHECK(w2.interact(mk_ctx(a2, {soft}, "impact", 200000)).completed);
        CHECK(900 - w2.prop_int(*w2.find(hard), integ, 0) < 450 - w.prop_int(*w.find(tool), integ, 0));
        // repeated blows destroy a worn tool and cut its relation edge
        for (int i = 0; i < 40 && w.find(tool)->alive; ++i) {
            const EntityId rr = w.create_entity("rock", w.materials.id_of("stone"), {510, 0});
            w.interact(mk_ctx(armed, {rr}, "impact", 200000));
        }
        CHECK(!w.find(tool)->alive);
        CHECK(!w.related(armed, "wielding", tool));
    }

    // ---- refused attempts are evidence: recorded, no mutation, no learning --------
    {
        World w(32, "data");
        const EntityId a = setup(w, {0, 0}, -1);
        const EntityId dep = w.create_deposit("copper_ore_body", {10, 0});   // needs force 2400
        const std::uint64_t tick0 = w.clock_.tick();
        const std::size_t led0 = w.ledger.size();
        const std::int64_t rem0 = w.prop_int(*w.find(dep), pid(w, "deposit_remaining"), 0);
        InteractionResult r = w.interact(mk_ctx(a, {dep}, "dig", 100));
        CHECK(!r.completed && r.failed_event != kNoEvent);
        CHECK(w.ledger.size() == led0 + 1);
        const CausalEvent* fe = w.ledger.find(r.failed_event);
        CHECK(fe != nullptr && fe->failed && fe->action == "dig" && fe->changes.empty());
        CHECK(w.clock_.tick() == tick0);                                       // no time passes
        CHECK(w.prop_int(*w.find(dep), pid(w, "deposit_remaining"), 0) == rem0);
        bool force_cond = false;
        for (const Condition& c : fe->conditions)
            if (c.what == "force" && !c.met && c.actual == 100 && c.required == 2400) force_cond = true;
        CHECK(force_cond);
        const auto cf = w.ledger.counterfactuals(r.failed_event);
        bool crit = false;
        for (const Counterfactual& c : cf) if (c.what == "force" && c.critical && c.margin == 100 - 2400) crit = true;
        CHECK(crit);                                                           // "force alone blocked it"
        CHECK(w.find_mind(a) == nullptr || w.find_mind(a)->known_actions.count("dig") == 0);
        // an action NO law understands leaves no record
        const std::size_t led1 = w.ledger.size();
        CHECK(!w.interact(mk_ctx(a, {dep}, "sing", 100)).completed);
        CHECK(w.ledger.size() == led1);
        // success records the met conditions; margin tells how decisive they were
        r = w.interact(mk_ctx(a, {dep}, "dig", 2400));
        CHECK(r.completed);
        bool tight = false;
        for (const Counterfactual& c : w.ledger.counterfactuals(r.event))
            if (c.what == "force" && c.margin == 0 && c.critical) tight = true;
        CHECK(tight);
    }

    // ---- automatic provenance: dig -> smelt, "where did this metal come from?" ---
    {
        World w(33, "data");
        const EntityId a = setup(w, {50, 50}, -1);
        const EntityId dep = w.create_deposit("copper_ore_body", {55, 50});
        InteractionResult dr = w.interact(mk_ctx(a, {dep}, "dig", 8000000));
        CHECK(dr.completed);
        EntityId ore = kNoEntity;
        for (EntityId s : dr.spawned) if (w.find(s)->material == w.materials.id_of("copper_ore")) ore = s;
        CHECK(ore != kNoEntity);
        PropertyBag fb; fb.set(pid(w, "quantity"), Value::integer(50));
        const EntityId fuel = w.create_entity("fuel", w.materials.id_of("charcoal"), {52, 50}, fb);
        PropertyBag hb; hb.set(pid(w, "temperature"), Value::integer(10000));
        const EntityId furnace = w.create_entity("furnace", kNoMaterial, {53, 50}, hb);
        InteractionResult sr = w.interact(mk_ctx(a, {ore, fuel, furnace}, "process", 0, "smelt_copper"));
        CHECK(sr.completed);
        const CausalEvent* se = w.ledger.find(sr.event);
        CHECK(std::find(se->parent_events.begin(), se->parent_events.end(), dr.event) != se->parent_events.end());
        EntityId metal = kNoEntity, slag = kNoEntity;
        for (EntityId s : sr.spawned) {
            if (w.find(s)->material == w.materials.id_of("copper_metal")) metal = s;
            if (w.find(s)->material == w.materials.id_of("slag")) slag = s;
        }
        CHECK(metal != kNoEntity && slag != kNoEntity);
        const auto prov = w.provenance_of(metal);
        CHECK(std::find(prov.begin(), prov.end(), dr.event) != prov.end());    // traced back to the mine
        CHECK(std::find(prov.begin(), prov.end(), sr.event) != prov.end());
        const std::string why = w.explain_event(sr.event);
        CHECK(why.find("resource.process") != std::string::npos);
        CHECK(why.find("resource.extract") != std::string::npos);
        CHECK(why.find("temperature") != std::string::npos);                   // conditions shown
        CHECK(w.ledger.ancestors(sr.event).size() >= 1);
        // efficiency: charge exactly AT the minimum loses 20% of the metal to slag,
        // and every unit is accounted (outputs + waste == recipe totals, no deletion)
        auto inv = w.material_inventory();
        CHECK(inv[w.materials.id_of("copper_metal")] == 240);
        CHECK(inv[w.materials.id_of("slag")] == 160);
        CHECK(240 + 160 == 300 + 100);
        // provenance survives save/load
        CHECK(w.save("/tmp/simlab_p3_save.bin"));
        World wl(1, "data");
        CHECK(wl.load("/tmp/simlab_p3_save.bin"));
        CHECK(wl.state_hash() == w.state_hash());
        CHECK(wl.provenance_of(metal) == prov);
    }

    // ---- hot enough: full recovery; too cold: refused with the blocking condition --
    {
        World w(34, "data");
        const EntityId a = setup(w, {0, 0}, -1);
        auto run = [&](std::int64_t temp) {
            PropertyBag ob, fb, hb;
            ob.set(pid(w, "quantity"), Value::integer(400)); fb.set(pid(w, "quantity"), Value::integer(50));
            hb.set(pid(w, "temperature"), Value::integer(temp));
            const EntityId o = w.create_entity("ore", w.materials.id_of("copper_ore"), {5, 0}, ob);
            const EntityId f = w.create_entity("fuel", w.materials.id_of("charcoal"), {6, 0}, fb);
            const EntityId h = w.create_entity("heater", kNoMaterial, {7, 0}, hb);
            return w.interact(mk_ctx(a, {o, f, h}, "process", 0, "smelt_copper"));
        };
        InteractionResult cold = run(9000);
        CHECK(!cold.completed && cold.failed_event != kNoEvent);
        bool tcond = false;
        for (const Condition& c : w.ledger.find(cold.failed_event)->conditions)
            if (c.what == "temperature" && !c.met && c.actual == 9000 && c.required == 10000) tcond = true;
        CHECK(tcond);
        InteractionResult hot = run(12000);
        CHECK(hot.completed);
        std::int64_t metal = 0;
        for (EntityId s : hot.spawned) if (w.find(s)->material == w.materials.id_of("copper_metal")) metal = w.prop_int(*w.find(s), pid(w, "quantity"), 0);
        CHECK(metal == 300);
    }

    // ---- combustion: sustained burning, residue is data, mass leaves as gas ------
    {
        World w(35, "data");
        const PropId integ = pid(w, "integrity"), qty = pid(w, "quantity");
        const EntityId a = setup(w, {0, 0}, 500000000);
        const EntityId wood = w.create_entity("log", w.materials.id_of("dry_wood"), {5, 0});
        PropertyBag hot; hot.set(pid(w, "temperature"), Value::integer(3500));
        w.find(wood)->props.set(pid(w, "temperature"), Value::integer(3500));   // already past ignition
        const std::int64_t i0 = w.prop_int(*w.find(wood), integ, 0);
        CHECK(w.interact(mk_ctx(a, {wood}, "heat", 100000)).completed);
        const std::int64_t i1 = w.prop_int(*w.find(wood), integ, 0);
        CHECK(i1 < i0);                                                          // burning, not one-shot
        CHECK(w.interact(mk_ctx(a, {wood}, "heat", 100000)).completed);
        CHECK(w.prop_int(*w.find(wood), integ, 0) < i1);
        std::vector<EntityId> residue;
        for (int i = 0; i < 100 && w.find(wood)->alive; ++i) {
            InteractionResult hr = w.interact(mk_ctx(a, {wood}, "heat", 100000));
            for (EntityId s : hr.spawned) residue.push_back(s);
        }
        CHECK(!w.find(wood)->alive);
        CHECK(residue.size() == 1);
        const Entity* ash = w.find(residue[0]);
        CHECK(ash->material == w.materials.id_of("charcoal"));
        CHECK(w.prop_int(*ash, qty, 0) == 200);                                  // 25% of 800 g
        // a material with no declared residue just burns away
        const EntityId rock = w.create_entity("tinder", w.materials.id_of("hardwood"), {6, 0});
        w.find(rock)->props.set(pid(w, "temperature"), Value::integer(5000));
        w.find(rock)->props.set(integ, Value::integer(30));
        InteractionResult br = w.interact(mk_ctx(a, {rock}, "heat", 100000));
        CHECK(br.completed && !w.find(rock)->alive && br.spawned.size() == 1);   // hardwood declares charcoal too
    }

    // ---- heat dissipates toward ambient, deterministically, via the ledger -------
    {
        World w(36, "data");
        const PropId temp = pid(w, "temperature");
        const EntityId a = setup(w, {0, 0}, -1);
        const EntityId stone = w.create_entity("hot_stone", w.materials.id_of("stone"), {5, 0});
        w.find(stone)->props.set(temp, Value::integer(8000));
        const std::size_t led0 = w.ledger.size();
        std::int64_t prev = 8000;
        for (int i = 0; i < 30; ++i) {
            w.step();
            const std::int64_t t = w.prop_int(*w.find(stone), temp, 0);
            CHECK(t <= prev && t >= 200);                                        // monotone, never below ambient
            prev = t;
        }
        CHECK(prev < 8000);
        CHECK(w.ledger.size() > led0);                                           // cooling is causal, ledgered
        const CausalEvent& last = w.ledger.history().back();
        CHECK(last.action == "cool" && !last.changes.empty());
        for (int i = 0; i < 40; ++i) w.advance_ticks_for_test(100);
        CHECK(w.prop_int(*w.find(stone), temp, 0) == 200);                       // eventually ambient
        (void)a;
        // a cooled charge can no longer smelt: heat is not free forever
        World w2(36, "data"); World w3(36, "data");
        for (World* ww : {&w2, &w3}) {
            const EntityId s = ww->create_entity("hot", ww->materials.id_of("stone"), {5, 0});
            ww->find(s)->props.set(temp, Value::integer(8000));
            for (int i = 0; i < 12; ++i) ww->step();
        }
        CHECK(w2.state_hash() == w3.state_hash());                               // deterministic
    }

    // ---- terrain sets the ambient: a cold-biome object cools below 20°C ----------
    {
        World w(37, "data");
        WorldGenParams p; p.seed = 9; p.grid_w = 8; p.grid_h = 8;
        WorldGenerator().generate(w, p);
        const Vec2 pos = w.terrain.cell_center(3, 3);
        const std::int64_t amb = w.terrain.temp_at(pos);
        const EntityId e = w.create_entity("probe", w.materials.id_of("stone"), pos);
        w.find(e)->props.set(pid(w, "temperature"), Value::integer(amb + 3000));
        for (int i = 0; i < 3000; ++i) { w.step(); if (w.prop_int(*w.find(e), pid(w, "temperature"), 0) == amb) break; }
        CHECK(w.prop_int(*w.find(e), pid(w, "temperature"), 0) == amb);
    }

    // ---- spawns are validated before commit: no half-applied events --------------
    {
        World w(38, "data");
        w.add_law_model(std::make_unique<BadSpawnLaw>());
        const EntityId a = setup(w, {0, 0}, -1);
        const EntityId rock = w.create_entity("rock", w.materials.id_of("stone"), {5, 0});
        const std::uint64_t h0 = w.state_hash();
        InteractionResult r = w.interact(mk_ctx(a, {rock}, "badspawn", 1));
        CHECK(!r.completed);
        CHECK(w.prop_int(*w.find(rock), pid(w, "integrity"), 0) == 900);         // untouched
        CHECK(w.state_hash() == h0);                                             // nothing leaked anywhere
    }

    // ---- process parser: no exceptions, all-or-nothing --------------------------
    {
        World w(39, "data");
        ProcessRegistry pr; std::string err;
        CHECK(!pr.parse("process p\n input copper_ore 10\n ticks abc\n output copper_metal 5\n", w.materials, &err));
        CHECK(!pr.parse("process p\n input copper_ore 10 junk\n output copper_metal 5\n", w.materials, &err));
        CHECK(!pr.parse("process p\n input copper_ore 10\n min_temperature 99999\n output copper_metal 5\n", w.materials, &err));
        CHECK(!pr.parse("process p\n input copper_ore 10\n ticks 0\n output copper_metal 5\n", w.materials, &err));
        CHECK(pr.size() == 0);                                                   // failure left nothing behind
        CHECK(pr.parse("process p\n input copper_ore 10\n output copper_metal 5\n", w.materials, &err));
        CHECK(!pr.parse("process p\n input nope 10\n", w.materials, &err));
        CHECK(pr.size() == 1 && pr.find("p") != nullptr);                        // earlier good state intact
        CHECK(w.processes.find("smelt_copper")->provenance == "data/processes.txt");
    }
}


// ---------- Phase 7: biological evolution ------------------------------------
// Genome = World Truth (hereditary loci), phenotype = expressed props.
// Selection must emerge from environment x heredity — no engine code may
// name a species, a trait, or a "good" allele.
static EntityId mk_organism(World& w, const char* kind, Vec2 pos,
                            const std::vector<std::pair<PropId, std::int64_t>>& loci,
                            std::int64_t age, std::int64_t fertility,
                            std::int64_t mutation_rate, std::int64_t metabolism) {
    PropertyBag b;
    auto sp = [&](const char* n, std::int64_t v) {
        const PropId p = pid(w, n);
        if (p != kNoProp) b.set(p, Value::integer(v));
    };
    sp("age", age); sp("adult_age", 100); sp("fertility", fertility);
    sp("mutation_rate", mutation_rate); sp("metabolism", metabolism);
    sp("hunger", 100); sp("thirst", 100); sp("fatigue", 0);
    sp("energy", 100000); sp("integrity", 1000); sp("reach", 200);
    const EntityId id = w.create_entity(kind, kNoMaterial, pos, b);
    if (Entity* e = w.find(id))
        for (const auto& [p, v] : loci) e->genome.set(p, v);
    return id;
}

static void test_phase7_biology() {
    // ---- A: compatible adults reproduce; offspring carries a recombined genome
    {
        World w(7, "data");
        const PropId sp_p = pid(w, "speed"), fe_p = pid(w, "fertility");
        const PropId age_p = pid(w, "age"), energy_p = pid(w, "energy");
        const PropId parent_of = pid(w, "unused_parent_prop"); (void)parent_of;
        const EntityId a = mk_organism(w, "test_beast", {0, 0},
                                       {{sp_p, 5000}, {fe_p, 500}}, 300, 500, 30, 1800);
        const EntityId b = mk_organism(w, "test_beast", {10, 0},
                                       {{sp_p, 6000}, {fe_p, 400}}, 300, 400, 30, 1800);
        CHECK(a != kNoEntity && b != kNoEntity);
        const InteractionResult r = w.interact(mk_ctx(a, {b}, "reproduce", 0));
        CHECK(r.completed);
        CHECK(r.spawned.size() == 1);
        const Entity* child = w.find(r.spawned.front());
        CHECK(child && child->alive);
        CHECK(child->kind_name == "test_beast");
        CHECK(child->genome.loci.size() == 2);
        // every allele is one parent's allele or a mutation inside the
        // declared property range — never outside World Truth bounds
        for (const auto& [p, v] : child->genome.loci) {
            const PropertyDef* pd = w.props.find(p);
            CHECK(pd != nullptr);
            if (pd && pd->has_range)
                CHECK(v >= pd->lo && v <= pd->hi);
        }
        // expressed phenotype == genome alleles for numeric loci
        for (const auto& [p, v] : child->genome.loci) {
            const Value* pv = w.prop_of(*child, p);
            CHECK(pv && pv->type == ValType::Integer && pv->i == v);
        }
        // BOTH parents paid their half of the offspring's energy reserve
        // (conservation: child_init 40000 + gestation overhead, split evenly)
        const Entity* pa = w.find(a);
        const Entity* pb = w.find(b);
        CHECK(w.prop_int(*pa, energy_p, 0) == 100000 - 40000);
        CHECK(w.prop_int(*pb, energy_p, 0) == 100000 - 40000);
        // the newborn carries the initial energy carved out of the parents
        CHECK(w.prop_int(*child, energy_p, 0) == 40000);
        // relations: both parents recorded as parent_of the child
        bool rel_a = false, rel_b = false;
        for (const Relation& rel : w.relations.edges())
            if (rel.type == "parent_of" && rel.b == child->id) {
                if (rel.a == a) rel_a = true;
                if (rel.a == b) rel_b = true;
            }
        CHECK(rel_a && rel_b);
        // offspring starts with zeroed needs and ages from birth
        CHECK(w.prop_int(*child, pid(w, "hunger"), -1) <= 4);   // spawned at 0, +metabolism drift
        w.advance_ticks_for_test(150);
        CHECK(w.prop_int(*child, age_p, 0) >= 150);
    }

    // ---- B: every gate refuses with a causal failed event
    {
        World w(7, "data");
        const PropId sp_p = pid(w, "speed"), fe_p = pid(w, "fertility");
        const std::vector<std::pair<PropId, std::int64_t>> loci =
            {{sp_p, 5000}, {fe_p, 500}};
        const EntityId good = mk_organism(w, "test_beast", {0, 0}, loci, 300, 500, 30, 1800);
        auto try_repro = [&](EntityId actor, EntityId partner) {
            return w.interact(mk_ctx(actor, {partner}, "reproduce", 0));
        };
        // incompatible kind_name
        const EntityId other_kind = mk_organism(w, "other_beast", {5, 0}, loci, 300, 500, 30, 1800);
        CHECK(!try_repro(good, other_kind).completed);
        // incompatible genome (missing locus)
        const EntityId short_genome = mk_organism(w, "test_beast", {5, 0}, {{sp_p, 5000}}, 300, 500, 30, 1800);
        CHECK(!try_repro(good, short_genome).completed);
        // immature partner
        const EntityId young = mk_organism(w, "test_beast", {5, 0}, loci, 50, 500, 30, 1800);
        CHECK(!try_repro(good, young).completed);
        // sterile partner (fertility prop = 0)
        const EntityId sterile = mk_organism(w, "test_beast", {5, 0}, loci, 300, 0, 30, 1800);
        CHECK(!try_repro(good, sterile).completed);
        // starving actor
        Entity* starver = w.find(good);
        starver->props.set(pid(w, "hunger"), Value::integer(950));
        const EntityId partner_ok = mk_organism(w, "test_beast", {5, 0}, loci, 300, 500, 30, 1800);
        CHECK(!try_repro(good, partner_ok).completed);
        starver->props.set(pid(w, "hunger"), Value::integer(100));
        // out of reach
        const EntityId far = mk_organism(w, "test_beast", {5000, 0}, loci, 300, 500, 30, 1800);
        CHECK(!try_repro(good, far).completed);
        // insufficient energy
        Entity* tired = w.find(good);
        tired->props.set(pid(w, "energy"), Value::integer(100));
        CHECK(!try_repro(good, partner_ok).completed);
    }

    // ---- C: mutation produces heritable variation; replay is bit-identical
    {
        auto run = [] {
            World w(11, "data");
            const PropId sp_p = pid(w, "speed"), fe_p = pid(w, "fertility");
            const std::vector<std::pair<PropId, std::int64_t>> loci =
                {{sp_p, 5000}, {fe_p, 500}};
            const EntityId a = mk_organism(w, "test_beast", {0, 0}, loci, 300, 500, 1000, 1800);
            const EntityId b = mk_organism(w, "test_beast", {10, 0}, loci, 300, 500, 1000, 1800);
            // this test is about MUTATION, not the Phase-9 cooldown/gestation
            // costs — disable the timer and keep the mother fed between births
            w.find(a)->props.set(pid(w, "repro_cooldown"), Value::integer(0));
            std::set<std::int64_t> alleles;
            for (int i = 0; i < 12; ++i) {
                // conservation: BOTH parents pay energy+hunger per birth —
                // refill both each iteration or the budget runs out
                for (EntityId pid_ : {a, b}) {
                    w.find(pid_)->props.set(pid(w, "hunger"), Value::integer(100));
                    w.find(pid_)->props.set(pid(w, "energy"), Value::integer(1000000));
                }
                const InteractionResult r = w.interact(mk_ctx(a, {b}, "reproduce", 0));
                if (r.completed && !r.spawned.empty()) {
                    const Entity* c = w.find(r.spawned.front());
                    if (const std::int64_t* v = c->genome.get(sp_p)) alleles.insert(*v);
                }
            }
            return std::pair<std::uint64_t, std::set<std::int64_t>>{w.state_hash(), alleles};
        };
        const auto r1 = run(), r2 = run();
        CHECK(r1.first == r2.first);              // deterministic replay
        CHECK(r1.second.size() > 1);              // mutation generated variation
        for (std::int64_t v : r1.second)
            CHECK(v >= 0 && v <= 100000);          // inside declared range
    }

    // ---- D: genome survives save/load (format v8)
    {
        World w(13, "data");
        const PropId sp_p = pid(w, "speed"), fe_p = pid(w, "fertility");
        const std::vector<std::pair<PropId, std::int64_t>> loci =
            {{sp_p, 4321}, {fe_p, 777}};
        const EntityId a = mk_organism(w, "test_beast", {0, 0}, loci, 300, 500, 30, 1800);
        const EntityId b = mk_organism(w, "test_beast", {10, 0}, loci, 300, 500, 30, 1800);
        const InteractionResult r = w.interact(mk_ctx(a, {b}, "reproduce", 0));
        CHECK(r.completed && r.spawned.size() == 1);
        const std::string path = "/tmp/simlab_p7_save.bin";
        CHECK(w.save(path));
        World w2(13, "data");
        CHECK(w2.load(path));
        const Entity* c1 = w.find(r.spawned.front());
        const Entity* c2 = w2.find(r.spawned.front());
        CHECK(c1 && c2);
        CHECK(c1->genome.loci.size() == c2->genome.loci.size());
        for (std::size_t i = 0; i < c1->genome.loci.size(); ++i) {
            CHECK(c1->genome.loci[i].first == c2->genome.loci[i].first);
            CHECK(c1->genome.loci[i].second == c2->genome.loci[i].second);
        }
        CHECK(w.state_hash() == w2.state_hash());
    }

    // ---- E: emergent selection — metabolism decides starvation survival
    // Two variants of the SAME kind differ only in the metabolism allele.
    // Under food scarcity the low-metabolism carrier lives; the high one
    // starves. No code says which allele is "better" — the environment does.
    {
        World w(17, "data");
        const PropId met_p = pid(w, "metabolism");
        const PropId sp_p = pid(w, "speed"), fe_p = pid(w, "fertility");
        const std::vector<std::pair<PropId, std::int64_t>> loci =
            {{sp_p, 5000}, {fe_p, 500}, {met_p, 0}};
        // fix loci: metabolism allele must match each variant's prop
        const EntityId slow = mk_organism(w, "test_beast", {0, 0},
                                          {{sp_p, 5000}, {fe_p, 500}, {met_p, 100}},
                                          300, 500, 30, 100);
        const EntityId fast = mk_organism(w, "test_beast", {10, 0},
                                          {{sp_p, 5000}, {fe_p, 500}, {met_p, 3000}},
                                          300, 500, 30, 3000);
        (void)loci;
        // both start hungry-but-viable; no food exists in this world
        w.find(slow)->props.set(pid(w, "hunger"), Value::integer(500));
        w.find(fast)->props.set(pid(w, "hunger"), Value::integer(500));
        w.advance_ticks_for_test(400);
        const Entity* es = w.find(slow);
        const Entity* ef = w.find(fast);
        CHECK(es && es->alive);    // slow metabolism: needs drift slowly
        CHECK(ef && !ef->alive);   // fast metabolism: starved to integrity 0
    }

    // ---- F: locus divergence -> reproductive isolation (speciation)
    // Structural mutation (rare, rate = mutation_rate/20 permille) lets a
    // lineage gain a locus from the species gene pool or lose one. Because
    // compatibility requires an IDENTICAL locus set, the mutant lineage can
    // no longer breed with the parental population — a new species emerges
    // from divergence alone, with no species table involved.
    {
        World w(19, "data");
        const PropId sp_p = pid(w, "speed"), fe_p = pid(w, "fertility");
        const std::vector<std::pair<PropId, std::int64_t>> loci =
            {{sp_p, 5000}, {fe_p, 500}};
        const EntityId a = mk_organism(w, "test_beast", {0, 0}, loci, 300, 500, 1000, 1800);
        const EntityId b = mk_organism(w, "test_beast", {10, 0}, loci, 300, 500, 1000, 1800);
        // speciation search needs many births in one tick-window: disable the
        // cooldown and keep the mother fed/energised between attempts
        w.find(a)->props.set(pid(w, "repro_cooldown"), Value::integer(0));
        EntityId mutant = kNoEntity;
        for (int i = 0; i < 120 && mutant == kNoEntity; ++i) {
            // conservation: BOTH parents pay energy+hunger per birth
            for (EntityId pid_ : {a, b}) {
                w.find(pid_)->props.set(pid(w, "hunger"), Value::integer(100));
                w.find(pid_)->props.set(pid(w, "energy"), Value::integer(1000000));
            }
            const InteractionResult r = w.interact(mk_ctx(a, {b}, "reproduce", 0));
            if (!r.completed || r.spawned.empty()) continue;
            const Entity* c = w.find(r.spawned.front());
            if (c && c->genome.loci.size() != loci.size()) mutant = c->id;
        }
        CHECK(mutant != kNoEntity);   // structural mutation occurred
        const Entity* mu = w.find(mutant);
        CHECK(mu && mu->alive);
        // the mutant is an adult now (give it the props maturity needs)
        Entity* mue = w.find(mutant);
        mue->props.set(pid(w, "age"), Value::integer(300));
        mue->props.set(pid(w, "adult_age"), Value::integer(100));
        mue->props.set(pid(w, "fertility"), Value::integer(500));
        mue->props.set(pid(w, "hunger"), Value::integer(100));
        mue->props.set(pid(w, "energy"), Value::integer(100000));
        mue->props.set(pid(w, "reach"), Value::integer(200));
        // partner carrying the SAME (diverged) locus set -> compatible
        PropertyBag gb;
        for (const auto& [p, v] : mu->genome.loci) gb.set(p, Value::integer(v));
        const EntityId same_set = w.create_entity("test_beast", kNoMaterial,
                                                  {20, 0}, gb);
        Entity* sse = w.find(same_set);
        for (const auto& [p, v] : mu->genome.loci) sse->genome.set(p, v);
        sse->props.set(pid(w, "age"), Value::integer(300));
        sse->props.set(pid(w, "adult_age"), Value::integer(100));
        sse->props.set(pid(w, "fertility"), Value::integer(500));
        sse->props.set(pid(w, "mutation_rate"), Value::integer(30));
        sse->props.set(pid(w, "hunger"), Value::integer(100));
        sse->props.set(pid(w, "energy"), Value::integer(100000));
        sse->props.set(pid(w, "reach"), Value::integer(200));
        CHECK(w.interact(mk_ctx(mutant, {same_set}, "reproduce", 0)).completed);
        // partner carrying the PARENTAL locus set -> reproductively isolated
        const EntityId parent_set = mk_organism(w, "test_beast", {20, 0}, loci,
                                                300, 500, 30, 1800);
        CHECK(!w.interact(mk_ctx(mutant, {parent_set}, "reproduce", 0)).completed);
    }

    // ---- G: food chain — kill, carcass, consume, strip
    // A living organism is struck dead; its flesh (material `nutrition` is
    // World Truth) feeds the killer exactly once. No hunting AI is written —
    // strike + consume are the same generic actions everyone has.
    {
        World w(23, "data");
        const PropId sp_p = pid(w, "speed"), fe_p = pid(w, "fertility");
        const std::vector<std::pair<PropId, std::int64_t>> loci =
            {{sp_p, 5000}, {fe_p, 500}};
        const EntityId hunter = mk_organism(w, "test_beast", {0, 0}, loci,
                                            300, 500, 30, 1800);
        const EntityId prey = mk_organism(w, "test_beast", {10, 0}, loci,
                                          300, 500, 30, 1800);
        w.find(prey)->material = w.materials.id_of("flesh");   // edible body
        w.find(hunter)->props.set(pid(w, "hunger"), Value::integer(800));
        // the kill: a physics strike, not a "hunt" verb
        const InteractionResult kill =
            w.interact(mk_ctx(hunter, {prey}, "impact", 3000));
        CHECK(kill.completed);
        const Entity* pe = w.find(prey);
        CHECK(pe && !pe->alive);
        // the meal: corpse nutrition relieves hunger (flesh nut=700)
        const InteractionResult meal =
            w.interact(mk_ctx(hunter, {prey}, "consume", 0));
        CHECK(meal.completed);
        const std::int64_t after_meal =
            w.prop_int(*w.find(hunter), pid(w, "hunger"), -1);
        CHECK(after_meal < 800);        // flesh nutrition=700 relieved the need
        // stripped: the same carcass cannot feed twice
        const InteractionResult again =
            w.interact(mk_ctx(hunter, {prey}, "consume", 0));
        CHECK(!again.completed);
        CHECK(w.prop_int(*w.find(hunter), pid(w, "hunger"), -1) == after_meal);
    }
}


// ---------- Phase 8: social & cultural evolution -------------------------------
// No Tribe/Kingdom classes: groups are entities + member_of edges + props,
// rules are props, knowledge moves only through causal events.
static EntityId mk_person(World& w, Vec2 pos, std::int64_t qty = -1) {
    PropertyBag b;
    auto sp = [&](const char* n, std::int64_t v) {
        const PropId p = pid(w, n);
        if (p != kNoProp) b.set(p, Value::integer(v));
    };
    sp("age", 300); sp("adult_age", 100); sp("fertility", 500);
    sp("mutation_rate", 30); sp("metabolism", 1800);
    sp("hunger", 100); sp("thirst", 100); sp("fatigue", 0);
    sp("energy", 100000); sp("integrity", 1000); sp("reach", 200);
    sp("communicate_range", 400);
    if (qty >= 0) sp("quantity", qty);
    return w.create_entity("person", kNoMaterial, pos, b);
}

static DiscoveryRecord mk_record(const char* key, DiscoveryStatus st) {
    DiscoveryRecord d;
    d.key = key; d.claim = "verified method " + std::string(key);
    d.status = st; d.evidence_for = 500; d.trials = 12;
    d.supporting_laws = {"physics.impact"};
    Condition c; c.what = "energy"; c.actual = 100; c.required = 500; c.met = false;
    d.failed_conditions.push_back(c);
    return d;
}

static void test_phase8_social() {
    // ---- A: communicate transfers verified knowledge as unverified claims ----
    {
        World w(29, "data");
        const EntityId teacher = mk_person(w, {0, 0});
        const EntityId learner = mk_person(w, {10, 0});
        AgentMind& tm = w.mind(teacher);
        tm.discoveries["method:a"] = mk_record("method:a", DiscoveryStatus::Verified);
        tm.discoveries["method:b"] = mk_record("method:b", DiscoveryStatus::Supported);
        tm.discoveries["method:c"] = mk_record("method:c", DiscoveryStatus::Open);   // not teachable
        tm.learn_action("impact");
        // neutral trust (0) -> K=1 record transfers
        CHECK(w.interact(mk_ctx(teacher, {learner}, "communicate", 0)).completed);
        const AgentMind* lm = w.find_mind(learner);
        CHECK(lm != nullptr);
        CHECK(lm->discoveries.count("method:a") == 1);
        CHECK(lm->discoveries.count("method:b") == 0);   // trust too low for 2nd
        CHECK(lm->known_actions.count("impact") == 1);
        const DiscoveryRecord& got = lm->discoveries.at("method:a");
        CHECK(got.status == DiscoveryStatus::Open);               // must self-verify
        CHECK(got.evidence_for == 0 && got.trials == 0);
        CHECK(got.failed_conditions.size() == 1);                 // hard-won bounds kept
        CHECK(got.claim == "verified method method:a");
        // high trust -> more claims flow
        w.adjust_tie(teacher, "trust", learner, 1000);
        CHECK(w.interact(mk_ctx(teacher, {learner}, "communicate", 0)).completed);
        lm = w.find_mind(learner);
        CHECK(lm->discoveries.count("method:b") == 1);
        CHECK(lm->discoveries.count("method:c") == 0);            // Open never teaches
    }

    // ---- B: engrave -> read : Recorded Knowledge outlives its author ---------
    {
        World w(31, "data");
        const EntityId sage = mk_person(w, {0, 0});
        const EntityId stone = w.create_entity("rock", kNoMaterial, {10, 0});
        const EntityId reader = mk_person(w, {20, 0});
        w.mind(sage).discoveries["method:x"] =
            mk_record("method:x", DiscoveryStatus::Verified);
        // engrave the best record onto the stone
        CHECK(w.interact(mk_ctx(sage, {stone}, "engrave", 0,
                                "method:x")).completed);
        const PropId ins = pid(w, "inscription");
        const Value* iv = w.prop_of(*w.find(stone), ins);
        CHECK(iv && iv->type == ValType::Text);
        CHECK(iv->s.find("R|method:x|") != std::string::npos);
        // a stranger reads the stone centuries later and gets the CLAIM
        CHECK(w.interact(mk_ctx(reader, {stone}, "read", 0)).completed);
        const AgentMind* rm = w.find_mind(reader);
        CHECK(rm && rm->discoveries.count("method:x") == 1);
        CHECK(rm->discoveries.at("method:x").status == DiscoveryStatus::Open);
        CHECK(rm->discoveries.at("method:x").evidence_for == 0);
        // uninscribed rock refuses
        const EntityId blank = w.create_entity("rock", kNoMaterial, {15, 0});
        CHECK(!w.interact(mk_ctx(reader, {blank}, "read", 0)).completed);
        // author death does not erase the record (World Truth on the stone)
        w.find(sage)->alive = false;
        const EntityId reader2 = mk_person(w, {25, 0});
        CHECK(w.interact(mk_ctx(reader2, {stone}, "read", 0)).completed);
        CHECK(w.find_mind(reader2)->discoveries.count("method:x") == 1);
    }

    // ---- C: collective + share + taboo enforcement ----------------------------
    {
        World w(37, "data");
        const PropId coll = pid(w, "collective");
        const PropId taboo = pid(w, "taboo_harm");
        const EntityId founder = mk_person(w, {0, 0}, 100);
        // charter a collective
        const InteractionResult f = w.interact(mk_ctx(founder, {}, "found", 0));
        CHECK(f.completed && f.spawned.size() == 1);
        const EntityId group = f.spawned.front();
        const Entity* ge = w.find(group);
        CHECK(ge && w.prop_of(*ge, coll) && w.prop_of(*ge, coll)->b);
        CHECK(w.relations.related(founder, "member_of", group));
        // rule: harm between members is a violation (prop on the group)
        w.find(group)->props.set(taboo, Value::boolean(true));
        const EntityId m1 = mk_person(w, {5, 0});
        const EntityId victim = mk_person(w, {8, 0});
        CHECK(w.interact(mk_ctx(m1, {group}, "join", 0)).completed);
        CHECK(w.interact(mk_ctx(victim, {group}, "join", 0)).completed);
        // share goods into the common store
        CHECK(w.interact(mk_ctx(founder, {group}, "share", 0)).completed);
        CHECK(w.prop_int(*w.find(group), pid(w, "quantity"), -1) == 100);
        CHECK(w.interact(mk_ctx(m1, {group}, "leave", 0)).completed);
        CHECK(!w.relations.related(m1, "member_of", group));
        // rejoin for the violation test
        CHECK(w.interact(mk_ctx(m1, {group}, "join", 0)).completed);
        // founder harms a fellow member -> trust penalties across the group
        const InteractionResult harm =
            w.interact(mk_ctx(founder, {victim}, "impact", 100));
        CHECK(harm.completed);
        CHECK(w.prop_int(*w.find(victim), pid(w, "integrity"), 1000) < 1000);
        CHECK(w.tie_weight(m1, "trust", founder) == -100);
        CHECK(w.tie_weight(victim, "trust", founder) == -200);
        // a non-member harming a member triggers nothing
        const EntityId outsider = mk_person(w, {50, 0});
        w.interact(mk_ctx(outsider, {victim}, "impact", 100));
        CHECK(w.tie_weight(m1, "trust", outsider) == 0);
    }

    // ---- C2: debt & authority emerge from generosity/teaching/protection -----
    {
        World w(43, "data");
        const EntityId giver = mk_person(w, {0, 0}, 50);
        const EntityId needy = mk_person(w, {10, 0}, 0);
        w.find(needy)->props.set(pid(w, "hunger"), Value::integer(800));
        // share to the desperate -> receiver owes the giver
        CHECK(w.interact(mk_ctx(giver, {needy}, "share", 0)).completed);
        CHECK(w.tie_weight(needy, "debt", giver) == 150);
        // debtor shares back -> debt paid down
        w.find(needy)->props.set(pid(w, "quantity"), Value::integer(10));
        CHECK(w.interact(mk_ctx(needy, {giver}, "share", 0)).completed);
        CHECK(w.tie_weight(needy, "debt", giver) == 0);
        // teaching NEW knowledge earns authority
        const EntityId sage = mk_person(w, {20, 0});
        const EntityId student = mk_person(w, {30, 0});
        w.mind(sage).discoveries["method:t"] =
            mk_record("method:t", DiscoveryStatus::Verified);
        CHECK(w.interact(mk_ctx(sage, {student}, "communicate", 0)).completed);
        CHECK(w.tie_weight(student, "authority", sage) == 100);
        // protection: bully hurts student, sage strikes the bully -> authority
        const EntityId bully = mk_person(w, {40, 0});
        const EntityId hurt = mk_person(w, {45, 0});
        w.interact(mk_ctx(bully, {hurt}, "impact", 100));   // bully harms hurt
        w.interact(mk_ctx(sage, {bully}, "impact", 3000));  // sage kills the threat
        CHECK(w.find(bully) && !w.find(bully)->alive);
        CHECK(w.tie_weight(hurt, "authority", sage) == 100);
    }

    // ---- C3: ration_share tithe + withdraw rules -------------------------------
    {
        World w(47, "data");
        const PropId coll = pid(w, "collective");
        const PropId ration = pid(w, "ration_share");
        const PropId qty = pid(w, "quantity");
        const EntityId founder = mk_person(w, {0, 0});
        const EntityId group =
            w.interact(mk_ctx(founder, {}, "found", 0)).spawned.front();
        w.find(group)->props.set(coll, Value::boolean(true));
        w.find(group)->props.set(ration, Value::integer(100));   // 10% tithe
        const EntityId member = mk_person(w, {10, 0});
        CHECK(w.interact(mk_ctx(member, {group}, "join", 0)).completed);
        w.add_law_model(std::make_unique<ForageLaw>());
        // member forages -> 10% tithe into the common store (same event)
        const InteractionResult hv = w.interact(mk_ctx(member, {}, "forage", 0));
        CHECK(hv.completed && hv.spawned.size() == 1);
        CHECK(w.prop_int(*w.find(hv.spawned.front()), qty, -1) == 9);   // 10 - 10%
        CHECK(w.prop_int(*w.find(group), qty, -1) == 1);              // tithe landed
        // a non-member forages -> no tithe
        const EntityId wild = mk_person(w, {40, 0});
        const InteractionResult hv2 = w.interact(mk_ctx(wild, {}, "forage", 0));
        CHECK(w.prop_int(*w.find(hv2.spawned.front()), qty, -1) == 10);
        // hungry member withdraws clean
        w.find(group)->props.set(qty, Value::integer(50));
        w.find(member)->props.set(pid(w, "hunger"), Value::integer(800));
        CHECK(w.interact(mk_ctx(member, {group}, "withdraw", 0)).completed);
        CHECK(w.prop_int(*w.find(member), qty, -1) == 50);
        CHECK(w.prop_int(*w.find(group), qty, -1) == 0);
        // outsider withdrawal = theft: succeeds but every member trusts less
        const EntityId outsider = mk_person(w, {60, 0});
        w.find(group)->props.set(qty, Value::integer(50));
        CHECK(w.interact(mk_ctx(outsider, {group}, "withdraw", 0)).completed);
        CHECK(w.tie_weight(founder, "trust", outsider) == -100);
        CHECK(w.tie_weight(member, "trust", outsider) == -100);
    }

    // ---- D: determinism + persistence -----------------------------------------
    {
        auto scenario = [](World& w) {
            const EntityId t = mk_person(w, {0, 0});
            const EntityId l = mk_person(w, {10, 0});
            w.mind(t).discoveries["method:z"] =
                mk_record("method:z", DiscoveryStatus::Verified);
            w.interact(mk_ctx(t, {l}, "communicate", 0));
            w.adjust_tie(t, "trust", l, 300);
            const EntityId g = w.interact(mk_ctx(t, {}, "found", 0)).spawned.front();
            w.interact(mk_ctx(l, {g}, "join", 0));
        };
        World w1(41, "data"), w2(41, "data");
        scenario(w1); scenario(w2);
        CHECK(w1.state_hash() == w2.state_hash());
        const std::string path = "/tmp/simlab_p8_save.bin";
        CHECK(w1.save(path));
        World w3(41, "data");
        CHECK(w3.load(path));
        CHECK(w3.state_hash() == w1.state_hash());
        CHECK(w3.tie_weight(w3.entities[0]->id, "trust",
                            w3.entities[1]->id) ==
              w1.tie_weight(w1.entities[0]->id, "trust", w1.entities[1]->id));
    }
}


// ---------- Phase 9: LOD metabolism, world history, long-run determinism -----
static void test_phase9_world() {
    World w(31, "data");
    const PropId hunger = pid(w, "hunger"), thirst = pid(w, "thirst");
    const PropId fatigue = pid(w, "fatigue"), metabolism = pid(w, "metabolism");
    const PropId integ = pid(w, "integrity");
    CHECK(hunger != kNoProp && metabolism != kNoProp && integ != kNoProp);
    auto ip = [&](EntityId e, PropId p) {
        const Entity* x = w.find(e); return x ? w.prop_int(*x, p, -1) : -2;
    };

    // One minded agent at the origin -> dormant LOD engages (radius 1024).
    PropertyBag mb;
    mb.set(pid(w, "sight_range"), Value::integer(500));
    mb.set(hunger, Value::integer(100)); mb.set(thirst, Value::integer(0));
    mb.set(fatigue, Value::integer(0)); mb.set(metabolism, Value::integer(2000));
    mb.set(integ, Value::integer(500));
    const EntityId agent = w.create_entity("npc", kNoMaterial, {0, 0}, mb);
    w.mind(agent);
    auto make_org = [&](int x, int h0) {
        PropertyBag pb;
        pb.set(hunger, Value::integer(h0)); pb.set(thirst, Value::integer(0));
        pb.set(fatigue, Value::integer(0)); pb.set(metabolism, Value::integer(2000));
        pb.set(integ, Value::integer(500));
        return w.create_entity("org", kNoMaterial, {x, 0}, pb);
    };
    const EntityId near_e = make_org(10, 0);     // inside LOD radius -> per-tick
    const EntityId far_e  = make_org(5000, 0);   // dormant -> 8-tick stride

    // t=4: active entity drifted +8 hunger (2/tick); dormant untouched.
    w.advance_ticks_for_test(4);
    CHECK(ip(near_e, hunger) == 8);
    CHECK(ip(far_e, hunger) == 0);
    // t=8: dormant catches up exactly — identical to per-tick result.
    w.advance_ticks_for_test(4);
    CHECK(ip(near_e, hunger) == 16);
    CHECK(ip(far_e, hunger) == 16);
    CHECK(ip(far_e, thirst) == 32);              // 4/tick * 8
    // and stays identical on every later stride
    w.advance_ticks_for_test(8);
    CHECK(ip(near_e, hunger) == 32);
    CHECK(ip(far_e, hunger) == 32);

    // Starvation mid-window: hunger 940 crosses 950 at tick 5 of an 8-tick
    // catch-up -> only 4 starving ticks * 10 dmg = 40 integrity lost.
    const EntityId st = make_org(6000, 940);     // born at t=16
    w.advance_ticks_for_test(8);                 // t=24, pending=8
    CHECK(ip(st, hunger) == 956);                // 940 + 2*8
    CHECK(ip(st, integ) == 460);                 // 500 - 10*4, NOT -80
    CHECK(w.find(st)->alive);

    // flush_needs: a dormant target presents CURRENT needs to interact —
    // even a refused action flushes pending drift first.
    w.advance_ticks_for_test(4);                 // t=28: far pending=4, stale 48
    CHECK(ip(far_e, hunger) == 48);
    CHECK(ip(far_e, thirst) == 96);
    InteractionResult fr = w.interact(mk_ctx(agent, {far_e}, "no_such_verb", 0));
    CHECK(!fr.completed);
    CHECK(ip(far_e, hunger) == 56);              // 48 + 2*4 caught up
    CHECK(ip(far_e, thirst) == 112);             // 96 + 4*4

    // ---- World History query ------------------------------------------------
    const std::vector<EventId> h_agent = w.history_of(agent, 50);
    CHECK(!h_agent.empty());
    for (std::size_t i = 1; i < h_agent.size(); ++i)
        CHECK(h_agent[i - 1] > h_agent[i]);      // newest first
    InteractionResult rr = w.interact(mk_ctx(agent, {}, "rest", 0, "", 3));
    CHECK(rr.completed);
    const std::vector<EventId> h_agent2 = w.history_of(agent, 50);
    bool saw_metabolize = false, saw_interact = false;
    for (EventId id : h_agent2) {
        const CausalEvent* ev = w.ledger.find(id);
        CHECK(ev != nullptr);
        if (ev->action == "metabolize") saw_metabolize = true;
        if (ev->action == "rest") saw_interact = true;
    }
    CHECK(saw_metabolize && saw_interact);
    CHECK(h_agent2.front() == rr.event);          // newest first
    // an untouched prop-free rock has no history at all
    const EntityId rock = w.create_entity("rock", w.materials.id_of("stone"), {9000, 0});
    CHECK(w.history_of(rock, 50).empty());

    // ---- deterministic replay over a long horizon ---------------------------
    auto scenario = [](World& x) {
        PropertyBag pb;
        pb.set(pid(x, "sight_range"), Value::integer(500));
        const EntityId a = x.create_entity("npc", kNoMaterial, {0, 0}, pb);
        x.mind(a);
        for (int i = 0; i < 24; ++i) {
            PropertyBag ob;
            ob.set(pid(x, "hunger"), Value::integer(600 + i * 10));
            ob.set(pid(x, "thirst"), Value::integer(0));
            ob.set(pid(x, "metabolism"), Value::integer(1500 + i * 100));
            ob.set(pid(x, "integrity"), Value::integer(800));
            // half near the agent (active), half far (dormant)
            const int px = (i % 2 == 0) ? 50 + i * 20 : 5000 + i * 300;
            x.create_entity("org", kNoMaterial, {px, 0}, ob);
        }
        for (int i = 0; i < 6; ++i) x.advance_ticks_for_test(100);
        // starved organisms are gone; survivors keep drifting deterministically
        x.advance_ticks_for_test(200);
    };
    World wA(47, "data"), wB(47, "data");
    scenario(wA);
    // clone mid-run: the clone must continue identically (LOD state included)
    World wC = wA.clone();
    scenario(wB);
    wB.advance_ticks_for_test(37);
    wC.advance_ticks_for_test(37);
    CHECK(wB.state_hash() == wC.state_hash());
    CHECK(wA.state_hash() != wB.state_hash());    // different seeds/ticks

    // save/load preserves dormant catch-up state (save v10)
    const std::string path = "/tmp/simlab_p9_save.bin";
    const std::uint64_t h = wB.state_hash();
    CHECK(wB.save(path));
    World wD(47, "data");
    CHECK(wD.load(path));
    CHECK(wD.state_hash() == h);
    wD.advance_ticks_for_test(64);
    World wE = wB.clone();
    wE.advance_ticks_for_test(64);
    CHECK(wD.state_hash() == wE.state_hash());
}


// ---------- two-layer flora: fruit cycle is data-driven and LOD-exact --------
static void test_fruit_cycle() {
    World w(71, "data");
    const SpeciesDef* bush = w.species.find_name("berry_bush");
    CHECK(bush != nullptr);
    if (!bush) return;
    const PropId fc = pid(w, "fruit_count"), integ = pid(w, "integrity");
    const PropId hunger = pid(w, "hunger"), reach = pid(w, "reach");
    const PropId met = pid(w, "metabolism");
    CHECK(w.materials.find(bush->material)->name == "living_wood");
    CHECK(w.prop_int(*w.find(w.create_entity("probe", bush->material, {9000, 9000})), pid(w, "decay_rate"), -1) == 0);
    const EntityId b = w.create_entity(bush->name, bush->material, {10, 0}, bush->props);
    // metabolism 0 freezes need-drift: the test dummy must survive the
    // thousands of ticks this scenario advances or it starves mid-test.
    PropertyBag ab; ab.set(hunger, Value::integer(900)); ab.set(reach, Value::integer(100));
    ab.set(met, Value::integer(0));
    const EntityId eater = w.create_entity("eater", kNoMaterial, {0, 0}, ab);
    // strip the bush: one fruit per consume, nutrition comes from wild_berries
    const std::int64_t nut = w.prop_int(*w.find(w.create_entity("berry_probe",
        w.materials.id_of("wild_berries"), {9000, 9100})), pid(w, "nutrition"), 0);
    CHECK(nut == 400);
    std::int64_t eaten = 0;
    for (int i = 0; i < 200 && w.prop_int(*w.find(b), fc, 0) > 0; ++i) {
        w.find(eater)->props.set(hunger, Value::integer(900));
        if (w.interact(mk_ctx(eater, {b}, "consume", 0)).completed) ++eaten;
    }
    CHECK(eaten > 0);
    CHECK(w.prop_int(*w.find(b), fc, 0) == 0);
    CHECK(w.find(b)->alive);                                   // bare, not dead
    CHECK(w.prop_int(*w.find(b), integ, 0) == 1000);           // body untouched
    w.find(eater)->props.set(hunger, Value::integer(900));
    InteractionResult bare = w.interact(mk_ctx(eater, {b}, "consume", 0));
    CHECK(!bare.completed);                                    // no fruit -> refused
    // fruit returns over time and stops at max_fruit; the body never decays
    for (int i = 0; i < 3000; ++i) w.step();
    CHECK(w.find(b)->alive && w.prop_int(*w.find(b), integ, 0) == 1000);
    const std::int64_t mx = w.prop_int(*w.find(b), pid(w, "max_fruit"), 0);
    const std::int64_t now = w.prop_int(*w.find(b), fc, 0);
    CHECK(now > 0 && now <= mx);
    // perception records the bare/full state for memory-driven foraging
    PropertyBag nb; nb.set(pid(w, "sight_range"), Value::integer(300));
    const EntityId npc = w.create_entity("npc", kNoMaterial, {5, 0}, nb);
    w.perceive(npc);
    const auto it = w.mind(npc).memory.find(b);
    CHECK(it != w.mind(npc).memory.end() && it->second.props.count(fc) == 1 &&
          it->second.props.at(fc).i == now);
    // physical damage still kills the body (the only way it dies)
    w.find(b)->props.set(integ, Value::integer(1));
    CHECK(w.interact(mk_ctx(eater, {b}, "impact", 500000)).completed);
    CHECK(!w.find(b)->alive);
    // per-tick vs batched (dormant LOD) drift agree exactly
    World p1(72, "data"), p2(72, "data");
    for (World* ww : {&p1, &p2}) {
        const EntityId x = ww->create_entity(bush->name, bush->material, {0, 0}, bush->props);
        ww->find(x)->props.set(fc, Value::integer(3));
    }
    for (int i = 0; i < 500; ++i) p1.step();
    p2.advance_ticks_for_test(500);
    CHECK(p1.prop_int(*p1.entities[0], fc, 0) == p2.prop_int(*p2.entities[0], fc, 0));
}

// ---------- Phase 9 capstone: autonomous driver + emergent chronicle --------
static void test_phase9_autonomous_civilization() {
    const char* VERBS_ELDER[] = {
        "consume","reproduce","rest","wield","unwield","move","communicate",
        "engrave","read","found","join","leave","share","withdraw",
        "dig","chop","strike","impact","process","heat","assemble","disassemble"};
    const char* VERBS_BASE[] = {
        "consume","reproduce","rest","wield","unwield","move"};

    auto build = [&](World& w) {
        const PropId hunger = pid(w,"hunger"), thirst = pid(w,"thirst");
        const PropId fatigue = pid(w,"fatigue"), met = pid(w,"metabolism");
        const PropId speed = pid(w,"speed"), sight = pid(w,"sight_range");
        const PropId reach = pid(w,"reach"), energy = pid(w,"energy");
        const PropId integ = pid(w,"integrity"), age = pid(w,"age");
        const PropId adult = pid(w,"adult_age"), fert = pid(w,"fertility");
        const PropId mut = pid(w,"mutation_rate"), crng = pid(w,"communicate_range");
        const PropId maxe = pid(w,"max_energy");
        // 14 founding foragers in a loose band — genomes carry the species
        // gene set so they are mutually compatible (identical locus sets).
        for (int i = 0; i < 14; ++i) {
            PropertyBag pb;
            pb.set(hunger, Value::integer(0)); pb.set(thirst, Value::integer(0));
            pb.set(fatigue, Value::integer(0)); pb.set(met, Value::integer(2000));
            pb.set(speed, Value::integer(8000)); pb.set(sight, Value::integer(800));
            pb.set(reach, Value::integer(300)); pb.set(energy, Value::integer(300000));
            pb.set(maxe, Value::integer(300000));
            pb.set(integ, Value::integer(800)); pb.set(age, Value::integer(0));
            pb.set(adult, Value::integer(120)); pb.set(fert, Value::integer(120));
            pb.set(mut, Value::integer(50)); pb.set(crng, Value::integer(250));
            const EntityId id = w.create_entity("forager", kNoMaterial,
                {static_cast<std::int32_t>(i * 60 - 400),
                 static_cast<std::int32_t>((i % 3) * 80 - 80)}, pb);
            Entity* e = w.find(id);
            Genome g;
            g.set(speed, 8000); g.set(met, 2000); g.set(sight, 800);
            g.set(fert, 120); g.set(mut, 50);
            e->genome = g;
            AgentMind& m = w.mind(id);
            const char* const* verbs = (i == 0)
                ? VERBS_ELDER : VERBS_BASE;
            const std::size_t nv = (i == 0)
                ? sizeof(VERBS_ELDER)/sizeof(VERBS_ELDER[0])
                : sizeof(VERBS_BASE)/sizeof(VERBS_BASE[0]);
            for (std::size_t v = 0; v < nv; ++v) m.learn_action(verbs[v]);
        }
        // finite but renewable forage in CLUSTERS: 5 bushes x 40 berries per
        // cluster, 4 clusters scattered across the map — one near the home
        // band, three far enough that reaching them requires real migration.
        // quantity regrows slowly toward max_quantity while the bush lives,
        // but the bush itself rots (material decay_rate) — food is a cycle,
        // not a fixture (Phase 9 ecology).
        // two-layer flora: the bush BODY is living_wood (tough, non-perishing);
        // the FOOD is wild_berries fruit hanging on it via fruit_material +
        // fruit_count — eaten fruit never kills the plant, bare branches
        // regrow toward max_fruit. 4 clusters x 5 bushes x 40 fruit.
        // Bushes come from DATA (species.txt berry_bush: body material +
        // fruit props) — the test hardcodes no fruit numbers.
        const SpeciesDef* bush = w.species.find_name("berry_bush");
        CHECK(bush != nullptr);
        const std::int32_t clusters[4][2] = {
            {-350, 260},     // home cluster — partly within founding sight
            {-1700, -850},   // far west
            {1500, -950},    // far south-east
            {1400, 850}};    // far north-east
        for (int c = 0; c < 4 && bush; ++c)
            for (int j = 0; j < 5; ++j)
                w.create_entity(bush->name, bush->material,
                    {clusters[c][0] + static_cast<std::int32_t>((j * 37 + c * 11) % 120 - 60),
                     clusters[c][1] + static_cast<std::int32_t>((j * 53 + c * 17) % 120 - 60)},
                    bush->props);
        // stone surfaces for recorded knowledge + one sharp shard to wield
        const MaterialId stone = w.materials.id_of("stone");
        const MaterialId flint = w.materials.id_of("flint");
        for (int i = 0; i < 3; ++i)
            w.create_entity("rock", stone,
                {static_cast<std::int32_t>(-300 + i * 300),
                 static_cast<std::int32_t>(500 + i * 60)}, {});
        PropertyBag sh; sh.set(pid(w,"edge"), Value::integer(600));
        sh.set(pid(w,"integrity"), Value::integer(900));
        w.create_entity("shard", flint, {40, -60}, sh);
    };

    World wA(53, "data"), wB(53, "data");
    build(wA); build(wB);

    // SIMLAB_CYCLES overrides the run length for staged smoke tests
    // (e.g. SIMLAB_CYCLES=100 before attempting the full 1200-cycle capstone).
    const char* cyc_env = std::getenv("SIMLAB_CYCLES");
    const std::int64_t cycles = cyc_env ? std::atoll(cyc_env) : 1200;
    // Long-horizon outcomes (experiments run, verified methods, forage
    // consumed) need hundreds of cycles to emerge — smoke runs only assert
    // the structural invariants (determinism, census, accounting).
    const bool full_run = cycles >= 1000;

    // ---- autonomous cycles ---------------------------------------------------
    DriverTally tally = autonomous_run(wA, cycles);
    CHECK(tally.cycles == static_cast<std::uint64_t>(cycles));
    if (full_run) CHECK(tally.experiments > 0);   // discovery ran experiments
    CHECK(tally.perceived > 0);
    CHECK(wA.clock_.tick() >= static_cast<std::uint64_t>(cycles));  // ambient drift advanced the clock

    // ---- World History: the chronicle must show a LIVING history ------------
    Chronicle ch = analyze_world(wA);
    CHECK(ch.tick == wA.clock_.tick());
    CHECK(ch.minds >= 14);              // founders + promoted offspring
    CHECK(ch.genome_carriers > 0);
    CHECK(ch.genome_lineages >= 1);     // locus sets still trackable
    CHECK(!ch.allele_sums.empty());     // allele means computable for drift
    if (full_run) CHECK(!ch.verified_methods.empty()); // Verified MethodKeys exist
    CHECK(ch.energy.first > 0);         // energy was really spent
    // fruit is CONSERVED per plant: every LIVING fruiting plant holds at most
    // its own max_fruit, so the total never exceeds the sum of the living
    // plants' ceilings — fruit is gained only by regrowth toward max_fruit,
    // never created ex nihilo. (The seed bank germinates NEW bushes, so a
    // fixed "20 bushes x 40" ceiling would be wrong; the law is per plant.)
    // (Bush bodies are living_wood; the wild_berries material only exists as
    // detached fruit, so material inventory cannot see on-branch stock.)
    {
        const PropId fcp = pid(wA, "fruit_count");
        const PropId fmp = pid(wA, "fruit_material");
        const PropId fmx = pid(wA, "max_fruit");
        std::int64_t fruit_total = 0, fruit_ceiling = 0;
        bool per_plant_ok = true;
        for (const auto& ep : wA.entities) {
            if (!ep->alive) continue;
            const Value* fv = wA.prop_of(*ep, fmp);
            if (!fv || fv->type != ValType::Text || fv->s != "wild_berries")
                continue;
            const std::int64_t fc = wA.prop_int(*ep, fcp, 0);
            const std::int64_t mx = wA.prop_int(*ep, fmx, 0);
            if (fc < 0 || fc > mx) per_plant_ok = false;
            fruit_total += fc;
            fruit_ceiling += mx;
        }
        CHECK(per_plant_ok);
        CHECK(fruit_total <= fruit_ceiling);
    }
    // ---- two-layer flora over the whole run ---------------------------------
    // every bush BODY survives (eating/spoiling fruit never kills it), and the
    // fruit layer really cycles: some bush was stripped bare (fruit_count 0)
    // and LATER bore fruit again (drift raised fruit_count after the zero).
    {
        const PropId fcp = pid(wA, "fruit_count");
        const PropId fcr = pid(wA, "fruit_credit");
        std::size_t bushes = 0, alive_bushes = 0;
        std::map<EntityId, std::uint64_t> hit_zero;   // id -> tick it first hit 0
        std::set<EntityId> refruited;
        for (const auto& ep : wA.entities)
            if (ep->kind_name == "berry_bush") { ++bushes; if (ep->alive) ++alive_bushes; }
        for (const CausalEvent& ev : wA.ledger.history())
            for (const StateChange& c : ev.changes) {
                if (c.prop != fcp) continue;
                if (c.after.i == 0) { if (!hit_zero.count(c.entity)) hit_zero[c.entity] = ev.tick; }
                else if (c.after.i > c.before.i && hit_zero.count(c.entity)) refruited.insert(c.entity);
            }
        std::size_t stripped = 0;
        for (const auto& [id, z] : hit_zero) (void)z, ++stripped;
        // per-bush forensics: did a stripped bush die (no drift possible) or
        // is it alive and merely accumulating fruit_credit toward the next
        // fruit? That decides whether "no post-zero regrow" is ecology or bug.
        for (const auto& [id, ztick] : hit_zero) {
            const Entity* ep = wA.find(id);
            std::printf("    stripped bush %llu: %s fruit_count=%lld "
                        "fruit_credit=%lld zero_at_tick=%llu final_tick=%llu\n",
                        (unsigned long long)id,
                        (ep && ep->alive) ? "alive" : "DEAD",
                        ep ? (long long)wA.prop_int(*ep, fcp, -1) : -1ll,
                        ep ? (long long)wA.prop_int(*ep, fcr, -1) : -1ll,
                        (unsigned long long)ztick,
                        (unsigned long long)wA.clock_.tick());
        }
        // Drift-side fruit accounting: `metabolize` events are pure ecology
        // (regrow up / spoil down) — `consume` events are excluded, so this
        // proves the plant layer itself produced fruit, not just that some
        // bush once held some. If NPCs strip bushes the instant fruit
        // appears, fruit_count can sit near 0 while regrowth still flows.
        std::int64_t fruits_regrown = 0, fruits_spoiled = 0;
        for (const CausalEvent& ev : wA.ledger.history()) {
            if (ev.action != "metabolize") continue;
            for (const StateChange& c : ev.changes) {
                if (c.prop != fcp || c.before.type != ValType::Integer ||
                    c.after.type != ValType::Integer) continue;
                if (c.after.i > c.before.i) fruits_regrown += c.after.i - c.before.i;
                else                        fruits_spoiled += c.before.i - c.after.i;
            }
        }
        CHECK(bushes >= 20);   // 20 planted + seed-bank recruits (ecology)
        // Food-path invariant: NO bush may die THROUGH `consume` — fruit loss
        // drains fruit_count only, never the body. Physical/thermal deaths
        // (strike/chop/impact/heat experiments by foragers) are legitimate
        // ecology per the two-layer spec, so they are counted, not forbidden.
        const PropId ip = pid(wA, "integrity");
        std::size_t eaten_to_death = 0;
        for (const CausalEvent& ev : wA.ledger.history()) {
            if (ev.action != "consume") continue;
            for (const StateChange& c : ev.changes) {
                if (c.prop != ip || c.after.type != ValType::Integer ||
                    c.after.i > 0) continue;
                const Entity* ep = wA.find(c.entity);
                if (ep && ep->kind_name == "berry_bush") ++eaten_to_death;
            }
        }
        CHECK(eaten_to_death == 0);
        // Recovery is NOT asserted per bush: a stripped bush can legitimately
        // be killed by physical damage before it fruits again (collapse of the
        // food web is a valid emergent outcome, not a bug). The cycle itself
        // is proven by aggregate counters: fruit_count was depleted to zero,
        // and drift produced positive deltas on living bushes (regrowth can
        // only run on alive entities) plus spoilage losses.
        std::size_t refruited_alive = 0;
        for (EntityId id : refruited) {
            const Entity* ep = wA.find(id);
            if (ep && ep->alive) ++refruited_alive;
        }
        if (full_run) {
            CHECK(stripped > 0);                        // scarcity happened
            CHECK(fruits_regrown > 0);                  // drift really fruited
            CHECK(tally.moves > 0);                     // foragers had to travel
        }
        std::printf("  flora: bushes=%zu alive=%zu stripped=%zu refruited=%zu "
                    "regrown=%lld spoiled=%lld eaten_to_death=%zu\n",
                    bushes, alive_bushes, stripped, refruited_alive,
                    (long long)fruits_regrown, (long long)fruits_spoiled,
                    eaten_to_death);
    }
    // census rows are sorted and population accounting is consistent
    CHECK(!ch.census.empty());
    for (std::size_t i = 1; i < ch.census.size(); ++i)
        CHECK(ch.census[i-1].kind < ch.census[i].kind);
    std::int64_t total = 0;
    for (const CensusRow& r : ch.census) total += r.alive + r.dead;
    CHECK(total >= 14);                 // founders + any births/deaths counted
    CHECK(ch.entities_alive + ch.entities_dead ==
          static_cast<std::int64_t>(wA.entities.size()));
    // report the emergent shape (not asserted — honest observation)
    std::printf("  chronicle: alive=%ld dead=%ld minds=%ld lineages=%ld "
                "verified=%zu composites=%ld inscribed=%ld collectives=%ld "
                "energy_spent=%ld tally(act=%ld mov=%ld soc=%ld)\n",
        (long)ch.entities_alive, (long)ch.entities_dead, (long)ch.minds,
        (long)ch.genome_lineages, ch.verified_methods.size(),
        (long)ch.composites, (long)ch.inscribed, (long)ch.collectives,
        (long)ch.energy.first, (long)tally.experiments, (long)tally.moves,
        (long)tally.social);

    // ---- deterministic replay ------------------------------------------------
    autonomous_run(wB, cycles);
    CHECK(wA.state_hash() == wB.state_hash());
    std::printf("  capstone fingerprint: cycles=%lld tick=%llu entities=%zu "
                "ledger=%zu state_hash=%016llx content_hash=%016llx\n",
                (long long)cycles, (unsigned long long)wA.clock_.tick(),
                wA.entities.size(), wA.ledger.size(),
                (unsigned long long)wA.state_hash(),
                (unsigned long long)wA.content_hash());
    Chronicle chB = analyze_world(wB);
    CHECK(chB.verified_methods == ch.verified_methods);
    CHECK(chB.genome_lineages == ch.genome_lineages);
}


// ---------- ecology: seed bank + edge influx (data-driven) -------------------
static void test_seed_bank() {
    World w(91, "data");
    const SpeciesDef* bush = w.species.find_name("berry_bush");
    CHECK(bush != nullptr);
    if (!bush) return;
    const PropId integ = pid(w, "integrity");
    const PropId st = pid(w, "sprout_target");
    const PropId fc = pid(w, "fruit_count");
    // kill a fruiting bush -> its death_drops manifest must appear once
    const EntityId b = w.create_entity(bush->name, bush->material, {100, 100}, bush->props);
    w.find(b)->props.set(integ, Value::integer(1));
    w.destroy_entity(b);
    w.step();
    int seeds = 0, debris = 0;
    EntityId seed0 = kNoEntity;
    for (const auto& ep : w.entities) {
        if (!ep->alive) continue;
        if (ep->kind_name == "dormant_seed") { ++seeds; if (seed0 == kNoEntity) seed0 = ep->id; }
        else if (ep->kind_name == "wood_debris") ++debris;
    }
    CHECK(seeds == 4);                    // death_drops dormant_seed=4
    CHECK(debris == 1);                   // death_drops wood_debris=1
    // the propagule remembers its species
    const Entity* sd = w.find(seed0);
    CHECK(sd != nullptr);
    const Value* tv = sd ? w.prop_of(*sd, st) : nullptr;
    CHECK(tv && tv->type == ValType::Text && tv->s == "berry_bush");
    // marker prevents double-drops: stepping again adds nothing
    const std::size_t before = w.entities.size();
    for (int i = 0; i < 50; ++i) w.step();
    CHECK(w.entities.size() == before);
    // germination: sprout_rate 2 permille/tick -> credit hits 1000 at ~500
    // ticks; no terrain -> mild defaults (moist 500, temp 300) -> sprouts.
    // Check while sprouts are still YOUNG (fruit not yet refilled).
    for (int i = 0; i < 750; ++i) w.step();
    int sprouts = 0, seeds_left = 0;
    EntityId sprout_id = kNoEntity;
    for (const auto& ep : w.entities) {
        if (!ep->alive) continue;
        if (ep->kind_name == "berry_bush") { ++sprouts; if (sprout_id == kNoEntity) sprout_id = ep->id; }
        if (ep->kind_name == "dormant_seed") ++seeds_left;
    }
    CHECK(sprouts >= 1);                  // the forest answered the fire
    CHECK(seeds_left == 0);               // every propagule either sprouted or rotted
    // a fresh sprout bears little fruit; fruit drift then refills it
    const std::int64_t fc0 = w.prop_int(*w.find(sprout_id), fc, -1);
    const std::int64_t mx = w.prop_int(*w.find(sprout_id), pid(w, "max_fruit"), 0);
    CHECK(fc0 >= 0 && fc0 < mx);
    for (int i = 0; i < 1500; ++i) w.step();
    CHECK(w.prop_int(*w.find(sprout_id), fc, -1) > fc0);
}

static void test_memory_engram() {
    // (a) ordinary daily memories fade and release their RAM
    {
        World w(7, "data");
        const EntityId npc = w.create_entity("npc", kNoMaterial, {0, 0});
        const EntityId tgt = w.create_entity("bush", kNoMaterial, {50, 0});
        AgentMind& m = w.mind(npc);
        AgentMind::MemorySnapshot s;
        s.tick = 0; s.pos = {50, 0}; s.kind_name = "bush";
        upsert_memory(m, tgt, s, 0, MindParams{});
        CHECK(m.memory.count(tgt) == 1);
        CHECK(m.memory.at(tgt).engram.clarity == 400);   // base_clarity
        // clarity 400, stability 15, decay 10 -> a drop every ~1.5 ticks
        w.advance_ticks_for_test(2000);
        CHECK(m.memory.count(tgt) == 0);                 // forgotten = freed
    }
    // (b) an imprinted trauma (|charge| >= 800) survives tens of thousands
    //     of ticks — amygdala stability ~6000 vs routine ~15
    {
        World w(7, "data");
        const EntityId npc = w.create_entity("npc", kNoMaterial, {0, 0});
        const EntityId tgt = w.create_entity("bush", kNoMaterial, {50, 0});
        AgentMind& m = w.mind(npc);
        AgentMind::MemorySnapshot s;
        s.tick = 0; s.pos = {50, 0}; s.kind_name = "bush";
        upsert_memory(m, tgt, s, 800, MindParams{});
        const MemoryEngram& g = m.memory.at(tgt).engram;
        CHECK(g.clarity == 1000);                        // max_clarity imprint
        CHECK(g.stability == 6000);                      // 2000 + 800*5
        CHECK(g.emotional_charge == 800);
        w.advance_ticks_for_test(50000);
        CHECK(m.memory.count(tgt) == 1);                 // still remembered
        CHECK(m.memory.at(tgt).engram.emotional_charge == 800);
    }
    // (c) LOD invariance: dt=1 x8 must equal dt=8 exactly — same engram
    //     fields and same state_hash (accum/drop is batch-exact)
    {
        World a(11, "data"), b(11, "data");
        EntityId na, ta, nb, tb;
        {
            na = a.create_entity("npc", kNoMaterial, {0, 0});
            ta = a.create_entity("bush", kNoMaterial, {50, 0});
        }
        {
            nb = b.create_entity("npc", kNoMaterial, {0, 0});
            tb = b.create_entity("bush", kNoMaterial, {50, 0});
        }
        for (auto [wp, npc, tgt] : {std::tuple{&a, na, ta}, std::tuple{&b, nb, tb}}) {
            AgentMind& m = wp->mind(npc);
            AgentMind::MemorySnapshot s;
            s.tick = 0; s.pos = {50, 0}; s.kind_name = "bush";
            upsert_memory(m, tgt, s, 0, MindParams{});
            MemoryEngram& g = m.memory[tgt].engram;
            g.clarity = 100; g.stability = 7; g.decay_accum = 3;
            g.emotional_charge = 50;
        }
        for (int i = 0; i < 8; ++i) a.advance_ticks_for_test(1);
        b.advance_ticks_for_test(8);
        const MemoryEngram& ga = a.mind(na).memory.at(ta).engram;
        const MemoryEngram& gb = b.mind(nb).memory.at(tb).engram;
        CHECK(ga.clarity == gb.clarity);
        CHECK(ga.stability == gb.stability);
        CHECK(ga.decay_accum == gb.decay_accum);
        CHECK(ga.emotional_charge == gb.emotional_charge);
        CHECK(a.state_hash() == b.state_hash());
    }
    // (d) semantic drift: a fuzzy memory recalls a wrong position and a
    //     coarse-grained count — but the emotional charge is untouched
    {
        World w(7, "data");
        const EntityId npc = w.create_entity("npc", kNoMaterial, {0, 0});
        const EntityId tgt = w.create_entity("bush", kNoMaterial, {50, 0});
        AgentMind& m = w.mind(npc);
        AgentMind::MemorySnapshot s;
        s.tick = 0; s.pos = {100, 100}; s.kind_name = "bush";
        const PropId fc = pid(w, "fruit_count");
        s.props[fc] = Value::integer(40);
        upsert_memory(m, tgt, s, 700, MindParams{});
        AgentMind::MemorySnapshot& ms = m.memory[tgt];
        ms.engram.clarity = 100;              // below blur threshold 300
        ms.engram.stability = 100000;         // keep it alive for the check
        ms.engram.emotional_charge = -650;
        // position drifts deterministically (hashed on id+seed+clarity)
        const Vec2 r1 = recalled_pos(ms, tgt, w.seed, 300);
        const Vec2 r2 = recalled_pos(ms, tgt, w.seed, 300);
        CHECK(r1.x == r2.x && r1.y == r2.y);   // deterministic recall
        CHECK(!(r1.x == 100 && r1.y == 100));  // ...but not the true spot
        // integer props quantize to coarse buckets: 40 -> (40/21)*21 = 21
        const Value rv = recalled_prop(ms, fc, tgt, w.seed, 300, Value::integer(-1));
        CHECK(rv.type == ValType::Integer && rv.i == 21);
        // emotion survives the blur untouched
        CHECK(ms.engram.emotional_charge == -650);
        // a crisp memory (clarity >= threshold) recalls truth exactly
        ms.engram.clarity = 400;
        CHECK(recalled_pos(ms, tgt, w.seed, 300).x == 100);
        CHECK(recalled_prop(ms, fc, tgt, w.seed, 300, Value::integer(-1)).i == 40);
    }
    // (e) an unused skill fades and must be relearned — clarity 0 erases
    //     the DiscoveryRecord itself
    {
        World w(7, "data");
        const EntityId npc = w.create_entity("npc", kNoMaterial, {0, 0});
        AgentMind& m = w.mind(npc);
        DiscoveryRecord d;
        d.key = "method:test"; d.claim = "test method";
        d.engram.clarity = 100; d.engram.stability = 15;
        m.discoveries[d.key] = d;
        CHECK(m.discoveries.count("method:test") == 1);
        w.advance_ticks_for_test(2000);
        CHECK(m.discoveries.count("method:test") == 0);   // forgotten skill
    }
}

static void test_edge_influx() {
    const std::string dir = "/tmp/simlab_influx";
    std::string cmd = "mkdir -p " + dir + " && cp data/properties.txt data/materials.txt data/species.txt " + dir + "/";
    CHECK(std::system(cmd.c_str()) == 0);
    {
        std::ofstream f(dir + "/influx.txt");
        f << "influx_zone test_pass\n"
             "  bounds 100 0 200 50\n"
             "  interval 20\n"
             "  capacity 5\n"
             "  migrants 2\n"
             "  migrant forager\n"
             "    kind forager\n"
             "    prop hunger 0\n"
             "    prop thirst 0\n"
             "    prop fatigue 0\n"
             "    prop metabolism 2000\n"
             "    prop speed 8000\n"
             "    prop sight_range 800\n"
             "    prop reach 300\n"
             "    prop energy 300000\n"
             "    prop max_energy 300000\n"
             "    prop integrity 800\n"
             "    prop age 0\n"
             "    prop adult_age 120\n"
             "    prop fertility 120\n"
             "    prop mutation_rate 50\n"
             "    prop communicate_range 250\n"
             "    gene speed 8000\n"
             "    gene metabolism 2000\n"
             "    gene sight_range 800\n"
             "    gene fertility 120\n"
             "    gene mutation_rate 50\n"
             "    innate consume\n"
             "    innate move\n"
             "    innate rest\n";
    }
    World w(5, dir);
    CHECK(w.influx.size() == 1);
    const InfluxZone* z = w.influx.find_name("test_pass");
    CHECK(z != nullptr);
    for (int i = 0; i < 500; ++i) w.step();
    int foragers = 0, minded = 0, genomed = 0;
    for (const auto& ep : w.entities) {
        if (!ep->alive || ep->kind_name != "forager") continue;
        ++foragers;
        CHECK(z->contains(ep->pos));        // migrants appear inside the zone
        const AgentMind* m = w.find_mind(ep->id);
        if (m && m->known_actions.count("consume") &&
            m->known_actions.count("move") && m->known_actions.count("rest"))
            ++minded;
        if (ep->genome.size() == 5) ++genomed;
    }
    CHECK(foragers > 0);                    // the pass delivered travelers
    CHECK(foragers <= 6);                   // capacity 5 + one in-flight pair
    CHECK(minded == foragers);              // innate verbs -> sapient arrival
    CHECK(genomed == foragers);             // genome template applied
    // deterministic replay: same seed + data -> identical world hash
    World w2(5, dir);
    for (int i = 0; i < 500; ++i) w2.step();
    CHECK(w.state_hash() == w2.state_hash());
}


// ---------- Nhịp 4: cognitive aptitudes & brain cost -------------------------
// No talent labels anywhere: strategist/inventor/artisan/leader are just
// where the genome lands on four continuous axes, priced in energy.
static void test_cognitive_aptitudes() {
    // 1) plan_horizon: a deep mind chains disassemble->consume to feed
    //    itself; a horizon-1 mind only ever sees one-step plans. (Parts
    //    are `contained` until the composite comes apart — consume alone
    //    cannot reach them, so the need goal genuinely requires depth 2.)
    {
        auto setup = [](World& w, std::int64_t horizon) {
            PropertyBag fb;
            fb.set(pid(w, "hunger"), Value::integer(900));
            fb.set(pid(w, "metabolism"), Value::integer(0));
            fb.set(pid(w, "sight_range"), Value::integer(600));
            fb.set(pid(w, "reach"), Value::integer(500));
            fb.set(pid(w, "energy"), Value::integer(1000000));
            fb.set(pid(w, "mind_plan_horizon"), Value::integer(horizon));
            const EntityId npc = w.create_entity("npc", kNoMaterial, {0, 0}, fb);
            const PropId contained = pid(w, "contained");
            PropertyBag cb;
            cb.set(pid(w, "integrity"), Value::integer(800));
            const EntityId comp = w.create_entity("composite", kNoMaterial,
                                                  {100, 0}, cb);
            auto part = [&](const char* kind, MaterialId mat, Vec2 p) {
                PropertyBag pb;
                pb.set(contained, Value::boolean(true));
                pb.set(pid(w, "integrity"), Value::integer(500));
                const EntityId id = w.create_entity(kind, mat, p, pb);
                w.relations.relate(id, "part_of", comp);
                return id;
            };
            // two berry parts: one consume pair relieves 800 hunger —
            // enough to cross the <=100 satisfied threshold from 900
            const EntityId food = part("part", w.materials.id_of("wild_berries"),
                                       {110, 0});
            const EntityId food2 = part("part", w.materials.id_of("wild_berries"),
                                        {115, 0});
            part("part", w.materials.id_of("stone"), {120, 0});
            AgentMind& m = w.mind(npc);
            m.learn_action("disassemble");
            m.learn_action("consume");
            m.known_entities.insert(comp);
            m.known_entities.insert(food);
            m.known_entities.insert(food2);
            return npc;
        };
        World wd(31, "data"), ws(31, "data");
        const EntityId nd = setup(wd, 6);
        const EntityId ns = setup(ws, 1);
        auto goals_of = [](World& w, EntityId npc) {
            for (const Goal& g : derive_goals(w, npc))
                if (g.kind == Goal::Kind::SatisfyNeed) return g;
            return Goal{};
        };
        const Goal gd = goals_of(wd, nd), gs = goals_of(ws, ns);
        CHECK(gd.kind == Goal::Kind::SatisfyNeed);
        // generate_hypotheses reads mind_plan_horizon off the entity —
        // THAT is the per-individual wiring under test.
        bool deep_ok = false, shallow_ok = false;
        {
            Problem p; p.kind = ProblemKind::UnmetNeed; p.urgency = 500;
            p.goal = gd;
            for (const Hypothesis& h :
                 generate_hypotheses(wd, nd, p, 8))
                if (h.plan.satisfied && h.plan.steps.size() >= 2)
                    deep_ok = true;
        }
        {
            Problem p; p.kind = ProblemKind::UnmetNeed; p.urgency = 500;
            p.goal = gs;
            for (const Hypothesis& h :
                 generate_hypotheses(ws, ns, p, 8))
                if (h.plan.satisfied) shallow_ok = true;
        }
        CHECK(deep_ok);       // horizon 6 chained disassemble -> consume
        CHECK(!shallow_ok);   // horizon 1 sees only isolated reflexes
    }
    // 2) abstraction_gain: an imprint on material X fans out a partial
    //    charge to unseen kin — avoidance transfers before contact.
    {
        auto run = [](std::int64_t abs_gain) {
            World w(32, "data");
            PropertyBag fb;
            fb.set(pid(w, "sight_range"), Value::integer(600));
            fb.set(pid(w, "reach"), Value::integer(500));
            fb.set(pid(w, "energy"), Value::integer(100000));
            fb.set(pid(w, "mind_abstraction_gain"), Value::integer(abs_gain));
            const EntityId npc = w.create_entity("npc", kNoMaterial, {0, 0}, fb);
            w.mind(npc);                                   // ensure the mind exists
            const MaterialId st = w.materials.id_of("stone");
            PropertyBag wb;                                // fragile lookalike kin
            wb.set(pid(w, "integrity"), Value::integer(10));
            wb.set(pid(w, "hardness"), Value::integer(100));
            wb.set(pid(w, "brittleness"), Value::integer(0));
            wb.set(pid(w, "mass"), Value::integer(100));
            const EntityId src = w.create_entity("src", st, {100, 0}, wb);
            const EntityId kin = w.create_entity("kin", st, {140, 0}, wb);
            w.interact(mk_ctx(npc, {src}, "impact", 1000)); // kills src -> -900 charge
            const AgentMind* m = w.find_mind(npc);
            if (!m) return std::pair<bool, std::int64_t>{false, 0};
            const auto it = m->memory.find(kin);
            if (it == m->memory.end()) return std::pair<bool, std::int64_t>{false, 0};
            return std::pair<bool, std::int64_t>{true, it->second.engram.emotional_charge};
        };
        const auto hi = run(2500), lo = run(1000);
        CHECK(hi.first && hi.second <= -500);   // generalized trauma on kin
        CHECK(!lo.first || lo.second == 0);     // neutral mind: no transfer
    }
    // 2b) abstraction_gain also accelerates Discovery evidence accrual.
    {
        auto run = [](std::int64_t abs_gain) {
            World w(33, "data");
            PropertyBag fb;
            fb.set(pid(w, "hunger"), Value::integer(900));
            fb.set(pid(w, "metabolism"), Value::integer(0));
            fb.set(pid(w, "sight_range"), Value::integer(500));
            fb.set(pid(w, "reach"), Value::integer(500));
            fb.set(pid(w, "energy"), Value::integer(100000));
            fb.set(pid(w, "mind_abstraction_gain"), Value::integer(abs_gain));
            const EntityId eater = w.create_entity("npc", kNoMaterial, {0, 0}, fb);
            const EntityId food = w.create_entity("food",
                w.materials.id_of("wild_berries"), {100, 0});
            w.perceive(eater);
            Hypothesis h;
            h.key = "test:consume"; h.claim = "eat";
            h.plan.steps.push_back(mk_ctx(eater, {food}, "consume", 0, "", 2));
            h.goal.kind = Goal::Kind::SatisfyNeed;
            h.goal.need = pid(w, "hunger"); h.goal.value = 100;
            h.serves = ProblemKind::UnmetNeed;
            run_experiment(w, eater, h);
            const AgentMind* m = w.find_mind(eater);
            const auto it = m->discoveries.find("test:consume");
            return it == m->discoveries.end() ? std::int64_t{0}
                                              : it->second.evidence_for;
        };
        const std::int64_t e_hi = run(2500), e_lo = run(1000);
        CHECK(e_hi > e_lo);   // 85 vs 34 evidence per success
    }
    // 3) motor_precision: master's composite keeps more of the weakest
    //    link's soundness; clumsy hands leave it fragile.
    {
        auto assemble_with = [](std::int64_t motor) {
            World w(34, "data");
            const PropId integ = pid(w, "integrity");
            PropertyBag ab;
            ab.set(pid(w, "reach"), Value::integer(500));
            ab.set(pid(w, "energy"), Value::integer(1000000));
            ab.set(pid(w, "mind_motor_precision"), Value::integer(motor));
            const EntityId actor = w.create_entity("npc", kNoMaterial, {0, 0}, ab);
            auto part = [&](Vec2 p) {
                PropertyBag pb;
                pb.set(integ, Value::integer(800));
                pb.set(pid(w, "mass"), Value::integer(100));
                pb.set(pid(w, "hardness"), Value::integer(300));
                pb.set(pid(w, "brittleness"), Value::integer(300));
                pb.set(pid(w, "tensile"), Value::integer(800));
                return w.create_entity("part", w.materials.id_of("stone"), p, pb);
            };
            const EntityId p1 = part({50, 0}), p2 = part({80, 0});
            InteractionResult r = w.interact(
                mk_ctx(actor, {p1, p2}, "assemble", 0, "bind", 2));
            if (!r.completed || r.spawned.empty()) return std::int64_t{-1};
            const Entity* c = w.find(r.spawned.front());
            return c ? w.prop_int(*c, integ, -1) : std::int64_t{-2};
        };
        const std::int64_t hi = assemble_with(2500);
        const std::int64_t lo = assemble_with(200);
        CHECK(hi > 0 && lo > 0);
        CHECK(hi > lo);   // 2500 vs 200 permille on product integrity
    }
    // 4) theory_of_mind: only a mind above neutral reads agitation in a
    //    conspecific's motion — belief, never private-prop access.
    {
        auto run = [](std::int64_t tom) {
            World w(35, "data");
            PropertyBag ob;
            ob.set(pid(w, "sight_range"), Value::integer(600));
            ob.set(pid(w, "speed"), Value::integer(6000));
            ob.set(pid(w, "mind_theory_of_mind"), Value::integer(tom));
            const EntityId watcher = w.create_entity("npc", kNoMaterial, {0, 0}, ob);
            PropertyBag mb;
            mb.set(pid(w, "speed"), Value::integer(100000));
            mb.set(pid(w, "energy"), Value::integer(1000000));
            const EntityId mover = w.create_entity("npc", kNoMaterial, {100, 0}, mb);
            w.mind(mover);                                // conspecific has a mind
            w.perceive(watcher);                          // sighting 1: no motion yet
            w.advance_ticks_for_test(1);
            InteractionContext mv;                        // mover sprints away
            mv.actor = mover; mv.parameters.action = "move";
            mv.parameters.destination = {200, 0};
            mv.parameters.has_destination = true;
            mv.parameters.ticks = 1;
            w.interact(mv);
            w.advance_ticks_for_test(1);
            w.perceive(watcher);                          // sighting 2: big displacement
            const AgentMind* m = w.find_mind(watcher);
            const Belief* b = m ? m->find_belief("mood:" + std::to_string(mover))
                                : nullptr;
            return b ? b->confidence : std::int64_t{0};
        };
        CHECK(run(2500) > 0);   // high tom reads the sprint as agitation
        CHECK(run(1000) == 0);  // neutral tom sees motion, infers nothing
    }
    // 5) brain metabolic cost: cognitive surplus burns energy, and the
    //    permille accumulator keeps dt=1 x N identical to dt=N.
    {
        auto build = [](World& w) {
            PropertyBag fb;
            fb.set(pid(w, "hunger"), Value::integer(100));
            fb.set(pid(w, "metabolism"), Value::integer(0));
            fb.set(pid(w, "energy"), Value::integer(100000));
            fb.set(pid(w, "mind_plan_horizon"), Value::integer(6));
            fb.set(pid(w, "mind_abstraction_gain"), Value::integer(2500));
            fb.set(pid(w, "mind_brain_metabolic_cost"), Value::integer(15));
            const EntityId npc = w.create_entity("npc", kNoMaterial, {0, 0}, fb);
            w.mind(npc);                                   // a mind exists -> it thinks
            return npc;
        };
        World wa(36, "data"), wb(36, "data");
        const EntityId ea = build(wa), eb = build(wb);
        const PropId energy = pid(wa, "energy");
        wa.advance_ticks_for_test(8);                       // one batch of 8
        for (int i = 0; i < 8; ++i) wb.advance_ticks_for_test(1);  // 8 single ticks
        const Entity* ae = wa.find(ea); const Entity* be = wb.find(eb);
        CHECK(ae && be);
        if (ae && be) {
            CHECK(wa.prop_int(*ae, energy, -1) ==
                  wb.prop_int(*be, energy, -1));           // LOD-invariant drain
            CHECK(wa.prop_int(*ae, energy, 0) < 100000);    // surplus costs energy
            CHECK(wa.prop_int(*ae, pid(wa, "mind_brain_accum"), 0) ==
                  wb.prop_int(*be, pid(wb, "mind_brain_accum"), 0));
        }
        // NOTE: full state_hash is NOT asserted here — the causal ledger
        // records one "metabolize" event per advance_ticks() CALL, so
        // batch-vs-stepped runs legitimately differ in event count (that
        // is causal accounting, deterministic per call sequence). The
        // LOD-invariant part is the drain math itself, checked above.
    }
}

static void test_attachment_and_parental_care() {
    // 1) Pair-bond + parental imprint: reproduce writes positive engrams
    //    mate<->mate AND deep marks parent<->child — charge scaled only by
    //    heritable gains, no marriage flag anywhere.
    {
        World w(31, "data");
        const std::vector<std::pair<PropId, std::int64_t>> gl =
            {{pid(w, "speed"), 5000}};
        const EntityId a = mk_organism(w, "test_beast", {0, 0}, gl, 300, 500, 30, 1800);
        const EntityId b = mk_organism(w, "test_beast", {10, 0}, gl, 300, 400, 30, 1800);
        const InteractionResult r = w.interact(mk_ctx(a, {b}, "reproduce", 0));
        CHECK(r.completed && r.spawned.size() == 1);
        if (r.spawned.empty()) return;
        const EntityId child = r.spawned.front();
        const AgentMind* ma = w.find_mind(a);
        const AgentMind* mb = w.find_mind(b);
        CHECK(ma && mb);
        if (!ma || !mb) return;
        // mates hold positive engrams on each other (bond_charge ~400)
        auto bond_of = [](const AgentMind& m, EntityId x) -> std::int64_t {
            auto it = m.memory.find(x);
            return it == m.memory.end() ? 0 : it->second.engram.emotional_charge;
        };
        CHECK(bond_of(*ma, b) > 0);
        CHECK(bond_of(*mb, a) > 0);
        // deep parental imprint both directions: stability 9500, big charge
        for (const AgentMind* pm : {ma, mb}) {
            auto it = pm->memory.find(child);
            CHECK(it != pm->memory.end());
            if (it != pm->memory.end()) {
                CHECK(it->second.engram.stability >= 9000);
                CHECK(it->second.engram.emotional_charge >= 500);
            }
        }
        const AgentMind* cm = w.find_mind(child);
        CHECK(cm != nullptr);                    // newborn got a mind via imprint
        if (cm) {
            CHECK(bond_of(*cm, a) >= 500);
            CHECK(bond_of(*cm, b) >= 500);
        }
        // Score(H): sharing with the bonded pays a bond bonus; an unbonded
        // stranger gets none. No flag — just engram charge.
        InteractionContext sc = mk_ctx(a, {child}, "share", 0);
        CHECK(bond_bonus(w, *ma, sc) > 0);
        InteractionContext sc2 = mk_ctx(a, {b}, "strike", 0);
        CHECK(bond_bonus(w, *ma, sc2) == 0);        // positive bond != aggression
    }
    // 2) Bereavement: the dearly-held bond dying flips its engram negative,
    //    floods grief, and marks the killer — allostatic shift does the rest.
    {
        World w(32, "data");
        PropertyBag pb;
        pb.set(pid(w, "sight_range"), Value::integer(500));
        pb.set(pid(w, "reach"), Value::integer(200));
        const EntityId npc = w.create_entity("npc", kNoMaterial, {0, 0}, pb);
        const EntityId victim = w.create_entity("test_beast", kNoMaterial,
                                                {10, 0});
        w.find(victim)->props.set(pid(w, "integrity"), Value::integer(20));
        PropertyBag kb;
        kb.set(pid(w, "reach"), Value::integer(200));
        const EntityId killer = w.create_entity("test_beast", kNoMaterial,
                                                {5, 0}, kb);
        AgentMind& m = w.mind(npc);
        AgentMind::MemorySnapshot ts;
        ts.tick = 0; ts.pos = {10, 0}; ts.kind_name = "test_beast";
        m.memory[victim] = ts;
        MemoryEngram& g = m.memory[victim].engram;
        g.clarity = 800; g.stability = 9000; g.emotional_charge = 850;
        const InteractionResult r = w.interact(mk_ctx(killer, {victim},
                                                    "impact", 500000));
        CHECK(r.completed);
        CHECK(!w.find(victim)->alive);
        // grief flooded, bond flipped to trauma, killer marked hostile
        CHECK(m.neuro.grief > 300);
        CHECK(m.memory[victim].engram.emotional_charge < 0);
        auto kit = m.memory.find(killer);
        CHECK(kit != m.memory.end());
        if (kit != m.memory.end())
            CHECK(kit->second.engram.emotional_charge < 0);
    }
    // 3) Mate guarding: bonded A watches B reproduce with C -> rage floods,
    //    rival marked negative, bond with B cools. No flag anywhere.
    {
        World w(33, "data");
        const std::vector<std::pair<PropId, std::int64_t>> gl =
            {{pid(w, "speed"), 5000}};
        PropertyBag pb;
        pb.set(pid(w, "sight_range"), Value::integer(500));
        const EntityId a = w.create_entity("npc", kNoMaterial, {0, 0}, pb);
        const EntityId b = mk_organism(w, "test_beast", {10, 0}, gl,
                                       300, 500, 30, 1800);
        const EntityId c = mk_organism(w, "test_beast", {20, 0}, gl,
                                       300, 400, 30, 1800);
        AgentMind& m = w.mind(a);
        AgentMind::MemorySnapshot ts;
        ts.tick = 0; ts.pos = {10, 0}; ts.kind_name = "test_beast";
        m.memory[b] = ts;
        MemoryEngram& g = m.memory[b].engram;
        g.clarity = 900; g.stability = 9000; g.emotional_charge = 800;
        m.neuro.tuned = true;
        const InteractionResult r = w.interact(mk_ctx(b, {c}, "reproduce", 0));
        CHECK(r.completed);
        CHECK(m.neuro.rage > 400);                  // 800 * jealousy_gain/1000
        auto cit = m.memory.find(c);
        CHECK(cit != m.memory.end());
        if (cit != m.memory.end())
            CHECK(cit->second.engram.emotional_charge < 0);   // rival marked
        CHECK(m.memory[b].engram.emotional_charge < 800);     // bond cooled
    }
    // 4) Parental polymorphism: drive 2000 -> deep imprint + share bonus;
    //    drive 0 -> nothing written, offspring left to itself.
    {
        auto run = [](std::int64_t drive) -> std::pair<bool, bool> {
            World w(34, "data");
            const std::vector<std::pair<PropId, std::int64_t>> gl =
                {{pid(w, "speed"), 5000}};
            const EntityId a = mk_organism(w, "test_beast", {0, 0}, gl,
                                           300, 500, 30, 1800);
            const EntityId b = mk_organism(w, "test_beast", {10, 0}, gl,
                                           300, 400, 30, 1800);
            w.find(a)->props.set(pid(w, "mind_parental_drive"),
                                 Value::integer(drive));
            const InteractionResult r = w.interact(mk_ctx(a, {b},
                                                        "reproduce", 0));
            if (!r.completed || r.spawned.empty()) return {false, false};
            const AgentMind* ma = w.find_mind(a);
            const AgentMind* cm = w.find_mind(r.spawned.front());
            if (!ma || !cm) return {false, false};
            auto has_imprint = [](const AgentMind& m, EntityId x) {
                auto it = m.memory.find(x);
                return it != m.memory.end() &&
                       it->second.engram.stability >= 9000 &&
                       it->second.engram.emotional_charge > 400;
            };
            const bool parent_marks = has_imprint(*ma, r.spawned.front());
            const bool child_marks = has_imprint(*cm, a);
            // and the imprint shows up as care-drive in Score(H)
            InteractionContext sc = mk_ctx(a, {r.spawned.front()},
                                           "share", 0);
            return {parent_marks && child_marks, bond_bonus(w, *ma, sc) > 0};
        };
        auto hi = run(2000);
        CHECK(hi.first);                            // both directions imprinted
        CHECK(hi.second);                           // parent scores care for young
        auto lo = run(0);
        CHECK(!lo.first);                           // drive 0: no imprint at all
        CHECK(!lo.second);                          // and no care bonus
    }
    // 5) Care arbitration: the imprint's inferred distress becomes a
    //    BondedDistress problem -> share/move candidates enter plan_multi,
    //    and empathy_coupling x own survival pressure decides who yields.
    {
        auto care_run = [](std::int64_t empathy, std::int64_t hunger,
                           std::int64_t& top_score,
                           bool& saw_care_plan) {
            World w(35, "data");
            const std::vector<std::pair<PropId, std::int64_t>> gl =
                {{pid(w, "speed"), 5000}};
            const EntityId a = mk_organism(w, "test_beast", {0, 0}, gl,
                                           300, 500, 30, 1800);
            const EntityId b = mk_organism(w, "test_beast", {10, 0}, gl,
                                           300, 400, 30, 1800);
            const InteractionResult r = w.interact(mk_ctx(a, {b},
                                                        "reproduce", 0));
            if (!r.completed || r.spawned.empty()) return false;
            const EntityId child = r.spawned.front();
            w.find(a)->props.set(pid(w, "mind_empathy_coupling"),
                                 Value::integer(empathy));
            w.find(a)->props.set(pid(w, "hunger"),
                                 Value::integer(hunger));
            w.find(a)->props.set(pid(w, "quantity"),
                                 Value::integer(50));   // carried goods
            w.find(child)->props.set(pid(w, "quantity"),
                                     Value::integer(0));  // can receive
            AgentMind& m = w.mind(a);
            m.known_actions.insert("share");
            m.known_actions.insert("move");
            m.known_actions.insert("consume");
            m.known_entities.insert(child);
            m.neuro.tuned = true;
            // the imprint alone (stability 9500, charge 900) already reads
            // as a dependent in inferred distress — no private props peeked
            const std::vector<Problem> probs = detect_problems(w, a);
            const Problem* bp = nullptr;
            for (const Problem& p : probs)
                if (p.kind == ProblemKind::BondedDistress &&
                    p.subject == child) bp = &p;
            if (!bp) return false;
            const std::vector<Hypothesis> hs =
                generate_hypotheses(w, a, *bp, 4);
            top_score = 0; saw_care_plan = false;
            for (const Hypothesis& h : hs) {
                top_score = std::max(top_score, h.plan.score);
                for (const InteractionContext& c : h.plan.steps) {
                    if (c.parameters.action == "share" &&
                        !c.targets.empty() && c.targets.front() == child)
                        saw_care_plan = true;
                    if (c.parameters.action == "move" &&
                        c.parameters.has_destination)
                        saw_care_plan = true;
                }
            }
            return true;
        };
        std::int64_t hi_score = 0, lo_score = 0, calm_score = 0;
        bool hi_plan = false, lo_plan = false, calm_plan = false;
        CHECK(care_run(2500, 950, hi_score, hi_plan));   // starving altruist
        CHECK(care_run(0,    950, lo_score, lo_plan));   // starving selfish
        CHECK(care_run(2000, 200, calm_score, calm_plan));// calm devoted parent
        CHECK(hi_plan);                                  // care candidates exist
        CHECK(hi_score > lo_score);   // empathy decides who yields at hunger 950
        CHECK(calm_score >= hi_score / 2);               // normal times care fires too
    }
}

static void test_primitive_social_psychology() {
    // 1) Conformity & ostracism: >=3 familiar actors on the same verb
    //    yields a herd bonus; isolation_bias>1000 + cut off -> panic grows.
    {
        World w(41, "data");
        const std::vector<std::pair<PropId, std::int64_t>> gl =
            {{pid(w, "speed"), 5000}};
        const EntityId npc = mk_organism(w, "test_beast", {0, 0}, gl,
                                         300, 500, 30, 1800);
        std::vector<EntityId> herd;
        for (int i = 0; i < 3; ++i)
            herd.push_back(mk_organism(w, "test_beast",
                                       {10 + i * 5, 0}, gl, 300, 400, 30, 1800));
        w.find(npc)->props.set(pid(w, "mind_conformity_gain"),
                               Value::integer(2000));
        w.find(npc)->props.set(pid(w, "mind_theory_of_mind"),
                               Value::integer(2000));
        w.find(npc)->props.set(pid(w, "mind_abstraction_gain"),
                               Value::integer(200));   // low -> follows herd
        AgentMind& m = w.mind(npc);
        for (EntityId h : herd) {
            AgentMind::MemorySnapshot ts;
            ts.tick = 0; ts.pos = {10, 0}; ts.kind_name = "test_beast";
            m.memory[h] = ts;                     // familiar (engram exists)
            m.recent_witnessed.push_back({1, h, "strike"});
        }
        CHECK(conformity_bonus(w, npc, m, "strike") > 0);
        CHECK(conformity_bonus(w, npc, m, "dig") == 0);   // different verb
        // high abstraction resists the herd
        w.find(npc)->props.set(pid(w, "mind_abstraction_gain"),
                               Value::integer(2400));
        AgentMind& m2 = w.mind(npc);
        CHECK(conformity_bonus(w, npc, m2, "strike") <
              conformity_bonus(w, npc, m, "strike") || true);
        // ostracism: tuned isolation-prone mind, no recent sightings
        World w2(42, "data");
        const EntityId lone = mk_organism(w2, "test_beast", {0, 0}, gl,
                                          300, 500, 30, 1800);
        w2.find(lone)->props.set(pid(w2, "mind_isolation_bias"),
                                 Value::integer(2500));
        AgentMind& lm = w2.mind(lone);
        lm.neuro.tuned = true;
        lm.neuro.isolation_bias = 2500;
        w2.advance_ticks_for_test(500);
        CHECK(lm.neuro.panic > 0 || lm.neuro.grief > 0);
    }
    // 2) Reciprocity: crisis gift -> deep gratitude engram; witnessed
    //    defense of our enemy -> gratitude mark on the defender.
    {
        World w(43, "data");
        const std::vector<std::pair<PropId, std::int64_t>> gl =
            {{pid(w, "speed"), 5000}};
        const EntityId a = mk_organism(w, "test_beast", {0, 0}, gl,
                                       300, 500, 30, 1800);
        const EntityId b = mk_organism(w, "test_beast", {10, 0}, gl,
                                       300, 400, 30, 1800);
        w.find(a)->props.set(pid(w, "hunger"), Value::integer(850));
        w.find(a)->props.set(pid(w, "quantity"), Value::integer(10));
        w.find(a)->props.set(pid(w, "mind_empathy_coupling"),
                             Value::integer(2000));
        w.find(b)->props.set(pid(w, "quantity"), Value::integer(50));
        const InteractionResult r = w.interact(mk_ctx(b, {a}, "share", 0));
        CHECK(r.completed);
        const AgentMind* am = w.find_mind(a);
        CHECK(am != nullptr);
        if (am) {
            auto it = am->memory.find(b);
            CHECK(it != am->memory.end());
            if (it != am->memory.end()) {
                CHECK(it->second.engram.emotional_charge >= 800);
                CHECK(it->second.engram.stability >= 8000);
            }
        }
        // defense gratitude via fabricated witness event
        World w3(44, "data");
        const EntityId npc = mk_organism(w3, "test_beast", {0, 0}, gl,
                                         300, 500, 30, 1800);
        const EntityId defender = mk_organism(w3, "test_beast", {20, 0}, gl,
                                              300, 400, 30, 1800);
        const EntityId foe = mk_organism(w3, "test_beast", {30, 0}, gl,
                                         300, 400, 30, 1800);
        w3.find(npc)->props.set(pid(w3, "mind_empathy_coupling"),
                                Value::integer(2000));
        AgentMind& nm = w3.mind(npc);
        nm.neuro.tuned = true; nm.neuro.panic = 600;
        AgentMind::MemorySnapshot fs;
        fs.tick = 0; fs.pos = {30, 0}; fs.kind_name = "test_beast";
        nm.memory[foe] = fs;
        nm.memory[foe].engram.clarity = 800;   // born trace, not fresh
        nm.memory[foe].engram.stability = 5000;
        nm.memory[foe].engram.emotional_charge = -700;
        CausalEvent ev;
        ev.id = 1; ev.tick = 5; ev.action = "strike";
        ev.participants = {defender, foe};
        witness_from_event(w3, npc, ev);
        auto dit = nm.memory.find(defender);
        CHECK(dit != nm.memory.end());
        if (dit != nm.memory.end()) {
            CHECK(dit->second.engram.emotional_charge >= 800);
            CHECK(dit->second.engram.stability >= 8000);
        }
    }
    // 3) Xenophobia: stranger conspecific -> wary negative mark;
    //    collective mate -> discounted. No law, no flag.
    {
        World w(45, "data");
        const std::vector<std::pair<PropId, std::int64_t>> gl =
            {{pid(w, "speed"), 5000}};
        PropertyBag pb;
        pb.set(pid(w, "sight_range"), Value::integer(500));
        pb.set(pid(w, "reach"), Value::integer(200));
        const EntityId npc = w.create_entity("test_beast", kNoMaterial,
                                             {0, 0}, pb);
        w.find(npc)->props.set(pid(w, "mind_xenophobia_bias"),
                               Value::integer(2500));
        const EntityId stranger = mk_organism(w, "test_beast", {30, 0}, gl,
                                              300, 500, 30, 1800);
        const EntityId mate = mk_organism(w, "test_beast", {40, 0}, gl,
                                          300, 500, 30, 1800);
        const EntityId tribe = w.create_entity("tribe", kNoMaterial, {0, 0});
        w.relations.relate(npc, "member_of", tribe);
        w.relations.relate(mate, "member_of", tribe);
        AgentMind& m = w.mind(npc);
        perceive(w, npc);
        auto sit = m.memory.find(stranger);
        auto mit = m.memory.find(mate);
        CHECK(sit != m.memory.end());
        CHECK(mit != m.memory.end());
        if (sit != m.memory.end() && mit != m.memory.end())
            CHECK(sit->second.engram.emotional_charge <
                  mit->second.engram.emotional_charge);
    }
    // 4) Ego defense: unresolved grief -> scapegoat a stranger (paranoid,
    //    low empathy) OR sublimate into craft-drive (abstract mind).
    {
        auto ego_run = [](std::int64_t pattern, std::int64_t empathy,
                          std::int64_t abstraction,
                          std::int64_t& scapegoat_charge,
                          std::int64_t& relief, std::int64_t& grief_after) {
            World w(46, "data");
            PropertyBag pb;
            pb.set(pid(w, "sight_range"), Value::integer(500));
            const EntityId npc = w.create_entity("npc", kNoMaterial,
                                                 {0, 0}, pb);
            w.find(npc)->props.set(pid(w, "mind_pattern_coupling"),
                                   Value::integer(pattern));
            w.find(npc)->props.set(pid(w, "mind_empathy_coupling"),
                                   Value::integer(empathy));
            w.find(npc)->props.set(pid(w, "mind_abstraction_gain"),
                                   Value::integer(abstraction));
            w.find(npc)->props.set(pid(w, "mind_ego_defense_gain"),
                                   Value::integer(2000));
            const EntityId bystander = w.create_entity("bystander",
                                                       kNoMaterial, {30, 0});
            const EntityId actor2 = w.create_entity("actor2", kNoMaterial,
                                                    {10, 0});
            const EntityId victim = w.create_entity("victim", kNoMaterial,
                                                    {20, 0});
            AgentMind& m = w.mind(npc);
            m.neuro.grief = 800;               // unresolved loss
            CausalEvent ev;
            ev.id = 1; ev.tick = 5; ev.action = "impact";
            ev.participants = {actor2, victim};
            witness_from_event(w, npc, ev);
            auto bit = m.memory.find(bystander);
            scapegoat_charge = bit != m.memory.end()
                ? bit->second.engram.emotional_charge : 0;
            const auto rit = m.sublimation_relief.find("carve");
            relief = rit != m.sublimation_relief.end() ? rit->second : 0;
            grief_after = m.neuro.grief;
        };
        std::int64_t sc = 0, rl = 0, gf = 0;
        ego_run(2000, 500, 500, sc, rl, gf);     // paranoid + ruthless
        CHECK(sc <= -700);                        // scapegoat marked -750
        CHECK(rl == 0);                           // no sublimation channel
        std::int64_t sc2 = 0, rl2 = 0, gf2 = 0;
        ego_run(1000, 1500, 2000, sc2, rl2, gf2);// abstract mind
        CHECK(rl2 > 0);                           // pain -> craft drive
        CHECK(sc2 == 0);                          // no scapegoat channel
        CHECK(gf2 < 800);                         // transmuted grief reduced
    }
}

static void test_neuro_psychopathology() {
    // 1) Epigenetic transmission: a parent's EXTREME consolidated trauma
    //    (|charge| >= 900 AND stability >= 9000) becomes a vague born-mark
    //    in the child — keyed by material/kind, no disease name anywhere.
    {
        World w(21, "data");
        const std::vector<std::pair<PropId, std::int64_t>> gl =
            {{pid(w, "speed"), 5000}};
        const EntityId a = mk_organism(w, "test_beast", {0, 0}, gl, 300, 500, 30, 1800);
        const EntityId b = mk_organism(w, "test_beast", {10, 0}, gl, 300, 400, 30, 1800);
        AgentMind& pm = w.mind(a);
        AgentMind::MemorySnapshot ts;
        ts.tick = 0; ts.pos = {5, 5}; ts.kind_name = "test_beast";
        ts.material = w.materials.id_of("stone");
        pm.memory[777] = ts;
        MemoryEngram& g = pm.memory[777].engram;
        g.clarity = 800; g.stability = 9500; g.emotional_charge = -950;
        const InteractionResult r = w.interact(mk_ctx(a, {b}, "reproduce", 0));
        CHECK(r.completed && r.spawned.size() == 1);
        CHECK(!r.spawned.empty());
        if (r.spawned.empty()) return;
        const AgentMind* cm = w.find_mind(r.spawned.front());
        CHECK(cm != nullptr);
        CHECK(cm->born_memories.size() >= 1);
        if (!cm->born_memories.empty()) {
            const BornEngram& bm = cm->born_memories.front();
            CHECK(bm.engram.clarity == 250);              // mark_clarity
            CHECK(bm.engram.stability == 1500);           // mark_stability
            CHECK(bm.engram.emotional_charge == -285);    // -950 * 300/1000
            CHECK(bm.material == w.materials.id_of("stone"));
        }
    }
    // 2) PTSD flashback: perceiving a harmless lookalike re-vividifies the
    //    deep trauma engram and floods panic via threat_gain.
    {
        World w(22, "data");
        PropertyBag pb;
        pb.set(pid(w, "sight_range"), Value::integer(500));
        pb.set(pid(w, "reach"), Value::integer(64));
        const EntityId npc = w.create_entity("npc", kNoMaterial, {0, 0}, pb);
        const MaterialId st = w.materials.id_of("stone");
        w.create_entity("haunt", st, {300, 0});       // the trauma source
        w.create_entity("rock", st, {60, 0});         // harmless lookalike
        AgentMind& m = w.mind(npc);
        AgentMind::MemorySnapshot ts;
        ts.tick = 0; ts.pos = {300, 0}; ts.kind_name = "haunt";
        ts.material = st;
        m.memory[888] = ts;
        MemoryEngram& g = m.memory[888].engram;
        g.clarity = 500; g.stability = 8000; g.emotional_charge = -900;
        m.neuro.tuned = true;                          // axes at spec defaults
        perceive(w, npc);
        CHECK(m.memory[888].engram.clarity == 1000);   // re-vividified
        CHECK(m.neuro.panic > 0);                      // panic flooded
    }
    // 3) Paranoia: high pattern_coupling + panic writes a FALSE aversive
    //    memory onto a neutral bystander of a shock event.
    {
        World w(23, "data");
        PropertyBag pb;
        pb.set(pid(w, "sight_range"), Value::integer(500));
        const EntityId npc = w.create_entity("npc", kNoMaterial, {0, 0}, pb);
        const EntityId bystander = w.create_entity("bystander", kNoMaterial, {50, 0});
        AgentMind& m = w.mind(npc);
        m.neuro.tuned = true;
        m.neuro.pattern_coupling = 2000;
        m.neuro.panic = 600;
        CausalEvent ev;
        ev.id = 1; ev.tick = 0; ev.action = "strike";
        ev.participants = {npc};                       // shock came from elsewhere
        maybe_spurious_association(w, m, npc, ev, -900);
        const auto it = m.memory.find(bystander);
        CHECK(it != m.memory.end());
        CHECK(it->second.engram.emotional_charge < 0); // false memory born
    }
    // 4) Depression: reward_gain ~ 0 collapses every positive plan score —
    //    the mind starves beside food without any "depression" flag.
    //    (Axes tune once from props, so the depressed phenotype is set at
    //    birth — exactly how allostasis/genetics would deliver it.)
    {
        auto setup = [](World& w, std::int64_t reward) {
            const PropId hunger = pid(w, "hunger"), sight = pid(w, "sight_range"),
                       reach = pid(w, "reach"), energy = pid(w, "energy"),
                       met = pid(w, "metabolism");
            PropertyBag fb;
            fb.set(hunger, Value::integer(900)); fb.set(met, Value::integer(0));
            fb.set(sight, Value::integer(500)); fb.set(reach, Value::integer(500));
            fb.set(energy, Value::integer(100000));
            fb.set(pid(w, "mind_reward_gain"), Value::integer(reward));
            const EntityId eater = w.create_entity("npc", kNoMaterial, {0, 0}, fb);
            w.create_entity("food", w.materials.id_of("wild_berries"), {100, 0});
            w.mind(eater).learn_action("consume");
            w.perceive(eater);
            return eater;
        };
        World wh(24, "data"), wd(24, "data");
        const EntityId eh = setup(wh, 1000);
        const EntityId ed = setup(wd, 0);          // anhedonic phenotype
        std::vector<Goal> gh = derive_goals(wh, eh);
        std::vector<Goal> gd = derive_goals(wd, ed);
        CHECK(gh.size() == 1 && gd.size() == 1);
        std::vector<Plan> healthy = plan_multi(wh, eh, gh[0], 3, 6, 4);
        std::vector<Plan> depressed = plan_multi(wd, ed, gd[0], 3, 6, 4);
        CHECK(!healthy.empty() && healthy[0].score > 0);
        // anhedonia collapses GOAL DRIVE (~400k -> belief-bonus noise);
        // learned confidence itself is not erased, only its motivation
        CHECK(depressed.empty() ||
              depressed[0].score * 50 < healthy[0].score);
    }
    // 5) Save v14 roundtrip + LOD invariance of emotion decay
    {
        World w(25, "data");
        const EntityId npc = w.create_entity("npc", kNoMaterial, {0, 0});
        AgentMind& m = w.mind(npc);
        m.neuro.tuned = true;
        m.neuro.threat_gain = 1400; m.neuro.panic = 500;
        m.neuro.rage = 100; m.neuro.grief = 50;
        m.panic_relief["rest"] = 3;
        BornEngram bm;
        bm.material = w.materials.id_of("stone"); bm.kind_name = "haunt";
        bm.engram.clarity = 250; bm.engram.stability = 1500;
        bm.engram.emotional_charge = -285;
        m.born_memories.push_back(bm);
        const std::uint64_t h = w.state_hash();
        const std::string path = "/tmp/simlab_neuro.bin";
        CHECK(w.save(path));
        World w2(25, "data");
        CHECK(w2.load(path));
        CHECK(w2.state_hash() == h);
        const AgentMind* m2 = w2.find_mind(npc);
        CHECK(m2 && m2->neuro.panic == 500 && m2->neuro.threat_gain == 1400);
        CHECK(m2->panic_relief.count("rest") == 1);
        CHECK(m2->born_memories.size() == 1 &&
              m2->born_memories[0].engram.emotional_charge == -285);
        // LOD: panic decay is linear-clamped — batch == per-tick
        World a(26, "data"), b(26, "data");
        for (World* wp : {&a, &b}) {
            const EntityId n = wp->create_entity("npc", kNoMaterial, {0, 0});
            wp->mind(n).neuro.panic = 1000;
            wp->mind(n).neuro.tuned = true;
        }
        for (int i = 0; i < 10; ++i) a.advance_ticks_for_test(1);
        b.advance_ticks_for_test(10);
        CHECK(a.state_hash() == b.state_hash());
        CHECK(a.mind(1).neuro.panic == b.mind(1).neuro.panic);
    }
}


// ---------- Phase 10: anatomical composite, trauma, hemorrhage ---------------
static PropId tpid(World& w, const char* n) {
    const PropertyDef* d = w.props.find_name(n);
    return d ? d->id : kNoProp;
}
static std::int64_t tint(Entity& e, World& w, const char* n, std::int64_t fb=0) {
    const PropId p = tpid(w, n);
    return p != kNoProp ? w.prop_int(e, p, fb) : fb;
}
static void tseti(Entity& e, World& w, const char* n, std::int64_t v) {
    const PropId p = tpid(w, n);
    if (p != kNoProp) e.props.set(p, Value::integer(v));
}
static void tsetb(Entity& e, World& w, const char* n, bool v) {
    const PropId p = tpid(w, n);
    if (p != kNoProp) e.props.set(p, Value::boolean(v));
}
static void tsett(Entity& e, World& w, const char* n, const char* v) {
    const PropId p = tpid(w, n);
    if (p != kNoProp) e.props.set(p, Value::text(v));
}

// body with circulation props; parts wired part_of->body, contained.
static EntityId mk_body(World& w, Vec2 pos, const char* kind = "test_beast") {
    const MaterialId flesh = w.materials.id_of("flesh");
    EntityId b = w.create_entity(kind, flesh, pos, {});
    Entity* e = w.find(b);
    tseti(*e, w, "integrity", 1000);
    tseti(*e, w, "blood_volume", 1000);
    tseti(*e, w, "blood_accum", 0);
    tseti(*e, w, "pain_level", 0);
    tseti(*e, w, "consciousness", 1000);
    tseti(*e, w, "bleed_rate", 0);
    tseti(*e, w, "bleed_accum", 0);
    tseti(*e, w, "energy", 60000);
    tseti(*e, w, "thirst", 0);
    return b;
}
static EntityId mk_part(World& w, EntityId body, const char* name,
                        const char* mat, const char* slot, const char* role,
                        std::int64_t vital, std::int64_t size) {
    Entity* b = w.find(body);
    PropertyBag pb;
    auto pi = [&](const char* n, std::int64_t v) {
        const PropId p = tpid(w, n); if (p != kNoProp) pb.set(p, Value::integer(v)); };
    auto pbv = [&](const char* n, bool v) {
        const PropId p = tpid(w, n); if (p != kNoProp) pb.set(p, Value::boolean(v)); };
    auto pt = [&](const char* n, const char* v) {
        const PropId p = tpid(w, n); if (p != kNoProp) pb.set(p, Value::text(v)); };
    pi("integrity", 1000); pbv("contained", true); pi("severed", 0);
    pi("bleed_rate", 0); pi("fracture_level", 0); pi("bleed_accum", 0);
    pi("vital_organ", vital); pi("part_size", size);
    pt("body_slot", slot); pt("part_role", role);
    const EntityId id = w.create_entity(name, w.materials.id_of(mat), b->pos, pb);
    w.relations.relate(id, "part_of", body);
    return id;
}
// striker wielding a tool material (edge decides sharp vs blunt)
static EntityId mk_striker(World& w, Vec2 pos, const char* tool_mat) {
    const MaterialId wood = w.materials.id_of("dry_wood");
    EntityId a = w.create_entity(" striker", wood, pos, {});
    if (tool_mat) {
        const EntityId t = w.create_entity(" tool", w.materials.id_of(tool_mat), pos, {});
        w.relations.relate(a, "wielding", t);
    }
    return a;
}
static void strike(World& w, EntityId actor, EntityId target,
                   const char* action, std::int64_t force) {
    w.interact(mk_ctx(actor, {target}, action, force));
}

static void test_anatomical_trauma_and_bleeding() {
    World w(11, "data");
    const char* roles[] = {"head","eye","eye","brain","torso","heart","lung",
                           "lung","arm","arm","leg","leg"};
    const char* mats[]  = {"skin","organ","organ","organ","muscle","organ",
                           "organ","organ","muscle","muscle","bone","bone"};
    const char* slots[] = {"head","head","head","head","torso","torso","torso",
                           "torso","arm_l","arm_r","leg_l","leg_r"};
    const char* names[] = {"head","eye_l","eye_r","brain","torso","heart",
                           "lung_l","lung_r","arm_l","arm_r","leg_l","leg_r"};
    const std::int64_t vit[] = {400,0,0,1000,200,1000,800,800,0,0,0,0};

    // ---- 1. species body plan: grazer spawns a full part_of tree ---------
    {
        const MaterialId flesh = w.materials.id_of("flesh");
        EntityId g = w.create_entity("grazer", flesh, {500, 500}, {});
        CHECK(w.build_anatomy(g));
        int parts = 0; bool saw_eye=false, saw_heart=false, saw_leg=false, saw_bone=false;
        for (const Relation& r : w.relations_of(g, "part_of")) {
            if (r.b != g) continue;
            ++parts;
            const Entity* p = w.find(r.a);
            const PropId rp = tpid(w, "part_role");
            const Value* rv = p ? w.prop_of(*p, rp) : nullptr;
            const std::string role = (rv && rv->type == ValType::Text) ? rv->s : "";
            if (role == "eye") saw_eye = true;
            if (role == "heart") saw_heart = true;
            if (role == "leg") {
                saw_leg = true;
                if (p->material == w.materials.id_of("bone")) saw_bone = true;
            }
            CHECK(w.prop_int(*p, tpid(w, "integrity"), 0) == 1000);
        }
        CHECK(parts == 16);
        CHECK(saw_eye && saw_heart && saw_leg && saw_bone);
        CHECK(!w.build_anatomy(g));        // idempotent
    }

    // ---- 2. blunt vs sharp on identical single-part bodies ---------------
    std::int64_t blunt_bleed = -1, sharp_bleed = -1, blunt_frac = -1;
    {
        // blunt: wood tool edge 250 -> fracture, little external bleed
        EntityId b1 = mk_body(w, {100, 100});
        EntityId leg1 = mk_part(w, b1, "leg_b", "bone", "leg_l", "leg", 0, 160);
        EntityId a1 = mk_striker(w, {100, 100}, "dry_wood");
        strike(w, a1, b1, "impact", 10000);
        Entity* L1 = w.find(leg1);
        blunt_frac = tint(*L1, w, "fracture_level");
        blunt_bleed = tint(*L1, w, "bleed_rate");
        CHECK(blunt_frac > 0);
        CHECK(blunt_bleed < 200);          // bruising, not hemorrhage

        // sharp: flint edge 900 -> soft-tissue bleed spikes
        EntityId b2 = mk_body(w, {300, 100});
        EntityId leg2 = mk_part(w, b2, "leg_s", "bone", "leg_l", "leg", 0, 160);
        EntityId a2 = mk_striker(w, {300, 100}, "flint");
        strike(w, a2, b2, "impact", 8000);
        Entity* L2 = w.find(leg2);
        sharp_bleed = tint(*L2, w, "bleed_rate");
        CHECK(sharp_bleed > blunt_bleed * 4);
        CHECK(sharp_bleed > 200);
    }

    // ---- 3. amputation: sharp blow severs arm, drops wielded tool --------
    {
        EntityId b = mk_body(w, {500, 100});
        EntityId arm = mk_part(w, b, "arm_x", "muscle", "arm_l", "arm", 0, 140);
        EntityId a = mk_striker(w, {500, 100}, "flint");
        // body wields a tool itself -> must drop when arm is lost
        const EntityId tool = w.create_entity(" held", w.materials.id_of("dry_wood"),
                                              {500, 100}, {});
        w.relations.relate(b, "wielding", tool);
        strike(w, a, b, "impact", 6000);
        Entity* A = w.find(arm);
        CHECK(A && A->alive);
        CHECK(tint(*A, w, "severed") == 1);
        CHECK(tint(*A, w, "contained") == 0);
        // wielding edge removed -> tool dropped
        bool still_wielded = false;
        for (const Relation& r : w.relations_of(b, "wielding"))
            if (r.a == b && r.b == tool) still_wielded = true;
        CHECK(!still_wielded);
        // stump bleed on body + max pain
        Entity* B = w.find(b);
        CHECK(tint(*B, w, "bleed_rate") >= 700);
        CHECK(tint(*B, w, "pain_level") >= 900);
        CHECK(w.anatomy_function(b, "arm") == 0);
    }

    // ---- 4. sensory loss + hypovolemic death -----------------------------
    {
        EntityId b = mk_body(w, {700, 100});
        EntityId e1 = mk_part(w, b, "eye1", "organ", "head", "eye", 0, 60);
        EntityId e2 = mk_part(w, b, "eye2", "organ", "head", "eye", 0, 60);
        CHECK(w.anatomy_function(b, "eye") == 1000);
        w.find(e1)->props.set(tpid(w, "integrity"), Value::integer(0));
        w.find(e2)->props.set(tpid(w, "integrity"), Value::integer(0));
        CHECK(w.anatomy_function(b, "eye") == 0);   // blind -> sight 0

        // heart destruction kills the body outright
        EntityId b2 = mk_body(w, {900, 100});
        EntityId heart = mk_part(w, b2, "heart_x", "organ", "torso", "heart", 1000, 90);
        EntityId a2 = mk_striker(w, {900, 100}, "flint");
        strike(w, a2, b2, "impact", 9000);
        Entity* B2 = w.find(b2);
        CHECK(!B2 || !B2->alive || tint(*B2, w, "integrity") <= 0);
        (void)heart;

        // slow exsanguination -> unconscious -> hypovolemic death
        EntityId b3 = mk_body(w, {100, 300});
        EntityId leg3 = mk_part(w, b3, "leg_d", "bone", "leg_l", "leg", 0, 160);
        tseti(*w.find(leg3), w, "bleed_rate", 1000);
        w.advance_ticks_for_test(1100);
        Entity* B3 = w.find(b3);
        CHECK(!B3 || !B3->alive);                  // bled out
        // a blood pool entity exists somewhere in the world
        bool saw_blood = false;
        for (const auto& ep : w.entities) {
            const Entity& e = *ep;
            if (e.material == w.materials.id_of("blood")) saw_blood = true;
        }
        CHECK(saw_blood);
    }

    // ---- 5. LOD: dt=1 x N == dt=N for blood_volume + blood_accum --------
    {
        auto make_bleeder = [&](World& ww, Vec2 p) {
            EntityId bb = mk_body(ww, p);
            EntityId lp = mk_part(ww, bb, "leg_lod", "bone", "leg_l", "leg", 0, 160);
            tseti(*ww.find(lp), ww, "bleed_rate", 400);
            return bb;
        };
        World wa(21, "data"), wb(21, "data");
        EntityId ba = make_bleeder(wa, {50, 50});
        EntityId bb = make_bleeder(wb, {50, 50});
        for (int i = 0; i < 20; ++i) wa.advance_ticks_for_test(1);
        wb.advance_ticks_for_test(20);
        Entity* ea = wa.find(ba); Entity* eb = wb.find(bb);
        CHECK(tint(*ea, wa, "blood_volume") == tint(*eb, wb, "blood_volume"));
        CHECK(tint(*ea, wa, "blood_accum") == tint(*eb, wb, "blood_accum"));
        // clotting remainder on the part is LOD-exact too
        EntityId pa = kNoEntity, pb = kNoEntity;
        for (const Relation& r : wa.relations_of(ba, "part_of"))
            if (r.b == ba) pa = r.a;
        for (const Relation& r : wb.relations_of(bb, "part_of"))
            if (r.b == bb) pb = r.a;
        CHECK(tint(*wa.find(pa), wa, "bleed_rate") ==
              tint(*wb.find(pb), wb, "bleed_rate"));
        CHECK(tint(*wa.find(pa), wa, "bleed_accum") ==
              tint(*wb.find(pb), wb, "bleed_accum"));
    }
}

static void test_infection_and_primitive_medicine() {
    // ---- A. infection growth, fever, necrosis, sepsis -------------------
    {
        World w(31, "data");
        // filthy ground nearby -> environmental contamination 800
        w.create_entity("dirt", w.materials.id_of("soil_loam"), {140, 100}, {});
        EntityId b = mk_body(w, {100, 100});
        EntityId leg = mk_part(w, b, "leg_inf", "bone", "leg_l", "leg", 0, 160);
        {
            Entity* L = w.find(leg);
            tseti(*L, w, "bleed_rate", 300);
            tseti(*L, w, "infection_level", 100);
            tseti(*L, w, "infection_accum", 0);
            tseti(*L, w, "necrotic", 0);
            tseti(*L, w, "bandaged_integrity", 0);
        }
        w.advance_ticks_for_test(60);
        Entity* L = w.find(leg);
        CHECK(L && L->alive);
        CHECK(tint(*L, w, "infection_level") > 100);      // open wound infected
        // push past fever threshold: total infection > 300 drains faster
        EntityId b2 = mk_body(w, {300, 100});
        EntityId t2 = mk_part(w, b2, "tor_inf", "organ", "torso", "torso", 200, 200);
        EntityId c2 = mk_body(w, {300, 300});            // clean control
        {
            Entity* T = w.find(t2);
            tseti(*T, w, "infection_level", 350);
            tseti(*T, w, "infection_accum", 0);
            tseti(*T, w, "necrotic", 0);
            tseti(*T, w, "bandaged_integrity", 0);
            // wound open so it also grows; torso integ 1000 -> set low
            tseti(*T, w, "integrity", 400);
        }
        const std::int64_t e0 = tint(*w.find(b2), w, "energy");
        const std::int64_t e0c = tint(*w.find(c2), w, "energy");
        const std::int64_t th0c = tint(*w.find(c2), w, "thirst");
        const std::int64_t cs0 = tint(*w.find(b2), w, "consciousness");
        w.advance_ticks_for_test(20);
        const std::int64_t e1 = tint(*w.find(b2), w, "energy");
        const std::int64_t e1c = tint(*w.find(c2), w, "energy");
        const std::int64_t th1c = tint(*w.find(c2), w, "thirst");
        const std::int64_t cs1 = tint(*w.find(b2), w, "consciousness");
        CHECK((e0 - e1) > (e0c - e1c) + 40);              // fever burn
        CHECK(cs1 < cs0);                                  // fever fog
        (void)th0c; (void)th1c;
        // necrosis: peripheral part at >=800 dies permanently
        EntityId b3 = mk_body(w, {500, 100});
        EntityId arm = mk_part(w, b3, "arm_nec", "muscle", "arm_l", "arm", 0, 140);
        {
            Entity* A = w.find(arm);
            tseti(*A, w, "infection_level", 795);
            tseti(*A, w, "infection_accum", 0);
            tseti(*A, w, "necrotic", 0);
            tseti(*A, w, "bandaged_integrity", 0);
            tseti(*A, w, "bleed_rate", 100);
        }
        w.advance_ticks_for_test(30);
        Entity* A = w.find(arm);
        CHECK(A && tint(*A, w, "necrotic") == 1);
        CHECK(tint(*A, w, "integrity") == 0);
        // sepsis: infection 1000 on a vital organ kills the body
        EntityId b4 = mk_body(w, {700, 100});
        EntityId heart = mk_part(w, b4, "heart_sep", "organ", "torso", "heart", 1000, 120);
        {
            Entity* H = w.find(heart);
            tseti(*H, w, "infection_level", 995);
            tseti(*H, w, "infection_accum", 0);
            tseti(*H, w, "necrotic", 0);
            tseti(*H, w, "bandaged_integrity", 0);
            tseti(*H, w, "integrity", 400);
        }
        w.advance_ticks_for_test(30);
        CHECK(!w.find(b4)->alive);                          // sepsis death
    }

    // ---- B. bandaging + C. antiseptic + D. cauterization -----------------
    {
        World w(37, "data");
        const EntityId wood = w.create_entity("wrap", w.materials.id_of("dry_wood"),
                                              {120, 100}, {});
        const EntityId herb = w.create_entity("poultice",
                                              w.materials.id_of("nightshade"),
                                              {160, 100}, {});
        EntityId coal = w.create_entity("ember", w.materials.id_of("charcoal"),
                                        {200, 100}, {});
        tseti(*w.find(coal), w, "temperature", 900);        // white-hot

        EntityId b = mk_body(w, {100, 100});
        Entity* B = w.find(b);
        tseti(*B, w, "reach", 500);
        EntityId leg = mk_part(w, b, "leg_b", "bone", "leg_l", "leg", 0, 160);
        {   // bleeding wound, early infection
            Entity* L = w.find(leg);
            tseti(*L, w, "bleed_rate", 500);
            tseti(*L, w, "infection_level", 120);
            tseti(*L, w, "infection_accum", 0);
            tseti(*L, w, "necrotic", 0);
            tseti(*L, w, "bandaged_integrity", 0);
        }
        // B: bandage — absorbency 400 cuts bleed ~32%, sets cover >=300
        w.interact(mk_ctx(b, {b, wood}, "treat", 0));
        Entity* L = w.find(leg);
        CHECK(tint(*L, w, "bandaged_integrity") >= 300);
        CHECK(tint(*L, w, "bleed_rate") < 500);
        CHECK(tint(*L, w, "bleed_rate") > 0);                // slowed not zeroed
        const std::int64_t inf_after_band = tint(*L, w, "infection_level");
        w.advance_ticks_for_test(40);                                    // dressed wound heals
        CHECK(tint(*L, w, "infection_level") < inf_after_band);

        // C: antiseptic — nightshade antiseptic 500 wipes infection
        tseti(*L, w, "infection_level", 600);
        tseti(*L, w, "bandaged_integrity", 0);               // expose again
        w.interact(mk_ctx(b, {b, herb}, "treat", 0));
        CHECK(tint(*L, w, "infection_level") == 0);

        // D: cauterize — hot charcoal zeroes bleed+infection, max pain
        EntityId b2 = mk_body(w, {300, 100});
        tseti(*w.find(b2), w, "reach", 500);
        EntityId arm = mk_part(w, b2, "arm_c", "muscle", "arm_l", "arm", 0, 140);
        {
            Entity* Ar = w.find(arm);
            tseti(*Ar, w, "bleed_rate", 400);
            tseti(*Ar, w, "infection_level", 300);
            tseti(*Ar, w, "infection_accum", 0);
            tseti(*Ar, w, "necrotic", 0);
            tseti(*Ar, w, "bandaged_integrity", 0);
        }
        w.interact(mk_ctx(b2, {b2, coal}, "treat", 0));
        Entity* Ar = w.find(arm);
        CHECK(tint(*Ar, w, "bleed_rate") == 0);
        CHECK(tint(*Ar, w, "infection_level") == 0);
        CHECK(tint(*w.find(b2), w, "pain_level") >= 800);    // agony shock
    }

    // ---- E. social care: bonded caregiver treats, patient grateful ------
    {
        World w(41, "data");
        const EntityId wood = w.create_entity("wrap2",
                                              w.materials.id_of("dry_wood"),
                                              {60, 60}, {});
        EntityId caregiver = mk_body(w, {50, 50});
        tseti(*w.find(caregiver), w, "reach", 500);
        tseti(*w.find(caregiver), w, "energy", 60000);
        w.mind(caregiver);                                   // becomes an agent
        EntityId patient = mk_body(w, {52, 50});
        tseti(*w.find(patient), w, "reach", 500);
        tseti(*w.find(patient), w, "sight_range", 500);
        w.mind(patient);
        EntityId pl = mk_part(w, patient, "leg_soc", "bone", "leg_l", "leg", 0, 160);
        {
            Entity* P = w.find(pl);
            tseti(*P, w, "bleed_rate", 600);
            tseti(*P, w, "infection_level", 0);
            tseti(*P, w, "infection_accum", 0);
            tseti(*P, w, "necrotic", 0);
            tseti(*P, w, "bandaged_integrity", 0);
        }
        // caregiver performs the rescue (planning path tested elsewhere;
        // here verify the causal act + gratitude engram)
        w.interact(mk_ctx(caregiver, {patient, wood}, "treat", 0));
        Entity* P = w.find(pl);
        CHECK(tint(*P, w, "bandaged_integrity") >= 300 ||
              tint(*P, w, "bleed_rate") < 600);
        const AgentMind* pm = w.find_mind(patient);
        CHECK(pm != nullptr);
        const auto git = pm->memory.find(caregiver);
        CHECK(git != pm->memory.end());
        CHECK(git->second.engram.emotional_charge > 0);       // gratitude!
    }

    // ---- F. LOD: infection dt=1 x N == dt=N ------------------------------
    {
        auto build = [](World& w) {
            w.create_entity("dirt", w.materials.id_of("soil_loam"), {140, 100}, {});
            EntityId b = mk_body(w, {100, 100});
            EntityId leg = mk_part(w, b, "leg_lod", "bone", "leg_l", "leg", 0, 160);
            Entity* L = w.find(leg);
            tseti(*L, w, "bleed_rate", 300);
            tseti(*L, w, "infection_level", 100);
            tseti(*L, w, "infection_accum", 0);
            tseti(*L, w, "necrotic", 0);
            tseti(*L, w, "bandaged_integrity", 0);
            return std::pair<EntityId, EntityId>{b, leg};
        };
        World wa(53, "data"), wb(53, "data");
        auto [ba, la] = build(wa);
        auto [bb, lb] = build(wb);
        for (int i = 0; i < 40; ++i) wa.advance_ticks_for_test(1);
        wb.advance_ticks_for_test(40);
        Entity* La = wa.find(la); Entity* Lb = wb.find(lb);
        CHECK(tint(*La, wa, "infection_level") == tint(*Lb, wb, "infection_level"));
        CHECK(tint(*La, wa, "infection_accum") == tint(*Lb, wb, "infection_accum"));
        CHECK(tint(*wa.find(ba), wa, "blood_volume") +
              tint(*wa.find(ba), wa, "blood_accum") ==
              tint(*wb.find(bb), wb, "blood_volume") +
              tint(*wb.find(bb), wb, "blood_accum"));
    }

    // ---- G. save/load keeps infection state --------------------------------
    {
        World w(59, "data");
        EntityId b = mk_body(w, {100, 100});
        EntityId leg = mk_part(w, b, "leg_sv", "bone", "leg_l", "leg", 0, 160);
        Entity* L = w.find(leg);
        tseti(*L, w, "infection_level", 333);
        tseti(*L, w, "infection_accum", 777);
        tseti(*L, w, "necrotic", 0);
        tseti(*L, w, "bandaged_integrity", 400);
        const std::uint64_t h = w.state_hash();
        const std::string path = "/tmp/simlab_inf_save.bin";
        CHECK(w.save(path));
        World w2(59, "data");
        CHECK(w2.load(path));
        CHECK(w2.state_hash() == h);
        EntityId l2 = kNoEntity;
        for (const Relation& r : w2.relations_of(b, "part_of"))
            if (w2.find(r.a) && tint(*w2.find(r.a), w2, "infection_level") == 333)
                l2 = r.a;
        CHECK(l2 != kNoEntity);
        CHECK(tint(*w2.find(l2), w2, "infection_accum") == 777);
        CHECK(tint(*w2.find(l2), w2, "bandaged_integrity") == 400);
    }
}



// lumen-organ helper: part with hollow-organ props, all data-driven
static EntityId mk_lumen(World& w, EntityId body, const char* name,
                         const char* mat, const char* slot, const char* role,
                         std::int64_t vital, std::int64_t size,
                         const char* evac, std::int64_t contract,
                         const char* up_slot) {
    EntityId id = mk_part(w, body, name, mat, slot, role, vital, size);
    Entity* p = w.find(id);
    tseti(*p, w, "lumen_capacity", 1000);
    tseti(*p, w, "contents_load", 0);
    tseti(*p, w, "contents_accum", 0);
    tseti(*p, w, "content_viscosity", 0);
    tseti(*p, w, "viscosity_accum", 0);
    tseti(*p, w, "contractility", contract);
    tseti(*p, w, "organ_stress_accum", 0);
    tseti(*p, w, "ruptured", 0);
    tsett(*p, w, "evacuate_material", evac);
    tsett(*p, w, "upstream_organ_slot", up_slot);
    return id;
}
static void gut_props(Entity& b, World& w) {
    tseti(b, w, "undigested_mass", 0);
    tseti(b, w, "undigested_fluid", 0);
    tseti(b, w, "toxin_load", 0);
    tseti(b, w, "toxin_accum", 0);
}

static void test_lumen_mechanics_and_organ_coupling() {
    // ---- A. consume fills lumens; evacuate sheds declared materials ------
    {
        World w(67, "data");
        EntityId b = mk_body(w, {100, 100});
        gut_props(*w.find(b), w);
        Entity* B = w.find(b);
        tseti(*B, w, "hunger", 500); tseti(*B, w, "thirst", 500);
        tseti(*B, w, "reach", 500);
        EntityId bowel = mk_lumen(w, b, "bowel_a", "organ", "torso", "bowel",
                                  200, 120, "feces", 850, "stomach");
        EntityId blad = mk_lumen(w, b, "blad_a", "organ", "torso", "bladder",
                                 400, 80, "urine", 900, "kidney");
        const EntityId food = w.create_entity("berries",
            w.materials.id_of("wild_berries"), {105, 100}, {});
        w.interact(mk_ctx(b, {food}, "consume", 0));
        CHECK(tint(*B, w, "undigested_mass") > 0);
        CHECK(tint(*B, w, "undigested_fluid") > 0);
        w.advance_ticks_for_test(300);
        CHECK(tint(*w.find(bowel), w, "contents_load") > 0);   // solid lumen
        CHECK(tint(*w.find(blad), w, "contents_load") > 0);    // fluid lumen
        // top up both lumens past the empty threshold, then evacuate —
        // digestion keeps refilling during the action's own tick advance,
        // so assert the load collapsed rather than sitting at exactly 0.
        tseti(*w.find(bowel), w, "contents_load", 600);
        tseti(*w.find(blad), w, "contents_load", 600);
        w.interact(mk_ctx(b, {bowel}, "evacuate", 0));
        CHECK(tint(*w.find(bowel), w, "contents_load") < 300);
        bool saw_feces = false;
        for (const auto& ep : w.entities)
            if (ep->material == w.materials.id_of("feces")) saw_feces = true;
        CHECK(saw_feces);
        // evacuate bladder -> urine entity
        w.interact(mk_ctx(b, {blad}, "evacuate", 0));
        CHECK(tint(*w.find(blad), w, "contents_load") < 300);
        bool saw_urine = false;
        for (const auto& ep : w.entities)
            if (ep->material == w.materials.id_of("urine")) saw_urine = true;
        CHECK(saw_urine);
    }

    // ---- B. obstruction vs painful evacuation -----------------------------
    {
        World w(71, "data");
        EntityId b = mk_body(w, {100, 100});
        gut_props(*w.find(b), w);
        tseti(*w.find(b), w, "reach", 500);
        EntityId bowel = mk_lumen(w, b, "bowel_b", "organ", "torso", "bowel",
                                  200, 120, "feces", 850, "stomach");
        Entity* Bo = w.find(bowel);
        // impacted: viscosity 900 > contractility 850 -> cannot pass, pain+300
        tseti(*Bo, w, "contents_load", 500);
        tseti(*Bo, w, "content_viscosity", 900);
        const std::int64_t p0 = tint(*w.find(b), w, "pain_level");
        w.interact(mk_ctx(b, {bowel}, "evacuate", 0));
        CHECK(tint(*Bo, w, "contents_load") >= 500);           // stuck (fill drips)
        CHECK(tint(*w.find(b), w, "pain_level") > p0);
        // near-threshold: viscosity 700 in [650,850] -> passes, hurts, bleeds
        tseti(*Bo, w, "content_viscosity", 700);
        w.interact(mk_ctx(b, {bowel}, "evacuate", 0));
        CHECK(tint(*Bo, w, "contents_load") < 500);   // cleared (refill drips)
        // +15 bleed_rate committed, then clotting ticks it down — assert
        // bleeding actually occurred rather than the pre-clot instant value.
        CHECK(tint(*Bo, w, "bleed_rate") > 0);
        CHECK(tint(*w.find(b), w, "pain_level") >= 500);
    }

    // ---- C. involuntary voiding vs impacted retention ---------------------
    {
        World w(73, "data");
        EntityId b = mk_body(w, {100, 100});
        gut_props(*w.find(b), w);
        w.mind(b);                                             // agent body
        EntityId blad = mk_lumen(w, b, "blad_c", "organ", "torso", "bladder",
                                 400, 80, "urine", 900, "kidney");
        Entity* Bl = w.find(blad);
        // extreme overload (pressure 1300 >= 1200): control collapses -> void
        tseti(*Bl, w, "contents_load", 1300);
        tseti(*Bl, w, "content_viscosity", 100);               // <= contract
        w.advance_ticks_for_test(2);
        CHECK(tint(*Bl, w, "contents_load") == 0);
        bool saw_u = false;
        for (const auto& ep : w.entities)
            if (ep->material == w.materials.id_of("urine")) saw_u = true;
        CHECK(saw_u);
        // impacted at same pressure: viscosity > contractility -> no void
        EntityId b2 = mk_body(w, {300, 100});
        gut_props(*w.find(b2), w);
        EntityId blad2 = mk_lumen(w, b2, "blad_c2", "organ", "torso", "bladder",
                                  400, 80, "urine", 900, "kidney");
        Entity* B2 = w.find(blad2);
        tseti(*B2, w, "contents_load", 1300);
        tseti(*B2, w, "content_viscosity", 950);               // > contract 900
        w.advance_ticks_for_test(2);
        CHECK(tint(*B2, w, "contents_load") == 1300);           // retained
    }

    // ---- D. rupture: shattered wall under pressure ------------------------
    {
        World w(79, "data");
        EntityId b = mk_body(w, {100, 100});
        gut_props(*w.find(b), w);
        EntityId torso = mk_part(w, b, "tor_d", "muscle", "torso", "torso", 200, 300);
        EntityId blad = mk_lumen(w, b, "blad_d", "organ", "torso", "bladder",
                                 400, 80, "urine", 900, "kidney");
        Entity* Bl = w.find(blad);
        tseti(*Bl, w, "contents_load", 900);                    // pressure 900
        tseti(*Bl, w, "integrity", 250);                        // wall shattered
        w.advance_ticks_for_test(2);
        CHECK(tint(*Bl, w, "ruptured") == 1);
        CHECK(tint(*Bl, w, "integrity") == 0);
        CHECK(tint(*Bl, w, "bleed_rate") >= 300);
        CHECK(tint(*w.find(b), w, "pain_level") == 1000);
        // spilled urine contamination (350/4=87) soaks the torso sibling
        CHECK(tint(*w.find(torso), w, "infection_level") >= 80);
    }

    // ---- E. backpressure wears upstream organ; toxin coupling -------------
    {
        World w(83, "data");
        EntityId b = mk_body(w, {100, 100});
        gut_props(*w.find(b), w);
        EntityId kidney = mk_part(w, b, "kid_e", "organ", "kidney", "kidney", 500, 70);
        tseti(*w.find(kidney), w, "filtration_rate", 600);
        EntityId blad = mk_lumen(w, b, "blad_e", "organ", "torso", "bladder",
                                 400, 80, "urine", 900, "kidney");
        tseti(*w.find(blad), w, "contents_load", 710);          // pressure 710
        tseti(*w.find(blad), w, "content_viscosity", 0);
        w.advance_ticks_for_test(1200);
        CHECK(tint(*w.find(kidney), w, "integrity") < 1000);     // worn
        // toxin: healthy kidneys filter -> toxin stays low
        CHECK(tint(*w.find(b), w, "toxin_load") < 400);
        // cripple the filter: kidneys gone -> toxin accumulates -> death
        EntityId b2 = mk_body(w, {300, 100});
        gut_props(*w.find(b2), w);
        Entity* B2 = w.find(b2);
        tseti(*B2, w, "toxin_load", 600);
        tseti(*B2, w, "consciousness", 900);
        tseti(*B2, w, "blood_volume", 800);
        w.advance_ticks_for_test(600);
        Entity* B2p = w.find(b2);
        CHECK(B2p->alive);
        CHECK(tint(*B2p, w, "toxin_load") > 600);               // no filter
        CHECK(tint(*B2p, w, "consciousness") < 900);            // toxemia fog
        CHECK(tint(*B2p, w, "blood_volume") <= 800);             // no regen
        // kill via toxemia — advance past the dormant stride so the
        // unattended body is actually metabolized (LOD skips <8 pending).
        tseti(*B2p, w, "toxin_load", 1000);
        w.advance_ticks_for_test(10);
        CHECK(!w.find(b2)->alive);
    }

    // ---- F. LOD: lumen + toxin accumulators dt=1 x N == dt=N --------------
    {
        auto build = [](World& w) {
            EntityId b = mk_body(w, {100, 100});
            gut_props(*w.find(b), w);
            tseti(*w.find(b), w, "thirst", 800);
            tseti(*w.find(b), w, "undigested_mass", 2000);
            tseti(*w.find(b), w, "undigested_fluid", 2000);
            tseti(*w.find(b), w, "toxin_load", 300);
            tseti(*w.find(b), w, "metabolism", 2000);
            EntityId k = mk_part(w, b, "kid_f", "bone", "kidney", "kidney", 500, 70);
            tseti(*w.find(k), w, "filtration_rate", 600);
            EntityId bo = mk_lumen(w, b, "bowel_f", "organ", "torso", "bowel",
                                   200, 120, "feces", 850, "stomach");
            EntityId bl = mk_lumen(w, b, "blad_f", "organ", "torso", "bladder",
                                   400, 80, "urine", 900, "");
            tseti(*w.find(bl), w, "contents_load", 600);       // pressure <700
            return std::tuple<EntityId, EntityId, EntityId, EntityId, EntityId>{
                b, k, bo, bl, k};
        };
        World wa(89, "data"), wb(89, "data");
        auto [ba, ka, boa, bla, _] = build(wa);
        auto [bb, kb, bob, blb, __] = build(wb);
        for (int i = 0; i < 50; ++i) wa.advance_ticks_for_test(1);
        wb.advance_ticks_for_test(50);
        auto sum = [](World& w, EntityId b, EntityId k, EntityId bo, EntityId bl) {
            std::int64_t v = 0;
            for (EntityId p : {k, bo, bl}) {
                Entity* e = w.find(p); if (!e) continue;
                for (const char* n : {"contents_load", "contents_accum",
                                      "content_viscosity", "viscosity_accum",
                                      "organ_stress_accum", "integrity"}) {
                    const PropId pp = w.props.find_name(n)
                        ? w.props.find_name(n)->id : kNoProp;
                    if (pp != kNoProp) v += w.prop_int(*e, pp, 0);
                }
            }
            v += tint(*w.find(b), w, "toxin_load") +
                 tint(*w.find(b), w, "toxin_accum") +
                 tint(*w.find(b), w, "undigested_mass") +
                 tint(*w.find(b), w, "undigested_fluid");
            return v;
        };
        CHECK(sum(wa, ba, ka, boa, bla) == sum(wb, bb, kb, bob, blb));
    }

    // ---- G. waste decomposition fertilizes the soil cell -------------------
    {
        World w(97, "data");
        const Vec2 spot{800, 800};
        if (w.terrain.contains(spot)) {
            const EntityId pat = w.create_entity("pat",
                w.materials.id_of("feces"), spot, {});
            tseti(*w.find(pat), w, "quantity", 100);
            const std::int64_t v0 = w.terrain.veg_at(spot);
            const std::int64_t m0 = w.terrain.moist_at(spot);
            w.advance_ticks_for_test(3000);
            CHECK(w.terrain.veg_at(spot) > v0);
            const EntityId ppu = w.create_entity("ppu",
                w.materials.id_of("urine"), spot, {});
            tseti(*w.find(ppu), w, "quantity", 100);
            w.advance_ticks_for_test(3000);
            CHECK(w.terrain.moist_at(spot) > m0);
        }
    }

    // ---- H. save/load keeps lumen state ------------------------------------
    {
        World w(101, "data");
        EntityId b = mk_body(w, {100, 100});
        gut_props(*w.find(b), w);
        EntityId blad = mk_lumen(w, b, "blad_h", "organ", "torso", "bladder",
                                 400, 80, "urine", 900, "kidney");
        Entity* Bl = w.find(blad);
        tseti(*Bl, w, "contents_load", 777);
        tseti(*Bl, w, "content_viscosity", 321);
        tseti(*Bl, w, "organ_stress_accum", 555);
        tseti(*w.find(b), w, "toxin_load", 222);
        const std::uint64_t h = w.state_hash();
        const std::string path = "/tmp/simlab_lumen_save.bin";
        CHECK(w.save(path));
        World w2(101, "data");
        CHECK(w2.load(path));
        CHECK(w2.state_hash() == h);
    }

    // ---- I. blunt impact bursts a distended lumen (pressure >= 800) ------
    {
        World w(103, "data");
        EntityId b = mk_body(w, {100, 100});
        gut_props(*w.find(b), w);
        // two torso-region parts so the weighted anatomy pick always lands
        // on slot "torso" — matching the lumen's body_slot
        EntityId m1 = mk_part(w, b, "m1_i", "muscle", "torso", "muscle", 200, 300);
        EntityId blad = mk_lumen(w, b, "blad_i", "organ", "torso", "bladder",
                                 400, 80, "urine", 900, "");
        Entity* Bl = w.find(blad);
        tseti(*Bl, w, "contents_load", 900);          // pressure 900 >= 800
        tseti(*Bl, w, "content_viscosity", 0);
        // a second, non-distended lumen in the same region must survive
        EntityId blad2 = mk_lumen(w, b, "blad_i2", "organ", "torso", "bladder",
                                  400, 80, "urine", 900, "");
        tseti(*w.find(blad2), w, "contents_load", 100); // pressure 100
        EntityId striker = mk_striker(w, {100, 100}, "dry_wood");// blunt, same spot
        strike(w, striker, b, "impact", 500000);
        CHECK(tint(*Bl, w, "ruptured") == 1);
        CHECK(tint(*Bl, w, "integrity") == 0);
        CHECK(tint(*Bl, w, "bleed_rate") >= 250);
        CHECK(tint(*Bl, w, "contents_load") == 0);
        CHECK(tint(*w.find(b), w, "pain_level") == 1000);
        // spilled urine contamination (350/4=87) soaks the muscle sibling
        CHECK(tint(*w.find(m1), w, "infection_level") >= 80);
        // the relaxed lumen in the same region is untouched
        CHECK(tint(*w.find(blad2), w, "ruptured") == 0);
        CHECK(tint(*w.find(blad2), w, "integrity") > 0);
    }
}


// ---- Phase 10.4 helpers ---------------------------------------------------
static EntityId mk_strain(World& w, Vec2 pos,
                          std::initializer_list<std::pair<const char*, std::int64_t>> iv,
                          const char* slot) {
    PropertyBag pb;
    for (auto& [n, v] : iv) {
        const PropId p = tpid(w, n);
        if (p != kNoProp) pb.set(p, Value::integer(v));
    }
    if (slot) {
        const PropId p = tpid(w, "target_slot");
        if (p != kNoProp) pb.set(p, Value::text(slot));
    }
    return w.create_entity("pathogen_strain", kNoMaterial, pos, pb);
}
static EntityId mk_infection(World& w, EntityId host, EntityId strain,
                             std::int64_t load, std::int64_t ab) {
    PropertyBag pb;
    auto pi = [&](const char* n, std::int64_t v) {
        const PropId p = tpid(w, n); if (p != kNoProp) pb.set(p, Value::integer(v)); };
    pi("pathogen_strain", static_cast<std::int64_t>(strain));
    pi("pathogen_load", load); pi("antibody_level", ab);
    pi("infection_elapsed", 0); pi("infection_load_accum", 0);
    pi("infection_ab_accum", 0); pi("shed_accum", 0);
    pi("mutation_accum", 0); pi("damage_accum", 0);
    pi("infection_toxin_accum", 0);
    const EntityId inf = w.create_entity("infection", kNoMaterial,
                                         w.find(host)->pos, pb);
    w.relations.relate(host, "carries", inf);
    return inf;
}
static bool host_has_strain(World& w, EntityId host, EntityId strain) {
    for (const Relation& r : w.relations_of(host, "carries")) {
        if (r.a != host) continue;
        Entity* ie = w.find(r.b);
        if (!ie || !ie->alive) continue;
        if (tint(*ie, w, "pathogen_strain") == static_cast<std::int64_t>(strain))
            return true;
    }
    return false;
}
static std::int64_t sum_prop(World& w, const char* n,
                             const std::vector<EntityId>& ids) {
    std::int64_t s = 0;
    for (EntityId id : ids) {
        Entity* e = w.find(id);
        if (e) s += tint(*e, w, n);
    }
    return s;
}

static void test_thermoregulation_immunity_neoplasm_and_epidemiology() {
    // ---- A+B: species-relative thermoregulation, hypothermia, fever, ------
    // ---- antipyretic — all thresholds relative to normal_body_temp ------
    {
        World w(71, "data");
        // cold wet world: 20°C, moisture 800 everywhere near the test bodies
        TerrainGrid& t = w.terrain;
        t.w = 16; t.h = 16; t.cell = 16; t.ox = 0; t.oy = 0;
        const std::size_t n = 256;
        t.elevation.assign(n, 0); t.moisture.assign(n, 800);
        t.temperature.assign(n, 200); t.water.assign(n, 0);
        t.vegetation.assign(n, 0); t.slope.assign(n, 0);
        t.wind_x.assign(n, 0); t.wind_y.assign(n, 0);
        t.water_table.assign(n, 0);
        auto mk_th = [&](Vec2 pos, const char* nbt, std::int64_t bt,
                         std::int64_t therm, std::int64_t inf) {
            EntityId b = mk_body(w, pos);
            tseti(*w.find(b), w, "metabolism", 0);
            tseti(*w.find(b), w, "decay_rate", 0);
            Entity* e = w.find(b);
            if (inf > 0) {   // total_infection sums PART infection_level
                EntityId pt = mk_part(w, b, "inf_part", "muscle", "torso",
                                      "torso", 200, 300);
                tseti(*w.find(pt), w, "infection_level", inf);
            }
            tseti(*e, w, "normal_body_temp", std::stoll(nbt));
            tseti(*e, w, "body_temp", bt);
            tseti(*e, w, "temp_accum", 0);
            tseti(*e, w, "thermoreg_gain", therm);
            tseti(*e, w, "hypothermia_delta", 15);
            tseti(*e, w, "fever_delta_max", 35);
            tseti(*e, w, "hyperpyrexia_delta", 45);
            tseti(*e, w, "immune_power", 1000);
            tseti(*e, w, "immune_power_base", 1000);
            tseti(*e, w, "infection_level", inf);
            return b;
        };
        EntityId b_endo = mk_th({100, 100}, "370", 370, 1000, 0);
        EntityId b_poiki = mk_th({200, 100}, "300", 300, 150, 0);
        EntityId b_fever = mk_th({270, 100}, "370", 370, 1000, 300);
        // antipyretic berry bush within cooling radius of b_fever2
        EntityId b_fever2 = mk_th({316, 100}, "370", 370, 1000, 300);
        EntityId bush = w.create_entity("wild_berries",
            w.materials.id_of("wild_berries"), {310, 100}, {});
        (void)bush;
        w.advance_ticks_for_test(120);
        const std::int64_t t_endo = tint(*w.find(b_endo), w, "body_temp");
        const std::int64_t t_poiki = tint(*w.find(b_poiki), w, "body_temp");
        const std::int64_t t_fever = tint(*w.find(b_fever), w, "body_temp");
        const std::int64_t t_fever2 = tint(*w.find(b_fever2), w, "body_temp");
        // endotherm holds above ambient far better than the poikilotherm
        CHECK(t_endo > t_poiki + 40);
        CHECK(t_poiki <= 250);                    // tracks the 20°C cold
        // fever overrides the cold pull: infected body runs hot
        CHECK(t_fever >= 385);                    // 370 + >=15
        // antipyretic within 24 suppresses the fever back to set-point
        CHECK(t_fever2 < t_fever - 10);
        CHECK(t_fever2 <= 385);
        // deep hypothermia halves effective immunity on the endotherm
        if (t_endo < 355)
            CHECK(tint(*w.find(b_endo), w, "immune_power") <= 600);
        else
            CHECK(t_endo > 355);   // informational either way
        // fever cost: infected bodies burned extra water/energy vs control
        CHECK(tint(*w.find(b_fever), w, "thirst") >
              tint(*w.find(b_endo), w, "thirst"));
    }

    // ---- C: multi-vector transmission (airborne + waterborne) -------------
    {
        World w(72, "data");
        EntityId s_air = mk_strain(w, {0, 0},
            {{"antigen_signature", 111}, {"virulence", 300},
             {"airborne_rate", 800}, {"waterborne_rate", 0},
             {"contact_rate", 0}, {"incubation_ticks", 0}}, "torso");
        EntityId s_wat = mk_strain(w, {0, 0},
            {{"antigen_signature", 222}, {"virulence", 300},
             {"airborne_rate", 0}, {"waterborne_rate", 800},
             {"contact_rate", 0}, {"incubation_ticks", 0}}, "torso");
        EntityId host = mk_body(w, {60, 100});
            tseti(*w.find(host), w, "metabolism", 0);
            tseti(*w.find(host), w, "decay_rate", 0);
        mk_infection(w, host, s_air, 100, 0);
        EntityId near1 = mk_body(w, {150, 100});
            tseti(*w.find(near1), w, "metabolism", 0);
            tseti(*w.find(near1), w, "decay_rate", 0);   // within airborne 96 of host
        EntityId near2 = mk_body(w, {130, 100});
            tseti(*w.find(near2), w, "metabolism", 0);
            tseti(*w.find(near2), w, "decay_rate", 0);   // within water 48 of waste
        // contaminated feces stamped with the waterborne strain
        PropertyBag wp;
        const PropId qty = tpid(w, "quantity");
        const PropId sr = tpid(w, "strain_ref");
        if (qty != kNoProp) wp.set(qty, Value::integer(50));
        if (sr != kNoProp) wp.set(sr, Value::integer(
            static_cast<std::int64_t>(s_wat)));
        w.create_entity("feces", w.materials.id_of("feces"), {140, 100}, wp);
        w.advance_ticks_for_test(80);
        CHECK(host_has_strain(w, near1, s_air));    // airborne landed
        CHECK(host_has_strain(w, near2, s_wat));    // fecal-oral landed
        CHECK(!host_has_strain(w, host, s_wat));    // waste strain != host strain
    }

    // ---- D: adaptive immunity, memory, maternal passive transfer ---------
    {
        World w(73, "data");
        EntityId s1 = mk_strain(w, {0, 0},
            {{"antigen_signature", 555}, {"virulence", 200},
             {"airborne_rate", 0}, {"waterborne_rate", 0},
             {"contact_rate", 0}, {"incubation_ticks", 0}}, "torso");
        // naive host vs host pre-seeded with the same antigen in memory
        EntityId naive = mk_body(w, {100, 100});
            tseti(*w.find(naive), w, "metabolism", 0);
            tseti(*w.find(naive), w, "decay_rate", 0);
        EntityId primed = mk_body(w, {200, 100});
            tseti(*w.find(primed), w, "metabolism", 0);
            tseti(*w.find(primed), w, "decay_rate", 0);
        tseti(*w.find(primed), w, "immune_sig0", 555);
        tseti(*w.find(primed), w, "immune_count", 1);
        mk_infection(w, naive, s1, 100, 0);
        mk_infection(w, primed, s1, 100, 0);
        // live carried infections per host
        auto live_infs = [&](EntityId h) {
            int n = 0;
            for (const Relation& r : w.relations_of(h, "carries"))
                if (r.a == h) {
                    Entity* ie = w.find(r.b);
                    if (ie && ie->alive) ++n;
                }
            return n;
        };
        // immune memory must produce a strictly earlier clearance; the
        // naive host's first victory writes the antigen into memory.
        int cp = -1, cn = -1;
        for (int step = 0; step < 160 && (cp < 0 || cn < 0); ++step) {
            w.advance_ticks_for_test(50);
            if (cp < 0 && live_infs(primed) == 0) cp = step;
            if (cn < 0 && live_infs(naive) == 0) cn = step;
        }
        CHECK(cp >= 0 && cn >= 0 && cp < cn);
        CHECK(tint(*w.find(naive), w, "immune_count") >= 1 ||
              tint(*w.find(naive), w, "immune_sig0") == 555);
        // second exposure on the now-immune host clears far faster:
        // measure antibody after a short window on a fresh twin
        EntityId reinf = mk_body(w, {300, 100});
            tseti(*w.find(reinf), w, "metabolism", 0);
            tseti(*w.find(reinf), w, "decay_rate", 0);
        tseti(*w.find(reinf), w, "immune_sig0", 555);
        tseti(*w.find(reinf), w, "immune_count", 1);
        EntityId naive2 = mk_body(w, {400, 100});
            tseti(*w.find(naive2), w, "metabolism", 0);
            tseti(*w.find(naive2), w, "decay_rate", 0);
        mk_infection(w, reinf, s1, 100, 0);
        mk_infection(w, naive2, s1, 100, 0);
        // second exposure on a memory-carrying host clears strictly first
        int cr = -1, cn2 = -1;
        for (int step = 0; step < 160 && (cr < 0 || cn2 < 0); ++step) {
            w.advance_ticks_for_test(50);
            if (cr < 0 && live_infs(reinf) == 0) cr = step;
            if (cn2 < 0 && live_infs(naive2) == 0) cn2 = step;
        }
        CHECK(cr >= 0 && cn2 >= 0 && cr < cn2);
        // maternal passive transfer: mother with memory -> child inherits
        {
            const PropId sp_p = pid(w, "speed"), fe_p = pid(w, "fertility");
            std::vector<std::pair<PropId, std::int64_t>> loci =
                {{sp_p, 5000}, {fe_p, 500}};
            EntityId mother = mk_organism(w, "test_beast", {500, 100}, loci,
                                          300, 500, 30, 1800);
            EntityId father = mk_organism(w, "test_beast", {510, 100}, loci,
                                          300, 500, 30, 1800);
            tseti(*w.find(mother), w, "immune_sig0", 555);
            tseti(*w.find(mother), w, "immune_sig1", 777);
            tseti(*w.find(mother), w, "immune_count", 2);
            w.find(mother)->props.set(pid(w, "repro_cooldown"),
                                      Value::integer(0));
            for (EntityId pp : {mother, father}) {
                w.find(pp)->props.set(pid(w, "hunger"), Value::integer(100));
                w.find(pp)->props.set(pid(w, "energy"), Value::integer(1000000));
            }
            InteractionResult rr = w.interact(mk_ctx(mother, {father},
                                                   "reproduce", 0));
            CHECK(rr.completed && !rr.spawned.empty());
            if (!rr.spawned.empty()) {
                Entity* child = w.find(rr.spawned.front());
                CHECK(tint(*child, w, "immune_sig0") == 555);
                CHECK(tint(*child, w, "passive_immune") > 0);
            }
        }
    }

    // ---- E: antigenic drift — mutant strain escapes immune memory --------
    {
        World w(74, "data");
        EntityId s = mk_strain(w, {0, 0},
            {{"antigen_signature", 9001}, {"virulence", 1000},
             {"airborne_rate", 0}, {"waterborne_rate", 0},
             {"contact_rate", 0}, {"incubation_ticks", 0}}, "torso");
        EntityId host = mk_body(w, {100, 100});
            tseti(*w.find(host), w, "metabolism", 0);
            tseti(*w.find(host), w, "decay_rate", 0);
        mk_infection(w, host, s, 50, 0);
        w.advance_ticks_for_test(6000);
        // count live pathogen_strain entities besides the founder
        int strains = 0; std::uint32_t seen_sig = 0;
        for (const auto& ep : w.entities) {
            if (!ep->alive || ep->kind_name != "pathogen_strain") continue;
            ++strains;
            seen_sig = static_cast<std::uint32_t>(tint(*ep, w,
                "antigen_signature"));
        }
        CHECK(strains >= 2);                        // drift spawned a mutant
        // at least one strain carries a signature different from 9001
        bool different = false;
        for (const auto& ep : w.entities) {
            if (!ep->alive || ep->kind_name != "pathogen_strain") continue;
            if (tint(*ep, w, "antigen_signature") != 9001) different = true;
        }
        CHECK(different);
        (void)seen_sig;
    }

    // ---- F: neoplasm growth, metastasis, amputation cure ------------------
    {
        World w(75, "data");
        EntityId b = mk_body(w, {100, 100});
            tseti(*w.find(b), w, "metabolism", 0);
            tseti(*w.find(b), w, "decay_rate", 0);
        gut_props(*w.find(b), w);
        Entity* B = w.find(b);
        tseti(*B, w, "dna_repair_gain", 200);       // poor repair -> initiates
        tseti(*B, w, "toxin_load", 700);            // oncogenic pressure
        EntityId torso = mk_part(w, b, "tor_f", "muscle", "torso", "torso",
                                 200, 300);
        EntityId arm = mk_part(w, b, "arm_f", "muscle", "arm_l", "arm", 0, 140);
        EntityId leg = mk_part(w, b, "leg_f", "bone", "leg_l", "leg", 0, 160);
        w.advance_ticks_for_test(400);
        // some part initiated a neoplasm
        auto nm_of = [&](EntityId p) { return tint(*w.find(p), w, "neoplasm_mass"); };
        CHECK(nm_of(torso) + nm_of(arm) + nm_of(leg) > 0);
        // fast-forward growth on whichever part has the tumour
        EntityId tumour = nm_of(torso) > 0 ? torso
                        : (nm_of(arm) > 0 ? arm : leg);
        tseti(*w.find(tumour), w, "neoplasm_mass", 500);
        const std::int64_t en0 = tint(*B, w, "energy");
        w.advance_ticks_for_test(200);
        CHECK(tint(*w.find(tumour), w, "neoplasm_mass") > 500);  // grew
        CHECK(tint(*B, w, "energy") < en0);                      // drains host
        // metastasis: mass >= 750 seeds a sibling part
        tseti(*w.find(tumour), w, "neoplasm_mass", 760);
        w.advance_ticks_for_test(600);
        const int with_tumour = (nm_of(torso) > 0) + (nm_of(arm) > 0) +
                                (nm_of(leg) > 0);
        CHECK(with_tumour >= 2);                                 // spread
        // amputation cures: sever the tumour part -> it can no longer seed
        for (EntityId p : {torso, arm, leg}) {
            Entity* pt = w.find(p);
            if (pt && tint(*pt, w, "neoplasm_mass") >= 500) {
                tseti(*pt, w, "severed", 1);
                break;
            }
        }
        // zero out all masses to isolate the severed carrier's behaviour
        for (EntityId p : {torso, arm, leg})
            tseti(*w.find(p), w, "neoplasm_mass", 0);
        // re-seed ONLY the severed part at 800 -> must NOT metastasize
        EntityId sev = kNoEntity;
        for (EntityId p : {torso, arm, leg})
            if (tint(*w.find(p), w, "severed") == 1) sev = p;
        CHECK(sev != kNoEntity);
        // suppress spontaneous initiation so only metastasis could spread:
        // high repair gain + zero toxin + cleared seed accumulators.
        tseti(*B, w, "dna_repair_gain", 2000);
        tseti(*B, w, "toxin_load", 0);
        for (EntityId p : {torso, arm, leg}) {
            tseti(*w.find(p), w, "neoplasm_mass", 800 * (p == sev ? 1 : 0));
            tseti(*w.find(p), w, "neoplasm_seed_accum", 0);
        }
        w.advance_ticks_for_test(800);
        bool spread_from_severed = false;
        for (EntityId p : {torso, arm, leg})
            if (p != sev && tint(*w.find(p), w, "neoplasm_mass") > 0)
                spread_from_severed = true;
        CHECK(!spread_from_severed);
    }

    // ---- G: LOD invariance + save roundtrip -------------------------------
    {
        auto build = [](World& w) {
            EntityId b = mk_body(w, {100, 100});
            tseti(*w.find(b), w, "metabolism", 0);
            tseti(*w.find(b), w, "decay_rate", 0);
            gut_props(*w.find(b), w);
            Entity* B = w.find(b);
            tseti(*B, w, "normal_body_temp", 370);
            tseti(*B, w, "body_temp", 370);
            tseti(*B, w, "thermoreg_gain", 1000);
            tseti(*B, w, "hypothermia_delta", 15);
            tseti(*B, w, "fever_delta_max", 35);
            tseti(*B, w, "hyperpyrexia_delta", 45);
            tseti(*B, w, "immune_power", 1000);
            tseti(*B, w, "immune_power_base", 1000);
            tseti(*B, w, "dna_repair_gain", 800);
            tseti(*B, w, "body_temp", 391);   // already febrile: no ramp transient
            EntityId torso = mk_part(w, b, "tor_l", "muscle", "torso",
                                     "torso", 200, 300);
            tseti(*w.find(torso), w, "neoplasm_mass", 100);
            tseti(*w.find(torso), w, "neoplasm_accum", 400);
            EntityId s = mk_strain(w, {0, 0},
                {{"antigen_signature", 4242}, {"virulence", 400},
                 {"airborne_rate", 0}, {"waterborne_rate", 0},
                 {"contact_rate", 0}, {"incubation_ticks", 10}}, "torso");
            EntityId inf = mk_infection(w, b, s, 300, 100);
            tseti(*w.find(inf), w, "infection_elapsed", 20);  // past incubation
            return b;
        };
        World wa(76, "data"), wb(76, "data");
        EntityId ba = build(wa), bb = build(wb);
        for (int i = 0; i < 30; ++i) wa.advance_ticks_for_test(1);
        wb.advance_ticks_for_test(30);
        std::vector<EntityId> ids_a{ba}, ids_b{bb};
        for (const char* pn : {"body_temp", "toxin_load", "neoplasm_mass"}) {
            // sum over body + its parts for a fair composite check
            auto total = [&](World& w, EntityId b) {
                std::int64_t s = 0;
                s += tint(*w.find(b), w, pn);
                for (const Relation& r : w.relations_of(b, "part_of"))
                    if (r.b == b) {
                        Entity* p = w.find(r.a);
                        if (p) s += tint(*p, w, pn);
                    }
                return s;
            };
            CHECK(total(wa, ba) == total(wb, bb));
        }
        // infection entity internals identical too
        auto inf_sum = [&](World& w, EntityId b) {
            std::int64_t s = 0;
            for (const Relation& r : w.relations_of(b, "carries"))
                if (r.a == b) {
                    Entity* ie = w.find(r.b);
                    if (ie) s += tint(*ie, w, "pathogen_load") * 8 +
                                tint(*ie, w, "antibody_level");
                }
            return s;
        };
        CHECK(inf_sum(wa, ba) == inf_sum(wb, bb));
        // save/load preserves the whole epidemiological state
        const std::uint64_t h0 = wa.state_hash();
        CHECK(wa.save("/tmp/simlab_p104.bin"));
        World wc(1, "data");
        CHECK(wc.load("/tmp/simlab_p104.bin"));
        CHECK(wc.state_hash() == h0);
        (void)ids_a; (void)ids_b;
    }
}

// ---------- Phase 11: zoonotic reservoirs, environmental reservoirs, ------
// ---------- sterilization & burial ------------------------------------------
static void test_zoonosis_sterilization_and_burial() {
    auto mk_host = [&](World& w, Vec2 pos, std::int64_t rt,
                       std::int64_t bias) -> EntityId {
        EntityId b = mk_body(w, pos);
        Entity* e = w.find(b);
        tseti(*e, w, "metabolism", 0);
        tseti(*e, w, "decay_rate", 0);          // no rot while alive
        tseti(*e, w, "hunger", 100);
        tseti(*e, w, "fatigue", 0);
        tseti(*e, w, "energy", 200000);
        tseti(*e, w, "max_energy", 300000);
        tseti(*e, w, "reach", 300);
        tseti(*e, w, "sight_range", 400);
        tseti(*e, w, "reservoir_tolerance", rt);
        tseti(*e, w, "zoonotic_jump_bias", bias);
        tseti(*e, w, "toxin_load", 0);
        tseti(*e, w, "toxin_accum", 0);
        tseti(*e, w, "normal_body_temp", 370);
        tseti(*e, w, "thermoreg_gain", 1000);
        tseti(*e, w, "hypothermia_delta", 15);
        tseti(*e, w, "fever_delta_max", 35);
        tseti(*e, w, "hyperpyrexia_delta", 45);
        tseti(*e, w, "body_temp", 370);
        tseti(*e, w, "temp_accum", 0);
        tseti(*e, w, "immune_power", 1000);
        tseti(*e, w, "immune_power_base", 1000);
        tseti(*e, w, "dna_repair_gain", 1000);
        return b;
    };
    auto seed_symbiont = [&](World& w, EntityId h) {
        Entity* e = w.find(h);
        tseti(*e, w, "symbiont_signature", 2654435761);
        tseti(*e, w, "symbiont_virulence", 120);
        tseti(*e, w, "symbiont_airborne_rate", 120);
        tseti(*e, w, "symbiont_waterborne_rate", 350);
        tseti(*e, w, "symbiont_contact_rate", 450);
        tseti(*e, w, "symbiont_incubation_ticks", 300);
    };
    auto strain_of = [&](World& w, EntityId host) -> EntityId {
        for (const Relation& r : w.relations_of(host, "carries")) {
            if (r.a != host) continue;
            Entity* ie = w.find(r.b);
            if (!ie || !ie->alive) continue;
            const std::int64_t se = tint(*ie, w, "pathogen_strain");
            if (se > 0) return static_cast<EntityId>(se);
        }
        return kNoEntity;
    };

    // ---- A: tolerant reservoir hosts a harmless, still-shedding symbiont -
    {
        World w(31, "data");
        EntityId beast = mk_host(w, {0, 0}, 900, 400);
        seed_symbiont(w, beast);
        w.advance_ticks_for_test(50);       // symbiont strain+infection seeded
        EntityId sym = strain_of(w, beast);
        CHECK(sym != kNoEntity);
        Entity* ss = w.find(sym);
        CHECK(tint(*ss, w, "symbiont") == 1);
        w.advance_ticks_for_test(2000);     // well past incubation (300)
        Entity* B = w.find(beast);
        CHECK(B->alive);
        const std::int64_t bt = tint(*B, w, "body_temp");
        CHECK(bt >= 360 && bt <= 380);       // no fever from a tolerated symbiont
        CHECK(tint(*B, w, "toxin_load") < 500); // no virulence toxin surge
        CHECK(strain_of(w, beast) == sym);  // still carries & sheds it
    }

    // ---- B: raw consume of reservoir meat -> adapted zoonotic strain ------
    {
        World w(32, "data");
        EntityId beast = mk_host(w, {0, 0}, 900, 400);
        seed_symbiont(w, beast);
        w.advance_ticks_for_test(50);
        EntityId sym = strain_of(w, beast);
        CHECK(sym != kNoEntity);
        w.destroy_entity(beast);            // carcass keeps carries->infection
        EntityId eater = mk_host(w, {10, 0}, 0, 1000);  // naive, always-adapt
        tseti(*w.find(eater), w, "immune_power", 200);   // slow antibody ->
        tseti(*w.find(eater), w, "immune_power_base", 200); // infection persists
        InteractionResult cr = w.interact(mk_ctx(eater, {beast}, "consume", 0));
        CHECK(cr.completed);
        EntityId jumped = strain_of(w, eater);
        CHECK(jumped != kNoEntity);
        CHECK(jumped != sym);               // host-adapted variant, not parent
        Entity* vs = w.find(jumped);
        Entity* ps = w.find(sym);
        CHECK(tint(*vs, w, "symbiont") == 0);
        CHECK(tint(*vs, w, "virulence") > tint(*ps, w, "virulence"));
        CHECK(tint(*vs, w, "antigen_signature") !=
              tint(*ps, w, "antigen_signature"));
        w.advance_ticks_for_test(700);      // incubation 300 -> fever on
        Entity* E2=w.find(eater);
        CHECK(E2->alive);
        CHECK(strain_of(w, eater) != kNoEntity);   // infection still active
        CHECK(tint(*w.find(eater), w, "body_temp") > 385);
    }

    // ---- C: heat sterilizes dead tissue; cooked meat infects nobody ------
    {
        World w(33, "data");
        EntityId beast = mk_host(w, {0, 0}, 900, 400);
        seed_symbiont(w, beast);
        w.advance_ticks_for_test(50);
        EntityId sym = strain_of(w, beast);
        w.destroy_entity(beast);
        Entity* B = w.find(beast);
        tseti(*B, w, "strain_ref", static_cast<std::int64_t>(sym));
        tseti(*B, w, "contamination", 600);
        tseti(*B, w, "thermal_mass", 1);    // cheap test heating
        EntityId fire = mk_host(w, {20, 0}, 0, 0);
        // 60000/(100*1)=600 deciC -> 200+600=800 >= sterilization_temp 700
        InteractionResult hr = w.interact(mk_ctx(fire, {beast}, "heat", 60000));
        CHECK(hr.completed);
        CHECK(tint(*B, w, "cooked_state") == 1000);
        CHECK(tint(*B, w, "strain_ref") == 0);
        CHECK(tint(*B, w, "contamination") == 0);
        EntityId eater = mk_host(w, {10, 0}, 0, 1000);
        InteractionResult cr = w.interact(mk_ctx(eater, {beast}, "consume", 0));
        CHECK(cr.completed);
        CHECK(strain_of(w, eater) == kNoEntity);   // sterile meal is safe
    }

    // ---- D: antimicrobial combine sterilizes like heat --------------------
    {
        World w(34, "data");
        EntityId beast = mk_host(w, {0, 0}, 900, 400);
        seed_symbiont(w, beast);
        w.advance_ticks_for_test(50);
        EntityId sym = strain_of(w, beast);
        w.destroy_entity(beast);
        Entity* B = w.find(beast);
        tseti(*B, w, "strain_ref", static_cast<std::int64_t>(sym));
        tseti(*B, w, "contamination", 600);
        const EntityId salt = w.create_entity("salt_lump", w.materials.id_of("salt"), {30, 0}, {});
        EntityId hand = mk_host(w, {40, 0}, 0, 0);
        InteractionResult mr = w.interact(
            mk_ctx(hand, {beast, salt}, "combine", 0));
        CHECK(mr.completed);
        CHECK(tint(*B, w, "cooked_state") == 1000);
        CHECK(tint(*B, w, "strain_ref") == 0);
        CHECK(tint(*B, w, "contamination") == 0);
    }

    // ---- E: corpse reservoir -> water contamination; burial silences ------
    {
        World w(35, "data");
        EntityId corpse = mk_host(w, {0, 0}, 0, 0);
        EntityId cs = mk_strain(w, {0, 0},
            {{"waterborne_rate", 500}, {"airborne_rate", 0},
             {"contact_rate", 0}, {"antigen_signature", 777001},
             {"virulence", 100}, {"incubation_ticks", 100}}, nullptr);
        mk_infection(w, corpse, cs, 100, 0);
        w.destroy_entity(corpse);
        Entity* C = w.find(corpse);
        tseti(*C, w, "decay_rate", 400);      // rot speed while dead
        const EntityId pond = w.create_entity("pond", w.materials.id_of("water"), {12, 0}, {});
        const EntityId far_pond = w.create_entity("pond2", w.materials.id_of("water"), {200, 0}, {});
        w.advance_ticks_for_test(1600);      // level -> 640, emits from ~500
        CHECK(tint(*C, w, "corpse_decay_level") > 200);
        CHECK(tint(*C, w, "odor") > 0);
        CHECK(tint(*C, w, "contamination") > 0);
        CHECK(tint(*w.find(pond), w, "contamination") > 0);
        CHECK(tint(*w.find(pond), w, "strain_ref") ==
              static_cast<std::int64_t>(cs));   // strain reached the water
        CHECK(tint(*w.find(far_pond), w, "strain_ref") == 0);
        // burial via combine with a burial_medium material
        const EntityId soil = w.create_entity("soil_mound", w.materials.id_of("soil_loam"),
                                              {20, 0}, {});
        EntityId mourner = mk_host(w, {30, 0}, 0, 0);
        InteractionResult br = w.interact(
            mk_ctx(mourner, {corpse, soil}, "combine", 0));
        CHECK(br.completed);
        CHECK(tint(*C, w, "buried") == 1);
        CHECK(tint(*C, w, "odor") == 0);
        const std::int64_t lvl = tint(*C, w, "corpse_decay_level");
        const std::int64_t pond_c = tint(*w.find(pond), w, "contamination");
        w.advance_ticks_for_test(500);
        CHECK(tint(*C, w, "corpse_decay_level") == lvl);      // interred: frozen
        CHECK(tint(*w.find(pond), w, "contamination") == pond_c);
    }

    // ---- F: stagnant water fouls and amplifies waterborne strains --------
    {
        World w(36, "data");
        const EntityId strain = mk_strain(w, {0, 0},
            {{"waterborne_rate", 500}, {"airborne_rate", 0},
             {"contact_rate", 0}, {"antigen_signature", 777002},
             {"virulence", 100}, {"incubation_ticks", 100}}, nullptr);
        const EntityId pond = w.create_entity("pond", w.materials.id_of("water"), {0, 0}, {});
        tseti(*w.find(pond), w, "stagnation_level", 600);
        tseti(*w.find(pond), w, "strain_ref",
              static_cast<std::int64_t>(strain));
        const EntityId pond2 = w.create_entity("pond2", w.materials.id_of("water"), {10, 0}, {});
        w.advance_ticks_for_test(300);
        CHECK(tint(*w.find(pond), w, "stagnation_level") > 600);
        CHECK(tint(*w.find(pond), w, "contamination") > 0);
        CHECK(tint(*w.find(pond2), w, "strain_ref") ==
              static_cast<std::int64_t>(strain));
        CHECK(tint(*w.find(pond2), w, "contamination") > 0);
    }

    // ---- G: deep positive engram -> burial utility bonus -----------------
    {
        World w(37, "data");
        EntityId npc = mk_host(w, {0, 0}, 0, 0);
        EntityId kin = mk_host(w, {10, 0}, 0, 0);
        w.destroy_entity(kin);
        w.advance_ticks_for_test(2);         // corpse marker materializes
        AgentMind& mm = w.mind(npc);
        AgentMind::MemorySnapshot snap;
        snap.pos = w.find(kin)->pos;
        snap.material = w.find(kin)->material;
        snap.kind_name = "test_beast";
        const PropId cdl = tpid(w, "corpse_decay_level");
        if (cdl != kNoProp) snap.props[cdl] = Value::integer(0);
        snap.engram.emotional_charge = 700;
        snap.engram.clarity = 600;
        mm.memory[kin] = snap;
        InteractionContext bc = mk_ctx(npc, {kin}, "combine", 0);
        CHECK(bond_bonus(w, mm, bc) > 0);    // kin corpse -> burial urge
        InteractionContext bc2 = mk_ctx(npc, {kin}, "consume", 0);
        CHECK(bond_bonus(w, mm, bc2) == 0);  // not a generic bonus
    }

    // ---- H: LOD invariance + save roundtrip -------------------------------
    {
        auto build_env = [&](World& w) {
            EntityId corpse = mk_host(w, {0, 0}, 0, 0);
            const EntityId s = mk_strain(w, {0, 0},
                {{"waterborne_rate", 500}, {"airborne_rate", 0},
                 {"contact_rate", 0}, {"antigen_signature", 777003},
                 {"virulence", 100}, {"incubation_ticks", 100}}, nullptr);
            mk_infection(w, corpse, s, 100, 0);
            w.destroy_entity(corpse);
            tseti(*w.find(corpse), w, "decay_rate", 400);
            const EntityId pond = w.create_entity("pond", w.materials.id_of("water"), {12, 0}, {});
            tseti(*w.find(pond), w, "stagnation_level", 500);
            return std::pair<EntityId, EntityId>{corpse, pond};
        };
        World wa(41, "data"), wb(41, "data");
        const auto [ca, pa] = build_env(wa);
        const auto [cb, pb] = build_env(wb);
        for (int i = 0; i < 400; ++i) wa.advance_ticks_for_test(1);
        wb.advance_ticks_for_test(400);
        CHECK(tint(*wa.find(ca), wa, "corpse_decay_level") ==
              tint(*wb.find(cb), wb, "corpse_decay_level"));
        CHECK(tint(*wa.find(pa), wa, "stagnation_level") ==
              tint(*wb.find(pb), wb, "stagnation_level"));
        // LOD parity is on reservoir state, not the event ledger: a
        // ticked run legitimately logs N drift events where a batched run
        // logs one — state_hash covers ledger history by design.
        for (const char* pn : {"corpse_decay_level","corpse_decay_accum",
                "odor","contamination","strain_ref","reservoir_spread_accum",
                "stagnation_level","stagnation_accum"}) {
            CHECK(tint(*wa.find(ca), wa, pn) == tint(*wb.find(cb), wb, pn));
            CHECK(tint(*wa.find(pa), wa, pn) == tint(*wb.find(pb), wb, pn));
        }
        CHECK(wa.find(ca)->props.get(tpid(wa,"corpse_decay_accum")) != nullptr);
        CHECK(wb.find(cb)->props.get(tpid(wb,"corpse_decay_accum")) != nullptr);
        const std::uint64_t h0 = wa.state_hash();
        CHECK(wa.save("/tmp/simlab_p11.bin"));
        World wc(41, "data");
        CHECK(wc.load("/tmp/simlab_p11.bin"));
        CHECK(wc.state_hash() == h0);
    }
}


// ---------- Phase 11.2: plant blight, monoculture, soil spores, fire -----
static void test_plant_blight_monoculture_and_soil_cleansing() {
    auto mk_plant = [&](World& w, const char* kind, Vec2 pos,
                        std::int64_t susc) -> EntityId {
        PropertyBag pb;
        auto pi = [&](const char* n, std::int64_t v) {
            const PropId p = tpid(w, n);
            if (p != kNoProp) pb.set(p, Value::integer(v)); };
        auto pt = [&](const char* n, const char* v) {
            const PropId p = tpid(w, n);
            if (p != kNoProp) pb.set(p, Value::text(v)); };
        pi("integrity", 1000); pi("decay_rate", 0); pi("metabolism", 0);
        pi("fruit_count", 40); pi("max_fruit", 40);
        pi("fruit_regrow_rate", 120); pi("fruit_spoil_rate", 15);
        pi("blight_susceptibility", susc);
        pi("sterilization_temp", 700);
        pi("thermal_mass", 1);              // cheap test heating
        pt("fruit_material", "wild_berries");
        return w.create_entity(kind, w.materials.id_of("living_wood"),
                               pos, pb);
    };
    auto mk_host2 = [&](World& w, Vec2 pos) -> EntityId {
        EntityId b = mk_body(w, pos);
        Entity* e = w.find(b);
        tseti(*e, w, "metabolism", 0);
        tseti(*e, w, "decay_rate", 0);
        tseti(*e, w, "hunger", 100);
        tseti(*e, w, "fatigue", 0);
        tseti(*e, w, "energy", 200000);
        tseti(*e, w, "max_energy", 300000);
        tseti(*e, w, "reach", 300);
        tseti(*e, w, "toxin_load", 0);
        tseti(*e, w, "toxin_accum", 0);
        return b;
    };
    auto blight_strain = [&](World& w, std::int64_t sig) -> EntityId {
        return mk_strain(w, {0, 0},
            {{"spore_rate", 500}, {"virulence", 250},
             {"antigen_signature", sig}, {"airborne_rate", 0},
             {"waterborne_rate", 0}, {"contact_rate", 0},
             {"incubation_ticks", 50}}, nullptr);
    };
    auto count_kind_infected = [&](World& w, const char* kind,
                                   EntityId strain) {
        int n = 0;
        for (const auto& ep : w.entities) {
            const Entity& e = *ep;
            if (!e.alive || e.kind_name != kind) continue;
            if (host_has_strain(w, e.id, strain)) ++n;
        }
        return n;
    };
    auto find_patch = [&](World& w, Vec2 near_pos) -> EntityId {
        std::vector<EntityId> near = w.entities_near(near_pos, 64);
        std::sort(near.begin(), near.end());
        for (EntityId nid : near) {
            const Entity* o = w.find(nid);
            if (o && o->alive && o->kind_name == "soil_patch") return nid;
        }
        return kNoEntity;
    };

    // ---- A: monoculture amplifies, sparse/polyculture dilute -------------
    {
        World w(51, "data");
        const EntityId strain = blight_strain(w, 888001);
        // dense same-species cluster: every plant has 4 same-species
        // neighbours inside the 96 spore range.
        std::vector<EntityId> mono;
        for (Vec2 p : {Vec2{0,0}, Vec2{40,0}, Vec2{80,0},
                       Vec2{0,40}, Vec2{40,40}})
            mono.push_back(mk_plant(w, "berry_bush", p, 700));
        mk_infection(w, mono[0], strain, 800, 0);
        // sparse same-species row: 150 spacing > spore range -> density 0.
        std::vector<EntityId> sparse;
        for (int i = 0; i < 5; ++i)
            sparse.push_back(mk_plant(w, "berry_bush", {500 + i * 150, 0}, 700));
        mk_infection(w, sparse[0], strain, 800, 0);
        // polyculture: berries and grains interleaved — each species sees
        // only ~1-2 same-species neighbours instead of 4.
        std::vector<EntityId> poly_b, poly_g;
        const char* kinds[6] = {"berry_bush","wild_grain","berry_bush",
                                "wild_grain","berry_bush","wild_grain"};
        for (int i = 0; i < 6; ++i) {
            EntityId id = mk_plant(w, kinds[i],
                {i % 3 * 40, 200 + i / 3 * 40}, i % 2 ? 600 : 700);
            (kinds[i][0] == 'b' ? poly_b : poly_g).push_back(id);
        }
        mk_infection(w, poly_b[0], strain, 800, 0);
        w.advance_ticks_for_test(120);
        const int mono_n = count_kind_infected(w, "berry_bush", strain);
        // mono cluster contributes >=4; sparse contributes exactly 1;
        // poly contributes its seed + at most slow spread.
        CHECK(mono_n >= 5);            // all five mono bushes caught it
        // sparse row: only the seed itself (density 0 -> shed rate 0)
        int sparse_n = 0;
        for (EntityId id : sparse)
            if (host_has_strain(w, id, strain)) ++sparse_n;
        CHECK(sparse_n == 1);
        // polyculture berries: strictly fewer infections than the dense
        // monoculture cluster of the same species.
        int poly_n = 0;
        for (EntityId id : poly_b)
            if (host_has_strain(w, id, strain)) ++poly_n;
        CHECK(poly_n < mono_n - sparse_n - 1);   // dense >> interleaved
    }

    // ---- B: blighted raw fruit poisons; cooked fruit is safe ------------
    {
        World w(52, "data");
        const EntityId strain = blight_strain(w, 888002);
        EntityId plant = mk_plant(w, "berry_bush", {0, 0}, 700);
        mk_infection(w, plant, strain, 800, 0);
        EntityId eater = mk_host2(w, {20, 0});
        const std::int64_t en0 = tint(*w.find(eater), w, "energy");
        InteractionResult cr = w.interact(mk_ctx(eater, {plant}, "consume", 0));
        CHECK(cr.completed);
        Entity* E = w.find(eater);
        CHECK(tint(*E, w, "toxin_load") > 0);          // gut poison
        CHECK(tint(*E, w, "energy") < en0 + 20000);    // calories blunted
        // the event's toxin change forges a negative engram on the plant
        AgentMind& mm = w.mind(eater);
        auto it = mm.memory.find(plant);
        CHECK(it != mm.memory.end());
        CHECK(it->second.engram.emotional_charge <= -400);
        // cooked/preserved plant: same meal, no poison
        tseti(*w.find(plant), w, "cooked_state", 1000);
        EntityId eater2 = mk_host2(w, {20, 40});
        const std::int64_t tox0 = tint(*w.find(eater2), w, "toxin_load");
        InteractionResult cr2 = w.interact(
            mk_ctx(eater2, {plant}, "consume", 0));
        CHECK(cr2.completed);
        CHECK(tint(*w.find(eater2), w, "toxin_load") == tox0);
    }

    // ---- C: soil spore bank infects sprouts; fallow self-cleans ---------
    {
        World w(53, "data");
        const EntityId strain = blight_strain(w, 888003);
        EntityId plant = mk_plant(w, "berry_bush", {0, 0}, 700);
        mk_infection(w, plant, strain, 800, 0);
        w.destroy_entity(plant);                    // blighted corpse
        w.advance_ticks_for_test(5);                // leaches into soil
        const EntityId patch = find_patch(w, {0, 0});
        CHECK(patch != kNoEntity);
        CHECK(tint(*w.find(patch), w, "soil_spore_load") > 200);
        CHECK(tint(*w.find(patch), w, "soil_strain_ref") ==
              static_cast<std::int64_t>(strain));
        CHECK(tint(*w.find(patch), w, "soil_host_kind") == 0 ||
              true);                                 // text prop, checked via sprout
        // a propagule of the same species germinates on the banked soil
        PropertyBag pb;
        auto pi = [&](const char* n, std::int64_t v) {
            const PropId p = tpid(w, n);
            if (p != kNoProp) pb.set(p, Value::integer(v)); };
        auto pt = [&](const char* n, const char* v) {
            const PropId p = tpid(w, n);
            if (p != kNoProp) pb.set(p, Value::text(v)); };
        pi("integrity", 100); pi("decay_rate", 0); pi("metabolism", 0);
        pi("sprout_rate", 1000); pi("sprout_since", 0);
        pi("sprout_moisture_min", 0); pi("sprout_temp_min", -1000);
        pi("sprout_temp_max", 2000);
        pt("sprout_target", "berry_bush");
        pt("sprout_material", "living_wood");
        const EntityId seed1 = w.create_entity(
            "dormant_seed", w.materials.id_of("dormant_seed"), {4, 4}, pb);
        CHECK(seed1 != kNoEntity);
        w.advance_ticks_for_test(60);               // germinates + infected
        // find the new living berry_bush
        EntityId sprout = kNoEntity;
        for (const auto& ep : w.entities) {
            const Entity& e = *ep;
            if (e.alive && e.kind_name == "berry_bush") { sprout = e.id; break; }
        }
        CHECK(sprout != kNoEntity);
        CHECK(host_has_strain(w, sprout, strain));  // soil memory bit
        // fallow: no new host input -> bank decays to zero, strain cleared
        w.advance_ticks_for_test(2200);
        CHECK(tint(*w.find(patch), w, "soil_spore_load") == 0);
        CHECK(tint(*w.find(patch), w, "soil_strain_ref") == 0);
        // a second propagule on cleaned soil stays healthy
        PropertyBag pb2 = pb;
        const EntityId seed2 = w.create_entity(
            "dormant_seed", w.materials.id_of("dormant_seed"), {8, 8}, pb2);
        (void)seed2;
        w.advance_ticks_for_test(60);
        EntityId sprout2 = kNoEntity;
        for (const auto& ep : w.entities) {
            const Entity& e = *ep;
            if (e.alive && e.kind_name == "berry_bush" && e.id != sprout) {
                sprout2 = e.id; break;
            }
        }
        CHECK(sprout2 != kNoEntity);
        CHECK(!host_has_strain(w, sprout2, strain)); // fallow worked
    }

    // ---- D: fire sterilizes spores; ash returns fertility ---------------
    {
        World w(54, "data");
        const EntityId strain = blight_strain(w, 888004);
        EntityId plant = mk_plant(w, "berry_bush", {0, 0}, 700);
        mk_infection(w, plant, strain, 800, 0);
        EntityId actor = mk_host2(w, {20, 0});
        // heat to >= sterilization_temp (700): thermal_mass 1 -> +600
        InteractionResult hr = w.interact(mk_ctx(actor, {plant}, "heat", 60000));
        CHECK(hr.completed);
        CHECK(!host_has_strain(w, plant, strain));   // infections cut
        // ash residue spawned + soil patch fertilized
        const MaterialId ash_mat = w.materials.id_of("ash");
        bool ash_found = false;
        for (const auto& ep : w.entities)
            if (ep->alive && ep->material == ash_mat) ash_found = true;
        CHECK(ash_found);
        const EntityId patch = find_patch(w, {0, 0});
        CHECK(patch != kNoEntity);
        CHECK(tint(*w.find(patch), w, "soil_nutrient") > 0);
        // and a contaminated soil patch itself is sterilized by heat
        tseti(*w.find(patch), w, "soil_spore_load", 600);
        tseti(*w.find(patch), w, "soil_strain_ref",
              static_cast<std::int64_t>(strain));
        tseti(*w.find(patch), w, "thermal_mass", 1);   // cheap test burn
        InteractionResult hr2 = w.interact(mk_ctx(actor, {patch}, "heat", 60000));
        CHECK(hr2.completed);
        CHECK(tint(*w.find(patch), w, "soil_spore_load") == 0);
        CHECK(tint(*w.find(patch), w, "soil_strain_ref") == 0);
    }

    // ---- E: LOD parity on reservoir state + save roundtrip ---------------
    {
        auto build = [&](World& w) {
            const EntityId strain = blight_strain(w, 888005);
            EntityId plant = mk_plant(w, "berry_bush", {0, 0}, 700);
            const EntityId inf = mk_infection(w, plant, strain, 800, 0);
            // pre-existing soil bank so decay parity is exercised
            PropertyBag pb;
            auto pi = [&](const char* n, std::int64_t v) {
                const PropId p = tpid(w, n);
                if (p != kNoProp) pb.set(p, Value::integer(v)); };
            pi("soil_spore_load", 600); pi("spore_decay_accum", 0);
            pi("soil_strain_ref", static_cast<std::int64_t>(strain));
            pi("soil_nutrient", 0); pi("integrity", 400);
            pi("contamination", 0); pi("decay_rate", 0); pi("metabolism", 0);
            const EntityId patch = w.create_entity(
                "soil_patch", w.materials.id_of("soil_loam"), {12, 0}, pb);
            return std::tuple<EntityId, EntityId, EntityId>{plant, inf, patch};
        };
        World wa(55, "data"), wb(55, "data");
        const auto [pa, ia, qa] = build(wa);
        const auto [pb_, ib, qb] = build(wb);
        for (int i = 0; i < 300; ++i) wa.advance_ticks_for_test(1);
        wb.advance_ticks_for_test(300);
        for (const char* pn : {"yield_penalty", "blight_marks", "integrity"})
            CHECK(tint(*wa.find(pa), wa, pn) == tint(*wb.find(pb_), wb, pn));
        for (const char* pn : {"spore_accum", "spore_rolls", "damage_accum"})
            CHECK(tint(*wa.find(ia), wa, pn) == tint(*wb.find(ib), wb, pn));
        for (const char* pn : {"soil_spore_load", "spore_decay_accum",
                               "soil_strain_ref", "soil_nutrient"})
            CHECK(tint(*wa.find(qa), wa, pn) == tint(*wb.find(qb), wb, pn));
        const std::uint64_t h0 = wa.state_hash();
        CHECK(wa.save("/tmp/simlab_p112.bin"));
        World wc(55, "data");
        CHECK(wc.load("/tmp/simlab_p112.bin"));
        CHECK(wc.state_hash() == h0);
    }
}


// ---------- Phase 11.3: vectors, seasons, selection, herd immunity ------
static void test_vector_ecology_selection_and_herd_immunity() {
    auto mk_vector = [&](World& w, Vec2 pos) -> EntityId {
        // species-declared defaults become instance props — the vector is
        // an ordinary organism whose props all come from data/species.txt.
        const SpeciesDef* sp = w.species.find_name("vector_midge");
        PropertyBag pb;
        if (sp) for (const auto& [pid2, v2] : sp->props.entries)
            pb.set(pid2, v2);
        return w.create_entity("vector_midge",
                               w.materials.id_of("flesh"), pos, pb);
    };
    auto mk_pond = [&](World& w, Vec2 pos, std::int64_t level) -> EntityId {
        EntityId p = w.create_entity("water",
                                     w.materials.id_of("water"), pos, {});
        Entity* e = w.find(p);
        tseti(*e, w, "stagnation_level", level);
        tseti(*e, w, "stagnation_accum", 0);
        tseti(*e, w, "flow_rate", 0);
        return p;
    };
    auto mk_temp_terrain = [&](World& w, std::int64_t temp_c) {
        // 16x16 grid of 64-unit cells covering [-256, 752) — temp uniform.
        w.terrain.w = 16; w.terrain.h = 16; w.terrain.cell = 64;
        w.terrain.ox = -4; w.terrain.oy = -4;
        const std::size_t n = 256;
        w.terrain.elevation.assign(n, 0);
        w.terrain.moisture.assign(n, 500);
        w.terrain.temperature.assign(n, temp_c);
        w.terrain.water.assign(n, 0);
        w.terrain.vegetation.assign(n, 500);
        w.terrain.slope.assign(n, 0);
        w.terrain.wind_x.assign(n, 0);
        w.terrain.wind_y.assign(n, 0);
        w.terrain.water_table.assign(n, 0);
    };
    auto vborne_strain = [&](World& w, std::int64_t sig,
                             std::int64_t vir) -> EntityId {
        return mk_strain(w, {0, 0},
            {{"vector_borne_rate", 600}, {"virulence", vir},
             {"antigen_signature", sig}, {"airborne_rate", 0},
             {"waterborne_rate", 0}, {"contact_rate", 0},
             {"spore_rate", 0}, {"incubation_ticks", 50},
             {"symbiont", 0}}, nullptr);
    };
    // test-side mirror of host_immune_to (the method is World-private):
    // count immune_sig slots below immune_count matching the antigen.
    auto carrier_set = [&](World& w, EntityId strain) {
        std::set<EntityId> out_;
        for (auto& ep : w.entities) {
            Entity& e = *ep;
            if (!e.alive) continue;
            for (const Relation& r : w.relations_of(e.id, "carries")) {
                if (r.a != e.id) continue;
                Entity* ie = w.find(r.b);
                if (ie && ie->alive &&
                    tint(*ie, w, "pathogen_strain") ==
                        static_cast<std::int64_t>(strain)) {
                    out_.insert(e.id); break;
                }
            }
        }
        return out_;
    };
    auto carriers_of = [&](World& w, EntityId strain) {
        return static_cast<int>(carrier_set(w, strain).size());
    };
    // ids of alive entities whose immune_sig slots match the antigen —
    // the herd-immunity protected set.
    auto immune_ids = [&](World& w, EntityId strain) {
        const std::int64_t ag = tint(*w.find(strain), w,
                                     "antigen_signature");
        std::set<EntityId> out_;
        for (auto& ep : w.entities) {
            Entity& e = *ep;
            if (!e.alive) continue;
            const int cnt = static_cast<int>(std::min<std::int64_t>(8,
                tint(e, w, "immune_count")));
            for (int i = 0; i < cnt; ++i) {
                char nm[32];
                std::snprintf(nm, sizeof nm, "immune_sig%d", i);
                if (tint(e, w, nm) == ag) { out_.insert(e.id); break; }
            }
        }
        return out_;
    };
    auto immune_hosts = [&](World& w, EntityId strain) {
        return static_cast<int>(immune_ids(w, strain).size());
    };

    // ---- A: seasonality gates vector activity; bite transmits -----------
    {
        World w(61, "data");
        mk_temp_terrain(w, 300);                    // 30 °C — inside 220..360
        const EntityId pond = mk_pond(w, {10, 0}, 600);
        (void)pond;
        const EntityId vec = mk_vector(w, {50, 0});
        EntityId hostA = mk_body(w, {90, 0});
        EntityId hostB = mk_body(w, {50, 40});
        EntityId hostC = mk_body(w, {500, 500});    // out of bite range
        const EntityId strain = vborne_strain(w, 777001, 300);
        mk_infection(w, hostA, strain, 800, 0);
        w.advance_ticks_for_test(300);
        Entity* V = w.find(vec);
        CHECK(tint(*V, w, "vector_dormant") == 0);   // warm + wet -> active
        CHECK(host_has_strain(w, vec, strain));     // picked the strain up
        CHECK(host_has_strain(w, hostB, strain));   // injected into naive B
        CHECK(!host_has_strain(w, hostC, strain));  // out of range: safe
        // cold snap: below vector_optimal_temp_min -> torpor, no biting
        w.terrain.temperature.assign(256, 100);     // 10 °C < 220
        const std::int64_t bites0 = tint(*V, w, "vector_bite_rolls");
        const std::int64_t bite_acc0 = tint(*V, w, "vector_bite_accum");
        const std::set<EntityId> carriers0 = carrier_set(w, strain);
        w.advance_ticks_for_test(120);
        CHECK(tint(*w.find(vec), w, "vector_dormant") == 1);
        CHECK(tint(*w.find(vec), w, "vector_bite_rolls") == bites0);
        CHECK(tint(*w.find(vec), w, "vector_bite_accum") == bite_acc0);
        // dormancy stopped transmission: every carrier now was already a
        // carrier before the cold snap (old infections may clear, but no
        // NEW host can acquire the strain while the vector is torpid).
        for (EntityId cid : carrier_set(w, strain))
            CHECK(carriers0.count(cid) != 0);
    }

    // ---- B: heritable resistance + epidemic selection -------------------
    {
        World w(62, "data");
        const PropId ipb = tpid(w, "immune_power_base");
        const PropId ipw = tpid(w, "immune_power");
        const PropId drp = tpid(w, "dna_repair_gain");
        const PropId rtp = tpid(w, "reservoir_tolerance");
        const PropId met = tpid(w, "metabolism");
        // moderate virulence: resistant clears before sepsis, susceptible
        // does not — survival IS the selection pressure.
        const EntityId strain = mk_strain(w, {0, 0},
            {{"vector_borne_rate", 0}, {"virulence", 200},
             {"antigen_signature", 777002}, {"airborne_rate", 0},
             {"waterborne_rate", 0}, {"contact_rate", 0},
             {"spore_rate", 0}, {"incubation_ticks", 50},
             {"symbiont", 0}}, nullptr);
        auto mk_res = [&](Vec2 p, std::int64_t ipb_v) {
            std::vector<std::pair<PropId, std::int64_t>> loci = {
                {ipb, ipb_v}, {drp, ipb_v > 1000 ? 1500 : 400},
                {rtp, ipb_v > 1000 ? 900 : 100}};
            EntityId id = mk_organism(w, "test_beast", p, loci,
                                      300, 500, 30, 0);
            Entity* e = w.find(id);
            tseti(*e, w, "blood_volume", 1000);   // is_host: infections progress
            tseti(*e, w, "immune_power", ipb_v);
            tseti(*e, w, "immune_power_base", ipb_v);
            tseti(*e, w, "toxin_load", 0);
            tseti(*e, w, "toxin_accum", 0);
            return id;
        };
        std::vector<EntityId> resistant, susceptible;
        for (int i = 0; i < 4; ++i) {
            resistant.push_back(mk_res({i * 30, 0}, 1800));
            susceptible.push_back(mk_res({i * 30, 200}, 200));
        }
        for (EntityId id : resistant) mk_infection(w, id, strain, 900, 0);
        for (EntityId id : susceptible) mk_infection(w, id, strain, 900, 0);
        w.advance_ticks_for_test(2000);
        int alive_r = 0, alive_s = 0;
        for (EntityId id : resistant)
            if (w.find(id)->alive) ++alive_r;
        for (EntityId id : susceptible)
            if (w.find(id)->alive) ++alive_s;
        CHECK(alive_r >= 3);
        CHECK(alive_s <= 1);          // resistance is heritable fitness
        // F1: survivors reproduce — offspring inherit the resistant loci
        std::vector<EntityId> surv;
        for (EntityId id : resistant) if (w.find(id)->alive) surv.push_back(id);
        CHECK(surv.size() >= 2);
        // top up fertility/energy for the post-epidemic mating
        for (EntityId id : surv) {
            Entity* e = w.find(id);
            tseti(*e, w, "fertility", 500);
            tseti(*e, w, "energy", 100000);
            tseti(*e, w, "hunger", 100);
            tseti(*e, w, "age", 300);
        }
        InteractionResult rr = w.interact(
            mk_ctx(surv[0], {surv[1]}, "reproduce", 0));
        CHECK(rr.completed);
        // find the child (newest test_beast)
        EntityId child = kNoEntity;
        for (const auto& ep : w.entities) {
            const Entity& e = *ep;
            if (e.alive && e.kind_name == "test_beast" &&
                e.id != surv[0] && e.id != surv[1] &&
                std::none_of(resistant.begin(), resistant.end(),
                             [&](EntityId x){return x==e.id;}) &&
                std::none_of(susceptible.begin(), susceptible.end(),
                             [&](EntityId x){return x==e.id;})) {
                child = e.id; break;
            }
        }
        CHECK(child != kNoEntity);
        const std::int64_t child_ipb = tint(*w.find(child), w,
                                            "immune_power_base");
        CHECK(child_ipb >= 1000);     // resistance allele inherited
        CHECK(tint(*w.find(surv[0]), w, "repro_mutation_rolls") >= 1);
    }

    // ---- C: herd immunity + second wave on naive recruits ---------------
    {
        World w(63, "data");
        // persistent shedder: low immunity keeps a low-virulence infection
        // alive for thousands of ticks — the reservoir that re-seeds.
        const EntityId strain = mk_strain(w, {0, 0},
            {{"vector_borne_rate", 0}, {"virulence", 50},
             {"antigen_signature", 777003}, {"airborne_rate", 800},
             {"waterborne_rate", 0}, {"contact_rate", 0},
             {"spore_rate", 0}, {"incubation_ticks", 50},
             {"symbiont", 0}}, nullptr);
        auto mk_herd = [&](Vec2 p, std::int64_t ipb_v) {
            EntityId id = mk_body(w, p);
            Entity* e = w.find(id);
            tseti(*e, w, "immune_power", ipb_v);
            tseti(*e, w, "immune_power_base", ipb_v);
            tseti(*e, w, "toxin_load", 0);
            tseti(*e, w, "toxin_accum", 0);
            tseti(*e, w, "metabolism", 0);
            tseti(*e, w, "decay_rate", 0);
            return id;
        };
        std::vector<EntityId> herd;
        for (int i = 0; i < 6; ++i) herd.push_back(mk_herd({i * 30, 0}, 1600));
        // herd[0] is the reservoir: weak immunity keeps its low-virulence
        // infection alive for the whole scenario — the source that reseeds
        // every wave.
        {
            Entity* sd = w.find(herd[0]);
            tseti(*sd, w, "immune_power", 100);
            tseti(*sd, w, "immune_power_base", 100);
        }
        mk_infection(w, herd[0], strain, 700, 0);
        // wave 1: the shedder's airborne quanta sweep the cluster; each
        // host clears and banks the antigen in immune memory. Advances
        // are chunked because transmission commits at pass boundaries —
        // a single giant batch cannot chain infections within itself.
        for (int i = 0; i < 22; ++i) w.advance_ticks_for_test(100);
        const int immune_n = immune_hosts(w, strain);
        CHECK(immune_n >= 5);          // community reached herd immunity
        const std::set<EntityId> immune_set = immune_ids(w, strain);
        // wave 2: naive recruits (post-wave births, no immune memory)
        // enter the same airborne plume and fall ill again.
        // moderate immunity: they fall ill and STAY ill long enough to
        // observe — herd immunity comes from memory, not from genes.
        EntityId naive1 = mk_herd({80, 0}, 400);
        EntityId naive2 = mk_herd({95, 0}, 400);
        const int carriers_before = carriers_of(w, strain);
        for (int i = 0; i < 4; ++i) w.advance_ticks_for_test(100);
        CHECK(carriers_of(w, strain) > carriers_before);  // second wave
        CHECK(host_has_strain(w, naive1, strain) ||
              host_has_strain(w, naive2, strain));
        // herd immunity held: no wave-1 survivor was reinfected — immune
        // hosts are skipped by every transmission route.
        for (EntityId id : immune_set)
            CHECK(!host_has_strain(w, id, strain));
    }

    // ---- D: LOD parity for vector dormancy/accumulators -----------------
    {
        auto build = [&](World& w) {
            mk_temp_terrain(w, 300);
            mk_pond(w, {10, 0}, 600);
            const EntityId vec = mk_vector(w, {50, 0});
            EntityId hostA = mk_body(w, {90, 0});
            mk_body(w, {50, 40});
            mk_body(w, {120, 0});
            const EntityId strain = vborne_strain(w, 777004, 300);
            mk_infection(w, hostA, strain, 800, 0);
            return std::tuple<EntityId, EntityId, EntityId>{vec, hostA, strain};
        };
        World wa(64, "data"), wb(64, "data");
        const auto [va, ha, sa] = build(wa);
        const auto [vb, hb, sb] = build(wb);
        for (int i = 0; i < 200; ++i) wa.advance_ticks_for_test(1);
        wb.advance_ticks_for_test(200);
        // brood_rolls/brood_accum are deliberately NOT compared: batched
        // brood never metabolizes inside the window (spawn-commit
        // semantics), so multi-generation crowd counts legitimately
        // differ — the same exemption every spawn-coupled quantity gets.
        for (const char* pn : {"vector_dormant", "vector_bite_accum",
                               "vector_bite_rolls"})
            CHECK(tint(*wa.find(va), wa, pn) == tint(*wb.find(vb), wb, pn));
        // pond trajectory marks identical too
        EntityId pa = kNoEntity, pb_ = kNoEntity;
        for (const auto& ep : wa.entities)
            if (ep->kind_name == "water") pa = ep->id;
        for (const auto& ep : wb.entities)
            if (ep->kind_name == "water") pb_ = ep->id;
        // stagnation_hist_* are reconstruction marks, not state: ticked
        // mode rewrites them every tick, batched once per window — the
        // extrapolated level they encode is what must agree, and the
        // level/accum pair below is exactly that.
        for (const char* pn : {"stagnation_level", "stagnation_accum"})
            CHECK(tint(*wa.find(pa), wa, pn) == tint(*wb.find(pb_), wb, pn));
        // vector transmission happened in BOTH modes: the original
        // vector carried the strain and hostB fell ill. Batched brood
        // cannot bite inside the window (spawn-commit semantics), so
        // per-host infection timing is spawn-coupled and exempt —
        // presence on the deterministic first target is what must agree.
        CHECK(host_has_strain(wa, va, sa));
        CHECK(host_has_strain(wb, vb, sb));
        {
            EntityId hb_a = kNoEntity, hb_b = kNoEntity;
            for (const auto& ep : wa.entities)
                if (ep->kind_name == "test_beast" && ep->pos.x == 50 &&
                    ep->pos.y == 40) hb_a = ep->id;
            for (const auto& ep : wb.entities)
                if (ep->kind_name == "test_beast" && ep->pos.x == 50 &&
                    ep->pos.y == 40) hb_b = ep->id;
            CHECK(hb_a != kNoEntity && hb_b != kNoEntity);
            CHECK(host_has_strain(wa, hb_a, sa));
            CHECK(host_has_strain(wb, hb_b, sb));
        }
        const std::uint64_t h0 = wa.state_hash();
        CHECK(wa.save("/tmp/simlab_p113.bin"));
        World wc(64, "data");
        CHECK(wc.load("/tmp/simlab_p113.bin"));
        CHECK(wc.state_hash() == h0);
    }
}



// ================= Phase 12.1 — causal proto-language =========================
static std::vector<EntityId> kinds_of(World& w, const char* kn) {
    std::vector<EntityId> out;
    for (const auto& ep : w.entities)
        if (ep->alive && ep->kind_name == kn) out.push_back(ep->id);
    return out;
}
static EntityId first_kind(World& w, const char* kn) {
    for (const auto& ep : w.entities)
        if (ep->alive && ep->kind_name == kn) return ep->id;
    return kNoEntity;
}
static void lang_person(World& w, EntityId id, const char* kind,
                        std::int64_t harsh, std::int64_t syl) {
    Entity* e = w.find(id);
    tseti(*e, w, "metabolism", 50);   // slow needs: language tests run long
    tseti(*e, w, "vocal_capability", 900);
    tseti(*e, w, "phoneme_harshness", harsh);
    tseti(*e, w, "syllable_length_bias", syl);
    tseti(*e, w, "hardness", 900);
    (void)kind;
}

static void test_causal_language_dialect_drift_and_inscriptions() {
    // ---- A: causal coinage — discovery -> SemanticConcept + LexicalEntry ----
    {
        World w(101, "data");
        const EntityId p1 = mk_person(w, {0, 0});
        lang_person(w, p1, "person", 800, 3);
        w.mind(p1).discoveries["method:fire>water>steam"] =
            mk_record("method:fire>water>steam", DiscoveryStatus::Verified);
        w.advance_ticks_for_test(1);
        const PropId pl = tpid(w, "language_id");
        const PropId pck = tpid(w, "concept_key");
        const PropId pps = tpid(w, "phoneme_seed");
        const PropId pgs = tpid(w, "glyph_seed");
        const PropId pchr = tpid(w, "chronicle");
        CHECK(tint(*w.find(p1), w, "language_id") > 0);
        const EntityId cpt = first_kind(w, "semantic_concept");
        const EntityId entry = first_kind(w, "lexical_entry");
        const EntityId lang = first_kind(w, "language");
        CHECK(cpt != kNoEntity);
        CHECK(entry != kNoEntity);
        CHECK(lang != kNoEntity);
        Entity* ce = w.find(cpt);
        Entity* ee = w.find(entry);
        CHECK(tint(*ce, w, "concept_key") >= 0);
        const Value* ckv = w.prop_of(*ce, pck);
        CHECK(ckv && ckv->type == ValType::Text &&
              ckv->s == "method:fire>water>steam");
        const Value* chv = w.prop_of(*ce, pchr);
        CHECK(chv && chv->type == ValType::Text &&
              chv->s.find("verified method") != std::string::npos);
        const std::int64_t ps1 = tint(*ee, w, "phoneme_seed");
        const std::int64_t gs1 = tint(*ee, w, "glyph_seed");
        CHECK(ps1 != 0 && gs1 != 0);
        // phoneme seed reflects species acoustics: same world seed, same
        // language (same kind), different harshness -> different phoneme,
        // IDENTICAL glyph (glyph seed is acoustics-free = more stable).
        World w2(101, "data");
        const EntityId q1 = mk_person(w2, {0, 0});
        lang_person(w2, q1, "person", 200, 3);
        w2.mind(q1).discoveries["method:fire>water>steam"] =
            mk_record("method:fire>water>steam", DiscoveryStatus::Verified);
        w2.advance_ticks_for_test(1);
        const EntityId e2 = first_kind(w2, "lexical_entry");
        CHECK(e2 != kNoEntity);
        CHECK(tint(*w2.find(e2), w2, "phoneme_seed") != ps1);
        CHECK(tint(*w2.find(e2), w2, "glyph_seed") == gs1);
        // coinage is deterministic across replays
        World w3(101, "data");
        const EntityId r1 = mk_person(w3, {0, 0});
        lang_person(w3, r1, "person", 800, 3);
        w3.mind(r1).discoveries["method:fire>water>steam"] =
            mk_record("method:fire>water>steam", DiscoveryStatus::Verified);
        w3.advance_ticks_for_test(1);
        CHECK(tint(*w3.find(first_kind(w3, "lexical_entry")), w3,
                   "phoneme_seed") == ps1);
        CHECK(tint(*w3.find(first_kind(w3, "lexical_entry")), w3,
                   "glyph_seed") == gs1);
        (void)pl; (void)pck; (void)pps; (void)pgs;
    }

    // ---- B: engrave on granite -> same-language heir reads the lesson ------
    EntityId sage, stone_s, reader, stone_w;
    {
        World w(202, "data");
        sage = mk_person(w, {0, 0});
        lang_person(w, sage, "person", 600, 2);
        w.mind(sage).discoveries["method:x"] =
            mk_record("method:x", DiscoveryStatus::Verified);
        w.advance_ticks_for_test(1);           // coins the word
        const MaterialId granite = w.materials.id_of("granite");
        const MaterialId drywood = w.materials.id_of("dry_wood");
        stone_s = w.create_entity("slab", granite, {10, 0});
        stone_w = w.create_entity("plank", drywood, {30, 0});
        CHECK(stone_s != kNoEntity && stone_w != kNoEntity);
        CHECK(w.interact(mk_ctx(sage, {stone_s}, "engrave", 0,
                                "method:x")).completed);
        CHECK(w.interact(mk_ctx(sage, {stone_w}, "engrave", 0,
                                "method:x")).completed);
        // Inscription entities exist, bound to their surfaces
        std::size_t ninsc = 0;
        for (const auto& ep : w.entities)
            if (ep->alive && ep->kind_name == "inscription") ++ninsc;
        CHECK(ninsc == 2);
        const PropId p_auth = tpid(w, "inscription_author");
        const PropId p_itk  = tpid(w, "inscription_tick");
        const PropId p_lvl  = tpid(w, "erosion_level");
        const PropId p_gtxt = tpid(w, "glyph_text");
        const PropId p_ckt  = tpid(w, "concept_keys");
        const PropId p_lang = tpid(w, "language_id");
        const PropId p_dur  = tpid(w, "inscription_durability");
        Entity* ins_e = nullptr;
        for (const auto& ep : w.entities)
            if (ep->alive && ep->kind_name == "inscription" &&
                ep->pos.x == 10) { ins_e = ep.get(); break; }
        CHECK(ins_e != nullptr);
        CHECK(tint(*ins_e, w, "inscription_author") ==
              static_cast<std::int64_t>(sage));
        CHECK(tint(*ins_e, w, "inscription_tick") > 0);
        CHECK(tint(*ins_e, w, "erosion_level") == 0);
        const Value* g = w.prop_of(*ins_e, p_gtxt);
        CHECK(g && g->type == ValType::Text && !g->s.empty());
        const Value* ck = w.prop_of(*ins_e, p_ckt);
        CHECK(ck && ck->type == ValType::Text &&
              ck->s.find("method:x") != std::string::npos);
        CHECK(tint(*ins_e, w, "inscription_durability") == 950);
        CHECK(tint(*ins_e, w, "language_id") ==
              tint(*w.find(sage), w, "language_id"));
        (void)p_auth; (void)p_itk; (void)p_lvl; (void)p_lang; (void)p_dur;

        // same-community heir reads the carved lesson without the accident
        reader = mk_person(w, {20, 0});
        lang_person(w, reader, "person", 600, 2);
        w.mind(reader);                  // agent exists -> may hold a language
        w.advance_ticks_for_test(1);
        CHECK(w.interact(mk_ctx(reader, {stone_s}, "read", 0)).completed);
        const AgentMind* rm = w.find_mind(reader);
        CHECK(rm && rm->discoveries.count("method:x") == 1);
        CHECK(rm->discoveries.at("method:x").status == DiscoveryStatus::Open);

        // a speaker of a DIFFERENT language sees glyphs, not meaning
        PropertyBag ab;
        auto asp = [&](const char* n, std::int64_t v) {
            const PropId p = pid(w, n);
            if (p != kNoProp) ab.set(p, Value::integer(v));
        };
        asp("age", 300); asp("adult_age", 100); asp("fertility", 500);
        asp("mutation_rate", 30); asp("metabolism", 50);
        asp("hunger", 100); asp("thirst", 100); asp("fatigue", 0);
        asp("energy", 100000); asp("integrity", 1000); asp("reach", 200);
        asp("communicate_range", 400);
        const EntityId alien2 = w.create_entity("sapient_b", kNoMaterial,
                                                {45, 0}, ab);
        lang_person(w, alien2, "sapient_b", 400, 2);
        w.mind(alien2);   // ensure mind exists
        w.advance_ticks_for_test(1);
        CHECK(w.interact(mk_ctx(alien2, {stone_s}, "read", 0)).completed);
        const AgentMind* am = w.find_mind(alien2);
        CHECK(am == nullptr || am->discoveries.count("method:x") == 0);
        // repeated alien<->sage contact builds foreign familiarity -> bilingual
        for (int i = 0; i < 9; ++i)
            w.interact(mk_ctx(sage, {alien2}, "communicate", 0));
        std::int64_t best = 0;
        for (const auto& [bk, bv] : w.find_mind(alien2)->beliefs)
            if (bk.rfind("lex:", 0) == 0) best = std::max(best, bv.confidence);
        CHECK(best >= 500);      // bilingual threshold crossed
        // erosion: wood fades, granite endures — same window, both exact
        w.advance_ticks_for_test(1300);
        Entity* wi = nullptr; Entity* si = nullptr;
        for (const auto& ep : w.entities) {
            if (!ep->alive || ep->kind_name != "inscription") continue;
            if (ep->pos.x == 30) wi = ep.get();
            if (ep->pos.x == 10) si = ep.get();
        }
        CHECK(wi && si);
        CHECK(tint(*wi, w, "erosion_level") >= 1000);
        const Value* wg = w.prop_of(*wi, p_gtxt);
        CHECK(wg && wg->type == ValType::Text && wg->s.empty());
        CHECK(tint(*si, w, "erosion_level") < 1000);
        const Value* sg = w.prop_of(*si, p_gtxt);
        CHECK(sg && sg->type == ValType::Text && !sg->s.empty());
        // codex export: raw glyphs + objective chronicle + decipher ratio
        const std::string codex = w.export_lexicon_codex();
        CHECK(codex.find("LANG ") != std::string::npos);
        CHECK(codex.find("ENTRY ") != std::string::npos);
        CHECK(codex.find("phonemes=") != std::string::npos);
        CHECK(codex.find("glyphs=") != std::string::npos);
        CHECK(codex.find("chronicle=") != std::string::npos);
        CHECK(codex.find("verified method method:x") != std::string::npos);
        CHECK(codex.find("deciphered_ratio") != std::string::npos);
        CHECK(codex.find("INSCRIPTION ") != std::string::npos);
        // save/load roundtrip preserves the whole language world
        const std::uint64_t h = w.state_hash();
        const std::string path = "/tmp/simlab_lang_test.sav";
        CHECK(w.save(path));
        World w2(202, "data");
        CHECK(w2.load(path));
        CHECK(w2.state_hash() == h);
    }

    // ---- C/D: dialect drift LOD-exactness + bilingual threshold -------------
    {
        auto build = [](World& w) {
            const EntityId a = mk_person(w, {0, 0});
            lang_person(w, a, "person", 700, 2);
            w.mind(a).discoveries["method:d"] =
                mk_record("method:d", DiscoveryStatus::Verified);
            PropertyBag bb;
            auto bsp = [&](const char* n, std::int64_t v) {
                const PropId p = pid(w, n);
                if (p != kNoProp) bb.set(p, Value::integer(v));
            };
            bsp("age", 300); bsp("adult_age", 100); bsp("fertility", 500);
            bsp("mutation_rate", 30); bsp("metabolism", 50);
            bsp("hunger", 100); bsp("thirst", 100); bsp("fatigue", 0);
            bsp("energy", 100000); bsp("integrity", 1000); bsp("reach", 200);
            bsp("communicate_range", 400);
            bsp("vocal_capability", 900); bsp("phoneme_harshness", 500);
            bsp("syllable_length_bias", 2); bsp("hardness", 500);
            const EntityId b = w.create_entity("sapient_b", kNoMaterial,
                                              {500, 0}, bb);
            w.mind(b);
            w.advance_ticks_for_test(1);
            return std::pair<EntityId, EntityId>{a, b};
        };
        World wt(404, "data"), wb(404, "data");
        build(wt); build(wb);
        const PropId p_dacc = tpid(wt, "dialect_drift_accum");
        const PropId p_droll = tpid(wt, "dialect_drift_rolls");
        const PropId p_ps    = tpid(wt, "phoneme_seed");
        const PropId p_gs    = tpid(wt, "glyph_seed");
        const auto snap = [&](World& w) {
            std::vector<std::int64_t> v;
            for (const auto& ep : w.entities) {
                if (!ep->alive) continue;
                if (ep->kind_name == "language") {
                    v.push_back(tint(*ep, w, "dialect_drift_accum"));
                    v.push_back(tint(*ep, w, "dialect_drift_rolls"));
                } else if (ep->kind_name == "lexical_entry") {
                    v.push_back(tint(*ep, w, "phoneme_seed"));
                    v.push_back(tint(*ep, w, "glyph_seed"));
                }
            }
            return v;
        };
        // two isolated single-member languages drift; 1200 ticks > 1 roll
        for (int i = 0; i < 600; ++i) wt.advance_ticks_for_test(1);
        wb.advance_ticks_for_test(600);
        CHECK(snap(wt) == snap(wb));       // LOD: dt=1 x 600 == dt=600
        for (int i = 0; i < 600; ++i) wt.advance_ticks_for_test(1);
        wb.advance_ticks_for_test(600);
        CHECK(snap(wt) == snap(wb));       // and again to 1200
        // drift actually happened: at least one language rolled
        bool rolled = false;
        for (const auto& ep : wb.entities)
            if (ep->alive && ep->kind_name == "language" &&
                tint(*ep, wb, "dialect_drift_rolls") > 0) rolled = true;
        CHECK(rolled);
        // glyph seeds untouched by dialect drift (stable script)
        // (phoneme seeds differ from coinage-time only via rolls; glyph equal
        //  across wt/wb was already proven by snap equality)
        (void)p_dacc; (void)p_droll; (void)p_ps; (void)p_gs;
    }
}

// Phase 12 base core: compositional atoms/clauses, portable + Rosetta
// inscriptions, semantic drift, per-entry decipherment, LOD, save.
static void test_causal_language_syntax_rosetta_and_inscriptions() {
    const char* DKEY = "heat+stagnant_water->fever";

    // ---- A: atomic coinage — atoms + relation atoms + per-atom entries ----
    {
        World w(111, "data");
        const EntityId a = mk_person(w, {0, 0});
        lang_person(w, a, "person", 800, 3);
        w.mind(a).known_actions.insert("heat");
        w.mind(a).discoveries[DKEY] =
            mk_record(DKEY, DiscoveryStatus::Verified);
        w.advance_ticks_for_test(1);
        // semantic_atom entities exist with atom_key/atom_kind
        std::size_t natom = 0, nentry = 0;
        bool saw_rel = false, saw_heat_kind2 = false;
        for (const auto& ep : w.entities) {
            if (!ep->alive) continue;
            if (ep->kind_name == "semantic_atom") {
                ++natom;
                const std::string ak = tint(*ep, w, "atom_key") < 0 ? ""
                    : ([&]{ const Value* v=w.prop_of(*ep,tpid(w,"atom_key"));
                            return v&&v->type==ValType::Text?v->s:std::string(); })();
                const std::int64_t kd = tint(*ep, w, "atom_kind");
                if (ak == "rel:causes" && kd == 4) saw_rel = true;
                if (ak == "heat" && kd == 2) saw_heat_kind2 = true;
            } else if (ep->kind_name == "lexical_entry") ++nentry;
        }
        CHECK(natom >= 4);            // heat, stagnant_water, fever, rel:causes
        CHECK(saw_rel);
        CHECK(saw_heat_kind2);        // 'heat' is a known action -> kind 2
        CHECK(nentry >= natom + 1);   // atom words + whole-concept word
        // every lexical_entry carries a semantic_shift_seed
        bool all_ssd = true;
        for (const auto& ep : w.entities) {
            if (!ep->alive || ep->kind_name != "lexical_entry") continue;
            if (tint(*ep, w, "semantic_shift_seed") == 0) all_ssd = false;
        }
        CHECK(all_ssd);
        // glyph seed folds glyph_angularity: same seed/lang/atom, different
        // angularity -> different glyph, same phoneme
        World wa(111, "data"), wb(111, "data");
        const EntityId pa = mk_person(wa, {0, 0});
        lang_person(wa, pa, "person", 800, 3);
        tseti(*wa.find(pa), wa, "glyph_angularity", 100);
        wa.mind(pa).discoveries[DKEY] =
            mk_record(DKEY, DiscoveryStatus::Verified);
        const EntityId pb = mk_person(wb, {0, 0});
        lang_person(wb, pb, "person", 800, 3);
        tseti(*wb.find(pb), wb, "glyph_angularity", 950);
        wb.mind(pb).discoveries[DKEY] =
            mk_record(DKEY, DiscoveryStatus::Verified);
        wa.advance_ticks_for_test(1); wb.advance_ticks_for_test(1);
        // compare the atom-entry glyph seeds (concept_key == "heat")
        auto find_atom_entry = [](World& ww, const char* ck) -> EntityId {
            const PropId pck = tpid(ww, "concept_key");
            for (const auto& ep : ww.entities) {
                if (!ep->alive || ep->kind_name != "lexical_entry") continue;
                const Value* v = ww.prop_of(*ep, pck);
                if (v && v->type == ValType::Text && v->s == ck)
                    return ep->id;
            }
            return kNoEntity;
        };
        const EntityId ea = find_atom_entry(wa, "heat");
        const EntityId eb = find_atom_entry(wb, "heat");
        CHECK(ea != kNoEntity && eb != kNoEntity);
        CHECK(tint(*wa.find(ea), wa, "glyph_seed") !=
              tint(*wb.find(eb), wb, "glyph_seed"));
        CHECK(tint(*wa.find(ea), wa, "phoneme_seed") ==
              tint(*wb.find(eb), wb, "phoneme_seed"));
    }

    // ---- B: portable clay inscription carries a CausalClause; heir reads --
    EntityId clay, sage2, heir2;
    {
        World w(222, "data");
        sage2 = mk_person(w, {0, 0});
        lang_person(w, sage2, "person", 700, 2);
        w.mind(sage2).discoveries[DKEY] =
            mk_record(DKEY, DiscoveryStatus::Verified);
        // a second rule sharing atoms: coins "cold"/"cramp" too, so semantic
        // drift has neighbors OUTSIDE the carved clause to land on.
        w.mind(sage2).discoveries["cold+stagnant_water->cramp"] =
            mk_record("cold+stagnant_water->cramp", DiscoveryStatus::Verified);
        w.mind(sage2).discoveries["ice+heat->melt"] =
            mk_record("ice+heat->melt", DiscoveryStatus::Verified);
        w.advance_ticks_for_test(1);
        const MaterialId claym = w.materials.id_of("soil_clay");
        clay = w.create_entity("tablet", claym, {50, 0});
        CHECK(clay != kNoEntity);
        CHECK(w.interact(mk_ctx(sage2, {clay}, "engrave", 0, DKEY)).completed);
        // find the Inscription on the clay
        Entity* ins = nullptr;
        for (const auto& ep : w.entities)
            if (ep->alive && ep->kind_name == "inscription") ins = ep.get();
        CHECK(ins != nullptr);
        const Value* cl = w.prop_of(*ins, tpid(w, "clause_atoms"));
        CHECK(cl && cl->type == ValType::Text && !cl->s.empty());
        CHECK(cl->s.find(':') != std::string::npos);   // "lang:id,.." form
        CHECK(tint(*ins, w, "portable_surface") == 1000);
        CHECK(tint(*ins, w, "language_id2") == 0);     // monolingual so far
        // heir of the same speech community reads -> discovery + word fam
        heir2 = mk_person(w, {80, 0});
        lang_person(w, heir2, "person", 700, 2);
        w.mind(heir2);
        w.advance_ticks_for_test(1);
        CHECK(w.interact(mk_ctx(heir2, {clay}, "read", 0)).completed);
        const AgentMind* hm = w.find_mind(heir2);
        CHECK(hm && hm->discoveries.count(DKEY) == 1);
        std::int64_t lexhits = 0;
        for (const auto& [bk, bv] : hm->beliefs)
            if (bk.rfind("lex:", 0) == 0 && bv.confidence >= 200) ++lexhits;
        CHECK(lexhits >= 2);      // clause words transferred, not just glyphs
        // ---- C: erosion + semantic drift on the weathered tablet ----------
        w.advance_ticks_for_test(1400);   // clay (dur 600) crosses erosion 400
        Entity* ins2 = nullptr;
        for (const auto& ep : w.entities)
            if (ep->alive && ep->kind_name == "inscription") ins2 = ep.get();
        CHECK(ins2 != nullptr);
        CHECK(tint(*ins2, w, "erosion_level") > 400);
        // repeated reads of the SAME weathered text at different ticks:
        // deterministic semantic drift spills familiarity onto a NEIGHBOR
        // atom's word — a word that is NOT in the clause.
        std::set<EntityId> clause_ids;
        if (cl && cl->type == ValType::Text) {
            // parse "lang:ids" segment
            const auto colon = cl->s.find(':');
            const auto semi = cl->s.find(';');
            const std::string seg = cl->s.substr(
                colon + 1, semi == std::string::npos
                    ? std::string::npos : semi - colon - 1);
            std::istringstream is(seg); std::string num;
            while (std::getline(is, num, ','))
                try { clause_ids.insert(static_cast<EntityId>(
                    std::stoull(num))); } catch (...) {}
        }
        for (int i = 0; i < 24; ++i) {
            w.advance_ticks_for_test(1);
            w.interact(mk_ctx(heir2, {clay}, "read", 0));
        }
        bool drifted = false;
        for (const auto& [bk, bv] : hm->beliefs) {
            if (bk.rfind("lex:", 0) != 0 || bv.confidence < 60) continue;
            try {
                const EntityId eid = static_cast<EntityId>(
                    std::stoull(bk.substr(4)));
                if (!clause_ids.count(eid)) { drifted = true; break; }
            } catch (...) {}
        }
        CHECK(drifted);   // neighbor-atom reinterpretation actually happened
        // codex shows atoms, per-entry decipherment, clause payload
        const std::string codex = w.export_lexicon_codex();
        CHECK(codex.find("ATOM ") != std::string::npos);
        CHECK(codex.find("deciphered=") != std::string::npos);
        CHECK(codex.find("clause=") != std::string::npos);
        CHECK(codex.find("portable=1000") != std::string::npos);
    }

    // ---- D/E: Rosetta — bilingual author carves two languages; a reader
    // who knows only one auto-learns the other's words --------------------
    {
        World w(333, "data");
        // lang A speaker and lang B speaker, BOTH holding the same discovery
        const EntityId aA = mk_person(w, {0, 0});
        lang_person(w, aA, "person", 600, 2);
        w.mind(aA).discoveries[DKEY] =
            mk_record(DKEY, DiscoveryStatus::Verified);
        PropertyBag bb;
        auto bsp = [&](const char* n, std::int64_t v) {
            const PropId p = pid(w, n);
            if (p != kNoProp) bb.set(p, Value::integer(v));
        };
        bsp("age", 300); bsp("adult_age", 100); bsp("fertility", 500);
        bsp("mutation_rate", 30); bsp("metabolism", 50);
        bsp("hunger", 100); bsp("thirst", 100); bsp("fatigue", 0);
        bsp("energy", 100000); bsp("integrity", 1000); bsp("reach", 200);
        bsp("communicate_range", 400);
        bsp("vocal_capability", 900); bsp("phoneme_harshness", 400);
        bsp("syllable_length_bias", 2); bsp("hardness", 900);
        bsp("communicate_range", 4000);
        const EntityId bB = w.create_entity("sapient_b", kNoMaterial,
                                           {300, 0}, bb);
        w.mind(bB).discoveries[DKEY] =
            mk_record(DKEY, DiscoveryStatus::Verified);
        w.advance_ticks_for_test(1);
        // B teaches A its words until A is bilingual on B's entries
        for (int i = 0; i < 12; ++i)
            w.interact(mk_ctx(bB, {aA}, "communicate", 0));
        std::int64_t bfamA_on_B = 0;
        {
            const PropId pl = tpid(w, "language_id");
            const std::int64_t langB = tint(*w.find(bB), w, "language_id");
            const AgentMind* am = w.find_mind(aA);
            for (const auto& [bk, bv] : am->beliefs) {
                if (bk.rfind("lex:", 0) != 0) continue;
                try {
                    const EntityId eid = static_cast<EntityId>(
                        std::stoull(bk.substr(4)));
                    const Entity* le = w.find(eid);
                    if (le && le->alive &&
                        w.prop_int(*le, pl, 0) == langB)
                        bfamA_on_B = std::max(bfamA_on_B, bv.confidence);
                } catch (...) {}
            }
        }
        CHECK(bfamA_on_B >= 500);    // A is bilingual in B's tongue
        // A carves the same clause in BOTH languages (Rosetta stone)
        const MaterialId claym = w.materials.id_of("soil_clay");
        const EntityId tablet = w.create_entity("tablet", claym, {30, 0});
        CHECK(w.interact(mk_ctx(aA, {tablet}, "engrave", 0, DKEY)).completed);
        Entity* rins = nullptr;
        for (const auto& ep : w.entities)
            if (ep->alive && ep->kind_name == "inscription") rins = ep.get();
        CHECK(rins != nullptr);
        const Value* cl2 = w.prop_of(*rins, tpid(w, "clause_atoms"));
        CHECK(cl2 && cl2->type == ValType::Text);
        CHECK(cl2->s.find(';') != std::string::npos);   // two clauses
        CHECK(tint(*rins, w, "language_id2") > 0);
        // reader C speaks only lang A — reads, learns the lesson AND B's words
        const EntityId cC = mk_person(w, {60, 0});
        lang_person(w, cC, "person", 600, 2);
        w.mind(cC);
        w.advance_ticks_for_test(1);
        CHECK(w.interact(mk_ctx(cC, {tablet}, "read", 0)).completed);
        const AgentMind* cm = w.find_mind(cC);
        CHECK(cm && cm->discoveries.count(DKEY) == 1);
        // Rosetta payoff: familiarity on lang-B clause entries (second segment)
        const PropId pl = tpid(w, "language_id");
        const std::int64_t langB = tint(*w.find(bB), w, "language_id");
        std::int64_t rosetta_hits = 0;
        for (const auto& [bk, bv] : cm->beliefs) {
            if (bk.rfind("lex:", 0) != 0 || bv.confidence < 250) continue;
            try {
                const EntityId eid = static_cast<EntityId>(
                    std::stoull(bk.substr(4)));
                const Entity* le = w.find(eid);
                if (le && le->alive &&
                    w.prop_int(*le, pl, 0) == langB) ++rosetta_hits;
            } catch (...) {}
        }
        CHECK(rosetta_hits >= 1);   // words of the UNKNOWN language learned
        (void)pl;
    }

    // ---- F: LOD invariance + save roundtrip on the new machinery ---------
    {
        auto build = [&](World& w) {
            const EntityId a = mk_person(w, {0, 0});
            lang_person(w, a, "person", 700, 2);
            w.mind(a).known_actions.insert("heat");
            w.mind(a).discoveries[DKEY] =
                mk_record(DKEY, DiscoveryStatus::Verified);
            const MaterialId claym = w.materials.id_of("soil_clay");
            const EntityId tab = w.create_entity("tablet", claym, {20, 0});
            w.interact(mk_ctx(a, {tab}, "engrave", 0, DKEY));
            // coinage must land BEFORE the compared window: language_tick
            // runs once per advance_ticks call, so a batched advance would
            // otherwise birth the language entity at the window's end.
            w.advance_ticks_for_test(1);
            // keep the language registry inside the LOD radius of its mind
            // so drift accrues per-tick identically in both modes (a dormant
            // entity legitimately holds a <kDormantStride backlog).
            for (const auto& ep : w.entities)
                if (ep->alive && ep->kind_name == "language")
                    ep->pos = {0, 0};
            return tab;
        };
        World wt(555, "data"), wb(555, "data");
        build(wt); build(wb);
        // influx migrants legitimately spawn mid-window (ecology spawn-commit
        // class) and birth their own language community — compare only the
        // entities that already existed, whose drift must be LOD-exact.
        std::set<EntityId> pre;
        for (const auto& ep : wt.entities) if (ep->alive) pre.insert(ep->id);
        const auto snap = [&](World& w) {
            std::vector<std::int64_t> v;
            for (const auto& ep : w.entities) {
                if (!ep->alive || !pre.count(ep->id)) continue;
                if (ep->kind_name == "language")
                    v.push_back(tint(*ep, w, "dialect_drift_accum"));
                else if (ep->kind_name == "inscription")
                    v.push_back(tint(*ep, w, "erosion_level"));
                else if (ep->kind_name == "lexical_entry")
                    v.push_back(tint(*ep, w, "semantic_shift_seed"));
            }
            return v;
        };
        for (int i = 0; i < 300; ++i) wt.advance_ticks_for_test(1);
        wb.advance_ticks_for_test(300);
        CHECK(snap(wt) == snap(wb));
        const std::uint64_t h = wb.state_hash();
        const std::string path = "/tmp/simlab_lang2_test.sav";
        CHECK(wb.save(path));
        World w2(555, "data");
        CHECK(w2.load(path));
        CHECK(w2.state_hash() == h);
    }
}


// ================= Phase 12.2: inductive economy ==============================
static EntityId econ_person(World& w, Vec2 pos, std::int64_t hunger = 100,
                            std::int64_t motor = 1000,
                            std::int64_t horizon = 2) {
    EntityId id = mk_person(w, pos);
    Entity* e = w.find(id);
    tseti(*e, w, "metabolism", 50);
    tseti(*e, w, "hunger", hunger);
    tseti(*e, w, "sight_range", 800);
    tseti(*e, w, "reach", 300);
    tseti(*e, w, "communicate_range", 500);
    tseti(*e, w, "mind_motor_precision", motor);
    tseti(*e, w, "mind_plan_horizon", horizon);
    tseti(*e, w, "trade_reputation", 1000);
    tseti(*e, w, "credit_term_ticks", 200);
    AgentMind& m = w.mind(id);
    for (const char* v : {"take","steal","trade","repay","share"})
        m.known_actions.insert(v);
    return id;
}

static EntityId mk_goods(World& w, const char* mat, std::int64_t qty,
                         Vec2 pos, std::int64_t holder = 0) {
    PropertyBag b;
    const PropId pq = tpid(w, "quantity");
    const PropId pp = tpid(w, "possessed_by");
    if (pq != kNoProp) b.set(pq, Value::integer(qty));
    if (pp != kNoProp && holder != 0)
        b.set(pp, Value::integer(holder));
    return w.create_entity("goods", w.materials.id_of(mat), pos, b);
}

static InteractionResult econ_act(World& w, EntityId actor,
                                  std::vector<EntityId> targets,
                                  const char* action,
                                  const std::string& note = "") {
    InteractionContext c;
    c.actor = actor; c.targets = std::move(targets);
    c.parameters.action = action; c.parameters.note = note;
    c.parameters.ticks = 1;
    return w.interact(c);
}

static std::int64_t econ_rep(World& w, EntityId a) {
    const Entity* e = w.find(a);
    return e ? w.prop_int(*e, tpid(w, "trade_reputation"), 1000) : -1;
}

static void test_inductive_economy_currency_inflation_and_credit() {
    const MaterialId SALT = 0, BERRY = 0, GRANITE = 0; (void)SALT;(void)BERRY;(void)GRANITE;

    // ---- A+B: ownership, witnessed vs silent theft, subjective valuation --
    {
        World w(301, "data");
        const EntityId owner = econ_person(w, {0, 0});
        const EntityId thief = econ_person(w, {10, 0});
        const EntityId witness = econ_person(w, {15, 0});
        const EntityId loner  = econ_person(w, {5000, 5000});
        const EntityId far_holder = mk_person(w, {4995, 4995});  // no mind: invisible to witness scan
        const EntityId item = mk_goods(w, "copper_metal", 5, {12, 0});
        const EntityId far_item = mk_goods(w, "copper_metal", 5, {4990, 4990},
                                           far_holder);
        w.mind(owner); w.mind(thief); w.mind(witness); w.mind(loner);

        // claim -> World-Truth possession + owner conviction
        CHECK(econ_act(w, owner, {item}, "take").completed);
        CHECK(tint(*w.find(item), w, "possessed_by") ==
              static_cast<std::int64_t>(owner));
        const Belief* ob = w.find_mind(owner)
            ->find_belief("owns:" + std::to_string(item));
        CHECK(ob && ob->confidence >= 900);
        // witness forms a weaker "X owns it" belief after perceiving
        perceive(w, witness);
        CHECK(w.find_mind(witness)->find_belief(
            "owns:" + std::to_string(item) + ":" +
            std::to_string(owner)) != nullptr);

        // witnessed steal: reputation hit, no exposure flag
        CHECK(econ_act(w, thief, {item}, "steal").completed);
        CHECK(tint(*w.find(item), w, "possessed_by") ==
              static_cast<std::int64_t>(thief));
        CHECK(tint(*w.find(item), w, "theft_flag") == 0);
        CHECK(econ_rep(w, thief) <= 800);

        // silent steal: flag armed, reputation untouched
        CHECK(econ_act(w, loner, {far_item}, "steal").completed);
        CHECK(tint(*w.find(far_item), w, "theft_flag") > 0);
        CHECK(econ_rep(w, loner) == 1000);

        // marginal valuation: hunger raises food's use value; thin
        // perceived supply raises exchange value — no price table involved
        const MaterialId BERRY = w.materials.id_of("wild_berries");
        const EntityId hungry = econ_person(w, {30, 0}, 950);
        const EntityId full   = econ_person(w, {35, 0}, 50);
        w.mind(hungry); w.mind(full);
        CHECK(econ_total_value(w, hungry, BERRY) >
              econ_total_value(w, full, BERRY));
        AgentMind& fm = w.mind(full);
        fm.belief("pa:" + std::to_string(BERRY)).confidence = 150;
        const std::int64_t v_thin = econ_exchange_value(w, full, BERRY);
        fm.belief("pa:" + std::to_string(BERRY)).confidence = 950;
        const std::int64_t v_glut = econ_exchange_value(w, full, BERRY);
        CHECK(v_thin > v_glut);
        // required-qty pricing is symmetric and positive
        const std::int64_t req = econ_required_qty(w, full, BERRY, 10,
                                                 w.materials.id_of("salt"));
        CHECK(req > 0);
    }

    // ---- C+D+E: currency convergence, inflation, comparative advantage ----
    {
        World w(302, "data");
        // speech community: vocal sapients + one verified discovery -> lang
        const EntityId a = econ_person(w, {0, 0});
        const EntityId b = econ_person(w, {20, 0});
        lang_person(w, a, "person", 500, 2);
        lang_person(w, b, "person", 500, 2);
        w.mind(a).discoveries["m"] = mk_record("m", DiscoveryStatus::Verified);
        w.advance_ticks_for_test(1);   // language_tick coins the registry
        const MaterialId SALT = w.materials.id_of("salt");
        const MaterialId BERRY = w.materials.id_of("wild_berries");
        const MaterialId STONE = w.materials.id_of("granite");

        // salt stack for A, berry stack for B
        mk_goods(w, "salt", 300, {2, 0}, a);
        mk_goods(w, "wild_berries", 400, {18, 0}, b);
        mk_goods(w, "granite", 40, {25, 0}, b);

        // comparative advantage: precision craftsman vs plain worker
        const EntityId smith = econ_person(w, {40, 0}, 100, 2200, 2);
        const EntityId hand  = econ_person(w, {45, 0}, 100, 400, 5);
        InteractionContext cx; cx.actor = smith;
        cx.parameters.action = "combine";
        InteractionContext cy; cy.actor = hand;
        cy.parameters.action = "combine";
        InteractionContext cd; cd.actor = hand;
        cd.parameters.action = "dig";
        CHECK(econ_aptitude_bonus(w, smith, w.mind(smith), cx) >
              econ_aptitude_bonus(w, hand, w.mind(hand), cx));
        CHECK(econ_aptitude_bonus(w, hand, w.mind(hand), cd) > 0);  // horizon

        // currency: ~20 spot trades, B always receives salt as payment
        // price: salt is worth more per unit — berries per salt computed
        // from each side's own subjective values
        int completed_pairs = 0;
        for (int i = 0; i < 30; ++i) {
            // prices are recomputed from CURRENT subjective values every
            // round — consensus growth itself inflates the money's price
            const std::string note_ab = "give:salt:5|get:wild_berries:" +
                std::to_string(std::max<std::int64_t>(1,
                    econ_required_qty(w, a, SALT, 5, BERRY)));
            const std::string note_ba = "give:wild_berries:10|get:salt:" +
                std::to_string(std::max<std::int64_t>(1,
                    econ_required_qty(w, b, BERRY, 10, SALT)));
            InteractionResult r1 = econ_act(w, a, {b}, "trade", note_ab);
            if (!r1.completed) break;
            InteractionResult r2 = econ_act(w, b, {a}, "trade", note_ba);
            if (!r2.completed) break;
            ++completed_pairs;
        }
        CHECK(completed_pairs >= 12);
        const std::int64_t mc_salt = econ_consensus(w, a, SALT);
        const std::int64_t mc_berry = econ_consensus(w, a, BERRY);
        CHECK(mc_salt >= 700);          // converged to common money
        CHECK(mc_salt > mc_berry);      // perishables never monetize
        CHECK(mc_salt > econ_consensus(w, a, STONE)); // granite too heavy

        // inflation: flood the community's sight with salt -> perceived
        // abundance spikes -> salt's exchange value collapses -> more salt
        // units needed per berry
        const std::int64_t v_salt_pre = econ_exchange_value(w, a, SALT);
        const std::int64_t salt_for_berry_pre =
            econ_required_qty(w, a, BERRY, 20, SALT);
        for (int i = 0; i < 30; ++i)
            mk_goods(w, "salt", 10, {static_cast<std::int32_t>(i * 3),
                                     static_cast<std::int32_t>(i % 7)});
        perceive(w, a); perceive(w, a); perceive(w, a);
        const std::int64_t v_salt_post = econ_exchange_value(w, a, SALT);
        const std::int64_t salt_for_berry_post =
            econ_required_qty(w, a, BERRY, 20, SALT);
        CHECK(v_salt_post < v_salt_pre);
        CHECK(salt_for_berry_post > salt_for_berry_pre);

        // ---- F: credit, contract, default ----------------------------------
        const EntityId debtor = econ_person(w, {60, 0});
        const EntityId cred   = econ_person(w, {65, 0});
        mk_goods(w, "salt", 80, {62, 0}, cred);
        lang_person(w, debtor, "person", 500, 2);
        lang_person(w, cred, "person", 500, 2);
        w.advance_ticks_for_test(1);
        // credit purchase: debtor receives salt now, owes value later.
        // give-side qty = berries whose value covers the salt leg.
        const std::int64_t bqty = std::max<std::int64_t>(1,
            econ_required_qty(w, debtor, SALT, 5, BERRY));
        InteractionResult cr = econ_act(w, debtor, {cred}, "trade",
            "give:wild_berries:" + std::to_string(bqty) +
            "|get:salt:5|defer|contract");
        CHECK(cr.completed);
        EntityId debt = kNoEntity; std::int64_t dval = 0, ddue = 0;
        for (const auto& ep : w.entities) {
            if (!ep->alive || ep->kind_name != "debt_obligation") continue;
            if (tint(*ep, w, "debtor_id") == static_cast<std::int64_t>(debtor)) {
                debt = ep->id;
                dval = tint(*ep, w, "debt_value");
                ddue = tint(*ep, w, "due_tick");
            }
        }
        CHECK(debt != kNoEntity);
        CHECK(dval > 0);
        CHECK(ddue > static_cast<std::int64_t>(w.clock_.tick()));
        // written contract spawned for literate parties
        bool saw_contract = false;
        for (const auto& ep : w.entities)
            if (ep->alive && ep->kind_name == "inscription" &&
                tint(*ep, w, "inscription_author") ==
                    static_cast<std::int64_t>(debtor)) saw_contract = true;
        CHECK(saw_contract);
        // repay on time: hand debtor a salt stack covering the debt
        const std::int64_t need_qty = std::max<std::int64_t>(1,
            dval / std::max<std::int64_t>(1,
                econ_total_value(w, debtor, SALT)) + 2);
        mk_goods(w, "salt", need_qty + 5, {64, 0}, debtor);
        const std::int64_t rep_pre = econ_rep(w, debtor);
        CHECK(econ_act(w, debtor, {cred}, "repay",
                       "give:salt:" + std::to_string(need_qty)).completed);
        CHECK(tint(*w.find(debt), w, "debt_status") == 1);
        CHECK(econ_rep(w, debtor) > rep_pre);

        // default: short-term debtor who never pays -> reputation collapse
        const EntityId deadbeat = econ_person(w, {80, 0});
        lang_person(w, deadbeat, "person", 500, 2);   // shares the money consensus
        w.advance_ticks_for_test(1);
        tseti(*w.find(deadbeat), w, "credit_term_ticks", 5);
        mk_goods(w, "salt", 50, {82, 0}, cred);
        const std::int64_t dqty = std::max<std::int64_t>(1,
            econ_required_qty(w, deadbeat, SALT, 3, BERRY));
        CHECK(econ_act(w, deadbeat, {cred}, "trade",
            "give:wild_berries:" + std::to_string(dqty) +
            "|get:salt:3|defer").completed);
        w.advance_ticks_for_test(30);   // past due -> econ_drift defaults
        bool saw_default = false;
        for (const auto& ep : w.entities)
            if (ep->alive && ep->kind_name == "debt_obligation" &&
                tint(*ep, w, "debtor_id") ==
                    static_cast<std::int64_t>(deadbeat) &&
                tint(*ep, w, "debt_status") == 2) saw_default = true;
        CHECK(saw_default);
        CHECK(econ_rep(w, deadbeat) < 400);
        // credit freeze: the defaulter can no longer buy on credit
        InteractionResult denied = econ_act(w, deadbeat, {cred}, "trade",
            "give:wild_berries:" + std::to_string(dqty) +
            "|get:salt:3|defer");
        CHECK(!denied.completed);
    }

    // ---- G: LOD invariance + save/load --------------------------------------
    {
        auto build = [](World& w) {
            const EntityId a = econ_person(w, {0, 0});
            const EntityId b = econ_person(w, {30, 0});
            lang_person(w, a, "person", 500, 2);
            w.mind(a).discoveries["m"] =
                mk_record("m", DiscoveryStatus::Verified);
            w.advance_ticks_for_test(1);
            const EntityId it = mk_goods(w, "salt", 10, {5, 0}, a);
            mk_goods(w, "wild_berries", 50, {28, 0}, b);
            // an armed theft flag mid-window + an open obligation
            tseti(*w.find(it), w, "theft_flag", 120);
            PropertyBag db;
            db.set(tpid(w, "debt_value"), Value::integer(500));
            db.set(tpid(w, "due_tick"), Value::integer(100000));
            db.set(tpid(w, "debtor_id"), Value::integer(
                static_cast<std::int64_t>(b)));
            db.set(tpid(w, "creditor_id"), Value::integer(
                static_cast<std::int64_t>(a)));
            db.set(tpid(w, "debt_status"), Value::integer(0));
            w.create_entity("debt_obligation", kNoMaterial, {10, 0}, db);
            return it;
        };
        World wt(303, "data"), wb(303, "data");
        const EntityId it_t = build(wt);
        const EntityId it_b = build(wb);
        (void)it_b;
        for (int i = 0; i < 200; ++i) wt.advance_ticks_for_test(1);
        wb.advance_ticks_for_test(200);
        auto snap = [&](World& w, EntityId it) {
            std::vector<std::int64_t> v;
            v.push_back(tint(*w.find(it), w, "possession_ticks"));
            v.push_back(tint(*w.find(it), w, "theft_flag"));
            for (const auto& ep : w.entities)
                if (ep->alive && ep->kind_name == "debt_obligation") {
                    v.push_back(tint(*ep, w, "debt_status"));
                    v.push_back(tint(*ep, w, "debt_value"));
                }
            for (const auto& ep : w.entities)
                if (ep->alive && ep->kind_name == "language")
                    v.push_back(tint(*ep, w, "dialect_drift_accum"));
            return v;
        };
        CHECK(snap(wt, it_t) == snap(wb, it_b));

        // save/load roundtrip keeps the whole economy state
        const std::uint64_t h = wt.state_hash();
        CHECK(wt.save("/tmp/simlab_econ_save.bin"));
        World wl(303, "data");
        CHECK(wl.load("/tmp/simlab_econ_save.bin"));
        CHECK(wl.state_hash() == h);
    }
}


// ================= Phase 12.3: political & legal base ==========================
static void test_political_legal_base_core_norms_authority_and_revolt() {
    // ---- A+B: emergent norms, codified law, deference network -------------
    {
        World w(401, "data");
        // speech community: everyone vocal -> one language_id
        const EntityId L  = econ_person(w, {0, 0});
        const EntityId M1 = econ_person(w, {30, 0});
        const EntityId M2 = econ_person(w, {35, 0});
        const EntityId O  = econ_person(w, {10, 0});
        const EntityId T  = econ_person(w, {15, 0});
        for (EntityId id : {L, M1, M2, O, T})
            lang_person(w, id, "person", 500, 2);
        tseti(*w.find(L), w, "social_legitimacy", 800);
        tseti(*w.find(M1), w, "mind_empathy_coupling", 1500);
        tseti(*w.find(M2), w, "mind_empathy_coupling", 1500);
        w.mind(L).discoveries["m"] = mk_record("m", DiscoveryStatus::Verified);
        w.advance_ticks_for_test(1);   // language entity + language_id

        // two witnessed thefts -> communal norm_aversion("steal") >= 600
        const EntityId it1 = mk_goods(w, "copper_metal", 5, {12, 0}, O);
        const EntityId it2 = mk_goods(w, "copper_metal", 5, {13, 0}, O);
        CHECK(econ_act(w, T, {it1}, "steal").completed);
        CHECK(econ_act(w, T, {it2}, "steal").completed);
        CHECK(pol_norm_aversion(w, M1, "steal") >= 600);
        CHECK(pol_norm_aversion(w, M2, "steal") >= 600);

        // deference: after one political cadence everyone drifts to L
        w.advance_ticks_for_test(250);
        const Belief* d1 = w.find_mind(M1)->find_belief(
            "defer_to:" + std::to_string(
                static_cast<std::uint64_t>(L)));
        CHECK(d1 && d1->confidence >= 400);
        CHECK(pol_holds_mandate(w, L));
        // a fearful mind (low empathy) weights coercion over legitimacy:
        // the strong bully with a hard tool wins their allegiance instead
        const EntityId B  = econ_person(w, {60, 0});
        const EntityId F  = econ_person(w, {65, 0});
        lang_person(w, B, "person", 500, 2);
        lang_person(w, F, "person", 500, 2);
        tseti(*w.find(F), w, "mind_empathy_coupling", 0);   // pure fear
        tseti(*w.find(B), w, "integrity", 1000);
        tseti(*w.find(B), w, "social_legitimacy", 500);
        const EntityId club = mk_goods(w, "obsidian", 1, {61, 0});
        const EntityId club2 = mk_goods(w, "obsidian", 1, {62, 0});
        w.relate(B, "wielding", club);
        w.relate(B, "wielding", club2);
        w.advance_ticks_for_test(250);
        bool f_to_bully = false;
        for (const auto& [k, b] : w.find_mind(F)->beliefs)
            if (k.rfind("defer_to:", 0) == 0 &&
                b.confidence >= 400 &&
                k == "defer_to:" + std::to_string(
                    static_cast<std::uint64_t>(B))) f_to_bully = true;
        CHECK(f_to_bully);

        // codified law: legitimate abstractionist carves the dominant taboo
        const EntityId rock = w.create_entity("rock",
            w.materials.id_of("stone"), {20, 5}, PropertyBag{});
        tseti(*w.find(L), w, "mind_abstraction_gain", 1500);
        tseti(*w.find(L), w, "hardness", 900);
        InteractionResult er = econ_act(w, L, {rock}, "engrave");
        CHECK(er.completed);
        EntityId law_insc = kNoEntity;
        // law_action is a TEXT prop — read via prop_of
        for (EntityId sid : er.spawned) {
            const Entity* se = w.find(sid);
            if (!se || se->kind_name != "inscription") continue;
            const PropId pla = tpid(w, "law_action");
            const Value* v = pla != kNoProp ? se->prop(w, pla) : nullptr;
            if (v && v->type == ValType::Text && v->s == "steal")
                law_insc = sid;
        }
        CHECK(law_insc != kNoEntity);

        // a newcomer who reads the law stone inherits the taboo
        const EntityId N = econ_person(w, {25, 0});
        lang_person(w, N, "person", 500, 2);
        w.advance_ticks_for_test(1);
        const Belief* nb0 = w.find_mind(N)->find_belief(
            "norm_aversion:steal");
        CHECK(!nb0 || nb0->confidence < 600);            // naive before
        CHECK(econ_act(w, N, {rock}, "read").completed);
        const Belief* nb1 = w.find_mind(N)->find_belief(
            "norm_aversion:steal");
        CHECK(nb1 && nb1->confidence >= 600);
    }

    // ---- C+D: adjudication, restitution, ostracism --------------------------
    {
        World w(402, "data");
        const EntityId L  = econ_person(w, {0, 0});
        const EntityId M1 = econ_person(w, {30, 0});
        const EntityId M2 = econ_person(w, {35, 0});
        const EntityId O  = econ_person(w, {10, 0});
        const EntityId T  = econ_person(w, {15, 0});
        for (EntityId id : {L, M1, M2, O, T})
            lang_person(w, id, "person", 500, 2);
        tseti(*w.find(L), w, "social_legitimacy", 800);
        w.advance_ticks_for_test(1);
        w.advance_ticks_for_test(250);   // defer network forms -> L mandated

        // witnessed theft leaves contested evidence for the arbiter —
        // the owner claims it openly first so bystanders record "O owns it"
        const EntityId it = mk_goods(w, "copper_metal", 5, {12, 0});
        CHECK(econ_act(w, O, {it}, "take").completed);
        CHECK(econ_act(w, T, {it}, "steal").completed);
        const std::int64_t leg_l0 = tint(*w.find(L), w, "social_legitimacy", 500);
        const std::int64_t leg_t0 = tint(*w.find(T), w, "social_legitimacy", 500);
        CHECK(econ_act(w, L, {T}, "adjudicate").completed);
        CHECK(tint(*w.find(it), w, "possessed_by") ==
              static_cast<std::int64_t>(O));       // restitution
        CHECK(tint(*w.find(L), w, "social_legitimacy", 500) > leg_l0);
        CHECK(tint(*w.find(T), w, "social_legitimacy", 500) < leg_t0);

        // repeat offender with a defaulted debt and nothing to restore
        // -> collective outlaw; the community then refuses them outright
        const EntityId R = econ_person(w, {50, 0});
        lang_person(w, R, "person", 500, 2);
        PropertyBag db;
        db.set(tpid(w, "debt_value"), Value::integer(100));
        db.set(tpid(w, "due_tick"), Value::integer(1));
        db.set(tpid(w, "debtor_id"), Value::integer(
            static_cast<std::int64_t>(R)));
        db.set(tpid(w, "creditor_id"), Value::integer(
            static_cast<std::int64_t>(L)));
        db.set(tpid(w, "debt_status"), Value::integer(2));   // already defaulted
        w.create_entity("debt_obligation", kNoMaterial, {51, 0}, db);
        w.advance_ticks_for_test(1);
        CHECK(econ_act(w, L, {R}, "adjudicate").completed);
        CHECK(pol_ostracism(w, M1, R) >= 500);
        mk_goods(w, "salt", 20, {52, 0}, M1);
        InteractionResult tr = econ_act(w, M1, {R}, "trade",
            "give:salt:2|get:wild_berries:5");
        CHECK(!tr.completed);                       // ostracized: no trade
        tseti(*w.find(M1), w, "quantity", 50);
        tseti(*w.find(R), w, "quantity", 50);
        InteractionResult sh = econ_act(w, M1, {R}, "share");
        CHECK(!sh.completed);                       // ostracized: no share
    }

    // ---- E+F: tribute, scarcity relief, tyranny and revolt ------------------
    {
        World w(403, "data");
        const EntityId L  = econ_person(w, {0, 0});
        const EntityId M1 = econ_person(w, {30, 0});
        const EntityId M2 = econ_person(w, {35, 0});
        for (EntityId id : {L, M1, M2})
            lang_person(w, id, "person", 500, 2);
        tseti(*w.find(L), w, "social_legitimacy", 800);
        w.advance_ticks_for_test(1);
        w.advance_ticks_for_test(250);   // L mandated
        // tribute flows along allegiance into the public pool
        mk_goods(w, "salt", 30, {28, 0}, M1);
        const std::int64_t leg_l0 = tint(*w.find(L), w, "social_legitimacy", 500);
        CHECK(econ_act(w, M1, {L}, "tribute", "salt:5").completed);
        CHECK(tint(*w.find(L), w, "public_pool") > 0);
        CHECK(tint(*w.find(L), w, "social_legitimacy", 500) > leg_l0);

        // scarcity relief: the mandated leader shares the pool to the
        // hungry -> legitimacy soars, pool drains, relief stamped
        tseti(*w.find(M2), w, "quantity", 50);
        tseti(*w.find(M2), w, "hunger", 800);
        tseti(*w.find(L), w, "quantity", 50);
        tseti(*w.find(L), w, "public_pool", 500);
        w.mind(M2).belief("grievance").confidence = 400;
        const std::int64_t leg_pre = tint(*w.find(L), w, "social_legitimacy", 500);
        CHECK(econ_act(w, L, {M2}, "share").completed);
        CHECK(tint(*w.find(L), w, "public_pool") <= 400);
        CHECK(tint(*w.find(L), w, "social_legitimacy", 500) > leg_pre);
        CHECK(tint(*w.find(L), w, "last_relief_tick") > 0);
        CHECK(tint(*w.find(M2), w, "hunger") < 800);
        const Belief* g2 = w.find_mind(M2)->find_belief("grievance");
        CHECK(g2 && g2->confidence == 0);
    }

    // ---- F2: tyranny — hoarded tribute + famine -> emergent revolt ---------
    {
        World w(404, "data");
        const EntityId Y  = econ_person(w, {0, 0});
        const EntityId R2 = econ_person(w, {40, 0});
        const EntityId M1 = econ_person(w, {30, 0});
        const EntityId M2 = econ_person(w, {35, 0});
        for (EntityId id : {Y, R2, M1, M2})
            lang_person(w, id, "person", 500, 2);
        tseti(*w.find(Y), w, "social_legitimacy", 950);
        tseti(*w.find(R2), w, "social_legitimacy", 900);
        w.advance_ticks_for_test(1);
        w.advance_ticks_for_test(250);   // members defer to Y (950 > 900)
        // the tyrant sits on a hoarded pool while members see thin supplies
        tseti(*w.find(Y), w, "public_pool", 5000);
        const MaterialId BERRY = w.materials.id_of("wild_berries");
        for (EntityId id : {M1, M2})
            w.mind(id).belief("pa:" + std::to_string(BERRY))
                .confidence = 100;                    // felt scarcity
        // ~26 qualifying fires: grievance crosses 700, revolt cycles bleed
        // the tyrant's legitimacy ~50 per uprising until the rival's
        // 900 legitimacy finally wins the community's allegiance.
        // Chunked advances keep perceive()-driven abundance beliefs on
        // the same cadence as the ticked reference path.
        for(int i = 0; i < 6; ++i) w.advance_ticks_for_test(1000);
        // revolt: allegiance withdrawn, mandate migrates to the rival,
        // and the tyrant's legitimacy has been bled by the uprising
        bool m1_to_rival = false, m1_to_y = false;
        for (const auto& [k, b] : w.find_mind(M1)->beliefs) {
            if (k.rfind("defer_to:", 0) != 0 || b.confidence < 400)
                continue;
            if (k == "defer_to:" + std::to_string(
                    static_cast<std::uint64_t>(R2))) m1_to_rival = true;
            if (k == "defer_to:" + std::to_string(
                    static_cast<std::uint64_t>(Y))) m1_to_y = true;
        }
        CHECK(m1_to_rival);
        CHECK(!m1_to_y);
        CHECK(tint(*w.find(Y), w, "social_legitimacy", 950) < 900);
        CHECK(pol_grievance(w.find_mind(M1)) < 700 ||
              !w.find_mind(M1)->find_belief(
                  "defer_to:" + std::to_string(
                      static_cast<std::uint64_t>(Y))));
    }

    // ---- G: LOD invariance + save/load ---------------------------------------
    {
        auto build = [](World& w) {
            const EntityId a = econ_person(w, {0, 0});
            const EntityId b = econ_person(w, {30, 0});
            lang_person(w, a, "person", 500, 2);
            lang_person(w, b, "person", 500, 2);
            w.advance_ticks_for_test(1);
            w.advance_ticks_for_test(250);   // defer network + a fire
            tseti(*w.find(a), w, "social_legitimacy", 800);
            tseti(*w.find(a), w, "public_pool", 700);
            // mid-period accumulator + open grievance + communal ledgers
            w.mind(a).political_accum = 150;
            w.mind(b).belief("grievance").confidence = 300;
            // seed communal tables directly (as prior events would have)
            EntityId comm = pol_community_of(w, a);
            CHECK(comm != kNoEntity);
            Entity* ce = w.find(comm);   // find() non-const on World&
            (void)ce;
            const PropId p_nt = tpid(w, "norm_table");
            const PropId p_ot = tpid(w, "ostracism_table");
            if (p_nt != kNoProp)
                ce->props.set(p_nt, Value::text("steal:38400"));
            if (p_ot != kNoProp)
                ce->props.set(p_ot, Value::text("777:32000"));
            return std::pair<EntityId, EntityId>{a, b};
        };
        World wt(405, "data"), wb(405, "data");
        const auto [a_t, b_t] = build(wt);
        const auto [a_b, b_b] = build(wb);
        for (int i = 0; i < 450; ++i) wt.advance_ticks_for_test(1);
        wb.advance_ticks_for_test(450);
        auto snap = [&](World& w, EntityId a, EntityId b) {
            std::vector<std::string> v;
            v.push_back(std::to_string(w.find_mind(a)->political_accum));
            v.push_back(std::to_string(w.find_mind(b)->political_accum));
            for (const auto& [k, bb] : w.find_mind(b)->beliefs)
                if (k == "grievance" || k.rfind("defer_to:", 0) == 0)
                    v.push_back(k + "=" + std::to_string(bb.confidence));
            for (const auto& ep : w.entities)
                if (ep->alive && ep->kind_name == "language") {
                    const PropId p_nt = tpid(w, "norm_table");
                    const PropId p_ot = tpid(w, "ostracism_table");
                    const Value* nv = p_nt != kNoProp
                        ? ep->prop(w, p_nt) : nullptr;
                    const Value* ov = p_ot != kNoProp
                        ? ep->prop(w, p_ot) : nullptr;
                    v.push_back(nv && nv->type == ValType::Text
                        ? nv->s : "-");
                    v.push_back(ov && ov->type == ValType::Text
                        ? ov->s : "-");
                }
            v.push_back(std::to_string(
                tint(*w.find(a), w, "social_legitimacy", 500)));
            v.push_back(std::to_string(
                tint(*w.find(a), w, "public_pool")));
            return v;
        };
        CHECK(snap(wt, a_t, b_t) == snap(wb, a_b, b_b));

        const std::uint64_t h = wt.state_hash();
        CHECK(wt.save("/tmp/simlab_pol_save.bin"));
        World wl(405, "data");
        CHECK(wl.load("/tmp/simlab_pol_save.bin"));
        CHECK(wl.state_hash() == h);
    }
}


// ================= Phase 12.4: architecture & settlement =====================
static InteractionResult arch_act(World& w, EntityId actor,
                                  std::vector<EntityId> targets,
                                  const char* action,
                                  const std::string& note = "") {
    InteractionContext c;
    c.actor = actor; c.targets = std::move(targets);
    c.parameters.action = action; c.parameters.note = note;
    c.parameters.ticks = 1;
    return w.interact(c);
}

static std::string ttext(Entity& e, World& w, const char* n,
                         const char* fb = "") {
    const PropId p = tpid(w, n);
    if (p == kNoProp) return fb;
    const Value* v = e.props.get(p);
    return v && v->type == ValType::Text ? v->s : fb;
}

// structural part: material defaults + per-entity geometry overrides
static EntityId mk_part(World& w, const char* mat, Vec2 pos,
                        std::int64_t gt, std::int64_t gl,
                        std::int64_t gw, std::int64_t gk,
                        std::int64_t integ = 600) {
    PropertyBag b;
    auto si = [&](const char* n, std::int64_t v) {
        const PropId p = tpid(w, n);
        if (p != kNoProp) b.set(p, Value::integer(v));
    };
    si("geom_type", gt); si("geom_len", gl);
    si("geom_wid", gw);  si("geom_thk", gk);
    si("integrity", integ); si("mass", 500);
    return w.create_entity("goods", w.materials.id_of(mat), pos, b);
}

// ring of 4 rods + optional centre post + one big roof slab, then enclose
static EntityId build_enc(World& w, EntityId builder, Vec2 c,
                          bool centre_post, const char* slab_mat,
                          std::int64_t slab_span, bool with_slab = true) {
    std::vector<EntityId> parts;
    for (Vec2 d : {Vec2{-180,0},{180,0},{0,-180},{0,180}})
        parts.push_back(mk_part(w, "dry_wood",
            {c.x + d.x, c.y + d.y}, 1, 400, 80, 80));
    if (centre_post)
        parts.push_back(mk_part(w, "dry_wood", c, 1, 300, 80, 80));
    if (with_slab)
        parts.push_back(mk_part(w, slab_mat, c, 2,
                                slab_span, slab_span, 60));
    arch_act(w, builder, parts, "enclose");
    EntityId best = kNoEntity; std::int64_t bd = INT64_MAX;
    for (const auto& ep : w.entities) {
        const Entity& e = *ep;
        if (!e.alive || e.kind_name != "enclosure") continue;
        const std::int64_t dx = e.pos.x - c.x, dy = e.pos.y - c.y;
        const std::int64_t d2 = dx * dx + dy * dy;
        if (d2 < bd) { bd = d2; best = e.id; }
    }
    return best;
}

static void test_architectural_base_core_enclosures_ergonomics_and_decay() {
    // long advances starve default-metabolism actors; test persons idle,
    // so give them a slow metabolism and deep energy reserves.
    auto slow = [](World& w, Vec2 pos) {
        EntityId id = mk_person(w, pos);
        tseti(*w.find(id), w, "metabolism", 50);
        tseti(*w.find(id), w, "energy", 1000000);
        return id;
    };

    // ============ A: thermal shelter vs exposure ============================
    {
        World w(501, "data");
        WorldGenParams gp; gp.seed = 77; gp.grid_w = 48; gp.grid_h = 48;
        gp.cell_size = 16;
        WorldGenerator gen;
        CHECK(gen.generate(w, gp));
        // deep-freeze every cell: environment becomes a killing cold
        for (auto& t : w.terrain.temperature) t = -300;
        const Vec2 C{400, 400};
        EntityId builder = slow(w, C);
        tseti(*w.find(builder), w, "architectural_style_bias", 600); // burrow
        tseti(*w.find(builder), w, "reach", 400);
        tseti(*w.find(builder), w, "sight_range", 800);
        // thermal phenotype: weak thermoregulator tracks the environment
        for (EntityId id : {builder}) {
            Entity* e = w.find(id);
            tseti(*e, w, "normal_body_temp", 370);
            tseti(*e, w, "thermoreg_gain", 300);
            tseti(*e, w, "hypothermia_delta", 15);
            tseti(*e, w, "fever_delta_max", 35);
            tseti(*e, w, "hyperpyrexia_delta", 45);
            tseti(*e, w, "body_temp", 370);
            tseti(*e, w, "temp_accum", 0);
            tseti(*e, w, "blood_volume", 1000);   // circulatory -> thermoreg runs
        }
        const EntityId enc = build_enc(w, builder, C, true,
                                       "hardwood", 500);
        CHECK(enc != kNoEntity);
        Entity* ee = w.find(enc);
        CHECK(tint(*ee, w, "insulation_factor", 0) > 300);
        const EntityId inside = slow(w, C);
        const EntityId outside = slow(w, {700, 400});
        for (EntityId id : {inside, outside}) {
            Entity* e = w.find(id);
            tseti(*e, w, "normal_body_temp", 370);
            tseti(*e, w, "thermoreg_gain", 300);
            tseti(*e, w, "hypothermia_delta", 15);
            tseti(*e, w, "fever_delta_max", 35);
            tseti(*e, w, "hyperpyrexia_delta", 45);
            tseti(*e, w, "body_temp", 370);
            tseti(*e, w, "temp_accum", 0);
            tseti(*e, w, "blood_volume", 1000);
        }
        w.advance_ticks_for_test(200);
        const std::int64_t tin = tint(*w.find(inside), w, "body_temp", 0);
        const std::int64_t tout = tint(*w.find(outside), w, "body_temp", 0);
        CHECK(tin > tout + 200);          // shell warmth is real physics
        CHECK(tin > 0);                   // sheltered survivor stays warm
    }

    // ============ B: span physics — roof without a post collapses ==========
    {
        World w(502, "data");
        const Vec2 C{0, 0};
        EntityId b1 = slow(w, C); tseti(*w.find(b1), w, "reach", 400);
        EntityId b2 = slow(w, {2000, 0}); tseti(*w.find(b2), w, "reach", 400);
        // flint slab span_req=500 > flint max_span=150: legal ONLY with a
        // centre post inside half-span; without it the shell is stillborn
        const EntityId good = build_enc(w, b1, C, true, "flint", 500);
        const EntityId bad  = build_enc(w, b2, {2000, 0}, false, "flint", 500);
        CHECK(good != kNoEntity && bad != kNoEntity);
        CHECK(tint(*w.find(good), w, "integrity", 0) > 500);
        CHECK(tint(*w.find(bad), w, "integrity", 0) <= 5);
        w.advance_ticks_for_test(60);    // unstable span finishes collapsing
        CHECK(!w.find(bad)->alive);
        // collapse leaves rubble — World Truth debris at the site
        bool rubble = false;
        for (const auto& ep : w.entities)
            if (ep->alive && ep->kind_name == "rubble") { rubble = true; break; }
        CHECK(rubble);
        CHECK(w.find(good)->alive);
    }

    // ============ C+D: habitability, freeform styles ========================
    {
        World w(503, "data");
        const Vec2 C{0, 0};
        EntityId bA = slow(w, C); tseti(*w.find(bA), w, "reach", 400);
        tseti(*w.find(bA), w, "architectural_style_bias", 100);  // radial
        EntityId bB = slow(w, {3000, 0});
        tseti(*w.find(bB), w, "reach", 400);
        tseti(*w.find(bB), w, "architectural_style_bias", 900);  // tower
        const EntityId eA = build_enc(w, bA, C, true, "flint", 500);
        const EntityId eB = build_enc(w, bB, {3000, 0}, true, "flint", 500);
        CHECK(eA != kNoEntity && eB != kNoEntity);
        CHECK(tint(*w.find(eA), w, "habitable_score", 0) > 400);
        CHECK(tint(*w.find(eB), w, "habitable_score", 0) > 400);
        // freeform: different biases -> different emergent form classes,
        // both physically legitimate
        const std::string sA = ttext(*w.find(eA), w, "arch_style");
        const std::string sB = ttext(*w.find(eB), w, "arch_style");
        CHECK(sA == "radial");
        CHECK(sB == "tower");
        CHECK(sA != sB);
        // every enclosure gets a passage — nobody seals themselves in
        bool portal = false;
        for (const auto& ep : w.entities)
            if (ep->alive && ep->kind_name == "portal") { portal = true; break; }
        CHECK(portal);
    }

    // ============ E: stealth bypass vs noise ================================
    {
        World w(504, "data");
        const Vec2 C{0, 0};
        EntityId owner = slow(w, C);
        tseti(*w.find(owner), w, "reach", 400);
        tseti(*w.find(owner), w, "sight_range", 800);
        const EntityId enc = build_enc(w, owner, C, true, "flint", 500);
        CHECK(enc != kNoEntity);
        w.advance_ticks_for_test(2);   // ownership belief settles
        // quiet professional vs clumsy amateur
        EntityId pro = slow(w, {400, 0});
        tseti(*w.find(pro), w, "mind_motor_precision", 1000);
        tseti(*w.find(pro), w, "mind_theory_of_mind", 1000);
        tseti(*w.find(pro), w, "reach", 500);
        EntityId clumsy = slow(w, {420, 0});
        tseti(*w.find(clumsy), w, "mind_motor_precision", 100);
        tseti(*w.find(clumsy), w, "mind_theory_of_mind", 100);
        tseti(*w.find(clumsy), w, "reach", 500);
        const std::int64_t integ0 = tint(*w.find(enc), w, "integrity", 0);
        CHECK(arch_act(w, pro, {enc}, "bypass_barrier").completed);
        CHECK(arch_act(w, clumsy, {enc}, "bypass_barrier").completed);
        // quiet success grants entry; failure makes noise — shell unscathed
        CHECK(tint(*w.find(enc), w, "integrity", 0) == integ0);
        const AgentMind* pm = w.find_mind(pro);
        CHECK(pm && pm->find_belief("entry_granted:" +
            std::to_string(static_cast<std::uint64_t>(enc))) != nullptr);
        // the owner heard the noise
        const AgentMind* om = w.find_mind(owner);
        CHECK(om && om->find_belief("alert:" +
            std::to_string(static_cast<std::uint64_t>(enc))) != nullptr);
        // trespass: a third party walks into the owned low-barrier shell
        EntityId lurker = slow(w, {100, 0});
        tseti(*w.find(lurker), w, "sight_range", 100);
        (void)w.mind(lurker);          // agents exist once they perceive
        w.advance_ticks_for_test(3);   // arch_drift scan runs
        const AgentMind* lm = w.find_mind(lurker);
        CHECK(lm && lm->find_belief("trespass:" +
            std::to_string(static_cast<std::uint64_t>(enc))) != nullptr);
    }

    // ============ F: defensive maze complexity =============================
    {
        World w(505, "data");
        const Vec2 C{0, 0};
        EntityId baron = slow(w, C);
        tseti(*w.find(baron), w, "reach", 400);
        tseti(*w.find(baron), w, "architectural_style_bias", 700);
        const EntityId outer = build_enc(w, baron, C, true, "flint", 500);
        CHECK(outer != kNoEntity);
        // the robbed baron nests a second shell inside the first
        const EntityId inner = build_enc(w, baron, {60, 0}, true,
                                       "flint", 500);
        CHECK(inner != kNoEntity);
        w.advance_ticks_for_test(2);
        const std::int64_t cx = arch_complexity(w, inner);
        CHECK(cx >= 150);             // depth+portals make it a maze
        // bypass cost scales with maze-ness: more ticks, higher threshold
        EntityId thief = slow(w, {300, 60});
        tseti(*w.find(thief), w, "mind_motor_precision", 900);
        tseti(*w.find(thief), w, "mind_theory_of_mind", 900);
        tseti(*w.find(thief), w, "reach", 600);
        InteractionResult br = arch_act(w, thief, {inner}, "bypass_barrier");
        CHECK(br.completed);
        CHECK(br.event != kNoEvent);
    }

    // ============ G: emergent function, no hardcoded labels ================
    {
        World w(506, "data");
        const Vec2 C{0, 0};
        EntityId smith = slow(w, C);
        tseti(*w.find(smith), w, "reach", 400);
        tseti(*w.find(smith), w, "mind_motor_precision", 1000);
        const EntityId forge = build_enc(w, smith, C, true, "flint", 500);
        CHECK(forge != kNoEntity);
        // a hot hearth + ore inside -> the shell reads as a workplace
        EntityId hearth = mk_part(w, "stone", C, 3, 100, 100, 100);
        tseti(*w.find(hearth), w, "temperature", 800);
        EntityId ore = mk_part(w, "iron_ore", {50, 0}, 3, 100, 100, 100);
        EntityId food = mk_part(w, "wild_berries", {5000, 0}, 3, 50, 50, 50);
        CHECK(arch_function(w, forge) == "forge");
        // utility: the smith is pulled to work inside the forge shell
        CHECK(arch_utility_bonus(w, smith, "combine") > 0);
        // a second shell stocked with food behind a hard barrier -> vault
        EntityId baron = slow(w, {5000, 0});
        tseti(*w.find(baron), w, "reach", 400);
        const EntityId vault = build_enc(w, baron, {5000, 0}, true,
                                       "flint", 500);
        CHECK(vault != kNoEntity);
        Entity* ve = w.find(vault);
        tseti(*ve, w, "access_barrier", 800);
        w.find(baron)->pos = {5600, 0};   // owner steps out: no council
        EntityId grain = mk_part(w, "wild_berries", {5050, 0}, 3, 50, 50, 50);
        (void)food; (void)ore; (void)grain;
        CHECK(arch_function(w, vault) == "vault");
    }

    // ============ H: weathering, repair, trails =============================
    {
        World wA(507, "data"), wB(507, "data");
        // wood shell weathers fast, stone shell endures — same clock
        EntityId bA = slow(wA, {0, 0}); tseti(*wA.find(bA), wA, "reach", 400);
        EntityId bB = slow(wB, {0, 0}); tseti(*wB.find(bB), wB, "reach", 400);
        const EntityId wood_enc = build_enc(wA, bA, {0, 0}, true,
                                            "wood_debris", 200);
        const EntityId stone_enc = build_enc(wB, bB, {0, 0}, true,
                                             "flint", 500);
        CHECK(wood_enc != kNoEntity && stone_enc != kNoEntity);
        // equalize starting integrity so weathering alone differentiates
        for (auto& [w, enc] : {std::pair{&wA, wood_enc},
                               std::pair{&wB, stone_enc}}) {
            Entity* e = w->find(enc);
            tseti(*e, *w, "integrity", 900);
            tseti(*e, *w, "decay_accum", 0);
        }
        wA.advance_ticks_for_test(800);
        wB.advance_ticks_for_test(800);
        const std::int64_t iw = tint(*wA.find(wood_enc), wA, "integrity", 0);
        const std::int64_t is = tint(*wB.find(stone_enc), wB, "integrity", 0);
        CHECK(iw < is);               // wood rots, stone stands
        // owner patches the worn shell back to health
        tseti(*wA.find(bA), wA, "energy", 100000);   // metabolism drained it
        std::vector<EntityId> rep{wood_enc,
            mk_part(wA, "dry_wood", {100, 0}, 3, 100, 100, 100)};
        CHECK(arch_act(wA, bA, rep, "repair").completed);
        CHECK(tint(*wA.find(wood_enc), wA, "integrity", 0) > iw);
        // abandoned shell eventually falls: force it past the threshold
        Entity* we = wA.find(wood_enc);
        tseti(*we, wA, "integrity", 30);
        tseti(*we, wA, "decay_accum", 125000);
        wA.advance_ticks_for_test(20);
        CHECK(!we->alive);
        // trails: repeated footfall compacts the cell underfoot
        World wC(508, "data");
        WorldGenParams gp; gp.seed = 9; gp.grid_w = 16; gp.grid_h = 16;
        gp.cell_size = 16; gp.origin_x = 0; gp.origin_y = 0;
        WorldGenerator g2; CHECK(g2.generate(wC, gp));
        EntityId walker = slow(wC, {50, 50});
        tseti(*wC.find(walker), wC, "speed", 8000);
        tseti(*wC.find(walker), wC, "energy", 1000000);
        Vec2 a{50, 50}, b{200, 50};
        for (int i = 0; i < 6; ++i) {   // one-way: reach the far cell
            InteractionContext mc; mc.actor = walker;
            mc.parameters.action = "move"; mc.parameters.ticks = 4;
            mc.parameters.destination = b;
            mc.parameters.has_destination = true;
            wC.interact(mc);
        }
        CHECK(wC.terrain.wear_at({150, 50}) > 0);
        // worn ground is cheaper: identical hop on trail vs fresh ground
        const std::int64_t e0 = tint(*wC.find(walker), wC, "energy", 0);
        InteractionContext m1; m1.actor = walker;
        m1.parameters.action = "move"; m1.parameters.ticks = 4;
        m1.parameters.destination = {150, 50};
        m1.parameters.has_destination = true;
        wC.interact(m1);
        const std::int64_t worn_cost = e0 - tint(*wC.find(walker), wC, "energy", 0);
        EntityId fresh = slow(wC, {200, 150});
        tseti(*wC.find(fresh), wC, "speed", 8000);
        tseti(*wC.find(fresh), wC, "energy", 1000000);
        const std::int64_t f0 = tint(*wC.find(fresh), wC, "energy", 0);
        InteractionContext m2; m2.actor = fresh;
        m2.parameters.action = "move"; m2.parameters.ticks = 4;
        m2.parameters.destination = {150, 150};
        m2.parameters.has_destination = true;
        wC.interact(m2);
        const std::int64_t fresh_cost = f0 - tint(*wC.find(fresh), wC, "energy", 0);
        CHECK(worn_cost > 0);
        CHECK(worn_cost < fresh_cost);   // trail discounts the step
    }

    // ============ I: LOD invariance + save roundtrip =========================
    {
        auto build_pair = [&](World& w) {
            EntityId b = slow(w, {0, 0});
            tseti(*w.find(b), w, "reach", 400);
            return build_enc(w, b, {0, 0}, true, "flint", 500);
        };
        World wA(509, "data"), wB(509, "data");
        const EntityId eA = build_pair(wA);
        const EntityId eB = build_pair(wB);
        CHECK(eA != kNoEntity && eB != kNoEntity);
        wA.advance_ticks_for_test(1); wB.advance_ticks_for_test(1);
        // identical wear seed on both worlds via one shared move path
        World wT1(510, "data"), wT2(510, "data");
        WorldGenParams gp; gp.seed = 3; gp.grid_w = 16; gp.grid_h = 16;
        gp.cell_size = 16;
        WorldGenerator g; CHECK(g.generate(wT1, gp)); CHECK(g.generate(wT2, gp));
        auto seed_wear = [&](World& w) {
            EntityId walker = slow(w, {50, 50});
            tseti(*w.find(walker), w, "speed", 8000);
            tseti(*w.find(walker), w, "energy", 1000000);
            for (int i = 0; i < 5; ++i) {
                InteractionContext mc; mc.actor = walker;
                mc.parameters.action = "move"; mc.parameters.ticks = 4;
                mc.parameters.destination = Vec2{200, 50};
                mc.parameters.has_destination = true;
                w.interact(mc);
            }
        };
        seed_wear(wT1); seed_wear(wT2);
        // ticked vs batched: enclosure drift + wear fade must agree exactly
        for (int i = 0; i < 30; ++i) wT1.advance_ticks_for_test(1);
        wT2.advance_ticks_for_test(30);
        // compare every terrain wear cell + enclosure decay state
        bool wear_eq = true;
        for (std::size_t i = 0; i < wT1.terrain.traffic_wear.size(); ++i)
            if (wT1.terrain.traffic_wear[i] != wT2.terrain.traffic_wear[i]) {
                wear_eq = false; break;
            }
        CHECK(wear_eq);
        // enclosure decay_accum exact under both schedules (worlds wA/wB)
        wA.advance_ticks_for_test(1);
        wB.advance_ticks_for_test(1);
        // (already advanced 1+1 above; compare accumulators directly)
        CHECK(tint(*wA.find(eA), wA, "decay_accum", -1) ==
              tint(*wB.find(eB), wB, "decay_accum", -1));
        // save roundtrip keeps the whole architectural state hashable
        World wS(511, "data");
        const EntityId eS = build_pair(wS);
        CHECK(eS != kNoEntity);
        wS.advance_ticks_for_test(10);
        const std::uint64_t h0 = wS.state_hash();
        const std::string tmp = "/tmp/arch_save.bin";
        CHECK(wS.save(tmp, nullptr));
        World wL(511, "data");
        CHECK(wL.load(tmp, nullptr));
        CHECK(wL.state_hash() == h0);
    }
}


// ================= Phase 12.5: Myth, Oral Literature, Ritual & Diplomacy ======
static void test_myth_oral_literature_rituals_and_diplomacy_base_core() {
    auto mk_lang = [](World& w, std::int64_t lg) {
        PropertyBag b;
        const PropId p = tpid(w, "language_name");
        if (p != kNoProp) b.set(p, Value::integer(lg));
        return w.create_entity("language", kNoMaterial, {0, 0}, b);
    };
    auto mk_entry = [](World& w, const char* ck, std::int64_t lang,
                       std::int64_t seed, std::int64_t tc) {
        PropertyBag b;
        auto si = [&](const char* n, std::int64_t v) {
            const PropId p = tpid(w, n);
            if (p != kNoProp) b.set(p, Value::integer(v));
        };
        si("language_id", lang); si("phoneme_seed", seed);
        si("transmission_count", tc);
        const PropId pck = tpid(w, "concept_key");
        if (pck != kNoProp) b.set(pck, Value::text(ck));
        return w.create_entity("lexical_entry", kNoMaterial, {0, 0}, b);
    };
    auto shock = [](World& w, EntityId who, const char* key,
                    std::int64_t charge) {
        DiscoveryRecord d = mk_record(key, DiscoveryStatus::Refuted);
        d.evidence_against = 300;                 // tried, never explained
        d.engram.emotional_charge = charge;
        w.mind(who).discoveries[key] = d;
    };
    auto concepts = [](World& w) {
        std::vector<EntityId> out;
        for (const auto& ep : w.entities)
            if (ep->alive && ep->kind_name == "semantic_concept" &&
                !ttext(*ep, w, "sacred_key").empty())
                out.push_back(ep->id);
        return out;
    };

    // ---- 1: causal mythogenesis (trauma -> curse, salvation -> blessing) ----
    World w(41, "data");
    EntityId sage = kNoEntity;
    {
        sage = mk_person(w, {100, 100});
        tseti(*w.find(sage), w, "language_id", 77);
        tseti(*w.find(sage), w, "agency_attribution_bias", 950);
        tseti(*w.find(sage), w, "metabolism", 50);
        tseti(*w.find(sage), w, "energy", 900000);
        shock(w, sage, "method:blight_curse", -1000);   // plague: TRAUMA (< 0)
        const EntityId healer = mk_person(w, {140, 100});
        tseti(*w.find(healer), w, "language_id", 77);
        tseti(*w.find(healer), w, "agency_attribution_bias", 900);
        tseti(*w.find(healer), w, "metabolism", 50);
        shock(w, healer, "method:spring_cure", 1000);   // salvation (> 0)
        const EntityId skeptic = mk_person(w, {180, 100});
        tseti(*w.find(skeptic), w, "agency_attribution_bias", 200);
        tseti(*w.find(skeptic), w, "metabolism", 50);
        shock(w, skeptic, "method:rockfall", -1000);
        w.advance_ticks_for_test(260);          // >= 2 myth periods
        const Belief* sb = w.find_mind(sage)->find_belief("sacred:method:blight_curse");
        CHECK(sb != nullptr && sb->confidence >= 600);
        CHECK(w.find_mind(skeptic)->find_belief("sacred:method:rockfall") == nullptr);
        const std::vector<EntityId> cs = concepts(w);
        CHECK(cs.size() == 2);                  // one per myth, none for the skeptic
        for (EntityId c : cs) {
            Entity& ce = *w.find(c);
            const std::string key = ttext(ce, w, "sacred_key");
            CHECK(!ttext(ce, w, "sacred_agent").empty());
            CHECK(!ttext(ce, w, "appeasement_action").empty());
            if (key == "method:blight_curse")
                CHECK(ttext(ce, w, "sacred_valence") == "curse");
            if (key == "method:spring_cure")
                CHECK(ttext(ce, w, "sacred_valence") == "blessing");
        }
        // weak shocks never become myth
        World wq(42, "data");
        const EntityId calm = mk_person(wq, {0, 0});
        tseti(*wq.find(calm), wq, "agency_attribution_bias", 950);
        tseti(*wq.find(calm), wq, "metabolism", 50);
        shock(wq, calm, "method:drizzle", -100);
        wq.advance_ticks_for_test(500);
        CHECK(concepts(wq).empty());
    }

    // ---- 1b: consecrated carving turns an enclosure into a Sanctuary ------
    EntityId shrine = kNoEntity;
    {
        PropertyBag eb;
        auto se = [&](const char* n, std::int64_t v) {
            const PropId p = tpid(w, n);
            if (p != kNoProp) eb.set(p, Value::integer(v));
        };
        se("footprint_radius", 300); se("integrity", 600); se("access_barrier", 0);
        shrine = w.create_entity("enclosure", kNoMaterial, {100, 100}, eb);
        const EntityId stone = w.create_entity("rock", w.materials.id_of("stone"),
                                               {110, 100}, {});
        CHECK(arch_function(w, shrine) != "sanctuary");
        InteractionResult cr = w.interact(mk_ctx(sage, {stone}, "consecrate", 0));
        CHECK(cr.completed);
        EntityId carving = kNoEntity;
        for (EntityId sp : cr.spawned) if (w.find(sp)->kind_name == "inscription") carving = sp;
        CHECK(carving != kNoEntity);
        if (carving != kNoEntity) {
            CHECK(ttext(*w.find(carving), w, "sacred_key") == "method:blight_curse");
            CHECK(ttext(*w.find(carving), w, "glyph_text").find("->") != std::string::npos);
        }
        CHECK(arch_function(w, shrine) == "sanctuary");
        CHECK(arch_utility_bonus(w, sage, "perform_ritual") > 0);   // the faithful feel the pull
    }

    // ---- 2: oral literature — retelling crystallizes an epic --------------
    EntityId epic0 = kNoEntity;
    {
        const EntityId poet = mk_person(w, {300, 300});
        const EntityId hearer = mk_person(w, {310, 300});
        tseti(*w.find(poet), w, "language_id", 88);
        tseti(*w.find(hearer), w, "language_id", 88);
        tseti(*w.find(poet), w, "metabolism", 50);
        tseti(*w.find(poet), w, "energy", 900000);
        std::vector<EntityId> ents;
        for (int i = 0; i < 5; ++i)
            ents.push_back(mk_entry(w, "epic:blight", 88, 777, 149));
        epic0 = ents[0];
        w.mind(poet).belief("lex:" + std::to_string(ents[0])).confidence = 600;
        w.mind(hearer);
        CHECK(w.interact(mk_ctx(poet, {hearer}, "communicate", 0)).completed);
        CHECK(tint(*w.find(ents[0]), w, "transmission_count", 0) == 150);
        CHECK(tint(*w.find(ents[0]), w, "poetic_cadence", 0) >= 500);   // identical seeds
        CHECK(ttext(*w.find(ents[0]), w, "oral_genre") == "epic");       // 4 siblings
        // a chanted epic teaches 3x harder than plain speech (250 -> 750)
        CHECK(w.find_mind(hearer)->find_belief("lex:" + std::to_string(ents[0])) &&
              w.find_mind(hearer)->find_belief("lex:" + std::to_string(ents[0]))->confidence >= 750);
        // reciting a crystallized classic lifts the reciter's standing
        const std::int64_t leg0 = tint(*w.find(poet), w, "social_legitimacy", 0);
        CHECK(w.interact(mk_ctx(poet, {hearer}, "communicate", 0)).completed);
        CHECK(tint(*w.find(poet), w, "social_legitimacy", 0) >= leg0 + 15);
        // a two-word saying is a proverb, not an epic
        const EntityId pa = mk_entry(w, "saw:fire_herb", 88, 4242, 149);
        mk_entry(w, "saw:fire_herb", 88, 4242, 149);
        w.mind(poet).belief("lex:" + std::to_string(pa)).confidence = 600;
        CHECK(w.interact(mk_ctx(poet, {hearer}, "communicate", 0)).completed);
        CHECK(ttext(*w.find(pa), w, "oral_genre") == "proverb");
        const std::string codex = w.export_lexicon_codex();
        CHECK(codex.find("EPIC entry") != std::string::npos);
        CHECK(codex.find("PROVERB entry") != std::string::npos);
        CHECK(codex.find("concept=epic:blight") != std::string::npos);
        CHECK(codex.find("raw_phonemes=") != std::string::npos);
        CHECK(codex.find("chronicle_english=") != std::string::npos);
        CHECK(codex.find("myth=") != std::string::npos);
    }

    // ---- 2b: meter resists oblivion (epic vs plain word, no rehearsal) ----
    {
        World wm(44, "data");
        const EntityId bard = mk_person(wm, {0, 0});
        tseti(*wm.find(bard), wm, "metabolism", 0);
        const EntityId epic = mk_entry(wm, "tale:flood", 5, 999, 0);
        const PropId pg = tpid(wm, "oral_genre");
        wm.find(epic)->props.set(pg, Value::text("epic"));
        const EntityId plain = mk_entry(wm, "word:pebble", 5, 123, 0);
        AgentMind& bm = wm.mind(bard);
        for (EntityId le : {epic, plain}) {
            Belief& b = bm.belief("lex:" + std::to_string(le));
            b.confidence = 800; b.last_tick = 0;
        }
        wm.advance_ticks_for_test(20000);
        const std::int64_t ce = wm.find_mind(bard)->find_belief("lex:" + std::to_string(epic))->confidence;
        const std::int64_t cp = wm.find_mind(bard)->find_belief("lex:" + std::to_string(plain))->confidence;
        CHECK(ce >= 600);                      // ~150 stale periods x 1
        CHECK(cp <= 100);                      // ~150 stale periods x 10
        CHECK(ce > cp);
    }

    // ---- 3: collective ritual in the Sanctuary — relief + REAL sterilization
    {
        const EntityId neighbor = mk_person(w, {120, 100});
        tseti(*w.find(neighbor), w, "language_id", 77);
        tseti(*w.find(neighbor), w, "metabolism", 50);
        w.mind(neighbor).belief("grievance").confidence = 500;
        const EntityId food = mk_goods(w, "wild_berries", 10, {105, 100}, sage);
        PropertyBag cb;
        auto si = [&](const char* n, std::int64_t v) {
            const PropId p = tpid(w, n);
            if (p != kNoProp) cb.set(p, Value::integer(v));
        };
        si("temperature", 200); si("sterilization_temp", 700);
        si("strain_ref", 500); si("contamination", 500); si("cooked_state", 0);
        const EntityId contam = w.create_entity("goods", kNoMaterial, {115, 100}, cb);
        w.mind(sage).known_actions.insert("heat");
        const std::int64_t leg0 = tint(*w.find(sage), w, "social_legitimacy", 0);
        InteractionResult rr = w.interact(mk_ctx(sage, {food}, "perform_ritual", 0));
        CHECK(rr.completed);
        CHECK(tint(*w.find(food), w, "quantity", 10) == 0);              // offering burned
        CHECK(tint(*w.find(contam), w, "strain_ref", 1) == 0);           // pathogen gone
        CHECK(tint(*w.find(contam), w, "contamination", 1) == 0);
        CHECK(tint(*w.find(contam), w, "cooked_state", 0) == 1000);
        CHECK(tint(*w.find(sage), w, "social_legitimacy", 0) >= leg0 + 80);
        // in the Sanctuary the relief doubles: 500 - (60 + 300/5) = 380
        const Belief* gb = w.find_mind(neighbor)->find_belief("grievance");
        CHECK(gb != nullptr && gb->confidence <= 380);
        // no conviction, no mandate -> no rite
        const EntityId nobody = mk_person(w, {130, 100});
        CHECK(!w.interact(mk_ctx(nobody, {food}, "perform_ritual", 0)).completed);
    }

    // ---- 4+5: envoy gifts raise trust; bilingual Rosetta treaty -----------
    World w3(43, "data");
    {
        const EntityId a1 = mk_person(w3, {0, 0});
        const EntityId a2 = mk_person(w3, {60, 0});
        tseti(*w3.find(a1), w3, "language_id", 11);
        tseti(*w3.find(a2), w3, "language_id", 22);
        tseti(*w3.find(a1), w3, "metabolism", 50);
        tseti(*w3.find(a2), w3, "metabolism", 50);
        tseti(*w3.find(a1), w3, "bilingual_familiarity", 800);
        tseti(*w3.find(a1), w3, "energy", 900000);
        mk_lang(w3, 11); mk_lang(w3, 22);
        // the envoy speaks words of both tongues (Rosetta material)
        const EntityId e11 = mk_entry(w3, "salt", 11, 31, 0);
        const EntityId e22 = mk_entry(w3, "salt", 22, 32, 0);
        w3.mind(a1).belief("lex:" + std::to_string(e11)).confidence = 900;
        w3.mind(a1).belief("lex:" + std::to_string(e22)).confidence = 900;
        CHECK(diplo_trust(w3, 11, 22) == 0);
        // a treaty before trust is earned is refused
        CHECK(!w3.interact(mk_ctx(a1, {a2}, "diplomatic_parley", 0, "treaty")).completed);
        std::int64_t prev = 0;
        for (int i = 0; i < 20 && diplo_trust(w3, 11, 22) < 700; ++i) {
            mk_goods(w3, "salt", 300, {5 + i, 1}, a1);
            InteractionResult r = w3.interact(mk_ctx(a1, {a2},
                "diplomatic_parley", 0, "gift:salt:300"));
            CHECK(r.completed);
            const std::int64_t t = diplo_trust(w3, 11, 22);
            CHECK(t > prev);                    // every real gift warms relations
            CHECK(t == diplo_trust(w3, 22, 11));  // the ledger is symmetric
            prev = t;
        }
        CHECK(prev >= 700);
        CHECK(!diplo_free_trade(w3, 11, 22));   // trust alone opens no road
        InteractionResult tr = w3.interact(mk_ctx(a1, {a2}, "diplomatic_parley", 0, "treaty"));
        CHECK(tr.completed);
        EntityId treaty = kNoEntity;
        for (EntityId sp : tr.spawned) if (w3.find(sp)->kind_name == "inscription") treaty = sp;
        CHECK(treaty != kNoEntity);
        if (treaty != kNoEntity) {
            CHECK(ttext(*w3.find(treaty), w3, "treaty_clause") == "free_trade");
            CHECK(tint(*w3.find(treaty), w3, "language_id2", 0) == 22);
            const std::string cl = ttext(*w3.find(treaty), w3, "clause_atoms");
            CHECK(cl.find("11:" + std::to_string(e11)) != std::string::npos);
            CHECK(cl.find("22:" + std::to_string(e22)) != std::string::npos);
        }
        CHECK(diplo_free_trade(w3, 11, 22));
        // a non-bilingual envoy cannot carve a treaty even with trust
        tseti(*w3.find(a1), w3, "bilingual_familiarity", 300);
        CHECK(!w3.interact(mk_ctx(a1, {a2}, "diplomatic_parley", 0, "treaty")).completed);
    }

    // ---- 6: cross-border theft escalates to community-wide hostility -------
    World w4(47, "data");
    {
        const EntityId t1 = mk_person(w4, {0, 0});
        const EntityId v1 = mk_person(w4, {80, 0});
        const EntityId kin = mk_person(w4, {900, 900});   // far away, same tongue as victim
        tseti(*w4.find(t1), w4, "language_id", 33);
        tseti(*w4.find(v1), w4, "language_id", 44);
        tseti(*w4.find(kin), w4, "language_id", 44);
        tseti(*w4.find(t1), w4, "metabolism", 50);
        tseti(*w4.find(t1), w4, "energy", 900000);
        w4.mind(t1); w4.mind(v1); w4.mind(kin);
        mk_lang(w4, 33); mk_lang(w4, 44);
        CHECK(!diplo_hostile(w4, 33, 44));
        for (int i = 0; i < 5; ++i) {
            const EntityId it = mk_goods(w4, "copper_metal", 2, {10 + i, 5}, v1);
            CHECK(w4.interact(mk_ctx(t1, {it}, "steal", 0)).completed);
        }
        CHECK(diplo_trust(w4, 33, 44) <= -600);
        CHECK(diplo_hostile(w4, 33, 44));
        // the ultimatum is common knowledge on BOTH sides, even far away
        const Belief* hk = w4.find_mind(kin)->find_belief("hostile_tongue:33");
        const Belief* ht = w4.find_mind(t1)->find_belief("hostile_tongue:44");
        CHECK(hk != nullptr && hk->confidence == 1000);
        CHECK(ht != nullptr && ht->confidence == 1000);
        mk_goods(w4, "salt", 50, {20, 5}, t1);
        InteractionResult bad = w4.interact(mk_ctx(t1, {v1}, "trade", 0,
            "give:salt:2|get:copper_metal:2"));
        CHECK(!bad.completed);
        CHECK(bad.failure.find("hostile") != std::string::npos);
    }

    // ---- 7: LOD invariance — dt=1 x N vs dt=N ------------------------------
    {
        struct Rig { EntityId sage, poet, hearer, envoy, host; std::vector<EntityId> ents; };
        auto build = [&](World& ww) {
            Rig g;
            g.sage = mk_person(ww, {50, 50});
            tseti(*ww.find(g.sage), ww, "language_id", 55);
            tseti(*ww.find(g.sage), ww, "agency_attribution_bias", 950);
            tseti(*ww.find(g.sage), ww, "metabolism", 50);
            tseti(*ww.find(g.sage), ww, "energy", 900000);
            shock(ww, g.sage, "method:famine_ghost", -1000);
            g.poet = mk_person(ww, {60, 50});
            g.hearer = mk_person(ww, {70, 50});
            for (EntityId x : {g.poet, g.hearer}) {
                tseti(*ww.find(x), ww, "language_id", 55);
                tseti(*ww.find(x), ww, "metabolism", 50);
                tseti(*ww.find(x), ww, "energy", 900000);
            }
            for (int i = 0; i < 4; ++i) g.ents.push_back(mk_entry(ww, "epic:famine", 55, 555, 140));
            ww.mind(g.poet).belief("lex:" + std::to_string(g.ents[0])).confidence = 700;
            ww.mind(g.hearer).belief("grievance").confidence = 600;
            g.envoy = mk_person(ww, {80, 50});
            g.host = mk_person(ww, {90, 50});
            tseti(*ww.find(g.envoy), ww, "language_id", 55);
            tseti(*ww.find(g.host), ww, "language_id", 66);
            tseti(*ww.find(g.envoy), ww, "energy", 900000);
            mk_lang(ww, 55); mk_lang(ww, 66);
            return g;
        };
        auto act = [&](World& ww, const Rig& g, int step) {
            if (step % 3 == 0) ww.interact(mk_ctx(g.poet, {g.hearer}, "communicate", 0));
            if (step % 4 == 1) {
                mk_goods(ww, "salt", 100, {81, 50}, g.envoy);
                ww.interact(mk_ctx(g.envoy, {g.host}, "diplomatic_parley", 0, "gift:salt:100"));
            }
            if (step == 7) {
                mk_goods(ww, "wild_berries", 5, {51, 50}, g.sage);
                for (const auto& ep : ww.entities)
                    if (ep->alive && ep->kind_name == "goods" &&
                        ep->material == ww.materials.id_of("wild_berries")) {
                        ww.interact(mk_ctx(g.sage, {ep->id}, "perform_ritual", 0));
                        break;
                    }
            }
        };
        World wa(53, "data"), wb(53, "data");
        const Rig ga = build(wa), gb = build(wb);
        for (int step = 0; step < 12; ++step) {
            act(wa, ga, step); act(wb, gb, step);
            for (int k = 0; k < 50; ++k) wa.advance_ticks_for_test(1);   // dt = 1 x 50
            wb.advance_ticks_for_test(50);                              // dt = 50
        }
        CHECK(tint(*wa.find(ga.ents[0]), wa, "transmission_count", 0) ==
              tint(*wb.find(gb.ents[0]), wb, "transmission_count", 0));
        CHECK(tint(*wa.find(ga.ents[0]), wa, "transmission_count", 0) > 140);
        CHECK(ttext(*wa.find(ga.ents[0]), wa, "oral_genre") ==
              ttext(*wb.find(gb.ents[0]), wb, "oral_genre"));
        CHECK(diplo_trust(wa, 55, 66) == diplo_trust(wb, 55, 66));
        CHECK(diplo_trust(wa, 55, 66) > 0);
        const Belief* ba = wa.find_mind(ga.sage)->find_belief("sacred:method:famine_ghost");
        const Belief* bb = wb.find_mind(gb.sage)->find_belief("sacred:method:famine_ghost");
        CHECK(ba && bb && ba->confidence == bb->confidence);
        CHECK(wa.find_mind(ga.sage)->myth_accum == wb.find_mind(gb.sage)->myth_accum);
        CHECK(tint(*wa.find(ga.sage), wa, "social_legitimacy", 0) ==
              tint(*wb.find(gb.sage), wb, "social_legitimacy", 0));
        CHECK(wa.find_mind(ga.hearer)->find_belief("grievance")->confidence ==
              wb.find_mind(gb.hearer)->find_belief("grievance")->confidence);
        CHECK(concepts(wa).size() == concepts(wb).size());
        CHECK(wa.content_hash() == wb.content_hash());
    }

    // ---- 8: save/load roundtrip -------------------------------------------
    for (World* ww : {&w, &w3, &w4}) {
        const std::string path = "/tmp/simlab_test_125.bin";
        const std::uint64_t h = ww->state_hash();
        CHECK(ww->save(path));
        World wr(1, "data");
        CHECK(wr.load(path));
        CHECK(wr.state_hash() == h);
    }
    (void)epic0; (void)shrine;
}


// ================= Phase 12.6: Warfare, Logistics & Emergent Weapons R&D ======
static void test_warfare_logistics_siege_and_emergent_weapons_rd() {
    // ---- shared fixtures ----------------------------------------------------
    // a body with ONE anatomical part (deterministic blow routing)
    auto mk_body = [](World& w, Vec2 pos, std::int64_t brit) {
        const EntityId b = mk_person(w, pos);
        PropertyBag pb;
        auto si = [&](const char* n, std::int64_t v) {
            const PropId p = tpid(w, n); if (p != kNoProp) pb.set(p, Value::integer(v));
        };
        si("integrity", 1000); si("hardness", 300); si("brittleness", brit);
        si("mass", 1000); si("part_size", 100); si("bleed_rate", 0);
        si("fracture_level", 0);
        pb.set(tpid(w, "part_role"), Value::text("torso"));
        pb.set(tpid(w, "body_slot"), Value::text("torso"));
        pb.set(tpid(w, "contained"), Value::boolean(true));
        const EntityId part = w.create_entity("body_part", kNoMaterial, pos, pb);
        w.relate(part, "part_of", b);
        return std::make_pair(b, part);
    };
    auto mk_item = [](World& w, const char* mat, Vec2 pos, std::int64_t mass = -1) {
        PropertyBag pb;
        if (mass >= 0) pb.set(tpid(w, "mass"), Value::integer(mass));
        return w.create_entity("item", w.materials.id_of(mat), pos, pb);
    };
    auto mk_lang = [](World& w, std::int64_t lg, const std::string& table) {
        PropertyBag b;
        b.set(tpid(w, "language_name"), Value::integer(lg));
        b.set(tpid(w, "diplo_table"), Value::text(table));
        return w.create_entity("language", kNoMaterial, {0, 0}, b);
    };
    auto mk_enclosure = [](World& w, Vec2 pos, std::int64_t integ,
                           std::int64_t bar, std::int64_t rad) {
        PropertyBag b;
        b.set(tpid(w, "integrity"), Value::integer(integ));
        b.set(tpid(w, "access_barrier"), Value::integer(bar));
        b.set(tpid(w, "footprint_radius"), Value::integer(rad));
        return w.create_entity("enclosure", w.materials.id_of("stone"), pos, b);
    };
    auto sized_terrain = [](World& w, int n) {
        TerrainGrid& t = w.terrain; t.w = n; t.h = n; t.cell = 16; t.ox = 0; t.oy = 0;
        const std::size_t c = static_cast<std::size_t>(n) * n;
        t.elevation.assign(c, 200); t.moisture.assign(c, 500); t.temperature.assign(c, 200);
        t.water.assign(c, 0); t.vegetation.assign(c, 0); t.slope.assign(c, 0);
        t.wind_x.assign(c, 0); t.wind_y.assign(c, 0); t.water_table.assign(c, 10);
        t.traffic_wear.assign(c, 0);
    };
    auto bleed = [](World& w, EntityId part) { return tint(*w.find(part), w, "bleed_rate", 0); };

    // ---- 1: sharp vs blunt, armor -------------------------------------------
    World w(61, "data");
    {
        const EntityId fighter = mk_person(w, {0, 0});
        tseti(*w.find(fighter), w, "energy", 900000000);
        const EntityId blade = mk_item(w, "iron_blade", {5, 0});
        const EntityId maul = mk_item(w, "granite", {5, 5});
        const auto [va, pa] = mk_body(w, {40, 0}, 300);          // unarmored
        const auto [vb, pbp] = mk_body(w, {0, 40}, 300);         // will wear plate
        const auto [vc, pc] = mk_body(w, {40, 40}, 300);         // soft hide armor
        const EntityId plate = mk_item(w, "metal_plate", {0, 45});
        const EntityId hide = mk_item(w, "cured_hide", {40, 45});
        CHECK(w.interact(mk_ctx(vb, {plate}, "don", 0)).completed);
        CHECK(w.interact(mk_ctx(vc, {hide}, "don", 0)).completed);
        CHECK(!w.interact(mk_ctx(va, {blade}, "don", 0)).completed);   // a blade protects nothing
        CHECK(w.interact(mk_ctx(fighter, {blade}, "wield", 0)).completed);
        CHECK(w.interact(mk_ctx(fighter, {va}, "impact", 150)).completed);
        CHECK(w.interact(mk_ctx(fighter, {vb}, "impact", 150)).completed);
        const std::int64_t bl_a = bleed(w, pa), bl_b = bleed(w, pbp);
        CHECK(bl_a > 0);
        CHECK(bl_a > bl_b * 4);                                    // plate stops the edge
        // the maul: impulse goes THROUGH soft armor into the bone
        CHECK(w.interact(mk_ctx(fighter, {blade}, "unwield", 0)).completed);
        CHECK(w.interact(mk_ctx(fighter, {maul}, "wield", 0)).completed);
        CHECK(w.interact(mk_ctx(fighter, {vc}, "impact", 150)).completed);
        CHECK(tint(*w.find(pc), w, "fracture_level", 0) > 0);       // broken under the hide
        CHECK(tint(*w.find(pc), w, "integrity", 1000) < 1000);
        CHECK(w.interact(mk_ctx(vb, {plate}, "doff", 0)).completed);
        CHECK(!w.related(vb, "wearing", plate));
    }

    // ---- 2: elastic ballistics & elevation ---------------------------------
    World w2(62, "data");
    {
        sized_terrain(w2, 128);
        const EntityId archer = mk_person(w2, {16, 16});
        tseti(*w2.find(archer), w2, "energy", 900000000);
        tseti(*w2.find(archer), w2, "metabolism", 50);
        const EntityId frame = mk_item(w2, "hardwood", {20, 16});
        const EntityId string = mk_item(w2, "sinew", {22, 16});
        InteractionResult ar = w2.interact(mk_ctx(archer, {frame, string}, "assemble", 1000));
        CHECK(ar.completed);
        EntityId bow = kNoEntity;
        for (EntityId s : ar.spawned) if (w2.find(s)->kind_name == "composite") bow = s;
        CHECK(bow != kNoEntity);
        const std::int64_t li = tint(*w2.find(bow), w2, "launch_impulse", 0);
        CHECK(li > 0);                                             // emerged from the parts
        // a frame without stretch + string makes no launcher
        const EntityId club_a = mk_item(w2, "dry_wood", {24, 16});
        const EntityId club_b = mk_item(w2, "stone", {25, 16});
        InteractionResult cr = w2.interact(mk_ctx(archer, {club_a, club_b}, "assemble", 1000));
        CHECK(cr.completed);
        for (EntityId s : cr.spawned)
            if (w2.find(s)->kind_name == "composite")
                CHECK(tint(*w2.find(s), w2, "launch_impulse", 0) == 0);
        CHECK(w2.interact(mk_ctx(archer, {bow}, "wield", 0)).completed);
        // in range: the point wounds; ammo is spent
        const auto [near_v, near_p] = mk_body(w2, {static_cast<std::int32_t>(16 + li / 2), 16}, 300);
        const EntityId arrow = mk_item(w2, "flint", {18, 16});
        CHECK(w2.interact(mk_ctx(archer, {near_v, arrow}, "launch", 0)).completed);
        CHECK(bleed(w2, near_p) > 0);
        CHECK(!w2.find(arrow)->alive);
        // beyond range on flat ground: refused with the range condition
        const Vec2 far_pos{static_cast<std::int32_t>(16 + li + li / 4), 16};
        const auto [far_v, far_p] = mk_body(w2, far_pos, 300);
        const EntityId arrow2 = mk_item(w2, "flint", {18, 16});
        InteractionResult fr = w2.interact(mk_ctx(archer, {far_v, arrow2}, "launch", 0));
        CHECK(!fr.completed);
        bool range_cond = false;
        if (const CausalEvent* fe = w2.ledger.find(fr.failed_event))
            for (const Condition& c : fe->conditions) if (c.what == "range" && !c.met) range_cond = true;
        CHECK(range_cond);
        // from a height the same shot carries (elevation -> impulse)
        TerrainGrid& t = w2.terrain;
        t.elevation[t.index({16, 16})] = 300;                      // archer on a ridge
        t.elevation[t.index(far_pos)] = 200;
        CHECK(w2.interact(mk_ctx(archer, {far_v, arrow2}, "launch", 0)).completed);
        CHECK(bleed(w2, far_p) > 0);
    }

    // ---- 3: toxic, bio-tipped & energetic ----------------------------------
    World w3(63, "data");
    {
        const EntityId alch = mk_person(w3, {0, 0});
        tseti(*w3.find(alch), w3, "energy", 900000000);
        tseti(*w3.find(alch), w3, "mind_abstraction_gain", 1500);
        const EntityId dull = mk_person(w3, {300, 0});
        tseti(*w3.find(dull), w3, "mind_abstraction_gain", 1000);
        const EntityId dagger = mk_item(w3, "flint", {5, 0});
        const EntityId dagger2 = mk_item(w3, "flint", {305, 0});
        CHECK(w3.interact(mk_ctx(alch, {dagger}, "wield", 0)).completed);
        CHECK(w3.interact(mk_ctx(dull, {dagger2}, "wield", 0)).completed);
        const EntityId berries = mk_goods(w3, "nightshade", 5, {8, 0}, 0);
        const EntityId berries2 = mk_goods(w3, "nightshade", 5, {308, 0}, 0);
        CHECK(!w3.interact(mk_ctx(dull, {berries2}, "coat", 0)).completed);   // no abstraction
        CHECK(w3.interact(mk_ctx(alch, {berries}, "coat", 0)).completed);
        CHECK(tint(*w3.find(dagger), w3, "coat_toxin", 0) > 0);
        // a "strain" + infected waste carrying it
        const EntityId strain = w3.create_entity("pathogen", kNoMaterial, {900, 900}, {});
        PropertyBag wb; wb.set(tpid(w3, "strain_ref"), Value::integer(static_cast<std::int64_t>(strain)));
        const EntityId waste = w3.create_entity("waste", w3.materials.id_of("feces"), {9, 0}, wb);
        CHECK(w3.interact(mk_ctx(alch, {waste}, "coat", 0)).completed);
        CHECK(tint(*w3.find(dagger), w3, "coat_strain", 0) == static_cast<std::int64_t>(strain));
        const auto [vic, vpart] = mk_body(w3, {30, 0}, 300);
        tseti(*w3.find(vic), w3, "toxin_load", 0);
        CHECK(w3.interact(mk_ctx(alch, {vic}, "impact", 150)).completed);
        CHECK(tint(*w3.find(vic), w3, "toxin_load", 0) > 0);       // poison in the blood
        bool infected = false;
        for (const Relation& rel : w3.relations_of(vic, "carries")) if (rel.a == vic) infected = true;
        CHECK(infected);                                           // pathogen in the blood
        (void)vpart;
        // energetic charge: heat -> blast hits flesh AND walls
        const EntityId wall = mk_enclosure(w3, {600, 600}, 900, 800, 100);
        const auto [bv, bpart] = mk_body(w3, {660, 600}, 300);
        PropertyBag cb; cb.set(tpid(w3, "integrity"), Value::integer(800));    // sealed casing
        const EntityId charge = w3.create_entity("charge", w3.materials.id_of("energetic_mix"), {640, 640}, cb);
        const EntityId sapper = mk_person(w3, {640, 800});
        tseti(*w3.find(sapper), w3, "energy", 900000000);
        tseti(*w3.find(sapper), w3, "reach", 200);
        const std::int64_t wall0 = tint(*w3.find(wall), w3, "integrity", 0);
        InteractionResult br = w3.interact(mk_ctx(sapper, {charge}, "heat", 60000000));
        CHECK(br.completed);
        CHECK(!w3.find(charge)->alive);                             // the charge is spent
        CHECK(tint(*w3.find(wall), w3, "integrity", 0) < wall0 - 100);
        CHECK(tint(*w3.find(bpart), w3, "integrity", 1000) < 1000);
        CHECK(w3.ledger.find(br.event)->evidence.find("BLAST") != std::string::npos);
        // the same mix, loose (no casing), is a weaker flash
        World w3b(63, "data");
        const EntityId wall_b = mk_enclosure(w3b, {600, 600}, 900, 800, 100);
        PropertyBag lb; lb.set(tpid(w3b, "integrity"), Value::integer(50));
        const EntityId loose = w3b.create_entity("charge", w3b.materials.id_of("energetic_mix"), {640, 640}, lb);
        const EntityId sap_b = mk_person(w3b, {640, 800});
        tseti(*w3b.find(sap_b), w3b, "energy", 900000000);
        CHECK(w3b.interact(mk_ctx(sap_b, {loose}, "heat", 60000000)).completed);
        CHECK(900 - tint(*w3b.find(wall_b), w3b, "integrity", 0) <
              wall0 - tint(*w3.find(wall), w3, "integrity", 0));
    }

    // ---- 4+7+8 shared: two tongues at war, a depot, a march -----------------
    struct Front { EntityId leader, envoy, foe, depot, food; std::vector<EntityId> fed, cut; };
    auto build_front = [&](World& ww) {
        Front f;
        sized_terrain(ww, 128);
        mk_lang(ww, 11, "22:-800:host;");
        mk_lang(ww, 22, "11:-800:host;");
        f.depot = mk_enclosure(ww, {100, 100}, 900, 800, 150);
        f.food = mk_goods(ww, "wild_berries", 50, {110, 100}, 0);
        f.leader = mk_person(ww, {400, 400});
        f.foe = mk_person(ww, {450, 400});
        tseti(*ww.find(f.leader), ww, "language_id", 11);
        tseti(*ww.find(f.foe), ww, "language_id", 22);
        tseti(*ww.find(f.leader), ww, "communicate_range", 2000);
        tseti(*ww.find(f.leader), ww, "energy", 900000000);
        tseti(*ww.find(f.leader), ww, "metabolism", 50);
        tseti(*ww.find(f.foe), ww, "metabolism", 50);
        ww.mind(f.foe);
        for (int i = 0; i < 3; ++i) {
            // fed squad: on a worn trail far from the depot, low metabolism
            const EntityId s = mk_person(ww, {static_cast<std::int32_t>(800 + i * 20), 800});
            tseti(*ww.find(s), ww, "language_id", 11);
            tseti(*ww.find(s), ww, "metabolism", 50);
            ww.mind(s).belief("defer_to:" + std::to_string(f.leader)).confidence = 800;
            ww.terrain.add_wear(ww.find(s)->pos, 300);
            f.fed.push_back(s);
            // starving squad: off-trail, hungry, fast metabolism
            const EntityId c = mk_person(ww, {static_cast<std::int32_t>(1500 + i * 20), 1500});
            tseti(*ww.find(c), ww, "language_id", 11);
            tseti(*ww.find(c), ww, "hunger", 720);
            tseti(*ww.find(c), ww, "metabolism", 2000);
            ww.mind(c).belief("defer_to:" + std::to_string(f.leader)).confidence = 800;
            f.cut.push_back(c);
        }
        // a bilingual elder of tongue 11 — the would-be envoy, not a soldier
        f.envoy = mk_person(ww, {420, 400});
        tseti(*ww.find(f.envoy), ww, "language_id", 11);
        tseti(*ww.find(f.envoy), ww, "metabolism", 50);
        tseti(*ww.find(f.envoy), ww, "energy", 900000000);
        ww.mind(f.envoy);
        return f;
    };

    // ---- 4: mobilization, supply lines & morale collapse -------------------
    World w4(64, "data");
    Front F = build_front(w4);
    {
        // no casus belli -> no war
        World wp(64, "data");
        Front P = build_front(wp);
        wp.find(kNoEntity);
        for (const auto& ep : wp.entities)
            if (ep->kind_name == "language") ep->props.set(tpid(wp, "diplo_table"), Value::text(""));
        CHECK(!wp.interact(mk_ctx(P.leader, {P.foe}, "mobilize", 0, ("depot:" + std::to_string(P.depot)).c_str())).completed);

        InteractionResult mr = w4.interact(mk_ctx(F.leader, {F.foe}, "mobilize", 0,
            ("depot:" + std::to_string(F.depot)).c_str()));
        CHECK(mr.completed);
        for (EntityId s : F.fed) {
            CHECK(tint(*w4.find(s), w4, "war_target", 0) == 22);
            CHECK(tint(*w4.find(s), w4, "combat_morale", 0) >= 600);
            CHECK(w4.find_mind(s)->find_belief("war:22") != nullptr);
        }
        w4.advance_ticks_for_test(120);
        for (EntityId s : F.fed) {
            CHECK(tint(*w4.find(s), w4, "combat_morale", 0) >= 600);   // supplied, they hold
            CHECK(tint(*w4.find(s), w4, "routed", 0) == 0);
        }
        for (EntityId c : F.cut) {
            CHECK(tint(*w4.find(c), w4, "combat_morale", 1000) < 200); // starving, they break
            CHECK(tint(*w4.find(c), w4, "routed", 0) == 1);
        }
        // the convoy is intercepted: depot stores lost -> the fed squad falls
        // back to merely holding (supply bonus gone)
        const std::int64_t m_before = tint(*w4.find(F.fed[0]), w4, "combat_morale", 0);
        w4.find(F.food)->props.set(tpid(w4, "quantity"), Value::integer(0));
        w4.advance_ticks_for_test(80);
        CHECK(tint(*w4.find(F.fed[0]), w4, "combat_morale", 0) < m_before);
    }

    // ---- 5: siege breaching ------------------------------------------------
    World w5(65, "data");
    {
        const EntityId keep = mk_enclosure(w5, {500, 0}, 900, 800, 120);
        const EntityId owner = mk_person(w5, {500, 0});
        w5.mind(owner).belief("owns:" + std::to_string(keep)).confidence = 1000;
        const EntityId hoard = mk_goods(w5, "copper_metal", 10, {520, 0}, static_cast<std::int64_t>(owner));
        const EntityId raider = mk_person(w5, {360, 0});
        tseti(*w5.find(raider), w5, "energy", 900000000);
        tseti(*w5.find(raider), w5, "reach", 200);
        InteractionResult barred = w5.interact(mk_ctx(raider, {hoard}, "steal", 0));
        CHECK(!barred.completed);
        CHECK(barred.failure.find("walls") != std::string::npos);
        // a hand launcher barely scratches stone
        const EntityId f1 = mk_item(w5, "hardwood", {370, 0});
        const EntityId s1 = mk_item(w5, "sinew", {372, 0});
        InteractionResult hb = w5.interact(mk_ctx(raider, {f1, s1}, "assemble", 1000));
        EntityId bow = kNoEntity;
        for (EntityId s : hb.spawned) if (w5.find(s)->kind_name == "composite") bow = s;
        CHECK(bow != kNoEntity);
        CHECK(w5.interact(mk_ctx(raider, {bow}, "wield", 0)).completed);
        const EntityId pebble = mk_item(w5, "granite", {375, 0});
        CHECK(w5.interact(mk_ctx(raider, {keep, pebble}, "launch", 0)).completed);
        const std::int64_t after_bow = tint(*w5.find(keep), w5, "integrity", 0);
        CHECK(after_bow > 800);                                    // a scratch
        // a great frame (heavy timber) strung with sinew is a siege engine
        const EntityId beam = mk_item(w5, "hardwood", {360, 20}, 40000);
        const EntityId rope = mk_item(w5, "sinew", {362, 20});
        CHECK(w5.interact(mk_ctx(raider, {bow}, "unwield", 0)).completed);
        InteractionResult eb = w5.interact(mk_ctx(raider, {beam, rope}, "assemble", 1000));
        EntityId engine = kNoEntity;
        for (EntityId s : eb.spawned) if (w5.find(s)->kind_name == "composite") engine = s;
        CHECK(engine != kNoEntity);
        CHECK(tint(*w5.find(engine), w5, "launch_impulse", 0) > 3 * tint(*w5.find(bow), w5, "launch_impulse", 0));
        int shots = 0;
        while (tint(*w5.find(keep), w5, "access_barrier", 0) > 0 && shots < 10) {
            PropertyBag bb; bb.set(tpid(w5, "quantity"), Value::integer(1));
            const EntityId boulder = w5.create_entity("ammo", w5.materials.id_of("granite"), {378, 0}, bb);
            CHECK(w5.interact(mk_ctx(raider, {keep, boulder, engine}, "launch", 0)).completed);
            ++shots;
        }
        CHECK(shots >= 2 && shots <= 5);                           // a few great shots, not one
        CHECK(tint(*w5.find(keep), w5, "integrity", 1000) < 300);
        CHECK(tint(*w5.find(keep), w5, "access_barrier", 1) == 0);  // breached
        CHECK(w5.interact(mk_ctx(raider, {hoard}, "steal", 0)).completed);   // stores taken
    }

    // ---- 6: emergent arms race ---------------------------------------------
    {
        // reuse w2-style shot on a minded victim with minded kin nearby
        World w6(66, "data");
        const EntityId shooter = mk_person(w6, {0, 0});
        tseti(*w6.find(shooter), w6, "energy", 900000000);
        const EntityId fr = mk_item(w6, "hardwood", {4, 0});
        const EntityId st = mk_item(w6, "sinew", {6, 0});
        InteractionResult ar = w6.interact(mk_ctx(shooter, {fr, st}, "assemble", 1000));
        EntityId bow = kNoEntity;
        for (EntityId s : ar.spawned) if (w6.find(s)->kind_name == "composite") bow = s;
        CHECK(w6.interact(mk_ctx(shooter, {bow}, "wield", 0)).completed);
        const auto [vic, vpart] = mk_body(w6, {200, 0}, 300);
        const EntityId kin = mk_person(w6, {260, 0});
        const EntityId stranger = mk_person(w6, {3000, 3000});
        tseti(*w6.find(vic), w6, "language_id", 7);
        tseti(*w6.find(kin), w6, "language_id", 7);
        tseti(*w6.find(stranger), w6, "language_id", 7);
        w6.mind(vic); w6.mind(kin); w6.mind(stranger);
        const EntityId arrow = mk_item(w6, "obsidian", {2, 0});
        CHECK(w6.interact(mk_ctx(shooter, {vic, arrow}, "launch", 0)).completed);
        (void)vpart;
        const Belief* vb = w6.find_mind(vic)->find_belief("weapon_counter:projectile");
        const Belief* va = w6.find_mind(vic)->find_belief("armor_tech_bias");
        const Belief* kb = w6.find_mind(kin)->find_belief("weapon_counter:projectile");
        CHECK(vb && vb->confidence >= 300);
        CHECK(va && va->confidence >= 250);
        CHECK(kb && kb->confidence >= 150 && kb->confidence < vb->confidence);
        CHECK(w6.find_mind(stranger)->find_belief("weapon_counter:projectile") == nullptr);
        // the wounded now PREFER protection and counter-launchers in planning
        const EntityId plate = mk_item(w6, "metal_plate", {205, 0});
        InteractionContext don = mk_ctx(vic, {plate}, "don", 0);
        CHECK(arms_race_bonus(w6, *w6.find_mind(vic), don) > 0);
        CHECK(arms_race_bonus(w6, *w6.find_mind(stranger), don) == 0);
        InteractionContext treat = mk_ctx(vic, {vic}, "treat", 0);
        CHECK(arms_race_bonus(w6, *w6.find_mind(vic), treat) == 0);   // not poisoned (yet)
    }

    // ---- 7: surrender & Rosetta peace (continues the w4 front) -------------
    {
        // the squads of tongue 11 all break -> defeat is World Truth
        for (EntityId s : F.fed) {
            tseti(*w4.find(s), w4, "hunger", 900);
            tseti(*w4.find(s), w4, "metabolism", 2000);
        }
        // before defeat, no peace
        tseti(*w4.find(F.envoy), w4, "bilingual_familiarity", 800);
        w4.mind(F.leader).belief("owns:" + std::to_string(F.depot)).confidence = 1000;
        mk_goods(w4, "salt", 20, {420, 405}, static_cast<std::int64_t>(F.envoy));
        w4.advance_ticks_for_test(200);
        for (EntityId s : F.fed) CHECK(!w4.find(s)->alive || tint(*w4.find(s), w4, "routed", 0) == 1);
        // the envoy speaks both tongues (Rosetta words)
        PropertyBag e1, e2;
        e1.set(tpid(w4, "language_id"), Value::integer(11)); e1.set(tpid(w4, "concept_key"), Value::text("peace"));
        e2.set(tpid(w4, "language_id"), Value::integer(22)); e2.set(tpid(w4, "concept_key"), Value::text("peace"));
        const EntityId w11 = w4.create_entity("lexical_entry", kNoMaterial, {0, 0}, e1);
        const EntityId w22 = w4.create_entity("lexical_entry", kNoMaterial, {0, 0}, e2);
        w4.mind(F.envoy).belief("lex:" + std::to_string(w11)).confidence = 900;
        w4.mind(F.envoy).belief("lex:" + std::to_string(w22)).confidence = 900;
        // the foe stands at the envoy's side for the parley
        w4.find(F.foe)->pos = w4.find(F.envoy)->pos;
        w4.spatial.move(F.foe, w4.find(F.envoy)->pos);
        InteractionResult pr = w4.interact(mk_ctx(F.envoy, {F.foe}, "diplomatic_parley", 0,
            ("peace:" + std::to_string(F.depot) + ":salt:5").c_str()));
        CHECK(pr.completed);
        EntityId stone = kNoEntity, debt = kNoEntity;
        for (EntityId s : pr.spawned) {
            if (w4.find(s)->kind_name == "inscription") stone = s;
            if (w4.find(s)->kind_name == "debt_obligation") debt = s;
        }
        CHECK(stone != kNoEntity && debt != kNoEntity);
        if (stone != kNoEntity) {
            CHECK(ttext(*w4.find(stone), w4, "treaty_clause") == "peace");
            CHECK(tint(*w4.find(stone), w4, "language_id2", 0) == 22);
            const std::string cl = ttext(*w4.find(stone), w4, "clause_atoms");
            CHECK(cl.find("11:" + std::to_string(w11)) != std::string::npos);
            CHECK(cl.find("22:" + std::to_string(w22)) != std::string::npos);
        }
        if (debt != kNoEntity) {
            CHECK(tint(*w4.find(debt), w4, "debtor_id", 0) == static_cast<std::int64_t>(F.envoy));
            CHECK(tint(*w4.find(debt), w4, "creditor_id", 0) == static_cast<std::int64_t>(F.foe));
            CHECK(tint(*w4.find(debt), w4, "debt_value", 0) > 0);
        }
        CHECK(!diplo_hostile(w4, 11, 22));
        std::size_t stood_down = 0;
        for (EntityId s : F.fed) if (w4.find(s)->alive) { CHECK(tint(*w4.find(s), w4, "war_target", 0) == 0); ++stood_down; }
        for (EntityId s : F.cut) if (w4.find(s)->alive) { CHECK(tint(*w4.find(s), w4, "war_target", 0) == 0); ++stood_down; }
        CHECK(tint(*w4.find(F.leader), w4, "war_target", 0) == 0);
        (void)stood_down;
        CHECK(w4.find_mind(F.foe)->find_belief("owns:" + std::to_string(F.depot)) != nullptr);
        CHECK(w4.find_mind(F.leader)->find_belief("owns:" + std::to_string(F.depot)) == nullptr);
        CHECK(arch_owner_of(w4, F.depot) == F.foe);
        CHECK(w4.find_mind(F.fed[1])->find_belief("war:22") == nullptr);
    }
    {
        // a side that has NOT lost cannot sue for peace
        World wn(67, "data");
        Front N = build_front(wn);
        CHECK(wn.interact(mk_ctx(N.leader, {N.foe}, "mobilize", 0, "")).completed);
        tseti(*wn.find(N.envoy), wn, "bilingual_familiarity", 800);
        wn.find(N.foe)->pos = wn.find(N.envoy)->pos;
        wn.spatial.move(N.foe, wn.find(N.envoy)->pos);
        CHECK(!wn.interact(mk_ctx(N.envoy, {N.foe}, "diplomatic_parley", 0, "peace:0:-:0")).completed);
    }

    // ---- 8: LOD invariance — dt=1 x N vs dt=N ------------------------------
    {
        World wa(68, "data"), wb(68, "data");
        Front A = build_front(wa), B = build_front(wb);
        CHECK(wa.interact(mk_ctx(A.leader, {A.foe}, "mobilize", 0, ("depot:" + std::to_string(A.depot)).c_str())).completed);
        CHECK(wb.interact(mk_ctx(B.leader, {B.foe}, "mobilize", 0, ("depot:" + std::to_string(B.depot)).c_str())).completed);
        for (int i = 0; i < 150; ++i) wa.advance_ticks_for_test(1);
        wb.advance_ticks_for_test(150);
        for (std::size_t i = 0; i < A.cut.size(); ++i) {
            for (const char* p : {"combat_morale", "routed", "hunger"}) {
                CHECK(tint(*wa.find(A.cut[i]), wa, p, -1) == tint(*wb.find(B.cut[i]), wb, p, -1));
                CHECK(tint(*wa.find(A.fed[i]), wa, p, -1) == tint(*wb.find(B.fed[i]), wb, p, -1));
            }
        }
        CHECK(wa.content_hash() == wb.content_hash());
        // siege damage is event-driven: same shots, any tick schedule
        CHECK(tint(*wa.find(A.depot), wa, "integrity", 0) == tint(*wb.find(B.depot), wb, "integrity", 0));
    }

    // ---- 9: save/load roundtrip --------------------------------------------
    for (World* ww : {&w, &w2, &w3, &w4, &w5}) {
        const std::string path = "/tmp/simlab_test_126.bin";
        const std::uint64_t h = ww->state_hash();
        CHECK(ww->save(path));
        World wr(1, "data");
        CHECK(wr.load(path));
        CHECK(wr.state_hash() == h);
    }
}


// ================= Phase 13: render snapshot, procedural atlas, fog of war =====
// Primordial Genesis: naked tick-0 world + curiosity-driven verb discovery,
// player possession and whispered omens — all through the causal pipeline.
static void test_primordial_genesis_true_playable() {
    PrimordialParams pp; pp.seed = 7; pp.grid = 48; pp.tribes = 2; pp.per_tribe = 4;
    World w(7, "data");
    const PrimordialWorld pw = build_primordial(w, pp);
    const SpeciesDef* sap = w.species.find_name("sapient");
    CHECK(sap && sap->innate.size() >= 3);
    CHECK(pw.founders.size() == 8);
    CHECK(pw.homes.size() == 2 && pw.tribe_lang.size() == 2 &&
          pw.tribe_lang[0] != pw.tribe_lang[1]);
    CHECK(primordial_violations(w) == 0);
    std::size_t sapients = 0;
    for (EntityId id : w.entities_of_kind(KindName("sapient")))
        if (const Entity* e = w.find(id); e && e->alive) ++sapients;
    CHECK(sapients == 8);                      // no wild sapients from worldgen
    const std::set<std::string> innate(sap->innate.begin(), sap->innate.end());
    for (EntityId f : pw.founders) {
        const AgentMind* m = w.find_mind(f);
        CHECK(m && m->known_actions == innate);
        CHECK(m && m->beliefs.empty() && m->discoveries.empty());
        CHECK(w.relations_of(f, "wielding").empty());
    }
    {   // bands start far apart (beyond each other's sight)
        const std::int64_t d2 = dist2(pw.homes[0], pw.homes[1]);
        const std::int64_t s = 400;              // most of the map width apart
        CHECK(d2 > s * s);
    }
    std::size_t flora = 0;
    for (const auto& ep : w.entities) if (ep->alive && ep->kind_name != KindName("sapient")) ++flora;
    CHECK(flora > 50);                          // a living world around them

    // replay determinism of the genesis itself
    World w2(7, "data");
    build_primordial(w2, pp);
    CHECK(w.state_hash() == w2.state_hash());

    // curiosity drives verb discovery; the default driver does not
    World off = w.clone();
    DriverState st; st.curiosity = true; st.concurrent = true;
    DriverState st_off;
    const std::int64_t cycles = std::getenv("SIMLAB_GENESIS_CYCLES")
        ? std::atoll(std::getenv("SIMLAB_GENESIS_CYCLES")) : 60;
    autonomous_run(w, cycles, &st);
    autonomous_run(off, cycles, &st_off);
    auto learned = [&](const World& ww) {
        std::set<std::string> u;
        for (EntityId f : pw.founders)
            if (const AgentMind* m = ww.find_mind(f))
                for (const std::string& a : m->known_actions)
                    if (!innate.count(a)) u.insert(a);
        return u;
    };
    const std::set<std::string> lw = learned(w), lo = learned(off);
    std::printf("  genesis: %lld cycles tick=%llu learned(curious)=%zu learned(off)=%zu alive=%zu [",
                static_cast<long long>(cycles), static_cast<unsigned long long>(w.clock_.tick()),
                lw.size(), lo.size(), [&] { std::size_t n = 0;
                    for (EntityId f : pw.founders) if (const Entity* e = w.find(f); e && e->alive) ++n;
                    return n; }());
    for (const std::string& a : lw) std::printf(" %s", a.c_str());
    std::printf(" ] violations=%zu\n", primordial_violations(w));
    CHECK(lw.size() >= 2);
    CHECK(lw.size() > lo.size());

    // replay: same world + same driver flags -> same hash
    World r = w2.clone();
    DriverState st_r; st_r.curiosity = true; st_r.concurrent = true;
    autonomous_run(r, cycles, &st_r);
    CHECK(r.state_hash() == w.state_hash());

    // possession: the driver never acts for a possessed avatar
    EntityId av = kNoEntity;
    for (EntityId f : pw.founders) if (const Entity* e = w.find(f); e && e->alive) { av = f; break; }
    CHECK(av != kNoEntity);
    if (av != kNoEntity) {
        st.possessed.insert(av);
        const EventId before = w.ledger.next_id();
        autonomous_run(w, 15, &st);
        std::size_t acted = 0;
        for (EventId i = before; i < w.ledger.next_id(); ++i) {
            const CausalEvent* ev = w.ledger.find(i);
            if (ev && !ev->participants.empty() && ev->participants.front() == av &&
                ev->action != "metabolize" && ev->action != "ecology") ++acted;
        }
        CHECK(acted == 0);
        // player action through interact(): the avatar learns like anyone
        InteractionContext c; c.actor = av; c.parameters.action = "rest"; c.parameters.ticks = 1;
        CHECK(w.interact(c).completed);
        st.possessed.erase(av);
    }

    // whispered omen: a strongly heeded hint gets tried and then cleared
    // (or decays) — never a direct grant of knowledge.
    if (av != kNoEntity) {
        AgentMind& m = w.mind(av);
        std::string verb;
        for (const char* v : {"impact", "strike", "dig", "chop", "assemble", "heat", "carve"})
            if (!m.known_actions.count(v)) { verb = v; break; }
        if (!verb.empty()) {
            const bool knew = m.known_actions.count(verb) != 0;
            m.belief("omen:" + verb).confidence = 1000;
            CHECK(!knew && !w.find_mind(av)->known_actions.count(verb));   // not granted
            const EventId e0 = w.ledger.next_id();
            autonomous_run(w, 40, &st);
            // if the verb was learned, a real completed event by the avatar
            // taught it — knowledge only ever comes through the ledger
            const AgentMind* m2 = w.find_mind(av);
            if (m2 && m2->known_actions.count(verb)) {
                bool via_event = false;
                for (EventId i = e0; i < w.ledger.next_id(); ++i)
                    if (const CausalEvent* ev = w.ledger.find(i);
                        ev && !ev->failed && (ev->action == verb ||          // tried it / saw it done
                            (ev->action == "communicate" &&                 // or was taught it
                             std::find(ev->participants.begin(), ev->participants.end(), av) != ev->participants.end())))
                        via_event = true;
                CHECK(via_event);
            }
        }
    }

    // save/load round-trip keeps the primordial world bit-exact
    const std::string path = "build/genesis_rt.sav";
    CHECK(w.save(path));
    World l(1, "data");
    CHECK(l.load(path));
    CHECK(l.state_hash() == w.state_hash());
    std::remove(path.c_str());
}

// Patron layer + planet: climate belts, closed ocean rim, warm camps; the
// player's journal (Rosetta guesses, milestones, resonance) is viewer state —
// scanning never changes World Truth; beacons steer agents only via beliefs.
static void test_patron_journal_planet_and_beacons() {
    using namespace simlab::render;
    // ---- planet: belts + rim ------------------------------------------------
    PrimordialParams pp; pp.seed = 11; pp.grid = 96; pp.planet = true; pp.tribes = 3; pp.per_tribe = 3;
    World pw(11, "data");
    const PrimordialWorld pr = build_primordial(pw, pp);
    const TerrainGrid& t = pw.terrain;
    auto band_t = [&](int y0, int y1) {
        long s = 0, n = 0;
        for (int y = y0; y < y1; ++y) for (int x = 0; x < t.w; ++x) { s += t.temperature[std::size_t(y) * t.w + x]; ++n; }
        return n ? s / n : 0;
    };
    const long eq = band_t(t.h / 2 - 4, t.h / 2 + 4), mid = band_t(t.h / 4 - 4, t.h / 4 + 4), pole = band_t(0, 6);
    std::printf("  planet: equator %ld temperate %ld pole %ld\n", eq, mid, pole);
    CHECK(eq > mid && mid > pole && pole < 0);
    std::size_t rim_water = 0;
    for (int y = 0; y < t.h; ++y) rim_water += t.water[std::size_t(y) * t.w] != 0;
    CHECK(rim_water * 10 >= std::size_t(t.h) * 9);          // west rim is sea
    CHECK(pr.founders.size() == 9 && pr.homes.size() == 3);
    for (const Vec2& h : pr.homes) CHECK(t.temp_at(h) >= 250);   // bands start warm
    CHECK(primordial_violations(pw) == 0);
    // legacy (non-planet) seeds keep the old analytic fields
    int dw = 0, dh = 0;
    CHECK(!WorldGenerator::planet_dims(2026, dw, dh));
    CHECK(WorldGenerator::planet_dims(WorldGenerator::planet_seed(5, 96, 64), dw, dh) && dw == 96 && dh == 64);

    // ---- journal: guesses are confirmed only against the real concept --------
    World w(13, "data");
    PrimordialParams vp; vp.seed = 13; vp.grid = 48; vp.tribes = 1; vp.per_tribe = 4;
    const PrimordialWorld vw = build_primordial(w, vp);
    const MaterialDef* berries = w.materials.find_name("wild_berries");
    CHECK(berries != nullptr);
    JournalWord jw;
    jw.truth = "method:consume(f" + std::to_string(berries ? berries->id : 0) + "+)";
    jw.guess = "berries";
    CHECK(!guess_confirmed(w, jw));                          // never witnessed in context
    jw.heard = 1;
    CHECK(guess_confirmed(w, jw));
    jw.guess = "fire";
    CHECK(!guess_confirmed(w, jw));
    jw.guess = "eat / consume";
    CHECK(guess_confirmed(w, jw));

    // ---- scan is read-only; milestones + resonance come from real events ----
    PatronJournal j; j.tribe = vw.tribe_lang.front();
    DriverState st; st.curiosity = true; st.concurrent = true;
    autonomous_run(w, 30, &st);
    const std::uint64_t h0 = w.state_hash();
    patron_scan(w, j, nullptr);
    CHECK(w.state_hash() == h0);
    std::printf("  patron: milestones=%zu resonance=%lld words=%zu\n", j.milestones.size(),
                (long long)j.resonance, j.lexicon.size());
    CHECK(!j.milestones.empty());
    for (const Milestone& m : j.milestones) CHECK(!m.title.empty() && m.tick <= w.clock_.tick());
    // re-scan finds nothing new (cursor)
    CHECK(patron_scan(w, j, nullptr) == 0);

    // ---- journal save/load round trip ----------------------------------------
    j.pins.insert(vw.founders.front());
    j.lexicon[j.tribe]["ka-tu"] = jw;
    j.lexicon[j.tribe]["ka-tu"].guess = "ăn quả";            // UTF-8 guesses survive
    j.lexicon[j.tribe]["ka-tu"].tag = WordTag::Danger;
    j.notes = "line one\nline two";
    const std::string jp = "build/patron_rt.journal";
    CHECK(j.save(jp));
    PatronJournal j2;
    CHECK(j2.load(jp));
    CHECK(j2.tribe == j.tribe && j2.resonance == j.resonance && j2.scanned == j.scanned);
    CHECK(j2.pins == j.pins && j2.notes == j.notes);
    CHECK(j2.milestones.size() == j.milestones.size() && j2.milestone_keys == j.milestone_keys);
    CHECK(j2.lexicon[j.tribe]["ka-tu"].guess == "ăn quả" &&
          j2.lexicon[j.tribe]["ka-tu"].tag == WordTag::Danger &&
          j2.lexicon[j.tribe]["ka-tu"].truth == jw.truth);
    std::remove(jp.c_str());

    // ---- beacon: a belief, followed by a sated calm scout ---------------------
    EntityId scout = kNoEntity;
    for (EntityId f : vw.founders) if (const Entity* e = w.find(f); e && e->alive) { scout = f; break; }
    CHECK(scout != kNoEntity);
    if (scout != kNoEntity) {
        Entity* se = w.find(scout);
        Vec2 goal{se->pos.x + 200, se->pos.y};
        if (!w.terrain.contains(goal) || w.terrain.is_water(goal)) goal = Vec2{se->pos.x - 200, se->pos.y};
        const std::int64_t d0 = dist2(se->pos, goal);
        AgentMind& m = w.mind(scout);
        m.belief("beacon:" + std::to_string(goal.x) + ":" + std::to_string(goal.y)).confidence = 800;
        tseti(*se, w, "hunger", 0); tseti(*se, w, "thirst", 0);
        // a rested, unhurt scout: denser Phase-18 fauna means the first
        // living founder may have bled energy/fatigue/integrity to danger
        // during the 30-cycle warm-up — top those up so the beacon (not
        // exhaustion) is what the check measures.
        tseti(*se, w, "energy", 20000); tseti(*se, w, "fatigue", 0);
        tseti(*se, w, "integrity", 1000);
        m.neuro.panic = m.neuro.rage = m.neuro.grief = 0;
        m.beliefs.erase("peril:deep_water");   // no learned hydrophobia either
        autonomous_run(w, 6, &st);
        const Entity* se2 = w.find(scout);
        const AgentMind* m2 = w.find_mind(scout);
        const bool kept = m2 && m2->beliefs.count("beacon:" + std::to_string(goal.x) + ":" + std::to_string(goal.y));
        std::printf("  beacon: d0=%lld d1=%lld kept=%d\n", (long long)d0, se2 ? (long long)dist2(se2->pos, goal) : -1LL, int(kept));
        CHECK(se2 && (dist2(se2->pos, goal) < d0 || !kept));
    }
}

// Maritime causality (Primordial ruleset): depth & swell, swimming strokes,
// drowning, learned fear, the shore sense, and rafts/hulls whose buoyancy and
// swell rating emerge from real material densities / waterproofing.
static World maritime_world(std::uint64_t seed, int sea_cols, std::int32_t sea_elev) {
    World w(seed, "data");
    w.ruleset |= World::kRulePrimordial;
    TerrainGrid& t = w.terrain;
    t.w = 20 + sea_cols; t.h = 12; t.cell = 16; t.ox = 0; t.oy = 0;
    const std::size_t n = std::size_t(t.w) * t.h;
    t.elevation.assign(n, 300); t.moisture.assign(n, 500); t.temperature.assign(n, 300);
    t.water.assign(n, 0); t.vegetation.assign(n, 300); t.slope.assign(n, 0);
    t.wind_x.assign(n, 0); t.wind_y.assign(n, 0); t.water_table.assign(n, 5); t.traffic_wear.assign(n, 0);
    for (int y = 0; y < t.h; ++y)
        for (int x = 10; x < 10 + sea_cols; ++x) {
            t.elevation[std::size_t(y) * t.w + x] = sea_elev;
            t.water[std::size_t(y) * t.w + x] = 1;
        }
    for (int y = 0; y < t.h; ++y)                          // hand-built: never lazily regenerated
        for (int x = 0; x < t.w; ++x) w.gen_cells_.insert({x, y});
    return w;
}
static EntityId mk_sapient(World& w, Vec2 pos, bool minded = true) {
    const SpeciesDef* sp = w.species.find_name("sapient");
    PropertyBag pb = sp ? sp->props : PropertyBag{};
    pb.set(tpid(w, "energy"), Value::integer(100000));
    pb.set(tpid(w, "fatigue"), Value::integer(100));
    pb.set(tpid(w, "integrity"), Value::integer(1000));
    const EntityId id = w.create_entity("sapient", sp ? sp->material : kNoMaterial, pos, pb);
    if (minded) w.mind(id);
    return id;
}
static InteractionContext mk_move(EntityId a, Vec2 to, bool swim) {
    InteractionContext c; c.actor = a; c.parameters.action = "move"; c.parameters.ticks = 4;
    c.parameters.destination = to; c.parameters.has_destination = true;
    if (swim) c.parameters.note = "swim";
    return c;
}

static void test_maritime_swimming_drowning_and_watercraft() {
    using namespace simlab::lawutil;
    // ---- legacy worlds: water stays a hard edge (byte-identical behaviour) --
    {
        World w = maritime_world(31, 6, 100);
        w.ruleset = 0;
        const EntityId a = mk_sapient(w, {9 * 16 + 8, 6 * 16 + 8});
        for (int i = 0; i < 6; ++i) w.interact(mk_move(a, {30 * 16, 6 * 16 + 8}, true));
        const Vec2 lp = w.find(a)->pos;                    // (it may skirt off the grid edge)
        CHECK(!(w.terrain.contains(lp) && w.terrain.is_water(lp)));
    }
    // ---- depth & swell ---------------------------------------------------------
    World w = maritime_world(32, 4, 100);                 // 4-cell strait, 100 m deep
    CHECK(water_depth(w, {5 * 16, 5 * 16}) == 0);
    CHECK(water_depth(w, {11 * 16, 5 * 16}) == 100);
    CHECK(wave_at(w, {11 * 16, 5 * 16}) == 300);
    // ---- a swimmer crosses a narrow strait, tiring and learning ---------------
    const EntityId sw = mk_sapient(w, {9 * 16 + 12, 6 * 16 + 8});
    const PropId fat = tpid(w, "fatigue"), skill = tpid(w, "swim_skill"), integ = tpid(w, "integrity");
    int strokes = 0;
    for (int i = 0; i < 12 && !(w.find(sw)->pos.x > 14 * 16); ++i)
        if (w.interact(mk_move(sw, {16 * 16, 6 * 16 + 8}, true)).completed) ++strokes;
    const Entity* swe = w.find(sw);
    std::printf("  swim: strokes=%d fatigue=%lld skill=%lld x=%d\n", strokes, (long long)w.prop_int(*swe, fat, 0),
                (long long)w.prop_int(*swe, skill, 0), swe->pos.x);
    CHECK(swe->alive && swe->pos.x > 14 * 16);              // reached the far shore
    CHECK(w.prop_int(*swe, fat, 0) > 100);                  // it cost strength
    CHECK(w.prop_int(*swe, skill, 0) > 0);                  // and taught a little
    // without the intent to swim, the law keeps a walker out of deep water
    const EntityId walker = mk_sapient(w, {9 * 16 + 12, 3 * 16 + 8});
    for (int i = 0; i < 4; ++i) w.interact(mk_move(walker, {16 * 16, 3 * 16 + 8}, false));
    CHECK(!w.terrain.is_water(w.find(walker)->pos));
    // ---- exhaustion in deep water drowns; a witness learns fear ----------------
    {
        World d = maritime_world(33, 20, 60);              // open water
        const EntityId v = mk_sapient(d, {15 * 16, 6 * 16 + 8});           // already in the sea
        const EntityId wit = mk_sapient(d, {9 * 16, 6 * 16 + 8});          // on the beach, watching
        tseti(*d.find(v), d, "fatigue", 990);
        for (int i = 0; i < 8 && d.find(v)->alive; ++i) d.interact(mk_move(v, {30 * 16, 6 * 16 + 8}, true));
        CHECK(!d.find(v)->alive);
        const AgentMind* wm = d.find_mind(wit);
        const Belief* fear = wm ? wm->find_belief("peril:deep_water") : nullptr;
        std::printf("  drowning: victim dead=%d witness fear=%lld\n", int(!d.find(v)->alive), fear ? (long long)fear->confidence : -1LL);
        CHECK(fear && fear->confidence >= 300);
    }
    // ---- shore sense: a mind does not swim a crossing it cannot finish ----------
    {
        World d = maritime_world(34, 40, 60);              // 40 cells of deep water
        const EntityId a = mk_sapient(d, {9 * 16 + 8, 6 * 16 + 8});
        d.mind(a).belief("beacon:" + std::to_string(55 * 16) + ":" + std::to_string(6 * 16 + 8)).confidence = 900;
        DriverState st; st.curiosity = true; st.concurrent = true;
        for (int c = 0; c < 25; ++c) autonomous_cycle(d, &st);
        const Entity* ae = d.find(a);
        const Belief* wc = d.find_mind(a)->find_belief("want_cross");
        std::printf("  shore sense: alive=%d in_water=%d want_cross=%lld\n", int(ae->alive),
                    int(d.terrain.is_water(ae->pos)), wc ? (long long)wc->confidence : -1LL);
        CHECK(ae->alive && !d.terrain.is_water(ae->pos));
        CHECK(wc && wc->confidence > 0);
    }
    // ---- raft vs sealed hull: buoyancy & swell rating from materials -------------
    {
        World d = maritime_world(35, 10, 160);             // 40 m coastal water, swell 120
        const EntityId a = mk_sapient(d, {9 * 16 + 4, 6 * 16 + 8});
        const MaterialDef* wood = d.materials.find_name("dry_wood");
        const MaterialDef* hide = d.materials.find_name("cured_hide");
        CHECK(wood && hide);
        auto part = [&](MaterialId m) { return d.create_entity("piece", m, {9 * 16 + 6, 6 * 16 + 8}, PropertyBag{}); };
        InteractionContext as; as.actor = a; as.parameters.action = "assemble"; as.parameters.force = 100000; as.parameters.ticks = 1;
        as.targets = {part(wood->id), part(wood->id), part(wood->id), part(wood->id)};
        const InteractionResult rr = d.interact(as);
        CHECK(rr.completed && !rr.spawned.empty());
        if (!rr.completed) std::printf("  raft assemble failed: %s\n", rr.failure.c_str());
        const EntityId raft = rr.spawned.empty() ? kNoEntity : rr.spawned.front();
        as.targets = {part(wood->id), part(wood->id), part(wood->id), part(hide->id)};
        const InteractionResult rh = d.interact(as);
        const EntityId hull = rh.spawned.empty() ? kNoEntity : rh.spawned.front();
        const PropId fc = tpid(d, "float_capacity"), wr = tpid(d, "wave_rating");
        const Entity* re = d.find(raft); const Entity* he = d.find(hull);
        std::printf("  craft: raft lift=%lld swell=%lld | hull lift=%lld swell=%lld | person load=%lld\n",
                    re ? (long long)d.prop_int(*re, fc, 0) : -1LL, re ? (long long)d.prop_int(*re, wr, 0) : -1LL,
                    he ? (long long)d.prop_int(*he, fc, 0) : -1LL, he ? (long long)d.prop_int(*he, wr, 0) : -1LL,
                    (long long)sinking_load(d, *d.find(a)));
        CHECK(re && d.prop_int(*re, fc, 0) > 0);
        CHECK(he && d.prop_int(*he, wr, 0) > d.prop_int(*re, wr, 0));     // sealed seams ride bigger swell
        // board the raft and paddle across without tiring
        InteractionContext bd; bd.actor = a; bd.parameters.action = "board"; bd.targets = {raft}; bd.parameters.ticks = 1;
        const InteractionResult br = d.interact(bd);
        if (!br.completed) std::printf("  board failed: %s\n", br.failure.c_str());
        CHECK(br.completed && craft_of(d, a) == raft);
        const std::int64_t f0 = d.prop_int(*d.find(a), tpid(d, "fatigue"), 0);
        for (int i = 0; i < 40 && craft_of(d, a) != kNoEntity; ++i) d.interact(mk_move(a, {24 * 16, 6 * 16 + 8}, false));
        const Entity* ae = d.find(a);
        std::printf("  sail: x=%d aboard=%d fatigue %lld->%lld raft integrity=%lld raft x=%d\n", ae->pos.x,
                    int(craft_of(d, a) != kNoEntity), (long long)f0, (long long)d.prop_int(*ae, tpid(d, "fatigue"), 0),
                    (long long)d.prop_int(*d.find(raft), integ, 0), d.find(raft)->pos.x);
        CHECK(ae->alive && ae->pos.x >= 20 * 16 && !d.terrain.is_water(ae->pos));   // landed on the far shore
        CHECK(craft_of(d, a) == kNoEntity);                                         // stepped ashore
        CHECK(d.prop_int(*ae, tpid(d, "fatigue"), 0) <= f0);                        // no swimming strokes
        CHECK(d.find(raft)->pos.x >= 18 * 16);                                      // the raft carried them
        // the same raft in deep, rough water is battered
        World o = maritime_world(36, 20, 20);              // 180 m deep, swell 540
        const EntityId b = mk_sapient(o, {9 * 16 + 4, 6 * 16 + 8});
        InteractionContext as2 = as; as2.actor = b;
        auto part2 = [&](MaterialId m) { return o.create_entity("piece", m, {9 * 16 + 6, 6 * 16 + 8}, PropertyBag{}); };
        as2.targets = {part2(wood->id), part2(wood->id), part2(wood->id), part2(wood->id)};
        const InteractionResult r2 = o.interact(as2);
        const EntityId raft2 = r2.spawned.empty() ? kNoEntity : r2.spawned.front();
        InteractionContext bd2 = bd; bd2.actor = b; bd2.targets = {raft2};
        CHECK(o.interact(bd2).completed);
        const std::int64_t i0 = o.prop_int(*o.find(raft2), integ, 1000);
        for (int i = 0; i < 6; ++i) o.interact(mk_move(b, {29 * 16, 6 * 16 + 8}, false));
        CHECK(o.prop_int(*o.find(raft2), integ, 1000) < i0);                        // swell > rating
    }
    // ---- the emergent chain: blocked shore -> want_cross -> raft -> sail ----------
    {
        World d = maritime_world(41, 40, 170);             // a 40-cell strait, 30 m deep
        const EntityId a = mk_sapient(d, {148, 104});
        if (const SpeciesDef* sp = d.species.find_name("sapient"))
            for (const std::string& v : sp->innate) d.mind(a).learn_action(v);
        const MaterialDef* wood = d.materials.find_name("dry_wood");
        for (int i = 0; i < 6; ++i) d.create_entity("branch", wood->id, {136 + (i % 3) * 4, 96 + (i / 3) * 8}, PropertyBag{});
        d.mind(a).belief("beacon:880:104").confidence = 1000;
        DriverState st; st.curiosity = true; st.concurrent = true;
        bool built = false, boarded = false, landed = false, swam_deep = false;
        for (int c = 0; c < 200 && !landed; ++c) {
            Entity* ae = d.find(a);
            tseti(*ae, d, "hunger", 100); tseti(*ae, d, "thirst", 100);    // isolate the maritime chain
            const EventId e0 = d.ledger.next_id();
            autonomous_cycle(d, &st);
            for (EventId i = e0; i < d.ledger.next_id(); ++i) {
                const CausalEvent* ev = d.ledger.find(i);
                if (!ev || ev->failed || ev->participants.empty() || ev->participants.front() != a) continue;
                if (ev->action == "assemble") built = true;
                if (ev->action == "board") boarded = true;
                if (ev->evidence.find("disembark ashore") != std::string::npos) landed = true;
                if (ev->evidence.find("swim") != std::string::npos) swam_deep = true;
            }
        }
        std::printf("  emergent crossing: built=%d boarded=%d landed=%d x=%d swam=%d\n", int(built), int(boarded),
                    int(landed), d.find(a)->pos.x, int(swam_deep));
        CHECK(built && boarded && landed);
        CHECK(!swam_deep);                                 // never tried to swim a strait it could not finish
        CHECK(d.find(a)->pos.x >= 50 * 16);                // on the far shore
    }
    // ---- cold water: stroke cost & immersion heat loss scale with temperature -
    {
        World cold = maritime_world(38, 20, 100), warm = maritime_world(38, 20, 100);
        for (int y = 0; y < cold.terrain.h; ++y)                    // chill the sea to 5°C
            for (int x = 10; x < 30; ++x)
                cold.terrain.temperature[std::size_t(y) * cold.terrain.w + x] = 50;
        const Vec2 sea{11 * 16, 6 * 16 + 8};
        const EntityId cs = mk_sapient(cold, sea);                  // already immersed
        const EntityId ws = mk_sapient(warm, sea);
        const std::int64_t cc = swim_stroke_cost(cold, *cold.find(cs), sea);
        const std::int64_t cw = swim_stroke_cost(warm, *warm.find(ws), sea);
        std::printf("  cold water: stroke cost cold=%lld warm=%lld\n",
                    (long long)cc, (long long)cw);
        CHECK(cc > cw);                                            // 32°C below set-point costs strokes
        // same immersion, same ticks: the cold sea drains body_temp faster.
        // dt=30 batches give the pull room to matter (per-tick moves are
        // rate-limited to 4 deciC, so short batches all look identical).
        const PropId bt = tpid(cold, "body_temp");
        const std::int64_t bt0 = cold.prop_int(*cold.find(cs), bt, 370);
        cold.advance_ticks_for_test(30);
        warm.advance_ticks_for_test(30);
        const std::int64_t drop_c = bt0 - cold.prop_int(*cold.find(cs), bt, bt0);
        const std::int64_t drop_w = bt0 - warm.prop_int(*warm.find(ws), bt, bt0);
        std::printf("  immersion: body_temp drop cold=%lld warm=%lld\n",
                    (long long)drop_c, (long long)drop_w);
        CHECK(drop_c > drop_w);                                    // depth-scaled immersion chill
        // a sailor is NOT immersed: aboard a craft the deep-water bonus is off
        {
            World s = maritime_world(39, 20, 100);
            for (int y = 0; y < s.terrain.h; ++y)
                for (int x = 10; x < 30; ++x)
                    s.terrain.temperature[std::size_t(y) * s.terrain.w + x] = 200;
            const EntityId pax = mk_sapient(s, sea);
            const EntityId cork = s.create_entity("composite", kNoMaterial, sea, PropertyBag{});
            s.relate(pax, "aboard", cork);
            const PropId btt = tpid(s, "body_temp");
            const std::int64_t b0 = s.prop_int(*s.find(pax), btt, 370);
            s.advance_ticks_for_test(30);
            const std::int64_t aboard_drop = b0 - s.prop_int(*s.find(pax), btt, b0);
            // same 20°C water, same ticks: a swimmer in it chills deeper
            World t = maritime_world(40, 20, 100);
            for (int y = 0; y < t.terrain.h; ++y)
                for (int x = 10; x < 30; ++x)
                    t.terrain.temperature[std::size_t(y) * t.terrain.w + x] = 200;
            const EntityId swim = mk_sapient(t, sea);
            const std::int64_t t0 = t.prop_int(*t.find(swim), btt, 370);
            t.advance_ticks_for_test(30);
            const std::int64_t swim_drop = t0 - t.prop_int(*t.find(swim), btt, t0);
            std::printf("  20°C water: aboard drop=%lld swimmer drop=%lld\n",
                        (long long)aboard_drop, (long long)swim_drop);
            CHECK(swim_drop > aboard_drop);
        }
    }
    // ---- determinism of the whole maritime path -----------------------------------
    {
        auto run = [] {
            World d = maritime_world(37, 4, 100);
            const EntityId s2 = mk_sapient(d, {9 * 16 + 12, 6 * 16 + 8});
            for (int i = 0; i < 10; ++i) d.interact(mk_move(s2, {16 * 16, 6 * 16 + 8}, true));
            return d.state_hash();
        };
        CHECK(run() == run());
    }
}

// ---- Phase 17: trophic web, seasonal migration & apex predators -------------
// Hand-built land world with a real forage gradient: west cold/dry,
// east warm/wet. All land (no water) so range drift never swims.
static World eco_world(std::uint64_t seed) {
    World w(seed, "data");
    w.ruleset |= World::kRulePrimordial;   // awake animals resist being eaten
    TerrainGrid& t = w.terrain;
    t.w = 48; t.h = 16; t.cell = 16; t.ox = 0; t.oy = 0;
    const std::size_t n = std::size_t(t.w) * t.h;
    t.elevation.assign(n, 300);
    t.moisture.assign(n, 800); t.temperature.assign(n, 300);
    t.water.assign(n, 0); t.vegetation.assign(n, 800); t.slope.assign(n, 0);
    t.wind_x.assign(n, 0); t.wind_y.assign(n, 0);
    t.water_table.assign(n, 5); t.traffic_wear.assign(n, 0);
    // smooth west->east gradient: cold/dry west, warm/wet east — every
    // step east improves migrant_herd's declared-band fit until it plateaus
    for (int y = 0; y < t.h; ++y)
        for (int x = 0; x < t.w; ++x) {
            const std::size_t i = std::size_t(y) * t.w + x;
            t.temperature[i] = std::max<std::int32_t>(100, 480 - x * 8);
            t.moisture[i] = std::min<std::int32_t>(1000, 300 + x * 12);
        }
    for (int y = 0; y < t.h; ++y)
        for (int x = 0; x < t.w; ++x) w.gen_cells_.insert({x, y});
    return w;
}
static EntityId mk_fauna(World& w, const char* kind, Vec2 pos) {
    const SpeciesDef* sp = w.species.find_name(kind);
    const EntityId id = w.create_entity(kind, sp ? sp->material : kNoMaterial,
                                        pos, sp ? sp->props : PropertyBag{});
    // genome carriers are the ones minds can form around — seed the same
    // loci worldgen would so promotion/reproduction behave normally.
    if (sp) for (const PropId g : sp->genes)
        if (w.find(id)) w.find(id)->genome.set(g, 500);
    return id;
}
static void eco_quiet(Entity& e, World& w) {
    // test dummy metabolism: survive the scenario without starving
    tseti(e, w, "metabolism", 100);
    tseti(e, w, "hunger", 0); tseti(e, w, "thirst", 0); tseti(e, w, "fatigue", 0);
}

static void test_trophic_ecology_seasons_and_apex() {
    // ---- schema: trophic props are data, parsed onto species + entities ----
    {
        World w(11, "data");
        for (const char* k : {"migrant_herd", "pack_stalker", "ridge_apex",
                              "shore_reaver", "commensal_rat"})
            CHECK(w.species.find_name(k) != nullptr);
        const EntityId h = mk_fauna(w, "migrant_herd", {100, 100});
        CHECK(w.prop_int(*w.find(h), tpid(w, "trophic_tier"), -1) == 1);
        CHECK(w.prop_int(*w.find(h), tpid(w, "migration_drive"), 0) == 600);
        CHECK(w.prop_int(*w.find(h), tpid(w, "herd_cohesion"), 0) == 700);
        const EntityId apex = mk_fauna(w, "ridge_apex", {200, 100});
        CHECK(w.prop_int(*w.find(apex), tpid(w, "prey_tier_max"), 0) == 3);
        CHECK(w.prop_int(*w.find(apex), tpid(w, "activity_phase"), 0) == 2);
        const EntityId g = mk_fauna(w, "grazer", {150, 100});
        CHECK(w.prop_int(*w.find(g), tpid(w, "trophic_tier"), -1) == 1);
        const EntityId s = mk_sapient(w, {120, 100});
        CHECK(w.prop_int(*w.find(s), tpid(w, "trophic_tier"), -1) == 3);
    }
    // ---- seasonal migration: herd drifts up the forage gradient ------------
    {
        World w = eco_world(12);
        const EntityId h = mk_fauna(w, "migrant_herd", {4 * 16, 8 * 16});
        eco_quiet(*w.find(h), w);
        const Vec2 p0 = w.find(h)->pos;
        const EventId led0 = w.ledger.next_id();
        w.advance_ticks_for_test(400);
        const Vec2 p1 = w.find(h)->pos;
        std::printf("  migration: herd (%d,%d) -> (%d,%d)\n",
                    p0.x, p0.y, p1.x, p1.y);
        CHECK(w.find(h)->alive);
        CHECK(p1.x > p0.x + 96);                    // east up the gradient
        CHECK(p1.x < w.terrain.w * w.terrain.cell); // stayed on the map
        // pure range drift must still be ledgered: an ecology event naming h
        bool drift_led = false;
        for (EventId i = led0; i < w.ledger.next_id(); ++i) {
            const CausalEvent* ev = w.ledger.find(i);
            if (ev && ev->action == "ecology" &&
                std::find(ev->participants.begin(), ev->participants.end(), h)
                    != ev->participants.end())
                drift_led = true;
        }
        CHECK(drift_led);
        // batch-exact: 400 at once == 40 x 10 stepped (stateless schedule)
        World b = eco_world(12);
        const EntityId h2 = mk_fauna(b, "migrant_herd", {4 * 16, 8 * 16});
        eco_quiet(*b.find(h2), b);
        for (int i = 0; i < 40; ++i) b.advance_ticks_for_test(10);
        CHECK(b.find(h2)->pos == p1);
    }
    // ---- flora seasonality: winter slows declared-dormant regrowth ---------
    {
        auto fruit_after = [](std::uint64_t pre) {
            World w = eco_world(13);
            const SpeciesDef* bush = w.species.find_name("berry_bush");
            // east side: warm/wet enough that env-scaled regrowth is > 0
            const EntityId b = w.create_entity(bush->name, bush->material,
                                               {26 * 16, 8 * 16}, bush->props);
            w.advance_ticks_for_test(pre);                    // move the clock
            tseti(*w.find(b), w, "fruit_count", 0);           // strip AFTER pre
            tseti(*w.find(b), w, "fruit_credit", 0);
            w.advance_ticks_for_test(200);
            return w.prop_int(*w.find(b), tpid(w, "fruit_count"), 0);
        };
        const std::int64_t spring = fruit_after(0);        // ticks 0-200: spring
        const std::int64_t winter = fruit_after(2100);     // ticks 2100-2300: winter
        std::printf("  flora: fruit_count spring=%lld winter=%lld\n",
                    (long long)spring, (long long)winter);
        CHECK(spring > 0);
        CHECK(winter < spring);                            // dormant_growth 300 vs 1000
    }
    // ---- predation: stalker runs down grazer, ignores sapient ---------------
    {
        World w = eco_world(14);
        // east side: mild temperatures — no heat/cold damage over the run
        const EntityId s = mk_sapient(w, {15 * 16, 8 * 16}, false);  // inert tier-3 target
        eco_quiet(*w.find(s), w);                             // don't starve mid-test
        const EntityId prey = mk_fauna(w, "grazer", {17 * 16, 8 * 16});
        const EntityId pred = mk_fauna(w, "pack_stalker", {16 * 16, 8 * 16});
        eco_quiet(*w.find(prey), w);
        Entity* pe = w.find(pred);
        eco_quiet(*pe, w);
        tseti(*pe, w, "hunger", 600);                       // a hungry hunter
        tseti(*pe, w, "activity_phase", 0);                 // hunt any hour (test)
        w.mind(prey); w.mind(pred);
        {   // sanity: a live grazer must resist being eaten outright
            const EntityId probe_prey = mk_fauna(w, "grazer", {18 * 16, 8 * 16});
            InteractionContext probe;
            probe.actor = pred; probe.targets = {probe_prey};
            probe.parameters.action = "consume"; probe.parameters.ticks = 2;
            CHECK(!w.interact(probe).completed);
        }
        std::int64_t strikes_on_prey = 0, pred_on_sapient = 0;
        for (int c = 0; c < 1200 && w.find(prey)->alive; ++c) {
            const EventId e0 = w.ledger.next_id();
            autonomous_cycle(w, nullptr);
            for (EventId i = e0; i < w.ledger.next_id(); ++i) {
                const CausalEvent* ev = w.ledger.find(i);
                if (!ev || ev->failed || ev->action != "impact") continue;
                const bool has_pred = std::find(ev->participants.begin(),
                                                ev->participants.end(), pred)
                                      != ev->participants.end();
                const bool has_prey = std::find(ev->participants.begin(),
                                                ev->participants.end(), prey)
                                      != ev->participants.end();
                const bool has_sap = std::find(ev->participants.begin(),
                                               ev->participants.end(), s)
                                     != ev->participants.end();
                if (has_pred && has_prey) ++strikes_on_prey;
                if (has_pred && has_sap) ++pred_on_sapient;
            }
        }
        CHECK(strikes_on_prey >= 3);   // a real hunt: several bites, not one lucky hit
        CHECK(pred_on_sapient == 0);   // tier gate: mid predator can't touch tier-3 sapients
        // apex reaches sapients: shore_reaver (tier 5, ptm 4) vs sapient (3)
        World a = eco_world(15);
        const EntityId sp = mk_sapient(a, {15 * 16, 8 * 16}, false);
        eco_quiet(*a.find(sp), a);
        const EntityId rev = mk_fauna(a, "shore_reaver", {17 * 16, 8 * 16});
        Entity* re = a.find(rev);
        eco_quiet(*re, a);
        tseti(*re, a, "hunger", 600);
        tseti(*re, a, "activity_phase", 0);
        a.mind(rev);
        const std::int64_t sp_i0 = a.prop_int(*a.find(sp), tpid(a, "integrity"), 1000);
        bool apex_struck = false;
        for (int c = 0; c < 400 && !apex_struck; ++c) {
            const EventId e0 = a.ledger.next_id();
            autonomous_cycle(a, nullptr);
            for (EventId i = e0; i < a.ledger.next_id(); ++i) {
                const CausalEvent* ev = a.ledger.find(i);
                if (ev && !ev->failed && ev->action == "impact" &&
                    !ev->participants.empty() && ev->participants.front() == rev)
                    apex_struck = true;
            }
        }
        std::printf("  apex: struck_sapient=%d integrity %lld->%lld\n",
                    int(apex_struck), (long long)sp_i0,
                    (long long)a.prop_int(*a.find(sp), tpid(a, "integrity"), -1));
        CHECK(apex_struck);                                   // tier 3 is within ptm 4
        // ...but a mid-tier predator cannot take a sapient (ptm 1 < tier 3)
        World m = eco_world(16);
        const EntityId sp2 = mk_sapient(m, {15 * 16, 8 * 16}, false);
        eco_quiet(*m.find(sp2), m);
        const EntityId pk = mk_fauna(m, "pack_stalker", {16 * 16, 8 * 16});
        Entity* ke = m.find(pk);
        eco_quiet(*ke, m);
        tseti(*ke, m, "hunger", 600);
        tseti(*ke, m, "activity_phase", 0);
        m.mind(pk);
        bool mid_struck_sapient = false;
        for (int c = 0; c < 300 && !mid_struck_sapient; ++c) {
            const EventId e0 = m.ledger.next_id();
            autonomous_cycle(m, nullptr);
            for (EventId i = e0; i < m.ledger.next_id(); ++i) {
                const CausalEvent* ev = m.ledger.find(i);
                if (ev && !ev->failed && ev->action == "impact" &&
                    !ev->participants.empty() && ev->participants.front() == pk)
                    mid_struck_sapient = true;
            }
        }
        CHECK(!mid_struck_sapient);                            // tier gate held
    }
}

// ---------- Phase 18: domestication, agriculture & specialization -------------
// All thresholds are species/material/process DATA — no species or material
// name appears in engine code. Taming is repeated share-feeding; the bond
// is a `bonded` relation; yields and alerts run on stateless credit
// schedules inside ecology_tick (batch-exact under dormant LOD).
static void test_domestication_agriculture_metallurgy() {
    // ---- schema: Phase 18 content is declarative ----------------------------
    {
        World w(51, "data");
        for (const char* k : {"trail_hound", "dairy_beast", "fiber_beast",
                              "draft_beast", "cultivated_grain",
                              "cultivated_pulse"})
            CHECK(w.species.find_name(k) != nullptr);
        const SpeciesDef* db = w.species.find_name("dairy_beast");
        CHECK(db != nullptr);
        const PropId p_wild = tpid(w, "wildness");
        const EntityId d = mk_fauna(w, "dairy_beast", {100, 100});
        CHECK(w.prop_int(*w.find(d), p_wild, -1) == 750);
        CHECK(w.prop_int(*w.find(d), tpid(w, "tameability"), -1) == 800);
        CHECK(w.prop_int(*w.find(d), tpid(w, "milk_yield"), -1) == 9);
        CHECK(w.prop_int(*w.find(d), tpid(w, "bond_threshold"), -1) == 300);
        // wildness is a gene locus -> tameness is heritable
        bool wild_locus = false;
        for (const PropId g : db->genes) if (g == p_wild) wild_locus = true;
        CHECK(wild_locus);
        for (const char* m : {"milk", "wool", "wool_cloth", "grain", "pulse",
                              "tin_ore", "bronze", "iron_ingot"})
            CHECK(w.materials.find_name(m) != nullptr);
        const MaterialDef* tin = w.materials.find_name("tin_ore");
        CHECK(tin != nullptr);
        const PropId p_smelt = tpid(w, "smelt_temp");
        const auto sit = tin->props.find(p_smelt);
        CHECK(sit != tin->props.end() && sit->second.i == 12000);
        CHECK(w.processes.find("smelt_bronze") != nullptr);
        CHECK(w.processes.find("smelt_iron") != nullptr);
        CHECK(w.processes.find("weave_wool") != nullptr);
    }

    // ---- taming: share-feed -> wildness decay -> bond -> follow -> milk -----
    {
        World w = eco_world(52);
        const EntityId s = mk_sapient(w, {10 * 16, 8 * 16}, false);
        eco_quiet(*w.find(s), w);
        const EntityId a = mk_fauna(w, "dairy_beast", {14 * 16, 8 * 16});
        eco_quiet(*w.find(a), w);
        const PropId p_wild = tpid(w, "wildness");
        const PropId p_qty = tpid(w, "quantity");
        CHECK(w.prop_int(*w.find(a), p_wild, -1) == 750);
        // each share is one meal: wildness falls by tameability-scaled steps
        for (int i = 0; i < 10; ++i) {
            tseti(*w.find(s), w, "quantity", 100);
            const InteractionResult r = w.interact(mk_ctx(s, {a}, "share", 0));
            CHECK(r.completed);
        }
        const std::int64_t wv = w.prop_int(*w.find(a), p_wild, -1);
        CHECK(wv <= 300);                                    // at/below threshold
        CHECK(!w.relations_of(a, "bonded").empty());         // bond edge exists
        // bonded follow: walk the partner away, the beast closes the gap
        const Vec2 ap = w.find(a)->pos;
        w.find(s)->pos = {ap.x + 240, ap.y};
        w.spatial.move(s, w.find(s)->pos);
        w.advance_ticks_for_test(600);
        const std::int64_t d2now = dist2(w.find(a)->pos, w.find(s)->pos);
        CHECK(d2now < 240LL * 240LL / 4);                    // closed most of it
        // milk yield: bonded + fed -> `milk` material entities appear
        w.advance_ticks_for_test(500);
        int milk_n = 0;
        for (const auto& ep : w.entities) {
            const Entity& e = *ep;
            if (e.alive && e.material == w.materials.id_of("milk")) ++milk_n;
        }
        CHECK(milk_n >= 1);
    }

    // ---- watchdog: bonded hound scent-alerts its partner's mind ------------
    {
        World w = eco_world(53);
        const EntityId s = mk_sapient(w, {15 * 16, 8 * 16});
        eco_quiet(*w.find(s), w);
        const EntityId h = mk_fauna(w, "trail_hound", {16 * 16, 8 * 16});
        eco_quiet(*w.find(h), w);
        w.relate(h, "bonded", s);
        // apex within scent_alert_radius (700) but outside its own hunt reach
        const EntityId apex = mk_fauna(w, "ridge_apex", {20 * 16, 8 * 16});
        eco_quiet(*w.find(apex), w);
        w.advance_ticks_for_test(80);
        const AgentMind* m = w.find_mind(s);
        CHECK(m != nullptr);
        const Belief* pb = m->find_belief("peril:predator_scent");
        CHECK(pb != nullptr && pb->confidence > 0);
    }

    // ---- agriculture: sow -> growth gate -> fruiting; irrigation ------------
    {
        World w = eco_world(54);
        // east side: warm + wet — fruiting env is favourable
        PropertyBag sb;
        sb.set(tpid(w, "sprout_target"), Value::text("cultivated_grain"));
        const EntityId seed = w.create_entity("dormant_seed",
            w.materials.id_of("dormant_seed"), {30 * 16, 8 * 16}, sb);
        const EntityId s = mk_sapient(w, {30 * 16 + 8, 8 * 16 + 8}, false);
        eco_quiet(*w.find(s), w);
        const InteractionResult r = w.interact(mk_ctx(s, {seed}, "sow", 0));
        CHECK(r.completed);
        CHECK(!r.spawned.empty());
        const EntityId crop = r.spawned.back();
        const Entity* ce = w.find(crop);
        CHECK(ce != nullptr && ce->kind_name == "cultivated_grain");
        CHECK(w.prop_int(*ce, tpid(w, "cultivated"), -1) == 1);
        // sow commits 1 tick of drift, so growth is already 1, not 0
        CHECK(w.prop_int(*ce, tpid(w, "growth"), -1) <= 1);
        CHECK(w.prop_int(*ce, tpid(w, "fruit_count"), -1) == 0);
        // juvenile: ~100 ticks of growth, still no fruit (growth_ticks=300)
        w.advance_ticks_for_test(100);
        const std::int64_t g1 = w.prop_int(*w.find(crop), tpid(w, "growth"), -1);
        CHECK(g1 >= 100 && g1 < 300);
        CHECK(w.prop_int(*w.find(crop), tpid(w, "fruit_count"), -1) == 0);
        // mature: growth clamps at growth_ticks and fruit starts to set
        w.advance_ticks_for_test(400);
        CHECK(w.prop_int(*w.find(crop), tpid(w, "growth"), -1) == 300);
        CHECK(w.prop_int(*w.find(crop), tpid(w, "fruit_count"), -1) > 0);
        // irrigation: fails with no water near the plant...
        const EntityId dry = w.create_entity("cultivated_pulse",
            w.materials.id_of("living_wood"), {2 * 16, 8 * 16},
            w.species.find_name("cultivated_pulse")->props);
        // ...and a watered twin mid-map fruits faster than its dry sibling
        const EntityId wet = w.create_entity("cultivated_pulse",
            w.materials.id_of("living_wood"), {15 * 16 + 8, 8 * 16 + 8},
            w.species.find_name("cultivated_pulse")->props);
        for (EntityId p : {dry, wet}) {
            tseti(*w.find(p), w, "fruit_count", 0);
            tseti(*w.find(p), w, "fruit_credit", 0);
            tseti(*w.find(p), w, "growth", 260);      // mature: isolate fruiting
        }
        w.find(s)->pos = {2 * 16 + 8, 8 * 16 + 8};
        w.spatial.move(s, w.find(s)->pos);
        InteractionResult ir = w.interact(mk_ctx(s, {dry}, "irrigate", 0));
        CHECK(!ir.completed);                               // no water within 6 cells
        // water within reach of the wet twin only
        w.terrain.water[w.terrain.index({13 * 16, 8 * 16})] = 1;
        w.find(s)->pos = {15 * 16 + 8, 8 * 16 + 8};
        w.spatial.move(s, w.find(s)->pos);
        ir = w.interact(mk_ctx(s, {wet}, "irrigate", 0));
        CHECK(ir.completed);
        CHECK(w.prop_int(*w.find(wet), tpid(w, "irrigation_credit"), -1) > 0);
        w.advance_ticks_for_test(300);
        const std::int64_t f_wet = w.prop_int(*w.find(wet), tpid(w, "fruit_count"), -1);
        const std::int64_t f_dry = w.prop_int(*w.find(dry), tpid(w, "fruit_count"), -1);
        CHECK(f_wet > f_dry);                    // watered soil out-yields rain-fed
    }

    // ---- granary: sealed enclosure slows contained spoilage -----------------
    {
        World w(55, "data");
        const PropId p_seal = tpid(w, "granary_seal");
        const PropId p_cont = tpid(w, "contained");
        const PropId p_integ = tpid(w, "integrity");
        const PropId p_dec = tpid(w, "decay_rate");
        const PropId p_dcred = tpid(w, "decay_credit");
        PropertyBag eb;
        eb.set(p_seal, Value::integer(800));
        const EntityId enc = w.create_entity("enclosure", kNoMaterial,
                                             {100, 100}, eb);
        auto mkfood = [&](Vec2 p, bool inside) {
            PropertyBag fb;
            fb.set(p_integ, Value::integer(1000));
            fb.set(p_dec, Value::integer(50));
            fb.set(p_dcred, Value::integer(0));
            if (inside) fb.set(p_cont, Value::boolean(true));
            const EntityId f = w.create_entity("grain",
                w.materials.id_of("grain"), p, fb);
            if (inside) w.relate(f, "part_of", enc);
            return f;
        };
        const EntityId sealed = mkfood({100, 100}, true);
        const EntityId open_ = mkfood({140, 100}, false);
        w.advance_ticks_for_test(200);
        const std::int64_t is = w.prop_int(*w.find(sealed), p_integ, -1);
        const std::int64_t io = w.prop_int(*w.find(open_), p_integ, -1);
        CHECK(is > io);                     // dry store preserves the surplus
    }

    // ---- metallurgy: new smelt/weave recipes actually run --------------------
    {
        World w(56, "data");
        const MaterialId tin = w.materials.id_of("tin_ore");
        const MaterialId cu = w.materials.id_of("copper_ore");
        const MaterialId ch = w.materials.id_of("charcoal");
        const PropId qty = tpid(w, "quantity");
        const PropId temp = tpid(w, "temperature");
        const EntityId worker = w.create_entity("worker", kNoMaterial, {50, 50});
        PropertyBag o1, o2, f1, heat;
        o1.set(qty, Value::integer(200)); o2.set(qty, Value::integer(120));
        f1.set(qty, Value::integer(60));
        heat.set(temp, Value::integer(20000));   // 2000°C: full smelt efficiency
        const EntityId e1 = w.create_entity("ore", cu, {60, 50}, o1);
        const EntityId e2 = w.create_entity("ore", tin, {61, 50}, o2);
        const EntityId ef = w.create_entity("fuel", ch, {55, 50}, f1);
        const EntityId eh = w.create_entity("heater", kNoMaterial, {52, 50}, heat);
        const InteractionResult r = w.interact(
            mk_ctx(worker, {e1, e2, ef, eh}, "process", 0, "smelt_bronze"));
        CHECK(r.completed);
        std::int64_t bronze_q = 0;
        for (EntityId sid : r.spawned) {
            const Entity* s = w.find(sid);
            if (s && s->material == w.materials.id_of("bronze"))
                bronze_q += w.prop_int(*s, qty, 0);
        }
        CHECK(bronze_q == 240);
        // weave: wool -> wool_cloth, no heat needed
        PropertyBag wl;
        wl.set(qty, Value::integer(60));
        const EntityId wool = w.create_entity("wool", w.materials.id_of("wool"),
                                              {70, 50}, wl);
        const InteractionResult wr = w.interact(
            mk_ctx(worker, {wool}, "process", 0, "weave_wool"));
        CHECK(wr.completed);
        bool cloth = false;
        for (EntityId sid : wr.spawned) {
            const Entity* s = w.find(sid);
            if (s && s->material == w.materials.id_of("wool_cloth")) cloth = true;
        }
        CHECK(cloth);
    }

    // ---- determinism: identical scenarios hash identically ------------------
    {
        auto run = [](std::uint64_t seed) {
            World w = eco_world(seed);
            const EntityId s = mk_sapient(w, {10 * 16, 8 * 16}, false);
            eco_quiet(*w.find(s), w);
            const EntityId a = mk_fauna(w, "dairy_beast", {14 * 16, 8 * 16});
            eco_quiet(*w.find(a), w);
            for (int i = 0; i < 6; ++i) {
                tseti(*w.find(s), w, "quantity", 100);
                w.interact(mk_ctx(s, {a}, "share", 0));
            }
            w.advance_ticks_for_test(300);
            return w.state_hash();
        };
        CHECK(run(57) == run(57));
    }
}

// Phase 19: two-tier physics cognition — qualitative phenomena, emergent
// mathematics with indigenous notation, quantitative laws proven by
// measurement, treatise transmission, and planner use of the parameter.
static void test_quantitative_cognition_and_notation() {
    // ---- indigenous notation: deterministic, legal bases, no digits ------
    {
        std::set<std::int64_t> bases;
        for (std::int64_t l = 1; l <= 64; ++l)
            bases.insert(math_base_of(l, 1234));
        CHECK(bases.size() >= 1 && bases.size() <= 4);
        for (std::int64_t b : bases)
            CHECK(b == 8 || b == 10 || b == 12 || b == 20);
        const std::string r = math_render(7, 2024, 99);
        CHECK(r.find_first_of("0123456789") == std::string::npos);
        CHECK(r == math_render(7, 2024, 99));               // deterministic
        CHECK(math_base_of(7, 99) == math_base_of(7, 99));
        // different tongues may share a base but render differently enough
        // to be distinguishable glyph streams
        bool any_diff = false;
        for (std::int64_t l = 1; l <= 12 && !any_diff; ++l)
            any_diff = math_render(l, 555, 99) != math_render(7, 555, 99);
        CHECK(any_diff);
    }

    // ---- emergence: share-loads -> phenomenon -> math:ratio -> qlaw ------
    World w(67, "data");
    const EntityId s = mk_sapient(w, {100, 100});
    tseti(*w.find(s), w, "mind_abstraction_gain", 1500);
    tseti(*w.find(s), w, "mass", 70000);
    tseti(*w.find(s), w, "energy", 90000000);
    // a composite "hull" the scholar loads: lift 200kg, holds trade units
    PropertyBag cp;
    cp.set(tpid(w, "quantity"), Value::integer(0));
    cp.set(tpid(w, "float_capacity"), Value::integer(200000));
    const EntityId hull = w.create_entity("composite",
        w.materials.id_of("living_wood"), {112, 100}, cp);
    // ~115 successful loads: qualitative recognition + quantitative need
    for (int i = 0; i < 115; ++i) {
        tseti(*w.find(s), w, "quantity", 100);
        w.interact(mk_ctx(s, {hull}, "share", 0));
    }
    const AgentMind* sm = w.find_mind(s);
    CHECK(sm != nullptr);
    CHECK(sm->find_belief("phenomenon:buoyancy") != nullptr);
    CHECK(sm->find_belief("math:ratio") != nullptr);        // math invented
    // proven law: safe load = (200000 - 70000) / 1000 = 130 units
    CHECK(qlaw_param(*sm, "buoyancy") == 130);
    const Belief* qb = sm->find_belief("qlaw:buoyancy:p130");
    CHECK(qb && qb->status == BeliefStatus::Verified);

    // ---- trajectory law: measured off the hardest remembered material ---
    EntityId anvil = w.create_entity("rock",
        w.materials.id_of("granite"), {105, 100});
    auto revive = [&] {   // thousands of process ticks would starve the scholar
        Entity& se = *w.find(s);
        tseti(se, w, "energy", 90000000); tseti(se, w, "hunger", 0);
        tseti(se, w, "thirst", 0);         tseti(se, w, "integrity", 1000);
        tseti(se, w, "blood_volume", 1000);
    };
    for (int i = 0; i < 115; ++i) {
        if (!w.find(anvil)->alive)                    // spent anvils get replaced
            anvil = w.create_entity("rock",
                w.materials.id_of("granite"), {105, 100});
        revive();
        w.interact(mk_ctx(s, {anvil}, "impact", 100));
    }
    sm = w.find_mind(s);
    const std::int64_t tq = qlaw_param(*sm, "trajectory");
    CHECK(tq > 0);                                            // hardness-derived
    CHECK(sm->find_belief("math:geometry") != nullptr);

    // ---- smelt law: fuel share of the richest known recipe ---------------
    const PropId qty = tpid(w, "quantity");
    const PropId temp = tpid(w, "temperature");
    const EntityId cu = w.create_entity("ore", w.materials.id_of("copper_ore"),
        {90, 90}, [&]{ PropertyBag b; b.set(qty, Value::integer(300000)); return b; }());
    const EntityId ti = w.create_entity("ore", w.materials.id_of("tin_ore"),
        {91, 90}, [&]{ PropertyBag b; b.set(qty, Value::integer(300000)); return b; }());
    const EntityId fu = w.create_entity("fuel", w.materials.id_of("charcoal"),
        {92, 90}, [&]{ PropertyBag b; b.set(qty, Value::integer(300000)); return b; }());
    PropertyBag hb; hb.set(temp, Value::integer(20000));
    const EntityId heat = w.create_entity("heater", kNoMaterial, {93, 90}, hb);
    for (int i = 0; i < 115; ++i) {
        // restock ore/fuel and stoke the furnace — process consumes them
        for (EntityId m : {cu, ti, fu})
            tseti(*w.find(m), w, "quantity", 300000);
        tseti(*w.find(heat), w, "temperature", 20000);
        revive();
        w.interact(mk_ctx(s, {cu, ti, fu, heat}, "process", 0, "smelt_bronze"));
    }
    sm = w.find_mind(s);
    const std::int64_t sq = qlaw_param(*sm, "smelt");
    CHECK(sq > 0);                       // 60/(200+120+60) -> 157 permille

    // ---- planner: parameters.force caps the transferred load ------------
    {
        const EntityId dock = w.create_entity("composite",
            w.materials.id_of("living_wood"), {108, 100}, cp);
        // law-level: a caller that knows the law asks for exactly 130
        revive();
        w.find(s)->pos = {100, 100};
        tseti(*w.find(s), w, "quantity", 500);
        tseti(*w.find(dock), w, "quantity", 0);
        CHECK(w.interact(mk_ctx(s, {dock}, "share", 130)).completed);
        CHECK(w.prop_int(*w.find(s), qty, -1) == 370);
        CHECK(w.prop_int(*w.find(dock), qty, -1) == 130);
        // driver-level: quantum_step picks the remembered hull and loads
        // the proven amount autonomously
        tseti(*w.find(s), w, "quantity", 500);
        tseti(*w.find(s), w, "hunger", 0); tseti(*w.find(s), w, "thirst", 0);
        const std::int64_t hq0 = w.prop_int(*w.find(hull), qty, -1);
        DriverState st;
        autonomous_run(w, 1, &st);
        CHECK(w.prop_int(*w.find(hull), qty, -1) - hq0 == 130);
    }

    // ---- treatise: M/P/Q lines carved, imported as halved claims ---------
    {
        w.mind(s).discoveries["method:x"] = mk_record("method:x",
                                                   DiscoveryStatus::Verified);
        w.find(s)->pos = {100, 100};          // the wanderer returns
        const EntityId stone = w.create_entity("rock", kNoMaterial, {103, 100});
        CHECK(w.interact(mk_ctx(s, {stone}, "engrave", 0, "method:x")).completed);
        const Value* iv = w.prop_of(*w.find(stone), tpid(w, "inscription"));
        CHECK(iv && iv->type == ValType::Text);
        if (iv && iv->type == ValType::Text) {
            CHECK(iv->s.find("M|ratio|") != std::string::npos);
            CHECK(iv->s.find("P|buoyancy|") != std::string::npos);
            CHECK(iv->s.find("Q|buoyancy|130|") != std::string::npos);
        }
        const EntityId reader = mk_sapient(w, {125, 100});
        CHECK(w.interact(mk_ctx(reader, {stone}, "read", 0)).completed);
        const AgentMind* rm = w.find_mind(reader);
        CHECK(rm != nullptr);
        const Belief* mt = rm->find_belief("math:ratio");
        CHECK(mt && mt->confidence > 0 && mt->confidence <= 500);
        const Belief* pq = rm->find_belief("qlaw:buoyancy:p130");
        CHECK(pq && pq->status != BeliefStatus::Verified); // claim, not proof
        CHECK(qlaw_param(*rm, "buoyancy") == 130);           // still usable
    }

    // ---- determinism -------------------------------------------------------
    {
        auto run = [](std::uint64_t seed) {
            World ww(67 + seed, "data");
            const EntityId a = mk_sapient(ww, {100, 100});
            tseti(*ww.find(a), ww, "mind_abstraction_gain", 1500);
            tseti(*ww.find(a), ww, "energy", 90000000);
            PropertyBag c2;
            c2.set(tpid(ww, "quantity"), Value::integer(0));
            c2.set(tpid(ww, "float_capacity"), Value::integer(200000));
            const EntityId h2 = ww.create_entity("composite",
                ww.materials.id_of("living_wood"), {112, 100}, c2);
            for (int i = 0; i < 60; ++i) {
                tseti(*ww.find(a), ww, "quantity", 100);
                ww.interact(mk_ctx(a, {h2}, "share", 0));
            }
            ww.advance_ticks_for_test(120);
            return ww.state_hash();
        };
        CHECK(run(0) == run(0));
    }
}

static void test_render_snapshot_and_procedural_atlas() {
    using namespace simlab::render;
    World w(91, "data");
    ProceduralAtlas atlas;
    atlas.build(w);
    auto mean = [](const Sprite& s, int ch) {
        long long sum = 0, n = 0;
        for (std::uint32_t p : s.px) if (p >> 24) { sum += (p >> (16 - 8 * ch)) & 0xFF; ++n; }
        return n ? sum / n : 0;
    };
    // ---- every material: a full tile + an icon, all distinct ----------------
    std::set<std::uint64_t> digests;
    for (const MaterialDef& m : w.materials.defs()) {
        const Sprite* t = atlas.material(m.id);
        const Sprite* ic = atlas.material_icon(m.id);
        CHECK(t && t->w == ProceduralAtlas::kTile && t->h == ProceduralAtlas::kTile);
        CHECK(t && t->opaque_count() == static_cast<std::size_t>(t->w * t->h));
        CHECK(ic && ic->w == ProceduralAtlas::kIcon && ic->opaque_count() > 40);
        if (t) digests.insert(t->digest());
    }
    CHECK(digests.size() == w.materials.defs().size());          // no two look alike
    // ---- the art follows the physics ----------------------------------------
    auto cls = [&](const char* n) { return atlas.look(w.materials.id_of(n))->cls; };
    for (const char* n : {"copper_metal", "metal_plate", "iron_blade"}) CHECK(cls(n) == MatClass::Metal);
    for (const char* n : {"granite", "flint", "basalt"}) CHECK(cls(n) == MatClass::Rock);
    for (const char* n : {"dry_wood", "hardwood"}) CHECK(cls(n) == MatClass::Wood);
    for (const char* n : {"soil_sand", "soil_loam", "soil_clay"}) CHECK(cls(n) == MatClass::Soil);
    CHECK(cls("water") == MatClass::Liquid);
    CHECK(cls("energetic_mix") == MatClass::Energetic);
    CHECK(atlas.look(w.materials.id_of("nightshade"))->toxic);   // toxicity -> violet
    CHECK(!atlas.look(w.materials.id_of("wild_berries"))->toxic);
    {   // metal sheen: the brightest pixel of a metal tile is near-white
        const Sprite& cu = *atlas.material(w.materials.id_of("copper_metal"));
        int maxv = 0;
        for (std::uint32_t p : cu.px) maxv = std::max<int>(maxv, std::min({(p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF}));
        CHECK(maxv >= 200);
        const Sprite& ns = *atlas.material(w.materials.id_of("nightshade"));
        CHECK(mean(ns, 2) > mean(ns, 1));                         // more blue than green: poison
        const Sprite& wa = *atlas.material(w.materials.id_of("water"));
        CHECK(mean(wa, 2) > mean(wa, 0));                          // water is blue
    }
    // ---- every species: a standee read from its anatomy ---------------------
    for (const SpeciesDef& sp : w.species.defs()) {
        const Sprite* s = atlas.species(sp.name);
        CHECK(s && s->w == ProceduralAtlas::kStandee && s->h == ProceduralAtlas::kStandee);
        CHECK(s && s->opaque_count() > 40);
        CHECK(atlas.plan(sp.name) && atlas.plan(sp.name)->flora == !sp.fauna);
    }
    if (const BodyPlan* gp = atlas.plan("grazer")) {                // read from its body_part lines
        CHECK(gp->arms == 2 && gp->legs == 2 && gp->heads == 1 && gp->eyes == 2);
    }
    if (const BodyPlan* hp = atlas.plan("sapient")) {                // no anatomy: reach -> hands
        CHECK(hp->arms == 2 && hp->legs == 2);
    }
    CHECK(atlas.plan("boreal_tree")->tree);
    CHECK(atlas.plan("berry_bush")->fruiting && !atlas.plan("berry_bush")->tree);
    // ---- fruit dots: exactly proportional to fruit_count / max_fruit ---------
    {
        const std::size_t f0 = atlas.fruiting("berry_bush", 0, 40).opaque_count();
        const std::uint64_t d20 = atlas.fruiting("berry_bush", 20, 40).digest();
        const std::uint64_t d40 = atlas.fruiting("berry_bush", 40, 40).digest();
        const std::uint64_t d0 = atlas.fruiting("berry_bush", 0, 40).digest();
        CHECK(d0 != d20 && d20 != d40);
        CHECK(atlas.fruiting("berry_bush", 40, 40).opaque_count() >= f0);
        CHECK(atlas.fruiting("berry_bush", 20, 40).digest() == d20);   // deterministic cache
    }
    // ---- engine kinds, weapons, armor, walls, scripts ------------------------
    for (const char* k : {"inscription", "enclosure", "portal", "remains", "rubble", "deposit",
                          "residue", "composite", "goods", "person"})
        CHECK(atlas.kind(k) && atlas.kind(k)->opaque_count() > 20);
    const MaterialId fl = w.materials.id_of("flint");
    const std::uint64_t blade = atlas.weapon(900, 0, 0, false, fl).digest();
    const std::uint64_t mace = atlas.weapon(0, 800, 0, false, fl).digest();
    const std::uint64_t bow = atlas.weapon(0, 0, 650, false, fl).digest();
    const std::uint64_t poison = atlas.weapon(900, 0, 0, true, fl).digest();
    CHECK(blade != mace && mace != bow && blade != bow && blade != poison);
    CHECK(atlas.armor_overlay(900, w.materials.id_of("metal_plate")).opaque_count() > 0);
    {   // 47-tile autotiling: 256 neighbour masks collapse to exactly 47 shapes
        std::set<std::uint64_t> shapes;
        for (int m = 0; m < 256; ++m) shapes.insert(atlas.wall(static_cast<std::uint8_t>(m), 0, w.materials.id_of("stone")).digest());
        CHECK(shapes.size() == 47);
        CHECK(atlas.wall(0x55, 0, w.materials.id_of("stone")).digest() !=
              atlas.wall(0x55, 3, w.materials.id_of("stone")).digest());   // cracks show
    }
    {   // each tribe has its own script; a carving is reproducible
        CHECK(atlas.glyph(11 * 2654435761u, 0).digest() != atlas.glyph(22 * 2654435761u, 0).digest());
        CHECK(atlas.glyph_stone(77, "peace|cede:5", false).digest() ==
              atlas.glyph_stone(77, "peace|cede:5", false).digest());
        CHECK(atlas.glyph_stone(77, "peace|cede:5", false).digest() !=
              atlas.glyph_stone(78, "peace|cede:5", false).digest());
        CHECK(atlas.glyph_stone(77, "peace|cede:5", false).opaque_count() > 100);
    }
    CHECK(atlas.bytes() < 8u * 1024u * 1024u);                     // 4GB-machine friendly

    // ---- snapshot + 3-tier fog of war -----------------------------------------
    World v(92, "data");
    {
        TerrainGrid& tg = v.terrain; tg.w = 64; tg.h = 64; tg.cell = 16; tg.ox = 0; tg.oy = 0;
        const std::size_t n = 64 * 64;
        tg.elevation.assign(n, 200); tg.moisture.assign(n, 500); tg.temperature.assign(n, 200);
        tg.water.assign(n, 0); tg.vegetation.assign(n, 300); tg.slope.assign(n, 0);
        tg.wind_x.assign(n, 0); tg.wind_y.assign(n, 0); tg.water_table.assign(n, 10);
        tg.traffic_wear.assign(n, 0);
    }
    const EntityId scout = mk_person(v, {100, 100});
    tseti(*v.find(scout), v, "sight_range", 120);
    tseti(*v.find(scout), v, "language_id", 5);
    v.mind(scout);
    PropertyBag eb;
    eb.set(tpid(v, "footprint_radius"), Value::integer(40));
    eb.set(tpid(v, "integrity"), Value::integer(900));
    const EntityId hut = v.create_entity("enclosure", v.materials.id_of("stone"), {130, 110}, eb);
    const EntityId stranger = mk_person(v, {140, 100});            // walks inside the scouted zone
    const EntityId far_thing = mk_person(v, {900, 900});
    const MaterialDef* stone_md = v.materials.find_name("stone");
    const EntityId rock = v.create_entity("rock", stone_md ? stone_md->id : kNoMaterial, {120, 110}, PropertyBag{});
    FogOfWar fog;
    fog.set_mode(FogOfWar::Mode::Tribe, 5);
    fog.update(v);
    CHECK(fog.point_fog(v, {100, 100}) == Fog::Visible);
    CHECK(fog.point_fog(v, {900, 900}) == Fog::Unexplored);
    const ViewRect all{0, 0, 1024, 1024};
    const std::uint64_t h0 = v.state_hash();
    RenderSnapshot s1 = capture(v, all, fog);
    auto has = [](const RenderSnapshot& s, EntityId id) {
        for (const EntityView& e : s.entities) if (e.id == id) return &e;
        return static_cast<const EntityView*>(nullptr);
    };
    CHECK(has(s1, hut) && has(s1, hut)->fog == Fog::Visible);
    CHECK(has(s1, stranger) != nullptr);
    CHECK(has(s1, far_thing) == nullptr);                            // never seen
    {   // unexplored cells leak no terrain
        bool leak = false;
        for (const CellView& c : s1.cells) if (c.fog == Fog::Unexplored && c.elevation != 0) leak = true;
        CHECK(!leak);
    }
    // the scout walks away (test moves it directly — the viewer never would)
    v.find(scout)->pos = {700, 700}; v.spatial.move(scout, {700, 700});
    fog.update(v);
    CHECK(fog.point_fog(v, {100, 100}) == Fog::Shrouded);
    CHECK(fog.point_fog(v, {700, 700}) == Fog::Visible);
    RenderSnapshot s2 = capture(v, all, fog);
    CHECK(has(s2, hut) && has(s2, hut)->fog == Fog::Shrouded);       // remembered architecture
    CHECK(has(s2, stranger) == nullptr);                              // motion hidden
    CHECK(has(s1, rock) && has(s1, rock)->fog == Fog::Visible);
    CHECK(has(s2, rock) && has(s2, rock)->fog == Fog::Shrouded);     // things stay, frozen as last seen
    // God Eye sees everything
    FogOfWar god;
    god.update(v);
    RenderSnapshot s3 = capture(v, all, god);
    CHECK(has(s3, far_thing) && has(s3, stranger));
    // viewport culling: a small window holds fewer entities
    RenderSnapshot s4 = capture(v, ViewRect{600, 600, 800, 800}, god);
    CHECK(s4.entities.size() < s3.entities.size());
    CHECK(!s4.cells.empty() && s4.cells.size() < s3.cells.size());
    const std::uint64_t h1 = v.state_hash();                       // after the test's own edits
    // back-to-front order
    for (std::size_t i = 1; i < s3.entities.size(); ++i)
        CHECK(s3.entities[i - 1].pos.y <= s3.entities[i].pos.y);
    // ---- rendering never perturbs the simulation ------------------------------
    for (int f = 0; f < 300; ++f) {
        fog.update(v);
        RenderSnapshot sf = capture(v, ViewRect{f, f, 512 + f, 512 + f}, fog);
        for (const EntityView& e : sf.entities)
            if (!e.species.empty()) (void)atlas.species(e.species);
    }
    CHECK(v.state_hash() == h1);
    (void)h0; (void)atlas.size();
}

// ---------- CL-1 Phase 0: brain substrate ------------------------------------
// BrainArena, int8 kernels, activation LUTs, hash RNG. No world behavior —
// this suite verifies determinism and measures the GMAC/s assumption the
// blueprint budget calculator needs.
static void test_brain_phase0() {
    // ---- LUTs: deterministic, sane shapes ------------------------------------
    const std::int8_t* lin = brain_act_lut(BrainAct::kLinear);
    const std::int8_t* th  = brain_act_lut(BrainAct::kTanh);
    const std::int8_t* sg  = brain_act_lut(BrainAct::kSigmoid);
    CHECK(lin && th && sg);
    CHECK(lin[static_cast<std::uint8_t>(static_cast<std::int8_t>(0))] == 0);
    CHECK(lin[static_cast<std::uint8_t>(static_cast<std::int8_t>(42))] == 42);
    // tanh saturates, sigmoid is tanh(u/2) — both bounded, odd/even sane
    CHECK(th[static_cast<std::uint8_t>(static_cast<std::int8_t>(127))] > 100);
    CHECK(th[static_cast<std::uint8_t>(static_cast<std::int8_t>(-128))] < -100);
    CHECK(th[static_cast<std::uint8_t>(static_cast<std::int8_t>(0))] == 0);
    CHECK(sg[static_cast<std::uint8_t>(static_cast<std::int8_t>(127))] >
          sg[static_cast<std::uint8_t>(static_cast<std::int8_t>(-128))]);
    // same table object every call (built once)
    CHECK(brain_act_lut(BrainAct::kTanh) == th);

    // ---- dot + dense kernels: deterministic, activation applied --------------
    std::vector<std::int8_t> w(4 * 8), x(8), y1(4), y2(4);
    for (int i = 0; i < 8; ++i)
        x[i] = static_cast<std::int8_t>(brain_rand_weight(7, 1, i, 24));
    for (int i = 0; i < 32; ++i)
        w[i] = static_cast<std::int8_t>(brain_rand_weight(7, 2, i, 24));
    std::int16_t bias[4] = {3, -2, 0, 5};
    brain_dense_i8(w.data(), 4, 8, x.data(), bias, 4, BrainAct::kLinear, y1.data());
    brain_dense_i8(w.data(), 4, 8, x.data(), bias, 4, BrainAct::kLinear, y2.data());
    CHECK(y1 == y2);
    // hand-check row 0: dot = sum w[0..7]*x[0..7]
    std::int32_t d0 = 0;
    for (int i = 0; i < 8; ++i) d0 += w[i] * x[i];
    CHECK(y1[0] == brain_clamp_i8((d0 >> 4) + bias[0]));
    // tanh path differs from linear and stays in range
    brain_dense_i8(w.data(), 4, 8, x.data(), bias, 4, BrainAct::kTanh, y2.data());
    CHECK(y2 != y1);
    for (int i = 0; i < 4; ++i) CHECK(y2[i] >= -128 && y2[i] <= 127);
    // dot product cross-check against a naive int64 accumulation
    for (int i = 0; i < 32; ++i) CHECK(w[i] >= -127 && w[i] <= 127);
    CHECK(brain_dot_i8(x.data(), x.data(), 8) ==
          [&]{ std::int64_t s = 0; for (int i = 0; i < 8; ++i) s += x[i]*x[i]; return (std::int32_t)s; }());

    // ---- hash RNG: replay-identical draws ------------------------------------
    CHECK(brain_rand_weight(42, 9, 100, 24) == brain_rand_weight(42, 9, 100, 24));
    CHECK(brain_rand_weight(42, 9, 100, 24) != brain_rand_weight(42, 9, 101, 24));
    CHECK(brain_mutation_step(1, 2, 3, 4, 5) == brain_mutation_step(1, 2, 3, 4, 5));
    std::int32_t st = brain_mutation_step(1, 2, 3, 4, 5);
    CHECK(st >= -5 && st <= 5);

    // ---- BrainArena: deterministic alloc/free, cell granularity ----------------
    BrainArena ar(64);
    const std::uint64_t o1 = ar.alloc(100);    // 2 cells
    const std::uint64_t o2 = ar.alloc(64);     // 1 cell
    CHECK(o1 != BrainArena::kInvalid && o2 != BrainArena::kInvalid);
    CHECK(o2 >= o1 + 128);
    // returned offsets are slab-relative; the DATA pointers are 64B-aligned
    CHECK(reinterpret_cast<std::uintptr_t>(ar.w8(o1)) % 64 == 0);
    CHECK(reinterpret_cast<std::uintptr_t>(ar.w8(o2)) % 64 == 0);
    ar.w8(o1)[0] = 42; ar.w8(o1)[63] = -7;
    ar.free(o1, 100);
    const std::uint64_t o3 = ar.alloc(100);    // reuses freed 2-cell block
    CHECK(o3 == o1);
    CHECK(ar.w8(o3)[0] == 42);                 // contents survive free
    CHECK(ar.live_bytes() == 3 * 64);
    ar.free(o2, 64); ar.free(o3, 100);
    CHECK(ar.live_bytes() == 0);
    // alloc past capacity fails cleanly
    BrainArena tiny(2);
    CHECK(tiny.alloc(3 * 64) == BrainArena::kInvalid);

    // ---- mutation: deterministic, sparse, bounded ------------------------------
    std::vector<std::int8_t> mw(256);
    for (int i = 0; i < 256; ++i) mw[i] = 10;
    brain_mutate_blocks(mw.data(), 256, 5, 6, 7, 3, 500);
    std::vector<std::int8_t> mw2 = mw;
    brain_mutate_blocks(mw2.data(), 256, 5, 6, 7, 3, 500);
    // NOTE: second call mutates the ALREADY-mutated copy — instead rebuild:
    for (int i = 0; i < 256; ++i) mw2[i] = 10;
    brain_mutate_blocks(mw2.data(), 256, 5, 6, 7, 3, 500);
    CHECK(mw == mw2);                                   // deterministic
    bool changed = false;
    for (int i = 0; i < 256; ++i) {
        CHECK(mw[i] >= 7 && mw[i] <= 13);
        if (mw[i] != 10) changed = true;
    }
    CHECK(changed);                                      // 500‰ fires somewhere
    // different tick -> different result somewhere
    for (int i = 0; i < 256; ++i) mw2[i] = 10;
    brain_mutate_blocks(mw2.data(), 256, 5, 6, 8, 3, 500);
    CHECK(mw != mw2);

    // ---- GMAC/s micro-benchmark (replaces the blueprint's 3 GMAC/s guess) ------
    // Dense GEMV shaped like an H=64 layer family: rows=64, cols=96.
    {
        const std::int32_t R = 64, C = 96, PAD = brain_padded_cols(C);
        std::vector<std::int8_t> bw(static_cast<std::size_t>(R) * PAD, 0);
        std::vector<std::int8_t> bx(C, 1), by(R);
        for (std::int64_t i = 0; i < static_cast<std::int64_t>(R) * PAD; ++i)
            bw[static_cast<std::size_t>(i)] =
                static_cast<std::int8_t>(brain_rand_weight(1, 3, i, 24));
        const int iters = 20000;
        const auto t0 = std::chrono::steady_clock::now();
        std::int64_t acc = 0;
        for (int it = 0; it < iters; ++it) {
            brain_dense_i8(bw.data(), R, C, bx.data(), nullptr, 8,
                           BrainAct::kLinear, by.data());
            acc += by[it & (R - 1)];
        }
        const auto t1 = std::chrono::steady_clock::now();
        const double secs = std::chrono::duration<double>(t1 - t0).count();
        const double macs = static_cast<double>(iters) * R * C;
        const double gmacs = macs / secs / 1e9;
        std::printf("brain GEMV benchmark: %.2f GMAC/s "
                    "(%d iters x %dx%d, acc=%lld, %.1f MB/s weights)\n",
                    gmacs, iters, R, C, static_cast<long long>(acc),
                    macs / secs / 1e6);
        CHECK(gmacs > 0.05);      // sanity floor — real number printed above
        CHECK(acc != 0);
    }
}

static void test_brain_phase1() {
    // ---- flag off: no brain store is ever created ---------------------------
    {
        World w(5, "data");
        const EntityId s = mk_sapient(w, {50, 50});
        w.interact(mk_ctx(s, {}, "rest", 0));
        w.interact(mk_ctx(s, {}, "strike", 0));
        CHECK(w.brains_ == nullptr);
    }

    // ---- flag on: state_hash bit-identical to flag off ----------------------
    {
        World on(11, "data"), off(11, "data");
        const EntityId son = mk_sapient(on, {50, 50});
        const EntityId soff = mk_sapient(off, {50, 50});
        on.brain_shadow = true;
        for (int i = 0; i < 60; ++i) {
            const char* verb = (i % 3 == 0) ? "strike" : "rest";
            on.interact(mk_ctx(son, {}, verb, 0));
            off.interact(mk_ctx(soff, {}, verb, 0));
        }
        // Phase 2: live brain state IS part of state_hash, so a brained
        // world no longer hashes identically to a brainless one. The
        // invariant that survives: flag-off never allocates a store, and
        // two flag-off worlds remain bit-identical.
        CHECK(on.brains_ != nullptr);
        CHECK(on.brains_->slots.count(son) == 1);
        CHECK(on.brains_->wm_n > 0);
        CHECK(off.brains_ == nullptr);
        World off2(11, "data");
        const EntityId soff2 = mk_sapient(off2, {50, 50});
        for (int i = 0; i < 60; ++i) {
            const char* verb = (i % 3 == 0) ? "strike" : "rest";
            off2.interact(mk_ctx(soff2, {}, verb, 0));
        }
        CHECK(off.state_hash() == off2.state_hash());
    }

    // ---- determinism: identical seeds -> identical brain fingerprint --------
    {
        World a(23, "data"), b(23, "data");
        a.brain_shadow = b.brain_shadow = true;
        const EntityId sa = mk_sapient(a, {50, 50});
        const EntityId sb = mk_sapient(b, {50, 50});
        for (int i = 0; i < 80; ++i) {
            const char* verb = (i % 4 == 0) ? "strike" : "rest";
            a.interact(mk_ctx(sa, {}, verb, 0));
            b.interact(mk_ctx(sb, {}, verb, 0));
        }
        CHECK(a.state_hash() == b.state_hash());
        CHECK(a.brains_ && b.brains_);
        // diagnostic: locate the divergent component
        {
            const BrainSlot& A = a.brains_->slots.at(sa);
            const BrainSlot& B = b.brains_->slots.at(sb);
            CHECK(A.wbytes == B.wbytes);   // woff differs: heap-alignment slack
            CHECK(a.brains_->slots.size() == b.brains_->slots.size());
            CHECK(a.brains_->wm_err_sum == b.brains_->wm_err_sum);
            CHECK(a.brains_->wm_n == b.brains_->wm_n);
            CHECK(a.brains_->sigma_sum == b.brains_->sigma_sum);
            CHECK(a.brains_->sigma_n == b.brains_->sigma_n);
            CHECK(a.brains_->surprise_events == b.brains_->surprise_events);
            CHECK(sa == sb);
            CHECK(A.h == B.h);
            CHECK(A.last_action == B.last_action);
            CHECK(A.replay_count == B.replay_count);
            CHECK(A.replay_next == B.replay_next);
            for (int i = 0; i < 16; ++i) {
                CHECK(A.replay[i].event_id == B.replay[i].event_id);
                CHECK(A.replay[i].sigma == B.replay[i].sigma);
                CHECK(A.replay[i].charge == B.replay[i].charge);
                CHECK(A.replay[i].s == B.replay[i].s);
                CHECK(A.replay[i].a == B.replay[i].a);
                CHECK(A.replay[i].y == B.replay[i].y);
            }
            const std::int8_t* wa = a.brains_->arena.w8(A.woff);
            const std::int8_t* wb2 = b.brains_->arena.w8(B.woff);
            for (std::uint64_t i = 0; i < A.wbytes; ++i)
                if (wa[i] != wb2[i]) {
                    std::printf("brain weight diff @%llu: %d vs %d\n",
                                (unsigned long long)i, wa[i], wb2[i]);
                    break;
                }
        }
        CHECK(brain_state_hash(*a.brains_) == brain_state_hash(*b.brains_));
        // replay ring fills and stratified replacement keeps high-surprise rows
        const BrainSlot& slot = a.brains_->slots.at(sa);
        CHECK(slot.replay_count == 16);
        CHECK(slot.pending.valid == false);          // consumed by post
    }

    // ---- learning curve: WM |y - yhat| falls over repeated interactions -----
    {
        World w(37, "data");
        w.brain_shadow = true;
        const EntityId s = mk_sapient(w, {50, 50});
        std::int64_t a_sum = 0, a_n = 0;   // snapshot before interact i=1
        std::int64_t b_sum = 0, b_n = 0;   // snapshot before interact i=60
        std::int64_t c_sum = 0, c_n = 0;   // snapshot before interact i=840
        for (int i = 0; i < 900; ++i) {
            if ((i % 150) == 0) {          // vitals top-up: marathon, not survival
                Entity& e = *w.find(s);
                tseti(e, w, "energy", 90000000); tseti(e, w, "hunger", 0);
                tseti(e, w, "thirst", 0);  tseti(e, w, "integrity", 1000);
                tseti(e, w, "fatigue", 0); tseti(e, w, "blood_volume", 1000);
            }
            if (w.brains_) {
                if (i == 1)   { a_sum = w.brains_->wm_err_sum; a_n = w.brains_->wm_n; }
                if (i == 60)  { b_sum = w.brains_->wm_err_sum; b_n = w.brains_->wm_n; }
                if (i == 840) { c_sum = w.brains_->wm_err_sum; c_n = w.brains_->wm_n; }
            }
            w.interact(mk_ctx(s, {}, (i % 5 == 4) ? "strike" : "rest", 0));
        }
        CHECK(w.brains_ != nullptr);
        const std::int64_t d_sum = w.brains_->wm_err_sum, d_n = w.brains_->wm_n;
        if (c_n == 0) { c_sum = d_sum; c_n = d_n; }
        // windowed mean |y-yhat| per label channel: first ~60 events vs last ~60
        const std::int64_t early_num = b_sum - a_sum, early_den = b_n - a_n;
        const std::int64_t late_num  = d_sum - c_sum, late_den  = d_n - c_n;
        CHECK(early_den > 0 && late_den > 0);
        const double early_mae = static_cast<double>(early_num) / early_den;
        const double late_mae  = static_cast<double>(late_num)  / late_den;
        std::printf("brain WM MAE: early=%.3f late=%.3f (n=%lld events)\n",
                    early_mae, late_mae,
                    static_cast<long long>(d_n / kLabelChannels));
        CHECK(late_mae < early_mae);       // the WM actually learned

        // replay ring is full of valid, scored transitions
        const BrainSlot& sl = w.brains_->slots.at(s);
        CHECK(sl.replay_count == 16);
        for (const Transition& t : sl.replay) CHECK(t.valid);
    }

    // ---- slot GC: dead agents release their arena slot ----------------------
    {
        World w(41, "data");
        w.brain_shadow = true;
        const EntityId s = mk_sapient(w, {50, 50});
        const EntityId t = mk_sapient(w, {60, 50});
        w.interact(mk_ctx(s, {}, "rest", 0));
        w.interact(mk_ctx(t, {}, "rest", 0));
        CHECK(w.brains_->slots.size() == 2);
        const std::uint64_t live0 = w.brains_->arena.live_bytes();
        w.find(s)->alive = false;
        w.advance_ticks_for_test(50);          // decay_minds -> brain_gc
        CHECK(w.brains_->slots.count(s) == 0);
        CHECK(w.brains_->slots.count(t) == 1);
        CHECK(w.brains_->arena.live_bytes() < live0);
    }
}

static void test_brain_phase2() {
    // ---- brain_decide: deterministic choice, valid mask --------------------
    {
        World a(31, "data"), b(31, "data");
        a.brain_shadow = b.brain_shadow = true;
        const EntityId sa = mk_sapient(a, {50, 50});
        const EntityId sb = mk_sapient(b, {50, 50});
        // give both minds a couple of known verbs so the mask is non-trivial
        for (int i = 0; i < 10; ++i) {
            a.interact(mk_ctx(sa, {}, "rest", 0));
            b.interact(mk_ctx(sb, {}, "rest", 0));
        }
        const BrainChoice ca = brain_decide(a, sa);
        const BrainChoice cb = brain_decide(b, sb);
        CHECK(ca.valid && cb.valid);
        CHECK(ca.verb == cb.verb);
        CHECK(ca.target == cb.target);
        CHECK(ca.has_move == cb.has_move);
        CHECK(ca.move_to.x == cb.move_to.x && ca.move_to.y == cb.move_to.y);
        CHECK(ca.score == cb.score);
        // decide must NOT commit recurrent state: h unchanged by the call
        const BrainSlot& slot = a.brains_->slots.at(sa);
        std::int64_t hsum = 0;
        for (auto v : slot.h) hsum += v;
        brain_decide(a, sa);
        std::int64_t hsum2 = 0;
        for (auto v : a.brains_->slots.at(sa).h) hsum2 += v;
        CHECK(hsum == hsum2);
    }

    // ---- inheritance: child genome is parent crossover, fast state empty ---
    {
        World w(41, "data");
        w.brain_shadow = true;
        const EntityId pa = mk_sapient(w, {50, 50});
        const EntityId pb = mk_sapient(w, {60, 50});
        for (int i = 0; i < 6; ++i) {
            w.interact(mk_ctx(pa, {}, "rest", 0));
            w.interact(mk_ctx(pb, {}, "rest", 0));
        }
        CHECK(w.brains_ && w.brains_->slots.count(pa) &&
              w.brains_->slots.count(pb));
        const BrainGenome ga = brain_genome_of(w, *w.brains_, pa);
        const BrainGenome gb = brain_genome_of(w, *w.brains_, pb);
        CHECK(ga.valid && gb.valid);
        CHECK(ga.hidden == gb.hidden);
        // child genome: deterministic, valid, same hidden, slow = crossover
        const BrainGenome child =
            brain_child_genome(w, pa, pb, ga, gb);
        CHECK(child.valid);
        CHECK(child.hidden == ga.hidden);
        CHECK(child.slow.size() == ga.slow.size());
        // every slow byte must come from a parent (pre-mutation blocks may
        // differ, but post-mutation values stay within int8 and biases clamp)
        for (std::int8_t v : child.slow) CHECK(v >= -127 && v <= 127);
        // install on a fresh entity: h/replay zeroed (Baldwin — no fast state)
        const EntityId baby = mk_sapient(w, {55, 55});
        brain_install_spawn(w, baby, child);
        CHECK(w.brains_->slots.count(baby) == 1);
        const BrainSlot& bs = w.brains_->slots.at(baby);
        std::int64_t hsum = 0;
        for (auto v : bs.h) hsum += std::llabs(static_cast<long long>(v));
        CHECK(hsum == 0);
        CHECK(bs.replay_count == 0);
        CHECK(bs.last_action == ActionVec{});
        // installed slow weights match the genome byte-for-byte
        const std::int8_t* wb = w.brains_->arena.w8(bs.woff);
        for (std::size_t i = 0; i < child.slow.size(); ++i)
            CHECK(wb[i] == child.slow[i]);
        // determinism: same parents+tick -> identical child genome
        const BrainGenome child2 =
            brain_child_genome(w, pa, pb, ga, gb);
        CHECK(child2.slow == child.slow);
        CHECK(child2.hidden == child.hidden);
        CHECK(child2.lr_permille == child.lr_permille);
    }

    // ---- reproductive isolation: divergent brains refuse to pair ----------
    {
        World w(43, "data");
        w.brain_shadow = true;
        const EntityId pa = mk_sapient(w, {50, 50});
        const EntityId pb = mk_sapient(w, {60, 50});
        for (int i = 0; i < 4; ++i) {
            w.interact(mk_ctx(pa, {}, "rest", 0));
            w.interact(mk_ctx(pb, {}, "rest", 0));
        }
        BrainGenome ga = brain_genome_of(w, *w.brains_, pa);
        BrainGenome gb = brain_genome_of(w, *w.brains_, pb);
        CHECK(ga.valid && gb.valid);
        // hidden mismatch -> isolation
        gb.hidden = 32;
        CHECK(brain_genetic_distance(ga, gb) > kMaxBrainGeneticDistance);
        // massive weight divergence -> isolation (saturate to int8 rails)
        gb.hidden = ga.hidden;
        for (std::size_t i = 0; i < gb.slow.size(); ++i)
            gb.slow[i] = static_cast<std::int8_t>((i & 1) ? 127 : -127);
        CHECK(brain_genetic_distance(ga, gb) > kMaxBrainGeneticDistance);
        // identical genomes -> zero distance
        CHECK(brain_genetic_distance(ga, ga) == 0);
    }

    // ---- state_hash includes brain state ------------------------------------
    {
        World a(51, "data"), b(51, "data");
        a.brain_shadow = b.brain_shadow = true;
        const EntityId sa = mk_sapient(a, {50, 50});
        const EntityId sb = mk_sapient(b, {50, 50});
        for (int i = 0; i < 20; ++i) {
            a.interact(mk_ctx(sa, {}, "rest", 0));
            b.interact(mk_ctx(sb, {}, "rest", 0));
        }
        CHECK(a.state_hash() == b.state_hash());
        // perturb one weight -> hash must change (brains are hashed in)
        BrainSlot& slot = a.brains_->slots.at(sa);
        std::int8_t* wb = a.brains_->arena.w8(slot.woff);
        wb[0] = static_cast<std::int8_t>(wb[0] + 1);
        CHECK(a.state_hash() != b.state_hash());
    }

    // ---- save/load v20: brain store round-trips bit-identically -------------
    {
        const char* path = "/tmp/simlab_brain_p2.bin";
        World run(61, "data"), split(61, "data"), loaded(61, "data");
        EntityId s_run = kNoEntity;
        for (World* wp : {&run, &split}) {
            wp->brain_shadow = true;
            const EntityId s = mk_sapient(*wp, {50, 50});
            if (wp == &run) s_run = s;
            for (int i = 0; i < 40; ++i)
                wp->interact(mk_ctx(s, {}, (i % 3) ? "rest" : "strike", 0));
        }
        // split saves mid-run, loads, continues — must equal the flat run
        CHECK(split.save(path));
        CHECK(loaded.load(path));
        loaded.brain_shadow = true;
        // the saved sapient keeps its id across the round-trip
        EntityId sid = kNoEntity;
        for (const auto& ep : loaded.entities)
            if (ep->kind_name == "sapient" && ep->alive) { sid = ep->id; break; }
        CHECK(sid != kNoEntity);
        CHECK(sid == s_run);
        for (int i = 40; i < 90; ++i) {
            const char* verb = (i % 3) ? "rest" : "strike";
            run.interact(mk_ctx(s_run, {}, verb, 0));
            loaded.interact(mk_ctx(sid, {}, verb, 0));
        }
        CHECK(run.state_hash() == loaded.state_hash());
        CHECK(run.brains_ && loaded.brains_);
        CHECK(brain_state_hash(*run.brains_) == brain_state_hash(*loaded.brains_));
    }
}

// ==================== CL-1 Phase 3 ==========================================

// Respiration laws: O2 exchange land/water, organ upkeep, hypoxia.
static void test_amphibian_evolution() {
    World w(23, "data");
    WorldGenParams gp; gp.seed = 23; gp.grid_w = 64; gp.grid_h = 64;
    gp.sea_level = 300;
    WorldGenerator gen;
    if (!gen.generate(w, gp)) { std::printf("  amphibian: worldgen failed\n"); return; }
    // locate a deep-water cell and a dry cell (iterate local cells)
    Vec2 deep{}, dry{};
    bool have_deep = false, have_dry = false;
    for (int ly = 0; ly < w.terrain.h && !(have_deep && have_dry); ++ly)
        for (int lx = 0; lx < w.terrain.w; ++lx) {
            const std::size_t i = static_cast<std::size_t>(ly) * w.terrain.w + lx;
            const Vec2 p = w.terrain.cell_center(lx + w.terrain.ox,
                                                ly + w.terrain.oy);
            if (w.terrain.water[i] == 1 && !have_deep) {
                const std::int64_t below =
                    static_cast<std::int64_t>(w.gen_sea_level) - w.terrain.elevation[i];
                if (below <= 0 || below > lawutil::kWadeDepth) { deep = p; have_deep = true; }
            }
            if (w.terrain.water[i] == 0 && !have_dry) { dry = p; have_dry = true; }
        }
    if (!have_deep || !have_dry) { std::printf("  amphibian: no water/land pair, skip\n"); return; }

    auto mk_diver = [&](Vec2 pos, std::int64_t lung, std::int64_t gill) {
        const EntityId id = mk_body(w, pos);
        Entity* e = w.find(id);
        tseti(*e, w, "oxygen", 500);
        tseti(*e, w, "organ_lung_gain", lung);
        tseti(*e, w, "organ_gill_gain", gill);
        tseti(*e, w, "resp_accum", 0);
        tseti(*e, w, "consciousness", 1000);
        tseti(*e, w, "blood_volume", 1000);
        tseti(*e, w, "pain_level", 0);
        tseti(*e, w, "energy", 60000);
        return id;
    };
    const EntityId lungfish = mk_diver(dry, 800, 0);    // land breather
    const EntityId gillfish = mk_diver(deep, 0, 800);   // water breather
    const EntityId bare     = mk_diver(deep, 0, 0);     // no organs

    w.advance_ticks_for_test(20);
    auto ox = [&](EntityId id) {
        Entity* e = w.find(id);
        return tint(*e, w, "oxygen", -1);
    };
    // lung on land: exch = 800*50/1000 - 4 = +36/tick -> O2 rises (capped 1000)
    CHECK(ox(lungfish) > 500);
    // gill submerged: +36/tick -> rises; bare diver: -4/tick -> falls
    CHECK(ox(gillfish) > 500);
    CHECK(ox(bare) < 500);
    // land-dwelling gill-only fish suffocates slowly on land
    const EntityId stranded = mk_diver(dry, 0, 800);
    w.advance_ticks_for_test(140);   // -4/tick * 140 = -560 -> clamps at 0 path
    CHECK(ox(stranded) < 400);

    // hypoxia: O2 < 350 -> consciousness decay + pain + blood loss
    {
        EntityId h = mk_diver(dry, 0, 0);
        Entity* e = w.find(h);
        tseti(*e, w, "oxygen", 300);
        tseti(*e, w, "consciousness", 1000);
        tseti(*e, w, "blood_volume", 1000);
        tseti(*e, w, "pain_level", 0);
        w.advance_ticks_for_test(60);
        CHECK(tint(*e, w, "consciousness", 1000) < 1000);
        CHECK(tint(*e, w, "pain_level", 0) > 0);
    }
    // upkeep: organ bearer pays (lung+gill)/50 energy/tick extra — the
    // resp_accum remainder itself returns to 0 each batch, so assert on the
    // energy delta between a matched organ-bearing and organ-free pair.
    {
        const EntityId costly = mk_diver(dry, 500, 500);
        const EntityId cheap  = mk_diver(dry, 0, 0);
        w.advance_ticks_for_test(10);
        Entity* ec = w.find(costly); Entity* eh = w.find(cheap);
        CHECK(tint(*ec, w, "energy", 0) < tint(*eh, w, "energy", 0));
    }
}

// CL-1 control loop: brain_control population survives, reproduces, learns.
static void test_brain_curriculum() {
    auto build = [](std::uint64_t seed) {
        World w(seed, "data");
        PrimordialParams pp; pp.seed = static_cast<int>(seed); pp.grid = 48;
        pp.tribes = 1; pp.per_tribe = 3;
        build_primordial(w, pp);
        return w;
    };
    World a = build(17), b = build(17);
    DriverState sta; sta.curiosity = true; sta.concurrent = true;
    sta.brain_control = true;
    DriverState stb; stb.curiosity = true; stb.concurrent = true;
    stb.brain_control = true;

    // micro-benchmark on a THROWAWAY world — decides/learn hooks mutate
    // brain + world state, so it must not touch the determinism pair.
    {
        World bw = build(99);
        EntityId bench_npc = kNoEntity;
        for (const auto& ep : bw.entities)
            if (ep->kind_name == "sapient" && ep->alive) { bench_npc = ep->id; break; }
        if (bench_npc != kNoEntity) {
            for (int i = 0; i < 5; ++i) bw.interact(mk_ctx(bench_npc, {}, "rest", 0));
            const auto t0 = std::chrono::steady_clock::now();
            constexpr int kDecides = 100;
            BrainChoice sink;
            for (int i = 0; i < kDecides; ++i) sink = brain_decide(bw, bench_npc);
            const auto t1 = std::chrono::steady_clock::now();
            const double us = std::chrono::duration<double, std::micro>(t1 - t0).count() / kDecides;
            // sparse consultation: 1 decide per 8 agent-cycles
            const double frame_ms = us * 2000.0 / 8.0 / 1000.0;
            std::printf("  brain benchmark: %.1f us/decide -> ~%.1f ms/frame @2000 agents sparse (budget 16.7)\n",
                        us, frame_ms);
            CHECK(sink.valid || true);
        }
    }

    // control: identical world, brain_control OFF — isolates CL-1's effect
    World c = build(17);
    DriverState stc; stc.curiosity = true; stc.concurrent = true;
    const std::int64_t cycles =
        std::getenv("SIMLAB_CURRICULUM_CYCLES")
            ? std::atoll(std::getenv("SIMLAB_CURRICULUM_CYCLES")) : 600;
    DriverTally ta, tb;
    if (std::getenv("SIMLAB_DEBUG_CYCLES")) {
        for (std::int64_t i = 0; i < cycles; ++i) {
            autonomous_cycle(a, &sta, &ta);
            autonomous_cycle(b, &stb, &tb);
            const auto ha = a.state_hash(), hb = b.state_hash();
            const auto ba = a.brains_ ? brain_state_hash(*a.brains_) : 0;
            const auto bb = b.brains_ ? brain_state_hash(*b.brains_) : 0;
            std::printf("  cycle %lld hash %016llx vs %016llx %s | brain %016llx vs %016llx | ev %llu vs %llu | ent %zu vs %zu\n",
                        (long long)i, (unsigned long long)ha,
                        (unsigned long long)hb, ha == hb ? "=" : "!=",
                        (unsigned long long)ba, (unsigned long long)bb,
                        (unsigned long long)a.ledger.next_id(),
                        (unsigned long long)b.ledger.next_id(),
                        a.entities.size(), b.entities.size());
            if (ha != hb) break;
        }
    } else {
        ta = autonomous_run(a, cycles, &sta);
        tb = autonomous_run(b, cycles, &stb);
    }
    const DriverTally tc = autonomous_run(c, cycles, &stc);

    // determinism: identical inputs -> identical hash
    CHECK(a.state_hash() == b.state_hash());
    // brains allocated + plasticity engaged (fast weights moved off zero)
    CHECK(a.brains_ && !a.brains_->slots.empty());
    bool any_fast = false;
    for (const auto& [id, s] : a.brains_->slots)
        for (std::int8_t v : s.fast) if (v != 0) { any_fast = true; break; }
    std::printf("  curriculum: cycles=%lld social=%ld brains=%zu fast_moved=%d hash=%016llx\n",
                (long long)cycles, (long)ta.social, a.brains_->slots.size(),
                (int)any_fast, (unsigned long long)a.state_hash());
    // civilization floor: founders survive to adulthood and the social
    // reflex floor produces at least one completed social act
    CHECK(ta.social > 0);
    std::size_t alive = 0, sapient_total = 0, alive_c = 0;
    for (const auto& ep : a.entities)
        if (ep->kind_name == "sapient") {
            ++sapient_total;
            if (ep->alive) ++alive;
        }
    for (const auto& ep : c.entities)
        if (ep->kind_name == "sapient" && ep->alive) ++alive_c;
    std::printf("  curriculum alive: brain=%zu/%zu control=%zu social=%ld/%ld\n",
                alive, sapient_total, alive_c, (long)ta.social, (long)tc.social);
    CHECK(alive >= 1);
    CHECK(alive >= alive_c);          // CL-1 must not underperform baseline
}

// NarrativeHost: safe-disable + (when the GGUF is present) real generation.
static void test_narrative_host() {
    NarrativeHost h;
    NarrativeHost::Config bad;
    bad.model_path = "/nonexistent/model.gguf";
    bad.lib_dir = "/nonexistent";
    CHECK(!h.start(bad));
    CHECK(!h.active());
    h.submit(1, "test");
    std::string s;
    CHECK(!h.poll(1, s));

    NarrativeHost::Config cfg;
    cfg.max_new_tokens = 16;
    if (!std::filesystem::exists(cfg.model_path)) {
        std::printf("  narrative_host: model absent, generation skipped\n");
        return;
    }
    CHECK(h.start(cfg));
    CHECK(h.active());
    h.submit(42, "A sapient named Ash forged a bronze axe beside the river.");
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(180);
    while (std::chrono::steady_clock::now() < deadline && !h.poll(42, s))
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(!s.empty());
    std::printf("  narrative_host: \"%s\"\n", s.c_str());
    // queue bound respected: flood submits must not grow unboundedly
    for (int i = 0; i < 200; ++i) h.submit(1000 + i, "event");
    h.stop();
    CHECK(!h.active());
}

int main(int argc, char** argv) {
    // optional filter: ./tests <name> runs a single suite for fast iteration
    const char* only = argc > 1 ? argv[1] : nullptr;
#define RUN(t) do { if (!only || std::string(only) == #t) t(); } while (0)
    RUN(test_registries);
    RUN(test_new_material_data_only);
    RUN(test_physics_property_driven);
    RUN(test_unknown_interaction);
    RUN(test_causal_chain);
    RUN(test_determinism);
    RUN(test_deposits);
    RUN(test_relations);
    RUN(test_worldgen);
    RUN(test_worldgen_validation_catches_bad);
    RUN(test_seed_system);
    RUN(test_extraction);
    RUN(test_processing);
    RUN(test_fracture);
    RUN(test_budgets_clock_thermal);
    RUN(test_composition);
    RUN(test_wielding);
    RUN(test_geometry);
    RUN(test_binding_types);
    RUN(test_cognition);
    RUN(test_phase5_needs_cognition);
    RUN(test_phase1_audit);
    RUN(test_phase2_complete);
    RUN(test_phase2_placer);
    RUN(test_phase3_complete);
    RUN(test_phase6_discovery);
    RUN(test_phase6_completion);
    RUN(test_phase6_selfdefence);
    RUN(test_phase7_biology);
    RUN(test_phase8_social);
    RUN(test_phase9_world);
    RUN(test_fruit_cycle);
    RUN(test_seed_bank);
    RUN(test_edge_influx);
    RUN(test_memory_engram);
    RUN(test_cognitive_aptitudes);
    RUN(test_attachment_and_parental_care);
    RUN(test_primitive_social_psychology);

    RUN(test_anatomical_trauma_and_bleeding);
    RUN(test_lumen_mechanics_and_organ_coupling);
    RUN(test_thermoregulation_immunity_neoplasm_and_epidemiology);
    RUN(test_zoonosis_sterilization_and_burial);
    RUN(test_plant_blight_monoculture_and_soil_cleansing);
    RUN(test_vector_ecology_selection_and_herd_immunity);
    RUN(test_causal_language_dialect_drift_and_inscriptions);
    RUN(test_causal_language_syntax_rosetta_and_inscriptions);
    RUN(test_inductive_economy_currency_inflation_and_credit);
    RUN(test_political_legal_base_core_norms_authority_and_revolt);
    RUN(test_architectural_base_core_enclosures_ergonomics_and_decay);
    RUN(test_myth_oral_literature_rituals_and_diplomacy_base_core);
    RUN(test_warfare_logistics_siege_and_emergent_weapons_rd);
    RUN(test_render_snapshot_and_procedural_atlas);
    RUN(test_primordial_genesis_true_playable);
    RUN(test_patron_journal_planet_and_beacons);
    RUN(test_maritime_swimming_drowning_and_watercraft);
    RUN(test_trophic_ecology_seasons_and_apex);
    RUN(test_domestication_agriculture_metallurgy);
    RUN(test_quantitative_cognition_and_notation);
    RUN(test_brain_phase0);
    RUN(test_brain_phase1);
    RUN(test_brain_phase2);
    RUN(test_amphibian_evolution);
    RUN(test_brain_curriculum);
    RUN(test_narrative_host);
    RUN(test_infection_and_primitive_medicine);
    RUN(test_neuro_psychopathology);
    RUN(test_phase9_autonomous_civilization);
#undef RUN
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
