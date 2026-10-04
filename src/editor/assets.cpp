#include "editor/assets.hpp"
#include <numbers>

namespace atelier::editor {
namespace {
geometry::MeshRef plane() {
    return std::make_shared<const geometry::Mesh>(std::vector<geometry::Vertex>{
        {{-1,0,-1},{0,1,0}},{{-1,0,1},{0,1,0}},{{1,0,1},{0,1,0}},
        {{-1,0,-1},{0,1,0}},{{1,0,1},{0,1,0}},{{1,0,-1},{0,1,0}}},
        std::vector<std::uint8_t>{5,3});
}
geometry::MeshRef sphere() {
    constexpr unsigned rings=12, segments=24;
    std::vector<geometry::Vertex> vertices; vertices.reserve(6*segments*(rings-1));
    std::vector<std::uint8_t> edges; edges.reserve(2*segments*(rings-1));
    const auto vertex=[](unsigned y,unsigned x) {
        const auto latitude=std::numbers::pi_v<float>*static_cast<float>(y)/rings;
        const auto longitude=2*std::numbers::pi_v<float>*static_cast<float>(x)/segments;
        const auto a=std::sin(latitude)*std::cos(longitude), b=std::cos(latitude), c=std::sin(latitude)*std::sin(longitude);
        return geometry::Vertex{{a*.5f,b*.5f,c*.5f},{a,b,c}};
    };
    for (unsigned y=0;y<rings;++y) for (unsigned x=0;x<segments;++x) {
        const auto a=vertex(y,x),b=vertex(y+1,x),c=vertex(y+1,x+1),d=vertex(y,x+1);
        if (y) { vertices.insert(vertices.end(),{a,d,b}); edges.push_back(y+1==rings?7:6); }
        if (y+1<rings) { vertices.insert(vertices.end(),{d,c,b}); edges.push_back(y==0?7:5); }
    }
    return std::make_shared<const geometry::Mesh>(std::move(vertices),std::move(edges));
}
}
std::span<const MeshAsset> mesh_assets() {
    static const std::array assets{
        MeshAsset{"builtin/cube",L"Cube",geometry::cube_mesh()},
        MeshAsset{"builtin/sphere",L"Sphere",sphere()},
        MeshAsset{"builtin/plane",L"Plane",plane()}};
    return assets;
}
const MeshAsset* mesh_asset(std::string_view key) {
    for (const auto& asset:mesh_assets()) if (asset.key==key) return &asset;
    return nullptr;
}
}
