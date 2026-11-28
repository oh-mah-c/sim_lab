#include "viewer.hpp"

namespace simlab_viewer {


    // Kick the Qwen chronicler off a background thread (model load takes
    // seconds; the title menu must stay responsive). Safe no-op when the
    // llama build or GGUF model is absent.
    void Viewer::start_llm() {
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
    std::string Viewer::llm_resolve(const std::string& rel) {
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



    // ---- CL-1 Phase 3: LLM chronicle scan/drain (UI thread, world lock held)
    // Salient causal events are summarised and handed to NarrativeHost; the
    // worker narrates them off-thread and llm_drain() re-injects the text as
    // tagged NarrativeLine{llm=true} rows in the same ChronicleFeed.
    bool Viewer::llm_skip_verb(const std::string& a) {
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
    const char* Viewer::kChapterSys() {
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


    std::string Viewer::chron_trim(const std::string& s) {
        std::size_t a = s.find_first_not_of(" \t\r\n");
        std::size_t b = s.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    }


    // Last ~3 sentences of a chapter body, for prompt continuity.
    std::string Viewer::chron_tail_of(const std::string& body) {
        if (body.size() <= 340) return body;
        std::string t = body.substr(body.size() - 340);
        const std::size_t dot = t.find(". ");
        if (dot != std::string::npos) t = t.substr(dot + 2);
        return chron_trim(t);
    }


    void Viewer::chron_load(const World& w) {
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


    void Viewer::chron_append(const ChronicleChapter& c) {
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
    void Viewer::chron_tick(const World& w) {
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


    void Viewer::llm_ch_drain() {
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
    std::string Viewer::mat_clean(std::string s, std::size_t cap) {
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


    std::string Viewer::mat_slug_of(const std::string& name) {
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
    void Viewer::mat_load() {
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


    bool Viewer::mat_known(std::uint64_t synth_id) const {
        char buf[24];
        std::snprintf(buf, sizeof buf, "0x%016llx",
                      static_cast<unsigned long long>(synth_id));
        for (const MatEntry& e : mat_entries)
            if (e.id == buf) return true;
        return llm_mat_pending.count(synth_id) != 0;
    }


    void Viewer::mat_scan(const World& w) {
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


    void Viewer::llm_mat_drain() {
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
    std::string Viewer::myth_dkey(const AgentMind::DoctrineSlot& d) {
        std::ostringstream o;
        o << static_cast<int>(d.trig_kind) << ':' << d.trig_prop << ':'
          << d.trig_thr << ':' << d.trig_id << ':' << d.trig_text << ':'
          << d.verb;
        return o.str();
    }


    std::string Viewer::myth_focus_text(const World& w,
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


    void Viewer::myth_load() {
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


    void Viewer::myth_scan(const World& w) {
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


    void Viewer::llm_myth_drain() {
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



    void Viewer::llm_scan(const World& w) {
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


    void Viewer::llm_drain() {
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

}  // namespace simlab_viewer
