#include "simlab/Relations.hpp"

namespace simlab {

std::vector<Relation> RelationshipGraph::relations_of(EntityId id) const {
    std::vector<Relation> out;
    const auto it = incident_.find(id);
    if (it != incident_.end()) out.assign(it->second.begin(), it->second.end());
    return out;
}

std::vector<Relation> RelationshipGraph::relations_of(EntityId id,
                                                      const std::string& type) const {
    std::vector<Relation> out;
    const auto it = incident_.find(id);
    if (it == incident_.end()) return out;
    for (const Relation& r : it->second)
        if (r.type == type) out.push_back(r);
    return out;
}

}  // namespace simlab
