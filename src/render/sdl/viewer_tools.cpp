#include "viewer.hpp"

namespace simlab_viewer {



    // ---- player agency: Resonance powers + Possess Avatar ----------------------
    // Every power writes AGENT KNOWLEDGE only (beliefs / memory engrams of
    // tribe minds); the minds decide what to do with it.
    std::vector<EntityId> Viewer::tribe_minds() const {
        std::vector<EntityId> out;
        const PropertyDef* ld = w.props.find_name("language_id");
        for (const auto& [id, m] : w.minds_) {
            (void)m;
            const Entity* e = w.find(id);
            if (!e || !e->alive || id == avatar) continue;
            if (ld && w.prop_int(*e, ld->id, 0) == tribe_lang()) out.push_back(id);
        }
        return out;
    }


    // the `n` tribe minds closest to p (deterministic: distance, id)
    std::vector<EntityId> Viewer::nearest_tribe(Vec2 p, std::size_t n) const {
        std::vector<std::pair<std::int64_t, EntityId>> v;
        for (EntityId id : tribe_minds()) v.push_back({dist2(w.find(id)->pos, p), id});
        std::sort(v.begin(), v.end());
        std::vector<EntityId> out;
        for (std::size_t i = 0; i < v.size() && i < n; ++i) out.push_back(v[i].second);
        return out;
    }


    void Viewer::do_whisper(EntityId id) {
        const auto& verbs = curiosity_verbs();
        if (verbs.empty()) return;
        if (!w.find_mind(id) || !w.find(id) || !w.find(id)->alive) return;
        if (!journal.spend(20)) { status_msg = "Not enough Resonance (20)"; return; }
        std::string key = std::string("omen:") + verbs[std::size_t(std::clamp<int>(whisper_verb, 0, int(verbs.size()) - 1))];
        if (whisper_mat > 0 && whisper_mat <= int(w.materials.defs().size()))
            key += ":" + std::to_string(w.materials.defs()[std::size_t(whisper_mat - 1)].id);
        Belief& b = w.mind(id).belief(key);
        b.confidence = std::min<std::int64_t>(1000, b.confidence + 700);
        b.last_tick = w.clock_.tick();
        status_msg = "Planted an idea: '" + key.substr(5) + "' - it may listen, or not";
    }


    void Viewer::whisper(EntityId id) { post([=, this] { do_whisper(id); }); }


    // Instill Fear: every tribe mind that REMEMBERS the thing feels dread of it
    void Viewer::do_instill_fear(EntityId target) {
        if (!w.find(target)) return;
        if (!journal.spend(15)) { status_msg = "Not enough Resonance (15)"; return; }
        int touched = 0;
        for (EntityId id : tribe_minds()) {
            AgentMind& m = w.mind(id);
            auto it = m.memory.find(target);
            if (it == m.memory.end()) continue;
            it->second.engram.emotional_charge = std::min<std::int64_t>(it->second.engram.emotional_charge, -800);
            it->second.engram.stability = std::max<std::int64_t>(it->second.engram.stability, 4000);
            ++touched;
        }
        status_msg = "A dread of " + sid(target) + " spreads through " + std::to_string(touched) + " minds";
        if (!touched) { journal.resonance += 15; status_msg = "Nobody of your people remembers that thing"; }
    }


    void Viewer::instill_fear(EntityId target) { post([=, this] { do_instill_fear(target); }); }


    // Global mutation pressure: rewrite `mutation_rate` on every living
    // genome-bearing organism (influx templates keep their own until they
    // too are swept — the slider is a god-tool, not a law).
    void Viewer::apply_mutation_override() {          // caller holds mx
        const int mp = mut_permille.load();
        if (mp < 0) return;
        const PropId mr = SIMLAB_PID(w.props, "mutation_rate");
        if (mr == kNoProp) return;
        for (const auto& ep : w.entities) {
            Entity* e = ep.get();
            if (!e || !e->alive || e->genome.loci.empty()) continue;
            e->props.set(mr, Value::integer(mp));
        }
    }


