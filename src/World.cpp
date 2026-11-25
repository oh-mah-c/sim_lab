#include "simlab/World.hpp"
#include "simlab/WorldGen.hpp"   // kGeneratorVersion for save compatibility
#include "simlab/Geometry.hpp"
#include "simlab/Cognition.hpp"
#include "simlab/Discovery.hpp"
#include "LawUtil.hpp"
#include <algorithm>
#include <unordered_set>
#include <cctype>
#include <limits>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <cstring>

namespace simlab {

namespace {
// literal names (static storage) so PropId caches can key on the address
constexpr const char* kImmuneSigNames[8] = {
    "immune_sig0", "immune_sig1", "immune_sig2", "immune_sig3",
    "immune_sig4", "immune_sig5", "immune_sig6", "immune_sig7"};
}  // namespace
namespace {
// Phase 11 props that make an entity worth a metabolize pass even when it
// has no need/metabolism props: corpse rot bookkeeping, standing-water
// fouling, strain stamps on reservoirs, symbiont templates, terminal flags.
const char* const kEnvPropNames[] = {
    "corpse_decay_level", "corpse_decay_accum", "buried",
    "stagnation_level", "stagnation_accum", "reservoir_spread_accum",
    "strain_ref", "symbiont_signature", "cremated",
    "soil_spore_load", "soil_nutrient", "spore_decay_accum",
    "yield_penalty", "blight_marks",
    "is_disease_vector", "vector_dormant", "vector_bite_accum",
    "vector_breed_accum", "vector_bite_rolls",
    "stagnation_hist_level", "stagnation_hist_acc",
    "stagnation_hist_rate", "stagnation_hist_start",
    "language_name", "dialect_drift_accum", "dialect_drift_rate",
    "dialect_drift_iso", "dialect_drift_rolls", "dialect_drift_threshold",
    "erosion_level", "erosion_accum", "inscription_durability"};
}  // namespace


namespace {
bool file_exists(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}
}

const Value* Entity::prop(const World& w, PropId p) const {
    return w.prop_of(*this, p);
}

const std::int64_t* Genome::get(PropId p) const {
    for (const auto& [pid, v] : loci)
        if (pid == p) return &v;
    return nullptr;
}
void Genome::set(PropId p, std::int64_t v) {
    auto it = std::lower_bound(loci.begin(), loci.end(), p,
        [](const std::pair<PropId, std::int64_t>& e, PropId k) { return e.first < k; });
    if (it != loci.end() && it->first == p) it->second = v;
    else loci.insert(it, {p, v});
}

std::uint32_t EntityRegistry::kind_id(const std::string& name) {
    const auto it = by_name_.find(name);
    if (it != by_name_.end()) return it->second;
    const std::uint32_t id = static_cast<std::uint32_t>(names_.size() + 1);
    names_.push_back(name);
    by_name_[name] = id;
    return id;
}
const std::string* intern_kind(std::string_view s) {
    // node-based set: element addresses never move, entries never erased
    static std::unordered_set<std::string> pool;
    auto it = pool.find(std::string(s));
    if (it == pool.end()) it = pool.emplace(s).first;
    return &*it;
}

const std::vector<EntityId>& World::entities_of_kind(const KindName& k) const {
    static const std::vector<EntityId> kEmpty;
    const auto it = by_kind_.find(k.interned());
    return it == by_kind_.end() ? kEmpty : it->second;
}

const std::string* EntityRegistry::kind_name(std::uint32_t id) const {
    if (id == 0 || id > names_.size()) return nullptr;
    return &names_[id - 1];
}

World::World(std::uint64_t s, const std::string& data_dir) : seed(s) {
    laws.push_back(std::make_unique<PhysicsModel>());
    laws.push_back(std::make_unique<ExtractionModel>());
    laws.push_back(std::make_unique<ProcessingModel>());
    laws.push_back(std::make_unique<ThermalModel>());
    laws.push_back(std::make_unique<CompositionModel>());
    laws.push_back(std::make_unique<PhysiologyModel>());
    laws.push_back(std::make_unique<TreatModel>());
    laws.push_back(std::make_unique<CombineModel>());
    laws.push_back(std::make_unique<BiologyModel>());
    laws.push_back(std::make_unique<SocialModel>());
    laws.push_back(std::make_unique<LocomotionModel>());
    laws.push_back(std::make_unique<WarfareModel>());
    load_content(data_dir, &content_error);
}

bool World::load_content(const std::string& data_dir, std::string* err) {
    // All-or-nothing: parse into temporaries, commit only if every file is valid.
    PropertyRegistry tp; MaterialRegistry tm; DepositRegistry td; ProcessRegistry tpr;
    SpeciesRegistry tsp;
    std::string e;
    bool ok = tp.load_file(data_dir + "/properties.txt", &e) &&
              tm.load_file(data_dir + "/materials.txt", tp, &e);
    // deposits.txt / processes.txt are optional: a world may have none.
    const std::string dep_path = data_dir + "/deposits.txt";
    if (ok && file_exists(dep_path)) ok = td.load_file(dep_path, tp, tm, &e);
    const std::string proc_path = data_dir + "/processes.txt";
    if (ok && file_exists(proc_path)) ok = tpr.load_file(proc_path, tm, &e);
    const std::string sp_path = data_dir + "/species.txt";
    if (ok && file_exists(sp_path)) ok = tsp.load_file(sp_path, tp, tm, &e);
    InfluxRegistry tin;
    const std::string inf_path = data_dir + "/influx.txt";
    if (ok && file_exists(inf_path)) ok = tin.load_file(inf_path, tp, tm, &e);
    if (!ok) {
        if (err) *err = e;
        content_error = e;
        return false;
    }
    props = std::move(tp); materials = std::move(tm);
    deposits = std::move(td); processes = std::move(tpr); species = std::move(tsp);
    influx = std::move(tin);
    content_error.clear();
    return true;
}

// Phase 10 anatomy: spawn a species body plan as contained part_of
// children. Parts share the body's position (contained entities do not
// locomote independently) and carry integrity 1000 + their role props.
bool World::build_anatomy(EntityId body) {
    Entity* e = find(body);
    if (!e || !e->alive) return false;
    const SpeciesDef* sp = species.find_name(e->kind_name);
    if (!sp || sp->anatomy.empty()) return false;
    // already has parts? (idempotent for re-loads / double calls)
    for (const Relation& r : relations_of(body, "part_of"))
        if (r.b == body) return false;
    const PropId contained = SIMLAB_PID(props, "contained");
    const PropId integ = SIMLAB_PID(props, "integrity");
    const PropId slot = SIMLAB_PID(props, "body_slot");
    const PropId role = SIMLAB_PID(props, "part_role");
    const PropId vital = SIMLAB_PID(props, "vital_organ");
    const PropId bleed = SIMLAB_PID(props, "bleed_rate");
    const PropId frac = SIMLAB_PID(props, "fracture_level");
    const PropId sev = SIMLAB_PID(props, "severed");
    const PropId psz = SIMLAB_PID(props, "part_size");
    const PropId inf = SIMLAB_PID(props, "infection_level");
    const PropId infa = SIMLAB_PID(props, "infection_accum");
    const PropId nec = SIMLAB_PID(props, "necrotic");
    const PropId band = SIMLAB_PID(props, "bandaged_integrity");
    for (const PartSpec& ps : sp->anatomy) {
        PropertyBag pb;
        if (contained != kNoProp) pb.set(contained, Value::boolean(true));
        if (integ != kNoProp) pb.set(integ, Value::integer(1000));
        if (slot != kNoProp) pb.set(slot, Value::text(ps.slot));
        if (role != kNoProp) pb.set(role, Value::text(ps.role));
        if (vital != kNoProp) pb.set(vital, Value::integer(ps.vital));
        if (bleed != kNoProp) pb.set(bleed, Value::integer(0));
        if (frac != kNoProp) pb.set(frac, Value::integer(0));
        if (sev != kNoProp) pb.set(sev, Value::integer(0));
        if (psz != kNoProp) pb.set(psz, Value::integer(ps.size));
        if (inf != kNoProp) pb.set(inf, Value::integer(0));
        if (infa != kNoProp) pb.set(infa, Value::integer(0));
        if (nec != kNoProp) pb.set(nec, Value::integer(0));
        if (band != kNoProp) pb.set(band, Value::integer(0));
        for (const auto& [xpid, xv] : ps.extra.entries) pb.set(xpid, xv);
        const EntityId pid = create_entity(
            e->kind_name + "/" + ps.name, ps.material, e->pos, pb);
        if (pid == kNoEntity) continue;
        relations.relate(pid, "part_of", body);
    }
    return true;
}

EntityId World::create_entity(const std::string& kind_name, MaterialId material,
                              Vec2 pos, const PropertyBag& overrides) {
    // validate BEFORE touching registries: a rejected create must not burn an
    // id or register a kind (would desync ids on save/load replay).
    for (const auto& [pid, v] : overrides.entries) {
        const PropertyDef* d = props.find(pid);
        if (!d || !props.value_ok(*d, v)) return kNoEntity;
    }
    {   // property dependencies (spec §4): a dependent prop needs its prerequisite
        std::vector<PropId> present;
        for (const auto& [pid, v] : overrides.entries) present.push_back(pid);
        if (material != kNoMaterial)
            if (const MaterialDef* m = materials.find(material))
                for (const auto& [pid, v] : m->props) present.push_back(pid);
        std::sort(present.begin(), present.end());
        if (!props.unmet_dependencies(present).empty()) return kNoEntity;
    }
    auto e = std::make_unique<Entity>();
    const std::uint32_t kid = kinds.kind_id(kind_name);
    e->id = kinds.allocate(kid);
    e->kind = kid;                        // true kind id (id's top byte wraps at 256 kinds)
    e->kind_name = kind_name;
    e->material = material;
    e->pos = pos;
    e->props = overrides;
    e->last_metabolized = clock_.tick();
    const EntityId id = e->id;
    index_[id] = entities.size();
    by_kind_[e->kind_name.interned()].push_back(id);
    entities.push_back(std::move(e));
    spatial.insert(id, pos);
    return id;
}

Entity* World::find(EntityId id) {
    const auto it = index_.find(id);
    return it == index_.end() ? nullptr : entities[it->second].get();
}
const Entity* World::find(EntityId id) const {
    const auto it = index_.find(id);
    return it == index_.end() ? nullptr : entities[it->second].get();
}
std::vector<EntityId> World::entities_near(Vec2 p, std::int64_t radius) const {
    return spatial.within(p, radius);
}
EntityId World::create_deposit(const std::string& name, Vec2 pos, bool outcrop) {
    const DepositDef* d = deposits.find_name(name);
    if (!d) return kNoEntity;
    PropertyBag overrides = d->props;
    for (const auto& [pid, v] : d->evidence.entries) overrides.set(pid, v);   // visible signs
    // Lifecycle props seeded from deposit data: remaining reserve + exposure.
    if (const PropertyDef* pdr = props.find_name("deposit_remaining")) {
        std::int64_t reserve = 1000;
        if (const PropertyDef* pres = props.find_name("reserve"))
            if (const Value* rv = d->props.get(pres->id)) reserve = rv->i;
        overrides.set(pdr->id, Value::integer(reserve));
    }
    if (const PropertyDef* pex = props.find_name("exposed"))
        overrides.set(pex->id, Value::boolean(d->surface || outcrop));
    const EntityId id = create_entity("deposit", kNoMaterial, pos, overrides);
    if (id == kNoEntity) return kNoEntity;
    if (Entity* e = find(id)) e->deposit = d->id;
    return id;
}

void World::relate(EntityId a, const std::string& type, EntityId b) {
    const Entity* ea = find(a); const Entity* eb = find(b);
    if (!ea || !eb || !ea->alive || !eb->alive) return;   // no dangling edges
    relations.relate(a, type, b);
}
bool World::related(EntityId a, const std::string& type, EntityId b) const {
    return relations.related(a, type, b);
}
std::vector<Relation> World::relations_of(EntityId id) const {
    return relations.relations_of(id);
}
std::vector<Relation> World::relations_of(EntityId id, const std::string& type) const {
    return relations.relations_of(id, type);
}

void World::destroy_entity(EntityId id) {
    if (Entity* e = find(id)) e->alive = false;
    spatial.erase(id);
}

const Value* World::prop_of(const Entity& e, PropId p) const {
    if (const Value* v = e.props.get(p)) return v;
    if (e.material != kNoMaterial) {
        if (const MaterialDef* m = materials.find(e.material)) {
            const auto it = m->props.find(p);
            if (it != m->props.end()) return &it->second;
        }
    }
    return nullptr;
}

std::int64_t World::prop_int(const Entity& e, PropId p, std::int64_t fallback) const {
    const Value* v = prop_of(e, p);
    if (!v) return fallback;
    if (v->type == ValType::Ref) return static_cast<std::int64_t>(v->target);
    if (v->type == ValType::Integer || v->type == ValType::Fixed) return v->i;
    if (v->type == ValType::Boolean) return v->b ? 1 : 0;
    return fallback;
}

void World::add_law_model(std::unique_ptr<INaturalLawModel> m) {
    laws.push_back(std::move(m));
}

World World::clone() const {
    World out;
    out.entities.reserve(entities.size());   // reuse-friendly bulk copy
    out.seed = seed;
    out.world_seed = world_seed;
    out.generator_version = generator_version;
    out.gen_origin_x = gen_origin_x;
    out.gen_origin_y = gen_origin_y;
    out.clock_ = clock_;
    out.props = props;
    out.materials = materials;
    out.deposits = deposits;
    out.processes = processes;
    out.species = species;
    out.influx = influx;
    out.gen_sea_level = gen_sea_level;
    out.ruleset = ruleset;
    out.relations = relations;
    out.social_ = social_;
    out.kinds = kinds;
    out.ledger = ledger;
    out.spatial = spatial;
    out.terrain = terrain;
    out.subsurface = subsurface;
    out.gen_spawned_ = gen_spawned_;
    out.gen_cells_ = gen_cells_;
    for (const auto& e : entities)
        out.entities.push_back(std::make_unique<Entity>(*e));
    out.laws = laws;   // law models are stateless: share, don't re-allocate
    for (const auto& [name, r] : rngs_)
        out.rngs_[name] = std::make_unique<Rng>(*r);
    out.minds_ = minds_;
    out.index_ = index_;
    out.by_kind_ = by_kind_;
    out.touched_ = touched_;
    out.content_error = content_error;
    out.speculative = speculative;
    return out;
}

// ---- speculative local copy ---------------------------------------------------
// Fog-of-war planning (Phase 9): plan_multi() clones the world once per
// search root and once per expanded candidate. Cloning the WHOLE map —
// every entity ever spawned plus the ever-growing causal ledger — is what
// made long autonomous runs OOM. The local copy keeps only what the
// thinking agent can possibly interact with: entities inside its sight
// radius, entities structurally linked to them (composite parts, wielded
// tools), its own mind, relations between copied entities, and the
// terrain/subsurface cells covering the radius. Entity ids are preserved
// so minds/relations/ledger references stay meaningful inside the clone.
// The ledger is empty: speculation needs physics, not history — event ids
// restart at 1 and stay self-consistent within the clone's lifetime.
World World::clone_local(Vec2 center, std::int64_t radius, EntityId focus) const {
    World out;
    out.seed = seed;
    out.world_seed = world_seed;
    out.generator_version = generator_version;
    out.gen_origin_x = gen_origin_x;
    out.gen_origin_y = gen_origin_y;
    out.clock_ = clock_;
    out.props = props;
    out.materials = materials;
    out.deposits = deposits;
    out.processes = processes;
    out.species = species;
    out.influx = influx;
    out.gen_sea_level = gen_sea_level;
    out.ruleset = ruleset;
    out.kinds = kinds;
    out.spatial = SpatialIndex{1024};
    out.laws = laws;   // law models are stateless: share, don't re-allocate
    for (const auto& [name, r] : rngs_)
        out.rngs_[name] = std::make_unique<Rng>(*r);
    out.content_error = content_error;

    // ---- entity selection: radius + structural closure ----------------------
    const std::int64_t r2 = radius > 0 ? radius * radius : 0;
    std::set<EntityId> keep;
    for (const auto& e : entities) {
        if (!e->alive) continue;
        if (e->id == focus || (r2 > 0 && dist2(center, e->pos) <= r2))
            keep.insert(e->id);
    }
    // part_of/wielding closure: a composite's parts and wielded tools must
    // travel with their carrier or the structure is broken inside the clone.
    // Fixpoint over the (sorted) edge set — bounded by entity count.
    for (bool grew = true; grew;) {
        grew = false;
        for (const Relation& rel : relations.edges()) {
            if (rel.type != "part_of" && rel.type != "wielding") continue;
            const bool ai = keep.count(rel.a) != 0, bi = keep.count(rel.b) != 0;
            if (ai == bi) continue;
            const EntityId miss = ai ? rel.b : rel.a;
            const Entity* me = find(miss);
            if (me && me->alive && !keep.count(miss)) {
                keep.insert(miss); grew = true;
            }
        }
    }
    out.entities.reserve(keep.size());
    std::size_t idx = 0;
    for (const auto& e : entities) {
        if (!keep.count(e->id)) continue;
        out.entities.push_back(std::make_unique<Entity>(*e));
        out.index_[e->id] = idx++;
        out.by_kind_[e->kind_name.interned()].push_back(e->id);
        out.spatial.insert(e->id, e->pos);
    }
    // relations + social ties only between copied entities (no dangling edges)
    for (const Relation& r : relations.edges())
        if (keep.count(r.a) && keep.count(r.b)) out.relations.relate(r.a, r.type, r.b);
    for (const auto& [rel, wt] : social_)
        if (keep.count(rel.a) && keep.count(rel.b)) out.social_[rel] = wt;
    // only the focusing agent's mind — other agents' knowledge is irrelevant
    // to this speculation and copying every mind is part of the OOM cost.
    if (const auto it = minds_.find(focus); it != minds_.end())
        out.minds_[focus] = it->second;

    // ---- terrain/subsurface: sub-rect of global cells covering the radius --
    auto copy_fields = [&](const TerrainGrid& src, TerrainGrid& dst,
                           int gx0, int gy0, int gx1, int gy1) {
        gx0 = std::max(gx0, src.ox); gy0 = std::max(gy0, src.oy);
        gx1 = std::min(gx1, src.ox + src.w - 1);
        gy1 = std::min(gy1, src.oy + src.h - 1);
        if (gx0 > gx1 || gy0 > gy1) return;
        dst.cell = src.cell; dst.ox = gx0; dst.oy = gy0;
        dst.w = gx1 - gx0 + 1; dst.h = gy1 - gy0 + 1;
        const std::size_t n = static_cast<std::size_t>(dst.w) * dst.h;
        auto cp = [&](const std::vector<std::int32_t>& s,
                      std::vector<std::int32_t>& d) {
            d.assign(n, 0);
            for (int ly = 0; ly < dst.h; ++ly)
                for (int lx = 0; lx < dst.w; ++lx) {
                    const int sx = gx0 + lx - src.ox, sy = gy0 + ly - src.oy;
                    d[static_cast<std::size_t>(ly) * dst.w + lx] =
                        s[static_cast<std::size_t>(sy) * src.w + sx];
                }
        };
        cp(src.elevation, dst.elevation); cp(src.moisture, dst.moisture);
        cp(src.temperature, dst.temperature); cp(src.slope, dst.slope);
        cp(src.wind_x, dst.wind_x); cp(src.wind_y, dst.wind_y);
        cp(src.water_table, dst.water_table);
        if (src.traffic_wear.size() == src.elevation.size())
            cp(src.traffic_wear, dst.traffic_wear);
        else dst.traffic_wear.assign(n, 0);
        dst.water.assign(n, 0);
        for (int ly = 0; ly < dst.h; ++ly)
            for (int lx = 0; lx < dst.w; ++lx) {
                const int sx = gx0 + lx - src.ox, sy = gy0 + ly - src.oy;
                dst.water[static_cast<std::size_t>(ly) * dst.w + lx] =
                    src.water[static_cast<std::size_t>(sy) * src.w + sx];
            }
        dst.vegetation.assign(n, 0);
        for (int ly = 0; ly < dst.h; ++ly)
            for (int lx = 0; lx < dst.w; ++lx) {
                const int sx = gx0 + lx - src.ox, sy = gy0 + ly - src.oy;
                dst.vegetation[static_cast<std::size_t>(ly) * dst.w + lx] =
                    src.vegetation[static_cast<std::size_t>(sy) * src.w + sx];
            }
    };
    if (!terrain.empty() && terrain.cell > 0) {
        const int cs = terrain.cell;
        const std::int64_t rr = radius > 0 ? radius : 512;
        const int x0 = static_cast<int>(center.x - rr), x1 = static_cast<int>(center.x + rr);
        const int y0 = static_cast<int>(center.y - rr), y1 = static_cast<int>(center.y + rr);
        // +1 cell margin: slope/near_water read neighbours of border cells
        copy_fields(terrain, out.terrain,
                    TerrainGrid::cell_of(x0, cs) - 1, TerrainGrid::cell_of(y0, cs) - 1,
                    TerrainGrid::cell_of(x1, cs) + 1, TerrainGrid::cell_of(y1, cs) + 1);
    }
    if (!subsurface.empty()) {
        const SubsurfaceGrid& src = subsurface;
        int gx0 = src.ox, gy0 = src.oy, gx1 = src.ox + src.w - 1, gy1 = src.oy + src.h - 1;
        if (!terrain.empty() && terrain.cell > 0) {
            const int cs = terrain.cell;
            const std::int64_t rr = radius > 0 ? radius : 512;
            const int x0 = static_cast<int>(center.x - rr), x1 = static_cast<int>(center.x + rr);
            const int y0 = static_cast<int>(center.y - rr), y1 = static_cast<int>(center.y + rr);
            gx0 = TerrainGrid::cell_of(x0, cs) - 1; gy0 = TerrainGrid::cell_of(y0, cs) - 1;
            gx1 = TerrainGrid::cell_of(x1, cs) + 1; gy1 = TerrainGrid::cell_of(y1, cs) + 1;
        }
        gx0 = std::max(gx0, src.ox); gy0 = std::max(gy0, src.oy);
        gx1 = std::min(gx1, src.ox + src.w - 1); gy1 = std::min(gy1, src.oy + src.h - 1);
        if (gx0 <= gx1 && gy0 <= gy1) {
            SubsurfaceGrid& dst = out.subsurface;
            dst.w = gx1 - gx0 + 1; dst.h = gy1 - gy0 + 1;
            dst.ox = gx0; dst.oy = gy0; dst.max_depth = src.max_depth;
            dst.cells.assign(static_cast<std::size_t>(dst.w) * dst.h, {});
            for (int ly = 0; ly < dst.h; ++ly)
                for (int lx = 0; lx < dst.w; ++lx) {
                    const int sx = gx0 + lx - src.ox, sy = gy0 + ly - src.oy;
                    dst.cells[static_cast<std::size_t>(ly) * dst.w + lx] =
                        src.cells[static_cast<std::size_t>(sy) * src.w + sx];
                }
        }
    }
    return out;
}

void World::advance_ticks(std::int64_t n) {
    for (std::int64_t i = 0; i < n; ++i) clock_.advance();
    if (n <= 0) return;
    if (speculative) {
        // planning copy (see World.hpp): only the THINKING agent's body runs
        // forward — its needs/anatomy/infection catch up (flush_needs; other
        // participants are flushed by interact() itself before prediction),
        // plus heat physics and its own memory decay. The rest of the local
        // world stays frozen for the few ticks a plan spans.
        flush_needs(minded_entities());
        dissipate_heat(n);
        // Primordial worlds: an imagined future does not forget. Decaying the
        // thinker's engrams here forced a copy-on-write of its WHOLE memory
        // map in every speculative clone (thousands of snapshots each), which
        // spiked RSS by hundreds of MB on planet-sized maps. Legacy worlds keep
        // the original behaviour (and their exact state_hash).
        if (!(ruleset & kRulePrimordial)) decay_minds(n);
        return;
    }
    metabolize(n); dissipate_heat(n); ecology_tick(n);
    language_tick(clock_.tick()); decay_minds(n); political_drift(n); myth_drift(n);
    terrain_wear_decay(n);
}

// Need drift (TODO §10.1): hunger/thirst/fatigue grow with `metabolism`.
// This is World Truth change — it is committed as a causal event so the
// ledger explains WHY a need changed, exactly like any interaction.
World::NeedProps World::need_props() const {
    NeedProps np;
    static thread_local SeqPidCache pid_c_;
    SeqPid pid(pid_c_, props);
    np.metabolism = pid("metabolism"); np.hunger = pid("hunger");
    np.thirst = pid("thirst");       np.fatigue = pid("fatigue");
    np.age = pid("age");             np.integrity = pid("integrity");
    np.quantity = pid("quantity");   np.regrow_rate = pid("regrow_rate");
    np.regrow_credit = pid("regrow_credit");
    np.max_quantity = pid("max_quantity");
    np.fruit_decay_rate = pid("fruit_decay_rate");
    np.fruit_count = pid("fruit_count");   np.max_fruit = pid("max_fruit");
    np.fruit_regrow_rate = pid("fruit_regrow_rate");
    np.fruit_spoil_rate = pid("fruit_spoil_rate");
    np.fruit_credit = pid("fruit_credit");
    np.decay_rate = pid("decay_rate");     np.decay_credit = pid("decay_credit");
    np.energy = pid("energy");
    np.mind_horizon = pid("mind_plan_horizon");
    np.mind_abstraction = pid("mind_abstraction_gain");
    np.mind_brain_cost = pid("mind_brain_metabolic_cost");
    np.brain_accum = pid("mind_brain_accum");
    np.yield_penalty = pid("yield_penalty");
    np.soil_nutrient = pid("soil_nutrient");
    np.growth = pid("growth");           np.growth_ticks = pid("growth_ticks");
    np.irrigation_credit = pid("irrigation_credit");
    return np;
}

// ---- Phase 17: seasons, trophic ranges & flora seasonality -----------------
// Clock contract: 1 tick = 1 hour, 24 ticks/day, 28 days/season -> a season
// is exactly 672 ticks and the year 2688, so season index = (tick/672)%4.
static std::int64_t eco_season(std::uint64_t tick) {
    return static_cast<std::int64_t>((tick / 672) % 4);
}
// Seasonal climate wave applied to RANGE decisions only: winter bites the
// growing bands, summer pushes warmth poleward. deci-°C offset added to
// static terrain temperature — physiology/vector dormancy keep raw fields.
static std::int64_t eco_season_temp(std::int64_t s) {
    switch (s) { case 0: return 0; case 1: return 40; case 2: return -20;
                 default: return -90; }
}
// Ticks spent in each season over [from,now] — at most ~n/672+1 segments,
// so even a huge dormant catch-up stays cheap and exact.
static void eco_season_counts(std::uint64_t from, std::uint64_t now,
                              std::int64_t c[4]) {
    c[0] = c[1] = c[2] = c[3] = 0;
    for (std::uint64_t p = from; p <= now;) {
        const std::int64_t s = eco_season(p);
        std::uint64_t end = ((p / 672) + 1) * 672 - 1;
        if (end > now) end = now;
        c[s] += static_cast<std::int64_t>(end - p + 1);
        p = end + 1;
    }
}
// Diurnal/nocturnal/crepuscular activity gate on the tick's hour.
static bool eco_phase_active(std::int64_t phase, std::int64_t hour) {
    switch (phase) {
        case 1: return hour >= 6 && hour < 18;                 // diurnal
        case 2: return hour >= 18 || hour < 6;                 // nocturnal
        case 3: return (hour >= 5 && hour < 8) ||
                       (hour >= 17 && hour < 20);             // crepuscular
        default: return true;
    }
}
// Season-weighted regrowth permille for a declared growing_mask
// (bit s = season s); dormant seasons scale by `dormant`. Linear in the
// window, so batched dormant catch-up is bit-identical to per-tick.
static std::int64_t eco_growth_permille(std::int64_t mask, std::int64_t dormant,
                                        std::uint64_t from, std::uint64_t now) {
    if (from > now) return 1000;
    std::int64_t c[4];
    eco_season_counts(from, now, c);
    std::int64_t total = 0;
    for (int s = 0; s < 4; ++s)
        total += c[s] * (((mask >> s) & 1) ? 1000 : dormant);
    return total / static_cast<std::int64_t>(now - from + 1);
}

// Per-entity need drift for dt ticks — the SAME math whether the entity is
// updated every tick (active LOD) or caught up in one batch (dormant LOD):
// linear needs, capped at 1000, starvation counted per-tick across the
// window so a mid-window threshold crossing damages exactly the starving
// ticks. integrity 0 reaps immediately inside the same causal event.
bool World::drift_needs(Entity& e, std::uint64_t dt_u, const NeedProps& np,
                        CausalEvent& ev, std::uint64_t now) {
    if (dt_u == 0 || !e.alive) return false;
    const std::int64_t dt = static_cast<std::int64_t>(dt_u);
    // Phase 17: a declared growing_mask (species data, bits = seasons)
    // scales flora regrowth; off-seasons run at dormant_growth_permille.
    // The window is season-weighted so batched catch-up stays exact.
    std::int64_t grow_eff = 1000;
    {
        const PropId p_gmask = SIMLAB_PID(props, "growing_mask");
        if (p_gmask != kNoProp && prop_of(e, p_gmask)) {
            const PropId p_dorm = SIMLAB_PID(props, "dormant_growth_permille");
            const std::uint64_t from = dt_u <= now ? now - dt_u + 1 : 0;
            grow_eff = eco_growth_permille(prop_int(e, p_gmask, 15),
                p_dorm != kNoProp ? prop_int(e, p_dorm, 400) : 400, from, now);
        }
    }
    constexpr std::int64_t kDefaultMetabolism = 2000;
    bool touched = false;
    auto bump = [&](PropId p, std::int64_t rate) {
        if (p == kNoProp || !prop_of(e, p)) return;
        const std::int64_t cur = prop_int(e, p, 0);
        const std::int64_t nxt = std::min<std::int64_t>(1000, cur + rate * dt);
        if (nxt == cur) return;
        StateChange ch;
        ch.entity = e.id; ch.prop = p; ch.had_before = true;
        ch.before = Value::integer(cur); ch.after = Value::integer(nxt);
        e.props.set(p, Value::integer(nxt));
        ev.changes.push_back(ch);
        touched = true;
    };
    if (np.age != kNoProp && !e.genome.loci.empty()) {
        const std::int64_t cur = prop_int(e, np.age, 0);
        const std::int64_t nxt = cur + dt;
        if (nxt != cur) {
            StateChange ch;
            ch.entity = e.id; ch.prop = np.age; ch.had_before = cur != 0;
            ch.before = Value::integer(cur); ch.after = Value::integer(nxt);
            e.props.set(np.age, Value::integer(nxt));
            ev.changes.push_back(ch);
            touched = true;
        }
    }
    const bool has_need =
        (np.hunger != kNoProp && prop_of(e, np.hunger)) ||
        (np.thirst != kNoProp && prop_of(e, np.thirst)) ||
        (np.fatigue != kNoProp && prop_of(e, np.fatigue));
    const std::int64_t met = np.metabolism != kNoProp
        ? prop_int(e, np.metabolism, kDefaultMetabolism) : 0;
    // window-start need values: starvation threshold crossing is counted
    // per-tick, so pre-drift values are required.
    const bool has_h = np.hunger != kNoProp && prop_of(e, np.hunger);
    const bool has_t = np.thirst != kNoProp && prop_of(e, np.thirst);
    const std::int64_t h_start = has_h ? prop_int(e, np.hunger, 0) : 0;
    const std::int64_t t_start = has_t ? prop_int(e, np.thirst, 0) : 0;
    if (has_need && met > 0) {
        bump(np.hunger, met / 1000);
        bump(np.thirst, met / 500);
        bump(np.fatigue, met / 2000);
        if (np.integrity != kNoProp) {
            const std::int64_t rh = met / 1000, rt = met / 500;
            auto first_starving = [&](bool has, std::int64_t cur,
                                      std::int64_t rate) {
                if (!has) return std::numeric_limits<std::int64_t>::max();
                if (cur >= 950) return std::int64_t{1};
                if (rate <= 0) return std::numeric_limits<std::int64_t>::max();
                return (950 - cur + rate - 1) / rate;    // ceil, 1-indexed
            };
            const std::int64_t s = std::min(
                first_starving(has_h, h_start, rh),
                first_starving(has_t, t_start, rt));
            const std::int64_t starving = s <= dt ? dt - s + 1 : 0;
            if (starving > 0) {
                const std::int64_t cur = prop_int(e, np.integrity, 500);
                const std::int64_t dmg =
                    std::max<std::int64_t>(1, met / 200) * starving;
                const std::int64_t nxt = std::max<std::int64_t>(0, cur - dmg);
                if (nxt != cur) {
                    StateChange ch;
                    ch.entity = e.id; ch.prop = np.integrity;
                    ch.had_before = true;
                    ch.before = Value::integer(cur);
                    ch.after = Value::integer(nxt);
                    e.props.set(np.integrity, Value::integer(nxt));
                    ev.changes.push_back(ch);
                    touched = true;
                }
                if (nxt <= 0) {
                    e.alive = false;
                    spatial.erase(e.id);
                    ev.evidence += "died (starvation); ";
                    return true;
                }
            }
        }
    }
    // Brain metabolic cost (Nhịp 4e): a mind pays milliJ/tick for each
    // unit of cognitive surplus — lookahead beyond the reflex horizon
    // (horizon-2) plus abstraction beyond neutral ((gain-1000)/1000).
    // The rate is permille-granular, so the remainder accumulates in
    // `mind_brain_accum` (milliJ*permille) and drains by Euclid division:
    // accum += rate*dt; drop = accum/1000; accum %= 1000 — bit-identical
    // whether the entity is ticked every step or caught up dormant.
    if (np.energy != kNoProp && np.mind_horizon != kNoProp &&
        np.brain_accum != kNoProp && minds_.count(e.id)) {
        const std::int64_t hor = std::clamp<std::int64_t>(
            prop_int(e, np.mind_horizon, 2), 1, 6);
        const std::int64_t abs_g = np.mind_abstraction != kNoProp
            ? prop_int(e, np.mind_abstraction, 1000) : 1000;
        const std::int64_t surplus_pm =
            std::max<std::int64_t>(0, hor - 2) * 1000 +
            std::max<std::int64_t>(0, abs_g - 1000);
        const std::int64_t bcost = np.mind_brain_cost != kNoProp
            ? prop_int(e, np.mind_brain_cost, 15) : 15;
        if (surplus_pm > 0 && bcost > 0) {
            std::int64_t acc = prop_int(e, np.brain_accum, 0);
            acc += bcost * surplus_pm * dt;          // milliJ*permille
            const std::int64_t drop = acc / 1000;     // whole milliJ
            acc %= 1000;
            const Value* en = prop_of(e, np.energy);
            if (en && en->type == ValType::Integer && drop > 0) {
                const std::int64_t nxt =
                    std::max<std::int64_t>(0, en->i - drop);
                if (nxt != en->i) {
                    StateChange ch;
                    ch.entity = e.id; ch.prop = np.energy;
                    ch.had_before = true;
                    ch.before = Value::integer(en->i);
                    ch.after = Value::integer(nxt);
                    e.props.set(np.energy, Value::integer(nxt));
                    ev.changes.push_back(ch);
                    touched = true;
                }
            }
            if (acc != prop_int(e, np.brain_accum, 0)) {
                StateChange ch;
                ch.entity = e.id; ch.prop = np.brain_accum;
                ch.had_before = true;
                ch.before = Value::integer(prop_int(e, np.brain_accum, 0));
                ch.after = Value::integer(acc);
                e.props.set(np.brain_accum, Value::integer(acc));
                ev.changes.push_back(ch);
                touched = true;
            }
        }
    }
    // Immune metabolic cost (Phase 11.3): an immune_power_base above the
    // species' declared threshold burns extra energy — permille-granular
    // like the brain cost, drained through immune_cost_accum by Euclid
    // division so batched and ticked evaluation agree. This is the
    // trade-off that keeps resistance selection from running away: a
    // hyper-immune lineage pays for every allele it keeps.
    if (np.energy != kNoProp) {
        static thread_local SeqPidCache ipid_c_;
    SeqPid ipid(ipid_c_, props);
        const PropId ipb = ipid("immune_power_base"),
                     ict = ipid("immune_cost_threshold"),
                     icp = ipid("immune_cost_permille"),
                     ica = ipid("immune_cost_accum");
        if (ipb != kNoProp && prop_of(e, ipb) && ica != kNoProp) {
            const std::int64_t base = prop_int(e, ipb, 0);
            const std::int64_t thresh = ict != kNoProp
                ? prop_int(e, ict, 1200) : 1200;
            const std::int64_t cperm = icp != kNoProp
                ? prop_int(e, icp, 0) : 0;
            const std::int64_t excess = std::max<std::int64_t>(0, base - thresh);
            if (excess > 0 && cperm > 0 && met > 0) {
                // eff permille of metabolism: excess*cperm/1000, floored —
                // sub-permille rates still accumulate exactly.
                const std::int64_t eff = excess * cperm / 1000;
                std::int64_t acc = prop_int(e, ica, 0);
                acc += met * eff * dt;
                const std::int64_t drop = acc / 1000;
                acc %= 1000;
                const Value* en = prop_of(e, np.energy);
                if (en && en->type == ValType::Integer && drop > 0) {
                    const std::int64_t nxt =
                        std::max<std::int64_t>(0, en->i - drop);
                    if (nxt != en->i) {
                        StateChange ch;
                        ch.entity = e.id; ch.prop = np.energy;
                        ch.had_before = true;
                        ch.before = Value::integer(en->i);
                        ch.after = Value::integer(nxt);
                        e.props.set(np.energy, Value::integer(nxt));
                        ev.changes.push_back(ch);
                        touched = true;
                    }
                }
                const std::int64_t a0 = prop_int(e, ica, 0);
                if (acc != a0) {
                    StateChange ch;
                    ch.entity = e.id; ch.prop = ica; ch.had_before = true;
                    ch.before = Value::integer(a0);
                    ch.after = Value::integer(acc);
                    e.props.set(ica, Value::integer(acc));
                    ev.changes.push_back(ch);
                    touched = true;
                }
            }
        }
    }
    // CL-1 brain work (Phase 3): MACs spent by brain_decide() since the
    // last pass are billed here — inside the causal metabolize event, so
    // ledger replay sees the energy cost. `mind_brain_work` records the
    // cumulative MAC ledger; `mind_brain_metabolic_cost` prices each MAC
    // (permille-microJ). mac_frac keeps the sub-unit remainder so batched
    // and per-tick evaluation agree. Speculative clones share brains_ but
    // must not drain the real store's pending counters.
    if (!speculative && brains_ && np.energy != kNoProp &&
        prop_of(e, np.energy)) {
        auto bit = brains_->slots.find(e.id);
        if (bit != brains_->slots.end() && bit->second.mac_pending > 0) {
            BrainSlot& bs = bit->second;
            static thread_local SeqPidCache bw_c_;
            SeqPid bw(bw_c_, props);
            const PropId pwork = bw("mind_brain_work");
            const std::int64_t macs = bs.mac_pending;
            bs.mac_pending = 0;
            if (pwork != kNoProp && prop_of(e, pwork)) {
                const std::int64_t w0 = prop_int(e, pwork, 0);
                const std::int64_t w1 = w0 + macs;
                StateChange ch;
                ch.entity = e.id; ch.prop = pwork; ch.had_before = true;
                ch.before = Value::integer(w0);
                ch.after = Value::integer(w1);
                e.props.set(pwork, Value::integer(w1));
                ev.changes.push_back(ch);
                touched = true;
            }
            const std::int64_t bcost = np.mind_brain_cost != kNoProp
                ? prop_int(e, np.mind_brain_cost, 15) : 15;
            if (bcost > 0) {
                // macs * bcost / 100000 energy units per MAC-lot, remainder
                // carried in mac_frac (permille-of-a-unit granularity).
                bs.mac_frac += macs * bcost;
                const std::int64_t drop = bs.mac_frac / 100000;
                bs.mac_frac %= 100000;
                if (drop > 0) {
                    const Value* en = prop_of(e, np.energy);
                    const std::int64_t nxt =
                        std::max<std::int64_t>(0, en->i - drop);
                    if (nxt != en->i) {
                        StateChange ch;
                        ch.entity = e.id; ch.prop = np.energy;
                        ch.had_before = true;
                        ch.before = Value::integer(en->i);
                        ch.after = Value::integer(nxt);
                        e.props.set(np.energy, Value::integer(nxt));
                        ev.changes.push_back(ch);
                        touched = true;
                    }
                }
            }
        }
    }
    // Flora fruit drift — an ecological process independent of `metabolism`
    // (plants do not eat) and, crucially, independent of the plant's own
    // `integrity`: fruit on the branch has a lifespan, the bush does not.
    //   * `regrow_rate` milli-units/tick scaled by the local moisture x
    //     temperature suitability refills `quantity` toward `max_quantity`.
    //   * `fruit_decay_rate` milli-units/tick rots fruit OFF the branch —
    //     `quantity` falls to a floor of 0 and STOPS; the plant stays alive
    //     and bare until the next fruiting cycle.
    // `regrow_credit` accumulates SIGNED sub-unit progress so per-tick and
    // batched dormant evaluation agree exactly; an emptied plant holds no
    // rot debt (credit is clamped at 0 when quantity reaches 0).
    // Only entities carrying the opt-in props drift — no species is named.
    const bool has_grow = np.regrow_rate != kNoProp &&
                          prop_of(e, np.regrow_rate);
    const bool has_frot = np.fruit_decay_rate != kNoProp &&
                          prop_of(e, np.fruit_decay_rate);
    if (np.quantity != kNoProp && np.regrow_credit != kNoProp &&
        (has_grow || has_frot) && prop_of(e, np.quantity)) {
        const std::int64_t cur = prop_int(e, np.quantity, 0);
        const std::int64_t mx = np.max_quantity != kNoProp &&
                                prop_of(e, np.max_quantity)
            ? prop_int(e, np.max_quantity, cur) : cur;
        if (cur != mx || (has_frot && cur > 0)) {
            std::int64_t net = 0;    // milli-units per tick, signed
            if (has_grow && cur < mx) {
                std::int64_t env = 500;    // neutral w/o terrain field
                if (!terrain.empty() && terrain.contains(e.pos)) {
                    const std::int64_t moist =
                        std::clamp<std::int64_t>(terrain.moist_at(e.pos), 0, 1000);
                    const std::int64_t temp = terrain.temp_at(e.pos);
                    const std::int64_t dev = temp > 200 ? temp - 200 : 200 - temp;
                    const std::int64_t tf =
                        std::clamp<std::int64_t>(1000 - dev * 4, 0, 1000);
                    env = moist * tf / 1000;
                }
                std::int64_t grow =
                    prop_int(e, np.regrow_rate, 0) * env / 1000;
                grow = grow * grow_eff / 1000;
                net += grow;
            }
            if (has_frot && cur > 0)      // nothing left to rot at 0
                net -= prop_int(e, np.fruit_decay_rate, 0);
            if (net != 0) {
                const std::int64_t credit0 = prop_int(e, np.regrow_credit, 0);
                const std::int64_t credit = credit0 + net * dt;
                const std::int64_t want = std::clamp<std::int64_t>(
                    credit / 1000, -cur, mx - cur);
                if (want != 0) {
                    StateChange ch;
                    ch.entity = e.id; ch.prop = np.quantity; ch.had_before = true;
                    ch.before = Value::integer(cur);
                    ch.after = Value::integer(cur + want);
                    e.props.set(np.quantity, ch.after);
                    ev.changes.push_back(ch);
                    touched = true;
                }
                std::int64_t credit1 = credit - want * 1000;
                if (cur + want <= 0) credit1 = std::max<std::int64_t>(0, credit1);
                if (credit1 != credit0) {
                    StateChange ch;
                    ch.entity = e.id; ch.prop = np.regrow_credit;
                    ch.had_before = prop_of(e, np.regrow_credit) != nullptr;
                    ch.before = Value::integer(credit0);
                    ch.after = Value::integer(credit1);
                    e.props.set(np.regrow_credit, ch.after);
                    ev.changes.push_back(ch);
                    touched = true;
                }
            }
        }
    }
    // Cultivation (Phase 18): `growth` accrues toward `growth_ticks` — a
    // sown crop is barren until mature. `irrigation_credit` is watered-soil
    // time left; it burns down linearly and lifts the effective moisture
    // the fruit drift reads below. Both are plain integer props advanced
    // by dt, so batched dormant catch-up is bit-identical to per-tick.
    if (np.growth != kNoProp && np.growth_ticks != kNoProp &&
        prop_of(e, np.growth) && prop_of(e, np.growth_ticks)) {
        const std::int64_t gt = prop_int(e, np.growth_ticks, 0);
        const std::int64_t g0 = prop_int(e, np.growth, 0);
        if (gt > 0 && g0 < gt) {
            const std::int64_t g1 = std::min(gt, g0 + dt);
            StateChange ch;
            ch.entity = e.id; ch.prop = np.growth;
            ch.had_before = true;
            ch.before = Value::integer(g0);
            ch.after = Value::integer(g1);
            e.props.set(np.growth, ch.after);
            ev.changes.push_back(ch);
            touched = true;
        }
        if (np.irrigation_credit != kNoProp && prop_of(e, np.irrigation_credit)) {
            const std::int64_t ic0 = prop_int(e, np.irrigation_credit, 0);
            if (ic0 > 0) {
                const std::int64_t ic1 = std::max<std::int64_t>(0, ic0 - dt);
                StateChange ch;
                ch.entity = e.id; ch.prop = np.irrigation_credit;
                ch.had_before = true;
                ch.before = Value::integer(ic0);
                ch.after = Value::integer(ic1);
                e.props.set(np.irrigation_credit, ch.after);
                ev.changes.push_back(ch);
                touched = true;
            }
        }
    }
    // Fruiting flora — two-layer food drift. `fruit_count` on the branch
    // regrows toward `max_fruit` at `fruit_regrow_rate` (env-scaled like
    // quantity regrowth) and spoils at `fruit_spoil_rate` — floored at 0,
    // NEVER touching the plant's integrity. `fruit_credit` accumulates
    // signed sub-unit progress so batched dormant catch-up is exact.
    const bool has_freg = np.fruit_regrow_rate != kNoProp &&
                          prop_of(e, np.fruit_regrow_rate);
    const bool has_fspoil = np.fruit_spoil_rate != kNoProp &&
                            prop_of(e, np.fruit_spoil_rate);
    if (np.fruit_count != kNoProp && np.fruit_credit != kNoProp &&
        (has_freg || has_fspoil) && prop_of(e, np.fruit_count)) {
        const std::int64_t cur = prop_int(e, np.fruit_count, 0);
        const std::int64_t mx = np.max_fruit != kNoProp &&
                                prop_of(e, np.max_fruit)
            ? prop_int(e, np.max_fruit, cur) : cur;
        if (cur != mx || (has_fspoil && cur > 0)) {
            std::int64_t net = 0;    // milli-units per tick, signed
            if (has_freg && cur < mx) {
                std::int64_t env = 500;
                if (!terrain.empty() && terrain.contains(e.pos)) {
                    std::int64_t moist =
                        std::clamp<std::int64_t>(terrain.moist_at(e.pos), 0, 1000);
                    // watered soil (Phase 18): `irrigation_credit` ticks of
                    // irrigation lift the moisture this plant's fruit drift
                    // sees — a channelled field fruits where rain will not.
                    if (np.irrigation_credit != kNoProp)
                        moist = std::max(moist,
                            prop_int(e, np.irrigation_credit, 0) > 0 ? 950 : moist);
                    const std::int64_t temp = terrain.temp_at(e.pos);
                    const std::int64_t dev = temp > 200 ? temp - 200 : 200 - temp;
                    const std::int64_t tf =
                        std::clamp<std::int64_t>(1000 - dev * 4, 0, 1000);
                    env = moist * tf / 1000;
                }
                std::int64_t regen =
                    prop_int(e, np.fruit_regrow_rate, 0) * env / 1000;
                regen = regen * grow_eff / 1000;
                // juvenile plants (Phase 18): `growth` below `growth_ticks`
                // means the crop has not fruited yet — sowing is an
                // investment, not an instant harvest.
                if (np.growth_ticks != kNoProp &&
                    prop_of(e, np.growth_ticks)) {
                    const std::int64_t gt = prop_int(e, np.growth_ticks, 0);
                    if (gt > 0 && prop_int(e, np.growth, 0) < gt) regen = 0;
                }
                // Phase 11.2: blight yield penalty and ash-fertilized soil
                // modulate the same rate — both World-Truth props, both
                // constant across a drift window (LOD-safe).
                if (np.yield_penalty != kNoProp) {
                    const std::int64_t yp =
                        std::min<std::int64_t>(900,
                            prop_int(e, np.yield_penalty, 0));
                    regen = regen * (1000 - yp) / 1000;
                }
                if (np.soil_nutrient != kNoProp) {
                    std::int64_t fert = 0;
                    const std::int64_t r2 = 64 * 64;
                    for (EntityId nid : spatial.within(e.pos, 64)) {
                        const Entity* o = find(nid);
                        if (!o || !o->alive || nid == e.id) continue;
                        if (dist2(o->pos, e.pos) > r2) continue;
                        fert = std::max(fert,
                            prop_int(*o, np.soil_nutrient, 0));
                    }
                    if (fert > 0)
                        regen = regen * (1000 + std::min<std::int64_t>(fert, 1000)) / 1000;
                }
                net += regen;
            }
            if (has_fspoil && cur > 0)     // bare branch holds no rot debt
                net -= prop_int(e, np.fruit_spoil_rate, 0);
            if (net != 0) {
                const std::int64_t credit0 = prop_int(e, np.fruit_credit, 0);
                const std::int64_t credit = credit0 + net * dt;
                const std::int64_t want = std::clamp<std::int64_t>(
                    credit / 1000, -cur, mx - cur);
                if (want != 0) {
                    StateChange ch;
                    ch.entity = e.id; ch.prop = np.fruit_count;
                    ch.had_before = true;
                    ch.before = Value::integer(cur);
                    ch.after = Value::integer(cur + want);
                    e.props.set(np.fruit_count, ch.after);
                    ev.changes.push_back(ch);
                    touched = true;
                }
                std::int64_t credit1 = credit - want * 1000;
                if (cur + want <= 0) credit1 = std::max<std::int64_t>(0, credit1);
                if (credit1 != credit0) {
                    StateChange ch;
                    ch.entity = e.id; ch.prop = np.fruit_credit;
                    ch.had_before = prop_of(e, np.fruit_credit) != nullptr;
                    ch.before = Value::integer(credit0);
                    ch.after = Value::integer(credit1);
                    e.props.set(np.fruit_credit, ch.after);
                    ev.changes.push_back(ch);
                    touched = true;
                }
            }
        }
    }
    // Perishability — material-driven rot. `decay_rate` (permille of the
    // 0..1000 integrity scale per tick; material props count via fallback,
    // an instance prop can override to 0 for non-perishing variants) erodes
    // `integrity` linearly; at 0 the entity is destroyed through the same
    // causal event — carcasses spoil, fruit bushes eventually die, stone
    // and wood (rate 0 or absent) last forever. decay_credit accumulates
    // milli-integrity so batched dormant catch-up is bit-identical.
    if (np.decay_rate != kNoProp && np.decay_credit != kNoProp &&
        np.integrity != kNoProp && prop_of(e, np.integrity)) {
        const Value* dv = prop_of(e, np.decay_rate);
        std::int64_t rate = dv && (dv->type == ValType::Integer ||
                                   dv->type == ValType::Fixed)
            ? dv->i : 0;
        // Granary seal (Phase 18): a contained good inside a sealed store
        // (`part_of` an enclosure carrying `granary_seal`) rots slower —
        // dry, dark storage is what makes a surplus survive to winter.
        if (rate > 0) {
            const PropId p_cont = SIMLAB_PID(props, "contained");
            const PropId p_seal = SIMLAB_PID(props, "granary_seal");
            if (p_cont != kNoProp && p_seal != kNoProp) {
                const Value* cv = prop_of(e, p_cont);
                if (cv && cv->type == ValType::Boolean && cv->b) {
                    for (const Relation& rel : relations_of(e.id, "part_of")) {
                        const EntityId hid = rel.a == e.id ? rel.b : rel.a;
                        const Entity* host = find(hid);
                        if (!host || !host->alive) continue;
                        const std::int64_t seal = prop_int(*host, p_seal, 0);
                        if (seal > 0)
                            rate = rate * (1000 - std::min<std::int64_t>(seal, 900)) / 1000;
                        break;
                    }
                }
            }
        }
        if (rate > 0) {
            const std::int64_t cur = prop_int(e, np.integrity, 0);
            const std::int64_t c0 = prop_int(e, np.decay_credit, 0);
            const std::int64_t c1 = c0 + rate * dt;   // milli-integrity
            const std::int64_t dmg = std::min(cur, c1 / 1000);
            if (dmg > 0 || c1 != c0) {
                const std::int64_t nxt = cur - dmg;
                if (nxt != cur) {
                    StateChange ch;
                    ch.entity = e.id; ch.prop = np.integrity;
                    ch.had_before = true;
                    ch.before = Value::integer(cur);
                    ch.after = Value::integer(nxt);
                    e.props.set(np.integrity, Value::integer(nxt));
                    ev.changes.push_back(ch);
                    touched = true;
                }
                const std::int64_t c2 = c1 - dmg * 1000;
                if (c2 != c0) {
                    StateChange ch;
                    ch.entity = e.id; ch.prop = np.decay_credit;
                    ch.had_before = prop_of(e, np.decay_credit) != nullptr;
                    ch.before = Value::integer(c0);
                    ch.after = Value::integer(c2);
                    e.props.set(np.decay_credit, ch.after);
                    ev.changes.push_back(ch);
                    touched = true;
                }
                // decomposition fertilizes (Phase 10.3): decaying matter
                // carrying `nutrient`/`moisture` amends the soil cell it
                // lies on — data-driven, any material may compost.
                if (dmg > 0 && terrain.contains(e.pos)) {
                    const PropertyDef* nd = props.find_name("nutrient");
                    const PropertyDef* md = props.find_name("moisture");
                    const std::int64_t nut =
                        nd ? prop_int(e, nd->id, 0) : 0;
                    const std::int64_t mst =
                        md ? prop_int(e, md->id, 0) : 0;
                    if (nut > 0 || mst > 0) {
                        const std::size_t ci = terrain.index(e.pos);
                        if (nut > 0)
                            terrain.vegetation[ci] = static_cast<std::int32_t>(
                                std::min<std::int64_t>(1000,
                                    std::max<std::int64_t>(0,
                                        terrain.vegetation[ci] +
                                        dmg * nut / 400)));
                        if (mst > 0)
                            terrain.moisture[ci] = static_cast<std::int32_t>(
                                std::min<std::int64_t>(1000,
                                    std::max<std::int64_t>(0,
                                        terrain.moisture[ci] +
                                        dmg * mst / 400)));
                        ev.evidence += "fertilized; ";
                    }
                }
                if (nxt <= 0) {
                    e.alive = false;
                    spatial.erase(e.id);
                    ev.evidence += "rotted away; ";
                    return true;
                }
            }
        }
    }
    return touched;
}

// Catch specific entities up to the current tick — used by interact() so a
// dormant participant is judged by its REAL current needs, not stale ones.
void World::flush_needs(const std::vector<EntityId>& ids) {
    const NeedProps np = need_props();
    if (np.metabolism == kNoProp && np.age == kNoProp) return;
    const std::uint64_t now = clock_.tick();
    CausalEvent ev;
    ev.tick = now;
    ev.action = "metabolize";
    ev.applied_laws.push_back(6);
    bool any = false;
    std::vector<ShedPool> shed_pools;
    std::vector<SpawnSpec> drift_spawns;
    for (EntityId id : ids) {
        Entity* ep = find(id);
        if (!ep || !ep->alive) continue;
        Entity& e = *ep;
        const std::uint64_t pending = now - e.last_metabolized;
        if (pending == 0) continue;
        // same reap rule as the bulk pass: integrity already <= 0 dies now
        if (np.integrity != kNoProp) {
            const Value* iv = prop_of(e, np.integrity);
            if (iv && iv->type == ValType::Integer && iv->i <= 0) {
                e.alive = false;
                spatial.erase(e.id);
                ev.evidence += "died (integrity 0); ";
                any = true;
                continue;
            }
        }
        bool drifted = false;
        const std::size_t chg0 = ev.changes.size();
        if (pending > 1 && physiology_coupled(e)) {
            for (std::uint64_t k = 0; k < pending && e.alive; ++k) {
                const std::uint64_t sub_now = now - pending + k + 1;
                if (drift_needs(e, 1, np, ev, sub_now)) drifted = true;
                anatomy_drift(e, 1, sub_now, ev, shed_pools, &drift_spawns);
                epidemiology_drift(e, 1, sub_now, ev, drift_spawns);
                morale_drift(e, 1, ev);
            }
        } else {
            drifted = drift_needs(e, pending, np, ev, now);
            // Phase 10: circulation runs for every living entity that carries
            // blood_volume — independent of the need-prop prefilter above.
            anatomy_drift(e, pending, now, ev, shed_pools, &drift_spawns);
            epidemiology_drift(e, static_cast<std::int64_t>(pending), now, ev,
                               drift_spawns);
            morale_drift(e, pending, ev);
        }
        if (drifted || ev.changes.size() != chg0) {
            ev.participants.push_back(e.id);
            any = true;
        }
        e.last_metabolized = now;
    }
    // shed waste lands as material entities — World Truth grows causally
    // inside the same metabolize event (blood pools, feces, urine).
    if (!shed_pools.empty()) {
        const PropId qty = SIMLAB_PID(props, "quantity");
        for (const ShedPool& sp : shed_pools) {
            PropertyBag pb;
            if (qty != kNoProp) pb.set(qty, Value::integer(sp.amount));
            const EntityId bid = create_entity(
                sp.kind, sp.material, sp.pos, pb);
            if (bid != kNoEntity) {
                ev.spawned.push_back(bid);
                ev.evidence += sp.kind + " " + std::to_string(bid) + "; ";
                any = true;
            }
        }
    }
    if (!drift_spawns.empty()) {
        commit_drift_spawns(ev, drift_spawns);
        any = true;
    }
    if (any) {
        ev.evidence = "needs caught up before interaction; " + ev.evidence;
        ledger.append(std::move(ev));
    }
}

// Need drift (TODO §10.1): hunger/thirst/fatigue grow with `metabolism`.
// Phase 9 LOD: entities far from every agent are DORMANT — they skip the
// per-tick pass and catch up in one batch of `pending` ticks. drift_needs
// makes the batch bit-identical to per-tick evaluation (linear needs,
// per-tick starvation accounting), so LOD changes COST, never the laws.
// Dormant mode engages only once the world has agents (minds); before
// that there is no observer to be "far" from and drift stays fine-grained.
const std::vector<PropId>& World::observable_props() const {
    if (obs_props_defs_ != props.defs().size()) {
        obs_props_cache_.clear();
        for (const PropertyDef& d : props.defs())
            if (d.domain == "observable") obs_props_cache_.push_back(d.id);
        obs_props_defs_ = props.defs().size();
    }
    return obs_props_cache_;
}

// Phase 10: functional fraction of a body's parts matching `role`.
// Denominator = every part entity ever wired part_of->body (severed parts
// keep the edge as anatomical record; `severed` is the detach truth).
// Numerator = attached (severed==0), alive, integrity>0. No anatomy -> 1000.
std::int64_t World::anatomy_function(EntityId body, const char* role) const {
    const PropId role_p = SIMLAB_PID(props, "part_role");
    const PropId sev_p = SIMLAB_PID(props, "severed");
    const PropId integ_p = SIMLAB_PID(props, "integrity");
    if (role_p == kNoProp) return 1000;
    std::int64_t total = 0, func = 0;
    for (const Relation& rel : relations_of(body, "part_of")) {
        if (rel.b != body) continue;
        const Entity* p = find(rel.a);
        if (!p || !p->alive) continue;
        const Value* rv = prop_of(*p, role_p);
        if (!rv || rv->type != ValType::Text || rv->s != role) continue;
        ++total;
        const Value* sv = sev_p != kNoProp ? prop_of(*p, sev_p) : nullptr;
        const bool severed = sv && sv->type == ValType::Integer && sv->i != 0;
        const Value* iv = integ_p != kNoProp ? prop_of(*p, integ_p) : nullptr;
        const bool broken = iv && iv->type == ValType::Integer && iv->i <= 0;
        if (!severed && !broken) ++func;
    }
    return total > 0 ? func * 1000 / total : 1000;
}

// Phase 10 circulation: hemorrhage, clotting, regeneration, hypovolemic
// shock — all integer, Euclid-exact via blood_accum/bleed_accum remainders
// so dt=1 x N is bit-identical to dt=N (same contract as drift_needs).
void World::anatomy_drift(Entity& e, std::uint64_t dt_u, std::uint64_t now,
                          CausalEvent& ev, std::vector<ShedPool>& pools,
                          std::vector<SpawnSpec>* xspawns) {
    if (dt_u == 0 || !e.alive) return;
    const std::int64_t dt = static_cast<std::int64_t>(dt_u);
    static thread_local SeqPidCache pid_c_;
    SeqPid pid(pid_c_, props);
    const PropId bv_p = pid("blood_volume"), ba_p = pid("blood_accum"),
                 br_p = pid("bleed_rate"), bac_p = pid("bleed_accum"),
                 cs_p = pid("consciousness"), integ_p = pid("integrity"),
                 energy_p = pid("energy"), thirst_p = pid("thirst"),
                 sev_p = pid("severed");
    if (bv_p == kNoProp || !prop_of(e, bv_p)) return;    // non-circulatory

    auto change = [&](PropId p, std::int64_t before, std::int64_t after) {
        if (p == kNoProp || before == after) return;
        StateChange ch;
        ch.entity = e.id; ch.prop = p; ch.had_before = true;
        ch.before = Value::integer(before); ch.after = Value::integer(after);
        e.props.set(p, Value::integer(after));
        ev.changes.push_back(ch);
    };

    // clotting + total bleed over attached parts AND the body's own
    // bleed_rate (stump bleeding after amputation lives on the body).
    std::int64_t total_bleed = 0;
    // clotting writes the PART's own props (its own StateChange entries).
    auto clot = [&](Entity& part) {
        if (br_p == kNoProp || bac_p == kNoProp) return;
        if (!prop_of(part, br_p)) return;
        const std::int64_t br = prop_int(part, br_p, 0);
        if (br <= 0) return;
        const std::int64_t bacc0 = prop_int(part, bac_p, 0);
        const std::int64_t acc = bacc0 + dt;
        const std::int64_t drop = acc / 50;      // bleed_rate -1 per 50 ticks
        const std::int64_t nacc = acc - drop * 50;
        const std::int64_t nbr = std::max<std::int64_t>(0, br - drop);
        if (nacc == bacc0 && nbr == br) return;
        auto pchange = [&](PropId p, std::int64_t before, std::int64_t after) {
            if (p == kNoProp || before == after) return;
            StateChange ch;
            ch.entity = part.id; ch.prop = p; ch.had_before = true;
            ch.before = Value::integer(before);
            ch.after = Value::integer(after);
            part.props.set(p, Value::integer(after));
            ev.changes.push_back(ch);
        };
        pchange(bac_p, bacc0, nacc);
        pchange(br_p, br, nbr);
    };
    if (br_p != kNoProp && prop_of(e, br_p)) {
        clot(e);
        total_bleed += prop_int(e, br_p, 0);
    }
    for (const Relation& rel : relations_of(e.id, "part_of")) {
        if (rel.b != e.id) continue;
        Entity* p = find(rel.a);
        if (!p || !p->alive) continue;
        if (sev_p != kNoProp) {
            const Value* sv = prop_of(*p, sev_p);
            if (sv && sv->type == ValType::Integer && sv->i != 0) continue;
        }
        if (prop_of(*p, br_p)) {
            clot(*p);
            total_bleed += prop_int(*p, br_p, 0);
        }
    }

    // toxin load from last tick gates infection progression & healing —
    // failing filtration lets toxins accumulate (Phase 10.3b). Read once
    // up-front; the toxin section below updates it for next tick.
    const PropId tox_e_p = pid("toxin_load");
    const std::int64_t toxin_prev = tox_e_p != kNoProp
        ? prop_int(e, tox_e_p, 0) : 0;
    const bool filter_ok = toxin_prev <= 400;

    // ---- infection & necrosis (Phase 10.2) --------------------------------
    // Open wounds (bleed_rate>0 or integrity<500) in unbandaged tissue
    // (bandaged_integrity<300) accumulate infection_level via infection_accum
    // (Euclid-exact). Rate scales with environmental contamination (max
    // `contamination` material prop within 96 units) and inversely with the
    // body's energy reserves. Bandaged wounds heal 2/tick (integer-exact).
    const PropId inf_p = pid("infection_level"), infa_p = pid("infection_accum"),
                 nec_p = pid("necrotic"), band_p = pid("bandaged_integrity"),
                 contam_p = pid("contamination"), integ2_p = pid("integrity"),
                 vital_p = pid("vital_organ"), pain_p = pid("pain_level");
    std::int64_t total_infection = 0;
    if (inf_p != kNoProp) {
        // environmental contamination: worst filth material near the body
        std::int64_t env_contam = 0;
        {
            // spatial query returns the same dist2<=96^2 set as the old
            // full-entity scan (max is order-free); alive check kept.
            spatial.for_each_within(e.pos, 96, [&](EntityId oid) {
                if (oid == e.id) return;
                const Entity* op = find(oid);
                if (!op || !op->alive) return;
                env_contam = std::max(env_contam, prop_int(*op, contam_p, 0));
            });
        }
        const std::int64_t energy_r = std::max<std::int64_t>(
            300, prop_int(e, pid("energy"), 60000) / 40);
        // toxemia doubles progression: failing filtration lets toxins
        // accumulate (Phase 10.3b organ coupling).
        const std::int64_t rate_milli =
            (150 + env_contam * 3 / 2) * 1000 / energy_r * (filter_ok ? 1 : 2);
        for (const Relation& rel : relations_of(e.id, "part_of")) {
            if (rel.b != e.id) continue;
            Entity* p = find(rel.a);
            if (!p || !p->alive || !prop_of(*p, inf_p)) continue;
            const std::int64_t br = br_p != kNoProp ? prop_int(*p, br_p, 0) : 0;
            const std::int64_t pi = prop_int(*p, integ2_p, 1000);
            const std::int64_t band = band_p != kNoProp ? prop_int(*p, band_p, 0) : 0;
            const std::int64_t inf = prop_int(*p, inf_p, 0);
            const std::int64_t nec = nec_p != kNoProp ? prop_int(*p, nec_p, 0) : 0;
            if (nec) { total_infection += inf; continue; }
            const bool wound_open = br > 0 || pi < 500;
            if (!wound_open) { total_infection += inf; continue; }
            if (band >= 300) {
                // dressed: contamination blocked, infection heals 2/tick
                const std::int64_t heal = std::min(inf,
                    filter_ok ? dt * 2 : dt / 4);
                if (heal > 0) {
                    auto pc = [&](PropId pp, std::int64_t b4, std::int64_t af) {
                        if (pp == kNoProp || b4 == af) return;
                        StateChange ch; ch.entity = p->id; ch.prop = pp;
                        ch.had_before = true; ch.before = Value::integer(b4);
                        ch.after = Value::integer(af);
                        p->props.set(pp, Value::integer(af));
                        ev.changes.push_back(ch);
                    };
                    pc(inf_p, inf, inf - heal);
                }
                total_infection += inf - heal;
                continue;
            }
            // progress via Euclid remainder
            const std::int64_t iacc0 = infa_p != kNoProp ? prop_int(*p, infa_p, 0) : 0;
            const std::int64_t iacc = iacc0 + dt * rate_milli;
            const std::int64_t delta = iacc / 1000;
            const std::int64_t ninf = std::min<std::int64_t>(1000, inf + delta);
            const std::int64_t nacc = iacc - delta * 1000;
            auto pc = [&](PropId pp, std::int64_t b4, std::int64_t af) {
                if (pp == kNoProp || b4 == af) return;
                StateChange ch; ch.entity = p->id; ch.prop = pp;
                ch.had_before = true; ch.before = Value::integer(b4);
                ch.after = Value::integer(af);
                p->props.set(pp, Value::integer(af));
                ev.changes.push_back(ch);
            };
            pc(inf_p, inf, ninf);
            pc(infa_p, iacc0, nacc);
            total_infection += ninf;
            // necrosis / sepsis thresholds
            const std::int64_t vital = vital_p != kNoProp ? prop_int(*p, vital_p, 0) : 0;
            if (ninf >= 800 && vital < 1000 && nec_p != kNoProp) {
                pc(nec_p, nec, 1);
                if (integ2_p != kNoProp) pc(integ2_p, pi, 0);
                ev.evidence += "necrosis; ";
            } else if (ninf >= 1000 && vital >= 1000 && integ2_p != kNoProp) {
                // sepsis: infection reaches a vital organ -> body dies
                const std::int64_t bi = prop_int(e, integ2_p, 500);
                StateChange ch; ch.entity = e.id; ch.prop = integ2_p;
                ch.had_before = true; ch.before = Value::integer(bi);
                ch.after = Value::integer(0);
                e.props.set(integ2_p, Value::integer(0));
                ev.changes.push_back(ch);
                e.alive = false; spatial.erase(e.id);
                ev.evidence += "sepsis; ";
                return;
            }
        }
    }

    // ---- lumen mechanics & organ coupling (Phase 10.3) ----------------------
    // Fully data-driven: any attached part carrying lumen_capacity>0 is a
    // hollow organ. Awaiting matter (body undigested_mass/fluid, charged
    // by "consume") seeps into phase-matched lumens; pressure, viscosity,
    // contractility, upstream backpressure, rupture and toxin coupling all
    // read part props — no organ names appear anywhere in this code.
    const PropId lc_p = pid("lumen_capacity"), cl_p = pid("contents_load"),
                 ca_p = pid("contents_accum"), cv_p = pid("content_viscosity"),
                 va_p = pid("viscosity_accum"), ct_p = pid("contractility"),
                 em_p = pid("evacuate_material"), us_p = pid("upstream_organ_slot"),
                 osa_p = pid("organ_stress_accum"), fr_p = pid("filtration_rate"),
                 rup_p = pid("ruptured"),
                 tox_p = pid("toxin_load"), toa_p = pid("toxin_accum"),
                 mat_p = pid("moisture"), met_p = pid("metabolism");
    const PropId um2_p = pid("undigested_mass"), uf2_p = pid("undigested_fluid");
    // gather lumen parts (alive, attached, non-severed, non-necrotic)
    struct Lumen {
        Entity* p = nullptr;
        bool fluid = false;                  // waste material moisture >= 600
        std::string evac;                    // evacuate_material name
        std::int64_t evac_moist = 0, evac_contam = 0;
        std::int64_t cap = 0, load = 0, visc = 0, contract = 0, integ = 1000;
        std::string up_slot;
    };
    std::vector<Lumen> lumens;
    if (lc_p != kNoProp) {
        for (const Relation& rel : relations_of(e.id, "part_of")) {
            if (rel.b != e.id) continue;
            Entity* pt = find(rel.a);
            if (!pt || !pt->alive) continue;
            if (sev_p != kNoProp) {
                const Value* sv = prop_of(*pt, sev_p);
                if (sv && sv->type == ValType::Integer && sv->i != 0) continue;
            }
            if (nec_p != kNoProp) {
                const Value* nv = prop_of(*pt, nec_p);
                if (nv && nv->type == ValType::Integer && nv->i != 0) continue;
            }
            std::int64_t cap = prop_int(*pt, lc_p, 0);
            if (cap <= 0) continue;
            // Phase 10.4: a growing mass physically occupies the lumen —
            // effective capacity shrinks, pressure mechanics do the rest.
            const PropId nm0 = pid("neoplasm_mass");
            if (nm0 != kNoProp) {
                const std::int64_t nm0v = prop_int(*pt, nm0, 0);
                if (nm0v > 0)
                    cap = std::max<std::int64_t>(1, cap * (1000 - nm0v) / 1000);
            }
            Lumen L; L.p = pt; L.cap = cap;
            L.load = cl_p != kNoProp ? prop_int(*pt, cl_p, 0) : 0;
            L.visc = cv_p != kNoProp ? prop_int(*pt, cv_p, 0) : 0;
            L.contract = ct_p != kNoProp ? prop_int(*pt, ct_p, 0) : 0;
            L.integ = integ_p != kNoProp ? prop_int(*pt, integ_p, 1000) : 1000;
            if (us_p != kNoProp) {
                const Value* uv = prop_of(*pt, us_p);
                if (uv && uv->type == ValType::Text) L.up_slot = uv->s;
            }
            if (em_p != kNoProp) {
                const Value* ev = prop_of(*pt, em_p);
                if (ev && ev->type == ValType::Text && !ev->s.empty()) {
                    L.evac = ev->s;
                    if (const MaterialDef* wm = materials.find_name(L.evac)) {
                        L.evac_moist = mat_p != kNoProp
                            ? prop_int(*pt, mat_p, 0) : 0;   // placeholder
                        // waste material props live on the MATERIAL, read
                        // through a temp lookup below
                        (void)wm;
                    }
                }
            }
            // phase: the waste material's own moisture decides — watery
            // waste (urine-like) fills fluid lumens, thick waste (feces-
            // like) fills solid ones. No names, only numbers.
            if (!L.evac.empty()) {
                if (const MaterialDef* wm = materials.find_name(L.evac)) {
                    Entity tmp; tmp.material = wm->id;
                    L.evac_moist = mat_p != kNoProp ? prop_int(tmp, mat_p, 0) : 0;
                    const PropId cp = pid("contamination");
                    L.evac_contam = cp != kNoProp ? prop_int(tmp, cp, 0) : 0;
                }
            }
            L.fluid = L.evac_moist >= 600;
            lumens.push_back(L);
        }
    }
    // fill: each lumen converts awaiting matter through contents_accum.
    // LOD-exact scheme: the per-tick rate is CONSTANT per lumen (capacity-
    // scaled), demand `d` is consumed from the accumulator standard-style,
    // and only the pool-bounded transfer `xfer = min(d, pool)` moves mass —
    // so dt=1 x N telescopes identically to dt=N (a pool-dependent rate
    // would re-sample the shrinking pool at different granularities).
    for (Lumen& L : lumens) {
        const PropId pool_p = L.fluid ? uf2_p : um2_p;
        const std::int64_t rate = 40 + L.cap / 25;    // milli-units/tick
        if (ca_p == kNoProp || cl_p == kNoProp) continue;
        const std::int64_t a0 = prop_int(*L.p, ca_p, 0);
        const std::int64_t acc = a0 + dt * rate;
        const std::int64_t d = acc / 1000;
        const std::int64_t nacc = acc - d * 1000;
        // transfer bounded by the awaiting pool, 1:1 (assimilation losses
        // are already priced into the constant rate).
        std::int64_t xfer = d;
        if (pool_p != kNoProp && prop_of(e, pool_p)) {
            const std::int64_t p0 = prop_int(e, pool_p, 0);
            xfer = std::min(d, std::max<std::int64_t>(0, p0));
            if (xfer > 0) change(pool_p, p0, p0 - xfer);
        }
        const std::int64_t nl = std::min<std::int64_t>(L.cap * 3 / 2,
                                                    L.load + xfer);
        auto pc = [&](PropId pp, std::int64_t b4, std::int64_t af) {
            if (pp == kNoProp || b4 == af) return;
            StateChange ch; ch.entity = L.p->id; ch.prop = pp;
            ch.had_before = true;
            ch.before = Value::integer(b4); ch.after = Value::integer(af);
            L.p->props.set(pp, Value::integer(af));
            ev.changes.push_back(ch);
        };
        pc(cl_p, L.load, nl); pc(ca_p, a0, nacc);
        L.load = nl;
        // viscosity: thick waste (moisture<600) hardens with thirst,
        // softens with water — signed Euclid drift on viscosity_accum.
        // Ungated on contents presence: a load>0 gate is not LOD-exact
        // (batched fill reports contents for the whole interval while
        // per-tick fill crosses 1 unit partway through). Viscosity is a
        // property of the contents phase — only consulted when load>0.
        if (cv_p != kNoProp && va_p != kNoProp && L.evac_moist < 600) {
            const std::int64_t thirsty = thirst_p != kNoProp &&
                prop_int(e, thirst_p, 0) > 600;
            const std::int64_t v0 = prop_int(*L.p, cv_p, 0);
            const std::int64_t s0 = prop_int(*L.p, va_p, 0);
            const std::int64_t sa = s0 + dt * (thirsty ? 650 : -200);
            const std::int64_t vd = sa / 1000;
            const std::int64_t nv = std::min<std::int64_t>(1000,
                std::max<std::int64_t>(0, v0 + vd));
            pc(cv_p, v0, nv); pc(va_p, s0, sa - vd * 1000);
            L.visc = nv;
        }
    }
    // pressure consequences: backpressure wear, involuntary void, rupture
    for (Lumen& L : lumens) {
        if (L.cap <= 0) continue;
        const std::int64_t pressure = L.load * 1000 / L.cap;
        // backpressure: distension >=700 wears the upstream organ slot
        if (pressure >= 700 && osa_p != kNoProp && !L.up_slot.empty()) {
            const std::int64_t s0 = prop_int(*L.p, osa_p, 0);
            const std::int64_t acc = s0 + dt * 120;
            const std::int64_t wear = acc / 1000;
            auto pc2 = [&](PropId pp, std::int64_t b4, std::int64_t af) {
                if (pp == kNoProp || b4 == af) return;
                StateChange ch; ch.entity = L.p->id; ch.prop = pp;
                ch.had_before = true;
                ch.before = Value::integer(b4); ch.after = Value::integer(af);
                L.p->props.set(pp, Value::integer(af));
                ev.changes.push_back(ch);
            };
            pc2(osa_p, s0, acc - wear * 1000);
            if (wear > 0) {
                for (const Relation& rel : relations_of(e.id, "part_of")) {
                    if (rel.b != e.id || rel.a == L.p->id) continue;
                    Entity* up = find(rel.a);
                    if (!up || !up->alive) continue;
                    if (sev_p != kNoProp) {
                        const Value* sv = prop_of(*up, sev_p);
                        if (sv && sv->type == ValType::Integer && sv->i != 0) continue;
                    }
                    if (nec_p != kNoProp) {
                        const Value* nv = prop_of(*up, nec_p);
                        if (nv && nv->type == ValType::Integer && nv->i != 0) continue;
                    }
                    const Value* sl = prop_of(*up, pid("body_slot"));
                    if (!sl || sl->type != ValType::Text || sl->s != L.up_slot)
                        continue;
                    if (integ_p == kNoProp) continue;
                    const std::int64_t ui = prop_int(*up, integ_p, 1000);
                    const std::int64_t nu = std::max<std::int64_t>(0, ui - wear);
                    if (nu == ui) continue;
                    StateChange ch; ch.entity = up->id; ch.prop = integ_p;
                    ch.had_before = true;
                    ch.before = Value::integer(ui); ch.after = Value::integer(nu);
                    up->props.set(integ_p, Value::integer(nu));
                    ev.changes.push_back(ch);
                }
            }
        }
        // involuntary voiding: pressure 1000 + wall gives way (panic,
        // unconsciousness, or exhaustion of retention) — only when the
        // contents can actually pass (viscosity <= contractility).
        if (pressure >= 1000 && L.visc <= L.contract && cl_p != kNoProp &&
            rup_p != kNoProp && prop_int(*L.p, rup_p, 0) == 0) {
            std::int64_t panic = 0, cs = 1000;
            const auto mit = minds_.find(e.id);
            if (mit != minds_.end()) panic = mit->second.neuro.panic;
            if (cs_p != kNoProp) cs = prop_int(e, cs_p, 1000);
            const bool give_way = panic >= 800 || cs < 300 || pressure >= 1200;
            if (give_way) {
                auto pc3 = [&](PropId pp, std::int64_t b4, std::int64_t af) {
                    if (pp == kNoProp || b4 == af) return;
                    StateChange ch; ch.entity = L.p->id; ch.prop = pp;
                    ch.had_before = true;
                    ch.before = Value::integer(b4); ch.after = Value::integer(af);
                    L.p->props.set(pp, Value::integer(af));
                    ev.changes.push_back(ch);
                };
                pc3(cl_p, L.load, 0);
                if (ca_p != kNoProp) pc3(ca_p, prop_int(*L.p, ca_p, 0), 0);
                if (!L.evac.empty()) {
                    const MaterialDef* wm = materials.find_name(L.evac);
                    pools.push_back({e.pos, L.load / 4 + 50,
                                     wm ? wm->id : kNoMaterial, L.evac});
                }
                if (mit != minds_.end()) {
                    mit->second.neuro.panic = std::min<std::int64_t>(1000,
                        mit->second.neuro.panic + 400);
                    mit->second.neuro.grief = std::min<std::int64_t>(1000,
                        mit->second.neuro.grief + 300);
                }
                ev.evidence += "involuntary voiding; ";
                L.load = 0;
                continue;
            }
        }
        // rupture: shattered wall under pressure, or pressure beyond what
        // a failing wall can hold — contents' contamination soaks every
        // sibling part (peritonitis), bleed_rate spikes, pain maxes.
        const bool wall_shattered = L.integ <= 300 && pressure >= 800;
        const bool over_max = pressure >= 1400 && L.visc > L.contract;
        if ((wall_shattered || over_max) && rup_p != kNoProp &&
            prop_int(*L.p, rup_p, 0) == 0) {
            auto pc4 = [&](Entity& tgt, PropId pp, std::int64_t b4,
                          std::int64_t af) {
                if (pp == kNoProp || b4 == af) return;
                StateChange ch; ch.entity = tgt.id; ch.prop = pp;
                ch.had_before = true;
                ch.before = Value::integer(b4); ch.after = Value::integer(af);
                tgt.props.set(pp, Value::integer(af));
                ev.changes.push_back(ch);
            };
            pc4(*L.p, rup_p, 0, 1);
            pc4(*L.p, integ_p, L.integ, 0);
            pc4(*L.p, br_p, prop_int(*L.p, br_p, 0),
                std::min<std::int64_t>(1000,
                    prop_int(*L.p, br_p, 0) + 300));
            pc4(*L.p, cl_p, L.load, 0);
            // contamination of the spilled contents infects siblings
            const std::int64_t spill = L.evac_contam / 4;
            if (spill > 0 && inf_p != kNoProp) {
                for (const Relation& rel : relations_of(e.id, "part_of")) {
                    if (rel.b != e.id || rel.a == L.p->id) continue;
                    Entity* sib = find(rel.a);
                    if (!sib || !sib->alive) continue;
                    const std::int64_t si = prop_int(*sib, inf_p, 0);
                    pc4(*sib, inf_p, si,
                        std::min<std::int64_t>(1000, si + spill));
                }
            }
            if (pain_p != kNoProp)
                pc4(e, pain_p, prop_int(e, pain_p, 0), 1000);
            ev.evidence += "organ rupture; ";
            L.load = 0;
        }
    }
    // ---- toxin coupling (Phase 10.3b) ---------------------------------------
    // toxin_load on the body: generated by metabolism + distended lumens,
    // filtered by attached parts carrying filtration_rate (scaled by their
    // integrity). toxin_accum is the signed Euclid remainder — dt-exact.
    if (tox_p != kNoProp && prop_of(e, tox_p) && toa_p != kNoProp) {
        std::int64_t gen = 100 + (met_p != kNoProp ? prop_int(e, met_p, 0) / 10 : 0);
        for (const Lumen& L : lumens) {
            const std::int64_t pressure = L.cap > 0 ? L.load * 1000 / L.cap : 0;
            if (pressure > 700) gen += (pressure - 700) * 10;
        }
        std::int64_t filt = 0;
        for (const Relation& rel : relations_of(e.id, "part_of")) {
            if (rel.b != e.id) continue;
            Entity* pt = find(rel.a);
            if (!pt || !pt->alive) continue;
            if (sev_p != kNoProp) {
                const Value* sv = prop_of(*pt, sev_p);
                if (sv && sv->type == ValType::Integer && sv->i != 0) continue;
            }
            if (nec_p != kNoProp) {
                const Value* nv = prop_of(*pt, nec_p);
                if (nv && nv->type == ValType::Integer && nv->i != 0) continue;
            }
            if (fr_p == kNoProp || integ_p == kNoProp) continue;
            const std::int64_t fr = prop_int(*pt, fr_p, 0);
            if (fr <= 0) continue;
            filt += fr * prop_int(*pt, integ_p, 1000) / 1000;
        }
        const std::int64_t t0 = prop_int(e, tox_p, 0);
        const std::int64_t a0 = prop_int(e, toa_p, 0);
        const std::int64_t acc = a0 + dt * (gen - filt);
        const std::int64_t d = acc / 1000;      // signed trunc: LOD-safe
        const std::int64_t nt = std::min<std::int64_t>(1000,
            std::max<std::int64_t>(0, t0 + d));
        change(tox_p, t0, nt);
        change(toa_p, a0, acc - d * 1000);
        if (nt >= 1000) {
            if (integ_p != kNoProp && prop_of(e, integ_p)) {
                change(integ_p, prop_int(e, integ_p, 0), 0);
            }
            e.alive = false; spatial.erase(e.id);
            ev.evidence += "toxemia; ";
        }
    }


    // ---- thermoregulation, adaptive immunity, neoplasm (Phase 10.4) ------
    // Species-relative: every threshold derives from the entity's own
    // normal_body_temp/thermoreg_gain/delta props — no fixed temperature
    // and no organ name appears here. All accumulators are Euclid-exact.
    {
        // generic prop writer for OTHER entities (parts, infections)
        auto change_on = [&](Entity& tgt, PropId p, std::int64_t before,
                             std::int64_t after) {
            if (p == kNoProp || before == after) return;
            StateChange ch;
            ch.entity = tgt.id; ch.prop = p; ch.had_before = true;
            ch.before = Value::integer(before);
            ch.after = Value::integer(after);
            tgt.props.set(p, Value::integer(after));
            ev.changes.push_back(ch);
        };
        const PropId nbt_p = pid("normal_body_temp"),
                     bt_p = pid("body_temp"),
                     tg_p = pid("thermoreg_gain"),
                     hd_p = pid("hypothermia_delta"),
                     fd_p = pid("fever_delta_max"),
                     hp_p = pid("hyperpyrexia_delta"),
                     ip_p = pid("immune_power"),
                     dr_p = pid("dna_repair_gain"),
                     ics_p = pid("immune_count"),
                     pi_p = pid("passive_immune"),
                     nm_p = pid("neoplasm_mass"),
                     na_p = pid("neoplasm_accum"),
                     nsa_p = pid("neoplasm_seed_accum"),
                     ma_p = pid("metastasis_accum"),
                     vn_p = pid("vital_neural"),
                     ap_p = pid("antipyretic"),
                     mu_p = pid("mutagenic"),
                     sl_p = pid("target_slot"),
                     bs_p = pid("body_slot"),
                     ps_p = pid("pathogen_strain"),
                     pl_p = pid("pathogen_load"),
                     ab_p = pid("antibody_level"),
                     ie_p = pid("infection_elapsed"),
                     ila_p = pid("infection_load_accum"),
                     iba_p = pid("infection_ab_accum"),
                     ma2_p = pid("mutation_accum"),
                     rl_p = pid("drift_rolls"),
                     nr_p = pid("neoplasm_rolls"),
                     mr_p = pid("metastasis_rolls"),
                     da_p = pid("damage_accum"),
                     ita_p = pid("infection_toxin_accum"),
                     sg_p = pid("antigen_signature"),
                     vir_p = pid("virulence");

        // collect this host's active infections (carries edges host->inf)
        struct Infect {
            Entity* ie = nullptr;
            Entity* strain = nullptr;
            std::int64_t vir = 0, load = 0, ab = 0, elapsed = 0;
            std::uint32_t antigen = 0;
            std::string slot;
            bool tolerated = false;   // reservoir host + symbiont strain
        };
        std::vector<Infect> infections;
        std::int64_t burden = total_infection;
        std::int64_t pathogen_sum = 0;
        if (ps_p != kNoProp && pl_p != kNoProp && ab_p != kNoProp) {
            for (const Relation& rel : relations_of(e.id, "carries")) {
                if (rel.a != e.id) continue;
                Entity* ie = find(rel.b);
                if (!ie || !ie->alive) continue;
                const std::int64_t se = ps_p != kNoProp
                    ? prop_int(*ie, ps_p, 0) : 0;
                Entity* st = se > 0 ? find(static_cast<EntityId>(se)) : nullptr;
                if (!st || !st->alive) continue;
                Infect I;
                I.ie = ie; I.strain = st;
                I.vir = prop_int(*st, vir_p, 0);
                I.load = prop_int(*ie, pl_p, 0);
                I.ab = prop_int(*ie, ab_p, 0);
                I.elapsed = ie_p != kNoProp ? prop_int(*ie, ie_p, 0) : 0;
                I.antigen = static_cast<std::uint32_t>(
                    sg_p != kNoProp ? prop_int(*st, sg_p, 0) : 0);
                if (sl_p != kNoProp) {
                    const Value* sv = prop_of(*st, sl_p);
                    if (sv && sv->type == ValType::Text) I.slot = sv->s;
                }
                // Phase 11: a reservoir-tolerant host carrying a symbiont
                // strain coexists peacefully — no virulence damage, no
                // fever, no toxin — but still sheds it environmentally.
                {
                    const PropertyDef* rtd = props.find_name("reservoir_tolerance");
                    const PropertyDef* syd = props.find_name("symbiont");
                    const PropertyDef* idv = props.find_name("is_disease_vector");
                    if (rtd && syd)
                        I.tolerated =
                            prop_int(e, rtd->id, 0) >= 700 &&
                            (prop_int(*st, syd->id, 0) != 0 ||
                             (idv && prop_of(e, idv->id) &&
                              prop_int(e, idv->id, 0) != 0));
                }
                pathogen_sum += I.load;
                infections.push_back(I);
            }
        }
        burden = std::min<std::int64_t>(2000, burden + pathogen_sum);

        // ---- (a) species-relative thermoregulation -------------------------
        std::int64_t dT = 0;          // body_temp - normal_body_temp
        bool fever_active = total_infection > 200;
        bool hyper_fever = false;
        // a complete thermal phenotype is required — every threshold is a
        // species-data delta, never an absolute mark in code.
        if (nbt_p != kNoProp && prop_of(e, nbt_p) &&
            hd_p != kNoProp && prop_of(e, hd_p) &&
            fd_p != kNoProp && prop_of(e, fd_p) &&
            hp_p != kNoProp && prop_of(e, hp_p)) {
            const std::int64_t normal = prop_int(e, nbt_p, 0);
            const std::int64_t hypo = prop_int(e, hd_p, 0);
            const std::int64_t fmax = prop_int(e, fd_p, 0);
            const std::int64_t hyper = prop_int(e, hp_p, 0);
            const std::int64_t cur = bt_p != kNoProp
                ? prop_int(e, bt_p, normal) : normal;
            const std::int64_t therm = tg_p != kNoProp
                ? prop_int(e, tg_p, 1000) : 1000;
            // ambient pull: weak thermoregulators track the environment;
            // cold + wet + exhausted pulls hardest (all data-driven deltas).
            // no terrain -> neutral environment at the species set-point
            std::int64_t env_t = normal, env_m = 500;
            if (!terrain.empty() && terrain.contains(e.pos)) {
                env_t = terrain.temp_at(e.pos);
                env_m = terrain.moist_at(e.pos);
            }
            // Phase 12.4: a roofed shell damps the environment toward the
            // occupant's set-point by its achieved insulation_factor —
            // shelter is physics (enclosure_ratio x shell material), not a
            // flag. Innermost footprint wins.
            {
                const PropId p_ins = SIMLAB_PID(props, "insulation_factor");
                const PropId p_rad2 = SIMLAB_PID(props, "footprint_radius");
                if (p_ins != kNoProp && p_rad2 != kNoProp) {
                    std::int64_t best_ins = 0, best_r = INT64_MAX;
                    for (const auto& sep : entities) {
                        const Entity& s = *sep;
                        if (!s.alive || s.kind_name != SIMLAB_KIND("enclosure"))
                            continue;
                        const std::int64_t r = prop_int(s, p_rad2, 0);
                        if (r <= 0) continue;
                        const std::int64_t dx = s.pos.x - e.pos.x;
                        const std::int64_t dy = s.pos.y - e.pos.y;
                        if (dx * dx + dy * dy > r * r) continue;
                        if (r < best_r) {
                            best_r = r;
                            best_ins = prop_int(s, p_ins, 0);
                        }
                    }
                    if (best_ins > 0) {
                        env_t = normal + (env_t - normal) *
                                (1000 - best_ins) / 1000;
                        env_m = env_m * (1000 - best_ins / 2) / 1000;
                    }
                }
            }
            const std::int64_t energy_r = prop_int(e, energy_p, 60000);
            std::int64_t pull = (1000 - therm);
            if (env_t < normal - 10)
                pull += std::min<std::int64_t>(400,
                    (normal - 10 - env_t) * 3);
            if (env_t > normal + 10)
                pull += std::min<std::int64_t>(200,
                    (env_t - normal - 10));
            if (env_m > 700) pull += 60;
            if (!terrain.empty() && terrain.contains(e.pos) &&
                terrain.is_water(e.pos)) {
                pull += 80;
                // Primordial immersion: a ford wets ankles, a swim floods
                // the chest — deeper water exposes more of the body, so the
                // core is pulled toward the water's temperature harder.
                // Wade-depth keeps the flat bonus; anyone aboard a craft
                // is not immersed (same depth rule as lawutil::water_depth).
                if ((ruleset & kRulePrimordial) != 0) {
                    const std::size_t wi = terrain.index(e.pos);
                    const std::uint8_t wt = terrain.water[wi];
                    std::int64_t dep = 0;
                    if (wt == 2) dep = 12;                       // river
                    else if (wt == 1) {                          // sea / lake
                        const std::int64_t below =
                            static_cast<std::int64_t>(gen_sea_level) -
                            terrain.elevation[wi];
                        dep = below <= 0 ? 40
                                         : std::max<std::int64_t>(20, below);
                    }
                    bool immersed = dep > 25;
                    if (immersed)
                        for (const Relation& ar : relations_of(e.id, "aboard"))
                            if (ar.a == e.id) { immersed = false; break; }
                    if (immersed)
                        pull += std::min<std::int64_t>(140, (dep - 25) * 2);
                }
            }
            if (energy_r < 20000) pull += 120;
            pull = std::min<std::int64_t>(1000, pull);
            std::int64_t target = normal +
                (env_t - normal) * pull / 1000;
            // defensive fever: infection burden elevates set-point by
            // 15..fever_delta_max scaled by load — species-relative.
            for (const Infect& I : infections)
                if (!I.tolerated && ie_p != kNoProp && I.elapsed >=
                    prop_int(*I.strain, pid("incubation_ticks"), 0))
                    fever_active = true;
            if (fever_active) {
                const std::int64_t span = std::max<std::int64_t>(1,
                    fmax - 15);
                const std::int64_t rise = 15 +
                    span * std::min<std::int64_t>(1000, burden) / 1000;
                target = std::max(target, normal + rise);
            }
            // antipyretic materials nearby suppress fever toward the set-
            // point; on a healthy body the same cooling overshoots into
            // hypothermia — the dose makes the poison, emergently.
            bool cool_near = false;
            {
                // same within-24 set as the old full scan; `any` is order-free
                spatial.for_each_within(e.pos, 24, [&](EntityId oid) {
                    if (cool_near || oid == e.id) return;
                    const Entity* op = find(oid);
                    if (!op || !op->alive) return;
                    if (ap_p != kNoProp &&
                        prop_int(*op, ap_p, 0) >= 400) cool_near = true;
                });
            }
            if (cool_near)
                target = fever_active ? normal
                                      : normal - hypo - hypo / 10;
            // LOD-exact approach: |move| <= 4 deciC per tick, linear in dt.
            const std::int64_t diff = target - cur;
            const std::int64_t maxmove = 4 * dt;
            const std::int64_t mv = diff > 0
                ? std::min(diff, maxmove)
                : std::max(diff, -maxmove);
            change(bt_p, cur, cur + mv);
            const std::int64_t newt = cur + mv;
            dT = newt - normal;
            // immune_power: halved once hypothermia threshold is crossed.
            // The BASE lives in immune_power_base so the halving is
            // idempotent — effective value derives, never decays.
            const PropId ipb_p = pid("immune_power_base");
            // one-time migration: an entity carrying immune_power but no
            // recorded base seeds the base from its current value, so the
            // hypothermia halving stays reversible forever after.
            if (ipb_p != kNoProp && ip_p != kNoProp &&
                prop_of(e, ip_p) && !prop_of(e, ipb_p))
                change(ipb_p, 0, prop_int(e, ip_p, 1000));
            if (ip_p != kNoProp && prop_of(e, ip_p)) {
                const std::int64_t ip_base =
                    ipb_p != kNoProp && prop_of(e, ipb_p)
                        ? prop_int(e, ipb_p, 1000)
                        : prop_int(e, ip_p, 1000);
                change(ip_p, prop_int(e, ip_p, 1000),
                       dT < -hypo ? ip_base / 2 : ip_base);
            }
            hyper_fever = dT > hyper;
            // hyperpyrexia: vital_neural parts take direct integrity loss
            // and consciousness sags — the tag decides, never a name.
            if (hyper_fever) {
                const std::int64_t over = dT - hyper;
                const std::int64_t neural_loss = dt * over / 4;
                if (neural_loss > 0)
                    for (const Relation& rel : relations_of(e.id, "part_of")) {
                        if (rel.b != e.id) continue;
                        Entity* pt = find(rel.a);
                        if (!pt || !pt->alive || !prop_of(*pt, vn_p))
                            continue;
                        const Value* nv = prop_of(*pt, vn_p);
                        if (!nv || nv->i == 0) continue;
                        const std::int64_t pi2 =
                            integ_p != kNoProp ? prop_int(*pt, integ_p, 1000) : 1000;
                        change_on(*pt, integ_p, pi2,
                                  std::max<std::int64_t>(0, pi2 - neural_loss));
                    }
                if (cs_p != kNoProp && prop_of(e, cs_p)) {
                    const std::int64_t cs0 = prop_int(e, cs_p, 1000);
                    change(cs_p, cs0,
                          std::max<std::int64_t>(0, cs0 - over));
                }
            }
            // fever cost once dT >= 15: extra energy + water burn.
            if (dT >= 15) {
                if (energy_p != kNoProp && prop_of(e, energy_p)) {
                    const std::int64_t en = prop_int(e, energy_p, 0);
                    change(energy_p, en, std::max<std::int64_t>(0, en - dt * 6));
                }
                if (thirst_p != kNoProp && prop_of(e, thirst_p)) {
                    const std::int64_t th = prop_int(e, thirst_p, 0);
                    change(thirst_p, th, std::min<std::int64_t>(1000, th + dt * 6));
                }
            }
        }

        // ---- (b) adaptive immunity: growth, antibody, drift, clearance ----
        const std::int64_t ip_eff = ip_p != kNoProp && prop_of(e, ip_p)
            ? prop_int(e, ip_p, 1000) : 1000;
        std::int64_t sig_slot[8];
        int sig_n = 0;
        for (int i = 0; i < 8; ++i) {
            const PropId sp = pid(kImmuneSigNames[i]);
            sig_slot[i] = sp != kNoProp && prop_of(e, sp)
                ? prop_int(e, sp, 0) : -1;
        }
        if (ics_p != kNoProp && prop_of(e, ics_p))
            sig_n = static_cast<int>(std::min<std::int64_t>(8,
                prop_int(e, ics_p, 0)));
        auto has_memory = [&](std::uint32_t ag) {
            for (int i = 0; i < sig_n; ++i)
                if (sig_slot[i] == static_cast<std::int64_t>(ag)) return true;
            return false;
        };
        // passive maternal immunity decays tick by tick; when it expires
        // the borrowed signatures leave immune memory — the shield was
        // lent, not earned, so the host becomes naive again (herd-immunity
        // waves need susceptibles to return with each generation).
        if (pi_p != kNoProp && prop_of(e, pi_p)) {
            const std::int64_t pv = prop_int(e, pi_p, 0);
            if (pv > 0) change(pi_p, pv, std::max<std::int64_t>(0, pv - dt));
        }
        const std::int64_t pi_left = pi_p != kNoProp
            ? prop_int(e, pi_p, 0) : 0;
        {
            const PropId psc_p = pid("passive_sig_count");
            if (pi_left <= 0 && psc_p != kNoProp &&
                prop_of(e, psc_p) && prop_int(e, psc_p, 0) > 0 &&
                ics_p != kNoProp) {
                const int psc = static_cast<int>(
                    std::min<std::int64_t>(8, prop_int(e, psc_p, 0)));
                int n = static_cast<int>(std::min<std::int64_t>(8,
                    prop_int(e, ics_p, 0)));
                // borrowed sigs occupy the leading slots; earned memories
                // sit behind them — compact the earned ones forward.
                for (int i = psc; i < n; ++i) {
                    const PropId dst = pid(("immune_sig" +
                        std::to_string(i - psc)).c_str());
                    const PropId src = pid(("immune_sig" +
                        std::to_string(i)).c_str());
                    if (dst == kNoProp || src == kNoProp) continue;
                    const std::int64_t sv = prop_int(e, src, 0);
                    if (prop_int(e, dst, 0) != sv)
                        change(dst, prop_int(e, dst, 0), sv);
                }
                for (int i = n - psc; i < n; ++i) {
                    const PropId tp = pid(("immune_sig" +
                        std::to_string(i)).c_str());
                    if (tp != kNoProp && prop_int(e, tp, 0) != 0)
                        change(tp, prop_int(e, tp, 0), 0);
                }
                if (n - psc != n && ics_p != kNoProp)
                    change(ics_p, n, std::max(0, n - psc));
                change(psc_p, psc, 0);
                ev.evidence += "passive immunity lapsed; ";
            }
        }
        const bool fever_kill = dT >= 15;
        for (Infect& I : infections) {
            Entity& ie = *I.ie;
            const std::int64_t inc = pid("incubation_ticks") != kNoProp
                ? prop_int(*I.strain, pid("incubation_ticks"), 0) : 0;
            I.elapsed += dt;
            change_on(ie, ie_p, I.elapsed - dt, I.elapsed);
            const bool incubated = I.elapsed >= inc;
            // growth: strain virulence drives it; fever halves it and adds
            // kill pressure; antibody_level brakes it proportionally.
            std::int64_t g = I.vir * 8 - I.ab * 3;
            if (fever_kill) g = g / 2 - 15;
            if (!incubated) g = g / 4;    // incubation dampens symptoms
            const std::int64_t la0 = ila_p != kNoProp
                ? prop_int(ie, ila_p, 0) : 0;
            const std::int64_t lacc = la0 + dt * g;
            const std::int64_t dl = lacc / 1000;
            const std::int64_t nl = std::min<std::int64_t>(1500,
                std::max<std::int64_t>(0, I.load + dl));
            change_on(ie, ila_p, la0, lacc - dl * 1000);
            change_on(ie, pl_p, I.load, nl);
            I.load = nl;
            // adaptive antibody: faster with immune memory match and with
            // maternally-transferred passive immunity still active.
            std::int64_t ar = ip_eff / 2 + (incubated ? ip_eff / 4 : 0);
            if (has_memory(I.antigen)) ar += ip_eff;
            if (pi_left > 0) ar += ip_eff / 2;
            const std::int64_t aa0 = iba_p != kNoProp
                ? prop_int(ie, iba_p, 0) : 0;
            const std::int64_t aacc = aa0 + dt * ar;
            const std::int64_t da = aacc / 1000;
            const std::int64_t nab = std::min<std::int64_t>(1500,
                I.ab + da);
            change_on(ie, iba_p, aa0, aacc - da * 1000);
            change_on(ie, ab_p, I.ab, nab);
            I.ab = nab;
            // tissue damage on the preferred slot (or any wound if unset)
            if (nl > 0 && integ_p != kNoProp) {
                const std::int64_t dacc0 = da_p != kNoProp
                    ? prop_int(ie, da_p, 0) : 0;
                const std::int64_t dacc = dacc0 +
                    dt * nl * I.vir / 200;
                const std::int64_t dd = dacc / 100000;
                // remainder persists every call — gating the write on
                // dd>0 starves the accumulator under per-tick evaluation.
                change_on(ie, da_p, dacc0, dacc - dd * 100000);
                if (dd > 0 && !I.tolerated) {
                    for (const Relation& rel : relations_of(e.id, "part_of")) {
                        if (rel.b != e.id) continue;
                        Entity* pt = find(rel.a);
                        if (!pt || !pt->alive) continue;
                        if (sev_p != kNoProp) {
                            const Value* sv = prop_of(*pt, sev_p);
                            if (sv && sv->i != 0) continue;
                        }
                        bool match = I.slot.empty();
                        if (!match && bs_p != kNoProp) {
                            const Value* bv2 = prop_of(*pt, bs_p);
                            if (bv2 && bv2->type == ValType::Text &&
                                bv2->s == I.slot) match = true;
                        }
                        if (!match) continue;
                        const std::int64_t pi3 = prop_int(*pt, integ_p, 1000);
                        change_on(*pt, integ_p, pi3,
                                  std::max<std::int64_t>(0, pi3 - dd));
                    }
                }
            }
            // virulence sheds toxin into the host's blood
            if (nl > 0 && !I.tolerated && tox_p != kNoProp &&
                prop_of(e, tox_p)) {
                const std::int64_t ta0 = ita_p != kNoProp
                    ? prop_int(ie, ita_p, 0) : 0;
                const std::int64_t tacc = ta0 + dt * nl * I.vir / 500;
                const std::int64_t td = tacc / 1000;
                change_on(ie, ita_p, ta0, tacc - td * 1000);
                if (td != 0) {
                    const std::int64_t t0 = prop_int(e, tox_p, 0);
                    change(tox_p, t0,
                          std::min<std::int64_t>(1000, t0 + td));
                    if (t0 + td >= 1000) {
                        if (integ_p != kNoProp && prop_of(e, integ_p))
                            change(integ_p, prop_int(e, integ_p, 0), 0);
                        e.alive = false; spatial.erase(e.id);
                        ev.evidence += "sepsis; ";
                    }
                }
            }
            // antigenic drift: high load rolls for a mutant strain —
            // deterministic accumulator, bit-shifted signature escapes
            // the host population's immune memory.
            if (nl >= 800 && xspawns) {
                const std::int64_t m0 = ma2_p != kNoProp
                    ? prop_int(ie, ma2_p, 0) : 0;
                const std::int64_t macc = m0 + dt * nl / 100;
                std::int64_t rolls = macc / 1000;
                change_on(ie, ma2_p, m0, macc - rolls * 1000);
                if (rolls > 0) {
                    // every roll is its own deterministic event. The epoch
                    // is a monotonic per-carrier counter, NOT the
                    // accumulator remainder — remainder levels repeat
                    // under per-tick evaluation and would correlate all
                    // rolls onto a few hash inputs.
                    if (rolls > 256) rolls = 256;
                    const std::int64_t rc0 = rl_p != kNoProp
                        ? prop_int(ie, rl_p, 0) : 0;
                    for (std::int64_t ri = 0; ri < rolls; ++ri) {
                        const std::uint64_t h = Rng::stream_seed(world_seed,
                            "antigenic_drift", ie.id ^
                            static_cast<std::uint64_t>(rc0 + ri));
                        if (h % 1000 < static_cast<std::uint64_t>(
                                std::max<std::int64_t>(1, I.vir / 10))) {
                            SpawnSpec ms;
                            ms.kind = "pathogen_strain";
                            ms.material = kNoMaterial;
                            ms.pos = e.pos;
                            for (const auto& [pp, vv] : I.strain->props.entries)
                                ms.props.set(pp, vv);
                            const std::uint32_t nsig = I.antigen ^
                                (1u << (h % 29)) ^ (1u << ((h >> 8) % 29));
                            if (sg_p != kNoProp)
                                ms.props.set(sg_p, Value::integer(nsig));
                            if (vir_p != kNoProp)
                                ms.props.set(vir_p, Value::integer(
                                    std::clamp<std::int64_t>(
                                        I.vir + static_cast<std::int64_t>(h % 81) - 40,
                                        0, 1000)));
                            xspawns->push_back(std::move(ms));
                            ev.evidence += "antigenic drift; ";
                        }
                    }
                    change_on(ie, rl_p, rc0, rc0 + rolls);
                }
            }
            // clearance: antibody dominance sterilizes the infection and
            // writes the antigen into immune memory (max 8 signatures).
            // A tolerated symbiont is chronic — the reservoir keeps it.
            if (!I.tolerated && (nl <= 0 || I.ab > nl)) {
                ie.alive = false; spatial.erase(ie.id);
                ev.evidence += "cleared ";
                if (!has_memory(I.antigen) && sig_n < 8) {
                    const PropId slotp = pid(("immune_sig" +
                        std::to_string(sig_n)).c_str());
                    if (slotp != kNoProp)
                        change(slotp, prop_int(e, slotp, 0),
                               static_cast<std::int64_t>(I.antigen));
                    if (ics_p != kNoProp)
                        change(ics_p, prop_int(e, ics_p, 0), sig_n + 1);
                }
                relations.unrelate(e.id, "carries", ie.id);
            }
        }

        // ---- (c) neoplasm initiation, growth, metastasis -------------------
        if (dr_p != kNoProp && prop_of(e, dr_p)) {
            // mutagenic exposure: worst mutagenic material within 64 units —
            // scanned lazily only for organisms already under oncogenic
            // pressure, keeping the common path cheap.
            std::int64_t env_mut = 0;
            {
                // same within-64 set as the old full scan; max is order-free
                spatial.for_each_within(e.pos, 64, [&](EntityId oid) {
                    if (oid == e.id) return;
                    const Entity* op = find(oid);
                    if (!op || !op->alive) return;
                    if (mu_p != kNoProp)
                        env_mut = std::max(env_mut, prop_int(*op, mu_p, 0));
                });
            }
            const std::int64_t dr = std::max<std::int64_t>(1,
                prop_int(e, dr_p, 1000));
            std::int64_t risk = 1;   // baseline spontaneous rate
            if (toxin_prev > 600) risk += 3;
            if (env_mut > 400) risk += 2;
            if (total_infection > 400) risk += 1;
            risk = risk * 1000 / dr;    // repair gain brakes initiation
            for (const Relation& rel : relations_of(e.id, "part_of")) {
                if (rel.b != e.id) continue;
                Entity* pt = find(rel.a);
                if (!pt || !pt->alive) continue;
                if (sev_p != kNoProp) {
                    const Value* sv = prop_of(*pt, sev_p);
                    if (sv && sv->i != 0) continue;
                }
                if (nec_p != kNoProp) {
                    const Value* nv = prop_of(*pt, nec_p);
                    if (nv && nv->i != 0) continue;
                }
                const std::int64_t nm = nm_p != kNoProp
                    ? prop_int(*pt, nm_p, 0) : 0;
                if (nm == 0) {
                    // initiation accumulator (Euclid): each 1000 milli-risk
                    // is one deterministic founding roll.
                    const std::int64_t s0 = nsa_p != kNoProp
                        ? prop_int(*pt, nsa_p, 0) : 0;
                    const std::int64_t sacc = s0 + dt * risk;
                    std::int64_t rolls = sacc / 1000;
                    change_on(*pt, nsa_p, s0, sacc - rolls * 1000);
                    if (rolls > 0) {
                        if (rolls > 256) rolls = 256;
                        const std::int64_t rc0 = nr_p != kNoProp
                            ? prop_int(*pt, nr_p, 0) : 0;
                        bool founded = false;
                        std::int64_t done = 0;
                        for (std::int64_t ri = 0; ri < rolls && !founded; ++ri) {
                            const std::uint64_t h = Rng::stream_seed(world_seed,
                                "neoplasm", pt->id ^
                                static_cast<std::uint64_t>(rc0 + ri));
                            ++done;
                            if (h % 1000 < 500) {
                                change_on(*pt, nm_p, 0, 10);
                                ev.evidence += "neoplasm initiated; ";
                                founded = true;
                            }
                        }
                        change_on(*pt, nr_p, rc0, rc0 + done);
                    }
                    continue;
                }
                // growth: mass accrues, drains host energy + blood, erodes
                // the part's own wall — all linear in dt.
                const std::int64_t g0 = na_p != kNoProp
                    ? prop_int(*pt, na_p, 0) : 0;
                const std::int64_t gacc = g0 + dt * (2 + nm / 40);
                const std::int64_t gd = gacc / 1000;
                const std::int64_t nnm = std::min<std::int64_t>(1000, nm + gd);
                change_on(*pt, na_p, g0, gacc - gd * 1000);
                if (nnm != nm) change_on(*pt, nm_p, nm, nnm);
                if (energy_p != kNoProp && prop_of(e, energy_p)) {
                    const std::int64_t en = prop_int(e, energy_p, 0);
                    change(energy_p, en,
                          std::max<std::int64_t>(0, en - dt * nm / 300));
                }
                if (bv_p != kNoProp && prop_of(e, bv_p)) {
                    const std::int64_t bvx = prop_int(e, bv_p, 1000);
                    change(bv_p, bvx,
                          std::max<std::int64_t>(0, bvx - dt * nm / 500));
                }
                if (integ_p != kNoProp) {
                    const std::int64_t pi4 = prop_int(*pt, integ_p, 1000);
                    change_on(*pt, integ_p, pi4,
                              std::max<std::int64_t>(0, pi4 - dt * nm / 900));
                }
                // metastasis: mass >= 750 seeds a sibling part determin-
                // istically; an amputated (severed) carrier cannot spread.
                if (nnm >= 750 && nm_p != kNoProp) {
                    const std::int64_t m0 = ma_p != kNoProp
                        ? prop_int(*pt, ma_p, 0) : 0;
                    const std::int64_t macc = m0 + dt * 4;
                    std::int64_t mrolls = macc / 1000;
                    change_on(*pt, ma_p, m0, macc - mrolls * 1000);
                    if (mrolls > 0) {
                        if (mrolls > 256) mrolls = 256;
                        const std::int64_t rc0 = mr_p != kNoProp
                            ? prop_int(*pt, mr_p, 0) : 0;
                        // pick a deterministic sibling part per roll
                        std::vector<EntityId> sibs;
                        for (const Relation& r2 : relations_of(e.id, "part_of")) {
                            if (r2.b != e.id || r2.a == rel.a) continue;
                            Entity* sp = find(r2.a);
                            if (!sp || !sp->alive) continue;
                            if (sev_p != kNoProp) {
                                const Value* sv = prop_of(*sp, sev_p);
                                if (sv && sv->i != 0) continue;
                            }
                            if (prop_int(*sp, nm_p, 0) == 0)
                                sibs.push_back(sp->id);
                        }
                        std::int64_t done = 0;
                        if (!sibs.empty()) {
                            for (std::int64_t ri = 0; ri < mrolls; ++ri) {
                                const std::uint64_t h = Rng::stream_seed(
                                    world_seed, "metastasis",
                                    pt->id ^ static_cast<std::uint64_t>(rc0 + ri));
                                ++done;
                                Entity* tgt = find(sibs[h % sibs.size()]);
                                if (tgt && prop_int(*tgt, nm_p, 0) == 0) {
                                    change_on(*tgt, nm_p, 0, 10);
                                    ev.evidence += "metastasis; ";
                                    break;
                                }
                            }
                        } else done = mrolls;
                        change_on(*pt, mr_p, rc0, rc0 + done);
                    }
                }
            }
        }
    }

    const std::int64_t bv = prop_int(e, bv_p, 1000);
    std::int64_t accum = ba_p != kNoProp ? prop_int(e, ba_p, 0) : 0;
    std::int64_t nbv = bv;
    if (total_bleed > 0) {
        // hemorrhage: bleed_rate is permille blood per 1000 ticks, so
        // accum (milli-blood) += dt * bleed_rate; drop = accum / 1000.
        accum += dt * total_bleed;
        const std::int64_t drop = accum / 1000;
        accum -= drop * 1000;
        nbv = std::max<std::int64_t>(0, bv - drop);
        // deterministic blood pools: one per 500-tick boundary crossed,
        // capped so a huge batch cannot flood the world in one call.
        constexpr std::uint64_t kPoolEvery = 500;
        const std::uint64_t before = (now - dt_u) / kPoolEvery;
        const std::uint64_t after = now / kPoolEvery;
        std::uint64_t n = after > before ? after - before : 0;
        if (n > 4) n = 4;
        const MaterialDef* bm = materials.find_name("blood");
        for (std::uint64_t i = 0; i < n; ++i)
            pools.push_back({e.pos, std::min<std::int64_t>(total_bleed / 5, 200),
                             bm ? bm->id : kNoMaterial, "blood"});
    } else {
        // regeneration: only while fed and watered (energy>0, thirst<800).
        const std::int64_t energy = energy_p != kNoProp ? prop_int(e, energy_p, 0) : 1;
        const std::int64_t thirst = thirst_p != kNoProp ? prop_int(e, thirst_p, 0) : 0;
        if (energy > 0 && thirst < 800 && bv < 1000 && toxin_prev < 400) {
            accum += dt * 12;                    // +12 permille/tick regen
            const std::int64_t gain = accum / 1000;
            accum -= gain * 1000;
            nbv = std::min<std::int64_t>(1000, bv + gain);
        }
    }
    change(bv_p, bv, nbv);
    change(ba_p, ba_p != kNoProp ? prop_int(e, ba_p, 0) : 0, accum);
    // consciousness tracks circulatory volume 1:1 (permille scale),
    // reduced by fever when total infection exceeds 300.
    if (cs_p != kNoProp) {
        const std::int64_t cs = prop_int(e, cs_p, 1000);
        std::int64_t cs_t = nbv;
        if (total_infection > 300)
            cs_t = std::min(cs_t, std::max<std::int64_t>(0, 1000 - total_infection / 4));
        change(cs_p, cs, cs_t);
        if (total_infection > 300) {
            // fever cost: extra energy burn + dehydration + panic
            // (linear, clamp-once — same LOD contract as the need drift)
            const PropId ep = pid("energy"), tp2 = pid("thirst");
            if (ep != kNoProp && prop_of(e, ep)) {
                const std::int64_t en = prop_int(e, ep, 0);
                change(ep, en, std::max<std::int64_t>(0, en - dt * 4));
            }
            if (tp2 != kNoProp && prop_of(e, tp2)) {
                const std::int64_t th = prop_int(e, tp2, 0);
                change(tp2, th, std::min<std::int64_t>(1000, th + dt * 4));
            }
            auto mit = minds_.find(e.id);
            if (mit != minds_.end())
                mit->second.neuro.panic = std::min<std::int64_t>(1000,
                    mit->second.neuro.panic + dt * 2);
        }
    }
    // ---- respiration (CL-1 Phase 3): O2 exchange, land vs water ------------
    // Opt-in like flora drift: only entities CARRYING `oxygen` respire.
    // Exchange rate scales with the expressed organ gain — `organ_lung_gain`
    // on land (depth <= kWadeDepth), `organ_gill_gain` submerged — minus a
    // flat metabolic O2 cost, so an organ-less diver slowly suffocates and
    // a weak lung slowly tires on land. Organ upkeep is priced in `energy`
    // proportional to (lung+gill): running BOTH is expensive, which is the
    // trade-off that makes amphibian generalists pay for their flexibility.
    // Organ expression is read from genome loci first (heritable), then
    // props — the same lookup order brain_locus uses. `resp_accum` is the
    // Euclid remainder for the upkeep drain; O2 itself is integer permille
    // and moves linearly, so dt-batching is exact.
    {
        const PropId ox_p = pid("oxygen"),
                     lg_p = pid("organ_lung_gain"),
                     gg_p = pid("organ_gill_gain"),
                     ra_p = pid("resp_accum"),
                     pain_p2 = pid("pain_level");
        if (ox_p != kNoProp && prop_of(e, ox_p)) {
            auto expr = [&](PropId p) -> std::int64_t {
                if (p == kNoProp) return 0;
                if (const std::int64_t* v = e.genome.get(p)) return *v;
                return prop_int(e, p, 0);
            };
            const std::int64_t lung = expr(lg_p);
            const std::int64_t gill = expr(gg_p);
            {
                const std::int64_t submerged =
                    lawutil::water_depth(*this, e.pos);
                const bool under = submerged > lawutil::kWadeDepth;
                // exchange permille/tick: gain*50/1000 - 4 baseline cost —
                // applied even with NO organs (gain 0 -> -4/tick suffocation)
                const std::int64_t gain = under ? gill : lung;
                const std::int64_t exch = gain * 50 / 1000 - 4;
                const std::int64_t ox0 = prop_int(e, ox_p, 1000);
                if (exch != 0) {
                    const std::int64_t nox =
                        std::max<std::int64_t>(0,
                            std::min<std::int64_t>(1000, ox0 + exch * dt));
                    if (nox != ox0) change(ox_p, ox0, nox);
                }
            }
            if (lung > 0 || gill > 0) {
                // organ upkeep: (lung+gill)/50 energy units per tick
                if (ra_p != kNoProp && energy_p != kNoProp &&
                    prop_of(e, ra_p) && prop_of(e, energy_p)) {
                    const std::int64_t upkeep = lung + gill;
                    if (upkeep > 0) {
                        std::int64_t acc = prop_int(e, ra_p, 0) + upkeep * dt;
                        const std::int64_t drop = acc / 50;
                        acc %= 50;
                        if (drop > 0) {
                            const std::int64_t en0 = prop_int(e, energy_p, 0);
                            change(energy_p, en0,
                                   std::max<std::int64_t>(0, en0 - drop));
                        }
                        change(ra_p, prop_int(e, ra_p, 0), acc);
                    }
                }
            }
            // hypoxia: O2 < 350 -> blood loss + consciousness decay + pain.
            // Pain deltas already pump neuro panic/rage and forge negative
            // engrams in learn_from_event — the CL-1 brain feels suffocation
            // as affect, not as a labelled state.
            const std::int64_t ox_now = prop_int(e, ox_p, 1000);
            if (ox_now < 350) {
                const std::int64_t deficit = 350 - ox_now;
                if (bv_p != kNoProp && prop_of(e, bv_p)) {
                    const std::int64_t bv0 = prop_int(e, bv_p, 500);
                    change(bv_p, bv0,
                           std::max<std::int64_t>(0,
                               bv0 - deficit * dt / 200));
                }
                if (cs_p != kNoProp && prop_of(e, cs_p)) {
                    const std::int64_t cs0 = prop_int(e, cs_p, 1000);
                    change(cs_p, cs0,
                           std::max<std::int64_t>(0,
                               cs0 - deficit * dt / 300));
                }
                if (pain_p2 != kNoProp && prop_of(e, pain_p2)) {
                    const std::int64_t pp0 = prop_int(e, pain_p2, 0);
                    change(pain_p2, pp0,
                           std::min<std::int64_t>(1000, pp0 + deficit * dt / 100));
                }
            }
        }
    }
    // hypovolemic shock: no blood -> clinical death, same reap rule.
    if (nbv <= 0 && integ_p != kNoProp && prop_of(e, integ_p)) {
        change(integ_p, prop_int(e, integ_p, 0), 0);
        e.alive = false;
        spatial.erase(e.id);
        ev.evidence += "hypovolemic shock; ";
    }
}

void World::commit_drift_spawns(CausalEvent& ev,
                                std::vector<SpawnSpec>& spawns) {
    for (const SpawnSpec& s : spawns) {
        const EntityId nid = create_entity(s.kind, s.material, s.pos, s.props);
        if (nid == kNoEntity) {
            ev.evidence += "drift spawn failed: " + s.kind + "; ";
            continue;
        }
        if (!s.genome.loci.empty())
            if (Entity* ne = find(nid)) ne->genome = s.genome;
        for (const auto& [type, other] : s.rel_from)
            relations.relate(other, type, nid);
        ev.evidence += "spawned " + s.kind + " " + std::to_string(nid) + "; ";
        ev.spawned.push_back(nid);
    }
    spawns.clear();
}

// Phase 10.4 epidemiology: each entity sheds transmission quanta from its
// active infections (airborne + contact routes, one shed_accum on the
// infection entity) or — when the entity IS contaminated waste carrying
// strain_ref — from the waterborne route. Quanta are Euclid-exact in the
// Phase 10.4: coupled physiology detector. Infection dynamics, neoplasm
// growth and fever feedback are nonlinear ODEs — antibody level brakes
// pathogen growth, pathogen load sets the fever target, toxin load gates
// oncogenesis. A single batched pass freezes those inputs at call start
// and diverges from per-tick evaluation; coupled entities therefore replay
// pending ticks one-by-one (rare — only carriers of active infections,
// growing tumours, tissue infection, or toxin near feedback thresholds).
// Phase 12.6 combat morale: drifts toward a TARGET set by the soldier's
// real situation — hunger, pain, a leader it defers to standing near, a
// supply line (stocked depot reached directly or along a worn trail) and
// the community's sung epics it carries in memory. 5 permille per tick
// toward the target; < 200 routs, >= 400 rallies. Pure function of state
// that is frozen during advance_ticks (positions, depot stock, beliefs), with
// hunger/pain stepped tick-wise via physiology_coupled -> LOD-exact.
void World::morale_drift(Entity& e, std::uint64_t ticks, CausalEvent& ev) {
    if (!e.alive || ticks == 0) return;
    static thread_local SeqPidCache c_;
    SeqPid pid(c_, props);
    const PropId wt_p = pid("war_target"), cm_p = pid("combat_morale"),
                 ro_p = pid("routed"), sd_p = pid("supply_depot_link"),
                 hu_p = pid("hunger"), pa_p = pid("pain_level"),
                 cr_p = pid("communicate_range"), fr_p = pid("footprint_radius"),
                 nu_p = pid("nutrition"), q_p = pid("quantity"),
                 og_p = pid("oral_genre");
    if (wt_p == kNoProp || cm_p == kNoProp || prop_int(e, wt_p, 0) <= 0) return;
    std::int64_t target = 500;
    const AgentMind* m = find_mind(e.id);
    // a leader it defers to, within earshot
    if (m) {
        EntityId lead = kNoEntity; std::int64_t best = 399;
        for (const auto& [k, b] : m->beliefs)
            if (k.rfind("defer_to:", 0) == 0 && b.confidence > best) {
                try { lead = static_cast<EntityId>(std::stoull(k.substr(9))); }
                catch (...) { continue; }
                best = b.confidence;
            }
        const Entity* le = lead != kNoEntity ? find(lead) : nullptr;
        const std::int64_t cr = cr_p != kNoProp ? prop_int(e, cr_p, 400) : 400;
        if (le && le->alive && dist2(le->pos, e.pos) <= cr * cr) target += 150;
    }
    // supply line: a stocked depot reached directly or along a worn trail
    const std::int64_t depot = sd_p != kNoProp ? prop_int(e, sd_p, 0) : 0;
    if (depot > 0) {
        const Entity* de = find(static_cast<EntityId>(depot));
        if (de && de->alive && de->kind_name == SIMLAB_KIND("enclosure")) {
            const std::int64_t rad = fr_p != kNoProp ? prop_int(*de, fr_p, 0) : 0;
            bool stocked = false;
            for (const EntityId id : entities_near(de->pos, rad)) {
                const Entity* g = find(id);
                if (!g || !g->alive || g->id == e.id) continue;
                if (nu_p != kNoProp && prop_int(*g, nu_p, 0) > 0 &&
                    (q_p == kNoProp || !g->props.get(q_p) || prop_int(*g, q_p, 0) > 0)) {
                    stocked = true; break;
                }
            }
            const bool at_depot = dist2(de->pos, e.pos) <= rad * rad;
            const bool on_trail = !terrain.empty() && terrain.contains(e.pos) &&
                                  terrain.wear_at(e.pos) >= 100;
            if (stocked && (at_depot || on_trail)) target += 150;
        }
    }
    const std::int64_t hunger = hu_p != kNoProp ? prop_int(e, hu_p, 0) : 0;
    if (hunger >= 700) target -= 500;          // starving: no leader holds them
    else if (hunger >= 500) target -= 200;
    if (pa_p != kNoProp) target -= prop_int(e, pa_p, 0) / 3;
    // a sung epic in memory steadies the ranks
    if (m && og_p != kNoProp) {
        for (const auto& [k, b] : m->beliefs) {
            if (k.rfind("lex:", 0) != 0 || b.confidence < 500) continue;
            EntityId le = kNoEntity;
            try { le = static_cast<EntityId>(std::stoull(k.substr(4))); } catch (...) { continue; }
            const Entity* lx = find(le);
            const Value* gv = lx ? prop_of(*lx, og_p) : nullptr;
            if (gv && gv->type == ValType::Text && !gv->s.empty()) { target += 100; break; }
        }
    }
    target = std::clamp<std::int64_t>(target, 0, 1000);
    const std::int64_t cur0 = prop_int(e, cm_p, 500);
    std::int64_t cur = cur0;
    for (std::uint64_t k = 0; k < ticks && cur != target; ++k)
        cur += std::clamp<std::int64_t>(target - cur, -5, 5);
    auto set_int = [&](PropId p, std::int64_t before, std::int64_t after) {
        if (p == kNoProp || before == after) return;
        StateChange ch; ch.entity = e.id; ch.prop = p;
        ch.had_before = prop_of(e, p) != nullptr;
        ch.before = Value::integer(before); ch.after = Value::integer(after);
        e.props.set(p, ch.after);
        ev.changes.push_back(ch);
    };
    set_int(cm_p, cur0, cur);
    const std::int64_t ro0 = ro_p != kNoProp ? prop_int(e, ro_p, 0) : 0;
    std::int64_t ro = ro0;
    if (cur < 200) ro = 1; else if (cur >= 400) ro = 0;
    set_int(ro_p, ro0, ro);
}

bool World::physiology_coupled(const Entity& e) const {
    if (!e.alive) return false;
    static thread_local SeqPidCache pid_c_;
    SeqPid pid(pid_c_, props);
    const PropId pl_p = pid("pathogen_load"),
                 nm_p = pid("neoplasm_mass"),
                 nsa_p = pid("neoplasm_seed_accum"),
                 inf_p = pid("infection_level"),
                 tox_p = pid("toxin_load");
    // active infections carried by this host
    for (const Relation& r : relations_of(e.id, "carries")) {
        if (r.a != e.id) continue;
        const Entity* ie = find(r.b);
        if (ie && ie->alive && pl_p != kNoProp &&
            prop_of(*ie, pl_p) != nullptr)
            return true;
    }
    // the host's own tissue-infection level feeds fever/consciousness
    if (inf_p != kNoProp && prop_int(e, inf_p, 0) > 0) return true;
    // parts carrying tumour or tissue-infection state
    for (const Relation& r : relations_of(e.id, "part_of")) {
        if (r.b != e.id) continue;
        const Entity* p = find(r.a);
        if (!p || !p->alive) continue;
        if ((nm_p != kNoProp && prop_int(*p, nm_p, 0) > 0) ||
            (nsa_p != kNoProp && prop_int(*p, nsa_p, 0) > 0) ||
            (inf_p != kNoProp && prop_int(*p, inf_p, 0) > 0))
            return true;
    }
    // soldiers on campaign (Phase 12.6): morale follows hunger tick by tick,
    // so a batched catch-up must replay per tick to stay bit-identical.
    {
        const PropId wt_p = SIMLAB_PID(props, "war_target");
        if (wt_p != kNoProp && prop_int(e, wt_p, 0) > 0) return true;
    }
    // toxin approaching the oncogenic threshold — the neoplasm risk term
    // flips mid-batch otherwise.
    if (tox_p != kNoProp && prop_int(e, tox_p, 0) > 300) return true;
    return false;
}

// host's pending ticks, so dormant catch-up transmits identically.
// Shared infection-spawn path: every transmission route (airborne,
// contact, waterborne, vector bite, soil sprout) queues the same
// ActiveInfection shape — deduped per pass so a host never gets two
// copies of the same strain queued in one metabolize event.
void World::queue_infection(std::vector<SpawnSpec>& out, EntityId host,
                            std::int64_t strain_eid, std::int64_t load) {
    Entity* h = find(host);
    if (!h) return;
    static thread_local SeqPidCache pid_c_;
    SeqPid pid(pid_c_, props);
    const PropId ps_p = pid("pathogen_strain"),
                 pl_p = pid("pathogen_load"),
                 ab_p = pid("antibody_level"),
                 ie_p = pid("infection_elapsed"),
                 ila_p = pid("infection_load_accum"),
                 iba_p = pid("infection_ab_accum"),
                 sa_p = pid("shed_accum");
    for (const SpawnSpec& sp : out) {
        bool is_carries = false;
        for (const auto& [t, o] : sp.rel_from)
            if (t == "carries" && o == host) is_carries = true;
        if (!is_carries) continue;
        const Value* v = sp.props.get(ps_p);
        if (v && v->type == ValType::Integer && v->i == strain_eid)
            return;   // identical infection already queued this pass
    }
    SpawnSpec s;
    s.kind = "infection";
    s.material = kNoMaterial;
    s.pos = h->pos;
    PropertyBag pb;
    pb.set(ps_p, Value::integer(strain_eid));
    pb.set(pl_p, Value::integer(load));
    pb.set(ab_p, Value::integer(0));
    if (ie_p != kNoProp) pb.set(ie_p, Value::integer(0));
    if (ila_p != kNoProp) pb.set(ila_p, Value::integer(0));
    if (iba_p != kNoProp) pb.set(iba_p, Value::integer(0));
    if (sa_p != kNoProp) pb.set(sa_p, Value::integer(0));
    s.props = pb;
    s.rel_from.push_back({"carries", host});
    out.push_back(std::move(s));
}

// Herd immunity (Phase 11.3): a host whose immune_sig slots hold the
// strain's antigen_signature is protected — transmission routes skip it.
// Slots beyond immune_count are stale (cleared or never written) and do
// not count; maternally borrowed sigs protect only while passive_immune
// remains (the expiry compaction in anatomy_drift removes them anyway).
bool World::host_immune_to(EntityId host, std::int64_t strain_id) const {
    const Entity* st = find(static_cast<EntityId>(strain_id));
    if (!st || !st->alive) return false;
    static thread_local SeqPidCache pid_c_;
    SeqPid pid(pid_c_, props);
    const PropId ag_p = pid("antigen_signature"),
                 ics_p = pid("immune_count");
    if (ag_p == kNoProp) return false;
    const std::int64_t ag = prop_int(*st, ag_p, 0);
    if (ag <= 0) return false;
    const Entity* h = find(host);
    if (!h || !h->alive) return false;
    const int n = ics_p != kNoProp
        ? static_cast<int>(std::min<std::int64_t>(8, prop_int(*h, ics_p, 0)))
        : 0;
    for (int i = 0; i < n; ++i) {
        const PropId sp = pid(kImmuneSigNames[i]);
        if (sp != kNoProp && prop_of(*h, sp) &&
            prop_int(*h, sp, 0) == ag)
            return true;
    }
    return false;
}

void World::epidemiology_drift(Entity& e, std::int64_t pending,
                               std::uint64_t now, CausalEvent& ev,
                               std::vector<SpawnSpec>& out) {
    (void)now;
    if (pending <= 0 || !e.alive) return;
    static thread_local SeqPidCache pid_c_;
    SeqPid pid(pid_c_, props);
    const PropId ps_p = pid("pathogen_strain"),
                 sa_p = pid("shed_accum"),
                 sr_p = pid("strain_ref"),
                 air_p = pid("airborne_rate"),
                 wat_p = pid("waterborne_rate"),
                 con_p = pid("contact_rate"),
                 bv_p = pid("blood_volume"),
                 br_p = pid("bleed_rate"),
                 inf_p = pid("infection_level");
    if (sa_p == kNoProp || ps_p == kNoProp) return;

    auto change_on = [&](Entity& tgt, PropId p, std::int64_t before,
                         std::int64_t after) {
        if (p == kNoProp || before == after) return;
        StateChange ch;
        ch.entity = tgt.id; ch.prop = p; ch.had_before = true;
        ch.before = Value::integer(before);
        ch.after = Value::integer(after);
        tgt.props.set(p, Value::integer(after));
        ev.changes.push_back(ch);
    };
    // organism = circulatory entity (blood_volume resolves); parts, waste
    // and flora never qualify, so they can neither catch nor spread.
    auto is_host = [&](const Entity& o) {
        return o.alive && bv_p != kNoProp && prop_of(o, bv_p) != nullptr;
    };
    auto carries_strain = [&](EntityId host, std::int64_t strain_eid) {
        for (const Relation& r : relations_of(host, "carries")) {
            if (r.a != host) continue;
            const Entity* ie = find(r.b);
            if (!ie || !ie->alive) continue;
            if (ps_p != kNoProp && prop_int(*ie, ps_p, 0) == strain_eid)
                return true;
        }
        return false;
    };
    auto spawn_infection = [&](EntityId host, std::int64_t strain_eid,
                               std::int64_t load) {
        queue_infection(out, host, strain_eid, load);
    };
    // deterministic target pick: sorted candidates + stream hash
    auto pick_target = [&](const std::vector<EntityId>& cands,
                           std::uint64_t mix, std::int64_t exclude_strain,
                           std::int64_t radius_kind) -> EntityId {
        std::vector<EntityId> ok;
        for (EntityId cid : cands) {
            const Entity* o = find(cid);
            if (!o || !is_host(*o) || o->id == e.id) continue;
            if (exclude_strain > 0 && carries_strain(cid, exclude_strain))
                continue;
            // herd immunity: a host carrying this strain's antigen in
            // immune memory cannot be reinfected — chains starve as the
            // recovered fraction of a cluster grows.
            if (exclude_strain > 0 && host_immune_to(cid, exclude_strain))
                continue;
            if (radius_kind == 1) {   // contact route: needs an open wound
                bool wounded = br_p != kNoProp && prop_int(*o, br_p, 0) > 0;
                if (!wounded && inf_p != kNoProp &&
                    prop_int(*o, inf_p, 0) > 200) wounded = true;
                if (!wounded) continue;
            }
            ok.push_back(cid);
        }
        if (ok.empty()) return kNoEntity;
        std::sort(ok.begin(), ok.end());
        const std::uint64_t h = Rng::stream_seed(seed, "epidemic_pick",
                                                e.id * 1315423911ull ^ mix);
        return ok[h % ok.size()];
    };

    // ---- waterborne: this entity is shed waste stamped with strain_ref ----
    if (sr_p != kNoProp && prop_of(e, sr_p)) {
        const std::int64_t seid = prop_int(e, sr_p, 0);
        Entity* strain = seid > 0 ? find(static_cast<EntityId>(seid)) : nullptr;
        if (strain && strain->alive) {
            const std::int64_t rate = wat_p != kNoProp
                ? prop_int(*strain, wat_p, 0) : 0;
            if (rate > 0) {
                const std::int64_t a0 = prop_int(e, sa_p, 0);
                const std::int64_t acc = a0 + pending * rate;
                const std::int64_t quanta = acc / 1000;
                // remainder persists EVERY call — otherwise per-tick
                // evaluation (rate < 1000) never crosses a quantum and
                // the accumulator silently resets each tick.
                change_on(e, sa_p, a0, acc - quanta * 1000);
                if (quanta > 0) {
                    const std::int64_t q = std::min<std::int64_t>(quanta, 8);
                    for (std::int64_t i = 0; i < q; ++i) {
                        const std::vector<EntityId> near =
                            spatial.within(e.pos, 48);
                        const EntityId t = pick_target(near,
                            static_cast<std::uint64_t>(acc) +
                            static_cast<std::uint64_t>(i) * 977,
                            seid, 0);
                        if (t != kNoEntity) {
                            spawn_infection(t, seid, 60);
                            ev.evidence += "waterborne transmission; ";
                        }
                    }
                }
            }
        }
        return;   // waste hosts no infections of its own
    }

    // Phase 11.3: a torpid vector neither sheds nor transmits — dormancy
    // is an ecological state (cold/dry season), not damage.
    {
        const PropId idv_p = pid("is_disease_vector"),
                     dm_p = pid("vector_dormant");
        if (idv_p != kNoProp && dm_p != kNoProp &&
            prop_of(e, idv_p) && prop_int(e, dm_p, 0) != 0)
            return;
    }

    // ---- airborne + contact: shed by each active infection on this host --
    if (!is_host(e)) return;
    for (const Relation& rel : relations_of(e.id, "carries")) {
        if (rel.a != e.id) continue;
        Entity* ie = find(rel.b);
        if (!ie || !ie->alive) continue;
        const std::int64_t seid = ps_p != kNoProp
            ? prop_int(*ie, ps_p, 0) : 0;
        Entity* strain = seid > 0 ? find(static_cast<EntityId>(seid)) : nullptr;
        if (!strain || !strain->alive) continue;
        const std::int64_t air = air_p != kNoProp
            ? prop_int(*strain, air_p, 0) : 0;
        const std::int64_t con = con_p != kNoProp
            ? prop_int(*strain, con_p, 0) : 0;
        const std::int64_t rate = air + con;
        if (rate <= 0) continue;
        const std::int64_t a0 = prop_int(*ie, sa_p, 0);
        const std::int64_t acc = a0 + pending * rate;
        const std::int64_t quanta = acc / 1000;
        // remainder persists every call (see waterborne branch above).
        change_on(*ie, sa_p, a0, acc - quanta * 1000);
        if (quanta <= 0) continue;
        const std::int64_t q = std::min<std::int64_t>(quanta, 8);
        for (std::int64_t i = 0; i < q; ++i) {
            // route split by the strain's own channel rates
            const std::uint64_t h = Rng::stream_seed(seed, "epidemic_route",
                ie->id ^ static_cast<std::uint64_t>(acc + i));
            const bool airborne = air > 0 &&
                (con <= 0 || h % static_cast<std::uint64_t>(rate) <
                 static_cast<std::uint64_t>(air));
            const std::vector<EntityId> near = airborne
                ? spatial.within(e.pos, 96) : spatial.within(e.pos, 24);
            const EntityId t = pick_target(near,
                static_cast<std::uint64_t>(acc) +
                static_cast<std::uint64_t>(i) * 977 +
                (airborne ? 0x9E3779B9ull : 0x85EBCA6Bull),
                seid, airborne ? 0 : 1);
            if (t != kNoEntity) {
                spawn_infection(t, seid, airborne ? 50 : 80);
                ev.evidence += airborne ? "airborne transmission; "
                                        : "contact transmission; ";
            }
        }
    }
}

void World::metabolize(std::int64_t dt) {
    // env-drift opt-in props, resolved once per call (not per entity)
    static thread_local SeqPidCache env_c_;
    std::vector<PropId> env_pids;
    {
        SeqPid ep(env_c_, props);
        for (const char* en : kEnvPropNames) env_pids.push_back(ep(en));
    }
    const PropId symbiont_p = SIMLAB_PID(props, "symbiont_signature");
    (void)dt;   // per-entity pending time replaces the bulk dt (Phase 9 LOD)
    const NeedProps np = need_props();
    const PropertyDef* idv_d = props.find_name("is_disease_vector");
    const PropId idv_p = idv_d ? idv_d->id : kNoProp;
    if ((np.metabolism == kNoProp && np.age == kNoProp) ||
        (np.hunger == kNoProp && np.thirst == kNoProp && np.fatigue == kNoProp &&
         np.integrity == kNoProp)) return;

    constexpr std::int64_t kDormantStride = 8;
    constexpr std::int64_t kLodRadius = 1024;
    const std::uint64_t now = clock_.tick();
    const bool lod = !minds_.empty();
    // Active = every mind itself + anything within kLodRadius of a living
    // mind. Test per ENTITY against a coarse grid of mind positions with an
    // early exit — in a crowd the first neighbour already answers, instead
    // of one spatial query (and set insert) per mind per tick.
    auto cell_of = [](std::int32_t v) -> std::int64_t {
        return v >= 0 ? v / kLodRadius : (v - kLodRadius + 1) / kLodRadius;
    };
    auto cell_key = [](std::int64_t cx, std::int64_t cy) {
        return (cx << 32) ^ (cy & 0xFFFFFFFFll);
    };
    std::unordered_map<std::int64_t, std::vector<Vec2>> mind_grid;
    if (lod) {
        for (const auto& [mid, mm] : minds_) {
            (void)mm;
            const Entity* me = find(mid);
            if (!me || !me->alive) continue;
            mind_grid[cell_key(cell_of(me->pos.x), cell_of(me->pos.y))]
                .push_back(me->pos);
        }
    }
    const std::int64_t dormant_stride =
        kDormantStride * lod_stride_;
    const std::int64_t lod_r2 = kLodRadius * kLodRadius;
    auto is_active = [&](const Entity& e) {
        if (minds_.count(e.id)) return true;
        // warp LOD: only minds stay hot — every other entity batches its
        // drift into the dormant catch-up path (accumulator-exact dt).
        if (lod_stride_ > 1) return false;
        const std::int64_t cx = cell_of(e.pos.x), cy = cell_of(e.pos.y);
        for (std::int64_t ax = cx - 1; ax <= cx + 1; ++ax)
            for (std::int64_t ay = cy - 1; ay <= cy + 1; ++ay) {
                const auto it = mind_grid.find(cell_key(ax, ay));
                if (it == mind_grid.end()) continue;
                for (const Vec2& mp : it->second)
                    if (dist2(mp, e.pos) <= lod_r2) return true;
            }
        return false;
    };
    // material prefilter: which materials can supply need/metabolism props
    // at all (instance props are checked per entity below). Lazily built —
    // registries are frozen after load_content.
    if (mat_need_flags_.size() != materials.defs().size()) {
        mat_need_flags_.assign(materials.defs().size(), 0);
        const PropId watch[] = {np.hunger, np.thirst, np.fatigue,
                                np.metabolism, np.integrity, np.age,
                                np.quantity, np.regrow_rate,
                                np.regrow_credit, np.max_quantity,
                                np.fruit_decay_rate,
                                np.fruit_count, np.max_fruit,
                                np.fruit_regrow_rate, np.fruit_spoil_rate,
                                np.fruit_credit,
                                np.decay_rate, np.decay_credit};
        for (const MaterialDef& md : materials.defs())
            for (PropId p : watch)
                if (p != kNoProp && md.props.count(p)) {
                    mat_need_flags_[md.id - 1] = 1; break;
                }
        for (const PropId edp : env_pids) {
            if (edp == kNoProp) continue;
            for (const MaterialDef& md : materials.defs())
                if (md.props.count(edp)) {
                    mat_need_flags_[md.id - 1] = 1; break;
                }
        }
    }

    // Phase 12.2: entities carrying ONLY economic props still need a pass
    const PropId econ_poss = SIMLAB_PID(props, "possessed_by");
    const PropId econ_theft = SIMLAB_PID(props, "theft_flag");

    CausalEvent ev;
    ev.tick = now;
    ev.action = "metabolize";
    ev.applied_laws.push_back(6);                      // physiology.metabolism
    bool any = false;
    std::vector<ShedPool> shed_pools;
    std::vector<SpawnSpec> drift_spawns;
    std::vector<EntityId> symbiont_hosts;
    for (const auto& ep : entities) {
        Entity& e = *ep;
        const std::uint64_t pending = now - e.last_metabolized;
        // per-entity phase stagger (id-hash) keeps catch-up bursts from
        // synchronizing across the whole dormant population during warp.
        const std::uint64_t cstride =
            static_cast<std::uint64_t>(dormant_stride) +
            (lod_stride_ > 1
                 ? e.id % static_cast<std::uint64_t>(lod_stride_) : 0);
        if (!e.alive) {
            // Phase 11: corpses are environmental reservoirs — they keep
            // decaying, emitting odor and shedding strain_ref long after
            // the organism's needs stopped mattering. Same LOD contract:
            // dormant catch-up is one batched, accumulator-exact update.
            if (pending > 0) {
                if (lod && pending < cstride && !is_active(e))
                    continue;
                const std::size_t chg0 = ev.changes.size();
                environmental_drift(e, pending, ev);
                if (ev.changes.size() != chg0) {
                    ev.participants.push_back(e.id);
                    any = true;
                }
                e.last_metabolized = now;
            }
            continue;
        }
        // LOD: dormant entities wait until enough ticks accrued — cheapest
        // possible reject before ANY property lookup.
        if (lod && pending < cstride && !is_active(e))
            continue;
        // reap first: integrity at 0 destroys the organism — same rule the
        // interact commit applies, extended to metabolism-driven death.
        if (np.integrity != kNoProp) {
            const Value* iv = prop_of(e, np.integrity);
            if (iv && iv->type == ValType::Integer && iv->i <= 0) {
                e.alive = false;
                spatial.erase(e.id);
                ev.evidence += "died (integrity 0); ";
                any = true;
                continue;
            }
        }
        // cheap prefilter: genome carriers age; others need an instance or
        // material-supplied need/metabolism/integrity prop to do any work
        bool maybe = !e.genome.loci.empty();
        if (!maybe) {
            const PropId watch[] = {np.hunger, np.thirst, np.fatigue,
                                    np.age, np.metabolism, np.integrity,
                                    np.quantity, np.regrow_rate,
                                    np.regrow_credit, np.max_quantity,
                                    np.fruit_decay_rate,
                                    np.fruit_count, np.max_fruit,
                                    np.fruit_regrow_rate, np.fruit_spoil_rate,
                                    np.fruit_credit,
                                    np.decay_rate, np.decay_credit};
            for (PropId p : watch)
                if (p != kNoProp && e.props.get(p)) { maybe = true; break; }
            if (!maybe)
                for (const PropId edp : env_pids)
                    if (edp != kNoProp && e.props.get(edp)) { maybe = true; break; }
            if (!maybe && (econ_poss != kNoProp &&
                           e.props.get(econ_poss)) ||
                          (econ_theft != kNoProp &&
                           e.props.get(econ_theft)) ||
                          e.kind_name == SIMLAB_KIND("debt_obligation"))
                maybe = true;
            if (!maybe && (e.kind_name == SIMLAB_KIND("enclosure") ||
                           e.kind_name == SIMLAB_KIND("portal")))
                maybe = true;
            if (!maybe && e.material != kNoMaterial &&
                e.material - 1 < mat_need_flags_.size() &&
                mat_need_flags_[e.material - 1])
                maybe = true;
        }
        if (!maybe) { e.last_metabolized = now; continue; }
        if (pending == 0) { e.last_metabolized = now; continue; }
        bool drifted = false;
        const std::size_t chg0 = ev.changes.size();
        // Vectors always replay tick-wise on catch-up: their dormancy,
        // biting and brood quanta interleave with need-driven death and
        // with the shared spawn queue exactly like advance(1) x N would.
        const bool is_vec = idv_p != kNoProp &&
            prop_of(e, idv_p) && prop_int(e, idv_p, 0) != 0;
        if (pending > 1 && (physiology_coupled(e) || is_vec)) {
            // coupled physiology replays tick-by-tick — the only way a
            // batched catch-up stays bit-identical to advance(1) x N.
            for (std::uint64_t k = 0; k < pending && e.alive; ++k) {
                const std::uint64_t sub_now = now - pending + k + 1;
                if (drift_needs(e, 1, np, ev, sub_now)) drifted = true;
                anatomy_drift(e, 1, sub_now, ev, shed_pools, &drift_spawns);
                epidemiology_drift(e, 1, sub_now, ev, drift_spawns);
                morale_drift(e, 1, ev);
                flora_drift(e, 1, sub_now, ev, drift_spawns);
                if (is_vec) vector_drift(e, 1, sub_now, ev, drift_spawns);
                if (!speculative) {
                    language_drift(e, 1, sub_now, ev);
                    econ_drift(e, 1, sub_now, ev);
                    arch_drift(e, 1, sub_now, ev, drift_spawns);
                }
            }
        } else {
            drifted = drift_needs(e, pending, np, ev, now);
            // Phase 10: circulation for blood_volume carriers.
            anatomy_drift(e, pending, now, ev, shed_pools, &drift_spawns);
            epidemiology_drift(e, static_cast<std::int64_t>(pending), now, ev,
                               drift_spawns);
            morale_drift(e, pending, ev);
            flora_drift(e, static_cast<std::int64_t>(pending), now, ev,
                        drift_spawns);
            vector_drift(e, static_cast<std::int64_t>(pending), now, ev,
                         drift_spawns);
            if (!speculative) {
                language_drift(e, static_cast<std::int64_t>(pending), now, ev);
                econ_drift(e, static_cast<std::int64_t>(pending), now, ev);
                arch_drift(e, static_cast<std::int64_t>(pending), now, ev,
                           drift_spawns);
            }
        }
        // Phase 11: standing water fouls into a stagnation reservoir on the
        // same batched, accumulator-exact contract as every other drift.
        environmental_drift(e, pending, ev);
        // Lazy symbiont: hosts carrying a symbiont_signature template get
        // their commensal strain seeded post-loop with real entity ids.
        {
            if (symbiont_p != kNoProp && prop_int(e, symbiont_p, 0) > 0)
                symbiont_hosts.push_back(e.id);
        }
        if (drifted || ev.changes.size() != chg0) {
            ev.participants.push_back(e.id);
            any = true;
        }
        e.last_metabolized = now;
    }
    if (!symbiont_hosts.empty()) {
        seed_symbionts(ev, symbiont_hosts);
        any = true;
    }
    // shed waste lands as material entities — World Truth grows causally
    // inside the same metabolize event (blood pools, feces, urine).
    if (!shed_pools.empty()) {
        const PropId qty = SIMLAB_PID(props, "quantity");
        for (const ShedPool& sp : shed_pools) {
            PropertyBag pb;
            if (qty != kNoProp) pb.set(qty, Value::integer(sp.amount));
            const EntityId bid = create_entity(
                sp.kind, sp.material, sp.pos, pb);
            if (bid != kNoEntity) {
                ev.spawned.push_back(bid);
                ev.evidence += sp.kind + " " + std::to_string(bid) + "; ";
                any = true;
            }
        }
    }
    if (!drift_spawns.empty()) {
        commit_drift_spawns(ev, drift_spawns);
        any = true;
    }
    if (any) {
        ev.evidence = "needs drift with metabolism; " + ev.evidence;
        ledger.append(std::move(ev));
    }
}

// Phase 11 — environmental reservoirs. One batched, accumulator-exact pass
// per metabolize call, identical contract to drift_needs: every remainder is
// written unconditionally, every quantum loop is bounded, buried/cremated
// corpses go silent immediately.
void World::environmental_drift(Entity& e, std::uint64_t pending_u,
                                CausalEvent& ev, bool record_hist) {
    if (pending_u == 0) return;
    const std::int64_t pending = static_cast<std::int64_t>(pending_u);
    // Phase 11.3: snapshot the pre-window stagnation mark so vectors can
    // evaluate "was the water fouled at tick T" for ANY T inside this
    // window — stagnation is affine in elapsed ticks, so one mark plus
    // the material rate reconstructs the whole trajectory exactly.
    if (record_hist && e.alive) {
        static thread_local SeqPidCache hpid_c_;
    SeqPid hpid(hpid_c_, props);
        const PropId stl = hpid("stagnation_level"),
                     sta = hpid("stagnation_accum"),
                     sra = hpid("stagnation_rate"),
                     flo = hpid("flow_rate"),
                     hl = hpid("stagnation_hist_level"),
                     ha = hpid("stagnation_hist_acc"),
                     hr = hpid("stagnation_hist_rate"),
                     hs = hpid("stagnation_hist_start");
        if (stl != kNoProp && prop_of(e, stl) && hl != kNoProp &&
            ha != kNoProp && hr != kNoProp && hs != kNoProp &&
            !(flo != kNoProp && prop_int(e, flo, 0) > 0)) {
            const std::int64_t rate = sra != kNoProp
                ? prop_int(e, sra, 0) : 0;
            if (rate > 0) {
                auto hset = [&](PropId p, std::int64_t v) {
                    if (p == kNoProp) return;
                    const Value* cur = prop_of(e, p);
                    const std::int64_t before = cur ? cur->i : 0;
                    if (before == v && cur) return;
                    StateChange ch;
                    ch.entity = e.id; ch.prop = p;
                    ch.had_before = cur != nullptr;
                    ch.before = cur ? *cur : Value::integer(0);
                    ch.after = Value::integer(v);
                    e.props.set(p, Value::integer(v));
                    ev.changes.push_back(ch);
                };
                hset(hl, prop_int(e, stl, 0));
                hset(ha, sta != kNoProp ? prop_int(e, sta, 0) : 0);
                hset(hr, rate);
                hset(hs, static_cast<std::int64_t>(clock_.tick()) - pending);
            }
        }
    }
    // LOD guard: reservoir levels are monotonic, so emission switches on
    // exactly once. A batch that straddles the threshold would emit for
    // the whole window while dt=1 ticks emit only after crossing — the
    // spread/odor accumulators would diverge. Replay tick-wise instead.
    if (pending > 1) {
        static thread_local SeqPidCache xpid_c_;
    SeqPid xpid(xpid_c_, props);
        bool cross = false;
        if (!e.alive) {
            const PropId cdl = xpid("corpse_decay_level"),
                         cda = xpid("corpse_decay_accum"),
                         buried = xpid("buried"),
                         crem = xpid("cremated"),
                         dec = xpid("decay_rate");
            if (cdl != kNoProp && cda != kNoProp &&
                !(buried != kNoProp && prop_int(e, buried, 0) != 0) &&
                !(crem != kNoProp && prop_int(e, crem, 0) != 0) &&
                prop_of(e, cdl)) {
                const std::int64_t lvl0 = prop_int(e, cdl, 0);
                const std::int64_t rate = dec != kNoProp
                    ? prop_int(e, dec, 0) : 0;
                const std::int64_t acc = prop_int(e, cda, 0) +
                    pending * rate;
                const std::int64_t nl = std::min<std::int64_t>(1000,
                    lvl0 + acc / 1000);
                cross = lvl0 <= 200 && nl > 200;
            }
        } else {
            const PropId stl = xpid("stagnation_level"),
                         sta = xpid("stagnation_accum"),
                         sra = xpid("stagnation_rate"),
                         flow = xpid("flow_rate");
            if (stl != kNoProp && sta != kNoProp && prop_of(e, stl) &&
                !(flow != kNoProp && prop_int(e, flow, 0) > 0)) {
                const std::int64_t rate = sra != kNoProp
                    ? prop_int(e, sra, 0) : 0;
                if (rate > 0) {
                    const std::int64_t lvl0 = prop_int(e, stl, 0);
                    const std::int64_t acc = prop_int(e, sta, 0) +
                        pending * rate;
                    const std::int64_t nl = std::min<std::int64_t>(1000,
                        lvl0 + acc / 1000);
                    cross = lvl0 <= 500 && nl > 500;
                }
            }
        }
        if (cross) {
            for (std::int64_t i = 0; i < pending; ++i)
                environmental_drift(e, 1, ev, false);
            return;
        }
    }
    static thread_local SeqPidCache pid_c_;
    SeqPid pid(pid_c_, props);
    const PropId cdl_p = pid("corpse_decay_level"),
                 cda_p = pid("corpse_decay_accum"),
                 buried_p = pid("buried"),
                 cremated_p = pid("cremated"),
                 stl_p = pid("stagnation_level"),
                 sta_p = pid("stagnation_accum"),
                 sra_p = pid("stagnation_rate"),
                 flow_p = pid("flow_rate"),
                 rsp_p = pid("reservoir_spread_accum"),
                 odor_p = pid("odor"),
                 cont_p = pid("contamination"),
                 sr_p = pid("strain_ref"),
                 dec_p = pid("decay_rate"),
                 ps_p = pid("pathogen_strain"),
                 spr_p = pid("spore_rate"),
                 pl_p = pid("pathogen_load"),
                 ssl_p = pid("soil_spore_load"),
                 ssr_p = pid("soil_strain_ref"),
                 sda_p = pid("spore_decay_accum"),
                 sdr_p = pid("spore_decay_rate"),
                 dep_p = pid("spores_deposited");
    auto change_on = [&](Entity& tgt, PropId p, std::int64_t before,
                         std::int64_t after) {
        if (p == kNoProp || before == after) return;
        StateChange ch;
        ch.entity = tgt.id; ch.prop = p; ch.had_before = true;
        ch.before = Value::integer(before);
        ch.after = Value::integer(after);
        tgt.props.set(p, Value::integer(after));
        ev.changes.push_back(ch);
    };
    // prop_of already falls back to material props — presence anywhere in
    // the entity+material chain is what makes a reservoir candidate.
    auto has_prop = [&](PropId p) { return p != kNoProp && prop_of(e, p); };
    // instance-marker write: observable corpse_decay_level must exist as an
    // ENTITY prop (not just the material default) so perception/snapshots
    // can tell a corpse from the living — e.props.get is instance-only.
    auto ensure_instance = [&](PropId p, std::int64_t v) {
        if (p == kNoProp || e.props.get(p)) return;
        StateChange ch;
        ch.entity = e.id; ch.prop = p; ch.had_before = false;
        ch.before = Value::integer(0);
        ch.after = Value::integer(v);
        e.props.set(p, Value::integer(v));
        ev.changes.push_back(ch);
    };

    std::int64_t emit_level = 0;      // >threshold -> reservoir is emitting
    bool is_corpse = false;
    if (!e.alive) {
        is_corpse = true;
        if ((cremated_p != kNoProp && prop_int(e, cremated_p, 0) != 0) ||
            (buried_p != kNoProp && prop_int(e, buried_p, 0) != 0)) {
            // interred/burned remains leave the world again: out of the
            // spatial index, invisible to perception and interaction.
            if (spatial.contains(e.id)) spatial.erase(e.id);
            return;                                    // inert / interred
        }
        // Phase 11.2: a corpse carrying a plant pathogen leaches its
        // spore load into the soil beneath it — once. The patch becomes a
        // persistent reservoir that infects future susceptible sprouts.
        if (dep_p != kNoProp && prop_int(e, dep_p, 0) == 0 &&
            spr_p != kNoProp && ps_p != kNoProp) {
            std::int64_t best_strain = 0, best_load = 0, total = 0;
            for (const Relation& rel : relations_of(e.id, "carries")) {
                if (rel.a != e.id) continue;
                const Entity* ie = find(rel.b);
                if (!ie || !ie->alive) continue;
                const std::int64_t seid = prop_int(*ie, ps_p, 0);
                const Entity* st = seid > 0
                    ? find(static_cast<EntityId>(seid)) : nullptr;
                if (!st || !st->alive) continue;
                if (prop_int(*st, spr_p, 0) <= 0) continue;
                const std::int64_t ld = prop_int(*ie, pl_p, 0);
                total += ld;
                if (ld > best_load) { best_load = ld; best_strain = seid; }
            }
            if (total > 0 && ssl_p != kNoProp && ssr_p != kNoProp) {
                const std::int64_t deposit =
                    std::min<std::int64_t>(400, std::max<std::int64_t>(100, total / 3));
                // find-or-create the soil patch at this spot (sorted near)
                EntityId patch_id = kNoEntity;
                {
                    std::vector<EntityId> near = spatial.within(e.pos, 24);
                    std::sort(near.begin(), near.end());
                    for (EntityId nid : near) {
                        Entity* o = find(nid);
                        if (o && o->alive && prop_of(*o, ssl_p)) {
                            patch_id = nid; break;
                        }
                    }
                }
                if (patch_id == kNoEntity) {
                    const MaterialId soil_mat = materials.id_of("soil_loam");
                    PropertyBag pb;
                    pb.set(ssl_p, Value::integer(0));
                    pb.set(ssr_p, Value::integer(0));
                    const PropertyDef* snu = props.find_name("soil_nutrient");
                    if (snu) pb.set(snu->id, Value::integer(0));
                    const PropertyDef* sda = props.find_name("spore_decay_accum");
                    if (sda) pb.set(sda->id, Value::integer(0));
                    const PropId cont_i = pid("contamination");
                    if (cont_i != kNoProp) pb.set(cont_i, Value::integer(0));
                    const PropId shk = pid("soil_host_kind");
                    if (shk != kNoProp)
                        pb.set(shk, Value::text(e.kind_name));
                    patch_id = create_entity("soil_patch", soil_mat, e.pos, pb);
                    if (patch_id != kNoEntity) {
                        ev.spawned.push_back(patch_id);
                        ev.evidence += "soil_patch " +
                            std::to_string(patch_id) + " formed; ";
                    }
                }
                if (patch_id != kNoEntity) {
                    if (Entity* patch = find(patch_id)) {
                        const std::int64_t cur = prop_int(*patch, ssl_p, 0);
                        change_on(*patch, ssl_p, cur,
                                  std::min<std::int64_t>(1000, cur + deposit));
                        if (cur == 0 && best_strain > 0)
                            change_on(*patch, ssr_p,
                                      prop_int(*patch, ssr_p, 0), best_strain);
                    }
                    change_on(e, dep_p, 0, 1);
                    ev.evidence += "spores leached to soil; ";
                }
            }
        }
        if (!has_prop(cdl_p)) return;                  // not a decaying remain
        // corpses re-enter the spatial index so they can be seen, smelled
        // and interacted with (consume/bury/heat) — death erases them once,
        // the reservoir pass restores the physical presence.
        if (!spatial.contains(e.id)) spatial.insert(e.id, e.pos);
        ensure_instance(cdl_p, 0);
        ensure_instance(cda_p, 0);   // remainder prop must exist even when
                                     // a batch lands exactly on its start
        const std::int64_t rate = dec_p != kNoProp
            ? prop_int(e, dec_p, 0) : 0;               // permille/tick, mat fallback
        if (rate > 0 && cda_p != kNoProp) {
            const std::int64_t a0 = prop_int(e, cda_p, 0);
            const std::int64_t acc = a0 + pending * rate;
            const std::int64_t quanta = acc / 1000;
            const std::int64_t lvl0 = prop_int(e, cdl_p, 0);
            const std::int64_t nl = std::min<std::int64_t>(1000, lvl0 + quanta);
            change_on(e, cdl_p, lvl0, nl);
            change_on(e, cda_p, a0, acc - quanta * 1000);
            if (nl > 200) emit_level = nl;
        } else if (prop_int(e, cdl_p, 0) > 200) {
            emit_level = prop_int(e, cdl_p, 0);
        }
    } else {
        // Phase 11.2: a soil patch is a living-world entity whose spore
        // bank decays when no host replenishes it — fallowing self-cleans.
        if (ssl_p != kNoProp && prop_of(e, ssl_p)) {
            const std::int64_t srate = sdr_p != kNoProp
                ? prop_int(e, sdr_p, 0) : 0;
            if (srate > 0 && sda_p != kNoProp) {
                const std::int64_t a0 = prop_int(e, sda_p, 0);
                const std::int64_t acc = a0 + pending * srate;
                const std::int64_t quanta = acc / 1000;
                const std::int64_t l0 = prop_int(e, ssl_p, 0);
                const std::int64_t nl = std::max<std::int64_t>(0, l0 - quanta);
                change_on(e, ssl_p, l0, nl);
                change_on(e, sda_p, a0, acc - quanta * 1000);
                if (nl <= 0 && ssr_p != kNoProp &&
                    prop_int(e, ssr_p, 0) != 0)
                    change_on(e, ssr_p, prop_int(e, ssr_p, 0), 0);
            }
            return;                     // soil is not water: no emit pass
        }
        // living entity: only standing (flow_rate <= 0) water fouls
        if (!has_prop(stl_p)) return;
        if (flow_p != kNoProp && prop_int(e, flow_p, 0) > 0) return;
        const std::int64_t rate = sra_p != kNoProp
            ? prop_int(e, sra_p, 0) : 0;               // milli-units/tick
        if (rate <= 0 || sta_p == kNoProp) return;
        ensure_instance(sta_p, 0);
        const std::int64_t a0 = prop_int(e, sta_p, 0);
        const std::int64_t acc = a0 + pending * rate;
        const std::int64_t quanta = acc / 1000;
        const std::int64_t lvl0 = prop_int(e, stl_p, 0);
        const std::int64_t nl = std::min<std::int64_t>(1000, lvl0 + quanta);
        change_on(e, stl_p, lvl0, nl);
        change_on(e, sta_p, a0, acc - quanta * 1000);
        if (nl > 500) emit_level = nl;
    }
    if (emit_level <= 0) return;

    // observable stench on corpses; both reservoir kinds raise contamination
    if (is_corpse && odor_p != kNoProp) {
        const std::int64_t ov = std::min<std::int64_t>(1000,
            (emit_level - 200) * 4);
        change_on(e, odor_p, prop_int(e, odor_p, 0), ov);
    }
    if (cont_p != kNoProp) {
        const std::int64_t ct = is_corpse
            ? std::min<std::int64_t>(1000, emit_level)
            : std::min<std::int64_t>(1000, 200 + (emit_level - 500));
        change_on(e, cont_p, prop_int(e, cont_p, 0),
                  std::max(prop_int(e, cont_p, 0), ct));
    }

    // emission quanta: every 250 ticks of reservoir time, the rot/fouling
    // reaches neighbouring cells — contamination rises and any strain the
    // reservoir carries (corpse keeps carries->infection edges; stamped
    // water/waste carries strain_ref) propagates to nearby entities.
    if (rsp_p == kNoProp) return;
    ensure_instance(rsp_p, 0);
    const std::int64_t r0 = prop_int(e, rsp_p, 0);
    const std::int64_t racc = r0 + pending * 1000;    // milli-ticks
    std::int64_t squanta = racc / 250000;       // one per 250 ticks
    if (squanta <= 0) {
        change_on(e, rsp_p, r0, racc);
        return;
    }
    const std::int64_t q = std::min<std::int64_t>(squanta, 32);
    // unprocessed quanta stay in the accumulator — catch-up is lossless.
    change_on(e, rsp_p, r0, racc - q * 250000);
    squanta = q;

    // strains this reservoir can pass on (sorted, deduped, capped)
    std::vector<std::int64_t> strains;
    for (const Relation& rel : relations_of(e.id, "carries")) {
        if (rel.a != e.id) continue;
        const Entity* ie = find(rel.b);
        if (!ie || !ie->alive || ps_p == kNoProp) continue;
        const std::int64_t seid = prop_int(*ie, ps_p, 0);
        if (seid > 0) strains.push_back(seid);
    }
    if (sr_p != kNoProp) {
        const std::int64_t sr = prop_int(e, sr_p, 0);
        if (sr > 0) strains.push_back(sr);
    }
    std::sort(strains.begin(), strains.end());
    strains.erase(std::unique(strains.begin(), strains.end()), strains.end());
    if (strains.size() > 4) strains.resize(4);
    const std::int64_t first_strain = strains.empty() ? 0 : strains.front();

    for (std::int64_t qi = 0; qi < q; ++qi) {
        std::vector<EntityId> near = spatial.within(e.pos, 64);
        std::sort(near.begin(), near.end());
        std::int64_t done = 0;
        for (EntityId cid : near) {
            if (done >= 6) break;
            if (cid == e.id) continue;
            Entity* o = find(cid);
            if (!o) continue;
            // reservoirs spread filth onto anything that can hold it:
            // contamination-capable matter or water (hydration carriers).
            const bool holds = (cont_p != kNoProp && prop_of(*o, cont_p)) ||
                               prop_int(*o, pid("hydration"), 0) > 0;
            if (!holds) continue;
            if (cont_p != kNoProp) {
                const std::int64_t c0 = prop_int(*o, cont_p, 0);
                change_on(*o, cont_p, c0, std::min<std::int64_t>(1000, c0 + 80));
            }
            const PropertyDef* bv_d = props.find_name("blood_volume");
            const bool is_hostile = bv_d && prop_of(*o, bv_d->id);
            if (first_strain > 0 && sr_p != kNoProp && !is_hostile &&
                prop_int(*o, sr_p, 0) == 0)
                change_on(*o, sr_p, 0, first_strain);
            ++done;
        }
    }
}

// Phase 11.3 — vector ecology. A disease vector is an ordinary organism
// whose activity is gated by the LOCAL environment: terrain temperature
// inside its declared band plus (optionally) a nearby stagnant-water
// reservoir. Dormant vectors neither bite nor breed — epizootics breathe
// with the seasons for free. Water state is read through the pond's
// stagnation_hist_* mark (written by environmental_drift) so "was the
// water fouled at tick T" is answered exactly for any T in the window —
// the same affine trajectory per-tick evaluation sees.
void World::vector_drift(Entity& e, std::int64_t pending, std::uint64_t now,
                         CausalEvent& ev, std::vector<SpawnSpec>& out) {
    if (pending <= 0 || !e.alive) return;
    static thread_local SeqPidCache pid_c_;
    SeqPid pid(pid_c_, props);
    const PropId idv_p = pid("is_disease_vector"),
                 dm_p = pid("vector_dormant"),
                 tmin_p = pid("vector_optimal_temp_min"),
                 tmax_p = pid("vector_optimal_temp_max"),
                 reqw_p = pid("vector_requires_stagnant_water"),
                 smin_p = pid("vector_stagnation_min"),
                 brad_p = pid("vector_bite_radius"),
                 brate_p = pid("vector_bite_rate"),
                 bacc_p = pid("vector_bite_accum"),
                 broll_p = pid("vector_bite_rolls"),
                 brood_p = pid("vector_brood_rolls"),
                 fec_p = pid("vector_fecundity"),
                 vacc_p = pid("vector_breed_accum"),
                 cload_p = pid("vector_carrier_load"),
                 cap_p = pid("vector_brood_cap"),
                 stl_p = pid("stagnation_level"),
                 flo_p = pid("flow_rate"),
                 hl_p = pid("stagnation_hist_level"),
                 ha_p = pid("stagnation_hist_acc"),
                 hr_p = pid("stagnation_hist_rate"),
                 hs_p = pid("stagnation_hist_start"),
                 bv_p = pid("blood_volume"),
                 ps_p = pid("pathogen_strain"),
                 vbr_p = pid("vector_borne_rate");
    if (idv_p == kNoProp || !prop_of(e, idv_p) ||
        prop_int(e, idv_p, 0) == 0)
        return;                                   // not a vector

    auto change_on = [&](Entity& tgt, PropId p, std::int64_t before,
                         std::int64_t after) {
        if (p == kNoProp || before == after) return;
        StateChange ch;
        ch.entity = tgt.id; ch.prop = p; ch.had_before = true;
        ch.before = Value::integer(before);
        ch.after = Value::integer(after);
        tgt.props.set(p, Value::integer(after));
        ev.changes.push_back(ch);
    };

    // ---- environment -------------------------------------------------------
    // Ambient fallback matches the engine's neutral 20 °C convention used
    // for entity temperature defaults; real worlds always carry terrain.
    std::int64_t env_temp = 200;
    if (!terrain.empty() && terrain.contains(e.pos))
        env_temp = terrain.temp_at(e.pos);
    const std::int64_t tmin = tmin_p != kNoProp
        ? prop_int(e, tmin_p, -1000) : -1000;
    const std::int64_t tmax = tmax_p != kNoProp
        ? prop_int(e, tmax_p, 2000) : 2000;
    const bool temp_ok = env_temp >= tmin && env_temp <= tmax;
    const bool req_water = reqw_p != kNoProp && prop_of(e, reqw_p) &&
                           prop_int(e, reqw_p, 0) != 0;
    const std::int64_t stag_min = smin_p != kNoProp
        ? prop_int(e, smin_p, 500) : 500;
    const std::int64_t bite_r = brad_p != kNoProp
        ? std::max<std::int64_t>(1, prop_int(e, brad_p, 48)) : 48;
    const std::int64_t bite_rate = brate_p != kNoProp
        ? prop_int(e, brate_p, 0) : 0;
    const std::int64_t fec = fec_p != kNoProp ? prop_int(e, fec_p, 0) : 0;

    // stagnant-water candidates in biting range (positions are frozen for
    // the whole metabolize pass — the set is identical every sub-tick)
    struct Pond {
        const Entity* e = nullptr;
        std::int64_t hl = 0, ha = 0, hr = 0, hs = 0;   // hist mark
        bool has_hist = false;
    };
    std::vector<Pond> ponds;
    if (req_water && stl_p != kNoProp) {
        std::vector<EntityId> near = spatial.within(e.pos, bite_r);
        std::sort(near.begin(), near.end());
        for (EntityId nid : near) {
            const Entity* o = find(nid);
            if (!o || !o->alive || o->id == e.id) continue;
            if (!prop_of(*o, stl_p)) continue;
            if (flo_p != kNoProp && prop_int(*o, flo_p, 0) > 0) continue;
            Pond pd; pd.e = o;
            if (hl_p != kNoProp && ha_p != kNoProp &&
                hr_p != kNoProp && hs_p != kNoProp &&
                prop_of(*o, hl_p) && prop_of(*o, ha_p) &&
                prop_of(*o, hr_p) && prop_of(*o, hs_p)) {
                pd.hl = prop_int(*o, hl_p, 0);
                pd.ha = prop_int(*o, ha_p, 0);
                pd.hr = prop_int(*o, hr_p, 0);
                pd.hs = prop_int(*o, hs_p, 0);
                pd.has_hist = true;
            }
            ponds.push_back(pd);
        }
    }
    // pond stagnation level at absolute tick T — reconstructed from the
    // hist mark when it covers T, else the live value (best available).
    auto pond_level_at = [&](const Pond& pd, std::int64_t T) -> std::int64_t {
        if (!pd.has_hist) return prop_int(*pd.e, stl_p, 0);
        if (T > static_cast<std::int64_t>(now))
            return prop_int(*pd.e, stl_p, 0);
        if (T < pd.hs) return pd.hl;          // before the mark: upper bound
        const std::int64_t el = T - pd.hs;
        const std::int64_t quanta =
            (pd.ha + el * pd.hr) / 1000;
        return std::min<std::int64_t>(1000, pd.hl + quanta);
    };
    auto wet_at = [&](std::int64_t T) -> bool {
        for (const Pond& pd : ponds)
            if (pond_level_at(pd, T) > stag_min) return true;
        return false;
    };

    // ---- per-tick work -----------------------------------------------------
    auto is_host = [&](const Entity& o) {
        return o.alive && bv_p != kNoProp && prop_of(o, bv_p) != nullptr;
    };
    auto carries_strain = [&](EntityId host, std::int64_t strain_eid) {
        for (const Relation& r : relations_of(host, "carries")) {
            if (r.a != host) continue;
            const Entity* ie = find(r.b);
            if (!ie || !ie->alive) continue;
            if (ps_p != kNoProp && prop_int(*ie, ps_p, 0) == strain_eid)
                return true;
        }
        // infections queued this pass are not entities yet — a batched
        // window must see the same "already infected" state ticked mode
        // would see on the next tick.
        for (const SpawnSpec& sp : out) {
            bool is_carries = false;
            for (const auto& [t, o] : sp.rel_from)
                if (t == "carries" && o == host) is_carries = true;
            if (!is_carries) continue;
            const Value* v = sp.props.get(ps_p);
            if (v && v->type == ValType::Integer && v->i == strain_eid)
                return true;
        }
        return false;
    };
    // strains this vector can inject: its real carries plus infections
    // already queued onto it this pass.
    auto carried_vbr_strains = [&]() -> std::vector<std::pair<std::int64_t, std::int64_t>> {
        std::vector<std::pair<std::int64_t, std::int64_t>> res; // (strain,vbr)
        auto consider = [&](std::int64_t seid) {
            if (seid <= 0 || vbr_p == kNoProp) return;
            const Entity* st = find(static_cast<EntityId>(seid));
            if (!st || !st->alive) return;
            const std::int64_t vbr = prop_int(*st, vbr_p, 0);
            if (vbr <= 0) return;
            for (auto& pr : res)
                if (pr.first == seid) return;
            res.push_back({seid, vbr});
        };
        if (ps_p != kNoProp)
            for (const Relation& r : relations_of(e.id, "carries")) {
                if (r.a != e.id) continue;
                const Entity* ie = find(r.b);
                if (ie && ie->alive)
                    consider(prop_int(*ie, ps_p, 0));
            }
        for (const SpawnSpec& sp : out) {
            bool is_carries = false;
            for (const auto& [t, o] : sp.rel_from)
                if (t == "carries" && o == e.id) is_carries = true;
            if (!is_carries) continue;
            const Value* v = sp.props.get(ps_p);
            if (v && v->type == ValType::Integer)
                consider(v->i);
        }
        std::sort(res.begin(), res.end());
        return res;
    };
    // hosts within bite range that could donate a strain to this vector
    auto pickup_candidates = [&]() {
        std::vector<std::pair<EntityId, std::int64_t>> res; // (host,strain)
        std::vector<EntityId> near = spatial.within(e.pos, bite_r);
        std::sort(near.begin(), near.end());
        for (EntityId nid : near) {
            const Entity* o = find(nid);
            if (!o || !is_host(*o) || o->id == e.id) continue;
            for (const Relation& r : relations_of(nid, "carries")) {
                if (r.a != nid) continue;
                const Entity* ie = find(r.b);
                if (!ie || !ie->alive || ps_p == kNoProp) continue;
                const std::int64_t seid = prop_int(*ie, ps_p, 0);
                const Entity* st = seid > 0
                    ? find(static_cast<EntityId>(seid)) : nullptr;
                if (!st || !st->alive || vbr_p == kNoProp) continue;
                if (prop_int(*st, vbr_p, 0) <= 0) continue;
                if (carries_strain(e.id, seid)) continue;
                res.push_back({nid, seid});
            }
        }
        std::sort(res.begin(), res.end());
        res.erase(std::unique(res.begin(), res.end()), res.end());
        return res;
    };
    // hosts within bite range that could receive an injected strain
    auto inject_candidates = [&](std::int64_t strain) {
        std::vector<EntityId> res;
        std::vector<EntityId> near = spatial.within(e.pos, bite_r);
        std::sort(near.begin(), near.end());
        for (EntityId nid : near) {
            const Entity* o = find(nid);
            if (!o || !is_host(*o) || o->id == e.id) continue;
            if (carries_strain(nid, strain)) continue;
            if (host_immune_to(nid, strain)) continue;
            res.push_back(nid);
        }
        return res;
    };

    std::uint64_t brood_n = brood_p != kNoProp
        ? static_cast<std::uint64_t>(prop_int(e, brood_p, 0)) : 0;
    auto do_bite = [&](std::uint64_t roll_epoch) {
        // inject first: a loaded vector passes its strain on; only a
        // vector with nothing to give goes looking for a new strain.
        const auto carried = carried_vbr_strains();
        if (!carried.empty()) {
            const auto& [seid, vbr] = carried.front();
            const std::vector<EntityId> cands = inject_candidates(seid);
            if (!cands.empty()) {
                const std::uint64_t h = Rng::stream_seed(seed,
                    "vector_bite", static_cast<std::uint64_t>(e.id) ^
                    roll_epoch);
                const EntityId tgt = cands[h % cands.size()];
                queue_infection(out, tgt, seid, vbr);
                ev.evidence += "vector bite transmission; ";
                return;
            }
        }
        const auto pickup = pickup_candidates();
        if (!pickup.empty()) {
            const std::uint64_t h = Rng::stream_seed(seed,
                "vector_pickup", static_cast<std::uint64_t>(e.id) ^
                roll_epoch);
            const auto& [host, seid] = pickup[h % pickup.size()];
            const std::int64_t load = cload_p != kNoProp
                ? prop_int(e, cload_p, 120) : 120;
            queue_infection(out, e.id, seid, load);
            ev.evidence += "vector picked up strain; ";
        }
    };
    auto do_brood = [&](std::uint64_t qi) {
        // species defaults give the newborn its full declared prop set;
        // genome loci then overwrite with the inherited alleles — the
        // same phenotype=allele rule BiologyModel uses.
        SpawnSpec s;
        s.kind = e.kind_name; s.material = e.material;
        const std::uint64_t h = Rng::stream_seed(seed, "vector_brood",
            static_cast<std::uint64_t>(e.id) ^ (brood_n + qi) * 2654435761ull);
        s.pos = {e.pos.x + static_cast<std::int32_t>(h % 49) - 24,
                 e.pos.y + static_cast<std::int32_t>((h >> 16) % 49) - 24};
        if (const SpeciesDef* sp = species.find_name(e.kind_name)) {
            for (const auto& [pid2, v2] : sp->props.entries)
                s.props.set(pid2, v2);
        }
        for (const auto& [p2, allele] : e.genome.loci) {
            const PropertyDef* pd = props.find(p2);
            if (!pd || (pd->type != ValType::Integer &&
                        pd->type != ValType::Fixed)) continue;
            std::int64_t v = allele;
            if (pd->has_range) v = std::clamp<std::int64_t>(v, pd->lo, pd->hi);
            s.props.set(p2, Value::integer(v));
        }
        s.genome = e.genome;
        s.rel_from.push_back({"parent_of", e.id});
        out.push_back(std::move(s));
    };
    // one tick of vector activity: dormancy flag + gated bite/breed work.
    // Bite roll epochs come from the PERSISTENT vector_bite_rolls counter —
    // a per-call counter would restart every tick and correlate picks.
    auto do_tick = [&](std::int64_t T) {
        const bool active = temp_ok && (!req_water || wet_at(T));
        const std::int64_t want = active ? 0 : 1;
        const std::int64_t d0 = dm_p != kNoProp ? prop_int(e, dm_p, 0) : 0;
        if (d0 != want) { change_on(e, dm_p, d0, want); }
        if (!active) return;
        if (fec > 0 && vacc_p != kNoProp && prop_of(e, vacc_p)) {
            // Density-dependent breeding: conspecifics in biting range —
            // live ones plus brood already queued this pass (pending
            // SpawnSpecs stay visible through `out` until commit, which is
            // exactly what a ticked run sees as committed entities on the
            // next tick) — pause reproduction past vector_brood_cap. The
            // count is a frozen spatial snapshot plus deterministic spawn
            // positions, so batched and ticked evaluation agree.
            const std::int64_t cap = cap_p != kNoProp
                ? std::max<std::int64_t>(1, prop_int(e, cap_p, 24)) : 24;
            std::int64_t crowd = 0;
            {
                std::vector<EntityId> cn = spatial.within(e.pos, bite_r);
                for (EntityId nid : cn) {
                    if (nid == e.id) continue;
                    const Entity* o = find(nid);
                    if (o && o->alive && o->kind_name == e.kind_name)
                        ++crowd;
                }
                const std::int64_t rr2 = bite_r * bite_r;
                for (const SpawnSpec& sp : out) {
                    if (sp.kind != e.kind_name) continue;
                    bool is_brood = false;
                    for (const auto& [t, o] : sp.rel_from)
                        if (t == "parent_of") { is_brood = true; break; }
                    if (!is_brood) continue;
                    if (dist2(e.pos, sp.pos) <= rr2) ++crowd;
                }
            }
            if (crowd < cap) {
                const std::int64_t a0 = prop_int(e, vacc_p, 0);
                const std::int64_t acc = a0 + fec;
                std::int64_t quanta = acc / 1000;
                const std::int64_t nsp = std::min<std::int64_t>(quanta, 4);
                change_on(e, vacc_p, a0, acc - nsp * 1000);
                for (std::int64_t i = 0; i < nsp; ++i) do_brood(brood_n + i);
                brood_n += static_cast<std::uint64_t>(nsp);
                if (brood_p != kNoProp)
                    change_on(e, brood_p, prop_int(e, brood_p, 0),
                              static_cast<std::int64_t>(brood_n));
            }
        }
        if (bite_rate > 0 && bacc_p != kNoProp && prop_of(e, bacc_p)) {
            const std::int64_t a0 = prop_int(e, bacc_p, 0);
            const std::int64_t acc = a0 + bite_rate;
            std::int64_t quanta = acc / 1000;
            const std::int64_t nb = std::min<std::int64_t>(quanta, 8);
            change_on(e, bacc_p, a0, acc - nb * 1000);
            for (std::int64_t i = 0; i < nb; ++i) {
                const std::int64_t br = broll_p != kNoProp
                    ? prop_int(e, broll_p, 0) : 0;
                const std::uint64_t h = Rng::stream_seed(seed,
                    "vector_bite_epoch", static_cast<std::uint64_t>(e.id) ^
                    static_cast<std::uint64_t>(br));
                do_bite(h);
                if (broll_p != kNoProp)
                    change_on(e, broll_p, br, br + 1);
            }
        }
    };

    // ---- schedule: batch when dormancy is steady, replay on a flip -------
    const std::int64_t wstart = static_cast<std::int64_t>(now) - pending;
    const bool wet_first = req_water ? wet_at(wstart + 1) : true;
    const bool wet_last = req_water ? wet_at(now) : true;
    if (pending > 1 && wet_first != wet_last) {
        // dormancy flips inside the window — replay tick-wise so bite and
        // breed accumulators only charge while the vector is active.
        for (std::int64_t k = 1; k <= pending && e.alive; ++k)
            do_tick(wstart + k);
        return;
    }
    // steady window: dormancy constant across every sub-tick — replay the
    // tick body pending times; accumulator telescoping makes this
    // bit-identical to advance(1) x N.
    for (std::int64_t k = 0; k < pending && e.alive; ++k)
        do_tick(wstart + k + 1);
}

// Phase 11.2 — flora epidemiology. A plant is a spore host when it carries
// blight_susceptibility > 0; a strain is a plant pathogen when it declares
// spore_rate > 0 (the same data flag that excludes it from zoonotic
// spillover). Shed rate scales with SAME-SPECIES neighbor density — the
// monoculture amplifier: interleaved species dilute the count and slow the
// epidemic with no scripted rule anywhere. Quanta found infections through
// the shared drift_spawns commit path; every roll uses a monotonic
// per-infection counter so batched and ticked evaluation agree.
void World::flora_drift(Entity& e, std::int64_t pending, std::uint64_t now,
                        CausalEvent& ev, std::vector<SpawnSpec>& out) {
    (void)now;
    if (pending <= 0 || !e.alive) return;
    static thread_local SeqPidCache pid_c_;
    SeqPid pid(pid_c_, props);
    const PropId susc_p = pid("blight_susceptibility"),
                 ps_p = pid("pathogen_strain"),
                 pl_p = pid("pathogen_load"),
                 ab_p = pid("antibody_level"),
                 spr_p = pid("spore_rate"),
                 vir_p = pid("virulence"),
                 sacc_p = pid("spore_accum"),
                 sroll_p = pid("spore_rolls"),
                 integ_p = pid("integrity"),
                 dacc_p = pid("damage_accum"),
                 yp_p = pid("yield_penalty"),
                 bm_p = pid("blight_marks");
    if (susc_p == kNoProp || ps_p == kNoProp || pl_p == kNoProp) return;
    if (prop_int(e, susc_p, 0) <= 0) return;    // not a susceptible host

    auto change_on = [&](Entity& tgt, PropId p, std::int64_t before,
                         std::int64_t after) {
        if (p == kNoProp || before == after) return;
        StateChange ch;
        ch.entity = tgt.id; ch.prop = p; ch.had_before = true;
        ch.before = Value::integer(before);
        ch.after = Value::integer(after);
        tgt.props.set(p, Value::integer(after));
        ev.changes.push_back(ch);
    };

    // ---- collect this plant's active spore infections ---------------------
    struct Infect {
        Entity* ie = nullptr;
        Entity* strain = nullptr;
        std::int64_t load = 0, vir = 0, rate = 0, strain_id = 0;
    };
    std::vector<Infect> infections;
    std::int64_t total_load = 0;
    for (const Relation& rel : relations_of(e.id, "carries")) {
        if (rel.a != e.id) continue;
        Entity* ie = find(rel.b);
        if (!ie || !ie->alive) continue;
        const std::int64_t seid = prop_int(*ie, ps_p, 0);
        Entity* st = seid > 0 ? find(static_cast<EntityId>(seid)) : nullptr;
        if (!st || !st->alive) continue;
        const std::int64_t rate = spr_p != kNoProp
            ? prop_int(*st, spr_p, 0) : 0;
        if (rate <= 0) continue;               // not a plant pathogen
        Infect I;
        I.ie = ie; I.strain = st; I.strain_id = seid;
        I.load = prop_int(*ie, pl_p, 0);
        I.vir = vir_p != kNoProp ? prop_int(*st, vir_p, 0) : 0;
        I.rate = rate;
        total_load += I.load;
        infections.push_back(I);
    }

    // ---- spore shed: density-scaled, Euclid-exact --------------------------
    // same-species neighbors within the spore range count once per call;
    // the count is a World-Truth snapshot, identical under batched and
    // per-tick evaluation as long as the entity set is static — coupled
    // plants already replay tick-wise via physiology_coupled().
    if (!infections.empty()) {
        std::int64_t density = 0;
        {
            std::vector<EntityId> near = spatial.within(e.pos, 96);
            std::sort(near.begin(), near.end());
            for (EntityId nid : near) {
                if (nid == e.id || density >= 12) continue;
                const Entity* o = find(nid);
                if (!o || !o->alive || o->kind_name != e.kind_name) continue;
                if (prop_int(*o, susc_p, 0) <= 0) continue;
                ++density;
            }
        }
        for (Infect& I : infections) {
            // virulence erodes the plant's own integrity every call —
            // blight is a wasting disease, independent of shed success.
            if (dacc_p != kNoProp && integ_p != kNoProp &&
                prop_of(e, integ_p)) {
                const std::int64_t da0 = prop_int(*I.ie, dacc_p, 0);
                const std::int64_t dacc = da0 + pending * I.load * I.vir / 200;
                const std::int64_t dq = dacc / 1000;
                change_on(*I.ie, dacc_p, da0, dacc - dq * 1000);
                if (dq > 0) {
                    const std::int64_t iv = prop_int(e, integ_p, 0);
                    change_on(e, integ_p, iv,
                              std::max<std::int64_t>(0, iv - dq * 3));
                }
            }
            // shed quanta: spore_rate permille/tick scaled by same-species
            // density/4 — zero neighbors means zero shed (isolation is the
            // polyculture benefit, emergent from the count alone).
            const std::int64_t shed =
                I.rate * std::min<std::int64_t>(density, 12) / 4;
            if (shed <= 0 || sacc_p == kNoProp) continue;
            const std::int64_t a0 = prop_int(*I.ie, sacc_p, 0);
            const std::int64_t acc = a0 + pending * shed;
            std::int64_t quanta = acc / 1000;
            change_on(*I.ie, sacc_p, a0, acc - quanta * 1000);
            if (quanta <= 0) continue;
            if (quanta > 32) quanta = 32;
            // each quantum picks one deterministic same-species target.
            // Epoch = monotonic per-infection roll counter (never the
            // accumulator remainder — Phase 10.4 contract).
            std::vector<EntityId> cands;
            {
                const std::int64_t r2 = 96 * 96;
                std::vector<EntityId> near = spatial.within(e.pos, 96);
                std::sort(near.begin(), near.end());
                for (EntityId nid : near) {
                    if (nid == e.id) continue;
                    const Entity* o = find(nid);
                    if (!o || !o->alive || o->kind_name != e.kind_name) continue;
                    if (prop_int(*o, susc_p, 0) <= 0) continue;
                    if (dist2(o->pos, e.pos) > r2) continue;
                    bool has = false;
                    for (const Relation& rr : relations_of(nid, "carries")) {
                        if (rr.a != nid) continue;
                        const Entity* x = find(rr.b);
                        if (x && x->alive &&
                            prop_int(*x, ps_p, 0) == I.strain_id) {
                            has = true; break;
                        }
                    }
                    if (!has) cands.push_back(nid);
                    if (cands.size() >= 16) break;
                }
            }
            if (cands.empty()) continue;
            const std::int64_t rc0 = sroll_p != kNoProp
                ? prop_int(*I.ie, sroll_p, 0) : 0;
            for (std::int64_t qi = 0; qi < quanta; ++qi) {
                const std::uint64_t h = Rng::stream_seed(world_seed,
                    "spore_spread",
                    static_cast<std::uint64_t>(I.ie->id) ^
                    static_cast<std::uint64_t>(I.strain_id) ^
                    static_cast<std::uint64_t>(rc0 + qi));
                const EntityId tgt = cands[
                    h % static_cast<std::uint64_t>(cands.size())];
                // dedupe against infections already queued this pass
                bool queued = false;
                for (const SpawnSpec& sp : out) {
                    bool is_carries = false;
                    for (const auto& [t, o] : sp.rel_from)
                        if (t == "carries" && o == tgt) is_carries = true;
                    if (!is_carries) continue;
                    const Value* v = sp.props.get(ps_p);
                    if (v && v->type == ValType::Integer &&
                        v->i == I.strain_id) { queued = true; break; }
                }
                if (queued) continue;
                SpawnSpec sp;
                sp.kind = "infection"; sp.material = kNoMaterial;
                sp.pos = e.pos;
                PropertyBag pb;
                pb.set(ps_p, Value::integer(I.strain_id));
                pb.set(pl_p, Value::integer(120));
                if (ab_p != kNoProp) pb.set(ab_p, Value::integer(0));
                const PropId ie_p = pid("infection_elapsed");
                if (ie_p != kNoProp) pb.set(ie_p, Value::integer(0));
                const PropId ila_p = pid("infection_load_accum");
                if (ila_p != kNoProp) pb.set(ila_p, Value::integer(0));
                const PropId iba_p = pid("infection_ab_accum");
                if (iba_p != kNoProp) pb.set(iba_p, Value::integer(0));
                const PropId sa_p = pid("shed_accum");
                if (sa_p != kNoProp) pb.set(sa_p, Value::integer(0));
                sp.props = pb;
                sp.rel_from.push_back({"carries", tgt});
                out.push_back(std::move(sp));
                ev.evidence += "spore jump; ";
            }
            if (sroll_p != kNoProp)
                change_on(*I.ie, sroll_p, rc0, rc0 + quanta);
        }
    }

    // ---- yield penalty + visible marks -------------------------------------
    // Blight load slows fruiting and shows as observable wilt — the
    // perception/engram channel learns the signs without any hidden state.
    {
        const std::int64_t yp = std::min<std::int64_t>(900, total_load / 2);
        change_on(e, yp_p,
                  yp_p != kNoProp ? prop_int(e, yp_p, 0) : 0, yp);
        const std::int64_t bm = std::min<std::int64_t>(1000, total_load);
        change_on(e, bm_p,
                  bm_p != kNoProp ? prop_int(e, bm_p, 0) : 0, bm);
    }
}

// Phase 11 — lazy symbiont seeding. A host carrying `symbiont_signature`
// (and `symbiont_*` strain templates) that does not already carry a
// symbiont=1 strain gets its commensal PathogenStrain + ActiveInfection
// created here — real ids, inside the metabolize causal event.
void World::seed_symbionts(CausalEvent& ev,
                           const std::vector<EntityId>& hosts) {
    static thread_local SeqPidCache pid_c_;
    SeqPid pid(pid_c_, props);
    const PropId sig_p = pid("symbiont_signature"),
                 ps_p = pid("pathogen_strain"),
                 pl_p = pid("pathogen_load"),
                 ab_p = pid("antibody_level"),
                 sym_p = pid("symbiont");
    if (sig_p == kNoProp || ps_p == kNoProp) return;
    for (EntityId hid : hosts) {
        Entity* h = find(hid);
        if (!h || !h->alive) continue;
        if (prop_int(*h, sig_p, 0) <= 0) continue;
        bool has_symbiont = false;
        for (const Relation& rel : relations_of(hid, "carries")) {
            if (rel.a != hid) continue;
            const Entity* ie = find(rel.b);
            if (!ie || !ie->alive) continue;
            const std::int64_t seid = prop_int(*ie, ps_p, 0);
            const Entity* st = seid > 0
                ? find(static_cast<EntityId>(seid)) : nullptr;
            if (st && st->alive && sym_p != kNoProp &&
                prop_int(*st, sym_p, 0) != 0) { has_symbiont = true; break; }
        }
        if (has_symbiont) continue;
        // strain entity: template props copied host -> strain 1:1
        PropertyBag sp;
        struct Map { const char* from; const char* to; };
        static const Map kMap[] = {
            {"symbiont_signature", "antigen_signature"},
            {"symbiont_virulence", "virulence"},
            {"symbiont_airborne_rate", "airborne_rate"},
            {"symbiont_waterborne_rate", "waterborne_rate"},
            {"symbiont_contact_rate", "contact_rate"},
            {"symbiont_incubation_ticks", "incubation_ticks"},
        };
        for (const Map& m : kMap) {
            const PropertyDef* fp = props.find_name(m.from);
            const PropertyDef* tp = props.find_name(m.to);
            if (!fp || !tp) continue;
            const Value* v = prop_of(*h, fp->id);
            if (v) sp.set(tp->id, *v);
        }
        const PropertyDef* sslot_p = props.find_name("symbiont_slot");
        const PropertyDef* tslot_p = props.find_name("target_slot");
        if (sslot_p && tslot_p) {
            if (const Value* v = prop_of(*h, sslot_p->id))
                sp.set(tslot_p->id, *v);
        }
        if (sym_p != kNoProp) sp.set(sym_p, Value::integer(1));
        const EntityId strain_id = create_entity(
            "pathogen_strain", kNoMaterial, h->pos, sp);
        if (strain_id == kNoEntity) continue;
        PropertyBag ip;
        ip.set(ps_p, Value::integer(static_cast<std::int64_t>(strain_id)));
        if (pl_p != kNoProp) ip.set(pl_p, Value::integer(200));
        if (ab_p != kNoProp) ip.set(ab_p, Value::integer(0));
        const EntityId inf = create_entity(
            "infection", kNoMaterial, h->pos, ip);
        if (inf == kNoEntity) continue;
        relations.relate(hid, "carries", inf);
        ev.spawned.push_back(strain_id);
        ev.spawned.push_back(inf);
        ev.evidence += "symbiont seeded; ";
    }
}

// Phase 11 — zoonotic spillover. Runs post-commit inside interact(): a
// successful consume of raw (cooked_state < 500) strain-carrying tissue or
// water infects a susceptible actor (reservoir_tolerance < 700). Below 500
// the deterministic zoonotic_jump_bias roll may found an adapted strain —
// new antigen_signature, amplified virulence — via a monotonic per-host
// spillover_rolls counter (LOD-invariant epoch, same contract as drift).
void World::zoonotic_spillover(const InteractionContext& ctx,
                               CausalEvent& ev,
                               std::vector<EntityId>& spawned) {
    if (ctx.parameters.action != "consume") return;
    Entity* actor = find(ctx.actor);
    if (!actor || !actor->alive) return;
    static thread_local SeqPidCache pid_c_;
    SeqPid pid(pid_c_, props);
    const PropId rt_p = pid("reservoir_tolerance"),
                 cooked_p = pid("cooked_state"),
                 bias_p = pid("zoonotic_jump_bias"),
                 sr_p = pid("strain_ref"),
                 ps_p = pid("pathogen_strain"),
                 rolls_p = pid("spillover_rolls"),
                 pl_p = pid("pathogen_load"),
                 ab_p = pid("antibody_level"),
                 ag_p = pid("antigen_signature"),
                 vir_p = pid("virulence"),
                 sym_p = pid("symbiont"),
                 spr_p = pid("spore_rate");
    if (rt_p == kNoProp || ps_p == kNoProp) return;
    const std::int64_t rt = prop_int(*actor, rt_p, 0);
    if (rt >= 700) return;                     // tolerant reservoir: no jump
    // gather unique strain ids reachable from the consumed targets
    std::vector<std::int64_t> strain_ids;
    for (EntityId tid : ctx.targets) {
        if (tid == ctx.actor) continue;
        const Entity* t = find(tid);
        if (!t) continue;
        if (cooked_p != kNoProp && prop_int(*t, cooked_p, 0) >= 500) continue;
        if (sr_p != kNoProp) {
            const std::int64_t sr = prop_int(*t, sr_p, 0);
            if (sr > 0) strain_ids.push_back(sr);
        }
        for (const Relation& rel : relations_of(tid, "carries")) {
            if (rel.a != tid) continue;
            const Entity* ie = find(rel.b);
            if (!ie || !ie->alive) continue;
            const std::int64_t seid = prop_int(*ie, ps_p, 0);
            if (seid > 0) strain_ids.push_back(seid);
        }
    }
    std::sort(strain_ids.begin(), strain_ids.end());
    strain_ids.erase(std::unique(strain_ids.begin(), strain_ids.end()),
                     strain_ids.end());
    if (strain_ids.size() > 4) strain_ids.resize(4);
    for (std::int64_t seid : strain_ids) {
        const Entity* strain = find(static_cast<EntityId>(seid));
        if (!strain || !strain->alive) continue;
        // plant pathogens (spore_rate > 0) poison via the gut, not via
        // species jump — they belong to the flora epidemiology channel.
        if (spr_p != kNoProp && prop_int(*strain, spr_p, 0) > 0) continue;
        // already carrying this strain? no duplicate infections
        bool already = false;
        for (const Relation& rel : relations_of(ctx.actor, "carries")) {
            if (rel.a != ctx.actor) continue;
            const Entity* ie = find(rel.b);
            if (ie && ie->alive &&
                prop_int(*ie, ps_p, 0) == seid) { already = true; break; }
        }
        if (already) continue;
        std::int64_t use_strain = seid;
        bool adapted = false;
        EntityId variant_id = kNoEntity;
        if (rt < 500 && rolls_p != kNoProp) {
            const std::int64_t bias = bias_p != kNoProp
                ? prop_int(*actor, bias_p, 300) : 300;
            const std::int64_t rc = prop_int(*actor, rolls_p, 0);
            const std::uint64_t h = Rng::stream_seed(world_seed,
                "zoonotic_jump",
                static_cast<std::uint64_t>(ctx.actor) ^
                static_cast<std::uint64_t>(seid) ^
                static_cast<std::uint64_t>(rc));
            {
                StateChange ch;
                ch.entity = ctx.actor; ch.prop = rolls_p; ch.had_before = true;
                ch.before = Value::integer(rc);
                ch.after = Value::integer(rc + 1);
                actor->props.set(rolls_p, Value::integer(rc + 1));
                ev.changes.push_back(ch);
            }
            if (h % 1000 < static_cast<std::uint64_t>(
                    std::max<std::int64_t>(1, bias))) {
                // host-adapted variant: copy the strain, re-sign the
                // antigen and amplify virulence for a naive host.
                PropertyBag sp;
                for (const auto& [pp, vv] : strain->props.entries)
                    sp.set(pp, vv);
                if (ag_p != kNoProp)
                    sp.set(ag_p, Value::integer(static_cast<std::int64_t>(
                        (static_cast<std::uint32_t>(h >> 16) |
                         static_cast<std::uint32_t>(
                             prop_int(*strain, ag_p, 0))) ^ 0x9E3779B9u)));
                if (vir_p != kNoProp) {
                    const std::int64_t v0 = prop_int(*strain, vir_p, 0);
                    sp.set(vir_p, Value::integer(std::min<std::int64_t>(1000,
                        v0 + v0 / 3 + bias / 20)));
                }
                if (sym_p != kNoProp) sp.set(sym_p, Value::integer(0));
                variant_id = create_entity("pathogen_strain", kNoMaterial,
                                           actor->pos, sp);
                if (variant_id != kNoEntity) {
                    use_strain = static_cast<std::int64_t>(variant_id);
                    adapted = true;
                    spawned.push_back(variant_id);
                }
            }
        }
        PropertyBag ip;
        ip.set(ps_p, Value::integer(use_strain));
        if (pl_p != kNoProp) ip.set(pl_p, Value::integer(120));
        if (ab_p != kNoProp) ip.set(ab_p, Value::integer(0));
        const EntityId inf = create_entity("infection", kNoMaterial,
                                           actor->pos, ip);
        if (inf == kNoEntity) continue;
        relations.relate(ctx.actor, "carries", inf);
        spawned.push_back(inf);
        ev.evidence += adapted ? "zoonotic spillover adapted; "
                               : "zoonotic spillover; ";
    }
}

void World::dissipate_heat(std::int64_t dt) {
    const PropertyDef* tp = props.find_name("temperature");
    if (!tp) return;
    const PropId temp = tp->id;
    const PropId cond = SIMLAB_PID(props, "conductivity");
    constexpr std::int64_t kAmbient = 200;               // 20°C when no terrain field
    CausalEvent ev;
    ev.tick = clock_.tick();
    ev.action = "cool";
    ev.applied_laws.push_back(4);                        // physics.heat
    for (const auto& ep : entities) {
        Entity& e = *ep;
        if (!e.alive) continue;
        const Value* tv = e.props.get(temp);             // only entities carrying a thermal state
        if (!tv || tv->type != ValType::Integer) continue;
        const std::int64_t amb = (!terrain.empty() && terrain.contains(e.pos))
            ? terrain.temp_at(e.pos) : kAmbient;
        std::int64_t cur = tv->i;
        if (cur == amb) continue;
        // loss rate (permille of the gap per tick) follows conductivity
        const std::int64_t k = std::clamp<std::int64_t>(
            cond != kNoProp ? prop_int(e, cond, 100) / 10 : 10, 1, 200);
        if (dt > 2000) cur = amb;
        else for (std::int64_t i = 0; i < dt && cur != amb; ++i) {
            const std::int64_t gap = amb - cur;
            std::int64_t step = (gap < 0 ? -gap : gap) * k / 1000;
            if (step < 1) step = 1;
            cur += gap < 0 ? -step : step;
        }
        if (cur == tv->i) continue;
        StateChange ch;
        ch.entity = e.id; ch.prop = temp; ch.had_before = true;
        ch.before = *tv; ch.after = Value::integer(cur);
        e.props.set(temp, ch.after);
        ev.changes.push_back(ch);
        ev.participants.push_back(e.id);
    }
    if (!ev.changes.empty()) {
        ev.evidence = "heat dissipates toward ambient";
        ledger.append(std::move(ev));
    }
}


void World::ecology_tick(std::int64_t n) {
    if (n <= 0) return;
    static thread_local SeqPidCache pid_c_;
    SeqPid pid(pid_c_, props);
    const PropId p_drops = pid("death_drops");
    const PropId p_drops_done = pid("death_drops_done");
    const PropId p_sprout_target = pid("sprout_target");
    const PropId p_sprout_material = pid("sprout_material");
    const PropId p_sprout_since = pid("sprout_since");
    const PropId p_sprout_rate = pid("sprout_rate");
    const PropId p_fruit_count = pid("fruit_count");
    const bool do_drops = p_drops != kNoProp && p_drops_done != kNoProp;
    const bool do_sprout = p_sprout_target != kNoProp && p_sprout_since != kNoProp &&
                          p_sprout_rate != kNoProp;
    // Trophic range drift must also keep this pass alive: a world with no
    // seed bank and no influx zones still owes its migrants their fires.
    // The entity scan only runs in that rare all-empty case.
    bool range_maybe = false;
    bool husb_maybe = false;
    if (!do_drops && !do_sprout && influx.defs().empty()) {
        const PropId p_mig0 = pid("migration_drive");
        const PropId p_terr0 = pid("territorial_radius");
        const PropId p_hy0 = pid("milk_yield");
        const PropId p_wy0 = pid("wool_yield");
        const PropId p_sa0 = pid("scent_alert_radius");
        if (p_mig0 != kNoProp || p_terr0 != kNoProp || p_hy0 != kNoProp ||
            p_wy0 != kNoProp || p_sa0 != kNoProp) {
            for (const auto& ep : entities) {
                const Entity& e = *ep;
                if (!e.alive) continue;
                if (!terrain.empty() &&
                    ((p_mig0 != kNoProp && prop_int(e, p_mig0, 0) > 0) ||
                     (p_terr0 != kNoProp && prop_int(e, p_terr0, 0) > 0)))
                    range_maybe = true;
                if ((p_hy0 != kNoProp && prop_int(e, p_hy0, 0) > 0) ||
                    (p_wy0 != kNoProp && prop_int(e, p_wy0, 0) > 0) ||
                    (p_sa0 != kNoProp && prop_int(e, p_sa0, 0) > 0))
                    husb_maybe = true;
                if (range_maybe && husb_maybe) break;
            }
        }
    }
    if (!do_drops && !do_sprout && influx.defs().empty() && !range_maybe &&
        !husb_maybe) return;

    const std::uint64_t now = clock_.tick();
    const std::uint64_t from = now - static_cast<std::uint64_t>(n) + 1;
    CausalEvent ev;
    ev.tick = now;
    ev.action = "ecology";
    ev.applied_laws.push_back(20);              // ecology: seed bank + edge influx
    std::vector<SpawnSpec> spawns;
    std::vector<std::pair<std::size_t, const InfluxMigrant*>> spawn_minds;

    // ---- seed bank: death drops ----------------------------------------------
    // A dead entity carrying `death_drops` ("material=count,...") leaves its
    // manifest exactly once, marked by `death_drops_done`. Drops whose
    // MATERIAL defines `sprout_rate` are propagules: they remember the corpse's
    // kind (`sprout_target`) and material (`sprout_material`) and carry
    // `sprout_since` = this tick — germination credit is purely elapsed time.
    if (do_drops) {
        for (const auto& ep : entities) {
            Entity& e = *ep;
            if (e.alive) continue;
            const Value* v = prop_of(e, p_drops);
            if (!v || v->type != ValType::Text || v->s.empty()) continue;
            if (prop_int(e, p_drops_done, 0) != 0) continue;
            std::stringstream ms(v->s);
            std::string tok;
            std::size_t made = 0;
            while (std::getline(ms, tok, ',')) {
                const std::size_t eq = tok.find('=');
                if (eq == std::string::npos) continue;
                const std::string mname = tok.substr(0, eq);
                std::int64_t cnt = 0;
                try { cnt = parse_i64(tok.substr(eq + 1)); } catch (...) { continue; }
                const MaterialDef* md = cnt > 0 ? materials.find_name(mname) : nullptr;
                if (!md) continue;
                const bool propagule = md->props.count(p_sprout_rate) != 0;
                for (std::int64_t i = 0; i < cnt; ++i) {
                    SpawnSpec s;
                    s.kind = mname;
                    s.material = md->id;
                    const std::uint64_t h =
                        Rng::hash(e.id ^ (static_cast<std::uint64_t>(i) * 0x9E3779B97F4A7C15ull + 1));
                    s.pos = {e.pos.x + static_cast<std::int32_t>((h >> 8) % 21) - 10,
                             e.pos.y + static_cast<std::int32_t>((h >> 40) % 21) - 10};
                    if (propagule && do_sprout) {
                        s.props.set(p_sprout_target, Value::text(e.kind_name));
                        if (e.material != kNoMaterial)
                            if (const MaterialDef* cm = materials.find(e.material))
                                s.props.set(p_sprout_material, Value::text(cm->name));
                        s.props.set(p_sprout_since, Value::integer(static_cast<std::int64_t>(now)));
                    }
                    spawns.push_back(std::move(s));
                    ++made;
                }
            }
            if (made > 0) {
                StateChange ch;
                ch.entity = e.id; ch.prop = p_drops_done;
                ch.had_before = prop_of(e, p_drops_done) != nullptr;
                ch.before = Value::integer(0);
                ch.after = Value::integer(1);
                e.props.set(p_drops_done, ch.after);
                ev.changes.push_back(ch);
                ev.participants.push_back(e.id);
                ev.evidence += "death drops x" + std::to_string(made) + " from " +
                               std::to_string(e.id) + " (" + e.kind_name + "); ";
            }
        }
    }

    // ---- seed bank: germination -----------------------------------------------
    // Terrain fields are static, so the habitat check is time-invariant:
    // sprout iff sprout_rate(permille/tick) x elapsed >= 1000 AND the local
    // moisture/temperature band fits. Exact under dormant-LOD batching — no
    // per-tick credit bookkeeping needed.
    if (do_sprout) {
        const PropId p_sprout_moist = pid("sprout_moisture_min");
        const PropId p_sprout_tmin = pid("sprout_temp_min");
        const PropId p_sprout_tmax = pid("sprout_temp_max");
        for (auto& ep : entities) {
            Entity& e = *ep;
            if (!e.alive) continue;
            const Value* tv = prop_of(e, p_sprout_target);
            if (!tv || tv->type != ValType::Text || tv->s.empty()) continue;
            const std::int64_t rate = prop_int(e, p_sprout_rate, 0);
            const std::int64_t since =
                prop_int(e, p_sprout_since, static_cast<std::int64_t>(e.last_metabolized));
            const std::int64_t elapsed =
                static_cast<std::int64_t>(now) - since;
            if (rate <= 0 || elapsed <= 0 || rate * elapsed < 1000) continue;
            const std::int64_t moist_min = prop_int(e, p_sprout_moist, 300);
            const std::int64_t tmin = prop_int(e, p_sprout_tmin, -1000);
            const std::int64_t tmax = prop_int(e, p_sprout_tmax, 2000);
            std::int64_t moist = 500, temp = 300;    // mild default, no terrain
            if (!terrain.empty() && terrain.contains(e.pos)) {
                moist = std::clamp<std::int64_t>(terrain.moist_at(e.pos), 0, 1000);
                temp = terrain.temp_at(e.pos);
            }
            if (moist < moist_min || temp < tmin || temp > tmax) continue;
            const std::string& target = tv->s;
            SpawnSpec s;
            if (const SpeciesDef* sp = species.find_name(target)) {
                s.kind = sp->name;
                s.material = sp->material;
                s.props = sp->props;
                if (p_fruit_count != kNoProp && s.props.get(p_fruit_count))
                    s.props.set(p_fruit_count, Value::integer(0));   // bare sprout
            } else {
                s.kind = target;
                const Value* mv = p_sprout_material != kNoProp
                    ? prop_of(e, p_sprout_material) : nullptr;
                if (mv && mv->type == ValType::Text) s.material = materials.id_of(mv->s);
            }
            s.pos = e.pos;
            spawns.push_back(std::move(s));
            ev.participants.push_back(e.id);
            ev.evidence += "seed " + std::to_string(e.id) + " sprouted '" + target + "'; ";
            // the propagule is consumed by germination — same reap rule as
            // integrity 0, ledgered inside this event
            e.alive = false;
            spatial.erase(e.id);
        }
    }

    // ---- trophic ranges: seasonal migration & territorial drift -------------
    // Species carrying migration_drive / territorial_radius (pure data
    // props — no species names in code) sense the seasonal habitat
    // gradient: their own declared condition bands evaluated against
    // terrain fields plus the seasonal temperature offset. A fire moves
    // the animal one step toward the best nearby cell, home when beyond
    // its territory, or toward its herd when cohesion pulls. Fires run
    // on a stateless per-entity hash schedule over [from,now] — identical
    // to the influx contract, so dormant-LOD batching stays exact.
    {
        const PropId p_mig = pid("migration_drive");
        const PropId p_terr = pid("territorial_radius");
        const PropId p_cohes = pid("herd_cohesion");
        const PropId p_ax = pid("anchor_x");
        const PropId p_ay = pid("anchor_y");
        const PropId p_phase = pid("activity_phase");
        const bool do_range = (p_mig != kNoProp || p_terr != kNoProp) &&
                              !terrain.empty();
        if (do_range) {
            struct Drifter { Entity* e; const SpeciesDef* sp; std::int64_t drive; };
            std::vector<Drifter> drifters;
            std::map<std::string, const SpeciesDef*> spcache;
            for (auto& ep : entities) {
                Entity& e = *ep;
                if (!e.alive) continue;
                const std::int64_t mig =
                    p_mig != kNoProp ? prop_int(e, p_mig, 0) : 0;
                const std::int64_t terr =
                    p_terr != kNoProp ? prop_int(e, p_terr, 0) : 0;
                const std::string kn = e.kind_name.str();
                auto ci = spcache.find(kn);
                if (ci == spcache.end())
                    ci = spcache.emplace(kn, species.find_name(kn)).first;
                if (!ci->second) continue;
                if (mig <= 0 && terr <= 0) {
                    // Phase 18: a tameable animal with no wild range still
                    // follows its bonded partner — the bond check is gated
                    // by tameability so plain wildlife never pays for it.
                    const PropId p_tame0 = pid("tameability");
                    if (p_tame0 == kNoProp ||
                        prop_int(e, p_tame0, 0) <= 0) continue;
                    bool has_bond = false;
                    for (const Relation& rel : relations_of(e.id, "bonded")) {
                        const EntityId o = rel.a == e.id ? rel.b : rel.a;
                        const Entity* oe = find(o);
                        if (oe && oe->alive) { has_bond = true; break; }
                    }
                    if (!has_bond) continue;
                    drifters.push_back({&e, ci->second, 40});
                    continue;
                }
                // territorial animals drift home slowly even with no
                // migration drive; migrants use their declared rate.
                drifters.push_back({&e, ci->second,
                    std::max<std::int64_t>(mig, terr > 0 ? 25 : 0)});
            }
            // per-condition fit: 1000 inside the band, decaying with the
            // overshoot (units differ per field — the multiplier is data
            // tuning, the band itself comes from the species file).
            auto field_fit = [](const std::string& f, std::int64_t v,
                                std::int64_t lo, std::int64_t hi) -> std::int64_t {
                if (v >= lo && v <= hi) return 1000;
                const std::int64_t over = v < lo ? lo - v : v - hi;
                std::int64_t k = 10;
                if (f == "temperature") k = 4;
                else if (f == "moisture" || f == "vegetation") k = 5;
                else if (f == "elevation" || f == "water_table") k = 2;
                else if (f == "near_water") return 0;
                return std::max<std::int64_t>(0, 1000 - over * k);
            };
            auto suitability = [&](const Drifter& d, Vec2 p,
                                   std::int64_t sdt) -> std::int64_t {
                if (!terrain.contains(p) || terrain.is_water(p)) return -1;
                std::int64_t sum = 0;
                int n = 0;
                for (const FormationCondition& c : d.sp->conditions) {
                    std::int64_t v = 0;
                    if (c.field == "temperature")
                        v = terrain.temp_at(p) + sdt;
                    else if (c.field == "moisture") v = terrain.moist_at(p);
                    else if (c.field == "vegetation") v = terrain.veg_at(p);
                    else if (c.field == "slope") v = terrain.slope_at(p);
                    else if (c.field == "elevation") v = terrain.elev_at(p);
                    else if (c.field == "water_table")
                        v = terrain.water_table_at(p);
                    else if (c.field == "near_water") {
                        v = 0;
                        for (int dy = -1; dy <= 1 && !v; ++dy)
                            for (int dx = -1; dx <= 1; ++dx) {
                                if (!dx && !dy) continue;
                                const Vec2 q{p.x + dx, p.y + dy};
                                if (terrain.contains(q) && terrain.is_water(q)) {
                                    v = 1; break;
                                }
                            }
                    } else continue;    // fault/lithology/placer: not cues
                    sum += field_fit(c.field, v, c.lo, c.hi);
                    ++n;
                }
                return n ? sum / n : 500;   // no evaluable band: neutral
            };
            // two rings: a fine step for local polish and a coarse step
            // that can actually cross a habitat gradient in one bound
            // (terrain fields are per-cell, so skipping cells is safe —
            // suitability is evaluated at the destination, not the path).
            static const std::int32_t kDir[16][2] = {
                {24,0},{-24,0},{0,24},{0,-24},
                {17,17},{17,-17},{-17,17},{-17,-17},
                {96,0},{-96,0},{0,96},{0,-96},
                {68,68},{68,-68},{-68,68},{-68,-68}};
            for (std::uint64_t t = from; t <= now && !drifters.empty(); ++t) {
                const std::int64_t sdt = eco_season_temp(eco_season(t));
                const std::int64_t hour = static_cast<std::int64_t>(t % 24);
                for (Drifter& d : drifters) {
                    Entity& e = *d.e;
                    if (!e.alive) continue;
                    const std::int64_t phase =
                        p_phase != kNoProp ? prop_int(e, p_phase, 0) : 0;
                    if (!eco_phase_active(phase, hour)) continue;
                    const std::uint64_t h = Rng::stream_seed(seed,
                        "range_drift",
                        static_cast<std::uint64_t>(e.id) * 0x9E3779B97F4A7C15ull
                            + t);
                    if (h % 1000 >= static_cast<std::uint64_t>(d.drive)) continue;
                    const std::int64_t terr =
                        p_terr != kNoProp ? prop_int(e, p_terr, 0) : 0;
                    // home anchor: first drift pins territorial_radius
                    // around wherever the animal currently stands.
                    if (terr > 0 && p_ax != kNoProp && p_ay != kNoProp &&
                        (!prop_of(e, p_ax) || !prop_of(e, p_ay))) {
                        for (int k = 0; k < 2; ++k) {
                            const PropId ap = k ? p_ay : p_ax;
                            if (prop_of(e, ap)) continue;
                            const std::int64_t av =
                                k ? static_cast<std::int64_t>(e.pos.y)
                                  : static_cast<std::int64_t>(e.pos.x);
                            StateChange ch;
                            ch.entity = e.id; ch.prop = ap;
                            ch.had_before = false;
                            ch.before = Value::integer(0);
                            ch.after = Value::integer(av);
                            e.props.set(ap, ch.after);
                            ev.changes.push_back(ch);
                        }
                    }
                    Vec2 np = e.pos;
                    bool moved = false;
                    // bonded partner pull (Phase 18): a tamed animal follows
                    // its bonded partner instead of ranging — husbandry
                    // overrides wild territorial drift until the bond ends.
                    EntityId bond_partner = kNoEntity;
                    for (const Relation& rel : relations_of(e.id, "bonded")) {
                        const EntityId o = rel.a == e.id ? rel.b : rel.a;
                        const Entity* oe = find(o);
                        if (oe && oe->alive) { bond_partner = o; break; }
                    }
                    const std::int64_t ax = p_ax != kNoProp
                        ? prop_int(e, p_ax, e.pos.x) : e.pos.x;
                    const std::int64_t ay = p_ay != kNoProp
                        ? prop_int(e, p_ay, e.pos.y) : e.pos.y;
                    const std::int64_t adx = ax - e.pos.x;
                    const std::int64_t ady = ay - e.pos.y;
                    if (bond_partner != kNoEntity) {
                        const Entity* be = find(bond_partner);
                        const std::int64_t bdx = be->pos.x - e.pos.x;
                        const std::int64_t bdy = be->pos.y - e.pos.y;
                        if (bdx * bdx + bdy * bdy > 24 * 24) {
                            Vec2 c{static_cast<std::int32_t>(
                                       e.pos.x + std::clamp<std::int64_t>(bdx, -24, 24)),
                                   static_cast<std::int32_t>(
                                       e.pos.y + std::clamp<std::int64_t>(bdy, -24, 24))};
                            if (terrain.contains(c) && !terrain.is_water(c)) {
                                np = c; moved = true;
                            }
                        }
                    } else if (terr > 0 && adx * adx + ady * ady > terr * terr) {
                        // outside home range: walk back toward the anchor
                        const std::int64_t step = 24;
                        Vec2 c{static_cast<std::int32_t>(
                                   e.pos.x + std::clamp<std::int64_t>(adx, -step, step)),
                               static_cast<std::int32_t>(
                                   e.pos.y + std::clamp<std::int64_t>(ady, -step, step))};
                        if (terrain.contains(c) && !terrain.is_water(c)) {
                            np = c; moved = true;
                        }
                    } else {
                        const std::int64_t cur_s = suitability(d, e.pos, sdt);
                        Vec2 best = e.pos;
                        std::int64_t best_s = cur_s;
                        auto consider = [&](Vec2 c) {
                            const std::int64_t s = suitability(d, c, sdt);
                            if (s >= 0 && s > best_s) { best_s = s; best = c; }
                        };
                        for (const auto& dd : kDir)
                            consider({e.pos.x + dd[0], e.pos.y + dd[1]});
                        // herd cohesion: pull toward the same-kind centroid
                        const std::int64_t cohes =
                            p_cohes != kNoProp ? prop_int(e, p_cohes, 0) : 0;
                        if (cohes > 0) {
                            std::int64_t sx = 0, sy = 0, cnt = 0;
                            for (EntityId oid : spatial.within(e.pos, 320)) {
                                if (oid == e.id) continue;
                                const Entity* o = find(oid);
                                if (!o || !o->alive ||
                                    o->kind_name != e.kind_name) continue;
                                sx += o->pos.x; sy += o->pos.y; ++cnt;
                            }
                            if (cnt > 0) {
                                const std::int64_t cx = sx / cnt;
                                const std::int64_t cy = sy / cnt;
                                Vec2 c{static_cast<std::int32_t>(
                                           e.pos.x + std::clamp<std::int64_t>(
                                               (cx - e.pos.x) * cohes / 1000,
                                               -24, 24)),
                                       static_cast<std::int32_t>(
                                           e.pos.y + std::clamp<std::int64_t>(
                                               (cy - e.pos.y) * cohes / 1000,
                                               -24, 24))};
                                consider(c);
                            }
                        }
                        // hysteresis: drift only on a clearly better cell
                        if (best_s > cur_s + 30 && best != e.pos) {
                            np = best; moved = true;
                        }
                    }
                    if (moved && np != e.pos) {
                        const Vec2 from_p = e.pos;
                        e.pos = np;
                        spatial.move(e.id, np);
                        // footfall compaction, same rule as locomotion
                        if (!terrain.traffic_wear.empty() && terrain.cell > 0) {
                            const std::int64_t dx =
                                static_cast<std::int64_t>(np.x) - from_p.x;
                            const std::int64_t dy =
                                static_cast<std::int64_t>(np.y) - from_p.y;
                            const std::int64_t d2 = dx * dx + dy * dy;
                            if (d2 > 0) {
                                std::int64_t dist = 1;
                                while (dist * dist < d2) ++dist;
                                const std::int64_t step = terrain.cell / 2;
                                std::int64_t samples = dist / step;
                                if (samples < 1) samples = 1;
                                for (std::int64_t i = 0; i <= samples; ++i) {
                                    const Vec2 cp{
                                        from_p.x + static_cast<std::int32_t>(
                                            dx * i / samples),
                                        from_p.y + static_cast<std::int32_t>(
                                            dy * i / samples)};
                                    if (!terrain.contains(cp)) continue;
                                    terrain.add_wear(cp, terrain.cell);
                                    wear_cells_.insert(
                                        (static_cast<std::uint64_t>(
                                             static_cast<std::uint32_t>(
                                                 TerrainGrid::cell_of(
                                                     cp.x, terrain.cell)))
                                         << 32) |
                                        static_cast<std::uint32_t>(
                                            TerrainGrid::cell_of(
                                                cp.y, terrain.cell)));
                                }
                            }
                        }
                        ev.participants.push_back(e.id);
                        ev.evidence += "range " + std::to_string(e.id) + " ->(" +
                            std::to_string(np.x) + "," + std::to_string(np.y) +
                            "); ";
                    }
                }
            }
        }
    }

    // ---- husbandry: bonded yields & watchdog alerts (Phase 18) ---------------
    // A bonded animal (a `bonded` relation edge to a living partner) with
    // milk_yield/wool_yield produces those materials on a stateless credit
    // schedule — yield units per 1000 ticks accumulate in *_credit and each
    // whole unit spawns one item entity, so batched catch-up is exact. A
    // bonded watchdog (scent_alert_radius > 0) smells a higher-tier
    // predator inside its radius and warns its partner's mind with a
    // `peril:predator_scent` belief — fog-of-war early warning, learned
    // from the animal's nose, not from a script.
    {
        const PropId p_milk_y = pid("milk_yield");
        const PropId p_wool_y = pid("wool_yield");
        const PropId p_milk_c = pid("milk_credit");
        const PropId p_wool_c = pid("wool_credit");
        const PropId p_alert = pid("scent_alert_radius");
        const PropId p_alert_cd = pid("alert_cooldown");
        const PropId p_tier = pid("trophic_tier");
        const PropId p_hun = pid("hunger");
        const PropId p_qty = pid("quantity");
        const bool do_husb = (p_milk_y != kNoProp || p_wool_y != kNoProp ||
                              p_alert != kNoProp);
        if (do_husb) {
            for (auto& ep : entities) {
                Entity& e = *ep;
                if (!e.alive) continue;
                EntityId partner = kNoEntity;
                for (const Relation& rel : relations_of(e.id, "bonded")) {
                    const EntityId o = rel.a == e.id ? rel.b : rel.a;
                    const Entity* oe = find(o);
                    if (oe && oe->alive) { partner = o; break; }
                }
                if (partner == kNoEntity) continue;
                // periodic yields — hungry bonded stock gives less
                const std::int64_t hunger_pen =
                    p_hun != kNoProp && prop_int(e, p_hun, 0) > 600 ? 500 : 1000;
                auto yield = [&](PropId yp, PropId cp, const char* matname) {
                    if (yp == kNoProp || cp == kNoProp) return;
                    const std::int64_t y = prop_int(e, yp, 0);
                    if (y <= 0) return;
                    const std::int64_t c0 = prop_int(e, cp, 0);
                    std::int64_t c1 = c0 +
                        y * static_cast<std::int64_t>(n) * hunger_pen / 1000;
                    while (c1 >= 1000) {
                        c1 -= 1000;
                        SpawnSpec s;
                        s.kind = matname;
                        const MaterialDef* md = materials.find_name(matname);
                        s.material = md ? md->id : kNoMaterial;
                        const std::uint64_t h = Rng::hash(
                            static_cast<std::uint64_t>(e.id) * 0xD1B54A32D192ED03ull
                                + c1);
                        s.pos = {e.pos.x + static_cast<std::int32_t>(h % 21) - 10,
                                 e.pos.y + static_cast<std::int32_t>((h >> 21) % 21) - 10};
                        if (p_qty != kNoProp)
                            s.props.set(p_qty, Value::integer(1));
                        spawns.push_back(std::move(s));
                        ev.evidence += std::string(matname) + " x1 from " +
                                       std::to_string(e.id) + "; ";
                    }
                    if (c1 != c0) {
                        StateChange ch;
                        ch.entity = e.id; ch.prop = cp;
                        ch.had_before = prop_of(e, cp) != nullptr;
                        ch.before = Value::integer(c0);
                        ch.after = Value::integer(c1);
                        e.props.set(cp, ch.after);
                        ev.changes.push_back(ch);
                    }
                };
                yield(p_milk_y, p_milk_c, "milk");
                yield(p_wool_y, p_wool_c, "wool");
                // watchdog scent alert
                if (p_alert != kNoProp) {
                    const std::int64_t ar = prop_int(e, p_alert, 0);
                    if (ar <= 0) continue;
                    std::int64_t cd = p_alert_cd != kNoProp
                        ? prop_int(e, p_alert_cd, 0) : 0;
                    if (cd > 0) {
                        const std::int64_t cd1 =
                            std::max<std::int64_t>(0, cd - static_cast<std::int64_t>(n));
                        if (p_alert_cd != kNoProp && cd1 != cd) {
                            StateChange ch;
                            ch.entity = e.id; ch.prop = p_alert_cd;
                            ch.had_before = prop_of(e, p_alert_cd) != nullptr;
                            ch.before = Value::integer(cd);
                            ch.after = Value::integer(cd1);
                            e.props.set(p_alert_cd, ch.after);
                            ev.changes.push_back(ch);
                        }
                        cd = cd1;
                    }
                    if (cd > 0) continue;
                    const std::int64_t own_tier = p_tier != kNoProp
                        ? prop_int(e, p_tier, 0) : 0;
                    std::vector<EntityId> near = spatial.within(e.pos, ar);
                    std::sort(near.begin(), near.end());
                    EntityId threat = kNoEntity;
                    for (EntityId oid : near) {
                        if (oid == e.id || oid == partner) continue;
                        const Entity* o = find(oid);
                        if (!o || !o->alive) continue;
                        const std::int64_t ot = p_tier != kNoProp
                            ? prop_int(*o, p_tier, 0) : 0;
                        if (ot > own_tier && ot >= 2) { threat = oid; break; }
                    }
                    if (threat != kNoEntity) {
                        auto mit = minds_.find(partner);
                        if (mit != minds_.end()) {
                            AgentMind& pm = mit->second;
                            Belief& b = pm.belief("peril:predator_scent");
                            b.confidence = std::min<std::int64_t>(1000, b.confidence + 300);
                            b.evidence_for += 300;
                            b.last_tick = now;
                            if (b.status == BeliefStatus::Unknown ||
                                b.status == BeliefStatus::Hypothesized)
                                b.status = BeliefStatus::Observed;
                        }
                        if (p_alert_cd != kNoProp) {
                            StateChange ch;
                            ch.entity = e.id; ch.prop = p_alert_cd;
                            ch.had_before = prop_of(e, p_alert_cd) != nullptr;
                            ch.before = Value::integer(0);
                            ch.after = Value::integer(500);
                            e.props.set(p_alert_cd, ch.after);
                            ev.changes.push_back(ch);
                        }
                        ev.participants.push_back(e.id);
                        ev.participants.push_back(partner);
                        ev.participants.push_back(threat);
                        ev.evidence += "watchdog " + std::to_string(e.id) +
                            " scent-alerts " + std::to_string(partner) +
                            " to " + std::to_string(threat) + "; ";
                    }
                }
            }
        }
    }

    // ---- edge influx ------------------------------------------------------------
    // Stateless schedule: zone fires on ticks where stream_seed(seed, zone,
    // tick) % interval == 0 — irregular but deterministic spacing, no mutable
    // zone state to hash/save. Capacity is judged at commit time.
    if (!influx.defs().empty()) {
        for (const InfluxZone& z : influx.defs()) {
            if (z.interval <= 0 || z.templates.empty()) continue;
            for (std::uint64_t t = from; t <= now; ++t) {
                const std::uint64_t h =
                    Rng::stream_seed(seed, "influx:" + z.name, t);
                if (h % static_cast<std::uint64_t>(z.interval) != 0) continue;
                if (z.capacity > 0) {
                    std::int64_t present = 0;
                    for (const auto& ep : entities) {
                        const Entity& e = *ep;
                        if (!e.alive || !z.contains(e.pos)) continue;
                        for (const InfluxMigrant& tp : z.templates)
                            if (e.kind_name == tp.kind) { ++present; break; }
                    }
                    if (present >= z.capacity) continue;
                }
                for (std::int64_t i = 0; i < z.migrants; ++i) {
                    const std::uint64_t mix = t ^ (static_cast<std::uint64_t>(i) << 32);
                    const std::size_t ti = static_cast<std::size_t>(
                        Rng::stream_seed(seed, "influx:" + z.name + ":pick", mix) %
                        z.templates.size());
                    const InfluxMigrant& tp = z.templates[ti];
                    const std::int64_t w = static_cast<std::int64_t>(z.x1) - z.x0 + 1;
                    const std::int64_t hgt = static_cast<std::int64_t>(z.y1) - z.y0 + 1;
                    SpawnSpec s;
                    s.kind = tp.kind;
                    s.material = tp.material;
                    s.props = tp.props;
                    s.pos = {
                        z.x0 + static_cast<std::int32_t>(
                            Rng::stream_seed(seed, "influx:" + z.name + ":x", mix) %
                            static_cast<std::uint64_t>(w)),
                        z.y0 + static_cast<std::int32_t>(
                            Rng::stream_seed(seed, "influx:" + z.name + ":y",
                                             mix ^ 0xD1B54A32D192ED03ull) %
                            static_cast<std::uint64_t>(hgt))};
                    for (const InfluxGene& g : tp.genes)
                        s.genome.set(g.prop, g.allele);
                    if (!tp.innate.empty())
                        spawn_minds.push_back({spawns.size(), &tp});
                    spawns.push_back(std::move(s));
                }
                ev.evidence += "influx " + z.name + " t=" + std::to_string(t) +
                               " (" + std::to_string(z.migrants) + "); ";
            }
        }
    }

    // range drift moves entities without prop changes — participants alone
    // must also keep the event alive or those steps go unledgered.
    if (spawns.empty() && ev.changes.empty() && ev.participants.empty()) return;
    std::vector<EntityId> spawn_ids(spawns.size(), kNoEntity);
    for (std::size_t i = 0; i < spawns.size(); ++i) {
        const SpawnSpec& s = spawns[i];
        const EntityId nid = create_entity(s.kind, s.material, s.pos, s.props);
        if (nid == kNoEntity) {
            ev.evidence += "ecology spawn failed: " + s.kind + "; ";
            continue;
        }
        if (!s.genome.loci.empty())
            if (Entity* ne = find(nid)) ne->genome = s.genome;
        ev.spawned.push_back(nid);
        spawn_ids[i] = nid;
    }
    // Phase 11.2: a sprout landing on a contaminated soil patch inherits
    // the banked strain — the patch's host range (soil_host_kind, "" =
    // generalist) decides which species the memory can hurt.
    {
        const PropId susc_p = pid("blight_susceptibility"),
                     ps_p = pid("pathogen_strain"),
                     pl_p = pid("pathogen_load"),
                     ab_p = pid("antibody_level"),
                     ssl_p = pid("soil_spore_load"),
                     ssr_p = pid("soil_strain_ref"),
                     shk_p = pid("soil_host_kind"),
                     ie_p = pid("infection_elapsed"),
                     ila_p = pid("infection_load_accum"),
                     iba_p = pid("infection_ab_accum"),
                     sa_p = pid("shed_accum");
        if (susc_p != kNoProp && ssl_p != kNoProp && ssr_p != kNoProp &&
            ps_p != kNoProp && pl_p != kNoProp) {
            for (EntityId nid : spawn_ids) {
                if (nid == kNoEntity) continue;
                Entity* ne = find(nid);
                if (!ne || !ne->alive) continue;
                if (prop_int(*ne, susc_p, 0) <= 0) continue;
                std::vector<EntityId> near = spatial.within(ne->pos, 32);
                std::sort(near.begin(), near.end());
                for (EntityId pid2 : near) {
                    Entity* patch = find(pid2);
                    if (!patch || !patch->alive) continue;
                    if (prop_int(*patch, ssl_p, 0) <= 200) continue;
                    const std::int64_t seid = prop_int(*patch, ssr_p, 0);
                    if (seid <= 0) continue;
                    const Entity* strain = find(static_cast<EntityId>(seid));
                    if (!strain || !strain->alive) continue;
                    if (shk_p != kNoProp) {
                        const Value* hk = prop_of(*patch, shk_p);
                        if (hk && hk->type == ValType::Text &&
                            !hk->s.empty() && hk->s != ne->kind_name)
                            continue;    // strain not adapted to this species
                    }
                    bool has = false;
                    for (const Relation& rr : relations_of(nid, "carries")) {
                        if (rr.a != nid) continue;
                        const Entity* x = find(rr.b);
                        if (x && x->alive &&
                            prop_int(*x, ps_p, 0) == seid) {
                            has = true; break;
                        }
                    }
                    if (has) break;
                    PropertyBag pb;
                    pb.set(ps_p, Value::integer(seid));
                    pb.set(pl_p, Value::integer(200));
                    if (ab_p != kNoProp) pb.set(ab_p, Value::integer(0));
                    if (ie_p != kNoProp) pb.set(ie_p, Value::integer(0));
                    if (ila_p != kNoProp) pb.set(ila_p, Value::integer(0));
                    if (iba_p != kNoProp) pb.set(iba_p, Value::integer(0));
                    if (sa_p != kNoProp) pb.set(sa_p, Value::integer(0));
                    const EntityId inf = create_entity(
                        "infection", kNoMaterial, ne->pos, pb);
                    if (inf == kNoEntity) break;
                    relations.relate(nid, "carries", inf);
                    ev.spawned.push_back(inf);
                    ev.evidence += "sprout infected from soil; ";
                    break;                      // one soil memory per sprout
                }
            }
        }
    }
    // innate-verbs migrants arrive sapient: an empty AgentMind with their
    // instinct set — everything else they must learn by contact/experiment.
    for (const auto& [idx, tp] : spawn_minds) {
        if (idx >= spawn_ids.size() || spawn_ids[idx] == kNoEntity) continue;
        AgentMind& m = mind(spawn_ids[idx]);
        for (const std::string& v : tp->innate) m.learn_action(v);
    }
    ledger.append(std::move(ev));
}

void World::note_touched(const CausalEvent& ev) {
    if (ev.failed || ev.action == "metabolize" || ev.action == "cool" ||
        ev.action == "ecology") return;   // drift is not provenance
    for (const StateChange& c : ev.changes) touched_[c.entity] = ev.id;
    for (EntityId s : ev.spawned) touched_[s] = ev.id;
}

EventId World::last_event_of(EntityId id) const {
    const auto it = touched_.find(id);
    return it == touched_.end() ? kNoEvent : it->second;
}

std::vector<EventId> World::provenance_of(EntityId id) const {
    std::vector<EventId> out;
    const EventId last = last_event_of(id);
    if (last == kNoEvent) return out;
    out = ledger.ancestors(last);
    out.push_back(last);
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<EventId> World::auto_parents(const InteractionContext& ctx) const {
    std::vector<EventId> out = ctx.parent_events;
    auto add = [&](EntityId e) {
        const EventId ev = last_event_of(e);
        if (ev != kNoEvent && std::find(out.begin(), out.end(), ev) == out.end()) out.push_back(ev);
    };
    for (EntityId t : ctx.targets) add(t);
    for (const Relation& rel : relations_of(ctx.actor, "wielding"))
        if (rel.a == ctx.actor) add(rel.b);               // the tool's own history matters
    if (out.size() > 16) out.resize(16);
    return out;
}

std::string World::explain_event(EventId id, int max_depth) const {
    std::ostringstream o;
    const CausalEvent* e = ledger.find(id);
    if (!e) return "no such event\n";
    o << "#" << id << " t=" << e->tick << " action=" << e->action
      << (e->failed ? " FAILED: " + e->failure : std::string()) << "\n";
    o << "  laws:";
    for (LawId l : e->applied_laws) {
        std::string nm = "?";
        for (const auto& m : laws) if (m->id() == l) nm = m->name();
        o << " " << nm;
    }
    o << "\n  conditions:";
    for (const Condition& c : e->conditions)
        o << " [" << c.what << " " << c.actual << (c.met ? " >= " : " < ") << c.required << "]";
    o << "\n  changes:";
    for (const StateChange& c : e->changes) {
        const PropertyDef* pd = props.find(c.prop);
        o << " e" << c.entity << "." << (pd ? pd->name : "?") << " "
          << value_str(c.before) << "->" << value_str(c.after);
    }
    if (!e->spawned.empty()) { o << "\n  spawned:"; for (EntityId s : e->spawned) o << " e" << s; }
    o << "\n";
    if (max_depth > 0)
        for (EventId p : e->parent_events) o << explain_event(p, max_depth - 1);
    return o.str();
}

InteractionResult World::interact(const InteractionContext& ctx) {
    InteractionResult res;
    // Phase 9 LOD: a dormant participant must be judged by its CURRENT
    // needs, not stale ones — flush pending drift before prediction.
    {
        std::vector<EntityId> parts;
        if (ctx.actor != kNoEntity) parts.push_back(ctx.actor);
        for (EntityId t : ctx.targets)
            if (std::find(parts.begin(), parts.end(), t) == parts.end())
                parts.push_back(t);
        flush_needs(parts);
    }
    // Phase 10: clinical unconsciousness (blood_volume < 300 ->
    // consciousness < 300) bars ALL voluntary action — one gate, every
    // verb. Non-circulatory entities have no consciousness prop -> pass.
    if (ctx.actor != kNoEntity) {
        if (const Entity* ae = find(ctx.actor)) {
            const PropId cs_p = SIMLAB_PID(props, "consciousness");
            if (cs_p != kNoProp) {
                const Value* cv = prop_of(*ae, cs_p);
                if (cv && cv->type == ValType::Integer && cv->i < 300) {
                    res.failure = "unconscious (consciousness " +
                        std::to_string(cv->i) + ")";
                    return res;
                }
            }
        }
    }
    // Pass 1: first supporting law model predicts (deterministic order).
    PredictionResult pred;
    const INaturalLawModel* used = nullptr;
    LawId refused_by = 0;
    std::vector<Condition> refused_conditions;
    // CL-1 shadow brain: encode + predict BEFORE any law commits — the
    // sensor sees pre-action World Truth through the mind's own memory.
    if (ctx.actor != kNoEntity) brain_pre(*this, ctx.actor, ctx);
    for (const auto& m : laws) {
        if (!m->supports(*this, ctx)) continue;
        PredictionResult p = m->predict(*this, ctx);
        if (p.possible) { pred = std::move(p); used = m.get(); break; }
        if (pred.failure.empty() && !p.failure.empty()) {
            pred.failure = p.failure;
            refused_by = m->id();
            refused_conditions = std::move(p.conditions);
        }
    }
    if (!used) {
        res.failure = pred.failure.empty() ? "no natural law supports this interaction" : pred.failure;
        // A law understood the action but the world refused it (reach, force,
        // energy, temperature...). The attempt is still a causal fact —
        // experimental evidence (TODO §5) — recorded without any state change.
        const Entity* ae = find(ctx.actor);
        if (refused_by != 0 && ae && ae->alive) {
            CausalEvent fe;
            fe.tick = clock_.tick();
            fe.participants.push_back(ctx.actor);
            for (EntityId t : ctx.targets) fe.participants.push_back(t);
            fe.parent_events = auto_parents(ctx);
            fe.action = ctx.parameters.action;
            fe.applied_laws.push_back(refused_by);
            fe.failed = true; fe.failure = res.failure; fe.evidence = res.failure;
            fe.conditions = std::move(refused_conditions);
            res.failed_event = ledger.append(std::move(fe));
            if (const CausalEvent* fev = ledger.find(res.failed_event))
                brain_post(*this, ctx.actor, *fev);
        }
        return res;
    }
    // Spawns are validated BEFORE anything mutates: a spawn that would be
    // rejected after the props changed would destroy inputs without products.
    for (const SpawnSpec& s : pred.spawns) {
        bool bad = s.kind.empty();
        for (const auto& [pid, v] : s.props.entries) {
            const PropertyDef* d = props.find(pid);
            if (!d || !props.value_ok(*d, v)) bad = true;
        }
        if (bad) { res.failure = "commit rejected (invalid spawn: " + s.kind + ")"; return res; }
    }
    // Pass 2: commit changes atomically (all-or-nothing per law's prediction).
    CausalEvent ev;
    ev.tick = clock_.tick();
    ev.participants.push_back(ctx.actor);
    for (EntityId t : ctx.targets) ev.participants.push_back(t);
    ev.parent_events = auto_parents(ctx);
    ev.conditions = pred.conditions;
    ev.action = ctx.parameters.action;
    ev.applied_laws.push_back(used->id());
    for (const std::string& n : pred.notes) ev.evidence += n + "; ";
    bool ok = true;
    for (const StateChange& ch : pred.changes) {
        Entity* e = find(ch.entity);
        if (!e) { ok = false; break; }
        StateChange committed = ch;
        committed.before = Value::integer(0); committed.had_before = false;
        if (const Value* cur = prop_of(*e, ch.prop)) { committed.before = *cur; committed.had_before = true; }
        if (const PropertyDef* d = props.find(ch.prop)) {
            if (!props.value_ok(*d, ch.after)) { ok = false; break; }
        }
        e->props.set(ch.prop, ch.after);
        ev.changes.push_back(committed);
        res.changes.push_back(committed);
    }
    if (!ok) {
        // Rollback: restore recorded `before` values in reverse order.
        for (auto it = ev.changes.rbegin(); it != ev.changes.rend(); ++it) {
            Entity* e = find(it->entity);
            if (!e) continue;
            if (it->had_before) e->props.set(it->prop, it->before);
            else e->props.erase(it->prop);
        }
        res.changes.clear();
        res.failure = "commit rejected (invalid property value)";
        return res;
    }
    // Entity destroyed when integrity reaches 0.
    std::vector<EntityId> died;
    for (const StateChange& ch : ev.changes) {
        const PropertyDef* d = props.find(ch.prop);
        if (d && d->name == "integrity" && ch.after.type == ValType::Integer && ch.after.i <= 0)
            if (Entity* e = find(ch.entity)) { e->alive = false; spatial.erase(ch.entity); died.push_back(ch.entity); }
    }
    // Structural invariant (engine-level, deterministic): a destroyed
    // container releases its `part_of` parts (contained -> false, edge cut);
    // `wielding` edges to/from the dead are dropped. Without this, parts
    // would be orphaned inside a structure that no longer exists.
    if (!died.empty()) {
        const PropId cont = SIMLAB_PID(props, "contained");
        std::vector<Relation> cut;
        for (const Relation& rel : relations.edges())
            for (EntityId d : died)
                if (rel.a == d || rel.b == d) { cut.push_back(rel); break; }
        for (auto it = social_.begin(); it != social_.end();) {
            if (std::find(died.begin(), died.end(), it->first.a) != died.end() ||
                std::find(died.begin(), died.end(), it->first.b) != died.end())
                it = social_.erase(it);
            else ++it;
        }
        for (const Relation& rel : cut) {
            relations.unrelate(rel.a, rel.type, rel.b);
            ev.evidence += "rel- " + rel.type + " " + std::to_string(rel.a) +
                          "->" + std::to_string(rel.b) + "; ";
            if (rel.type == "part_of" && rel.b != kNoEntity) {
                // the container (rel.b) died -> free the part (rel.a)
                bool container_died = false;
                for (EntityId d : died) if (d == rel.b) container_died = true;
                if (container_died && cont != kNoProp)
                    if (Entity* part = find(rel.a)) {
                        const Value* cv = prop_of(*part, cont);
                        const bool was = cv && cv->type == ValType::Boolean && cv->b;
                        part->props.set(cont, Value::boolean(false));
                        if (was) {
                            StateChange rc;
                            rc.entity = rel.a; rc.prop = cont; rc.had_before = true;
                            rc.before = Value::boolean(true); rc.after = Value::boolean(false);
                            ev.changes.push_back(rc); res.changes.push_back(rc);
                        }
                    }
            }
        }
    }
    // Relation mutations commit with the same atomicity as prop changes.
    for (const RelationChange& rc : pred.rel_changes) {
        if (rc.add) relations.relate(rc.a, rc.type, rc.b);
        else relations.unrelate(rc.a, rc.type, rc.b);
        ev.evidence += std::string(rc.add ? "rel+ " : "rel- ") + rc.type +
                      " " + std::to_string(rc.a) + "->" + std::to_string(rc.b) + "; ";
    }
    // Law-proposed relocations commit atomically with the event (Phase 9
    // locomotion): position is World Truth, so it moves through the ledger
    // like every other state change — evidence records where it went.
    for (const MoveSpec& mv : pred.moves) {
        Entity* me = find(mv.entity);
        if (!me || !me->alive) continue;
        const Vec2 from = me->pos;
        me->pos = mv.to;
        spatial.move(mv.entity, mv.to);
        // Phase 12.4: footfall compacts the ground — every cell the step
        // crosses gains traffic_wear (trails emerge from use, fade linearly).
        if (!terrain.traffic_wear.empty() && terrain.cell > 0) {
            const std::int64_t dx =
                static_cast<std::int64_t>(mv.to.x) - from.x;
            const std::int64_t dy =
                static_cast<std::int64_t>(mv.to.y) - from.y;
            const std::int64_t d2 = dx * dx + dy * dy;
            if (d2 > 0) {
                std::int64_t dist = 1;
                while (dist * dist < d2) ++dist;   // ceil sqrt
                const std::int64_t step = terrain.cell / 2;
                std::int64_t samples = dist / step;
                if (samples < 1) samples = 1;
                for (std::int64_t i = 0; i <= samples; ++i) {
                    const Vec2 p{
                        from.x + static_cast<std::int32_t>(
                            dx * i / samples),
                        from.y + static_cast<std::int32_t>(
                            dy * i / samples)};
                    if (!terrain.contains(p)) continue;
                    terrain.add_wear(p, terrain.cell);
                    wear_cells_.insert(
                        (static_cast<std::uint64_t>(
                             static_cast<std::uint32_t>(
                                 TerrainGrid::cell_of(p.x,
                                                      terrain.cell)))
                         << 32) |
                        static_cast<std::uint32_t>(
                            TerrainGrid::cell_of(p.y, terrain.cell)));
                }
            }
        }
        ev.evidence += "move " + std::to_string(mv.entity) + " ->(" +
                       std::to_string(mv.to.x) + "," + std::to_string(mv.to.y) + "); ";
    }
    // Law-proposed spawns commit after state changes (TODO §9 extraction/
    // processing produce NEW entities — World Truth grows causally).
    for (const SpawnSpec& s : pred.spawns) {
        const EntityId nid = create_entity(s.kind, s.material, s.pos, s.props);
        if (nid == kNoEntity) {
            ev.evidence += "spawn failed: " + s.kind + "; ";
            continue;
        }
        if (!s.genome.loci.empty())
            if (Entity* ne = find(nid)) ne->genome = s.genome;
        // CL-1 Phase 2: inherited brain installs with slow weights +
        // hyper-genes; h/replay/fast state starts empty (Baldwin).
        if (s.brain.valid) brain_install_spawn(*this, nid, s.brain);
        // Epigenetic marks (Nhịp 3): the newborn's AgentMind is born with
        // vague trauma engrams keyed by material/kind — born_memories,
        // never entity memory. Only created when marks actually transmit.
        if (!s.legacy.empty()) {
            if (Entity* ne = find(nid)) {
                const MindParams mp = mind_params(*this, *ne);
                AgentMind& cm = mind(nid);
                for (const SpawnSpec::LegacyEngram& lg : s.legacy) {
                    BornEngram bm;
                    bm.material = lg.material;
                    bm.kind_name = lg.kind_name;
                    bm.engram.clarity = mp.mark_clarity;
                    bm.engram.stability = mp.mark_stability;
                    bm.engram.emotional_charge =
                        lg.charge * mp.mark_charge_permille / 1000;
                    cm.born_memories.push_back(std::move(bm));
                }
            }
        }
        for (const auto& [type, other] : s.rel_from)
            relations.relate(other, type, nid);
        // Phase 10: newborns of species with a body plan grow anatomy.
        if (build_anatomy(nid))
            ev.evidence += "anatomy " + std::to_string(nid) + "; ";
        ev.evidence += "spawned " + std::to_string(nid) + " " + s.kind + "; ";
        res.spawned.push_back(nid);
    }
    // Collective tithe (Phase 8): members of ration_share groups pay a
    // permille cut of what this event yielded into the common store — same
    // causal event, same atomicity as the harvest itself.
    social_tithe(ev, ctx.actor, res.spawned);
    // Phase 11: a raw meal can jump species — resolved here, post-commit,
    // inside the same causal event (real strain/infection entity ids).
    zoonotic_spillover(ctx, ev, res.spawned);
    // Actions consume simulated time: the law's declared duration wins,
    // else the caller's tick count (default 1). Time passing also
    // metabolizes needs (ledgered as "metabolize" events).
    const std::int64_t dt = pred.ticks > 0 ? pred.ticks : ctx.parameters.ticks;
    ev.spawned = res.spawned;
    if (defer_time) deferred_dt = std::max(deferred_dt, dt);
    else advance_ticks(dt);
    res.completed = true;
    res.law_used = used->id();
    res.event = ledger.append(std::move(ev));
    if (const CausalEvent* committed = ledger.find(res.event)) {
        note_touched(*committed);
        for (const MoveSpec& mv : pred.moves) touched_[mv.entity] = res.event;
    }
    // actor learns from its own action (World Truth -> agent knowledge,
    // one direction only: the mind never writes back into entities)
    if (ctx.actor != kNoEntity) learn_from_event(ctx.actor, res.event);
    if (ctx.actor != kNoEntity && brains_)
        if (const CausalEvent* cev = ledger.find(res.event))
            brain_post(*this, ctx.actor, *cev);
    // witness learning (TODO §10.1): other living agents whose sight_range
    // covers the actor observe the event and learn the verb + WEAK evidence.
    // Only entities that already have a mind count — an entity becomes an
    // agent by perceiving, not by existing.
    if (speculative) return res;   // planning copy: no witness/social bookkeeping
    if (const CausalEvent* pev = ledger.find(res.event)) {
        const Entity* ae = find(ctx.actor);
        if (ae) {
            const PropId sight = SIMLAB_PID(props, "sight_range");
            for (auto& [wid, wm] : minds_) {
                (void)wm;
                if (wid == ctx.actor) continue;
                const Entity* we = find(wid);
                if (!we || !we->alive) continue;
                const std::int64_t range = sight != kNoProp
                    ? prop_int(*we, sight, 0) : 0;
                if (range <= 0) continue;
                if (dist2(we->pos, ae->pos) > range * range) continue;
                witness_from_event(*this, wid, *pev);
            }
        }
    }
    // social consequences (Phase 8): knowledge transfer for communicate/
    // read, trust penalties for rule violations — all post-commit, minds only
    if (const CausalEvent* sev = ledger.find(res.event)) {
        social_from_event(*this, ctx.actor, ctx.targets, *sev);
        simlab::econ_from_event(*this, ctx.actor, ctx.targets, *sev);
        simlab::political_from_event(*this, ctx.actor, ctx.targets, *sev);
        simlab::arch_from_event(*this, ctx.actor, ctx.targets, *sev);
        simlab::myth_from_event(*this, ctx.actor, ctx.targets, *sev);
        simlab::war_from_event(*this, ctx.actor, ctx.targets, *sev);
    }
    return res;
}

std::vector<EventId> World::history_of(EntityId id, std::size_t limit) const {
    std::vector<EventId> out;
    const auto& hist = ledger.history();
    for (auto it = hist.rbegin(); it != hist.rend() && out.size() < limit; ++it) {
        const CausalEvent& ev = *it;
        bool hit = std::find(ev.participants.begin(), ev.participants.end(), id) != ev.participants.end();
        if (!hit)
            for (const StateChange& c : ev.changes)
                if (c.entity == id) { hit = true; break; }
        if (!hit)
            for (EntityId sp : ev.spawned)
                if (sp == id) { hit = true; break; }
        if (hit) out.push_back(ev.id);
    }
    return out;
}

bool World::claim_spawn(std::uint32_t tag, int gx, int gy) {
    return gen_spawned_.emplace(tag, gx, gy).second;
}

void World::social_tithe(CausalEvent& ev, EntityId actor,
                         const std::vector<EntityId>& spawned) {
    if (actor == kNoEntity) return;
    const PropId coll = SIMLAB_PID(props, "collective");
    const PropId ration = SIMLAB_PID(props, "ration_share");
    const PropId qty = SIMLAB_PID(props, "quantity");
    const PropId nut = SIMLAB_PID(props, "nutrition");
    if (coll == kNoProp || ration == kNoProp || qty == kNoProp) return;
    // collectives the actor belongs to that levy a tithe
    std::vector<EntityId> groups;
    for (const Relation& rel : relations_of(actor, "member_of")) {
        if (rel.a != actor) continue;
        const Entity* g = find(rel.b);
        if (!g || !g->alive) continue;
        const Value* rv = prop_of(*g, ration);
        const Value* cv = prop_of(*g, coll);
        if (rv && rv->type == ValType::Integer && rv->i > 0 &&
            cv && cv->type == ValType::Boolean && cv->b)
            groups.push_back(rel.b);
    }
    if (groups.empty()) return;
    // gained resources this event: actor quantity gains + spawned entities
    // carrying nutrition (harvested food). Whole items without `quantity`
    // cannot be partially tithed — noted limitation.
    std::int64_t gained = 0;
    for (const StateChange& ch : ev.changes) {
        if (ch.entity == actor && ch.prop == qty && ch.had_before &&
            ch.after.type == ValType::Integer && ch.before.type == ValType::Integer &&
            ch.after.i > ch.before.i)
            gained += ch.after.i - ch.before.i;
    }
    std::vector<std::pair<EntityId, std::int64_t>> spawn_qty;
    for (EntityId sid : spawned) {
        Entity* se = find(sid);
        if (!se || !se->alive) continue;
        const Value* nv = nut != kNoProp ? prop_of(*se, nut) : nullptr;
        const Value* qv = prop_of(*se, qty);
        if (!nv || nv->type != ValType::Integer || nv->i <= 0) continue;
        if (qv && qv->type == ValType::Integer && qv->i > 0) {
            spawn_qty.push_back({sid, qv->i});
            gained += qv->i;
        }
    }
    if (gained <= 0) return;
    for (EntityId g : groups) {
        const Entity* ge = find(g);
        const Value* rv = ge ? prop_of(*ge, ration) : nullptr;
        if (!rv) continue;
        const std::int64_t share = rv->i;
        std::int64_t tribute = gained * share / 1000;
        if (tribute <= 0) continue;
        // deduct from spawned quantities first (in spawn order), remainder
        // from the actor's own gained quantity if any
        std::int64_t left = tribute;
        for (auto& [sid, q] : spawn_qty) {
            if (left <= 0) break;
            Entity* se = find(sid);
            const Value* qv = se ? prop_of(*se, qty) : nullptr;
            if (!qv) continue;
            const std::int64_t take = std::min(left, qv->i);
            if (take <= 0) continue;
            StateChange ch;
            ch.entity = sid; ch.prop = qty; ch.had_before = true;
            ch.before = Value::integer(qv->i);
            ch.after = Value::integer(qv->i - take);
            se->props.set(qty, Value::integer(qv->i - take));
            ev.changes.push_back(ch);
            left -= take;
        }
        if (left > 0) {
            Entity* ae = find(actor);
            const Value* av = ae ? prop_of(*ae, qty) : nullptr;
            if (av && av->type == ValType::Integer) {
                const std::int64_t take = std::min(left, av->i);
                if (take > 0) {
                    StateChange ch;
                    ch.entity = actor; ch.prop = qty; ch.had_before = true;
                    ch.before = Value::integer(av->i);
                    ch.after = Value::integer(av->i - take);
                    ae->props.set(qty, Value::integer(av->i - take));
                    ev.changes.push_back(ch);
                    left -= take;
                }
            }
        }
        const std::int64_t paid = tribute - left;
        if (paid <= 0) continue;
        Entity* ge2 = find(g);
        const Value* gq = ge2 ? prop_of(*ge2, qty) : nullptr;
        if (!gq || gq->type != ValType::Integer) continue;
        const std::int64_t after = gq->i + paid;
        StateChange ch;
        ch.entity = g; ch.prop = qty; ch.had_before = true;
        ch.before = Value::integer(gq->i); ch.after = Value::integer(after);
        ge2->props.set(qty, Value::integer(after));
        ev.changes.push_back(ch);
        ev.evidence += "tithe " + std::to_string(paid) + " -> collective " +
                      std::to_string(g) + "; ";
    }
}

std::map<MaterialId, std::int64_t> World::material_inventory() const {
    std::map<MaterialId, std::int64_t> inv;
    if (const PropertyDef* pq = props.find_name("quantity")) {
        for (const auto& ep : entities) {
            const Entity& e = *ep;
            if (!e.alive || e.material == kNoMaterial) continue;
            if (const Value* v = e.props.get(pq->id)) inv[e.material] += v->i;
        }
    }
    return inv;
}

std::pair<std::int64_t, std::int64_t> World::energy_accounting() const {
    // Derive energy spent/gained purely from the causal ledger: every
    // `energy` prop change records before/after — no separate counter needed.
    std::int64_t spent = 0, gained = 0;
    if (const PropertyDef* pe = props.find_name("energy")) {
        for (const CausalEvent& ev : ledger.history())
            for (const StateChange& c : ev.changes)
                if (c.prop == pe->id && c.had_before &&
                    c.before.type == ValType::Integer && c.after.type == ValType::Integer) {
                    const std::int64_t d = c.before.i - c.after.i;
                    if (d > 0) spent += d; else gained -= d;
                }
    }
    return {spent, gained};
}

std::int64_t World::tie_weight(EntityId a, const std::string& type, EntityId b) const {
    const auto it = social_.find({a, type, b});
    return it == social_.end() ? 0 : it->second;
}
void World::adjust_tie(EntityId a, const std::string& type, EntityId b, std::int64_t delta) {
    if (a == kNoEntity || b == kNoEntity || a == b || delta == 0) return;
    Relation key{a, type, b};
    std::int64_t v = (social_.count(key) ? social_.at(key) : 0) + delta;
    v = std::clamp<std::int64_t>(v, -1000, 1000);
    if (v == 0) social_.erase(key); else social_[key] = v;
}

AgentMind& World::mind(EntityId npc) { return minds_[npc]; }
const AgentMind* World::find_mind(EntityId npc) const {
    auto it = minds_.find(npc);
    return it == minds_.end() ? nullptr : &it->second;
}
std::vector<EntityId> World::minded_entities() const {
    std::vector<EntityId> out;
    out.reserve(minds_.size());
    for (const auto& [id, m] : minds_) { (void)m; out.push_back(id); }
    return out;
}
std::size_t World::perceive(EntityId npc) { return simlab::perceive(*this, npc); }
void World::learn_from_event(EntityId npc, EventId ev) {
    simlab::learn_from_event(*this, npc, ev);
}

std::pair<std::int64_t, std::int64_t> World::tool_bonus(EntityId actor) const {
    std::int64_t reach = 0, edge = 0;
    const PropId thrust = SIMLAB_PID(props, "thrust_range");
    const PropId edgep = SIMLAB_PID(props, "edge");
    const PropId cont = SIMLAB_PID(props, "contained");
    for (const Relation& rel : relations_of(actor, "wielding")) {
        if (rel.a != actor) continue;                    // actor wields rel.b
        const Entity* t = find(rel.b);
        if (!t || !t->alive) continue;
        if (cont != kNoProp) {
            const Value* cv = prop_of(*t, cont);
            if (cv && cv->type == ValType::Boolean && cv->b) continue;
        }
        if (thrust != kNoProp) reach = std::max(reach, prop_int(*t, thrust, 0));
        // geometry-derived reach: a long rod reaches farther than its
        // declared thrust_range — shape is state, not a tag (§8.5)
        reach = std::max(reach, geom_stats(*this, rel.b).swing);
        if (edgep != kNoProp) edge = std::max(edge, prop_int(*t, edgep, 0));
    }
    return {reach, edge};
}

Rng& World::rng(const std::string& stream) {
    const auto it = rngs_.find(stream);
    if (it != rngs_.end()) return *it->second;
    auto r = std::make_unique<Rng>(Rng::stream_seed(seed, stream));
    Rng& ref = *r;
    rngs_[stream] = std::move(r);
    return ref;
}

std::uint64_t World::content_hash() const {
    auto fnv = [](std::uint64_t h, std::uint64_t v) {
        for (int i = 0; i < 8; ++i) { h ^= (v >> (i * 8)) & 0xFF; h *= 1099511628211ull; }
        return h;
    };
    auto fnvs = [&](std::uint64_t h, const std::string& s) {
        h = fnv(h, s.size());
        for (char c : s) { h ^= static_cast<unsigned char>(c); h *= 1099511628211ull; }
        return h;
    };
    std::vector<std::uint64_t> per;
    for (const auto& ep : entities) {
        const Entity& e = *ep;
        if (!e.alive) continue;
        std::uint64_t h = 1469598103934665603ull;
        h = fnvs(h, e.kind_name);
        h = fnv(h, e.material); h = fnv(h, e.deposit);
        h = fnv(h, static_cast<std::uint32_t>(e.pos.x)); h = fnv(h, static_cast<std::uint32_t>(e.pos.y));
        for (const auto& [pid, v] : e.props.entries) {
            h = fnv(h, pid); h = fnv(h, static_cast<std::uint64_t>(v.type));
            h = fnv(h, static_cast<std::uint64_t>(v.i)); h = fnv(h, v.b ? 1 : 0);
            h = fnvs(h, v.s); h = fnv(h, v.target);
        }
        h = fnv(h, e.genome.loci.size());
        for (const auto& [pid, v] : e.genome.loci) { h = fnv(h, pid); h = fnv(h, static_cast<std::uint64_t>(v)); }
        per.push_back(h);
    }
    std::sort(per.begin(), per.end());
    std::uint64_t h = 1469598103934665603ull;
    h = fnv(h, per.size());
    for (std::uint64_t x : per) h = fnv(h, x);
    auto vec = [&](const std::vector<std::int32_t>& v) {
        h = fnv(h, v.size());
        for (std::int32_t x : v) h = fnv(h, static_cast<std::uint32_t>(x));
    };
    vec(terrain.elevation); vec(terrain.moisture); vec(terrain.temperature);
    vec(terrain.vegetation); vec(terrain.slope); vec(terrain.water_table);
    vec(terrain.traffic_wear);
    for (std::uint8_t x : terrain.water) h = fnv(h, x);
    for (const auto& col : subsurface.cells)
        for (const Stratum& s : col) { h = fnv(h, s.material); h = fnv(h, static_cast<std::uint32_t>(s.top)); h = fnv(h, static_cast<std::uint32_t>(s.bottom)); }
    return h;
}

std::uint64_t World::state_hash() const {
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&](std::uint64_t v) {
        for (int i = 0; i < 8; ++i) { h ^= (v >> (i * 8)) & 0xFF; h *= 1099511628211ull; }
    };
    auto mixs = [&](const std::string& s) {
        mix(s.size());                       // length prefix: ("ab","c") != ("a","bc")
        for (char c : s) { h ^= static_cast<unsigned char>(c); h *= 1099511628211ull; }
    };
    mix(seed);
    // content signature: same state over different data must not hash equal
    mix(props.defs().size());
    for (const PropertyDef& d : props.defs()) { mixs(d.name); mix(static_cast<std::uint64_t>(d.type)); }
    mix(materials.defs().size());
    for (const MaterialDef& m : materials.defs()) {
        mixs(m.name);
        for (const auto& [pid, v] : m.props) {
            mix(pid); mix(static_cast<std::uint64_t>(v.i)); mix(v.b ? 1 : 0); mixs(v.s); mix(v.target);
        }
    }
    mix(deposits.defs().size());
    for (const DepositDef& d : deposits.defs()) mixs(d.name);
    // influx zones steer spawning — part of the world's effective content
    mix(influx.defs().size());
    for (const InfluxZone& z : influx.defs()) {
        mixs(z.name);
        mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(z.x0)));
        mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(z.y0)));
        mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(z.x1)));
        mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(z.y1)));
        mix(static_cast<std::uint64_t>(z.interval));
        mix(static_cast<std::uint64_t>(z.capacity));
        mix(static_cast<std::uint64_t>(z.migrants));
        for (const InfluxMigrant& t : z.templates) {
            mixs(t.name); mixs(t.kind); mix(t.material);
            for (const auto& [pid, v] : t.props.entries) {
                mix(pid); mix(static_cast<std::uint64_t>(v.type));
                mix(static_cast<std::uint64_t>(v.i)); mix(v.b ? 1 : 0);
                mixs(v.s); mix(v.target);
            }
            mix(t.genes.size());
            for (const InfluxGene& g : t.genes) { mix(g.prop); mix(static_cast<std::uint64_t>(g.allele)); }
            mix(t.innate.size());
            for (const std::string& v : t.innate) mixs(v);
        }
    }
    mix(clock_.tick());
    mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(gen_sea_level)));
    mix(world_seed); mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(generator_version)));
    if (ruleset != 0) mix(0x52554C45ull ^ ruleset);   // legacy worlds (0) hash as before
    // CL-1 Phase 2: live brain state (weights + h + replay) is World state —
    // replay must reproduce it bit-identically. Skipped on speculative
    // clones (they share brains_ but are never hashed for replay anyway).
    if (brains_ && !speculative) mix(brain_state_hash(*brains_));
    mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(gen_origin_x)));
    mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(gen_origin_y)));
    mix(kinds.counter());
    for (const auto& e : entities) {
        mix(e->id); mix(e->kind); mix(e->material); mix(e->deposit); mix(e->alive ? 1 : 0);
        mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(e->pos.x)));
        mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(e->pos.y)));
        mixs(e->kind_name);
        for (const auto& [pid, v] : e->props.entries) {
            mix(pid); mix(static_cast<std::uint64_t>(v.type));
            mix(static_cast<std::uint64_t>(v.i)); mix(v.b ? 1 : 0); mixs(v.s); mix(v.target);
        }
        mix(e->genome.loci.size());
        for (const auto& [pid, v] : e->genome.loci) { mix(pid); mix(static_cast<std::uint64_t>(v)); }
        mix(e->last_metabolized);
    }
    // terrain + subsurface fields (worldgen state)
    if (!terrain.empty()) {
        mix(static_cast<std::uint64_t>(terrain.w)); mix(static_cast<std::uint64_t>(terrain.h));
        mix(static_cast<std::uint64_t>(terrain.cell));
        auto mixv = [&](const std::vector<std::int32_t>& v) {
            mix(v.size()); for (std::int32_t x : v) mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)));
        };
        mixv(terrain.elevation); mixv(terrain.moisture); mixv(terrain.temperature);
        mixv(terrain.vegetation); mixv(terrain.slope); mixv(terrain.wind_x); mixv(terrain.wind_y);
        mixv(terrain.water_table); mixv(terrain.traffic_wear);
        for (std::uint8_t x : terrain.water) mix(x);
    }
    if (!subsurface.empty()) {
        for (const auto& col : subsurface.cells)
            for (const Stratum& s : col) { mix(s.material); mix(s.top); mix(s.bottom); }
    }
    for (const auto& [tag, gx, gy] : gen_spawned_) {
        mix(tag); mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(gx)));
        mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(gy)));
    }
    mix(gen_cells_.size());
    for (const auto& [gx, gy] : gen_cells_) {
        mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(gx)));
        mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(gy)));
    }
    for (const Relation& r : relations.edges()) {
        mix(r.a); mixs(r.type); mix(r.b);
    }
    mix(social_.size());
    for (const auto& [rel, wt] : social_) {
        mix(rel.a); mixs(rel.type); mix(rel.b); mix(static_cast<std::uint64_t>(wt));
    }
    for (const auto& ev : ledger.history()) {
        mix(ev.id); mix(ev.tick); mix(ev.applied_laws.size());
        for (LawId l : ev.applied_laws) mix(l);
        mix(ev.changes.size());
        for (const auto& c : ev.changes) {
            mix(c.entity); mix(c.prop); mix(c.had_before ? 1 : 0);
            mix(static_cast<std::uint64_t>(c.before.type)); mix(static_cast<std::uint64_t>(c.before.i));
            mix(c.before.b ? 1 : 0); mixs(c.before.s); mix(c.before.target);
            mix(static_cast<std::uint64_t>(c.after.type)); mix(static_cast<std::uint64_t>(c.after.i));
            mix(c.after.b ? 1 : 0); mixs(c.after.s); mix(c.after.target);
        }
        mix(ev.spawned.size());
        for (EntityId s : ev.spawned) mix(s);
        mix(ev.failed ? 1 : 0); mixs(ev.action); mixs(ev.failure);
        mix(ev.parent_events.size()); for (EventId p : ev.parent_events) mix(p);
        mix(ev.conditions.size());
        for (const Condition& c : ev.conditions) {
            mixs(c.what); mix(static_cast<std::uint64_t>(c.actual));
            mix(static_cast<std::uint64_t>(c.required)); mix(c.met ? 1 : 0);
        }
    }
    // Ledger archive state: retained events above + the digest of everything
    // evicted + the next unassigned id — together they cover FULL history.
    mix(ledger.next_id());
    mix(ledger.archived_hash());
    // rng stream positions: consumed randomness is sim state
    mix(rngs_.size());
    for (const auto& [name, rg] : rngs_) { mixs(name); mix(rg->state()); }
    // agent minds: knowledge is sim state (affects future actions) -> hashed
    mix(minds_.size());
    for (const auto& [nid, m] : minds_) {
        mix(nid);
        mix(m.observations.size());
        for (const Observation& o : m.observations) {
            mix(o.entity); mix(o.tick);
            mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(o.pos.x)));
            mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(o.pos.y)));
            mix(o.props.size());
            for (const auto& [pp, v] : o.props) {
                mix(pp); mix(static_cast<std::uint64_t>(v.type));
                mix(static_cast<std::uint64_t>(v.i)); mix(v.b ? 1 : 0); mixs(v.s); mix(v.target);
            }
        }
        mix(m.beliefs.size());
        for (const auto& [k, b] : m.beliefs) {
            mixs(k); mix(static_cast<std::uint64_t>(b.status));
            mix(static_cast<std::uint64_t>(b.confidence));
            mix(static_cast<std::uint64_t>(b.evidence_for));
            mix(static_cast<std::uint64_t>(b.evidence_against));
            mix(static_cast<std::uint64_t>(b.self_evidence));
            mix(b.last_tick);
        }
        mix(m.known_actions.size());
        for (const std::string& a : m.known_actions) mixs(a);
        mix(m.known_entities.size());
        for (EntityId x : m.known_entities) mix(x);
        mix(m.discoveries.size());
        for (const auto& [k, d] : m.discoveries) {
            mixs(k); mixs(d.claim);
            mix(d.experiments.size());
            for (EventId e : d.experiments) mix(e);
            mix(d.supporting_laws.size());
            for (const std::string& s : d.supporting_laws) mixs(s);
            mix(d.failed_conditions.size());
            for (const Condition& c : d.failed_conditions) {
                mixs(c.what); mix(static_cast<std::uint64_t>(c.actual));
                mix(static_cast<std::uint64_t>(c.required)); mix(c.met ? 1 : 0);
            }
            mix(d.sample_entities.size());
            for (EntityId x : d.sample_entities) mix(x);
            mix(static_cast<std::uint64_t>(d.evidence_for));
            mix(static_cast<std::uint64_t>(d.evidence_against));
            mix(d.trials);
            mix(static_cast<std::uint64_t>(d.status));
            mix(d.last_tick);
            mix(static_cast<std::uint64_t>(d.engram.clarity));
            mix(static_cast<std::uint64_t>(d.engram.stability));
            mix(static_cast<std::uint64_t>(d.engram.emotional_charge));
            mix(static_cast<std::uint64_t>(d.engram.rehearsal_count));
            mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(d.engram.decay_accum)));
            mix(d.engram.last_rehearsal);
        }
        // fog-of-war memory: what the mind last perceived + where it has been
        mix(m.memory.size());
        for (const auto& [eid, s] : m.memory) {
            mix(eid); mix(s.tick); mix(s.material);
            mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(s.pos.x)));
            mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(s.pos.y)));
            mixs(s.kind_name);
            mix(s.props.size());
            for (const auto& [pp, v] : s.props) {
                mix(pp); mix(static_cast<std::uint64_t>(v.type));
                mix(static_cast<std::uint64_t>(v.i)); mix(v.b ? 1 : 0); mixs(v.s); mix(v.target);
            }
            mix(static_cast<std::uint64_t>(s.engram.clarity));
            mix(static_cast<std::uint64_t>(s.engram.stability));
            mix(static_cast<std::uint64_t>(s.engram.emotional_charge));
            mix(static_cast<std::uint64_t>(s.engram.rehearsal_count));
            mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(s.engram.decay_accum)));
            mix(s.engram.last_rehearsal);
        }
        mix(m.visited_cells.size());
        for (const auto& [gx, gy] : m.visited_cells) {
            mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(gx)));
            mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(gy)));
        }
        // Nhịp 2-3: neuro axes, transient emotions, compulsion memory,
        // epigenetic born marks — all part of deterministic mind state.
        const NeuroState& ns = m.neuro;
        mix(ns.tuned ? 1 : 0);
        mix(static_cast<std::uint64_t>(ns.threat_gain));
        mix(static_cast<std::uint64_t>(ns.reward_gain));
        mix(static_cast<std::uint64_t>(ns.pattern_coupling));
        mix(static_cast<std::uint64_t>(ns.empathy_coupling));
        mix(static_cast<std::uint64_t>(ns.low_light_bias));
        mix(static_cast<std::uint64_t>(ns.isolation_bias));
        mix(static_cast<std::uint64_t>(ns.executive_override));
        mix(static_cast<std::uint64_t>(ns.panic));
        mix(static_cast<std::uint64_t>(ns.rage));
        mix(static_cast<std::uint64_t>(ns.grief));
        mix(static_cast<std::uint64_t>(ns.panic_at_event));
        mix(ns.last_event_tick);
        mix(m.panic_relief.size());
        for (const auto& [act, cnt] : m.panic_relief) {
            mixs(act); mix(static_cast<std::uint64_t>(cnt));
        }
        mix(m.born_memories.size());
        for (const BornEngram& bm : m.born_memories) {
            mix(bm.material); mixs(bm.kind_name);
            mix(static_cast<std::uint64_t>(bm.engram.clarity));
            mix(static_cast<std::uint64_t>(bm.engram.stability));
            mix(static_cast<std::uint64_t>(bm.engram.emotional_charge));
            mix(static_cast<std::uint64_t>(bm.engram.rehearsal_count));
            mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(bm.engram.decay_accum)));
            mix(bm.engram.last_rehearsal);
        }
        // v15: herd memory, sublimation relief, ostracism accumulator
        mix(m.recent_witnessed.size());
        for (const WitnessedAct& wa : m.recent_witnessed) {
            mix(wa.tick); mix(wa.actor); mixs(wa.action);
        }
        mix(m.sublimation_relief.size());
        for (const auto& [act, cnt] : m.sublimation_relief) {
            mixs(act); mix(static_cast<std::uint64_t>(cnt));
        }
        mix(static_cast<std::uint64_t>(m.ostracism_accum));
        mix(static_cast<std::uint64_t>(m.political_accum));
        mix(static_cast<std::uint64_t>(m.myth_accum));
    }
    return h;
}

// ---- persistence (TODO §2.8 save/load) ---------------------------------------
// Binary snapshot of World Truth, explicit little-endian. Content references
// (material/deposit/property) are stored by NAME so saves survive registry
// reordering; ids are verified on load. Sections follow state_hash() order.
namespace {

constexpr std::uint32_t kSaveMagic = 0x444C5753u;   // 'SWLD'
constexpr std::uint32_t kSaveFormat = 21;            // v3: event.spawned, belief.self_evidence; v4: rng stream counters; v5: event failed/failure/conditions; v6: mind discovery records; v7: record failed_conditions + sample_entities; v8: entity genome; v9: social tie weights; v10: entity last_metabolized; v11: gen_cells_ + mind memory/visited_cells; v12: ledger archive state; v13: memory/discovery engrams (Ebbinghaus); v14: neuro state, panic_relief, born_memories; v15: recent_witnessed, sublimation_relief, ostracism_accum; v16: political_accum; v17: terrain.traffic_wear; v18: myth_accum; v19: ruleset; v20: CL-1 BrainStore; v21: Transition.w1h (dream replay)

struct Writer {
    std::ofstream f;
    template <typename T> void put(T v) {
        using U = std::make_unsigned_t<T>;
        U u = static_cast<U>(v);
        for (std::size_t i = 0; i < sizeof(T); ++i)
            f.put(static_cast<char>((u >> (i * 8)) & 0xFF));
    }
    void u8(std::uint8_t v) { put(v); }
    void boolean(bool b) { u8(b ? 1 : 0); }
    void str(const std::string& s) {
        put(static_cast<std::uint32_t>(s.size()));
        f.write(s.data(), static_cast<std::streamsize>(s.size()));
    }
    void value(const Value& v) {
        u8(static_cast<std::uint8_t>(v.type));
        put(v.i); boolean(v.b); str(v.s); put(v.target);
    }
    bool ok() const { return f.good(); }
};

struct Reader {
    std::ifstream f;
    template <typename T> bool get(T& v) {
        using U = std::make_unsigned_t<T>;
        U u = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            char c = 0;
            f.get(c);
            if (!f.good()) return false;
            u |= static_cast<U>(static_cast<unsigned char>(c)) << (i * 8);
        }
        v = static_cast<T>(u);
        return true;
    }
    bool str(std::string& s) {
        std::uint32_t n = 0;
        if (!get(n) || n > (64u << 20)) return false;
        s.resize(n);
        f.read(s.data(), n);
        return f.good();
    }
    bool value(Value& v) {
        std::uint8_t t = 0;
        if (!get(t)) return false;
        v.type = static_cast<ValType>(t);
        std::uint8_t b = 0;
        return get(v.i) && get(b) && str(v.s) && get(v.target) &&
               (v.b = b != 0, true);
    }
    bool ok() const { return f.good(); }
};

void write_i32vec(Writer& w, const std::vector<std::int32_t>& v) {
    w.put(static_cast<std::uint32_t>(v.size()));
    for (std::int32_t x : v) w.put(x);
}
void write_u8vec(Writer& w, const std::vector<std::uint8_t>& v) {
    w.put(static_cast<std::uint32_t>(v.size()));
    for (std::uint8_t x : v) w.u8(x);
}
bool read_i32vec(Reader& r, std::vector<std::int32_t>& v) {
    std::uint32_t n = 0;
    if (!r.get(n) || n > (1u << 28)) return false;
    v.resize(n);
    for (std::int32_t& x : v) if (!r.get(x)) return false;
    return true;
}
bool read_u8vec(Reader& r, std::vector<std::uint8_t>& v) {
    std::uint32_t n = 0;
    if (!r.get(n) || n > (1u << 28)) return false;
    v.resize(n);
    for (std::uint8_t& x : v) if (!r.get(x)) return false;
    return true;
}

}  // namespace

bool World::save(const std::string& path, std::string* err) const {
    Writer w{std::ofstream(path, std::ios::binary)};
    if (!w.f) { if (err) *err = "save: cannot open " + path; return false; }
    w.put(kSaveMagic); w.put(kSaveFormat);
    w.put(world_seed); w.put(generator_version); w.put(gen_origin_x); w.put(gen_origin_y);
    w.put(clock_.tick());
    // entities (creation order preserved -> kind ids and entity ids replay)
    w.put(static_cast<std::uint64_t>(entities.size()));
    for (const auto& e : entities) {
        w.str(e->kind_name);
        w.put(e->kind);
        w.str(e->material == kNoMaterial ? "" : materials.find(e->material)->name);
        w.str(e->deposit == kNoDeposit ? "" : deposits.find(e->deposit)->name);
        w.put(e->id); w.put(e->pos.x); w.put(e->pos.y);
        w.boolean(e->alive);
        w.put(static_cast<std::uint32_t>(e->props.entries.size()));
        for (const auto& [pid, v] : e->props.entries) {
            const PropertyDef* pd = props.find(pid);
            w.str(pd ? pd->name : "");
            w.value(v);
        }
        w.put(static_cast<std::uint64_t>(e->genome.loci.size()));
        for (const auto& [pid, v] : e->genome.loci) {
            const PropertyDef* pd = props.find(pid);
            w.str(pd ? pd->name : "");
            w.put(static_cast<std::int64_t>(v));
        }
        w.put(e->last_metabolized);
    }
    // terrain
    w.put(terrain.w); w.put(terrain.h); w.put(terrain.cell);
    w.put(terrain.ox); w.put(terrain.oy);
    write_i32vec(w, terrain.elevation); write_i32vec(w, terrain.moisture);
    write_i32vec(w, terrain.temperature); write_i32vec(w, terrain.vegetation);
    write_i32vec(w, terrain.slope); write_i32vec(w, terrain.wind_x);
    write_i32vec(w, terrain.wind_y); write_i32vec(w, terrain.water_table);
    write_i32vec(w, terrain.traffic_wear);
    write_u8vec(w, terrain.water);
    // subsurface
    w.put(subsurface.w); w.put(subsurface.h); w.put(subsurface.ox); w.put(subsurface.oy);
    w.put(subsurface.max_depth);
    w.put(static_cast<std::uint32_t>(subsurface.cells.size()));
    for (const auto& col : subsurface.cells) {
        w.put(static_cast<std::uint32_t>(col.size()));
        for (const Stratum& s : col) {
            w.str(s.material == kNoMaterial ? "" : materials.find(s.material)->name);
            w.put(s.top); w.put(s.bottom);
        }
    }
    // worldgen spawn claims (region-regeneration dedup survives save/load)
    w.put(static_cast<std::uint64_t>(gen_spawned_.size()));
    for (const auto& [tag, gx, gy] : gen_spawned_) {
        w.put(tag); w.put(gx); w.put(gy);
    }
    // materialized-cell bookkeeping (lazy region generation, v11)
    w.put(static_cast<std::uint64_t>(gen_cells_.size()));
    for (const auto& [gx, gy] : gen_cells_) { w.put(gx); w.put(gy); }
    // relations
    w.put(static_cast<std::uint64_t>(relations.edges().size()));
    for (const Relation& r : relations.edges()) { w.put(r.a); w.str(r.type); w.put(r.b); }
    // social tie weights (Phase 8)
    w.put(static_cast<std::uint64_t>(social_.size()));
    for (const auto& [rel, wt] : social_) {
        w.put(rel.a); w.str(rel.type); w.put(rel.b); w.put(static_cast<std::int64_t>(wt));
    }
    // causal ledger — archive state first so a windowed ledger restores
    // its id sequence and full-history digest exactly (v12)
    w.put(ledger.archived_hash());
    w.put(static_cast<std::uint64_t>(
        ledger.next_id() - static_cast<EventId>(ledger.history().size())));
    w.put(static_cast<std::uint64_t>(ledger.history().size()));
    for (const CausalEvent& ev : ledger.history()) {
        w.put(ev.id); w.put(ev.tick);
        w.put(static_cast<std::uint32_t>(ev.participants.size()));
        for (EntityId x : ev.participants) w.put(x);
        w.put(static_cast<std::uint32_t>(ev.parent_events.size()));
        for (EventId x : ev.parent_events) w.put(x);
        w.put(static_cast<std::uint32_t>(ev.applied_laws.size()));
        for (LawId x : ev.applied_laws) w.put(x);
        w.str(ev.action);
        w.str(ev.evidence);
        w.put(static_cast<std::uint32_t>(ev.changes.size()));
        for (const StateChange& c : ev.changes) {
            w.put(c.entity);
            const PropertyDef* pd = props.find(c.prop);
            w.str(pd ? pd->name : "");
            w.boolean(c.had_before);
            w.value(c.before); w.value(c.after);
        }
        w.put(static_cast<std::uint32_t>(ev.spawned.size()));
        for (EntityId s : ev.spawned) w.put(s);
        w.boolean(ev.failed); w.str(ev.failure);
        w.put(static_cast<std::uint32_t>(ev.conditions.size()));
        for (const Condition& c : ev.conditions) {
            w.str(c.what); w.put(static_cast<std::uint64_t>(c.actual));
            w.put(static_cast<std::uint64_t>(c.required)); w.boolean(c.met);
        }
    }
    // agent minds (knowledge state)
    w.put(static_cast<std::uint64_t>(minds_.size()));
    for (const auto& [nid, m] : minds_) {
        w.put(nid);
        w.put(static_cast<std::uint32_t>(m.observations.size()));
        for (const Observation& o : m.observations) {
            w.put(o.entity); w.put(o.tick);
            w.put(static_cast<std::uint32_t>(o.pos.x)); w.put(static_cast<std::uint32_t>(o.pos.y));
            w.put(static_cast<std::uint32_t>(o.props.size()));
            for (const auto& [pp, v] : o.props) { w.put(pp); w.value(v); }
        }
        w.put(static_cast<std::uint32_t>(m.beliefs.size()));
        for (const auto& [k, b] : m.beliefs) {
            w.str(k); w.put(static_cast<std::uint8_t>(b.status));
            w.put(static_cast<std::uint64_t>(b.confidence));
            w.put(static_cast<std::uint64_t>(b.evidence_for));
            w.put(static_cast<std::uint64_t>(b.evidence_against));
            w.put(b.last_tick);
            w.put(static_cast<std::uint64_t>(b.self_evidence));
        }
        w.put(static_cast<std::uint32_t>(m.known_actions.size()));
        for (const std::string& a : m.known_actions) w.str(a);
        w.put(static_cast<std::uint32_t>(m.known_entities.size()));
        for (EntityId x : m.known_entities) w.put(x);
        w.put(static_cast<std::uint32_t>(m.discoveries.size()));
        for (const auto& [k, d] : m.discoveries) {
            w.str(k); w.str(d.claim);
            w.put(static_cast<std::uint32_t>(d.experiments.size()));
            for (EventId e : d.experiments) w.put(e);
            w.put(static_cast<std::uint32_t>(d.supporting_laws.size()));
            for (const std::string& s : d.supporting_laws) w.str(s);
            w.put(static_cast<std::uint32_t>(d.failed_conditions.size()));
            for (const Condition& c : d.failed_conditions) {
                w.str(c.what);
                w.put(static_cast<std::uint64_t>(c.actual));
                w.put(static_cast<std::uint64_t>(c.required));
                w.boolean(c.met);
            }
            w.put(static_cast<std::uint32_t>(d.sample_entities.size()));
            for (EntityId x : d.sample_entities) w.put(x);
            w.put(static_cast<std::uint64_t>(d.evidence_for));
            w.put(static_cast<std::uint64_t>(d.evidence_against));
            w.put(d.trials);
            w.put(static_cast<std::uint8_t>(d.status));
            w.put(d.last_tick);
            w.put(static_cast<std::uint64_t>(d.engram.clarity));
            w.put(static_cast<std::uint64_t>(d.engram.stability));
            w.put(static_cast<std::uint64_t>(d.engram.emotional_charge));
            w.put(static_cast<std::uint64_t>(d.engram.rehearsal_count));
            w.put(static_cast<std::int64_t>(d.engram.decay_accum));
            w.put(d.engram.last_rehearsal);
        }
        // fog-of-war memory + visited cells (v11)
        w.put(static_cast<std::uint32_t>(m.memory.size()));
        for (const auto& [eid, s] : m.memory) {
            w.put(eid); w.put(s.tick); w.put(s.material);
            w.put(static_cast<std::uint32_t>(s.pos.x));
            w.put(static_cast<std::uint32_t>(s.pos.y));
            w.str(s.kind_name);
            w.put(static_cast<std::uint32_t>(s.props.size()));
            for (const auto& [pp, v] : s.props) { w.put(pp); w.value(v); }
            w.put(static_cast<std::uint64_t>(s.engram.clarity));
            w.put(static_cast<std::uint64_t>(s.engram.stability));
            w.put(static_cast<std::uint64_t>(s.engram.emotional_charge));
            w.put(static_cast<std::uint64_t>(s.engram.rehearsal_count));
            w.put(static_cast<std::int64_t>(s.engram.decay_accum));
            w.put(s.engram.last_rehearsal);
        }
        w.put(static_cast<std::uint32_t>(m.visited_cells.size()));
        for (const auto& [gx, gy] : m.visited_cells) { w.put(gx); w.put(gy); }
        // Nhịp 2-3 neuro state (v14)
        const NeuroState& ns = m.neuro;
        w.boolean(ns.tuned);
        w.put(static_cast<std::uint64_t>(ns.threat_gain));
        w.put(static_cast<std::uint64_t>(ns.reward_gain));
        w.put(static_cast<std::uint64_t>(ns.pattern_coupling));
        w.put(static_cast<std::uint64_t>(ns.empathy_coupling));
        w.put(static_cast<std::uint64_t>(ns.low_light_bias));
        w.put(static_cast<std::uint64_t>(ns.isolation_bias));
        w.put(static_cast<std::uint64_t>(ns.executive_override));
        w.put(static_cast<std::uint64_t>(ns.panic));
        w.put(static_cast<std::uint64_t>(ns.rage));
        w.put(static_cast<std::uint64_t>(ns.grief));
        w.put(static_cast<std::uint64_t>(ns.panic_at_event));
        w.put(ns.last_event_tick);
        w.put(static_cast<std::uint32_t>(m.panic_relief.size()));
        for (const auto& [act, cnt] : m.panic_relief) {
            w.str(act); w.put(static_cast<std::uint64_t>(cnt));
        }
        w.put(static_cast<std::uint32_t>(m.born_memories.size()));
        for (const BornEngram& bm : m.born_memories) {
            w.put(bm.material); w.str(bm.kind_name);
            w.put(static_cast<std::uint64_t>(bm.engram.clarity));
            w.put(static_cast<std::uint64_t>(bm.engram.stability));
            w.put(static_cast<std::uint64_t>(bm.engram.emotional_charge));
            w.put(static_cast<std::uint64_t>(bm.engram.rehearsal_count));
            w.put(static_cast<std::int64_t>(bm.engram.decay_accum));
            w.put(bm.engram.last_rehearsal);
        }
        // v15: herd memory + sublimation relief + ostracism accumulator
        w.put(static_cast<std::uint32_t>(m.recent_witnessed.size()));
        for (const WitnessedAct& wa : m.recent_witnessed) {
            w.put(wa.tick); w.put(wa.actor); w.str(wa.action);
        }
        w.put(static_cast<std::uint32_t>(m.sublimation_relief.size()));
        for (const auto& [act, cnt] : m.sublimation_relief) {
            w.str(act); w.put(static_cast<std::uint64_t>(cnt));
        }
        w.put(static_cast<std::int64_t>(m.ostracism_accum));
        w.put(static_cast<std::int64_t>(m.political_accum));
        w.put(static_cast<std::int64_t>(m.myth_accum));
    }
    // rng stream counters; v5: event failed/failure/conditions (v4): stochastic laws must replay after load
    w.put(seed);
    w.put(gen_sea_level);
    w.put(static_cast<std::uint32_t>(rngs_.size()));
    for (const auto& [name, rg] : rngs_) { w.str(name); w.put(rg->state()); }
    w.put(ruleset);                                   // v19
    // v20: CL-1 brain store (arena bookkeeping + live slots)
    w.boolean(brains_ != nullptr);
    if (brains_) {
        BrainStore& bs = *brains_;
        w.put(bs.arena.capacity_cells());
        w.put(bs.arena.used_cells());
        w.put(bs.arena.live_bytes());
        const auto& fl = bs.arena.free_lists();
        w.put(static_cast<std::uint32_t>(fl.size()));
        for (const auto& v : fl) {
            w.put(static_cast<std::uint32_t>(v.size()));
            for (std::uint64_t x : v) w.put(x);
        }
        w.put(static_cast<std::uint64_t>(bs.slots.size()));
        for (const auto& [nid, s] : bs.slots) {
            w.put(nid);
            w.put(s.hidden); w.put(s.lr_permille);
            w.put(s.trace_decay); w.put(s.mut_scale);
            w.put(s.woff); w.put(s.wbytes);
            w.str(std::string(reinterpret_cast<const char*>(bs.arena.w8(s.woff)),
                              static_cast<std::size_t>(s.wbytes)));
            w.str(std::string(reinterpret_cast<const char*>(s.h.data()),
                              s.h.size() * sizeof(std::int16_t)));
            w.str(std::string(reinterpret_cast<const char*>(s.last_action.data()),
                              s.last_action.size()));
            w.put(s.replay_next); w.put(s.replay_count);
            w.put(s.mac_pending); w.put(s.mac_frac);
            w.str(std::string(reinterpret_cast<const char*>(s.fast.data()),
                              s.fast.size()));
            w.str(std::string(reinterpret_cast<const char*>(s.anchor.data()),
                              s.anchor.size()));
            for (const Transition& t : s.replay) {
                w.boolean(t.valid); w.put(t.event_id);
                w.put(t.sigma); w.put(t.charge);
                w.str(std::string(reinterpret_cast<const char*>(t.s.data()), t.s.size()));
                w.str(std::string(reinterpret_cast<const char*>(t.a.data()), t.a.size()));
                w.str(std::string(reinterpret_cast<const char*>(t.y.data()), t.y.size()));
                w.str(std::string(reinterpret_cast<const char*>(t.wh.data()), t.wh.size()));
                w.str(std::string(reinterpret_cast<const char*>(t.w1h.data()), t.w1h.size()));
            }
        }
        w.put(bs.wm_err_sum); w.put(bs.wm_n);
        w.put(bs.sigma_sum); w.put(bs.sigma_n);
        w.put(bs.surprise_events);
    }
    w.f.close();
    if (!w.ok()) { if (err) *err = "save: write failed"; return false; }
    return true;
}

bool World::load(const std::string& path, std::string* err) {
    auto fail = [&](const std::string& m) { if (err) *err = "load: " + m; return false; };
    Reader r{std::ifstream(path, std::ios::binary)};
    if (!r.f) return fail("cannot open " + path);
    std::uint32_t magic = 0, fmt = 0;
    if (!r.get(magic) || !r.get(fmt)) return fail("bad header");
    if (magic != kSaveMagic) return fail("not a Sunnyworld save");
    if (fmt > kSaveFormat) return fail("save format too new for this build");
    if (fmt < 2) return fail("legacy save format unsupported (regenerate world)");

    std::uint64_t wseed = 0, etick = 0;
    int gver = 0, ox = 0, oy = 0;
    if (!r.get(wseed) || !r.get(gver) || !r.get(ox) || !r.get(oy) || !r.get(etick))
        return fail("truncated header");
    if (gver > kGeneratorVersion) return fail("generator_version too new for this build");

    // reset world truth (content registries stay — names must resolve)
    entities.clear(); index_.clear(); by_kind_.clear(); touched_.clear(); spatial.clear(); relations.clear(); social_.clear(); ledger.clear(); minds_.clear(); brains_.reset();
    rngs_.clear();
    gen_spawned_.clear();
    kinds = EntityRegistry{};
    clock_.tick_ = etick;
    world_seed = wseed; generator_version = gver;
    gen_origin_x = ox; gen_origin_y = oy;

    std::uint64_t ec = 0;
    if (!r.get(ec) || ec > (1ull << 32)) return fail("bad entity count");
    for (std::uint64_t i = 0; i < ec; ++i) {
        std::string kname, mname, dname, pname;
        std::uint32_t kind = 0; EntityId id = 0;
        std::int32_t px = 0, py = 0; std::uint8_t alive = 0; std::uint32_t pc = 0;
        if (!r.str(kname) || !r.get(kind) || !r.str(mname) || !r.str(dname) ||
            !r.get(id) || !r.get(px) || !r.get(py) || !r.get(alive) || !r.get(pc) ||
            pc > (1u << 20)) return fail("truncated entity");
        auto e = std::make_unique<Entity>();
        const std::uint32_t kid = kinds.kind_id(kname);
        const EntityId eid = kinds.allocate(kid);
        if (eid != id) return fail("entity id mismatch (kind order differs)");
        e->id = id; e->kind = kind; e->kind_name = kname;
        e->material = mname.empty() ? kNoMaterial : materials.id_of(mname);
        e->deposit = dname.empty() ? kNoDeposit : deposits.id_of(dname);
        if ((!mname.empty() && e->material == kNoMaterial) ||
            (!dname.empty() && e->deposit == kNoDeposit))
            return fail("unknown material/deposit in save: " + mname + "/" + dname);
        e->pos = {px, py};
        e->alive = alive != 0;
        for (std::uint32_t j = 0; j < pc; ++j) {
            if (!r.str(pname)) return fail("truncated prop name");
            Value v;
            if (!r.value(v)) return fail("truncated prop value");
            const PropertyDef* pd = props.find_name(pname);
            if (!pd) return fail("unknown property in save: " + pname);
            e->props.entries.emplace_back(pd->id, v);
        }
        std::uint64_t gc2 = 0;
        if (!r.get(gc2) || gc2 > (1u << 20)) return fail("truncated genome");
        for (std::uint64_t j = 0; j < gc2; ++j) {
            std::string gname; std::int64_t gv = 0;
            if (!r.str(gname) || !r.get(gv)) return fail("truncated genome locus");
            const PropertyDef* gd = props.find_name(gname);
            if (!gd) return fail("unknown genome property in save: " + gname);
            e->genome.loci.emplace_back(gd->id, gv);
        }
        if (!r.get(e->last_metabolized)) return fail("truncated entity tick");
        index_[id] = entities.size();
        by_kind_[e->kind_name.interned()].push_back(id);
        entities.push_back(std::move(e));
        spatial.insert(id, {px, py});
    }

    int tw = 0, th = 0, tcell = 0, tox = 0, toy = 0;
    if (!r.get(tw) || !r.get(th) || !r.get(tcell) || !r.get(tox) || !r.get(toy))
        return fail("truncated terrain header");
    terrain.w = tw; terrain.h = th; terrain.cell = tcell;
    terrain.ox = tox; terrain.oy = toy;
    if (!read_i32vec(r, terrain.elevation) || !read_i32vec(r, terrain.moisture) ||
        !read_i32vec(r, terrain.temperature) || !read_i32vec(r, terrain.vegetation) ||
        !read_i32vec(r, terrain.slope) || !read_i32vec(r, terrain.wind_x) ||
        !read_i32vec(r, terrain.wind_y) || !read_i32vec(r, terrain.water_table) ||
        !read_i32vec(r, terrain.traffic_wear) ||
        !read_u8vec(r, terrain.water)) return fail("truncated terrain");

    int sw = 0, sh = 0, sox = 0, soy = 0, sdepth = 0; std::uint32_t scc = 0;
    if (!r.get(sw) || !r.get(sh) || !r.get(sox) || !r.get(soy) ||
        !r.get(sdepth) || !r.get(scc) || scc > (1u << 28))
        return fail("truncated subsurface header");
    subsurface.w = sw; subsurface.h = sh; subsurface.ox = sox; subsurface.oy = soy;
    subsurface.max_depth = sdepth;
    subsurface.cells.clear();
    subsurface.cells.reserve(scc);
    for (std::uint32_t i = 0; i < scc; ++i) {
        std::uint32_t sc = 0;
        if (!r.get(sc) || sc > (1u << 16)) return fail("truncated strata");
        std::vector<Stratum> col;
        col.reserve(sc);
        for (std::uint32_t j = 0; j < sc; ++j) {
            Stratum s; std::string mname;
            if (!r.str(mname) || !r.get(s.top) || !r.get(s.bottom))
                return fail("truncated stratum");
            s.material = mname.empty() ? kNoMaterial : materials.id_of(mname);
            if (!mname.empty() && s.material == kNoMaterial)
                return fail("unknown stratum material: " + mname);
            col.push_back(s);
        }
        subsurface.cells.push_back(std::move(col));
    }

    std::uint64_t gc = 0;
    if (!r.get(gc) || gc > (1ull << 32)) return fail("bad spawn-claim count");
    for (std::uint64_t i = 0; i < gc; ++i) {
        std::uint32_t tag = 0; int gx = 0, gy = 0;
        if (!r.get(tag) || !r.get(gx) || !r.get(gy)) return fail("truncated spawn claim");
        gen_spawned_.emplace(tag, gx, gy);
    }
    if (fmt >= 11) {
        std::uint64_t mc = 0;
        if (!r.get(mc) || mc > (1ull << 32)) return fail("bad gen-cells count");
        for (std::uint64_t i = 0; i < mc; ++i) {
            int gx = 0, gy = 0;
            if (!r.get(gx) || !r.get(gy)) return fail("truncated gen cell");
            gen_cells_.emplace(gx, gy);
        }
    }

    std::uint64_t rc = 0;
    if (!r.get(rc) || rc > (1ull << 32)) return fail("bad relation count");
    for (std::uint64_t i = 0; i < rc; ++i) {
        EntityId a = 0, b = 0; std::string ty;
        if (!r.get(a) || !r.str(ty) || !r.get(b)) return fail("truncated relation");
        relations.relate(a, ty, b);
    }

    std::uint64_t sc = 0;
    if (!r.get(sc) || sc > (1u << 20)) return fail("bad social-tie count");
    for (std::uint64_t i = 0; i < sc; ++i) {
        EntityId sa = 0, sb = 0; std::string sty; std::int64_t sw = 0;
        if (!r.get(sa) || !r.str(sty) || !r.get(sb) || !r.get(sw))
            return fail("truncated social tie");
        social_[{sa, sty, sb}] = sw;
    }

    if (fmt >= 12) {
        std::uint64_t ah = 0, first_id = 1;
        if (!r.get(ah) || !r.get(first_id)) return fail("bad ledger archive");
        ledger.set_archive(ah, static_cast<EventId>(first_id));
    }
    std::uint64_t lc = 0;
    if (!r.get(lc) || lc > (1ull << 32)) return fail("bad ledger count");
    for (std::uint64_t i = 0; i < lc; ++i) {
        CausalEvent ev; std::uint64_t evid = 0; std::uint32_t n = 0;
        if (!r.get(evid) || !r.get(ev.tick)) return fail("truncated event");
        ev.id = evid;
        if (!r.get(n) || n > (1u << 20)) return fail("truncated participants");
        for (std::uint32_t j = 0; j < n; ++j) { EntityId x; if (!r.get(x)) return fail("truncated participants"); ev.participants.push_back(x); }
        if (!r.get(n) || n > (1u << 20)) return fail("truncated parents");
        for (std::uint32_t j = 0; j < n; ++j) { EventId x; if (!r.get(x)) return fail("truncated parents"); ev.parent_events.push_back(x); }
        if (!r.get(n) || n > (1u << 20)) return fail("truncated laws");
        for (std::uint32_t j = 0; j < n; ++j) { LawId x; if (!r.get(x)) return fail("truncated laws"); ev.applied_laws.push_back(x); }
        if (!r.str(ev.action) || !r.str(ev.evidence)) return fail("truncated evidence");
        if (!r.get(n) || n > (1u << 20)) return fail("truncated changes");
        for (std::uint32_t j = 0; j < n; ++j) {
            StateChange c; std::uint8_t hb = 0; std::string pname;
            if (!r.get(c.entity) || !r.str(pname) || !r.get(hb) ||
                !r.value(c.before) || !r.value(c.after)) return fail("truncated change");
            const PropertyDef* pd = props.find_name(pname);
            if (!pd) return fail("unknown property in save: " + pname);
            c.prop = pd->id;
            c.had_before = hb != 0;
            ev.changes.push_back(c);
        }
        if (fmt >= 3) {
            std::uint32_t sn = 0;
            if (!r.get(sn) || sn > (1u << 20)) return fail("truncated spawned");
            for (std::uint32_t j = 0; j < sn; ++j) {
                EntityId s = 0;
                if (!r.get(s)) return fail("truncated spawned id");
                ev.spawned.push_back(s);
            }
        }
        if (fmt >= 5) {
            std::uint8_t fl = 0; std::uint32_t cn = 0;
            if (!r.get(fl) || !r.str(ev.failure)) return fail("truncated failure");
            ev.failed = fl != 0;
            if (!r.get(cn) || cn > (1u << 16)) return fail("truncated conditions");
            for (std::uint32_t j = 0; j < cn; ++j) {
                Condition c; std::uint64_t a = 0, rq = 0; std::uint8_t mt = 0;
                if (!r.str(c.what) || !r.get(a) || !r.get(rq) || !r.get(mt)) return fail("truncated condition");
                c.actual = static_cast<std::int64_t>(a); c.required = static_cast<std::int64_t>(rq); c.met = mt != 0;
                ev.conditions.push_back(std::move(c));
            }
        }
        ledger.append(std::move(ev));
        note_touched(ledger.history().back());
    }
    // agent minds
    {
        std::uint64_t mn = 0;
        if (!r.get(mn) || mn > (1u << 20)) return fail("bad minds count");
        for (std::uint64_t i = 0; i < mn; ++i) {
            EntityId nid = 0; AgentMind m;
            if (!r.get(nid)) return fail("truncated mind");
            std::uint32_t n = 0;
            if (!r.get(n) || n > (1u << 20)) return fail("bad observations");
            for (std::uint32_t j = 0; j < n; ++j) {
                Observation o; std::uint32_t np = 0;
                if (!r.get(o.entity) || !r.get(o.tick)) return fail("truncated obs");
                std::uint32_t px = 0, py = 0;
                if (!r.get(px) || !r.get(py)) return fail("truncated obs pos");
                o.pos = {static_cast<std::int32_t>(px), static_cast<std::int32_t>(py)};
                if (!r.get(np) || np > (1u << 16)) return fail("bad obs props");
                for (std::uint32_t k = 0; k < np; ++k) {
                    PropId pp = 0; Value v;
                    if (!r.get(pp) || !r.value(v)) return fail("truncated obs prop");
                    const PropertyDef* pd = props.find(pp);
                    if (!pd) return fail("unknown obs prop id");
                    o.props[pd->id] = v;
                }
                m.observations.push_back(o);
            }
            if (!r.get(n) || n > (1u << 20)) return fail("bad beliefs");
            for (std::uint32_t j = 0; j < n; ++j) {
                Belief b; std::string k; std::uint8_t st = 0;
                if (!r.str(k) || !r.get(st)) return fail("truncated belief");
                b.key = k; b.status = static_cast<BeliefStatus>(st);
                if (!r.get(reinterpret_cast<std::uint64_t&>(b.confidence)) ||
                    !r.get(reinterpret_cast<std::uint64_t&>(b.evidence_for)) ||
                    !r.get(reinterpret_cast<std::uint64_t&>(b.evidence_against)) ||
                    !r.get(b.last_tick)) return fail("truncated belief vals");
                if (fmt >= 3 &&
                    !r.get(reinterpret_cast<std::uint64_t&>(b.self_evidence)))
                    return fail("truncated belief self_evidence");
                m.beliefs[k] = b;
            }
            if (!r.get(n) || n > (1u << 20)) return fail("bad actions");
            for (std::uint32_t j = 0; j < n; ++j) { std::string a; if (!r.str(a)) return fail("truncated action"); m.known_actions.insert(a); }
            if (!r.get(n) || n > (1u << 20)) return fail("bad known entities");
            for (std::uint32_t j = 0; j < n; ++j) { EntityId x; if (!r.get(x)) return fail("truncated known entity"); m.known_entities.insert(x); }
            if (fmt >= 6) {
                if (!r.get(n) || n > (1u << 16)) return fail("bad discoveries");
                for (std::uint32_t j = 0; j < n; ++j) {
                    DiscoveryRecord d; std::string k;
                    std::uint8_t st = 0; std::uint32_t en = 0, ln = 0;
                    if (!r.str(k) || !r.str(d.claim)) return fail("truncated discovery");
                    d.key = k;
                    if (!r.get(en) || en > (1u << 20)) return fail("bad discovery experiments");
                    for (std::uint32_t q = 0; q < en; ++q) {
                        EventId e = 0; if (!r.get(e)) return fail("truncated discovery event");
                        d.experiments.push_back(e);
                    }
                    if (!r.get(ln) || ln > (1u << 16)) return fail("bad discovery laws");
                    for (std::uint32_t q = 0; q < ln; ++q) {
                        std::string s; if (!r.str(s)) return fail("truncated discovery law");
                        d.supporting_laws.push_back(s);
                    }
                    if (fmt >= 7) {
                        std::uint32_t cn = 0, sn = 0;
                        if (!r.get(cn) || cn > (1u << 16)) return fail("bad discovery conditions");
                        for (std::uint32_t q = 0; q < cn; ++q) {
                            Condition c; std::uint64_t a = 0, rq = 0; std::uint8_t mt = 0;
                            if (!r.str(c.what) || !r.get(a) || !r.get(rq) || !r.get(mt))
                                return fail("truncated discovery condition");
                            c.actual = static_cast<std::int64_t>(a);
                            c.required = static_cast<std::int64_t>(rq);
                            c.met = mt != 0;
                            d.failed_conditions.push_back(c);
                        }
                        if (!r.get(sn) || sn > (1u << 16)) return fail("bad discovery samples");
                        for (std::uint32_t q = 0; q < sn; ++q) {
                            EntityId x; if (!r.get(x)) return fail("truncated discovery sample");
                            d.sample_entities.push_back(x);
                        }
                    }
                    std::uint64_t evf = 0, eva = 0, tr = 0, lt = 0;
                    if (!r.get(evf) || !r.get(eva) || !r.get(tr) ||
                        !r.get(st) || !r.get(lt)) return fail("truncated discovery vals");
                    d.evidence_for = static_cast<std::int64_t>(evf);
                    d.evidence_against = static_cast<std::int64_t>(eva);
                    d.trials = tr;
                    d.status = static_cast<DiscoveryStatus>(st);
                    d.last_tick = lt;
                    if (fmt >= 13) {
                        std::uint64_t ec = 0, es = 0, eq = 0, er = 0, elr = 0;
                        std::int64_t ea = 0;
                        if (!r.get(ec) || !r.get(es) || !r.get(eq) || !r.get(er) ||
                            !r.get(ea) || !r.get(elr))
                            return fail("truncated discovery engram");
                        d.engram.clarity = static_cast<std::int64_t>(ec);
                        d.engram.stability = static_cast<std::int64_t>(es);
                        d.engram.emotional_charge = static_cast<std::int64_t>(eq);
                        d.engram.rehearsal_count = static_cast<std::int64_t>(er);
                        d.engram.decay_accum = static_cast<std::int32_t>(ea);
                        d.engram.last_rehearsal = elr;
                    } else {
                        // pre-engram save: migrate as an ordinary memory trace
                        const MindParams mp;
                        d.engram.clarity = mp.base_clarity;
                        d.engram.stability = mp.base_stability;
                    }
                    m.discoveries[k] = d;
                }
            }
            if (fmt >= 11) {
                if (!r.get(n) || n > (1u << 20)) return fail("bad memory");
                for (std::uint32_t j = 0; j < n; ++j) {
                    AgentMind::MemorySnapshot s; EntityId eid = 0;
                    std::uint32_t px = 0, py = 0, np = 0;
                    if (!r.get(eid) || !r.get(s.tick) || !r.get(s.material) ||
                        !r.get(px) || !r.get(py) || !r.str(s.kind_name) ||
                        !r.get(np) || np > (1u << 16)) return fail("truncated memory");
                    s.pos = {static_cast<std::int32_t>(px), static_cast<std::int32_t>(py)};
                    for (std::uint32_t q = 0; q < np; ++q) {
                        PropId pp = 0; Value v;
                        if (!r.get(pp) || !r.value(v)) return fail("truncated mem prop");
                        const PropertyDef* pd = props.find(pp);
                        if (!pd) return fail("unknown mem prop id");
                        s.props[pd->id] = v;
                    }
                    if (fmt >= 13) {
                        std::uint64_t ec = 0, es = 0, eq = 0, er = 0, elr = 0;
                        std::int64_t ea = 0;
                        if (!r.get(ec) || !r.get(es) || !r.get(eq) || !r.get(er) ||
                            !r.get(ea) || !r.get(elr))
                            return fail("truncated memory engram");
                        s.engram.clarity = static_cast<std::int64_t>(ec);
                        s.engram.stability = static_cast<std::int64_t>(es);
                        s.engram.emotional_charge = static_cast<std::int64_t>(eq);
                        s.engram.rehearsal_count = static_cast<std::int64_t>(er);
                        s.engram.decay_accum = static_cast<std::int32_t>(ea);
                        s.engram.last_rehearsal = elr;
                    } else {
                        const MindParams mp;
                        s.engram.clarity = mp.base_clarity;
                        s.engram.stability = mp.base_stability;
                    }
                    m.memory[eid] = std::move(s);
                }
                if (!r.get(n) || n > (1u << 20)) return fail("bad visited cells");
                for (std::uint32_t j = 0; j < n; ++j) {
                    int gx = 0, gy = 0;
                    if (!r.get(gx) || !r.get(gy)) return fail("truncated visited cell");
                    m.visited_cells.emplace(gx, gy);
                }
            }
            if (fmt >= 14) {
                // Nhịp 2-3 neuro state
                std::uint8_t tuned = 0;
                std::uint64_t v[12] = {};
                if (!r.get(tuned)) return fail("truncated neuro tuned");
                for (int i = 0; i < 12; ++i)
                    if (!r.get(v[i])) return fail("truncated neuro axes");
                m.neuro.tuned = tuned != 0;
                m.neuro.threat_gain = static_cast<std::int64_t>(v[0]);
                m.neuro.reward_gain = static_cast<std::int64_t>(v[1]);
                m.neuro.pattern_coupling = static_cast<std::int64_t>(v[2]);
                m.neuro.empathy_coupling = static_cast<std::int64_t>(v[3]);
                m.neuro.low_light_bias = static_cast<std::int64_t>(v[4]);
                m.neuro.isolation_bias = static_cast<std::int64_t>(v[5]);
                m.neuro.executive_override = static_cast<std::int64_t>(v[6]);
                m.neuro.panic = static_cast<std::int64_t>(v[7]);
                m.neuro.rage = static_cast<std::int64_t>(v[8]);
                m.neuro.grief = static_cast<std::int64_t>(v[9]);
                m.neuro.panic_at_event = static_cast<std::int64_t>(v[10]);
                m.neuro.last_event_tick = v[11];
                std::uint32_t rn = 0;
                if (!r.get(rn) || rn > (1u << 16)) return fail("bad panic_relief");
                for (std::uint32_t j = 0; j < rn; ++j) {
                    std::string act; std::uint64_t cnt = 0;
                    if (!r.str(act) || !r.get(cnt))
                        return fail("truncated panic_relief");
                    m.panic_relief[act] = static_cast<std::int64_t>(cnt);
                }
                std::uint32_t bn = 0;
                if (!r.get(bn) || bn > (1u << 16)) return fail("bad born_memories");
                for (std::uint32_t j = 0; j < bn; ++j) {
                    BornEngram bm;
                    std::uint64_t ec = 0, es = 0, eq = 0, er = 0, elr = 0;
                    std::int64_t ea = 0;
                    if (!r.get(bm.material) || !r.str(bm.kind_name) ||
                        !r.get(ec) || !r.get(es) || !r.get(eq) || !r.get(er) ||
                        !r.get(ea) || !r.get(elr))
                        return fail("truncated born_memory");
                    bm.engram.clarity = static_cast<std::int64_t>(ec);
                    bm.engram.stability = static_cast<std::int64_t>(es);
                    bm.engram.emotional_charge = static_cast<std::int64_t>(eq);
                    bm.engram.rehearsal_count = static_cast<std::int64_t>(er);
                    bm.engram.decay_accum = static_cast<std::int32_t>(ea);
                    bm.engram.last_rehearsal = elr;
                    m.born_memories.push_back(std::move(bm));
                }
            }
            if (fmt >= 15) {
                std::uint32_t wn = 0;
                if (!r.get(wn) || wn > 64) return fail("bad witnessed");
                for (std::uint32_t j = 0; j < wn; ++j) {
                    WitnessedAct wa; std::uint64_t tk = 0;
                    if (!r.get(tk) || !r.get(wa.actor) ||
                        !r.str(wa.action))
                        return fail("truncated witnessed");
                    wa.tick = tk;
                    m.recent_witnessed.push_back(std::move(wa));
                }
                std::uint32_t sn = 0;
                if (!r.get(sn) || sn > (1u << 16))
                    return fail("bad sublimation_relief");
                for (std::uint32_t j = 0; j < sn; ++j) {
                    std::string act; std::uint64_t cnt = 0;
                    if (!r.str(act) || !r.get(cnt))
                        return fail("truncated sublimation_relief");
                    m.sublimation_relief[act] =
                        static_cast<std::int64_t>(cnt);
                }
                std::int64_t oa = 0;
                if (!r.get(oa)) return fail("truncated ostracism_accum");
                m.ostracism_accum = oa;
                std::int64_t pa2 = 0;
                if (!r.get(pa2)) return fail("truncated political_accum");
                m.political_accum = pa2;
                if (fmt >= 18) {
                    std::int64_t ma2 = 0;
                    if (!r.get(ma2)) return fail("truncated myth_accum");
                    m.myth_accum = ma2;
                }
            }
            minds_[nid] = std::move(m);
        }
        // Phase 12.4: rebuild the wear dirty-set from terrain truth —
        // deterministic scan, no extra serialized state needed.
        wear_cells_.clear();
        if (!terrain.traffic_wear.empty()) {
            for (int gy = 0; gy < terrain.h; ++gy)
                for (int gx = 0; gx < terrain.w; ++gx) {
                    const std::size_t ci =
                        static_cast<std::size_t>(gy) * terrain.w + gx;
                    if (terrain.traffic_wear[ci] > 0)
                        wear_cells_.insert(
                            (static_cast<std::uint64_t>(
                                 static_cast<std::uint32_t>(
                                     gx + terrain.ox)) << 32) |
                            static_cast<std::uint32_t>(gy + terrain.oy));
                }
        }
    }
    if (fmt >= 4) {
        std::uint64_t sd = 0;
        if (!r.get(sd)) return fail("truncated seed");
        seed = sd;                       // rng streams derive from it
        if (!r.get(gen_sea_level)) return fail("truncated sea level");
        std::uint32_t rc = 0;
        if (!r.get(rc) || rc > (1u << 16)) return fail("bad rng count");
        for (std::uint32_t i = 0; i < rc; ++i) {
            std::string name; std::uint64_t ctr = 0;
            if (!r.str(name) || !r.get(ctr)) return fail("truncated rng state");
            rng(name).set_state(ctr);
        }
    }
    ruleset = 0;
    if (fmt >= 19 && !r.get(ruleset)) return fail("truncated ruleset");
    // v20: CL-1 brain store
    if (fmt >= 20) {
        std::uint8_t has_brains = 0;
        if (!r.get(has_brains)) return fail("truncated brain flag");
        if (has_brains) {
            auto bs = std::make_shared<BrainStore>();
            std::uint64_t cells = 0, hw = 0, live = 0;
            if (!r.get(cells) || !r.get(hw) || !r.get(live))
                return fail("truncated brain arena");
            std::uint32_t nfl = 0;
            if (!r.get(nfl) || nfl > (1u << 20)) return fail("bad brain free lists");
            std::vector<std::vector<std::uint64_t>> fl(nfl);
            for (auto& v : fl) {
                std::uint32_t n = 0;
                if (!r.get(n) || n > (1u << 24)) return fail("bad brain free list");
                v.resize(n);
                for (auto& x : v) if (!r.get(x)) return fail("truncated brain free list");
            }
            bs->arena.restore_state(cells, hw, live, fl);
            std::uint64_t nslots = 0;
            if (!r.get(nslots) || nslots > (1u << 22)) return fail("bad brain slot count");
            for (std::uint64_t i = 0; i < nslots; ++i) {
                EntityId nid = 0;
                BrainSlot s;
                if (!r.get(nid) || !r.get(s.hidden) || !r.get(s.lr_permille) ||
                    !r.get(s.trace_decay) || !r.get(s.mut_scale) ||
                    !r.get(s.woff) || !r.get(s.wbytes))
                    return fail("truncated brain slot");
                std::string wblob, hblob, ablob;
                if (!r.str(wblob) || wblob.size() != s.wbytes)
                    return fail("truncated brain weights");
                if (!r.str(hblob) || hblob.size() != s.h.size() * sizeof(std::int16_t))
                    return fail("truncated brain hidden");
                if (!r.str(ablob) || ablob.size() != s.last_action.size())
                    return fail("truncated brain action");
                std::memcpy(bs->arena.w8(s.woff), wblob.data(), s.wbytes);
                std::memcpy(s.h.data(), hblob.data(), hblob.size());
                std::memcpy(s.last_action.data(), ablob.data(), ablob.size());
                if (!r.get(s.replay_next) || !r.get(s.replay_count) ||
                    !r.get(s.mac_pending) || !r.get(s.mac_frac))
                    return fail("truncated brain replay head");
                s.fast.assign(kBrainFastLen, 0);
                s.anchor.assign(kBrainFastLen, 0);
                std::string fblob, anblob;
                if (!r.str(fblob) || fblob.size() != s.fast.size() ||
                    !r.str(anblob) || anblob.size() != s.anchor.size())
                    return fail("truncated brain plasticity");
                std::memcpy(s.fast.data(), fblob.data(), fblob.size());
                std::memcpy(s.anchor.data(), anblob.data(), anblob.size());
                for (Transition& t : s.replay) {
                    std::uint8_t tv = 0;
                    if (!r.get(tv) || !r.get(t.event_id) || !r.get(t.sigma) ||
                        !r.get(t.charge)) return fail("truncated brain transition");
                    t.valid = tv != 0;
                    std::string sb, ab, yb, wb;
                    if (!r.str(sb) || sb.size() != t.s.size() ||
                        !r.str(ab) || ab.size() != t.a.size() ||
                        !r.str(yb) || yb.size() != t.y.size() ||
                        !r.str(wb) || wb.size() != t.wh.size())
                        return fail("truncated brain transition vecs");
                    std::memcpy(t.s.data(), sb.data(), sb.size());
                    std::memcpy(t.a.data(), ab.data(), ab.size());
                    std::memcpy(t.y.data(), yb.data(), yb.size());
                    std::memcpy(t.wh.data(), wb.data(), wb.size());
                    if (fmt >= 21) {           // v21: dream-replay worker hidden
                        std::string h1b;
                        if (!r.str(h1b) || h1b.size() != t.w1h.size())
                            return fail("truncated brain transition w1h");
                        std::memcpy(t.w1h.data(), h1b.data(), h1b.size());
                    }
                }
                bs->slots.emplace(nid, std::move(s));
            }
            if (!r.get(bs->wm_err_sum) || !r.get(bs->wm_n) ||
                !r.get(bs->sigma_sum) || !r.get(bs->sigma_n) ||
                !r.get(bs->surprise_events))
                return fail("truncated brain stats");
            brains_ = std::move(bs);
        }
    }
    r.f.close();
    if (!r.ok()) return fail("read failed");
    return true;
}


// ================= Phase 12.1 — causal proto-language =========================
namespace {

std::uint32_t lang_prop_id(const World& w, const char* n) {
    static thread_local LitPidCache c_;
    return c_.get(w.props, n);
}

// Deterministic u32 seed: FNV-style mix over a composed key. Every "random"
// looking value in the language layer derives from (world_seed, key parts) —
// nothing is stored as literal vocabulary.
std::uint32_t lang_seed(std::uint64_t world_seed, const std::string& key) {
    std::uint64_t h = Rng::hash(key) ^ (world_seed * 0x9E3779B97F4A7C15ull);
    h ^= h >> 29; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 32;
    h *= 0x94D049BB133111EBull; h ^= h >> 29;
    return static_cast<std::uint32_t>(h & 0xFFFFFFFFull);
}

// Render a seed as a deterministic pseudo-word: syllable_length_bias
// syllables, each 1-2 chars drawn from a seed-derived alphabet. This is a
// display codec only — the alphabet is generated per-seed, never a stored
// vocabulary list.
std::string render_seed(std::uint32_t seed, std::int64_t syllables,
                        std::uint64_t world_seed) {
    if (seed == 0 || syllables <= 0) return "";
    std::string out;
    std::uint64_t h = seed;
    const int nsyl = static_cast<int>(std::clamp<std::int64_t>(syllables, 1, 4));
    for (int i = 0; i < nsyl; ++i) {
        h ^= h >> 13; h *= 0x5DEECE66Dull; h ^= h >> 16;
        const int len = 1 + static_cast<int>((h ^ (h >> 20)) % 2);
        for (int j = 0; j < len; ++j) {
            h ^= h >> 11; h *= 0x2545F4914Full; h ^= h >> 15;
            const char base = static_cast<char>('a' +
                static_cast<int>((h ^ world_seed) % 26));
            out.push_back(base);
        }
        if (i + 1 < nsyl) out.push_back('-');
    }
    return out;
}

bool lang_is_lang_entity(const Entity& e) { return e.kind_name == SIMLAB_KIND("language"); }
bool lang_is_entry(const Entity& e)    { return e.kind_name == SIMLAB_KIND("lexical_entry"); }
bool lang_is_concept(const Entity& e)  { return e.kind_name == SIMLAB_KIND("semantic_concept"); }
bool lang_is_atom(const Entity& e)    { return e.kind_name == SIMLAB_KIND("semantic_atom"); }
bool lang_is_inscription(const Entity& e){ return e.kind_name == SIMLAB_KIND("inscription"); }

// Familiarity an NPC holds for a lexical_entry entity, stored as an ordinary
// belief keyed "lex:<entry_id>" — beliefs already serialize, decay-adjacent
// and stay inside agent knowledge (never World Truth).
constexpr const char* kLexBeliefPrefix = "lex:";
std::string lex_belief_key(EntityId entry) {
    return std::string(kLexBeliefPrefix) + std::to_string(entry);
}
std::int64_t lex_familiarity(const AgentMind& m, EntityId entry) {
    const Belief* b = m.find_belief(lex_belief_key(entry));
    return b ? b->confidence : 0;
}

}  // namespace

void World::language_tick(std::uint64_t now) {
    const PropId p_lang   = lang_prop_id(*this, "language_id");
    const PropId p_vocal  = lang_prop_id(*this, "vocal_capability");
    const PropId p_harsh  = lang_prop_id(*this, "phoneme_harshness");
    const PropId p_syl    = lang_prop_id(*this, "syllable_length_bias");
    const PropId p_ckey   = lang_prop_id(*this, "concept_key");
    const PropId p_ckind  = lang_prop_id(*this, "concept_kind");
    const PropId p_cause  = lang_prop_id(*this, "cause_key");
    const PropId p_chron  = lang_prop_id(*this, "chronicle");
    const PropId p_pseed  = lang_prop_id(*this, "phoneme_seed");
    const PropId p_gseed  = lang_prop_id(*this, "glyph_seed");
    const PropId p_lname  = lang_prop_id(*this, "language_name");
    const PropId p_dacc   = lang_prop_id(*this, "dialect_drift_accum");
    const PropId p_drate  = lang_prop_id(*this, "dialect_drift_rate");
    const PropId p_diso   = lang_prop_id(*this, "dialect_drift_iso");
    const PropId p_droll  = lang_prop_id(*this, "dialect_drift_rolls");
    const PropId p_dthr   = lang_prop_id(*this, "dialect_drift_threshold");
    const PropId p_akey   = lang_prop_id(*this, "atom_key");
    const PropId p_akind  = lang_prop_id(*this, "atom_kind");
    const PropId p_ang    = lang_prop_id(*this, "glyph_angularity");
    const PropId p_ssd    = lang_prop_id(*this, "semantic_shift_seed");
    if (p_lang == kNoProp || p_ckey == kNoProp || p_pseed == kNoProp ||
        p_gseed == kNoProp)
        return;

    // Index existing language-world entities once per pass.
    std::map<std::string, EntityId> concepts;           // concept_key -> id
    std::map<std::string, EntityId> atoms;              // atom_key -> id
    std::set<std::string> entries;                      // "lang|ckey" covered
    std::set<std::int64_t> langs_seen;
    for (const auto& ep : entities) {
        const Entity& e = *ep;
        if (!e.alive) continue;
        if (lang_is_concept(e)) {
            const Value* v = prop_of(e, p_ckey);
            if (v && v->type == ValType::Text && !v->s.empty())
                concepts.emplace(v->s, e.id);
        } else if (lang_is_atom(e)) {
            const Value* v = p_akey != kNoProp ? prop_of(e, p_akey) : nullptr;
            if (v && v->type == ValType::Text && !v->s.empty())
                atoms.emplace(v->s, e.id);
        } else if (lang_is_entry(e)) {
            const Value* ck = prop_of(e, p_ckey);
            const Value* lv = prop_of(e, p_lang);
            if (ck && ck->type == ValType::Text && lv &&
                lv->type == ValType::Integer)
                entries.insert(std::to_string(lv->i) + "|" + ck->s);
            if (lv && lv->type == ValType::Integer) langs_seen.insert(lv->i);
        } else if (lang_is_lang_entity(e)) {
            const Value* lv = prop_of(e, p_lname);
            if (lv && lv->type == ValType::Integer) langs_seen.insert(lv->i);
        }
    }

    CausalEvent ev;
    ev.tick = now;
    ev.action = "language";
    ev.applied_laws.push_back(8);
    bool any = false;
    std::size_t coined = 0;
    constexpr std::size_t kMaxCoinagePerPass = 64;
    // verb vocabulary of the whole community: a decomposed token matching a
    // known action is an action atom (kind 2), everything else kind 1.
    // Built lazily: only a pass that actually coins a compositional word
    // needs it (known_actions never change during this pass, so the lazy
    // set equals the eager one — this used to be rebuilt every tick).
    std::set<std::string> verbs;
    bool verbs_built = false;
    auto get_verbs = [&]() -> const std::set<std::string>& {
        if (!verbs_built) {
            for (const auto& [mid, m] : minds_)
                for (const std::string& a : m.known_actions) verbs.insert(a);
            verbs_built = true;
        }
        return verbs;
    };

    for (auto& [mid, m] : minds_) {
        if (coined >= kMaxCoinagePerPass) break;
        Entity* e = find(mid);
        if (!e || !e->alive) continue;
        const std::int64_t vocal = p_vocal != kNoProp ? prop_int(*e, p_vocal, 0) : 0;
        if (vocal < 500) continue;                    // cannot coin words
        std::int64_t lang = p_lang != kNoProp ? prop_int(*e, p_lang, 0) : 0;
        if (lang <= 0) {
            // Community identity: same species/kind on the same world seed
            // converges on one language_id — a real "speech community".
            lang = 1 + static_cast<std::int64_t>(
                lang_seed(world_seed, "language|" + e->kind_name) % 1000000000ull);
            const Value* old = prop_of(*e, p_lang);
            StateChange ch;
            ch.entity = e->id; ch.prop = p_lang; ch.had_before = old != nullptr;
            ch.before = old ? *old : Value::integer(0);
            ch.after = Value::integer(lang);
            e->props.set(p_lang, Value::integer(lang));
            ev.changes.push_back(ch);
            any = true;
        }
        // lazily found one `language` registry entity per community
        if (!langs_seen.count(lang)) {
            PropertyBag lb;
            if (p_lname != kNoProp) lb.set(p_lname, Value::integer(lang));
            if (p_dacc  != kNoProp) lb.set(p_dacc,  Value::integer(0));
            if (p_drate != kNoProp) lb.set(p_drate, Value::integer(50));
            if (p_diso  != kNoProp) lb.set(p_diso,  Value::integer(400));
            if (p_droll != kNoProp) lb.set(p_droll, Value::integer(0));
            if (p_dthr  != kNoProp) lb.set(p_dthr,  Value::integer(20000));
            const EntityId lid = create_entity("language", kNoMaterial,
                                               e->pos, lb);
            if (lid != kNoEntity) {
                ev.spawned.push_back(lid);
                ev.evidence += "language " + std::to_string(lang) + " id " +
                               std::to_string(lid) + "; ";
                langs_seen.insert(lang);
                any = true;
            }
        }

        const std::int64_t harsh = p_harsh != kNoProp ? prop_int(*e, p_harsh, 500) : 500;
        const std::int64_t syl   = p_syl   != kNoProp ? prop_int(*e, p_syl,   2)   : 2;
        const std::int64_t ang   = p_ang   != kNoProp ? prop_int(*e, p_ang,   500) : 500;
        for (const auto& [dkey, d] : m.discoveries) {
            if (coined >= kMaxCoinagePerPass) break;
            if (d.status != DiscoveryStatus::Supported &&
                d.status != DiscoveryStatus::Verified) continue;
            const std::string& ck = d.key;
            if (ck.empty()) continue;
            const std::string hk = std::to_string(lang) + "|" + ck;
            if (entries.count(hk)) continue;         // community already has it

            // SemanticConcept: one per causal key, shared across languages.
            EntityId cid = kNoEntity;
            const auto ci = concepts.find(ck);
            if (ci != concepts.end()) cid = ci->second;
            else {
                PropertyBag cb;
                cb.set(p_ckey, Value::text(ck));
                if (p_ckind != kNoProp) cb.set(p_ckind, Value::integer(4));
                if (p_cause != kNoProp) cb.set(p_cause, Value::text(ck));
                if (p_chron != kNoProp) cb.set(p_chron, Value::text(d.claim));
                cid = create_entity("semantic_concept", kNoMaterial, e->pos, cb);
                if (cid == kNoEntity) continue;
                ev.spawned.push_back(cid);
                ev.evidence += "concept " + ck + " id " + std::to_string(cid) + "; ";
                concepts.emplace(ck, cid);
                any = true;
            }
            // LexicalEntry: phoneme seed folds species acoustics (harshness,
            // syllable bias); glyph seed stays purely (seed, lang, concept).
            const std::uint32_t ps = lang_seed(world_seed,
                "phoneme|" + std::to_string(lang) + "|" + ck + "|" +
                std::to_string(harsh) + "|" + std::to_string(syl));
            const std::uint32_t gs = lang_seed(world_seed,
                "glyph|" + std::to_string(lang) + "|" + ck + "|" +
                std::to_string(ang));
            PropertyBag eb;
            eb.set(p_ckey,  Value::text(ck));
            eb.set(p_lang,  Value::integer(lang));
            eb.set(p_pseed, Value::integer(static_cast<std::int64_t>(ps)));
            eb.set(p_gseed, Value::integer(static_cast<std::int64_t>(gs)));
            if (p_ssd != kNoProp)
                eb.set(p_ssd, Value::integer(static_cast<std::int64_t>(
                    lang_seed(world_seed, "shift|" + std::to_string(lang) +
                              "|" + ck))));
            const EntityId eid = create_entity("lexical_entry", kNoMaterial,
                                               e->pos, eb);
            if (eid == kNoEntity) continue;
            relations.relate(eid, "names", cid);
            ev.spawned.push_back(eid);
            ev.evidence += "lexeme " + ck + " lang " + std::to_string(lang) +
                           " id " + std::to_string(eid) + "; ";
            entries.insert(hk);
            // the coiner knows their own word well
            Belief& b = m.belief(lex_belief_key(eid));
            b.status = BeliefStatus::Verified;
            b.confidence = 800;
            b.last_tick = now;
            relations.relate(mid, "knows_word", eid);
            ++coined;
            any = true;

            // Compositional layer: decompose the causal key into SemanticAtoms
            // and coin one word per atom — CausalClauses recombine these at
            // inscription time instead of minting a word per situation.
            if (p_akey != kNoProp) {
                const std::vector<AtomRef> ats = decompose_key(d.key, get_verbs());
                for (const AtomRef& at : ats) {
                    if (coined >= kMaxCoinagePerPass) break;
                    const std::string ahk =
                        std::to_string(lang) + "|" + at.key;
                    if (entries.count(ahk)) continue;
                    EntityId aid = kNoEntity;
                    const auto ai = atoms.find(at.key);
                    if (ai != atoms.end()) aid = ai->second;
                    else {
                        PropertyBag ab;
                        ab.set(p_akey, Value::text(at.key));
                        if (p_akind != kNoProp)
                            ab.set(p_akind, Value::integer(at.kind));
                        if (p_ckind != kNoProp)
                            ab.set(p_ckind, Value::integer(at.kind));
                        if (p_chron != kNoProp)
                            ab.set(p_chron, Value::text(d.claim));
                        aid = create_entity("semantic_atom", kNoMaterial,
                                            e->pos, ab);
                        if (aid == kNoEntity) continue;
                        ev.spawned.push_back(aid);
                        atoms.emplace(at.key, aid);
                        any = true;
                    }
                    const std::uint32_t aps = lang_seed(world_seed,
                        "phoneme|" + std::to_string(lang) + "|" + at.key +
                        "|" + std::to_string(harsh) + "|" +
                        std::to_string(syl));
                    const std::uint32_t ags = lang_seed(world_seed,
                        "glyph|" + std::to_string(lang) + "|" + at.key +
                        "|" + std::to_string(ang));
                    PropertyBag aeb;
                    aeb.set(p_ckey, Value::text(at.key));
                    aeb.set(p_lang, Value::integer(lang));
                    aeb.set(p_pseed,
                        Value::integer(static_cast<std::int64_t>(aps)));
                    aeb.set(p_gseed,
                        Value::integer(static_cast<std::int64_t>(ags)));
                    if (p_ssd != kNoProp)
                        aeb.set(p_ssd, Value::integer(
                            static_cast<std::int64_t>(lang_seed(world_seed,
                                "shift|" + std::to_string(lang) + "|" +
                                at.key))));
                    const EntityId aeid = create_entity("lexical_entry",
                        kNoMaterial, e->pos, aeb);
                    if (aeid == kNoEntity) continue;
                    relations.relate(aeid, "names", aid);
                    ev.spawned.push_back(aeid);
                    entries.insert(ahk);
                    Belief& bb = m.belief(lex_belief_key(aeid));
                    bb.status = BeliefStatus::Verified;
                    bb.confidence = 800;
                    bb.last_tick = now;
                    relations.relate(mid, "knows_word", aeid);
                    ++coined;
                    any = true;
                }
            }
        }
    }
    if (any) ledger.append(std::move(ev));
}

void World::language_drift(Entity& e, std::int64_t pending, std::uint64_t /*now*/,
                           CausalEvent& ev) {
    static thread_local SeqPidCache lp_c_;
    SeqPid lpid(lp_c_, props);
    if (pending <= 0 || !e.alive) return;
    const PropId p_lname = lpid("language_name");
    const PropId p_dacc  = lpid("dialect_drift_accum");
    const PropId p_drate = lpid("dialect_drift_rate");
    const PropId p_diso  = lpid("dialect_drift_iso");
    const PropId p_droll = lpid("dialect_drift_rolls");
    const PropId p_dthr  = lpid("dialect_drift_threshold");
    const PropId p_lang  = lpid("language_id");
    const PropId p_ps    = lpid("phoneme_seed");
    const PropId p_lvl   = lpid("erosion_level");
    const PropId p_cktxt = lpid("concept_keys");
    const PropId p_eacc  = lpid("erosion_accum");
    const PropId p_dur   = lpid("inscription_durability");
    const PropId p_gtxt  = lpid("glyph_text");

    if (lang_is_lang_entity(e)) {
        if (p_lname == kNoProp || p_dacc == kNoProp || p_drate == kNoProp ||
            p_droll == kNoProp || p_dthr == kNoProp)
            return;
        const std::int64_t lang = prop_int(e, p_lname, 0);
        if (lang <= 0) return;
        // isolation: the community counts as isolated when NO member has a
        // fellow speaker inside communicate_range. Positions do not move
        // during metabolize, so the flag is constant across the window.
        const PropId p_crange = lpid("communicate_range");
        const std::int64_t crange = p_crange != kNoProp
            ? prop_int(e, p_crange, 400) : 400;
        bool isolated = true;
        {
            std::vector<EntityId> members;
            for (const auto& ep : entities) {
                const Entity& o = *ep;
                if (!o.alive || o.id == e.id) continue;
                if (o.kind_name == SIMLAB_KIND("language") || o.kind_name == SIMLAB_KIND("lexical_entry") ||
                    o.kind_name == SIMLAB_KIND("semantic_concept") ||
                    o.kind_name == SIMLAB_KIND("inscription")) continue;
                if (p_lang != kNoProp && prop_int(o, p_lang, 0) == lang)
                    members.push_back(o.id);
            }
            for (EntityId mid : members) {
                const Entity* me = find(mid);
                if (!me) continue;
                bool near = false;
                for (const EntityId nid : entities_near(me->pos, crange)) {
                    if (nid == mid) continue;
                    const Entity* ne = find(nid);
                    if (!ne || !ne->alive) continue;
                    if (ne->kind_name == SIMLAB_KIND("language") ||
                        ne->kind_name == SIMLAB_KIND("lexical_entry") ||
                        ne->kind_name == SIMLAB_KIND("semantic_concept") ||
                        ne->kind_name == SIMLAB_KIND("inscription")) continue;
                    if (p_lang != kNoProp && prop_int(*ne, p_lang, 0) == lang) {
                        near = true; break;
                    }
                }
                if (near) { isolated = false; break; }
            }
        }
        const std::int64_t rate = prop_int(e, p_drate, 50);
        const std::int64_t iso  = prop_int(e, p_diso, 400);
        const std::int64_t mult = isolated ? iso : 1000;
        const std::int64_t acc0 = prop_int(e, p_dacc, 0);
        const std::int64_t nacc = acc0 + pending * rate * mult / 1000;
        const std::int64_t thr  = std::max<std::int64_t>(
            prop_int(e, p_dthr, 20000), 1);
        const std::int64_t rolls0 = prop_int(e, p_droll, 0);
        const std::int64_t rolls1 = rolls0 + nacc / thr;
        const std::int64_t rem = nacc % thr;
        auto setp = [&](PropId p, std::int64_t v) {
            if (p == kNoProp) return;
            const Value* old = prop_of(e, p);
            StateChange ch;
            ch.entity = e.id; ch.prop = p; ch.had_before = old != nullptr;
            ch.before = old ? *old : Value::integer(0);
            ch.after = Value::integer(v);
            e.props.set(p, Value::integer(v));
            ev.changes.push_back(ch);
        };
        setp(p_dacc, rem);
        if (rolls1 != rolls0) {
            setp(p_droll, rolls1);
            // cumulative XOR drift: roll r shifts each entry's phoneme_seed
            // by mask(lang, entry, r). Final seed depends only on roll COUNT
            // -> dt=1 x N and dt=N produce identical words.
            if (p_ps != kNoProp && p_lang != kNoProp) {
                for (const auto& ep : entities) {
                    Entity& le = *ep;
                    if (!le.alive || !lang_is_entry(le)) continue;
                    if (prop_int(le, p_lang, 0) != lang) continue;
                    const PropId lps = p_ps;
                    const Value* sv = prop_of(le, lps);
                    if (!sv || sv->type != ValType::Integer) continue;
                    std::uint32_t seed = static_cast<std::uint32_t>(sv->i);
                    for (std::int64_t r = rolls0 + 1; r <= rolls1; ++r) {
                        const std::uint32_t m = lang_seed(world_seed,
                            "drift|" + std::to_string(lang) + "|" +
                            std::to_string(le.id) + "|" + std::to_string(r));
                        seed ^= m;
                    }
                    const Value* old = prop_of(le, lps);
                    StateChange ch;
                    ch.entity = le.id; ch.prop = lps; ch.had_before = true;
                    ch.before = *old; ch.after = Value::integer(
                        static_cast<std::int64_t>(seed));
                    le.props.set(lps, Value::integer(
                        static_cast<std::int64_t>(seed)));
                    ev.changes.push_back(ch);
                }
            }
            ev.participants.push_back(e.id);
        }
        return;
    }

    if (lang_is_inscription(e)) {
        if (p_lvl == kNoProp || p_eacc == kNoProp || p_dur == kNoProp)
            return;
        // rate: (1000 - durability) permille-ish per tick, scaled x1000 into
        // the milli accumulator; 1000 accum = 1 erosion_level permille.
        const std::int64_t dur = prop_int(e, p_dur, 0);
        const std::int64_t acc0 = prop_int(e, p_eacc, 0);
        const std::int64_t nacc = acc0 + pending * std::max<std::int64_t>(0, 1000 - dur);
        const std::int64_t lvl0 = prop_int(e, p_lvl, 0);
        std::int64_t lvl1 = lvl0 + nacc / 1000;
        const std::int64_t rem = nacc % 1000;
        if (lvl1 > 1000) lvl1 = 1000;
        auto setp = [&](PropId p, std::int64_t v) {
            if (p == kNoProp) return;
            const Value* old = prop_of(e, p);
            StateChange ch;
            ch.entity = e.id; ch.prop = p; ch.had_before = old != nullptr;
            ch.before = old ? *old : Value::integer(0);
            ch.after = Value::integer(v);
            e.props.set(p, Value::integer(v));
            ev.changes.push_back(ch);
        };
        setp(p_eacc, rem);
        if (lvl1 != lvl0) setp(p_lvl, lvl1);
        // illegible: glyph stream and concept mapping are erased for good.
        // Monotonic — ticked and batched reach the same terminal state.
        if (lvl1 >= 1000 && (p_gtxt != kNoProp || p_cktxt != kNoProp)) {
            const Value* gv = p_gtxt != kNoProp ? prop_of(e, p_gtxt) : nullptr;
            const Value* kv = p_cktxt != kNoProp ? prop_of(e, p_cktxt) : nullptr;
            const bool gone = (!gv || (gv->type == ValType::Text && gv->s.empty())) &&
                              (!kv || (kv->type == ValType::Text && kv->s.empty()));
            if (!gone) {
                if (p_gtxt != kNoProp) {
                    StateChange ch; ch.entity = e.id; ch.prop = p_gtxt;
                    ch.had_before = gv != nullptr; ch.before = gv ? *gv : Value::text("");
                    ch.after = Value::text("");
                    e.props.set(p_gtxt, Value::text(""));
                    ev.changes.push_back(ch);
                }
                if (p_cktxt != kNoProp) {
                    StateChange ch; ch.entity = e.id; ch.prop = p_cktxt;
                    ch.had_before = kv != nullptr; ch.before = kv ? *kv : Value::text("");
                    ch.after = Value::text("");
                    e.props.set(p_cktxt, Value::text(""));
                    ev.changes.push_back(ch);
                }
            }
        }
        if (lvl1 != lvl0 || rem != acc0) ev.participants.push_back(e.id);
    }
}


void World::econ_drift(Entity& e, std::int64_t pending, std::uint64_t now,
                       CausalEvent& ev) {
    if (pending <= 0 || !e.alive) return;
    const PropId p_poss  = SIMLAB_PID(props, "possessed_by");
    const PropId p_ptick = SIMLAB_PID(props, "possession_ticks");
    const PropId p_theft = SIMLAB_PID(props, "theft_flag");
    const PropId p_ds    = SIMLAB_PID(props, "debt_status");
    const PropId p_due   = SIMLAB_PID(props, "due_tick");
    const PropId p_dv    = SIMLAB_PID(props, "debt_value");
    const PropId p_db    = SIMLAB_PID(props, "debtor_id");
    const PropId p_rep   = SIMLAB_PID(props, "trade_reputation");
    auto setp = [&](Entity& ent, PropId p, std::int64_t v) {
        if (p == kNoProp) return;
        const Value* old = prop_of(ent, p);
        StateChange ch;
        ch.entity = ent.id; ch.prop = p; ch.had_before = old != nullptr;
        ch.before = old ? *old : Value::integer(0);
        ch.after = Value::integer(v);
        ent.props.set(p, Value::integer(v));
        ev.changes.push_back(ch);
    };

    // ---- debt_obligation overdue -> default --------------------------------
    if (e.kind_name == SIMLAB_KIND("debt_obligation") && p_ds != kNoProp &&
        p_due != kNoProp) {
        const std::int64_t st = prop_int(e, p_ds, 0);
        const std::int64_t due = prop_int(e, p_due, 0);
        if (st == 0 && static_cast<std::int64_t>(now) > due) {
            setp(e, p_ds, 2);
            setp(e, p_dv, 0);
            const std::int64_t debtor = p_db != kNoProp
                ? prop_int(e, p_db, 0) : 0;
            if (debtor != 0 && p_rep != kNoProp) {
                if (Entity* de = find(static_cast<EntityId>(debtor))) {
                    const std::int64_t rep = prop_int(*de, p_rep, 1000);
                    // default: reputation collapses below the credit floor
                    setp(*de, p_rep, std::max<std::int64_t>(0, rep - 650));
                }
            }
            ev.participants.push_back(e.id);
            ev.evidence += "debt defaulted; ";
        }
        return;   // obligations carry no possession state
    }

    // ---- possession bookkeeping --------------------------------------------
    if (p_poss != kNoProp) {
        const std::int64_t holder = prop_int(e, p_poss, 0);
        if (holder != 0) {
            if (p_ptick != kNoProp) {
                const std::int64_t t0 = prop_int(e, p_ptick, 0);
                setp(e, p_ptick, std::min<std::int64_t>(
                    1000000000, t0 + pending));
            }
        }
    }
    // ---- theft exposure window ----------------------------------------------
    if (p_theft != kNoProp) {
        const std::int64_t flag = prop_int(e, p_theft, 0);
        if (flag > 0 && p_poss != kNoProp) {
            const std::int64_t holder = prop_int(e, p_poss, 0);
            // exposure = any OTHER minded agent within sight of the item.
            // Positions are constant across a metabolize window, so the
            // witness set is identical ticked vs batched: evaluate ONCE
            // against flag>0 (pre-decrement) to stay LOD-exact.
            bool exposed = false;
            if (holder != 0) {
                const PropId sight = SIMLAB_PID(props, "sight_range");
                for (const auto& [wid, wm] : minds_) {
                    (void)wm;
                    if (wid == static_cast<EntityId>(holder)) continue;
                    const Entity* we = find(wid);
                    if (!we || !we->alive) continue;
                    const std::int64_t range = sight != kNoProp
                        ? prop_int(*we, sight, 0) : 0;
                    if (range > 0 &&
                        dist2(we->pos, e.pos) <= range * range) {
                        exposed = true; break;
                    }
                }
            }
            if (exposed) {
                // caught holding the hot item: reputation hit, flag spent
                setp(e, p_theft, 0);
                if (holder != 0 && p_rep != kNoProp) {
                    if (Entity* he = find(static_cast<EntityId>(holder))) {
                        const std::int64_t rep = prop_int(*he, p_rep, 1000);
                        setp(*he, p_rep,
                             std::max<std::int64_t>(0, rep - 200));
                    }
                }
                ev.evidence += "theft exposed; ";
            } else {
                setp(e, p_theft, std::max<std::int64_t>(0, flag - pending));
            }
        }
    }
}


// ---- Phase 12.3: political cadence ------------------------------------------
// One accumulator-exact pass per advance, same contract as decay_minds:
// fires = floor((accum+n)/200) is identical for dt=1 xN and dt=N; every
// condition read (defer beliefs, public_pool, last_relief_tick,
// perceived_abundance beliefs, legitimacy) is interact-stable, so the
// per-fire deltas compose linearly and the final state matches exactly.
void World::political_drift(std::int64_t n) {
    if (n <= 0) return;
    constexpr std::int64_t kPeriod = 200;
    const std::uint64_t now = clock_.tick();
    CausalEvent ev;
    ev.tick = now;
    ev.action = "political_drift";
    bool ledgered = false;
    const PropId p_leg = SIMLAB_PID(props, "social_legitimacy");
    for (auto& [eid, mind] : minds_) {
        Entity* e = find(eid);
        if (!e || !e->alive) continue;
        mind.political_accum += n;
        std::int64_t fires = mind.political_accum / kPeriod;
        const std::int64_t rem = mind.political_accum % kPeriod;
        mind.political_accum = rem;
        // Per-fire loop, NOT fires*delta batching: the revolt threshold is
        // nonlinear, so each period must be replayed in order — identical
        // to what dt=1 xN produces across separate decay_minds calls. Each
        // fire is evaluated at ITS historical tick so time-gated conditions
        // (relief freshness) match the ticked replay exactly.
        while (fires-- > 0) {
            const std::int64_t ftick = std::max<std::int64_t>(0,
                static_cast<std::int64_t>(now) - rem -
                fires * kPeriod);
            // (b) deference network refresh
            pol_update_defer(*this, eid);

            // current allegiance target (top-confidence defer_to belief)
            EntityId tyr = kNoEntity;
            std::int64_t best_conf = 0;
            for (const auto& [k, b] : mind.beliefs) {
                if (k.rfind("defer_to:", 0) != 0 ||
                    b.confidence <= best_conf) continue;
                try {
                    tyr = static_cast<EntityId>(std::stoull(k.substr(9)));
                    best_conf = b.confidence;
                } catch (...) {}
            }
            Belief& g = mind.belief("grievance");
            std::int64_t delta = -2;                // slow natural cooling
            if (tyr != kNoEntity) {
                const Entity* te = find(tyr);
                const std::int64_t pool = te
                    ? pol_prop(*this, *te, "public_pool", 0) : 0;
                const std::int64_t relief = te
                    ? pol_prop(*this, *te, "last_relief_tick", 0) : 0;
                bool scarce = false;
                for (const auto& [bk, bv] : mind.beliefs)
                    if (bk.rfind("pa:", 0) == 0 &&
                        bv.confidence < 300) { scarce = true; break; }
                if (pool > 500 && scarce && ftick > relief + 1000)
                    delta += 60;  // hoarded tribute while pots run thin
                else if (pool > 0 && relief > 0 && ftick <= relief + 1000)
                    delta -= 80;  // fresh relief cools resentment fast
            }
            g.confidence = std::clamp<std::int64_t>(
                g.confidence + delta, 0, 1000);
            g.last_tick = static_cast<std::uint64_t>(ftick);

            // (d) revolt: grievance past threshold withdraws allegiance and
            // bleeds the tyrant's legitimacy — ledgered once per call.
            if (g.confidence >= 700) {
                std::vector<std::string> stale;
                for (const auto& [k, b] : mind.beliefs)
                    if (k.rfind("defer_to:", 0) == 0)
                        stale.push_back(k);
                for (const auto& k : stale) mind.beliefs.erase(k);
                g.confidence = 400;                  // hysteresis band
                if (tyr != kNoEntity && p_leg != kNoProp) {
                    if (Entity* te = find(tyr)) {
                        const std::int64_t leg =
                            prop_int(*te, p_leg, 500);
                        const Value* old = prop_of(*te, p_leg);
                        StateChange ch;
                        ch.entity = te->id; ch.prop = p_leg;
                        ch.had_before = old != nullptr;
                        ch.before = old ? *old : Value::integer(500);
                        ch.after = Value::integer(std::max<
                            std::int64_t>(0, leg - 50));
                        te->props.set(p_leg, ch.after);
                        ev.changes.push_back(ch);
                        ev.participants.push_back(tyr);
                        ledgered = true;
                    }
                }
            }
        }
    }
    if (ledgered) {
        ev.evidence = "deference network & grievance drift; ";
        ledger.append(std::move(ev));
    }
}

// Phase 12.5 mythogenesis: a mind that lived through an unexplained shock —
// a discovery record that was tested but never Verified, carrying strong
// emotional charge of EITHER sign (trauma < 0: plague, collapse; salvation
// > 0: a cure nobody can explain) — and whose agency_attribution_bias is
// high begins reading WILL into the causal key. Conviction grows per period
// by (agency-300)/2 scaled by abstraction_gain; the first crossing of 600
// crystallizes ONE shared semantic_concept per key with a seed-derived
// agent name, a derived appeasement act and a valence ("curse" for trauma,
// "blessing" for salvation) — nothing named in code.
//
// The same period runs oral attrition: a `lex:` familiarity not rehearsed
// (spoken, heard or chanted) for kLexStale ticks fades by kLexFade per
// period — but a word that crystallized into a proverb/epic (oral_genre set
// on the ENTRY, World Truth) fades only kLexFadeOral: meter resists oblivion.
// Per-mind accumulator, per-fire evaluation at each fire's historical tick
// — identical contract to political_drift, so dt=1xN == dt=N exactly.
void World::myth_drift(std::int64_t n) {
    if (n <= 0) return;
    constexpr std::int64_t kPeriod = 100;
    constexpr std::int64_t kLexStale = 5000;
    constexpr std::int64_t kLexFade = 10, kLexFadeOral = 1;
    const std::uint64_t now = clock_.tick();
    static thread_local SeqPidCache pidn_c_;
    SeqPid pidn(pidn_c_, props);
    const PropId p_skey  = pidn("sacred_key");
    const PropId p_sagent= pidn("sacred_agent");
    const PropId p_appe  = pidn("appeasement_action");
    const PropId p_val   = pidn("sacred_valence");
    const PropId p_ckey  = pidn("concept_key");
    const PropId p_chron = pidn("chronicle");
    const PropId p_cause = pidn("cause_key");
    const PropId p_abias = pidn("agency_attribution_bias");
    const PropId p_genre = pidn("oral_genre");
    if (p_skey == kNoProp || p_ckey == kNoProp) return;

    // cheap precheck: advance every accumulator; only minds with a period
    // boundary in this call do any work (no O(entities) scan otherwise).
    std::vector<std::pair<EntityId, std::int64_t>> due;   // (mind, fires)
    for (auto& [eid, mind] : minds_) {
        const Entity* le = find(eid);
        if (!le || !le->alive) continue;          // the dead dream no myths
        mind.myth_accum += n;
        const std::int64_t fires = mind.myth_accum / kPeriod;
        mind.myth_accum %= kPeriod;
        if (fires > 0) due.emplace_back(eid, fires);
    }
    if (due.empty()) return;

    // existing sacred concepts by causal key — one World-Truth concept per myth
    std::map<std::string, EntityId> sacred_seen;
    for (const auto& ep : entities) {
        const Entity& e = *ep;
        if (!e.alive) continue;
        const Value* sk = prop_of(e, p_skey);
        if (sk && sk->type == ValType::Text && !sk->s.empty())
            sacred_seen.emplace(sk->s, e.id);
    }
    // is a lexical entry an oral classic? (memoized per call — World Truth)
    std::map<EntityId, bool> oral;
    auto is_oral = [&](EntityId le) {
        const auto it = oral.find(le);
        if (it != oral.end()) return it->second;
        bool o = false;
        if (p_genre != kNoProp)
            if (const Entity* ee = find(le)) {
                const Value* gv = prop_of(*ee, p_genre);
                o = gv && gv->type == ValType::Text && !gv->s.empty();
            }
        oral.emplace(le, o);
        return o;
    };

    CausalEvent ev;
    ev.tick = now;
    ev.action = "myth_drift";
    ev.applied_laws.push_back(8);                 // social.interaction family
    bool ledgered = false;
    for (const auto& [eid, fires0] : due) {
        AgentMind& mind = minds_[eid];
        Entity* e = find(eid);
        if (!e || !e->alive) continue;
        const std::int64_t rem = mind.myth_accum;
        const std::int64_t agency = p_abias != kNoProp
            ? prop_int(*e, p_abias, 500) : 500;
        const MindParams mp = mind_params(*this, *e);
        std::int64_t fires = fires0;
        while (fires-- > 0) {
            const std::int64_t ftick = std::max<std::int64_t>(0,
                static_cast<std::int64_t>(now) - rem - fires * kPeriod);
            // ---- oral attrition (every mind, every period) ----------------
            for (auto& [bk, b] : mind.beliefs) {
                if (bk.rfind("lex:", 0) != 0 || b.confidence <= 0) continue;
                if (ftick - static_cast<std::int64_t>(b.last_tick) < kLexStale)
                    continue;
                EntityId le = kNoEntity;
                try { le = static_cast<EntityId>(std::stoull(bk.substr(4))); }
                catch (...) { continue; }
                b.confidence = std::max<std::int64_t>(0,
                    b.confidence - (is_oral(le) ? kLexFadeOral : kLexFade));
            }
            // ---- mythogenesis (only minds that read will into events) -----
            if (agency < 400) continue;      // materialist minds need no myth
            const std::int64_t gain = std::max<std::int64_t>(0,
                (agency - 300) / 2 * mp.abstraction_gain / 1000);
            if (gain <= 0) continue;
            for (const auto& [dkey, d] : mind.discoveries) {
                if (d.status == DiscoveryStatus::Verified ||
                    d.status == DiscoveryStatus::Open) continue;
                if (d.trials == 0 && d.evidence_for + d.evidence_against == 0)
                    continue;
                const std::int64_t charge = d.engram.emotional_charge;
                if ((charge < 0 ? -charge : charge) < mp.trauma_charge) continue;
                Belief& sb = mind.belief("sacred:" + d.key);
                if (sb.confidence >= 1000) continue;
                const std::int64_t before = sb.confidence;
                sb.confidence = std::min<std::int64_t>(1000, sb.confidence + gain);
                sb.status = BeliefStatus::Observed;
                sb.last_tick = static_cast<std::uint64_t>(ftick);
                if (before >= 600 || sb.confidence < 600) continue;
                if (sacred_seen.count(d.key)) continue;
                // crystallize: one shared concept per causal key
                const std::uint32_t hseed = static_cast<std::uint32_t>(
                    lang_seed(world_seed, "agent|" + d.key));
                const std::string agent = render_seed(hseed, 2, world_seed);
                std::string appease = "heat";
                if (mind.known_actions.count("share") &&
                    !mind.known_actions.count("heat"))
                    appease = "share";
                const std::string valence = charge < 0 ? "curse" : "blessing";
                PropertyBag pb;
                pb.set(p_ckey, Value::text("sacred|" + d.key));
                pb.set(p_skey, Value::text(d.key));
                if (p_sagent != kNoProp) pb.set(p_sagent, Value::text(agent));
                if (p_appe   != kNoProp) pb.set(p_appe, Value::text(appease));
                if (p_val    != kNoProp) pb.set(p_val, Value::text(valence));
                if (p_chron  != kNoProp) pb.set(p_chron,
                    Value::text(d.claim + " — " + valence + " of " + agent));
                if (p_cause  != kNoProp) pb.set(p_cause, Value::text(d.key));
                const EntityId cid = create_entity(
                    "semantic_concept", kNoMaterial, e->pos, pb);
                if (cid != kNoEntity) {
                    sacred_seen.emplace(d.key, cid);
                    ev.spawned.push_back(cid);
                    ev.participants.push_back(eid);
                    ev.evidence += "sacred concept " + d.key + " agent=" +
                        agent + " valence=" + valence + "; ";
                    ledgered = true;
                }
            }
        }
    }
    if (ledgered) {
        const EventId id = ledger.append(std::move(ev));
        if (const CausalEvent* ce = ledger.find(id)) note_touched(*ce);
    }
}

std::string World::export_lexicon_codex() const {
    const PropId p_ckey  = lang_prop_id(*this, "concept_key");
    const PropId p_lang  = lang_prop_id(*this, "language_id");
    const PropId p_ps    = lang_prop_id(*this, "phoneme_seed");
    const PropId p_gs    = lang_prop_id(*this, "glyph_seed");
    const PropId p_chron = lang_prop_id(*this, "chronicle");
    const PropId p_lvl   = lang_prop_id(*this, "erosion_level");
    const PropId p_gtxt  = lang_prop_id(*this, "glyph_text");
    const PropId p_auth  = lang_prop_id(*this, "inscription_author");
    const PropId p_itk   = lang_prop_id(*this, "inscription_tick");
    const PropId p_akey  = lang_prop_id(*this, "atom_key");
    const PropId p_akind = lang_prop_id(*this, "atom_kind");
    const PropId p_cl    = lang_prop_id(*this, "clause_atoms");
    const PropId p_l2    = lang_prop_id(*this, "language_id2");
    const PropId p_port  = lang_prop_id(*this, "portable_surface");
    if (p_ckey == kNoProp) return "";

    // chronicle per concept_key (World Truth — written at coinage)
    std::map<std::string, std::string> chronicles;
    for (const auto& ep : entities) {
        const Entity& e = *ep;
        if (!e.alive || !lang_is_concept(e)) continue;
        const Value* ck = prop_of(e, p_ckey);
        const Value* ch = p_chron != kNoProp ? prop_of(e, p_chron) : nullptr;
        if (ck && ck->type == ValType::Text && ch && ch->type == ValType::Text)
            chronicles[ck->s] = ch->s;
    }
    // per-language familiarity: best confidence any mind holds per entry
    std::map<std::int64_t, std::int64_t> lang_known, lang_total;
    struct LexRow { EntityId id; std::int64_t lang; std::uint32_t ps, gs;
                    std::string ck; std::int64_t deciphered; };
    std::vector<LexRow> rows;
    for (const auto& ep : entities) {
        const Entity& e = *ep;
        if (!e.alive || !lang_is_entry(e)) continue;
        const Value* ck = prop_of(e, p_ckey);
        const Value* lv = p_lang != kNoProp ? prop_of(e, p_lang) : nullptr;
        const Value* pv = p_ps   != kNoProp ? prop_of(e, p_ps)   : nullptr;
        const Value* gv = p_gs   != kNoProp ? prop_of(e, p_gs)   : nullptr;
        if (!ck || ck->type != ValType::Text || !lv ||
            lv->type != ValType::Integer) continue;
        LexRow r; r.id = e.id; r.lang = lv->i; r.ck = ck->s;
        r.ps = pv && pv->type == ValType::Integer
            ? static_cast<std::uint32_t>(pv->i) : 0;
        r.gs = gv && gv->type == ValType::Integer
            ? static_cast<std::uint32_t>(gv->i) : 0;
        r.deciphered = 0;
        for (const auto& [mid, m] : minds_) {
            (void)mid;
            r.deciphered = std::max<std::int64_t>(
                r.deciphered, lex_familiarity(m, r.id));
        }
        rows.push_back(r);
        lang_total[r.lang]++;
        if (r.deciphered >= 500) lang_known[r.lang]++;
    }
    std::sort(rows.begin(), rows.end(),
              [](const LexRow& a, const LexRow& b){ return a.id < b.id; });

    std::ostringstream o;
    o << "# lexicon codex — world_seed " << world_seed << "\n";
    // per language: raw phoneme/glyph renderings + objective chronicles +
    // community deciphered_ratio (permille of entries known >=500 fam)
    std::set<std::int64_t> langs;
    for (const auto& r : rows) langs.insert(r.lang);
    for (std::int64_t lg : langs) {
        const std::int64_t tot = lang_total[lg];
        const std::int64_t kn  = lang_known[lg];
        const std::int64_t ratio = tot > 0 ? kn * 1000 / tot : 0;
        o << "LANG " << lg << " deciphered_ratio " << ratio << "\n";
        for (const auto& r : rows) {
            if (r.lang != lg) continue;
            // syllable length from the species acoustics is unknowable for a
            // dead language — render at the community-neutral default 2.
            const std::string ph = render_seed(r.ps, 2, world_seed);
            const std::string gl = render_seed(r.gs, 2, world_seed);
            const auto ci = chronicles.find(r.ck);
            const std::string chron = ci != chronicles.end() ? ci->second : "";
            o << "  ENTRY " << r.id << " phonemes=" << ph
              << " glyphs=" << gl << " concept=" << r.ck
              << " deciphered=" << r.deciphered
              << " chronicle=" << chron << "\n";
        }
    }
    // Phase 19 indigenous mathematics: each tongue's numeral base, glyph
    // style and a worked sample so the player can decode carved M/P/Q lines.
    for (std::int64_t lg : langs) {
        const std::int64_t base = math_base_of(lg, world_seed);
        static const char* kStyle[] = {"tally", "dot-bar", "bar", "script"};
        std::uint64_t h = static_cast<std::uint64_t>(lg) * 0xC2B2AE3D27D4EB4Full
                       ^ world_seed * 0x165667B19E3779F9ull;
        h ^= h >> 29; h *= 0x94D049BB133111EBull; h ^= h >> 32;
        o << "MATH lang " << lg << " base " << base
          << " style " << kStyle[h % 4]
          << " sample8=" << math_render(lg, 8, world_seed)
          << " sample13=" << math_render(lg, 13, world_seed) << "\n";
    }
    // semantic atoms: the atomic meanings words name (compositional layer)
    for (const auto& ep : entities) {
        const Entity& e = *ep;
        if (!e.alive || !lang_is_atom(e)) continue;
        const Value* ak = p_akey  != kNoProp ? prop_of(e, p_akey)  : nullptr;
        const Value* ai = p_akind != kNoProp ? prop_of(e, p_akind) : nullptr;
        const Value* ac = p_chron != kNoProp ? prop_of(e, p_chron) : nullptr;
        o << "ATOM " << e.id
          << " key=" << (ak && ak->type == ValType::Text ? ak->s : "")
          << " kind=" << (ai && ai->type == ValType::Integer ? ai->i : 0)
          << " chronicle=" << (ac && ac->type == ValType::Text ? ac->s : "")
          << "\n";
    }
    // inscriptions: raw carved glyph stream + which language + erosion
    for (const auto& ep : entities) {
        const Entity& e = *ep;
        if (!e.alive || !lang_is_inscription(e)) continue;
        const Value* gt = p_gtxt  != kNoProp ? prop_of(e, p_gtxt)  : nullptr;
        const Value* lv = p_lang  != kNoProp ? prop_of(e, p_lang)  : nullptr;
        const Value* el = p_lvl    != kNoProp ? prop_of(e, p_lvl)   : nullptr;
        const Value* au = p_auth   != kNoProp ? prop_of(e, p_auth)  : nullptr;
        const Value* tk = p_itk    != kNoProp ? prop_of(e, p_itk)   : nullptr;
        const std::string glyphs = gt && gt->type == ValType::Text ? gt->s : "";
        const std::int64_t lg = lv && lv->type == ValType::Integer ? lv->i : 0;
        const std::int64_t er = el && el->type == ValType::Integer ? el->i : 0;
        const std::int64_t dr = lg > 0 && lang_total.count(lg)
            ? (lang_known[lg] * 1000 / lang_total[lg]) : 0;
        const Value* cl = p_cl   != kNoProp ? prop_of(e, p_cl)   : nullptr;
        const Value* l2 = p_l2   != kNoProp ? prop_of(e, p_l2)   : nullptr;
        const Value* po = p_port != kNoProp ? prop_of(e, p_port) : nullptr;
        o << "INSCRIPTION " << e.id << " lang " << lg
          << " erosion " << er << " deciphered_ratio " << dr
          << " author " << (au && au->type == ValType::Integer ? au->i : 0)
          << " tick " << (tk && tk->type == ValType::Integer ? tk->i : 0)
          << " glyphs=" << glyphs
          << " lang2=" << (l2 && l2->type == ValType::Integer ? l2->i : 0)
          << " portable=" << (po && po->type == ValType::Integer ? po->i : 0)
          << " clause=" << (cl && cl->type == ValType::Text ? cl->s : "")
          << "\n";
    }
    // Phase 12.5 oral literature: words that survived transmission with
    // meter crystallize into proverbs/epics — exported under EPIC / PROVERB
    // with the verse itself (raw_phonemes: every sibling entry of the same
    // concept+tongue rendered in order), the historical chronicle it carries
    // (chronicle_english), its cadence/transmissions and how far it has been
    // MYTHOLOGIZED (myth: strongest `sacred:<concept>` conviction any mind
    // holds, permille — 0 = remembered history, 1000 = revealed scripture).
    {
        const PropId p_genre = lang_prop_id(*this, "oral_genre");
        const PropId p_tcount = lang_prop_id(*this, "transmission_count");
        const PropId p_cad   = lang_prop_id(*this, "poetic_cadence");
        const PropId p_ps2   = lang_prop_id(*this, "phoneme_seed");
        if (p_genre != kNoProp) {
            for (const auto& ep : entities) {
                const Entity& e = *ep;
                if (!e.alive || !lang_is_entry(e)) continue;
                const Value* gv = prop_of(e, p_genre);
                if (!gv || gv->type != ValType::Text || gv->s.empty())
                    continue;
                const Value* tv = p_tcount != kNoProp
                    ? prop_of(e, p_tcount) : nullptr;
                const Value* cv = p_cad != kNoProp
                    ? prop_of(e, p_cad) : nullptr;
                const Value* ck = prop_of(e, p_ckey);
                const std::string cks =
                    ck && ck->type == ValType::Text ? ck->s : "";
                const Value* lv = p_lang != kNoProp ? prop_of(e, p_lang) : nullptr;
                const std::int64_t lg =
                    lv && lv->type == ValType::Integer ? lv->i : 0;
                // the verse: this entry + its siblings (same concept+tongue)
                std::string verse;
                for (const auto& sp : entities) {
                    const Entity& s2 = *sp;
                    if (!s2.alive || !lang_is_entry(s2)) continue;
                    const Value* k2 = prop_of(s2, p_ckey);
                    const Value* l2 = p_lang != kNoProp ? prop_of(s2, p_lang) : nullptr;
                    if (!k2 || k2->type != ValType::Text || k2->s != cks) continue;
                    if ((l2 && l2->type == ValType::Integer ? l2->i : 0) != lg) continue;
                    const Value* pv = p_ps2 != kNoProp ? prop_of(s2, p_ps2) : nullptr;
                    const std::uint32_t ps = pv && pv->type == ValType::Integer
                        ? static_cast<std::uint32_t>(pv->i) : 0;
                    if (!verse.empty()) verse.push_back(' ');
                    verse += render_seed(ps, 2, world_seed);
                }
                std::int64_t myth = 0;
                for (const auto& [mid, mm] : minds_) {
                    (void)mid;
                    for (const std::string& k : {std::string("sacred:") + cks}) {
                        const Belief* b = mm.find_belief(k);
                        if (b) myth = std::max(myth, b->confidence);
                    }
                }
                const auto ci = chronicles.find(cks);
                std::string tag = gv->s;
                for (char& c : tag) c = static_cast<char>(std::toupper(
                    static_cast<unsigned char>(c)));
                o << tag << " entry " << e.id
                  << " concept=" << cks
                  << " raw_phonemes=" << verse
                  << " cadence=" << (cv && cv->type == ValType::Integer
                                     ? cv->i : 0)
                  << " transmissions=" << (tv && tv->type == ValType::Integer
                                           ? tv->i : 0)
                  << " myth=" << myth
                  << " chronicle_english=" << (ci != chronicles.end()
                                              ? ci->second : "")
                  << "\n";
            }
        }
    }
    return o.str();
}

// ---- Phase 12.4: architecture drift ----------------------------------------
// Enclosure weathering is accumulator-exact: decay_accum += pending *
// (1000 - weather_resistance) in permille-ticks; every 64000 worth fires
// one integrity hit. Linear in `pending`, so dt=1 xN and dt=N agree.
// Collapse is causal: debris spawns + occupant impact inside the same
// metabolize event, then the entity dies (integrity 0 reap rule).
void World::terrain_wear_decay(std::int64_t n) {
    if (n <= 0 || wear_cells_.empty() || terrain.traffic_wear.empty())
        return;
    constexpr std::int64_t kFade = 1;    // wear units lost per tick
    for (auto it = wear_cells_.begin(); it != wear_cells_.end();) {
        const std::uint64_t key = *it;
        const int gx = static_cast<std::int32_t>(key >> 32);
        const int gy = static_cast<std::int32_t>(key & 0xFFFFFFFFull);
        int lx = 0, ly = 0;
        if (!terrain.local_cell(gx, gy, lx, ly)) {
            it = wear_cells_.erase(it); continue;
        }
        const std::size_t ci =
            static_cast<std::size_t>(ly) * terrain.w + lx;
        const std::int64_t w =
            static_cast<std::int64_t>(terrain.traffic_wear[ci]) - n * kFade;
        if (w <= 0) {
            terrain.traffic_wear[ci] = 0;
            it = wear_cells_.erase(it);
        } else {
            terrain.traffic_wear[ci] = static_cast<std::int32_t>(w);
            ++it;
        }
    }
}

void World::arch_drift(Entity& e, std::int64_t pending, std::uint64_t now,
                       CausalEvent& ev, std::vector<SpawnSpec>& out) {
    if (pending <= 0 || !e.alive) return;
    const PropId p_da = SIMLAB_PID(props, "decay_accum");
    const PropId p_wres = SIMLAB_PID(props, "weather_resistance");
    const PropId p_integ = SIMLAB_PID(props, "integrity");
    const PropId p_rad = SIMLAB_PID(props, "footprint_radius");
    const PropId p_ratio = SIMLAB_PID(props, "enclosure_ratio");

    if (e.kind_name == SIMLAB_KIND("portal")) {
        // A portal lives only while an enclosure footprint still covers it;
        // collapse or relocation orphans the passage and it fails.
        bool hosted = false;
        for (const auto& ep : entities) {
            const Entity& h = *ep;
            if (!h.alive || h.id == e.id || h.kind_name != SIMLAB_KIND("enclosure") ||
                p_rad == kNoProp) continue;
            const std::int64_t r = prop_int(h, p_rad, 0);
            if (r <= 0) continue;
            const std::int64_t dx = h.pos.x - e.pos.x;
            const std::int64_t dy = h.pos.y - e.pos.y;
            if (dx * dx + dy * dy <= r * r) { hosted = true; break; }
        }
        if (!hosted) {
            e.alive = false;
            spatial.erase(e.id);
            ev.evidence += "portal orphaned; ";
        }
        return;
    }
    if (e.kind_name != SIMLAB_KIND("enclosure") || p_da == kNoProp) return;

    // ---- trespass / consent scan -----------------------------------------
    // Every minded agent inside the footprint is either consented (owner,
    // entry_granted, own claim) or trespassing. `inside:<enc>` marks current
    // presence so alerts fire once per entry, not per tick — idempotent and
    // therefore LOD-exact (positions/beliefs are interact-stable).
    {
        const std::int64_t r = p_rad != kNoProp ? prop_int(e, p_rad, 0) : 0;
        const std::string owns_key = "owns:" +
            std::to_string(static_cast<std::uint64_t>(e.id));
        const std::string inside_key = "inside:" +
            std::to_string(static_cast<std::uint64_t>(e.id));
        const std::string grant_key = "entry_granted:" +
            std::to_string(static_cast<std::uint64_t>(e.id));
        const std::string tres_key = "trespass:" +
            std::to_string(static_cast<std::uint64_t>(e.id));
        // owners = minds holding a Verified-grade owns belief on this shell
        std::vector<EntityId> owners;
        for (const auto& [oid, om] : minds_) {
            (void)om;
            const Belief* b = om.find_belief(owns_key);
            if (b && b->confidence >= 600) owners.push_back(oid);
        }
        if (r > 0 && !owners.empty()) {
            const PropId p_sight = SIMLAB_PID(props, "sight_range");
            for (auto& [mid, mm] : minds_) {
                const Entity* me = find(mid);
                if (!me || !me->alive) continue;
                const std::int64_t dx = me->pos.x - e.pos.x;
                const std::int64_t dy = me->pos.y - e.pos.y;
                const bool inside = dx * dx + dy * dy <= r * r;
                auto ib_it = mm.beliefs.find(inside_key);
                Belief* ib = ib_it != mm.beliefs.end() ? &ib_it->second
                                                       : nullptr;
                if (!inside) {
                    if (ib) { mm.beliefs.erase(inside_key); }
                    continue;
                }
                bool consent = false;
                for (EntityId o : owners)
                    if (o == mid) { consent = true; break; }
                if (!consent) {
                    const Belief* gb = mm.find_belief(grant_key);
                    if (gb && gb->confidence >= 500) consent = true;
                    const Belief* ob = mm.find_belief(owns_key);
                    if (ob && ob->confidence >= 600) consent = true;
                }
                if (consent) {
                    if (ib) mm.beliefs.erase(inside_key);
                    continue;
                }
                if (!ib) {
                    // fresh entry: mark presence + trespass state
                    Belief& in = mm.belief(inside_key);
                    in.confidence = 1000; in.status = BeliefStatus::Observed;
                    in.last_tick = now;
                    Belief& tb = mm.belief(tres_key);
                    tb.confidence = 1000; tb.status = BeliefStatus::Observed;
                    tb.last_tick = now;
                    // alert only if an owner could plausibly see the shell
                    for (EntityId o : owners) {
                        const Entity* oe = find(o);
                        if (!oe || !oe->alive) continue;
                        const std::int64_t ox = oe->pos.x - e.pos.x;
                        const std::int64_t oy = oe->pos.y - e.pos.y;
                        const std::int64_t sight = p_sight != kNoProp
                            ? prop_int(*oe, p_sight, 0) : 0;
                        if (ox * ox + oy * oy <= sight * sight * 4) {
                            AgentMind& om = mind(o);
                            Belief& al = om.belief("alert:" +
                                std::to_string(static_cast<std::uint64_t>(mid)));
                            al.confidence = 800;
                            al.status = BeliefStatus::Observed;
                            al.last_tick = now;
                        }
                    }
                }
            }
        }
    }

    // ---- weathering --------------------------------------------------------
    const std::int64_t wres = p_wres != kNoProp ? prop_int(e, p_wres, 500) : 500;
    const std::int64_t accum0 = p_da != kNoProp ? prop_int(e, p_da, 0) : 0;
    const std::int64_t accum = accum0 + pending * (1000 - wres);
    std::int64_t integ = p_integ != kNoProp ? prop_int(e, p_integ, 500) : 500;
    std::int64_t fires = accum / 64000;
    std::int64_t rem = accum % 64000;
    // per-fire evaluation keeps collapse ordering identical to ticked play
    while (fires-- > 0 && e.alive && integ > 0) {
        integ -= 25;
        if (integ <= 0) break;
    }
    if (integ <= 0 && e.alive) {
        // collapse: shell fails, debris falls, occupants take impact
        const std::int64_t r = p_rad != kNoProp ? prop_int(e, p_rad, 0) : 0;
        static const std::int64_t kOff[3][2] = {
            {1, 0}, {-1, 0}, {0, -1}};
        for (int i = 0; i < 3; ++i) {
            SpawnSpec ds;
            ds.kind = "rubble"; ds.material = e.material;
            ds.pos = {e.pos.x + static_cast<std::int32_t>(
                          r * kOff[i][0] / 2),
                      e.pos.y + static_cast<std::int32_t>(
                          r * kOff[i][1] / 2)};
            PropertyBag pb;
            const PropId pq = SIMLAB_PID(props, "quantity");
            const PropId pi = SIMLAB_PID(props, "integrity");
            if (pq != kNoProp) pb.set(pq, Value::integer(std::max<
                std::int64_t>(1, r / 20)));
            if (pi != kNoProp) pb.set(pi, Value::integer(80));
            ds.props = pb;
            out.push_back(std::move(ds));
        }
        if (p_integ != kNoProp && r > 0) {
            for (EntityId oid : spatial.within(e.pos, r)) {
                if (oid == e.id) continue;
                Entity* oe = find(oid);
                if (!oe || !oe->alive || p_integ == kNoProp) continue;
                const std::int64_t oi = prop_int(*oe, p_integ, 500);
                if (oi <= 0) continue;
                const std::int64_t na = std::max<std::int64_t>(0, oi - 300);
                StateChange ch;
                ch.entity = oid; ch.prop = p_integ; ch.had_before = true;
                ch.before = Value::integer(oi);
                ch.after = Value::integer(na);
                oe->props.set(p_integ, ch.after);
                ev.changes.push_back(ch);
                ev.participants.push_back(oid);
            }
        }
        e.alive = false;
        spatial.erase(e.id);
        ev.participants.push_back(e.id);
        ev.evidence += "enclosure collapsed; ";
        return;
    }
    // write back accumulator remainder + integrity
    if (p_da != kNoProp) {
        const std::int64_t cur = prop_int(e, p_da, 0);
        if (rem != cur) {
            StateChange ch;
            ch.entity = e.id; ch.prop = p_da; ch.had_before = true;
            ch.before = Value::integer(cur);
            ch.after = Value::integer(rem);
            e.props.set(p_da, Value::integer(rem));
            ev.changes.push_back(ch);
        }
    }
    if (p_integ != kNoProp) {
        const std::int64_t cur = prop_int(e, p_integ, 500);
        if (integ != cur) {
            StateChange ch;
            ch.entity = e.id; ch.prop = p_integ; ch.had_before = true;
            ch.before = Value::integer(cur);
            ch.after = Value::integer(integ);
            e.props.set(p_integ, Value::integer(integ));
            ev.changes.push_back(ch);
            ev.participants.push_back(e.id);
        }
    }
}

}  // namespace simlab

namespace simlab {
// public display codec for the viewer/patron layer (same as the codex)
std::string lang_render_word(std::uint32_t seed, std::int64_t syllables, std::uint64_t world_seed) {
    return render_seed(seed, syllables, world_seed);
}
}  // namespace simlab
