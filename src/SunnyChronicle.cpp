// Sunnyworld Chronicle — era-boundary literary historian.
// READ-ONLY collector: scans the causal ledger + brain slots + terrain,
// formats a structured prompt, hands it to NarrativeHost's worker thread.
// This file NEVER mutates World state and uses no RNG — the chronicle
// cannot affect state_hash(), t1==t8, or dev==release.
#include "simlab/SunnyChronicle.hpp"

#include <algorithm>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "simlab/Brain.hpp"
#include "simlab/Causal.hpp"
#include "simlab/NarrativeHost.hpp"
#include "simlab/Terrain.hpp"
#include "simlab/World.hpp"

namespace simlab {
namespace {

std::string trim(const std::string& s) {
    std::size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    std::size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Last ~3 sentences of a chapter body -> continuity tail for next prompt.
std::string tail_of(const std::string& body) {
    if (body.size() <= 340) return body;
    std::string t = body.substr(body.size() - 340);
    const std::size_t dot = t.find(". ");
    if (dot != std::string::npos) t = t.substr(dot + 2);
    return trim(t);
}

bool skip_action(const std::string& a) {
    static const char* kSkip[] = {
        "metabolize", "cool", "ecology", "political_drift", "myth_drift",
        "language", "anatomy_drift", "decay_minds", "move", "rest",
        "perceive", "witness", "advance_ticks", "flush_needs", "drift",
        "sleep_consolidate", "reproduce_drift"};
    for (const char* s : kSkip) if (a == s) return true;
    return a.empty();
}

// Salience of one causal event (deterministic, integer).
std::int64_t event_score(const CausalEvent& ev) {
    if (ev.failed || skip_action(ev.action)) return 0;
    const std::string& a = ev.action;
    std::int64_t s = 30;
    if (a == "reproduce") s = 240;
    else if (a == "strike") s = 170 + (ev.participants.size() > 2 ? 60 : 0);
    else if (a == "steal") s = 110;
    else if (a == "trade" || a == "gift" || a == "share" ||
             a == "treaty" || a == "diplomatic_parley") s = 150;
    else if (a == "perform_ritual" || a == "consecrate") s = 130;
    else if (a == "treat" || a == "heal" || a == "sow" || a == "dig" ||
             a == "combine" || a == "craft" || a == "carve") s = 110;
    else if (a == "communicate") s = 45;
    if (!ev.spawned.empty()) s += 80;
    if (ev.participants.size() >= 3) s += 40;   // crowd scenes matter more
    return s;
}

struct Moment {
    std::uint64_t tick = 0;
    std::int64_t score = 0;
    std::string text;
    Vec2 pos{0, 0};
};

struct ChapterStub {
    std::uint64_t tick0 = 0, tick1 = 0;
    std::string facts;
};

} // namespace

struct SunnyChronicle::Impl {
    Config cfg;
    bool primed = false;
    std::uint64_t era0 = 0, era1 = 0;          // [era0, era1) open era
    std::uint64_t ledger_cursor = 0;          // last scanned event id
    std::size_t synth_cursor = 0;
    std::unordered_set<EntityId> alive_prev;  // alive set at last boundary
    std::vector<Moment> moments;              // accumulated (tick-tagged)
    // LLM plumbing (presentation only)
    std::uint64_t key_seq = 0;
    std::unordered_map<std::uint64_t, ChapterStub> pend;
    std::deque<std::uint64_t> outq;
    std::string tail;                          // rolling ~3-sentence summary

