// brain_bench — Phase 11 scale benchmark for the 12-module spiking brain.
// Usage: brain_bench [npc_count] [rounds] [threads]
// Spawns N sapient agents (minded), then runs `rounds` think-bursts:
// every agent's brain_decide() once per round, partitioned across `threads`
// (0 = auto, cap 16) exactly like Driver's parallel_think_pass — each decide
// mutates only its own BrainSlot, so results are thread-count invariant.
// The world never drifts during the bench (no interact/metabolize), so the
// population is stable and every NPC is measured every round.
// Reports: wall time, ms/NPC/round, agent-decisions/sec, active spike
// ratio %, brain memory bytes/NPC, save/load latency.
#include "simlab/World.hpp"
#include "simlab/Driver.hpp"
#include "simlab/Brain.hpp"
#include "simlab/Species.hpp"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace simlab;
using Clock = std::chrono::steady_clock;

int main(int argc, char** argv) {
    const int N = argc > 1 ? std::atoi(argv[1]) : 1000;
    std::int64_t rounds = argc > 2 ? std::atoll(argv[2]) : 0;
    if (rounds <= 0) rounds = N <= 500 ? 50 : (N <= 2000 ? 20 : 8);
    int T = argc > 3 ? std::atoi(argv[3]) : 0;

    World w(7, "data");
    const SpeciesDef* sp = w.species.find_name("sapient");
    const PropId energy = SIMLAB_PID(w.props, "energy");
    const PropId fatigue = SIMLAB_PID(w.props, "fatigue");
    const PropId integ = SIMLAB_PID(w.props, "integrity");
    const PropId hunger = SIMLAB_PID(w.props, "hunger");
    const PropId thirst = SIMLAB_PID(w.props, "thirst");
    static const char* const kVerbs[] = {"consume", "move", "rest"};

    const int side = static_cast<int>(std::sqrt(static_cast<double>(N))) + 1;
    std::vector<EntityId> agents;
    agents.reserve(N);
    for (int i = 0; i < N; ++i) {
        PropertyBag pb = sp ? sp->props : PropertyBag{};
        pb.set(energy, Value::integer(100000));
        pb.set(fatigue, Value::integer(100));
        pb.set(integ, Value::integer(1000));
        pb.set(hunger, Value::integer(0));
        pb.set(thirst, Value::integer(0));
        const Vec2 pos{static_cast<std::int32_t>((i % side) * 300 - 30000),
                       static_cast<std::int32_t>((i / side) * 300 - 30000)};
        const EntityId id = w.create_entity(
            "sapient", sp ? sp->material : kNoMaterial, pos, pb);
        if (!id) continue;
        AgentMind& m = w.mind(id);
        for (const char* v : kVerbs) m.learn_action(v);
        agents.push_back(id);
    }
    if (!w.brains_) w.brains_ = std::make_shared<BrainStore>();
    BrainStore& store = *w.brains_;
    // sequential slot pre-creation (arena may grow; map not thread-safe)
    for (EntityId id : agents) brain_slot(w, store, id);
    if (std::getenv("BB_SCALAR"))
        for (auto& kv : store.slots) kv.second.net.avx2 = false;
    fprintf(stderr, "bench: %zu sapients, %zu brain slots\n",
            agents.size(), store.slots.size());

    if (T <= 0) {
        const unsigned hw = std::thread::hardware_concurrency();
        T = static_cast<int>(hw ? hw : 2);
    }
    T = std::max(1, std::min(T, 16));
    T = std::min<int>(T, static_cast<int>(agents.size()));

    // ---- timed think rounds -----------------------------------------------------
    std::uint64_t decisions = 0;
    const auto t0 = Clock::now();
    for (std::int64_t r = 0; r < rounds; ++r) {
        if (T <= 1) {
            for (EntityId id : agents) {
                (void)brain_decide(w, id);
                ++decisions;
            }
        } else {
            std::vector<std::thread> pool;
            pool.reserve(static_cast<std::size_t>(T) - 1);
            const std::size_t n = agents.size();
            auto work = [&](int t) {
                const std::size_t lo = n * static_cast<std::size_t>(t) /
                                       static_cast<std::size_t>(T);
                const std::size_t hi = n * static_cast<std::size_t>(t + 1) /
                                       static_cast<std::size_t>(T);
                for (std::size_t i = lo; i < hi; ++i) {
                    (void)brain_decide(w, agents[i]);
                }
            };
            for (int t = 1; t < T; ++t) pool.emplace_back(work, t);
            work(0);
            for (auto& th : pool) th.join();
            decisions += n;
        }
    }
    const double sec = std::chrono::duration<double>(Clock::now() - t0).count();

    // ---- neural stats ------------------------------------------------------------
    std::uint64_t spikes = 0, neuron_ticks = 0;
    std::size_t slots_mem = 0;
    for (const auto& kv : store.slots) {
        const BrainSlot& s = kv.second;
        const BrainNet& n = s.net;
        spikes += n.spikes;
        neuron_ticks += static_cast<std::uint64_t>(n.neuron_count) *
                        std::max<std::uint64_t>(1, n.tick);
        slots_mem += sizeof(BrainSlot) + sizeof(BrainNet) +
            n.neuron_count * (sizeof(BrainNeuronState) +
                              sizeof(BrainNeuronGen) +
                              sizeof(std::array<std::uint64_t, 4>) +
                              sizeof(std::uint8_t) + sizeof(std::uint32_t) +
                              sizeof(std::uint8_t)) +
            n.syn_count * (sizeof(BrainSynapse) + sizeof(std::int8_t)) +
            (n.neuron_count + 1) * sizeof(std::uint32_t) +
            n.spike_masks.capacity() * sizeof(std::uint64_t) +
            n.kick_hist.capacity() * sizeof(n.kick_hist[0]) +
            n.recent_pos.capacity() + n.last_fire.capacity() * 8 +
            n.module_of.capacity() + n.dw.capacity() +
            n.row_ptr.capacity() * 4 + 16 * 64;
    }
    const double spike_pct = neuron_ticks
        ? 100.0 * static_cast<double>(spikes) / neuron_ticks : 0.0;

    // ---- save / load latency ------------------------------------------------------
    const char* sv = "/tmp/brain_bench_save.bin";
    std::string err;
    const auto s0 = Clock::now();
    const bool ok_s = w.save(sv, &err);
    const double save_ms = std::chrono::duration<double, std::milli>(
        Clock::now() - s0).count();
    World w2(7, "data");
    const auto l0 = Clock::now();
    const bool ok_l = w2.load(sv, &err);
    const double load_ms = std::chrono::duration<double, std::milli>(
        Clock::now() - l0).count();

    printf("N=%zu rounds=%lld threads=%d\n",
           agents.size(), (long long)rounds, T);
    printf("wall=%.2fs  us/NPC/round=%.1f  decisions/s=%.0f\n",
           sec, sec * 1e6 / (decisions > 0 ? decisions : 1),
           decisions / (sec > 0 ? sec : 1e-9));
    printf("brain slots=%zu  spikes=%llu  spike_ratio=%.2f%%\n",
           store.slots.size(), (unsigned long long)spikes, spike_pct);
    printf("brain_state_hash=%016llx\n",
           (unsigned long long)brain_state_hash(store));
    printf("brain_mem=%.1f KB/NPC  (total %.1f MB)\n",
           store.slots.empty() ? 0.0 : slots_mem / 1024.0 / store.slots.size(),
           slots_mem / 1048576.0);
    printf("save=%s %.0fms  load=%s %.0fms\n",
           ok_s ? "ok" : "FAIL", save_ms, ok_l ? "ok" : "FAIL", load_ms);
    return 0;
}
