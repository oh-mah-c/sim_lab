// Phase 13/14 — sunnyworld_viewer: "Sunnyworld - Primordial Sandbox".
//
// The viewer owns a World built by Primordial Genesis (simlab/Genesis.hpp):
// a naked tick-0 world, founders with instincts only. The autonomous driver
// runs it with curiosity + concurrent rounds on. Everything it DRAWS comes
// from a RenderSnapshot (const World&) and the ProceduralAtlas (generated
// from data/*.txt — no image files). The player's only writes go through
// the same channels an agent uses:
//   * Whisper Omen  -> an `omen:<verb>[:<material>]` BELIEF in one mind
//                      (costs Focus; the agent may ignore it by personality)
//   * Possess Avatar -> World::interact() calls with that agent as actor and
//                      only the verbs its mind knows (driver skips it)
//
//   sunnyworld_viewer [--seed N] [--data DIR] [--selftest FRAMES]
//
// Keys: Esc menu, WASD/arrows pan, wheel zoom, 1..6 lenses, F fog mode,
// Space pause, +/- speed, left click inspect (possessed: click ground = walk,
// click thing = target), right drag pan, F5 quick-save.
#include <SDL3/SDL.h>
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "simlab/World.hpp"
#include "simlab/WorldGen.hpp"
#include "simlab/Driver.hpp"
#include "simlab/Cognition.hpp"
#include "simlab/Genesis.hpp"
#include "simlab/render/Atlas.hpp"
#include "simlab/render/Snapshot.hpp"
#include "simlab/render/Patron.hpp"
#include "simlab/render/Globe.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <atomic>
#include <list>
#include <map>
#include <set>
#include <mutex>
#include <thread>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace simlab;
using namespace simlab::render;

namespace {

// display-only short entity id (low 5 digits; full id in tooltips/inspector)
std::string sid(EntityId id) { return "#" + std::to_string(id % 100000); }

// ---------------------------------------------------------------------------
// texture cache: Sprite (stable address in the atlas maps) -> SDL_Texture
// ---------------------------------------------------------------------------
struct TexCache {
    SDL_Renderer* r = nullptr;
    std::unordered_map<const Sprite*, SDL_Texture*> map, flipped;
    SDL_Texture* make(const Sprite& s, bool mirror) {
        SDL_Texture* t = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, s.w, s.h);
        if (!t) return nullptr;
        if (mirror) {
            std::vector<std::uint32_t> px(s.px.size());
            for (int y = 0; y < s.h; ++y) for (int x = 0; x < s.w; ++x)
                px[std::size_t(y) * s.w + x] = s.px[std::size_t(y) * s.w + (s.w - 1 - x)];
            SDL_UpdateTexture(t, nullptr, px.data(), s.w * 4);
        } else {
            SDL_UpdateTexture(t, nullptr, s.px.data(), s.w * 4);
        }
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(t, SDL_SCALEMODE_NEAREST);
        return t;
    }
    // pre-mirrored textures: the software renderer's rotate/flip path is slow
    SDL_Texture* get(const Sprite& s, bool mirror = false) {
        auto& m = mirror ? flipped : map;
        auto it = m.find(&s);
        if (it != m.end()) return it->second;
        return m[&s] = make(s, mirror);
    }
    void clear() {
        for (auto* m : {&map, &flipped}) { for (auto& [k, t] : *m) if (t) SDL_DestroyTexture(t); m->clear(); }
    }
};

// ---------------------------------------------------------------------------
// camera: oblique 2.5D (~65deg): y compressed 0.72, elevation lifts tiles
// ---------------------------------------------------------------------------
struct Camera {
    float cx = 0, cy = 0, zoom = 0.9f;
    int sw = 1280, sh = 800;
    static constexpr float kY = 0.72f, kElev = 0.12f;
    float sx(float wx) const { return (wx - cx) * zoom + sw * 0.5f; }
    float sy(float wy, float elev = 0) const { return ((wy - cy) * kY - elev * kElev) * zoom + sh * 0.5f; }
    float wx(float px) const { return (px - sw * 0.5f) / zoom + cx; }
    float wy(float py) const { return (py - sh * 0.5f) / zoom / kY + cy; }
    ViewRect view() const {
        const float hw = sw * 0.5f / zoom + 48, hh = sh * 0.5f / zoom / kY + 96;
        return ViewRect{static_cast<std::int32_t>(cx - hw), static_cast<std::int32_t>(cy - hh),
                        static_cast<std::int32_t>(cx + hw), static_cast<std::int32_t>(cy + hh)};
    }
};

struct Particle { float x, y, vx, vy, life; std::uint32_t c; };

enum Lens { kNatural = 0, kEpidemic, kEconomy, kPolitics, kSacred, kMilitary };
const char* kLensName[] = {"1 Natural", "2 Epidemiology", "3 Economy", "4 Politics", "5 Sacred", "6 Military"};

std::size_t rss_kb() {
    std::ifstream f("/proc/self/status"); std::string l;
    while (std::getline(f, l)) if (l.rfind("VmRSS:", 0) == 0) return std::stoul(l.substr(6));
    return 0;
}

void set_color(SDL_Renderer* r, std::uint32_t c, int a = -1) {
    SDL_SetRenderDrawColor(r, (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, a < 0 ? (c >> 24) & 0xFF : a);
}

// dark-parchment ImGui theme
void apply_theme() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 3; s.FrameRounding = 3; s.WindowBorderSize = 1; s.GrabRounding = 3;
    // touch-friendly hit areas (Android later): bigger frames + extra slop
    s.FramePadding = ImVec2(8, 5); s.ItemSpacing = ImVec2(8, 6); s.TouchExtraPadding = ImVec2(4, 4);
    s.ScrollbarSize = 16; s.GrabMinSize = 14;
    ImVec4* c = s.Colors;
    // dark slate panels, parchment text
    c[ImGuiCol_WindowBg] = ImVec4(0.10f, 0.11f, 0.12f, 1.0f);   // opaque: the map never shows through a panel
    c[ImGuiCol_ChildBg] = ImVec4(0.13f, 0.14f, 0.15f, 1.0f);
    c[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.12f, 0.13f, 1.0f);
    c[ImGuiCol_Border] = ImVec4(0.55f, 0.40f, 0.20f, 0.8f);
    c[ImGuiCol_TitleBg] = ImVec4(0.20f, 0.15f, 0.10f, 1.0f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.35f, 0.24f, 0.12f, 1.0f);
    c[ImGuiCol_Text] = ImVec4(0.93f, 0.87f, 0.74f, 1.0f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.50f, 0.42f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.22f, 0.19f, 0.15f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.33f, 0.27f, 0.18f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.36f, 0.26f, 0.14f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.52f, 0.38f, 0.18f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.64f, 0.47f, 0.22f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.36f, 0.26f, 0.14f, 0.8f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.52f, 0.38f, 0.18f, 0.8f);
    c[ImGuiCol_Tab] = ImVec4(0.25f, 0.19f, 0.12f, 1.0f);
    c[ImGuiCol_TabSelected] = ImVec4(0.50f, 0.36f, 0.17f, 1.0f);
    c[ImGuiCol_CheckMark] = ImVec4(0.90f, 0.70f, 0.30f, 1.0f);
    c[ImGuiCol_SliderGrab] = ImVec4(0.80f, 0.60f, 0.25f, 1.0f);
}

// ---------------------------------------------------------------------------
// Viewer
// ---------------------------------------------------------------------------
struct Viewer {
    World& w;
    std::string data_dir = "data";
    PrimordialParams pp;               // last genesis parameters
    PrimordialWorld pw;                // homes / tribe ids (derived on load)
    int tribe_view = 0;                // index into pw.tribe_lang for Tribe vision
    // ---- game layer (viewer state, not World Truth) ----
    bool in_menu = true;
    char seed_buf[24] = "2026";
    int menu_grid = 2, menu_tribes = 2, menu_per = 6;
    std::string pref_dir;              // SDL_GetPrefPath("Sunnyworld","SimLab")
    std::string status_msg;
    PatronJournal journal;             // player knowledge + Resonance (saved as .journal)
    enum Tool { kToolNone = 0, kToolBeacon, kToolBlueprint, kToolFear };
    int tool = kToolNone;
    bool dragging = false;
    Vec2 drag_a{}, drag_b{};
    bool show_journal = false;
    int journal_tab_req = -1;
    // word editor (Rosetta): opened from a bubble, a stone or the journal
    bool open_word_popup = false;
    std::int64_t edit_lang = 0;
    std::string edit_word;
    char edit_buf[128] = {0};
    int edit_tag = 0;
    struct BubbleRect { float x0, y0, x1, y1; std::int64_t lang; std::string word; };
    std::vector<BubbleRect> bubble_rects;
    std::unordered_map<EntityId, Vec2> c_speaker_pos;
    std::set<std::int64_t> c_avatar_knows;   // entry phoneme words the avatar knows (lang<<32 ^ hash)
    std::map<std::string, std::string> c_avatar_gloss;   // word -> concept (avatar's own understanding)
    struct Sign { Vec2 p; std::int64_t r; bool site; };
    std::vector<Sign> c_signs;
    struct Legend {
        EntityId id; bool alive; std::string kind; long long age, verbs, verified, integ;
        std::vector<EntityId> parents, children; long long grandchildren;
        std::vector<std::string> scars; Vec2 pos;
    };
    std::vector<Legend> c_legends;
    std::vector<EntityId> c_successors;
    bool need_successor = false;
    EntityId fallen = kNoEntity;
    double move_cool = 0;
    Vec2 c_av_pos{};
    // player commands: queued by the UI, executed on the UI thread the next
    // time it holds the world lock (never waits for a running sim cycle)
    std::vector<std::function<void()>> cmds;
    // ---- planet / globe (viewer state) ----
    int planet_w = 0, planet_h = 0;
    std::uint64_t gen_seed = 0;
    PlanetMap pmap;
    PlanetTiles ptiles;
    GlobeRenderer grend, grend_mini;
    GlobeView gview;
    bool globe = false, globe_dive = false;
    double dive_lon = 0, dive_lat = 0, gview_sun_offset = 0;
    SDL_Texture* globe_tex = nullptr;
    int globe_tw = 0, globe_th = 0;
    std::vector<std::uint32_t> globe_px, stars;
    std::vector<std::uint8_t> globe_fog;
    std::uint64_t globe_fog_frame = 0;
    SDL_Texture* mini_globe_tex = nullptr;
    std::vector<std::uint32_t> mini_px;
    std::unordered_map<std::uint64_t, SDL_Texture*> tile_tex;
    std::list<std::uint64_t> tile_lru;
    struct GIcon { double lon = 0, lat = 0; int type = 0; std::int64_t lang = 0; float r = 0; std::string label; };
    std::vector<GIcon> c_gicons;
    std::map<SDL_FingerID, ImVec2> fingers;
    float pinch_d = 0;
    void post(std::function<void()> f) { cmds.push_back(std::move(f)); }
    std::vector<std::pair<std::int64_t, std::string>> c_stone_words;   // per stone: lang, words joined
    EntityId avatar = kNoEntity;       // possessed agent
    std::vector<EntityId> av_targets;  // last clicked things (front = newest)
    int whisper_verb = 0, whisper_mat = 0;
    std::vector<std::string> c_av_verbs;
    bool want_quit = false;
    std::string c_av_line;
    SDL_Window* win = nullptr;
    SDL_Renderer* ren = nullptr;
    ProceduralAtlas atlas;
    TexCache tex;
    Camera cam;
    FogOfWar fog;
    RenderSnapshot snap;
    int lens = kNatural;
    int speed = 1;                 // 0 pause, else cycles per second index
    double sim_accum = 0;
    DriverState dstate;
    EntityId selected = kNoEntity;
    std::vector<Particle> parts;
    std::unordered_map<EntityId, Vec2> last_pos;
    std::unordered_map<EntityId, float> facing, bob_until;
    std::unordered_map<EntityId, std::int64_t> last_integ, last_pain;
    bool show_atlas = false, show_codex = false, show_inspector = true;
    std::map<std::string, std::string> rosetta;     // player guesses: glyph token -> meaning
    SDL_Texture* glow = nullptr;
    Sprite glow_sprite;
    double time_s = 0;
    std::uint64_t frames = 0;
    // ground cache: the whole terrain pre-composited once into ONE target
    // texture (2 px per map unit); a frame blits the visible slice of it.
    static constexpr int kGround = 2;              // px per map unit
    SDL_Texture* ground = nullptr;
    int ground_w = 0, ground_h = 0, ground_margin = 0;
    std::uint64_t ground_key = ~0ull;
    // fog: one pixel per cell, stretched with LINEAR filtering -> soft edges
    SDL_Texture* fog_tex = nullptr;
    SDL_Texture* mini_tex = nullptr;
    // the WORLD is rasterized at 1/kPix resolution into world_rt and blown
    // up with nearest filtering (pixel-art look, 4x fewer software pixels);
    // ImGui draws at full window resolution on top.
    static constexpr int kPix = 2;
    SDL_Texture* world_rt = nullptr;
    // UI layer cache: ImGui runs EVERY frame (input, hit-testing) but its
    // software rasterization (glyph triangles) is the most expensive pass,
    // so the draw data is re-rasterized into ui_rt only on input or every
    // 4th frame; other frames just composite the cached layer.
    SDL_Texture* ui_rt = nullptr;
    bool ui_dirty = true;
    int win_w = 1280, win_h = 800;
    float ux(float x) const { return x * kPix; }      // world-target px -> window px
    // simulation runs on its own thread; the UI thread takes the lock only
    // to snapshot + read World for its windows (never to write it)
    std::mutex mx;
    std::atomic<bool> running{true};
    std::atomic<int> speed_atomic{1};
    std::thread sim_thread;
    std::string shot_path;          // --shot FILE.bmp: save the next composed frame
    std::atomic<bool> ui_wants{false};   // UI asks the sim thread to yield the lock
    // UI caches: refreshed ONLY while the UI thread holds the world lock;
    // every window renders from them, so a long sim cycle never stalls a frame
    std::uint64_t c_tick = 0; std::size_t c_minds = 0;
    struct InspCache {
        EntityId id = kNoEntity; bool found = false, has_mind = false;
        std::string head, mat, mind_head;
        std::vector<std::pair<std::string, std::string>> props;
        std::vector<std::string> body, discoveries, property;
        std::vector<std::pair<std::string, long long>> beliefs;
    } insp;
    struct Stone { EntityId id; std::int64_t lang; std::string text, treaty; };
    std::vector<Stone> c_stones;
    std::string c_codex;
    std::vector<std::string> c_chron;

    explicit Viewer(World& world) : w(world) {
        dstate.curiosity = true; dstate.concurrent = true;
    }

