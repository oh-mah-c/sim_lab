#include "viewer.hpp"

namespace simlab_viewer {



    float Viewer::elev_at(Vec2 p) const {
        return w.terrain.contains(p) ? float(w.terrain.elev_at(p)) : 0.f;
    }



    void Viewer::emit(float x, float y, std::uint32_t c, int n) {
        for (int i = 0; i < n && parts.size() < 600; ++i) {
            const float a = float((frames * 7 + i * 53) % 628) / 100.f;
            parts.push_back({x, y, std::cos(a) * 30, std::sin(a) * 30 - 40, 0.6f, c});
        }
    }



    // enclosure: a ring of auto-tiled 2.5D wall segments around the footprint
    void Viewer::draw_enclosure(const EntityView& e) {
        const int cell = snap.cell;
        const int rc = std::max<int>(1, int(e.radius / cell));
        const int gx0 = TerrainGrid::cell_of(e.pos.x, cell), gy0 = TerrainGrid::cell_of(e.pos.y, cell);
        auto on_ring = [&](int dx, int dy) {
            const int d2 = dx * dx + dy * dy;
            return d2 <= rc * rc + rc && d2 >= (rc - 1) * (rc - 1) + (rc - 1);
        };
        const int crack = e.integrity >= 800 ? 0 : e.integrity >= 550 ? 1 : e.integrity >= 300 ? 2 : 3;
        const MaterialId mat = e.material != kNoMaterial ? e.material : w.materials.id_of("stone");
        for (int dy = -rc; dy <= rc; ++dy)
            for (int dx = -rc; dx <= rc; ++dx) {
                if (!on_ring(dx, dy)) continue;
                if (e.barrier == 0 && e.integrity < 300 && ((dx + dy) & 1)) continue;   // breached gaps
                std::uint8_t mask = 0;
                const int nb[8][2] = {{0,-1},{1,-1},{1,0},{1,1},{0,1},{-1,1},{-1,0},{-1,-1}};
                for (int k = 0; k < 8; ++k) if (on_ring(dx + nb[k][0], dy + nb[k][1])) mask |= 1 << k;
                const Sprite& s = atlas.wall(mask, crack, mat);
                const Vec2 wp{(gx0 + dx) * cell, (gy0 + dy) * cell};
                SDL_FRect dst{cam.sx(float(wp.x)), cam.sy(float(wp.y), elev_at(wp)) - cell * cam.zoom * 0.35f,
                              cell * cam.zoom, cell * cam.zoom};
                SDL_Texture* t = tex.get(s);
                if (e.fog == Fog::Shrouded) SDL_SetTextureColorMod(t, 120, 120, 135);
                SDL_RenderTexture(ren, t, nullptr, &dst);
                SDL_SetTextureColorMod(t, 255, 255, 255);
            }
        if (lens == kMilitary || lens == kSacred || selected == e.id) {
            // function label: inferred by the engine, not stored
            ImGui::GetForegroundDrawList()->AddText(ImVec2(ux(cam.sx(float(e.pos.x))) - 20, ux(cam.sy(float(e.pos.y), elev_at(e.pos))) - 28),
                e.sacred ? IM_COL32(255, 210, 120, 255) : IM_COL32(230, 220, 200, 255),
                (e.function + (lens == kMilitary ? " " + std::to_string(e.integrity) : std::string())).c_str());
        }
        // etched stones glow faintly cyan — readable at night like sacred fire
        if ((e.sacred || !e.text.empty()) && e.fog == Fog::Visible)
            draw_glow(e.pos, e.sacred ? 0xFFC060 : 0x70C8FF,
                      e.sacred ? 2.0f : 1.1f);
    }



