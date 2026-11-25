// Phase 15 — Emergent Narrative Chronicle (SDL-free, viewer side).
// See include/simlab/render/Narrative.hpp. Reads the causal ledger + World
// Truth, composes sentences from data/narrative_templates.txt. Pure read:
// scan() mutates only the caller-owned ChronicleFeed.
#include "simlab/render/Narrative.hpp"
#include "simlab/render/Patron.hpp"
#include "simlab/render/Atlas.hpp"   // fnv
#include "simlab/World.hpp"
#include "simlab/Cognition.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace simlab::render {

namespace {

constexpr std::size_t kMaxLines = 400;
constexpr std::size_t kMaxPerScan = 240;
constexpr std::size_t kMaxSpawnsPerEvent = 3;
constexpr int kNearPad = 160;             // map units around the camera rect

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
std::string prop_name(const World& w, PropId p) {
    const PropertyDef* d = w.props.find(p);
    return d ? d->name : "?";
}
std::string mat_name(const World& w, MaterialId m) {
    if (m == kNoMaterial) return "";
    const MaterialDef* d = w.materials.find(m);
    return d ? d->name : "";
}
std::string words_of(const std::string& s) {           // "cannot_enter_water" -> "cannot enter water"
    std::string o = s;
    for (char& c : o) if (c == '_') c = ' ';
    return o;
}
std::uint64_t mix64(std::uint64_t h, std::uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 29;
    return h;
}

// ambient verbs thinned by a deterministic modulus (0 would mean skip —
// those live in skip_verb instead)
int thin_mod(const std::string& a) {
    if (a == "rest") return 12;
    if (a == "consume") return 8;
    if (a == "share" || a == "gift") return 3;
    if (a == "take" || a == "withdraw") return 6;
    if (a == "join" || a == "leave") return 4;
    if (a == "trade" || a == "tribute" || a == "repay") return 2;
    if (a == "read" || a == "process" || a == "combine") return 3;
    if (a == "gather" || a == "harvest" || a == "dig" || a == "chop") return 3;
    if (a == "extract" || a == "quarry" || a == "plant" || a == "sow") return 4;
    if (a == "impact" || a == "don" || a == "doff" || a == "wield") return 4;
    if (a == "coat" || a == "treat" || a == "enclose_repair") return 3;
    return 1;
}
bool skip_verb(const std::string& a) {
    return a == "metabolize" || a == "cool" || a == "ecology" ||
           a == "political_drift" || a == "myth_drift" || a == "language" ||
           a == "anatomy_drift" || a == "decay_minds" || a == "move" ||
           a == "perceive" || a == "witness" || a == "advance_ticks" ||
           a == "flush_needs" || a.empty();
}
// failed attempts worth a chronicle line (the rest are physics noise)
bool notable_failure(const std::string& a) {
    static const std::set<std::string> k = {
        "strike", "launch", "impact", "steal", "trade", "treaty", "adjudicate",
        "consecrate", "perform_ritual", "assemble", "reproduce",
        "diplomatic_parley", "mobilize", "tribute", "don", "coat", "wield",
        "consume", "engrave", "compose_clause"};
    return k.count(a) != 0;
}

std::uint8_t cat_bits(const std::string& s) {
    std::uint8_t c = 0;
    std::size_t a = 0;
    while (a <= s.size()) {
        const std::size_t b = s.find('|', a);
        const std::string t = s.substr(a, b == std::string::npos ? std::string::npos : b - a);
        if (t == "combat") c |= kCatCombat;
        else if (t == "survival") c |= kCatSurvival;
        else if (t == "discovery") c |= kCatDiscovery;
        else if (t == "society") c |= kCatSociety;
        else c |= kCatOther;
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return c;
}

std::string describe_impl(const World& w, EntityId id) {
    const Entity* e = w.find(id);
    if (!e) return "#" + std::to_string(id % 100000) + " (gone)";
    std::string d = "#" + std::to_string(id % 100000);
    if (!e->kind_name.empty()) d += " " + e->kind_name.str();
    if (e->material != kNoMaterial) {
        const MaterialDef* m = w.materials.find(e->material);
        if (m && m->name != "flesh" && e->kind_name.str().find(m->name) == std::string::npos)
            d += " (" + m->name + ")";
    }
    return d;
}

bool in_near(const ViewRect* near, Vec2 p) {
    if (!near) return true;
    return p.x >= near->x0 - kNearPad && p.x < near->x1 + kNearPad &&
           p.y >= near->y0 - kNearPad && p.y < near->y1 + kNearPad;
}

// Everything a {slot} may need for one emitted line.
struct SlotCtx {
    const World& w;
    const CausalEvent& ev;
    EntityId actor = kNoEntity, about = kNoEntity;
    const PatronJournal* journal = nullptr;
    const Entity* spawned = nullptr;         // spawn context (spawn:* keys)
    std::string part = "body", result = "it buckled";
    std::string item = "bare hands";
    std::string mat = "something";
    std::string qty = "a little";
    std::string pct = "?";
    std::string word = "...";
    std::string tongue{};
    std::string when{};
    std::string failure{};
};

void fill_ctx(SlotCtx& c) {
    const World& w = c.w;
    c.actor = c.ev.participants.empty() ? kNoEntity : c.ev.participants.front();
    c.about = c.ev.participants.size() > 1 ? c.ev.participants[1] : kNoEntity;
    const Entity* ae = c.actor != kNoEntity ? w.find(c.actor) : nullptr;
    const Entity* be = c.about != kNoEntity ? w.find(c.about) : nullptr;

    // wielded item in the actor's hands
    c.item = "bare hands";
    for (const Relation& r : w.relations_of(c.actor, "wielding"))
        if (r.a == c.actor && w.find(r.b)) { c.item = describe_impl(w, r.b); break; }

    // anatomy part hit + severity, read from the event's own state changes
    const PropId role_p = pid_of(w, "part_role");
    const PropId integ_p = pid_of(w, "integrity");
    bool fractured = false, bleeding = false, poisoned = false, broke = false;
    for (const StateChange& ch : c.ev.changes) {
        if (ch.entity == kNoEntity) continue;
        const Entity* pe = w.find(ch.entity);
        const std::string pn = prop_name(w, ch.prop);
        if (pe && role_p != kNoProp)
            if (const Value* rv = w.prop_of(*pe, role_p);
                rv && rv->type == ValType::Text && c.part == "body") c.part = rv->s;
        if (pn == "fracture_level" && ch.after.i > 0) fractured = true;
        else if (pn == "bleed_rate" && ch.after.i > 0) bleeding = true;
        else if (pn == "toxin" && ch.after.i > ch.before.i) poisoned = true;
        else if (pn == "integrity" && ch.after.i <= 0) broke = true;
    }
    if (poisoned && c.ev.action == "consume") {
        c.part = "gut";
        c.result = "it burned going down";
    } else if ((be && !be->alive) || broke ||
               (be && integ_p != kNoProp && w.prop_int(*be, integ_p, 1000) <= 0))
        c.result = "it lies still";
    else if (fractured) c.result = "the bone gave way";
    else if (bleeding) c.result = "blood sheets down";

    // quantity moved (prop named quantity, biggest |delta|)
    std::int64_t best = 0;
    for (const StateChange& ch : c.ev.changes)
        if (prop_name(w, ch.prop) == "quantity") {
            const std::int64_t d = ch.after.i - ch.before.i;
            if (std::llabs(d) > std::llabs(best)) best = d;
        }
    if (best) c.qty = std::to_string(std::llabs(best));
    if (c.spawned)
        if (const std::int64_t q = int_prop(w, *c.spawned, "quantity", 0); q > 0 && c.qty == "a little")
            c.qty = std::to_string(q);

    // integrity percent of the about/actor after the event
    if (integ_p != kNoProp)
        if (const Entity* e = be ? be : ae)
            c.pct = std::to_string(std::clamp<std::int64_t>(w.prop_int(*e, integ_p, 1000) / 10, 0, 100)) + "%";

    // material / gloss of the target — for spawn:* keys the spawned thing
    // speaks for itself (lexical_entry -> concept gloss, concept -> chronicle)
    if (c.spawned) {
        const std::string sk = c.spawned->kind_name.str();
        if (sk == "lexical_entry") {
            c.mat = concept_gloss(w, text_prop(w, *c.spawned, "concept_key"));
            const std::int64_t lang = int_prop(w, *c.spawned, "language_id");
            c.tongue = lang ? "tongue " + std::to_string(lang) : "";
            const std::string wd = word_of_entry(w, c.spawned->id);
            c.word = wd.empty() ? "..." : "\"" + wd + "\"";
            if (c.journal && !wd.empty()) {
                const std::string rd = journal_reading(w, *c.journal, lang, wd);
                if (!rd.empty()) c.word += " (" + rd + ")";
            }
        } else if (sk == "semantic_concept") {
            c.mat = text_prop(w, *c.spawned, "chronicle");
            if (c.mat.empty()) c.mat = sk;
        } else if (sk == "inscription") {
            c.mat = mat_name(w, c.spawned->material);
            if (c.mat.empty()) c.mat = sk;
            const std::int64_t lang = int_prop(w, *c.spawned, "language_id");
            c.tongue = lang ? "tongue " + std::to_string(lang) : "";
            std::string ws;
            for (const std::string& wd : inscription_words(w, c.spawned->id)) {
                if (!ws.empty()) ws += ", ";
                ws += "\"" + wd + "\"";
                if (c.journal) {
                    const std::string rd = journal_reading(w, *c.journal, lang, wd);
                    if (!rd.empty()) ws += " (" + rd + ")";
                }
            }
            c.word = ws.empty() ? "..." : ws;
        } else {
            c.mat = mat_name(w, c.spawned->material);
            if (c.mat.empty()) c.mat = sk;
        }
    } else {
        MaterialId mm = kNoMaterial;
        if (be && be->material != kNoMaterial) mm = be->material;
        else if (ae && ae->material != kNoMaterial) mm = ae->material;
        c.mat = mat_name(w, mm);
        if (c.mat.empty()) c.mat = "something";
    }

    // spoken words for communicate (speaker's confident lexicon, top 3)
    if (c.spawned == nullptr && c.ev.action == "communicate" && ae) {
        const AgentMind* m = w.find_mind(c.actor);
        if (m) {
            std::vector<std::pair<std::int64_t, EntityId>> known;
            for (auto it = m->beliefs.lower_bound("lex:");
                 it != m->beliefs.end() && it->first.rfind("lex:", 0) == 0; ++it) {
                const EntityId eid = static_cast<EntityId>(std::strtoull(it->first.c_str() + 4, nullptr, 10));
                if (it->second.confidence >= 300 && w.find(eid))
                    known.push_back({-it->second.confidence, eid});
            }
            std::sort(known.begin(), known.end());
            const std::int64_t lang = int_prop(w, *ae, "language_id");
            c.tongue = lang ? "tongue " + std::to_string(lang) : "";
            std::string ws;
            for (std::size_t k = 0; k < known.size() && k < 3; ++k) {
                const std::string wd = word_of_entry(w, known[k].second);
                if (wd.empty()) continue;
                if (!ws.empty()) ws += ", ";
                ws += "\"" + wd + "\"";
                if (c.journal) {
                    const std::string rd = journal_reading(w, *c.journal, lang, wd);
                    if (!rd.empty()) ws += " (" + rd + ")";
                }
            }
            if (!ws.empty()) c.word = ws;
            else c.word = "…";    // speaker has no confident words yet
        }
    }
    // carved/inscribed words when the event itself is a carving act
    if (c.spawned == nullptr &&
        (c.ev.action == "engrave" || c.ev.action == "inscribe" || c.ev.action == "compose_clause")) {
        for (EntityId sid : c.ev.spawned)
            if (const Entity* se = w.find(sid);
                se && se->kind_name == SIMLAB_KIND("inscription")) {
                const std::int64_t lang = int_prop(w, *se, "language_id");
                c.tongue = lang ? "tongue " + std::to_string(lang) : "";
                std::string ws;
                for (const std::string& wd : inscription_words(w, sid)) {
                    if (!ws.empty()) ws += ", ";
                    ws += "\"" + wd + "\"";
                    if (c.journal) {
                        const std::string rd = journal_reading(w, *c.journal, lang, wd);
                        if (!rd.empty()) ws += " (" + rd + ")";
                    }
                }
                if (!ws.empty()) c.word = ws;
                break;
            }
    }

    const std::uint64_t t = c.ev.tick;
    char wb[40];
    std::snprintf(wb, sizeof wb, "d%llu %02dh", (unsigned long long)(t / 24 + 1), int(t % 24));
    c.when = wb;
    c.failure = c.ev.failed ? words_of(c.ev.failure) : "";
}

std::string fill_slots(const std::string& tpl, const SlotCtx& c) {
    std::string out = tpl;
    auto sub = [&](const char* key, const std::string& val) {
        const std::string pat = std::string("{") + key + "}";
        std::size_t p = 0;
        while ((p = out.find(pat, p)) != std::string::npos) {
            out.replace(p, pat.size(), val);
            p += val.size();
        }
    };
    sub("actor", describe_impl(c.w, c.actor));
    sub("about", c.about != kNoEntity ? describe_impl(c.w, c.about) : "someone");
    sub("item", c.item);
    sub("part", c.part);
    sub("qty", c.qty);
    sub("mat", c.mat);
    sub("pct", c.pct);
    sub("word", c.word);
    sub("tongue", c.tongue);
    sub("claim", "");
    sub("charge", "");
    sub("when", c.when);
    sub("failure", c.failure);
    sub("site", "the camp");
    // collapse doubled spaces left by empty slots
    std::string tidy;
    tidy.reserve(out.size());
    bool sp = false;
    for (char ch : out) {
        if (ch == ' ') { if (!sp) tidy.push_back(ch); sp = true; }
        else { tidy.push_back(ch); sp = false; }
    }
    while (!tidy.empty() && tidy.back() == ' ') tidy.pop_back();
    return tidy;
}

// qualifier upgrade: "strike" -> "strike:wound" / "strike:kill" etc.
std::string qualifier(const World& w, const CausalEvent& ev) {
    const std::string& a = ev.action;
    if (a == "strike" || a == "launch") {
        const Entity* be = ev.participants.size() > 1 ? w.find(ev.participants[1]) : nullptr;
        if (be && !be->alive) return "kill";
        const PropId integ_p = pid_of(w, "integrity"), frac_p = pid_of(w, "fracture_level"),
                     bleed_p = pid_of(w, "bleed_rate");
        const PropId role_p = pid_of(w, "part_role");
        for (const StateChange& ch : ev.changes) {
            if (ch.entity == kNoEntity) continue;
            const Entity* pe = w.find(ch.entity);
            const std::string pn = prop_name(w, ch.prop);
            const bool is_part = pe && role_p != kNoProp && w.prop_of(*pe, role_p);
            if (!is_part) continue;
            if ((pn == "integrity" && ch.after.i < ch.before.i) ||
                (pn == "fracture_level" && ch.after.i > ch.before.i) ||
                (pn == "bleed_rate" && ch.after.i > ch.before.i)) return "wound";
        }
        (void)integ_p; (void)frac_p; (void)bleed_p;
    }
    if (a == "consume") {
        const PropId tox_p = pid_of(w, "toxin");
        if (tox_p != kNoProp && !ev.participants.empty())
            if (const Entity* ae = w.find(ev.participants.front()))
                for (const StateChange& ch : ev.changes)
                    if (ch.entity == ae->id && ch.prop == tox_p && ch.after.i > ch.before.i)
                        return "toxic";
    }
    return "";
}

}  // namespace

const char* narr_cat_name(std::uint8_t cat) {
    if (cat & kCatCombat) return "combat";
    if (cat & kCatSurvival) return "survival";
    if (cat & kCatDiscovery) return "discovery";
    if (cat & kCatSociety) return "society";
    return "other";
}

// ---- template loading ---------------------------------------------------------
bool NarrativeComposer::load(const std::string& path) {
    rules_.clear(); by_key_.clear();
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        // strip comment
        const std::size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        // split on '|'
        std::vector<std::string> f;
        std::size_t a = 0;
        while (a <= line.size()) {
            const std::size_t b = line.find('|', a);
            f.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
            if (b == std::string::npos) break;
            a = b + 1;
        }
        if (f.size() < 4) continue;
        for (std::string& s : f) {   // trim
            const auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
            while (!s.empty() && ws(s.front())) s.erase(s.begin());
            while (!s.empty() && ws(s.back())) s.pop_back();
        }
        if (f[0].empty()) continue;
        Rule r; r.key = f[0]; r.cat = cat_bits(f[1]); r.callout = f[2] == "1" || f[2] == "true";
        // template = fields 3.. joined back with '|'
        for (std::size_t i = 3; i < f.size(); ++i) { if (i > 3) r.text += '|'; r.text += f[i]; }
        by_key_[r.key].push_back(rules_.size());
        rules_.push_back(std::move(r));
    }
    if (rules_.empty()) {
        // built-in fallback: a minimal but functional set
        const char* fb[] = {
            "strike|combat|1|{actor} struck {about} with {item} — {when}",
            "consume|survival|0|{actor} ate {qty} {mat}",
            "communicate|society|0|{actor} spoke: \"{word}\"",
            "found|society|1|{actor} raised a new community",
            "*|other|0|{actor} {when} — {mat}",
        };
        for (const char* l : fb) {
            std::istringstream ls(l);
            std::string k, c, co, t;
            std::getline(ls, k, '|'); std::getline(ls, c, '|');
            std::getline(ls, co, '|'); std::getline(ls, t, '|');
            Rule r; r.key = k; r.cat = cat_bits(c); r.callout = co == "1"; r.text = t;
            by_key_[r.key].push_back(rules_.size());
            rules_.push_back(std::move(r));
        }
        return false;
    }
    return true;
}