    void Viewer::do_bless_clone(EntityId id) {
        const Entity* e = w.find(id);
        if (!e || !e->alive || e->genome.loci.empty()) {
            status_msg = "Bless: pick a living organism with a genome";
            return;
        }
        if (!w.brains_) { status_msg = "Bless: no brains allocated yet"; return; }
        const BrainGenome bg = brain_genome_of(w, *w.brains_, id);
        if (!bg.valid) { status_msg = "Bless: that mind has no brain yet"; return; }
        GenePoolEntry en;
        en.brain = bg; en.body = e->genome;
        en.kind = e->kind_name + ""; en.seen_tick = w.clock_.tick();
        gene_pool.bless(en);
        save_gene_pool();
        // spawn 4 lightly-mutated copies around the blessed one
        const PropId age_p = SIMLAB_PID(w.props, "age");
        const PropId adult_p = SIMLAB_PID(w.props, "adult");
        const std::int64_t adult_age =
            adult_p != kNoProp ? w.prop_int(*e, adult_p, 250) : 250;
        int made = 0;
        for (int i = 0; i < 4; ++i) {
            const Vec2 cp{e->pos.x + (i % 2 ? 28 : -28),
                          e->pos.y + (i < 2 ? -28 : 28)};
            PropertyBag pb = e->props;
            if (age_p != kNoProp)
                pb.set(age_p, Value::integer(adult_age));
            const EntityId nid =
                w.create_entity(e->kind_name + "", e->material, cp, pb);
            if (nid == kNoEntity) continue;
            if (Entity* ne = w.find(nid)) ne->genome = e->genome;
            BrainGenome cg = bg;
            brain_mutate_blocks(cg.slow.data(),
                                static_cast<std::int64_t>(cg.slow.size()),
                                w.world_seed, nid, w.clock_.tick(), 4, 350);
            brain_install_spawn(w, nid, cg);
            w.build_anatomy(nid);
            ++made;
        }
        status_msg = "Blessed " + sid(id) + " into the gene pool - " +
                     std::to_string(made) + " clones born";
    }


    void Viewer::bless_clone(EntityId id) { post([=, this] { do_bless_clone(id); }); }


    void Viewer::do_punish(EntityId id) {
        const Entity* e = w.find(id);
        if (!e || !e->alive) { status_msg = "Punish: no living entity"; return; }
        AgentMind& m = w.mind(id);   // get-or-create (god-tool, same as whisper)
        // Shock the three freshest memories aversive + spike the acute axes:
        // the WM replay and fast weights then consolidate "don't repeat".
        int marked = 0;
        for (auto it = m.memory.begin();
             it != m.memory.end() && marked < 3; ++it) {
            MemoryEngram& g = it->second.engram;
            g.emotional_charge = std::min<std::int64_t>(g.emotional_charge, -900);
            g.stability = std::max<std::int64_t>(g.stability, 6000);
            ++marked;
        }
        m.neuro.panic = std::min<std::int64_t>(1000, m.neuro.panic + 500);
        m.neuro.rage = std::min<std::int64_t>(1000, m.neuro.rage + 400);
        m.neuro.grief = std::min<std::int64_t>(1000, m.neuro.grief + 200);
        status_msg = "Punished " + sid(id) + ": " +
                     std::to_string(marked) + " memories turned aversive";
    }


    void Viewer::punish(EntityId id) { post([=, this] { do_punish(id); }); }


    // ---- God-Hand sandbox tools ---------------------------------------------
    // Every command is a deterministic world mutation issued under the lock
    // via post(): same inputs -> same state_hash, headless runs never see
    // them (the toolbar only exists in the GUI).
    // deterministic icon tint for a material — name-substring palette with
    // a stable hash fallback. Render-state only, never persisted.
    std::uint32_t Viewer::mat_icon_col(MaterialId m) const {
        const MaterialDef* md = m == kNoMaterial ? nullptr : w.materials.find(m);
        const std::string& n = md ? md->name : std::string();
        auto has = [&](const char* s) { return n.find(s) != std::string::npos; };
        if (has("water") || has("sap")) return 0xFF4FB8E8;
        if (has("berries") || has("herb") || has("moss") || has("grain") ||
            has("pulse") || has("sprout") || has("wool")) return 0xFF58B040;
        if (has("meat") || has("flesh") || has("blood") || has("milk")) return 0xFFE05858;
        if (has("metal") || has("bronze") || has("iron") || has("copper") ||
            has("gold") || has("tin") || has("alloy") || has("blade") ||
            has("plate")) return 0xFFD8A838;
        if (has("wood") || has("hide") || has("sinew") || has("cloth") ||
            has("debris") || has("wool")) return 0xFF9A6A3C;
        if (has("stone") || has("flint") || has("obsidian") || has("ore") ||
            has("rock") || has("marble") || has("granite") || has("basalt") ||
            has("host") || has("slate") || has("shale")) return 0xFF9A9AA8;
        if (has("charcoal") || has("coal") || has("ash") || has("soil") ||
            has("sand") || has("clay") || has("loam")) return 0xFF6A5A48;
        if (has("salt") || has("crystal") || has("quartz") || has("gem"))
            return 0xFFE8F0FF;
        std::uint32_t h = 2166136261u ^ std::uint32_t(m);
        h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
        const std::uint32_t r = 96 + ((h >> 16) & 0x9F);
        const std::uint32_t g = 96 + ((h >> 8) & 0x9F);
        const std::uint32_t b = 96 + (h & 0x9F);
        return 0xFF000000u | (r << 16) | (g << 8) | b;
    }


