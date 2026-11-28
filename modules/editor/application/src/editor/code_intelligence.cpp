#include "editor/code_intelligence.hpp"
#include "editor/project_code.hpp"
#include "foundation/hash.hpp"
#include "foundation/inline_array.hpp"
#include "foundation/map.hpp"
#include "foundation/set.hpp"
#include "json/json.hpp"
#include "language/declarations.hpp"
#include "language/module.hpp"
#include "physics/physics.hpp"
#include "platform/utf.hpp"
#include "platform/windows_path.hpp"
#include "presentation/presentation.hpp"
#include "procedural/graph.hpp"
#include "source/program.hpp"
#include "ui/style_sheet.hpp"
#include "ui_language/component.hpp"
#include "ui_language/declaration.hpp"
#include "world/world.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace atelier::editor {
namespace {

struct StyleSheetScanState {
    int brace_depth{};

    bool masked{};
};

StyleSheetScanState scan_style_sheet_prefix(StringView prefix) noexcept {
    StyleSheetScanState state;
    bool in_comment = false;
    bool in_str = false;
    char quote = '\0';
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        const char c = prefix[i];
        const char next = i + 1 < prefix.size() ? prefix[i + 1] : '\0';
        if (in_comment) {
            if (c == '*' && next == '/') {
                in_comment = false;
                ++i;
            }
            continue;
        }
        if (in_str) {
            if (c == '\\' && i + 1 < prefix.size())
                ++i;
            else if (c == quote)
                in_str = false;
            continue;
        }
        if (c == '/' && next == '*') {
            in_comment = true;
            ++i;
            continue;
        }
        if (c == '"' || c == '\'') {
            in_str = true;
            quote = c;
            continue;
        }
        if (c == '{')
            ++state.brace_depth;
        else if (c == '}')
            --state.brace_depth;
    }
    state.masked = in_comment || in_str;
    return state;
}

String u8_lit(StringView validated) noexcept {
    return String::from_utf8_validated(validated);
}

void u8_append(String& dst, StringView validated) noexcept {
    static_cast<void>(dst.append(validated));
}

String u8_cat(std::initializer_list<StringView> parts) noexcept {
    std::size_t total = 0;
    for (const StringView part : parts)
        total += part.size();
    String out;
    out.reserve(total);
    for (const StringView part : parts)
        static_cast<void>(out.append(part));
    return out;
}

String u8_safe(StringView raw) noexcept {
    if (auto res = String::from_utf8(raw))
        return std::move(*res);
    return String::literal<"(invalid UTF-8)">();
}

template <class T>
    requires std::same_as<std::remove_cvref_t<T>, std::string> ||
             std::same_as<std::remove_cvref_t<T>, std::string_view>
String u8_safe(const T& raw) noexcept {
    return u8_safe(StringView{std::string_view{raw}});
}

String sanitize_diag_utf8(StringView sv) noexcept {
    if (!sv.is_valid_utf8())
        return String::literal<"(诊断文本不是有效 UTF-8)">();
    bool needs_clean = false;
    for (std::size_t i = 0; i < sv.size(); ++i) {
        const auto b = static_cast<unsigned char>(sv[i]);
        if ((b < 32u && b != '\n' && b != '\t') || b == 127u) {
            needs_clean = true;
            break;
        }
    }
    if (!needs_clean)
        return String::from_utf8_validated(sv);
    Array<char> cleaned;
    cleaned.reserve(sv.size() + 16);
    for (std::size_t i = 0; i < sv.size(); ++i) {
        const auto b = static_cast<unsigned char>(sv[i]);
        if (b == '\r')
            continue;
        if (b < 32u && b != '\n' && b != '\t') {
            cleaned.add('\xe2');
            cleaned.add('\x90');
            cleaned.add(static_cast<char>(0x80u + b));
        } else if (b == 127u) {
            cleaned.append({'\xe2', '\x90', '\xa1'});
        } else {
            cleaned.add(sv[i]);
        }
    }
    return String::from_utf8_validated({cleaned.data(), cleaned.size()});
}

template <class T>
    requires std::same_as<std::remove_cvref_t<T>, std::string> ||
             std::same_as<std::remove_cvref_t<T>, std::string_view>
String sanitize_diag_utf8(const T& raw) noexcept {
    return sanitize_diag_utf8(StringView{std::string_view{raw}});
}

std::wstring to_tooltip_wide(StringView validated_u8) {
    return platform::utf8_to_utf16_validated(validated_u8.utf8_prefix(4096));
}

std::wstring to_tooltip_wide(const String& validated_u8) {
    return to_tooltip_wide(validated_u8.view());
}

std::filesystem::path path_from_utf8(StringView u8) {
    if (auto w = platform::utf8_to_utf16(u8))
        return std::filesystem::path{std::move(*w)};
    return {};
}

String filename_or_fallback(const std::filesystem::path& p, StringView fallback = "draft") {
    if (auto f = platform::filename_utf8(p); f && !f->empty())
        return std::move(*f);
    return u8_lit(fallback);
}

String wrap_array_depth(String base, std::uint32_t depth) {
    for (std::uint32_t i = 0; i < depth; ++i)
        base = u8_cat({"Array<", base.view(), ">"});
    return base;
}

String format_source_type_name(const language::TypeName& t) {
    return wrap_array_depth(u8_safe(t.name), t.array_depth);
}

String format_method_param(const language::MethodParameter& mp) {
    String out;
    if (mp.passing == ParameterPassing::read_only)
        u8_append(out, "const ");
    out.append(format_source_type_name(mp.type));
    if (mp.passing != ParameterPassing::value)
        u8_append(out, "&");
    if (!mp.name.empty()) {
        u8_append(out, " ");
        out.append(u8_safe(mp.name));
    }
    return out;
}

String format_method_signature(const language::ClassMethod& m, Array<String>* out_params = nullptr) {
    const String mname = u8_safe(m.name);
    String sig = u8_cat({mname.view(), "("});
    for (std::size_t i = 0; i < m.parameters.size(); ++i) {
        if (i > 0)
            u8_append(sig, ", ");
        String p = format_method_param(m.parameters[i]);
        sig.append(p);
        if (out_params)
            out_params->add(std::move(p));
    }
    u8_append(sig, ") -> ");
    sig.append(format_source_type_name(m.result_type));
    return sig;
}

// Decodes SourceModule's internal ABI signature key `<ret>:<depth>([const ]<type>:<depth>[&];...`
// into human-readable Source syntax `name(T0 p0, ...) -> Ret`.
String format_encoded_function_signature(std::string_view fn_name_raw, std::string_view encoded_sig_raw,
                                         std::span<const std::string> param_names, Array<String>* out_params = nullptr,
                                         String* out_ret = nullptr) {
    const StringView fn_name{fn_name_raw};
    const StringView encoded_sig{encoded_sig_raw};
    const auto parse_typed_atom = [](StringView atom) -> String {
        const std::size_t colon = atom.rfind(':');
        if (colon == StringView::npos)
            return u8_safe(atom);
        std::uint32_t depth = 0;
        const StringView d_str = atom.slice(colon + 1);
        std::from_chars(d_str.data(), d_str.data() + d_str.size(), depth);
        return wrap_array_depth(u8_safe(atom.slice(0, colon)), depth);
    };

    const std::size_t paren = encoded_sig.find('(');
    const String ret =
        paren == StringView::npos ? String::literal<"void">() : parse_typed_atom(encoded_sig.slice(0, paren));
    if (out_ret)
        *out_ret = ret;

    const String fname = u8_safe(fn_name);
    String sig = u8_cat({fname.view(), "("});
    if (paren != StringView::npos) {
        StringView rest = encoded_sig.slice(paren + 1);
        std::size_t idx = 0;
        while (!rest.empty()) {
            const std::size_t semi = rest.find(';');
            StringView seg = semi == StringView::npos ? rest : rest.slice(0, semi);
            rest = semi == StringView::npos ? StringView{} : rest.slice(semi + 1);
            if (seg.empty())
                continue;
            const bool is_const = seg.starts_with("const ");
            if (is_const)
                seg.remove_prefix(6);
            const bool is_ref = !seg.empty() && seg.back() == '&';
            if (is_ref)
                seg.remove_suffix(1);
            String param_str;
            if (is_const)
                u8_append(param_str, "const ");
            param_str.append(parse_typed_atom(seg));
            if (is_ref)
                u8_append(param_str, "&");
            if (idx < param_names.size() && !param_names[idx].empty()) {
                u8_append(param_str, " ");
                param_str.append(u8_safe(param_names[idx]));
            }
            if (idx > 0)
                u8_append(sig, ", ");
            sig.append(param_str);
            if (out_params)
                out_params->add(std::move(param_str));
            ++idx;
        }
    }
    u8_append(sig, ") -> ");
    sig.append(ret);
    return sig;
}

std::pair<std::uint32_t, std::uint32_t> diagnostic_token_span(StringView line_u8, std::uint32_t col_start) noexcept {
    if (line_u8.empty())
        return {0u, 0u};
    const auto n = static_cast<std::uint32_t>(line_u8.size());
    std::uint32_t s = col_start < n ? col_start : n - 1;
    while (s > 0 && (static_cast<unsigned char>(line_u8[s]) & 0xc0u) == 0x80u)
        --s;
    while (s < n && (line_u8[s] == ' ' || line_u8[s] == '\t'))
        ++s;
    if (s >= n)
        return {col_start < n ? col_start : n, n};
    const auto lead = static_cast<unsigned char>(line_u8[s]);
    const std::uint32_t cp_len = lead < 0x80u ? 1u : lead < 0xe0u ? 2u : lead < 0xf0u ? 3u : 4u;
    std::uint32_t e = std::min(n, s + cp_len);
    if (is_ascii_ident_continue(line_u8[s]) || line_u8[s] == '-') {
        while (e < n && (is_ascii_ident_continue(line_u8[e]) || line_u8[e] == '-'))
            ++e;
    } else if (is_ascii_digit(line_u8[s])) {
        while (e < n && (is_ascii_ident_continue(line_u8[e]) || line_u8[e] == '.'))
            ++e;
    } else if (line_u8[s] == '"' || line_u8[s] == '\'') {
        const char q = line_u8[s];
        while (e < n && line_u8[e] != q) {
            if (line_u8[e] == '\\' && e + 1 < n)
                ++e;
            const auto b = static_cast<unsigned char>(line_u8[e]);
            e += b < 0x80u ? 1u : b < 0xe0u ? 2u : b < 0xf0u ? 3u : 4u;
            if (e > n)
                e = n;
        }
        if (e < n)
            ++e;
    }
    return {s, e};
}

StringView strip_location_prefix(StringView raw, StringView prefix_name, std::size_t* out_line = nullptr,
                                 std::size_t* out_col = nullptr) {
    StringView rest = raw;
    if (!prefix_name.empty() && rest.starts_with(prefix_name) && rest.size() > prefix_name.size() &&
        rest[prefix_name.size()] == ':') {
        rest.remove_prefix(prefix_name.size() + 1);
    } else {
        const std::size_t colon = rest.find(':');
        if (colon != StringView::npos && colon + 1 < rest.size() && is_ascii_digit(rest[colon + 1]))
            rest.remove_prefix(colon + 1);
    }
    std::size_t ln = 0;
    const auto r1 = std::from_chars(rest.data(), rest.data() + rest.size(), ln);
    if (r1.ec == std::errc{} && r1.ptr < rest.data() + rest.size() && *r1.ptr == ':') {
        std::size_t cl = 0;
        const auto r2 = std::from_chars(r1.ptr + 1, rest.data() + rest.size(), cl);
        if (r2.ec == std::errc{} && r2.ptr < rest.data() + rest.size() && *r2.ptr == ':') {
            if (out_line && ln > 0)
                *out_line = ln;
            if (out_col && cl > 0)
                *out_col = cl;
            StringView tail(r2.ptr + 1, static_cast<std::size_t>((rest.data() + rest.size()) - (r2.ptr + 1)));
            while (!tail.empty() && tail.front() == ' ')
                tail.remove_prefix(1);
            return tail;
        }
    }
    return raw;
}

int fuzzy_match_score(StringView candidate, StringView query) noexcept {
    if (query.empty())
        return 100;
    if (query.size() > candidate.size())
        return -1;
    if (candidate.starts_with(query))
        return 1000 - static_cast<int>(candidate.size() - query.size());
    if (candidate.starts_with_ascii_case_insensitive(query))
        return 850 - static_cast<int>(candidate.size() - query.size());

    std::size_t qi = 0;
    int bonus = 0;
    for (std::size_t ci = 0; ci < candidate.size() && qi < query.size(); ++ci) {
        const char raw_a = candidate[ci];
        const bool upper_boundary =
            (ci == 0) || (raw_a >= 'A' && raw_a <= 'Z') ||
            (ci > 0 && (candidate[ci - 1] == '_' || candidate[ci - 1] == '.' || candidate[ci - 1] == '-'));
        if (ascii_lower(raw_a) == ascii_lower(query[qi])) {
            if (upper_boundary)
                bonus += 25;
            ++qi;
        }
    }
    if (qi == query.size())
        return 400 + bonus - static_cast<int>(candidate.size());
    return -1;
}

Array<String> to_sorted_array(const Set<String>& set) {
    Array<String> out;
    out.reserve(set.size());
    for (const String& item : set)
        out.add(item);
    std::ranges::sort(out);
    return out;
}

struct IntrinsicSpec {
    StringView name;

    StringView return_type;

    std::array<StringView, 4> params;

    std::size_t param_count{};

    StringView doc;
};

constexpr std::array<IntrinsicSpec, 43> kMathIntrinsics{{
    {"dot", "T", {"vectorN a", "vectorN b"}, 2, "计算两个同维向量的点积（标量内积）。"},
    {"cross", "vector3", {"vector3 a", "vector3 b"}, 2, "计算两个三维向量的叉积（右手坐标系垂直向量）。"},
    {"length", "T", {"vectorN v"}, 1, "返回向量或四元数的欧几里得模长。"},
    {"length_squared", "T", {"vectorN v"}, 1, "返回向量或四元数模长的平方，避免开方开销。"},
    {"distance", "T", {"vectorN a", "vectorN b"}, 2, "返回两点之间的欧几里得距离。"},
    {"distance_squared", "T", {"vectorN a", "vectorN b"}, 2, "返回两点距离的平方。"},
    {"normalize_or_zero", "T", {"T v"}, 1, "安全归一化向量或四元数；零输入返回零值而不产生 NaN。"},
    {"fast_normalize_or_zero", "T", {"T v"}, 1, "使用近似倒数平方根快速安全归一化向量或四元数。"},
    {"conjugate", "quaternion", {"quaternion q"}, 1, "返回四元数的共轭四元数。"},
    {"slerp01",
     "quaternion",
     {"quaternion a", "quaternion b", "float t"},
     3,
     "在 [0, 1] 闭区间对单位四元数执行球面线性插值。"},
    {"fast_slerp01", "quaternion", {"quaternion a", "quaternion b", "float t"}, 3, "快速近似单位四元数球面插值。"},
    {"rotate", "vector3", {"quaternion q", "vector3 v"}, 2, "用单位四元数 q 旋转三维向量 v。"},
    {"transpose", "matrix4", {"matrix4 m"}, 1, "返回 4x4 矩阵的转置矩阵。"},
    {"transform_point",
     "vector3",
     {"vector3 point", "matrix4 m"},
     2,
     "按行向量约定通过 4x4 矩阵变换三维空间点（含平移）。"},
    {"transform_direction",
     "vector3",
     {"vector3 dir", "matrix4 m"},
     2,
     "按行向量约定通过 4x4 矩阵线性部分变换三维方向向量（忽略平移）。"},
    {"sqrt", "T", {"T x"}, 1, "逐分量计算平方根。"},
    {"fast_rsqrt", "T", {"T x"}, 1, "逐分量计算快速近似倒数平方根 1 / sqrt(x)。"},
    {"floor", "T", {"T x"}, 1, "逐分量向下取整。"},
    {"ceil", "T", {"T x"}, 1, "逐分量向上取整。"},
    {"trunc", "T", {"T x"}, 1, "逐分量向零截断小数部分。"},
    {"round", "T", {"T x"}, 1, "逐分量四舍五入到最近整数。"},
    {"fract", "T", {"T x"}, 1, "逐分量返回 x - floor(x) 小数部分。"},
    {"abs", "T", {"T x"}, 1, "逐分量返回绝对值。"},
    {"sign", "T", {"T x"}, 1, "逐分量返回符号值（-1, 0, +1）。"},
    {"min", "T", {"T a", "T b"}, 2, "逐分量返回较小值。"},
    {"max", "T", {"T a", "T b"}, 2, "逐分量返回较大值。"},
    {"step", "T", {"T edge", "T x"}, 2, "阶跃函数：x < edge 返回 0，否则返回 1。"},
    {"clamp", "T", {"T x", "T low", "T high"}, 3, "逐分量将 x 钳制在 [low, high] 闭区间内。"},
    {"cwise_mul", "vectorN", {"vectorN a", "vectorN b"}, 2, "执行两个同维向量的逐分量乘法（Hadamard 积）。"},
    {"cwise_div", "vectorN", {"vectorN a", "vectorN b"}, 2, "执行两个同维向量的逐分量除法。"},
    {"radians", "T", {"T degrees"}, 1, "将角度值转换为弧度值。"},
    {"degrees", "T", {"T radians"}, 1, "将弧度值转换为角度值。"},
    {"lerp", "T", {"T a", "T b", "T t"}, 3, "线性插值 a + (b - a) * t。"},
    {"smoothstep", "T", {"T edge0", "T edge1", "T x"}, 3, "在 [edge0, edge1] 之间执行 Hermite 平滑插值。"},
    {"sin", "T", {"T radians"}, 1, "逐分量计算正弦值（输入单位为弧度）。"},
    {"cos", "T", {"T radians"}, 1, "逐分量计算余弦值（输入单位为弧度）。"},
    {"atan2", "T", {"T y", "T x"}, 2, "逐分量计算四象限反正切值 atan2(y, x)，返回 [-pi, pi] 弧度。"},
    {"exp", "T", {"T x"}, 1, "逐分量计算自然指数 e^x。"},
    {"log", "T", {"T x"}, 1, "逐分量计算自然对数 ln(x)。"},
    {"pow", "T", {"T base", "T exp"}, 2, "逐分量计算幂函数 base^exp。"},
    {"is_finite", "bool", {"T x"}, 1, "检查标量、向量、四元数或矩阵全部分量是否为有限浮点数。"},
    {"as_uint", "uint", {"T x"}, 1, "按 IEEE-754 / 32-bit 位模式无损重解释为 uint。"},
    {"as_int", "int", {"T x"}, 1, "按 32-bit 位模式无损重解释为 int。"},
}};

