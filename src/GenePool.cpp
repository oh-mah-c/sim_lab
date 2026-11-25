// CL-1 Phase 4 — Persistent Gene Pool implementation.
// See include/simlab/GenePool.hpp for the determinism contract.
#include "simlab/GenePool.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "simlab/Properties.hpp"
#include "simlab/Rng.hpp"

namespace simlab {
namespace {

constexpr std::uint32_t kMagic = 0x31475053;   // "SPG1"
constexpr std::uint32_t kVersion = 1;

std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    h *= 0xBF58476D1CE4E5B9ull;
    return h ^ (h >> 29);
}

void put64(std::string& b, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<char>(v >> (i * 8)));
}
void put32(std::string& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<char>(v >> (i * 8)));
}
void put_bytes(std::string& b, const void* p, std::size_t n) {
    const char* c = static_cast<const char*>(p);
    b.append(c, n);
}
void put_str(std::string& b, const std::string& s) {
    put32(b, static_cast<std::uint32_t>(s.size()));
    b.append(s);
}

struct Reader {
    const char* p;
    const char* end;
    bool ok = true;
    std::uint64_t u64() {
        if (end - p < 8) { ok = false; return 0; }
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= std::uint64_t(static_cast<std::uint8_t>(*p++)) << (i * 8);
        return v;
    }
    std::uint32_t u32() {
        if (end - p < 4) { ok = false; return 0; }
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= std::uint32_t(static_cast<std::uint8_t>(*p++)) << (i * 8);
        return v;
    }
    std::string str() {
        const std::uint32_t n = u32();
        if (!ok || end - p < static_cast<std::ptrdiff_t>(n)) { ok = false; return {}; }
        std::string s(p, n);
        p += n;
        return s;
    }
    bool bytes(void* dst, std::size_t n) {
        if (!ok || end - p < static_cast<std::ptrdiff_t>(n)) { ok = false; return false; }
        std::memcpy(dst, p, n);
        p += n;
        return true;
    }
};

} // namespace

// ---- hall-of-fame bookkeeping -------------------------------------------------

static bool entry_sane(const GenePoolEntry& e) {
    return e.brain.valid && !e.brain.slow.empty() &&
           e.brain.slow.size() == brain_layout_h(e.brain.hidden).total_bytes;
}

void GenePool::consider(GenePoolEntry e) {
    if (!entry_sane(e)) return;
    dedupe_slow(e.brain.slow);
    if (entries_.size() >= kMaxEntries &&
        e.fitness <= entries_.back().fitness)
        return;
    entries_.push_back(std::move(e));
    std::sort(entries_.begin(), entries_.end(),
              [](const GenePoolEntry& a, const GenePoolEntry& b) {
                  return a.fitness > b.fitness;
              });
    if (entries_.size() > kMaxEntries) entries_.resize(kMaxEntries);
}

void GenePool::bless(GenePoolEntry e) {
    if (!entry_sane(e)) return;
    dedupe_slow(e.brain.slow);
    e.fitness = entries_.empty() ? 1 : entries_.front().fitness + 1;
    entries_.insert(entries_.begin(), std::move(e));
    if (entries_.size() > kMaxEntries) entries_.resize(kMaxEntries);
}

void GenePool::dedupe_slow(const std::vector<std::int8_t>& slow) {
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
        [&](const GenePoolEntry& e) { return e.brain.slow == slow; }),
        entries_.end());
}

// ---- deterministic derivation -------------------------------------------------