    AgentMind::MemorySnapshot Viewer::god_snap(const World& w, const Entity& e) {
        AgentMind::MemorySnapshot s;
        s.tick = w.clock_.tick(); s.pos = e.pos;
        s.material = e.material; s.kind_name = e.kind_name;
        for (PropId op : w.observable_props())
            if (const Value* v = w.prop_of(e, op)) s.props[op] = *v;
        return s;
    }


    // Forge/raise a brain slot and surge neuromodulators (god tools may
    // allocate a slot — the command itself is the deterministic input).
    void Viewer::god_mod_surge(EntityId id, int dopa, int sero, int cortisol,
                       int norepi, int joy) {
        if (!w.brains_) return;
        BrainSlot& bs = brain_slot(w, *w.brains_, id);
        auto bump = [](std::int16_t& v, int d, int lo, int hi) {
            v = static_cast<std::int16_t>(std::clamp<int>(int(v) + d, lo, hi)); };
        bump(bs.vital.mod[kBModDopa], dopa, 0, 1000);
        bump(bs.vital.mod[kBModSero], sero, 0, 1000);
        bump(bs.vital.mod[kBModCortisol], cortisol, 0, 1000);
        bump(bs.vital.mod[kBModNorepi], norepi, 0, 1000);
        bump(bs.vital.emo[kBEmoJoy], joy, -1000, 1000);
    }


    void Viewer::do_cupid(EntityId a, EntityId b) {
        const Entity* ea = w.find(a); const Entity* eb = w.find(b);
        if (!ea || !eb || !ea->alive || !eb->alive || a == b) {
            status_msg = "Cupid: pick two different living beings"; return; }
        if (!w.find_mind(a) || !w.find_mind(b)) {
            status_msg = "Cupid: only sapient minds can be kindled"; return; }
        if (!journal.spend(15)) { status_msg = "Not enough Resonance (15)"; return; }
        AgentMind& ma = w.mind(a); AgentMind& mb = w.mind(b);
        const MindParams mpa = mind_params(w, *ea), mpb = mind_params(w, *eb);
        // mutual deep-positive engrams (~bonded threshold is 650) — the
        // same channel natural courtship writes through, just kindled.
        upsert_memory(ma, b, god_snap(w, *eb), 800, mpa);
        upsert_memory(mb, a, god_snap(w, *ea), 800, mpb);
        god_mod_surge(a, 220, 200, 0, 0, 220);
        god_mod_surge(b, 220, 200, 0, 0, 220);
        status_msg = "Cupid's arrow: " + sid(a) + " <3 " + sid(b) +
                     " — watch whether they close the distance";
    }


    void Viewer::cupid(EntityId a, EntityId b) { post([=, this] { do_cupid(a, b); }); }


    void Viewer::do_instill_panic(EntityId id) {
        const Entity* e = w.find(id);
        if (!e || !e->alive) { status_msg = "Panic: no living entity"; return; }
        if (!journal.spend(10)) { status_msg = "Not enough Resonance (10)"; return; }
        AgentMind& m = w.mind(id);   // get-or-create (god-tool, same as punish)
        m.neuro.panic = std::min<std::int64_t>(1000, m.neuro.panic + 700);
        m.neuro.rage = std::min<std::int64_t>(1000, m.neuro.rage + 400);
        god_mod_surge(id, 0, 0, 400, 300, 0);   // cortisol + norepinephrine
        status_msg = "Drama instilled in " + sid(id) +
                     " — expect paranoia, flight or factional gossip";
    }


    void Viewer::instill_panic(EntityId id) { post([=, this] { do_instill_panic(id); }); }


    // deterministic pseudo-random ring offset for scatter spawns
    Vec2 Viewer::god_ring(Vec2 p, int k, int cell, int rad) {
        const int dx = int((k * 37) % (2 * rad + 1)) - rad;
        const int dy = int((k * 53) % (2 * rad + 1)) - rad;
        return Vec2{p.x + dx * cell, p.y + dy * cell};
    }


