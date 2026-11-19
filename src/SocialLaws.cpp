// SocialLaws.cpp — Phase 8: Social & Cultural Evolution (TODO §12.3/§12.4/§13).
//
// One law model, seven generic verbs. NOTHING here names a tribe, kingdom,
// religion, quest or technology — institutions are entities with `member_of`
// edges, rules are props on those entities, and knowledge moves only through
// causal events:
//
//   communicate — two actors in range; post-commit the actor's verified
//                 discoveries flow to the partner's mind as unverified claims
//                 (social_from_event), volume scaled by the trust tie.
//   engrave     — carve one Supported+ DiscoveryRecord of the ACTOR's mind
//                 onto a target's `inscription` text prop. The payload is a
//                 causal prop change — Recorded Knowledge is World Truth.
//   read        — study an inscribed entity; the records enter the reader's
//                 mind post-commit, again as unverified claims.
//   found       — spawn a `collective` entity and make the actor its first
//                 member. A group is an entity, not a class.
//   join/leave  — member_of edge changes on a `collective` target.
//   share       — move the actor's whole `quantity` to the target (common
//                 store, gifts, tribute — the meaning is the caller's).
//
// Rule enforcement is NOT in this file: social_from_event watches every
// committed event and applies trust penalties when a `taboo_harm` collective
// is violated. New rules = new props + new checks, never new classes.

#include "simlab/Laws.hpp"
#include "simlab/World.hpp"
#include "simlab/Discovery.hpp"
#include "simlab/Cognition.hpp"
#include "simlab/Causal.hpp"
#include "LawUtil.hpp"
#include <algorithm>
#include <sstream>

