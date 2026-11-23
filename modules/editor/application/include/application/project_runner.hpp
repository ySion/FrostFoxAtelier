#pragma once
#include <span>

namespace atelier::application {
int run_project_command(std::span<wchar_t*> arguments);
}