constexpr std::array<std::pair<StringView, StringView>, 25> kUiNodeKinds{{
    {"Column", "纵向弹性栈布局容器"},
    {"Row", "横向弹性行布局容器"},
    {"Overlay", "层叠绝对/锚点定位容器"},
    {"List", "带滚动条的纵向列表容器"},
    {"Text", "静态或动态单行/多行文本与图标标签"},
    {"Button", "一次性命令触发按钮"},
    {"ToggleButton", "二态开关指示器按钮（建议纯图标）"},
    {"ChoiceButton", "互斥分段切换项或子页签按钮"},
    {"Combo", "下拉选择框或菜单弹出锚点"},
    {"Checkbox", "布尔复选框控件"},
    {"TextField", "单行可编辑或只读文本输入框"},
    {"NumberRow", "带标签与数值阶梯拖拽的数值行"},
    {"ParameterRow", "标准参数检查器左右分栏行"},
    {"Foldout", "可折叠分类分组标题栏"},
    {"Table", "高性能虚拟化数据表格/详情/平铺视图"},
    {"Tree", "层级大纲树视图容器"},
    {"TreeItem", "树节点行条目"},
    {"Splitter", "可拖拽面板分割条"},
    {"CodeEditor", "多色语法高亮与语言服务代码编辑器"},
    {"ColorPlane", "2D 饱和度/明度色板或色相光谱条"},
    {"Popup", "独立弹出菜单或浮层根容器"},
    {"MenuItem", "弹出菜单行命令项"},
    {"Repeat", "数据驱动动态列表复用节点"},
    {"Viewport", "3D 场景渲染视口嵌入槽"},
    {"Tab", "停靠面板标签页标题控件"},
}};

constexpr std::array<std::pair<StringView, StringView>, 44> kUiProperties{{
    {"text", "Text — 显示文本"},
    {"classes", "Text — 空格分隔的样式表类名"},
    {"icon", "Text | double — 语义矢量图标名或枚举值"},
    {"width", "double — 固定宽度 (DIP)"},
    {"height", "double — 固定高度 (DIP)"},
    {"min_width", "double — 最小宽度约束 (DIP)"},
    {"shrink_width", "double — 可收缩首选宽度 (DIP)"},
    {"flex_width", "double — 弹性基准宽度 (DIP)"},
    {"grow_width", "double — 弹性伸展基础宽度 (DIP)"},
    {"content_width", "double — 自适应内容宽度保底值"},
    {"content_height", "double — 自适应内容高度保底值"},
    {"expand_width", "bool — 填满父容器剩余水平空间"},
    {"expand_height", "bool — 填满父容器剩余垂直空间"},
    {"gap", "double — 子元素间距 (DIP)"},
    {"padding_left", "double — 左内边距 (DIP)"},
    {"padding_top", "double — 上内边距 (DIP)"},
    {"padding_right", "double — 右内边距 (DIP)"},
    {"padding_bottom", "double — 下内边距 (DIP)"},
    {"align_center", "bool — 交叉轴居中对齐"},
    {"wrap", "bool — 允许行内子项换行"},
    {"single_line", "bool — 单行截断不换行"},
    {"caption", "bool — 使用次级说明字重/比例"},
    {"panel", "bool — 作为面板表面绘制背景与边框"},
    {"enabled", "bool — 是否启用交互"},
    {"selected", "bool — 是否处于激活/选中态"},
    {"collapsed", "bool — 折叠隐藏且不占布局空间"},
    {"mounted", "bool — 条件挂载子组件作用域"},
    {"read_only", "bool — 只读模式（允许选择与复制，禁止修改）"},
    {"modified", "bool — 标记属性值已偏离默认值"},
    {"action", "ActionBinding — 激活命令绑定"},
    {"edit", "EditBinding | TextEditBinding — 编辑值绑定"},
    {"document", "CodeBinding — 代码文档与诊断注解绑定"},
    {"model", "TableBinding — 表格数据模型绑定"},
    {"items", "Items<T> — Repeat 列表数据源"},
    {"context", "Key — 右键或弹出菜单上下文键"},
    {"selection_group", "Key — ChoiceButton 互斥组键"},
    {"placeholder", "Text — 输入框为空时的占位提示"},
    {"out_of_flow", "bool — 脱离常规文档流（用于 Overlay 子项）"},
    {"offset_x", "double — 锚点水平偏移 (DIP)"},
    {"offset_y", "double — 锚点垂直偏移 (DIP)"},
    {"anchor_x", "double — 父容器归一化水平锚点 [0..1]"},
    {"anchor_y", "double — 父容器归一化垂直锚点 [0..1]"},
    {"tooltip_title", "Text — 悬停提示主标题"},
    {"tooltip", "Text — 悬停提示正文说明"},
}};

constexpr std::array<StringView, 36> kUiIconNames{{
    "close",     "maximize",     "restore",    "plus",   "back",      "forward", "up",        "down",   "left",
    "right",     "world",        "object",     "axes",   "component", "mesh",    "material",  "folder", "search",
    "view_list", "view_details", "view_tiles", "filter", "more",      "pointer", "translate", "rotate", "scale",
    "magnet",    "pin",          "lock",       "unlock", "save",      "reload",  "build",     "play",   "stop",
}};

constexpr std::array<std::pair<StringView, StringView>, 30> kSsProperties{{
    {"background", "Color | token(...) — 背景纯色填充"},
    {"background-image", "linear-gradient(top, bottom) | none — 垂直线性渐变背景"},
    {"color", "Color | token(...) — 前景文本颜色"},
    {"border-width", "Ndip — 边框线宽"},
    {"border-style", "solid | none — 边框样式"},
    {"border-edges", "all | top | right | bottom | left — 绘制边框的边集合"},
    {"border-color", "Color | token(...) — 外边框颜色"},
    {"border-inner-color", "Color | token(...) — 内侧 1px 高光描边颜色"},
    {"border-radius", "Ndip — 圆角半径（工业风建议 1.5dip–2dip）"},
    {"padding", "Ndip [Hdip ...] — 内边距（禁止在 :hover/:focus/:pressed 修改）"},
    {"font-family", "\"Font Name\" — 字体族名称"},
    {"font-size", "Ndip — 字号大小"},
    {"font-weight", "400 | 600 | 700 — 字重（禁止在瞬态选择器中修改）"},
    {"font-style", "normal | italic — 字体斜体样式"},
    {"text-align", "left | center | right — 水平文本对齐"},
    {"text-vertical-align", "top | center | bottom — 垂直文本对齐"},
    {"selection-color", "Color — 文本选区或行选中背景色"},
    {"caret-color", "Color — 文本输入光标颜色"},
    {"indicator-size", "Ndip — 矢量图标尺寸"},
    {"indicator-color", "Color | token(...) — 矢量图标前景色"},
    {"indicator-background", "Color — 指示器底槽颜色"},
    {"indicator-border-color", "Color — 指示器边框色"},
    {"accent-color", "Color | token(...) — 激活强调条颜色"},
    {"accent-width", "Ndip — 激活强调条厚度"},
    {"combo-indicator", "down | caret | none — 下拉指示箭头样式"},
    {"icon-align", "left | center | right — 纯图标对齐方式"},
    {"scrollbar-width", "Ndip — 滚动条宽度"},
    {"scrollbar-gutter", "stable | overlay — 滚动条槽位模式"},
    {"tree-row-height", "Ndip — 树节点行高"},
    {"tree-alternate-color", "Color — 奇偶交替行或编辑器当前行底色"},
}};
} // namespace

ui::CodeLanguage detect_code_language(const std::filesystem::path& path) noexcept {
    if (path.empty())
        return ui::CodeLanguage::plain;
    const auto fname = platform::filename_utf8(path);
    if (!fname)
        return ui::CodeLanguage::plain;
    const StringView sv = fname->view();
    if (sv.ends_with_ascii_case_insensitive(".ui.source"))
        return ui::CodeLanguage::ui_source;
    if (sv.ends_with_ascii_case_insensitive(".source") || sv.ends_with_ascii_case_insensitive(".ms"))
        return ui::CodeLanguage::source;
    if (sv.ends_with_ascii_case_insensitive(".ss"))
        return ui::CodeLanguage::style_sheet;
    if (sv.ends_with_ascii_case_insensitive(".json") || sv.ends_with_ascii_case_insensitive(".scene") ||
        sv.ends_with_ascii_case_insensitive(".material") || sv.ends_with_ascii_case_insensitive(".procgraph") ||
        sv.ends_with_ascii_case_insensitive(".project"))
        return ui::CodeLanguage::json;
    return ui::CodeLanguage::plain;
}

StringView code_language_name(ui::CodeLanguage language) noexcept {
    switch (language) {
    case ui::CodeLanguage::source:
        return "Atelier Source";
    case ui::CodeLanguage::ui_source:
        return "UI Source";
    case ui::CodeLanguage::style_sheet:
        return "Style Sheet";
    case ui::CodeLanguage::json:
        return "JSON";
    case ui::CodeLanguage::plain:
        return "Plain Text";
    }
    return "Plain Text";
}

struct CodeIntelligence::Impl {
    using Clock = std::chrono::steady_clock;

    struct SsTokenEntry {
        String name;

        String value;

        std::size_t line{1};

        std::size_t column{1};
    };

    struct UiDeclLocation {
        String name;

        std::uint32_t line{1};

        std::uint32_t column{1};

        std::uint32_t end_line{1};
    };

    // Scratch compiler contexts: Runtime MUST outlive Program / Module!
    std::unique_ptr<Runtime> scratch_runtime;

    std::unique_ptr<source::Program> scratch_program;

    std::unique_ptr<Runtime> scratch_ui_runtime;

    std::shared_ptr<ui_language::Module> last_ui_module;

    Array<source::SourceFile> cached_engine_files;

    bool engine_files_loaded{};

    std::filesystem::path cached_project_root;

    std::uint64_t cached_project_revision{};

    std::shared_ptr<const source::SourceSnapshot> cached_base_snapshot;

    // Observed runtime type catalog cache
    const Runtime* observed_runtime{};

    std::size_t observed_type_count{};

    struct NativeMemberDoc {
        String name;

        String signature;

        String return_type;

        String description;

        Array<String> params;

        bool is_method{};
    };

    struct NativeTypeDoc {
        String full_name;

        String short_name;

        String description;

        TypeKind kind{TypeKind::object};

        Array<NativeMemberDoc> members;

        Array<std::pair<String, String>> enumerators;
    };

    Array<NativeTypeDoc> native_types;

    // Active file tracking & debounce state
    std::filesystem::path active_path;

    ui::CodeLanguage active_language{ui::CodeLanguage::plain};

    std::uint64_t seen_revision{std::numeric_limits<std::uint64_t>::max()};

    std::uint64_t checked_revision{std::numeric_limits<std::uint64_t>::max()};

    std::uint64_t checked_open_digest{};

    Clock::time_point last_edit_time{};

    bool pending_validation{};

    // Last valid parsed AST / symbol caches for resilience while typing
    Optional<language::SourceModule> last_source_module;

    Optional<source::SourceSnapshot> last_snapshot;

    Array<language::TypeDeclaration> last_source_declarations;

    Array<ui_language::Declaration> last_ui_declarations;

    Array<UiDeclLocation> last_ui_locations;

    Array<SsTokenEntry> last_ss_tokens;

    Array<String> last_ss_classes;

    std::uint64_t generation{1};

    // Outputs consumed by SourceEditor / CodePanel
    Array<ui::CodeDiagnostic> active_diagnostics;

    Array<CodeProblemItem> problems;

    Array<DocumentSymbol> outline;

    Array<String> semantic_types;

    Array<String> semantic_functions;

    Array<String> semantic_fields;

    ~Impl() {
        scratch_program.reset();
        scratch_runtime.reset();
        last_ui_module.reset();
        scratch_ui_runtime.reset();
    }

    bool ensure_scratch_source() {
        if (scratch_program)
            return true;
        auto rt = std::make_unique<Runtime>();
        if (!world::register_types(*rt))
            return false;
        if (!presentation::register_types(*rt))
            return false;
        if (!physics::register_types(*rt))
            return false;
        if (!procedural::register_types(*rt))
            return false;
        auto prog = source::Program::create(*rt);
        if (!prog)
            return false;
        scratch_runtime = std::move(rt);
        scratch_program = std::move(*prog);
        return true;
    }

    bool ensure_scratch_ui() {
        if (scratch_ui_runtime)
            return true;
        auto rt = std::make_unique<Runtime>();
        if (!ui_language::register_types(*rt))
            return false;
        scratch_ui_runtime = std::move(rt);
        return true;
    }

    void ensure_engine_files() {
        if (engine_files_loaded)
            return;
        engine_files_loaded = true;
        if (auto loaded = source::engine_sources(source::default_engine_root()))
            cached_engine_files = std::move(*loaded);
    }

    String format_value_desc(const Runtime& rt, const ValueDescription& vd) const {
        const auto info = rt.describe(vd.type);
        if (!info)
            return String::literal<"unknown">();
        String name = u8_safe((*info)->name);
        if ((*info)->math != math::Type{})
            name = u8_safe(math::short_name((*info)->math));
        else if (name == "core.f32")
            name = String::literal<"float">();
        else if (name == "core.f64")
            name = String::literal<"double">();
        else if (name == "core.i32")
            name = String::literal<"int">();
        else if (name == "core.u32")
            name = String::literal<"uint">();
        else if (name == "core.bool")
            name = String::literal<"bool">();
        else if (name == "core.String")
            name = String::literal<"String">();
        if (vd.form == ValueForm::weak_reference)
            name = u8_cat({"weak ", name.view()});
        return name;
    }

