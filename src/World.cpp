#include "simlab/World.hpp"
#include "simlab/WorldGen.hpp"   // kGeneratorVersion for save compatibility
#include "simlab/Geometry.hpp"
#include "simlab/Cognition.hpp"
#include <algorithm>
#include <limits>
#include <fstream>
#include <sstream>

namespace simlab {

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
    laws.push_back(std::make_unique<BiologyModel>());
    laws.push_back(std::make_unique<SocialModel>());
    laws.push_back(std::make_unique<LocomotionModel>());
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
    out.touched_ = touched_;
    out.content_error = content_error;
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
    if (n > 0) { metabolize(n); dissipate_heat(n); ecology_tick(n); decay_minds(n); }
}

// Need drift (TODO §10.1): hunger/thirst/fatigue grow with `metabolism`.
// This is World Truth change — it is committed as a causal event so the
// ledger explains WHY a need changed, exactly like any interaction.
World::NeedProps World::need_props() const {
    NeedProps np;
    auto pid = [&](const char* n) -> PropId {
        const PropertyDef* d = props.find_name(n);
        return d ? d->id : kNoProp;
    };
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
    return np;
}

// Per-entity need drift for dt ticks — the SAME math whether the entity is
// updated every tick (active LOD) or caught up in one batch (dormant LOD):
// linear needs, capped at 1000, starvation counted per-tick across the
// window so a mid-window threshold crossing damages exactly the starving
// ticks. integrity 0 reaps immediately inside the same causal event.
bool World::drift_needs(Entity& e, std::uint64_t dt_u, const NeedProps& np,
                        CausalEvent& ev) {
    if (dt_u == 0 || !e.alive) return false;
    const std::int64_t dt = static_cast<std::int64_t>(dt_u);
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
                net += prop_int(e, np.regrow_rate, 0) * env / 1000;
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
                    const std::int64_t moist =
                        std::clamp<std::int64_t>(terrain.moist_at(e.pos), 0, 1000);
                    const std::int64_t temp = terrain.temp_at(e.pos);
                    const std::int64_t dev = temp > 200 ? temp - 200 : 200 - temp;
                    const std::int64_t tf =
                        std::clamp<std::int64_t>(1000 - dev * 4, 0, 1000);
                    env = moist * tf / 1000;
                }
                net += prop_int(e, np.fruit_regrow_rate, 0) * env / 1000;
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
        const std::int64_t rate = dv && (dv->type == ValType::Integer ||
                                         dv->type == ValType::Fixed)
            ? dv->i : 0;
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
        if (drift_needs(e, pending, np, ev)) {
            ev.participants.push_back(e.id);
            any = true;
        }
        e.last_metabolized = now;
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

void World::metabolize(std::int64_t dt) {
    (void)dt;   // per-entity pending time replaces the bulk dt (Phase 9 LOD)
    const NeedProps np = need_props();
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
    const std::int64_t lod_r2 = kLodRadius * kLodRadius;
    auto is_active = [&](const Entity& e) {
        if (minds_.count(e.id)) return true;
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
    }

    CausalEvent ev;
    ev.tick = now;
    ev.action = "metabolize";
    ev.applied_laws.push_back(6);                      // physiology.metabolism
    bool any = false;
    for (const auto& ep : entities) {
        Entity& e = *ep;
        if (!e.alive) continue;
        const std::uint64_t pending = now - e.last_metabolized;
        // LOD: dormant entities wait until enough ticks accrued — cheapest
        // possible reject before ANY property lookup.
        if (lod && pending < static_cast<std::uint64_t>(kDormantStride) &&
            !is_active(e))
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
            if (!maybe && e.material != kNoMaterial &&
                e.material - 1 < mat_need_flags_.size() &&
                mat_need_flags_[e.material - 1])
                maybe = true;
        }
        if (!maybe) { e.last_metabolized = now; continue; }
        if (pending == 0) { e.last_metabolized = now; continue; }
        if (drift_needs(e, pending, np, ev)) {
            ev.participants.push_back(e.id);
            any = true;
        }
        e.last_metabolized = now;
    }
    if (any) {
        ev.evidence = "needs drift with metabolism; " + ev.evidence;
        ledger.append(std::move(ev));
    }
}

