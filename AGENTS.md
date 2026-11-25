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
  baseline hashes for SIMLAB_CYCLES 100/300/500/800 are d43a7c624c9084ec /
  fe23ce3faf80853c / 1270c51471227658 / d978b3ec11b5982d (re-baselined
  for Phase 19: quantum_observe writes math:/phenomenon:/qlaw: beliefs
  into agent minds and Driver's quantum_step/hunt-force read them, so
  the ledger diverges; content_hash is unchanged at ecdfe1d6288bf455 /
  acb20ad3a9575888 / 31e4d11aebf23ed6 /
  6f62e05292f578fb — only event ordering/counts moved). A pure
  optimization must reproduce them exactly.
- Locomotion stride is balance-critical: `move` actions use
  `ctx.parameters.ticks = 1` (~8 world units/step at speed 6000-8000).
  Raising the stride to even 2x collapses the capstone civilization
  (agents disperse to remote food clusters before reaching
  `adult_age`, then starve: dead≈39, zero lineages). `PredictionResult.
  ticks` is pinned to 1 inside `LocomotionModel::predict` so stride and
  world-time stay decoupled — do NOT raise it without redesigning the
  survival/metabolism balance and re-verifying emergence.

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

## Phase 14b — planetary globe & seamless zoom (viewer)

- Planet scale 2:1: `PrimordialParams::planet_w/planet_h` (1024x512,
  2048x1024 default in the menu, 4096x2048). The planet is ANALYTIC (pure
  field functions of the planet-coded worldgen seed, continental mask at
  planetary scales, uniform deep sea at the date line -> seamless E-W wrap);
  only a home region (`grid` x `grid*2/3`, best lowland longitude on the warm
  belt) is materialized + simulated. `planet_w = 0` keeps the old square
  planet (grid x grid).
- `simlab/render/Globe.hpp` (SDL-free): `PlanetMap` (albedo <= 2048x1024
  sampled once), `PlanetTiles` (32x32-cell colour tiles, LRU 384),
  `GlobeRenderer` (per-pixel inverse-projection table rebuilt only on
  radius/tilt change; integer N.L day/night from the world clock, rim glow,
  fog texels), `project/unproject`.
- Viewer: M / wheel-out at min zoom / pinch -> globe; drag spins; wheel/pinch
  toward the cursor; double click dives; crossing the radius threshold lands
  in 2.5D at that lon/lat. Surface wraps east-west and clamps at the poles;
  far terrain = analytic tiles under the region ground (ground texture is
  transparent only on planets). Mini-globe replaces the minimap on planets.
  Smoke test: `SDL_VIDEO_DRIVER=dummy SIMLAB_VPLANET=2048 SIMLAB_VGLOBE=1
  sunnyworld_viewer --selftest 4 [--shot prefix]` (exit 0 = globe, dive,
  wrap, wheel-out and hash checks passed).
- Planet ground (14b+): one chunked renderer (`draw_ground_chunks`, 32x32
  cells, 1 px/unit, LRU 96, 3 builds/frame, flat tile placeholder) for BOTH
  the simulated region (terrain grid: wear, lenses) and the dormant planet
  (analytic fields) -> no seam; unexplored = one dark haze everywhere.
  Valley worlds keep the single pre-composited ground texture.
- Menus: Title (Continue = newest of autosave/slots, New Game, Load, Quit;
  disabled entries when no save) and in-game Pause (Resume, Save, Load, New,
  Save & Quit to Title). Closing the window autosaves (`autosave.sav`).
  No world is built until New/Continue/Load; a decorative planet turns
  behind the title. Tests: `SIMLAB_PREF=<dir>/` redirects saves;
  `SIMLAB_VTITLE=1 sunnyworld_viewer [--shot p]` smoke-tests the flow.
- HUD (viewer): one slim top bar (play/pause + 1x/5x/20x/100x, day/hour,
  tribe, Resonance, Journal, View, Menu). Map overlays (former lenses 1-6),
  vision mode, panels and perf stats live in the View popup; status text is
  a fading toast; the Inspector opens only on selection; powers / possessed
  verbs sit in a bottom-left action bar; minimap / mini-globe bottom-right.

## Phase 16 — maritime physics, drowning & emergent watercraft