    void refresh_native_catalog(const Runtime* world_rt) {
        const Runtime* rt = world_rt;
        if (!rt) {
            if (ensure_scratch_source())
                rt = scratch_runtime.get();
        }
        if (!rt)
            return;
        const std::size_t count = rt->type_count();
        if (rt == observed_runtime && count == observed_type_count)
            return;
        observed_runtime = rt;
        observed_type_count = count;
        native_types.clear();
        for (const TypeId tid : rt->types()) {
            const auto info_res = rt->describe(tid);
            if (!info_res)
                continue;
            const TypeInfo& info = **info_res;
            if (language::editor_reserved(info.name.view().std_view()) ||
                language::synthesized_array_name(info.name.view().std_view()))
                continue;
            NativeTypeDoc doc;
            doc.full_name = u8_safe(info.name);
            const std::size_t dot = doc.full_name.rfind('.');
            doc.short_name = dot == StringView::npos ? doc.full_name : u8_lit(doc.full_name.slice(dot + 1));
            doc.description = u8_safe(info.description);
            doc.kind = info.kind;
            if (info.enumeration) {
                for (std::size_t i = 0; i < info.enumeration->values.size(); ++i) {
                    const auto& [ename, ebits] = info.enumeration->values[i];
                    String edesc = String::from_number(ebits);
                    if (i < info.enumeration->metadata.size() && !info.enumeration->metadata[i].description.empty()) {
                        const String desc_u8 = u8_safe(info.enumeration->metadata[i].description);
                        edesc = u8_cat({edesc.view(), " — ", desc_u8.view()});
                    }
                    doc.enumerators.emplace(u8_safe(ename), std::move(edesc));
                }
            }
            for (const FieldInfo& f : info.fields) {
                NativeMemberDoc m;
                m.name = u8_safe(f.name);
                m.return_type = format_value_desc(*rt, f.value);
                m.signature = u8_cat({m.return_type.view(), " ", m.name.view()});
                m.description = u8_safe(f.description);
                m.is_method = false;
                doc.members.add(std::move(m));
            }
            for (const MethodInfo& fn : info.methods) {
                NativeMemberDoc m;
                m.name = u8_safe(fn.name);
                m.return_type =
                    fn.signature.result ? format_value_desc(*rt, *fn.signature.result) : String::literal<"void">();
                m.is_method = true;
                m.description = u8_safe(fn.description);
                String sig = u8_cat({m.name.view(), "("});
                for (std::size_t p = 0; p < fn.signature.parameters.size(); ++p) {
                    if (p > 0)
                        u8_append(sig, ", ");
                    const auto& pd = fn.signature.parameters[p];
                    String ptext;
                    if (pd.passing == ParameterPassing::reference)
                        u8_append(ptext, "inout ");
                    else if (pd.passing == ParameterPassing::read_only)
                        u8_append(ptext, "in ");
                    ptext.append(format_value_desc(*rt, pd));
                    if (p < fn.parameter_names.size() && !fn.parameter_names[p].empty()) {
                        u8_append(ptext, " ");
                        ptext.append(u8_safe(fn.parameter_names[p]));
                    } else {
                        u8_append(ptext, " arg");
                        ptext.append(String::from_number(p));
                    }
                    sig.append(ptext);
                    m.params.add(std::move(ptext));
                }
                u8_append(sig, ") -> ");
                sig.append(m.return_type);
                m.signature = std::move(sig);
                doc.members.add(std::move(m));
            }
            native_types.add(std::move(doc));
        }
        rebuild_semantic_tables();
    }

    void rebuild_semantic_tables() {
        Set<String> types_set;
        Set<String> funcs_set;
        Set<String> fields_set;

        for (const auto& nt : native_types) {
            if (nt.kind == TypeKind::object || nt.kind == TypeKind::record || nt.kind == TypeKind::enumeration) {
                types_set.add(nt.short_name);
                types_set.add(nt.full_name);
            }
            for (const auto& m : nt.members) {
                if (m.is_method)
                    funcs_set.add(m.name);
                else
                    fields_set.add(m.name);
            }
        }
        for (const auto& intr : kMathIntrinsics)
            funcs_set.add(u8_lit(intr.name));

        if (last_source_module) {
            for (const auto& decl : last_source_module->declarations) {
                const String uname = u8_safe(decl.name);
                const std::size_t dot = uname.rfind('.');
                const String short_u = dot == StringView::npos ? uname : u8_lit(uname.slice(dot + 1));
                if (decl.kind == language::TopLevel::Kind::type) {
                    types_set.add(uname);
                    types_set.add(short_u);
                } else if (decl.kind == language::TopLevel::Kind::function) {
                    funcs_set.add(uname);
                    funcs_set.add(short_u);
                } else {
                    fields_set.add(short_u);
                }
            }
            for (const auto& cls : last_source_module->classes) {
                const String uname = u8_safe(cls.name);
                const std::size_t dot = uname.rfind('.');
                types_set.add(uname);
                if (dot != StringView::npos)
                    types_set.add(u8_lit(uname.slice(dot + 1)));
                for (const auto& f : cls.fields)
                    fields_set.add(u8_safe(f.name));
                for (const auto& m : cls.methods)
                    funcs_set.add(u8_safe(m.name));
            }
        }
        if (!last_ui_declarations.empty()) {
            for (const auto& d : last_ui_declarations)
                types_set.add(u8_safe(d.name));
            funcs_set.add(String::literal<"choose">());
        }

        semantic_types = to_sorted_array(types_set);
        semantic_functions = to_sorted_array(funcs_set);
        semantic_fields = to_sorted_array(fields_set);
        ++generation;
    }

    static bool paths_equivalent(const std::filesystem::path& a, const std::filesystem::path& b) noexcept {
        if (a.empty() || b.empty())
            return false;
        if (platform::paths_lexically_equal(a, b))
            return true;
        if (!platform::paths_lexically_equal(a.filename(), b.filename()))
            return false;
        std::error_code ec;
        return std::filesystem::equivalent(a, b, ec) && !ec;
    }

    static std::uint32_t count_lines_up_to(StringView text, std::size_t offset) noexcept {
        std::uint32_t line = 1;
        const std::size_t lim = std::min(offset, text.size());
        for (std::size_t i = 0; i < lim; ++i)
            if (text[i] == '\n')
                ++line;
        return line;
    }

    static std::size_t find_next_code_brace(StringView text, std::size_t start_offset,
                                            bool allow_line_comments = true) noexcept {
        bool in_str = false;
        bool in_line_comment = false;
        bool in_block_comment = false;
        for (std::size_t i = start_offset; i < text.size(); ++i) {
            const char c = text[i];
            const char nx = i + 1 < text.size() ? text[i + 1] : '\0';
            if (in_line_comment) {
                if (c == '\n')
                    in_line_comment = false;
                continue;
            }
            if (in_block_comment) {
                if (c == '*' && nx == '/') {
                    in_block_comment = false;
                    ++i;
                }
                continue;
            }
            if (in_str) {
                if (c == '\\' && i + 1 < text.size())
                    ++i;
                else if (c == '"')
                    in_str = false;
                continue;
            }
            if (allow_line_comments && c == '/' && nx == '/') {
                in_line_comment = true;
                ++i;
                continue;
            }
            if (c == '/' && nx == '*') {
                in_block_comment = true;
                ++i;
                continue;
            }
            if (c == '"') {
                in_str = true;
                continue;
            }
            if (c == '{')
                return i;
        }
        return StringView::npos;
    }

    static std::size_t skip_brace_block(StringView text, std::size_t open_brace,
                                        bool allow_line_comments = true) noexcept {
        int depth = 0;
        bool in_str = false;
        bool in_line_comment = false;
        bool in_block_comment = false;
        for (std::size_t i = open_brace; i < text.size(); ++i) {
            const char c = text[i];
            const char nx = i + 1 < text.size() ? text[i + 1] : '\0';
            if (in_line_comment) {
                if (c == '\n')
                    in_line_comment = false;
                continue;
            }
            if (in_block_comment) {
                if (c == '*' && nx == '/') {
                    in_block_comment = false;
                    ++i;
                }
                continue;
            }
            if (in_str) {
                if (c == '\\' && i + 1 < text.size())
                    ++i;
                else if (c == '"')
                    in_str = false;
                continue;
            }
            if (allow_line_comments && c == '/' && nx == '/') {
                in_line_comment = true;
                ++i;
                continue;
            }
            if (c == '/' && nx == '*') {
                in_block_comment = true;
                ++i;
                continue;
            }
            if (c == '"') {
                in_str = true;
                continue;
            }
            if (c == '{')
                ++depth;
            else if (c == '}') {
                --depth;
                if (depth <= 0)
                    return i + 1;
            }
        }
        return text.size();
    }

    static std::uint32_t find_closing_brace_line(StringView text, std::size_t start_offset,
                                                 bool allow_line_comments = true) noexcept {
        const std::size_t pos = find_next_code_brace(text, start_offset, allow_line_comments);
        if (pos == StringView::npos)
            return count_lines_up_to(text, start_offset);
        const std::size_t after = skip_brace_block(text, pos, allow_line_comments);
        return count_lines_up_to(text, after > 0 && after <= text.size() ? after - 1 : text.size());
    }

    void clear_per_file_caches() {
        last_ui_declarations.clear();
        last_ui_locations.clear();
        last_ui_module.reset();
        last_ss_tokens.clear();
        last_ss_classes.clear();
    }

    void add_diagnostic_for_file(const ui::CodeDocument* active_doc, const std::filesystem::path& active_file_path,
                                 const std::filesystem::path& diag_path, String file_label, std::size_t line_1b,
                                 std::size_t utf8_col_1b, String message,
                                 ui::CodeDiagnosticSeverity severity = ui::CodeDiagnosticSeverity::error) {
        const std::size_t safe_line = line_1b > 0 ? line_1b : 1;
        std::size_t col_1b = utf8_col_1b > 0 ? utf8_col_1b : 1;
        const bool is_active = active_doc && (diag_path.empty() || paths_equivalent(diag_path, active_file_path));
        if (is_active) {
            const auto& edit = active_doc->edit();
            const auto row = static_cast<std::uint32_t>(std::min<std::size_t>(safe_line - 1, edit.line_count() - 1));
            const StringView line_u8 = edit.line(row);
            const std::size_t clamped_byte =
                std::min<std::size_t>(utf8_col_1b > 0 ? utf8_col_1b - 1 : 0, line_u8.size());
            const auto col0 = static_cast<std::uint32_t>(line_u8.utf8_prefix(clamped_byte).size());
            const auto [span_s, span_e] = diagnostic_token_span(line_u8, col0);
            col_1b = static_cast<std::size_t>(col0) + 1;
            ui::CodeDiagnostic cd;
            cd.line = row;
            cd.col_start = span_s;
            cd.col_end = span_e;
            cd.severity = severity;
            cd.message = message;
            if (!active_diagnostics.contains(cd))
                active_diagnostics.add(std::move(cd));
        }
        CodeProblemItem item;
        item.severity = severity;
        item.path = diag_path.empty() ? active_file_path : diag_path;
        item.file_label = file_label.empty() ? filename_or_fallback(item.path) : std::move(file_label);
        item.line = safe_line;
        item.column = col_1b;
        item.message = std::move(message);
        if (!problems.contains(item))
            problems.add(std::move(item));
    }

    void validate_source(const Project* project, const ProjectBuild* build, Span<const OpenSourceView> open_files,
                         const OpenSourceView& active) {
        if (!ensure_scratch_source())
            return;
        ensure_engine_files();

        source::SourceSnapshot snapshot;
        if (build && build->sources) {
            snapshot = *build->sources;
        } else if (project) {
            if (!platform::paths_lexically_equal(cached_project_root, project->root()) ||
                cached_project_revision != project->revision() || !cached_base_snapshot) {
                cached_project_root = project->root();
                cached_project_revision = project->revision();
                if (auto snap = ProjectCode::snapshot(*project))
                    cached_base_snapshot = std::make_shared<const source::SourceSnapshot>(std::move(*snap));
            }
            if (cached_base_snapshot)
                snapshot = *cached_base_snapshot;
        } else if (cached_base_snapshot && !cached_project_root.empty()) {
            if (platform::is_subpath_of(active.path, cached_project_root))
                snapshot = *cached_base_snapshot;
        }
        if (snapshot.files.empty()) {
            snapshot.name = "Authoring"_s;
            snapshot.version = 1;
            snapshot.files = cached_engine_files;
        }

        // Overlay all open .source buffers directly from UTF-8 CodeDocument without transcoding
        auto overlay_one = [&](const OpenSourceView& view) {
            if (!view.document || view.document->language() != ui::CodeLanguage::source)
                return;
            const String& utf8 = view.document->text();
            auto disk_u8 = platform::generic_path_to_utf8(view.path.lexically_normal());
            for (auto& sf : snapshot.files) {
                if (!sf.disk_path.empty()) {
                    const std::filesystem::path sf_disk = path_from_utf8(sf.disk_path);
                    if (paths_equivalent(sf_disk, view.path)) {
                        sf.source = utf8;
                        sf.version = source::content_hash(sf.source);
                        return;
                    }
                }
            }
            String rel_name = "draft.source"_s;
            if (project && platform::is_subpath_of(view.path, project->root())) {
                const auto rel = view.path.lexically_normal().lexically_relative(project->root().lexically_normal());
                if (auto ru8 = platform::generic_path_to_utf8(rel))
                    rel_name = std::move(*ru8);
            } else if (auto fu8 = platform::filename_utf8(view.path)) {
                rel_name = std::move(*fu8);
            }
            snapshot.files.add({std::move(rel_name), source::content_hash(utf8), utf8,
                                disk_u8 ? std::move(*disk_u8) : String{}});
        };

        bool active_overlaid = false;
        for (const auto& ov : open_files) {
            overlay_one(ov);
            if (ov.document == active.document || paths_equivalent(ov.path, active.path))
                active_overlaid = true;
        }
        if (!active_overlaid)
            overlay_one(active);

        std::uint64_t snap_ver = 0;
        for (const auto& sf : snapshot.files)
            snap_ver = hash_fields(snap_ver, Hash<StringView>{}(sf.path.view()),
                                   Hash<StringView>{}(sf.source.view()));
        snapshot.version = snap_ver ? snap_ver : 1;

        Optional<std::uint32_t> active_file_idx;
        for (std::uint32_t i = 0; i < snapshot.files.size(); ++i) {
            if (!snapshot.files[i].disk_path.empty() &&
                paths_equivalent(path_from_utf8(snapshot.files[i].disk_path), active.path)) {
                active_file_idx.emplace(i);
                break;
            }
        }

        auto collected = language::collect_module(snapshot);
        if (!collected) {
            const auto& mdiag = collected.error();
            std::filesystem::path err_path;
            String err_label;
            if (mdiag.file < snapshot.files.size()) {
                const auto& sf = snapshot.files[mdiag.file];
                err_label = u8_safe(sf.path);
                if (!sf.disk_path.empty())
                    err_path = path_from_utf8(sf.disk_path);
            }
            add_diagnostic_for_file(active.document, active.path, err_path, std::move(err_label),
                                    mdiag.diagnostic.position.line, mdiag.diagnostic.position.column,
                                    sanitize_diag_utf8(mdiag.diagnostic.message));
        } else {
            last_source_module.emplace(std::move(*collected));
            last_snapshot.emplace(std::move(snapshot));
            last_source_declarations.clear();
            for (const auto& sf : last_snapshot->files) {
                if (auto decls = language::collect_declarations(sf.source.view().std_view())) {
                    for (auto& td : *decls)
                        last_source_declarations.add(std::move(td));
                }
            }
            rebuild_semantic_tables();

            auto req = scratch_program->request(*last_snapshot, {.fast = true});
            Optional<Error> compile_err;
            if (!req)
                compile_err.emplace(std::move(req.error()));
            else if (auto cand = source::Program::prepare(*req); !cand)
                compile_err.emplace(std::move(cand.error()));

            if (compile_err) {
                std::filesystem::path err_path;
                String err_label;
                std::size_t ln = 1;
                std::size_t col = 1;
                if (compile_err->source) {
                    ln = compile_err->source->line;
                    col = compile_err->source->column;
                    err_label = u8_safe(compile_err->source->path);
                    if (!compile_err->source->disk_path.empty())
                        err_path = path_from_utf8(compile_err->source->disk_path);
                    else if (project) {
                        if (auto resolved = project->resolve(path_from_utf8(compile_err->source->path)))
                            err_path = *resolved;
                    }
                }
                const StringView clean_msg = strip_location_prefix(
                    compile_err->message, compile_err->source ? compile_err->source->path.view() : StringView{}, &ln,
                    &col);
                add_diagnostic_for_file(active.document, active.path, err_path, std::move(err_label), ln, col,
                                        sanitize_diag_utf8(clean_msg));
            }
        }

        // Rebuild active file outline from last_source_module
        outline.clear();
        if (last_source_module && last_snapshot) {
            Optional<std::uint32_t> outline_idx = active_file_idx;
            if (!outline_idx) {
                for (std::uint32_t i = 0; i < last_snapshot->files.size(); ++i) {
                    if (!last_snapshot->files[i].disk_path.empty() &&
                        paths_equivalent(path_from_utf8(last_snapshot->files[i].disk_path), active.path)) {
                        outline_idx.emplace(i);
                        break;
                    }
                }
            }
            if (outline_idx && *outline_idx < last_snapshot->files.size()) {
                const StringView file_text = last_snapshot->files[*outline_idx].source;
                for (const auto& decl : last_source_module->declarations) {
                    if (decl.file != *outline_idx)
                        continue;
                    DocumentSymbol sym;
                    sym.name = u8_safe(decl.name);
                    sym.detail = decl.kind == language::TopLevel::Kind::type       ? String::literal<"type">()
                                 : decl.kind == language::TopLevel::Kind::function ? String::literal<"fn">()
                                                                                   : String::literal<"const">();
                    sym.icon = decl.kind == language::TopLevel::Kind::type ? ui::ControlIcon::object
                                                                           : ui::ControlIcon::component;
                    sym.line = static_cast<std::uint32_t>(std::max<std::size_t>(1, decl.position.line));
                    sym.column = static_cast<std::uint32_t>(std::max<std::size_t>(1, decl.position.column));
                    sym.end_line = find_closing_brace_line(file_text, decl.position.offset);
                    outline.add(std::move(sym));
                }
                for (const auto& cls : last_source_module->classes) {
                    if (cls.file != *outline_idx)
                        continue;
                    const String base_u8 = u8_safe(cls.base);
                    DocumentSymbol csym;
                    csym.name = u8_safe(cls.name);
                    csym.detail = u8_cat({": ", base_u8.view()});
                    csym.icon = ui::ControlIcon::component;
                    csym.line = static_cast<std::uint32_t>(std::max<std::size_t>(1, cls.position.line));
                    csym.column = static_cast<std::uint32_t>(std::max<std::size_t>(1, cls.position.column));
                    csym.end_line = find_closing_brace_line(file_text, cls.position.offset);
                    outline.add(std::move(csym));
                    for (const auto& m : cls.methods) {
                        const String mname = u8_safe(m.name);
                        DocumentSymbol msym;
                        msym.name = u8_cat({mname.view(), "()"});
                        msym.detail = u8_safe(m.result_type.name);
                        msym.icon = ui::ControlIcon::play;
                        msym.line = static_cast<std::uint32_t>(std::max<std::size_t>(1, m.position.line));
                        msym.column = static_cast<std::uint32_t>(std::max<std::size_t>(1, m.position.column));
                        msym.end_line = find_closing_brace_line(file_text, m.position.offset);
                        msym.is_member = true;
                        outline.add(std::move(msym));
                    }
                }
                std::ranges::sort(outline, [](const DocumentSymbol& a, const DocumentSymbol& b) {
                    return a.line != b.line ? a.line < b.line : a.column < b.column;
                });
            }
        }
    }

