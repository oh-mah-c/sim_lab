#include "simlab/Deposits.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
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

bool is_condition_field(const std::string& n) {
    static const std::set<std::string> kFields = {
        "elevation", "moisture", "temperature", "slope", "near_water",
        "lithology", "fault", "vegetation", "water_table", "placer_site"};
    return kFields.count(n) != 0;
}

bool DepositRegistry::load_file(const std::string& path,
                                const PropertyRegistry& props,
                                const MaterialRegistry& materials,
                                std::string* err) {
    std::ifstream f(path);
    if (!f) { if (err) *err = "cannot open " + path; return false; }
    std::stringstream ss; ss << f.rdbuf();
    if (!parse(ss.str(), props, materials, err)) return false;
    for (DepositDef& d : defs_) d.provenance = path;
    return true;
}

bool DepositRegistry::parse(const std::string& text,
                            const PropertyRegistry& props,
                            const MaterialRegistry& materials,
                            std::string* err) {
    DepositRegistry tmp;
    std::istringstream in(text);
    std::string line;
    int lineno = 0;
    std::size_t cur_idx = static_cast<std::size_t>(-1);
    std::int64_t share_sum = 0;
    auto bad = [&](const std::string& m) {
        if (err) *err = "deposits: " + m + " line " + std::to_string(lineno);
        return false;
    };
    auto finish_block = [&]() -> bool {
        if (cur_idx == static_cast<std::size_t>(-1)) return true;
        DepositDef& d = tmp.defs_[cur_idx];
        if (d.surface && d.depth != 0) return bad("surface deposit '" + d.name + "' must have depth 0");
        if (!d.surface && d.depth <= 0) return bad("buried deposit '" + d.name + "' needs depth > 0");
        if (d.composition.empty()) return bad("deposit '" + d.name + "' has no substances");
        if (share_sum != 1000) return bad("deposit '" + d.name + "' shares sum to " +
                                          std::to_string(share_sum) + ", expected 1000");
        std::sort(d.composition.begin(), d.composition.end(),
                  [](const DepositComponent& a, const DepositComponent& b) {
                      return a.material < b.material;
                  });
        return true;
    };
    while (std::getline(in, line)) {
        ++lineno;
        const std::size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        line = trim(line);
        if (line.empty()) continue;
        if (line.rfind("deposit ", 0) == 0) {
            if (!finish_block()) return false;
            const std::string name = trim(line.substr(8));
            if (name.empty() || name.find(' ') != std::string::npos)
                return bad("bad deposit name");
            DepositDef d; d.name = name; d.provenance = "inline";
            if (tmp.add(d) == kNoDeposit) return bad("duplicate deposit '" + name + "'");
            cur_idx = tmp.defs_.size() - 1;
            share_sum = 0;
            continue;
        }
        if (cur_idx == static_cast<std::size_t>(-1)) return bad("content before any 'deposit' line");
        DepositDef& d = tmp.defs_[cur_idx];
        if (line.rfind("substance ", 0) == 0) {
            std::istringstream ls(line.substr(9));
            std::string mname; std::int64_t share = 0;
            if (!(ls >> mname >> share) || share <= 0) return bad("bad substance line");
            const MaterialDef* m = materials.find_name(mname);
            if (!m) return bad("unknown substance material '" + mname + "'");
            if (share_sum + share > 1000) return bad("composition shares exceed 1000");
            share_sum += share;
            d.composition.push_back({m->id, share});
            continue;
        }
        if (line == "accessibility surface") { d.surface = true; continue; }
        if (line == "accessibility buried")  { d.surface = false; continue; }
        if (line.rfind("depth ", 0) == 0) {
            try { d.depth = parse_i64(trim(line.substr(6))); }
            catch (...) { return bad("bad depth"); }
            continue;
        }
        if (line.rfind("condition ", 0) == 0) {
            std::istringstream ls(line.substr(10));
            FormationCondition c; std::int64_t lo = 0, hi = 0;
            if (!(ls >> c.field >> lo)) return bad("bad condition line");
            if (!(ls >> hi)) hi = lo;          // single value means exact match
            if (lo > hi) return bad("condition lo > hi");
            if (!is_condition_field(c.field)) return bad("unknown condition field '" + c.field + "'");
            c.lo = lo; c.hi = hi;
            d.conditions.push_back(c);
            continue;
        }
        bool is_evidence = false;
        if (line.rfind("evidence ", 0) == 0) { is_evidence = true; line = trim(line.substr(9)); }
        // otherwise: <prop_name> <value>   (or "evidence <prop_name> <value>")
        std::istringstream ls(line);
        std::string pname, raw, extra;
        ls >> pname >> raw;
        if (!ls || pname.empty() || raw.empty() || (ls >> extra)) return bad("bad line: " + line);
        const PropertyDef* pd = props.find_name(pname);
        if (!pd) return bad("unknown property '" + pname + "'");
        Value v;
        try {
            switch (pd->type) {
                case ValType::Integer: v = Value::integer(parse_i64(raw)); break;
                case ValType::Fixed:   v = Value::fixed(parse_i64(raw)); break;
                case ValType::Boolean:
                    if (raw == "true" || raw == "1") v = Value::boolean(true);
                    else if (raw == "false" || raw == "0") v = Value::boolean(false);
                    else throw std::invalid_argument("bool");
                    break;
                case ValType::Text: v = Value::text(raw); break;
                case ValType::Ref:  v = Value::ref(parse_u64(raw)); break;
            }
        } catch (...) { return bad("bad value '" + raw + "' for " + pname); }
        if (!props.value_ok(*pd, v)) return bad("value out of range for '" + pname + "'");
        if (is_evidence) {
            // evidence = what a passer-by can SEE of a (possibly buried) deposit:
            // must be an observable-domain property, never a truth property.
            if (pd->domain != "observable")
                return bad("evidence property '" + pname + "' must have domain=observable");
            d.evidence.set(pd->id, v);
        } else {
            d.props.set(pd->id, v);
        }
    }
    if (!finish_block()) return false;
    defs_ = std::move(tmp.defs_);
    by_name_ = std::move(tmp.by_name_);
    return true;
}

DepositId DepositRegistry::add(DepositDef def) {
    if (def.name.empty() || by_name_.count(def.name)) return kNoDeposit;
    def.id = static_cast<DepositId>(defs_.size() + 1);
    by_name_[def.name] = def.id;
    defs_.push_back(std::move(def));
    return defs_.back().id;
}
const DepositDef* DepositRegistry::find(DepositId id) const {
    if (id == 0 || id > defs_.size()) return nullptr;
    return &defs_[id - 1];
}
const DepositDef* DepositRegistry::find_name(const std::string& name) const {
    const auto it = by_name_.find(name);
    return it == by_name_.end() ? nullptr : find(it->second);
}
DepositId DepositRegistry::id_of(const std::string& name) const {
    const DepositDef* d = find_name(name);
    return d ? d->id : kNoDeposit;
}

}  // namespace simlab
