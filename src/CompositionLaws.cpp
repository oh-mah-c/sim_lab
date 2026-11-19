// CompositionLaws.cpp — Phase 4: Universal Composition.
//
// action="assemble" combines >=2 reachable entities into a composite
// structure; action="disassemble" reverses it. There is NO recipe table:
// the solver evaluates feasibility purely from properties, and the
// product's properties are *derived* from its parts by per-group
// computation models (see TODO §8.1-8.3).
//
// Structure is REAL: parts stay alive, flagged `contained`, and linked to
// the composite by `part_of` relation edges (committed through the causal
// pipeline as RelationChange/SpawnSpec::rel_from). Disassembly restores
// them exactly — no mass or props are lost.
//
// Derivation models (all integer, documented — TODO §8.2):
//   sum     mass, thermal_mass, length            — extensive quantities add
//   min     integrity                              — weakest link governs
//   max     flammability, edge                     — worst/best case dominates
//   wavg    hardness, brittleness, conductivity,   — intensive: mass-weighted
//           temperature
// Emergent (TODO §8.3): `thrust_range` exists ONLY on the composite, never
// on parts — it is reach+edge organized into a new capability.
//
// Constraints (solver): >=2 parts, all in reach, integrity >= floor,
// total mass <= actor `max_load`, energy budget, <= kMaxParts.

#include "simlab/Laws.hpp"
#include "simlab/World.hpp"
#include "LawUtil.hpp"
#include "simlab/Geometry.hpp"
#include <algorithm>
#include <cstdint>
#include <sstream>

