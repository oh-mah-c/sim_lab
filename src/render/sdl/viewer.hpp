#pragma once
// Phase 13/14 — sunnyworld_viewer: "Sunnyworld - Primordial Sandbox".
// Split from the original monolithic viewer_main.cpp: shared helpers
// and the Viewer class declaration live here; method bodies are in
// viewer_*.cpp (planet/terrain, entities, HUD, god-tools, LLM, core).

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

namespace simlab_viewer {


// display-only short entity id (low 5 digits; full id in tooltips/inspector)
inline std::string sid(EntityId id) { return "#" + std::to_string(id % 100000); }

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
inline const char* kLensName[] = {"1 Natural", "2 Epidemiology", "3 Economy", "4 Politics", "5 Sacred", "6 Military", "7 Ecology"};

inline std::size_t rss_kb() {
    std::ifstream f("/proc/self/status"); std::string l;
    while (std::getline(f, l)) if (l.rfind("VmRSS:", 0) == 0) return std::stoul(l.substr(6));
    return 0;
}

inline void set_color(SDL_Renderer* r, std::uint32_t c, int a = -1) {
    SDL_SetRenderDrawColor(r, (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, a < 0 ? (c >> 24) & 0xFF : a);
}

// dark-parchment ImGui theme
inline void apply_theme() {
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


    PrimordialParams pp;

               // last genesis parameters
    PrimordialWorld pw;

                // homes / tribe ids (derived on load)
    int tribe_view = 0;

                // index into pw.tribe_lang for Tribe vision
    // ---- game layer (viewer state, not World Truth) ----
    bool in_menu = true;


    int screen = 0;

                    // Screen (menu page)
    std::atomic<bool> has_game{false};

 // a world is loaded/created (sim thread checks it)
    PlanetMap title_map;

               // decorative planet behind the title menu
    GlobeView title_view;


    char seed_buf[24] = "2026";


    int menu_grid = 2, menu_tribes = 2, menu_per = 6;


    std::string pref_dir;

              // SDL_GetPrefPath("Sunnyworld","SimLab")
    std::string status_msg;


    PatronJournal journal;

             // player knowledge + Resonance (saved as .journal)
    enum Tool { kToolNone = 0, kToolBeacon, kToolBlueprint, kToolFear,
                kToolCupid, kToolPanic, kToolRain, kToolFeast, kToolPredator,
                kToolMiracle, kToolBounty, kToolRelic, kToolTempest };


    int tool = kToolNone;


    EntityId cupid_a = kNoEntity;

      // first pick of a Cupid's-arrow pair
    bool god_open = true;

              // God-Hand toolbar row expanded
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


    std::set<std::int64_t> c_avatar_knows;

   // entry phoneme words the avatar knows (lang<<32 ^ hash)
    std::map<std::string, std::string> c_avatar_gloss;

   // word -> concept (avatar's own understanding)
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


    std::uint64_t narr_seen = 0;

             // last NarrativeLine.seq drained
    int chron_filter = 0;

                    // 0 all 1 combat 2 survival 3 discovery 4 society 5 pinned
    std::map<EntityId, std::vector<std::string>> c_carved;

   // legend -> translated stone words
    std::uint64_t c_carved_tick = ~0ull;

                      // rebuild gate (world tick)
    bool need_successor = false;


    EntityId fallen = kNoEntity;


    // ---- CL-1 Phase 3: async LLM chronicler (Qwen via NarrativeHost) ---------
    // Presentation only: the worker thread never touches World state, so
    // state_hash() is identical with the host on or off. `llm_host` is
    // started explicitly by start_llm() (main, non-selftest runs only);
    // llm_starter joins it so model load never blocks the UI thread.
    NarrativeHost llm_host;


    std::thread llm_starter;


    EventId llm_scanned = 0;

               // ledger cursor (mirrors cfeed.scanned)
    bool llm_primed = false;


    std::uint64_t llm_key_seq = 0;

         // outbound narration keys
    std::unordered_map<std::uint64_t, NarrativeLine> llm_pending;

  // key -> line stub
    std::deque<std::uint64_t> llm_outstanding;

                     // FIFO poll order
    std::unordered_map<std::uint64_t, std::uint64_t> llm_last_tell;

 // actor^class -> tick
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


    std::set<std::string> chron_kinds;

          // every kind ever seen (new-species trigger)
    std::uint64_t chron_win_start = 0;


    bool chron_catastrophic = false;


    std::vector<ChronicleChapter> chron_chapters;

   // oldest -> newest (memory cap)
    std::unordered_map<std::uint64_t, ChapterStub> llm_ch_pending;


    std::deque<std::uint64_t> llm_ch_outstanding;


    std::string chron_tail;

                     // last ~3 sentences of prev chapter
    std::string chron_path;

                     // saves/chronicle_<seed>.md
    // ---- Sunnyworld Chronicle: era-boundary literary novelist -------------
    // Read-only collector -> NarrativeHost worker -> saves/sunnyworld_
    // chronicle.md. Lives outside World; cannot affect state_hash().
    SunnyChronicle sunny_chron;


    int chron_open = -1;

                        // expanded chapter row (-1 none)
    static constexpr std::size_t kChronFactCap = 1024;

  // facts per chapter window
    std::uint64_t chron_overflow = 0;

           // facts dropped past the cap (this window)
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


    std::size_t mat_scanned = 0;

                    // synthesized_ cursor
    std::unordered_map<std::uint64_t, MatPending> llm_mat_pending;


    std::deque<std::uint64_t> llm_mat_outstanding;


    std::vector<MatEntry> mat_entries;

              // catalog (file-backed)
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


    std::set<std::string> myth_seen;

                // doctrine dedup keys
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


    int pool_rounds = 0;

                 // sim_loop counter -> periodic save
    std::atomic<int> warp{0};

            // headless cycles queued by Time Warp
    std::atomic<int> mut_permille{-1};

   // -1 = keep each organism's own prop
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


    void post(std::function<void()> f) ;


    std::vector<std::pair<std::int64_t, std::string>> c_stone_words;

   // per stone: lang, words joined
    EntityId avatar = kNoEntity;

       // possessed agent
    std::vector<EntityId> av_targets;

  // last clicked things (front = newest)
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


    std::uint64_t snap_gen = 0;

         // bumped on every front-buffer swap
    CaptureScratch cap_scratch;

      // wealth-scan cache shared across frames
    int lens = kNatural;


    int speed = 1;

                 // 0 pause, else cycles per second index
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


    std::unordered_map<EntityId, float> dust_at;

        // last dust puff (time_s)
    // Action telegraph: last causal verb per actor, scanned incrementally
    // from the ledger — display-only.
    std::uint64_t act_last = 0;

                          // last scanned EventId
    // ---- DF-style HUD (viewer-only, never serialized) ------------------------
    // Thought/action badges above NPCs: label + color + world-tick expiry.
    // rank resolves collisions: fear(3) > action(2) > rest(1) > idle(0).
    struct ThoughtBadge { std::string label; ImU32 col = 0; std::uint64_t until = 0; int rank = 0; };


    std::unordered_map<EntityId, ThoughtBadge> c_badge;


    // remembered-target links for selected/hovered NPCs (dashed intent line)
    struct IntentLink { EntityId target = kNoEntity; Vec2 pos{}; std::string label; std::uint64_t until = 0; };


    std::unordered_map<EntityId, IntentLink> c_intent;


    EntityId hover_id = kNoEntity;

                       // nearest entity to cursor
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
        float tantrum_until = -1.f;          // stomp-bounce deadline (time_s)
        bool mourning = false;               // grief>300: droop, slow gait, tears
        float argue = 0.f;                   // hot temper + fresh grudge: lean-in
        float proud = 0.f;                   // high dopa + satiated: chest-puff
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


    // comic glyph pops (! ? ~) for arguments — world_rt px anchor + age
    struct Sym { float x, y, t; char c; };


    std::vector<Sym> syms;


    // Miracle tool: golden beam columns — world pos + spawn time (time_s)
    struct Beam { float x, y, t0; };


    std::vector<Beam> beams;


    // emergent gatherings: minds intent-fixed on one focal entity
    struct Gather { EntityId focal = kNoEntity; Vec2 pos{};
                    std::vector<EntityId> members; };


    std::vector<Gather> c_gather;


    std::unordered_map<EntityId, EntityId> gather_of;

   // member -> focal
    // carried items: holder -> [(item, icon color)] rebuilt every ~20 scans
    struct HeldItem { EntityId id = kNoEntity; std::uint32_t col = 0xFF888888; };


    std::unordered_map<EntityId, std::vector<HeldItem>> c_held;


    // wielded equipment: entity -> icon color (from EntityView.w_mat)
    std::unordered_map<EntityId, std::uint32_t> c_wield_col;


    // footpath wear: client-side traffic heat per terrain cell
    std::unordered_map<std::uint64_t, std::uint16_t> path_heat;


    std::uint64_t path_tick = 0;

                       // last heat update (frames)
    // Tempest flash: screen-space white blink timestamp (time_s)
    double storm_t0 = -100.0;


    // mind-reader: one outstanding Qwen interrogation per selected NPC
    std::uint64_t mind_ask_key = 0, mind_ask_seq = 0;


    EntityId mind_ask_id = kNoEntity;


    std::string mind_reply;


    bool mind_ask_busy = false;


    int mind_ask_wait = 0;


    bool show_badges = true, show_ticker = true;


    bool show_atlas = false, show_codex = false, show_inspector = true;


    std::map<std::string, std::string> rosetta;

     // player guesses: glyph token -> meaning
    SDL_Texture* glow = nullptr;


    Sprite glow_sprite;


    double time_s = 0;


    std::uint64_t frames = 0;


    // ground cache: the whole terrain pre-composited once into ONE target
    // texture (2 px per map unit); a frame blits the visible slice of it.
    static constexpr int kGround = 2;

              // px per map unit
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


    float aux_rect[2][4] = {};

        // x, y, w, h in window px (framebuffer space)
    bool aux_ok[2] = {};


    // fog rebuild gates: bulk fog textures regenerate only when the flat
    // fog grid's generation counter actually moved.
    std::uint64_t fog_tex_gen = ~0ull, mini_fog_gen = ~0ull, globe_fog_gen = ~0ull;


    double fog_update_t = -1;


    int win_w = 1280, win_h = 800;


    float ux(float x) const ;

      // world-target px -> window px
    // simulation runs on its own thread; the UI thread takes the lock only
    // to snapshot + read World for its windows (never to write it)
    std::mutex mx;


    std::atomic<bool> running{true};


    std::atomic<int> speed_atomic{1};


    std::thread sim_thread;


    std::string shot_path;

          // --shot FILE.bmp: save the next composed frame
    std::atomic<bool> ui_wants{false};

   // UI asks the sim thread to yield the lock
    // UI caches: refreshed ONLY while the UI thread holds the world lock;
    // every window renders from them, so a long sim cycle never stalls a frame
    std::uint64_t c_tick = 0;

 std::size_t c_minds = 0;


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
        std::string mind_digest;        // text fed to Qwen for Ask/Read Mind
    }

 insp;


    struct Stone { EntityId id; std::int64_t lang; std::string text, treaty; };


    std::vector<Stone> c_stones;


    std::string c_codex;


    std::vector<std::string> c_codex_lines;

   // c_codex split for clipped display
    std::vector<std::string> c_chron;



    explicit Viewer(World& world);


    ~Viewer() ;


    // Kick the Qwen chronicler off a background thread (model load takes
    // seconds; the title menu must stay responsive). Safe no-op when the
    // llama build or GGUF model is absent.
    void start_llm() ;


    // Model/lib paths are relative to the repo root; when the viewer is
    // launched from elsewhere, retry relative to the executable location.
    static std::string llm_resolve(const std::string& rel) ;



    std::int64_t tribe_lang() const ;


    // reset every viewer-side cache after the World was replaced
    void reset_view() ;


    // ---- New Primordial World (caller holds the lock / sim paused) ----
    void new_world(const PrimordialParams& p) ;


    // tribes of a loaded world: distinct language_id among living minds
    void derive_tribes() ;


    std::string slot_path(int slot) const ;


    void save_slot(int slot) ;


    bool load_slot(int slot) ;



    // ======================= planet: globe + far surface =======================
    // The planet beyond the materialized home region is ANALYTIC (pure field
    // functions of the worldgen seed): the globe samples it once into an
    // albedo map; the 2.5D surface pulls 32x32-cell colour tiles on demand.
    static constexpr double kPI = 3.14159265358979323846;


    bool is_planet() const ;


    void planet_setup() ;


    double planet_units_w() const ;


    double planet_units_h() const ;


    void lonlat_of(double x, double y, double& lon, double& lat) const ;


    void pos_of(double lon, double lat, float& x, float& y) const ;


    void wrap_camera() ;


    void enter_globe() ;


    void enter_surface(double lon, double lat) ;


    void dive_to(double lon, double lat) ;


    static double ang_lerp(double a, double b, double t) ;


    // wheel/pinch on the globe: zoom toward the point under (px, py) (world_rt px)
    void globe_zoom(double factor, float px, float py) ;


    void globe_step(double dt) ;


    // fog texels (under the lock): explored -> 1, visible -> 2
    void build_globe_fog() ;


    double sun_lon() const ;


    // title screen: a slowly turning decorative planet behind the menu
    void draw_title_globe(double dt) ;


    void draw_globe() ;


    // strategic icons on the sphere (cached under the lock, drawn in the UI layer)
    void build_globe_icons() ;


    void draw_globe_icons() ;


    // far terrain (analytic tiles) under the materialized region, surface view
    void draw_far_terrain() ;


    SDL_Texture* tile_texture(int cx, int cy) ;


    // ---- planet ground: one chunked renderer for home region AND far planet ----
    // 32x32-cell chunks drawn with the same material sprites, tint, cliffs and
    // elevation lift everywhere, so the simulated region and the dormant
    // planet join without a seam. Region cells read the terrain grid (wear,
    // lenses); far cells read the analytic fields. LRU-bounded, a few chunk
    // builds per frame (a flat colour tile stands in until a chunk is ready).
    static constexpr int kCC = 32;

          // cells per chunk side
    static constexpr float kCS = 1.0f;

      // chunk texture px per map unit
    struct GChunk { SDL_Texture* t = nullptr; std::uint64_t used = 0, epoch = 0; int lens = -1; };


    std::unordered_map<std::uint64_t, GChunk> gchunks;


    void clear_gchunks() ;


    int gchunk_margin() const ;


    bool chunk_in_region(int wcx, int cy) const ;


    CellView planet_cell(int gx, int gy) const ;


    void build_gchunk(GChunk& g, int wcx, int cy) ;


    void draw_ground_chunks() ;


    // fog on planets: region keeps its per-cell soft fog; the dormant planet
    // outside it is unexplored by definition -> the same dark haze
    // ---- fog layer lifted with the terrain --------------------------------------
    // Fog is painted per cell at the SAME elevation lift (and cliff face) as
    // the ground, so the lit disk sits exactly where the people stand. Low
    // resolution (kFS px per map unit) + linear filtering = soft edges, cheap
    // rebuilds. Painter order top -> bottom with BLENDMODE_NONE: like the
    // ground, the frontmost cell owns every pixel it covers.
    static constexpr float kFS = 0.25f;


    static std::uint32_t fog_rgba(Fog f) ;


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


    void build_fog_ground() ;


    void draw_fog_ground() ;


    // planet: one small fog texture per ground chunk, rebuilt only when a
    // cell inside it changed tier (dirty set from the fog's visible lists)
    struct FChunk { SDL_Texture* t = nullptr; bool dirty = true; std::uint64_t used = 0; };


    std::unordered_map<std::uint64_t, FChunk> fchunks;


    std::vector<std::uint64_t> fog_prev_visible;


    std::size_t fog_explored_seen = 0;


    int fchunk_mode = -1;

 std::int64_t fchunk_tribe = 0;


    void clear_fchunks() ;


    std::uint64_t fchunk_key_of_cell(int gx, int gy) const ;


    // called under the lock after fog.update(): mark touched chunks dirty
    void fog_mark_dirty() ;


    void draw_fog_chunks() ;



    void draw_planet_fog() ;



    // mini-globe in the minimap corner, following the camera
    void update_mini_globe() ;


    void ui_mini_globe() ;



    void resize_world_rt() ;


    void init_textures() ;



    // ---- simulation pacing (the ONLY thing that advances the World) --------
    // Runs on sim_thread. A round is processed in ~6 ms wall-clock slices
    // (autonomous_cycle_step) so the UI thread can grab the world lock for
    // captures between slices instead of starving for a whole ~350 ms cycle.
    void sim_loop() ;


    // synchronous variant for the headless self-test
    void step_sim(double dt) ;



    // ---- terrain look from fields ---------------------------------------------
    MaterialId terrain_material(const CellView& c) const ;



    float cell_elev(int gx, int gy) const ;


    // (re)composite the whole map once; called when lens/day changes
    void build_ground() ;


    // fog: 1 px per cell, alpha by tier, linear-filtered when stretched
    void update_fog_tex() ;


    void draw_terrain() ;



    // ambient water: sparse glints drifting inside visible water cells,
    // phased by cell hash so the pattern is stable but shimmer moves.
    void draw_water_glints() ;



    void draw_cell_lens(const CellView& c, const SDL_FRect& dst) ;



    float elev_at(Vec2 p) const ;



    void emit(float x, float y, std::uint32_t c, int n) ;



    // enclosure: a ring of auto-tiled 2.5D wall segments around the footprint
    void draw_enclosure(const EntityView& e) ;



    void draw_glow(Vec2 p, std::uint32_t rgb, float scale) ;



    // ---- procedural character animation (sapients only) -------------------
    // RimWorld-style overlay pass in world_rt px on top of the creature
    // sprite: expressive pixel eyes driven by live neuromodulators (fear ->
    // wide jittering, dopamine/joy -> squint, exhaustion -> heavy lids), two
    // articulated hands posed by the freshest action verb, and floating "z"
    // motes for sleepers. Pure presentation — reads only c_anim viewer state.
    void draw_actor_anim(const EntityView& e, AnimState& an, float fx, float fy,
                         float sw, float sh, bool gliding, bool lying) ;



    void draw_entities(double dt) ;



    void draw_entity_lens(const EntityView& e, float fx, float fy, float sw, float sh) ;



    void draw_links() ;



    // DF-style intent lines: a dashed line from the selected (or hovered)
    // NPC to the entity its mind/brain is currently reaching for.
    static void dashed_seg(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col,
                           float dash = 7.f, float gap = 5.f, float thick = 1.5f) ;


    void draw_intents() ;



    // Phase 15: narrative callouts — short story lines floating over the spot
    // where they happened, rising and fading over ~4 s of render time.
    void draw_callouts() ;



    // Worn footpaths: client-side traffic heat rendered as packed-dirt
    // cells. Heavily traveled routes between water, groves and focal
    // points visibly wear smooth; unused cells fade back out.
    void draw_footpaths() ;


    // Warm halos over emergent gatherings — draw_glow already scales its
    // alpha by the day/night clock, so circles flare at dusk and night.
    void draw_gather_glow() ;


    void draw_night() ;



    // ---- UI -------------------------------------------------------------------------
    // ---- HUD: one slim bar. Time | day | tribe ......... Resonance Journal View Menu
    bool show_perf = false;


    std::string toast_msg;


    double toast_t = -10;


    void ui_topbar() ;


    // status messages: a toast above the bottom edge, fading after 4 s
    void ui_toast() ;



    // ---- live event ticker ("jump-to-drama"): last notable chronicle lines -
    // click flies the camera to the spot and selects the actor. Pure viewer
    // state read from cfeed; nothing here touches World.
    void ui_ticker() ;



    // ---- menus: Title -> New Game / Continue / Load ; in game Esc -> Pause --------
    enum Screen { kTitle = 0, kNewGame, kLoadGame, kSaveGame, kPause };


    struct SlotInfo { bool exists = false; std::string when; long long day = 0; std::filesystem::file_time_type mtime{}; };


    SlotInfo slot_info(int slot) const ;


    int newest_slot() const ;


    static const char* slot_name(int s) ;


    void open_menu(int scr) ;


    void close_menu() ;


    bool big_button(const char* label, bool enabled = true) ;


    void ui_menu() ;



    // ---- player agency: Resonance powers + Possess Avatar ----------------------
    // Every power writes AGENT KNOWLEDGE only (beliefs / memory engrams of
    // tribe minds); the minds decide what to do with it.
    std::vector<EntityId> tribe_minds() const ;


    // the `n` tribe minds closest to p (deterministic: distance, id)
    std::vector<EntityId> nearest_tribe(Vec2 p, std::size_t n) const ;


    void do_whisper(EntityId id) ;


    void whisper(EntityId id) ;


    // Instill Fear: every tribe mind that REMEMBERS the thing feels dread of it
    void do_instill_fear(EntityId target) ;


    void instill_fear(EntityId target) ;



    // ---- CL-1 Phase 4: gene pool persistence + god-tools ---------------------
    // All run under the world lock on the UI thread (via post) or the sim
    // thread (periodic harvest). The pool itself is read-only world input —
    // bless/spawn writes only affect FUTURE brains, never live state_hash.
    void ensure_gene_pool() ;


    void attach_gene_pool() ;


    void harvest_gene_pool() ;


    void save_gene_pool() ;


    // Global mutation pressure: rewrite `mutation_rate` on every living
    // genome-bearing organism (influx templates keep their own until they
    // too are swept — the slider is a god-tool, not a law).
    void apply_mutation_override() ;


    void do_bless_clone(EntityId id) ;


    void bless_clone(EntityId id) ;


    void do_punish(EntityId id) ;


    void punish(EntityId id) ;


    // ---- God-Hand sandbox tools ---------------------------------------------
    // Every command is a deterministic world mutation issued under the lock
    // via post(): same inputs -> same state_hash, headless runs never see
    // them (the toolbar only exists in the GUI).
    // deterministic icon tint for a material — name-substring palette with
    // a stable hash fallback. Render-state only, never persisted.
    std::uint32_t mat_icon_col(MaterialId m) const ;


    static AgentMind::MemorySnapshot god_snap(const World& w, const Entity& e) ;


    // Forge/raise a brain slot and surge neuromodulators (god tools may
    // allocate a slot — the command itself is the deterministic input).
    void god_mod_surge(EntityId id, int dopa, int sero, int cortisol,
                       int norepi, int joy) ;


    void do_cupid(EntityId a, EntityId b) ;


    void cupid(EntityId a, EntityId b) ;


    void do_instill_panic(EntityId id) ;


    void instill_panic(EntityId id) ;


    // deterministic pseudo-random ring offset for scatter spawns
    static Vec2 god_ring(Vec2 p, int k, int cell, int rad) ;


    // fx bodies (payment already taken by the do_* wrapper / bounty combo)
    void rain_fx(Vec2 p) ;


    void feast_fx(Vec2 p) ;


    void do_summon_rain(Vec2 p) ;


    void summon_rain(Vec2 p) ;


    void do_summon_feast(Vec2 p) ;


    void summon_feast(Vec2 p) ;


    // Bounty: rain + feast in one divine gesture (single payment)
    void do_bounty(Vec2 p) ;


    void bounty(Vec2 p) ;


    // Miracle: localized awe impulse. Pure physiology — sero/dopa/joy surge
    // plus cortisol relief for every sapient mind within 220 units, and a
    // strong positive engram toward the focal entity (picked target, else
    // nearest living thing to the tile). Whether they revere, fight over,
    // or ignore the spot is left entirely to the SNN/ STDP.
    void do_miracle(Vec2 p, EntityId picked) ;


    void miracle(Vec2 p, EntityId picked) ;


    // Relic: drop a rare obsidian artifact — no props, no nutrition, no
    // script. Agents perceive it, form engrams, and the SNN decides whether
    // anyone claims, hoards, gifts or fights over it.
    void do_relic(Vec2 p) ;


    void relic(Vec2 p) ;


    // Tempest: localized storm shock — a moisture pulse plus thirst/chill
    // pressure and an acute stress spike for every mind in the radius.
    // Deterministic integers only; whether the tribe bands together via
    // focal_buffer co-regulation or fractures is left to the SNN.
    void do_tempest(Vec2 p) ;


    void tempest(Vec2 p) ;


    void do_summon_predator(Vec2 p) ;


    void summon_predator(Vec2 p) ;


    void do_place_beacon(Vec2 p) ;


    void place_beacon(Vec2 p) ;


    void do_place_blueprint(Vec2 c, std::int64_t r) ;


    void place_blueprint(Vec2 c, std::int64_t r) ;


    void do_avatar_act(const std::string& verb) ;


    void avatar_act(const std::string& verb) ;


    void do_avatar_move(Vec2 dest) ;


    void avatar_move(Vec2 dest) ;


    void set_possess(EntityId id) ;


    void do_set_possess(EntityId id) ;


    void open_word(std::int64_t lang, const std::string& word) ;


    // ---- action bar (bottom-left): powers, or the possessed body's verbs ----
    void ui_player() ;



    // caches read under the world lock (signs, legends, avatar vocabulary)
    void build_patron_caches() ;



    // ---- successor choice after the avatar's death ---------------------------
    void ui_successor() ;



    // ---- Field Journal: Rosetta lexicon, chronicle, legends, notes ------------
    void ui_journal() ;


    void ui_word_editor() ;



    // ---- overlays: speech bubbles with live translation, signs, drag ---------
    void draw_overlays() ;



    void update_minimap() ;


    void ui_minimap() ;



    // ---- inspector: built under the lock, drawn from the cache ---------------
    void build_inspector() ;


    // ---- Continuous Chronicle chapters (Biên Niên Sử) -------------------------
    // Long-form LLM chapters in chronological order; click flies the camera
    // to the chapter's event centroid, Export copies the .md path. Shared by
    // the live Chronicle window and the codex "Chronicle" tab.
    void ui_chapters() ;




    void ui_inspector() ;



    void ui_atlas() ;



    // Chronicle & Rosetta codex: the engine's codex text + the player's own
    // decipherment notes (saved beside the binary; never written to World)
    void load_rosetta() ;


    void save_rosetta() ;


    void build_codex() ;


    void ui_codex() ;



    // ---- input ------------------------------------------------------------------------
    Vec2 screen_to_world(float mx, float my) const ;


    void pick(float mx, float my) ;



    void handle(const SDL_Event& e, bool& quit) ;



    void pan_keys(double dt) ;



    // ---- CL-1 Phase 3: LLM chronicle scan/drain (UI thread, world lock held)
    // Salient causal events are summarised and handed to NarrativeHost; the
    // worker narrates them off-thread and llm_drain() re-injects the text as
    // tagged NarrativeLine{llm=true} rows in the same ChronicleFeed.
    static bool llm_skip_verb(const std::string& a) ;


    // ---- Continuous Chronicle: chapter batching ------------------------------
    // System prompt tuned for a small instruct model: strict truthfulness to
    // the supplied facts, epic continuity with the previous passage, and a
    // rigid "title line + prose" output format.
    static const char* kChapterSys() ;


    static std::string chron_trim(const std::string& s) ;


    // Last ~3 sentences of a chapter body, for prompt continuity.
    static std::string chron_tail_of(const std::string& body) ;


    void chron_load(const World& w) ;


    void chron_append(const ChronicleChapter& c) ;


    // Window check + catastrophic flush; called under the world lock.
    // EVERY action event is a fact now — the chapter prompt gets up to
    // ~4800 chars of them and the .md keeps a single-line record of the
    // same, so the chronicle is a complete account, not just milestones.
    void chron_tick(const World& w) ;


    void llm_ch_drain() ;



    // ================= open-ended invention: material cataloger ================
    static std::string mat_clean(std::string s, std::size_t cap) ;


    static std::string mat_slug_of(const std::string& name) ;


    // Parse saves/discovered_materials.txt back into mat_entries (idempotent
    // reload — the file is the catalog's source of truth across sessions).
    void mat_load() ;


    bool mat_known(std::uint64_t synth_id) const ;


    void mat_scan(const World& w) ;


    void llm_mat_drain() ;



    // ---- tribal mythology: scan + drain --------------------------------------
    // Dedup key mirrors doctrine_key() (Cognition.cpp, anonymous namespace):
    // "kind:prop:thr:id:text:verb" — rebuilt field-by-field here.
    static std::string myth_dkey(const AgentMind::DoctrineSlot& d) ;


    static std::string myth_focus_text(const World& w,
                                       const AgentMind::DoctrineSlot& d) ;


    void myth_load() ;


    void myth_scan(const World& w) ;


    void llm_myth_drain() ;



    void llm_scan(const World& w) ;


    void llm_drain() ;



    // one frame: snapshot (const, under the world lock) -> draw -> UI
    void frame(double dt) ;



    // ---- 3D voxel "HD-2D" view (client-only; never touches World) ---------
    struct V3Vert { float x, y, z; std::uint8_t r, g, b, a; };
    struct V3Chunk { std::vector<V3Vert> v; std::vector<int> i; double t = 0; };
    bool view3d = false;
    float v3_x = 0, v3_z = 0, v3_y = 220;       // camera position (world units; y = up)
    float v3_yaw = 0.f, v3_pitch = -0.85f;     // radians; pitch < 0 looks down
    float v3_speed = 460.f;
    bool v3_look = false;                      // right-mouse drag look active
    float v3_saved_zoom = 0.9f;
    std::unordered_map<std::uint64_t, V3Chunk> v3_chunks;
    void toggle_view3d() ;
    void draw_3d(double dt) ;
    Vec2 v3_pick(float mx, float my, EntityId* hit) ;
    float v3_height(float x, float z) const ;
    void v3_build_chunk(int ccx, int ccz) ;

};

}  // namespace simlab_viewer
