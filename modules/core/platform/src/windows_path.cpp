#include "platform/windows_path.hpp"

#include <Windows.h>
#include <climits>
#include <string_view>

namespace atelier::platform {
namespace {

bool ordinal_equal(std::wstring_view left, std::wstring_view right) noexcept {
    if (left.size() != right.size())
        return false;
    if (left.empty())
        return true;
    if (left.size() > INT_MAX)
        return false;
    const auto len = static_cast<int>(left.size());
    return CompareStringOrdinal(left.data(), len, right.data(), len, TRUE) == CSTR_EQUAL;
}

std::wstring_view trim_non_root_separators(std::wstring_view path, std::size_t root_length) noexcept {
    while (path.size() > root_length && (path.back() == L'\\' || path.back() == L'/'))
        path.remove_suffix(1);
    return path;
}

} // namespace

bool paths_lexically_equal(const std::filesystem::path& left, const std::filesystem::path& right) noexcept {
    auto left_norm = left.lexically_normal();
    left_norm.make_preferred();
    auto right_norm = right.lexically_normal();
    right_norm.make_preferred();
    const auto left_root = left_norm.root_path().native().size();
    const auto right_root = right_norm.root_path().native().size();
    return ordinal_equal(trim_non_root_separators(left_norm.native(), left_root),
                         trim_non_root_separators(right_norm.native(), right_root));
}

bool is_subpath_of(const std::filesystem::path& candidate, const std::filesystem::path& base) noexcept {
    if (candidate.empty() || base.empty())
        return false;
    auto cand_norm = candidate.lexically_normal();
    cand_norm.make_preferred();
    auto base_norm = base.lexically_normal();
    base_norm.make_preferred();

    if (cand_norm.has_root_directory() != base_norm.has_root_directory() ||
        !ordinal_equal(cand_norm.root_name().native(), base_norm.root_name().native())) {
        return false;
    }

    auto cand_it = cand_norm.begin();
    const auto cand_end = cand_norm.end();
    const auto base_end = base_norm.end();
    const auto skip_neutral = [](auto& it, const auto& end) noexcept {
        while (it != end && (it->empty() || it->native() == L"."))
            ++it;
    };

    for (auto base_it = base_norm.begin();;) {
        skip_neutral(base_it, base_end);
        if (base_it == base_end)
            break;
        skip_neutral(cand_it, cand_end);
        if (cand_it == cand_end || !ordinal_equal(cand_it->native(), base_it->native()))
            return false;
        ++base_it;
        ++cand_it;
    }
    for (; cand_it != cand_end; ++cand_it) {
        if (cand_it->native() == L"..")
            return false;
    }
    return true;
}

std::expected<String, UtfError> generic_path_to_utf8(const std::filesystem::path& path) noexcept {
    const std::wstring_view wide = path.native();
    if (wide.empty())
        return String{};
    if (wide.size() <= String::inline_capacity && detail::is_pure_ascii(wide)) {
        char inline_buf[String::inline_capacity];
        for (std::size_t i = 0; i < wide.size(); ++i) {
            const char c = static_cast<char>(static_cast<std::uint16_t>(wide[i]));
            inline_buf[i] = c == '\\' ? '/' : c;
        }
        return String::from_utf8_validated({inline_buf, wide.size()});
    }
    InlineArray<char, 128> scratch;
    if (const auto status = detail::utf16_to_utf8_scratch(wide, scratch); !status)
        return std::unexpected(status.error());
    for (char& ch : scratch) {
        if (ch == '\\')
            ch = '/';
    }
    return String::from_utf8_validated({scratch.data(), scratch.size()});
}

} // namespace atelier::platform