bool GenePool::derive(std::uint64_t world_seed, EntityId npc,
                      BrainGenome& out) const {
    if (entries_.empty()) return false;
    const std::size_t n = entries_.size();
    std::uint64_t h = mix(world_seed, static_cast<std::uint64_t>(npc));
    h = mix(h, 0x5EED5EED5EEDull);
    // 70% draw: the remaining 30% keeps hash-init diversity
    if (h % kDeriveDenominator >= 7) return false;

    const std::size_t i1 = static_cast<std::size_t>((h >> 8) % n);
    const std::size_t i2 = static_cast<std::size_t>((h >> 24) % n);
    // donors must share a hidden size for the block crossover to be legal
    const std::int32_t H = entries_[i1].brain.hidden;
    const GenePoolEntry* e1 = nullptr;
    const GenePoolEntry* e2 = nullptr;
    for (std::size_t k = 0; k < n && (!e1 || !e2); ++k) {
        const GenePoolEntry& c = entries_[(i1 + k) % n];
        if (!e1 && c.brain.hidden == H && !c.brain.slow.empty()) e1 = &c;
    }
    for (std::size_t k = 0; k < n && !e2; ++k) {
        const GenePoolEntry& c = entries_[(i2 + k) % n];
        if (c.brain.hidden == H && !c.brain.slow.empty() &&
            (!e1 || &c != e1 || n == 1)) { e2 = &c; break; }
    }
    if (!e1) return false;
    if (!e2) e2 = e1;

    const BrainGenome& ga = e1->brain;
    const BrainGenome& gb = e2->brain;
    BrainGenome g;
    g.valid = true;
    g.hidden = H;
    auto pick = [&](std::int32_t x, std::int32_t y, std::uint64_t f) {
        return (mix(world_seed ^ (static_cast<std::uint64_t>(npc) << 1), f) & 1)
                   ? y : x;
    };
    g.lr_permille = pick(ga.lr_permille, gb.lr_permille, 1);
    g.trace_decay = pick(ga.trace_decay, gb.trace_decay, 2);
    g.mut_scale = pick(ga.mut_scale, gb.mut_scale, 3);

    const BrainLayout& L = brain_layout_h(H);
    const std::size_t nb = L.total_bytes;
    g.slow.assign(nb, 0);
    // block crossover: 64-byte blocks from either donor, hash-picked
    for (std::size_t blk = 0; blk * 64 < nb; ++blk) {
        const GenePoolEntry& src =
            (mix(world_seed ^ (static_cast<std::uint64_t>(npc) << 1),
                 blk + 100) & 1) ? *e2 : *e1;
        const std::size_t cnt = std::min<std::size_t>(64, nb - blk * 64);
        const std::size_t soff = blk * 64;
        if (soff + cnt <= src.brain.slow.size())
            std::memcpy(g.slow.data() + soff, src.brain.slow.data() + soff, cnt);
    }
    // light mutation at the child's own mut_scale
    brain_mutate_blocks(g.slow.data(), static_cast<std::int64_t>(nb),
                        world_seed, npc, 0, 3, g.mut_scale);
    // clamp int16 bias regions (byte crossover can produce bad halves)
    auto clamp_bias = [&](std::uint32_t boff, std::uint32_t rows) {
        std::int16_t* bp = reinterpret_cast<std::int16_t*>(g.slow.data() + boff);
        for (std::uint32_t i = 0; i < rows; ++i)
            bp[i] = static_cast<std::int16_t>(
                std::max<std::int64_t>(-4096, std::min<std::int64_t>(4095, bp[i])));
    };
    const std::int32_t Hh = g.hidden;
    clamp_bias(L.embed_b, Hh); clamp_bias(L.zb, Hh); clamp_bias(L.hb, Hh);
    clamp_bias(L.m1b, 32);    clamp_bias(L.m2b, 8);
    clamp_bias(L.w1b, 64);    clamp_bias(L.w2b, 16); clamp_bias(L.aqb, 16);
    clamp_bias(L.wm1b, 96);   clamp_bias(L.wm2b, 25); clamp_bias(L.valb, 1);
    out = std::move(g);
    return true;
}

// ---- binary persistence -------------------------------------------------------

bool GenePool::load(const std::string& path, const PropertyRegistry& props) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { entries_.clear(); return false; }
    std::string blob((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    Reader r{blob.data(), blob.data() + blob.size(), true};
    if (r.u32() != kMagic || r.u32() != kVersion || !r.ok) {
        entries_.clear();
        return false;
    }
    std::vector<GenePoolEntry> loaded;
    const std::uint32_t count = r.u32();
    for (std::uint32_t i = 0; i < count && r.ok; ++i) {
        GenePoolEntry e;
        e.brain.hidden = static_cast<std::int32_t>(r.u32());
        e.brain.lr_permille = static_cast<std::int32_t>(r.u32());
        e.brain.trace_decay = static_cast<std::int32_t>(r.u32());
        e.brain.mut_scale = static_cast<std::int32_t>(r.u32());
        e.fitness = static_cast<std::int64_t>(r.u64());
        e.seen_tick = r.u64();
        e.kind = r.str();
        const std::uint32_t nb = r.u32();
        for (std::uint32_t k = 0; k < nb && r.ok; ++k) {
            const std::string pname = r.str();
            const std::int64_t v = static_cast<std::int64_t>(r.u64());
            if (const PropertyDef* d = props.find_name(pname))
                e.body.set(d->id, v);
        }
        const std::uint32_t nw = r.u32();
        e.brain.slow.resize(nw);
        if (nw && !r.bytes(e.brain.slow.data(), nw)) { r.ok = false; break; }
        e.brain.valid = !e.brain.slow.empty();
        if (entry_sane(e)) loaded.push_back(std::move(e));
    }
    if (!r.ok) { entries_.clear(); return false; }
    entries_ = std::move(loaded);
    if (entries_.size() > kMaxEntries) entries_.resize(kMaxEntries);
    return true;
}

bool GenePool::save(const std::string& path,
                    const PropertyRegistry& props) const {
    std::string b;
    b.reserve(1 << 16);
    put32(b, kMagic);
    put32(b, kVersion);
    put32(b, static_cast<std::uint32_t>(entries_.size()));
    for (const GenePoolEntry& e : entries_) {
        put32(b, static_cast<std::uint32_t>(e.brain.hidden));
        put32(b, static_cast<std::uint32_t>(e.brain.lr_permille));
        put32(b, static_cast<std::uint32_t>(e.brain.trace_decay));
        put32(b, static_cast<std::uint32_t>(e.brain.mut_scale));
        put64(b, static_cast<std::uint64_t>(e.fitness));
        put64(b, e.seen_tick);
        put_str(b, e.kind);
        put32(b, static_cast<std::uint32_t>(e.body.loci.size()));
        for (const auto& [pid, v] : e.body.loci) {
            const PropertyDef* d = props.find(pid);
            put_str(b, d ? d->name : std::string("?"));
            put64(b, static_cast<std::uint64_t>(v));
        }
        put32(b, static_cast<std::uint32_t>(e.brain.slow.size()));
        put_bytes(b, e.brain.slow.data(), e.brain.slow.size());
    }
    const std::filesystem::path parent =
        std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(b.data(), static_cast<std::streamsize>(b.size()));
    return static_cast<bool>(out);
}

} // namespace simlab
