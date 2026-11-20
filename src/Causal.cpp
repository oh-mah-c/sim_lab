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

namespace {
// heap estimate of one event (exact enough to bound RAM; deterministic)
std::size_t event_bytes(const CausalEvent& e) {
    std::size_t b = sizeof(CausalEvent) + e.evidence.capacity() + e.action.capacity() + e.failure.capacity();
    b += e.changes.capacity() * sizeof(StateChange);
    for (const StateChange& c : e.changes) b += c.before.s.capacity() + c.after.s.capacity();
    b += e.participants.capacity() * sizeof(EntityId) + e.parent_events.capacity() * sizeof(EventId);
    b += e.applied_laws.capacity() * sizeof(LawId) + e.spawned.capacity() * sizeof(EntityId);
    b += e.conditions.capacity() * sizeof(Condition);
    return b;
}
}  // namespace

EventId EventLedger::append(CausalEvent e) {
    const EventId id = next_id_++;
    e.id = id;
    retained_bytes_ += event_bytes(e);
    history_.push_back(std::move(e));
    // Amortized window eviction: only when the buffer exceeds window+batch
    // (or the byte budget) do we drop the oldest events — O(1) amortized.
    const bool over_count = history_.size() > kMaxWindow + kEvictBatch;
    const bool over_bytes = byte_budget_ > 0 && retained_bytes_ > byte_budget_ &&
                            history_.size() > 1;
    if (over_count || over_bytes) {
        std::size_t n = over_count ? kEvictBatch : 0;
        if (over_bytes) {          // evict until 3/4 of the budget is free again
            std::size_t freed = 0, need = retained_bytes_ - byte_budget_ * 3 / 4;
            std::size_t k = 0;
            while (k + 1 < history_.size() && freed < need) freed += event_bytes(history_[k++]);
            n = std::max(n, k);
        }
        n = std::min(n, history_.size() - 1);
        std::uint64_t h = archived_hash_;
        for (std::size_t i = 0; i < n; ++i) {
            h = event_digest(history_[i], h);
            retained_bytes_ -= std::min(retained_bytes_, event_bytes(history_[i]));
        }
        archived_hash_ = h;
        history_.erase(history_.begin(), history_.begin() + static_cast<std::ptrdiff_t>(n));
        if (over_bytes) history_.shrink_to_fit();
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
