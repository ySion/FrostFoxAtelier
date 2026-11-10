#pragma once
#include "ui/style_sheet.hpp"
#include "runtime/runtime.hpp"
#include <memory>
#include <string>
#include <vector>

namespace atelier::application {
std::vector<std::wstring> editor_fonts();

Result<std::shared_ptr<const ui::StyleSheet>> editor_styles();
}
