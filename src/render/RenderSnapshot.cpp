// Phase 13 — read-only RenderSnapshot capture + 3-tier FogOfWar.
// Takes `const World&` only: rendering can never perturb the simulation.
#include "simlab/render/Snapshot.hpp"
#include "simlab/World.hpp"
#include "simlab/Cognition.hpp"
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace simlab {
std::int64_t sight_radius_of(const World& w, const Entity& e);   // Cognition.cpp
}

namespace simlab::render {

namespace {

// abstract records: World Truth, but not things with a body to draw
bool abstract_kind(const Entity& e) {
    static const KindName kAbstract[] = {
        SIMLAB_KIND("language"), SIMLAB_KIND("lexical_entry"), SIMLAB_KIND("semantic_atom"),
        SIMLAB_KIND("semantic_concept"), SIMLAB_KIND("collective"),
        SIMLAB_KIND("debt_obligation"), SIMLAB_KIND("infection"), SIMLAB_KIND("pathogen_strain")};
    for (const KindName& k : kAbstract) if (e.kind_name == k) return true;
    return false;
}
bool static_kind(const Entity& e) {
    return e.kind_name == SIMLAB_KIND("enclosure") || e.kind_name == SIMLAB_KIND("inscription") ||
           e.kind_name == SIMLAB_KIND("portal") || e.deposit != kNoDeposit;
}

}  // namespace

// ---- fog ------------------------------------------------------------------------
namespace {
std::uint64_t pack_cell(int gx, int gy) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(gx)) << 32) |
           static_cast<std::uint32_t>(gy);
}
}  // namespace

void FogOfWar::ensure_grid(const World& w) {
    const TerrainGrid& t = w.terrain;
    int x0, y0, x1, y1;
    if (bounds_set_) {
        x0 = bx0_; y0 = by0_; x1 = bx1_; y1 = by1_;
    } else if (!t.empty()) {
        // terrain plus a margin so sight disks at the edge are not clipped
        constexpr int kMargin = 48;
        x0 = t.ox - kMargin; y0 = t.oy - kMargin;
        x1 = t.ox + t.w + kMargin; y1 = t.oy + t.h + kMargin;
    } else {
        x0 = y0 = -256; x1 = y1 = 256;
    }
    // never shrink an already-allocated grid (lists index into it)
    if (gw_ > 0) {
        x0 = std::min(x0, ox_); y0 = std::min(y0, oy_);
        x1 = std::max(x1, ox_ + gw_); y1 = std::max(y1, oy_ + gh_);
    }
    const int nw = x1 - x0, nh = y1 - y0;
    if (nw <= 0 || nh <= 0) return;
    if (gw_ == nw && gh_ == nh && ox_ == x0 && oy_ == y0) return;
    // grow-only: copy the old window into the new grid at its offset
    const int nox = ox_, noy = oy_, ow = gw_, oh = gh_;
    std::vector<std::uint8_t> ng(std::size_t(nw) * nh, 0);
    if (ow > 0) {
        const int cx0 = std::max(0, nox - x0), cy0 = std::max(0, noy - y0);
        const int cw = std::min(ow - cx0, nw - cx0), ch = std::min(oh - cy0, nh - cy0);
        if (cw > 0 && ch > 0)
            for (int y = 0; y < ch; ++y)
                std::copy_n(grid_.data() + std::size_t(cy0 + y) * ow + cx0, cw,
                            ng.data() + std::size_t(cy0 + y) * nw + cx0);
    }
    grid_.swap(ng); ox_ = x0; oy_ = y0; gw_ = nw; gh_ = nh;
}