    void Viewer::draw_glow(Vec2 p, std::uint32_t rgb, float scale) {
        const float s = 64 * scale * cam.zoom;
        SDL_FRect d{cam.sx(float(p.x)) - s / 2, cam.sy(float(p.y), elev_at(p)) - s / 2, s, s};
        const int hour = snap.day_phase;
        const int night = (hour < 6 || hour >= 19) ? 255 : (hour < 8 || hour >= 17) ? 140 : 50;
        SDL_SetTextureColorMod(glow, (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
        SDL_SetTextureAlphaMod(glow, Uint8(night));
        SDL_RenderTexture(ren, glow, nullptr, &d);
    }



    // ---- procedural character animation (sapients only) -------------------
    // RimWorld-style overlay pass in world_rt px on top of the creature
    // sprite: expressive pixel eyes driven by live neuromodulators (fear ->
    // wide jittering, dopamine/joy -> squint, exhaustion -> heavy lids), two
    // articulated hands posed by the freshest action verb, and floating "z"
    // motes for sleepers. Pure presentation — reads only c_anim viewer state.
    void Viewer::draw_actor_anim(const EntityView& e, AnimState& an, float fx, float fy,
                         float sw, float sh, bool gliding, bool lying) {
        if (lying) {
            // sleeper: occasional rising "z" (glyphs drawn in the ImGui pass)
            if (time_s - an.z_at > 0.8f && zeds.size() < 48) {
                an.z_at = float(time_s);
                zeds.push_back({fx + sw * 0.30f, fy - sh * 0.60f, 0.f});
            }
            return;
        }
        const bool fresh = an.until > c_tick;
        const std::string& v = an.verb;
        const bool eating = fresh && (v == "consume" || v == "eat" || v == "forage");
        const bool attacking = fresh && (v == "strike" || v == "launch" || v == "attack");
        const bool lovin = fresh && (v == "reproduce" || v == "mate");
        const float hr = std::max(1.4f, sw * 0.055f);          // hand half-size
        const bool gathered = gather_of.count(e.id) != 0;
        const float breathe = std::sin(float(time_s) * 2.2f +
            (gathered ? 0.f : float(e.id % 7))) * 0.7f;   // ritual circle breathes as one
        const float swing = std::sin(an.phase);                 // gait -1..1
        // ---- hands -------------------------------------------------------
        for (int i = 0; i < 2; ++i) {
            const float side = i == 0 ? -1.f : 1.f;
            float hx = fx + side * sw * 0.30f;
            float hy = fy - sh * 0.42f + breathe;
            if (gliding) {                                     // alternating arm swing
                hx += side * swing * sw * 0.05f;
                hy += swing * (i ? -1.f : 1.f) * sh * 0.06f;
            }
            if (lovin) {                                     // happy wiggle
                hx += std::sin(float(time_s) * 7.f + float(i) * 3.14159f) * sw * 0.05f;
                hy += std::sin(float(time_s) * 9.f + float(i)) * sh * 0.04f;
            }
            if (eating) {                                      // raise to mouth, chew
                hx = fx + side * sw * 0.10f;
                hy = fy - sh * 0.70f +
                     std::sin(float(time_s) * 11.f + float(i) * 3.14159f) * sh * 0.05f;
            } else if (attacking) {                            // slashing lunge
                const float dir = std::cos(an.ang) >= 0.f ? 1.f : -1.f;
                const float jab = std::max(0.f, std::sin(float(time_s) * 13.f + float(i) * 3.14159f));
                hx += dir * jab * sw * 0.14f * (i ? 0.6f : 1.f);
                hy -= jab * sh * 0.10f;
            }
            // carried/wielded item chip rides the right hand — material
            // tint resolved in poll_world (World Truth, under the lock)
            std::uint32_t held_col = 0;
            if (i == 1) {
                auto wit = c_wield_col.find(e.id);
                if (wit != c_wield_col.end()) held_col = wit->second;
                else {
                    auto hit = c_held.find(e.id);
                    if (hit != c_held.end() && !hit->second.empty())
                        held_col = hit->second.front().col;
                }
            }
            if (held_col) {
                SDL_FRect chip{hx - hr * 0.9f, hy - hr * 1.55f,
                               hr * 1.8f, hr * 1.35f};
                set_color(ren, held_col); SDL_RenderFillRect(ren, &chip);
                set_color(ren, 0xFF202028); SDL_RenderRect(ren, &chip);
            } else {
                SDL_FRect hd{hx - hr, hy - hr, hr * 2.f, hr * 2.f};
                set_color(ren, 0xFFBE9678); SDL_RenderFillRect(ren, &hd);
                set_color(ren, 0xFF8A6248); SDL_RenderRect(ren, &hd);
            }
        }
        // ---- face ---------------------------------------------------------
        const float gaze_x = std::cos(an.ang) * hr * 0.9f;
        const float gaze_y = std::sin(an.ang) * hr * 0.4f;
        float ex = sw * 0.16f;                             // half eye spacing
        if (an.argue > 0.5f) ex *= 0.55f;                   // argument: lean-in
        float er = std::max(1.3f, sw * 0.085f);                  // eye radius
        er *= 1.f + an.fear * 0.55f;                            // fear -> wide eyes
        const float jit = an.fear * std::sin(float(time_s) * 43.f + float(e.id)) * 0.8f +
            (an.argue > 0.5f ? std::sin(float(time_s) * 31.f + float(e.id)) * 1.1f : 0.f);
        const float hx = fx + gaze_x * 0.4f, hy = fy - sh * 0.80f;
        const float lid_k = an.tired > 0.55f ? 1.f - 0.45f * an.tired : 1.f;
        for (int i = 0; i < 2; ++i) {
            const float exx = hx + (i ? ex : -ex) + jit;
            const float eyy = hy + gaze_y * 0.3f;
            if (an.joy > 0.55f && an.fear < 0.4f) {              // happy squint
                SDL_FRect lid{exx - er * 1.4f, eyy - er * 0.35f,
                              er * 2.8f, std::max(1.f, er * 0.55f)};
                set_color(ren, 0xFF2A1E18); SDL_RenderFillRect(ren, &lid);
                continue;
            }
            SDL_FRect wht{exx - er, eyy - er * lid_k, er * 2.f, er * 2.f * lid_k};
            set_color(ren, 0xFFF6EEDC); SDL_RenderFillRect(ren, &wht);
            if (lid_k < 1.f) {                                  // heavy eyelid
                SDL_FRect lid{wht.x, wht.y, wht.w, wht.h * (1.f - lid_k) * 1.6f};
                set_color(ren, 0xFFBE9678); SDL_RenderFillRect(ren, &lid);
            }
            const float pr = er * 0.55f;                        // pupil w/ gaze
            SDL_FRect pu{exx + gaze_x - pr, eyy + gaze_y - pr, pr * 2.f, pr * 2.f};
            set_color(ren, 0xFF241A12); SDL_RenderFillRect(ren, &pu);
        }
        // rosy cheeks: dopamine/joy spike (lovin', sharing, feasting)
        if (an.joy > 0.5f && an.fear < 0.4f) {
            const float cr = std::max(1.2f, sw * 0.05f);
            for (int i = 0; i < 2; ++i) {
                const float cx = hx + (i ? ex * 0.85f : -ex * 0.85f);
                SDL_FRect ch{cx - cr, hy + er * 1.9f - cr, cr * 2.f, cr * 2.f};
                set_color(ren, 0xFFE89090, int(150 + an.joy * 80));
                SDL_RenderFillRect(ren, &ch);
            }
        }
        if (eating) {                                           // open mouth
            SDL_FRect mo{hx - er, hy + er * 2.4f, er * 2.f, er * 1.2f};
            set_color(ren, 0xFF5A1E1E); SDL_RenderFillRect(ren, &mo);
        }
        // hit-flash decal: brief white bloom over the sprite
        if (time_s < an.flash) {
            const float a = std::clamp((an.flash - float(time_s)) / 0.18f, 0.f, 1.f);
            set_color(ren, 0xFFFFFFFF, int(140 * a));
            SDL_FRect fl{fx - sw * 0.55f, fy - sh * 1.05f, sw * 1.1f, sh * 1.1f};
            SDL_RenderFillRect(ren, &fl);
        }
    }



    void Viewer::draw_entities(double dt) {
        // hover pick: nearest living entity to the cursor (world_rt px)
        hover_id = kNoEntity;
        {
            const ImVec2 mp = ImGui::GetIO().MousePos;
            if (mp.x >= 0 && show_badges) {
                const float mxx = mp.x / kPix, myy = mp.y / kPix;
                float best = 28.f * 28.f;
                for (const EntityView& e : snap.entities) {
                    if (!e.alive || e.fog != Fog::Visible) continue;
                    const float fx = cam.sx(float(e.pos.x)), fy = cam.sy(float(e.pos.y), elev_at(e.pos));
                    const float d = (fx - mxx) * (fx - mxx) + (fy - myy) * (fy - myy);
                    if (d < best) { best = d; hover_id = e.id; }
                }
            }
        }
        for (const EntityView& e : snap.entities) {
            if (e.kind == "enclosure") { draw_enclosure(e); continue; }
            // ---- smooth visual position: exponential chase of sim pos ----
            // (viewer-side only; sim positions stay integer World Truth)
            const float tx = float(e.pos.x), ty = float(e.pos.y);
            auto vit = vis_pos.find(e.id);
            if (vit == vis_pos.end())
                vit = vis_pos.emplace(e.id, ImVec2(tx, ty)).first;
            else {
                const float ddx = tx - vit->second.x, ddy = ty - vit->second.y;
                if (ddx * ddx + ddy * ddy > 400.f * 400.f)      // spawn/teleport: snap
                    vit->second = ImVec2(tx, ty);
                else {
                    const float k = 1.f - std::exp(-float(dt) * 12.f);
                    vit->second.x += ddx * k; vit->second.y += ddy * k;
                }
            }
            const auto pit = vis_prev.find(e.id);
            const ImVec2 pp = pit == vis_prev.end() ? vit->second : pit->second;
            const float mvx = vit->second.x - pp.x, mvy = vit->second.y - pp.y;
            vis_prev[e.id] = vit->second;
            if (std::fabs(mvx) > 0.02f) facing[e.id] = mvx < 0 ? -1.f : 1.f;   // face travel dir
            const float spd = std::sqrt(mvx * mvx + mvy * mvy) / std::max(1e-3f, float(dt));
            const bool gliding = spd > 12.f;                     // world units / sec
            const float elev = elev_at(Vec2{std::int32_t(vit->second.x), std::int32_t(vit->second.y)});
            const float fx = cam.sx(vit->second.x), fy = cam.sy(vit->second.y, elev);
            // ---- procedural animation state (sapients, viewer-only) -------
            AnimState* an = nullptr;
            bool lying = false;
            double lean = 0.0;
            if (e.minded && e.alive && e.fog == Fog::Visible) {
                auto ait = c_anim.find(e.id);
                if (ait != c_anim.end()) an = &ait->second;
            }
            if (an) {
                const bool mourn_gait = an->mourning && an->expr_until > c_tick;
                an->phase += float(dt) * (gliding ? (mourn_gait ? 5.f : 9.f)
                                                   : (mourn_gait ? 0.9f : 2.f));
                if (!gliding && gather_of.count(e.id))      // shared ritual sway
                    an->phase = std::fmod(float(time_s) * 2.f, 6.2831853f);
                // facing: remembered/intent target beats raw velocity; the
                // angle is smoothed so characters visibly turn toward mates,
                // water and predators instead of snapping left/right.
                float want = an->ang;
                bool has_dir = false;
                if ((an->until > c_tick || an->expr_until > c_tick) &&
                    an->target != kNoEntity && an->tpos_ok) {
                    const float sx2 = cam.sx(float(an->tpos.x));
                    const float sy2 = cam.sy(float(an->tpos.y), elev_at(an->tpos));
                    const float ddx = sx2 - fx, ddy = sy2 - fy;
                    if (ddx * ddx + ddy * ddy > 9.f) { want = std::atan2(ddy, ddx); has_dir = true; }
                }
                if (!has_dir && (std::fabs(mvx) > 0.03f || std::fabs(mvy) > 0.03f)) {
                    want = std::atan2(mvy, mvx); has_dir = true;
                }
                if (has_dir) {
                    float d = want - an->ang;
                    while (d > 3.14159265f) d -= 6.2831853f;
                    while (d < -3.14159265f) d += 6.2831853f;
                    an->ang += d * std::min(1.f, float(dt) * 9.f);
                }
                if (gliding) {                                     // body lean into motion
                    const float mdir = spd > 1.f ? mvx / (spd * float(dt)) : 0.f;
                    lean = std::clamp(mdir, -1.f, 1.f) * 6.0;
                }
                lying = an->lying && an->expr_until > c_tick;
                // expanding ripple while wading (throttled per entity)
                if (gliding && an->in_water && frames - an->last_puff > 18 && rings.size() < 48) {
                    an->last_puff = frames;
                    rings.push_back({fx, fy - cam.zoom, 2.f, 16.f, 0.8f, 0.8f, 0xFF7FC8E8});
                }
            }
            // ---- comedy body-states (viewer-only) -------------------------
            const bool just_ate = an && an->until > c_tick &&
                (an->verb == "consume" || an->verb == "eat" || an->verb == "forage");
            const bool tantrum_now = an && float(time_s) < an->tantrum_until;
            const bool starving = an && an->tired > 0.85f;   // hunger/thirst critical
            const bool mourn_now = an && an->mourning && an->expr_until > c_tick;
            const bool proud_now = an && an->proud > 0.5f && an->expr_until > c_tick;
            const float bounce = tantrum_now
                ? std::fabs(std::sin(float(time_s) * 18.f + float(e.id % 5))) * 3.f * cam.zoom
                : 0.f;                                       // stomp hop
            // motion memory -> bob / flip (viewer-side, never World state)
            auto lp = last_pos.find(e.id);
            if (lp != last_pos.end() && (lp->second.x != e.pos.x || lp->second.y != e.pos.y)) {
                bob_until[e.id] = float(time_s) + 0.8f;
            }
            last_pos[e.id] = e.pos;
            // damage feedback -> particles
            auto li = last_integ.find(e.id);
            if (li != last_integ.end() && e.integrity < li->second)
                emit(fx, fy - 12 * cam.zoom, e.flora ? 0xFF6A9A40 : 0xFFB01818, 8);
            last_integ[e.id] = e.integrity;
            auto lpn = last_pain.find(e.id);
            if (lpn != last_pain.end() && e.pain > lpn->second) emit(fx, fy - 16 * cam.zoom, 0xFFFFD060, 6);
            last_pain[e.id] = e.pain;

            const Sprite* sp = nullptr;
            if (!e.species.empty() && e.flora && e.fruit_max > 0)
                sp = &atlas.fruiting(e.species, int(e.fruit), int(e.fruit_max));   // real fruit dots
            else if (!e.species.empty()) sp = atlas.species(e.species);
            else if (e.kind == "inscription") sp = &atlas.glyph_stone(e.lang_seed, e.text, false);
            else if (e.minded) sp = atlas.kind("person");
            else if (atlas.kind(e.kind)) sp = atlas.kind(e.kind);
            else if (e.material != kNoMaterial) sp = atlas.material_icon(e.material);
            else sp = atlas.kind("goods");
            if (!sp || sp->empty()) continue;
            const bool creature = e.fauna || e.minded;
            float sw = sp->w * cam.zoom, sh = sp->h * cam.zoom;
            float squash = 1.f, ox = 0, sox = 0;
            const bool moving = gliding || (bob_until.count(e.id) && bob_until[e.id] > time_s);
            if (just_ate) { sw *= 1.13f; sh *= 1.05f; }        // food baby belly
            if (proud_now) {                                   // dopamine chest-puff
                const float pp = 1.f + 0.035f * std::sin(float(time_s) * 5.f);
                sw *= pp; sh *= pp;
            }
            if (mourn_now) { sw *= 0.97f; sh *= 0.98f; }        // grief slouch
            if (creature && e.alive && moving) {               // squash & stretch hop
                const float ph = std::sin(float(time_s) * 12.f + float(e.id % 17));
                squash = 1.f + 0.08f * ph; sh *= squash; sw /= squash;
            } else if (creature && e.alive) {                  // idle breathing
                sh *= 1.f + 0.018f * std::sin(float(time_s) * 2.2f + float(e.id % 31));
            }
            if (e.flora && !creature && e.alive && e.fog != Fog::Shrouded)   // foliage sway (frozen memory under the shroud)
                sox = std::sin(float(time_s) * 1.25f + float((e.id * 2654435761u >> 8) % 1024) / 163.f) * 1.4f * cam.zoom;
            if (creature && (e.infected || e.toxin > 300 || e.temperature > 385 || starving))
                ox = std::sin(float(time_s) * 40.f + float(e.id)) * 1.2f * cam.zoom;   // fever / hunger shiver
            SDL_FRect dst{fx - sw / 2 + ox + sox, fy - sh + (lying ? sh * 0.22f : 0.f) - bounce, sw, sh};
            // ground shadow, slanted by the sun
            const float sun = float((snap.day_phase - 12)) / 12.f;
            SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 70);
            SDL_FRect sh_r{fx - sw * 0.35f + sun * sw * 0.4f, fy - 2 * cam.zoom, sw * 0.7f, 3 * cam.zoom};
            SDL_RenderFillRect(ren, &sh_r);
            bool flip = facing.count(e.id) && facing[e.id] < 0;
            if (an && std::fabs(std::cos(an->ang)) > 0.25f) flip = std::cos(an->ang) < 0.f;
            SDL_Texture* t = tex.get(*sp, flip);                   // paper flip, pre-mirrored
            if (e.fog == Fog::Shrouded) SDL_SetTextureColorMod(t, 120, 120, 135);
            else if (!e.alive) SDL_SetTextureColorMod(t, 150, 140, 130);
            else if (starving) SDL_SetTextureColorMod(t, 185, 192, 208);   // pale & shaky
            double tilt = !e.alive ? 90.0 : e.routed ? -12.0 : 0.0;
            if (an && e.alive && !e.routed)
                tilt = lying ? (flip ? -90.0 : 90.0) : lean;      // rest pose / motion lean
            if (mourn_now && !lying) tilt += 14.0;                 // grief hunch
            if (proud_now && !lying) tilt -= 3.0;                  // proud upright
            // post-coital wiggle: reproduce/mate verb fresh -> gentle sway
            if (an && e.alive && an->until > c_tick &&
                (an->verb == "reproduce" || an->verb == "mate") && !lying)
                tilt += std::sin(float(time_s) * 6.f + float(e.id % 11)) * 5.0;
            auto blit = [&](SDL_Texture* tx, const SDL_FRect& d, double ang) {
                if (ang == 0.0) SDL_RenderTexture(ren, tx, nullptr, &d);      // fast path
                else SDL_RenderTextureRotated(ren, tx, nullptr, &d, ang, nullptr, SDL_FLIP_NONE);
            };
            blit(t, dst, tilt);
            SDL_SetTextureColorMod(t, 255, 255, 255);
            // equipment overlays from REAL props
            if (e.armor > 0) blit(tex.get(atlas.armor_overlay(e.armor, e.armor_mat), flip), dst, tilt);
            if (e.w_sharp || e.w_blunt || e.w_elastic) {
                SDL_Texture* wt = tex.get(atlas.weapon(e.w_sharp, e.w_blunt, e.w_elastic, e.w_coated, e.w_mat), flip);
                SDL_FRect wd{fx + (flip ? -sw * 0.6f : sw * 0.05f), fy - sh * 0.75f, sw * 0.6f, sh * 0.6f};
                blit(wt, wd, 0.0);
            }
            if (an && show_badges)
                draw_actor_anim(e, *an, fx, fy - bounce, sw, sh, gliding, lying);
            if (e.leader) {                                    // crown of authority
                SDL_SetRenderDrawColor(ren, 255, 210, 60, 255);
                for (int k = 0; k < 3; ++k) {
                    SDL_FRect cr{fx - 4 * cam.zoom + k * 3 * cam.zoom, fy - sh - 3 * cam.zoom, 2 * cam.zoom, 3 * cam.zoom};
                    SDL_RenderFillRect(ren, &cr);
                }
            }
            if (e.bleed > 0 && e.alive && (frames / 8) % 3 == 0)
                emit(fx, fy - sh * 0.5f, 0xFFA01010, 1);
            // gait dust: soft puffs kicked up at the feet while gliding
            if (creature && e.alive && moving && e.fog == Fog::Visible && parts.size() < 600) {
                auto dit = dust_at.find(e.id);
                if (dit == dust_at.end() || time_s - dit->second > 0.14) {
                    const float j = float((frames * 31 + int(e.id % 13)) % 17) / 17.f - 0.5f;
                    const float fv = facing.count(e.id) ? facing[e.id] : 1.f;
                    parts.push_back({fx + j * sw * 0.4f, fy - cam.zoom, -fv * 16.f + j * 20.f, -9.f, 0.5f, 0xD8C8B088});
                    dust_at[e.id] = float(time_s);
                }
            }
            const Vec2 vpos{std::int32_t(vit->second.x), std::int32_t(vit->second.y)};
            if (e.temperature >= 3000) {                        // fire / forge
                draw_glow(vpos, 0xFF8030, 1.2f);
                if (e.fog == Fog::Visible && frames % 6 == std::uint64_t(e.id % 6) && parts.size() < 600) {
                    const float j = float((frames + int(e.id % 7)) % 11) / 11.f;   // smoke + embers
                    parts.push_back({fx + (j - 0.5f) * 6.f, fy - sh * 0.9f, (j - 0.5f) * 10.f, -20.f - j * 14.f, 1.3f, 0xFFB9BDC6, -14.f});
                    if ((frames / 6) % 3 == 0)
                        parts.push_back({fx + (j - 0.5f) * 4.f, fy - sh * 0.7f, (j - 0.5f) * 16.f, -34.f - j * 20.f, 0.7f, 0xFFFFA040, 30.f});
                }
            }
            if (e.epic && lens == kSacred) draw_glow(vpos, 0xFFE090, 0.8f);
            if (e.kind == "artifact" && e.alive) {              // Relic glints
                draw_glow(vpos, 0xA0E8FF, 0.9f);
                if (e.fog == Fog::Visible &&
                    frames % 45 == std::uint64_t(e.id % 45) && parts.size() < 600) {
                    const float jx = float((frames / 7 + int(e.id % 5)) % 9 - 4) * 3.f;
                    const float jy = float((frames / 11 + int(e.id % 3)) % 5) * 0.18f;
                    parts.push_back({fx + jx, fy - sh * (0.45f + jy),
                        0.f, -15.f, 0.6f, 0xFFC8F0FF});
                }
            }
            draw_entity_lens(e, fx, fy, sw, sh);
            // thought/action badge: DF-style pill floating over the head,
            // fades out over its last ~8 world ticks (viewer-only state)
            if (creature && e.alive && e.fog == Fog::Visible && show_badges) {
                auto bit = c_badge.find(e.id);
                if (bit != c_badge.end() && bit->second.until > c_tick) {
                    const ThoughtBadge& b = bit->second;
                    const float rem = float(bit->second.until - c_tick);
                    const int alpha = int(235.f * std::min(1.f, rem / 8.f));
                    ImDrawList* dl = ImGui::GetForegroundDrawList();
                    const ImVec2 tp(ux(fx), ux(fy - sh) - 10.f);
                    const ImVec2 ts = ImGui::CalcTextSize(b.label.c_str());
                    const ImU32 bg = IM_COL32(10, 10, 14, alpha * 3 / 4);
                    const ImU32 fg = (b.col & 0x00FFFFFFu) | (ImU32(alpha) << 24);
                    dl->AddRectFilled(ImVec2(tp.x - ts.x * 0.5f - 4.f, tp.y - 2.f),
                                      ImVec2(tp.x + ts.x * 0.5f + 4.f, tp.y + ts.y + 2.f),
                                      bg, 3.f);
                    dl->AddRect(ImVec2(tp.x - ts.x * 0.5f - 4.f, tp.y - 2.f),
                                ImVec2(tp.x + ts.x * 0.5f + 4.f, tp.y + ts.y + 2.f),
                                (b.col & 0x00FFFFFFu) | (ImU32(alpha * 2 / 3) << 24), 3.f);
                    dl->AddText(ImVec2(tp.x - ts.x * 0.5f, tp.y), fg, b.label.c_str());
                }
            }
            if (e.id == selected) {
                SDL_SetRenderDrawColor(ren, 255, 230, 120, 255);
                SDL_RenderRect(ren, &dst);
            }
        }
        // particles
        for (Particle& p : parts) {
            p.life -= float(dt); p.x += p.vx * float(dt); p.y += p.vy * float(dt); p.vy += p.grav * float(dt);
            if (p.life <= 0) continue;
            SDL_SetRenderDrawColor(ren, (p.c >> 16) & 0xFF, (p.c >> 8) & 0xFF, p.c & 0xFF, Uint8(255 * std::min(1.f, p.life * 2)));
            SDL_FRect d{p.x, p.y, 2 * cam.zoom, 2 * cam.zoom};
            SDL_RenderFillRect(ren, &d);
        }
        parts.erase(std::remove_if(parts.begin(), parts.end(), [](const Particle& p) { return p.life <= 0; }), parts.end());
        // ---- ambient particle layers (viewer-only juice) ------------------
        // expanding rings: wading ripples + speech waves; rising hearts on
        // births; floating "z" on sleepers. All fade with their own TTL.
        {
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            for (Ring& r : rings) {
                r.life -= float(dt); r.r += r.vr * float(dt);
                if (r.life <= 0.f) continue;
                const int a = int(210.f * (r.life / r.ttl));
                dl->AddCircle(ImVec2(ux(r.x), ux(r.y)), r.r * float(kPix),
                              (r.c & 0x00FFFFFFu) | (ImU32(a) << 24), 1.5f, 24);
            }
            rings.erase(std::remove_if(rings.begin(), rings.end(), [](const Ring& r) { return r.life <= 0.f; }), rings.end());
            auto heart = [&](ImVec2 c, float s, ImU32 col) {
                dl->AddCircleFilled(ImVec2(c.x - s * 0.34f, c.y - s * 0.16f), s * 0.36f, col, 10);
                dl->AddCircleFilled(ImVec2(c.x + s * 0.34f, c.y - s * 0.16f), s * 0.36f, col, 10);
                dl->AddTriangleFilled(ImVec2(c.x - s * 0.66f, c.y - s * 0.04f),
                                      ImVec2(c.x + s * 0.66f, c.y - s * 0.04f),
                                      ImVec2(c.x, c.y + s * 0.60f), col);
            };
            for (Heart& h : hearts) {
                h.t += float(dt);
                if (h.t > 1.4f) continue;
                const float a = 1.f - h.t / 1.4f;
                const float wob = std::sin(h.t * 9.f) * 1.5f;
                heart(ImVec2(ux(h.x + wob), ux(h.y - h.t * 10.f)), 4.2f,
                      IM_COL32(255, 110, 150, int(235 * a)));
            }
            hearts.erase(std::remove_if(hearts.begin(), hearts.end(), [](const Heart& h) { return h.t > 1.4f; }), hearts.end());
            for (Zed& z : zeds) {
                z.t += float(dt);
                if (z.t > 1.7f) continue;
                const float a = z.t < 0.3f ? z.t / 0.3f : 1.f - (z.t - 0.3f) / 1.4f;
                const float wob = std::sin(z.t * 3.f + z.x) * 1.2f;
                dl->AddText(ImVec2(ux(z.x + wob), ux(z.y - z.t * 8.f)),
                            IM_COL32(170, 200, 235, int(220 * a)), "z");
            }
            zeds.erase(std::remove_if(zeds.begin(), zeds.end(), [](const Zed& z) { return z.t > 1.7f; }), zeds.end());
            // ---- emergent gathering auras: soft pulsing gold ellipse under
            // clustered attendees of the same focal entity
            for (const Gather& g : c_gather) {
                const float gx = ux(cam.sx(float(g.pos.x)));
                const float gy = ux(cam.sy(float(g.pos.y), elev_at(g.pos)));
                const float pulse = 0.7f + 0.3f * std::sin(float(time_s) * 2.2f);
                const float rx = 46.f * cam.zoom * pulse, ry = 18.f * cam.zoom * pulse;
                dl->AddEllipseFilled(ImVec2(gx, gy), ImVec2(rx, ry),
                                     IM_COL32(255, 220, 130, 26), 0.f, 24);
                dl->AddEllipse(ImVec2(gx, gy), ImVec2(rx, ry),
                               IM_COL32(255, 225, 150, 70), 0.f, 1.2f, 24);
            }
            // ---- miracle beams: golden column fading over ~1.4 s
            for (Beam& b : beams) {
                const float age = float(time_s) - b.t0;
                if (age > 1.4f) continue;
                const float a = 1.f - age / 1.4f;
                const float bx = ux(b.x), by = ux(b.y);
                const float bh = 90.f * cam.zoom;
                dl->AddRectFilledMultiColor(ImVec2(bx - 7.f * cam.zoom, by - bh),
                                            ImVec2(bx + 7.f * cam.zoom, by),
                                            IM_COL32(255, 230, 140, int(90 * a)),
                                            IM_COL32(255, 230, 140, int(90 * a)),
                                            IM_COL32(255, 200, 80, int(10 * a)),
                                            IM_COL32(255, 200, 80, int(10 * a)));
                dl->AddCircleFilled(ImVec2(bx, by - 4.f * cam.zoom),
                                    10.f * cam.zoom * (0.6f + 0.4f * a),
                                    IM_COL32(255, 235, 160, int(200 * a)), 16);
            }
            beams.erase(std::remove_if(beams.begin(), beams.end(),
                [&](const Beam& b) { return float(time_s) - b.t0 > 1.4f; }), beams.end());
            // ---- comic glyph pops (! ? ~ #) for arguments
            for (Sym& s : syms) {
                s.t += float(dt);
                if (s.t > 0.9f) continue;
                const float a = 1.f - s.t / 0.9f;
                const float wob = std::sin(s.t * 11.f) * 2.f;
                dl->AddText(ImVec2(ux(s.x + wob), ux(s.y - s.t * 16.f)),
                            IM_COL32(255, 90, 70, int(240 * a)),
                            std::string(1, s.c).c_str());
            }
            syms.erase(std::remove_if(syms.begin(), syms.end(),
                [](const Sym& s) { return s.t > 0.9f; }), syms.end());
            // ---- tempest lightning flash: brief cold-white blink --------
            {
                const double fage = time_s - storm_t0;
                if (fage >= 0.0 && fage < 0.45) {
                    const float fa = float(1.0 - fage / 0.45) * 0.55f;
                    dl->AddRectFilled(ImVec2(0, 0),
                        ImVec2(float(win_w), float(win_h)),
                        IM_COL32(220, 235, 255, int(255 * fa)));
                }
            }
        }
        // prune aux animation maps: entries for entities that died or left
        // the snapshot would otherwise grow forever.
        if (frames % 120 == 0 && !snap.entities.empty()) {
            std::unordered_set<EntityId> live;
            live.reserve(snap.entities.size());
            for (const EntityView& e : snap.entities) live.insert(e.id);
            auto prune = [&](auto& m) {
                for (auto it = m.begin(); it != m.end();)
                    it = live.count(it->first) ? std::next(it) : m.erase(it);
            };
            prune(last_pos); prune(facing); prune(bob_until); prune(last_integ); prune(last_pain);
            prune(vis_pos); prune(vis_prev); prune(dust_at);
            prune(c_badge); prune(c_intent); prune(c_anim);
        }
    }



    void Viewer::draw_entity_lens(const EntityView& e, float fx, float fy, float sw, float sh) {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        fx = ux(fx); fy = ux(fy); sw = ux(sw); sh = ux(sh);
        if (lens == kEpidemic && (e.infected || e.toxin > 0)) {
            const float r = (e.infected ? 18.f : 10.f) * cam.zoom * kPix;
            dl->AddCircleFilled(ImVec2(fx, fy - sh / 2), r, IM_COL32(120, 230, 60, 60));
        }
        if (lens == kEconomy && e.wealth > 0)
            dl->AddText(ImVec2(fx - 8, fy - sh - 12), IM_COL32(240, 200, 80, 255), std::to_string(e.wealth).c_str());
        if (lens == kPolitics && e.grievance > 0)
            dl->AddCircleFilled(ImVec2(fx, fy - sh / 2), (6 + e.grievance / 60) * cam.zoom * kPix,
                                IM_COL32(220, 40, 30, int(40 + e.grievance / 8)));
        if (lens == kSacred && e.sacred_conv > 0)
            dl->AddCircle(ImVec2(fx, fy - sh / 2), 10 * cam.zoom * kPix, IM_COL32(255, 220, 120, int(e.sacred_conv / 4)));
        if (lens == kMilitary) {
            if (e.morale >= 0) {
                const float wbar = sw * float(e.morale) / 1000.f;
                dl->AddRectFilled(ImVec2(fx - sw / 2, fy - sh - 6), ImVec2(fx - sw / 2 + wbar, fy - sh - 3),
                                  e.routed ? IM_COL32(200, 40, 40, 255) : IM_COL32(80, 200, 80, 255));
            }
            if (e.launch_range > 0)
                dl->AddCircle(ImVec2(fx, fy), e.launch_range * cam.zoom * kPix, IM_COL32(255, 120, 60, 110), 48);
        }
        if (lens == kEcology && e.fauna) {
            // apex predators (tier >= 4) get a red ring; migrants a chevron
            if (e.trophic_tier >= 4)
                dl->AddCircle(ImVec2(fx, fy - sh / 2), 9 * cam.zoom * kPix,
                              IM_COL32(220, 50, 40, 200), 20, 1.5f);
            if (e.migration_drive > 0) {
                const float ay = fy - sh - 14.f * cam.zoom;
                dl->AddTriangleFilled(ImVec2(fx, ay - 4.f * cam.zoom),
                                      ImVec2(fx - 4.f * cam.zoom, ay + 3.f * cam.zoom),
                                      ImVec2(fx + 4.f * cam.zoom, ay + 3.f * cam.zoom),
                                      IM_COL32(240, 210, 90, 230));
            }
        }
    }



    void Viewer::draw_links() {
        if (lens != kPolitics) return;
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        for (const LinkView& l : snap.links)
            dl->AddLine(ImVec2(ux(cam.sx(float(l.a.x))), ux(cam.sy(float(l.a.y), elev_at(l.a)) - 12 * cam.zoom)),
                        ImVec2(ux(cam.sx(float(l.b.x))), ux(cam.sy(float(l.b.y), elev_at(l.b)) - 20 * cam.zoom)),
                        IM_COL32(230, 190, 60, 150), 1.5f);
    }



    // DF-style intent lines: a dashed line from the selected (or hovered)
    // NPC to the entity its mind/brain is currently reaching for.
    void Viewer::dashed_seg(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col,
                           float dash , float gap , float thick ) {
        const float dx = b.x - a.x, dy = b.y - a.y;
        const float len = std::hypot(dx, dy);
        if (len < 1.f) return;
        const float ux_ = dx / len, uy_ = dy / len;
        for (float t = 0.f; t < len; t += dash + gap) {
            const float e = std::min(t + dash, len);
            dl->AddLine(ImVec2(a.x + ux_ * t, a.y + uy_ * t),
                        ImVec2(a.x + ux_ * e, a.y + uy_ * e), col, thick);
        }
    }


    void Viewer::draw_intents() {
        if (!show_badges || globe) return;
        // chosen actor: explicit selection wins, else hovered entity
        auto fresh = [&](EntityId id) {
            auto it = c_intent.find(id);
            return it != c_intent.end() && it->second.until > c_tick;
        };
        EntityId who = kNoEntity;
        if (selected != kNoEntity && fresh(selected)) who = selected;
        else if (hover_id != kNoEntity && fresh(hover_id)) who = hover_id;
        if (who == kNoEntity) return;
        auto iit = c_intent.find(who);
        if (iit == c_intent.end() || iit->second.until <= c_tick) return;
        const EntityView* src = nullptr;
        const EntityView* dst = nullptr;
        for (const EntityView& e : snap.entities) {
            if (e.id == who) src = &e;
            if (e.id == iit->second.target) dst = &e;
            if (src && dst) break;
        }
        if (!src || !src->alive || src->fog != Fog::Visible) return;
        const float ax = ux(cam.sx(float(src->pos.x)));
        const float ay = ux(cam.sy(float(src->pos.y), elev_at(src->pos)));
        float bx, by;
        if (dst && dst->fog == Fog::Visible) {
            bx = ux(cam.sx(float(dst->pos.x)));
            by = ux(cam.sy(float(dst->pos.y), elev_at(dst->pos)));
        } else {
            bx = ux(cam.sx(float(iit->second.pos.x)));
            by = ux(cam.sy(float(iit->second.pos.y)));
        }
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const ImU32 col = who == selected ? IM_COL32(255, 225, 120, 200) : IM_COL32(150, 210, 255, 170);
        dashed_seg(dl, ImVec2(ax, ay - 14.f), ImVec2(bx, by - 10.f), col);
        if (!iit->second.label.empty()) {
            const ImVec2 tp((ax + bx) * 0.5f, (ay + by) * 0.5f - 8.f);
            const ImVec2 ts = ImGui::CalcTextSize(iit->second.label.c_str());
            dl->AddRectFilled(ImVec2(tp.x - ts.x * 0.5f - 3.f, tp.y - 1.f),
                              ImVec2(tp.x + ts.x * 0.5f + 3.f, tp.y + ts.y + 1.f),
                              IM_COL32(10, 10, 14, 170), 2.f);
            dl->AddText(ImVec2(tp.x - ts.x * 0.5f, tp.y), IM_COL32(235, 225, 180, 230),
                        iit->second.label.c_str());
        }
    }



    // Phase 15: narrative callouts — short story lines floating over the spot
    // where they happened, rising and fading over ~4 s of render time.
    void Viewer::draw_callouts() {
        if (c_callouts.empty()) return;
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        for (auto it = c_callouts.begin(); it != c_callouts.end();) {
            if (it->until <= frames) { it = c_callouts.erase(it); continue; }
            const float life = float(it->until - frames) / 260.f;   // 1 -> 0
            const float rise = (1.f - life) * 16.f * cam.zoom;
            const float px = ux(cam.sx(float(it->pos.x)));
            const float py = ux(cam.sy(float(it->pos.y))) - 30.f * cam.zoom - it->oy - rise;
            const ImVec2 ts = ImGui::CalcTextSize(it->text.c_str());
            const int alpha = int(225.f * std::min(1.f, life * 2.5f));
            dl->AddRectFilled(ImVec2(px - ts.x * 0.5f - 3.f, py - 2.f),
                              ImVec2(px + ts.x * 0.5f + 3.f, py + ts.y + 2.f),
                              IM_COL32(8, 8, 12, alpha * 3 / 4), 3.f);
            dl->AddText(ImVec2(px - ts.x * 0.5f, py),
                        (it->col & 0x00FFFFFFu) | (ImU32(alpha) << 24), it->text.c_str());
            ++it;
        }
    }



    // Worn footpaths: client-side traffic heat rendered as packed-dirt
    // cells. Heavily traveled routes between water, groves and focal
    // points visibly wear smooth; unused cells fade back out.
    void Viewer::draw_footpaths() {
        if (path_heat.empty()) return;
        const int cell = w.terrain.cell > 0 ? w.terrain.cell : 16;
        const ViewRect vr = cam.view();   // world-unit bounds (kY-aware)
        const std::int64_t x0 = vr.x0 / cell - 1, x1 = vr.x1 / cell + 1;
        const std::int64_t y0 = vr.y0 / cell - 1, y1 = vr.y1 / cell + 1;
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        for (const auto& [key, heat] : path_heat) {
            if (heat < 18) continue;
            const std::int64_t cx = std::int64_t(std::int32_t(key >> 32));
            const std::int64_t cy = std::int64_t(std::int32_t(key));
            if (cx < x0 || cx > x1 || cy < y0 || cy > y1) continue;
            const Vec2 cp{std::int32_t(cx * cell + cell / 2),
                          std::int32_t(cy * cell + cell / 2)};
            const float px = cam.sx(float(cp.x));
            const float py = cam.sy(float(cp.y), elev_at(cp));
            const float s = float(cell) * cam.zoom;
            const int a = std::min(105, int(heat / 9));
            SDL_SetRenderDrawColor(ren, 104, 80, 52, Uint8(a));
            SDL_FRect r{px - s * 0.5f, py - s * 0.42f, s, s * 0.84f};
            SDL_RenderFillRect(ren, &r);
        }
    }


    // Warm halos over emergent gatherings — draw_glow already scales its
    // alpha by the day/night clock, so circles flare at dusk and night.
    void Viewer::draw_gather_glow() {
        for (const Gather& g : c_gather)
            draw_glow(g.pos, 0xFFC860,
                      0.7f + 0.12f * float(g.members.size()));
    }


    void Viewer::draw_night() {
        const int h = snap.day_phase;
        int a = 0;
        if (h < 5 || h >= 21) a = 150; else if (h < 7) a = 150 - (h - 5) * 60; else if (h >= 19) a = (h - 18) * 60;
        if (a <= 0) return;
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren, 12, 16, 46, Uint8(std::min(170, a)));      // cold dark-fantasy tone
        SDL_FRect all{0, 0, float(cam.sw), float(cam.sh)};
        SDL_RenderFillRect(ren, &all);
    }



