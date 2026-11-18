#include "simlab/Driver.hpp"
#include "simlab/Cognition.hpp"
#include "simlab/Discovery.hpp"
#include "simlab/Laws.hpp"
#include "simlab/WorldGen.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <set>
#include <string>

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

enum VerbSlot { kWield = 0, kWithdraw, kReproduce, kCommunicate, kEngrave, kFound, kConsume };

PropId dpid(const World& w, const char* n) {
    const PropertyDef* d = w.props.find_name(n);
    return d ? d->id : kNoProp;
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

void autonomous_cycle(World& w, DriverState* st, DriverTally* tally) {
    const std::uint64_t now = w.clock_.tick();
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
        for (const auto& ep : w.entities)
            if (ep->alive && !ep->genome.loci.empty() && !w.find_mind(ep->id))
                cands.push_back(ep->id);
        if (!cands.empty() && sight != kNoProp) {
            const std::vector<EntityId> minded = w.minded_entities();
            for (EntityId cid : cands) {
                const Entity* ce = w.find(cid);
                if (!ce) continue;
                for (EntityId mid : minded) {
                    const Entity* a = w.find(mid);
                    if (!a || !a->alive || mid == cid) continue;
                    const std::int64_t sr = w.prop_int(*a, sight, 0);
                    if (sr > 0 && dist2(a->pos, ce->pos) <= sr * sr) {
                        w.mind(cid);                            // empty AgentMind
                        break;
                    }
                }
            }
        }
    }

    const std::vector<EntityId> agents = w.minded_entities();   // sorted ids
    int discover_budget = kDiscoverPerCycle;   // deterministic: sorted order
    for (EntityId npc : agents) {
        Entity* me = w.find(npc);
        if (!me || !me->alive) continue;

        // 1. perception
        const std::size_t nobs = perceive(w, npc);
        if (tally) tally->perceived += static_cast<std::int64_t>(nobs);

        // 1b. lazy worldgen: materialize entity stages around the agent when
        // it nears the edge of the generated zone (World bookkeeping only).
        ensure_region(w, npc);

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

        // 2a. maintenance: wield best tool when hands are empty
        if (!already_wielding(w, npc) && cooled(st, npc, kWield, now)) {
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
                InteractionContext fc = mk(npc, {}, "move", 1);
                fc.parameters.destination = fd;
                fc.parameters.has_destination = true;
                if (w.interact(fc).completed) {
                    if (tally) ++tally->moves;
                    continue;
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
                        rc.parameters.destination = {me->pos.x + away.x,
                                                     me->pos.y + away.y};
                        rc.parameters.has_destination = true;
                    } else {
                        rc.parameters.action = "rest";   // grief: curl up
                    }
                    w.interact(rc);
                    continue;                    // reflex consumed the cycle
                }
            }
        }

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
        if (!walking_pending && !gated && discover_budget > 0) {
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
            InteractionContext mc = mk(npc, {}, "move", 1);
            mc.parameters.destination = goal.target;
            mc.parameters.has_destination = true;
            if (w.interact(mc).completed) {
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

    }
    w.advance_ticks_for_test(1);  // ambient drift: needs, heat, clock
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
    const PropId hp = w.props.find_name("hunger") ? w.props.find_name("hunger")->id : kNoProp;
    const PropId tp = w.props.find_name("thirst") ? w.props.find_name("thirst")->id : kNoProp;
    const PropId ip = w.props.find_name("integrity") ? w.props.find_name("integrity")->id : kNoProp;
    const PropId ep = w.props.find_name("energy") ? w.props.find_name("energy")->id : kNoProp;
    const PropId mp = w.props.find_name("metabolism") ? w.props.find_name("metabolism")->id : kNoProp;
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
