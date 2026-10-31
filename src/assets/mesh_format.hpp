#pragma once
#include "assets/assets.hpp"
#include "assets/file.hpp"
#include "foundation/string.hpp"
#include <bit>
#include <map>

namespace atelier::assets::mesh_format {
inline constexpr std::uint32_t version = 1;
inline constexpr std::size_t maximum_bytes = 256 * 1024 * 1024;
inline constexpr std::size_t maximum_text_bytes = 4096;

namespace detail {
inline void text(std::string_view value) {
    if (value.size() > maximum_text_bytes || value.find('\0') != value.npos || !StringView(value).is_valid_utf8())
        throw std::runtime_error("Mesh snapshot text requires valid UTF-8, no NUL, and at most 4096 bytes.");
}
inline void key(std::string_view value) {
    text(value);
    if (value.empty() || value.find_first_of("\\:\"<>|?*") != value.npos ||
        std::ranges::any_of(value, [](unsigned char c) { return c < 32 || c == 127; }))
        throw std::runtime_error("Mesh snapshot requires a nonempty relative asset key.");
    while (!value.empty()) {
        const auto slash = value.find('/');
        const auto part = value.substr(0, slash);
        if (part.empty() || part == "." || part == "..")
            throw std::runtime_error("Mesh snapshot asset key has an invalid path segment.");
        if (slash == value.npos)
            return;
        value.remove_prefix(slash + 1);
    }
    throw std::runtime_error("Mesh snapshot asset key cannot end with a slash.");
}
inline void bindings(const MeshAsset& asset) {
    std::map<std::string_view, unsigned, std::less<>> matches;
    for (const auto& binding : asset.materials) {
        text(binding.slot);
        key(binding.key);
        if (binding.slot.empty() || !matches.emplace(binding.slot, 0).second)
            throw std::runtime_error("Mesh snapshot has an empty or duplicate default material slot.");
    }
    if (matches.empty())
        return;
    for (std::size_t i = 0; i < asset.mesh->materials().size(); ++i) {
        const auto at = matches.find(geometry::material_slot_name(*asset.mesh, i));
        if (at != matches.end() && ++at->second > 1)
            throw std::runtime_error("Mesh snapshot default names an ambiguous material slot.");
    }
    for (const auto& [slot, count] : matches) {
        (void)slot;
        if (count != 1)
            throw std::runtime_error("Mesh snapshot default names a missing material slot.");
    }
}
inline void add_size(std::size_t& size, std::size_t count, std::size_t stride, std::size_t maximum) {
    if (count > (maximum - size) / stride)
        throw std::length_error("Mesh snapshot exceeds its byte limit (at most 256 MiB).");
    size += count * stride;
}
template <class T>
inline void array_size(std::size_t& size, std::size_t count, std::size_t stride, std::size_t maximum) {
    if (count > maximum_bytes / sizeof(T))
        throw std::length_error("Mesh snapshot array exceeds its decoded storage limit.");
    add_size(size, 1, 4, maximum);
    add_size(size, count, stride, maximum);
}
inline void text_size(std::size_t& size, std::string_view value, std::size_t maximum) {
    text(value);
    add_size(size, 1, 4, maximum);
    add_size(size, value.size(), 1, maximum);
}
class Writer {
public:
    explicit Writer(std::size_t size) { bytes_.reserve(size); }
    void u8(std::uint8_t value) { bytes_.push_back(static_cast<char>(value)); }
    void u32(std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) {
            u8(static_cast<std::uint8_t>(value));
            value >>= 8;
        }
    }
    void count(std::size_t value) { u32(static_cast<std::uint32_t>(value)); }
    void floats(std::span<const float> values) {
        for (float value : values)
            u32(std::bit_cast<std::uint32_t>(value));
    }
    void text(std::string_view value) {
        count(value.size());
        bytes_.append(value);
    }
    std::string finish() { return std::move(bytes_); }

private:
    std::string bytes_;
};
class Reader {
public:
    explicit Reader(std::string_view bytes) : bytes_(bytes) {
        if (bytes.size() > maximum_bytes)
            throw std::length_error("Mesh snapshot exceeds the 256 MiB byte limit.");
    }
    std::uint8_t u8() {
        if (bytes_.empty())
            throw std::runtime_error("Truncated mesh snapshot.");
        const auto value = static_cast<std::uint8_t>(bytes_.front());
        bytes_.remove_prefix(1);
        return value;
    }
    std::uint32_t u32() {
        std::uint32_t result{};
        for (unsigned i = 0; i < 4; ++i)
            result |= std::uint32_t(u8()) << (i * 8);
        return result;
    }
    template <class T>
    std::size_t count(std::size_t stride) {
        const auto size = u32();
        // Validate wire length and storage before any reserve/resize. In particular,
        // a tiny corrupt count cannot cause an unbounded metadata allocation.
        if (size > bytes_.size() / stride || size > maximum_bytes / sizeof(T))
            throw std::runtime_error("Mesh snapshot array length exceeds the remaining bytes or storage limit.");
        return size;
    }
    void floats(std::span<float> values) {
        for (float& value : values) {
            value = std::bit_cast<float>(u32());
            if (!std::isfinite(value))
                throw std::runtime_error("Mesh snapshot requires finite published values.");
        }
    }
    std::string text() {
        const auto size = u32();
        if (size > maximum_text_bytes || size > bytes_.size())
            throw std::runtime_error("Mesh snapshot text length is invalid.");
        const auto value = bytes_.substr(0, size);
        detail::text(value);
        bytes_.remove_prefix(size);
        return std::string(value);
    }
    void finish() const {
        if (!bytes_.empty())
            throw std::runtime_error("Mesh snapshot contains trailing bytes.");
    }

private:
    std::string_view bytes_;
};
inline constexpr std::uint32_t magic = 0x534d5441; // ATMS, little endian.
}

