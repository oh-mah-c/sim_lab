#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "Materials.hpp"
#include "Types.hpp"

namespace simlab {

// ---- Process registry (TODO §9.5) ---------------------------------------------
// A processing recipe is DATA, not code: required inputs, fuel, minimum
// temperature, duration, outputs and waste. The engine supplies the physics;
// the data supplies the recipe. Adding "smelt tin" later = a new block here,
// zero engine changes.
//
// File format (one process per block, '#' comments):
//   process <name>
//     input <material_name> <qty_units>     # repeatable; consumed
//     fuel <material_name> <qty_units>      # repeatable; consumed
//     min_temperature <deci-°C>             # any participant must reach this
//     ticks <n>                             # simulated duration (recorded)
//     output <material_name> <qty_units>    # repeatable; produced
//     waste <material_name> <qty_units>     # repeatable; produced (mass balance)
struct ProcessIO {
    std::string material_name;
    MaterialId material = kNoMaterial;   // resolved at load
    std::int64_t qty = 0;                // units
};

struct ProcessDef {
    std::uint32_t id = 0;
    std::string name;
    std::vector<ProcessIO> inputs;
    std::vector<ProcessIO> fuels;
    std::vector<ProcessIO> outputs;
    std::vector<ProcessIO> wastes;
    std::int64_t min_temperature = 0;    // deci-°C; 0 = no heat required
    std::int64_t ticks = 1;
    std::string provenance;              // file the recipe came from
};

class ProcessRegistry {
public:
    bool load_file(const std::string& path,
                   const MaterialRegistry& materials,
                   std::string* err = nullptr);
    bool parse(const std::string& text,
               const MaterialRegistry& materials,
               std::string* err = nullptr);

    const ProcessDef* find(const std::string& name) const;
    const std::vector<ProcessDef>& defs() const { return defs_; }
    std::size_t size() const { return defs_.size(); }

private:
    std::vector<ProcessDef> defs_;
    std::map<std::string, std::uint32_t> by_name_;
};

}  // namespace simlab
