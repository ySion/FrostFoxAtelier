#include "probe_api.h"
#include <Windows.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <mimalloc.h>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
static double micros(Clock::duration d) { return std::chrono::duration<double, std::micro>(d).count(); }
static void require(bool ok, std::string_view message) { if (!ok) throw std::runtime_error(std::string(message)); }
static void hr_check(HRESULT hr, const char* where) { if (FAILED(hr)) throw std::runtime_error(std::string(where) + ": " + std::to_string(static_cast<unsigned long>(hr))); }
static bool nearly_equal(float a, float b) { return std::abs(a-b) <= 0.08f + 0.000002f*std::max(std::abs(a),std::abs(b)); }
static std::string escaped(std::string_view s) {
    std::string r;
    for (const unsigned char c : s) {
        if (c=='"' || c=='\\') { r.push_back('\\'); r.push_back(static_cast<char>(c)); }
        else if (c=='\n') r += "\\n";
        else if (c=='\r') r += "\\r";
        else if (c<32) { char b[8]{}; std::snprintf(b,sizeof(b),"\\u%04x",static_cast<unsigned>(c)); r += b; }
        else r.push_back(static_cast<char>(c));
    }
    return r;
}

struct MeasureKey {
    std::string text;
    uint32_t font{},width{},height{},wm{},hm{},revision{};
    bool operator==(const MeasureKey&) const = default;
};
struct MeasureHash {
    size_t operator()(const MeasureKey& k) const noexcept {
        size_t h=std::hash<std::string>{}(k.text);
        for (auto v : {k.font,k.width,k.height,k.wm,k.hm,k.revision}) h ^= static_cast<size_t>(v)+0x9e3779b9u+(h<<6)+(h>>2);
        return h;
    }
};
struct TextProvider {
    ComPtr<IDWriteFactory> factory;
    std::unordered_map<uint32_t,ComPtr<IDWriteTextFormat>> formats;
    std::unordered_map<MeasureKey,LpMeasured,MeasureHash> cache;
    uint64_t calls{},shapes{},bytes{},callback_bytes{},fail_at{};
    double callback_us{},shape_us{};
    float font_scale{1};
    uint32_t font_revision{};
    bool use_cache{true},try_reentry{},reentry_attempted{},reentry_rejected{};
    void* active_engine{};
    std::string error;
    TextProvider() {
        hr_check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(factory.GetAddressOf())),"DWriteCreateFactory");
        ComPtr<IDWriteFontCollection> collection;
        hr_check(factory->GetSystemFontCollection(collection.GetAddressOf()),"font collection");
        UINT32 index{}; BOOL found{};
        hr_check(collection->FindFamilyName(L"Segoe UI",&index,&found),"font family");
        require(found!=FALSE,"Segoe UI unavailable; no silent font substitution");
    }
    LpMeasured get(const LpMeasureRequest& r) {
        require(r.text!=nullptr || r.text_length==0,"null text");
        require(std::isfinite(r.font_size) && r.font_size>0,"invalid font size");
        const float fs=r.font_size*font_scale;
        MeasureKey key{std::string(r.text ? r.text : "",r.text_length),std::bit_cast<uint32_t>(fs),std::bit_cast<uint32_t>(r.width),std::bit_cast<uint32_t>(r.height),r.width_mode,r.height_mode,font_revision};
        if (use_cache) if (const auto it=cache.find(key);it!=cache.end()) return it->second;
        const auto start=Clock::now();
        ++shapes; bytes+=r.text_length;
        const auto size_key=std::bit_cast<uint32_t>(fs);
        auto it=formats.find(size_key);
        if (it==formats.end()) {
            ComPtr<IDWriteTextFormat> format;
            hr_check(factory->CreateTextFormat(L"Segoe UI",nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,fs,L"en-US",format.GetAddressOf()),"CreateTextFormat");
            hr_check(format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP),"SetWordWrapping");
            it=formats.emplace(size_key,std::move(format)).first;
        }
        require(r.text_length<=static_cast<uint32_t>(std::numeric_limits<int>::max()),"text too large");
        const int len=static_cast<int>(r.text_length);
        const int n=len ? MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,r.text,len,nullptr,0) : 0;
        require(len==0 || n>0,"invalid UTF-8 content");
        std::wstring wide(static_cast<size_t>(n),L'\0');
        if(n) require(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,r.text,len,wide.data(),n)==n,"UTF-8 conversion");
        const float w=(r.width_mode==LP_UNBOUNDED || r.width_mode==LP_MIN_CONTENT) ? 1000000.0f : r.width;
        const float h=r.height_mode==LP_UNBOUNDED ? 1000000.0f : r.height;
        require(std::isfinite(w)&&w>=0&&std::isfinite(h)&&h>=0,"invalid measure constraints");
        ComPtr<IDWriteTextLayout> layout;
        hr_check(factory->CreateTextLayout(wide.data(),static_cast<UINT32>(wide.size()),it->second.Get(),w,h,layout.GetAddressOf()),"CreateTextLayout");
        float min_width{};
        if(r.width_mode==LP_MIN_CONTENT) {
            hr_check(layout->DetermineMinWidth(&min_width),"DetermineMinWidth");
            hr_check(layout->SetMaxWidth(std::max(min_width,0.001f)),"SetMaxWidth");
        }
        DWRITE_TEXT_METRICS metrics{};
        hr_check(layout->GetMetrics(&metrics),"GetMetrics");
        UINT32 count{};
        const auto line_hr=layout->GetLineMetrics(nullptr,0,&count);
        require(SUCCEEDED(line_hr) || line_hr==E_NOT_SUFFICIENT_BUFFER,"line metrics query");
        std::vector<DWRITE_LINE_METRICS> lines(count);
        if(count) hr_check(layout->GetLineMetrics(lines.data(),count,&count),"GetLineMetrics");
        LpMeasured out{r.width_mode==LP_MIN_CONTENT ? min_width : metrics.widthIncludingTrailingWhitespace,metrics.height,count ? lines[0].baseline : 0,count};
        if(r.width_mode==LP_EXACT) out.width=r.width;
        else if(r.width_mode==LP_AT_MOST) out.width=std::min(out.width,r.width);
        if(r.height_mode==LP_EXACT) out.height=r.height;
        else if(r.height_mode==LP_AT_MOST) out.height=std::min(out.height,r.height);
        shape_us+=micros(Clock::now()-start);
        if(use_cache) cache.emplace(std::move(key),out);
        return out;
    }
    static int32_t callback(void* raw,const LpMeasureRequest* request,LpMeasured* result) noexcept {
        auto& self=*static_cast<TextProvider*>(raw);
        const auto start=Clock::now(); ++self.calls;
        int32_t status=0;
        try {
            if(self.try_reentry) {
                self.try_reentry=false; self.reentry_attempted=true;
                self.reentry_rejected=lp_layout(self.active_engine,100,100)!=0;
            }
            if(self.fail_at && self.calls==self.fail_at) throw std::runtime_error("injected provider failure after real preceding measurements");
            require(request && result,"null measure request");
            self.callback_bytes+=request->text_length;
            *result=self.get(*request);
        } catch(const std::exception& e) { self.error=e.what(); status=-1; }
        catch(...) { self.error="unknown provider error"; status=-1; }
        self.callback_us+=micros(Clock::now()-start);
        return status;
    }
    LpMeasured direct(std::string_view text,float size,float width,uint32_t mode=LP_AT_MOST) {
        LpMeasureRequest q{text.data(),static_cast<uint32_t>(text.size()),size,width,0,mode,LP_UNBOUNDED};
        return get(q);
    }
};

