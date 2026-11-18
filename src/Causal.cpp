#include "simlab/Causal.hpp"
#include <algorithm>
#include <sstream>

namespace simlab {

namespace {
// Deterministic digest of one evicted event — folded into archived_hash_
// so the ledger window stays bounded while state_hash() still covers the
// entire causal history. Covers every field state_hash() mixes per event.
std::uint64_t event_digest(const CausalEvent& ev, std::uint64_t h) {
    auto mix = [&](std::uint64_t v) { h ^= v; h *= 1099511628211ull; };
    auto mixs = [&](const std::string& s) {
        mix(s.size()); for (char c : s) mix(static_cast<unsigned char>(c));
    };
    mix(ev.id); mix(ev.tick);
    mix(ev.participants.size()); for (EntityId p : ev.participants) mix(p);
    mix(ev.parent_events.size()); for (EventId p : ev.parent_events) mix(p);
    mix(ev.applied_laws.size()); for (LawId l : ev.applied_laws) mix(l);
    mix(ev.changes.size());
    for (const StateChange& c : ev.changes) {
        mix(c.entity); mix(c.prop); mix(c.had_before ? 1 : 0);
        mix(static_cast<std::uint64_t>(c.before.type)); mix(c.before.i);
        mix(c.before.b ? 1 : 0); mixs(c.before.s); mix(c.before.target);
        mix(static_cast<std::uint64_t>(c.after.type)); mix(c.after.i);
        mix(c.after.b ? 1 : 0); mixs(c.after.s); mix(c.after.target);
    }
    mix(ev.spawned.size()); for (EntityId s : ev.spawned) mix(s);
    mix(ev.failed ? 1 : 0); mixs(ev.action); mixs(ev.failure);
    mix(ev.conditions.size());
    for (const Condition& c : ev.conditions) {
        mixs(c.what); mix(static_cast<std::uint64_t>(c.actual));
        mix(static_cast<std::uint64_t>(c.required)); mix(c.met ? 1 : 0);
    }
    mixs(ev.evidence);
    return h;
}
}  // namespace

EventId EventLedger::append(CausalEvent e) {
    const EventId id = next_id_++;
    e.id = id;
    history_.push_back(std::move(e));
    // Amortized window eviction: only when the buffer exceeds window+batch
    // do we drop kEvictBatch oldest events — O(1) amortized, never per-append.
    if (history_.size() > kMaxWindow + kEvictBatch) {
        std::uint64_t h = archived_hash_;
        for (std::size_t i = 0; i < kEvictBatch; ++i)
            h = event_digest(history_[i], h);
        archived_hash_ = h;
        history_.erase(history_.begin(), history_.begin() + kEvictBatch);
    }
    return id;
}

const CausalEvent* EventLedger::find(EventId id) const {
    if (id == 0 || id >= next_id_) return nullptr;
    const EventId base = next_id_ - static_cast<EventId>(history_.size());
    if (id < base) return nullptr;              // archived — digest only
    return &history_[static_cast<std::size_t>(id - base)];
}

namespace {
void explain_rec(const EventLedger& led, EventId id, int depth, int max_depth,
                 std::string& out) {
    const CausalEvent* e = led.find(id);
    if (!e || depth > max_depth) return;
    out.append(static_cast<std::size_t>(depth) * 2, ' ');
    out += "#" + std::to_string(id) + " t=" + std::to_string(e->tick) +
           (e->action.empty() ? "" : " " + e->action) + (e->failed ? " FAILED(" + e->failure + ")" : "") +
           (e->evidence.empty() ? "" : "  " + e->evidence) + "\n";
    for (EventId p : e->parent_events) explain_rec(led, p, depth + 1, max_depth, out);
}
}

std::vector<Counterfactual> EventLedger::counterfactuals(EventId id) const {
    std::vector<Counterfactual> out;
    const CausalEvent* e = find(id);
    if (!e) return out;
    for (const Condition& c : e->conditions) {
        Counterfactual cf;
        cf.what = c.what; cf.actual = c.actual; cf.required = c.required;
        cf.margin = c.actual - c.required;
        // failed: every unmet condition alone blocked the outcome.
        // succeeded: a condition within 10% of its threshold (or exactly met)
        // is one the outcome hung on.
        cf.critical = c.met ? (c.required > 0 && cf.margin * 10 <= c.required) : true;
        out.push_back(cf);
    }
    return out;
}

std::vector<EventId> EventLedger::ancestors(EventId id, std::size_t limit) const {
    std::vector<EventId> out, stack;
    const CausalEvent* e = find(id);
    if (!e) return out;
    stack.insert(stack.end(), e->parent_events.begin(), e->parent_events.end());
    while (!stack.empty() && out.size() < limit) {
        const EventId cur = stack.back(); stack.pop_back();
        if (std::find(out.begin(), out.end(), cur) != out.end()) continue;
        const CausalEvent* c = find(cur);
        if (!c) continue;
        out.push_back(cur);
        stack.insert(stack.end(), c->parent_events.begin(), c->parent_events.end());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string EventLedger::explain(EventId id, int max_depth) const {
    std::string out;
    explain_rec(*this, id, 0, max_depth, out);
    return out;
}

}  // namespace simlab
