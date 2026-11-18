// Cognition.cpp — TODO §10 cognitive layer.
//
// Hard rules enforced here:
//  * Perception copies ONLY props whose domain is "observable" — World Truth
//    (deposit composition, reserve, nutrition, ...) never enters a mind.
//  * Minds never mutate entities; they only choose which interactions the
//    NPC attempts through World::interact.
//  * Planning evaluates candidates with law predict() / speculative World
//    clones — simulation, never belief — and only over actions the NPC has
//    learned or witnessed.
//  * Needs are body-state props (domain "need"): private to the agent, read
//    directly from its own entity — never through perception of others.

#include "simlab/Cognition.hpp"
#include "simlab/World.hpp"
#include "simlab/Laws.hpp"
#include "simlab/Causal.hpp"
#include "simlab/Discovery.hpp"
#include "simlab/Rng.hpp"
#include <algorithm>
#include <climits>
#include <sstream>

namespace simlab {

// ---- belief status machine (TODO §10.2) ---------------------------------------
// Verified requires PERSONAL repeated success: self_evidence >= 150 and total
// corroborated evidence >= 200 with zero counter-evidence. Witnessed events
// raise evidence_for but never self_evidence — you trust what YOU tested.
namespace {
constexpr std::int64_t kVerifiedSelf = 150;
constexpr std::int64_t kVerifiedFor = 200;
constexpr std::int64_t kSupportedFor = 3;

void recompute_status(Belief& b) {
    if (b.evidence_against > b.evidence_for * 2 && b.evidence_against > 0) {
        b.status = BeliefStatus::Disputed;
    } else if (b.self_evidence >= kVerifiedSelf &&
               b.evidence_for >= kVerifiedFor && b.evidence_against == 0) {
        b.status = BeliefStatus::Verified;
    } else if (b.evidence_for >= kSupportedFor && b.evidence_against == 0) {
        b.status = BeliefStatus::Supported;
    } else if (b.evidence_for > b.evidence_against && b.evidence_for > 0) {
        b.status = BeliefStatus::Tested;
    } else if (b.evidence_for > 0 || b.evidence_against > 0) {
        b.status = BeliefStatus::Observed;
    } else {
        b.status = BeliefStatus::Hypothesized;
    }
}
}  // namespace

// ---- AgentMind ------------------------------------------------------------------

void AgentMind::observe(Observation o) {
    if (!known_entities.insert(o.entity).second) {
        // refresh: drop stale observation of same entity, keep newest
        observations.erase(std::remove_if(observations.begin(), observations.end(),
            [&](const Observation& x) { return x.entity == o.entity; }), observations.end());
    }
    observations.push_back(std::move(o));
    if (observations.size() > kMaxObservations)
        observations.pop_front();               // O(1) on a deque
}

Belief& AgentMind::belief(const std::string& key) {
    auto it = beliefs.find(key);
    if (it == beliefs.end()) {
        Belief b; b.key = key; b.status = BeliefStatus::Hypothesized;
        it = beliefs.emplace(key, b).first;
    }
    return it->second;
}

const Belief* AgentMind::find_belief(const std::string& key) const {
    auto it = beliefs.find(key);
    return it == beliefs.end() ? nullptr : &it->second;
}

void AgentMind::reinforce(const std::string& key, std::int64_t delta,
                          std::uint64_t tick, bool self) {
    Belief& b = belief(key);
    b.evidence_for += delta;
    if (self) b.self_evidence += delta;
    b.confidence = std::min<std::int64_t>(1000, b.confidence + delta);
    b.last_tick = tick;
    recompute_status(b);
}

void AgentMind::weaken(const std::string& key, std::int64_t delta, std::uint64_t tick) {
    Belief& b = belief(key);
    b.evidence_against += delta;
    b.confidence = std::max<std::int64_t>(0, b.confidence - delta);
    b.last_tick = tick;
    recompute_status(b);
}

// ---- perception -----------------------------------------------------------------

// ---- Ebbinghaus memory services (Nhịp 1) ---------------------------------------

namespace {

std::int64_t mem_clamp(std::int64_t v, std::int64_t lo, std::int64_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
std::int64_t mem_abs(std::int64_t v) { return v < 0 ? -v : v; }

// integer square root (Newton) — deterministic, no floats
std::int64_t mem_isqrt(std::int64_t v) {
    if (v <= 0) return 0;
    std::int64_t x = v, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + v / x) / 2; }
    return x;
}

std::int64_t need_of(const World& w, const Entity& e, const char* name) {
    const PropertyDef* d = w.props.find_name(name);
    return d ? w.prop_int(e, d->id, 0) : 0;
}

// Does the target look edible to a beholder? Material nutrition/hydration
// or a fruiting plant with fruit left — the same two-layer rule the
// consume law uses, evaluated on World Truth at encoding time (the NPC
// is looking at the thing; salience is private bookkeeping).
bool looks_edible(const World& w, const Entity& t) {
    if (t.material != kNoMaterial) {
        if (const MaterialDef* md = w.materials.find(t.material)) {
            const PropertyDef* nut = w.props.find_name("nutrition");
            const PropertyDef* hyd = w.props.find_name("hydration");
            if (nut && md->props.count(nut->id) && md->props.at(nut->id).i > 0)
                return true;
            if (hyd && md->props.count(hyd->id) && md->props.at(hyd->id).i > 0)
                return true;
        }
    }
    const PropertyDef* fmp = w.props.find_name("fruit_material");
    const PropertyDef* fcp = w.props.find_name("fruit_count");
    if (!fmp || !fcp) return false;
    const Value* fv = w.prop_of(t, fmp->id);
    const Value* fc = w.prop_of(t, fcp->id);
    return fv && fv->type == ValType::Text && !fv->s.empty() &&
           fc && fc->type == ValType::Integer && fc->i > 0;
}

// Snapshot of what an agent directly witnessed/participated in: observable
// props only, same boundary as perceive(). For a gone entity the caller
// passes the last remembered snapshot instead.
AgentMind::MemorySnapshot snapshot_now(const World& w, EntityId eid,
                                        std::uint64_t tick) {
    AgentMind::MemorySnapshot s;
    s.tick = tick;
    if (const Entity* e = w.find(eid)) {
        s.pos = e->pos;
        s.material = e->material;
        s.kind_name = e->kind_name;
        for (PropId p : w.observable_props())
            if (const Value* v = w.prop_of(*e, p)) s.props[p] = *v;
    }
    return s;
}

// Charge for a freshly perceived entity: survival-relevant signals only.
// Finding food while starving is the classic flashbulb memory.
std::int64_t perceive_charge(const World& w, const Entity& npc,
                             const Entity& target) {
    std::int64_t charge = 0;
    const std::int64_t hunger = need_of(w, npc, "hunger");
    const std::int64_t thirst = need_of(w, npc, "thirst");
    const bool desperate = hunger > 850 || thirst > 850;
    const bool urgent = desperate || hunger > 500 || thirst > 500;
    if (looks_edible(w, target))
        charge += desperate ? 600 : (urgent ? 250 : 0);
    // water is as vital as food: a drinkable-looking target in crisis
    if (urgent && !looks_edible(w, target) && target.material != kNoMaterial) {
        if (const MaterialDef* md = w.materials.find(target.material)) {
            const PropertyDef* hyd = w.props.find_name("hydration");
            if (hyd && md->props.count(hyd->id) && md->props.at(hyd->id).i > 0)
                charge += desperate ? 600 : 250;
        }
    }
    return mem_clamp(charge, -1000, 1000);
}

// Charge for remembering `tid` after causal event `ev`, from `learner`'s
// side. Uses only what the event already records (changes/spawns) plus
// the learner's own body state — never hidden World Truth.
std::int64_t event_charge(const World& w, EntityId learner,
                          const CausalEvent& ev, EntityId tid) {
    std::int64_t charge = 0;
    const PropId integ = w.props.find_name("integrity")
        ? w.props.find_name("integrity")->id : kNoProp;
    // witnessed death: the entity is gone or flagged dead after the event.
    // SIGN CONVENTION (Nhịp 2): trauma/threat is NEGATIVE, satisfaction
    // and discovery POSITIVE — valence now carries meaning, not just size.
    if (const Entity* t = w.find(tid)) {
        if (!t->alive) charge -= 900;
    } else {
        charge -= 900;                    // participant vanished = died
    }
    // the learner themself was harmed — everything seen is charged context
    if (integ != kNoProp) {
        for (const StateChange& c : ev.changes)
            if (c.entity == learner && c.prop == integ &&
                c.before.type == ValType::Integer && c.after.type == ValType::Integer &&
                c.after.i < c.before.i) { charge -= 900; break; }
    }
    // ate/drank in crisis: the meal itself becomes salient (positive)
    if (ev.action == "consume" && tid != learner) {
        const Entity* me = w.find(learner);
        if (me && (need_of(w, *me, "hunger") > 850 ||
                   need_of(w, *me, "thirst") > 850))
            charge += 500;
    }
    // poison aftermath handled separately as a negative charge (-900)
    return mem_clamp(charge, -1000, 1000);
}

}  // namespace

MindParams mind_params(const World& w, const Entity& e) {
    MindParams p;
    auto gi = [&](const char* name, std::int64_t dflt) -> std::int64_t {
        const PropertyDef* d = w.props.find_name(name);
        return d ? w.prop_int(e, d->id, dflt) : dflt;
    };
    p.base_clarity          = gi("mind_base_clarity", p.base_clarity);
    p.max_clarity           = gi("mind_max_clarity", p.max_clarity);
    p.base_stability        = gi("mind_base_stability", p.base_stability);
    p.max_stability         = gi("mind_max_stability", p.max_stability);
    p.decay_rate            = gi("mind_decay_rate", p.decay_rate);
    p.imprint_threshold     = gi("mind_imprint_threshold", p.imprint_threshold);
    p.imprint_base_stability= gi("mind_imprint_base_stability", p.imprint_base_stability);
    p.amygdala_multiplier   = gi("mind_amygdala_multiplier", p.amygdala_multiplier);
    p.rehearsal_stability_bonus = gi("mind_rehearsal_stability_bonus",
                                     p.rehearsal_stability_bonus);
    p.detail_blur_threshold = gi("mind_detail_blur_threshold", p.detail_blur_threshold);
    p.sleep_prune_threshold = gi("mind_sleep_prune_threshold", p.sleep_prune_threshold);
    p.rehearsal_interval    = gi("mind_rehearsal_interval", p.rehearsal_interval);
    p.threat_gain           = gi("mind_threat_gain", p.threat_gain);
    p.reward_gain           = gi("mind_reward_gain", p.reward_gain);
    p.pattern_coupling      = gi("mind_pattern_coupling", p.pattern_coupling);
    p.empathy_coupling      = gi("mind_empathy_coupling", p.empathy_coupling);
    p.low_light_bias        = gi("mind_low_light_bias", p.low_light_bias);
    p.isolation_bias        = gi("mind_isolation_bias", p.isolation_bias);
    p.executive_override    = gi("mind_executive_override_threshold",
                                 p.executive_override);
    p.emotion_decay         = gi("mind_emotion_decay", p.emotion_decay);
    p.trauma_charge         = gi("mind_trauma_charge", p.trauma_charge);
    p.inherit_charge        = gi("mind_inherit_charge", p.inherit_charge);
    p.inherit_stability     = gi("mind_inherit_stability", p.inherit_stability);
    p.mark_clarity          = gi("mind_mark_clarity", p.mark_clarity);
    p.mark_stability        = gi("mind_mark_stability", p.mark_stability);
    p.mark_charge_permille  = gi("mind_mark_charge_permille",
                                 p.mark_charge_permille);
    p.flashback_charge      = gi("mind_flashback_charge", p.flashback_charge);
    p.flashback_stability   = gi("mind_flashback_stability",
                                 p.flashback_stability);
    p.paranoia_coupling     = gi("mind_paranoia_coupling", p.paranoia_coupling);
    p.paranoia_panic        = gi("mind_paranoia_panic", p.paranoia_panic);
    p.paranoia_charge       = gi("mind_paranoia_charge", p.paranoia_charge);
    p.plan_horizon          = gi("mind_plan_horizon", p.plan_horizon);
    p.abstraction_gain      = gi("mind_abstraction_gain", p.abstraction_gain);
    p.motor_precision       = gi("mind_motor_precision", p.motor_precision);
    p.theory_of_mind        = gi("mind_theory_of_mind", p.theory_of_mind);
    p.brain_metabolic_cost  = gi("mind_brain_metabolic_cost",
                                 p.brain_metabolic_cost);
    p.pair_bond_gain        = gi("mind_pair_bond_gain", p.pair_bond_gain);
    p.jealousy_gain         = gi("mind_jealousy_gain", p.jealousy_gain);
    p.parental_drive        = gi("mind_parental_drive", p.parental_drive);
    p.conformity_gain       = gi("mind_conformity_gain", p.conformity_gain);
    p.xenophobia_bias       = gi("mind_xenophobia_bias", p.xenophobia_bias);
    p.ego_defense_gain      = gi("mind_ego_defense_gain", p.ego_defense_gain);
    if (p.plan_horizon < 1) p.plan_horizon = 1;
    if (p.plan_horizon > 6) p.plan_horizon = 6;
    if (p.max_clarity <= 0) p.max_clarity = 1;
    if (p.max_stability <= 0) p.max_stability = 1;
    return p;
}

