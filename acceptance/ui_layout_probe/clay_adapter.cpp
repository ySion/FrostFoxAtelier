// Research adapter for the fixed Clay M snapshot. Not a production Interface.
#include "probe_api.h"
#define CLAY_IMPLEMENTATION
#include <clay.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace {
std::recursive_mutex clay_mutex;
thread_local bool in_measure=false;
struct Item { LpNode node{}; std::string text; std::array<char,32> name{}; Clay_ElementId id{}; };
struct Engine {
    LpMeasureFn measure{}; void* user{};
    std::vector<Item> items;
    std::vector<uint32_t> offsets,children;
    std::vector<float> widths;
    std::vector<Clay_BoundingBox> absolute;
    std::vector<LpBox> pending,published;
    std::unique_ptr<std::byte[]> arena;
    Clay_Context* context{};
    uint32_t element_capacity{},word_capacity{};
    uint64_t revision{},published_revision{};
    float last_width{},last_height{};
    bool dirty{true},poisoned{},failed{},adaptive{},has_publication{},measure_dirty{};
    std::array<char,512> error{};
    LpStats stats{};
};
bool finite_nonnegative(float x){return std::isfinite(x)&&x>=0;}
bool native_integer(float x){return finite_nonnegative(x)&&x<=65535&&std::floor(x)==x;}
void error(Engine& e,const char* text){std::snprintf(e.error.data(),e.error.size(),"%s",text);}
void fail(Engine& e,const char* text){if(!e.failed)error(e,text);e.failed=true;e.poisoned=true;}
void clay_error(Clay_ErrorData d){auto& e=*static_cast<Engine*>(d.userData);if(!e.failed)std::snprintf(e.error.data(),e.error.size(),"Clay error %d: %.*s",static_cast<int>(d.errorType),std::clamp(d.errorText.length,0,400),d.errorText.chars?d.errorText.chars:"");e.failed=true;e.poisoned=true;}
Clay_Dimensions measure_text(Clay_StringSlice text,Clay_TextElementConfig* config,void* raw) noexcept {
    auto& e=*static_cast<Engine*>(raw);if(e.failed)return {};
    if(!config||text.length<0){fail(e,"invalid native text request");return {};}
    LpMeasureRequest q{text.chars,static_cast<uint32_t>(text.length),static_cast<float>(config->fontSize),0,0,LP_UNBOUNDED,LP_UNBOUNDED};
    LpMeasured result{};++e.stats.measure_calls;in_measure=true;int32_t status=-1;
    try {status=e.measure(e.user,&q,&result);}catch(...){status=-1;}
    in_measure=false;
    if(status || !finite_nonnegative(result.width)||!finite_nonnegative(result.height)||!finite_nonnegative(result.baseline)){fail(e,"real text provider failed or returned invalid geometry");return {};}
    return {result.width,result.height};
}
class ContextScope {
    Clay_Context* previous_=Clay_GetCurrentContext();
    decltype(Clay__MeasureText) measure_=Clay__MeasureText;
    void* user_=previous_?previous_->measureTextUserData:nullptr;
    int32_t elements_=Clay__defaultMaxElementCount;
    int32_t words_=Clay__defaultMaxMeasureTextWordCacheCount;
public:
    ~ContextScope(){Clay_SetCurrentContext(nullptr);Clay_SetMaxElementCount(elements_);Clay_SetMaxMeasureTextCacheWordCount(words_);Clay_SetCurrentContext(previous_);if(previous_)Clay_SetMeasureTextFunction(measure_,user_);else Clay__MeasureText=measure_;}
    void forget(Clay_Context* p){if(previous_==p)previous_=nullptr;}
};
uint32_t capacity_for(uint64_t requested){if(requested>1048576)throw std::runtime_error("experimental Clay capacity exceeded");uint32_t n=256;while(n<requested)n*=2;return n;}
void initialize(Engine& e,float w,float h,ContextScope& context_scope){
    uint64_t text_bytes=0;for(const auto& item:e.items)text_bytes+=item.text.size();
    const auto words=capacity_for(std::max<uint64_t>(4096,text_bytes*2+1024));
    // This snapshot indexes wordCapacity/32 buckets in an elementCapacity array.
    const auto elements=capacity_for(std::max<uint64_t>(e.items.size()*4+64,words/32));
    if(e.context&&!e.poisoned&&e.element_capacity>=elements&&e.word_capacity>=words){Clay_SetCurrentContext(e.context);Clay_SetMeasureTextFunction(measure_text,&e);return;}
    context_scope.forget(e.context);Clay_SetCurrentContext(nullptr);e.context=nullptr;e.arena.reset();
    Clay_SetMaxElementCount(static_cast<int32_t>(elements));Clay_SetMaxMeasureTextCacheWordCount(static_cast<int32_t>(words));
    const auto bytes=Clay_MinMemorySize();auto storage=std::make_unique<std::byte[]>(bytes);
    const auto arena=Clay_CreateArenaWithCapacityAndMemory(bytes,storage.get());
    const auto c=Clay_Initialize(arena,{w,h},{clay_error,&e});
    if(!c||e.failed)throw std::runtime_error("Clay arena initialization failed");
    e.context=c;e.arena=std::move(storage);e.element_capacity=elements;e.word_capacity=words;e.poisoned=false;++e.stats.rebuilds;
    Clay_SetCurrentContext(e.context);Clay_SetMeasureTextFunction(measure_text,&e);Clay_SetCullingEnabled(false);
}
bool validate(Engine& e,const LpNode* nodes,uint32_t count){
    if(!nodes||count==0||count>65500){error(e,"invalid node batch or experimental capacity");return false;}
    std::unordered_set<uint64_t> ids;ids.reserve(count);std::vector<uint32_t> child_count(count);
    for(uint32_t i=0;i<count;++i){const auto& n=nodes[i];
        if(!n.id||!ids.insert(n.id).second||(i==0?n.parent!=UINT32_MAX:n.parent>=i)||n.kind>LP_BOX||n.flow>LP_GRID_TWO||n.width.mode>LP_GROW||n.height.mode>LP_GROW){error(e,"invalid identity, ordered tree or enum");return false;}
        if(i)++child_count[n.parent];
        if((!n.text&&n.text_length)||n.text_length>static_cast<uint32_t>(INT32_MAX)){error(e,"invalid text span");return false;}
        if(!finite_nonnegative(n.width.value)||!finite_nonnegative(n.height.value)||!native_integer(n.padding)||!native_integer(n.gap)||!std::isfinite(n.left)||!std::isfinite(n.top)||!finite_nonnegative(n.first_column)) {error(e,"invalid geometry; native Clay padding/gap require uint16 values");return false;}
        if((n.width.mode==LP_FIXED&&n.width.value==0)||(n.height.mode==LP_FIXED&&n.height.value==0)){error(e,"native Clay zero FIXED bound is not supported by this adapter");return false;}
        if((n.has_min_width&&!finite_nonnegative(n.min_width))||(n.has_max_width&&(!finite_nonnegative(n.max_width)||n.max_width==0))||(n.has_min_width&&n.has_max_width&&n.min_width>n.max_width)){error(e,"invalid min/max width");return false;}
        if(n.width.mode==LP_FIXED&&((n.has_min_width&&n.width.value<n.min_width)||(n.has_max_width&&n.width.value>n.max_width)||n.width.value<2*n.padding)){error(e,"fixed width conflicts with bounds/padding");return false;}
        if(n.kind==LP_TEXT&&(!native_integer(n.font_size)||n.font_size==0||(!n.text&&n.text_length)||n.text_length>static_cast<uint32_t>(INT32_MAX))){error(e,"invalid native text configuration");return false;}
    }
    for(uint32_t i=0;i<count;++i){if(nodes[i].kind!=LP_CONTAINER&&child_count[i]){error(e,"non-container has children");return false;}if(nodes[i].flow==LP_GRID_TWO&&(child_count[i]%2||nodes[i].first_column<=0)){error(e,"two-column grid requires paired cells and positive first track");return false;}if(i&&nodes[nodes[i].parent].flow==LP_WRAP&&!nodes[i].absolute&&(nodes[i].width.mode!=LP_FIXED||nodes[i].height.mode!=LP_FIXED)){error(e,"supplemental wrap supports fixed box children only");return false;}}
    return true;
}
Clay_SizingAxis sizing(LpAxis a,bool stretch=false){Clay_SizingAxis result{};result.type=a.mode==LP_FIXED?CLAY__SIZING_TYPE_FIXED:((a.mode==LP_GROW||stretch)?CLAY__SIZING_TYPE_GROW:CLAY__SIZING_TYPE_FIT);result.size.minMax={a.mode==LP_FIXED?a.value:0,a.mode==LP_FIXED?a.value:std::numeric_limits<float>::max()};return result;}
Clay_ElementDeclaration declaration(Engine& e,uint32_t index,float forced_width=-1){
    const auto& n=e.items[index].node;Clay_ElementDeclaration d{};
    const bool parent_column=index&&e.items[n.parent].node.flow==LP_COLUMN;
    const bool parent_row=index&&e.items[n.parent].node.flow==LP_ROW;
    d.layout.sizing.width=sizing(n.width,parent_column&&n.width.mode!=LP_FIXED);
    d.layout.sizing.height=sizing(n.height,parent_row&&n.height.mode!=LP_FIXED);
    if(n.has_min_width)d.layout.sizing.width.size.minMax.min=n.min_width;
    if(n.has_max_width)d.layout.sizing.width.size.minMax.max=n.max_width;
    if(forced_width>=0)d.layout.sizing.width=sizing({LP_FIXED,forced_width});
    const auto p=static_cast<uint16_t>(n.padding);d.layout.padding={p,p,p,p};d.layout.childGap=static_cast<uint16_t>(n.gap);
    d.layout.layoutDirection=n.flow==LP_ROW?CLAY_LEFT_TO_RIGHT:CLAY_TOP_TO_BOTTOM;
    if(n.absolute){d.floating.attachTo=CLAY_ATTACH_TO_ELEMENT_WITH_ID;d.floating.parentId=e.items[n.parent].id.id;const float padding=e.items[n.parent].node.padding;d.floating.offset={n.left+padding,n.top+padding};d.floating.pointerCaptureMode=CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH;}
    if(index==0){d.layout.sizing.width=sizing({LP_FIXED,e.widths[0]});d.layout.sizing.height=sizing({LP_FIXED,e.last_height});}
    return d;
}
Clay_ElementId row_id(const Item& parent,uint32_t first){return Clay_GetElementIdWithIndex({false,static_cast<int32_t>(std::strlen(parent.name.data())),parent.name.data()},0x80000000u+first);}
void emit(Engine& e,uint32_t index,float forced_width=-1){
    if(e.failed)return;
    auto& item=e.items[index];const auto& n=item.node;
    Clay__OpenElementWithId(item.id);Clay__ConfigureOpenElement(declaration(e,index,forced_width));
    if(n.kind==LP_TEXT){Clay_TextElementConfig config{};config.fontSize=static_cast<uint16_t>(n.font_size);config.wrapMode=CLAY_TEXT_WRAP_WORDS;Clay__OpenTextElement({false,static_cast<int32_t>(item.text.size()),item.text.data()},config);}
    else if(n.flow==LP_GRID_TWO||n.flow==LP_WRAP){
        const auto first=e.offsets[index],end=e.offsets[index+1];
        const float available=std::max(0.0f,e.widths[index]-2*n.padding);
        bool row_open=false;float occupied=0;uint32_t ordinal=0;
        for(uint32_t k=first;k<end;++k){const auto child=e.children[k];const auto& cn=e.items[child].node;
            if(cn.absolute){emit(e,child);continue;}
            const float width=n.flow==LP_GRID_TWO?(ordinal%2==0?n.first_column:available-n.first_column-n.gap):cn.width.value;
            if(width<=0){fail(e,"shared track has no positive remaining width");break;}
            const bool new_row=!row_open||(n.flow==LP_GRID_TWO?ordinal%2==0:occupied+n.gap+width>available+0.001f);
            if(new_row){if(row_open)Clay__CloseElement();Clay_ElementDeclaration row{};row.layout.sizing.width=sizing({LP_GROW,0});row.layout.sizing.height=sizing({LP_AUTO,0});row.layout.layoutDirection=CLAY_LEFT_TO_RIGHT;row.layout.childGap=static_cast<uint16_t>(n.gap);Clay__OpenElementWithId(row_id(item,k));Clay__ConfigureOpenElement(row);row_open=true;occupied=0;}
            emit(e,child,width);occupied+=(occupied>0?n.gap:0)+width;++ordinal;
        }
        if(row_open)Clay__CloseElement();
    }else for(uint32_t k=e.offsets[index];k<e.offsets[index+1];++k)emit(e,e.children[k]);
    Clay__CloseElement();
}
bool frame(Engine& e,float w,float h){
    Clay_SetLayoutDimensions({w,h});Clay_BeginLayout();emit(e,0);(void)Clay_EndLayout(0);++e.stats.layouts;
    if(e.failed)return false;
    for(size_t i=0;i<e.items.size();++i){const auto data=Clay_GetElementData(e.items[i].id);if(!data.found){fail(e,"native Clay lost an input element");return false;}e.absolute[i]=data.boundingBox;}
    return true;
}
}
extern "C" {
const char* lp_name(void){return "clay-e6cc369-native-text+shared-tracks";}
void* lp_create(LpMeasureFn measure,void* user){if(!measure||in_measure)return nullptr;try{auto e=std::make_unique<Engine>();e->measure=measure;e->user=user;return e.release();}catch(...){return nullptr;}}
void lp_destroy(void* handle){if(!handle||in_measure)return;std::lock_guard lock(clay_mutex);auto* e=static_cast<Engine*>(handle);if(Clay_GetCurrentContext()==e->context)Clay_SetCurrentContext(nullptr);delete e;}
int32_t lp_apply(void* handle,const LpNode* nodes,uint32_t count,uint64_t revision){
    if(!handle||in_measure)return -2;std::lock_guard lock(clay_mutex);auto& e=*static_cast<Engine*>(handle);++e.stats.apply_calls;e.error[0]='\0';
    try{
        if(!validate(e,nodes,count))return -1;
        std::vector<Item> next(count);bool same_topology=e.items.size()==count;bool same=true;bool measure_changed=false;
        for(uint32_t i=0;i<count;++i){next[i].node=nodes[i];next[i].text.assign(nodes[i].text?nodes[i].text:"",nodes[i].text_length);next[i].node.text=nullptr;std::snprintf(next[i].name.data(),next[i].name.size(),"probe-%llu",static_cast<unsigned long long>(nodes[i].id));
            if(i>=e.items.size()){same=false;same_topology=false;continue;}const auto& old=e.items[i];same_topology=same_topology&&old.node.id==nodes[i].id&&old.node.parent==nodes[i].parent&&old.node.kind==nodes[i].kind&&old.node.flow==nodes[i].flow;auto old_node=old.node;old_node.text=nullptr;same=same&&std::memcmp(&old_node,&next[i].node,sizeof(LpNode))==0&&old.text==next[i].text;
            measure_changed=measure_changed||old.node.measure_revision!=nodes[i].measure_revision||(old.node.content_revision!=nodes[i].content_revision&&old.text==next[i].text);}
        if(!same_topology){std::vector<uint32_t> offsets(count+1);for(uint32_t i=1;i<count;++i)++offsets[nodes[i].parent+1];for(uint32_t i=1;i<=count;++i)offsets[i]+=offsets[i-1];auto cursor=offsets;std::vector<uint32_t> children(count-1);for(uint32_t i=1;i<count;++i)children[cursor[nodes[i].parent]++]=i;
            std::vector<float> widths(count);std::vector<Clay_BoundingBox> absolute(count);e.pending.reserve(count);
            e.items.swap(next);e.offsets.swap(offsets);e.children.swap(children);e.widths.swap(widths);e.absolute.swap(absolute);e.poisoned=true;same=false;}
        else if(!same){for(uint32_t i=0;i<count;++i){e.items[i].node=next[i].node;e.items[i].text.swap(next[i].text);}}
        e.adaptive=false;for(auto& item:e.items){item.node.text=item.text.data();item.id=Clay_GetElementId({false,static_cast<int32_t>(std::strlen(item.name.data())),item.name.data()});e.adaptive=e.adaptive||item.node.flow==LP_GRID_TWO||item.node.flow==LP_WRAP;}
        e.dirty=e.dirty||!same;e.measure_dirty=e.measure_dirty||measure_changed;e.revision=revision;e.stats.input_nodes+=count;return 0;
    }catch(const std::exception& ex){error(e,ex.what());e.poisoned=true;return -1;}catch(...){error(e,"native adapter allocation error");e.poisoned=true;return -1;}
}
int32_t lp_layout(void* handle,float w,float h){
    if(!handle||in_measure)return -2;std::lock_guard lock(clay_mutex);auto& e=*static_cast<Engine*>(handle);e.error[0]='\0';
    if(!finite_nonnegative(w)||!finite_nonnegative(h)||w==0||h==0||e.items.empty()){error(e,"invalid/zero root constraints or absent input");return -1;}
    if(!e.dirty&&!e.poisoned&&e.has_publication&&e.last_width==w&&e.last_height==h){e.published_revision=e.revision;return 0;}
    ContextScope scope;e.failed=false;
    try{
        initialize(e,w,h,scope);if(e.measure_dirty)Clay_ResetMeasureTextCache();e.last_height=h;
        e.widths[0]=w;for(uint32_t i=1;i<e.items.size();++i){const auto& n=e.items[i].node;e.widths[i]=n.width.mode==LP_FIXED?n.width.value:std::max(0.0f,e.widths[n.parent]-2*e.items[n.parent].node.padding);}
        if(!frame(e,w,h))return -1;
        if(e.adaptive){for(size_t i=0;i<e.items.size();++i)e.widths[i]=e.absolute[i].width;if(!frame(e,w,h))return -1;}
        e.pending.clear();
        for(size_t i=0;i<e.items.size();++i){const auto& n=e.items[i].node;const auto& b=e.absolute[i];if(!finite_nonnegative(b.width)||!finite_nonnegative(b.height)||!std::isfinite(b.x)||!std::isfinite(b.y)){fail(e,"nonfinite native box");return -1;}const auto parent=i?e.absolute[n.parent]:Clay_BoundingBox{};e.pending.push_back({n.id,i?e.items[n.parent].node.id:0,b.x-parent.x,b.y-parent.y,b.width,b.height});}
        e.published.swap(e.pending);e.published_revision=e.revision;e.stats.output_boxes+=e.published.size();e.dirty=false;e.measure_dirty=false;e.has_publication=true;e.last_width=w;e.error[0]='\0';return 0;
    }catch(const std::exception& ex){fail(e,ex.what());return -1;}catch(...){fail(e,"native Clay adapter exception");return -1;}
}
uint32_t lp_copy_boxes(void* handle,LpBox* out,uint32_t capacity){if(!handle)return 0;std::lock_guard lock(clay_mutex);const auto& e=*static_cast<Engine*>(handle);if(capacity<e.published.size()||(!out&&!e.published.empty()))return 0;if(!e.published.empty())std::memcpy(out,e.published.data(),e.published.size()*sizeof(LpBox));return static_cast<uint32_t>(e.published.size());}
uint64_t lp_output_revision(void* handle){return handle?static_cast<Engine*>(handle)->published_revision:0;}
const char* lp_last_error(void* handle){if(in_measure)return "Clay adapter rejects callback reentry";return handle?static_cast<Engine*>(handle)->error.data():"null Clay engine";}
LpStats lp_stats(void* handle){return handle?static_cast<Engine*>(handle)->stats:LpStats{};}
size_t lp_sizeof_node(void){return sizeof(LpNode);}
size_t lp_sizeof_box(void){return sizeof(LpBox);}
}
