// Phase 13/14 — sunnyworld_viewer: "Sunnyworld - Primordial Sandbox".
// Entry point only — the Viewer class is split across viewer.hpp
// and viewer_{core,planet,entities,hud,tools,mind}.cpp.

#include "viewer.hpp"

using namespace simlab_viewer;

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
