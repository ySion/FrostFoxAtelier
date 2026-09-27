#include "kernel.hpp"
#include <Windows.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <mimalloc.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace layout_step1;
using Microsoft::WRL::ComPtr;
static void require(bool ok,std::string_view text){if(!ok)throw std::runtime_error(std::string(text));}
static bool close(float a,float b){return std::abs(a-b)<=0.0002f+0.000002f*std::max(std::abs(a),std::abs(b));}
static void hr(HRESULT value){if(FAILED(value))throw std::runtime_error("DirectWrite HRESULT "+std::to_string(static_cast<unsigned long>(value)));}
struct TextProvider {
    struct Content {std::wstring text;float font_size{16};uint64_t revision{1};};
    struct Layout {ComPtr<IDWriteTextLayout> value;uint64_t content_revision{},provider_revision{};std::optional<float> last_width;bool configured{};};
    ComPtr<IDWriteFactory> factory;
    std::map<uint64_t,Content> content;
    std::map<uint64_t,Layout> layouts;
    uint64_t calls{},creates{},queries{},fail_at{},provider_revision{1};float scale{1};
    Kernel* active{};std::vector<Node> replacement;bool attempt_reentry{},edit_rejected{},compute_rejected{};
    bool invalid_result{},throw_result{};
    TextProvider(){
        hr(DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(factory.GetAddressOf())));
        ComPtr<IDWriteFontCollection> fonts;hr(factory->GetSystemFontCollection(fonts.GetAddressOf()));UINT32 index{};BOOL found{};hr(fonts->FindFamilyName(L"Segoe UI",&index,&found));require(found!=FALSE,"Segoe UI must exist");
    }
    ComPtr<IDWriteTextLayout> create(const Content& text){
        ComPtr<IDWriteTextFormat> format;hr(factory->CreateTextFormat(L"Segoe UI",nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,text.font_size*scale,L"en-US",format.GetAddressOf()));
        hr(format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP));
        ComPtr<IDWriteTextLayout> result;hr(factory->CreateTextLayout(text.text.data(),static_cast<UINT32>(text.text.size()),format.Get(),0,0,result.GetAddressOf()));++creates;return result;
    }
    Size metrics(IDWriteTextLayout* text,std::optional<float> width){
        hr(text->SetWordWrapping(width?DWRITE_WORD_WRAPPING_WRAP:DWRITE_WORD_WRAPPING_NO_WRAP));
        hr(text->SetMaxWidth(width.value_or(0)));
        DWRITE_TEXT_METRICS m{};hr(text->GetMetrics(&m));++queries;
        return {m.widthIncludingTrailingWhitespace,m.height};
    }
    Size direct(uint64_t key,std::optional<float> width){auto layout=create(content.at(key));return metrics(layout.Get(),width);}
    static Result<Size> measure(void* raw,const Query& query){
        auto& self=*static_cast<TextProvider*>(raw);++self.calls;
        if(self.attempt_reentry){self.attempt_reentry=false;auto edit=self.active->set_nodes(self.replacement);auto compute=self.active->compute({50,50},query.provider_revision);self.edit_rejected=!edit&&edit.error().code==Code::busy;self.compute_rejected=!compute&&compute.error().code==Code::busy;}
        if(self.throw_result)throw std::runtime_error("provider exception");
        if(self.invalid_result)return Size{0,std::numeric_limits<float>::quiet_NaN()};
        if(self.fail_at&&self.calls==self.fail_at)return std::unexpected(Failure{Code::provider});
        auto found=self.content.find(query.content_key);if(found==self.content.end()||found->second.revision!=query.content_revision||query.provider_revision!=self.provider_revision)return std::unexpected(Failure{Code::provider});
        auto& layout=self.layouts[query.content_key];
        if(!layout.value||layout.content_revision!=query.content_revision||layout.provider_revision!=query.provider_revision){layout.value=self.create(found->second);layout.content_revision=query.content_revision;layout.provider_revision=query.provider_revision;layout.configured=false;}
        const auto result=self.metrics(layout.value.Get(),query.width);layout.last_width=query.width;layout.configured=true;return result;
    }
};
static Node node(uint64_t id,uint32_t parent,Kind kind){Node n;n.id=id;n.parent=parent;n.kind=kind;return n;}
static Node text(uint64_t id,uint32_t parent,uint64_t key){auto n=node(id,parent,Kind::measured);n.content_key=key;n.content_revision=1;return n;}
static std::vector<Box> copy(View v){return {v.boxes.begin(),v.boxes.end()};}
static View run(Kernel& k,const std::vector<Node>& nodes,Size viewport,uint64_t provider=1){auto set=k.set_nodes(nodes);require(set.has_value(),"valid input rejected");auto result=k.compute(viewport,provider);require(result.has_value(),"layout failed");return *result;}
static void compare(View a,View b){require(a.boxes.size()==b.boxes.size(),"box count");for(size_t i=0;i<a.boxes.size();++i){const auto& x=a.boxes[i];const auto& y=b.boxes[i];require(x.id==y.id&&x.parent_id==y.parent_id&&close(x.x,y.x)&&close(x.y,y.y)&&close(x.width,y.width)&&close(x.height,y.height),"geometry mismatch");}}
static void fresh(Kernel& old,TextProvider& provider,const std::vector<Node>& nodes,Size viewport){Kernel current(TextProvider::measure,&provider,{20000,10000});compare(old.view(),run(current,nodes,viewport,provider.provider_revision));}

