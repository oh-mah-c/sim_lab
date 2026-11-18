#include "simlab/Influx.hpp"
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

bool InfluxRegistry::load_file(const std::string& path, const PropertyRegistry& props,
                               const MaterialRegistry& materials, std::string* err) {
    std::ifstream f(path);
    if (!f) { if (err) *err = "cannot open " + path; return false; }
    std::stringstream ss; ss << f.rdbuf();
    if (!parse(ss.str(), props, materials, err)) return false;
    for (InfluxZone& z : defs_) z.provenance = path;
    return true;
}

bool InfluxRegistry::parse(const std::string& text, const PropertyRegistry& props,
                           const MaterialRegistry& materials, std::string* err) {
    InfluxRegistry tmp;
    std::istringstream in(text);
    std::string line;
    int lineno = 0;
    std::size_t cur = static_cast<std::size_t>(-1);      // zone index
    std::size_t mt  = static_cast<std::size_t>(-1);      // migrant template index
    auto bad = [&](const std::string& m) {
        if (err) *err = "influx: " + m + " line " + std::to_string(lineno);
        return false;
    };
    auto finish_zone = [&]() -> bool {
        if (cur == static_cast<std::size_t>(-1)) return true;
        const InfluxZone& z = tmp.defs_[cur];
        if (z.interval <= 0) return bad("zone '" + z.name + "' needs interval > 0");
        if (z.migrants <= 0) return bad("zone '" + z.name + "' needs migrants > 0");
        if (z.templates.empty()) return bad("zone '" + z.name + "' has no migrant template");
        for (const InfluxMigrant& t : z.templates)
            if (t.kind.empty()) return bad("zone '" + z.name + "' migrant missing kind");
        return true;
    };
    auto finish_migrant = [&]() -> bool {
        if (mt == static_cast<std::size_t>(-1) || cur == static_cast<std::size_t>(-1))
            return true;
        const InfluxMigrant& t = tmp.defs_[cur].templates[mt];
        if (t.name.empty()) return bad("migrant template missing name");
        return true;
    };
    while (std::getline(in, line)) {
        ++lineno;
        const std::size_t h = line.find('#');
        if (h != std::string::npos) line.erase(h);
        line = trim(line);
        if (line.empty()) continue;
        std::istringstream ls(line);
        std::string key; ls >> key;
        try {
            if (key == "influx_zone") {
                if (!finish_migrant() || !finish_zone()) return false;
                std::string name; ls >> name;
                if (name.empty() || name.find(' ') != std::string::npos)
                    return bad("bad zone name");
                for (const InfluxZone& z : tmp.defs_)
                    if (z.name == name) return bad("duplicate zone '" + name + "'");
                InfluxZone z; z.name = name; z.id = static_cast<std::uint32_t>(tmp.defs_.size() + 1);
                tmp.defs_.push_back(std::move(z));
                cur = tmp.defs_.size() - 1; mt = static_cast<std::size_t>(-1);
                continue;
            }
            if (key == "migrant") {
                if (cur == static_cast<std::size_t>(-1)) return bad("migrant before any zone");
                if (!finish_migrant()) return false;
                std::string name; ls >> name;
                if (name.empty()) return bad("bad migrant name");
                InfluxMigrant t; t.name = name;
                tmp.defs_[cur].templates.push_back(std::move(t));
                mt = tmp.defs_[cur].templates.size() - 1;
                continue;
            }
            if (cur == static_cast<std::size_t>(-1)) return bad("content before any 'influx_zone'");
            InfluxZone& z = tmp.defs_[cur];
            // zone-level keys stay valid even after a migrant block opened —
            // file order inside a zone is free.
            if (key == "bounds") {
                std::string a, b, c, d, extra;
                if (!(ls >> a >> b >> c >> d) || (ls >> extra))
                    return bad("bounds needs 4 coords");
                const std::int64_t v0 = parse_i64(a), v1 = parse_i64(b),
                                  v2 = parse_i64(c), v3 = parse_i64(d);
                if (v0 < INT32_MIN || v0 > INT32_MAX || v1 < INT32_MIN || v1 > INT32_MAX ||
                    v2 < INT32_MIN || v2 > INT32_MAX || v3 < INT32_MIN || v3 > INT32_MAX)
                    return bad("bounds coord out of int32 range");
                z.x0 = static_cast<std::int32_t>(v0); z.y0 = static_cast<std::int32_t>(v1);
                z.x1 = static_cast<std::int32_t>(v2); z.y1 = static_cast<std::int32_t>(v3);
                if (z.x0 > z.x1 || z.y0 > z.y1) return bad("bounds lo > hi");
            } else if (key == "interval") {
                std::string v; ls >> v; z.interval = parse_i64(v);
            } else if (key == "capacity") {
                std::string v; ls >> v; z.capacity = parse_i64(v);
            } else if (key == "migrants") {
                std::string v; ls >> v; z.migrants = parse_i64(v);
            } else if (mt != static_cast<std::size_t>(-1)) {
                InfluxMigrant& t = z.templates[mt];
                if (key == "kind") {
                    std::string k; ls >> k;
                    if (k.empty()) return bad("empty kind");
                    t.kind = k;
                } else if (key == "material") {
                    std::string m; ls >> m;
                    const MaterialDef* md = materials.find_name(m);
                    if (!md) return bad("unknown material '" + m + "'");
                    t.material = md->id;
                } else if (key == "prop") {
                    std::string name, raw, extra; ls >> name >> raw;
                    const PropertyDef* pd = props.find_name(name);
                    if (!pd || raw.empty() || (ls >> extra))
                        return bad("bad prop line: " + line);
                    Value v;
                    switch (pd->type) {
                        case ValType::Integer: v = Value::integer(parse_i64(raw)); break;
                        case ValType::Fixed:   v = Value::fixed(parse_i64(raw)); break;
                        case ValType::Boolean: v = Value::boolean(raw == "1" || raw == "true"); break;
                        case ValType::Text:    v = Value::text(raw); break;
                        case ValType::Ref:     v = Value::ref(parse_u64(raw)); break;
                    }
                    if (!props.value_ok(*pd, v)) return bad("value out of range for '" + name + "'");
                    t.props.set(pd->id, v);
                } else if (key == "gene") {
                    std::string name, raw, extra; ls >> name >> raw;
                    const PropertyDef* pd = props.find_name(name);
                    if (!pd || raw.empty() || (ls >> extra) ||
                        (pd->type != ValType::Integer && pd->type != ValType::Fixed))
                        return bad("bad gene line: " + line);
                    t.genes.push_back({pd->id, parse_i64(raw)});
                } else if (key == "innate") {
                    std::string v; ls >> v;
                    if (v.empty()) return bad("empty innate verb");
                    t.innate.push_back(v);
                } else {
                    return bad("unknown migrant key '" + key + "'");
                }
            } else {
                return bad("zone key '" + key + "' outside migrant block");
            }
        } catch (...) { return bad("bad number in: " + line); }
    }
    if (!finish_migrant() || !finish_zone()) return false;
    for (InfluxZone& z : tmp.defs_) {
        tmp.by_name_[z.name] = z.id;
    }
    defs_ = std::move(tmp.defs_);
    by_name_ = std::move(tmp.by_name_);
    return true;
}

const InfluxZone* InfluxRegistry::find(std::uint32_t id) const {
    for (const InfluxZone& z : defs_)
        if (z.id == id) return &z;
    return nullptr;
}
const InfluxZone* InfluxRegistry::find_name(const std::string& name) const {
    const auto it = by_name_.find(name);
    return it == by_name_.end() ? nullptr : find(it->second);
}

}  // namespace simlab
