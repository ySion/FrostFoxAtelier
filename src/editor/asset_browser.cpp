#include "editor/asset_browser.hpp"
#include "editor/commands.hpp"
#include <algorithm>
#include <cmath>
#include <cwctype>

namespace atelier::editor {
namespace {
std::wstring_view kind_name(AssetKind kind) {
    switch (kind) { case AssetKind::folder:return L"Folder"; case AssetKind::mesh:return L"Mesh";
    case AssetKind::scene:return L"Scene"; case AssetKind::source:return L"Source"; default:return L"File"; }
}
std::wstring folded(std::wstring text) { for (auto& c:text) c=static_cast<wchar_t>(std::towlower(c)); return text; }
bool matches_filter(const AssetBrowser::Item& item,std::wstring_view query,unsigned filter) {
    return (query.empty() || folded(item.name).find(query)!=std::wstring::npos) &&
        (!filter || item.kind==AssetKind::folder || item.kind==(filter==1?AssetKind::mesh:filter==2?AssetKind::scene:AssetKind::source));
}
int compare_names(std::wstring_view a,std::wstring_view b) {
    for (std::size_t i=0;i<std::min(a.size(),b.size());++i) {
        const auto left=std::towlower(a[i]),right=std::towlower(b[i]);
        if (left!=right) return left<right?-1:1;
    }
    return a.size()==b.size()?0:a.size()<b.size()?-1:1;
}
constexpr std::array column_minima{96.f,64.f,64.f};
std::optional<std::size_t> column_index(ui::TableKey key) {
    if (key>=browser_control::name_column && key<=browser_control::size_column) return static_cast<std::size_t>(key-browser_control::name_column);
    return std::nullopt;
}
ui::ControlIcon item_icon(AssetKind kind) {
    return kind==AssetKind::folder?ui::ControlIcon::folder:kind==AssetKind::mesh?ui::ControlIcon::mesh:ui::ControlIcon::object;
}
}
void AssetBrowser::reset(bool has_project) {
    engine_=!has_project;has_project_=has_project;directory_=has_project?std::filesystem::path{L"Content"}:std::filesystem::path{};
    query_.clear();filter_=0;selected_.reset();targets_.clear();keys_.clear();row_indices_.clear();directories_.clear();project_root_.clear();
    table_data_.reset();active_column_=browser_control::name_column;
    navigation_={{engine_,directory_}};navigation_cursor_=0;catalog_revision_=directory_revision_=0;
    tree_expansion_.clear();tree_frame_=std::numeric_limits<std::uint64_t>::max();
    directory_keys_={browser_control::project,browser_control::engine};
    invalidate_items(); // Keys remain monotonic; the previous project's accepted tree snapshot is not reused.
}
ui::ControlKey AssetBrowser::key_for(const std::string& value) {
    const auto [key,inserted]=keys_.try_emplace(StringView{value},next_key_);if (inserted) ++next_key_;return *key;
}
ui::ControlKey AssetBrowser::directory_control(const std::filesystem::path& path) const {
    if (path.empty()) return browser_control::project;
    const auto name="nav:"+utf8(path.generic_wstring());
    const auto* key=keys_.find(StringView{name});return key?*key:0;
}
void AssetBrowser::visit(Location location,bool remember) {
    if (location.engine) location.directory.clear();
    else {location.directory=location.directory.lexically_normal();if (location.directory==L".") location.directory.clear();}
    if (location==Location{engine_,directory_}) return;
    engine_=location.engine;directory_=location.directory;query_.clear();selected_.reset();targets_.clear();
    if (navigation_.empty()) {navigation_.push_back(location);navigation_cursor_=0;}
    else if (remember) {
        navigation_.resize(navigation_cursor_+1);navigation_.push_back(location);
        if (navigation_.size()>64) navigation_.erase(navigation_.begin());
        navigation_cursor_=navigation_.size()-1;
    } else navigation_[navigation_cursor_]=location;
    invalidate_items();
}
void AssetBrowser::navigate(std::filesystem::path path) { visit({false,std::move(path)}); }
void AssetBrowser::update_directories(const Project* project) {
    const auto root=project?project->root():std::filesystem::path{};
    const auto revision=project?project->revision():0;
    if (root==project_root_ && revision==directory_revision_ && has_project_==(project!=nullptr)) return;
    has_project_=project!=nullptr;project_root_=root;directory_revision_=revision;directories_.clear();
    directory_keys_={browser_control::project,browser_control::engine};
    if (project) for (const auto& entry:project->entries()) if (entry.kind==AssetKind::folder) {
        directories_.push_back({entry.key,entry.path,entry.path.filename().wstring(),entry.kind,0});
        directory_keys_.insert(key_for("nav:"+utf8(entry.path.generic_wstring())));
    }
    std::erase_if(tree_expansion_,[&](const auto& entry){return !directory_keys_.contains(entry.first);});
    const auto valid=[&](const Location& location) {
        return location.engine || (has_project_ && (location.directory.empty() ||
            std::ranges::any_of(directories_,[&](const auto& item){return item.path==location.directory;})));
    };
    if (!valid({engine_,directory_})) visit({!has_project_,{}},false);
    std::size_t destination{},cursor{};
    for (std::size_t i=0;i<navigation_.size();++i) if (valid(navigation_[i])) {
        if (i<=navigation_cursor_) cursor=destination;
        if (destination!=i) navigation_[destination]=std::move(navigation_[i]);
        ++destination;
    }
    navigation_.resize(destination);navigation_cursor_=cursor;
    invalidate_items();
}
void AssetBrowser::sort_items() {
    std::sort(items_.begin(),items_.end(),[&](const auto& a,const auto& b) {
        if ((a.kind==AssetKind::folder)!=(b.kind==AssetKind::folder)) return a.kind==AssetKind::folder;
        int order{};
        if (sort_.column==browser_control::size_column) order=a.bytes==b.bytes?0:a.bytes<b.bytes?-1:1;
        else if (sort_.column==browser_control::type_column) order=kind_name(a.kind).compare(kind_name(b.kind));
        else order=compare_names(a.name,b.name);
        if (order) return sort_.direction==ui::TableSortDirection::ascending?order<0:order>0;
        return a.key<b.key;
    });
    row_indices_.clear();row_indices_.reserve(items_.size());
    for (std::size_t i=0;i<items_.size();++i) row_indices_.emplace(key_for("item:"+items_[i].key),i);
    table_dirty_=true;
}
void AssetBrowser::update_items(const Project* project) {
    if (!items_dirty_ && catalog_revision_==(project?project->revision():0)) return;
    items_.clear();
    if (engine_) for (const auto& asset:mesh_assets()) items_.push_back({asset.key,{},asset.name,AssetKind::mesh,0});
    else if (project) for (const auto& entry:project->entries())
        if (entry.path.parent_path()==directory_) items_.push_back({entry.key,entry.path,entry.path.filename().wstring(),entry.kind,entry.bytes});
    const auto query=folded(query_);
    std::erase_if(items_,[&](const auto& item) {return !matches_filter(item,query,filter_);});
    if (selected_) {
        const auto found=std::ranges::find(items_,selected_->key,&Item::key);
        if (found==items_.end()) selected_.reset();else selected_=*found;
    }
    sort_items();items_dirty_=false;catalog_revision_=project?project->revision():0;
}
bool AssetBrowser::select_item(ui::TableKey key) {
    const auto found=row_indices_.find(key);
    if (found==row_indices_.end()) return false;
    const auto& item=items_[found->second];
    if (!matches_filter(item,folded(query_),filter_)) return false;
    if (!selected_ || selected_->key!=item.key) selected_=item;
    return true;
}
const std::shared_ptr<const ui::TableData>& AssetBrowser::table_data() {
    if (!table_dirty_ && table_data_) return table_data_;
    auto data=std::make_shared<ui::TableData>();
    data->columns={{browser_control::name_column,L"Name",column_widths_[0],column_minima[0]},
        {browser_control::type_column,L"Type",column_widths_[1],column_minima[1]},
        {browser_control::size_column,L"Size",column_widths_[2],column_minima[2],ui::TextAlign::right}};
    data->rows.reserve(items_.size());
    for (const auto& item:items_) {
        const auto row=control(item.key);
        const auto size=item.kind==AssetKind::folder || item.path.empty()?L"":item.bytes<1024?std::to_wstring(item.bytes)+L" B":
            std::to_wstring(item.bytes/1024+(item.bytes%1024!=0))+L" KB";
        data->rows.push_back({row,{{(row<<2)|browser_control::name_column,item.name,item_icon(item.kind)},
            {(row<<2)|browser_control::type_column,std::wstring(kind_name(item.kind))},
            {(row<<2)|browser_control::size_column,size}}});
    }
    data->sort=sort_;
    table_data_=std::move(data);table_dirty_=false;return table_data_;
}
std::vector<ui::MenuItem> AssetBrowser::menu_items(ui::ControlKey source) const {
    namespace id=browser_control;
    std::vector<ui::MenuItem> items;
    const auto radio=[&](ui::ControlKey key,std::wstring title,bool enabled,bool selected,ui::ControlIcon icon=ui::ControlIcon::none) {
        ui::MenuItem item{key,std::move(title),enabled,selected,{}};item.behavior=ui::MenuBehavior::radio;item.radio_group=source;item.icon=icon;
        items.push_back(std::move(item));
    };
    if (source==id::filter) {
        const std::array names{L"All types",L"Meshes",L"Scenes",L"Source"};
        for (std::size_t i=0;i<names.size();++i) radio(id::filter_first+i,names[i],true,filter_==i);
    } else if (source==id::view_menu) {
        radio(id::list,L"List",true,view_==View::list,ui::ControlIcon::view_list);
        radio(id::details,L"Details",true,view_==View::details,ui::ControlIcon::view_details);
        radio(id::tiles,L"Tiles",true,view_==View::tiles,ui::ControlIcon::view_tiles);
    } else if (source==id::path) {
        radio(id::project,L"Project",has_project_,!engine_ && directory_.empty(),ui::ControlIcon::folder);
        if (!engine_) {
            std::filesystem::path ancestor;
            for (const auto& part:directory_) {
                ancestor/=part;
                if (const auto key=directory_control(ancestor)) radio(key,ancestor.generic_wstring(),true,ancestor==directory_,ui::ControlIcon::folder);
            }
        }
        radio(id::engine,L"Engine",true,engine_,ui::ControlIcon::mesh);
    } else {
        const auto target=row_indices_.find(source);
        if (source!=id::actions && source!=id::results && source!=scroll_key_ &&
            (target==row_indices_.end() || !selected_ || selected_->key!=items_[target->second].key)) return {};
        items.push_back({id::open,L"Open",selected_ && (selected_->kind==AssetKind::folder || selected_->kind==AssetKind::scene),false,{}});
        items.back().icon=ui::ControlIcon::folder;
        items.push_back({id::add,L"Add to Scene",can_create_ && selected_ && selected_->kind==AssetKind::mesh,false,{}});
        items.back().icon=ui::ControlIcon::plus;
        for (const auto command:{Command::import_mesh,Command::refresh_assets}) {
            const auto* definition=command_definition(static_cast<ui::ControlKey>(command));
            items.push_back({static_cast<ui::ControlKey>(command),std::wstring(definition->title),has_project_,false,{},std::wstring(definition->shortcut),command==Command::import_mesh});
        }
    }
    return items;
}
bool AssetBrowser::select_context(ui::ControlKey source) {
    if (source==browser_control::results) return selected_ && matches_filter(*selected_,folded(query_),filter_);
    return select_item(source);
}
bool AssetBrowser::apply(const ui::Action& action) {
    using namespace browser_control;
    const auto filter_changed=[&] {
        if (selected_ && !matches_filter(*selected_,folded(query_),filter_)) selected_.reset();
        invalidate_items();
    };
    if (action.key==search) {
        if (const auto* value=std::get_if<std::wstring>(&action.value);value && query_!=*value) {query_=*value;filter_changed();}
        return true;
    }
    if (action.key==results) {
        if (action.kind==ui::ActionKind::select) {
            if (const auto* value=std::get_if<ui::TableSelection>(&action.value);value && column_index(value->column)) {
                if (!value->row) selected_.reset();
                else if (!select_item(value->row)) return true;
                active_column_=value->column;
            }
            return true;
        }
        if (action.kind==ui::ActionKind::table_sort) {
            if (const auto* value=std::get_if<ui::TableSort>(&action.value);value && column_index(value->column) && sort_!=*value) {sort_=*value;sort_items();}
            return true;
        }
        if (action.kind==ui::ActionKind::table_column_resize) {
            if (const auto* value=std::get_if<ui::TableColumnResize>(&action.value);value && std::isfinite(value->width)) {
                if (const auto index=column_index(value->column)) {
                    const auto width=std::max(value->width,column_minima[*index]);
                    if (column_widths_[*index]!=width) {column_widths_[*index]=width;table_dirty_=true;}
                }
            }
            return true;
        }
    }
    if (action.kind!=ui::ActionKind::activate && action.kind!=ui::ActionKind::select) return false;
    if (action.key==project) {if (has_project_) visit({false,{}});return true;}
    if (action.key==browser_control::engine) {visit({true,{}});return true;}
    for (const auto& target:targets_) if (target.key==action.key) {navigate(target.item.path);return true;}
    if (action.kind!=ui::ActionKind::activate) return false;
    if (action.key>=filter_first && action.key<filter_first+4) {filter_=static_cast<unsigned>(action.key-filter_first);filter_changed();return true;}
    switch (action.key) {
    case back:if (navigation_cursor_) {const auto location=navigation_[--navigation_cursor_];visit(location,false);}return true;
    case up:if (!engine_ && !directory_.empty()) navigate(directory_.parent_path());return true;
    case clear:query_.clear();filter_=0;filter_changed();return true;
    case list:view_=View::list;return true;
    case details:view_=View::details;return true;
    case tiles:view_=View::tiles;return true;
    default:break;
    }
    return select_item(action.key);
}
void AssetBrowser::append(std::vector<ui::Control>& controls,ui::ControlKey parent,const Project* project,bool can_create,const ui::UiFrame* frame) {
    using namespace ui_layout; using ui::ControlKind; namespace id=browser_control;
    update_directories(project);can_create_=can_create;
    if (navigation_.empty()) navigation_.push_back({engine_,directory_});
    if (frame && frame->drawing->id!=tree_frame_) {
        if (tree_frame_!=std::numeric_limits<std::uint64_t>::max()) for (const auto& row:frame->tree_rows)
            if (directory_keys_.contains(row.key)) tree_expansion_[row.key]=row.expanded;
        tree_frame_=frame->drawing->id;
    }
    auto auxiliary=id::root+100;
    const auto add=[&](ui::ControlKey key,ui::ControlKey host,ControlKind kind,NodeInput layout,std::wstring text={}) -> ui::Control& {
        ui::Control c; c.key=key;c.parent=host;c.kind=kind;c.layout=std::move(layout);c.text=std::move(text);
        controls.push_back(std::move(c));return controls.back();
    };
    const auto icon_button=[&](ui::ControlKey key,ui::ControlKey host,ui::ControlIcon icon,bool enabled=true,bool menu=false)->ui::Control& {
        auto shape=ui::single_line_layout(23);
        auto& c=add(key,host,menu?ControlKind::combo:ControlKind::button,shape);
        c.classes={"browser-tool","browser-icon"};c.icon=icon;c.enabled=enabled;
        if (menu) c.context_target=key;
        return c;
    };
    auto toolbar=NodeInput::row(2);toolbar.padding={4,3,4,3};toolbar.flow=FlexSpec{};
    const auto navigation=auxiliary++;add(navigation,parent,ControlKind::container,toolbar).classes={"browser-toolbar"};
    icon_button(id::back,navigation,ui::ControlIcon::back,navigation_cursor_!=0);
    icon_button(id::up,navigation,ui::ControlIcon::up,!engine_ && !directory_.empty());
    auto address=ui::single_line_layout();address.width=Axis::expand(1,32);
    auto& path=add(id::path,navigation,ControlKind::combo,address,engine_?L"Engine":directory_.empty()?L"Project":L"Project / "+directory_.generic_wstring());
    path.classes={"browser-path"};path.context_target=id::path;
    const auto searchbar=auxiliary++;auto search_layout=NodeInput::row();search_layout.width=Axis::expand(1,44);
    add(searchbar,navigation,ControlKind::container,search_layout).classes={"browser-search"};
    auto& search_icon=add(auxiliary++,searchbar,ControlKind::label,NodeInput::box(18,23));
    search_icon.icon=ui::ControlIcon::search;search_icon.classes={"browser-search-icon"};
    auto search=ui::single_line_layout();search.width=Axis::expand(1,8);
    add(id::search,searchbar,ControlKind::text_edit,search,query_).classes={"browser-search-input"};
    icon_button(id::clear,searchbar,ui::ControlIcon::close,!query_.empty() || filter_!=0).layout.width=Axis::fixed(18);
    icon_button(id::filter,navigation,ui::ControlIcon::filter,true,true).selected=filter_!=0;
    icon_button(id::view_menu,navigation,view_==View::list?ui::ControlIcon::view_list:view_==View::details?ui::ControlIcon::view_details:ui::ControlIcon::view_tiles,true,true);
    icon_button(id::actions,navigation,ui::ControlIcon::more,true,true);
    const auto body=auxiliary++;auto horizontal=NodeInput::row(1);horizontal.height=Axis::expand();horizontal.flow=FlexSpec{};
    add(body,parent,ControlKind::container,horizontal);
    targets_.clear();
    auto tree=NodeInput::column();tree.width=Axis::flex(Basis::fixed(144),0,1,96);tree.height=Axis::expand();
    add(id::directories,body,ControlKind::tree,tree).classes={"browser-directories"};
    const auto tree_row=[&](ui::ControlKey key,ui::ControlKey owner,std::wstring text,bool selected,bool expanded=true)->ui::Control& {
        auto row=NodeInput::box(0,20);row.width=Axis::expand();
        auto& item=add(key,id::directories,ControlKind::tree_item,row,std::move(text));
        item.tree_parent=owner;item.icon=ui::ControlIcon::folder;item.selected=selected;
        const auto state=tree_expansion_.find(key);item.tree_initially_expanded=state==tree_expansion_.end()?expanded:state->second;
        return item;
    };
    tree_row(id::project,0,L"Project",!engine_ && directory_.empty()).enabled=has_project_;
    for (const auto& entry:directories_) {
        const auto key=directory_control(entry.path);
        const auto relative=directory_.lexically_relative(entry.path);
        const bool ancestor=!engine_ && !relative.empty() && *relative.begin()!=L"..";
        tree_row(key,directory_control(entry.path.parent_path()),entry.name,!engine_ && entry.path==directory_,ancestor);
        targets_.push_back({key,entry});
    }
    tree_row(id::engine,0,L"Engine",engine_).icon=ui::ControlIcon::mesh;
    update_items(project);
    auto viewport=NodeInput::column();viewport.width=Axis::expand(1,64);viewport.height=Axis::expand();
    if (view_==View::details) {
        auto& table=add(id::results,body,ControlKind::table,viewport);table.classes={"asset-table"};
        table.table=table_data();table.action_target=id::open;table.context_target=id::results;
        table.table_selection={selected_?control(selected_->key):0,active_column_};
    } else {
        const auto scroll=scroll_key_;viewport.padding={4,2,4,2};
        add(scroll,body,ControlKind::list,viewport).context_target=id::actions;
        float width=180,height=160;double offset{};
        if (frame) {
            if (const auto* box=frame->find(scroll)) {
                width=std::max(1.f,box->bounds.width/frame->drawing->dpi_scale-8-box->style->scrollbar_width-2*box->style->scrollbar_inset);
                height=box->bounds.height/frame->drawing->dpi_scale;
            }
            for (const auto& bar:frame->scrollbars) if (bar.key==scroll && !bar.horizontal) offset=bar.value;
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
        auto layout=NodeInput::overlay();layout.height=Axis::fixed(std::max(ui::single_line_height,static_cast<float>(visible.extent)));
        add(id::results,scroll,ControlKind::container,layout).context_target=id::actions;
        for (auto index=visible.begin*columns;index<std::min(items_.size(),visible.end*columns);++index) {
            const auto& item=items_[index];const auto key=control(item.key);
            auto shape=NodeInput::box(0,view_==View::tiles?66.f:24.f);shape.width=view_==View::tiles?Axis::fixed(90):Axis::expand();
            shape.out_of_flow=true;shape.offset_y=static_cast<float>(index/columns)*stride;shape.offset_x=static_cast<float>(index%columns)*93;
            auto& c=add(key,id::results,ControlKind::button,shape,item.name);c.classes={view_==View::tiles?"asset-tile":"asset-entry"};
            c.icon=item_icon(item.kind);c.selected=selected_ && selected_->key==item.key;c.context_target=key;
        }
        if (items_.empty()) {
            auto empty=ui::single_line_layout();empty.width=Axis::expand();
            add(auxiliary++,id::results,ControlKind::label,empty,query_.empty() && !filter_?L"This folder is empty.":L"No matches. Clear the search or type filter.");
        }
    }
    auto status=NodeInput::box(0,18);status.width=Axis::expand();status.padding={5,0,5,0};
    const auto count=items_.empty()?(query_.empty() && !filter_?L"This folder is empty.":L"No matches."):std::to_wstring(items_.size())+L" items";
    auto& summary=add(auxiliary++,parent,ControlKind::label,status,count+(selected_?L"  |  "+selected_->name:L""));
    summary.role=ui::Role::caption;summary.classes={"browser-status"};
}
}