namespace simlab {
namespace {

constexpr std::int64_t kDefaultReach = 64;
constexpr std::int64_t kFragileFloor = 20;        // min integrity to bind
constexpr std::int64_t kDefaultMaxLoad = 500000;  // milli-units, actor prop `max_load`
constexpr std::size_t kMaxParts = 8;              // solver bound on structure size

PropId prop_id(const World& w, const char* name) {
    const PropertyDef* d = w.props.find_name(name);
    return d ? d->id : kNoProp;
}

bool prop_bool(const World& w, const Entity& e, PropId p) {
    const Value* v = w.prop_of(e, p);
    return v && v->type == ValType::Boolean && v->b;
}

bool in_reach(const World& w, const Entity& actor, const Entity& target,
              PropId reach_pid) {
    const std::int64_t reach = reach_pid != kNoProp
        ? w.prop_int(actor, reach_pid, kDefaultReach) : kDefaultReach;
    return dist2(actor.pos, target.pos) <= reach * reach;
}

bool charge_or_fail(const World& /*w*/, const Entity& actor, PropId energy_pid,
                    std::int64_t cost, PredictionResult& r) {
    if (energy_pid == kNoProp || cost <= 0) return true;
    const Value* ev = actor.props.get(energy_pid);
    if (!ev) return true;                        // unbounded actor
    if (ev->i < cost) {
        r.failure = "insufficient energy: need " + std::to_string(cost) +
                    ", have " + std::to_string(ev->i);
        return false;
    }
    StateChange ch;
    ch.entity = actor.id; ch.prop = energy_pid; ch.had_before = true;
    ch.before = Value::integer(ev->i);
    ch.after = Value::integer(ev->i - cost);
    r.changes.push_back(ch);
    return true;
}

void set_change_v(PredictionResult& r, EntityId id, PropId prop,
                  Value before, Value after) {
    StateChange ch;
    ch.entity = id; ch.prop = prop; ch.had_before = true;
    ch.before = std::move(before);
    ch.after = std::move(after);
    r.changes.push_back(ch);
}

void set_change(PredictionResult& r, EntityId id, PropId prop,
                std::int64_t before, std::int64_t after) {
    set_change_v(r, id, prop, Value::integer(before), Value::integer(after));
}

std::int64_t wavg(std::int64_t num, std::int64_t den) {
    return den > 0 ? num / den : 0;
}

}  // namespace

LawId CompositionModel::id() const { return 5; }

bool CompositionModel::supports(const World& w, const InteractionContext& ctx) const {
    (void)w;
    if (ctx.targets.empty()) return false;
    const std::string& a = ctx.parameters.action;
    if (a == "assemble") return ctx.targets.size() >= 2;
    if (a == "disassemble" || a == "wield" || a == "unwield") return true;
    if (a == "carve") return ctx.targets.size() == 1;
    return false;
}

// ---- assemble ------------------------------------------------------------------
static PredictionResult predict_assemble(const World& w,
                                         const InteractionContext& ctx) {
    PredictionResult r;
    r.law = 5; r.law_name = "physical.compose";
    const Entity* actor = w.find(ctx.actor);
    if (!actor || !actor->alive) { r.failure = "no living actor"; return r; }

    const PropId reach_pid = prop_id(w, "reach");
    const PropId energy_pid = prop_id(w, "energy");
    const PropId contained_pid = prop_id(w, "contained");
    const PropId mass_pid = prop_id(w, "mass");
    const PropId integ_pid = prop_id(w, "integrity");
    const PropId hard_pid = prop_id(w, "hardness");
    const PropId brit_pid = prop_id(w, "brittleness");
    const PropId flamm_pid = prop_id(w, "flammability");
    const PropId tmass_pid = prop_id(w, "thermal_mass");
    const PropId cond_pid = prop_id(w, "conductivity");
    const PropId temp_pid = prop_id(w, "temperature");
    const PropId len_pid = prop_id(w, "length");
    const PropId edge_pid = prop_id(w, "edge");
    const PropId thrust_pid = prop_id(w, "thrust_range");
    const PropId madeof_pid = prop_id(w, "made_of");
    const PropId maxload_pid = prop_id(w, "max_load");
    const PropId gt_pid = prop_id(w, "geom_type");
    const PropId gl_pid = prop_id(w, "geom_len");
    const PropId gw_pid = prop_id(w, "geom_wid");
    const PropId gk_pid = prop_id(w, "geom_thk");
    const PropId tensile_pid = prop_id(w, "tensile");
    const PropId joint_pid = prop_id(w, "joint_type");
    const PropId motor_pid = prop_id(w, "mind_motor_precision");
    if (mass_pid == kNoProp || integ_pid == kNoProp) {
        r.failure = "composition requires mass/integrity properties";
        return r;
    }

    struct Part {
        EntityId id; MaterialId material; Vec2 pos;
        std::int64_t mass, integ, hard, brit, flamm, tmass, cond, temp, len, edge;
        std::int64_t bind_r, tensile;
    };
    std::vector<Part> parts;
    std::int64_t total_mass = 0, min_integ = INT64_MAX;
    std::int64_t w_hard = 0, w_brit = 0, w_cond = 0, w_temp = 0;
    std::int64_t sum_tmass = 0, sum_len = 0, max_flamm = 0, max_edge = 0;
    Vec2 centroid{0, 0};

    for (EntityId tid : ctx.targets) {
        const Entity* t = w.find(tid);
        if (!t || !t->alive) continue;
        if (contained_pid != kNoProp && prop_bool(w, *t, contained_pid)) {
            r.failure = "part " + std::to_string(tid) + " already contained";
            return r;
        }
        if (t->kind_name == "composite") {
            r.failure = "cannot nest composites (part " +
                        std::to_string(tid) + ")";
            return r;
        }
        if (!in_reach(w, *actor, *t, reach_pid)) {
            r.failure = "part " + std::to_string(tid) + " out of reach";
            return r;
        }
        Part p;
        p.id = tid; p.material = t->material; p.pos = t->pos;
        p.mass  = w.prop_int(*t, mass_pid, 1000);
        p.integ  = w.prop_int(*t, integ_pid, 500);
        p.hard   = hard_pid  != kNoProp ? w.prop_int(*t, hard_pid, 300)  : 0;
        p.brit   = brit_pid  != kNoProp ? w.prop_int(*t, brit_pid, 400)  : 0;
        p.flamm  = flamm_pid != kNoProp ? w.prop_int(*t, flamm_pid, 0)   : 0;
        p.tmass  = tmass_pid != kNoProp ? w.prop_int(*t, tmass_pid, 1000): 0;
        p.cond   = cond_pid  != kNoProp ? w.prop_int(*t, cond_pid, 100)  : 0;
        p.temp   = temp_pid  != kNoProp ? w.prop_int(*t, temp_pid, 200)  : 0;
        p.len    = len_pid   != kNoProp ? w.prop_int(*t, len_pid, 0)     : 0;
        p.edge   = edge_pid  != kNoProp ? w.prop_int(*t, edge_pid, 0)    : 0;
        // bind radius: half the largest primitive dim; entities without
        // geometry get a small default so they can still be bound
        if (gt_pid != kNoProp && w.prop_int(*t, gt_pid, 0) > 0) {
            p.bind_r = std::max({w.prop_int(*t, gl_pid, 0),
                                 gw_pid != kNoProp ? w.prop_int(*t, gw_pid, 0) : 0,
                                 gk_pid != kNoProp ? w.prop_int(*t, gk_pid, 0) : 0}) / 2;
        } else p.bind_r = 50;
        p.tensile = tensile_pid != kNoProp ? w.prop_int(*t, tensile_pid, 500) : 500;
        if (p.integ < kFragileFloor) {
            r.failure = "part " + std::to_string(tid) +
                        " too fragile to bind (integrity " +
                        std::to_string(p.integ) + " < " +
                        std::to_string(kFragileFloor) + ")";
            return r;
        }
        total_mass += p.mass;
        min_integ = std::min(min_integ, p.integ);
        w_hard += p.hard * p.mass;  w_brit += p.brit * p.mass;
        w_cond += p.cond * p.mass;  w_temp += p.temp * p.mass;
        sum_tmass += p.tmass; sum_len += p.len;
        max_flamm = std::max(max_flamm, p.flamm);
        max_edge  = std::max(max_edge, p.edge);
        centroid.x += p.pos.x; centroid.y += p.pos.y;
        parts.push_back(p);
    }
    if (parts.size() < 2) { r.failure = "need at least 2 parts"; return r; }
    // geometric fit (§8.5.3): socket-to-socket distance decides whether a
    // binding can span the gap — surfaces must nearly touch. Parts without
    // geometry fall back to the coarse bind-radius rule.
    constexpr std::int64_t kSocketGap2 = 100LL * 100;
    constexpr std::int64_t kBindGap = 60;
    std::int64_t min_joint = INT64_MAX;
    for (std::size_t i = 0; i < parts.size(); ++i)
        for (std::size_t j = i + 1; j < parts.size(); ++j) {
            const std::int64_t sg = socket_gap(w, parts[i].id, parts[j].id);
            bool fits;
            if (sg != INT64_MAX) fits = sg <= kSocketGap2;
            else {
                const std::int64_t dx = parts[i].pos.x - parts[j].pos.x;
                const std::int64_t dy = parts[i].pos.y - parts[j].pos.y;
                const std::int64_t reach = parts[i].bind_r + parts[j].bind_r + kBindGap;
                fits = dx * dx + dy * dy <= reach * reach;
            }
            if (!fits) {
                r.failure = "parts " + std::to_string(parts[i].id) + " and " +
                            std::to_string(parts[j].id) + " too far apart to bind";
                return r;
            }
        }

    // joint strength (§8.5.3): binding type comes from the caller's note
    // ("bind"|"wedge"|"pierce"|"balance"), joint = weakest tensile of the
    // pair scaled by the binding factor. pierce needs a sharp edge.
    std::string jtype = ctx.parameters.note.empty() ? "bind" : ctx.parameters.note;
    static const std::map<std::string, std::int64_t> kJointFactor = {
        {"bind", 800}, {"wedge", 900}, {"pierce", 600}, {"balance", 400}};
    auto fit = kJointFactor.find(jtype);
    if (fit == kJointFactor.end()) { r.failure = "unknown binding type: " + jtype; return r; }
    if (jtype == "pierce" && max_edge < 300) {
        r.failure = "pierce needs an edge >= 300"; return r;
    }
    for (std::size_t i = 0; i < parts.size(); ++i)
        for (std::size_t j = i + 1; j < parts.size(); ++j) {
            const std::int64_t jt = std::min(parts[i].tensile, parts[j].tensile)
                                  * fit->second / 1000;
            min_joint = std::min(min_joint, jt);
        }
    // the structure fails at its weakest link: part integrity OR joint
    min_integ = std::min(min_integ, min_joint);
    if (parts.size() > kMaxParts) {
        r.failure = "too many parts: " + std::to_string(parts.size()) +
                    " > " + std::to_string(kMaxParts);
        return r;
    }

    const std::int64_t max_load = maxload_pid != kNoProp
        ? w.prop_int(*actor, maxload_pid, kDefaultMaxLoad) : kDefaultMaxLoad;
    if (total_mass > max_load) {
        r.failure = "assembly too heavy: " + std::to_string(total_mass) +
                    " > max_load " + std::to_string(max_load);
        return r;
    }

    const std::int64_t cost = 500 + total_mass / 100;
    if (!charge_or_fail(w, *actor, energy_pid, cost, r)) return r;

    centroid.x /= static_cast<std::int64_t>(parts.size());
    centroid.y /= static_cast<std::int64_t>(parts.size());

    PropertyBag pb;
    auto seti = [&](PropId p, std::int64_t v) { if (p != kNoProp) pb.set(p, Value::integer(v)); };
    seti(mass_pid, total_mass);
    // Nhịp 4c: a master's binding preserves more of the weakest link's
    // soundness; a clumsy artisan leaves a fragile composite (min 1 —
    // integrity 0 would mean the product dies at birth).
    const std::int64_t motor0 = motor_pid != kNoProp
        ? w.prop_int(*actor, motor_pid, 1000) : 1000;
    // Phase 10: lost arms / low blood degrade craft precision the same
    // way they degrade strike force.
    const std::int64_t motor = motor0 * lawutil::anatomy_motor(w, *actor) / 1000;
    seti(integ_pid, std::clamp<std::int64_t>(min_integ * motor / 1000, 1, 1000));
    if (hard_pid != kNoProp) seti(hard_pid, wavg(w_hard, total_mass));
    if (brit_pid != kNoProp) seti(brit_pid, wavg(w_brit, total_mass));
    if (flamm_pid != kNoProp) seti(flamm_pid, max_flamm);
    if (tmass_pid != kNoProp) seti(tmass_pid, sum_tmass);
    if (cond_pid != kNoProp) seti(cond_pid, wavg(w_cond, total_mass));
    if (temp_pid != kNoProp) seti(temp_pid, wavg(w_temp, total_mass));
    if (len_pid != kNoProp) seti(len_pid, sum_len);
    if (edge_pid != kNoProp) seti(edge_pid, max_edge);

    // emergent capability: edge organized with length -> thrust_range.
    std::int64_t thrust = 0;
    if (thrust_pid != kNoProp && max_edge > 0 && sum_len > 0)
        thrust = sum_len * (1000 + max_edge) / 1000;
    if (thrust > 0) seti(thrust_pid, thrust);
    if (joint_pid != kNoProp)
        pb.set(joint_pid, Value::text(jtype));
    if (madeof_pid != kNoProp) {
        std::ostringstream m;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (i) m << ',';
            m << parts[i].id;
            if (parts[i].material != kNoMaterial)
                m << ':' << static_cast<unsigned long long>(parts[i].material);
        }
        pb.set(madeof_pid, Value::text(m.str()));
    }

