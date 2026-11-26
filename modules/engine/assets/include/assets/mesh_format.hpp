#pragma once
#include "assets/assets.hpp"
#include "assets/file.hpp"
#include "foundation/map.hpp"
#include "foundation/string.hpp"
#include "platform/utf.hpp"
#include <bit>

namespace atelier::assets::mesh_format {
inline constexpr std::uint32_t version = 1;

inline constexpr std::size_t maximum_bytes = 256 * 1024 * 1024;

inline constexpr std::size_t maximum_text_bytes = 4096;

namespace detail {
inline Result<void> text(std::string_view value) {
    if (value.size() > maximum_text_bytes || value.find('\0') != value.npos || !StringView(value).is_valid_utf8())
        return std::unexpected(Error{ErrorCode::invalid_storage,
                                     "Mesh snapshot text requires valid UTF-8, no NUL, and at most 4096 bytes."});
    return {};
}

inline Result<void> key(std::string_view value) {
    ATELIER_TRY(text(value));
    if (value.empty() || value.find_first_of("\\:\"<>|?*") != value.npos ||
        std::ranges::any_of(value, [](unsigned char c) { return c < 32 || c == 127; }))
        return std::unexpected(
            Error{ErrorCode::invalid_storage, "Mesh snapshot requires a nonempty relative asset key."});
    while (!value.empty()) {
        const auto slash = value.find('/');
        const auto part = value.substr(0, slash);
        if (part.empty() || part == "." || part == "..")
            return std::unexpected(
                Error{ErrorCode::invalid_storage, "Mesh snapshot asset key has an invalid path segment."});
        if (slash == value.npos)
            return {};
        value.remove_prefix(slash + 1);
    }
    return std::unexpected(Error{ErrorCode::invalid_storage, "Mesh snapshot asset key cannot end with a slash."});
}

inline Result<void> bindings(const MeshAsset& asset) {
    Map<std::string_view, unsigned> matches;
    matches.reserve(asset.materials.size());
    for (const auto& binding : asset.materials) {
        ATELIER_TRY(text(binding.slot));
        ATELIER_TRY(key(binding.key));
        if (binding.slot.empty() || !matches.try_emplace(binding.slot, 0u).inserted)
            return std::unexpected(
                Error{ErrorCode::invalid_storage, "Mesh snapshot has an empty or duplicate default material slot."});
    }
    if (matches.empty())
        return {};
    for (std::size_t i = 0; i < asset.mesh->materials().size(); ++i) {
        if (auto* count = matches.find(geometry::material_slot_name(*asset.mesh, i)); count && ++*count > 1)
            return std::unexpected(
                Error{ErrorCode::invalid_storage, "Mesh snapshot default names an ambiguous material slot."});
    }
    for (const auto [slot, count] : matches) {
        (void)slot;
        if (count != 1)
            return std::unexpected(
                Error{ErrorCode::invalid_storage, "Mesh snapshot default names a missing material slot."});
    }
    return {};
}

inline Result<void> add_size(std::size_t& size, std::size_t count, std::size_t stride, std::size_t maximum) {
    if (count > (maximum - size) / stride)
        return std::unexpected(
            Error{ErrorCode::invalid_storage, "Mesh snapshot exceeds its byte limit (at most 256 MiB)."});
    size += count * stride;
    return {};
}

template <class T>
inline Result<void> array_size(std::size_t& size, std::size_t count, std::size_t stride, std::size_t maximum) {
    if (count > maximum_bytes / sizeof(T))
        return std::unexpected(
            Error{ErrorCode::invalid_storage, "Mesh snapshot array exceeds its decoded storage limit."});
    ATELIER_TRY(add_size(size, 1, 4, maximum));
    return add_size(size, count, stride, maximum);
}

inline Result<void> text_size(std::size_t& size, std::string_view value, std::size_t maximum) {
    ATELIER_TRY(text(value));
    ATELIER_TRY(add_size(size, 1, 4, maximum));
    return add_size(size, value.size(), 1, maximum);
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
    explicit Reader(std::string_view bytes) : bytes_(bytes) {}

    Result<std::uint8_t> u8() {
        if (bytes_.empty())
            return std::unexpected(Error{ErrorCode::invalid_storage, "Truncated mesh snapshot."});
        const auto value = static_cast<std::uint8_t>(bytes_.front());
        bytes_.remove_prefix(1);
        return value;
    }

    Result<std::uint32_t> u32() {
        std::uint32_t result{};
        for (unsigned i = 0; i < 4; ++i) {
            ATELIER_TRY_BIND(byte, u8());
            result |= std::uint32_t(*byte) << (i * 8);
        }
        return result;
    }

    template <class T>
    Result<std::size_t> count(std::size_t stride) {
        ATELIER_TRY_BIND(size, u32());
        // Validate wire length and storage before any reserve/resize. In particular,
        // a tiny corrupt count cannot cause an unbounded metadata allocation.
        if (*size > bytes_.size() / stride || *size > maximum_bytes / sizeof(T))
            return std::unexpected(Error{ErrorCode::invalid_storage,
                                         "Mesh snapshot array length exceeds the remaining bytes or storage limit."});
        return *size;
    }

    Result<void> floats(std::span<float> values) {
        for (float& value : values) {
            ATELIER_TRY_BIND(bits, u32());
            value = std::bit_cast<float>(*bits);
            if (!std::isfinite(value))
                return std::unexpected(
                    Error{ErrorCode::invalid_storage, "Mesh snapshot requires finite published values."});
        }
        return {};
    }

    Result<std::string> text() {
        ATELIER_TRY_BIND(size, u32());
        if (*size > maximum_text_bytes || *size > bytes_.size())
            return std::unexpected(Error{ErrorCode::invalid_storage, "Mesh snapshot text length is invalid."});
        const auto value = bytes_.substr(0, *size);
        ATELIER_TRY(detail::text(value));
        bytes_.remove_prefix(*size);
        return std::string(value);
    }

    Result<void> finish() const {
        if (!bytes_.empty())
            return std::unexpected(Error{ErrorCode::invalid_storage, "Mesh snapshot contains trailing bytes."});
        return {};
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
// Malformed or oversized content returns invalid_storage; no project reads or roots.
// maximum can lower the encoder's limit to the caller's remaining spill budget.
inline Result<std::string> encode(const MeshAsset& asset, std::size_t maximum = maximum_bytes) {
    if (!asset.mesh)
        return std::unexpected(Error{ErrorCode::invalid_storage, "Cannot encode an unresolved mesh snapshot."});
    maximum = std::min(maximum, maximum_bytes);
    ATELIER_TRY(detail::key(asset.key));
    if (asset.name.size() > maximum_text_bytes)
        return std::unexpected(Error{ErrorCode::invalid_storage, "Mesh snapshot name is too long."});
    const auto name = platform::utf16_to_utf8(asset.name);
    if (!name)
        return std::unexpected(Error{ErrorCode::invalid_storage, "Invalid UTF-16 text."});
    const auto& mesh = *asset.mesh;
    std::size_t size{};
    ATELIER_TRY(detail::add_size(size, 2, 4, maximum));
    ATELIER_TRY(detail::text_size(size, asset.key, maximum));
    ATELIER_TRY(detail::text_size(size, *name, maximum));
    ATELIER_TRY(detail::array_size<geometry::Vertex>(size, mesh.vertices().size(), 12 * 4, maximum));
    ATELIER_TRY(detail::array_size<std::uint8_t>(size, mesh.vertices().size() / 3, 1, maximum));
    ATELIER_TRY(detail::array_size<geometry::MaterialSlot>(size, mesh.materials().size(), 4, maximum));
    for (const auto& slot : mesh.materials()) {
        ATELIER_TRY(detail::text(slot.name));
        ATELIER_TRY(detail::add_size(size, slot.name.size(), 1, maximum));
    }
    ATELIER_TRY(detail::array_size<geometry::MeshPart>(size, mesh.parts().size(), 3 * 4, maximum));
    ATELIER_TRY(detail::array_size<SlotMaterial>(size, asset.materials.size(), 2 * 4, maximum));
    for (const auto& binding : asset.materials) {
        ATELIER_TRY(detail::text(binding.slot));
        ATELIER_TRY(detail::text(binding.key));
        ATELIER_TRY(detail::add_size(size, binding.slot.size(), 1, maximum));
        ATELIER_TRY(detail::add_size(size, binding.key.size(), 1, maximum));
    }
    if (asset.materials.size() > mesh.materials().size())
        return std::unexpected(Error{ErrorCode::invalid_storage, "Mesh snapshot has too many default materials."});
    ATELIER_TRY(detail::bindings(asset));
    detail::Writer out(size);
    out.u32(detail::magic);
    out.u32(version);
    out.text(asset.key);
    out.text(*name);
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

inline Result<MeshAsset> decode(std::string_view bytes) {
    if (bytes.size() > maximum_bytes)
        return std::unexpected(Error{ErrorCode::invalid_storage, "Mesh snapshot exceeds the 256 MiB byte limit."});
    detail::Reader in(bytes);
    ATELIER_TRY_BIND(magic, in.u32());
    if (*magic != detail::magic)
        return std::unexpected(Error{ErrorCode::invalid_storage, "Unsupported mesh snapshot format or version."});
    ATELIER_TRY_BIND(wire_version, in.u32());
    if (*wire_version != version)
        return std::unexpected(Error{ErrorCode::invalid_storage, "Unsupported mesh snapshot format or version."});
    MeshAsset asset;
    ATELIER_TRY_BIND(key, in.text());
    asset.key = std::move(*key);
    ATELIER_TRY(detail::key(asset.key));
    ATELIER_TRY_BIND(name, in.text());
    asset.name = platform::utf8_to_utf16_validated(*name);
    ATELIER_TRY_BIND(vertex_count, in.count<geometry::Vertex>(12 * 4));
    if (!*vertex_count || *vertex_count % 3 || *vertex_count > std::numeric_limits<int>::max())
        return std::unexpected(Error{ErrorCode::invalid_storage, "Mesh snapshot requires a nonempty triangle list."});
    std::vector<geometry::Vertex> vertices(*vertex_count);
    for (auto& vertex : vertices) {
        ATELIER_TRY(in.floats(vertex.position));
        ATELIER_TRY(in.floats(vertex.normal));
        ATELIER_TRY(in.floats(vertex.uv));
        ATELIER_TRY(in.floats(vertex.tangent));
    }
    ATELIER_TRY_BIND(mask_count, in.count<std::uint8_t>(1));
    if (*mask_count != *vertex_count / 3)
        return std::unexpected(Error{ErrorCode::invalid_storage, "Mesh snapshot requires one edge mask per triangle."});
    std::vector<std::uint8_t> masks(*mask_count);
    for (auto& mask : masks) {
        ATELIER_TRY_BIND(value, in.u8());
        mask = *value;
    }
    ATELIER_TRY_BIND(slot_count, in.count<geometry::MaterialSlot>(4));
    if (!*slot_count)
        return std::unexpected(Error{ErrorCode::invalid_storage, "Mesh snapshot requires material slots."});
    std::vector<geometry::MaterialSlot> slots;
    slots.reserve(*slot_count);
    for (std::size_t i = 0; i < *slot_count; ++i) {
        ATELIER_TRY_BIND(slot, in.text());
        slots.push_back({std::move(*slot)});
    }
    ATELIER_TRY_BIND(part_count, in.count<geometry::MeshPart>(3 * 4));
    if (!*part_count || *part_count > *vertex_count / 3)
        return std::unexpected(Error{ErrorCode::invalid_storage, "Mesh snapshot has an invalid part count."});
    std::vector<geometry::MeshPart> parts;
    parts.reserve(*part_count);
    for (std::size_t i = 0; i < *part_count; ++i) {
        ATELIER_TRY_BIND(first, in.u32());
        ATELIER_TRY_BIND(count, in.u32());
        ATELIER_TRY_BIND(material, in.u32());
        parts.push_back({*first, *count, *material});
    }
    ATELIER_TRY_BIND(default_count, in.count<SlotMaterial>(2 * 4));
    if (*default_count > *slot_count)
        return std::unexpected(Error{ErrorCode::invalid_storage, "Mesh snapshot has too many default materials."});
    asset.materials.reserve(*default_count);
    for (std::size_t i = 0; i < *default_count; ++i) {
        ATELIER_TRY_BIND(slot, in.text());
        ATELIER_TRY_BIND(key, in.text());
        asset.materials.push_back({std::move(*slot), std::move(*key)});
    }
    ATELIER_TRY(in.finish());
    auto mesh =
        geometry::Mesh::restore_published(std::move(vertices), std::move(masks), std::move(slots), std::move(parts));
    if (!mesh)
        return std::unexpected(Error{ErrorCode::invalid_storage, std::move(mesh.error().message)});
    asset.mesh = std::move(*mesh);
    ATELIER_TRY(detail::bindings(asset));
    return asset;
}
}
