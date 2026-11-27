#include "assets/mesh_import.hpp"
#include "assets/file.hpp"
#include "assets/image_decode.hpp"
#include "foundation/string.hpp"
#include "platform/error.hpp"
#include "platform/utf.hpp"
#include <charconv>
#include <sstream>
#include <unordered_map>

namespace atelier::assets {
namespace {
Result<void> require(bool ok, const char* message) {
    if (!ok)
        return std::unexpected(Error{ErrorCode::invalid_storage, message});
    return {};
}

std::string rest(std::istringstream& row) {
    std::string value;
    std::getline(row >> std::ws, value);
    const auto end = value.find_last_not_of(" \t\r");
    if (end == value.npos)
        return {};
    value.resize(end + 1);
    return value;
}

Result<std::filesystem::path> relative_path(std::wstring value) {
    std::ranges::replace(value, L'\\', L'/');
    if (value.size() > 1 && value.front() == L'"' && value.back() == L'"')
        value = value.substr(1, value.size() - 2);
    auto path = std::filesystem::path(std::move(value));
    ATELIER_TRY(require(!path.empty() && !path.has_root_path(), "OBJ dependency must be relative."));
    for (const auto& part : path)
        ATELIER_TRY(require(part != L".." && part.wstring().find(L':') == std::wstring::npos,
                            "OBJ dependency must stay inside the source directory."));
    return path.lexically_normal();
}

Result<std::size_t> index(std::string_view text, std::size_t count) {
    std::int64_t value{};
    auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    ATELIER_TRY(require(ec == std::errc{} && end == text.data() + text.size() && value != 0, "Invalid OBJ index."));
    value = value > 0 ? value - 1 : static_cast<std::int64_t>(count) + value;
    ATELIER_TRY(require(value >= 0 && std::uint64_t(value) < count, "OBJ index is out of range."));
    return static_cast<std::size_t>(value);
}

Result<float> scalar(std::istringstream& row) {
    float v{};
    ATELIER_TRY(require(bool(row >> v) && std::isfinite(v), "Invalid OBJ/MTL number."));
    return v;
}

Result<std::vector<std::array<unsigned, 3>>> triangulate(const std::vector<geometry::Vertex>& face) {
    if (face.size() == 3)
        return std::vector<std::array<unsigned, 3>>{{0, 1, 2}};
    std::array<double, 3> normal{};
    for (std::size_t i = 0; i < face.size(); ++i) {
        const auto& a = face[i].position;
        const auto& b = face[(i + 1) % face.size()].position;
        normal[0] += (double(a[1]) - b[1]) * (double(a[2]) + b[2]);
        normal[1] += (double(a[2]) - b[2]) * (double(a[0]) + b[0]);
        normal[2] += (double(a[0]) - b[0]) * (double(a[1]) + b[1]);
    }
    unsigned axis = 0;
    for (unsigned i = 1; i < 3; ++i)
        if (std::abs(normal[i]) > std::abs(normal[axis]))
            axis = i;
    const unsigned x = (axis + 1) % 3;
    const unsigned y = (axis + 2) % 3;
    const double sign = normal[axis] < 0 ? -1 : 1;
    const auto cross = [&](unsigned a, unsigned b, unsigned c) {
        return sign * ((double(face[b].position[x]) - face[a].position[x]) *
                           (double(face[c].position[y]) - face[a].position[y]) -
                       (double(face[b].position[y]) - face[a].position[y]) *
                           (double(face[c].position[x]) - face[a].position[x]));
    };
    std::vector<unsigned> remaining;
    for (unsigned i = 0; i < face.size(); ++i)
        remaining.push_back(i);
    std::vector<std::array<unsigned, 3>> triangles;
    while (remaining.size() > 3) {
        bool found = false;
        for (std::size_t i = 0; i < remaining.size(); ++i) {
            const auto a = remaining[(i + remaining.size() - 1) % remaining.size()];
            const auto b = remaining[i];
            const auto c = remaining[(i + 1) % remaining.size()];
            if (cross(a, b, c) < 0)
                continue;
            bool inside = false;
            for (auto p : remaining)
                if (p != a && p != b && p != c && cross(a, b, c) > 0 && cross(a, b, p) >= 0 && cross(b, c, p) >= 0 &&
                    cross(c, a, p) >= 0) {
                    inside = true;
                    break;
                }
            if (inside)
                continue;
            triangles.push_back({a, b, c});
            remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(i));
            found = true;
            break;
        }
        ATELIER_TRY(require(found, "OBJ polygon cannot be triangulated."));
    }
    triangles.push_back({remaining[0], remaining[1], remaining[2]});
    return triangles;
}
}

