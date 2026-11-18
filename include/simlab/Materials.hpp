#pragma once
#include <map>
#include <string>
#include <vector>
#include "Properties.hpp"
#include "Types.hpp"

namespace simlab {

// ---- Material registry ------------------------------------------------------
// Materials are DATA. A material is a named bundle of property values; adding a
// material must never require engine code. Raw payloads in files are integers:
// integer->i, fixed->milli-units, boolean->0/1, text->string, ref->id.
struct MaterialDef {
    MaterialId id = kNoMaterial;
    std::string name;
    std::map<PropId, Value> props;        // default property values
    std::string provenance;               // e.g. "data/materials.txt"
};

class MaterialRegistry {
public:
    // File format:
    //   material <name>
    //     <prop_name> <value> [<value>...]
    // Value encoding depends on the property's declared ValType.
    bool load_file(const std::string& path,
                   const PropertyRegistry& props,
                   std::string* err = nullptr);
    bool parse(const std::string& text,
               const PropertyRegistry& props,
               std::string* err = nullptr);

    MaterialId add(MaterialDef def);      // kNoMaterial on duplicate/invalid
    const MaterialDef* find(MaterialId id) const;
    const MaterialDef* find_name(const std::string& name) const;
    MaterialId id_of(const std::string& name) const;
    std::size_t size() const { return defs_.size() + 1; }
    const std::vector<MaterialDef>& defs() const { return defs_; }

private:
    std::vector<MaterialDef> defs_;
    std::map<std::string, MaterialId> by_name_;
};

}  // namespace simlab
