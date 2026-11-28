#include "simlab/Driver.hpp"
#include "simlab/Cognition.hpp"
#include "simlab/Discovery.hpp"
#include "simlab/Laws.hpp"
#include "simlab/WorldGen.hpp"
#include "simlab/Brain.hpp"
#include "simlab/Sensor.hpp"
#include "LawUtil.hpp"
#include <algorithm>
#include <chrono>
#include <iterator>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <set>
#include <string>
#include <thread>

namespace simlab {

namespace {

constexpr std::int64_t kCooldown[7] = {48, 24, 220, 32, 96, 128, 8};
// slots: 0=wield, 1=withdraw, 2=reproduce, 3=communicate, 4=engrave,
//        5=found, 6=consume (reflex)

// Phase-9 budgets (deterministic — derived from sorted agent ids, never RNG):
//  * kDiscoverPerCycle: at most this many agents run discover_step() per
//    tick — the expensive path (speculative clones). Agents are processed
//    in sorted-id order so the budget grant is deterministic.
//  * kPendingTicks: how long an ExploreGoal waypoint stays cached before
//    the agent reconsiders (re-plan) — a walking agent does not re-run
//    hypothesis generation every tick.
// Population is NOT capped here: births are limited by the conservation
// laws in BiologyLaws (parents must pay energy+hunger), not by a counter.
constexpr int kDiscoverPerCycle = 8;
constexpr std::uint64_t kPendingTicks = 240;

enum VerbSlot { kWield = 0, kWithdraw, kReproduce, kCommunicate, kEngrave, kFound, kConsume,
                kBabble, kSite, kQuantum };

PropId dpid(const World& w, const char* n) {
    static thread_local LitPidCache c_;
    return c_.get(w.props, n);
}

std::int64_t dint(const World& w, const Entity& e, PropId p, std::int64_t fb = 0) {
    return p == kNoProp ? fb : w.prop_int(e, p, fb);
}

bool cooled(const DriverState* st, EntityId npc, int slot, std::uint64_t now) {
    if (!st) return true;
    const auto it = st->next_try.find({npc, slot});
    return it == st->next_try.end() || it->second <= now;
}
void cool(DriverState* st, EntityId npc, int slot, std::uint64_t now,
          std::int64_t ticks) {
    if (st) st->next_try[{npc, slot}] = now + static_cast<std::uint64_t>(ticks);
}

// World-Truth reproduction cooldown (Phase 9): `last_reproduce` vs
// `repro_cooldown` props — the law enforces it, the driver just avoids
// spending an interact on a guaranteed refusal.
bool repro_ready(const World& w, const Entity& e, std::uint64_t now) {
    const PropId lp = dpid(w, "last_reproduce");
    if (lp == kNoProp) return true;
    const PropId cp = dpid(w, "repro_cooldown");
    const std::int64_t cd = cp != kNoProp ? w.prop_int(e, cp, 200) : 200;
    if (cd <= 0) return true;
    const std::int64_t last = w.prop_int(e, lp, -cd);
    return static_cast<std::int64_t>(now) - last >= cd;
}

InteractionContext mk(EntityId actor, std::vector<EntityId> targets,
                      const char* action, std::int64_t ticks = 1) {
    InteractionContext c;
    c.actor = actor; c.targets = std::move(targets);
    c.parameters.action = action; c.parameters.ticks = ticks;
    return c;
}

// walking stride per action. Legacy/capstone: 1 (balance-critical, see
// AGENTS.md). Game mode (concurrent rounds): 4 — a round is one action per
// agent, so 1-tick strides made people crawl ~5 units per round on screen.
// LocomotionModel pins the world-time cost to 1 tick either way.
std::int64_t move_stride(const DriverState* st) { return st && st->concurrent ? 4 : 1; }

// Phase 19 quantitative planner: a mind holding a proven `qlaw:buoyancy`
// law loads a remembered craft only to the measured safe load
// (parameters.force = the law's parameter, in trade units) instead of
// dumping the whole pack aboard and hoping. Surplus only — a hungry
// carrier eats first. Claims arrive halved, so a taught law still works,
// just conservatively until re-proven.
bool quantum_step(World& w, DriverState* st, EntityId npc, std::uint64_t now) {
    const AgentMind* mm = w.find_mind(npc);
    const Entity* me = w.find(npc);
    if (!mm || !me || !me->alive) return false;
    const std::int64_t load = qlaw_param(*mm, "buoyancy");
    if (load <= 0) return false;
    if (dint(w, *me, dpid(w, "hunger"), 0) > 300 ||
        dint(w, *me, dpid(w, "thirst"), 0) > 300) return false;
    const PropId qty = dpid(w, "quantity");
    if (qty == kNoProp || w.prop_int(*me, qty, 0) < load) return false;
    if (!cooled(st, npc, kQuantum, now)) return false;
    const std::int64_t rr = dint(w, *me, dpid(w, "reach"), 64);
    const std::int64_t r2 = rr * rr;
    for (const auto& [kid, snap] : mm->memory) {
        if (kid == npc || snap.kind_name != "composite") continue;
        if (dist2(me->pos, snap.pos) > r2) continue;
        const Entity* c = w.find(kid);
        if (!c || !c->alive) continue;
        cool(st, npc, kQuantum, now, 48);
        InteractionContext sc = mk(npc, {kid}, "share", 1);
        sc.parameters.force = load;      // the proven safe load, units
        if (w.interact(sc).completed) return true;
    }
    return false;
}

// ---- CL-1 Phase 2: brain-driven action --------------------------------------
// The brain proposes (verb, target, destination); the Driver only maps it
// onto an InteractionContext — all law checks still run inside interact().
// Returns true when the brain produced a usable choice and interact()
// committed (or at least attempted) it.
EntityId nearest_agent(const World& w, EntityId npc, std::int64_t range2);

// Phase 3 social floor: with brain_control on, CL-1 owns STRATEGIC choice
// but the civilization-critical reflexes (reproduce, communicate) still
// fire first — an untrained brain must not starve the species of its
// reproductive loop. Mirrors the section-5 predicates exactly.
bool social_floor(World& w, DriverState* st, EntityId npc, std::uint64_t now) {
    const Entity* me = w.find(npc);
    if (!me || !me->alive) return false;
    const PropId hunger = dpid(w, "hunger");
    const PropId age = dpid(w, "age"), adult = dpid(w, "adult");
    const PropId fert = dpid(w, "fertility"), energy = dpid(w, "energy");
    const PropId crange = dpid(w, "communication_range"),
               reach = dpid(w, "reach");
    // reproduce
    if (cooled(st, npc, kReproduce, now) && repro_ready(w, *me, now) &&
        !me->genome.loci.empty() &&
        dint(w, *me, age, 0) >= dint(w, *me, adult, 100) &&
        dint(w, *me, fert, 0) > 0 &&
        (hunger == kNoProp || dint(w, *me, hunger, 0) <= 500) &&
        dint(w, *me, energy, 40000) >= 40000) {
        const std::int64_t rr = dint(w, *me, reach, 64);
        const EntityId partner = nearest_agent(w, npc, rr * rr);
        if (partner != kNoEntity) {
            const Entity* p = w.find(partner);
            if (p && dint(w, *p, age, 0) >= dint(w, *p, adult, 100) &&
                dint(w, *p, fert, 0) > 0 &&
                (hunger == kNoProp || dint(w, *p, hunger, 0) <= 500) &&
                dint(w, *p, energy, 0) >= 40000 &&
                !p->genome.loci.empty()) {
                cool(st, npc, kReproduce, now, kCooldown[kReproduce]);
                if (w.interact(mk(npc, {partner}, "reproduce")).completed)
                    return true;
            }
        }
    }
    // communicate
    const std::int64_t cr2 = dint(w, *me, crange, 0);
    if (cr2 > 0 && cooled(st, npc, kCommunicate, now)) {
        const EntityId partner = nearest_agent(w, npc, cr2 * cr2);
        if (partner != kNoEntity) {
            const AgentMind* om = w.find_mind(partner);
            const AgentMind* mm = w.find_mind(npc);
            if (om && mm && om->known_actions != mm->known_actions) {
                cool(st, npc, kCommunicate, now, kCooldown[kCommunicate]);
                if (w.interact(mk(npc, {partner}, "communicate")).completed)
                    return true;
            }
        }
    }
    return false;
}

// Open-ended invention: a brain-chosen "combine" carries only ONE target,
// but synthesis needs two reagents. Pair it with the nearest OTHER
// remembered material-bearing entity — the same pick brain_decide uses
// when pricing the epistemic bonus, so imagined and executed combos key
// identically in the tried-set.
static EntityId second_material_target(const World& w, EntityId npc,
                                       EntityId exclude) {
    const Entity* me = w.find(npc);
    const AgentMind* mind = w.find_mind(npc);
    if (!me || !mind) return kNoEntity;
    EntityId best = kNoEntity;
    std::int64_t bd2 = 0;
    for (const auto& [kid, snap] : mind->memory) {
        if (kid == npc || kid == exclude || snap.material == kNoMaterial)
            continue;
        const std::int64_t dx = snap.pos.x - me->pos.x;
        const std::int64_t dy = snap.pos.y - me->pos.y;
        const std::int64_t d2 = dx * dx + dy * dy;
        if (best == kNoEntity || d2 < bd2) { best = kid; bd2 = d2; }
    }
    return best;
}

bool brain_step(World& w, DriverState* st, EntityId npc, std::uint64_t now,
                DriverTally* tally) {
    if (!st || !st->brain_control || w.speculative) return false;
    const Entity* me = w.find(npc);
    if (!me || !me->alive) return false;
    if (st->legacy_planner_compat) {
        // ---- Phase-3 stabilized hybrid (regression-suite mode) --------------
        if (social_floor(w, st, npc, now)) {
            if (tally) ++tally->social;
            return true;
        }
        // Survival gate: a desperate agent (hunger/thirst > 500, same
        // threshold the consume reflex uses) hands control back to the
        // legacy planner — an untrained brain must not starve its body.
        const PropId hp = dpid(w, "hunger"), tp = dpid(w, "thirst");
        if ((hp != kNoProp && dint(w, *me, hp, 0) > 500) ||
            (tp != kNoProp && dint(w, *me, tp, 0) > 500))
            return false;
        // Sparse consultation: 1-in-8 cycles per agent.
        if (((now + static_cast<std::uint64_t>(npc)) & 7) != 0) return false;
        BrainChoice ch = brain_decide(w, npc);
        if (!ch.valid) return false;
        InteractionContext c = mk(npc, {}, ch.verb.c_str(), 1);
        if (ch.target != kNoEntity) c.targets.push_back(ch.target);
        if (ch.verb == "combine" && ch.target != kNoEntity) {
            const EntityId t2 = second_material_target(w, npc, ch.target);
            if (t2 != kNoEntity) c.targets.push_back(t2);
        }
        if (ch.has_move) {
            c.parameters.destination = ch.move_to;
            c.parameters.has_destination = true;
            c.parameters.ticks = move_stride(st);
        }
        w.interact(c);
        return true;
    }
    // ---- 100% neural mode: CL-1 owns every cycle ---------------------------
    // No social floor, no urgent-need bailout, no sparsity, no plan_multi
    // fallback — natural selection is the only safety net. An undecidable
    // brain rests (which also runs sleep consolidation on its replay).
    // parallel_think: replay the choice pre-computed against the frozen
    // round-start world; otherwise decide at this agent's turn (sees all
    // earlier same-round actions, exactly like the legacy path).
    BrainChoice ch;
    const auto cit = st->brain_last.find(npc);
    if (st->parallel_think && st->think_round == now &&
        cit != st->brain_last.end()) {
        ch = cit->second;
    } else {
        ch = brain_decide(w, npc);
    }
    InteractionContext c = ch.valid ? mk(npc, {}, ch.verb.c_str(), 1)
                                    : mk(npc, {}, "rest", 1);
    if (ch.target != kNoEntity) c.targets.push_back(ch.target);
    if (ch.verb == "combine" && ch.target != kNoEntity) {
        const EntityId t2 = second_material_target(w, npc, ch.target);
        if (t2 != kNoEntity) c.targets.push_back(t2);
    }
    if (ch.has_move) {
        c.parameters.destination = ch.move_to;
        c.parameters.has_destination = true;
        c.parameters.ticks = move_stride(st);
    }
    w.interact(c);
    return true;
}

// CL-1 Phase 4b: deterministic parallel think pre-pass. Runs at round open,
// while the world is still frozen (no interacts yet this round). Every
// agent's brain_decide is a pure function of (round-start world, own brain
// slot) — per-slot writes (last_action, mac_pending) are disjoint, so any
// partition/thread count yields identical BrainChoice results. Choices are
// staged per-thread and merged in agent order; brain_step replays them.
static void parallel_think_pass(World& w, DriverState* st,
                                const std::vector<EntityId>& agents,
                                std::uint64_t now) {
    st->brain_last.clear();
    st->think_round = now;
    if (!w.brains_) w.brains_ = std::make_shared<BrainStore>();
    BrainStore& store = *w.brains_;
    // Sequential slot pre-creation: BrainArena may grow its slab and the
    // slots map must not be inserted into concurrently.
    for (EntityId id : agents) {
        const Entity* e = w.find(id);
        if (e && e->alive && w.find_mind(id)) brain_slot(w, store, id);
    }
    const std::size_t n = agents.size();
    if (n == 0) return;
    // Small populations: spawning a thread pool costs more than the
    // decides it saves — leave think_round stale so brain_step decides
    // live, exactly as if parallel_think were off.
    std::size_t minds = 0;
    for (EntityId id : agents) {
        const Entity* e = w.find(id);
        if (e && e->alive && w.find_mind(id)) ++minds;
    }
    if (minds < 48) { st->think_round = 0; return; }
    const unsigned hw = std::thread::hardware_concurrency();
    int T = st->think_threads > 0 ? st->think_threads
                                  : static_cast<int>(hw ? hw : 2);
    T = std::max(1, std::min(T, 16));
    T = std::min<int>(T, static_cast<int>(agents.size()));
    if (T <= 1) {
        for (EntityId id : agents) {
            const Entity* e = w.find(id);
            if (!e || !e->alive || !w.find_mind(id)) continue;
            const BrainChoice ch = brain_decide(w, id);
            if (ch.valid) st->brain_last[id] = ch;
        }
        return;
    }
    std::vector<std::vector<std::pair<EntityId, BrainChoice>>> parts(T);
    auto work = [&](int t) {
        const std::size_t lo = n * static_cast<std::size_t>(t) /
                               static_cast<std::size_t>(T);
        const std::size_t hi = n * static_cast<std::size_t>(t + 1) /
                               static_cast<std::size_t>(T);
        for (std::size_t i = lo; i < hi; ++i) {
            const EntityId id = agents[i];
            const Entity* e = w.find(id);
            if (!e || !e->alive || !w.find_mind(id)) continue;
            const BrainChoice ch = brain_decide(w, id);
            if (ch.valid) parts[static_cast<std::size_t>(t)]
                              .emplace_back(id, ch);
        }
    };
    std::vector<std::thread> pool;
    pool.reserve(static_cast<std::size_t>(T) - 1);
    for (int t = 1; t < T; ++t) pool.emplace_back(work, t);
    work(0);
    for (auto& th : pool) th.join();
    for (auto& p : parts)
        for (auto& [id, ch] : p) st->brain_last[id] = ch;
}

// ---- shore sense (game mode, Primordial ruleset) ---------------------------
// Before a step that would leave the ford for deep water, a mind weighs the
// crossing it can SEE: strokes to the far shore along its heading (cost from
// the same physics the law uses) against the fatigue it has left, inflated
// by fear learned from drownings (peril:deep_water) and its threat gain.
//   0 = go (maybe annotated "swim"), 1 = acted otherwise (boarded a craft),
//   2 = stay ashore (and the wish to cross — want_cross — grows).
// Panic overrides the weighing: a fleeing mind jumps in.
int shore_sense(World& w, DriverState* st, EntityId npc, InteractionContext& mc, bool panic) {
    if (!(st && st->curiosity) || !(w.ruleset & World::kRulePrimordial) || w.terrain.empty()) return 0;
    Entity* me = w.find(npc);
    if (!me || !mc.parameters.has_destination) return 0;
    if (lawutil::craft_of(w, npc) != kNoEntity) return 0;             // sailing: the law steers
    const Vec2 from = me->pos, to = mc.parameters.destination;
    const std::int64_t dx = std::int64_t(to.x) - from.x, dy = std::int64_t(to.y) - from.y;
    const std::int64_t d2 = dx * dx + dy * dy;
    if (d2 == 0) return 0;
    std::int64_t dist = 1; while (dist * dist < d2) ++dist;
    const std::int64_t speed = dint(w, *me, dpid(w, "speed"), 0);
    const std::int64_t step = std::max<std::int64_t>(1, speed * std::max<std::int64_t>(1, mc.parameters.ticks) / 1000);
    auto at = [&](std::int64_t k) {
        return Vec2{static_cast<std::int32_t>(from.x + dx * k / dist), static_cast<std::int32_t>(from.y + dy * k / dist)};
    };
    const Vec2 first = at(std::min(step, dist));
    if (lawutil::water_depth(w, first) <= lawutil::kWadeDepth) return 0;   // dry or a ford
    // march the heading stroke by stroke until the far shore (or give up)
    std::int64_t cost = 0; bool shore = false;
    for (int i = 1; i <= 96; ++i) {
        const Vec2 p = at(step * i);
        if (!w.terrain.contains(p)) break;                               // open sea beyond sight
        if (lawutil::water_depth(w, p) == 0) { shore = true; break; }
        cost += lawutil::swim_stroke_cost(w, *me, p);
    }
    const AgentMind* mm = w.find_mind(npc);
    const MindParams mp = mind_params(w, *me);
    const Belief* peril = mm ? mm->find_belief("peril:deep_water") : nullptr;
    const std::int64_t fear = peril ? peril->confidence : 0;
    const std::int64_t headroom = 1000 - dint(w, *me, dpid(w, "fatigue"), 0) - 80;
    const std::int64_t need = cost * (1000 + fear) / 1000 * std::max<std::int64_t>(500, mp.threat_gain) / 1000;
    if (panic || (shore && need <= headroom)) { mc.parameters.note = "swim"; return 0; }
    // too far to swim: a remembered craft in reach?
    if (mm && mm->known_actions.count("board")) {
        const std::int64_t rr = dint(w, *me, dpid(w, "reach"), 64);
        const PropId fc = dpid(w, "float_capacity");
        for (const auto& [kid, snap] : mm->memory) {
            if (snap.kind_name != "composite" || dist2(snap.pos, me->pos) > rr * rr) continue;
            const Entity* c = w.find(kid);
            if (!c || !c->alive || fc == kNoProp || w.prop_int(*c, fc, 0) <= 0) continue;
            InteractionContext bc = mk(npc, {kid}, "board", 1);
            if (w.interact(bc).completed) return 1;
        }
    }
    AgentMind& m = w.mind(npc);
    Belief& b = m.belief("want_cross");
    b.confidence = std::min<std::int64_t>(1000, b.confidence + 60);
    b.last_tick = w.clock_.tick();
    // remember WHERE it wanted to go: the far shore outlives the errand
    for (auto it = m.beliefs.lower_bound("far_shore:"); it != m.beliefs.end() && it->first.rfind("far_shore:", 0) == 0;)
        it = m.beliefs.erase(it);
    Belief& fs = m.belief("far_shore:" + std::to_string(to.x) + ":" + std::to_string(to.y));
    fs.confidence = 900; fs.last_tick = w.clock_.tick();
    return 2;
}
// interact a planned move through the shore sense; true if the turn was used
bool drive_move(World& w, DriverState* st, EntityId npc, InteractionContext& mc, bool panic = false,
                bool* shore_blocked = nullptr) {
    const int sense = shore_sense(w, st, npc, mc, panic);
    if (shore_blocked) *shore_blocked = sense == 2;
    if (sense == 1) return true;
    if (sense == 2) return false;
    return w.interact(mc).completed;
}
// afloat with a far shore in mind: paddle there first (Primordial game mode)
bool sail_step(World& w, DriverState* st, EntityId npc) {
    if (!(st && st->curiosity) || !(w.ruleset & World::kRulePrimordial)) return false;
    if (lawutil::craft_of(w, npc) == kNoEntity) return false;
    const AgentMind* mm = w.find_mind(npc);
    if (!mm) return false;
    const auto it = mm->beliefs.lower_bound("far_shore:");
    if (it == mm->beliefs.end() || it->first.rfind("far_shore:", 0) != 0) return false;
    long long x = 0, y = 0;
    if (std::sscanf(it->first.c_str() + 10, "%lld:%lld", &x, &y) != 2) return false;
    const std::string key = it->first;
    InteractionContext mc = mk(npc, {}, "move", move_stride(st));
    mc.parameters.destination = {std::int32_t(x), std::int32_t(y)};
    mc.parameters.has_destination = true;
    const InteractionResult r = w.interact(mc);
    if (lawutil::craft_of(w, npc) == kNoEntity)              // stepped ashore: crossed
        w.mind(npc).beliefs.erase(key);
    return r.completed;
}


// ---- Primordial curiosity: affordance babbling (DriverState::curiosity) ----
// An idle mind with an incomplete verb repertoire pokes at the world: it
// picks a verb it has never used (or the one a whispered omen suggests) and
// tries it on things it REMEMBERS within reach. Nothing here grants
// knowledge: the attempt is an ordinary interact(); the laws decide, and only
// a completed event teaches the verb (learn_from_event) — witnesses copy it.
// Personality decides the tempo and whether an omen is heeded:
//   openness = abstraction + reward/2 - threat/2   (neutral 1000)
// Deterministic: choices come from a hash of (seed, agent, tick).
const char* const kBabbleVerbs[] = {
    "impact", "strike", "dig", "chop", "carve", "assemble", "disassemble",
    "process", "heat", "wield", "combine", "consume", "treat", "board"};

std::uint64_t bhash(std::uint64_t a, std::uint64_t b, std::uint64_t c) {
    std::uint64_t x = a * 0x9E3779B97F4A7C15ull ^ (b + 0x632BE59BD9B4E019ull) ^
                      (c * 0xD6E8FEB86659FD93ull);
    x ^= x >> 31; x *= 0xBF58476D1CE4E5B9ull; x ^= x >> 29;
    x *= 0x94D049BB133111EBull; x ^= x >> 32;
    return x;
}

std::int64_t babble_openness(const MindParams& mp) {
    return std::clamp<std::int64_t>(
        mp.abstraction_gain + mp.reward_gain / 2 - mp.threat_gain / 2, 200, 2500);
}

bool law_allows(const World& w, const InteractionContext& c) {
    for (const auto& m : w.laws) {
        if (!m->supports(w, c)) continue;
        if (m->predict(w, c).possible) return true;
    }
    return false;
}

bool babble_step(World& w, DriverState* st, EntityId npc, std::uint64_t now) {
    const Entity* me = w.find(npc);
    const AgentMind* mm = w.find_mind(npc);
    if (!me || !mm) return false;
    const MindParams mp = mind_params(w, *me);
    const std::int64_t open = babble_openness(mp);
    // tempo: open minds poke often, anxious ones rarely; need pressure
    // (hunger/thirst > 600) doubles the drive.
    std::int64_t cd = std::clamp<std::int64_t>(40 * 1000 / open, 12, 200);
    const bool pressed = dint(w, *me, dpid(w, "hunger"), 0) > 600 ||
                         dint(w, *me, dpid(w, "thirst"), 0) > 600;
    if (pressed) cd = std::max<std::int64_t>(6, cd / 2);

    // omen: strongest `omen:` belief; heeded with a personality probability
    std::string omen_key, omen_verb;
    MaterialId omen_mat = kNoMaterial;
    std::int64_t omen_conf = 0;
    for (auto it = mm->beliefs.lower_bound("omen:");
         it != mm->beliefs.end() && it->first.rfind("omen:", 0) == 0; ++it)
        if (it->second.confidence > omen_conf) {
            omen_conf = it->second.confidence; omen_key = it->first;
        }
    if (!omen_key.empty()) {
        const std::string rest = omen_key.substr(5);
        const std::size_t c = rest.find(':');
        omen_verb = rest.substr(0, c);
        if (c != std::string::npos)
            omen_mat = static_cast<MaterialId>(std::atoll(rest.c_str() + c + 1));
        const std::int64_t heed = std::clamp<std::int64_t>(
            200 + (open - 1000) / 2 + omen_conf / 3 - mm->neuro.panic / 2, 50, 950);
        if (static_cast<std::int64_t>(bhash(w.seed, npc, now ^ 0x0E0Eu) % 1000) >= heed) {
            omen_verb.clear();                         // ignored this time
            AgentMind& m = w.mind(npc);                // an unheeded whisper fades
            auto it = m.beliefs.find(omen_key);
            if (it != m.beliefs.end() && (it->second.confidence -= 100) <= 0) {
                m.beliefs.erase(it);
                omen_key.clear();
            }
            mm = w.find_mind(npc);
        }
        cd = std::max<std::int64_t>(6, cd / 2);        // an omen stirs curiosity
    }
    cool(st, npc, kBabble, now, cd);

    std::vector<std::string> verbs;
    if (!omen_verb.empty()) verbs.push_back(omen_verb);
    // a mind stuck on a shore it cannot swim from (want_cross) plays with
    // whatever FLOATS: try to bind light things together, try to get on them
    const Belief* wc = mm->find_belief("want_cross");
    const bool crossing = wc && wc->confidence >= 300 && (w.ruleset & World::kRulePrimordial);
    if (crossing) {
        if (omen_verb != "board") verbs.push_back("board");
        if (omen_verb != "assemble") verbs.push_back("assemble");
        cd = std::max<std::int64_t>(6, cd / 2);
        cool(st, npc, kBabble, now, cd);
    }
    {
        std::vector<std::string> unk;
        for (const char* v : kBabbleVerbs)
            if (!mm->known_actions.count(v) && omen_verb != v &&
                !(crossing && (std::string(v) == "board" || std::string(v) == "assemble"))) unk.emplace_back(v);
        if (!unk.empty()) {
            const std::size_t off = bhash(w.seed, npc, now) % unk.size();
            for (std::size_t i = 0; i < unk.size(); ++i)
                verbs.push_back(unk[(off + i) % unk.size()]);
        }
    }
    if (verbs.empty()) return false;

    // things remembered within reach (memory, not World Truth)
    const std::int64_t rr = dint(w, *me, dpid(w, "reach"), 64);
    struct T { EntityId id; std::int64_t d; bool hinted; };
    std::vector<T> near;
    // light-as-water sense: material lighter than water (seen it float), or
    // a made thing that already floats
    std::int64_t water_d = 1000000;
    const PropId dens_p = dpid(w, "density"), fc_p = dpid(w, "float_capacity");
    if (const MaterialDef* wmd = w.materials.find_name("water"); wmd && dens_p != kNoProp)
        if (auto it = wmd->props.find(dens_p); it != wmd->props.end() && it->second.i > 0) water_d = it->second.i;
    auto floats = [&](EntityId kid, const AgentMind::MemorySnapshot& sn) {
        if (const Entity* e = w.find(kid); e && fc_p != kNoProp && w.prop_int(*e, fc_p, 0) > 0) return true;
        const MaterialDef* md = w.materials.find(sn.material);
        if (!md || dens_p == kNoProp) return false;
        const auto it = md->props.find(dens_p);
        return it != md->props.end() && it->second.i > 0 && it->second.i < water_d;
    };
    for (const auto& [kid, s] : mm->memory) {
        if (kid == npc) continue;
        // idle curiosity pokes at THINGS, not at living animals: harming a
        // creature only happens through need (hunting via discover_step),
        // threat or rage — never as a pastime
        if (const SpeciesDef* sd = w.species.find_name(s.kind_name); sd && sd->fauna) continue;
        if (w.find_mind(kid)) continue;
        const std::int64_t d = dist2(me->pos, s.pos);
        if (d > rr * rr) continue;
        near.push_back({kid, d, (omen_mat != kNoMaterial && s.material == omen_mat) || (crossing && floats(kid, s))});
    }
    std::sort(near.begin(), near.end(), [](const T& a, const T& b) {
        if (a.hinted != b.hinted) return a.hinted;
        if (a.d != b.d) return a.d < b.d;
        return a.id < b.id;
    });
    if (near.size() > 6) near.resize(6);
    if (near.empty()) return false;

    int budget = 12;                                   // predict() calls
    for (const std::string& v : verbs) {
        std::vector<std::vector<EntityId>> tries;
        const bool pair = v == "assemble" || v == "combine";
        if (pair && crossing && v == "assemble") {
            // a raft wants several light members bound together
            std::vector<EntityId> lot;
            for (const T& t : near) if (t.hinted && lot.size() < 4) lot.push_back(t.id);
            if (lot.size() >= 2) tries.push_back(lot);
        }
        if (pair) {
            for (std::size_t i = 0; i < near.size() && tries.size() < 4; ++i)
                for (std::size_t j = i + 1; j < near.size() && tries.size() < 3; ++j)
                    tries.push_back({near[i].id, near[j].id});
        } else {
            for (std::size_t i = 0; i < near.size() && tries.size() < 3; ++i)
                tries.push_back({near[i].id});
        }
        for (const auto& tg : tries) {
            if (--budget < 0) break;
            if (crossing && v == "disassemble" && tg.size() == 1) {   // never pull apart the hope of crossing
                const auto ms = mm->memory.find(tg.front());
                if (ms != mm->memory.end() && floats(tg.front(), ms->second)) continue;
            }
            InteractionContext c = mk(npc, tg, v.c_str(), 1);
            c.parameters.force = 100000;
            if (!law_allows(w, c)) continue;
            if (w.interact(c).completed) {
                if (!omen_key.empty() && v == omen_verb)
                    w.mind(npc).beliefs.erase(omen_key);   // the omen came true
                if (crossing && v == "board")               // afloat: the wish is answered
                    w.mind(npc).beliefs.erase("want_cross");
                return true;
            }
        }
        if (budget < 0) break;
    }
    if (!omen_verb.empty()) {                          // heeded but nothing gave
        AgentMind& m = w.mind(npc);
        auto it = m.beliefs.find(omen_key);
        if (it != m.beliefs.end() && (it->second.confidence -= 200) <= 0)
            m.beliefs.erase(it);
    }
    return false;
}

// ---- Patron signs (curiosity mode): beacons and building sites -------------
// A beacon (`beacon:<x>:<y>` belief) draws an idle scout toward a place; a
// site (`site:<x>:<y>:<r>` belief) draws a builder there, who then tries to
// `enclose` the loose solids it REMEMBERS around the spot (>= 4 parts; the
// architecture law decides whether a shelter stands). Both are agent
// knowledge only — a sign never moves matter by itself.
bool parse_xy(const std::string& rest, std::int64_t& x, std::int64_t& y, std::int64_t* r) {
    const std::size_t a = rest.find(':');
    if (a == std::string::npos) return false;
    x = std::atoll(rest.c_str());
    y = std::atoll(rest.c_str() + a + 1);
    if (r) {
        const std::size_t b = rest.find(':', a + 1);
        *r = b == std::string::npos ? 96 : std::atoll(rest.c_str() + b + 1);
    }
    return true;
}

bool patron_sign_step(World& w, DriverState* st, EntityId npc, std::uint64_t now) {
    const AgentMind* mm = w.find_mind(npc);
    const Entity* me = w.find(npc);
    if (!mm || !me) return false;
    std::string key;
    for (const char* pre : {"site:", "beacon:"}) {
        const auto it = mm->beliefs.lower_bound(pre);
        if (it != mm->beliefs.end() && it->first.rfind(pre, 0) == 0) { key = it->first; break; }
    }
    if (key.empty()) return false;
    const bool site = key.rfind("site:", 0) == 0;
    std::int64_t x = 0, y = 0, r = 96;
    if (!parse_xy(key.substr(site ? 5 : 7), x, y, site ? &r : nullptr)) {
        w.mind(npc).beliefs.erase(key); return false;
    }
    const Vec2 tgt{static_cast<std::int32_t>(x), static_cast<std::int32_t>(y)};
    const std::int64_t rr = dint(w, *me, dpid(w, "reach"), 64);
    const std::int64_t d2 = dist2(me->pos, tgt);
    if (!site && d2 <= 96 * 96) { w.mind(npc).beliefs.erase(key); return false; }   // arrived
    if (d2 > (r + rr) * (r + rr) || (!site && d2 > 96 * 96)) {
        InteractionContext mc = mk(npc, {}, "move", move_stride(st));
        mc.parameters.destination = tgt; mc.parameters.has_destination = true;
        bool by_water = false;
        if (drive_move(w, st, npc, mc, false, &by_water)) return true;
        if (by_water) return false;          // not unreachable: it needs a craft (want_cross)
        // unreachable (water/cliff): the sign fades
        AgentMind& m = w.mind(npc);
        if (auto it = m.beliefs.find(key); it != m.beliefs.end() && (it->second.confidence -= 100) <= 0)
            m.beliefs.erase(it);
        return false;
    }
    if (!cooled(st, npc, kSite, now)) return false;
    cool(st, npc, kSite, now, 24);
    // at the site: done when a remembered enclosure already stands there
    struct P { EntityId id; std::int64_t d; };
    std::vector<P> parts;
    for (const auto& [kid, s] : mm->memory) {
        if (kid == npc) continue;
        const std::int64_t ds = dist2(s.pos, tgt);
        if (s.kind_name == "enclosure" && ds <= r * r) { w.mind(npc).beliefs.erase(key); return false; }
        if (ds > (r + rr) * (r + rr) || dist2(me->pos, s.pos) > rr * rr * 4) continue;
        if (w.species.find_name(s.kind_name) || w.find_mind(kid)) continue;   // living things
        if (s.kind_name == "water") continue;
        parts.push_back({kid, ds});
    }
    std::sort(parts.begin(), parts.end(), [](const P& a, const P& b) {
        return a.d != b.d ? a.d < b.d : a.id < b.id; });
    if (parts.size() > 8) parts.resize(8);
    AgentMind& m = w.mind(npc);
    if (parts.size() >= 4) {
        std::vector<EntityId> ids;
        for (const P& p : parts) ids.push_back(p.id);
        InteractionContext c = mk(npc, ids, "enclose", 2);
        c.parameters.force = 100000;
        if (w.interact(c).completed) { w.mind(npc).beliefs.erase(key); return true; }
    }
    if (auto it = m.beliefs.find(key); it != m.beliefs.end() && (it->second.confidence -= 60) <= 0)
        m.beliefs.erase(it);
    return false;
}


// ---- maintenance -------------------------------------------------------------

// Best wieldable tool in reach: highest edge, then thrust_range, then lowest
// id — a total deterministic order over World Truth props only.
EntityId best_tool(const World& w, EntityId npc) {
    const Entity* me = w.find(npc);
    if (!me) return kNoEntity;
    const PropId reach = dpid(w, "reach");
    const PropId edge = dpid(w, "edge");
    const PropId thrust = dpid(w, "thrust_range");
    const PropId contained = dpid(w, "contained");
    const std::int64_t rr = dint(w, *me, reach, 64);
    const std::int64_t r2 = rr * rr;
    struct Cand { EntityId id; std::int64_t e, t; };
    std::vector<Cand> cands;
    for (EntityId tid : w.entities_near(me->pos, rr)) {
        if (tid == npc) continue;
        const Entity* t = w.find(tid);
        if (!t || !t->alive) continue;
        if (contained != kNoProp &&
            w.prop_int(*t, contained, 0) != 0) continue;
        const std::int64_t te = dint(w, *t, edge, 0);
        const std::int64_t tt = dint(w, *t, thrust, 0);
        if (te <= 0 && tt <= 0) continue;         // not a tool
        if (dist2(me->pos, t->pos) > r2) continue;
        cands.push_back({tid, te, tt});
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        if (a.e != b.e) return a.e > b.e;
        if (a.t != b.t) return a.t > b.t;
        return a.id < b.id;
    });
    return cands.empty() ? kNoEntity : cands.front().id;
}

bool already_wielding(const World& w, EntityId npc) {
    for (const Relation& rel : w.relations_of(npc, "wielding"))
        if (rel.a == npc) {
            const Entity* t = w.find(rel.b);
            if (t && t->alive) return true;
        }
    return false;
}

// Hungry member of a non-empty rationed collective within reach.
EntityId ration_store(const World& w, EntityId npc) {
    const PropId hunger = dpid(w, "hunger");
    const Entity* me = w.find(npc);
    if (!me || hunger == kNoProp || w.prop_int(*me, hunger, 0) <= 700)
        return kNoEntity;
    const PropId qty = dpid(w, "quantity");
    const PropId reach = dpid(w, "reach");
    const std::int64_t rr = dint(w, *me, reach, 64);
    const std::int64_t r2 = rr * rr;
    for (const Relation& rel : w.relations_of(npc, "member_of")) {
        if (rel.a != npc) continue;
        const Entity* g = w.find(rel.b);
        if (!g || !g->alive) continue;
        if (qty == kNoProp || w.prop_int(*g, qty, 0) <= 0) continue;
        if (dist2(me->pos, g->pos) > r2) continue;
        return g->id;
    }
    return kNoEntity;
}

// ---- region materialization (fog-of-war, Phase 9) ------------------------------
// Terrain fields are analytic everywhere, but ENTITY stages (deposits,
// species) only exist where generate_region() has run. When an agent
// approaches the edge of the materialized zone we materialize a rect of
// global cells around it — World bookkeeping only (gen_cells_), never
// agent knowledge. Idempotent: already-materialized cells are skipped.
void ensure_region(World& w, EntityId npc) {
    if (w.terrain.empty() || w.terrain.cell <= 0) return;
    const Entity* me = w.find(npc);
    if (!me || !me->alive) return;
    const int cs = w.terrain.cell;
    const int cgx = TerrainGrid::cell_of(me->pos.x, cs);
    const int cgy = TerrainGrid::cell_of(me->pos.y, cs);
    const PropId sight = dpid(w, "sight_range");
    const std::int64_t sr = sight != kNoProp ? w.prop_int(*me, sight, 256) : 256;
    const int rc = static_cast<int>(sr / cs) + 2;    // sight + walking margin
    int x0 = cgx - rc, y0 = cgy - rc;
    int x1 = cgx + rc, y1 = cgy + rc;
    x0 = std::max(x0, w.terrain.ox); y0 = std::max(y0, w.terrain.oy);
    x1 = std::min(x1, w.terrain.ox + w.terrain.w - 1);
    y1 = std::min(y1, w.terrain.oy + w.terrain.h - 1);
    if (x0 > x1 || y0 > y1) return;                  // outside dense grid — nothing to do
    bool done = true;
    for (int gy = y0; gy <= y1 && done; ++gy)
        for (int gx = x0; gx <= x1; ++gx)
            if (!w.gen_cells_.count({gx, gy})) { done = false; break; }
    if (done) return;
    WorldGenParams p;
    p.seed = w.world_seed; p.sea_level = w.gen_sea_level;
    p.cell_size = cs; p.grid_w = w.terrain.w; p.grid_h = w.terrain.h;
    p.origin_x = w.terrain.ox; p.origin_y = w.terrain.oy;
    p.defer_entities = false;
    // per-region ecology cap would make world content depend on WHICH
    // region materialized first — region-order independence requires none.
    p.max_trees = std::numeric_limits<int>::max();
    std::string err;
    WorldGenerator gen;
    gen.generate_region(w, p, x0, y0, x1 - x0 + 1, y1 - y0 + 1, &err);
}

// ---- survival locomotion -------------------------------------------------------
// Edible identity of a remembered snapshot: a fruiting plant feeds through
// its `fruit_material` (and only while `fruit_count` > 0 — a bare branch is
// not food); everything else feeds through its own material. This mirrors
// predict_consume's two-layer rule so locomotion and the law agree.
struct FoodId { MaterialId mat; bool fruiting; bool feeds; };
FoodId snapshot_food(const World& w, EntityId eid,
                     const AgentMind::MemorySnapshot& s,
                     std::int64_t blur, std::uint64_t seed) {
    FoodId r{kNoMaterial, false, false};
    MaterialId mat = s.material;
    bool fruiting = false;
    const PropId fmp = dpid(w, "fruit_material");
    if (fmp != kNoProp) {
        const auto it = s.props.find(fmp);
        if (it != s.props.end() && it->second.type == ValType::Text &&
            !it->second.s.empty()) {
            const MaterialId fm = w.materials.id_of(it->second.s);
            if (fm != kNoMaterial) { mat = fm; fruiting = true; }
        }
    }
    if (fruiting) {
        const PropId fcp = dpid(w, "fruit_count");
        if (fcp != kNoProp) {
            const auto ci = s.props.find(fcp);
            if (ci != s.props.end()) {
                // semantic drift: a fuzzy memory may recall the wrong count
                const Value cv = recalled_prop(s, fcp, eid, seed, blur, ci->second);
                if (cv.type == ValType::Integer && cv.i <= 0)
                    return r;                   // bare branch — nothing to eat
            }
        }
    }
    const MaterialDef* md = w.materials.find(mat);
    if (!md) return r;
    const PropId nut = dpid(w, "nutrition");
    const PropId hyd = dpid(w, "hydration");
    r.feeds =
        (nut != kNoProp && md->props.count(nut) && md->props.at(nut).i > 0) ||
        (hyd != kNoProp && md->props.count(hyd) && md->props.at(hyd).i > 0);
    r.mat = mat; r.fruiting = fruiting;
    return r;
}
// Urgent need + KNOWN food beyond reach -> walk to it. "Known" means the
// agent's MEMORY snapshot — the last thing it perceived, possibly stale.
// No World-Truth lookup of remembered entities: whether the berry bush is
// still there is discovered by walking back and perceiving it again.
// Nothing remembered edible -> frontier-explore away from visited_cells.
Vec2 forage_dest(const World& w, EntityId npc) {
    const Entity* me = w.find(npc);
    if (!me) return {0, 0};
    const PropId hunger = dpid(w, "hunger");
    const PropId thirst = dpid(w, "thirst");
    const bool urgent =
        (hunger != kNoProp && w.prop_int(*me, hunger, 0) > 500) ||
        (thirst != kNoProp && w.prop_int(*me, thirst, 0) > 500);
    if (!urgent) return {0, 0};
    const PropId reach = dpid(w, "reach");
    const std::int64_t rr = dint(w, *me, reach, 64);
    const std::int64_t r2 = rr * rr;
    const AgentMind* mm = w.find_mind(npc);
    if (!mm) return {0, 0};
    // semantic drift: below the blur threshold recalled positions jitter
    // deterministically — the agent walks toward where it THINKS the bush is.
    const MindParams mp = mind_params(w, *me);
    const std::int64_t blur = mp.detail_blur_threshold;
    // remembered edibles split by reach: something edible already inside
    // reach means cognition should ACT (consume), not walk — returning a
    // destination here would preempt discover_step forever and the agent
    // would starve standing next to food. Only walk when every remembered
    // meal is beyond reach.
    bool food_in_reach = false;
    EntityId food = kNoEntity;
    std::int64_t bd = std::numeric_limits<std::int64_t>::max();
    for (const auto& [kid, s] : mm->memory) {
        if (kid == npc) continue;
        // aversion: deeply negative engrams are avoided, not sought
        if (s.engram.emotional_charge <= -mp.paranoia_charge) continue;
        if (!snapshot_food(w, kid, s, blur, w.seed).feeds) continue;
        const Vec2 rp = recalled_pos(s, kid, w.seed, blur);
        const std::int64_t d = dist2(me->pos, rp);
        if (d <= r2) { food_in_reach = true; break; }
        if (d < bd) { bd = d; food = kid; }
    }
    if (food_in_reach) return {0, 0};
    if (food != kNoEntity)
        return recalled_pos(mm->memory.at(food), food, w.seed, blur);
    // frontier: farthest remembered position pushes the edge of knowledge —
    // memory.pos is stale belief, exactly what exploration should chase.
    Vec2 far{0, 0};
    bd = -1;
    for (const auto& [kid, s] : mm->memory) {
        if (kid == npc) continue;
        const Vec2 rp = recalled_pos(s, kid, w.seed, blur);
        const std::int64_t d = dist2(me->pos, rp);
        if (d > bd) { bd = d; far = rp; }
    }
    if (bd > 0) return far;
    // nothing remembered at all: drift along +x through unvisited ground —
    // deterministic compass fallback until perception fills memory.
    return {me->pos.x + 256, me->pos.y};
}

// ---- social reflexes -----------------------------------------------------------

EntityId nearest_agent(const World& w, EntityId npc, std::int64_t range2) {
    const Entity* me = w.find(npc);
    if (!me) return kNoEntity;
    EntityId best = kNoEntity;
    std::int64_t bd = std::numeric_limits<std::int64_t>::max();
    // spatial candidates (sorted ids -> same lowest-id tie-break as before)
    std::int64_t radius = 0;
    while (radius * radius < range2) ++radius;
    for (EntityId oid : w.entities_near(me->pos, radius)) {
        if (oid == npc || !w.find_mind(oid)) continue;
        const Entity* o = w.find(oid);
        if (!o || !o->alive || o->kind_name != me->kind_name) continue;
        const std::int64_t d = dist2(me->pos, o->pos);
        if (d <= range2 && d < bd) { bd = d; best = oid; }
    }
    return best;
}

}  // namespace

