#include "assets/reader.hpp"
#include <numbers>

namespace atelier::assets {
namespace {
geometry::MeshRef plane() {
    return std::make_shared<const geometry::Mesh>(std::vector<geometry::Vertex>{
        {{-1,0,-1},{0,1,0},{0,1}},{{-1,0,1},{0,1,0},{0,0}},{{1,0,1},{0,1,0},{1,0}},
        {{-1,0,-1},{0,1,0},{0,1}},{{1,0,1},{0,1,0},{1,0}},{{1,0,-1},{0,1,0},{1,1}}},
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
        return geometry::Vertex{{a*.5f,b*.5f,c*.5f},{a,b,c},{static_cast<float>(x)/segments,static_cast<float>(y)/rings}};
    };
    for (unsigned y=0;y<rings;++y) for (unsigned x=0;x<segments;++x) {
        const auto a=vertex(y,x),b=vertex(y+1,x),c=vertex(y+1,x+1),d=vertex(y,x+1);
        if (y) { vertices.insert(vertices.end(),{a,d,b}); edges.push_back(y+1==rings?7:6); }
        if (y+1<rings) { vertices.insert(vertices.end(),{d,c,b}); edges.push_back(y==0?7:5); }
    }
    return std::make_shared<const geometry::Mesh>(std::move(vertices),std::move(edges));
}
struct Producer {
    const char* key;
    const wchar_t* name;
    std::uint32_t semantic_version;
    geometry::MeshRef (*make)();
};
// This table owns identities, order and producer compatibility declarations.
constexpr std::array producers{
    Producer{"builtin/cube",L"Cube",1,geometry::cube_mesh},
    Producer{"builtin/sphere",L"Sphere",1,sphere},
    Producer{"builtin/plane",L"Plane",1,plane}};
}
std::span<const MeshAsset> mesh_assets() {
    static const auto assets=[] {
        std::array<MeshAsset,producers.size()> result;
        for(std::size_t i=0;i<producers.size();++i) {
            const auto& p=producers[i];result[i]={p.key,p.name,p.make()};
        }
        return result;
    }();
    return assets;
}
Result<EngineDependency> Reader::engine_dependency(std::string_view key,AssetKind expected) const {
    const auto at=std::ranges::find(producers,key,&Producer::key);
    if(at==producers.end()) return std::unexpected(Error{ErrorCode::invalid_definition,"Unknown installed engine asset: "+std::string(key)});
    if(expected!=AssetKind::mesh) return std::unexpected(Error{ErrorCode::invalid_definition,"Installed engine asset is a mesh, not the requested kind: "+std::string(key)});
    const auto digest=mesh_content_sha256(mesh_assets()[static_cast<std::size_t>(at-producers.begin())]);
    if(!digest) return std::unexpected(digest.error());
    return EngineDependency{std::string(key),AssetKind::mesh,at->semantic_version,mesh_digest_version,*digest};
}
}