// A self-contained publication, never a source path to reload. All integers and
// IEEE float bits are written explicitly in little endian, without struct padding.
// Layout: magic, version, key/name text, vertices (12 floats each), edge masks,
// slot names, parts (first/count/material), defaults (slot/key text). Every text
// and array has a uint32 count; bounds are derived again from unchanged positions.
// Both functions throw on malformed/oversized content; no project reads or roots.
// maximum can lower the encoder's limit to the caller's remaining spill budget.
inline std::string encode(const MeshAsset& asset, std::size_t maximum = maximum_bytes) {
    if (!asset.mesh)
        throw std::runtime_error("Cannot encode an unresolved mesh snapshot.");
    maximum = std::min(maximum, maximum_bytes);
    detail::key(asset.key);
    if (asset.name.size() > maximum_text_bytes)
        throw std::length_error("Mesh snapshot name is too long.");
    const auto name = assets::utf8(asset.name);
    const auto& mesh = *asset.mesh;
    std::size_t size{};
    detail::add_size(size, 2, 4, maximum);
    detail::text_size(size, asset.key, maximum);
    detail::text_size(size, name, maximum);
    detail::array_size<geometry::Vertex>(size, mesh.vertices().size(), 12 * 4, maximum);
    detail::array_size<std::uint8_t>(size, mesh.vertices().size() / 3, 1, maximum);
    detail::array_size<geometry::MaterialSlot>(size, mesh.materials().size(), 4, maximum);
    for (const auto& slot : mesh.materials()) {
        detail::text(slot.name);
        detail::add_size(size, slot.name.size(), 1, maximum);
    }
    detail::array_size<geometry::MeshPart>(size, mesh.parts().size(), 3 * 4, maximum);
    detail::array_size<SlotMaterial>(size, asset.materials.size(), 2 * 4, maximum);
    for (const auto& binding : asset.materials) {
        detail::text(binding.slot);
        detail::text(binding.key);
        detail::add_size(size, binding.slot.size(), 1, maximum);
        detail::add_size(size, binding.key.size(), 1, maximum);
    }
    if (asset.materials.size() > mesh.materials().size())
        throw std::runtime_error("Mesh snapshot has too many default materials.");
    detail::bindings(asset);
    detail::Writer out(size);
    out.u32(detail::magic);
    out.u32(version);
    out.text(asset.key);
    out.text(name);
    out.count(mesh.vertices().size());
    for (const auto& vertex : mesh.vertices()) {
        out.floats(vertex.position);
        out.floats(vertex.normal);
        out.floats(vertex.uv);
        out.floats(vertex.tangent);
    }
    out.count(mesh.vertices().size() / 3);
    for (std::size_t i = 0; i < mesh.vertices().size() / 3; ++i)
        out.u8(mesh.edge_mask(i));
    out.count(mesh.materials().size());
    for (const auto& slot : mesh.materials())
        out.text(slot.name);
    out.count(mesh.parts().size());
    for (const auto& part : mesh.parts()) {
        out.u32(part.first_vertex);
        out.u32(part.vertex_count);
        out.u32(part.material);
    }
    out.count(asset.materials.size());
    for (const auto& binding : asset.materials) {
        out.text(binding.slot);
        out.text(binding.key);
    }
    return out.finish();
}
inline MeshAsset decode(std::string_view bytes) {
    detail::Reader in(bytes);
    if (in.u32() != detail::magic || in.u32() != version)
        throw std::runtime_error("Unsupported mesh snapshot format or version.");
    MeshAsset asset;
    asset.key = in.text();
    detail::key(asset.key);
    asset.name = assets::wide(in.text());
    const auto vertex_count = in.count<geometry::Vertex>(12 * 4);
    if (!vertex_count || vertex_count % 3 || vertex_count > std::numeric_limits<int>::max())
        throw std::runtime_error("Mesh snapshot requires a nonempty triangle list.");
    std::vector<geometry::Vertex> vertices(vertex_count);
    for (auto& vertex : vertices) {
        in.floats(vertex.position);
        in.floats(vertex.normal);
        in.floats(vertex.uv);
        in.floats(vertex.tangent);
    }
    const auto mask_count = in.count<std::uint8_t>(1);
    if (mask_count != vertex_count / 3)
        throw std::runtime_error("Mesh snapshot requires one edge mask per triangle.");
    std::vector<std::uint8_t> masks(mask_count);
    for (auto& mask : masks)
        mask = in.u8();
    const auto slot_count = in.count<geometry::MaterialSlot>(4);
    if (!slot_count)
        throw std::runtime_error("Mesh snapshot requires material slots.");
    std::vector<geometry::MaterialSlot> slots;
    slots.reserve(slot_count);
    for (std::size_t i = 0; i < slot_count; ++i)
        slots.push_back({in.text()});
    const auto part_count = in.count<geometry::MeshPart>(3 * 4);
    if (!part_count || part_count > vertex_count / 3)
        throw std::runtime_error("Mesh snapshot has an invalid part count.");
    std::vector<geometry::MeshPart> parts;
    parts.reserve(part_count);
    for (std::size_t i = 0; i < part_count; ++i)
        parts.push_back({in.u32(), in.u32(), in.u32()});
    const auto default_count = in.count<SlotMaterial>(2 * 4);
    if (default_count > slot_count)
        throw std::runtime_error("Mesh snapshot has too many default materials.");
    asset.materials.reserve(default_count);
    for (std::size_t i = 0; i < default_count; ++i)
        asset.materials.push_back({in.text(), in.text()});
    in.finish();
    asset.mesh =
        geometry::Mesh::restore_published(std::move(vertices), std::move(masks), std::move(slots), std::move(parts));
    detail::bindings(asset);
    return asset;
}
}
