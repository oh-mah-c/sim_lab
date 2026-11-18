#pragma once
#include <cstdint>
#include <string>
#include <stdexcept>

namespace simlab {

// Stable identifiers. Ids are monotonic and never reused within a run.
using EntityId = std::uint64_t;
using EventId = std::uint64_t;
using PropId = std::uint32_t;
using MaterialId = std::uint32_t;
using LawId = std::uint32_t;
using DepositId = std::uint32_t;

constexpr EntityId kNoEntity = 0;
constexpr EventId kNoEvent = 0;
constexpr MaterialId kNoMaterial = 0;
constexpr PropId kNoProp = 0;
constexpr DepositId kNoDeposit = 0;

// Fixed-point scalar for simulation state: 1/1000 units. Deterministic on
// every platform — floats are never stored in world state.
struct Fx {
    std::int64_t raw = 0;  // milli-units
    static Fx from_int(std::int64_t v) { return {v * 1000}; }
    static Fx ratio(std::int64_t n, std::int64_t d) { return {d ? n * 1000 / d : 0}; }
    std::int64_t floor_int() const { return raw >= 0 ? raw / 1000 : -((-raw + 999) / 1000); }
    double to_double() const { return raw / 1000.0; }
};
inline Fx operator+(Fx a, Fx b) { return {a.raw + b.raw}; }
inline Fx operator-(Fx a, Fx b) { return {a.raw - b.raw}; }
inline Fx operator*(Fx a, Fx b) { return {a.raw * b.raw / 1000}; }
inline Fx operator/(Fx a, Fx b) { return b.raw ? Fx{a.raw * 1000 / b.raw} : Fx{}; }
inline bool operator<(Fx a, Fx b) { return a.raw < b.raw; }
inline bool operator>(Fx a, Fx b) { return a.raw > b.raw; }
inline bool operator<=(Fx a, Fx b) { return a.raw <= b.raw; }
inline bool operator>=(Fx a, Fx b) { return a.raw >= b.raw; }
inline bool operator==(Fx a, Fx b) { return a.raw == b.raw; }

// Strict data-file number parsing: the whole token must be a number
// (std::stoll silently accepts "12abc"). Throws std::invalid_argument.
inline std::int64_t parse_i64(const std::string& t) {
    std::size_t n = 0;
    const std::int64_t v = std::stoll(t, &n);
    if (n != t.size()) throw std::invalid_argument("trailing characters: " + t);
    return v;
}
inline std::uint64_t parse_u64(const std::string& t) {
    if (!t.empty() && t[0] == '-') throw std::invalid_argument("negative: " + t);
    std::size_t n = 0;
    const std::uint64_t v = std::stoull(t, &n);
    if (n != t.size()) throw std::invalid_argument("trailing characters: " + t);
    return v;
}

// Typed property value. `i` carries Integer payloads and Fixed payloads in
// milli-units; `ref` carries an EntityId/MaterialId for Ref values.
enum class ValType : std::uint8_t { Integer = 1, Fixed, Boolean, Text, Ref };
struct Value {
    ValType type = ValType::Integer;
    std::int64_t i = 0;
    bool b = false;
    std::string s;
    std::uint64_t target = 0;
    static Value integer(std::int64_t v) { Value x; x.type = ValType::Integer; x.i = v; return x; }
    static Value fixed(std::int64_t milli) { Value x; x.type = ValType::Fixed; x.i = milli; return x; }
    static Value boolean(bool v) { Value x; x.type = ValType::Boolean; x.b = v; return x; }
    static Value text(std::string v) { Value x; x.type = ValType::Text; x.s = std::move(v); return x; }
    static Value ref(std::uint64_t v) { Value x; x.type = ValType::Ref; x.target = v; return x; }
};
bool value_eq(const Value& a, const Value& b);
std::string value_str(const Value& v);

// 2D world position in integer map units.
struct Vec2 {
    std::int32_t x = 0, y = 0;
    bool operator==(const Vec2& o) const { return x == o.x && y == o.y; }
};
inline std::int64_t dist2(Vec2 a, Vec2 b) {
    const std::int64_t dx = static_cast<std::int64_t>(a.x) - b.x;
    const std::int64_t dy = static_cast<std::int64_t>(a.y) - b.y;
    return dx * dx + dy * dy;
}

}  // namespace simlab
