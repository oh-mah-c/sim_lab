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
