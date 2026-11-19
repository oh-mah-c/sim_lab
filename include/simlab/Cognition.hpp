#pragma once
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "Causal.hpp"
#include "Types.hpp"

namespace simlab {

// Copy-on-write container wrapper: copying a Cow shares the payload, the
// first MUTATING access on a shared payload detaches (deep-copies) it.
// Speculative world clones (planning) copy AgentMinds thousands of times per
// second; memory snapshots and visited cells are read-only there, so the
// clone costs one shared_ptr copy instead of ~thousands of heap allocations.
template <class C>
class Cow {
public:
    using iterator = typename C::iterator;
    using const_iterator = typename C::const_iterator;
    Cow() : p_(std::make_shared<C>()) {}
    // const (read) access never detaches
    std::size_t size() const { return p_->size(); }
    bool empty() const { return p_->empty(); }
    const_iterator begin() const { return p_->begin(); }
    const_iterator end() const { return p_->end(); }
    template <class K> const_iterator find(const K& k) const { return p_->find(k); }
    template <class K> std::size_t count(const K& k) const { return p_->count(k); }
    template <class K> const auto& at(const K& k) const { return p_->at(k); }
    // mutating access detaches first
    iterator begin() { return m().begin(); }
    iterator end() { return m().end(); }
    template <class K> iterator find(const K& k) { return m().find(k); }
    template <class K> auto& operator[](const K& k) { return m()[k]; }
    template <class... A> auto emplace(A&&... a) { return m().emplace(std::forward<A>(a)...); }
    iterator erase(iterator it) { return m().erase(it); }
    void clear() { m().clear(); }
private:
    C& m() {
        if (p_.use_count() > 1) p_ = std::make_shared<C>(*p_);
        return *p_;
    }
    std::shared_ptr<C> p_;
};


class World;
struct Entity;
struct InteractionContext;
struct CausalEvent;

// ---- Cognitive layer (TODO §10, §9.10) ----------------------------------------
// Agent knowledge is SEPARATE from World Truth: an AgentMind only holds what
// the NPC could observe or learn. Nothing here writes back into entities —
// minds influence behavior only by choosing which interactions to attempt.

// What the NPC can see of an entity: identity, position, and ONLY props whose
// PropertyDef domain is "observable" (e.g. `exposed`). Truth props like
// deposit composition/reserve are never copied.
struct Observation {
    EntityId entity = kNoEntity;
    std::uint64_t tick = 0;
    Vec2 pos;
    std::map<PropId, Value> props;      // observable-domain props only
};

enum class BeliefStatus : std::uint8_t {
    Unknown = 0, Observed, Hypothesized, Tested, Supported, Verified, Disputed
};

// A belief is a directional claim the NPC holds, with confidence 0..1000 and
// an evidence trail. `key` is stable: e.g. "outcome:dig->exposed:deposit".
// `self_evidence` counts only evidence gathered from the NPC's OWN actions —
// witnessed events reinforce `evidence_for` but never `self_evidence`, which
// is what lets Verified mean "personally tested many times" (§10.2).
struct Belief {
    std::string key;
    BeliefStatus status = BeliefStatus::Unknown;
    std::int64_t confidence = 0;        // permille
    std::int64_t evidence_for = 0;
    std::int64_t evidence_against = 0;
    std::int64_t self_evidence = 0;     // subset of evidence_for, own actions only
    std::uint64_t last_tick = 0;
};

// ---- discovery records (TODO §11.5) -------------------------------------------
// A DiscoveryRecord is the verification trail of one hypothesis the NPC
// tested in the REAL world: which causal events were its experiments, which
// laws supported the outcomes, and the reliability evidence gathered. It is
// agent knowledge — it never writes into World Truth.
enum class DiscoveryStatus : std::uint8_t {
    Open = 0, Tested, Supported, Verified, Refuted
};

// Ebbinghaus memory trace (Nhịp 1): every remembered thing — an entity
// snapshot or a discovery record — carries clarity (0..max), stability
// (forgetting resistance), emotional charge (-1000..+1000, the amygdala
// hook), rehearsal bookkeeping and a sub-drop accumulator. Forgetting is
// the integer-exact `accum += dt*rate; drop = accum/stability` recurrence —
// batch-exact for any dt, so dormant LOD minds decay identically.
struct MemoryEngram {
    std::int64_t clarity = 0;             // 0 = forgotten (entry is erased)
    std::int64_t stability = 1;           // 1..max_stability
    std::int64_t emotional_charge = 0;    // -1000..+1000
    std::int64_t rehearsal_count = 0;
    std::int32_t decay_accum = 0;         // remainder below one clarity drop
    std::uint64_t last_rehearsal = 0;     // tick of last stability boost
};

// SpeciesMindParams — neural tuning of one mind. Read from `mind_*`
// properties on the entity (species/material defaults, genome-expressed
// per individual); absent props fall back to the spec defaults below.
// 100% data: a planet of crystal minds just edits properties.txt.
struct MindParams {
    std::int64_t base_clarity = 400;
    std::int64_t max_clarity = 1000;
    std::int64_t base_stability = 15;
    std::int64_t max_stability = 10000;
    std::int64_t decay_rate = 10;
    std::int64_t imprint_threshold = 500;
    std::int64_t imprint_base_stability = 2000;
    std::int64_t amygdala_multiplier = 5;
    std::int64_t rehearsal_stability_bonus = 50;
    std::int64_t detail_blur_threshold = 300;
    std::int64_t sleep_prune_threshold = 50;
    std::int64_t rehearsal_interval = 100;
    // Nhịp 2-3: four heritable neural axes + epigenetic thresholds.
    // Axes default 1000 = neutral; every "disorder" is a phase-shift of
    // these numbers, never a named flag.
    std::int64_t threat_gain = 1000;
    std::int64_t reward_gain = 1000;
    std::int64_t pattern_coupling = 1000;
    std::int64_t empathy_coupling = 1000;
    std::int64_t low_light_bias = 0;
    std::int64_t isolation_bias = 0;
    std::int64_t executive_override = 750;
    std::int64_t emotion_decay = 10;         // permille/tick for panic/rage/grief
    std::int64_t trauma_charge = 850;        // |charge| counted as a shock
    std::int64_t inherit_charge = 900;       // |charge| needed to transmit
    std::int64_t inherit_stability = 9000;   // stability needed to transmit
    std::int64_t mark_clarity = 250;
    std::int64_t mark_stability = 1500;
    std::int64_t mark_charge_permille = 300; // child charge = parent * x/1000
    std::int64_t flashback_charge = 800;     // |charge| trauma that can flash back
    std::int64_t flashback_stability = 7000; // stability floor for flashback
    std::int64_t paranoia_coupling = 1600;   // pattern_coupling for false memories
    std::int64_t paranoia_panic = 500;       // panic floor for false memories
    std::int64_t paranoia_charge = 700;      // |charge| written on innocents
    // Nhịp 4: heritable cognitive aptitudes — continuous axes, never
    // talent labels. plan_horizon = lookahead depth (1..6); the other
    // three are permille scales around neutral 1000; brain cost is the
    // species' milliJ/tick price per unit of cognitive surplus.
    std::int64_t plan_horizon = 2;
    std::int64_t abstraction_gain = 1000;
    std::int64_t motor_precision = 1000;
    std::int64_t theory_of_mind = 1000;
    std::int64_t brain_metabolic_cost = 15;
    // Nhịp 4.5: social bonding axes — pair-bond formation, jealousy
    // sensitivity, parental imprint strength. All heritable, all neutral
    // at 1000; zero parental_drive literally means "walks away".
    std::int64_t pair_bond_gain = 1000;
    std::int64_t jealousy_gain = 1000;
    std::int64_t parental_drive = 1000;
    // Nhịp 5: primitive social psychology — conformity pressure,
    // out-group wariness, ego-defense channel selection.
    std::int64_t conformity_gain = 1000;
    std::int64_t xenophobia_bias = 1000;
    std::int64_t ego_defense_gain = 1000;
};

// Dynamic neuro-emotional state (Nhịp 2-3): the four axes AS THIS MIND
// CURRENTLY RUNS THEM — copied once from `mind_*` props (genes/species),
// then shifted permanently by allostatic trauma. panic/rage/grief are
// transient states decaying linearly (batch-exact under any dt).
// `tuned` marks whether the prop copy already happened.
struct NeuroState {
    bool tuned = false;
    std::int64_t threat_gain = 1000;
    std::int64_t reward_gain = 1000;
    std::int64_t pattern_coupling = 1000;
    std::int64_t empathy_coupling = 1000;
    std::int64_t low_light_bias = 0;
    std::int64_t isolation_bias = 0;
    std::int64_t executive_override = 750;
    std::int64_t panic = 0;              // 0..1000 acute fear
    std::int64_t rage = 0;               // 0..1000 acute anger
    std::int64_t grief = 0;              // 0..1000 acute loss
    std::int64_t panic_at_event = 0;     // panic sampled at last causal event
    std::uint64_t last_event_tick = 0;   // for relief bookkeeping
};

// An epigenetically inherited trauma mark (Nhịp 3): a vague, kind/material-
// keyed engram the child is BORN with. No entity id exists yet — matching
// happens by material/kind at flashback time. Decays like any engram.
struct BornEngram {
    MemoryEngram engram;
    MaterialId material = kNoMaterial;
    std::string kind_name;
};

struct AgentMind;
MindParams mind_params(const World& w, const Entity& e);
void rehearse_engram(MemoryEngram& e, const MindParams& p, std::uint64_t tick);
void sleep_consolidate(AgentMind& mind, const MindParams& p);

// ---- Nhịp 2-3 neuro services ---------------------------------------------------
// Lazy prop->axis copy: first mutation/read-with-write touches tune the
// NeuroState from the entity's `mind_*` props exactly once.
void neuro_touch(World& w, EntityId npc);
const NeuroState& neuro_of(const AgentMind& m);   // defaults when untuned
// Allostatic shift: a shock at/below -trauma_charge permanently bends the
// axes AWAY from their genetic baseline, scaled by the genome (high
// threat_gain -> panic-prone, high pattern_coupling -> paranoid, high
// empathy + kin death -> anhedonia). Deterministic per (npc, shock).
void allostatic_shift(World& w, AgentMind& mind, EntityId npc,
                      std::int64_t shock, bool kin_died);
// Spurious association: paranoid coupling writes a false negative engram
// onto a neutral bystander near a shock event.
void maybe_spurious_association(World& w, AgentMind& mind, EntityId npc,
                                const CausalEvent& ev, std::int64_t shock);
// Compulsion bonus for plan scoring: actions that relieved panic before.
std::int64_t compulsion_bonus(const AgentMind& mind, const std::string& action);
// Bond/imprint-driven utility bonus for plan scoring (Nhịp 4.5).
std::int64_t bond_bonus(const World& w, const AgentMind& mind,
                        const InteractionContext& c);
// Inferred welfare of a bonded engram target (0..1000): ToM mood
// confidence + imprint depth + OBSERVABLE snapshot signs only — private
// need props of others are never read.
std::int64_t bonded_distress(const World& w, const AgentMind& mind,
                             EntityId eid);
// Herd pressure (Nhịp 5a): bonus when >=3 familiar/collective conspecifics
// recently performed the same action — conformity_gain x theory_of_mind,
// discounted by abstraction_gain (independent minds resist the herd).
std::int64_t conformity_bonus(const World& w, EntityId npc,
                              const AgentMind& mind,
                              const std::string& action);
// Sublimated-pain craft drive (Nhịp 5c): grief/rage partially transmuted
// into bonus for expressive actions, scaled by accumulated relief.
std::int64_t sublimation_bonus(const AgentMind& mind,
                               const std::string& action);

struct DiscoveryRecord {
    std::string key;                     // stable hypothesis id (MethodKey)
    std::string claim;                   // what the NPC set out to test
    std::vector<EventId> experiments;    // causal events produced by trials
    std::vector<std::string> supporting_laws;  // law model names that fired
    // validConditions (§11.5): condition boundaries that refused/f failed
    // trials, deduped by `what` keeping the strictest `required`. A Refuted
    // record unlocks for retest when a new candidate's predicted conditions
    // satisfy these boundaries (better tool, more heat, more force).
    std::vector<Condition> failed_conditions;
    std::vector<EntityId> sample_entities;   // provenance: entities tried (cap 8)
    std::int64_t evidence_for = 0;
    std::int64_t evidence_against = 0;
    std::uint64_t trials = 0;
    DiscoveryStatus status = DiscoveryStatus::Open;
    std::uint64_t last_tick = 0;
    MemoryEngram engram;                  // Ebbinghaus trace of this skill
};

// One witnessed act by another agent — the raw material of herd behavior.
struct WitnessedAct {
    std::uint64_t tick = 0;
    EntityId actor = kNoEntity;
    std::string action;
};

struct AgentMind {
    NeuroState neuro;                             // Nhịp 2-3 dynamic state
    std::map<std::string, std::int64_t> panic_relief;  // action -> relief evidence
    std::vector<BornEngram> born_memories;        // epigenetic marks
    // Nhịp 5: bounded ring of others' recently witnessed actions (feeds
    // conformity) and sublimated-pain relief bookkeeping (feeds craft-drive).
    std::deque<WitnessedAct> recent_witnessed;
    std::map<std::string, std::int64_t> sublimation_relief;
    std::int64_t ostracism_accum = 0;             // Euclid remainder (LOD)
    std::int64_t political_accum = 0;             // defer/grievance Euclid remainder (LOD)
    std::deque<Observation> observations;          // FIFO, capped
    std::map<std::string, Belief> beliefs;
    std::map<std::string, DiscoveryRecord> discoveries;  // §11.5 records
    std::set<std::string> known_actions;            // verbs learned/witnessed
    std::set<EntityId> known_entities;              // entities ever observed