// ---- picking -------------------------------------------------------------------
const NarrativeComposer::Rule* NarrativeComposer::pick(const std::string& key,
                                                       std::uint64_t tick,
                                                       bool star_fallback) const {
    std::string k = key;
    if (k.empty() && star_fallback) k = "*";
    auto it = by_key_.find(k);
    if (it == by_key_.end() || it->second.empty()) {
        if (!star_fallback || k == "*") return nullptr;
        it = by_key_.find("*");
        if (it == by_key_.end() || it->second.empty()) return nullptr;
    }
    const auto& v = it->second;
    std::uint64_t h = fnv(k);
    h = mix64(h, tick);
    return &rules_[v[h % v.size()]];
}
const NarrativeComposer::Rule* NarrativeComposer::pick2(const std::string& qualified,
                                                        const std::string& base,
                                                        std::uint64_t tick) const {
    if (!qualified.empty())
        if (const Rule* r = pick(qualified, tick, false)) return r;
    if (!base.empty())
        if (const Rule* r = pick(base, tick, false)) return r;
    return pick("", tick, true);
}

// ---- emitting -------------------------------------------------------------------
void NarrativeComposer::emit(const World& w, ChronicleFeed& f, const std::string& key,
                             const std::string& base_key, const CausalEvent& ev,
                             EntityId actor, EntityId about, const Entity* spawned,
                             Vec2 pos, const PatronJournal* journal, bool pinned,
                             bool star_fallback) const {
    const Rule* r = star_fallback ? pick2(key, base_key, ev.tick)
                                  : pick(key, ev.tick, false);
    if (!r) return;
    // Spam throttle: one actor repeating the same kind of act tells the
    // chronicle once per cooldown — a predator that "takes up" sticks every
    // cycle is ecology, not news. Combat and callout-worthy lines always tell.
    const std::uint64_t kh = mix64(fnv(std::to_string(actor)), fnv(r->key));
    std::uint64_t& slot = f.last_tell[kh];
    const std::uint64_t cooldown = (r->cat & kCatCombat) ? 0
        : r->callout ? 240 : 960;   // ~10 / ~40 days at 24 ticks per day
    if (cooldown && slot != 0 && ev.tick < slot + cooldown) return;
    slot = ev.tick;
    SlotCtx c{w, ev, actor, about, journal, spawned};
    fill_ctx(c);
    NarrativeLine l;
    l.seq = f.seq_next++;
    l.tick = ev.tick; l.pos = pos; l.who = actor; l.about = about;
    l.text = fill_slots(r->text, c);
    if (ev.failed && !c.failure.empty()) l.text += " — " + c.failure;
    l.cat = r->cat | (pinned ? kCatPinned : 0);
    l.callout = r->callout;
    f.lines.push_back(std::move(l));
    while (f.lines.size() > kMaxLines) f.lines.pop_front();
}

