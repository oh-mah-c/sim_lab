#pragma once
#include <cstdint>
#include <string>
#include "Types.hpp"

namespace simlab {

// Deterministic RNG. Every consumer gets its own named stream derived from the
// world seed; there is no global RNG and no wall-clock input anywhere.
// Algorithm: splitmix64-style counter hashed per draw (same family as the
// proven Sunnyworld Rng). Pure integer arithmetic -> identical across platforms.
class Rng {
public:
    Rng() = default;
    Rng(std::uint64_t seed, std::string stream = "") { reseed(seed, stream); }

    void reseed(std::uint64_t seed, const std::string& stream) {
        seed_ = seed;
        stream_hash_ = hash(stream);
        counter_ = 0;
    }

    // Independent sub-stream for an entity/event without touching this one.
    static std::uint64_t stream_seed(std::uint64_t world_seed, const std::string& stream,
                                     std::uint64_t entity = 0) {
        std::uint64_t h = world_seed * 0x9E3779B97F4A7C15ull
                        ^ hash(stream) * 0xBF58476D1CE4E5B9ull
                        ^ (entity + 0x9E3779B9ull) * 0x94D049BB133111EBull;
        h ^= h >> 29; h *= 0x94D049BB133111EBull; h ^= h >> 32;
        return h;
    }

    std::uint64_t next() {
        std::uint64_t z = (seed_ ^ stream_hash_ ^ (counter_ + 0x9E3779B97F4A7C15ull))
                        * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        ++counter_;
        return z;
    }

    // Uniform in [0, n). n must be > 0.
    std::int64_t range(std::int64_t lo, std::int64_t hi) {
        if (hi < lo) return lo;
        const std::uint64_t span = static_cast<std::uint64_t>(hi - lo + 1);
        return lo + static_cast<std::int64_t>(next() % span);
    }
    // Permille chance: true with probability n/1000.
    bool chance(std::int64_t n, std::int64_t d = 1000) {
        return range(0, d - 1) < n;
    }

    std::uint64_t state() const { return counter_; }
    void set_state(std::uint64_t c) { counter_ = c; }

    static std::uint64_t hash(const std::string& s) {
        std::uint64_t h = 1469598103934665603ull;
        for (char c : s) { h ^= static_cast<unsigned char>(c); h *= 1099511628211ull; }
        return h;
    }
    static std::uint64_t hash(std::uint64_t v) {
        v ^= v >> 33; v *= 0xFF51AFD7ED558CCDull; v ^= v >> 33;
        v *= 0xC4CEB9FE1A85EC53ull; v ^= v >> 33;
        return v;
    }

private:
    std::uint64_t seed_ = 0, stream_hash_ = 0, counter_ = 0;
};

}  // namespace simlab
