#pragma once
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <unordered_map>
#include <string>
#include <vector>
#include "Causal.hpp"
#include "Clock.hpp"
#include "Deposits.hpp"
#include "Entity.hpp"
#include "Laws.hpp"
#include "Influx.hpp"
#include "Materials.hpp"
#include "Properties.hpp"
#include "Processes.hpp"
#include "Relations.hpp"
#include "SharedReg.hpp"
#include "Species.hpp"
#include "Rng.hpp"
#include "Spatial.hpp"
#include "Subsurface.hpp"
#include "Terrain.hpp"
#include "Types.hpp"
#include "Cognition.hpp"
#include "Brain.hpp"

namespace simlab {

class GenePool;

// ---- World Core -------------------------------------------------------------
// Owns World Truth: clock, entities, registries, spatial index, causal ledger,
// natural-law models and deterministic RNG streams. It knows nothing about
// agents, goals, technology or history — those are data and events, not code.
class World {
public:
    World(std::uint64_t seed, const std::string& data_dir = "data");

    // ---- speculative copy (planning, TODO §10.4) ----
    // Deep copy of ALL mutable world state: entities+props, terrain,
    // subsurface, relations, ledger, minds, RNG counters, clock — law models
    // are re-cloned (they are stateless). Used by plan_multi() to simulate
    // candidate action sequences without touching real World Truth.
    // The copy is fully independent: interacting with it cannot affect `*this`.
    World clone() const;

    // ---- speculative LOCAL copy (fog-of-war planning, Phase 9) ----
    // Like clone() but scoped to one agent's neighbourhood: only alive
    // entities within `radius` of `center` plus entities linked to them by
    // `part_of`/`wielding` edges (a composite's parts, a wielded tool),
    // only the `focus` agent's mind, only relations/social ties between
    // copied entities, and only the terrain/subsurface cells covering the
    // radius (remapped to a local grid). Entity ids are preserved.
    // The causal ledger is NOT copied (empty — event ids restart at 1,
    // self-consistent inside the clone), nor are the worldgen bookkeeping
    // sets (clone never worldgens). Used by plan_multi(): speculation
    // stays local instead of cloning the whole map + ledger per candidate.
    World clone_local(Vec2 center, std::int64_t radius, EntityId focus) const;

    // ---- content loading (data-driven: no engine change needed) ----
    bool load_content(const std::string& data_dir, std::string* err = nullptr);

    // ---- entities ----
    // Phase 10 anatomy: if `kind_name` resolves to a species with a body
    // plan, spawn contained part_of children (integrity 1000 each) and
    // return true. Idempotent — bodies already carrying parts are skipped.
    bool build_anatomy(EntityId body);
    // Functional fraction (permille 0..1000) of attached, undamaged parts
    // matching `role` (eye/leg/arm/...). Severed or destroyed parts count
    // against function; entities without anatomy return 1000.
    std::int64_t anatomy_function(EntityId body, const char* role) const;
    EntityId create_entity(const std::string& kind_name, MaterialId material,
                           Vec2 pos, const PropertyBag& overrides = {});
    Entity* find(EntityId id);
    // Every entity ever created with this kind, creation order (alive or not).
    const std::vector<EntityId>& entities_of_kind(const KindName& k) const;
    const Entity* find(EntityId id) const;
    std::vector<EntityId> entities_near(Vec2 p, std::int64_t radius) const;
    void destroy_entity(EntityId id);       // marks dead; id is never reused

    // ---- deposits (natural occurrences — World Truth, not agent knowledge) ----
    // Places a deposit occurrence as an entity. Returns kNoEntity if unknown.
    // `outcrop` = erosion has already stripped the cover (buried deposit starts exposed).
    EntityId create_deposit(const std::string& deposit_name, Vec2 pos, bool outcrop = false);

    // ---- world history queries (Phase 9) ----
    // Causal events that touched `id` (as participant, changed entity, or
    // spawned entity), most recent first, capped at `limit`.
    // World History query (Phase 9): EventIds of events that touched `id`,
    // newest first. Ids are stable across later appends — resolve via
    // ledger.find(). Pointers into the ledger would dangle on growth.
    std::vector<EventId> history_of(EntityId id, std::size_t limit = 32) const;

