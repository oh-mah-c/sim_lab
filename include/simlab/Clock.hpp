#pragma once
#include <cstdint>

namespace simlab {

// Simulation clock. 1 tick = 1 in-game hour; 24 ticks/day; 112 days/year.
// All scheduling keys off tick() — no wall-clock time is ever read.
class SimulationClock {
public:
    static constexpr std::uint64_t TICKS_PER_HOUR = 1;
    static constexpr std::uint64_t TICKS_PER_DAY = 24;
    static constexpr std::uint64_t DAYS_PER_SEASON = 28;
    static constexpr std::uint64_t DAYS_PER_YEAR = 112;
    static constexpr std::uint64_t TICKS_PER_YEAR = TICKS_PER_DAY * DAYS_PER_YEAR;

    enum class Season : std::uint8_t { Spring = 0, Summer, Autumn, Winter };

    void advance() { ++tick_; }
    std::uint64_t tick() const { return tick_; }
    std::uint64_t day_index() const { return tick_ / TICKS_PER_DAY; }
    int day_of_year() const { return static_cast<int>(day_index() % DAYS_PER_YEAR); }
    Season season() const { return static_cast<Season>(day_of_year() / DAYS_PER_SEASON); }
    int year() const { return static_cast<int>(day_index() / DAYS_PER_YEAR); }
    bool is_day_start() const { return tick_ % TICKS_PER_DAY == 0; }

    std::uint64_t tick_ = 0;
};

}  // namespace simlab