void FogOfWar::update(const World& w) {
    cell_ = w.terrain.empty() ? 16 : std::max(1, w.terrain.cell);
    ensure_grid(w);
    // demote last update's visible ring to shrouded (still explored)
    for (const std::uint64_t k : visible_list_) {
        const int gx = static_cast<int>(k >> 32), gy = static_cast<int>(std::uint32_t(k));
        if (gx < ox_ || gy < oy_ || gx >= ox_ + gw_ || gy >= oy_ + gh_) continue;
        std::uint8_t& c = grid_[std::size_t(gy - oy_) * gw_ + (gx - ox_)];
        if (c == 2) c = 1;
    }
    visible_list_.clear();
    if (mode_ == Mode::GodEye || gw_ <= 0) { ++generation_; return; }
    const PropId lang_p = SIMLAB_PID(w.props, "language_id");
    // first pass: collect observers (minds of the tribe, or every mind)
    struct Obs { int cx, cy; std::int64_t r; };
    std::vector<Obs> obs;
    for (const auto& [mid, m] : w.minds_) {
        (void)m;
        const Entity* e = w.find(mid);
        if (!e || !e->alive) continue;
        if (mode_ == Mode::Avatar && static_cast<std::int64_t>(mid) != tribe_) continue;
        if (mode_ == Mode::Tribe && tribe_ != 0 &&
            (lang_p == kNoProp || w.prop_int(*e, lang_p, 0) != tribe_)) continue;
        Obs o;
        o.r = std::max<std::int64_t>(0, sight_radius_of(w, *e));
        o.cx = TerrainGrid::cell_of(e->pos.x, cell_);
        o.cy = TerrainGrid::cell_of(e->pos.y, cell_);
        obs.push_back(o);
    }
    auto mark = [&](int gx, int gy) {
        if (wrap_x_ > 0) gx = ((gx % wrap_x_) + wrap_x_) % wrap_x_;
        if (gx < ox_ || gy < oy_ || gx >= ox_ + gw_ || gy >= oy_ + gh_) return;
        const std::size_t i = std::size_t(gy - oy_) * gw_ + (gx - ox_);
        std::uint8_t& c = grid_[i];
        if (c == 0) { c = 1; explored_list_.push_back(pack_cell(gx, gy)); }
        if (c != 2) { c = 2; visible_list_.push_back(pack_cell(gx, gy)); }
    };
    for (const Obs& o : obs) {
        // scanline disk fill: for each row dy, the chord half-width in cells
        const std::int64_t r2 = o.r * o.r;
        const int rc = static_cast<int>(o.r / cell_) + 1;
        for (int dy = -rc; dy <= rc; ++dy) {
            const std::int64_t my = static_cast<std::int64_t>(dy) * cell_;
            const std::int64_t rem = r2 - my * my;
            if (rem < 0) continue;
            const int dx = static_cast<int>(std::sqrt(static_cast<double>(rem)) / cell_);
            const int gy = o.cy + dy;
            for (int gx = o.cx - dx; gx <= o.cx + dx; ++gx) mark(gx, gy);
        }
    }
    ++generation_;
}

Fog FogOfWar::cell_fog(int gx, int gy) const {
    if (mode_ == Mode::GodEye) return Fog::Visible;
    if (wrap_x_ > 0) gx = ((gx % wrap_x_) + wrap_x_) % wrap_x_;
    if (gw_ <= 0 || gx < ox_ || gy < oy_ || gx >= ox_ + gw_ || gy >= oy_ + gh_)
        return Fog::Unexplored;
    const std::uint8_t c = grid_[std::size_t(gy - oy_) * gw_ + (gx - ox_)];
    return c == 2 ? Fog::Visible : c == 1 ? Fog::Shrouded : Fog::Unexplored;
}
Fog FogOfWar::point_fog(const World& w, Vec2 p) const {
    const int c = w.terrain.empty() ? 16 : std::max(1, w.terrain.cell);
    return cell_fog(TerrainGrid::cell_of(p.x, c), TerrainGrid::cell_of(p.y, c));
}

