#include "simlab/render/Patron.hpp"
#include "simlab/World.hpp"
#include "simlab/render/Snapshot.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace simlab::render {

namespace {

PropId pid_of(const World& w, const char* n) {
    const PropertyDef* d = w.props.find_name(n);
    return d ? d->id : kNoProp;
}
std::string text_prop(const World& w, const Entity& e, const char* n) {
    const PropId p = pid_of(w, n);
    if (p == kNoProp) return "";
    const Value* v = w.prop_of(e, p);
    return v && v->type == ValType::Text ? v->s : "";
}
std::int64_t int_prop(const World& w, const Entity& e, const char* n, std::int64_t d = 0) {
    const PropId p = pid_of(w, n);
    return p == kNoProp ? d : w.prop_int(e, p, d);
}

std::string hex(const std::string& s) {
    static const char* k = "0123456789abcdef";
    std::string o;
    o.reserve(s.size() * 2 + 1);
    for (unsigned char c : s) { o.push_back(k[c >> 4]); o.push_back(k[c & 15]); }
    return o.empty() ? "-" : o;
}
std::string unhex(const std::string& s) {
    if (s == "-") return "";
    std::string o;
    auto nib = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (std::size_t i = 0; i + 1 < s.size(); i += 2)
        o.push_back(static_cast<char>(nib(s[i]) * 16 + nib(s[i + 1])));
    return o;
}

std::vector<std::string> words_lower(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c >= 0x80) cur.push_back(static_cast<char>(std::tolower(c)));
        else if (!cur.empty()) { out.push_back(cur); cur.clear(); }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// display titles for firsts (viewer text only; any verb without an entry
// still becomes a milestone under a generic title)
const char* verb_title(const std::string& v) {
    static const std::map<std::string, const char*> k = {
        {"heat", "First Fire"}, {"wield", "First Tool in Hand"},
        {"assemble", "First Crafted Object"}, {"strike", "First Blow Struck"},
        {"chop", "First Felling"}, {"dig", "First Digging"}, {"carve", "First Carving"},
        {"process", "First Processing"}, {"communicate", "First Words Spoken"},
        {"engrave", "First Engraving"}, {"found", "First Community"},
        {"enclose", "First Shelter Raised"}, {"perform_ritual", "First Rite"},
        {"consecrate", "First Sacred Stone"}, {"adjudicate", "First Judgement"},
        {"trade", "First Trade"}, {"treat", "First Healing"}, {"reproduce", "First Birth"},
        {"launch", "First Missile Loosed"}, {"mobilize", "First War Band"},
        {"diplomatic_parley", "First Parley"}, {"steal", "First Theft"},
        {"share", "First Gift"}, {"tribute", "First Tribute"},
        {"impact", "First Knapping"}, {"combine", "First Mixture"},
        {"disassemble", "First Dismantling"}, {"read", "First Reading"},
        {"coat", "First Poisoned Edge"}, {"don", "First Armour Worn"},
        {"repay", "First Debt Repaid"}, {"enclose_repair", "First Repair"}};
    const auto it = k.find(v);
    return it == k.end() ? nullptr : it->second;
}
const char* kind_title(const std::string& kind) {
    static const std::map<std::string, const char*> k = {
        {"lexical_entry", "First Word Coined"}, {"language", "A Tongue Is Born"},
        {"semantic_concept", "First Idea Named"}, {"composite", "First Tool Made"},
        {"enclosure", "First Shelter Standing"}, {"inscription", "First Inscription"},
        {"debt_obligation", "First Debt"}};
    const auto it = k.find(kind);
    return it == k.end() ? nullptr : it->second;
}

bool trivial_action(const std::string& a) {
    return a == "metabolize" || a == "ecology" || a == "cool" || a == "language" ||
           a == "move" || a == "rest" || a == "decay" || a.empty();
}

std::int64_t resonance_gain(const std::string& a) {
    if (a == "reproduce") return 2;
    if (a == "engrave" || a == "consecrate") return 6;
    if (a == "perform_ritual") return 8;
    if (a == "found" || a == "enclose") return 8;
    if (a == "diplomatic_parley" || a == "treaty") return 12;
    if (a == "treat") return 3;
    return 0;
}

// (lang, concept) -> entry, entry -> concept (lexical entries only)
struct LexIndex {
    std::map<std::pair<std::int64_t, std::string>, EntityId> by_concept;
    std::map<EntityId, std::string> concept_of;
    std::map<EntityId, std::int64_t> lang_of;
    void build(const World& w) {
        for (EntityId id : w.entities_of_kind(KindName("lexical_entry"))) {
            const Entity* e = w.find(id);
            if (!e || !e->alive) continue;
            const std::string ck = text_prop(w, *e, "concept_key");
            const std::int64_t lg = int_prop(w, *e, "language_id");
            concept_of[id] = ck; lang_of[id] = lg;
            by_concept.emplace(std::make_pair(lg, ck), id);
        }
    }
};

bool cultural_actor(const World& w, const Entity& e) {
    const SpeciesDef* sp = w.species.find_name(e.kind_name.str());
    return sp && !sp->innate.empty();
}

}  // namespace

