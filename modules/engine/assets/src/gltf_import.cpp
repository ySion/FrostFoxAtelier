#include "assets/mesh_import.hpp"
#include "assets/assets.hpp"
#include "assets/file.hpp"
#include "platform/error.hpp"
#include "platform/utf.hpp"
#include "json/json.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace atelier::assets {
namespace {
// Sized for the 2K Intel Sponza (largest file: 3.75M triangles, 134 MiB of buffers, 72 images,
// 1.1 GiB decoded) with about 2x headroom; documented in docs/modules/editor/projects.md.
constexpr std::size_t maximum_document = 16ull << 20;

constexpr std::size_t maximum_buffers = 512ull << 20;

constexpr std::size_t maximum_image_file = 64ull << 20;

constexpr std::size_t maximum_images = 256;

constexpr std::size_t maximum_decoded = 2ull << 30;

constexpr std::size_t maximum_triangles = 8'000'000;

constexpr std::size_t maximum_nodes = 65536;

constexpr std::size_t no_slot = ~std::size_t{};

json::Result<void> require(bool ok, std::string_view message) {
    if (!ok)
        return std::unexpected(json::Error{json::Fault::range, std::string(message), {}, {}});
    return {};
}

json::Result<json::Value> field(json::Value object, std::string_view key, json::Value fallback) {
    ATELIER_TRY_BIND(found, object.find(key));
    return *found ? **found : fallback;
}

json::Result<std::uint64_t> integer(json::Value value, std::uint64_t maximum = UINT64_MAX) {
    if (value.kind() == json::Kind::integer) {
        ATELIER_TRY_BIND(number, value.u64());
        if (*number <= maximum)
            return *number;
    } else {
        ATELIER_TRY_BIND(number, value.number());
        if (std::isfinite(*number) && *number >= 0 && *number < 0x1p64 && std::trunc(*number) == *number &&
            static_cast<std::uint64_t>(*number) <= maximum)
            return static_cast<std::uint64_t>(*number);
    }
    return std::unexpected(value.error(json::Fault::range, "Expected nonnegative integral value in range"));
}

// Build each top-level index only when it is actually consumed. Unused fields
// remain unvalidated, and entries borrow the immutable import document.
json::Result<json::Value> element(json::Value root, std::unordered_map<std::string_view, Array<json::Value>>& arrays,
                                  std::string_view name, json::Value index) {
    ATELIER_TRY_BIND(position, integer(index));
    auto found = arrays.find(name);
    if (found == arrays.end()) {
        ATELIER_TRY_BIND(value, root.at(name));
        ATELIER_TRY_BIND(items, value->array());
        Array<json::Value> entries;
        entries.reserve(items->size());
        for (auto item : *items)
            entries.emplace(item);
        found = arrays.emplace(name, std::move(entries)).first;
    }
    if (*position >= found->second.size())
        return std::unexpected(index.error(json::Fault::range, "Array index out of range"));
    return found->second[*position];
}

json::Result<double> number(json::Value object, std::string_view key, double fallback) {
    ATELIER_TRY_BIND(found, object.find(key));
    return *found ? (**found).number() : json::Result<double>{fallback};
}

template <std::size_t N>
json::Result<std::array<float, N>> numbers(json::Value object, std::string_view key, std::array<float, N> fallback) {
    ATELIER_TRY_BIND(found, object.find(key));
    if (!*found)
        return fallback;
    ATELIER_TRY_BIND(items, (**found).array());
    if (items->size() != N)
        return std::unexpected((**found).error(json::Fault::range, "Vector has the wrong length"));
    std::array<float, N> result{};
    std::size_t i{};
    for (auto item : *items) {
        ATELIER_TRY_BIND(number, item.f32());
        result[i++] = *number;
    }
    return result;
}

// RFC 3986 percent-decoding; the result is a relative path inside the source directory.
json::Result<std::filesystem::path> dependency(json::Value uri) {
    ATELIER_TRY_BIND(value_text, uri.string());
    const auto text = *value_text;
    ATELIER_TRY(require(!text.starts_with("data:"), "embedded data URIs are not supported."));
    std::string decoded;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '%') {
            decoded += text[i];
            continue;
        }
        unsigned value{};
        ATELIER_TRY(
            require(i + 2 < text.size() &&
                        std::from_chars(text.data() + i + 1, text.data() + i + 3, value, 16).ptr == text.data() + i + 3,
                    "invalid percent-encoding in uri."));
        decoded += static_cast<char>(value);
        i += 2;
    }
    auto converted = platform::utf8_to_utf16(decoded);
    if (!converted)
        return std::unexpected(uri.error(json::Fault::encoding, "Invalid UTF-8 text."));
    const auto path = std::filesystem::path(std::move(*converted));
    ATELIER_TRY(require(!path.empty() && !path.has_root_path(), "dependency must be a relative path."));
    for (const auto& part : path)
        ATELIER_TRY(require(part != L".." && part.wstring().find(L':') == std::wstring::npos,
                            "dependency must stay inside the source directory."));
    return path.lexically_normal();
}

using Matrix = std::array<double, 16>; // Column-major, as glTF stores it.

