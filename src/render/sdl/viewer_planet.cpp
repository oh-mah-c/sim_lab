#include "viewer.hpp"

namespace simlab_viewer {


    bool Viewer::is_planet() const { return planet_w > 0; }


    void Viewer::planet_setup() {
        chron_load(w);   // reload this world's Biên Niên Sử + narrative tail
        planet_w = planet_h = 0; globe = false; globe_dive = false;
        clear_gchunks();
        int pw_ = 0, ph_ = 0;
        for (auto& [k, t] : tile_tex) if (t) SDL_DestroyTexture(t);
        tile_tex.clear(); tile_lru.clear();
        if (!WorldGenerator::planet_dims(w.world_seed, pw_, ph_) || pw_ < 2 * ph_ - 1) return;   // valley
        planet_w = pw_; planet_h = ph_; gen_seed = w.world_seed;
        pmap.build(gen_seed, pw_, ph_, pw_ >= 4096 ? 2048 : 1024);
        ptiles.reset(gen_seed, pw_, ph_, 384);
        globe_fog.assign(std::size_t(pmap.aw) * pmap.ah, 0);
        globe_fog_frame = 0;
        // flat fog grid covers the whole planet (~8 MB worst case); gx wraps
        // at the antimeridian like planet_cell() does for terrain sampling.
        fog.set_bounds(0, 0, pw_, ph_);
        fog.set_wrap_x(pw_);
    }


    double Viewer::planet_units_w() const { return double(planet_w) * w.terrain.cell; }


    double Viewer::planet_units_h() const { return double(planet_h) * w.terrain.cell; }


    void Viewer::lonlat_of(double x, double y, double& lon, double& lat) const {
        lon = std::remainder(x / planet_units_w() * 2 * kPI, 2 * kPI);
        lat = (0.5 - y / planet_units_h()) * kPI;
    }


    void Viewer::pos_of(double lon, double lat, float& x, float& y) const {
        double u = std::fmod(lon / (2 * kPI), 1.0); if (u < 0) u += 1;
        x = float(u * planet_units_w());
        y = float((0.5 - lat / kPI) * planet_units_h());
    }


    void Viewer::wrap_camera() {
        if (!is_planet()) return;
        const double W = planet_units_w(), H = planet_units_h(), c = w.terrain.cell;
        cam.cx = float(std::fmod(std::fmod(double(cam.cx), W) + W, W));
        cam.cy = float(std::clamp(double(cam.cy), 6 * c, H - 6 * c));       // the ice walls of the poles
    }


    void Viewer::enter_globe() {
        if (!is_planet()) return;
        lonlat_of(cam.cx, cam.cy, gview.lon, gview.lat);
        gview.lat = std::clamp(gview.lat, -1.35, 1.35);
        gview.radius = std::max(cam.sw, cam.sh) * 1.2;
        globe = true; globe_dive = false; tool = kToolNone;
        status_msg = "Planet view - drag to spin, wheel/pinch to dive";
    }


    void Viewer::enter_surface(double lon, double lat) {
        pos_of(lon, lat, cam.cx, cam.cy);
        cam.zoom = 0.3f;
        wrap_camera();
        globe = false; globe_dive = false;
        status_msg = "Surface - wheel out at max zoom (or M) to return to orbit";
    }


    void Viewer::dive_to(double lon, double lat) { globe_dive = true; dive_lon = lon; dive_lat = lat; }


    double Viewer::ang_lerp(double a, double b, double t) { return a + std::remainder(b - a, 2 * 3.14159265358979323846) * t; }


    // wheel/pinch on the globe: zoom toward the point under (px, py) (world_rt px)
    void Viewer::globe_zoom(double factor, float px, float py) {
        double lon = 0, lat = 0;
        const bool hit = GlobeRenderer::unproject(gview, px - cam.sw * 0.5, py - cam.sh * 0.5, lon, lat);
        gview.radius = std::clamp(gview.radius * factor, std::min(cam.sw, cam.sh) * 0.16, 1e5);
        if (factor > 1 && hit) {
            gview.lon = ang_lerp(gview.lon, lon, 0.22);
            gview.lat = std::clamp(gview.lat + (lat - gview.lat) * 0.22, -1.35, 1.35);
        }
        if (gview.radius > std::max(cam.sw, cam.sh) * 1.35) enter_surface(gview.lon, gview.lat);
    }


    void Viewer::globe_step(double dt) {
        if (!globe_dive) return;
        gview.lon = ang_lerp(gview.lon, dive_lon, std::min(1.0, dt * 4));
        gview.lat += (dive_lat - gview.lat) * std::min(1.0, dt * 4);
        gview.radius *= 1.0 + dt * 2.6;
        if (gview.radius > std::max(cam.sw, cam.sh) * 1.35) enter_surface(dive_lon, dive_lat);
        ui_dirty = true;
    }


    // fog texels (under the lock): explored -> 1, visible -> 2
    void Viewer::build_globe_fog() {
        if (!is_planet() || fog.mode() == FogOfWar::Mode::GodEye) return;
        if (fog.generation() == globe_fog_gen) return;   // nothing moved
        globe_fog_gen = fog.generation();
        std::fill(globe_fog.begin(), globe_fog.end(), 0);
        auto mark = [&](std::uint64_t k, std::uint8_t v) {
            const int gx = static_cast<int>(k >> 32), gy = static_cast<int>(std::uint32_t(k));
            globe_fog[std::size_t(pmap.row_of(gy)) * pmap.aw + pmap.col_of(gx)] = v;
        };
        for (const std::uint64_t k : fog.explored_indices()) mark(k, 1);
        for (const std::uint64_t k : fog.visible_indices()) mark(k, 2);
    }


