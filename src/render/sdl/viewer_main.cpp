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
#if defined(__GLIBC__) || defined(__linux__)
#include <malloc.h>
#endif
#include "imgui.h"
#include "imgui_internal.h"
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
#include "simlab/render/Narrative.hpp"
#include "simlab/render/Globe.hpp"
#include "simlab/NarrativeHost.hpp"
#include "simlab/Chronicle.hpp"
#include "simlab/SunnyChronicle.hpp"
#include "simlab/GenePool.hpp"
#include "simlab/Brain.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <atomic>
#include <iterator>
#include <list>
#include <map>
#include <set>
#include <mutex>
#include <thread>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

struct Particle { float x, y, vx, vy, life; std::uint32_t c; float grav = 90.f; };

enum Lens { kNatural = 0, kEpidemic, kEconomy, kPolitics, kSacred, kMilitary, kEcology };
const char* kLensName[] = {"1 Natural", "2 Epidemiology", "3 Economy", "4 Politics", "5 Sacred", "6 Military", "7 Ecology"};

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
    int screen = 0;                    // Screen (menu page)
    std::atomic<bool> has_game{false}; // a world is loaded/created (sim thread checks it)
    PlanetMap title_map;               // decorative planet behind the title menu
    GlobeView title_view;
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
    // ---- Phase 15: Emergent Narrative Chronicle ------------------------------
    // UI-thread only: cfeed/c_callouts are written inside poll_world (which
    // already holds the world lock) and read by the HUD windows — no extra
    // synchronisation needed.
    NarrativeComposer composer;
    ChronicleFeed cfeed;
    struct Callout { Vec2 pos; float oy = 0; std::string text; std::uint64_t until; ImU32 col; };
    std::deque<Callout> c_callouts;
    std::uint64_t narr_seen = 0;             // last NarrativeLine.seq drained
    int chron_filter = 0;                    // 0 all 1 combat 2 survival 3 discovery 4 society 5 pinned
    std::map<EntityId, std::vector<std::string>> c_carved;   // legend -> translated stone words
    std::uint64_t c_carved_tick = ~0ull;                      // rebuild gate (world tick)
    bool need_successor = false;
    EntityId fallen = kNoEntity;
    // ---- CL-1 Phase 3: async LLM chronicler (Qwen via NarrativeHost) ---------
    // Presentation only: the worker thread never touches World state, so
    // state_hash() is identical with the host on or off. `llm_host` is
    // started explicitly by start_llm() (main, non-selftest runs only);
    // llm_starter joins it so model load never blocks the UI thread.
    NarrativeHost llm_host;
    std::thread llm_starter;
    EventId llm_scanned = 0;               // ledger cursor (mirrors cfeed.scanned)
    bool llm_primed = false;
    std::uint64_t llm_key_seq = 0;         // outbound narration keys
    std::unordered_map<std::uint64_t, NarrativeLine> llm_pending;  // key -> line stub
    std::deque<std::uint64_t> llm_outstanding;                     // FIFO poll order
    std::unordered_map<std::uint64_t, std::uint64_t> llm_last_tell; // actor^class -> tick
    // ---- Continuous Chronicle (Biên Niên Sử): batched LLM chapters --------
    // Salient ledger events accumulate as facts; every ~300 ticks (or on a
    // catastrophic trigger) they are composed into ONE chapter prompt that
    // carries the previous chapter's tail for narrative continuity. Finished
    // chapters append to saves/chronicle_<world_seed>.md — presentation
    // only, never touches World state or state_hash().
    struct ChapterFact { std::uint64_t tick; std::string text; Vec2 pos; };
    struct ChronicleChapter {
        std::uint64_t tick0 = 0, tick1 = 0;
        std::string title, body, facts;
        Vec2 pos{};
    };
    struct ChapterStub { std::uint64_t tick0 = 0, tick1 = 0; Vec2 pos{}; std::string facts; };
    std::vector<ChapterFact> chron_facts;
    std::set<std::string> chron_kinds;          // every kind ever seen (new-species trigger)
    std::uint64_t chron_win_start = 0;
    bool chron_catastrophic = false;
    std::vector<ChronicleChapter> chron_chapters;   // oldest -> newest (memory cap)
    std::unordered_map<std::uint64_t, ChapterStub> llm_ch_pending;
    std::deque<std::uint64_t> llm_ch_outstanding;
    std::string chron_tail;                     // last ~3 sentences of prev chapter
    std::string chron_path;                     // saves/chronicle_<seed>.md
    // ---- Sunnyworld Chronicle: era-boundary literary novelist -------------
    // Read-only collector -> NarrativeHost worker -> saves/sunnyworld_
    // chronicle.md. Lives outside World; cannot affect state_hash().
    SunnyChronicle sunny_chron;
    int chron_open = -1;                        // expanded chapter row (-1 none)
    static constexpr std::size_t kChronFactCap = 1024;  // facts per chapter window
    std::uint64_t chron_overflow = 0;           // facts dropped past the cap (this window)
    // ---- Open-ended invention: LLM material cataloger -----------------------
    // Each new World::SynthRecord gets a NAME=/DESC= prompt on the background
    // host; completed pairs append a [DISCOVERY] block to
    // saves/discovered_materials.txt and a line in the chronicle facts.
    // Presentation only — the worker never touches World state.
    struct MatPending {
        std::uint64_t synth_id = 0, tick = 0;
        std::string verb, pa, pb, actor;
        std::array<std::int64_t, 7> attrs{};
        Vec2 pos{};
    };
    struct MatEntry { std::string id, slug, name, desc; };
    std::size_t mat_scanned = 0;                    // synthesized_ cursor
    std::unordered_map<std::uint64_t, MatPending> llm_mat_pending;
    std::deque<std::uint64_t> llm_mat_outstanding;
    std::vector<MatEntry> mat_entries;              // catalog (file-backed)
    std::string mat_path = "saves/discovered_materials.txt";
    // ---- Tribal mythology: Qwen myth-maker --------------------------------
    // DoctrineSlots that survive >=3 oral generations (or InscribedArtifact
    // engram carvings) get a MYTH_TITLE=/SCRIPTURE= prompt; completed pairs
    // append a [MYTH] block (myth_record_line format) to
    // saves/tribal_mythology.txt and a chronicle fact. Presentation only.
    struct MythPending {
        AgentMind::DoctrineSlot doc;  // the crystallized tradition (artifacts: synthetic focus slot)
        std::int64_t lang = 0;        // tribe language_id (0 = artifact unknown)
        bool artifact = false;        // true = inscription translation request
        EntityId insc = kNoEntity;
        std::uint64_t tick = 0;
        Vec2 pos{};
    };
    std::unordered_map<std::uint64_t, MythPending> llm_myth_pending;
    std::deque<std::uint64_t> llm_myth_outstanding;
    std::set<std::string> myth_seen;                // doctrine dedup keys
    std::string myth_path = "saves/tribal_mythology.txt";
    // smooth camera fly-to (chapter click)
    bool cam_goal = false;
    float cam_gx = 0, cam_gy = 0, cam_gz = 1;
    // ---- CL-1 Phase 4: persistent gene pool + god-tools ----------------------
    // Hall of Fame survives world resets/wipeouts: fresh brains derive from
    // it deterministically (World::gene_pool is read-only world input, never
    // in state_hash). harvest runs on the sim thread under the world lock.
    GenePool gene_pool;
    bool gene_pool_loaded = false;
    int pool_rounds = 0;                 // sim_loop counter -> periodic save
    std::atomic<int> warp{0};            // headless cycles queued by Time Warp
    std::atomic<int> mut_permille{-1};   // -1 = keep each organism's own prop
    double move_cool = 0;
    int rounds_since_trim = 0;
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
    // Double-buffered snapshot: capture() writes snap_back under the world
    // lock, then the pair is swapped under the tiny snap_mx (<0.1 ms). The
    // draw code always reads `snap` (front) — it never waits on the sim.
    RenderSnapshot snap, snap_back;
    std::mutex snap_mx;
    std::uint64_t snap_gen = 0;         // bumped on every front-buffer swap
    CaptureScratch cap_scratch;      // wealth-scan cache shared across frames
    int lens = kNatural;
    int speed = 1;                 // 0 pause, else cycles per second index
    int last_speed = 1;
    double sim_accum = 0;
    DriverState dstate;
    EntityId selected = kNoEntity;
    std::vector<Particle> parts;
    std::unordered_map<EntityId, Vec2> last_pos;
    std::unordered_map<EntityId, float> facing, bob_until;
    std::unordered_map<EntityId, std::int64_t> last_integ, last_pain;
    // ---- Visual Interpolation & Ambient Animation (viewer-side ONLY) ----
    // vis_pos/vis_prev: float render positions that exponentially chase the
    // integer sim positions each frame; vis_prev is last frame's position,
    // giving a velocity estimate for facing flips and gait detection.
    std::unordered_map<EntityId, ImVec2> vis_pos, vis_prev;
    std::unordered_map<EntityId, float> dust_at;        // last dust puff (time_s)
    // Action telegraph: last causal verb per actor, scanned incrementally
    // from the ledger — display-only.
    std::uint64_t act_last = 0;                          // last scanned EventId
    // ---- DF-style HUD (viewer-only, never serialized) ------------------------
    // Thought/action badges above NPCs: label + color + world-tick expiry.
    // rank resolves collisions: fear(3) > action(2) > rest(1) > idle(0).
    struct ThoughtBadge { std::string label; ImU32 col = 0; std::uint64_t until = 0; int rank = 0; };
    std::unordered_map<EntityId, ThoughtBadge> c_badge;
    // remembered-target links for selected/hovered NPCs (dashed intent line)
    struct IntentLink { EntityId target = kNoEntity; Vec2 pos{}; std::string label; std::uint64_t until = 0; };
    std::unordered_map<EntityId, IntentLink> c_intent;
    EntityId hover_id = kNoEntity;                       // nearest entity to cursor
    // ---- Procedural animation & juice (viewer-only, never serialized) ------
    // Per-mind animation state: facing angle (screen-space rad, 0 = +x),
    // gait phase, expression drivers read from live neuromodulators, and the
    // freshest action verb/target for pose selection. Everything here is
    // derived each poll from World reads — nothing feeds back.
    struct AnimState {
        std::string verb;                    // raw ledger verb, "" = none fresh
        EntityId target = kNoEntity;         // remembered/gaze target
        Vec2 tpos{};                         // last known target world pos
        bool tpos_ok = false;                // tpos holds a real position
        float ang = 0;                       // smoothed facing (screen rad)
        float phase = 0;                     // gait cycle accumulator
        float fear = 0, joy = 0, tired = 0;  // expression drivers 0..1
        bool lying = false;                  // rest pose (side-lying + zZ)
        bool in_water = false;               // standing in a water cell
        std::uint64_t until = 0;             // verb freshness (world tick)
        std::uint64_t expr_until = 0;        // expression-state freshness (mind scan)
        float flash = -1.f;                  // hit-flash deadline (time_s)
        std::uint64_t last_puff = 0;         // ripple throttle (frames)
        float z_at = -10.f;                  // last "z" particle (time_s)
    };
    std::unordered_map<EntityId, AnimState> c_anim;
    // expanding ring particles (footstep ripples, speech waves) — world_rt px
    struct Ring { float x, y, r, vr, life, ttl; std::uint32_t c; };
    std::vector<Ring> rings;
    // rising heart motes on birth/reproduce events — world_rt px anchor
    struct Heart { float x, y, t; };
    std::vector<Heart> hearts;
    // floating "z" glyphs for sleepers — world_rt px anchor + age
    struct Zed { float x, y, t; };
    std::vector<Zed> zeds;
    bool show_badges = true, show_ticker = true;
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
    // ui_rt is re-rasterized only when the UI could have changed: input
    // events, a new snapshot generation, a new world tick (clock text),
    // toast fade, or every 8th frame as a safety net for animations.
    std::uint64_t ui_last_tick = ~0ull, ui_last_snap_gen = ~0ull;
    // aux_rt caches the software-expensive reference windows (Atlas gallery,
    // Chronicle codex): they rasterize at ~7.5 Hz into aux_rt and are blitted
    // back at the window rects captured at rasterize time, so dragging a
    // window never smears stale pixels.
    SDL_Texture* aux_rt = nullptr;
    float aux_rect[2][4] = {};        // x, y, w, h in window px (framebuffer space)
    bool aux_ok[2] = {};
    // fog rebuild gates: bulk fog textures regenerate only when the flat
    // fog grid's generation counter actually moved.
    std::uint64_t fog_tex_gen = ~0ull, mini_fog_gen = ~0ull, globe_fog_gen = ~0ull;
    double fog_update_t = -1;
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
        std::string intent;                  // Phase 15: what it's doing now
        std::vector<std::string> life;       // Phase 15: scars/fears/loves/proofs
        // ---- DF HUD fields (viewer-only) ------------------------------------
        std::string gen;                     // "F0 Founder" / "F1" / "F2+"
        long long age = 0, adult_age = 0, n_parents = 0, n_children = 0;
        std::vector<std::pair<std::string, std::pair<long long, long long>>> needs;  // label,(cur,max)
        std::vector<std::string> memories;   // charge-tagged engrams
        std::vector<std::string> ties;       // social bonds/trust
        std::vector<std::pair<std::string, int>> brain_act;   // 12 modules, 0..100
        std::vector<std::pair<std::string, int>> neuros;      // neuromodulators 0..1000
        std::vector<std::pair<std::string, std::string>> traits;  // inherited genome props
    } insp;
    struct Stone { EntityId id; std::int64_t lang; std::string text, treaty; };
    std::vector<Stone> c_stones;
    std::string c_codex;
    std::vector<std::string> c_codex_lines;   // c_codex split for clipped display
    std::vector<std::string> c_chron;

    explicit Viewer(World& world) : w(world) {
        dstate.curiosity = true; dstate.concurrent = true;
        dstate.brain_control = true;      // CL-1 is the sapient controller
        dstate.parallel_think = true;     // 100% neural + parallel think
        if (std::getenv("SIMLAB_NO_BRAIN")) dstate.brain_control = false;
        if (std::getenv("SIMLAB_NO_PARALLEL")) dstate.parallel_think = false;
        composer.load(data_dir + "/narrative_templates.txt");
    }
    ~Viewer() {
        // Wait out an in-flight model load so ~NarrativeHost never races it.
        if (llm_starter.joinable()) llm_starter.join();
    }
    // Kick the Qwen chronicler off a background thread (model load takes
    // seconds; the title menu must stay responsive). Safe no-op when the
    // llama build or GGUF model is absent.
    void start_llm() {
        if (llm_starter.joinable() || llm_host.active()) return;
        NarrativeHost::Config hc;
        if (const char* m = std::getenv("SIMLAB_LLM_MODEL")) hc.model_path = m;
        else hc.model_path = llm_resolve(hc.model_path);
        if (const char* l = std::getenv("SIMLAB_LLM_LIB")) hc.lib_dir = l;
        else hc.lib_dir = llm_resolve(hc.lib_dir);
        llm_starter = std::thread([this, hc] { llm_host.start(hc); });
    }
    // Model/lib paths are relative to the repo root; when the viewer is
    // launched from elsewhere, retry relative to the executable location.
    static std::string llm_resolve(const std::string& rel) {
        if (std::filesystem::exists(rel)) return rel;
        std::error_code ec;
        const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);
        if (ec) return rel;
        for (const char* up : {"..", "../.."}) {
            const std::filesystem::path p = exe.parent_path() / up / rel;
            if (std::filesystem::exists(p)) return p.string();
        }
        return rel;
    }

    std::int64_t tribe_lang() const {
        return pw.tribe_lang.empty() ? 0 : pw.tribe_lang[std::size_t(std::clamp<int>(tribe_view, 0, int(pw.tribe_lang.size()) - 1))];
    }
    // reset every viewer-side cache after the World was replaced
    void reset_view() {
        dstate = DriverState{}; dstate.curiosity = true; dstate.concurrent = true;
        dstate.brain_control = true; dstate.parallel_think = true;
        fog = FogOfWar{};
        fog.set_mode(FogOfWar::Mode::Tribe, tribe_lang());
        selected = kNoEntity; avatar = kNoEntity; av_targets.clear();
        parts.clear(); last_pos.clear(); facing.clear(); bob_until.clear(); last_integ.clear(); last_pain.clear();
        vis_pos.clear(); vis_prev.clear(); dust_at.clear();
        act_last = 0;
        c_badge.clear(); c_intent.clear(); hover_id = kNoEntity;
        c_anim.clear(); rings.clear(); hearts.clear(); zeds.clear();
        ground_key = ~0ull;
        if (ground) { SDL_DestroyTexture(ground); ground = nullptr; }
        if (fog_tex) { SDL_DestroyTexture(fog_tex); fog_tex = nullptr; }
        if (fog_ground) { SDL_DestroyTexture(fog_ground); fog_ground = nullptr; }
        clear_fchunks(); fchunk_mode = -1; fog_update_t = -1;
        if (mini_tex) { SDL_DestroyTexture(mini_tex); mini_tex = nullptr; }
        insp = InspCache{}; c_stones.clear(); c_codex.clear(); c_chron.clear();
        journal.bubbles.clear(); bubble_rects.clear(); c_signs.clear(); c_legends.clear();
        cfeed = ChronicleFeed{}; c_callouts.clear(); narr_seen = 0; c_carved.clear();
        llm_scanned = 0; llm_primed = false; llm_key_seq = 0;
        llm_pending.clear(); llm_outstanding.clear(); llm_last_tell.clear();
        mat_scanned = 0; llm_mat_pending.clear(); llm_mat_outstanding.clear();
        mat_entries.clear(); mat_load();
        llm_myth_pending.clear(); llm_myth_outstanding.clear();
        myth_load();
        cap_scratch = CaptureScratch{};
        snap = RenderSnapshot{}; snap_back = RenderSnapshot{}; snap_gen = 0;
        fog_tex_gen = mini_fog_gen = globe_fog_gen = ~0ull;
        ui_last_tick = ui_last_snap_gen = ~0ull;
        need_successor = false; tool = kToolNone; dragging = false;
        if (!pw.homes.empty()) { cam.cx = float(pw.homes[0].x); cam.cy = float(pw.homes[0].y); }
        ui_dirty = true;
    }
    // ---- New Primordial World (caller holds the lock / sim paused) ----
    void new_world(const PrimordialParams& p) {
        w = World(p.seed, data_dir);
        attach_gene_pool();
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
    std::string slot_path(int slot) const {
        return pref_dir + (slot <= 0 ? std::string("autosave") : "slot" + std::to_string(slot)) + ".sav";
    }
    void save_slot(int slot) {
        std::string err;
        std::lock_guard<std::mutex> lk(mx);
        if (w.save(slot_path(slot), &err)) {
            std::ofstream m(slot_path(slot) + ".meta");
            m << pp.seed << ' ' << pp.grid << ' ' << int(pp.planet) << ' ' << tribe_view << ' ' << w.clock_.tick() << '\n';
            journal.save(slot_path(slot) + ".journal");
            save_gene_pool();
            status_msg = std::string("Saved ") + slot_name(slot) + " (day " + std::to_string(w.clock_.tick() / 24) + ")";
        } else status_msg = "Save failed: " + err;
        ui_dirty = true;
    }
    bool load_slot(int slot) {
        std::string err;
        std::lock_guard<std::mutex> lk(mx);
        World nw(1, data_dir);
        if (!nw.load(slot_path(slot), &err)) { status_msg = "Load failed: " + err; return false; }
        w = std::move(nw);
        attach_gene_pool();
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
        has_game = true;
        status_msg = std::string("Loaded ") + slot_name(slot) + " (day " + std::to_string(w.clock_.tick() / 24) + ")";
        return true;
    }

    // ======================= planet: globe + far surface =======================
    // The planet beyond the materialized home region is ANALYTIC (pure field
    // functions of the worldgen seed): the globe samples it once into an
    // albedo map; the 2.5D surface pulls 32x32-cell colour tiles on demand.
    static constexpr double kPI = 3.14159265358979323846;
    bool is_planet() const { return planet_w > 0; }
    void planet_setup() {
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
    double sun_lon() const {
        const double hour = double(c_tick % 24) + 0.5;
        return std::remainder(kPI - hour / 24.0 * 2 * kPI + gview_sun_offset, 2 * kPI);
    }
    // title screen: a slowly turning decorative planet behind the menu
    void draw_title_globe(double dt) {
        if (!title_map.valid()) return;
        title_view.lon += dt * 0.06;
        title_view.lat = 0.35;
        title_view.radius = std::min(cam.sw, cam.sh) * 0.55;
        draw_globe();
    }
    void draw_globe() {
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
    // ---- planet ground: one chunked renderer for home region AND far planet ----
    // 32x32-cell chunks drawn with the same material sprites, tint, cliffs and
    // elevation lift everywhere, so the simulated region and the dormant
    // planet join without a seam. Region cells read the terrain grid (wear,
    // lenses); far cells read the analytic fields. LRU-bounded, a few chunk
    // builds per frame (a flat colour tile stands in until a chunk is ready).
    static constexpr int kCC = 32;          // cells per chunk side
    static constexpr float kCS = 1.0f;      // chunk texture px per map unit
    struct GChunk { SDL_Texture* t = nullptr; std::uint64_t used = 0, epoch = 0; int lens = -1; };
    std::unordered_map<std::uint64_t, GChunk> gchunks;
    void clear_gchunks() {
        for (auto& [k, g] : gchunks) if (g.t) SDL_DestroyTexture(g.t);
        gchunks.clear();
    }
    int gchunk_margin() const { return int(600 * Camera::kElev * kCS) + 6; }
    bool chunk_in_region(int wcx, int cy) const {
        const TerrainGrid& t = w.terrain;
        const int x0 = wcx * kCC, y0 = cy * kCC;
        return x0 < t.ox + t.w && x0 + kCC > t.ox && y0 < t.oy + t.h && y0 + kCC > t.oy;
    }
    CellView planet_cell(int gx, int gy) const {
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
    void build_gchunk(GChunk& g, int wcx, int cy) {
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
    void draw_ground_chunks() {
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
    // fog on planets: region keeps its per-cell soft fog; the dormant planet
    // outside it is unexplored by definition -> the same dark haze
    // ---- fog layer lifted with the terrain --------------------------------------
    // Fog is painted per cell at the SAME elevation lift (and cliff face) as
    // the ground, so the lit disk sits exactly where the people stand. Low
    // resolution (kFS px per map unit) + linear filtering = soft edges, cheap
    // rebuilds. Painter order top -> bottom with BLENDMODE_NONE: like the
    // ground, the frontmost cell owns every pixel it covers.
    static constexpr float kFS = 0.25f;
    static std::uint32_t fog_rgba(Fog f) { return f == Fog::Visible ? 0u : f == Fog::Shrouded ? 0x780A0A12u : 0xD8120F0Cu; }
    template <class ElevFn>
    void paint_fog_cells(int gx0, int gy0, int gx1, int gy1, float org_x, float org_y, int margin_px, ElevFn elev_slope) {
        const int cell = w.terrain.cell;
        const float cw = float(cell) * kFS;
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
        for (int gy = gy0; gy < gy1; ++gy)
            for (int gx = gx0; gx < gx1; ++gx) {
                std::int32_t e = 0, sl = 0;
                elev_slope(gx, gy, e, sl);
                const std::uint32_t c = fog_rgba(fog.cell_fog(gx, gy));
                const float x = (float(gx * cell) - org_x) * kFS;
                const float y = (float(gy * cell) * Camera::kY - float(e) * Camera::kElev - org_y) * kFS + float(margin_px);
                const float face = sl > 12 ? std::min(float(cell), float(sl) * Camera::kElev) * kFS : 0.f;
                SDL_SetRenderDrawColor(ren, Uint8(c >> 16), Uint8(c >> 8), Uint8(c), Uint8(c >> 24));
                SDL_FRect r{x, y, cw + 0.5f, cw * Camera::kY + face + 1.f};
                SDL_RenderFillRect(ren, &r);
            }
    }
    // valley: one fog texture over the single ground texture
    SDL_Texture* fog_ground = nullptr;
    int fog_ground_margin = 0;
    void build_fog_ground() {
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
    void draw_fog_ground() {
        if (!fog_ground || fog.mode() == FogOfWar::Mode::GodEye) return;
        const TerrainGrid& t = w.terrain;
        const float gox = float(t.ox * t.cell), goy = float(t.oy * t.cell);
        SDL_FRect src{(cam.cx - gox - cam.sw * 0.5f / cam.zoom) * kFS,
                      ((cam.cy - goy) * Camera::kY - cam.sh * 0.5f / cam.zoom) * kFS + fog_ground_margin,
                      cam.sw / cam.zoom * kFS, cam.sh / cam.zoom * kFS};
        SDL_FRect dst{0, 0, float(cam.sw), float(cam.sh)};
        SDL_RenderTexture(ren, fog_ground, &src, &dst);
    }
    // planet: one small fog texture per ground chunk, rebuilt only when a
    // cell inside it changed tier (dirty set from the fog's visible lists)
    struct FChunk { SDL_Texture* t = nullptr; bool dirty = true; std::uint64_t used = 0; };
    std::unordered_map<std::uint64_t, FChunk> fchunks;
    std::vector<std::uint64_t> fog_prev_visible;
    std::size_t fog_explored_seen = 0;
    int fchunk_mode = -1; std::int64_t fchunk_tribe = 0;
    void clear_fchunks() {
        for (auto& [k, f] : fchunks) if (f.t) SDL_DestroyTexture(f.t);
        fchunks.clear(); fog_prev_visible.clear(); fog_explored_seen = 0;
    }
    std::uint64_t fchunk_key_of_cell(int gx, int gy) const {
        const int ncx = std::max(1, (planet_w + kCC - 1) / kCC);
        const int cx = ((gx / kCC) % ncx + ncx) % ncx, cy = gy / kCC;
        return (std::uint64_t(std::uint32_t(cy)) << 32) | std::uint32_t(cx);
    }
    // called under the lock after fog.update(): mark touched chunks dirty
    void fog_mark_dirty() {
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
    void draw_fog_chunks() {
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

    void draw_planet_fog() {
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
        if (aux_rt) SDL_DestroyTexture(aux_rt);
        aux_rt = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, win_w, win_h);
        SDL_SetTextureBlendMode(aux_rt, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
        aux_ok[0] = aux_ok[1] = false;
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
    // Runs on sim_thread. A round is processed in ~6 ms wall-clock slices
    // (autonomous_cycle_step) so the UI thread can grab the world lock for
    // captures between slices instead of starving for a whole ~350 ms cycle.
    void sim_loop() {
        static const double kRate[] = {0, 2, 10, 40, 200};   // cycles / second
        auto last = std::chrono::steady_clock::now();
        double accum = 0;
        while (running) {
            const auto now = std::chrono::steady_clock::now();
            accum += std::chrono::duration<double>(now - last).count() * kRate[std::clamp(speed_atomic.load(), 0, 4)];
            last = now;
            // Headless Time Warp: run queued cycles back-to-back in bigger
            // slices, no pacing sleep — the UI still gets the lock between
            // slices so windows stay responsive (~25 rounds/sec).
            if (warp.load() > 0) {
                accum = 0;
                if (has_game) {
                    bool done = false;
                    {
                        std::lock_guard<std::mutex> lk(mx);
                        w.set_lod_stride(4);   // warp LOD: batch dormant drift
                        done = autonomous_cycle_step(w, &dstate, nullptr, 40.0);
                        if (done && ++pool_rounds >= 300) {
                            pool_rounds = 0;
                            save_gene_pool();
                            apply_mutation_override();
                        }
                    }
                    if (done) warp.fetch_sub(1);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                } else warp.store(0);
                continue;
            }
            if (accum >= 1.0) {
                bool round_done = true;
                if (has_game) {
                    {
                        std::lock_guard<std::mutex> lk(mx);
                        w.set_lod_stride(1);   // warp over — back to exact ticks
                        round_done = autonomous_cycle_step(w, &dstate, nullptr, 10.0);
                        if (round_done && ++pool_rounds >= 300) {
                            pool_rounds = 0;
                            save_gene_pool();
                            apply_mutation_override();
                        }
                    }
                    // leave the lock between slices: the UI's try_to_lock
                    // capture path gets a ~4 ms window every ~14 ms — often
                    // enough for ~15 fresh snapshots/sec while the round
                    // still completes in ~0.5 s (~2 rounds/sec at speed 1,
                    // matching the pre-slice throughput).
                    const bool wants = ui_wants.exchange(false);
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(wants ? 8 : 4));
                }
                if (round_done) {
                    accum = std::min(accum - 1.0, 4.0);   // never queue a backlog
#if defined(__GLIBC__)
                    // hand freed planner clones back to the OS: RSS tracks the
                    // live world, not the high-water mark of one heavy round
                    if (++rounds_since_trim >= 4) { malloc_trim(0); rounds_since_trim = 0; }
#endif
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
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
        draw_fog_ground();                         // lifted with the terrain (Tribe & Avatar)
    }

    // ambient water: sparse glints drifting inside visible water cells,
    // phased by cell hash so the pattern is stable but shimmer moves.
    void draw_water_glints() {
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

    // ---- procedural character animation (sapients only) -------------------
    // RimWorld-style overlay pass in world_rt px on top of the creature
    // sprite: expressive pixel eyes driven by live neuromodulators (fear ->
    // wide jittering, dopamine/joy -> squint, exhaustion -> heavy lids), two
    // articulated hands posed by the freshest action verb, and floating "z"
    // motes for sleepers. Pure presentation — reads only c_anim viewer state.
    void draw_actor_anim(const EntityView& e, AnimState& an, float fx, float fy,
                         float sw, float sh, bool gliding, bool lying) {
        if (lying) {
            // sleeper: occasional rising "z" (glyphs drawn in the ImGui pass)
            if (time_s - an.z_at > 0.8f && zeds.size() < 48) {
                an.z_at = float(time_s);
                zeds.push_back({fx + sw * 0.30f, fy - sh * 0.60f, 0.f});
            }
            return;
        }
        const bool fresh = an.until > c_tick;
        const std::string& v = an.verb;
        const bool eating = fresh && (v == "consume" || v == "eat" || v == "forage");
        const bool attacking = fresh && (v == "strike" || v == "launch" || v == "attack");
        const float hr = std::max(1.4f, sw * 0.055f);          // hand half-size
        const float breathe = std::sin(float(time_s) * 2.2f + float(e.id % 7)) * 0.7f;
        const float swing = std::sin(an.phase);                 // gait -1..1
        // ---- hands -------------------------------------------------------
        for (int i = 0; i < 2; ++i) {
            const float side = i == 0 ? -1.f : 1.f;
            float hx = fx + side * sw * 0.30f;
            float hy = fy - sh * 0.42f + breathe;
            if (gliding) {                                     // alternating arm swing
                hx += side * swing * sw * 0.05f;
                hy += swing * (i ? -1.f : 1.f) * sh * 0.06f;
            }
            if (eating) {                                      // raise to mouth, chew
                hx = fx + side * sw * 0.10f;
                hy = fy - sh * 0.70f +
                     std::sin(float(time_s) * 11.f + float(i) * 3.14159f) * sh * 0.05f;
            } else if (attacking) {                            // slashing lunge
                const float dir = std::cos(an.ang) >= 0.f ? 1.f : -1.f;
                const float jab = std::max(0.f, std::sin(float(time_s) * 13.f + float(i) * 3.14159f));
                hx += dir * jab * sw * 0.14f * (i ? 0.6f : 1.f);
                hy -= jab * sh * 0.10f;
            }
            SDL_FRect hd{hx - hr, hy - hr, hr * 2.f, hr * 2.f};
            set_color(ren, 0xFFBE9678); SDL_RenderFillRect(ren, &hd);
            set_color(ren, 0xFF8A6248); SDL_RenderRect(ren, &hd);
        }
        // ---- face ---------------------------------------------------------
        const float gaze_x = std::cos(an.ang) * hr * 0.9f;
        const float gaze_y = std::sin(an.ang) * hr * 0.4f;
        const float ex = sw * 0.16f;                            // half eye spacing
        float er = std::max(1.3f, sw * 0.085f);                  // eye radius
        er *= 1.f + an.fear * 0.55f;                            // fear -> wide eyes
        const float jit = an.fear * std::sin(float(time_s) * 43.f + float(e.id)) * 0.8f;
        const float hx = fx + gaze_x * 0.4f, hy = fy - sh * 0.80f;
        const float lid_k = an.tired > 0.55f ? 1.f - 0.45f * an.tired : 1.f;
        for (int i = 0; i < 2; ++i) {
            const float exx = hx + (i ? ex : -ex) + jit;
            const float eyy = hy + gaze_y * 0.3f;
            if (an.joy > 0.55f && an.fear < 0.4f) {              // happy squint
                SDL_FRect lid{exx - er * 1.4f, eyy - er * 0.35f,
                              er * 2.8f, std::max(1.f, er * 0.55f)};
                set_color(ren, 0xFF2A1E18); SDL_RenderFillRect(ren, &lid);
                continue;
            }
            SDL_FRect wht{exx - er, eyy - er * lid_k, er * 2.f, er * 2.f * lid_k};
            set_color(ren, 0xFFF6EEDC); SDL_RenderFillRect(ren, &wht);
            if (lid_k < 1.f) {                                  // heavy eyelid
                SDL_FRect lid{wht.x, wht.y, wht.w, wht.h * (1.f - lid_k) * 1.6f};
                set_color(ren, 0xFFBE9678); SDL_RenderFillRect(ren, &lid);
            }
            const float pr = er * 0.55f;                        // pupil w/ gaze
            SDL_FRect pu{exx + gaze_x - pr, eyy + gaze_y - pr, pr * 2.f, pr * 2.f};
            set_color(ren, 0xFF241A12); SDL_RenderFillRect(ren, &pu);
        }
        if (eating) {                                           // open mouth
            SDL_FRect mo{hx - er, hy + er * 2.4f, er * 2.f, er * 1.2f};
            set_color(ren, 0xFF5A1E1E); SDL_RenderFillRect(ren, &mo);
        }
        // hit-flash decal: brief white bloom over the sprite
        if (time_s < an.flash) {
            const float a = std::clamp((an.flash - float(time_s)) / 0.18f, 0.f, 1.f);
            set_color(ren, 0xFFFFFFFF, int(140 * a));
            SDL_FRect fl{fx - sw * 0.55f, fy - sh * 1.05f, sw * 1.1f, sh * 1.1f};
            SDL_RenderFillRect(ren, &fl);
        }
    }

    void draw_entities(double dt) {
        // hover pick: nearest living entity to the cursor (world_rt px)
        hover_id = kNoEntity;
        {
            const ImVec2 mp = ImGui::GetIO().MousePos;
            if (mp.x >= 0 && show_badges) {
                const float mxx = mp.x / kPix, myy = mp.y / kPix;
                float best = 28.f * 28.f;
                for (const EntityView& e : snap.entities) {
                    if (!e.alive || e.fog != Fog::Visible) continue;
                    const float fx = cam.sx(float(e.pos.x)), fy = cam.sy(float(e.pos.y), elev_at(e.pos));
                    const float d = (fx - mxx) * (fx - mxx) + (fy - myy) * (fy - myy);
                    if (d < best) { best = d; hover_id = e.id; }
                }
            }
        }
        for (const EntityView& e : snap.entities) {
            if (e.kind == "enclosure") { draw_enclosure(e); continue; }
            // ---- smooth visual position: exponential chase of sim pos ----
            // (viewer-side only; sim positions stay integer World Truth)
            const float tx = float(e.pos.x), ty = float(e.pos.y);
            auto vit = vis_pos.find(e.id);
            if (vit == vis_pos.end())
                vit = vis_pos.emplace(e.id, ImVec2(tx, ty)).first;
            else {
                const float ddx = tx - vit->second.x, ddy = ty - vit->second.y;
                if (ddx * ddx + ddy * ddy > 400.f * 400.f)      // spawn/teleport: snap
                    vit->second = ImVec2(tx, ty);
                else {
                    const float k = 1.f - std::exp(-float(dt) * 12.f);
                    vit->second.x += ddx * k; vit->second.y += ddy * k;
                }
            }
            const auto pit = vis_prev.find(e.id);
            const ImVec2 pp = pit == vis_prev.end() ? vit->second : pit->second;
            const float mvx = vit->second.x - pp.x, mvy = vit->second.y - pp.y;
            vis_prev[e.id] = vit->second;
            if (std::fabs(mvx) > 0.02f) facing[e.id] = mvx < 0 ? -1.f : 1.f;   // face travel dir
            const float spd = std::sqrt(mvx * mvx + mvy * mvy) / std::max(1e-3f, float(dt));
            const bool gliding = spd > 12.f;                     // world units / sec
            const float elev = elev_at(Vec2{std::int32_t(vit->second.x), std::int32_t(vit->second.y)});
            const float fx = cam.sx(vit->second.x), fy = cam.sy(vit->second.y, elev);
            // ---- procedural animation state (sapients, viewer-only) -------
            AnimState* an = nullptr;
            bool lying = false;
            double lean = 0.0;
            if (e.minded && e.alive && e.fog == Fog::Visible) {
                auto ait = c_anim.find(e.id);
                if (ait != c_anim.end()) an = &ait->second;
            }
            if (an) {
                an->phase += float(dt) * (gliding ? 9.f : 2.f);   // gait cycle
                // facing: remembered/intent target beats raw velocity; the
                // angle is smoothed so characters visibly turn toward mates,
                // water and predators instead of snapping left/right.
                float want = an->ang;
                bool has_dir = false;
                if ((an->until > c_tick || an->expr_until > c_tick) &&
                    an->target != kNoEntity && an->tpos_ok) {
                    const float sx2 = cam.sx(float(an->tpos.x));
                    const float sy2 = cam.sy(float(an->tpos.y), elev_at(an->tpos));
                    const float ddx = sx2 - fx, ddy = sy2 - fy;
                    if (ddx * ddx + ddy * ddy > 9.f) { want = std::atan2(ddy, ddx); has_dir = true; }
                }
                if (!has_dir && (std::fabs(mvx) > 0.03f || std::fabs(mvy) > 0.03f)) {
                    want = std::atan2(mvy, mvx); has_dir = true;
                }
                if (has_dir) {
                    float d = want - an->ang;
                    while (d > 3.14159265f) d -= 6.2831853f;
                    while (d < -3.14159265f) d += 6.2831853f;
                    an->ang += d * std::min(1.f, float(dt) * 9.f);
                }
                if (gliding) {                                     // body lean into motion
                    const float mdir = spd > 1.f ? mvx / (spd * float(dt)) : 0.f;
                    lean = std::clamp(mdir, -1.f, 1.f) * 6.0;
                }
                lying = an->lying && an->expr_until > c_tick;
                // expanding ripple while wading (throttled per entity)
                if (gliding && an->in_water && frames - an->last_puff > 18 && rings.size() < 48) {
                    an->last_puff = frames;
                    rings.push_back({fx, fy - cam.zoom, 2.f, 16.f, 0.8f, 0.8f, 0xFF7FC8E8});
                }
            }
            // motion memory -> bob / flip (viewer-side, never World state)
            auto lp = last_pos.find(e.id);
            if (lp != last_pos.end() && (lp->second.x != e.pos.x || lp->second.y != e.pos.y)) {
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
            float squash = 1.f, ox = 0, sox = 0;
            const bool moving = gliding || (bob_until.count(e.id) && bob_until[e.id] > time_s);
            if (creature && e.alive && moving) {               // squash & stretch hop
                const float ph = std::sin(float(time_s) * 12.f + float(e.id % 17));
                squash = 1.f + 0.08f * ph; sh *= squash; sw /= squash;
            } else if (creature && e.alive) {                  // idle breathing
                sh *= 1.f + 0.018f * std::sin(float(time_s) * 2.2f + float(e.id % 31));
            }
            if (e.flora && !creature && e.alive && e.fog != Fog::Shrouded)   // foliage sway (frozen memory under the shroud)
                sox = std::sin(float(time_s) * 1.25f + float((e.id * 2654435761u >> 8) % 1024) / 163.f) * 1.4f * cam.zoom;
            if (creature && (e.infected || e.toxin > 300 || e.temperature > 385))   // fever tremor
                ox = std::sin(float(time_s) * 40.f + float(e.id)) * 1.2f * cam.zoom;
            SDL_FRect dst{fx - sw / 2 + ox + sox, fy - sh + (lying ? sh * 0.22f : 0.f), sw, sh};
            // ground shadow, slanted by the sun
            const float sun = float((snap.day_phase - 12)) / 12.f;
            SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 70);
            SDL_FRect sh_r{fx - sw * 0.35f + sun * sw * 0.4f, fy - 2 * cam.zoom, sw * 0.7f, 3 * cam.zoom};
            SDL_RenderFillRect(ren, &sh_r);
            bool flip = facing.count(e.id) && facing[e.id] < 0;
            if (an && std::fabs(std::cos(an->ang)) > 0.25f) flip = std::cos(an->ang) < 0.f;
            SDL_Texture* t = tex.get(*sp, flip);                   // paper flip, pre-mirrored
            if (e.fog == Fog::Shrouded) SDL_SetTextureColorMod(t, 120, 120, 135);
            if (!e.alive) SDL_SetTextureColorMod(t, 150, 140, 130);
            double tilt = !e.alive ? 90.0 : e.routed ? -12.0 : 0.0;
            if (an && e.alive && !e.routed)
                tilt = lying ? (flip ? -90.0 : 90.0) : lean;      // rest pose / motion lean
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
            if (an && show_badges)
                draw_actor_anim(e, *an, fx, fy, sw, sh, gliding, lying);
            if (e.leader) {                                    // crown of authority
                SDL_SetRenderDrawColor(ren, 255, 210, 60, 255);
                for (int k = 0; k < 3; ++k) {
                    SDL_FRect cr{fx - 4 * cam.zoom + k * 3 * cam.zoom, fy - sh - 3 * cam.zoom, 2 * cam.zoom, 3 * cam.zoom};
                    SDL_RenderFillRect(ren, &cr);
                }
            }
            if (e.bleed > 0 && e.alive && (frames / 8) % 3 == 0)
                emit(fx, fy - sh * 0.5f, 0xFFA01010, 1);
            // gait dust: soft puffs kicked up at the feet while gliding
            if (creature && e.alive && moving && e.fog == Fog::Visible && parts.size() < 600) {
                auto dit = dust_at.find(e.id);
                if (dit == dust_at.end() || time_s - dit->second > 0.14) {
                    const float j = float((frames * 31 + int(e.id % 13)) % 17) / 17.f - 0.5f;
                    const float fv = facing.count(e.id) ? facing[e.id] : 1.f;
                    parts.push_back({fx + j * sw * 0.4f, fy - cam.zoom, -fv * 16.f + j * 20.f, -9.f, 0.5f, 0xD8C8B088});
                    dust_at[e.id] = float(time_s);
                }
            }
            const Vec2 vpos{std::int32_t(vit->second.x), std::int32_t(vit->second.y)};
            if (e.temperature >= 3000) {                        // fire / forge
                draw_glow(vpos, 0xFF8030, 1.2f);
                if (e.fog == Fog::Visible && frames % 6 == std::uint64_t(e.id % 6) && parts.size() < 600) {
                    const float j = float((frames + int(e.id % 7)) % 11) / 11.f;   // smoke + embers
                    parts.push_back({fx + (j - 0.5f) * 6.f, fy - sh * 0.9f, (j - 0.5f) * 10.f, -20.f - j * 14.f, 1.3f, 0xFFB9BDC6, -14.f});
                    if ((frames / 6) % 3 == 0)
                        parts.push_back({fx + (j - 0.5f) * 4.f, fy - sh * 0.7f, (j - 0.5f) * 16.f, -34.f - j * 20.f, 0.7f, 0xFFFFA040, 30.f});
                }
            }
            if (e.epic && lens == kSacred) draw_glow(vpos, 0xFFE090, 0.8f);
            draw_entity_lens(e, fx, fy, sw, sh);
            // thought/action badge: DF-style pill floating over the head,
            // fades out over its last ~8 world ticks (viewer-only state)
            if (creature && e.alive && e.fog == Fog::Visible && show_badges) {
                auto bit = c_badge.find(e.id);
                if (bit != c_badge.end() && bit->second.until > c_tick) {
                    const ThoughtBadge& b = bit->second;
                    const float rem = float(bit->second.until - c_tick);
                    const int alpha = int(235.f * std::min(1.f, rem / 8.f));
                    ImDrawList* dl = ImGui::GetForegroundDrawList();
                    const ImVec2 tp(ux(fx), ux(fy - sh) - 10.f);
                    const ImVec2 ts = ImGui::CalcTextSize(b.label.c_str());
                    const ImU32 bg = IM_COL32(10, 10, 14, alpha * 3 / 4);
                    const ImU32 fg = (b.col & 0x00FFFFFFu) | (ImU32(alpha) << 24);
                    dl->AddRectFilled(ImVec2(tp.x - ts.x * 0.5f - 4.f, tp.y - 2.f),
                                      ImVec2(tp.x + ts.x * 0.5f + 4.f, tp.y + ts.y + 2.f),
                                      bg, 3.f);
                    dl->AddRect(ImVec2(tp.x - ts.x * 0.5f - 4.f, tp.y - 2.f),
                                ImVec2(tp.x + ts.x * 0.5f + 4.f, tp.y + ts.y + 2.f),
                                (b.col & 0x00FFFFFFu) | (ImU32(alpha * 2 / 3) << 24), 3.f);
                    dl->AddText(ImVec2(tp.x - ts.x * 0.5f, tp.y), fg, b.label.c_str());
                }
            }
            if (e.id == selected) {
                SDL_SetRenderDrawColor(ren, 255, 230, 120, 255);
                SDL_RenderRect(ren, &dst);
            }
        }
        // particles
        for (Particle& p : parts) {
            p.life -= float(dt); p.x += p.vx * float(dt); p.y += p.vy * float(dt); p.vy += p.grav * float(dt);
            if (p.life <= 0) continue;
            SDL_SetRenderDrawColor(ren, (p.c >> 16) & 0xFF, (p.c >> 8) & 0xFF, p.c & 0xFF, Uint8(255 * std::min(1.f, p.life * 2)));
            SDL_FRect d{p.x, p.y, 2 * cam.zoom, 2 * cam.zoom};
            SDL_RenderFillRect(ren, &d);
        }
        parts.erase(std::remove_if(parts.begin(), parts.end(), [](const Particle& p) { return p.life <= 0; }), parts.end());
        // ---- ambient particle layers (viewer-only juice) ------------------
        // expanding rings: wading ripples + speech waves; rising hearts on
        // births; floating "z" on sleepers. All fade with their own TTL.
        {
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            for (Ring& r : rings) {
                r.life -= float(dt); r.r += r.vr * float(dt);
                if (r.life <= 0.f) continue;
                const int a = int(210.f * (r.life / r.ttl));
                dl->AddCircle(ImVec2(ux(r.x), ux(r.y)), r.r * float(kPix),
                              (r.c & 0x00FFFFFFu) | (ImU32(a) << 24), 1.5f, 24);
            }
            rings.erase(std::remove_if(rings.begin(), rings.end(), [](const Ring& r) { return r.life <= 0.f; }), rings.end());
            auto heart = [&](ImVec2 c, float s, ImU32 col) {
                dl->AddCircleFilled(ImVec2(c.x - s * 0.34f, c.y - s * 0.16f), s * 0.36f, col, 10);
                dl->AddCircleFilled(ImVec2(c.x + s * 0.34f, c.y - s * 0.16f), s * 0.36f, col, 10);
                dl->AddTriangleFilled(ImVec2(c.x - s * 0.66f, c.y - s * 0.04f),
                                      ImVec2(c.x + s * 0.66f, c.y - s * 0.04f),
                                      ImVec2(c.x, c.y + s * 0.60f), col);
            };
            for (Heart& h : hearts) {
                h.t += float(dt);
                if (h.t > 1.4f) continue;
                const float a = 1.f - h.t / 1.4f;
                const float wob = std::sin(h.t * 9.f) * 1.5f;
                heart(ImVec2(ux(h.x + wob), ux(h.y - h.t * 10.f)), 4.2f,
                      IM_COL32(255, 110, 150, int(235 * a)));
            }
            hearts.erase(std::remove_if(hearts.begin(), hearts.end(), [](const Heart& h) { return h.t > 1.4f; }), hearts.end());
            for (Zed& z : zeds) {
                z.t += float(dt);
                if (z.t > 1.7f) continue;
                const float a = z.t < 0.3f ? z.t / 0.3f : 1.f - (z.t - 0.3f) / 1.4f;
                const float wob = std::sin(z.t * 3.f + z.x) * 1.2f;
                dl->AddText(ImVec2(ux(z.x + wob), ux(z.y - z.t * 8.f)),
                            IM_COL32(170, 200, 235, int(220 * a)), "z");
            }
            zeds.erase(std::remove_if(zeds.begin(), zeds.end(), [](const Zed& z) { return z.t > 1.7f; }), zeds.end());
        }
        // prune aux animation maps: entries for entities that died or left
        // the snapshot would otherwise grow forever.
        if (frames % 120 == 0 && !snap.entities.empty()) {
            std::unordered_set<EntityId> live;
            live.reserve(snap.entities.size());
            for (const EntityView& e : snap.entities) live.insert(e.id);
            auto prune = [&](auto& m) {
                for (auto it = m.begin(); it != m.end();)
                    it = live.count(it->first) ? std::next(it) : m.erase(it);
            };
            prune(last_pos); prune(facing); prune(bob_until); prune(last_integ); prune(last_pain);
            prune(vis_pos); prune(vis_prev); prune(dust_at);
            prune(c_badge); prune(c_intent); prune(c_anim);
        }
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
        if (lens == kEcology && e.fauna) {
            // apex predators (tier >= 4) get a red ring; migrants a chevron
            if (e.trophic_tier >= 4)
                dl->AddCircle(ImVec2(fx, fy - sh / 2), 9 * cam.zoom * kPix,
                              IM_COL32(220, 50, 40, 200), 20, 1.5f);
            if (e.migration_drive > 0) {
                const float ay = fy - sh - 14.f * cam.zoom;
                dl->AddTriangleFilled(ImVec2(fx, ay - 4.f * cam.zoom),
                                      ImVec2(fx - 4.f * cam.zoom, ay + 3.f * cam.zoom),
                                      ImVec2(fx + 4.f * cam.zoom, ay + 3.f * cam.zoom),
                                      IM_COL32(240, 210, 90, 230));
            }
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

    // DF-style intent lines: a dashed line from the selected (or hovered)
    // NPC to the entity its mind/brain is currently reaching for.
    static void dashed_seg(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col,
                           float dash = 7.f, float gap = 5.f, float thick = 1.5f) {
        const float dx = b.x - a.x, dy = b.y - a.y;
        const float len = std::hypot(dx, dy);
        if (len < 1.f) return;
        const float ux_ = dx / len, uy_ = dy / len;
        for (float t = 0.f; t < len; t += dash + gap) {
            const float e = std::min(t + dash, len);
            dl->AddLine(ImVec2(a.x + ux_ * t, a.y + uy_ * t),
                        ImVec2(a.x + ux_ * e, a.y + uy_ * e), col, thick);
        }
    }
    void draw_intents() {
        if (!show_badges || globe) return;
        // chosen actor: explicit selection wins, else hovered entity
        auto fresh = [&](EntityId id) {
            auto it = c_intent.find(id);
            return it != c_intent.end() && it->second.until > c_tick;
        };
        EntityId who = kNoEntity;
        if (selected != kNoEntity && fresh(selected)) who = selected;
        else if (hover_id != kNoEntity && fresh(hover_id)) who = hover_id;
        if (who == kNoEntity) return;
        auto iit = c_intent.find(who);
        if (iit == c_intent.end() || iit->second.until <= c_tick) return;
        const EntityView* src = nullptr;
        const EntityView* dst = nullptr;
        for (const EntityView& e : snap.entities) {
            if (e.id == who) src = &e;
            if (e.id == iit->second.target) dst = &e;
            if (src && dst) break;
        }
        if (!src || !src->alive || src->fog != Fog::Visible) return;
        const float ax = ux(cam.sx(float(src->pos.x)));
        const float ay = ux(cam.sy(float(src->pos.y), elev_at(src->pos)));
        float bx, by;
        if (dst && dst->fog == Fog::Visible) {
            bx = ux(cam.sx(float(dst->pos.x)));
            by = ux(cam.sy(float(dst->pos.y), elev_at(dst->pos)));
        } else {
            bx = ux(cam.sx(float(iit->second.pos.x)));
            by = ux(cam.sy(float(iit->second.pos.y)));
        }
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const ImU32 col = who == selected ? IM_COL32(255, 225, 120, 200) : IM_COL32(150, 210, 255, 170);
        dashed_seg(dl, ImVec2(ax, ay - 14.f), ImVec2(bx, by - 10.f), col);
        if (!iit->second.label.empty()) {
            const ImVec2 tp((ax + bx) * 0.5f, (ay + by) * 0.5f - 8.f);
            const ImVec2 ts = ImGui::CalcTextSize(iit->second.label.c_str());
            dl->AddRectFilled(ImVec2(tp.x - ts.x * 0.5f - 3.f, tp.y - 1.f),
                              ImVec2(tp.x + ts.x * 0.5f + 3.f, tp.y + ts.y + 1.f),
                              IM_COL32(10, 10, 14, 170), 2.f);
            dl->AddText(ImVec2(tp.x - ts.x * 0.5f, tp.y), IM_COL32(235, 225, 180, 230),
                        iit->second.label.c_str());
        }
    }

    // Phase 15: narrative callouts — short story lines floating over the spot
    // where they happened, rising and fading over ~4 s of render time.
    void draw_callouts() {
        if (c_callouts.empty()) return;
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        for (auto it = c_callouts.begin(); it != c_callouts.end();) {
            if (it->until <= frames) { it = c_callouts.erase(it); continue; }
            const float life = float(it->until - frames) / 260.f;   // 1 -> 0
            const float rise = (1.f - life) * 16.f * cam.zoom;
            const float px = ux(cam.sx(float(it->pos.x)));
            const float py = ux(cam.sy(float(it->pos.y))) - 30.f * cam.zoom - it->oy - rise;
            const ImVec2 ts = ImGui::CalcTextSize(it->text.c_str());
            const int alpha = int(225.f * std::min(1.f, life * 2.5f));
            dl->AddRectFilled(ImVec2(px - ts.x * 0.5f - 3.f, py - 2.f),
                              ImVec2(px + ts.x * 0.5f + 3.f, py + ts.y + 2.f),
                              IM_COL32(8, 8, 12, alpha * 3 / 4), 3.f);
            dl->AddText(ImVec2(px - ts.x * 0.5f, py),
                        (it->col & 0x00FFFFFFu) | (ImU32(alpha) << 24), it->text.c_str());
            ++it;
        }
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
    // ---- HUD: one slim bar. Time | day | tribe ......... Resonance Journal View Menu
    bool show_perf = false;
    std::string toast_msg;
    double toast_t = -10;
    void ui_topbar() {
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(float(win_w), 0), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 6));
        ImGui::Begin("##hud", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse);
        // time controls: play/pause + speed segments
        if (ImGui::Button(speed ? "||" : " > ", ImVec2(34, 0))) { speed = speed ? 0 : std::max(1, last_speed); speed_atomic = speed; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pause / resume (Space)");
        static const char* kSp[] = {"1x", "5x", "20x", "100x"};
        for (int i = 1; i <= 4; ++i) {
            ImGui::SameLine(0, 2);
            const bool on = speed == i;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70f, 0.52f, 0.22f, 1));
            if (ImGui::Button(kSp[i - 1], ImVec2(42, 0))) { speed = i; speed_atomic = i; last_speed = i; }
            if (on) ImGui::PopStyleColor();
        }
        ImGui::SameLine(0, 8);
        const int wr = warp.load();
        if (wr > 0) ImGui::TextColored(ImVec4(1.f, 0.75f, 0.35f, 1.f), "warp %d", wr);
        if (ImGui::Button(wr > 0 ? "Warp.." : "Time", ImVec2(48, 0))) ImGui::OpenPopup("warppop");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Headless time-warp + evolution god-tools");
        if (ImGui::BeginPopup("warppop")) {
            ImGui::TextDisabled("Headless warp (cycles, no render):");
            if (ImGui::Button("+100", ImVec2(52, 0))) warp.fetch_add(100);
            ImGui::SameLine();
            if (ImGui::Button("+1k", ImVec2(52, 0))) warp.fetch_add(1000);
            ImGui::SameLine();
            if (ImGui::Button("+10k", ImVec2(56, 0))) warp.fetch_add(10000);
            if (wr > 0 && ImGui::Button("Stop", ImVec2(52, 0))) warp.store(0);
            ImGui::Separator();
            int mp = mut_permille.load();
            ImGui::Text("Global mutation rate:");
            ImGui::SetNextItemWidth(170);
            if (ImGui::SliderInt("##mut", &mp, -1, 1000, mp < 0 ? "auto" : "%d"))
                mut_permille.store(mp);
            ImGui::TextDisabled("permille per locus; -1 = each species' own rate");
            ImGui::Separator();
            ImGui::TextDisabled("Gene pool (hall of fame): %zu / %zu",
                                gene_pool.size(), GenePool::kMaxEntries);
            ImGui::EndPopup();
        }
        ImGui::SameLine(0, 16);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Day %llu  %02d:00", (unsigned long long)(c_tick / 24), int(c_tick % 24));
        if (pw.tribe_lang.size() > 1 && fog.mode() != FogOfWar::Mode::GodEye) {
            ImGui::SameLine(0, 16); ImGui::SetNextItemWidth(110);
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
        // right cluster
        const float right = 150 + 8 + 74 + 8 + 66 + 8 + 66 + 12;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 12, float(win_w) - right));
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.45f, 0.68f, 0.92f, 1));
        ImGui::ProgressBar(float(journal.resonance) / float(std::max<std::int64_t>(1, journal.resonance_max)), ImVec2(150, 0),
                           ("Resonance " + std::to_string(journal.resonance)).c_str());
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Your power as Patron. Grows when your people live through great moments.");
        ImGui::SameLine(); if (ImGui::Button("Journal", ImVec2(74, 0))) show_journal = !show_journal;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rosetta lexicon, chronicle, legends, notes (J)");
        ImGui::SameLine(); if (ImGui::Button("View", ImVec2(66, 0))) ImGui::OpenPopup("view");
        ImGui::SameLine(); if (ImGui::Button("Menu", ImVec2(66, 0))) open_menu(kPause);
        if (ImGui::BeginPopup("view")) {
            ImGui::TextDisabled("Vision");
            bool tribe = fog.mode() != FogOfWar::Mode::GodEye;
            if (ImGui::RadioButton("What my people see", tribe) && !tribe) fog.set_mode(FogOfWar::Mode::Tribe, tribe_lang());
            if (ImGui::RadioButton("The whole world", !tribe) && tribe) fog.set_mode(FogOfWar::Mode::GodEye);
            ImGui::Separator();
            ImGui::TextDisabled("Map overlay");
            static const char* kOv[] = {"None", "Disease", "Wealth & trade", "Power", "Faith", "War", "Ecosystem"};
            ImGui::SetNextItemWidth(170); ImGui::Combo("##ov", &lens, kOv, 7);
            ImGui::Separator();
            ImGui::TextDisabled("Panels");
            ImGui::Checkbox("Inspector on selection", &show_inspector);
            ImGui::Checkbox("Thought badges (B)", &show_badges);
            ImGui::Checkbox("Event ticker", &show_ticker);
            ImGui::Checkbox("Field journal (chronicle)", &show_journal);
            ImGui::Checkbox("Chronicle & Rosetta codex", &show_codex);
            ImGui::Checkbox("Sprite atlas", &show_atlas);
            ImGui::Checkbox("Performance stats", &show_perf);
            if (is_planet()) { ImGui::Separator(); if (ImGui::MenuItem("Planet view", "M")) enter_globe(); }
            ImGui::EndPopup();
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
        if (show_perf) {
            ImGui::SetNextWindowPos(ImVec2(10, 48), ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.6f);
            ImGui::Begin("##perf", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                            ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings);
            ImGui::Text("FPS %.0f  RSS %zu MB  entities %zu  minds %zu", ImGui::GetIO().Framerate, rss_kb() / 1024,
                        snap.entities.size(), c_minds);
            ImGui::End();
        }
    }
    // status messages: a toast above the bottom edge, fading after 4 s
    void ui_toast() {
        if (status_msg != toast_msg) { toast_msg = status_msg; toast_t = time_s; }
        const double age = time_s - toast_t;
        if (toast_msg.empty() || age > 4.5) return;
        const float a = float(std::clamp(4.5 - age, 0.0, 1.0));
        const ImVec2 sz = ImGui::CalcTextSize(toast_msg.c_str());
        ImGui::SetNextWindowPos(ImVec2((win_w - sz.x) * 0.5f - 14, 50), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.85f * a);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, a);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8);
        ImGui::Begin("##toast", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                         ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::TextColored(ImVec4(1, 0.88f, 0.6f, 1), "%s", toast_msg.c_str());
        ImGui::End();
        ImGui::PopStyleVar(2);
    }

    // ---- live event ticker ("jump-to-drama"): last notable chronicle lines -
    // click flies the camera to the spot and selects the actor. Pure viewer
    // state read from cfeed; nothing here touches World.
    void ui_ticker() {
        if (!show_ticker || !has_game || globe || in_menu) return;
        // pinned bottom-left; ~10 wrapped lines max so 240px clears the HUD
        ImGui::SetNextWindowPos(ImVec2(10, float(win_h) - 240), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.72f);
        ImGui::Begin("##ticker", nullptr, ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
        ImGui::TextDisabled("chronicle");
        int shown = 0;
        for (auto it = cfeed.lines.rbegin(); it != cfeed.lines.rend() && shown < 10; ++it) {
            const NarrativeLine& l = *it;
            if (!(l.cat & (kCatCombat | kCatSurvival | kCatDiscovery | kCatSociety | kCatPinned))) continue;
            ++shown;
            ImGui::PushID(static_cast<int>(l.seq % 100000));
            ImGui::PushStyleColor(ImGuiCol_Text,
                l.llm ? ImVec4(1.f, 0.9f, 0.55f, 1)
                : (l.cat & kCatCombat) ? ImVec4(1.f, 0.55f, 0.45f, 1)
                : (l.cat & kCatDiscovery) ? ImVec4(1.f, 0.82f, 0.5f, 1)
                : (l.cat & kCatSociety) ? ImVec4(0.6f, 0.9f, 0.95f, 1)
                : (l.cat & kCatSurvival) ? ImVec4(0.65f, 0.95f, 0.6f, 1)
                : ImVec4(0.85f, 0.85f, 0.88f, 1));
            if (ImGui::Selectable(l.text.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick,
                                  ImVec2(520, 0))) {
                cam_gx = float(l.pos.x); cam_gy = float(l.pos.y); cam_gz = cam.zoom;
                cam_goal = true;
                if (l.who != kNoEntity) selected = l.who;
                else if (l.about != kNoEntity) selected = l.about;
                if (globe) { globe = false; globe_dive = false; }
            }
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::End();
    }

    // ---- menus: Title -> New Game / Continue / Load ; in game Esc -> Pause --------
    enum Screen { kTitle = 0, kNewGame, kLoadGame, kSaveGame, kPause };
    struct SlotInfo { bool exists = false; std::string when; long long day = 0; std::filesystem::file_time_type mtime{}; };
    SlotInfo slot_info(int slot) const {
        SlotInfo si;
        std::error_code ec;
        if (!std::filesystem::exists(slot_path(slot), ec)) return si;
        si.exists = true;
        si.mtime = std::filesystem::last_write_time(slot_path(slot), ec);
        const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(si.mtime);
        const std::time_t tt = std::chrono::system_clock::to_time_t(sys);
        char buf[64]; std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", std::localtime(&tt));
        si.when = buf;
        if (std::ifstream m(slot_path(slot) + ".meta"); m) {
            long long a, b, c, d, tk = 0; m >> a >> b >> c >> d >> tk; si.day = tk / 24;
        }
        return si;
    }
    int newest_slot() const {          // most recent save of any kind, -1 none
        int best = -1; std::filesystem::file_time_type bt{};
        for (int s = 0; s <= 5; ++s) {
            const SlotInfo si = slot_info(s);
            if (si.exists && (best < 0 || si.mtime > bt)) { best = s; bt = si.mtime; }
        }
        return best;
    }
    static const char* slot_name(int s) { static const char* k[] = {"Autosave", "Slot 1", "Slot 2", "Slot 3", "Slot 4", "Slot 5"}; return k[std::clamp(s, 0, 5)]; }
    void open_menu(int scr) { in_menu = true; screen = scr; speed = 0; speed_atomic = 0; ui_dirty = true; }
    void close_menu() { in_menu = false; if (!speed) { speed = 1; speed_atomic = 1; } }
    bool big_button(const char* label, bool enabled = true) {
        ImGui::BeginDisabled(!enabled);
        const bool r = ImGui::Button(label, ImVec2(-1, 44));
        ImGui::EndDisabled();
        return r && enabled;
    }
    void ui_menu() {
        const float W = 480, H = screen == kTitle ? 330.f : screen == kPause ? 380.f : 520.f;
        ImGui::SetNextWindowPos(ImVec2((win_w - W) / 2, (win_h - H) / 2), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(W, H), ImGuiCond_Always);
        ImGui::Begin("##menu", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
        ImGui::SetWindowFontScale(1.6f);
        ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.45f, 1), "SUNNYWORLD");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::TextDisabled("Primordial Sandbox");
        ImGui::Separator();
        if (screen == kTitle) {
            const int ns = newest_slot();
            ImGui::TextWrapped("A naked planet. A few bands of humans who know only hunger, thirst, cold, fear and "
                               "curiosity. No words, no tools, no gods - whatever they become, they discover.");
            ImGui::Spacing();
            std::string cont = "Continue";
            if (ns >= 0) { const SlotInfo si = slot_info(ns); cont += std::string("   (") + slot_name(ns) + ", day " + std::to_string(si.day) + ")"; }
            if (big_button(cont.c_str(), ns >= 0)) { if (load_slot(ns)) close_menu(); }
            if (big_button("New Game")) screen = kNewGame;
            if (big_button("Load Game", ns >= 0)) screen = kLoadGame;
            if (big_button("Quit")) want_quit = true;
        } else if (screen == kPause) {
            ImGui::Text("Paused  -  day %llu", (unsigned long long)(c_tick / 24));
            ImGui::Spacing();
            if (big_button("Resume")) close_menu();
            if (big_button("Save Game")) screen = kSaveGame;
            if (big_button("Load Game")) screen = kLoadGame;
            if (big_button("New Game")) screen = kNewGame;
            if (big_button("Save & Quit to Title")) { save_slot(0); has_game = false; screen = kTitle; }
        } else if (screen == kNewGame) {
            ImGui::TextUnformatted("New Primordial World");
            ImGui::Spacing();
            ImGui::SetNextItemWidth(200); ImGui::InputText("World seed", seed_buf, sizeof seed_buf, ImGuiInputTextFlags_CharsDecimal);
            ImGui::SameLine();
            if (ImGui::Button("Random")) {
                const std::uint64_t r = SDL_GetTicksNS() ^ (std::uint64_t(SDL_GetPerformanceCounter()) << 17);
                std::snprintf(seed_buf, sizeof seed_buf, "%llu", (unsigned long long)(r % 1000000000ull));
            }
            static const char* kSizes[] = {"Local Valley (64 x 64)", "Medium planet (1024 x 512)",
                                           "Large planet (2048 x 1024)", "Colossal planet (4096 x 2048)"};
            static const int kGrid[] = {64, 128, 128, 128};
            static const int kPW[] = {0, 1024, 2048, 4096};
            ImGui::SetNextItemWidth(280); ImGui::Combo("World", &menu_grid, kSizes, 4);
            ImGui::TextDisabled(menu_grid > 0 ? "Whole planet to explore; your bands live in a %d x %d home region." : "A small valley, fast on any machine.",
                                kGrid[std::clamp(menu_grid, 0, 3)], kGrid[std::clamp(menu_grid, 0, 3)] * 2 / 3);
            ImGui::SetNextItemWidth(200); ImGui::SliderInt("Starting tribes", &menu_tribes, 1, 4);
            ImGui::SetNextItemWidth(200); ImGui::SliderInt("Humans per tribe", &menu_per, 3, 12);
            ImGui::Spacing();
            if (has_game) ImGui::TextColored(ImVec4(1, 0.7f, 0.4f, 1), "The current world will be autosaved first.");
            if (big_button("Create world")) {
                PrimordialParams p;
                p.seed = std::strtoull(seed_buf, nullptr, 10);
                const int mi = std::clamp(menu_grid, 0, 3);
                p.grid = kGrid[mi];
                p.planet = kPW[mi] > 0; p.planet_w = kPW[mi]; p.planet_h = kPW[mi] / 2;
                p.tribes = menu_tribes; p.per_tribe = menu_per;
                if (has_game) save_slot(0);
                { std::lock_guard<std::mutex> lk(mx); new_world(p); }
                has_game = true;
                close_menu();
            }
            if (big_button("Back")) screen = has_game ? kPause : kTitle;
        } else if (screen == kLoadGame || screen == kSaveGame) {
            const bool saving = screen == kSaveGame;
            ImGui::TextUnformatted(saving ? "Save Game" : "Load Game");
            ImGui::TextDisabled("%s", pref_dir.c_str());
            ImGui::Spacing();
            for (int s = saving ? 1 : 0; s <= 5; ++s) {
                const SlotInfo si = slot_info(s);
                ImGui::PushID(s);
                const std::string label = std::string(slot_name(s)) +
                    (si.exists ? "   day " + std::to_string(si.day) + "   " + si.when : std::string("   - empty -"));
                if (big_button(label.c_str(), saving || si.exists)) {
                    if (saving) { save_slot(s); screen = kPause; }
                    else if (load_slot(s)) close_menu();
                }
                ImGui::PopID();
            }
            if (big_button("Back")) screen = has_game ? kPause : kTitle;
        }
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

    // ---- CL-1 Phase 4: gene pool persistence + god-tools ---------------------
    // All run under the world lock on the UI thread (via post) or the sim
    // thread (periodic harvest). The pool itself is read-only world input —
    // bless/spawn writes only affect FUTURE brains, never live state_hash.
    void ensure_gene_pool() {                 // caller holds mx
        if (gene_pool_loaded) return;
        gene_pool_loaded = true;
        gene_pool.load("saves/gene_pool.bin", w.props);
    }
    void attach_gene_pool() {                 // caller holds mx, w fresh
        ensure_gene_pool();
        w.gene_pool = &gene_pool;
    }
    void harvest_gene_pool() {                // caller holds mx
        if (!w.brains_) return;
        const PropId age_p = SIMLAB_PID(w.props, "age");
        for (const auto& [mid, mind] : w.minds_) {
            const Entity* e = w.find(mid);
            if (!e || !e->alive || e->kind_name != SIMLAB_KIND("sapient"))
                continue;
            if (!w.brains_->slots.count(mid)) continue;
            GenePoolEntry en;
            en.brain = brain_genome_of(w, *w.brains_, mid);
            en.body = e->genome;
            en.kind = "sapient";
            en.seen_tick = w.clock_.tick();
            // spec 2.3 pure natural selection: fitness = reproductive
            // lineage — offspring alive past their species' adult_age,
            // tie-broken by cumulative scarcity survived (milli-ticks).
            // The old age+discovery+memory heuristic is gone: a long-lived
            // sage with no descendants no longer outranks a prolific
            // survivor of lean seasons.
            const PropId adult_p = SIMLAB_PID(w.props, "adult_age");
            const PropId scarc_p = SIMLAB_PID(w.props, "scarcity_survived");
            std::int64_t offspring = 0;
            for (const Relation& rel : w.relations_of(mid, "parent_of")) {
                if (rel.a != mid) continue;
                const Entity* ch = w.find(rel.b);
                if (!ch || !ch->alive) continue;
                const std::int64_t mat = adult_p != kNoProp
                    ? w.prop_int(*ch, adult_p, 200) : 200;
                if (w.prop_int(*ch, age_p, 0) >= mat) ++offspring;
            }
            const std::int64_t scarcity =
                scarc_p != kNoProp ? w.prop_int(*e, scarc_p, 0) : 0;
            en.fitness = offspring * 1000000 +
                std::min<std::int64_t>(999999, scarcity);
            gene_pool.consider(en);
        }
    }
    void save_gene_pool() {                   // caller holds mx
        harvest_gene_pool();
        gene_pool.save("saves/gene_pool.bin", w.props);
    }
    // Global mutation pressure: rewrite `mutation_rate` on every living
    // genome-bearing organism (influx templates keep their own until they
    // too are swept — the slider is a god-tool, not a law).
    void apply_mutation_override() {          // caller holds mx
        const int mp = mut_permille.load();
        if (mp < 0) return;
        const PropId mr = SIMLAB_PID(w.props, "mutation_rate");
        if (mr == kNoProp) return;
        for (const auto& ep : w.entities) {
            Entity* e = ep.get();
            if (!e || !e->alive || e->genome.loci.empty()) continue;
            e->props.set(mr, Value::integer(mp));
        }
    }
    void do_bless_clone(EntityId id) {
        const Entity* e = w.find(id);
        if (!e || !e->alive || e->genome.loci.empty()) {
            status_msg = "Bless: pick a living organism with a genome";
            return;
        }
        if (!w.brains_) { status_msg = "Bless: no brains allocated yet"; return; }
        const BrainGenome bg = brain_genome_of(w, *w.brains_, id);
        if (!bg.valid) { status_msg = "Bless: that mind has no brain yet"; return; }
        GenePoolEntry en;
        en.brain = bg; en.body = e->genome;
        en.kind = e->kind_name + ""; en.seen_tick = w.clock_.tick();
        gene_pool.bless(en);
        save_gene_pool();
        // spawn 4 lightly-mutated copies around the blessed one
        const PropId age_p = SIMLAB_PID(w.props, "age");
        const PropId adult_p = SIMLAB_PID(w.props, "adult");
        const std::int64_t adult_age =
            adult_p != kNoProp ? w.prop_int(*e, adult_p, 250) : 250;
        int made = 0;
        for (int i = 0; i < 4; ++i) {
            const Vec2 cp{e->pos.x + (i % 2 ? 28 : -28),
                          e->pos.y + (i < 2 ? -28 : 28)};
            PropertyBag pb = e->props;
            if (age_p != kNoProp)
                pb.set(age_p, Value::integer(adult_age));
            const EntityId nid =
                w.create_entity(e->kind_name + "", e->material, cp, pb);
            if (nid == kNoEntity) continue;
            if (Entity* ne = w.find(nid)) ne->genome = e->genome;
            BrainGenome cg = bg;
            brain_mutate_blocks(cg.slow.data(),
                                static_cast<std::int64_t>(cg.slow.size()),
                                w.world_seed, nid, w.clock_.tick(), 4, 350);
            brain_install_spawn(w, nid, cg);
            w.build_anatomy(nid);
            ++made;
        }
        status_msg = "Blessed " + sid(id) + " into the gene pool - " +
                     std::to_string(made) + " clones born";
    }
    void bless_clone(EntityId id) { post([=, this] { do_bless_clone(id); }); }
    void do_punish(EntityId id) {
        const Entity* e = w.find(id);
        if (!e || !e->alive) { status_msg = "Punish: no living entity"; return; }
        AgentMind& m = w.mind(id);   // get-or-create (god-tool, same as whisper)
        // Shock the three freshest memories aversive + spike the acute axes:
        // the WM replay and fast weights then consolidate "don't repeat".
        int marked = 0;
        for (auto it = m.memory.begin();
             it != m.memory.end() && marked < 3; ++it) {
            MemoryEngram& g = it->second.engram;
            g.emotional_charge = std::min<std::int64_t>(g.emotional_charge, -900);
            g.stability = std::max<std::int64_t>(g.stability, 6000);
            ++marked;
        }
        m.neuro.panic = std::min<std::int64_t>(1000, m.neuro.panic + 500);
        m.neuro.rage = std::min<std::int64_t>(1000, m.neuro.rage + 400);
        m.neuro.grief = std::min<std::int64_t>(1000, m.neuro.grief + 200);
        status_msg = "Punished " + sid(id) + ": " +
                     std::to_string(marked) + " memories turned aversive";
    }
    void punish(EntityId id) { post([=, this] { do_punish(id); }); }
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
    // ---- action bar (bottom-left): powers, or the possessed body's verbs ----
    void ui_player() {
        ImGui::SetNextWindowPos(ImVec2(10, float(win_h) - 10), ImGuiCond_Always, ImVec2(0, 1));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));
        ImGui::Begin("##actions", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        if (avatar != kNoEntity) {
            ImGui::TextColored(ImVec4(0.6f, 1, 0.6f, 1), "You are %s", sid(avatar).c_str());
            ImGui::SameLine(); ImGui::TextDisabled("  %s", c_av_line.c_str());
            int n = 0;
            for (const std::string& v : c_av_verbs) {
                if (v == "move") continue;
                if (n % 6) ImGui::SameLine(0, 4);
                ++n;
                if (ImGui::Button(v.c_str(), ImVec2(90, 0))) avatar_act(v);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Key %d - acts on the thing you last clicked", n);
            }
            ImGui::TextDisabled("WASD walk   click: target (%zu picked)", av_targets.size());
            ImGui::SameLine();
            if (ImGui::SmallButton("Release")) set_possess(kNoEntity);
            ImGui::End();
            ImGui::PopStyleVar();
            return;
        }
        auto power = [&](const char* label, int t, int cost, const char* tip) {
            const bool on = tool == t;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.48f, 0.70f, 1));
            ImGui::BeginDisabled(journal.resonance < cost && !on);
            if (ImGui::Button(label, ImVec2(104, 0))) tool = on ? kToolNone : t;
            ImGui::EndDisabled();
            if (on) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s (costs %d)", tip, cost);
        };
        power("Beacon", kToolBeacon, 10, "Click the map: two scouts feel drawn there");
        ImGui::SameLine(0, 4);
        power("Blueprint", kToolBlueprint, 25, "Drag a circle: builders dream of a shelter there");
        ImGui::SameLine(0, 4);
        power("Dread", kToolFear, 15, "Click a thing: your people who remember it come to fear it");
        if (tool != kToolNone)
            ImGui::TextColored(ImVec4(0.6f, 0.82f, 1, 1), tool == kToolBlueprint ? "Drag on the map - right click cancels"
                                                                                 : "Click the map - right click cancels");
        if (insp.found && insp.has_mind) {
            ImGui::Separator();
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s", sid(selected).c_str());
            ImGui::SameLine();
            ImGui::BeginDisabled(journal.resonance < 20);
            if (ImGui::Button("Plant idea", ImVec2(96, 0))) ImGui::OpenPopup("idea");
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Whisper a hint into a dream (costs 20)");
            ImGui::SameLine(0, 4); if (ImGui::Button("Possess", ImVec2(80, 0))) set_possess(selected);
            ImGui::SameLine(0, 4);
            const bool pinned = journal.pins.count(selected) != 0;
            if (ImGui::Button(pinned ? "Unpin" : "Pin", ImVec2(60, 0))) { if (pinned) journal.pins.erase(selected); else journal.pins.insert(selected); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Follow this life in Journal > Legends");
            ImGui::SameLine(0, 4);
            if (ImGui::Button("Bless", ImVec2(52, 0))) bless_clone(selected);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hall-of-fame its brain + spawn 4 mutated clones");
            ImGui::SameLine(0, 4);
            if (ImGui::Button("Punish", ImVec2(56, 0))) punish(selected);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Aversive shock: recent memories + acute stress axes");
            if (ImGui::BeginPopup("idea")) {
                ImGui::TextDisabled("Make it wonder about...");
                const auto& verbs = curiosity_verbs();
                ImGui::SetNextItemWidth(150);
                if (ImGui::BeginCombo("action", verbs.empty() ? "" : verbs[std::size_t(std::clamp<int>(whisper_verb, 0, int(verbs.size()) - 1))])) {
                    for (int i = 0; i < int(verbs.size()); ++i) if (ImGui::Selectable(verbs[std::size_t(i)], i == whisper_verb)) whisper_verb = i;
                    ImGui::EndCombo();
                }
                const auto& mats = w.materials.defs();
                const char* mname = whisper_mat > 0 && whisper_mat <= int(mats.size()) ? mats[std::size_t(whisper_mat - 1)].name.c_str() : "anything";
                ImGui::SetNextItemWidth(150);
                if (ImGui::BeginCombo("on", mname)) {
                    if (ImGui::Selectable("anything", whisper_mat == 0)) whisper_mat = 0;
                    for (int i = 0; i < int(mats.size()); ++i)
                        if (ImGui::Selectable(mats[std::size_t(i)].name.c_str(), whisper_mat == i + 1)) whisper_mat = i + 1;
                    ImGui::EndCombo();
                }
                if (ImGui::Button("Whisper (20)", ImVec2(-1, 0))) { whisper(selected); ImGui::CloseCurrentPopup(); }
                ImGui::EndPopup();
            }
        } else if (insp.found) {
            ImGui::Separator();
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s", sid(selected).c_str());
            ImGui::SameLine();
            ImGui::BeginDisabled(journal.resonance < 15);
            if (ImGui::Button("Dread of this", ImVec2(120, 0))) instill_fear(selected);
            ImGui::EndDisabled();
        }
        ImGui::End();
        ImGui::PopStyleVar();
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
        // Phase 15: stones carved by pinned legends, words + the player's own
        // Rosetta reading — rebuilt once per world tick (inscription_words
        // builds a lexicon index, too heavy for every 15-frame cache pass).
        if (w.clock_.tick() != c_carved_tick) {
            c_carved_tick = w.clock_.tick();
            c_carved.clear();
            const PropId auth_p = SIMLAB_PID(w.props, "inscription_author");
            if (auth_p != kNoProp)
                for (EntityId sid : w.entities_of_kind(SIMLAB_KIND("inscription"))) {
                    const Entity* s = w.find(sid);
                    if (!s || !s->alive) continue;
                    const EntityId author = EntityId(w.prop_int(*s, auth_p, 0));
                    if (author == kNoEntity || !journal.pins.count(author)) continue;
                    auto& vec = c_carved[author];
                    if (vec.size() >= 3) continue;
                    const PropId lang_p = SIMLAB_PID(w.props, "language_id");
                    const std::int64_t lang = lang_p != kNoProp ? w.prop_int(*s, lang_p, 0) : 0;
                    std::string line;
                    for (const std::string& wd : inscription_words(w, sid)) {
                        if (!line.empty()) line += ", ";
                        line += "\"" + wd + "\"";
                        const std::string rd = journal_reading(w, journal, lang, wd);
                        if (!rd.empty()) line += " (" + rd + ")";
                    }
                    if (!line.empty()) vec.push_back(line);
                }
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
                        // clipper: a tongue can hold hundreds of words — only
                        // rasterize the rows actually inside the scroll window
                        ImGuiListClipper clip;
                        clip.Begin(static_cast<int>(words.size()));
                        while (clip.Step()) {
                            auto wit = words.begin();
                            std::advance(wit, clip.DisplayStart);
                            for (int wi = clip.DisplayStart; wi < clip.DisplayEnd; ++wi, ++wit) {
                                const auto& [wd, jw] = *wit;
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
                        }
                        ImGui::EndTable();
                    }
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Chronicle", nullptr, tabflag(1))) {
                // THE chronicle — the single home for the world's story:
                // LLM-written chapters on top (persisted to
                // saves/chronicle_<seed>.md), then the complete live record
                // (every event, template + LLM one-liners), then Firsts.
                ui_chapters();
                ImGui::Separator();
                ImGui::TextDisabled("Live record — every event, newest first.");
                {
                    static const char* kFilters[] = {"All", "Combat", "Survival", "Discovery", "Society", "Pinned"};
                    for (int i = 0; i < 6; ++i) {
                        if (i) ImGui::SameLine();
                        const bool sel = chron_filter == i;
                        if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.40f, 0.20f, 1));
                        if (ImGui::SmallButton(kFilters[i])) chron_filter = i;
                        if (sel) ImGui::PopStyleColor();
                    }
                    ImGui::Separator();
                    std::vector<const NarrativeLine*> rows;
                    rows.reserve(cfeed.lines.size());
                    for (auto it = cfeed.lines.rbegin(); it != cfeed.lines.rend(); ++it) {
                        const NarrativeLine& l = *it;
                        const bool ok = chron_filter == 0 ? true
                            : chron_filter == 1 ? (l.cat & kCatCombat) != 0
                            : chron_filter == 2 ? (l.cat & kCatSurvival) != 0
                            : chron_filter == 3 ? (l.cat & kCatDiscovery) != 0
                            : chron_filter == 4 ? (l.cat & kCatSociety) != 0
                            : (l.cat & kCatPinned) != 0;
                        if (ok) rows.push_back(&l);
                    }
                    if (rows.empty()) ImGui::TextDisabled("The chronicle is quiet — events appear as they happen.");
                    ImGuiListClipper feedclip;
                    feedclip.Begin(static_cast<int>(rows.size()));
                    while (feedclip.Step()) {
                        for (int i = feedclip.DisplayStart; i < feedclip.DisplayEnd; ++i) {
                            const NarrativeLine* l = rows[i];
                            ImGui::PushID(static_cast<int>(l->seq % 100000));
                            char when[24];
                            std::snprintf(when, sizeof when, "d%llu %02dh",
                                          (unsigned long long)(l->tick / 24 + 1), int(l->tick % 24));
                            ImGui::TextDisabled("%s", when);
                            ImGui::SameLine(0, 4);
                            ImGui::PushStyleColor(ImGuiCol_Text,
                                l->llm ? ImVec4(1.f, 0.9f, 0.55f, 1)
                                : (l->cat & kCatCombat) ? ImVec4(1.f, 0.55f, 0.45f, 1)
                                : (l->cat & kCatDiscovery) ? ImVec4(1.f, 0.82f, 0.5f, 1)
                                : (l->cat & kCatSociety) ? ImVec4(0.6f, 0.9f, 0.95f, 1)
                                : (l->cat & kCatSurvival) ? ImVec4(0.65f, 0.95f, 0.6f, 1)
                                : ImVec4(0.85f, 0.85f, 0.88f, 1));
                            if (ImGui::Selectable(l->text.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick,
                                                  ImVec2(0, 0))) {
                                cam.cx = float(l->pos.x); cam.cy = float(l->pos.y);
                                if (globe) { globe = false; globe_dive = false; }
                                if (l->who != kNoEntity) selected = l->who;
                            }
                            ImGui::PopStyleColor();
                            ImGui::PopID();
                        }
                    }
                }
                ImGui::Separator();
                ImGui::TextDisabled("Firsts of this world. Click Go to fly to where it happened.");
                ImGuiListClipper clip;
                clip.Begin(static_cast<int>(journal.milestones.size()));
                while (clip.Step()) {
                    for (int mi = clip.DisplayStart; mi < clip.DisplayEnd; ++mi) {
                        const Milestone& it = journal.milestones[journal.milestones.size() - 1 - mi];
                        ImGui::PushID(it.key.c_str());
                        if (ImGui::SmallButton("Go")) {
                            cam.cx = float(it.pos.x); cam.cy = float(it.pos.y);
                            if (w.find(it.who)) selected = it.who;
                        }
                        ImGui::SameLine();
                        ImGui::Text("day %llu  %s  (%s, tongue %lld)", (unsigned long long)(it.tick / 24), it.title.c_str(),
                                    sid(it.who).c_str(), (long long)it.lang);
                        ImGui::PopID();
                    }
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
                    // Phase 15: their story so far — chronicle lines they starred
                    // in, plus their carved stones read through your Rosetta.
                    {
                        std::vector<std::string> deeds;
                        for (auto it = cfeed.lines.rbegin(); it != cfeed.lines.rend() && deeds.size() < 5; ++it)
                            if (it->who == L.id || it->about == L.id) deeds.push_back(it->text);
                        if (!deeds.empty()) {
                            ImGui::TextDisabled("lately:");
                            for (const std::string& d : deeds) ImGui::BulletText("%s", d.c_str());
                        }
                        if (auto ci = c_carved.find(L.id); ci != c_carved.end())
                            for (const std::string& s : ci->second)
                                ImGui::TextWrapped("carved: %s", s.c_str());
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
            // measure — cap the overhead bubble at a glance: first 4 words
            // plus a "+N" chip; the full utterance lives in Journal/Chronicle.
            std::vector<std::pair<std::string, std::string>> all;
            all.reserve(it->words.size());
            for (const std::string& wd : it->words) {
                std::string tr = journal_reading(w, journal, it->lang, wd);
                if (avatar != kNoEntity) {
                    const auto gi = c_avatar_gloss.find(wd);
                    if (gi != c_avatar_gloss.end()) tr = gi->second + " (you know)";
                }
                all.push_back({wd, tr});
            }
            std::vector<std::pair<std::string, std::string>> parts;
            if (all.size() > 5) {
                parts.assign(all.begin(), all.begin() + 4);
                parts.push_back({"+" + std::to_string(all.size() - 4), "\xC2\xB7"});
            } else {
                parts = std::move(all);
            }
            float wsum = 8;
            for (const auto& [wd, tr] : parts) {
                const float ww = std::max(ImGui::CalcTextSize(wd.c_str()).x, ImGui::CalcTextSize(tr.c_str()).x) + 10;
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
        ImGui::SetNextWindowPos(ImVec2(float(win_w) - 10, float(win_h) - 10), ImGuiCond_Always, ImVec2(1, 1));
        ImGui::Begin("##minimap", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
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
        // ---- DF HUD: lineage generation (parent_of walk, dead ancestors ----
        // still count — lineage is a record, not a survival claim)
        {
            int depth = 0;
            std::vector<EntityId> frontier{e->id};
            std::set<EntityId> seen{e->id};
            for (int d = 0; d < 8 && !frontier.empty(); ++d) {
                std::vector<EntityId> next;
                bool any = false;
                for (EntityId cur : frontier)
                    for (const Relation& r : w.relations_of(cur, "parent_of"))
                        if (r.b == cur && seen.insert(r.a).second) { next.push_back(r.a); any = true; }
                if (!any) break;
                ++depth; frontier.swap(next);
            }
            for (const Relation& r : w.relations_of(e->id, "parent_of")) {
                if (r.b == e->id) ++insp.n_parents;
                else if (r.a == e->id) ++insp.n_children;
            }
            insp.gen = depth == 0 ? "F0 Founder" : depth == 1 ? "F1" : "F2+";
            insp.age = w.prop_int(*e, P("age"), 0);
            insp.adult_age = w.prop_int(*e, P("adult_age"), 0);
            auto need = [&](const char* n, long long mx) {
                insp.needs.emplace_back(n, std::pair<long long,long long>{
                    w.prop_int(*e, P(n), 0), mx});
            };
            need("energy", 120000); need("hunger", 1000);
            need("thirst", 1000); need("integrity", 1000);
        }
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
            // ---- Phase 15: current intent ------------------------------------
            // Controller memory first (pending journey), then neuro overrides,
            // then an open hypothesis, then plain needs — all read under lock.
            {
                std::string intent;
                auto pit = dstate.pending.find(e->id);
                if (pit != dstate.pending.end()) {
                    intent = "journeying to (" + std::to_string(pit->second.target.x) + "," +
                             std::to_string(pit->second.target.y) + ")";
                    if (pit->second.subject != kNoEntity) intent += " after " + sid(pit->second.subject);
                } else {
                    const NeuroState& nr = m->neuro;
                    if (nr.panic > 600) intent = "panicked — thrashing";
                    else if (nr.rage > 600) intent = "enraged";
                    else if (nr.grief > 600) intent = "grieving";
                }
                if (intent.empty()) {
                    for (const auto& [dk, d] : m->discoveries)
                        if (d.status == DiscoveryStatus::Open || d.status == DiscoveryStatus::Tested) {
                            intent = "testing: " + d.claim; break;
                        }
                }
                if (intent.empty()) {
                    auto need = [&](const char* n, long long lim) {
                        const PropertyDef* d = w.props.find_name(n);
                        return d && w.prop_int(*e, d->id, 0) > lim;
                    };
                    if (need("hunger", 750)) intent = "foraging for food";
                    else if (need("thirst", 750)) intent = "seeking water";
                    const PropertyDef* ed = w.props.find_name("energy");
                    if (intent.empty() && ed && w.prop_int(*e, ed->id, 1000) < 150) intent = "resting";
                    if (intent.empty()) intent = "watching";
                }
                insp.intent = intent;
            }
            // ---- Phase 15: life story — deepest engrams + proofs + births -----
            {
                struct EM { std::int64_t score; std::string s; };
                std::vector<EM> mems;
                auto push_mem = [&](const MemoryEngram& g, const std::string& what) {
                    if (g.clarity < 150) return;
                    const std::int64_t ch = g.emotional_charge;
                    std::string s;
                    if (ch <= -400) s = "haunted by " + what;
                    else if (ch >= 400) s = "holds dear " + what;
                    else return;
                    mems.push_back({-std::llabs(ch), s});
                };
                for (const auto& [eid, ms] : m->memory) {
                    std::string what = ms.kind_name;
                    if (ms.material != kNoMaterial)
                        if (const MaterialDef* md = w.materials.find(ms.material)) what += " (" + md->name + ")";
                    push_mem(ms.engram, what);
                }
                for (const BornEngram& b : m->born_memories) {
                    std::string what = b.kind_name.empty() ? "something" : b.kind_name;
                    if (b.material != kNoMaterial)
                        if (const MaterialDef* md = w.materials.find(b.material)) what += " (" + md->name + ")";
                    const std::int64_t ch = b.engram.emotional_charge;
                    if (ch < 0) mems.push_back({-std::llabs(ch), "born fearing " + what});
                }
                for (const auto& [dk, d] : m->discoveries)
                    if (d.status == DiscoveryStatus::Verified)
                        mems.push_back({-5000, "proved: " + d.claim});
                std::sort(mems.begin(), mems.end(), [](const EM& a, const EM& b) { return a.score < b.score; });
                for (std::size_t i = 0; i < mems.size() && i < 10; ++i) insp.life.push_back(mems[i].s);
            }
            // ---- DF HUD: memories with affective charge ----------------------
            {
                struct ME { std::int64_t score; std::string s; };
                std::vector<ME> top;
                for (const auto& [eid, ms] : m->memory) {
                    const MemoryEngram& g = ms.engram;
                    if (g.clarity < 150 || g.emotional_charge == 0) continue;
                    std::string what = ms.kind_name;
                    if (ms.material != kNoMaterial)
                        if (const MaterialDef* md = w.materials.find(ms.material)) what += " (" + md->name + ")";
                    char tag[48];
                    std::snprintf(tag, sizeof tag, "%s%lld",
                                  g.emotional_charge >= 0 ? "+" : "", (long long)g.emotional_charge);
                    top.push_back({-std::llabs(g.emotional_charge) * std::min<std::int64_t>(g.clarity, 400) / 400,
                                   "#" + std::to_string(eid % 100000) + " " + what + "  [" + tag + "]"});
                }
                std::sort(top.begin(), top.end(), [](const ME& a, const ME& b) { return a.score < b.score; });
                for (std::size_t i = 0; i < top.size() && i < 8; ++i) insp.memories.push_back(top[i].s);
            }
            // ---- DF HUD: live brain — module activity, neuromodulators, -----
            // social ties, inherited traits (all read-only)
            {
                const BrainSlot* bs = nullptr;
                if (w.brains_) {
                    auto sit = w.brains_->slots.find(e->id);
                    if (sit != w.brains_->slots.end()) bs = &sit->second;
                }
                if (bs) {
                    static const char* kModNames[kBModCount] = {
                        "Dopamine", "Serotonin", "Norepinephrine", "Acetylcholine",
                        "GABA", "Glutamate", "Cortisol"};
                    for (int i = 0; i < kBModCount; ++i)
                        insp.neuros.emplace_back(kModNames[i], int(bs->vital.mod[i]));
                    static const char* kModShort[kBrainModuleCount] = {
                        "Brainstem", "Thalamus", "Hypothal.", "Cerebellum",
                        "BasalGang.", "Amygdala", "Hippocampus", "Prefrontal",
                        "Sens.Cortex", "MotorCort.", "Association", "Language"};
                    for (int i = 0; i < kBrainModuleCount; ++i) {
                        const BrainModuleState& ms = bs->net.modules[i];
                        int act = 0;
                        for (std::uint32_t j = 0; j < ms.count; ++j) {
                            const std::uint32_t n = ms.first + j;
                            if (n >= bs->net.st.size()) break;
                            if (bs->net.st[n].membrane * 2 > bs->net.gen[n].threshold ||
                                bs->net.st[n].refractory_left > 0) ++act;
                        }
                        insp.brain_act.emplace_back(kModShort[i],
                            ms.count ? int(act * 100 / ms.count) : 0);
                    }
                    // social ties: BrainSocial keys are hash(entity id) —
                    // resolve by hashing every living mind's id once
                    {
                        std::unordered_map<std::uint64_t, EntityId> key2id;
                        for (const auto& [mid, mm] : w.minds_) {
                            const Entity* other = w.find(mid);
                            if (!other || !other->alive) continue;
                            key2id[Rng::hash(static_cast<std::uint64_t>(mid) ^ 0x5DEECE66DULL)] = mid;
                        }
                        for (const BrainSocial& s : bs->cog.social) {
                            if (s.key == 0) continue;
                            auto kit = key2id.find(s.key);
                            if (kit == key2id.end()) continue;
                            const Entity* other = w.find(kit->second);
                            char buf[160];
                            std::snprintf(buf, sizeof buf, "%s %s  trust %+d  fam %d",
                                sid(kit->second).c_str(),
                                other ? other->kind_name.str().c_str() : "?",
                                int(s.trust), int(s.familiarity));
                            insp.ties.push_back(buf);
                        }
                    }
                    const BrainNetParams np = brainnet_params_of(bs->genome);
                    insp.traits.emplace_back("plasticity", std::to_string(np.plastic_rate));
                    insp.traits.emplace_back("mod sensitivity", std::to_string(np.mod_sens_pm));
                    insp.traits.emplace_back("rollout budget", std::to_string(np.rollout_budget_pm));
                    insp.traits.emplace_back("connectome profile", std::to_string(np.profile));
                }
                // inherited life-history traits live as expressed gene props
                for (const char* tn : {"thirst_scale", "juvenile_need_scale",
                                       "birth_energy", "metabolism", "fertility"}) {
                    const PropertyDef* td = w.props.find_name(tn);
                    if (!td) continue;
                    const Value* tv = w.prop_of(*e, td->id);
                    if (tv) insp.traits.emplace_back(tn, value_str(*tv));
                }
            }
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
    // ---- Continuous Chronicle chapters (Biên Niên Sử) -------------------------
    // Long-form LLM chapters in chronological order; click flies the camera
    // to the chapter's event centroid, Export copies the .md path. Shared by
    // the live Chronicle window and the codex "Chronicle" tab.
    void ui_chapters() {
        if (chron_chapters.empty()) return;
        ImGui::TextDisabled("Biên Niên Sử — %zu chapters", chron_chapters.size());
        if (ImGui::SmallButton("Export .md") && !chron_path.empty()) {
            SDL_SetClipboardText(chron_path.c_str());
            status_msg = "Chronicle path copied: " + chron_path;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", chron_path.c_str());
        ImGui::Separator();
        ImGuiListClipper cclip;
        cclip.Begin(static_cast<int>(chron_chapters.size()));
        while (cclip.Step()) {
            for (int ci = cclip.DisplayStart; ci < cclip.DisplayEnd; ++ci) {
                const ChronicleChapter& c = chron_chapters[ci];
                ImGui::PushID(2000000 + ci);
                char label[192];
                std::snprintf(label, sizeof label, "[d%llu–d%llu] %s",
                    (unsigned long long)(c.tick0 / 24 + 1),
                    (unsigned long long)(c.tick1 / 24 + 1), c.title.c_str());
                if (ImGui::Selectable(label, chron_open == ci,
                                      ImGuiSelectableFlags_AllowDoubleClick)) {
                    chron_open = (chron_open == ci) ? -1 : ci;
                    cam_gx = float(c.pos.x); cam_gy = float(c.pos.y);
                    cam_gz = 0.9f; cam_goal = true;
                    if (globe) { globe = false; globe_dive = false; }
                }
                if (chron_open == ci) {
                    ImGui::Indent(12.f);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.82f, 0.84f, 0.9f, 1));
                    // body/facts can be huge for raw record chapters — cap
                    // the on-screen text; the .md file keeps everything.
                    constexpr std::size_t kDisp = 6000;
                    if (c.body.size() <= kDisp) ImGui::TextWrapped("%s", c.body.c_str());
                    else ImGui::TextWrapped("%.*s\n…(truncated — full record in %s)",
                                            int(kDisp), c.body.c_str(), chron_path.c_str());
                    if (!c.facts.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.58f, 0.62f, 1));
                        if (c.facts.size() <= kDisp)
                            ImGui::TextWrapped("Facts: %s", c.facts.c_str());
                        else
                            ImGui::TextWrapped("Facts: %.*s\n…(+%zu chars in %s)",
                                               int(kDisp), c.facts.c_str(),
                                               c.facts.size() - kDisp, chron_path.c_str());
                        ImGui::PopStyleColor();
                    }
                    ImGui::PopStyleColor();
                    ImGui::Unindent(12.f);
                }
                ImGui::PopID();
            }
        }
        ImGui::Separator();
        // ---- discovered materials catalog (Qwen-named syntheses) ------------
        if (!mat_entries.empty()) {
            ImGui::TextDisabled("Vật liệu do NPC khám phá — %zu", mat_entries.size());
            ImGuiListClipper mclip;
            mclip.Begin(static_cast<int>(mat_entries.size()));
            while (mclip.Step()) {
                for (int mi = mclip.DisplayStart; mi < mclip.DisplayEnd; ++mi) {
                    const MatEntry& m = mat_entries[mi];
                    ImGui::PushID(3000000 + mi);
                    if (ImGui::TreeNode(m.name.c_str())) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.62f, 0.68f, 1));
                        ImGui::TextWrapped("%s", m.desc.c_str());
                        ImGui::TextDisabled("slug: %s   id: %s", m.slug.c_str(), m.id.c_str());
                        ImGui::PopStyleColor();
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                }
            }
            ImGui::Separator();
        }
    }


    void ui_inspector() {
        if (!show_inspector || selected == kNoEntity || !insp.found) return;   // appears on selection only
        ImGui::SetNextWindowPos(ImVec2(float(win_w) - 360, 52), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(350, float(std::max(220, win_h - 52 - 250))), ImGuiCond_FirstUseEver);
        bool open = true;
        ImGui::Begin("Inspector", &open);
        if (!open) { selected = kNoEntity; ImGui::End(); return; }
        ImGui::TextUnformatted(insp.head.c_str());
        if (!insp.mat.empty()) ImGui::TextUnformatted(insp.mat.c_str());
        if (!insp.gen.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(insp.gen == "F0 Founder" ? ImVec4(0.55f,0.75f,1.f,1)
                                 : insp.gen == "F1" ? ImVec4(0.75f,0.6f,1.f,1)
                                 : ImVec4(1.f,0.8f,0.4f,1), "  %s", insp.gen.c_str());
        }
        if (ImGui::BeginTabBar("insp")) {
            if (ImGui::BeginTabItem("Status")) {
                // biography header: age/maturity + lineage counts
                if (insp.adult_age > 0) {
                    const float mat = std::clamp(float(insp.age) / float(insp.adult_age), 0.f, 1.f);
                    ImGui::Text("age %lld  (adult at %lld)", insp.age, insp.adult_age);
                    ImGui::ProgressBar(mat, ImVec2(-1, 0), mat >= 1.f ? "adult" : "juvenile");
                } else if (insp.age > 0) {
                    ImGui::Text("age %lld", insp.age);
                }
                if (insp.n_parents || insp.n_children)
                    ImGui::TextDisabled("parents %lld   offspring %lld", insp.n_parents, insp.n_children);
                // needs bars
                for (const auto& [label, nv] : insp.needs) {
                    const float frac = nv.second > 0 ? std::clamp(float(nv.first) / float(nv.second), 0.f, 1.f) : 0.f;
                    ImGui::TextUnformatted(label.c_str()); ImGui::SameLine(90);
                    ImGui::ProgressBar(frac, ImVec2(-1, 0),
                        (std::to_string(nv.first) + "/" + std::to_string(nv.second)).c_str());
                }
                if (!insp.intent.empty())
                    ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.45f, 1), "now: %s", insp.intent.c_str());
                ImGui::Separator();
                if (ImGui::BeginTable("props", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 160))) {
                    ImGuiListClipper clip;
                    clip.Begin(static_cast<int>(insp.props.size()));
                    while (clip.Step())
                        for (int pi = clip.DisplayStart; pi < clip.DisplayEnd; ++pi) {
                            const auto& [k, v] = insp.props[pi];
                            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(k.c_str());
                            ImGui::TableNextColumn(); ImGui::TextUnformatted(v.c_str());
                        }
                    ImGui::EndTable();
                }
                for (const std::string& l : insp.body) ImGui::BulletText("%s", l.c_str());
                ImGui::EndTabItem();
            }
            if (insp.has_mind && ImGui::BeginTabItem("Memories & Ties")) {
                ImGui::TextUnformatted(insp.mind_head.c_str());
                if (!insp.memories.empty()) {
                    ImGui::Separator();
                    ImGui::TextDisabled("engrams:");
                    for (const std::string& mm : insp.memories) ImGui::BulletText("%s", mm.c_str());
                }
                if (!insp.ties.empty()) {
                    ImGui::Separator();
                    ImGui::TextDisabled("social ties:");
                    for (const std::string& t : insp.ties) ImGui::BulletText("%s", t.c_str());
                }
                if (ImGui::BeginTable("bel", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 140))) {
                    ImGuiListClipper clip;
                    clip.Begin(static_cast<int>(insp.beliefs.size()));
                    while (clip.Step())
                        for (int bi = clip.DisplayStart; bi < clip.DisplayEnd; ++bi) {
                            const auto& [k, c] = insp.beliefs[bi];
                            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(k.c_str());
                            ImGui::TableNextColumn(); ImGui::Text("%lld", c);
                        }
                    ImGui::EndTable();
                }
                if (!insp.life.empty()) {
                    ImGui::Separator();
                    ImGui::TextDisabled("life:");
                    for (const std::string& l : insp.life) ImGui::BulletText("%s", l.c_str());
                }
                for (const std::string& d : insp.discoveries) ImGui::BulletText("%s", d.c_str());
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Brain")) {
                if (!insp.brain_act.empty()) {
                    ImGui::TextDisabled("module activity (live):");
                    for (const auto& [name, act] : insp.brain_act) {
                        ImGui::TextUnformatted(name.c_str()); ImGui::SameLine(110);
                        ImGui::ProgressBar(act / 100.f, ImVec2(-1, 0),
                            (std::to_string(act) + "%").c_str());
                    }
                }
                if (!insp.neuros.empty()) {
                    ImGui::Separator();
                    ImGui::TextDisabled("neuromodulators:");
                    for (const auto& [name, v] : insp.neuros) {
                        ImGui::TextUnformatted(name.c_str()); ImGui::SameLine(110);
                        ImGui::ProgressBar(v / 1000.f, ImVec2(-1, 0),
                            std::to_string(v).c_str());
                    }
                }
                if (!insp.traits.empty()) {
                    ImGui::Separator();
                    ImGui::TextDisabled("inherited traits:");
                    for (const auto& [k, v] : insp.traits)
                        ImGui::BulletText("%s = %s", k.c_str(), v.c_str());
                }
                if (insp.brain_act.empty() && insp.neuros.empty() && insp.traits.empty())
                    ImGui::TextDisabled("no connectome allocated for this entity");
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
        c_codex_lines.clear();
        for (std::size_t i = 0; i <= c_codex.size();) {
            const std::size_t j = c_codex.find('\n', i);
            c_codex_lines.push_back(c_codex.substr(i, j == std::string::npos ? j : j - i));
            if (j == std::string::npos) break;
            i = j + 1;
        }
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
            // pre-wrap to ~100-char rows so the list clips uniformly
            const std::string line = "t" + std::to_string(ev.tick) + " " + ev.action +
                                     (ev.failed ? " (failed)" : "") + "  " + ev.evidence.substr(0, 160);
            std::size_t start = 0;
            while (start < line.size()) {
                std::size_t end = std::min(start + 100, line.size());
                if (end < line.size()) {
                    const std::size_t sp = line.rfind(' ', end);
                    if (sp > start + 40) end = sp;
                }
                c_chron.push_back(line.substr(start, end - start));
                start = end;
                while (start < line.size() && line[start] == ' ') ++start;
            }
        }
        if (std::getenv("SIMLAB_VDEBUG"))
            std::fprintf(stderr, "codex: stones=%zu chron_rows=%zu codex_lines=%zu\n",
                         c_stones.size(), c_chron.size(), c_codex_lines.size());
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
                // clipped line list — InputTextMultiline would lay out the
                // whole codex every rasterize (~tens of ms on big lexicons)
                ImGuiListClipper clip;
                clip.Begin(static_cast<int>(c_codex_lines.size()));
                while (clip.Step())
                    for (int li = clip.DisplayStart; li < clip.DisplayEnd; ++li)
                        ImGui::TextUnformatted(c_codex_lines[li].c_str());
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Codex chronicle")) {
                // The Biên Niên Sử lives ONLY in the Field Journal "Chronicle"
                // tab now — this tab keeps the exported codex text lines.
                ImGuiListClipper clip;
                clip.Begin(static_cast<int>(c_chron.size()));
                while (clip.Step())
                    for (int li = clip.DisplayStart; li < clip.DisplayEnd; ++li)
                        ImGui::TextUnformatted(c_chron[li].c_str());
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
                if (!in_menu) { if (has_game) open_menu(kPause); }
                else if (screen == kPause) close_menu();
                else if (screen != kTitle) screen = has_game ? kPause : kTitle;
                return;
            }
            if (k == SDLK_F5) { save_slot(1); return; }
            if (k == SDLK_J) { show_journal = !show_journal; return; }
            if (k == SDLK_B) { show_badges = !show_badges; return; }
            if (k == SDLK_M && is_planet()) { if (globe) enter_surface(gview.lon, gview.lat); else enter_globe(); return; }
            if (avatar != kNoEntity && k >= SDLK_1 && k <= SDLK_9) {   // avatar verbs
                int n = 0;
                for (const std::string& v : c_av_verbs) {
                    if (v == "move") continue;
                    if (++n == int(k - SDLK_1) + 1) { avatar_act(v); break; }
                }
                return;
            }
            if (k == SDLK_SPACE) speed = speed ? 0 : std::max(1, last_speed);
            if (k == SDLK_EQUALS || k == SDLK_KP_PLUS) last_speed = speed = std::min(4, speed + 1);
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

    // ---- CL-1 Phase 3: LLM chronicle scan/drain (UI thread, world lock held)
    // Salient causal events are summarised and handed to NarrativeHost; the
    // worker narrates them off-thread and llm_drain() re-injects the text as
    // tagged NarrativeLine{llm=true} rows in the same ChronicleFeed.
    static bool llm_skip_verb(const std::string& a) {
        return a.empty() || a == "metabolize" || a == "cool" || a == "ecology" ||
               a == "political_drift" || a == "myth_drift" || a == "language" ||
               a == "anatomy_drift" || a == "decay_minds" || a == "move" ||
               a == "rest" ||
               a == "perceive" || a == "witness" || a == "advance_ticks" ||
               a == "flush_needs";
    }
    // ---- Continuous Chronicle: chapter batching ------------------------------
    // System prompt tuned for a small instruct model: strict truthfulness to
    // the supplied facts, epic continuity with the previous passage, and a
    // rigid "title line + prose" output format.
    static const char* kChapterSys() {
        return "You are the Historian of Sunnyworld — a silent observer "
               "recording the evolution, survival and extinction of its "
               "creatures.\n"
               "ABSOLUTE RULES:\n"
               "1. TRUTHFUL TO THE RECORD: narrate ONLY from the facts "
               "provided (names, species, deaths, births, disasters). NEVER "
               "invent characters, magic, or events absent from the data.\n"
               "2. EPIC CONTINUITY: continue naturally from the Previous "
               "passage so the chronicle reads as one unbroken history. Use "
               "solemn, vivid prose.\n"
               "3. REQUIRED FORMAT:\n"
               "Line 1: a short chapter title (at most 8 words, no quotes).\n"
               "Following lines: one chronicle paragraph of 3 to 5 sentences "
               "(no bullet lists, no meta commentary).";
    }
    static std::string chron_trim(const std::string& s) {
        std::size_t a = s.find_first_not_of(" \t\r\n");
        std::size_t b = s.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    }
    // Last ~3 sentences of a chapter body, for prompt continuity.
    static std::string chron_tail_of(const std::string& body) {
        if (body.size() <= 340) return body;
        std::string t = body.substr(body.size() - 340);
        const std::size_t dot = t.find(". ");
        if (dot != std::string::npos) t = t.substr(dot + 2);
        return chron_trim(t);
    }
    void chron_load(const World& w) {
        chron_facts.clear(); chron_chapters.clear();
        llm_ch_pending.clear(); llm_ch_outstanding.clear();
        chron_tail.clear(); chron_catastrophic = false; chron_open = -1;
        chron_overflow = 0;
        chron_kinds.clear();
        for (const auto& ep : w.entities)
            if (ep->alive) chron_kinds.insert(std::string(ep->kind_name));
        chron_win_start = w.clock_.tick();
        chron_path = "saves/chronicle_" + std::to_string(w.world_seed) + ".md";
        std::ifstream f(chron_path);
        if (!f) return;
        // Parse "### [t0 – t1] title" sections; the trailing
        // "*(Dữ kiện gốc: ...)" line is stored separately as raw facts.
        std::string line, title, body, facts;
        std::uint64_t t0 = 0, t1 = 0;
        bool have = false;
        auto flush_sec = [&]() {
            if (!have) return;
            ChronicleChapter c;
            c.tick0 = t0; c.tick1 = t1; c.title = chron_trim(title);
            std::string b = chron_trim(body);
            const std::string mark = "*(Dữ kiện gốc:";
            const std::size_t fp = b.rfind(mark);
            if (fp != std::string::npos && b.find('\n', fp) == std::string::npos) {
                c.facts = chron_trim(b.substr(fp + mark.size(),
                    b.size() - fp - mark.size() - (b.back() == ')' ? 2 : 1)));
                b = chron_trim(b.substr(0, fp));
            }
            c.body = b;
            if (!c.body.empty()) chron_chapters.push_back(std::move(c));
        };
        while (std::getline(f, line)) {
            if (line.rfind("### [", 0) == 0) {
                flush_sec();
                have = true; body.clear(); facts.clear(); title.clear();
                const std::size_t rb = line.find(']');
                const std::size_t sep = line.find('\xE2');   // en-dash in "t0 – t1"
                std::uint64_t a = 0, b2 = 0;
                if (sep != std::string::npos) {
                    a = std::strtoull(line.c_str() + 5, nullptr, 10);
                    b2 = std::strtoull(line.c_str() + sep + 3, nullptr, 10);
                } else {
                    const std::size_t dash = line.find('-');
                    if (dash != std::string::npos) {
                        a = std::strtoull(line.c_str() + 5, nullptr, 10);
                        b2 = std::strtoull(line.c_str() + dash + 1, nullptr, 10);
                    }
                }
                t0 = a; t1 = b2;
                if (rb != std::string::npos) title = line.substr(rb + 1);
            } else if (have) {
                body += line; body += '\n';
            }
        }
        flush_sec();
        if (chron_chapters.size() > 500) {          // memory cap; file keeps all
            chron_chapters.erase(chron_chapters.begin(),
                chron_chapters.end() - 500);
        }
        if (!chron_chapters.empty())
            chron_tail = chron_tail_of(chron_chapters.back().body);
    }
    void chron_append(const ChronicleChapter& c) {
        std::error_code ec;
        std::filesystem::create_directories("saves", ec);
        std::ofstream f(chron_path, std::ios::app);
        if (!f) return;
        f << "### [" << c.tick0 << " \xE2\x80\x93 " << c.tick1 << "] "
          << c.title << "\n" << c.body << "\n";
        if (!c.facts.empty()) f << "*(Dữ kiện gốc: " << c.facts << ")*\n";
        f << "\n";
    }
    // Window check + catastrophic flush; called under the world lock.
    // EVERY action event is a fact now — the chapter prompt gets up to
    // ~4800 chars of them and the .md keeps a single-line record of the
    // same, so the chronicle is a complete account, not just milestones.
    void chron_tick(const World& w) {
        const std::uint64_t now = w.clock_.tick();
        if (chron_facts.empty()) {
            if (!chron_catastrophic) chron_win_start = now;
            return;
        }
        const bool due = now - chron_win_start >= 300 && chron_facts.size() >= 2;
        const bool full = chron_facts.size() >= kChronFactCap;
        if (!due && !chron_catastrophic && !full) return;
        if (llm_ch_outstanding.size() >= 8) return;   // backpressure: keep accumulating
        chron_catastrophic = false;
        // ---- compose the chapter prompt -------------------------------------
        std::uint64_t alive = 0, minds = 0;
        std::set<std::string> species;
        for (const auto& ep : w.entities) {
            if (!ep->alive) continue;
            ++alive;
            species.insert(std::string(ep->kind_name));
        }
        minds = w.minds_.size();
        std::string facts;    // "- ..." bullet block for the LLM prompt (≤4800)
        std::string record;   // single-line persistent fact record (.md) — COMPLETE
        std::string raw;      // plain lines — body of a model-less chapter — COMPLETE
        std::int64_t sx = 0, sy = 0;
        std::size_t used = 0, np = 0;
        const std::size_t n = chron_facts.size();
        for (std::size_t i = 0; i < n; ++i) {
            std::string t = chron_facts[i].text;
            if (t.size() > 150) t = t.substr(0, 147) + "...";
            for (char& ch : t) if (ch == '\n' || ch == '\r') ch = ' ';   // record stays one line
            const std::string line = "- " + t + "\n";
            if (np == 0 || used + line.size() <= 4800) {
                facts += line; used += line.size(); ++np;
            }
            raw += t; raw += '\n';
            if (!record.empty()) record += "; ";
            record += t;
            sx += chron_facts[i].pos.x; sy += chron_facts[i].pos.y;
        }
        // The prompt only carries the first ~4800 chars of facts; tell the
        // model (and the reader) how much of the chapter it did not see.
        // record/raw above are NOT truncated — the .md keeps every event.
        const std::uint64_t more = (n - np) + chron_overflow;
        if (more)
            facts += "- ...and " + std::to_string(more) +
                     " further events this chapter.\n";
        const std::uint64_t t0 = chron_facts.front().tick;
        const std::uint64_t t1 = chron_facts.back().tick;
        const Vec2 cen = n ? Vec2{std::int32_t(sx / std::int64_t(n)),
                                 std::int32_t(sy / std::int64_t(n))} : Vec2{};
        auto commit = [&](ChronicleChapter& c) {
            chron_append(c);
            chron_tail = chron_tail_of(c.body);
            chron_chapters.push_back(std::move(c));
            if (chron_chapters.size() > 500) {       // memory cap; file keeps all
                chron_chapters.erase(chron_chapters.begin(),
                    chron_chapters.begin() + (chron_chapters.size() - 500));
            }
        };
        if (!llm_host.active()) {
            // No model: still persist the raw record so the chronicle
            // captures EVERYTHING even without LLM narration.
            ChronicleChapter c;
            c.tick0 = t0; c.tick1 = t1; c.pos = cen; c.facts = record;
            c.title = "Records day " + std::to_string(t0 / 24 + 1) + " - day " +
                      std::to_string(t1 / 24 + 1);
            c.body = chron_trim(raw);
            if (c.body.empty()) c.body = "(no narrated events)";
            commit(c);
        } else {
            std::string user = std::string("[Previous passage]:\n") +
                (chron_tail.empty() ? std::string("(The chronicle begins here.)")
                                    : chron_tail) + "\n\n";
            user += "[World context — cycles " + std::to_string(t0) + " -> " +
                    std::to_string(t1) + "]:\n";
            user += "Survivors: " + std::to_string(alive) + " | Species: " +
                    std::to_string(species.size()) + " | Minds: " +
                    std::to_string(minds) + "\n\n";
            user += "[Events of this chapter]:\n" + facts + "\n";
            user += "Write the next chronicle chapter:\n";
            ChapterStub stub;
            stub.tick0 = t0; stub.tick1 = t1;
            stub.pos = cen;
            stub.facts = record;
            const std::uint64_t key = ++llm_key_seq;
            llm_ch_pending.emplace(key, std::move(stub));
            llm_ch_outstanding.push_back(key);
            llm_host.submit_prompt(key, kChapterSys(), user);
        }
        chron_facts.clear();
        chron_overflow = 0;
        chron_win_start = now;
    }
    void llm_ch_drain() {
        if (llm_ch_outstanding.empty()) return;
        std::deque<std::uint64_t> keep;
        for (std::uint64_t key : llm_ch_outstanding) {
            std::string text;
            if (!llm_host.poll(key, text)) { keep.push_back(key); continue; }
            auto it = llm_ch_pending.find(key);
            if (it == llm_ch_pending.end()) continue;    // stale (world reset)
            // Format: line 1 = title, rest = prose.
            ChronicleChapter c;
            const ChapterStub stub = it->second;
            c.tick0 = stub.tick0; c.tick1 = stub.tick1;
            c.pos = stub.pos; c.facts = stub.facts;
            const std::size_t nl = text.find('\n');
            if (nl == std::string::npos) {
                c.body = chron_trim(text);
                c.title = "Chronicle " + std::to_string(c.tick0) +
                          "-" + std::to_string(c.tick1);
            } else {
                c.title = chron_trim(text.substr(0, nl));
                c.body = chron_trim(text.substr(nl + 1));
                // strip a leading "Title:"/"#"/markdown echo if the model added one
                if (c.title.rfind("Title:", 0) == 0 ||
                    c.title.rfind("title:", 0) == 0)
                    c.title = chron_trim(c.title.substr(6));
                while (!c.title.empty() &&
                       (c.title.front() == '#' || c.title.front() == '*' ||
                        c.title.front() == '"' || c.title.front() == '\''))
                    c.title.erase(c.title.begin());
                while (!c.title.empty() &&
                       (c.title.back() == '*' || c.title.back() == '"' ||
                        c.title.back() == '\''))
                    c.title.pop_back();
                c.title = chron_trim(c.title);
                if (c.title.size() > 80) c.title = c.title.substr(0, 77) + "...";
                if (c.title.empty()) c.title = "Chapter " +
                    std::to_string(c.tick0) + "-" + std::to_string(c.tick1);
            }
            if (c.body.empty()) { llm_ch_pending.erase(it); continue; }
            chron_append(c);
            chron_tail = chron_tail_of(c.body);
            chron_chapters.push_back(std::move(c));
            if (chron_chapters.size() > 500)
                chron_chapters.erase(chron_chapters.begin(),
                                      chron_chapters.begin() +
                                      (chron_chapters.size() - 500));
            llm_ch_pending.erase(it);
        }
        llm_ch_outstanding.swap(keep);
    }

    // ================= open-ended invention: material cataloger ================
    static std::string mat_clean(std::string s, std::size_t cap) {
        std::string out;
        out.reserve(s.size());
        for (char ch : s) {
            const unsigned char c = static_cast<unsigned char>(ch);
            if (c < 32 || c == 127) {
                if (!out.empty() && out.back() != ' ') out += ' ';
                continue;
            }
            out += ch;
        }
        while (!out.empty() && out.back() == ' ') out.pop_back();
        if (out.size() > cap) out = out.substr(0, cap);
        return out;
    }
    static std::string mat_slug_of(const std::string& name) {
        std::string s;
        for (char ch : name) {
            const unsigned char c = static_cast<unsigned char>(ch);
            if (std::isalnum(c)) s += static_cast<char>(std::tolower(c));
            else if (!s.empty() && s.back() != '_') s += '_';
        }
        while (!s.empty() && s.back() == '_') s.pop_back();
        if (s.empty()) s = "material";
        return s;
    }
    // Parse saves/discovered_materials.txt back into mat_entries (idempotent
    // reload — the file is the catalog's source of truth across sessions).
    void mat_load() {
        mat_entries.clear();
        std::ifstream f(mat_path);
        if (!f) return;
        MatEntry cur; bool in = false;
        std::string line;
        auto flush = [&]() {
            if (in && !cur.name.empty()) mat_entries.push_back(cur);
            cur = MatEntry{}; in = false;
        };
        while (std::getline(f, line)) {
            if (line == "[DISCOVERY]") { flush(); in = true; continue; }
            if (!in) continue;
            auto val = [&](const char* k) -> std::string {
                const std::string p = std::string(k) + "=";
                if (line.rfind(p, 0) != 0) return "";
                return line.substr(p.size());
            };
            if (std::string v = val("id"); !v.empty()) cur.id = v;
            else if (std::string v = val("slug"); !v.empty()) cur.slug = v;
            else if (std::string v = val("name"); !v.empty()) cur.name = v;
            else if (std::string v = val("desc"); !v.empty()) cur.desc = v;
        }
        flush();
    }
    bool mat_known(std::uint64_t synth_id) const {
        char buf[24];
        std::snprintf(buf, sizeof buf, "0x%016llx",
                      static_cast<unsigned long long>(synth_id));
        for (const MatEntry& e : mat_entries)
            if (e.id == buf) return true;
        return llm_mat_pending.count(synth_id) != 0;
    }
    void mat_scan(const World& w) {
        // cursor restart on world reset/load (synthesized_ is per-world)
        if (mat_scanned > w.synthesized_.size()) mat_scanned = 0;
        if (w.synthesized_.size() <= mat_scanned) {
            mat_scanned = w.synthesized_.size();
            return;
        }
        if (!llm_host.active()) return;   // backlog waits for the model (cheap: few synths)
        // SIMLAB_PID caches one slot per call-site -> literals only, unrolled.
        const PropId p_attr[7] = {
            SIMLAB_PID(w.props, "hardness"),
            SIMLAB_PID(w.props, "flammability"),
            SIMLAB_PID(w.props, "nutrition"),
            SIMLAB_PID(w.props, "toxicity"),
            SIMLAB_PID(w.props, "conductivity"),
            SIMLAB_PID(w.props, "density"),
            SIMLAB_PID(w.props, "edge_sharpness")};
        for (; mat_scanned < w.synthesized_.size(); ++mat_scanned) {
            const World::SynthRecord& r = w.synthesized_[mat_scanned];
            if (mat_known(r.synth_id)) continue;
            MatPending mp;
            mp.synth_id = r.synth_id; mp.tick = r.tick; mp.verb = r.verb;
            auto mname = [&](MaterialId mid) -> std::string {
                if (mid == kNoMaterial) return "";
                const MaterialDef* md = w.materials.find(mid);
                return md ? md->name : "";
            };
            mp.pa = mname(r.parent_a); mp.pb = mname(r.parent_b);
            const Entity* ae = r.actor != kNoEntity ? w.find(r.actor) : nullptr;
            mp.actor = ae ? NarrativeComposer::describe(w, r.actor)
                          : std::string("a sapient");
            mp.pos = ae ? ae->pos : Vec2{};
            if (const MaterialDef* md = w.materials.find(r.material)) {
                for (int i = 0; i < 7; ++i) {
                    if (p_attr[i] == kNoProp) continue;
                    const auto it = md->props.find(p_attr[i]);
                    if (it != md->props.end() &&
                        it->second.type == ValType::Integer)
                        mp.attrs[i] = it->second.i;
                }
            }
            // ---- prompt: strict two-line contract -------------------------
            std::string user =
                "A new material was invented in a living world.\n\n"
                "Inventor: " + mp.actor + "\n"
                "Method: " + mp.verb + " of " +
                (mp.pa.empty() ? std::string("raw matter") : mp.pa) +
                (mp.pb.empty() ? "" : " + " + mp.pb) + "\n"
                "Measured attributes: hardness=" +
                std::to_string(mp.attrs[0]) +
                " flammability=" + std::to_string(mp.attrs[1]) +
                " nutrition=" + std::to_string(mp.attrs[2]) +
                " toxicity=" + std::to_string(mp.attrs[3]) +
                " conductivity=" + std::to_string(mp.attrs[4]) +
                " density=" + std::to_string(mp.attrs[5]) +
                " sharpness=" + std::to_string(mp.attrs[6]) + "\n\n"
                "Respond with EXACTLY two lines:\n"
                "NAME=<a 2-4 word English name for this material>\n"
                "DESC=<one sentence describing what it is like>\n";
            const std::string sys =
                "You are a Natural Philosopher cataloging materials "
                "discovered in a living world. Output only the two "
                "requested lines, nothing else.";
            llm_mat_pending.emplace(r.synth_id, mp);
            llm_mat_outstanding.push_back(r.synth_id);
            llm_host.submit_prompt(r.synth_id, sys, user);
        }
    }
    void llm_mat_drain() {
        if (llm_mat_outstanding.empty()) return;
        std::deque<std::uint64_t> keep;
        for (std::uint64_t key : llm_mat_outstanding) {
            std::string text;
            if (!llm_host.poll(key, text)) { keep.push_back(key); continue; }
            auto it = llm_mat_pending.find(key);
            if (it == llm_mat_pending.end()) continue;    // stale
            MatPending mp = std::move(it->second);
            llm_mat_pending.erase(it);
            // ---- parse NAME=/DESC= (tolerate "Name:" / bare first line) -----
            std::string name, desc;
            std::istringstream ls(text);
            std::string line;
            while (std::getline(ls, line)) {
                std::string t = mat_clean(line, 400);
                if (t.empty()) continue;
                std::string up = t;
                for (char& ch : up) ch = static_cast<char>(std::toupper(
                    static_cast<unsigned char>(ch)));
                auto strip_key = [&](const char* k) -> bool {
                    std::string p = k; p += "=";
                    if (t.rfind(p, 0) == 0) { t = t.substr(p.size()); return true; }
                    p = k; p += ":";
                    if (t.rfind(p, 0) == 0) { t = t.substr(p.size()); return true; }
                    return false;
                };
                if (up.rfind("NAME", 0) == 0 && strip_key("NAME")) {
                    name = mat_clean(t, 80); continue;
                }
                if (up.rfind("DESC", 0) == 0 && strip_key("DESC")) {
                    if (!desc.empty()) desc += " ";
                    desc += mat_clean(t, 300); continue;
                }
                if (name.empty()) name = mat_clean(t, 80);
                else { if (!desc.empty()) desc += " "; desc += mat_clean(t, 300); }
            }
            name = mat_clean(name, 80); desc = mat_clean(desc, 300);
            if (name.empty()) continue;                      // unparsable -> drop
            // ---- catalog entry + .txt block ----------------------------------
            MatEntry e;
            char buf[24];
            std::snprintf(buf, sizeof buf, "0x%016llx",
                          static_cast<unsigned long long>(mp.synth_id));
            e.id = buf;
            e.slug = mat_slug_of(name);
            // slug collision -> numeric suffix keeps the file parseable
            bool dup = false;
            for (const MatEntry& x : mat_entries)
                if (x.slug == e.slug) { dup = true; break; }
            if (dup) e.slug += "_" + std::to_string(mat_entries.size() + 1);
            e.name = name; e.desc = desc;
            mat_entries.push_back(e);
            {
                std::error_code ec;
                std::filesystem::create_directories("saves", ec);
                std::ofstream f(mat_path, std::ios::app);
                if (f) {
                    f << "[DISCOVERY]\n";
                    f << "id=" << e.id << "\n";
                    f << "slug=" << e.slug << "\n";
                    f << "name=" << e.name << "\n";
                    f << "parents=" << mp.pa << "+" << mp.pb << "\n";
                    f << "verb=" << mp.verb << "\n";
                    f << "hardness=" << mp.attrs[0] << "\n";
                    f << "flammability=" << mp.attrs[1] << "\n";
                    f << "nutrition=" << mp.attrs[2] << "\n";
                    f << "toxicity=" << mp.attrs[3] << "\n";
                    f << "conductivity=" << mp.attrs[4] << "\n";
                    f << "density=" << mp.attrs[5] << "\n";
                    f << "sharpness=" << mp.attrs[6] << "\n";
                    f << "desc=" << e.desc << "\n\n";
                }
            }
            // surface the naming in the next chronicle window
            if (chron_facts.size() < kChronFactCap)
                chron_facts.push_back({mp.tick,
                    mp.actor + " named the invented material '" + e.name + "'",
                    mp.pos});
        }
        llm_mat_outstanding.swap(keep);
    }

    // ---- tribal mythology: scan + drain --------------------------------------
    // Dedup key mirrors doctrine_key() (Cognition.cpp, anonymous namespace):
    // "kind:prop:thr:id:text:verb" — rebuilt field-by-field here.
    static std::string myth_dkey(const AgentMind::DoctrineSlot& d) {
        std::ostringstream o;
        o << static_cast<int>(d.trig_kind) << ':' << d.trig_prop << ':'
          << d.trig_thr << ':' << d.trig_id << ':' << d.trig_text << ':'
          << d.verb;
        return o.str();
    }
    static std::string myth_focus_text(const World& w,
                                       const AgentMind::DoctrineSlot& d) {
        switch (d.trig_kind) {
        case 0: {
            std::string pn = "a property";
            if (d.trig_prop != kNoProp)
                if (const PropertyDef* pd = w.props.find(d.trig_prop))
                    pn = pd->name;
            return pn + " >= " + std::to_string(d.trig_thr);
        }
        case 1: {
            const MaterialDef* md =
                w.materials.find(static_cast<MaterialId>(d.trig_id));
            return "the material '" +
                (md ? md->name : std::string("?")) + "'";
        }
        case 2:
            return "synthesis recipe #" + std::to_string(d.trig_id);
        case 3:
            return "the " + d.trig_text + " kind";
        }
        return "an unknown focus";
    }
    void myth_load() {
        // repopulate myth_seen from the file so a restarted viewer does not
        // re-ask Qwen for myths it already scripted (id= + tribe= fields).
        myth_seen.clear();
        std::ifstream f(myth_path);
        if (!f) return;
        std::string line, id, tribe;
        auto flush = [&]() {
            if (!id.empty())
                myth_seen.insert("d:" + tribe + ":" + id);
            id.clear(); tribe.clear();
        };
        while (std::getline(f, line)) {
            if (line == "[MYTH]") { flush(); continue; }
            if (line.rfind("id=doctrine:", 0) == 0)
                id = line.substr(12);
            else if (line.rfind("tribe=", 0) == 0)
                tribe = line.substr(6);
            else if (line.rfind("id=a", 0) == 0)
                myth_seen.insert("a:" + line.substr(3));
        }
        flush();
    }
    void myth_scan(const World& w) {
        if (!llm_host.active()) return;
        const PropId p_lang = SIMLAB_PID(w.props, "language_id");
        const PropId p_art = SIMLAB_PID(w.props, "artifact_engrams");
        const auto submit = [&](std::uint64_t key, MythPending&& mp,
                                const std::string& user) {
            llm_myth_pending.emplace(key, std::move(mp));
            llm_myth_outstanding.push_back(key);
            llm_host.submit_prompt(key,
                "You are the Myth-Keeper of Sunnyworld. Output only the two "
                "requested lines, nothing else.", user);
        };
        // ---- crystallized oral traditions: depth >= 3 retellings ----------
        for (const auto& [mid, m] : w.minds_) {
            const Entity* me = w.find(mid);
            if (!me || !me->alive) continue;
            const std::int64_t lang = p_lang != kNoProp
                ? w.prop_int(*me, p_lang, 0) : 0;
            for (const AgentMind::DoctrineSlot& d : m.doctrines) {
                if (d.depth < 3) continue;
                const std::string dkey = myth_dkey(d);
                if (!myth_seen.insert("d:" + std::to_string(lang) + ":" +
                                      dkey).second) continue;
                MythPending mp;
                mp.doc = d; mp.lang = lang; mp.tick = d.last_tick;
                mp.pos = me->pos;
                const std::string conlang =
                    "speech-community " + std::to_string(lang);
                const std::string event =
                    d.valence < -500 ? "a catastrophe" :
                    d.valence > 500 ? "a salvation" : "an event";
                const std::string focus = myth_focus_text(w, d);
                const std::string vword = d.valence < 0
                    ? "taboo" : "reverence";
                std::ostringstream u;
                u << "A tribe speaking " << conlang << " has passed down an "
                  << "oral tradition for " << d.depth
                  << " generations following " << event << " tied to "
                  << focus << ", resulting in a cultural " << vword
                  << " toward " << focus << ".\n\n"
                  << "Write a short mythic title (max 4 words) and a "
                  << "1-sentence sacred proverb or taboo law.\n"
                  << "MYTH_TITLE=\nSCRIPTURE=";
                const std::uint64_t key = Rng::hash(
                    w.world_seed ^ (mid * 0x9E3779B97F4A7C15ull) ^
                    (static_cast<std::uint64_t>(d.depth) << 8) ^
                    std::hash<std::string>{}(dkey));
                submit(key, std::move(mp), u.str());
            }
        }
        // ---- InscribedArtifact translations --------------------------------
        for (EntityId iid : w.entities_of_kind(SIMLAB_KIND("inscription"))) {
            const Entity* ie = w.find(iid);
            if (!ie || !ie->alive || !myth_seen.insert(
                    "a:" + std::to_string(iid)).second) continue;
            if (p_art == kNoProp) continue;
            const Value* av = w.prop_of(*ie, p_art);
            if (!av || av->type != ValType::Text || av->s.empty()) continue;
            MythPending mp;
            mp.artifact = true; mp.insc = iid; mp.tick = w.clock_.tick();
            mp.pos = ie->pos;
            if (p_lang != kNoProp) {
                const Value* lv = w.prop_of(*ie, p_lang);
                if (lv && lv->type == ValType::Integer) mp.lang = lv->i;
            }
            // synthetic focus slot so myth_record_line still emits [MYTH]
            mp.doc.trig_kind = 3;
            mp.doc.trig_text = "artifact";
            mp.doc.trig_id = static_cast<std::uint64_t>(iid);
            mp.doc.last_tick = w.clock_.tick();
            const std::string conlang =
                "speech-community " + std::to_string(mp.lang);
            std::size_t n_eng = 0;
            {
                std::istringstream ls(av->s); std::string ln;
                while (std::getline(ls, ln))
                    if (ln.rfind("E|", 0) == 0) ++n_eng;
            }
            std::ostringstream u;
            u << "An ancient inscription carved by a " << conlang
              << " speaker holds " << n_eng
              << " engraved neural memories — distilled experience of a "
              << "dead or living tribe. Translate its sacred meaning.\n\n"
              << "Write a short mythic title (max 4 words) and a "
              << "1-sentence sacred proverb or taboo law.\n"
              << "MYTH_TITLE=\nSCRIPTURE=";
            const std::uint64_t key = Rng::hash(
                w.world_seed ^ (iid * 0xD1B54A32D192ED03ull) ^
                0x4D595448ull);
            submit(key, std::move(mp), u.str());
        }
    }
    void llm_myth_drain() {
        if (llm_myth_outstanding.empty()) return;
        std::deque<std::uint64_t> keep;
        for (std::uint64_t key : llm_myth_outstanding) {
            std::string text;
            if (!llm_host.poll(key, text)) { keep.push_back(key); continue; }
            auto it = llm_myth_pending.find(key);
            if (it == llm_myth_pending.end()) continue;
            MythPending mp = std::move(it->second);
            llm_myth_pending.erase(it);
            // parse MYTH_TITLE= / SCRIPTURE= (tolerant, like mat_clean)
            std::string title, scripture;
            std::istringstream ls(text);
            std::string line;
            while (std::getline(ls, line)) {
                std::string t = mat_clean(line, 400);
                if (t.empty()) continue;
                std::string up = t;
                for (char& ch : up) ch = static_cast<char>(std::toupper(
                    static_cast<unsigned char>(ch)));
                auto strip_key = [&](const char* k) -> bool {
                    std::string p = k; p += "=";
                    if (t.rfind(p, 0) == 0) { t = t.substr(p.size()); return true; }
                    p = k; p += ":";
                    if (t.rfind(p, 0) == 0) { t = t.substr(p.size()); return true; }
                    return false;
                };
                if (up.rfind("MYTH_TITLE", 0) == 0 && strip_key("MYTH_TITLE")) {
                    title = mat_clean(t, 80); continue;
                }
                if (up.rfind("SCRIPTURE", 0) == 0 && strip_key("SCRIPTURE")) {
                    if (!scripture.empty()) scripture += " ";
                    scripture += mat_clean(t, 300); continue;
                }
                if (title.empty()) title = mat_clean(t, 80);
                else { if (!scripture.empty()) scripture += " ";
                       scripture += mat_clean(t, 300); }
            }
            title = mat_clean(title, 80);
            scripture = mat_clean(scripture, 300);
            if (title.empty()) continue;
            const std::string rec =
                myth_record_line(mp.doc, mp.lang, title, scripture);
            {
                std::error_code ec;
                std::filesystem::create_directories("saves", ec);
                std::ofstream f(myth_path, std::ios::app);
                if (f) f << rec << "\n";
            }
            if (chron_facts.size() < kChronFactCap)
                chron_facts.push_back({mp.tick,
                    "the tribe keeps the myth '" + title + "'",
                    mp.pos});
        }
        llm_myth_outstanding.swap(keep);
    }

    void llm_scan(const World& w) {
        // Chapter facts are collected whether or not the model is loaded —
        // chron_tick() persists them as raw "Records" chapters, so the
        // chronicle still records EVERYTHING without an LLM. Only the
        // expensive one-liner submissions below require an active host.
        // While the model is still loading, fast-forward the cursor ONCE so
        // the first scan doesn't replay (and drop) the whole backlog.
        if (!llm_host.active() && !llm_primed) {
            if (!w.ledger.history().empty()) llm_scanned = w.ledger.next_id() - 1;
            llm_primed = true;
            return;
        }
        const auto& hist = w.ledger.history();
        if (hist.empty()) return;
        const std::uint64_t base = w.ledger.next_id() - hist.size();
        std::size_t i0 = hist.size();
        if (!llm_primed) { llm_primed = true; llm_scanned = w.ledger.next_id(); }
        else if (llm_scanned + 1 >= base)
            i0 = std::min<std::size_t>(std::size_t(llm_scanned + 1 - base), hist.size());
        else llm_scanned = w.ledger.next_id() - 1;   // cursor evicted: skip backlog

        std::size_t made = 0;
        const PropId pain_p = SIMLAB_PID(w.props, "pain_level");
        const PropId integ_p = SIMLAB_PID(w.props, "integrity");
        for (std::size_t i = i0; i < hist.size(); ++i) {
            const CausalEvent& ev = hist[i];
            llm_scanned = base + i;
            const std::string& a = ev.action;
            if (llm_skip_verb(a)) continue;
            const EntityId actor = ev.participants.empty() ? kNoEntity : ev.participants.front();
            const EntityId about = ev.participants.size() > 1 ? ev.participants[1] : kNoEntity;
            const Entity* ae = actor != kNoEntity ? w.find(actor) : nullptr;
            const Entity* be = about != kNoEntity ? w.find(about) : nullptr;
            // Salience proxy for |emotional_charge| >= 800 — the charge lives
            // in mind engrams, not on the event, so we key on what the
            // engram would encode: death, birth, rare-kind spawn, heavy trauma.
            const bool death = (be && !be->alive) || (ae && !ae->alive);
            const bool birth = (a == "reproduce" && !ev.failed);
            bool rare = false;
            for (EntityId sid : ev.spawned)
                if (const Entity* se = w.find(sid);
                    se && se->kind_name == SIMLAB_KIND("sapient")) { rare = true; break; }
            std::int64_t hurt = 0;
            for (const StateChange& ch : ev.changes) {
                if (ch.entity == kNoEntity) continue;
                if (ch.prop == pain_p) hurt = std::max(hurt, ch.after.i - ch.before.i);
                else if (ch.prop == integ_p) hurt = std::max(hurt, ch.before.i - ch.after.i);
            }
            const bool trauma = hurt >= 400 ||
                (be && integ_p != kNoProp && w.prop_int(*be, integ_p, 1000) <= 0);
            // Structured summary — used BOTH as a chapter fact (every action
            // event is recorded) and, for salient events, as the one-liner
            // chronicler prompt below.
            std::string s = "Day " + std::to_string(ev.tick / 24 + 1) + ", hour " +
                            std::to_string(ev.tick % 24) + ". ";
            s += NarrativeComposer::describe(w, actor);
            s += " "; s += a;
            if (about != kNoEntity) { s += " "; s += NarrativeComposer::describe(w, about); }
            if (ev.failed && !ev.failure.empty()) { s += " — failed: "; s += ev.failure; }
            // ---- Continuous chronicle: EVERY action event becomes a fact ---
            // (previously only the salient subset reached the .md, so whole
            // stretches of ordinary life were never recorded). Catastrophic
            // triggers are tracked even when the fact buffer is full.
            for (EntityId sid : ev.spawned)
                if (const Entity* se = w.find(sid))
                    if (chron_kinds.insert(std::string(se->kind_name)).second)
                        chron_catastrophic = true;
            if (death) {
                const Entity* dead = (ae && !ae->alive) ? ae : be;
                if (dead && dead->kind_name == SIMLAB_KIND("sapient")) {
                    bool any_sapient = false;
                    for (EntityId sid : w.entities_of_kind(SIMLAB_KIND("sapient")))
                        if (const Entity* se = w.find(sid); se && se->alive) {
                            any_sapient = true; break;
                        }
                    if (!any_sapient) chron_catastrophic = true;
                }
            }
            if (chron_facts.size() < kChronFactCap)
                chron_facts.push_back({ev.tick, s,
                    ae ? ae->pos : (be ? be->pos : Vec2{})});
            else ++chron_overflow;
            // ---- one-liner chronicler: salient events only ------------------
            // (skipped entirely while the model is offline — facts above are
            // still recorded, so nothing is lost from the chronicle)
            if (made >= 4 || !llm_host.active()) continue;
            if (!death && !birth && !rare && !trauma) continue;
            // Per actor+class cooldown: one feud must not fill the queue.
            const std::uint64_t cls = death ? 1 : birth ? 2 : rare ? 3 : 4;
            const std::uint64_t kh = std::uint64_t(actor) * 0x9E3779B97F4A7C15ull ^ cls;
            auto [it, ins] = llm_last_tell.try_emplace(kh, 0);
            const std::uint64_t cd = (death || birth) ? 480 : 960;   // ~20 / ~40 days
            if (it->second && ev.tick < it->second + cd) continue;
            it->second = ev.tick;
            NarrativeLine l;
            l.tick = ev.tick;
            l.pos = ae ? ae->pos : (be ? be->pos : Vec2{});
            l.who = actor; l.about = about;
            l.cat = death ? kCatCombat : (birth || rare) ? kCatSociety : kCatSurvival;
            l.callout = death || birth || rare;
            const std::uint64_t key = ++llm_key_seq;
            llm_pending.emplace(key, l);
            llm_outstanding.push_back(key);
            llm_host.submit(key, s);
            ++made;
        }
    }
    void llm_drain() {
        if (llm_outstanding.empty()) return;
        std::deque<std::uint64_t> keep;
        for (std::uint64_t key : llm_outstanding) {
            std::string text;
            if (!llm_host.poll(key, text)) { keep.push_back(key); continue; }
            auto it = llm_pending.find(key);
            if (it == llm_pending.end()) continue;    // stale (world was reset)
            NarrativeLine l = std::move(it->second);
            l.seq = cfeed.seq_next++;
            l.text = std::move(text);
            l.llm = true;
            cfeed.lines.push_back(std::move(l));
            while (cfeed.lines.size() > 400) cfeed.lines.pop_front();
            llm_pending.erase(it);
        }
        llm_outstanding.swap(keep);
    }

    // one frame: snapshot (const, under the world lock) -> draw -> UI
    void frame(double dt) {
        const auto t_frame = std::chrono::steady_clock::now();
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
        // NEVER wait for the simulation: if a cycle is running, draw the
        // last snapshot/caches (the world cannot have visibly changed
        // mid-cycle anyway) and ask the sim thread to yield next time.
        // Called twice per frame — a failed try_lock early in the frame gets
        // a second chance a few ms later, roughly doubling snapshot rate
        // while the sim holds the world mutex ~70% of the time.
        auto poll_world = [&]() {
            std::unique_lock<std::mutex> lk(mx, std::try_to_lock);
            if (lk.owns_lock() && has_game) {
                // fog of war: recompute the visible set ~6x/s; the flat
                // grid makes each update O(disk area) and the bulk fog
                // textures rebuild only when the generation counter moved.
                if (time_s - fog_update_t >= 0.12) { fog.update(w); fog_update_t = time_s; }   // ~8x/s whenever the lock is ours
                if (fog.generation() != fog_tex_gen) {
                    fog_tex_gen = fog.generation();
                    if (is_planet()) fog_mark_dirty(); else build_fog_ground();
                }
                if (frames % 30 == 1) update_minimap();
                // Tribe vision tracks LIVE speech communities: language_id
                // is not inherited (children converge on the per-kind hash
                // lang), so genesis tribe_langs go stale as founders die —
                // without this refresh Tribe fog ends up with ZERO observers
                // (black map, NPCs invisible beyond the last explored ring).
                if (frames % 600 == 5) {
                    derive_tribes();
                    if (!pw.tribe_lang.empty())
                        tribe_view = std::min(tribe_view, int(pw.tribe_lang.size()) - 1);
                    if (fog.mode() == FogOfWar::Mode::Tribe)
                        fog.set_mode(FogOfWar::Mode::Tribe, tribe_lang());
                }
                // snapshot 1.5x the view: panning while the sim is busy
                // still has content at the edges
                ViewRect v = cam.view();
                const std::int32_t mx_ = (v.x1 - v.x0) / 4, my_ = (v.y1 - v.y0) / 4;
                v.x0 -= mx_; v.x1 += mx_; v.y0 -= my_; v.y1 += my_;
                if (!globe) {                          // orbit view never needs the 2.5D snapshot
                    snap_back = capture(w, v, fog, &cap_scratch);
                    {   // publish atomically: draw code keeps reading `snap`
                        std::lock_guard<std::mutex> slk(snap_mx);
                        std::swap(snap, snap_back);
                        ++snap_gen;
                    }
                }
                c_tick = w.clock_.tick(); c_minds = w.minds_.size();
                // ---- action telegraph feed ---------------------------------
                // Scan ONLY new ledger events since the last poll (amortized
                // O(1)); maps actor -> last verb for the overhead label.
                // Purely display-side: nothing is written back to the World.
                {
                    const auto& hist = w.ledger.history();
                    // history_ is front-erased in batches, so an index cursor
                    // would silently rescan (and re-telegraph) old events —
                    // track the last EventId instead; ids are sequential.
                    const std::uint64_t base = w.ledger.next_id() - hist.size();
                    std::size_t i0 = hist.size();       // first poll: skip backlog
                    if (act_last) {
                        if (act_last + 1 < base) act_last = w.ledger.next_id() - 1;   // cursor evicted: skip backlog
                        else i0 = std::size_t(act_last + 1 - base);
                    } else {
                        act_last = w.ledger.next_id() - 1;   // mark backlog as seen
                    }
                    for (std::size_t i = i0; i < hist.size(); ++i) {
                        const CausalEvent& ev = hist[i];
                        act_last = base + i;
                        if (ev.participants.empty() || ev.action.empty()) continue;
                        const std::string& a = ev.action;
                        if (a == "metabolize" || a == "cool" || a == "ecology" ||
                            a == "political_drift" || a == "myth_drift" || a == "language" ||
                            a == "anatomy_drift" || a == "decay_minds" || a == "move" ||
                            a == "perceive" || a == "witness" || a == "advance_ticks") continue;
                        // DF-style badge: friendly label + theme color,
                        // lives ~30 world ticks (viewer-only state).
                        std::string label; ImU32 col = IM_COL32(205, 210, 220, 255);
                        EntityId actor = ev.participants[0];
                        EntityId target = ev.participants.size() > 1 ? ev.participants[1] : kNoEntity;
                        if (a == "consume" || a == "eat" || a == "forage") {
                            // drink vs eat: the target's material decides
                            const Entity* te = target != kNoEntity ? w.find(target) : nullptr;
                            const MaterialDef* tm = te ? w.materials.find(te->material) : nullptr;
                            const bool watery = tm && tm->name.find("water") != std::string::npos;
                            label = watery ? "Drink" : "Eat";
                            col = watery ? IM_COL32(90, 205, 240, 255) : IM_COL32(235, 175, 80, 255);
                        }
                        else if (a == "assemble" || a == "disassemble" || a == "craft" ||
                                 a == "compose_clause" || a == "inscribe")          { label = "Craft";  col = IM_COL32(230, 170, 70, 255); }
                        else if (a == "dig" || a == "extract" || a == "quarry")      { label = "Dig";    col = IM_COL32(190, 150, 100, 255); }
                        else if (a == "plant" || a == "sow")                          { label = "Plant";  col = IM_COL32(120, 200, 110, 255); }
                        else if (a == "harvest" || a == "gather")                     { label = "Gather"; col = IM_COL32(220, 190, 80, 255); }
                        else if (a == "strike" || a == "launch" || a == "attack")    { label = "Fight!"; col = IM_COL32(235, 70, 60, 255); }
                        else if (a == "treat" || a == "heal")                         { label = "Heal";   col = IM_COL32(120, 220, 220, 255); }
                        else if (a == "communicate" || a == "speak" || a == "chant") { label = "Talk";   col = IM_COL32(110, 220, 120, 255); }
                        else if (a == "perform_ritual" || a == "consecrate")         { label = "Rite";   col = IM_COL32(235, 200, 110, 255); }
                        else if (a == "rest" || a == "sleep")                        { label = "zZ";     col = IM_COL32(185, 190, 215, 255); }
                        else if (a == "reproduce" || a == "mate")                    { label = "Mate";   col = IM_COL32(235, 135, 200, 255); }
                        else if (a == "trade" || a == "gift" || a == "share")        { label = "Share";  col = IM_COL32(225, 200, 100, 255); }
                        else if (a == "steal")                                       { label = "Steal!"; col = IM_COL32(200, 90, 160, 255); }
                        else if (a == "treaty" || a == "parley" || a == "diplomatic_parley") { label = "Talk"; col = IM_COL32(130, 225, 225, 255); }
                        else if (a == "wield" || a == "don" || a == "doff" || a == "coat")  { label = "Gear";   col = IM_COL32(170, 160, 190, 255); }
                        else { label = a.substr(0, 10); }
                        if (ev.failed) { label += "!"; col = IM_COL32(150, 155, 165, 255); }
                        {
                            ThoughtBadge& b = c_badge[actor];
                            if (b.rank < 3) { b.label = label; b.col = col; b.until = c_tick + 30; b.rank = 2; }
                        }
                        // intent link: actor -> last interacted target
                        if (target != kNoEntity) {
                            IntentLink& il = c_intent[actor];
                            il.target = target;
                            if (const Entity* te = w.find(target)) il.pos = te->pos;
                            il.label = label;
                            il.until = c_tick + 60;
                        }
                        // ---- procedural animation state + event juice ------
                        // Pose/facing/gaze for the draw pass; particles are
                        // screen-space bursts spawned once per causal event.
                        {
                            AnimState& an = c_anim[actor];
                            an.verb = a; an.until = c_tick + 40;
                            an.target = target;
                            if (target != kNoEntity)
                                if (const Entity* te = w.find(target)) {
                                    an.tpos = te->pos; an.tpos_ok = true;
                                }
                            if (const Entity* ae = w.find(actor)) {
                                const float ax = cam.sx(float(ae->pos.x));
                                const float ay = cam.sy(float(ae->pos.y), elev_at(ae->pos));
                                const bool watery = label.compare(0, 5, "Drink") == 0;
                                if (a == "consume" || a == "eat" || a == "forage") {
                                    // droplets for drinking, crumbs for eating
                                    for (int k = 0; k < 5 && parts.size() < 600; ++k) {
                                        const float j = float((frames * 13 + k * 29) % 23) / 23.f - 0.5f;
                                        parts.push_back({ax + j * 8.f, ay - 8.f * cam.zoom,
                                            j * 26.f, watery ? -34.f - float(k % 3) * 9.f : -16.f - float(k) * 6.f,
                                            watery ? 0.55f : 0.8f,
                                            watery ? 0xFF5FD8F0 : 0xFFE0A040});
                                    }
                                } else if (a == "communicate" || a == "speak" || a == "chant" ||
                                           a == "treaty" || a == "parley" || a == "diplomatic_parley") {
                                    if (rings.size() < 40)
                                        rings.push_back({ax, ay - 10.f * cam.zoom, 3.f, 26.f, 0.7f, 0.7f, 0xFF80E0A0});
                                } else if (a == "reproduce" || a == "mate") {
                                    for (int k = 0; k < 3 && hearts.size() < 60; ++k)
                                        hearts.push_back({ax + float(k - 1) * 5.f, ay - 10.f * cam.zoom, float(k) * 0.12f});
                                } else if (a == "strike" || a == "launch" || a == "attack") {
                                    // victim hit-flash (viewer-side decal)
                                    if (target != kNoEntity) c_anim[target].flash = float(time_s) + 0.18f;
                                }
                            }
                        }
                        // newborns flash a "Born" badge (generation counted
                        // lazily by the inspector; the badge is generic)
                        for (EntityId nid : ev.spawned)
                            if (nid != actor) {
                                ThoughtBadge& nb = c_badge[nid];
                                if (nb.rank < 3) {
                                    nb.label = "Born"; nb.col = IM_COL32(255, 215, 90, 255);
                                    nb.until = c_tick + 60; nb.rank = 2;
                                }
                            }
                    }
                }
                // ---- thought-state scan: fear / rest / idle badges + ------
                // intent links from journeys & fresh brain commits. Reads
                // minds + brain slots READ-ONLY (never creates a slot).
                if (frames % 10 == 4) {
                    for (const EntityView& e : snap.entities) {
                        if (!e.minded || !e.alive || e.fog != Fog::Visible) continue;
                        const AgentMind* m = w.find_mind(e.id);
                        if (!m) continue;
                        const BrainSlot* bs = nullptr;
                        if (w.brains_) {
                            auto sit = w.brains_->slots.find(e.id);
                            if (sit != w.brains_->slots.end()) bs = &sit->second;
                        }
                        bool resting = bs && bs->vital.drive_fatigue > 650;
                        if (!resting && bs) {
                            int bi = -1, bv = 0;
                            for (int i = 0; i < kActionVerbs; ++i) {
                                const int v = std::abs(int(bs->last_action[i]));
                                if (v > bv) { bv = v; bi = i; }
                            }
                            if (bi >= 0) {
                                const char* vn = sensor_verb_table()[bi];
                                resting = vn && std::string(vn) == "rest";
                            }
                        }
                        const bool fear = m->neuro.panic > 550 ||
                                          (bs && bs->vital.emo[kBEmoFear] > 600);
                        if (fear) {
                            ThoughtBadge& b = c_badge[e.id];
                            b.label = "!! Fear"; b.col = IM_COL32(255, 70, 60, 255);
                            b.until = c_tick + 30; b.rank = 3;
                            // sudden fear -> sweat beads (viewer-side juice)
                            if (parts.size() < 600) {
                                const float fx = cam.sx(float(e.pos.x));
                                const float fy = cam.sy(float(e.pos.y), elev_at(e.pos));
                                for (int k = 0; k < 2; ++k)
                                    parts.push_back({fx + float(k * 3 - 1) * cam.zoom, fy - 14.f * cam.zoom,
                                        float(k - 1) * 8.f, -26.f, 0.5f, 0xFFAFD8FF});
                            }
                        } else {
                            auto bit = c_badge.find(e.id);
                            const bool fresh_action = bit != c_badge.end() &&
                                bit->second.rank >= 2 && bit->second.until > c_tick + 12;
                            if (!fresh_action) {
                                ThoughtBadge& b = c_badge[e.id];
                                if (resting) {
                                    if (b.rank < 2) {
                                        b.label = "zZ"; b.col = IM_COL32(170, 180, 215, 255);
                                        b.until = c_tick + 30; b.rank = 1;
                                    }
                                } else {
                                    auto iit = c_badge.find(e.id);
                                    const bool any_fresh = iit != c_badge.end() && iit->second.until > c_tick;
                                    if (!any_fresh && e.moved_tick + 30 < c_tick &&
                                        dstate.pending.find(e.id) == dstate.pending.end()) {
                                        b.label = "..."; b.col = IM_COL32(140, 145, 160, 255);
                                        b.until = c_tick + 30; b.rank = 0;
                                    }
                                }
                            }
                        }
                        // intent link: active journey beats a fresh brain
                        // commit target; the ledger-derived link is the fallback
                        auto pit = dstate.pending.find(e.id);
                        if (pit != dstate.pending.end() && pit->second.until > c_tick) {
                            IntentLink& l = c_intent[e.id];
                            l.target = pit->second.subject;
                            l.pos = pit->second.target;
                            l.label = "journey"; l.until = c_tick + 60;
                        } else if (bs && bs->pending.valid && bs->pending.pm_target != kNoEntity &&
                                   c_tick - bs->pending.tick < 80) {
                            IntentLink& l = c_intent[e.id];
                            l.target = bs->pending.pm_target;
                            if (const Entity* te = w.find(l.target)) l.pos = te->pos;
                            l.label = "reach"; l.until = c_tick + 60;
                        }
                        // ---- facial-expression + pose drivers ---------------
                        // All read-only: panic/emo fear -> wide eyes, dopamine/
                        // joy -> squint, fatigue/hunger/thirst -> heavy lids,
                        // rest verb -> side-lying pose, water cell -> ripples.
                        {
                            AnimState& an = c_anim[e.id];
                            an.fear = std::clamp(
                                std::max(int(m->neuro.panic), bs ? int(bs->vital.emo[kBEmoFear]) : 0) / 1000.f,
                                0.f, 1.f);
                            const int joy_raw = bs ? std::max(int(bs->vital.mod[kBModDopa]),
                                                               int(bs->vital.emo[kBEmoJoy])) : 0;
                            an.joy = std::clamp(joy_raw / 1000.f, 0.f, 1.f);
                            const int drain = bs ? std::max({int(bs->vital.drive_fatigue),
                                                             int(bs->vital.drive_hunger),
                                                             int(bs->vital.drive_thirst)}) : 0;
                            an.tired = std::clamp(drain / 1000.f, 0.f, 1.f);
                            an.lying = resting;
                            an.in_water = w.terrain.is_water(e.pos);
                            an.expr_until = c_tick + 30;
                            // gaze prefers the freshest intent target (journey
                            // or brain commit), else the last ledger target
                            auto iit = c_intent.find(e.id);
                            if (iit != c_intent.end() && iit->second.until > c_tick &&
                                iit->second.target != kNoEntity) {
                                an.target = iit->second.target;
                                an.tpos = iit->second.pos; an.tpos_ok = true;
                            }
                        }
                    }
                }
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
                // Narrative chronicle: same incremental ledger scan, narrates
                // events near the camera + everything about pinned legends.
                if (frames % 4 == 2) {
                    ViewRect nv = cam.view();
                    composer.scan(w, cfeed, &nv, journal.pins, &journal);
                    for (const NarrativeLine& l : cfeed.lines) {
                        if (l.seq <= narr_seen) continue;
                        narr_seen = l.seq;
                        if (!l.callout) continue;
                        ImU32 col = IM_COL32(215, 215, 225, 255);
                        if (l.llm) col = IM_COL32(255, 230, 140, 255);
                        else if (l.cat & kCatCombat) col = IM_COL32(255, 110, 90, 255);
                        else if (l.cat & kCatDiscovery) col = IM_COL32(255, 205, 90, 255);
                        else if (l.cat & kCatSociety) col = IM_COL32(120, 215, 225, 255);
                        else if (l.cat & kCatSurvival) col = IM_COL32(140, 225, 130, 255);
                        c_callouts.push_back({l.pos, float(l.seq % 7) * 10.f - 30.f, l.text, frames + 260, col});
                    }
                    while (c_callouts.size() > 48) c_callouts.pop_front();
                }
                // LLM chronicler: same incremental ledger scan, salient events
                // only; results re-enter cfeed tagged llm=true.
                if (frames % 4 == 2) { llm_scan(w); llm_drain(); mat_scan(w); llm_mat_drain(); myth_scan(w); llm_myth_drain(); chron_tick(w); llm_ch_drain(); sunny_chron.tick(w, llm_host.active() ? &llm_host : nullptr); }
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
                const std::uint64_t key = (c_tick / 240) * 16 + std::uint64_t(lens);
                if (!globe && !is_planet() && (key != ground_key || !ground)) { build_ground(); ground_key = key; }
            } else if (frames % 8 == 0) {
                // ask the sim thread for a longer pause — but only on a
                // periodic miss, not every failed try_lock, or the request
                // flag is ~always set and halves simulation throughput.
                ui_wants = true;
            }
        };
        poll_world();
        t_cap = ms(t); t = clk::now();
        globe_step(dt);
        // smooth camera fly-to (chronicle chapter click): ease cx/cy/zoom
        if (cam_goal) {
            const float k = std::min(1.f, float(dt) * 3.5f);
            cam.cx += (cam_gx - cam.cx) * k;
            cam.cy += (cam_gy - cam.cy) * k;
            cam.zoom += (cam_gz - cam.zoom) * k;
            if (std::abs(cam_gx - cam.cx) < 0.6f && std::abs(cam_gy - cam.cy) < 0.6f &&
                std::abs(cam_gz - cam.zoom) < 0.01f) cam_goal = false;
        }
        if (!has_game) {
            draw_title_globe(dt);
        } else if (globe) {
            draw_globe();
        } else {
            if (!(skip & 1)) {
                if (is_planet()) { draw_ground_chunks(); draw_fog_chunks(); }
                else { draw_terrain(); draw_water_glints(); }
            }
            t_ter = ms(t); t = clk::now();
            if (!(skip & 2)) draw_entities(dt);
            draw_links();
            draw_intents();
            draw_callouts();
            if (!(skip & 4)) draw_night();
        }
        SDL_SetRenderTarget(ren, nullptr);
        if (!(skip & 32)) SDL_RenderTexture(ren, world_rt, nullptr, nullptr);     // nearest x kPix
        t_ent = ms(t); t = clk::now();
        {   // second lock attempt: the world draw above took ~5 ms, so the
            // sim's slice/sleep phase may have rolled over — retry so UI
            // windows read a fresher snapshot this frame.
            const auto tp = clk::now();
            poll_world();
            t_cap += ms(tp);
        }
        if (in_menu || !has_game) { if (!in_menu) open_menu(kTitle); ui_menu(); }
        else if (!(skip & 16)) {
            if (globe) draw_globe_icons(); else draw_overlays();
            ui_topbar();
            if (!globe) { if (is_planet()) ui_mini_globe(); else ui_minimap(); }
            ui_inspector(); ui_player(); ui_journal(); ui_successor(); ui_toast(); ui_ticker();
            if (!show_journal) ui_word_editor();
        }
        if (!in_menu) {
            if (!(skip & 8) && !(skip & 128)) ui_atlas();
            if (!(skip & 8) && !(skip & 256)) ui_codex();
        }
        t_ui = ms(t);
        t = clk::now();
        ImGui::Render();
        ImDrawData* dd = ImGui::GetDrawData();
        static const bool ui_direct = std::getenv("SIMLAB_VSKIP") && (std::atoi(std::getenv("SIMLAB_VSKIP")) & 64);
        // Heavy reference windows (Atlas gallery, Chronicle codex) cost ~25 ms
        // to software-rasterize, so their draw lists go to a separate cached
        // layer refreshed ~7.5x/s (or on any UI event) and blitted back at the
        // window rect captured at rasterize time. HUD stays cheap + live.
        ImGuiWindow* aux_win[2] = {
            ImGui::FindWindowByName("Data Atlas Gallery (generated from data/*.txt)"),
            ImGui::FindWindowByName("Chronicle & Rosetta Codex")};
        ImDrawList* slow[2] = {nullptr, nullptr};
        int slow_idx[2] = {-1, -1}, n_slow = 0;
        for (int i = 0; i < 2; ++i)
            if (aux_win[i] && aux_win[i]->DrawList &&
                std::find(dd->CmdLists.begin(), dd->CmdLists.end(), aux_win[i]->DrawList) != dd->CmdLists.end()) {
                slow[n_slow] = aux_win[i]->DrawList; slow_idx[n_slow] = i; ++n_slow;
            }
        ImDrawData fast_dd, slow_dd;
        ImVector<ImDrawList*> fast_lists, slow_lists;
        if (n_slow) {
            for (ImDrawList* l : dd->CmdLists) {
                bool s = false;
                for (int i = 0; i < n_slow; ++i) if (l == slow[i]) { s = true; break; }
                (s ? slow_lists : fast_lists).push_back(l);
            }
            fast_dd = *dd; fast_dd.CmdLists = fast_lists;
            slow_dd = *dd; slow_dd.CmdLists = slow_lists;
        } else {
            fast_dd = *dd;
        }
        // Re-rasterize ui_rt only when the UI could have changed: an input
        // event / explicit dirty, the fading toast, a new snapshot or world
        // tick (clock text, resonance), or a periodic ~30 Hz / 8-frame safety
        // net. Paused and menu-idle frames skip the glyph pass entirely.
        const bool ui_anim = !toast_msg.empty() && (time_s - toast_t) < 4.5;
        const bool ui_event = ui_dirty;   // sampled before the fast pass clears it
        const bool ui_urgent = ui_event || ui_anim;
        const bool ui_periodic = (frames % 3 == 0) &&
            (snap_gen != ui_last_snap_gen || c_tick != ui_last_tick || frames % 9 == 0);
        auto raster = [&](ImDrawData* d, SDL_Texture* target) {
            SDL_SetRenderTarget(ren, target);
            SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 0);
            SDL_RenderClear(ren);
            ImGui_ImplSDLRenderer3_RenderDrawData(d, ren);
            SDL_SetRenderTarget(ren, nullptr);
        };
        if (ui_direct) {
            ImGui_ImplSDLRenderer3_RenderDrawData(dd, ren);
        } else {
            if (ui_urgent || ui_periodic) {
                raster(&fast_dd, ui_rt);
                ui_dirty = false;
                ui_last_snap_gen = snap_gen;
                ui_last_tick = c_tick;
            }
            // aux refresh: ~7.5 Hz baseline, or immediately while the pointer/
            // keyboard is captured by ImGui (hover, InputText, window drag) —
            // world pans must NOT retrigger the expensive pass every frame.
            const ImGuiIO& io = ImGui::GetIO();
            if (n_slow && ((ui_event && (io.WantCaptureMouse || io.WantCaptureKeyboard)) || frames % 32 == 0)) {
                raster(&slow_dd, aux_rt);
                for (int i = 0; i < n_slow; ++i) {
                    const ImRect r = aux_win[slow_idx[i]]->Rect();
                    float* b = aux_rect[slow_idx[i]];
                    b[0] = (r.Min.x - dd->DisplayPos.x) * dd->FramebufferScale.x;
                    b[1] = (r.Min.y - dd->DisplayPos.y) * dd->FramebufferScale.y;
                    b[2] = (r.Max.x - dd->DisplayPos.x) * dd->FramebufferScale.x - b[0];
                    b[3] = (r.Max.y - dd->DisplayPos.y) * dd->FramebufferScale.y - b[1];
                    aux_ok[slow_idx[i]] = true;
                }
            }
            SDL_RenderTexture(ren, ui_rt, nullptr, nullptr);
            for (int i = 0; i < 2; ++i) if (aux_ok[i] && aux_win[i] && aux_win[i]->DrawList &&
                std::find(dd->CmdLists.begin(), dd->CmdLists.end(), aux_win[i]->DrawList) != dd->CmdLists.end()) {
                SDL_FRect src{aux_rect[i][0], aux_rect[i][1], aux_rect[i][2], aux_rect[i][3]}, dst = src;
                SDL_RenderTexture(ren, aux_rt, &src, &dst);
            }
        }
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
        const double total_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_frame).count();
        if (std::getenv("SIMLAB_VDEBUG") && total_ms > 24.0)
            std::printf("  SLOW frame %llu total %.1f: capture %.1f terrain %.1f entities %.1f ui %.1f imgui-draw %.1f present %.1f (ents %zu)\n",
                        (unsigned long long)frames, total_ms, t_cap, t_ter, t_ent, t_ui, t_im, t_pr, snap.entities.size());
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
    // Renderer selection: hardware-accelerated by default (SDL picks
    // Vulkan/D3D11/GL/GLES as available). Software only when headless
    // (dummy video driver, e.g. selftest) or forced via SIMLAB_RENDERER.
    // SIMLAB_RENDERER=software|opengl|vulkan|... overrides everything.
    const char* want = std::getenv("SIMLAB_RENDERER");
    const bool headless = selftest > 0 ||
        (std::getenv("SDL_VIDEODRIVER") && std::string(std::getenv("SDL_VIDEODRIVER")) == "dummy") ||
        (std::getenv("SDL_VIDEO_DRIVER") && std::string(std::getenv("SDL_VIDEO_DRIVER")) == "dummy");
    if (want) {
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, want);
    } else if (headless) {
        // software only: never load the GL/EGL driver stack (on a GPU box it
        // alone costs ~80 MB of RSS; on a GPU-less box it does not exist)
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
        SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "0");
    } else {
        // pixel-art sharpness is per-texture: world_rt/ui_rt/globe_tex all
        // set SDL_SCALEMODE_NEAREST/LINEAR explicitly (SDL3 dropped the
        // SDL_HINT_RENDER_SCALE_QUALITY hint).
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) { std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    World world(seed, data);
    if (!world.content_error.empty()) { std::fprintf(stderr, "content: %s\n", world.content_error.c_str()); return 1; }
    // 4GB-machine budget: keep the causal ledger window under 16 MB in RAM
    // (older events fold into the archive digest — state_hash stays exact)
    world.ledger.set_byte_budget(16u << 20);
    Viewer v(world);
    v.data_dir = data;
    if (const char* pd = std::getenv("SIMLAB_PREF")) v.pref_dir = pd;          // tests: never touch the user's saves
    else if (char* pref = SDL_GetPrefPath("Sunnyworld", "SimLab")) { v.pref_dir = pref; SDL_free(pref); }
    else v.pref_dir = "./";
    std::snprintf(v.seed_buf, sizeof v.seed_buf, "%llu", (unsigned long long)seed);
    if (selftest > 0) {   // selftest world (the interactive game starts at the title menu)
        PrimordialParams p; p.seed = seed; p.grid = 64; p.tribes = 2; p.per_tribe = 6;
        if (const char* pl = std::getenv("SIMLAB_VPLANET")) {      // e.g. 2048 -> 2048 x 1024 planet
            p.planet = true; p.planet_w = std::atoi(pl); p.planet_h = p.planet_w / 2; p.grid = 128;
        }
        v.pp = p;
        v.pw = build_primordial(world, p);
        if (!v.pw.homes.empty()) { v.cam.cx = float(v.pw.homes[0].x); v.cam.cy = float(v.pw.homes[0].y); }
        v.fog.set_mode(FogOfWar::Mode::Tribe, v.tribe_lang());
        if (const char* sb = std::getenv("SIMLAB_SIM_BENCH")) {   // pure-sim timing, no render
            const int n = std::atoi(sb);
            // SIMLAB_SIM_STRIDE=N emulates warp LOD (dormant metabolize
            // batching + perceive memory cache) without the UI.
            if (const char* st = std::getenv("SIMLAB_SIM_STRIDE"))
                world.set_lod_stride(std::atoll(st));
            std::vector<double> ms;
            ms.reserve(n);
            for (int c = 0; c < n; ++c) {
                const auto a = std::chrono::steady_clock::now();
                autonomous_cycle(world, &v.dstate);
                ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count());
            }
            std::sort(ms.begin(), ms.end());
            double sum = 0; for (double x : ms) sum += x;
            std::size_t alive_n = 0;
            for (const auto& ep : world.entities) if (ep->alive) ++alive_n;
            std::printf("simbench: cycles=%d avg=%.1fms p50=%.1f p95=%.1f entities=%zu alive=%zu minds=%zu hash=%016llx\n",
                        n, sum / n, ms[n / 2], ms[n * 95 / 100], world.entities.size(), alive_n, world.minds_.size(),
                        (unsigned long long)world.state_hash());
            return 0;
        }
    }
    v.speed = 0; v.speed_atomic = 0;               // paused under the title menu
    v.has_game = selftest > 0;
    // CL-1 Phase 3: start the async Qwen chronicler on real runs only —
    // selftests/headless runs skip it (SIMLAB_NO_LLM=1 also disables).
    if (selftest <= 0 && !std::getenv("SIMLAB_VTITLE") && !std::getenv("SIMLAB_NO_LLM"))
        v.start_llm();
    v.title_map.build(WorldGenerator::planet_seed(std::uint32_t(seed), 1024, 512), 1024, 512, 1024);
    v.title_view.radius = 300;
    v.win = SDL_CreateWindow("Sunnyworld - Primordial Sandbox", v.win_w, v.win_h, SDL_WINDOW_RESIZABLE);
    if (!v.win) { std::fprintf(stderr, "window: %s\n", SDL_GetError()); return 1; }
    // nullptr = SDL's preferred driver (accelerated when a GPU backend
    // exists); software is the explicit fallback for headless/weak boxes.
    v.ren = SDL_CreateRenderer(v.win, nullptr);
    if (!v.ren) v.ren = SDL_CreateRenderer(v.win, "software");
    if (!v.ren) { std::fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); return 1; }
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
    if (std::getenv("SIMLAB_VTITLE")) {
        // menu flow smoke test: title -> new game -> save & quit -> continue
        v.has_game = false; v.in_menu = true; v.screen = Viewer::kTitle;
        for (int f = 0; f < 20; ++f) v.frame(1.0 / 60);
        if (!shot.empty()) { v.ui_dirty = true; v.shot_path = shot + "_title.bmp"; v.frame(1.0 / 60); }
        const int before = v.newest_slot();
        PrimordialParams p; p.seed = 77; p.grid = 64; p.tribes = 1; p.per_tribe = 4;
        { std::lock_guard<std::mutex> lk(v.mx); v.new_world(p); }
        v.has_game = true; v.close_menu();
        for (int c = 0; c < 5; ++c) autonomous_cycle(world, &v.dstate);
        for (int f = 0; f < 10; ++f) v.frame(1.0 / 60);
        const std::uint64_t h = world.state_hash();
        v.open_menu(Viewer::kPause);
        v.save_slot(0); v.has_game = false; v.screen = Viewer::kTitle;
        for (int f = 0; f < 5; ++f) v.frame(1.0 / 60);
        const int ns = v.newest_slot();
        const bool ok_load = ns >= 0 && v.load_slot(ns);
        v.close_menu();
        for (int f = 0; f < 5; ++f) v.frame(1.0 / 60);
        std::printf("title: newest_before=%d newest_after=%d continue_loaded=%d has_game=%d hash_restored=%d menu_closed=%d\n",
                    before, ns, int(ok_load), int(v.has_game.load()), int(world.state_hash() == h), int(!v.in_menu));
        return ok_load && world.state_hash() == h ? 0 : 5;
    }
    if (selftest > 0) {
        // headless self-test: (1) render-only frames must leave state_hash
        // untouched; (2) then run the sim and report frame time + RSS.
        v.show_atlas = v.show_codex = true;
        v.in_menu = false;
        const std::uint64_t h0 = world.state_hash();
        const bool dbg = std::getenv("SIMLAB_VDEBUG") != nullptr;
        if (dbg) std::printf("rss before frames %zu\n", rss_kb());
        for (long f = 0; f < selftest / 2; ++f) {
            v.lens = int((f / 40) % 7);
            const auto tf = std::chrono::steady_clock::now();
            v.frame(1.0 / 60);
            if (dbg && f < 6) std::printf("frame %ld %.1fms rss %zu tex %zu\n", f,
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tf).count(), rss_kb(), v.tex.map.size());
        }
        if (std::getenv("SIMLAB_VCHRON")) {
            // repro: click rows of the Chronicle tab (Field Journal) with
            // real ImGui input
            v.show_atlas = v.show_codex = false;
            v.show_journal = true; v.journal_tab_req = 1;
            int c = 0;
            for (; c < 150 && v.cfeed.lines.size() < 3; ++c) { autonomous_cycle(world, &v.dstate); v.frame(1.0 / 60); }
            std::printf("chron: %zu lines after %d cycles\n", v.cfeed.lines.size(), c);
            ImGuiIO& io = ImGui::GetIO();
            ImGuiWindow* cw = ImGui::FindWindowByName("Field Journal");
            const ImVec2 wp = cw ? cw->Pos : ImVec2(10, v.win_h * 0.52f);
            std::printf("chron: window at %.0f,%.0f size %.0fx%.0f\n", wp.x, wp.y, cw ? cw->Size.x : 0.f, cw ? cw->Size.y : 0.f);
            auto click = [&](float x, float y, int n) {
                for (int r = 0; r < n; ++r) {
                    io.AddMousePosEvent(x, y); v.frame(1.0 / 60);
                    io.AddMouseButtonEvent(0, true); v.frame(1.0 / 60);
                    io.AddMouseButtonEvent(0, false); v.frame(1.0 / 60);
                }
                for (int f = 0; f < 6; ++f) v.frame(1.0 / 60);
            };
            for (int k = 0; k < 8; ++k) {
                click(wp.x + 90, wp.y + 64 + k * 19, k % 2 ? 2 : 1);
                std::printf("chron: row click %d ok selected=%llu\n", k, (unsigned long long)v.selected);
            }
            for (int b = 0; b < 6; ++b) { click(wp.x + 20 + b * 62, wp.y + 38, 1); std::printf("chron: filter %d ok\n", b); }
            for (int f = 0; f < 30; ++f) { autonomous_cycle(world, &v.dstate); v.frame(1.0 / 60); }
            std::printf("chron: survived\n");
            return 0;
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
            if (!shot.empty()) {
                for (int f = 0; f < 40; ++f) v.frame(1.0 / 60);          // let chunk builds finish
                v.ui_dirty = true; v.shot_path = shot + "_surface.bmp"; v.frame(1.0 / 60);
            }
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
            v.show_journal = !std::getenv("SIMLAB_VNOJOURNAL");
            if (const char* z = std::getenv("SIMLAB_VZOOM")) v.cam.zoom = float(std::atof(z));
            if (std::getenv("SIMLAB_VGOD")) v.fog.set_mode(FogOfWar::Mode::GodEye);
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
        for (long f = selftest / 2; f < selftest; ++f) { v.lens = int((f / 40) % 7); v.frame(1.0 / 60); }
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
            if (const char* vs = std::getenv("SIMLAB_VSPEED")) v.speed = std::atoi(vs);
            v.speed_atomic = v.speed;
            const std::uint64_t snap0 = v.snap_gen;
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
            std::printf("live: frames=%zu avg_ms=%.2f p95_ms=%.2f max_ms=%.2f tick=%llu minds=%zu captures=%llu\n", ft.size(),
                        sum / double(ft.size()), ft[ft.size() * 95 / 100], ft.back(),
                        (unsigned long long)world.clock_.tick(), world.minds_.size(),
                        (unsigned long long)(v.snap_gen - snap0));
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
    if (v.has_game && selftest <= 0) v.save_slot(0);   // closing the window autosaves -> Continue
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
