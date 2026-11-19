#include "simlab/Laws.hpp"
#include "simlab/World.hpp"
#include "LawUtil.hpp"
#include <algorithm>
#include <sstream>

namespace simlab {

using namespace lawutil;

LawId PhysicsModel::id() const { return 1; }

bool PhysicsModel::supports(const World& w, const InteractionContext& ctx) const {
    (void)w;
    return ctx.parameters.action == "impact" && !ctx.targets.empty();
}

// Damage model (all integer, all property-driven):
//   E     = applied force (milli-units)
//   armor = target hardness (0..1000)
//   crit  = brittleness raises crack damage, lowers needed force to fracture
//   mass  slows the effective hit: dmg /= (1 + mass/50000)
//   integrity loss = E * (1000 + 2*crit) / (armor + 500) / mass_factor
// Fracture: if a single hit exceeds `fracture_threshold` permille of the
// target's brittleness-scaled ceiling, integrity is set to 0 (destroyed).
// Nothing here names a material — obsidian vs wood differ only in data.
PredictionResult PhysicsModel::predict(const World& w, const InteractionContext& ctx) const {
    PredictionResult r;
    r.law = id();
    r.law_name = name();
    const Entity* actor = w.find(ctx.actor);
    if (!actor || !actor->alive) { r.failure = "no living actor"; return r; }

    // Resolve well-known property ids by name (per-world registry).
    const PropertyDef* pd_hard = w.props.find_name("hardness");
    const PropertyDef* pd_brit = w.props.find_name("brittleness");
    const PropertyDef* pd_mass = w.props.find_name("mass");
    const PropertyDef* pd_integ = w.props.find_name("integrity");
    if (!pd_hard || !pd_brit || !pd_mass || !pd_integ) {
        r.failure = "physics requires hardness/brittleness/mass/integrity properties";
        return r;
    }

    const std::int64_t E0 = ctx.parameters.force > 0 ? ctx.parameters.force : 1000;
    // wielded tools: thrust_range extends reach, edge sharpens the blow
    const auto [treach, tedge] = w.tool_bonus(ctx.actor);
    // Nhịp 4c: motor_precision scales transmitted force — a master's
    // strike lands harder, a clumsy one's glances off.
    const PropId motor_pid = pid(w, "mind_motor_precision");
    const std::int64_t motor0 = motor_pid != kNoProp
        ? w.prop_int(*actor, motor_pid, 1000) : 1000;
    // Phase 10: lost arms / low blood volume degrade effective motor skill.
    const std::int64_t motor = motor0 * anatomy_motor(w, *actor) / 1000;
    if (!conscious_enough(w, *actor, r)) return r;
    const std::int64_t E = E0 * (1000 + tedge) / 1000 * motor / 1000;
    const PropId reach_pid = pid(w, "reach"), energy_pid = pid(w, "energy"),
                 contained_pid = pid(w, "contained");
    const EntityId striker = wielded_tool(w, ctx.actor);
    std::int64_t wear_total = 0;

    for (EntityId tid : ctx.targets) {
        const Entity* t = w.find(tid);
        if (!t || !t->alive) continue;
        if (is_contained(w, *t, contained_pid)) continue;       // inside a structure
        if (!in_reach(w, *actor, *t, reach_pid, treach, &r)) {
            r.notes.push_back("target " + std::to_string(tid) + " out of reach");
            continue;
        }
        // Phase 10: anatomical bodies route the blow into ONE body part —
        // picked deterministically by part_size weight and a hash of the
        // blow (same inputs -> same part, replay-safe).
        std::vector<EntityId> parts;
        std::vector<std::int64_t> psz;
        std::int64_t wsum = 0;
        const PropId sev_pid = pid(w, "severed");
        const PropId role_chk = pid(w, "part_role");
        for (const Relation& rel : w.relations_of(tid, "part_of")) {
            if (rel.b != tid) continue;
            const Entity* p = w.find(rel.a);
            if (!p || !p->alive) continue;
            // anatomy parts carry part_role; ordinary composite structure
            // parts (spear head/shaft) do not -> they keep physics damage.
            if (role_chk != kNoProp) {
                const Value* rv = w.prop_of(*p, role_chk);
                if (!rv || rv->type != ValType::Text || rv->s.empty()) continue;
            }
            const PropId sev = sev_pid;
            if (sev != kNoProp) {
                const Value* sv = w.prop_of(*p, sev);
                if (sv && sv->type == ValType::Integer && sv->i != 0) continue;
            }
            const std::int64_t sz =
                w.prop_int(*p, pid(w, "part_size"), 200);
            parts.push_back(rel.a); psz.push_back(sz); wsum += sz;
        }
        if (!parts.empty()) {
            // deterministic weighted pick
            std::uint64_t h = static_cast<std::uint64_t>(tid) * 0x9E3779B97F4A7C15ull
                ^ static_cast<std::uint64_t>(E) * 0xBF58476D1CE4E5B9ull
                ^ static_cast<std::uint64_t>(w.clock_.tick()) * 0x94D049BB133111EBull;
            h ^= h >> 29; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 32;
            std::int64_t pick = static_cast<std::int64_t>(h % wsum), pi = 0;
            while (pick >= psz[pi]) { pick -= psz[pi]; ++pi; }
            const EntityId part_id = parts[pi];
            const Entity* part = w.find(part_id);
            const std::int64_t hard = w.prop_int(*part, pd_hard->id, 300);
            const std::int64_t brit = w.prop_int(*part, pd_brit->id, 400);
            const std::int64_t mass = w.prop_int(*part, pd_mass->id, 1000);
            const std::int64_t integ = w.prop_int(*part, pd_integ->id, 500);
            const std::int64_t mass_factor = 1000 + mass / 50;
            std::int64_t dmg = E * (1000 + 2 * brit) /
                ((hard + 500) * mass_factor / 1000);
            if (dmg < 1 && E > 0) dmg = 1;
            const bool sharp = tedge >= 350;
            const PropId br_p = pid(w, "bleed_rate"),
                         fr_p = pid(w, "fracture_level"),
                         pv_p = pid(w, "vital_organ"),
                         rl_p = pid(w, "part_role"),
                         sl_p = pid(w, "body_slot"),
                         cn_p = pid(w, "contained"),
                         pa_p = pid(w, "pain_level"),
                         bo_p = pid(w, "bleed_rate");
            // layered response: blunt -> fracture + deep bruising bleed;
            // sharp -> integ loss + heavy soft-tissue bleed; bone parts
            // (brittleness>700) fracture under both.
            std::int64_t n_integ = integ, n_bleed = 0, n_frac = 0;
            if (sharp) {
                n_integ = std::max<std::int64_t>(0, integ - dmg);
                n_bleed = std::min<std::int64_t>(1000,
                    w.prop_int(*part, br_p, 0) + dmg * 3 / 4);
            } else {
                n_integ = std::max<std::int64_t>(0, integ - dmg * 3 / 5);
                n_bleed = std::min<std::int64_t>(1000,
                    w.prop_int(*part, br_p, 0) + dmg / 20);
            }
            if (brit > 700) n_frac = std::min<std::int64_t>(1000,
                w.prop_int(*part, fr_p, 0) + dmg / 3);
            const std::int64_t vital = pv_p != kNoProp
                ? w.prop_int(*part, pv_p, 0) : 0;
            const bool amputate = sharp && dmg >= integ && vital < 1000;
            const bool part_destroyed = n_integ <= 0 || (sharp && dmg >= integ);

            // secondary organ hit: sharp penetration damages the vital
            // organ sharing this part's body_slot (heart/lung/brain).
            EntityId organ_id = kNoEntity;
            std::int64_t organ_dmg = 0;
            if (sharp && dmg > 300 && sl_p != kNoProp) {
                const Value* myslot = w.prop_of(*part, sl_p);
                if (myslot && myslot->type == ValType::Text) {
                    for (const Relation& rel2 : w.relations_of(tid, "part_of")) {
                        if (rel2.b != tid || rel2.a == part_id) continue;
                        const Entity* q = w.find(rel2.a);
                        if (!q || !q->alive) continue;
                        const Value* sv = sev_pid != kNoProp ? w.prop_of(*q, sev_pid) : nullptr;
                        if (sv && sv->type == ValType::Integer && sv->i != 0) continue;
                        if (role_chk != kNoProp) {
                            const Value* qr = w.prop_of(*q, role_chk);
                            if (!qr || qr->type != ValType::Text || qr->s.empty()) continue;
                        }
                        const Value* qs = w.prop_of(*q, sl_p);
                        if (!qs || qs->type != ValType::Text || qs->s != myslot->s) continue;
                        if (pv_p != kNoProp && w.prop_int(*q, pv_p, 0) >= 800) {
                            organ_id = rel2.a; organ_dmg = dmg / 2; break;
                        }
                    }
                }
            }

            // pain on the body: sharp wounds hurt more; amputation maxes.
            std::int64_t pain_add = amputate ? 1000 : (sharp ? dmg : dmg / 2);
            const std::int64_t body_pain = pa_p != kNoProp
                ? w.prop_int(*t, pa_p, 0) : 0;
            const std::int64_t n_pain = std::min<std::int64_t>(1000, body_pain + pain_add);

            auto setp = [&](EntityId id, PropId p, std::int64_t before, std::int64_t after) {
                if (p == kNoProp || before == after) return;
                StateChange ch; ch.entity = id; ch.prop = p; ch.had_before = true;
                ch.before = Value::integer(before); ch.after = Value::integer(after);
                r.changes.push_back(ch);
            };
            setp(part_id, pd_integ->id, integ, amputate ? 60 : n_integ);
            if (br_p != kNoProp) setp(part_id, br_p, w.prop_int(*part, br_p, 0), amputate ? 1000 : n_bleed);
            if (fr_p != kNoProp && n_frac) setp(part_id, fr_p, w.prop_int(*part, fr_p, 0), n_frac);
            if (amputate) {
                // severed part stays alive at the scene: severed=1,
                // contained=false -> physically independent entity.
                const PropId sv_p = pid(w, "severed");
                const std::int64_t sv0 = sv_p != kNoProp ? w.prop_int(*part, sv_p, 0) : 0;
                setp(part_id, sv_p, sv0, 1);
                if (cn_p != kNoProp) {
                    // contained is a BOOLEAN prop — integer write would
                    // break is_contained's ValType::Boolean check.
                    const Value* cv = w.prop_of(*part, cn_p);
                    const bool was = cv && cv->type == ValType::Boolean && cv->b;
                    if (was) {
                        StateChange ch; ch.entity = part_id; ch.prop = cn_p;
                        ch.had_before = true; ch.before = Value::boolean(true);
                        ch.after = Value::boolean(false);
                        r.changes.push_back(ch);
                    }
                }
                // the edge is KEPT as anatomical record; `severed` is the
                // detach truth (lets anatomy_function still count it).
                // stump bleeding moves onto the body's own bleed_rate.
                const std::int64_t bbr = bo_p != kNoProp ? w.prop_int(*t, bo_p, 0) : 0;
                setp(tid, bo_p, bbr, std::min<std::int64_t>(1000, bbr + 700));
                // severed arm drops whatever the body was wielding
                if (rl_p != kNoProp) {
                    const Value* rv = w.prop_of(*part, rl_p);
                    if (rv && rv->type == ValType::Text && rv->s == "arm") {
                        for (const Relation& wr : w.relations_of(tid, "wielding"))
                            if (wr.a == tid)
                                r.rel_changes.push_back({tid, "wielding", wr.b, false});
                    }
                }
            }
            if (organ_id != kNoEntity) {
                const Entity* op = w.find(organ_id);
                if (op) {
                    const std::int64_t oi = w.prop_int(*op, pd_integ->id, 500);
                    setp(organ_id, pd_integ->id, oi, std::max<std::int64_t>(0, oi - organ_dmg));
                    if (br_p != kNoProp)
                        setp(organ_id, br_p, w.prop_int(*op, br_p, 0),
                             std::min<std::int64_t>(1000, w.prop_int(*op, br_p, 0) + organ_dmg / 2));
                }
            }
            // Phase 10.3 lumen rupture: a blow landing on a body region
            // bursts any distended hollow organ (lumen_capacity>0,
            // luminal pressure>=800) sharing that region's body_slot.
            // Fully generic — slot names, capacities and waste materials
            // all come from data; no organ or disease names in code.
            {
                const PropId lc_i = pid(w, "lumen_capacity"),
                           cl_i = pid(w, "contents_load"),
                           ca_i = pid(w, "contents_accum"),
                           rup_i = pid(w, "ruptured"),
                           inf_i = pid(w, "infection_level"),
                           em_i = pid(w, "evacuate_material"),
                           nec_i = pid(w, "necrotic"),
                           cnt_i = pid(w, "contamination");
                const Value* hit_slot = sl_p != kNoProp
                    ? w.prop_of(*part, sl_p) : nullptr;
                if (lc_i != kNoProp && cl_i != kNoProp && rup_i != kNoProp &&
                    hit_slot && hit_slot->type == ValType::Text) {
                    for (const Relation& rel3 : w.relations_of(tid, "part_of")) {
                        if (rel3.b != tid) continue;
                        const Entity* lp = w.find(rel3.a);
                        if (!lp || !lp->alive) continue;
                        if (sev_pid != kNoProp) {
                            const Value* sv = w.prop_of(*lp, sev_pid);
                            if (sv && sv->type == ValType::Integer && sv->i != 0)
                                continue;
                        }
                        if (nec_i != kNoProp) {
                            const Value* nv = w.prop_of(*lp, nec_i);
                            if (nv && nv->type == ValType::Integer && nv->i != 0)
                                continue;
                        }
                        const std::int64_t lcap =
                            w.prop_int(*lp, lc_i, 0);
                        if (lcap <= 0) continue;
                        const std::int64_t lload =
                            w.prop_int(*lp, cl_i, 0);
                        if (lload * 1000 / lcap < 800) continue;
                        if (rup_i != kNoProp &&
                            w.prop_int(*lp, rup_i, 0) != 0) continue;
                        const Value* ls = sl_p != kNoProp
                            ? w.prop_of(*lp, sl_p) : nullptr;
                        if (!ls || ls->type != ValType::Text ||
                            ls->s != hit_slot->s) continue;
                        // rupture: wall bursts, contents spill as contamination
                        setp(rel3.a, rup_i, w.prop_int(*lp, rup_i, 0), 1);
                        setp(rel3.a, pd_integ->id,
                             w.prop_int(*lp, pd_integ->id, 500), 0);
                        if (br_p != kNoProp)
                            setp(rel3.a, br_p, w.prop_int(*lp, br_p, 0),
                                 std::min<std::int64_t>(1000,
                                     w.prop_int(*lp, br_p, 0) + 300));
                        setp(rel3.a, cl_i, lload, 0);
                        if (ca_i != kNoProp && w.prop_of(*lp, ca_i))
                            setp(rel3.a, ca_i,
                                 w.prop_int(*lp, ca_i, 0), 0);
                        // spilled contents infect sibling parts (peritonitis)
                        std::int64_t spill = 0;
                        if (em_i != kNoProp && cnt_i != kNoProp) {
                            const Value* ev = w.prop_of(*lp, em_i);
                            if (ev && ev->type == ValType::Text &&
                                !ev->s.empty())
                                if (const MaterialDef* wm =
                                        w.materials.find_name(ev->s)) {
                                    Entity tmp; tmp.material = wm->id;
                                    spill = w.prop_int(tmp, cnt_i, 0) / 4;
                                }
                        }
                        if (spill > 0 && inf_i != kNoProp)
                            for (const Relation& rel4 :
                                 w.relations_of(tid, "part_of")) {
                                if (rel4.b != tid || rel4.a == rel3.a)
                                    continue;
                                const Entity* sib = w.find(rel4.a);
                                if (!sib || !sib->alive) continue;
                                setp(rel4.a, inf_i,
                                     w.prop_int(*sib, inf_i, 0),
                                     std::min<std::int64_t>(1000,
                                         w.prop_int(*sib, inf_i, 0) + spill));
                            }
                        if (pa_p != kNoProp)
                            setp(tid, pa_p, body_pain, 1000);
                        r.notes.push_back(
                            "lumen rupture under impact (pressure " +
                            std::to_string(lload * 1000 / lcap) + ")");
                    }
                }
            }
            // vital organ (brain/heart, vital_organ==1000) destroyed or
            // severed -> the body dies on the spot.
            const bool vital_dead = vital >= 1000 &&
                (part_destroyed || amputate || organ_dmg > 0);
            const PropId body_integ = pid(w, "integrity");
            if (vital_dead && body_integ != kNoProp) {
                const std::int64_t bi = w.prop_int(*t, body_integ, 500);
                setp(tid, body_integ, bi, 0);
            }
            setp(tid, pa_p, body_pain, n_pain);
            if (striker != kNoEntity && striker != part_id)
                wear_total += reaction_wear(w, striker, integ - (amputate ? 60 : n_integ), hard);
            std::ostringstream n;
            n << "impact E=" << E << " on part " << part_id << " of " << tid
              << " part=" << part_id
              << " dmg=" << dmg << (amputate ? " AMPUTATED" : "")
              << (sharp ? " sharp" : " blunt");
            r.notes.push_back(n.str());
            r.possible = true;
            continue;
        }

        const std::int64_t hard = w.prop_int(*t, pd_hard->id, 300);
        const std::int64_t brit = w.prop_int(*t, pd_brit->id, 400);
        const std::int64_t mass = w.prop_int(*t, pd_mass->id, 1000);
        const std::int64_t integ = w.prop_int(*t, pd_integ->id, 500);

        const std::int64_t mass_factor = 1000 + mass / 50;      // heavier absorbs more
        std::int64_t dmg = E * (1000 + 2 * brit) / ((hard + 500) * mass_factor / 1000);
        if (dmg < 1 && E > 0) dmg = 1;
        // brittle targets fracture when the hit exceeds what their surface can take
        const bool fracture = dmg * 4 > (hard + brit) && brit > 600;
        const std::int64_t new_integ = fracture ? 0 : std::max<std::int64_t>(0, integ - dmg);

        StateChange ch;
        ch.entity = tid; ch.prop = pd_integ->id; ch.had_before = true;
        ch.before = Value::integer(integ);
        ch.after = Value::integer(new_integ);
        r.changes.push_back(ch);
        if (striker != kNoEntity && striker != tid)
            wear_total += reaction_wear(w, striker, integ - new_integ, hard);

        std::ostringstream n;
        n << "impact E=" << E << " on " << tid
          << " hard=" << hard << " brit=" << brit << " mass=" << mass
          << " -> dmg=" << dmg << (fracture ? " FRACTURE" : "");
        r.notes.push_back(n.str());
        r.possible = true;
    }
    // precise hands damage their own tools less (collateral wear proxy
    // for self-injury: 1000 -> 1.0x, 200 -> 1.4x, 2500 -> 0.25x)
    if (r.possible && wear_total > 0)
        wear_total = wear_total *
            std::clamp<std::int64_t>(3000 - motor, 0, 3000) / 2000;
    if (r.possible && wear_total > 0) apply_tool_wear(w, striker, wear_total, r);
    if (r.possible && !charge_energy(*actor, energy_pid, E0, r)) {
        r.possible = false; r.changes.clear(); r.spawns.clear(); r.notes.clear();
        return r;
    }
    r.ticks = ctx.parameters.ticks;
    if (!r.possible && r.failure.empty()) r.failure = "no valid targets";
    return r;
}

std::unique_ptr<INaturalLawModel> PhysicsModel::clone() const {
    return std::make_unique<PhysicsModel>();
}

}  // namespace simlab
