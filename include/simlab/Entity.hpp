#pragma once
#include <map>
#include <string>
#include <vector>
#include "Materials.hpp"
#include "Properties.hpp"
#include "Types.hpp"

namespace simlab {

class World;

// ---- Genome (TODO §8.4/§12.1 Biological Evolution) ----------------------------
// Hereditary loci: PropId -> allele (raw integer payload). Sorted by PropId
// for deterministic iteration/hashing. A genome is World Truth — it is NOT
// agent knowledge. Phenotype = the entity's expressed props; for quantitative
// traits expression is identity (allele == prop value). Which props are
// hereditary is DATA (species.txt `gene` lines), never engine-hardcoded.
struct Genome {
    std::vector<std::pair<PropId, std::int64_t>> loci;   // sorted by PropId
    const std::int64_t* get(PropId p) const;
    void set(PropId p, std::int64_t v);                  // sorted insert/replace
    std::size_t size() const { return loci.size(); }
};

// ---- Entity & ontology ------------------------------------------------------
// Entities are composition, not classes: identity + material + position +
// property overrides + lifecycle flag. Adding a new KIND of thing never
// requires a new C++ type — kind is an open registry id.
struct Entity {
    EntityId id = kNoEntity;
    std::uint32_t kind = 0;               // open kind id (0 = unclassified)
    std::string kind_name;                // optional label, never load-bearing
    MaterialId material = kNoMaterial;    // 0 = no material (abstract entity)
    DepositId deposit = kNoDeposit;       // >0 = natural occurrence (World Truth)
    Vec2 pos;
    PropertyBag props;                    // instance overrides (sparse)
    Genome genome;                        // hereditary loci (empty = non-organism)
    std::uint64_t last_metabolized = 0;    // LOD: tick of last need-drift update
    bool alive = true;

    // Effective property: instance override > material default > none.
    const Value* prop(const World& w, PropId p) const;
};

// Open entity-kind registry: kinds are named ids assigned on first use.
class EntityRegistry {
public:
    std::uint32_t kind_id(const std::string& name);   // assigns if new
    const std::string* kind_name(std::uint32_t id) const;
    std::uint32_t kind_count() const { return static_cast<std::uint32_t>(names_.size()); }
    std::uint64_t next_id() const { return next_id_; }
    EntityId allocate(std::uint32_t kind) {
        return (static_cast<EntityId>(kind & 0xFF) << 56) | (next_id_++ & 0x00FFFFFFFFFFFFFFull);
    }
    void set_next_id(std::uint64_t v) { next_id_ = v; }
    std::uint64_t counter() const { return next_id_; }

private:
    std::vector<std::string> names_;       // index 0 unused -> names_[id-1]
    std::map<std::string, std::uint32_t> by_name_;
    std::uint64_t next_id_ = 1;
};

}  // namespace simlab
