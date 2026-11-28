#include "viewer.hpp"

namespace simlab_viewer {


    void Viewer::ui_topbar() {
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
            ImGui::Separator();
            if (ImGui::MenuItem(view3d ? "2D top-down view" : "3D voxel view", "V")) toggle_view3d();
            if (view3d) ImGui::TextDisabled("WASD fly · RMB look · Space/C up/down · wheel speed");
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
    void Viewer::ui_toast() {
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
    void Viewer::ui_ticker() {
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


    Viewer::SlotInfo Viewer::slot_info(int slot) const {
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


    int Viewer::newest_slot() const {          // most recent save of any kind, -1 none
        int best = -1; std::filesystem::file_time_type bt{};
        for (int s = 0; s <= 5; ++s) {
            const SlotInfo si = slot_info(s);
            if (si.exists && (best < 0 || si.mtime > bt)) { best = s; bt = si.mtime; }
        }
        return best;
    }


    const char* Viewer::slot_name(int s) { static const char* k[] = {"Autosave", "Slot 1", "Slot 2", "Slot 3", "Slot 4", "Slot 5"}; return k[std::clamp(s, 0, 5)]; }


    void Viewer::open_menu(int scr) { in_menu = true; screen = scr; speed = 0; speed_atomic = 0; ui_dirty = true; }


    void Viewer::close_menu() { in_menu = false; if (!speed) { speed = 1; speed_atomic = 1; } }


    bool Viewer::big_button(const char* label, bool enabled ) {
        ImGui::BeginDisabled(!enabled);
        const bool r = ImGui::Button(label, ImVec2(-1, 44));
        ImGui::EndDisabled();
        return r && enabled;
    }


    void Viewer::ui_menu() {
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


    void Viewer::open_word(std::int64_t lang, const std::string& word) {
        edit_lang = lang; edit_word = word; open_word_popup = true;
        const auto& jw = journal.lexicon[lang][word];
        std::snprintf(edit_buf, sizeof edit_buf, "%s", jw.guess.c_str());
        edit_tag = int(jw.tag);
    }



    // caches read under the world lock (signs, legends, avatar vocabulary)
    void Viewer::build_patron_caches() {
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
    void Viewer::ui_successor() {
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
    void Viewer::ui_journal() {
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


    void Viewer::ui_word_editor() {
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



    // ---- inspector: built under the lock, drawn from the cache ---------------
    void Viewer::build_inspector() {
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
            // ---- mind-reader digest: everything Qwen needs to voice the
            // NPC in first person (built under lock, consumed by the
            // inspector's Ask button — display-only, never serialized)
            {
                std::string d;
                d += "You are " + sid(e->id) + ", a " + e->kind_name.str() +
                     " in Sunnyworld.\n";
                d += "Current state: " + insp.intent + ".\n";
                d += "Neuro: panic " + std::to_string(m->neuro.panic) +
                     ", rage " + std::to_string(m->neuro.rage) +
                     ", grief " + std::to_string(m->neuro.grief) + ".\n";
                const BrainSlot* bs = nullptr;
                if (w.brains_) {
                    auto sit = w.brains_->slots.find(e->id);
                    if (sit != w.brains_->slots.end()) bs = &sit->second;
                }
                if (bs) {
                    d += "Neuromods: dopa " + std::to_string(bs->vital.mod[kBModDopa]) +
                         ", sero " + std::to_string(bs->vital.mod[kBModSero]) +
                         ", norepi " + std::to_string(bs->vital.mod[kBModNorepi]) +
                         ", cortisol " + std::to_string(bs->vital.mod[kBModCortisol]) + ".\n";
                    d += "Drives: hunger " + std::to_string(bs->vital.drive_hunger) +
                         ", thirst " + std::to_string(bs->vital.drive_thirst) +
                         ", fatigue " + std::to_string(bs->vital.drive_fatigue) + ".\n";
                }
                if (!insp.memories.empty()) {
                    d += "Memories: ";
                    for (std::size_t i = 0; i < insp.memories.size() && i < 6; ++i) {
                        if (i) d += "; ";
                        d += insp.memories[i];
                    }
                    d += ".\n";
                }
                if (!insp.ties.empty()) {
                    d += "Relationships: ";
                    for (std::size_t i = 0; i < insp.ties.size() && i < 4; ++i) {
                        if (i) d += "; ";
                        d += insp.ties[i];
                    }
                    d += ".\n";
                }
                if (!insp.life.empty()) {
                    d += "Life: ";
                    for (std::size_t i = 0; i < insp.life.size() && i < 4; ++i) {
                        if (i) d += "; ";
                        d += insp.life[i];
                    }
                    d += ".\n";
                }
                insp.mind_digest = std::move(d);
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
    void Viewer::ui_chapters() {
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




    void Viewer::ui_inspector() {
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
        // ---- Ask / Read Mind: async Qwen monologue (never blocks render) --
        if (insp.has_mind) {
            ImGui::Separator();
            if (mind_ask_busy) {
                ImGui::TextDisabled("Qwen is reading its mind...");
            } else if (ImGui::Button("Ask / Read Mind", ImVec2(-1, 0))) {
                if (llm_host.active() && !insp.mind_digest.empty()) {
                    static const char* kMindSys =
                        "You are reading the mind of a creature in a "
                        "deterministic ecosystem simulation. Using ONLY the "
                        "facts given, write a short first-person monologue "
                        "(2-4 sentences) in Vietnamese as the creature "
                        "itself — what it fears, desires, remembers, or "
                        "resent. No narration, no quotes.";
                    mind_ask_key = ++llm_key_seq;
                    mind_ask_id = selected;
                    mind_reply.clear();
                    mind_ask_wait = 0;
                    llm_host.submit_prompt(mind_ask_key, kMindSys, insp.mind_digest);
                    mind_ask_busy = true;
                } else {
                    status_msg = "Mind-reader needs the Qwen model "
                                 "(SIMLAB_LLM_MODEL) and a sapient target";
                }
            }
            if (!mind_reply.empty()) {
                ImGui::Separator();
                ImGui::TextWrapped("%s", mind_reply.c_str());
            }
        }
        ImGui::End();
    }



    void Viewer::ui_atlas() {
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
    void Viewer::load_rosetta() {
        std::ifstream f("viewer_rosetta_notes.txt"); std::string l;
        while (std::getline(f, l)) { const auto t = l.find('\t'); if (t != std::string::npos) rosetta[l.substr(0, t)] = l.substr(t + 1); }
    }


    void Viewer::save_rosetta() {
        std::ofstream f("viewer_rosetta_notes.txt");
        for (const auto& [k, v] : rosetta) if (!v.empty()) f << k << '\t' << v << '\n';
    }


    void Viewer::build_codex() {
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


    void Viewer::ui_codex() {
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

}  // namespace simlab_viewer