struct Scene {
    std::vector<LpNode> nodes;
    std::vector<std::string> texts;
    uint64_t revision{1},next_id{1};
    float width{480},height{2000};
    uint32_t add(uint32_t parent,uint32_t kind=LP_CONTAINER,uint32_t flow=LP_COLUMN) {
        LpNode n{}; n.id=next_id++; n.parent=parent; n.kind=kind; n.flow=flow;
        n.width={LP_GROW,0}; n.height={LP_AUTO,0}; n.font_size=16; n.content_revision=1; n.measure_revision=1;
        nodes.push_back(n); texts.emplace_back(); return static_cast<uint32_t>(nodes.size()-1);
    }
    uint32_t box(uint32_t parent,float w,float h) {
        const auto i=add(parent,LP_BOX); nodes[i].width={LP_FIXED,w}; nodes[i].height={LP_FIXED,h};return i;
    }
    uint32_t text(uint32_t parent,std::string value,float size=16) {
        const auto i=add(parent,LP_TEXT);texts[i]=std::move(value);nodes[i].font_size=size;return i;
    }
    void bind() { for(size_t i=0;i<nodes.size();++i) { nodes[i].text=texts[i].data(); nodes[i].text_length=static_cast<uint32_t>(texts[i].size()); } }
    void changed(uint32_t i) { ++revision; ++nodes[i].content_revision; }
};
struct Session {
    TextProvider provider;
    void* engine{};
    uint32_t capacity{};
    Session() { engine=lp_create(TextProvider::callback,&provider);require(engine!=nullptr,"candidate create failed");provider.active_engine=engine;
        require(mi_is_in_heap_region(engine),"candidate engine allocation bypasses the shared mimalloc instance");
        require(lp_sizeof_node()==sizeof(LpNode)&&lp_sizeof_box()==sizeof(LpBox),"cross-language ABI size mismatch"); }
    ~Session(){if(engine)lp_destroy(engine);}
    Session(const Session&)=delete;
    Session& operator=(const Session&)=delete;
    void apply(Scene& s) { s.bind();capacity=std::max(capacity,static_cast<uint32_t>(s.nodes.size()));const auto status=lp_apply(engine,s.nodes.data(),static_cast<uint32_t>(s.nodes.size()),s.revision);require(status==0,std::string("apply: ")+lp_last_error(engine)); }
    void layout(const Scene& s) { const auto status=lp_layout(engine,s.width,s.height);require(status==0,std::string("layout: ")+lp_last_error(engine)+" provider="+provider.error); }
    std::vector<LpBox> boxes() const { std::vector<LpBox> b(capacity);const auto n=lp_copy_boxes(engine,b.data(),capacity);require(n<=capacity,"output count");b.resize(n);return b; }
    std::vector<LpBox> run(Scene& s){apply(s);layout(s);return boxes();}
};
static void compare(const std::vector<LpBox>& a,const std::vector<LpBox>& b,const char* label) {
    require(a.size()==b.size(),std::string(label)+" box count");
    for(size_t i=0;i<a.size();++i) require(a[i].id==b[i].id && a[i].parent_id==b[i].parent_id && nearly_equal(a[i].x,b[i].x)&&nearly_equal(a[i].y,b[i].y)&&nearly_equal(a[i].width,b[i].width)&&nearly_equal(a[i].height,b[i].height),std::string(label)+" id="+std::to_string(a[i].id)+" expected=("+std::to_string(b[i].x)+","+std::to_string(b[i].y)+","+std::to_string(b[i].width)+","+std::to_string(b[i].height)+") actual=("+std::to_string(a[i].x)+","+std::to_string(a[i].y)+","+std::to_string(a[i].width)+","+std::to_string(a[i].height)+")");
}
static void fresh_equivalent(Session& warm,Scene& s,const char* label) {
    Session fresh;fresh.provider.font_scale=warm.provider.font_scale;fresh.provider.font_revision=warm.provider.font_revision;
    compare(warm.boxes(),fresh.run(s),label);
}
static Scene inspector(uint32_t rows=12,bool international=false) {
    Scene s;s.add(UINT32_MAX,LP_CONTAINER,LP_GRID_TWO);s.nodes[0].padding=8;s.nodes[0].gap=6;
    for(uint32_t i=0;i<rows;++i) {
        s.text(0,i%3==0 ? "Long property label" : "Value");
        s.text(0,international && i%3==0 ? "这是没有空格的中文内容用于验证窄列自动换行和高度变化。" : (i%3==1 ? "short" : "The quick brown fox jumps over the lazy dog while the inspector changes width."),i%4==0?20.0f:16.0f);
    }
    return s;
}
static void set_tracks(Scene& s,TextProvider& p) {
    float maximum=0;
    for(size_t i=1;i<s.nodes.size();i+=2) maximum=std::max(maximum,p.direct(s.texts[i],s.nodes[i].font_size,0,LP_UNBOUNDED).width);
    s.nodes[0].first_column=std::clamp(maximum,80.0f,180.0f);
}
static void check_inspector(Session& session,Scene& s) {
    const auto b=session.boxes();require(b.size()==s.nodes.size(),"inspector output count");
    const auto& root=s.nodes[0];float y=root.padding;
    const float second=s.width-2*root.padding-root.first_column-root.gap;
    require(second>0,"fixture has valid remaining width");
    for(size_t i=1;i<s.nodes.size();i+=2) {
        const auto left=session.provider.direct(s.texts[i],s.nodes[i].font_size,root.first_column);
        const auto right=session.provider.direct(s.texts[i+1],s.nodes[i+1].font_size,second);
        const auto expected=std::max(left.height,right.height);
        require(nearly_equal(b[i].x,root.padding)&&nearly_equal(b[i+1].x,root.padding+root.first_column+root.gap),"shared column x id="+std::to_string(b[i].id));
        require(nearly_equal(b[i].width,root.first_column)&&nearly_equal(b[i+1].width,second),"shared column widths");
        require(nearly_equal(b[i].y,y)&&nearly_equal(b[i+1].y,y),"row y id="+std::to_string(b[i].id)+" expected="+std::to_string(y)+" actual="+std::to_string(b[i].y));
        require(nearly_equal(std::max(b[i].height,b[i+1].height),expected),"DWrite row height id="+std::to_string(b[i].id)+" expected="+std::to_string(expected)+" actual="+std::to_string(std::max(b[i].height,b[i+1].height)));
        y+=expected+root.gap;
    }
}