- All of it is gated by `World::kRulePrimordial` (legacy worlds keep water
  as a hard edge). Helpers in `lawutil` (LocomotionLaws.cpp):
  `water_depth` (sea = sea_level - elevation >= 20, basin lake 40, river
  12), `wave_at` (depth*3 + |wind|/4, 0..1000), `craft_of` (relation
  `aboard`), `sinking_load` (own excess over neutral buoyancy + 1000 g +
  gear), `swim_stroke_cost`, `craft_capacity_left`.
- Locomotion: depth <= `kWadeDepth` (25) = wade; deeper needs
  `note == "swim"`: fatigue += stroke cost, `swim_skill` +1, fatigue at 1000
  in deep water = -250 integrity per stroke ("drowning"/"drowned" in the
  evidence). `swim_stroke_cost` = (15 + depth/10 + wave/50 +
  |water_temp - normal_body_temp|/20 capped 600) x load x /(500+skill) —
  carried gear (wielding/wearing) sinks you via `sinking_load`; only
  entities with `normal_body_temp` pay the temperature term. Immersion
  heat loss is depth-scaled in thermoregulation: wade keeps the flat
  +80 ambient pull, swimming (>25 deep, not aboard) adds up to +140 —
  cold seas genuinely kill over a long crossing. Aboard a craft: on land
  the craft is hauled; in water the craft
  and every passenger move together, swell above `wave_rating` batters it
  ((wave-rating)/4+1 integrity), 0 = capsized (all thrown in); stepping
  from water onto land disembarks. `board` / `disembark` verbs (law 9):
  board needs reach, water within 2x reach, and craft lift >= the
  newcomer's sinking load on top of those aboard.
- Assembly (CompositionLaws): `float_capacity` = sum mass x (water - d)/d
  over the parts (material `density`), `wave_rating` = (150 + 25 x min(6,
  parts) + 0.6 x best part `waterproofing`) x motor precision. Raft of
  dry_wood ~250 (calm water); sealed with cured_hide ~640 (open sea). No
  craft names in code.
- Driver (game mode): `shore_sense` marches the heading stroke by stroke
  to the far shore and compares the cost (x (1000 + peril)/1000 x threat
  gain) with fatigue headroom; swim only if it fits (panic overrides),
  else board a remembered floating craft, else stay ashore and grow
  `want_cross` + remember `far_shore:x:y`. Beacons blocked by water don't
  fade. `want_cross >= 300` biases curiosity to bind lighter-than-water
  things (up to 4 parts) and to `board` them, never to disassemble a
  floating thing; `sail_step` paddles to the far shore first.
- Cognition: `maritime_lesson` — drowning/capsizing in an event's
  evidence raises `peril:deep_water` for the actor and every witness.
- Test: `test_maritime_swimming_drowning_and_watercraft` (legacy edge,
  depth/swell, swim + skill, drowning + witness fear, shore sense, raft vs
  sealed hull, sailing + landing, swell damage, the full emergent
  shore -> raft -> crossing chain, determinism).

## Phase 17 — trophic web, seasonal ranges & apex predators

- Data schema (data/properties.txt, domain tags matter): `trophic_tier`
  (0 flora .. 5 deep apex, observable so minds can read it off snapshots),
  `prey_tier_max` (highest tier a hunter may take; 0 = none), `scent_radius`,
  `herd_cohesion`, `territorial_radius`, `activity_phase` (0 any, 1
  diurnal, 2 nocturnal, 3 crepuscular — tick % 24 IS the hour),
  `fire_fear` (panic above fire_fear/2 suppresses hunting),
  `migration_drive` (permille per-tick fire chance), `anchor_x/y`
  (territory home, pinned on first drift), `growing_mask` +
  `dormant_growth_permille` (flora seasonality). NO species names in code.
- Seasons: `eco_season` (672 ticks/season, 2688/year) inside World.cpp;
  flora regrowth in `drift_needs` is scaled by `eco_growth_permille`, a
  weighted sum over the exact season counts of the dormant window —
  batched advance stays tick-equivalent. `growing_mask` is a bitmask over
  the four seasons (spring|summer|autumn = 7).
