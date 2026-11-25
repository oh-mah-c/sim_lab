#include "simlab/Sensor.hpp"
#include "simlab/World.hpp"
#include "simlab/Cognition.hpp"
#include "simlab/Causal.hpp"
#include "simlab/Laws.hpp"
#include "simlab/Materials.hpp"
#include <algorithm>
#include <cmath>
#include <set>

namespace simlab {

namespace {

// ---- fixed channel tables -----------------------------------------------------
const std::array<const char*, kInteroChannels> kInteroNames = {
    "hunger", "thirst", "fatigue", "energy", "integrity", "blood_volume",
    "consciousness", "pain_level", "temperature", "toxin_load",
    "pathogen_load", "speed", "metabolism", "immune_power", "mass", "age"};
// scale: prop units -> full int8 range. Permille props scale by their max;
// big-range props compress so typical values land mid-channel.
const std::array<std::int64_t, kInteroChannels> kInteroScale = {
    1000, 1000, 1000, 400000, 1000, 1000,
    1000, 1000, 25000, 1000, 1500, 100000,
    100000, 2000, 50000000, 50000000};
const std::array<std::int64_t, kInteroChannels> kInteroOffset = {
    0, 0, 0, 0, 0, 0, 0, 0, 5000, 0, 0, 0, 0, 0, 0, 0};  // temp deciC
const std::array<std::int32_t, kInteroChannels> kInteroSign = {
    1, 1, 1, -1, -1, -1, -1, 1, 1, 1, 1, -1, -1, -1, -1, 1};
// sign: +1 worse-when-high (needs/toxins/age), -1 worse-when-low (resources)

const char* const kVerbs[kActionVerbs] = {
    "consume", "move", "strike", "impact", "rest", "share", "dig", "sow",
    "irrigate", "process", "combine", "reproduce", "communicate", "engrave",
    "read", "enclose"};

std::int64_t isqrt_i64(std::int64_t v) {
    if (v <= 0) return 0;
    std::int64_t r = v;
    std::int64_t x = (r + 1) / 2;
    while (x < r) { r = x; x = (x + v / x) / 2; }
    return r;
}

PropId intero_pid(const World& w, int i) {
    static thread_local std::array<PropId, kInteroChannels> cache{};
    static thread_local std::uint64_t seen_uid = 0;
    const std::uint64_t uid = w.props.uid();
    if (uid != seen_uid) {
        seen_uid = uid;
        for (int k = 0; k < kInteroChannels; ++k) {
            const PropertyDef* d = w.props.find_name(kInteroNames[k]);
            cache[k] = d ? d->id : kNoProp;
        }
    }
    return cache[i];
}

}  // namespace

const std::array<const char*, kInteroChannels>& sensor_intero_names() {
    return kInteroNames;
}
const std::array<std::int64_t, kInteroChannels>& sensor_intero_scale() {
    return kInteroScale;
}
const std::array<std::int32_t, kInteroChannels>& sensor_intero_sign() {
    return kInteroSign;
}
const std::array<std::int64_t, kInteroChannels>& sensor_intero_offset() {
    return kInteroOffset;
}
const char* const* sensor_verb_table() { return kVerbs; }

std::int8_t sensor_norm(std::int64_t v, std::int64_t scale) {
    if (scale <= 0) return 0;
    const std::int64_t q = v * 127 / scale;
    if (q > 127) return 127;
    if (q < -128) return -128;
    return static_cast<std::int8_t>(q);
}

SensorVec sensor_encode(const World& w, EntityId npc,
                        const ActionVec* last_action) {
    SensorVec s{};
    const Entity* me = w.find(npc);
    const AgentMind* mind = w.find_mind(npc);
    if (!me) return s;

    // ---- channels 0..15: interoception (own observable props) -----------------
    for (int i = 0; i < kInteroChannels; ++i) {
        const PropId p = intero_pid(w, i);
        const std::int64_t v =
            p != kNoProp ? w.prop_int(*me, p, 0) : 0;
        s[i] = sensor_norm(v + kInteroOffset[i], kInteroScale[i]);
    }

    // ---- channels 16..23: neuro axes ------------------------------------------
    if (mind) {
        const NeuroState& n = neuro_of(*mind);
        s[16] = sensor_norm(n.panic, 8);
        s[17] = sensor_norm(n.rage, 8);
        s[18] = sensor_norm(n.grief, 8);
        s[19] = sensor_norm(n.threat_gain - 1000, 16);
        s[20] = sensor_norm(n.reward_gain - 1000, 16);
        s[21] = sensor_norm(n.pattern_coupling - 1000, 16);
        s[22] = sensor_norm(n.empathy_coupling - 1000, 16);
        s[23] = sensor_norm(n.panic + n.rage + n.grief, 24);
    }

    // ---- channels 24..71: 6 nearest remembered entities x 8 features ----------
    const PropId p_sight = SIMLAB_PID(w.props, "sight_range");
    const std::int64_t sight =
        p_sight != kNoProp ? w.prop_int(*me, p_sight, 64) : 64;
    struct Near { std::int64_t d2; EntityId id; };
    // bounded top-6 insertion into a stack array (no heap alloc); map
    // iteration order is sorted-by-id so ties resolve deterministically
    Near near[kEntitySlots];
    int nnear = 0;
    if (mind) {
        for (const auto& [kid, snap] : mind->memory) {
            if (kid == npc) continue;
            const std::int64_t dx = snap.pos.x - me->pos.x;
            const std::int64_t dy = snap.pos.y - me->pos.y;
            const std::int64_t d2 = dx * dx + dy * dy;
            if (nnear == kEntitySlots && d2 >= near[kEntitySlots - 1].d2)
                continue;
            int p = nnear < kEntitySlots ? nnear++ : kEntitySlots - 1;
            while (p > 0 && near[p - 1].d2 > d2) { near[p] = near[p - 1]; --p; }
            near[p] = {d2, kid};
        }
    }
    const PropId p_qty = SIMLAB_PID(w.props, "quantity");
    const PropId p_integ = SIMLAB_PID(w.props, "integrity");
    for (int slot = 0; slot < kEntitySlots && slot < nnear; ++slot) {
        const EntityId kid = near[slot].id;
        const auto& snap = mind->memory.at(kid);
        const std::int64_t dx = snap.pos.x - me->pos.x;
        const std::int64_t dy = snap.pos.y - me->pos.y;
        const std::int64_t dist = isqrt_i64(near[slot].d2);
        std::int8_t* f = &s[24 + slot * kEntityFeats];
        f[0] = sensor_norm(dx * 8, sight);          // dx/sight, x8 headroom
        f[1] = sensor_norm(dy * 8, sight);
        f[2] = sensor_norm(dist * 8, sight);
        // integrity: observable prop if recorded, else mid-range prior
        std::int64_t integ = 500;
        const auto ip = snap.props.find(p_integ);
        if (ip != snap.props.end() && ip->second.type == ValType::Integer)
            integ = ip->second.i;
        f[3] = sensor_norm(integ, 8);
        // bulk: carried quantity if the snapshot saw any
        std::int64_t qty = 0;
        const auto qp = snap.props.find(p_qty);
        if (qp != snap.props.end() && qp->second.type == ValType::Integer)
            qty = qp->second.i;
        f[4] = sensor_norm(qty, 1000);
        // resource potential: material declares nutrition (data-driven)
        const MaterialDef* md = w.materials.find(snap.material);
        std::int64_t nut = 0;
        if (md) {
            const auto np = md->props.find(SIMLAB_PID(w.props, "nutrition"));
            if (np != md->props.end() && np->second.type == ValType::Integer)
                nut = np->second.i;
        }
        f[5] = sensor_norm(nut, 1000);
        // emotional charge of the engram tied to this entity (friend/foe)
        f[6] = sensor_norm(snap.engram.emotional_charge, 8);
        // memory clarity as recency/strength proxy (last-action flag is not
        // observable in snapshots — clarity is the honest substitute)
        f[7] = sensor_norm(snap.engram.clarity, 2);
    }

    // ---- channels 72..79: environment -----------------------------------------
    if (w.terrain.contains(me->pos)) {
        s[72] = sensor_norm(w.terrain.moist_at(me->pos), 8);
        s[73] = sensor_norm(w.terrain.temp_at(me->pos) - 300, 16);
        s[74] = sensor_norm(w.terrain.elev_at(me->pos), 4);
        s[75] = sensor_norm(w.terrain.slope_at(me->pos), 4);
        s[76] = static_cast<std::int8_t>(w.terrain.is_water(me->pos) ? 100 : -100);
        s[77] = sensor_norm(w.terrain.veg_at(me->pos), 8);
        s[78] = sensor_norm(w.terrain.wear_at(me->pos), 8);
        s[79] = sensor_norm(w.terrain.water_table_at(me->pos), 100);
    }
    // ---- channels 80..95: last action one-hot ----------------------------------
    if (last_action)
        std::copy(last_action->begin(), last_action->end(), s.begin() + 80);
    return s;
}

ActionVec sensor_action(const InteractionContext& ctx) {
    ActionVec a{};
    for (int i = 0; i < kActionVerbs; ++i)
        if (ctx.parameters.action == kVerbs[i]) { a[i] = 100; return a; }
    return a;
}

LabelVec sensor_label(const World& w, const CausalEvent& ev, EntityId actor,
                      const SensorVec& pre) {
    LabelVec y{};
    // ---- channels 0..15: intero deltas in channel units ------------------------
    std::array<std::int64_t, kInteroChannels> post;
    for (int i = 0; i < kInteroChannels; ++i) post[i] = pre[i];
    for (const StateChange& ch : ev.changes) {
        if (ch.entity != actor) continue;
        if (ch.before.type != ValType::Integer ||
            ch.after.type != ValType::Integer) continue;
        for (int i = 0; i < kInteroChannels; ++i) {
            if (ch.prop != intero_pid(w, i)) continue;
            // delta in raw prop units -> channel units via same scale
            const std::int64_t d =
                (ch.after.i - ch.before.i) * 127 / kInteroScale[i];
            std::int64_t v = static_cast<std::int64_t>(pre[i]) + d;
            v = std::max<std::int64_t>(-128, std::min<std::int64_t>(127, v));
            y[i] = static_cast<std::int8_t>(v - pre[i]);
            post[i] = v;
            break;
        }
    }
    // ---- channels 16..23: outcome classes ---------------------------------------
    y[16] = ev.failed ? static_cast<std::int8_t>(-100)
                      : static_cast<std::int8_t>(100);
    std::int64_t met = 0, unmet = 0;
    for (const Condition& c : ev.conditions) (c.met ? met : unmet)++;
    const std::int64_t tot = std::max<std::int64_t>(1, met + unmet);
    y[17] = static_cast<std::int8_t>(met * 127 / tot);
    y[18] = static_cast<std::int8_t>(unmet * 127 / tot);
    y[19] = sensor_norm(static_cast<std::int64_t>(ev.spawned.size()) * 2000, 1000);
    y[20] = sensor_norm(static_cast<std::int64_t>(ev.changes.size()) * 500, 1000);
    y[21] = sensor_norm(static_cast<std::int64_t>(ev.parent_events.size()) * 2000,
                        1000);
    y[22] = sensor_norm(static_cast<std::int64_t>(ev.evidence.size()) * 20, 1000);
    // y[23] reserved (0)
    // ---- channel 24: homeostatic reward -----------------------------------------
    // cost(channel) is low when healthy; reward = cost(pre) - cost(post),
    // positive when the action moved interoception toward setpoints.
    std::int64_t cost_pre = 0, cost_post = 0;
    for (int i = 0; i < kInteroChannels; ++i) {
        const std::int64_t ch_pre = pre[i];
        const std::int64_t bad_pre = kInteroSign[i] > 0 ? ch_pre : 127 - ch_pre;
        const std::int64_t bad_post =
            kInteroSign[i] > 0 ? post[i] : 127 - post[i];
        cost_pre += bad_pre; cost_post += bad_post;
    }
    std::int64_t r = (cost_pre - cost_post) / 8;
    y[24] = static_cast<std::int8_t>(std::max<std::int64_t>(
        -128, std::min<std::int64_t>(127, r)));
    return y;
}

std::int32_t sensor_homeo_cost(const SensorVec& s) {
    std::int32_t cost = 0;
    for (int i = 0; i < kInteroChannels; ++i) {
        const std::int64_t bad = kInteroSign[i] > 0 ? s[i] : 127 - s[i];
        cost += static_cast<std::int32_t>(bad);
    }
    return cost;
}

}  // namespace simlab
