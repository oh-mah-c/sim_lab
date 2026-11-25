// BiologyLaws.cpp — Phase 7: biological evolution core (TODO §8.4/§12.1).
//
// One generic action: "reproduce" — actor + one partner.
//
// Compatibility is STRUCTURAL, never nominal: both parents must be living
// organisms carrying genomes with the SAME kind_name and the SAME sorted
// locus set. No species table is consulted — two populations can only cross
// if their heredity schemas happen to match.
//
// Gates (all property-driven, all recorded as Conditions on failure):
//   maturity  — `age` >= `adult_age` (default kDefaultAdultAge ticks)
//   fertility — `fertility` > 0 permille on BOTH parents
//   hunger    — actor `hunger` < 900 when the prop exists (a starving parent
//               does not reproduce; this is the selection pressure, not a rule)
//   reach     — partner inside the actor's `reach`
//   energy    — actor pays `reproduction_cost` milliJ (or ctx force)
//
// Heredity: each locus picks ONE parent's allele. The choice is a pure hash
// of (world seed, parent ids, clock tick, locus index) — deterministic,
// clone-safe (plan_multi speculation sees the same outcome), and uses no RNG
// stream state. Mutation fires at the actor's `mutation_rate` permille; the
// delta is a small hash-derived step clamped to the property's declared
// range. Expression is identity for numeric traits: allele == prop value.
//
// Nothing here names a species, a trait, or a "good" allele. If the
// environment rewards a variant, the variant's carriers simply leave more
// descendants — that is the whole mechanism.

#include "simlab/Laws.hpp"
#include "simlab/World.hpp"
#include "simlab/Species.hpp"
#include "simlab/Rng.hpp"
#include <algorithm>

