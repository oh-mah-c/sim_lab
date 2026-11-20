#pragma once
#include <cstdint>
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

    // Identity of this registry's CONTENT: a process-unique number that
    // changes on every mutation (add/parse/load) and on every copy. Name->id
    // caches (LitPidCache) key on it, so a cached PropId can never outlive
    // the registry state it was resolved against. Never reused.
    std::uint64_t uid() const { return uid_; }

    PropertyRegistry() : uid_(next_uid()) {}
    PropertyRegistry(const PropertyRegistry& o)
        : defs_(o.defs_), by_name_(o.by_name_), uid_(next_uid()) {}
    PropertyRegistry(PropertyRegistry&& o) noexcept
        : defs_(std::move(o.defs_)), by_name_(std::move(o.by_name_)),
          uid_(next_uid()) { o.uid_ = next_uid(); }
    PropertyRegistry& operator=(const PropertyRegistry& o) {
        if (this != &o) { defs_ = o.defs_; by_name_ = o.by_name_; uid_ = next_uid(); }
        return *this;
    }
    PropertyRegistry& operator=(PropertyRegistry&& o) noexcept {
        if (this != &o) {
            defs_ = std::move(o.defs_); by_name_ = std::move(o.by_name_);
            uid_ = next_uid(); o.uid_ = next_uid();
        }
        return *this;
    }

private:
    static std::uint64_t next_uid() {
        static std::uint64_t counter = 0;   // single-threaded engine
        return ++counter;
    }
    std::vector<PropertyDef> defs_;       // defs_[i].id == i+1
    struct NameHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view s) const {
            return std::hash<std::string_view>{}(s);
        }
    };
    std::unordered_map<std::string, PropId, NameHash, std::equal_to<>> by_name_;
    std::uint64_t uid_ = 0;
};

// ---- hot-path name -> PropId cache ------------------------------------------
// Law/drift code resolves property ids by NAME (data-driven: no engine enum).
// Hashing the name string on every call dominated profiles (find_name was
// 35% of CPU), so call sites keep a static cache keyed by the LITERAL's
// address. Contract: pass only string literals (static storage, fixed
// content). The cache is invalidated whenever the registry's uid() changes,
// so worlds with different data, clones and post-load add() stay correct.
struct LitPidCache {
    struct PtrHash {
        std::size_t operator()(const char* p) const noexcept {
            const auto v = reinterpret_cast<std::uintptr_t>(p);
            return static_cast<std::size_t>(v ^ (v >> 17));
        }
    };
    struct Slot { PropId id; std::string name; };
    std::uint64_t uid = 0;
    std::unordered_map<const char*, Slot, PtrHash> ids;
    PropId get(const PropertyRegistry& reg, const char* lit) {
        if (uid != reg.uid()) { ids.clear(); uid = reg.uid(); }
        auto it = ids.find(lit);
        // pointer hit + content check: a recycled temporary buffer at the
        // same address with different text can never return a stale id
        if (it != ids.end() && it->second.name == lit) return it->second.id;
        const PropertyDef* d = reg.find_name(lit);
        const PropId id = d ? d->id : kNoProp;
        if (it != ids.end()) it->second = Slot{id, lit};
        else ids.emplace(lit, Slot{id, lit});
        return id;
    }
};

// Resolve a LITERAL property name through a per-call-site static cache:
// one literal per site, so the cache is a single (uid, id) slot.
#define SIMLAB_PID(reg, lit) \
    ([](const ::simlab::PropertyRegistry& r_) -> ::simlab::PropId { \
        static thread_local std::uint64_t u_ = 0; \
        static thread_local ::simlab::PropId id_ = ::simlab::kNoProp; \
        if (u_ != r_.uid()) { \
            const ::simlab::PropertyDef* d_ = r_.find_name(lit); \
            id_ = d_ ? d_->id : ::simlab::kNoProp; u_ = r_.uid(); \
        } \
        return id_; \
    }(reg))

// Sequence cache for functions that resolve MANY literal names in a fixed
// order on every call (per-entity drift passes). The first call records
// the (literal, id) sequence; later calls walk it with one pointer compare
// per lookup. A branch that skips/reorders names falls back to the map
// cache without corrupting the recorded sequence.
struct SeqPidCache {
    std::uint64_t uid = 0;
    std::vector<std::pair<const char*, PropId>> seq;
    LitPidCache fallback;
};
class SeqPid {
public:
    SeqPid(SeqPidCache& c, const PropertyRegistry& r) : c_(c), r_(r) {
        if (c_.uid != r_.uid()) { c_.seq.clear(); c_.uid = r_.uid(); }
    }
    PropId operator()(const char* lit) {
        if (i_ < c_.seq.size() && c_.seq[i_].first == lit)
            return c_.seq[i_++].second;
        const PropId id = c_.fallback.get(r_, lit);
        if (in_order_ && i_ == c_.seq.size()) { c_.seq.emplace_back(lit, id); ++i_; }
        else in_order_ = false;           // diverged: stop extending/advancing
        return id;
    }
private:
    SeqPidCache& c_;
    const PropertyRegistry& r_;
    std::size_t i_ = 0;
    bool in_order_ = true;
};

}  // namespace simlab