    // ---- social ties (Phase 8) ----
    std::int64_t tie_weight(EntityId a, const std::string& type, EntityId b) const;
    void adjust_tie(EntityId a, const std::string& type, EntityId b, std::int64_t delta);

    // ---- relationships ----
    void relate(EntityId a, const std::string& type, EntityId b);
    bool related(EntityId a, const std::string& type, EntityId b) const;
    std::vector<Relation> relations_of(EntityId id) const;
    std::vector<Relation> relations_of(EntityId id, const std::string& type) const;

    // ---- property resolution (instance > material > none) ----
    const Value* prop_of(const Entity& e, PropId p) const;
    // Observable-domain prop ids (what minds may perceive). The registry is
    // frozen after load_content, so this is built once and reused.
    const std::vector<PropId>& observable_props() const;
    // Integer convenience: raw payload of an Integer/Fixed prop, or fallback.
    std::int64_t prop_int(const Entity& e, PropId p, std::int64_t fallback = 0) const;

    // ---- interaction pipeline ----
    // Resolve an interaction against World Truth: law models predict, the
    // resolver commits, and the result lands in the causal ledger.
    InteractionResult interact(const InteractionContext& ctx);
    void add_law_model(std::unique_ptr<INaturalLawModel> m);

    // ---- resource accounting (TODO §3 Energy & Resource Accounting) ----
    // Total units of each material currently tracked by `quantity` props on
    // living entities. Extraction/processing conserve mass via waste outputs;
    // this is the hạch toán view over World Truth.
    std::map<MaterialId, std::int64_t> material_inventory() const;
    // Energy spent/gained across all causal events (milliJ), derived from
    // `energy` prop deltas in the ledger — hạch toán, not a separate counter.
    std::pair<std::int64_t, std::int64_t> energy_accounting() const;
    // Wielded-tool bonuses for `actor` (Phase 4 emergent capabilities):
    // scans `wielding` relations to alive, non-contained entities and returns
    // (max thrust_range, max edge) — reach extension and edge bonus that laws
    // may apply. Pure World Truth read; no agent knowledge involved.
    std::pair<std::int64_t, std::int64_t> tool_bonus(EntityId actor) const;

    // ---- cognition (TODO §10): per-NPC knowledge, separate from World Truth --
    AgentMind& mind(EntityId npc);                    // get-or-create
    const AgentMind* find_mind(EntityId npc) const;
    // All entities that currently have an AgentMind (sorted by id —
    // deterministic iteration for drivers/analyzers).
    std::vector<EntityId> minded_entities() const;
    std::size_t perceive(EntityId npc);               // -> observations
    void learn_from_event(EntityId npc, EventId ev);

    // ---- causal queries (TODO §7) ----
    // Human-readable "why": action, laws (by name), checked conditions, state
    // changes and the ancestor chain, bounded depth.
    std::string explain_event(EventId id, int max_depth = 4) const;
    // Every event in the provenance of an entity: the last event that touched
    // it plus all of that event's ancestors (where the material came from, who
    // processed it with what). Sorted ascending.
    std::vector<EventId> provenance_of(EntityId id) const;
    EventId last_event_of(EntityId id) const;

    // ---- time & determinism ----
    void step() { advance_ticks(1); }
    // Advance n ticks with no action (used by long-horizon tests/tools).
    void advance_ticks_for_test(std::int64_t n) { advance_ticks(n); }
    std::uint64_t state_hash() const;      // FNV-1a over all persistent state
    // Phase 12.1d: deterministic lexicon codex export (public API).
    std::string export_lexicon_codex() const;
    // ORDER-INDEPENDENT digest of world CONTENT (alive entities without ids,
    // terrain, subsurface): identical for any worldgen region materialization
    // order, unlike state_hash() which also covers entity ids/vector order.
    std::uint64_t content_hash() const;
    Rng& rng(const std::string& stream);   // named deterministic stream