Matrix multiply(const Matrix& a, const Matrix& b) {
    Matrix r{};
    for (std::size_t c = 0; c < 4; ++c)
        for (std::size_t row = 0; row < 4; ++row)
            for (std::size_t k = 0; k < 4; ++k)
                r[c * 4 + row] += a[k * 4 + row] * b[c * 4 + k];
    return r;
}

json::Result<Matrix> local_matrix(json::Value node) {
    ATELIER_TRY_BIND(matrix, node.find("matrix"));
    if (*matrix) {
        ATELIER_TRY_BIND(m, numbers<16>(node, "matrix", {}));
        Matrix r{};
        std::ranges::copy(*m, r.begin());
        return r;
    }
    ATELIER_TRY_BIND(t, numbers<3>(node, "translation", {0, 0, 0}));
    ATELIER_TRY_BIND(q, numbers<4>(node, "rotation", {0, 0, 0, 1}));
    ATELIER_TRY_BIND(s, numbers<3>(node, "scale", {1, 1, 1}));
    const double x = (*q)[0];
    const double y = (*q)[1];
    const double z = (*q)[2];
    const double w = (*q)[3];
    const std::array<double, 9> rotation{1 - 2 * (y * y + z * z), 2 * (x * y + z * w),     2 * (x * z - y * w),
                                         2 * (x * y - z * w),     1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
                                         2 * (x * z + y * w),     2 * (y * z - x * w),     1 - 2 * (x * x + y * y)};
    Matrix r{};
    for (std::size_t c = 0; c < 3; ++c)
        for (std::size_t row = 0; row < 3; ++row)
            r[c * 4 + row] = rotation[c * 3 + row] * (*s)[c];
    r[12] = (*t)[0];
    r[13] = (*t)[1];
    r[14] = (*t)[2];
    r[15] = 1;
    return r;
}

struct Accessor {
    const std::byte* data{};

    std::size_t count{};

    std::size_t stride{};

    std::uint32_t component{};
};

std::size_t component_size(std::uint32_t type) {
    return type == 5121 ? 1 : type == 5123 ? 2 : 4;
}

json::Result<Accessor> accessor(json::Value gltf, std::unordered_map<std::string_view, Array<json::Value>>& indexed,
                                std::span<const std::string_view> buffers, json::Value index, std::string_view type,
                                std::initializer_list<std::uint32_t> components) {
    ATELIER_TRY_BIND(a, element(gltf, indexed, "accessors", index));
    ATELIER_TRY_BIND(sparse, a->find("sparse"));
    ATELIER_TRY(require(!*sparse, "sparse accessors are not supported."));
    ATELIER_TRY_BIND(actual_type, a->string({"type"}));
    ATELIER_TRY(require(*actual_type == type, "accessor has the wrong type for its attribute."));
    ATELIER_TRY_BIND(component_value, a->at("componentType"));
    ATELIER_TRY_BIND(component, integer(*component_value, UINT32_MAX));
    ATELIER_TRY(require(std::ranges::find(components, *component) != components.end(),
                        "accessor has an unsupported component type."));
    ATELIER_TRY_BIND(normalized, a->find("normalized"));
    if (*normalized) {
        ATELIER_TRY_BIND(enabled, (**normalized).boolean());
        ATELIER_TRY(require(!*enabled, "normalized accessors are not supported."));
    }
    ATELIER_TRY_BIND(view_index, a->at("bufferView"));
    ATELIER_TRY_BIND(view, element(gltf, indexed, "bufferViews", *view_index));
    ATELIER_TRY_BIND(buffer_value, view->at("buffer"));
    ATELIER_TRY_BIND(buffer, integer(*buffer_value));
    ATELIER_TRY(require(*buffer < buffers.size(), "invalid buffer index."));
    const auto bytes = buffers[*buffer];
    const std::uint64_t width = (type == "SCALAR" ? 1
                                 : type == "VEC2" ? 2
                                 : type == "VEC3" ? 3
                                                  : 4) *
                                component_size(static_cast<std::uint32_t>(*component));
    ATELIER_TRY_BIND(view_offset_value, view->find("byteOffset"));
    ATELIER_TRY_BIND(view_offset, *view_offset_value ? integer(**view_offset_value) : json::Result<std::uint64_t>{0});
    ATELIER_TRY_BIND(length_value, view->at("byteLength"));
    ATELIER_TRY_BIND(view_length, integer(*length_value));
    ATELIER_TRY_BIND(offset_value, a->find("byteOffset"));
    ATELIER_TRY_BIND(offset, *offset_value ? integer(**offset_value) : json::Result<std::uint64_t>{0});
    ATELIER_TRY_BIND(count_value, a->at("count"));
    ATELIER_TRY_BIND(count, integer(*count_value));
    ATELIER_TRY_BIND(stride_value, view->find("byteStride"));
    ATELIER_TRY_BIND(stride, *stride_value ? integer(**stride_value) : json::Result<std::uint64_t>{width});
    ATELIER_TRY(require(*count > 0 && *count <= std::uint64_t{1} << 32 && *stride >= width && *stride <= 252,
                        "accessor has an invalid count or stride."));
    ATELIER_TRY(require(*view_offset <= bytes.size() && *view_length <= bytes.size() - *view_offset &&
                            *offset <= *view_length && (*count - 1) * *stride + width <= *view_length - *offset,
                        "accessor exceeds its buffer."));
    return Accessor{reinterpret_cast<const std::byte*>(bytes.data()) + *view_offset + *offset,
                    static_cast<std::size_t>(*count), static_cast<std::size_t>(*stride),
                    static_cast<std::uint32_t>(*component)};
}