const std::vector<const char*>& curiosity_verbs() {
    static const std::vector<const char*> v(std::begin(kBabbleVerbs), std::end(kBabbleVerbs));
    return v;
}

namespace {


// activity_phase gate shared with World::range_drift: 0 any, 1 diurnal,
// 2 nocturnal, 3 crepuscular — the tick IS the hour (1 tick = 1 hour).
bool phase_active(std::int64_t phase, std::int64_t hour) {
    switch (phase) {
        case 1: return hour >= 6 && hour < 18;
        case 2: return hour >= 18 || hour < 6;
        case 3: return (hour >= 5 && hour < 8) ||
                       (hour >= 17 && hour < 20);
        default: return true;
    }
}

// species with instinct verbs (data: `innate`) = cultural people
bool cultural_kind(const World& w, const Entity& e) {
    const SpeciesDef* sp = w.species.find_name(e.kind_name.str());
    return sp && !sp->innate.empty();
}

// One agent's share of an autonomous round — the old autonomous_cycle
// agent-loop body, verbatim. A do/while(false) wrapper keeps the original
// `continue` statements working: at this level they mean "next agent",
// which here is just "return". (`break`/`continue` inside nested loops
// still bind to those loops.)
void cycle_one_agent(World& w, DriverState* st, DriverTally* tally,
                     EntityId npc, std::size_t my_idx, std::size_t rr_stride,
                     int& discover_budget, std::uint64_t now) {
        Entity* me = w.find(npc);
        if (!me || !me->alive) return;
        do {

        // 1. perception — game mode: beasts look around every 4th round
        // (staggered by id); people every round
        const bool look = !(st && st->curiosity) || cultural_kind(w, *me) ||
                          (my_idx + static_cast<std::size_t>(st->cycles)) % 4 == 0;
        const std::size_t nobs = look ? perceive(w, npc) : 0;
        if (tally) tally->perceived += static_cast<std::int64_t>(nobs);

        // 1b. lazy worldgen: materialize entity stages around the agent when
        // it nears the edge of the generated zone (World bookkeeping only).
        ensure_region(w, npc);
        if (st && st->possessed.count(npc)) continue;   // player drives it

        const PropId hunger = dpid(w, "hunger");
        const PropId thirst = dpid(w, "thirst");
        const PropId age = dpid(w, "age");
        const PropId adult = dpid(w, "adult_age");
        const PropId fertility = dpid(w, "fertility");
        const PropId energy = dpid(w, "energy");
        const PropId crange = dpid(w, "communicate_range");
        const PropId reach = dpid(w, "reach");
        const Entity* me_c = w.find(npc);
        if (!me_c) continue;

        // CL-1 free will (Phase 4c): in 100% neural mode the survival/
        // social reflex stack below is OFF — affect reaches behaviour
        // through rollout scoring inside brain_decide instead of hardcoded
        // hijacks, so depression/anorexia/withdrawal can emerge. The
        // legacy_planner_compat path keeps the full reflex suite.
        const bool neural_free =
            st && st->brain_control && !st->legacy_planner_compat;

        if (!neural_free) {
        // 2a. maintenance: wield best tool when hands are empty
        // (primordial: a reflex needs the verb — nobody "knows" tools yet)
        const bool wield_known = !(st && st->curiosity) ||
            (w.find_mind(npc) && w.find_mind(npc)->known_actions.count("wield"));
        if (wield_known && !already_wielding(w, npc) && cooled(st, npc, kWield, now)) {
            const EntityId tool = best_tool(w, npc);
            if (tool != kNoEntity) {
                cool(st, npc, kWield, now, kCooldown[kWield]);
                if (w.interact(mk(npc, {tool}, "wield")).completed) {
                    if (tally) ++tally->maintenance;
                    continue;                    // one action per cycle
                }
            }
        }
        // 2b. maintenance: withdraw from ration store when starving
        if (cooled(st, npc, kWithdraw, now)) {
            const EntityId store = ration_store(w, npc);
            if (store != kNoEntity) {
                cool(st, npc, kWithdraw, now, kCooldown[kWithdraw]);
                if (w.interact(mk(npc, {store}, "withdraw")).completed) {
                    if (tally) ++tally->maintenance;
                    continue;
                }
            }
        }

        // 3. survival first: an urgent need with known food beyond reach
        // preempts cognition — a starving animal does not philosophize.
        // (Foraging targets come from the agent's OWN known_entities —
        // personal knowledge, not world truth leakage.)
        {
            const Vec2 fd = forage_dest(w, npc);
            if (fd.x != 0 || fd.y != 0) {
                cool(st, npc, kWield, now, 8);
                InteractionContext fc = mk(npc, {}, "move", move_stride(st));
                fc.parameters.destination = fd;
                fc.parameters.has_destination = true;
                if (drive_move(w, st, npc, fc)) {
                    if (tally) ++tally->moves;
                    continue;
                }
            }
        }

        // 3a. hunt reflex — a carnivore (prey_tier_max > 0, pure species
        // data) tracks remembered game by scent and sight. Live prey is
        // approached, then struck: the consume law refuses awake animals,
        // so the kill is physical damage, not a menu choice. Anything
        // edible-tier already in reach goes down directly — a carcass or
        // an unconscious animal needs no second blow. fire_fear suppresses
        // the hunt while panic runs hot; activity_phase gates it to the
        // animal's declared hours. Every act still flows through interact.
        if (cooled(st, npc, kConsume, now)) {
            // hunting is driven by hunger like any other meal — a sated
            // predator lets game live (trophic cascades need that slack).
            const bool hungry =
                dint(w, *me, hunger, 0) > 400 || dint(w, *me, thirst, 0) > 400;
            const std::int64_t ptm = hungry
                ? dint(w, *me, dpid(w, "prey_tier_max"), 0) : 0;
            if (ptm > 0) {
                const AgentMind* mm = w.find_mind(npc);
                const std::int64_t phase =
                    dint(w, *me, dpid(w, "activity_phase"), 0);
                const std::int64_t ffear =
                    dint(w, *me, dpid(w, "fire_fear"), 0);
                const bool fearful = ffear > 0 && mm &&
                    mm->neuro.panic * 2 > ffear;
                if (mm && !fearful &&
                    phase_active(phase, static_cast<std::int64_t>(now % 24))) {
                    const std::int64_t mytier =
                        dint(w, *me, dpid(w, "trophic_tier"), 0);
                    const PropId tier_pid = dpid(w, "trophic_tier");
                    const std::int64_t rr = dint(w, *me_c, reach, 64);
                    const std::int64_t sense = std::max<std::int64_t>(
                        dint(w, *me, dpid(w, "hunt_range"), 0),
                        dint(w, *me, dpid(w, "scent_radius"), 0));
                    const std::int64_t r2 = rr * rr;
                    const std::int64_t s2 = sense * sense;
                    const MindParams mp = mind_params(w, *me);
                    const std::int64_t blur = mp.detail_blur_threshold;
                    struct Prey { EntityId id; std::int64_t d; bool near; };
                    std::vector<Prey> prey;
                    for (const auto& [kid, s] : mm->memory) {
                        if (kid == npc) continue;
                        if (s.kind_name == me->kind_name.str()) continue;
                        // the prey's tier is an observable prop, so it
                        // survives in the mind's memory snapshot.
                        std::int64_t t = 0;
                        if (tier_pid != kNoProp) {
                            const auto it = s.props.find(tier_pid);
                            if (it != s.props.end() &&
                                it->second.type == ValType::Integer)
                                t = it->second.i;
                        }
                        if (t < 1 || t > ptm || t >= mytier) continue;
                        const Vec2 rp = recalled_pos(s, kid, w.seed, blur);
                        const std::int64_t d = dist2(me->pos, rp);
                        if (d > s2) continue;
                        prey.push_back({kid, d, d <= r2});
                    }
                    std::sort(prey.begin(), prey.end(),
                              [](const Prey& a, const Prey& b) {
                                  if (a.near != b.near) return a.near;
                                  if (a.d != b.d) return a.d < b.d;
                                  return a.id < b.id;
                              });
                    if (!prey.empty()) {
                        const Prey& p0 = prey.front();
                        if (p0.near) {
                            // hunt cadence is shorter than meal pacing: a
                            // predator works a wounded animal down over
                            // several bites; only a completed meal re-arms
                            // the full consume cooldown.
                            cool(st, npc, kConsume, now, 48);
                            if (w.interact(mk(npc, {p0.id}, "consume", 2))
                                    .completed) {
                                cool(st, npc, kConsume, now,
                                     kCooldown[kConsume]);
                                if (tally) ++tally->maintenance;
                                continue;
                            }
                            // still awake and resisting: put it down —
                            // "impact" (not the mining "strike") so the blow
                            // routes through anatomy: claws bleed, fangs
                            // carry natural_toxin into the wound.
                            InteractionContext kc =
                                mk(npc, {p0.id}, "impact");
                            // Phase 19: a proven trajectory law replaces
                            // the species' instinctive effort guess with
                            // the measured hardness permille.
                            const std::int64_t qf =
                                (mm ? qlaw_param(*mm, "trajectory") : -1);
                            kc.parameters.force = qf > 0
                                ? qf * dint(w, *me, dpid(w, "natural_force"), 100) / 100
                                : dint(w, *me, dpid(w, "natural_force"), 40);
                            if (w.interact(kc).completed) {
                                if (tally) ++tally->maintenance;
                                continue;
                            }
                        } else {
                            // scent contact: close on the remembered game
                            cool(st, npc, kWield, now, 8);
                            InteractionContext hc =
                                mk(npc, {}, "move", move_stride(st));
                            hc.parameters.destination = recalled_pos(
                                mm->memory.at(p0.id), p0.id, w.seed, blur);
                            hc.parameters.has_destination = true;
                            if (drive_move(w, st, npc, hc)) {
                                if (tally) ++tally->moves;
                                continue;
                            }
                        }
                    }
                }
            }
        }

        // 3b. consume reflex — routine survival, not research. A food whose
        // material already carries a Supported/Verified consume record is
        // eaten directly; UNKNOWN materials still owe a discover_step
        // taste-test, and Refuted (taboo) materials are skipped — unless
        // desperation (>900) overrides risk assessment entirely and any
        // remembered edible goes down (50/50: poison vs starvation).
        if (cooled(st, npc, kConsume, now)) {
            const AgentMind* mm = w.find_mind(npc);
            const bool urgent =
                dint(w, *me, hunger, 0) > 500 || dint(w, *me, thirst, 0) > 500;
            const bool desperate =
                dint(w, *me, hunger, 0) > 900 || dint(w, *me, thirst, 0) > 900;
            // Secretive hoarding: a stressed, hungry mind does not dine in
            // front of a crowd — it first puts distance between itself and
            // nearby conspecifics, then eats in peace. Deterministic:
            // sorted ids, fixed-axis away vector, consume cooldown shared
            // with a meal so it cannot jitter every cycle.
            if (mm && urgent && !desperate &&
                dint(w, *me, hunger, 0) > 500 && mm->neuro.panic >= 300) {
                std::vector<EntityId> near = w.entities_near(me->pos, 96);
                std::sort(near.begin(), near.end());
                std::int64_t cx = 0, cy = 0;
                int cn = 0;
                for (EntityId cid : near) {
                    if (cid == npc) continue;
                    const Entity* ce = w.find(cid);
                    if (!ce || !ce->alive || ce->kind_name != me->kind_name)
                        continue;
                    cx += ce->pos.x; cy += ce->pos.y; ++cn;
                }
                if (cn >= 2) {
                    cool(st, npc, kConsume, now, kCooldown[kConsume]);
                    const std::int64_t ax = me->pos.x * cn - cx;
                    const std::int64_t ay = me->pos.y * cn - cy;
                    const bool xdom = std::llabs(ax) >= std::llabs(ay);
                    InteractionContext rc;
                    rc.actor = npc; rc.parameters.ticks = 1;
                    rc.parameters.action = "move";
                    rc.parameters.destination = xdom
                        ? Vec2{me->pos.x +
                              static_cast<std::int32_t>(ax > 0 ? 96 : -96),
                              me->pos.y}
                        : Vec2{me->pos.x, me->pos.y +
                              static_cast<std::int32_t>(ay > 0 ? 96 : -96)};
                    rc.parameters.has_destination = true;
                    if (drive_move(w, st, npc, rc)) {
                        if (tally) ++tally->maintenance;
                        continue;
                    }
                }
            }
            if (mm && urgent && mm->known_actions.count("consume")) {
                const std::int64_t rr = dint(w, *me_c, reach, 64);
                const std::int64_t r2 = rr * rr;
                const MindParams mp = mind_params(w, *me);
                const std::int64_t blur = mp.detail_blur_threshold;
                struct Bite { EntityId id; MaterialId mat; bool fruiting;
                              std::int64_t d; };
                std::vector<Bite> bites;
                for (const auto& [kid, s] : mm->memory) {
                    if (kid == npc) continue;
                    if (s.engram.emotional_charge <= -mp.paranoia_charge) continue;
                    const FoodId f = snapshot_food(w, kid, s, blur, w.seed);
                    if (!f.feeds) continue;
                    const Vec2 rp = recalled_pos(s, kid, w.seed, blur);
                    const std::int64_t d = dist2(me->pos, rp);
                    if (d > r2) continue;                    // not in reach
                    bites.push_back({kid, f.mat, f.fruiting, d});
                }
                std::sort(bites.begin(), bites.end(),
                          [](const Bite& a, const Bite& b) {
                              if (a.d != b.d) return a.d < b.d;
                              return a.id < b.id;
                          });
                std::vector<EntityId> meal;
                for (const Bite& b : bites) {
                    if (meal.size() >= 4) break;
                    if (!desperate) {
                        const DiscoveryStatus stt = food_method_status(
                            w, npc, "consume", b.mat, b.fruiting);
                        if (stt != DiscoveryStatus::Supported &&
                            stt != DiscoveryStatus::Verified) continue;
                    }
                    meal.push_back(b.id);
                }
                if (!meal.empty()) {
                    cool(st, npc, kConsume, now, kCooldown[kConsume]);
                    if (w.interact(mk(npc, meal, "consume", 2)).completed) {
                        if (tally) ++tally->maintenance;
                        continue;
                    }
                }
            }
        }

        // 3c. executive override (Nhịp 3d): panic/rage/grief above the
        // mind's own threshold hijacks deliberation — plan_multi is skipped
        // entirely and the strongest reflex wins. Rage strikes the most
        // hated remembered thing, panic flees the worst trauma, grief
        // curls up and rests. Thresholds and gains are data/genome, the
        // choice is arithmetic — no disease flag exists.
        {
            neuro_touch(w, npc);
            const AgentMind* om = w.find_mind(npc);
            if (om) {
                const NeuroState& ons = om->neuro;
                const std::int64_t ovr = ons.executive_override;
                const std::int64_t top = std::max({ons.panic, ons.rage,
                                                   ons.grief});
                if (top > ovr) {
                    InteractionContext rc;
                    rc.actor = npc;
                    rc.parameters.ticks = 1;
                    if (ons.rage >= ons.panic && ons.rage >= ons.grief) {
                        // strike the most negative-charge memory in reach
                        EntityId foe = kNoEntity;
                        std::int64_t worst = 0;
                        const std::int64_t rr = dint(w, *me_c, reach, 64);
                        const std::int64_t r2 = rr * rr * 4;
                        for (const auto& [kid, s] : om->memory) {
                            if (kid == npc || s.engram.emotional_charge >= 0)
                                continue;
                            if (dist2(me->pos, s.pos) > r2) continue;
                            if (s.engram.emotional_charge < worst ||
                                foe == kNoEntity) {
                                worst = s.engram.emotional_charge;
                                foe = kid;
                            }
                        }
                        if (foe != kNoEntity) {
                            rc.parameters.action = "strike";
                            rc.targets = {foe};
                        } else {
                            rc.parameters.action = "rest";
                        }
                    } else if (ons.panic >= ons.grief) {
                        // flee directly away from the worst remembered trauma
                        Vec2 away{96, 0};
                        std::int64_t worst = 0;
                        for (const auto& [kid, s] : om->memory) {
                            if (s.engram.emotional_charge >= 0) continue;
                            if (s.engram.emotional_charge < worst ||
                                worst == 0) {
                                worst = s.engram.emotional_charge;
                                const std::int64_t dx = me->pos.x - s.pos.x;
                                const std::int64_t dy = me->pos.y - s.pos.y;
                                if (dx * dx + dy * dy > 0) {
                                    // fixed-point flee: swap-dominant axis
                                    away = (std::llabs(dx) >= std::llabs(dy))
                                        ? Vec2{static_cast<std::int32_t>(
                                                  dx > 0 ? 96 : -96), 0}
                                        : Vec2{0, static_cast<std::int32_t>(
                                                  dy > 0 ? 96 : -96)};
                                }
                            }
                        }
                        rc.parameters.action = "move";
                        rc.parameters.ticks = move_stride(st);   // flee stride matches locomotion
                        rc.parameters.destination = {me->pos.x + away.x,
                                                     me->pos.y + away.y};
                        rc.parameters.has_destination = true;
                    } else {
                        rc.parameters.action = "rest";   // grief: curl up
                    }
                    if (rc.parameters.action == "move") drive_move(w, st, npc, rc, /*panic=*/true);
                    else w.interact(rc);
                    continue;                    // reflex consumed the cycle
                }
            }
        }
        }   // !neural_free — reflex stack skipped in 100% neural mode

        // 4. cognition & discovery — plan/goal caching (Phase 9):
        //  * a cached ExploreGoal waypoint keeps the agent walking WITHOUT
        //    re-running detect/generate hypotheses (the expensive path);
        //  * the waypoint is cancelled if its subject entity lost integrity
        //    (a Threat appeared — re-plan immediately);
        //  * at most kDiscoverPerCycle agents per tick may run discover_step.
        ExploreGoal goal;
        bool acted = false;
        bool walking_pending = false;
        if (st) {
            auto pit = st->pending.find(npc);
            if (pit != st->pending.end()) {
                PendingExplore& pe = pit->second;
                // survival overrides exploration: an urgent need cancels the
                // cached waypoint so forage/cognition get the cycle back.
                bool cancel = now >= pe.until ||
                              dist2(me->pos, pe.target) <= 32 * 32 ||
                              dint(w, *me, hunger, 0) > 600 ||
                              dint(w, *me, dpid(w, "thirst"), 0) > 600;
                if (!cancel && pe.subject != kNoEntity) {
                    if (const Entity* se = w.find(pe.subject)) {
                        const PropId integ = dpid(w, "integrity");
                        if (integ != kNoProp) {
                            const std::int64_t cur = w.prop_int(*se, integ, -1);
                            if (cur >= 0 && pe.last_integrity >= 0 &&
                                cur < pe.last_integrity) cancel = true;  // Threat!
                            else pe.last_integrity = cur;
                        }
                    } else cancel = true;    // subject vanished — stale memory
                }
                if (cancel) st->pending.erase(pit);
                else {
                    goal.target = pe.target; goal.subject = pe.subject;
                    walking_pending = true;
                }
            }
        }
        // Search backoff: an agent whose discover_step found nothing to run
        // rests its planner 8 -> 16 -> 32 ticks (reset on success). Urgent
        // hunger/thirst bypasses the gate so survival reasoning never sleeps.
        bool gated = false;
        if (st && !walking_pending) {
            const bool urgent = dint(w, *me, hunger, 0) > 600 ||
                                dint(w, *me, dpid(w, "thirst"), 0) > 600;
            const auto gi = st->next_discover.find(npc);
            gated = !urgent && gi != st->next_discover.end() && now < gi->second;
        }
        // a whispered omen stirs curiosity BEFORE routine deliberation
        // (the mind may still ignore it — babble_step rolls personality)
        if (st && st->curiosity && !walking_pending && cooled(st, npc, kBabble, now)) {
            const AgentMind* om = w.find_mind(npc);
            const auto ob = om ? om->beliefs.lower_bound("omen:") : decltype(om->beliefs.end()){};
            if (om && ob != om->beliefs.end() && ob->first.rfind("omen:", 0) == 0 &&
                babble_step(w, st, npc, now)) {
                if (tally) ++tally->experiments;
                continue;
            }
        }
        // afloat with a far shore in mind: paddle (game mode)
        if (sail_step(w, st, npc)) { if (tally) ++tally->moves; continue; }
        // patron signs (beacon / building site) — only when not in need
        if (st && st->curiosity && !walking_pending &&
            dint(w, *me, hunger, 0) <= 600 && dint(w, *me, dpid(w, "thirst"), 0) <= 600 &&
            patron_sign_step(w, st, npc, now)) {
            if (tally) ++tally->moves;
            continue;
        }
        // Phase 19: proven quantitative laws tune action parameters —
        // load the craft to its measured safe limit, strike at the
        // measured hardness. Belief-side only; laws stay World Truth.
        if (quantum_step(w, st, npc, now)) {
            if (tally) ++tally->moves;
            continue;
        }
        // CL-1 Phase 2: the brain is the primary action selector. It runs
        // every cycle (its own C=5 imagination budget is internal); the
        // legacy discover/plan_multi path only fires as a bootstrap
        // fallback while the brain has no usable policy yet.
        if (st && st->brain_control && !walking_pending &&
            (neural_free || !gated) &&
            brain_step(w, st, npc, now, tally)) {
            if (tally) ++tally->experiments;
            continue;
        }
        bool may_discover = discover_budget > 0 && !neural_free;
        if (st && st->curiosity) {
            const bool urgent = dint(w, *me, hunger, 0) > 600 ||
                                dint(w, *me, dpid(w, "thirst"), 0) > 600;
            may_discover = urgent ||
                (my_idx + static_cast<std::size_t>(st->cycles)) % rr_stride == 0;
        }
        if (!walking_pending && !gated && may_discover) {
            --discover_budget;
            acted = discover_step(w, npc, &goal);
            if (st) {
                if (acted) {
                    st->discover_fails.erase(npc);
                    st->next_discover.erase(npc);
                } else {
                    const int f = std::min(++st->discover_fails[npc], 3);
                    st->next_discover[npc] = now + (static_cast<std::uint64_t>(4) << f);
                }
            }
        }
        if (acted) {
            if (tally) ++tally->experiments;
            continue;                            // time was consumed inside
        }
        // 4b. primordial curiosity — poke the world with an untried verb
        if (st && st->curiosity && !walking_pending &&
            goal.target.x == 0 && goal.target.y == 0 &&
            cooled(st, npc, kBabble, now) && babble_step(w, st, npc, now)) {
            if (tally) ++tally->experiments;
            continue;
        }

        // 5. locomotion toward the survey waypoint Discovery reported
        // (fresh or cached pending — same movement either way)
        if (goal.target.x != 0 || goal.target.y != 0) {
            if (st && !walking_pending) {
                PendingExplore pe;
                pe.target = goal.target; pe.subject = goal.subject;
                pe.until = now + kPendingTicks;
                if (goal.subject != kNoEntity)
                    if (const Entity* se = w.find(goal.subject)) {
                        const PropId integ = dpid(w, "integrity");
                        pe.last_integrity = integ != kNoProp
                            ? w.prop_int(*se, integ, -1) : -1;
                    }
                st->pending[npc] = pe;
            }
            cool(st, npc, kWield, now, 8);        // don't re-tool mid-stride
            InteractionContext mc = mk(npc, {}, "move", move_stride(st));
            mc.parameters.destination = goal.target;
            mc.parameters.has_destination = true;
            if (drive_move(w, st, npc, mc)) {
                if (tally) ++tally->moves;
                continue;
            }
        }

        // 5. social reflexes — only when cognition found nothing to do.
        const std::int64_t cr2 = dint(w, *me_c, crange, 0);
        const std::int64_t rr2 = dint(w, *me_c, reach, 64);
        const std::int64_t rr2sq = rr2 * rr2;

        // reproduce: compatible partner close by, both fit the biology gates
        // (hunger <= 500 and energy >= 40000 per parent — mirrors the law so
        // we don't spend an interact on a guaranteed refusal)
        if (cooled(st, npc, kReproduce, now) && repro_ready(w, *me_c, now) &&
            !me_c->genome.loci.empty() &&
            dint(w, *me_c, age, 0) >= dint(w, *me_c, adult, 100) &&
            dint(w, *me_c, fertility, 0) > 0 &&
            (hunger == kNoProp || dint(w, *me_c, hunger, 0) <= 500) &&
            dint(w, *me_c, energy, 40000) >= 40000) {
            const EntityId partner = nearest_agent(w, npc, rr2sq);
            if (partner != kNoEntity) {
                const Entity* p = w.find(partner);
                if (p && dint(w, *p, age, 0) >= dint(w, *p, adult, 100) &&
                    dint(w, *p, fertility, 0) > 0 &&
                    (hunger == kNoProp || dint(w, *p, hunger, 0) <= 500) &&
                    dint(w, *p, energy, 0) >= 40000 &&
                    !p->genome.loci.empty()) {
                    cool(st, npc, kReproduce, now, kCooldown[kReproduce]);
                    if (w.interact(mk(npc, {partner}, "reproduce")).completed) {
                        if (tally) ++tally->social;
                        continue;
                    }
                }
            }
        }

        // communicate: same-kind agent in range whose action set differs
        if (cr2 > 0 && cooled(st, npc, kCommunicate, now)) {
            const EntityId partner = nearest_agent(w, npc, cr2 * cr2);
            if (partner != kNoEntity) {
                const AgentMind* om = w.find_mind(partner);
                const AgentMind* mm = w.find_mind(npc);
                if (om && mm && om->known_actions != mm->known_actions) {
                    cool(st, npc, kCommunicate, now, kCooldown[kCommunicate]);
                    if (w.interact(mk(npc, {partner}, "communicate")).completed) {
                        if (tally) ++tally->social;
                        continue;
                    }
                }
            }
        }

        // engrave: a Verified record + a durable nearby surface to carve on
        if (cooled(st, npc, kEngrave, now)) {
            const AgentMind* mm = w.find_mind(npc);
            std::string best_key;
            std::int64_t best_ev = -1;
            if (mm) for (const auto& [key, rec] : mm->discoveries)
                if (rec.status == DiscoveryStatus::Verified &&
                    rec.evidence_for > best_ev) {
                    best_ev = rec.evidence_for; best_key = key;
                }
            if (!best_key.empty()) {
                const PropId insc = dpid(w, "inscription");
                const PropId contained = dpid(w, "contained");
                for (EntityId tid : w.entities_near(me_c->pos, dint(w, *me_c, reach, 64))) {
                    if (tid == npc) continue;
                    const Entity* t = w.find(tid);
                    if (!t || !t->alive) continue;
                    if (contained != kNoProp &&
                        w.prop_int(*t, contained, 0) != 0) continue;
                    if (dist2(me_c->pos, t->pos) > rr2sq) continue;
                    if (insc != kNoProp) {
                        if (const Value* iv = w.prop_of(*t, insc))
                            if (iv->type == ValType::Text &&
                                iv->s.find("R|") != std::string::npos)
                                continue;            // already carved
                    }
                    cool(st, npc, kEngrave, now, kCooldown[kEngrave]);
                    InteractionContext ec = mk(npc, {tid}, "engrave", 2);
                    ec.parameters.note = best_key;
                    if (w.interact(ec).completed) {
                        if (tally) ++tally->social;
                        break;
                    }
                }
                if (cooled(st, npc, kEngrave, now) == false) continue;
            }
        }

        // found: a Verified-holding agent with no nearby collective starts one
        if (cooled(st, npc, kFound, now)) {
            const AgentMind* mm = w.find_mind(npc);
            bool has_verified = false;
            if (mm) for (const auto& [k, rec] : mm->discoveries)
                if (rec.status == DiscoveryStatus::Verified) { has_verified = true; break; }
            if (has_verified) {
                bool near_collective = false;
                for (EntityId tid : w.entities_near(me_c->pos, rr2)) {
                    const Entity* t = w.find(tid);
                    if (!t || !t->alive) continue;
                    const PropId coll = dpid(w, "collective");
                    if (coll != kNoProp && w.prop_int(*t, coll, 0) != 0) {
                        near_collective = true; break;
                    }
                }
                if (!near_collective) {
                    cool(st, npc, kFound, now, kCooldown[kFound]);
                    if (w.interact(mk(npc, {}, "found", 2)).completed) {
                        if (tally) ++tally->social;
                        continue;
                    }
                }
            }
        }

        } while (false);
}

// ---- sliced round driver ---------------------------------------------------
// autonomous_cycle_step() runs the SAME round as autonomous_cycle — same
// agent order, same budgets, same interact() sequence — but returns to the
// caller after ~max_ms of wall-clock so the viewer's UI thread can take the
// world lock between batches. Resume state lives in DriverCycleSlice.
static bool cycle_step(World& w, DriverState* st, DriverTally* tally,
                       double max_ms, DriverCycleSlice& sl) {
    if (!sl.open) {
        const std::uint64_t now = w.clock_.tick();
        const bool concurrent = st && st->concurrent;
        if (concurrent) { w.defer_time = true; w.deferred_dt = 0; }
        // brain_shadow gates the WM learn hooks; brain_control implies them
        // (a controlling brain still predicts/learns every interact).
        if (st) w.brain_shadow = st->brain_shadow || st->brain_control;
        if (st) ++st->cycles;
        if (tally) ++tally->cycles;

        // Agency bootstrap: a living genome-carrier with no mind becomes an
        // agent once a minded agent can perceive it (sight_range World Truth).
        // Its mind starts EMPTY — knowledge must still be learned socially or
        // through its own experiments. This is the honest version of "growing
        // up": you exist as biomass first, personhood is acquired by contact.
        // No population cap — minds grow only as fast as nearby carriers are
        // perceived, and births are already throttled by parental energy cost.
        {
            // spatial promotion: for each minded agent, only nearby candidates —
            // O(minds x local density), not O(entities x minds) per cycle.
            // Candidate-centric: collect the (usually few) unminded carriers
            // first; no candidates -> no work. A candidate is promoted iff some
            // living minded agent has it within that agent's own sight_range.
            const PropId sight = dpid(w, "sight_range");
            std::vector<EntityId> cands;
            const bool game = st && st->curiosity;
            for (const auto& ep : w.entities)
                if (ep->alive && !ep->genome.loci.empty() && !w.find_mind(ep->id)) {
                    if (game) {           // game mode: plants never become minds
                        const SpeciesDef* sp = w.species.find_name(ep->kind_name.str());
                        if (!sp || !sp->fauna || sp->anatomy.empty()) continue;   // and no mindless swarms
                    }
                    cands.push_back(ep->id);
                }
            if (!cands.empty() && sight != kNoProp) {
                std::vector<EntityId> minded = w.minded_entities();
                // game mode (curiosity): only CULTURAL minds (species with
                // innate verbs) awaken others — otherwise every awakened
                // beast awakens the next and the whole region cascades into
                // thousands of minds (rounds of tens of seconds on a planet).
                if (st && st->curiosity)
                    minded.erase(std::remove_if(minded.begin(), minded.end(),
                        [&](EntityId id) { const Entity* a = w.find(id); return !a || !cultural_kind(w, *a); }),
                        minded.end());
                for (EntityId cid : cands) {
                    const Entity* ce = w.find(cid);
                    if (!ce) continue;
                    for (EntityId mid : minded) {
                        const Entity* a = w.find(mid);
                        if (!a || !a->alive || mid == cid) continue;
                        const std::int64_t sr = w.prop_int(*a, sight, 0);
                        if (sr > 0 && dist2(a->pos, ce->pos) <= sr * sr) {
                            w.mind(cid);                        // empty AgentMind
                            break;
                        }
                    }
                }
            }
        }

        sl.agents = w.minded_entities();                    // sorted ids
        sl.budget = kDiscoverPerCycle;                      // deterministic order
        // Primordial fairness (curiosity mode only): founders are created after
        // worldgen, so their ids sort AFTER every promoted animal and a plain
        // first-come budget would starve them of discovery forever. In curiosity
        // mode the discover budget is a deterministic round-robin instead:
        // agent #i may plan when (i + cycle) % stride == 0, and urgent
        // hunger/thirst always may (survival reasoning never queues).
        sl.stride = std::max<std::size_t>(
            1, (sl.agents.size() + kDiscoverPerCycle - 1) / kDiscoverPerCycle);
        sl.now = now;
        sl.concurrent = concurrent;
        sl.pos = 0;
        // Phase 4b: parallel think — pre-compute all brain choices against
        // the frozen round-start world before the first interact lands.
        if (st && st->parallel_think && st->brain_control &&
            !st->legacy_planner_compat && !w.speculative)
            parallel_think_pass(w, st, sl.agents, now);
        sl.open = true;
    }
    const auto t0 = std::chrono::steady_clock::now();
    while (sl.pos < sl.agents.size()) {
        cycle_one_agent(w, st, tally, sl.agents[sl.pos], sl.pos,
                        sl.stride, sl.budget, sl.now);
        ++sl.pos;
        if (std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count() >= max_ms)
            break;
    }
    if (sl.pos < sl.agents.size()) return false;
    if (sl.concurrent) {
        // the round lasted as long as its longest action (>= 1 tick)
        w.defer_time = false;
        const std::int64_t dt = std::max<std::int64_t>(1, w.deferred_dt);
        w.deferred_dt = 0;
        w.advance_ticks_for_test(dt);
    } else {
        w.advance_ticks_for_test(1);  // ambient drift: needs, heat, clock
    }
    sl.open = false;
    sl.agents.clear();
    sl.pos = 0;
    sl.budget = 0;
    return true;
}
}  // namespace

