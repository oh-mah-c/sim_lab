#pragma once
#include <cstdint>
#include <memory>
#include <string>

// Sunnyworld Chronicle — era-boundary literary historian (Qwen 1.5B).
//
// tick() is invoked once per simulation cycle/frame by the host app
// (viewer or a headless runner). It is STRICTLY read-only with respect to
// World: it scans the causal ledger, diffs the alive set, samples brain
// slots and terrain, then hands a formatted prompt to a NarrativeHost
// worker thread. Completed chapters append to `out_path`.
//
// Determinism isolation: this object lives outside World, uses no RNG,
// mutates no entity/prop/ledger state, and the LLM worker never touches
// World — state_hash(), t1==t8 and dev==release are unaffected whether
// the chronicle is on or off.

namespace simlab {

class World;
class NarrativeHost;

class SunnyChronicle {
public:
    struct Config {
        std::uint64_t era_ticks = 500;    // world ticks per chapter
        std::string out_path = "saves/sunnyworld_chronicle.md";
        std::string language = "English";   // chapter language
        int top_moments = 6;               // highest-salience moments kept
    };

    SunnyChronicle();
    explicit SunnyChronicle(Config cfg);
    ~SunnyChronicle();
    SunnyChronicle(const SunnyChronicle&) = delete;
    SunnyChronicle& operator=(const SunnyChronicle&) = delete;

    // Call once per cycle with the world (any thread that holds the world
    // lock, or none — the collector only reads). `host` may be null or
    // inactive: the raw fact record is still appended so the chronicle
    // captures everything even without a model.
    void tick(World& w, NarrativeHost* host);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace simlab