    double Viewer::sun_lon() const {
        const double hour = double(c_tick % 24) + 0.5;
        return std::remainder(kPI - hour / 24.0 * 2 * kPI + gview_sun_offset, 2 * kPI);
    }


    // title screen: a slowly turning decorative planet behind the menu
    void Viewer::draw_title_globe(double dt) {
        if (!title_map.valid()) return;
        title_view.lon += dt * 0.06;
        title_view.lat = 0.35;
        title_view.radius = std::min(cam.sw, cam.sh) * 0.55;
        draw_globe();
    }


    void Viewer::draw_globe() {
        const int W = cam.sw, H = cam.sh;
        // The sphere's pixel loop costs ~pi * radius^2 per frame (20 ms at
        // radius 768). When zoomed far out we render into a proportionally
        // smaller buffer — same image, GPU-upscaled with LINEAR — capping the
        // effective radius so a frame stays ~2-3 ms. px_ is rebuilt once per
        // (scaled) radius; lon/light changes reuse the cached projection.
        GlobeView gv = has_game ? gview : title_view;
        const double rr = std::max(1.0, double(gv.radius));
        const double gs = rr > 256.0 ? 256.0 / rr : 1.0;
        const int bw = std::max(96, int(W * gs)), bh = std::max(64, int(H * gs));
        if (!globe_tex || globe_tw != bw || globe_th != bh) {
            if (globe_tex) SDL_DestroyTexture(globe_tex);
            globe_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, bw, bh);
            SDL_SetTextureScaleMode(globe_tex, SDL_SCALEMODE_LINEAR);   // smooth upscale of the low-res sphere
            globe_tw = bw; globe_th = bh;
            stars.assign(std::size_t(bw) * bh, 0xFF05070Cu);
            std::uint32_t h = 0x9E3779B9u;
            for (int i = 0; i < bw * bh / 180; ++i) {          // deterministic starfield
                h ^= h << 13; h ^= h >> 17; h ^= h << 5;
                const std::uint32_t v = 90 + (h >> 24) % 150;
                stars[h % stars.size()] = 0xFF000000u | (v << 16) | (v << 8) | std::min<std::uint32_t>(255, v + 20);
            }
        }
        globe_px = stars;
        gv.radius *= gs;                                     // scale view with the buffer
        if (has_game)
            grend.render(pmap, gv, sun_lon(), fog.mode() == FogOfWar::Mode::GodEye ? nullptr : globe_fog.data(),
                         globe_px.data(), bw, bh, bw / 2, has_game ? bh / 2 : bh * 5 / 8);
        else
            grend.render(title_map, gv, time_s * 0.15, nullptr, globe_px.data(), bw, bh, bw / 2, bh * 5 / 8);
        SDL_UpdateTexture(globe_tex, nullptr, globe_px.data(), bw * 4);
        const SDL_FRect dst{0, 0, float(W), float(H)};
        SDL_RenderTexture(ren, globe_tex, nullptr, &dst);
    }


    // strategic icons on the sphere (cached under the lock, drawn in the UI layer)
    void Viewer::build_globe_icons() {
        c_gicons.clear();
        if (!is_planet()) return;
        const PropertyDef* ld = w.props.find_name("language_id");
        const PropId warp = SIMLAB_PID(w.props, "war_target");
        std::map<std::int64_t, std::vector<Vec2>> tribe_pos;
        std::map<std::int64_t, int> sick, war;
        for (const auto& [id, m] : w.minds_) {
            (void)m;
            const Entity* e = w.find(id);
            if (!e || !e->alive || !ld) continue;
            const std::int64_t l = w.prop_int(*e, ld->id, 0);
            if (l <= 0) continue;
            tribe_pos[l].push_back(e->pos);
            for (const Relation& r : w.relations_of(id, "carries"))
                if (r.a == id) if (const Entity* inf = w.find(r.b); inf && inf->alive && inf->kind_name == KindName("infection")) { ++sick[l]; break; }
            if (warp != kNoProp && w.prop_int(*e, warp, 0) > 0) ++war[l];
        }
        for (const auto& [l, ps] : tribe_pos) {
            double sx = 0, sy = 0;
            for (const Vec2& p : ps) { sx += p.x; sy += p.y; }
            sx /= double(ps.size()); sy /= double(ps.size());
            double rr = 0;
            for (const Vec2& p : ps) rr = std::max(rr, std::hypot(p.x - sx, p.y - sy));
            GIcon g; lonlat_of(sx, sy, g.lon, g.lat);
            g.type = 0; g.lang = l; g.r = float((rr + 4 * w.terrain.cell) / planet_units_w() * 2 * kPI);
            g.label = "tongue " + std::to_string(l) + " (" + std::to_string(ps.size()) + ")";
            c_gicons.push_back(g);
            if (sick[l]) { GIcon a = g; a.type = 3; a.label = std::to_string(sick[l]) + " sick"; c_gicons.push_back(a); }
            if (war[l]) { GIcon a = g; a.type = 4; a.label = std::to_string(war[l]) + " at war"; c_gicons.push_back(a); }
        }
        const TerrainGrid& t = w.terrain;
        if (t.traffic_wear.size() == t.elevation.size()) {
            int n = 0;
            for (int y = 0; y < t.h && n < 400; y += 2)
                for (int x = 0; x < t.w && n < 400; x += 2)
                    if (t.traffic_wear[std::size_t(y) * t.w + x] >= 80) {
                        GIcon g; lonlat_of((x + t.ox + 0.5) * t.cell, (y + t.oy + 0.5) * t.cell, g.lon, g.lat);
                        g.type = 1; c_gicons.push_back(g); ++n;
                    }
        }
        const PropId l2 = SIMLAB_PID(w.props, "language_id2");
        int ns = 0;
        for (EntityId id : w.entities_of_kind(SIMLAB_KIND("inscription"))) {
            const Entity* e = w.find(id);
            if (!e || !e->alive || ns++ > 60) continue;
            GIcon g; lonlat_of(e->pos.x, e->pos.y, g.lon, g.lat);
            g.type = 2; g.r = (l2 != kNoProp && w.prop_int(*e, l2, 0) > 0) ? 1.f : 0.f;
            c_gicons.push_back(g);
        }
    }


    void Viewer::draw_globe_icons() {
        if (!globe) return;
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        const float cx = cam.sw * 0.5f * kPix, cy = cam.sh * 0.5f * kPix;
        const bool blink = (frames / 20) % 2 == 0;
        int nlabel = 0;
        for (const GIcon& g : c_gicons) {
            double dx, dy;
            if (!GlobeRenderer::project(gview, g.lon, g.lat, dx, dy)) continue;   // far side
            const ImVec2 p(cx + float(dx) * kPix, cy + float(dy) * kPix);
            switch (g.type) {
                case 0: {   // territory + capital
                    ImVec2 prev; bool have = false;
                    for (int k = 0; k <= 32; ++k) {
                        const double a = k * 2 * kPI / 32;
                        double ex, ey;
                        const double lo = g.lon + g.r * std::cos(a) / std::max(0.2, std::cos(g.lat)), la = g.lat + g.r * std::sin(a);
                        if (!GlobeRenderer::project(gview, lo, la, ex, ey)) { have = false; continue; }
                        const ImVec2 q(cx + float(ex) * kPix, cy + float(ey) * kPix);
                        if (have) dl->AddLine(prev, q, IM_COL32(255, 210, 120, 220), 2.0f);
                        prev = q; have = true;
                    }
                    dl->AddCircleFilled(p, 6, IM_COL32(255, 215, 90, 255));
                    dl->AddCircle(p, 9, IM_COL32(60, 30, 10, 255), 12, 2);
                    const float ly = p.y - 8 + 15.f * float(nlabel++);   // stacked: nearby bands never overprint
                    dl->AddLine(ImVec2(p.x + 6, p.y), ImVec2(p.x + 12, ly + 7), IM_COL32(255, 240, 200, 160));
                    dl->AddText(ImVec2(p.x + 14, ly), IM_COL32(255, 240, 200, 255), g.label.c_str());
                    break;
                }
                case 1: dl->AddCircleFilled(p, 1.6f, IM_COL32(230, 190, 110, 200)); break;          // trails
                case 2: dl->AddRectFilled(ImVec2(p.x - 3, p.y - 5), ImVec2(p.x + 3, p.y + 5),
                                          g.r > 0 ? IM_COL32(120, 220, 255, 255) : IM_COL32(200, 200, 210, 230), 1); break;
                case 3: if (blink) { dl->AddCircle(p, 16, IM_COL32(120, 255, 80, 255), 16, 3); dl->AddText(ImVec2(p.x + 10, p.y + 8), IM_COL32(150, 255, 120, 255), g.label.c_str()); } break;
                case 4: if (blink) { dl->AddCircle(p, 22, IM_COL32(255, 70, 40, 255), 16, 3); dl->AddText(ImVec2(p.x + 10, p.y + 22), IM_COL32(255, 120, 90, 255), g.label.c_str()); } break;
            }
        }
    }


    // far terrain (analytic tiles) under the materialized region, surface view
    void Viewer::draw_far_terrain() {
        if (!is_planet()) return;
        const int cell = w.terrain.cell;
        const double CU = double(PlanetTiles::kChunk) * cell;
        const double x0 = cam.cx - cam.sw * 0.5 / cam.zoom, x1 = cam.cx + cam.sw * 0.5 / cam.zoom;
        const double y0 = cam.cy - cam.sh * 0.5 / cam.zoom / Camera::kY, y1 = cam.cy + cam.sh * 0.5 / cam.zoom / Camera::kY;
        const bool dim = fog.mode() != FogOfWar::Mode::GodEye;
        for (int cy = int(std::floor(y0 / CU)); cy <= int(std::floor(y1 / CU)); ++cy)
            for (int cxi = int(std::floor(x0 / CU)); cxi <= int(std::floor(x1 / CU)); ++cxi) {
                SDL_Texture* tx = tile_texture(cxi, cy);
                if (!tx) continue;
                SDL_FRect d{cam.sx(float(cxi * CU)), cam.sy(float(cy * CU)), float(CU) * cam.zoom + 1,
                            float(CU) * Camera::kY * cam.zoom + 1};
                if (dim) SDL_SetTextureColorMod(tx, 66, 58, 48); else SDL_SetTextureColorMod(tx, 255, 255, 255);
                SDL_RenderTexture(ren, tx, nullptr, &d);
            }
    }


    SDL_Texture* Viewer::tile_texture(int cx, int cy) {
        const int ncx = (planet_w + PlanetTiles::kChunk - 1) / PlanetTiles::kChunk;
        const int wcx = ((cx % ncx) + ncx) % ncx;
        const std::uint64_t k = ptiles.key(wcx, cy);
        if (auto it = tile_tex.find(k); it != tile_tex.end()) return it->second;
        const std::vector<std::uint32_t>* px = ptiles.tile(wcx, cy);
        if (!px) return nullptr;
        if (tile_tex.size() >= 384) {                   // LRU (insertion order) bound
            const std::uint64_t old = tile_lru.front(); tile_lru.pop_front();
            if (auto it = tile_tex.find(old); it != tile_tex.end()) { SDL_DestroyTexture(it->second); tile_tex.erase(it); }
        }
        SDL_Texture* t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, PlanetTiles::kChunk, PlanetTiles::kChunk);
        SDL_UpdateTexture(t, nullptr, px->data(), PlanetTiles::kChunk * 4);
        SDL_SetTextureScaleMode(t, SDL_SCALEMODE_NEAREST);
        tile_tex[k] = t; tile_lru.push_back(k);
        return t;
    }


    void Viewer::clear_gchunks() {
        for (auto& [k, g] : gchunks) if (g.t) SDL_DestroyTexture(g.t);
        gchunks.clear();
    }


    int Viewer::gchunk_margin() const { return int(600 * Camera::kElev * kCS) + 6; }


    bool Viewer::chunk_in_region(int wcx, int cy) const {
        const TerrainGrid& t = w.terrain;
        const int x0 = wcx * kCC, y0 = cy * kCC;
        return x0 < t.ox + t.w && x0 + kCC > t.ox && y0 < t.oy + t.h && y0 + kCC > t.oy;
    }


    CellView Viewer::planet_cell(int gx, int gy) const {
        CellView c; c.gx = gx; c.gy = gy;
        int lx, ly;
        const TerrainGrid& t = w.terrain;
        if (t.local_cell(gx, gy, lx, ly)) {
            const std::size_t i = std::size_t(ly) * t.w + lx;
            c.elevation = t.elevation[i]; c.moisture = t.moisture[i]; c.vegetation = t.vegetation[i];
            c.temperature = t.temperature[i]; c.slope = t.slope[i]; c.water = t.water[i];
            c.wear = t.traffic_wear.size() == t.elevation.size() ? t.traffic_wear[i] : 0;
            return c;
        }
        const std::int32_t sl = w.gen_sea_level;
        const int gxw = ((gx % planet_w) + planet_w) % planet_w;
        c.elevation = WorldGenerator::elevation_at(gen_seed, gxw, gy);
        c.temperature = WorldGenerator::temperature_at(gen_seed, gxw, gy, sl);
        c.water = WorldGenerator::water_at(gen_seed, gxw, gy, sl);
        c.moisture = c.water ? 1000 : WorldGenerator::moisture_at(gen_seed, gxw, gy, sl);
        c.vegetation = c.water ? 0 : (c.temperature >= -50 && c.temperature <= 350 ? c.moisture * 9 / 10 : c.moisture / 5);
        c.slope = WorldGenerator::slope_at(gen_seed, gxw, gy);
        return c;
    }


    void Viewer::build_gchunk(GChunk& g, int wcx, int cy) {
        const int cell = w.terrain.cell;
        const int margin = gchunk_margin();
        const int TW = int(kCC * cell * kCS), TH = int(kCC * cell * Camera::kY * kCS) + margin + 8;
        if (!g.t) {
            g.t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, TW, TH);
            SDL_SetTextureBlendMode(g.t, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(g.t, SDL_SCALEMODE_NEAREST);
        }
        SDL_SetRenderTarget(ren, g.t);
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 0);
        SDL_RenderClear(ren);
        const float ts = float(cell) * kCS;
        const bool region = chunk_in_region(wcx, cy);
        for (int ly = 0; ly < kCC; ++ly) {
            const int gy = cy * kCC + ly;
            if (gy < 0 || gy >= planet_h) continue;
            for (int lx = 0; lx < kCC; ++lx) {
                const CellView c = planet_cell(wcx * kCC + lx, gy);
                const float x = float(lx) * ts;
                const float y = (float(ly * cell) * Camera::kY - c.elevation * Camera::kElev) * kCS + margin;
                SDL_FRect dst{x, y, ts, ts * Camera::kY + 1};
                if (const Sprite* sp = atlas.material(terrain_material(c))) {
                    SDL_Texture* tx = tex.get(*sp);
                    int r = 255, gg = 255, b = 255;
                    if (!c.water) {
                        r = 255 - c.vegetation / 6; b = 255 - c.vegetation / 5;
                        const int wear = std::min(1000, c.wear);
                        r -= (r - 170) * wear / 1000; gg -= (gg - 140) * wear / 1000; b -= (b - 100) * wear / 1000;
                    }
                    SDL_SetTextureColorMod(tx, Uint8(std::clamp(r, 0, 255)), Uint8(std::clamp(gg, 0, 255)), Uint8(std::clamp(b, 0, 255)));
                    SDL_RenderTexture(ren, tx, nullptr, &dst);
                    SDL_SetTextureColorMod(tx, 255, 255, 255);
                }
                SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
                if (c.temperature < 20) {                               // frost -> snow -> ice
                    const int a = std::clamp((20 - c.temperature) * 2, 30, 230);
                    SDL_SetRenderDrawColor(ren, 236, 242, 250, Uint8(c.water ? a / 2 : a));
                    SDL_RenderFillRect(ren, &dst);
                }
                if (c.slope > 12 && !c.water) {
                    SDL_FRect face{x, y + ts * Camera::kY, ts, std::min(ts, c.slope * Camera::kElev * kCS)};
                    SDL_SetRenderDrawColor(ren, 40, 30, 22, 170);
                    SDL_RenderFillRect(ren, &face);
                }
                if (region) draw_cell_lens(c, dst);
                SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
            }
        }
        g.epoch = region ? c_tick / 240 : 0;
        g.lens = region ? lens : -1;
        SDL_SetRenderTarget(ren, world_rt);
    }


    void Viewer::draw_ground_chunks() {
        const int cell = w.terrain.cell;
        const double CU = double(kCC) * cell;
        const float margin_u = float(gchunk_margin()) / kCS;
        const int TW = int(kCC * cell * kCS), TH = int(kCC * cell * Camera::kY * kCS) + gchunk_margin() + 8;
        const double x0 = cam.cx - cam.sw * 0.5 / cam.zoom, x1 = cam.cx + cam.sw * 0.5 / cam.zoom;
        const double y0 = cam.cy - cam.sh * 0.5 / cam.zoom / Camera::kY, y1 = cam.cy + cam.sh * 0.5 / cam.zoom / Camera::kY;
        const int ncx = (planet_w + kCC - 1) / kCC;
        int budget = 3;                                   // chunk builds per frame
        const std::uint64_t ep = c_tick / 240;
        for (int cy = int(std::floor(y0 / CU)); cy <= int(std::floor(y1 / CU)) + 1; ++cy) {
            if (cy < 0 || cy * kCC >= planet_h) continue;
            for (int cxi = int(std::floor(x0 / CU)); cxi <= int(std::floor(x1 / CU)); ++cxi) {
                const int wcx = ((cxi % ncx) + ncx) % ncx;
                const std::uint64_t key = (std::uint64_t(std::uint32_t(cy)) << 32) | std::uint32_t(wcx);
                GChunk& g = gchunks[key];
                const bool stale = g.t && chunk_in_region(wcx, cy) && (g.epoch != ep || g.lens != lens);
                if ((!g.t || stale) && budget > 0) { build_gchunk(g, wcx, cy); --budget; }
                g.used = frames;
                const float sx = cam.sx(float(cxi * CU));
                if (g.t) {
                    SDL_FRect d{sx, cam.sy(float(cy * CU)) - margin_u * cam.zoom, float(TW) / kCS * cam.zoom + 0.5f,
                                float(TH) / kCS * cam.zoom};
                    SDL_RenderTexture(ren, g.t, nullptr, &d);
                } else if (SDL_Texture* ph = tile_texture(wcx, cy)) {     // placeholder until built
                    SDL_FRect d{sx, cam.sy(float(cy * CU)), float(CU) * cam.zoom + 1, float(CU) * Camera::kY * cam.zoom + 1};
                    SDL_SetTextureColorMod(ph, 255, 255, 255);
                    SDL_RenderTexture(ren, ph, nullptr, &d);
                }
            }
        }
        // LRU bound: ~0.9 MB per chunk, keep <= 96
        while (gchunks.size() > 96) {
            auto old = gchunks.end();
            for (auto it = gchunks.begin(); it != gchunks.end(); ++it)
                if (old == gchunks.end() || it->second.used < old->second.used) old = it;
            if (old->second.t) SDL_DestroyTexture(old->second.t);
            gchunks.erase(old);
        }
    }


    std::uint32_t Viewer::fog_rgba(Fog f) { return f == Fog::Visible ? 0u : f == Fog::Shrouded ? 0x780A0A12u : 0xD8120F0Cu; }


    void Viewer::build_fog_ground() {
        const TerrainGrid& t = w.terrain;
        if (t.empty()) return;
        const int cell = t.cell;
        fog_ground_margin = int(600 * Camera::kElev * kFS) + 2;
        const int W = int(t.w * cell * kFS) + 1, H = int(t.h * cell * Camera::kY * kFS) + fog_ground_margin + 4;
        if (!fog_ground) {
            fog_ground = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, W, H);
            SDL_SetTextureBlendMode(fog_ground, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(fog_ground, SDL_SCALEMODE_LINEAR);
        }
        SDL_Texture* prev = SDL_GetRenderTarget(ren);
        SDL_SetRenderTarget(ren, fog_ground);
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 0);
        SDL_RenderClear(ren);
        paint_fog_cells(t.ox, t.oy, t.ox + t.w, t.oy + t.h, float(t.ox * cell), float(t.oy * cell) * Camera::kY,
                        fog_ground_margin, [&](int gx, int gy, std::int32_t& e, std::int32_t& sl) {
                            const std::size_t i = std::size_t(gy - t.oy) * t.w + (gx - t.ox);
                            e = t.elevation[i]; sl = t.water[i] ? 0 : t.slope[i];
                        });
        SDL_SetRenderTarget(ren, prev);
    }


    void Viewer::draw_fog_ground() {
        if (!fog_ground || fog.mode() == FogOfWar::Mode::GodEye) return;
        const TerrainGrid& t = w.terrain;
        const float gox = float(t.ox * t.cell), goy = float(t.oy * t.cell);
        SDL_FRect src{(cam.cx - gox - cam.sw * 0.5f / cam.zoom) * kFS,
                      ((cam.cy - goy) * Camera::kY - cam.sh * 0.5f / cam.zoom) * kFS + fog_ground_margin,
                      cam.sw / cam.zoom * kFS, cam.sh / cam.zoom * kFS};
        SDL_FRect dst{0, 0, float(cam.sw), float(cam.sh)};
        SDL_RenderTexture(ren, fog_ground, &src, &dst);
    }


    void Viewer::clear_fchunks() {
        for (auto& [k, f] : fchunks) if (f.t) SDL_DestroyTexture(f.t);
        fchunks.clear(); fog_prev_visible.clear(); fog_explored_seen = 0;
    }


    std::uint64_t Viewer::fchunk_key_of_cell(int gx, int gy) const {
        const int ncx = std::max(1, (planet_w + kCC - 1) / kCC);
        const int cx = ((gx / kCC) % ncx + ncx) % ncx, cy = gy / kCC;
        return (std::uint64_t(std::uint32_t(cy)) << 32) | std::uint32_t(cx);
    }


    // called under the lock after fog.update(): mark touched chunks dirty
    void Viewer::fog_mark_dirty() {
        if (int(fog.mode()) != fchunk_mode || fog.tribe() != fchunk_tribe) {
            clear_fchunks(); fchunk_mode = int(fog.mode()); fchunk_tribe = fog.tribe();
        }
        auto mark = [&](std::uint64_t packed) {
            const int gx = int(packed >> 32), gy = int(std::uint32_t(packed));
            if (auto it = fchunks.find(fchunk_key_of_cell(gx, gy)); it != fchunks.end()) it->second.dirty = true;
        };
        // only cells whose tier CHANGED: (old visible) xor (new visible);
        // newly explored cells are in the new visible set by definition
        std::vector<std::uint64_t> cur = fog.visible_indices();
        std::sort(cur.begin(), cur.end());
        std::vector<std::uint64_t> diff;
        std::set_symmetric_difference(fog_prev_visible.begin(), fog_prev_visible.end(),
                                      cur.begin(), cur.end(), std::back_inserter(diff));
        for (std::uint64_t k : diff) mark(k);
        fog_explored_seen = fog.explored_indices().size();
        fog_prev_visible.swap(cur);
    }


    void Viewer::draw_fog_chunks() {
        if (fog.mode() == FogOfWar::Mode::GodEye) return;
        const int cell = w.terrain.cell;
        const double CU = double(kCC) * cell;
        const int mpx = int(600 * Camera::kElev * kFS) + 2;
        const int TW = int(kCC * cell * kFS) + 1, TH = int(kCC * cell * Camera::kY * kFS) + mpx + 4;
        const double x0 = cam.cx - cam.sw * 0.5 / cam.zoom, x1 = cam.cx + cam.sw * 0.5 / cam.zoom;
        const double y0 = cam.cy - cam.sh * 0.5 / cam.zoom / Camera::kY, y1 = cam.cy + cam.sh * 0.5 / cam.zoom / Camera::kY;
        const int ncx = (planet_w + kCC - 1) / kCC;
        int budget = 8;
        for (int cy = int(std::floor(y0 / CU)); cy <= int(std::floor(y1 / CU)) + 1; ++cy) {
            if (cy < 0 || cy * kCC >= planet_h) continue;
            for (int cxi = int(std::floor(x0 / CU)); cxi <= int(std::floor(x1 / CU)); ++cxi) {
                const int wcx = ((cxi % ncx) + ncx) % ncx;
                const std::uint64_t key = (std::uint64_t(std::uint32_t(cy)) << 32) | std::uint32_t(wcx);
                FChunk& fc = fchunks[key];
                fc.used = frames;
                if (fc.dirty && budget > 0) {
                    --budget;
                    if (!fc.t) {
                        fc.t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, TW, TH);
                        SDL_SetTextureBlendMode(fc.t, SDL_BLENDMODE_BLEND);
                        SDL_SetTextureScaleMode(fc.t, SDL_SCALEMODE_LINEAR);
                    }
                    SDL_Texture* prev = SDL_GetRenderTarget(ren);
                    SDL_SetRenderTarget(ren, fc.t);
                    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
                    SDL_SetRenderDrawColor(ren, 0, 0, 0, 0);
                    SDL_RenderClear(ren);
                    const int gx0 = wcx * kCC, gy0 = cy * kCC;
                    paint_fog_cells(gx0, gy0, gx0 + kCC, std::min(gy0 + kCC, planet_h), float(gx0 * cell),
                                    float(gy0 * cell) * Camera::kY, mpx,
                                    [&](int gx, int gy, std::int32_t& e, std::int32_t& sl) {
                                        const CellView c = planet_cell(gx, gy);
                                        e = c.elevation; sl = c.water ? 0 : c.slope;
                                    });
                    SDL_SetRenderTarget(ren, prev);
                    fc.dirty = false;
                }
                const float sx = cam.sx(float(cxi * CU));
                if (fc.t) {                                 // stale-but-close until repainted
                    SDL_FRect d{sx, cam.sy(float(cy * CU)) - float(mpx) / kFS * cam.zoom, float(TW) / kFS * cam.zoom,
                                float(TH) / kFS * cam.zoom};
                    SDL_RenderTexture(ren, fc.t, nullptr, &d);
                } else {                                   // not painted yet: plain haze
                    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
                    SDL_SetRenderDrawColor(ren, 18, 15, 12, 0xD8);
                    SDL_FRect d{sx, cam.sy(float(cy * CU)), float(CU) * cam.zoom + 1, float(CU) * Camera::kY * cam.zoom + 1};
                    SDL_RenderFillRect(ren, &d);
                }
            }
        }
        while (fchunks.size() > 160) {                     // ~60 KB each
            auto old = fchunks.end();
            for (auto it = fchunks.begin(); it != fchunks.end(); ++it)
                if (old == fchunks.end() || it->second.used < old->second.used) old = it;
            if (old->second.t) SDL_DestroyTexture(old->second.t);
            fchunks.erase(old);
        }
    }



    void Viewer::draw_planet_fog() {
        if (fog.mode() == FogOfWar::Mode::GodEye) return;
        const TerrainGrid& t = w.terrain;
        if (fog_tex) {
            const float rx0 = cam.sx(float(t.ox * t.cell)), ry0 = cam.sy(float(t.oy * t.cell));
            SDL_FRect fd{rx0, ry0, float(t.w * t.cell) * cam.zoom, float(t.h * t.cell) * Camera::kY * cam.zoom};
            SDL_RenderTexture(ren, fog_tex, nullptr, &fd);
            SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(ren, 18, 15, 12, 0xC8);
            const float W = float(cam.sw), H = float(cam.sh);
            const float rx1 = rx0 + fd.w, ry1 = ry0 + fd.h;
            SDL_FRect bands[4] = {{0, 0, W, std::max(0.f, ry0)},
                                  {0, std::max(0.f, ry1), W, std::max(0.f, H - ry1)},
                                  {0, std::max(0.f, ry0), std::max(0.f, rx0), std::max(0.f, std::min(H, ry1) - std::max(0.f, ry0))},
                                  {std::max(0.f, rx1), std::max(0.f, ry0), std::max(0.f, W - rx1), std::max(0.f, std::min(H, ry1) - std::max(0.f, ry0))}};
            for (const SDL_FRect& b : bands) if (b.w > 0 && b.h > 0) SDL_RenderFillRect(ren, &b);
        }
    }



    // mini-globe in the minimap corner, following the camera
    void Viewer::update_mini_globe() {
        if (!is_planet()) return;
        const int S = 170;
        if (!mini_globe_tex) {
            mini_globe_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, S, S);
            SDL_SetTextureBlendMode(mini_globe_tex, SDL_BLENDMODE_BLEND);
        }
        mini_px.assign(std::size_t(S) * S, 0x00000000u);
        GlobeView v;
        lonlat_of(cam.cx, cam.cy, v.lon, v.lat);
        v.lat = std::clamp(v.lat, -1.3, 1.3);
        v.radius = S * 0.44;
        grend_mini.render(pmap, v, sun_lon(), fog.mode() == FogOfWar::Mode::GodEye ? nullptr : globe_fog.data(),
                          mini_px.data(), S, S, S / 2, S / 2);
        SDL_UpdateTexture(mini_globe_tex, nullptr, mini_px.data(), S * 4);
    }


    void Viewer::ui_mini_globe() {
        if (!mini_globe_tex) return;
        const float S = 170.f;
        ImGui::SetNextWindowPos(ImVec2(float(win_w) - 10, float(win_h) - 10), ImGuiCond_Always, ImVec2(1, 1));
        ImGui::Begin("##planet", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                          ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        const ImVec2 o = ImGui::GetCursorScreenPos();
        ImGui::Image(ImTextureRef((ImTextureID)(intptr_t)mini_globe_tex), ImVec2(S, S));
        // view frame at the centre (camera footprint as an angle on the sphere)
        const float half_w = float(cam.sw * 0.5 / cam.zoom / planet_units_w() * 2 * kPI * S * 0.44);
        const float half_h = float(cam.sh * 0.5 / cam.zoom / Camera::kY / planet_units_h() * kPI * S * 0.44);
        const ImVec2 c(o.x + S * 0.5f, o.y + S * 0.5f);
        ImGui::GetWindowDrawList()->AddRect(ImVec2(c.x - std::max(2.f, half_w), c.y - std::max(2.f, half_h)),
                                           ImVec2(c.x + std::max(2.f, half_w), c.y + std::max(2.f, half_h)), IM_COL32(255, 255, 255, 230), 0, 0, 1.5f);
        if (ImGui::IsItemClicked()) {
            const ImVec2 m = ImGui::GetMousePos();
            GlobeView v; lonlat_of(cam.cx, cam.cy, v.lon, v.lat); v.lat = std::clamp(v.lat, -1.3, 1.3); v.radius = S * 0.44;
            double lo, la;
            if (GlobeRenderer::unproject(v, m.x - c.x, m.y - c.y, lo, la)) { pos_of(lo, la, cam.cx, cam.cy); wrap_camera(); }
        }
        if (ImGui::Button("Planet view (M)", ImVec2(S, 0))) enter_globe();
        ImGui::End();
    }



    // ---- terrain look from fields ---------------------------------------------
    MaterialId Viewer::terrain_material(const CellView& c) const {
        if (c.water) return w.materials.id_of("water");
        if (c.elevation > 430 || c.slope > 40) return w.materials.id_of("granite");
        if (c.moisture < 300) return w.materials.id_of("soil_sand");
        if (c.moisture > 750) return w.materials.id_of("soil_clay");
        return w.materials.id_of("soil_loam");
    }



    float Viewer::cell_elev(int gx, int gy) const {
        int lx, ly;
        if (!w.terrain.local_cell(gx, gy, lx, ly)) return 0;
        return float(w.terrain.elevation[std::size_t(ly) * w.terrain.w + lx]);
    }


    // (re)composite the whole map once; called when lens/day changes
    void Viewer::build_ground() {
        const TerrainGrid& t = w.terrain;
        if (t.empty()) return;
        const int cell = t.cell;
        ground_margin = int(600 * Camera::kElev * kGround) + 8;
        const int gw = t.w * cell * kGround, gh = int(t.h * cell * Camera::kY * kGround) + ground_margin + 8;
        if (!ground || gw != ground_w || gh != ground_h) {
            if (ground) SDL_DestroyTexture(ground);
            ground = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, gw, gh);
            SDL_SetTextureScaleMode(ground, SDL_SCALEMODE_NEAREST);
            ground_w = gw; ground_h = gh;
        }
        SDL_SetRenderTarget(ren, ground);
        // planet: transparent margins so far tiles show around the region;
        // valley: opaque copy (alpha blending the big blit costs ~2x in software)
        SDL_SetTextureBlendMode(ground, is_planet() ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
        if (is_planet()) SDL_SetRenderDrawColor(ren, 0, 0, 0, 0); else SDL_SetRenderDrawColor(ren, 18, 15, 12, 255);
        SDL_RenderClear(ren);
        const float ts = float(cell * kGround);
        for (int ly = 0; ly < t.h; ++ly)
            for (int lx = 0; lx < t.w; ++lx) {
                const std::size_t i = std::size_t(ly) * t.w + lx;
                CellView c;
                c.gx = lx + t.ox; c.gy = ly + t.oy;
                c.elevation = t.elevation[i]; c.moisture = t.moisture[i]; c.vegetation = t.vegetation[i];
                c.slope = t.slope[i]; c.water = t.water[i];
                c.wear = t.traffic_wear.size() == t.elevation.size() ? t.traffic_wear[i] : 0;
                const float x = float(lx * cell * kGround);
                const float y = (float(ly * cell) * Camera::kY - c.elevation * Camera::kElev) * kGround + ground_margin;
                SDL_FRect dst{x, y, ts, ts * Camera::kY + 1};
                if (const Sprite* sp = atlas.material(terrain_material(c))) {
                    SDL_Texture* tx = tex.get(*sp);
                    int r = 255, g = 255, b = 255;
                    if (!c.water) {
                        r = 255 - c.vegetation / 6; b = 255 - c.vegetation / 5;
                        const int wear = std::min(1000, c.wear);        // trails -> packed earth
                        r -= (r - 170) * wear / 1000; g -= (g - 140) * wear / 1000; b -= (b - 100) * wear / 1000;
                    }
                    SDL_SetTextureColorMod(tx, Uint8(std::clamp(r, 0, 255)), Uint8(std::clamp(g, 0, 255)), Uint8(std::clamp(b, 0, 255)));
                    SDL_RenderTexture(ren, tx, nullptr, &dst);
                    SDL_SetTextureColorMod(tx, 255, 255, 255);
                }
                if (c.slope > 12 && !c.water) {                         // cliff face
                    SDL_FRect face{x, y + ts * Camera::kY, ts, std::min(ts, c.slope * Camera::kElev * kGround)};
                    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
                    SDL_SetRenderDrawColor(ren, 40, 30, 22, 170);
                    SDL_RenderFillRect(ren, &face);
                }
                draw_cell_lens(c, dst);
            }
        SDL_SetRenderTarget(ren, world_rt);
    }


    // fog: 1 px per cell, alpha by tier, linear-filtered when stretched
    void Viewer::update_fog_tex() {
        const TerrainGrid& t = w.terrain;
        if (t.empty()) return;
        if (!fog_tex) {
            fog_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, t.w, t.h);
            SDL_SetTextureBlendMode(fog_tex, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(fog_tex, SDL_SCALEMODE_LINEAR);
        }
        std::vector<std::uint32_t> px(std::size_t(t.w) * t.h);
        for (int y = 0; y < t.h; ++y) for (int x = 0; x < t.w; ++x) {
            const Fog f = fog.cell_fog(x + t.ox, y + t.oy);
            px[std::size_t(y) * t.w + x] = f == Fog::Visible ? 0u : f == Fog::Shrouded ? 0x730A0A12u : 0xC8120F0Cu;   // unexplored: dark haze, relief faintly readable
        }
        SDL_UpdateTexture(fog_tex, nullptr, px.data(), t.w * 4);
    }


    void Viewer::draw_terrain() {
        if (!ground) return;                       // (re)built under the lock in frame()
        // visible slice of the ground texture -> whole screen (one blit)
        const float gox = float(w.terrain.ox * w.terrain.cell), goy = float(w.terrain.oy * w.terrain.cell);
        SDL_FRect src{(cam.cx - gox - cam.sw * 0.5f / cam.zoom) * kGround,
                      ((cam.cy - goy) * Camera::kY - cam.sh * 0.5f / cam.zoom) * kGround + ground_margin,
                      cam.sw / cam.zoom * kGround, cam.sh / cam.zoom * kGround};
        SDL_FRect dst{0, 0, float(cam.sw), float(cam.sh)};
        SDL_RenderTexture(ren, ground, &src, &dst);
        draw_fog_ground();                         // lifted with the terrain (Tribe & Avatar)
    }



    // ambient water: sparse glints drifting inside visible water cells,
    // phased by cell hash so the pattern is stable but shimmer moves.
    void Viewer::draw_water_glints() {
        if (snap.cells.empty()) return;
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        for (const CellView& c : snap.cells) {
            if (!c.water || c.fog == Fog::Unexplored) continue;
            if (((c.gx * 31 + c.gy * 17) & 3) != 0) continue;         // ~1/4 of cells
            const float ph = std::sin(float(time_s) * 1.7f + float((c.gx * 73 + c.gy * 41) % 628) / 100.f);
            if (ph < 0.25f) continue;
            const float a = (ph - 0.25f) * 0.9f;
            const float cx = cam.sx(float(c.gx * snap.cell)) +
                (0.5f + 0.3f * std::sin(float(time_s) * 0.6f + c.gx)) * snap.cell * cam.zoom;
            const float cy = cam.sy(float(c.gy * snap.cell)) +
                (0.5f + 0.3f * std::cos(float(time_s) * 0.5f + c.gy)) * snap.cell * Camera::kY * cam.zoom;
            SDL_SetRenderDrawColor(ren, 205, 232, 255, Uint8(80 * a));
            SDL_FRect g{cx, cy, 2.6f * cam.zoom, 1.3f * cam.zoom};
            SDL_RenderFillRect(ren, &g);
        }
    }



    void Viewer::draw_cell_lens(const CellView& c, const SDL_FRect& dst) {
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        if (lens == kEpidemic && c.water && c.moisture > 0) {
            SDL_SetRenderDrawColor(ren, 120, 220, 60, 40);
            SDL_RenderFillRect(ren, &dst);
        }
        if (lens == kEconomy && c.wear > 50) {            // goods flow along trails
            SDL_SetRenderDrawColor(ren, 230, 190, 60, Uint8(std::min(160, c.wear / 4)));
            SDL_RenderFillRect(ren, &dst);
        }
        if (lens == kMilitary && c.fog != Fog::Visible) {
            SDL_SetRenderDrawColor(ren, 60, 0, 0, 70);
            SDL_RenderFillRect(ren, &dst);
        }
        if (lens == kEcology) {
            // flora base (green) with a fauna accent (amber) — the census is
            // fog-filtered at capture, so shrouded land stays blank.
            if (c.flora > 0) {
                SDL_SetRenderDrawColor(ren, 40, 140, 50,
                                       Uint8(std::min(120, 30 + c.flora * 22)));
                SDL_RenderFillRect(ren, &dst);
            }
            if (c.fauna > 0) {
                SDL_SetRenderDrawColor(ren, 225, 140, 40,
                                       Uint8(std::min(150, 40 + c.fauna * 30)));
                SDL_RenderFillRect(ren, &dst);
            }
        }
    }

}  // namespace simlab_viewer
