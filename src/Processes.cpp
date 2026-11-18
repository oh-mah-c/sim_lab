#include "simlab/Processes.hpp"
#include <fstream>
#include <sstream>

namespace simlab {

namespace {

bool parse_io(const std::string& rest, const MaterialRegistry& mats,
              ProcessIO& io, std::string* err) {
    std::istringstream ss(rest);
    std::string name; std::int64_t qty = 0; std::string extra;
    if (!(ss >> name >> qty) || qty <= 0 || (ss >> extra)) {
        if (err) *err = "process io: expected '<material> <qty>'";
        return false;
    }
    io.material_name = name;
    io.material = mats.id_of(name);
    if (io.material == kNoMaterial) {
        if (err) *err = "process io: unknown material '" + name + "'";
        return false;
    }
    io.qty = qty;
    return true;
}

}  // namespace

bool ProcessRegistry::load_file(const std::string& path,
                                const MaterialRegistry& mats,
                                std::string* err) {
    std::ifstream f(path);
    if (!f) { if (err) *err = "cannot open " + path; return false; }
    std::ostringstream ss; ss << f.rdbuf();
    if (!parse(ss.str(), mats, err)) return false;
    for (ProcessDef& d : defs_) d.provenance = path;
    return true;
}

bool ProcessRegistry::parse(const std::string& text,
                            const MaterialRegistry& mats,
                            std::string* err) {
    // all-or-nothing: parse into a temporary registry, commit on success
    ProcessRegistry tmp;
    std::istringstream in(text);
    std::string line;
    ProcessDef* cur = nullptr;
    int lineno = 0;
    auto bad = [&](const std::string& m) {
        if (err) *err = "process: " + m + " (line " + std::to_string(lineno) + ")";
        return false;
    };
    while (std::getline(in, line)) {
        ++lineno;
        const std::size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        std::istringstream ls(line);
        std::string kw; ls >> kw;
        if (kw.empty()) continue;
        if (kw == "process") {
            std::string name; ls >> name;
            if (name.empty() || tmp.by_name_.count(name)) return bad("missing/duplicate name '" + name + "'");
            ProcessDef d; d.id = static_cast<std::uint32_t>(tmp.defs_.size() + 1);
            d.name = name; d.provenance = "inline";
            tmp.defs_.push_back(std::move(d));
            tmp.by_name_[name] = tmp.defs_.back().id;
            cur = &tmp.defs_.back();
            continue;
        }
        if (!cur) return bad("data before first 'process'");
        std::string rest; std::getline(ls, rest);
        while (!rest.empty() && rest.front() == ' ') rest.erase(rest.begin());
        if (kw == "input" || kw == "fuel" || kw == "output" || kw == "waste") {
            ProcessIO io;
            if (!parse_io(rest, mats, io, err)) return bad(err && !err->empty() ? *err : "bad io");
            if (kw == "input") cur->inputs.push_back(io);
            else if (kw == "fuel") cur->fuels.push_back(io);
            else if (kw == "output") cur->outputs.push_back(io);
            else cur->wastes.push_back(io);
        } else if (kw == "min_temperature" || kw == "ticks") {
            std::int64_t v = 0;
            try { v = parse_i64(rest); } catch (...) { return bad("bad number for " + kw + ": '" + rest + "'"); }
            if (kw == "ticks") {
                if (v < 1) return bad("ticks must be >= 1");
                cur->ticks = v;
            } else {
                if (v < 0 || v > 20000) return bad("min_temperature out of 0..20000 deci-C");
                cur->min_temperature = v;
            }
        } else {
            return bad("unknown directive '" + kw + "'");
        }
    }
    for (const ProcessDef& d : tmp.defs_)
        if (d.inputs.empty() && d.outputs.empty()) {
            if (err) *err = "process '" + d.name + "' has no inputs/outputs";
            return false;
        }
    defs_ = std::move(tmp.defs_);
    by_name_ = std::move(tmp.by_name_);
    return true;
}

const ProcessDef* ProcessRegistry::find(const std::string& name) const {
    const auto it = by_name_.find(name);
    if (it == by_name_.end()) return nullptr;
    return &defs_[it->second - 1];
}

}  // namespace simlab