// ---- event narration ------------------------------------------------------------
void NarrativeComposer::narrate_event(const World& w, ChronicleFeed& f, const CausalEvent& ev,
                                      const ViewRect* near, const std::set<EntityId>& pins,
                                      const PatronJournal* journal) const {
    const std::string& verb = ev.action;
    if (skip_verb(verb)) return;
    if (ev.failed && !notable_failure(verb)) return;
    const EntityId actor = ev.participants.empty() ? kNoEntity : ev.participants.front();
    const EntityId about = ev.participants.size() > 1 ? ev.participants[1] : kNoEntity;
    // deterministic thinning for ambient verbs (toxic meals always tell)
    const std::string qual = qualifier(w, ev);
    const int mod = (verb == "consume" && qual == "toxic") ? 1 : thin_mod(verb);
    if (mod > 1) {
        std::uint64_t h = mix64(fnv(verb), ev.tick);
        h = mix64(h, static_cast<std::uint64_t>(actor));
        if (h % static_cast<std::uint64_t>(mod) != 0) return;
    }
    const Entity* ae = actor != kNoEntity ? w.find(actor) : nullptr;
    const Entity* be = about != kNoEntity ? w.find(about) : nullptr;
    // The chronicle tells the people's story: only minds that speak a
    // language_id (founders from genesis, sapient kin, tongue-joined minds)
    // are people here — promoted fauna ecology is physics noise. A person on
    // EITHER side of the event ( striker or victim, even dead ) tells it.
    auto is_person = [&](EntityId id) -> bool {
        if (id == kNoEntity || w.find_mind(id) == nullptr) return false;
        const Entity* e = w.find(id);
        if (!e) return false;
        const PropId lp = pid_of(w, "language_id");
        return lp != kNoProp && w.prop_int(*e, lp, 0) > 0;
    };
    if (!is_person(actor) && !is_person(about)) return;
    Vec2 pos = ae ? ae->pos : (be ? be->pos : Vec2{});
    const bool pinned = pins.count(actor) || (about != kNoEntity && pins.count(about));
    if (!pinned && !in_near(near, pos)) return;

    // verb line (qualified first, e.g. strike:wound, consume:toxic)
    if (!verb.empty()) {
        const std::string qkey = qual.empty() ? verb : verb + ":" + qual;
        emit(w, f, qkey, verb, ev, actor, about, nullptr, pos, journal, pinned, true);
    }
    // spawn narratives (things born from this event)
    std::size_t done = 0;
    for (EntityId sid : ev.spawned) {
        if (done >= kMaxSpawnsPerEvent) break;
        const Entity* se = w.find(sid);
        if (!se || !se->alive) continue;
        const std::string sk = se->kind_name.str();
        // only narratable culture kinds get their own line
        static const std::set<std::string> kNarr = {
            "lexical_entry", "semantic_concept", "language", "inscription",
            "composite", "enclosure", "debt_obligation", "collective"};
        if (!kNarr.count(sk)) continue;
        emit(w, f, "spawn:" + sk, "", ev, actor, sid, se, se->pos, journal,
             pinned || pins.count(sid), false);
        ++done;
    }
}

