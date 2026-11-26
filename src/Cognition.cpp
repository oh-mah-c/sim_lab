#include <cctype>
#include <cstdio>
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

std::int64_t need_of(const World& w, const Entity& e, PropId id) {
    return id != kNoProp ? w.prop_int(e, id, 0) : 0;
}

// Does the target look edible to a beholder? Material nutrition/hydration
// or a fruiting plant with fruit left — the same two-layer rule the
// consume law uses, evaluated on World Truth at encoding time (the NPC
// is looking at the thing; salience is private bookkeeping).
bool looks_edible(const World& w, const Entity& t) {
    if (t.material != kNoMaterial) {
        if (const MaterialDef* md = w.materials.find(t.material)) {
            const PropId nut = SIMLAB_PID(w.props, "nutrition");
            const PropId hyd = SIMLAB_PID(w.props, "hydration");
            if (nut != kNoProp) {
                const auto it = md->props.find(nut);
                if (it != md->props.end() && it->second.i > 0) return true;
            }
            if (hyd != kNoProp) {
                const auto it = md->props.find(hyd);
                if (it != md->props.end() && it->second.i > 0) return true;
            }
        }
    }
    const PropId fmp = SIMLAB_PID(w.props, "fruit_material");
    const PropId fcp = SIMLAB_PID(w.props, "fruit_count");
    if (fmp == kNoProp || fcp == kNoProp) return false;
    const Value* fv = w.prop_of(t, fmp);
    const Value* fc = w.prop_of(t, fcp);
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
        // same effective set as prop_of() over observable_props, gathered
        // by walking the two sorted bags once (instance wins over material)
        const std::vector<PropId>& obs = w.observable_props();
        for (const auto& [p, v] : e->props.entries)
            if (std::binary_search(obs.begin(), obs.end(), p)) s.props[p] = v;
        if (e->material != kNoMaterial) {
            if (const MaterialDef* md = w.materials.find(e->material))
                for (const auto& [p, v] : md->props)
                    if (std::binary_search(obs.begin(), obs.end(), p) &&
                        !e->props.get(p)) s.props[p] = v;
        }
    }
    return s;
}

// Charge for a freshly perceived entity: survival-relevant signals only.
// Finding food while starving is the classic flashbulb memory.
std::int64_t perceive_charge(const World& w, const Entity& npc,
                             const Entity& target) {
    std::int64_t charge = 0;
    const std::int64_t hunger = need_of(w, npc, SIMLAB_PID(w.props, "hunger"));
    const std::int64_t thirst = need_of(w, npc, SIMLAB_PID(w.props, "thirst"));
    const bool desperate = hunger > 850 || thirst > 850;
    const bool urgent = desperate || hunger > 500 || thirst > 500;
    if (looks_edible(w, target))
        charge += desperate ? 600 : (urgent ? 250 : 0);
    // water is as vital as food: a drinkable-looking target in crisis
    if (urgent && !looks_edible(w, target) && target.material != kNoMaterial) {
        if (const MaterialDef* md = w.materials.find(target.material)) {
            const PropId hyd = SIMLAB_PID(w.props, "hydration");
            if (hyd != kNoProp) {
                const auto it = md->props.find(hyd);
                if (it != md->props.end() && it->second.i > 0)
                    charge += desperate ? 600 : 250;
            }
        }
    }
    // Phase 11: stench and filth repel — observable odor/contamination on
    // a perceived entity writes a negative engram charge (disgust/grief),
    // the same channel every other valence uses.
    {
        const PropId od = SIMLAB_PID(w.props, "odor");
        const PropId ct = SIMLAB_PID(w.props, "contamination");
        std::int64_t bad = 0;
        if (od != kNoProp) bad += w.prop_int(target, od, 0) * 3 / 5;
        if (ct != kNoProp) bad += w.prop_int(target, ct, 0) / 4;
        charge -= std::min<std::int64_t>(800, bad);
    }
    // Phase 11.2: visible blight/wilt marks on flora repel through the
    // same observable-prop channel — a marked plant reads as sickly even
    // when it still looks edible.
    {
        const PropId bm = SIMLAB_PID(w.props, "blight_marks");
        if (bm != kNoProp)
            charge -= std::min<std::int64_t>(500,
                w.prop_int(target, bm, 0) / 2);
    }
    return mem_clamp(charge, -1000, 1000);
}

// Charge for remembering `tid` after causal event `ev`, from `learner`'s
// side. Uses only what the event already records (changes/spawns) plus
// the learner's own body state — never hidden World Truth.
std::int64_t event_charge(const World& w, EntityId learner,
                          const CausalEvent& ev, EntityId tid) {
    std::int64_t charge = 0;
    const PropId integ = SIMLAB_PID(w.props, "integrity");
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
        if (me && (need_of(w, *me, SIMLAB_PID(w.props, "hunger")) > 850 ||
                   need_of(w, *me, SIMLAB_PID(w.props, "thirst")) > 850))
            charge += 500;
    }
    // Phase 11.2: a meal that raised the learner's toxin_load marks the
    // food as the culprit — blighted fruit forges a strong negative
    // engram through the same causal channel as every other valence.
    if (ev.action == "consume" && tid != learner) {
        const PropId tl = SIMLAB_PID(w.props, "toxin_load");
        if (tl != kNoProp) {
            for (const StateChange& c : ev.changes)
                if (c.entity == learner && c.prop == tl &&
                    c.before.type == ValType::Integer &&
                    c.after.type == ValType::Integer &&
                    c.after.i > c.before.i) { charge -= 800; break; }
        }
    }
    // poison aftermath handled separately as a negative charge (-900)
    return mem_clamp(charge, -1000, 1000);
}

}  // namespace

