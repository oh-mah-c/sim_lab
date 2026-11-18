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