Result<MeshImport> load_obj(const std::filesystem::path& source, ImageCache* shared_images) {
    MeshImport result;
    std::error_code error;
    const auto root = std::filesystem::canonical(source.parent_path(), error);
    if (error)
        return std::unexpected(Error{ErrorCode::invalid_storage, platform::error_message(error)});
    std::unordered_map<std::string, std::size_t> files;
    std::unordered_map<std::string, material::ImageRef> images;
    std::size_t total_bytes{};
    std::size_t image_bytes{};
    const auto read = [&](const std::filesystem::path& relative) -> Result<std::size_t> {
        const auto key = platform::utf16_to_utf8(relative.generic_wstring());
        if (!key)
            return std::unexpected(Error{ErrorCode::invalid_storage, "Invalid UTF-16 text."});
        if (const auto found = files.find(*key); found != files.end())
            return found->second;
        ATELIER_TRY(require(files.size() < 512, "OBJ has too many dependencies."));
        const auto path = std::filesystem::weakly_canonical(root / relative, error);
        if (error)
            return std::unexpected(Error{ErrorCode::invalid_storage, platform::error_message(error)});
        const auto within = path.lexically_relative(root);
        ATELIER_TRY(require(!within.empty() && !within.has_root_path() && *within.begin() != L"..",
                            "OBJ dependency escapes through a link."));
        ATELIER_TRY_BIND(bytes, read_file(path, 128 * 1024 * 1024));
        ATELIER_TRY(
            require(total_bytes + bytes->size() <= 512ull * 1024 * 1024, "OBJ dependency bytes exceed 512 MiB."));
        total_bytes += bytes->size();
        const auto slot = result.files.size();
        files.emplace(*key, slot);
        result.files.push_back({relative, std::move(*bytes)});
        return slot;
    };
    const auto image = [&](const std::filesystem::path& relative) -> Result<material::ImageRef> {
        const auto key = platform::utf16_to_utf8(relative.generic_wstring());
        if (!key)
            return std::unexpected(Error{ErrorCode::invalid_storage, "Invalid UTF-16 text."});
        if (const auto found = images.find(*key); found != images.end())
            return found->second;
        ATELIER_TRY_BIND(slot, read(relative));
        const std::string_view bytes = result.files[*slot].bytes;
        ATELIER_TRY_BIND(decoded, decode_images(std::span(&bytes, 1), shared_images));
        auto loaded = decoded->front();
        ATELIER_TRY(
            require(image_bytes + loaded->byte_size() <= 768ull * 1024 * 1024, "Decoded OBJ textures exceed 768 MiB."));
        image_bytes += loaded->byte_size();
        images.emplace(*key, loaded);
        result.decoded_textures.push_back({result.files[*slot].relative, loaded});
        return loaded;
    };
    struct AuthorMaterial {
        std::array<float, 4> color{1, 1, 1, 1};

        bool color_alpha{};
    };
    std::vector<geometry::MaterialSlot> materials(1);
    std::vector<AuthorMaterial> authors(1);
    result.materials.emplace_back();
    result.textures.emplace_back();
    std::unordered_map<std::string, std::uint32_t> material_names;
    const auto mtl = [&](const std::filesystem::path& relative) -> Result<void> {
        ATELIER_TRY_BIND(slot, read(relative));
        std::istringstream input(result.files[*slot].bytes);
        input.imbue(std::locale::classic());
        std::string line;
        std::string kind;
        AuthorMaterial* current = nullptr;
        std::size_t current_slot{};
        while (std::getline(input, line)) {
            if (auto p = line.find('#'); p != line.npos)
                line.resize(p);
            std::istringstream row(line);
            row.imbue(std::locale::classic());
            if (!(row >> kind))
                continue;
            if (kind == "newmtl") {
                auto name = rest(row);
                ATELIER_TRY(
                    require(!name.empty() && material_names.size() < 1024, "Invalid or excessive MTL material names."));
                if (!StringView(name).is_valid_utf8())
                    return std::unexpected(Error{ErrorCode::invalid_storage, "Invalid UTF-8 MTL material name."});
                ATELIER_TRY(require(material_names.emplace(name, static_cast<std::uint32_t>(materials.size())).second,
                                    "Duplicate MTL material name."));
                current_slot = materials.size();
                materials.push_back({std::move(name)});
                authors.emplace_back();
                current = &authors.back();
                result.materials.emplace_back(MaterialInstanceAsset{});
                result.textures.emplace_back();
            } else {
                ATELIER_TRY(require(current != nullptr, "MTL property precedes newmtl."));
                if (kind == "Kd")
                    for (unsigned i = 0; i < 3; ++i) {
                        ATELIER_TRY_BIND(value, scalar(row));
                        current->color[i] = *value;
                    }
                else if (kind == "d" || kind == "Tr") {
                    ATELIER_TRY_BIND(v, scalar(row));
                    current->color[3] = kind == "d" ? *v : 1 - *v;
                } else if (kind.starts_with("map_") || kind == "bump") {
                    auto name = rest(row);
                    ATELIER_TRY(require(!name.empty() && name.front() != '-', "MTL map options are not supported."));
                    auto dependency = platform::utf8_to_utf16(name);
                    if (!dependency)
                        return std::unexpected(Error{ErrorCode::invalid_storage, "Invalid UTF-8 text."});
                    ATELIER_TRY_BIND(relative_dependency, relative_path(std::move(*dependency)));
                    const auto path = relative.parent_path() / *relative_dependency;
                    if (kind == "map_Kd") {
                        ATELIER_TRY_BIND(loaded, image(path));
                        current->color_alpha = (*loaded)->has_alpha();
                        result.textures[current_slot][0] = path;
                    } else if (kind == "map_d") {
                        ATELIER_TRY(image(path));
                        result.textures[current_slot][5] = path;
                    } else
                        ATELIER_TRY(read(path)); // Preserve unrendered source maps during import.
                } else if (kind != "Ka" && kind != "Ks" && kind != "Ke" && kind != "Ns" && kind != "Ni" &&
                           kind != "Tf" && kind != "illum")
                    return std::unexpected(Error{ErrorCode::invalid_storage, "Unsupported MTL record: " + kind});
            }
        }
        return {};
    };
    ATELIER_TRY_BIND(source_path, relative_path(source.filename().native()));
    ATELIER_TRY_BIND(source_slot, read(*source_path));
    std::istringstream input(result.files[*source_slot].bytes);
    input.imbue(std::locale::classic());
    std::vector<std::array<float, 3>> positions;
    std::vector<std::array<float, 3>> normals;
    std::vector<std::array<float, 2>> uv;
    std::vector<geometry::Vertex> vertices;
    std::vector<std::uint8_t> masks;
    std::vector<geometry::MeshPart> parts;
    std::uint32_t material{};
    std::string line;
    std::string kind;
    while (std::getline(input, line)) {
        if (auto p = line.find('#'); p != line.npos)
            line.resize(p);
        std::istringstream row(line);
        row.imbue(std::locale::classic());
        if (!(row >> kind))
            continue;
        if (kind == "v" || kind == "vn") {
            auto& target = kind == "v" ? positions : normals;
            ATELIER_TRY(require(target.size() < 1000000, "OBJ exceeds the vertex limit."));
            ATELIER_TRY_BIND(x, scalar(row));
            ATELIER_TRY_BIND(y, scalar(row));
            ATELIER_TRY_BIND(z, scalar(row));
            target.push_back({*x, *y, *z});
        } else if (kind == "vt") {
            ATELIER_TRY(require(uv.size() < 1000000, "OBJ exceeds the UV limit."));
            ATELIER_TRY_BIND(u, scalar(row));
            ATELIER_TRY_BIND(v, scalar(row));
            uv.push_back({*u, 1 - *v});
        } else if (kind == "mtllib") {
            auto path = platform::utf8_to_utf16(rest(row));
            if (!path)
                return std::unexpected(Error{ErrorCode::invalid_storage, "Invalid UTF-8 text."});
            ATELIER_TRY_BIND(relative, relative_path(std::move(*path)));
            ATELIER_TRY(mtl(*relative));
        } else if (kind == "usemtl") {
            const auto found = material_names.find(rest(row));
            ATELIER_TRY(require(found != material_names.end(), "OBJ references an unknown material."));
            material = found->second;
        } else if (kind == "f") {
            std::vector<geometry::Vertex> face;
            std::vector<bool> has_normal;
            std::string token;
            while (row >> token) {
                ATELIER_TRY(require(face.size() < 64, "OBJ polygon exceeds 64 corners."));
                geometry::Vertex vertex{};
                const auto a = token.find('/');
                const auto b = a == token.npos ? token.npos : token.find('/', a + 1);
                ATELIER_TRY_BIND(position, index(std::string_view(token).substr(0, a), positions.size()));
                std::ranges::copy(positions[*position], vertex.position);
                if (a != token.npos && a + 1 < (b == token.npos ? token.size() : b)) {
                    ATELIER_TRY_BIND(
                        texcoord, index(std::string_view(token).substr(a + 1, b == token.npos ? token.npos : b - a - 1),
                                        uv.size()));
                    std::ranges::copy(uv[*texcoord], vertex.uv);
                } else {
                    const auto valid = require(
                        std::ranges::all_of(result.textures[material], [](const auto& path) { return path.empty(); }),
                        "Textured OBJ face requires UV indices.");
                    if (!valid)
                        return std::unexpected(valid.error());
                }
                const bool normal = b != token.npos && b + 1 < token.size();
                if (normal) {
                    ATELIER_TRY_BIND(direction, index(std::string_view(token).substr(b + 1), normals.size()));
                    std::ranges::copy(normals[*direction], vertex.normal);
                }
                face.push_back(vertex);
                has_normal.push_back(normal);
            }
            ATELIER_TRY(require(face.size() >= 3, "OBJ face requires at least three corners."));
            ATELIER_TRY_BIND(triangles, triangulate(face));
            ATELIER_TRY(require(vertices.size() + triangles->size() * 3 <= 3000000, "OBJ exceeds the triangle limit."));
            if (parts.empty() || parts.back().material != material)
                parts.push_back({static_cast<std::uint32_t>(vertices.size()), 0, material});
            for (const auto& t : *triangles) {
                std::array<geometry::Vertex, 3> triangle{face[t[0]], face[t[1]], face[t[2]]};
                std::array<double, 3> a{};
                std::array<double, 3> b{};
                std::array<double, 3> n{};
                for (unsigned i = 0; i < 3; ++i) {
                    a[i] = double(triangle[1].position[i]) - triangle[0].position[i];
                    b[i] = double(triangle[2].position[i]) - triangle[0].position[i];
                }
                n = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
                const auto length = std::hypot(n[0], n[1], n[2]);
                std::uint8_t mask = 0;
                for (unsigned i = 0; i < 3; ++i) {
                    if (!has_normal[t[i]]) {
                        ATELIER_TRY(require(length > 0, "Degenerate OBJ face needs explicit normals."));
                        for (unsigned j = 0; j < 3; ++j)
                            triangle[i].normal[j] = static_cast<float>(n[j] / length);
                    }
                    const auto j = t[(i + 1) % 3];
                    const auto k = t[(i + 2) % 3];
                    if ((j + 1) % face.size() == k || (k + 1) % face.size() == j)
                        mask |= static_cast<std::uint8_t>(1u << i);
                }
                vertices.insert(vertices.end(), triangle.begin(), triangle.end());
                masks.push_back(mask);
                parts.back().vertex_count += 3;
            }
        } else if (kind != "o" && kind != "g" && kind != "s")
            return std::unexpected(Error{ErrorCode::invalid_storage, "Unsupported OBJ record: " + kind});
    }
    for (std::size_t i = 1; i < materials.size(); ++i) {
        const auto& value = authors[i];
        const auto& color = value.color;
        auto& instance = *result.materials[i];
        const auto valid = require(std::ranges::all_of(color, [](float v) { return v >= 0 && v <= 1; }),
                                   "Invalid MTL base color or opacity.");
        if (!valid)
            return std::unexpected(valid.error());
        const bool alpha = color[3] < 1 || value.color_alpha || !result.textures[i][5].empty();
        instance.parent_key =
            alpha ? "builtin/material/standard-mask-double" : "builtin/material/standard-opaque-double";
        // MTL's supported appearance is authored data, including explicit white.
        // Keep the import's established PBR defaults independently of the mesh.
        instance.entries = {{"base_color", math::Vector4{color[0], color[1], color[2], color[3]}},
                            {"roughness", .65f},
                            {"metallic", 0.f},
                            {"normal_scale", 1.f},
                            {"normal_y_sign", 1.f},
                            {"emissive", math::Vector3{}},
                            {"alpha_cutoff", .5f}};
        for (std::size_t input = 0; input < imported_material_inputs.size(); ++input)
            if (!result.textures[i][input].empty()) {
                const auto name = std::string(imported_material_inputs[input]);
                instance.entries.push_back(
                    {name + "_texture", material::TextureBinding{{}, material::Address::repeat}});
                instance.entries.push_back({name + "_uv.scale", math::Vector2{1, 1}});
                instance.entries.push_back({name + "_uv.offset", math::Vector2{}});
                instance.entries.push_back({name + "_uv.rotation", 0.f});
                if (input == 5)
                    instance.entries.push_back({"opacity_channel", 0.f});
            }
    }
    auto mesh = geometry::Mesh::create(std::move(vertices), std::move(masks), std::move(materials), std::move(parts));
    if (!mesh)
        return std::unexpected(Error{ErrorCode::invalid_storage, std::move(mesh.error().message)});
    result.mesh = std::move(*mesh);
    return result;
}
}
