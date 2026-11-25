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

Result<std::filesystem::path> editor_style_path() {
#ifdef ATELIER_STYLE_AUTHORING_PATH
    const std::filesystem::path authoring{ATELIER_STYLE_AUTHORING_PATH};
    std::error_code error;
    if (std::filesystem::is_regular_file(authoring, error))
        return authoring;
#endif
    ATELIER_TRY_BIND(root, editor::asset_directory());
    return *root / "editor.ss";
}

Result<std::shared_ptr<const ui::StyleSheet>> editor_styles(std::string* raw_source) {
    ATELIER_TRY_BIND(path, editor_style_path());
    ATELIER_TRY_BIND(source, assets::read_file(*path, 1024 * 1024));
    auto parsed = ui::StyleSheet::parse(*source);
    if (!parsed)
        return std::unexpected(Error{ErrorCode::invalid_definition, "editor.ss:" + std::to_string(parsed.error().line) +
                                                                        ":" + std::to_string(parsed.error().column) +
                                                                        ": " + parsed.error().message});
    if (raw_source)
        *raw_source = std::move(*source);
    return std::move(*parsed);
}
}