    // Fog-of-war memory (Phase 9): the ONLY source planners may use for
    // entities outside sight range. A snapshot records what the mind last
    // perceived — pos/material/kind/observable props — never live World
    // Truth. Stale by design: revisiting an area refreshes it.
    struct MemorySnapshot {
        std::uint64_t tick = 0;
        Vec2 pos{};
        MaterialId material = kNoMaterial;
        std::string kind_name;
        std::map<PropId, Value> props;              // observable props only
        MemoryEngram engram;                        // Ebbinghaus trace
    };
    Cow<std::map<EntityId, MemorySnapshot>> memory;
    // Global terrain cells this mind has stood in — per-agent frontier
    // record (exploration memory lives in the mind, never in World Truth).
    Cow<std::set<std::pair<int, int>>> visited_cells;

    static constexpr std::size_t kMaxObservations = 64;
    // No kMaxMemory: engrams self-release when clarity hits 0 (Ebbinghaus).

    void observe(Observation o);
    void learn_action(const std::string& a) { known_actions.insert(a); }
    Belief& belief(const std::string& key);
    const Belief* find_belief(const std::string& key) const;
    // confidence update rules (TODO §10.6): success raises, failure lowers.
    // `self` distinguishes the NPC's own experiment from witnessed evidence.
    void reinforce(const std::string& key, std::int64_t delta, std::uint64_t tick,
                   bool self = true);
    void weaken(const std::string& key, std::int64_t delta, std::uint64_t tick);
};

// ---- cognition services --------------------------------------------------------
// Perception: entities within the NPC's `sight_range` become Observations.
// Returns count added; also records them in the NPC's mind.
std::size_t perceive(World& w, EntityId npc);

// Learning from the causal ledger: after an interaction commits, the actor's
// mind records (a) an observation of the event, (b) a learned action verb,
// (c) outcome beliefs keyed by action+prop for each changed property, and
// (d) yield beliefs for every entity the event spawned.
void learn_from_event(World& w, EntityId npc, std::uint64_t event_id);

// Witness learning (TODO §10.1/§12.4 boundary): an agent that could see the
// event learns the verb and WEAK outcome/yield evidence (self=false). The
// world does not care who watched — only that knowledge has a source.
void witness_from_event(World& w, EntityId npc, const CausalEvent& ev);

// ---- Ebbinghaus memory services (Nhịp 1) ---------------------------------------
// upsert_memory: salience-encoded write into the entity memory map. New
// engrams initialise from emotional_charge (imprint vs base); existing
// ones rehearse (clarity refresh + throttled stability doubling).
void upsert_memory(AgentMind& mind, EntityId eid,
                   const AgentMind::MemorySnapshot& snap,
                   std::int64_t emotional_charge, const MindParams& p);
// Semantic drift: retrieval-time blur. Below detail_blur_threshold the
// remembered position jitters by a deterministic hash of
// (entity, world_seed, clarity) and integer props quantize to coarse
// buckets — emotion survives, detail fades. Truth is never mutated.
Vec2 recalled_pos(const AgentMind::MemorySnapshot& s, EntityId eid,
                  std::uint64_t world_seed, std::int64_t blur_threshold);
// Blurred prop lookup: returns the snapshot's value, quantized when the
// engram is below the blur threshold. `fallback` used when absent.
Value recalled_prop(const AgentMind::MemorySnapshot& s, PropId p,
                    EntityId eid, std::uint64_t world_seed,
                    std::int64_t blur_threshold, const Value& fallback);

// A goal names a desired end state. Two kinds:
//   PropValue   — a prop on a target entity reaches `value` (target may be
//                 kNoEntity = any known entity).
//   SatisfyNeed — the NPC's own `need` prop drops to <= `value` (target is
//                 the NPC itself; produced by derive_goals).
struct Goal {
    enum class Kind : std::uint8_t { PropValue = 0, SatisfyNeed };
    Kind kind = Kind::PropValue;
    PropId prop = kNoProp;              // PropValue: desired property on target
    std::int64_t value = 0;             // PropValue: desired payload; SatisfyNeed: satisfied level
    EntityId target = kNoEntity;        // kNoEntity = any known entity
    PropId need = kNoProp;              // SatisfyNeed: which need prop to reduce
};

// Needs & self-directed goals (TODO §10.1): derive_goals reads the NPC's own
// body-state props (hunger/thirst/fatigue — domain "need", never observable
// to others) and emits SatisfyNeed goals for needs above the urgency
// threshold, most urgent first. An NPC with no needs gets no goals — it does
// not invent motivations.
std::vector<Goal> derive_goals(const World& w, EntityId npc);

// ---- planning (TODO §10.4) ----------------------------------------------------

// Depth-1 planner: score every generic action x known entity via the law
// models' predict() (forward model — predict never mutates World Truth) and
// return ranked candidate contexts, best first. Only actions in
// known_actions are tried: an NPC cannot plan what it has never seen/done.
std::vector<InteractionContext> plan(const World& w, EntityId npc,
                                     const Goal& goal, std::size_t max_results = 4);

// A candidate multi-step plan found by speculative search.
struct Plan {
    std::vector<InteractionContext> steps;   // execute in order via interact()
    std::int64_t score = 0;                  // higher is better
    bool satisfied = false;                  // goal reached within the horizon
};

// Multi-step speculative planner (TODO §10.4/§10.6): beam search over
// World::clone() copies. Each node simulates candidate actions through the
// REAL interact() pipeline on the clone — laws decide what is physically
// possible, minds on the clone learn from simulated outcomes, and spawned
// entities join the NPC's known set (it sees what it makes). Candidate
// ranking combines goal progress with belief-guided scoring: Supported/
// Verified outcome beliefs (including analogical transfer across similar
// materials) reward a candidate, Disputed beliefs penalize it.
// max_depth bounds plan length; beam_width bounds nodes expanded per level.
// Deterministic: candidates iterate sorted sets, ties keep generation order.
std::vector<Plan> plan_multi(const World& w, EntityId npc, const Goal& goal,
                             int max_depth = 3, std::size_t beam_width = 6,
                             std::size_t max_results = 4);

// Social consequences of a committed causal event (Phase 8, TODO §12.3-13).
// Called by World::interact AFTER the event is ledgered — it mutates MINDS
// and social tie weights only, never entity props:
//   - "communicate": actor's Supported+ discovery records + known_actions
//     flow to the partner as UNVERIFIED Open claims (self_evidence stays 0 —
//     the learner must re-verify in the real world). Volume scales with the
//     actor's `trust` tie toward the partner.
//   - "read": records carved in the target's `inscription` prop enter the
//     actor's mind the same way — Recorded Knowledge survives its author.
//   - member-conflict rule: when the actor's action damaged an entity that
//     shares a `collective` with `taboo_harm`, every member's trust in the
//     actor drops — rules are props on the group, enforcement is causal.
void social_from_event(World& w, EntityId actor,
                       const std::vector<EntityId>& targets,
                       const CausalEvent& ev);


// Phase 12.2: inductive economy — all valuations are SUBJUCTIVE (viewer
// needs, pa:<mat> abundance beliefs, community monetary consensus, material
// physics). No price table exists anywhere.

std::int64_t econ_abundance(const AgentMind* m, MaterialId mat);
std::int64_t econ_consensus(const World& w, EntityId viewer, MaterialId mat);
std::int64_t econ_use_value(const World& w, EntityId viewer, MaterialId mat);
std::int64_t econ_exchange_value(const World& w, EntityId viewer,
                                 MaterialId mat);
std::int64_t econ_total_value(const World& w, EntityId viewer,
                              MaterialId mat);
// get-qty of `get` a fair trade demands for `give_qty` of `give`.
std::int64_t econ_required_qty(const World& w, EntityId viewer,
                               MaterialId give, std::int64_t give_qty,
                               MaterialId get);
EntityId econ_held_stack(const World& w, EntityId agent, MaterialId mat,
                         std::int64_t need);
bool econ_is_store_of_value(const World& w, MaterialId mat);
// Returns the updated consensus text after a store-of-value trade
// (pure function — predict commits it as a StateChange).
std::string econ_consensus_bump_text(const std::string& cur,
                                     MaterialId mat, std::int64_t qty);
void econ_from_event(World& w, EntityId actor,
                     const std::vector<EntityId>& targets,
                     const CausalEvent& ev);
std::int64_t econ_aptitude_bonus(const World& w, EntityId npc,
                                 const AgentMind& mind,
                                 const InteractionContext& c);

// ---- Phase 12.3: political & legal base --------------------------------------
// Communal political state lives on the `language` registry entity as text
// ledgers (norm_table / ostracism_table), mirroring monetary_consensus.
// Personal political state (defer_to:<id>, grievance) lives in beliefs —
// both serialize with the mind and decay LOD-exact inside political_drift.
EntityId pol_community_of(const World& w, EntityId agent);
PropId pol_pid(const World& w, const char* name);
std::int64_t pol_prop(const World& w, const Entity& e, const char* name,
                      std::int64_t fallback);
std::int64_t pol_legitimacy(const World& w, EntityId agent);   // absent = 500
std::int64_t pol_coercive(const World& w, EntityId agent);     // computed
std::int64_t pol_grievance(const AgentMind* m);                // belief, 0..1000
std::int64_t pol_pool(const World& w, EntityId agent);         // prop, 0..1e9
std::int64_t pol_norm_aversion(const World& w, EntityId viewer,
                               const std::string& action);     // 0..1000
std::int64_t pol_ostracism(const World& w, EntityId viewer,
                           EntityId agent);                    // 0..1000
std::int64_t pol_defer_conf(const AgentMind& m, EntityId leader);
// recompute one mind's defer_to:<top> belief toward the community's most
// influential member (legitimacy vs coercion weighted by empathy/fear).
void pol_update_defer(World& w, EntityId mind_id);
// does `agent` currently hold the community's defer mandate? (top count)
bool pol_holds_mandate(const World& w, EntityId agent);
// strongest communal taboo: fills action key + aversion permille (0..1000).
bool pol_dominant_norm(const World& w, EntityId viewer,
                       std::string* action, std::int64_t* aversion);
std::string pol_table_bump_text(const std::string& cur,
                                const std::string& key,
                                std::int64_t delta_permille);
std::int64_t pol_table_get(const std::string& cur,
                           const std::string& key);
std::int64_t pol_defer_count(const World& w, EntityId leader);
void political_from_event(World& w, EntityId actor,
                          const std::vector<EntityId>& targets,
                          const CausalEvent& ev);

}  // namespace simlab
