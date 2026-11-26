// Phase 12.6 — warfare, logistics & emergent weapons R&D.
//
// One law model (`WarfareModel`, id 23) for the physical acts of war:
//   launch  — throw a projectile with stored strain energy (any composite
//             whose parts make an elastic frame + a high-tension string has
//             an emergent `launch_impulse`; big frames are siege engines)
//   coat    — smear a toxic/infectious material on the wielded weapon
//   don/doff— wear / remove protection (armor_penetration_resist)
//   mobilize— a leader calls followers to march on a HOSTILE community
// plus `blast()` (shared with ThermalModel: heating a sealed energetic
// charge). Nothing here names a weapon, armor, unit type or recipe: every
// effect is a function of material/composite properties, terrain and the
// diplomatic ledger. Damage to bodies goes through lawutil::deliver_blow,
// the same anatomy/armor/coating path a hand-held impact uses.
#include "simlab/Laws.hpp"
#include "simlab/World.hpp"
#include "simlab/Cognition.hpp"
#include "LawUtil.hpp"
#include <algorithm>
#include <sstream>

namespace simlab {

using namespace lawutil;

namespace {

constexpr std::int64_t kBreachIntegrity = 300;   // walls below this no longer bar entry

std::int64_t pint(const World& w, const Entity& e, const char* lit, std::int64_t d = 0) {
    const PropId p = w.props.find_name(lit) ? w.props.find_name(lit)->id : kNoProp;
    return p != kNoProp ? w.prop_int(e, p, d) : d;
}

void setc(PredictionResult& r, EntityId id, PropId p, std::int64_t before, std::int64_t after) {
    if (p == kNoProp || before == after) return;
    set_change(r, id, p, before, after);
}

bool is_enclosure(const Entity& e) { return e.kind_name == SIMLAB_KIND("enclosure"); }

// Damage a STRUCTURE (enclosure wall) by an impulse with a blunt channel.
// Walls resist with their access_barrier; flammable walls take extra loss
// from a burning projectile. Breach: below kBreachIntegrity the wall no
// longer bars entry (access_barrier -> 0).
void wound_structure(const World& w, PredictionResult& r, const Entity& s,
                     std::int64_t E, std::int64_t blunt, std::int64_t fire,
                     const char* mech) {
    const PropId integ_p = SIMLAB_PID(w.props, "integrity");
    const PropId bar_p = SIMLAB_PID(w.props, "access_barrier");
    const PropId fl_p = SIMLAB_PID(w.props, "flammability");
    const std::int64_t integ = w.prop_int(s, integ_p, 500);
    const std::int64_t bar = bar_p != kNoProp ? w.prop_int(s, bar_p, 0) : 0;
    std::int64_t loss = E * (500 + blunt) / (5000 * (500 + bar));
    if (fire > 0 && fl_p != kNoProp)
        loss += w.prop_int(s, fl_p, 0) * fire / 1000 / 4;    // incendiary
    if (loss < 1 && E > 0) loss = 1;
    const std::int64_t after = std::max<std::int64_t>(0, integ - loss);
    setc(r, s.id, integ_p, integ, after);
    if (after < kBreachIntegrity && bar > 0) {
        setc(r, s.id, bar_p, bar, 0);
        r.notes.push_back("structure=" + std::to_string(s.id) + " BREACHED");
    }
    std::ostringstream n;
    n << "structure=" << s.id << " mech=" << mech << " integrity " << integ
      << "->" << after;
    r.notes.push_back(n.str());
}

// does any mind of language `lang` defer to `leader`?  (followers list)
std::vector<EntityId> followers_of(const World& w, EntityId leader,
                                   std::int64_t lang, std::int64_t range) {
    std::vector<EntityId> out;
    const Entity* le = w.find(leader);
    if (!le) return out;
    const std::string key = "defer_to:" + std::to_string(leader);
    const PropId lang_p = SIMLAB_PID(w.props, "language_id");
    for (const auto& [mid, m] : w.minds_) {
        if (mid == leader) continue;
        const Entity* me = w.find(mid);
        if (!me || !me->alive) continue;
        if (lang_p == kNoProp || w.prop_int(*me, lang_p, 0) != lang) continue;
        if (dist2(me->pos, le->pos) > range * range) continue;
        const Belief* b = m.find_belief(key);
        if (b && b->confidence >= 400) out.push_back(mid);
    }
    return out;
}

}  // namespace

namespace lawutil {

// Energetic release (Phase 12.6). A charge of `expansion` force sealed in a
// casing of integrity `casing` releases an impulse that falls off linearly
// to zero at its radius: living bodies take a blunt "blast" blow through
// deliver_blow (armor only blunts it by 1/3), structures lose integrity.
// Containment matters: the same mix loose (low integrity) is a flash, sealed
// it is a bomb. Returns the impulse (0 = no blast).
std::int64_t blast(const World& w, PredictionResult& r, Vec2 center,
                   std::int64_t expansion, std::int64_t casing,
                   std::int64_t mass, const std::vector<EntityId>& exclude) {
    if (expansion < 300) return 0;
    const std::int64_t impulse = expansion * (500 + casing / 2) / 1000 *
                                 (1000 + mass / 50) / 1000;
    const std::int64_t radius = 50 + expansion / 4;
    const PropId cont_p = SIMLAB_PID(w.props, "contained");
    std::int64_t wear = 0;
    for (const EntityId id : w.entities_near(center, radius)) {
        if (std::find(exclude.begin(), exclude.end(), id) != exclude.end()) continue;
        const Entity* e = w.find(id);
        if (!e || !e->alive || is_contained(w, *e, cont_p)) continue;
        std::int64_t d = 0;
        const std::int64_t d2 = dist2(center, e->pos);
        while (d * d < d2) ++d;
        const std::int64_t fall = (radius - d) * 1000 / radius;
        if (fall <= 0) continue;
        if (is_enclosure(*e)) {
            wound_structure(w, r, *e, impulse * fall * 3 / 2, 800, 0, "blast");
            continue;
        }
        Blow b; b.blunt = 800; b.mech = "blast";
        deliver_blow(w, r, id, impulse * 2 * fall / 1000, b, kNoEntity, wear);
    }
    r.notes.push_back("BLAST impulse=" + std::to_string(impulse) +
                      " radius=" + std::to_string(radius));
    return impulse;
}

}  // namespace lawutil

LawId WarfareModel::id() const { return 23; }

bool WarfareModel::supports(const World& w, const InteractionContext& ctx) const {
    (void)w;
    const std::string& a = ctx.parameters.action;
    if (a == "launch") return ctx.targets.size() == 2 || ctx.targets.size() == 3;
    if (a == "coat" || a == "don" || a == "doff") return ctx.targets.size() == 1;
    if (a == "mobilize") return ctx.targets.size() == 1;
    return false;
}

namespace {

PredictionResult predict_launch(const World& w, const InteractionContext& ctx, LawId law) {
    PredictionResult r; r.law = law; r.law_name = "warfare.ballistics";
    const Entity* actor = w.find(ctx.actor);
    if (!actor || !actor->alive) { r.failure = "no living actor"; return r; }
    if (!conscious_enough(w, *actor, r)) return r;
    const PropId reach_p = SIMLAB_PID(w.props, "reach");
    const PropId energy_p = SIMLAB_PID(w.props, "energy");
    const PropId cont_p = SIMLAB_PID(w.props, "contained");
    const PropId li_p = SIMLAB_PID(w.props, "launch_impulse");
    const PropId q_p = SIMLAB_PID(w.props, "quantity");
    const PropId integ_p = SIMLAB_PID(w.props, "integrity");
    const PropId temp_p = SIMLAB_PID(w.props, "temperature");
    const Entity* tgt = w.find(ctx.targets[0]);
    const Entity* ammo = w.find(ctx.targets[1]);
    if (!tgt || !tgt->alive || !ammo || !ammo->alive) { r.failure = "no target/ammo"; return r; }
    if (is_contained(w, *ammo, cont_p)) { r.failure = "ammo is built into a structure"; return r; }
    if (!in_reach(w, *actor, *ammo, reach_p, 0, &r)) { r.failure = "ammo out of reach"; return r; }
    // launcher: a crewed engine standing within reach (3rd target) or the
    // wielded tool. Whatever it is, it must HAVE stored strain energy.
    EntityId launcher = kNoEntity;
    if (ctx.targets.size() == 3) {
        const Entity* l = w.find(ctx.targets[2]);
        if (!l || !l->alive || is_contained(w, *l, cont_p)) { r.failure = "no launcher"; return r; }
        if (!in_reach(w, *actor, *l, reach_p, 0, &r)) { r.failure = "launcher out of reach"; return r; }
        launcher = l->id;
    } else {
        launcher = wielded_tool(w, ctx.actor);
    }
    const Entity* le = launcher != kNoEntity ? w.find(launcher) : nullptr;
    const std::int64_t impulse0 = le && li_p != kNoProp ? w.prop_int(*le, li_p, 0) : 0;
    add_cond(r, "launch_impulse", impulse0, 1);
    if (impulse0 <= 0) { r.failure = "nothing stores strain energy to launch with"; return r; }
    // height turns into range and energy (plunging shot from the walls)
    std::int64_t elev_bonus = 0;
    if (!w.terrain.empty() && w.terrain.contains(actor->pos) && w.terrain.contains(tgt->pos)) {
        const std::int64_t dz = static_cast<std::int64_t>(w.terrain.elev_at(actor->pos)) -
                                w.terrain.elev_at(tgt->pos);
        elev_bonus = std::clamp<std::int64_t>(dz * 5, -300, 500);
    }
    const std::int64_t motor = w.prop_int(*actor, SIMLAB_PID(w.props, "mind_motor_precision"), 1000) *
                               anatomy_motor(w, *actor) / 1000;
    const std::int64_t impulse = impulse0 * (1000 + elev_bonus) / 1000;
    const std::int64_t range = impulse;                            // map units
    std::int64_t d = 0;
    { const std::int64_t d2 = dist2(actor->pos, tgt->pos); while (d * d < d2) ++d; }
    add_cond(r, "range", range, d);
    if (d > range) { r.failure = "target beyond launch range"; return r; }
    if (!charge_effort(w, *actor, energy_p, 2000 + impulse * 10, r)) return r;
    // one stored-energy budget, two scales: flesh takes a blow of E_body
    // (comparable to a strong hand strike), masonry the full momentum E_wall
    const std::int64_t E_body = impulse * motor / 1000;
    const std::int64_t E = impulse * 500 * motor / 1000;
    std::int64_t wear = 0;
    Blow b = blow_of(w, ammo->id, 0);
    b.mech = "projectile";
    const std::int64_t hot = temp_p != kNoProp ? w.prop_int(*ammo, temp_p, 200) : 200;
    const std::int64_t afl = w.prop_int(*ammo, SIMLAB_PID(w.props, "flammability"), 0);
    const std::int64_t fire = hot >= 3000 ? afl : 0;
    if (is_enclosure(*tgt)) wound_structure(w, r, *tgt, E, b.blunt, fire, "projectile");
    else {
        deliver_blow(w, r, tgt->id, E_body, b, launcher, wear);
        if (fire > 0) {
            const PropId pa = SIMLAB_PID(w.props, "pain_level");
            if (pa != kNoProp) setc(r, tgt->id, pa, w.prop_int(*tgt, pa, 0),
                                    std::min<std::int64_t>(1000, w.prop_int(*tgt, pa, 0) + fire / 4));
            r.notes.push_back("victim=" + std::to_string(tgt->id) + " mech=thermal");
        }
    }
    // an energetic payload that arrives hot goes off at the target
    const std::int64_t vol = w.prop_int(*ammo, SIMLAB_PID(w.props, "volatility"), 0);
    const std::int64_t exf = w.prop_int(*ammo, SIMLAB_PID(w.props, "expansion_force"), 0);
    if (vol >= 300 && hot >= 4000 - vol * 3)
        blast(w, r, tgt->pos, exf, w.prop_int(*ammo, integ_p, 200),
              w.prop_int(*ammo, SIMLAB_PID(w.props, "mass"), 500), {ammo->id});
    // the projectile is spent (one unit of a stack, or the whole object)
    if (q_p != kNoProp && ammo->props.get(q_p) && w.prop_int(*ammo, q_p, 0) > 1)
        setc(r, ammo->id, q_p, w.prop_int(*ammo, q_p, 0), w.prop_int(*ammo, q_p, 0) - 1);
    else
        setc(r, ammo->id, integ_p, w.prop_int(*ammo, integ_p, 500), 0);
    r.possible = true;
    r.ticks = le && pint(w, *le, "mass", 0) > 20000 ? 4 : 1;     // big engines reload slowly
    r.notes.push_back("launch impulse=" + std::to_string(impulse) +
                      " elev=" + std::to_string(elev_bonus) + " E=" + std::to_string(E));
    return r;
}

PredictionResult predict_coat(const World& w, const InteractionContext& ctx, LawId law) {
    PredictionResult r; r.law = law; r.law_name = "warfare.alchemy";
    const Entity* actor = w.find(ctx.actor);
    if (!actor || !actor->alive) { r.failure = "no living actor"; return r; }
    const PropId reach_p = SIMLAB_PID(w.props, "reach");
    const PropId cont_p = SIMLAB_PID(w.props, "contained");
    // abstract thought: seeing that "what sickens the eater" can ride a blade
    const std::int64_t abst = w.prop_int(*actor, SIMLAB_PID(w.props, "mind_abstraction_gain"), 1000);
    add_cond(r, "abstraction", abst, 1200);
    if (abst < 1200) { r.failure = "no mind to see a poison as a weapon"; return r; }
    const EntityId weapon = wielded_tool(w, ctx.actor);
    if (weapon == kNoEntity) { r.failure = "nothing wielded to coat"; return r; }
    const Entity* src = w.find(ctx.targets[0]);
    if (!src || !src->alive || is_contained(w, *src, cont_p)) { r.failure = "no coating source"; return r; }
    if (!in_reach(w, *actor, *src, reach_p, 0, &r)) { r.failure = "source out of reach"; return r; }
    const std::int64_t tox = pint(w, *src, "toxicity", 0);
    std::int64_t strain = pint(w, *src, "strain_ref", 0);
    if (strain == 0) strain = pint(w, *src, "pathogen_strain", 0);
    if (tox <= 0 && strain <= 0) { r.failure = "source carries neither toxin nor pathogen"; return r; }
    const Entity* we = w.find(weapon);
    const PropId ct_p = SIMLAB_PID(w.props, "coat_toxin");
    const PropId cs_p = SIMLAB_PID(w.props, "coat_strain");
    // coat_toxin is a signed int8 charge (spec 1.2): clamp into range so
    // commit validation accepts potent sources (e.g. toxicity 250).
    if (tox > 0) setc(r, weapon, ct_p, w.prop_int(*we, ct_p, 0),
                      std::min<std::int64_t>(128,
                          std::max(w.prop_int(*we, ct_p, 0), tox)));
    if (strain > 0) setc(r, weapon, cs_p, w.prop_int(*we, cs_p, 0), strain);
    const PropId q_p = SIMLAB_PID(w.props, "quantity");
    const PropId integ_p = SIMLAB_PID(w.props, "integrity");
    if (q_p != kNoProp && src->props.get(q_p) && w.prop_int(*src, q_p, 0) > 1)
        setc(r, src->id, q_p, w.prop_int(*src, q_p, 0), w.prop_int(*src, q_p, 0) - 1);
    else
        setc(r, src->id, integ_p, w.prop_int(*src, integ_p, 500), 0);
    if (!charge_effort(w, *actor, SIMLAB_PID(w.props, "energy"), 300, r)) return r;
    r.possible = true; r.ticks = 2;
    r.notes.push_back("coated weapon " + std::to_string(weapon) + " tox=" +
                      std::to_string(tox) + " strain=" + std::to_string(strain));
    return r;
}

PredictionResult predict_don(const World& w, const InteractionContext& ctx, LawId law, bool off) {
    PredictionResult r; r.law = law; r.law_name = "warfare.armor";
    const Entity* actor = w.find(ctx.actor);
    if (!actor || !actor->alive) { r.failure = "no living actor"; return r; }
    const Entity* a = w.find(ctx.targets[0]);
    if (!a || !a->alive) { r.failure = "no armor"; return r; }
    if (off) {
        if (!w.related(ctx.actor, "wearing", a->id)) { r.failure = "not wearing it"; return r; }
        r.rel_changes.push_back({ctx.actor, "wearing", a->id, false});
        r.possible = true; r.ticks = 1;
        return r;
    }
    const PropId cont_p = SIMLAB_PID(w.props, "contained");
    if (is_contained(w, *a, cont_p)) { r.failure = "armor is built into a structure"; return r; }
    if (!in_reach(w, *actor, *a, SIMLAB_PID(w.props, "reach"), 0, &r)) { r.failure = "armor out of reach"; return r; }
    const std::int64_t apr = pint(w, *a, "armor_penetration_resist", 0);
    add_cond(r, "armor_penetration_resist", apr, 1);
    if (apr <= 0) { r.failure = "it protects nothing"; return r; }
    if (w.related(ctx.actor, "wearing", a->id)) { r.failure = "already worn"; return r; }
    r.rel_changes.push_back({ctx.actor, "wearing", a->id, true});
    r.possible = true; r.ticks = 2;
    r.notes.push_back("donned armor apr=" + std::to_string(apr));
    return r;
}

PredictionResult predict_mobilize(const World& w, const InteractionContext& ctx, LawId law) {
    PredictionResult r; r.law = law; r.law_name = "warfare.mobilize";
    const Entity* actor = w.find(ctx.actor);
    const Entity* foe = w.find(ctx.targets[0]);
    if (!actor || !actor->alive || !foe) { r.failure = "no leader/foe"; return r; }
    const PropId lang_p = SIMLAB_PID(w.props, "language_id");
    const PropId lname_p = SIMLAB_PID(w.props, "language_name");
    const std::int64_t la = w.prop_int(*actor, lang_p, 0);
    std::int64_t lb = w.prop_int(*foe, lang_p, 0);
    if (lb == 0 && foe->kind_name == SIMLAB_KIND("language")) lb = w.prop_int(*foe, lname_p, 0);
    if (la <= 0 || lb <= 0 || la == lb) { r.failure = "no foreign community to march on"; return r; }
    // war needs a casus belli: the ledgers must already say "hostile"
    if (!diplo_hostile(w, la, lb)) { r.failure = "communities are not hostile"; return r; }
    // and a leader people follow: mandate or deference
    const std::int64_t range = std::max<std::int64_t>(400,
        pint(w, *actor, "communicate_range", 400));
    std::vector<EntityId> host = followers_of(w, ctx.actor, la, range);
    if (!pol_holds_mandate(w, ctx.actor) && host.empty()) {
        r.failure = "nobody follows this leader"; return r;
    }
    host.insert(host.begin(), ctx.actor);
    std::int64_t depot = 0;
    if (ctx.parameters.note.rfind("depot:", 0) == 0) {
        try { depot = std::stoll(ctx.parameters.note.substr(6)); } catch (...) { depot = 0; }
        const Entity* de = depot > 0 ? w.find(static_cast<EntityId>(depot)) : nullptr;
        if (!de || !de->alive || !is_enclosure(*de)) { r.failure = "depot is not a standing enclosure"; return r; }
    }
    const PropId wt_p = SIMLAB_PID(w.props, "war_target");
    const PropId cm_p = SIMLAB_PID(w.props, "combat_morale");
    const PropId ro_p = SIMLAB_PID(w.props, "routed");
    const PropId sd_p = SIMLAB_PID(w.props, "supply_depot_link");
    for (EntityId s : host) {
        const Entity* se = w.find(s);
        auto setv = [&](PropId p, std::int64_t v) {
            if (p == kNoProp) return;
            StateChange ch; ch.entity = s; ch.prop = p;
            const Value* old = se->props.get(p);
            ch.had_before = old != nullptr;
            ch.before = old ? *old : Value::integer(0);
            ch.after = Value::integer(v);
            if (!old || old->i != v) r.changes.push_back(ch);
        };
        setv(wt_p, lb);
        setv(cm_p, std::max<std::int64_t>(600, w.prop_int(*se, cm_p, 0)));
        setv(ro_p, 0);
        if (depot > 0) setv(sd_p, depot);
    }
    if (!charge_energy(*actor, SIMLAB_PID(w.props, "energy"), 200, r)) return r;
    r.possible = true; r.ticks = 2;
    r.notes.push_back("mobilized " + std::to_string(host.size()) + " against tongue " +
                      std::to_string(lb));
    return r;
}

}  // namespace

PredictionResult WarfareModel::predict(const World& w, const InteractionContext& ctx) const {
    const std::string& a = ctx.parameters.action;
    if (a == "launch") return predict_launch(w, ctx, id());
    if (a == "coat") return predict_coat(w, ctx, id());
    if (a == "don") return predict_don(w, ctx, id(), false);
    if (a == "doff") return predict_don(w, ctx, id(), true);
    if (a == "mobilize") return predict_mobilize(w, ctx, id());
    PredictionResult r; r.law = id(); r.failure = "unknown warfare act";
    return r;
}

std::unique_ptr<INaturalLawModel> WarfareModel::clone() const {
    return std::make_unique<WarfareModel>();
}

}  // namespace simlab