- Range drift lives inside `World::ecology_tick` (one "ecology" causal
  event per advance, same contract as influx): per-entity stateless hash
  schedule `stream_seed(seed,"range_drift",id*tick) % 1000 < drive`,
  gated by `eco_phase_active`. Suitability = the species' OWN declared
  `condition` bands evaluated on terrain fields plus the seasonal
  temperature offset (`eco_season_temp`) — migrants climb food/moisture
  gradients, territorial animals walk back inside `territorial_radius`
  of their anchor, `herd_cohesion` pulls toward same-kind neighbours.
  Two candidate rings (24 and 96 units) cross cell-scale gradients;
  hysteresis (+30) prevents jitter. Moves update pos + spatial index +
  traffic wear and are ledgered via event participants/evidence — the
  commit gate also accepts participants-only events so pure moves are
  never unledgered. The early-return guard scans for range-capable
  entities so a world with no seed bank/influx still migrates.
- Hunting is a Driver reflex (`hunt_step`, before the consume reflex):
  eligibility is `prey_tier_max > 0` + hunger/thirst > 400 +
  `activity_phase` + fire_fear/panic below threshold; prey is chosen
  from AgentMind MEMORY snapshots only (observable `trophic_tier` must
  be >= 1 and <= prey_tier_max, inside `hunt_range`/`scent_radius`).
  In reach: `consume` on dead/unconscious, `strike` on awake resisters;
  beyond: `move` toward the remembered position. World Truth is never
  queried for the decision — only for action legality inside interact.
- New species (all data): `migrant_herd` (tier 1 grazer, migration_drive
  600, herd_cohesion 700), `pack_stalker` (tier 3, ptm 1, nocturnal),
  `ridge_apex` (tier 4, ptm 3, nocturnal, territorial 800),
  `shore_reaver` (tier 5 deep apex, ptm 4 — reaches sapients and weak
  watercraft crews near_water), `commensal_rat` (tier 1 pest, fertility
  900 — booms without predators, raid stored food via the generic
  forage/consume path, zoonosis via the existing spillover system).
- Cascades are emergent, not scripted: overhunting releases flora
  (consume x regrow), predator loss releases pests (fertility vs
  hunt_step), pest pressure on provisions + vectors activate Phase 11
  spillover. The "ecology" batch event stays skipped by NarrativeComposer
  (physics noise); the meaningful beats — strikes, kills, starvation,
  disease — already narrate through their own verbs.
- Viewer: 7th map lens "Ecosystem" (CellView.fauna/flora census is
  fog-filtered at capture; apex tier>=4 gets a red ring, migrants a
  chevron), atlas creature sprites shift hue/saturation/value by
  `trophic_tier` so predators read darker. Atlas/body plans remain fully
  data-driven — no per-species render code.
- Test: `test_trophic_ecology_seasons_and_apex` (schema parse, seasonal
  migration across a temperature/moisture gradient, growing_mask
  spring-vs-winter fruit, tier-gated hunt killing grazer while the
  mid-tier predator never touches a tier-3 sapient, apex reaching
  sapients, batched-vs-stepped determinism).

## Phase 18 — domestication, agriculture, metallurgy & specialization

- Domestication is a `share`-verb side path in `predict_share`
  (SocialLaws.cpp), gated purely on data: target needs `tameability > 0`
  and `wildness` above `bond_threshold`. Feeding consumes up to 20 units
  of the shared `quantity`, cuts the animal's `hunger`, and drops
  `wildness` by `tameability * eaten / 100`; at/below threshold a
  `bonded` relation edge is committed through `rel_changes`. Bonded
  animals take the normal storage path for `share` — `pack_load_bonus`
  widens the target `quantity` cap, which is how pack cargo loads.
  `wildness` is a `gene` locus on the domesticable species, so lowered
  wildness is heritable through the existing recombination system.
- Husbandry runs inside `World::ecology_tick` (one "ecology" ledger event
  per advance, LOD-batched): bonded animals accrue `milk_credit` /
  `wool_credit` (spawn `milk`/`wool` material entities at thresholds,
  reduced when the partner is hungry), and a bonded watchdog
  (`scent_alert_radius > 0`) smells higher-tier predators inside its
  radius on `alert_cooldown` and writes a `peril:` belief into the
  bonded partner's AgentMind — belief only, never World Truth.
