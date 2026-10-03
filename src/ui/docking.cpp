#include "ui/docking.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace atelier::ui {
namespace {
constexpr float tab_height=22, divider_width=8;
constexpr ControlKey close_offset=1'000'000, wrapper_offset=2'000'000, open_offset=3'000'000, preview_offset=4'000'000;
bool inside(Rect r,float x,float y) { return x>=r.x && y>=r.y && x<r.x+r.width && y<r.y+r.height; }
Control& add(std::vector<Control>& controls,ControlKey key,ControlKey parent,ControlKind kind,
             ui_layout::NodeInput layout,std::wstring text={}) {
    Control control; control.key=key; control.parent=parent; control.kind=kind;
    control.layout=std::move(layout); control.text=std::move(text);
    controls.push_back(std::move(control)); return controls.back();
}
}
DockSpace::DockSpace(ControlKey base,std::vector<DockTab> tabs) : base_(base),next_(base),tabs_(std::move(tabs)) {
    if (!base || base>std::numeric_limits<ControlKey>::max()-preview_offset-1 || tabs_.empty() || tabs_.size()>64)
        throw std::invalid_argument("Invalid dock registry or key range.");
    std::vector<ControlKey> keys;
    for (const auto& view:tabs_) {
        if (!view.key || !view.content || view.title.empty() || !std::isfinite(view.minimum.width) || !std::isfinite(view.minimum.height) ||
            view.minimum.width<120 || view.minimum.height<60) throw std::invalid_argument("Invalid dock view.");
        for (auto key:{view.key,view.content}) {
            if ((key>=base && key<=base+preview_offset) || std::find(keys.begin(),keys.end(),key)!=keys.end())
                throw std::invalid_argument("Overlapping dock identities.");
            keys.push_back(key);
        }
    }
    reset();
}
DockSpace::~DockSpace()=default;
std::unique_ptr<DockSpace::Node> DockSpace::make_node() {
    if (next_>=base_+close_offset-16) throw std::length_error("Dock identities exhausted.");
    auto result=std::make_unique<Node>(); result->key=next_; next_+=16; return result;
}
const DockTab* DockSpace::tab(ControlKey key) const noexcept {
    const auto at=std::find_if(tabs_.begin(),tabs_.end(),[&](const auto& view){return view.key==key;});
    return at==tabs_.end()?nullptr:&*at;
}
DockSpace::Node* DockSpace::find(ControlKey key) const noexcept {
    const auto visit=[&](auto&& self,Node* node)->Node* {
        if (node->key==key) return node;
        if (!node->split()) return nullptr;
        if (auto* found=self(self,node->first.get())) return found;
        return self(self,node->second.get());
    };
    return visit(visit,root_.get());
}
DockSpace::Node* DockSpace::owner(ControlKey key) const noexcept {
    const auto visit=[&](auto&& self,Node* node)->Node* {
        if (!node->split()) return std::find(node->tabs.begin(),node->tabs.end(),key)!=node->tabs.end()?node:nullptr;
        if (auto* found=self(self,node->first.get())) return found;
        return self(self,node->second.get());
    };
    return visit(visit,root_.get());
}
void DockSpace::reset() {
    root_=make_node();
    for (const auto& view:tabs_) root_->tabs.push_back(view.key);
    focused_=root_->active=tabs_.front().key;
    maximized_=menu_pane_=0; menu_add_=false; drag_.reset();
}
ControlKey DockSpace::pane(ControlKey key) const noexcept { const auto* node=owner(key); return node?node->key:0; }
bool DockSpace::visible(ControlKey key) const noexcept {
    const auto* node=owner(key);
    if (!node || node->active!=key || (maximized_ && maximized_!=node->key)) return false;
    for (auto* above=parent(node->key);above;node=above,above=parent(node->key))
        if ((above->collapsed==1 && above->first.get()==node) || (above->collapsed==2 && above->second.get()==node)) return false;
    return true;
}
DockSpace::Node* DockSpace::parent(ControlKey key) const noexcept {
    const auto visit=[&](auto&& self,Node* node)->Node* {
        if (!node->split()) return nullptr;
        if (node->first->key==key || node->second->key==key) return node;
        if (auto* found=self(self,node->first.get())) return found;
        return self(self,node->second.get());
    };
    return visit(visit,root_.get());
}
void DockSpace::reveal(ControlKey key) {
    auto* node=owner(key); if (!node) return;
    for (auto* above=parent(node->key);above;node=above,above=parent(node->key)) {
        if ((above->collapsed==1 && above->first.get()==node) || (above->collapsed==2 && above->second.get()==node)) above->collapsed=0;
    }
}
void DockSpace::collapse(Node& node,unsigned side) {
    node.collapsed=node.collapsed==side?0:side;
    maximized_=0; drag_.reset();
    if (!visible(focused_)) {
        focused_=0;
        for (const auto& view:tabs_) if (visible(view.key)) { focused_=view.key; break; }
    }
}
void DockSpace::remove(Node& node,ControlKey key) {
    const auto at=std::find(node.tabs.begin(),node.tabs.end(),key);
    if (at==node.tabs.end()) return;
    const auto index=static_cast<std::size_t>(at-node.tabs.begin());
    node.tabs.erase(at);
    if (node.active==key) node.active=node.tabs.empty()?0:node.tabs[std::min(index,node.tabs.size()-1)];
}
void DockSpace::normalize(std::unique_ptr<Node>& node) {
    if (!node->split()) return;
    normalize(node->first); normalize(node->second);
    if (!node->first->split() && node->first->tabs.empty()) { auto remaining=std::move(node->second); node=std::move(remaining); }
    else if (!node->second->split() && node->second->tabs.empty()) { auto remaining=std::move(node->first); node=std::move(remaining); }
}
bool DockSpace::select(ControlKey key) {
    if (!tab(key)) return false;
    auto* node=owner(key);
    if (!node) {
        node=find(menu_pane_);
        if (!node || node->split()) {
            node=root_.get(); while (node->split()) node=node->second.get();
        }
        node->tabs.push_back(key);
    }
    reveal(key); focused_=node->active=key;
    if (maximized_) maximized_=node->key;
    return true;
}
bool DockSpace::close(ControlKey key) {
    auto* node=owner(key); if (!node) return false;
    remove(*node,key); const auto next=node->active;
    normalize(root_);
    if (maximized_ && !find(maximized_)) maximized_=0;
    if (focused_==key) {
        focused_=next;
        if (!focused_) for (const auto& view:tabs_) if (visible(view.key)) { focused_=view.key; break; }
    }
    drag_.reset(); return true;
}
void DockSpace::maximize(ControlKey key) {
    reveal(key);
    if (const auto* node=owner(key)) { maximized_=maximized_==node->key?0:node->key; focused_=node->active; }
}
void DockSpace::cycle(int direction) {
    auto* node=owner(focused_); if (!node || node->tabs.empty()) return;
    const auto at=std::find(node->tabs.begin(),node->tabs.end(),node->active)-node->tabs.begin();
    const auto count=static_cast<std::ptrdiff_t>(node->tabs.size());
    select(node->tabs[static_cast<std::size_t>((at+count+(direction<0?-1:1))%count)]);
}
void DockSpace::focus_control(ControlKey key,const UiFrame& frame) {
    while (key) {
        for (const auto& view:tabs_) if (key==view.content && visible(view.key)) { focused_=view.key; return; }
        const auto* box=frame.find(key); if (!box) return; key=box->parent;
    }
}
bool DockSpace::move(ControlKey key,ControlKey target,DockSide side,float fraction) {
    const auto* node=owner(target); return node && place(key,node->key,side,0,fraction);
}
bool DockSpace::place(ControlKey key,ControlKey target,DockSide side,ControlKey before,float fraction) {
    if (!tab(key) || !std::isfinite(fraction) || fraction<=0 || fraction>=1) return false;
    auto* to=find(target); auto* from=owner(key);
    if (!to || to->split() || !from || (to==from && side!=DockSide::tabs && from->tabs.size()==1)) return false;
    if (before==key) return true;
    if (before && std::find(to->tabs.begin(),to->tabs.end(),before)==to->tabs.end()) return false;
    remove(*from,key);
    if (side==DockSide::tabs) {
        const auto at=before?std::find(to->tabs.begin(),to->tabs.end(),before):to->tabs.end();
        to->tabs.insert(at,key); to->active=key;
    } else {
        // Preserve the old leaf key and all content keys while inserting a split above it.
        auto previous=make_node(); std::swap(*previous,*to);
        auto added=make_node(); added->tabs={key}; added->active=key;
        to->axis=side==DockSide::left || side==DockSide::right?DragAxis::horizontal:DragAxis::vertical;
        to->fraction=fraction;
        const bool leading=side==DockSide::left || side==DockSide::top;
        to->first=leading?std::move(added):std::move(previous);
        to->second=leading?std::move(previous):std::move(added);
    }
    normalize(root_); maximized_=0; reveal(key); focused_=key; return true;
}
ui_layout::Size DockSpace::minimum(const Node& node) const {
    if (node.split()) {
        auto a=node.collapsed==1?ui_layout::Size{}:minimum(*node.first);
        auto b=node.collapsed==2?ui_layout::Size{}:minimum(*node.second);
        return node.axis==DragAxis::horizontal?ui_layout::Size{a.width+b.width+divider_width,std::max(a.height,b.height)}:
            ui_layout::Size{std::max(a.width,b.width),a.height+b.height+divider_width};
    }
    ui_layout::Size size{120,60};
    for (auto key:node.tabs) { const auto& value=tab(key)->minimum; size.width=std::max(size.width,value.width); size.height=std::max(size.height,value.height); }
    return size;
}
ControlKey DockSpace::divider(ControlKey key,DragAxis axis) const noexcept {
    const auto leaf=pane(key);
    const auto visit=[&](auto&& self,const Node* node)->std::pair<bool,ControlKey> {
        if (!node->split()) return {node->key==leaf,0};
        auto child=self(self,node->first.get()); if (!child.first) child=self(self,node->second.get());
        if (child.first && node->axis==axis) child.second=node->key+1;
        return child;
    };
    return visit(visit,root_.get()).second;
}
std::optional<DockSpace::Drop> DockSpace::destination(ControlKey source,const DragPoint& point,const UiFrame& frame) const {
    const auto* from=owner(source); if (!from) return {};
    const auto scale=frame.drawing->dpi_scale;
    const auto visit=[&](auto&& self,const Node& node)->std::optional<Drop> {
        if (node.split()) { if (auto result=self(self,*node.first)) return result; return self(self,*node.second); }
        const auto* box=frame.find(node.key); if (!box || !inside(box->bounds,point.x,point.y)) return {};
        const auto b=box->bounds;
        Drop drop{node.key,0,DockSide::tabs,{b.x/scale,b.y/scale,b.width/scale,b.height/scale}};
        if (point.y<b.y+tab_height*scale) {
            for (auto key:node.tabs) if (key!=source) if (const auto* item=frame.find(key))
                if (point.x<item->bounds.x+item->bounds.width*.5f) { drop.before=key; break; }
            drop.preview.height=tab_height;
            return drop;
        }
        const float edge=std::min({48.f*scale,b.width*.22f,b.height*.22f});
        if (point.x<b.x+edge) drop.side=DockSide::left;
        else if (point.x>b.x+b.width-edge) drop.side=DockSide::right;
        else if (point.y<b.y+tab_height*scale+edge) drop.side=DockSide::top;
        else if (point.y>b.y+b.height-edge) drop.side=DockSide::bottom;
        if (drop.side!=DockSide::tabs) {
            if (from==&node && from->tabs.size()==1) return {};
            auto other=minimum(node); const auto own=tab(source)->minimum;
            if (drop.side==DockSide::left || drop.side==DockSide::right) {
                if (b.width/scale<other.width+own.width+divider_width) return {};
                drop.preview.width=(drop.preview.width-divider_width)*.5f;
                if (drop.side==DockSide::right) drop.preview.x+=drop.preview.width+divider_width;
            } else {
                if (b.height/scale<other.height+own.height+divider_width) return {};
                drop.preview.height=(drop.preview.height-divider_width)*.5f;
                if (drop.side==DockSide::bottom) drop.preview.y+=drop.preview.height+divider_width;
            }
        }
        return drop;
    };
    return visit(visit,*root_);
}
bool DockSpace::apply(const Action& action,const UiFrame& frame) {
    if (tab(action.key)) {
        if (action.kind==ActionKind::activate) return select(action.key);
        if (action.kind==ActionKind::drag_begin) { drag_=Drag{action.key,{}}; return true; }
        if (action.kind==ActionKind::drag_cancel) { drag_.reset(); return true; }
        if (action.kind==ActionKind::drag_preview || action.kind==ActionKind::drag_commit) {
            if (!drag_ || drag_->tab!=action.key) return true;
            const auto* point=std::get_if<DragPoint>(&action.value); if (!point) return true;
            drag_->target=destination(action.key,*point,frame);
            if (action.kind==ActionKind::drag_commit) {
                const auto target=drag_->target; drag_.reset();
                if (target) place(action.key,target->pane,target->side,target->before);
            }
            return true;
        }
        return false;
    }
    for (std::size_t index=0;index<tabs_.size();++index) {
        if (action.kind==ActionKind::activate && action.key==base_+close_offset+index) return close(tabs_[index].key);
        if (action.kind==ActionKind::activate && action.key==base_+open_offset+index) {
            const auto key=tabs_[index].key;
            // Registered views retain a single state/history when added to another group.
            if (menu_add_ && owner(key) && pane(key)!=menu_pane_) return place(key,menu_pane_,DockSide::tabs);
            return select(key);
        }
    }
    if (action.key<base_ || action.key>=base_+close_offset) return false;
    const auto key=base_+(action.key-base_)/16*16;
    auto* node=find(key); if (!node) return true;
    const auto offset=action.key-key;
    if (node->split()) {
        if (action.kind==ActionKind::activate) {
            if (offset==12) collapse(*node,1);
            else if (offset==13) collapse(*node,2);
            else if (offset==14) {
                const auto* rotate=std::get_if<bool>(&action.value);
                if (rotate && *rotate) node->axis=node->axis==DragAxis::horizontal?DragAxis::vertical:DragAxis::horizontal;
                else { std::swap(node->first,node->second); if (node->collapsed) node->collapsed=3-node->collapsed; }
            }
        } else if (offset==1 || offset==14) {
            if (const auto* value=std::get_if<double>(&action.value))
                if (action.kind==ActionKind::edit_preview || action.kind==ActionKind::edit_commit || action.kind==ActionKind::edit_cancel)
                    node->fraction=static_cast<float>(*value);
        }
        return true;
    }
    if (action.kind==ActionKind::activate) {
        if (offset==3 || offset==6) maximize(node->active);
        else if (offset==5) close(node->active);
        else if ((offset==7 || offset==8) && node->tabs.size()>1) place(node->active,node->key,offset==7?DockSide::right:DockSide::bottom);
        return true;
    }
    return false;
}
std::vector<MenuItem> DockSpace::menu_items(ControlKey source,const UiFrame* frame) {
    Node* node=owner(source);
    if (!node && source>=base_ && source<base_+close_offset) node=find(base_+(source-base_)/16*16);
    if (!node || node->split()) return {};
    menu_pane_=node->key;
    menu_add_=source==add_tab_button(node->key);
    std::vector<MenuItem> result;
    for (std::size_t index=0;index<tabs_.size();++index)
        result.push_back({base_+open_offset+index,(menu_add_?L"":L"Show ")+tabs_[index].title,true,node->active==tabs_[index].key,{}});
    if (menu_add_) return result;
    result.push_back({node->key+6,maximized_?L"Restore Pane":L"Maximize Pane",node->active!=0,false,{}});
    const auto* bounds=frame?frame->find(node->key):nullptr;
    const auto need=minimum(*node);
    const auto own=tab(node->active)?tab(node->active)->minimum:ui_layout::Size{};
    const bool horizontal=node->tabs.size()>1 && bounds && bounds->bounds.width/frame->drawing->dpi_scale>=need.width+own.width+divider_width;
    const bool vertical=node->tabs.size()>1 && bounds && bounds->bounds.height/frame->drawing->dpi_scale>=need.height+own.height+divider_width;
    result.push_back({node->key+7,L"Split Active Tab Right",horizontal,false,{}});
    result.push_back({node->key+8,L"Split Active Tab Below",vertical,false,{}});
    result.push_back({node->key+5,L"Close Active Tab",node->active!=0,false,{}});
    return result;
}
void DockSpace::append_node(std::vector<Control>& controls,const Node& node,ControlKey parent,
                           ui_layout::Size size,const Describe& describe) {
    using namespace ui_layout;
    auto layout=node.split()?(node.axis==DragAxis::horizontal?NodeInput::row():NodeInput::column()):NodeInput::column();
    layout.width=Axis::fixed(size.width); layout.height=Axis::fixed(size.height);
    add(controls,node.key,parent,ControlKind::container,layout).classes={"dock-pane"};
    if (node.split()) {
        const bool horizontal=node.axis==DragAxis::horizontal;
        const auto a=minimum(*node.first),b=minimum(*node.second);
        const auto span=std::max(0.f,(horizontal?size.width:size.height)-divider_width);
        const auto low=horizontal?a.width:a.height, high=horizontal?b.width:b.height;
        const auto floor=span>=low+high?low:span*low/(low+high);
        const auto ceiling=span>=low+high?span-high:floor;
        const auto cut=node.collapsed==1?0.f:node.collapsed==2?span:std::clamp(span*node.fraction,floor,ceiling);
        auto first_size=size,second_size=size;
        (horizontal?first_size.width:first_size.height)=cut;
        (horizontal?second_size.width:second_size.height)=std::max(0.f,span-cut);
        if (node.collapsed!=1) append_node(controls,*node.first,node.key,first_size,describe);
        auto bar=NodeInput::overlay(); bar.width=Axis::fixed(horizontal?std::min(divider_width,size.width):size.width);
        bar.height=Axis::fixed(horizontal?size.height:std::min(divider_width,size.height));
        const auto orientation=horizontal?"vertical":"horizontal";
        add(controls,node.key+15,node.key,ControlKind::container,bar);
        const auto append_splitter=[&](ControlKey key,NodeInput shape,bool grip) {
            auto& divider=add(controls,key,node.key+15,ControlKind::splitter,shape);
            divider.value=span?cut/span:.5f; divider.minimum=span?floor/span:.5f; divider.maximum=span?ceiling/span:.5f;
            divider.drag_axis=node.axis; divider.drag_step=divider.fine_step=1/std::max(span,1.f);
            divider.classes={grip?"dock-grip":"dock-divider",orientation}; divider.click_action=grip;
            // Collapsed geometry must not overwrite the remembered fraction.
            if (node.collapsed) { divider.value=divider.minimum=divider.maximum=node.fraction; }
            if (grip) divider.icon=horizontal?ControlIcon::grip_vertical:ControlIcon::grip_horizontal;
        };
        auto fill=NodeInput::box(0,0); fill.width=fill.height=Axis::expand();
        append_splitter(node.key+1,fill,false);
        // Keep the rail narrow; give all three actions equal, contiguous hit areas.
        // Compress the group together if a small host cannot fit its usual length.
        const float action_length=std::min(36.f,(horizontal?size.height:size.width)/3);
        const float action_width=std::max(0.f,std::min(divider_width,horizontal?size.width:size.height)-4.f/3);
        for (unsigned part=0;part<3;++part) {
            auto shape=NodeInput::box(horizontal?action_width:action_length,horizontal?action_length:action_width);
            shape.out_of_flow=true; shape.anchor_x=shape.anchor_y=shape.pivot_x=shape.pivot_y=.5f;
            const float offset=(static_cast<float>(part)-1)*action_length;
            (horizontal?shape.offset_y:shape.offset_x)=offset;
            if (part==1) append_splitter(node.key+14,shape,true);
            else {
                const bool first=part==0, hidden=node.collapsed==(first?1u:2u);
                auto& button=add(controls,node.key+(first?12:13),node.key+15,ControlKind::button,shape);
                button.icon=horizontal?(first!=hidden?ControlIcon::left:ControlIcon::right):(first!=hidden?ControlIcon::up:ControlIcon::down);
                button.classes={"dock-collapse"}; button.selected=hidden;
            }
        }
        if (node.collapsed!=2) append_node(controls,*node.second,node.key,second_size,describe);
        return;
    }
    auto strip=NodeInput::row(); strip.height=Axis::fixed(tab_height); strip.flow=FlexSpec{};
    add(controls,node.key+2,node.key,ControlKind::container,strip).classes={"dock-tabs"};
    auto tabs=NodeInput::row(); tabs.width=Axis::flex(Basis::content(),0,1,0); tabs.height=Axis::expand();
    tabs.flow=FlexSpec{};
    add(controls,node.key+9,node.key+2,ControlKind::container,tabs);
    for (auto key:node.tabs) {
        const auto* view=tab(key); const auto index=static_cast<ControlKey>(view-tabs_.data());
        auto group=NodeInput::row(); group.width=Axis::flex(Basis::content(),0,1,44); group.height=Axis::expand(); group.flow=FlexSpec{};
        auto& row=add(controls,base_+wrapper_offset+index,node.key+9,ControlKind::container,group);
        row.classes={"dock-tab-group"}; row.selected=key==node.active;
        auto text=NodeInput::box(0,0); text.height=Axis::expand(); text.width=Axis::flex(Basis::content(),1,1,12);
        auto& button=add(controls,key,row.key,ControlKind::tab,text,view->title);
        button.selected=key==node.active; button.context_target=key; button.classes={"dock-tab"};
        button.classes.insert(button.classes.end(),view->classes.begin(),view->classes.end());
        auto close_shape=NodeInput::box(20,0); close_shape.height=Axis::expand();
        auto& close_button=add(controls,base_+close_offset+index,base_+wrapper_offset+index,ControlKind::button,close_shape);
        close_button.icon=ControlIcon::close; close_button.classes={"dock-close"}; close_button.selected=key==node.active;
    }
    auto& add_button=add(controls,add_tab_button(node.key),node.key+2,ControlKind::combo,NodeInput::box(24,tab_height));
    add_button.icon=ControlIcon::plus; add_button.classes={"dock-add"}; add_button.context_target=add_button.key;
    // The elastic gap follows Add, keeping tools reachable when tab text overflows.
    auto gap=NodeInput::box(0,tab_height); gap.width=Axis::expand();
    add(controls,node.key+15,node.key+2,ControlKind::container,gap);
    auto& maximize_button=add(controls,node.key+3,node.key+2,ControlKind::button,NodeInput::box(22,tab_height));
    maximize_button.icon=maximized_?ControlIcon::restore:ControlIcon::maximize; maximize_button.classes={"dock-tool"}; maximize_button.enabled=node.active!=0;
    auto& menu_button=add(controls,node.key+4,node.key+2,ControlKind::combo,NodeInput::box(22,tab_height));
    menu_button.classes={"dock-menu"}; menu_button.icon=ControlIcon::down; menu_button.context_target=node.key+4;
    if (const auto* active=tab(node.active)) describe(*active,controls,node.key);
    else {
        auto blank=NodeInput::box(0,0); blank.width=blank.height=Axis::expand(); blank.padding={12,12,12,12};
        add(controls,node.key+10,node.key,ControlKind::label,blank,L"Open a view from the pane menu.").role=Role::caption;
    }
}
void DockSpace::append(std::vector<Control>& controls,ControlKey parent,ui_layout::Size size,const Describe& describe) {
    auto* node=maximized_?find(maximized_):root_.get(); if (!node) node=root_.get();
    append_node(controls,*node,parent,size,describe);
    if (drag_ && drag_->target) {
        const auto area=drag_->target->preview;
        auto layout=ui_layout::NodeInput::box(area.width,area.height); layout.out_of_flow=true;
        layout.offset_x=area.x; layout.offset_y=area.y;
        auto& preview=add(controls,base_+preview_offset,0,ControlKind::container,layout);
        preview.enabled=false; preview.classes={"dock-preview"};
    }
}
} // namespace atelier::ui