void rehearse_engram(MemoryEngram& e, const MindParams& p, std::uint64_t tick) {
    e.clarity = p.max_clarity;
    if (static_cast<std::int64_t>(tick - e.last_rehearsal) >= p.rehearsal_interval) {
        ++e.rehearsal_count;
        e.stability = std::min(p.max_stability,
                               e.stability * 2 + p.rehearsal_stability_bonus);
        e.last_rehearsal = tick;
    }
}

void sleep_consolidate(AgentMind& mind, const MindParams& p) {
    // Sleep consolidates the most emotional memories (stability doubles,
    // capped) and prunes noise: anything below sleep_prune_threshold clarity
    // — including unused skills — is forgotten and must be relearned.
    std::vector<std::pair<std::int64_t, EntityId>> ranked;
    ranked.reserve(mind.memory.size());
    for (const auto& [kid, s] : mind.memory)
        ranked.push_back({mem_abs(s.engram.emotional_charge), kid});
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) {
                  if (a.first != b.first) return a.first > b.first;
                  return a.second < b.second;
              });
    const std::size_t top = std::min<std::size_t>(ranked.size(), 8);
    for (std::size_t i = 0; i < top; ++i) {
        auto it = mind.memory.find(ranked[i].second);
        if (it == mind.memory.end()) continue;
        MemoryEngram& g = it->second.engram;
        if (g.emotional_charge != 0)
            g.stability = std::min(p.max_stability, g.stability * 2);
    }
    std::vector<EntityId> prune;
    for (const auto& [kid, s] : mind.memory)
        if (s.engram.clarity > 0 && s.engram.clarity < p.sleep_prune_threshold)
            prune.push_back(kid);
    for (EntityId kid : prune) {
        auto it = mind.memory.find(kid);
        if (it != mind.memory.end()) mind.memory.erase(it);
    }
    std::vector<std::string> gone;
    for (const auto& [k, d] : mind.discoveries)
        if (d.engram.clarity > 0 && d.engram.clarity < p.sleep_prune_threshold)
            gone.push_back(k);
    for (const std::string& k : gone) mind.discoveries.erase(k);
}

void upsert_memory(AgentMind& mind, EntityId eid,
                   const AgentMind::MemorySnapshot& snap,
                   std::int64_t emotional_charge, const MindParams& p) {
    auto it = mind.memory.find(eid);
    const bool fresh = it == mind.memory.end() || it->second.engram.clarity <= 0;
    AgentMind::MemorySnapshot& ms = mind.memory[eid];
    ms.tick = snap.tick; ms.pos = snap.pos; ms.material = snap.material;
    ms.kind_name = snap.kind_name; ms.props = snap.props;
    MemoryEngram& g = ms.engram;
    if (fresh) {
        // Salience encoding: a charge at/above imprint_threshold forges a
        // deep memory in ONE exposure (amygdala); anything else is routine.
        g.emotional_charge = mem_clamp(emotional_charge, -1000, 1000);
        if (mem_abs(g.emotional_charge) >= p.imprint_threshold) {
            g.clarity = p.max_clarity;
            g.stability = std::min(p.max_stability,
                p.imprint_base_stability +
                mem_abs(g.emotional_charge) * p.amygdala_multiplier);
        } else {
            g.clarity = p.base_clarity;
            g.stability = p.base_stability;
        }
        g.rehearsal_count = 0;
        g.decay_accum = 0;
        g.last_rehearsal = 0;
    } else {
        // Re-encounter: keep the stronger emotional trace, refresh clarity,
        // and let rehearsal double stability at the data-set interval.
        if (mem_abs(emotional_charge) > mem_abs(g.emotional_charge))
            g.emotional_charge = mem_clamp(emotional_charge, -1000, 1000);
        rehearse_engram(g, p, snap.tick);
    }
}

// Abstraction (Nhịp 4b): a mind whose abstraction_gain exceeds 1200
// generalizes a FRESH imprint — the charge is copied, scaled by
// (gain-1000)/1000, onto every other VISIBLE entity sharing the source's
// material or kind (same physical constitution, same danger). This is how
// "burned by glowing coal once" becomes "avoid hot-looking things": the
// lesson transfers before direct contact. Only imprints spread; routine
// traces stay entity-specific. Writes belief only — never World Truth.
void generalize_abstraction(const World& w, AgentMind& mind, EntityId src,
                            const AgentMind::MemorySnapshot& snap,
                            std::int64_t charge, const MindParams& p,
                            std::int64_t radius) {
    if (p.abstraction_gain <= 1200 || radius <= 0) return;
    if (mem_abs(charge) < p.imprint_threshold) return;
    const std::int64_t share = mem_clamp(
        charge * (p.abstraction_gain - 1000) / 1000, -1000, 1000);
    if (share == 0) return;
    const std::vector<PropId>& obs = w.observable_props();
    for (EntityId fid : w.entities_near(snap.pos, radius)) {
        if (fid == src) continue;
        const Entity* f = w.find(fid);
        if (!f || !f->alive) continue;
        const bool kin =
            (snap.material != kNoMaterial && f->material == snap.material) ||
            (!snap.kind_name.empty() && f->kind_name == snap.kind_name);
        if (!kin) continue;
        AgentMind::MemorySnapshot fs;
        fs.tick = snap.tick; fs.pos = f->pos;
        fs.material = f->material; fs.kind_name = f->kind_name;
        for (PropId op : obs)
            if (const Value* v = w.prop_of(*f, op)) fs.props[op] = *v;
        upsert_memory(mind, fid, fs, share, p);
    }
}

// upsert_memory + abstraction fan-out. `fresh` is evaluated BEFORE the
// upsert so only first-time imprints propagate to kin entities.
void upsert_memory_g(const World& w, AgentMind& mind, EntityId eid,
                     const AgentMind::MemorySnapshot& snap,
                     std::int64_t charge, const MindParams& p,
                     std::int64_t radius) {
    const auto it = mind.memory.find(eid);
    const bool fresh = it == mind.memory.end() || it->second.engram.clarity <= 0;
    upsert_memory(mind, eid, snap, charge, p);
    if (fresh) generalize_abstraction(w, mind, eid, snap, charge, p, radius);
}

// sight radius of `e` — the honest perception bound for generalization.
std::int64_t sight_radius_of(const World& w, const Entity& e) {
    const PropertyDef* d = w.props.find_name("sight_range");
    return d ? w.prop_int(e, d->id, 256) : 256;
}

Vec2 recalled_pos(const AgentMind::MemorySnapshot& s, EntityId eid,
                  std::uint64_t world_seed, std::int64_t blur_threshold) {
    const std::int64_t clarity = s.engram.clarity;
    if (blur_threshold <= 0 || clarity >= blur_threshold) return s.pos;
    // Semantic drift: the fuzzier the memory, the wider the deterministic
    // positional jitter — hashed on (entity, seed, clarity) so two runs
    // recall the same wrong place.
    const std::int64_t blur = (blur_threshold - clarity) / 8 + 1;
    const std::uint64_t h = Rng::stream_seed(world_seed, "recall",
        static_cast<std::uint64_t>(eid) * 0x9E3779B97F4A7C15ull ^
        static_cast<std::uint64_t>(clarity));
    const std::int64_t w2 = 2 * blur + 1;
    const std::int64_t dx = static_cast<std::int64_t>(h % static_cast<std::uint64_t>(w2)) - blur;
    const std::int64_t dy = static_cast<std::int64_t>((h >> 32) % static_cast<std::uint64_t>(w2)) - blur;
    return {s.pos.x + static_cast<std::int32_t>(dx),
            s.pos.y + static_cast<std::int32_t>(dy)};
}

Value recalled_prop(const AgentMind::MemorySnapshot& s, PropId p,
                    EntityId eid, std::uint64_t world_seed,
                    std::int64_t blur_threshold, const Value& fallback) {
    const auto it = s.props.find(p);
    if (it == s.props.end()) return fallback;
    const Value& v = it->second;
    if (v.type != ValType::Integer || blur_threshold <= 0 ||
        s.engram.clarity >= blur_threshold)
        return v;
    // Quantize to coarse buckets: "about forty, somewhere over there".
    const std::int64_t bucket = (blur_threshold - s.engram.clarity) / 10 + 1;
    return Value::integer((v.i / bucket) * bucket);
}

// ---- Nhịp 2-3: neuro-emotional dynamics ---------------------------------------

void neuro_touch(World& w, EntityId npc) {
    AgentMind& m = w.mind(npc);
    if (m.neuro.tuned) return;
    m.neuro.tuned = true;
    if (const Entity* e = w.find(npc)) {
        const MindParams p = mind_params(w, *e);
        m.neuro.threat_gain = p.threat_gain;
        m.neuro.reward_gain = p.reward_gain;
        m.neuro.pattern_coupling = p.pattern_coupling;
        m.neuro.empathy_coupling = p.empathy_coupling;
        m.neuro.low_light_bias = p.low_light_bias;
        m.neuro.isolation_bias = p.isolation_bias;
        m.neuro.executive_override = p.executive_override;
    }
}

const NeuroState& neuro_of(const AgentMind& m) { return m.neuro; }

void allostatic_shift(World& w, AgentMind& mind, EntityId npc,
                      std::int64_t shock, bool kin_died) {
    const Entity* e = w.find(npc);
    const MindParams p = e ? mind_params(w, *e) : MindParams{};
    NeuroState& ns = mind.neuro;
    if (!ns.tuned) {           // untuned minds still shift from spec defaults
        ns.tuned = true;
        if (e) {
            ns.threat_gain = p.threat_gain; ns.reward_gain = p.reward_gain;
            ns.pattern_coupling = p.pattern_coupling;
            ns.empathy_coupling = p.empathy_coupling;
            ns.low_light_bias = p.low_light_bias;
            ns.isolation_bias = p.isolation_bias;
            ns.executive_override = p.executive_override;
        }
    }
    // deterministic per-individual spread so identical twins still diverge
    const std::uint64_t h = Rng::stream_seed(w.seed, "allostasis",
        static_cast<std::uint64_t>(npc) ^
        static_cast<std::uint64_t>(static_cast<std::int64_t>(shock)));
    auto cap = [](std::int64_t v) { return std::min<std::int64_t>(3000, v); };
    // the genome decides WHICH axis bends: high baseline -> bigger shift
    if (ns.threat_gain >= 1200)
        ns.threat_gain = cap(ns.threat_gain + 400 + static_cast<std::int64_t>(h % 400));
    if (ns.pattern_coupling >= 1200)
        ns.pattern_coupling = cap(ns.pattern_coupling + 500);
    if (ns.empathy_coupling >= 1200 && kin_died)
        ns.reward_gain = std::max<std::int64_t>(
            0, ns.reward_gain - 700 - static_cast<std::int64_t>((h >> 8) % 300));
    // the shock itself floods panic, scaled by the (possibly new) gain
    const std::int64_t spike = 300 + static_cast<std::int64_t>((h >> 16) % 200);
    ns.panic = std::min<std::int64_t>(1000,
        ns.panic + spike * ns.threat_gain / 1000);
    ns.grief = std::min<std::int64_t>(1000, ns.grief + 150);
}

void maybe_spurious_association(World& w, AgentMind& mind, EntityId npc,
                                const CausalEvent& ev, std::int64_t shock) {
    (void)shock;
    NeuroState& ns = mind.neuro;
    const Entity* me = w.find(npc);
    if (!me || !ns.tuned) return;
    if (ns.pattern_coupling <= 1600 || ns.panic <= 500) return;
    // collect neutral bystanders: alive, near, not a participant
    const PropId sight = w.props.find_name("sight_range")
        ? w.props.find_name("sight_range")->id : kNoProp;
    const std::int64_t sr = sight != kNoProp ? w.prop_int(*me, sight, 200) : 200;
    std::vector<EntityId> near_ids;
    for (EntityId cid : w.entities_near(me->pos, sr)) {
        if (cid == npc) continue;
        if (std::find(ev.participants.begin(), ev.participants.end(), cid) !=
            ev.participants.end()) continue;
        const Entity* ce = w.find(cid);
        if (!ce || !ce->alive) continue;
        near_ids.push_back(cid);
    }
    if (near_ids.empty()) return;
    const std::uint64_t h = Rng::stream_seed(w.seed, "paranoia",
        static_cast<std::uint64_t>(npc) ^ ev.id);
    const EntityId victim = near_ids[h % near_ids.size()];
    // the innocent gets a FALSE aversive memory — paranoia is born here,
    // from coupling x panic, with no disease flag anywhere
    AgentMind::MemorySnapshot snap = snapshot_now(w, victim, ev.tick);
    const std::int64_t charge = -static_cast<std::int64_t>(
        600 + (h >> 20) % 250);                    // -600..-850
    const MindParams p = mind_params(w, *me);
    upsert_memory_g(w, mind, victim, snap, charge, p, sr);
}

void neuro_relief_track(AgentMind& mind, const CausalEvent& ev,
                        std::int64_t emotion_decay) {
    NeuroState& ns = mind.neuro;
    if (!ns.tuned) return;
    const std::int64_t elapsed = ns.last_event_tick
        ? static_cast<std::int64_t>(ev.tick - ns.last_event_tick) : 0;
    const std::int64_t expected_drop = elapsed * emotion_decay;
    const std::int64_t actual_drop = ns.panic_at_event - ns.panic;
    if (ns.panic_at_event > 500 && actual_drop > expected_drop + 50)
        ++mind.panic_relief[ev.action];
    ns.panic_at_event = ns.panic;
    ns.last_event_tick = ev.tick;
}

