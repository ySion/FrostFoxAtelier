#pragma once
#include "foundation/span.hpp"

namespace atelier::application {
int run_project_command(Span<wchar_t*> arguments);
}
