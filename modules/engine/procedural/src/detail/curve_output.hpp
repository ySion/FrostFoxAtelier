#pragma once

#include "polyline_arc.hpp"

namespace atelier::procedural::detail {

// One output primitive (or one loose sample) per input primitive, in input order.
class CurveOutput {
public:
    CurveOutput(const Geometry& source, bool points_only);
    void reserve(std::uint64_t points, std::uint64_t vertices);
    Result<void> emit(const PolylineArc& arc, ArcStation donor, double distance, math::Vector3 tangent,
                      bool close = false);
    void finish_primitive();
    Result<Geometry> geometry();

private:
    const Geometry& source_;
    bool points_only_, need_donors_;
    std::uint32_t first_point_{};
    Array<math::Vector3> positions_, tangents_;
    Array<float> lengths_, parameters_;
    Array<std::uint32_t> vertices_, offsets_{0};
    Array<ArcStation> donors_;
};

} // namespace atelier::procedural::detail