    void scan_ui_locations(StringView text) {
        last_ui_locations.clear();
        std::size_t pos = 0;
        while (pos < text.size()) {
            if (text[pos] == '/' && pos + 1 < text.size() && text[pos + 1] == '/') {
                while (pos < text.size() && text[pos] != '\n')
                    ++pos;
                continue;
            }
            if (text[pos] == '/' && pos + 1 < text.size() && text[pos + 1] == '*') {
                pos += 2;
                while (pos + 1 < text.size() && !(text[pos] == '*' && text[pos + 1] == '/'))
                    ++pos;
                pos += 2;
                continue;
            }
            if (text[pos] == '"') {
                ++pos;
                while (pos < text.size() && text[pos] != '"') {
                    if (text[pos] == '\\' && pos + 1 < text.size())
                        ++pos;
                    ++pos;
                }
                ++pos;
                continue;
            }
            if ((pos == 0 || !is_ascii_alnum(text[pos - 1])) && text.slice(pos).starts_with("ui") &&
                pos + 2 < text.size() && (text[pos + 2] == ' ' || text[pos + 2] == '\t')) {
                const std::size_t kw_pos = pos;
                pos += 2;
                while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t'))
                    ++pos;
                const std::size_t name_start = pos;
                while (pos < text.size() && is_ascii_ident_continue(text[pos]))
                    ++pos;
                if (pos > name_start) {
                    UiDeclLocation loc;
                    loc.name = u8_safe(text.slice(name_start, pos - name_start));
                    loc.line = count_lines_up_to(text, kw_pos);
                    std::size_t line_begin = text.rfind('\n', kw_pos);
                    line_begin = line_begin == StringView::npos ? 0 : line_begin + 1;
                    loc.column = static_cast<std::uint32_t>(name_start - line_begin + 1);
                    loc.end_line = find_closing_brace_line(text, pos);
                    last_ui_locations.add(std::move(loc));
                }
                continue;
            }
            ++pos;
        }
    }

    void validate_ui_source(const OpenSourceView& active) {
        if (!active.document)
            return;
        const StringView utf8 = active.document->text().view();
        const String fname = filename_or_fallback(active.path, "draft.ui.source");
        const StringView source_name = fname.view();

        scan_ui_locations(utf8);
        auto lowered = ui_language::lower_module(source_name, utf8);
        if (!lowered) {
            const auto& diag = lowered.error();
            const StringView clean = strip_location_prefix(StringView{diag.message}, source_name);
            add_diagnostic_for_file(active.document, active.path, active.path, fname, diag.position.line,
                                    diag.position.column, sanitize_diag_utf8(clean));
        } else {
            last_ui_declarations = std::move(*lowered);
            rebuild_semantic_tables();
            if (ensure_scratch_ui()) {
                auto compiled =
                    ui_language::Module::compile(*scratch_ui_runtime, source_name, utf8, last_ui_module.get());
                if (compiled) {
                    last_ui_module = std::move(*compiled);
                } else {
                    std::size_t ln = 1;
                    std::size_t col = 1;
                    if (compiled.error().source) {
                        ln = compiled.error().source->line;
                        col = compiled.error().source->column;
                    }
                    const StringView clean = strip_location_prefix(compiled.error().message, source_name, &ln, &col);
                    add_diagnostic_for_file(active.document, active.path, active.path, fname, ln, col,
                                            sanitize_diag_utf8(clean));
                }
            }
        }

        outline.clear();
        for (const auto& loc : last_ui_locations) {
            DocumentSymbol sym;
            sym.name = u8_cat({"ui ", loc.name.view()});
            for (const auto& decl : last_ui_declarations) {
                if (loc.name == decl.name.view()) {
                    const String in_cnt = String::from_number(decl.inputs.size());
                    const String nd_cnt = String::from_number(decl.nodes.size());
                    sym.detail = u8_cat({in_cnt.view(), " in · ", nd_cnt.view(), " nodes"});
                    break;
                }
            }
            sym.icon = ui::ControlIcon::view_tiles;
            sym.line = loc.line;
            sym.column = loc.column;
            sym.end_line = loc.end_line;
            outline.add(std::move(sym));
        }
    }

    void validate_style_sheet(const OpenSourceView& active) {
        if (!active.document)
            return;
        const StringView text = active.document->text().view();

        outline.clear();
        Array<SsTokenEntry> tokens;
        Set<String> classes;
        std::size_t pos = 0;
        while (pos < text.size()) {
            if (text[pos] == '/' && pos + 1 < text.size() && text[pos + 1] == '*') {
                pos += 2;
                while (pos + 1 < text.size() && !(text[pos] == '*' && text[pos + 1] == '/'))
                    ++pos;
                pos += 2;
                continue;
            }
            if (text.slice(pos).starts_with("@tokens")) {
                const std::uint32_t start_ln = count_lines_up_to(text, pos);
                const std::uint32_t end_ln = find_closing_brace_line(text, pos, false);
                outline.add({String::literal<"@tokens">(), String::literal<"Design Tokens">(), ui::ControlIcon::sliders,
                             start_ln, 1u, end_ln, false});
                const std::size_t brace = find_next_code_brace(text, pos, false);
                if (brace != StringView::npos) {
                    const std::size_t block_end = skip_brace_block(text, brace, false);
                    const std::size_t content_limit =
                        block_end > brace && text[block_end - 1] == '}' ? block_end - 1 : block_end;
                    std::size_t cur = brace + 1;
                    while (cur < content_limit) {
                        while (cur < content_limit &&
                               (text[cur] == ' ' || text[cur] == '\t' || text[cur] == '\r' || text[cur] == '\n'))
                            ++cur;
                        if (cur + 1 < content_limit && text[cur] == '/' && text[cur + 1] == '*') {
                            cur += 2;
                            while (cur + 1 < content_limit && !(text[cur] == '*' && text[cur + 1] == '/'))
                                ++cur;
                            cur = std::min(content_limit, cur + 2);
                            continue;
                        }
                        if (cur >= content_limit)
                            break;
                        const std::size_t t_start = cur;
                        while (cur < content_limit && text[cur] != ':' && text[cur] != '\n')
                            ++cur;
                        if (cur < content_limit && text[cur] == ':') {
                            StringView tname = text.slice(t_start, cur - t_start);
                            while (!tname.empty() && (tname.back() == ' ' || tname.back() == '\t'))
                                tname.remove_suffix(1);
                            ++cur;
                            const std::size_t v_start = cur;
                            while (cur < content_limit && text[cur] != ';') {
                                if (cur + 1 < content_limit && text[cur] == '/' && text[cur + 1] == '*') {
                                    cur += 2;
                                    while (cur + 1 < content_limit && !(text[cur] == '*' && text[cur + 1] == '/'))
                                        ++cur;
                                    cur = std::min(content_limit, cur + 2);
                                    continue;
                                }
                                ++cur;
                            }
                            StringView tval = text.slice(v_start, cur - v_start);
                            while (!tval.empty() && (tval.front() == ' ' || tval.front() == '\t'))
                                tval.remove_prefix(1);
                            while (!tval.empty() && (tval.back() == ' ' || tval.back() == '\t'))
                                tval.remove_suffix(1);
                            if (cur < content_limit && text[cur] == ';')
                                ++cur;
                            if (!tname.empty()) {
                                const std::uint32_t ln = count_lines_up_to(text, t_start);
                                std::size_t lb = text.rfind('\n', t_start);
                                lb = lb == StringView::npos ? 0 : lb + 1;
                                tokens.add({u8_safe(tname), u8_safe(tval), ln, t_start - lb + 1});
                            }
                        } else {
                            ++cur;
                        }
                    }
                    pos = block_end;
                    continue;
                }
            }
            if (text[pos] == '.') {
                const std::size_t c_start = pos + 1;
                std::size_t c_end = c_start;
                while (c_end < text.size() && ui::is_code_ident_char(text[c_end], ui::CodeLanguage::style_sheet))
                    ++c_end;
                if (c_end > c_start)
                    classes.add(u8_safe(text.slice(c_start, c_end - c_start)));
            }
            if (text[pos] != ' ' && text[pos] != '\t' && text[pos] != '\r' && text[pos] != '\n' && text[pos] != '}') {
                const std::size_t sel_start = pos;
                const std::size_t brace = find_next_code_brace(text, pos, false);
                if (brace != StringView::npos) {
                    StringView sel = text.slice(sel_start, brace - sel_start);
                    while (!sel.empty() &&
                           (sel.back() == ' ' || sel.back() == '\t' || sel.back() == '\r' || sel.back() == '\n'))
                        sel.remove_suffix(1);
                    // Also collect any additional `.class` selectors between sel_start and `{`
                    for (std::size_t si = sel_start + 1; si < brace; ++si) {
                        if (si + 1 < brace && text[si] == '/' && text[si + 1] == '*') {
                            si += 2;
                            while (si + 1 < brace && !(text[si] == '*' && text[si + 1] == '/'))
                                ++si;
                            ++si;
                            continue;
                        }
                        if (text[si] == '.') {
                            const std::size_t cs = si + 1;
                            std::size_t ce = cs;
                            while (ce < brace && ui::is_code_ident_char(text[ce], ui::CodeLanguage::style_sheet))
                                ++ce;
                            if (ce > cs)
                                classes.add(u8_safe(text.slice(cs, ce - cs)));
                            si = ce - 1;
                        }
                    }
                    const std::uint32_t s_ln = count_lines_up_to(text, sel_start);
                    const std::uint32_t e_ln = find_closing_brace_line(text, brace, false);
                    if (!sel.empty() && sel.size() <= 72 && outline.size() < 96) {
                        Array<char> clean_sel;
                        clean_sel.reserve(sel.size());
                        for (const char ch : sel)
                            clean_sel.add((ch == '\n' || ch == '\r') ? ' ' : ch);
                        outline.add({u8_safe({clean_sel.data(), clean_sel.size()}), String::literal<"Rule">(),
                                     ui::ControlIcon::material, s_ln, 1u, e_ln, false});
                    }
                    pos = skip_brace_block(text, brace, false);
                    continue;
                }
            }
            ++pos;
        }
        auto parsed = ui::StyleSheet::parse(text);
        if (parsed || !tokens.empty())
            last_ss_tokens = std::move(tokens);
        if (parsed || !classes.empty())
            last_ss_classes = to_sorted_array(classes);
        if (!parsed) {
            const auto& diag = parsed.error();
            add_diagnostic_for_file(active.document, active.path, active.path, filename_or_fallback(active.path),
                                    diag.line, diag.column, sanitize_diag_utf8(diag.message));
        }
    }

    void validate_json(const OpenSourceView& active) {
        if (!active.document)
            return;
        outline.clear();
        const StringView utf8 = active.document->text().view();
        auto parsed = json::Document::parse(utf8, {64, json::DepthPolicy::containers, true});
        if (!parsed) {
            const auto& err = parsed.error();
            add_diagnostic_for_file(active.document, active.path, active.path, filename_or_fallback(active.path),
                                    err.line ? err.line : 1, err.column ? err.column : 1,
                                    sanitize_diag_utf8(err.message));
        }
    }
};

CodeIntelligence::CodeIntelligence() : impl_(std::make_unique<Impl>()) {}

CodeIntelligence::~CodeIntelligence() = default;

void CodeIntelligence::reset() {
    impl_->active_path.clear();
    impl_->active_language = ui::CodeLanguage::plain;
    impl_->seen_revision = std::numeric_limits<std::uint64_t>::max();
    impl_->checked_revision = std::numeric_limits<std::uint64_t>::max();
    impl_->pending_validation = false;
    impl_->active_diagnostics.clear();
    impl_->problems.clear();
    impl_->outline.clear();
    impl_->clear_per_file_caches();
    impl_->last_source_module.reset();
    impl_->last_snapshot.reset();
    impl_->last_source_declarations.clear();
    impl_->semantic_types.clear();
    impl_->semantic_functions.clear();
    impl_->semantic_fields.clear();
    impl_->cached_base_snapshot.reset();
    impl_->cached_project_root.clear();
    ++impl_->generation;
}