    SpawnSpec s;
    s.kind = "composite"; s.material = kNoMaterial; s.pos = centroid; s.props = pb;
    // wire part_of edges once the composite id exists
    for (const Part& p : parts) s.rel_from.push_back({"part_of", p.id});
    r.spawns.push_back(s);

    // parts stay alive but become contained (not independently interactable)
    for (const Part& p : parts) {
        if (contained_pid != kNoProp)
            set_change_v(r, p.id, contained_pid,
                         Value::boolean(false), Value::boolean(true));
    }

    r.possible = true;
    r.ticks = static_cast<std::int64_t>(parts.size());
    std::ostringstream n;
    n << "assemble " << parts.size() << " parts -> composite mass=" << total_mass
      << " min_integ=" << min_integ << " len=" << sum_len << " edge=" << max_edge
      << " thrust_range=" << thrust << " cost=" << cost;
    r.notes.push_back(n.str());
    return r;
}

// ---- disassemble ---------------------------------------------------------------
static PredictionResult predict_disassemble(const World& w,
                                            const InteractionContext& ctx) {
    PredictionResult r;
    r.law = 5; r.law_name = "physical.compose";
    const Entity* actor = w.find(ctx.actor);
    if (!actor || !actor->alive) { r.failure = "no living actor"; return r; }

    const PropId reach_pid = prop_id(w, "reach");
    const PropId energy_pid = prop_id(w, "energy");
    const PropId contained_pid = prop_id(w, "contained");
    const PropId integ_pid = prop_id(w, "integrity");
    if (integ_pid == kNoProp) { r.failure = "no integrity property"; return r; }

    // find the composite among targets (must carry part_of edges or made_of)
    EntityId comp = kNoEntity;
    for (EntityId tid : ctx.targets) {
        const Entity* t = w.find(tid);
        if (!t || !t->alive) continue;
        if (t->kind_name == "composite" ||
            (prop_id(w, "made_of") != kNoProp &&
             w.prop_of(*t, prop_id(w, "made_of")) != nullptr)) { comp = tid; break; }
    }
    if (comp == kNoEntity) { r.failure = "no composite target"; return r; }
    const Entity* c = w.find(comp);
    if (!in_reach(w, *actor, *c, reach_pid)) {
        r.failure = "composite out of reach"; return r;
    }

    // parts = entities with part_of -> composite
    std::vector<EntityId> parts;
    for (const Relation& rel : w.relations.edges())
        if (rel.type == "part_of" && rel.b == comp && w.find(rel.a) && w.find(rel.a)->alive)
            parts.push_back(rel.a);
    std::sort(parts.begin(), parts.end());
    if (parts.empty()) { r.failure = "composite has no parts"; return r; }

    const std::int64_t cost = 200 + static_cast<std::int64_t>(parts.size()) * 50;
    if (!charge_or_fail(w, *actor, energy_pid, cost, r)) return r;

    // destroy composite, free parts
    const std::int64_t ci = w.prop_int(*c, integ_pid, 500);
    set_change(r, comp, integ_pid, ci, 0);
    for (EntityId pid_ : parts) {
        if (contained_pid != kNoProp) {
            const std::int64_t before = prop_bool(w, *w.find(pid_), contained_pid) ? 1 : 0;
            StateChange ch;
            ch.entity = pid_; ch.prop = contained_pid; ch.had_before = true;
            ch.before = Value::boolean(before != 0);
            ch.after = Value::boolean(false);
            r.changes.push_back(ch);
        }
        r.rel_changes.push_back({pid_, "part_of", comp, false});
    }

    r.possible = true;
    r.ticks = static_cast<std::int64_t>(parts.size());
    r.notes.push_back("disassemble composite " + std::to_string(comp) +
                      " -> " + std::to_string(parts.size()) + " parts cost=" +
                      std::to_string(cost));
    return r;
}