- Bonded-follow lives in `range_drift`: a bonded animal with no range
  props still drifts, steering toward its bonded partner's position
  (hysteresis +30 like migration); range-capable bonded animals prefer
  the bond pull over territory when the partner is outside
  `territorial_radius`.
- Agriculture verbs in BiologyLaws.cpp: `sow` consumes a propagule-like
  target and spawns the species its material/kind declares, stamping
  `cultivated=1`, `growth=0`, declared `growth_ticks`; `irrigate`
  requires a real water cell within `kIrrigateWaterScan` TERRAIN CELLS
  of the plant (cell-scaled, not world units) and grants
  `irrigation_credit`. `drift_needs` accrues `growth` toward
  `growth_ticks` (clamped) and decays `irrigation_credit`; fruit regrow
  is gated to `growth >= growth_ticks` and treats
  `irrigation_credit > 0` as moisture 950 for that plant's env calc —
  all integer, all ledgered StateChanges, batch-exact.
- Granaries are emergent: `CompositionLaws` derives `granary_seal =
  ins_fac / 2` on spawned enclosures from insulation/coverage (no
  building names). `drift_needs` perishability slows `decay_rate` for
  contained items `part_of`-related to an enclosure carrying
  `granary_seal` — physical storage, not a storage verb.
- Metallurgy stays on the generic `process` law: new materials
  (`tin_ore`, `bronze`, `iron_ingot`, `wool`, `wool_cloth`, `grain`,
  `pulse`, `milk`) carry `smelt_temp`/`ore_tag`/`malleability`/
  `smelt_yield` descriptors; new processes (`smelt_bronze`,
  `smelt_iron`, `weave_wool`) are pure data. Output quantity scales
  with process efficiency, which rises with temperature margin over
  `min_temperature` — tests must heat well past the threshold for full
  yield.
- Specialization substrate: `learn_from_event` reinforces a
  `craft:<verb>` belief for craft-family actions, alongside the
  existing `yield:<action>:<material>` beliefs — surplus-driven craft
  focus emerges from reinforcement, no profession assignment anywhere.
- New species (all data): `trail_hound`, `dairy_beast`, `fiber_beast`,
  `draft_beast` (domesticable: tameability/wildness/bond_threshold +
  milk_yield/wool_yield/pack_load_bonus/mount_capable/scent_alert_radius
  as declared), `cultivated_grain`, `cultivated_pulse` (sowable crops:
  cultivated/growth/growth_ticks/irrigation_credit). No engine branch
  on any species or material name.
- Narrative: `sow`/`irrigate`/`share`/`process` template lines cover the
  new verbs; the ecology batch event remains intentionally un-narrated.
- Test: `test_domestication_agriculture_metallurgy` (62 checks) —
  feed-to-bond chain, bonded follow + milk/wool yields, watchdog
  alert belief, sow/growth/irrigation/fruit gating, granary seal vs
  open spoilage, bronze/iron/weave process quantities.

## Phase 19 — quantitative cognition, emergent mathematics & treatises

- Two-tier physics cognition in `Cognition.cpp` (`quantum_observe`,
  called from `learn_from_event` for every self-witnessed causal event):
  Tier 1 `phenomenon:<domain>` beliefs (buoyancy/smelt/trajectory)
  accumulate qualitatively from successful craft-family events — no
  math required. Tier 2 needs `mathneed:<tier>` (count/ratio/geometry)
  to cross 550 with `mind_abstraction_gain >= 1100`, which "spends" the
  need and forges a `math:<tier>` belief; sustained craft events then
  prove `qlaw:<domain>:p<param>` (status Verified) once phenomenon >=
  650, the matching math tier >= 550 and abstraction >= 1200.