    // ---- persistence (TODO §2.8 save/load) ----
    // Binary snapshot of World Truth: clock, entities+props, terrain,
    // subsurface, relations, causal ledger. Requires the same data_dir
    // content (property/material/deposit ids are positional) — load()
    // rejects files whose generator_version is unknown to this build.
    bool save(const std::string& path, std::string* err = nullptr) const;
    bool load(const std::string& path, std::string* err = nullptr);  // resets state

    // ---- worldgen spawn dedup (TODO §2.8.B) ----
    // Tags: deposit id for occurrences, kSpawnTreeTag for ecology entities.
    // claim_spawn returns true exactly once per (tag, global cell) — region
    // regeneration or overlapping chunks cannot duplicate world content.
    static constexpr std::uint32_t kSpawnTreeTag = 0xFFFFFFFFu;
    bool claim_spawn(std::uint32_t tag, int gx, int gy);
    const std::set<std::tuple<std::uint32_t, int, int>>& gen_spawned() const {
        return gen_spawned_;
    }

    // Global cells whose worldgen entity stages (deposits/species) have run.
    // Pure materialization bookkeeping — like gen_spawned_ but per-cell so a
    // lazy region driver can skip already-processed ground. Agents NEVER
    // read this: it tracks what exists in World Truth, not what is known.
    std::set<std::pair<int, int>> gen_cells_;
    void mark_gen_cells(int gx0, int gy0, int gw, int gh) {
        for (int gy = gy0; gy < gy0 + gh; ++gy)
            for (int gx = gx0; gx < gx0 + gw; ++gx)
                gen_cells_.emplace(gx, gy);
    }

    // Herd immunity: does `host` carry immune memory matching the antigen
    // of the strain entity `strain_id`? Used by every transmission route
    // (and by coated weapons, Phase 12.6).
    bool host_immune_to(EntityId host, std::int64_t strain_id) const;

    // ---- speculative planning copy (Bước 3 / A1) ----
    // Set ONLY on the throw-away worlds plan_multi() thinks with. A
    // speculative world still runs the physics and body laws that decide
    // whether an action works and what it does to the agent (law predict/
    // commit; the thinking agent's needs/anatomy/infection via flush_needs;
    // heat dissipation; its own memory decay), but freezes the rest of the
    // local world: no per-entity metabolize pass, ecology_tick,
    // language_tick, political_drift, myth_drift, terrain_wear_decay,
    // witness learning or *_from_event social hooks.
    // Never saved or hashed; real worlds keep it false.
    bool speculative = false;
    // Concurrent-time round (Primordial game driver, DriverState::concurrent):
    // while set, interact() does NOT advance the clock per action; it keeps
    // the longest action duration in `deferred_dt` and the driver advances
    // the world once per round by that amount (actions of one round happen
    // side by side, so a large population no longer starves each member of
    // turns). Transient: never saved, hashed or cloned; default off.
    bool defer_time = false;
    // Physics ruleset bits (saved v19, hashed only when non-zero so legacy
    // worlds keep their exact state_hash). kRulePrimordial (Genesis worlds):
    // an awake animal resists being eaten whole; a living organism is not
    // a hand tool. Laws read it; nothing else changes it after creation.
    static constexpr std::uint32_t kRulePrimordial = 1u;
    std::uint32_t ruleset = 0;
    std::int64_t deferred_dt = 0;

