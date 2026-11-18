#include "simlab/Properties.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace simlab {

const Value* PropertyBag::get(PropId p) const {
    auto it = std::lower_bound(entries.begin(), entries.end(), p,
        [](const std::pair<PropId, Value>& a, PropId k) { return a.first < k; });
    return (it != entries.end() && it->first == p) ? &it->second : nullptr;
}
void PropertyBag::set(PropId p, Value v) {
    auto it = std::lower_bound(entries.begin(), entries.end(), p,
        [](const std::pair<PropId, Value>& a, PropId k) { return a.first < k; });
    if (it != entries.end() && it->first == p) it->second = std::move(v);
    else entries.insert(it, {p, std::move(v)});
}
bool PropertyBag::erase(PropId p) {
    auto it = std::lower_bound(entries.begin(), entries.end(), p,
        [](const std::pair<PropId, Value>& a, PropId k) { return a.first < k; });
    if (it == entries.end() || it->first != p) return false;
    entries.erase(it);
    return true;
}

bool PropertyRegistry::load_file(const std::string& path, std::string* err) {
    std::ifstream f(path);
    if (!f) { if (err) *err = "cannot open " + path; return false; }
    std::stringstream ss; ss << f.rdbuf();
    if (!parse(ss.str(), err)) return false;
    for (PropertyDef& d : defs_) d.provenance = path;
    return true;
}

namespace {
std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}
ValType parse_type(const std::string& t, bool& ok) {
    ok = true;
    if (t == "integer") return ValType::Integer;
    if (t == "fixed") return ValType::Fixed;
    if (t == "boolean") return ValType::Boolean;
    if (t == "text") return ValType::Text;
    if (t == "ref") return ValType::Ref;
    ok = false; return ValType::Integer;
}
}

bool PropertyRegistry::parse(const std::string& text, std::string* err) {
    PropertyRegistry tmp;
    std::istringstream in(text);
    std::string line;
    int lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        const std::size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        line = trim(line);
        if (line.empty()) continue;
        std::istringstream ls(line);
        std::string name, tname, unit;
        ls >> name >> tname >> unit;
        bool ok = false;
        const ValType vt = parse_type(tname, ok);
        if (!ls || name.empty() || !ok || unit.empty()) {
            if (err) *err = "properties: bad line " + std::to_string(lineno) + ": " + line;
            return false;
        }
        PropertyDef d; d.name = name; d.type = vt; d.unit = unit; d.provenance = "inline";
        std::string tok;
        while (ls >> tok) {
            if (tok.rfind("domain=", 0) == 0) { d.domain = tok.substr(7); continue; }
            if (tok.rfind("depends=", 0) == 0) { d.depends = tok.substr(8); continue; }
            if (tok.rfind("applies=", 0) == 0) { d.applies = tok.substr(8); continue; }
            if (tok.rfind("uncertainty=", 0) == 0) {
                try { d.uncertainty = parse_i64(tok.substr(12)); }
                catch (...) { if (err) *err = "properties: bad uncertainty line " + std::to_string(lineno); return false; }
                if (d.uncertainty < 0 || d.uncertainty > 1000) { if (err) *err = "properties: uncertainty out of 0..1000 line " + std::to_string(lineno); return false; }
                continue;
            }
            if (tok == "range") {
                std::string lo, hi;
                if (!(ls >> lo >> hi)) { if (err) *err = "properties: bad range line " + std::to_string(lineno); return false; }
                try {
                    d.lo = parse_i64(lo); d.hi = parse_i64(hi);
                    if (d.lo > d.hi) throw std::invalid_argument("lo>hi");
                    d.has_range = true;
                } catch (...) { if (err) *err = "properties: bad range numbers line " + std::to_string(lineno); return false; }
                continue;
            }
            if (err) *err = "properties: unknown token '" + tok + "' line " + std::to_string(lineno);
            return false;
        }
        if (vt != ValType::Integer && vt != ValType::Fixed && d.has_range) {
            if (err) *err = "properties: range only valid for integer/fixed (line " + std::to_string(lineno) + ")";
            return false;
        }
        if (tmp.add(d) == kNoProp) {
            if (err) *err = "properties: duplicate or invalid '" + name + "' line " + std::to_string(lineno);
            return false;
        }
    }
    // cross-reference check: `depends` must name a declared property
    for (const PropertyDef& d : tmp.defs_)
        if (!d.depends.empty() && !tmp.find_name(d.depends)) {
            if (err) *err = "properties: '" + d.name + "' depends on unknown property '" + d.depends + "'";
            return false;
        }
    defs_ = std::move(tmp.defs_);
    by_name_ = std::move(tmp.by_name_);
    return true;
}

PropId PropertyRegistry::add(PropertyDef def) {
    if (def.name.empty()) return kNoProp;   // unit may be empty = dimensionless
    if (by_name_.find(def.name) != by_name_.end()) return kNoProp;
    def.id = static_cast<PropId>(defs_.size() + 1);
    by_name_[def.name] = def.id;
    defs_.push_back(std::move(def));
    return defs_.back().id;
}

const PropertyDef* PropertyRegistry::find(PropId id) const {
    if (id == 0 || id > defs_.size()) return nullptr;
    return &defs_[id - 1];
}
const PropertyDef* PropertyRegistry::find_name(std::string_view name) const {
    const auto it = by_name_.find(name);
    return it == by_name_.end() ? nullptr : find(it->second);
}

bool PropertyRegistry::value_ok(const PropertyDef& def, const Value& v) const {
    if (v.type != def.type) return false;
    if (def.has_range && (def.type == ValType::Integer || def.type == ValType::Fixed))
        if (v.i < def.lo || v.i > def.hi) return false;
    return true;
}

}  // namespace simlab

namespace simlab {
std::vector<std::string> PropertyRegistry::unmet_dependencies(
    const std::vector<PropId>& present) const {
    std::vector<std::string> out;
    for (PropId p : present) {
        const PropertyDef* d = find(p);
        if (!d || d->depends.empty()) continue;
        const PropertyDef* dep = find_name(d->depends);
        if (!dep || std::find(present.begin(), present.end(), dep->id) == present.end())
            out.push_back(d->name);
    }
    return out;
}
}  // namespace simlab
