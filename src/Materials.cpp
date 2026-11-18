#include "simlab/Materials.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace simlab {
namespace {
std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}
}

bool MaterialRegistry::load_file(const std::string& path,
                                 const PropertyRegistry& props,
                                 std::string* err) {
    std::ifstream f(path);
    if (!f) { if (err) *err = "cannot open " + path; return false; }
    std::stringstream ss; ss << f.rdbuf();
    if (!parse(ss.str(), props, err)) return false;
    for (MaterialDef& d : defs_) d.provenance = path;
    return true;
}

bool MaterialRegistry::parse(const std::string& text,
                             const PropertyRegistry& props,
                             std::string* err) {
    MaterialRegistry tmp;
    std::istringstream in(text);
    std::string line;
    int lineno = 0;
    std::size_t cur_idx = static_cast<std::size_t>(-1);   // index into tmp.defs_
    while (std::getline(in, line)) {
        ++lineno;
        const std::size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        line = trim(line);
        if (line.empty()) continue;
        if (line.rfind("material ", 0) == 0) {
            const std::string name = trim(line.substr(9));
            if (name.empty() || name.find(' ') != std::string::npos) {
                if (err) *err = "materials: bad material name line " + std::to_string(lineno);
                return false;
            }
            MaterialDef d; d.name = name; d.provenance = "inline";
            if (tmp.add(d) == kNoMaterial) {
                if (err) *err = "materials: duplicate material '" + name + "' line " + std::to_string(lineno);
                return false;
            }
            cur_idx = tmp.defs_.size() - 1;
            continue;
        }
        if (cur_idx == static_cast<std::size_t>(-1)) {
            if (err) *err = "materials: property before any 'material' line " + std::to_string(lineno);
            return false;
        }
        std::istringstream ls(line);
        std::string pname, raw;
        ls >> pname >> raw;
        std::string extra;
        if (!ls || pname.empty() || (ls >> extra)) {
            if (err) *err = "materials: bad property line " + std::to_string(lineno) + ": " + line;
            return false;
        }
        const PropertyDef* pd = props.find_name(pname);
        if (!pd) {
            if (err) *err = "materials: unknown property '" + pname + "' line " + std::to_string(lineno);
            return false;
        }
        Value v;
        try {
            switch (pd->type) {
                case ValType::Integer: v = Value::integer(parse_i64(raw)); break;
                case ValType::Fixed:   v = Value::fixed(parse_i64(raw)); break;
                case ValType::Boolean: {
                    if (raw == "true" || raw == "1") v = Value::boolean(true);
                    else if (raw == "false" || raw == "0") v = Value::boolean(false);
                    else throw std::invalid_argument("bool");
                    break;
                }
                case ValType::Text: v = Value::text(raw); break;
                case ValType::Ref:  v = Value::ref(parse_u64(raw)); break;
            }
        } catch (...) {
            if (err) *err = "materials: bad value '" + raw + "' for " + pname + " line " + std::to_string(lineno);
            return false;
        }
        if (!props.value_ok(*pd, v)) {
            if (err) *err = "materials: value out of range/type for '" + pname + "' line " + std::to_string(lineno);
            return false;
        }
        tmp.defs_[cur_idx].props[pd->id] = v;
    }
    defs_ = std::move(tmp.defs_);
    by_name_ = std::move(tmp.by_name_);
    return true;
}

MaterialId MaterialRegistry::add(MaterialDef def) {
    if (def.name.empty() || by_name_.count(def.name)) return kNoMaterial;
    def.id = static_cast<MaterialId>(defs_.size() + 1);
    by_name_[def.name] = def.id;
    defs_.push_back(std::move(def));
    return defs_.back().id;
}
const MaterialDef* MaterialRegistry::find(MaterialId id) const {
    if (id == 0 || id > defs_.size()) return nullptr;
    return &defs_[id - 1];
}
const MaterialDef* MaterialRegistry::find_name(const std::string& name) const {
    const auto it = by_name_.find(name);
    return it == by_name_.end() ? nullptr : find(it->second);
}
MaterialId MaterialRegistry::id_of(const std::string& name) const {
    const MaterialDef* d = find_name(name);
    return d ? d->id : kNoMaterial;
}

}  // namespace simlab