// ---- discovery verification scan -----------------------------------------------
void NarrativeComposer::scan_minds(const World& w, ChronicleFeed& f, const ViewRect* near,
                                   const std::set<EntityId>& pins) const {
    std::size_t made = 0;
    for (const auto& [mid, mind] : w.minds_) {
        if (made >= 24) break;
        const Entity* e = w.find(mid);
        if (!e || !e->alive) continue;
        for (const auto& [key, rec] : mind.discoveries) {
            if (rec.status != DiscoveryStatus::Verified) continue;
            const std::string dk = std::to_string(mid) + ":" + key;
            if (!f.seen_verified.insert(dk).second) continue;
            const bool pinned = pins.count(mid) != 0;
            if (!pinned && !in_near(near, e->pos)) continue;
            // compose directly (no ledger event): "#42 human proved a truth: <claim>"
            NarrativeLine l;
            l.seq = f.seq_next++;
            l.tick = w.clock_.tick(); l.pos = e->pos; l.who = mid;
            l.text = describe_impl(w, mid) + " proved a truth: " + rec.claim;
            l.cat = kCatDiscovery | (pinned ? kCatPinned : 0);
            l.callout = true;
            f.lines.push_back(std::move(l));
            while (f.lines.size() > kMaxLines) f.lines.pop_front();
            ++made;
        }
    }
}

