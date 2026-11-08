#include "assets/reader.hpp"
#include "foundation/sha256.hpp"
#include <bit>
#include <cstring>
#include <map>

namespace atelier::assets {
namespace {
// Canonical scalar encoding is buffered: a large mesh never makes one provider
// call per scalar or allocates another mesh-sized byte string.
class Encoding {
public:
    void bytes(std::span<const std::byte> value) {
        while (!value.empty()) {
            const auto count = std::min(value.size(), buffer_.size() - used_);
            std::memcpy(buffer_.data() + used_, value.data(), count);
            used_ += count;
            value = value.subspan(count);
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

    void floats(std::span<const float> values) {
        for (auto value : values)
            u32(std::bit_cast<std::uint32_t>(value));
    }

    void text(std::string_view value) {
        size(value.size());
        bytes(std::as_bytes(std::span(value)));
    }

    std::string finish() {
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

Result<std::string> mesh_content_sha256(const MeshAsset& asset) {
    if (!asset.mesh)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Cannot digest an unresolved mesh."});
    const auto& mesh = *asset.mesh;
    std::map<std::string, std::size_t, std::less<>> slots;
    for (std::size_t i = 0; i < mesh.materials().size(); ++i)
        if (auto [at, inserted] = slots.emplace(geometry::material_slot_name(mesh, i), i); !inserted)
            at->second = mesh.materials().size();
    std::vector<const std::string*> defaults(mesh.materials().size());
    for (const auto& binding : asset.materials) {
        const auto at = slots.find(binding.slot);
        if (at == slots.end() || at->second == defaults.size() || defaults[at->second])
            return std::unexpected(Error{ErrorCode::invalid_definition,
                                         "Mesh default names a missing, ambiguous or duplicate slot: " + binding.slot});
        defaults[at->second] = &binding.key;
    }
    Encoding out;
    constexpr std::string_view domain = "Atelier.Mesh";
    out.bytes(std::as_bytes(std::span(domain)));
    out.u32(mesh_digest_version);
    out.size(mesh.vertices().size());
    for (const auto& v : mesh.vertices()) {
        out.floats(v.position);
        out.floats(v.normal);
        out.floats(v.uv);
        out.floats(v.tangent);
    }
    out.floats(mesh.bounds().minimum);
    out.floats(mesh.bounds().maximum);
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
