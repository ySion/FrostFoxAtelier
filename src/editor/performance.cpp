#include "editor/performance.hpp"
#include <algorithm>
#include <cmath>
#include <format>
#include <numeric>

namespace atelier::editor {
namespace {
using Pass=RenderPerformance::Pass;
// Indices 0..19 match the completed sample's mask; total is computed per sample.
constexpr std::array<Pass,RenderPerformance::pass_count> definitions{{
    {"gbuffer","GBuffer",-1,0,true},
    {"acceleration","AS / uploads",-1,0,true},
    {"direct","Direct lighting",-1,0,true},
    {"ddgi","DDGI",-1,0,true},
    {"ao","GTAO / Local GI",-1,0,true},
    {"hdr_reflections","HDR / SSSR",-1,0,true},
    {"hdr_resolve","HDR resolve",-1,0,true},
    {"display","Bloom / tone / AA / display",-1,0,true},
    {"ddgi_update","Update / transfer",3,1,false},
    {"ddgi_gather","Receiver gather",3,1,false},
    {"direct_initial","Initial / temporal reservoir",2,1,false},
    {"direct_spatial","Spatial reservoir",2,1,false},
    {"direct_visibility","Visibility / shading",2,1,false},
    {"direct_resolve","Radiance resolve / filter",2,1,false},
    {"ddgi_trace","Primary trace / relocation",8,2,false},
    {"ddgi_lighting","Hit lighting / convolution",8,2,false},
    {"direct_temporal_resolve","Temporal radiance resolve",13,2,false},
    {"direct_filter_1","Spatial filter step 1",13,2,false},
    {"direct_filter_2","Spatial filter step 2",13,2,false},
    {"direct_filter_4","Spatial filter step 4",13,2,false},
    {"total","Measured total",-1,0,false}
}};
constexpr std::array<std::size_t,RenderPerformance::pass_count> row_order{RenderPerformance::total_pass,0,1,2,10,11,12,13,16,17,18,19,3,8,14,15,9,4,5,6,7};
constexpr std::array<float,5> minima{120,60,60,60,55};
constexpr std::array<std::wstring_view,4> aa_names{L"Off",L"TAA",L"SMAA",L"SMAA T2x"};
std::wstring wide(std::string_view value) {return {value.begin(),value.end()};}
}

std::span<const RenderPerformance::Pass> RenderPerformance::passes() {return definitions;}
RenderPerformance::RenderPerformance() {reset();}
void RenderPerformance::reset() {
    domain_.reset();first_frame_=sequence_=frame_id_=0;next_=count_=0;resource_bytes_.reset();
    published_={};dirty_=true;publish(Clock::now());
}
void RenderPerformance::observe(const renderer::Frame& frame,const renderer::FrameResult& result,Clock::time_point now) {
    if(result.status!=renderer::FrameStatus::accepted) return;
    Domain current{frame.scene_identity,result.accepted_extent,frame.scene_viewport,frame.view,
        frame.view.shading==renderer::Shading::scene_lit && frame.view.wireframe!=renderer::Wireframe::only &&
        !frame.meshes.empty() && frame.scene_viewport.width>0 && frame.scene_viewport.height>0};
    bool force=!domain_ || *domain_!=current;
    if(force) {
        domain_=current;first_frame_=frame.ui_frame.id;sequence_=frame_id_=0;next_=count_=0;dirty_=true;
    }
    const auto& stats=result.stats;
    if(resource_bytes_!=stats.scene_lit_resource_bytes) {resource_bytes_=stats.scene_lit_resource_bytes;dirty_=true;}
    // A completed sample can lag a configuration change. The Application uses
    // monotonic UI identities; its first accepted frame fences the new domain.
    if(current.active && stats.scene_lit_gpu_timing_valid && stats.scene_lit_gpu_sample_frame_id>=first_frame_ &&
       stats.scene_lit_gpu_sample_frame_id<=frame.ui_frame.id &&
       (!count_ || stats.scene_lit_gpu_sample_sequence>sequence_)) {
        std::array<double,pass_count> sample{};
        std::copy(stats.scene_lit_gpu_ms.begin(),stats.scene_lit_gpu_ms.end(),sample.begin());
        std::copy(stats.scene_lit_ddgi_gpu_ms.begin(),stats.scene_lit_ddgi_gpu_ms.end(),sample.begin()+8);
        std::copy(stats.scene_lit_direct_gpu_ms.begin(),stats.scene_lit_direct_gpu_ms.end(),sample.begin()+10);
        std::copy(stats.scene_lit_ddgi_update_gpu_ms.begin(),stats.scene_lit_ddgi_update_gpu_ms.end(),sample.begin()+14);
        std::copy(stats.scene_lit_direct_reconstruction_gpu_ms.begin(),stats.scene_lit_direct_reconstruction_gpu_ms.end(),sample.begin()+16);
        sample[total_pass]=std::accumulate(sample.begin(),sample.begin()+8,0.);
        if(std::ranges::all_of(sample,[](double value){return std::isfinite(value) && value>=0;})) {
            force|=count_==0;
            samples_[next_]=sample;masks_[next_]=(stats.scene_lit_gpu_pass_mask&((1u<<total_pass)-1))|(1u<<total_pass);
            next_=(next_+1)%window_size;count_=std::min(count_+1,window_size);
            sequence_=stats.scene_lit_gpu_sample_sequence;frame_id_=stats.scene_lit_gpu_sample_frame_id;dirty_=true;
        }
    }
    if(dirty_ && (force || now>=next_publish_)) publish(now);
}
void RenderPerformance::publish(Clock::time_point now) {
    published_={};published_.resource_bytes=resource_bytes_;
    if(domain_) {
        published_.extent=domain_->extent;published_.viewport=domain_->viewport;
        published_.scene_identity=domain_->scene;published_.first_frame=first_frame_;
        published_.antialiasing=domain_->view.lit.antialiasing;
        published_.direct_candidates=domain_->view.lit.direct_candidates;
        published_.ddgi=domain_->view.lit.ddgi;published_.direct_reference=domain_->view.lit.direct_reference;
    }
    published_.state=domain_ && domain_->active?(count_?State::ready:State::waiting):State::unavailable;
    published_.samples=count_;published_.sequence=sequence_;published_.frame_id=frame_id_;
    if(count_) {
        const auto latest=(next_+window_size-1)%window_size;
        std::array<double,window_size> sorted;
        const auto percentile=(count_*95+99)/100-1;
        for(std::size_t pass=0;pass<pass_count;++pass) {
            Timing timing;timing.current=samples_[latest][pass];timing.current_active=(masks_[latest]&(1u<<pass))!=0;
            for(std::size_t i=0;i<count_;++i) {
                sorted[i]=samples_[i][pass];timing.mean+=sorted[i];
                timing.active_samples+=(masks_[i]&(1u<<pass))!=0;
            }
            timing.mean/=double(count_);std::sort(sorted.begin(),sorted.begin()+count_);timing.p95=sorted[percentile];
            published_.timings[pass]=timing;
        }
        const double total=published_.timings[total_pass]->mean;
        if(total>0) for(auto& timing:published_.timings) timing->share=timing->mean/total*100.;
    }
    if(published_.state==State::unavailable) status_=L"Unavailable · choose Scene Lit with visible geometry";
    else if(published_.state==State::waiting) status_=std::format(L"Waiting · Target {} × {} · View {:.0f} × {:.0f}",
        published_.extent.width,published_.extent.height,published_.viewport.width,published_.viewport.height);
    else status_=std::format(L"Target {} × {} · View {:.0f} × {:.0f} · {} / {} samples",published_.extent.width,published_.extent.height,
        published_.viewport.width,published_.viewport.height,count_,window_size);
    resources_=resource_bytes_?std::format(L"Logical resources: {:.2f} MiB · DI {}{} · {} · DDGI {} · 4 Hz",double(*resource_bytes_)/(1024.*1024.),
        published_.direct_candidates,published_.direct_reference?L" reference":L"",aa_names[static_cast<std::size_t>(published_.antialiasing)],published_.ddgi?L"on":L"off"):
        L"Scene Lit logical resources: unavailable";
    dirty_=false;next_publish_=now+std::chrono::milliseconds(250);rebuild_table();
}
void RenderPerformance::rebuild_table() {
    auto data=std::make_shared<ui::TableData>();
    constexpr std::array titles{L"Pass",L"Current ms",L"Mean ms",L"P95 ms",L"Share %"};
    for(std::size_t i=0;i<titles.size();++i)
        data->columns.push_back({i+1,titles[i],widths_[i],minima[i],i?ui::TextAlign::right:ui::TextAlign::left,false});
    data->rows.reserve(pass_count);
    for(auto index:row_order) {
        const auto& pass=definitions[index];const auto& timing=published_.timings[index];const ui::TableKey key=index+1;
        const auto title=std::wstring(pass.depth*3,L' ')+wide(pass.title);
        const bool ready=timing && timing->active_samples;
        const std::array<std::wstring,5> values{title,
            !timing?L"—":timing->current_active?std::format(L"{:.3f}",timing->current):L"Skipped",
            ready?std::format(L"{:.3f}",timing->mean):L"—",ready?std::format(L"{:.3f}",timing->p95):L"—",
            ready && timing->share?std::format(L"{:.1f}",*timing->share):L"—"};
        ui::TableRow row{key,{}};
        for(std::size_t column=0;column<values.size();++column)row.cells.push_back({key*8+column+1,values[column]});
        data->rows.push_back(std::move(row));
    }
    table_=std::move(data);
}
bool RenderPerformance::apply(const ui::Action& action) {
    if(action.kind==ui::ActionKind::select) {
        if(const auto* value=std::get_if<ui::TableSelection>(&action.value);value && value->row<=pass_count && value->column>=1 && value->column<=widths_.size())
            selection_=*value;
        return true;
    }
    if(action.kind==ui::ActionKind::table_column_resize) {
        if(const auto* value=std::get_if<ui::TableColumnResize>(&action.value);value && std::isfinite(value->width) && value->column>=1 && value->column<=widths_.size()) {
            const auto index=value->column-1;const auto width=std::clamp(value->width,minima[index],1000.f);
            if(widths_[index]!=width){widths_[index]=width;rebuild_table();}
        }
        return true;
    }
    return false;
}
}
