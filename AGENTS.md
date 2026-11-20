# sim_lab — Sunnyworld Open-ended Evolution Engine

Pure C++20 world-simulation engine per `TODO.md`. No SDL, no rendering,
no floats in simulation state — deterministic fixed-point only.

## Layout

- `include/simlab/` — public headers (Types, Rng, Clock, Properties,
  Materials, Deposits, Entity, Relations, Spatial, Terrain, Causal, Laws,
  World, WorldGen)
- `src/` — implementations
- `data/` — declarative content: `properties.txt`, `materials.txt`,
  `deposits.txt`, `processes.txt`, `species.txt`, `influx.txt`
  (last four optional)
- `tests/test_main.cpp` — verification suite

## Commands

    make            # build build/tests
    make test       # run the suite
    make clean

Data files are loaded from `data/` relative to the working directory
(`World(seed, "data")`). Adding a material = editing `data/materials.txt`;
no engine code changes.

## Invariants (do not break)

- Determinism: seeded per-stream RNG (`World::rng(name)`), sorted
  containers for iteration, `state_hash()` must match across replays.
- World Truth vs belief: law models read entity/material properties only;
  agent knowledge (future layers) must never write into `Ontology`/entities.
- Causality: every state change goes through `World::interact` ->
  `EventLedger`; nothing mutates entity props outside `interact`.
- `integrity` reaching 0 destroys an entity (convention used by physics).
- Deposits are World Truth (`Entity::deposit` + `DepositRegistry`), never
  agent knowledge. `deposits.txt` is optional; `create_deposit()` seeds
  `deposit_remaining` (from `reserve`) and `exposed` (= surface accessibility).
- `RelationshipGraph` edges are open string types in a sorted set —
  deterministic iteration; included in `state_hash()`.
- Ecology (seed bank + edge influx): `World::ecology_tick` runs once per
  `advance_ticks` and commits ONE "ecology" causal event. `death_drops`
  ("material=count,..." text prop on a corpse) spawns its manifest once
  (`death_drops_done` marker); drops whose MATERIAL defines `sprout_rate`
  are propagules — they get `sprout_target`/`sprout_material`/`sprout_since`
  and germinate when `sprout_rate` x elapsed ticks >= 1000 permille-ticks
  AND local terrain moisture/temperature fit the `sprout_*` band (static
  terrain -> batch-exact, no per-tick credit). `data/influx.txt` declares
  `influx_zone`s: stateless hash schedule
  (`stream_seed(seed, zone, tick) % interval == 0`), `capacity` gate,
  migrant templates with `prop`/`gene`/`innate` (innate verbs -> AgentMind).
- Memory (Ebbinghaus, "Nhịp 1"): `AgentMind::MemorySnapshot` and
  `DiscoveryRecord` carry a `MemoryEngram` (clarity/stability/
  emotional_charge/rehearsal_count/decay_accum/last_rehearsal). There is NO
  FIFO cap — `World::decay_minds(n)` runs once per `advance_ticks` and
  forgets via the integer-exact `accum += n*rate; drop = accum/stability`
  recurrence (batch-exact for any dt -> LOD-safe, replay-safe). Salience
  encoding in `perceive()`/`learn_from_event()`/`witness_from_event()`:
  |charge| >= `mind_imprint_threshold` forges a deep memory in one exposure
  (`imprint_base_stability + |charge|*amygdala_multiplier`); rest events
  run `sleep_consolidate` (top-|charge| stability doubling + prune below
  `mind_sleep_prune_threshold`). Retrieval blurs via `recalled_pos` /
  `recalled_prop` (deterministic hash on entity+seed+clarity) — stored
  truth is never mutated. `mind_*` props (data/properties.txt,
  domain=cognitive) tune each mind; `gene mind_*` loci make traits
  heritable. Save format v13.