std::uint32_t index_at(const Accessor& a, std::size_t i) {
    const auto* p = a.data + i * a.stride;
    if (a.component == 5121)
        return std::to_integer<std::uint32_t>(*p);
    if (a.component == 5123) {
        std::uint16_t v;
        std::memcpy(&v, p, 2);
        return v;
    }
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

template <std::size_t N>
json::Result<std::array<double, N>> floats_at(const Accessor& a, std::size_t i) {
    std::array<float, N> v;
    std::memcpy(v.data(), a.data + i * a.stride, N * 4);
    std::array<double, N> result;
    for (std::size_t k = 0; k < N; ++k) {
        ATELIER_TRY(require(std::isfinite(v[k]), "a vertex attribute is not finite."));
        result[k] = v[k];
    }
    return result;
}

json::Result<std::string_view> alpha_mode(json::Value material) {
    ATELIER_TRY_BIND(value, material.find("alphaMode"));
    ATELIER_TRY_BIND(mode, *value ? (**value).string() : json::Result<std::string_view>{"OPAQUE"});
    ATELIER_TRY(require(*mode == "OPAQUE" || *mode == "MASK" || *mode == "BLEND", "invalid alphaMode."));
    return *mode;
}

json::Result<std::string> material_name(json::Value material, std::size_t index) {
    ATELIER_TRY_BIND(value, material.find("name"));
    ATELIER_TRY_BIND(name, *value ? (**value).string() : json::Result<std::string_view>{""});
    return name->empty() ? "material " + std::to_string(index) : std::string(*name);
}
}