std::int64_t compulsion_bonus(const AgentMind& mind, const std::string& action) {
    const auto it = mind.panic_relief.find(action);
    if (it == mind.panic_relief.end() || it->second <= 0) return 0;
    const std::int64_t panic = mind.neuro.panic;
    const std::int64_t coupling = mind.neuro.pattern_coupling;
    // obsession: panic x coupling amplifies the pull of proven-relief acts
    std::int64_t bonus = it->second * panic * coupling / 50000;
    return std::min<std::int64_t>(300, bonus);
}

// Bond-driven utility (Nhịp 4.5a/d): actions aimed at a dearly-held
// positive engram score extra — gifting the bonded, striking whoever
// carries a deep negative mark (which is also how a parent ends up
// defending its imprint: the threat wears a negative engram). Proximity
// itself isn't a plannable action, so "move closer" shows up through
// share/defence candidates. Capped so bonds bias, never dominate.
std::int64_t bond_bonus(const AgentMind& mind,
                        const InteractionContext& c) {
    std::int64_t bonus = 0;
    for (EntityId t : c.targets) {
        const auto it = mind.memory.find(t);
        if (it == mind.memory.end()) continue;
        const std::int64_t q = it->second.engram.emotional_charge;
        if (q >= 600 && c.parameters.action == "share")
            bonus += q;                          // gift to the bonded
        else if (q <= -600 &&
                 (c.parameters.action == "strike" ||
                  c.parameters.action == "impact" ||
                  c.parameters.action == "chop" ||
                  c.parameters.action == "dig"))
            bonus += -q;                         // aversion -> aggression
    }
    return std::min<std::int64_t>(bonus, 1500);
}

// Inferred welfare of a bonded target (Nhịp 4.5 care): ONLY agent-side
// knowledge — ToM mood belief, imprint depth (altricial young are needy
// by definition), and observable snapshot signs (empty-handed quantity,
// visible wounds). Private need props of others are never consulted.
std::int64_t bonded_distress(const World& w, const AgentMind& mind,
                             EntityId eid) {
    const auto it = mind.memory.find(eid);
    if (it == mind.memory.end()) return 0;
    const MemoryEngram& g = it->second.engram;
    if (g.emotional_charge < 600) return 0;      // not a charge worth minding
    std::int64_t d = 0;
    if (const Belief* b = mind.find_belief("mood:" + std::to_string(eid)))
        d = std::max(d, b->confidence);           // ToM-inferred agitation
    if (g.stability >= 9000 && g.emotional_charge >= 800)
        d = std::max(d, static_cast<std::int64_t>(600));  // imprint = dependent
    const auto pv = [&](const char* name) -> const Value* {
        const PropertyDef* pd = w.props.find_name(name);
        if (!pd) return nullptr;
        const auto pi = it->second.props.find(pd->id);
        return pi == it->second.props.end() ? nullptr : &pi->second;
    };
    if (const Value* q = pv("quantity"))
        if (q->type == ValType::Integer && q->i <= 0)
            d = std::max(d, static_cast<std::int64_t>(400));   // empty-handed
    if (const Value* ig = pv("integrity"))
        if (ig->type == ValType::Integer && ig->i < 400)
            d = std::max(d, static_cast<std::int64_t>(500));   // visible wounds
    return std::min<std::int64_t>(d, 1000);
}

// Care-drive utility (Nhịp 4.5 depth): actions that serve a DISTRESSED
// bonded charge score on satisfaction scale, opposed by the actor's own
// survival pressure — and empathy_coupling decides who yields. Low
// empathy + hunger>900 -> self-preservation wins (the parent eats the
// last scrap); empathy near max -> the care bonus survives starvation.
// Everything derives from engram charge x gains — no care flag exists.
std::int64_t care_bonus(const World& w, EntityId npc,
                        const AgentMind& mind,
                        const InteractionContext& c) {
    const Entity* me = w.find(npc);
    if (!me) return 0;
    const MindParams mp = mind_params(w, *me);
    std::int64_t bonus = 0;
    for (EntityId t : c.targets) {
        const auto it = mind.memory.find(t);
        if (it == mind.memory.end() ||
            it->second.engram.emotional_charge < 600) continue;
        const std::int64_t distress = bonded_distress(w, mind, t);
        if (distress < 300) continue;          // bonded is fine: affection only
        std::int64_t rel = 0;
        if (c.parameters.action == "share")
            rel = 1000;                        // direct resource transfer
        else if (c.parameters.action == "move" &&
                 c.parameters.has_destination &&
                 dist2(c.parameters.destination, it->second.pos) <= 4096)
            rel = 700;                         // closing in on the bonded
        if (rel == 0) continue;
        // impulse: bond depth x empathy x relevance x distress (0..3000)
        const std::int64_t raw = it->second.engram.emotional_charge
            * mp.empathy_coupling / 1000
            * rel / 1000 * distress / 1000;
        // survival opposition: own worst need vs empathy-bought immunity
        const std::int64_t pressure = std::max(
            need_of(w, *me, "hunger"), need_of(w, *me, "thirst"));
        const std::int64_t opp = std::max<std::int64_t>(0,
            1000 - mp.empathy_coupling * 3 / 4);   // emp0->1000, emp3000->250
        const std::int64_t keep = 1000 - std::min<std::int64_t>(
            1000, pressure * opp / 1000);
        bonus += raw * 1500 * keep / 1000;
    }
    return std::min<std::int64_t>(bonus, 3000000);
}

bool share_collective(const World& w, EntityId a, EntityId b);
void gratitude_engram(World& w, EntityId who, EntityId benefactor,
                      std::uint64_t tick, const MindParams& mp);

// Herd pressure (Nhịp 5a): count DISTINCT familiar/collective
// conspecifics that recently performed this action. >=3 -> conformity
// bonus scaled by conformity_gain x theory_of_mind, discounted by
// abstraction_gain (abstract minds think for themselves).
std::int64_t conformity_bonus(const World& w, EntityId npc,
                              const AgentMind& mind,
                              const std::string& action) {
    const Entity* me = w.find(npc);
    if (!me) return 0;
    const MindParams mp = mind_params(w, *me);
    if (mp.conformity_gain <= 0) return 0;
    std::set<EntityId> herd;
    for (const WitnessedAct& wa : mind.recent_witnessed) {
        if (wa.action != action || wa.actor == kNoEntity) continue;
        if (herd.count(wa.actor)) continue;
        const Entity* ae = w.find(wa.actor);
        if (!ae || !ae->alive || ae->kind_name != me->kind_name) continue;
        // "quen biết" = holds an engram, or shares a collective
        const bool familiar = mind.memory.find(wa.actor) != mind.memory.end();
        if (!familiar && !share_collective(w, npc, wa.actor)) continue;
        herd.insert(wa.actor);
    }
    if (herd.size() < 3) return 0;
    const std::int64_t n = static_cast<std::int64_t>(herd.size());
    std::int64_t bonus = 120 * mp.conformity_gain / 1000
                       * mp.theory_of_mind / 1000
                       * std::max<std::int64_t>(0, 2000 - mp.abstraction_gain)
                       / 1000 * n / 3;
    return std::min<std::int64_t>(bonus, 800);
}

// Sublimated-pain craft drive (Nhịp 5c): accumulated relief pays out
// proportionally to current grief/rage — mourning becomes making.
std::int64_t sublimation_bonus(const AgentMind& mind,
                               const std::string& action) {
    const auto it = mind.sublimation_relief.find(action);
    if (it == mind.sublimation_relief.end() || it->second <= 0) return 0;
    const std::int64_t emo = std::max(mind.neuro.rage, mind.neuro.grief);
    return std::min<std::int64_t>(600, it->second * emo / 3000);
}

// Agent-level shock of a causal event + its aftermath (Nhịp 2-3):
// allostatic shift when |shock| crosses trauma_charge, spurious
// association when paranoia thresholds hold, and relief bookkeeping so
// plan scoring can reward actions that previously calmed the panic.
void apply_event_shock(World& w, AgentMind& mind, EntityId npc,
                       const CausalEvent& ev) {
    const Entity* me = w.find(npc);
    if (!me) return;
    const MindParams p = mind_params(w, *me);
    neuro_touch(w, npc);
    std::int64_t shock = 0;
    bool kin_died = false;
    for (EntityId tid : ev.participants) {
        if (tid == npc || tid == kNoEntity) continue;
        shock = std::min(shock, event_charge(w, npc, ev, tid));
        if (const Entity* t = w.find(tid))
            if (!t->alive && t->kind_name == me->kind_name) kin_died = true;
    }
    if (shock <= -p.trauma_charge) {
        allostatic_shift(w, mind, npc, shock, kin_died);
        maybe_spurious_association(w, mind, npc, ev, shock);
    }
    neuro_relief_track(mind, ev, p.emotion_decay);
    // Ego defense (Nhịp 5c): grief/rage with no clear perpetrator seeks
    // an outlet through the same gain axes — scapegoat a stranger when
    // paranoid+ruthless, or sublimate the pain into craft-drive when
    // abstract. No "guilt"/"art" flag: just charge x gain routing.
    NeuroState& ns = mind.neuro;
    if ((ns.grief >= 700 || ns.rage >= 700) && p.ego_defense_gain > 1200) {
        if (ns.pattern_coupling > 1500 && ns.empathy_coupling < 800) {
            // scapegoat: false blame lands on a nearby stranger/weakling
            const PropId sight = w.props.find_name("sight_range")
                ? w.props.find_name("sight_range")->id : kNoProp;
            const std::int64_t sr = sight != kNoProp
                ? w.prop_int(*me, sight, 200) : 200;
            std::vector<EntityId> cands;
            for (EntityId cid : w.entities_near(me->pos, sr)) {
                if (cid == npc) continue;
                if (std::find(ev.participants.begin(), ev.participants.end(),
                              cid) != ev.participants.end()) continue;
                const Entity* ce = w.find(cid);
                if (!ce || !ce->alive) continue;
                if (mind.memory.find(cid) == mind.memory.end())
                    cands.push_back(cid);          // prefer strangers
            }
            if (cands.empty())
                for (EntityId cid : w.entities_near(me->pos, sr)) {
                    if (cid == npc) continue;
                    if (std::find(ev.participants.begin(),
                                  ev.participants.end(), cid) !=
                        ev.participants.end()) continue;
                    const Entity* ce = w.find(cid);
                    if (ce && ce->alive) cands.push_back(cid);
                }
            if (!cands.empty()) {
                const std::uint64_t h = Rng::stream_seed(w.seed,
                    "scapegoat", static_cast<std::uint64_t>(npc) ^ ev.id);
                const EntityId scapegoat = cands[h % cands.size()];
                AgentMind::MemorySnapshot snap =
                    snapshot_now(w, scapegoat, ev.tick);
                upsert_memory(mind, scapegoat, snap, -750, p);
                auto git = mind.memory.find(scapegoat);
                if (git != mind.memory.end())
                    git->second.engram.stability = std::min(p.max_stability,
                        std::max(git->second.engram.stability,
                                 static_cast<std::int64_t>(4000)));
            }
        }
        if (p.abstraction_gain > 1500) {
            // sublimation: a slice of the pain becomes craft-drive —
            // expressive acts carry relief bonus in plan scoring, and the
            // transmuted fraction no longer crushes reward_gain.
            for (const char* act :
                 {"carve", "assemble", "process", "dig", "chop", "strike"}) {
                std::int64_t& slot = mind.sublimation_relief[act];
                slot = std::min<std::int64_t>(2000,
                    slot + std::min<std::int64_t>(500,
                        (ns.grief + ns.rage) / 8));
            }
            ns.grief = ns.grief * 2 / 3;
            ns.rage  = ns.rage  * 2 / 3;
        }
    }
}

