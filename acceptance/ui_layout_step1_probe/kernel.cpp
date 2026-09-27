#include "kernel.hpp"
#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace layout_step1 {
namespace {
bool nonnegative(double value){return std::isfinite(value)&&value>=0;}
float bounded(float value,const Axis& axis,float floor=0){return std::max(std::max(floor,axis.minimum),std::min(value,axis.maximum.value_or(std::numeric_limits<float>::max())));}
float horizontal(const Node& n){return n.padding.left+n.padding.right;}
float vertical(const Node& n){return n.padding.top+n.padding.bottom;}
bool flow(const Node& n,const Node& parent){return !n.anchored&&parent.kind!=Kind::overlay;}
Result<float> coordinate(double value,uint32_t node){if(!std::isfinite(value)||std::abs(value)>std::numeric_limits<float>::max())return std::unexpected(Failure{Code::numeric_range,node});return static_cast<float>(value);}
bool axis_valid(const Axis& axis,float padding){
    if(static_cast<unsigned>(axis.rule)>static_cast<unsigned>(Rule::expand)||!nonnegative(axis.minimum)||!nonnegative(axis.value))return false;
    if(axis.maximum&&(!nonnegative(*axis.maximum)||*axis.maximum<std::max(axis.minimum,padding)))return false;
    if(axis.rule==Rule::expand&&axis.value==0)return false;
    return axis.rule!=Rule::fixed||(axis.value>=std::max(axis.minimum,padding)&&(!axis.maximum||axis.value<=*axis.maximum));
}
Result<void> distribute_into(std::span<const Share> items,double pool,std::vector<double>& out,std::vector<std::pair<double,size_t>>& events,std::vector<double>& sums){
    if(!nonnegative(pool))return std::unexpected(Failure{Code::bad_input});
    if(items.size()>sums.max_size()/2||items.size()>events.max_size()/2)return std::unexpected(Failure{Code::limit});
    out.resize(items.size());events.clear();double floor_sum=0;
    for(size_t i=0;i<items.size();++i){const auto& item=items[i];
        if(!nonnegative(item.minimum)||!std::isfinite(item.weight)||item.weight<=0||(item.maximum&&(!nonnegative(*item.maximum)||*item.maximum<item.minimum)))return std::unexpected(Failure{Code::bad_input});
        floor_sum+=item.minimum;out[i]=item.minimum;
        const double enter=item.minimum/item.weight;
        if(!std::isfinite(enter))return std::unexpected(Failure{Code::numeric_range});
        events.emplace_back(enter,i*2);
        if(item.maximum){const double leave=*item.maximum/item.weight;if(!std::isfinite(leave))return std::unexpected(Failure{Code::numeric_range});events.emplace_back(leave,i*2+1);}
    }
    if(!std::isfinite(floor_sum))return std::unexpected(Failure{Code::numeric_range});
    if(pool<=floor_sum||items.empty())return {};
    std::sort(events.begin(),events.end());
    const size_t leaf_base=items.size();sums.assign(leaf_base*2,0);
    double remaining=pool-floor_sum,level=0,slope=0;size_t at=0;
    while(at<events.size()){
        const double next=events[at].first,growth=(next-level)*slope;
        if(slope>0&&remaining<=growth){level+=remaining/slope;remaining=0;break;}
        remaining-=growth;level=next;
        while(at<events.size()&&events[at].first==next){
            const auto event=events[at++].second;size_t pos=leaf_base+event/2;
            sums[pos]=(event&1)?0:items[event/2].weight;
            // Recompute ancestors instead of subtracting from a rounded global sum.
            // Small active weights must reappear when larger capped weights leave.
            for(pos/=2;pos;pos/=2)sums[pos]=sums[pos*2]+sums[pos*2+1];
        }
        slope=sums[1];if(!std::isfinite(slope))return std::unexpected(Failure{Code::numeric_range});
    }
    if(remaining>0&&slope>0)level+=remaining/slope;
    if(!std::isfinite(level))return std::unexpected(Failure{Code::numeric_range});
    for(size_t i=0;i<items.size();++i)out[i]=std::clamp(level*items[i].weight,items[i].minimum,items[i].maximum.value_or(std::numeric_limits<double>::max()));
    return {};
}
} // namespace
Result<std::vector<double>> distribute(std::span<const Share> items,double pool){
    std::vector<double> out,sums;std::vector<std::pair<double,size_t>> events;
    if(auto status=distribute_into(items,pool,out,events,sums);!status)return std::unexpected(status.error());return out;
}
Result<void> Kernel::set_nodes(std::span<const Node> input){
    if(active_)return std::unexpected(Failure{Code::busy});
    if(input.empty())return std::unexpected(Failure{Code::bad_input});
    if(input.size()>limits_.nodes||input.size()>=no_parent||input_revision_==std::numeric_limits<uint64_t>::max())return std::unexpected(Failure{Code::limit});
    if(input.size()==nodes_.size()&&std::equal(input.begin(),input.end(),nodes_.begin()))return {};
    const auto count=static_cast<uint32_t>(input.size());std::unordered_set<uint64_t> ids;ids.reserve(count);
    std::vector<uint32_t> offsets(count+1);
    for(uint32_t i=0;i<count;++i){const auto& n=input[i];
        if(!n.id||!ids.insert(n.id).second)return std::unexpected(Failure{Code::bad_input,i});
        if((i==0?n.parent!=no_parent:n.parent>=count||n.parent==i)||static_cast<unsigned>(n.kind)>static_cast<unsigned>(Kind::measured))return std::unexpected(Failure{Code::bad_input,i});
        if(!nonnegative(n.padding.left)||!nonnegative(n.padding.right)||!nonnegative(n.padding.top)||!nonnegative(n.padding.bottom)||!nonnegative(n.gap)||!nonnegative(horizontal(n))||!nonnegative(vertical(n)))return std::unexpected(Failure{Code::bad_input,i});
        if(!axis_valid(n.width,horizontal(n))||!axis_valid(n.height,vertical(n)))return std::unexpected(Failure{Code::bad_input,i});
        for(float f:{n.align_x,n.align_y,n.anchor_x,n.anchor_y,n.pivot_x,n.pivot_y,n.offset_x,n.offset_y})if(!std::isfinite(f))return std::unexpected(Failure{Code::bad_input,i});
        if(n.align_x<0||n.align_x>1||n.align_y<0||n.align_y>1)return std::unexpected(Failure{Code::bad_input,i});
        if(i&&n.kind==Kind::overlay&&(n.width.rule==Rule::content||n.height.rule==Rule::content))return std::unexpected(Failure{Code::unsupported_dependency,i});
        if(i)++offsets[n.parent+1];
    }
    for(uint32_t i=1;i<=count;++i)offsets[i]+=offsets[i-1];
    auto cursor=offsets;std::vector<uint32_t> children(count-1),order,depth(count);order.reserve(count);order.push_back(0);
    for(uint32_t i=1;i<count;++i)children[cursor[input[i].parent]++]=i;
    for(size_t p=0;p<order.size();++p){const uint32_t i=order[p];
        if((input[i].kind==Kind::box||input[i].kind==Kind::measured)&&offsets[i]!=offsets[i+1])return std::unexpected(Failure{Code::bad_input,i});
        for(uint32_t k=offsets[i];k<offsets[i+1];++k){const auto child=children[k];depth[child]=depth[i]+1;if(depth[child]>limits_.depth)return std::unexpected(Failure{Code::limit,child});order.push_back(child);}
    }
    if(order.size()!=count)return std::unexpected(Failure{Code::bad_input});
    bool same_ids=nodes_.size()==count;
    if(same_ids)for(uint32_t i=0;i<count;++i)if(nodes_[i].id!=input[i].id){same_ids=false;break;}
    std::vector<Node> next(input.begin(),input.end());std::vector<Cache> new_cache;
    if(!same_ids)new_cache.resize(count);
    nodes_.swap(next);offsets_.swap(offsets);children_.swap(children);order_.swap(order);if(!same_ids)cache_.swap(new_cache);
    ++input_revision_;return {};
}
Result<Size> Kernel::measured(uint32_t i,std::optional<float> width){
    const auto& node=nodes_[i];const Query query{node.content_key,node.content_revision,provider_revision_,width};
    auto& staged=width?pending_[i].constrained:pending_[i].intrinsic;
    const auto& old=width?cache_[i].constrained:cache_[i].intrinsic;
    if(staged.valid&&staged.attempt==attempt_&&staged.query==query){++stats_.cache_hits;return staged.size;}
    if(old.valid&&old.query==query){++stats_.cache_hits;return old.size;}
    ++stats_.measure_calls;if(width)++stats_.width_queries;else ++stats_.intrinsic_queries;
    if(!measure_)return std::unexpected(Failure{Code::provider,i});
    Result<Size> result=std::unexpected(Failure{Code::provider,i});
    try{result=measure_(user_,query);}catch(...){return std::unexpected(Failure{Code::provider,i});}
    if(!result||!nonnegative(result->width)||!nonnegative(result->height))return std::unexpected(Failure{Code::provider,i});
    if(pending_[i].touched!=attempt_){pending_[i].touched=attempt_;touched_.push_back(i);}
    staged={true,query,*result,attempt_};return *result;
}
Result<float> Kernel::intrinsic(uint32_t start){
    intrinsic_stack_.clear();intrinsic_stack_.push_back(start);
    while(!intrinsic_stack_.empty()){
        const auto i=intrinsic_stack_.back();const auto& node=nodes_[i];
        if(intrinsic_valid_[i]){intrinsic_stack_.pop_back();continue;}
        if(node.width.rule==Rule::fixed){intrinsic_width_[i]=node.width.value;intrinsic_valid_[i]=1;intrinsic_stack_.pop_back();continue;}
        if(node.kind==Kind::overlay)return std::unexpected(Failure{Code::unsupported_dependency,i});
        bool missing=false;
        for(uint32_t k=offsets_[i];k<offsets_[i+1];++k){const auto child=children_[k];if(flow(nodes_[child],node)&&!intrinsic_valid_[child]){intrinsic_stack_.push_back(child);missing=true;}}
        if(missing)continue;
        double value=horizontal(node);uint32_t count=0;
        if(node.kind==Kind::measured){auto metric=measured(i,{});if(!metric)return std::unexpected(metric.error());value+=metric->width;}
        else for(uint32_t k=offsets_[i];k<offsets_[i+1];++k){const auto child=children_[k];if(!flow(nodes_[child],node))continue;
            if(node.kind==Kind::row){value+=intrinsic_width_[child];if(count++)value+=node.gap;}
            else value=std::max(value,static_cast<double>(horizontal(node))+intrinsic_width_[child]);
        }
        const auto value32=coordinate(value,i);if(!value32)return std::unexpected(value32.error());intrinsic_width_[i]=bounded(*value32,node.width,horizontal(node));intrinsic_valid_[i]=1;intrinsic_stack_.pop_back();
    }
    return intrinsic_width_[start];
}
Result<void> Kernel::resolve_widths(Size viewport){
    working_[0].width=viewport.width;slot_width_[0]=viewport.width;
    for(const auto i:order_){++stats_.visited;const auto& parent=nodes_[i];const float available=std::max(0.0f,working_[i].width-horizontal(parent));
        shares_.clear();share_indices_.clear();double fixed=0;uint32_t count=0;
        for(uint32_t k=offsets_[i];k<offsets_[i+1];++k){const auto child=children_[k];const auto& node=nodes_[child];
            const bool shared=parent.kind==Kind::row&&flow(node,parent);
            if(shared){if(count++)fixed+=parent.gap;if(node.width.rule==Rule::expand){shares_.push_back({std::max(node.width.minimum,horizontal(node)),node.width.value,node.width.maximum?std::optional<double>(*node.width.maximum):std::nullopt});share_indices_.push_back(child);continue;}}
            float width=available;
            if(node.width.rule==Rule::fixed)width=node.width.value;
            else if(node.width.rule==Rule::content||!node.fill_width){const auto natural=intrinsic(child);if(!natural)return std::unexpected(natural.error());width=node.width.rule==Rule::expand?std::min(*natural,available):*natural;}
            working_[child].width=bounded(width,node.width,horizontal(node));slot_width_[child]=shared?working_[child].width:available;
            if(shared)fixed+=slot_width_[child];
        }
        if(!shares_.empty()){
            if(auto status=distribute_into(shares_,std::max(0.0,static_cast<double>(available)-fixed),shares_out_,events_,weight_sums_);!status)return status;
            for(size_t k=0;k<share_indices_.size();++k){const auto child=share_indices_[k];const auto& node=nodes_[child];auto slot=coordinate(shares_out_[k],child);if(!slot)return std::unexpected(slot.error());slot_width_[child]=*slot;
                float width=*slot;if(!node.fill_width){auto natural=intrinsic(child);if(!natural)return std::unexpected(natural.error());width=std::min(width,*natural);}working_[child].width=bounded(width,node.width,horizontal(node));}
        }
    }
    return {};
}
Result<void> Kernel::resolve_heights(){
    for(auto p=order_.rbegin();p!=order_.rend();++p){const auto i=*p;const auto& node=nodes_[i];++stats_.visited;double value=vertical(node);uint32_t count=0;
        if(node.kind==Kind::measured&&node.height.rule!=Rule::fixed){auto metric=measured(i,std::max(0.0f,working_[i].width-horizontal(node)));if(!metric)return std::unexpected(metric.error());value+=metric->height;}
        else if(node.kind==Kind::row||node.kind==Kind::column){for(uint32_t k=offsets_[i];k<offsets_[i+1];++k){const auto child=children_[k];const auto& n=nodes_[child];if(!flow(n,node))continue;
            const float child_height=n.height.rule==Rule::fixed?n.height.value:bounded(natural_height_[child],n.height,vertical(n));
            if(node.kind==Kind::column){value+=child_height;if(count++)value+=node.gap;}
            else value=std::max(value,static_cast<double>(vertical(node))+child_height);
        }}
        const auto height=coordinate(value,i);if(!height)return std::unexpected(height.error());natural_height_[i]=bounded(*height,node.height,vertical(node));
    }
    return {};
}
Result<void> Kernel::arrange(Size viewport){
    working_[0].x=0;working_[0].y=0;working_[0].height=viewport.height;
    for(const auto i:order_){++stats_.visited;const auto& parent=nodes_[i];const float width=std::max(0.0f,working_[i].width-horizontal(parent)),height=std::max(0.0f,working_[i].height-vertical(parent));
        shares_.clear();share_indices_.clear();double fixed=0;uint32_t count=0;
        for(uint32_t k=offsets_[i];k<offsets_[i+1];++k){const auto child=children_[k];const auto& node=nodes_[child];const bool shared=parent.kind==Kind::column&&flow(node,parent);
            if(shared){if(count++)fixed+=parent.gap;if(node.height.rule==Rule::expand){shares_.push_back({std::max(node.height.minimum,vertical(node)),node.height.value,node.height.maximum?std::optional<double>(*node.height.maximum):std::nullopt});share_indices_.push_back(child);continue;}}
            float h=node.height.rule==Rule::fixed?node.height.value:(node.height.rule==Rule::content?natural_height_[child]:height);
            if(node.height.rule==Rule::expand&&!node.fill_height)h=std::min(h,natural_height_[child]);
            working_[child].height=bounded(h,node.height,vertical(node));slot_height_[child]=shared?working_[child].height:height;
            if(shared)fixed+=slot_height_[child];
        }
        if(!shares_.empty()){
            if(auto status=distribute_into(shares_,std::max(0.0,static_cast<double>(height)-fixed),shares_out_,events_,weight_sums_);!status)return status;
            for(size_t k=0;k<share_indices_.size();++k){const auto child=share_indices_[k];const auto& node=nodes_[child];auto slot=coordinate(shares_out_[k],child);if(!slot)return std::unexpected(slot.error());slot_height_[child]=*slot;
                float h=*slot;if(!node.fill_height)h=std::min(h,natural_height_[child]);working_[child].height=bounded(h,node.height,vertical(node));}
        }
        double cursor=0;uint32_t placed=0;
        for(uint32_t k=offsets_[i];k<offsets_[i+1];++k){const auto child=children_[k];const auto& node=nodes_[child];auto& box=working_[child];double x=parent.padding.left,y=parent.padding.top;
            if(!flow(node,parent)){x+=static_cast<double>(width)*node.anchor_x-static_cast<double>(box.width)*node.pivot_x+node.offset_x;y+=static_cast<double>(height)*node.anchor_y-static_cast<double>(box.height)*node.pivot_y+node.offset_y;}
            else if(parent.kind==Kind::row){if(placed++)cursor+=parent.gap;x+=cursor+(static_cast<double>(slot_width_[child])-box.width)*node.align_x;y+=(static_cast<double>(height)-box.height)*node.align_y;cursor+=slot_width_[child];}
            else {if(placed++)cursor+=parent.gap;y+=cursor+(static_cast<double>(slot_height_[child])-box.height)*node.align_y;x+=(static_cast<double>(width)-box.width)*node.align_x;cursor+=slot_height_[child];}
            auto px=coordinate(x,child),py=coordinate(y,child);if(!px)return std::unexpected(px.error());if(!py)return std::unexpected(py.error());box.x=*px;box.y=*py;
        }
    }
    return {};
}
Result<View> Kernel::compute(Size viewport,uint64_t provider_revision){
    if(active_)return std::unexpected(Failure{Code::busy});
    if(nodes_.empty()||!nonnegative(viewport.width)||!nonnegative(viewport.height))return std::unexpected(Failure{Code::bad_input});
    if(publication_&&published_input_==input_revision_&&published_provider_==provider_revision&&published_viewport_==viewport)return view();
    if(attempt_==std::numeric_limits<uint64_t>::max()||publication_==std::numeric_limits<uint64_t>::max())return std::unexpected(Failure{Code::limit});
    struct Activity {bool& value;Activity(bool& v):value(v){value=true;}~Activity(){value=false;}} activity(active_);
    ++attempt_;++stats_.computes;provider_revision_=provider_revision;const auto count=nodes_.size();
    working_.resize(count);pending_.resize(count);intrinsic_width_.resize(count);natural_height_.resize(count);slot_width_.resize(count);slot_height_.resize(count);intrinsic_valid_.assign(count,0);touched_.clear();
    for(size_t i=0;i<count;++i)working_[i]={nodes_[i].id,i?nodes_[nodes_[i].parent].id:0,0,0,0,0};
    if(auto status=resolve_widths(viewport);!status)return std::unexpected(status.error());
    if(auto status=resolve_heights();!status)return std::unexpected(status.error());
    if(auto status=arrange(viewport);!status)return std::unexpected(status.error());
    for(uint32_t i=0;i<count;++i){const auto& b=working_[i];if(!std::isfinite(b.x)||!std::isfinite(b.y)||!nonnegative(b.width)||!nonnegative(b.height))return std::unexpected(Failure{Code::numeric_range,i});}
    for(const auto i:touched_){const auto& p=pending_[i];if(p.intrinsic.attempt==attempt_)cache_[i].intrinsic=p.intrinsic;if(p.constrained.attempt==attempt_)cache_[i].constrained=p.constrained;}
    published_.swap(working_);++publication_;++stats_.publications;published_input_=input_revision_;published_provider_=provider_revision;published_viewport_=viewport;return view();
}
}