    // ---- public state ----
    std::uint64_t seed = 0;
    // Worldgen provenance (0/empty when the world was not generated).
    std::uint64_t world_seed = 0;
    int generator_version = 0;
    int gen_origin_x = 0, gen_origin_y = 0;   // global cell offset of terrain
    int gen_sea_level = 200;                  // worldgen config persisted for replay/validation
    SimulationClock clock_;
    // Content registries: immutable after load_content, SHARED (copy-on-write)
    // between a world and its clones — see SharedReg.hpp.
    SharedReg<PropertyRegistry> props;
    SharedReg<MaterialRegistry> materials;
    SharedReg<DepositRegistry> deposits;
    SharedReg<ProcessRegistry> processes;
    SharedReg<SpeciesRegistry> species;   // habitat data (data/species.txt, optional)
    SharedReg<InfluxRegistry> influx;     // edge-migration zones (data/influx.txt, optional)
    RelationshipGraph relations;
    // Weighted social ties between agents (Phase 8): key = Relation{a,type,b}
    // ("trust"/"debt"/"authority"/...), value = signed weight [-1000,1000].
    // Ties are World Truth about relationships — not agent beliefs.
    std::map<Relation, std::int64_t> social_;
    EntityRegistry kinds;
    EventLedger ledger;
    SpatialIndex spatial{1024};
    TerrainGrid terrain;               // continuous fields (empty until worldgen)
    SubsurfaceGrid subsurface;         // strata columns (empty until worldgen)
    std::vector<std::unique_ptr<Entity>> entities;
    std::vector<std::shared_ptr<INaturalLawModel>> laws;   // stateless -> shared by clones
    std::map<EntityId, AgentMind> minds_;             // agent knowledge (not world truth)
    // CL-1 shadow brains (Phase 1): shared_ptr so speculative clones alias
    // the same store; ALL brain writes are guarded by `speculative` —
    // planning copies observe but never learn. NOT part of state_hash
    // (brain_state_hash() fingerprints it separately for replay tests).
    bool brain_shadow = false;
    std::shared_ptr<BrainStore> brains_;
    // CL-1 Phase 4: optional read-only pointer to the persistent gene pool
    // (viewer-owned, lives outside World). brain_slot() consults it on
    // lazy init: 70% of fresh brains derive from pool donors, 30% stay
    // hash-init. NOT copied by clone() (speculative worlds never inject)
    // and NOT part of state_hash — the pool file is world INPUT, read
    // deterministically at spawn time only.
    const GenePool* gene_pool = nullptr;
    // Empty when content loaded cleanly; otherwise the load_content() error.
    // On failure registries are left EMPTY (all-or-nothing), never half-loaded.
    std::string content_error;
    mutable std::vector<PropId> obs_props_cache_;
    mutable std::size_t obs_props_defs_ = static_cast<std::size_t>(-1);

private:
    World() = default;                     // clone() fills every field
    // Advance the clock by n ticks AND metabolize: need props
    // (hunger/thirst/fatigue) grow with `metabolism`. Any resulting state
    // change is committed as ONE causal event ("metabolize") — needs drift
    // is World Truth change and must pass through the ledger like everything
    // else (AGENTS.md causality invariant).
    void advance_ticks(std::int64_t n);
    // Collective tithe (Phase 8): after an interact commits, members of
    // `ration_share` collectives pay a permille share of gained resources
    // into the common store — appended to the SAME causal event.
    void social_tithe(CausalEvent& ev, EntityId actor,
                      const std::vector<EntityId>& spawned);
    void metabolize(std::int64_t dt);
    // Phase 12.6: combat_morale drift toward the soldier's situation target.
    void morale_drift(Entity& e, std::uint64_t ticks, CausalEvent& ev);
    // Phase 9 LOD helpers: need-drift math factored out so interact() can
    // flush pending drift for its participants BEFORE law prediction —
    // a dormant entity must present current needs to the laws that judge it.
    struct NeedProps {
        PropId metabolism = kNoProp, hunger = kNoProp, thirst = kNoProp,
               fatigue = kNoProp, age = kNoProp, integrity = kNoProp;
        // flora regrowth bookkeeping (drift_needs): quantity recovers toward
        // max_quantity at regrow_rate milli-units/tick scaled by local
        // moisture x temperature suitability; regrow_credit accumulates
        // sub-unit progress so batched dormant catch-up stays exact.
        PropId quantity = kNoProp, regrow_rate = kNoProp,
               regrow_credit = kNoProp, max_quantity = kNoProp,
               fruit_decay_rate = kNoProp;
        // two-layer flora (fruiting bushes): the plant body is a normal
        // entity (living_wood); `fruit_count` is the stock ON the branch,
        // refilled by fruit_regrow_rate and spoiled by fruit_spoil_rate —
        // both milli-units/tick, batched-exact via fruit_credit. The body
        // is never touched by fruit loss.
        PropId fruit_count = kNoProp, max_fruit = kNoProp,
               fruit_regrow_rate = kNoProp, fruit_spoil_rate = kNoProp,
               fruit_credit = kNoProp;
        // perishability bookkeeping: decay_rate (permille of the integrity
        // scale per tick, material fallback allowed) rots `integrity`;
        // decay_credit accumulates sub-point damage so batched catch-up
        // is bit-identical to per-tick evaluation.
        PropId decay_rate = kNoProp, decay_credit = kNoProp;
        // energy + cognitive-metabolism bookkeeping (Nhịp 4e): energy is
        // the drain target; the mind_* props let drift_needs price brain
        // work per entity; brain_accum is the permille remainder that
        // keeps batched catch-up bit-identical to per-tick (Euclid LOD).
        PropId energy = kNoProp, mind_horizon = kNoProp,
               mind_abstraction = kNoProp, mind_brain_cost = kNoProp,
               brain_accum = kNoProp;
        // Phase 11.2 flora epidemiology reads: yield_penalty slows fruiting
        // on blighted plants; soil_nutrient on nearby patches is the ash
        // fertilization credit that speeds regrowth.
        PropId yield_penalty = kNoProp, soil_nutrient = kNoProp;
        // Phase 18 cultivation: `growth` accrues toward `growth_ticks`
        // before fruiting is allowed; `irrigation_credit` ticks of watered
        // soil lift the effective moisture the fruit drift sees, then burn
        // down. Plain integer props — batch-exact under dormant LOD.
        PropId growth = kNoProp, growth_ticks = kNoProp,
               irrigation_credit = kNoProp;
    };
    NeedProps need_props() const;
    // Apply dt ticks of need/age drift to one entity; appends StateChanges
    // to ev. Returns true if the entity's props changed. Handles the
    // integrity-0 reap inline (starvation death is immediate, not next pass).
    // A waste deposit an organism sheds (blood pool, feces, urine) —
    // committed as a real entity after the drift loop, causally.
    struct ShedPool {
        Vec2 pos{};
        std::int64_t amount = 0;
        MaterialId material = kNoMaterial;
        std::string kind;
    };
    // Phase 10 circulation + 10.3 excretion: blood loss/regen/clotting,
    // consciousness, gut/bladder fill, retention pathology for
    // anatomical bodies. Mutates entity props like drift_needs; appends
    // shed-waste spawn requests to `pools` for post-loop commit.
    void anatomy_drift(Entity& e, std::uint64_t dt, std::uint64_t now,
                       CausalEvent& ev, std::vector<ShedPool>& pools,
                       std::vector<SpawnSpec>* xspawns = nullptr);
    // Phase 10.4 epidemiology: transmission quanta shed by this entity's
    // active infections (airborne/contact routes) or by contaminated waste
    // it IS (waterborne route via strain_ref). Appends infection SpawnSpecs
    // to `out`; all bookkeeping writes are causal StateChanges.
    void epidemiology_drift(Entity& e, std::int64_t pending,
                            std::uint64_t now, CausalEvent& ev,
                            std::vector<SpawnSpec>& out);
    // True when this entity's physiology has state feedback that a single
    // batched pass cannot reproduce bit-identically (active infections,
    // neoplasm growth/initiation, tissue infection near fever thresholds,
    // toxin load near the oncogenic threshold). Coupled entities replay
    // pending ticks one-by-one inside metabolize — rare, and the only way
    // dt=1 x N stays exact for nonlinear ODEs.
    bool physiology_coupled(const Entity& e) const;
    // Materialize drift-proposed spawns (infections, mutant strains) after
    // the metabolize entity loop — create_entity + relations + ledger.
    void commit_drift_spawns(CausalEvent& ev, std::vector<SpawnSpec>& spawns);
    bool drift_needs(Entity& e, std::uint64_t dt, const NeedProps& np,
                     CausalEvent& ev, std::uint64_t now);
    // Catch dormant participants up to the current tick (ledgered as a
    // "metabolize" event) so predictions see real needs, not stale ones.
    void flush_needs(const std::vector<EntityId>& ids);
    // Newtonian cooling toward the local ambient temperature (terrain field,
    // else 20°C); ledgered as one "cool" event like metabolism.
    void dissipate_heat(std::int64_t dt);
    // Ecology pass (seed bank + edge influx): runs once per advance_ticks.
    //  - dead entities carrying `death_drops` spawn their manifest once
    //  - dormant_seed entities germinate when sprout_rate x elapsed >= 1000
    //    permille-ticks AND local terrain moisture/temperature fit the band
    //  - influx zones fire on a stateless hash schedule (seed+zone+tick)
    // Everything commits as ONE "ecology" causal event — spawns, kills and
    // bookkeeping prop changes are all ledgered like any other world change.
    void ecology_tick(std::int64_t n);
    // Phase 11 environmental reservoirs: corpse rot (corpse_decay_level via
    // decay_rate) and standing-water fouling (stagnation_level via
    // stagnation_rate), both Euclid-batched; past their data thresholds a
    // reservoir emits odor/contamination and spreads strain_ref to nearby
    // entities on a reservoir_spread_accum quantum. `buried`/`cremated`
    // silence a corpse instantly. Runs for dead AND living entities inside
    // metabolize — batch-exact like every other drift.
    void environmental_drift(Entity& e, std::uint64_t pending,
                             CausalEvent& ev, bool record_hist = true);
    // Lazy symbiont seeding: a host carrying `symbiont_signature` and no
    // symbiont strain infection gets its commensal PathogenStrain +
    // ActiveInfection created here (real entity ids, causal event).
    void seed_symbionts(CausalEvent& ev, const std::vector<EntityId>& hosts);
    // Zoonotic spillover: after a successful consume commit, a susceptible
    // actor (reservoir_tolerance < 700) that ate raw strain-carrying tissue
    // or water is infected — with reservoir_tolerance < 500 a deterministic
    // zoonotic_jump_bias roll may found an adapted strain variant.
    void zoonotic_spillover(const InteractionContext& ctx, CausalEvent& ev,
                            std::vector<EntityId>& spawned);
    // Phase 11.2 flora epidemiology: a susceptible plant (blight_susceptibility
    // > 0) carrying a spore_rate>0 strain sheds spore quanta whose rate scales
    // with same-species neighbor density (monoculture amplification); quanta
    // found new infections on nearby same-species plants. Also drives
    // virulence integrity loss, yield_penalty and observable blight_marks.
    // Appends infection SpawnSpecs to `out` — same commit path as epidemiology.
    void flora_drift(Entity& e, std::int64_t pending, std::uint64_t now,
                     CausalEvent& ev, std::vector<SpawnSpec>& out);
    // Phase 11.3 vector ecology: an is_disease_vector organism reads the
    // local environment (terrain temperature band + nearby stagnant water)
    // and flips vector_dormant. Active vectors bite on a vector_bite_accum
    // quantum: they pick strains up from infected hosts and inject them
    // into naive ones at the strain's vector_borne_rate; dormant vectors
    // neither bite nor breed. Active vectors also breed on
    // vector_breed_accum — offspring inherit the parental genome. Dormancy
    // flips mid-window replay tick-wise (LOD-exact); steady windows batch.
    void vector_drift(Entity& e, std::int64_t pending, std::uint64_t now,
                      CausalEvent& ev, std::vector<SpawnSpec>& out);
    // Phase 12.1 causal proto-language: per-advance coinage pass. Every
    // mind holding a Supported/Verified discovery whose concept has no
    // LexicalEntry in the speaker's language gets one coined — concept +
    // entry entities spawned through the causal spawn path with seeds
    // derived from world_seed, language_id, concept_key and the species'
    // acoustic props. Individuals with vocal_capability >= 500 and no
    // language_id yet are assigned one deterministically from
    // (world_seed, kind_name). All writes ride one "language" causal event.
    void language_tick(std::uint64_t now);
    // Per-entity language drift inside metabolize: `language` entities
    // accumulate dialect_drift_accum (rate x isolation multiplier); each
    // dialect_drift_threshold worth of accum advances dialect_drift_rolls
    // and XOR-shifts every LexicalEntry phoneme_seed of that language by a
    // mask derived from the roll index — cumulative XOR makes the final
    // seed depend only on the roll COUNT, so batched windows are exact.
    // `inscription` entities erode by (1000 - durability) permille/tick
    // into erosion_level; at 1000 the glyph/concept payload is illegible.
    void language_drift(Entity& e, std::int64_t pending, std::uint64_t now,
                        CausalEvent& ev);
    // Phase 12.2 economy drift inside metabolize: possessed items accrue
    // possession_ticks; theft_flag counts down its exposure window and a
    // still-armed grab is exposed when any OTHER minded agent sees the
    // item (reputation hit, flag cleared); open debt_obligations past
    // due_tick default (status=2, debtor reputation collapses).
    void econ_drift(Entity& e, std::int64_t pending, std::uint64_t now,
                    CausalEvent& ev);
    // Phase 12.1d lexicon codex: deterministic text export — per language,
    // every LexicalEntry rendered as raw phoneme/glyph strings (seeds ->
    // syllable stream via species acoustics) with the objective chronicle
    // of its concept and the community's deciphered_ratio (0..1000).

