#pragma once
#include "foundation/array.hpp"
#include "ui/style_sheet.hpp"
#include "runtime/runtime.hpp"
#include <filesystem>
#include <memory>
#include <string>

namespace atelier::application {
Result<Array<std::wstring>> editor_fonts();

Result<std::filesystem::path> editor_style_path();

Result<std::shared_ptr<const ui::StyleSheet>> editor_styles(Array<std::byte>* raw_source = nullptr);
}