    // ---- overlays: speech bubbles with live translation, signs, drag ---------
    void Viewer::draw_overlays() {
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        bubble_rects.clear();
        const std::uint64_t now = c_tick;
        auto scr = [&](Vec2 p) { return ImVec2(cam.sx(float(p.x)) * kPix, cam.sy(float(p.y), elev_at(p)) * kPix); };
        // signs (beacons / building sites) the tribe currently holds
        for (const Sign& sg : c_signs) {
            const ImVec2 c = scr(sg.p);
            if (sg.site) {
                dl->AddCircle(c, float(sg.r) * cam.zoom * kPix, IM_COL32(250, 210, 120, 200), 48, 2.0f);
                dl->AddText(ImVec2(c.x - 30, c.y - 8), IM_COL32(250, 220, 150, 230), "blueprint");
            } else {
                dl->AddCircleFilled(c, 6, IM_COL32(120, 200, 255, 220));
                dl->AddLine(ImVec2(c.x, c.y), ImVec2(c.x, c.y - 26), IM_COL32(120, 200, 255, 220), 2);
            }
        }
        if (dragging && tool == kToolBlueprint) {
            const ImVec2 c = scr(drag_a);
            const float r = std::sqrt(float(dist2(drag_a, drag_b)));
            dl->AddCircle(c, std::clamp(r, 48.f, 220.f) * cam.zoom * kPix, IM_COL32(255, 230, 140, 255), 48, 2.5f);
        }
        // bubbles: newest per speaker, ~40 ticks of life
        std::set<EntityId> drawn;
        for (auto it = journal.bubbles.rbegin(); it != journal.bubbles.rend(); ++it) {
            if (it->tick + 40 < now || drawn.count(it->speaker)) continue;
            const auto pi = c_speaker_pos.find(it->speaker);
            if (pi == c_speaker_pos.end()) continue;
            drawn.insert(it->speaker);
            ImVec2 a = scr(pi->second);
            a.y -= 44 * cam.zoom * kPix * 0.5f + 18;
            float x = a.x - 10;
            const float y = a.y - 30;
            // measure — cap the overhead bubble at a glance: first 4 words
            // plus a "+N" chip; the full utterance lives in Journal/Chronicle.
            std::vector<std::pair<std::string, std::string>> all;
            all.reserve(it->words.size());
            for (const std::string& wd : it->words) {
                std::string tr = journal_reading(w, journal, it->lang, wd);
                if (avatar != kNoEntity) {
                    const auto gi = c_avatar_gloss.find(wd);
                    if (gi != c_avatar_gloss.end()) tr = gi->second + " (you know)";
                }
                all.push_back({wd, tr});
            }
            std::vector<std::pair<std::string, std::string>> parts;
            if (all.size() > 5) {
                parts.assign(all.begin(), all.begin() + 4);
                parts.push_back({"+" + std::to_string(all.size() - 4), "\xC2\xB7"});
            } else {
                parts = std::move(all);
            }
            float wsum = 8;
            for (const auto& [wd, tr] : parts) {
                const float ww = std::max(ImGui::CalcTextSize(wd.c_str()).x, ImGui::CalcTextSize(tr.c_str()).x) + 10;
                wsum += ww;
            }
            x = a.x - wsum * 0.5f;
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + wsum, y + 34), IM_COL32(238, 226, 196, 235), 6);
            dl->AddRect(ImVec2(x, y), ImVec2(x + wsum, y + 34), IM_COL32(90, 60, 30, 255), 6);
            dl->AddTriangleFilled(ImVec2(a.x - 5, y + 34), ImVec2(a.x + 5, y + 34), ImVec2(a.x, y + 42), IM_COL32(238, 226, 196, 235));
            float cx = x + 6;
            for (const auto& [wd, tr] : parts) {
                const float ww = std::max(ImGui::CalcTextSize(wd.c_str()).x, ImGui::CalcTextSize(tr.c_str()).x) + 10;
                dl->AddText(ImVec2(cx, y + 2), IM_COL32(60, 35, 15, 255), wd.c_str());
                if (!tr.empty())
                    dl->AddText(ImVec2(cx, y + 17), tr.find("[ok]") != std::string::npos || tr.find("(you know)") != std::string::npos
                                    ? IM_COL32(20, 110, 40, 255) : IM_COL32(150, 90, 20, 255), tr.c_str());
                else
                    dl->AddText(ImVec2(cx, y + 17), IM_COL32(140, 120, 100, 255), "?");
                bubble_rects.push_back({cx - 3, y, cx + ww - 3, y + 34, it->lang, wd});
                cx += ww;
            }
        }
    }



    void Viewer::update_minimap() {
        const TerrainGrid& t = w.terrain;
        if (t.empty()) return;
        if (!mini_tex) {
            mini_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, t.w, t.h);
            SDL_SetTextureScaleMode(mini_tex, SDL_SCALEMODE_NEAREST);
        }
        std::vector<std::uint32_t> px(std::size_t(t.w) * t.h);
        for (int y = 0; y < t.h; ++y) for (int x = 0; x < t.w; ++x) {
            const std::size_t i = std::size_t(y) * t.w + x;
            const Fog f = fog.cell_fog(x + t.ox, y + t.oy);
            std::uint32_t c = f == Fog::Unexplored ? rgba(20, 17, 14)
                            : t.water[i] ? rgba(50, 90, 150)
                            : rgba(90 + t.elevation[i] / 6, 110 + t.vegetation[i] / 10, 60);
            if (f == Fog::Shrouded) c = shade(c, 550);
            px[i] = c;
        }
        for (const auto& [mid, m] : w.minds_) {
            (void)m;
            const Entity* e = w.find(mid);
            if (!e || !e->alive || fog.point_fog(w, e->pos) != Fog::Visible) continue;
            const int x = TerrainGrid::cell_of(e->pos.x, t.cell) - t.ox, y = TerrainGrid::cell_of(e->pos.y, t.cell) - t.oy;
            if (x >= 0 && y >= 0 && x < t.w && y < t.h) px[std::size_t(y) * t.w + x] = rgba(255, 230, 140);
        }
        SDL_UpdateTexture(mini_tex, nullptr, px.data(), t.w * 4);
    }


    void Viewer::ui_minimap() {
        if (w.terrain.empty() || !mini_tex) return;
        const TerrainGrid& t = w.terrain;
        const float S = 180.f;
        ImGui::SetNextWindowPos(ImVec2(float(win_w) - 10, float(win_h) - 10), ImGuiCond_Always, ImVec2(1, 1));
        ImGui::Begin("##minimap", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        const ImVec2 o = ImGui::GetCursorScreenPos();
        ImGui::Image(ImTextureRef((ImTextureID)(intptr_t)mini_tex), ImVec2(S, S));
        const float k = S / float(std::max(t.w, t.h));
        const ViewRect v = cam.view();
        ImGui::GetWindowDrawList()->AddRect(ImVec2(o.x + v.x0 / float(t.cell) * k, o.y + v.y0 / float(t.cell) * k),
            ImVec2(o.x + v.x1 / float(t.cell) * k, o.y + v.y1 / float(t.cell) * k), IM_COL32(255, 255, 255, 200));
        if (ImGui::IsItemClicked()) {
            const ImVec2 m = ImGui::GetMousePos();
            cam.cx = (m.x - o.x) / k * t.cell; cam.cy = (m.y - o.y) / k * t.cell;
        }
        ImGui::End();
    }

}  // namespace simlab_viewer
