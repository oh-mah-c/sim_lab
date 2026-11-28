#include "viewer.hpp"

namespace simlab_viewer {


    void Viewer::post(std::function<void()> f) { cmds.push_back(std::move(f)); }


    float Viewer::ux(float x) const { return x * kPix; }



    Viewer::Viewer(World& world) : w(world) {
        dstate.curiosity = true; dstate.concurrent = true;
        dstate.brain_control = true;      // CL-1 is the sapient controller
        dstate.parallel_think = true;     // 100% neural + parallel think
        if (std::getenv("SIMLAB_NO_BRAIN")) dstate.brain_control = false;
        if (std::getenv("SIMLAB_NO_PARALLEL")) dstate.parallel_think = false;
        composer.load(data_dir + "/narrative_templates.txt");
    }


    Viewer::~Viewer() {
        // Wait out an in-flight model load so ~NarrativeHost never races it.
        if (llm_starter.joinable()) llm_starter.join();
    }



    std::int64_t Viewer::tribe_lang() const {
        return pw.tribe_lang.empty() ? 0 : pw.tribe_lang[std::size_t(std::clamp<int>(tribe_view, 0, int(pw.tribe_lang.size()) - 1))];
    }


    // reset every viewer-side cache after the World was replaced
    void Viewer::reset_view() {
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
        syms.clear(); beams.clear(); c_gather.clear(); gather_of.clear();
        c_held.clear(); c_wield_col.clear(); path_heat.clear();
        path_tick = 0; storm_t0 = -100.0;
        cupid_a = kNoEntity; god_open = true;
        mind_ask_key = 0; mind_ask_id = kNoEntity; mind_reply.clear();
        mind_ask_busy = false; mind_ask_wait = 0;
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
    void Viewer::new_world(const PrimordialParams& p) {
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
    void Viewer::derive_tribes() {
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


    std::string Viewer::slot_path(int slot) const {
        return pref_dir + (slot <= 0 ? std::string("autosave") : "slot" + std::to_string(slot)) + ".sav";
    }


    void Viewer::save_slot(int slot) {
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


    bool Viewer::load_slot(int slot) {
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



    void Viewer::resize_world_rt() {
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


    void Viewer::init_textures() {
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
    void Viewer::sim_loop() {
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
    void Viewer::step_sim(double dt) {
        static const double kRate[] = {0, 2, 10, 40, 200};
        sim_accum += dt * kRate[std::clamp(speed, 0, 4)];
        while (sim_accum >= 1.0) { autonomous_cycle(w, &dstate); sim_accum -= 1.0; }
    }



    // ---- CL-1 Phase 4: gene pool persistence + god-tools ---------------------
    // All run under the world lock on the UI thread (via post) or the sim
    // thread (periodic harvest). The pool itself is read-only world input —
    // bless/spawn writes only affect FUTURE brains, never live state_hash.
    void Viewer::ensure_gene_pool() {                 // caller holds mx
        if (gene_pool_loaded) return;
        gene_pool_loaded = true;
        gene_pool.load("saves/gene_pool.bin", w.props);
    }


    void Viewer::attach_gene_pool() {                 // caller holds mx, w fresh
        ensure_gene_pool();
        w.gene_pool = &gene_pool;
    }


    void Viewer::harvest_gene_pool() {                // caller holds mx
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


    void Viewer::save_gene_pool() {                   // caller holds mx
        harvest_gene_pool();
        gene_pool.save("saves/gene_pool.bin", w.props);
    }



    // ---- input ------------------------------------------------------------------------
    Vec2 Viewer::screen_to_world(float mx, float my) const {   // world_rt px -> map units (elevation 0)
        const float wx = (mx - cam.sw * 0.5f) / cam.zoom + cam.cx;
        const float wy = (my - cam.sh * 0.5f) / cam.zoom / Camera::kY + cam.cy;
        return Vec2{std::int32_t(wx), std::int32_t(wy)};
    }


    void Viewer::pick(float mx, float my) {
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



    void Viewer::handle(const SDL_Event& e, bool& quit) {
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
            if (tool == kToolCupid && selected != kNoEntity) {
                if (cupid_a == kNoEntity || cupid_a == selected) cupid_a = selected;
                else { const EntityId b = selected; const EntityId a2 = cupid_a;
                       cupid_a = kNoEntity; cupid(a2, b); }
            }
            if (tool == kToolPanic && selected != kNoEntity) { instill_panic(selected); tool = kToolNone; }
            if (tool == kToolRain) { summon_rain(wp); tool = kToolNone; }
            if (tool == kToolFeast) { summon_feast(wp); tool = kToolNone; }
            if (tool == kToolPredator) { summon_predator(wp); tool = kToolNone; }
            if (tool == kToolMiracle) {
                miracle(wp, selected);   // focal resolved under the lock
                beams.push_back({cam.sx(float(wp.x)),
                                 cam.sy(float(wp.y), elev_at(wp)), float(time_s)});
                tool = kToolNone;
            }
            if (tool == kToolBounty) { bounty(wp); tool = kToolNone; }
            if (tool == kToolRelic) { relic(wp); tool = kToolNone; }
            if (tool == kToolTempest) { tempest(wp); tool = kToolNone; }
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
            if (k == SDLK_P) {                 // possess selected / release
                if (avatar != kNoEntity) set_possess(kNoEntity);
                else if (selected != kNoEntity) set_possess(selected);
                return;
            }
            if (k == SDLK_E && avatar != kNoEntity) { avatar_act("consume"); return; }
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



    void Viewer::pan_keys(double dt) {
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
    void Viewer::frame(double dt) {
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
                        else if (a == "reproduce" || a == "mate")                    { label = "Lovin'"; col = IM_COL32(255, 140, 210, 255); }
                        else if (a == "trade" || a == "gift" || a == "share")        { label = "Share";  col = IM_COL32(225, 200, 100, 255); }
                        else if (a == "steal")                                       { label = "Steal!"; col = IM_COL32(200, 90, 160, 255); }
                        else if (a == "treaty" || a == "parley" || a == "diplomatic_parley") { label = "Talk"; col = IM_COL32(130, 225, 225, 255); }
                        else if (a == "wield" || a == "don" || a == "doff" || a == "coat")  { label = "Gear";   col = IM_COL32(170, 160, 190, 255); }
                        else { label = a.substr(0, 10); }
                        if (ev.failed && (a == "reproduce" || a == "mate")) {
                            // rejected advance: cooldown / not in the mood
                            label = "Cooldown!"; col = IM_COL32(205, 185, 225, 255);
                        } else if (ev.failed) { label += "!"; col = IM_COL32(150, 155, 165, 255); }
                        {
                            ThoughtBadge& b = c_badge[actor];
                            if (b.rank < 3) { b.label = label; b.col = col;
                                b.until = c_tick + ((a == "reproduce" || a == "mate") ? 50 : 30);
                                b.rank = 2; }
                        }
                        // post-coital glow: the partner wears Lovin' too —
                        // or "Not interested!" when the advance was rejected
                        if ((a == "reproduce" || a == "mate") && target != kNoEntity) {
                            ThoughtBadge& tb = c_badge[target];
                            if (tb.rank < 3) {
                                tb.label = ev.failed ? "Not interested!" : "Lovin'";
                                tb.col = ev.failed ? IM_COL32(170, 165, 185, 255)
                                                   : IM_COL32(255, 140, 210, 255);
                                tb.until = c_tick + (ev.failed ? 40 : 50); tb.rank = 2;
                            }
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
                                } else if ((a == "reproduce" || a == "mate") && !ev.failed) {
                                    for (int k = 0; k < 3 && hearts.size() < 60; ++k)
                                        hearts.push_back({ax + float(k - 1) * 5.f, ay - 10.f * cam.zoom, float(k) * 0.12f});
                                    // nervous sweat — even love is sweaty
                                    for (int k = 0; k < 2 && parts.size() < 600; ++k)
                                        parts.push_back({ax + float(k * 2 - 1) * 6.f, ay - 12.f * cam.zoom,
                                            float(k - 1) * 10.f, -30.f, 0.5f, 0xFFBFE8FF});
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
                        // ---- shared drama reads: conspecific engram scan ----
                        // fresh_neg/stale_grudge feed badges; grudge_id/pos
                        // feed tantrum facing + bubbles; mate_id feeds the
                        // affectionate idle line. Read-only, never mutates.
                        int fresh_neg = 0, stale_grudge = 0;
                        std::int64_t worst_neg = 0;
                        EntityId grudge_id = kNoEntity;
                        Vec2 grudge_pos{};
                        EntityId mate_id = kNoEntity;
                        std::int64_t mate_d2 = 260LL * 260;
                        for (const auto& [kid, s] : m->memory) {
                            if (kid == e.id) continue;
                            const bool conspec = s.kind_name == e.kind ||
                                                 s.kind_name == e.species;
                            if (!conspec) continue;
                            const std::int64_t ch = s.engram.emotional_charge;
                            if (ch <= -450) {
                                const std::uint64_t age =
                                    c_tick > s.engram.last_rehearsal
                                        ? c_tick - s.engram.last_rehearsal : 0;
                                if (age < 120) {
                                    ++fresh_neg;
                                    if (ch < worst_neg) {
                                        worst_neg = ch; grudge_id = kid;
                                        grudge_pos = s.pos;
                                    }
                                } else if (ch <= -600 && age < 3000) ++stale_grudge;
                            } else if (ch >= 650 && mate_id == kNoEntity) {
                                const std::int64_t dx =
                                    std::int64_t(s.pos.x) - e.pos.x;
                                const std::int64_t dy =
                                    std::int64_t(s.pos.y) - e.pos.y;
                                const std::int64_t d2 = dx * dx + dy * dy;
                                const Entity* ge = w.find(kid);
                                if (ge && ge->alive && d2 < mate_d2) {
                                    mate_id = kid; mate_d2 = d2;
                                }
                            }
                        }
                        // crowd read: >=2 conspecifics within 128 units while
                        // hungry+stressed — feeds Secretive badge AND bubble
                        bool crowded = false;
                        if (bs && bs->vital.drive_hunger > 600 &&
                            (bs->vital.mod[kBModCortisol] > 350 ||
                             m->neuro.panic > 250)) {
                            int cn = 0;
                            for (const EntityView& o : snap.entities) {
                                if (cn >= 2) break;
                                if (o.id == e.id || !o.alive ||
                                    o.fog != Fog::Visible) continue;
                                if (o.kind != e.kind && o.species != e.species)
                                    continue;
                                const std::int64_t dx =
                                    std::int64_t(o.pos.x) - e.pos.x;
                                const std::int64_t dy =
                                    std::int64_t(o.pos.y) - e.pos.y;
                                if (dx * dx + dy * dy < 128 * 128) ++cn;
                            }
                            crowded = cn >= 2;
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
                                        // dynamic status tag: peak neuromod
                                        // deviation + live focus (top engram,
                                        // bonded mate, grudge, dominant need).
                                        // No canned lines — pure telemetry.
                                        const std::int64_t hunger = bs ? bs->vital.drive_hunger : 0;
                                        const std::int64_t thirst = bs ? bs->vital.drive_thirst : 0;
                                        const std::int64_t fatigue = bs ? bs->vital.drive_fatigue : 0;
                                        std::string bub, focus;
                                        if (mate_id != kNoEntity &&
                                            m->neuro.panic < 300 && m->neuro.rage < 400)
                                            focus = "#" +
                                                std::to_string(mate_id % 100000) + " <3";
                                        else if (fresh_neg >= 1 && grudge_id != kNoEntity)
                                            focus = "#" +
                                                std::to_string(grudge_id % 100000) + "!";
                                        else {
                                            // strongest |charge| engram wins
                                            std::int64_t best = 0;
                                            std::string bk;
                                            for (const auto& [kid, s] : m->memory) {
                                                const std::int64_t ch =
                                                    s.engram.emotional_charge;
                                                if (std::llabs(ch) > std::llabs(best) &&
                                                    s.engram.clarity > 150) {
                                                    best = ch; bk = s.kind_name;
                                                }
                                            }
                                            if (!bk.empty())
                                                focus = bk.size() > 14 ? bk.substr(0, 14) : bk;
                                            else if (thirst > 500) focus = "H2O";
                                            else if (hunger > 500) focus = "FOOD";
                                            else if (fatigue > 700) focus = "REST";
                                            else focus = "...";
                                        }
                                        if (bs) {
                                            static const char* kModN[kBModCount] = {
                                                "DOPA", "SERO", "NEPI", "ACHI",
                                                "GABA", "GLUT", "CORT"};
                                            int bi = 0, bd = 0;
                                            for (int i = 0; i < kBModCount; ++i) {
                                                const int d = std::abs(
                                                    int(bs->vital.mod[i]) - 500);
                                                if (d > bd) { bd = d; bi = i; }
                                            }
                                            bub = "[" + std::string(kModN[bi]) +
                                                  (int(bs->vital.mod[bi]) >= 500 ? "↑" : "↓") +
                                                  "] " + focus;
                                        } else if (m->neuro.panic > 500) bub = "[PANIC] " + focus;
                                        else if (m->neuro.rage > 500) bub = "[RAGE] " + focus;
                                        else if (m->neuro.grief > 400) bub = "[GRIEF] " + focus;
                                        else bub = "[--] " + focus;
                                        b.label = bub; b.col = IM_COL32(150, 156, 172, 255);
                                        b.until = c_tick + 30; b.rank = 0;
                                    }
                                }
                            }
                        }
                        // ---- drama badges: jealousy signature, fresh grudges,
                        // betrayal wounds, secretive hunger+stress — reuses
                        // the shared engram/crowd reads above (never mutates).
                        {
                            const char* dl = nullptr; ImU32 dc = 0; int dr = 0;
                            if (fresh_neg >= 2) {
                                dl = "Jealous!"; dc = IM_COL32(255, 120, 60, 255); dr = 2;
                            } else if (fresh_neg == 1 && worst_neg <= -680) {
                                dl = "Grudge"; dc = IM_COL32(200, 60, 60, 255); dr = 2;
                            } else if (fresh_neg == 1) {
                                dl = "Betrayed!"; dc = IM_COL32(255, 110, 170, 255); dr = 2;
                            } else if (stale_grudge > 0) {
                                dl = "Grudge"; dc = IM_COL32(160, 70, 70, 255); dr = 1;
                            } else if (crowded) {
                                dl = "Secretive"; dc = IM_COL32(150, 170, 200, 255); dr = 1;
                            }
                            if (dl) {
                                ThoughtBadge& b = c_badge[e.id];
                                if (b.rank <= dr || b.until <= c_tick) {
                                    b.label = dl; b.col = dc;
                                    b.until = c_tick + 40; b.rank = dr;
                                }
                            }
                            // ---- tantrum: fresh grudge + hot temper -> stomp
                            // bounce, anger puffs, and a glare at the rival.
                            // Purely cosmetic state in c_anim/c_badge.
                            if (fresh_neg >= 1 && grudge_id != kNoEntity &&
                                (m->neuro.rage > 400 || m->neuro.panic > 400)) {
                                AnimState& an = c_anim[e.id];
                                an.tantrum_until = float(time_s) + 0.8f;
                                if (parts.size() < 600) {
                                    const float fx = cam.sx(float(e.pos.x));
                                    const float fy = cam.sy(float(e.pos.y),
                                                             elev_at(e.pos));
                                    for (int k = 0; k < 3; ++k)
                                        parts.push_back({fx + float(k - 1) * 7.f,
                                            fy - 18.f * cam.zoom,
                                            float(k - 1) * 22.f, -30.f - float(k) * 8.f,
                                            0.55f,
                                            k == 1 ? 0xFFFF5030 : 0xFFFFC040});
                                    // comic glyph pop (! ? ~) — deterministic pick
                                    static const char kGly[] = {'!', '?', '~', '#'};
                                    if (syms.size() < 48)
                                        syms.push_back({fx + float((c_tick / 9) % 5 - 2) * 8.f,
                                            fy - 26.f * cam.zoom, float(time_s),
                                            kGly[(size_t(e.id) + size_t(c_tick)) % 4]});
                                }
                                IntentLink& il = c_intent[e.id];
                                il.target = grudge_id; il.pos = grudge_pos;
                                il.label = "argument"; il.until = c_tick + 70;
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
                            // grief -> mourning posture (droop + tears); hot
                            // temper near a grudge -> argument lean-in; high
                            // dopamine + satiation -> proud chest-puff.
                            an.mourning = m->neuro.grief > 300;
                            an.argue = (fresh_neg >= 1 &&
                                        (m->neuro.rage > 400 || m->neuro.panic > 400))
                                       ? 1.f : 0.f;
                            an.proud = bs && bs->vital.mod[kBModDopa] > 650 &&
                                       bs->vital.drive_hunger < 400 ? 1.f : 0.f;
                            if (an.mourning && frames % 22 == e.id % 22 &&
                                parts.size() < 600) {
                                const float mx = cam.sx(float(e.pos.x));
                                const float my = cam.sy(float(e.pos.y), elev_at(e.pos));
                                parts.push_back({mx + 4.f, my - 13.f * cam.zoom,
                                    0.f, 12.f, 0.7f, 0xFF9FC8F0});
                            }
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
                    // ---- emergent gatherings --------------------------------
                    // >=3 living minds intent-fixed on the SAME focal entity
                    // while clustered within ~280 units: shared ground aura,
                    // synced idle rhythm, drifting incense sparkles. Purely a
                    // read of c_intent + world positions — nothing is written
                    // to the simulation.
                    c_gather.clear(); gather_of.clear();
                    {
                        std::map<EntityId, std::vector<EntityId>> by_focal;
                        for (const auto& [mid, il] : c_intent)
                            if (il.until > c_tick && il.target != kNoEntity)
                                by_focal[il.target].push_back(mid);
                        for (auto& [focal, mem] : by_focal) {
                            if (mem.size() < 3) continue;
                            std::int64_t cx = 0, cy = 0; int n = 0;
                            for (EntityId mid : mem) {
                                const Entity* me = w.find(mid);
                                if (!me || !me->alive) continue;
                                cx += me->pos.x; cy += me->pos.y; ++n;
                            }
                            if (n < 3) continue;
                            cx /= n; cy /= n;
                            bool tight = true;
                            for (EntityId mid : mem) {
                                const Entity* me = w.find(mid);
                                if (!me || !me->alive) continue;
                                const std::int64_t dx = me->pos.x - cx;
                                const std::int64_t dy = me->pos.y - cy;
                                if (dx * dx + dy * dy > 280LL * 280) {
                                    tight = false; break;
                                }
                            }
                            if (!tight) continue;
                            Gather g; g.focal = focal;
                            g.pos = Vec2{std::int32_t(cx), std::int32_t(cy)};
                            for (EntityId mid : mem) {
                                const Entity* me = w.find(mid);
                                if (me && me->alive) {
                                    g.members.push_back(mid);
                                    gather_of[mid] = focal;
                                }
                            }
                            if (g.members.size() < 3) continue;
                            c_gather.push_back(std::move(g));
                            if (parts.size() < 550) {
                                const Vec2 gp{std::int32_t(cx), std::int32_t(cy)};
                                const float gx = cam.sx(float(gp.x));
                                const float gy = cam.sy(float(gp.y), elev_at(gp));
                                parts.push_back({gx + float((c_tick / 4) % 9 - 4) * 9.f,
                                    gy - 4.f * cam.zoom,
                                    0.f, -11.f - float(c_tick % 5) * 3.f, 1.1f,
                                    0xFFFFE9A0});
                            }
                        }
                    }
                    // ---- carried items + wielded gear (icon state) --------
                    // possessed_by is World Truth; resolved under the lock,
                    // drawn as a small material-tinted chip in the hand.
                    if (frames % 20 == 4) {
                        c_held.clear(); c_wield_col.clear();
                        const PropId p_poss =
                            w.props.find_name("possessed_by")
                                ? w.props.find_name("possessed_by")->id
                                : kNoProp;
                        if (p_poss != kNoProp) {
                            for (const auto& ep : w.entities) {
                                const Entity& it = *ep;
                                if (!it.alive) continue;
                                const std::int64_t holder =
                                    w.prop_int(it, p_poss, 0);
                                if (holder <= 0) continue;
                                const Entity* he =
                                    w.find(static_cast<EntityId>(holder));
                                if (!he || !he->alive) continue;
                                c_held[static_cast<EntityId>(holder)]
                                    .push_back({it.id, mat_icon_col(it.material)});
                            }
                        }
                        for (const EntityView& e : snap.entities)
                            if (e.w_mat != kNoMaterial && e.alive &&
                                e.fog == Fog::Visible)
                                c_wield_col[e.id] = mat_icon_col(e.w_mat);
                    }
                }
                // ---- footpath wear: client-side traffic heat ---------------
                // Every pass adds +1 to the cell each visible entity stands
                // on; a slow halving lets unused paths fade back to grass.
                // Pure render state — never touches World.
                if (frames - path_tick >= 6) {
                    path_tick = frames;
                    const int cell = w.terrain.cell > 0 ? w.terrain.cell : 16;
                    for (const EntityView& e : snap.entities) {
                        if (!e.alive || e.fog == Fog::Shrouded) continue;
                        const std::int64_t cx = e.pos.x / cell;
                        const std::int64_t cy = e.pos.y / cell;
                        const std::uint64_t key =
                            (std::uint64_t(std::uint32_t(cx)) << 32) |
                            std::uint64_t(std::uint32_t(cy));
                        auto& h = path_heat[key];
                        if (h < 60000) ++h;
                    }
                }
                if (frames % 900 == 0 && !path_heat.empty()) {
                    for (auto it = path_heat.begin(); it != path_heat.end();) {
                        it->second = std::uint16_t(it->second / 2);
                        if (it->second < 6) it = path_heat.erase(it);
                        else ++it;
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
                else { draw_terrain(); draw_water_glints(); draw_footpaths(); }
            }
            t_ter = ms(t); t = clk::now();
            if (!(skip & 2)) draw_entities(dt);
            draw_links();
            draw_intents();
            draw_callouts();
            if (!(skip & 4)) { draw_gather_glow(); draw_night(); }
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
            // mind-reader reply: poll the worker every frame (mutexed,
            // non-blocking) so it completes even with the inspector shut
            if (mind_ask_busy && mind_ask_key) {
                std::string out;
                if (llm_host.poll(mind_ask_key, out)) {
                    mind_reply = out; mind_ask_busy = false;
                } else if (++mind_ask_wait > 3600) {   // ~60 s: give up
                    mind_ask_busy = false;
                    status_msg = "Mind-reader timed out";
                }
            }
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

}  // namespace simlab_viewer
