// Phase 13 — Procedural Art Engine: pixel art generated from data/*.txt.
// See include/simlab/render/Atlas.hpp. Pure CPU, deterministic, SDL-free.
#include "simlab/render/Atlas.hpp"
#include "simlab/World.hpp"
#include <algorithm>
#include <cstdlib>

namespace simlab::render {

// ---- colour & hashing ----------------------------------------------------------
std::uint32_t rgba(int r, int g, int b, int a) {
    auto c = [](int v) { return static_cast<std::uint32_t>(std::clamp(v, 0, 255)); };
    return (c(a) << 24) | (c(r) << 16) | (c(g) << 8) | c(b);
}
static int R(std::uint32_t c) { return (c >> 16) & 0xFF; }
static int G(std::uint32_t c) { return (c >> 8) & 0xFF; }
static int B(std::uint32_t c) { return c & 0xFF; }
static int A(std::uint32_t c) { return (c >> 24) & 0xFF; }
std::uint32_t shade(std::uint32_t c, int pm) {
    return rgba(R(c) * pm / 1000, G(c) * pm / 1000, B(c) * pm / 1000, A(c));
}
std::uint32_t mix(std::uint32_t a, std::uint32_t b, int t) {
    t = std::clamp(t, 0, 1000);
    return rgba((R(a) * (1000 - t) + R(b) * t) / 1000, (G(a) * (1000 - t) + G(b) * t) / 1000,
                (B(a) * (1000 - t) + B(b) * t) / 1000, (A(a) * (1000 - t) + A(b) * t) / 1000);
}
std::uint64_t fnv(const std::string& s) {
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char ch : s) { h ^= ch; h *= 1099511628211ull; }
    return h;
}
// HSV-ish (h 0..359, s/v permille) -> rgb
static std::uint32_t hsv(int h, int s, int v, int a = 255) {
    h = ((h % 360) + 360) % 360;
    const int c = v * s / 1000;
    const int x = c * (1000 - std::abs((h * 1000 / 60) % 2000 - 1000)) / 1000;
    const int m = v - c;
    int r = 0, g = 0, b = 0;
    if (h < 60) { r = c; g = x; } else if (h < 120) { r = x; g = c; }
    else if (h < 180) { g = c; b = x; } else if (h < 240) { g = x; b = c; }
    else if (h < 300) { r = x; b = c; } else { r = c; b = x; }
    return rgba((r + m) * 255 / 1000, (g + m) * 255 / 1000, (b + m) * 255 / 1000, a);
}
// deterministic per-pixel noise 0..999
static int noise(std::uint64_t seed, int x, int y) {
    std::uint64_t h = seed ^ (static_cast<std::uint64_t>(x) * 0x9E3779B97F4A7C15ull) ^
                      (static_cast<std::uint64_t>(y) * 0xC2B2AE3D27D4EB4Full);
    h ^= h >> 31; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 29;
    return static_cast<int>(h % 1000);
}

// ---- Sprite ----------------------------------------------------------------------
void Sprite::put(int x, int y, std::uint32_t c) {
    if (x < 0 || y < 0 || x >= w || y >= h || A(c) == 0) return;
    std::uint32_t& d = px[static_cast<std::size_t>(y) * w + x];
    if (A(c) == 255 || A(d) == 0) { d = c; return; }
    d = mix(d, c | 0xFF000000u, A(c) * 1000 / 255) | 0xFF000000u;
}
void Sprite::fill_rect(int x, int y, int ww, int hh, std::uint32_t c) {
    for (int j = y; j < y + hh; ++j) for (int i = x; i < x + ww; ++i) put(i, j, c);
}
std::size_t Sprite::opaque_count() const {
    std::size_t n = 0;
    for (std::uint32_t p : px) if (A(p)) ++n;
    return n;
}
std::uint64_t Sprite::digest() const {
    std::uint64_t h = 1469598103934665603ull;
    for (std::uint32_t p : px) { h ^= p; h *= 1099511628211ull; }
    return h;
}
static Sprite blank(int w, int h) { Sprite s; s.w = w; s.h = h; s.px.assign(static_cast<std::size_t>(w) * h, 0); return s; }

