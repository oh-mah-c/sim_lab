#pragma once
#include <string>
#include <vector>
#include "Types.hpp"

namespace simlab {

// ---- Causal layer -----------------------------------------------------------
// Every world change is an event with explicit parents, the laws that produced
// it, and the state changes it committed. The ledger is the answer to "why did
// this happen?" — inspectable, replayable, never decorative.
struct StateChange {
    EntityId entity = kNoEntity;
    PropId prop = kNoProp;
    Value before, after;                  // before may be "unset" for new props
    bool had_before = false;
};

// A condition a law checked while deciding this outcome (TODO §7 q.3/q.6).
// Recorded as DATA so the engine can answer "would the result change if this
// condition were missing?" without replaying: an event succeeded only because
// actual >= required held; a failed attempt records which condition blocked it.
struct Condition {
    std::string what;                     // "force", "temperature", "reach", "energy", ...
    std::int64_t actual = 0;
    std::int64_t required = 0;
    bool met = true;                      // actual >= required
};

// Counterfactual reading of one condition of an event.
struct Counterfactual {
    std::string what;
    std::int64_t actual = 0, required = 0;
    std::int64_t margin = 0;              // actual - required (negative = was short)
    bool critical = false;                // a small change flips the outcome
};

struct CausalEvent {
    EventId id = kNoEvent;
    std::uint64_t tick = 0;
    std::vector<EntityId> participants;
    std::vector<EventId> parent_events;   // direct causes — the causal chain
    std::vector<LawId> applied_laws;      // which natural laws fired
    std::vector<StateChange> changes;
    std::vector<EntityId> spawned;        // entities created by this event
    std::string action;                   // interaction verb (dig/strike/...)
    bool failed = false;                  // attempted but physically impossible
    std::string failure;                  // why (failed events only)
    std::vector<Condition> conditions;    // what the laws checked
    std::string evidence;                 // human-readable derivation, for tools
};

class EventLedger {
public:
    // Bounded RAM window (Phase 9): only the most recent kMaxWindow events
    // are kept in memory. Older events are evicted in batches and folded
    // into `archived_hash_` — a deterministic digest of everything evicted,
    // so state_hash() still covers the FULL history without the RAM cost.
    // Evicted events remain causally valid history (their ids are stable);
    // find()/explain()/ancestors() simply return nullptr for them.
    static constexpr std::size_t kMaxWindow = 20000;
    static constexpr std::size_t kEvictBatch = 1024;

    EventId append(CausalEvent e);        // assigns id = next_id_++
    // RAM budget (Phase 13, 4GB machines): besides the kMaxWindow COUNT
    // bound, keep the retained window under `bytes` (0 = no byte bound,
    // the default — preserves every existing replay/hash). Eviction is the
    // same deterministic fold into archived_hash_, so state_hash stays a
    // pure function of (history, budget). Per-tick metabolize events carry a
    // StateChange per drifting entity, so a count-only window can grow to
    // gigabytes on a large world.
    void set_byte_budget(std::size_t bytes) { byte_budget_ = bytes; }
    std::size_t byte_budget() const { return byte_budget_; }
    std::size_t retained_bytes() const { return retained_bytes_; }
    const CausalEvent* find(EventId id) const;
    const std::vector<CausalEvent>& history() const { return history_; }
    std::size_t size() const { return history_.size(); }
    EventId next_id() const { return next_id_; }
    std::uint64_t archived_hash() const { return archived_hash_; }
    // Restore archive state on load: `next` = id of the first retained event
    // (appends then continue the original id sequence).
    void set_archive(std::uint64_t h, EventId next) {
        archived_hash_ = h; next_id_ = next;
    }
    // Render the causal chain of an event as indented text (bounded depth).
    std::string explain(EventId id, int max_depth = 8) const;
    // Which recorded conditions were decisive? (TODO §7 q.6)
    std::vector<Counterfactual> counterfactuals(EventId id) const;
    // All ancestor events (parents, grandparents...), sorted, bounded by `limit`.
    std::vector<EventId> ancestors(EventId id, std::size_t limit = 4096) const;
    void clear() { history_.clear(); next_id_ = 1; archived_hash_ = 0; retained_bytes_ = 0; }

private:
    std::vector<CausalEvent> history_;    // RETAINED window only
    EventId next_id_ = 1;                 // one past the last assigned id
    std::uint64_t archived_hash_ = 0;     // FNV-1a digest of evicted events
    std::size_t byte_budget_ = 0;         // 0 = unbounded (count window only)
    std::size_t retained_bytes_ = 0;      // estimated heap of the retained window
};

}  // namespace simlab
