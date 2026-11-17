#pragma once
#include "runtime/native.hpp"
#include "runtime/reflection.hpp"
#include <cstdint>

namespace atelier::acceptance::upstream {
enum class Mode : std::uint32_t { idle = 0, ready = 1 };

struct Payload {
    float scale{2.0f};
};
}

namespace atelier::reflection {
template <>
struct Reflected<::atelier::acceptance::upstream::Mode, TypeKind::enumeration> {};

template <>
struct Reflected<::atelier::acceptance::upstream::Payload, TypeKind::record> {};
}
