#pragma once
// Autonomous Simulation Driver (Phase 9): the missing "heartbeat" that turns
// the engine's laws + cognition into a self-running world. The driver is a
// CONTROLLER, not a law — it decides WHEN to perceive/act/move, but every
// state change still goes through World::interact -> EventLedger. Nothing
// here writes entity props directly.
//
// Per agent, per cycle:
//   1. perceive()                       — refresh observations/beliefs
//   2. maintenance                      — auto-wield best tool in reach when
//                                        hands are empty; auto-withdraw from
//                                        a rationed collective when starving
//   3. discover_step()                  — run the best hypothesis experiment,
//                                        or receive an ExploreGoal waypoint
//   4. locomotion                       — "move" toward the waypoint (speed
//                                        prop + terrain rules decide feasibility)
//   5. social reflexes                   — reproduce / communicate / engrave /
//                                        found, gated by cheap prop checks and
//                                        per-verb cooldowns (controller memory,
//                                        NOT world truth — never saved)
// Then one ambient advance_ticks(1) for metabolism/heat drift.
#include <cstdint>
#include <map>
#include <set>
#include <utility>
#include <vector>
#include "Types.hpp"
#include "World.hpp"

namespace simlab {

// Controller-side memory: per-(agent,verb) retry ticks. Deliberately NOT part
// of World Truth — a driver is a player/test harness, and its impatience must
// not leak into the simulation's deterministic state.
// An ExploreGoal the controller is still walking toward. Cached so a
// multi-tick journey does not re-run detect/generate hypotheses every
// cycle — discovery is expensive (speculative world clones), locomotion
// is not. Controller memory only: never saved, never hashed.
struct PendingExplore {
    Vec2 target{};
    EntityId subject = kNoEntity;
    std::uint64_t until = 0;          // give up after this tick
    std::int64_t last_integrity = -1; // Threat check: integrity drop cancels
};

struct DriverState {
    std::map<std::pair<EntityId, int>, std::uint64_t> next_try;
    std::map<EntityId, PendingExplore> pending;
    std::map<EntityId, std::uint64_t> next_discover;  // per-agent interval gate
    std::map<EntityId, int> discover_fails;           // consecutive empty discover_steps (backoff)
    std::uint64_t cycles = 0;
    // Primordial Genesis (viewer game mode). Both default OFF so every
    // pre-existing scenario keeps its exact trajectory/state_hash.
    //  * curiosity: idle agents "babble" — try a verb they do not know yet on
    //    something they remember within reach (or on two such things). The
    //    laws decide; a completed attempt teaches the verb through the normal
    //    learn_from_event path and witnesses pick it up by imitation.
    //    A whispered omen (`omen:<verb>[:<material>]` belief) biases which
    //    verb/target is tried, accepted or ignored by personality.
    //  * possessed: player-driven avatars — the driver only lets them
    //    perceive; every action comes from the player through interact().
    //  * concurrent: one round's actions share the clock (World::defer_time);
    //    the world advances once per round by the longest action.
    bool curiosity = false;
    bool concurrent = false;
    std::set<EntityId> possessed;
};

struct DriverTally {
    std::uint64_t cycles = 0;
    std::int64_t perceived = 0;
    std::int64_t experiments = 0;    // discover_step ran something
    std::int64_t moves = 0;
    std::int64_t social = 0;         // reproduce/communicate/engrave/found
    std::int64_t maintenance = 0;    // wield/withdraw
};

// Verbs curiosity may try on remembered things (also the whisperable set).
const std::vector<const char*>& curiosity_verbs();

// One full round: every minded, living agent gets maintenance + cognition +
// at most one social/move action, then the world drifts one tick.
void autonomous_cycle(World& w, DriverState* st = nullptr,
                      DriverTally* tally = nullptr);

// Run `cycles` autonomous rounds. Deterministic: same world state + same
// cycle count -> identical state_hash (driver state evolves purely from
// world reads, never from wall-clock or RNG).
DriverTally autonomous_run(World& w, std::int64_t cycles,
                           DriverState* st = nullptr);

}  // namespace simlab