namespace simlab {
namespace {

using namespace lawutil;

constexpr std::int64_t kCommCost = 50;     // milliJ per communicate
constexpr std::int64_t kCommTicks = 2;
constexpr std::int64_t kEngraveCost = 100;
constexpr std::int64_t kEngraveTicks = 3;
constexpr std::int64_t kReadCost = 20;
constexpr std::int64_t kReadTicks = 1;
constexpr std::int64_t kFoundCost = 1000;
constexpr std::int64_t kFoundTicks = 4;
constexpr std::int64_t kJoinCost = 10;
constexpr std::int64_t kShareCost = 10;
constexpr std::size_t kMaxInscription = 8000;
constexpr std::int64_t kDefaultCommRange = 400;

PropId comm_range_pid(const World& w) { return pid(w, "communicate_range"); }

std::int64_t comm_range(const World& w, const Entity& actor) {
    const PropId p = comm_range_pid(w);
    return p != kNoProp ? w.prop_int(actor, p, kDefaultCommRange)
                        : kDefaultCommRange;
}

bool alive_pair(const World& w, const InteractionContext& ctx,
                PredictionResult& r, PropId contained) {
    const Entity* a = w.find(ctx.actor);
    if (!a || !a->alive) { r.failure = "no living actor"; return false; }
    if (is_contained(w, *a, contained)) { r.failure = "actor is contained"; return false; }
    return true;
}

const Entity* single_target(const World& w, const InteractionContext& ctx,
                            PredictionResult& r, PropId contained) {
    if (ctx.targets.size() != 1 || ctx.targets.front() == ctx.actor) {
        r.failure = "needs exactly one distinct target"; return nullptr;
    }
    const Entity* t = w.find(ctx.targets.front());
    if (!t || !t->alive) { r.failure = "no living target"; return nullptr; }
    if (is_contained(w, *t, contained)) { r.failure = "target is contained"; return nullptr; }
    return t;
}

bool in_comm_range(const World& w, const Entity& actor, const Entity& target,
                   PredictionResult* r) {
    const std::int64_t range = comm_range(w, actor);
    const std::int64_t d2 = dist2(actor.pos, target.pos);
    if (r) {
        std::int64_t d = 0;
        while (d * d < d2) ++d;
        add_cond(*r, "communicate_range", range, d);
    }
    return d2 <= range * range;
}

// ---- record selection + inscription codec ------------------------------------
// Line format (one per record): R|key|status|efor|eagainst|trials|claim
// claim is sanitized ('|' and newlines -> space) so parsing is unambiguous.
// Phase 12: does this entity's MATERIAL declare the prop? Instance props
// and material defaults both feed prop_of, but rules like "can this surface
// hold a carving" only bite on materials that opt in — legacy blank rocks
// (kNoMaterial) keep the old always-engrave behavior.
bool material_has_prop(const World& w, const Entity& e, PropId p) {
    if (p == kNoProp || e.material == kNoMaterial) return false;
    const auto& defs = w.materials.defs();
    if (e.material - 1 >= defs.size()) return false;
    return defs[e.material - 1].props.count(p) != 0;
}

// Deterministic pseudo-glyph renderer — same codec World::export_lexicon_
// codex uses for raw strings. Display only; the alphabet is seed-derived.
std::string render_glyph_seed(std::uint32_t seed, std::int64_t syllables,
                              std::uint64_t world_seed) {
    if (seed == 0 || syllables <= 0) return "";
    std::string out;
    std::uint64_t h = seed;
    const int nsyl = static_cast<int>(std::clamp<std::int64_t>(syllables, 1, 4));
    for (int i = 0; i < nsyl; ++i) {
        h ^= h >> 13; h *= 0x5DEECE66Dull; h ^= h >> 16;
        const int len = 1 + static_cast<int>((h ^ (h >> 20)) % 2);
        for (int j = 0; j < len; ++j) {
            h ^= h >> 11; h *= 0x2545F4914Full; h ^= h >> 15;
            out.push_back(static_cast<char>('a' +
                static_cast<int>((h ^ world_seed) % 26)));
        }
        if (i + 1 < nsyl) out.push_back('-');
    }
    return out;
}

// LexicalEntries the actor would carve for `concept_key`: same-language
// entries first, then any — capped, in deterministic entity order.
struct CarveEntry { EntityId id; std::string ck; std::uint32_t gs; };
std::vector<CarveEntry> gather_carvings(const World& w, EntityId actor,
                                        const std::string& concept_key) {
    const PropId p_ckey = pid(w, "concept_key");
    const PropId p_lang = pid(w, "language_id");
    const PropId p_gs   = pid(w, "glyph_seed");
    const Entity* ae = w.find(actor);
    const std::int64_t alang = ae && p_lang != kNoProp
        ? w.prop_int(*ae, p_lang, 0) : 0;
    std::vector<CarveEntry> same, other;
    for (const auto& ep : w.entities) {
        const Entity& e = *ep;
        if (!e.alive || e.kind_name != "lexical_entry") continue;
        const Value* ck = p_ckey != kNoProp ? w.prop_of(e, p_ckey) : nullptr;
        if (!ck || ck->type != ValType::Text || ck->s != concept_key) continue;
        const Value* gv = p_gs != kNoProp ? w.prop_of(e, p_gs) : nullptr;
        const Value* lv = p_lang != kNoProp ? w.prop_of(e, p_lang) : nullptr;
        CarveEntry ce;
        ce.id = e.id; ce.ck = ck->s;
        ce.gs = gv && gv->type == ValType::Integer
            ? static_cast<std::uint32_t>(gv->i) : 0;
        const std::int64_t el = lv && lv->type == ValType::Integer ? lv->i : 0;
        (el == alang && alang > 0 ? same : other).push_back(ce);
        if (same.size() + other.size() >= 8) break;
    }
    std::vector<CarveEntry> out = same;
    for (const auto& c : other) { if (out.size() >= 8) break; out.push_back(c); }
    return out;
}


// Familiarity an agent holds for a lexical_entry — the "lex:<id>" belief is
// the knows_word store (beliefs serialize/hash with the mind already).
std::int64_t lex_fam_of(const AgentMind* m, EntityId e) {
    if (!m) return 0;
    const Belief* b = m->find_belief(std::string("lex:") + std::to_string(e));
    return b ? b->confidence : 0;
}

struct ClauseOut { std::int64_t lang; std::vector<EntityId> entries; };

// Compose the CausalClause(s) for a discovery key: decompose into atoms,
// resolve each atom to this community's lexical_entry, and — when the author
// is bilingual — add a second clause in the best-covered other language
// (the Rosetta layer: same meaning, two scripts side by side).
std::vector<ClauseOut> compose_clause(const World& w, const AgentMind* mind,
                                      EntityId actor,
                                      const std::string& key) {
    const PropId p_ckey = pid(w, "concept_key");
    const PropId p_lang = pid(w, "language_id");
    const PropId p_bf   = pid(w, "bilingual_familiarity");
    const Entity* ae = w.find(actor);
    const std::int64_t alang = ae && p_lang != kNoProp
        ? w.prop_int(*ae, p_lang, 0) : 0;
    const std::int64_t bf = ae && p_bf != kNoProp
        ? w.prop_int(*ae, p_bf, 500) : 500;
    const std::vector<AtomRef> ats = decompose_key(key, {});
    if (ats.empty() || p_ckey == kNoProp) return {};
    // entries per atom: atom_key -> lang -> entry ids (sorted, deduped)
    std::map<std::string,
             std::map<std::int64_t, std::vector<EntityId>>> by_atom;
    for (const auto& ep : w.entities) {
        const Entity& e = *ep;
        if (!e.alive || e.kind_name != "lexical_entry") continue;
        const Value* ck = w.prop_of(e, p_ckey);
        const Value* lv = p_lang != kNoProp ? w.prop_of(e, p_lang) : nullptr;
        if (!ck || ck->type != ValType::Text || !lv ||
            lv->type != ValType::Integer) continue;
        by_atom[ck->s][lv->i].push_back(e.id);
    }
    ClauseOut primary; primary.lang = alang;
    std::map<std::int64_t, ClauseOut> seconds;
    for (const AtomRef& at : ats) {
        const auto ai = by_atom.find(at.key);
        if (ai == by_atom.end()) continue;
        // primary: author's own language first, else lowest-lang fallback
        EntityId pick = kNoEntity;
        const auto same = ai->second.find(alang);
        if (same != ai->second.end() && !same->second.empty())
            pick = same->second.front();
        else if (!ai->second.empty()) pick = ai->second.begin()->second.front();
        if (pick != kNoEntity) primary.entries.push_back(pick);
        // secondary: any OTHER language whose entry the author knows well
        for (const auto& [lg, ids] : ai->second) {
            if (lg == primary.lang || lg == 0 || ids.empty()) continue;
            if (lex_fam_of(mind, ids.front()) < bf) continue;
            seconds[lg].lang = lg;
            seconds[lg].entries.push_back(ids.front());
        }
    }
    if (primary.entries.empty()) return {};
    std::vector<ClauseOut> out{primary};
    // Rosetta cap: at most ONE second language — the one covering most atoms
    // (ties -> lowest language_id for determinism).
    if (!seconds.empty()) {
        auto best = seconds.begin();
        for (auto it = seconds.begin(); it != seconds.end(); ++it)
            if (it->second.entries.size() > best->second.entries.size())
                best = it;
        if (!best->second.entries.empty()) out.push_back(best->second);
    }
    return out;
}

std::string sanitize(std::string s) {
    for (char& c : s)
        if (c == '|' || c == '\n' || c == '\r') c = ' ';
    return s;
}

std::string encode_record(const DiscoveryRecord& d) {
    std::ostringstream o;
    o << "R|" << sanitize(d.key) << '|' << static_cast<int>(d.status) << '|'
      << d.evidence_for << '|' << d.evidence_against << '|' << d.trials << '|'
      << sanitize(d.claim);
    return o.str();
}

const DiscoveryRecord* pick_record(const AgentMind& m, const std::string& note,
                                   std::string* why) {
    if (note.empty()) {
        // best verified claim: Verified > Supported, ties -> key order (map is sorted)
        const DiscoveryRecord* best = nullptr;
        int best_rank = -1;
        for (const auto& [k, d] : m.discoveries) {
            const int rank = d.status == DiscoveryStatus::Verified ? 2
                         : d.status == DiscoveryStatus::Supported ? 1 : 0;
            if (rank > best_rank) { best_rank = rank; best = &d; }
        }
        if (!best) *why = "nothing verified to record";
        return best;
    }
    const auto it = m.discoveries.find(note);
    if (it == m.discoveries.end() ||
        (it->second.status != DiscoveryStatus::Supported &&
         it->second.status != DiscoveryStatus::Verified)) {
        *why = "no verified record keyed '" + note + "'";
        return nullptr;
    }
    return &it->second;
}

// ---- per-action predictions ----------------------------------------------------

PredictionResult predict_communicate(const World& w, const InteractionContext& ctx) {
    PredictionResult r; r.law = 8; r.law_name = "social.interaction";
    const PropId contained = pid(w, "contained");
    const PropId energy = pid(w, "energy");
    if (!alive_pair(w, ctx, r, contained)) return r;
    const Entity* actor = w.find(ctx.actor);
    const Entity* t = single_target(w, ctx, r, contained);
    if (!t) return r;
    if (!in_comm_range(w, *actor, *t, &r)) { r.failure = "out of communication range"; return r; }
    if (!charge_energy(*actor, energy, kCommCost, r)) return r;
    r.possible = true; r.ticks = kCommTicks;
    r.notes.push_back("communicate: knowledge may flow (see social ties)");
    return r;
}

PredictionResult predict_engrave(const World& w, const InteractionContext& ctx) {
    PredictionResult r; r.law = 8; r.law_name = "social.interaction";
    const PropId contained = pid(w, "contained");
    const PropId energy = pid(w, "energy");
    const PropId ins = pid(w, "inscription");
    if (ins == kNoProp) { r.failure = "no inscription property"; return r; }
    if (!alive_pair(w, ctx, r, contained)) return r;
    const Entity* actor = w.find(ctx.actor);
    const Entity* t = single_target(w, ctx, r, contained);
    if (!t) return r;
    const PropId reach = pid(w, "reach");
    if (!in_reach(w, *actor, *t, reach, 0, &r)) { r.failure = "target out of reach"; return r; }
    // Phase 12: inscription-capable materials opt in via data props. When
    // the target's material declares them, durability must be > 0 and the
    // carver's effective hardness (own + wielded tools) must meet the req.
    const PropId p_dur = pid(w, "inscription_durability");
    const PropId p_hreq = pid(w, "carving_hardness_req");
    if (material_has_prop(w, *t, p_dur) || material_has_prop(w, *t, p_hreq)) {
        const std::int64_t dur = p_dur != kNoProp ? w.prop_int(*t, p_dur, 0) : 0;
        const std::int64_t hreq = p_hreq != kNoProp ? w.prop_int(*t, p_hreq, 0) : 0;
        if (dur <= 0) { r.failure = "surface cannot hold a carving"; return r; }
        if (hreq > 0) {
            std::int64_t hard = w.prop_int(*actor, pid(w, "hardness"), 0);
            for (const Relation& rel : w.relations_of(ctx.actor, "wielding")) {
                if (rel.a != ctx.actor) continue;
                const Entity* te = w.find(rel.b);
                if (te && te->alive)
                    hard = std::max(hard, w.prop_int(*te, pid(w, "hardness"), 0));
            }
            if (hard < hreq) { r.failure = "tool too soft to carve"; return r; }
        }
    }
    const AgentMind* mind = w.find_mind(ctx.actor);
    if (!mind) { r.failure = "actor has no knowledge to record"; return r; }
    std::string why;
    const DiscoveryRecord* rec = pick_record(*mind, ctx.parameters.note, &why);
    if (!rec) { r.failure = why; return r; }
    const Value* cur = w.prop_of(*t, ins);
    std::string old = (cur && cur->type == ValType::Text) ? cur->s : "";
    const std::string line = encode_record(*rec);
    const std::string neu = old.empty() ? line : old + "\n" + line;
    if (neu.size() > kMaxInscription) { r.failure = "inscription archive full"; return r; }
    if (!charge_energy(*actor, energy, kEngraveCost, r)) return r;
    set_change_v(r, t->id, ins, Value::text(old), Value::text(neu));
    // Phase 12: also raise an Inscription entity bound to the surface —
    // glyph stream + concept keys + provenance + erosion state. Legacy
    // text payload stays for backward compatibility.
    {
        const PropId p_lang = pid(w, "language_id");
        const PropId p_gtxt = pid(w, "glyph_text");
        const PropId p_cktxt = pid(w, "concept_keys");
        const PropId p_auth = pid(w, "inscription_author");
        const PropId p_itk  = pid(w, "inscription_tick");
        const PropId p_dur2 = pid(w, "inscription_durability");
        const PropId p_lvl  = pid(w, "erosion_level");
        const PropId p_ea   = pid(w, "erosion_accum");
        const std::int64_t alang = p_lang != kNoProp && actor
            ? w.prop_int(*actor, p_lang, 0) : 0;
        const auto carv = gather_carvings(w, ctx.actor, rec->key);
        std::string glyphs, ckeys;
        for (const auto& c : carv) {
            if (!glyphs.empty()) glyphs.push_back(' ');
            glyphs += render_glyph_seed(c.gs, 2, w.world_seed);
            if (!ckeys.empty()) ckeys.push_back('|');
            ckeys += c.ck;
        }
        if (glyphs.empty()) {
            // concept has no coined word yet — carve a deterministic
            // fallback glyph so the record is still externalized memory.
            glyphs = render_glyph_seed(
                std::uint32_t(ckeys.empty() ? 0 :
                    (std::uint64_t)rec->key.size() * 2654435761ull), 2,
                w.world_seed);
        }
        SpawnSpec is;
        is.kind = "inscription";
        is.pos = t->pos;
        if (p_lang != kNoProp)  is.props.set(p_lang, Value::integer(alang));
        if (p_gtxt != kNoProp)  is.props.set(p_gtxt, Value::text(glyphs));
        if (p_cktxt != kNoProp) is.props.set(p_cktxt,
            Value::text(ckeys.empty() ? rec->key : ckeys));
        if (p_auth != kNoProp)  is.props.set(p_auth,
            Value::integer(static_cast<std::int64_t>(ctx.actor)));
        if (p_itk != kNoProp)   is.props.set(p_itk,
            Value::integer(static_cast<std::int64_t>(w.clock_.tick())));
        if (p_lvl != kNoProp)   is.props.set(p_lvl, Value::integer(0));
        if (p_ea  != kNoProp)   is.props.set(p_ea,  Value::integer(0));
        if (p_dur2 != kNoProp) {
            const std::int64_t dur = p_dur != kNoProp
                ? w.prop_int(*t, p_dur, 0) : 0;
            is.props.set(p_dur2, Value::integer(dur));
        }
        // portable medium: clay tablets / bone can be carried off and traded —
        // the Inscription rides the surface entity wherever it goes.
        const PropId p_port = pid(w, "portable_surface");
        if (p_port != kNoProp) {
            const std::int64_t pv = material_has_prop(w, *t, p_port)
                ? w.prop_int(*t, p_port, 0) : 0;
            is.props.set(p_port, Value::integer(pv));
        }
        // CausalClause payload: ordered atom-entry ids per language —
        // "lang:id1,id2;lang2:g1,g2" (second clause = Rosetta layer).
        {
            const PropId p_cl  = pid(w, "clause_atoms");
            const PropId p_l2  = pid(w, "language_id2");
            const std::vector<ClauseOut> cls =
                compose_clause(w, mind, ctx.actor, rec->key);
            if (p_cl != kNoProp && !cls.empty()) {
                std::string cs;
                for (const ClauseOut& c : cls) {
                    if (!cs.empty()) cs.push_back(';');
                    cs += std::to_string(c.lang) + ":";
                    for (std::size_t i = 0; i < c.entries.size(); ++i) {
                        if (i) cs.push_back(',');
                        cs += std::to_string(c.entries[i]);
                    }
                }
                is.props.set(p_cl, Value::text(cs));
                if (cls.size() > 1 && p_l2 != kNoProp)
                    is.props.set(p_l2, Value::integer(cls[1].lang));
            }
        }
        is.rel_from.push_back({"inscribed_on", t->id});
        r.spawns.push_back(is);
    }
    r.possible = true; r.ticks = kEngraveTicks;
    r.notes.push_back("engraved " + rec->key);
    return r;
}

PredictionResult predict_read(const World& w, const InteractionContext& ctx) {
    PredictionResult r; r.law = 8; r.law_name = "social.interaction";
    const PropId contained = pid(w, "contained");
    const PropId energy = pid(w, "energy");
    const PropId ins = pid(w, "inscription");
    if (!alive_pair(w, ctx, r, contained)) return r;
    const Entity* actor = w.find(ctx.actor);
    const Entity* t = single_target(w, ctx, r, contained);
    if (!t) return r;
    if (ins == kNoProp) { r.failure = "no inscription property"; return r; }
    const Value* iv = w.prop_of(*t, ins);
    if (!iv || iv->type != ValType::Text || iv->s.empty()) {
        r.failure = "nothing inscribed"; return r;
    }
    if (!in_comm_range(w, *actor, *t, &r)) { r.failure = "out of reading range"; return r; }
    if (!charge_energy(*actor, energy, kReadCost, r)) return r;
    r.possible = true; r.ticks = kReadTicks;
    r.notes.push_back("read " + std::to_string(iv->s.size()) + " bytes of record");
    return r;
}

PredictionResult predict_found(const World& w, const InteractionContext& ctx) {
    PredictionResult r; r.law = 8; r.law_name = "social.interaction";
    const PropId contained = pid(w, "contained");
    const PropId energy = pid(w, "energy");
    const PropId coll = pid(w, "collective");
    const Entity* actor = w.find(ctx.actor);
    if (!actor || !actor->alive) { r.failure = "no living actor"; return r; }
    if (is_contained(w, *actor, contained)) { r.failure = "actor is contained"; return r; }
    if (!charge_energy(*actor, energy, kFoundCost, r)) return r;
    SpawnSpec s;
    s.kind = "collective";
    s.pos = actor->pos;
    if (coll != kNoProp) s.props.set(coll, Value::boolean(true));
    const PropId qty = pid(w, "quantity");
    if (qty != kNoProp) s.props.set(qty, Value::integer(0));   // common store
    s.rel_from.push_back({"member_of", actor->id});
    r.spawns.push_back(s);
    r.possible = true; r.ticks = kFoundTicks;
    r.notes.push_back("charter a collective");
    return r;
}

PredictionResult predict_joinleave(const World& w, const InteractionContext& ctx,
                                   bool join) {
    PredictionResult r; r.law = 8; r.law_name = "social.interaction";
    const PropId contained = pid(w, "contained");
    const PropId energy = pid(w, "energy");
    const PropId coll = pid(w, "collective");
    const PropId reach = pid(w, "reach");
    if (!alive_pair(w, ctx, r, contained)) return r;
    const Entity* actor = w.find(ctx.actor);
    const Entity* t = single_target(w, ctx, r, contained);
    if (!t) return r;
    const Value* cv = coll != kNoProp ? w.prop_of(*t, coll) : nullptr;
    if (!cv || cv->type != ValType::Boolean || !cv->b) {
        r.failure = "target is not a collective"; return r;
    }
    const bool member = w.relations.related(ctx.actor, "member_of", t->id);
    if (join == member) {
        r.failure = join ? "already a member" : "not a member"; return r;
    }
    if (!in_reach(w, *actor, *t, reach, 0, &r)) { r.failure = "group out of reach"; return r; }
    if (!charge_energy(*actor, energy, kJoinCost, r)) return r;
    RelationChange rc;
    rc.a = ctx.actor; rc.type = "member_of"; rc.b = t->id; rc.add = join;
    r.rel_changes.push_back(rc);
    r.possible = true; r.ticks = 1;
    r.notes.push_back(join ? "join collective" : "leave collective");
    return r;
}

PredictionResult predict_share(const World& w, const InteractionContext& ctx) {
    PredictionResult r; r.law = 8; r.law_name = "social.interaction";
    const PropId contained = pid(w, "contained");
    const PropId energy = pid(w, "energy");
    const PropId qty = pid(w, "quantity");
    const PropId reach = pid(w, "reach");
    if (qty == kNoProp) { r.failure = "no quantity property"; return r; }
    if (!alive_pair(w, ctx, r, contained)) return r;
    const Entity* actor = w.find(ctx.actor);
    const Entity* t = single_target(w, ctx, r, contained);
    if (!t) return r;
    const Value* av = w.prop_of(*actor, qty);
    const Value* tv = w.prop_of(*t, qty);
    if (!av || av->type != ValType::Integer || av->i <= 0) {
        r.failure = "actor has nothing to share"; return r;
    }
    if (!tv || tv->type != ValType::Integer) {
        r.failure = "target cannot hold shared goods"; return r;
    }
    const PropertyDef* qd = w.props.find(qty);
    if (qd && qd->has_range && tv->i + av->i > qd->hi) {
        r.failure = "target store would overflow"; return r;
    }
    if (!in_reach(w, *actor, *t, reach, 0, &r)) { r.failure = "target out of reach"; return r; }
    if (!charge_energy(*actor, energy, kShareCost, r)) return r;
    set_change(r, actor->id, qty, av->i, 0);
    set_change(r, t->id, qty, tv->i, tv->i + av->i);
    r.possible = true; r.ticks = 1;
    r.notes.push_back("share " + std::to_string(av->i) + " units");
    return r;
}

// withdraw: draw from a collective's common store. Members in need pull
// clean; everyone else does it under the social eye — trust consequences are
// applied post-commit by social_from_event (theft vs legitimate ration).
PredictionResult predict_withdraw(const World& w, const InteractionContext& ctx) {
    PredictionResult r; r.law = 8; r.law_name = "social.interaction";
    const PropId contained = pid(w, "contained");
    const PropId energy = pid(w, "energy");
    const PropId coll = pid(w, "collective");
    const PropId qty = pid(w, "quantity");
    const PropId reach = pid(w, "reach");
    if (!alive_pair(w, ctx, r, contained)) return r;
    const Entity* actor = w.find(ctx.actor);
    const Entity* t = single_target(w, ctx, r, contained);
    if (!t) return r;
    const Value* cv = coll != kNoProp ? w.prop_of(*t, coll) : nullptr;
    if (!cv || cv->type != ValType::Boolean || !cv->b) {
        r.failure = "target is not a collective"; return r;
    }
    const Value* gq = qty != kNoProp ? w.prop_of(*t, qty) : nullptr;
    if (!gq || gq->type != ValType::Integer || gq->i <= 0) {
        r.failure = "common store is empty"; return r;
    }
    const Value* aq = qty != kNoProp ? w.prop_of(*actor, qty) : nullptr;
    const PropertyDef* qd = qty != kNoProp ? w.props.find(qty) : nullptr;
    if (qd && qd->has_range && aq && aq->type == ValType::Integer &&
        aq->i + gq->i > qd->hi) {
        r.failure = "actor cannot carry the withdrawal"; return r;
    }
    if (!in_reach(w, *actor, *t, reach, 0, &r)) { r.failure = "store out of reach"; return r; }
    if (!charge_energy(*actor, energy, kShareCost, r)) return r;
    set_change(r, t->id, qty, gq->i, 0);
    set_change(r, actor->id, qty, aq ? aq->i : 0, (aq ? aq->i : 0) + gq->i);
    r.possible = true; r.ticks = 1;
    r.notes.push_back("withdraw " + std::to_string(gq->i) + " units");
    return r;
}

}  // namespace

LawId SocialModel::id() const { return 8; }

bool SocialModel::supports(const World& w, const InteractionContext& ctx) const {
    (void)w;
    const std::string& a = ctx.parameters.action;
    if (a == "found") return ctx.targets.empty();
    if (a == "communicate" || a == "engrave" || a == "read" ||
        a == "join" || a == "leave" || a == "share" || a == "withdraw")
        return ctx.targets.size() == 1;
    return false;
}

PredictionResult SocialModel::predict(const World& w,
                                      const InteractionContext& ctx) const {
    const std::string& a = ctx.parameters.action;
    if (a == "communicate") return predict_communicate(w, ctx);
    if (a == "engrave")    return predict_engrave(w, ctx);
    if (a == "read")       return predict_read(w, ctx);
    if (a == "found")      return predict_found(w, ctx);
    if (a == "join")       return predict_joinleave(w, ctx, true);
    if (a == "leave")      return predict_joinleave(w, ctx, false);
    if (a == "share")      return predict_share(w, ctx);
    if (a == "withdraw")   return predict_withdraw(w, ctx);
    PredictionResult r; r.law = id(); r.law_name = name();
    r.failure = "unsupported social action";
    return r;
}

std::unique_ptr<INaturalLawModel> SocialModel::clone() const {
    return std::make_unique<SocialModel>();
}

}  // namespace simlab