std::size_t perceive(World& w, EntityId npc) {
    const Entity* me = w.find(npc);
    if (!me || !me->alive) return 0;
    AgentMind& mind = w.mind(npc);
    const PropId sight = w.props.find_name("sight_range")
        ? w.props.find_name("sight_range")->id : kNoProp;
    const std::int64_t range = sight != kNoProp ? w.prop_int(*me, sight, 0) : 0;
    if (range <= 0) return 0;                     // no sight -> no observations
    const std::int64_t r2 = range * range;

    // observable-domain props: the only props allowed into a mind (the
    // registry is frozen after load_content, so build the list once)
    const std::vector<PropId>& obs_props = w.observable_props();

    const PropertyDef* exd = w.props.find_name("exposed");
    const PropId exposed_p = exd ? exd->id : kNoProp;
    const MindParams mparams = mind_params(w, *me);
    neuro_touch(w, npc);

    std::size_t added = 0;
    for (const EntityId eid : w.entities_near(me->pos, range)) {
        if (eid == npc) continue;
        const Entity* e = w.find(eid);
        if (!e || !e->alive) continue;
        // World Truth: a buried occurrence is invisible (TODO §9.3) except for
        // its declared surface Observable Evidence (§9.10.2). Exposed deposits
        // are fully visible; a buried one with no evidence is undetectable.
        bool buried = false;
        if (e->deposit != kNoDeposit && exposed_p != kNoProp &&
            w.prop_int(*e, exposed_p, 0) == 0) {
            buried = true;
            const DepositDef* dd = w.deposits.find(e->deposit);
            if (!dd || dd->evidence.entries.empty()) continue;
        }
        const std::int64_t dx = static_cast<std::int64_t>(e->pos.x) - me->pos.x;
        const std::int64_t dy = static_cast<std::int64_t>(e->pos.y) - me->pos.y;
        if (dx * dx + dy * dy > r2) continue;
        Observation o;
        o.entity = eid; o.tick = w.clock_.tick(); o.pos = e->pos;
        for (PropId p : obs_props) {
            if (buried && p == exposed_p) continue;          // exposure state is not a sign
            if (buried) {                                    // only declared evidence props
                const DepositDef* dd = w.deposits.find(e->deposit);
                if (!dd->evidence.get(p)) continue;
            }
            if (const Value* v = w.prop_of(*e, p)) o.props[p] = *v;
        }
        // fog-of-war memory: Ebbinghaus-encoded snapshot of what was just
        // perceived — stale belief the planner may use after the entity
        // leaves sight range, never written back to World Truth. Charge
        // comes from the NPC's own needs vs what it sees (salience).
        AgentMind::MemorySnapshot snap;
        snap.tick = o.tick; snap.pos = o.pos; snap.props = o.props;
        snap.material = e->material; snap.kind_name = e->kind_name;
        // previous VISUAL sighting — theory-of-mind reads motion between
        // observations. (Witness events may refresh memory.pos, but only
        // perceive() feeds the observation stream, so this is the honest
        // "last time I actually saw it" record.)
        const Observation* prev_obs = nullptr;
        for (auto it = mind.observations.rbegin();
             it != mind.observations.rend(); ++it)
            if (it->entity == eid) { prev_obs = &*it; break; }
        const bool had_engram =
            mind.memory.find(eid) != mind.memory.end();
        upsert_memory_g(w, mind, eid, snap, perceive_charge(w, *me, *e),
                        mparams, range);
        // Out-group wariness (Nhịp 5b): first sight of a conspecific
        // stranger — no engram yet, no shared collective — writes a
        // wary mark scaled by xenophobia_bias; collective mates get
        // their negative charge discounted instead.
        if (e->kind_name == me->kind_name && eid != npc &&
            mparams.xenophobia_bias > 0) {
            const bool ingroup = share_collective(w, npc, eid);
            auto git = mind.memory.find(eid);
            if (git != mind.memory.end()) {
                MemoryEngram& g = git->second.engram;
                if (!had_engram && !ingroup && g.emotional_charge < 100) {
                    const std::int64_t wary =
                        mparams.xenophobia_bias * 30 / 100;   // -300..-750
                    g.emotional_charge = std::min(g.emotional_charge, -wary);
                } else if (ingroup && g.emotional_charge < 0) {
                    const std::int64_t f =
                        1000 - mparams.xenophobia_bias * 300 / 2500;
                    g.emotional_charge = g.emotional_charge * f / 1000;
                }
            }
        }
        // Theory of mind (Nhịp 4d): a sophisticated observer infers a
        // conspecific's agitation from OBSERVED MOTION ONLY — displacement
        // between sightings vs the observer's own top speed. Private props
        // (hunger/panic) are never read; the read lands as a Belief, and
        // theory_of_mind scales its confidence. No World Truth is touched.
        if (mparams.theory_of_mind > 1000 && w.find_mind(eid) != nullptr &&
            prev_obs && o.tick > prev_obs->tick) {
            const std::int64_t d2 = dist2(prev_obs->pos, e->pos);
            if (d2 > 0) {
                const std::int64_t d = mem_isqrt(d2);
                const std::int64_t dt =
                    static_cast<std::int64_t>(o.tick - prev_obs->tick);
                // observed pace in milli-units/tick vs own cruise speed
                const std::int64_t rate = d * 1000 / dt;
                const PropertyDef* spd = w.props.find_name("speed");
                const std::int64_t my_speed =
                    spd ? w.prop_int(*me, spd->id, 1000) : 1000;
                const std::int64_t agit = std::clamp<std::int64_t>(
                    rate * 1000 / std::max<std::int64_t>(1, my_speed),
                    0, 1000);
                if (agit > 0) {
                    const std::int64_t conf = std::min<std::int64_t>(
                        1000, agit * mparams.theory_of_mind / 1000);
                    Belief& b = mind.belief("mood:" + std::to_string(eid));
                    b.status = BeliefStatus::Observed;
                    b.confidence = std::max(b.confidence, conf);
                    b.evidence_for += conf;
                    b.last_tick = o.tick;
                }
            }
        }
        // Flashback (Nhịp 3a): if this thing's material/kind matches a deep
        // NEGATIVE engram — stored snapshot or born mark — the trauma
        // re-vividifies and panic floods, scaled by threat_gain.
        {
            NeuroState& ns = mind.neuro;
            const std::int64_t fb_charge = -mparams.flashback_charge;
            auto trauma_match = [&](MaterialId mat, const std::string& kind,
                                    const MemoryEngram& g) {
                if (g.emotional_charge > fb_charge || g.emotional_charge >= 0 ||
                    g.stability < mparams.flashback_stability) return false;
                return (mat != kNoMaterial && mat == e->material) ||
                       (!kind.empty() && kind == e->kind_name);
            };
            bool flashed = false;
            std::int64_t fb_mag = 0;
            for (auto& [kid, ms] : mind.memory) {
                if (!trauma_match(ms.material, ms.kind_name, ms.engram)) continue;
                ms.engram.clarity = mparams.max_clarity;
                fb_mag = -ms.engram.emotional_charge;
                flashed = true;
                break;
            }
            if (!flashed) {
                for (BornEngram& bm : mind.born_memories) {
                    if (!trauma_match(bm.material, bm.kind_name, bm.engram))
                        continue;
                    bm.engram.clarity = mparams.max_clarity;
                    fb_mag = -bm.engram.emotional_charge;
                    // encountering the feared thing again consolidates the
                    // born mark toward instinct
                    bm.engram.stability = std::min(mparams.max_stability,
                        bm.engram.stability + 400);
                    flashed = true;
                    break;
                }
            }
            if (flashed)
                ns.panic = std::min<std::int64_t>(1000, ns.panic +
                    fb_mag * ns.threat_gain / 1000);
        }
        mind.observe(std::move(o));
        ++added;
    }
    // visited_cells: the agent's own frontier record (per-mind knowledge —
    // never a World flag). Drives ExploreGoal targets.
    if (!w.terrain.empty() && w.terrain.cell > 0) {
        const int cgx = TerrainGrid::cell_of(me->pos.x, w.terrain.cell);
        const int cgy = TerrainGrid::cell_of(me->pos.y, w.terrain.cell);
        mind.visited_cells.emplace(cgx, cgy);
        // also mark the 8 neighbours within sight as "seen" ground? No —
        // visited means stood-in; frontier derives from the visited set.
    }
    // No size cap: engrams decay by clarity and self-release at 0 —
    // forgetting IS the memory bound (Ebbinghaus, Nhịp 1).
    return added;
}

// ---- learning from causal events -------------------------------------------------

namespace {

// outcome-belief key shared by self-learning, witness learning and plan
// scoring: "outcome:<action>:<propname>:<dir>" plus ":mat<id>" when the entity
// has a material — the material suffix is what enables analogical transfer
// (§10.3). outcome_base() is the material-less prefix used for belief lookup.
std::string outcome_base(const World& w, const std::string& action,
                         const StateChange& c) {
    const char* dir = "same";
    if (c.before.type == ValType::Integer && c.after.type == ValType::Integer) {
        if (c.after.i > c.before.i) dir = "up";
        else if (c.after.i < c.before.i) dir = "down";
    } else if (c.before.type == ValType::Boolean && c.after.type == ValType::Boolean) {
        if (!c.before.b && c.after.b) dir = "up";
        else if (c.before.b && !c.after.b) dir = "down";
    }
    const PropertyDef* pd = w.props.find(c.prop);
    return "outcome:" + action + ":" + (pd ? pd->name : "?") + ":" + dir;
}

std::string outcome_key(const World& w, const std::string& action,
                        const StateChange& c) {
    std::string key = outcome_base(w, action, c);
    const Entity* e = w.find(c.entity);
    if (e && e->material != kNoMaterial)
        key += ":mat" + std::to_string(e->material);
    return key;
}

// What did the event produce? yield:<action>:<material-or-kind name> — the
// NPC learns "dig yields copper_ore" from seeing the spawn, not from truth.
void learn_yields(World& w, AgentMind& mind, const CausalEvent& ev,
                  std::int64_t delta, bool self) {
    for (EntityId sid : ev.spawned) {
        const Entity* s = w.find(sid);
        if (!s) continue;
        std::string what;
        if (s->material != kNoMaterial) {
            if (const MaterialDef* md = w.materials.find(s->material))
                what = md->name;
        }
        if (what.empty()) what = s->kind_name;
        if (what.empty()) continue;
        mind.reinforce("yield:" + ev.action + ":" + what, delta, ev.tick, self);
    }
}

void learn_outcomes(World& w, AgentMind& mind, const CausalEvent& ev,
                    std::int64_t delta, bool self, bool mark_observed) {
    for (const StateChange& c : ev.changes) {
        const PropertyDef* pd = w.props.find(c.prop);
        if (!pd) continue;
        const std::string key = outcome_key(w, ev.action, c);
        mind.reinforce(key, delta, ev.tick, self);
        if (mark_observed) mind.belief(key).status = BeliefStatus::Observed;
    }
}

}  // namespace

// Poison attribution: when a consume event damages its actor, EVERY agent
// who learns of it — the eater (self) and eyewitnesses — records a Refuted
// method against the material signature(s) involved. The dead cannot
// speak, so the causal ledger speaks for them; communicate() then spreads
// the taboo through teach_record like any other hard-won boundary.
void learn_harm_taboo(World& w, EntityId learner, const CausalEvent& ev) {
    if (ev.action != "consume" || ev.participants.size() < 2) return;
    const EntityId eater = ev.participants.front();
    const PropId integ = w.props.find_name("integrity")
        ? w.props.find_name("integrity")->id : kNoProp;
    if (integ == kNoProp) return;
    bool harmed = false;
    for (const StateChange& c : ev.changes)
        if (c.entity == eater && c.prop == integ &&
            c.before.type == ValType::Integer && c.after.type == ValType::Integer &&
            c.after.i < c.before.i) { harmed = true; break; }
    if (!harmed) return;
    const PropId fmp = w.props.find_name("fruit_material")
        ? w.props.find_name("fruit_material")->id : kNoProp;
    for (std::size_t k = 1; k < ev.participants.size(); ++k) {
        const EntityId tid = ev.participants[k];
        // food identity: a fruiting plant harms THROUGH its fruit material
        // (nightshade on living_wood), everything else through its own.
        MaterialId m = kNoMaterial;
        bool fruiting = false;
        auto fruit_of = [&](const Value* fv) -> MaterialId {
            if (!fv || fv->type != ValType::Text || fv->s.empty())
                return kNoMaterial;
            return w.materials.id_of(fv->s);
        };
        if (const Entity* t = w.find(tid)) {
            if (fmp != kNoProp) {
                if (const Value* fv = w.prop_of(*t, fmp)) {
                    const MaterialId fm = fruit_of(fv);
                    if (fm != kNoMaterial) { m = fm; fruiting = true; }
                }
            }
            if (m == kNoMaterial) m = t->material;
        }
        if (m == kNoMaterial) {           // entity gone: last perceived snapshot
            if (const AgentMind* lm = w.find_mind(learner)) {
                const auto it = lm->memory.find(tid);
                if (it != lm->memory.end()) {
                    if (fmp != kNoProp) {
                        const auto fp = it->second.props.find(fmp);
                        if (fp != it->second.props.end()) {
                            const MaterialId fm = fruit_of(&fp->second);
                            if (fm != kNoMaterial) { m = fm; fruiting = true; }
                        }
                    }
                    if (m == kNoMaterial) m = it->second.material;
                }
            }
        }
        if (m == kNoMaterial) continue;
        std::string mat_name = "?";
        if (const MaterialDef* md = w.materials.find(m)) mat_name = md->name;
        note_harmful_method(w, learner, "consume", m, fruiting,
                            "consume " + mat_name + " (harm witnessed)");
        // Trauma engram: the harmful substance itself is imprinted with a
        // strongly negative charge — one poisoning forges a lasting aversion.
        if (const Entity* le = w.find(learner)) {
            AgentMind* lmp = w.find_mind(learner) ? &w.mind(learner) : nullptr;
            if (lmp) {
                AgentMind::MemorySnapshot snap;
                auto mit = lmp->memory.find(tid);
                if (w.find(tid)) snap = snapshot_now(w, tid, ev.tick);
                else if (mit != lmp->memory.end()) snap = mit->second;
                else continue;
                const MindParams lmp_ = mind_params(w, *le);
                upsert_memory_g(w, *lmp, tid, snap, -900, lmp_,
                                sight_radius_of(w, *le));
            }
        }
    }
}

// Bereavement (Nhịp 4.5b): a deeply-held bond (charge >= +700) dying in
// an event flips the trace into a trauma shock, floods grief, and marks
// the killer (event actor, when it isn't us or the deceased) with a deep
// negative engram -> rage-driven retaliation later, via Score() aversion.
// No "widow" flag: just valence inversion + emotion pumps.
bool bereavement_check(World& w, AgentMind& mind, EntityId npc,
                       const CausalEvent& ev, EntityId tid,
                       const Entity* me, const MindParams& mp) {
    const Entity* te = w.find(tid);
    if (te && te->alive) return false;                    // not a death
    auto mit = mind.memory.find(tid);
    if (mit == mind.memory.end()) return false;
    MemoryEngram& g = mit->second.engram;
    if (g.emotional_charge < 700) return false;          // not dearly held
    g.emotional_charge = mem_clamp(
        -900 * mp.empathy_coupling / 1000, -1000, 1000);  // valence flips
    g.clarity = mp.max_clarity;
    g.stability = std::min(mp.max_stability,
                           g.stability + mp.imprint_base_stability);
    neuro_touch(w, npc);
    mind.neuro.grief = std::min<std::int64_t>(1000,
        mind.neuro.grief + 400 + mp.empathy_coupling / 4);
    // participants[0] is the event actor (World::interact convention)
    const EntityId killer = ev.participants.empty()
        ? kNoEntity : ev.participants.front();
    if (killer != kNoEntity && killer != npc && killer != tid) {
        mind.neuro.rage = std::min<std::int64_t>(1000,
            mind.neuro.rage + 300 + mp.jealousy_gain / 4);
        AgentMind::MemorySnapshot ks = snapshot_now(w, killer, ev.tick);
        upsert_memory(mind, killer, ks, -800, mp);
    }
    (void)me;
    return true;
}

