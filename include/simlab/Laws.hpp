#pragma once
#include <array>
#include <memory>
#include <string>
#include <vector>
#include "Causal.hpp"
#include "Entity.hpp"
#include "Types.hpp"
#include "Brain.hpp"

namespace simlab {

// ---- Natural law system -----------------------------------------------------
// Laws are modular domain solvers behind one interface (see TODO §4):
//   supports()  — can this model handle this interaction?
//   predict()   — compute outcome from World Truth (never from beliefs)
//   apply()     — commit the prediction's changes and emit causal events
//
// A law model reads PROPERTIES and STRUCTURE, never entity names. "Stone is
// hard" is data; "stone breaks wood" is a law. If an agent does not know fire
// burns wood, the law still fires — World Truth is independent of belief.

struct ActionParameters {
    std::string action;                  // e.g. "impact" — physical, not a product name
    std::int64_t force = 0;              // milli-units of applied effort/energy
    std::int64_t ticks = 1;              // simulated time the action consumes
    std::string note;                    // free-form caller context (tools, provenance)
    Vec2 destination{0, 0};              // locomotion target (Phase 9 driver)
    bool has_destination = false;
};

struct InteractionContext {
    EntityId actor = kNoEntity;
    std::vector<EntityId> targets;
    ActionParameters parameters;
    std::vector<EventId> parent_events;  // causal parents supplied by the caller
};

// A new entity a law wants created on commit (extracted ore, processed
// goods, waste). Laws propose; World::interact commits atomically with the
// prop changes. Props are instance overrides validated at spawn time.
struct SpawnSpec {
    std::string kind;
    MaterialId material = kNoMaterial;
    Vec2 pos;
    PropertyBag props;
    Genome genome;                        // offspring inherits recombined loci
    // Relations to wire once the spawn gets its id: each entry means
    // relate(other, type, new_entity) — e.g. ("part_of", part) makes the
    // spawned composite the target of the part's containment edge.
    std::vector<std::pair<std::string, EntityId>> rel_from;
    // Epigenetic inheritance (Nhịp 3): trauma marks a parent transmits to
    // this spawn — material/kind signature + signed emotional charge. The
    // spawn-apply path turns them into the child's born_memories engrams.
    struct LegacyEngram {
        MaterialId material = kNoMaterial;
        std::string kind_name;
        std::int64_t charge = 0;
    };
    std::vector<LegacyEngram> legacy;
    // CL-1 Phase 2 (Baldwin): inherited brain — slow weights + hyper-genes
    // only. Fast weights/h/replay always start empty on the newborn.
    // Invalid/empty -> the spawn gets a hash-initialized brain lazily.
    BrainGenome brain;
    // Open-ended invention: when has_synth is set the spawn's material does
    // not exist yet — World::interact materializes it from this descriptor
    // at commit time (synth_id -> MaterialDef in the registry + a
    // SynthRecord for the catalog). `material` stays kNoMaterial meanwhile.
    struct SynthSpec {
        std::uint64_t synth_id = 0;
        std::string verb;
        MaterialId parent_a = kNoMaterial;
        MaterialId parent_b = kNoMaterial;
        std::int64_t temp_band = 0;
        std::array<std::int64_t, 8> attrs{};
    };
    SynthSpec synth;
    bool has_synth = false;
};

// A proposed entity relocation — first-class causal change alongside
// RelationChange: locomotion moves WHOLE entities, which is neither a prop
// change nor a relation edge. Committed atomically with the event.
struct MoveSpec {
    EntityId entity = kNoEntity;
    Vec2 to{0, 0};
};

// A proposed relationship-graph mutation — first-class causal change, so
// structure (part_of, located_in, ...) is inspectable in the ledger just
// like property changes. Committed atomically with the rest of the event.
struct RelationChange {
    EntityId a = kNoEntity;
    std::string type;
    EntityId b = kNoEntity;
    bool add = true;                    // false = remove the edge
};

struct PredictionResult {
    LawId law = 0;
    std::string law_name;
    bool possible = false;
    std::vector<StateChange> changes;    // proposed, not yet committed
    std::vector<RelationChange> rel_changes;  // proposed relation mutations
    std::vector<MoveSpec> moves;            // proposed relocations (locomotion)
    std::vector<SpawnSpec> spawns;       // entities to create on commit
    std::int64_t ticks = 0;              // simulated duration; 0 = use ctx ticks
    std::vector<std::string> notes;      // derivation steps for `evidence`
    std::string failure;                 // why the interaction cannot proceed
    std::vector<Condition> conditions;   // thresholds checked (recorded in the event)
};

struct InteractionResult {
    bool completed = false;
    LawId law_used = 0;
    EventId event = kNoEvent;
    std::vector<StateChange> changes;
    std::vector<EntityId> spawned;        // entities created by law spawns
    std::string failure;                 // set when no law could apply
    EventId failed_event = kNoEvent;     // ledger record of a refused attempt (evidence)
};

class INaturalLawModel {
public:
    virtual ~INaturalLawModel() = default;
    virtual LawId id() const = 0;
    virtual const char* name() const = 0;
    virtual std::vector<std::string> domains() const = 0;   // property domains it reads
    virtual bool supports(const World& w, const InteractionContext& ctx) const = 0;
    virtual PredictionResult predict(const World& w, const InteractionContext& ctx) const = 0;
    // Law models are stateless domain solvers; clone() lets World::clone()
    // build a speculative copy for planning without sharing mutable state.
    virtual std::unique_ptr<INaturalLawModel> clone() const = 0;
};

// ---- Physics model (first domain solver) ------------------------------------
// Handles action="impact": a moving/struck entity transfers force into a
// target. Outcome derives from target properties:
//   integrity  0..1000  structural soundness; 0 = destroyed
//   hardness   0..1000  resistance to deformation
//   brittleness 0..1000 tendency to crack rather than deform (higher = shatters easier)
//   mass       grams    inertia; heavier targets absorb more
// Damage is computed, never scripted per material: obsidian and dry wood
// behave differently because their DATA differs.
class PhysicsModel final : public INaturalLawModel {
public:
    LawId id() const override;
    const char* name() const override { return "physics.impact"; }
    std::vector<std::string> domains() const override { return {"physical"}; }
    bool supports(const World& w, const InteractionContext& ctx) const override;
    PredictionResult predict(const World& w, const InteractionContext& ctx) const override;
    std::unique_ptr<INaturalLawModel> clone() const override;
};

// ---- Extraction model (TODO §9.4) ---------------------------------------------
// Generic physical extraction ops: "dig", "chop", "strike". No per-material
// actions — results derive from deposit composition, exposed/min_force/
// dig_resistance/yield_rate/deposit_remaining props, and material physics.
// Buried deposits must be dug exposed before other actions yield anything.
class ExtractionModel final : public INaturalLawModel {
public:
    LawId id() const override;
    const char* name() const override { return "resource.extract"; }
    std::vector<std::string> domains() const override { return {"physical"}; }
    bool supports(const World& w, const InteractionContext& ctx) const override;
    PredictionResult predict(const World& w, const InteractionContext& ctx) const override;
    std::unique_ptr<INaturalLawModel> clone() const override;
};

// ---- Processing model (TODO §9.5) ----------------------------------------------
// action="process", parameters.note = process name from processes.txt.
// Process defs are DATA: required input materials+quantities, fuel,
// min_temperature (deci-°C on any participant), ticks, outputs and waste.
// Fails when inputs/fuel/temperature conditions are unmet; mass is accounted
// via waste outputs so nothing is created from nothing.
class ProcessingModel final : public INaturalLawModel {
public:
    LawId id() const override;
    const char* name() const override { return "resource.process"; }
    std::vector<std::string> domains() const override { return {"physical"}; }
    bool supports(const World& w, const InteractionContext& ctx) const override;
    PredictionResult predict(const World& w, const InteractionContext& ctx) const override;
    std::unique_ptr<INaturalLawModel> clone() const override;
};

// ---- Composition model ---------------------------------------------------------
// action="assemble": generic A+B+... -> composite structure. NOT a recipe
// table — the solver checks feasibility from properties (reach, energy,
// stability, load) and derives the product's properties from its parts:
//   sum:    mass, thermal_mass, length
//   min:    integrity (weakest link governs)
//   max:    flammability, edge
//   wavg:   hardness, brittleness, conductivity, temperature (by mass)
// Emergent: `thrust_range` appears only on the assembled structure when it
// has both edge and length — neither property exists on the parts alone.
class CompositionModel final : public INaturalLawModel {
public:
    LawId id() const override;
    const char* name() const override { return "physical.compose"; }
    std::vector<std::string> domains() const override { return {"physical"}; }
    bool supports(const World& w, const InteractionContext& ctx) const override;
    PredictionResult predict(const World& w, const InteractionContext& ctx) const override;
    std::unique_ptr<INaturalLawModel> clone() const override;
};

// ---- Thermal model -------------------------------------------------------------
// action="heat": raises target `temperature` (deci-°C) by E/(100*thermal_mass);
// flammable targets crossing 300°C ignite (integrity loss scaled by
// flammability). Property-driven — feeds ProcessingModel's min_temperature.
class ThermalModel final : public INaturalLawModel {
public:
    LawId id() const override;
    const char* name() const override { return "physics.heat"; }
    std::vector<std::string> domains() const override { return {"physical"}; }
    bool supports(const World& w, const InteractionContext& ctx) const override;
    PredictionResult predict(const World& w, const InteractionContext& ctx) const override;
    std::unique_ptr<INaturalLawModel> clone() const override;
};

// ---- Physiology model (TODO §10.1 needs) --------------------------------------
// Agent body-state solver. Two generic actions, fully property-driven:
//   "consume" — eat/drink a target carrying `nutrition`/`hydration` props
//               (permille of the matching need relieved per unit). Decrements
//               `quantity` or consumes the whole entity; relieves the actor's
//               `hunger`/`thirst` need props. Nothing names a food.
//   "rest"    — actor-only recovery: `fatigue` drops by `rest_recovery`
//               permille per tick. Requires the actor to carry `fatigue`.
// Needs themselves (hunger/thirst/fatigue) grow with `metabolism` via
// World::advance_ticks — recorded in the causal ledger as "metabolize".
class PhysiologyModel final : public INaturalLawModel {
public:
    LawId id() const override;
    const char* name() const override { return "physiology.metabolism"; }
    std::vector<std::string> domains() const override { return {"need", "biological"}; }
    bool supports(const World& w, const InteractionContext& ctx) const override;
    PredictionResult predict(const World& w, const InteractionContext& ctx) const override;
    std::unique_ptr<INaturalLawModel> clone() const override;
};

// Primitive medicine (Phase 10.2): one generic verb, zero recipe table.
//   "treat" — actor applies targets[1] (a material/entity) onto targets[0]
//   (the patient: self or another organism). The material's PHYSICAL props
//   decide the effect — absorbency dresses wounds, antiseptic fights
//   infection, temperature>=50C cauterizes. No herb names, no formulas:
//   a material is a dressing because its numbers say so.
class TreatModel final : public INaturalLawModel {
public:
    LawId id() const override;
    const char* name() const override { return "physiology.treat"; }
    std::vector<std::string> domains() const override { return {"biological", "physical"}; }
    bool supports(const World& w, const InteractionContext& ctx) const override;
    PredictionResult predict(const World& w, const InteractionContext& ctx) const override;
    std::unique_ptr<INaturalLawModel> clone() const override;
};

// Biological reproduction & heredity (TODO §8.4/§12.1). One generic action:
//   "reproduce" — actor + one partner target. Both must be living organisms
//   with compatible genomes (same kind + identical locus set), adult
//   (`age` >= `adult_age`), fertile (`fertility` > 0) and not starving
//   (`hunger` < 900 when the prop exists). The offspring's genome is built by
//   deterministic recombination — each locus picks one parent's allele via a
//   pure hash of (world seed, parent ids, tick, locus) — plus mutation at the
//   actor's `mutation_rate` permille. No RNG state, no species names, no
//   scripted fitness: selection is what the environment does to carriers.
class BiologyModel final : public INaturalLawModel {
public:
    LawId id() const override;
    const char* name() const override { return "biology.reproduction"; }
    std::vector<std::string> domains() const override { return {"biological"}; }
    bool supports(const World& w, const InteractionContext& ctx) const override;
    PredictionResult predict(const World& w, const InteractionContext& ctx) const override;
    std::unique_ptr<INaturalLawModel> clone() const override;
};

// Social & cultural evolution (TODO §12.3/§12.4/§13). Generic actions only —
// no Tribe/Kingdom/Religion classes anywhere:
//   communicate — actor transfers knowledge to one partner (mind->mind,
//                 trust-scaled, post-commit via social_from_event)
//   engrave    — actor carves one of ITS Supported+ discovery records onto
//                a target entity's `inscription` prop (Recorded Knowledge)
//   read       — actor studies an inscribed entity; records enter its mind
//                post-commit as unverified claims (post-commit hook)
//   found      — actor charters a collective entity (member_of self)
//   join/leave — manage member_of edges to a `collective` entity
//   share      — move the actor's `quantity` to a target (common store)
// Group RULES are data: a collective carrying `taboo_harm` makes
// member-caused integrity loss a trust violation (social_from_event).
class SocialModel final : public INaturalLawModel {
public:
    LawId id() const override;
    const char* name() const override { return "social.interaction"; }
    std::vector<std::string> domains() const override { return {"social"}; }
    bool supports(const World& w, const InteractionContext& ctx) const override;
    PredictionResult predict(const World& w, const InteractionContext& ctx) const override;
    std::unique_ptr<INaturalLawModel> clone() const override;
};

// ---- Locomotion model (Phase 9) ----------------------------------------------
// Generic movement: action="move" with ActionParameters::destination set.
// Step length derives from the actor's `speed` prop (data, not code) and the
// action's ticks; terrain water blocks entry; energy is charged per unit
// travelled. No per-species movement code — a fast animal simply has a
// bigger `speed` number in its data.
class LocomotionModel final : public INaturalLawModel {
public:
    LawId id() const override;
    const char* name() const override { return "physics.locomotion"; }
    std::vector<std::string> domains() const override { return {"physical", "biological"}; }
    bool supports(const World& w, const InteractionContext& ctx) const override;
    PredictionResult predict(const World& w, const InteractionContext& ctx) const override;
    std::unique_ptr<INaturalLawModel> clone() const override;
};

// ---- Material combination (Phase 11) -----------------------------------------
// Generic "combine": >=2 reachable targets; entities carrying
// `antimicrobial` >= 500 sterilize their siblings (cooked_state 1000, strain_ref
// and contamination cleared); entities carrying `burial_medium` >= 500 bury
// dead siblings (buried 1, odor silenced). No material or species is named —
// salt preserves because its data says so, soil buries because its data says so.
class CombineModel final : public INaturalLawModel {
public:
    LawId id() const override;
    const char* name() const override { return "physical.combine"; }
    std::vector<std::string> domains() const override { return {"physical", "biological"}; }
    bool supports(const World& w, const InteractionContext& ctx) const override;
    PredictionResult predict(const World& w, const InteractionContext& ctx) const override;
    std::unique_ptr<INaturalLawModel> clone() const override;
};

// ---- Open-ended invention ------------------------------------------------------
// SmeltModel (id 24) sits BEFORE ThermalModel: bare "heat" on a target already
// at/above its material's smelt_temp synthesizes a smelted product instead of
// just warming it. SynthesisModel (id 25) is LAST: "combine" with >=2 material
// targets and "process" naming no authored recipe blend inputs' physical
// attribute vectors into a brand-new MaterialDef (registered at commit).
class SmeltModel final : public INaturalLawModel {
public:
    LawId id() const override;
    const char* name() const override { return "physical.smelt"; }
    std::vector<std::string> domains() const override { return {"physical"}; }
    bool supports(const World& w, const InteractionContext& ctx) const override;
    PredictionResult predict(const World& w, const InteractionContext& ctx) const override;
    std::unique_ptr<INaturalLawModel> clone() const override;
};

class SynthesisModel final : public INaturalLawModel {
public:
    LawId id() const override;
    const char* name() const override { return "physical.synthesize"; }
    std::vector<std::string> domains() const override { return {"physical"}; }
    bool supports(const World& w, const InteractionContext& ctx) const override;
    PredictionResult predict(const World& w, const InteractionContext& ctx) const override;
    std::unique_ptr<INaturalLawModel> clone() const override;
};

// ---- Warfare model (Phase 12.6) ------------------------------------------------
// launch / coat / don / doff / mobilize — weapons, armor and armies emerge
// from material numbers (edge_sharpness, blunt_mass, elasticity, tensile,
// armor_penetration_resist, volatility, expansion_force) and the diplomatic
// ledger. No weapon, armor, unit or tech names exist in the engine.
class WarfareModel final : public INaturalLawModel {
public:
    LawId id() const override;
    const char* name() const override { return "warfare"; }
    std::vector<std::string> domains() const override { return {"physical", "social"}; }
    bool supports(const World& w, const InteractionContext& ctx) const override;
    PredictionResult predict(const World& w, const InteractionContext& ctx) const override;
    std::unique_ptr<INaturalLawModel> clone() const override;
};

}  // namespace simlab