// ---- wield / unwield -------------------------------------------------------------
// Generic equipment: `wielding` relation actor->tool. Laws read the tool's
// emergent props (thrust_range extends reach, edge sharpens force) through
// World::tool_bonus — no tool is named anywhere.
static PredictionResult predict_wield(const World& w, const InteractionContext& ctx,
                                      bool unwield) {
    PredictionResult r;
    r.law = 5; r.law_name = "physical.compose";
    const Entity* actor = w.find(ctx.actor);
    if (!actor || !actor->alive) { r.failure = "no living actor"; return r; }
    const PropId reach_pid = prop_id(w, "reach");
    const PropId energy_pid = prop_id(w, "energy");
    const PropId contained_pid = prop_id(w, "contained");
    std::size_t n = 0;
    for (EntityId tid : ctx.targets) {
        const Entity* t = w.find(tid);
        if (!t || !t->alive || tid == ctx.actor) continue;
        if (contained_pid != kNoProp && prop_bool(w, *t, contained_pid)) continue;
        if (!unwield && !in_reach(w, *actor, *t, reach_pid)) {
            r.failure = "tool " + std::to_string(tid) + " out of reach"; return r;
        }
        r.rel_changes.push_back({ctx.actor, "wielding", tid, !unwield});
        ++n;
    }
    if (n == 0) { r.failure = unwield ? "nothing wielded" : "no wieldable target"; return r; }
    if (!charge_or_fail(w, *actor, energy_pid, unwield ? 50 : 100, r)) return r;
    r.possible = true; r.ticks = 1;
    r.notes.push_back(std::string(unwield ? "unwield " : "wield ") +
                      std::to_string(n) + " tool(s)");
    return r;
}


