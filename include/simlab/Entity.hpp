#pragma once
#include <map>
#include <string>
#include <string_view>
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

// ---- Interned kind names ------------------------------------------------------
// Kind labels repeat across thousands of entities and clones; storing a
// std::string per entity cost 32 bytes each and made every `kind == "x"`
// test a strcmp. KindName holds a pointer into a process-wide, append-only
// intern pool (node-based set: stable addresses), so equality between kinds
// is a pointer compare and copying an entity copies 8 bytes. Text semantics
// are unchanged: it converts to `const std::string&` everywhere a string is
// expected (hashing, save files, printing). Single-threaded engine: the pool
// must be guarded before simulation runs on multiple threads.
const std::string* intern_kind(std::string_view s);

class KindName {
public:
    KindName() : p_(intern_kind("")) {}
    KindName(const std::string& s) : p_(intern_kind(s)) {}
    KindName(const char* s) : p_(intern_kind(s)) {}
    KindName& operator=(const std::string& s) { p_ = intern_kind(s); return *this; }
    KindName& operator=(const char* s) { p_ = intern_kind(s); return *this; }
    operator const std::string&() const { return *p_; }
    const std::string& str() const { return *p_; }
    const char* c_str() const { return p_->c_str(); }
    bool empty() const { return p_->empty(); }
    std::size_t size() const { return p_->size(); }
    std::size_t find(const char* s, std::size_t pos = 0) const { return p_->find(s, pos); }
    std::size_t find(const std::string& s, std::size_t pos = 0) const { return p_->find(s, pos); }
    std::size_t rfind(const char* s, std::size_t pos = std::string::npos) const { return p_->rfind(s, pos); }
    std::string substr(std::size_t pos, std::size_t n = std::string::npos) const { return p_->substr(pos, n); }
    const std::string* interned() const { return p_; }

    friend bool operator==(const KindName& a, const KindName& b) { return a.p_ == b.p_; }
    friend bool operator!=(const KindName& a, const KindName& b) { return a.p_ != b.p_; }
    friend bool operator==(const KindName& a, const std::string& b) { return *a.p_ == b; }
    friend bool operator!=(const KindName& a, const std::string& b) { return *a.p_ != b; }
    friend bool operator==(const std::string& b, const KindName& a) { return *a.p_ == b; }
    friend bool operator!=(const std::string& b, const KindName& a) { return *a.p_ != b; }
    // literal compare: resolved through a per-call-site cache of the
    // interned pointer (see SIMLAB_KIND) when written as kind_is(); the
    // plain operator stays correct for any C string.
    friend bool operator==(const KindName& a, const char* b) { return *a.p_ == b; }
    friend bool operator!=(const KindName& a, const char* b) { return *a.p_ != b; }
    friend bool operator<(const KindName& a, const KindName& b) { return *a.p_ < *b.p_; }
    friend std::string operator+(const KindName& a, const std::string& b) { return *a.p_ + b; }
    friend std::string operator+(const std::string& a, const KindName& b) { return a + *b.p_; }
    friend std::string operator+(const KindName& a, const char* b) { return *a.p_ + b; }
    friend std::string operator+(const char* a, const KindName& b) { return a + *b.p_; }
private:
    const std::string* p_;
};

// Interned pointer of a LITERAL kind name, cached per call site:
// `e.kind_name == SIMLAB_KIND("enclosure")` is a single pointer compare.
#define SIMLAB_KIND(lit) \
    ([]() -> ::simlab::KindName { \
        static const ::simlab::KindName k_(lit); return k_; }())

// ---- Entity & ontology ------------------------------------------------------
// Entities are composition, not classes: identity + material + position +
// property overrides + lifecycle flag. Adding a new KIND of thing never
// requires a new C++ type — kind is an open registry id.
struct Entity {
    EntityId id = kNoEntity;
    std::uint32_t kind = 0;               // open kind id (0 = unclassified)
    KindName kind_name;                   // interned label (8 bytes), never load-bearing
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