static void case_distribution(){
    const std::array<Share,3> base{{{0,1,120},{0,1,{}},{0,1,30}}};std::array<uint32_t,3> order{0,1,2};uint32_t permutations=0;
    do{std::array<Share,3> shuffled{base[order[0]],base[order[1]],base[order[2]]};auto result=distribute(shuffled,300);require(result.has_value(),"valid allocation failed");const std::array<double,3> expected{120,150,30};for(size_t i=0;i<3;++i)require(std::abs((*result)[i]-expected[order[i]])<1e-8,"saturation must be order independent");++permutations;}while(std::next_permutation(order.begin(),order.end()));
    const std::array<Share,2> floors{{{80,1,{}},{0,1,{}}}};auto floor_result=distribute(floors,100);require(floor_result&&close(static_cast<float>((*floor_result)[0]),80)&&close(static_cast<float>((*floor_result)[1]),20),"whole-pool 80/20, not minimum-plus-remainder 90/10");
    const std::array<Share,2> capped{{{0,1,30},{0,1,20}}};auto extra=distribute(capped,100);require(extra&&(*extra)[0]==30&&(*extra)[1]==20,"saturated surplus must remain free");
    auto deficient=distribute(floors,40);require(deficient&&(*deficient)[0]==80&&(*deficient)[1]==0,"hard minimum overflow policy");
    auto weighted=distribute(std::array<Share,2>{{{0,1,{}},{0,2,{}}}},90);require(weighted&&(*weighted)[0]==30&&(*weighted)[1]==60,"weighted pool");
    auto mixed=distribute(std::array<Share,2>{{{80,1,{}},{0,1,10}}},100);require(mixed&&(*mixed)[0]==90&&(*mixed)[1]==10,"a minimum is not a permanently frozen allocation");
    std::printf("{\"kind\":\"allocation\",\"permutations\":%u,\"saturated\":[120,150,30],\"minimum_case\":[80,20],\"overflow_case\":[80,0]}\n",permutations);
}
static void case_weight_range(){
    const std::array<Share,3> values{{{0,1e30,1},{0,1,1},{0,1e-30,{}}}};auto result=distribute(values,100);require(result&&std::abs((*result)[0]-1)<1e-8&&std::abs((*result)[1]-1)<1e-8&&std::abs((*result)[2]-98)<1e-6,"active slope loses small weights after large saturated weights leave");
    const std::array<float,3> weights{1e30f,1.f,1e-30f},expected{1,1,98};std::array<uint32_t,3> order{0,1,2};uint32_t permutations=0;
    do{Kernel k(nullptr,nullptr,{10,2});std::vector<Node> nodes{node(1,no_parent,Kind::row)};
        for(const auto index:order){auto item=node(index+2,0,Kind::box);item.width=Axis::expand(weights[index],0,index<2?std::optional<float>(1):std::nullopt);item.height=Axis::fixed(1);nodes.push_back(item);}
        const auto output=run(k,nodes,{100,10});for(size_t i=1;i<output.boxes.size();++i)require(close(output.boxes[i].width,expected[output.boxes[i].id-2]),"real row allocation loses small active weights");++permutations;
    }while(std::next_permutation(order.begin(),order.end()));
    std::printf("{\"kind\":\"weight_range\",\"row_permutations\":%u,\"expected_by_identity\":[1,1,98]}\n",permutations);
}
static void case_slots(){
    Kernel k(nullptr,nullptr,{20,10});std::vector<Node> nodes{node(1,no_parent,Kind::row),node(2,0,Kind::box),node(3,0,Kind::box)};
    nodes[1].width=Axis::expand(1,20);nodes[1].fill_width=false;nodes[1].align_x=.5f;nodes[1].height=Axis::fixed(10);nodes[2].height=Axis::fixed(10);
    auto r=run(k,nodes,{100,40});require(close(r.boxes[1].width,20)&&close(r.boxes[1].x,15)&&close(r.boxes[2].x,50)&&close(r.boxes[2].width,50),"slot and filled box are distinct");
    nodes[0].kind=Kind::column;nodes[1].width=Axis::fixed(10);nodes[1].height=Axis::expand(1,80);nodes[1].fill_height=true;nodes[2].height=Axis::expand();r=run(k,nodes,{100,100});require(close(r.boxes[1].height,80)&&close(r.boxes[2].height,20)&&close(r.boxes[2].y,80),"column shares use the same allocator");
}
static void case_text(){
    TextProvider p;p.content[1]={L"这是没有空格的中文内容，用于验证已知宽度下的自然高度，而不是视口高度。",20,1};
    Kernel k(TextProvider::measure,&p,{20,10});std::vector<Node> nodes{node(1,no_parent,Kind::column),text(2,0,1)};
    auto r=run(k,nodes,{100,40});const auto golden=p.direct(1,100);require(close(r.boxes[1].height,golden.height)&&r.boxes[1].height>r.boxes[0].height,"natural text height must overflow short viewport");
    const auto before=k.stats();const auto* data=r.boxes.data();const auto revision=r.publication;
    r=*k.compute({100,40},1);require(r.boxes.data()==data&&r.publication==revision&&k.stats().measure_calls==before.measure_calls&&k.stats().visited==before.visited,"clean compute must reuse publication without traversal");
    r=*k.compute({100,80},1);require(close(r.boxes[1].height,golden.height)&&k.stats().measure_calls==before.measure_calls,"viewport height must not invalidate H(W)");
    r=*k.compute({220,80},1);require(close(r.boxes[1].height,p.direct(1,220).height)&&r.boxes[1].height<golden.height,"known width changes natural height");fresh(k,p,nodes,{220,80});
    p.scale=1.25f;++p.provider_revision;r=*k.compute({220,80},p.provider_revision);require(close(r.boxes[1].height,p.direct(1,220).height),"font epoch invalidates measurements");fresh(k,p,nodes,{220,80});
    std::printf("{\"kind\":\"text\",\"width100_natural_height\":%.6f,\"viewport_height\":40,\"width220_scaled_height\":%.6f,\"core_width_queries\":%llu,\"core_intrinsic_queries\":%llu}\n",golden.height,r.boxes[1].height,static_cast<unsigned long long>(k.stats().width_queries),static_cast<unsigned long long>(k.stats().intrinsic_queries));
}
static void case_content_width(){
    TextProvider p;p.content[1]={L"Label with independent intrinsic width",16,1};Kernel k(TextProvider::measure,&p,{20,10});
    std::vector<Node> nodes{node(1,no_parent,Kind::column),node(2,0,Kind::row),text(3,1,1)};nodes[1].width=Axis::content(0,180);nodes[2].width=Axis::expand();
    auto r=run(k,nodes,{400,100});require(close(r.boxes[1].width,180)&&close(r.boxes[2].width,180)&&close(r.boxes[2].height,p.direct(1,180).height),"Content width uses independent intrinsic, then final H(W)");require(k.stats().intrinsic_queries==1&&k.stats().width_queries==1,"only required intrinsic and constrained queries");fresh(k,p,nodes,{400,100});
}
static void case_provider_shapes(){
    TextProvider p;std::array<std::wstring,5> values{L"A\nB",L"word   ",L"",L"averylongunbreakablewordwithoutspaces",L"自动换行的中文文字"};uint64_t key=0;
    for(const auto& value:values){++key;p.content[key]={value,16,1};Query q{key,1,1,{}};auto a=TextProvider::measure(&p,q);const auto d=p.direct(key,{});require(a&&close(a->width,d.width)&&close(a->height,d.height),"intrinsic NO_WRAP metrics");for(float width:{40.0f,220.0f,40.0f}){q.width=width;auto measured=TextProvider::measure(&p,q);const auto expected=p.direct(key,width);require(measured&&close(measured->height,expected.height)&&close(measured->width,expected.width),"reused layout must match fresh object");}}
    p.content[100]={L"Object reuse does not prove the absence of internal reshaping.",16,1};const auto creates=p.creates;
    for(float width:{80.0f,160.0f,80.0f})require(TextProvider::measure(&p,Query{100,1,1,width}).has_value(),"object reuse query");require(p.creates-creates==1,"three widths use one mutable metrics-only object");
    std::printf("{\"kind\":\"provider\",\"reuse_queries\":3,\"layout_objects_created\":1,\"internal_shaping_count\":null,\"scope\":\"metrics-only mutable scratch; not a retained draw lease\"}\n");
}
static void case_failure(){
    TextProvider p;for(uint64_t key=1;key<=3;++key)p.content[key]={L"A real measured paragraph that depends on its allocated width.",16,1};Kernel k(TextProvider::measure,&p,{20,10});p.active=&k;
    std::vector<Node> nodes{node(1,no_parent,Kind::column),text(2,0,1),text(3,0,2),text(4,0,3)};auto old=run(k,nodes,{180,300});const auto owned=copy(old);const auto* old_data=old.boxes.data();
    for(size_t i=1;i<nodes.size();++i){++nodes[i].content_revision;++p.content[nodes[i].content_key].revision;p.content[nodes[i].content_key].text+=L" Changed.";}
    require(k.set_nodes(nodes).has_value(),"edit before failed layout");p.replacement={nodes.front()};p.attempt_reentry=true;p.fail_at=p.calls+3;const auto callbacks=k.stats().measure_calls;
    auto failed=k.compute({180,300},1);require(!failed&&failed.error().code==Code::provider&&k.view().publication==old.publication&&k.view().boxes.data()==old_data,"failed layout cannot replace old view");require(std::equal(owned.begin(),owned.end(),k.view().boxes.begin()),"failed layout changed old bytes");require(p.edit_rejected&&p.compute_rejected,"callback mutation and nested compute must be rejected");
    p.fail_at=0;auto retry=k.compute({180,300},1);require(retry.has_value()&&k.stats().measure_calls-callbacks==6,"failed attempt cache writes must not leak into retry");fresh(k,p,nodes,{180,300});
    const auto good=copy(k.view());const auto good_revision=k.view().publication;
    ++p.provider_revision;p.invalid_result=true;require(!k.compute({180,300},p.provider_revision)&&k.view().publication==good_revision,"nonfinite callback result rejected");p.invalid_result=false;p.throw_result=true;require(!k.compute({180,300},p.provider_revision)&&k.view().publication==good_revision,"provider exception rejected");p.throw_result=false;require(k.compute({180,300},p.provider_revision).has_value(),"recovery after callback exception");require(!good.empty()&&!owned.empty(),"owned old results survive publication");
    std::printf("{\"kind\":\"transaction\",\"failed_attempt_queries\":3,\"retry_queries\":3,\"old_view_preserved\":true,\"callback_edit_rejected\":true,\"nested_compute_rejected\":true}\n");
}
static void case_owned_geometry_lifetime(){
    std::vector<Box> owned;
    {Kernel k(nullptr,nullptr,{10,2});std::vector<Node> nodes{node(1,no_parent,Kind::overlay),node(2,0,Kind::box)};
        nodes[1].width=Axis::fixed(10.25f);nodes[1].height=Axis::fixed(5.5f);nodes[1].offset_x=2.25f;
        const auto first=run(k,nodes,{100,100});owned=copy(first);const auto* first_data=first.boxes.data();
        nodes[1].offset_x=9.5f;require(k.set_nodes(nodes).has_value(),"edit before next publication");
        require(k.view().boxes.data()==first_data&&k.view().publication==first.publication&&k.view().boxes[1].x==2.25f,"editing must not replace published view");
        const auto second=k.compute({100,100},1);require(second&&second->publication==first.publication+1&&second->boxes[1].x==9.5f&&owned[1].x==2.25f,"owned geometry must survive next successful publication");
    }
    require(owned.size()==2&&owned[1].id==2&&owned[1].x==2.25f&&owned[1].width==10.25f&&owned[1].height==5.5f,"owned geometry must survive document destruction");
    std::printf("{\"kind\":\"lifetime\",\"edit_preserves_view\":true,\"copy_survives_publication\":true,\"copy_survives_destruction\":true,\"old_text_lease_proven\":false}\n");
}
static void case_locality(){
    TextProvider p;p.content[1]={L"The first pane changes.",16,1};p.content[2]={L"The second pane stays unchanged.",16,1};Kernel k(TextProvider::measure,&p,{20,10});std::vector<Node> nodes{node(1,no_parent,Kind::row),text(2,0,1),text(3,0,2)};
    run(k,nodes,{400,200});const auto before=k.stats();++nodes[1].content_revision;++p.content[1].revision;p.content[1].text+=L" It now contains more words.";run(k,nodes,{400,200});require(k.stats().measure_calls-before.measure_calls==1,"unchanged sibling measurement must remain cached");fresh(k,p,nodes,{400,200});
    std::printf("{\"kind\":\"locality\",\"changed_leaves\":1,\"provider_queries\":1,\"core_visits\":%llu,\"full_incremental_scheduler_proven\":false}\n",static_cast<unsigned long long>(k.stats().visited-before.visited));
}
static void case_anchors(){
    Kernel k(nullptr,nullptr,{20,10});std::vector<Node> nodes{node(1,no_parent,Kind::overlay),node(2,0,Kind::box),node(3,0,Kind::overlay),node(4,2,Kind::box)};
    nodes[1].width=Axis::fixed(80);nodes[1].height=Axis::fixed(40);nodes[1].anchor_x=1;nodes[1].anchor_y=1;nodes[1].pivot_x=1;nodes[1].pivot_y=1;nodes[1].offset_x=-10;nodes[1].offset_y=-10;
    nodes[2].width=Axis::fixed(100);nodes[2].height=Axis::fixed(100);nodes[2].offset_x=.4f;nodes[2].offset_y=.4f;nodes[3].width=Axis::fixed(10);nodes[3].height=Axis::fixed(10);nodes[3].offset_x=.4f;nodes[3].offset_y=.4f;
    auto r=run(k,nodes,{800,450});require(close(r.boxes[1].x,710)&&close(r.boxes[1].y,400)&&close(r.boxes[2].x+r.boxes[3].x,.8f),"parent anchors, pivot and unrounded local coordinates");
    const auto snapshot=copy(r);for(float dpi:{1.f,1.5f,2.f})require(std::isfinite(std::round((r.boxes[2].x+r.boxes[3].x)*dpi)),"pure pixel consumption");require(std::equal(snapshot.begin(),snapshot.end(),k.view().boxes.begin()),"pixel conversion must not write back");
}
static void case_rejection(){
    Kernel k(nullptr,nullptr,{10,3});std::vector<Node> nodes{node(1,no_parent,Kind::column),node(2,0,Kind::box)};nodes[1].width=Axis::fixed(0);nodes[1].height=Axis::fixed(0);auto old=run(k,nodes,{0,0});const auto version=old.publication;
    auto bad=nodes;bad[1].parent=1;require(!k.set_nodes(bad),"cycle rejection");bad=nodes;bad[1].id=1;require(!k.set_nodes(bad),"duplicate ID rejection");bad=nodes;bad[1].width=Axis::content(30,20);require(!k.set_nodes(bad),"invalid min/max rejection");bad=nodes;bad[1].width.value=std::numeric_limits<float>::quiet_NaN();require(!k.set_nodes(bad),"nonfinite input rejection");bad=nodes;bad[1].kind=Kind::overlay;bad[1].width=Axis::content();bad[1].height=Axis::content();auto feedback=k.set_nodes(bad);require(!feedback&&feedback.error().code==Code::unsupported_dependency,"Content overlay dependency must not be guessed");
    require(k.compute({0,0},1)->publication==version,"invalid batch changed active state");
    std::vector<Node> deep{node(1,no_parent,Kind::column)};for(uint32_t i=1;i<=4;++i)deep.push_back(node(i+1,i-1,Kind::column));auto limit=k.set_nodes(deep);require(!limit&&limit.error().code==Code::limit&&k.view().publication==version,"depth limit before publication");
    std::vector<Node> wide{node(1,no_parent,Kind::column)};for(uint32_t i=1;i<=10;++i)wide.push_back(node(i+1,0,Kind::box));limit=k.set_nodes(wide);require(!limit&&limit.error().code==Code::limit&&k.view().publication==version,"node limit before publication");
    std::vector<Node> huge{node(1,no_parent,Kind::column)};for(uint32_t i=1;i<=3;++i){auto item=node(i+1,0,Kind::box);item.height=Axis::fixed(std::numeric_limits<float>::max());huge.push_back(item);}require(k.set_nodes(huge).has_value(),"finite extreme sizes accepted for checked computation");const auto overflow=k.compute({100,100},1);require(!overflow&&overflow.error().code==Code::numeric_range&&k.view().publication==version,"computed numeric overflow must preserve old publication");
    auto missing=nodes;missing[1]=text(2,0,999);TextProvider p;Kernel measured(TextProvider::measure,&p,{10,3});require(measured.set_nodes(missing).has_value()&&!measured.compute({100,100},1)&&measured.view().publication==0,"missing real content rejected");
}
static void case_deep(){
    constexpr uint32_t depth=4096;TextProvider p;p.content[1]={L"A real leaf in a deep but explicitly scheduled tree.",16,1};Kernel k(TextProvider::measure,&p,{5000,4096});std::vector<Node> nodes;nodes.reserve(depth+1);nodes.push_back(node(1,no_parent,Kind::column));for(uint32_t i=1;i<depth;++i)nodes.push_back(node(i+1,i-1,Kind::column));nodes.push_back(text(depth+1,depth-1,1));auto r=run(k,nodes,{180,40});require(r.boxes.size()==depth+1&&close(r.boxes.back().height,p.direct(1,180).height)&&k.stats().measure_calls==1,"explicit traversal of deep tree");
    const auto stats=k.stats();std::printf("{\"kind\":\"deep\",\"depth\":%u,\"nodes\":%zu,\"measure_calls\":%llu,\"visits\":%llu,\"product_limit_claimed\":false}\n",depth,r.boxes.size(),static_cast<unsigned long long>(stats.measure_calls),static_cast<unsigned long long>(stats.visited));
}
int main(){
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    try{require(mi_version()==MI_MALLOC_VERSION&&mi_is_redirected(),"shared mimalloc/version/redirection");uint32_t passed=0,failed=0;
        const auto run_case=[&](const char* name,const std::function<void()>& work){try{work();++passed;std::printf("{\"kind\":\"case\",\"name\":\"%s\",\"status\":\"PASS\"}\n",name);}catch(const std::exception& e){++failed;std::printf("{\"kind\":\"case\",\"name\":\"%s\",\"status\":\"FAIL\",\"error\":\"%s\"}\n",name,e.what());}};
        run_case("bounded_pool",case_distribution);run_case("weight_dynamic_range",case_weight_range);run_case("slots_and_fill",case_slots);run_case("known_width_natural_height",case_text);run_case("independent_content_width",case_content_width);run_case("provider_metrics_reuse",case_provider_shapes);run_case("failed_publication_and_retry",case_failure);run_case("leaf_measurement_locality",case_locality);run_case("parent_anchors",case_anchors);run_case("invalid_inputs_and_limits",case_rejection);run_case("explicit_deep_schedule",case_deep);
        run_case("owned_geometry_lifetime",case_owned_geometry_lifetime);
        std::printf("{\"kind\":\"summary\",\"passed\":%u,\"failed\":%u,\"production_interface\":false}\n",passed,failed);return failed?2:0;
    }catch(const std::exception& e){std::fprintf(stderr,"probe fatal: %s\n",e.what());return 3;}
}