MindParams mind_params(const World& w, const Entity& e) {
    MindParams p;
    static thread_local SeqPidCache gi_c_;
    SeqPid gpid(gi_c_, w.props);
    auto gi = [&](const char* name, std::int64_t dflt) -> std::int64_t {
        const PropId id = gpid(name);
        return id != kNoProp ? w.prop_int(e, id, dflt) : dflt;
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
                   AgentMind::MemorySnapshot snap,
                   std::int64_t emotional_charge, const MindParams& p) {
    auto it = mind.memory.find(eid);
    const bool fresh = it == mind.memory.end() || it->second.engram.clarity <= 0;
    AgentMind::MemorySnapshot& ms = mind.memory[eid];
    ms.tick = snap.tick; ms.pos = snap.pos; ms.material = snap.material;
    ms.kind_name = std::move(snap.kind_name);
    ms.props = std::move(snap.props);
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
                     AgentMind::MemorySnapshot snap,
                     std::int64_t charge, const MindParams& p,
                     std::int64_t radius) {
    const auto it = mind.memory.find(eid);
    const bool fresh = it == mind.memory.end() || it->second.engram.clarity <= 0;
    // generalize touches only OTHER entities' entries — running it before
    // the src upsert lets the src snapshot move into memory afterwards.
    if (fresh) generalize_abstraction(w, mind, eid, snap, charge, p, radius);
    upsert_memory(mind, eid, std::move(snap), charge, p);
}

// sight radius of `e` — the honest perception bound for generalization.
std::int64_t sight_radius_of(const World& w, const Entity& e) {
    const PropertyDef* d = w.props.find_name("sight_range");
    std::int64_t r = d ? w.prop_int(e, d->id, 256) : 256;
    // Phase 10: lost eyes blind proportionally; blood_volume < 600 dims
    // vision (hypotension). Non-anatomical entities unaffected (1000).
    r = r * w.anatomy_function(e.id, "eye") / 1000;
    const PropId bv = SIMLAB_PID(w.props, "blood_volume");
    if (bv != kNoProp) {
        const Value* v = w.prop_of(e, bv);
        if (v && v->type == ValType::Integer && v->i < 600)
            r = r * std::max<std::int64_t>(200, v->i * 1000 / 600) / 1000;
    }
    return r;
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
    (void)eid; (void)world_seed;
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
    const PropId sight = SIMLAB_PID(w.props, "sight_range");
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
std::int64_t bond_bonus(const World& w, const AgentMind& mind,
                        const InteractionContext& c) {
    std::int64_t bonus = 0;
    const PropertyDef* cdl_d = w.props.find_name("corpse_decay_level");
    const PropId cdl_id = cdl_d ? cdl_d->id : kNoProp;
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
        // Phase 11 kin burial: a deep positive engram toward a corpse the
        // mind remembers (snapshot carries the corpse-only rot prop) makes
        // "combine" — the burial/sterilization verb — genuinely desirable.
        else if (q >= 600 && c.parameters.action == "combine" &&
                 cdl_id != kNoProp &&
                 it->second.props.count(cdl_id) != 0)
            bonus += q;
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
    // Phase 10.2: a blood pool remembered near the bonded is an observable
    // hemorrhage sign — memory snapshots carry material + position, both
    // legitimately perceivable. No private prop is read.
    {
        const MaterialId blood = w.materials.find_name("blood")
            ? w.materials.find_name("blood")->id : kNoMaterial;
        if (blood != kNoMaterial) {
            const auto bit = mind.memory.find(eid);
            if (bit != mind.memory.end()) {
                for (const auto& [mid, ms] : mind.memory) {
                    if (mid == eid || ms.material != blood) continue;
                    if (dist2(ms.pos, bit->second.pos) <= 96 * 96) {
                        d = std::max(d, static_cast<std::int64_t>(600));
                        break;
                    }
                }
            }
        }
    }
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
            need_of(w, *me, SIMLAB_PID(w.props, "hunger")),
            need_of(w, *me, SIMLAB_PID(w.props, "thirst")));
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
std::int64_t wound_bonus(const World& w, EntityId npc,
                         const AgentMind& mind,
                         const InteractionContext& c);
std::int64_t excrete_penalty(const World& w, EntityId npc,
                             const AgentMind& mind,
                             const InteractionContext& c);


// Wound care (Phase 10.2): "treat" on a wounded self/bonded earns a bonus
// scaled by the dressing's absorbency+antiseptic (observable props the
// agent can see) and the actor's abstraction/motor gains — the emergent
// "physician" is just high-gain minds winning the same Score(H) arena.
std::int64_t wound_bonus(const World& w, EntityId npc,
                         const AgentMind& mind,
                         const InteractionContext& c) {
    if (c.parameters.action != "treat" || c.targets.size() < 2) return 0;
    const Entity* me = w.find(npc);
    if (!me) return 0;
    const MindParams mp = mind_params(w, *me);
    const EntityId patient = c.targets[0], med = c.targets[1];
    // patient must be self or a bonded charge
    const bool self = patient == npc;
    if (!self) {
        const auto it = mind.memory.find(patient);
        if (it == mind.memory.end() ||
            it->second.engram.emotional_charge < 600) return 0;
    }
    // dressing quality from OBSERVABLE props (what the agent can see)
    const Entity* md = w.find(med);
    if (!md) return 0;
    const PropId ap = SIMLAB_PID(w.props, "absorbency");
    const PropId sp = SIMLAB_PID(w.props, "antiseptic");
    const PropId tp = SIMLAB_PID(w.props, "temperature");
    const std::int64_t absorb = ap != kNoProp ? w.prop_int(*md, ap, 0) : 0;
    const std::int64_t anti = sp != kNoProp ? w.prop_int(*md, sp, 0) : 0;
    const std::int64_t temp = tp != kNoProp ? w.prop_int(*md, tp, 0) : 0;
    const std::int64_t quality = std::max(absorb,
        std::max(anti, static_cast<std::int64_t>(temp >= 500 ? 600 : 0)));
    if (quality < 400) return 0;
    // wound severity on the patient (self = World Truth own body; bonded
    // severity already priced by care_bonus — here a flat medical pull)
    std::int64_t sev = 400;
    if (self) {
        const PropId br = SIMLAB_PID(w.props, "bleed_rate");
        const PropId iv = SIMLAB_PID(w.props, "infection_level");
        sev = 0;
        for (const Relation& rel : w.relations_of(npc, "part_of")) {
            if (rel.b != npc) continue;
            const Entity* pt = w.find(rel.a);
            if (!pt || !pt->alive) continue;
            sev = std::max(sev, br != kNoProp ? w.prop_int(*pt, br, 0) * 2 : 0);
            sev = std::max(sev, iv != kNoProp ? w.prop_int(*pt, iv, 0) : 0);
        }
        if (sev <= 0) return 0;
    }
    // physician factor: abstraction sees the remedy, motor applies it
    const std::int64_t phys =
        (mp.abstraction_gain + mp.motor_precision) / 2;
    std::int64_t bonus = quality * sev / 1000 * phys / 1000 * 4;
    if (!self) bonus = bonus * 3 / 4;    // care slightly discounted vs self
    return std::min<std::int64_t>(bonus, 1200000);
}


// Excretion privacy (Phase 10.3): voiding in front of conspecifics, near
// food/water, or while threatened carries a shame penalty scored from
// conformity_gain x abstraction_gain — high-conformity minds hold it in
// and seek a latrine (dig), low ones relieve themselves anywhere. The
// penalty is a score subtraction, never a block: urgency still wins.
std::int64_t excrete_penalty(const World& w, EntityId npc,
                             const AgentMind& mind,
                             const InteractionContext& c) {
    if (c.parameters.action != "evacuate") return 0;
    const Entity* me = w.find(npc);
    if (!me) return 0;
    const MindParams mp = mind_params(w, *me);
    std::int64_t base = 0;
    int neighbors = 0; bool threat = false;
    for (const auto& [id, ms] : mind.memory) {
        if (id == npc) continue;
        if (dist2(ms.pos, me->pos) > 128 * 128) continue;
        if (!ms.kind_name.empty() && ms.kind_name == me->kind_name)
            ++neighbors;
        if (ms.engram.emotional_charge <= -400) threat = true;
    }
    base += neighbors * 500;
    if (mind.neuro.panic >= 400) base += 800;   // too scared to linger
    if (threat) base += 1200;                     // hostile nearby: hide it
    if (base == 0) return 0;
    // shame scales with herd instinct AND self-awareness; abstraction lets
    // the mind convert pressure into planning (latrine digging) rather
    // than simply suffering it — capped so the urge always remains winnable.
    std::int64_t pen = base * mp.conformity_gain / 1000;
    pen = pen * mp.abstraction_gain / 1000;
    return std::min<std::int64_t>(pen, 8000);
}

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
            const PropId sight = SIMLAB_PID(w.props, "sight_range");
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
    const PropId sight = SIMLAB_PID(w.props, "sight_range");
    const std::int64_t range = sight != kNoProp ? w.prop_int(*me, sight, 0) : 0;
    if (range <= 0) return 0;                     // no sight -> no observations
    const std::int64_t r2 = range * range;

    // observable-domain props: the only props allowed into a mind (the
    // registry is frozen after load_content, so build the list once)
    const std::vector<PropId>& obs_props = w.observable_props();   // sorted by PropId

    const PropId exposed_p = SIMLAB_PID(w.props, "exposed");
    const PropId qty_pid_p = SIMLAB_PID(w.props, "quantity");
    const PropId cdl_id = SIMLAB_PID(w.props, "corpse_decay_level");
    const PropId buried_p = SIMLAB_PID(w.props, "buried");
    const PropId cremated_p = SIMLAB_PID(w.props, "cremated");
    const MindParams mparams = mind_params(w, *me);
    neuro_touch(w, npc);

    std::size_t added = 0;
    // Phase 12.2: perceived supply per material — what the agent actually
    // SEES nearby becomes its pa:<mat> abundance belief (private economics).
    std::map<MaterialId, std::int64_t> seen_supply;
    // previous VISUAL sighting per entity (latest wins) — tick + pos only,
    // copied out because observe() below may erase/shift the deque.
    struct LastSeen { std::uint64_t tick; Vec2 pos; };
    std::unordered_map<EntityId, LastSeen> last_seen;
    last_seen.reserve(mind.observations.size());
    for (const Observation& ob : mind.observations)
        last_seen[ob.entity] = LastSeen{ob.tick, ob.pos};
    // Flashback candidate index (exact): the sorted set of memory entries
    // whose engram is CURRENTLY deep trauma (the charge/stability half of
    // the flashback predicate). Built once, then kept exact: every engram
    // write in this call re-checks the written entry (fb_touch), and the
    // abstraction fan-out (which rewrites other entries) forces a rebuild.
    // Scanning this set in key order finds the same first match as scanning
    // the whole memory map — usually it is empty or tiny.
    const std::int64_t fb_floor = -mparams.flashback_charge;
    auto fb_eligible = [&](const MemoryEngram& g) {
        return g.emotional_charge <= fb_floor && g.emotional_charge < 0 &&
               g.stability >= mparams.flashback_stability;
    };
    std::vector<EntityId> fb_cands;
    bool fb_dirty = true;
    auto fb_rebuild = [&]() {
        fb_cands.clear();
        for (const auto& [kid, ms] : std::as_const(mind.memory))
            if (fb_eligible(ms.engram)) fb_cands.push_back(kid);
        fb_dirty = false;
    };
    auto fb_touch = [&](EntityId id) {
        if (fb_dirty) return;
        const auto mit = std::as_const(mind.memory).find(id);
        const bool elig = mit != std::as_const(mind.memory).end() && fb_eligible(mit->second.engram);
        const auto pos = std::lower_bound(fb_cands.begin(), fb_cands.end(), id);
        const bool present = pos != fb_cands.end() && *pos == id;
        if (elig && !present) fb_cands.insert(pos, id);
        else if (!elig && present) fb_cands.erase(pos);
    };
    for (const EntityId eid : w.entities_near(me->pos, range)) {
        if (eid == npc) continue;
        const Entity* e = w.find(eid);
        if (!e) continue;
        // Phase 11: the dead are visible only as unburied, uncremated
        // remains — a corpse is a thing in the world (and a source of
        // stench), an interred or burned husk is not.
        if (!e->alive) {
            auto dead_visible = [&]() {
                if (cdl_id == kNoProp || !w.prop_of(*e, cdl_id)) return false;
                if (buried_p != kNoProp && w.prop_int(*e, buried_p, 0) != 0)
                    return false;
                if (cremated_p != kNoProp && w.prop_int(*e, cremated_p, 0) != 0)
                    return false;
                return true;
            };
            if (!dead_visible()) continue;
        }
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
        // Same effective prop set prop_of() would return for obs_props,
        // gathered by walking the two sorted bags once instead of ~60
        // binary searches per entity. Instance props win over material
        // defaults exactly like prop_of(); o.props is a sorted map so
        // insertion order is irrelevant.
        const DepositDef* ev_dd = buried ? w.deposits.find(e->deposit) : nullptr;
        auto admit = [&](PropId p) {
            if (buried && p == exposed_p) return false;   // exposure state is not a sign
            if (buried && !(ev_dd && ev_dd->evidence.get(p))) return false;
            // rot stage is a corpse sign only — a living body's material
            // default must not masquerade as one in memory snapshots.
            if (e->alive && p == cdl_id) return false;
            return std::binary_search(obs_props.begin(), obs_props.end(), p);
        };
        for (const auto& [p, v] : e->props.entries)
            if (admit(p)) o.props[p] = v;
        if (e->material != kNoMaterial) {
            if (const MaterialDef* md = w.materials.find(e->material))
                for (const auto& [p, v] : md->props)
                    if (admit(p) && !e->props.get(p)) o.props[p] = v;
        }
        // Time-warp LOD: the entity has not moved since its last memory
        // write — the engram is still fresh, so skip the snapshot rebuild,
        // salience charge and social side-effects for lod_stride_-1 ticks.
        // The Observation itself is still emitted (sensor stays live);
        // only the memory/engrams path is cached. Never active at stride 1.
        bool mem_fresh = false;
        if (w.lod_stride() > 1) {
            const auto mit = mind.memory.find(eid);
            if (mit != mind.memory.end() &&
                mit->second.pos.x == e->pos.x &&
                mit->second.pos.y == e->pos.y &&
                static_cast<std::int64_t>(o.tick - mit->second.tick) <
                    w.lod_stride())
                mem_fresh = true;
        }
        if (!mem_fresh) {
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
        // O(1): last sighting per entity, indexed once per perceive() call
        // (this entity's own record is untouched until its observe() below)
        const auto lsit = last_seen.find(eid);
        const LastSeen* prev_obs = lsit == last_seen.end() ? nullptr : &lsit->second;
        const bool had_engram =
            mind.memory.find(eid) != mind.memory.end();
        const std::int64_t pcharge = perceive_charge(w, *me, *e);
        upsert_memory_g(w, mind, eid, std::move(snap), pcharge, mparams, range);
        // abstraction fan-out may rewrite OTHER entries -> full re-index
        if (mparams.abstraction_gain > 1200 &&
            mem_abs(pcharge) >= mparams.imprint_threshold) fb_dirty = true;
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
        fb_touch(eid);                 // after upsert + xenophobia rewrite
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
            if (fb_dirty) fb_rebuild();
            for (const EntityId kid : fb_cands) {
                auto mit = mind.memory.find(kid);   // few: deep traumas only
                if (mit == mind.memory.end()) continue;
                AgentMind::MemorySnapshot& ms = mit->second;
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
        if (e->material != kNoMaterial && e->alive) {
            std::int64_t q = 1;
            if (qty_pid_p != kNoProp) {
                const Value* qv = w.prop_of(*e, qty_pid_p);
                if (qv && qv->type == ValType::Integer && qv->i > 0)
                    q = qv->i;
            }
            seen_supply[e->material] += q;
        }
        }   // !mem_fresh — warp LOD cache
        mind.observe(std::move(o));
        ++added;
    }
    // fold the visual supply survey into abundance beliefs (EMA):
    // abundance is RELATIVE — 1000 = "as much as I ever see", the exact
    // scale matters less than the trend the agent experiences.
    {
        for (const auto& [mat, total] : seen_supply) {
            const std::int64_t sample = std::min<std::int64_t>(total * 120, 1000);
            Belief& b = mind.belief("pa:" + std::to_string(mat));
            const bool fresh = b.evidence_for == 0 && b.confidence == 0;
            b.confidence = fresh ? sample
                                 : (b.confidence * 3 + sample) / 4;
            b.evidence_for += sample / 10 + 1;
            b.last_tick = w.clock_.tick();
            if (b.status == BeliefStatus::Unknown ||
                b.status == BeliefStatus::Hypothesized)
                b.status = BeliefStatus::Observed;
        }
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
    const PropId integ = SIMLAB_PID(w.props, "integrity");
    if (integ == kNoProp) return;
    bool harmed = false;
    for (const StateChange& c : ev.changes)
        if (c.entity == eater && c.prop == integ &&
            c.before.type == ValType::Integer && c.after.type == ValType::Integer &&
            c.after.i < c.before.i) { harmed = true; break; }
    if (!harmed) return;
    const PropId fmp = SIMLAB_PID(w.props, "fruit_material");
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


// Primitive medicine gratitude (Phase 10.2): being treated for a real
// wound writes a deep positive engram on the healer — the recipient's
// own body state is first-person knowledge. Called from both the actor
// path (self-treatment) and the witness path (bonded rescue).
static void treat_gratitude(World& w, EntityId npc, const CausalEvent& ev,
                            const MindParams& mp) {
    if (ev.action != "treat" || ev.participants.empty() ||
        ev.participants.front() == npc) return;
    const PropId br = SIMLAB_PID(w.props, "bleed_rate");
    const PropId iv = SIMLAB_PID(w.props, "infection_level");
    if (br == kNoProp && iv == kNoProp) return;
    // treat writes bleed/infection deltas on the patient's PART entities —
    // gather them plus the body itself.
    std::set<EntityId> mine{npc};
    for (const Relation& rel : w.relations_of(npc, "part_of"))
        if (rel.b == npc) mine.insert(rel.a);
    for (const StateChange& ch : ev.changes) {
        if (!mine.count(ch.entity)) continue;
        const bool hit = (br != kNoProp && ch.prop == br) ||
                         (iv != kNoProp && ch.prop == iv);
        if (hit && ch.before.type == ValType::Integer &&
            ch.after.type == ValType::Integer && ch.after.i < ch.before.i) {
            gratitude_engram(w, npc, ev.participants.front(), ev.tick, mp);
            return;
        }
    }
}

// Maritime lessons (Primordial): drowning and capsizing are written into the
// event evidence by the locomotion law; living through one, or seeing one,
// leaves a learned fear of deep water that the shore sense weighs before
// every crossing. Belief only — World Truth is untouched.
void maritime_lesson(World& w, AgentMind& mind, const CausalEvent& ev, bool self) {
    if (ev.evidence.find("drown") == std::string::npos &&
        ev.evidence.find("capsized") == std::string::npos) return;
    const bool died = ev.evidence.find("drowned") != std::string::npos;
    const std::int64_t gain = ev.evidence.find("capsized") != std::string::npos ? 200
                            : died ? 600 : (self ? 400 : 300);
    Belief& b = mind.belief("peril:deep_water");
    b.confidence = std::min<std::int64_t>(1000, b.confidence + gain);
    b.evidence_for += gain;
    if (self) b.self_evidence += gain;
    b.last_tick = ev.tick;
    if (b.status == BeliefStatus::Unknown || b.status == BeliefStatus::Hypothesized)
        b.status = BeliefStatus::Observed;
    (void)w;
}

// ---- Phase 19: two-tier physics cognition -----------------------------------
// Tier 1 `phenomenon:<domain>` — qualitative recognition: wood floats and
// stone sinks, a drawn bow flies farther, fierce fire melts ore. No math
// required; these beliefs only bias estimation (existing outcome/yield
// machinery does the qualitative work).
// Tier 2 gate `math:<tier>` — a tribe invents counting, ratios or
// geometry only after repeated quantitative NEED (`mathneed:<tier>`
// accumulates from craft events) plus sufficient abstraction. Inventing
// math is a belief, never a World Truth write.
// Tier 2 result `qlaw:<domain>:p<param>` — a quantitative law proven by
// measuring real World quantities (the same local-sense precedent as
// shore_sense: a scholar's instrument reading, not agent omniscience).
// The winning integer parameter rides in the key, so planners can apply
// it verbatim; transmitted copies arrive halved — claims, not proof.
namespace {

void qbump(AgentMind& mind, const std::string& key, std::int64_t gain,
           std::uint64_t tick) {
    Belief& b = mind.belief(key);
    if (b.status == BeliefStatus::Unknown)
        b.status = BeliefStatus::Observed;
    b.confidence = std::min<std::int64_t>(1000, b.confidence + gain);
    b.evidence_for += gain;
    b.self_evidence += gain;
    b.last_tick = tick;
}

// event -> (phenomenon domain or "", math tier or "")
void quantum_observe(World& w, AgentMind& mind, EntityId npc,
                     const CausalEvent& ev) {
    const Entity* me = w.find(npc);
    if (!me) return;
    std::string domain, tier, tier2;
    if (ev.action == "process" || ev.action == "combine") {
        domain = "smelt"; tier = "ratio";
    } else if (ev.action == "impact" || ev.action == "strike") {
        domain = "trajectory"; tier = "geometry";
    } else if (ev.action == "sow" || ev.action == "plant" ||
               ev.action == "dig" || ev.action == "irrigate") {
        tier = "geometry";
    } else if (ev.action == "share" || ev.action == "consume") {
        tier = "count";
    }
    // buoyancy: any craft (float_capacity > 0) touched by the event —
    // boarding it, loading it, or watching it float/foundering. Loading a
    // hull is a RATIO problem (load vs lift) on top of mere counting.
    const PropId p_fc = SIMLAB_PID(w.props, "float_capacity");
    if (p_fc != kNoProp) {
        for (EntityId pid2 : ev.participants) {
            const Entity* pe = w.find(pid2);
            if (pe && pe->alive && w.prop_int(*pe, p_fc, 0) > 0) {
                if (domain.empty()) domain = "buoyancy";
                if (tier == "count") tier2 = "ratio";
                else if (tier.empty()) tier = "ratio";
                break;
            }
        }
    }
    if (domain.empty() && tier.empty() && tier2.empty()) return;
    const std::int64_t gain = ev.failed ? 2 : 6;
    if (!domain.empty() && !ev.failed)
        qbump(mind, "phenomenon:" + domain, gain, ev.tick);
    for (const std::string* t : {&tier, &tier2}) {
        if (t->empty()) continue;
        qbump(mind, "mathneed:" + *t, gain * 2, ev.tick);
        // invention: sustained quantitative need + a mind that can abstract
        const Belief* need = mind.find_belief("mathneed:" + *t);
        const Belief* have = mind.find_belief("math:" + *t);
        if (need && have == nullptr && need->confidence >= 550 &&
            mind_params(w, *me).abstraction_gain >= 1100) {
            qbump(mind, "math:" + *t, need->confidence, ev.tick);
            mind.belief("mathneed:" + *t).confidence = 0;   // need spent
        }
    }
    // quantitative proof: phenomenon + the required math tier + abstraction
    if (domain.empty() || ev.failed) return;
    const char* req = domain == "trajectory" ? "geometry" : "ratio";
    const Belief* ph = mind.find_belief("phenomenon:" + domain);
    const Belief* mt = mind.find_belief(std::string("math:") + req);
    if (!ph || !mt || ph->confidence < 650 || mt->confidence < 550 ||
        mind_params(w, *me).abstraction_gain < 1200)
        return;
    // measure the parameter from real world quantities (one reading):
    std::int64_t param = -1;
    if (domain == "smelt") {
        // fuel share of the richest smelt recipe the tribe knows: the
        // process registry is World Truth data; the mind "reads" the
        // ratio off the crucible scale it invented with its math.
        std::int64_t best = -1;
        for (const ProcessDef& pd : w.processes.defs()) {
            if (pd.fuels.empty() || pd.inputs.empty()) continue;
            std::int64_t fq = 0, tq = 0;
            for (const ProcessIO& f : pd.fuels) { fq += f.qty; tq += f.qty; }
            for (const ProcessIO& i : pd.inputs) tq += i.qty;
            if (tq <= 0) continue;
            const std::int64_t p = fq * 1000 / tq;
            if (p > best) best = p;
        }
        if (best > 0) param = best;
    } else if (domain == "buoyancy") {
        // safe cargo units of the best craft the mind REMEMBERS: lift minus
        // the carrier's own mass. Props are physical-domain (never in
        // memory snapshots), so the scholar reads the instrument — the
        // remembered entity's live props — exactly like shore_sense reads
        // terrain. Only ids already in memory are eligible.
        std::int64_t cap = 0;
        for (const auto& [kid, snap] : mind.memory) {
            if (snap.kind_name != "composite") continue;
            const Entity* ce = w.find(kid);
            if (!ce || !ce->alive) continue;
            cap = std::max(cap, w.prop_int(*ce, p_fc, 0));
        }
        const PropId p_mass = SIMLAB_PID(w.props, "mass");
        const std::int64_t self = p_mass != kNoProp
            ? w.prop_int(*me, p_mass, 0) : 0;
        if (cap > self && cap > 0) {
            // parameter = safe cargo in trade units (kg-scale): lift minus
            // the carrier's own mass, converted off the gram scale. A law
            // never advises a load that founders the vessel.
            param = std::clamp<std::int64_t>((cap - self) / 1000, 1, 950);
        }
    } else if (domain == "trajectory") {
        // hardest remembered material's hardness IS the effort permille
        // a proven blow needs — the striker measured it hit by hit.
        // hardness is a material prop; the struck thing's material is
        // observable, so this stays inside remembered knowledge.
        const PropId p_hard = SIMLAB_PID(w.props, "hardness");
        if (p_hard != kNoProp) {
            std::int64_t h = 0;
            for (const auto& [kid, snap] : mind.memory) {
                const MaterialDef* md = w.materials.find(snap.material);
                if (!md) continue;
                const auto hit = md->props.find(p_hard);
                if (hit != md->props.end() && hit->second.type == ValType::Integer)
                    h = std::max(h, hit->second.i);
            }
            if (h > 0) param = std::clamp<std::int64_t>(h, 100, 950);
        }
    }
    if (param < 0) return;
    // already proven? reinforce the standing law instead of re-deriving
    const std::string prefix = "qlaw:" + domain + ":p";
    for (const auto& [k, b] : mind.beliefs)
        if (k.rfind(prefix, 0) == 0) {
            Belief& lb = mind.beliefs[k];
            lb.confidence = std::min<std::int64_t>(1000, lb.confidence + 20);
            lb.last_tick = ev.tick;
            return;
        }
    const std::string key = prefix + std::to_string(param);
    Belief& q = mind.belief(key);
    q.status = BeliefStatus::Verified;
    q.confidence = std::min<std::int64_t>(
        1000, (ph->confidence + mt->confidence) / 2 + 200);
    q.evidence_for = q.self_evidence = ph->confidence;
    q.last_tick = ev.tick;
}

}  // namespace

// Indigenous mathematics notation (Phase 19): each language_id hashes to a
// base in {8,10,12,20} and a glyph style {tally, dot-bar, script}. Values
// render positionally — never an Arabic digit. Deterministic in
// (language, seed): the same tongue always writes the same numerals.
std::int64_t math_base_of(std::int64_t lang, std::uint64_t seed) {
    static const std::int64_t kBases[] = {8, 10, 12, 20};
    std::uint64_t h = static_cast<std::uint64_t>(lang) * 0x9E3779B97F4A7C15ull
                   ^ seed * 0xD6E8FEB86659FD93ull;
    h ^= h >> 29; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 32;
    return kBases[h % 4];
}

std::string math_render(std::int64_t lang, std::int64_t value,
                        std::uint64_t seed) {
    const std::int64_t base = math_base_of(lang, seed);
    std::uint64_t h = static_cast<std::uint64_t>(lang) * 0xC2B2AE3D27D4EB4Full
                   ^ seed * 0x165667B19E3779F9ull;
    h ^= h >> 29; h *= 0x94D049BB133111EBull; h ^= h >> 32;
    const int style = static_cast<int>(h % 4);      // 0 tally 1 dot 2 bar 3 script
    static const char* kUnit[] = {"|", "•", "—", ""};
    static const char kBaseMark[] = {'A', 'B', 'C', 'D'};   // 8/10/12/20
    const int bidx = base == 8 ? 0 : base == 10 ? 1 : base == 12 ? 2 : 3;
    std::string out(1, kBaseMark[bidx]);
    if (value < 0) value = 0;
    // positional digits, most significant first, '.' separated
    std::int64_t digits[16]; int nd = 0;
    std::int64_t v = value;
    do { digits[nd++] = v % base; v /= base; } while (v > 0 && nd < 16);
    for (int i = nd - 1; i >= 0; --i) {
        const std::int64_t d = digits[i];
        if (i != nd - 1) out.push_back('.');
        if (style == 3) {                        // script: letters a..s, z=0
            out.push_back(static_cast<char>(d == 0 ? 'z' : 'a' + d - 1));
        } else if (d == 0) {
            out.push_back('o');                  // zero glyph, all styles
        } else {
            for (std::int64_t k = 0; k < d; ++k) out += kUnit[style];
        }
    }
    return out;
}

// belief tables carved onto treatises alongside the DiscoveryRecord lines —
// "M|tier|conf", "P|domain|conf", "Q|domain|param|conf". Read back by the
// same parser that imports R| lines (claims arrive at half confidence).
std::string quantum_tables(const AgentMind& mind) {
    std::string out;
    for (const char* pre : {"math:", "phenomenon:", "qlaw:"}) {
        for (const auto& [k, b] : mind.beliefs) {
            if (k.rfind(pre, 0) != 0 || b.confidence < 100) continue;
            if (!out.empty()) out.push_back('\n');
            if (pre[0] == 'm')
                out += "M|" + k.substr(5) + "|" + std::to_string(b.confidence);
            else if (pre[0] == 'p')
                out += "P|" + k.substr(11) + "|" + std::to_string(b.confidence);
            else {
                // qlaw:<domain>:p<param> -> Q|domain|param|conf
                const std::size_t c = k.rfind(":p");
                out += "Q|" + k.substr(5, c == std::string::npos ? std::string::npos : c - 5)
                     + "|" + (c == std::string::npos ? "0" : k.substr(c + 2))
                     + "|" + std::to_string(b.confidence);
            }
        }
    }
    return out;
}

// planner-side lookup: the proven parameter of a quantitative law, or -1.
// Driver/cognition layers only — laws never read beliefs.
std::int64_t qlaw_param(const AgentMind& mind, const char* domain) {
    const std::string prefix = std::string("qlaw:") + domain + ":p";
    std::int64_t best = -1;
    for (const auto& [k, b] : mind.beliefs) {
        if (k.rfind(prefix, 0) != 0 || b.confidence < 300) continue;
        const std::string num = k.substr(prefix.size());
        std::int64_t v = 0; bool ok = !num.empty();
        for (char c : num) { if (c < '0' || c > '9') { ok = false; break; }
                             v = v * 10 + (c - '0'); }
        if (ok) best = std::max(best, v);
    }
    return best;
}

void learn_from_event(World& w, EntityId npc, std::uint64_t event_id) {
    const CausalEvent* ev = w.ledger.find(event_id);
    if (!ev) return;
    AgentMind& mind = w.mind(npc);
    mind.learn_action(ev->action);
    quantum_observe(w, mind, npc, *ev);
    maritime_lesson(w, mind, *ev, /*self=*/true);
    learn_outcomes(w, mind, *ev, 34, /*self=*/true, /*mark_observed=*/true);
    learn_yields(w, mind, *ev, 34, /*self=*/true);
    // Phase 18 specialization: repeating a craft verb forges a `craft:`
    // conviction alongside the per-yield `yield:` beliefs — together they
    // are the substrate of emergent division of labor: an agent that keeps
    // succeeding at smelting/weaving/cultivating comes to prefer it, and
    // surplus (low hunger pressure) leaves the time to practice. No role
    // is assigned; the trade is earned one causal event at a time.
    {
        static const char* kCraft[] = {
            "process", "sow", "irrigate", "combine", "treat",
            "enclose", "assemble", "repair", "engrave"};
        for (const char* v : kCraft) {
            if (ev->action == v) {
                mind.reinforce(std::string("craft:") + v, 6, ev->tick, true);
                break;
            }
        }
    }
    learn_harm_taboo(w, npc, *ev);
    doctrine_from_event(w, npc, *ev);
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
        treat_gratitude(w, npc, *ev, mp);
        // Nociception (Phase 10d): a pain_level jump on the actor is a
        // somatic shock — panic vs rage split by the threat axis, and a
        // deep negative engram on the attacker (first participant).
        {
            const PropId pain_p = SIMLAB_PID(w.props, "pain_level");
            if (pain_p != kNoProp) {
                std::int64_t dpain = 0;
                for (const StateChange& ch : ev->changes)
                    if (ch.entity == npc && ch.prop == pain_p &&
                        ch.before.type == ValType::Integer &&
                        ch.after.type == ValType::Integer &&
                        ch.after.i > ch.before.i)
                        dpain += ch.after.i - ch.before.i;
                if (dpain > 0) {
                    mind.neuro.panic = std::min<std::int64_t>(1000,
                        mind.neuro.panic + dpain * mp.threat_gain / 2000);
                    mind.neuro.rage = std::min<std::int64_t>(1000,
                        mind.neuro.rage + dpain * (2000 - std::min<std::int64_t>(mp.threat_gain, 2000)) / 2000);
                    // trauma engram keyed on the attacker's kind/material
                    for (EntityId tid : ev->participants) {
                        if (tid == npc || tid == kNoEntity) continue;
                        const Entity* te = w.find(tid);
                        if (!te) break;
                        AgentMind::MemorySnapshot snap = snapshot_now(w, tid, ev->tick);
                        upsert_memory_g(w, mind, tid, snap,
                            -std::min<std::int64_t>(800, dpain * 2), mp,
                            me ? sight_radius_of(w, *me) : 0);
                        break;                      // first participant = attacker
                    }
                }
            }
        }
        apply_event_shock(w, mind, npc, *ev);
    }
}

void witness_from_event(World& w, EntityId npc, const CausalEvent& ev) {
    AgentMind& mind = w.mind(npc);
    mind.learn_action(ev.action);                      // saw the verb used
    maritime_lesson(w, mind, ev, /*self=*/false);
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
    doctrine_from_event(w, npc, ev);
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
        treat_gratitude(w, npc, ev, mp);
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
        "combine",
        "consume", "rest", "evacuate", "treat"};

    struct Scored { InteractionContext ctx; std::int64_t score; };
    std::vector<Scored> cand;

    const bool need_goal = goal.kind == Goal::Kind::SatisfyNeed;
    const PropId hunger = SIMLAB_PID(w.props, "hunger");
    const PropId thirst = SIMLAB_PID(w.props, "thirst");
    const PropId nutrition = SIMLAB_PID(w.props, "nutrition");
    const PropId hydration = SIMLAB_PID(w.props, "hydration");

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
                    if ((ch.entity == npc ||
                         (goal.target != kNoEntity && ch.entity == goal.target)) &&
                        ch.prop == goal.need) {
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
                    if ((ch.entity == npc ||
                         (goal.target != kNoEntity && ch.entity == goal.target)) &&
                        ch.prop == goal.need &&
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
        // the "need" prop may live on a body PART (lumen contents_load) —
        // goal.target names it; body needs live on the npc itself.
        const std::int64_t cur = need_value(
            w, goal.target != kNoEntity ? goal.target : npc, goal.need);
        if (cur <= goal.value) {
            r.satisfied = true; r.score = 1000000 - steps * 50; return r;
        }
        // heuristic: need reduction so far + best edible reachable in `known`
        const PropId reach = SIMLAB_PID(w.props, "reach");
        const PropId hunger = SIMLAB_PID(w.props, "hunger");
        const PropId thirst = SIMLAB_PID(w.props, "thirst");
        const PropId nutrition = SIMLAB_PID(w.props, "nutrition");
        const PropId hydration = SIMLAB_PID(w.props, "hydration");
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
                if ((ch.entity == npc ||
                     (goal.target != kNoEntity && ch.entity == goal.target)) &&
                    ch.prop == goal.need && ch.after.i < ch.before.i)
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
    auto corpse_target = [&](const Entity* e) {
        if (e->alive) return true;
        const PropId cdl = SIMLAB_PID(w.props, "corpse_decay_level");
        if (cdl == kNoProp || !w.prop_of(*e, cdl)) return false;
        const PropId b = SIMLAB_PID(w.props, "buried");
        if (b != kNoProp && w.prop_int(*e, b, 0) != 0) return false;
        const PropId cr = SIMLAB_PID(w.props, "cremated");
        if (cr != kNoProp && w.prop_int(*e, cr, 0) != 0) return false;
        return true;
    };
    std::vector<EntityId> targets;
    for (EntityId id : known) {
        const Entity* e = w.find(id);
        if (e && corpse_target(e)) targets.push_back(id);
    }
    // bonded charges (Nhịp 4.5): deeply-held positive engrams are valid
    // targets even when the entity has left known_entities — memory IS
    // the agent's map. Approach moves aim at the last-seen position.
    std::vector<std::pair<EntityId, Vec2>> bonded;
    for (const auto& [id, ms] : mind.memory) {
        if (ms.engram.emotional_charge < 600) continue;
        const Entity* e = w.find(id);
        if (!e || !corpse_target(e)) continue;
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
    const PropId sight = SIMLAB_PID(w.props, "sight_range");
    const std::int64_t sight_r = sight != kNoProp ? w.prop_int(*me, sight, 256) : 256;
    const std::int64_t reach_p = SIMLAB_PID(w.props, "reach");
    const std::int64_t reach_r = reach_p != kNoProp ? w.prop_int(*me, reach_p, 64) : 64;
    const std::int64_t radius = sight_r + reach_r + 128;   // see + act margin
    World root_world = w.clone_local(me->pos, radius, npc);
    root_world.speculative = true;   // think with body+physics only (A1)
    const std::int64_t root_need = goal.kind == Goal::Kind::SatisfyNeed
        ? need_value(root_world,
                     goal.target != kNoEntity ? goal.target : npc,
                     goal.need) : 0;
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

    // Deterministic compute budget (replay-safe: pure counters, no wall
    // clock). Each candidate costs one speculative clone + interact +
    // evaluate; unbounded beam x depth x kMaxExpand reaches ~1500 sims on
    // high-horizon minds, which is what made rounds take minutes on dense
    // maps. Per-depth and total caps keep deep plans possible while
    // bounding worst-case latency to a few hundred sims per call.
    // Primordial (game) worlds bound the imagination tighter: on planet-sized
    // maps every speculative clone is several MB, and beam + children alive
    // at once spiked RSS by ~350 MB. Legacy worlds keep the original caps.
    const bool lean = (w.ruleset & World::kRulePrimordial) != 0;
    const std::size_t kDepthExpand = lean ? 12 : 24;
    const std::size_t kTotalExpand = lean ? 96 : 160;
    if (lean) beam_width = std::min<std::size_t>(beam_width, 6);
    std::size_t total_expansions = 0;

    for (int depth = 1; depth <= max_depth && !beam.empty(); ++depth) {
        std::vector<SearchNode> next;
        std::size_t depth_expansions = 0;
        for (auto& node : beam) {
            if (total_expansions >= kTotalExpand) break;
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
                if (depth_expansions >= kDepthExpand ||
                    total_expansions >= kTotalExpand) break;
                const InteractionContext& c = cands[rk.second];
                std::unique_ptr<World> spec = fresh_spec(*node.w);
                ++depth_expansions; ++total_expansions;
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
                                     + bond_bonus(*spec, *lmind, c)
                                     + care_bonus(*spec, npc, *lmind, c)
                                     + wound_bonus(*spec, npc, *lmind, c)
                                     - excrete_penalty(*spec, npc, *lmind, c)
                                     + sublimation_bonus(*lmind,
                                                        c.parameters.action)
                                     : 0);
                if (lmind) {
                    child.score += conformity_bonus(*spec, npc, *lmind,
                                                   c.parameters.action);
                    child.score += econ_aptitude_bonus(*spec, npc, *lmind, c);
                    child.score += arms_race_bonus(*spec, *lmind, c);
                }
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
    static thread_local LitPidCache c_;
    return c_.get(w.props, n);
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


// ---- Phase 12 base core: compositional lexicon --------------------------------
// Familiarity bump + knows_word edge in one place. The "lex:<id>" belief is
// the per-mind knows_word store (beliefs serialize/hash already); the
// relation edge makes the same fact visible to the social graph.
void lex_boost(World& w, EntityId who, AgentMind& m, EntityId entry,
               std::int64_t gain, std::uint64_t tick) {
    Belief& b = m.belief(std::string("lex:") + std::to_string(entry));
    if (b.status == BeliefStatus::Unknown)
        b.status = BeliefStatus::Observed;
    b.confidence = std::min<std::int64_t>(1000, b.confidence + gain);
    b.last_tick = tick;
    w.relations.relate(who, "knows_word", entry);
}

std::int64_t lex_fam(const AgentMind& m, EntityId entry) {
    const Belief* b = m.find_belief(std::string("lex:") + std::to_string(entry));
    return b ? b->confidence : 0;
}

// Semantic drift target: the lexical_entry naming the NEIGHBORING atom of
// the same kind (sorted atom_key order, direction from the entry's drift
// seed). Returns kNoEntity when the word names no atom (whole-concept
// entries have no neighbors — they don't drift semantically).
EntityId lex_neighbor_entry(const World& w, EntityId entry) {
    const Entity* le = w.find(entry);
    if (!le || !le->alive || le->kind_name != SIMLAB_KIND("lexical_entry"))
        return kNoEntity;
    const PropId p_ckey = s_pid(w, "concept_key");
    const PropId p_lang = s_pid(w, "language_id");
    const PropId p_akey = s_pid(w, "atom_key");
    const PropId p_akind = s_pid(w, "atom_kind");
    const Value* ck = p_ckey != kNoProp ? w.prop_of(*le, p_ckey) : nullptr;
    if (!ck || ck->type != ValType::Text) return kNoEntity;
    const std::int64_t elang = p_lang != kNoProp
        ? w.prop_int(*le, p_lang, 0) : 0;
    // which atom does this word name, and of what kind?
    std::string akey; std::int64_t akind = -1;
    for (const auto& ep : w.entities) {
        const Entity& a = *ep;
        if (!a.alive || a.kind_name != SIMLAB_KIND("semantic_atom")) continue;
        const Value* av = p_akey != kNoProp ? w.prop_of(a, p_akey) : nullptr;
        if (av && av->type == ValType::Text && av->s == ck->s) {
            akey = av->s;
            const Value* kv = p_akind != kNoProp
                ? w.prop_of(a, p_akind) : nullptr;
            akind = kv && kv->type == ValType::Integer ? kv->i : -1;
            break;
        }
    }
    if (akey.empty() || akind < 0) return kNoEntity;
    // sibling atoms of the same kind, sorted by atom_key
    std::vector<std::string> sibs;
    for (const auto& ep : w.entities) {
        const Entity& a = *ep;
        if (!a.alive || a.kind_name != SIMLAB_KIND("semantic_atom")) continue;
        const Value* kv = p_akind != kNoProp
            ? w.prop_of(a, p_akind) : nullptr;
        const Value* av = p_akey != kNoProp
            ? w.prop_of(a, p_akey) : nullptr;
        if (kv && kv->type == ValType::Integer && kv->i == akind &&
            av && av->type == ValType::Text)
            sibs.push_back(av->s);
    }
    if (sibs.size() < 2) return kNoEntity;
    std::sort(sibs.begin(), sibs.end());
    sibs.erase(std::unique(sibs.begin(), sibs.end()), sibs.end());
    const auto it = std::find(sibs.begin(), sibs.end(), akey);
    if (it == sibs.end() || sibs.size() < 2) return kNoEntity;
    // direction from the entry's own identity — deterministic per word
    const std::size_t pos = static_cast<std::size_t>(it - sibs.begin());
    const bool fwd = (static_cast<std::uint64_t>(entry) * 2654435761ull >> 8) & 1;
    const std::size_t npos = fwd ? (pos + 1) % sibs.size()
                                 : (pos + sibs.size() - 1) % sibs.size();
    const std::string& nkey = sibs[npos];
    // the neighbor word in the SAME language
    EntityId best = kNoEntity;
    for (const auto& ep : w.entities) {
        const Entity& e = *ep;
        if (!e.alive || e.kind_name != SIMLAB_KIND("lexical_entry")) continue;
        const Value* ck2 = p_ckey != kNoProp ? w.prop_of(e, p_ckey) : nullptr;
        const Value* lv  = p_lang != kNoProp ? w.prop_of(e, p_lang) : nullptr;
        if (!ck2 || ck2->type != ValType::Text || ck2->s != nkey ||
            !lv || lv->type != ValType::Integer || lv->i != elang)
            continue;
        if (best == kNoEntity || e.id < best) best = e.id;
    }
    return best;
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
            // Phase 12: speech-community barrier — two speakers of different
            // language_id understand each other at a quarter rate until
            // bilingual familiarity bridges the gap.
            std::int64_t lang_a = 0, lang_b = 0;
            {
                const PropId pl = s_pid(w, "language_id");
                const Entity* ae = w.find(actor);
                const Entity* be = w.find(learner_id);
                if (pl != kNoProp) {
                    if (ae) lang_a = w.prop_int(*ae, pl, 0);
                    if (be) lang_b = w.prop_int(*be, pl, 0);
                }
            }
            bool barrier = lang_a > 0 && lang_b > 0 && lang_a != lang_b;
            if (barrier && diplo_free_trade(w, lang_a, lang_b))
                barrier = false;   // FREE_TRADE treaty: tongues interoperate
            // trust -> how many verified claims transfer (0 trust: 1, 500: 3, 1000: 5)
            std::int64_t k = std::clamp<std::int64_t>(
                (trust + 250) / 250 * tom / 1000, 0, 8);
            if (barrier) k = std::max<std::int64_t>(1, k / 4);
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
            // Phase 12 lexicon flow: the speaker's words cross the air.
            // Same-language exposure teaches fast; a foreign tongue is
            // picked up slowly — ostensive contact is what makes a mind
            // bilingual (familiarity >= bilingual_familiarity on entries
            // of the OTHER language_id).
            {
                const PropId pl = s_pid(w, "language_id");
                const PropId pbf = s_pid(w, "bilingual_familiarity");
                const std::int64_t bf = pbf != kNoProp
                    ? w.prop_int(*w.find(learner_id), pbf, 500) : 500;
                std::size_t ne = 0;
                for (const auto& [bk, bv] : teacher.beliefs) {
                    if (ne >= 24) break;
                    if (bk.rfind("lex:", 0) != 0) continue;
                    EntityId eid = kNoEntity;
                    try { eid = static_cast<EntityId>(
                        std::stoull(bk.substr(4))); } catch (...) { continue; }
                    const Entity* le = w.find(eid);
                    if (!le || !le->alive) continue;
                    const std::int64_t lel = pl != kNoProp
                        ? w.prop_of(*le, pl) && w.prop_of(*le, pl)->type == ValType::Integer
                          ? w.prop_of(*le, pl)->i : 0 : 0;
                    const bool same_lang = lel == lang_b;
                    std::int64_t gain = same_lang ? 250 : 60;
                    // Phase 12.5: a proverb/epic (oral_genre on the entry)
                    // is chanted, metered, memorable — it lands 3x harder.
                    {
                        const PropId pg = s_pid(w, "oral_genre");
                        const Value* gv = pg != kNoProp ? w.prop_of(*le, pg)
                                                        : nullptr;
                        if (gv && gv->type == ValType::Text && !gv->s.empty())
                            gain = std::min<std::int64_t>(1000, gain * 3);
                    }
                    // oral semantic drift: with no script to anchor the word,
                    // a rare deterministic slip lands the familiarity on the
                    // neighboring atom instead (mythogenesis, ~5%).
                    EntityId tgt = eid;
                    const std::uint64_t oh =
                        static_cast<std::uint64_t>(eid) ^
                        (static_cast<std::uint64_t>(ev.tick) << 17) ^
                        (static_cast<std::uint64_t>(actor) << 33);
                    if ((oh % 1000) < 50) {
                        const EntityId nb = lex_neighbor_entry(w, eid);
                        if (nb != kNoEntity) tgt = nb;
                    }
                    lex_boost(w, learner_id, learner, tgt, gain, ev.tick);
                    (void)bf;
                    ++ne;
                }
            }
        }
    } else if (ev.action == "read" && targets.size() == 1) {
        const EntityId src = targets.front();
        const PropId ins = s_pid(w, "inscription");
        const Entity* se = w.find(src);
        // Phase 12: if the surface carries a carved Inscription entity, its
        // language_id gates comprehension — same speech community reads
        // freely, a bilingual scholar decodes, anyone else sees glyphs.
        EntityId insc_id = kNoEntity;
        const Entity* insc_e = nullptr;
        std::int64_t insc_lang = 0;
        {
            for (const Relation& rel : w.relations_of(src, "inscribed_on")) {
                if (rel.a != src) continue;
                const Entity* ie = w.find(rel.b);
                if (ie && ie->alive && ie->kind_name == SIMLAB_KIND("inscription")) {
                    insc_id = rel.b;
                    insc_e = ie;
                    const PropId pl = s_pid(w, "language_id");
                    if (pl != kNoProp) {
                        const Value* lv = w.prop_of(*ie, pl);
                        if (lv && lv->type == ValType::Integer) insc_lang = lv->i;
                    }
                    break;
                }
            }
        }
        if (ins != kNoProp && se) {
            const Value* iv = w.prop_of(*se, ins);
            bool comprehensible = true;
            if (insc_id != kNoEntity && insc_lang > 0) {
                const PropId pl = s_pid(w, "language_id");
                const PropId pbf = s_pid(w, "bilingual_familiarity");
                const std::int64_t rl = pl != kNoProp
                    ? w.prop_int(*se, pl, 0) : 0;   // placeholder, fixed below
                (void)rl;
                const Entity* re = w.find(actor);
                const std::int64_t reader_lang = pl != kNoProp && re
                    ? w.prop_int(*re, pl, 0) : 0;
                if (reader_lang == insc_lang) {
                    comprehensible = true;
                } else {
                    // bilingual check: familiarity >= threshold on ANY entry
                    // of the inscription's language
                    const std::int64_t bf = pbf != kNoProp && re
                        ? w.prop_int(*re, pbf, 500) : 500;
                    AgentMind& rm = w.mind(actor);
                    comprehensible = false;
                    for (const auto& [bk, bv] : rm.beliefs) {
                        if (bk.rfind("lex:", 0) != 0 || bv.confidence < bf)
                            continue;
                        EntityId eid = kNoEntity;
                        try { eid = static_cast<EntityId>(
                            std::stoull(bk.substr(4))); } catch (...) { continue; }
                        const Entity* le = w.find(eid);
                        if (!le) continue;
                        const Value* lv = pl != kNoProp
                            ? w.prop_of(*le, pl) : nullptr;
                        if (lv && lv->type == ValType::Integer &&
                            lv->i == insc_lang) { comprehensible = true; break; }
                    }
                }
                if (!comprehensible) {
                    // exposure still teaches the SHAPE of the foreign words:
                    // small familiarity gain on that language's entries.
                    AgentMind& rm = w.mind(actor);
                    std::size_t ne = 0;
                    for (const auto& ep : w.entities) {
                        if (ne >= 8) break;
                        const Entity& ee = *ep;
                        if (!ee.alive || ee.kind_name != SIMLAB_KIND("lexical_entry")) continue;
                        const Value* lv = pl != kNoProp
                            ? w.prop_of(ee, pl) : nullptr;
                        if (!lv || lv->type != ValType::Integer ||
                            lv->i != insc_lang) continue;
                        lex_boost(w, actor, rm, ee.id, 40, ev.tick);
                        ++ne;
                    }
                }
            }
            // Phase 12 base core: CausalClause payload — ordered atom words
            // per language. A readable clause grants comprehension even when
            // the legacy language_id gate failed; an unreadable second clause
            // on a comprehensible carving is a Rosetta stone: its words map
            // onto meaning the reader already has, so they transfer fast.
            const PropId p_clause = s_pid(w, "clause_atoms");
            const PropId p_eros   = s_pid(w, "erosion_level");
            const PropId p_ssd    = s_pid(w, "semantic_shift_seed");
            if (insc_e && p_clause != kNoProp) {
                const Value* cv = w.prop_of(*insc_e, p_clause);
                if (cv && cv->type == ValType::Text && !cv->s.empty()) {
                    struct Clause { std::int64_t lang;
                                    std::vector<EntityId> ids; };
                    std::vector<Clause> clauses;
                    std::istringstream css(cv->s);
                    std::string seg;
                    while (std::getline(css, seg, ';')) {
                        const auto colon = seg.find(':');
                        if (colon == std::string::npos) continue;
                        Clause cl;
                        try { cl.lang = std::stoll(seg.substr(0, colon)); }
                        catch (...) { continue; }
                        std::istringstream is(seg.substr(colon + 1));
                        std::string num;
                        while (std::getline(is, num, ',')) {
                            try { cl.ids.push_back(
                                static_cast<EntityId>(std::stoull(num))); }
                            catch (...) {}
                        }
                        if (!cl.ids.empty()) clauses.push_back(cl);
                    }
                    if (!clauses.empty()) {
                        const Entity* re = w.find(actor);
                        const PropId pl2 = s_pid(w, "language_id");
                        const PropId pbf2 = s_pid(w, "bilingual_familiarity");
                        const std::int64_t rlang = pl2 != kNoProp && re
                            ? w.prop_int(*re, pl2, 0) : 0;
                        const std::int64_t bf = pbf2 != kNoProp && re
                            ? w.prop_int(*re, pbf2, 500) : 500;
                        AgentMind& rm = w.mind(actor);
                        const std::int64_t er = p_eros != kNoProp
                            ? w.prop_int(*insc_e, p_eros, 0) : 0;
                        // pass 1: which clauses can this reader parse?
                        std::vector<bool> rdv(clauses.size(), false);
                        bool clause_read = false;
                        for (std::size_t ci = 0; ci < clauses.size(); ++ci) {
                            const Clause& cl = clauses[ci];
                            bool rd = cl.lang > 0 && rlang == cl.lang;
                            if (!rd) {
                                for (EntityId id : cl.ids) {
                                    if (lex_fam(rm, id) >= bf) {
                                        rd = true; break;
                                    }
                                }
                            }
                            rdv[ci] = rd;
                            if (rd) clause_read = true;
                        }
                        // pass 2: mapped words of READABLE clauses land
                        // (with semantic drift on weathered text); unreadable
                        // clauses on a comprehensible carving are Rosetta.
                        for (std::size_t ci = 0; ci < clauses.size(); ++ci) {
                            const Clause& cl = clauses[ci];
                            const bool rd = rdv[ci];
                            if (rd) {
                                clause_read = true;
                                for (EntityId id : cl.ids) {
                                    // semantic drift: weathered text (>400)
                                    // can be misread onto the neighboring
                                    // atom — the seed of myth and tabu.
                                    EntityId tgt = id;
                                    if (er > 400) {
                                        const Entity* te = w.find(id);
                                        std::uint64_t salt = 0;
                                        if (te && p_ssd != kNoProp) {
                                            const Value* sv =
                                                w.prop_of(*te, p_ssd);
                                            if (sv && sv->type ==
                                                ValType::Integer)
                                                salt = static_cast<
                                                    std::uint64_t>(sv->i);
                                        }
                                        const std::uint64_t h =
                                            static_cast<std::uint64_t>(id) ^
                                            salt ^
                                            (static_cast<std::uint64_t>(
                                                ev.tick) << 20) ^
                                            (static_cast<std::uint64_t>(
                                                actor) << 40);
                                        if ((h % 1000) < 200) {
                                            const EntityId nb =
                                                lex_neighbor_entry(w, id);
                                            if (nb != kNoEntity) tgt = nb;
                                        }
                                    }
                                    // mapped words land nearly native;
                                    // drifted ones only faintly.
                                    const std::int64_t gain =
                                        (tgt != id) ? 140 : 300;
                                    lex_boost(w, actor, rm, tgt, gain,
                                              ev.tick);
                                }
                            } else if (clause_read || comprehensible) {
                                // Rosetta: an unreadable clause on a carving
                                // the reader CAN read — the words are
                                // anchored to known meaning, learn fast.
                                for (EntityId id : cl.ids)
                                    lex_boost(w, actor, rm, id, 300,
                                              ev.tick);
                            }
                        }
                        if (clause_read) comprehensible = true;
                    }
                }
            }
            if (iv && iv->type == ValType::Text && !iv->s.empty() &&
                comprehensible) {
                AgentMind& learner = w.mind(actor);
                std::istringstream lines(iv->s);
                std::string line;
                std::size_t n = 0;
                std::size_t nq = 0;
                while (std::getline(lines, line) && n < 8) {
                    // Phase 19 quantum lines: M|tier|conf, P|domain|conf,
                    // Q|domain|param|conf — transmitted as claims at half
                    // the carved confidence (proof must be re-earned).
                    if (line.rfind("M|", 0) == 0 ||
                        line.rfind("P|", 0) == 0 ||
                        line.rfind("Q|", 0) == 0) {
                        if (++nq > 24) continue;
                        std::vector<std::string> q;
                        std::string qc;
                        std::istringstream qls(line.substr(2));
                        while (std::getline(qls, qc, '|')) q.push_back(qc);
                        if (q.size() < 2) continue;
                        std::string key;
                        if (line[0] == 'M')      key = "math:" + q[0];
                        else if (line[0] == 'P') key = "phenomenon:" + q[0];
                        else {
                            // Q|domain|param|conf -> [domain,param,conf]
                            if (q.size() < 3) continue;
                            key = "qlaw:" + q[0] + ":p" + q[1];
                            const std::int64_t qc2 = std::atoll(q[2].c_str());
                            Belief& qb = learner.belief(key);
                            if (qb.status == BeliefStatus::Unknown)
                                qb.status = BeliefStatus::Observed;
                            qb.confidence = std::max<std::int64_t>(qb.confidence,
                                std::min<std::int64_t>(500, qc2 / 2));
                            qb.last_tick = ev.tick;
                            continue;
                        }
                        const std::int64_t qc2 = std::atoll(q.back().c_str());
                        Belief& qb = learner.belief(key);
                        if (qb.status == BeliefStatus::Unknown)
                            qb.status = BeliefStatus::Observed;
                        qb.confidence = std::max<std::int64_t>(qb.confidence,
                            std::min<std::int64_t>(500, qc2 / 2));
                        qb.last_tick = ev.tick;
                        continue;
                    }
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
            // Phase 12.3: a comprehensible Law Inscription teaches its
            // bound taboo directly — newcomers inherit the norm without
            // ever witnessing the crime it condemns.
            if (insc_e && comprehensible) {
                const PropId p_la = s_pid(w, "law_action");
                const PropId p_lv = s_pid(w, "law_aversion");
                if (p_la != kNoProp && p_lv != kNoProp) {
                    const Value* lav = w.prop_of(*insc_e, p_la);
                    const Value* lv2 = w.prop_of(*insc_e, p_lv);
                    if (lav && lav->type == ValType::Text &&
                        !lav->s.empty() &&
                        lv2 && lv2->type == ValType::Integer) {
                        Belief& nb = w.mind(actor).belief(
                            "norm_aversion:" + lav->s);
                        nb.confidence = std::max<std::int64_t>(
                            nb.confidence,
                            std::min<std::int64_t>(1000, lv2->i));
                        nb.status = BeliefStatus::Observed;
                        nb.last_tick = ev.tick;
                    }
                }
            }
            // ---- InscribedArtifact: engraved neural engrams -----------------
            // Top-charge CL-1 transitions carved by an ancestor replay through
            // the reader's own world model at `comprehension` strength:
            // same tongue -> full dose; dead language -> 25%, growing with
            // hidden size (>=80 scholar) and repeated study cycles.
            if (insc_e && comprehensible) {
                const PropId p_art = s_pid(w, "artifact_engrams");
                const Value* av = p_art != kNoProp
                    ? w.prop_of(*insc_e, p_art) : nullptr;
                if (av && av->type == ValType::Text && !av->s.empty()) {
                    std::int64_t comp = 1000;
                    std::int64_t rlang = 0, ilang = insc_lang;
                    {
                        const PropId pl3 = s_pid(w, "language_id");
                        const Entity* re = w.find(actor);
                        if (pl3 != kNoProp && re)
                            rlang = w.prop_int(*re, pl3, 0);
                    }
                    const std::int64_t kin = tongue_kinship(rlang, ilang);
                    if (kin <= 600) {
                        // dead/foreign tongue: quarter dose, scholars and
                        // repeat readers decipher more (study:<id> counts)
                        std::int64_t studies = 0;
                        const Belief* sb = w.mind(actor).find_belief(
                            "study:" + std::to_string(insc_id));
                        if (sb) studies = sb->confidence / 100;
                        comp = 250;
                        if (w.brains_) {
                            const auto bit = w.brains_->slots.find(actor);
                            if (bit != w.brains_->slots.end() &&
                                bit->second.net.neuron_count >= 400)
                                comp += 200;
                        }
                        comp = std::min<std::int64_t>(750,
                            comp + std::min<std::int64_t>(300, studies * 100));
                    }
                    // count this reading for next time's decipherment
                    {
                        Belief& sb = w.mind(actor).belief(
                            "study:" + std::to_string(insc_id));
                        sb.confidence = std::min<std::int64_t>(1000,
                            sb.confidence + 100);
                        sb.last_tick = ev.tick;
                    }
                    const std::vector<EngramPayload> eps =
                        engram_decode(av->s);
                    AgentMind& rm = w.mind(actor);
                    const MindParams rmp = mind_params(w, *w.find(actor));
                    for (std::size_t ei = 0; ei < eps.size(); ++ei) {
                        const EngramPayload& ep = eps[ei];
                        brain_assimilate(w, actor, ep.verb,
                            static_cast<std::int64_t>(ep.charge),
                            static_cast<std::int64_t>(ep.sigma),
                            static_cast<std::uint64_t>(insc_id) ^
                                (static_cast<std::uint64_t>(ei) << 40),
                            ep.wh.data(), ep.w1h.data(), ep.y.data(), comp);
                        // episodic echo: the focused entity enters memory
                        // at comprehension-scaled charge
                        if (ep.focus != 0 && w.find(
                                static_cast<EntityId>(ep.focus))) {
                            AgentMind::MemorySnapshot snap =
                                snapshot_now(w,
                                    static_cast<EntityId>(ep.focus),
                                    ev.tick);
                            const std::int64_t chg = std::min<std::int64_t>(
                                1000, static_cast<std::int64_t>(ep.charge)
                                * comp / 1000);
                            upsert_memory_g(w, rm,
                                static_cast<EntityId>(ep.focus),
                                std::move(snap), chg, rmp,
                                sight_radius_of(w, *w.find(actor)));
                        }
                        // synthesized recipe -> a teachable claim so the
                        // formula spreads mouth-to-ear like any discovery
                        if (ep.synth != 0) {
                            for (const World::SynthRecord& sr :
                                    w.synthesized_) {
                                if (sr.synth_id != ep.synth) continue;
                                DiscoveryRecord d;
                                d.key = "synth:" + std::to_string(ep.synth);
                                d.claim = "synthesis " + sr.verb + " " +
                                    std::to_string(sr.parent_a) + "+" +
                                    std::to_string(sr.parent_b);
                                d.status = DiscoveryStatus::Supported;
                                d.evidence_for = 60;
                                d.supporting_laws = {"synthesis"};
                                d.last_tick = ev.tick;
                                teach_record(rm, d, ev.tick);
                                break;
                            }
                        }
                    }
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
    // CL-1: dead agents release their brain slots (arena free-list reuse).
    if (brains_) brain_gc(*this, *brains_);
}


// ================= Phase 12.2: inductive economy ==============================
// Everything below is SUBJUCTIVE microeconomics: value emerges from the
// viewer's own needs, its perceived-abundance beliefs, its speech
// community's monetary consensus, and material physics. No price table,
// no currency enum, no profession list — a "merchant" is just a mind whose
// aptitudes make trade score well.

namespace {

PropId econ_pid(const World& w, const char* n) {
    static thread_local LitPidCache c_;
    return c_.get(w.props, n);
}

// Material-default lookup (instance props already fell back via prop_of;
// here we read the MaterialDef directly for entities that may not exist).
std::int64_t mat_prop(const World& w, MaterialId mat, const char* name,
                      std::int64_t fallback) {
    if (mat == kNoMaterial) return fallback;
    const MaterialDef* md = w.materials.find(mat);
    if (!md) return fallback;
    const PropId p = econ_pid(w, name);
    if (p == kNoProp) return fallback;
    const auto it = md->props.find(p);
    if (it == md->props.end()) return fallback;
    const Value& v = it->second;
    if (v.type == ValType::Integer || v.type == ValType::Fixed) return v.i;
    if (v.type == ValType::Boolean) return v.b ? 1 : 0;
    return fallback;
}

std::int64_t belief_conf(const AgentMind* m, const std::string& key,
                         std::int64_t fallback) {
    if (!m) return fallback;
    const Belief* b = m->find_belief(key);
    return b ? b->confidence : fallback;
}

}  // namespace

std::int64_t econ_abundance(const AgentMind* m, MaterialId mat) {
    return belief_conf(m, "pa:" + std::to_string(mat), 500);
}

std::int64_t econ_consensus(const World& w, EntityId viewer, MaterialId mat) {
    const PropId p_lang = econ_pid(w, "language_id");
    const PropId p_lname = econ_pid(w, "language_name");
    const PropId p_mc = econ_pid(w, "monetary_consensus");
    if (p_lang == kNoProp || p_lname == kNoProp || p_mc == kNoProp)
        return 0;
    const Entity* ve = w.find(viewer);
    if (!ve) return 0;
    const std::int64_t lang = w.prop_int(*ve, p_lang, 0);
    if (lang <= 0) return 0;
    // find this community's registry entity and read its consensus table
    for (const auto& ep : w.entities) {
        const Entity& e = *ep;
        if (!e.alive || e.kind_name != SIMLAB_KIND("language")) continue;
        if (w.prop_int(e, p_lname, 0) != lang) continue;
        const Value* tv = w.prop_of(e, p_mc);
        if (!tv || tv->type != ValType::Text) return 0;
        // "mid:fixed;mid:fixed..." — fixed = permille * 64
        const std::string want = std::to_string(mat) + ":";
        std::size_t pos = 0;
        while (pos <= tv->s.size()) {
            const std::size_t semi = tv->s.find(';', pos);
            const std::string ent = tv->s.substr(
                pos, semi == std::string::npos ? std::string::npos
                                               : semi - pos);
            if (ent.compare(0, want.size(), want) == 0) {
                const std::int64_t fx = std::stoll(ent.substr(want.size()));
                return std::min<std::int64_t>(1000, fx / 64);
            }
            if (semi == std::string::npos) break;
            pos = semi + 1;
        }
        return 0;
    }
    return 0;
}

std::int64_t econ_use_value(const World& w, EntityId viewer, MaterialId mat) {
    const Entity* ve = w.find(viewer);
    if (!ve) return 0;
    const AgentMind* m = w.find_mind(viewer);
    const std::int64_t pa = econ_abundance(m, mat);
    std::int64_t use = 0;
    // marginal use: own needs vs what the material relieves
    const std::int64_t hunger = w.prop_int(*ve, econ_pid(w, "hunger"), 0);
    const std::int64_t thirst = w.prop_int(*ve, econ_pid(w, "thirst"), 0);
    const std::int64_t nut = mat_prop(w, mat, "nutrition", 0);
    const std::int64_t hyd = mat_prop(w, mat, "hydration", 0);
    if (nut > 0) use += hunger * nut / 500;
    if (hyd > 0) use += thirst * hyd / 500;
    // medicine: wounded/sick actor values dressings & antipyretics
    const std::int64_t integ = w.prop_int(*ve, econ_pid(w, "integrity"), 1000);
    if (integ < 800) {
        const std::int64_t pot = std::max({
            mat_prop(w, mat, "antiseptic", 0),
            mat_prop(w, mat, "antipyretic", 0),
            mat_prop(w, mat, "absorbency", 0)});
        use += (800 - integ) * pot / 1000;
    }
    // storage value: durable goods are worth hoarding when supply looks
    // thin — scaled by the agent's planning horizon (far-sighted minds
    // stockpile; myopic ones live hand to mouth)
    const std::int64_t spoil = mat_prop(w, mat, "spoilage_rate", 0);
    const std::int64_t horizon = m ? mind_params(w, *ve).plan_horizon : 2;
    if (spoil < 1000) {
        const std::int64_t thin = std::max<std::int64_t>(0, 1000 - pa);
        use += horizon * 167 * (1000 - spoil) / 1000 * thin / 1000 * 3;
    }
    return std::min<std::int64_t>(use, 4000);
}

std::int64_t econ_exchange_value(const World& w, EntityId viewer,
                                 MaterialId mat) {
    const AgentMind* m = w.find_mind(viewer);
    const std::int64_t pa = econ_abundance(m, mat);
    const std::int64_t mc = econ_consensus(w, viewer, mat);
    const std::int64_t port = mat_prop(w, mat, "portability_score", 300);
    const std::int64_t spoil = mat_prop(w, mat, "spoilage_rate", 0);
    const std::int64_t scar = mat_prop(w, mat, "intrinsic_scarcity", 0);
    // exch permille = port*(1000-spoil)*(1000+scarce)*(1000+mc)
    //                 / ((1000+pa) * 1e6); 0..~4000
    const __int128 n = static_cast<__int128>(port) * (1000 - spoil) *
                       (1000 + scar) * (1000 + mc);
    const __int128 d = static_cast<__int128>(1000 + pa) * 1000000;
    const std::int64_t v = static_cast<std::int64_t>(n / d);
    return std::min<std::int64_t>(v, 6000);
}

std::int64_t econ_total_value(const World& w, EntityId viewer,
                              MaterialId mat) {
    return econ_exchange_value(w, viewer, mat) +
           econ_use_value(w, viewer, mat);
}

std::int64_t econ_required_qty(const World& w, EntityId viewer,
                               MaterialId give, std::int64_t give_qty,
                               MaterialId get) {
    const std::int64_t vg = econ_total_value(w, viewer, give);
    const std::int64_t vt = econ_total_value(w, viewer, get);
    if (vt <= 0 || vg <= 0) return -1;
    return give_qty * vg / vt;      // floor; caller applies margin
}

EntityId econ_held_stack(const World& w, EntityId agent, MaterialId mat,
                         std::int64_t need) {
    const PropId p_poss = econ_pid(w, "possessed_by");
    const PropId p_qty = econ_pid(w, "quantity");
    for (const auto& ep : w.entities) {
        const Entity& e = *ep;
        if (!e.alive || e.material != mat) continue;
        if (p_poss == kNoProp) continue;
        if (w.prop_int(e, p_poss, 0) != static_cast<std::int64_t>(agent))
            continue;
        std::int64_t q = 1;
        if (p_qty != kNoProp) {
            const Value* v = w.prop_of(e, p_qty);
            if (v && v->type == ValType::Integer && v->i > 0) q = v->i;
        }
        if (q >= need) return e.id;
    }
    return kNoEntity;
}

bool econ_is_store_of_value(const World& w, MaterialId mat) {
    if (mat == kNoMaterial) return false;
    if (mat_prop(w, mat, "portability_score", 0) < 500) return false;
    if (mat_prop(w, mat, "spoilage_rate", 0) > 400) return false;
    if (mat_prop(w, mat, "nutrition", 0) > 0 ||
        mat_prop(w, mat, "hydration", 0) > 0) return false;
    return true;
}

// Consensus table codec: "mid:fixed;mid:fixed" sorted by mid, fixed =
// permille*64. The stored fraction is the accumulator — bumps below never
// round, so dust-sized trades still count (Euclid-style remainder).
std::string econ_consensus_render(const std::map<std::uint32_t,
                                               std::int64_t>& tab) {
    std::string out;
    for (const auto& [mid, fx] : tab) {
        if (!out.empty()) out.push_back(';');
        out += std::to_string(mid) + ":" + std::to_string(fx);
    }
    return out;
}

std::map<std::uint32_t, std::int64_t>
econ_consensus_parse(const std::string& s) {
    std::map<std::uint32_t, std::int64_t> tab;
    std::size_t pos = 0;
    while (pos <= s.size()) {
        const std::size_t semi = s.find(';', pos);
        const std::string ent = s.substr(pos, semi == std::string::npos
            ? std::string::npos : semi - pos);
        const std::size_t colon = ent.find(':');
        if (colon != std::string::npos && colon > 0) {
            try {
                tab[static_cast<std::uint32_t>(
                    std::stoul(ent.substr(0, colon)))] =
                    std::stoll(ent.substr(colon + 1));
            } catch (...) { /* malformed entries are skipped */ }
        }
        if (semi == std::string::npos) break;
        pos = semi + 1;
    }
    return tab;
}

std::string econ_consensus_bump_text(const std::string& cur,
                                     MaterialId mat, std::int64_t qty) {
    auto tab = econ_consensus_parse(cur);
    // +8 permille per unit received as a store-of-value candidate;
    // fixed = permille*64, clamp at consensus 1000
    std::int64_t& fx = tab[mat];
    fx = std::min<std::int64_t>(64000, fx + qty * 512);
    return econ_consensus_render(tab);
}

// ---- ownership beliefs -------------------------------------------------------

// Who currently holds `item` per World Truth (0 = loose).
std::int64_t econ_holder(const World& w, EntityId item) {
    const Entity* e = w.find(item);
    if (!e) return 0;
    return w.prop_int(*e, econ_pid(w, "possessed_by"), 0);
}

void econ_ownership_beliefs(World& w, EntityId item, std::int64_t holder,
                            const Vec2& where, std::uint64_t tick) {
    // holder's own conviction is absolute; witnesses hold a weaker claim
    const std::string self_key = "owns:" + std::to_string(item);
    if (holder != 0 && w.find_mind(static_cast<EntityId>(holder))) {
        AgentMind& hm = w.mind(static_cast<EntityId>(holder));
        Belief& b = hm.belief(self_key);
        b.status = BeliefStatus::Verified;
        b.confidence = 1000; b.evidence_for += 500;
        b.self_evidence += 500; b.last_tick = tick;
        // previous owner's claim lapses
        // (callers handle the demotion via the `before` holder)
    }
    // witnesses: every OTHER minded agent in sight of the event location
    for (auto& [wid, wm] : w.minds_) {
        if (wid == static_cast<EntityId>(holder)) continue;
        const Entity* we = w.find(wid);
        if (!we || !we->alive) continue;
        const PropId sight = econ_pid(w, "sight_range");
        const std::int64_t range = sight != kNoProp
            ? w.prop_int(*we, sight, 0) : 0;
        if (range <= 0) continue;
        if (dist2(we->pos, where) > range * range) continue;
        // "<holder> owns <item>" — confidence scales with theory_of_mind
        const std::string key = self_key + ":" + std::to_string(holder);
        Belief& b = wm.belief(key);
        b.status = BeliefStatus::Observed;
        std::int64_t tom = 1000;
        if (const Entity* ae = w.find(wid))
            tom = mind_params(w, *ae).theory_of_mind;
        const std::int64_t conf = 300 + tom * 400 / 1000;   // 300..700
        b.confidence = std::max(b.confidence, conf);
        b.evidence_for += conf / 4; b.last_tick = tick;
    }
}

// negative engram toward `wrongdoer` for every witness in range — theft is
// a social shock, encoded through the same Ebbinghaus channel as violence.
void econ_witness_shame(World& w, EntityId wrongdoer, const Vec2& where,
                        std::uint64_t tick, std::int64_t charge) {
    for (auto& [wid, wm] : w.minds_) {
        if (wid == wrongdoer) continue;
        const Entity* we = w.find(wid);
        if (!we || !we->alive) continue;
        const PropId sight = econ_pid(w, "sight_range");
        const std::int64_t range = sight != kNoProp
            ? w.prop_int(*we, sight, 0) : 0;
        if (range <= 0 || dist2(we->pos, where) > range * range) continue;
        AgentMind::MemorySnapshot snap = snapshot_now(w, wrongdoer, tick);
        const MindParams mp = mind_params(w, *we);
        upsert_memory_g(w, wm, wrongdoer, snap, charge, mp, range);
    }
}

// post-commit economic consequences (called from World::interact after
// social_from_event). Reads the committed event; writes MINDS and relation
// ties only — every prop change was already proposed by predict().
void econ_from_event(World& w, EntityId actor,
                     const std::vector<EntityId>& targets,
                     const CausalEvent& ev) {
    if (actor == kNoEntity) return;
    const PropId p_poss = econ_pid(w, "possessed_by");
    const PropId p_theft = econ_pid(w, "theft_flag");
    const std::string& act = ev.action;
    if (act != "take" && act != "steal" && act != "trade" &&
        act != "repay" && act != "share")
        return;
    // items whose possession changed hands in this event
    std::set<EntityId> touched;
    for (const StateChange& ch : ev.changes)
        if (ch.prop == p_poss && ch.after.type == ValType::Integer &&
            ch.after.i != 0)
            touched.insert(ch.entity);
    for (EntityId s : ev.spawned) {
        const Entity* se = w.find(s);
        if (se && p_poss != kNoProp &&
            w.prop_int(*se, p_poss, 0) != 0)
            touched.insert(s);
    }
    const Entity* ae = w.find(actor);
    const Vec2 where = ae ? ae->pos : Vec2{0, 0};
    for (EntityId item : touched) {
        const std::int64_t holder = econ_holder(w, item);
        if (holder == 0) continue;
        econ_ownership_beliefs(w, item, holder, where, ev.tick);
        // previous owner's conviction lapses
        for (const StateChange& ch : ev.changes) {
            if (ch.entity != item || ch.prop != p_poss) continue;
            const std::int64_t prev = ch.before.type == ValType::Integer
                ? ch.before.i : 0;
            if (prev != 0 && prev != holder && w.find_mind(
                    static_cast<EntityId>(prev))) {
                AgentMind& om = w.mind(static_cast<EntityId>(prev));
                om.belief("owns:" + std::to_string(item))
                    .confidence = 0;
            }
        }
    }
    // witnessed theft: shame engrams for every onlooker (silent grabs —
    // theft_flag still armed — only expose the holder later via econ_drift)
    if (act == "steal") {
        bool caught = false;
        for (EntityId item : touched) {
            const Entity* ie = w.find(item);
            if (ie && p_theft != kNoProp &&
                w.prop_int(*ie, p_theft, 0) == 0) { caught = true; break; }
        }
        if (caught)
            econ_witness_shame(w, actor, where, ev.tick, -600);
    }
    // settling a debt earns gratitude toward the payer
    if (act == "repay" && targets.size() == 1) {
        const EntityId creditor = targets.front();
        if (creditor != kNoEntity && ae) {
            const MindParams mp = mind_params(w, *ae);
            if (const Entity* ce = w.find(creditor))
                gratitude_engram(w, creditor, actor, ev.tick,
                                 mind_params(w, *ce));
            (void)mp;
        }
    }
}

// comparative-advantage aptitude: craft verbs ride motor_precision,
// harvesting rides plan_horizon, medicine/lore ride abstraction_gain,
// and holding store-of-value goods feeds a merchant's nose.
std::int64_t econ_aptitude_bonus(const World& w, EntityId npc,
                                 const AgentMind& mind,
                                 const InteractionContext& c) {
    (void)mind;
    const Entity* me = w.find(npc);
    if (!me || !me->alive) return 0;
    const MindParams mp = mind_params(w, *me);
    const std::string& a = c.parameters.action;
    std::int64_t bonus = 0;
    auto craft = [&](const char* v) {
        return a == v;
    };
    if (craft("assemble") || craft("process") || craft("combine") ||
        craft("engrave") || craft("strike"))
        bonus += (mp.motor_precision - 1000) * 500 / 1000;
    if (craft("dig") || craft("chop"))
        bonus += (mp.plan_horizon - 2) * 200;
    if (craft("treat") || craft("read"))
        bonus += (mp.abstraction_gain - 1000) * 500 / 1000;
    if (a == "trade") {
        // merchant nose: holding portable durable stores biases toward
        // more dealing — the hoard itself is the aptitude signal
        const PropId p_poss = econ_pid(w, "possessed_by");
        if (p_poss != kNoProp) {
            for (const auto& ep : w.entities) {
                const Entity& e = *ep;
                if (!e.alive || e.material == kNoMaterial) continue;
                if (w.prop_int(e, p_poss, 0) !=
                    static_cast<std::int64_t>(npc)) continue;
                if (econ_is_store_of_value(w, e.material)) {
                    bonus += 400; break;
                }
            }
        }
    }
    bonus += arch_utility_bonus(w, npc, a);
    return std::clamp<std::int64_t>(bonus, -800, 1200);
}


// ---- Phase 12.3: political & legal base -------------------------------------
// Communal ledgers (norm_table / ostracism_table) are text props on the
// `language` registry entity — "key:fixed;" with fixed = permille*64, the
// same encoding monetary_consensus uses. Personal politics (defer_to,
// grievance) are beliefs; legitimacy/pool are entity props mutated only
// through causal changes or the ledgered political_drift event.

PropId pol_pid(const World& w, const char* name) {
    static thread_local LitPidCache c_;
    return c_.get(w.props, name);
}

std::int64_t pol_prop(const World& w, const Entity& e, const char* name,
                      std::int64_t fallback) {
    const PropId p = pol_pid(w, name);
    return p != kNoProp ? w.prop_int(e, p, fallback) : fallback;
}

EntityId pol_community_of(const World& w, EntityId agent) {
    const PropId p_lang = pol_pid(w, "language_id");
    const PropId p_lname = pol_pid(w, "language_name");
    if (p_lang == kNoProp || p_lname == kNoProp) return kNoEntity;
    const Entity* ae = w.find(agent);
    if (!ae) return kNoEntity;
    const std::int64_t lang = w.prop_int(*ae, p_lang, 0);
    if (lang <= 0) return kNoEntity;
    for (const EntityId kid_ : w.entities_of_kind(SIMLAB_KIND("language"))) {
        const Entity& e = *w.find(kid_);
        if (e.alive && e.kind_name == SIMLAB_KIND("language") &&
            w.prop_int(e, p_lname, 0) == lang)
            return e.id;
    }
    return kNoEntity;
}

std::int64_t pol_legitimacy(const World& w, EntityId agent) {
    const Entity* e = w.find(agent);
    if (!e) return 500;
    return std::clamp<std::int64_t>(
        pol_prop(w, *e, "social_legitimacy", 500), 0, 2000);
}

// coercive_power is never stored — it is recomputed from what a witness
// could see: bodily condition, weapons in hand, and the size of the
// armed following (defer beliefs already held by others).
std::int64_t pol_coercive(const World& w, EntityId agent) {
    const Entity* e = w.find(agent);
    if (!e || !e->alive) return 0;
    std::int64_t score = pol_prop(w, *e, "integrity", 500) * 3 / 4;
    // wielded hardware: hardest tool in hand adds deterrence
    const PropId p_hard = pol_pid(w, "hardness");
    for (const Relation& rel : w.relations_of(agent, "wielding")) {
        if (rel.a != agent) continue;
        const Entity* te = w.find(rel.b);
        if (te && te->alive && p_hard != kNoProp)
            score += w.prop_int(*te, p_hard, 0) / 4;
    }
    // armed following: each deferent adds muscle to the show of force
    score += pol_defer_count(w, agent) * 120;
    return std::clamp<std::int64_t>(score, 0, 2000);
}

std::int64_t pol_grievance(const AgentMind* m) {
    if (!m) return 0;
    const Belief* b = m->find_belief("grievance");
    return b ? std::clamp<std::int64_t>(b->confidence, 0, 1000) : 0;
}

std::int64_t pol_pool(const World& w, EntityId agent) {
    const Entity* e = w.find(agent);
    if (!e) return 0;
    return std::max<std::int64_t>(0,
        pol_prop(w, *e, "public_pool", 0));
}

// "key:fixed;" text ledger helpers — fixed = permille * 64.
static std::map<std::string, std::int64_t>
pol_table_parse(const std::string& s) {
    std::map<std::string, std::int64_t> tab;
    std::size_t pos = 0;
    while (pos <= s.size()) {
        const std::size_t semi = s.find(';', pos);
        const std::string ent = s.substr(pos, semi == std::string::npos
            ? std::string::npos : semi - pos);
        const std::size_t colon = ent.find(':');
        if (colon != std::string::npos && colon > 0) {
            try {
                tab[ent.substr(0, colon)] =
                    std::stoll(ent.substr(colon + 1));
            } catch (...) {}
        }
        if (semi == std::string::npos) break;
        pos = semi + 1;
    }
    return tab;
}
static std::string pol_table_render(
    const std::map<std::string, std::int64_t>& tab) {
    std::string out;
    for (const auto& [k, v] : tab) {
        if (!out.empty()) out.push_back(';');
        out += k + ":" + std::to_string(v);
    }
    return out;
}
// exported for SocialLaws predict paths (proper causal changes)
std::string pol_table_bump_text(const std::string& cur,
                                const std::string& key,
                                std::int64_t delta_permille) {
    auto tab = pol_table_parse(cur);
    std::int64_t& fx = tab[key];
    fx = std::clamp<std::int64_t>(
        fx + delta_permille * 64, 0, 64000);
    return pol_table_render(tab);
}
std::int64_t pol_table_get(const std::string& cur,
                           const std::string& key) {
    const auto tab = pol_table_parse(cur);
    const auto it = tab.find(key);
    return it == tab.end() ? 0 : std::min<std::int64_t>(1000, it->second / 64);
}

std::int64_t pol_norm_aversion(const World& w, EntityId viewer,
                               const std::string& action) {
    std::int64_t v = 0;
    const EntityId comm = pol_community_of(w, viewer);
    if (comm != kNoEntity) {
        const Entity* ce = w.find(comm);
        const PropId p_nt = pol_pid(w, "norm_table");
        if (ce && p_nt != kNoProp) {
            const Value* tv = w.prop_of(*ce, p_nt);
            if (tv && tv->type == ValType::Text)
                v = pol_table_get(tv->s, action);
        }
    }
    // private conviction (set by witnessed harm or reading a law stone)
    // can only reinforce the communal reading, never contradict it here
    if (const AgentMind* m = w.find_mind(viewer)) {
        const Belief* b = m->find_belief("norm_aversion:" + action);
        if (b) v = std::max<std::int64_t>(v, b->confidence);
    }
    return std::clamp<std::int64_t>(v, 0, 1000);
}

std::int64_t pol_ostracism(const World& w, EntityId viewer,
                           EntityId agent) {
    const EntityId comm = pol_community_of(w, viewer);
    if (comm == kNoEntity) return 0;
    const Entity* ce = w.find(comm);
    const PropId p_ot = pol_pid(w, "ostracism_table");
    if (!ce || p_ot == kNoProp) return 0;
    const Value* tv = w.prop_of(*ce, p_ot);
    if (!tv || tv->type != ValType::Text) return 0;
    return pol_table_get(tv->s, std::to_string(
        static_cast<std::uint64_t>(agent)));
}

bool pol_dominant_norm(const World& w, EntityId viewer,
                       std::string* action, std::int64_t* aversion) {
    const EntityId comm = pol_community_of(w, viewer);
    if (comm == kNoEntity) return false;
    const Entity* ce = w.find(comm);
    const PropId p_nt = pol_pid(w, "norm_table");
    if (!ce || p_nt == kNoProp) return false;
    const Value* tv = w.prop_of(*ce, p_nt);
    if (!tv || tv->type != ValType::Text) return false;
    const auto tab = pol_table_parse(tv->s);
    std::string best; std::int64_t bestv = 600;   // taboo threshold
    for (const auto& [k, fx] : tab) {
        const std::int64_t v = fx / 64;
        if (v > bestv || (v == bestv && k < best)) {
            bestv = v; best = k;
        }
    }
    if (best.empty()) return false;
    if (action) *action = best;
    if (aversion) *aversion = std::min<std::int64_t>(1000, bestv);
    return true;
}

std::int64_t pol_defer_count(const World& w, EntityId leader) {
    std::int64_t n = 0;
    const std::string key = "defer_to:" +
        std::to_string(static_cast<std::uint64_t>(leader));
    for (const auto& [wid, m] : w.minds_) {
        (void)m;
        const AgentMind* mm = w.find_mind(wid);
        const Belief* b = mm ? mm->find_belief(key) : nullptr;
        if (b && b->confidence >= 400) ++n;
    }
    return n;
}

std::int64_t pol_defer_conf(const AgentMind& m, EntityId leader) {
    const Belief* b = m.find_belief("defer_to:" +
        std::to_string(static_cast<std::uint64_t>(leader)));
    return b ? b->confidence : 0;
}

// influence score: legitimacy carries the day for empathetic minds;
// the fearful weight coercion instead. empathy axis = mind_empathy_coupling
// (0..2000 heritable); fear_w = 1000 - legit_w keeps the split integer.
static void pol_update_defer_impl(World& w, AgentMind& mind,
                                  EntityId self) {
    const Entity* se = w.find(self);
    if (!se || !se->alive) return;
    const PropId p_lang = pol_pid(w, "language_id");
    const std::int64_t lang = p_lang != kNoProp
        ? w.prop_int(*se, p_lang, 0) : 0;
    // drop stale defer beliefs first — allegiance is recomputed wholesale
    std::vector<std::string> stale;
    for (const auto& [k, b] : mind.beliefs)
        if (k.rfind("defer_to:", 0) == 0) stale.push_back(k);
    for (const std::string& k : stale) mind.beliefs.erase(k);
    if (lang <= 0) return;                      // no speech community, no mandate
    const std::int64_t empathy = std::clamp<std::int64_t>(
        pol_prop(w, *se, "mind_empathy_coupling", 1000), 0, 2000);
    const std::int64_t legit_w = empathy / 2;   // 0..1000
    const std::int64_t fear_w = 1000 - legit_w;
    EntityId best = kNoEntity; std::int64_t best_score = 0;
    for (const auto& [cid, cm] : w.minds_) {
        (void)cm;
        if (cid == self) continue;
        const Entity* ce = w.find(cid);
        if (!ce || !ce->alive) continue;
        if (p_lang == kNoProp ||
            w.prop_int(*ce, p_lang, 0) != lang) continue;
        const std::int64_t leg = pol_legitimacy(w, cid);
        const std::int64_t coer = pol_coercive(w, cid);
        const std::int64_t score =
            (leg * legit_w + coer * fear_w) / 1000;
        if (score > best_score ||
            (score == best_score && score > 0 &&
             static_cast<std::uint64_t>(cid) <
             static_cast<std::uint64_t>(best))) {
            best_score = score; best = cid;
        }
    }
    if (best == kNoEntity || best_score <= 0) return;
    Belief& b = mind.belief("defer_to:" +
        std::to_string(static_cast<std::uint64_t>(best)));
    b.status = BeliefStatus::Observed;
    b.confidence = std::min<std::int64_t>(1000, best_score);
    b.evidence_for += 50;
    b.last_tick = w.clock_.tick();
}

void pol_update_defer(World& w, EntityId mind_id) {
    if (!w.find_mind(mind_id)) return;
    AgentMind& m = w.mind(mind_id);
    pol_update_defer_impl(w, m, mind_id);
}

// mandate = strictly the most deferents; ties break to the lowest id so
// the "speaker of the community" is always deterministic.
bool pol_holds_mandate(const World& w, EntityId agent) {
    const Entity* ae = w.find(agent);
    if (!ae) return false;
    const PropId p_lang = pol_pid(w, "language_id");
    const std::int64_t lang = p_lang != kNoProp
        ? w.prop_int(*ae, p_lang, 0) : 0;
    std::map<std::uint64_t, std::int64_t> counts;
    for (const auto& [wid, m] : w.minds_) {
        (void)m;
        const AgentMind* mm = w.find_mind(wid);
        if (!mm) continue;
        // the mandate is a community office: only same-language minds vote
        if (lang > 0) {
            const Entity* we = w.find(wid);
            if (!we || w.prop_int(*we, p_lang, 0) != lang) continue;
        }
        for (const auto& [k, b] : mm->beliefs) {
            if (k.rfind("defer_to:", 0) != 0 || b.confidence < 400)
                continue;
            try {
                counts[std::stoull(k.substr(9))]++;
            } catch (...) {}
        }
    }
    if (counts.empty()) return false;
    std::uint64_t top = 0; std::int64_t topn = -1; bool tie = false;
    for (const auto& [id, n] : counts) {
        if (n > topn) { topn = n; top = id; tie = false; }
        else if (n == topn) tie = true;
    }
    if (tie) {
        // lowest id among the tied leaders holds the mandate
        std::uint64_t low = ~0ull;
        for (const auto& [id, n] : counts)
            if (n == topn && id < low) low = id;
        top = low;
    }
    return top == static_cast<std::uint64_t>(agent) && topn >= 2;
}

// communal norm bump helper for post-commit consequences (witnessed harm
// that no single predict could foresee — e.g. integrity damage inside
// physics/impact laws). Writes the language entity's ledger directly, the
// same post-commit sanction adjust_tie already uses for relations.
static void pol_norm_bump_communal(World& w, EntityId agent,
                                   const std::string& action,
                                   std::int64_t delta_permille) {
    const EntityId comm = pol_community_of(w, agent);
    if (comm == kNoEntity) return;
    Entity* ce = w.find(comm);
    const PropId p_nt = pol_pid(w, "norm_table");
    if (!ce || p_nt == kNoProp) return;
    const Value* cur = w.prop_of(*ce, p_nt);
    const std::string before = cur && cur->type == ValType::Text
        ? cur->s : std::string();
    const std::string after = pol_table_bump_text(before, action,
                                                delta_permille);
    if (after != before) ce->props.set(p_nt, Value::text(after));
}

void political_from_event(World& w, EntityId actor,
                          const std::vector<EntityId>& targets,
                          const CausalEvent& ev) {
    if (actor == kNoEntity) return;
    const std::string& act = ev.action;

    // ---- generic witnessed harm -> communal norm aversion ---------------
    // Any causal event that damaged integrity on a bystander is a public
    // wrong: the community ledger hardens against that action key.
    {
        const PropId integ = pol_pid(w, "integrity");
        bool harm = false;
        if (integ != kNoProp) {
            for (const StateChange& ch : ev.changes) {
                if (ch.prop != integ || ch.entity == actor ||
                    !ch.had_before) continue;
                if (ch.after.type == ValType::Integer &&
                    ch.before.type == ValType::Integer &&
                    ch.after.i < ch.before.i) { harm = true; break; }
            }
        }
        if (harm && !act.empty())
            pol_norm_bump_communal(w, actor, act, 200);
    }

    // ---- witnessed theft -> personal norm conviction ----------------------
    // The communal ledger (predict-time change) records the taboo; each
    // onlooker also forms a PRIVATE conviction — that is what a law stone
    // later transmits to minds that never saw the crime.
    if (act == "steal") {
        const PropId p_rep = pol_pid(w, "trade_reputation");
        bool witnessed = false;
        if (p_rep != kNoProp) {
            for (const StateChange& ch : ev.changes)
                if (ch.entity == actor && ch.prop == p_rep &&
                    ch.after.type == ValType::Integer &&
                    ch.before.type == ValType::Integer &&
                    ch.after.i < ch.before.i) { witnessed = true; break; }
        }
        if (witnessed) {
            const Entity* ae = w.find(actor);
            const PropId sight = pol_pid(w, "sight_range");
            for (auto& [wid, wm] : w.minds_) {
                if (wid == actor) continue;
                const Entity* we = w.find(wid);
                if (!we || !we->alive || !ae) continue;
                const std::int64_t range = sight != kNoProp
                    ? w.prop_int(*we, sight, 0) : 0;
                if (range <= 0 ||
                    dist2(we->pos, ae->pos) > range * range) continue;
                Belief& b = wm.belief("norm_aversion:steal");
                b.confidence = std::min<std::int64_t>(1000,
                    b.confidence + 300);
                b.status = BeliefStatus::Observed;
                b.last_tick = ev.tick;
            }
        }
    }

    // ---- allegiance refresh after social contact --------------------------
    // defer_to is a belief, so it may be rewritten post-commit freely.
    if (act == "communicate" || act == "trade" || act == "share" ||
        act == "repay" || act == "adjudicate" || act == "tribute") {
        pol_update_defer(w, actor);
        for (EntityId t : targets) pol_update_defer(w, t);
    }

    // ---- tribute: grievance bookkeeping is mental, so it lives in beliefs --
    if (act == "tribute" && targets.size() == 1) {
        AgentMind& am = w.mind(actor);
        const std::int64_t leg = pol_legitimacy(w, targets.front());
        Belief& g = am.belief("grievance");
        if (leg < 300) {
            // tribute to a discredited leader reads as extortion
            g.confidence = std::min<std::int64_t>(
                1000, g.confidence + 30);
        } else {
            g.confidence = std::max<std::int64_t>(
                0, g.confidence - 10);
        }
        g.last_tick = ev.tick;
    }

    // ---- relief received: shared food under the mandate cools grievance ---
    if (act == "share") {
        bool relief = false;
        const PropId p_pool = pol_pid(w, "public_pool");
        for (const StateChange& ch : ev.changes)
            if (ch.prop == p_pool && ch.after.type == ValType::Integer &&
                ch.before.type == ValType::Integer &&
                ch.after.i < ch.before.i) { relief = true; break; }
        if (relief) {
            for (EntityId t : targets) {
                if (!w.find_mind(t)) continue;
                Belief& g = w.mind(t).belief("grievance");
                g.confidence = 0;
                g.last_tick = ev.tick;
            }
        }
    }

    // ---- unjust adjudication: victims remember the miscarriage ------------
    // predict_adjudicate embeds "victim:<id>" markers in its notes (which
    // land in ev.evidence) when a mandate-holder pardons a proven wrong.
    if (act == "adjudicate" &&
        ev.evidence.find("unjust pardon") != std::string::npos) {
        std::size_t pos = 0;
        while ((pos = ev.evidence.find("victim:", pos)) !=
               std::string::npos) {
            pos += 7;
            std::size_t e = pos;
            while (e < ev.evidence.size() &&
                   (std::isdigit(ev.evidence[e]) || ev.evidence[e] == ';' ||
                    ev.evidence[e] == ' ')) ++e;
            // ids are plain digits; parse until non-digit
            std::size_t d = pos;
            while (d < ev.evidence.size() &&
                   std::isdigit(ev.evidence[d])) ++d;
            try {
                const EntityId vid = static_cast<EntityId>(
                    std::stoull(ev.evidence.substr(pos, d - pos)));
                if (w.find_mind(vid)) {
                    Belief& g = w.mind(vid).belief("grievance");
                    g.confidence = std::min<std::int64_t>(
                        1000, g.confidence + 250);
                    g.last_tick = ev.tick;
                }
            } catch (...) {}
            pos = e > pos ? e : pos + 1;
        }
    }
}



// ---- Phase 12.4: architecture & settlement ----------------------------------
// Enclosures are entities; containment is SPATIAL (footprint_radius),
// never a kind flag. Function is INFERRED from contents — nothing here
// names a building type; the labels are readouts of material facts.

PropId arch_pid(const World& w, const char* name) {
    static thread_local LitPidCache c_;
    return c_.get(w.props, name);
}
std::int64_t arch_prop(const World& w, const Entity& e, const char* name,
                       std::int64_t fallback) {
    const PropId p = arch_pid(w, name);
    return p != kNoProp ? w.prop_int(e, p, fallback) : fallback;
}

// innermost (smallest footprint) alive enclosure covering pos
EntityId arch_enclosure_at(const World& w, Vec2 pos) {
    const PropId p_rad = arch_pid(w, "footprint_radius");
    if (p_rad == kNoProp) return kNoEntity;
    EntityId best = kNoEntity; std::int64_t best_r = INT64_MAX;
    for (const EntityId kid_ : w.entities_of_kind(SIMLAB_KIND("enclosure"))) {
        const Entity& e = *w.find(kid_);
        if (!e.alive || e.kind_name != SIMLAB_KIND("enclosure")) continue;
        const std::int64_t r = w.prop_int(e, p_rad, 0);
        if (r <= 0) continue;
        const std::int64_t dx = e.pos.x - pos.x;
        const std::int64_t dy = e.pos.y - pos.y;
        if (dx * dx + dy * dy > r * r) continue;
        if (r < best_r) { best_r = r; best = e.id; }
    }
    return best;
}

// owner = lowest-id mind holding a Verified-grade owns:<enc> belief
EntityId arch_owner_of(const World& w, EntityId enc) {
    const std::string key = "owns:" +
        std::to_string(static_cast<std::uint64_t>(enc));
    EntityId best = kNoEntity;
    for (const auto& [mid, m] : w.minds_) {
        (void)m;
        const Belief* b = m.find_belief(key);
        if (b && b->confidence >= 600 &&
            (best == kNoEntity || mid < best)) best = mid;
    }
    return best;
}

// maze-ness: stored base + live nesting depth + portal count + inner rooms
std::int64_t arch_complexity(const World& w, EntityId enc) {
    const Entity* e = w.find(enc);
    if (!e || !e->alive || e->kind_name != SIMLAB_KIND("enclosure")) return 0;
    const PropId p_rad = arch_pid(w, "footprint_radius");
    const std::int64_t r = arch_prop(w, *e, "footprint_radius", 0);
    if (r <= 0) return 0;
    std::int64_t depth = 0, portals = 0, inner = 0;
    for (const auto& ep : w.entities) {
        const Entity& o = *ep;
        if (!o.alive || o.id == enc) continue;
        const std::int64_t ox = o.pos.x - e->pos.x;
        const std::int64_t oy = o.pos.y - e->pos.y;
        const std::int64_t d2 = ox * ox + oy * oy;
        if (o.kind_name == SIMLAB_KIND("portal") && d2 <= r * r) ++portals;
        else if (o.kind_name == SIMLAB_KIND("enclosure")) {
            const std::int64_t orad = arch_prop(w, o,
                "footprint_radius", 0);
            if (orad <= 0) continue;
            if (d2 <= r * r) ++inner;                    // room inside this one
            // does `o` enclose US? (we sit inside its footprint)
            const std::int64_t wx = e->pos.x - o.pos.x;
            const std::int64_t wy = e->pos.y - o.pos.y;
            if (wx * wx + wy * wy <= orad * orad) ++depth;
        }
    }
    const std::int64_t base = arch_prop(w, *e, "spatial_complexity", 0);
    return std::clamp<std::int64_t>(
        base + depth * 60 + portals * 50 + inner * 40, 0, 1000);
}

// emergent function: read off what the shell CONTAINS, never a label table
std::string arch_function(const World& w, EntityId enc) {
    const Entity* e = w.find(enc);
    if (!e || !e->alive || e->kind_name != SIMLAB_KIND("enclosure")) return "";
    const PropId p_rad = arch_pid(w, "footprint_radius");
    const std::int64_t r = arch_prop(w, *e, "footprint_radius", 0);
    if (r <= 0) return "open_ground";
    const PropId p_temp = arch_pid(w, "temperature");
    const PropId p_law = arch_pid(w, "law_action");
    const PropId p_bar = arch_pid(w, "access_barrier");
    const PropId p_integ = arch_pid(w, "integrity");
    const PropId p_nut = arch_pid(w, "nutrition");
    bool heat = false, law = false, rich_food = false, sacred = false;
    std::int64_t inner_enc = 0;
    const PropId p_skey = arch_pid(w, "sacred_key");
    const PropId p_sag = arch_pid(w, "sacred_agent");
    for (const auto& ep : w.entities) {
        const Entity& o = *ep;
        if (!o.alive || o.id == enc) continue;
        const std::int64_t ox = o.pos.x - e->pos.x;
        const std::int64_t oy = o.pos.y - e->pos.y;
        if (ox * ox + oy * oy > r * r) continue;
        if (o.kind_name == SIMLAB_KIND("enclosure")) { ++inner_enc; continue; }
        // a Sanctuary needs a CARVED sacred text inside the walls — the
        // abstract semantic_concept of a myth is belief, not a shrine.
        auto has_text = [&](PropId p) {
            const Value* v = p != kNoProp ? w.prop_of(o, p) : nullptr;
            return v && v->type == ValType::Text && !v->s.empty();
        };
        if (o.kind_name == SIMLAB_KIND("inscription") && (has_text(p_skey) || has_text(p_sag)))
            sacred = true;
        if (o.kind_name == SIMLAB_KIND("inscription") && p_law != kNoProp) {
            const Value* lv = w.prop_of(o, p_law);
            if (lv && lv->type == ValType::Text && !lv->s.empty())
                law = true;
        }
        if (p_temp != kNoProp &&
            w.prop_int(o, p_temp, 0) > 500) heat = true;
        if (o.material != kNoMaterial && p_nut != kNoProp &&
            w.prop_int(o, p_nut, 0) >= 400) rich_food = true;
    }
    const std::int64_t integ = arch_prop(w, *e, "integrity", 500);
    const std::int64_t bar = arch_prop(w, *e, "access_barrier", 0);
    const EntityId owner = arch_owner_of(w, enc);
    // mandate-holder frequenting the shell marks it as the council fire
    bool council = false;
    if (owner != kNoEntity) {
        const Entity* oe = w.find(owner);
        if (oe) {
            const std::int64_t ox = oe->pos.x - e->pos.x;
            const std::int64_t oy = oe->pos.y - e->pos.y;
            council = ox * ox + oy * oy <= r * r;
        }
    }
    if (integ >= 800 && bar >= 800 && inner_enc >= 1) return "fortress";
    if (sacred) return "sanctuary";
    if (heat) return "forge";
    if (law || council) return "court";
    if (bar >= 600 && (rich_food || bar >= 800)) return "vault";
    return "dwelling";
}

// utility pull toward functional zones — same slot as econ_aptitude_bonus
std::int64_t arch_utility_bonus(const World& w, EntityId npc,
                                const std::string& action) {
    const Entity* me = w.find(npc);
    if (!me || !me->alive) return 0;
    const EntityId enc = arch_enclosure_at(w, me->pos);
    if (enc == kNoEntity) return 0;
    const std::string fn = arch_function(w, enc);
    const MindParams mp = mind_params(w, *me);
    if (fn == "forge" &&
        (action == "combine" || action == "process" ||
         action == "carve" || action == "assemble" || action == "dig"))
        return (mp.motor_precision - 800) * 2 / 5 + 200;
    if (fn == "vault" &&
        (action == "trade" || action == "tribute"))
        return 300;
    if (fn == "court" && action == "adjudicate")
        return 400;
    if (fn == "sanctuary" &&
        (action == "perform_ritual" || action == "communicate" ||
         action == "consecrate")) {
        // the faithful feel the pull; doubters do not
        std::int64_t pull = 0;
        if (const AgentMind* am = w.find_mind(npc)) {
            for (const auto& [bk, bv] : am->beliefs)
                if (bk.rfind("sacred:", 0) == 0 && bv.confidence >= 400) {
                    pull = 300; break;
                }
        }
        return pull;
    }
    return 0;
}

void arch_from_event(World& w, EntityId actor,
                     const std::vector<EntityId>& targets,
                     const CausalEvent& ev) {
    if (actor == kNoEntity || ev.action.empty()) return;
    if (ev.action == "enclose") {
        // builder claims the shell — same ownership-belief machinery as goods
        for (EntityId nid : ev.spawned) {
            const Entity* ne = w.find(nid);
            if (ne && ne->alive && ne->kind_name == SIMLAB_KIND("enclosure"))
                econ_ownership_beliefs(w, nid, actor, ne->pos, ev.tick);
        }
    } else if (ev.action == "bypass_barrier" && !targets.empty()) {
        const EntityId enc = targets.front();
        const std::string enc_key = std::to_string(
            static_cast<std::uint64_t>(enc));
        const bool failed =
            ev.evidence.find("bypass repelled") != std::string::npos;
        if (!w.find_mind(actor)) return;
        if (failed) {
            // noise carries: every owner who can see the shell is alerted
            const std::string owns_key = "owns:" + enc_key;
            const Entity* ee = w.find(enc);
            const PropId p_sight = arch_pid(w, "sight_range");
            for (auto& [oid, om] : w.minds_) {
                (void)om;
                const Belief* ob = om.find_belief(owns_key);
                if (!ob || ob->confidence < 600) continue;
                const Entity* oe = w.find(oid);
                if (!oe || !oe->alive || !ee) continue;
                const std::int64_t sight = p_sight != kNoProp
                    ? w.prop_int(*oe, p_sight, 0) : 0;
                const std::int64_t dx = oe->pos.x - ee->pos.x;
                const std::int64_t dy = oe->pos.y - ee->pos.y;
                if (dx * dx + dy * dy <= sight * sight * 4) {
                    AgentMind& om2 = w.mind(oid);
                    Belief& al = om2.belief("alert:" + enc_key);
                    al.confidence = 800;
                    al.status = BeliefStatus::Observed;
                    al.last_tick = ev.tick;
                }
            }
        } else {
            // stealth success: quiet entry granted — consent, not force
            Belief& g = w.mind(actor).belief("entry_granted:" + enc_key);
            g.confidence = 900;
            g.status = BeliefStatus::Observed;
            g.last_tick = ev.tick;
        }
    }
}

// ---- Phase 12.5: myth, oral literature, ritual & diplomacy -------------------

namespace {

PropId my_pid(const World& w, const char* n) {
    static thread_local LitPidCache c_;
    return c_.get(w.props, n);
}

// same seed->pseudo-word codec World::export_lexicon_codex uses — display
// only, alphabet is seed-derived, never a stored vocabulary.
std::string my_render_seed(std::uint32_t seed, std::int64_t syllables,
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
            out.push_back(static_cast<char>('a' +
                static_cast<int>((h ^ world_seed) % 26)));
        }
        if (i + 1 < nsyl) out.push_back('-');
    }
    return out;
}

// language_id of an entity (0 = speechless)
std::int64_t my_lang(const World& w, const Entity& e) {
    const PropId p = my_pid(w, "language_id");
    return p != kNoProp ? w.prop_int(e, p, 0) : 0;
}

// the `language` registry entity for a language_id (same lookup
// pol_community_of performs)
EntityId my_lang_entity(const World& w, std::int64_t lang) {
    const PropId p_ln = my_pid(w, "language_name");
    for (const EntityId kid_ : w.entities_of_kind(SIMLAB_KIND("language"))) {
        const Entity& e = *w.find(kid_);
        if (e.alive && e.kind_name == SIMLAB_KIND("language") && p_ln != kNoProp &&
            w.prop_int(e, p_ln, 0) == lang)
            return e.id;
    }
    return kNoEntity;
}

}  // namespace

// ---- diplo_table codec (single definition; SocialLaws uses it too) ----------
// "peer:trust[:ft][:host];..." — trust is ALWAYS the stored number
// (-1000..+1000); `ft` (free trade) is set only by a treaty and lapses if
// trust falls below 0; `host` (hostility) follows trust <= -600 both ways.
std::vector<DiploEntry> diplo_parse(const std::string& cur) {
    std::vector<DiploEntry> out;
    std::size_t pos = 0;
    while (pos < cur.size()) {
        std::size_t semi = cur.find(';', pos);
        if (semi == std::string::npos) semi = cur.size();
        const std::string tok = cur.substr(pos, semi - pos);
        pos = semi + 1;
        if (tok.empty()) continue;
        std::vector<std::string> f;
        std::size_t a = 0;
        while (true) {
            const std::size_t c = tok.find(':', a);
            f.push_back(tok.substr(a, c == std::string::npos ? std::string::npos : c - a));
            if (c == std::string::npos) break;
            a = c + 1;
        }
        if (f.size() < 2) continue;
        DiploEntry d;
        try { d.peer = std::stoll(f[0]); d.trust = std::stoll(f[1]); }
        catch (...) { continue; }
        for (std::size_t k = 2; k < f.size(); ++k) {
            if (f[k] == "ft") d.free_trade = true;
            else if (f[k] == "host") d.hostile = true;
        }
        out.push_back(d);
    }
    return out;
}

std::string diplo_render(const std::vector<DiploEntry>& v) {
    std::string out;
    for (const DiploEntry& d : v) {
        out += std::to_string(d.peer) + ":" + std::to_string(d.trust);
        if (d.free_trade) out += ":ft";
        if (d.hostile) out += ":host";
        out += ";";
    }
    return out;
}

std::string diplo_bump(const std::string& cur, std::int64_t peer,
                       std::int64_t delta) {
    std::vector<DiploEntry> v = diplo_parse(cur);
    DiploEntry* d = nullptr;
    for (DiploEntry& x : v) if (x.peer == peer) d = &x;
    if (!d) { v.push_back(DiploEntry{}); d = &v.back(); d->peer = peer; }
    d->trust = std::clamp<std::int64_t>(d->trust + delta, -1000, 1000);
    d->hostile = d->trust <= -600;
    if (d->trust < 0) d->free_trade = false;     // a broken peace closes the road
    return diplo_render(v);
}

std::string diplo_mark_trade(const std::string& cur, std::int64_t peer) {
    std::vector<DiploEntry> v = diplo_parse(cur);
    DiploEntry* d = nullptr;
    for (DiploEntry& x : v) if (x.peer == peer) d = &x;
    if (!d) { v.push_back(DiploEntry{}); d = &v.back(); d->peer = peer; }
    d->free_trade = true;
    return diplo_render(v);
}

namespace {
// read one peer's entry from a table text
const DiploEntry* diplo_find(const std::vector<DiploEntry>& v, std::int64_t peer) {
    for (const DiploEntry& d : v) if (d.peer == peer) return &d;
    return nullptr;
}
}  // namespace

std::int64_t diplo_trust(const World& w, std::int64_t lang_a,
                         std::int64_t lang_b) {
    if (lang_a <= 0 || lang_b <= 0 || lang_a == lang_b) return 0;
    const PropId p_dt = my_pid(w, "diplo_table");
    const EntityId la = my_lang_entity(w, lang_a);
    if (la == kNoEntity || p_dt == kNoProp) return 0;
    const Entity* e = w.find(la);
    if (!e) return 0;
    const Value* v = w.prop_of(*e, p_dt);
    if (!v || v->type != ValType::Text) return 0;
    const auto ents = diplo_parse(v->s);
    const DiploEntry* d = diplo_find(ents, lang_b);
    return d ? d->trust : 0;
}

bool diplo_free_trade(const World& w, std::int64_t lang_a,
                      std::int64_t lang_b) {
    if (lang_a <= 0 || lang_b <= 0 || lang_a == lang_b) return false;
    const PropId p_dt = my_pid(w, "diplo_table");
    for (std::int64_t lg : {lang_a, lang_b}) {
        const EntityId le = my_lang_entity(w, lg);
        const Entity* e = le != kNoEntity ? w.find(le) : nullptr;
        if (!e || p_dt == kNoProp) continue;
        const Value* v = w.prop_of(*e, p_dt);
        if (v && v->type == ValType::Text) {
            const auto ents = diplo_parse(v->s);
            const DiploEntry* d = diplo_find(ents, lg == lang_a ? lang_b : lang_a);
            if (d && d->free_trade) return true;
        }
    }
    return false;
}

bool diplo_hostile(const World& w, std::int64_t lang_a,
                   std::int64_t lang_b) {
    if (lang_a <= 0 || lang_b <= 0 || lang_a == lang_b) return false;
    const PropId p_dt = my_pid(w, "diplo_table");
    for (std::int64_t lg : {lang_a, lang_b}) {
        const EntityId le = my_lang_entity(w, lg);
        const Entity* e = le != kNoEntity ? w.find(le) : nullptr;
        if (!e || p_dt == kNoProp) continue;
        const Value* v = w.prop_of(*e, p_dt);
        if (v && v->type == ValType::Text) {
            const auto ents = diplo_parse(v->s);
            const DiploEntry* d = diplo_find(ents, lg == lang_a ? lang_b : lang_a);
            if (d && d->hostile) return true;
        }
    }
    return false;
}

// meter of an oral tradition: permille scoring the entry's clause —
// adjacent rendered syllable-length matches and shared onsets across the
// entries naming the same concept in the same language.
std::int64_t oral_cadence_of(const World& w, EntityId entry) {
    const Entity* e = w.find(entry);
    if (!e || !e->alive || e->kind_name != SIMLAB_KIND("lexical_entry")) return 0;
    const PropId p_ckey = my_pid(w, "concept_key");
    const PropId p_lang = my_pid(w, "language_id");
    const PropId p_ps   = my_pid(w, "phoneme_seed");
    if (p_ckey == kNoProp || p_ps == kNoProp) return 0;
    const Value* ck = w.prop_of(*e, p_ckey);
    const Value* lv = p_lang != kNoProp ? w.prop_of(*e, p_lang) : nullptr;
    if (!ck || ck->type != ValType::Text) return 0;
    const std::int64_t lg = lv && lv->type == ValType::Integer ? lv->i : 0;
    // gather sibling entries of the same concept+language (<=6, sorted)
    std::vector<std::uint32_t> seeds;
    for (const auto& ep : w.entities) {
        const Entity& o = *ep;
        if (!o.alive || o.id == entry || o.kind_name != SIMLAB_KIND("lexical_entry"))
            continue;
        const Value* ok = w.prop_of(o, p_ckey);
        const Value* ol = p_lang != kNoProp ? w.prop_of(o, p_lang) : nullptr;
        if (!ok || ok->type != ValType::Text || ok->s != ck->s) continue;
        const std::int64_t olg = ol && ol->type == ValType::Integer
            ? ol->i : 0;
        if (olg != lg) continue;
        const Value* pv = w.prop_of(o, p_ps);
        if (pv && pv->type == ValType::Integer)
            seeds.push_back(static_cast<std::uint32_t>(pv->i));
        if (seeds.size() >= 5) break;
    }
    const Value* pv0 = w.prop_of(*e, p_ps);
    if (pv0 && pv0->type == ValType::Integer)
        seeds.insert(seeds.begin(),
                     static_cast<std::uint32_t>(pv0->i));
    if (seeds.size() < 2) return 0;
    // render each seed to its pseudo-word; score adjacent pairs:
    // +500 for equal rendered length, +500 for shared first letter —
    // cadence is the mean over adjacent pairs, permille.
    std::int64_t score = 0, pairs = 0;
    std::string prev;
    for (std::uint32_t sd : seeds) {
        const std::string cur = my_render_seed(sd, 2, w.world_seed);
        if (cur.empty()) continue;
        if (!prev.empty()) {
            ++pairs;
            if (cur.size() == prev.size()) score += 500;
            if (cur[0] == prev[0]) score += 500;
        }
        prev = cur;
    }
    if (pairs == 0) return 0;
    return std::clamp<std::int64_t>(score / pairs, 0, 1000);
}

void myth_from_event(World& w, EntityId actor,
                     const std::vector<EntityId>& targets,
                     const CausalEvent& ev) {
    (void)targets;
    if (actor == kNoEntity || ev.action.empty() || ev.failed) return;
    const PropId p_lang = my_pid(w, "language_id");
    const Entity* ae = w.find(actor);
    if (!ae || p_lang == kNoProp) return;
    const std::int64_t alang = w.prop_int(*ae, p_lang, 0);

    // speaking IS rehearsal: the words the speaker just recited (the same
    // <=8 confident `lex:` entries predict_communicate counted) are fresh
    // again — oral attrition in myth_drift measures time since last use.
    if (ev.action == "communicate") {
        if (AgentMind* sm = w.find_mind(actor) ? &w.mind(actor) : nullptr) {
            std::size_t n = 0;
            for (auto& [bk, bv] : sm->beliefs) {
                if (n >= 8) break;
                if (bk.rfind("lex:", 0) != 0 || bv.confidence < 500) continue;
                bv.last_tick = ev.tick;
                ++n;
            }
        }
        return;
    }

    // collective ritual: the faithful gather — grievance cools for every
    // same-tongue mind inside earshot; inside a Sanctuary (an enclosure whose
    // inferred function is sacred) the convener's pull doubles the relief.
    // Chanting rehearses every confident word the listeners hold.
    if (ev.action == "perform_ritual") {
        if (alang <= 0) return;
        const PropId p_cr = my_pid(w, "communicate_range");
        const std::int64_t range = p_cr != kNoProp
            ? w.prop_int(*ae, p_cr, 400) : 400;
        const std::int64_t relief =
            60 + arch_utility_bonus(w, actor, "perform_ritual") / 5;
        for (auto& [mid, mm] : w.minds_) {
            const Entity* me = w.find(mid);
            if (!me || !me->alive) continue;
            if (my_lang(w, *me) != alang) continue;
            if (dist2(me->pos, ae->pos) > range * range) continue;
            Belief& g = mm.belief("grievance");
            g.confidence = std::max<std::int64_t>(0, g.confidence - relief);
            g.last_tick = ev.tick;
            for (auto& [bk, bv] : mm.beliefs)
                if (bk.rfind("lex:", 0) == 0 && bv.confidence >= 500)
                    bv.last_tick = ev.tick;          // rehearsal refresh
        }
        return;
    }

    // hostility escalation: once a theft/parley leaves two communities'
    // ledgers at "hostile", EVERY mind of both tongues learns of the feud
    // (an ultimatum is common knowledge) — belief `hostile_tongue:<peer>`.
    if (ev.action == "steal" || ev.action == "diplomatic_parley") {
        if (alang <= 0) return;
        const PropId p_ln = my_pid(w, "language_name");
        if (p_ln == kNoProp) return;
        std::vector<std::int64_t> foes;
        for (const auto& ep : w.entities) {
            const Entity& le = *ep;
            if (!le.alive || le.kind_name != SIMLAB_KIND("language")) continue;
            const std::int64_t lg = w.prop_int(le, p_ln, 0);
            if (lg > 0 && lg != alang && diplo_hostile(w, alang, lg))
                foes.push_back(lg);
        }
        for (std::int64_t foe : foes)
            for (auto& [mid, mm] : w.minds_) {
                const Entity* me = w.find(mid);
                if (!me || !me->alive) continue;
                const std::int64_t ml = my_lang(w, *me);
                if (ml != alang && ml != foe) continue;
                Belief& hb = mm.belief("hostile_tongue:" +
                    std::to_string(ml == alang ? foe : alang));
                hb.confidence = 1000;
                hb.status = BeliefStatus::Observed;
                hb.last_tick = ev.tick;
            }
    }
}

// ---- Phase 12.6: arms race, mobilization & peace (post-event, minds only) -----

namespace {
std::int64_t war_lang(const World& w, const Entity& e) {
    const PropId p = SIMLAB_PID(w.props, "language_id");
    return p != kNoProp ? w.prop_int(e, p, 0) : 0;
}
void bump_belief(AgentMind& m, const std::string& key, std::int64_t d,
                 std::uint64_t tick) {
    Belief& b = m.belief(key);
    b.confidence = std::clamp<std::int64_t>(b.confidence + d, 0, 1000);
    if (b.status == BeliefStatus::Unknown) b.status = BeliefStatus::Observed;
    b.last_tick = tick;
}
}  // namespace

void war_from_event(World& w, EntityId actor, const std::vector<EntityId>& targets,
                    const CausalEvent& ev) {
    (void)actor; (void)targets;
    if (ev.failed) return;
    // (1) arms race: every wound the laws tagged "victim=<id> mech=<m>" teaches
    // the victim — and the kin of its tongue nearby — to look for a counter.
    const std::string& t = ev.evidence;
    std::size_t pos = 0;
    while ((pos = t.find("victim=", pos)) != std::string::npos) {
        pos += 7;
        std::size_t e = pos;
        while (e < t.size() && std::isdigit(static_cast<unsigned char>(t[e]))) ++e;
        if (e == pos) continue;
        EntityId vic = kNoEntity;
        try { vic = static_cast<EntityId>(std::stoull(t.substr(pos, e - pos))); } catch (...) { continue; }
        const std::size_t stop = std::min(t.find("victim=", e), t.find(';', e));
        std::vector<std::string> mechs;
        for (std::size_t mpos = t.find("mech=", e); mpos != std::string::npos && mpos < stop;
             mpos = t.find("mech=", mpos + 5)) {
            std::size_t me = mpos + 5;
            while (me < t.size() && (std::isalpha(static_cast<unsigned char>(t[me])) || t[me] == '_')) ++me;
            mechs.push_back(t.substr(mpos + 5, me - mpos - 5));
        }
        const Entity* ve = w.find(vic);
        if (!ve) continue;
        const std::int64_t vlang = war_lang(w, *ve);
        for (const std::string& mech : mechs) {
            const bool armorable = mech == "bleed" || mech == "projectile" ||
                                   mech == "blunt" || mech == "blast";
            for (auto& [mid, m] : w.minds_) {
                const Entity* me = w.find(mid);
                if (!me) continue;
                const bool self = mid == vic;
                const bool kin = !self && vlang > 0 && war_lang(w, *me) == vlang &&
                                 me->alive && dist2(me->pos, ve->pos) <= 600 * 600;
                if (!self && !kin) continue;
                const std::int64_t gain = self ? 300 : 150;
                bump_belief(m, "weapon_counter:" + mech, gain, ev.tick);
                if (armorable) bump_belief(m, "armor_tech_bias", gain * 5 / 6, ev.tick);
            }
        }
    }
    // (2) mobilization: a soldier's own mind learns which tongue it marches on
    const PropId wt_p = SIMLAB_PID(w.props, "war_target");
    if (wt_p != kNoProp)
        for (const StateChange& c : ev.changes)
            if (c.prop == wt_p && c.after.type == ValType::Integer) {
                AgentMind* m = w.find_mind(c.entity) ? &w.mind(c.entity) : nullptr;
                if (!m) continue;
                if (c.after.i > 0) bump_belief(*m, "war:" + std::to_string(c.after.i), 1000, ev.tick);
                else if (c.before.type == ValType::Integer && c.before.i > 0)
                    m->beliefs.erase("war:" + std::to_string(c.before.i));
            }
    // (3) peace: the treaty's enclosure changes hands in every mind that held it
    const std::size_t pp = t.find("PEACE enc=");
    if (pp != std::string::npos) {
        std::int64_t enc = 0, to = 0, la = 0, lb = 0;
        std::sscanf(t.c_str() + pp, "PEACE enc=%lld to=%lld la=%lld lb=%lld",
                    reinterpret_cast<long long*>(&enc), reinterpret_cast<long long*>(&to),
                    reinterpret_cast<long long*>(&la), reinterpret_cast<long long*>(&lb));
        for (auto& [mid, m] : w.minds_) {
            const Entity* me = w.find(mid);
            const std::int64_t ml = me ? war_lang(w, *me) : 0;
            if (ml == la || ml == lb) {
                m.beliefs.erase("hostile_tongue:" + std::to_string(ml == la ? lb : la));
                m.beliefs.erase("war:" + std::to_string(ml == la ? lb : la));
            }
            if (enc > 0 && ml == la) m.beliefs.erase("owns:" + std::to_string(enc));
        }
        if (enc > 0 && to > 0 && w.find_mind(static_cast<EntityId>(to))) {
            Belief& ob = w.mind(static_cast<EntityId>(to)).belief("owns:" + std::to_string(enc));
            ob.confidence = 1000; ob.status = BeliefStatus::Verified; ob.last_tick = ev.tick;
        }
    }
}

std::int64_t arms_race_bonus(const World& w, const AgentMind& m,
                             const InteractionContext& c) {
    auto bel = [&](const char* k) {
        const Belief* b = m.find_belief(k);
        return b ? b->confidence : 0;
    };
    const std::string& a = c.parameters.action;
    const PropId apr_p = SIMLAB_PID(w.props, "armor_penetration_resist");
    const PropId el_p = SIMLAB_PID(w.props, "elasticity");
    std::int64_t best_apr = 0, best_el = 0;
    for (EntityId t : c.targets)
        if (const Entity* te = w.find(t)) {
            if (apr_p != kNoProp) best_apr = std::max(best_apr, w.prop_int(*te, apr_p, 0));
            if (el_p != kNoProp) best_el = std::max(best_el, w.prop_int(*te, el_p, 0));
        }
    if (a == "don") return bel("armor_tech_bias") * best_apr / 2000;
    if (a == "assemble")
        return (best_apr > 0 ? bel("armor_tech_bias") / 3 : 0) +
               (best_el > 0 ? bel("weapon_counter:projectile") / 3 : 0);
    if (a == "treat") return bel("weapon_counter:toxin") / 2;
    if (a == "launch") return bel("weapon_counter:projectile") / 3;
    return 0;
}

// ================= Emergent culture, archaeology & tribal mythology =========
// Everything below is agent knowledge (AgentMind) or artifact props — the
// only World-Truth writes are the Inscription entity's own props, committed
// through the normal interact -> causal event path.

namespace {

std::string hex_encode(const std::int8_t* p, std::size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s; s.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t b = static_cast<std::uint8_t>(p[i]);
        s.push_back(d[b >> 4]); s.push_back(d[b & 15]);
    }
    return s;
}
int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
bool hex_decode(const std::string& s, std::size_t off, std::int8_t* out,
                std::size_t n) {
    if (s.size() < off + n * 2) return false;
    for (std::size_t i = 0; i < n; ++i) {
        const int hi = hex_val(s[off + i * 2]), lo = hex_val(s[off + i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<std::int8_t>((hi << 4) | lo);
    }
    return true;
}
// (parse_u64 comes from Types.hpp — simlab::parse_u64)

// doctrine slot identity for dedup/drift — two slots are "the same myth"
// when trigger + verb agree (valence/depth may differ).
bool doctrine_same(const AgentMind::DoctrineSlot& a,
                   const AgentMind::DoctrineSlot& b) {
    return a.trig_kind == b.trig_kind && a.trig_prop == b.trig_prop &&
           a.trig_thr == b.trig_thr && a.trig_id == b.trig_id &&
           a.trig_text == b.trig_text && a.verb == b.verb;
}
std::string doctrine_key(const AgentMind::DoctrineSlot& d) {
    std::ostringstream o;
    o << static_cast<int>(d.trig_kind) << ':' << d.trig_prop << ':'
      << d.trig_thr << ':' << d.trig_id << ':' << d.trig_text << ':' << d.verb;
    return o.str();
}
// insert-or-refresh: full slot table replaces the weakest |valence| entry
// when the newcomer is stronger; an existing identical myth refreshes
// depth/valence only when the incoming telling is deeper.
void doctrine_upsert(AgentMind& m, const AgentMind::DoctrineSlot& in) {
    for (auto& d : m.doctrines)
        if (doctrine_same(d, in)) {
            if (in.depth > d.depth) { d.depth = in.depth; d.seed = in.seed; }
            d.valence = in.valence;
            d.last_tick = in.last_tick;
            return;
        }
    if (m.doctrines.size() < AgentMind::kMaxDoctrines) {
        m.doctrines.push_back(in);
        return;
    }
    std::size_t weak = 0;
    for (std::size_t i = 1; i < m.doctrines.size(); ++i) {
        const std::int64_t ai = m.doctrines[i].valence < 0
            ? -m.doctrines[i].valence : m.doctrines[i].valence;
        const std::int64_t aw = m.doctrines[weak].valence < 0
            ? -m.doctrines[weak].valence : m.doctrines[weak].valence;
        if (ai < aw) weak = i;
    }
    const std::int64_t inw = in.valence < 0 ? -in.valence : in.valence;
    const std::int64_t ww = m.doctrines[weak].valence < 0
        ? -m.doctrines[weak].valence : m.doctrines[weak].valence;
    if (inw > ww) m.doctrines[weak] = in;
}

}  // namespace

std::string engram_encode(const EngramPayload& p) {
    std::ostringstream o;
    o << "E|" << p.verb << '|' << p.charge << '|' << p.sigma << '|'
      << p.focus << '|' << p.synth << '|'
      << hex_encode(p.wh.data(), p.wh.size()) << '|'
      << hex_encode(p.w1h.data(), p.w1h.size()) << '|'
      << hex_encode(p.y.data(), p.y.size());
    return o.str();
}

std::vector<EngramPayload> engram_decode(const std::string& blob) {
    std::vector<EngramPayload> out;
    std::istringstream ls(blob);
    std::string line;
    while (std::getline(ls, line) && out.size() < 4) {
        if (line.rfind("E|", 0) != 0) continue;
        std::vector<std::string> f;
        std::string cur;
        std::istringstream fs(line.substr(2));
        while (std::getline(fs, cur, '|')) f.push_back(cur);
        if (f.size() != 8) continue;
        EngramPayload p;
        p.verb = std::atoi(f[0].c_str());
        p.charge = simlab::parse_u64(f[1]);
        p.sigma = simlab::parse_u64(f[2]);
        p.focus = simlab::parse_u64(f[3]);
        p.synth = simlab::parse_u64(f[4]);
        if (p.verb < 0 || p.verb >= kActionVerbs) continue;
        if (!hex_decode(f[5], 0, p.wh.data(), p.wh.size()) ||
            !hex_decode(f[6], 0, p.w1h.data(), p.w1h.size()) ||
            !hex_decode(f[7], 0, p.y.data(), p.y.size()))
            continue;
        out.push_back(p);
    }
    return out;
}

std::int64_t tongue_kinship(std::int64_t lang_a, std::int64_t lang_b) {
    if (lang_a <= 0 || lang_b <= 0) return 0;
    const std::uint64_t x =
        static_cast<std::uint64_t>(lang_a) ^ static_cast<std::uint64_t>(lang_b);
    return std::max<std::int64_t>(
        0, 1000 - static_cast<std::int64_t>(__builtin_popcountll(x)) * 34);
}

bool doctrine_trigger_hits(const World& w, const Entity& e,
                           const AgentMind::DoctrineSlot& d) {
    switch (d.trig_kind) {
    case 0: {   // property threshold — entity prop, else material default
        if (d.trig_prop == kNoProp) return false;
        return w.prop_int(e, d.trig_prop, 0) >= d.trig_thr;
    }
    case 1:     // specific material
        return static_cast<std::uint64_t>(e.material) == d.trig_id;
    case 2: {   // synthesized recipe: entity's material must be that synth
        if (e.material == kNoMaterial) return false;
        for (const World::SynthRecord& r : w.synthesized_)
            if (r.synth_id == d.trig_id &&
                static_cast<std::uint64_t>(r.material) ==
                    static_cast<std::uint64_t>(e.material))
                return true;
        return false;
    }
    case 3:     // entity kind name (interned compare, runtime string OK)
        return e.kind_name == KindName(d.trig_text);
    }
    return false;
}

std::int64_t doctrine_bias_for(const World& w, const AgentMind& mind,
                               const Entity& actor, int verb,
                               const Entity* target) {
    if (mind.doctrines.empty() || !target) return 0;
    const PropId hp = SIMLAB_PID(w.props, "hunger");
    if (hp != kNoProp && w.prop_int(actor, hp, 0) > 900) return 0;  // starving
    std::int64_t bias = 0;
    for (const AgentMind::DoctrineSlot& d : mind.doctrines) {
        if (d.verb >= 0 && d.verb != verb) continue;
        if (!doctrine_trigger_hits(w, *target, d)) continue;
        // valence -1000..+1000 -> +-250 score; panic dampens the pull of
        // dogma (calm/social minds obey taboos, terrified ones don't).
        const std::int64_t panic = mind.neuro.panic;
        const std::int64_t calm = 1000 - std::min<std::int64_t>(panic, 1000);
        bias += d.valence * calm / 4000;
    }
    return bias;
}

void doctrine_from_event(World& w, EntityId npc, const CausalEvent& ev) {
    if (npc == kNoEntity || ev.failed || !w.find_mind(npc)) return;
    AgentMind* mp = nullptr;
    auto seed = [&](std::uint8_t kind, PropId prop, std::int64_t thr,
                    std::uint64_t id, const std::string& text, int verb,
                    std::int64_t valence) {
        AgentMind& m = w.mind(npc);
        if (!mp) mp = &m;
        // already carrying this myth? deepen it instead of duplicating
        for (auto& d : m.doctrines)
            if (d.trig_kind == kind && d.trig_prop == prop &&
                d.trig_thr == thr && d.trig_id == id &&
                d.trig_text == text && d.verb == verb) {
                d.valence = std::clamp<std::int64_t>(
                    d.valence + valence / 4, -1000, 1000);
                d.last_tick = ev.tick;
                return;
            }
        AgentMind::DoctrineSlot d;
        d.trig_kind = kind; d.trig_prop = prop; d.trig_thr = thr;
        d.trig_id = id; d.trig_text = text; d.verb = verb;
        d.valence = std::clamp<std::int64_t>(valence, -1000, 1000);
        d.depth = 0;
        d.seed = Rng::hash(ev.id ^ (static_cast<std::uint64_t>(npc) << 20) ^
                           static_cast<std::uint64_t>(valence));
        d.last_tick = ev.tick;
        doctrine_upsert(m, d);
    };

    // ---- catastrophe seeds --------------------------------------------------
    if (ev.action == "detonation") {
        // a blast teaches fire-aversion: anything remotely flammable is
        // taboo until proven otherwise (valence scales with deaths seen)
        const PropId p_int = SIMLAB_PID(w.props, "integrity");
        std::int64_t dead = 0;
        for (const StateChange& c : ev.changes)
            if (c.prop == p_int && c.after.type == ValType::Integer &&
                c.after.i == 0 && c.had_before &&
                c.before.type == ValType::Integer && c.before.i > 0)
                ++dead;
        const PropId p_fl = SIMLAB_PID(w.props, "flammability");
        if (p_fl != kNoProp)
            seed(0, p_fl, 500, 0, "", -1, dead > 0 ? -900 : -700);
        return;
    }
    // self-seeded survivals (drift_needs fabricates these events at the
    // moment a need crosses its catastrophe threshold)
    if (ev.action == "self_plague") {
        const PropId p_inf = SIMLAB_PID(w.props, "infection_level");
        if (p_inf != kNoProp) seed(0, p_inf, 400, 0, "", -1, -700);
        return;
    }
    if (ev.action == "self_famine") {
        const PropId p_nut = SIMLAB_PID(w.props, "nutrition");
        if (p_nut != kNoProp) seed(0, p_nut, 200, 0, "", 0, 400);
        return;
    }
    // miracle cure: a consume that scrubbed toxin_load (antidote draught)
    // — the material that saved them becomes reverent, especially if it
    // was a synthesis nobody else has tasted yet.
    if (ev.action == "consume") {
        const PropId p_tox = SIMLAB_PID(w.props, "toxin_load");
        bool cured = false;
        for (const StateChange& c : ev.changes)
            if (c.prop == p_tox && c.after.type == ValType::Integer &&
                c.before.type == ValType::Integer &&
                c.after.i < c.before.i) { cured = true; break; }
        if (!cured) return;
        // target = first non-actor participant entity carrying a material
        for (EntityId pid_ : ev.participants) {
            if (pid_ == npc) continue;
            const Entity* te = w.find(pid_);
            if (!te || !te->alive || te->material == kNoMaterial) continue;
            seed(1, kNoProp, 0, te->material, "", 0, 900);   // consume=0
            // and the recipe itself, when the cure was synthesized
            for (const World::SynthRecord& r : w.synthesized_)
                if (static_cast<std::uint64_t>(r.material) ==
                    static_cast<std::uint64_t>(te->material))
                    seed(2, kNoProp, 0, r.synth_id, "", 0, 900);
            return;
        }
        return;
    }
}

void doctrine_drift(std::int64_t n, World& w) {
    if (n <= 0) return;
    constexpr std::int64_t kPeriod = 150;      // oral cadence ~ myth period
    constexpr std::int64_t kEarshot = 96;     // "adjacent" in world units
    const std::uint64_t now = w.clock_.tick();
    const PropId p_lang = SIMLAB_PID(w.props, "language_id");
    const PropId p_sight = SIMLAB_PID(w.props, "sight_range");

    // accumulator-exact like decay_minds: fires land on absolute ticks so
    // dt=1 xN and dt=N produce identical tellings.
    struct Due { EntityId id; std::int64_t fires; std::int64_t rem; };
    std::vector<Due> due;
    for (auto& [eid, m] : w.minds_) {
        const Entity* le = w.find(eid);
        if (!le || !le->alive || m.doctrines.empty()) continue;
        m.doctrine_accum += n;
        const std::int64_t fires = m.doctrine_accum / kPeriod;
        m.doctrine_accum %= kPeriod;
        if (fires > 0) due.push_back({eid, fires, m.doctrine_accum});
    }
    if (due.empty()) return;

    const PropId p_hunger = SIMLAB_PID(w.props, "hunger");
    for (const Due& d : due) {
        AgentMind& sender = w.minds_[d.id];
        const Entity* se = w.find(d.id);
        if (!se || !se->alive || sender.doctrines.empty()) continue;
        const std::int64_t slang = p_lang != kNoProp
            ? w.prop_int(*se, p_lang, 0) : 0;
        if (slang <= 0) continue;
        if (sender.neuro.panic >= 200) continue;   // only calm mouths speak
        // strongest slot = max |valence| (first wins ties — vector order)
        std::size_t bi = 0;
        for (std::size_t i = 1; i < sender.doctrines.size(); ++i) {
            const std::int64_t ai = sender.doctrines[i].valence < 0
                ? -sender.doctrines[i].valence : sender.doctrines[i].valence;
            const std::int64_t aw = sender.doctrines[bi].valence < 0
                ? -sender.doctrines[bi].valence : sender.doctrines[bi].valence;
            if (ai > aw) bi = i;
        }
        const AgentMind::DoctrineSlot& src = sender.doctrines[bi];
        // nearest eligible listener: alive mind, kin tongue, calm, in earshot
        EntityId best = kNoEntity; std::int64_t bd2 = 0;
        const std::int64_t ear = p_sight != kNoProp
            ? std::max<std::int64_t>(kEarshot,
                  w.prop_int(*se, p_sight, 0)) : kEarshot;
        const std::int64_t ear2 = ear * ear;
        for (const auto& [eid, rm] : w.minds_) {
            if (eid == d.id) continue;
            const Entity* re = w.find(eid);
            if (!re || !re->alive || rm.neuro.panic >= 200) continue;
            const std::int64_t rlang = p_lang != kNoProp
                ? w.prop_int(*re, p_lang, 0) : 0;
            if (tongue_kinship(slang, rlang) <= 700) continue;
            const std::int64_t dx = re->pos.x - se->pos.x;
            const std::int64_t dy = re->pos.y - se->pos.y;
            const std::int64_t dd = dx * dx + dy * dy;
            if (dd > ear2) continue;
            if (best == kNoEntity || dd < bd2 ||
                (dd == bd2 && eid < best)) { best = eid; bd2 = dd; }
        }
        if (best == kNoEntity) continue;
        AgentMind& recv = w.minds_[best];
        AgentMind::DoctrineSlot cp = src;
        cp.depth = src.depth + 1;                    // one more retelling
        // telephone drift: +-5% valence perturbation, hash-seeded per
        // (sender, listener, slot, fire-index) — deterministic, content-free
        const std::uint64_t h = Rng::hash(
            src.seed ^ (static_cast<std::uint64_t>(d.id) << 24) ^
            (static_cast<std::uint64_t>(best) << 40) ^
            static_cast<std::uint64_t>(now) ^
            static_cast<std::uint64_t>(bi) * 0x9E3779B97F4A7C15ull);
        const std::int64_t jit = static_cast<std::int64_t>(h % 11) - 5;  // -5..5
        cp.valence = std::clamp<std::int64_t>(
            cp.valence * (100 + jit) / 100, -1000, 1000);
        cp.seed = h;
        cp.last_tick = now;
        // a full table only accepts a STRONGER myth (weakest-slot rule
        // inside doctrine_upsert already enforces that)
        doctrine_upsert(recv, cp);
        (void)p_hunger;
    }
}

std::string myth_record_line(const AgentMind::DoctrineSlot& d,
                             std::int64_t lang, const std::string& title,
                             const std::string& scripture) {
    auto san = [](std::string s) {
        for (char& c : s)
            if (c == '|' || c == '\n' || c == '\r' || c == '=') c = ' ';
        return s;
    };
    std::ostringstream o;
    o << "[MYTH]\n"
      << "id=doctrine:" << doctrine_key(d) << '\n'
      << "tribe=" << lang << '\n'
      << "generation=" << d.depth << '\n'
      << "focus=" << d.trig_kind << ':' << d.trig_prop << ':'
      << d.trig_thr << ':' << d.trig_id << ':' << d.trig_text << ':'
      << d.verb << '\n'
      << "valence=" << d.valence << '\n'
      << "title=" << san(title) << '\n'
      << "scripture=\"" << san(scripture) << "\"\n";
    return o.str();
}

}  // namespace simlab