Result<MeshImport> load_gltf(const std::filesystem::path& source, ImageCache* shared_images) {
    MeshImport result;
    std::error_code error;
    const auto root = std::filesystem::canonical(source.parent_path(), error);
    if (error)
        return std::unexpected(Error{ErrorCode::invalid_storage, platform::error_message(error)});
    std::unordered_map<std::wstring, std::size_t> read_files;
    const auto read = [&](const std::filesystem::path& relative, std::size_t maximum) -> json::Result<std::size_t> {
        if (const auto found = read_files.find(relative.generic_wstring()); found != read_files.end()) {
            ATELIER_TRY(
                require(result.files[found->second].bytes.size() <= maximum, "a dependency exceeds its size limit."));
            return found->second;
        }
        const auto path = std::filesystem::weakly_canonical(root / relative, error);
        if (error)
            return std::unexpected(json::Error{json::Fault::range, platform::error_message(error), {}, {}});
        const auto within = path.lexically_relative(root);
        ATELIER_TRY(require(!within.empty() && !within.has_root_path() && *within.begin() != L"..",
                            "dependency escapes through a link."));
        auto bytes = read_file(path, maximum);
        if (!bytes)
            return std::unexpected(json::Error{json::Fault::range, bytes.error().message, {}, {}});
        read_files.emplace(relative.generic_wstring(), result.files.size());
        result.files.push_back({relative, std::move(*bytes)});
        return result.files.size() - 1;
    };
    const auto imported = [&]() -> json::Result<void> {
        ATELIER_TRY_BIND(file, read(source.filename(), maximum_document));
        ATELIER_TRY_BIND(document, json::Document::parse_gltf(result.files[*file].bytes));
        const auto gltf = document->root();
        std::unordered_map<std::string_view, Array<json::Value>> indexed;
        const json::Builder array_default{json::Kind::array};
        const json::Builder object_default;
        const auto empty_array = array_default.root();
        const auto empty_object = object_default.root();
        ATELIER_TRY_BIND(version, gltf.string({"asset", "version"}));
        ATELIER_TRY(require(version->starts_with("2."), "only glTF 2.x is supported."));
        ATELIER_TRY_BIND(required, gltf.find("extensionsRequired"));
        if (*required) {
            ATELIER_TRY_BIND(extensions, (**required).array());
            ATELIER_TRY(require(!extensions->size(), "required extensions are not supported."));
        }
        // glTF is right-handed and the engine left-handed, both +Y up in meters (renderer contract:
        // "相机为左手系，+Y 向上"): mirroring X keeps an asset's +Z front facing +Z.
        const Matrix conversion{-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        ATELIER_TRY_BIND(nodes_value, field(gltf, "nodes", empty_array));
        ATELIER_TRY_BIND(node_items, nodes_value->array());
        ATELIER_TRY(require(node_items->size() <= maximum_nodes, "too many nodes."));
        // Nodes and materials are addressed repeatedly by glTF indices. Cache
        // borrowed nodes once rather than repeatedly walking linked arrays.
        Array<json::Value> nodes;
        for (auto node : *node_items)
            nodes.emplace(node);
        ATELIER_TRY_BIND(scene_value, gltf.find("scene"));
        ATELIER_TRY_BIND(scene_index, *scene_value ? integer(**scene_value) : json::Result<std::uint64_t>{0});
        ATELIER_TRY_BIND(scenes, gltf.at("scenes"));
        ATELIER_TRY_BIND(scene, scenes->at(*scene_index));
        // Depth-first in file order, so parts follow the document.
        std::vector<std::pair<std::size_t, Matrix>> placed;
        std::vector<std::pair<std::size_t, Matrix>> stack;
        std::vector<bool> visited(nodes.size());
        const auto push = [&](json::Value indices, const Matrix& parent) -> json::Result<void> {
            ATELIER_TRY_BIND(items, indices.array());
            const auto begin = stack.size();
            for (auto value : *items) {
                ATELIER_TRY_BIND(index, integer(value));
                ATELIER_TRY(require(*index < nodes.size(), "invalid node index."));
                stack.push_back({*index, parent});
            }
            std::reverse(stack.begin() + begin, stack.end());
            return {};
        };
        ATELIER_TRY_BIND(roots, field(*scene, "nodes", empty_array));
        ATELIER_TRY(push(*roots, conversion));
        while (!stack.empty()) {
            const auto [index, parent] = stack.back();
            stack.pop_back();
            ATELIER_TRY(require(!visited[index], "node graph is not a tree."));
            visited[index] = true;
            const auto& node = nodes[index];
            ATELIER_TRY_BIND(skin, node.find("skin"));
            ATELIER_TRY(require(!*skin, "skinned meshes are not supported."));
            ATELIER_TRY_BIND(local, local_matrix(node));
            const auto world = multiply(parent, *local);
            ATELIER_TRY_BIND(mesh, node.find("mesh"));
            if (*mesh)
                placed.push_back({index, world});
            ATELIER_TRY_BIND(children, field(node, "children", empty_array));
            ATELIER_TRY(push(*children, world));
        }
        const auto primitives = [&](std::size_t node) -> json::Result<json::ArrayView> {
            ATELIER_TRY_BIND(index, nodes[node].at("mesh"));
            ATELIER_TRY_BIND(mesh, element(gltf, indexed, "meshes", *index));
            ATELIER_TRY_BIND(items, mesh->at("primitives"));
            return items->array();
        };
        const auto corner_count = [&](json::Value primitive) -> json::Result<std::uint64_t> {
            ATELIER_TRY_BIND(indices, primitive.find("indices"));
            ATELIER_TRY_BIND(index, *indices ? json::Result<json::Value>{**indices}
                                             : primitive.path({"attributes", "POSITION"}));
            ATELIER_TRY_BIND(access, element(gltf, indexed, "accessors", *index));
            ATELIER_TRY_BIND(count, access->at("count"));
            return integer(*count);
        };
        // Material slots: materials used by imported primitives, in file order; BLEND is left out.
        ATELIER_TRY_BIND(materials_value, field(gltf, "materials", empty_array));
        ATELIER_TRY_BIND(material_items, materials_value->array());
        Array<json::Value> materials;
        for (auto material : *material_items)
            materials.emplace(material);
        std::vector<bool> used(materials.size());
        bool default_used = false;
        std::size_t corners{};
        struct Skipped {
            std::size_t primitives{};

            std::size_t triangles{};
        };
        std::map<std::size_t, Skipped> skipped;
        for (const auto& [index, world] : placed) {
            ATELIER_TRY_BIND(parts, primitives(index));
            for (auto primitive : *parts) {
                ATELIER_TRY_BIND(mode_value, primitive.find("mode"));
                ATELIER_TRY_BIND(mode, *mode_value ? integer(**mode_value) : json::Result<std::uint64_t>{4});
                ATELIER_TRY(require(*mode == 4, "only triangle-list primitives are supported."));
                ATELIER_TRY_BIND(count, corner_count(primitive));
                ATELIER_TRY_BIND(material_value, primitive.find("material"));
                if (*material_value) {
                    ATELIER_TRY_BIND(m, integer(**material_value));
                    ATELIER_TRY(require(*m < materials.size(), "invalid material index."));
                    ATELIER_TRY_BIND(alpha, alpha_mode(materials[*m]));
                    if (*alpha == "BLEND") {
                        auto& s = skipped[*m];
                        ++s.primitives;
                        s.triangles += *count / 3;
                        continue;
                    }
                    used[*m] = true;
                } else
                    default_used = true;
                ATELIER_TRY(require(*count <= maximum_triangles * 3 - corners, "too many triangles."));
                corners += *count;
            }
        }
        for (const auto& [m, count] : skipped) {
            ATELIER_TRY_BIND(name, material_name(materials[m], m));
            result.notes.push_back("BLEND material skipped: " + *name + " (" + std::to_string(count.primitives) +
                                   " primitives, " + std::to_string(count.triangles) +
                                   " triangles); alpha blending is not supported.");
        }
        // Read every dependency before taking views: result.files grows while reading.
        std::vector<std::size_t> buffer_files;
        std::size_t buffer_bytes{};
        ATELIER_TRY_BIND(buffer_values, field(gltf, "buffers", empty_array));
        ATELIER_TRY_BIND(buffer_items, buffer_values->array());
        for (auto buffer : *buffer_items) {
            ATELIER_TRY_BIND(uri, buffer.at("uri"));
            ATELIER_TRY_BIND(path, dependency(*uri));
            ATELIER_TRY_BIND(file, read(*path, maximum_buffers - buffer_bytes));
            buffer_bytes += result.files[*file].bytes.size();
            ATELIER_TRY_BIND(length_value, buffer.at("byteLength"));
            ATELIER_TRY_BIND(length, integer(*length_value));
            ATELIER_TRY(require(*length <= result.files[*file].bytes.size(), "buffer is shorter than its byteLength."));
            buffer_files.push_back(*file);
        }
        ATELIER_TRY_BIND(images_value, field(gltf, "images", empty_array));
        ATELIER_TRY_BIND(image_items, images_value->array());
        auto& images = indexed["images"];
        images.reserve(image_items->size());
        for (auto image : *image_items)
            images.emplace(image);
        std::vector<std::size_t> image_slot(images.size(), no_slot);
        std::vector<std::size_t> image_files;
        const auto texture_image = [&](json::Value info) -> json::Result<std::size_t> {
            ATELIER_TRY_BIND(coord_value, info.find("texCoord"));
            ATELIER_TRY_BIND(coord, *coord_value ? integer(**coord_value) : json::Result<std::uint64_t>{0});
            ATELIER_TRY(require(*coord == 0, "textures must use TEXCOORD_0."));
            ATELIER_TRY_BIND(index, info.at("index"));
            ATELIER_TRY_BIND(texture, element(gltf, indexed, "textures", *index));
            ATELIER_TRY_BIND(source_value, texture->at("source"));
            ATELIER_TRY_BIND(source_index, integer(*source_value));
            ATELIER_TRY_BIND(image, element(gltf, indexed, "images", *source_value));
            auto& slot = image_slot[*source_index];
            if (slot == no_slot) {
                ATELIER_TRY(require(image_files.size() < maximum_images, "too many images."));
                ATELIER_TRY_BIND(view, image->find("bufferView"));
                ATELIER_TRY(require(!*view, "images stored in buffers are not supported."));
                ATELIER_TRY_BIND(uri, image->at("uri"));
                ATELIER_TRY_BIND(path, dependency(*uri));
                // Generated materials reference the image as a Project texture asset.
                ATELIER_TRY(require(is_texture_path(*path), "images must be PNG, JPEG, BMP or TIFF files."));
                slot = image_files.size();
                ATELIER_TRY_BIND(file, read(*path, maximum_image_file));
                image_files.push_back(*file);
            }
            return slot;
        };
        for (std::size_t m = 0; m < materials.size(); ++m)
            if (used[m]) {
                ATELIER_TRY_BIND(pbr, field(materials[m], "pbrMetallicRoughness", empty_object));
                for (const auto* key : {"baseColorTexture", "metallicRoughnessTexture"}) {
                    ATELIER_TRY_BIND(info, pbr->find(key));
                    if (*info)
                        ATELIER_TRY(texture_image(**info));
                }
                for (const auto* key : {"normalTexture", "emissiveTexture"}) {
                    ATELIER_TRY_BIND(info, materials[m].find(key));
                    if (*info)
                        ATELIER_TRY(texture_image(**info));
                }
            }
        std::vector<std::string_view> buffers;
        std::vector<std::string_view> image_sources;
        for (const auto file : buffer_files)
            buffers.push_back(result.files[file].bytes);
        for (const auto file : image_files)
            image_sources.push_back(result.files[file].bytes);
        // Decode to validate the same images and byte budget. Author MI values
        // carry paths/keys, never decoded pixels or a second surface definition.
        static_cast<void>(decode_images(image_sources, shared_images, maximum_decoded));
        const auto instance = [&](json::Value source_material,
                                  std::array<std::filesystem::path, imported_material_inputs.size()>& files)
            -> json::Result<MaterialInstanceAsset> {
            MaterialInstanceAsset author;
            const auto texture = [&](std::size_t input, json::Value info, float channel = 0.f) -> json::Result<void> {
                // Every used material texture was resolved above, before taking
                // views into result.files. Publication only consumes that snapshot.
                ATELIER_TRY_BIND(index, info.at("index"));
                ATELIER_TRY_BIND(source_texture, element(gltf, indexed, "textures", *index));
                ATELIER_TRY_BIND(source_value, source_texture->at("source"));
                ATELIER_TRY_BIND(source_index, integer(*source_value));
                const auto image = image_slot[*source_index];
                files[input] = result.files[image_files[image]].relative;
                ATELIER_TRY_BIND(sampler_index, source_texture->find("sampler"));
                ATELIER_TRY_BIND(sampler, *sampler_index ? element(gltf, indexed, "samplers", **sampler_index)
                                                         : json::Result<json::Value>{empty_object});
                ATELIER_TRY_BIND(wrap_s, sampler->find("wrapS"));
                ATELIER_TRY_BIND(wrap_t, sampler->find("wrapT"));
                ATELIER_TRY_BIND(s, *wrap_s ? integer(**wrap_s) : json::Result<std::uint64_t>{10497});
                ATELIER_TRY_BIND(t, *wrap_t ? integer(**wrap_t) : json::Result<std::uint64_t>{10497});
                const auto wrap = *s;
                ATELIER_TRY(require(wrap == *t && (wrap == 10497 || wrap == 33071 || wrap == 33648),
                                    "samplers must use one supported wrap mode for both axes."));
                const auto address = wrap == 33071   ? material::Address::clamp
                                     : wrap == 33648 ? material::Address::mirror
                                                     : material::Address::repeat;
                const auto name = std::string(imported_material_inputs[input]);
                author.entries.push_back({name + "_texture", material::TextureBinding{{}, address}});
                author.entries.push_back({name + "_uv.scale", math::Vector2{1, 1}});
                author.entries.push_back({name + "_uv.offset", math::Vector2{}});
                author.entries.push_back({name + "_uv.rotation", 0.f});
                if (input == 1 || input == 2)
                    author.entries.push_back({name + "_channel", channel});
                return {};
            };
            ATELIER_TRY_BIND(pbr, field(source_material, "pbrMetallicRoughness", empty_object));
            ATELIER_TRY_BIND(color, numbers<4>(*pbr, "baseColorFactor", {1, 1, 1, 1}));
            ATELIER_TRY_BIND(metallic_value, number(*pbr, "metallicFactor", 1));
            ATELIER_TRY_BIND(roughness_value, number(*pbr, "roughnessFactor", 1));
            ATELIER_TRY_BIND(normal_info, field(source_material, "normalTexture", empty_object));
            ATELIER_TRY_BIND(scale_value, number(*normal_info, "scale", 1));
            ATELIER_TRY_BIND(emissive, numbers<3>(source_material, "emissiveFactor", {0, 0, 0}));
            ATELIER_TRY_BIND(cutoff_value, number(source_material, "alphaCutoff", .5));
            const auto metallic = static_cast<float>(*metallic_value);
            const auto roughness = static_cast<float>(*roughness_value);
            const auto normal_scale = static_cast<float>(*scale_value);
            const auto cutoff = static_cast<float>(*cutoff_value);
            const auto unit = [](float value) { return std::isfinite(value) && value >= 0 && value <= 1; };
            ATELIER_TRY(require(std::ranges::all_of(*color, unit) && unit(metallic) && unit(roughness) && unit(cutoff),
                                "invalid material color, factor or alpha cutoff."));
            ATELIER_TRY(require(std::isfinite(normal_scale) && normal_scale >= 0 && normal_scale <= 16,
                                "invalid normal scale."));
            ATELIER_TRY(require(
                std::ranges::all_of(*emissive,
                                    [](float value) { return std::isfinite(value) && value >= 0 && value <= 65504.f; }),
                "invalid emissive factor."));
            ATELIER_TRY_BIND(alpha, alpha_mode(source_material));
            const bool mask = *alpha == "MASK";
            ATELIER_TRY_BIND(sided, source_material.find("doubleSided"));
            ATELIER_TRY_BIND(two_sided, *sided ? (**sided).boolean() : json::Result<bool>{false});
            author.parent_key =
                mask ? (*two_sided ? "builtin/material/standard-mask-double" : "builtin/material/standard-mask-single")
                     : (*two_sided ? "builtin/material/standard-opaque-double"
                                   : "builtin/material/standard-opaque-single");
            // glTF's implicit factors are part of its appearance (not the engine
            // defaults). Explicit values equal to defaults remain overrides too.
            author.entries = {{"base_color", math::Vector4{(*color)[0], (*color)[1], (*color)[2], (*color)[3]}},
                              {"roughness", roughness},
                              {"metallic", metallic},
                              {"normal_scale", normal_scale},
                              {"normal_y_sign", 1.f},
                              {"emissive", math::Vector3{(*emissive)[0], (*emissive)[1], (*emissive)[2]}},
                              {"alpha_cutoff", cutoff}};
            ATELIER_TRY_BIND(base_texture, pbr->find("baseColorTexture"));
            if (*base_texture)
                ATELIER_TRY(texture(0, **base_texture));
            ATELIER_TRY_BIND(mr_texture, pbr->find("metallicRoughnessTexture"));
            if (*mr_texture) {
                ATELIER_TRY(texture(1, **mr_texture, 1.f));
                ATELIER_TRY(texture(2, **mr_texture, 2.f));
            }
            ATELIER_TRY_BIND(normal_texture, source_material.find("normalTexture"));
            if (*normal_texture)
                ATELIER_TRY(texture(3, **normal_texture));
            ATELIER_TRY_BIND(emissive_texture, source_material.find("emissiveTexture"));
            if (*emissive_texture)
                ATELIER_TRY(texture(4, **emissive_texture));
            return author;
        };
        // Slot names are unique: scenes bind materials to slots by name.
        std::vector<geometry::MaterialSlot> slots;
        std::vector<std::size_t> slot_of(materials.size(), no_slot);
        std::unordered_set<std::string> names;
        const auto add_slot = [&](const std::string& name, const json::Value* source_material) -> json::Result<void> {
            auto unique = name;
            for (std::size_t n = 2; !names.insert(unique).second; ++n)
                unique = name + " (" + std::to_string(n) + ")";
            slots.push_back({std::move(unique)});
            result.textures.emplace_back();
            if (source_material) {
                ATELIER_TRY_BIND(material, instance(*source_material, result.textures.back()));
                result.materials.emplace_back(std::move(*material));
            } else
                result.materials.emplace_back();
            return {};
        };
        for (std::size_t m = 0; m < materials.size(); ++m)
            if (used[m]) {
                slot_of[m] = slots.size();
                ATELIER_TRY_BIND(name, material_name(materials[m], m));
                ATELIER_TRY(add_slot(*name, &materials[m]));
            }
        const auto default_slot = slots.size();
        if (default_used)
            ATELIER_TRY(add_slot("default", nullptr));
        std::vector<geometry::Vertex> vertices;
        vertices.reserve(corners);
        std::vector<geometry::MeshPart> parts;
        for (const auto& [index, world] : placed) {
            const std::array<double, 9> linear{world[0], world[1], world[2], world[4], world[5],
                                               world[6], world[8], world[9], world[10]};
            const double det = linear[0] * (linear[4] * linear[8] - linear[7] * linear[5]) -
                               linear[3] * (linear[1] * linear[8] - linear[7] * linear[2]) +
                               linear[6] * (linear[1] * linear[5] - linear[4] * linear[2]);
            ATELIER_TRY(require(std::isfinite(det) && det != 0, "a mesh node has a singular transform."));
            // Normals use the cofactor matrix: the inverse transpose scaled by the positive |det|.
            const double sign = det < 0 ? -1 : 1;
            const std::array<double, 9> cofactor{(linear[4] * linear[8] - linear[5] * linear[7]) * sign,
                                                 (linear[5] * linear[6] - linear[3] * linear[8]) * sign,
                                                 (linear[3] * linear[7] - linear[4] * linear[6]) * sign,
                                                 (linear[2] * linear[7] - linear[1] * linear[8]) * sign,
                                                 (linear[0] * linear[8] - linear[2] * linear[6]) * sign,
                                                 (linear[1] * linear[6] - linear[0] * linear[7]) * sign,
                                                 (linear[1] * linear[5] - linear[2] * linear[4]) * sign,
                                                 (linear[2] * linear[3] - linear[0] * linear[5]) * sign,
                                                 (linear[0] * linear[4] - linear[1] * linear[3]) * sign};
            const auto apply = [](const std::array<double, 9>& m, const auto& v) {
                return std::array<double, 3>{m[0] * v[0] + m[3] * v[1] + m[6] * v[2],
                                             m[1] * v[0] + m[4] * v[1] + m[7] * v[2],
                                             m[2] * v[0] + m[5] * v[1] + m[8] * v[2]};
            };
            const auto store_unit = [](const std::array<double, 3>& v, float* out,
                                       std::string_view what) -> json::Result<void> {
                const auto length = std::hypot(v[0], v[1], v[2]);
                ATELIER_TRY(require(length > 0 && std::isfinite(length), what));
                for (std::size_t axis = 0; axis < 3; ++axis)
                    out[axis] = static_cast<float>(v[axis] / length);
                return {};
            };
            ATELIER_TRY_BIND(primitive_items, primitives(index));
            for (auto primitive : *primitive_items) {
                ATELIER_TRY_BIND(material_value, primitive.find("material"));
                ATELIER_TRY_BIND(material_index,
                                 *material_value ? integer(**material_value) : json::Result<std::uint64_t>{0});
                const auto slot = *material_value ? slot_of[*material_index] : default_slot;
                if (slot == no_slot)
                    continue; // BLEND, reported in notes.
                ATELIER_TRY_BIND(attributes, primitive.at("attributes"));
                ATELIER_TRY_BIND(position_index, attributes->at("POSITION"));
                ATELIER_TRY_BIND(position, accessor(gltf, indexed, buffers, *position_index, "VEC3", {5126}));
                const auto optional = [&](const char* name,
                                          std::string_view type) -> json::Result<std::optional<Accessor>> {
                    std::optional<Accessor> a;
                    ATELIER_TRY_BIND(value, attributes->find(name));
                    if (*value) {
                        ATELIER_TRY_BIND(found, accessor(gltf, indexed, buffers, **value, type, {5126}));
                        a = *found;
                        ATELIER_TRY(require(a->count == position->count, "vertex attributes differ in count."));
                    }
                    return a;
                };
                ATELIER_TRY_BIND(normal, optional("NORMAL", "VEC3"));
                ATELIER_TRY_BIND(tangent, optional("TANGENT", "VEC4"));
                ATELIER_TRY_BIND(uv, optional("TEXCOORD_0", "VEC2"));
                const auto textured =
                    std::ranges::any_of(result.textures[slot], [](const auto& path) { return !path.empty(); });
                ATELIER_TRY(require(*uv || !textured, "a textured primitive has no TEXCOORD_0."));
                std::vector<geometry::Vertex> source(position->count);
                for (std::size_t v = 0; v < position->count; ++v) {
                    auto& out = source[v];
                    ATELIER_TRY_BIND(p, floats_at<3>(*position, v));
                    for (std::size_t axis = 0; axis < 3; ++axis)
                        out.position[axis] = static_cast<float>(world[axis] * (*p)[0] + world[4 + axis] * (*p)[1] +
                                                                world[8 + axis] * (*p)[2] + world[12 + axis]);
                    if (*normal) {
                        ATELIER_TRY_BIND(n, floats_at<3>(**normal, v));
                        ATELIER_TRY(store_unit(apply(cofactor, *n), out.normal, "a normal is zero."));
                    }
                    if (*tangent) {
                        ATELIER_TRY_BIND(t, floats_at<4>(**tangent, v));
                        const auto mapped = apply(linear, *t);
                        const auto length = std::hypot(mapped[0], mapped[1], mapped[2]);
                        for (std::size_t axis = 0; axis < 3; ++axis)
                            out.tangent[axis] = length > 0 ? static_cast<float>(mapped[axis] / length) : 0.f;
                        // The bitangent cross(N,T)*w must follow the transformed surface, so a mirror flips w.
                        out.tangent[3] = ((*t)[3] < 0) == (det < 0) ? 1.f : -1.f;
                    }
                    if (*uv) {
                        ATELIER_TRY_BIND(t, floats_at<2>(**uv, v));
                        out.uv[0] = static_cast<float>((*t)[0]);
                        out.uv[1] = static_cast<float>((*t)[1]);
                    }
                }
                std::optional<Accessor> indices;
                ATELIER_TRY_BIND(indices_value, primitive.find("indices"));
                if (*indices_value) {
                    ATELIER_TRY_BIND(found,
                                     accessor(gltf, indexed, buffers, **indices_value, "SCALAR", {5121, 5123, 5125}));
                    indices = *found;
                }
                const auto count = indices ? indices->count : position->count;
                ATELIER_TRY(require(count % 3 == 0, "a triangle list has a partial triangle."));
                const auto first = vertices.size();
                for (std::size_t c = 0; c < count; c += 3) {
                    std::array<geometry::Vertex, 3> triangle;
                    for (std::size_t k = 0; k < 3; ++k) {
                        const auto i = indices ? index_at(*indices, c + k) : static_cast<std::uint32_t>(c + k);
                        ATELIER_TRY(require(i < position->count, "an index exceeds its vertex count."));
                        triangle[k] = source[i];
                    }
                    if (!*normal) {
                        // glTF fronts are counter-clockwise before the node transform; sign(det) restores them after it.
                        std::array<double, 3> a{};
                        std::array<double, 3> b{};
                        for (std::size_t axis = 0; axis < 3; ++axis) {
                            a[axis] = double(triangle[1].position[axis]) - triangle[0].position[axis];
                            b[axis] = double(triangle[2].position[axis]) - triangle[0].position[axis];
                        }
                        float flat[3];
                        ATELIER_TRY(store_unit({(a[1] * b[2] - a[2] * b[1]) * sign, (a[2] * b[0] - a[0] * b[2]) * sign,
                                                (a[0] * b[1] - a[1] * b[0]) * sign},
                                               flat, "a triangle without normals is degenerate."));
                        for (auto& v : triangle)
                            std::ranges::copy(flat, v.normal);
                    }
                    vertices.insert(vertices.end(), triangle.begin(), triangle.end());
                }
                if (!*tangent) {
                    // Mikk takes handedness from the UV winding and assumes normals follow the triangle
                    // winding, which after the conversion holds only when det>0; there its bitangent
                    // points toward +V, while glTF normal maps put +Y toward -V.
                    const auto generated = std::span(vertices).subspan(first);
                    geometry::generate_tangents(generated);
                    if (det > 0)
                        for (auto& v : generated)
                            v.tangent[3] = -v.tangent[3];
                }
                parts.push_back({static_cast<std::uint32_t>(first), static_cast<std::uint32_t>(count),
                                 static_cast<std::uint32_t>(slot)});
            }
        }
        ATELIER_TRY(require(!vertices.empty(), "the scene has no importable triangles."));
        result.mesh =
            std::make_shared<const geometry::Mesh>(std::move(vertices), std::vector<std::uint8_t>{}, std::move(slots),
                                                   std::move(parts), geometry::Tangents::authored);
        return {};
    }();
    if (!imported)
        return std::unexpected(Error{ErrorCode::invalid_storage, "glTF: " + imported.error().describe()});
    return result;
}

bool is_mesh_path(const std::filesystem::path& path) {
    return path.extension() == L".obj" || path.extension() == L".gltf";
}

Result<MeshImport> load_mesh(const std::filesystem::path& source, ImageCache* images) {
    if (source.extension() == L".gltf")
        return load_gltf(source, images);
    if (source.extension() == L".obj")
        return load_obj(source, images);
    throw std::runtime_error("Mesh sources are Wavefront OBJ or glTF 2.0 (.gltf).");
}
}
