#include "assets/reader.hpp"
#include "foundation/map.hpp"
#include "foundation/sha256.hpp"
#include <bit>
#include <cstring>

namespace atelier::assets {
namespace {
// Canonical scalar encoding is buffered: a large mesh never makes one provider
// call per scalar or allocates another mesh-sized byte string.
class Encoding {
public:
    void bytes(Span<const std::byte> value) {
        while (!value.empty()) {
            const auto count = std::min(value.size(), buffer_.size() - used_);
            std::memcpy(buffer_.data() + used_, value.data(), count);
            used_ += count;
            value = value.slice(count);
            if (used_ == buffer_.size())
                flush();
        }
    }

    void u8(std::uint8_t value) {
        if (used_ == buffer_.size())
            flush();
        buffer_[used_++] = static_cast<std::byte>(value);
    }

    void u32(std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) {
            u8(static_cast<std::uint8_t>(value));
            value >>= 8;
        }
    }

    void size(std::uint64_t value) {
        for (unsigned i = 0; i < 8; ++i) {
            u8(static_cast<std::uint8_t>(value));
            value >>= 8;
        }
    }

    void floats(Span<const float> values) {
        for (auto value : values)
            u32(std::bit_cast<std::uint32_t>(value));
    }

    void vector3(math::Vector3 value) {
        u32(std::bit_cast<std::uint32_t>(value.x));
        u32(std::bit_cast<std::uint32_t>(value.y));
        u32(std::bit_cast<std::uint32_t>(value.z));
    }

    void text(StringView value) {
        size(value.size());
        bytes(as_bytes(value));
    }

    String finish() {
        flush();
        return hash_.finish();
    }

private:
    void flush() {
        if (used_)
            hash_.add({buffer_.data(), used_});
        used_ = 0;
    }

    foundation::Sha256 hash_;

    std::array<std::byte, 65536> buffer_;

    std::size_t used_{};
};
}

Result<String> mesh_content_sha256(const MeshAsset& asset) {
    if (!asset.mesh)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Cannot digest an unresolved mesh."});
    const auto& mesh = *asset.mesh;
    Map<String, std::size_t> slots;
    slots.reserve(mesh.materials().size());
    for (std::size_t i = 0; i < mesh.materials().size(); ++i)
        if (auto [at, inserted] = slots.try_emplace(geometry::material_slot_name(mesh, i), i); !inserted)
            *at = mesh.materials().size();
    Array<const String*> defaults(mesh.materials().size());
    for (const auto& binding : asset.materials) {
        const auto* at = slots.find(binding.slot);
        if (!at || *at == defaults.size() || defaults[*at])
            return std::unexpected(
                Error{ErrorCode::invalid_definition,
                      String::concat("Mesh default names a missing, ambiguous or duplicate slot: ", binding.slot)});
        defaults[*at] = &binding.key;
    }
    Encoding out;
    constexpr StringView domain = "Atelier.Mesh";
    out.bytes(as_bytes(domain));
    out.u32(mesh_digest_version);
    out.size(mesh.vertices().size());
    for (const auto& v : mesh.vertices()) {
        out.floats(v.position);
        out.floats(v.normal);
        out.floats(v.uv);
        out.floats(v.tangent);
    }
    out.vector3(mesh.bounds().minimum);
    out.vector3(mesh.bounds().maximum);
    out.size(mesh.vertices().size() / 3);
    for (std::size_t i = 0; i < mesh.vertices().size() / 3; ++i)
        out.u8(mesh.edge_mask(i));
    out.size(mesh.parts().size());
    for (const auto& p : mesh.parts()) {
        out.u32(p.first_vertex);
        out.u32(p.vertex_count);
        out.u32(p.material);
    }
    out.size(mesh.materials().size());
    for (const auto& slot : mesh.materials())
        out.text(slot.name);
    out.size(defaults.size());
    for (const auto* key : defaults) {
        out.u8(key != nullptr);
        if (key)
            out.text(*key);
    }
    return out.finish();
}
}