static void case_inspector(bool international) {
    Session x;auto s=inspector(8,international);set_tracks(s,x.provider);x.run(s);check_inspector(x,s);
    s.width=300;++s.revision;x.run(s);check_inspector(x,s);fresh_equivalent(x,s,"resize warm/fresh");
    s.texts[1]="An exceptionally long label outside the original maximum";s.changed(1);set_tracks(s,x.provider);x.run(s);check_inspector(x,s);
    s.texts[4]="A changed value with a longer validation message that wraps into more lines.";s.changed(4);x.run(s);check_inspector(x,s);fresh_equivalent(x,s,"content warm/fresh");
    x.provider.font_scale=1.15f;++x.provider.font_revision;++s.revision;for(auto& n:s.nodes)++n.measure_revision;set_tracks(s,x.provider);x.run(s);check_inspector(x,s);fresh_equivalent(x,s,"font warm/fresh");
    s.nodes.erase(s.nodes.begin()+1,s.nodes.begin()+3);s.texts.erase(s.texts.begin()+1,s.texts.begin()+3);++s.revision;set_tracks(s,x.provider);x.run(s);check_inspector(x,s);fresh_equivalent(x,s,"removed maximum");
    std::swap(s.nodes[1],s.nodes[3]);std::swap(s.nodes[2],s.nodes[4]);std::swap(s.texts[1],s.texts[3]);std::swap(s.texts[2],s.texts[4]);++s.revision;x.run(s);check_inspector(x,s);fresh_equivalent(x,s,"reordered rows");
}
static void case_hud() {
    Session x;Scene s;s.width=800;s.height=450;s.add(UINT32_MAX);
    auto a=s.box(0,100,30);s.nodes[a].absolute=1;s.nodes[a].left=10;s.nodes[a].top=10;
    auto b=s.box(0,80,40);s.nodes[b].absolute=1;s.nodes[b].left=s.width-90;s.nodes[b].top=s.height-50;
    auto c=s.add(0);s.nodes[c].absolute=1;s.nodes[c].left=0.4f;s.nodes[c].top=0.4f;s.nodes[c].width={LP_FIXED,100};s.nodes[c].height={LP_FIXED,100};
    auto d=s.box(c,10,10);s.nodes[d].absolute=1;s.nodes[d].left=0.4f;s.nodes[d].top=0.4f;
    auto boxes=x.run(s);require(nearly_equal(boxes[a].x,10)&&nearly_equal(boxes[b].x,710)&&nearly_equal(boxes[b].y,400),"HUD anchors");
    require(nearly_equal(boxes[c].x+boxes[d].x,0.8f),"nested logical precision");
    const auto preserved=boxes;for(float dpi:{1.0f,1.5f,2.0f}){const float px=std::round((boxes[c].x+boxes[d].x)*dpi);require(std::isfinite(px),"pixel consumption");}
    compare(preserved,x.boxes(),"DPI does not mutate layout");
    s.width=1280;s.height=720;s.nodes[b].left=s.width-90;s.nodes[b].top=s.height-50;++s.revision;x.run(s);fresh_equivalent(x,s,"HUD resize");
    s.nodes[c].padding=3;++s.revision;const auto padded=x.run(s);require(nearly_equal(padded[d].x,3.4f)&&nearly_equal(padded[d].y,3.4f),"absolute content-origin padding");fresh_equivalent(x,s,"absolute padded parent");
}
static void case_wrap() {
    Session x;Scene s;s.width=130;s.height=400;s.add(UINT32_MAX,LP_CONTAINER,LP_WRAP);s.nodes[0].padding=5;s.nodes[0].gap=4;
    for(int i=0;i<11;++i)s.box(0,30,20);
    for(float w:{130.0f,150.0f}) {s.width=w;++s.revision;const auto b=x.run(s);const auto cols=static_cast<uint32_t>(std::floor((w-10+4)/34));
        for(uint32_t i=0;i<11;++i)require(nearly_equal(b[i+1].x,5+34.0f*static_cast<float>(i%cols))&&nearly_equal(b[i+1].y,5+24.0f*static_cast<float>(i/cols)),"wrapped grid geometry index="+std::to_string(i));
        fresh_equivalent(x,s,"wrap resize");}
}
static void case_failure() {
    // Safety must remain testable when a candidate's paragraph wrapping differs.
    Session x;auto s=inspector(6);for(size_t i=1;i<s.texts.size();++i)s.texts[i]=i%2?"Label":"Value";set_tracks(s,x.provider);x.run(s);const auto old=x.boxes();const auto revision=lp_output_revision(x.engine);
    const auto valid=s.texts[4];s.texts[4]=std::string("\xc3(",2);s.changed(4);x.apply(s);
    require(lp_layout(x.engine,s.width,s.height)!=0,"invalid real UTF-8 must fail measurement");
    require(lp_output_revision(x.engine)==revision,"failed measure must not publish revision");compare(old,x.boxes(),"failed measure preserves boxes");
    s.texts[4]=valid;s.changed(4);x.run(s);check_inspector(x,s);fresh_equivalent(x,s,"error recovery");
    ++s.revision;for(auto& n:s.nodes)++n.measure_revision;x.apply(s);x.provider.try_reentry=true;x.provider.fail_at=x.provider.calls+3;
    const auto before=x.boxes();const auto before_revision=lp_output_revision(x.engine);
    require(lp_layout(x.engine,s.width,s.height)!=0,"third provider measure failure");
    require(x.provider.reentry_attempted&&x.provider.reentry_rejected,"synchronous reentry rejection");
    require(lp_output_revision(x.engine)==before_revision,"injected failure publication");compare(before,x.boxes(),"injected failure preserves mapping");
    x.provider.fail_at=0;++s.revision;for(auto& n:s.nodes)++n.measure_revision;x.run(s);fresh_equivalent(x,s,"failed cache retry");
    const auto delayed=old;s.texts[2]="replacement";s.changed(2);x.run(s);require(!delayed.empty()&&delayed[1].id==old[1].id,"two-frame owned geometry remains readable");
}
static void case_invalid() {
    Session x;Scene s;s.add(UINT32_MAX);s.box(0,40,20);x.run(s);const auto before=x.boxes();
    auto bad=s;bad.nodes[1].parent=1;bad.bind();require(lp_apply(x.engine,bad.nodes.data(),static_cast<uint32_t>(bad.nodes.size()),2)!=0,"cycle rejection");
    bad=s;bad.nodes[1].width.value=std::numeric_limits<float>::quiet_NaN();bad.bind();require(lp_apply(x.engine,bad.nodes.data(),static_cast<uint32_t>(bad.nodes.size()),2)!=0,"nonfinite rejection");
    bad=s;bad.nodes[1].id=bad.nodes[0].id;bad.bind();require(lp_apply(x.engine,bad.nodes.data(),static_cast<uint32_t>(bad.nodes.size()),2)!=0,"duplicate identity rejection");compare(before,x.boxes(),"invalid batch preserves result");x.layout(s);compare(before,x.boxes(),"invalid batch preserves active tree");
}
static void case_custom() {
    Session x;Scene s;s.width=400;s.height=600;s.add(UINT32_MAX);auto fixed=s.add(0);s.nodes[fixed].width={LP_FIXED,180};s.nodes[fixed].height={LP_FIXED,160};
    s.text(fixed,"A fixed panel contains real wrapped text without changing its outer dimensions.");
    auto natural=s.add(0);s.nodes[natural].width={LP_FIXED,180};s.text(natural,"Content determines this panel height and must respond when the parent width changes.");
    auto first=x.run(s);require(nearly_equal(first[fixed].height,160),"fixed container boundary");require(first[natural].height>0,"natural container height");
    s.nodes[natural].width.value=120;++s.revision;x.run(s);fresh_equivalent(x,s,"custom width dependency");
    // An explicit host dependency loop: candidate computes each proposed fixed box;
    // the host owns and rejects the cyclic next-constraint rule, not the solver.
    Scene loop;loop.add(UINT32_MAX);auto child=loop.box(0,220,20);loop.width=180;std::vector<float> seen;bool rejected=false;
    for(int i=0;i<8;++i){if(std::find(seen.begin(),seen.end(),loop.width)!=seen.end()){rejected=true;break;}seen.push_back(loop.width);loop.nodes[child].width.value=loop.width<200?220.0f:180.0f;++loop.revision;const auto result=x.run(loop);loop.width=result[child].width;}
    require(rejected,"host cyclic constraint policy rejects rather than iterates forever");
}
static void case_virtual() {
    constexpr uint32_t count=10000;
    Session x;std::vector<std::string> labels(count),values(count);std::vector<float> heights(count);std::vector<double> prefix(count+1);
    for(uint32_t i=0;i<count;++i){labels[i]=i%7?"Label":"Long property";values[i]=i%5?"A real row value that wraps when the editor narrows its column.":"short";}
    labels[7500]="The unique longest off-screen label controls the global shared track";
    float viewport_width=440,track{};
    auto update_index=[&]{track=0;for(const auto& label:labels)track=std::max(track,x.provider.direct(label,16,0,LP_UNBOUNDED).width);track=std::clamp(track,80.0f,180.0f);prefix[0]=0;
        for(uint32_t i=0;i<count;++i){heights[i]=std::max(x.provider.direct(labels[i],16,track).height,x.provider.direct(values[i],16,viewport_width-track-6).height);prefix[i+1]=prefix[i]+heights[i]+6;}};
    update_index();
    uint32_t anchor=5000;const double inset=3;
    for(int pass=0;pass<3;++pass){
        if(pass==1){labels[7500]="tiny";viewport_width=300;update_index();}
        if(pass==2){x.provider.font_scale=1.1f;++x.provider.font_revision;update_index();}
        const double scroll=prefix[anchor]+inset;
        const auto first=static_cast<uint32_t>(std::upper_bound(prefix.begin(),prefix.end(),scroll)-prefix.begin()-1);
        require(first==anchor,"variable-height anchor policy");
        uint32_t end=first;while(end<count && prefix[end]<scroll+320+100)++end;
        Scene s;s.width=viewport_width;s.height=320;s.revision=static_cast<uint64_t>(pass+1);s.add(UINT32_MAX);
        const auto before=s.box(0,viewport_width,static_cast<float>(prefix[first]));(void)before;
        const auto grid=s.add(0,LP_CONTAINER,LP_GRID_TWO);s.nodes[grid].first_column=track;s.nodes[grid].gap=6;
        for(uint32_t i=first;i<end;++i){auto li=s.text(grid,labels[i]);auto vi=s.text(grid,values[i]);s.nodes[li].id=100000+2ULL*i;s.nodes[vi].id=100001+2ULL*i;}
        // Grid has no trailing gap; reserve the same logical row spacing in tail.
        const auto tail=s.box(0,viewport_width,static_cast<float>(prefix[count]-prefix[end]+(end>first?6:0)));
        const auto b=x.run(s);require(nearly_equal(b[grid].y,static_cast<float>(prefix[first])),"virtual prefix spacer");
        require(nearly_equal(b[tail].y+b[tail].height,static_cast<float>(prefix[count])),"virtual content extent");
        require(s.nodes.size()<120,"windowed instances not logical-size tree");fresh_equivalent(x,s,"virtual warm/fresh");
        std::printf("{\"kind\":\"virtualization\",\"candidate\":\"%s\",\"pass\":%d,\"logical_items\":%u,\"instantiated_nodes\":%zu,\"visible_start\":%u,\"window_end\":%u,\"extent\":%.3f,\"measure_calls\":%llu,\"shapes\":%llu}\n",lp_name(),pass,count,s.nodes.size(),first,end,prefix[count],static_cast<unsigned long long>(x.provider.calls),static_cast<unsigned long long>(x.provider.shapes));
    }
}
static void case_capacity() {
    Session x;Scene s;s.width=480;s.height=1000000;s.add(UINT32_MAX);const auto leaf=s.text(0,"short");x.run(s);const auto retained=x.boxes();
    std::string paragraph;paragraph.reserve(40000);for(uint32_t i=0;i<8000;++i)paragraph+="word ";
    s.texts[leaf]=std::move(paragraph);s.changed(leaf);const auto large=x.run(s);
    require(large.size()==s.nodes.size()&&std::isfinite(large[leaf].height)&&large[leaf].height>retained[leaf].height,"long text capacity growth");
    fresh_equivalent(x,s,"long text cache capacity warm/fresh");
    s.texts[leaf]="short";s.changed(leaf);x.run(s);compare(retained,x.boxes(),"capacity shrink preserves short result");
}
static double percentile(std::vector<double> a,double p){std::sort(a.begin(),a.end());return a[std::min(a.size()-1,static_cast<size_t>(p*static_cast<double>(a.size()-1)))];}
static void saturation_path() {
    Session x;Scene s;s.width=300;s.height=40;s.add(UINT32_MAX,LP_CONTAINER,LP_ROW);
    for(int i=0;i<3;++i){const auto child=s.box(0,0,10);s.nodes[child].width={LP_GROW,0};if(i!=1){s.nodes[child].has_max_width=1;s.nodes[child].max_width=i==0?120.0f:30.0f;}}
    for(int order=0;order<2;++order){
        const auto b=x.run(s);float widths[3]{};
        for(size_t i=1;i<b.size();++i)widths[b[i].id-2]=b[i].width;
        const float total=widths[0]+widths[1]+widths[2];
        std::printf("{\"kind\":\"diagnostic\",\"candidate\":\"%s\",\"name\":\"equal_grow_max_saturation\",\"order\":%d,\"profile\":\"remaining_space_redistributed\",\"expected_B_C_A\":[120,150,30],\"actual_B_C_A\":[%.6f,%.6f,%.6f],\"used_width\":%.6f,\"fills_available\":%s}\n",lp_name(),order,widths[0],widths[1],widths[2],total,nearly_equal(total,300)?"true":"false");
        std::swap(s.nodes[1],s.nodes[3]);++s.revision;
    }
}
static void depth_path(uint32_t depth) {
    require(depth>0&&depth<=10000,"depth experiment range");Session x;Scene s;s.width=200;s.height=1000;
    uint32_t parent=s.add(UINT32_MAX);
    for(uint32_t i=0;i<depth;++i)parent=s.add(parent);
    s.text(parent,"A real measured leaf at the end of the deep chain.");
    const auto boxes=x.run(s);require(boxes.size()==s.nodes.size()&&boxes.back().height>0,"deep path result");
    std::printf("{\"kind\":\"depth\",\"candidate\":\"%s\",\"depth\":%u,\"nodes\":%zu,\"status\":\"PASS\",\"leaf_height\":%.6f}\n",lp_name(),depth,boxes.size(),boxes.back().height);
}
static void benchmark(uint32_t node_count,uint32_t iterations,bool text) {
    require(iterations>=10,"benchmark needs >=10 samples");Session x;Scene s;
    if(text){s=inspector(std::max(1u,node_count/2));set_tracks(s,x.provider);}
    else{s.width=800;s.height=1000;s.add(UINT32_MAX);for(uint32_t i=0;i<node_count;++i)s.box(0,100,20);}
    const auto check_boxes=[&]{const auto b=x.boxes();require(b.size()==s.nodes.size(),"box fixture count");for(size_t i=1;i<b.size();++i)require(b[i].id==s.nodes[i].id&&nearly_equal(b[i].x,0)&&nearly_equal(b[i].y,20.0f*static_cast<float>(i-1))&&nearly_equal(b[i].width,s.nodes[i].width.value)&&nearly_equal(b[i].height,20),"fixed box benchmark golden");};
    x.run(s);if(text)check_inspector(x,s);else check_boxes();fresh_equivalent(x,s,"benchmark precondition");
    std::vector<LpBox> sink(s.nodes.size());
    require(lp_copy_boxes(x.engine,sink.data(),static_cast<uint32_t>(sink.size()))==sink.size(),"initial published output");
    for(const std::string_view mode:{"static_skip","cold_solver_warm_fonts","clean_refresh","single_leaf","root_resize","structure_reorder","high_change"}) {
        std::vector<double> totals,prepares,layouts,exports,callbacks,layout_self;
        for(auto* v:{&totals,&prepares,&layouts,&exports,&callbacks,&layout_self})v->reserve(iterations);
        uint64_t calls=0,shapes=0,shape_bytes=0,callback_bytes=0,input_nodes=0,input_text_bytes=0,output_bytes=0,rebuilds=0,layout_calls=0,api_calls=0;
        double checksum=0,shape_us=0;
        for(uint32_t sample=0;sample<iterations+20;++sample){
            // Administrative counters are outside the measured API transaction.
            const auto stats0=mode=="cold_solver_warm_fonts"?LpStats{}:lp_stats(x.engine);
            const auto c0=x.provider.calls,sh0=x.provider.shapes,b0=x.provider.bytes,cb0=x.provider.callback_bytes;
            const auto shape0=x.provider.shape_us,callback0=x.provider.callback_us;
            uint64_t sample_api_calls=0;
            const auto t0=Clock::now();
            bool apply=false;
            if(mode=="cold_solver_warm_fonts") {lp_destroy(x.engine);x.engine=lp_create(TextProvider::callback,&x.provider);require(x.engine!=nullptr,"cold create");x.provider.active_engine=x.engine;apply=true;sample_api_calls+=2;}
            if(mode=="single_leaf") {++s.revision;apply=true;if(text){s.texts[2]=(sample%2)?"A changing real value with content reflow in the inspector.":"short";++s.nodes[2].content_revision;}else{s.nodes.back().width.value=sample%2?101.0f:100.0f;}}
            if(mode=="root_resize")s.width=(sample%2)?480.0f:420.0f;
            if(mode=="structure_reorder"&&s.nodes.size()>4){apply=true;++s.revision;if(text){std::swap(s.nodes[1],s.nodes[3]);std::swap(s.nodes[2],s.nodes[4]);std::swap(s.texts[1],s.texts[3]);std::swap(s.texts[2],s.texts[4]);}else{std::swap(s.nodes[1],s.nodes.back());}}
            if(mode=="high_change"){apply=true;++s.revision;for(size_t i=1;i<s.nodes.size();++i){if(text){if(i%2==0){s.texts[i]="Frame "+std::to_string(sample)+" property "+std::to_string(i)+" has a changing real value that wraps in the inspector.";++s.nodes[i].content_revision;}}else{s.nodes[i].width.value=100.0f+static_cast<float>((sample+i)%3);}}}
            if(apply){s.bind();++sample_api_calls;if(lp_apply(x.engine,s.nodes.data(),static_cast<uint32_t>(s.nodes.size()),s.revision)!=0)throw std::runtime_error(lp_last_error(x.engine));}
            const auto t1=Clock::now();
            if(mode!="static_skip"){++sample_api_calls;if(lp_layout(x.engine,s.width,s.height)!=0)throw std::runtime_error(lp_last_error(x.engine));}
            const auto t2=Clock::now();
            if(mode!="static_skip"){++sample_api_calls;require(lp_copy_boxes(x.engine,sink.data(),static_cast<uint32_t>(sink.size()))==sink.size(),"benchmark output");}
            checksum+=sink.back().x+sink.back().y+sink.back().width+sink.back().height;
            const auto t3=Clock::now();
            if(sample>=20){
                const double callback=x.provider.callback_us-callback0;
                totals.push_back(micros(t3-t0));prepares.push_back(micros(t1-t0));layouts.push_back(micros(t2-t1));exports.push_back(micros(t3-t2));callbacks.push_back(callback);layout_self.push_back(std::max(0.0,micros(t2-t1)-callback));
                const auto stats=lp_stats(x.engine);rebuilds+=stats.rebuilds-stats0.rebuilds;layout_calls+=stats.layouts-stats0.layouts;input_nodes+=stats.input_nodes-stats0.input_nodes;
                calls+=x.provider.calls-c0;shapes+=x.provider.shapes-sh0;shape_bytes+=x.provider.bytes-b0;callback_bytes+=x.provider.callback_bytes-cb0;shape_us+=x.provider.shape_us-shape0;api_calls+=sample_api_calls;
                if(apply)for(const auto& value:s.texts)input_text_bytes+=value.size();
                if(mode!="static_skip")output_bytes+=sink.size()*sizeof(LpBox);
            }
            if(sample==0){if(text)check_inspector(x,s);else check_boxes();fresh_equivalent(x,s,"first changed benchmark state");}
        }
        size_t elapsed{},user{},system{},rss{},peak_rss{},commit{},peak_commit{},faults{};
        mi_process_info(&elapsed,&user,&system,&rss,&peak_rss,&commit,&peak_commit,&faults);
        // A failed dynamic mode must never emit an apparently valid timing record.
        if(text)check_inspector(x,s);else check_boxes();fresh_equivalent(x,s,"post-benchmark correctness");
        std::printf("{\"kind\":\"benchmark\",\"validated\":true,\"candidate\":\"%s\",\"fixture\":\"%s\",\"mode\":\"%.*s\",\"input_nodes\":%zu,\"samples\":%u,\"warmup\":20,",lp_name(),text?"latin_inspector":"fixed_boxes",static_cast<int>(mode.size()),mode.data(),s.nodes.size(),iterations);
        std::printf("\"median_us\":%.6f,\"p95_us\":%.6f,\"p99_us\":%.6f,\"prepare_apply_median_us\":%.6f,\"layout_inclusive_median_us\":%.6f,\"layout_excluding_callback_median_us\":%.6f,\"export_consume_median_us\":%.6f,",percentile(totals,.5),percentile(totals,.95),percentile(totals,.99),percentile(prepares,.5),percentile(layouts,.5),percentile(layout_self,.5),percentile(exports,.5));
        const auto u=[](uint64_t v){return static_cast<unsigned long long>(v);};
        std::printf("\"api_calls\":%llu,\"measure_calls\":%llu,\"callback_text_bytes\":%llu,\"callback_metadata_bytes\":%llu,\"shapes\":%llu,\"shaped_text_bytes\":%llu,\"shape_us\":%.6f,",u(api_calls),u(calls),u(callback_bytes),u(calls*(sizeof(LpMeasureRequest)+sizeof(LpMeasured))),u(shapes),u(shape_bytes),shape_us);
        std::printf("\"input_struct_bytes\":%llu,\"borrowed_input_text_bytes\":%llu,\"output_copy_bytes\":%llu,\"rebuilds\":%llu,\"layout_passes\":%llu,",u(input_nodes*sizeof(LpNode)),u(input_text_bytes),u(output_bytes),u(rebuilds),u(layout_calls));
        std::printf("\"process_current_rss\":%zu,\"process_lifetime_peak_rss\":%zu,\"process_current_commit\":%zu,\"process_lifetime_peak_commit\":%zu,\"checksum_including_warmup\":%.3f,",rss,peak_rss,commit,peak_commit,checksum);
        std::printf("\"sample_columns\":[\"total\",\"prepare_apply\",\"layout_inclusive\",\"callback\",\"export_consume\"],\"raw_samples_us\":[");
        for(size_t i=0;i<totals.size();++i)std::printf("%s[%.6f,%.6f,%.6f,%.6f,%.6f]",i?",":"",totals[i],prepares[i],layouts[i],callbacks[i],exports[i]);
        std::printf("]}\n");
    }
}
int main(int argc,char** argv) {
    try {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
        require(mi_version()==MI_MALLOC_VERSION,"mimalloc headers/DLL mismatch");require(mi_is_redirected(),"CRT redirection inactive");
        if(argc>1 && std::string_view(argv[1])=="--depth") {depth_path(argc>2?static_cast<uint32_t>(std::stoul(argv[2])):64);return 0;}
        if(argc>1 && std::string_view(argv[1])=="--diagnostics") {saturation_path();return 0;}
        if(argc>1 && std::string_view(argv[1])=="--bench") {const uint32_t n=argc>2?static_cast<uint32_t>(std::stoul(argv[2])):1000;const uint32_t k=argc>3?static_cast<uint32_t>(std::stoul(argv[3])):100;benchmark(n,k,argc>4 && std::string_view(argv[4])=="text");return 0;}
        uint32_t failed=0,passed=0;
        const auto run=[&](const char* name,const std::function<void()>& work){try{work();++passed;std::printf("{\"kind\":\"case\",\"candidate\":\"%s\",\"name\":\"%s\",\"status\":\"PASS\"}\n",lp_name(),name);}catch(const std::exception& e){++failed;std::printf("{\"kind\":\"case\",\"candidate\":\"%s\",\"name\":\"%s\",\"status\":\"FAIL\",\"error\":\"%s\"}\n",lp_name(),name,escaped(e.what()).c_str());}};
        run("P1/latin_shared_tracks",[]{case_inspector(false);});run("P1/cjk_shared_tracks",[]{case_inspector(true);});
        run("P2/hud_fractional_coordinates",case_hud);run("P3/wrap_grid",case_wrap);run("P4/variable_height_virtualization",case_virtual);
        run("P5/custom_size_dependencies",case_custom);run("P6/provider_failure_reentry_recovery",case_failure);run("P6/invalid_batch_transaction",case_invalid);
        run("P6/long_text_capacity",case_capacity);
        std::printf("{\"kind\":\"summary\",\"candidate\":\"%s\",\"passed\":%u,\"failed\":%u,\"node_abi_size\":%zu,\"box_abi_size\":%zu}\n",lp_name(),passed,failed,sizeof(LpNode),sizeof(LpBox));return failed?2:0;
    } catch(const std::exception& e){std::fprintf(stderr,"probe fatal: %s\n",e.what());return 3;}
}