void autonomous_cycle(World& w, DriverState* st, DriverTally* tally) {
    DriverCycleSlice sl;
    while (!cycle_step(w, st, tally, 1e18, sl)) {}
}

bool autonomous_cycle_step(World& w, DriverState* st, DriverTally* tally,
                           double max_ms) {
    if (!st) { autonomous_cycle(w, nullptr, tally); return true; }
    return cycle_step(w, st, tally, max_ms, st->slice);
}

// Telemetry for long capstone runs: every 300 cycles print RSS + census so
// memory growth is observable without a profiler. stdout only — never
// feeds back into simulation state.
static void print_census_tick(const World& w, std::int64_t cycle) {
    std::int64_t rss_kb = -1;
    if (std::ifstream f("/proc/self/status"); f) {
        std::string line;
        while (std::getline(f, line))
            if (line.rfind("VmRSS:", 0) == 0) {
                rss_kb = std::atoll(line.c_str() + 6);
                break;
            }
    }
    std::int64_t alive = 0;
    for (const auto& e : w.entities) if (e->alive) ++alive;
    std::int64_t nconsume = 0, nrepro = 0, nmove = 0;
    for (const auto& ev : w.ledger.history()) {
        if (ev.action == "consume" && !ev.failed) ++nconsume;
        else if (ev.action == "reproduce" && !ev.failed) ++nrepro;
        else if (ev.action == "move" && !ev.failed) ++nmove;
    }
    std::printf("  [t=%ld] rss=%ldMB alive=%ld dead=%ld minds=%zu events=%zu "
                "consume=%ld repro=%ld move=%ld\n",
                (long)cycle, (long)(rss_kb / 1024), (long)alive,
                (long)(w.entities.size() - static_cast<std::size_t>(alive)),
                w.minded_entities().size(), w.ledger.size(),
                (long)nconsume, (long)nrepro, (long)nmove);
    // debug: every genome carrier's body state (alive and dead)
    const PropId hp = SIMLAB_PID(w.props, "hunger");
    const PropId tp = SIMLAB_PID(w.props, "thirst");
    const PropId ip = SIMLAB_PID(w.props, "integrity");
    const PropId ep = SIMLAB_PID(w.props, "energy");
    const PropId mp = SIMLAB_PID(w.props, "metabolism");
    for (const auto& e : w.entities) {
        if (e->genome.loci.empty()) continue;
        std::printf("    e%lu %s h=%ld th=%ld i=%ld en=%ld met=%ld pos=(%d,%d)\n",
                    (unsigned long)e->id, e->alive ? "live" : "dead",
                    (long)w.prop_int(*e, hp, -1), (long)w.prop_int(*e, tp, -1),
                    (long)w.prop_int(*e, ip, -1), (long)w.prop_int(*e, ep, -1),
                    (long)w.prop_int(*e, mp, -1),
                    (int)e->pos.x, (int)e->pos.y);
    }
}

DriverTally autonomous_run(World& w, std::int64_t cycles, DriverState* st) {
    DriverState local;
    if (!st) st = &local;
    DriverTally tally;
    for (std::int64_t i = 0; i < cycles; ++i) {
        autonomous_cycle(w, st, &tally);
        if ((i + 1) % 50 == 0) print_census_tick(w, i + 1);
        // every-50 carrier dump removed for speed
    }
    return tally;
}

}  // namespace simlab