const char* word_tag_name(WordTag t) {
    switch (t) {
        case WordTag::Noun: return "Noun";
        case WordTag::Verb: return "Verb";
        case WordTag::Danger: return "Danger";
        case WordTag::Sacred: return "Sacred";
        default: return "-";
    }
}

std::string word_of_entry(const World& w, EntityId entry) {
    const Entity* e = w.find(entry);
    if (!e) return "";
    const std::int64_t ps = int_prop(w, *e, "phoneme_seed");
    return lang_render_word(static_cast<std::uint32_t>(ps), 2, w.world_seed);
}

std::string concept_gloss(const World& w, const std::string& key) {
    std::string g = key;
    for (EntityId id : w.entities_of_kind(KindName("semantic_concept"))) {
        const Entity* e = w.find(id);
        if (!e || !e->alive || text_prop(w, *e, "concept_key") != key) continue;
        g += " " + text_prop(w, *e, "chronicle");
        break;
    }
    // material ids inside keys (f26, m26, 26) -> material names
    std::string extra;
    for (const std::string& t : words_lower(key)) {
        std::string digits = t;
        if (!digits.empty() && (digits[0] == 'f' || digits[0] == 'm')) digits = digits.substr(1);
        if (digits.empty() || !std::all_of(digits.begin(), digits.end(), ::isdigit)) continue;
        if (digits.size() > 6) continue;
        if (const MaterialDef* md = w.materials.find(static_cast<MaterialId>(std::stoi(digits))))
            extra += " " + md->name;
    }
    return g + extra;
}

bool guess_confirmed(const World& w, const JournalWord& jw) {
    if (jw.guess.empty() || jw.truth.empty() || jw.heard + jw.carved <= 0) return false;
    const std::vector<std::string> truth = words_lower(concept_gloss(w, jw.truth));
    for (const std::string& gw : words_lower(jw.guess)) {
        if (gw.size() < 3) continue;
        for (const std::string& tw : truth) {
            if (tw == gw) return true;
            // shared stem of >= 4 letters ("burn"/"burning", "berr"/"berries")
            if (gw.size() >= 4 && tw.size() >= 4 && tw.compare(0, 4, gw, 0, 4) == 0) return true;
        }
    }
    return false;
}

std::string journal_reading(const World& w, const PatronJournal& j,
                            std::int64_t lang, const std::string& word) {
    const auto li = j.lexicon.find(lang);
    if (li == j.lexicon.end()) return "";
    const auto wi = li->second.find(word);
    if (wi == li->second.end() || wi->second.guess.empty()) return "";
    return wi->second.guess + (guess_confirmed(w, wi->second) ? " [ok]" : " [?]");
}

