#include "simlab/Types.hpp"

namespace simlab {

bool value_eq(const Value& a, const Value& b) {
    if (a.type != b.type) return false;
    switch (a.type) {
        case ValType::Integer: case ValType::Fixed: return a.i == b.i;
        case ValType::Boolean: return a.b == b.b;
        case ValType::Text: return a.s == b.s;
        case ValType::Ref: return a.target == b.target;
    }
    return false;
}

std::string value_str(const Value& v) {
    switch (v.type) {
        case ValType::Integer: return std::to_string(v.i);
        case ValType::Fixed: return std::to_string(v.i / 1000) + "." +
            ([](std::int64_t r) { std::string s = std::to_string(r < 0 ? -r : r);
                return std::string(3 - s.size(), '0') + s; }(v.i % 1000));
        case ValType::Boolean: return v.b ? "true" : "false";
        case ValType::Text: return v.s;
        case ValType::Ref: return std::to_string(v.target);
    }
    return "?";
}

}  // namespace simlab
