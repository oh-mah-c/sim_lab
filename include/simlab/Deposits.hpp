#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "Materials.hpp"
#include "Properties.hpp"
#include "Types.hpp"

namespace simlab {

// ---- Deposit / occurrence registry ------------------------------------------
// A DepositDef is WORLD TRUTH data: where a natural occurrence exists, what it
// is made of, and its physical properties. It is NOT knowledge — agents must
// discover deposits through observation (see TODO §9).
//
// Five concepts stay distinct (TODO §9.1):
//   substance      -> entries in MaterialRegistry (raw natural materials)
//   deposit def    -> this file: composition + physical props + accessibility
//   occurrence     -> an Entity with Entity::deposit set (placed in the world)
//   extracted resource -> entity spawned by extraction laws (Phase 3)
//   processed material  -> entity/material after processing (Phase 3)

struct DepositComponent {
    MaterialId material = kNoMaterial;   // substance produced on extraction
    std::int64_t share = 0;              // permille of total yield (sum = 1000)
};

// A formation condition gates where a deposit may occur (TODO §2.2).
// field: "elevation" (m) | "moisture" (permille) | "temperature" (deci-°C)
//        | "slope" (m) | "near_water" (0/1)
struct FormationCondition {
    std::string field;
    std::int64_t lo = 0, hi = 0;
};

// True for every field name usable in a formation/habitat `condition` line
// (evaluated by worldgen from analytic world fields).
bool is_condition_field(const std::string& name);

struct DepositDef {
    DepositId id = kNoDeposit;
    std::string name;
    std::vector<DepositComponent> composition;  // sorted by MaterialId
    PropertyBag props;                    // hardness, yield_rate, reserve, ...
    // Observable Evidence (TODO §9.10.2): surface signs a passer-by can see even
    // when the deposit is buried (colour stain, trace minerals...). observable-
    // domain props only; copied onto the occurrence entity as World Truth.
    // A buried deposit with NO evidence is undetectable by perception.
    PropertyBag evidence;
    std::vector<FormationCondition> conditions;  // where it may form (empty = anywhere)
    bool surface = false;                 // accessibility: true = surface, false = buried
    std::int64_t depth = 0;               // metres below surface (0 for surface)
    std::string provenance;
};

class DepositRegistry {
public:
    // File format (one deposit per block, '#' comments):
    //   deposit <name>
    //     substance <material_name> <share_permille>   (repeatable, sum=1000)
    //     accessibility surface|buried
    //     depth <metres>
    //     condition <field> <lo> <hi>                  (repeatable; see FormationCondition)
    //     <prop_name> <value>                          (any registered property)
    //     evidence <prop_name> <value>                 (observable-domain sign, repeatable)
    bool load_file(const std::string& path,
                   const PropertyRegistry& props,
                   const MaterialRegistry& materials,
                   std::string* err = nullptr);
    bool parse(const std::string& text,
               const PropertyRegistry& props,
               const MaterialRegistry& materials,
               std::string* err = nullptr);

    DepositId add(DepositDef def);        // kNoDeposit on duplicate/invalid
    const DepositDef* find(DepositId id) const;
    const DepositDef* find_name(const std::string& name) const;
    DepositId id_of(const std::string& name) const;
    std::size_t size() const { return defs_.size() + 1; }
    const std::vector<DepositDef>& defs() const { return defs_; }

private:
    std::vector<DepositDef> defs_;        // defs_[i].id == i+1
    std::map<std::string, DepositId> by_name_;
};

}  // namespace simlab