void World::dissipate_heat(std::int64_t dt) {
    const PropertyDef* tp = props.find_name("temperature");
    if (!tp) return;
    const PropId temp = tp->id;
    const PropId cond = props.find_name("conductivity") ? props.find_name("conductivity")->id : kNoProp;
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
    auto pid = [&](const char* nm) -> PropId {
        const PropertyDef* d = props.find_name(nm);
        return d ? d->id : kNoProp;
    };
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
    if (!do_drops && !do_sprout && influx.defs().empty()) return;

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

    if (spawns.empty() && ev.changes.empty()) return;
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
    // Pass 1: first supporting law model predicts (deterministic order).
    PredictionResult pred;
    const INaturalLawModel* used = nullptr;
    LawId refused_by = 0;
    std::vector<Condition> refused_conditions;
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
        const PropId cont = props.find_name("contained")
            ? props.find_name("contained")->id : kNoProp;
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
        me->pos = mv.to;
        spatial.move(mv.entity, mv.to);
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
        ev.evidence += "spawned " + std::to_string(nid) + " " + s.kind + "; ";
        res.spawned.push_back(nid);
    }
    // Collective tithe (Phase 8): members of ration_share groups pay a
    // permille cut of what this event yielded into the common store — same
    // causal event, same atomicity as the harvest itself.
    social_tithe(ev, ctx.actor, res.spawned);
    // Actions consume simulated time: the law's declared duration wins,
    // else the caller's tick count (default 1). Time passing also
    // metabolizes needs (ledgered as "metabolize" events).
    const std::int64_t dt = pred.ticks > 0 ? pred.ticks : ctx.parameters.ticks;
    ev.spawned = res.spawned;
    advance_ticks(dt);
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
    // witness learning (TODO §10.1): other living agents whose sight_range
    // covers the actor observe the event and learn the verb + WEAK evidence.
    // Only entities that already have a mind count — an entity becomes an
    // agent by perceiving, not by existing.
    if (const CausalEvent* pev = ledger.find(res.event)) {
        const Entity* ae = find(ctx.actor);
        if (ae) {
            const PropId sight = props.find_name("sight_range")
                ? props.find_name("sight_range")->id : kNoProp;
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
    if (const CausalEvent* sev = ledger.find(res.event))
        social_from_event(*this, ctx.actor, ctx.targets, *sev);
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
    const PropId coll = props.find_name("collective")
        ? props.find_name("collective")->id : kNoProp;
    const PropId ration = props.find_name("ration_share")
        ? props.find_name("ration_share")->id : kNoProp;
    const PropId qty = props.find_name("quantity")
        ? props.find_name("quantity")->id : kNoProp;
    const PropId nut = props.find_name("nutrition")
        ? props.find_name("nutrition")->id : kNoProp;
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
    const PropId thrust = props.find_name("thrust_range")
        ? props.find_name("thrust_range")->id : kNoProp;
    const PropId edgep = props.find_name("edge")
        ? props.find_name("edge")->id : kNoProp;
    const PropId cont = props.find_name("contained")
        ? props.find_name("contained")->id : kNoProp;
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
        mixv(terrain.water_table);
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
    }
    return h;
}

// ---- persistence (TODO §2.8 save/load) ---------------------------------------
// Binary snapshot of World Truth, explicit little-endian. Content references
// (material/deposit/property) are stored by NAME so saves survive registry
// reordering; ids are verified on load. Sections follow state_hash() order.
namespace {

constexpr std::uint32_t kSaveMagic = 0x444C5753u;   // 'SWLD'
constexpr std::uint32_t kSaveFormat = 15;            // v3: event.spawned, belief.self_evidence; v4: rng stream counters; v5: event failed/failure/conditions; v6: mind discovery records; v7: record failed_conditions + sample_entities; v8: entity genome; v9: social tie weights; v10: entity last_metabolized; v11: gen_cells_ + mind memory/visited_cells; v12: ledger archive state; v13: memory/discovery engrams (Ebbinghaus); v14: neuro state, panic_relief, born_memories; v15: recent_witnessed, sublimation_relief, ostracism_accum

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
    }
    // rng stream counters; v5: event failed/failure/conditions (v4): stochastic laws must replay after load
    w.put(seed);
    w.put(gen_sea_level);
    w.put(static_cast<std::uint32_t>(rngs_.size()));
    for (const auto& [name, rg] : rngs_) { w.str(name); w.put(rg->state()); }
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
    entities.clear(); index_.clear(); touched_.clear(); spatial.clear(); relations.clear(); social_.clear(); ledger.clear(); minds_.clear();
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
            }
            minds_[nid] = std::move(m);
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
    r.f.close();
    if (!r.ok()) return fail("read failed");
    return true;
}

}  // namespace simlab