- Parameters are MEASURED, never declared: buoyancy = (best remembered
  composite's live `float_capacity` minus actor `mass`) / 1000 clamped
  [1,950] — physical props aren't in memory snapshots, so the scholar
  reads the remembered entity's live props (the shore_sense precedent);
  trajectory = hardest `hardness` among remembered materials clamped
  [100,950]; smelt = fuel share permille of the richest known process
  recipe. Repeated proofs reinforce the standing `qlaw:*:p*` belief
  (+20 conf) instead of re-deriving.
- Event->domain map is verb-only, no content names: process/combine ->
  smelt+ratio, impact/strike -> trajectory+geometry, sow/plant/dig/
  irrigate -> geometry, share/consume -> count; any event touching an
  entity with `float_capacity > 0` adds buoyancy and upgrades a `count`
  need to `ratio` (loading a hull IS a ratio problem).
- Indigenous mathematics (`math_base_of`/`math_render`, Cognition.hpp):
  each `language_id` hashes (with world seed) to a base in {8,10,12,20}
  and a glyph style {tally,dot-bar,bar,script}; values render
  positionally with a base-marker letter and a 'o' zero — never an
  Arabic digit. Deterministic per (lang, seed).
- Treatises: `quantum_tables` appends `M|tier|conf`, `P|domain|conf`,
  `Q|domain|param|conf` lines to the `inscription` text payload at
  `engrave` (confidence >= 100 only). The `read` parser imports them at
  half confidence (cap 500) as Observed claims — a transmitted law is
  usable via `qlaw_param` (needs >= 300) but stays unproven until the
  reader re-earns it.
- Planner use (Driver.cpp only — laws never read beliefs):
  `quantum_step` (cooldown kQuantum=48) loads remembered in-reach
  composites to exactly `qlaw_param("buoyancy")` via
  `parameters.force`, gated on surplus (hunger/thirst <= 300);
  `predict_share` honors `parameters.force` as the requested transfer
  amount (capped by the normal rules); the hunt kill-blow scales
  `natural_force` by `qlaw_param("trajectory")/100` when proven.
- Rosetta: `export_lexicon_codex` emits a `MATH lang <id> base <b>
  style <s> sample8=… sample13=…` line per language so players can
  decode each tongue's numeral system.
- Test: `test_quantitative_cognition_and_notation` (32 checks) —
  notation determinism, share->phenomenon->mathneed->math->qlaw chain
  for buoyancy, trajectory-via-anvil-replacement and smelt-with-restocked
  furnace, parameters.force law+driver paths, M/P/Q carve + halved
  import, determinism hash.

## CL-1 Phase 0 — brain substrate (no behavior change)

- `include/simlab/Brain.hpp` + `src/Brain.cpp`: the integer-only neural
  substrate for the CL-1 blueprint (`ai_architecture_blueprint.html`).
  Nothing is wired into World/Driver yet — Phase 0 is arena + kernels +
  LUTs + hash RNG only, so `state_hash` is untouched.
- `BrainArena`: deterministic bump + per-size-class LIFO free-list over
  one slab carved into 64-byte cells; `alloc`/`free` return slab-relative
  offsets (data pointers are 64B-aligned via a computed base offset).
  Allocation order is a pure function of the call sequence.
- Kernels: `brain_dot_i8` (sequential int32 accumulation — the ONLY
  accumulation order, so results are SIMD-width independent),
  `brain_dense_i8` (`y = act(clamp_i8((dot >> shift) + bias))`).
  Weights int8 [-127,127], activations int8 [-128,127].
- Activations: 256-entry LUTs indexed by the raw int8 bit pattern,
  built lazily once in fixed-point (tanh via Padé u(27+u²)/(27+9u²) in
  Q20; sigmoid = tanh(u/2) composed by index arithmetic; linear =
  identity). Bit-identical on every platform/build.
- Hash RNG: `brain_rand_weight`/`brain_mutation_step`/
  `brain_mutate_blocks` derive every draw from (seed, salt/entity,
  index/tick, block) via `Rng::hash` — replay-identical, no wall-clock.
- Layout helpers: `brain_padded_cols` (rows padded to 16 cols, zero
  pad), `brain_dense_weight_bytes`/`brain_dense_bias_bytes`.
- Test `test_brain_phase0` (322 checks total incl. suite): LUT sanity,
  kernel determinism + hand-checked row, hash-RNG replay, arena
  alloc/free/alignment/live-bytes, sparse block mutation bounds,
  and a GMAC/s micro-benchmark. Measured ~1.2-1.5 GMAC/s scalar int8
  GEMV (64x96) on the dev box — replaces the blueprint's assumed 3.

## CL-1 Phase 1 — sensor, replay buffer & world model (shadow mode)

- `include/simlab/Sensor.hpp` + `src/Sensor.cpp`: the 96-channel int8
  encoder. Channels 0-15 intero (hunger/thirst/fatigue/energy/
  blood_volume/pain/consciousness/integrity/temperature/toxin/age...,
  per-channel scale+sign+offset tables), 16-23 NeuroState, 24-71 the
  6 nearest REMEMBERED entities x 8 features (dx/dy/dist normalized by
  sight_range, integrity, quantity-as-bulk, edibility, engram charge,
  clarity — all from `AgentMind::memory` snapshots, never World Truth),
  72-79 terrain under the agent, 80-95 last-action one-hot over the
  fixed 16-verb table (`sensor_verb_table`). `sensor_label` turns a
  committed CausalEvent into 25 channels: 16 intero deltas, 8 outcome
  classes (success/met/unmet/spawned/changes/parents/evidence), reward
  = homeostatic cost improvement.
- `Brain.hpp` Phase-1 structs: `BrainSlot` (h[64] int16 Q8 GRU state,
  last_action, BrainPending pre-commit snapshot, 16-transition replay
  ring), `BrainStore` (arena + `std::map<EntityId,BrainSlot>` + WM
  error/surprise observability sums), `BrainLayout`/`brain_layout()`
  (embed 96->64, minGRU z/h 64->64, manager 64->32->8, worker
  (64+8)->64->16+attn, WM 80->96->25, value 64->1 — all int8 weights,
  int16 biases in one arena block per brain).
- `brain_pre` (before the law loop in `World::interact`) encodes s_t,
  runs embed->minGRU->WM forward, stashes `BrainPending` (yhat, WM
  hidden, post-GRU h). `brain_post` (on BOTH the failed-event and the
  committed-event paths) builds the label, sigma = min(1000, L1/8),
  charge from neuro panic/rage/grief, stratified-replaces the
  min(charge+sigma) replay slot, then takes ONE integer SGD step on
  the WM output layer only (Huber-lite err, eta = 384 + sigma/2,
  dw clamp +-8, bias clamp +-4095). Manager/Worker/Value weights are
  allocated+hash-initialized but not yet forwarded (Phase 2 heads).
- Gating: `World::brain_shadow` (default false) + `DriverState.
  brain_shadow` propagated per cycle. Both hooks and `brain_gc`
  (called at the end of `decay_minds`) early-return on
  `w.speculative` — speculative clones share `brains_` via shared_ptr
  but must never allocate slots or write weights. With the flag off,
  `brains_` stays null and NOTHING runs: capstone state_hash is
  bit-identical (verified d978b3ec11b5982d @ 800).
- `brain_state_hash(store)`: FNV-1a over sorted slots — weights, h,
  replay, stats. NOTE: `BrainSlot::woff` is deliberately NOT hashed —
  it embeds the arena's heap-alignment slack (`slab_base_off_`), which
  is pointer-dependent; woff is allocator bookkeeping, not brain state.
- Snapshot observability: `RenderSnapshot` gains `brain_slots`,
  `brain_wm_mae`, `brain_avg_surprise` (-1 when shadow off).
- Test `test_brain_phase1` (143 checks): flag-off creates no store;
  flag-on state_hash == flag-off; two same-seed shadow worlds produce
  identical `brain_state_hash` (component-level diff diagnostics kept
  in the test); WM MAE windowed mean drops 7.55 -> 5.52 over 900
  rest/strike interacts; dead agents release arena slots via
  `advance_ticks` -> `decay_minds` -> `brain_gc`.

## CL-1 Phase 2 — heritable brains, CL-1 decision path, save v20

- `BrainGenome` (Brain.hpp): the Baldwin inheritance unit — `hidden`
  ({32,48,64,96} via `snap_hidden`), `lr_permille`, `trace_decay`,
  `mut_scale`, and the `slow` int8 weight block. Fast weights/h/replay
  are NEVER inherited: `brain_install_spawn` zeroes them at birth.
- Layout is now per-hidden-size: `brain_layout_h(H)` (cached per H);
  `brain_slot(w,store,id,genome?)` installs `genome->slow` verbatim when
  sizes match, else hash-inits and zeroes int16 biases. H=64 layout is
  byte-identical to Phase 1. `BrainArena` offsets are now cell-grid
  relative (`w8` adds `slab_base_off_`) so `reserve()` reallocs never
  invalidate handed-out offsets; `alloc` exhaustion auto-grows.
- `brain_decide(w,npc)` — the CL-1 controller: sensor_encode -> embed ->
  minGRU (on a COPY of h — decide never commits recurrent state) ->
  manager 32->8 drive logits + NeuroState personality bias -> worker
  (h8++drive)->64->16 verb logits + 16-dim attention query -> mask to
  `known_actions` (+move/rest) -> top-C=5 (verb,target) over the 6
  nearest remembered entities -> N=`mind_plan_horizon` WM imagination
  rollouts (discount `trace_decay`/1024) + terminal `Value` -> argmax
  with a `mut_scale`-scaled deterministic jitter. Returns
  `BrainChoice{verb,target,move_to}`.
- Inheritance: `reproduce()` (BiologyLaws.cpp) reads
  `brain_genome_of` for both parents when `w.brains_` exists; refuses
  the pairing (`r.failure="brain genomes incompatible (reproductive
  isolation)")` on hidden mismatch or
  `brain_genetic_distance > kMaxBrainGeneticDistance` (=180 permille of
  the int8 range, mean |dw|), else `SpawnSpec::brain =
  brain_child_genome` — hyper-gene pick + 64-weight block crossover
  (bit of `Rng::hash(seed,a,b,tick,block)`) + `brain_mutate_blocks` at
  the child's `mut_scale`, int16 biases re-clamped. Spawn-commit calls
  `brain_install_spawn`.
- `state_hash()` now mixes `brain_state_hash(*brains_)` whenever
  `brains_ && !speculative` — live brain weights/h/replay are World
  Truth from Phase 2 on. Worlds that never allocate a store hash
  exactly as before.
- Save format v20 serializes the whole BrainStore: arena
  capacity/high-water/live-bytes + per-size free lists, then per slot
  (sorted EntityId) hidden/hypers/woff/wbytes/weights/h/last_action/
  replay ring + WM stats. `BrainArena::restore_state` rebuilds
  bookkeeping; weights memcpy back at their offsets. v19 and older
  loads leave `brains_` null (lazy hash-init on next use).
- Driver: `DriverState::brain_control` (DEFAULT OFF) routes the
  cognition slot through `brain_step` -> `brain_decide` ->
  `InteractionContext` -> `interact()` before `discover_step`; the
  legacy plan_multi path remains the fallback when the brain cannot
  decide. `w.brain_shadow` is set when EITHER flag is on so the WM
  learn hooks still run under control. Default-off is deliberate:
  an untrained brain fires every cycle and starves the social reflexes
  (reproduce/communicate/engrave) — the capstone civilization collapses
  (alive 29 vs ~200, repro 6). plan_multi/clone_local are NOT yet
  removed; that is a Phase-3 decision once the WM has a training
  curriculum. `mind_brain_work` prop exists for the future MAC->energy
  metabolic charge (not yet wired).
- `data/properties.txt` gained `mind_brain_work`, `brain_hidden`,
  `brain_lr`, `brain_trace_decay`, `brain_mut_scale` — APPENDED LAST:
  PropId order feeds `Genome::loci` sorting and `inherit_allele`'s
  per-index draws, so mid-file insertion silently re-rolls all
  inheritance. `data/species.txt` deliberately has NO `gene brain_*`
  lines yet: loci count feeds `heredity_hash(...,loci.size(),...)`
  speciation draws, and +4 loci flipped an emergent-history assertion
  (beacon scout). Brain hyper-genes inherit through `BrainGenome`
  regardless; species-level drift can be added with a re-baseline.
- Test `test_brain_phase2` (69,737 checks incl. per-byte loops):
  decide determinism + h not committed, crossover validity/range/
  fast-state-empty/byte-exact install/double-call determinism,
  isolation on hidden-mismatch and saturated-weight divergence,
  state_hash sensitivity to a single weight perturbation, and a
  save->load->continue bit-identical `state_hash` +
  `brain_state_hash` round-trip.

## CL-1 Phase 3 — metabolism, plasticity, respiration, NarrativeHost

- Metabolic cost: `brain_decide` accumulates an estimated MAC count in
  `BrainSlot::mac_pending`/`mac_frac` (hashed into `brain_state_hash`,
  serialized). `drift_needs` (World.cpp) converts it to an `energy`
  StateChange via `mind_brain_metabolic_cost` permille and records
  `mind_brain_work` as a causal prop — big brains/deep rollouts eat.
- Respiration (PhysiologyLaws `anatomy_drift`, opt-in on `oxygen`
  prop): `organ_lung_gain`/`organ_gill_gain` read genome loci first
  then props; exchange `(gain*50/1000 - 4)` permille/tick runs OUTSIDE
  the organ gate so organ-less entities suffocate at -4/tick. Organ
  upkeep `(lung+gill)` drains via `resp_accum` Euclid remainder /50.
  `oxygen<350` decays `blood_volume`/`consciousness` and raises pain.
  Props appended at END of properties.txt (PropId-order invariant).
- Plasticity: `Transition` stores WM-hidden (`wh`); `BrainSlot::fast`
  (24,00 bytes) + `anchor` (birth-WM2 slow snapshot). `wm2_forward`
  merges slow+fast for inference; `brain_post` runs fast-weight SGD
  (eta_f = 1.5*eta, clamped +-96). `rest` verb triggers sleep
  consolidation: top-4 replay SGD + fast->slow transfer >>2 + L2-SP
  anchor pull >>4 (catastrophic-forgetting guard). All integer-only.
- Driver social floor: `brain_step` runs section-5 reproduce/
  communicate predicates FIRST inside the brain gate, then consults
  CL-1 only on `(now+npc)&7==0` cycles (sparse advisor ~1-in-8) and
  only when no urgent need (hunger/thirst>500 -> legacy path). The
  sparse design is the fix for the Phase-2 collapse: `brain_pre`/
  `brain_post` learn on EVERY interact regardless of chooser, so
  reflex-driven behavior still trains the WM.
- `brain_control` remains DEFAULT OFF. Curriculum parity achieved:
  `test_brain_curriculum` runs brain vs control worlds 600 cycles;
  alive>=1 in each and brain social-floor acts >= control-2.
- NarrativeHost (include/simlab/NarrativeHost.hpp + src/
  NarrativeHost.cpp): pimpl, `dlopen`s libllama from
  `third_party/llama-build/bin` (+ggml backends) — NO link-time dep;
  absent libs -> `active()==false`, safe-disable. Generation uses the
  modern `llama_batch_ext`/`llama_process` API (deprecated
  `llama_batch_get_one`/`llama_batch_free` caused free() corruption).
  Worker `std::jthread`, bounded deque; results keyed by EventId/
  EntityId for UI. NEVER touches state_hash — runs fully async.
  Makefile: -Ithird_party/lang-host includes + -ldl for tests.
- Tests: `test_amphibian_evolution` (7), `test_brain_curriculum`
  (determinism + control parity), `test_narrative_host` (7, real Qwen
  output + safe-disable path), plus a 2000-agent decide benchmark in
  curriculum (~12.5 ms/frame, under the 16.7 ms budget).
- Viewer wiring (Phase 3 completion): `sunnyworld_viewer` auto-starts
  the Qwen chronicler on real runs — `Viewer::start_llm()` spawns
  `llm_starter` (model load off the UI thread; ~Viewer joins it).
  Paths resolve CWD-first then relative to the exe (`/proc/self/exe`,
  `../` and `../../`); env overrides `SIMLAB_LLM_MODEL`,
  `SIMLAB_LLM_LIB`, `SIMLAB_NO_LLM=1` disables. Selftests/`--shot`
  paths skip it. `llm_scan` (in poll_world, frames%4==2) walks new
  ledger events and submits salience proxies for |charge|>=800 —
  death, successful reproduce, sapient spawn, or >=400 pain/integrity
  hit — with per actor+class cooldown (480/960 ticks) and <=4 submits
  per scan; `llm_drain` re-injects finished text as
  `NarrativeLine{llm=true}` (gold in Chronicle + callouts). The host
  is presentation-only: state_hash identical on/off.
  `DriverState.brain_control = true` is now the viewer default
  (ctor + reset_view) — CL-1 sparse-advisor + social floor drives
  sapients; `brain_shadow` hooks still train the WM on every interact.
