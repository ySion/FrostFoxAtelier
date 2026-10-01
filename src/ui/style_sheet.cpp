#include "ui/style_sheet.hpp"
#include <Windows.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>

namespace atelier::ui {
namespace {
bool bounded(float n,float minimum,float maximum) { return std::isfinite(n) && n>=minimum && n<=maximum; }
bool color_valid(Color c) { return bounded(c.r,0,1) && bounded(c.g,0,1) && bounded(c.b,0,1) && bounded(c.a,0,1); }
bool name_valid(const std::wstring& value) { return !value.empty() && value.size()<=128 && value.find(L'\0')==value.npos; }
bool layout_properties(const StyleProperties& p) {
    return p.font_family || p.font_locale || p.font_size || p.font_weight || p.padding;
}
unsigned specificity(const StyleSelector& s) {
    unsigned states=0;
    for (auto bits=static_cast<unsigned>(s.states);bits;bits>>=1) states+=bits&1;
    return static_cast<unsigned>(s.classes.size())*100+states*100+(s.kind?1:0)+(s.role?1:0);
}
std::string_view trim(std::string_view value) {
    constexpr auto space=" \t\r\n";
    const auto first=value.find_first_not_of(space);
    return first==value.npos?std::string_view{}:value.substr(first,value.find_last_not_of(space)-first+1);
}
struct ParseFailure { std::size_t position; std::string message; };
class Parser {
    struct Value { std::string_view text; std::size_t position; };
    struct Declaration { std::string name; Value value; };
    struct Block { std::vector<StyleSelector> selectors; std::vector<Declaration> declarations; };
    std::string source_;
    std::span<const NamedImage> images_;
    std::size_t at_{};
    std::map<std::string,Value,std::less<>> tokens_;
    std::vector<Block> blocks_;
    [[noreturn]] void fail(std::string message,std::size_t position) const { throw ParseFailure{position,std::move(message)}; }
    [[noreturn]] void fail(std::string message) const { fail(std::move(message),at_); }
    void space() { while (at_<source_.size() && std::string_view(" \t\r\n").find(source_[at_])!=std::string_view::npos) ++at_; }
    bool take(char c) { space(); if (at_<source_.size() && source_[at_]==c) { ++at_; return true; } return false; }
    void expect(char c) { if (!take(c)) fail(std::string("Expected '")+c+"'"); }
    std::string identifier() {
        space(); const auto start=at_;
        while (at_<source_.size()) {
            const auto c=source_[at_];
            if (!(c>='a' && c<='z') && !(c>='A' && c<='Z') && !(c>='0' && c<='9') && c!='_' && c!='-') break;
            ++at_;
        }
        auto value=source_.substr(start,at_-start);
        if (!StyleSheet::valid_class(value)) fail("Expected an identifier",start);
        return value;
    }
    Value value() {
        space(); const auto start=at_; bool quoted=false; char quote=0; unsigned depth=0;
        while (at_<source_.size()) {
            const auto c=source_[at_];
            if (quoted) { if (c==quote) quoted=false; }
            else if (c=='\'' || c=='"') { quoted=true; quote=c; }
            else if (c=='(') { if (++depth>8) fail("Value nesting is too deep"); }
            else if (c==')') { if (!depth) fail("Unexpected ')'"); --depth; }
            else if ((c==';' || c=='}') && !depth) break;
            ++at_;
        }
        if (quoted || depth) fail("Unterminated value",start);
        auto text=trim(std::string_view(source_).substr(start,at_-start));
        if (text.empty() || text.size()>4096) fail("Empty or oversized value",start);
        expect(';'); return {text,start};
    }
    StyleSelector selector() {
        StyleSelector result; bool any=false;
        space();
        if (at_<source_.size() && source_[at_]!='.' && source_[at_]!=':') {
            if (take('*')) any=true;
            else {
                const auto type=identifier(); any=true;
                constexpr std::string_view kinds[]={"container","label","button","list","item","number","splitter","viewport","text_edit","checkbox","foldout"};
                const auto found=std::find(std::begin(kinds),std::end(kinds),type);
                if (found==std::end(kinds)) fail("Unknown control type: "+type);
                result.kind=static_cast<ControlKind>(found-std::begin(kinds));
            }
        }
        // A compound selector targets one node. Whitespace never means descendant matching.
        while (at_<source_.size() && (source_[at_]=='.' || source_[at_]==':')) {
            const auto prefix=source_[at_++]; const auto id=identifier(); any=true;
            if (prefix=='.') result.classes.push_back(id);
            else if (id=="role") {
                expect('('); const auto role=identifier(); expect(')');
                constexpr std::string_view roles[]={"normal","panel","title","caption"};
                const auto found=std::find(std::begin(roles),std::end(roles),role);
                if (found==std::end(roles) || result.role) fail("Unknown or duplicate role");
                result.role=static_cast<Role>(found-std::begin(roles));
            } else {
                constexpr std::string_view names[]={"enabled","disabled","hover","pressed","focus","selected","invalid","modified"};
                const auto found=std::find(std::begin(names),std::end(names),id);
                if (found==std::end(names)) fail("Unknown state: "+id);
                const auto bit=static_cast<StyleState>(1u<<(found-std::begin(names)));
                if ((static_cast<unsigned>(result.states)&static_cast<unsigned>(bit))!=0) fail("Duplicate state: "+id);
                result.states=result.states|bit;
            }
        }
        if (!any) fail("Expected a selector");
        if (result.classes.size()>16) fail("A selector supports at most 16 classes");
        if ((static_cast<unsigned>(result.states)&3)==3) fail("A selector cannot be both enabled and disabled");
        return result;
    }
    Value resolve(Value input,unsigned depth=0) const {
        if (!input.text.starts_with("token(")) return input;
        if (depth>=32 || !input.text.ends_with(')')) fail("Invalid or cyclic token reference",input.position);
        const auto key=trim(input.text.substr(6,input.text.size()-7));
        const auto found=tokens_.find(key);
        if (found==tokens_.end()) fail("Unknown token: "+std::string(key),input.position);
        return resolve(found->second,depth+1);
    }
    float number(Value input,bool length=true) const {
        input=resolve(input); auto text=input.text;
        if (length && text.ends_with("dip")) text.remove_suffix(3);
        float result{}; const auto parsed=std::from_chars(text.data(),text.data()+text.size(),result);
        if (parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size() || !std::isfinite(result)) fail("Expected a finite number",input.position);
        return result;
    }
    std::vector<float> lengths(Value input) const {
        input=resolve(input); auto remaining=input.text; std::vector<float> values;
        while (!remaining.empty()) {
            const auto end=remaining.find_first_of(" \t\r\n");
            values.push_back(number({remaining.substr(0,end),input.position}));
            if (values.size()>4) fail("Expected one, two or four lengths",input.position);
            remaining=end==remaining.npos?std::string_view{}:trim(remaining.substr(end));
        }
        if (values.size()!=1 && values.size()!=2 && values.size()!=4) fail("Expected one, two or four lengths",input.position);
        return values;
    }
    Color color(Value input) const {
        input=resolve(input);
        if (input.text=="transparent" || input.text=="none") return {0,0,0,0};
        if (!input.text.starts_with('#') || (input.text.size()!=7 && input.text.size()!=9)) fail("Expected #RRGGBB, #RRGGBBAA or transparent",input.position);
        unsigned channels[4]={0,0,0,255};
        for (std::size_t i=0;i<(input.text.size()-1)/2;++i) {
            const auto piece=input.text.substr(1+i*2,2);
            const auto parsed=std::from_chars(piece.data(),piece.data()+2,channels[i],16);
            if (parsed.ec!=std::errc{} || parsed.ptr!=piece.data()+2) fail("Invalid hexadecimal color",input.position);
        }
        const auto linear=[](unsigned byte) { const auto x=byte/255.f; return x<=.04045f?x/12.92f:std::pow((x+.055f)/1.055f,2.4f); };
        return {linear(channels[0]),linear(channels[1]),linear(channels[2]),channels[3]/255.f};
    }
    std::wstring string(Value input) const {
        input=resolve(input); auto text=input.text;
        if (text.size()<2 || (text.front()!='\'' && text.front()!='"') || text.back()!=text.front()) fail("Expected a quoted UTF-8 string",input.position);
        text.remove_prefix(1); text.remove_suffix(1);
        if (text.find('\\')!=text.npos) fail("String escapes are not supported",input.position);
        const auto count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
        if (!count) fail("Invalid or empty UTF-8 string",input.position);
        std::wstring result(static_cast<std::size_t>(count),L'\0');
        if (MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),result.data(),count)!=count) fail("Invalid UTF-8 string",input.position);
        return result;
    }
    void property(StyleProperties& p,const Declaration& d) const {
        const auto& name=d.name; const auto v=d.value;
        if (name=="color") p.foreground=color(v);
        else if (name=="background") p.background=color(v);
        else if (name=="border-color") p.border=color(v);
        else if (name=="selection-color") p.selection=color(v);
        else if (name=="caret-color") p.caret=color(v);
        else if (name=="scrollbar-color") p.scrollbar=color(v);
        else if (name=="font-family") p.font_family=string(v);
        else if (name=="font-locale") p.font_locale=string(v);
        else if (name=="font-size") p.font_size=number(v);
        else if (name=="font-weight") {
            const auto weight=number(v,false);
            if (weight<1 || weight>999 || weight!=std::floor(weight)) fail("Font weight must be an integer in 1..999",v.position);
            p.font_weight=static_cast<std::uint16_t>(weight);
        } else if (name=="padding") {
            const auto n=lengths(v);
            p.padding=n.size()==1?ui_layout::Insets{n[0],n[0],n[0],n[0]}:
                n.size()==2?ui_layout::Insets{n[1],n[0],n[1],n[0]}:ui_layout::Insets{n[3],n[0],n[1],n[2]};
        } else if (name=="border-radius") {
            const auto n=lengths(v);
            p.radius=n.size()==1?CornerRadii{n[0],n[0],n[0],n[0]}:
                n.size()==2?CornerRadii{n[0],n[1],n[0],n[1]}:CornerRadii{n[0],n[1],n[2],n[3]};
        } else if (name=="border-width") p.border_width=number(v);
        else if (name=="caret-width") p.caret_width=number(v);
        else if (name=="scrollbar-width") p.scrollbar_width=number(v);
        else if (name=="scrollbar-inset") p.scrollbar_inset=number(v);
        else if (name=="scrollbar-min-thumb") p.scrollbar_min_thumb=number(v);
        else if (name=="background-image") {
            const auto input=resolve(v);
            if (input.text=="none") p.image=std::shared_ptr<const ColorImage>{};
            else {
                if (!input.text.starts_with("image(") || !input.text.ends_with(')')) fail("Expected image(name) or none",v.position);
                const auto id=trim(input.text.substr(6,input.text.size()-7));
                const auto found=std::find_if(images_.begin(),images_.end(),[&](const auto& image){return image.name==id;});
                if (found==images_.end() || !found->image) fail("Unknown image resource: "+std::string(id),v.position);
                p.image=found->image;
            }
        } else fail("Unknown property: "+name,v.position);
        if (!StyleSheet::valid_properties(p)) fail("Property value is outside the supported range: "+name,v.position);
    }
public:
    Parser(std::string_view source,std::span<const NamedImage> images) : source_(source),images_(images) {
        // Replace comments with spaces to retain exact source locations and value views.
        bool quoted=false; char quote=0;
        for (std::size_t i=0;i<source_.size();++i) {
            const char c=source_[i];
            if (quoted) { if (c==quote) quoted=false; continue; }
            if (c=='\'' || c=='"') { quoted=true; quote=c; continue; }
            if (c=='/' && i+1<source_.size() && source_[i+1]=='*') {
                const auto end=source_.find("*/",i+2);
                if (end==source_.npos) fail("Unterminated comment",i);
                for (auto j=i;j<end+2;++j) if (source_[j]!='\n' && source_[j]!='\r') source_[j]=' ';
                i=end+1;
            }
        }
    }
    std::vector<StyleRule> run() {
        while (true) {
            space(); if (at_==source_.size()) break;
            if (take('@')) {
                if (identifier()!="tokens") fail("Only @tokens is supported");
                expect('{');
                while (!take('}')) { const auto id=identifier(); expect(':'); const auto item=value();
                    if (!tokens_.emplace(id,item).second) fail("Duplicate token: "+id,item.position); }
                continue;
            }
            Block block;
            do { block.selectors.push_back(selector()); } while (take(','));
            expect('{');
            while (!take('}')) { const auto id=identifier(); expect(':'); block.declarations.push_back({id,value()}); }
            blocks_.push_back(std::move(block));
            if (blocks_.size()>1024) fail("Too many style blocks");
        }
        for (const auto& [name,item]:tokens_) { (void)name; resolve(item); }
        std::vector<StyleRule> rules;
        for (const auto& block:blocks_) {
            StyleProperties properties;
            for (const auto& d:block.declarations) property(properties,d);
            for (const auto& selector:block.selectors) {
                if (selector.states!=StyleState::none && layout_properties(properties))
                    fail("State selectors may only change paint properties",block.declarations.empty()?0:block.declarations.front().value.position);
                rules.push_back({selector,properties});
                if (rules.size()>4096) fail("Too many style rules");
            }
        }
        return rules;
    }
};
}