    std::int64_t tribe_lang() const {
        return pw.tribe_lang.empty() ? 0 : pw.tribe_lang[std::size_t(std::clamp<int>(tribe_view, 0, int(pw.tribe_lang.size()) - 1))];
    }
    // reset every viewer-side cache after the World was replaced
    void reset_view() {
        dstate = DriverState{}; dstate.curiosity = true; dstate.concurrent = true;
        fog = FogOfWar{};
        fog.set_mode(FogOfWar::Mode::Tribe, tribe_lang());
        selected = kNoEntity; avatar = kNoEntity; av_targets.clear();
        parts.clear(); last_pos.clear(); facing.clear(); bob_until.clear(); last_integ.clear(); last_pain.clear();
        ground_key = ~0ull;
        if (ground) { SDL_DestroyTexture(ground); ground = nullptr; }
        if (fog_tex) { SDL_DestroyTexture(fog_tex); fog_tex = nullptr; }
        if (mini_tex) { SDL_DestroyTexture(mini_tex); mini_tex = nullptr; }
        insp = InspCache{}; c_stones.clear(); c_codex.clear(); c_chron.clear();
        journal.bubbles.clear(); bubble_rects.clear(); c_signs.clear(); c_legends.clear();
        need_successor = false; tool = kToolNone; dragging = false;
        if (!pw.homes.empty()) { cam.cx = float(pw.homes[0].x); cam.cy = float(pw.homes[0].y); }
        ui_dirty = true;
    }
    // ---- New Primordial World (caller holds the lock / sim paused) ----
    void new_world(const PrimordialParams& p) {
        w = World(p.seed, data_dir);
        w.ledger.set_byte_budget(16u << 20);
        pp = p;
        pw = build_primordial(w, p);
        tribe_view = 0;
        journal = PatronJournal{};
        journal.tribe = tribe_lang();
        journal.res_tick = w.clock_.tick();
        reset_view();
        planet_setup();
        status_msg = "New primordial world, seed " + std::to_string(p.seed) +
                     (is_planet() ? "  (planet " + std::to_string(planet_w) + "x" + std::to_string(planet_h) + ", M = globe)" : "");
    }
    // tribes of a loaded world: distinct language_id among living minds
    void derive_tribes() {
        pw = PrimordialWorld{};
        const PropertyDef* ld = w.props.find_name("language_id");
        if (!ld) return;
        for (const auto& [id, m] : w.minds_) {
            (void)m;
            const Entity* e = w.find(id);
            if (!e || !e->alive) continue;
            const std::int64_t l = w.prop_int(*e, ld->id, 0);
            if (l <= 0) continue;
            if (std::find(pw.tribe_lang.begin(), pw.tribe_lang.end(), l) == pw.tribe_lang.end()) {
                pw.tribe_lang.push_back(l); pw.homes.push_back(e->pos);
            }
            pw.founders.push_back(id);
        }
    }
    std::string slot_path(int slot) const { return pref_dir + "slot" + std::to_string(slot) + ".sav"; }
    void save_slot(int slot) {
        std::string err;
        std::lock_guard<std::mutex> lk(mx);
        if (w.save(slot_path(slot), &err)) {
            std::ofstream m(slot_path(slot) + ".meta");
            m << pp.seed << ' ' << pp.grid << ' ' << int(pp.planet) << ' ' << tribe_view << '\n';
            journal.save(slot_path(slot) + ".journal");
            status_msg = "Saved slot " + std::to_string(slot) + " (tick " + std::to_string(w.clock_.tick()) + ")";
        } else status_msg = "Save failed: " + err;
        ui_dirty = true;
    }
    bool load_slot(int slot) {
        std::string err;
        std::lock_guard<std::mutex> lk(mx);
        World nw(1, data_dir);
        if (!nw.load(slot_path(slot), &err)) { status_msg = "Load failed: " + err; return false; }
        w = std::move(nw);
        w.ledger.set_byte_budget(16u << 20);
        derive_tribes();
        tribe_view = 0;
        int planet = 0;
        if (std::ifstream m(slot_path(slot) + ".meta"); m) m >> pp.seed >> pp.grid >> planet >> tribe_view;
        pp.planet = planet != 0;
        if (!journal.load(slot_path(slot) + ".journal")) {
            journal = PatronJournal{}; journal.scanned = w.ledger.next_id(); journal.res_tick = w.clock_.tick();
        }
        if (journal.tribe != 0)
            for (std::size_t i = 0; i < pw.tribe_lang.size(); ++i) if (pw.tribe_lang[i] == journal.tribe) tribe_view = int(i);
        journal.tribe = tribe_lang();
        reset_view();
        planet_setup();
        status_msg = "Loaded slot " + std::to_string(slot) + " (tick " + std::to_string(w.clock_.tick()) + ")";
        return true;
    }

