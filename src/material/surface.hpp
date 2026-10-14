#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace atelier::material {

// Immutable image content. A slot, not the file, determines its transfer function.
class Image final {
public:
    Image(std::uint32_t width, std::uint32_t height, std::vector<std::uint8_t> pixels)
        : width_(width), height_(height), pixels_(std::move(pixels)) {
        if (!width || !height || width > 8192 || height > 8192 ||
            std::uint64_t(width) * height * 4 != pixels_.size())
            throw std::invalid_argument("Invalid material image dimensions or RGBA storage.");
        std::array<std::array<std::uint64_t,256>,3> histogram{};
        for(std::size_t i=0;i<pixels_.size();i+=4) {
            for(std::size_t c=0;c<3;++c)++histogram[c][pixels_[i+c]];
            has_alpha_|=pixels_[i+3]!=255;
        }
        for(std::size_t c=0;c<3;++c) {
            double sum=0;
            for(std::size_t v=0;v<256;++v) {
                const double value=double(v)/255;
                sum+=histogram[c][v]*(value<=.04045?value/12.92:std::pow((value+.055)/1.055,2.4));
            }
            mean_linear_[c]=static_cast<float>(sum/(std::uint64_t(width)*height));
        }
    }
    Image& operator=(const Image&) = delete;
    Image& operator=(Image&&) = delete;
    std::uint32_t width() const noexcept { return width_; }
    std::uint32_t height() const noexcept { return height_; }
    std::span<const std::uint8_t> pixels() const noexcept { return pixels_; }
    const std::array<float,3>& mean_linear() const noexcept { return mean_linear_; }
    bool has_alpha() const noexcept { return has_alpha_; }
private:
    std::uint32_t width_, height_;
    std::vector<std::uint8_t> pixels_;
    std::array<float,3> mean_linear_{};
    bool has_alpha_{};
};
using ImageRef = std::shared_ptr<const Image>;

enum class Input : std::uint32_t { base_color, roughness, metallic, normal, emissive, opacity, count };
enum class Address : std::uint32_t { repeat, clamp, mirror };
enum class Coverage : std::uint32_t { opaque, mask };
inline constexpr float emission_limit = 65504.f; // Linear emission captured in the FP16 GBuffer.
inline constexpr std::size_t input_count = static_cast<std::size_t>(Input::count);

struct Texture {
    ImageRef image;
    std::array<float, 2> scale{1, 1}, offset{};
    float rotation{}; // Radians, applied after scale and before offset.
    Address address{Address::repeat};
    std::uint32_t channel{}; // Scalar inputs select R/G/B/A. Color and normal use RGB.
    bool operator==(const Texture&) const = default;
};
struct Parameters {
    std::array<float, 4> base_color{1, 1, 1, 1};
    float roughness{.65f}, metallic{}, normal_scale{1}, normal_y_sign{1};
    std::array<float, 3> emissive{}; // Linear radiance, not display brightness.
    Coverage coverage{Coverage::opaque};
    float alpha_cutoff{.5f};
    bool double_sided{true}, emission_two_sided{true};
    bool operator==(const Parameters&) const = default;
};
struct Description {
    Parameters parameters;
    std::array<Texture, input_count> textures;
    bool operator==(const Description&) const = default;
    Texture& texture(Input input) { return textures[static_cast<std::size_t>(input)]; }
    const Texture& texture(Input input) const { return textures[static_cast<std::size_t>(input)]; }
};

// Point picking uses the same UV transform, bilinear base-mip alpha and cutoff
// as material ray visibility. Screen derivatives belong only to raster filtering.
inline bool covered(const Description& surface,std::array<double,2> uv) {
    if(surface.parameters.coverage==Coverage::opaque)return true;
    const auto sample=[&](const Texture& texture,std::uint32_t channel) {
        if(!texture.image)return 1.;
        const double c=std::cos(double(texture.rotation)),s=std::sin(double(texture.rotation));
        const double u=uv[0]*texture.scale[0],v=uv[1]*texture.scale[1];
        std::array<double,2> point{c*u-s*v+texture.offset[0],s*u+c*v+texture.offset[1]};
        const std::array size{int(texture.image->width()),int(texture.image->height())};
        std::array<int,2> lower{};std::array<double,2> weight{};
        for(std::size_t axis=0;axis<2;++axis) {
            auto& p=point[axis];
            if(texture.address==Address::repeat)p-=std::floor(p);
            else if(texture.address==Address::mirror) {p-=std::floor(p*.5)*2;p=p>1?2-p:p;}
            else p=std::clamp(p,0.,1.);
            p=p*size[axis]-.5;lower[axis]=static_cast<int>(std::floor(p));weight[axis]=p-lower[axis];
        }
        const auto coordinate=[&](int value,int extent) {
            return texture.address==Address::repeat?(value+extent)%extent:std::clamp(value,0,extent-1);
        };
        const auto texel=[&](int x,int y) {
            const auto index=std::size_t(coordinate(y,size[1]))*size[0]+coordinate(x,size[0]);
            return texture.image->pixels()[index*4+channel]/255.;
        };
        return std::lerp(std::lerp(texel(lower[0],lower[1]),texel(lower[0]+1,lower[1]),weight[0]),
                         std::lerp(texel(lower[0],lower[1]+1),texel(lower[0]+1,lower[1]+1),weight[0]),weight[1]);
    };
    const auto& opacity=surface.texture(Input::opacity);
    return surface.parameters.base_color[3]*sample(surface.texture(Input::base_color),3)*sample(opacity,opacity.channel)>=surface.parameters.alpha_cutoff;
}

// A validated, immutable capture. It owns pixels, never Runtime objects or GPU handles.
class Surface final {
public:
    explicit Surface(Description description = {}) : description_(std::move(description)) {
        const auto unit = [](float v) { return std::isfinite(v) && v >= 0 && v <= 1; };
        const auto& p = description_.parameters;
        for (float v : p.base_color) if (!unit(v)) throw std::invalid_argument("Invalid material base color.");
        for (float v : p.emissive) if (!std::isfinite(v) || v < 0 || v > emission_limit)
            throw std::invalid_argument("Invalid material emission.");
        if (!unit(p.roughness) || !unit(p.metallic) || !unit(p.alpha_cutoff) ||
            !std::isfinite(p.normal_scale) || p.normal_scale < 0 || p.normal_scale > 16 ||
            (p.normal_y_sign!=1 && p.normal_y_sign!=-1) || p.coverage > Coverage::mask)
            throw std::invalid_argument("Invalid material parameters.");
        for (const auto& texture : description_.textures) {
            if (texture.address > Address::mirror || texture.channel > 3 || !std::isfinite(texture.rotation))
                throw std::invalid_argument("Invalid material texture sampling.");
            for (float v : texture.scale) if (!std::isfinite(v) || std::abs(v) > 1e6f)
                throw std::invalid_argument("Invalid material UV scale.");
            for (float v : texture.offset) if (!std::isfinite(v) || std::abs(v) > 1e6f)
                throw std::invalid_argument("Invalid material UV offset.");
        }
    }
    Surface& operator=(const Surface&) = delete;
    Surface& operator=(Surface&&) = delete;
    const Description& description() const noexcept { return description_; }
private:
    Description description_;
};
using SurfaceRef = std::shared_ptr<const Surface>;
inline SurfaceRef default_surface() {
    static const auto surface = std::make_shared<const Surface>();
    return surface;
}
struct Slot {
    std::string name;
    SurfaceRef surface{default_surface()};
};

}