// ---- drawing primitives ---------------------------------------------------------------
static void line(Sprite& s, int x0, int y0, int x1, int y1, std::uint32_t c) {
    int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (int guard = 0; guard < 512; ++guard) {
        s.put(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
static void disc(Sprite& s, int cx, int cy, int r, std::uint32_t c) {
    for (int y = -r; y <= r; ++y) for (int x = -r; x <= r; ++x)
        if (x * x + y * y <= r * r + r) s.put(cx + x, cy + y, c);
}
static void ellipse(Sprite& s, int cx, int cy, int rx, int ry, std::uint32_t c) {
    if (rx <= 0 || ry <= 0) return;
    for (int y = -ry; y <= ry; ++y) for (int x = -rx; x <= rx; ++x)
        if (x * x * ry * ry + y * y * rx * rx <= rx * rx * ry * ry) s.put(cx + x, cy + y, c);
}
// 1px dark rim around opaque pixels (paper-cut outline)
static void outline(Sprite& s, std::uint32_t c) {
    Sprite o = s;
    for (int y = 0; y < s.h; ++y) for (int x = 0; x < s.w; ++x) {
        if (A(s.at(x, y))) continue;
        bool edge = false;
        for (int k = 0; k < 4 && !edge; ++k) {
            const int nx = x + (k == 0) - (k == 1), ny = y + (k == 2) - (k == 3);
            if (nx >= 0 && ny >= 0 && nx < s.w && ny < s.h && A(s.at(nx, ny))) edge = true;
        }
        if (edge) o.px[static_cast<std::size_t>(y) * s.w + x] = c;
    }
    s = o;
}

const char* mat_class_name(MatClass c) {
    switch (c) {
        case MatClass::Rock: return "rock";       case MatClass::Metal: return "metal";
        case MatClass::Wood: return "wood";       case MatClass::Plant: return "plant";
        case MatClass::Food: return "food";       case MatClass::Soil: return "soil";
        case MatClass::Liquid: return "liquid";   case MatClass::Flesh: return "flesh";
        case MatClass::Mineral: return "mineral"; case MatClass::Energetic: return "energetic";
        case MatClass::Ash: return "ash";
    }
    return "?";
}

// ---- classification: properties -> art class & palette ------------------------------
static std::int64_t mprop(const World& w, const MaterialDef& m, const char* name) {
    const PropertyDef* d = w.props.find_name(name);
    if (!d) return 0;
    const auto it = m.props.find(d->id);
    return it == m.props.end() ? 0 : it->second.i;
}

MaterialLook ProceduralAtlas::classify(const World& w, MaterialId id) {
    MaterialLook L;
    const MaterialDef* m = w.materials.find(id);
    if (!m) return L;
    const std::int64_t hard = mprop(w, *m, "hardness"), dens = mprop(w, *m, "density") / 1000,
        flam = mprop(w, *m, "flammability"), cond = mprop(w, *m, "conductivity"),
        tox = mprop(w, *m, "toxicity"), nut = mprop(w, *m, "nutrition"),
        tens = mprop(w, *m, "tensile"), soil = mprop(w, *m, "soil_texture"),
        vol = mprop(w, *m, "volatility"), hyd = mprop(w, *m, "hydration"),
        lith = mprop(w, *m, "lithology");
    const int vary = static_cast<int>(fnv(m->name) % 40) - 20;     // small per-name hue shift
    if (vol >= 300 && flam >= 600) L.cls = MatClass::Energetic;
    else if (cond >= 250 && dens >= 3500 && flam == 0 && hard >= 400) L.cls = MatClass::Metal;
    else if (hard <= 10 && flam == 0 && dens <= 1100) L.cls = MatClass::Liquid;
    else if (soil > 0) L.cls = MatClass::Soil;
    else if (nut >= 400 && flam <= 300) L.cls = MatClass::Flesh;
    else if (nut > 0 && flam >= 300) L.cls = MatClass::Food;
    else if (flam >= 500 && tens >= 300 && nut == 0) L.cls = MatClass::Wood;
    else if (flam >= 400 && hard <= 300) L.cls = MatClass::Plant;
    else if (hard <= 120 && flam == 0) L.cls = MatClass::Ash;
    else if (hard >= 600 || lith > 0) L.cls = MatClass::Rock;
    else L.cls = MatClass::Mineral;
    // palette: hue by class, value by density/hardness, saturation by organic-ness
    int h = 30, s = 300, v = 600;
    switch (L.cls) {
        case MatClass::Rock:  h = 30 + vary; s = 80 + static_cast<int>(lith) * 40; v = 350 + static_cast<int>(1000 - hard) / 3; break;
        case MatClass::Metal: h = cond >= 600 ? 210 : 25 + vary / 2; s = cond >= 600 ? 120 : 550; v = 700; break;
        case MatClass::Wood:  h = 28 + vary / 2; s = 600; v = 350 + static_cast<int>(dens) / 5; break;
        case MatClass::Plant: h = 95 + vary; s = 600; v = 550; break;
        case MatClass::Food:  h = 345 + vary; s = 700; v = 600; break;
        case MatClass::Soil:  h = soil == 1 ? 42 : soil == 3 ? 15 : 28; s = 500; v = soil == 1 ? 720 : 420; break;
        case MatClass::Liquid:h = nut > 300 ? 355 : 205; s = nut > 300 ? 750 : 600; v = nut > 300 ? 450 : 700; break;
        case MatClass::Flesh: h = 355 + vary / 2; s = 550; v = 650; break;
        case MatClass::Mineral: h = 50 + vary * 3; s = 250 + static_cast<int>(dens) / 20; v = 750; break;
        case MatClass::Energetic: h = 40; s = 200; v = 250; break;
        case MatClass::Ash:   h = 30; s = 40; v = 420; break;
    }
    if (tox > 0) { L.toxic = true; h = 285; s = 650; }               // sickly violet
    L.base = hsv(h, s, v);
    L.dark = hsv(h, std::min(1000, s + 100), v * 55 / 100);
    L.light = hsv(h, s * 6 / 10, std::min(1000, v * 135 / 100));
    L.accent = L.cls == MatClass::Food ? hsv(h + 20, 800, 800)
             : L.toxic ? hsv(120, 700, 800) : hsv(h + 180, 300, 800);
    L.sheen = L.cls == MatClass::Metal;
    return L;
}

// material tile: the surface PATTERN follows the physics class
static Sprite material_tile(const MaterialLook& L, std::uint64_t seed, std::int64_t brit) {
    const int T = ProceduralAtlas::kTile;
    Sprite s = blank(T, T);
    for (int y = 0; y < T; ++y) for (int x = 0; x < T; ++x) {
        const int n = noise(seed, x, y);
        std::uint32_t c = L.base;
        switch (L.cls) {
            case MatClass::Wood:   // vertical grain + rings
                c = mix(L.base, L.dark, ((x * 7 + noise(seed, x / 3, y / 9) / 120) % 6 == 0) ? 700 : n / 6);
                break;
            case MatClass::Metal: { // brushed + specular band
                const int band = std::abs((x + y) - T) < 4 ? 700 : 0;
                c = mix(mix(L.base, L.dark, n / 8), 0xFFFFFFFFu, band + (y % 3 == 0 ? 60 : 0));
                break;
            }
            case MatClass::Liquid: // ripples
                c = mix(L.base, L.light, ((y + (x / 5) % 3) % 7 == 0) ? 600 : n / 10);
                break;
            case MatClass::Soil: case MatClass::Ash: // speckle
                c = n < 120 ? L.dark : n > 900 ? L.light : L.base;
                break;
            case MatClass::Plant: case MatClass::Food:
                c = n < 250 ? L.dark : n > 850 ? L.accent : L.base;
                break;
            case MatClass::Energetic: // granular dark powder with bright grains
                c = n > 940 ? rgba(255, 220, 90) : n < 400 ? L.dark : L.base;
                break;
            default: // rock / mineral / flesh: mottled
                c = mix(L.base, n < 500 ? L.dark : L.light, std::abs(n - 500) / 2);
        }
        s.put(x, y, c);
    }
    // brittle hard rock: sharp crack lines (count by brittleness)
    if (L.cls == MatClass::Rock || L.cls == MatClass::Mineral) {
        const int cracks = 1 + static_cast<int>(brit / 250);
        for (int k = 0; k < cracks; ++k) {
            int x = noise(seed, k, 99) % T, y = noise(seed, 99, k) % T;
            for (int step = 0; step < 10; ++step) {
                const int nx = x + (noise(seed, k, step) % 7) - 3, ny = y + 3;
                line(s, x, y, nx, ny, L.dark);
                x = nx; y = ny;
            }
        }
    }
    if (L.toxic)   // toxic motes
        for (int k = 0; k < 6; ++k) s.put(noise(seed, k, 7) % T, noise(seed, 7, k) % T, L.accent);
    return s;
}

// material icon: a lump / ingot / log / berry pile shaped by class
static Sprite make_material_icon(const MaterialLook& L, std::uint64_t seed) {
    const int I = ProceduralAtlas::kIcon;
    Sprite s = blank(I, I);
    switch (L.cls) {
        case MatClass::Metal:
            for (int y = 9; y < 17; ++y) line(s, 4 + (16 - y) / 2, y, 19 - (16 - y) / 2, y, y < 11 ? L.light : L.base);
            line(s, 7, 10, 15, 10, 0xFFFFFFFFu);
            break;
        case MatClass::Wood:
            ellipse(s, 12, 13, 9, 4, L.base);
            for (int x = 4; x < 21; x += 3) s.put(x, 13, L.dark);
            ellipse(s, 20, 13, 2, 4, L.light);
            break;
        case MatClass::Food: case MatClass::Flesh:
            for (int k = 0; k < 6; ++k)
                disc(s, 7 + (k % 3) * 5, 10 + (k / 3) * 5 + (k % 2), 3, k % 2 ? L.base : L.accent);
            break;
        case MatClass::Liquid:
            ellipse(s, 12, 15, 8, 4, L.base); ellipse(s, 10, 14, 3, 1, L.light);
            break;
        case MatClass::Energetic:
            ellipse(s, 12, 14, 7, 6, L.dark); line(s, 12, 8, 15, 3, rgba(200, 180, 120));
            s.put(15, 2, rgba(255, 200, 60));
            break;
        default:
            ellipse(s, 12, 14, 8, 6, L.base);
            ellipse(s, 10, 12, 4, 2, L.light);
            for (int k = 0; k < 5; ++k) s.put(6 + noise(seed, k, 3) % 12, 11 + noise(seed, 3, k) % 6, L.dark);
    }
    if (L.toxic) for (int k = 0; k < 3; ++k) disc(s, 6 + k * 6, 4, 1, L.accent);
    outline(s, rgba(20, 16, 12));
    return s;
}

// ---- species body plans ---------------------------------------------------------------
BodyPlan ProceduralAtlas::body_plan(const World& w, const std::string& name) {
    BodyPlan p;
    const SpeciesDef* sp = w.species.find_name(name);
    if (!sp) return p;
    p.flora = !sp->fauna;
    const PropertyDef* fm = w.props.find_name("fruit_material");
    p.fruiting = fm && sp->props.get(fm->id) != nullptr;
    if (p.flora) {
        const MaterialLook L = classify(w, sp->material);
        p.tree = !p.fruiting && L.cls == MatClass::Wood;
        p.height = p.tree ? 30 : 18;
        p.width = p.tree ? 18 : 22;
        return p;
    }
    std::int64_t total = 0;
    for (const PartSpec& ps : sp->anatomy) {
        const std::string& r = ps.role;
        if (r == "leg") ++p.legs; else if (r == "arm") ++p.arms;
        else if (r == "head") ++p.heads; else if (r == "eye") ++p.eyes;
        else if (r == "tail") ++p.tails; else if (r == "wing") ++p.wings;
        else if (r == "horn") ++p.horns;
        total += ps.size;
    }
    // no anatomy declared: infer a minimal creature from mass
    const PropertyDef* md = w.props.find_name("mass");
    const std::int64_t mass = md && sp->props.get(md->id) ? sp->props.get(md->id)->i : 0;
    if (sp->anatomy.empty()) {
        // no body_part lines: infer from what the species can DO — a
        // manipulator (reach) or a voice (vocal_capability) implies hands on
        // an upright body; otherwise a grounded quadruped; tiny = hexapod.
        auto has = [&](const char* n) {
            const PropertyDef* d = w.props.find_name(n);
            const Value* v = d ? sp->props.get(d->id) : nullptr;
            return v && v->i > 0;
        };
        if (has("reach") || has("vocal_capability")) { p.legs = 2; p.arms = 2; }
        else p.legs = mass > 0 && mass < 100 ? 6 : 4;
        p.heads = 1; p.eyes = 2;
        total = p.arms == 2 ? 1500 : 600;
    }
    if (p.heads == 0 && p.eyes > 0) p.heads = 1;
    // upright if it has arms and two legs; size from total part size
    p.height = std::clamp<int>(14 + static_cast<int>(total / 150), 12, 31);
    if (mass > 0 && mass < 100) p.height = 10;                 // tiny flyers / midges
    p.width = p.arms >= 2 && p.legs == 2 ? 12 : 22;
    return p;
}

static Sprite flora_sprite(const BodyPlan& p, const MaterialLook& wood, const MaterialLook& leaf,
                           std::uint64_t seed) {
    const int S = ProceduralAtlas::kStandee;
    Sprite s = blank(S, S);
    if (p.tree) {
        s.fill_rect(14, 18, 4, 13, wood.base);
        line(s, 14, 18, 14, 30, wood.dark);
        const std::uint32_t canopy = mix(leaf.base, rgba(40, 110, 40), 500);
        disc(s, 16, 11, 9, shade(canopy, 800));
        disc(s, 12, 13, 6, canopy); disc(s, 20, 12, 6, canopy);
        for (int k = 0; k < 14; ++k) s.put(8 + noise(seed, k, 1) % 16, 4 + noise(seed, 1, k) % 14, shade(canopy, 1300));
    } else {
        const std::uint32_t bush = mix(leaf.base, rgba(50, 120, 45), 600);
        ellipse(s, 16, 22, 11, 8, shade(bush, 800));
        ellipse(s, 13, 20, 6, 5, bush); ellipse(s, 20, 20, 6, 5, bush);
        for (int k = 0; k < 10; ++k) s.put(6 + noise(seed, k, 2) % 20, 15 + noise(seed, 2, k) % 12, shade(bush, 1300));
    }
    outline(s, rgba(18, 26, 14));
    return s;
}

static Sprite creature_sprite(const BodyPlan& p, const MaterialLook& L, std::uint64_t seed) {
    const int S = ProceduralAtlas::kStandee;
    Sprite s = blank(S, S);
    const int hue = static_cast<int>(seed % 360);
    const std::uint32_t body = mix(L.base, hsv(hue, 450, 600), 450);
    const std::uint32_t dark = shade(body, 650), light = shade(body, 1250);
    const int foot = S - 1, h = p.height;
    if (p.arms >= 2 && p.legs == 2) {
        // upright paper standee: legs, torso, arms, head
        const int torso_top = foot - h + 7, torso_bot = foot - 8;
        line(s, 14, torso_bot, 13, foot, dark); line(s, 18, torso_bot, 19, foot, dark);
        s.fill_rect(12, torso_top, 9, torso_bot - torso_top + 1, body);
        s.fill_rect(12, torso_top, 2, torso_bot - torso_top + 1, light);
        line(s, 11, torso_top + 1, 9, torso_top + 8, dark);     // arms
        line(s, 21, torso_top + 1, 23, torso_top + 8, dark);
        disc(s, 16, torso_top - 4, 4, mix(body, rgba(230, 190, 150), 500));    // head
        if (p.eyes) { s.put(15, torso_top - 4, rgba(15, 15, 15)); s.put(17, torso_top - 4, rgba(15, 15, 15)); }
    } else {
        // quadruped / hexapod: horizontal body on legs
        const int legs = std::max(2, p.legs);
        const int by = foot - std::max(4, h / 3), bh = std::max(3, h / 3);
        ellipse(s, 15, by - bh / 2, p.width / 2, bh / 2 + 1, body);
        ellipse(s, 13, by - bh / 2 - 1, p.width / 3, bh / 4 + 1, light);
        for (int k = 0; k < legs; ++k) {
            const int lx = 15 - p.width / 2 + 2 + k * (p.width - 4) / std::max(1, legs - 1);
            line(s, lx, by, lx, foot, dark);
        }
        const int hx = 15 + p.width / 2 + 1, hy = by - bh;
        if (p.heads) disc(s, hx, hy, std::max(2, bh / 2 + 1), body);
        if (p.eyes) s.put(hx + 1, hy - 1, rgba(10, 10, 10));
        for (int k = 0; k < p.horns; ++k) line(s, hx - 1 + k * 2, hy - 2, hx - 2 + k * 4, hy - 5, light);
        for (int k = 0; k < p.tails; ++k) line(s, 15 - p.width / 2, by - bh / 2, 15 - p.width / 2 - 4, by - bh - 2 + k, dark);
        for (int k = 0; k < p.wings; ++k) ellipse(s, 13 + k * 4, by - bh - 3, 5, 2, shade(light, 1100));
    }
    if (L.toxic) s.put(16, 2, L.accent);
    outline(s, rgba(16, 12, 10));
    return s;
}

// ---- generic engine kinds ---------------------------------------------------------------
static Sprite kind_sprite(const std::string& k) {
    const int I = ProceduralAtlas::kIcon;
    Sprite s = blank(I, I);
    const std::uint64_t seed = fnv(k);
    if (k == "inscription") {
        s.fill_rect(6, 3, 12, 19, rgba(120, 118, 112));
        s.fill_rect(6, 3, 2, 19, rgba(160, 158, 150));
        for (int y = 6; y < 19; y += 4) line(s, 9, y, 15, y, rgba(60, 58, 55));
    } else if (k == "enclosure") {
        s.fill_rect(3, 9, 18, 12, rgba(140, 110, 80)); line(s, 2, 9, 12, 2, rgba(90, 60, 40));
        line(s, 12, 2, 22, 9, rgba(90, 60, 40)); s.fill_rect(10, 14, 4, 7, rgba(40, 30, 20));
    } else if (k == "portal") {
        s.fill_rect(8, 4, 8, 18, rgba(70, 50, 30)); s.put(14, 13, rgba(220, 200, 90));
    } else if (k == "remains") {
        line(s, 5, 16, 19, 16, rgba(225, 220, 205)); disc(s, 18, 14, 3, rgba(225, 220, 205));
        for (int x = 7; x < 16; x += 3) line(s, x, 13, x, 19, rgba(200, 195, 180));
    } else if (k == "rubble" || k == "fragment" || k == "deposit") {
        for (int i = 0; i < 6; ++i)
            disc(s, 5 + noise(seed, i, 0) % 14, 12 + noise(seed, 0, i) % 8, 2 + i % 2, rgba(120 + i * 8, 110 + i * 6, 100));
    } else if (k == "residue" || k == "soil_patch") {
        ellipse(s, 12, 16, 9, 4, k == "residue" ? rgba(70, 70, 70) : rgba(110, 80, 50));
    } else if (k == "composite") {
        line(s, 4, 20, 19, 5, rgba(140, 100, 60)); line(s, 5, 20, 20, 5, rgba(110, 80, 50));
    } else {   // goods/items/processed: a sack/bundle
        ellipse(s, 12, 15, 7, 6, rgba(170, 140, 95)); line(s, 9, 9, 15, 9, rgba(110, 85, 55));
    }
    outline(s, rgba(18, 14, 10));
    return s;
}

// ---- build -------------------------------------------------------------------------------
void ProceduralAtlas::build(const World& w) {
    w_ = &w;
    mat_.clear(); mat_icon_.clear(); look_.clear(); sp_.clear(); kind_.clear(); plan_.clear(); cache_.clear();
    for (const MaterialDef& m : w.materials.defs()) {
        const MaterialLook L = classify(w, m.id);
        look_[m.id] = L;
        mat_[m.id] = material_tile(L, fnv(m.name), mprop(w, m, "brittleness"));
        mat_icon_[m.id] = make_material_icon(L, fnv(m.name));
    }
    for (const SpeciesDef& sp : w.species.defs()) {
        const BodyPlan p = body_plan(w, sp.name);
        plan_[sp.name] = p;
        const MaterialLook body = classify(w, sp.material);
        if (p.flora) {
            MaterialLook leaf = body; leaf.base = hsv(95 + static_cast<int>(fnv(sp.name) % 50), 600, 520);
            const PropertyDef* fm = w.props.find_name("fruit_material");
            if (fm && sp.props.get(fm->id) && sp.props.get(fm->id)->type == ValType::Text) {
                const MaterialId fruit = w.materials.id_of(sp.props.get(fm->id)->s);
                leaf.accent = classify(w, fruit).accent;
            }
            sp_[sp.name] = flora_sprite(p, body, leaf, fnv(sp.name));
        } else {
            sp_[sp.name] = creature_sprite(p, body, fnv(sp.name));
        }
    }
    for (const char* k : {"inscription", "enclosure", "portal", "remains", "rubble", "fragment",
                          "deposit", "residue", "soil_patch", "composite", "goods", "item",
                          "raw_resource", "processed_material", "waste"})
        kind_[k] = kind_sprite(k);
    // a generic humanoid for agents of no declared species
    BodyPlan hum; hum.arms = 2; hum.legs = 2; hum.heads = 1; hum.eyes = 2; hum.height = 26;
    MaterialLook skin; skin.base = rgba(190, 150, 120);
    kind_["person"] = creature_sprite(hum, skin, fnv("person"));
}

const Sprite* ProceduralAtlas::material(MaterialId id) const { auto it = mat_.find(id); return it == mat_.end() ? nullptr : &it->second; }
const Sprite* ProceduralAtlas::material_icon(MaterialId id) const { auto it = mat_icon_.find(id); return it == mat_icon_.end() ? nullptr : &it->second; }
const Sprite* ProceduralAtlas::species(const std::string& n) const { auto it = sp_.find(n); return it == sp_.end() ? nullptr : &it->second; }
const Sprite* ProceduralAtlas::kind(const std::string& n) const { auto it = kind_.find(n); return it == kind_.end() ? nullptr : &it->second; }
const MaterialLook* ProceduralAtlas::look(MaterialId id) const { auto it = look_.find(id); return it == look_.end() ? nullptr : &it->second; }
const BodyPlan* ProceduralAtlas::plan(const std::string& n) const { auto it = plan_.find(n); return it == plan_.end() ? nullptr : &it->second; }

std::size_t ProceduralAtlas::size() const {
    return mat_.size() + mat_icon_.size() + sp_.size() + kind_.size() + cache_.size();
}
std::size_t ProceduralAtlas::bytes() const {
    std::size_t b = 0;
    auto add = [&](const auto& m) { for (const auto& [k, s] : m) b += s.px.size() * 4; };
    add(mat_); add(mat_icon_); add(sp_); add(kind_); add(cache_);
    return b;
}

// ---- runtime variants ----------------------------------------------------------------------
const Sprite& ProceduralAtlas::fruiting(const std::string& species, int fruit, int max_fruit) {
    const int dots = max_fruit > 0 ? std::clamp(fruit * 12 / max_fruit, 0, 12) : 0;
    const std::string key = "fruit|" + species + "|" + std::to_string(dots);
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;
    Sprite s;
    if (const Sprite* b = this->species(species)) s = *b; else s = blank(kStandee, kStandee);
    std::uint32_t berry = rgba(200, 40, 70);
    if (w_) {
        const SpeciesDef* sp = w_->species.find_name(species);
        const PropertyDef* fm = w_->props.find_name("fruit_material");
        if (sp && fm && sp->props.get(fm->id) && sp->props.get(fm->id)->type == ValType::Text)
            berry = classify(*w_, w_->materials.id_of(sp->props.get(fm->id)->s)).accent;
    }
    // EXACT proportional count: dots = round(12 * fruit / max_fruit)
    for (int k = 0; k < dots; ++k)
        disc(s, 8 + (k * 5) % 16, 16 + (k * 3) % 9, 1, berry);
    return cache_.emplace(key, std::move(s)).first->second;
}

const Sprite& ProceduralAtlas::wall(std::uint8_t mask, int crack, MaterialId mat) {
    crack = std::clamp(crack, 0, 3);
    const std::string key = "wall|" + std::to_string(mask) + "|" + std::to_string(crack) + "|" + std::to_string(mat);
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;
    // 8-neighbour mask bits: 0 N,1 NE,2 E,3 SE,4 S,5 SW,6 W,7 NW. The 47-tile
    // reduction: a corner bit only matters when both adjacent edges are set.
    auto bit = [&](int b) { return (mask >> b) & 1; };
    std::uint8_t m = mask & 0x55;                     // edges
    if (bit(0) && bit(2) && bit(1)) m |= 1 << 1;
    if (bit(2) && bit(4) && bit(3)) m |= 1 << 3;
    if (bit(4) && bit(6) && bit(5)) m |= 1 << 5;
    if (bit(6) && bit(0) && bit(7)) m |= 1 << 7;
    const MaterialLook* L = look(mat);
    const std::uint32_t top = L ? L->light : rgba(170, 150, 120), face = L ? L->base : rgba(130, 110, 85),
                        dark = L ? L->dark : rgba(70, 55, 40);
    const int T = kTile;
    Sprite s = blank(T, T);
    // 2.5D thickness: a top cap (lighter) over a front face (darker), joined
    // toward each connected neighbour
    const int c0 = 10, c1 = 22;
    auto cap = [&](int x0, int y0, int x1, int y1) {
        s.fill_rect(x0, y0, x1 - x0, y1 - y0, top);
        s.fill_rect(x0, y1, x1 - x0, 6, face);
        line(s, x0, y1 + 5, x1 - 1, y1 + 5, dark);
    };
    cap(c0, c0 - 4, c1, c1 - 6);
    if (m & 0x01) cap(c0, 0, c1, c0);
    if (m & 0x10) cap(c0, c1 - 6, c1, T - 6);
    if (m & 0x04) cap(c1, c0 - 4, T, c1 - 6);
    if (m & 0x40) cap(0, c0 - 4, c0, c1 - 6);
    for (int b = 1; b < 8; b += 2)
        if (m & (1 << b)) s.put(b == 1 || b == 3 ? T - 2 : 1, b == 1 || b == 7 ? 1 : T - 8, top);
    // cracks: inversely proportional to structural integrity
    for (int k = 0; k < crack * 3; ++k) {
        int x = 10 + static_cast<int>((mask * 7 + k * 13) % 12), y = 8;
        for (int st = 0; st < 5; ++st) { const int nx = x + ((k + st) % 3) - 1; line(s, x, y, nx, y + 3, rgba(25, 20, 18)); x = nx; y += 3; }
    }
    return cache_.emplace(key, std::move(s)).first->second;
}

const Sprite& ProceduralAtlas::weapon(std::int64_t sharp, std::int64_t blunt, std::int64_t elastic,
                                      bool coated, MaterialId mat) {
    const std::string key = "wpn|" + std::to_string(sharp / 100) + "|" + std::to_string(blunt / 100) + "|" +
                            std::to_string(elastic / 100) + "|" + std::to_string(coated) + "|" + std::to_string(mat);
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;
    const MaterialLook* L = look(mat);
    const std::uint32_t head = L ? L->light : rgba(190, 190, 200), haft = rgba(120, 85, 50);
    Sprite s = blank(kIcon, kIcon);
    if (elastic >= 300) {                         // bow: bent limb + string
        for (int y = 2; y < 22; ++y) { const int dx = (y - 12) * (y - 12) / 14; s.put(8 + dx, y, haft); s.put(9 + dx, y, shade(haft, 1200)); }
        line(s, 9, 2, 9, 21, rgba(230, 225, 210));
    } else if (blunt > sharp) {                   // mace: round heavy head
        line(s, 6, 21, 15, 9, haft);
        disc(s, 17, 7, 4 + static_cast<int>(blunt / 300), head);
    } else {                                      // blade / spear: bright long edge
        line(s, 5, 21, 10, 15, haft);
        const int len = 6 + static_cast<int>(sharp / 90);
        line(s, 10, 15, 10 + len, 15 - len, head);
        line(s, 11, 15, 11 + len, 15 - len, 0xFFFFFFFFu);
    }
    if (coated) { disc(s, 19, 4, 1, rgba(120, 220, 60)); s.put(19, 6, rgba(120, 220, 60)); }
    outline(s, rgba(15, 12, 10));
    return cache_.emplace(key, std::move(s)).first->second;
}

const Sprite& ProceduralAtlas::armor_overlay(std::int64_t apr, MaterialId mat) {
    const std::string key = "armor|" + std::to_string(apr / 100) + "|" + std::to_string(mat);
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;
    const MaterialLook* L = look(mat);
    Sprite s = blank(kStandee, kStandee);
    const std::uint32_t c = L ? L->base : rgba(150, 150, 160);
    s.fill_rect(12, 13, 9, 7, c);
    if (L && L->sheen) line(s, 13, 14, 19, 14, 0xFFFFFFFFu);
    for (int k = 0; k < apr / 300; ++k) line(s, 12, 15 + k * 2, 20, 15 + k * 2, shade(c, 700));
    return cache_.emplace(key, std::move(s)).first->second;
}

// Procedural ancient glyphs: each language seed defines its own stroke
// vocabulary (which of 12 strokes on a 3x3 lattice each glyph uses), so two
// tribes' scripts look visibly different and every carving is reproducible.
const Sprite& ProceduralAtlas::glyph(std::uint32_t lang_seed, int index) {
    const std::string key = "glyph|" + std::to_string(lang_seed) + "|" + std::to_string(index);
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;
    Sprite s = blank(8, 8);
    const std::uint64_t h = fnv(std::to_string(lang_seed) + "/" + std::to_string(index));
    // script style from the language seed: curved vs angular, stroke count
    const bool curved = (lang_seed >> 3) & 1;
    const int strokes = 2 + static_cast<int>((lang_seed >> 5) % 3);
    const int pts[9][2] = {{1,1},{4,1},{6,1},{1,4},{4,4},{6,4},{1,6},{4,6},{6,6}};
    const std::uint32_t ink = rgba(40, 32, 26);
    for (int k = 0; k < strokes; ++k) {
        const int a = static_cast<int>((h >> (k * 8)) % 9), b = static_cast<int>((h >> (k * 8 + 4)) % 9);
        if (curved && k == 0) { disc(s, pts[a][0], pts[a][1], 1, ink); continue; }
        line(s, pts[a][0], pts[a][1], pts[b][0], pts[b][1], ink);
    }
    return cache_.emplace(key, std::move(s)).first->second;
}

const Sprite& ProceduralAtlas::glyph_stone(std::uint32_t lang_seed, const std::string& text, bool hide) {
    const std::string key = "stone|" + std::to_string(lang_seed) + "|" + std::to_string(fnv(text)) + "|" + std::to_string(hide);
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;
    Sprite s = blank(kIcon, kIcon + 8);
    const std::uint32_t face = hide ? rgba(185, 160, 120) : rgba(128, 126, 120);
    s.fill_rect(3, 2, 18, 28, face);
    s.fill_rect(3, 2, 2, 28, shade(face, 1250));
    // one glyph per text token (up to 6), chosen by token hash in this script
    std::size_t tok = 0, start = 0;
    for (std::size_t i = 0; i <= text.size() && tok < 6; ++i) {
        if (i == text.size() || text[i] == ' ' || text[i] == '|' || text[i] == ':' || text[i] == ',' || text[i] == ';') {
            if (i > start) {
                const int gi = static_cast<int>(fnv(text.substr(start, i - start)) % 64);
                const Sprite g = glyph(lang_seed, gi);       // copy: cache may rehash
                const int gx = 4 + static_cast<int>(tok % 2) * 8, gy = 4 + static_cast<int>(tok / 2) * 8;
                for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x)
                    if (A(g.at(x, y))) s.put(gx + x, gy + y, g.at(x, y));
                ++tok;
            }
            start = i + 1;
        }
    }
    outline(s, rgba(20, 18, 15));
    return cache_.emplace(key, std::move(s)).first->second;
}

}  // namespace simlab::render
