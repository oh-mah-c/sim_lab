#pragma once
#include <map>
#include <string>
#include <vector>
#include "Deposits.hpp"
#include "Materials.hpp"
#include "Properties.hpp"
#include "Types.hpp"

namespace simlab {

// ---- Species registry (TODO §2.1 Ecology Generator) ---------------------------
// A species is HABITAT DATA: which climate/terrain conditions let an organism
// live, plus the material/props of an individual. Adding a species = editing
// data/species.txt; worldgen contains no species names.
// One anatomical part in a species body plan (Phase 10): spawned as a
// contained `part_of` child entity with its own layered material.
struct PartSpec {
    std::string name;          // entity kind suffix, e.g. "eye_l"
    MaterialId material = kNoMaterial;
    std::string slot;          // body_slot: head/torso/arm_l/leg_r/...
    std::string role;          // part_role: eye/heart/lung/arm/leg/brain/...
    std::int64_t vital = 0;    // vital_organ permille: 1000 = lethal to lose
    std::int64_t size = 200;   // part_size permille: hit-weight within parent
    PropId blood_p = kNoProp;  // resolved blood_volume prop (shared)
    PropertyBag extra;         // optional key=value organ props (lumen mechanics)
};

struct SpeciesDef {
    std::uint32_t id = 0;
    std::string name;
    bool fauna = false;                          // false = flora
    MaterialId material = kNoMaterial;
    std::int64_t density = 0;                    // permille chance per eligible cell
    std::vector<FormationCondition> conditions;  // habitat (all must hold)
    PropertyBag props;                           // per-individual props
    std::vector<PropId> genes;                   // hereditary loci (Phase 7)
    std::vector<PartSpec> anatomy;               // body plan (Phase 10)
    std::vector<std::string> innate;             // instinct verbs a founder mind starts with
    std::string provenance;
};

class SpeciesRegistry {
public:
    bool load_file(const std::string& path, const PropertyRegistry& props,
                   const MaterialRegistry& materials, std::string* err = nullptr);
    bool parse(const std::string& text, const PropertyRegistry& props,
               const MaterialRegistry& materials, std::string* err = nullptr);
    std::uint32_t add(SpeciesDef def);           // 0 on duplicate/invalid
    const SpeciesDef* find(std::uint32_t id) const;
    const SpeciesDef* find_name(const std::string& name) const;
    const std::vector<SpeciesDef>& defs() const { return defs_; }

private:
    std::vector<SpeciesDef> defs_;               // defs_[i].id == i+1
    std::map<std::string, std::uint32_t> by_name_;
};

}  // namespace simlab