void learn_from_event(World& w, EntityId npc, std::uint64_t event_id) {
    const CausalEvent* ev = w.ledger.find(event_id);
    if (!ev) return;
    AgentMind& mind = w.mind(npc);
    mind.learn_action(ev->action);
    learn_outcomes(w, mind, *ev, 34, /*self=*/true, /*mark_observed=*/true);
    learn_yields(w, mind, *ev, 34, /*self=*/true);
    learn_harm_taboo(w, npc, *ev);
    // Ebbinghaus encoding: the actor's memory of every other participant
    // is charged by what the event meant to them (death, harm, crisis meal).
    if (const Entity* me = w.find(npc)) {
        const MindParams mp = mind_params(w, *me);
        for (EntityId tid : ev->participants) {
            if (tid == npc || tid == kNoEntity) continue;
            AgentMind::MemorySnapshot snap;
            auto mit = mind.memory.find(tid);
            if (w.find(tid)) snap = snapshot_now(w, tid, ev->tick);
            else if (mit != mind.memory.end()) snap = mit->second;
            else continue;
            if (bereavement_check(w, mind, npc, *ev, tid, me, mp)) continue;
            upsert_memory_g(w, mind, tid, snap,
                            event_charge(w, npc, *ev, tid), mp,
                            me ? sight_radius_of(w, *me) : 0);
        }
        // Sleeping consolidates: emotional traces strengthen, noise prunes.
        if (ev->action == "rest") sleep_consolidate(mind, mp);
        apply_event_shock(w, mind, npc, *ev);
    }
}

void witness_from_event(World& w, EntityId npc, const CausalEvent& ev) {
    AgentMind& mind = w.mind(npc);
    mind.learn_action(ev.action);                      // saw the verb used
    // Herd memory (Nhịp 5a): what others are doing right now — the raw
    // signal conformity later amplifies. Bounded ring, deterministic.
    if (!ev.participants.empty() && ev.participants.front() != npc) {
        mind.recent_witnessed.push_back(
            {ev.tick, ev.participants.front(), ev.action});
        while (mind.recent_witnessed.size() > 16)
            mind.recent_witnessed.pop_front();
    }
    learn_outcomes(w, mind, ev, 8, /*self=*/false, /*mark_observed=*/false);
    learn_yields(w, mind, ev, 8, /*self=*/false);
    learn_harm_taboo(w, npc, ev);
    // The witness also forms (or refreshes) engrams of who was involved.
    if (const Entity* me = w.find(npc)) {
        const MindParams mp = mind_params(w, *me);
        for (EntityId tid : ev.participants) {
            if (tid == npc || tid == kNoEntity) continue;
            AgentMind::MemorySnapshot snap;
            auto mit = mind.memory.find(tid);
            if (w.find(tid)) snap = snapshot_now(w, tid, ev.tick);
            else if (mit != mind.memory.end()) snap = mit->second;
            else continue;
            if (bereavement_check(w, mind, npc, ev, tid, me, mp)) continue;
            upsert_memory_g(w, mind, tid, snap,
                            event_charge(w, npc, ev, tid), mp,
                            me ? sight_radius_of(w, *me) : 0);
        }
        // Mate guarding (Nhịp 4.5c): witnessing a deeply-bonded partner
        // reproduce or share intimately with a third party floods rage
        // (charge x jealousy_gain) and marks the rival negative while
        // cooling the bond — guarding emerges, no flag required.
        if (ev.action == "reproduce" || ev.action == "share") {
            EntityId bonded = kNoEntity, rival = kNoEntity;
            for (EntityId p : ev.participants) {
                if (p == npc || p == kNoEntity) continue;
                const auto bit = mind.memory.find(p);
                if (bit != mind.memory.end() &&
                    bit->second.engram.emotional_charge >= 650) {
                    bonded = p; break;
                }
            }
            if (bonded != kNoEntity) {
                for (EntityId p : ev.participants)
                    if (p != npc && p != bonded && p != kNoEntity) {
                        rival = p; break;
                    }
                if (rival != kNoEntity) {
                    auto bit = mind.memory.find(bonded);
                    const std::int64_t bq =
                        bit->second.engram.emotional_charge;
                    neuro_touch(w, npc);
                    mind.neuro.rage = std::min<std::int64_t>(1000,
                        mind.neuro.rage + bq * mp.jealousy_gain / 1000);
                    AgentMind::MemorySnapshot rs =
                        snapshot_now(w, rival, ev.tick);
                    upsert_memory(mind, rival, rs, -700, mp);
                    bit->second.engram.emotional_charge = bq * 3 / 4;
                }
            }
        }
        // Reciprocity (Nhịp 5b): witnessing a third party strike down OUR
        // enemy while we are under threat writes a gratitude mark —
        // deep positive engram on the defender, no ledger of favors.
        if ((ev.action == "strike" || ev.action == "impact") &&
            !ev.participants.empty() && ev.participants.size() >= 2 &&
            mind.neuro.tuned && mind.neuro.panic >= 400) {
            const EntityId defender = ev.participants.front();
            const EntityId foe = ev.participants[1];
            if (defender != npc && foe != npc && defender != foe) {
                const auto fit = mind.memory.find(foe);
                if (fit != mind.memory.end() &&
                    fit->second.engram.emotional_charge <= -600) {
                    gratitude_engram(w, npc, defender, ev.tick, mp);
                }
            }
        }
        apply_event_shock(w, mind, npc, ev);
    }
}

// ---- needs & self-directed goals (TODO §10.1) ------------------------------------

namespace {
constexpr std::int64_t kNeedUrgency = 500;    // permille — goal emitted at/above
constexpr std::int64_t kNeedSatisfied = 100;  // permille — goal met at/below
}