bool StyleSheet::valid_class(std::string_view name) noexcept {
    if (name.empty() || name.size()>64 || (name.front()>='0' && name.front()<='9')) return false;
    return std::all_of(name.begin(),name.end(),[](char c){return (c>='a'&&c<='z') || (c>='A'&&c<='Z') || (c>='0'&&c<='9') || c=='_' || c=='-';});
}
bool StyleSheet::valid_properties(const StyleProperties& p) noexcept {
    if (p.image && *p.image) {
        const auto& image=**p.image;
        if (!image.identity || !image.width || !image.height || image.width>16384 || image.height>16384 ||
            std::uint64_t(image.row_pitch)<std::uint64_t(image.width)*4 ||
            std::uint64_t(image.row_pitch)*image.height>image.pixels.size()) return false;
    }
    if ((p.font_family && !name_valid(*p.font_family)) || (p.font_locale && !name_valid(*p.font_locale)) ||
        (p.font_size && !bounded(*p.font_size,4,256)) || (p.font_weight && (!*p.font_weight || *p.font_weight>999))) return false;
    for (const auto* color:{&p.foreground,&p.background,&p.border,&p.selection,&p.caret,&p.scrollbar}) if (*color && !color_valid(**color)) return false;
    if (p.padding) for (auto n:{p.padding->left,p.padding->top,p.padding->right,p.padding->bottom}) if (!bounded(n,0,128)) return false;
    if (p.radius) for (auto n:{p.radius->top_left,p.radius->top_right,p.radius->bottom_right,p.radius->bottom_left}) if (!bounded(n,0,32768)) return false;
    return (!p.border_width || bounded(*p.border_width,0,128)) && (!p.caret_width || bounded(*p.caret_width,.25f,16)) &&
        (!p.scrollbar_width || bounded(*p.scrollbar_width,.25f,32)) && (!p.scrollbar_inset || bounded(*p.scrollbar_inset,0,32)) &&
        (!p.scrollbar_min_thumb || bounded(*p.scrollbar_min_thumb,1,128));
}
StyleSheet::Result StyleSheet::create(std::vector<StyleRule> rules) {
    if (rules.size()>4096) return std::unexpected(StyleDiagnostic{0,0,"Too many style rules"});
    for (auto& rule:rules) {
        auto& s=rule.selector;
        const auto bits=static_cast<unsigned>(s.states);
        if (!valid_properties(rule.properties) || (s.kind && static_cast<unsigned>(*s.kind)>static_cast<unsigned>(ControlKind::foldout)) ||
            (s.role && static_cast<unsigned>(*s.role)>static_cast<unsigned>(Role::caption)) || bits>255 || (bits&3)==3 ||
            (bits && layout_properties(rule.properties)) || s.classes.size()>16 ||
            !std::all_of(s.classes.begin(),s.classes.end(),[](const auto& id){return valid_class(id);}))
            return std::unexpected(StyleDiagnostic{0,0,"Invalid selector or style properties"});
        std::sort(s.classes.begin(),s.classes.end());
        s.classes.erase(std::unique(s.classes.begin(),s.classes.end()),s.classes.end());
    }
    std::stable_sort(rules.begin(),rules.end(),[](const auto& a,const auto& b){return specificity(a.selector)<specificity(b.selector);});
    return std::shared_ptr<const StyleSheet>(new StyleSheet(std::move(rules)));
}
StyleSheet::Result StyleSheet::parse(std::string_view source,std::span<const NamedImage> images) {
    if (source.size()>1024*1024 || source.find('\0')!=source.npos) return std::unexpected(StyleDiagnostic{1,1,"Invalid or oversized style source"});
    try { return create(Parser(source,images).run()); }
    catch (const ParseFailure& failure) {
        StyleDiagnostic error{1,1,failure.message};
        for (std::size_t i=0;i<std::min(failure.position,source.size());++i) {
            if (source[i]=='\n') { ++error.line; error.column=1; } else ++error.column;
        }
        return std::unexpected(std::move(error));
    }
}
} // namespace atelier::ui