- Neuro-emotional dynamics (Nhịp 2-3): `AgentMind::neuro` (NeuroState)
  holds the four heritable axes (threat/reward/pattern/empathy coupling,
  tuned once from `mind_*` props) plus transient panic/rage/grief. NO
  disease flags exist — every "disorder" is an axis phase-shift:
  `allostatic_shift` bends axes permanently on shocks <= -mind_trauma_charge
  (genome decides WHICH axis bends); `maybe_spurious_association` writes
  false aversive engrams when pattern_coupling>1600 & panic>500;
  `reward_gain` scales positive plan scores in plan_multi (anhedonia);
  `panic_relief` + `compulsion_bonus` reward actions that calmed panic;
  Driver's executive override (panic/rage/grief > threshold) skips
  plan_multi for reflex strike/flee/rest; flashback in perceive()
  re-vividifies deep negative engrams matching material/kind. Epigenetic
  marks: reproduce() transmits up to 2 born_memories (clarity 250,
  stability 1500, charge*30%) for parent engrams with |charge|>=900 &
  stability>=9000. Charge sign convention: trauma NEGATIVE, satisfaction
  POSITIVE. Save format v14.
- Cognitive aptitudes (Nhịp 4): four heritable axes in MindParams —
  plan_horizon (1..6, feeds generate_hypotheses -> plan_multi depth AND
  beam=4+2*h), abstraction_gain (>1200 generalizes fresh imprints to
  visible same-material/kind entities via upsert_memory_g, scaled
  (gain-1000)/1000; also scales Discovery evidence_for), motor_precision
  (scales impact/extraction force, composite product integrity, tool-wear
  collateral, and skill stability gains in run_experiment),
  theory_of_mind (>1000 infers conspecific agitation from OBSERVED MOTION
  between perceive() sightings only — witness events don't count — as a
  `mood:<id>` Belief; scales communicate claim-count/authority/trust).
  Brain cost: drift_needs drains `energy` by
  mind_brain_metabolic_cost * ((horizon-2) + (abstraction-1000)/1000)
  permille-granular, remainder in `mind_brain_accum` (Euclid LOD-exact).
  NOTE: state_hash covers the causal ledger, so batch-vs-stepped
  advance_ticks legitimately differ in metabolize EVENT COUNT — LOD
  invariance applies to prop math, not ledger granularity.
- Worldgen: `WorldGenerator::generate(world, params)` fills `world.terrain`
  (elevation/moisture/temperature/water/vegetation/slope, integer units —
  see Terrain.hpp) then places deposits only where `deposits.txt`
  `condition` clauses hold, plus species (data/species.txt) entities. Fully deterministic via
  coordinate hashing — no floats, no RNG state. `WorldGenerator::validate`
  checks consistency (e.g. no surface deposit on water).

## Cognition layers (Nhịp 1–4.5)

- `MemoryEngram` (clarity/stability/emotional_charge/rehearsal/decay_accum)
  replaces FIFO memory; Ebbinghaus decay in `World::decay_minds` is
  Euclidean-batched (dt-invariant). Salience: trauma negative, satisfaction
  positive. Semantic drift jitters low-clarity recalls.
- Neuro axes (`mind_threat/reward/pattern/empathy_gain`) tuned once from
  props; panic/rage/grief decay Euclid; allostatic shift + epigenetic
  `born_memories` on reproduce; flashbacks & spurious associations emerge
  from charge x gain — no disease flags anywhere.
- Aptitudes (`mind_plan_horizon/abstraction_gain/motor_precision/
  theory_of_mind`) are heritable genes scaling plan depth, discovery,
  craft precision, and social inference; `mind_brain_metabolic_cost`
  drains energy in `drift_needs` (integer remainder prop keeps dt-invariant).
- Pair-bonding (4.5): positive engrams from reproduce/share scaled by
  `mind_pair_bond_gain x empathy`; `bond_bonus` in plan scoring rewards
  share->bonded and strike->negative-engrams. Bereavement flips dear
  engrams (charge>=700) negative + grief/rage. Jealousy: witnessing a
  bonded partner reproduce/share with a rival floods rage and marks the
  rival. `mind_parental_drive` writes deep imprints (stability 9500)
  parent<->child at birth; drive 0 writes nothing.
