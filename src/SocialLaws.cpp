#include <string_view>
#include <unordered_map>
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
        if (!e.alive || e.kind_name != SIMLAB_KIND("lexical_entry")) continue;
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
        if (!e.alive || e.kind_name != SIMLAB_KIND("lexical_entry")) continue;
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

// ---- Phase 12.5: ritual, consecration & diplomacy ----------------------------

// `language` registry entity for a language_id (0 when the community never
// coalesced — diplomacy may spawn it on first contact).
EntityId diplo_lang_entity(const World& w, std::int64_t lang) {
    const PropId p_ln = pid(w, "language_name");
    if (p_ln == kNoProp || lang <= 0) return kNoEntity;
    for (const EntityId kid_ : w.entities_of_kind(SIMLAB_KIND("language"))) {
        const Entity& e = *w.find(kid_);
        if (e.alive && e.kind_name == SIMLAB_KIND("language") &&
            w.prop_int(e, p_ln, 0) == lang)
            return e.id;
    }
    return kNoEntity;
}

// language_id of an entity (0 = speechless)
std::int64_t sl_lang_of(const World& w, const Entity& e) {
    const PropId p = pid(w, "language_id");
    return p != kNoProp ? w.prop_int(e, p, 0) : 0;
}


// deterministic FNV-1a for seed-derived names (no std::hash portability)
std::uint32_t sl_fnv(const std::string& s) {
    std::uint64_t h = 1469598103934665603ull;
    for (char c : s) { h ^= static_cast<unsigned char>(c);
                       h *= 1099511628211ull; }
    return static_cast<std::uint32_t>(h & 0xFFFFFFFFull);
}

// find the strongest sacred belief key an agent holds ("" = none)
std::string sacred_key_of(const AgentMind* m, std::int64_t min_conf) {
    if (!m) return "";
    std::string best;
    for (const auto& [bk, bv] : m->beliefs) {
        if (bk.rfind("sacred:", 0) != 0) continue;
        if (bv.confidence < min_conf) continue;
        if (best.empty() || bk < best) best = bk;
    }
    return best.empty() ? "" : best.substr(7);
}

// appeasement act for a sacred key: read the shared concept's derived
// action; fall back to what the mind can actually do.
std::string appeasement_for(const World& w, const AgentMind* m,
                            const std::string& skey) {
    const PropId p_skey = pid(w, "sacred_key");
    const PropId p_app  = pid(w, "appeasement_action");
    if (p_skey != kNoProp && p_app != kNoProp && !skey.empty()) {
        for (const auto& ep : w.entities) {
            const Entity& e = *ep;
            if (!e.alive) continue;
            const Value* v = w.prop_of(e, p_skey);
            if (v && v->type == ValType::Text && v->s == skey) {
                const Value* a = w.prop_of(e, p_app);
                if (a && a->type == ValType::Text && !a->s.empty())
                    return a->s;
            }
        }
    }
    if (m && m->known_actions.count("heat")) return "heat";
    if (m && m->known_actions.count("share")) return "share";
    return "heat";
}

// forward decls — defined later in this anonymous namespace
bool econ_transfer(PredictionResult& r, const World& w,
                   EntityId from, EntityId to, MaterialId mat,
                   std::int64_t qty, const Vec2& to_pos);
struct TradeTerms;
TradeTerms parse_trade_note(const World& w, const std::string& note);

PredictionResult predict_perform_ritual(const World& w,
                                        const InteractionContext& ctx) {
    PredictionResult r; r.law = 8; r.law_name = "social.interaction";
    const PropId contained = pid(w, "contained");
    const PropId energy = pid(w, "energy");
    const PropId quantity = pid(w, "quantity");
    const PropId integrity = pid(w, "integrity");
    const PropId temp_p = pid(w, "temperature");
    const PropId st_p = pid(w, "sterilization_temp");
    const PropId cooked_p = pid(w, "cooked_state");
    const PropId strain_p = pid(w, "strain_ref");
    const PropId cont_p = pid(w, "contamination");
    const PropId pl_p = pid(w, "pathogen_load");
    const PropId flamm_p = pid(w, "flammability");
    if (ctx.targets.size() > 8) {
        r.failure = "too many offerings"; return r;
    }
    if (!alive_pair(w, ctx, r, contained)) return r;
    const Entity* actor = w.find(ctx.actor);
    if (!actor) return r;
    // authority to convene: a mandate-holder or a convicted believer
    const AgentMind* am = w.find_mind(ctx.actor);
    const std::string skey = sacred_key_of(am, 400);
    const bool mandate = pol_holds_mandate(w, ctx.actor);
    if (skey.empty() && !mandate) {
        r.failure = "no sacred conviction or mandate to call a rite";
        return r;
    }
    if (!charge_energy(*actor, energy, 150, r)) return r;
    // offerings are consumed by the rite — quantity drains, bare relics
    // break. Their worth is the convener's own marginal valuation (Nhịp 2):
    // a costly sacrifice convinces, a token one barely registers.
    std::int64_t offered = 0;
    for (EntityId oid : ctx.targets) {
        const Entity* o = w.find(oid);
        if (!o || !o->alive || is_contained(w, *o, contained)) continue;
        if (quantity != kNoProp && o->props.get(quantity)) {
            const std::int64_t q = w.prop_int(*o, quantity, 0);
            if (q > 0 && o->material != kNoMaterial)
                offered += econ_total_value(w, ctx.actor, o->material) * q;
            if (q > 0)
                set_change(r, oid, quantity, q, 0);
        } else if (integrity != kNoProp) {
            const std::int64_t ig = w.prop_int(*o, integrity, 500);
            set_change(r, oid, integrity, ig, 0);
        }
    }
    // fire appeasement: offerings that burn, or a mind that knows "heat",
    // raise a real hearth — entities in the ring conduct the heat and any
    // that cross their material's sterilization_temp are cleansed by the
    // SAME rule ThermalModel applies (no ritual exemption).
    const std::string appease = appeasement_for(w, am, skey);
    bool fire = appease == "heat";
    if (!fire && flamm_p != kNoProp) {
        for (EntityId oid : ctx.targets) {
            const Entity* o = w.find(oid);
            if (o && w.prop_int(*o, flamm_p, 0) > 300) { fire = true; break; }
        }
    }
    if (fire && temp_p != kNoProp) {
        constexpr std::int64_t kRing = 200, kHeat = 1200;
        for (const auto& ep : w.entities) {
            const Entity& o = *ep;
            if (!o.alive || o.id == ctx.actor) continue;
            const Value* tv = o.props.get(temp_p);
            if (!tv || tv->type != ValType::Integer) continue;
            const std::int64_t dx = o.pos.x - actor->pos.x;
            const std::int64_t dy = o.pos.y - actor->pos.y;
            if (dx * dx + dy * dy > kRing * kRing) continue;
            const std::int64_t nw = std::min<std::int64_t>(10000,
                tv->i + kHeat);
            set_change(r, o.id, temp_p, tv->i, nw);
            const std::int64_t st = st_p != kNoProp
                ? w.prop_int(o, st_p, 0) : 0;
            if (st > 0 && nw >= st) {
                if (cooked_p != kNoProp &&
                    w.prop_int(o, cooked_p, 0) != 1000)
                    set_change(r, o.id, cooked_p,
                        w.prop_int(o, cooked_p, 0), 1000);
                if (strain_p != kNoProp &&
                    w.prop_int(o, strain_p, 0) != 0)
                    set_change(r, o.id, strain_p,
                        w.prop_int(o, strain_p, 0), 0);
                if (cont_p != kNoProp &&
                    w.prop_int(o, cont_p, 0) != 0)
                    set_change(r, o.id, cont_p,
                        w.prop_int(o, cont_p, 0), 0);
                r.notes.push_back("rite-fire sterilized " +
                    std::to_string(o.id));
            }
        }
    }
    // the rite legitimizes: shared awe lifts the convener's standing
    const PropId p_leg = pid(w, "social_legitimacy");
    const std::int64_t awe = std::clamp<std::int64_t>(80 + offered / 50, 80, 300);
    if (p_leg != kNoProp)
        set_change(r, ctx.actor, p_leg, pol_legitimacy(w, ctx.actor),
                   std::min<std::int64_t>(2000,
                       pol_legitimacy(w, ctx.actor) + awe));
    r.possible = true; r.ticks = 6;
    r.notes.push_back("rite performed appeasement=" + appease +
                      (fire ? " fire" : "") + " offered=" +
                      std::to_string(offered) + " awe=" + std::to_string(awe));
    return r;
}

