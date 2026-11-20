// Phase 13 — read-only RenderSnapshot capture + 3-tier FogOfWar.
// Takes `const World&` only: rendering can never perturb the simulation.
#include "simlab/render/Snapshot.hpp"
#include "simlab/World.hpp"
#include "simlab/Cognition.hpp"
#include <algorithm>
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
std::int64_t key2(int gx, int gy) {
    return (static_cast<std::int64_t>(gx) << 32) ^ static_cast<std::uint32_t>(gy);
}

}  // namespace

// ---- fog ------------------------------------------------------------------------
void FogOfWar::update(const World& w) {
    cell_ = w.terrain.empty() ? 16 : std::max(1, w.terrain.cell);
    visible_.clear();
    if (mode_ == Mode::GodEye) return;               // God Eye: no fog at all
    const PropId lang_p = SIMLAB_PID(w.props, "language_id");
    for (const auto& [mid, m] : w.minds_) {
        (void)m;
        const Entity* e = w.find(mid);
        if (!e || !e->alive) continue;
        if (mode_ == Mode::Avatar && static_cast<std::int64_t>(mid) != tribe_) continue;
        if (mode_ == Mode::Tribe && tribe_ != 0 &&
            (lang_p == kNoProp || w.prop_int(*e, lang_p, 0) != tribe_)) continue;
        const std::int64_t r = std::max<std::int64_t>(0, sight_radius_of(w, *e));
        const int cx = TerrainGrid::cell_of(e->pos.x, cell_), cy = TerrainGrid::cell_of(e->pos.y, cell_);
        const int rc = static_cast<int>(r / cell_) + 1;
        for (int dy = -rc; dy <= rc; ++dy)
            for (int dx = -rc; dx <= rc; ++dx) {
                const std::int64_t mx = static_cast<std::int64_t>(dx) * cell_,
                                   my = static_cast<std::int64_t>(dy) * cell_;
                if (mx * mx + my * my > r * r) continue;
                visible_.emplace(cx + dx, cy + dy);
            }
    }
    explored_.insert(visible_.begin(), visible_.end());
}

Fog FogOfWar::cell_fog(int gx, int gy) const {
    if (mode_ == Mode::GodEye) return Fog::Visible;
    if (visible_.count({gx, gy})) return Fog::Visible;
    if (explored_.count({gx, gy})) return Fog::Shrouded;
    return Fog::Unexplored;
}
Fog FogOfWar::point_fog(const World& w, Vec2 p) const {
    const int c = w.terrain.empty() ? 16 : std::max(1, w.terrain.cell);
    return cell_fog(TerrainGrid::cell_of(p.x, c), TerrainGrid::cell_of(p.y, c));
}

// ---- capture --------------------------------------------------------------------
RenderSnapshot capture(const World& w, const ViewRect& view, FogOfWar& fog) {
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
                const std::size_t i = static_cast<std::size_t>(ly) * t.w + lx;
                CellView c;
                c.gx = gx; c.gy = gy;
                c.elevation = t.elevation[i]; c.moisture = t.moisture[i];
                c.temperature = t.temperature[i]; c.vegetation = t.vegetation[i];
                c.slope = t.slope[i]; c.water = t.water[i];
                c.wear = t.traffic_wear.size() == t.elevation.size() ? t.traffic_wear[i] : 0;
                c.fog = fog.cell_fog(gx, gy);
                if (c.fog == Fog::Unexplored) { c.elevation = 0; c.water = 0; c.wear = 0; }  // no leak
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
                 skey = P("sacred_key"), og = P("oral_genre"), cut = P("cultivar");
    (void)cut;
    // wealth by possessor (one pass)
    std::unordered_map<std::int64_t, std::int64_t> wealth;
    if (poss != kNoProp)
        for (const auto& ep : w.entities)
            if (ep->alive) {
                const std::int64_t h = w.prop_int(*ep, poss, 0);
                if (h > 0) wealth[h] += std::max<std::int64_t>(1, qty != kNoProp ? w.prop_int(*ep, qty, 1) : 1);
            }
    // ---- entities in view ----
    const std::int64_t cxm = (static_cast<std::int64_t>(view.x0) + view.x1) / 2,
                       cym = (static_cast<std::int64_t>(view.y0) + view.y1) / 2;
    const std::int64_t hw = (static_cast<std::int64_t>(view.x1) - view.x0) / 2 + 64,
                       hh = (static_cast<std::int64_t>(view.y1) - view.y0) / 2 + 64;
    std::int64_t radius = 0; while (radius * radius < hw * hw + hh * hh) radius += 16;
    const std::vector<EntityId> near = w.entities_near({static_cast<std::int32_t>(cxm), static_cast<std::int32_t>(cym)}, radius);
    s.culled = w.entities.size() > near.size() ? w.entities.size() - near.size() : 0;
    std::set<EntityId> drawn;
    for (const EntityId id : near) {
        const Entity* e = w.find(id);
        if (!e || abstract_kind(*e)) continue;
        if (e->pos.x < view.x0 - 32 || e->pos.x >= view.x1 + 32 ||
            e->pos.y < view.y0 - 32 || e->pos.y >= view.y1 + 64) continue;
        if (cont != kNoProp) {                         // parts inside a body/structure
            const Value* cv = w.prop_of(*e, cont);
            if (cv && cv->type == ValType::Boolean && cv->b) continue;
        }
        const Fog f = fog.point_fog(w, e->pos);
        if (f == Fog::Unexplored) continue;
        const bool stat = static_kind(*e);
        if (f == Fog::Shrouded && !stat) continue;     // motion hidden under the shroud
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
        v.wealth = wealth.count(static_cast<std::int64_t>(id)) ? wealth[static_cast<std::int64_t>(id)] : 0;
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
        if (stat && f == Fog::Visible) fog.remembered[id] = v;      // update the map memory
        drawn.insert(id);
        s.entities.push_back(std::move(v));
    }
    // shrouded areas: show the LAST SEEN architecture (it may have changed since)
    for (const auto& [id, v] : fog.remembered) {
        if (drawn.count(id)) continue;
        if (v.pos.x < view.x0 - 32 || v.pos.x >= view.x1 + 32 || v.pos.y < view.y0 - 32 || v.pos.y >= view.y1 + 64) continue;
        if (fog.point_fog(w, v.pos) != Fog::Shrouded) continue;
        EntityView r = v; r.fog = Fog::Shrouded;
        s.entities.push_back(std::move(r));
    }
    std::sort(s.entities.begin(), s.entities.end(), [](const EntityView& a, const EntityView& b) {
        return a.pos.y != b.pos.y ? a.pos.y < b.pos.y : a.id < b.id;
    });
    return s;
}

}  // namespace simlab::render