// ---- carve ---------------------------------------------------------------------
// Dynamic reshaping (§8.5): physical work removes material and CHANGES the
// primitive — shape is runtime state, not locked to the material def.
// Mass is rescaled by the volume ratio so accounting stays conserved.
static PredictionResult predict_carve(const World& w, const InteractionContext& ctx) {
    PredictionResult r;
    r.law = 5; r.law_name = "physical.compose";
    const Entity* actor = w.find(ctx.actor);
    if (!actor || !actor->alive) { r.failure = "no living actor"; return r; }
    const Entity* t = w.find(ctx.targets.front());
    if (!t || !t->alive) { r.failure = "no target"; return r; }

    const PropId reach_pid = prop_id(w, "reach");
    const PropId energy_pid = prop_id(w, "energy");
    const PropId contained_pid = prop_id(w, "contained");
    const PropId gt = prop_id(w, "geom_type");
    const PropId gl = prop_id(w, "geom_len");
    const PropId gw = prop_id(w, "geom_wid");
    const PropId gk = prop_id(w, "geom_thk");
    const PropId mass_pid = prop_id(w, "mass");
    if (gt == kNoProp || gl == kNoProp) { r.failure = "no geometry props"; return r; }
    if (contained_pid != kNoProp && prop_bool(w, *t, contained_pid)) {
        r.failure = "target is contained in a structure"; return r;
    }
    if (!in_reach(w, *actor, *t, reach_pid)) {
        r.failure = "target out of reach"; return r;
    }
    const std::int64_t type = w.prop_int(*t, gt, 0);
    if (type == 0) { r.failure = "target has no primitive geometry"; return r; }
    const std::int64_t L = w.prop_int(*t, gl, 0);
    const std::int64_t W = gw != kNoProp ? w.prop_int(*t, gw, 0) : 0;
    const std::int64_t K = gk != kNoProp ? w.prop_int(*t, gk, 0) : 0;
    const GeomType pt = static_cast<GeomType>(type);
    const std::int64_t E = ctx.parameters.force > 0 ? ctx.parameters.force : 1000;

    // removal per primitive kind; floors keep the primitive valid
    std::int64_t nL = L, nW = W, nK = K;
    if (pt == GeomType::Rod) {
        nL = std::max<std::int64_t>(std::max<std::int64_t>(W * 2, 20), L - std::max<std::int64_t>(1, E / 2000));
    } else if (pt == GeomType::Plate) {
        nK = std::max<std::int64_t>(10, K - std::max<std::int64_t>(1, E / 4000));
    } else {  // Blob: shave all dims uniformly
        const std::int64_t d = std::max<std::int64_t>(1, E / 6000);
        nL = std::max<std::int64_t>(20, L - d);
        nW = std::max<std::int64_t>(20, W - d);
        nK = std::max<std::int64_t>(20, K - d);
    }
    if (nL == L && nW == W && nK == K) {
        r.failure = "carve too weak to remove material"; return r;
    }
    const std::int64_t v0 = prim_volume(pt, L, W, K);
    const std::int64_t v1 = prim_volume(pt, nL, nW, nK);
    if (v1 <= 0 || v1 >= v0) { r.failure = "invalid carve result"; return r; }

    // mass conservation: entity mass scales with remaining volume
    if (mass_pid != kNoProp) {
        const std::int64_t m0 = w.prop_int(*t, mass_pid, 1000);
        const std::int64_t m1 = std::max<std::int64_t>(1, m0 * v1 / v0);
        set_change(r, t->id, mass_pid, m0, m1);
    }
    if (nL != L) set_change(r, t->id, gl, L, nL);
    if (gw != kNoProp && nW != W) set_change(r, t->id, gw, W, nW);
    if (gk != kNoProp && nK != K) set_change(r, t->id, gk, K, nK);

    const std::int64_t cost = 100 + E / 5000;
    if (!charge_or_fail(w, *actor, energy_pid, cost, r)) return r;
    r.possible = true; r.ticks = 1;
    std::ostringstream n;
    n << "carve type=" << type << " " << L << "x" << W << "x" << K
      << " -> " << nL << "x" << nW << "x" << nK << " cost=" << cost;
    r.notes.push_back(n.str());
    return r;
}

PredictionResult CompositionModel::predict(const World& w,
                                           const InteractionContext& ctx) const {
    const std::string& a = ctx.parameters.action;
    if (a == "disassemble") return predict_disassemble(w, ctx);
    if (a == "wield") return predict_wield(w, ctx, false);
    if (a == "unwield") return predict_wield(w, ctx, true);
    if (a == "carve") return predict_carve(w, ctx);
    return predict_assemble(w, ctx);
}

std::unique_ptr<INaturalLawModel> CompositionModel::clone() const {
    return std::make_unique<CompositionModel>();
}

}  // namespace simlab