bool CodeIntelligence::sync(const Runtime* world_runtime, const Project* project, const ProjectBuild* build,
                            Span<const OpenSourceView> open_files, const OpenSourceView* active_file,
                            bool force_immediate) {
    impl_->refresh_native_catalog(world_runtime);

    if (!active_file || !active_file->document) {
        const bool had = !impl_->active_diagnostics.empty() || !impl_->problems.empty() || !impl_->outline.empty() ||
                         !impl_->last_ui_declarations.empty() || !impl_->last_ss_tokens.empty();
        impl_->active_path.clear();
        impl_->active_language = ui::CodeLanguage::plain;
        impl_->active_diagnostics.clear();
        impl_->problems.clear();
        impl_->outline.clear();
        impl_->clear_per_file_caches();
        impl_->pending_validation = false;
        if (had)
            ++impl_->generation;
        return had;
    }

    const auto now = Impl::Clock::now();
    const ui::CodeLanguage lang = active_file->document->language();
    const std::uint64_t rev = active_file->document->revision();

    std::uint64_t open_digest = 0;
    for (const auto& ov : open_files) {
        if (ov.document)
            open_digest = hash_fields(open_digest, ov.document->revision());
    }
    if (build)
        open_digest = hash_fields(open_digest, build->published_version);

    const bool file_switched =
        !Impl::paths_equivalent(impl_->active_path, active_file->path) || impl_->active_language != lang;
    if (file_switched) {
        const bool left_ui_source =
            impl_->active_language == ui::CodeLanguage::ui_source && lang != ui::CodeLanguage::ui_source;
        impl_->active_path = active_file->path;
        impl_->active_language = lang;
        impl_->seen_revision = rev;
        impl_->checked_revision = std::numeric_limits<std::uint64_t>::max();
        impl_->clear_per_file_caches();
        impl_->outline.clear();
        if (!project && !impl_->cached_project_root.empty() &&
            !platform::is_subpath_of(active_file->path, impl_->cached_project_root)) {
            impl_->last_source_module.reset();
            impl_->last_snapshot.reset();
            impl_->last_source_declarations.clear();
        }
        if (left_ui_source)
            impl_->rebuild_semantic_tables();
        impl_->pending_validation = true;
        force_immediate = true;
    } else if (rev != impl_->seen_revision || open_digest != impl_->checked_open_digest) {
        impl_->seen_revision = rev;
        impl_->last_edit_time = now;
        impl_->pending_validation = true;
    }

    if (!impl_->pending_validation && rev == impl_->checked_revision && open_digest == impl_->checked_open_digest &&
        !force_immediate) {
        return false;
    }

    constexpr auto kDebounce = std::chrono::milliseconds(160);
    if (!force_immediate && (now - impl_->last_edit_time) < kDebounce)
        return false;

    impl_->pending_validation = false;
    impl_->checked_revision = rev;
    impl_->checked_open_digest = open_digest;
    impl_->active_diagnostics.clear();
    impl_->problems.clear();

    switch (lang) {
    case ui::CodeLanguage::source:
        impl_->validate_source(project, build, open_files, *active_file);
        break;
    case ui::CodeLanguage::ui_source:
        impl_->validate_ui_source(*active_file);
        break;
    case ui::CodeLanguage::style_sheet:
        impl_->validate_style_sheet(*active_file);
        break;
    case ui::CodeLanguage::json:
        impl_->validate_json(*active_file);
        break;
    case ui::CodeLanguage::plain:
        impl_->outline.clear();
        break;
    }

    if (build && build->error && build->error->source && project) {
        const auto& loc = *build->error->source;
        const std::filesystem::path rel_path = path_from_utf8(loc.path);
        if (auto resolved = project->resolve(rel_path)) {
            const bool already_open_dirty = std::ranges::any_of(open_files, [&](const OpenSourceView& ov) {
                return ov.dirty && Impl::paths_equivalent(ov.path, *resolved);
            });
            if (!already_open_dirty) {
                const StringView clean = strip_location_prefix(build->error->message, loc.path);
                impl_->add_diagnostic_for_file(
                    Impl::paths_equivalent(*resolved, active_file->path) ? active_file->document : nullptr,
                    active_file->path, *resolved, u8_safe(loc.path), loc.line, loc.column, sanitize_diag_utf8(clean));
            }
        }
    }

    ++impl_->generation;
    return true;
}

bool CodeIntelligence::check_pending() const noexcept {
    return impl_->pending_validation;
}

std::uint64_t CodeIntelligence::generation() const noexcept {
    return impl_->generation;
}

Span<const ui::CodeDiagnostic> CodeIntelligence::active_diagnostics() const noexcept {
    return impl_->active_diagnostics.view();
}

Span<const CodeProblemItem> CodeIntelligence::problems() const noexcept {
    return impl_->problems.view();
}

Span<const DocumentSymbol> CodeIntelligence::outline() const noexcept {
    return impl_->outline.view();
}

String CodeIntelligence::breadcrumb_for_line(std::size_t zero_based_line) const {
    const auto ln = static_cast<std::uint32_t>(zero_based_line + 1);
    String container;
    String member;
    for (const auto& sym : impl_->outline) {
        if (ln >= sym.line && ln <= sym.end_line) {
            if (sym.is_member)
                member = sym.name;
            else
                container = sym.name;
        }
    }
    if (!container.empty() && !member.empty())
        return u8_cat({container.view(), "  ›  ", member.view()});
    if (!container.empty())
        return container;
    return member;
}

Span<const String> CodeIntelligence::semantic_types() const noexcept {
    return impl_->semantic_types.view();
}

Span<const String> CodeIntelligence::semantic_functions() const noexcept {
    return impl_->semantic_functions.view();
}

Span<const String> CodeIntelligence::semantic_fields() const noexcept {
    return impl_->semantic_fields.view();
}

namespace {
struct EnclosingSourceContext {
    String class_name;

    String base_name;

    String method_name;

    Map<String, String> local_types;
};

EnclosingSourceContext resolve_source_context(StringView doc_text, std::uint32_t caret_offset,
                                              const Optional<language::SourceModule>& mod) {
    EnclosingSourceContext ctx;
    const std::size_t lim = std::min<std::size_t>(caret_offset, doc_text.size());
    std::size_t line_1b = 1;
    for (std::size_t i = 0; i < lim; ++i)
        if (doc_text[i] == '\n')
            ++line_1b;

    if (mod) {
        for (const auto& cls : mod->classes) {
            if (cls.position.line <= line_1b) {
                ctx.class_name = u8_safe(cls.name);
                ctx.base_name = u8_safe(cls.base);
                ctx.method_name.clear();
                ctx.local_types.clear();
                for (const auto& m : cls.methods) {
                    if (m.position.line <= line_1b) {
                        ctx.method_name = u8_safe(m.name);
                        ctx.local_types.clear();
                        for (const auto& p : m.parameters)
                            ctx.local_types.insert_or_assign(u8_safe(p.name), format_source_type_name(p.type));
                    }
                }
            }
        }
    }

    std::size_t window_start = lim > 4096 ? lim - 4096 : 0;
    while (window_start < lim && (static_cast<unsigned char>(doc_text[window_start]) & 0xc0u) == 0x80u)
        ++window_start;
    const StringView win = doc_text.slice(window_start, lim - window_start);

    if (ctx.class_name.empty()) {
        const std::size_t cpos = win.rfind("class ");
        if (cpos != StringView::npos) {
            std::size_t i = cpos + 6;
            while (i < win.size() && win[i] == ' ')
                ++i;
            const std::size_t ns = i;
            while (i < win.size() && (is_ascii_ident_continue(win[i]) || win[i] == '.'))
                ++i;
            ctx.class_name = u8_lit(win.slice(ns, i - ns));
            while (i < win.size() && win[i] == ' ')
                ++i;
            if (i < win.size() && win[i] == ':') {
                ++i;
                while (i < win.size() && win[i] == ' ')
                    ++i;
                const std::size_t bs = i;
                while (i < win.size() && (is_ascii_ident_continue(win[i]) || win[i] == '.'))
                    ++i;
                ctx.base_name = u8_lit(win.slice(bs, i - bs));
            }
        }
    }

    std::size_t ls = 0;
    while (ls < win.size()) {
        std::size_t le = win.find('\n', ls);
        if (le == StringView::npos)
            le = win.size();
        StringView ln = win.slice(ls, le - ls);
        ls = le + 1;
        while (!ln.empty() && (ln.front() == ' ' || ln.front() == '\t'))
            ln.remove_prefix(1);
        if (ln.starts_with("const "))
            ln.remove_prefix(6);
        while (!ln.empty() && ln.front() == ' ')
            ln.remove_prefix(1);
        std::size_t ti = 0;
        int angle = 0;
        while (ti < ln.size()) {
            const char tc = ln[ti];
            if (tc == '<')
                ++angle;
            else if (tc == '>' && angle > 0)
                --angle;
            else if (angle == 0 && !is_ascii_ident_continue(tc) && tc != '.')
                break;
            ++ti;
        }
        if (ti == 0 || angle != 0 || ti >= ln.size() || (ln[ti] != ' ' && ln[ti] != '&'))
            continue;
        const StringView tname = ln.slice(0, ti);
        if (ln[ti] == '&')
            ++ti;
        if (tname == "return" || tname == "if" || tname == "for" || tname == "while" || tname == "class" ||
            tname == "struct" || tname == "enum" || tname == "else")
            continue;
        while (ti < ln.size() && ln[ti] == ' ')
            ++ti;
        const std::size_t vs = ti;
        while (ti < ln.size() && is_ascii_ident_continue(ln[ti]))
            ++ti;
        if (ti > vs) {
            const StringView vname = ln.slice(vs, ti - vs);
            while (ti < ln.size() && ln[ti] == ' ')
                ++ti;
            if (ti < ln.size() && (ln[ti] == '=' || ln[ti] == ';'))
                ctx.local_types.insert_or_assign(u8_lit(vname), u8_lit(tname));
        }
    }
    return ctx;
}
} // namespace