- NOTE: `state_hash` includes the causal ledger, so batched vs per-tick
  `advance_ticks` differ in metabolize event COUNT (valid); drain math
  itself is dt-invariant.
- Care depth (4.5b): `bonded_distress()` infers a bonded entity's welfare
  from ToM mood belief + imprint depth + OBSERVABLE snapshot signs
  (quantity==0, integrity<400) — private needs of others are never read.
  `detect_problems` emits `ProblemKind::BondedDistress`; `gen_candidates`
  adds bonded engrams as targets plus `move` candidates aimed at the
  remembered position; `care_bonus` in plan_multi scores share/move-
  toward on satisfaction scale, gated by own hunger/thirst pressure vs
  empathy_coupling (starving low-empathy yields to self, extreme empathy
  yields to the bonded). `score_plan_hypothesis` maps care plan.score to
  Ps for this problem kind.
- Primitive social psych (Nhịp 5): `mind_conformity_gain/xenophobia_bias/
  ego_defense_gain` heritable. Conformity: `recent_witnessed` ring (v15)
  feeds `conformity_bonus` in plan_multi (>=3 familiar/collective
  conspecifics on same verb; discounted by abstraction). Ostracism:
  isolation_bias>1000 + no recent sighting or >=2 hostile engrams ->
  fused net-rate panic/grief drive in decay_minds (LOD-exact: drive and
  decay share one clamped expression). Reciprocity: crisis gift or
  witnessed defense of own enemy -> gratitude engram (stab 8500).
  Xenophobia: first-sight conspecific stranger gets wary negative mark
  (xenophobia*30/100); collective mates get negative charge discounted.
  Ego defense: grief/rage>=700 + ego>1200 -> scapegoat stranger
  (pattern>1500 & empathy<800) or sublimate into sublimation_relief
  craft bonus (abstraction>1500, grief/rage *2/3).
- Save v15 adds recent_witnessed, sublimation_relief, ostracism_accum.
## Phase 10 — anatomy, trauma, circulation

- Species body plans live in `data/species.txt` as `body_part <name>
  <material> <slot> <role> <vital> <size>` lines inside a `species`
  block (`SpeciesDef::anatomy`). `World::build_anatomy` spawns them as
  contained `part_of` children (integrity 1000); called from worldgen and
  the interact spawn-commit (reproduce).
- Anatomy parts are identified by the `part_role` text prop — ordinary
  composite structure parts (spear head/shaft) do NOT have it, so
  `PhysicsModel` routes blows into anatomy only when role-marked parts
  exist; composites keep plain integrity damage.
- Severed parts KEEP their `part_of` edge (anatomical record) with
  `severed=1` + `contained=false`; `anatomy_function(body, role)` counts
  them against function. Amputating an arm removes the body's `wielding`
  edges (the tool drops).
- Circulation runs in `World::anatomy_drift` (inside `metabolize` and
  `flush_needs`): attached-part `bleed_rate` sum drains `blood_volume`
  via `blood_accum` Euclid remainder; clotting uses per-part
  `bleed_accum`; regen (+12/tick) needs energy>0 && thirst<800;
  `consciousness` mirrors `blood_volume`; bv<=0 -> integrity 0 + reap.
  Blood pools spawn as `blood`-material entities at 500-tick boundaries.
- `consciousness < 300` blocks ALL actions in `World::interact` (one
  gate) plus per-law `conscious_enough`/`anatomy_motor` (LawUtil.hpp)
  scale motor precision by arm function and blood volume.
- `sight_radius_of` (Cognition.cpp) scales by eye function and blood
  volume; LocomotionLaws scales speed by leg function.
- Pain: PhysicsModel adds `pain_level` changes on the body; delta pain
  in `learn_from_event` pumps `neuro.panic`/`neuro.rage` (split by
  threat_gain) and writes a negative trauma engram on the attacker.

## Phase 12.5 — myth, oral literature, ritual, diplomacy