    // ======================= planet: globe + far surface =======================
    // The planet beyond the materialized home region is ANALYTIC (pure field
    // functions of the worldgen seed): the globe samples it once into an
    // albedo map; the 2.5D surface pulls 32x32-cell colour tiles on demand.
    static constexpr double kPI = 3.14159265358979323846;
    bool is_planet() const { return planet_w > 0; }
    void planet_setup() {
        planet_w = planet_h = 0; globe = false; globe_dive = false;
        int pw_ = 0, ph_ = 0;
        for (auto& [k, t] : tile_tex) if (t) SDL_DestroyTexture(t);
        tile_tex.clear(); tile_lru.clear();
        if (!WorldGenerator::planet_dims(w.world_seed, pw_, ph_) || pw_ < 2 * ph_ - 1) return;   // valley
        planet_w = pw_; planet_h = ph_; gen_seed = w.world_seed;
        pmap.build(gen_seed, pw_, ph_, pw_ >= 4096 ? 2048 : 1024);
        ptiles.reset(gen_seed, pw_, ph_, 384);
        globe_fog.assign(std::size_t(pmap.aw) * pmap.ah, 0);
        globe_fog_frame = 0;
    }
    double planet_units_w() const { return double(planet_w) * w.terrain.cell; }
    double planet_units_h() const { return double(planet_h) * w.terrain.cell; }
    void lonlat_of(double x, double y, double& lon, double& lat) const {
        lon = std::remainder(x / planet_units_w() * 2 * kPI, 2 * kPI);
        lat = (0.5 - y / planet_units_h()) * kPI;
    }
    void pos_of(double lon, double lat, float& x, float& y) const {
        double u = std::fmod(lon / (2 * kPI), 1.0); if (u < 0) u += 1;
        x = float(u * planet_units_w());
        y = float((0.5 - lat / kPI) * planet_units_h());
    }
    void wrap_camera() {
        if (!is_planet()) return;
        const double W = planet_units_w(), H = planet_units_h(), c = w.terrain.cell;
        cam.cx = float(std::fmod(std::fmod(double(cam.cx), W) + W, W));
        cam.cy = float(std::clamp(double(cam.cy), 6 * c, H - 6 * c));       // the ice walls of the poles
    }
    void enter_globe() {
        if (!is_planet()) return;
        lonlat_of(cam.cx, cam.cy, gview.lon, gview.lat);
        gview.lat = std::clamp(gview.lat, -1.35, 1.35);
        gview.radius = std::max(cam.sw, cam.sh) * 1.2;
        globe = true; globe_dive = false; tool = kToolNone;
        status_msg = "Planet view - drag to spin, wheel/pinch to dive";
    }
    void enter_surface(double lon, double lat) {
        pos_of(lon, lat, cam.cx, cam.cy);
        cam.zoom = 0.3f;
        wrap_camera();
        globe = false; globe_dive = false;
        status_msg = "Surface - wheel out at max zoom (or M) to return to orbit";
    }
    void dive_to(double lon, double lat) { globe_dive = true; dive_lon = lon; dive_lat = lat; }
    static double ang_lerp(double a, double b, double t) { return a + std::remainder(b - a, 2 * 3.14159265358979323846) * t; }
    // wheel/pinch on the globe: zoom toward the point under (px, py) (world_rt px)
    void globe_zoom(double factor, float px, float py) {
        double lon = 0, lat = 0;
        const bool hit = GlobeRenderer::unproject(gview, px - cam.sw * 0.5, py - cam.sh * 0.5, lon, lat);
        gview.radius = std::clamp(gview.radius * factor, std::min(cam.sw, cam.sh) * 0.16, 1e5);
        if (factor > 1 && hit) {
            gview.lon = ang_lerp(gview.lon, lon, 0.22);
            gview.lat = std::clamp(gview.lat + (lat - gview.lat) * 0.22, -1.35, 1.35);
        }
        if (gview.radius > std::max(cam.sw, cam.sh) * 1.35) enter_surface(gview.lon, gview.lat);
    }
    void globe_step(double dt) {
        if (!globe_dive) return;
        gview.lon = ang_lerp(gview.lon, dive_lon, std::min(1.0, dt * 4));
        gview.lat += (dive_lat - gview.lat) * std::min(1.0, dt * 4);
        gview.radius *= 1.0 + dt * 2.6;
        if (gview.radius > std::max(cam.sw, cam.sh) * 1.35) enter_surface(dive_lon, dive_lat);
        ui_dirty = true;
    }
    // fog texels (under the lock): explored -> 1, visible -> 2
    void build_globe_fog() {
        if (!is_planet() || fog.mode() == FogOfWar::Mode::GodEye) return;
        std::fill(globe_fog.begin(), globe_fog.end(), 0);
        for (const auto& c : fog.explored_cells())
            globe_fog[std::size_t(pmap.row_of(c.second)) * pmap.aw + pmap.col_of(c.first)] = 1;
        for (const auto& c : fog.visible_cells())
            globe_fog[std::size_t(pmap.row_of(c.second)) * pmap.aw + pmap.col_of(c.first)] = 2;
    }
    double sun_lon() const {
        const double hour = double(c_tick % 24) + 0.5;
        return std::remainder(kPI - hour / 24.0 * 2 * kPI + gview_sun_offset, 2 * kPI);
    }
    void draw_globe() {
        const int W = cam.sw, H = cam.sh;
        if (!globe_tex || globe_tw != W || globe_th != H) {
            if (globe_tex) SDL_DestroyTexture(globe_tex);
            globe_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, W, H);
            SDL_SetTextureScaleMode(globe_tex, SDL_SCALEMODE_NEAREST);
            globe_tw = W; globe_th = H;
            stars.assign(std::size_t(W) * H, 0xFF05070Cu);
            std::uint32_t h = 0x9E3779B9u;
            for (int i = 0; i < W * H / 180; ++i) {          // deterministic starfield
                h ^= h << 13; h ^= h >> 17; h ^= h << 5;
                const std::uint32_t v = 90 + (h >> 24) % 150;
                stars[h % stars.size()] = 0xFF000000u | (v << 16) | (v << 8) | std::min<std::uint32_t>(255, v + 20);
            }
        }
        globe_px = stars;
        grend.render(pmap, gview, sun_lon(), fog.mode() == FogOfWar::Mode::GodEye ? nullptr : globe_fog.data(),
                     globe_px.data(), W, H, W / 2, H / 2);
        SDL_UpdateTexture(globe_tex, nullptr, globe_px.data(), W * 4);
        SDL_RenderTexture(ren, globe_tex, nullptr, nullptr);
    }
    // strategic icons on the sphere (cached under the lock, drawn in the UI layer)
    void build_globe_icons() {
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
    void draw_globe_icons() {
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
    void draw_far_terrain() {
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
    SDL_Texture* tile_texture(int cx, int cy) {
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
    // mini-globe in the minimap corner, following the camera
    void update_mini_globe() {
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
    void ui_mini_globe() {
        if (!mini_globe_tex) return;
        const float S = 170.f;
        ImGui::SetNextWindowPos(ImVec2(float(win_w) - S - 24, float(win_h) - S - 44), ImGuiCond_Always);
        ImGui::Begin("Planet", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove);
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
        if (ImGui::SmallButton("Globe (M)")) enter_globe();
        ImGui::End();
    }

    void resize_world_rt() {
        if (world_rt) SDL_DestroyTexture(world_rt);
        cam.sw = std::max(64, win_w / kPix); cam.sh = std::max(64, win_h / kPix);
        world_rt = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, cam.sw, cam.sh);
        SDL_SetTextureScaleMode(world_rt, SDL_SCALEMODE_NEAREST);
        if (ui_rt) SDL_DestroyTexture(ui_rt);
        ui_rt = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, win_w, win_h);
        // ImGui rasterized INTO this target with normal blending leaves
        // premultiplied colour; compositing it must not multiply alpha again
        SDL_SetTextureBlendMode(ui_rt, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
        ui_dirty = true;
    }
    void init_textures() {
        resize_world_rt();
        tex.r = ren;
        glow_sprite.w = glow_sprite.h = 64; glow_sprite.px.assign(64 * 64, 0);
        for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) {
            const float d = std::sqrt(float((x - 32) * (x - 32) + (y - 32) * (y - 32))) / 32.f;
            const int a = d >= 1 ? 0 : int(255 * (1 - d) * (1 - d));
            glow_sprite.px[y * 64 + x] = (std::uint32_t(a) << 24) | 0xFFFFFF;
        }
        glow = tex.get(glow_sprite);
        SDL_SetTextureBlendMode(glow, SDL_BLENDMODE_ADD);
        load_rosetta();
    }

    // ---- simulation pacing (the ONLY thing that advances the World) --------
    // Runs on sim_thread. Each cycle holds the world lock; the UI thread
    // only reads under the same lock, so it never sees a half-applied cycle.
    void sim_loop() {
        static const double kRate[] = {0, 2, 10, 40, 200};   // cycles / second
        auto last = std::chrono::steady_clock::now();
        double accum = 0;
        while (running) {
            const auto now = std::chrono::steady_clock::now();
            accum += std::chrono::duration<double>(now - last).count() * kRate[std::clamp(speed_atomic.load(), 0, 4)];
            last = now;
            if (accum >= 1.0) {
                { std::lock_guard<std::mutex> lk(mx); autonomous_cycle(w, &dstate); }
                accum = std::min(accum - 1.0, 4.0);       // never queue a backlog
                if (ui_wants.exchange(false))              // let the UI take one fresh look
                    std::this_thread::sleep_for(std::chrono::milliseconds(6));
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
            }
        }
    }
    // synchronous variant for the headless self-test
    void step_sim(double dt) {
        static const double kRate[] = {0, 2, 10, 40, 200};
        sim_accum += dt * kRate[std::clamp(speed, 0, 4)];
        while (sim_accum >= 1.0) { autonomous_cycle(w, &dstate); sim_accum -= 1.0; }
    }

    // ---- terrain look from fields ---------------------------------------------
    MaterialId terrain_material(const CellView& c) const {
        if (c.water) return w.materials.id_of("water");
        if (c.elevation > 430 || c.slope > 40) return w.materials.id_of("granite");
        if (c.moisture < 300) return w.materials.id_of("soil_sand");
        if (c.moisture > 750) return w.materials.id_of("soil_clay");
        return w.materials.id_of("soil_loam");
    }

    float cell_elev(int gx, int gy) const {
        int lx, ly;
        if (!w.terrain.local_cell(gx, gy, lx, ly)) return 0;
        return float(w.terrain.elevation[std::size_t(ly) * w.terrain.w + lx]);
    }
    // (re)composite the whole map once; called when lens/day changes
    void build_ground() {
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
    void update_fog_tex() {
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
    void draw_terrain() {
        if (!ground) return;                       // (re)built under the lock in frame()
        // visible slice of the ground texture -> whole screen (one blit)
        const float gox = float(w.terrain.ox * w.terrain.cell), goy = float(w.terrain.oy * w.terrain.cell);
        SDL_FRect src{(cam.cx - gox - cam.sw * 0.5f / cam.zoom) * kGround,
                      ((cam.cy - goy) * Camera::kY - cam.sh * 0.5f / cam.zoom) * kGround + ground_margin,
                      cam.sw / cam.zoom * kGround, cam.sh / cam.zoom * kGround};
        SDL_FRect dst{0, 0, float(cam.sw), float(cam.sh)};
        SDL_RenderTexture(ren, ground, &src, &dst);
        if (fog.mode() == FogOfWar::Mode::Tribe && fog_tex) {
            const TerrainGrid& t = w.terrain;
            const float x0 = cam.sx(float(t.ox * t.cell)), y0 = cam.sy(float(t.oy * t.cell));
            SDL_FRect fd{x0, y0, float(t.w * t.cell) * cam.zoom, float(t.h * t.cell) * Camera::kY * cam.zoom};
            SDL_RenderTexture(ren, fog_tex, nullptr, &fd);
        }
    }

    void draw_cell_lens(const CellView& c, const SDL_FRect& dst) {
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
    }

    float elev_at(Vec2 p) const {
        return w.terrain.contains(p) ? float(w.terrain.elev_at(p)) : 0.f;
    }

    void emit(float x, float y, std::uint32_t c, int n) {
        for (int i = 0; i < n && parts.size() < 600; ++i) {
            const float a = float((frames * 7 + i * 53) % 628) / 100.f;
            parts.push_back({x, y, std::cos(a) * 30, std::sin(a) * 30 - 40, 0.6f, c});
        }
    }

    // enclosure: a ring of auto-tiled 2.5D wall segments around the footprint
    void draw_enclosure(const EntityView& e) {
        const int cell = snap.cell;
        const int rc = std::max<int>(1, int(e.radius / cell));
        const int gx0 = TerrainGrid::cell_of(e.pos.x, cell), gy0 = TerrainGrid::cell_of(e.pos.y, cell);
        auto on_ring = [&](int dx, int dy) {
            const int d2 = dx * dx + dy * dy;
            return d2 <= rc * rc + rc && d2 >= (rc - 1) * (rc - 1) + (rc - 1);
        };
        const int crack = e.integrity >= 800 ? 0 : e.integrity >= 550 ? 1 : e.integrity >= 300 ? 2 : 3;
        const MaterialId mat = e.material != kNoMaterial ? e.material : w.materials.id_of("stone");
        for (int dy = -rc; dy <= rc; ++dy)
            for (int dx = -rc; dx <= rc; ++dx) {
                if (!on_ring(dx, dy)) continue;
                if (e.barrier == 0 && e.integrity < 300 && ((dx + dy) & 1)) continue;   // breached gaps
                std::uint8_t mask = 0;
                const int nb[8][2] = {{0,-1},{1,-1},{1,0},{1,1},{0,1},{-1,1},{-1,0},{-1,-1}};
                for (int k = 0; k < 8; ++k) if (on_ring(dx + nb[k][0], dy + nb[k][1])) mask |= 1 << k;
                const Sprite& s = atlas.wall(mask, crack, mat);
                const Vec2 wp{(gx0 + dx) * cell, (gy0 + dy) * cell};
                SDL_FRect dst{cam.sx(float(wp.x)), cam.sy(float(wp.y), elev_at(wp)) - cell * cam.zoom * 0.35f,
                              cell * cam.zoom, cell * cam.zoom};
                SDL_Texture* t = tex.get(s);
                if (e.fog == Fog::Shrouded) SDL_SetTextureColorMod(t, 120, 120, 135);
                SDL_RenderTexture(ren, t, nullptr, &dst);
                SDL_SetTextureColorMod(t, 255, 255, 255);
            }
        if (lens == kMilitary || lens == kSacred || selected == e.id) {
            // function label: inferred by the engine, not stored
            ImGui::GetForegroundDrawList()->AddText(ImVec2(ux(cam.sx(float(e.pos.x))) - 20, ux(cam.sy(float(e.pos.y), elev_at(e.pos))) - 28),
                e.sacred ? IM_COL32(255, 210, 120, 255) : IM_COL32(230, 220, 200, 255),
                (e.function + (lens == kMilitary ? " " + std::to_string(e.integrity) : std::string())).c_str());
        }
        if (e.sacred && e.fog == Fog::Visible) draw_glow(e.pos, 0xFFC060, 2.0f);
    }

    void draw_glow(Vec2 p, std::uint32_t rgb, float scale) {
        const float s = 64 * scale * cam.zoom;
        SDL_FRect d{cam.sx(float(p.x)) - s / 2, cam.sy(float(p.y), elev_at(p)) - s / 2, s, s};
        const int hour = snap.day_phase;
        const int night = (hour < 6 || hour >= 19) ? 255 : (hour < 8 || hour >= 17) ? 140 : 50;
        SDL_SetTextureColorMod(glow, (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
        SDL_SetTextureAlphaMod(glow, Uint8(night));
        SDL_RenderTexture(ren, glow, nullptr, &d);
    }

    void draw_entities(double dt) {
        for (const EntityView& e : snap.entities) {
            if (e.kind == "enclosure") { draw_enclosure(e); continue; }
            const float elev = elev_at(e.pos);
            const float fx = cam.sx(float(e.pos.x)), fy = cam.sy(float(e.pos.y), elev);
            // motion memory -> bob / flip (viewer-side, never World state)
            auto lp = last_pos.find(e.id);
            if (lp != last_pos.end() && (lp->second.x != e.pos.x || lp->second.y != e.pos.y)) {
                facing[e.id] = e.pos.x < lp->second.x ? -1.f : 1.f;
                bob_until[e.id] = float(time_s) + 0.8f;
            }
            last_pos[e.id] = e.pos;
            // damage feedback -> particles
            auto li = last_integ.find(e.id);
            if (li != last_integ.end() && e.integrity < li->second)
                emit(fx, fy - 12 * cam.zoom, e.flora ? 0xFF6A9A40 : 0xFFB01818, 8);
            last_integ[e.id] = e.integrity;
            auto lpn = last_pain.find(e.id);
            if (lpn != last_pain.end() && e.pain > lpn->second) emit(fx, fy - 16 * cam.zoom, 0xFFFFD060, 6);
            last_pain[e.id] = e.pain;

            const Sprite* sp = nullptr;
            if (!e.species.empty() && e.flora && e.fruit_max > 0)
                sp = &atlas.fruiting(e.species, int(e.fruit), int(e.fruit_max));   // real fruit dots
            else if (!e.species.empty()) sp = atlas.species(e.species);
            else if (e.kind == "inscription") sp = &atlas.glyph_stone(e.lang_seed, e.text, false);
            else if (e.minded) sp = atlas.kind("person");
            else if (atlas.kind(e.kind)) sp = atlas.kind(e.kind);
            else if (e.material != kNoMaterial) sp = atlas.material_icon(e.material);
            else sp = atlas.kind("goods");
            if (!sp || sp->empty()) continue;
            const bool creature = e.fauna || e.minded;
            float sw = sp->w * cam.zoom, sh = sp->h * cam.zoom;
            float squash = 1.f, ox = 0;
            const bool moving = bob_until.count(e.id) && bob_until[e.id] > time_s;
            if (creature && e.alive && moving) {               // squash & stretch hop
                const float ph = std::sin(float(time_s) * 12.f + float(e.id % 17));
                squash = 1.f + 0.08f * ph; sh *= squash; sw /= squash;
            }
            if (creature && (e.infected || e.toxin > 300 || e.temperature > 385))   // fever tremor
                ox = std::sin(float(time_s) * 40.f + float(e.id)) * 1.2f * cam.zoom;
            SDL_FRect dst{fx - sw / 2 + ox, fy - sh, sw, sh};
            // ground shadow, slanted by the sun
            const float sun = float((snap.day_phase - 12)) / 12.f;
            SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 70);
            SDL_FRect sh_r{fx - sw * 0.35f + sun * sw * 0.4f, fy - 2 * cam.zoom, sw * 0.7f, 3 * cam.zoom};
            SDL_RenderFillRect(ren, &sh_r);
            const bool flip = facing.count(e.id) && facing[e.id] < 0;
            SDL_Texture* t = tex.get(*sp, flip);                   // paper flip, pre-mirrored
            if (e.fog == Fog::Shrouded) SDL_SetTextureColorMod(t, 120, 120, 135);
            if (!e.alive) SDL_SetTextureColorMod(t, 150, 140, 130);
            const double tilt = !e.alive ? 90.0 : e.routed ? -12.0 : 0.0;
            auto blit = [&](SDL_Texture* tx, const SDL_FRect& d, double ang) {
                if (ang == 0.0) SDL_RenderTexture(ren, tx, nullptr, &d);      // fast path
                else SDL_RenderTextureRotated(ren, tx, nullptr, &d, ang, nullptr, SDL_FLIP_NONE);
            };
            blit(t, dst, tilt);
            SDL_SetTextureColorMod(t, 255, 255, 255);
            // equipment overlays from REAL props
            if (e.armor > 0) blit(tex.get(atlas.armor_overlay(e.armor, e.armor_mat), flip), dst, tilt);
            if (e.w_sharp || e.w_blunt || e.w_elastic) {
                SDL_Texture* wt = tex.get(atlas.weapon(e.w_sharp, e.w_blunt, e.w_elastic, e.w_coated, e.w_mat), flip);
                SDL_FRect wd{fx + (flip ? -sw * 0.6f : sw * 0.05f), fy - sh * 0.75f, sw * 0.6f, sh * 0.6f};
                blit(wt, wd, 0.0);
            }
            if (e.leader) {                                    // crown of authority
                SDL_SetRenderDrawColor(ren, 255, 210, 60, 255);
                for (int k = 0; k < 3; ++k) {
                    SDL_FRect cr{fx - 4 * cam.zoom + k * 3 * cam.zoom, fy - sh - 3 * cam.zoom, 2 * cam.zoom, 3 * cam.zoom};
                    SDL_RenderFillRect(ren, &cr);
                }
            }
            if (e.bleed > 0 && e.alive && (frames / 8) % 3 == 0)
                emit(fx, fy - sh * 0.5f, 0xFFA01010, 1);
            if (e.temperature >= 3000) draw_glow(e.pos, 0xFF8030, 1.2f);    // fire / forge
            if (e.epic && lens == kSacred) draw_glow(e.pos, 0xFFE090, 0.8f);
            draw_entity_lens(e, fx, fy, sw, sh);
            if (e.id == selected) {
                SDL_SetRenderDrawColor(ren, 255, 230, 120, 255);
                SDL_RenderRect(ren, &dst);
            }
        }
        // particles
        for (Particle& p : parts) {
            p.life -= float(dt); p.x += p.vx * float(dt); p.y += p.vy * float(dt); p.vy += 90 * float(dt);
            if (p.life <= 0) continue;
            SDL_SetRenderDrawColor(ren, (p.c >> 16) & 0xFF, (p.c >> 8) & 0xFF, p.c & 0xFF, Uint8(255 * std::min(1.f, p.life * 2)));
            SDL_FRect d{p.x, p.y, 2 * cam.zoom, 2 * cam.zoom};
            SDL_RenderFillRect(ren, &d);
        }
        parts.erase(std::remove_if(parts.begin(), parts.end(), [](const Particle& p) { return p.life <= 0; }), parts.end());
    }

    void draw_entity_lens(const EntityView& e, float fx, float fy, float sw, float sh) {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        fx = ux(fx); fy = ux(fy); sw = ux(sw); sh = ux(sh);
        if (lens == kEpidemic && (e.infected || e.toxin > 0)) {
            const float r = (e.infected ? 18.f : 10.f) * cam.zoom * kPix;
            dl->AddCircleFilled(ImVec2(fx, fy - sh / 2), r, IM_COL32(120, 230, 60, 60));
        }
        if (lens == kEconomy && e.wealth > 0)
            dl->AddText(ImVec2(fx - 8, fy - sh - 12), IM_COL32(240, 200, 80, 255), std::to_string(e.wealth).c_str());
        if (lens == kPolitics && e.grievance > 0)
            dl->AddCircleFilled(ImVec2(fx, fy - sh / 2), (6 + e.grievance / 60) * cam.zoom * kPix,
                                IM_COL32(220, 40, 30, int(40 + e.grievance / 8)));
        if (lens == kSacred && e.sacred_conv > 0)
            dl->AddCircle(ImVec2(fx, fy - sh / 2), 10 * cam.zoom * kPix, IM_COL32(255, 220, 120, int(e.sacred_conv / 4)));
        if (lens == kMilitary) {
            if (e.morale >= 0) {
                const float wbar = sw * float(e.morale) / 1000.f;
                dl->AddRectFilled(ImVec2(fx - sw / 2, fy - sh - 6), ImVec2(fx - sw / 2 + wbar, fy - sh - 3),
                                  e.routed ? IM_COL32(200, 40, 40, 255) : IM_COL32(80, 200, 80, 255));
            }
            if (e.launch_range > 0)
                dl->AddCircle(ImVec2(fx, fy), e.launch_range * cam.zoom * kPix, IM_COL32(255, 120, 60, 110), 48);
        }
    }

    void draw_links() {
        if (lens != kPolitics) return;
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        for (const LinkView& l : snap.links)
            dl->AddLine(ImVec2(ux(cam.sx(float(l.a.x))), ux(cam.sy(float(l.a.y), elev_at(l.a)) - 12 * cam.zoom)),
                        ImVec2(ux(cam.sx(float(l.b.x))), ux(cam.sy(float(l.b.y), elev_at(l.b)) - 20 * cam.zoom)),
                        IM_COL32(230, 190, 60, 150), 1.5f);
    }

    void draw_night() {
        const int h = snap.day_phase;
        int a = 0;
        if (h < 5 || h >= 21) a = 150; else if (h < 7) a = 150 - (h - 5) * 60; else if (h >= 19) a = (h - 18) * 60;
        if (a <= 0) return;
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren, 12, 16, 46, Uint8(std::min(170, a)));      // cold dark-fantasy tone
        SDL_FRect all{0, 0, float(cam.sw), float(cam.sh)};
        SDL_RenderFillRect(ren, &all);
    }

    // ---- UI -------------------------------------------------------------------------
    void ui_topbar() {
        ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_Always);
        ImGui::Begin("##top", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove);
        const char* sp[] = {"Pause", "1x", "5x", "20x", "100x"};
        for (int i = 0; i < 5; ++i) {
            if (i) ImGui::SameLine();
            if (ImGui::RadioButton(sp[i], speed == i)) { speed = i; speed_atomic = i; }
        }
        ImGui::SameLine(); ImGui::Text("| tick %llu  day %llu  %02d:00", (unsigned long long)c_tick,
                                       (unsigned long long)(c_tick / 24), snap.day_phase);
        for (int i = 0; i < 6; ++i) {
            if (i) ImGui::SameLine();
            if (ImGui::RadioButton(kLensName[i], lens == i)) lens = i;
        }
        bool tribe = fog.mode() == FogOfWar::Mode::Tribe;
        if (ImGui::Checkbox("Tribe vision", &tribe))
            fog.set_mode(tribe ? FogOfWar::Mode::Tribe : FogOfWar::Mode::GodEye, tribe_lang());
        if (pw.tribe_lang.size() > 1) {
            ImGui::SameLine(); ImGui::SetNextItemWidth(90);
            const std::string cur = "Tribe " + std::to_string(tribe_view + 1);
            if (ImGui::BeginCombo("##tribe", cur.c_str())) {
                for (int i = 0; i < int(pw.tribe_lang.size()); ++i)
                    if (ImGui::Selectable(("Tribe " + std::to_string(i + 1)).c_str(), i == tribe_view)) {
                        tribe_view = i;
                        if (fog.mode() == FogOfWar::Mode::Tribe) fog.set_mode(FogOfWar::Mode::Tribe, tribe_lang());
                        if (i < int(pw.homes.size())) { cam.cx = float(pw.homes[i].x); cam.cy = float(pw.homes[i].y); }
                    }
                ImGui::EndCombo();
            }
        }
        ImGui::SameLine(); ImGui::Checkbox("Inspector", &show_inspector);
        ImGui::SameLine(); ImGui::Checkbox("Atlas", &show_atlas);
        ImGui::SameLine(); ImGui::Checkbox("Chronicle & Rosetta", &show_codex);
        ImGui::Text("entities %zu (culled %zu)  FPS %.0f  RSS %zu KB  minds %zu", snap.entities.size(), snap.culled,
                    ImGui::GetIO().Framerate, rss_kb(), c_minds);
        if (ImGui::Button("Menu (Esc)")) { in_menu = true; speed = 0; speed_atomic = 0; }
        ImGui::SameLine(); if (ImGui::Button("Quick save (F5)")) save_slot(1);
        ImGui::SameLine(); if (ImGui::Button("Journal (J)")) show_journal = !show_journal;
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.55f, 0.75f, 0.95f, 1));
        ImGui::ProgressBar(float(journal.resonance) / float(std::max<std::int64_t>(1, journal.resonance_max)), ImVec2(170, 0),
                           ("Resonance " + std::to_string(journal.resonance)).c_str());
        ImGui::PopStyleColor();
        if (!status_msg.empty()) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1, 0.85f, 0.5f, 1), "%s", status_msg.c_str()); }
        ImGui::End();
    }

    // ---- title / pause overlay: New Primordial World, Load, Continue ----------
    void ui_menu() {
        const ImVec2 sz(460, 440);
        ImGui::SetNextWindowPos(ImVec2((win_w - sz.x) / 2, (win_h - sz.y) / 2), ImGuiCond_Always);
        ImGui::SetNextWindowSize(sz, ImGuiCond_Always);
        ImGui::Begin("Sunnyworld - Primordial Sandbox", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
        ImGui::TextWrapped("A naked world at tick 0: rivers, forests, beasts, ore and disease - and a few bands of "
                           "humans who know nothing but hunger, thirst, cold, fear and curiosity. No words, no tools, "
                           "no gods. Whatever they become, they discover.");
        ImGui::Separator();
        ImGui::TextUnformatted("New Primordial World");
        ImGui::SetNextItemWidth(160); ImGui::InputText("World seed", seed_buf, sizeof seed_buf, ImGuiInputTextFlags_CharsDecimal);
        ImGui::SameLine();
        if (ImGui::Button("Random")) {
            const std::uint64_t r = SDL_GetTicksNS() ^ (std::uint64_t(SDL_GetPerformanceCounter()) << 17);
            std::snprintf(seed_buf, sizeof seed_buf, "%llu", (unsigned long long)(r % 1000000000ull));
        }
        static const char* kSizes[] = {"Local Valley (64 x 64)", "Medium planet (1024 x 512)",
                                       "Large planet (2048 x 1024)", "Colossal planet (4096 x 2048)"};
        static const int kGrid[] = {64, 128, 128, 128};
        static const int kPW[] = {0, 1024, 2048, 4096};
        ImGui::SetNextItemWidth(260); ImGui::Combo("World", &menu_grid, kSizes, 4);
        if (menu_grid > 0) ImGui::TextDisabled("2:1 planet: ice poles, wet equator, oceans, mountain spines.\n"
                                               "Your bands live in a simulated home region; the rest sleeps until reached.");
        ImGui::SetNextItemWidth(160); ImGui::SliderInt("Starting tribes", &menu_tribes, 1, 4);
        ImGui::SetNextItemWidth(160); ImGui::SliderInt("Humans per tribe", &menu_per, 3, 12);
        if (ImGui::Button("Create world", ImVec2(160, 0))) {
            PrimordialParams p;
            p.seed = std::strtoull(seed_buf, nullptr, 10);
            const int mi = std::clamp(menu_grid, 0, 3);
            p.grid = kGrid[mi];
            p.planet = kPW[mi] > 0; p.planet_w = kPW[mi]; p.planet_h = kPW[mi] / 2;
            p.tribes = menu_tribes; p.per_tribe = menu_per;
            { std::lock_guard<std::mutex> lk(mx); new_world(p); }
            in_menu = false; speed = 1; speed_atomic = 1;
        }
        ImGui::Separator();
        ImGui::Text("Saved worlds  (%s)", pref_dir.c_str());
        for (int slot = 1; slot <= 5; ++slot) {
            std::error_code ec;
            const bool has = std::filesystem::exists(slot_path(slot), ec);
            ImGui::PushID(slot);
            ImGui::Text("Slot %d: %s", slot, has ? "saved" : "empty");
            ImGui::SameLine(150);
            if (ImGui::Button("Save here")) save_slot(slot);
            if (has) {
                ImGui::SameLine();
                if (ImGui::Button("Load")) if (load_slot(slot)) { in_menu = false; speed = 0; speed_atomic = 0; }
            }
            ImGui::PopID();
        }
        ImGui::Separator();
        if (ImGui::Button("Continue", ImVec2(120, 0))) { in_menu = false; if (!speed) { speed = 1; speed_atomic = 1; } }
        ImGui::SameLine();
        if (ImGui::Button("Quit", ImVec2(120, 0))) want_quit = true;
        if (!status_msg.empty()) ImGui::TextColored(ImVec4(1, 0.85f, 0.5f, 1), "%s", status_msg.c_str());
        ImGui::End();
    }

    // ---- player agency: Resonance powers + Possess Avatar ----------------------
    // Every power writes AGENT KNOWLEDGE only (beliefs / memory engrams of
    // tribe minds); the minds decide what to do with it.
    std::vector<EntityId> tribe_minds() const {
        std::vector<EntityId> out;
        const PropertyDef* ld = w.props.find_name("language_id");
        for (const auto& [id, m] : w.minds_) {
            (void)m;
            const Entity* e = w.find(id);
            if (!e || !e->alive || id == avatar) continue;
            if (ld && w.prop_int(*e, ld->id, 0) == tribe_lang()) out.push_back(id);
        }
        return out;
    }
    // the `n` tribe minds closest to p (deterministic: distance, id)
    std::vector<EntityId> nearest_tribe(Vec2 p, std::size_t n) const {
        std::vector<std::pair<std::int64_t, EntityId>> v;
        for (EntityId id : tribe_minds()) v.push_back({dist2(w.find(id)->pos, p), id});
        std::sort(v.begin(), v.end());
        std::vector<EntityId> out;
        for (std::size_t i = 0; i < v.size() && i < n; ++i) out.push_back(v[i].second);
        return out;
    }
    void do_whisper(EntityId id) {
        const auto& verbs = curiosity_verbs();
        if (verbs.empty()) return;
        if (!w.find_mind(id) || !w.find(id) || !w.find(id)->alive) return;
        if (!journal.spend(20)) { status_msg = "Not enough Resonance (20)"; return; }
        std::string key = std::string("omen:") + verbs[std::size_t(std::clamp<int>(whisper_verb, 0, int(verbs.size()) - 1))];
        if (whisper_mat > 0 && whisper_mat <= int(w.materials.defs().size()))
            key += ":" + std::to_string(w.materials.defs()[std::size_t(whisper_mat - 1)].id);
        Belief& b = w.mind(id).belief(key);
        b.confidence = std::min<std::int64_t>(1000, b.confidence + 700);
        b.last_tick = w.clock_.tick();
        status_msg = "Planted an idea: '" + key.substr(5) + "' - it may listen, or not";
    }
    void whisper(EntityId id) { post([=, this] { do_whisper(id); }); }
    // Instill Fear: every tribe mind that REMEMBERS the thing feels dread of it
    void do_instill_fear(EntityId target) {
        if (!w.find(target)) return;
        if (!journal.spend(15)) { status_msg = "Not enough Resonance (15)"; return; }
        int touched = 0;
        for (EntityId id : tribe_minds()) {
            AgentMind& m = w.mind(id);
            auto it = m.memory.find(target);
            if (it == m.memory.end()) continue;
            it->second.engram.emotional_charge = std::min<std::int64_t>(it->second.engram.emotional_charge, -800);
            it->second.engram.stability = std::max<std::int64_t>(it->second.engram.stability, 4000);
            ++touched;
        }
        status_msg = "A dread of " + sid(target) + " spreads through " + std::to_string(touched) + " minds";
        if (!touched) { journal.resonance += 15; status_msg = "Nobody of your people remembers that thing"; }
    }
    void instill_fear(EntityId target) { post([=, this] { do_instill_fear(target); }); }
    void do_place_beacon(Vec2 p) {
        if (!journal.spend(10)) { status_msg = "Not enough Resonance (10)"; return; }
        const auto scouts = nearest_tribe(p, 2);
        for (EntityId id : scouts) {
            Belief& b = w.mind(id).belief("beacon:" + std::to_string(p.x) + ":" + std::to_string(p.y));
            b.confidence = 800; b.last_tick = w.clock_.tick();
        }
        status_msg = "Beacon lit: " + std::to_string(scouts.size()) + " scouts feel drawn to it";
    }
    void place_beacon(Vec2 p) { post([=, this] { do_place_beacon(p); }); }
    void do_place_blueprint(Vec2 c, std::int64_t r) {
        if (!journal.spend(25)) { status_msg = "Not enough Resonance (25)"; return; }
        r = std::clamp<std::int64_t>(r, 48, 220);
        const auto builders = nearest_tribe(c, 3);
        for (EntityId id : builders) {
            Belief& b = w.mind(id).belief("site:" + std::to_string(c.x) + ":" + std::to_string(c.y) + ":" + std::to_string(r));
            b.confidence = 900; b.last_tick = w.clock_.tick();
        }
        status_msg = "Blueprint dreamt by " + std::to_string(builders.size()) + " builders (they still need loose stone/wood there)";
    }
    void place_blueprint(Vec2 c, std::int64_t r) { post([=, this] { do_place_blueprint(c, r); }); }
    void do_avatar_act(const std::string& verb) {
        const Entity* me = w.find(avatar);
        if (!me || !me->alive) { status_msg = "Avatar is gone"; return; }
        InteractionContext c;
        c.actor = avatar; c.parameters.action = verb; c.parameters.ticks = 1; c.parameters.force = 100000;
        const bool self_only = verb == "rest" || verb == "evacuate" || verb == "found";
        const bool pair = verb == "assemble" || verb == "combine";
        if (!self_only) {
            for (EntityId t : av_targets) {
                if (t == avatar || !w.find(t)) continue;
                c.targets.push_back(t);
                if (c.targets.size() >= (pair ? 2u : 1u)) break;
            }
            if (c.targets.empty()) { status_msg = "Pick a target first (click something)"; return; }
        }
        if (verb == "engrave") {   // carve the avatar's strongest verified knowledge
            if (const AgentMind* am = w.find_mind(avatar)) {
                std::int64_t best = -1;
                for (const auto& [k, rec] : am->discoveries)
                    if (rec.status == DiscoveryStatus::Verified && rec.evidence_for > best) { best = rec.evidence_for; c.parameters.note = k; }
            }
            if (c.parameters.note.empty()) { status_msg = "Nothing verified worth carving yet"; return; }
        }
        const InteractionResult r = w.interact(c);
        status_msg = verb + (r.completed ? ": done" : ": failed - " + r.failure);
    }
    void avatar_act(const std::string& verb) { post([v = verb, this] { do_avatar_act(v); }); }
    void do_avatar_move(Vec2 dest) {
        const Entity* me = w.find(avatar);
        if (!me || !me->alive) return;
        InteractionContext c;
        c.actor = avatar; c.parameters.action = "move"; c.parameters.ticks = 1;
        c.parameters.destination = dest; c.parameters.has_destination = true;
        const InteractionResult r = w.interact(c);
        status_msg = r.completed ? "walking" : "move failed - " + r.failure;
    }
    void avatar_move(Vec2 dest) { post([=, this] { do_avatar_move(dest); }); }
    void set_possess(EntityId id) { post([=, this] { do_set_possess(id); }); }
    void do_set_possess(EntityId id) {
        if (avatar != kNoEntity) dstate.possessed.erase(avatar);
        avatar = id; av_targets.clear();
        if (id != kNoEntity) {
            dstate.possessed.insert(id);
            fog.set_mode(FogOfWar::Mode::Avatar, std::int64_t(id));   // its eyes only
            need_successor = false;
        } else {
            fog.set_mode(FogOfWar::Mode::Tribe, tribe_lang());
        }
        status_msg = id != kNoEntity ? "Possessing " + sid(id) + " - you see only what it sees"
                                     : "Released to its own mind";
    }
    void open_word(std::int64_t lang, const std::string& word) {
        edit_lang = lang; edit_word = word; open_word_popup = true;
        const auto& jw = journal.lexicon[lang][word];
        std::snprintf(edit_buf, sizeof edit_buf, "%s", jw.guess.c_str());
        edit_tag = int(jw.tag);
    }
    void ui_player() {
        ImGui::SetNextWindowPos(ImVec2(8, float(win_h) - 300), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(380, 286), ImGuiCond_FirstUseEver);
        ImGui::Begin("Patron");
        if (avatar != kNoEntity) {
            ImGui::TextColored(ImVec4(0.6f, 1, 0.6f, 1), "Possessing %s", sid(avatar).c_str());
            ImGui::TextWrapped("%s", c_av_line.c_str());
            ImGui::TextWrapped("WASD walk  |  click thing: target (%zu)  |  1-9 verbs", av_targets.size());
            ImGui::TextUnformatted("What this body knows how to do:");
            int n = 0;
            for (const std::string& v : c_av_verbs) {
                if (v == "move") continue;
                if (n % 4) ImGui::SameLine();
                ++n;
                if (ImGui::Button((std::to_string(n) + " " + v).c_str(), ImVec2(86, 0))) avatar_act(v);
            }
            if (ImGui::Button("Release to its own mind")) set_possess(kNoEntity);
            ImGui::End();
            return;
        }
        // map tools
        ImGui::TextUnformatted("Map powers:");
        auto toolbtn = [&](const char* label, int t, const char* tip) {
            const bool on = tool == t;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.45f, 0.65f, 1));
            if (ImGui::Button(label)) tool = on ? kToolNone : t;
            if (on) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        };
        toolbtn("Beacon 10", kToolBeacon, "Click the map: two scouts feel drawn there (explore the dark).");
        ImGui::SameLine();
        toolbtn("Blueprint 25", kToolBlueprint, "Drag a circle: three builders dream of a shelter there.");
        ImGui::SameLine();
        toolbtn("Dread 15", kToolFear, "Click a thing: your people who remember it come to fear it.");
        if (tool != kToolNone) ImGui::TextColored(ImVec4(0.6f, 0.8f, 1, 1), "Tool armed - click the map (Esc/right click cancels)");
        ImGui::Separator();
        if (insp.found && insp.has_mind) {
            ImGui::Text("Selected mind %s", sid(selected).c_str());
            const auto& verbs = curiosity_verbs();
            ImGui::SetNextItemWidth(120);
            if (ImGui::BeginCombo("try", verbs.empty() ? "" : verbs[std::size_t(std::clamp<int>(whisper_verb, 0, int(verbs.size()) - 1))])) {
                for (int i = 0; i < int(verbs.size()); ++i) if (ImGui::Selectable(verbs[std::size_t(i)], i == whisper_verb)) whisper_verb = i;
                ImGui::EndCombo();
            }
            ImGui::SameLine(); ImGui::SetNextItemWidth(140);
            const auto& mats = w.materials.defs();
            const char* mname = whisper_mat > 0 && whisper_mat <= int(mats.size()) ? mats[std::size_t(whisper_mat - 1)].name.c_str() : "any thing";
            if (ImGui::BeginCombo("on", mname)) {
                if (ImGui::Selectable("any thing", whisper_mat == 0)) whisper_mat = 0;
                for (int i = 0; i < int(mats.size()); ++i)
                    if (ImGui::Selectable(mats[std::size_t(i)].name.c_str(), whisper_mat == i + 1)) whisper_mat = i + 1;
                ImGui::EndCombo();
            }
            ImGui::BeginDisabled(journal.resonance < 20);
            if (ImGui::Button("Plant Idea in a dream (20)")) whisper(selected);
            ImGui::EndDisabled();
            ImGui::TextWrapped("A hint, not a command: curiosity, fear and drive decide.");
            if (ImGui::Button("Possess")) set_possess(selected);
            ImGui::SameLine();
            const bool pinned = journal.pins.count(selected) != 0;
            if (ImGui::Button(pinned ? "Unpin legend" : "Pin as legend")) {
                if (pinned) journal.pins.erase(selected); else journal.pins.insert(selected);
            }
        } else if (insp.found) {
            ImGui::Text("Selected thing %s", sid(selected).c_str());
            ImGui::BeginDisabled(journal.resonance < 15);
            if (ImGui::Button("Instill dread of this (15)")) instill_fear(selected);
            ImGui::EndDisabled();
        } else {
            ImGui::TextWrapped("Select a mind to plant an idea, pin or possess it. "
                               "Resonance grows when your people live through great moments.");
        }
        ImGui::End();
    }

    // caches read under the world lock (signs, legends, avatar vocabulary)
    void build_patron_caches() {
        c_signs.clear();
        for (EntityId id : tribe_minds()) {
            const AgentMind* m = w.find_mind(id);
            if (!m) continue;
            for (const char* pre : {"beacon:", "site:"})
                for (auto it = m->beliefs.lower_bound(pre); it != m->beliefs.end() && it->first.rfind(pre, 0) == 0; ++it) {
                    const std::string rest = it->first.substr(std::strlen(pre));
                    long long x = 0, y = 0, r = 0;
                    const bool site = pre[0] == 's';
                    if ((site ? std::sscanf(rest.c_str(), "%lld:%lld:%lld", &x, &y, &r) == 3
                              : std::sscanf(rest.c_str(), "%lld:%lld", &x, &y) == 2)) {
                        const Vec2 p{std::int32_t(x), std::int32_t(y)};
                        bool dup = false;
                        for (const Sign& sg : c_signs) if (sg.p.x == p.x && sg.p.y == p.y) dup = true;
                        if (!dup) c_signs.push_back({p, r, site});
                    }
                }
        }
        c_avatar_gloss.clear();
        if (avatar != kNoEntity)
            if (const AgentMind* am = w.find_mind(avatar))
                for (auto it = am->beliefs.lower_bound("lex:"); it != am->beliefs.end() && it->first.rfind("lex:", 0) == 0; ++it) {
                    if (it->second.confidence < 500) continue;
                    const EntityId eid = EntityId(std::strtoull(it->first.c_str() + 4, nullptr, 10));
                    const Entity* le = w.find(eid);
                    if (!le) continue;
                    const PropId ck = SIMLAB_PID(w.props, "concept_key");
                    const Value* v = ck != kNoProp ? w.prop_of(*le, ck) : nullptr;
                    c_avatar_gloss[word_of_entry(w, eid)] = v && v->type == ValType::Text ? v->s : "?";
                }
        c_legends.clear();
        const PropId agep = SIMLAB_PID(w.props, "age"), integp = SIMLAB_PID(w.props, "integrity"),
                     rolep = SIMLAB_PID(w.props, "part_role"), sevp = SIMLAB_PID(w.props, "severed");
        for (EntityId id : journal.pins) {
            const Entity* e = w.find(id);
            if (!e) continue;
            Legend L{};
            L.id = id; L.alive = e->alive; L.kind = e->kind_name.str(); L.pos = e->pos;
            L.age = agep != kNoProp ? (long long)w.prop_int(*e, agep, 0) : 0;
            L.integ = integp != kNoProp ? (long long)w.prop_int(*e, integp, 0) : 0;
            if (const AgentMind* m = w.find_mind(id)) {
                L.verbs = (long long)m->known_actions.size();
                for (const auto& [k, rec] : m->discoveries) if (rec.status == DiscoveryStatus::Verified) ++L.verified;
            }
            for (const Relation& r : w.relations_of(id, "parent_of")) {
                if (r.b == id) L.parents.push_back(r.a);
                if (r.a == id) {
                    L.children.push_back(r.b);
                    for (const Relation& r2 : w.relations_of(r.b, "parent_of")) if (r2.a == r.b) ++L.grandchildren;
                }
            }
            for (const Relation& r : w.relations_of(id, "part_of")) {
                if (r.b != id) continue;          // part --part_of--> body
                const Entity* pt = w.find(r.a);
                if (!pt || rolep == kNoProp || !w.prop_of(*pt, rolep)) continue;
                const Value* rv = w.prop_of(*pt, rolep);
                const std::string role = rv && rv->type == ValType::Text ? rv->s : "part";
                if (sevp != kNoProp && w.prop_int(*pt, sevp, 0)) L.scars.push_back("lost " + role);
                else if (integp != kNoProp && w.prop_int(*pt, integp, 1000) < 1000)
                    L.scars.push_back(role + " " + std::to_string(w.prop_int(*pt, integp, 1000) / 10) + "%");
            }
            c_legends.push_back(std::move(L));
        }
    }

    // ---- successor choice after the avatar's death ---------------------------
    void ui_successor() {
        if (!need_successor) return;
        ImGui::SetNextWindowPos(ImVec2(win_w * 0.5f - 200, win_h * 0.3f), ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(ImVec2(400, 0), ImGuiCond_Appearing);
        ImGui::Begin("Your vessel has fallen", &need_successor, ImGuiWindowFlags_NoCollapse);
        ImGui::TextWrapped("#%llu is dead. The world goes on; if its deeds were great, its people may sing of them. "
                           "Choose who to walk in next, or watch as the Patron.", (unsigned long long)fallen);
        for (EntityId id : c_successors) {
            ImGui::PushID(int(id));
            if (ImGui::Button(("Become " + sid(id)).c_str())) { set_possess(id); need_successor = false; }
            ImGui::PopID();
        }
        if (ImGui::Button("Return to the Patron's view")) need_successor = false;
        ImGui::End();
    }

    // ---- Field Journal: Rosetta lexicon, chronicle, legends, notes ------------
    void ui_journal() {
        if (!show_journal) return;
        ImGui::SetNextWindowSize(ImVec2(620, 520), ImGuiCond_FirstUseEver);
        // parchment page
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.83f, 0.76f, 0.60f, 1));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.20f, 0.13f, 0.07f, 1));
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImVec4(0.42f, 0.33f, 0.22f, 1));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.92f, 0.86f, 0.72f, 1));
        ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, ImVec4(0.78f, 0.70f, 0.54f, 1));
        ImGui::PushStyleColor(ImGuiCol_TitleBg, ImVec4(0.55f, 0.42f, 0.25f, 1));
        ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0.62f, 0.47f, 0.27f, 1));
        ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(0.70f, 0.60f, 0.42f, 1));
        ImGui::PushStyleColor(ImGuiCol_TabHovered, ImVec4(0.92f, 0.84f, 0.64f, 1));
        ImGui::PushStyleColor(ImGuiCol_TabSelected, ImVec4(0.93f, 0.86f, 0.68f, 1));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.70f, 0.60f, 0.42f, 1));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.47f, 0.27f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.72f, 0.56f, 0.32f, 1));
        ImGui::Begin("Field Journal", &show_journal);
        auto tabflag = [&](int i) { return journal_tab_req == i ? ImGuiTabItemFlags_SetSelected : 0; };
        if (ImGui::BeginTabBar("jr")) {
            if (ImGui::BeginTabItem("Rosetta Lexicon", nullptr, tabflag(0))) {
                if (journal.lexicon.empty())
                    ImGui::TextWrapped("No words yet. Watch your people: when they speak or carve, the words land here. "
                                       "Click a word to write what you think it means.");
                for (const auto& [lang, words] : journal.lexicon) {
                    const bool own = lang == tribe_lang();
                    if (!ImGui::CollapsingHeader(("Tongue " + std::to_string(lang) + (own ? "  (your people)" : "") +
                                                  "  - " + std::to_string(words.size()) + " words").c_str(),
                                                 own ? ImGuiTreeNodeFlags_DefaultOpen : 0)) continue;
                    if (ImGui::BeginTable(("lx" + std::to_string(lang)).c_str(), 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                        ImGui::TableSetupColumn("word"); ImGui::TableSetupColumn("heard/carved");
                        ImGui::TableSetupColumn("your meaning"); ImGui::TableSetupColumn("tag"); ImGui::TableSetupColumn("");
                        ImGui::TableHeadersRow();
                        for (const auto& [wd, jw] : words) {
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn(); ImGui::TextUnformatted(wd.c_str());
                            ImGui::TableNextColumn(); ImGui::Text("%d / %d", jw.heard, jw.carved);
                            ImGui::TableNextColumn();
                            const bool ok = guess_confirmed(w, jw);
                            if (jw.guess.empty()) ImGui::TextDisabled("?");
                            else ImGui::TextColored(ok ? ImVec4(0.10f, 0.45f, 0.15f, 1) : ImVec4(0.45f, 0.30f, 0.05f, 1),
                                                    "%s %s", jw.guess.c_str(), ok ? "[ok]" : "[?]");
                            ImGui::TableNextColumn(); ImGui::TextUnformatted(word_tag_name(jw.tag));
                            ImGui::TableNextColumn();
                            ImGui::PushID((std::to_string(lang) + wd).c_str());
                            if (ImGui::SmallButton("edit")) open_word(lang, wd);
                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Chronicle", nullptr, tabflag(1))) {
                ImGui::TextDisabled("Firsts of this world. Click Go to fly to where it happened.");
                for (auto it = journal.milestones.rbegin(); it != journal.milestones.rend(); ++it) {
                    ImGui::PushID(it->key.c_str());
                    if (ImGui::SmallButton("Go")) {
                        cam.cx = float(it->pos.x); cam.cy = float(it->pos.y);
                        if (w.find(it->who)) selected = it->who;
                    }
                    ImGui::SameLine();
                    ImGui::Text("day %llu  %s  (%s, tongue %lld)", (unsigned long long)(it->tick / 24), it->title.c_str(),
                                sid(it->who).c_str(), (long long)it->lang);
                    ImGui::PopID();
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Legends", nullptr, tabflag(2))) {
                if (c_legends.empty()) ImGui::TextWrapped("Pin remarkable people (Patron panel) to follow their lives and lineage.");
                for (const Legend& L : c_legends) {
                    ImGui::PushID(int(L.id));
                    ImGui::Separator();
                    ImGui::Text("%s %s  %s  age %lld", sid(L.id).c_str(), L.kind.c_str(), L.alive ? "alive" : "DEAD", L.age);
                    ImGui::Text("knows %lld verbs, %lld verified truths, integrity %lld", L.verbs, L.verified, L.integ);
                    std::string par, ch;
                    for (EntityId p2 : L.parents) par += " " + sid(p2);
                    for (EntityId c2 : L.children) ch += " " + sid(c2);
                    ImGui::TextWrapped("parents:%s   children:%s   grandchildren: %lld", par.empty() ? " founders" : par.c_str(),
                                       ch.empty() ? " none" : ch.c_str(), L.grandchildren);
                    if (!L.scars.empty()) {
                        std::string sc; for (const std::string& x : L.scars) sc += x + "  ";
                        ImGui::TextWrapped("scars: %s", sc.c_str());
                    }
                    if (ImGui::SmallButton("Go")) { cam.cx = float(L.pos.x); cam.cy = float(L.pos.y); selected = L.id; }
                    ImGui::SameLine();
                    if (L.alive && ImGui::SmallButton("Possess")) set_possess(L.id);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Unpin")) journal.pins.erase(L.id);
                    for (EntityId c2 : L.children) {
                        ImGui::SameLine();
                        if (ImGui::SmallButton(("pin " + sid(c2)).c_str())) journal.pins.insert(c2);
                    }
                    ImGui::PopID();
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Notes")) {
                static char buf[8192];
                std::snprintf(buf, sizeof buf, "%s", journal.notes.c_str());
                if (ImGui::InputTextMultiline("##notes", buf, sizeof buf, ImVec2(-1, -1))) journal.notes = buf;
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        journal_tab_req = -1;
        ImGui::End();
        ImGui::PopStyleColor(13);
        ui_word_editor();
    }
    void ui_word_editor() {
        if (open_word_popup) { ImGui::OpenPopup("Decipher word"); open_word_popup = false; }
        ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("Decipher word", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            JournalWord& jw = journal.lexicon[edit_lang][edit_word];
            ImGui::Text("Tongue %lld:  \"%s\"", (long long)edit_lang, edit_word.c_str());
            ImGui::Text("heard %d times, seen carved %d times", jw.heard, jw.carved);
            ImGui::SetNextItemWidth(260);
            ImGui::InputText("meaning", edit_buf, sizeof edit_buf);
            static const char* kTags[] = {"-", "Noun", "Verb", "Danger", "Sacred"};
            ImGui::SetNextItemWidth(160); ImGui::Combo("tag", &edit_tag, kTags, 5);
            if (ImGui::Button("Save to journal", ImVec2(160, 0))) {
                jw.guess = edit_buf; jw.tag = WordTag(edit_tag);
                status_msg = std::string("\"") + edit_word + "\" = " + jw.guess +
                             (guess_confirmed(w, jw) ? "  [ok] confirmed by what you witnessed!" : "  [?] a guess");
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    // ---- overlays: speech bubbles with live translation, signs, drag ---------
    void draw_overlays() {
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        bubble_rects.clear();
        const std::uint64_t now = c_tick;
        auto scr = [&](Vec2 p) { return ImVec2(cam.sx(float(p.x)) * kPix, cam.sy(float(p.y), elev_at(p)) * kPix); };
        // signs (beacons / building sites) the tribe currently holds
        for (const Sign& sg : c_signs) {
            const ImVec2 c = scr(sg.p);
            if (sg.site) {
                dl->AddCircle(c, float(sg.r) * cam.zoom * kPix, IM_COL32(250, 210, 120, 200), 48, 2.0f);
                dl->AddText(ImVec2(c.x - 30, c.y - 8), IM_COL32(250, 220, 150, 230), "blueprint");
            } else {
                dl->AddCircleFilled(c, 6, IM_COL32(120, 200, 255, 220));
                dl->AddLine(ImVec2(c.x, c.y), ImVec2(c.x, c.y - 26), IM_COL32(120, 200, 255, 220), 2);
            }
        }
        if (dragging && tool == kToolBlueprint) {
            const ImVec2 c = scr(drag_a);
            const float r = std::sqrt(float(dist2(drag_a, drag_b)));
            dl->AddCircle(c, std::clamp(r, 48.f, 220.f) * cam.zoom * kPix, IM_COL32(255, 230, 140, 255), 48, 2.5f);
        }
        // bubbles: newest per speaker, ~40 ticks of life
        std::set<EntityId> drawn;
        for (auto it = journal.bubbles.rbegin(); it != journal.bubbles.rend(); ++it) {
            if (it->tick + 40 < now || drawn.count(it->speaker)) continue;
            const auto pi = c_speaker_pos.find(it->speaker);
            if (pi == c_speaker_pos.end()) continue;
            drawn.insert(it->speaker);
            ImVec2 a = scr(pi->second);
            a.y -= 44 * cam.zoom * kPix * 0.5f + 18;
            float x = a.x - 10;
            const float y = a.y - 30;
            // measure
            float wsum = 8;
            std::vector<std::pair<std::string, std::string>> parts;
            for (const std::string& wd : it->words) {
                std::string tr = journal_reading(w, journal, it->lang, wd);
                if (avatar != kNoEntity) {
                    const auto gi = c_avatar_gloss.find(wd);
                    if (gi != c_avatar_gloss.end()) tr = gi->second + " (you know)";
                }
                const float ww = std::max(ImGui::CalcTextSize(wd.c_str()).x, ImGui::CalcTextSize(tr.c_str()).x) + 10;
                parts.push_back({wd, tr});
                wsum += ww;
            }
            x = a.x - wsum * 0.5f;
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + wsum, y + 34), IM_COL32(238, 226, 196, 235), 6);
            dl->AddRect(ImVec2(x, y), ImVec2(x + wsum, y + 34), IM_COL32(90, 60, 30, 255), 6);
            dl->AddTriangleFilled(ImVec2(a.x - 5, y + 34), ImVec2(a.x + 5, y + 34), ImVec2(a.x, y + 42), IM_COL32(238, 226, 196, 235));
            float cx = x + 6;
            for (const auto& [wd, tr] : parts) {
                const float ww = std::max(ImGui::CalcTextSize(wd.c_str()).x, ImGui::CalcTextSize(tr.c_str()).x) + 10;
                dl->AddText(ImVec2(cx, y + 2), IM_COL32(60, 35, 15, 255), wd.c_str());
                if (!tr.empty())
                    dl->AddText(ImVec2(cx, y + 17), tr.find("[ok]") != std::string::npos || tr.find("(you know)") != std::string::npos
                                    ? IM_COL32(20, 110, 40, 255) : IM_COL32(150, 90, 20, 255), tr.c_str());
                else
                    dl->AddText(ImVec2(cx, y + 17), IM_COL32(140, 120, 100, 255), "?");
                bubble_rects.push_back({cx - 3, y, cx + ww - 3, y + 34, it->lang, wd});
                cx += ww;
            }
        }
    }

    void update_minimap() {
        const TerrainGrid& t = w.terrain;
        if (t.empty()) return;
        if (!mini_tex) {
            mini_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, t.w, t.h);
            SDL_SetTextureScaleMode(mini_tex, SDL_SCALEMODE_NEAREST);
        }
        std::vector<std::uint32_t> px(std::size_t(t.w) * t.h);
        for (int y = 0; y < t.h; ++y) for (int x = 0; x < t.w; ++x) {
            const std::size_t i = std::size_t(y) * t.w + x;
            const Fog f = fog.cell_fog(x + t.ox, y + t.oy);
            std::uint32_t c = f == Fog::Unexplored ? rgba(20, 17, 14)
                            : t.water[i] ? rgba(50, 90, 150)
                            : rgba(90 + t.elevation[i] / 6, 110 + t.vegetation[i] / 10, 60);
            if (f == Fog::Shrouded) c = shade(c, 550);
            px[i] = c;
        }
        for (const auto& [mid, m] : w.minds_) {
            (void)m;
            const Entity* e = w.find(mid);
            if (!e || !e->alive || fog.point_fog(w, e->pos) != Fog::Visible) continue;
            const int x = TerrainGrid::cell_of(e->pos.x, t.cell) - t.ox, y = TerrainGrid::cell_of(e->pos.y, t.cell) - t.oy;
            if (x >= 0 && y >= 0 && x < t.w && y < t.h) px[std::size_t(y) * t.w + x] = rgba(255, 230, 140);
        }
        SDL_UpdateTexture(mini_tex, nullptr, px.data(), t.w * 4);
    }
    void ui_minimap() {
        if (w.terrain.empty() || !mini_tex) return;
        const TerrainGrid& t = w.terrain;
        const float S = 180.f;
        ImGui::SetNextWindowPos(ImVec2(float(win_w) - S - 24, float(win_h) - S - 44), ImGuiCond_Always);
        ImGui::Begin("Minimap", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove);
        const ImVec2 o = ImGui::GetCursorScreenPos();
        ImGui::Image(ImTextureRef((ImTextureID)(intptr_t)mini_tex), ImVec2(S, S));
        const float k = S / float(std::max(t.w, t.h));
        const ViewRect v = cam.view();
        ImGui::GetWindowDrawList()->AddRect(ImVec2(o.x + v.x0 / float(t.cell) * k, o.y + v.y0 / float(t.cell) * k),
            ImVec2(o.x + v.x1 / float(t.cell) * k, o.y + v.y1 / float(t.cell) * k), IM_COL32(255, 255, 255, 200));
        if (ImGui::IsItemClicked()) {
            const ImVec2 m = ImGui::GetMousePos();
            cam.cx = (m.x - o.x) / k * t.cell; cam.cy = (m.y - o.y) / k * t.cell;
        }
        ImGui::End();
    }

    // ---- inspector: built under the lock, drawn from the cache ---------------
    void build_inspector() {
        insp = InspCache{};
        insp.id = selected;
        const Entity* e = w.find(selected);
        if (!e) return;
        insp.found = true;
        insp.head = "#" + std::to_string(e->id) + "  " + e->kind_name.str() + (e->alive ? "  alive" : "  DEAD");
        if (const MaterialDef* m = w.materials.find(e->material)) {
            const MaterialLook* L = atlas.look(m->id);
            insp.mat = "material " + m->name + " (" + (L ? mat_class_name(L->cls) : "?") + ")";
        }
        for (const auto& [pid, v] : e->props.entries) {
            const PropertyDef* d = w.props.find(pid);
            insp.props.emplace_back(d ? d->name : "?", value_str(v));
        }
        auto P = [&](const char* n) { const PropertyDef* d = w.props.find_name(n); return d ? d->id : kNoProp; };
        const PropertyDef* rd = w.props.find_name("part_role");
        for (const Relation& r : w.relations_of(e->id, "part_of")) {     // anatomy (Phase 10)
            if (r.b != e->id) continue;
            const Entity* p = w.find(r.a);
            if (!p) continue;
            const Value* role = rd ? w.prop_of(*p, rd->id) : nullptr;
            char buf[160];
            std::snprintf(buf, sizeof buf, "%s  integ %lld  bleed %lld  fracture %lld",
                role && role->type == ValType::Text ? role->s.c_str() : p->kind_name.c_str(),
                (long long)w.prop_int(*p, P("integrity"), 0), (long long)w.prop_int(*p, P("bleed_rate"), 0),
                (long long)w.prop_int(*p, P("fracture_level"), 0));
            insp.body.emplace_back(buf);
        }
        for (const Relation& r : w.relations_of(e->id, "carries"))
            if (r.a == e->id)
                if (const Entity* inf = w.find(r.b)) {
                    char buf[160];
                    std::snprintf(buf, sizeof buf, "infection: strain %lld load %lld antibody %lld",
                        (long long)w.prop_int(*inf, P("pathogen_strain"), 0),
                        (long long)w.prop_int(*inf, P("pathogen_load"), 0),
                        (long long)w.prop_int(*inf, P("antibody_level"), 0));
                    insp.body.emplace_back(buf);
                }
        if (const AgentMind* m = w.find_mind(e->id)) {
            insp.has_mind = true;
            insp.mind_head = "known actions " + std::to_string(m->known_actions.size()) + "  memories " +
                             std::to_string(m->memory.size()) + "  discoveries " + std::to_string(m->discoveries.size());
            for (const auto& [k, b] : m->beliefs) insp.beliefs.emplace_back(k, (long long)b.confidence);
            for (const auto& [dk, d] : m->discoveries)
                insp.discoveries.push_back(d.claim + "  trials " + std::to_string(d.trials));
        }
        const PropId poss = P("possessed_by");
        for (const auto& ep : w.entities)
            if (ep->alive && poss != kNoProp && w.prop_int(*ep, poss, 0) == std::int64_t(e->id)) {
                const MaterialDef* gm = w.materials.find(ep->material);
                insp.property.push_back((gm ? gm->name : ep->kind_name.str()) + " x" +
                    std::to_string(w.prop_int(*ep, P("quantity"), 1)) + "  (value " +
                    std::to_string(ep->material ? econ_total_value(w, e->id, ep->material) : 0) + ")");
            }
        const PropId debtor = P("debtor_id"), dv = P("debt_value");
        for (const EntityId did : w.entities_of_kind(SIMLAB_KIND("debt_obligation")))
            if (const Entity* d = w.find(did); d && d->alive && w.prop_int(*d, debtor, 0) == std::int64_t(e->id))
                insp.property.push_back("owes " + std::to_string(w.prop_int(*d, dv, 0)));
    }
    void ui_inspector() {
        if (!show_inspector) return;
        ImGui::SetNextWindowPos(ImVec2(float(win_w) - 380, 120), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(360, float(std::max(200, win_h - 120 - 260))), ImGuiCond_FirstUseEver);
        ImGui::Begin("Entity Inspector", &show_inspector);
        if (!insp.found) { ImGui::TextDisabled("Left-click anything on the map."); ImGui::End(); return; }
        ImGui::TextUnformatted(insp.head.c_str());
        if (!insp.mat.empty()) ImGui::TextUnformatted(insp.mat.c_str());
        if (ImGui::BeginTabBar("insp")) {
            if (ImGui::BeginTabItem("Body")) {
                if (ImGui::BeginTable("props", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 160))) {
                    for (const auto& [k, v] : insp.props) {
                        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(k.c_str());
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(v.c_str());
                    }
                    ImGui::EndTable();
                }
                for (const std::string& l : insp.body) ImGui::BulletText("%s", l.c_str());
                ImGui::EndTabItem();
            }
            if (insp.has_mind && ImGui::BeginTabItem("Mind")) {
                ImGui::TextUnformatted(insp.mind_head.c_str());
                if (ImGui::BeginTable("bel", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 260))) {
                    for (const auto& [k, c] : insp.beliefs) {
                        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(k.c_str());
                        ImGui::TableNextColumn(); ImGui::Text("%lld", c);
                    }
                    ImGui::EndTable();
                }
                for (const std::string& d : insp.discoveries) ImGui::BulletText("%s", d.c_str());
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Property & Law")) {
                for (const std::string& l : insp.property) ImGui::BulletText("%s", l.c_str());
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::End();
    }

    void ui_atlas() {
        if (!show_atlas) return;
        ImGui::SetNextWindowSize(ImVec2(620, 520), ImGuiCond_FirstUseEver);
        ImGui::Begin("Data Atlas Gallery (generated from data/*.txt)", &show_atlas);
        if (ImGui::BeginTabBar("atlas")) {
            if (ImGui::BeginTabItem("Materials")) {
                int i = 0;
                for (const MaterialDef& m : w.materials.defs()) {
                    const Sprite* t = atlas.material(m.id);
                    const Sprite* ic = atlas.material_icon(m.id);
                    if (!t || !ic) continue;
                    ImGui::BeginGroup();
                    ImGui::Image(ImTextureRef((ImTextureID)(intptr_t)tex.get(*t)), ImVec2(48, 48));
                    ImGui::SameLine(); ImGui::Image(ImTextureRef((ImTextureID)(intptr_t)tex.get(*ic)), ImVec2(36, 36));
                    const MaterialLook* L = atlas.look(m.id);
                    ImGui::Text("%s", m.name.c_str());
                    ImGui::TextDisabled("%s%s%s", L ? mat_class_name(L->cls) : "", L && L->toxic ? " toxic" : "",
                                        L && L->sheen ? " sheen" : "");
                    ImGui::EndGroup();
                    if (ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip();
                        for (const auto& [pid, v] : m.props)
                            if (const PropertyDef* d = w.props.find(pid)) ImGui::Text("%s = %s", d->name.c_str(), value_str(v).c_str());
                        ImGui::EndTooltip();
                    }
                    if (++i % 5) ImGui::SameLine(0, 18);
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Species")) {
                int i = 0;
                for (const SpeciesDef& sp : w.species.defs()) {
                    const Sprite* s = atlas.species(sp.name);
                    if (!s) continue;
                    const BodyPlan* p = atlas.plan(sp.name);
                    ImGui::BeginGroup();
                    ImGui::Image(ImTextureRef((ImTextureID)(intptr_t)tex.get(*s)), ImVec2(64, 64));
                    ImGui::Text("%s", sp.name.c_str());
                    if (p) ImGui::TextDisabled(p->flora ? (p->tree ? "tree" : p->fruiting ? "fruiting" : "plant")
                                                        : "legs %d arms %d", p->legs, p->arms);
                    ImGui::EndGroup();
                    if (++i % 6) ImGui::SameLine(0, 14);
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Kinds & Arms")) {
                int i = 0;
                for (const auto& [k, s] : atlas.kinds()) {
                    ImGui::BeginGroup();
                    ImGui::Image(ImTextureRef((ImTextureID)(intptr_t)tex.get(s)), ImVec2(48, 48));
                    ImGui::Text("%s", k.c_str()); ImGui::EndGroup();
                    if (++i % 7) ImGui::SameLine(0, 12);
                }
                ImGui::NewLine();
                ImGui::SeparatorText("weapons from property channels");
                const MaterialId fl = w.materials.id_of("flint"), cu = w.materials.id_of("copper_metal");
                struct W { const char* n; int s, b, e; bool c; MaterialId m; } ws[] = {
                    {"edge 900", 900, 0, 0, false, fl}, {"blunt 800", 0, 800, 0, false, cu},
                    {"elastic 650", 0, 0, 650, false, fl}, {"edge+coat", 900, 0, 0, true, fl}};
                for (const W& x : ws) {
                    ImGui::BeginGroup();
                    ImGui::Image(ImTextureRef((ImTextureID)(intptr_t)tex.get(atlas.weapon(x.s, x.b, x.e, x.c, x.m))), ImVec2(48, 48));
                    ImGui::Text("%s", x.n); ImGui::EndGroup(); ImGui::SameLine(0, 12);
                }
                ImGui::NewLine();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::End();
    }

    // Chronicle & Rosetta codex: the engine's codex text + the player's own
    // decipherment notes (saved beside the binary; never written to World)
    void load_rosetta() {
        std::ifstream f("viewer_rosetta_notes.txt"); std::string l;
        while (std::getline(f, l)) { const auto t = l.find('\t'); if (t != std::string::npos) rosetta[l.substr(0, t)] = l.substr(t + 1); }
    }
    void save_rosetta() {
        std::ofstream f("viewer_rosetta_notes.txt");
        for (const auto& [k, v] : rosetta) if (!v.empty()) f << k << '\t' << v << '\n';
    }
    void build_codex() {
        c_codex = w.export_lexicon_codex();
        c_stones.clear();
        const PropId lp = SIMLAB_PID(w.props, "language_id"), gt = SIMLAB_PID(w.props, "glyph_text"),
                     cl = SIMLAB_PID(w.props, "clause_atoms"), tc = SIMLAB_PID(w.props, "treaty_clause");
        for (const EntityId id : w.entities_of_kind(SIMLAB_KIND("inscription"))) {
            const Entity* e = w.find(id);
            if (!e || !e->alive) continue;
            Stone st; st.id = id; st.lang = w.prop_int(*e, lp, 0);
            for (PropId p : {gt, cl}) if (const Value* v = p != kNoProp ? w.prop_of(*e, p) : nullptr;
                                          v && v->type == ValType::Text && !v->s.empty()) { st.text = v->s; break; }
            if (tc != kNoProp) if (const Value* v = w.prop_of(*e, tc)) st.treaty = value_str(*v);
            c_stones.push_back(std::move(st));
        }
        c_chron.clear();
        const auto& h = w.ledger.history();
        for (std::size_t i = h.size() > 200 ? h.size() - 200 : 0; i < h.size(); ++i) {
            const CausalEvent& ev = h[i];
            if (ev.action == "metabolize" || ev.action == "cool" || ev.action == "ecology") continue;
            c_chron.push_back("t" + std::to_string(ev.tick) + " " + ev.action + (ev.failed ? " (failed)" : "") +
                              "  " + ev.evidence.substr(0, 160));
        }
    }
    void ui_codex() {
        if (!show_codex) return;
        ImGui::SetNextWindowSize(ImVec2(700, 560), ImGuiCond_FirstUseEver);
        ImGui::Begin("Chronicle & Rosetta Codex", &show_codex);
        if (ImGui::BeginTabBar("cx")) {
            if (ImGui::BeginTabItem("Inscriptions")) {
                for (const Stone& st : c_stones) {
                    const std::uint32_t seed = std::uint32_t(st.lang * 2654435761u);
                    ImGui::PushID(int(st.id));
                    ImGui::Image(ImTextureRef((ImTextureID)(intptr_t)tex.get(atlas.glyph_stone(seed, st.text, false))), ImVec2(48, 64));
                    ImGui::SameLine();
                    ImGui::BeginGroup();
                    ImGui::Text("stone #%llu  tongue %lld  %s", (unsigned long long)st.id, (long long)st.lang,
                                st.treaty.empty() ? "" : ("treaty: " + st.treaty).c_str());
                    // each token: its glyph + the player's guess
                    std::size_t start = 0, tok = 0;
                    for (std::size_t i = 0; i <= st.text.size() && tok < 8; ++i) {
                        if (i < st.text.size() && std::strchr(" |:,;", st.text[i]) == nullptr) continue;
                        if (i > start) {
                            const std::string token = st.text.substr(start, i - start);
                            const int gi = int(fnv(token) % 64);
                            ImGui::Image(ImTextureRef((ImTextureID)(intptr_t)tex.get(atlas.glyph(seed, gi))), ImVec2(24, 24));
                            ImGui::SameLine();
                            const std::string key = std::to_string(st.lang) + "/" + std::to_string(gi);
                            char buf[64] = {}; std::strncpy(buf, rosetta[key].c_str(), 63);
                            ImGui::SetNextItemWidth(110);
                            if (ImGui::InputText(("##g" + std::to_string(tok)).c_str(), buf, sizeof buf)) {
                                rosetta[key] = buf; save_rosetta();
                            }
                            if (ImGui::IsItemHovered()) ImGui::SetTooltip("objective: %s", token.c_str());
                            if (tok % 3 != 2) ImGui::SameLine();
                            ++tok;
                        }
                        start = i + 1;
                    }
                    ImGui::EndGroup();
                    ImGui::Separator();
                    ImGui::PopID();
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Lexicon codex")) {
                ImGui::InputTextMultiline("##codex", c_codex.data(), c_codex.size() + 1, ImVec2(-1, -1), ImGuiInputTextFlags_ReadOnly);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Chronicle")) {
                for (const std::string& l : c_chron) ImGui::TextWrapped("%s", l.c_str());
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::End();
    }

    // ---- input ------------------------------------------------------------------------
    Vec2 screen_to_world(float mx, float my) const {   // world_rt px -> map units (elevation 0)
        const float wx = (mx - cam.sw * 0.5f) / cam.zoom + cam.cx;
        const float wy = (my - cam.sh * 0.5f) / cam.zoom / Camera::kY + cam.cy;
        return Vec2{std::int32_t(wx), std::int32_t(wy)};
    }
    void pick(float mx, float my) {
        float best = 1e9f; EntityId hit = kNoEntity;
        for (const EntityView& e : snap.entities) {
            const float fx = cam.sx(float(e.pos.x)), fy = cam.sy(float(e.pos.y), elev_at(e.pos)) - 14 * cam.zoom;
            const float d = (fx - mx) * (fx - mx) + (fy - my) * (fy - my);
            if (d < best && d < 400 * cam.zoom * cam.zoom) { best = d; hit = e.id; }
        }
        if (avatar != kNoEntity) {
            if (hit != kNoEntity && hit != avatar) {
                av_targets.insert(av_targets.begin(), hit);
                if (av_targets.size() > 2) av_targets.resize(2);
                selected = hit;
            } else if (hit == kNoEntity) {
                // screen -> map (inverse of Camera::sx/sy at elevation 0)
                const float wx = (mx - cam.sw * 0.5f) / cam.zoom + cam.cx;
                const float wy = (my - cam.sh * 0.5f) / cam.zoom / Camera::kY + cam.cy;
                avatar_move(Vec2{std::int32_t(wx), std::int32_t(wy)});
            }
            return;
        }
        selected = hit;
    }

    void handle(const SDL_Event& e, bool& quit) {
        ImGui_ImplSDL3_ProcessEvent(&e);
        ui_dirty = true;
        const bool ui = ImGui::GetIO().WantCaptureMouse, kb = ImGui::GetIO().WantCaptureKeyboard;
        if (e.type == SDL_EVENT_QUIT) quit = true;
        if (e.type == SDL_EVENT_WINDOW_RESIZED) { win_w = e.window.data1; win_h = e.window.data2; resize_world_rt(); }
        if (e.type == SDL_EVENT_MOUSE_WHEEL && !ui && !in_menu) {
            if (globe) globe_zoom(e.wheel.y > 0 ? 1.15 : 1 / 1.15, e.wheel.mouse_x / kPix, e.wheel.mouse_y / kPix);
            else if (is_planet() && e.wheel.y < 0 && cam.zoom <= 0.2501f) enter_globe();   // zoom out to orbit
            else cam.zoom = std::clamp(cam.zoom * (e.wheel.y > 0 ? 1.15f : 1 / 1.15f), 0.25f, 3.0f);
        }
        // ---- touch: 1 finger pans (surface), 2 fingers pinch-zoom ----
        if (e.type == SDL_EVENT_FINGER_DOWN || e.type == SDL_EVENT_FINGER_MOTION || e.type == SDL_EVENT_FINGER_UP) {
            const ImVec2 fp(e.tfinger.x * win_w, e.tfinger.y * win_h);
            if (e.type == SDL_EVENT_FINGER_UP) { fingers.erase(e.tfinger.fingerID); pinch_d = 0; }
            else {
                fingers[e.tfinger.fingerID] = fp;
                if (fingers.size() == 2) {
                    const ImVec2 a = fingers.begin()->second, b = std::next(fingers.begin())->second;
                    const float d = std::hypot(a.x - b.x, a.y - b.y);
                    if (pinch_d > 0 && d > 0 && !in_menu) {
                        const float f = d / pinch_d;
                        if (globe) globe_zoom(f, (a.x + b.x) * 0.5f / kPix, (a.y + b.y) * 0.5f / kPix);
                        else if (is_planet() && f < 1 && cam.zoom <= 0.2501f) enter_globe();
                        else cam.zoom = std::clamp(cam.zoom * f, 0.25f, 3.0f);
                    }
                    pinch_d = d;
                } else if (e.type == SDL_EVENT_FINGER_MOTION && fingers.size() == 1 && !globe && !ui && !in_menu) {
                    cam.cx -= e.tfinger.dx * win_w / kPix / cam.zoom;
                    cam.cy -= e.tfinger.dy * win_h / kPix / cam.zoom / Camera::kY;
                }
            }
        }
        // ---- globe: drag spins, double click dives ----
        if (globe && !ui && !in_menu) {
            if (e.type == SDL_EVENT_MOUSE_MOTION && (e.motion.state & (SDL_BUTTON_LMASK | SDL_BUTTON_RMASK))) {
                gview.lon -= e.motion.xrel / kPix / gview.radius;
                gview.lat = std::clamp(gview.lat + e.motion.yrel / kPix / gview.radius, -1.35, 1.35);
                globe_dive = false;
                return;
            }
            if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                double lo, la;
                if (e.button.clicks >= 2 &&
                    GlobeRenderer::unproject(gview, e.button.x / kPix - cam.sw * 0.5, e.button.y / kPix - cam.sh * 0.5, lo, la))
                    dive_to(lo, la);
                return;
            }
        }
        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && !ui && !in_menu && e.button.button == SDL_BUTTON_RIGHT && tool != kToolNone) {
            tool = kToolNone; dragging = false; return;
        }
        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && !ui && !in_menu && e.button.button == SDL_BUTTON_LEFT) {
            // a word in a speech bubble -> decipher it
            for (const BubbleRect& b : bubble_rects)
                if (e.button.x >= b.x0 && e.button.x <= b.x1 && e.button.y >= b.y0 && e.button.y <= b.y1) {
                    open_word(b.lang, b.word); return;
                }
            const Vec2 wp = screen_to_world(e.button.x / kPix, e.button.y / kPix);
            if (tool == kToolBeacon) { place_beacon(wp); tool = kToolNone; return; }
            if (tool == kToolBlueprint) { dragging = true; drag_a = drag_b = wp; return; }
            pick(e.button.x / kPix, e.button.y / kPix);
            if (tool == kToolFear && selected != kNoEntity) { instill_fear(selected); tool = kToolNone; }
        }
        if (e.type == SDL_EVENT_MOUSE_MOTION && dragging) drag_b = screen_to_world(e.motion.x / kPix, e.motion.y / kPix);
        if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && dragging && e.button.button == SDL_BUTTON_LEFT) {
            dragging = false;
            const std::int64_t r = std::int64_t(std::sqrt(double(dist2(drag_a, drag_b))));
            place_blueprint(drag_a, r); tool = kToolNone;
        }
        if (e.type == SDL_EVENT_MOUSE_MOTION && !ui && (e.motion.state & SDL_BUTTON_RMASK)) {
            cam.cx -= e.motion.xrel / kPix / cam.zoom; cam.cy -= e.motion.yrel / kPix / cam.zoom / Camera::kY;
        }
        if (e.type == SDL_EVENT_KEY_DOWN && !kb) {
            const SDL_Keycode k = e.key.key;
            if (k == SDLK_ESCAPE) {
                if (tool != kToolNone) { tool = kToolNone; dragging = false; return; }
                in_menu = !in_menu; if (in_menu) { speed = 0; speed_atomic = 0; } return;
            }
            if (k == SDLK_F5) { save_slot(1); return; }
            if (k == SDLK_J) { show_journal = !show_journal; return; }
            if (k == SDLK_M && is_planet()) { if (globe) enter_surface(gview.lon, gview.lat); else enter_globe(); return; }
            if (avatar != kNoEntity && k >= SDLK_1 && k <= SDLK_9) {   // avatar verbs
                int n = 0;
                for (const std::string& v : c_av_verbs) {
                    if (v == "move") continue;
                    if (++n == int(k - SDLK_1) + 1) { avatar_act(v); break; }
                }
                return;
            }
            if (k >= SDLK_1 && k <= SDLK_6) lens = int(k - SDLK_1);
            if (k == SDLK_SPACE) speed = speed ? 0 : 1;
            if (k == SDLK_EQUALS || k == SDLK_KP_PLUS) speed = std::min(4, speed + 1);
            if (k == SDLK_MINUS || k == SDLK_KP_MINUS) speed = std::max(0, speed - 1);
            speed_atomic = speed;
            if (k == SDLK_F) fog.set_mode(fog.mode() == FogOfWar::Mode::GodEye ? FogOfWar::Mode::Tribe
                                                                              : FogOfWar::Mode::GodEye, tribe_lang());
        }
    }

    void pan_keys(double dt) {
        if (ImGui::GetIO().WantCaptureKeyboard) return;
        const bool* ks = SDL_GetKeyboardState(nullptr);
        if (globe) {                             // WASD spins the planet
            const double r = 1.4 * dt;
            if (ks[SDL_SCANCODE_A] || ks[SDL_SCANCODE_LEFT]) gview.lon -= r;
            if (ks[SDL_SCANCODE_D] || ks[SDL_SCANCODE_RIGHT]) gview.lon += r;
            if (ks[SDL_SCANCODE_W] || ks[SDL_SCANCODE_UP]) gview.lat = std::min(1.35, gview.lat + r);
            if (ks[SDL_SCANCODE_S] || ks[SDL_SCANCODE_DOWN]) gview.lat = std::max(-1.35, gview.lat - r);
            return;
        }
        if (avatar != kNoEntity) {               // WASD walks the possessed body
            move_cool -= dt;
            int dx = 0, dy = 0;
            if (ks[SDL_SCANCODE_W] || ks[SDL_SCANCODE_UP]) dy -= 1;
            if (ks[SDL_SCANCODE_S] || ks[SDL_SCANCODE_DOWN]) dy += 1;
            if (ks[SDL_SCANCODE_A] || ks[SDL_SCANCODE_LEFT]) dx -= 1;
            if (ks[SDL_SCANCODE_D] || ks[SDL_SCANCODE_RIGHT]) dx += 1;
            if ((dx || dy) && move_cool <= 0) {
                avatar_move(Vec2{c_av_pos.x + dx * 64, c_av_pos.y + dy * 64});
                move_cool = 0.22;
            }
            return;
        }
        const float v = float(500.0 * dt / cam.zoom);
        if (ks[SDL_SCANCODE_W] || ks[SDL_SCANCODE_A] || ks[SDL_SCANCODE_S] || ks[SDL_SCANCODE_D] ||
            ks[SDL_SCANCODE_UP] || ks[SDL_SCANCODE_DOWN] || ks[SDL_SCANCODE_LEFT] || ks[SDL_SCANCODE_RIGHT])
            ui_dirty = true;                            // labels follow the camera
        if (ks[SDL_SCANCODE_W] || ks[SDL_SCANCODE_UP]) cam.cy -= v / Camera::kY;
        if (ks[SDL_SCANCODE_S] || ks[SDL_SCANCODE_DOWN]) cam.cy += v / Camera::kY;
        if (ks[SDL_SCANCODE_A] || ks[SDL_SCANCODE_LEFT]) cam.cx -= v;
        if (ks[SDL_SCANCODE_D] || ks[SDL_SCANCODE_RIGHT]) cam.cx += v;
    }

    // one frame: snapshot (const, under the world lock) -> draw -> UI
    void frame(double dt) {
        time_s += dt; ++frames;
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        SDL_SetRenderTarget(ren, world_rt);
        SDL_SetRenderDrawColor(ren, 18, 15, 12, 255);
        SDL_RenderClear(ren);
        using clk = std::chrono::steady_clock;
        auto ms = [](clk::time_point a) { return std::chrono::duration<double, std::milli>(clk::now() - a).count(); };
        double t_cap = 0, t_ter = 0, t_ent = 0, t_ui = 0, t_im = 0, t_pr = 0;
        static const int skip = std::getenv("SIMLAB_VSKIP") ? std::atoi(std::getenv("SIMLAB_VSKIP")) : 0;
        auto t = clk::now();
        {
            // NEVER wait for the simulation: if a cycle is running, draw the
            // last snapshot/caches (the world cannot have visibly changed
            // mid-cycle anyway) and ask the sim thread to yield next time.
            std::unique_lock<std::mutex> lk(mx, std::try_to_lock);
            if (lk.owns_lock()) {
                if (frames % 6 == 1) { fog.update(w); update_fog_tex(); }
                if (frames % 30 == 1) update_minimap();
                // snapshot 1.5x the view: panning while the sim is busy
                // still has content at the edges
                ViewRect v = cam.view();
                const std::int32_t mx_ = (v.x1 - v.x0) / 4, my_ = (v.y1 - v.y0) / 4;
                v.x0 -= mx_; v.x1 += mx_; v.y0 -= my_; v.y1 += my_;
                if (!globe) snap = capture(w, v, fog);      // orbit view never needs the 2.5D snapshot
                c_tick = w.clock_.tick(); c_minds = w.minds_.size();
                wrap_camera();
                if (is_planet() && frames % 30 == 3) { build_globe_fog(); if (globe) build_globe_icons(); }
                if (is_planet() && !globe && frames % 10 == 4) update_mini_globe();
                // player commands first (they act at this exact world state)
                if (!cmds.empty()) {
                    auto q = std::move(cmds); cmds.clear();
                    for (auto& f : q) f();
                }
                // Patron: overhear / read / record firsts (incremental, const)
                if (frames % 8 == 1) {
                    journal.tribe = tribe_lang();
                    const std::size_t nm = patron_scan(w, journal, fog.mode() == FogOfWar::Mode::GodEye ? nullptr : &fog);
                    if (nm && !journal.milestones.empty())
                        status_msg = "Milestone: " + journal.milestones.back().title;
                    c_speaker_pos.clear();
                    for (const SpeechBubble& b : journal.bubbles)
                        if (const Entity* se = w.find(b.speaker); se && se->alive) c_speaker_pos[b.speaker] = se->pos;
                }
                if (frames % 15 == 2) build_patron_caches();
                if (avatar != kNoEntity) {
                    c_av_verbs.clear(); c_av_line.clear();
                    const Entity* ae = w.find(avatar);
                    if (!ae || !ae->alive) {
                        // the vessel fell: offer its children, then kin nearby
                        fallen = avatar;
                        dstate.possessed.erase(avatar); avatar = kNoEntity;
                        fog.set_mode(FogOfWar::Mode::Tribe, tribe_lang());
                        c_successors.clear();
                        for (const Relation& r : w.relations_of(fallen, "parent_of"))
                            if (r.a == fallen) if (const Entity* ce = w.find(r.b); ce && ce->alive) c_successors.push_back(r.b);
                        if (const Entity* fe = w.find(fallen))
                            for (EntityId id : nearest_tribe(fe->pos, 6))
                                if (std::find(c_successors.begin(), c_successors.end(), id) == c_successors.end()) c_successors.push_back(id);
                        need_successor = true;
                        status_msg = "Your vessel died";
                    } else {
                        if (const AgentMind* am = w.find_mind(avatar))
                            c_av_verbs.assign(am->known_actions.begin(), am->known_actions.end());
                        auto pi = [&](const char* n) { const PropertyDef* d = w.props.find_name(n); return d ? (long long)w.prop_int(*ae, d->id, 0) : 0LL; };
                        c_av_line = "hunger " + std::to_string(pi("hunger")) + "  thirst " + std::to_string(pi("thirst")) +
                                    "  body " + std::to_string(pi("body_temp") / 10) + "C  blood " + std::to_string(pi("blood_volume")) +
                                    "  pain " + std::to_string(pi("pain_level")) + "  integrity " + std::to_string(pi("integrity"));
                        c_av_pos = ae->pos;
                        cam.cx += (float(ae->pos.x) - cam.cx) * 0.2f; cam.cy += (float(ae->pos.y) - cam.cy) * 0.2f;
                    }
                }
                if (show_inspector && (frames % 10 == 1 || insp.id != selected)) build_inspector();
                if (show_codex && (frames % 60 == 1 || c_codex.empty())) build_codex();
                const std::uint64_t key = (c_tick / 48) * 16 + std::uint64_t(lens);
                if (!globe && (key != ground_key || !ground)) { build_ground(); ground_key = key; }
            } else {
                ui_wants = true;
            }
        }
        t_cap = ms(t); t = clk::now();
        globe_step(dt);
        if (globe) {
            draw_globe();
        } else {
            if (!(skip & 1)) { draw_far_terrain(); draw_terrain(); }
            t_ter = ms(t); t = clk::now();
            if (!(skip & 2)) draw_entities(dt);
            draw_links();
            if (!(skip & 4)) draw_night();
        }
        SDL_SetRenderTarget(ren, nullptr);
        if (!(skip & 32)) SDL_RenderTexture(ren, world_rt, nullptr, nullptr);     // nearest x kPix
        t_ent = ms(t); t = clk::now();
        if (in_menu) ui_menu();
        else if (!(skip & 16)) {
            if (globe) draw_globe_icons(); else draw_overlays();
            ui_topbar();
            if (!globe) { if (is_planet()) ui_mini_globe(); else ui_minimap(); }
            ui_inspector(); ui_player(); ui_journal(); ui_successor();
            if (!show_journal) ui_word_editor();
        }
        if (!(skip & 8) && !in_menu) { ui_atlas(); ui_codex(); }
        t_ui = ms(t);
        t = clk::now();
        ImGui::Render();
        static const bool ui_direct = std::getenv("SIMLAB_VSKIP") && (std::atoi(std::getenv("SIMLAB_VSKIP")) & 64);
        if (ui_direct) {
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), ren);
        } else if (ui_dirty || frames % 4 == 0) {
            SDL_SetRenderTarget(ren, ui_rt);
            SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 0);
            SDL_RenderClear(ren);
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), ren);
            SDL_SetRenderTarget(ren, nullptr);
            ui_dirty = false;
        }
        if (!ui_direct) SDL_RenderTexture(ren, ui_rt, nullptr, nullptr);
        t_im = ms(t); t = clk::now();
        if (!shot_path.empty()) {
            if (SDL_Surface* shot = SDL_RenderReadPixels(ren, nullptr)) {
                SDL_SaveBMP(shot, shot_path.c_str());
                SDL_DestroySurface(shot);
            }
            shot_path.clear();
        }
        SDL_RenderPresent(ren);
        t_pr = ms(t);
        if (std::getenv("SIMLAB_VDEBUG") && frames % 40 == 5)
            std::printf("  frame %llu: capture %.1f terrain %.1f entities %.1f ui %.1f imgui-draw %.1f present %.1f (ents %zu)\n",
                        (unsigned long long)frames, t_cap, t_ter, t_ent, t_ui, t_im, t_pr, snap.entities.size());
    }
};

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t seed = 2026;
    std::string data = "data";
    long selftest = 0;
    std::string shot;
    bool live = false;     // --selftest N --live: frames while the sim thread runs at 100x
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--seed" && i + 1 < argc) seed = std::stoull(argv[++i]);
        else if (a == "--data" && i + 1 < argc) data = argv[++i];
        else if (a == "--selftest" && i + 1 < argc) selftest = std::stol(argv[++i]);
        else if (a == "--shot" && i + 1 < argc) shot = argv[++i];
        else if (a == "--live") live = true;
    }
    if (selftest > 0 && !std::getenv("SDL_VIDEO_DRIVER")) SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    // software only: never load the GL/EGL driver stack (on a GPU box it
    // alone costs ~80 MB of RSS; on a GPU-less box it does not exist)
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "0");
    if (!SDL_Init(SDL_INIT_VIDEO)) { std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    World world(seed, data);
    if (!world.content_error.empty()) { std::fprintf(stderr, "content: %s\n", world.content_error.c_str()); return 1; }
    // 4GB-machine budget: keep the causal ledger window under 16 MB in RAM
    // (older events fold into the archive digest — state_hash stays exact)
    world.ledger.set_byte_budget(16u << 20);
    Viewer v(world);
    v.data_dir = data;
    if (char* pref = SDL_GetPrefPath("Sunnyworld", "SimLab")) { v.pref_dir = pref; SDL_free(pref); }
    else v.pref_dir = "./";
    std::snprintf(v.seed_buf, sizeof v.seed_buf, "%llu", (unsigned long long)seed);
    {   // backdrop world behind the title menu (also the selftest world)
        PrimordialParams p; p.seed = seed; p.grid = 64; p.tribes = 2; p.per_tribe = 6;
        if (const char* pl = std::getenv("SIMLAB_VPLANET")) {      // e.g. 2048 -> 2048 x 1024 planet
            p.planet = true; p.planet_w = std::atoi(pl); p.planet_h = p.planet_w / 2; p.grid = 128;
        }
        v.pp = p;
        v.pw = build_primordial(world, p);
        if (!v.pw.homes.empty()) { v.cam.cx = float(v.pw.homes[0].x); v.cam.cy = float(v.pw.homes[0].y); }
        v.fog.set_mode(FogOfWar::Mode::Tribe, v.tribe_lang());
    }
    v.speed = 0; v.speed_atomic = 0;               // paused under the title menu
    v.win = SDL_CreateWindow("Sunnyworld - Primordial Sandbox", v.win_w, v.win_h, SDL_WINDOW_RESIZABLE);
    if (!v.win) { std::fprintf(stderr, "window: %s\n", SDL_GetError()); return 1; }
    v.ren = SDL_CreateRenderer(v.win, "software");               // no GPU required
    if (!v.ren) v.ren = SDL_CreateRenderer(v.win, nullptr);
    if (!v.ren) { std::fprintf(stderr, "renderer: %s\n", SDL_GetError()); return 1; }
    // no vsync: with a software renderer a frame that misses 16.6 ms would be
    // held to the next vblank (60 -> 30 FPS judder); we pace frames ourselves
    SDL_SetRenderVSync(v.ren, 0);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    apply_theme();
    ImGui_ImplSDL3_InitForSDLRenderer(v.win, v.ren);
    ImGui_ImplSDLRenderer3_Init(v.ren);
    v.atlas.build(world);
    v.init_textures();
    v.planet_setup();
    std::printf("atlas: %zu sprites, %zu KB pixels\n", v.atlas.size(), v.atlas.bytes() / 1024);

    bool quit = false;
    auto last = std::chrono::steady_clock::now();
    if (selftest > 0) {
        // headless self-test: (1) render-only frames must leave state_hash
        // untouched; (2) then run the sim and report frame time + RSS.
        v.show_atlas = v.show_codex = true;
        v.in_menu = false;
        const std::uint64_t h0 = world.state_hash();
        const bool dbg = std::getenv("SIMLAB_VDEBUG") != nullptr;
        if (dbg) std::printf("rss before frames %zu\n", rss_kb());
        for (long f = 0; f < selftest / 2; ++f) {
            v.lens = int((f / 40) % 6);
            const auto tf = std::chrono::steady_clock::now();
            v.frame(1.0 / 60);
            if (dbg && f < 6) std::printf("frame %ld %.1fms rss %zu tex %zu\n", f,
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tf).count(), rss_kb(), v.tex.map.size());
        }
        if (std::getenv("SIMLAB_VGLOBE")) {
            // globe smoke test: orbit view, spin, render cost, dive back down
            if (!v.is_planet()) { std::printf("globe: not a planet world (set SIMLAB_VPLANET)\n"); return 3; }
            const std::uint64_t hg = world.state_hash();
            v.show_atlas = v.show_codex = false;
            v.enter_globe();
            if (std::getenv("SIMLAB_VGOD")) v.fog.set_mode(FogOfWar::Mode::GodEye);
            v.gview.radius = std::min(v.cam.sw, v.cam.sh) * 0.42;
            std::vector<double> ft;
            for (int f = 0; f < 90; ++f) {
                v.gview.lon += 0.03; if (f % 30 == 0) v.gview.lat = 0.25 * std::sin(f * 0.1);
                const auto a = std::chrono::steady_clock::now();
                v.frame(1.0 / 60);
                ft.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count());
            }
            std::sort(ft.begin(), ft.end());
            double sum = 0; for (double x : ft) sum += x;
            if (!shot.empty()) {
                if (!v.pw.homes.empty()) v.lonlat_of(v.pw.homes[0].x, v.pw.homes[0].y, v.gview.lon, v.gview.lat);
                v.gview.lon -= 0.35; v.gview.lat = std::clamp(v.gview.lat + 0.25, -1.3, 1.3);
                for (int f = 0; f < 35; ++f) v.frame(1.0 / 60);
                v.ui_dirty = true; v.shot_path = shot + "_globe.bmp"; v.frame(1.0 / 60);
            }
            // double-click dive onto the first band's home
            double lo = 0, la = 0;
            v.lonlat_of(v.pw.homes.empty() ? 0 : v.pw.homes[0].x, v.pw.homes.empty() ? 0 : v.pw.homes[0].y, lo, la);
            v.dive_to(lo, la);
            int steps = 0;
            while (v.globe && steps < 400) { v.frame(1.0 / 60); ++steps; }
            for (int f = 0; f < 30; ++f) v.frame(1.0 / 60);
            const bool landed = !v.globe;
            const float dx = v.cam.cx - (v.pw.homes.empty() ? 0 : v.pw.homes[0].x), dy = v.cam.cy - (v.pw.homes.empty() ? 0 : v.pw.homes[0].y);
            // wrap-around: walk the camera one full planet east, it must land on the same spot
            const float x0 = v.cam.cx;
            v.cam.cx += float(v.planet_units_w()); v.frame(1.0 / 60);
            const bool wrapped = std::fabs(v.cam.cx - x0) < 1.0f;
            if (!shot.empty()) { v.ui_dirty = true; v.shot_path = shot + "_surface.bmp"; v.frame(1.0 / 60); }
            // zoom back out to orbit through the wheel path
            v.cam.zoom = 0.25f;
            SDL_Event we{}; we.type = SDL_EVENT_MOUSE_WHEEL; we.wheel.y = -1; we.wheel.mouse_x = 400; we.wheel.mouse_y = 300;
            bool q2 = false; v.handle(we, q2);
            const bool back_to_orbit = v.globe;
            std::printf("globe: planet %dx%d albedo %dx%d frame avg %.2f ms p95 %.2f ms | dive %d frames landed=%d off=(%.0f,%.0f) "
                        "| wrap=%d | wheel-out->globe=%d | tiles %zu | rss %zu KB | hash_unchanged=%d icons=%zu\n",
                        v.planet_w, v.planet_h, v.pmap.aw, v.pmap.ah, sum / ft.size(), ft[ft.size() * 95 / 100], steps, int(landed),
                        dx, dy, int(wrapped), int(back_to_orbit), v.tile_tex.size(), rss_kb(), int(world.state_hash() == hg), v.c_gicons.size());
            return landed && wrapped && back_to_orbit && world.state_hash() == hg ? 0 : 4;
        }
        if (!shot.empty() && std::getenv("SIMLAB_VDEMO")) {
            // gameplay demo: let the world run, pin + possess a founder,
            // light a beacon and dream a blueprint, open the journal
            v.show_atlas = v.show_codex = false;
            const int cyc = std::atoi(std::getenv("SIMLAB_VDEMO"));
            for (int c = 0; c < cyc; ++c) { autonomous_cycle(world, &v.dstate); v.frames = 0; v.frame(1.0 / 60); }
            if (!v.pw.founders.empty()) {
                v.journal.pins.insert(v.pw.founders.front());
                const Entity* f0 = world.find(v.pw.founders.front());
                if (f0) {
                    v.place_beacon(Vec2{f0->pos.x + 160, f0->pos.y - 120});
                    v.place_blueprint(Vec2{f0->pos.x - 60, f0->pos.y + 40}, 90);
                }
                if (v.pw.founders.size() > 1 && std::getenv("SIMLAB_VPOSSESS")) v.set_possess(v.pw.founders[1]);
            }
            v.show_journal = true;
            if (const char* jt = std::getenv("SIMLAB_VTAB")) v.journal_tab_req = std::atoi(jt);
            for (int f = 0; f < 20; ++f) v.frame(1.0 / 60);
            for (int c = 0; c < 6; ++c) { autonomous_cycle(world, &v.dstate); v.frame(1.0 / 60); }
            for (int f = 0; f < 20; ++f) v.frame(1.0 / 60);
            v.ui_dirty = true; v.shot_path = shot; v.frame(1.0 / 60);
            std::printf("demo: milestones=%zu words=%zu resonance=%lld signs=%zu avatar=%llu\n", v.journal.milestones.size(),
                        v.journal.lexicon.size(), (long long)v.journal.resonance, v.c_signs.size(), (unsigned long long)v.avatar);
            return 0;
        }
        if (!shot.empty()) { v.show_atlas = v.show_codex = false; v.ui_dirty = true; v.shot_path = shot; v.frame(1.0 / 60); }
        const bool same = world.state_hash() == h0;
        // frame cost alone (the sim normally runs on its own thread)
        const auto t0 = std::chrono::steady_clock::now();
        for (long f = selftest / 2; f < selftest; ++f) { v.lens = int((f / 40) % 6); v.frame(1.0 / 60); }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        // then a burst of simulation to show the world-side RAM stays bounded
        const std::size_t rss_render = rss_kb();
        v.speed = 2;
        const auto ts = std::chrono::steady_clock::now();
        for (int c = 0; c < 20; ++c) v.step_sim(1.0 / 10);
        const double sim_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ts).count() / 20;
        std::printf("selftest: render-phase rss_kb=%zu  sim avg_cycle_ms=%.1f  ledger_kb=%zu  minds=%zu\n",
                    rss_render, sim_ms, world.ledger.retained_bytes() / 1024, world.minds_.size());
        std::printf("selftest: frames=%ld render_only_hash_unchanged=%d avg_frame_ms=%.2f rss_kb=%zu entities=%zu\n",
                    selftest, int(same), ms / double(std::max<long>(1, selftest - selftest / 2)), rss_kb(), world.entities.size());
        if (live) {
            // smoothness under load: the sim thread runs flat out (100x)
            // while we render; a frame must never wait for a cycle
            v.speed = 4; v.speed_atomic = 4;
            v.sim_thread = std::thread([&v] { v.sim_loop(); });
            std::vector<double> ft;
            for (long f = 0; f < selftest; ++f) {
                const auto a = std::chrono::steady_clock::now();
                v.frame(1.0 / 60);
                ft.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count());
            }
            v.running = false;
            v.sim_thread.join();
            std::sort(ft.begin(), ft.end());
            double sum = 0; for (double x : ft) sum += x;
            std::printf("live: frames=%zu avg_ms=%.2f p95_ms=%.2f max_ms=%.2f tick=%llu minds=%zu\n", ft.size(),
                        sum / double(ft.size()), ft[ft.size() * 95 / 100], ft.back(),
                        (unsigned long long)world.clock_.tick(), world.minds_.size());
        }
        quit = true;
        if (!same) return 2;
    }
    if (!quit) v.sim_thread = std::thread([&v] { v.sim_loop(); });
    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) v.handle(e, quit);
        if (v.want_quit) quit = true;
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::min(0.1, std::chrono::duration<double>(now - last).count());
        last = now;
        v.pan_keys(dt);
        v.frame(dt);
        // ~60 FPS pacing: sleep what is left of the 16.6 ms budget
        const double spent = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - now).count();
        if (spent < 16.0) std::this_thread::sleep_for(std::chrono::microseconds(int((16.0 - spent) * 1000)));
    }
    v.running = false;
    if (v.sim_thread.joinable()) v.sim_thread.join();
    v.tex.clear();
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(v.ren);
    SDL_DestroyWindow(v.win);
    SDL_Quit();
    return 0;
}
