#pragma once
#include "ui/style_sheet.hpp"
#include "runtime/runtime.hpp"
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace atelier::application {
Result<std::vector<std::wstring>> editor_fonts();

Result<std::filesystem::path> editor_style_path();

Result<std::shared_ptr<const ui::StyleSheet>> editor_styles(std::string* raw_source = nullptr);
}