// ---- capture --------------------------------------------------------------------
RenderSnapshot capture(const World& w, const ViewRect& view, FogOfWar& fog,
                       CaptureScratch* scratch) {
    RenderSnapshot s;
    s.tick = w.clock_.tick();
    s.day_phase = static_cast<int>(s.tick % 24);
    s.view = view;
    s.cell = w.terrain.empty() ? 16 : w.terrain.cell;
    // ---- terrain cells in view (viewport culling) ----
    const TerrainGrid& t = w.terrain;
    if (!t.empty()) {
        const int gx0 = TerrainGrid::cell_of(view.x0, t.cell), gx1 = TerrainGrid::cell_of(view.x1 - 1, t.cell);
        const int gy0 = TerrainGrid::cell_of(view.y0, t.cell), gy1 = TerrainGrid::cell_of(view.y1 - 1, t.cell);
        for (int gy = gy0; gy <= gy1; ++gy)
            for (int gx = gx0; gx <= gx1; ++gx) {
                int lx, ly;
                if (!t.local_cell(gx, gy, lx, ly)) continue;
                const Fog f = fog.cell_fog(gx, gy);
                if (f == Fog::Unexplored) continue;
                CellView c;
                c.gx = gx; c.gy = gy;
                const std::size_t i = static_cast<std::size_t>(ly) * t.w + lx;
                c.elevation = t.elevation[i]; c.moisture = t.moisture[i];
                c.temperature = t.temperature[i]; c.vegetation = t.vegetation[i];
                c.slope = t.slope[i]; c.water = t.water[i];
                c.wear = t.traffic_wear.size() == t.elevation.size() ? t.traffic_wear[i] : 0;
                c.fog = f;
                s.cells.push_back(c);
            }
    }
    // ---- prop ids (one lookup per call) ----
    auto P = [&](const char* n) { const PropertyDef* d = w.props.find_name(n); return d ? d->id : kNoProp; };
    const PropId integ = P("integrity"), fc = P("fruit_count"), fm = P("max_fruit"), pain = P("pain_level"),
                 bleed = P("bleed_rate"), tox = P("toxin_load"), morale = P("combat_morale"), routed = P("routed"),
                 lang = P("language_id"), rad = P("footprint_radius"), bar = P("access_barrier"),
                 temp = P("temperature"), qty = P("quantity"), es = P("edge_sharpness"), edge = P("edge"),
                 bl = P("blunt_mass"), el = P("elasticity"), li = P("launch_impulse"), ct = P("coat_toxin"),
                 cs = P("coat_strain"), apr = P("armor_penetration_resist"), cont = P("contained"),
                 poss = P("possessed_by"), gt = P("glyph_text"), cl = P("clause_atoms"), ck = P("concept_keys"),
                 skey = P("sacred_key"), og = P("oral_genre"), cut = P("cultivar"),
                 tt = P("trophic_tier"), mdrv = P("migration_drive");
    (void)cut;
    // ---- entities in view ----
    const std::int64_t cxm = (static_cast<std::int64_t>(view.x0) + view.x1) / 2,
                       cym = (static_cast<std::int64_t>(view.y0) + view.y1) / 2;
    const std::int64_t hw = (static_cast<std::int64_t>(view.x1) - view.x0) / 2 + 32,
                       hh = (static_cast<std::int64_t>(view.y1) - view.y0) / 2 + 32;
    std::int64_t radius = 0; while (radius * radius < hw * hw + hh * hh) radius += 16;
    const std::vector<EntityId> near = w.entities_near({static_cast<std::int32_t>(cxm), static_cast<std::int32_t>(cym)}, radius);
    s.culled = w.entities.size() > near.size() ? w.entities.size() - near.size() : 0;
    // wealth by possessor (only for possessors visible in near) — cached
    // ~30 ticks in `scratch`: the scan is O(all entities), too hot per frame.
    std::unordered_map<std::int64_t, std::int64_t> wealth_local;
    const std::unordered_map<std::int64_t, std::int64_t>* wealth = nullptr;
    if (poss != kNoProp) {
        std::set<std::int64_t> in_view;
        for (const EntityId id : near) if (w.find_mind(id)) in_view.insert(static_cast<std::int64_t>(id));
        if (!in_view.empty()) {
            const std::uint64_t epoch = w.clock_.tick() / 30;
            if (scratch && scratch->epoch == epoch) {
                wealth = &scratch->wealth;
            } else {
                wealth_local.clear();
                for (const auto& ep : w.entities)
                    if (ep->alive) {
                        const std::int64_t h = w.prop_int(*ep, poss, 0);
                        if (h > 0)   // accumulate for ALL possessors; callers filter by in_view
                            wealth_local[h] += std::max<std::int64_t>(1, qty != kNoProp ? w.prop_int(*ep, qty, 1) : 1);
                    }
                if (scratch) { scratch->wealth = wealth_local; scratch->epoch = epoch; }
                wealth = scratch ? &scratch->wealth : &wealth_local;
            }
        }
    }
    std::set<EntityId> drawn;
    // per-cell species census for the ecology lens — only entities that
    // survived the fog checks above are counted, so the overlay can never
    // reveal what the observer cannot see.
    std::unordered_map<std::uint64_t, std::pair<int, int>> eco_density;
    for (const EntityId id : near) {
        const Entity* e = w.find(id);
        if (!e || abstract_kind(*e)) continue;
        if (e->pos.x < view.x0 - 16 || e->pos.x >= view.x1 + 16 ||
            e->pos.y < view.y0 - 16 || e->pos.y >= view.y1 + 32) continue;
        if (cont != kNoProp) {                         // parts inside a body/structure
            const Value* cv = w.prop_of(*e, cont);
            if (cv && cv->type == ValType::Boolean && cv->b) continue;
        }
        const Fog f = fog.point_fog(w, e->pos);
        if (f == Fog::Unexplored) continue;
        const bool stat = static_kind(*e);
        // map memory: everything that is not a living creature (plants,
        // ore, water, corpses, items, structures) is remembered as last seen;
        // under the shroud it shows frozen from that memory, creatures hide
        bool creature = false;
        if (e->alive) {
            if (w.find_mind(id)) creature = true;
            else if (const SpeciesDef* sp = w.species.find_name(e->kind_name.str()); sp && sp->fauna) creature = true;
            else {                                     // anything that walks or hungers moves
                const PropId spd = SIMLAB_PID(w.props, "speed"), hun = SIMLAB_PID(w.props, "hunger");
                if ((spd != kNoProp && w.prop_int(*e, spd, 0) > 0) || (hun != kNoProp && w.prop_of(*e, hun)))
                    creature = true;
            }
        }
        const bool memorable = !creature;
        if (f == Fog::Shrouded) continue;              // shroud: drawn from memory below
        EntityView v;
        v.id = id; v.pos = e->pos; v.kind = e->kind_name.str(); v.material = e->material;
        v.fog = f; v.alive = e->alive; v.structure = stat;
        if (const SpeciesDef* sp = w.species.find_name(e->kind_name.str())) {
            v.species = sp->name; v.flora = !sp->fauna; v.fauna = sp->fauna;
        }
        const AgentMind* m = w.find_mind(id);
        v.minded = m != nullptr;
        auto I = [&](PropId p, std::int64_t d) { return p != kNoProp ? w.prop_int(*e, p, d) : d; };
        v.integrity = I(integ, 1000); v.fruit = I(fc, 0); v.fruit_max = I(fm, 0);
        v.pain = I(pain, 0); v.bleed = I(bleed, 0); v.toxin = I(tox, 0);
        v.morale = e->props.get(morale) ? I(morale, -1) : -1; v.routed = I(routed, 0);
        v.lang = I(lang, 0); v.radius = I(rad, 0); v.barrier = I(bar, 0);
        v.trophic_tier = I(tt, 0); v.migration_drive = I(mdrv, 0);
        v.temperature = I(temp, 200); v.quantity = I(qty, 0);
        v.lang_seed = static_cast<std::uint32_t>(v.lang * 2654435761u);
        for (const Relation& r : w.relations_of(id, "carries")) if (r.a == id) v.infected = true;
        // equipment
        for (const Relation& r : w.relations_of(id, "wielding")) {
            if (r.a != id) continue;
            const Entity* tl = w.find(r.b);
            if (!tl || !tl->alive) continue;
            auto T = [&](PropId p) { return p != kNoProp ? w.prop_int(*tl, p, 0) : 0; };
            v.w_sharp = std::max(T(es), T(edge)); v.w_blunt = T(bl);
            v.w_elastic = std::max<std::int64_t>(T(el), T(li) > 0 ? 500 : 0);
            v.launch_range = T(li);
            v.w_coated = T(ct) > 0 || T(cs) > 0; v.w_mat = tl->material;
            break;
        }
        for (const Relation& r : w.relations_of(id, "wearing")) {
            if (r.a != id) continue;
            if (const Entity* a = w.find(r.b); a && a->alive && apr != kNoProp) {
                const std::int64_t ap = w.prop_int(*a, apr, 0);
                if (ap > v.armor) { v.armor = ap; v.armor_mat = a->material; }
            }
        }
        if (m) {
            if (const Belief* g = m->find_belief("grievance")) v.grievance = g->confidence;
            EntityId lead = kNoEntity; std::int64_t best = 399;
            for (const auto& [k, b] : m->beliefs) {
                if (k.rfind("sacred:", 0) == 0) v.sacred_conv = std::max(v.sacred_conv, b.confidence);
                if (k.rfind("defer_to:", 0) == 0 && b.confidence > best) {
                    try { lead = static_cast<EntityId>(std::stoull(k.substr(9))); best = b.confidence; } catch (...) {}
                }
                if (!v.epic && og != kNoProp && k.rfind("lex:", 0) == 0 && b.confidence >= 500) {
                    try {
                        const Entity* lx = w.find(static_cast<EntityId>(std::stoull(k.substr(4))));
                        const Value* gv = lx ? w.prop_of(*lx, og) : nullptr;
                        v.epic = gv && gv->type == ValType::Text && !gv->s.empty();
                    } catch (...) {}
                }
            }
            if (lead != kNoEntity)
                if (const Entity* le = w.find(lead); le && le->alive) {
                    LinkView lv; lv.from = id; lv.to = lead; lv.a = e->pos; lv.b = le->pos;
                    s.links.push_back(lv);
                }
            v.leader = pol_holds_mandate(w, id);
        }
        if (wealth) {
            const auto wi = wealth->find(static_cast<std::int64_t>(id));
            v.wealth = wi != wealth->end() ? wi->second : 0;
        }
        if (e->kind_name == SIMLAB_KIND("enclosure")) {
            v.function = arch_function(w, id);
            v.sacred = v.function == "sanctuary";
        }
        if (e->kind_name == SIMLAB_KIND("inscription")) {
            for (PropId p : {gt, cl, ck})
                if (const Value* tv = p != kNoProp ? w.prop_of(*e, p) : nullptr;
                    tv && tv->type == ValType::Text && !tv->s.empty()) { v.text = tv->s; break; }
            if (skey != kNoProp) { const Value* sv = w.prop_of(*e, skey); v.sacred = sv && sv->type == ValType::Text && !sv->s.empty(); }
        }
        if (memorable && f == Fog::Visible) fog.remembered[id] = v;   // update the map memory
        else if (!memorable && f == Fog::Visible) fog.remembered.erase(id);
        if (v.fauna || v.flora) {
            const std::uint64_t key =
                (static_cast<std::uint64_t>(static_cast<std::uint32_t>(
                     TerrainGrid::cell_of(e->pos.x, s.cell))) << 32) |
                static_cast<std::uint32_t>(TerrainGrid::cell_of(e->pos.y, s.cell));
            auto& cnt = eco_density[key];
            if (v.fauna) ++cnt.first; else ++cnt.second;
        }
        drawn.insert(id);
        s.entities.push_back(std::move(v));
    }
    // shrouded areas: show the LAST SEEN things, frozen (they may have
    // changed since). A remembered thing whose spot is in sight again but
    // that is no longer there (eaten, felled, moved) is forgotten.
    for (auto it = fog.remembered.begin(); it != fog.remembered.end();) {
        const EntityView& v = it->second;
        if (drawn.count(it->first) ||
            v.pos.x < view.x0 - 32 || v.pos.x >= view.x1 + 32 || v.pos.y < view.y0 - 32 || v.pos.y >= view.y1 + 64) { ++it; continue; }
        const Fog f = fog.point_fog(w, v.pos);
        if (f == Fog::Visible) { it = fog.remembered.erase(it); continue; }   // looked again: gone
        if (f == Fog::Shrouded) { EntityView r = v; r.fog = Fog::Shrouded; s.entities.push_back(std::move(r)); }
        ++it;
    }
    if (!eco_density.empty())
        for (CellView& c : s.cells) {
            const auto it = eco_density.find(
                (static_cast<std::uint64_t>(static_cast<std::uint32_t>(c.gx)) << 32) |
                static_cast<std::uint32_t>(c.gy));
            if (it != eco_density.end()) { c.fauna = it->second.first; c.flora = it->second.second; }
        }
    std::sort(s.entities.begin(), s.entities.end(), [](const EntityView& a, const EntityView& b) {
        return a.pos.y != b.pos.y ? a.pos.y < b.pos.y : a.id < b.id;
    });
    // CL-1 shadow-brain observability: read-only stats, never fed back.
    if (w.brain_shadow && w.brains_) {
        s.brain_slots = static_cast<std::int64_t>(w.brains_->slots.size());
        s.brain_wm_mae = w.brains_->wm_n > 0
            ? w.brains_->wm_err_sum / static_cast<std::int64_t>(w.brains_->wm_n) : 0;
        s.brain_avg_surprise = w.brains_->sigma_n > 0
            ? w.brains_->sigma_sum / static_cast<std::int64_t>(w.brains_->sigma_n) : 0;
    }
    return s;
}

}  // namespace simlab::render
