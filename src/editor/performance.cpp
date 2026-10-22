#include "editor/performance.hpp"
#include <algorithm>
#include <cmath>
#include <format>
#include <numeric>

namespace atelier::editor {
namespace {
using Pass=FramePerformance::Pass;
using Timing=FramePerformance::Timing;
// Indices 0..21 match the completed sample's mask; total is computed per sample.
constexpr std::array<Pass,FramePerformance::pass_count> definitions{{
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
    {"direct_cluster","Light list construction",2,1,false},
    {"shadow_cache","Shadow atlas updates",2,1,false},
    {"total","GPU measured total",-1,0,false}
}};
constexpr std::array<std::size_t,FramePerformance::pass_count> row_order{FramePerformance::total_pass,0,1,2,20,21,10,11,12,13,16,17,18,19,3,8,14,15,9,4,5,6,7};
constexpr std::array<float,6> minima{120,60,60,60,60,55};
constexpr std::array<std::wstring_view,4> aa_names{L"Off",L"TAA",L"SMAA",L"SMAA T2x"};
std::wstring wide(std::string_view value) {return {value.begin(),value.end()};}
double milliseconds(std::chrono::nanoseconds value) {return std::chrono::duration<double,std::milli>(value).count();}
// Mean, nearest-rank p95 and max; reorders `values`.
void distribution(std::span<double> values,Timing& timing) {
    timing.mean=std::accumulate(values.begin(),values.end(),0.)/double(values.size());
    const auto rank=values.begin()+static_cast<std::ptrdiff_t>((values.size()*95+99)/100-1);
    std::nth_element(values.begin(),rank,values.end());
    timing.p95=*rank;timing.max=*std::max_element(rank,values.end());
}
}

std::span<const FramePerformance::Pass> FramePerformance::passes() {return definitions;}
FramePerformance::FramePerformance():cpu_(window_size) {reset();}
void FramePerformance::reset() {
    domain_.reset();first_frame_=sequence_=frame_id_=0;next_=count_=0;resource_bytes_.reset();clear_cpu();
    published_={};dirty_=true;publish(Clock::now());
}
void FramePerformance::clear_cpu() {
    cpu_next_=cpu_count_=0;cpu_seen_=cpu_sequence_=cpu_frame_=0;cpu_entries_={};interval_next_=interval_count_=0;previous_sample_.reset();
}
void FramePerformance::observe(const renderer::Frame& frame,const renderer::FrameResult& result,Clock::time_point now) {
    if(result.status!=renderer::FrameStatus::accepted) return;
    Domain current{frame.scene_identity,result.accepted_extent,frame.scene_viewport,frame.view,
        frame.view.shading==renderer::Shading::scene_lit && frame.view.wireframe!=renderer::Wireframe::only &&
        !frame.meshes.empty() && frame.scene_viewport.width>0 && frame.scene_viewport.height>0};
    bool force=!domain_ || *domain_!=current;
    if(force) {
        domain_=current;first_frame_=frame.ui_frame.id;sequence_=frame_id_=0;next_=count_=0;clear_cpu();dirty_=true;
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
        sample[20]=stats.scene_lit_direct_cluster_gpu_ms;
        sample[21]=stats.scene_lit_shadow_cache_gpu_ms;
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
void FramePerformance::observe(const timing::FrameRecord& record,std::optional<std::uint64_t> sampled_frame,Clock::time_point now) {
    if(record.structure_errors) {
        structure_errors_+=record.structure_errors;
        if(!first_structure_error_) first_structure_error_=record.first_error;
        dirty_=true;
    }
    if(!sampled_frame) return;
    auto& sample=cpu_[cpu_next_];
    sample.entered=0;
    double children{};
    for(const auto* marker:timing::catalog()) {
        const auto index=marker->index();
        sample.ms[index]=milliseconds(record.duration[index]);
        cpu_entries_[index]=record.entries[index];
        if(record.entries[index]) sample.entered|=std::uint64_t{1}<<index;
        if(marker->parent()==&timing::frame) children+=sample.ms[index];
    }
    sample.unattributed=std::max(0.,sample.ms[timing::frame.index()]-children);
    cpu_seen_|=sample.entered;
    const bool first=cpu_count_==0;
    cpu_next_=(cpu_next_+1)%window_size;cpu_count_=std::min(cpu_count_+1,window_size);
    cpu_sequence_=record.sequence;cpu_frame_=*sampled_frame;
    // Only adjacent steps of one domain form an interval; a skipped or captured step breaks the chain.
    bool first_interval{};
    if(previous_sample_ && previous_sample_->first+1==record.sequence) {
        intervals_[interval_next_]=milliseconds(record.begin-previous_sample_->second);
        interval_ends_[interval_next_]=record.begin;
        first_interval=interval_count_==0;
        interval_next_=(interval_next_+1)%window_size;interval_count_=std::min(interval_count_+1,window_size);
    }
    previous_sample_.emplace(record.sequence,record.begin);
    dirty_=true;
    if(first || first_interval || now>=next_publish_) publish(now);
}
void FramePerformance::publish(Clock::time_point now) {
    published_={};published_.resource_bytes=resource_bytes_;
    if(domain_) {
        published_.extent=domain_->extent;published_.viewport=domain_->viewport;
        published_.scene_identity=domain_->scene;published_.first_frame=first_frame_;
        published_.antialiasing=domain_->view.lit.antialiasing;
        published_.direct_candidates=domain_->view.lit.direct_candidates;
        published_.ddgi=domain_->view.lit.ddgi;published_.direct_reference=domain_->view.lit.direct_reference;
        published_.direct_clustered=domain_->view.lit.direct_clustered;
        published_.direct_shadow_cache=domain_->view.lit.direct_shadow_cache;
    }
    published_.state=domain_ && domain_->active?(count_?State::ready:State::waiting):State::unavailable;
    published_.samples=count_;published_.sequence=sequence_;published_.frame_id=frame_id_;
    std::array<double,window_size> values;
    if(count_) {
        const auto latest=(next_+window_size-1)%window_size;
        for(std::size_t pass=0;pass<pass_count;++pass) {
            Timing timing;timing.current=samples_[latest][pass];timing.current_active=(masks_[latest]&(1u<<pass))!=0;
            for(std::size_t i=0;i<count_;++i) {values[i]=samples_[i][pass];timing.active_samples+=(masks_[i]&(1u<<pass))!=0;}
            distribution({values.data(),count_},timing);
            published_.timings[pass]=timing;
        }
        const double total=published_.timings[total_pass]->mean;
        if(total>0) for(auto& timing:published_.timings) timing->share=timing->mean/total*100.;
    }
    auto& cpu=published_.cpu;
    cpu.samples=cpu_count_;cpu.sequence=cpu_sequence_;cpu.frame_id=cpu_frame_;cpu.current_entries=cpu_entries_;
    if(cpu_count_) {
        const auto& latest=cpu_[(cpu_next_+window_size-1)%window_size];
        const auto statistics=[&](auto value,std::uint64_t bit) {
            Timing timing;timing.current=value(latest);timing.current_active=(latest.entered&bit)==bit;
            for(std::size_t i=0;i<cpu_count_;++i) {values[i]=value(cpu_[i]);timing.active_samples+=(cpu_[i].entered&bit)==bit;}
            distribution({values.data(),cpu_count_},timing);
            return timing;
        };
        for(const auto* marker:timing::catalog()) {
            const auto index=marker->index();const auto bit=std::uint64_t{1}<<index;
            if(cpu_seen_&bit) cpu.phases[index]=statistics([index](const CpuSample& sample){return sample.ms[index];},bit);
        }
        cpu.unattributed=statistics([](const CpuSample& sample){return sample.unattributed;},0);
        if(const double frame=cpu.phases[timing::frame.index()]->mean;frame>0) {
            for(auto& phase:cpu.phases) if(phase) phase->share=phase->mean/frame*100.;
            cpu.unattributed->share=cpu.unattributed->mean/frame*100.;
        }
    }
    if(interval_count_) {
        const auto latest=(interval_next_+window_size-1)%window_size;
        Timing timing;timing.current=intervals_[latest];timing.current_active=true;timing.active_samples=interval_count_;
        std::copy_n(intervals_.begin(),interval_count_,values.begin());
        distribution({values.data(),interval_count_},timing);
        cpu.interval=timing;
        double sum{};std::size_t recent{};
        for(;recent<interval_count_;++recent) {
            const auto at=(latest+window_size-recent)%window_size;
            if(interval_ends_[latest]-interval_ends_[at]>std::chrono::milliseconds(500)) break;
            sum+=intervals_[at];
        }
        cpu.recent_interval=sum/double(recent);
    }
    status_=std::format(L"CPU {} / {} · GPU ",cpu_count_,window_size);
    if(published_.state==State::unavailable) status_+=L"unavailable · choose Scene Lit with visible geometry";
    else if(published_.state==State::waiting) status_+=std::format(L"waiting · Target {} × {} · View {:.0f} × {:.0f}",
        published_.extent.width,published_.extent.height,published_.viewport.width,published_.viewport.height);
    else status_+=std::format(L"Target {} × {} · View {:.0f} × {:.0f} · {} / {} samples",published_.extent.width,published_.extent.height,
        published_.viewport.width,published_.viewport.height,count_,window_size);
    if(structure_errors_) status_+=std::format(L" · {} timing structure errors, first {}",structure_errors_,
        first_structure_error_?wide(first_structure_error_->key()):std::wstring(L"frame"));
    resources_=resource_bytes_?std::format(L"Logical resources: {:.2f} MiB · DI {}{} · {} · DDGI {} · 4 Hz",double(*resource_bytes_)/(1024.*1024.),
        published_.direct_candidates,published_.direct_reference?L" reference":L"",aa_names[static_cast<std::size_t>(published_.antialiasing)],published_.ddgi?L"on":L"off"):
        L"Scene Lit logical resources: unavailable";
    dirty_=false;next_publish_=now+std::chrono::milliseconds(250);rebuild_table();
}
void FramePerformance::rebuild_table() {
    auto data=std::make_shared<ui::TableData>();
    constexpr std::array titles{L"Phase",L"Current ms",L"Mean ms",L"P95 ms",L"Max ms",L"Share %"};
    for(std::size_t i=0;i<titles.size();++i)
        data->columns.push_back({i+1,titles[i],widths_[i],minima[i],i?ui::TextAlign::right:ui::TextAlign::left,false});
    const auto add=[&](ui::TableKey key,std::size_t depth,std::wstring_view title,const std::optional<Timing>& timing,std::wstring_view inactive) {
        const bool ready=timing && timing->active_samples;
        const std::array<std::wstring,6> values{std::wstring(depth*3,L' ').append(title),
            !timing?L"—":timing->current_active?std::format(L"{:.3f}",timing->current):std::wstring(inactive),
            ready?std::format(L"{:.3f}",timing->mean):L"—",ready?std::format(L"{:.3f}",timing->p95):L"—",
            ready?std::format(L"{:.3f}",timing->max):L"—",ready && timing->share?std::format(L"{:.1f}",*timing->share):L"—"};
        ui::TableRow row{key,{}};
        for(std::size_t column=0;column<values.size();++column) row.cells.push_back({key*8+column+1,values[column]});
        data->rows.push_back(std::move(row));
    };
    const auto& cpu=published_.cpu;
    if(cpu_rows_.empty()) {
        const auto catalog=timing::catalog();
        const auto visit=[&](const auto& self,const timing::Marker* parent)->void {
            for(const auto* marker:catalog) if(marker->parent()==parent) {cpu_rows_.push_back(marker);self(self,marker);}
        };
        visit(visit,&timing::frame);
    }
    // A CPU row stays from its first entry until the domain resets, so rows do not come and go.
    add(cpu_frame_row,0,L"CPU frame",timing::frame.registered()?cpu.phases[timing::frame.index()]:std::optional<Timing>{},L"—");
    for(const auto* marker:cpu_rows_)
        if(cpu.phases[marker->index()]) add(cpu_frame_row+marker->index(),timing::depth(*marker),wide(marker->title()),cpu.phases[marker->index()],L"—");
    if(cpu.unattributed) add(cpu_unattributed_row,1,L"Unattributed",cpu.unattributed,L"—");
    for(auto index:row_order) {
        const auto& pass=definitions[index];
        add(index+1,pass.depth,wide(pass.title),published_.timings[index],L"Skipped");
    }
    table_=std::move(data);
}
bool FramePerformance::apply(const ui::Action& action) {
    if(action.kind==ui::ActionKind::select) {
        if(const auto* value=std::get_if<ui::TableSelection>(&action.value);value && value->column>=1 && value->column<=widths_.size() &&
            std::ranges::find(table_->rows,value->row,&ui::TableRow::key)!=table_->rows.end())
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
