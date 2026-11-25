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
#include "simlab/Cognition.hpp"
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
    static thread_local LitPidCache c_;
    return c_.get(w.props, name);
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
    if (a == "enclose") return ctx.targets.size() >= 4;
    if (a == "bypass_barrier") return ctx.targets.size() == 1;
    if (a == "repair") return ctx.targets.size() >= 2;
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
        if (t->kind_name == SIMLAB_KIND("composite")) {
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

    // Phase 12.6 war channels: a structure is as keen/heavy/protective/
    // energetic as its strongest part (max), coatings ride along.
    {
        static const char* const kMaxProps[] = {
            "edge_sharpness", "blunt_mass", "elasticity",
            "armor_penetration_resist", "volatility", "expansion_force",
            "coat_toxin", "coat_strain"};
        for (const char* nm : kMaxProps) {
            const PropId q = prop_id(w, nm);
            if (q == kNoProp) continue;
            std::int64_t mx = 0;
            for (const Part& pp : parts)
                if (const Entity* pe = w.find(pp.id)) mx = std::max(mx, w.prop_int(*pe, q, 0));
            if (mx > 0) seti(q, mx);
        }
        // EMERGENT launcher: a flexing frame (elasticity) strung with a
        // high-tension part (tensile) stores strain energy neither has
        // alone — launch_impulse ∝ elasticity x tension x builder precision.
        const PropId el_p = prop_id(w, "elasticity"), li_p = prop_id(w, "launch_impulse");
        if (el_p != kNoProp && li_p != kNoProp && tensile_pid != kNoProp) {
            std::int64_t best = 0;
            for (const Part& fa : parts) {
                const Entity* fe = w.find(fa.id);
                const std::int64_t el = fe ? w.prop_int(*fe, el_p, 0) : 0;
                if (el < 300) continue;
                for (const Part& sb : parts) {
                    if (sb.id == fa.id) continue;
                    const Entity* se = w.find(sb.id);
                    const std::int64_t tn = se ? w.prop_int(*se, tensile_pid, 0) : 0;
                    if (tn < 300) continue;
                    // frame mass scales stored energy: big frames = siege engines
                    best = std::max(best, el * tn / 1000 * (1000 + fa.mass / 10) / 1000);
                }
            }
            if (best > 0) seti(li_p, best * motor / 1000);
        }
    }

    // Primordial maritime: buoyancy EMERGES from the parts' real densities.
    // lift = sum of mass x (water - density) / density over the parts (a
    // dense part subtracts); the swell a hull survives grows with how well
    // its seams are sealed (best `waterproofing` among the parts) and with
    // how many members brace it. Raft (logs + sinew) -> calm water only;
    // hide-sealed hull -> open sea. No craft names exist in code.
    if (w.ruleset & World::kRulePrimordial) {
        const PropId fc_p = prop_id(w, "float_capacity"), wr_p = prop_id(w, "wave_rating"),
                     wp_p = prop_id(w, "waterproofing"), dens_p = prop_id(w, "density");
        const MaterialDef* wmd = w.materials.find_name("water");
        std::int64_t D = 1000000;
        if (wmd && dens_p != kNoProp) if (auto it = wmd->props.find(dens_p); it != wmd->props.end() && it->second.i > 0) D = it->second.i;
        std::int64_t lift = 0, best_wp = 0;
        for (const Part& pp : parts) {
            const Entity* pe = w.find(pp.id);
            if (!pe) continue;
            const std::int64_t d = dens_p != kNoProp ? std::max<std::int64_t>(1, w.prop_int(*pe, dens_p, D)) : D;
            lift += pp.mass * (D - d) / d;
            if (wp_p != kNoProp) best_wp = std::max(best_wp, w.prop_int(*pe, wp_p, 0));
        }
        if (fc_p != kNoProp && lift > 0) {
            seti(fc_p, lift);
            if (wr_p != kNoProp)
                seti(wr_p, std::clamp<std::int64_t>(
                    150 + std::min<std::int64_t>(6, std::int64_t(parts.size())) * 25 + best_wp * 6 / 10, 0, 1000) *
                    motor / 1000);
        }
    }

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
        if (t->kind_name == SIMLAB_KIND("composite") ||
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
        // a living organism (a species individual: rooted plant or animal)
        // is not a hand tool — branches/stones must be broken off first.
        if (!unwield && (w.ruleset & World::kRulePrimordial) &&
            w.species.find_name(t->kind_name.str())) {
            r.failure = "target " + std::to_string(tid) + " is a living organism"; continue;
        }
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

PredictionResult predict_enclose(const World& w,
                                 const InteractionContext& ctx);
PredictionResult predict_bypass_barrier(const World& w,
                                        const InteractionContext& ctx);
PredictionResult predict_repair(const World& w,
                                const InteractionContext& ctx);

PredictionResult CompositionModel::predict(const World& w,
                                           const InteractionContext& ctx) const {
    const std::string& a = ctx.parameters.action;
    if (a == "disassemble") return predict_disassemble(w, ctx);
    if (a == "enclose")      return predict_enclose(w, ctx);
    if (a == "bypass_barrier") return predict_bypass_barrier(w, ctx);
    if (a == "repair")       return predict_repair(w, ctx);
    if (a == "wield") return predict_wield(w, ctx, false);
    if (a == "unwield") return predict_wield(w, ctx, true);
    if (a == "carve") return predict_carve(w, ctx);
    return predict_assemble(w, ctx);
}

std::unique_ptr<INaturalLawModel> CompositionModel::clone() const {
    return std::make_unique<CompositionModel>();
}



// ---- Phase 12.4: architecture -----------------------------------------------
// "enclose" raises a roofed space from loose structural parts; "bypass_
// barrier" is the quiet-entry contest against access_barrier+maze
// complexity; "repair" patches a shell from spare material. No building
// types exist — walls are rods, roofs are slabs, doors are portals, and
// every verdict comes from material physics.

namespace {

struct ArchPart {
    EntityId id;
    MaterialId material;
    Vec2 pos;
    std::int64_t mass, integ, geom_len, geom_wid, geom_thk, geom_type;
    std::int64_t lbs, span, insul, wres, hard, flamm;
};

std::int64_t arch_mat_prop(const World& w, MaterialId m, const char* name,
                           std::int64_t fb) {
    if (m == kNoMaterial) return fb;
    const MaterialDef* d = w.materials.find(m);
    if (!d) return fb;
    const PropertyDef* pd = w.props.find_name(name);
    if (!pd) return fb;
    const auto it = d->props.find(pd->id);
    return it != d->props.end() && it->second.type == ValType::Integer
        ? it->second.i : fb;
}

bool arch_load_part(const World& w, const Entity& t, ArchPart& p,
                    const char* failbuf, std::string& fail) {
    (void)w; (void)failbuf;
    if (!t.alive) { fail = "dead part"; return false; }
    const PropId contained = prop_id(w, "contained");
    if (contained != kNoProp && prop_bool(w, t, contained)) {
        fail = "part already contained"; return false;
    }
    if (t.kind_name == SIMLAB_KIND("enclosure") || t.kind_name == SIMLAB_KIND("portal")) {
        fail = "cannot build from a finished space"; return false;
    }
    p.id = t.id; p.material = t.material; p.pos = t.pos;
    p.mass   = w.prop_int(t, prop_id(w, "mass"), 1000);
    p.integ  = w.prop_int(t, prop_id(w, "integrity"), 500);
    p.geom_type = w.prop_int(t, prop_id(w, "geom_type"), 0);
    p.geom_len = w.prop_int(t, prop_id(w, "geom_len"), 0);
    p.geom_wid = w.prop_int(t, prop_id(w, "geom_wid"), 0);
    p.geom_thk = w.prop_int(t, prop_id(w, "geom_thk"), 0);
    p.lbs   = arch_mat_prop(w, t.material, "load_bearing_strength", 300);
    p.span  = arch_mat_prop(w, t.material, "max_span", 0);
    p.insul = arch_mat_prop(w, t.material, "thermal_insulation", 300);
    p.wres  = arch_mat_prop(w, t.material, "weather_resistance", 400);
    p.hard  = w.prop_int(t, prop_id(w, "hardness"), 300);
    p.flamm = w.prop_int(t, prop_id(w, "flammability"), 200);
    return true;
}

}  // namespace

PredictionResult predict_enclose(const World& w,
                                 const InteractionContext& ctx) {
    PredictionResult r; r.law = 5; r.law_name = "physical.compose";
    const PropId contained = prop_id(w, "contained");
    const PropId energy = prop_id(w, "energy");
    const PropId reach = prop_id(w, "reach");
    const PropId p_rad = prop_id(w, "footprint_radius");
    const PropId p_ratio = prop_id(w, "enclosure_ratio");
    const PropId p_ins = prop_id(w, "insulation_factor");
    const PropId p_hab = prop_id(w, "habitable_score");
    const PropId p_cx = prop_id(w, "spatial_complexity");
    const PropId p_bar = prop_id(w, "access_barrier");
    const PropId p_style = prop_id(w, "arch_style");
    const PropId p_da = prop_id(w, "decay_accum");
    const PropId p_wres = prop_id(w, "weather_resistance");
    const PropId p_integ = prop_id(w, "integrity");
    const PropId p_temp = prop_id(w, "temperature");
    if (ctx.targets.size() < 4) {
        r.failure = "enclosure needs at least 4 parts"; return r;
    }
    const Entity* actor = w.find(ctx.actor);
    if (!actor || !actor->alive) { r.failure = "no living actor"; return r; }

    std::vector<ArchPart> parts;
    std::int64_t total_mass = 0;
    Vec2 centroid{0, 0};
    for (EntityId tid : ctx.targets) {
        const Entity* t = w.find(tid);
        if (!t) { r.failure = "missing part"; return r; }
        ArchPart p;
        std::string why;
        if (!arch_load_part(w, *t, p, "", why)) {
            r.failure = "part " + std::to_string(tid) + ": " + why;
            return r;
        }
        if (!in_reach(w, *actor, *t, reach)) {
            r.failure = "part " + std::to_string(tid) + " out of reach";
            return r;
        }
        total_mass += p.mass;
        centroid.x += p.pos.x; centroid.y += p.pos.y;
        parts.push_back(p);
    }
    centroid.x /= static_cast<std::int64_t>(parts.size());
    centroid.y /= static_cast<std::int64_t>(parts.size());

    // footprint: half the widest part spread, clamped to buildable sizes
    std::int64_t radsq = 0;
    for (const ArchPart& p : parts) {
        const std::int64_t dx = p.pos.x - centroid.x;
        const std::int64_t dy = p.pos.y - centroid.y;
        radsq = std::max<std::int64_t>(radsq, dx * dx + dy * dy);
    }
    // integer sqrt via Newton (same trick as locomotion)
    std::int64_t spread = 0;
    if (radsq > 0) {
        std::int64_t x = radsq, y = (x + 1) / 2;
        while (y < x) { x = y; y = (x + radsq / x) / 2; }
        spread = x;
    }
    std::int64_t radius = spread / 2 + 120;
    radius = std::clamp<std::int64_t>(radius, 120, 3000);

    // ---- span physics ------------------------------------------------------
    // Slabs (geom_type 2 plates) bridge between posts (geom_type 1 rods).
    // A slab whose bridging dimension exceeds its material's max_span AND
    // which has no post within half that span collapses the whole shell.
    bool unstable = false;
    std::int64_t min_integ = INT64_MAX;
    std::int64_t w_hard = 0, w_ins = 0, w_wres = 0, w_fl = 0, w_lbs = 0;
    MaterialId shell_mat = kNoMaterial; std::int64_t shell_mass = -1;
    for (const ArchPart& p : parts) {
        min_integ = std::min(min_integ, p.integ);
        w_hard += p.hard * p.mass; w_ins += p.insul * p.mass;
        w_wres += p.wres * p.mass; w_fl += p.flamm * p.mass;
        w_lbs += p.lbs * p.mass;
        if (p.mass > shell_mass) { shell_mass = p.mass; shell_mat = p.material; }
    }
    auto wavg = [](std::int64_t acc, std::int64_t mass) {
        return mass > 0 ? acc / mass : 0; };
    const std::int64_t hard_a = wavg(w_hard, total_mass);
    const std::int64_t ins_a = wavg(w_ins, total_mass);
    const std::int64_t wres_a = wavg(w_wres, total_mass);
    const std::int64_t fl_a = wavg(w_fl, total_mass);
    const std::int64_t lbs_a = wavg(w_lbs, total_mass);
    (void)lbs_a;

    for (const ArchPart& s : parts) {
        if (s.geom_type != 2) continue;                    // slabs only
        const std::int64_t span_req = std::max(s.geom_len, s.geom_wid);
        const std::int64_t mspan = s.span;
        if (span_req <= 0 || mspan <= 0) continue;
        if (span_req <= mspan) continue;                   // material copes
        // nearest post distance
        std::int64_t d2min = INT64_MAX;
        for (const ArchPart& q : parts) {
            if (q.geom_type != 1 || q.id == s.id) continue;
            const std::int64_t dx = q.pos.x - s.pos.x;
            const std::int64_t dy = q.pos.y - s.pos.y;
            d2min = std::min<std::int64_t>(d2min, dx * dx + dy * dy);
        }
        // support within half the allowed span -> still stands
        if (d2min == INT64_MAX || d2min * 4 > mspan * mspan)
            unstable = true;
    }

    // ---- freeform style ----------------------------------------------------
    // bias quartiles pick a form CLASS; every class must still pass the
    // same physics — aesthetics bend the rules, never break them.
    const std::int64_t bias = std::clamp<std::int64_t>(
        w.prop_int(*actor, prop_id(w, "architectural_style_bias"), 500),
        0, 1000);
    const int style = static_cast<int>(bias * 4 / 1000);  // 0..3
    static const char* kStyleName[4] =
        {"radial", "court", "burrow", "tower"};
    // radial wants a round ring (6R perimeter), court a square (8R);
    // burrow reads as earth-sheltered (insulation bonus), tower packs tight
    // with more inner complexity for its footprint.
    const std::int64_t perim = (style == 0 ? 6 : 8) * radius;
    const std::int64_t ins_bonus = style == 2 ? 150 : 0;
    const std::int64_t cx_bonus = style == 3 ? 60 : style == 0 ? 30 : 0;

    // coverage: rods/walls contribute length, fill contributes a stub
    std::int64_t wall_cov = 0;
    for (const ArchPart& p : parts) {
        if (p.geom_type == 2) continue;                    // slabs roof, not wall
        wall_cov += p.geom_len > 0 ? p.geom_len : 120;
    }
    std::int64_t roof_area = 0;
    for (const ArchPart& p : parts)
        if (p.geom_type == 2)
            roof_area += std::max<std::int64_t>(0, p.geom_len) *
                         std::max<std::int64_t>(0, p.geom_wid);
    const std::int64_t floor_area = 4 * radius * radius;  // square proxy
    const std::int64_t wall_ratio = wall_cov * 1000 / std::max<
        std::int64_t>(1, perim);
    const std::int64_t roof_ratio = roof_area * 1000 /
        std::max<std::int64_t>(1, floor_area);
    const std::int64_t ratio = std::clamp<std::int64_t>(
        std::min(wall_ratio, roof_ratio), 0, 1000);

    // inner rooms already standing inside the footprint add complexity
    std::int64_t inner_enc = 0;
    for (const auto& ep : w.entities) {
        const Entity& o = *ep;
        if (!o.alive || o.kind_name != SIMLAB_KIND("enclosure") || o.id == ctx.actor)
            continue;
        const std::int64_t ox = o.pos.x - centroid.x;
        const std::int64_t oy = o.pos.y - centroid.y;
        if (ox * ox + oy * oy <= radius * radius) ++inner_enc;
    }
    const std::int64_t complexity = std::min<std::int64_t>(1000,
        40 + inner_enc * 40 + cx_bonus);

    // ---- habitability -------------------------------------------------------
    // sealed + heated interior + flammable shell = fire risk penalty
    bool interior_heat = false;
    if (p_temp != kNoProp) {
        for (const auto& ep : w.entities) {
            const Entity& o = *ep;
            if (!o.alive) continue;
            const std::int64_t ox = o.pos.x - centroid.x;
            const std::int64_t oy = o.pos.y - centroid.y;
            if (ox * ox + oy * oy <= radius * radius &&
                w.prop_int(o, p_temp, 0) > 500) { interior_heat = true; break; }
        }
    }
    std::int64_t hab = 350;                                 // portal spawned open
    hab += radius >= 150 ? 350 : radius * 350 / 150;        // floor clearance
    if (ratio < 300) hab -= 150;                            // too open
    if (ratio > 800 && interior_heat && fl_a > 400) hab -= 300;
    hab = std::clamp<std::int64_t>(hab, 0, 1000);

    const std::int64_t ins_fac = std::clamp<std::int64_t>(
        ratio * ins_a / 1000 + ins_bonus, 0, 1000);
    const std::int64_t barrier = std::clamp<std::int64_t>(
        hard_a * ratio / 1000 + (ratio > 800 ? 100 : 0), 0, 1000);

    const std::int64_t cost = 300 + total_mass / 100;
    if (!charge_or_fail(w, *actor, energy, cost, r)) return r;

    // ---- spawn --------------------------------------------------------------
    PropertyBag pb;
    auto seti = [&](PropId p, std::int64_t v) {
        if (p != kNoProp) pb.set(p, Value::integer(v)); };
    seti(p_rad, radius);
    seti(p_ratio, ratio);
    seti(p_ins, ins_fac);
    seti(p_hab, hab);
    seti(p_cx, complexity);
    seti(p_bar, barrier);
    seti(p_da, unstable ? 63500 : 0);
    seti(p_wres, wres_a);
    // granary seal (Phase 18): a well-sealed, high-insulation shell is a
    // dry store — contained perishables rot slower by granary_seal
    // permille. Derived purely from shell physics (insulation x coverage);
    // no "granary" verb or building name exists.
    seti(prop_id(w, "granary_seal"), ins_fac / 2);
    if (p_style != kNoProp)
        pb.set(p_style, Value::text(kStyleName[style]));
    // integrity: weakest link governs; an unstable span is born failing
    const std::int64_t integ0 = std::clamp<std::int64_t>(
        std::min(min_integ, wavg(w_lbs, total_mass)), 1, 1000);
    seti(p_integ, unstable ? 1 : integ0);
    seti(prop_id(w, "mass"), total_mass);

    SpawnSpec s;
    s.kind = "enclosure"; s.material = shell_mat; s.pos = centroid;
    s.props = pb;
    for (const ArchPart& p : parts)
        s.rel_from.push_back({"part_of", p.id});
    r.spawns.push_back(s);

    // one open passage on the rim — the door is a portal entity, not a flag
    {
        SpawnSpec ps;
        ps.kind = "portal";
        ps.material = kNoMaterial;
        ps.pos = {centroid.x + static_cast<std::int32_t>(radius),
                  centroid.y};
        PropertyBag pp;
        const PropId p_open = prop_id(w, "portal_open");
        const PropId p_abar = prop_id(w, "access_barrier");
        if (p_open != kNoProp) pp.set(p_open, Value::integer(1));
        if (p_abar != kNoProp) pp.set(p_abar, Value::integer(barrier));
        ps.props = pp;
        r.spawns.push_back(ps);
    }

    for (const ArchPart& p : parts) {
        if (contained != kNoProp)
            set_change_v(r, p.id, contained,
                         Value::boolean(false), Value::boolean(true));
    }
    r.possible = true;
    r.ticks = static_cast<std::int64_t>(parts.size()) / 2 + 1;
    std::ostringstream n;
    n << "enclose r=" << radius << " ratio=" << ratio
      << " insul=" << ins_fac << " hab=" << hab << " cx=" << complexity
      << " barrier=" << barrier << " style=" << kStyleName[style]
      << (unstable ? " UNSTABLE-SPAN" : "") << " cost=" << cost;
    r.notes.push_back(n.str());
    return r;
}

PredictionResult predict_bypass_barrier(const World& w,
                                        const InteractionContext& ctx) {
    PredictionResult r; r.law = 5; r.law_name = "physical.compose";
    const PropId energy = prop_id(w, "energy");
    const PropId contained = prop_id(w, "contained");
    const PropId reach = prop_id(w, "reach");
    const PropId p_bar = prop_id(w, "access_barrier");
    const PropId p_open = prop_id(w, "portal_open");
    (void)p_open;
    const Entity* actor = w.find(ctx.actor);
    if (!actor || !actor->alive) { r.failure = "no living actor"; return r; }
    const Entity* enc = ctx.targets.empty() ? nullptr
        : w.find(ctx.targets.front());
    if (!enc || !enc->alive) { r.failure = "no living target"; return r; }
    if (enc->kind_name != SIMLAB_KIND("enclosure")) {
        r.failure = "target is not an enclosure"; return r;
    }
    if (!in_reach(w, *actor, *enc, reach)) {
        r.failure = "enclosure out of reach"; return r;
    }
    // owner never needs to bypass their own shell
    if (arch_owner_of(w, enc->id) == ctx.actor) {
        r.failure = "already inside consent"; return r;
    }
    if (!charge_or_fail(w, *actor, energy, 60, r)) return r;
    const std::int64_t barrier = p_bar != kNoProp
        ? w.prop_int(*enc, p_bar, 0) : 0;
    const std::int64_t cx = arch_complexity(w, enc->id);
    const std::int64_t motor = w.prop_int(*actor,
        prop_id(w, "mind_motor_precision"), 1000);
    const std::int64_t tom = w.prop_int(*actor,
        prop_id(w, "mind_theory_of_mind"), 1000);
    const std::int64_t skill = (motor + tom) / 2;
    const std::int64_t need = barrier + cx / 10;
    r.ticks = 2 + cx / 20;
    if (skill >= need) {
        r.possible = true;
        r.notes.push_back("bypass succeeded quietly");
    } else {
        // the attempt still happens — noise is the cost of failure
        r.possible = true;
        r.notes.push_back("bypass repelled — noise alerted the household");
    }
    return r;
}

PredictionResult predict_repair(const World& w,
                                const InteractionContext& ctx) {
    PredictionResult r; r.law = 5; r.law_name = "physical.compose";
    const PropId contained = prop_id(w, "contained");
    const PropId energy = prop_id(w, "energy");
    const PropId reach = prop_id(w, "reach");
    const PropId p_integ = prop_id(w, "integrity");
    if (ctx.targets.size() < 2) {
        r.failure = "repair needs an enclosure plus material"; return r;
    }
    const Entity* actor = w.find(ctx.actor);
    if (!actor || !actor->alive) { r.failure = "no living actor"; return r; }
    const Entity* enc = w.find(ctx.targets.front());
    if (!enc || enc->kind_name != SIMLAB_KIND("enclosure")) {
        r.failure = "repair target is not an enclosure"; return r;
    }
    if (!in_reach(w, *actor, *enc, reach)) {
        r.failure = "enclosure out of reach"; return r;
    }
    if (!charge_or_fail(w, *actor, energy, 100, r)) return r;
    std::int64_t integ = p_integ != kNoProp
        ? w.prop_int(*enc, p_integ, 500) : 500;
    std::int64_t patched = 0;
    for (std::size_t i = 1; i < ctx.targets.size(); ++i) {
        const Entity* t = w.find(ctx.targets[i]);
        if (!t || !t->alive) continue;
        if (!in_reach(w, *actor, *t, reach)) {
            r.failure = "patch material out of reach"; return r;
        }
        const std::int64_t ti = p_integ != kNoProp
            ? w.prop_int(*t, p_integ, 200) : 200;
        const std::int64_t add = std::min<std::int64_t>(ti / 2, 120);
        integ = std::min<std::int64_t>(1000, integ + add);
        patched += add;
        if (contained != kNoProp)
            set_change_v(r, t->id, contained,
                         Value::boolean(false), Value::boolean(true));
    }
    if (patched <= 0) { r.failure = "nothing to patch with"; return r; }
    if (p_integ != kNoProp) {
        const std::int64_t cur = w.prop_int(*enc, p_integ, 500);
        set_change(r, enc->id, p_integ, cur, integ);
    }
    r.possible = true; r.ticks = 2;
    r.notes.push_back("patched shell +" + std::to_string(patched));
    return r;
}

}  // namespace simlab
