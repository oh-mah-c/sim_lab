#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "Materials.hpp"
#include "Properties.hpp"
#include "Types.hpp"

namespace simlab {

// ---- Influx zones (edge migration) -------------------------------------------
// A zone is DATA, not code: an axis-aligned band at the world edge plus the
// migrant templates allowed through it. The engine owns ONE generic loop —
// deterministic flux schedule (hash of world seed + zone + tick), local
// capacity gate, template instantiation. Adding a corridor, a fish run or a
// dead desert (empty influx_pool) is editing data/influx.txt only.
//
// File format (one zone per block, '#' comments):
//   influx_zone <name>
//     bounds <x0> <y0> <x1> <y1>   # inclusive cell range
//     interval <ticks>             # mean ticks between flux events (>0)
//     capacity <n>                 # max living migrant-kind entities inside
//     migrants <n>                 # individuals per flux event
//     migrant <template_name>
//       kind <kind_name>
//       material <material_name>   # optional
//       prop <name> <value>        # repeatable instance override (raw per type)
//       gene <prop_name> <allele>  # repeatable genome locus
//       innate <verb>              # repeatable: sapient migrant -> AgentMind
//                                   # created with these actions pre-learned
struct InfluxGene {
    PropId prop = kNoProp;
    std::int64_t allele = 0;
};

struct InfluxMigrant {
    std::string name;
    std::string kind;
    MaterialId material = kNoMaterial;
    PropertyBag props;
    std::vector<InfluxGene> genes;
    std::vector<std::string> innate;   // non-empty -> spawn gets an AgentMind
};

struct InfluxZone {
    std::uint32_t id = 0;
    std::string name;
    std::int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    std::int64_t interval = 0;         // mean ticks between flux events
    std::int64_t capacity = 0;         // local carrying capacity inside bounds
    std::int64_t migrants = 0;         // group size per flux event
    std::vector<InfluxMigrant> templates;
    std::string provenance;

    bool contains(Vec2 p) const {
        return p.x >= x0 && p.x <= x1 && p.y >= y0 && p.y <= y1;
    }
};

class InfluxRegistry {
public:
    bool load_file(const std::string& path, const PropertyRegistry& props,
                   const MaterialRegistry& materials, std::string* err = nullptr);
    bool parse(const std::string& text, const PropertyRegistry& props,
               const MaterialRegistry& materials, std::string* err = nullptr);

    const InfluxZone* find(std::uint32_t id) const;
    const InfluxZone* find_name(const std::string& name) const;
    const std::vector<InfluxZone>& defs() const { return defs_; }
    std::size_t size() const { return defs_.size(); }

private:
    std::vector<InfluxZone> defs_;
    std::map<std::string, std::uint32_t> by_name_;
};

}  // namespace simlab
