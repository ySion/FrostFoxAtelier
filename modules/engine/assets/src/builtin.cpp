#include "assets/reader.hpp"
#include "assets/builtin_materials.hpp"
#include "assets/material_format.hpp"
#include "foundation/sha256.hpp"
#include <numbers>

namespace atelier::assets {
namespace {
geometry::MeshRef plane() {
    auto mesh = geometry::Mesh::create(Array<geometry::Vertex>{{{-1, 0, -1}, {0, 1, 0}, {0, 1}},
                                                               {{-1, 0, 1}, {0, 1, 0}, {0, 0}},
                                                               {{1, 0, 1}, {0, 1, 0}, {1, 0}},
                                                               {{-1, 0, -1}, {0, 1, 0}, {0, 1}},
                                                               {{1, 0, 1}, {0, 1, 0}, {1, 0}},
                                                               {{1, 0, -1}, {0, 1, 0}, {1, 1}}},
                                       Array<std::uint8_t>{5, 3});
    if (!mesh)
        fatal(mesh.error().message);
    return std::move(*mesh);
}

geometry::MeshRef sphere() {
    constexpr unsigned rings = 12;
    constexpr unsigned segments = 24;
    Array<geometry::Vertex> vertices;
    vertices.reserve(6 * segments * (rings - 1));
    Array<std::uint8_t> edges;
    edges.reserve(2 * segments * (rings - 1));
    const auto vertex = [](unsigned y, unsigned x) {
        const auto latitude = std::numbers::pi_v<float> * static_cast<float>(y) / rings;
        const auto longitude = 2 * std::numbers::pi_v<float> * static_cast<float>(x) / segments;
        const auto a = std::sin(latitude) * std::cos(longitude);
        const auto b = std::cos(latitude);
        const auto c = std::sin(latitude) * std::sin(longitude);
        return geometry::Vertex{
            {a * .5f, b * .5f, c * .5f}, {a, b, c}, {static_cast<float>(x) / segments, static_cast<float>(y) / rings}};
    };
    for (unsigned y = 0; y < rings; ++y)
        for (unsigned x = 0; x < segments; ++x) {
            const auto a = vertex(y, x);
            const auto b = vertex(y + 1, x);
            const auto c = vertex(y + 1, x + 1);
            const auto d = vertex(y, x + 1);
            if (y) {
                vertices.append({a, d, b});
                edges.add(y + 1 == rings ? 7 : 6);
            }
            if (y + 1 < rings) {
                vertices.append({d, c, b});
                edges.add(y == 0 ? 7 : 5);
            }
        }
    auto mesh = geometry::Mesh::create(std::move(vertices), std::move(edges));
    if (!mesh)
        fatal(mesh.error().message);
    return std::move(*mesh);
}

struct Producer {
    const char* key;

    const wchar_t* name;

    std::uint32_t semantic_version;

    geometry::MeshRef (*make)();
};

// This table owns identities, order and producer compatibility declarations.
constexpr std::array producers{Producer{"builtin/cube", L"Cube", 1, geometry::cube_mesh},
                               Producer{"builtin/sphere", L"Sphere", 1, sphere},
                               Producer{"builtin/plane", L"Plane", 1, plane}};
}

Span<const MeshAsset> mesh_assets() {
    static const auto assets = [] {
        std::array<MeshAsset, producers.size()> result;
        for (std::size_t i = 0; i < producers.size(); ++i) {
            const auto& p = producers[i];
            result[i] = {p.key, p.name, p.make()};
        }
        return result;
    }();
    return assets;
}

Result<EngineDependency> Reader::engine_dependency(std::string_view key, AssetKind expected) const {
    for (const auto& asset : material_assets())
        if (asset.key == key) {
            if (expected != AssetKind::material)
                return std::unexpected(
                    Error{ErrorCode::invalid_definition,
                          "Installed engine asset is a material, not the requested kind: " + std::string(key)});
            const auto* author = std::get_if<MaterialSourceAsset>(&asset.author);
            if (!author)
                return std::unexpected(
                    Error{ErrorCode::invalid_definition, "Installed material catalogue requires an MS definition."});
            const auto source_file = builtin_material_source(author->source_path);
            if (!source_file)
                return std::unexpected(Error{ErrorCode::invalid_definition,
                                             "Installed material Source is missing: " + author->source_path});
            // v2 canonical JSON author data plus actual Source bytes; no runtime IDs.
            auto encoded = [&]() -> json::Result<Array<char>> {
                json::Builder builder;
                const auto root = builder.root();
                builder.set_validated(root, "domain", "Atelier.MaterialSource");
                ATELIER_TRY(builder.set(root, "digest_version", material_digest_version));
                ATELIER_TRY_BIND(author_node, builder.object(root, "author"));
                ATELIER_TRY(material_format::encode(builder, *author_node, asset));
                builder.set_validated(root, "key", asset.key);
                ATELIER_TRY(builder.set(root, "source_path", source_file->path));
                ATELIER_TRY(builder.set(root, "source", source_file->source));
                return json::write(root);
            }();
            if (!encoded)
                return std::unexpected(Error{ErrorCode::invalid_definition, encoded.error().describe()});
            return EngineDependency{asset.key, AssetKind::material, 1, material_digest_version,
                                    foundation::sha256(as_bytes(encoded->view()))};
        }
    const auto at = std::ranges::find(producers, key, &Producer::key);
    if (at == producers.end())
        return std::unexpected(
            Error{ErrorCode::invalid_definition, "Unknown installed engine asset: " + std::string(key)});
    if (expected != AssetKind::mesh)
        return std::unexpected(Error{ErrorCode::invalid_definition,
                                     "Installed engine asset is a mesh, not the requested kind: " + std::string(key)});
    const auto digest = mesh_content_sha256(mesh_assets()[static_cast<std::size_t>(at - producers.begin())]);
    if (!digest)
        return std::unexpected(digest.error());
    return EngineDependency{std::string(key), AssetKind::mesh, at->semantic_version, mesh_digest_version, *digest};
}
}