Array<CodeCompletionItem> CodeIntelligence::completions(const ui::CodeDocument& document, std::uint32_t caret_offset,
                                                        bool explicit_request) const {
    Array<CodeCompletionItem> items;
    const StringView full_text = document.text().view();
    if (caret_offset > full_text.size())
        caret_offset = static_cast<std::uint32_t>(full_text.size());

    const auto& edit = document.edit();
    const std::size_t row = edit.line_at(caret_offset);
    const std::uint32_t line_start = edit.line_start(row);
    const StringView line_u8 = edit.line(row);
    const std::uint32_t col =
        std::min<std::uint32_t>(caret_offset - line_start, static_cast<std::uint32_t>(line_u8.size()));

    if (!explicit_request) {
        bool in_str = false;
        char q = 0;
        for (std::uint32_t i = 0; i < col; ++i) {
            if (in_str) {
                if (line_u8[i] == '\\' && i + 1 < col)
                    ++i;
                else if (line_u8[i] == q)
                    in_str = false;
            } else if (line_u8[i] == '"' || line_u8[i] == '\'') {
                in_str = true;
                q = line_u8[i];
            } else if (line_u8[i] == '/' && i + 1 < col && line_u8[i + 1] == '/') {
                return {};
            }
        }
    }

    const ui::CodeLanguage lang = document.language();
    std::uint32_t word_start_col = col;
    while (word_start_col > 0 && ui::is_code_ident_char(line_u8[word_start_col - 1], lang))
        --word_start_col;
    const StringView prefix = line_u8.slice(word_start_col, col - word_start_col);
    const std::uint32_t rep_start = line_start + word_start_col;
    const std::uint32_t rep_end = caret_offset;

    auto push_candidate = [&](StringView badge, StringView badge_cls, String label, String detail, String doc,
                              String insert_text, std::uint32_t backtrack = 0) {
        const int sc = fuzzy_match_score(label.view(), prefix);
        if (sc < 0)
            return;
        items.add({u8_lit(badge), u8_lit(badge_cls), std::move(label), std::move(detail), std::move(doc),
                   std::move(insert_text), rep_start, rep_end, backtrack, sc});
    };

    const StringView before_word = line_u8.slice(0, word_start_col);

    if (lang == ui::CodeLanguage::source) {
        bool in_attr_brackets = false;
        bool in_param_attr = false;
        {
            bool in_s = false;
            int open_sq_idx = -1;
            for (std::size_t i = 0; i < before_word.size(); ++i) {
                const char ch = before_word[i];
                if (in_s) {
                    if (ch == '\\' && i + 1 < before_word.size())
                        ++i;
                    else if (ch == '"')
                        in_s = false;
                    continue;
                }
                if (ch == '"')
                    in_s = true;
                else if (ch == '[')
                    open_sq_idx = static_cast<int>(i);
                else if (ch == ']')
                    open_sq_idx = -1;
            }
            if (!in_s && open_sq_idx >= 0) {
                std::size_t p = static_cast<std::size_t>(open_sq_idx);
                while (p > 0 && (before_word[p - 1] == ' ' || before_word[p - 1] == '\t'))
                    --p;
                if (p == 0 || before_word[p - 1] == '(' || before_word[p - 1] == ',') {
                    in_attr_brackets = true;
                    in_param_attr = p > 0;
                }
            }
        }

        if (before_word.ends_with("::")) {
            const std::size_t te = before_word.size() - 2;
            std::size_t ts = te;
            while (ts > 0 && (is_ascii_ident_continue(before_word[ts - 1]) || before_word[ts - 1] == '.'))
                --ts;
            const StringView target_enum = before_word.slice(ts, te - ts);
            for (const auto& nt : impl_->native_types) {
                if ((nt.full_name == target_enum || nt.short_name == target_enum) && !nt.enumerators.empty()) {
                    for (const auto& [ename, edesc] : nt.enumerators)
                        push_candidate("enum", "comp-badge-prop", ename, nt.full_name, edesc, ename);
                }
            }
            for (const auto& td : impl_->last_source_declarations) {
                const String utd = u8_safe(td.name);
                const std::size_t dot = utd.rfind('.');
                const StringView short_td = dot == StringView::npos ? utd.view() : utd.slice(dot + 1);
                if (td.enumeration && (utd == target_enum || short_td == target_enum)) {
                    for (std::size_t i = 0; i < td.enumerators.size(); ++i) {
                        const String en = u8_safe(td.enumerators[i].first);
                        String ed = String::from_number(td.enumerators[i].second);
                        if (i < td.enumerator_attributes.size() && !td.enumerator_attributes[i].description.empty()) {
                            const String dstr = u8_safe(td.enumerator_attributes[i].description);
                            ed = u8_cat({ed.view(), " — ", dstr.view()});
                        }
                        push_candidate("enum", "comp-badge-prop", en, utd, std::move(ed), en);
                    }
                }
            }
        } else if (before_word.ends_with('.')) {
            const auto ctx = resolve_source_context(full_text, caret_offset, impl_->last_source_module);
            const std::size_t re = before_word.size() - 1;
            std::size_t rs = re;
            while (rs > 0 && (is_ascii_ident_continue(before_word[rs - 1]) || before_word[rs - 1] == '.'))
                --rs;
            const StringView recv = before_word.slice(rs, re - rs);

            auto add_type_members = [&](StringView type_name) {
                if (type_name.empty())
                    return;
                if (impl_->last_source_module) {
                    for (const auto& cls : impl_->last_source_module->classes) {
                        const String ucls = u8_safe(cls.name);
                        const std::size_t dot = ucls.rfind('.');
                        if (ucls == type_name || (dot != StringView::npos && ucls.slice(dot + 1) == type_name)) {
                            for (const auto& f : cls.fields) {
                                const String fname = u8_safe(f.name);
                                push_candidate("fld", "comp-badge-fld", fname, u8_safe(f.type), u8_safe(f.description),
                                               fname);
                            }
                            for (const auto& m : cls.methods) {
                                const String mn = u8_safe(m.name);
                                const bool has_params = !m.parameters.empty();
                                push_candidate("fn", "comp-badge-fn", mn, format_method_signature(m), {},
                                               u8_cat({mn.view(), "()"}), has_params ? 1u : 0u);
                            }
                            if (!cls.base.empty()) {
                                const String ubase = u8_safe(cls.base);
                                for (const auto& nt : impl_->native_types) {
                                    if (nt.full_name == ubase || nt.short_name == ubase) {
                                        for (const auto& nm : nt.members) {
                                            if (nm.is_method)
                                                push_candidate("fn", "comp-badge-fn", nm.name, nm.signature,
                                                               nm.description, u8_cat({nm.name.view(), "()"}),
                                                               nm.params.empty() ? 0u : 1u);
                                            else
                                                push_candidate("fld", "comp-badge-fld", nm.name, nm.return_type,
                                                               nm.description, nm.name);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                for (const auto& td : impl_->last_source_declarations) {
                    if (td.enumeration)
                        continue;
                    const String utd = u8_safe(td.name);
                    const std::size_t dot = utd.rfind('.');
                    if (utd == type_name || (dot != StringView::npos && utd.slice(dot + 1) == type_name)) {
                        for (std::size_t fi = 0; fi < td.fields.size(); ++fi) {
                            const String fname = u8_safe(td.fields[fi].first);
                            const String ftype = format_source_type_name(td.fields[fi].second);
                            const String fdesc =
                                fi < td.field_descriptions.size() ? u8_safe(td.field_descriptions[fi]) : String{};
                            push_candidate("fld", "comp-badge-fld", fname, ftype, fdesc, fname);
                        }
                    }
                }
                for (const auto& nt : impl_->native_types) {
                    if (nt.full_name == type_name || nt.short_name == type_name) {
                        for (const auto& nm : nt.members) {
                            if (nm.is_method)
                                push_candidate("fn", "comp-badge-fn", nm.name, nm.signature, nm.description,
                                               u8_cat({nm.name.view(), "()"}), nm.params.empty() ? 0u : 1u);
                            else
                                push_candidate("fld", "comp-badge-fld", nm.name, nm.return_type, nm.description,
                                               nm.name);
                        }
                    }
                }
                const bool is_f64 = type_name.ends_with('d');
                const String lane_t = is_f64 ? String::literal<"double">() : String::literal<"float">();
                if (type_name == "vector2" || type_name == "vector2d" || type_name == "math.vector2" ||
                    type_name == "math.vector2d") {
                    push_candidate("fld", "comp-badge-fld", String::literal<"x">(), lane_t, String::literal<"X 分量">(),
                                   String::literal<"x">());
                    push_candidate("fld", "comp-badge-fld", String::literal<"y">(), lane_t, String::literal<"Y 分量">(),
                                   String::literal<"y">());
                } else if (type_name == "vector3" || type_name == "vector3d" || type_name == "math.vector3" ||
                           type_name == "math.vector3d") {
                    push_candidate("fld", "comp-badge-fld", String::literal<"x">(), lane_t, String::literal<"X 分量">(),
                                   String::literal<"x">());
                    push_candidate("fld", "comp-badge-fld", String::literal<"y">(), lane_t, String::literal<"Y 分量">(),
                                   String::literal<"y">());
                    push_candidate("fld", "comp-badge-fld", String::literal<"z">(), lane_t, String::literal<"Z 分量">(),
                                   String::literal<"z">());
                } else if (type_name == "vector4" || type_name == "vector4d" || type_name == "quaternion" ||
                           type_name == "quaterniond" || type_name == "math.vector4" || type_name == "math.vector4d" ||
                           type_name == "math.quaternion" || type_name == "math.quaterniond") {
                    push_candidate("fld", "comp-badge-fld", String::literal<"x">(), lane_t, String::literal<"X 分量">(),
                                   String::literal<"x">());
                    push_candidate("fld", "comp-badge-fld", String::literal<"y">(), lane_t, String::literal<"Y 分量">(),
                                   String::literal<"y">());
                    push_candidate("fld", "comp-badge-fld", String::literal<"z">(), lane_t, String::literal<"Z 分量">(),
                                   String::literal<"z">());
                    push_candidate("fld", "comp-badge-fld", String::literal<"w">(), lane_t, String::literal<"W 分量">(),
                                   String::literal<"w">());
                } else if (type_name == "String" || type_name.starts_with("Array<")) {
                    const bool is_arr = type_name.starts_with("Array<");
                    push_candidate(
                        "fn", "comp-badge-fn", String::literal<"size">(), String::literal<"size() -> uint">(),
                        is_arr ? String::literal<"返回数组元素个数">() : String::literal<"返回 UTF-8 字符串字节长度">(),
                        String::literal<"size()">());
                    push_candidate("fn", "comp-badge-fn", String::literal<"clear">(),
                                   String::literal<"clear() -> void">(), String::literal<"清空序列内容">(),
                                   String::literal<"clear()">());
                    push_candidate("fn", "comp-badge-fn", String::literal<"reserve">(),
                                   String::literal<"reserve(uint capacity) -> void">(),
                                   String::literal<"预分配序列容量">(), String::literal<"reserve()">(), 1u);
                    push_candidate("fn", "comp-badge-fn", String::literal<"append">(),
                                   String::literal<"append(seq) -> void">(),
                                   String::literal<"追加同类型序列全部元素">(), String::literal<"append()">(), 1u);
                    if (is_arr) {
                        push_candidate("fn", "comp-badge-fn", String::literal<"resize">(),
                                       String::literal<"resize(uint count) -> void">(),
                                       String::literal<"调整可变数组元素数量">(), String::literal<"resize()">(), 1u);
                        push_candidate("fn", "comp-badge-fn", String::literal<"add">(),
                                       String::literal<"add(T element) -> void">(),
                                       String::literal<"向可变数组尾部追加单个元素">(), String::literal<"add()">(), 1u);
                    }
                }
            };

            if (recv == "self") {
                if (!ctx.class_name.empty())
                    add_type_members(ctx.class_name.view());
                if (!ctx.base_name.empty())
                    add_type_members(ctx.base_name.view());
            } else if (recv.starts_with("self.") && impl_->last_source_module) {
                const StringView fname = recv.slice(5);
                for (const auto& cls : impl_->last_source_module->classes) {
                    if (ctx.class_name == StringView{cls.name}) {
                        for (const auto& f : cls.fields)
                            if (fname == StringView{f.name}) {
                                const String ftype = u8_safe(f.type);
                                add_type_members(ftype.view());
                            }
                    }
                }
            } else if (const String* found = ctx.local_types.find(recv)) {
                add_type_members(found->view());
            } else if (recv == "math") {
                for (const auto& intr : kMathIntrinsics) {
                    String sig = u8_cat({"math.", intr.name, "("});
                    for (std::size_t i = 0; i < intr.param_count; ++i) {
                        if (i > 0)
                            u8_append(sig, ", ");
                        u8_append(sig, intr.params[i]);
                    }
                    u8_append(sig, ") -> ");
                    u8_append(sig, intr.return_type);
                    push_candidate("fn", "comp-badge-fn", u8_lit(intr.name), std::move(sig), u8_lit(intr.doc),
                                   u8_cat({intr.name, "()"}), 1u);
                }
            } else {
                const String ns_prefix = u8_cat({recv, "."});
                for (const auto& nt : impl_->native_types) {
                    if (nt.full_name.starts_with(ns_prefix.view())) {
                        const String rem = u8_lit(nt.full_name.slice(ns_prefix.size()));
                        push_candidate("type", "comp-badge-type", rem, nt.full_name, nt.description, rem);
                    }
                }
                if (impl_->last_source_module) {
                    for (const auto& cls : impl_->last_source_module->classes) {
                        const String ucls = u8_safe(cls.name);
                        if (ucls.starts_with(ns_prefix.view())) {
                            const String rem = u8_lit(ucls.slice(ns_prefix.size()));
                            const String base_u8 = u8_safe(cls.base);
                            push_candidate("cls", "comp-badge-type", rem, u8_cat({": ", base_u8.view()}),
                                           u8_safe(cls.description), rem);
                        }
                    }
                    for (const auto& decl : impl_->last_source_module->declarations) {
                        const String ud = u8_safe(decl.name);
                        if (ud.starts_with(ns_prefix.view())) {
                            const String rem = u8_lit(ud.slice(ns_prefix.size()));
                            const bool is_fn = decl.kind == language::TopLevel::Kind::function;
                            push_candidate(is_fn ? "fn" : "type", is_fn ? "comp-badge-fn" : "comp-badge-type", rem, ud,
                                           {}, is_fn ? u8_cat({rem.view(), "()"}) : rem, is_fn ? 1u : 0u);
                        }
                    }
                }
            }
        } else if (in_attr_brackets) {
            if (in_param_attr) {
                push_candidate("attr", "comp-badge-prop", String::literal<"In">(), String::literal<"[In(\"attr\")]">(),
                               String::literal<"绑定只读几何属性列输入">(), String::literal<"In(\"\")">(), 2u);
                push_candidate("attr", "comp-badge-prop", String::literal<"Out">(),
                               String::literal<"[Out(\"attr\")]">(), String::literal<"绑定只写几何属性列输出">(),
                               String::literal<"Out(\"\")">(), 2u);
                push_candidate("attr", "comp-badge-prop", String::literal<"InOut">(),
                               String::literal<"[InOut(\"attr\")]">(), String::literal<"绑定原地读写几何属性列">(),
                               String::literal<"InOut(\"\")">(), 2u);
                push_candidate("attr", "comp-badge-prop", String::literal<"Group">(),
                               String::literal<"[Group(From = \"field\")]">(),
                               String::literal<"按字段指定的点/面组掩码过滤">(),
                               String::literal<"Group(From = \"\")">(), 2u);
                push_candidate("attr", "comp-badge-prop", String::literal<"Port">(), String::literal<"[Port]">(),
                               String::literal<"声明节点输入/输出拓扑端口">(), String::literal<"Port">());
                push_candidate("attr", "comp-badge-prop", String::literal<"Optional">(), String::literal<"Optional">(),
                               String::literal<"允许属性列缺省为空序列">(), String::literal<"Optional">());
                push_candidate("attr", "comp-badge-prop", String::literal<"From">(),
                               String::literal<"From = \"field\"">(), String::literal<"从指定字符串字段读取名称">(),
                               String::literal<"From = \"\"">(), 1u);
                push_candidate("attr", "comp-badge-prop", String::literal<"DESC">(),
                               String::literal<"DESC = \"...\"">(), String::literal<"设置参数说明文档">(),
                               String::literal<"DESC = \"\"">(), 1u);
            } else {
                push_candidate(
                    "attr", "comp-badge-prop", String::literal<"DESC">(), String::literal<"[DESC = \"...\"]">(),
                    String::literal<"设置类型、函数、字段或枚举项说明文档">(), String::literal<"DESC = \"\"">(), 1u);
                push_candidate(
                    "attr", "comp-badge-prop", String::literal<"Label">(), String::literal<"[Label = \"...\"]">(),
                    String::literal<"设置属性面板或枚举项显示名称">(), String::literal<"Label = \"\"">(), 1u);
                push_candidate("attr", "comp-badge-prop", String::literal<"Category">(),
                               String::literal<"[Category = \"...\"]">(), String::literal<"设置属性检查器分组名称">(),
                               String::literal<"Category = \"\"">(), 1u);
                push_candidate("attr", "comp-badge-prop", String::literal<"Min">(), String::literal<"Min = 0.0">(),
                               String::literal<"设置数值滑条 UI 软下界">(), String::literal<"Min = 0.0">());
                push_candidate("attr", "comp-badge-prop", String::literal<"Max">(), String::literal<"Max = 1.0">(),
                               String::literal<"设置数值滑条 UI 软上界">(), String::literal<"Max = 1.0">());
                push_candidate("attr", "comp-badge-prop", String::literal<"HardMin">(),
                               String::literal<"HardMin = 0.0">(), String::literal<"设置数值硬下界约束">(),
                               String::literal<"HardMin = 0.0">());
                push_candidate("attr", "comp-badge-prop", String::literal<"HardMax">(),
                               String::literal<"HardMax = 1.0">(), String::literal<"设置数值硬上界约束">(),
                               String::literal<"HardMax = 1.0">());
                push_candidate("attr", "comp-badge-prop", String::literal<"LockMin">(), String::literal<"LockMin">(),
                               String::literal<"将 Min 同步锁定为硬下界">(), String::literal<"LockMin">());
                push_candidate("attr", "comp-badge-prop", String::literal<"LockMax">(), String::literal<"LockMax">(),
                               String::literal<"将 Max 同步锁定为硬上界">(), String::literal<"LockMax">());
                push_candidate("attr", "comp-badge-prop", String::literal<"ReadOnly">(), String::literal<"ReadOnly">(),
                               String::literal<"属性面板只读展示该字段">(), String::literal<"ReadOnly">());
                push_candidate("attr", "comp-badge-prop", String::literal<"Transient">(),
                               String::literal<"Transient">(), String::literal<"不参与场景持久序列化">(),
                               String::literal<"Transient">());
                push_candidate("attr", "comp-badge-prop", String::literal<"RunOver">(), String::literal<"RunOver">(),
                               String::literal<"标记程序化节点域迭代模式字段">(), String::literal<"RunOver">());
                push_candidate("attr", "comp-badge-prop", String::literal<"Flags">(), String::literal<"[Flags]">(),
                               String::literal<"声明位掩码枚举类型（需 : uint）">(), String::literal<"Flags">());
                push_candidate("attr", "comp-badge-prop", String::literal<"Meta">(),
                               String::literal<"Meta(\"key\", \"value\")">(), String::literal<"附加自定义键值元数据">(),
                               String::literal<"Meta(\"\", \"\")">(), 6u);
            }
        } else if (explicit_request || !prefix.empty()) {
            const auto ctx = resolve_source_context(full_text, caret_offset, impl_->last_source_module);
            if (!ctx.class_name.empty())
                push_candidate("kw", "comp-badge-kw", String::literal<"self">(), ctx.class_name,
                               String::literal<"当前实例接收者引用">(), String::literal<"self">());
            for (const auto [lname, ltype] : ctx.local_types)
                push_candidate("var", "comp-badge-fld", lname, ltype, String::literal<"当前作用域局部变量/参数">(),
                               lname);

            push_candidate("kw", "comp-badge-kw", String::literal<"math">(), String::literal<"math.*">(),
                           String::literal<"内置向量/四元数/矩阵数学函数命名空间">(), String::literal<"math.">());
            for (const auto& intr : kMathIntrinsics) {
                const String qname = u8_cat({"math.", intr.name});
                String sig = u8_cat({qname.view(), "("});
                for (std::size_t i = 0; i < intr.param_count; ++i) {
                    if (i > 0)
                        u8_append(sig, ", ");
                    u8_append(sig, intr.params[i]);
                }
                u8_append(sig, ") -> ");
                u8_append(sig, intr.return_type);
                push_candidate("fn", "comp-badge-fn", qname, std::move(sig), u8_lit(intr.doc),
                               u8_cat({qname.view(), "()"}), 1u);
            }

            if (impl_->last_source_module) {
                for (const auto& fn : impl_->last_source_module->functions) {
                    const String ufn = u8_safe(fn.name);
                    push_candidate("fn", "comp-badge-fn", ufn,
                                   format_encoded_function_signature(fn.name, fn.signature, fn.parameters), {},
                                   u8_cat({ufn.view(), "()"}), fn.parameters.empty() ? 0u : 1u);
                }
                for (const auto& cls : impl_->last_source_module->classes) {
                    const String ucls = u8_safe(cls.name);
                    const String base_u8 = u8_safe(cls.base);
                    push_candidate("cls", "comp-badge-type", ucls, u8_cat({": ", base_u8.view()}),
                                   u8_safe(cls.description), ucls);
                }
                for (const auto& decl : impl_->last_source_module->declarations) {
                    if (decl.kind == language::TopLevel::Kind::function)
                        continue;
                    const String ud = u8_safe(decl.name);
                    const bool is_type = decl.kind == language::TopLevel::Kind::type;
                    push_candidate(is_type ? "type" : "const", is_type ? "comp-badge-type" : "comp-badge-fld", ud,
                                   is_type ? String::literal<"Source Type">() : String::literal<"const">(), {}, ud);
                }
            }

            for (const auto& nt : impl_->native_types)
                push_candidate("type", "comp-badge-type", nt.full_name, String::literal<"Runtime Type">(),
                               nt.description, nt.full_name);

            constexpr std::array<std::pair<StringView, StringView>, 36> kSourceBuiltins{{
                {"float", "32-bit IEEE 754 浮点标量"},
                {"double", "64-bit IEEE 754 双精度标量"},
                {"int", "32-bit 有符号整数"},
                {"uint", "32-bit 无符号整数"},
                {"bool", "布尔类型"},
                {"void", "无返回值"},
                {"String", "UTF-8 字符串值"},
                {"Array", "连续元素序列 Array<T>"},
                {"vector2", "二维单精度浮点向量"},
                {"vector3", "三维单精度浮点向量"},
                {"vector4", "四维单精度浮点向量"},
                {"quaternion", "单精度四元数 (x, y, z, w)"},
                {"matrix4", "4x4 单精度矩阵"},
                {"vector2d", "二维双精度浮点向量"},
                {"vector3d", "三维双精度浮点向量"},
                {"vector4d", "四维双精度浮点向量"},
                {"quaterniond", "双精度四元数 (x, y, z, w)"},
                {"matrix4d", "4x4 双精度矩阵"},
                {"weak", "弱引用类型修饰符"},
                {"class", "声明派生自 Runtime 对象的业务类"},
                {"struct", "声明纯值结构体"},
                {"enum", "声明枚举类型"},
                {"const", "声明只读常量或只读引用参数"},
                {"return", "从当前函数或方法返回"},
                {"if", "条件分支语句"},
                {"else", "否则分支语句"},
                {"for", "循环语句"},
                {"while", "条件循环语句"},
                {"parallel_for", "并行数据循环语句"},
                {"break", "跳出当前循环"},
                {"continue", "跳至下一轮循环"},
                {"fail", "中止执行并报告诊断错误 fail(\"...\")"},
                {"new", "创建受管对象强引用实例"},
                {"observe", "获取对象弱引用句柄"},
                {"try_acquire", "尝试将弱引用提升为强引用"},
                {"null", "空对象引用字面量"},
            }};
            for (const auto& [kw, doc] : kSourceBuiltins) {
                const bool is_arr = kw == "Array";
                push_candidate("kw", "comp-badge-kw", u8_lit(kw), String::literal<"Source">(), u8_lit(doc),
                               is_arr ? String::literal<"Array<>">() : u8_lit(kw), is_arr ? 1u : 0u);
            }
        }
    } else if (lang == ui::CodeLanguage::ui_source) {
        const std::size_t caret_ln_1b = row + 1;
        const ui_language::Declaration* active_decl = nullptr;
        for (const auto& loc : impl_->last_ui_locations) {
            if (caret_ln_1b >= loc.line && caret_ln_1b <= loc.end_line) {
                for (const auto& d : impl_->last_ui_declarations)
                    if (loc.name == StringView{d.name}) {
                        active_decl = &d;
                        break;
                    }
            }
        }

        const std::size_t icon_quote = before_word.rfind('"');
        bool in_icon_string = false;
        if (icon_quote != StringView::npos) {
            StringView head = before_word.slice(0, icon_quote);
            while (!head.empty() && (head.back() == ' ' || head.back() == '\t'))
                head.remove_suffix(1);
            if (head.ends_with("icon:"))
                in_icon_string = true;
        }

        if (in_icon_string) {
            for (const StringView ic : kUiIconNames)
                push_candidate("icon", "comp-badge-prop", u8_lit(ic), String::literal<"ControlIcon">(),
                               String::literal<"内置矢量图标">(), u8_lit(ic));
        } else if (before_word.ends_with("input.")) {
            if (active_decl) {
                for (const auto& inp : active_decl->inputs) {
                    const String iname = u8_safe(inp.name);
                    push_candidate("in", "comp-badge-fld", iname, String::literal<"input">(),
                                   String::literal<"组件输入绑定参数">(), iname);
                }
            }
        } else if (before_word.ends_with("state.")) {
            if (active_decl) {
                for (const auto& st : active_decl->states) {
                    const String sname = u8_safe(st.name);
                    push_candidate("state", "comp-badge-fld", sname, String::literal<"state">(),
                                   String::literal<"组件局部持久状态槽">(), sname);
                }
            }
        } else if (before_word.ends_with("node.")) {
            if (active_decl) {
                for (const auto& nd : active_decl->nodes) {
                    const String nname = u8_safe(nd.name);
                    push_candidate("node", "comp-badge-fld", nname, String::literal<"ControlKey">(),
                                   String::literal<"具名控件实例标识">(), nname);
                }
            }
        } else if (explicit_request || !prefix.empty()) {
            const std::size_t colon_pos = before_word.rfind(':');
            const std::size_t semi_pos = before_word.rfind(';');
            const std::size_t brace_pos = before_word.rfind('{');
            const bool in_value_expr = colon_pos != StringView::npos &&
                                       (semi_pos == StringView::npos || colon_pos > semi_pos) &&
                                       (brace_pos == StringView::npos || colon_pos > brace_pos);

            if (in_value_expr) {
                push_candidate("kw", "comp-badge-kw", String::literal<"input">(), String::literal<"input.*">(),
                               String::literal<"访问组件输入参数">(), String::literal<"input.">());
                push_candidate("kw", "comp-badge-kw", String::literal<"state">(), String::literal<"state.*">(),
                               String::literal<"访问组件局部状态">(), String::literal<"state.">());
                push_candidate("kw", "comp-badge-kw", String::literal<"node">(), String::literal<"node.*">(),
                               String::literal<"引用当前组件具名控件键">(), String::literal<"node.">());
                push_candidate("fn", "comp-badge-fn", String::literal<"choose">(),
                               String::literal<"choose(cond, when_true, when_false)">(),
                               String::literal<"声明式三元条件选择表达式">(), String::literal<"choose()">(), 1u);
                push_candidate("kw", "comp-badge-kw", String::literal<"true">(), String::literal<"bool">(), {},
                               String::literal<"true">());
                push_candidate("kw", "comp-badge-kw", String::literal<"false">(), String::literal<"bool">(), {},
                               String::literal<"false">());
            } else {
                for (const auto& [pname, pdoc] : kUiProperties)
                    push_candidate("prop", "comp-badge-prop", u8_lit(pname), u8_lit(pdoc), {}, u8_cat({pname, ": "}));
                for (const auto& [nname, ndoc] : kUiNodeKinds)
                    push_candidate("ui", "comp-badge-type", u8_lit(nname), String::literal<"Built-in Control">(),
                                   u8_lit(ndoc), u8_lit(nname));
                for (const auto& d : impl_->last_ui_declarations) {
                    const String cname = u8_safe(d.name);
                    push_candidate("ui", "comp-badge-type", cname, String::literal<"Custom Component">(), {}, cname);
                }
                push_candidate("kw", "comp-badge-kw", String::literal<"ui">(),
                               String::literal<"ui Name(...) { ... }">(), String::literal<"声明可复用 UI 组件">(),
                               String::literal<"ui ">());
                push_candidate("kw", "comp-badge-kw", String::literal<"state">(),
                               String::literal<"state double x = 0.0d;">(), String::literal<"声明组件内部状态">(),
                               String::literal<"state ">());
            }
        }
    } else if (lang == ui::CodeLanguage::style_sheet) {
        const StyleSheetScanState ss_state = scan_style_sheet_prefix(full_text.slice(0, caret_offset));
        if (ss_state.masked)
            return {};
        if (before_word.ends_with("token(")) {
            const bool has_close_paren = col < line_u8.size() && line_u8[col] == ')';
            for (const auto& tok : impl_->last_ss_tokens)
                push_candidate("tok", "comp-badge-prop", tok.name, tok.value, String::literal<"@tokens 设计变量">(),
                               has_close_paren ? tok.name : u8_cat({tok.name.view(), ")"}));
        } else if (before_word.ends_with(':') && !before_word.ends_with(": ")) {
            if (ss_state.brace_depth <= 0) {
                for (const StringView st : {"hover", "pressed", "focus", "selected", "disabled", "invalid", "modified"})
                    push_candidate("state", "comp-badge-kw", u8_lit(st), String::literal<"StyleState">(),
                                   String::literal<"控件状态伪类选择器">(), u8_lit(st));
            }
        } else if (explicit_request || !prefix.empty() || before_word.ends_with('@') || before_word.ends_with('.')) {
            if (ss_state.brace_depth > 0) {
                const std::size_t colon_pos = before_word.rfind(':');
                const std::size_t semi_pos = before_word.rfind(';');
                const std::size_t brace_pos = before_word.rfind('{');
                const bool in_val = colon_pos != StringView::npos &&
                                    (semi_pos == StringView::npos || colon_pos > semi_pos) &&
                                    (brace_pos == StringView::npos || colon_pos > brace_pos);
                if (in_val) {
                    push_candidate("fn", "comp-badge-fn", String::literal<"token">(), String::literal<"token(name)">(),
                                   String::literal<"引用 @tokens 变量值">(), String::literal<"token()">(), 1u);
                    push_candidate("fn", "comp-badge-fn", String::literal<"linear-gradient">(),
                                   String::literal<"linear-gradient(#top, #bottom)">(),
                                   String::literal<"垂直双色线性渐变图像">(), String::literal<"linear-gradient()">(),
                                   1u);
                    for (const StringView kw :
                         {"transparent", "none", "solid", "all", "top", "bottom", "left", "right", "center"})
                        push_candidate("kw", "comp-badge-kw", u8_lit(kw), String::literal<"value">(), {}, u8_lit(kw));
                } else {
                    for (const auto& [pname, pdoc] : kSsProperties)
                        push_candidate("prop", "comp-badge-prop", u8_lit(pname), u8_lit(pdoc), {},
                                       u8_cat({pname, ": "}));
                }
            } else {
                if (before_word.ends_with('@')) {
                    push_candidate("kw", "comp-badge-kw", String::literal<"tokens">(),
                                   String::literal<"@tokens { ... }">(), String::literal<"定义全局设计变量表">(),
                                   String::literal<"tokens {\n    \n}">(), 2u);
                } else if (before_word.ends_with('.')) {
                    for (const auto& cls : impl_->last_ss_classes)
                        push_candidate("cls", "comp-badge-type", cls, String::literal<"Style Class">(), {}, cls);
                } else {
                    for (const StringView sel :
                         {"button", "combo", "checkbox", "text_field", "table", "tree", "code_editor", "foldout",
                          "popup", "menu_item", "tab", "splitter", "color_plane"})
                        push_candidate("sel", "comp-badge-type", u8_lit(sel), String::literal<"ControlKind Selector">(),
                                       {}, u8_lit(sel));
                }
            }
        }
    }

    std::ranges::sort(items, [](const CodeCompletionItem& a, const CodeCompletionItem& b) {
        if (a.score != b.score)
            return a.score > b.score;
        return a.label < b.label;
    });

    Array<CodeCompletionItem> unique_items;
    unique_items.reserve(std::min<std::size_t>(items.size(), 12));
    for (auto& it : items) {
        if (unique_items.contains_if([&](const CodeCompletionItem& u) { return u.label == it.label; }))
            continue;
        unique_items.add(std::move(it));
        if (unique_items.size() >= 10)
            break;
    }
    if (!explicit_request && unique_items.size() == 1 && unique_items.front().label == prefix)
        return {};
    return unique_items;
}

Optional<CodeSignatureHelp> CodeIntelligence::signature_help(const ui::CodeDocument& document,
                                                             std::uint32_t caret_offset) const {
    const ui::CodeLanguage lang = document.language();
    if (lang != ui::CodeLanguage::source && lang != ui::CodeLanguage::ui_source)
        return std::nullopt;

    const StringView text = document.text().view();
    if (caret_offset > text.size() || caret_offset == 0)
        return std::nullopt;

    const std::uint32_t raw_limit = caret_offset > 1024 ? caret_offset - 1024 : 0;
    const std::size_t start_row = document.edit().line_at(raw_limit);
    const std::uint32_t limit = document.edit().line_start(start_row);

    struct CallFrame {
        std::uint32_t open_paren{};

        int bracket_depth{};

        std::size_t arg_index{};
    };

    InlineArray<CallFrame, 16> stack;
    bool in_str = false;
    bool in_line_comment = false;
    bool in_block_comment = false;
    for (std::uint32_t i = limit; i < caret_offset; ++i) {
        const char ch = text[i];
        const char nx = i + 1 < caret_offset ? text[i + 1] : '\0';
        if (in_line_comment) {
            if (ch == '\n')
                in_line_comment = false;
            continue;
        }
        if (in_block_comment) {
            if (ch == '*' && nx == '/') {
                in_block_comment = false;
                ++i;
            }
            continue;
        }
        if (in_str) {
            if (ch == '\\' && i + 1 < caret_offset)
                ++i;
            else if (ch == '"')
                in_str = false;
            continue;
        }
        if (ch == '/' && nx == '/') {
            in_line_comment = true;
            ++i;
            continue;
        }
        if (ch == '/' && nx == '*') {
            in_block_comment = true;
            ++i;
            continue;
        }
        if (ch == '"') {
            in_str = true;
            continue;
        }
        if (ch == ';' || ch == '{' || ch == '}') {
            stack.clear();
        } else if (ch == '(') {
            stack.add({i, 0, 0});
        } else if (ch == ')') {
            if (!stack.empty())
                stack.pop();
        } else if (ch == '[' && !stack.empty()) {
            ++stack.back().bracket_depth;
        } else if (ch == ']' && !stack.empty() && stack.back().bracket_depth > 0) {
            --stack.back().bracket_depth;
        } else if (ch == ',' && !stack.empty() && stack.back().bracket_depth == 0) {
            ++stack.back().arg_index;
        }
    }
    if (stack.empty())
        return std::nullopt;

    const std::uint32_t open_paren = stack.back().open_paren;
    const std::size_t arg_index = stack.back().arg_index;

    std::uint32_t fn_end = open_paren;
    while (fn_end > 0 && text[fn_end - 1] == ' ')
        --fn_end;
    std::uint32_t fn_start = fn_end;
    while (fn_start > 0 && is_ascii_ident_continue(text[fn_start - 1]))
        --fn_start;
    if (fn_start == fn_end)
        return std::nullopt;
    const StringView callee = text.slice(fn_start, fn_end - fn_start);
    if (callee == "if" || callee == "for" || callee == "while" || callee == "parallel_for")
        return std::nullopt;

    std::uint32_t qual_start = fn_start;
    while (qual_start > 1 && text[qual_start - 1] == '.' && is_ascii_ident_continue(text[qual_start - 2])) {
        --qual_start;
        while (qual_start > 0 && is_ascii_ident_continue(text[qual_start - 1]))
            --qual_start;
    }
    const StringView qualified_callee = text.slice(qual_start, fn_end - qual_start);

    auto build_help = [&](StringView name, Span<const String> params, StringView ret_type,
                          StringView doc) -> CodeSignatureHelp {
        CodeSignatureHelp out;
        out.prefix = u8_cat({name, "("});
        for (std::size_t i = 0; i < params.size(); ++i) {
            const StringView sep = i > 0 ? StringView{", "} : StringView{};
            if (i < arg_index) {
                u8_append(out.prefix, sep);
                out.prefix.append(params[i]);
            } else if (i == arg_index) {
                u8_append(out.prefix, sep);
                out.active_parameter = params[i];
            } else {
                u8_append(out.suffix, sep);
                out.suffix.append(params[i]);
            }
        }
        if (arg_index >= params.size() && !params.empty())
            out.active_parameter = params.back();
        u8_append(out.suffix, ")");
        if (!ret_type.empty()) {
            u8_append(out.suffix, " -> ");
            u8_append(out.suffix, ret_type);
        }
        out.documentation = u8_lit(doc);
        return out;
    };

    if (lang == ui::CodeLanguage::ui_source && callee == "choose") {
        const std::array<String, 3> p{String::literal<"bool condition">(), String::literal<"T when_true">(),
                                      String::literal<"T when_false">()};
        return Optional<CodeSignatureHelp>{
            build_help("choose", Span<const String>{p}, "T", "根据布尔条件在两个同类型值之间选择。")};
    }

    if (qualified_callee.starts_with("math.") && qualified_callee.slice(5) == callee) {
        for (const auto& intr : kMathIntrinsics) {
            if (intr.name == callee) {
                InlineArray<String, 4> p;
                for (std::size_t i = 0; i < intr.param_count; ++i)
                    p.add(u8_lit(intr.params[i]));
                return Optional<CodeSignatureHelp>{build_help(qualified_callee, p.view(), intr.return_type, intr.doc)};
            }
        }
    }

    if (impl_->last_source_module) {
        for (const auto& cls : impl_->last_source_module->classes) {
            for (const auto& m : cls.methods) {
                if (callee == StringView{m.name}) {
                    Array<String> p;
                    format_method_signature(m, &p);
                    const String res_t = format_source_type_name(m.result_type);
                    return Optional<CodeSignatureHelp>{build_help(callee, p.view(), res_t.view(), {})};
                }
            }
        }
        for (const auto& fn : impl_->last_source_module->functions) {
            const String ufn = u8_safe(fn.name);
            const std::size_t dot = ufn.rfind('.');
            if (ufn == qualified_callee || ufn == callee || (dot != StringView::npos && ufn.slice(dot + 1) == callee)) {
                Array<String> p;
                String ret;
                format_encoded_function_signature(fn.name, fn.signature, fn.parameters, &p, &ret);
                return Optional<CodeSignatureHelp>{build_help(ufn.view(), p.view(), ret.view(), {})};
            }
        }
    }

    for (const auto& nt : impl_->native_types) {
        for (const auto& nm : nt.members) {
            if (nm.is_method && nm.name == callee)
                return Optional<CodeSignatureHelp>{
                    build_help(nm.name.view(), nm.params.view(), nm.return_type.view(), nm.description.view())};
        }
    }

    return std::nullopt;
}

namespace {
struct ExtractedSymbol {
    std::size_t row{};

    std::uint32_t line_start{};

    std::uint32_t word_start{};

    std::uint32_t word_end{};

    String word;

    String qualified;
};

Optional<ExtractedSymbol> extract_symbol_at(const ui::CodeDocument& document, std::uint32_t doc_offset) {
    const StringView text = document.text().view();
    if (doc_offset >= text.size())
        return std::nullopt;
    const auto& edit = document.edit();
    const std::size_t row = edit.line_at(doc_offset);
    const std::uint32_t ls = edit.line_start(row);
    const StringView line_u8 = edit.line(row);
    const std::uint32_t col = doc_offset - ls;
    if (col >= line_u8.size())
        return std::nullopt;

    const ui::CodeLanguage lang = document.language();
    const auto is_ch = [&](char c) noexcept { return ui::is_code_ident_char(c, lang); };
    if (!is_ch(line_u8[col]))
        return std::nullopt;

    std::uint32_t ws = col;
    while (ws > 0 && is_ch(line_u8[ws - 1]))
        --ws;
    std::uint32_t we = col;
    while (we < line_u8.size() && is_ch(line_u8[we]))
        ++we;

    std::uint32_t qs = ws;
    while (qs > 1 && line_u8[qs - 1] == '.' && is_ascii_ident_continue(line_u8[qs - 2])) {
        --qs;
        while (qs > 0 && is_ascii_ident_continue(line_u8[qs - 1]))
            --qs;
    }
    return Optional<ExtractedSymbol>{
        ExtractedSymbol{row, ls, ws, we, u8_lit(line_u8.slice(ws, we - ws)), u8_lit(line_u8.slice(qs, we - qs))}};
}
} // namespace

Optional<CodeDefinitionTarget> CodeIntelligence::definition(const ui::CodeDocument& document,
                                                            std::uint32_t doc_offset) const {
    const auto sym = extract_symbol_at(document, doc_offset);
    if (!sym)
        return std::nullopt;
    const ui::CodeLanguage lang = document.language();
    const StringView word = sym->word.view();
    const StringView qualified = sym->qualified.view();

    if (lang == ui::CodeLanguage::style_sheet) {
        for (const auto& tok : impl_->last_ss_tokens) {
            if (tok.name == word)
                return Optional<CodeDefinitionTarget>{CodeDefinitionTarget{impl_->active_path, tok.line, tok.column}};
        }
        return std::nullopt;
    }

    if (lang == ui::CodeLanguage::ui_source) {
        for (const auto& loc : impl_->last_ui_locations) {
            if (loc.name == word)
                return Optional<CodeDefinitionTarget>{CodeDefinitionTarget{impl_->active_path, loc.line, loc.column}};
        }
        return std::nullopt;
    }

    if (lang == ui::CodeLanguage::source && impl_->last_source_module && impl_->last_snapshot) {
        const auto file_path_for = [&](std::uint32_t file_idx) -> std::filesystem::path {
            if (file_idx < impl_->last_snapshot->files.size()) {
                const auto& sf = impl_->last_snapshot->files[file_idx];
                if (!sf.disk_path.empty())
                    return path_from_utf8(sf.disk_path);
            }
            return impl_->active_path;
        };

        for (const auto& cls : impl_->last_source_module->classes) {
            const StringView ucls{cls.name};
            const std::size_t dot = ucls.rfind('.');
            const StringView short_cls = dot == StringView::npos ? ucls : ucls.slice(dot + 1);
            if (ucls == qualified || short_cls == word)
                return Optional<CodeDefinitionTarget>{
                    CodeDefinitionTarget{file_path_for(cls.file), cls.position.line, cls.position.column}};
            for (const auto& f : cls.fields) {
                if (word == StringView{f.name})
                    return Optional<CodeDefinitionTarget>{
                        CodeDefinitionTarget{file_path_for(cls.file), f.position.line, f.position.column}};
            }
            for (const auto& m : cls.methods) {
                if (word == StringView{m.name})
                    return Optional<CodeDefinitionTarget>{
                        CodeDefinitionTarget{file_path_for(cls.file), m.position.line, m.position.column}};
            }
        }
        for (const auto& decl : impl_->last_source_module->declarations) {
            const StringView ud{decl.name};
            const std::size_t dot = ud.rfind('.');
            const StringView short_d = dot == StringView::npos ? ud : ud.slice(dot + 1);
            if (ud == qualified || short_d == word)
                return Optional<CodeDefinitionTarget>{
                    CodeDefinitionTarget{file_path_for(decl.file), decl.position.line, decl.position.column}};
        }
        for (const auto& td : impl_->last_source_declarations) {
            if (!td.enumeration)
                continue;
            for (const auto& [ename, eval] : td.enumerators) {
                (void)eval;
                if (word == StringView{ename}) {
                    for (const auto& decl : impl_->last_source_module->declarations) {
                        if (decl.name == td.name)
                            return Optional<CodeDefinitionTarget>{CodeDefinitionTarget{
                                file_path_for(decl.file), decl.position.line, decl.position.column}};
                    }
                }
            }
        }
    }
    return std::nullopt;
}

Optional<CodeHoverInfo> CodeIntelligence::hover(const ui::CodeDocument& document, std::uint32_t doc_offset) const {
    const StringView text = document.text().view();
    if (doc_offset >= text.size())
        return std::nullopt;
    const auto& edit = document.edit();
    const std::size_t row = edit.line_at(doc_offset);
    const std::uint32_t ls = edit.line_start(row);
    const std::uint32_t col = doc_offset - ls;

    for (const auto& diag : impl_->active_diagnostics) {
        if (diag.line == row && col >= diag.col_start && col <= std::max(diag.col_end, diag.col_start + 1)) {
            CodeHoverInfo info;
            info.range_start = ls + diag.col_start;
            info.range_end = ls + std::max(diag.col_end, diag.col_start + 1);
            info.tooltip.title = diag.severity == ui::CodeDiagnosticSeverity::error     ? L"Compiler Error"
                                 : diag.severity == ui::CodeDiagnosticSeverity::warning ? L"Warning"
                                                                                        : L"Diagnostic";
            info.tooltip.identity = L"Ln " + std::to_wstring(row + 1) + L", Col " + std::to_wstring(diag.col_start + 1);
            info.tooltip.description = to_tooltip_wide(diag.message);
            return Optional<CodeHoverInfo>{std::move(info)};
        }
    }

    const auto sym = extract_symbol_at(document, doc_offset);
    if (!sym)
        return std::nullopt;
    const ui::CodeLanguage lang = document.language();
    const StringView word = sym->word.view();
    const StringView qualified = sym->qualified.view();

    CodeHoverInfo out;
    out.range_start = sym->line_start + sym->word_start;
    out.range_end = sym->line_start + sym->word_end;
    out.definition = definition(document, doc_offset);

    if (lang == ui::CodeLanguage::style_sheet) {
        for (const auto& tok : impl_->last_ss_tokens) {
            if (tok.name == word) {
                out.tooltip.title = to_tooltip_wide(u8_cat({"token(", tok.name.view(), ")"}));
                out.tooltip.identity = to_tooltip_wide(tok.value);
                out.tooltip.description =
                    L"定义于 @tokens (第 " + std::to_wstring(tok.line) + L" 行)。按住 Ctrl 单击或按 F12 跳转到定义。";
                return Optional<CodeHoverInfo>{std::move(out)};
            }
        }
        for (const auto& [pname, pdoc] : kSsProperties) {
            if (pname == word) {
                out.tooltip.title = to_tooltip_wide(pname);
                out.tooltip.identity = L"StyleSheet Property";
                out.tooltip.description = to_tooltip_wide(pdoc);
                return Optional<CodeHoverInfo>{std::move(out)};
            }
        }
        return std::nullopt;
    }

    if (lang == ui::CodeLanguage::ui_source) {
        for (const auto& [nname, ndoc] : kUiNodeKinds) {
            if (nname == word) {
                out.tooltip.title = to_tooltip_wide(nname);
                out.tooltip.identity = L"Declarative UI Control";
                out.tooltip.description = to_tooltip_wide(ndoc);
                return Optional<CodeHoverInfo>{std::move(out)};
            }
        }
        for (const auto& [pname, pdoc] : kUiProperties) {
            if (pname == word) {
                out.tooltip.title = to_tooltip_wide(pname);
                out.tooltip.identity = L"UI Node Property";
                out.tooltip.description = to_tooltip_wide(pdoc);
                return Optional<CodeHoverInfo>{std::move(out)};
            }
        }
        for (const auto& d : impl_->last_ui_declarations) {
            if (word == StringView{d.name}) {
                out.tooltip.title = to_tooltip_wide(u8_cat({"ui ", word}));
                out.tooltip.identity =
                    std::to_wstring(d.inputs.size()) + L" inputs · " + std::to_wstring(d.nodes.size()) + L" nodes";
                out.tooltip.description = L"自定义声明式 UI 组件。按住 Ctrl 单击或按 F12 跳转到组件定义。";
                return Optional<CodeHoverInfo>{std::move(out)};
            }
        }
        return std::nullopt;
    }

    if (lang == ui::CodeLanguage::source) {
        if (qualified.starts_with("math.") && qualified.slice(5) == word) {
            for (const auto& intr : kMathIntrinsics) {
                if (intr.name == word) {
                    String sig = u8_cat({"math.", intr.name, "("});
                    for (std::size_t i = 0; i < intr.param_count; ++i) {
                        if (i > 0)
                            u8_append(sig, ", ");
                        u8_append(sig, intr.params[i]);
                    }
                    u8_append(sig, ") -> ");
                    u8_append(sig, intr.return_type);
                    out.tooltip.title = to_tooltip_wide(sig);
                    out.tooltip.identity = L"Math Intrinsic";
                    out.tooltip.description = to_tooltip_wide(intr.doc);
                    return Optional<CodeHoverInfo>{std::move(out)};
                }
            }
        }

        if (impl_->last_source_module) {
            for (const auto& cls : impl_->last_source_module->classes) {
                const String ucls = u8_safe(cls.name);
                const std::size_t dot = ucls.rfind('.');
                const StringView short_c = dot == StringView::npos ? ucls.view() : ucls.slice(dot + 1);
                if (ucls == qualified || short_c == word) {
                    const String base_u8 = u8_safe(cls.base);
                    out.tooltip.title = to_tooltip_wide(u8_cat({"class ", ucls.view(), " : ", base_u8.view()}));
                    out.tooltip.identity = L"Source Class";
                    out.tooltip.description = cls.description.empty() ? L"按住 Ctrl 单击或按 F12 跳转到类定义。"
                                                                      : to_tooltip_wide(u8_safe(cls.description));
                    return Optional<CodeHoverInfo>{std::move(out)};
                }
                for (const auto& f : cls.fields) {
                    if (word == StringView{f.name}) {
                        const String ftype_u8 = u8_safe(f.type);
                        out.tooltip.title = to_tooltip_wide(u8_cat({ftype_u8.view(), " ", word}));
                        out.tooltip.identity = to_tooltip_wide(ucls);
                        out.tooltip.description =
                            f.description.empty() ? L"业务类字段成员。" : to_tooltip_wide(u8_safe(f.description));
                        return Optional<CodeHoverInfo>{std::move(out)};
                    }
                }
                for (const auto& m : cls.methods) {
                    if (word == StringView{m.name}) {
                        out.tooltip.title = to_tooltip_wide(format_method_signature(m));
                        out.tooltip.identity = to_tooltip_wide(ucls);
                        out.tooltip.description = L"业务类方法成员。按住 Ctrl 单击或按 F12 跳转到定义。";
                        return Optional<CodeHoverInfo>{std::move(out)};
                    }
                }
            }
            for (const auto& fn : impl_->last_source_module->functions) {
                const String ufn = u8_safe(fn.name);
                const std::size_t dot = ufn.rfind('.');
                if (ufn == qualified || (dot != StringView::npos && ufn.slice(dot + 1) == word)) {
                    out.tooltip.title =
                        to_tooltip_wide(format_encoded_function_signature(fn.name, fn.signature, fn.parameters));
                    out.tooltip.identity = L"Source Function";
                    out.tooltip.description = L"模块级函数。按住 Ctrl 单击或按 F12 跳转到定义。";
                    return Optional<CodeHoverInfo>{std::move(out)};
                }
            }
            for (const auto& td : impl_->last_source_declarations) {
                const String utd = u8_safe(td.name);
                const std::size_t dot = utd.rfind('.');
                const StringView short_td = dot == StringView::npos ? utd.view() : utd.slice(dot + 1);
                if (utd == qualified || short_td == word) {
                    if (td.enumeration) {
                        out.tooltip.title = to_tooltip_wide(u8_cat({"enum ", utd.view()}));
                        out.tooltip.identity = L"Source Enum";
                        String body;
                        for (std::size_t i = 0; i < td.enumerators.size() && i < 6; ++i) {
                            if (i > 0)
                                u8_append(body, ", ");
                            const String en = u8_safe(td.enumerators[i].first);
                            const String ev = String::from_number(td.enumerators[i].second);
                            u8_append(body, en.view());
                            u8_append(body, " = ");
                            u8_append(body, ev.view());
                        }
                        if (td.enumerators.size() > 6)
                            u8_append(body, ", ...");
                        if (!td.description.empty()) {
                            const String dstr = u8_safe(td.description);
                            body = body.empty() ? dstr : u8_cat({dstr.view(), "\n", body.view()});
                        }
                        out.tooltip.description =
                            body.empty() ? L"枚举类型。按住 Ctrl 单击或按 F12 跳转到定义。" : to_tooltip_wide(body);
                    } else {
                        out.tooltip.title = to_tooltip_wide(u8_cat({"struct ", utd.view()}));
                        out.tooltip.identity = L"Source Struct";
                        String body;
                        for (std::size_t i = 0; i < td.fields.size() && i < 6; ++i) {
                            if (i > 0)
                                u8_append(body, "; ");
                            const String ft = format_source_type_name(td.fields[i].second);
                            const String fn = u8_safe(td.fields[i].first);
                            u8_append(body, ft.view());
                            u8_append(body, " ");
                            u8_append(body, fn.view());
                        }
                        if (td.fields.size() > 6)
                            u8_append(body, "; ...");
                        if (!td.description.empty()) {
                            const String dstr = u8_safe(td.description);
                            body = body.empty() ? dstr : u8_cat({dstr.view(), "\n", body.view()});
                        }
                        out.tooltip.description =
                            body.empty() ? L"值结构体类型。按住 Ctrl 单击或按 F12 跳转到定义。" : to_tooltip_wide(body);
                    }
                    return Optional<CodeHoverInfo>{std::move(out)};
                }
                if (td.enumeration) {
                    for (std::size_t i = 0; i < td.enumerators.size(); ++i) {
                        if (word == StringView{td.enumerators[i].first}) {
                            const String en = u8_safe(td.enumerators[i].first);
                            const String ev = String::from_number(td.enumerators[i].second);
                            out.tooltip.title =
                                to_tooltip_wide(u8_cat({utd.view(), "::", en.view(), " = ", ev.view()}));
                            out.tooltip.identity = L"Source Enumerator";
                            out.tooltip.description =
                                (i < td.enumerator_attributes.size() &&
                                 !td.enumerator_attributes[i].description.empty())
                                    ? to_tooltip_wide(u8_safe(td.enumerator_attributes[i].description))
                                    : L"枚举常量成员。按住 Ctrl 单击或按 F12 跳转到枚举定义。";
                            return Optional<CodeHoverInfo>{std::move(out)};
                        }
                    }
                }
            }
            for (const auto& decl : impl_->last_source_module->declarations) {
                if (decl.kind != language::TopLevel::Kind::constant)
                    continue;
                const String ud = u8_safe(decl.name);
                const std::size_t dot = ud.rfind('.');
                const StringView short_d = dot == StringView::npos ? ud.view() : ud.slice(dot + 1);
                if (ud == qualified || short_d == word) {
                    out.tooltip.title = to_tooltip_wide(u8_cat({"const ", ud.view()}));
                    out.tooltip.identity = L"Source Constant";
                    out.tooltip.description = L"模块级只读常量。按住 Ctrl 单击或按 F12 跳转到定义。";
                    return Optional<CodeHoverInfo>{std::move(out)};
                }
            }
        }

        for (const auto& nt : impl_->native_types) {
            if (nt.full_name == qualified || nt.short_name == word) {
                out.tooltip.title = to_tooltip_wide(nt.full_name);
                out.tooltip.identity = L"Native Runtime Type";
                out.tooltip.description =
                    nt.description.empty() ? L"引擎权威运行时注册类型。" : to_tooltip_wide(nt.description);
                return Optional<CodeHoverInfo>{std::move(out)};
            }
            for (const auto& nm : nt.members) {
                if (nm.name == word) {
                    out.tooltip.title = to_tooltip_wide(nm.signature);
                    out.tooltip.identity = to_tooltip_wide(nt.full_name);
                    out.tooltip.description = nm.description.empty()
                                                  ? (nm.is_method ? L"原生方法成员。" : L"原生属性字段。")
                                                  : to_tooltip_wide(nm.description);
                    return Optional<CodeHoverInfo>{std::move(out)};
                }
            }
        }
    }

    return std::nullopt;
}

} // namespace atelier::editor