    static std::string sys_prompt(const std::string& lang) {
        return "You are the Historian of Sunnyworld — a silent witness "
            "recording the evolution, suffering and wonder of its "
            "creatures across the ages.\n"
            "ABSOLUTE RULES:\n"
            "1. TRUTHFUL TO THE RECORD: narrate ONLY the facts provided "
            "(events, names, species, terrain, emotions, fears, trust). "
            "NEVER invent characters, materials, places or events absent "
            "from the data.\n"
            "2. CONTINUITY: continue naturally from the Previous passage "
            "so the chronicle reads as one unbroken novel.\n"
            "3. VOICE: solemn, vivid, literary prose with psychological "
            "depth — render fear as dread, joy as warmth, betrayal as a "
            "cold shadow. Describe the land itself (biome, weather, "
            "water, ruins) as a character.\n"
            "4. REQUIRED FORMAT:\n"
            "Line 1: a short chapter title (at most 8 words, no quotes).\n"
            "Following lines: 1-2 paragraphs of chronicle prose "
            "(no bullet lists, no meta commentary).\n"
            "5. LANGUAGE: write the entire chapter in " + lang + ".";
    }

    std::string entity_label(const World& w, EntityId id) const {
        const Entity* e = w.find(id);
        if (!e) return {};
        static thread_local SeqPidCache pc;
        SeqPid np(pc, w.props);
        const PropId name_p = np("name");
        if (name_p != kNoProp) {
            if (const Value* v = w.prop_of(*e, name_p))
                if (v->type == ValType::Text && !v->s.empty()) return v->s;
        }
        if (e->material != kNoMaterial) {
            if (const MaterialDef* m = w.materials.find(e->material))
                return m->name;
        }
        return std::string(e->kind_name);
    }

    void scan_ledger(World& w, std::uint64_t bound) {
        const auto& hist = w.ledger.history();
        // history_ is id-ordered: jump straight to the first unscanned event
        // (O(log n) — tick() is called every cycle, never rescan the window).
        auto it = std::lower_bound(hist.begin(), hist.end(), ledger_cursor,
            [](const CausalEvent& e, std::uint64_t c){ return e.id <= c; });
        for (; it != hist.end(); ++it) {
            const CausalEvent& ev = *it;
            if (ev.tick >= bound) break;   // ids ~ tick-ordered: rest is next era
            ledger_cursor = ev.id;
            const std::int64_t sc = event_score(ev);
            if (sc < 60) continue;
            Moment m;
            m.tick = ev.tick; m.score = sc;
            m.pos = Vec2{0, 0};
            if (!ev.participants.empty())
                if (const Entity* a = w.find(ev.participants[0]))
                    m.pos = a->pos;
            std::string line = "t" + std::to_string(ev.tick) + ": " +
                               ev.action;
            if (ev.participants.size() > 1) {
                const std::string who = entity_label(w, ev.participants[1]);
                if (!who.empty()) line += " -> " + who;
            }
            if (!ev.spawned.empty())
                line += " (spawned " + std::to_string(ev.spawned.size()) + ")";
            m.text = std::move(line);
            // bounded top-K by score (keep the era's most salient moments)
            if (moments.size() < 160) moments.push_back(std::move(m));
            else {
                auto it = std::min_element(moments.begin(), moments.end(),
                    [](const Moment& a, const Moment& b){ return a.score < b.score; });
                if (sc > it->score) *it = std::move(m);
            }
        }
    }

    void scan_synth(World& w) {
        while (synth_cursor < w.synthesized_.size()) {
            const World::SynthRecord& r = w.synthesized_[synth_cursor];
            if (r.tick >= era1) break;   // belongs to next era
            ++synth_cursor;
            const MaterialDef* m = w.materials.find(r.material);
            Moment mm;
            mm.tick = r.tick; mm.score = 200;
            mm.text = "t" + std::to_string(r.tick) +
                      ": new material '" +
                      (m ? m->name : std::string("?")) + "' synthesized (" +
                      r.verb + ")";
            if (const Entity* a = w.find(r.actor)) mm.pos = a->pos;
            moments.push_back(std::move(mm));
        }
    }

