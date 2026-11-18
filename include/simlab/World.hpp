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
#include "Species.hpp"
#include "Rng.hpp"
#include "Spatial.hpp"
#include "Subsurface.hpp"
#include "Terrain.hpp"
#include "Types.hpp"
#include "Cognition.hpp"

namespace simlab {

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
    EntityId create_entity(const std::string& kind_name, MaterialId material,
                           Vec2 pos, const PropertyBag& overrides = {});
    Entity* find(EntityId id);
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

    // ---- public state ----
    std::uint64_t seed = 0;
    // Worldgen provenance (0/empty when the world was not generated).
    std::uint64_t world_seed = 0;
    int generator_version = 0;
    int gen_origin_x = 0, gen_origin_y = 0;   // global cell offset of terrain
    int gen_sea_level = 200;                  // worldgen config persisted for replay/validation
    SimulationClock clock_;
    PropertyRegistry props;
    MaterialRegistry materials;
    DepositRegistry deposits;
    ProcessRegistry processes;
    SpeciesRegistry species;           // habitat data (data/species.txt, optional)
    InfluxRegistry influx;             // edge-migration zones (data/influx.txt, optional)
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
    };
    NeedProps need_props() const;
    // Apply dt ticks of need/age drift to one entity; appends StateChanges
    // to ev. Returns true if the entity's props changed. Handles the
    // integrity-0 reap inline (starvation death is immediate, not next pass).
    bool drift_needs(Entity& e, std::uint64_t dt, const NeedProps& np,
                     CausalEvent& ev);
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
    // Ebbinghaus decay (Nhịp 1): every mind's memory engrams and discovery
    // engrams lose clarity by the integer-exact accum/drop recurrence —
    // batch-exact for any dt, so per-tick and dormant-tick paths agree.
    // clarity == 0 erases the entry (forgetting releases the RAM).
    void decay_minds(std::int64_t n);
    // Causal parents of an interaction: caller-supplied + the last event that
    // touched each target / wielded tool (automatic provenance chain).
    std::vector<EventId> auto_parents(const InteractionContext& ctx) const;
    void note_touched(const CausalEvent& ev);   // update touched_ from a ledger event

    std::map<std::string, std::unique_ptr<Rng>> rngs_;
    std::unordered_map<EntityId, std::size_t> index_;
    std::map<EntityId, EventId> touched_;
    std::vector<std::uint8_t> mat_need_flags_;        // LOD prefilter (lazy)               // entity -> last causal event that changed/created it   // id -> entities[] slot (lookup only)
    std::set<std::tuple<std::uint32_t, int, int>> gen_spawned_;
};

}  // namespace simlab