    // Queue an ActiveInfection SpawnSpec on `host` for `strain_id` —
    // deduped against infections already pending in `out` this pass.
    void queue_infection(std::vector<SpawnSpec>& out, EntityId host,
                         std::int64_t strain_id, std::int64_t load);
    // Ebbinghaus decay (Nhịp 1): every mind's memory engrams and discovery
    // engrams lose clarity by the integer-exact accum/drop recurrence —
    // batch-exact for any dt, so per-tick and dormant-tick paths agree.
    // clarity == 0 erases the entry (forgetting releases the RAM).
    void decay_minds(std::int64_t n);
    // Phase 12.3: per-mind political cadence — defer_to recomputation,
    // grievance drift, revolt. Accumulator-exact like decay_minds; prop
    // consequences ride one ledgered CausalEvent per call.
    void political_drift(std::int64_t n);
    // Phase 12.5: per-mind mythogenesis accumulator — unexplained shocks
    // crystallize into sacred beliefs + semantic_concept entities.
    void myth_drift(std::int64_t n);
    // Phase 12.4 architecture: per-entity structural drift inside
    // metabolize — enclosure weathering (decay_accum Euclid), collapse
    // into debris + occupant impact, orphan portal reaping. Also scans
    // enclosure footprints for trespass/consent state each pass.
    void arch_drift(Entity& e, std::int64_t pending, std::uint64_t now,
                    CausalEvent& ev, std::vector<SpawnSpec>& out);
    // Footfall compaction: linear per-tick fade over dirty cells only —
    // exact under dt=1 xN vs dt=N because the decay has no memory.
    void terrain_wear_decay(std::int64_t n);
    // Causal parents of an interaction: caller-supplied + the last event that
    // touched each target / wielded tool (automatic provenance chain).
    std::vector<EventId> auto_parents(const InteractionContext& ctx) const;
    void note_touched(const CausalEvent& ev);   // update touched_ from a ledger event

    std::map<std::string, std::unique_ptr<Rng>> rngs_;
    std::unordered_map<EntityId, std::size_t> index_;
    // kind -> entity ids in CREATION order (same relative order as
    // `entities`, dead included: callers filter alive exactly as their old
    // full scans did). Keyed by the interned kind pointer.
    std::unordered_map<const std::string*, std::vector<EntityId>> by_kind_;
    std::map<EntityId, EventId> touched_;
    std::vector<std::uint8_t> mat_need_flags_;        // LOD prefilter (lazy)               // entity -> last causal event that changed/created it   // id -> entities[] slot (lookup only)
    std::set<std::tuple<std::uint32_t, int, int>> gen_spawned_;
    // Global cell coords (gx,gy packed) whose terrain.traffic_wear > 0 —
    // rebuilt deterministically from terrain on load; only dirty cells pay
    // the per-advance decay cost.
    std::set<std::uint64_t> wear_cells_;
};

}  // namespace simlab

namespace simlab {
// Deterministic spoken/written rendering of a language seed (display codec).
std::string lang_render_word(std::uint32_t seed, std::int64_t syllables, std::uint64_t world_seed);
}