// ---- the scan --------------------------------------------------------------------
void NarrativeComposer::scan(const World& w, ChronicleFeed& f, const ViewRect* near,
                             const std::set<EntityId>& pins,
                             const PatronJournal* journal) const {
    // ---- ledger events ---------------------------------------------------------
    const auto& hist = w.ledger.history();
    if (!hist.empty()) {
        const std::uint64_t base = w.ledger.next_id() - hist.size();
        std::size_t i0 = hist.size();
        std::size_t produced = 0;
        if (!f.primed) {
            f.primed = true;
            f.scanned = w.ledger.next_id();          // skip backlog on first scan
        } else if (f.scanned + 1 >= base) {
            i0 = std::min<std::size_t>(std::size_t(f.scanned + 1 - base), hist.size());
        } else {
            f.scanned = w.ledger.next_id() - 1;      // cursor evicted: skip backlog
        }
        for (std::size_t i = i0; i < hist.size() && produced < kMaxPerScan; ++i) {
            const CausalEvent& ev = hist[i];
            f.scanned = base + i;
            const std::size_t before = f.lines.size();
            narrate_event(w, f, ev, near, pins, journal);
            produced += f.lines.size() - before;
        }
    }
    // ---- verified discoveries (mind-side, not ledger events) -------------------
    const std::uint64_t now = w.clock_.tick();
    if (now >= f.next_mind_scan) {
        f.next_mind_scan = now + 240;               // ~10 s at 24 t/day? ticks are hours
        scan_minds(w, f, near, pins);
    }
}

// ---- public describe -------------------------------------------------------------
std::string NarrativeComposer::describe(const World& w, EntityId id) {
    return describe_impl(w, id);
}
std::string NarrativeComposer::describe_kind(const World& w, const std::string& kind_name,
                                             MaterialId mat) {
    std::string d = kind_name;
    if (const std::string m = mat_name(w, mat); !m.empty() && d.find(m) == std::string::npos)
        d += " (" + m + ")";
    return d.empty() ? "something" : d;
}

}  // namespace simlab::render