- Mythogenesis runs in `World::myth_drift` (per-mind `myth_accum`, period
  100, each fire evaluated at its own historical tick, so dt=1xN == dt=N).
  A discovery that was tried but never Verified, whose engram has
  |emotional_charge| >= `mind_trauma_charge` (either sign), grows a
  `sacred:<key>` belief by `(agency_attribution_bias-300)/2 *
  abstraction_gain/1000` per period (agency < 400 = materialist, no myth).
  The first crossing of 600 spawns ONE `semantic_concept` per key with
  `sacred_key`, seed-derived `sacred_agent`, derived `appeasement_action`
  and `sacred_valence` ("curse" for trauma < 0, "blessing" for salvation
  > 0). No deity, rite or treaty names exist in code.
- Oral attrition shares the same period: a `lex:` belief not rehearsed for
  5000 ticks loses 10 per period; if its ENTRY has `oral_genre` set it
  loses only 1. Speaking (`communicate`) and chanting (`perform_ritual`)
  refresh `last_tick` in `myth_from_event`.
- `communicate` bumps `transmission_count` on up to 8 confident entries;
  at >= 150 transmissions with `oral_cadence_of` >= 500 the entry gets
  `poetic_cadence` + `oral_genre` ("proverb" with <= 3 siblings, else
  "epic"). Classic entries teach 3x (social_from_event) and lift the
  reciter's `social_legitimacy` by 15. `export_lexicon_codex` prints
  `EPIC`/`PROVERB` rows with raw_phonemes, cadence, transmissions, myth
  degree and chronicle_english.
