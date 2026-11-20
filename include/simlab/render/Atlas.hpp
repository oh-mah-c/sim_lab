#pragma once
// Phase 13 — Procedural Art Engine (SDL-free core).
//
// Every picture the viewer shows is GENERATED from data/*.txt at load time:
// no PNG ships with the game. A material's look is a pure function of its
// physical properties (hardness -> cracked stone, conductivity+density ->
// metal sheen, flammability+tensile -> wood grain, nutrition -> fruit,
// toxicity/volatility -> sickly tint ...); a species' look is a function of
// its anatomy (body_part roles: legs, arms, head, tail/wing/horn) and kind.
// Deterministic (FNV hash of names, no RNG state), pure CPU, RGBA8888 —
// unit-testable headless; the SDL viewer only uploads these buffers.
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "simlab/Types.hpp"

namespace simlab {
class World;
}

namespace simlab::render {

// 0xAARRGGBB, row-major.
struct Sprite {
    int w = 0, h = 0;
    std::vector<std::uint32_t> px;
    bool empty() const { return w == 0 || h == 0 || px.empty(); }
    std::uint32_t at(int x, int y) const { return px[static_cast<std::size_t>(y) * w + x]; }
    void put(int x, int y, std::uint32_t c);           // clipped, alpha-over
    void fill_rect(int x, int y, int ww, int hh, std::uint32_t c);
    std::size_t opaque_count() const;                   // alpha > 0
    std::uint64_t digest() const;                       // FNV over pixels
};

// What the generator decided a material IS (exposed for tests/gallery).
enum class MatClass : std::uint8_t {
    Rock, Metal, Wood, Plant, Food, Soil, Liquid, Flesh, Mineral, Energetic, Ash
};
const char* mat_class_name(MatClass c);

struct MaterialLook {
    MatClass cls = MatClass::Rock;
    std::uint32_t base = 0, dark = 0, light = 0, accent = 0;
    bool toxic = false;       // toxicity > 0 -> sickly purple tint + motes
    bool sheen = false;       // specular highlight (metal)
};

// Body plan the generator READ from species anatomy data.
struct BodyPlan {
    int legs = 0, arms = 0, heads = 0, tails = 0, wings = 0, horns = 0, eyes = 0;
    int height = 24;          // standee pixel height (from mass / part sizes)
    int width = 10;
    bool flora = false, fruiting = false, tree = false;
};

class ProceduralAtlas {
public:
    static constexpr int kTile = 32;      // terrain/material tile
    static constexpr int kStandee = 32;   // creature standee (32 wide x 32 high)
    static constexpr int kIcon = 24;      // items, weapons, glyph stones

    // Scan the world's registries (loaded from data/*.txt) and generate a
    // sprite for EVERY material, EVERY species and every built-in entity
    // kind the engine spawns. Safe to call again after content changes.
    void build(const World& w);

    const Sprite* material(MaterialId id) const;
    const Sprite* material_icon(MaterialId id) const;
    const Sprite* species(const std::string& name) const;
    const Sprite* kind(const std::string& kind_name) const;   // generic kinds
    const MaterialLook* look(MaterialId id) const;
    const BodyPlan* plan(const std::string& species) const;

    // Runtime variants (generated lazily, cached): a fruiting bush with N of
    // its max fruit dots, an enclosure wall tile for an 8-neighbour mask and
    // crack level, a weapon icon from its property channels, a glyph stone
    // for a language seed + text, a terrain tile for given fields.
    const Sprite& fruiting(const std::string& species, int fruit, int max_fruit);
    const Sprite& wall(std::uint8_t mask8, int crack_level, MaterialId mat);
    const Sprite& weapon(std::int64_t sharp, std::int64_t blunt, std::int64_t elastic,
                         bool coated, MaterialId mat);
    const Sprite& glyph_stone(std::uint32_t lang_seed, const std::string& text, bool hide);
    const Sprite& glyph(std::uint32_t lang_seed, int index);   // one 8x8 glyph
    const Sprite& armor_overlay(std::int64_t apr, MaterialId mat);

    std::size_t size() const;                        // total generated sprites
    std::size_t bytes() const;                       // pixel memory
    const std::map<MaterialId, Sprite>& materials() const { return mat_; }
    const std::map<std::string, Sprite>& species_sprites() const { return sp_; }
    const std::map<std::string, Sprite>& kinds() const { return kind_; }

    // pure helpers (tests)
    static MaterialLook classify(const World& w, MaterialId id);
    static BodyPlan body_plan(const World& w, const std::string& species);

private:
    const World* w_ = nullptr;
    std::map<MaterialId, Sprite> mat_, mat_icon_;
    std::map<MaterialId, MaterialLook> look_;
    std::map<std::string, Sprite> sp_, kind_;
    std::map<std::string, BodyPlan> plan_;
    std::map<std::string, Sprite> cache_;            // runtime variants
};

// colour utilities shared with the viewer
std::uint32_t rgba(int r, int g, int b, int a = 255);
std::uint32_t shade(std::uint32_t c, int permille);     // scale RGB
std::uint32_t mix(std::uint32_t a, std::uint32_t b, int t_permille);
std::uint64_t fnv(const std::string& s);

}  // namespace simlab::render
