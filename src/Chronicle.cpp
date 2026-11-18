#include "simlab/Chronicle.hpp"
#include "simlab/Cognition.hpp"
#include <algorithm>
#include <set>

namespace simlab {

Chronicle analyze_world(const World& w) {
    Chronicle c;
    c.tick = w.clock_.tick();
    c.minds = static_cast<std::int64_t>(w.minded_entities().size());

    const PropId hunger = w.props.find_name("hunger")
        ? w.props.find_name("hunger")->id : kNoProp;
    const PropId thirst = w.props.find_name("thirst")
        ? w.props.find_name("thirst")->id : kNoProp;
    const PropId collective_p = w.props.find_name("collective")
        ? w.props.find_name("collective")->id : kNoProp;
    const PropId inscription_p = w.props.find_name("inscription")
        ? w.props.find_name("inscription")->id : kNoProp;

    std::map<std::string, CensusRow> census;
    std::set<std::string> lineages;
    std::set<std::string> verified;
    std::vector<EntityId> collectives;

    for (const auto& ep : w.entities) {
        const Entity& e = *ep;
        CensusRow& row = census[e.kind_name];
        row.kind = e.kind_name;
        if (e.alive) {
            ++row.alive; ++c.entities_alive;
        } else {
            ++row.dead; ++c.entities_dead;
            // death-cause heuristic on World Truth props, not on intent
            const std::int64_t h = hunger != kNoProp ? w.prop_int(e, hunger, 0) : 0;
            const std::int64_t t = thirst != kNoProp ? w.prop_int(e, thirst, 0) : 0;
            if (h >= 900 || t >= 900) ++row.starved; else ++row.other_dead;
        }

        if (!e.alive || e.genome.loci.empty()) continue;
        ++c.genome_carriers;
        // locus-set signature = speciation lineage
        std::string sig;
        for (const auto& [pid, v] : e.genome.loci) {
            sig += std::to_string(pid) + ":" + std::to_string(v) + ";";
            const PropertyDef* pd = w.props.find(pid);
            const std::string pname = pd ? pd->name : std::to_string(pid);
            auto& [sum, cnt] = c.allele_sums[pname];
            sum += v; ++cnt;
        }
        lineages.insert(sig);

        if (e.kind_name == "composite") ++c.composites;
        if (inscription_p != kNoProp) {
            if (const Value* iv = w.prop_of(e, inscription_p))
                if (iv->type == ValType::Text && !iv->s.empty()) ++c.inscribed;
        }
        if (collective_p != kNoProp) {
            if (const Value* cv = w.prop_of(e, collective_p))
                if (cv->type == ValType::Boolean && cv->b) {
                    ++c.collectives;
                    collectives.push_back(e.id);
                }
        }
    }
    c.genome_lineages = static_cast<std::int64_t>(lineages.size());
    for (auto& [k, r] : census) c.census.push_back(std::move(r));
    std::sort(c.census.begin(), c.census.end(),
              [](const CensusRow& a, const CensusRow& b) { return a.kind < b.kind; });

    // Verified methods across every agent mind (MethodKey strings).
    for (EntityId mid : w.minded_entities()) {
        const AgentMind* m = w.find_mind(mid);
        if (!m) continue;
        for (const auto& [key, rec] : m->discoveries)
            if (rec.status == DiscoveryStatus::Verified) verified.insert(key);
    }
    c.verified_methods.assign(verified.begin(), verified.end());

    // Leadership: for each collective, the member with the greatest summed
    // `authority` received from the OTHER members (emergent, not elected).
    for (EntityId g : collectives) {
        std::vector<EntityId> members;
        for (const Relation& rel : w.relations_of(g, "member_of"))
            if (rel.b == g) members.push_back(rel.a);
        for (EntityId m : members) {
            std::int64_t auth = 0;
            for (EntityId o : members)
                if (o != m) auth += w.tie_weight(o, "authority", m);
            if (auth > c.top_authority) {
                c.top_authority = auth;
                c.top_leader = m;
                const Entity* le = w.find(m);
                c.leader_kind = le ? le->kind_name : "";
            }
        }
    }

    c.inventory = w.material_inventory();
    c.energy = w.energy_accounting();
    return c;
}

}  // namespace simlab