std::vector<Goal> derive_goals(const World& w, EntityId npc) {
    std::vector<Goal> out;
    const Entity* e = w.find(npc);
    if (!e || !e->alive) return out;
    static const char* kNeeds[] = {"hunger", "thirst", "fatigue"};
    std::vector<std::pair<std::int64_t, Goal>> urgent;
    for (const char* name : kNeeds) {
        const PropertyDef* d = w.props.find_name(name);
        if (!d) continue;
        const Value* v = w.prop_of(*e, d->id);
        if (!v || v->type != ValType::Integer || v->i < kNeedUrgency) continue;
        Goal g;
        g.kind = Goal::Kind::SatisfyNeed;
        g.need = d->id;
        g.value = kNeedSatisfied;
        g.target = npc;
        urgent.push_back({v->i, g});
    }
    std::sort(urgent.begin(), urgent.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    for (auto& [_, g] : urgent) out.push_back(g);
    return out;
}

// ---- depth-1 planning -------------------------------------------------------------

namespace {

InteractionContext mk(EntityId actor, std::vector<EntityId> targets,
                      const std::string& action) {
    InteractionContext c;
    c.actor = actor; c.targets = std::move(targets);
    c.parameters.action = action; c.parameters.force = 100000; c.parameters.ticks = 1;
    return c;
}

}  // namespace

std::vector<InteractionContext> plan(const World& w, EntityId npc, const Goal& goal,
                                     std::size_t max_results) {
    std::vector<InteractionContext> out;
    const AgentMind* mind = w.find_mind(npc);
    const Entity* me = w.find(npc);
    if (!mind || !me || !me->alive || mind->known_actions.empty())
        return out;   // dead/unknown actor or knows no verbs

    // candidate verbs: learned actions that laws actually support
    static const std::vector<std::string> kVerbs = {
        "dig", "chop", "strike", "impact", "carve", "assemble",
        "disassemble", "process", "heat", "wield", "unwield",
        "consume", "rest"};

    struct Scored { InteractionContext ctx; std::int64_t score; };
    std::vector<Scored> cand;

    const bool need_goal = goal.kind == Goal::Kind::SatisfyNeed;
    const PropId hunger = w.props.find_name("hunger")
        ? w.props.find_name("hunger")->id : kNoProp;
    const PropId thirst = w.props.find_name("thirst")
        ? w.props.find_name("thirst")->id : kNoProp;
    const PropId nutrition = w.props.find_name("nutrition")
        ? w.props.find_name("nutrition")->id : kNoProp;
    const PropId hydration = w.props.find_name("hydration")
        ? w.props.find_name("hydration")->id : kNoProp;

    for (const Observation& ob : mind->observations) {
        if (!need_goal && goal.target != kNoEntity && ob.entity != goal.target) continue;
        // Fog-of-war: NO World-Truth existence/alive check here. Whether the
        // remembered entity still exists is discovered only by a real
        // interact()/perceive() — law predict() on a dead/absent entity
        // simply yields no candidate, which is the correct boundary.
        for (const std::string& verb : kVerbs) {
            if (!mind->known_actions.count(verb)) continue;
            InteractionContext c = mk(npc, {ob.entity}, verb);
            // evaluate with the forward model: first supporting law predicts
            PredictionResult best;
            bool possible = false;
            for (const auto& m : w.laws) {
                if (!m->supports(w, c)) continue;
                PredictionResult p = m->predict(w, c);
                if (p.possible) { best = std::move(p); possible = true; break; }
            }
            if (!possible) continue;
            std::int64_t score = 0;
            if (need_goal) {
                // does the prediction reduce the need on the ACTOR, or spawn
                // something edible/drinkable (material props, World Truth)?
                for (const StateChange& ch : best.changes)
                    if (ch.entity == npc && ch.prop == goal.need) {
                        std::int64_t after = ch.after.type == ValType::Integer
                            ? ch.after.i : 0;
                        std::int64_t before = ch.before.type == ValType::Integer
                            ? ch.before.i : 0;
                        if (after < before) score += 600;
                    }
                const bool wants_hunger = hunger != kNoProp && w.prop_of(*w.find(npc), hunger);
                const bool wants_thirst = thirst != kNoProp && w.prop_of(*w.find(npc), thirst);
                for (const SpawnSpec& s : best.spawns) {
                    if (s.material == kNoMaterial) continue;
                    const MaterialDef* md = w.materials.find(s.material);
                    if (!md) continue;
                    auto mat_prop = [&](PropId p) -> std::int64_t {
                        if (p == kNoProp) return 0;
                        auto it = md->props.find(p);
                        return it == md->props.end() ? 0 : it->second.i;
                    };
                    std::int64_t food = 0;
                    if (wants_hunger) food += mat_prop(nutrition);
                    if (wants_thirst) food += mat_prop(hydration);
                    if (food > 0) score += std::min<std::int64_t>(food / 2, 300);
                }
            } else {
                for (const StateChange& ch : best.changes)
                    if (ch.entity == ob.entity && ch.prop == goal.prop) {
                        std::int64_t after = 0;
                        if (ch.after.type == ValType::Integer) after = ch.after.i;
                        else if (ch.after.type == ValType::Boolean) after = ch.after.b ? 1 : 0;
                        score += (after == goal.value) ? 1000 : 100;
                    }
            }
            if (score > 0) cand.push_back({c, score});
        }
    }
    // actor-only candidates (rest): no target needed
    if (need_goal)
        for (const std::string& verb : kVerbs) {
            if (!mind->known_actions.count(verb)) continue;
            InteractionContext c = mk(npc, {}, verb);
            for (const auto& m : w.laws) {
                if (!m->supports(w, c)) continue;
                PredictionResult p = m->predict(w, c);
                if (!p.possible) continue;
                std::int64_t score = 0;
                for (const StateChange& ch : p.changes)
                    if (ch.entity == npc && ch.prop == goal.need &&
                        ch.after.type == ValType::Integer &&
                        ch.before.type == ValType::Integer &&
                        ch.after.i < ch.before.i) score += 600;
                if (score > 0) { cand.push_back({c, score}); break; }
            }
        }
    std::sort(cand.begin(), cand.end(),
              [](const Scored& a, const Scored& b) { return a.score > b.score; });
    for (std::size_t i = 0; i < cand.size() && i < max_results; ++i)
        out.push_back(cand[i].ctx);
    return out;
}

// ---- multi-step speculative planning (TODO §10.4/§10.6) ----------------------------

namespace {

struct SearchNode {
    std::unique_ptr<World> w;
    std::vector<InteractionContext> path;
    std::set<EntityId> known;          // entities the NPC may target (obs + own spawns)
    std::int64_t score = 0;
};

struct EvalResult { bool satisfied = false; std::int64_t score = 0; };

std::int64_t need_value(const World& w, EntityId npc, PropId p) {
    const Entity* e = w.find(npc);
    if (!e || !e->alive || p == kNoProp) return 0;
    const Value* v = w.prop_of(*e, p);
    if (!v) return 0;                  // absent need = already satisfied
    return v->i;
}

// ---- analogical reasoning (TODO §10.3) -----------------------------------------
// Material similarity is PROPERTY-driven: a quantized signature of
// hardness/brittleness/mass. Experience on one material transfers to its
// analogs — never by name, always by measured similarity.
struct MatSig { std::int64_t h, b, m; };
bool operator==(const MatSig& a, const MatSig& b) {
    return a.h == b.h && a.b == b.b && a.m == b.m;
}
MatSig mat_sig(const World& w, MaterialId mat) {
    const MaterialDef* d = w.materials.find(mat);
    auto get = [&](const char* n) -> std::int64_t {
        if (!d) return 0;
        const PropertyDef* pd = w.props.find_name(n);
        if (!pd) return 0;
        auto it = d->props.find(pd->id);
        return it == d->props.end() ? 0 : it->second.i;
    };
    return {get("hardness") / 100, get("brittleness") / 100, get("mass") / 500};
}
bool mat_analog(const World& w, MaterialId a, MaterialId b) {
    if (a == b) return true;
    if (a == kNoMaterial || b == kNoMaterial) return false;
    return mat_sig(w, a) == mat_sig(w, b);
}

// Best belief weight among keys "base" and "base:mat<N>" where N is the
// target's material or an analog of it. Returns the strongest status tier.
std::int64_t belief_tier(const BeliefStatus s) {
    switch (s) {
        case BeliefStatus::Verified:   return 120;
        case BeliefStatus::Supported:  return 80;
        case BeliefStatus::Tested:     return 30;
        case BeliefStatus::Disputed:   return -150;
        case BeliefStatus::Observed:
        case BeliefStatus::Hypothesized: return 5;
        default: return 0;
    }
}

std::int64_t best_base_belief(const AgentMind& mind, const std::string& base,
                              MaterialId target_mat, const World& w) {
    std::int64_t best = 0;
    const std::string matp = base + ":mat";
    for (const auto& [key, b] : mind.beliefs) {
        if (key == base) {                       // material-less variant
            best = std::max(best, belief_tier(b.status));
            continue;
        }
        if (key.compare(0, matp.size(), matp) != 0) continue;
        // parse the material id suffix
        std::int64_t mid = 0; bool ok = false;
        for (std::size_t i = matp.size(); i < key.size(); ++i) {
            const char ch = key[i];
            if (ch < '0' || ch > '9') { ok = false; break; }
            mid = mid * 10 + (ch - '0'); ok = true;
        }
        if (!ok || mid <= 0) continue;
        if (mat_analog(w, static_cast<MaterialId>(mid), target_mat))
            best = std::max(best, belief_tier(b.status));
    }
    return best;
}

// Belief-guided candidate scoring: what the NPC's EXPERIENCE says about
// this action's outcomes. Verified/Supported evidence rewards, Disputed
// evidence penalizes — analog materials count via mat_analog (§10.3).
std::int64_t belief_bonus(const World& w, EntityId npc,
                          const InteractionContext& ctx,
                          const InteractionResult& res) {
    const AgentMind* mind = w.find_mind(npc);
    if (!mind) return 0;
    std::int64_t bonus = 0;
    for (const StateChange& c : res.changes) {
        if (c.entity == npc) continue;           // own-body changes aren't outcomes
        const Entity* e = w.find(c.entity);
        const MaterialId tmat = e ? e->material : kNoMaterial;
        const std::string base = outcome_base(w, ctx.parameters.action, c);
        bonus += best_base_belief(*mind, base, tmat, w);
    }
    // yield beliefs: spawned materials the NPC has learned about
    for (EntityId sid : res.spawned) {
        const Entity* s = w.find(sid);
        if (!s) continue;
        MaterialId smat = s->material;
        std::vector<std::string> names;
        auto add_name = [&](MaterialId m) {
            if (m == kNoMaterial) return;
            if (const MaterialDef* md = w.materials.find(m)) names.push_back(md->name);
        };
        add_name(smat);
        // analog materials yield the same stuff in the NPC's experience
        for (const MaterialDef& md : w.materials.defs())
            if (mat_analog(w, md.id, smat)) add_name(md.id);
        for (const std::string& nm : names) {
            const std::string key = "yield:" + ctx.parameters.action + ":" + nm;
            if (const Belief* b = mind->find_belief(key))
                bonus += belief_tier(b->status);
        }
    }
    return std::max<std::int64_t>(-400, std::min<std::int64_t>(400, bonus));
}

EvalResult evaluate(const World& w, EntityId npc, const Goal& goal,
                    std::int64_t root_need, const std::set<EntityId>& known,
                    std::int64_t steps) {
    EvalResult r;
    const Entity* actor = w.find(npc);
    if (!actor || !actor->alive) { r.score = -1000000; return r; }

    if (goal.kind == Goal::Kind::SatisfyNeed) {
        const std::int64_t cur = need_value(w, npc, goal.need);
        if (cur <= goal.value) {
            r.satisfied = true; r.score = 1000000 - steps * 50; return r;
        }
        // heuristic: need reduction so far + best edible reachable in `known`
        const PropId reach = w.props.find_name("reach")
            ? w.props.find_name("reach")->id : kNoProp;
        const PropId hunger = w.props.find_name("hunger")
            ? w.props.find_name("hunger")->id : kNoProp;
        const PropId thirst = w.props.find_name("thirst")
            ? w.props.find_name("thirst")->id : kNoProp;
        const PropId nutrition = w.props.find_name("nutrition")
            ? w.props.find_name("nutrition")->id : kNoProp;
        const PropId hydration = w.props.find_name("hydration")
            ? w.props.find_name("hydration")->id : kNoProp;
        const bool wants_food =
            (hunger != kNoProp && w.prop_of(*actor, hunger)) ||
            (thirst != kNoProp && w.prop_of(*actor, thirst));
        std::int64_t potential = 0;
        if (wants_food && nutrition != kNoProp) {
            const std::int64_t rr = reach != kNoProp
                ? w.prop_int(*actor, reach, 64) : 64;
            const std::int64_t r2 = rr * rr;
            for (EntityId id : known) {
                const Entity* e = w.find(id);
                if (!e || !e->alive) continue;
                if (dist2(actor->pos, e->pos) > r2) continue;
                std::int64_t v = 0;
                if (nutrition != kNoProp) v += w.prop_int(*e, nutrition, 0);
                if (hydration != kNoProp) v += w.prop_int(*e, hydration, 0);
                potential = std::max(potential, v);
            }
        }
        r.score = std::max<std::int64_t>(0, root_need - cur) * 1000 +
                  std::min<std::int64_t>(potential, 2000) * 30 - steps * 10;
        return r;
    }

    // PropValue goal: best distance-to-goal among candidate entities
    std::vector<EntityId> cands;
    if (goal.target != kNoEntity) cands.push_back(goal.target);
    else for (EntityId id : known) cands.push_back(id);
    std::int64_t best_dist = LLONG_MAX;
    bool any_alive = false;
    for (EntityId id : cands) {
        const Entity* e = w.find(id);
        if (!e || !e->alive) continue;
        any_alive = true;
        if (!w.prop_of(*e, goal.prop)) continue;
        const std::int64_t cur = w.prop_int(*e, goal.prop, 0);
        const std::int64_t d = cur > goal.value ? cur - goal.value : goal.value - cur;
        best_dist = std::min(best_dist, d);
        if (d == 0) break;
    }
    if (best_dist == LLONG_MAX) {
        r.score = any_alive ? -200000 : -500000;   // prop unknown or all dead
        return r;
    }
    if (best_dist == 0) {
        r.satisfied = true; r.score = 1000000 - steps * 50; return r;
    }
    r.score = -best_dist * 10 - steps * 10;
    return r;
}

// First law that predicts `possible` — mirrors World::interact pass 1.
bool predictable(const World& w, const InteractionContext& c) {
    for (const auto& m : w.laws) {
        if (!m->supports(w, c)) continue;
        if (m->predict(w, c).possible) return true;
    }
    return false;
}

// Utility rank of a candidate from the forward model ALONE (no clone, no
// commit): the first law that predicts `possible` — exactly the gate
// predictable() uses — scored by how directly its predicted changes move the
// goal. Lets plan_multi simulate only the most promising candidates per node
// instead of every possible one (the dominant planning cost).
bool rank_candidate(const World& w, const InteractionContext& c, EntityId npc,
                    const Goal& goal, std::int64_t& q) {
    for (const auto& m : w.laws) {
        if (!m->supports(w, c)) continue;
        PredictionResult p = m->predict(w, c);
        if (!p.possible) continue;
        q = 0;
        for (const StateChange& ch : p.changes) {
            q += 5;                                       // any effect beats a no-op
            if (ch.before.type != ValType::Integer || ch.after.type != ValType::Integer)
                continue;
            if (goal.kind == Goal::Kind::SatisfyNeed) {
                if (ch.entity == npc && ch.prop == goal.need && ch.after.i < ch.before.i)
                    q += 1000;
            } else if (ch.prop == goal.prop &&
                       (goal.target == kNoEntity || ch.entity == goal.target)) {
                const std::int64_t db = ch.before.i > goal.value ? ch.before.i - goal.value
                                                                 : goal.value - ch.before.i;
                const std::int64_t da = ch.after.i > goal.value ? ch.after.i - goal.value
                                                                : goal.value - ch.after.i;
                if (da < db) q += 1000;
            }
        }
        q += static_cast<std::int64_t>(p.spawns.size()) * 50;
        return true;
    }
    return false;
}

// Candidate actions for one search node: every learned verb x {no target,
// each known entity, entity pairs when the known set is small}. Laws gate
// everything downstream — the planner proposes, physics disposes.
std::vector<InteractionContext> gen_candidates(const World& w, EntityId npc,
                                               const AgentMind& mind,
                                               const std::set<EntityId>& known) {
    std::vector<InteractionContext> out;
    std::vector<EntityId> targets;
    for (EntityId id : known) {
        const Entity* e = w.find(id);
        if (e && e->alive) targets.push_back(id);
    }
    // bonded charges (Nhịp 4.5): deeply-held positive engrams are valid
    // targets even when the entity has left known_entities — memory IS
    // the agent's map. Approach moves aim at the last-seen position.
    std::vector<std::pair<EntityId, Vec2>> bonded;
    for (const auto& [id, ms] : mind.memory) {
        if (ms.engram.emotional_charge < 600) continue;
        const Entity* e = w.find(id);
        if (!e || !e->alive) continue;
        if (std::find(targets.begin(), targets.end(), id) == targets.end())
            targets.push_back(id);
        bonded.emplace_back(id, ms.pos);
    }
    for (const std::string& verb : mind.known_actions) {
        out.push_back(mk(npc, {}, verb));                    // actor-only
        for (EntityId t : targets) out.push_back(mk(npc, {t}, verb));
        if (targets.size() <= 12)                            // bounded pairs
            for (std::size_t i = 0; i < targets.size(); ++i)
                for (std::size_t j = i + 1; j < targets.size(); ++j)
                    out.push_back(mk(npc, {targets[i], targets[j]}, verb));
    }
    // move-toward-bonded: locomotion candidates carry the remembered
    // position as destination (fog-of-war: last seen, not live truth)
    if (mind.known_actions.count("move"))
        for (const auto& [id, pos] : bonded) {
            InteractionContext c;
            c.actor = npc; c.targets = {id};
            c.parameters.action = "move";
            c.parameters.destination = pos;
            c.parameters.has_destination = true;
            c.parameters.force = 100000; c.parameters.ticks = 1;
            out.push_back(std::move(c));
        }
    return out;
}

}  // namespace

std::vector<Plan> plan_multi(const World& w, EntityId npc, const Goal& goal,
                             int max_depth, std::size_t beam_width,
                             std::size_t max_results) {
    std::vector<Plan> results;
    const AgentMind* mind = w.find_mind(npc);
    if (!mind || mind->known_actions.empty() || max_depth <= 0)
        return results;

    // root: LOCAL clone only — entities/terrain within sight range of the
    // thinking agent (plus structurally linked parts/tools and its own
    // mind). Cloning the whole map + causal ledger per candidate is what
    // made autonomous runs exhaust memory (Phase 9 fog-of-war).
    const Entity* me = w.find(npc);
    if (!me || !me->alive) return results;
    const PropId sight = w.props.find_name("sight_range")
        ? w.props.find_name("sight_range")->id : kNoProp;
    const std::int64_t sight_r = sight != kNoProp ? w.prop_int(*me, sight, 256) : 256;
    const std::int64_t reach_p = w.props.find_name("reach")
        ? w.props.find_name("reach")->id : kNoProp;
    const std::int64_t reach_r = reach_p != kNoProp ? w.prop_int(*me, reach_p, 64) : 64;
    const std::int64_t radius = sight_r + reach_r + 128;   // see + act margin
    World root_world = w.clone_local(me->pos, radius, npc);
    const std::int64_t root_need = goal.kind == Goal::Kind::SatisfyNeed
        ? need_value(root_world, npc, goal.need) : 0;
    {
        EvalResult ev = evaluate(root_world, npc, goal, root_need,
                                 mind->known_entities, 0);
        if (ev.satisfied) {
            results.push_back({{}, 1000000, true});
            return results;
        }
    }

    std::vector<SearchNode> beam;
    {
        SearchNode n;
        n.w = std::make_unique<World>(std::move(root_world));
        n.known = mind->known_entities;
        n.score = evaluate(*n.w, npc, goal, root_need, n.known, 0).score;
        beam.push_back(std::move(n));
    }

    // Speculative-world pool: candidates that satisfy the goal (or whose
    // expanded world is pruned by the beam) would otherwise be destroyed
    // and the next candidate would pay fresh top-level allocations. The
    // pool recycles those World shells — clone() move-assigns into them,
    // so steady-state planning stops churning heap per candidate.
    std::vector<std::unique_ptr<World>> spec_pool;
    auto fresh_spec = [&](const World& src) -> std::unique_ptr<World> {
        if (!spec_pool.empty()) {
            auto w = std::move(spec_pool.back());
            spec_pool.pop_back();
            *w = src.clone();
            return w;
        }
        return std::make_unique<World>(src.clone());
    };
    auto recycle_spec = [&](std::unique_ptr<World>&& w) {
        if (spec_pool.size() < 32) spec_pool.push_back(std::move(w));
    };

    for (int depth = 1; depth <= max_depth && !beam.empty(); ++depth) {
        std::vector<SearchNode> next;
        for (auto& node : beam) {
            const AgentMind* nm = node.w->find_mind(npc);
            if (!nm) continue;
            std::vector<InteractionContext> cands =
                gen_candidates(*node.w, npc, *nm, node.known);
            // Utility pre-ranking: only the top kMaxExpand candidates by
            // predicted goal progress get the expensive clone + interact.
            constexpr std::size_t kMaxExpand = 16;
            std::vector<std::pair<std::int64_t, std::size_t>> ranked;
            ranked.reserve(cands.size());
            for (std::size_t ci = 0; ci < cands.size(); ++ci) {
                std::int64_t q = 0;
                if (rank_candidate(*node.w, cands[ci], npc, goal, q))   // forward model gate
                    ranked.push_back({q, ci});
            }
            std::stable_sort(ranked.begin(), ranked.end(),
                             [](const auto& a, const auto& b) { return a.first > b.first; });
            if (ranked.size() > kMaxExpand) ranked.resize(kMaxExpand);
            for (const auto& rk : ranked) {
                const InteractionContext& c = cands[rk.second];
                std::unique_ptr<World> spec = fresh_spec(*node.w);
                InteractionResult res = spec->interact(c);
                if (!res.completed) { recycle_spec(std::move(spec)); continue; }
                SearchNode child;
                child.path = node.path;
                child.path.push_back(c);
                child.known = node.known;
                for (EntityId s : res.spawned) child.known.insert(s);
                EvalResult ev = evaluate(*spec, npc, goal, root_need,
                                         child.known,
                                         static_cast<std::int64_t>(child.path.size()));
                // Utility distortion (Nhịp 3c): anhedonia — reward_gain
                // near 0 collapses every positive goal score, so a
                // depressed mind simply stops trying. No flag, just gain.
                neuro_touch(*spec, npc);   // clone: tune axes from props once
                const AgentMind* lmind = spec->find_mind(npc);
                if (lmind && ev.score > 0)
                    ev.score = ev.score * lmind->neuro.reward_gain / 1000;
                child.score = ev.score + belief_bonus(*spec, npc, c, res)
                            + (lmind ? compulsion_bonus(*lmind,
                                     c.parameters.action)
                                     + bond_bonus(*lmind, c)
                                     + care_bonus(*spec, npc, *lmind, c)
                                     + sublimation_bonus(*lmind,
                                                        c.parameters.action)
                                     : 0);
                if (lmind)
                    child.score += conformity_bonus(*spec, npc, *lmind,
                                                   c.parameters.action);
                results.push_back({child.path, child.score, ev.satisfied});
                if (!ev.satisfied) {
                    // Incremental beam pruning: never hold more than
                    // beam_width speculative Worlds (each is a full clone).
                    // Ties keep the earlier child, as a stable sort would.
                    if (beam_width == 0) { recycle_spec(std::move(spec)); continue; }
                    if (next.size() >= beam_width) {
                        std::size_t worst = 0;
                        for (std::size_t i = 1; i < next.size(); ++i)
                            if (next[i].score <= next[worst].score) worst = i;
                        if (child.score <= next[worst].score) {
                            recycle_spec(std::move(spec));
                            continue;
                        }
                        recycle_spec(std::move(next[worst].w));
                        next.erase(next.begin() + static_cast<std::ptrdiff_t>(worst));
                    }
                    child.w = std::move(spec);
                    next.push_back(std::move(child));
                } else {
                    recycle_spec(std::move(spec));
                }
            }
        }
        std::stable_sort(next.begin(), next.end(),
                         [](const SearchNode& a, const SearchNode& b) {
                             return a.score > b.score;
                         });
        beam = std::move(next);
    }

    std::sort(results.begin(), results.end(), [](const Plan& a, const Plan& b) {
        if (a.satisfied != b.satisfied) return a.satisfied > b.satisfied;
        return a.score > b.score;
    });
    if (results.size() > max_results) results.resize(max_results);
    return results;
}

// ============================ Phase 8: social consequences ======================
namespace {

PropId s_pid(const World& w, const char* n) {
    const PropertyDef* d = w.props.find_name(n);
    return d ? d->id : kNoProp;
}

// A taught/inscribed record enters the learner's mind as an UNVERIFIED claim:
// same method key and hard-won failed_conditions, but zero self-generated
// evidence — Verified must be earned by the learner's own experiments (§13:
// recorded knowledge transmits claims, not proof).
void teach_record(AgentMind& learner, const DiscoveryRecord& src,
                  std::uint64_t tick) {
    auto it = learner.discoveries.find(src.key);
    if (it == learner.discoveries.end()) {
        DiscoveryRecord d;
        d.key = src.key; d.claim = src.claim;
        d.supporting_laws = src.supporting_laws;
        d.failed_conditions = src.failed_conditions;
        // a Refuted claim (e.g. a witnessed poison) arrives AS refuted —
        // taboos must not reset to Open just because they were taught.
        d.status = src.status == DiscoveryStatus::Refuted
            ? DiscoveryStatus::Refuted : DiscoveryStatus::Open;
        d.evidence_against = d.status == DiscoveryStatus::Refuted ? 80 : 0;
        d.last_tick = tick;
        // taught knowledge is an ordinary memory trace — vivid enough to
        // act on, but it still fades unless the learner rehearses it.
        const MindParams mp;
        d.engram.clarity = mp.base_clarity;
        d.engram.stability = mp.base_stability;
        learner.discoveries.emplace(src.key, std::move(d));
        return;
    }
    DiscoveryRecord& d = it->second;
    if (d.status == DiscoveryStatus::Open) {
        // merge hard-won boundaries: union by `what`, keep strictest required
        for (const Condition& c : src.failed_conditions) {
            auto jt = std::find_if(d.failed_conditions.begin(),
                                   d.failed_conditions.end(),
                                   [&](const Condition& x) { return x.what == c.what; });
            if (jt == d.failed_conditions.end()) d.failed_conditions.push_back(c);
            else if (c.required > jt->required) *jt = c;
        }
        std::sort(d.failed_conditions.begin(), d.failed_conditions.end(),
                  [](const Condition& a, const Condition& b) { return a.what < b.what; });
        for (const std::string& l : src.supporting_laws)
            if (std::find(d.supporting_laws.begin(), d.supporting_laws.end(), l) ==
                d.supporting_laws.end())
                d.supporting_laws.push_back(l);
        std::sort(d.supporting_laws.begin(), d.supporting_laws.end());
    }
    // never downgrade a record the learner already verified itself
}

std::vector<const DiscoveryRecord*> teachable_records(const AgentMind& m) {
    std::vector<const DiscoveryRecord*> out;
    for (const auto& [k, d] : m.discoveries)
        if (d.status == DiscoveryStatus::Supported ||
            d.status == DiscoveryStatus::Verified ||
            d.status == DiscoveryStatus::Refuted)   // taboos transmit too
            out.push_back(&d);
    return out;   // map iteration is already key-sorted -> deterministic
}

}  // namespace

// ---- pair-bonding / jealousy / parental imprint (Nhịp 4.5) -----------------
// No marriage flag, no morality table: attachment is just a positive
// MemoryEngram between two ids, jealousy is rage x gain, and imprint is a
// deep engram written at birth. Selection and circumstance do the rest.

// Positive social charge for shared proximity/reproduction/gifts:
// (+300..+600) scaled by pair_bond_gain x empathy_coupling.
std::int64_t bond_charge(const MindParams& mp) {
    return mem_clamp(400 * mp.pair_bond_gain * mp.empathy_coupling
                     / 1000000, 0, 1000);
}

// Write/refresh a positive engram `who` holds about `about`.
void bond_engram(World& w, EntityId who, EntityId about,
                 std::uint64_t tick, std::int64_t charge,
                 const MindParams& mp) {
    if (charge <= 0 || who == kNoEntity || about == kNoEntity || who == about)
        return;
    AgentMind& m = w.mind(who);
    upsert_memory(m, about, snapshot_now(w, about, tick), charge, mp);
}

// Deep imprint (parental mark): fixed high stability, charge scaled by
// parental_drive x empathy — drive 0 writes nothing at all.
void imprint_engram(World& w, EntityId who, EntityId about,
                    std::uint64_t tick, std::int64_t charge,
                    const MindParams& mp, std::int64_t stability) {
    if (charge <= 0 || who == kNoEntity || about == kNoEntity || who == about)
        return;
    AgentMind& m = w.mind(who);
    AgentMind::MemorySnapshot snap = snapshot_now(w, about, tick);
    auto it = m.memory.find(about);
    if (it != m.memory.end() && it->second.engram.clarity > 0) {
        // already bonded: consolidate, keep the stronger charge
        MemoryEngram& g = it->second.engram;
        if (charge > g.emotional_charge)
            g.emotional_charge = charge;
        g.clarity = mp.max_clarity;
        g.stability = std::min(mp.max_stability,
            std::max(g.stability, stability));
        return;
    }
    m.memory[about] = snap;
    MemoryEngram& g = m.memory[about].engram;
    g.clarity = mp.max_clarity;
    g.stability = std::min(mp.max_stability, stability);
    g.emotional_charge = mem_clamp(charge, -1000, 1000);
    g.rehearsal_count = 0;
    g.decay_accum = 0;
    g.last_rehearsal = tick;
}

// Do two entities share any `member_of` collective? (in-group test for
// xenophobia discount — relations are open strings, deterministic order)
bool share_collective(const World& w, EntityId a, EntityId b) {
    if (a == kNoEntity || b == kNoEntity || a == b) return false;
    std::set<EntityId> ga;
    for (const Relation& r : w.relations_of(a, "member_of"))
        if (r.a == a) ga.insert(r.b);
    for (const Relation& r : w.relations_of(b, "member_of"))
        if (r.a == b && ga.count(r.b)) return true;
    return false;
}

// Gratitude mark (Nhịp 5b): helped in crisis -> deep positive engram on
// the benefactor. Same imprint machinery, shallower stability (8500).
void gratitude_engram(World& w, EntityId who, EntityId benefactor,
                      std::uint64_t tick, const MindParams& mp) {
    const std::int64_t charge = mem_clamp(
        800 * mp.empathy_coupling / 1000, 0, 1000);
    imprint_engram(w, who, benefactor, tick, charge, mp, 8500);
}

void social_from_event(World& w, EntityId actor,
                       const std::vector<EntityId>& targets,
                       const CausalEvent& ev) {
    if (actor == kNoEntity) return;
    const PropId integrity = s_pid(w, "integrity");
    const PropId collective = s_pid(w, "collective");
    const PropId taboo = s_pid(w, "taboo_harm");

    // ---- member-conflict rule enforcement (any causal event) -----------------
    // If the actor's action destroyed integrity on an entity sharing a
    // `taboo_harm` collective with the actor, trust in the actor falls for
    // every member. Rules live as props on the group; consequences are ties.
    if (integrity != kNoProp && collective != kNoProp && taboo != kNoProp) {
        auto member_groups = [&](EntityId id, std::vector<EntityId>& out) {
            for (const Relation& rel : w.relations_of(id, "member_of")) {
                if (rel.a != id) continue;
                const Entity* g = w.find(rel.b);
                if (!g || !g->alive) continue;
                const Value* cv = w.prop_of(*g, collective);
                if (cv && cv->type == ValType::Boolean && cv->b) out.push_back(rel.b);
            }
            std::sort(out.begin(), out.end());
        };
        for (const StateChange& ch : ev.changes) {
            if (ch.prop != integrity || ch.entity == actor || !ch.had_before) continue;
            if (ch.after.type != ValType::Integer || ch.before.type != ValType::Integer)
                continue;
            if (ch.after.i >= ch.before.i) continue;    // only harm, not healing
            const Entity* victim = w.find(ch.entity);
            if (!victim || !victim->alive) continue;
            std::vector<EntityId> ga, gv;
            member_groups(actor, ga); member_groups(ch.entity, gv);
            for (EntityId g : ga) {
                if (std::find(gv.begin(), gv.end(), g) == gv.end()) continue;
                const Entity* ge = w.find(g);
                if (!ge) continue;
                const Value* tv = w.prop_of(*ge, taboo);
                if (!tv || tv->type != ValType::Boolean || !tv->b) continue;
                // every member's trust in the perpetrator drops; the victim's
                // most of all. Deterministic: members iterate sorted edges.
                for (const Relation& rel : w.relations_of(g, "member_of")) {
                    if (rel.b != g || rel.a == actor || rel.a == ch.entity) continue;
                    w.adjust_tie(rel.a, "trust", actor, -100);
                }
                w.adjust_tie(ch.entity, "trust", actor, -200);
            }
        }
    }

    // ---- knowledge transmission ------------------------------------------------
    if (ev.action == "communicate" && targets.size() == 1) {
        const EntityId learner_id = targets.front();
        if (learner_id != actor && w.find(learner_id) && w.find(learner_id)->alive) {
            AgentMind& teacher = w.mind(actor);
            AgentMind& learner = w.mind(learner_id);
            const std::int64_t trust = w.tie_weight(actor, "trust", learner_id);
            // Nhịp 4d: theory_of_mind scales persuasion — a mind that reads
            // others teaches more per exchange and earns authority faster.
            std::int64_t tom = 1000;
            if (const Entity* td = w.find(actor))
                tom = mind_params(w, *td).theory_of_mind;
            // trust -> how many verified claims transfer (0 trust: 1, 500: 3, 1000: 5)
            const std::int64_t k = std::clamp<std::int64_t>(
                (trust + 250) / 250 * tom / 1000, 0, 8);
            std::size_t done = 0, fresh = 0;
            for (const DiscoveryRecord* d : teachable_records(teacher)) {
                if (done >= static_cast<std::size_t>(k)) break;
                const bool is_new = learner.discoveries.count(d->key) == 0;
                teach_record(learner, *d, ev.tick);
                if (is_new) ++fresh;
                ++done;
            }
            // teaching something the learner didn't know earns authority —
            // natural leaders are the ones who know things (§12.3);
            // theory_of_mind scales how convincingly it lands
            if (fresh > 0)
                w.adjust_tie(learner_id, "authority", actor,
                             100 * tom / 1000);
            // a well-aimed explanation builds mutual trust — scaled by tom
            const std::int64_t tom_trust = 50 * tom / 1000;
            if (tom_trust > 0) {
                w.adjust_tie(actor, "trust", learner_id, tom_trust);
                w.adjust_tie(learner_id, "trust", actor, tom_trust);
            }
            // verbs are cheap: the learner picks up what it saw used
            std::size_t na = 0;
            for (const std::string& a : teacher.known_actions) {
                if (na >= 16) break;
                if (learner.known_actions.insert(a).second) ++na;
            }
        }
    } else if (ev.action == "read" && targets.size() == 1) {
        const EntityId src = targets.front();
        const PropId ins = s_pid(w, "inscription");
        const Entity* se = w.find(src);
        if (ins != kNoProp && se) {
            const Value* iv = w.prop_of(*se, ins);
            if (iv && iv->type == ValType::Text && !iv->s.empty()) {
                AgentMind& learner = w.mind(actor);
                std::istringstream lines(iv->s);
                std::string line;
                std::size_t n = 0;
                while (std::getline(lines, line) && n < 8) {
                    // format: R|key|status|efor|eagainst|trials|claim (6 fields)
                    if (line.rfind("R|", 0) != 0) continue;
                    std::vector<std::string> f;
                    std::string cur;
                    std::istringstream ls(line.substr(2));
                    while (std::getline(ls, cur, '|')) f.push_back(cur);
                    if (f.size() < 6 || f[0].empty()) continue;
                    DiscoveryRecord d;
                    d.key = f[0]; d.claim = f[5];
                    d.status = DiscoveryStatus::Open;
                    d.last_tick = ev.tick;
                    teach_record(learner, d, ev.tick);
                    ++n;
                }
            }
        }
    } else if (ev.action == "reproduce") {
        // pair-bond the parents AND imprint the newborn — all through the
        // same engram machinery, scaled by heritable gains only.
        const Entity* ae = w.find(actor);
        if (ae) {
            const MindParams mp = mind_params(w, *ae);
            const EntityId partner = targets.empty() ? kNoEntity : targets.front();
            if (partner != kNoEntity && w.find(partner) && w.find(partner)->alive) {
                const std::int64_t bc = bond_charge(mp);
                bond_engram(w, actor, partner, ev.tick, bc, mp);
                bond_engram(w, partner, actor, ev.tick, bc, mp);
                // parental imprint: deep mark each way parent<->child;
                // drive ~0 writes nothing -> the lineage abandons its young
                const std::int64_t pc = mem_clamp(
                    900 * mp.parental_drive * mp.empathy_coupling / 1000000,
                    0, 1000);
                for (EntityId ch : ev.spawned) {
                    if (!w.find(ch)) continue;
                    imprint_engram(w, actor, ch, ev.tick, pc, mp, 9500);
                    imprint_engram(w, partner, ch, ev.tick, pc, mp, 9500);
                    imprint_engram(w, ch, actor, ev.tick, pc, mp, 9500);
                    imprint_engram(w, ch, partner, ev.tick, pc, mp, 9500);
                }
            }
        }
    } else if (ev.action == "share" && targets.size() == 1) {
        // a gift builds goodwill in both directions — small, deterministic
        w.adjust_tie(actor, "trust", targets.front(), 50);
        w.adjust_tie(targets.front(), "trust", actor, 50);
        const Entity* t = w.find(targets.front());
        const PropId hunger = s_pid(w, "hunger");
        const PropId integ = s_pid(w, "integrity");
        const bool needy = t &&
            ((hunger != kNoProp && w.prop_int(*t, hunger, 0) > 500) ||
             (integ != kNoProp && w.prop_int(*t, integ, 1000) < 500));
        if (needy) {
            // generosity to the desperate creates obligation (§12.3 exchange)
            w.adjust_tie(targets.front(), "debt", actor, 150);
        }
        // repaying: a debtor who shares with their creditor pays the debt down
        // (overpaying reverses it — negative debt is the creditor's claim)
        if (w.tie_weight(actor, "debt", targets.front()) > 0)
            w.adjust_tie(actor, "debt", targets.front(), -150);
        // pair-bond: gifts are the cheapest bonding signal there is
        if (const Entity* ae = w.find(actor)) {
            const MindParams mp = mind_params(w, *ae);
            const std::int64_t bc = bond_charge(mp);
            bond_engram(w, actor, targets.front(), ev.tick, bc, mp);
            bond_engram(w, targets.front(), actor, ev.tick, bc, mp);
            // reciprocity: a gift landing on a starving recipient writes a
            // gratitude mark — deep positive engram on the giver
            if (const Entity* te = w.find(targets.front())) {
                const PropId hp = s_pid(w, "hunger");
                const PropId tp = s_pid(w, "thirst");
                const bool crisis =
                    (hp != kNoProp && w.prop_int(*te, hp, 0) >= 800) ||
                    (tp != kNoProp && w.prop_int(*te, tp, 0) >= 800);
                if (crisis) gratitude_engram(w, targets.front(), actor,
                                             ev.tick, mp);
            }
        }
    } else if (ev.action == "withdraw" && targets.size() == 1) {
        // ration withdrawal: members in need draw clean; outsiders are thieves
        const PropId hunger = s_pid(w, "hunger");
        const PropId coll = s_pid(w, "collective");
        const EntityId group = targets.front();
        bool member = false, hungry = false;
        if (const Entity* ae = w.find(actor)) {
            member = w.relations.related(actor, "member_of", group);
            hungry = hunger != kNoProp && w.prop_int(*ae, hunger, 0) > 700;
        }
        (void)coll;
        if (member && hungry) {
            // legitimate ration — no penalty
        } else if (member) {
            w.adjust_tie(group, "trust", actor, -25);   // taking extra
        } else {
            // outsider raiding the common store: every member's trust drops
            for (const Relation& rel : w.relations_of(group, "member_of")) {
                if (rel.b == group && rel.a != actor)
                    w.adjust_tie(rel.a, "trust", actor, -100);
            }
        }
    }

    // ---- protector's authority --------------------------------------------------
    // If this event's actor damaged entity T, and T recently harmed some B,
    // then B respects the actor who struck the threat down. Bounded scan of
    // the ledger tail — recent aggression only.
    if (integrity != kNoProp) {
        std::vector<EntityId> struck;
        for (const StateChange& ch : ev.changes) {
            if (ch.prop == integrity && ch.had_before && ch.entity != actor &&
                ch.after.type == ValType::Integer && ch.before.type == ValType::Integer &&
                ch.after.i < ch.before.i)
                struck.push_back(ch.entity);
        }
        if (!struck.empty()) {
            const auto& hist = w.ledger.history();
            const std::size_t tail = std::min<std::size_t>(hist.size(), 32);
            for (EntityId t : struck) {
                std::set<EntityId> victims;
                for (std::size_t i = hist.size() - tail; i < hist.size(); ++i) {
                    const CausalEvent& pe = hist[i];
                    if (pe.failed || pe.action == "metabolize" || pe.action == "cool")
                        continue;
                    if (pe.participants.empty() || pe.participants.front() != t)
                        continue;
                    for (const StateChange& pc : pe.changes)
                        if (pc.prop == integrity && pc.had_before && pc.entity != t &&
                            pc.after.type == ValType::Integer &&
                            pc.before.type == ValType::Integer &&
                            pc.after.i < pc.before.i)
                            victims.insert(pc.entity);
                }
                for (EntityId b : victims) {
                    const Entity* be = w.find(b);
                    if (be && be->alive && b != actor)
                        w.adjust_tie(b, "authority", actor, 100);
                }
            }
        }
    }
}

// ---- World::decay_minds (Nhịp 1) ------------------------------------------------
// One integer-exact recurrence forgets every engram: accum += dt*rate,
// drop = accum/stability, clarity -= drop, accum %= stability. Because the
// remainder is kept, running dt=1 N times equals one dt=N — dormant LOD
// minds and per-tick minds produce identical state_hash.
void World::decay_minds(std::int64_t n) {
    if (n <= 0) return;
    for (auto& [eid, mind] : minds_) {
        const Entity* e = find(eid);
        const MindParams p = (e && e->alive) ? mind_params(*this, *e)
                                             : MindParams{};
        const std::int64_t rate = p.decay_rate;
        auto decay_one = [&](MemoryEngram& g) -> bool {
            if (g.clarity == 0) {
                // Unborn engram (record/snapshot created without a salience
                // write — e.g. taught claims, test fixtures): give it an
                // ordinary trace instead of insta-forgetting it. A clarity
                // that decayed TO zero is erased below, never persists.
                g.clarity = p.base_clarity;
                g.stability = p.base_stability;
                g.decay_accum = 0;
                return false;
            }
            if (g.clarity < 0) return true;            // already dead
            const std::int64_t accum = g.decay_accum + n * rate;
            const std::int64_t stab = g.stability > 0 ? g.stability : 1;
            const std::int64_t drop = accum / stab;
            if (drop <= 0) {
                g.decay_accum = static_cast<std::int32_t>(accum);
                return false;
            }
            g.clarity -= drop;
            g.decay_accum = static_cast<std::int32_t>(accum % stab);
            return g.clarity <= 0;
        };
        std::vector<EntityId> gone_mem;
        for (auto& [kid, s] : mind.memory)
            if (decay_one(s.engram)) gone_mem.push_back(kid);
        for (EntityId kid : gone_mem) {
            auto it = mind.memory.find(kid);
            if (it != mind.memory.end()) mind.memory.erase(it);
        }
        // Skills are engrams too: an unused method fades and must be
        // relearned — clarity 0 erases the DiscoveryRecord entirely.
        std::vector<std::string> gone_skill;
        for (auto& [k, d] : mind.discoveries)
            if (decay_one(d.engram)) gone_skill.push_back(k);
        for (const std::string& k : gone_skill) mind.discoveries.erase(k);
        // Born marks (epigenetic trauma) decay on the same recurrence —
        // peace fades them, renewed danger consolidates them (flashback).
        if (!mind.born_memories.empty()) {
            std::vector<BornEngram> kept;
            kept.reserve(mind.born_memories.size());
            for (BornEngram& bm : mind.born_memories)
                if (!decay_one(bm.engram)) kept.push_back(std::move(bm));
            mind.born_memories = std::move(kept);
        }
        // Ostracism pain (Nhịp 5a): isolation_bias>1000 minds hurt when
        // socially cut off — no conspecific seen recently, or fresh
        // hostile marks all around. The drive is fused with emotion_decay
        // into ONE net linear term per emotion so dt=1 xN == dt=N even
        // at the 0/1000 clamps (a mid-interval clamp would diverge).
        std::int64_t ostr_p = 0, ostr_g = 0;   // ostracism drive, this call
        if (mind.neuro.tuned && mind.neuro.isolation_bias > 1000 && n > 0) {
            const std::uint64_t now = clock_.tick();
            bool saw_anyone = false;
            for (const Observation& o : mind.observations)
                if (now - o.tick <= 1000) { saw_anyone = true; break; }
            int hostile = 0;
            for (const auto& [kid, ms] : mind.memory)
                if (ms.engram.emotional_charge <= -500 &&
                    ms.engram.clarity > 0) ++hostile;
            const bool isolated = !saw_anyone || hostile >= 2;
            if (isolated) {
                // milli-emotion/tick: ~15/tick at bias 2500 vs decay 10 —
                // chronic loneliness slowly wins, briefly does not.
                const std::int64_t rate = std::min<std::int64_t>(20000,
                    10 * (mind.neuro.isolation_bias - 1000));
                mind.ostracism_accum += n * rate;
                const std::int64_t drop = mind.ostracism_accum / 1000;
                mind.ostracism_accum %= 1000;
                ostr_p = drop;
                ostr_g = drop;   // same drive — per-tick delta stays
                                 // constant, keeping dt-batch equality
            }
        }
        // Transient emotions fade linearly; ostracism adds its drive in
        // the same expression — one clamp per emotion per call.
        const std::int64_t ed = p.emotion_decay;
        const std::int64_t dec = n * ed;
        mind.neuro.panic = std::clamp<std::int64_t>(
            mind.neuro.panic + ostr_p - dec, 0, 1000);
        mind.neuro.rage  = std::max<std::int64_t>(0, mind.neuro.rage  - dec);
        mind.neuro.grief = std::clamp<std::int64_t>(
            mind.neuro.grief + ostr_g - dec, 0, 1000);
    }
}

}  // namespace simlab