namespace simlab {
namespace {

constexpr std::int64_t kDefaultReach = 64;
constexpr std::int64_t kDefaultAdultAge = 100;    // ticks
constexpr std::int64_t kDefaultMutationRate = 25; // permille per locus
constexpr std::int64_t kDefaultReproCooldown = 200; // ticks between offspring
constexpr std::int64_t kFertilityMin = 1;         // permille — must be > 0
// Parental budget (mass/energy conservation): each parent pays half the
// offspring's initial energy reserve; the other half-equivalent is the
// biological overhead of gestation. The child is spawned WITH
// kChildInitEnergy milliJ — energy moves parent -> offspring, it is
// neither created nor destroyed. A parent that cannot pay, or is too
// hungry to gestate, fails physically — scarcity stops births through
// the same laws as everything else, never through a population cap.
constexpr std::int64_t kChildInitEnergy = 40000;  // milliJ in the newborn
constexpr std::int64_t kParentEnergyShare = 40000;// milliJ per parent
constexpr std::int64_t kParentHungerCost = 300;   // permille hunger per birth
constexpr std::int64_t kParentHungerMax = 500;    // permille — above = too hungry

PropId prop_id(const World& w, const char* name) {
    static thread_local LitPidCache c_;
    return c_.get(w.props, name);
}

bool prop_bool(const World& w, const Entity& e, PropId p) {
    const Value* v = w.prop_of(e, p);
    return v && ((v->type == ValType::Boolean && v->b) ||
                 (v->type == ValType::Integer && v->i != 0));
}

void set_change(PredictionResult& r, EntityId id, PropId prop,
                std::int64_t before, std::int64_t after) {
    StateChange ch;
    ch.entity = id; ch.prop = prop; ch.had_before = true;
    ch.before = Value::integer(before);
    ch.after = Value::integer(after);
    r.changes.push_back(ch);
}

void add_cond(PredictionResult& r, const char* what, std::int64_t actual,
              std::int64_t required) {
    Condition c; c.what = what; c.actual = actual; c.required = required;
    c.met = actual >= required;
    r.conditions.push_back(std::move(c));
}

bool in_reach(const World& w, const Entity& actor, const Entity& target,
              PropId reach_pid, PredictionResult* r) {
    const std::int64_t reach = reach_pid != kNoProp
        ? w.prop_int(actor, reach_pid, kDefaultReach) : kDefaultReach;
    const std::int64_t d2 = dist2(actor.pos, target.pos);
    if (r) {
        std::int64_t d = 0;
        while (d * d < d2) ++d;
        add_cond(*r, "reach", reach, d);
    }
    return d2 <= reach * reach;
}

// Same heredity schema? Sorted locus PropId sets must match exactly —
// recombination aligns loci by INDEX, so mismatched sets cannot cross.
bool compatible_genomes(const Entity& a, const Entity& b) {
    if (a.genome.loci.empty() || b.genome.loci.empty()) return false;
    if (a.genome.loci.size() != b.genome.loci.size()) return false;
    for (std::size_t i = 0; i < a.genome.loci.size(); ++i)
        if (a.genome.loci[i].first != b.genome.loci[i].first) return false;
    return true;
}

// Deterministic per-(world,parents,tick,locus) hash — no RNG state consumed,
// so predict() is pure and clone-safe.
std::uint64_t heredity_hash(std::uint64_t seed, EntityId a, EntityId b,
                            std::uint64_t tick, std::uint64_t locus,
                            std::uint64_t salt) {
    std::uint64_t h = seed ^ Rng::hash(static_cast<std::uint64_t>(a))
                    ^ Rng::hash(static_cast<std::uint64_t>(b) + 0x9E3779B9ull)
                    ^ Rng::hash(tick + 0x85EBCA6Bull)
                    ^ Rng::hash(locus + salt * 0xC2B2AE35ull);
    h ^= h >> 29; h *= 0x94D049BB133111EBull; h ^= h >> 32;
    return h;
}

// Recombine + mutate one allele. Returns the offspring allele for locus i.
std::int64_t inherit_allele(const World& w, const Entity& a, const Entity& b,
                            std::uint64_t tick, std::size_t i,
                            std::int64_t mutation_rate) {
    const std::uint64_t h = heredity_hash(w.world_seed, a.id, b.id, tick, i, 1);
    const Genome& src = (h & 1) ? b.genome : a.genome;
    std::int64_t allele = src.loci[i].second;
    if (mutation_rate <= 0) return allele;
    const std::uint64_t hm = heredity_hash(w.world_seed, a.id, b.id, tick, i, 2);
    if (hm % 1000 >= static_cast<std::uint64_t>(mutation_rate)) return allele;
    // mutation: small hash-derived step inside the property's declared range
    const PropId pid = a.genome.loci[i].first;
    const PropertyDef* pd = w.props.find(pid);
    const std::int64_t lo = pd && pd->has_range ? pd->lo : allele - 50;
    const std::int64_t hi = pd && pd->has_range ? pd->hi : allele + 50;
    const std::int64_t span = std::max<std::int64_t>(1, hi - lo);
    const std::int64_t step = std::max<std::int64_t>(1, span / 20);
    const std::uint64_t hd = heredity_hash(w.world_seed, a.id, b.id, tick, i, 3);
    const std::int64_t delta = static_cast<std::int64_t>(hd % (2 * step + 1)) - step;
    allele = std::clamp<std::int64_t>(allele + delta, lo, hi);
    return allele;
}

}  // namespace

LawId BiologyModel::id() const { return 7; }

bool BiologyModel::supports(const World& w, const InteractionContext& ctx) const {
    (void)w;
    if (ctx.targets.size() != 1) return false;
    const std::string& a = ctx.parameters.action;
    return a == "reproduce" || a == "sow" || a == "irrigate";
}

// ---- Phase 18: cultivation -----------------------------------------------
// `sow` — actor + one propagule (an entity carrying `sprout_target`, i.e.
// a seed that already knows what it becomes). The seed is consumed and the
// declared species sprouts at the actor's feet as a `cultivated` crop with
// `growth` 0 — it fruits only after `growth_ticks` of drift (World.cpp).
// `irrigate` — actor + one fruiting plant, requires standing water within
// reach of the plant (the channel/haul constraint is geography, not a
// tech flag); grants `irrigation_credit` ticks of watered soil.
namespace {

constexpr std::int64_t kSowCost = 60;        // milliJ
constexpr std::int64_t kIrrigateCost = 40;
constexpr std::int64_t kIrrigateCredit = 2000; // watered-soil ticks granted
constexpr std::int64_t kIrrigateWaterScan = 6; // cells around the plant

PredictionResult predict_sow(const World& w, const InteractionContext& ctx) {
    PredictionResult r; r.law = 7; r.law_name = "biology.cultivation";
    const Entity* actor = w.find(ctx.actor);
    const Entity* seed = w.find(ctx.targets.front());
    if (!actor || !actor->alive) { r.failure = "no living actor"; return r; }
    if (!seed || !seed->alive) { r.failure = "no living seed"; return r; }
    const PropId contained = prop_id(w, "contained");
    if (contained != kNoProp && prop_bool(w, *actor, contained)) {
        r.failure = "actor is contained"; return r;
    }
    const PropId p_target = prop_id(w, "sprout_target");
    const Value* tv = p_target != kNoProp ? w.prop_of(*seed, p_target) : nullptr;
    if (!tv || tv->type != ValType::Text || tv->s.empty()) {
        r.failure = "target is not a propagule (no sprout_target)"; return r;
    }
    const PropId reach_p = prop_id(w, "reach");
    if (!in_reach(w, *actor, *seed, reach_p, &r)) {
        r.failure = "seed out of reach"; return r;
    }
    if (!w.terrain.empty() && w.terrain.contains(actor->pos) &&
        w.terrain.is_water(actor->pos)) {
        r.failure = "cannot sow on water"; return r;
    }
    const PropId energy_p = prop_id(w, "energy");
    const Value* ev = energy_p != kNoProp ? w.prop_of(*actor, energy_p) : nullptr;
    const std::int64_t e0 = ev ? ev->i : 0;
    add_cond(r, "energy", e0, kSowCost);
    if (energy_p == kNoProp || !ev || ev->i < kSowCost) {
        r.failure = "actor lacks energy to sow"; return r;
    }
    set_change(r, actor->id, energy_p, e0, e0 - kSowCost);
    // the propagule is spent — integrity 0 reaps it through the normal path
    const PropId integ = prop_id(w, "integrity");
    if (integ != kNoProp) {
        const std::int64_t si = w.prop_int(*seed, integ, 100);
        set_change(r, seed->id, integ, si, 0);
    }
    // the crop: species props, bare of fruit, marked cultivated at growth 0
    SpawnSpec s;
    s.pos = actor->pos;
    if (const SpeciesDef* sp = w.species.find_name(tv->s)) {
        s.kind = sp->name;
        s.material = sp->material;
        s.props = sp->props;
    } else {
        s.kind = tv->s;
        const PropId p_sm = prop_id(w, "sprout_material");
        const Value* mv = p_sm != kNoProp ? w.prop_of(*seed, p_sm) : nullptr;
        if (mv && mv->type == ValType::Text)
            s.material = w.materials.id_of(mv->s);
    }
    const PropId p_fc = prop_id(w, "fruit_count");
    if (p_fc != kNoProp && s.props.get(p_fc))
        s.props.set(p_fc, Value::integer(0));
    const PropId p_cult = prop_id(w, "cultivated");
    if (p_cult != kNoProp) s.props.set(p_cult, Value::integer(1));
    const PropId p_g = prop_id(w, "growth");
    if (p_g != kNoProp) s.props.set(p_g, Value::integer(0));
    r.spawns.push_back(s);
    r.possible = true; r.ticks = 1;
    r.notes.push_back("sow '" + tv->s + "'");
    return r;
}

PredictionResult predict_irrigate(const World& w, const InteractionContext& ctx) {
    PredictionResult r; r.law = 7; r.law_name = "biology.cultivation";
    const Entity* actor = w.find(ctx.actor);
    const Entity* plant = w.find(ctx.targets.front());
    if (!actor || !actor->alive) { r.failure = "no living actor"; return r; }
    if (!plant || !plant->alive) { r.failure = "no living plant"; return r; }
    const PropId p_fc = prop_id(w, "fruit_count");
    if (p_fc == kNoProp || !w.prop_of(*plant, p_fc)) {
        r.failure = "target is not a fruiting plant"; return r;
    }
    const PropId reach_p = prop_id(w, "reach");
    if (!in_reach(w, *actor, *plant, reach_p, &r)) {
        r.failure = "plant out of reach"; return r;
    }
    // water must be near the PLANT — irrigation is geography, not a flag:
    // a dry field far from a channel cannot be watered by wishful thinking.
    // The scan radius is in terrain CELLS (kIrrigateWaterScan), converted
    // to world units by the grid pitch.
    if (w.terrain.empty()) { r.failure = "no terrain"; return r; }
    const std::int64_t cell = std::max(1, w.terrain.cell);
    const std::int64_t scan = kIrrigateWaterScan * cell;
    bool water = false;
    for (std::int64_t dy = -scan; dy <= scan && !water; dy += cell)
        for (std::int64_t dx = -scan; dx <= scan; dx += cell) {
            const Vec2 q{plant->pos.x + static_cast<std::int32_t>(dx),
                         plant->pos.y + static_cast<std::int32_t>(dy)};
            if (w.terrain.contains(q) && w.terrain.is_water(q)) { water = true; break; }
        }
    if (!water) { r.failure = "no water within irrigation reach"; return r; }
    const PropId energy_p = prop_id(w, "energy");
    const Value* ev = energy_p != kNoProp ? w.prop_of(*actor, energy_p) : nullptr;
    const std::int64_t e0 = ev ? ev->i : 0;
    add_cond(r, "energy", e0, kIrrigateCost);
    if (energy_p == kNoProp || !ev || ev->i < kIrrigateCost) {
        r.failure = "actor lacks energy to irrigate"; return r;
    }
    set_change(r, actor->id, energy_p, e0, e0 - kIrrigateCost);
    const PropId p_ic = prop_id(w, "irrigation_credit");
    if (p_ic != kNoProp) {
        const std::int64_t ic0 = w.prop_int(*plant, p_ic, 0);
        set_change(r, plant->id, p_ic, ic0,
                   std::min<std::int64_t>(100000, ic0 + kIrrigateCredit));
    }
    r.possible = true; r.ticks = 1;
    r.notes.push_back("irrigate +" + std::to_string(kIrrigateCredit) + " ticks");
    return r;
}

}  // namespace

PredictionResult BiologyModel::predict(const World& w,
                                       const InteractionContext& ctx) const {
    if (ctx.parameters.action == "sow") return predict_sow(w, ctx);
    if (ctx.parameters.action == "irrigate") return predict_irrigate(w, ctx);
    PredictionResult r;
    r.law = id(); r.law_name = name();
    const Entity* actor = w.find(ctx.actor);
    const Entity* partner = w.find(ctx.targets.front());
    if (!actor || !actor->alive) { r.failure = "no living actor"; return r; }
    if (!partner || !partner->alive || partner->id == actor->id) {
        r.failure = "no living partner"; return r;
    }
    const PropId contained = prop_id(w, "contained");
    if (contained != kNoProp && prop_bool(w, *actor, contained)) {
        r.failure = "actor is contained"; return r;
    }
    if (contained != kNoProp && prop_bool(w, *partner, contained)) {
        r.failure = "partner is contained"; return r;
    }

    // ---- heredity compatibility -------------------------------------------
    if (actor->genome.loci.empty() || partner->genome.loci.empty()) {
        r.failure = "no genome"; return r;
    }
    if (actor->kind_name != partner->kind_name ||
        !compatible_genomes(*actor, *partner)) {
        r.failure = "incompatible genomes"; return r;
    }

    // ---- maturity -----------------------------------------------------------
    const PropId age_p = prop_id(w, "age");
    const PropId adult_p = prop_id(w, "adult_age");
    for (const Entity* p : {actor, partner}) {
        const std::int64_t adult_age = adult_p != kNoProp
            ? w.prop_int(*p, adult_p, kDefaultAdultAge) : kDefaultAdultAge;
        const std::int64_t age = age_p != kNoProp ? w.prop_int(*p, age_p, 0) : 0;
        add_cond(r, "age", age, adult_age);
        if (age < adult_age) {
            r.failure = "not mature (age " + std::to_string(age) +
                        " < " + std::to_string(adult_age) + ")";
            return r;
        }
    }

    // ---- fertility ----------------------------------------------------------
    const PropId fert_p = prop_id(w, "fertility");
    for (const Entity* p : {actor, partner}) {
        const std::int64_t f = fert_p != kNoProp ? w.prop_int(*p, fert_p, 0) : 0;
        add_cond(r, "fertility", f, kFertilityMin);
        if (f <= 0) { r.failure = "sterile"; return r; }
    }

    // ---- reach ---------------------------------------------------------------
    const PropId reach_p = prop_id(w, "reach");
    if (!in_reach(w, *actor, *partner, reach_p, &r)) {
        r.failure = "partner out of reach"; return r;
    }

    // ---- reproduction cooldown (Phase 9): World-Truth timer on the actor ----
    // `last_reproduce` records the clock tick of the last successful birth;
    // `repro_cooldown` (entity/species prop, default kDefaultReproCooldown)
    // gates the next. Without this, Driver's controller-side cooldown was
    // the only brake and population grew exponentially -> OOM.
    const PropId last_rep_p = prop_id(w, "last_reproduce");
    const PropId cool_p = prop_id(w, "repro_cooldown");
    const Value* last_rep_val = nullptr;
    std::int64_t cooldown = kDefaultReproCooldown;
    if (cool_p != kNoProp) cooldown = w.prop_int(*actor, cool_p, cooldown);
    if (cooldown > 0 && last_rep_p != kNoProp) {
        last_rep_val = w.prop_of(*actor, last_rep_p);
        const std::int64_t last = last_rep_val ? last_rep_val->i
            : -static_cast<std::int64_t>(cooldown);
        const std::int64_t elapsed =
            static_cast<std::int64_t>(w.clock_.tick()) - last;
        add_cond(r, "repro_cooldown", elapsed, cooldown);
        if (elapsed < cooldown) {
            r.failure = "reproduction cooldown (" +
                        std::to_string(elapsed) + "/" +
                        std::to_string(cooldown) + " ticks)";
            return r;
        }
    }

    // ---- parental budget: BOTH parents pay for the offspring ------------------
    // Each contributes kParentEnergyShare milliJ of `energy` (half the
    // child's initial reserve; the rest is gestation overhead) plus
    // kParentHungerCost hunger. A parent with no energy reserve, too little
    // energy, or hunger above kParentHungerMax cannot reproduce — the
    // offspring's kChildInitEnergy is carved out of what the parents paid,
    // so no energy is created from nothing.
    const PropId energy_p = prop_id(w, "energy");
    const PropId hunger_p = prop_id(w, "hunger");
    std::int64_t parent_energy[2] = {0, 0};
    {
        int pi = 0;
        for (const Entity* p : {actor, partner}) {
            const Value* ev = energy_p != kNoProp
                ? w.prop_of(*p, energy_p) : nullptr;
            const std::int64_t have = ev ? ev->i : 0;
            parent_energy[pi++] = have;
            add_cond(r, "parent_energy", have, kParentEnergyShare);
            if (hunger_p != kNoProp) {
                const std::int64_t h = w.prop_int(*p, hunger_p, 0);
                Condition hc;
                hc.what = "parent_hunger"; hc.actual = h;
                hc.required = kParentHungerMax; hc.met = h <= kParentHungerMax;
                r.conditions.push_back(std::move(hc));
                if (h > kParentHungerMax) {
                    r.failure = "parent too hungry to reproduce (hunger " +
                                std::to_string(h) + " > " +
                                std::to_string(kParentHungerMax) + ")";
                    return r;
                }
            }
            if (energy_p == kNoProp || !ev || ev->i < kParentEnergyShare) {
                r.failure = "parent lacks energy reserve for offspring";
                return r;
            }
        }
    }

    // ---- offspring genome: recombine + mutate ---------------------------------
    const std::uint64_t tick = w.clock_.tick();
    // stamp the cooldown + charge gestation hunger on the actor — both are
    // causal StateChanges, so ledger replay and state_hash see them.
    if (last_rep_p != kNoProp) {
        StateChange ch;
        ch.entity = actor->id; ch.prop = last_rep_p;
        ch.had_before = last_rep_val != nullptr;
        ch.before = last_rep_val ? *last_rep_val : Value::integer(0);
        ch.after = Value::integer(static_cast<std::int64_t>(tick));
        r.changes.push_back(ch);
    }
    // Parental payments — symmetric: BOTH parents lose kParentEnergyShare
    // energy and gain kParentHungerCost hunger (gestation). Recorded as
    // causal StateChanges so the ledger shows exactly where the child's
    // initial energy came from.
    {
        int pi = 0;
        for (const Entity* p : {actor, partner}) {
            if (energy_p != kNoProp)
                set_change(r, p->id, energy_p, parent_energy[pi],
                           parent_energy[pi] - kParentEnergyShare);
            if (hunger_p != kNoProp) {
                const Value* hv = w.prop_of(*p, hunger_p);
                if (hv && hv->type == ValType::Integer) {
                    const std::int64_t nh =
                        std::min<std::int64_t>(1000, hv->i + kParentHungerCost);
                    if (nh != hv->i)
                        set_change(r, p->id, hunger_p, hv->i, nh);
                }
            }
            ++pi;
        }
    }
    const std::int64_t mut = [&] {
        const PropId mp = prop_id(w, "mutation_rate");
        return mp != kNoProp ? w.prop_int(*actor, mp, kDefaultMutationRate)
                             : kDefaultMutationRate;
    }();
    Genome child;
    for (std::size_t i = 0; i < actor->genome.loci.size(); ++i) {
        const PropId pid = actor->genome.loci[i].first;
        child.set(pid, inherit_allele(w, *actor, *partner, tick, i, mut));
    }

    // Structural mutation — locus gain/loss (speciation). Rare: derived from
    // the actor's mutation_rate (permille / 20). A gained locus is drawn from
    // the species' declared gene pool (data/species.txt `gene` lines) that the
    // child does not already carry; a lost locus is any existing one (never
    // the last). Because compatibility requires an IDENTICAL locus set, a
    // lineage that gains or loses a locus is reproductively isolated from
    // its parents — species emerge from divergence, never from a table.
    std::string struct_note;
    const std::int64_t struct_p = mut / 20;
    if (struct_p > 0 && !child.loci.empty()) {
        const std::uint64_t sh = heredity_hash(w.world_seed, actor->id,
                                               partner->id, tick,
                                               child.loci.size(), 7);
        if (sh % 1000 < static_cast<std::uint64_t>(struct_p)) {
            const std::uint64_t sh2 = heredity_hash(w.world_seed, actor->id,
                                                    partner->id, tick,
                                                    child.loci.size(), 8);
            if ((sh2 & 1) && child.loci.size() > 1) {
                const std::size_t idx =
                    static_cast<std::size_t>(sh2 % child.loci.size());
                const PropId lost = child.loci[idx].first;
                child.loci.erase(child.loci.begin() +
                                 static_cast<std::ptrdiff_t>(idx));
                const PropertyDef* ld = w.props.find(lost);
                struct_note = "locus loss: " + (ld ? ld->name : "?");
            } else {
                const SpeciesDef* sp = w.species.find_name(actor->kind_name);
                if (sp) {
                    std::vector<PropId> pool;
                    for (const PropId g : sp->genes)
                        if (!child.get(g)) pool.push_back(g);
                    if (!pool.empty()) {
                        const PropId np = pool[sh2 % pool.size()];
                        const PropertyDef* pd = w.props.find(np);
                        std::int64_t allele = 0;
                        if (pd && pd->has_range)
                            allele = pd->lo + static_cast<std::int64_t>(
                                heredity_hash(w.world_seed, actor->id,
                                              partner->id, tick, np, 9) %
                                static_cast<std::uint64_t>(pd->hi - pd->lo + 1));
                        else
                            allele = w.prop_int(*actor, np, 0);
                        child.set(np, allele);
                        struct_note = "locus gain: " +
                                      (pd ? pd->name : "?");
                    }
                }
            }
        }
    }

    // ---- offspring phenotype: expressed props ---------------------------------
    // Identity expression for numeric traits: allele -> prop value (clamped
    // to the declared range). Needs start at zero so the child metabolizes
    // like every other organism — immortality by omission is not allowed.
    SpawnSpec s;
    s.kind = actor->kind_name;
    s.material = actor->material;
    s.pos = {(actor->pos.x + partner->pos.x) / 2,
             (actor->pos.y + partner->pos.y) / 2};
    s.genome = child;
    // ---- CL-1 brain inheritance (Baldwin, Phase 2) ---------------------------
    // Only SLOW weights + hyper-genes cross the generational boundary; the
    // newborn's h/replay/fast state starts empty (installed at spawn-commit).
    // Reproductive isolation: hidden-size mismatch OR mean weight distance
    // beyond kMaxBrainGeneticDistance -> the pairing is refused, which is
    // how divergent lineages become separate species without a species table.
    if (w.brains_) {
        const BrainGenome ga = brain_genome_of(w, *w.brains_, actor->id);
        const BrainGenome gb = brain_genome_of(w, *w.brains_, partner->id);
        if (ga.valid || gb.valid) {
            if (!ga.valid || !gb.valid || ga.hidden != gb.hidden ||
                brain_genetic_distance(ga, gb) > kMaxBrainGeneticDistance) {
                r.failure =
                    "brain genomes incompatible (reproductive isolation)";
                return r;
            }
            s.brain = brain_child_genome(w, actor->id, partner->id, ga, gb);
        }
    }
    // Epigenetic transmission (Nhịp 3): a parent's EXTREME consolidated
    // trauma — |charge| >= inherit_charge AND stability >= inherit_stability
    // — becomes a vague born-mark in the child, at most 2 marks total.
    // No disease is inherited by name; only the scar itself, keyed by the
    // material/kind that caused it. Peace fades it; renewal consolidates it.
    {
        const MindParams ip = mind_params(w, *actor);
        auto transmit = [&](EntityId who) {
            const AgentMind* mp = w.find_mind(who);
            if (!mp) return;
            auto scan = [&](MaterialId mat, const std::string& kind,
                            const MemoryEngram& g) {
                if (s.legacy.size() >= 2) return;
                if (g.emotional_charge <= -ip.inherit_charge &&
                    g.stability >= ip.inherit_stability)
                    s.legacy.push_back({mat, kind, g.emotional_charge});
            };
            for (const auto& [eid, snap] : mp->memory)
                scan(snap.material, snap.kind_name, snap.engram);
            for (const BornEngram& bm : mp->born_memories)
                scan(bm.material, bm.kind_name, bm.engram);
        };
        transmit(actor->id);
        transmit(partner->id);
    }
    const PropId hunger2 = prop_id(w, "hunger"), thirst2 = prop_id(w, "thirst"),
               fatigue2 = prop_id(w, "fatigue");
    for (const auto& [pid, allele] : child.loci) {
        const PropertyDef* pd = w.props.find(pid);
        if (!pd || (pd->type != ValType::Integer && pd->type != ValType::Fixed))
            continue;
        std::int64_t v = allele;
        if (pd->has_range) v = std::clamp<std::int64_t>(v, pd->lo, pd->hi);
        s.props.set(pid, Value::integer(v));
    }
    if (hunger2 != kNoProp) s.props.set(hunger2, Value::integer(0));
    if (thirst2 != kNoProp) s.props.set(thirst2, Value::integer(0));
    if (fatigue2 != kNoProp) s.props.set(fatigue2, Value::integer(0));
    // The newborn's energy reserve is carved out of what BOTH parents
    // paid — without this prop the child was unbounded (missing energy
    // used to mean "free actions forever").
    if (energy_p != kNoProp)
        s.props.set(energy_p, Value::integer(kChildInitEnergy));
    // Phase 10.4 passive immunity: the mother passes up to 2 of her
    // strongest cleared-antigen signatures to the newborn as temporary
    // immune memory — not heritable alleles, a borrowed shield that fades
    // with `passive_immune` ticks.
    {
        const PropId ic_p = prop_id(w, "immune_count");
        int n = ic_p != kNoProp ? static_cast<int>(w.prop_int(*actor, ic_p, 0))
                               : 0;
        n = std::min(n, 8);
        const int give = std::min(n, 2);
        if (give > 0) {
            for (int i = 0; i < give; ++i) {
                const PropId sp = prop_id(w, ("immune_sig" +
                                              std::to_string(i)).c_str());
                if (sp == kNoProp) continue;
                s.props.set(sp, Value::integer(
                    w.prop_int(*actor, sp, 0)));
            }
            const PropId cc = prop_id(w, "immune_count");
            if (cc != kNoProp) s.props.set(cc, Value::integer(give));
            const PropId psc = prop_id(w, "passive_sig_count");
            if (psc != kNoProp) s.props.set(psc, Value::integer(give));
            const PropId pp = prop_id(w, "passive_immune");
            if (pp != kNoProp)
                s.props.set(pp, Value::integer(2000));
        }
    }
    s.rel_from.push_back({"parent_of", actor->id});
    s.rel_from.push_back({"parent_of", partner->id});
    r.spawns.push_back(s);
    // Phase 11.3: monotonic per-actor reproduction counter — the epoch salt
    // for heritable-trait mutation rolls, mirroring drift_rolls.
    {
        const PropId rmr = prop_id(w, "repro_mutation_rolls");
        if (rmr != kNoProp) {
            const std::int64_t rv = w.prop_int(*actor, rmr, 0);
            set_change(r, actor->id, rmr, rv, rv + 1);
        }
    }

    r.possible = true;
    r.ticks = ctx.parameters.ticks > 0 ? ctx.parameters.ticks : 4;
    r.notes.push_back("offspring genome: " +
                      std::to_string(child.loci.size()) + " loci, mutation_rate=" +
                      std::to_string(mut) + (struct_note.empty() ? "" : "; " + struct_note));
    return r;
}

std::unique_ptr<INaturalLawModel> BiologyModel::clone() const {
    return std::make_unique<BiologyModel>();
}

}  // namespace simlab
