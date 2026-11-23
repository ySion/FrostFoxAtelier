#include "application/presentation.hpp"
#include "editor/assets.hpp"
#include "assets/file.hpp"

namespace atelier::application {
Result<std::vector<std::wstring>> editor_fonts() {
    ATELIER_TRY_BIND(root, editor::asset_directory());
    const auto directory = *root / "fonts";
    std::vector<std::wstring> paths;
    for (const auto* file : {L"SarasaUiSC-Regular.ttf", L"SarasaUiSC-SemiBold.ttf", L"SourceCodePro-Regular.otf",
                             L"SourceCodePro-Bold.otf"})
        paths.push_back((directory / file).wstring());
    return paths;
}

Result<std::shared_ptr<const ui::StyleSheet>> editor_styles() {
    ATELIER_TRY_BIND(root, editor::asset_directory());
    ATELIER_TRY_BIND(source, assets::read_file(*root / "editor.ss", 1024 * 1024));
    auto parsed = ui::StyleSheet::parse(*source);
    if (!parsed)
        return std::unexpected(Error{ErrorCode::invalid_definition, "editor.ss:" + std::to_string(parsed.error().line) +
                                                                        ":" + std::to_string(parsed.error().column) +
                                                                        ": " + parsed.error().message});
    return std::move(*parsed);
}
}
