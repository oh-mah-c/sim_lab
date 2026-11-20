#include "simlab/Genesis.hpp"
#include "simlab/World.hpp"
#include "simlab/WorldGen.hpp"
#include <algorithm>
#include <climits>
#include <cstdlib>

namespace simlab {

namespace {

std::uint64_t ghash(std::uint64_t a, std::uint64_t b, std::uint64_t c) {
    std::uint64_t x = a * 0x9E3779B97F4A7C15ull ^ (b + 0x632BE59BD9B4E019ull) ^
                      (c * 0xD6E8FEB86659FD93ull);
    x ^= x >> 31; x *= 0xBF58476D1CE4E5B9ull; x ^= x >> 29;
    x *= 0x94D049BB133111EBull; x ^= x >> 32;
    return x;
}

// habitable camp: best land cell within `radius` of the anchor — warm
// (terrain temperature near 33 C: a naked band dies of cold elsewhere),
// with water within 6 cells (thirst is an instinct, drinking is not a
// technology), then closest to the anchor. Deterministic scan order.
Vec2 camp_near(const World& w, int ax, int ay, int radius) {
    const TerrainGrid& t = w.terrain;
    Vec2 best = t.cell_center(ax, ay);
    std::int64_t best_score = INT64_MIN;
    for (int dy = -radius; dy <= radius; ++dy)
        for (int dx = -radius; dx <= radius; ++dx) {
            const int gx = ax + dx, gy = ay + dy;
            const Vec2 c = t.cell_center(gx, gy);
            if (!t.contains(c) || t.is_water(c)) continue;
            bool wet = false;
            for (int wy = -6; wy <= 6 && !wet; ++wy)
                for (int wx = -6; wx <= 6; ++wx) {
                    const Vec2 q = t.cell_center(gx + wx, gy + wy);
                    if (t.contains(q) && t.is_water(q)) { wet = true; break; }
                }
            const std::int64_t temp = t.temp_at(c);
            const std::int64_t score = (wet ? 0 : -100000) - std::abs(temp - 330) * 8 -
                                       static_cast<std::int64_t>(dx * dx + dy * dy);
            if (score > best_score) { best_score = score; best = c; }
        }
    return best;
}

}  // namespace

PrimordialWorld build_primordial(World& w, const PrimordialParams& pp) {
    PrimordialWorld out;
    const int grid = std::clamp(pp.grid, 32, 256);
    const int tribes = std::clamp(pp.tribes, 1, 4);
    const int per = std::clamp(pp.per_tribe, 3, 12);

    int ox = 0, oy = 0, gw = grid, gh = grid;
    std::uint64_t gseed = pp.seed;
    if (pp.planet) {
        const bool scaled = pp.planet_w > 0 && pp.planet_h > 0;
        const int PW = scaled ? std::clamp(pp.planet_w, 256, 8192) : grid;
        const int PH = scaled ? std::clamp(pp.planet_h, 128, 4096) : grid;
        gseed = WorldGenerator::planet_seed(static_cast<std::uint32_t>(pp.seed), PW, PH);
        out.planet_w = PW; out.planet_h = PH;
        if (scaled) {
            // home region on the warm belt, at the longitude with the most
            // lowland (sampled every 4 cells; deterministic scan order)
            gh = std::max(64, grid * 2 / 3);
            oy = PH / 2 - gh / 2;
            long best = -1;
            for (int cx = PW / 10; cx + gw < PW - PW / 10; cx += 32) {
                long land = 0;
                for (int y = oy; y < oy + gh; y += 4)
                    for (int x = cx; x < cx + gw; x += 4)
                        land += !WorldGenerator::is_sea(gseed, x, y, 200);
                if (land > best) { best = land; ox = cx; }
            }
        }
    }
    out.gen_seed = gseed;
    out.region_x = ox; out.region_y = oy; out.region_w = gw; out.region_h = gh;
    WorldGenParams g;
    g.seed = gseed;
    g.grid_w = gw; g.grid_h = gh;
    g.origin_x = ox; g.origin_y = oy;
    g.max_trees = 1 << 24;
    g.skip_species = {pp.founder_species};
    WorldGenerator().generate(w, g);
    w.ruleset |= World::kRulePrimordial;

    // living water: shoreline cells of rivers/lakes expose drinkable water
    // as `water`-material stock entities (flowing: flow_rate 1000 -> never
    // stagnates). This is terrain made graspable, not culture.
    if (const MaterialDef* wm = w.materials.find_name("water")) {
        const PropertyDef* qd = w.props.find_name("quantity");
        const PropertyDef* fd = w.props.find_name("flow_rate");
        const TerrainGrid& t = w.terrain;
        for (int gy = t.oy; gy < t.oy + t.h; ++gy)
            for (int gx = t.ox; gx < t.ox + t.w; ++gx) {
                const Vec2 c = t.cell_center(gx, gy);
                if (!t.is_water(c)) continue;
                bool shore = false;
                for (const auto& d : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
                    const Vec2 q = t.cell_center(gx + d.first, gy + d.second);
                    if (t.contains(q) && !t.is_water(q)) { shore = true; break; }
                }
                if (!shore || ghash(pp.seed, gx * 7919 + gy, 0x3A7Eu) % 3 != 0) continue;
                PropertyBag pb;
                if (qd) pb.set(qd->id, Value::integer(1000000));
                if (fd) pb.set(fd->id, Value::integer(1000));
                w.create_entity("water", wm->id, c, pb);
            }
    }

    const SpeciesDef* sp = w.species.find_name(pp.founder_species);
    if (!sp) return out;
    auto pid = [&](const char* n) {
        const PropertyDef* d = w.props.find_name(n);
        return d ? d->id : kNoProp;
    };
    const PropId p_lang = pid("language_id"), p_age = pid("age"),
                 p_adult = pid("adult_age");
    // personality axes: heritable variation so curiosity, caution and
    // drive differ between founders (and between their descendants).
    const char* kAxes[] = {"mind_abstraction_gain", "mind_threat_gain",
                           "mind_reward_gain", "mind_empathy_gain"};

    // band anchors. Valley: a ring around the centre, far apart. Planet:
    // spread east-west along the warm belt (each on its own continent if
    // the seas allow) — isolation breeds separate tongues and myths.
    for (int t = 0; t < tribes; ++t) {
        int ax = ox + gw / 2, ay = oy + gh / 2;
        if (pp.planet) {
            ax = ox + (t + 1) * gw / (tribes + 1);
            ay = oy + gh / 2 + ((t % 2) ? gh / 12 : -gh / 12);
        } else if (tribes > 1) {
            static const int kRing[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
            static const int kRing3[3][2] = {{-1, -1}, {1, -1}, {0, 1}};
            const int (*rp)[2] = tribes == 3 ? kRing3 : kRing;
            ax += rp[t][0] * grid * 38 / 100;
            ay += rp[t][1] * grid * 38 / 100;
        }
        const int j = static_cast<int>(ghash(pp.seed, t, 0xCA4Bu) % 7) - 3;
        const int radius = std::max(6, grid / (pp.planet ? 8 : 10));
        out.homes.push_back(camp_near(w, ax + j, ay - j, radius));
        out.tribe_lang.push_back(
            static_cast<std::int64_t>(100 + (ghash(pp.seed, t, 0x1A47u) % 900) * 4 + t));
    }

    std::int64_t adult = 250;
    if (p_adult != kNoProp)
        if (const Value* v = sp->props.get(p_adult)) adult = v->i;
    for (int t = 0; t < tribes; ++t) {
        const Vec2 home = out.homes[static_cast<std::size_t>(t)];
        for (int i = 0; i < per; ++i) {
            PropertyBag pb = sp->props;
            if (p_lang != kNoProp) pb.set(p_lang, Value::integer(out.tribe_lang[t]));
            // a band arrives rested and fed; mating waits for the species'
            // repro_cooldown measured from tick 0 (no tick-1 baby boom that
            // drains every founder's energy before anyone has eaten).
            if (const PropId pe = pid("energy"), pm = pid("max_energy"); pe != kNoProp && pm != kNoProp)
                if (const Value* mv = sp->props.get(pm)) pb.set(pe, Value::integer(mv->i));
            if (const PropId lr = pid("last_reproduce"); lr != kNoProp) pb.set(lr, Value::integer(0));
            if (p_age != kNoProp)
                pb.set(p_age, Value::integer(adult + static_cast<std::int64_t>(
                                                 ghash(pp.seed, t * 64 + i, 0xA6Eu) % 200)));
            const Vec2 pos{home.x + (i % 4) * 24 - 36, home.y + (i / 4) * 24 - 24};
            const EntityId id = w.create_entity(sp->name, sp->material, pos, pb);
            if (id == kNoEntity) continue;
            Entity* e = w.find(id);
            // founding genome: species loci + personality axes, each with a
            // deterministic per-founder deviation (genome == phenotype).
            std::vector<PropId> loci = sp->genes;
            for (const char* a : kAxes) if (pid(a) != kNoProp) loci.push_back(pid(a));
            for (std::size_t li = 0; li < loci.size(); ++li) {
                const PropertyDef* pd = w.props.find(loci[li]);
                if (!pd) continue;
                const Value* cur = e->prop(w, loci[li]);
                const bool axis = li >= sp->genes.size();
                const std::int64_t base = cur ? cur->i : (axis ? 1000 : 0);
                const std::int64_t span = axis ? 350 : std::max<std::int64_t>(1, base / 10);
                std::int64_t v = base + static_cast<std::int64_t>(
                    ghash(pp.seed, id, 0x6E9Eu + li) % static_cast<std::uint64_t>(2 * span + 1)) - span;
                if (pd->has_range) v = std::clamp(v, pd->lo, pd->hi);
                e->genome.set(loci[li], v);
                e->props.set(loci[li], Value::integer(v));
            }
            w.build_anatomy(id);
            AgentMind& m = w.mind(id);                 // empty lexicon, no beliefs
            for (const std::string& v : sp->innate) m.learn_action(v);
            out.founders.push_back(id);
        }
    }
    return out;
}

std::size_t primordial_violations(const World& w) {
    std::size_t n = 0;
    for (const char* k : {"enclosure", "inscription", "lexical_entry",
                          "semantic_concept", "semantic_atom", "debt_obligation",
                          "composite", "language", "portal"})
        for (EntityId id : w.entities_of_kind(KindName(k)))
            if (const Entity* e = w.find(id); e && e->alive) ++n;
    return n;
}

}  // namespace simlab
