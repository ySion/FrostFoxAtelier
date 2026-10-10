#pragma once
#include "material/surface.hpp"
#include <string_view>
namespace atelier::editor {
// Shared WIC decoding for project texture assets and model-import dependencies.
material::ImageRef decode_image(std::string_view bytes);
}