    std::string biome_at(const World& w, Vec2 p) const {
        const TerrainGrid& t = w.terrain;
        if (t.w <= 0 || !t.contains(p)) return "terra incognita";
        const std::int64_t temp = t.temp_at(p);    // deci-°C
        const std::int64_t moist = t.moist_at(p);  // permille
        const std::int64_t elev = t.elev_at(p);
        if (t.is_water(p)) return "mat nuoc";
        std::string b;
        b += temp < 250 ? "bang gia, " : temp < 380 ? "lanh, " :
             temp < 480 ? "on hoa, " : "nong, ";
        b += moist < 300 ? "kho can" : moist < 600 ? "kho" :
             moist < 800 ? "am" : "ngap nuoc";
        if (elev > 300) b += ", vung cao";
        return b;
    }

    // Compose the era snapshot: top moments + environment + brain states.
    std::string compose_facts(World& w, std::uint64_t t0, std::uint64_t t1,
                              std::string& stub_facts) {
        // top-N moments by score (deterministic order: score desc, tick asc)
        std::vector<const Moment*> top;
        for (const Moment& m : moments)
            if (m.tick >= t0 && m.tick < t1) top.push_back(&m);
        std::sort(top.begin(), top.end(), [](const Moment* a, const Moment* b){
            if (a->score != b->score) return a->score > b->score;
            return a->tick < b->tick;
        });
        if (static_cast<int>(top.size()) > cfg.top_moments)
            top.resize(static_cast<std::size_t>(cfg.top_moments));

        std::string facts;
        // ---- headline stats ------------------------------------------------
        std::uint64_t alive = 0, dead = 0;
        std::map<std::string, std::uint64_t> kinds;
        for (const auto& ep : w.entities) {
            if (!ep->alive) { ++dead; continue; }
            ++alive;
            ++kinds[std::string(ep->kind_name)];
        }
        std::string ks;
        std::size_t nk = 0;
        for (const auto& [k, n] : kinds) {
            if (++nk > 14) break;   // cap prompt size; full census stays in .md? no — keep top kinds only
            if (!ks.empty()) ks += ", ";
            ks += k + " x" + std::to_string(n);
        }
        if (kinds.size() > 14) ks += ", ...";
        facts += "[Ky nguyen " + std::to_string(t0) + "-" + std::to_string(t1) +
                 "] Song: " + std::to_string(alive) +
                 " | Tu vong tich luy: " + std::to_string(dead) +
                 " | Loai: " + ks + "\n";

        // ---- births / deaths this era (alive-set diff) ---------------------
        std::unordered_set<EntityId> alive_now;
        for (const auto& ep : w.entities) if (ep->alive) alive_now.insert(ep->id);
        std::uint64_t births = 0, deaths = 0;
        for (EntityId id : alive_now) if (!alive_prev.count(id)) ++births;
        for (EntityId id : alive_prev) if (!alive_now.count(id)) ++deaths;
        facts += "Sinh moi: " + std::to_string(births) +
                 " | Chet trong ky nguyen: " + std::to_string(deaths) + "\n";
        alive_prev = std::move(alive_now);

        // ---- top moments ----------------------------------------------------
        facts += "[Khoanh khac tieu bieu]:\n";
        for (const Moment* m : top) facts += "- " + m->text + "\n";
        stub_facts.clear();
        for (const Moment* m : top) { stub_facts += m->text; stub_facts += "; "; }

        // ---- environmental context ------------------------------------------
        Vec2 cen{0, 0}; std::int64_t n = 0;
        for (const Moment* m : top) { cen.x += m->pos.x; cen.y += m->pos.y; ++n; }
        if (n) { cen.x = static_cast<std::int32_t>(cen.x / n);
                 cen.y = static_cast<std::int32_t>(cen.y / n); }
        else if (!w.entities.empty()) cen = w.entities.front()->pos;
        facts += "[Moi truong]: tam su kien (" + std::to_string(cen.x) + "," +
                 std::to_string(cen.y) + ") - " + biome_at(w, cen) + "\n";
        // landmarks: enclosures / language entities (settlements, monuments)
        std::uint64_t encl = 0, langs = 0;
        for (EntityId id : w.entities_of_kind(SIMLAB_KIND("enclosure")))
            if (const Entity* e = w.find(id); e && e->alive) ++encl;
        for (EntityId id : w.entities_of_kind(SIMLAB_KIND("language")))
            if (const Entity* e = w.find(id); e && e->alive) ++langs;
        facts += "Cong trinh: " + std::to_string(encl) + " enclosure, " +
                 std::to_string(langs) + " ngon ngu/van tu\n";

        // ---- brain internals: dominant emotion, fears, trust, migration ----
        if (w.brains_) {
            struct BLine { std::int64_t drama; std::string text; };
            std::vector<BLine> bl;
            int scanned = 0;
            for (const auto& [id, slot] : w.brains_->slots) {
                if (++scanned > 48) break;    // bounded read
                const Entity* e = w.find(id);
                if (!e || !e->alive) continue;
                const BrainVital& v = slot.vital;
                // dominant emotion
                int ei = 0;
                for (int i = 1; i < kBEmoCount; ++i)
                    if (std::abs(v.emo[i]) > std::abs(v.emo[ei])) ei = i;
                static const char* kEmo[] = {
                    "so hai"," gian du"," to mo","bi ai","vui suong","ghe tom",
                    "ngac nhien","tin cay","gan bo"};
                // strongest fear trace
                std::int64_t fv = 0; int nfear = 0;
                for (const auto& f : v.fears)
                    if (f.ctx && f.strength > 0) { ++nfear; fv = std::min(fv, static_cast<std::int64_t>(f.valence) * f.strength / 1000); }
                // social: min trust, max familiarity
                std::int64_t mintrust = 1000;
                for (const BrainSocial& so : slot.cog.social) {
                    if (!so.key) continue;
                    const std::int64_t tr = so.trust;
                    if (tr < mintrust) mintrust = tr;
                }
                const std::int64_t drama =
                    std::abs(static_cast<std::int64_t>(v.last_rpe)) +
                    std::abs(static_cast<std::int64_t>(v.last_charge)) +
                    (fv < 0 ? -fv : 0);
                std::string line = "- " + entity_label(w, id) + ": cam xuc " +
                    kEmo[ei] + " (" + std::to_string(v.emo[ei]) + ")";
                if (nfear) line += ", " + std::to_string(nfear) +
                    " noi so (sau nhat " + std::to_string(fv) + ")";
                if (mintrust < 1000) line += ", trust thap nhat " +
                    std::to_string(mintrust);
                bl.push_back({drama, std::move(line)});
            }
            std::sort(bl.begin(), bl.end(), [](const BLine& a, const BLine& b){ return a.drama > b.drama; });
            facts += "[Noi tam noi bat]:\n";
            for (std::size_t i = 0; i < bl.size() && i < 4; ++i)
                facts += bl[i].text + "\n";
        }

        // ---- discoveries ------------------------------------------------------
        if (synth_cursor < w.synthesized_.size()) {
            facts += "[Kham pha vat lieu]:\n";
            for (std::size_t i = synth_cursor; i < w.synthesized_.size() &&
                 i < synth_cursor + 8; ++i) {
                const auto& r = w.synthesized_[i];
                const MaterialDef* m = w.materials.find(r.material);
                facts += "- '" + (m ? m->name : std::string("?")) +
                         "' (" + r.verb + ")\n";
            }
        }
        return facts;
    }

