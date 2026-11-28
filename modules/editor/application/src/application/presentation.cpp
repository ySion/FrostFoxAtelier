#include "application/presentation.hpp"
#include "editor/assets.hpp"
#include "assets/file.hpp"

namespace atelier::application {
Result<Array<std::wstring>> editor_fonts() {
    ATELIER_TRY_BIND(root, editor::asset_directory());
    const auto directory = *root / "fonts";
    Array<std::wstring> paths;
    for (const auto* file : {L"SarasaUiSC-Regular.ttf", L"SarasaUiSC-SemiBold.ttf", L"SourceCodePro-Regular.otf",
                             L"SourceCodePro-Bold.otf"})
        paths.add((directory / file).wstring());
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

Result<std::shared_ptr<const ui::StyleSheet>> editor_styles(Array<std::byte>* raw_source) {
    ATELIER_TRY_BIND(path, editor_style_path());
    ATELIER_TRY_BIND(source, assets::read_file(*path, 1024 * 1024));
    const auto chars = as_chars(*source);
    auto parsed = ui::StyleSheet::parse(chars);
    if (!parsed)
        return std::unexpected(Error{ErrorCode::invalid_definition,
                                     String::concat("editor.ss:", parsed.error().line, ":", parsed.error().column,
                                                    ": ", parsed.error().message)});
    if (raw_source)
        *raw_source = std::move(*source);
    return std::move(*parsed);
}
}
