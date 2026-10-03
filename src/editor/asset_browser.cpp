#include "editor/asset_browser.hpp"
#include "editor/commands.hpp"
#include <algorithm>
#include <cwctype>

namespace atelier::editor {
namespace {
std::wstring_view kind_name(AssetKind kind) {
    switch (kind) { case AssetKind::folder:return L"Folder"; case AssetKind::mesh:return L"Mesh";
    case AssetKind::scene:return L"Scene"; case AssetKind::source:return L"Source"; default:return L"File"; }
}
std::wstring folded(std::wstring text) { for (auto& c:text) c=static_cast<wchar_t>(std::towlower(c)); return text; }
}
void AssetBrowser::reset(bool has_project) {
    engine_=!has_project; navigate(L"Content"); targets_.clear(); keys_.clear(); // next_key stays monotonic across projects.
}
std::vector<ui::MenuItem> AssetBrowser::filter_menu() const {
    const std::array names{L"All types",L"Meshes",L"Scenes",L"Source"};
    std::vector<ui::MenuItem> items;
    for (std::size_t i=0;i<names.size();++i) items.push_back({browser_control::filter_first+i,names[i],true,filter_==i,{}});
    return items;
}
bool AssetBrowser::apply(const ui::Action& action) {
    using namespace browser_control;
    if (action.key==search) {
        if (const auto* value=std::get_if<std::wstring>(&action.value);value && query_!=*value) {query_=*value;invalidate_items();}
        return true;
    }
    if (action.kind==ui::ActionKind::select) for (const auto& target:targets_)
        if (target.key==action.key && target.navigation) { navigate(target.item.path); return true; }
    if (action.kind!=ui::ActionKind::activate) return false;
    if (action.key>=filter_first && action.key<filter_first+4) {filter_=static_cast<unsigned>(action.key-filter_first);invalidate_items();return true;}
    switch (action.key) {
    case project:engine_=false; selected_.reset();invalidate_items();return true;
    case browser_control::engine:engine_=true;selected_.reset();invalidate_items();return true;
    case up:if (directory_.has_parent_path()) navigate(directory_.parent_path());return true;
    case clear:query_.clear();filter_=0;invalidate_items();return true;
    case list:view_=View::list;return true;
    case details:view_=View::details;return true;
    case tiles:view_=View::tiles;return true;
    default:break;
    }
    for (const auto& target:targets_) if (action.key==target.key) {
        if (target.navigation) navigate(target.item.path); else selected_=target.item;
        return true;
    }
    return false;
}
void AssetBrowser::append(std::vector<ui::Control>& controls,ui::ControlKey parent,const Project* project,bool can_create,const ui::UiFrame* frame) {
    using namespace ui_layout; using ui::ControlKind; namespace id=browser_control;
    auto auxiliary=id::root+100;
    const auto add=[&](ui::ControlKey key,ui::ControlKey host,ControlKind kind,NodeInput layout,std::wstring text={}) -> ui::Control& {
        ui::Control c; c.key=key;c.parent=host;c.kind=kind;c.layout=std::move(layout);c.text=std::move(text);
        controls.push_back(std::move(c));return controls.back();
    };
    const auto button=[&](ui::ControlKey key,ui::ControlKey host,std::wstring text,bool enabled=true,bool selected=false)->ui::Control& {
        auto shape=ui::single_line_layout();shape.width=Axis::content();
        auto& c=add(key,host,ControlKind::button,shape,std::move(text));c.classes={"browser-tool"};c.enabled=enabled;c.selected=selected;return c;
    };
    auto toolbar=NodeInput::row(2);toolbar.padding={4,3,4,3};toolbar.flow=FlexSpec{};toolbar.flow->wrap=Wrap::wrap;
    const auto navigation=auxiliary++;add(navigation,parent,ControlKind::container,toolbar);
    button(id::project,navigation,L"Project",project!=nullptr,!engine_);
    button(id::engine,navigation,L"Engine",true,engine_);
    button(id::up,navigation,L"Up",!engine_ && directory_.has_parent_path()).icon=ui::ControlIcon::up;
    auto address=ui::single_line_layout();address.width=Axis::expand(1,100);
    add(id::path,navigation,ControlKind::label,address,engine_?L"Engine / Primitives":directory_.generic_wstring()).classes={"browser-path"};
    const auto searchbar=auxiliary++;add(searchbar,parent,ControlKind::container,toolbar);
    auto search=ui::single_line_layout();search.width=Axis::expand(1,80);
    add(id::search,searchbar,ControlKind::text_edit,search,query_);
    const std::array filter_names{L"All types",L"Meshes",L"Scenes",L"Source"};
    auto& type_filter=button(id::filter,searchbar,filter_names[filter_],true,filter_!=0);
    type_filter.kind=ControlKind::combo;type_filter.context_target=id::filter;
    button(id::clear,searchbar,L"Clear",!query_.empty() || filter_!=0);
    button(id::list,searchbar,L"List",true,view_==View::list);
    button(id::details,searchbar,L"Details",true,view_==View::details);
    button(id::tiles,searchbar,L"Tiles",true,view_==View::tiles);
    const auto body=auxiliary++;auto horizontal=NodeInput::row(4);horizontal.height=Axis::expand();
    add(body,parent,ControlKind::container,horizontal);
    targets_.clear();
    const auto key_for=[&](const std::string& value) {
        const auto [it,inserted]=keys_.try_emplace(value,next_key_);if (inserted) ++next_key_;return it->second;
    };
    const auto directories=auxiliary++;auto tree=NodeInput::column();tree.width=Axis::fixed(104);tree.height=Axis::expand();
    tree.collapsed=engine_ || !project;
    add(directories,body,ControlKind::tree,tree).classes={"browser-directories"};
    if (!engine_ && project) {
        for (const auto& entry:project->entries()) if (entry.kind==AssetKind::folder) {
            const auto relative=directory_.lexically_relative(entry.path);
            if (entry.path.has_parent_path() && (relative.empty() || *relative.begin()==L"..")) continue;
            const auto key=key_for("nav:"+utf8(entry.path.generic_wstring()));
            auto row=NodeInput::box(0,20);row.width=Axis::expand();
            auto& c=add(key,directories,ControlKind::tree_item,row,entry.path.filename().wstring());
            c.tree_parent=entry.path.has_parent_path()?key_for("nav:"+utf8(entry.path.parent_path().generic_wstring())):0;
            c.icon=ui::ControlIcon::folder;c.selected=entry.path==directory_;
            targets_.push_back({key,{{},entry.path,entry.path.filename().wstring(),AssetKind::folder},true});
        }
    }
    const auto scroll=scroll_key_;auto viewport=NodeInput::column();viewport.width=viewport.height=Axis::expand();viewport.padding={4,2,4,2};
    add(scroll,body,ControlKind::list,viewport);
    if (items_dirty_ || catalog_revision_!=(project?project->revision():0)) {
        items_.clear();
        if (engine_) for (const auto& asset:mesh_assets()) items_.push_back({asset.key,{},asset.name,AssetKind::mesh,0});
        else if (project) for (const auto& entry:project->entries())
            if (entry.path.parent_path()==directory_) items_.push_back({entry.key,entry.path,entry.path.filename().wstring(),entry.kind,entry.bytes});
        std::stable_sort(items_.begin(),items_.end(),[](const auto& a,const auto& b) {
            if ((a.kind==AssetKind::folder)!=(b.kind==AssetKind::folder)) return a.kind==AssetKind::folder;
            return folded(a.name)<folded(b.name);
        });
        if (selected_) {
            const auto found=std::ranges::find_if(items_,[&](const auto& item){return item.key==selected_->key;});
            if (found==items_.end()) selected_.reset();else selected_=*found;
        }
        const auto query=folded(query_);
        std::erase_if(items_,[&](const auto& item) {
            return (!query.empty() && folded(item.name).find(query)==std::wstring::npos) ||
                (filter_ && item.kind!=AssetKind::folder && item.kind!=(filter_==1?AssetKind::mesh:filter_==2?AssetKind::scene:AssetKind::source));
        });
        items_dirty_=false;catalog_revision_=project?project->revision():0;
    }
    float width=180,height=160;double offset{};
    if (frame) {
        if (const auto* box=frame->find(scroll)) {
            width=std::max(1.f,box->bounds.width/frame->drawing->dpi_scale-8-box->style->scrollbar_width-2*box->style->scrollbar_inset);
            height=box->bounds.height/frame->drawing->dpi_scale;
        }
        for (const auto& bar:frame->scrollbars) if (bar.key==scroll) offset=bar.value;
    }
    const auto columns=view_==View::tiles?std::max(std::size_t{1},static_cast<std::size_t>((width+3)/93)):1;
    const auto count=(items_.size()+columns-1)/columns;
    const float stride=view_==View::tiles?69.f:25.f;
    if (count!=rows_.size() || row_height_!=stride) {
        if (!rows_.reset(count,stride)) throw std::runtime_error("Asset result count exceeds the virtual list limit.");
        row_height_=stride;
    }
    if (!rows_.set_viewport(offset,height,2*stride)) throw std::runtime_error("Invalid asset viewport geometry.");
    const auto visible=rows_.range();
    const auto results=auxiliary++;auto layout=NodeInput::overlay();layout.height=Axis::fixed(std::max(ui::single_line_height,static_cast<float>(visible.extent)));
    add(results,scroll,ControlKind::container,layout);
    const auto shown=items_.size();
    for (auto index=visible.begin*columns;index<std::min(items_.size(),visible.end*columns);++index) {
        const auto& item=items_[index];

        const auto key=key_for("item:"+item.key);auto shape=NodeInput::box(0,view_==View::tiles?66.f:24.f);shape.width=view_==View::tiles?Axis::fixed(90):Axis::expand();
        shape.out_of_flow=true;shape.offset_y=static_cast<float>(index/columns)*stride;shape.offset_x=static_cast<float>(index%columns)*93;
        auto host=results;
        if (view_==View::details) {
            host=key_for("row:"+item.key);auto row=NodeInput::overlay();row.height=Axis::fixed(24);row.out_of_flow=true;row.offset_y=shape.offset_y;add(host,results,ControlKind::container,row);
            shape.out_of_flow=false;shape.offset_y=0;
            shape.padding.right=110;
        }
        auto& c=add(key,host,ControlKind::button,shape,item.name);c.classes={view_==View::tiles?"asset-tile":"asset-entry"};
        c.icon=item.kind==AssetKind::folder?ui::ControlIcon::folder:item.kind==AssetKind::mesh?ui::ControlIcon::mesh:ui::ControlIcon::object;
        c.selected=selected_ && selected_->key==item.key;
        if (view_==View::details) {
            for (bool size:{false,true}) {
                auto column=NodeInput::box(size?54.f:56.f,24);column.out_of_flow=true;column.anchor_x=column.pivot_x=1;column.offset_x=size?-4.f:-62.f;
                const auto text=size?(item.bytes?std::to_wstring((item.bytes+1023)/1024)+L" KB":L""):std::wstring(kind_name(item.kind));
                auto& label=add(key_for((size?"size:":"type:")+item.key),host,ControlKind::label,column,text);
                label.enabled=false;label.classes={"asset-column"};
            }
        }
        targets_.push_back({key,item,false});
    }
    if (!shown) {
        auto empty=ui::single_line_layout();empty.width=Axis::expand();
        add(auxiliary++,results,ControlKind::label,empty,query_.empty() && !filter_?L"This folder is empty.":L"No matches. Clear the search or type filter.");
    }
    const auto actions=auxiliary++;add(actions,parent,ControlKind::container,toolbar);
    button(id::open,actions,L"Open",selected_ && (selected_->kind==AssetKind::folder || selected_->kind==AssetKind::scene));
    button(id::add,actions,L"Add to Scene",can_create && selected_ && selected_->kind==AssetKind::mesh);
    button(static_cast<ui::ControlKey>(Command::import_mesh),actions,L"Import OBJ",project!=nullptr);
    button(static_cast<ui::ControlKey>(Command::refresh_assets),actions,L"Refresh",project!=nullptr);
    auto status=ui::single_line_layout();status.width=Axis::expand(1,60);
    add(auxiliary++,actions,ControlKind::label,status,std::to_wstring(shown)+L" items").role=ui::Role::caption;
}
}
