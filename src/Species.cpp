#include "simlab/Species.hpp"
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

bool SpeciesRegistry::load_file(const std::string& path, const PropertyRegistry& props,
                                const MaterialRegistry& materials, std::string* err) {
    std::ifstream f(path);
    if (!f) { if (err) *err = "cannot open " + path; return false; }
    std::stringstream ss; ss << f.rdbuf();
    if (!parse(ss.str(), props, materials, err)) return false;
    for (SpeciesDef& d : defs_) d.provenance = path;
    return true;
}

bool SpeciesRegistry::parse(const std::string& text, const PropertyRegistry& props,
                            const MaterialRegistry& materials, std::string* err) {
    SpeciesRegistry tmp;
    std::istringstream in(text);
    std::string line;
    int lineno = 0;
    std::size_t cur = static_cast<std::size_t>(-1);
    auto bad = [&](const std::string& m) {
        if (err) *err = "species: " + m + " line " + std::to_string(lineno);
        return false;
    };
    auto finish = [&]() -> bool {
        if (cur == static_cast<std::size_t>(-1)) return true;
        const SpeciesDef& d = tmp.defs_[cur];
        if (d.material == kNoMaterial) return bad("species '" + d.name + "' has no material");
        if (d.density <= 0 || d.density > 1000) return bad("species '" + d.name + "' needs density 1..1000");
        return true;
    };
    while (std::getline(in, line)) {
        ++lineno;
        const std::size_t h = line.find('#');
        if (h != std::string::npos) line.erase(h);
        line = trim(line);
        if (line.empty()) continue;
        if (line.rfind("species ", 0) == 0) {
            if (!finish()) return false;
            SpeciesDef d; d.name = trim(line.substr(8)); d.provenance = "inline";
            if (d.name.empty() || d.name.find(' ') != std::string::npos) return bad("bad species name");
            if (tmp.add(d) == 0) return bad("duplicate species '" + d.name + "'");
            cur = tmp.defs_.size() - 1;
            continue;
        }
        if (cur == static_cast<std::size_t>(-1)) return bad("content before any 'species' line");
        SpeciesDef& d = tmp.defs_[cur];
        std::istringstream ls(line);
        std::string key; ls >> key;
        try {
            if (key == "kind") {
                std::string k; ls >> k;
                if (k == "flora") d.fauna = false;
                else if (k == "fauna") d.fauna = true;
                else return bad("kind must be flora|fauna");
            } else if (key == "material") {
                std::string m; ls >> m;
                const MaterialDef* md = materials.find_name(m);
                if (!md) return bad("unknown material '" + m + "'");
                d.material = md->id;
            } else if (key == "density") {
                std::string v; ls >> v; d.density = parse_i64(v);
            } else if (key == "condition") {
                FormationCondition c; std::string lo, hi;
                ls >> c.field >> lo >> hi;
                if (!is_condition_field(c.field) || c.field == "lithology")
                    return bad("unknown habitat field '" + c.field + "'");
                c.lo = parse_i64(lo); c.hi = parse_i64(hi);
                if (c.lo > c.hi) return bad("condition lo > hi");
                d.conditions.push_back(c);
            } else if (key == "body_part") {
                // anatomy (Phase 10): `body_part <name> <material> <slot>
                // <role> <vital> <size>` — child entities spawned contained
                // under the body via part_of edges.
                PartSpec ps; std::string mat, vital, size;
                ls >> ps.name >> mat >> ps.slot >> ps.role >> vital >> size;
                const MaterialDef* md = materials.find_name(mat);
                if (ps.name.empty() || !md) return bad("bad body_part line");
                ps.material = md->id;
                ps.vital = parse_i64(vital);
                ps.size = size.empty() ? 200 : parse_i64(size);
                // optional trailing key=value organ props — lumen_capacity,
                // contractility, evacuate_material, filtration_rate, ...
                // Any registered property is allowed; text props take the
                // raw token, integer props parse it.
                std::string kv;
                while (ls >> kv) {
                    const auto eq = kv.find('=');
                    if (eq == std::string::npos)
                        return bad("bad body_part extra '" + kv + "'");
                    const std::string k = kv.substr(0, eq);
                    const std::string v = kv.substr(eq + 1);
                    const PropertyDef* pd = props.find_name(k);
                    if (!pd) return bad("unknown body_part prop '" + k + "'");
                    if (pd->type == ValType::Text)
                        ps.extra.set(pd->id, Value::text(v));
                    else
                        ps.extra.set(pd->id, Value::integer(parse_i64(v)));
                }
                d.anatomy.push_back(ps);
            } else if (key == "gene") {
                // hereditary locus (Phase 7): `gene <prop>` declares which
                // props pass to offspring. PropId only — no trait semantics.
                std::string g; ls >> g;
                const PropertyDef* gd = props.find_name(g);
                if (!gd) return bad("unknown gene property '" + g + "'");
                if (gd->type != ValType::Integer && gd->type != ValType::Fixed)
                    return bad("gene property '" + g + "' must be numeric");
                d.genes.push_back(gd->id);
            } else {
                std::string raw, extra; ls >> raw;
                const PropertyDef* pd = props.find_name(key);
                if (!pd) return bad("unknown key/property '" + key + "'");
                if (raw.empty() || (ls >> extra)) return bad("bad line: " + line);
                Value v;
                switch (pd->type) {
                    case ValType::Integer: v = Value::integer(parse_i64(raw)); break;
                    case ValType::Fixed:   v = Value::fixed(parse_i64(raw)); break;
                    case ValType::Boolean: v = Value::boolean(raw == "1" || raw == "true"); break;
                    case ValType::Text:    v = Value::text(raw); break;
                    case ValType::Ref:     v = Value::ref(parse_u64(raw)); break;
                }
                if (!props.value_ok(*pd, v)) return bad("value out of range for '" + key + "'");
                d.props.set(pd->id, v);
            }
        } catch (...) { return bad("bad number in: " + line); }
    }
    if (!finish()) return false;
    defs_ = std::move(tmp.defs_);
    by_name_ = std::move(tmp.by_name_);
    return true;
}

std::uint32_t SpeciesRegistry::add(SpeciesDef def) {
    if (def.name.empty() || by_name_.count(def.name)) return 0;
    def.id = static_cast<std::uint32_t>(defs_.size() + 1);
    by_name_[def.name] = def.id;
    defs_.push_back(std::move(def));
    return defs_.back().id;
}
const SpeciesDef* SpeciesRegistry::find(std::uint32_t id) const {
    if (id == 0 || id > defs_.size()) return nullptr;
    return &defs_[id - 1];
}
const SpeciesDef* SpeciesRegistry::find_name(const std::string& name) const {
    const auto it = by_name_.find(name);
    return it == by_name_.end() ? nullptr : find(it->second);
}

}  // namespace simlab