PredictionResult predict_consecrate(const World& w,
                                    const InteractionContext& ctx) {
    PredictionResult r; r.law = 8; r.law_name = "social.interaction";
    const PropId contained = pid(w, "contained");
    const PropId energy = pid(w, "energy");
    const PropId reach = pid(w, "reach");
    const PropId ins = pid(w, "inscription");
    if (ctx.targets.size() != 1) {
        r.failure = "consecrate needs one surface"; return r;
    }
    if (!alive_pair(w, ctx, r, contained)) return r;
    const Entity* actor = w.find(ctx.actor);
    const Entity* t = w.find(ctx.targets.front());
    if (!t || !t->alive || is_contained(w, *t, contained)) {
        r.failure = "no living target"; return r;
    }
    if (!in_reach(w, *actor, *t, reach, 0, &r)) {
        r.failure = "surface out of reach"; return r;
    }
    // only inscription-capable materials hold a sacred carving
    const PropId p_dur = pid(w, "inscription_durability");
    const PropId p_hreq = pid(w, "carving_hardness_req");
    if (material_has_prop(w, *t, p_dur) ||
        material_has_prop(w, *t, p_hreq)) {
        const std::int64_t dur = p_dur != kNoProp
            ? w.prop_int(*t, p_dur, 0) : 0;
        if (dur <= 0) { r.failure = "surface cannot hold a carving"; return r; }
    }
    const AgentMind* am = w.find_mind(ctx.actor);
    const std::string skey = sacred_key_of(am, 600);
    if (skey.empty()) {
        r.failure = "no sacred conviction to carve"; return r;
    }
    if (!charge_energy(*actor, energy, 120, r)) return r;
    // copy the shared concept's derived agent/appeasement onto the carving
    const PropId p_skey = pid(w, "sacred_key");
    const PropId p_sag = pid(w, "sacred_agent");
    const PropId p_app = pid(w, "appeasement_action");
    std::string agent, appease = appeasement_for(w, am, skey);
    for (const auto& ep : w.entities) {
        const Entity& e = *ep;
        if (!e.alive || p_skey == kNoProp) continue;
        const Value* v = w.prop_of(e, p_skey);
        if (v && v->type == ValType::Text && v->s == skey) {
            const Value* av = p_sag != kNoProp ? w.prop_of(e, p_sag)
                                               : nullptr;
            if (av && av->type == ValType::Text) agent = av->s;
            const Value* ap = p_app != kNoProp ? w.prop_of(e, p_app)
                                               : nullptr;
            if (ap && ap->type == ValType::Text && !ap->s.empty())
                appease = ap->s;
            break;
        }
    }
    if (agent.empty()) {
        agent = "the-" + std::to_string(sl_fnv("agent|" + skey) % 997);
    }
    SpawnSpec is;
    is.kind = "inscription";
    is.pos = t->pos;
    const PropId p_lang = pid(w, "language_id");
    if (p_lang != kNoProp)
        is.props.set(p_lang, Value::integer(sl_lang_of(w, *actor)));
    if (p_skey != kNoProp) is.props.set(p_skey, Value::text(skey));
    if (p_sag != kNoProp) is.props.set(p_sag, Value::text(agent));
    if (p_app != kNoProp) is.props.set(p_app, Value::text(appease));
    const PropId p_ck = pid(w, "concept_keys");
    if (p_ck != kNoProp) is.props.set(p_ck, Value::text(skey));
    const PropId p_gt = pid(w, "glyph_text");
    if (p_gt != kNoProp)
        is.props.set(p_gt, Value::text(skey + "->" + appease));
    const PropId p_it = pid(w, "inscription_tick");
    if (p_it != kNoProp)
        is.props.set(p_it, Value::integer(
            static_cast<std::int64_t>(w.clock_.tick())));
    const PropId p_ia = pid(w, "inscription_author");
    if (p_ia != kNoProp)
        is.props.set(p_ia, Value::integer(static_cast<std::int64_t>(ctx.actor)));
    // the sacred key IS a discovery key: carve its CausalClause the same way
    // `engrave` does (bilingual authors add the Rosetta layer), so the
    // shrine stone is readable — [cause atoms] beside the derived rite.
    {
        const PropId p_cl = pid(w, "clause_atoms");
        const PropId p_l2 = pid(w, "language_id2");
        const std::vector<ClauseOut> cls = compose_clause(w, am, ctx.actor, skey);
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
    r.possible = true; r.ticks = 3;
    r.notes.push_back("consecrated " + skey + " agent=" + agent);
    return r;
}

PredictionResult predict_diplomatic_parley(const World& w,
                                            const InteractionContext& ctx) {
    PredictionResult r; r.law = 8; r.law_name = "social.interaction";
    const PropId contained = pid(w, "contained");
    const PropId energy = pid(w, "energy");
    const PropId p_poss = pid(w, "possessed_by");
    const PropId p_lang = pid(w, "language_id");
    if (ctx.targets.size() != 1) {
        r.failure = "parley needs one envoy counterpart"; return r;
    }
    if (!alive_pair(w, ctx, r, contained)) return r;
    const Entity* actor = w.find(ctx.actor);
    const Entity* t = w.find(ctx.targets.front());
    if (!t || !t->alive || is_contained(w, *t, contained)) {
        r.failure = "no living counterpart"; return r;
    }
    if (!in_comm_range(w, *actor, *t, &r)) {
        r.failure = "counterpart out of range"; return r;
    }
    const std::int64_t la = p_lang != kNoProp ? w.prop_int(*actor, p_lang, 0) : 0;
    const std::int64_t lb = p_lang != kNoProp ? w.prop_int(*t, p_lang, 0) : 0;
    if (la <= 0 || lb <= 0) { r.failure = "no speech community"; return r; }
    if (la == lb) { r.failure = "same community — not diplomacy"; return r; }
    if (!charge_energy(*actor, energy, 80, r)) return r;

    const std::string& note = ctx.parameters.note;
    // Phase 12.6 peace: the DEFEATED side's bilingual envoy sues for peace —
    // note "peace:<enclosure_id|0>:<material|->:<qty>". Defeat is World
    // Truth: two thirds of the actor's tongue marching on the other have
    // routed or fallen, or the depot feeding them is breached. The Rosetta carving
    // records cession + reparations; ledgers drop hostility; armies stand down.
    if (note.rfind("peace:", 0) == 0) {
        const PropId p_wt = pid(w, "war_target"), p_ro = pid(w, "routed"),
                     p_sd = pid(w, "supply_depot_link"), p_bar = pid(w, "access_barrier"),
                     p_int = pid(w, "integrity");
        std::int64_t soldiers = 0, routed = 0; bool depot_fell = false;
        for (const auto& ep : w.entities) {
            const Entity& e = *ep;
            if (p_wt == kNoProp || w.prop_int(e, p_wt, 0) != lb) continue;
            if (w.prop_int(e, p_lang, 0) != la) continue;
            ++soldiers;                       // the fallen count as lost, too
            if (!e.alive || (p_ro != kNoProp && w.prop_int(e, p_ro, 0) != 0)) ++routed;
            const std::int64_t dp = p_sd != kNoProp ? w.prop_int(e, p_sd, 0) : 0;
            if (const Entity* de = dp > 0 ? w.find(static_cast<EntityId>(dp)) : nullptr)
                if (!de->alive || (w.prop_int(*de, p_bar, 0) == 0 && w.prop_int(*de, p_int, 0) < 300))
                    depot_fell = true;
        }
        add_cond(r, "soldiers_routed", routed, soldiers);
        // the host is broken when two thirds of it have fled or fallen
        if (!((soldiers > 0 && routed * 3 >= soldiers * 2) || depot_fell)) {
            r.failure = "not defeated — no cause to sue for peace"; return r;
        }
        const PropId p_bf = pid(w, "bilingual_familiarity");
        if ((p_bf != kNoProp ? w.prop_int(*actor, p_bf, 500) : 500) < 600) {
            r.failure = "envoy is not bilingual"; return r;
        }
        std::int64_t enc = 0, qty = 0; std::string mat;
        {
            const std::string rest = note.substr(6);
            const std::size_t c1 = rest.find(':'), c2 = c1 == std::string::npos ? c1 : rest.find(':', c1 + 1);
            if (c1 == std::string::npos || c2 == std::string::npos) { r.failure = "malformed peace terms"; return r; }
            try { enc = std::stoll(rest.substr(0, c1)); qty = std::stoll(rest.substr(c2 + 1)); }
            catch (...) { r.failure = "malformed peace terms"; return r; }
            mat = rest.substr(c1 + 1, c2 - c1 - 1);
        }
        if (enc > 0) {
            const Entity* ee = w.find(static_cast<EntityId>(enc));
            if (!ee || !ee->alive || ee->kind_name != SIMLAB_KIND("enclosure")) {
                r.failure = "ceded enclosure does not stand"; return r;
            }
        }
        // Rosetta peace stone: both tongues, the envoy's own words
        SpawnSpec is;
        is.kind = "inscription"; is.pos = actor->pos;
        is.props.set(p_lang, Value::integer(la));
        if (const PropId p_l2 = pid(w, "language_id2"); p_l2 != kNoProp)
            is.props.set(p_l2, Value::integer(lb));
        if (const PropId p_tc = pid(w, "treaty_clause"); p_tc != kNoProp)
            is.props.set(p_tc, Value::text("peace"));
        if (const PropId p_gt = pid(w, "glyph_text"); p_gt != kNoProp)
            is.props.set(p_gt, Value::text("peace|cede:" + std::to_string(enc) +
                                           "|reparation:" + mat + "x" + std::to_string(qty)));
        if (const PropId p_it = pid(w, "inscription_tick"); p_it != kNoProp)
            is.props.set(p_it, Value::integer(static_cast<std::int64_t>(w.clock_.tick())));
        if (const PropId p_ia = pid(w, "inscription_author"); p_ia != kNoProp)
            is.props.set(p_ia, Value::integer(static_cast<std::int64_t>(ctx.actor)));
        if (const PropId p_cl = pid(w, "clause_atoms"); p_cl != kNoProp) {
            const AgentMind* em = w.find_mind(ctx.actor);
            std::vector<EntityId> wa, wb;
            if (em) for (const auto& [bk, bv] : em->beliefs) {
                if (bk.rfind("lex:", 0) != 0 || bv.confidence < 500) continue;
                EntityId eid = kNoEntity;
                try { eid = static_cast<EntityId>(std::stoull(bk.substr(4))); } catch (...) { continue; }
                const Entity* le = w.find(eid);
                if (!le || !le->alive || le->kind_name != SIMLAB_KIND("lexical_entry")) continue;
                const std::int64_t ll = w.prop_int(*le, p_lang, 0);
                if (ll == la && wa.size() < 4) wa.push_back(eid);
                if (ll == lb && wb.size() < 4) wb.push_back(eid);
            }
            if (!wa.empty() && !wb.empty()) {
                std::string cs = std::to_string(la) + ":";
                for (std::size_t i = 0; i < wa.size(); ++i) cs += (i ? "," : "") + std::to_string(wa[i]);
                cs += ";" + std::to_string(lb) + ":";
                for (std::size_t i = 0; i < wb.size(); ++i) cs += (i ? "," : "") + std::to_string(wb[i]);
                is.props.set(p_cl, Value::text(cs));
            }
        }
        r.spawns.push_back(is);
        // reparations: a World-Truth debt the loser owes the victor
        if (qty > 0 && mat != "-") {
            const MaterialId gm = w.materials.id_of(mat);
            if (gm == kNoMaterial) { r.failure = "unknown reparation material"; return r; }
            SpawnSpec dbt;
            dbt.kind = "debt_obligation"; dbt.pos = actor->pos;
            dbt.props.set(pid(w, "debt_value"), Value::integer(
                std::max<std::int64_t>(1, econ_total_value(w, t->id, gm) * qty)));
            dbt.props.set(pid(w, "due_tick"), Value::integer(
                static_cast<std::int64_t>(w.clock_.tick()) + 2000));
            dbt.props.set(pid(w, "debtor_id"), Value::integer(static_cast<std::int64_t>(ctx.actor)));
            dbt.props.set(pid(w, "creditor_id"), Value::integer(static_cast<std::int64_t>(t->id)));
            dbt.props.set(pid(w, "debt_status"), Value::integer(0));
            r.spawns.push_back(dbt);
        }
        // ledgers: hostility ends, trust restarts at 0 on both sides
        const PropId p_dt = pid(w, "diplo_table");
        for (std::int64_t side : {la, lb}) {
            const std::int64_t peer = side == la ? lb : la;
            const EntityId le = diplo_lang_entity(w, side);
            const Entity* lee = le != kNoEntity ? w.find(le) : nullptr;
            if (!lee || p_dt == kNoProp) continue;
            const Value* cur = w.prop_of(*lee, p_dt);
            const std::string before = cur && cur->type == ValType::Text ? cur->s : "";
            std::vector<DiploEntry> v = diplo_parse(before);
            bool found = false;
            for (DiploEntry& d : v) if (d.peer == peer) { d.trust = 0; d.hostile = false; d.free_trade = false; found = true; }
            if (!found) { DiploEntry d; d.peer = peer; v.push_back(d); }
            const std::string after = diplo_render(v);
            if (after != before) set_change_v(r, le, p_dt, Value::text(before), Value::text(after));
        }
        // armies stand down on both sides
        for (const auto& ep : w.entities) {
            const Entity& e = *ep;
            if (!e.alive || p_wt == kNoProp) continue;
            const std::int64_t wt = w.prop_int(e, p_wt, 0);
            const std::int64_t el = w.prop_int(e, p_lang, 0);
            if (!((el == la && wt == lb) || (el == lb && wt == la))) continue;
            set_change(r, e.id, p_wt, wt, 0);
            if (p_ro != kNoProp && w.prop_int(e, p_ro, 0) != 0)
                set_change(r, e.id, p_ro, w.prop_int(e, p_ro, 0), 0);
        }
        r.possible = true; r.ticks = 5;
        r.notes.push_back("PEACE enc=" + std::to_string(enc) + " to=" + std::to_string(t->id) +
                          " la=" + std::to_string(la) + " lb=" + std::to_string(lb));
        return r;
    }
    if (note == "treaty") {
        // peace requires earned trust AND a mind that bridges the tongues
        if (diplo_trust(w, la, lb) < 500) {
            r.failure = "trust too shallow for a treaty"; return r;
        }
        const PropId p_bf = pid(w, "bilingual_familiarity");
        const std::int64_t bf = p_bf != kNoProp
            ? w.prop_int(*actor, p_bf, 500) : 500;
        if (bf < 600) { r.failure = "envoy is not bilingual"; return r; }
        const std::int64_t trust = diplo_trust(w, la, lb);
        const std::string clause =
            trust >= 700 ? "free_trade" : "non_aggression";
        // bilingual Rosetta treaty: primary clause + second language layer
        SpawnSpec is;
        is.kind = "inscription";
        is.pos = actor->pos;
        is.props.set(p_lang, Value::integer(la));
        const PropId p_l2 = pid(w, "language_id2");
        if (p_l2 != kNoProp) is.props.set(p_l2, Value::integer(lb));
        const PropId p_tc = pid(w, "treaty_clause");
        if (p_tc != kNoProp)
            is.props.set(p_tc, Value::text(clause));
        const PropId p_ck = pid(w, "concept_keys");
        if (p_ck != kNoProp)
            is.props.set(p_ck, Value::text("treaty:" + clause));
        const PropId p_it = pid(w, "inscription_tick");
        if (p_it != kNoProp)
            is.props.set(p_it, Value::integer(
                static_cast<std::int64_t>(w.clock_.tick())));
        const PropId p_ia = pid(w, "inscription_author");
        if (p_ia != kNoProp)
            is.props.set(p_ia, Value::integer(static_cast<std::int64_t>(ctx.actor)));
        // Rosetta payload: the bilingual envoy's confident words of BOTH
        // tongues side by side ("la:ids;lb:ids") — a reader of either
        // language parses one clause and picks up the other (Nhịp 1B read).
        {
            const PropId p_cl = pid(w, "clause_atoms");
            const AgentMind* em = w.find_mind(ctx.actor);
            std::vector<EntityId> wa, wb;
            if (em && p_cl != kNoProp) {
                for (const auto& [bk, bv] : em->beliefs) {
                    if (bk.rfind("lex:", 0) != 0 || bv.confidence < 500) continue;
                    EntityId eid = kNoEntity;
                    try { eid = static_cast<EntityId>(std::stoull(bk.substr(4))); }
                    catch (...) { continue; }
                    const Entity* le = w.find(eid);
                    if (!le || !le->alive || le->kind_name != SIMLAB_KIND("lexical_entry")) continue;
                    const std::int64_t ll = w.prop_int(*le, p_lang, 0);
                    if (ll == la && wa.size() < 4) wa.push_back(eid);
                    if (ll == lb && wb.size() < 4) wb.push_back(eid);
                }
            }
            if (!wa.empty() && !wb.empty()) {
                std::string cs = std::to_string(la) + ":";
                for (std::size_t i = 0; i < wa.size(); ++i)
                    cs += (i ? "," : "") + std::to_string(wa[i]);
                cs += ";" + std::to_string(lb) + ":";
                for (std::size_t i = 0; i < wb.size(); ++i)
                    cs += (i ? "," : "") + std::to_string(wb[i]);
                is.props.set(p_cl, Value::text(cs));
            }
        }
        r.spawns.push_back(is);
        // treaty effect: both ledgers mark free passage when the clause says so
        if (clause == "free_trade") {
            for (std::int64_t side : {la, lb}) {
                const std::int64_t peer = side == la ? lb : la;
                const EntityId le = diplo_lang_entity(w, side);
                if (le == kNoEntity) continue;
                const Entity* le_e = w.find(le);
                if (!le_e) continue;
                const PropId p_dt = pid(w, "diplo_table");
                const Value* cur = p_dt != kNoProp
                    ? w.prop_of(*le_e, p_dt) : nullptr;
                const std::string before =
                    cur && cur->type == ValType::Text ? cur->s : "";
                const std::string after = diplo_mark_trade(before, peer);
                if (after != before)
                    set_change_v(r, le, p_dt, Value::text(before),
                                 Value::text(after));
            }
        }
        r.possible = true; r.ticks = 5;
        r.notes.push_back("treaty carved: " + clause);
        return r;
    }

    // gift parley: note "gift:<material>:<qty>" moves real goods and the
    // ledgers move by the gift's marginal worth — economics IS diplomacy.
    std::int64_t delta = 10;   // mere contact warms relations slightly
    if (note.rfind("gift:", 0) == 0) {
        const std::string rest = note.substr(5);
        const std::size_t colon = rest.rfind(':');
        if (colon == std::string::npos) {
            r.failure = "malformed gift terms"; return r;
        }
        const std::string mname = rest.substr(0, colon);
        std::int64_t qty = 0;
        try { qty = std::stoll(rest.substr(colon + 1)); }
        catch (...) { r.failure = "malformed gift qty"; return r; }
        if (qty <= 0) { r.failure = "gift must be positive"; return r; }
        const MaterialId gm = w.materials.id_of(mname);
        if (gm == kNoMaterial) { r.failure = "unknown gift material"; return r; }
        const EntityId stack = econ_held_stack(w, ctx.actor, gm, qty);
        if (stack == kNoEntity) {
            r.failure = "envoy lacks the gift"; return r;
        }
        const std::int64_t val = econ_total_value(w, ctx.actor, gm) * qty;
        if (!econ_transfer(r, w, ctx.actor, t->id, gm, qty, t->pos)) {
            r.failure = "gift transfer failed"; return r;
        }
        delta = std::clamp<std::int64_t>(val / 20, 50, 400);
        r.notes.push_back("gift " + mname + " x" + std::to_string(qty) +
                          " worth " + std::to_string(val));
    }
    // symmetric ledger write on both communities' language entities
    {
        const PropId p_dt = pid(w, "diplo_table");
        for (std::int64_t side : {la, lb}) {
            const std::int64_t peer = side == la ? lb : la;
            EntityId le = diplo_lang_entity(w, side);
            if (le == kNoEntity) {
                SpawnSpec ls;
                ls.kind = "language"; ls.pos = actor->pos;
                const PropId p_ln = pid(w, "language_name");
                if (p_ln != kNoProp) ls.props.set(p_ln, Value::integer(side));
                if (p_dt != kNoProp)
                    ls.props.set(p_dt, Value::text(std::to_string(peer) +
                        ":" + std::to_string(std::clamp<std::int64_t>(
                            delta, -1000, 1000)) + ";"));
                r.spawns.push_back(ls);
                continue;
            }
            const Entity* le_e = w.find(le);
            if (!le_e) continue;
            const Value* cur = p_dt != kNoProp
                ? w.prop_of(*le_e, p_dt) : nullptr;
            const std::string before =
                cur && cur->type == ValType::Text ? cur->s : "";
            const std::string after = diplo_bump(before, peer, delta);
            if (after != before)
                set_change_v(r, le, p_dt, Value::text(before),
                             Value::text(after));
        }
    }
    r.possible = true; r.ticks = 3;
    r.notes.push_back("parley delta=" + std::to_string(delta));
    return r;
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
    // Phase 12.5 oral tradition: every word the speaker actually knows gets
    // another transmission — World Truth on the entry, ledgered here.
    // Past the meter threshold a well-rhymed clause crystallizes as
    // proverb/epic: oral_genre + poetic_cadence become World Truth too.
    {
        const PropId p_tcount = pid(w, "transmission_count");
        const PropId p_cad = pid(w, "poetic_cadence");
        const PropId p_genre = pid(w, "oral_genre");
        const PropId p_ckey = pid(w, "concept_key");
        const PropId p_lang = pid(w, "language_id");
        const AgentMind* am = w.find_mind(ctx.actor);
        std::size_t bumped = 0;
        bool recited_classic = false;
        if (am && p_tcount != kNoProp) {
            for (const auto& [bk, bv] : am->beliefs) {
                if (bumped >= 8) break;
                if (bk.rfind("lex:", 0) != 0 || bv.confidence < 500)
                    continue;
                EntityId eid = kNoEntity;
                try { eid = static_cast<EntityId>(
                    std::stoull(bk.substr(4))); } catch (...) { continue; }
                const Entity* le = w.find(eid);
                if (!le || !le->alive ||
                    le->kind_name != SIMLAB_KIND("lexical_entry")) continue;
                const std::int64_t cur = w.prop_int(*le, p_tcount, 0);
                const std::int64_t nxt = std::min<std::int64_t>(100000,
                    cur + 1);
                set_change(r, eid, p_tcount, cur, nxt);
                ++bumped;
                if (p_genre != kNoProp) {
                    const Value* g0 = w.prop_of(*le, p_genre);
                    if (g0 && g0->type == ValType::Text && !g0->s.empty())
                        recited_classic = true;
                }
                // crystallization: meter + repetition make it unforgetable
                if (nxt >= 150 && p_genre != kNoProp) {
                    const std::int64_t cad = oral_cadence_of(w, eid);
                    if (cad >= 500) {
                        const std::int64_t ocur =
                            p_cad != kNoProp ? w.prop_int(*le, p_cad, 0) : 0;
                        if (p_cad != kNoProp && ocur != cad)
                            set_change(r, eid, p_cad, ocur, cad);
                        const Value* gv = w.prop_of(*le, p_genre);
                        const std::string gcur =
                            gv && gv->type == ValType::Text ? gv->s : "";
                        if (gcur.empty()) {
                            // proverb = short clause, epic = long chain
                            std::int64_t sibs = 0;
                            if (p_ckey != kNoProp && p_lang != kNoProp) {
                                const Value* ck = w.prop_of(*le, p_ckey);
                                const Value* lv = w.prop_of(*le, p_lang);
                                if (ck && ck->type == ValType::Text &&
                                    lv && lv->type == ValType::Integer) {
                                    for (const auto& ep2 : w.entities) {
                                        const Entity& o = *ep2;
                                        if (!o.alive ||
                                            o.kind_name != SIMLAB_KIND("lexical_entry") ||
                                            o.id == eid) continue;
                                        const Value* ok =
                                            w.prop_of(o, p_ckey);
                                        const Value* ol =
                                            w.prop_of(o, p_lang);
                                        if (ok && ok->type == ValType::Text &&
                                            ok->s == ck->s && ol &&
                                            ol->type == ValType::Integer &&
                                            ol->i == lv->i) {
                                            ++sibs;
                                            if (sibs > 3) break;
                                        }
                                    }
                                }
                            }
                            set_change_v(r, eid, p_genre,
                                Value::text(gcur),
                                Value::text(sibs <= 3 ? "proverb" : "epic"));
                        }
                    }
                }
            }
        }
        (void)p_lang;
        // reciting a crystallized proverb/epic binds the hearers: shared
        // memory of the community's own history lifts the reciter's standing
        const PropId p_leg = pid(w, "social_legitimacy");
        if (recited_classic && p_leg != kNoProp)
            set_change(r, ctx.actor, p_leg, pol_legitimacy(w, ctx.actor),
                std::min<std::int64_t>(2000, pol_legitimacy(w, ctx.actor) + 15));
    }
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
    std::string neu = old.empty() ? line : old + "\n" + line;
    // Phase 19: the scholar's quantitative cognition rides the same stone —
    // math tiers, phenomena and proven laws as M|/P|/Q| lines.
    {
        const std::string qt = quantum_tables(*mind);
        if (!qt.empty()) neu += "\n" + qt;
    }
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
        // Phase 12.3: a legitimate, abstract-minded authority may codify
        // the community's dominant taboo as a Law Inscription — the
        // carving carries [action]->[sanction] for any literate reader.
        {
            const PropId p_la = pid(w, "law_action");
            const PropId p_ls = pid(w, "law_sanction");
            const PropId p_lv = pid(w, "law_aversion");
            std::string lact; std::int64_t lav = 0;
            const MindParams lmp = mind_params(w, *actor);
            if (p_la != kNoProp && p_ls != kNoProp && p_lv != kNoProp &&
                pol_legitimacy(w, ctx.actor) >= 700 &&
                lmp.abstraction_gain >= 1100 &&
                pol_dominant_norm(w, ctx.actor, &lact, &lav)) {
                is.props.set(p_la, Value::text(lact));
                is.props.set(p_ls, Value::text(
                    lav >= 800 ? "ostracism" : "restitution"));
                is.props.set(p_lv, Value::integer(lav));
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
    // Phase 18 domestication: sharing with a WILD tameable animal is
    // feeding, not storage — the animal eats a meal (wildness falls by its
    // own tameability) and, once wildness drops to bond_threshold, forms a
    // `bonded` relation with the feeder. A bonded animal instead uses the
    // normal storage path — that is how pack cargo gets loaded.
    const PropId p_tame = pid(w, "tameability");
    const PropId p_wild = pid(w, "wildness");
    const PropId p_bth = pid(w, "bond_threshold");
    const bool feed = p_tame != kNoProp && p_wild != kNoProp &&
        w.prop_int(*t, p_tame, 0) > 0 &&
        w.prop_int(*t, p_wild, 1000) >
            (p_bth != kNoProp ? w.prop_int(*t, p_bth, 300) : 300);
    // Phase 19: parameters.force optionally caps the transferred amount —
    // a quantitative-law planner shares the measured safe load, not the
    // whole pack. 0 = legacy behavior (transfer everything).
    const std::int64_t amount = ctx.parameters.force > 0
        ? std::min<std::int64_t>(av->i, ctx.parameters.force) : av->i;
    if (!tv || tv->type != ValType::Integer) {
        if (!feed) { r.failure = "target cannot hold shared goods"; return r; }
    } else if (!feed) {
        const PropertyDef* qd = w.props.find(qty);
        std::int64_t hi = qd && qd->has_range ? qd->hi : tv->i + amount;
        // pack_load_bonus: a pack animal's declared cargo multiplier —
        // the bonus is data on the species, not a role assignment.
        const PropId p_pack = pid(w, "pack_load_bonus");
        if (p_pack != kNoProp && qd && qd->has_range) {
            const std::int64_t bonus = w.prop_int(*t, p_pack, 0);
            if (bonus > 0) hi = hi + hi * bonus / 1000;
        }
        if (qd && qd->has_range && tv->i + amount > hi) {
            r.failure = "target store would overflow"; return r;
        }
    }
    if (!in_reach(w, *actor, *t, reach, 0, &r)) { r.failure = "target out of reach"; return r; }
    if (pol_ostracism(w, ctx.actor, t->id) >= 500) {
        r.failure = "ostracized by the community"; return r;
    }
    if (!charge_energy(*actor, energy, kShareCost, r)) return r;
    if (feed) {
        const std::int64_t eaten = std::min<std::int64_t>(av->i, 20);
        set_change(r, actor->id, qty, av->i, av->i - eaten);
        const PropId p_hun2 = pid(w, "hunger");
        if (p_hun2 != kNoProp) {
            const std::int64_t h0 = w.prop_int(*t, p_hun2, 500);
            set_change(r, t->id, p_hun2, h0,
                       std::max<std::int64_t>(0, h0 - 400));
        }
        const std::int64_t w0 = w.prop_int(*t, p_wild, 1000);
        const std::int64_t tame = w.prop_int(*t, p_tame, 0);
        const std::int64_t w1 =
            std::max<std::int64_t>(0, w0 - tame * eaten / 100);
        set_change(r, t->id, p_wild, w0, w1);
        const std::int64_t bth =
            p_bth != kNoProp ? w.prop_int(*t, p_bth, 300) : 300;
        if (w1 <= bth)
            r.rel_changes.push_back({t->id, "bonded", ctx.actor, true});
        r.possible = true; r.ticks = 1;
        r.notes.push_back("feed " + std::to_string(eaten) +
                          " wildness->" + std::to_string(w1) +
                          (w1 <= bth ? " BOND" : ""));
        return r;
    }
    set_change(r, actor->id, qty, av->i, av->i - amount);
    set_change(r, t->id, qty, tv->i, tv->i + amount);
    // Phase 12.3: a mandated leader dispensing the public pool in scarcity
    // converts charity into legitimacy and stamps the relief tick.
    const PropId p_pool = pid(w, "public_pool");
    const PropId p_leg = pid(w, "social_legitimacy");
    const PropId p_lrt = pid(w, "last_relief_tick");
    const PropId p_hun = pid(w, "hunger");
    if (p_pool != kNoProp && p_leg != kNoProp &&
        pol_holds_mandate(w, ctx.actor) &&
        w.prop_int(*actor, p_pool, 0) > 0 &&
        p_hun != kNoProp && w.prop_int(*t, p_hun, 0) > 600) {
        const std::int64_t pool0 = w.prop_int(*actor, p_pool, 0);
        const std::int64_t leg0 = pol_legitimacy(w, ctx.actor);
        set_change(r, actor->id, p_pool, pool0,
                   std::max<std::int64_t>(0, pool0 - 100));
        if (p_lrt != kNoProp)
            set_change(r, actor->id, p_lrt,
                w.prop_int(*actor, p_lrt, 0),
                static_cast<std::int64_t>(w.clock_.tick()));
        set_change(r, actor->id, p_leg, leg0,
                   std::min<std::int64_t>(2000, leg0 + 20));
        const std::int64_t h0 = w.prop_int(*t, p_hun, 0);
        set_change(r, t->id, p_hun, h0,
                   std::max<std::int64_t>(0, h0 - 50));
        r.notes.push_back("scarcity relief from the mandate");
    }
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


// ---- Phase 12.2: inductive economy -----------------------------------------
// take/steal/trade/repay — every price is computed at predict time from the
// two agents' SUBJUCTIVE valuations (Cognition.hpp econ_*); consequences
// that are World Truth (possession, stacks, obligations, consensus) ride
// the committed StateChange/SpawnSpec set; consequences that are private
// (ownership beliefs, shame engrams) land post-commit in econ_from_event.

constexpr std::int64_t kTakeCost = 15;
constexpr std::int64_t kStealCost = 15;
constexpr std::int64_t kTradeCost = 30;
constexpr std::int64_t kRepayCost = 20;
constexpr std::int64_t kTheftExposureTicks = 300;   // unwitnessed-grab window
constexpr std::int64_t kDefaultCreditTerm = 400;
constexpr std::int64_t kCreditFloorRep = 400;       // below: no more credit

// community registry entity for an agent's language_id (0 = none).
EntityId econ_community_of(const World& w, EntityId agent) {
    const PropId p_lang = pid(w, "language_id");
    const PropId p_lname = pid(w, "language_name");
    if (p_lang == kNoProp || p_lname == kNoProp) return kNoEntity;
    const Entity* ae = w.find(agent);
    if (!ae) return kNoEntity;
    const std::int64_t lang = w.prop_int(*ae, p_lang, 0);
    if (lang <= 0) return kNoEntity;
    for (const auto& ep : w.entities) {
        const Entity& e = *ep;
        if (e.alive && e.kind_name == SIMLAB_KIND("language") &&
            w.prop_int(e, p_lname, 0) == lang)
            return e.id;
    }
    return kNoEntity;
}

// goods-like check: an entity that can be owned/traded.
bool econ_is_goods(const World& w, const Entity& e) {
    if (e.material != kNoMaterial) return true;
    const PropId q = pid(w, "quantity");
    if (q != kNoProp) {
        const Value* v = w.prop_of(e, q);
        if (v && v->type == ValType::Integer && v->i > 0) return true;
    }
    return false;
}

// transfer `qty` units of `mat` from `from` to `to`: decrements the source
// held stack, tops up an existing destination stack or spawns a new one at
// the recipient's feet. Returns false when the source cannot cover.
bool econ_transfer(PredictionResult& r, const World& w,
                   EntityId from, EntityId to, MaterialId mat,
                   std::int64_t qty, const Vec2& to_pos) {
    const EntityId src = econ_held_stack(w, from, mat, qty);
    if (src == kNoEntity) return false;
    const Entity* se = w.find(src);
    if (!se) return false;
    const PropId p_qty = pid(w, "quantity");
    const PropId p_poss = pid(w, "possessed_by");
    const PropId p_integ = pid(w, "integrity");
    const std::int64_t sq = p_qty != kNoProp ? w.prop_int(*se, p_qty, 1) : 1;
    // source side
    if (sq == qty && p_poss != kNoProp) {
        // hand over the WHOLE stack — possession simply changes hands
        set_change(r, src, p_poss, w.prop_int(*se, p_poss, 0),
                   static_cast<std::int64_t>(to));
    } else {
        set_change(r, src, p_qty, sq, sq - qty);
    }
    // destination side: merge into a same-material held stack or spawn
    const EntityId dst = econ_held_stack(w, to, mat, 1);
    const Entity* de = dst != kNoEntity ? w.find(dst) : nullptr;
    if (de && dst != src) {
        const std::int64_t dq = p_qty != kNoProp ? w.prop_int(*de, p_qty, 1) : 1;
        const std::int64_t cap = 100000000;
        set_change(r, dst, p_qty, dq, std::min(cap, dq + qty));
    } else {
        SpawnSpec sp;
        sp.kind = se->kind_name;
        sp.material = mat;
        sp.pos = to_pos;
        if (p_qty != kNoProp) sp.props.set(p_qty, Value::integer(qty));
        if (p_poss != kNoProp)
            sp.props.set(p_poss, Value::integer(
                static_cast<std::int64_t>(to)));
        r.spawns.push_back(sp);
    }
    (void)p_integ;
    return true;
}

// bump a community's monetary_consensus for a store-of-value receipt —
// committed as a StateChange so the ledger records the money supply shift.
void econ_consensus_commit(PredictionResult& r, const World& w,
                           EntityId community, MaterialId mat,
                           std::int64_t qty) {
    if (community == kNoEntity || !econ_is_store_of_value(w, mat)) return;
    const PropId p_mc = pid(w, "monetary_consensus");
    if (p_mc == kNoProp) return;
    const Entity* ce = w.find(community);
    if (!ce) return;
    const Value* cur = w.prop_of(*ce, p_mc);
    const std::string before = cur && cur->type == ValType::Text
        ? cur->s : std::string();
    const std::string after = econ_consensus_bump_text(before, mat, qty);
    if (after != before)
        set_change_v(r, community, p_mc, Value::text(before),
                     Value::text(after));
}

// parse "give:MAT:QTY|get:MAT:QTY" (+ optional "|defer" "|contract")
struct TradeTerms {
    MaterialId give = kNoMaterial, get = kNoMaterial;
    std::int64_t give_qty = 0, get_qty = 0;
    bool defer = false, contract = false;
    bool ok = false;
};
TradeTerms parse_trade_note(const World& w, const std::string& note) {
    TradeTerms t;
    std::vector<std::string> parts;
    std::string cur;
    for (char ch : note) {
        if (ch == '|' || ch == ';') { parts.push_back(cur); cur.clear(); }
        else cur.push_back(ch);
    }
    parts.push_back(cur);
    for (const std::string& pt : parts) {
        if (pt == "defer") { t.defer = true; continue; }
        if (pt == "contract") { t.contract = true; continue; }
        const std::size_t c1 = pt.find(':');
        const std::size_t c2 = pt.find(':', c1 == std::string::npos
                                          ? c1 : c1 + 1);
        if (c1 == std::string::npos || c2 == std::string::npos) continue;
        const std::string side = pt.substr(0, c1);
        const std::string mname = pt.substr(c1 + 1, c2 - c1 - 1);
        std::int64_t qty = 0;
        try { qty = std::stoll(pt.substr(c2 + 1)); } catch (...) { continue; }
        const MaterialId mid = w.materials.id_of(mname);
        if (mid == kNoMaterial || qty <= 0) continue;
        if (side == "give" && t.give == kNoMaterial) {
            t.give = mid; t.give_qty = qty;
        } else if (side == "get" && t.get == kNoMaterial) {
            t.get = mid; t.get_qty = qty;
        }
    }
    t.ok = t.give != kNoMaterial && t.get != kNoMaterial &&
           t.give != t.get && t.give_qty > 0 && t.get_qty > 0;
    return t;
}

// quantity helper local to this file (Cognition's econ_qty is anonymous).
std::int64_t econ_qty_local(const World& w, const Entity& e) {
    const PropId q = pid(w, "quantity");
    if (q == kNoProp) return 1;
    const Value* v = w.prop_of(e, q);
    if (!v || v->type != ValType::Integer || v->i <= 0) return 1;
    return v->i;
}

PredictionResult predict_take(const World& w, const InteractionContext& ctx,
                              bool steal) {
    PredictionResult r; r.law = 8; r.law_name = "social.interaction";
    const PropId contained = pid(w, "contained");
    const PropId energy = pid(w, "energy");
    const PropId p_poss = pid(w, "possessed_by");
    const PropId p_theft = pid(w, "theft_flag");
    const PropId p_rep = pid(w, "trade_reputation");
    const PropId p_reach = pid(w, "reach");
    if (p_poss == kNoProp) { r.failure = "no possession property"; return r; }
    if (!alive_pair(w, ctx, r, contained)) return r;
    const Entity* actor = w.find(ctx.actor);
    const Entity* t = single_target(w, ctx, r, contained);
    if (!t) return r;
    if (t->id == ctx.actor) { r.failure = "cannot take oneself"; return r; }
    if (!econ_is_goods(w, *t)) { r.failure = "not a goods entity"; return r; }
    const std::int64_t holder = w.prop_int(*t, p_poss, 0);
    const bool mine = holder == static_cast<std::int64_t>(ctx.actor);
    if (holder != 0 && !mine) {
        if (!steal) { r.failure = "already claimed"; return r; }
    }
    if (!in_reach(w, *actor, *t, p_reach, 0, &r)) {
        r.failure = "out of reach"; return r;
    }
    // Phase 12.6 siege: goods stored inside a standing, barred enclosure
    // cannot be reached from outside its walls by anyone but its owner —
    // until the walls are breached (access_barrier knocked to 0).
    if (const EntityId enc = arch_enclosure_at(w, t->pos); enc != kNoEntity) {
        const Entity* ee = w.find(enc);
        const PropId p_bar = pid(w, "access_barrier"), p_fr = pid(w, "footprint_radius");
        const std::int64_t bar = ee && p_bar != kNoProp ? w.prop_int(*ee, p_bar, 0) : 0;
        const std::int64_t rad = ee && p_fr != kNoProp ? w.prop_int(*ee, p_fr, 0) : 0;
        const bool inside = ee && dist2(ee->pos, actor->pos) <= rad * rad;
        if (bar >= 500 && !inside && arch_owner_of(w, enc) != ctx.actor) {
            add_cond(r, "access_barrier", 0, bar);
            r.failure = "the walls bar the stores"; return r;
        }
    }
    if (!charge_energy(*actor, energy, steal ? kStealCost : kTakeCost, r))
        return r;
    // witnessed? any OTHER minded agent in sight of the actor sees the grab
    bool seen = false;
    for (const auto& [wid, wm] : w.minds_) {
        (void)wm;
        if (wid == ctx.actor) continue;
        const Entity* we = w.find(wid);
        if (!we || !we->alive) continue;
        const PropId sight = pid(w, "sight_range");
        const std::int64_t range = sight != kNoProp
            ? w.prop_int(*we, sight, 0) : 0;
        if (range > 0 && dist2(we->pos, actor->pos) <= range * range) {
            seen = true; break;
        }
    }
    set_change(r, t->id, p_poss, holder,
               static_cast<std::int64_t>(ctx.actor));
    if (steal && holder != 0) {
        if (p_theft != kNoProp)
            set_change(r, t->id, p_theft, w.prop_int(*t, p_theft, 0),
                       seen ? 0 : kTheftExposureTicks);
        if (seen && p_rep != kNoProp) {
            const std::int64_t rep = w.prop_int(*actor, p_rep, 1000);
            set_change(r, ctx.actor, p_rep, rep,
                       std::max<std::int64_t>(0, rep - 200));
            // Phase 12.3: a witnessed grab hardens the communal norm
            // ledger against theft and scars the thief's legitimacy.
            const EntityId comm = econ_community_of(w, ctx.actor);
            const PropId p_nt = pid(w, "norm_table");
            if (comm != kNoEntity && p_nt != kNoProp) {
                const Entity* ce = w.find(comm);
                const Value* cur = ce ? w.prop_of(*ce, p_nt) : nullptr;
                const std::string before =
                    cur && cur->type == ValType::Text ? cur->s : "";
                const std::string after = pol_table_bump_text(
                    before, "steal", 400);
                if (after != before)
                    set_change_v(r, comm, p_nt, Value::text(before),
                                 Value::text(after));
            }
            const PropId p_leg = pid(w, "social_legitimacy");
            if (p_leg != kNoProp)
                set_change(r, ctx.actor, p_leg,
                    pol_legitimacy(w, ctx.actor),
                    std::max<std::int64_t>(0,
                        pol_legitimacy(w, ctx.actor) - 100));
            r.notes.push_back("theft witnessed");
        } else if (!seen) {
            r.notes.push_back("theft unseen — exposure window armed");
        }
    }
    // Phase 12.5: stealing across a speech-community border is an act of
    // war by the ledgers' lights — both diplo_tables harden.
    if (steal && holder != 0) {
        const Entity* holder_e = w.find(
            static_cast<EntityId>(holder));
        const PropId p_lang = pid(w, "language_id");
        const PropId p_dt = pid(w, "diplo_table");
        const std::int64_t la = sl_lang_of(w, *actor);
        const std::int64_t lb = holder_e && p_lang != kNoProp
            ? w.prop_int(*holder_e, p_lang, 0) : 0;
        if (la > 0 && lb > 0 && la != lb && p_dt != kNoProp) {
            for (std::int64_t side : {la, lb}) {
                const std::int64_t peer = side == la ? lb : la;
                const EntityId le = diplo_lang_entity(w, side);
                if (le == kNoEntity) continue;
                const Entity* lee = w.find(le);
                if (!lee) continue;
                const Value* cur = w.prop_of(*lee, p_dt);
                const std::string before =
                    cur && cur->type == ValType::Text ? cur->s : "";
                const std::string after =
                    diplo_bump(before, peer, -150);
                if (after != before)
                    set_change_v(r, le, p_dt, Value::text(before),
                                 Value::text(after));
            }
            r.notes.push_back("cross-border theft — ledgers harden");
        }
    }
    r.possible = true; r.ticks = 1;
    r.notes.push_back(steal ? "stole item" : "claimed item");
    return r;
}

PredictionResult predict_trade(const World& w, const InteractionContext& ctx) {
    PredictionResult r; r.law = 8; r.law_name = "social.interaction";
    const PropId contained = pid(w, "contained");
    const PropId energy = pid(w, "energy");
    const PropId p_poss = pid(w, "possessed_by");
    const PropId p_rep = pid(w, "trade_reputation");
    const PropId p_reach = pid(w, "reach");
    if (p_poss == kNoProp) { r.failure = "no possession property"; return r; }
    if (!alive_pair(w, ctx, r, contained)) return r;
    const Entity* actor = w.find(ctx.actor);
    const Entity* partner = single_target(w, ctx, r, contained);
    if (!partner) return r;
    if (!in_reach(w, *actor, *partner, p_reach, 0, &r)) {
        r.failure = "trading partner out of reach"; return r;
    }
    if (pol_ostracism(w, ctx.actor, partner->id) >= 500) {
        r.failure = "ostracized by the community"; return r;
    }
    // Phase 12.5: communities whose ledgers say "hostile" do not trade —
    // the treaty/hostility state is World Truth on the language entities.
    {
        const PropId p_lang = pid(w, "language_id");
        const std::int64_t la = p_lang != kNoProp
            ? w.prop_int(*actor, p_lang, 0) : 0;
        const std::int64_t lb = p_lang != kNoProp
            ? w.prop_int(*partner, p_lang, 0) : 0;
        if (la > 0 && lb > 0 && la != lb && diplo_hostile(w, la, lb)) {
            r.failure = "communities are hostile"; return r;
        }
    }
    TradeTerms tt = parse_trade_note(w, ctx.parameters.note);
    if (!tt.ok) { r.failure = "malformed trade terms"; return r; }
    if (!charge_energy(*actor, energy, kTradeCost, r)) return r;

    // subjective price check — the ACTOR must not overpay by its own lights
    const std::int64_t vg = econ_total_value(w, ctx.actor, tt.give);
    const std::int64_t vt = econ_total_value(w, ctx.actor, tt.get);
    if (vt <= 0) { r.failure = "consideration worthless to actor"; return r; }
    const std::int64_t req = tt.give_qty * vg / vt;
    // 10% deal margin — haggling noise, deterministic
    if (tt.get_qty * 10 < req * 9) {
        r.failure = "insufficient consideration"; return r;
    }
    // partner's side: must not lose more than an eighth of the deal's worth
    const std::int64_t pvg = econ_total_value(w, partner->id, tt.get);
    const std::int64_t pvt = econ_total_value(w, partner->id, tt.give);
    const __int128 p_surplus = static_cast<__int128>(tt.give_qty) * pvt -
        static_cast<__int128>(tt.get_qty) * pvg;
    if (p_surplus < -(static_cast<__int128>(tt.get_qty) * pvg) / 8) {
        r.failure = "partner would refuse the terms"; return r;
    }

    const std::uint64_t now = w.clock_.tick();
    if (tt.defer) {
        // credit sale: actor receives `get` now, owes debt_value later
        const std::int64_t rep = p_rep != kNoProp
            ? w.prop_int(*actor, p_rep, 1000) : 1000;
        if (rep < kCreditFloorRep) {
            r.failure = "no credit — reputation too low"; return r;
        }
        // goods flow partner -> actor immediately
        if (!econ_transfer(r, w, partner->id, ctx.actor, tt.get,
                           tt.get_qty, actor->pos)) {
            r.failure = "partner lacks the goods"; return r;
        }
        const std::int64_t received_val =
            econ_total_value(w, ctx.actor, tt.get) * tt.get_qty;
        const std::int64_t premium = std::clamp<std::int64_t>(
            1400 - rep, 0, 1200);
        const std::int64_t debt_val =
            static_cast<std::int64_t>(
                static_cast<__int128>(received_val) * (1000 + premium) / 1000);
        const std::int64_t term = w.prop_int(*actor,
            pid(w, "credit_term_ticks"), kDefaultCreditTerm);
        SpawnSpec dbt;
        dbt.kind = "debt_obligation";
        dbt.pos = actor->pos;
        dbt.props.set(pid(w, "debt_value"), Value::integer(debt_val));
        dbt.props.set(pid(w, "due_tick"),
                      Value::integer(static_cast<std::int64_t>(now) + term));
        dbt.props.set(pid(w, "debtor_id"),
                      Value::integer(static_cast<std::int64_t>(ctx.actor)));
        dbt.props.set(pid(w, "creditor_id"),
                      Value::integer(static_cast<std::int64_t>(partner->id)));
        dbt.props.set(pid(w, "debt_status"), Value::integer(0));
        r.spawns.push_back(dbt);
        // optional written contract: a literate debtor carves the terms
        if (tt.contract) {
            const PropId p_lang = pid(w, "language_id");
            const std::int64_t al = p_lang != kNoProp
                ? w.prop_int(*actor, p_lang, 0) : 0;
            const std::int64_t pl = p_lang != kNoProp
                ? w.prop_int(*partner, p_lang, 0) : 0;
            if (al > 0 && pl > 0) {
                SpawnSpec ins;
                ins.kind = "inscription";
                ins.pos = actor->pos;
                ins.props.set(p_lang, Value::integer(al));
                const PropId p_ck = pid(w, "concept_keys");
                if (p_ck != kNoProp)
                    ins.props.set(p_ck, Value::text(
                        "debt:" + std::to_string(tt.get) + ":" +
                        std::to_string(tt.get_qty)));
                const PropId p_it = pid(w, "inscription_tick");
                if (p_it != kNoProp)
                    ins.props.set(p_it, Value::integer(
                        static_cast<std::int64_t>(now)));
                const PropId p_ia = pid(w, "inscription_author");
                if (p_ia != kNoProp)
                    ins.props.set(p_ia, Value::integer(
                        static_cast<std::int64_t>(ctx.actor)));
                r.spawns.push_back(ins);
            }
        }
        econ_consensus_commit(r, w, econ_community_of(w, ctx.actor),
                              tt.get, tt.get_qty);
        econ_consensus_commit(r, w, econ_community_of(w, partner->id),
                              tt.give, 0);   // nothing received yet — no bump
        r.possible = true; r.ticks = kTradeCost;
        r.notes.push_back("credit sale, debt " + std::to_string(debt_val));
        return r;
    }

    // spot trade: both legs settle now
    if (!econ_transfer(r, w, ctx.actor, partner->id, tt.give,
                       tt.give_qty, partner->pos)) {
        r.failure = "actor lacks the goods"; return r;
    }
    if (!econ_transfer(r, w, partner->id, ctx.actor, tt.get,
                       tt.get_qty, actor->pos)) {
        r.failure = "partner lacks the goods"; return r;
    }
    // store-of-value receipts push the community's money consensus
    econ_consensus_commit(r, w, econ_community_of(w, ctx.actor),
                          tt.get, tt.get_qty);
    econ_consensus_commit(r, w, econ_community_of(w, partner->id),
                          tt.give, tt.give_qty);
    r.possible = true; r.ticks = kTradeCost;
    r.notes.push_back("trade " + std::to_string(tt.give_qty) + "x give");
    return r;
}

PredictionResult predict_repay(const World& w, const InteractionContext& ctx) {
    PredictionResult r; r.law = 8; r.law_name = "social.interaction";
    const PropId contained = pid(w, "contained");
    const PropId energy = pid(w, "energy");
    const PropId p_poss = pid(w, "possessed_by");
    const PropId p_rep = pid(w, "trade_reputation");
    const PropId p_reach = pid(w, "reach");
    if (!alive_pair(w, ctx, r, contained)) return r;
    const Entity* actor = w.find(ctx.actor);
    const Entity* creditor = single_target(w, ctx, r, contained);
    if (!creditor) return r;
    if (!in_reach(w, *actor, *creditor, p_reach, 0, &r)) {
        r.failure = "creditor out of reach"; return r;
    }
    // find the open obligation debtor=actor -> creditor (lowest id wins)
    const PropId p_dv = pid(w, "debt_value");
    const PropId p_db = pid(w, "debtor_id");
    const PropId p_cr = pid(w, "creditor_id");
    const PropId p_ds = pid(w, "debt_status");
    EntityId debt = kNoEntity;
    std::int64_t debt_val = 0;
    for (const auto& ep : w.entities) {
        const Entity& e = *ep;
        if (!e.alive || e.kind_name != SIMLAB_KIND("debt_obligation")) continue;
        if (w.prop_int(e, p_ds, 0) != 0) continue;
        if (w.prop_int(e, p_db, 0) != static_cast<std::int64_t>(ctx.actor))
            continue;
        if (w.prop_int(e, p_cr, 0) !=
            static_cast<std::int64_t>(creditor->id)) continue;
        if (debt == kNoEntity || e.id < debt) {
            debt = e.id;
            debt_val = w.prop_int(e, p_dv, 0);
        }
    }
    if (debt == kNoEntity) { r.failure = "no open debt"; return r; }
    if (!charge_energy(*actor, energy, kRepayCost, r)) return r;
    // payment terms: "mat:qty" in note — must cover the debt's value
    const TradeTerms tt = parse_trade_note(w, ctx.parameters.note);
    MaterialId pmat = tt.give; std::int64_t pqty = tt.give_qty;
    if (pmat == kNoMaterial) {
        // default: pay with the single most valuable held stack, all of it
        std::int64_t best_v = -1; EntityId best = kNoEntity;
        for (const auto& ep : w.entities) {
            const Entity& e = *ep;
            if (!e.alive || e.material == kNoMaterial ||
                p_poss == kNoProp) continue;
            if (w.prop_int(e, p_poss, 0) !=
                static_cast<std::int64_t>(ctx.actor)) continue;
            const std::int64_t v = econ_total_value(w, ctx.actor,
                                                   e.material) *
                                   econ_qty_local(w, e);
            if (v > best_v) { best_v = v; best = e.id; }
        }
        if (best == kNoEntity) { r.failure = "nothing to repay with"; return r; }
        const Entity* be = w.find(best);
        pmat = be->material;
        pqty = econ_qty_local(w, *be);
    }
    const std::int64_t paid_val =
        econ_total_value(w, ctx.actor, pmat) * pqty;
    if (paid_val < debt_val) {
        r.failure = "payment does not cover the debt"; return r;
    }
    if (!econ_transfer(r, w, ctx.actor, creditor->id, pmat, pqty,
                       creditor->pos)) {
        r.failure = "cannot part with the goods"; return r;
    }
    if (p_ds != kNoProp) set_change(r, debt, p_ds, 0, 1);
    if (p_dv != kNoProp) set_change(r, debt, p_dv, debt_val, 0);
    // on-time settlement builds credit; late settlement still counts but
    // the overdue default already hit reputation in econ_drift
    const std::int64_t rep = p_rep != kNoProp
        ? w.prop_int(*actor, p_rep, 1000) : 1000;
    if (p_rep != kNoProp)
        set_change(r, ctx.actor, p_rep, rep,
                   std::min<std::int64_t>(2000, rep + 150));
    r.possible = true; r.ticks = kRepayCost;
    r.notes.push_back("debt settled");
    return r;
}


// ---- Phase 12.3: political & legal base -------------------------------------
// adjudicate/tribute — authority is never assigned: the mandate holder is
// whoever the community's defer_to beliefs converge on (pol_holds_mandate).
// Every verdict rides StateChanges; mental aftermath lands in
// political_from_event.

constexpr std::int64_t kAdjCost = 40;
constexpr std::int64_t kTribCost = 20;

// contested property: an item the offender holds while some OTHER mind
// still claims "<victim> owns <item>" from before the grab. Returns victim.
EntityId pol_contested_victim(const World& w, EntityId offender,
                              EntityId item) {
    const std::string prefix = "owns:" +
        std::to_string(static_cast<std::uint64_t>(item)) + ":";
    EntityId best_v = kNoEntity; std::int64_t best_c = 300;
    for (const auto& [wid, m] : w.minds_) {
        (void)m;
        const AgentMind* mm = w.find_mind(wid);
        if (!mm || wid == offender) continue;
        for (const auto& [k, b] : mm->beliefs) {
            if (k.rfind(prefix, 0) != 0 || b.confidence <= best_c)
                continue;
            try {
                const EntityId v = static_cast<EntityId>(
                    std::stoull(k.substr(prefix.size())));
                if (v != offender && w.find_mind(v)) {
                    best_c = b.confidence; best_v = v;
                }
            } catch (...) {}
        }
    }
    return best_v;
}

// defaulted obligations count as proven harm even when goods are gone.
std::int64_t pol_defaulted_debts(const World& w, EntityId debtor) {
    const PropId p_ds = pid(w, "debt_status");
    const PropId p_db = pid(w, "debtor_id");
    if (p_ds == kNoProp || p_db == kNoProp) return 0;
    std::int64_t n = 0;
    for (const auto& ep : w.entities) {
        const Entity& e = *ep;
        if (!e.alive || e.kind_name != SIMLAB_KIND("debt_obligation")) continue;
        if (w.prop_int(e, p_ds, 0) == 2 &&
            w.prop_int(e, p_db, 0) ==
                static_cast<std::int64_t>(debtor)) ++n;
    }
    return n;
}

PredictionResult predict_adjudicate(const World& w,
                                    const InteractionContext& ctx) {
    PredictionResult r; r.law = 8; r.law_name = "social.interaction";
    const PropId contained = pid(w, "contained");
    const PropId energy = pid(w, "energy");
    const PropId p_poss = pid(w, "possessed_by");
    const PropId p_leg = pid(w, "social_legitimacy");
    const PropId p_reach = pid(w, "reach");
    const PropId p_ot = pid(w, "ostracism_table");
    if (!alive_pair(w, ctx, r, contained)) return r;
    const Entity* actor = w.find(ctx.actor);
    const Entity* offender = single_target(w, ctx, r, contained);
    if (!offender) return r;
    if (offender->id == ctx.actor) {
        r.failure = "cannot adjudicate oneself"; return r;
    }
    if (!in_reach(w, *actor, *offender, p_reach, 0, &r)) {
        r.failure = "offender out of reach"; return r;
    }
    // only the community's mandated arbiter may sit in judgment
    if (!pol_holds_mandate(w, ctx.actor)) {
        r.failure = "no mandate to adjudicate"; return r;
    }
    if (!charge_energy(*actor, energy, kAdjCost, r)) return r;

    const bool pardon =
        ctx.parameters.note.find("pardon") != std::string::npos;
    // gather contested goods the offender still holds
    std::vector<std::pair<EntityId, EntityId>> contested;  // item -> victim
    for (const auto& ep : w.entities) {
        const Entity& e = *ep;
        if (!e.alive || p_poss == kNoProp) continue;
        if (w.prop_int(e, p_poss, 0) !=
            static_cast<std::int64_t>(offender->id)) continue;
        const EntityId v = pol_contested_victim(w, offender->id, e.id);
        if (v != kNoEntity) contested.push_back({e.id, v});
    }
    const std::int64_t defaults =
        pol_defaulted_debts(w, offender->id);
    const bool proven = !contested.empty() || defaults > 0;

    if (pardon && proven) {
        // mercy for a proven wrong: the arbiter's own standing pays for it
        r.notes.push_back("unjust pardon");
        for (const auto& [it, v] : contested)
            r.notes.push_back("victim:" +
                std::to_string(static_cast<std::uint64_t>(v)));
        if (p_leg != kNoProp)
            set_change(r, actor->id, p_leg,
                pol_legitimacy(w, ctx.actor),
                std::max<std::int64_t>(0,
                    pol_legitimacy(w, ctx.actor) - 300));
        r.possible = true; r.ticks = kAdjCost;
        r.notes.push_back("pardoned a proven wrong");
        return r;
    }
    if (contested.empty()) {
        // nothing to restore — but proven harm without goods means the
        // offender keeps offending: the community outlaws them instead
        if (!proven) { r.failure = "no grounds for judgment"; return r; }
        const EntityId comm = econ_community_of(w, ctx.actor);
        if (comm == kNoEntity || p_ot == kNoProp) {
            r.failure = "no community to enforce the verdict"; return r;
        }
        const Entity* ce = w.find(comm);
        const Value* cur = ce ? w.prop_of(*ce, p_ot) : nullptr;
        const std::string before = cur && cur->type == ValType::Text
            ? cur->s : std::string();
        const std::string key = std::to_string(
            static_cast<std::uint64_t>(offender->id));
        const std::string after = pol_table_bump_text(before, key, 1000);
        if (after != before)
            set_change_v(r, comm, p_ot, Value::text(before),
                         Value::text(after));
        if (p_leg != kNoProp) {
            set_change(r, actor->id, p_leg,
                pol_legitimacy(w, ctx.actor),
                std::min<std::int64_t>(2000,
                    pol_legitimacy(w, ctx.actor) + 60));
            set_change(r, offender->id, p_leg,
                pol_legitimacy(w, offender->id),
                std::max<std::int64_t>(0,
                    pol_legitimacy(w, offender->id) - 150));
        }
        r.possible = true; r.ticks = kAdjCost;
        r.notes.push_back("ostracized repeat offender");
        return r;
    }
    // restitution: every contested item goes back to its proven owner
    for (const auto& [it, v] : contested) {
        const Entity* ie = w.find(it);
        if (!ie) continue;
        set_change(r, it, p_poss,
            static_cast<std::int64_t>(offender->id),
            static_cast<std::int64_t>(v));
    }
    if (p_leg != kNoProp) {
        set_change(r, actor->id, p_leg,
            pol_legitimacy(w, ctx.actor),
            std::min<std::int64_t>(2000,
                pol_legitimacy(w, ctx.actor) + 80));
        set_change(r, offender->id, p_leg,
            pol_legitimacy(w, offender->id),
            std::max<std::int64_t>(0,
                pol_legitimacy(w, offender->id) - 80));
    }
    r.possible = true; r.ticks = kAdjCost;
    r.notes.push_back("restitution of " +
        std::to_string(contested.size()) + " contested items");
    return r;
}

PredictionResult predict_tribute(const World& w,
                                 const InteractionContext& ctx) {
    PredictionResult r; r.law = 8; r.law_name = "social.interaction";
    const PropId contained = pid(w, "contained");
    const PropId energy = pid(w, "energy");
    const PropId p_reach = pid(w, "reach");
    const PropId p_leg = pid(w, "social_legitimacy");
    const PropId p_pool = pid(w, "public_pool");
    if (!alive_pair(w, ctx, r, contained)) return r;
    const Entity* actor = w.find(ctx.actor);
    const Entity* leader = single_target(w, ctx, r, contained);
    if (!leader) return r;
    if (leader->id == ctx.actor) {
        r.failure = "cannot tribute oneself"; return r;
    }
    if (!in_reach(w, *actor, *leader, p_reach, 0, &r)) {
        r.failure = "leader out of reach"; return r;
    }
    // tribute flows along allegiance — no defer_to, no obligation
    const AgentMind* am = w.find_mind(ctx.actor);
    if (!am || pol_defer_conf(*am, leader->id) < 300) {
        r.failure = "no allegiance to this leader"; return r;
    }
    // payment terms: "MAT:QTY" or the single most valuable held stack
    MaterialId pmat = kNoMaterial; std::int64_t pqty = 0;
    {
        const std::string& n = ctx.parameters.note;
        const std::size_t c = n.find(':');
        if (c != std::string::npos) {
            try {
                const MaterialId m = w.materials.id_of(n.substr(0, c));
                const std::int64_t qq =
                    std::stoll(n.substr(c + 1));
                if (m != kNoMaterial && qq > 0) { pmat = m; pqty = qq; }
            } catch (...) {}
        }
    }
    if (pmat == kNoMaterial) {
        std::int64_t best_v = -1; EntityId best = kNoEntity;
        const PropId p_poss2 = pid(w, "possessed_by");
        for (const auto& ep : w.entities) {
            const Entity& e = *ep;
            if (!e.alive || e.material == kNoMaterial ||
                p_poss2 == kNoProp) continue;
            if (w.prop_int(e, p_poss2, 0) !=
                static_cast<std::int64_t>(ctx.actor)) continue;
            const std::int64_t v = econ_total_value(w, ctx.actor,
                                                   e.material) *
                                   econ_qty_local(w, e);
            if (v > best_v) { best_v = v; best = e.id; }
        }
        if (best == kNoEntity) { r.failure = "nothing to tribute"; return r; }
        const Entity* be = w.find(best);
        pmat = be->material;
        pqty = econ_qty_local(w, *be);
    }
    if (!charge_energy(*actor, energy, kTribCost, r)) return r;
    if (!econ_transfer(r, w, ctx.actor, leader->id, pmat, pqty,
                       leader->pos)) {
        r.failure = "cannot part with the goods"; return r;
    }
    const std::int64_t paid_val =
        econ_total_value(w, ctx.actor, pmat) * pqty;
    const std::int64_t pool0 = pol_pool(w, leader->id);
    const std::int64_t leg0 = pol_legitimacy(w, leader->id);
    if (p_pool != kNoProp)
        set_change(r, leader->id, p_pool, pool0,
                   std::min<std::int64_t>(1000000000, pool0 + paid_val));
    if (p_leg != kNoProp)
        set_change(r, leader->id, p_leg, leg0,
                   std::min<std::int64_t>(2000, leg0 + 5));
    r.possible = true; r.ticks = kTribCost;
    r.notes.push_back("tribute " + std::to_string(paid_val) + " value");
    return r;
}

}  // namespace

LawId SocialModel::id() const { return 8; }

bool SocialModel::supports(const World& w, const InteractionContext& ctx) const {
    (void)w;
    // every law is asked about every action; one hash probe instead of a
    // 17-way string compare chain (same verdicts, same order of laws).
    enum class Arity : std::uint8_t { None, One, UpTo8 };
    static const std::unordered_map<std::string_view, Arity> kVerbs = {
        {"found", Arity::None},
        {"communicate", Arity::One}, {"engrave", Arity::One}, {"read", Arity::One},
        {"join", Arity::One}, {"leave", Arity::One}, {"share", Arity::One},
        {"withdraw", Arity::One}, {"take", Arity::One}, {"steal", Arity::One},
        {"trade", Arity::One}, {"repay", Arity::One}, {"adjudicate", Arity::One},
        {"tribute", Arity::One}, {"consecrate", Arity::One},
        {"diplomatic_parley", Arity::One},
        {"perform_ritual", Arity::UpTo8}};
    const auto it = kVerbs.find(ctx.parameters.action);
    if (it == kVerbs.end()) return false;
    switch (it->second) {
        case Arity::None:  return ctx.targets.empty();
        case Arity::One:   return ctx.targets.size() == 1;
        case Arity::UpTo8: return ctx.targets.size() <= 8;
    }
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
    if (a == "take")       return predict_take(w, ctx, false);
    if (a == "steal")      return predict_take(w, ctx, true);
    if (a == "trade")      return predict_trade(w, ctx);
    if (a == "repay")      return predict_repay(w, ctx);
    if (a == "adjudicate") return predict_adjudicate(w, ctx);
    if (a == "tribute")    return predict_tribute(w, ctx);
    if (a == "perform_ritual") return predict_perform_ritual(w, ctx);
    if (a == "consecrate") return predict_consecrate(w, ctx);
    if (a == "diplomatic_parley") return predict_diplomatic_parley(w, ctx);
    PredictionResult r; r.law = id(); r.law_name = name();
    r.failure = "unsupported social action";
    return r;
}

std::unique_ptr<INaturalLawModel> SocialModel::clone() const {
    return std::make_unique<SocialModel>();
}

}  // namespace simlab