    void append_file(const std::string& title, const std::string& body,
                     const std::string& facts) {
        std::error_code ec;
        std::filesystem::create_directories("saves", ec);
        std::ofstream f(cfg.out_path, std::ios::app);
        if (!f) return;
        f << "## " << title << "\n\n" << body << "\n\n";
        if (!facts.empty()) f << "*(Du kien goc: " << facts << ")*\n\n";
    }

    void close_era(World& w, NarrativeHost* host) {
        if (era1 <= era0) return;
        std::string stub_facts;
        const std::string facts = compose_facts(w, era0, era1, stub_facts);
        // drop used moments of this era
        moments.erase(std::remove_if(moments.begin(), moments.end(),
            [&](const Moment& m){ return m.tick < era1; }), moments.end());

        if (!host || !host->active() || outq.size() >= 4) {
            // raw record fallback — chronicle stays complete without LLM
            append_file("Ky nguyen " + std::to_string(era0) + " - " +
                        std::to_string(era1), trim(facts), stub_facts);
        } else {
            std::string user = std::string("[Previous passage]:\n") +
                (tail.empty() ? std::string("(The chronicle begins here.)")
                              : tail) + "\n\n";
            user += "[Era facts - cycles " + std::to_string(era0) + " -> " +
                    std::to_string(era1) + "]:\n" + facts + "\n";
            user += "Write the next chronicle chapter entirely in " +
                    cfg.language +
                    ". Title line first, then prose grounded ONLY in the "
                    "facts above — invent nothing.\n";
            const std::uint64_t key = ++key_seq;
            ChapterStub stub; stub.tick0 = era0; stub.tick1 = era1;
            stub.facts = stub_facts;
            pend.emplace(key, std::move(stub));
            outq.push_back(key);
            host->submit_prompt(key, sys_prompt(cfg.language), user, 384);
        }
        era0 = era1;
        era1 = era0 + cfg.era_ticks;
        // advance past any skipped eras (sparse tick calls)
        const std::uint64_t now = w.clock_.tick();
        while (now >= era1) { era0 = era1; era1 += cfg.era_ticks; }
    }

};

SunnyChronicle::SunnyChronicle() : SunnyChronicle(Config{}) {}
SunnyChronicle::SunnyChronicle(Config cfg)
    : impl_(std::make_unique<Impl>()) { impl_->cfg = std::move(cfg); }
SunnyChronicle::~SunnyChronicle() = default;

void SunnyChronicle::tick(World& w, NarrativeHost* host) {
    Impl& im = *impl_;
    const std::uint64_t now = w.clock_.tick();
    if (!im.primed) {
        im.primed = true;
        im.era0 = (now / im.cfg.era_ticks) * im.cfg.era_ticks;
        im.era1 = im.era0 + im.cfg.era_ticks;
        for (const auto& ep : w.entities)
            if (ep->alive) im.alive_prev.insert(ep->id);
        im.ledger_cursor = w.ledger.next_id();
        im.synth_cursor = w.synthesized_.size();
    }
    im.scan_ledger(w, im.era1);
    im.scan_synth(w);
    if (now >= im.era1) im.close_era(w, host);
    // drain completed chapters (needs host)
    if (host && !im.outq.empty()) {
        std::deque<std::uint64_t> keep;
        for (std::uint64_t key : im.outq) {
            std::string text;
            if (!host->poll(key, text)) { keep.push_back(key); continue; }
            auto it = im.pend.find(key);
            if (it == im.pend.end()) continue;
            const ChapterStub stub = it->second;
            im.pend.erase(it);
            const std::size_t nl = text.find('\n');
            std::string title, body;
            if (nl == std::string::npos) {
                body = trim(text);
                title = "Ky nguyen " + std::to_string(stub.tick0) + " - " +
                        std::to_string(stub.tick1);
            } else {
                title = trim(text.substr(0, nl));
                body = trim(text.substr(nl + 1));
                while (!title.empty() && (title.front() == '#' ||
                       title.front() == '*' || title.front() == '"' ||
                       title.front() == '\'' || title.front() == '[' ||
                       title.front() == ' ')) title.erase(title.begin());
                while (!title.empty() && (title.back() == '*' ||
                       title.back() == '#' || title.back() == ' ' ||
                       title.back() == '"' || title.back() == '\'' ||
                       title.back() == ']')) title.pop_back();
            }
            if (body.empty()) body = stub.facts;
            im.append_file(title.empty() ? "Ky nguyen" : title, body,
                           stub.facts);
            im.tail = tail_of(body);
        }
        im.outq = std::move(keep);
    }
}

} // namespace simlab