- `consecrate` carves the sacred key (+ CausalClause via `compose_clause`)
  on a surface. An enclosure containing such an INSCRIPTION is inferred as
  `sanctuary` by `arch_function` (abstract concepts don't count).
- `perform_ritual` needs a sacred conviction or a mandate; offerings drain,
  legitimacy gain = clamp(80 + offered marginal value/50, 80, 300); a fire
  rite heats entities within 200 units and sterilizes by the Phase 11 rule.
  Grievance relief = 60 + sanctuary pull/5 for same-tongue minds in range.
- Diplomacy ledger: `diplo_table` text on `language` entities, codec in
  Cognition.cpp (`diplo_parse/render/bump/mark_trade`):
  `peer:trust[:ft][:host];`. Trust is always stored numerically; `host`
  follows trust <= -600; `ft` is set ONLY by a free_trade treaty and lapses
  below trust 0. Gifts move real goods and add clamp(value/20, 50, 400);
  cross-border `steal` subtracts 150; hostile pairs refuse `trade`, and
  every mind of both tongues gets `hostile_tongue:<peer>` = 1000.
  `treaty` needs trust >= 500 and a bilingual envoy (>= 600); clause is
  free_trade at trust >= 700, else non_aggression, carved as a Rosetta
  inscription (`language_id2` + `clause_atoms` from the envoy's words).
- Save format v18 adds `myth_accum`.

## Performance conventions (optimization pass, state_hash-neutral)

- Builds: `make` (build/, -O2), `make MODE=release` (build-release/,
  -O3 -flto), `make MODE=profile` (build-profile/, -pg for gprof). The
  engine is integer-only, so every mode yields the same `state_hash`.
- Property ids by name: NEVER call `props.find_name("x")` on a hot path.
  Use `SIMLAB_PID(w.props, "x")` (per-call-site cache keyed on
  `PropertyRegistry::uid()`), or for functions resolving many names per
  call `static thread_local SeqPidCache c; SeqPid pid(c, w.props);` then
  `pid("x")`. Both REQUIRE string literals (pointer-keyed caches; temporary
  strings must go through a static literal table, see kImmuneSigNames).
  `PropertyRegistry::uid()` changes on every mutation/copy, invalidating
  caches automatically.
- Content registries (`props/materials/deposits/processes/species/influx`)
  are `SharedReg<R>` copy-on-write handles: clones share one registry; a
  write through `add/parse/load_file/mut()` detaches first.
- `Entity::kind_name` is a `KindName` (interned `const std::string*`,
  process-wide append-only pool). Compare literals with
  `e.kind_name == SIMLAB_KIND("enclosure")` (pointer compare).
  `World::entities_of_kind(k)` lists ids of a kind in creation order
  (dead included) — use it instead of scanning `entities` for one kind.
- `World::speculative` (set by `plan_multi` on its clones): advance_ticks
  only runs `flush_needs(minds)`, `dissipate_heat`, `decay_minds`; no
  per-entity metabolize pass, ecology/language/political/myth/terrain
  drift, witness learning or `*_from_event` hooks. Never true on a real world.
- The capstone test prints `capstone fingerprint: ... state_hash=...`;
  baseline hashes for SIMLAB_CYCLES 100/300/500/800 are b01e7e33ed7962ff /
  fb3ddce7393e6e5e / 82715b8f27ec0cfc / d47683031ca69ffc. A pure
  optimization must reproduce them exactly.

## Phase 12.6 — warfare, logistics & emergent weapons R&D

- `WarfareModel` (law 23, src/WarfareLaws.cpp): `launch` (target, ammo
  [, launcher]), `coat` (source), `don`/`doff` (armor), `mobilize` (foe;
  note `depot:<enclosure>`). Peace is `diplomatic_parley` note
  `peace:<enclosure|0>:<material|->:<qty>` (SocialLaws).
- Material channels (data only): `edge_sharpness` (cut/bleed),
  `blunt_mass` (impulse through soft armor), `elasticity` + `tensile`
  (launchers), `armor_penetration_resist` (worn via `wearing` relation:
  sharp x (1000-apr)/1000, blunt x (1000-apr/3)/1000), `volatility` +
  `expansion_force` (blast), `coat_toxin`/`coat_strain` (penetrating
  edge -> blood `toxin_load` / ActiveInfection). Composites take the MAX of
  parts; `launch_impulse` EMERGES from elastic frame x tensile part x
  (1000 + frame_mass/10)/1000 x motor — heavy frames are siege engines.
- `lawutil::deliver_blow(w,r,target,E,Blow,striker,wear)` is the single
  body/structure damage path (PhysicsModel impact, launch, blast). Wounds
  note `victim=<id> mech=<bleed|blunt|projectile|blast|toxin|thermal>`.
- `lawutil::blast`: impulse = expansion x (500+casing/2)/1000 x
  (1000+mass/50)/1000, radius 50+expansion/4, linear falloff; bodies take a
  blunt "blast" blow, enclosures lose integrity. ThermalModel fires it when
  a charge (volatility>=300, expansion>=300) crosses 4000-3*volatility.
- Structures: launch/blast damage = E x (500+blunt)/(5000 x (500+barrier));
  below integrity 300 the wall is BREACHED (access_barrier -> 0). `take`/
  `steal` of goods inside a barred (>=500) enclosure fails from outside its
  footprint unless the actor owns it.
- Morale: `World::morale_drift` inside metabolize/flush_needs, per tick
  (soldiers = war_target>0 are physiology_coupled -> LOD-exact). Target =
  500 +150 leader (defer_to within communicate_range) +150 supply (stocked
  depot reached directly or on terrain wear>=100) -500 hunger>=700 / -200
  >=500 - pain/3 +100 known epic; step 5/tick; <200 sets `routed`=1,
  >=400 clears it.
- `war_from_event` (Cognition): arms race (`weapon_counter:<mech>`,
  `armor_tech_bias` beliefs on victim and same-tongue kin within 600),
  `war:<tongue>` beliefs from war_target changes, peace ownership transfer
  (`owns:<enc>`). `arms_race_bonus` feeds plan_multi scoring.
- Defeat (peace precondition): >= 2/3 of the tongue's soldiers marching on
  the other routed or dead, or their depot breached.

## Phase 13 - observation gate (renderer)

- Render CORE is SDL-free and linked into the tests: `src/render/*.cpp`,
  `include/simlab/render/` - `ProceduralAtlas` (every sprite generated from
  data/*.txt at load: material tiles/icons from physical props, species
  standees from `body_part` anatomy or inferred from reach/vocal props,
  fruit dots exactly proportional to fruit_count/max_fruit, 47-shape wall
  autotiling with integrity cracks, weapon icons from edge/blunt/elastic/
  coat channels, per-language procedural glyph scripts) and
  `capture(const World&, ViewRect, FogOfWar&) -> RenderSnapshot` (viewport
  culled, back-to-front). `FogOfWar` (viewer state, not World Truth):
  Unexplored / Shrouded (architecture remembered, motion hidden) / Visible;
  Tribe vision (minds of one language_id) or God Eye.
- `build/sunnyworld_viewer` (`make viewer`, src/render/sdl/viewer_main.cpp):
  SDL3 (vendored third_party/SDL-main, static, X11 dlopen'ed; compile-time
  X11 headers in third_party/x11-headers) + Dear ImGui 1.92.9
  (third_party/imgui). Software renderer only (GL never loaded). The sim
  runs on a worker thread under a mutex; the UI thread only snapshots/reads.
  Ground pre-composited into one target texture; world drawn at 1/2 res and
  upscaled nearest; ImGui layer cached, re-rasterized on input or every 4th
  frame. `--selftest N` (SDL_VIDEO_DRIVER=dummy) checks render-only frames
  leave state_hash unchanged and prints frame ms / RSS. Diagnostics:
  SIMLAB_VDEBUG=1 (per-pass ms), SIMLAB_VSKIP bitmask.
- `EventLedger::set_byte_budget(bytes)`: optional RAM bound on the retained
  ledger window (0 = off, the default; the viewer uses 16 MB). Eviction is
  the same deterministic fold into archived_hash.

## Phase 14 — Primordial Genesis & Patron gameplay layer

- `simlab/Genesis.hpp` `build_primordial(World&, PrimordialParams)`: naked
  tick-0 world (worldgen with `skip_species={founder}`), drinkable shoreline
  `water` stock entities, founders with ONLY the species `innate` verbs
  (data/species.txt `innate <verb>`), personality genes
  (`mind_abstraction/threat/reward/empathy_gain`), energy = max_energy,
  `last_reproduce = 0`. Sets `World::ruleset |= kRulePrimordial`.
  `primordial_violations()` counts pre-made culture (must be 0 at tick 0).
- `World::ruleset` (save v19, hashed ONLY when non-zero → legacy hashes
  unchanged). `kRulePrimordial`: an awake animal (consciousness >= 300, no
  stock) resists being eaten whole; a living species individual is not a
  wieldable tool.
- Planet mode is encoded in the WORLDGEN seed
  (`WorldGenerator::planet_seed/planet_dims/planet_latitude`, bit 63): climate
  belts by latitude, ocean rim, ridged ranges, several continents. Legacy
  seeds (bit 63 clear) keep the original analytic fields.
- `DriverState` flags (all default OFF → every pre-existing scenario is
  bit-identical): `curiosity` (babble_step: idle minds try unknown verbs on
  remembered non-animal THINGS in reach; `omen:<verb>[:<mat>]` beliefs are
  heeded by personality and fade when ignored; `beacon:x:y` / `site:x:y:r`
  beliefs walk scouts/builders, builders try `enclose` with remembered loose
  solids; round-robin discover budget; wield reflex needs the verb),
  `concurrent` (World::defer_time: one round's actions share the clock, the
  world advances once by the longest action), `possessed` (driver skips the
  player's avatar — it only perceives).
- Viewer-side `simlab/render/Patron.hpp` (SDL-free, linked into tests):
  `PatronJournal` (Rosetta lexicon per tongue: heard/carved words, player
  guess + tag, CONFIRMED only when the guess matches the word's concept gloss
  and it was witnessed; milestones = first completed verb by a sapient + first
  culture kind; Resonance; pins; notes) saved as `slotN.sav.journal`.
  `patron_scan(const World&, ...)` is incremental and read-only.
- `FogOfWar::Mode::Avatar` = only the possessed body's eyes.
- Viewer: player commands are queued and run on the UI thread while it holds
  the world lock (never blocks on a sim cycle). Demo screenshot:
  `SDL_VIDEO_DRIVER=dummy SIMLAB_VDEMO=80 SIMLAB_VTAB=1 sunnyworld_viewer --selftest 4 --shot f.bmp`.
