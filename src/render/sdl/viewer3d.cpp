// 3D voxel "HD-2D" view mode (V key / View menu): stepped voxel terrain +
// cylindrical Y-axis billboards for every entity. STRICTLY client-side —
// reads snap/w.terrain under the same const snapshot rules as the 2D path
// and never mutates World state (render_only_hash_unchanged=1).
//
// SDL_Renderer is a 2D API: every 3D vertex is projected here into
// world_rt pixel space and rasterized via SDL_RenderGeometry. Painter's
// algorithm (chunks far->near, billboards far->near) approximates the
// depth ordering a z-buffer would give.
#include "viewer.hpp"

namespace simlab_viewer {

namespace {

struct F3 { float x = 0, y = 0, z = 0; };
F3 operator+(F3 a, F3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
F3 operator-(F3 a, F3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
F3 operator*(F3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot3(F3 a, F3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
F3 cross3(F3 a, F3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float len3(F3 a) { return std::sqrt(dot3(a, a)); }
F3 norm3(F3 a) { const float l = len3(a); return l > 1e-6f ? a * (1.f / l) : F3{0, 0, 0}; }

constexpr float kV3HScale = 0.06f;        // terrain elevation -> world Y
constexpr int   kV3ChunkCells = 32;       // terrain cells per chunk side
constexpr float kV3ViewRadius = 1350.f;   // cull distance (world units)
constexpr float kV3Fov = 1.05f;           // vertical fov ~60 deg

std::uint64_t v3_key(int cx, int cz) {
    return (std::uint64_t(std::uint32_t(cx)) << 32) | std::uint32_t(cz);
}

struct V3Cam {
    F3 pos, fwd, right, up;
    float fl = 1;                        // focal length in px
    float sw = 1, sh = 1;
};
V3Cam v3_cam(const Viewer& v) {
    V3Cam c;
    c.pos = {v.v3_x, v.v3_y, v.v3_z};
    c.sw = float(v.cam.sw); c.sh = float(v.cam.sh);
    const float cy = std::cos(v.v3_yaw), sy = std::sin(v.v3_yaw);
    const float cp = std::cos(v.v3_pitch), sp = std::sin(v.v3_pitch);
    c.fwd = norm3({sy * cp, sp, cy * cp});
    F3 r = cross3(c.fwd, {0, 1, 0});
    if (len3(r) < 1e-4f) r = {1, 0, 0};
    c.right = norm3(r);
    c.up = norm3(cross3(c.right, c.fwd));
    c.fl = c.sh * 0.5f / std::tan(kV3Fov * 0.5f);
    return c;
}

// world point -> world_rt px; false when behind the camera
bool v3_project(const V3Cam& c, F3 p, float& sx, float& sy) {
    const F3 d = p - c.pos;
    float zv = dot3(d, c.fwd);
    if (zv < 2.f) zv = 2.f;                    // clamp: avoids inf at the plane
    sx = c.sw * 0.5f + dot3(d, c.right) / zv * c.fl;
    sy = c.sh * 0.5f - dot3(d, c.up) / zv * c.fl;
    return dot3(d, c.fwd) > 0.5f;
}

// per-cell terrain colour (mirrors build_ground's palette, as RGB 0-255)
void v3_cell_rgb(const TerrainGrid& t, int lx, int ly, float& r, float& g, float& b) {
    const std::size_t i = std::size_t(ly) * t.w + lx;
    const std::int32_t el = t.elevation[i], mo = t.moisture[i],
                      ve = t.vegetation[i], sl = t.slope[i];
    if (t.water[i] != 0) { r = 66; g = 122; b = 205; return; }
    if (el > 430 || sl > 40) { r = 132; g = 126; b = 118; }        // granite
    else if (mo < 300) { r = 205; g = 178; b = 128; }              // sand
    else if (mo > 750) { r = 138; g = 100; b = 70; }               // clay
    else {                                                      // loam -> fern
        const float k = float(ve) / 1000.f;
        r = 104 - 40 * k; g = 142 - 30 * k; b = 72 - 18 * k;
    }
    if (sl > 12) { r = r * 0.6f + 60 * 0.4f; g = g * 0.6f + 56 * 0.4f; b = b * 0.6f + 52 * 0.4f; }
    const std::int32_t wear = t.traffic_wear.size() == t.elevation.size() ? t.traffic_wear[i] : 0;
    const float wk = std::min(1000, int(wear)) / 1000.f * 0.65f;    // trails -> packed earth
    r += (168 - r) * wk; g += (134 - g) * wk; b += (98 - b) * wk;
}

} // namespace

// terrain surface height (world Y) at map position (x, z == sim y)
float Viewer::v3_height(float x, float z) const {
    const TerrainGrid& t = w.terrain;
    if (t.empty()) return 0;
    const int cell = t.cell;
    const int gx = TerrainGrid::cell_of(std::int32_t(x), cell);
    const int gy = TerrainGrid::cell_of(std::int32_t(z), cell);
    int lx, ly;
    if (!t.local_cell(gx, gy, lx, ly)) return 0;
    return float(t.elevation[std::size_t(ly) * t.w + lx]) * kV3HScale;
}

// build (or rebuild) one 32x32-cell voxel chunk: top faces everywhere,
// side faces only where the neighbour is lower (hidden-face culling)
void Viewer::v3_build_chunk(int ccx, int ccz) {
    V3Chunk ch;
    const TerrainGrid& t = w.terrain;
    const int cell = t.cell > 0 ? t.cell : 16;
    const float cs = float(cell);
    auto H = [&](int gx, int gy) -> float {
        int lx, ly;
        if (!t.local_cell(gx, gy, lx, ly)) return 0.f;
        return float(t.elevation[std::size_t(ly) * t.w + lx]) * kV3HScale;
    };
    auto col = [&](int gx, int gy, float& r, float& g, float& b, float& a) {
        int lx, ly;
        if (!t.local_cell(gx, gy, lx, ly)) { r = 40; g = 36; b = 30; a = 255; return; }
        v3_cell_rgb(t, lx, ly, r, g, b);
        a = t.water[std::size_t(ly) * t.w + lx] ? 215.f : 255.f;
    };
    auto quad = [&](F3 p0, F3 p1, F3 p2, F3 p3,
                    float r, float g, float b, float a, float shd) {
        const std::uint32_t base = std::uint32_t(ch.v.size());
        const V3Vert vv[4] = {
            {p0.x, p0.y, p0.z, Uint8(std::clamp(r * shd, 0.f, 255.f)), Uint8(std::clamp(g * shd, 0.f, 255.f)), Uint8(std::clamp(b * shd, 0.f, 255.f)), Uint8(a)},
            {p1.x, p1.y, p1.z, Uint8(std::clamp(r * shd, 0.f, 255.f)), Uint8(std::clamp(g * shd, 0.f, 255.f)), Uint8(std::clamp(b * shd, 0.f, 255.f)), Uint8(a)},
            {p2.x, p2.y, p2.z, Uint8(std::clamp(r * shd, 0.f, 255.f)), Uint8(std::clamp(g * shd, 0.f, 255.f)), Uint8(std::clamp(b * shd, 0.f, 255.f)), Uint8(a)},
            {p3.x, p3.y, p3.z, Uint8(std::clamp(r * shd, 0.f, 255.f)), Uint8(std::clamp(g * shd, 0.f, 255.f)), Uint8(std::clamp(b * shd, 0.f, 255.f)), Uint8(a)},
        };
        for (const V3Vert& v : vv) ch.v.push_back(v);
        ch.i.push_back(int(base)); ch.i.push_back(int(base) + 1); ch.i.push_back(int(base) + 2);
        ch.i.push_back(int(base)); ch.i.push_back(int(base) + 2); ch.i.push_back(int(base) + 3);
    };
    for (int cy = 0; cy < kV3ChunkCells; ++cy)
        for (int cx = 0; cx < kV3ChunkCells; ++cx) {
            const int gx = ccx * kV3ChunkCells + cx, gy = ccz * kV3ChunkCells + cy;
            const float x0 = gx * cs, z0 = gy * cs, h = H(gx, gy);
            float r, g, b, a;
            col(gx, gy, r, g, b, a);
            quad({x0, h, z0}, {x0 + cs, h, z0}, {x0 + cs, h, z0 + cs}, {x0, h, z0 + cs},
                 r, g, b, a, 1.0f);
            struct Edge { int dx, dy; float shd; };
            const Edge edges[4] = {{1, 0, 0.80f}, {-1, 0, 0.80f}, {0, 1, 0.68f}, {0, -1, 0.68f}};
            for (const Edge& e : edges) {
                const float hn = H(gx + e.dx, gy + e.dy);
                if (h <= hn + 0.01f) continue;
                F3 p[4];
                if (e.dx == 1)      { const float x = x0 + cs; p[0]={x,hn,z0}; p[1]={x,h,z0}; p[2]={x,h,z0+cs}; p[3]={x,hn,z0+cs}; }
                else if (e.dx == -1){ const float x = x0;      p[0]={x,hn,z0+cs}; p[1]={x,h,z0+cs}; p[2]={x,h,z0}; p[3]={x,hn,z0}; }
                else if (e.dy == 1) { const float z = z0 + cs; p[0]={x0+cs,hn,z}; p[1]={x0+cs,h,z}; p[2]={x0,hn,z}; p[3]={x0,h,z}; }
                else                { const float z = z0;      p[0]={x0,hn,z}; p[1]={x0,h,z}; p[2]={x0+cs,hn,z}; p[3]={x0+cs,h,z}; }
                quad(p[0], p[1], p[2], p[3], r, g, b, a, e.shd);
            }
        }
    ch.t = time_s;
    v3_chunks[v3_key(ccx, ccz)] = std::move(ch);
}

void Viewer::toggle_view3d() {
    if (globe) {
        if (!is_planet()) return;
        enter_surface(gview.lon, gview.lat);
    }
    view3d = !view3d;
    if (view3d) {
        v3_saved_zoom = cam.zoom;
        v3_x = cam.cx; v3_z = cam.cy;
        v3_y = v3_height(v3_x, v3_z) + 300.f;
        if (v3_y < 120) v3_y = 120;
        v3_chunks.clear();
    } else {
        cam.cx = v3_x; cam.cy = v3_z; cam.zoom = v3_saved_zoom;
        v3_chunks.clear();
    }
    ui_dirty = true;
}

void Viewer::draw_3d(double dt) {
    const int cell = w.terrain.cell > 0 ? w.terrain.cell : 16;
    const float chunk_world = float(kV3ChunkCells * cell);

    // ---- camera flight (god view) / possess follow --------------------
    const bool* ks = SDL_GetKeyboardState(nullptr);
    const bool kb = ImGui::GetIO().WantCaptureKeyboard;
    if (!kb) {
        if (avatar == kNoEntity) {
            v3_av = kNoEntity;
            const float boost = (ks[SDL_SCANCODE_LSHIFT] || ks[SDL_SCANCODE_RSHIFT]) ? 3.f : 1.f;
            const float sp = v3_speed * boost * float(dt);
            const float sy = std::sin(v3_yaw), cyw = std::cos(v3_yaw);
            float mx = 0, mz = 0;
            if (ks[SDL_SCANCODE_W] || ks[SDL_SCANCODE_UP])    { mx += sy; mz += cyw; }
            if (ks[SDL_SCANCODE_S] || ks[SDL_SCANCODE_DOWN])  { mx -= sy; mz -= cyw; }
            if (ks[SDL_SCANCODE_D] || ks[SDL_SCANCODE_RIGHT]) { mx -= cyw; mz += sy; }
            if (ks[SDL_SCANCODE_A] || ks[SDL_SCANCODE_LEFT])  { mx += cyw; mz -= sy; }
            const float ml = std::hypot(mx, mz);
            if (ml > 1e-4f) { v3_x += mx / ml * sp; v3_z += mz / ml * sp; }
            if (ks[SDL_SCANCODE_SPACE]) v3_y += sp;
            if (ks[SDL_SCANCODE_C])     v3_y -= sp;
            if (ks[SDL_SCANCODE_Q]) v3_yaw += 1.8f * float(dt);
            if (ks[SDL_SCANCODE_E]) v3_yaw -= 1.8f * float(dt);
        } else {
            // third-person possess: Minecraft-style follow cam. Pitch eases
            // in only for the first ~1.2 s after possessing, then the player
            // owns it (RMB drag / Q / E). WASD steers camera-relative.
            if (avatar != v3_av) { v3_av = avatar; v3_blend = 0.f; }
            v3_blend += float(dt);
            const float tx = float(c_av_pos.x), tz = float(c_av_pos.y);
            const float gy = v3_height(tx, tz);
            const float sy = std::sin(v3_yaw), cyw = std::cos(v3_yaw);
            const float dx = tx - sy * 78.f, dz = tz - cyw * 78.f;
            const float k = std::min(1.f, float(dt) * 6.f);
            v3_x += (dx - v3_x) * k; v3_z += (dz - v3_z) * k;
            v3_y += ((gy + 46.f) - v3_y) * k;
            if (v3_blend < 1.2f)
                v3_pitch += (-0.38f - v3_pitch) * std::min(1.f, float(dt) * 3.f);
        }
        const float floor_y = v3_height(v3_x, v3_z) + 26.f;
        if (v3_y < floor_y) v3_y = floor_y;
        v3_pitch = std::clamp(v3_pitch, -1.48f, 1.48f);   // +-85 deg
    }

    // keep the 2D snapshot camera covering the 3D view (poll_world reads it)
    cam.cx = v3_x; cam.cy = v3_z;
    cam.zoom = cam.sw * 0.5f / 1300.f;

    const V3Cam cc = v3_cam(*this);

    // ---- terrain chunks: build missing, refresh stale, far->near --------
    const int pcx = int(std::floor(v3_x / chunk_world));
    const int pcz = int(std::floor(v3_z / chunk_world));
    const int reach = int(std::ceil(kV3ViewRadius / chunk_world)) + 1;
    struct ChunkRef { const V3Chunk* c; float d; };
    std::vector<ChunkRef> vis;
    int built = 0, refreshed = 0;
    for (int dz = -reach; dz <= reach; ++dz)
        for (int dx = -reach; dx <= reach; ++dx) {
            const int cx = pcx + dx, cz = pcz + dz;
            const float wx = (cx + 0.5f) * chunk_world, wz = (cz + 0.5f) * chunk_world;
            const float dd = std::hypot(wx - v3_x, wz - v3_z);
            if (dd > kV3ViewRadius + chunk_world * 0.75f) continue;
            const std::uint64_t key = v3_key(cx, cz);
            auto it = v3_chunks.find(key);
            if (it == v3_chunks.end()) {
                if (built < 3) { v3_build_chunk(cx, cz); ++built; }
                continue;                                    // next frame
            }
            if (time_s - it->second.t > 9.0 && refreshed < 2) {
                v3_build_chunk(cx, cz); ++refreshed;
                it = v3_chunks.find(key);
            }
            vis.push_back({&it->second, dd});
        }
    std::sort(vis.begin(), vis.end(), [](const ChunkRef& a, const ChunkRef& b) { return a.d > b.d; });

    // project chunk verts into a scratch SDL_Vertex buffer per chunk
    std::vector<SDL_Vertex> pv;
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    for (const ChunkRef& cr : vis) {
        if (cr.c->v.empty()) continue;
        pv.resize(cr.c->v.size());
        for (std::size_t i = 0; i < cr.c->v.size(); ++i) {
            const V3Vert& s = cr.c->v[i];
            float sx, sy;
            v3_project(cc, {s.x, s.y, s.z}, sx, sy);
            pv[i].position = {sx, sy};
            pv[i].color = {s.r / 255.f, s.g / 255.f, s.b / 255.f, s.a / 255.f};
            pv[i].tex_coord = {0, 0};
        }
        SDL_RenderGeometry(ren, nullptr, pv.data(), int(pv.size()),
                           const_cast<int*>(cr.c->i.data()), int(cr.c->i.size()));
    }

    // ---- entity billboards (cylindrical, Y-locked) ----------------------
    struct BB {
        float d = 0, w = 0, h = 0, gy = 0, ex = 0, ez = 0;
        float yoff = 0, xoff = 0, sx = 1, sy = 1;
        const EntityView* e = nullptr;
        const Sprite* sp = nullptr;
        bool flip = false;
    };
    std::vector<BB> bbs;
    bbs.reserve(snap.entities.size());
    const float syaw = std::sin(v3_yaw), cyaw = std::cos(v3_yaw);
    const F3 rh{-cyaw, 0.f, syaw};        // billboard right (faces camera horizontally)
    // smooth visual positions + gait phase — mirrors draw_entities' update so
    // NPCs keep walking/breathing/squash-stretching exactly like the 2D view
    for (const EntityView& e : snap.entities) {
        const float tx = float(e.pos.x), ty = float(e.pos.y);
        auto vit = vis_pos.find(e.id);
        if (vit == vis_pos.end()) vit = vis_pos.emplace(e.id, ImVec2(tx, ty)).first;
        else {
            const float ddx = tx - vit->second.x, ddy = ty - vit->second.y;
            if (ddx * ddx + ddy * ddy > 400.f * 400.f) vit->second = ImVec2(tx, ty);
            else {
                const float k = 1.f - std::exp(-float(dt) * 12.f);
                vit->second.x += ddx * k; vit->second.y += ddy * k;
            }
        }
        const auto pit = vis_prev.find(e.id);
        const ImVec2 pp = pit == vis_prev.end() ? vit->second : pit->second;
        const float mvx = vit->second.x - pp.x, mvy = vit->second.y - pp.y;
        vis_prev[e.id] = vit->second;
        const float spd = std::sqrt(mvx * mvx + mvy * mvy) / std::max(1e-3f, float(dt));
        const bool gliding = spd > 12.f;
        AnimState* an = nullptr;
        if (e.minded && e.alive) {
            auto ait = c_anim.find(e.id);
            if (ait != c_anim.end()) an = &ait->second;
        }
        const bool mourn_gait = an && an->mourning && an->expr_until > c_tick;
        float phase = 0.f;
        if (an) {
            an->phase += float(dt) * (gliding ? (mourn_gait ? 5.f : 9.f) : (mourn_gait ? 0.9f : 2.f));
            phase = an->phase;
        }
        const float ex = vit->second.x, ez = vit->second.y;
        const float dx = ex - v3_x, dz = ez - v3_z;
        if (dx * dx + dz * dz > kV3ViewRadius * kV3ViewRadius) continue;
        const Sprite* sp = nullptr;
        if (!e.species.empty() && e.flora && e.fruit_max > 0)
            sp = &atlas.fruiting(e.species, int(e.fruit), int(e.fruit_max));
        else if (!e.species.empty()) sp = atlas.species(e.species);
        else if (e.kind == "inscription") sp = &atlas.glyph_stone(e.lang_seed, e.text, false);
        else if (e.minded) sp = atlas.kind("person");
        else if (atlas.kind(e.kind)) sp = atlas.kind(e.kind);
        else if (e.material != kNoMaterial) sp = atlas.material_icon(e.material);
        else sp = atlas.kind("goods");
        if (!sp || sp->empty()) continue;
        const float gy = v3_height(ex, ez);
        const F3 base{ex, gy + 0.4f, ez};
        const float d = dot3(base - cc.pos, cc.fwd);
        if (d < 4.f) continue;
        // facing: flip when travelling screen-left (velocity . billboard right)
        const float cvx = mvx * (-cyaw) + mvy * syaw;
        bool flip = gliding && cvx < -0.4f;
        BB b;
        b.d = d; b.w = float(sp->w) * 0.85f; b.h = float(sp->h) * 0.85f; b.gy = gy;
        b.ex = ex; b.ez = ez; b.e = &e; b.sp = sp; b.flip = flip;
        // ---- procedural pose (same drivers as the 2D anim) --------------
        const bool lying = an && an->lying && an->expr_until > c_tick;
        const bool tantrum = an && float(time_s) < an->tantrum_until;
        const bool proud = an && an->proud > 0.5f && an->expr_until > c_tick;
        const bool mourn = an && an->mourning && an->expr_until > c_tick;
        if (gliding && e.alive) {                     // gait squash & stretch + bob
            const float sq = 1.f + 0.07f * std::sin(phase * 2.f);
            b.sy *= sq; b.sx /= sq;
            b.yoff += std::fabs(std::sin(phase)) * 1.7f;
        } else if (e.alive && an) {                   // idle breathing
            b.sy *= 1.f + 0.016f * std::sin(float(time_s) * 2.2f + float(e.id % 31));
        }
        if (lying) { b.sx *= 1.55f; b.sy *= 0.45f; b.yoff += b.h * 0.12f; }
        if (mourn) { b.sy *= 0.96f; }
        if (proud) { const float pp = 1.f + 0.035f * std::sin(float(time_s) * 5.f); b.sx *= pp; b.sy *= pp; }
        if (tantrum) b.yoff += std::fabs(std::sin(float(time_s) * 18.f + float(e.id % 5))) * 2.2f;
        if (an && an->tired > 0.85f)                  // critical hunger/thirst shiver
            b.xoff = std::sin(float(time_s) * 40.f + float(e.id)) * 0.9f;
        bbs.push_back(b);
    }
    std::sort(bbs.begin(), bbs.end(), [](const BB& a, const BB& b) { return a.d > b.d; });

    SDL_Vertex qv[4];
    int qi[6] = {0, 1, 2, 0, 2, 3};

    // contact shadows (flat dark quads on the block top)
    for (const BB& b : bbs) {
        const float ex = b.ex + b.xoff, ez = b.ez;
        const float gy = b.gy + 0.5f;
        const float hw = b.w * b.sx * 0.34f, hd = b.w * b.sx * 0.16f;
        const F3 pp[4] = {
            {ex - rh.x * hw - syaw * hd, gy, ez - rh.z * hw - cyaw * hd},
            {ex + rh.x * hw - syaw * hd, gy, ez + rh.z * hw - cyaw * hd},
            {ex + rh.x * hw + syaw * hd, gy, ez + rh.z * hw + cyaw * hd},
            {ex - rh.x * hw + syaw * hd, gy, ez - rh.z * hw + cyaw * hd},
        };
        bool ok = true;
        for (int i = 0; i < 4; ++i) {
            float sx, sy;
            ok &= v3_project(cc, pp[i], sx, sy);
            qv[i].position = {sx, sy};
            qv[i].color = {0.f, 0.f, 0.f, 0.32f};
            qv[i].tex_coord = {0, 0};
        }
        if (ok) SDL_RenderGeometry(ren, nullptr, qv, 4, qi, 6);
    }

    // billboards: one textured quad each, sorted back-to-front
    for (const BB& b : bbs) {
        const float ex = b.ex + b.xoff, ez = b.ez;
        const float hw = b.w * b.sx * 0.5f;
        const float bot = b.gy + 0.4f + b.yoff, top = bot + b.h * b.sy;
        const F3 p[4] = {
            {ex - rh.x * hw, bot, ez - rh.z * hw},
            {ex + rh.x * hw, bot, ez + rh.z * hw},
            {ex + rh.x * hw, top, ez + rh.z * hw},
            {ex - rh.x * hw, top, ez - rh.z * hw},
        };
        bool ok = true;
        for (int i = 0; i < 4; ++i) {
            float sx, sy;
            ok &= v3_project(cc, p[i], sx, sy);
            qv[i].position = {sx, sy};
            qv[i].color = {1.f, 1.f, 1.f, 1.f};
            qv[i].tex_coord = SDL_FPoint{i == 1 || i == 2 ? 1.f : 0.f, i >= 2 ? 0.f : 1.f};
        }
        if (!ok) continue;
        SDL_Texture* tx = tex.get(*b.sp, b.flip);
        int mr = 255, mg = 255, mb = 255;
        if (b.e->fog == Fog::Shrouded) { mr = 120; mg = 120; mb = 135; }
        else if (!b.e->alive) { mr = 150; mg = 140; mb = 130; }
        SDL_SetTextureColorMod(tx, Uint8(mr), Uint8(mg), Uint8(mb));
        SDL_RenderGeometry(ren, tx, qv, 4, qi, 6);
        SDL_SetTextureColorMod(tx, 255, 255, 255);
    }

    // ---- day/night wash --------------------------------------------------
    const int hr = int(c_tick % 24);
    float daylight = 0.f;
    if (hr >= 6 && hr <= 18) daylight = std::sin((hr - 6) / 12.f * 3.14159265f);
    const int dark = int((1.f - daylight) * 95.f);
    if (dark > 4) {
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren, 8, 12, 42, dark);
        SDL_RenderFillRect(ren, nullptr);
    }
}

// ray from mouse px -> world; hit = nearest billboard sphere, else terrain
Vec2 Viewer::v3_pick(float mx, float my, EntityId* hit) {
    const V3Cam cc = v3_cam(*this);
    const float tanv = std::tan(kV3Fov * 0.5f);
    const float aspect = cc.sw / cc.sh;
    const float ax = (mx / cc.sw * 2.f - 1.f) * tanv * aspect;
    const float ay = (1.f - my / cc.sh * 2.f) * tanv;
    F3 rd = norm3(cc.fwd + cc.right * ax + cc.up * ay);

    // 1) billboards: ray vs bounding sphere (cheap, stable picking)
    float best_t = 1e9f; EntityId best_id = kNoEntity;
    for (const EntityView& e : snap.entities) {
        const float ex = float(e.pos.x), ez = float(e.pos.y);
        const float gy = v3_height(ex, ez);
        const Sprite* sp = e.minded ? atlas.kind("person")
                       : !e.species.empty() ? atlas.species(e.species)
                       : atlas.kind(e.kind);
        if (!sp) continue;
        const float rad = std::max(float(sp->w), float(sp->h)) * 0.55f;
        const F3 ctr{ex, gy + float(sp->h) * 0.45f, ez};
        const F3 oc = ctr - cc.pos;
        const float tca = dot3(oc, rd);
        if (tca < 0 || tca > 4000) continue;
        const float d2 = dot3(oc, oc) - tca * tca;
        if (d2 > rad * rad) continue;
        const float t = tca - std::sqrt(rad * rad - d2);
        if (t > 0 && t < best_t) { best_t = t; best_id = e.id; }
    }

    // 2) terrain heightfield march; nearer of billboard vs terrain wins
    float t = 0.f;
    for (; t < 4000.f; t += 12.f) {
        const F3 p = cc.pos + rd * t;
        if (p.y <= v3_height(p.x, p.z) + 1.5f) break;
    }
    float lo = std::max(0.f, t - 12.f), hi = t;
    for (int i = 0; i < 10; ++i) {
        const float mid = (lo + hi) * 0.5f;
        const F3 p = cc.pos + rd * mid;
        if (p.y <= v3_height(p.x, p.z) + 1.5f) hi = mid; else lo = mid;
    }
    const bool bb_win = best_id != kNoEntity && best_t < hi;
    if (hit) *hit = bb_win ? best_id : kNoEntity;
    if (bb_win) {
        for (const EntityView& e : snap.entities)
            if (e.id == best_id) return Vec2{std::int32_t(e.pos.x), std::int32_t(e.pos.y)};
    }
    const F3 p = cc.pos + rd * hi;
    return Vec2{std::int32_t(p.x), std::int32_t(p.z)};
}

}  // namespace simlab_viewer
