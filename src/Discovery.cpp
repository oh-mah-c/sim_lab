// Discovery.cpp — TODO §11 Discovery Engine (Phase 6, completed).
//
// Pipeline: detect_problems -> generate_hypotheses -> run_experiment.
// Candidates derive ONLY from agent knowledge (known_actions, observations,
// beliefs via plan_multi). Experiments commit through World::interact — the
// same causal pipeline as everything else — and every trial lands in the
// mind's DiscoveryRecord with its event ids as provenance.
//
// MethodKeys generalize by SIGNATURE (material quantization / observable-sign
// profile), never by entity id: experience on one marked rock transfers to
// every rock showing the same signs. Refuted records store the condition
// boundaries that refused them (validConditions) and unlock automatically
// when a new candidate's predicted conditions clear those boundaries.

#include "simlab/Discovery.hpp"
#include "simlab/World.hpp"
#include "simlab/Laws.hpp"
#include "simlab/Causal.hpp"
#include <algorithm>
#include <set>

namespace simlab {

namespace {

// Verbs the law models understand — the engine's action vocabulary. An NPC
// still needs each verb in known_actions before it may try it.
const std::vector<std::string>& action_verbs() {
    static const std::vector<std::string> kVerbs = {
        "dig", "chop", "strike", "impact", "carve", "assemble",
        "disassemble", "process", "heat", "wield", "unwield",
        "consume", "rest"};
    return kVerbs;
}

InteractionContext mk(EntityId actor, std::vector<EntityId> targets,
                      const std::string& action) {
    InteractionContext c;
    c.actor = actor; c.targets = std::move(targets);
    c.parameters.action = action; c.parameters.force = 100000; c.parameters.ticks = 1;
    return c;
}

std::int64_t need_value(const World& w, EntityId npc, PropId p) {
    const Entity* e = w.find(npc);
    if (!e || !e->alive || p == kNoProp) return 0;
    const Value* v = w.prop_of(*e, p);
    return v ? v->i : 0;
}

std::string law_name(const World& w, LawId id) {
    for (const auto& m : w.laws)
        if (m->id() == id) return m->name();
    return "?";
}

// First law that predicts `possible` — mirrors World::interact pass 1.
// When `conds` is non-null it receives the winning prediction's conditions.
bool predictable(const World& w, const InteractionContext& c,
                 std::vector<Condition>* conds = nullptr) {
    for (const auto& m : w.laws) {
        if (!m->supports(w, c)) continue;
        PredictionResult p = m->predict(w, c);
        if (p.possible) {
            if (conds) *conds = std::move(p.conditions);
            return true;
        }
    }
    return false;
}

// ---- entity signatures (MethodKey generalization) -------------------------------
// A signature is what the NPC can legitimately generalize on: material
// quantization for material entities (§10.3 analog), or the observable-sign
// profile for entities it can only see (deposits carry no material — their
// identity IS the signs). Never an entity id.

std::string mat_sig_str(const World& w, MaterialId m) {
    const MaterialDef* d = w.materials.find(m);
    auto get = [&](const char* n) -> std::int64_t {
        if (!d) return 0;
        const PropertyDef* pd = w.props.find_name(n);
        if (!pd) return 0;
        auto it = d->props.find(pd->id);
        return it == d->props.end() ? 0 : it->second.i;
    };
    return "m" + std::to_string(get("hardness") / 100) + "_" +
           std::to_string(get("brittleness") / 100) + "_" +
           std::to_string(get("mass") / 500);
}

const Observation* latest_observation(const AgentMind& mind, EntityId e) {
    const Observation* found = nullptr;
    for (const Observation& o : mind.observations)
        if (o.entity == e) found = &o;      // observations are unique per entity
    return found;
}

std::string prop_quant(const World& w, PropId pid, const Value& v) {
    const PropertyDef* pd = w.props.find(pid);
    if (v.type == ValType::Boolean) return v.b ? "1" : "0";
    if (v.type == ValType::Integer || v.type == ValType::Fixed) {
        if (pd && pd->has_range && pd->hi > pd->lo) {
            const std::int64_t span = pd->hi - pd->lo + 1;
            std::int64_t b = (v.i - pd->lo) * 16 / span;
            b = std::max<std::int64_t>(0, std::min<std::int64_t>(15, b));
            return std::to_string(b);
        }
        return std::to_string(v.i / 50);
    }
    if (v.type == ValType::Text) return v.s.substr(0, 12);
    return std::to_string(v.target);
}

std::string obs_sig_str(const World& w, const AgentMind& mind, EntityId e) {
    const Observation* o = latest_observation(mind, e);
    if (!o) return "o?";
    std::string s = "o";
    for (const auto& [pid, v] : o->props) {
        const PropertyDef* pd = w.props.find(pid);
        s += "." + (pd ? pd->name : "?") + "=" + prop_quant(w, pid, v);
    }
    return s;
}

// Fruit-identity signature: a fruiting plant's FOOD is its fruit material,
// not its body material — method keys must tell berry from nightshade even
// when both grow on identical living_wood (signature quantization would
// otherwise generalize a poison lesson onto look-alike safe fruit).
MaterialId fruit_material_prop(const World& w, const Value* fv) {
    if (!fv || fv->type != ValType::Text || fv->s.empty()) return kNoMaterial;
    return w.materials.id_of(fv->s);
}

std::string entity_sig(const World& w, EntityId npc, EntityId e) {
    const Entity* ent = w.find(e);
    const PropId fmp = w.props.find_name("fruit_material")
        ? w.props.find_name("fruit_material")->id : kNoProp;
    // Fog-of-war: a remembered-but-absent entity still has a signature —
    // fall back to the mind's last snapshot (material / observed signs),
    // never inventing identity the NPC didn't perceive.
    if (!ent) {
        if (const AgentMind* mind = w.find_mind(npc)) {
            const auto it = mind->memory.find(e);
            if (it != mind->memory.end()) {
                if (fmp != kNoProp) {
                    const auto fp = it->second.props.find(fmp);
                    if (fp != it->second.props.end()) {
                        const MaterialId fm = fruit_material_prop(w, &fp->second);
                        if (fm != kNoMaterial)
                            return "f" + std::to_string(fm);
                    }
                }
                if (it->second.material != kNoMaterial)
                    return mat_sig_str(w, it->second.material);
                const Observation* o = latest_observation(*mind, e);
                if (o) {
                    std::string s = "o";
                    for (const auto& [pid, v] : o->props) {
                        const PropertyDef* pd = w.props.find(pid);
                        s += "." + (pd ? pd->name : "?") + "=" + prop_quant(w, pid, v);
                    }
                    return s;
                }
                if (!it->second.kind_name.empty())
                    return "k" + it->second.kind_name;
            }
        }
        return "x";
    }
    if (fmp != kNoProp) {
        if (const Value* fv = w.prop_of(*ent, fmp)) {
            const MaterialId fm = fruit_material_prop(w, fv);
            if (fm != kNoMaterial) return "f" + std::to_string(fm);
        }
    }
    if (ent->material != kNoMaterial) return mat_sig_str(w, ent->material);
    const AgentMind* mind = w.find_mind(npc);
    if (mind) {
        const std::string os = obs_sig_str(w, *mind, e);
        if (os != "o?") return os;
    }
    return "k" + ent->kind_name;
}

// ---- record helpers ----------------------------------------------------------------

void merge_failed_conditions(DiscoveryRecord& rec, const CausalEvent& ev) {
    for (const Condition& c : ev.conditions) {
        if (c.met) continue;
        auto it = std::find_if(rec.failed_conditions.begin(),
                               rec.failed_conditions.end(),
                               [&](const Condition& x) { return x.what == c.what; });
        if (it == rec.failed_conditions.end()) {
            rec.failed_conditions.push_back(c);
        } else if (c.required > it->required) {
            *it = c;                        // keep the strictest boundary seen
        }
    }
}

void note_samples(DiscoveryRecord& rec, const InteractionContext& c) {
    for (EntityId t : c.targets) {
        if (std::find(rec.sample_entities.begin(), rec.sample_entities.end(), t) !=
            rec.sample_entities.end()) continue;
        if (rec.sample_entities.size() >= 8) return;   // provenance cap
        rec.sample_entities.push_back(t);
    }
}

// Would this candidate clear every boundary a Refuted record hit? Checks the
// FIRST step's predicted conditions against failed_conditions (§11.5 unlock).
bool conditions_unlocked(const World& w, const InteractionContext& first,
                         const DiscoveryRecord& rec) {
    if (rec.status != DiscoveryStatus::Refuted || rec.failed_conditions.empty())
        return false;
    std::vector<Condition> pred;
    if (!predictable(w, first, &pred)) return false;
    for (const Condition& fc : rec.failed_conditions) {
        const Condition* hit = nullptr;
        for (const Condition& pc : pred)
            if (pc.what == fc.what) { hit = &pc; break; }
        // a boundary the new prediction no longer evaluates is gone (e.g. a
        // multi-step plan whose first step has no energy check) -> satisfied
        if (hit && !hit->met) return false;   // boundary still unmet -> stays blocked
    }
    return true;
}

// ---- §11.3 hypothesis scoring ---------------------------------------------------
// Score(H) = w1*Ps + w2*Pu + w3*R - w4*C - w5*U  (integer permille heuristic —
// a ranking model, NOT a physical probability, per the spec).
std::int64_t uncertainty_of(const DiscoveryRecord* r, bool unlocked) {
    if (unlocked) return 500;                             // retest-worthy
    if (!r) return 800;                                   // never tried
    switch (r->status) {
        case DiscoveryStatus::Verified:   return 50;
        case DiscoveryStatus::Supported:  return 250;
        case DiscoveryStatus::Tested:     return 500;
        case DiscoveryStatus::Refuted:    return 950;
        default:                          return 800;
    }
}

struct ScoreCtx {
    std::int64_t Ps, Pu, C, U;
};
std::int64_t combine_score(const Problem& p, const ScoreCtx& s) {
    const std::int64_t R = std::min<std::int64_t>(1000, std::max<std::int64_t>(0, p.urgency));
    return (2 * s.Ps + 2 * s.Pu + R - 2 * s.C - s.U) / 4;
}

std::int64_t score_plan_hypothesis(const World& w, EntityId npc,
                                   const Problem& p, const Plan& plan,
                                   const std::string& key, bool unlocked) {
    const AgentMind* mind = w.find_mind(npc);
    const DiscoveryRecord* rec =
        (mind && mind->discoveries.count(key)) ? &mind->discoveries.at(key) : nullptr;
    // For bonded-distress problems plan.score IS the care utility (the
    // goal payload is only a formality), so satisfaction potential maps
    // straight from care magnitude — a devoted parent's plan can outrank
    // a merely-satisfied self-plan.
    const std::int64_t Ps = plan.satisfied ? 900 :
        (p.kind == ProblemKind::BondedDistress
             ? std::clamp<std::int64_t>(plan.score / 1500, 150, 900)
             : (plan.score > 0 ? 550 : 150));
    const std::int64_t Pu = std::min<std::int64_t>(1000, std::max<std::int64_t>(0, plan.score / 2000));
    std::int64_t cost = 0, ticks = 0;
    for (const InteractionContext& c : plan.steps) {
        cost += 40;
        ticks += std::max<std::int64_t>(1, c.parameters.ticks);
    }
    ScoreCtx s{Ps, Pu, cost + ticks * 10, uncertainty_of(rec, unlocked)};
    return combine_score(p, s);
}

std::int64_t score_probe_hypothesis(const World& w, EntityId npc,
                                    const Problem& p, const std::string& key,
                                    bool unlocked) {
    const AgentMind* mind = w.find_mind(npc);
    const DiscoveryRecord* rec =
        (mind && mind->discoveries.count(key)) ? &mind->discoveries.at(key) : nullptr;
    ScoreCtx s{600, 400, 50, uncertainty_of(rec, unlocked)};
    return combine_score(p, s);
}

// Exploratory synthesis (§11.2 + §19E budget): possible-but-unproven
// candidates — assemble pairs, heat/strike transforms. Higher uncertainty,
// moderate cost; they compete with proven methods by Score(H).
std::int64_t score_synth_hypothesis(const World& w, EntityId npc,
                                    const Problem& p, const std::string& key,
                                    bool unlocked) {
    const AgentMind* mind = w.find_mind(npc);
    const DiscoveryRecord* rec =
        (mind && mind->discoveries.count(key)) ? &mind->discoveries.at(key) : nullptr;
    ScoreCtx s{500, 600, 100, uncertainty_of(rec, unlocked)};
    return combine_score(p, s);
}

// Human-readable claim for a candidate plan: "dig -> heat -> process".
std::string describe_plan(const World& w, const Plan& plan) {
    std::string s;
    for (std::size_t i = 0; i < plan.steps.size(); ++i) {
        if (i) s += " -> ";
        const InteractionContext& c = plan.steps[i];
        s += c.parameters.action;
        if (!c.targets.empty()) {
            s += "(";
            for (std::size_t j = 0; j < c.targets.size(); ++j) {
                if (j) s += "+";
                const Entity* t = w.find(c.targets[j]);
                s += t ? t->kind_name : "?";
            }
            s += ")";
        }
    }
    return s;
}

std::string plan_key(const World& w, EntityId npc, const Plan& plan) {
    std::string k = "method:";
    for (const InteractionContext& c : plan.steps) {
        k += c.parameters.action;
        k += "(";
        for (EntityId t : c.targets) { k += entity_sig(w, npc, t); k += "+"; }
        k += ");";
    }
    return k;
}

// ---- food-safety knowledge -------------------------------------------------------

// The MethodKey a single-target plan on this material would carry — the
// same string plan_key() emits, so records line up exactly.
std::string material_method_key(const World& w, EntityId /*npc*/,
                                const std::string& verb, MaterialId m,
                                bool fruiting) {
    const std::string sig = fruiting
        ? "f" + std::to_string(m) : mat_sig_str(w, m);
    return "method:" + verb + "(" + sig + "+);";
}

// ---- experiment outcome evaluation ----------------------------------------------
// Success is measured from World Truth deltas the trial produced — the NPC's
// own experiment result, which is exactly what beliefs/records may cite.
bool goal_satisfied_now(const World& w, const Goal& g) {
    if (g.kind == Goal::Kind::SatisfyNeed)
        return need_value(w, g.target, g.need) <= g.value;
    // PropValue: the target entity (or kNoEntity = any living entity) holds it
    if (g.prop == kNoProp) return false;
    if (g.target != kNoEntity) {
        const Entity* e = w.find(g.target);
        return e && e->alive && w.prop_int(*e, g.prop, -1) == g.value;
    }
    for (const auto& ep : w.entities) {
        const Entity& e = *ep;
        if (!e.alive) continue;
        if (w.prop_int(e, g.prop, -1) == g.value) return true;
    }
    return false;
}

// A sign probe "worked" when the trial recovered something: a spawn, the
// mark becoming exposed, or a recoverable quantity dropping.
bool sign_yielded(const World& w, EntityId subject,
                  const std::vector<EventId>& events) {
    const PropId exposed = w.props.find_name("exposed")
        ? w.props.find_name("exposed")->id : kNoProp;
    const PropId dep_rem = w.props.find_name("deposit_remaining")
        ? w.props.find_name("deposit_remaining")->id : kNoProp;
    const PropId quantity = w.props.find_name("quantity")
        ? w.props.find_name("quantity")->id : kNoProp;
    for (EventId id : events) {
        const CausalEvent* ev = w.ledger.find(id);
        if (!ev) continue;
        if (!ev->spawned.empty()) return true;
        for (const StateChange& c : ev->changes) {
            if (c.entity != subject) continue;
            const PropertyDef* pd = w.props.find(c.prop);
            if (!pd) continue;
            if (exposed != kNoProp && c.prop == exposed &&
                c.before.type == ValType::Boolean && !c.before.b &&
                c.after.type == ValType::Boolean && c.after.b) return true;
            if ((dep_rem != kNoProp && c.prop == dep_rem) ||
                (quantity != kNoProp && c.prop == quantity)) {
                if (c.after.type == ValType::Integer &&
                    c.before.type == ValType::Integer &&
                    c.after.i < c.before.i) return true;
            }
        }
    }
    return false;
}

// A defensive/tool hypothesis "worked" when it produced a usable structure:
// a spawn (assembled composite / processed goods / fragments) or a marked
// gain on the probed entity.
bool synth_yielded(const World& w, const InteractionContext& first,
                   const std::vector<EventId>& events) {
    for (EventId id : events) {
        const CausalEvent* ev = w.ledger.find(id);
        if (!ev) continue;
        if (!ev->spawned.empty()) return true;
    }
    // single-target transforms: any alive change on the target counts
    if (first.targets.size() == 1) {
        const PropId exposed = w.props.find_name("exposed")
            ? w.props.find_name("exposed")->id : kNoProp;
        for (EventId id : events) {
            const CausalEvent* ev = w.ledger.find(id);
            if (!ev) continue;
            for (const StateChange& c : ev->changes) {
                if (c.entity != first.targets[0]) continue;
                if (exposed != kNoProp && c.prop == exposed &&
                    c.after.type == ValType::Boolean && c.after.b) return true;
            }
        }
    }
    return false;
}

void recompute_record_status(DiscoveryRecord& rec) {
    if (rec.evidence_against > rec.evidence_for * 2 && rec.evidence_against > 0) {
        rec.status = DiscoveryStatus::Refuted;
    } else if (rec.trials >= 6 && rec.evidence_against == 0 &&
               rec.evidence_for >= 150) {
        rec.status = DiscoveryStatus::Verified;   // personally repeated success
    } else if (rec.evidence_for >= 90 && rec.evidence_against == 0) {
        rec.status = DiscoveryStatus::Supported;
    } else if (rec.trials > 0 || rec.evidence_for > 0 || rec.evidence_against > 0) {
        rec.status = DiscoveryStatus::Tested;
    } else {
        rec.status = DiscoveryStatus::Open;
    }
}

// ---- hypothesis assembly ------------------------------------------------------------

// Shared tail for every hypothesis kind: dedupe by key (keep best score),
// mark tested/blocked state from the mind's records.
void emit_hypothesis(std::map<std::string, Hypothesis>& best,
                     const World& w, EntityId npc,
                     Hypothesis h, std::int64_t score) {
    h.score = score;
    const AgentMind* mind = w.find_mind(npc);
    if (mind) {
        const auto it = mind->discoveries.find(h.key);
        if (it != mind->discoveries.end()) {
            h.tested = it->second.trials > 0;
            // Refuted + boundaries still unmet -> do not offer the retry
            if (it->second.status == DiscoveryStatus::Refuted &&
                !h.plan.steps.empty() &&
                !conditions_unlocked(w, h.plan.steps.front(), it->second))
                return;
            h.refuted_unlocked = it->second.status == DiscoveryStatus::Refuted;
        }
    }
    auto cur = best.find(h.key);
    if (cur == best.end() || h.score > cur->second.score) best[h.key] = h;
}

// Single-target / pair probes with every known verb the laws predict possible
// on the subject — survey (signs) and defence (threats) share this branch.
void add_probe_hypotheses(std::map<std::string, Hypothesis>& best,
                          const World& w, EntityId npc, const Problem& p,
                          EntityId subject, const char* key_prefix,
                          const char* claim_verb) {
    const AgentMind* mind = w.find_mind(npc);
    if (!mind) return;
    // Fog-of-war: the subject's identity comes from MEMORY (last perceived
    // snapshot), not a live lookup — whether it still exists is only learned
    // when a real probe interact() reaches it. predictable() still gates
    // every candidate through law predict() on World Truth.
    std::string sub_kind = "?";
    if (const auto it = mind->memory.find(subject); it != mind->memory.end())
        sub_kind = it->second.kind_name.empty() ? "?" : it->second.kind_name;
    else if (const Observation* o = latest_observation(*mind, subject))
        (void)o;                                    // signs live in obs props
    for (const std::string& verb : action_verbs()) {
        if (!mind->known_actions.count(verb)) continue;
        if (verb == "rest") continue;                    // actor-only, no probe
        InteractionContext c = mk(npc, {subject}, verb);
        if (!predictable(w, c)) continue;                // laws dispose
        Plan plan; plan.steps.push_back(c); plan.score = 0;
        Hypothesis h;
        h.key = std::string(key_prefix) + entity_sig(w, npc, subject) + ":" + verb;
        h.claim = std::string(claim_verb) + " " + sub_kind + " with " + verb;
        h.plan = plan; h.serves = p.kind;
        const bool unl = [&] {
            const AgentMind* m = w.find_mind(npc);
            const auto it = m->discoveries.find(h.key);
            return it != m->discoveries.end() &&
                   conditions_unlocked(w, c, it->second);
        }();
        emit_hypothesis(best, w, npc, h,
                        score_probe_hypothesis(w, npc, p, h.key, unl));
    }
}

// Exploratory Synthesis (§11.2 + §19E search budget): when plans come up
// empty or a capability gap/threat demands new means, propose assemble
// pairs and heat/strike transforms over the NPC's known entities. Everything
// is still gated by law predict() and known_actions — synthesis proposes,
// physics disposes.
void add_synthesis_hypotheses(std::map<std::string, Hypothesis>& best,
                              const World& w, EntityId npc, const Problem& p) {
    const AgentMind* mind = w.find_mind(npc);
    const Entity* me = w.find(npc);
    if (!mind || !me || !me->alive) return;

    // candidate pool: REMEMBERED entities (memory snapshots — possibly
    // stale), capped for budget. Dead/absent ones are filtered downstream
    // by law predict(), which is the honest boundary: the NPC proposes,
    // physics disposes, and a failed probe is itself evidence.
    std::vector<EntityId> pool;
    for (const auto& [id, s] : mind->memory) {
        if (id == npc) continue;
        const PropId cont = w.props.find_name("contained")
            ? w.props.find_name("contained")->id : kNoProp;
        // `contained` is not observable — skip the truth check entirely;
        // predict() refuses contained targets anyway.
        (void)cont;
        pool.push_back(id);
        if (pool.size() >= 8) break;                    // §19E: bounded pool
    }
    if (pool.empty()) return;

    const bool can_assemble = mind->known_actions.count("assemble") != 0;
    const bool can_heat = mind->known_actions.count("heat") != 0;
    const bool can_strike = mind->known_actions.count("strike") != 0;
    if (!can_assemble && !can_heat && !can_strike) return;

    int synth_count = 0;
    const int kSynthBudget = 6;                          // per-problem cap
    auto try_emit = [&](InteractionContext c, const std::string& key,
                        const std::string& claim) {
        if (synth_count >= kSynthBudget) return;
        if (!predictable(w, c)) return;
        ++synth_count;
        Plan plan; plan.steps.push_back(c); plan.score = 0;
        Hypothesis h;
        h.key = key; h.claim = claim; h.plan = plan; h.serves = p.kind;
        const bool unl = [&] {
            const AgentMind* m = w.find_mind(npc);
            const auto it = m->discoveries.find(h.key);
            return it != m->discoveries.end() &&
                   conditions_unlocked(w, c, it->second);
        }();
        emit_hypothesis(best, w, npc, h,
                        score_synth_hypothesis(w, npc, p, h.key, unl));
    };

    // kind labels for claims come from memory snapshots — never a live
    // lookup that would leak whether the entity still exists.
    auto kind_of = [&](EntityId id) -> std::string {
        if (const auto it = mind->memory.find(id); it != mind->memory.end() &&
            !it->second.kind_name.empty()) return it->second.kind_name;
        if (const Entity* e = w.find(id)) return e->kind_name;
        return "?";
    };
    if (can_assemble) {
        for (std::size_t i = 0; i < pool.size(); ++i)
            for (std::size_t j = i + 1; j < pool.size(); ++j) {
                InteractionContext c = mk(npc, {pool[i], pool[j]}, "assemble");
                try_emit(c, "synth:assemble:" + entity_sig(w, npc, pool[i]) +
                              "+" + entity_sig(w, npc, pool[j]),
                         "assemble " + kind_of(pool[i]) + " + " + kind_of(pool[j]));
            }
    }
    for (EntityId t : pool) {
        if (can_heat)
            try_emit(mk(npc, {t}, "heat"),
                     "synth:heat:" + entity_sig(w, npc, t),
                     "heat " + kind_of(t));
        if (can_strike)
            try_emit(mk(npc, {t}, "strike"),
                     "synth:strike:" + entity_sig(w, npc, t),
                     "strike " + kind_of(t));
    }
}

}  // namespace

// ---- food-safety knowledge -------------------------------------------------------

DiscoveryStatus food_method_status(const World& w, EntityId npc,
                                   const std::string& verb, MaterialId m,
                                   bool fruiting) {
    const AgentMind* mind = w.find_mind(npc);
    if (!mind || m == kNoMaterial) return DiscoveryStatus::Open;
    const std::string sig = (fruiting
        ? "f" + std::to_string(m) : mat_sig_str(w, m)) + "+";
    const std::string prefix = "method:" + verb + "(";
    DiscoveryStatus best = DiscoveryStatus::Open;
    bool any = false;
    auto rank = [](DiscoveryStatus s) -> int {
        switch (s) {
            case DiscoveryStatus::Verified:   return 5;
            case DiscoveryStatus::Supported:  return 4;
            case DiscoveryStatus::Tested:     return 3;
            case DiscoveryStatus::Open:       return 2;
            case DiscoveryStatus::Refuted:    return 1;
        }
        return 0;
    };
    for (const auto& [key, rec] : mind->discoveries) {
        if (key.rfind(prefix, 0) != 0) continue;
        if (key.find(sig) == std::string::npos) continue;
        any = true;
        // taboo dominates: any refuted signature-match makes the material
        // dangerous in this NPC's experience, whatever else succeeded.
        if (rec.status == DiscoveryStatus::Refuted)
            return DiscoveryStatus::Refuted;
        if (rank(rec.status) > rank(best)) best = rec.status;
    }
    return any ? best : DiscoveryStatus::Open;
}

void note_harmful_method(World& w, EntityId npc, const std::string& verb,
                         MaterialId m, bool fruiting, const std::string& claim) {
    if (m == kNoMaterial) return;
    AgentMind* mp = w.find_mind(npc) ? &w.mind(npc) : nullptr;
    if (!mp) return;                  // mindless actors keep no knowledge
    AgentMind& mind = *mp;
    const std::string key = material_method_key(w, npc, verb, m, fruiting);
    DiscoveryRecord& rec = mind.discoveries[key];
    if (rec.key.empty()) {
        rec.key = key;
        rec.claim = claim;
        rec.status = DiscoveryStatus::Open;
        MindParams mp;
        if (const Entity* me = w.find(npc)) mp = mind_params(w, *me);
        rec.engram.clarity = mp.base_clarity;
        rec.engram.stability = mp.base_stability;
    }
    rec.evidence_against += 80;      // a witnessed death is strong evidence
    rec.last_tick = w.clock_.tick();
    recompute_record_status(rec);    // Open/Tested -> Refuted
}


// ---- problem detection -----------------------------------------------------------

std::vector<Problem> detect_problems(const World& w, EntityId npc,
                                     const std::vector<Goal>& extra) {
    std::vector<Problem> out;
    const AgentMind* mind = w.find_mind(npc);
    const Entity* me = w.find(npc);
    if (!me || !me->alive) return out;

    for (const Goal& g : derive_goals(w, npc)) {
        Problem p; p.kind = ProblemKind::UnmetNeed; p.goal = g;
        const Value* v = w.prop_of(*me, g.need);
        p.urgency = v ? v->i : 600;
        out.push_back(p);
    }
    for (const Goal& g : extra) {
        Problem p; p.kind = ProblemKind::ExtraGoal; p.goal = g;
        p.urgency = 450;
        out.push_back(p);
    }
    // unexplained observable signs: any observable prop other than `exposed`
    // (trace-mineral stains etc.) marks something worth a closer look.
    if (mind) {
        const PropId exposed = w.props.find_name("exposed")
            ? w.props.find_name("exposed")->id : kNoProp;
        for (const Observation& o : mind->observations) {
            // Fog-of-war: sign detection uses ONLY the observation's own
            // props — no live entity lookup. Whether the marked entity is
            // still there is proven by walking over and probing it.
            std::size_t signs = 0;
            for (const auto& [pid, v] : o.props)
                if (pid != exposed) ++signs;
            if (signs == 0) continue;
            Problem p; p.kind = ProblemKind::SuspiciousSign;
            p.subject = o.entity;
            p.urgency = std::min<std::int64_t>(600, 200 + 100 * signs);
            out.push_back(p);
        }
    }

    // ---- Threat & CapabilityGap from the causal ledger (TODO §11.1/§10.6) ----
    // The NPC's own history is legitimate self-knowledge: it remembers being
    // hurt and being refused. Scan the recent tail of the ledger.
    const PropId integ = w.props.find_name("integrity")
        ? w.props.find_name("integrity")->id : kNoProp;
    const std::size_t led = w.ledger.size();
    const std::size_t scan = std::min<std::size_t>(led, 32);
    bool threat = false; std::int64_t threat_urgency = 0; EntityId threat_src = kNoEntity;
    int gap_count = 0; EntityId gap_src = kNoEntity;
    for (std::size_t i = led; i-- > led - scan;) {
        const CausalEvent& ev = w.ledger.history()[i];
        if (!threat && integ != kNoProp)
            for (const StateChange& c : ev.changes)
                if (c.entity == npc && c.prop == integ &&
                    c.after.type == ValType::Integer && c.before.type == ValType::Integer &&
                    c.after.i < c.before.i) {
                    threat = true;
                    threat_urgency = 500 + std::min<std::int64_t>(450,
                                        (c.before.i - c.after.i) * 4);
                    for (EntityId x : ev.participants)
                        if (x != npc) { threat_src = x; break; }
                    break;
                }
        if (ev.failed)
            for (const Condition& c : ev.conditions) {
                if (c.met) continue;
                if (c.what == "force" || c.what == "reach" || c.what == "energy") {
                    ++gap_count;
                    if (gap_src == kNoEntity)
                        for (EntityId x : ev.participants)
                            if (x != npc) { gap_src = x; break; }
                    break;
                }
            }
        if (threat && gap_count > 0) break;
    }
    if (threat) {
        Problem p; p.kind = ProblemKind::Threat; p.subject = threat_src;
        p.urgency = threat_urgency;
        out.push_back(p);
    }
    if (gap_count > 0) {
        Problem p; p.kind = ProblemKind::CapabilityGap; p.subject = gap_src;
        p.urgency = std::min<std::int64_t>(900, 400 + 150 * gap_count);
        out.push_back(p);
    }

    // Bonded distress (Nhịp 4.5): a charge >= +600 whose inferred welfare
    // is low becomes a problem of its own — care then competes with
    // self-preservation inside the same Score(H) machinery.
    if (mind) {
        EntityId worst = kNoEntity; std::int64_t wd = 0;
        for (const auto& [eid, ms] : mind->memory) {
            if (ms.engram.emotional_charge < 600) continue;
            const std::int64_t d = bonded_distress(w, *mind, eid);
            if (d > wd) { wd = d; worst = eid; }
        }
        if (wd >= 400) {
            Problem p; p.kind = ProblemKind::BondedDistress;
            p.subject = worst; p.urgency = wd;
            out.push_back(p);
        }
    }

    std::sort(out.begin(), out.end(),
              [](const Problem& a, const Problem& b) { return a.urgency > b.urgency; });
    return out;
}

// ---- candidate generation ---------------------------------------------------------

std::vector<Hypothesis> generate_hypotheses(const World& w, EntityId npc,
                                            const Problem& p,
                                            std::size_t max_results) {
    std::vector<Hypothesis> out;
    const AgentMind* mind = w.find_mind(npc);
    if (!mind || mind->known_actions.empty()) return out;

    std::map<std::string, Hypothesis> best;

    if (p.kind == ProblemKind::SuspiciousSign ||
        (p.kind == ProblemKind::Threat && p.subject != kNoEntity)) {
        // survey / defence probes on the marked entity
        add_probe_hypotheses(best, w, npc, p, p.subject,
                             p.kind == ProblemKind::Threat ? "defence:" : "sign:",
                             p.kind == ProblemKind::Threat ? "counter" : "probe");
    }

    if (p.kind == ProblemKind::Threat || p.kind == ProblemKind::CapabilityGap ||
        p.kind == ProblemKind::UnmetNeed || p.kind == ProblemKind::ExtraGoal ||
        p.kind == ProblemKind::BondedDistress) {
        if (p.kind == ProblemKind::UnmetNeed || p.kind == ProblemKind::ExtraGoal ||
            p.kind == ProblemKind::BondedDistress) {
            // need / caller goal: multi-step speculative candidates.
            // Nhịp 4a: lookahead depth AND branch budget come from the
            // individual's genome-expressed `mind_plan_horizon` (1..6) —
            // a strategist sees deeper chains, a reflexive mind only
            // immediate actions. Executive override still hijacks this
            // upstream in the driver when panic/rage crosses threshold.
            std::int64_t horizon = 2, beam = 6;
            if (const Entity* me = w.find(npc)) {
                auto gp = [&](const char* n, std::int64_t d) {
                    const PropertyDef* pd = w.props.find_name(n);
                    return pd ? w.prop_int(*me, pd->id, d) : d;
                };
                horizon = std::clamp<std::int64_t>(
                    gp("mind_plan_horizon", 2), 1, 6);
                beam = std::clamp<std::int64_t>(4 + 2 * horizon, 4, 16);
            }
            Goal g = p.goal;
            if (p.kind == ProblemKind::BondedDistress) {
                // care plans are scored by care_bonus inside plan_multi;
                // the goal payload only keeps evaluate() well-formed —
                // the actor's own worst need, satisfied level 0.
                g.kind = Goal::Kind::SatisfyNeed; g.target = kNoEntity;
                g.value = 0;
                if (const Entity* me = w.find(npc))
                    for (const char* nn : {"hunger", "thirst", "fatigue"}) {
                        const PropertyDef* pd = w.props.find_name(nn);
                        if (pd && w.prop_of(*me, pd->id)) {
                            g.need = pd->id; break;
                        }
                    }
                if (g.need == kNoProp) g.need =
                    w.props.find_name("hunger")
                        ? w.props.find_name("hunger")->id : kNoProp;
            }
            std::vector<Plan> plans = plan_multi(w, npc, g,
                                                 static_cast<int>(horizon),
                                                 static_cast<std::size_t>(beam),
                                                 max_results);
            for (const Plan& pl : plans) {
                if (pl.steps.empty()) continue;
                Hypothesis h;
                h.key = plan_key(w, npc, pl);
                h.claim = describe_plan(w, pl) + (pl.satisfied ? " [satisfies]" : "");
                h.plan = pl; h.goal = p.goal; h.serves = p.kind;
                const bool unl = !pl.steps.empty() && [&] {
                    const AgentMind* m = w.find_mind(npc);
                    const auto it = m->discoveries.find(h.key);
                    return it != m->discoveries.end() &&
                           conditions_unlocked(w, pl.steps.front(), it->second);
                }();
                emit_hypothesis(best, w, npc, h,
                                score_plan_hypothesis(w, npc, p, pl, h.key, unl));
            }
        }
        // capability gaps & threats always get synthesis candidates; need/goal
        // problems get them too when plan_multi found nothing usable — new
        // means are how stuck NPCs invent tools (§11.2, §19E budget inside).
        const bool plans_empty = best.empty();
        if (p.kind == ProblemKind::Threat || p.kind == ProblemKind::CapabilityGap ||
            plans_empty)
            add_synthesis_hypotheses(best, w, npc, p);
    }

    for (auto& [_, h] : best) out.push_back(h);
    std::sort(out.begin(), out.end(),
              [](const Hypothesis& a, const Hypothesis& b) { return a.score > b.score; });
    if (out.size() > max_results) out.resize(max_results);
    return out;
}

// ---- experimentation & verification ----------------------------------------------

ExperimentOutcome run_experiment(World& w, EntityId npc, const Hypothesis& h) {
    ExperimentOutcome out;
    const Entity* me = w.find(npc);
    if (!me || !me->alive || h.plan.steps.empty()) {
        out.note = "no live actor or empty plan";
        return out;
    }
    AgentMind& mind = w.mind(npc);
    DiscoveryRecord& rec = mind.discoveries[h.key];
    const MindParams mp = me ? mind_params(w, *me) : MindParams{};
    if (rec.key.empty()) {
        rec.key = h.key; rec.claim = h.claim;
        rec.engram.clarity = mp.base_clarity;
        rec.engram.stability = mp.base_stability;
    } else if (rec.engram.clarity > 0 && me) {
        // practising a method rehearses it — unused skills fade instead
        rehearse_engram(rec.engram, mind_params(w, *me), w.clock_.tick());
    }

    const bool need_goal = h.goal.kind == Goal::Kind::SatisfyNeed;
    const std::int64_t need_before = need_goal ? need_value(w, npc, h.goal.need) : 0;
    const InteractionContext& first = h.plan.steps.front();
    const EntityId subject = !first.targets.empty() ? first.targets[0] : kNoEntity;

    // harm baseline: a trial that damages or kills the actor is NEVER a
    // success, no matter how well it relieved the need — poison still fills
    // the belly on the way down.
    const PropId integ_pid = w.props.find_name("integrity")
        ? w.props.find_name("integrity")->id : kNoProp;
    const std::int64_t integ_before = integ_pid != kNoProp
        ? w.prop_int(*me, integ_pid, -1) : -1;

    bool all_completed = true;
    for (const InteractionContext& c : h.plan.steps) {
        InteractionResult res = w.interact(c);       // REAL world, real laws
        out.ran = true;
        if (res.event != kNoEvent) {
            out.events.push_back(res.event);
            rec.experiments.push_back(res.event);
            const CausalEvent* ev = w.ledger.find(res.event);
            if (ev) {
                for (LawId l : ev->applied_laws) {
                    const std::string nm = law_name(w, l);
                    if (std::find(rec.supporting_laws.begin(),
                                  rec.supporting_laws.end(), nm) == rec.supporting_laws.end())
                        rec.supporting_laws.push_back(nm);
                }
                // only REFUSED conditions bound a method: a successful event
                // can still carry unmet per-target conditions (other targets
                // skipped) that are not failures of the method itself
                if (ev->failed) merge_failed_conditions(rec, *ev);
            }
        }
        if (res.failed_event != kNoEvent) {
            out.events.push_back(res.failed_event);
            rec.experiments.push_back(res.failed_event);
            const CausalEvent* fe = w.ledger.find(res.failed_event);
            if (fe) merge_failed_conditions(rec, *fe);
        }
        note_samples(rec, c);
        if (!res.completed) {
            all_completed = false;
            if (out.note.empty()) out.note = res.failure;
            break;                                    // later steps moot
        }
    }
    if (!out.ran) { out.note = "nothing attempted"; return out; }

    // verdict from World Truth deltas, not from intent
    if (need_goal) {
        out.succeeded = need_value(w, npc, h.goal.need) < need_before;
    } else if (h.serves == ProblemKind::SuspiciousSign && subject != kNoEntity) {
        out.succeeded = sign_yielded(w, subject, out.events);
    } else if (h.serves == ProblemKind::Threat && subject != kNoEntity) {
        out.succeeded = synth_yielded(w, first, out.events);
    } else if (h.serves == ProblemKind::CapabilityGap ||
               h.serves == ProblemKind::ExtraGoal) {
        if (h.goal.prop != kNoProp) out.succeeded = goal_satisfied_now(w, h.goal);
        else out.succeeded = synth_yielded(w, first, out.events);
    } else {
        out.succeeded = all_completed;                // method chain ran clean
    }
    if (!all_completed && out.succeeded) out.succeeded = false;

    // actor harm overrides every other verdict: integrity loss or death
    // during the trial marks the method dangerous, not merely unsuccessful.
    const Entity* me_after = w.find(npc);
    const std::int64_t integ_after = integ_pid != kNoProp && me_after
        ? w.prop_int(*me_after, integ_pid, -1) : -1;
    const bool actor_harmed =
        (integ_before >= 0 && integ_after >= 0 && integ_after < integ_before) ||
        (me_after != nullptr && !me_after->alive);
    if (actor_harmed) {
        out.succeeded = false;
        if (out.note.empty()) out.note = "trial harmed the actor";
    }

    // ---- verification bookkeeping (§11.5) ----
    // Nhịp 4b/c: abstraction_gain speeds evidence accrual (the "inventor"
    // extracts more lesson per trial); motor_precision deepens the skill
    // trace itself — a master's method consolidates faster.
    rec.trials += 1;
    if (out.succeeded) {
        rec.evidence_for += std::max<std::int64_t>(1,
            34 * mp.abstraction_gain / 1000);
        rec.engram.stability = std::min(mp.max_stability,
            rec.engram.stability +
            mp.rehearsal_stability_bonus * mp.motor_precision / 1000);
    } else {
        rec.evidence_against += (all_completed ? 25 : 10);
    }
    if (actor_harmed) rec.evidence_against += 60;   // harm is strong counter-evidence
    rec.last_tick = w.clock_.tick();
    std::sort(rec.supporting_laws.begin(), rec.supporting_laws.end());
    recompute_record_status(rec);
    return out;
}

// ---- one discovery cycle -----------------------------------------------------------

bool discover_step(World& w, EntityId npc, ExploreGoal* out_explore) {
    const AgentMind* mind0 = w.find_mind(npc);
    if (!mind0 || mind0->known_actions.empty()) return false;

    std::vector<Problem> problems = detect_problems(w, npc);
    if (problems.empty()) return false;

    // gather hypotheses for the top few problems, best score first
    std::vector<Hypothesis> cands;
    const std::size_t nprob = std::min<std::size_t>(problems.size(), 3);
    for (std::size_t i = 0; i < nprob; ++i) {
        std::vector<Hypothesis> hs = generate_hypotheses(w, npc, problems[i], 4);
        cands.insert(cands.end(), hs.begin(), hs.end());
    }

    // nothing runnable -> maybe the interesting thing is out of reach:
    // report a survey waypoint for the controller layer (Discovery never
    // moves the NPC itself).
    auto try_explore = [&]() -> bool {
        if (!out_explore) return false;
        const PropId reach = w.props.find_name("reach")
            ? w.props.find_name("reach")->id : kNoProp;
        const Entity* me = w.find(npc);
        if (!me) return false;
        const std::int64_t rr = reach != kNoProp ? w.prop_int(*me, reach, 64) : 64;
        const std::int64_t r2 = rr * rr;
        for (const Problem& p : problems) {
            if (p.kind != ProblemKind::SuspiciousSign &&
                p.kind != ProblemKind::Threat) continue;
            // waypoint comes from the mind's MEMORY (last perceived pos) —
            // the NPC walks toward where it LAST SAW the sign, not toward
            // live World Truth it cannot perceive from here.
            const AgentMind* mm = w.find_mind(npc);
            if (!mm) continue;
            const auto mi = mm->memory.find(p.subject);
            if (mi == mm->memory.end()) continue;
            if (dist2(me->pos, mi->second.pos) <= r2) continue;  // seen already
            out_explore->target = mi->second.pos;
            out_explore->subject = p.subject;
            out_explore->reason = p.kind == ProblemKind::Threat
                ? "threat beyond reach" : "suspicious sign beyond reach";
            return true;
        }
        return false;
    };

    if (cands.empty()) {
        if (try_explore()) return false;      // waypoint reported via out_explore
        return false;
    }
    std::sort(cands.begin(), cands.end(),
              [](const Hypothesis& a, const Hypothesis& b) { return a.score > b.score; });

    for (const Hypothesis& h : cands) {
        const AgentMind* m = w.find_mind(npc);
        if (!m) break;
        const auto it = m->discoveries.find(h.key);
        if (it != m->discoveries.end() &&
            it->second.status == DiscoveryStatus::Verified) continue;  // proven
        ExperimentOutcome oc = run_experiment(w, npc, h);
        if (oc.ran) return true;
    }
    if (try_explore()) return false;
    return false;
}

}  // namespace simlab
