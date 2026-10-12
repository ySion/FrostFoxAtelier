#pragma once
#include "renderer.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace atelier::renderer::detail {
// Matches the shader constant layout. Physical slots stay fixed when the
// logical window scrolls. Previous descriptors address an immutable snapshot.
struct GpuProbeVolume {
    std::array<float,4> origin{},spacing{},tracking{};
    std::array<std::uint32_t,4> grid{}; // XYZ counts, first physical pool slot.
    std::array<std::int32_t,4> scroll{},movement{}; // movement.w invalidates this slot range.
};
static_assert(sizeof(GpuProbeVolume)==96);

struct ProbeField {
    std::array<GpuProbeVolume,probe_volume_limit> current{},previous{};
    std::array<std::array<std::int64_t,3>,probe_volume_limit> cells{},previous_cells{};
    std::array<ProbeVolumeSettings,probe_volume_limit> committed{};
    std::array<std::array<std::uint32_t,4>,probe_resident_limit/4> work{};
    std::array<std::uint8_t,probe_resident_limit> warmup{};
    std::uint32_t total{},width{1},height{1},scheduled{},round_robin_progress{},cursor{},warm_cursor{},stride{1};
    bool valid{};

    static std::uint32_t count(const GpuProbeVolume& v) {return v.grid[0]*v.grid[1]*v.grid[2];}
    static std::int32_t wrap(std::int64_t value,std::uint32_t size) {
        const auto n=static_cast<std::int64_t>(size);return static_cast<std::int32_t>((value%n+n)%n);
    }
    static std::array<std::uint32_t,3> coordinates(std::uint32_t local,const GpuProbeVolume& v) {
        return {local%v.grid[0],local/v.grid[0]%v.grid[1],local/(v.grid[0]*v.grid[1])};
    }
    static bool fresh(std::uint32_t local,const GpuProbeVolume& v) {
        if(v.movement[3])return true;
        const auto physical=coordinates(local,v);
        for(std::size_t a=0;a<3;++a) {
            const auto logical=wrap(std::int64_t(physical[a])-v.scroll[a],v.grid[a]);
            const auto old=logical+v.movement[a];
            if(old<0 || old>=static_cast<std::int32_t>(v.grid[a]))return true;
        }
        return false;
    }
    void invalidate() {valid=false;cursor=0;warm_cursor=0;warmup.fill(0);}
    // Called before encoding. Configuration validation is performed at the
    // public frame boundary; no GPU ownership or scene mutation lives here.
    void describe(const SceneLitSettings& settings,const std::array<float,3>& focus) {
        const auto old_total=total;total=0;
        for(std::size_t slot=0;slot<current.size();++slot) {
            auto& v=current[slot];v={};const auto& s=settings.probe_volumes[slot];
            if(!s.enabled)continue;
            v.grid={s.counts[0],s.counts[1],s.counts[2],total};
            total+=count(v);
            const auto& old=committed[slot];
            v.movement[3]=!valid || !old.enabled || s.minimum!=old.minimum || s.maximum!=old.maximum ||
                s.counts!=old.counts || s.placement!=old.placement || v.grid!=previous[slot].grid;
            double ray_distance_squared=0;
            for(std::size_t a=0;a<3;++a) {
                const double size=double(s.maximum[a])-s.minimum[a];
                const double spacing=size/(s.counts[a]-1);
                v.spacing[a]=static_cast<float>(spacing);ray_distance_squared+=size*size;
                const double location=s.placement==ProbePlacement::follow_focus?double(focus[a])/spacing:0;
                cells[slot][a]=static_cast<std::int64_t>(std::floor(location));
                v.origin[a]=static_cast<float>(double(s.minimum[a])+double(cells[slot][a])*spacing);
                v.tracking[a]=static_cast<float>(location-std::floor(location));
                v.scroll[a]=wrap(cells[slot][a],s.counts[a]);
                const auto delta=cells[slot][a]-previous_cells[slot][a];
                v.movement[a]=static_cast<std::int32_t>(std::clamp(delta,-std::int64_t(s.counts[a]),std::int64_t(s.counts[a])));
            }
            v.origin[3]=static_cast<float>(std::sqrt(ray_distance_squared));
            v.spacing[3]=s.blend_cells;
            v.tracking[3]=s.placement==ProbePlacement::follow_focus?1.f:0.f;
        }
        width=std::min(64u,std::max(1u,total));height=std::max(1u,(total+width-1)/width);
        if(total!=old_total) {cursor=0;warm_cursor=0;stride=std::max(1u,static_cast<std::uint32_t>(double(total)*.61803398875));
            while(total && std::gcd(total,stride)!=1)++stride;}
    }
    void plan(const SceneLitSettings& settings,FrameStats& stats) {
        scheduled=0;round_robin_progress=0;stats.scene_lit_probe_volumes={};
        for(std::size_t slot=0;slot<current.size();++slot) {
            const auto& v=current[slot];auto& out=stats.scene_lit_probe_volumes[slot];const auto n=count(v);
            if(!n)continue;
            out.enabled=true;out.rolling=v.tracking[3]!=0;out.resident=n;
            for(std::size_t a=0;a<3;++a) {out.minimum[a]=v.origin[a];out.maximum[a]=v.origin[a]+v.spacing[a]*float(v.grid[a]-1);
                out.spacing[a]=v.spacing[a];out.counts[a]=v.grid[a];}
            for(std::uint32_t p=0;p<n;++p)if(fresh(p,v)){warmup[v.grid[3]+p]=2;++out.reset_probes;}
        }
        if(!settings.ddgi || total==0)return;
        const auto budget=std::min(settings.probe_update_budget,total);
        std::array<bool,probe_resident_limit> chosen{};
        const auto add=[&](std::uint32_t probe) {
            chosen[probe]=true;work[scheduled/4][scheduled%4]=probe;++scheduled;
            if(warmup[probe])--warmup[probe];
            for(std::size_t slot=0;slot<current.size();++slot) {const auto& v=current[slot];
                if(probe>=v.grid[3] && probe<v.grid[3]+count(v)){++stats.scene_lit_probe_volumes[slot].updated;break;}}
        };
        // A quarter of every budget remains a global round-robin opportunity,
        // including off-screen transport. New slices cannot starve old volumes.
        const auto initialization_budget=budget-(budget+3)/4;
        for(std::uint32_t scanned=0;scanned<total && scheduled<initialization_budget;++scanned) {
            const auto probe=(warm_cursor*stride)%total;warm_cursor=(warm_cursor+1)%total;
            if(warmup[probe])add(probe);
        }
        while(scheduled<budget) {
            const auto probe=(cursor*stride)%total;cursor=(cursor+1)%total;
            ++round_robin_progress;
            if(!chosen[probe])add(probe);
        }
    }
    void commit(const SceneLitSettings& settings) {previous=current;previous_cells=cells;committed=settings.probe_volumes;valid=true;}
};
}
