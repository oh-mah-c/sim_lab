#pragma once
#include <map>
#include <string_view>
#include <unordered_map>
#include <string>
#include <vector>
#include "Types.hpp"

namespace simlab {

// ---- Property system --------------------------------------------------------
// A property is a typed, unit-annotated, range-validated slot. `hardness = 0.8`
// means nothing without a unit and a scale; every PropertyDef carries both.
// Domain tags ("physical", "biological", ...) let law models declare which
// property domains they understand.
struct PropertyDef {
    PropId id = kNoProp;
    std::string name;
    ValType type = ValType::Integer;
    std::string unit;                    // "mohs", "kg/m3", "permille", ...
    bool has_range = false;
    std::int64_t lo = 0, hi = 0;         // inclusive, on the raw payload
    std::string domain;                  // solver domain hint, open string
    // Spec §4 metadata (all optional, declared in data):
    std::int64_t uncertainty = 0;        // measurement uncertainty, permille of value (0 = exact)
    std::string depends;                 // prop that must resolve on the entity for this one to apply
    std::string applies;                 // comma list of entity kinds it applies to ("" = any)
    std::string provenance;              // where the definition came from (file path / "inline")
};

// An entity's own property values — sparse overrides on top of material
// defaults. Sorted by PropId for deterministic iteration and hashing.
struct PropertyBag {
    std::vector<std::pair<PropId, Value>> entries;   // sorted by PropId
    const Value* get(PropId p) const;
    void set(PropId p, Value v);                     // sorted insert/replace
    bool erase(PropId p);
};

class PropertyRegistry {
public:
    // Loads definitions from a declarative file. Format (one per line, '#' comments):
    //   name TYPE unit [range LO HI] [domain=TAG] [uncertainty=PERMILLE]
    //        [depends=PROP] [applies=kind1,kind2]
    // TYPE in {integer,fixed,boolean,text,ref}. lo/hi apply to integer/fixed only.
    // Returns false on malformed input; partially loaded state is discarded.
    bool load_file(const std::string& path, std::string* err = nullptr);
    // Same content from a string (used by tests).
    bool parse(const std::string& text, std::string* err = nullptr);

    PropId add(PropertyDef def);          // returns id, or kNoProp if invalid/dup
    const PropertyDef* find(PropId id) const;
    const PropertyDef* find_name(std::string_view name) const;
    bool value_ok(const PropertyDef& def, const Value& v) const;
    // Names of properties whose `depends` is not satisfied by the given
    // property ids (entity overrides + material defaults). Empty = consistent.
    std::vector<std::string> unmet_dependencies(
        const std::vector<PropId>& present) const;

    std::size_t size() const { return defs_.size() + 1; }   // ids start at 1
    const std::vector<PropertyDef>& defs() const { return defs_; }

private:
    std::vector<PropertyDef> defs_;       // defs_[i].id == i+1
    struct NameHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view s) const {
            return std::hash<std::string_view>{}(s);
        }
    };
    std::unordered_map<std::string, PropId, NameHash, std::equal_to<>> by_name_;
};

}  // namespace simlab