std::vector<std::string> inscription_words(const World& w, EntityId stone) {
    std::vector<std::string> out;
    const Entity* e = w.find(stone);
    if (!e) return out;
    const std::int64_t lg = int_prop(w, *e, "language_id");
    const std::string keys = text_prop(w, *e, "concept_keys");
    LexIndex ix; ix.build(w);
    std::size_t a = 0;
    while (a <= keys.size() && !keys.empty()) {
        const std::size_t b = keys.find('|', a);
        const std::string k = keys.substr(a, b == std::string::npos ? std::string::npos : b - a);
        if (const auto it = ix.by_concept.find({lg, k}); it != ix.by_concept.end()) {
            const std::string wd = word_of_entry(w, it->second);
            if (!wd.empty()) out.push_back(wd);
        }
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return out;
}

std::size_t patron_scan(const World& w, PatronJournal& j, const FogOfWar* fog) {
    const std::uint64_t now = w.clock_.tick();
    if (now > j.res_tick) {                                   // slow regen: 1 / 60 ticks
        const std::uint64_t n = (now - j.res_tick) / 60;
        j.resonance = std::min<std::int64_t>(j.resonance_max, j.resonance + static_cast<std::int64_t>(n));
        j.res_tick += n * 60;
    }
    const auto& hist = w.ledger.history();
    if (hist.empty() || hist.back().id < j.scanned) { j.scanned = w.ledger.next_id(); return 0; }
    LexIndex ix;
    bool ix_built = false;
    std::size_t found = 0;
    auto visible = [&](Vec2 p) { return !fog || fog->point_fog(w, p) == Fog::Visible; };
    auto add_ms = [&](const std::string& key, const std::string& title, const CausalEvent& ev,
                      Vec2 pos, EntityId who, std::int64_t lang) {
        if (!j.milestone_keys.insert(key).second) return false;
        j.milestones.push_back({key, title, ev.tick, pos, who, lang});
        ++found;
        return true;
    };
    for (const CausalEvent& ev : hist) {
        if (ev.id < j.scanned) continue;
        if (ev.failed || trivial_action(ev.action) || ev.participants.empty()) continue;
        const Entity* ae = w.find(ev.participants.front());
        if (!ae || !cultural_actor(w, *ae)) continue;
        const std::int64_t lang = int_prop(w, *ae, "language_id");
        const bool mine = lang != 0 && lang == j.tribe;
        // ---- firsts: verbs ---------------------------------------------------
        if (!(ev.action == "consume")) {
            const char* t = verb_title(ev.action);
            const std::string title = t ? t : "First '" + ev.action + "'";
            add_ms("act:" + ev.action, title, ev, ae->pos, ae->id, lang);
            if (j.milestone_keys.insert("act:" + ev.action + "@" + std::to_string(lang)).second && mine)
                j.resonance += 15;                            // our people's own first
        }
        // ---- firsts: culture kinds born from this event ------------------------
        for (EntityId sid : ev.spawned) {
            const Entity* se = w.find(sid);
            if (!se) continue;
            const char* kt = kind_title(se->kind_name.str());
            if (!kt) continue;
            add_ms("kind:" + se->kind_name.str(), kt, ev, se->pos, ae->id, lang);
            if (j.milestone_keys.insert("kind:" + se->kind_name.str() + "@" + std::to_string(lang)).second && mine)
                j.resonance += 10;
        }
        if (mine) j.resonance += resonance_gain(ev.action);
        // ---- the patron overhears / reads -------------------------------------
        if (!visible(ae->pos)) continue;
        if (ev.action == "communicate") {
            const AgentMind* m = w.find_mind(ae->id);
            if (!m) continue;
            if (!ix_built) { ix.build(w); ix_built = true; }
            std::vector<std::pair<std::int64_t, EntityId>> known;
            for (auto it = m->beliefs.lower_bound("lex:");
                 it != m->beliefs.end() && it->first.rfind("lex:", 0) == 0; ++it) {
                const EntityId eid = static_cast<EntityId>(std::strtoull(it->first.c_str() + 4, nullptr, 10));
                if (it->second.confidence >= 300 && ix.concept_of.count(eid))
                    known.push_back({-it->second.confidence, eid});
            }
            std::sort(known.begin(), known.end());
            SpeechBubble b; b.speaker = ae->id; b.lang = lang; b.tick = ev.tick;
            for (std::size_t k = 0; k < known.size() && k < 3; ++k) {
                const EntityId eid = known[k].second;
                const std::string wd = word_of_entry(w, eid);
                if (wd.empty()) continue;
                b.words.push_back(wd);
                JournalWord& jw = j.lexicon[ix.lang_of[eid]][wd];
                if (jw.heard + jw.carved == 0) jw.first_tick = ev.tick;
                ++jw.heard; jw.truth = ix.concept_of[eid];
            }
            if (!b.words.empty()) {
                j.bubbles.push_back(std::move(b));
                if (j.bubbles.size() > 48) j.bubbles.erase(j.bubbles.begin());
            }
        }
        for (EntityId sid : ev.spawned) {
            const Entity* se = w.find(sid);
            if (!se || se->kind_name != KindName("inscription")) continue;
            if (!ix_built) { ix.build(w); ix_built = true; }
            const std::int64_t slg = int_prop(w, *se, "language_id");
            const std::string keys = text_prop(w, *se, "concept_keys");
            std::size_t a = 0;
            while (!keys.empty()) {
                const std::size_t b2 = keys.find('|', a);
                const std::string k = keys.substr(a, b2 == std::string::npos ? std::string::npos : b2 - a);
                if (const auto it = ix.by_concept.find({slg, k}); it != ix.by_concept.end()) {
                    const std::string wd = word_of_entry(w, it->second);
                    if (!wd.empty()) {
                        JournalWord& jw = j.lexicon[slg][wd];
                        if (jw.heard + jw.carved == 0) jw.first_tick = ev.tick;
                        ++jw.carved; jw.truth = k;
                    }
                }
                if (b2 == std::string::npos) break;
                a = b2 + 1;
            }
        }
    }
    j.resonance = std::clamp<std::int64_t>(j.resonance, 0, j.resonance_max);
    j.scanned = w.ledger.next_id();
    return found;
}

bool PatronJournal::save(const std::string& path) const {
    std::ofstream o(path);
    if (!o) return false;
    o << "SWJ1\n";
    o << "tribe " << tribe << "\n";
    o << "res " << resonance << " " << resonance_max << " " << res_tick << "\n";
    o << "scanned " << scanned << "\n";
    for (EntityId p : pins) o << "pin " << p << "\n";
    for (const auto& [lang, words] : lexicon)
        for (const auto& [wd, jw] : words)
            o << "word " << lang << " " << hex(wd) << " " << static_cast<int>(jw.tag) << " "
              << jw.heard << " " << jw.carved << " " << jw.first_tick << " " << hex(jw.guess)
              << " " << hex(jw.truth) << "\n";
    for (const std::string& k : milestone_keys) o << "mk " << hex(k) << "\n";
    for (const Milestone& m : milestones)
        o << "ms " << hex(m.key) << " " << hex(m.title) << " " << m.tick << " " << m.pos.x
          << " " << m.pos.y << " " << m.who << " " << m.lang << "\n";
    o << "notes " << hex(notes) << "\n";
    return static_cast<bool>(o);
}

bool PatronJournal::load(const std::string& path) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    if (!std::getline(in, line) || line != "SWJ1") return false;
    *this = PatronJournal{};
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string k; ls >> k;
        if (k == "tribe") ls >> tribe;
        else if (k == "res") ls >> resonance >> resonance_max >> res_tick;
        else if (k == "scanned") ls >> scanned;
        else if (k == "pin") { EntityId p = 0; ls >> p; pins.insert(p); }
        else if (k == "word") {
            std::int64_t lang = 0; std::string wd, g, t; int tag = 0; JournalWord jw;
            ls >> lang >> wd >> tag >> jw.heard >> jw.carved >> jw.first_tick >> g >> t;
            jw.tag = static_cast<WordTag>(std::clamp(tag, 0, static_cast<int>(WordTag::Count) - 1));
            jw.guess = unhex(g); jw.truth = unhex(t);
            lexicon[lang][unhex(wd)] = jw;
        } else if (k == "mk") { std::string h; ls >> h; milestone_keys.insert(unhex(h)); }
        else if (k == "ms") {
            Milestone m; std::string hk, ht;
            ls >> hk >> ht >> m.tick >> m.pos.x >> m.pos.y >> m.who >> m.lang;
            m.key = unhex(hk); m.title = unhex(ht);
            milestones.push_back(m);
        } else if (k == "notes") { std::string h; ls >> h; notes = unhex(h); }
    }
    return true;
}

}  // namespace simlab::render
