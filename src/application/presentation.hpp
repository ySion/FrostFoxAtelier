#pragma once
#include "ui/style_sheet.hpp"
#include <memory>
#include <string>
#include <vector>

namespace atelier::application {
std::vector<std::wstring> editor_fonts();
std::shared_ptr<const ui::StyleSheet> editor_styles();
}
