#pragma once
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>
#include "Types.hpp"

namespace simlab {

// ---- Relationship graph -----------------------------------------------------
// Open-ended entity-to-entity relations ("located_in", "part_of", "owns", ...).
// Relation types are strings — new relation kinds are data, not code.
// Stored in a sorted set so iteration and hashing are deterministic.
struct Relation {
    EntityId a = kNoEntity;
    std::string type;
    EntityId b = kNoEntity;
    bool operator<(const Relation& o) const {
        return std::tie(a, type, b) < std::tie(o.a, o.type, o.b);
    }
    bool operator==(const Relation& o) const {
        return a == o.a && type == o.type && b == o.b;
    }
};

class RelationshipGraph {
public:
    void relate(EntityId a, const std::string& type, EntityId b) {
        if (a == kNoEntity || b == kNoEntity || a == b) return;
        if (!edges_.insert({a, type, b}).second) return;
        incident_[a].insert({a, type, b});
        incident_[b].insert({a, type, b});
    }
    void unrelate(EntityId a, const std::string& type, EntityId b) {
        if (!edges_.erase({a, type, b})) return;
        drop(a, {a, type, b});
        drop(b, {a, type, b});
    }
    bool related(EntityId a, const std::string& type, EntityId b) const {
        return edges_.count({a, type, b}) != 0;
    }
    // All relations touching `id` (as source or target), deterministic order.
    std::vector<Relation> relations_of(EntityId id) const;
    // Relations of a specific type touching `id`.
    std::vector<Relation> relations_of(EntityId id, const std::string& type) const;
    std::size_t size() const { return edges_.size(); }
    const std::set<Relation>& edges() const { return edges_; }
    void clear() { edges_.clear(); incident_.clear(); }

private:
    void drop(EntityId id, const Relation& r) {
        const auto it = incident_.find(id);
        if (it == incident_.end()) return;
        it->second.erase(r);
        if (it->second.empty()) incident_.erase(it);
    }
    std::set<Relation> edges_;
    // per-entity index of touching edges (sorted like edges_) -> O(deg) queries
    std::map<EntityId, std::set<Relation>> incident_;
};

}  // namespace simlab
