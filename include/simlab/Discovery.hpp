#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <set>
#include "Cognition.hpp"
#include "Types.hpp"

namespace simlab {

class World;
struct Plan;
struct InteractionContext;

// ---- Discovery Engine (TODO §11, Phase 6) -------------------------------------
// Turns agent knowledge into tested methods:
//   Problem Detection -> Candidate Generation -> Hypothesis -> Experimentation
//   -> Verification (DiscoveryRecord).
//
// Hard rules (same as cognition):
//  * Candidates come ONLY from the NPC's own knowledge — known_actions,
//    observations, beliefs. Nothing here reads World Truth on the NPC's behalf.
//  * Methods generalize by SIGNATURE (material quantization / observable-sign
//    profile), never by entity identity — "dig the blue-stained rock" works on
//    every rock that shows the same signs.
//  * Experiments run through World::interact on the REAL world — laws decide
//    outcomes; a plausible idea can still fail and the failure is evidence,
//    including WHICH condition boundary was hit (validConditions).
//  * DiscoveryRecords live in the AgentMind: knowledge with provenance, never
//    a write into entities/terrain/ledger-truth.

// What the NPC wants resolved (TODO §11.1: discovery starts from a problem).
enum class ProblemKind : std::uint8_t {
    UnmetNeed = 0,        // body need above urgency (hunger/thirst/fatigue)
    SuspiciousSign,       // observed entity carries unexplained observable signs
    ExtraGoal,            // caller-supplied goal that has no satisfying plan yet
    Threat,               // recent ledger: the NPC's integrity was damaged
    CapabilityGap,        // recent ledger: attempts refused on force/reach/energy
    BondedDistress,       // a dearly-held engram shows inferred need/threat
    SomaticTrauma,        // own body: open wound bleeding or infected
    Evacuate              // own hollow organ: contents_load demands relief
};
struct Problem {
    ProblemKind kind = ProblemKind::UnmetNeed;
    Goal goal;                          // UnmetNeed/ExtraGoal payload
    EntityId subject = kNoEntity;       // SuspiciousSign/Threat: the marked entity
    std::int64_t urgency = 0;           // permille ranking hint
};

// Movement directive for an external locomotion/controller layer. Discovery
// NEVER moves the NPC itself — it only reports where worth going is.
struct ExploreGoal {
    Vec2 target{};                      // map position to walk toward
    EntityId subject = kNoEntity;       // entity that motivated the trip (0 = none)
    std::string reason;                 // e.g. "suspicious sign out of reach"
};

// A candidate method/hypothesis (§11.2): ranked by Score(H), never a
// guaranteed outcome. `plan` is executed verbatim by run_experiment —
// speculative evaluation already happened inside plan_multi on clones.
struct Hypothesis {
    std::string key;                    // MethodKey: signature-based, entity-free
    std::string claim;                  // human-readable what-is-being-tested
    Plan plan;
    Goal goal;                          // need/extra goal this serves (if any)
    ProblemKind serves = ProblemKind::UnmetNeed;
    std::int64_t score = 0;             // §11.3 ranking (higher = try sooner)
    bool tested = false;                // a DiscoveryRecord with trials exists
    // Refuted record whose failed conditions are NOW satisfied by this
    // candidate's predicted conditions -> unlocked for retest.
    bool refuted_unlocked = false;
};

struct ExperimentOutcome {
    bool ran = false;                   // at least one step attempted
    bool succeeded = false;             // experiment produced the hoped result
    std::vector<EventId> events;        // causal events the trial produced
    std::string note;                   // why it failed / what happened
};

// ---- services ------------------------------------------------------------------

// Problem Detection: unmet needs (derive_goals) + unexplained observable signs
// + caller goals + Threat (recent integrity loss) + CapabilityGap (recent
// force/reach/energy refusals). Sorted by urgency, desc.
std::vector<Problem> detect_problems(const World& w, EntityId npc,
                                     const std::vector<Goal>& extra = {});

// Candidate Generator: hypotheses for ONE problem.
//  * need/extra goals -> plan_multi() candidate chains (belief-guided already)
//  * suspicious signs/threats -> single-step probes with every known verb the
//    laws predict possible on the marked entity (survey / defence)
//  * capability gaps & empty plans -> Exploratory Synthesis: assemble pairs of
//    known entities, heat/strike transform candidates (search-budget capped)
// Ranked by the §11.3 score; `tested` marks hypotheses with prior trials.
std::vector<Hypothesis> generate_hypotheses(const World& w, EntityId npc,
                                            const Problem& p,
                                            std::size_t max_results = 4);

// Experimentation + Verification: execute the hypothesis's plan on the REAL
// world via interact(), then update the mind's DiscoveryRecord — trials,
// evidence for/against, supporting law names, failed-condition boundaries
// (validConditions), sample entities, status transitions
// (Open -> Tested -> Supported -> Verified | Refuted). Failed steps are
// evidence too: a refused/failed interaction still lands in the ledger.
ExperimentOutcome run_experiment(World& w, EntityId npc, const Hypothesis& h);

// One discovery cycle: detect problems, generate hypotheses across them,
// pick the best-scored hypothesis without a Verified record, and run it as a
// real experiment. Returns false when there is nothing worth trying
// (no problems, no known actions, or everything verified).
// If `out_explore` is given and the best problem needs movement first
// (e.g. a sign beyond reach), it receives the waypoint and no experiment runs.
bool discover_step(World& w, EntityId npc, ExploreGoal* out_explore = nullptr);

// ---- food-safety knowledge (reflexes & witnessed harm) -------------------------
// MethodKey status lookup by MATERIAL SIGNATURE: the worst/best
// DiscoveryStatus the NPC holds for `verb` plans that mention this material.
// Open = never tested (taste-test territory), Refuted = taboo, Verified/
// Supported = known-good. Used by the driver's consume reflex so routine
// eating skips plan_multi while unknown or taboo foods still go through
// discover_step — knowledge gates reflexes, laws gate outcomes.
DiscoveryStatus food_method_status(const World& w, EntityId npc,
                                   const std::string& verb, MaterialId m,
                                   bool fruiting = false);

// Record witnessed/self harm against a material: creates (or worsens) a
// Refuted DiscoveryRecord keyed on the material signature so the taboo is
// explicit, shareable knowledge — communicate() transmits it like any
// hard-won failed_conditions boundary.
void note_harmful_method(World& w, EntityId npc, const std::string& verb,
                         MaterialId m, bool fruiting, const std::string& claim);

// ---- compositional proto-language (Phase 12 base core) -------------------------
// One atomic unit of meaning decomposed out of a causal key: a content token
// (object/action/state — kind 1/2) or a relation operator (CAUSE/NEGATE/AND —
// kind 4). Atoms are what lexical_entries actually name; CausalClauses are
// ordered atom sequences, so a community coins a small vocabulary once and
// recombines it forever instead of minting a word per situation.
struct AtomRef {
    std::string key;      // stable atom id (content token or "rel:<op>")
    std::int64_t kind = 1;// 1 object/state, 2 action verb, 4 relation
};

// Deterministically decompose a causal key ("heat+stagnant_water->fever")
// into ordered atoms. `verbs` = the community's known action names: a token
// matching one is an action atom (kind 2), anything else is a content atom
// (kind 1). Operators become relation atoms (kind 4): '+'->rel:and,
// '->'/'>->'->rel:causes, '!'->rel:not. Pure function — same key + same verb
// set always yields the same clause skeleton.
std::vector<AtomRef> decompose_key(const std::string& key,
                                   const std::set<std::string>& verbs);

}  // namespace simlab