    // fx bodies (payment already taken by the do_* wrapper / bounty combo)
    void Viewer::rain_fx(Vec2 p) {
        const TerrainGrid& t = w.terrain;
        const int cell = t.cell > 0 ? t.cell : 16;
        int wetted = 0;
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx) {
                const Vec2 q{p.x + dx * cell, p.y + dy * cell};
                if (t.contains(q) && !t.is_water(q)) {
                    w.terrain.add_moisture(q, 140); ++wetted;
                }
            }
        // drinkable pools left behind (kind "water", stock quantity)
        const MaterialId wm = w.materials.id_of("water");
        const PropId qd = w.props.find_name("quantity")
            ? w.props.find_name("quantity")->id : kNoProp;
        PropertyBag pb;
        if (qd != kNoProp) pb.set(qd, Value::integer(3000));
        int pools = 0;
        for (int k = 0; k < 6; ++k) {
            const Vec2 q = god_ring(p, k + 11, cell, 2);
            if (!t.contains(q) || t.is_water(q)) continue;
            if (w.create_entity("water", wm, q, pb) != kNoEntity) ++pools;
        }
        status_msg = "Rain over the hollow: " + std::to_string(wetted) +
                     " cells moistened, " + std::to_string(pools) + " pools";
    }


    void Viewer::feast_fx(Vec2 p) {
        const TerrainGrid& t = w.terrain;
        const int cell = t.cell > 0 ? t.cell : 16;
        const MaterialId bm = w.materials.id_of("wild_berries");
        const PropId qd = w.props.find_name("quantity")
            ? w.props.find_name("quantity")->id : kNoProp;
        PropertyBag pb;
        if (qd != kNoProp) pb.set(qd, Value::integer(12));
        int made = 0;
        for (int k = 0; k < 8; ++k) {
            const Vec2 q = god_ring(p, k + 31, cell, 3);
            if (!t.contains(q) || t.is_water(q)) continue;
            if (w.create_entity("goods", bm, q, pb) != kNoEntity) ++made;
        }
        // a waterskin alongside the berries
        const MaterialId wm = w.materials.id_of("water");
        PropertyBag wpb;
        if (qd != kNoProp) wpb.set(qd, Value::integer(2000));
        for (int k = 0; k < 2; ++k) {
            const Vec2 q = god_ring(p, k + 47, cell, 2);
            if (t.contains(q) && !t.is_water(q) &&
                w.create_entity("water", wm, q, wpb) != kNoEntity) ++made;
        }
        status_msg = "Feast spread: " + std::to_string(made) +
                     " caches — sharing vs hoarding is now observable";
    }


    void Viewer::do_summon_rain(Vec2 p) {
        if (!journal.spend(20)) { status_msg = "Not enough Resonance (20)"; return; }
        rain_fx(p);
    }


    void Viewer::summon_rain(Vec2 p) { post([=, this] { do_summon_rain(p); }); }


    void Viewer::do_summon_feast(Vec2 p) {
        if (!journal.spend(30)) { status_msg = "Not enough Resonance (30)"; return; }
        feast_fx(p);
    }


    void Viewer::summon_feast(Vec2 p) { post([=, this] { do_summon_feast(p); }); }


    // Bounty: rain + feast in one divine gesture (single payment)
    void Viewer::do_bounty(Vec2 p) {
        if (!journal.spend(35)) { status_msg = "Not enough Resonance (35)"; return; }
        rain_fx(p); feast_fx(p);
        status_msg = "Bounty falls: rain and a feast — watch who shares, who hoards";
    }


    void Viewer::bounty(Vec2 p) { post([=, this] { do_bounty(p); }); }


    // Miracle: localized awe impulse. Pure physiology — sero/dopa/joy surge
    // plus cortisol relief for every sapient mind within 220 units, and a
    // strong positive engram toward the focal entity (picked target, else
    // nearest living thing to the tile). Whether they revere, fight over,
    // or ignore the spot is left entirely to the SNN/ STDP.
    void Viewer::do_miracle(Vec2 p, EntityId picked) {
        if (!journal.spend(25)) { status_msg = "Not enough Resonance (25)"; return; }
        // resolve focal: the picked entity if it is near the click,
        // else the nearest living entity to the tile (min id = deterministic)
        EntityId focal = kNoEntity;
        if (picked != kNoEntity) {
            const Entity* pe = w.find(picked);
            if (pe && pe->alive && dist2(pe->pos, p) < 96LL * 96) focal = picked;
        }
        if (focal == kNoEntity) {
            std::vector<EntityId> near = w.entities_near(p, 128);
            std::sort(near.begin(), near.end());
            for (EntityId nid : near) {
                const Entity* ne = w.find(nid);
                if (ne && ne->alive) { focal = nid; break; }
            }
        }
        int touched = 0;
        for (auto& [mid, mm] : w.minds_) {
            const Entity* me = w.find(mid);
            if (!me || !me->alive) continue;
            if (dist2(me->pos, p) > 220LL * 220) continue;
            auto sit = w.brains_ ? w.brains_->slots.find(mid)
                                 : w.brains_->slots.end();
            if (w.brains_ && sit != w.brains_->slots.end()) {
                BrainVital& bv = sit->second.vital;
                bv.mod[kBModSero] = static_cast<std::int16_t>(
                    std::clamp<int>(int(bv.mod[kBModSero]) + 160, 0, 1000));
                bv.mod[kBModDopa] = static_cast<std::int16_t>(
                    std::clamp<int>(int(bv.mod[kBModDopa]) + 120, 0, 1000));
                bv.emo[kBEmoJoy] = static_cast<std::int16_t>(
                    std::clamp<int>(int(bv.emo[kBEmoJoy]) + 150, -1000, 1000));
                bv.mod[kBModCortisol] = static_cast<std::int16_t>(
                    std::clamp<int>(int(bv.mod[kBModCortisol]) - 60, 0, 1000));
            }
            mm.neuro.panic = std::max<std::int64_t>(0, mm.neuro.panic - 60);
            if (focal != kNoEntity) {
                const Entity* fe = w.find(focal);
                if (fe) {
                    const MindParams mp = mind_params(w, *me);
                    upsert_memory(mm, focal, god_snap(w, *fe), 450, mp);
                }
            }
            ++touched;
        }
        status_msg = "Miracle at the hollow: " + std::to_string(touched) +
                     " minds felt the awe" +
                     (focal != kNoEntity ? " — a focal point is born" : "");
    }


    void Viewer::miracle(Vec2 p, EntityId picked) { post([=, this] { do_miracle(p, picked); }); }


    // Relic: drop a rare obsidian artifact — no props, no nutrition, no
    // script. Agents perceive it, form engrams, and the SNN decides whether
    // anyone claims, hoards, gifts or fights over it.
    void Viewer::do_relic(Vec2 p) {
        if (!journal.spend(30)) { status_msg = "Not enough Resonance (30)"; return; }
        const TerrainGrid& t = w.terrain;
        const int cell = t.cell > 0 ? t.cell : 16;
        const Vec2 q = god_ring(p, 7, cell, 2);
        if (!t.contains(q) || t.is_water(q)) {
            status_msg = "The relic needs dry ground"; return;
        }
        MaterialId m = w.materials.id_of("obsidian");
        if (m == kNoMaterial) m = w.materials.id_of("stone");
        if (m == kNoMaterial) { status_msg = "No relic material"; return; }
        PropertyBag pb;
        const PropId qd = w.props.find_name("quantity")
            ? w.props.find_name("quantity")->id : kNoProp;
        if (qd != kNoProp) pb.set(qd, Value::integer(1));
        if (w.create_entity("artifact", m, q, pb) != kNoEntity)
            status_msg = "An obsidian relic hums in the dust — who will claim it?";
        else status_msg = "The relic failed to take form";
    }


    void Viewer::relic(Vec2 p) { post([=, this] { do_relic(p); }); }


    // Tempest: localized storm shock — a moisture pulse plus thirst/chill
    // pressure and an acute stress spike for every mind in the radius.
    // Deterministic integers only; whether the tribe bands together via
    // focal_buffer co-regulation or fractures is left to the SNN.
    void Viewer::do_tempest(Vec2 p) {
        if (!journal.spend(40)) { status_msg = "Not enough Resonance (40)"; return; }
        const TerrainGrid& t = w.terrain;
        const int cell = t.cell > 0 ? t.cell : 16;
        int wetted = 0;
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx) {
                const Vec2 q{p.x + dx * cell, p.y + dy * cell};
                if (t.contains(q) && !t.is_water(q)) {
                    w.terrain.add_moisture(q, 90); ++wetted;
                }
            }
        int shocked = 0;
        for (auto& [mid, mm] : w.minds_) {
            const Entity* me = w.find(mid);
            if (!me || !me->alive) continue;
            if (dist2(me->pos, p) > 340LL * 340) continue;
            if (w.brains_) {
                auto sit = w.brains_->slots.find(mid);
                if (sit != w.brains_->slots.end()) {
                    BrainVital& bv = sit->second.vital;
                    bv.drive_thirst = static_cast<std::int16_t>(
                        std::clamp<int>(int(bv.drive_thirst) + 160, 0, 1000));
                    bv.drive_temp = static_cast<std::int16_t>(
                        std::clamp<int>(int(bv.drive_temp) + 120, 0, 1000));
                    bv.mod[kBModNorepi] = static_cast<std::int16_t>(
                        std::clamp<int>(int(bv.mod[kBModNorepi]) + 120, 0, 1000));
                    bv.mod[kBModCortisol] = static_cast<std::int16_t>(
                        std::clamp<int>(int(bv.mod[kBModCortisol]) + 110, 0, 1000));
                    bv.emo[kBEmoFear] = static_cast<std::int16_t>(
                        std::clamp<int>(int(bv.emo[kBEmoFear]) + 90, -1000, 1000));
                }
            }
            mm.neuro.panic = std::min<std::int64_t>(1000, mm.neuro.panic + 80);
            ++shocked;
        }
        status_msg = "Tempest: " + std::to_string(wetted) +
                     " cells stormed, " + std::to_string(shocked) +
                     " minds shaken — band together or fracture?";
    }


    void Viewer::tempest(Vec2 p) {
        post([=, this] { do_tempest(p); });
        // viewer-side lightning flash + wind streaks (render state only)
        storm_t0 = time_s;
        const float cx = cam.sx(float(p.x));
        const float cy = cam.sy(float(p.y), elev_at(p));
        for (int k = 0; k < 24 && parts.size() < 600; ++k) {
            const float j = float(k % 8) / 8.f - 0.5f;
            const float dir = (k % 2) ? 1.f : -1.f;
            parts.push_back({cx + j * 120.f * cam.zoom,
                cy + float(k / 8 - 1) * 40.f * cam.zoom,
                dir * (140.f + float(k % 5) * 30.f),
                float((k % 3) - 1) * 20.f, 0.7f, 0xCFE8FF, -20.f});
        }
    }


    void Viewer::do_summon_predator(Vec2 p) {
        if (!journal.spend(40)) { status_msg = "Not enough Resonance (40)"; return; }
        const TerrainGrid& t = w.terrain;
        const int cell = t.cell > 0 ? t.cell : 16;
        const SpeciesDef* sp = w.species.find_name("pack_stalker");
        if (!sp) sp = w.species.find_name("trail_hound");
        if (!sp) { status_msg = "Predator: no predator species in data"; return; }
        // nearest dry land to the click
        Vec2 q = p; bool found = false;
        for (int r = 0; r <= 6 && !found; ++r)
            for (int k = 0; k < 8 * (r + 1) && !found; ++k) {
                const Vec2 c = god_ring(p, k, cell, r);
                if (t.contains(c) && !t.is_water(c)) { q = c; found = true; }
            }
        if (!found) { status_msg = "Predator: no dry land in reach"; return; }
        PropertyBag pb = sp->props;
        const EntityId id = w.create_entity(sp->name, sp->material, q, pb);
        if (id == kNoEntity) { status_msg = "Predator: spawn rejected"; return; }
        Entity* e = w.find(id);
        // species gene loci at baseline (same as a worldgen individual,
        // minus the per-capita hash deviation)
        for (PropId g : sp->genes) {
            const PropertyDef* pd = w.props.find(g);
            if (!pd) continue;
            const Value* cur = e->prop(w, g);
            std::int64_t v = cur ? cur->i : 0;
            if (pd->has_range) v = std::clamp(v, pd->lo, pd->hi);
            e->genome.set(g, v); e->props.set(g, Value::integer(v));
        }
        w.build_anatomy(id);
        status_msg = "A " + sp->name + " stalks the land — tribal defense, or dinner, will decide";
    }


    void Viewer::summon_predator(Vec2 p) { post([=, this] { do_summon_predator(p); }); }


    void Viewer::do_place_beacon(Vec2 p) {
        if (!journal.spend(10)) { status_msg = "Not enough Resonance (10)"; return; }
        const auto scouts = nearest_tribe(p, 2);
        for (EntityId id : scouts) {
            Belief& b = w.mind(id).belief("beacon:" + std::to_string(p.x) + ":" + std::to_string(p.y));
            b.confidence = 800; b.last_tick = w.clock_.tick();
        }
        status_msg = "Beacon lit: " + std::to_string(scouts.size()) + " scouts feel drawn to it";
    }


    void Viewer::place_beacon(Vec2 p) { post([=, this] { do_place_beacon(p); }); }


    void Viewer::do_place_blueprint(Vec2 c, std::int64_t r) {
        if (!journal.spend(25)) { status_msg = "Not enough Resonance (25)"; return; }
        r = std::clamp<std::int64_t>(r, 48, 220);
        const auto builders = nearest_tribe(c, 3);
        for (EntityId id : builders) {
            Belief& b = w.mind(id).belief("site:" + std::to_string(c.x) + ":" + std::to_string(c.y) + ":" + std::to_string(r));
            b.confidence = 900; b.last_tick = w.clock_.tick();
        }
        status_msg = "Blueprint dreamt by " + std::to_string(builders.size()) + " builders (they still need loose stone/wood there)";
    }


    void Viewer::place_blueprint(Vec2 c, std::int64_t r) { post([=, this] { do_place_blueprint(c, r); }); }


    void Viewer::do_avatar_act(const std::string& verb) {
        const Entity* me = w.find(avatar);
        if (!me || !me->alive) { status_msg = "Avatar is gone"; return; }
        InteractionContext c;
        c.actor = avatar; c.parameters.action = verb; c.parameters.ticks = 1; c.parameters.force = 100000;
        const bool self_only = verb == "rest" || verb == "evacuate" || verb == "found";
        const bool pair = verb == "assemble" || verb == "combine";
        if (!self_only) {
            for (EntityId t : av_targets) {
                if (t == avatar || !w.find(t)) continue;
                c.targets.push_back(t);
                if (c.targets.size() >= (pair ? 2u : 1u)) break;
            }
            if (c.targets.empty()) { status_msg = "Pick a target first (click something)"; return; }
        }
        if (verb == "engrave") {   // carve the avatar's strongest verified knowledge
            if (const AgentMind* am = w.find_mind(avatar)) {
                std::int64_t best = -1;
                for (const auto& [k, rec] : am->discoveries)
                    if (rec.status == DiscoveryStatus::Verified && rec.evidence_for > best) { best = rec.evidence_for; c.parameters.note = k; }
            }
            if (c.parameters.note.empty()) { status_msg = "Nothing verified worth carving yet"; return; }
        }
        const InteractionResult r = w.interact(c);
        status_msg = verb + (r.completed ? ": done" : ": failed - " + r.failure);
    }


    void Viewer::avatar_act(const std::string& verb) { post([v = verb, this] { do_avatar_act(v); }); }


    void Viewer::do_avatar_move(Vec2 dest) {
        const Entity* me = w.find(avatar);
        if (!me || !me->alive) return;
        InteractionContext c;
        c.actor = avatar; c.parameters.action = "move"; c.parameters.ticks = 1;
        c.parameters.destination = dest; c.parameters.has_destination = true;
        const InteractionResult r = w.interact(c);
        status_msg = r.completed ? "walking" : "move failed - " + r.failure;
    }


    void Viewer::avatar_move(Vec2 dest) { post([=, this] { do_avatar_move(dest); }); }


    void Viewer::set_possess(EntityId id) { post([=, this] { do_set_possess(id); }); }


    void Viewer::do_set_possess(EntityId id) {
        if (avatar != kNoEntity) dstate.possessed.erase(avatar);
        avatar = id; av_targets.clear();
        if (id != kNoEntity) {
            dstate.possessed.insert(id);
            fog.set_mode(FogOfWar::Mode::Avatar, std::int64_t(id));   // its eyes only
            need_successor = false;
        } else {
            fog.set_mode(FogOfWar::Mode::Tribe, tribe_lang());
        }
        status_msg = id != kNoEntity ? "Possessing " + sid(id) + " - you see only what it sees"
                                     : "Released to its own mind";
    }


    // ---- action bar (bottom-left): powers, or the possessed body's verbs ----
    void Viewer::ui_player() {
        ImGui::SetNextWindowPos(ImVec2(10, float(win_h) - 10), ImGuiCond_Always, ImVec2(0, 1));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));
        ImGui::Begin("##actions", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        if (avatar != kNoEntity) {
            ImGui::TextColored(ImVec4(0.6f, 1, 0.6f, 1), "You are %s", sid(avatar).c_str());
            ImGui::SameLine(); ImGui::TextDisabled("  %s", c_av_line.c_str());
            int n = 0;
            for (const std::string& v : c_av_verbs) {
                if (v == "move") continue;
                if (n % 6) ImGui::SameLine(0, 4);
                ++n;
                if (ImGui::Button(v.c_str(), ImVec2(90, 0))) avatar_act(v);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Key %d - acts on the thing you last clicked", n);
            }
            ImGui::TextDisabled("WASD walk   click: target (%zu picked)", av_targets.size());
            ImGui::SameLine();
            if (ImGui::SmallButton("Release")) set_possess(kNoEntity);
            ImGui::End();
            ImGui::PopStyleVar();
            return;
        }
        auto power = [&](const char* label, int t, int cost, const char* tip) {
            const bool on = tool == t;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.48f, 0.70f, 1));
            ImGui::BeginDisabled(journal.resonance < cost && !on);
            if (ImGui::Button(label, ImVec2(104, 0))) tool = on ? kToolNone : t;
            ImGui::EndDisabled();
            if (on) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s (costs %d)", tip, cost);
        };
        power("Beacon", kToolBeacon, 10, "Click the map: two scouts feel drawn there");
        ImGui::SameLine(0, 4);
        power("Blueprint", kToolBlueprint, 25, "Drag a circle: builders dream of a shelter there");
        ImGui::SameLine(0, 4);
        power("Dread", kToolFear, 15, "Click a thing: your people who remember it come to fear it");
        ImGui::SameLine(0, 4);
        if (ImGui::SmallButton(god_open ? "God-Hand v" : "God-Hand >")) god_open = !god_open;
        if (god_open) {
            // second row: sandbox powers (deterministic world commands)
            power("Cupid", kToolCupid, 15, "Click two sapients: kindle a mutual bond");
            ImGui::SameLine(0, 4);
            power("Drama", kToolPanic, 10, "Click an agent: jealousy/paranoia spike — gossip & chaos");
            ImGui::SameLine(0, 4);
            power("Rain", kToolRain, 20, "Click terrain: moisture pulse + drinkable pools");
            ImGui::SameLine(0, 4);
            power("Feast", kToolFeast, 30, "Click terrain: berry caches + water — sharing vs hoarding");
            ImGui::SameLine(0, 4);
            power("Predator", kToolPredator, 40, "Click terrain: a wild pack_stalker arrives");
            ImGui::SameLine(0, 4);
            power("Miracle", kToolMiracle, 25, "Click tile/entity: awe impulse — gold beam, nearby minds feel it");
            ImGui::SameLine(0, 4);
            power("Bounty", kToolBounty, 35, "Click terrain: rain + feast caches — sharing vs hoarding");
            ImGui::SameLine(0, 4);
            power("Relic", kToolRelic, 30, "Click terrain: drop an obsidian artifact — who claims it?");
            ImGui::SameLine(0, 4);
            power("Tempest", kToolTempest, 40, "Click terrain: storm shock — thirst/chill/stress pulse");
        }
        if (tool != kToolNone) {
            const std::string hint =
                tool == kToolBlueprint ? "Drag on the map - right click cancels"
                : tool == kToolCupid ? "Cupid: click two sapients" +
                    std::string(cupid_a != kNoEntity ? " (second pick)" : " (first pick)") +
                    " - right click cancels"
                : "Click the map - right click cancels";
            ImGui::TextColored(ImVec4(0.6f, 0.82f, 1, 1), "%s", hint.c_str());
        }
        if (insp.found && insp.has_mind) {
            ImGui::Separator();
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s", sid(selected).c_str());
            ImGui::SameLine();
            ImGui::BeginDisabled(journal.resonance < 20);
            if (ImGui::Button("Plant idea", ImVec2(96, 0))) ImGui::OpenPopup("idea");
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Whisper a hint into a dream (costs 20)");
            ImGui::SameLine(0, 4); if (ImGui::Button("Possess", ImVec2(80, 0))) set_possess(selected);
            ImGui::SameLine(0, 4);
            const bool pinned = journal.pins.count(selected) != 0;
            if (ImGui::Button(pinned ? "Unpin" : "Pin", ImVec2(60, 0))) { if (pinned) journal.pins.erase(selected); else journal.pins.insert(selected); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Follow this life in Journal > Legends");
            ImGui::SameLine(0, 4);
            if (ImGui::Button("Bless", ImVec2(52, 0))) bless_clone(selected);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hall-of-fame its brain + spawn 4 mutated clones");
            ImGui::SameLine(0, 4);
            if (ImGui::Button("Punish", ImVec2(56, 0))) punish(selected);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Aversive shock: recent memories + acute stress axes");
            if (ImGui::BeginPopup("idea")) {
                ImGui::TextDisabled("Make it wonder about...");
                const auto& verbs = curiosity_verbs();
                ImGui::SetNextItemWidth(150);
                if (ImGui::BeginCombo("action", verbs.empty() ? "" : verbs[std::size_t(std::clamp<int>(whisper_verb, 0, int(verbs.size()) - 1))])) {
                    for (int i = 0; i < int(verbs.size()); ++i) if (ImGui::Selectable(verbs[std::size_t(i)], i == whisper_verb)) whisper_verb = i;
                    ImGui::EndCombo();
                }
                const auto& mats = w.materials.defs();
                const char* mname = whisper_mat > 0 && whisper_mat <= int(mats.size()) ? mats[std::size_t(whisper_mat - 1)].name.c_str() : "anything";
                ImGui::SetNextItemWidth(150);
                if (ImGui::BeginCombo("on", mname)) {
                    if (ImGui::Selectable("anything", whisper_mat == 0)) whisper_mat = 0;
                    for (int i = 0; i < int(mats.size()); ++i)
                        if (ImGui::Selectable(mats[std::size_t(i)].name.c_str(), whisper_mat == i + 1)) whisper_mat = i + 1;
                    ImGui::EndCombo();
                }
                if (ImGui::Button("Whisper (20)", ImVec2(-1, 0))) { whisper(selected); ImGui::CloseCurrentPopup(); }
                ImGui::EndPopup();
            }
        } else if (insp.found) {
            ImGui::Separator();
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s", sid(selected).c_str());
            ImGui::SameLine();
            ImGui::BeginDisabled(journal.resonance < 15);
            if (ImGui::Button("Dread of this", ImVec2(120, 0))) instill_fear(selected);
            ImGui::EndDisabled();
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }

}  // namespace simlab_viewer
