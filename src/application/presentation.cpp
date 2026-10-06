#include "application/presentation.hpp"
#include "editor/assets.hpp"
#include <fstream>
#include <stdexcept>

namespace atelier::application {
std::vector<std::wstring> editor_fonts() {
    const auto directory=editor::asset_directory()/"fonts";
    std::vector<std::wstring> paths;
    for (const auto* file:{L"SarasaUiSC-Regular.ttf",L"SarasaUiSC-SemiBold.ttf",
                          L"SourceCodePro-Regular.otf",L"SourceCodePro-Bold.otf"}) paths.push_back((directory/file).wstring());
    return paths;
}
std::shared_ptr<const ui::StyleSheet> editor_styles() {
    const auto path=editor::asset_directory()/"editor.ss";
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if (!file) throw std::runtime_error("Cannot open editor.ss beside the executable");
    const auto size=file.tellg();
    if (size<0 || size>1024*1024) throw std::runtime_error("Invalid or oversized editor.ss");
    std::string source(static_cast<std::size_t>(size),'\0');
    file.seekg(0);
    if (!file.read(source.data(),static_cast<std::streamsize>(source.size()))) throw std::runtime_error("Cannot read editor.ss");
    auto parsed=ui::StyleSheet::parse(source);
    if (!parsed) throw std::runtime_error("editor.ss:"+std::to_string(parsed.error().line)+":"+
        std::to_string(parsed.error().column)+": "+parsed.error().message);
    return std::move(*parsed);
}
}
