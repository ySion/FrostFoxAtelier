#include "ui/session.hpp"
#include "ui/controls.hpp"
#include "ui/text.hpp"
#include <Windows.h>
#include <icu.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

using namespace atelier;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> T accepted(ui::Result<T> result) {
    if (!result) throw std::runtime_error("UI error " + std::to_string(static_cast<int>(result.error().code)) +
        " native " + std::to_string(result.error().native_code));
    return std::move(*result);
}
void accepted(ui::Result<void> result) { require(result.has_value(),"UI operation failed"); }
const ui::DrawQuad& first_text(const ui::UiFrame& frame) {
    for (const auto& draw : frame.drawing->draws) if (draw.coverage) return draw;
    throw std::runtime_error("frame has no text");
}
std::vector<ui::Action> actions_of(const ui::Reply& reply) {
    std::vector<ui::Action> result;
    for (const auto& effect:reply.effects) if (const auto* action=std::get_if<ui::Action>(&effect)) result.push_back(*action);
    return result;
}
double number_value(const ui::Action& action) { return std::get<double>(action.value); }
ui::Reply key(ui::Session& session, ui::Key command, bool control=false, bool shift=false) {
    auto reply=session.input({.kind=ui::EventKind::key_down,.key=command,.shift=shift,.control=control});
    session.input({.kind=ui::EventKind::key_up,.key=command,.shift=shift,.control=control});
    return reply;
}
ui::Control leaf(std::uint64_t key, ui::ControlKind kind, float y, std::wstring text = {}) {
    ui::Control value; value.key = key; value.kind = kind; value.text = std::move(text);
    value.layout = ui_layout::NodeInput::box(180,30); value.layout.offset_x = 10; value.layout.offset_y = y;
    if (kind == ui::ControlKind::label) value.layout.padding = {6,4,6,4};
    return value;
}
struct Consumer {
    ui::Session ui;
    std::vector<ui::Control> controls;
    std::shared_ptr<const ui::UiFrame> frame;
    Consumer() { accepted(ui.set_surface({{640,480},1,1})); }
    void prepare(bool publish = true) {
        accepted(ui.set_controls(controls)); frame = accepted(ui.prepare());
        if (publish) require(ui.accept(frame),"candidate not accepted");
    }
    ui::Reply click(std::uint64_t key) {
        const auto* box = frame->find(key); require(box,"control missing");
        const float x = box->bounds.x+box->bounds.width/2, y = box->bounds.y+box->bounds.height/2;
        auto down = ui.input({.kind=ui::EventKind::pointer_down,.x=x,.y=y});
        require(down.capture == ui::Capture::acquire,"press did not request capture");
        return ui.input({.kind=ui::EventKind::pointer_up,.x=x,.y=y});
    }
};
void text_and_frame_versions() {
    Consumer consumer;
    consumer.controls = {leaf(1,ui::ControlKind::label,10,L"Scene 场景"),leaf(2,ui::ControlKind::button,50,L"Reset camera")};
    consumer.prepare(); const auto old = consumer.frame;
    std::vector<std::uint8_t> pixels;
    std::shared_ptr<const ui::Coverage> held;
    for (const auto& quad : old->drawing->draws) if (quad.coverage) { held=quad.coverage; pixels=held->pixels; break; }
    require(held && std::any_of(pixels.begin(),pixels.end(),[](auto c){return c > 0 && c < 255;}),"real antialiased text pixels missing");
    consumer.prepare();
    require(std::any_of(consumer.frame->drawing->draws.begin(),consumer.frame->drawing->draws.end(),
        [&](const auto& quad){return quad.coverage == held;}),"unchanged text was rasterized again");
    consumer.controls[0].text = L"Changed 文字"; consumer.prepare();
    require(held->pixels == pixels,"old frame pixels mutated");
    const auto action = consumer.click(2);
    require(actions_of(action).size()==1 && actions_of(action)[0].kind==ui::ActionKind::activate,"button did not activate");
    consumer.controls[0].layout.offset_x = 100; consumer.prepare(false);
    require(consumer.ui.presented()->find(1)->bounds.x==10,"prepare prematurely switched presented geometry");
    const auto presented=consumer.ui.presented();
    consumer.controls[0].role=static_cast<ui::Role>(999);
    require(!consumer.ui.set_controls(consumer.controls) && consumer.ui.presented()==presented,"invalid model discarded old frame");
}
void accepted_geometry_and_surface() {
    Consumer consumer; consumer.controls={leaf(1,ui::ControlKind::button,10,L"Button")}; consumer.prepare();
    const auto old=consumer.frame;
    consumer.controls[0].layout.offset_y=100; consumer.prepare(false);
    consumer.ui.input({.kind=ui::EventKind::pointer_down,.x=20,.y=20});
    const auto up=consumer.ui.input({.kind=ui::EventKind::pointer_up,.x=20,.y=20});
    require(actions_of(up).size()==1,"unaccepted candidate moved hit geometry");
    const auto transition=accepted(consumer.ui.set_surface({{800,600},1.5f,2}));
    require(transition.redraw && !consumer.ui.accept(consumer.frame),"stale surface candidate accepted");
    require(!consumer.ui.input({.kind=ui::EventKind::pointer_down,.x=20,.y=20}).consumed,"surface transition allowed stale hit");
    consumer.prepare();
    require(consumer.frame->find(1)->bounds.y==150 && consumer.frame->drawing->dpi_scale==1.5f,"DPI applied incorrectly");
    require(!consumer.ui.accept(old),"old frame reaccepted");
    accepted(consumer.ui.set_surface({{0,0},1.5f,3}));
    require(!consumer.ui.prepare(),"zero surface produced candidate");
}
void numeric_text_and_drag() {
    Consumer consumer; consumer.controls={leaf(1,ui::ControlKind::number,10)}; consumer.controls[0].value=2;
    consumer.prepare(); consumer.click(1);
    auto erased=consumer.ui.input({.kind=ui::EventKind::key_down,.key=ui::Key::delete_key});
    require(erased.consumed && actions_of(erased).size()==1 && actions_of(erased)[0].kind==ui::ActionKind::edit_begin,"Delete escaped numeric focus");
    auto typed=consumer.ui.input({.kind=ui::EventKind::text,.data=std::wstring{L"5"}});
    require(actions_of(typed).size()==1 && number_value(actions_of(typed)[0])==5,"numeric preview missing");
    require(actions_of(consumer.ui.input({.kind=ui::EventKind::capture_lost})).empty(),
        "normal pointer release notification cancelled keyboard editing");
    consumer.controls[0].value=5; consumer.prepare();
    auto commit=key(consumer.ui,ui::Key::enter);
    require(actions_of(commit).size()==1 && actions_of(commit)[0].kind==ui::ActionKind::edit_commit,"numeric text did not commit once");
    consumer.ui.input({.kind=ui::EventKind::pointer_down,.x=30,.y=20});
    auto drag=consumer.ui.input({.kind=ui::EventKind::pointer_move,.x=130,.y=20});
    require(actions_of(drag).size()==2 && std::abs(number_value(actions_of(drag).back())-6)<1e-8,"drag begin/preview wrong");
    auto fine=consumer.ui.input({.kind=ui::EventKind::pointer_move,.x=140,.y=20,.shift=true});
    require(actions_of(fine).size()==1 && std::abs(number_value(actions_of(fine)[0])-6.01)<1e-8,"fine drag jumped");
    auto cancel=key(consumer.ui,ui::Key::escape);
    require(cancel.capture==ui::Capture::release && actions_of(cancel).size()==1 && actions_of(cancel)[0].kind==ui::ActionKind::edit_cancel &&
        number_value(actions_of(cancel)[0])==5,"Esc did not restore gesture origin");
    require(actions_of(consumer.ui.input({.kind=ui::EventKind::pointer_up,.x=140,.y=20})).empty(),"cancelled release committed again");
    consumer.ui.input({.kind=ui::EventKind::pointer_down,.x=30,.y=20});
    auto second_drag=consumer.ui.input({.kind=ui::EventKind::pointer_move,.x=80,.y=20});
    require(actions_of(second_drag).size()==2 && actions_of(second_drag)[0].kind==ui::ActionKind::edit_begin,"drag did not start a new gesture");
    auto released=consumer.ui.input({.kind=ui::EventKind::pointer_up,.x=80,.y=20});
    require(actions_of(released).size()==1 && actions_of(released)[0].kind==ui::ActionKind::edit_commit &&
        std::abs(number_value(actions_of(released)[0])-5.5)<1e-8,"release did not commit exactly one gesture");
    consumer.controls[0].value=5.5; consumer.prepare(); consumer.click(1);
    consumer.ui.input({.kind=ui::EventKind::text,.data=std::wstring{L"-"}});
    auto invalid=key(consumer.ui,ui::Key::enter);
    require(actions_of(invalid).empty() && consumer.ui.wants_keyboard(),"invalid Enter discarded the draft");
    require(actions_of(key(consumer.ui,ui::Key::escape)).size()==1,"Esc did not cancel the invalid draft");
    consumer.click(1); consumer.ui.input({.kind=ui::EventKind::text,.data=std::wstring{L"7"}});
    auto removed=accepted(consumer.ui.set_controls({}));
    require(actions_of(removed).size()==1 && actions_of(removed)[0].kind==ui::ActionKind::edit_cancel &&
        number_value(actions_of(removed)[0])==5.5,"removed edit target did not cancel with its original value");
}
void list_and_splitter() {
    Consumer consumer;
    ui::Control list; list.key=10; list.kind=ui::ControlKind::list;
    list.layout=ui_layout::NodeInput::column(); list.layout.width=ui_layout::Axis::fixed(200); list.layout.height=ui_layout::Axis::fixed(100);
    consumer.controls.push_back(list);
    for(unsigned i=0;i<12;++i) {
        auto row=leaf(20+i,ui::ControlKind::item,0,L"Object "+std::to_wstring(i)); row.parent=10;
        consumer.controls.push_back(std::move(row));
    }
    auto splitter=leaf(100,ui::ControlKind::splitter,120); splitter.value=-280; splitter.minimum=-440;splitter.maximum=-220;splitter.drag_step=1;
    consumer.controls.push_back(splitter); consumer.prepare();
    auto wheel=consumer.ui.input({.kind=ui::EventKind::wheel,.x=20,.y=50,.wheel=-1});
    require(wheel.consumed,"list did not consume scrolling"); consumer.prepare();
    require(consumer.frame->find(20)->bounds.y<0,"list geometry did not scroll");
    consumer.ui.input({.kind=ui::EventKind::pointer_down,.x=20,.y=130});
    auto drag=consumer.ui.input({.kind=ui::EventKind::pointer_move,.x=40,.y=130});
    require(actions_of(drag).size()==2 && number_value(actions_of(drag).back())==-260,"negative splitter values rejected");
    auto lost=consumer.ui.input({.kind=ui::EventKind::capture_lost});
    require(actions_of(lost).size()==1 && number_value(actions_of(lost)[0])==-280,"capture loss did not cancel splitter");
    consumer.controls.erase(consumer.controls.begin()+2,consumer.controls.begin()+13); consumer.prepare();
    require(consumer.frame->find(20)->bounds.y==0,"shortened list retained out-of-range scroll");
}
void identity_and_lifetime() {
    std::shared_ptr<const ui::UiFrame> held;
    std::weak_ptr<const ui::Coverage> released;
    {
        Consumer consumer; consumer.controls={leaf(1,ui::ControlKind::button,10,L"Original")};consumer.prepare();held=consumer.frame;
        released=first_text(*held).coverage;
        consumer.controls.clear(); accepted(consumer.ui.set_controls(consumer.controls));
        consumer.controls={leaf(1,ui::ControlKind::button,10,L"Reused key")};accepted(consumer.ui.set_controls(consumer.controls));
        require(!consumer.ui.input({.kind=ui::EventKind::pointer_down,.x=20,.y=20}).consumed,"reused key accepted old frame hit");
        consumer.prepare();require(actions_of(consumer.click(1)).size()==1,"new incarnation could not activate");
    }
    require(held && held->drawing && !held->drawing->draws.empty(),"UI frame failed to outlive Session");
    for(const auto& draw:held->drawing->draws)if(draw.coverage)require(!draw.coverage->pixels.empty(),"frame lost text pixels after teardown");
    held.reset(); require(released.expired(),"accounting retained pixels after the last frame was released");
}
void style_and_invalidation() {
    Consumer consumer;
    consumer.controls={leaf(1,ui::ControlKind::number,10),leaf(2,ui::ControlKind::button,70,L"Measured content")};
    consumer.controls[0].value=123.45;
    consumer.controls[1].layout.width=ui_layout::Axis::content();
    consumer.controls[1].layout.height=ui_layout::Axis::content();
    consumer.prepare();
    const auto old=consumer.frame;
    const auto coverage=first_text(*old).coverage;
    const auto pixels=coverage->pixels;
    const auto initial=consumer.ui.statistics();
    auto style=consumer.ui.style(); style.palette.text={.9f,.1f,.2f,1}; style.border_width=2;
    accepted(consumer.ui.set_style(style)); consumer.prepare(false);
    auto after=consumer.ui.statistics();
    require(first_text(*consumer.frame).coverage==coverage && first_text(*consumer.frame).color==style.palette.text,
        "paint style did not reuse tinted coverage");
    require(after.rasterizations==initial.rasterizations && after.measure_calls==initial.measure_calls &&
        after.text_layouts==initial.text_layouts && after.provider_revision==initial.provider_revision,
        "paint style invalidated text resources");
    require(consumer.ui.presented()==old && consumer.frame->style_revision!=old->style_revision,
        "style prepare changed the accepted frame");
    require(consumer.ui.accept(consumer.frame),"tinted frame rejected");
    auto invalid=style; invalid.font.size=std::numeric_limits<float>::quiet_NaN(); invalid.palette.text={0,1,0,1};
    auto rejected=consumer.ui.set_style(invalid);
    require(!rejected && rejected.error().code==ui::ErrorCode::invalid_style && consumer.ui.style()==style &&
        consumer.ui.statistics().provider_revision==initial.provider_revision,"invalid style partially changed configuration");
    invalid=style; invalid.font.family=std::wstring(L"A\0B",3);
    require(!consumer.ui.set_style(invalid) && consumer.ui.style()==style,"embedded NUL accepted as a font family");
    const auto prior_width=consumer.frame->find(2)->bounds.width;
    style.font.family=L"Segoe UI"; style.font.size=22; style.font.weight=500;
    accepted(consumer.ui.set_style(style)); consumer.prepare(); after=consumer.ui.statistics();
    require(after.provider_revision!=initial.provider_revision && after.measure_calls>initial.measure_calls &&
        first_text(*consumer.frame).coverage!=coverage && consumer.frame->find(2)->bounds.width!=prior_width,
        "font policy failed to invalidate layout and pixels");
    require(coverage->pixels==pixels && first_text(*old).color!=style.palette.text,"font update mutated the old frame");
    const auto font_provider=after.provider_revision;
    const auto before_padding=consumer.frame;
    style.control_padding.left+=9;
    accepted(consumer.ui.set_style(style)); consumer.prepare();
    require(consumer.ui.statistics().provider_revision==font_provider &&
        std::abs(consumer.frame->find(2)->bounds.width-before_padding->find(2)->bounds.width-9)<.01f &&
        std::abs(first_text(*consumer.frame).bounds.x-first_text(*before_padding).bounds.x-9)<.01f,
        "measurement and raster did not use the same content inset");
    const auto accepted_frame=consumer.frame;
    auto impossible=style; impossible.control_padding.top=64;
    accepted(consumer.ui.set_style(impossible));
    require(!consumer.ui.prepare() && consumer.ui.presented()==accepted_frame && coverage->pixels==pixels,
        "failed styled layout discarded accepted resources");
    accepted(consumer.ui.set_style(style)); consumer.prepare();
    require(consumer.frame->find(2)->bounds==accepted_frame->find(2)->bounds,"style failure could not recover");
}
void visible_range_and_accounting() {
    Consumer consumer;
    ui::Control list; list.key=1; list.kind=ui::ControlKind::list; list.layout=ui_layout::NodeInput::box(200,96);
    consumer.controls.push_back(list);
    for (unsigned i=0;i<256;++i) {
        auto row=leaf(100+i,ui::ControlKind::item,0,L"Object "+std::to_wstring(i)); row.parent=1;
        row.layout.height=ui_layout::Axis::fixed(24); row.layout.offset_x=0;
        consumer.controls.push_back(std::move(row));
    }
    consumer.prepare();
    auto first=consumer.frame;
    auto first_pixels=first_text(*first).coverage;
    const auto initial=consumer.ui.statistics();
    require(first->controls.size()==257 && first->find(355) && initial.culled_text_controls==252 &&
        initial.rasterizations==4 && initial.live_coverages==4,"offscreen rows were rasterized or lost their geometry");
    require(initial.live_coverage_bytes==initial.raster_bytes && initial.live_coverage_bytes>0,"initial R8 byte accounting is wrong");
    consumer.ui.input({.kind=ui::EventKind::wheel,.x=20,.y=50,.wheel=-1000}); consumer.prepare();
    require(consumer.frame->find(355)->bounds.y<96 && consumer.frame->find(100)->bounds.y<0 &&
        consumer.ui.statistics().rasterizations==8,"scroll did not rasterize only newly visible rows");
    consumer.ui.input({.kind=ui::EventKind::wheel,.x=20,.y=50,.wheel=1000}); consumer.prepare();
    require(first_text(*consumer.frame).coverage==first_pixels && consumer.ui.statistics().rasterizations==8,
        "scrolling back failed to reuse visible text");
    // Retained parent metadata must still transform nested descendants while culled.
    ui::Control parent; parent.key=500; parent.kind=ui::ControlKind::container;
    parent.layout=ui_layout::NodeInput::column(); parent.layout.width=ui_layout::Axis::fixed(120);
    parent.layout.height=ui_layout::Axis::fixed(40); parent.layout.offset_y=600;
    auto child=leaf(501,ui::ControlKind::button,0,L"Nested"); child.parent=500; child.layout.width=ui_layout::Axis::fixed(100);
    consumer.controls={parent,child}; consumer.prepare();
    require(consumer.frame->find(501) && consumer.frame->find(501)->bounds.y==600 && consumer.frame->drawing->draws.empty(),
        "culled parent lost child geometry or emitted draws");
    consumer.controls[0].layout.offset_y=150; consumer.prepare();
    require(consumer.frame->find(501)->bounds.y==150 && first_text(*consumer.frame).coverage,
        "returning nested control did not draw");
    consumer.controls.clear(); consumer.prepare();
    require(consumer.ui.statistics().live_coverages==4,"retired cache resources were retained without owners");
    first.reset();
    require(consumer.ui.statistics().live_coverages==1 &&
        consumer.ui.statistics().live_coverage_bytes==first_pixels->pixels.size(),"live accounting counted aliases or omitted held pixels");
    first_pixels.reset();
    require(consumer.ui.statistics().live_coverages==0 && consumer.ui.statistics().live_coverage_bytes==0,
        "last owner release failed to retire CPU accounting");
    std::printf("UI visible range: 256 rows, first rasterized=%llu, first culled=%llu, scroll+return rasterized=8\n",
        static_cast<unsigned long long>(initial.rasterizations),static_cast<unsigned long long>(initial.culled_text_controls));
}
#include "text_edit.inl"
#include "style_snapshots.inl"
#include "style_sheets.inl"
#include "tree.inl"
#include "menus.inl"
#include "table.inl"
void styled_baseline_placement() {
    for (unsigned mode=0;mode<3;++mode) {
        Consumer consumer;
        ui::Control row; row.key=1; row.kind=ui::ControlKind::container; row.layout=ui_layout::NodeInput::row(10);
        row.layout.width=ui_layout::Axis::fixed(500); row.layout.height=ui_layout::Axis::fixed(100);
        row.layout.flow=ui_layout::FlexSpec{};
        if (mode!=0) row.layout.flow->align_items=ui_layout::Alignment::baseline();
        auto label=leaf(2,ui::ControlKind::label,0,L"Same text"); label.parent=1;
        label.layout.padding={}; label.layout.height=ui_layout::Axis::fixed(60);
        if (mode==0) label.layout.vertical_alignment=ui_layout::Alignment::baseline();
        auto button=leaf(3,ui::ControlKind::button,0,L"Same text"); button.parent=1;
        button.layout.height=ui_layout::Axis::fixed(60);
        if (mode==0) button.layout.vertical_alignment=ui_layout::Alignment::baseline();
        consumer.controls={row,label};
        if (mode==2) {
            ui::Control column; column.key=4; column.parent=1; column.kind=ui::ControlKind::container;
            column.layout=ui_layout::NodeInput::column(); column.layout.width=ui_layout::Axis::fixed(200);
            column.layout.height=ui_layout::Axis::fixed(70); column.layout.padding.top=3;
            consumer.controls.push_back(column); button.parent=4;
        }
        consumer.controls.push_back(button); consumer.prepare();
        std::vector<float> ys;
        for (const auto& draw:consumer.frame->drawing->draws) if (draw.coverage) ys.push_back(draw.bounds.y);
        require(ys.size()==2 && std::abs(ys[0]-ys[1])<.01f,"real glyphs did not share the arranged baseline");
        const auto top_offset=ys[1]-consumer.frame->find(3)->bounds.y;
        consumer.controls[0].layout.flow->align_items=ui_layout::Alignment::stretch();
        for (auto& control:consumer.controls) control.layout.vertical_alignment={};
        consumer.prepare();
        ys.clear(); for (const auto& draw:consumer.frame->drawing->draws) if (draw.coverage) ys.push_back(draw.bounds.y);
        require(ys.size()==2 && ys[1]-consumer.frame->find(3)->bounds.y>top_offset+5,
            "removing baseline context failed to restore centered control text");
    }
}
void prepare_current(Consumer& consumer,bool publish=true) {
    consumer.frame=accepted(consumer.ui.prepare());
    if (publish) require(consumer.ui.accept(consumer.frame),"updated candidate rejected");
}
void button_activation_semantics() {
    Consumer c; c.controls={leaf(1,ui::ControlKind::button,10,L"Toggle")}; c.controls[0].action_target=90;
    c.prepare(); const auto command_frame=c.frame;
    c.controls[0].button_behavior=ui::ButtonBehavior::toggle; c.prepare();
    require(c.frame->find(1)->kind==ui::ControlKind::button && c.frame->drawing->draws.size()==command_frame->drawing->draws.size() &&
        first_text(*c.frame).bounds==first_text(*command_frame).bounds,"Toggle behavior added an indicator or changed the button layout");
    auto actions=actions_of(c.click(1));
    require(actions.size()==1 && actions[0].key==1 && actions[0].target==90 && std::get<bool>(actions[0].value),
        "Toggle click did not request the inverse selected value through its host route");
    c.controls[0].selected=true; accepted(c.ui.update_controls(c.controls));
    actions=actions_of(c.click(1));
    require(actions.size()==1 && std::get<bool>(actions[0].value),"Toggle click used an unaccepted selected value");
    prepare_current(c);
    c.ui.input({.kind=ui::EventKind::key_down,.key=ui::Key::space});
    actions=actions_of(c.ui.input({.kind=ui::EventKind::key_up,.key=ui::Key::space}));
    require(actions.size()==1 && actions[0].target==90 && !std::get<bool>(actions[0].value),"Keyboard toggle did not use the accepted selected value");
    for (bool retarget : {false,true}) for (bool keyboard : {false,true}) {
        c.controls[0].button_behavior=ui::ButtonBehavior::toggle; c.controls[0].action_target=90; c.prepare();
        if (keyboard) {
            c.ui.focus(1);
            require(c.ui.input({.kind=ui::EventKind::key_down,.key=ui::Key::space}).consumed,"Button did not accept the held key");
        } else require(c.ui.input({.kind=ui::EventKind::pointer_down,.x=20,.y=20}).capture==ui::Capture::acquire,
            "Button did not capture the held press");
        if (retarget) c.controls[0].action_target=91; else c.controls[0].button_behavior=ui::ButtonBehavior::command;
        const auto cancelled=accepted(c.ui.update_controls(c.controls));
        require((keyboard || cancelled.capture==ui::Capture::release) && actions_of(cancelled).empty(),
            "Changing button semantics retained its press or emitted an activation");
        require(actions_of(c.ui.input({.kind=ui::EventKind::pointer_up,.x=20,.y=20})).empty() &&
            actions_of(c.ui.input({.kind=ui::EventKind::key_up,.key=ui::Key::space})).empty(),"Retired button input activated its replacement");
        require(c.ui.input({.kind=ui::EventKind::pointer_down,.x=20,.y=20}).capture!=ui::Capture::acquire,
            "Old accepted geometry used a new button behavior or route");
        prepare_current(c); actions=actions_of(c.click(1));
        require(actions.size()==1 && actions[0].target==(retarget?91:90) &&
            (retarget?std::holds_alternative<bool>(actions[0].value):std::holds_alternative<std::monostate>(actions[0].value)),
            "Accepted button semantics did not recover after replacement");
    }
    auto invalid=c.controls[0]; invalid.kind=ui::ControlKind::combo; invalid.action_target=0;
    require(!c.ui.set_controls(std::span(&invalid,1)),"Toggle behavior was accepted for a non-button control");
}
void persistent_control_updates() {
    Consumer c;
    ui::Control parent; parent.key=1; parent.layout=ui_layout::NodeInput::column();
    parent.layout.width=ui_layout::Axis::fixed(280);
    c.controls.push_back(parent);
    for (std::uint64_t id=2;id<=33;++id) {
        auto label=leaf(id,ui::ControlKind::label,0,L"Persistent label"); label.parent=1;
        label.layout.width=label.layout.height=ui_layout::Axis::content();
        c.controls.push_back(std::move(label));
    }
    c.prepare(); const auto initial=c.ui.statistics(); const auto held=c.frame;
    require(initial.controls_created==33 && initial.layout_nodes_created==33,"static nodes were not created once");
    const auto coverage=first_text(*held).coverage;
    const auto pixels=coverage->pixels;
    c.controls[1].style.foreground=ui::Color{.9f,.2f,.1f,1};
    auto reply=accepted(c.ui.update_controls(std::span(&c.controls[1],1)));
    require(reply.redraw && reply.effects.empty(),"paint patch did not request only redraw");
    prepare_current(c);
    const auto color=c.ui.statistics();
    require(color.controls_updated==initial.controls_updated+1 && color.controls_created==initial.controls_created &&
        color.layout_nodes_created==initial.layout_nodes_created && color.layout_syncs==initial.layout_syncs &&
        color.layout.node_visits==initial.layout.node_visits && color.measure_calls==initial.measure_calls &&
        color.rasterizations==initial.rasterizations,"single color patch synchronized or remeasured layout");
    require(first_text(*c.frame).coverage==coverage && coverage->pixels==pixels &&
        c.frame->find(2)->incarnation==held->find(2)->incarnation && c.frame->find(33)->incarnation==held->find(33)->incarnation,
        "paint patch changed identity or old pixels");
    c.controls[0].style.foreground=ui::Color{.1f,.8f,.2f,1};
    accepted(c.ui.update_controls(std::span(&c.controls[0],1))); prepare_current(c);
    const auto inherited=c.ui.statistics();
    require(c.frame->find(3)->style->foreground==*c.controls[0].style.foreground &&
        inherited.layout_syncs==color.layout_syncs && inherited.layout.node_visits==color.layout.node_visits &&
        inherited.measure_calls==color.measure_calls && inherited.rasterizations==color.rasterizations,
        "inherited color invalidated geometry or failed to reach descendants");
    c.controls[1].text=L"Only this natural-size label changes its real measured width.";
    accepted(c.ui.update_controls(std::span(&c.controls[1],1))); prepare_current(c);
    const auto text=c.ui.statistics();
    require(text.controls_updated==inherited.controls_updated+1 && text.layout_syncs==inherited.layout_syncs+1 &&
        text.measure_calls>inherited.measure_calls && text.layout.node_visits>inherited.layout.node_visits &&
        text.layout_nodes_created==initial.layout_nodes_created,"text patch did not synchronize just its measured control");
    const auto before_noop=c.frame->controls_revision;
    accepted(c.ui.update_controls(std::span(&c.controls[1],1))); prepare_current(c);
    const auto noop=c.ui.statistics();
    require(c.frame->controls_revision==before_noop && noop.control_batches==text.control_batches &&
        noop.layout_syncs==text.layout_syncs && noop.layout.node_visits==text.layout.node_visits && noop.measure_calls==text.measure_calls,
        "identical sparse patch performed work");
    c.controls[0].style.font_size=20;
    accepted(c.ui.update_controls(std::span(&c.controls[0],1))); prepare_current(c);
    const auto font=c.ui.statistics();
    require(c.frame->find(3)->style->font.size==20 && font.layout_syncs>noop.layout_syncs+1 &&
        font.measure_calls>noop.measure_calls && held->find(3)->style->font.size!=20,
        "inherited font did not invalidate necessary descendants or changed old frame");
    std::printf("UI sparse update counters: nodes=33, color_control_updates=%llu, color_layout_syncs=%llu, color_layout_visits=%llu, color_measure_calls=%llu, color_rasterizations=%llu, text_layout_syncs=%llu\n",
        static_cast<unsigned long long>(color.controls_updated-initial.controls_updated),
        static_cast<unsigned long long>(color.layout_syncs-initial.layout_syncs),
        static_cast<unsigned long long>(color.layout.node_visits-initial.layout.node_visits),
        static_cast<unsigned long long>(color.measure_calls-initial.measure_calls),
        static_cast<unsigned long long>(color.rasterizations-initial.rasterizations),
        static_cast<unsigned long long>(text.layout_syncs-inherited.layout_syncs));
}
void control_batch_validation() {
    Consumer c;
    c.controls={leaf(1,ui::ControlKind::number,10),leaf(2,ui::ControlKind::button,70,L"Original")};
    c.controls[0].value=2; c.controls[1].layout.width=ui_layout::Axis::content();
    c.prepare(); c.click(1);
    c.ui.input({.kind=ui::EventKind::text,.data=std::wstring{L"9"}});
    prepare_current(c);
    const auto before=c.ui.statistics(); const auto old=c.frame; const auto token=c.ui.text_input()->token;
    auto button=c.controls[1]; button.text=L"A much longer replacement that must not be committed";
    auto invalid=c.controls[0]; invalid.value=invalid.maximum+1;
    const std::array bad{button,invalid};
    require(!c.ui.update_controls(bad),"invalid sparse batch accepted");
    require(!c.ui.set_controls(bad),"invalid full batch accepted");
    const std::array duplicate{c.controls[0],c.controls[0]};
    require(!c.ui.update_controls(duplicate),"duplicate sparse key accepted");
    auto changed=c.controls[0]; changed.key=999;
    require(!c.ui.update_controls(std::span(&changed,1)),"missing sparse key created a node");
    changed=c.controls[0]; changed.parent=2;
    require(!c.ui.update_controls(std::span(&changed,1)),"sparse update changed parent");
    changed=c.controls[0]; changed.kind=ui::ControlKind::text_edit;
    require(!c.ui.update_controls(std::span(&changed,1)),"sparse update changed kind");
    changed=c.controls[0]; changed.context_target=2;
    require(!c.ui.update_controls(std::span(&changed,1)),"sparse update changed binding reference");
    prepare_current(c);
    require(c.frame->controls_revision==old->controls_revision && c.ui.text_input()->token==token &&
        c.frame->find(2)->bounds.width==old->find(2)->bounds.width && c.ui.statistics().controls_updated==before.controls_updated,
        "rejected batch changed descriptors, draft or revision");
    c.controls[0].value=77;
    accepted(c.ui.update_controls(std::span(&c.controls[0],1))); prepare_current(c);
    require(c.frame->find_text(1)->paragraph->matches(L"9"),"authoritative projection overwrote active draft");
    const auto committed=actions_of(key(c.ui,ui::Key::enter));
    require(committed.size()==1 && committed[0].kind==ui::ActionKind::edit_commit && number_value(committed[0])==9,
        "patch changed the active edit commit");
    c.controls[0].value=7;
    accepted(c.ui.update_controls(std::span(&c.controls[0],1))); prepare_current(c);
    require(c.frame->find_text(1)->paragraph->matches(L"7"),"passive draft did not follow authoritative patch");
}
void control_update_cleanup() {
    Consumer c;
    ui::Control parent; parent.key=1; parent.layout=ui_layout::NodeInput::column();
    auto number=leaf(2,ui::ControlKind::number,0); number.parent=1; number.value=2;
    c.controls={parent,number}; c.prepare(); c.click(2);
    c.ui.input({.kind=ui::EventKind::text,.data=std::wstring{L"9"}});
    const auto token=c.ui.text_input()->token;
    c.ui.input({.kind=ui::EventKind::composition,.data=ui::CompositionEvent{ui::CompositionKind::start,501,token,{},{}}});
    c.ui.input({.kind=ui::EventKind::composition,.data=ui::CompositionEvent{ui::CompositionKind::update,501,token,{},ui::Preedit{L"拼",1,{}}}});
    c.controls[0].enabled=false;
    const auto disabled=accepted(c.ui.update_controls(std::span(&c.controls[0],1)));
    const auto cancelled=actions_of(disabled);
    bool ime_cancel=false;
    for (const auto& effect:disabled.effects) if (const auto* platform=std::get_if<ui::PlatformRequest>(&effect))
        if (const auto* ime=std::get_if<ui::ImeRequest>(platform)) ime_cancel|=ime->kind==ui::ImeKind::cancel && ime->composition_id==501;
    require(ime_cancel && cancelled.size()==1 && cancelled[0].kind==ui::ActionKind::edit_cancel &&
        number_value(cancelled[0])==2 && c.ui.focused_control()==0 && !c.ui.text_input(),
        "ancestor disable did not cancel edit and IME through the shared path");
    c.controls[0].enabled=true;
    accepted(c.ui.update_controls(std::span(&c.controls[0],1))); prepare_current(c);
    const auto bounds=c.frame->find(2)->bounds;
    c.ui.input({.kind=ui::EventKind::pointer_down,.x=bounds.x+20,.y=bounds.y+10});
    c.ui.input({.kind=ui::EventKind::pointer_move,.x=bounds.x+70,.y=bounds.y+10});
    c.controls[0].enabled=false;
    const auto released=accepted(c.ui.update_controls(std::span(&c.controls[0],1)));
    require(released.capture==ui::Capture::release && actions_of(released).size()==1 &&
        actions_of(released)[0].kind==ui::ActionKind::edit_cancel,"ancestor patch retained an active capture");
    c.controls[0].enabled=true;
    accepted(c.ui.update_controls(std::span(&c.controls[0],1))); prepare_current(c); c.click(2);
    c.ui.input({.kind=ui::EventKind::text,.data=std::wstring{L"8"}});
    const auto held=c.frame;
    c.controls.pop_back();
    const auto removed=accepted(c.ui.set_controls(c.controls));
    const auto old_hit=c.ui.input({.kind=ui::EventKind::pointer_down,.x=bounds.x+20,.y=bounds.y+10});
    require(actions_of(removed).size()==1 && actions_of(removed)[0].kind==ui::ActionKind::edit_cancel &&
        old_hit.capture!=ui::Capture::acquire && actions_of(old_hit).empty() && c.ui.focused_control()!=2 && held->find(2),
        "full removal diverged from sparse cleanup or old frame lost its resource");

    Consumer menu;
    auto owner=leaf(1,ui::ControlKind::combo,10,L"Owner");
    ui::Control popup; popup.key=2; popup.kind=ui::ControlKind::popup; popup.popup_owner=1;
    popup.layout=ui_layout::NodeInput::column(); popup.layout.width=ui_layout::Axis::fixed(180); popup.layout.offset_y=60;
    auto item=leaf(3,ui::ControlKind::menu_item,0,L"Item"); item.parent=2;
    menu.controls={owner,popup,item}; menu.prepare();
    menu.controls[0].enabled=false;
    auto bad=item; bad.role=static_cast<ui::Role>(999);
    const std::array invalid{menu.controls[0],bad};
    require(!menu.ui.update_controls(invalid) && menu.ui.has_popup(),"rejected batch dismissed an owned popup");
    const auto dismissed=actions_of(accepted(menu.ui.update_controls(std::span(&menu.controls[0],1))));
    require(dismissed.size()==1 && dismissed[0].kind==ui::ActionKind::dismiss && dismissed[0].key==2,
        "owner disable did not dismiss its popup");
}
void control_candidate_revision() {
    Consumer c; c.controls={leaf(1,ui::ControlKind::button,10,L"Button")}; c.prepare();
    const auto held=c.frame; const auto pending=accepted(c.ui.prepare());
    c.controls[0].style.foreground=ui::Color{.8f,.1f,.2f,1};
    accepted(c.ui.update_controls(c.controls));
    require(!c.ui.accept(pending) && c.ui.presented()==held,"descriptor change accepted a stale candidate");
    prepare_current(c);
    require(c.frame->find(1)->incarnation==held->find(1)->incarnation,"paint change replaced incarnation");
    const auto style_pending=accepted(c.ui.prepare());
    auto style=c.ui.style(); style.palette.text={.1f,.8f,.2f,1}; accepted(c.ui.set_style(style));
    require(!c.ui.accept(style_pending),"theme change accepted a stale candidate"); prepare_current(c);
    const auto last_good=c.frame;
    c.controls[0].layout.width=ui_layout::Axis::fixed(-1);
    accepted(c.ui.update_controls(c.controls));
    require(!c.ui.prepare() && c.ui.presented()==last_good,"failed prepare replaced the accepted frame");
    require(!c.ui.input({.kind=ui::EventKind::pointer_down,.x=20,.y=20}).consumed,
        "failed publication routed input through the pending description");
    c.controls[0].layout.width=ui_layout::Axis::fixed(180);
    accepted(c.ui.update_controls(c.controls)); prepare_current(c,false);
    require(!c.ui.input({.kind=ui::EventKind::pointer_down,.x=20,.y=20}).consumed,
        "unaccepted recovery re-enabled input");
    require(c.ui.accept(c.frame) && actions_of(c.click(1)).size()==1,"accepted recovery did not restore input");
    const auto previous=c.frame;
    const auto publication=c.ui.statistics().layout.publications;
    c.controls[0].layout.offset_x=2e8f;
    accepted(c.ui.update_controls(c.controls));
    require(!c.ui.prepare() && c.ui.statistics().layout.publications>publication && c.ui.presented()==previous,
        "failure after geometry publication replaced the complete UI frame");
    require(!c.ui.input({.kind=ui::EventKind::pointer_down,.x=20,.y=20}).consumed,
        "late UI preparation failure allowed old geometry to route pending input");
    c.controls[0].layout.offset_x=10;
    accepted(c.ui.update_controls(c.controls)); prepare_current(c);
    require(actions_of(c.click(1)).size()==1,"late preparation failure did not recover");
    const auto before_suspend=c.frame;
    const auto pending_before_suspend=accepted(c.ui.prepare());
    accepted(c.ui.set_surface({{0,0},1,1}));
    require(!c.ui.prepare(),"suspended surface prepared a frame");
    // Resume the same coordinate mapping, without changing descriptors or theme.
    accepted(c.ui.set_surface({{640,480},1,1}));
    require(!c.ui.accept(before_suspend) && !c.ui.accept(pending_before_suspend),
        "pre-failure frame bypassed the recovery boundary");
    prepare_current(c,false);
    require(!c.ui.accept(pending_before_suspend) &&
        !c.ui.input({.kind=ui::EventKind::pointer_down,.x=20,.y=20}).consumed,
        "unaccepted recovery allowed a pre-failure frame to restore input");
    require(c.ui.accept(c.frame) && c.ui.accept(c.frame) && actions_of(c.click(1)).size()==1,
        "fresh recovery or repeated acceptance failed");
}
}
int main() {
    UVersionInfo icu{},unicode{}; u_getVersion(icu); u_getUnicodeVersion(unicode);
    std::printf("UI system ICU %u.%u, Unicode %u.%u\n",icu[0],icu[1],unicode[0],unicode[1]);
    unsigned failures=0;
    const std::pair<const char*,void(*)()> cases[]={{"text_and_frame_versions",text_and_frame_versions},
        {"accepted_geometry_and_surface",accepted_geometry_and_surface},{"numeric_text_and_drag",numeric_text_and_drag},
        {"list_and_splitter",list_and_splitter},{"identity_and_lifetime",identity_and_lifetime},
        {"style_and_invalidation",style_and_invalidation},{"visible_range_and_accounting",visible_range_and_accounting},
        {"styled_baseline_placement",styled_baseline_placement},{"shared_edit_history_and_focus",shared_edit_history_and_focus},
        {"unicode_visual_editing",unicode_visual_editing},{"clipboard_roundtrips",clipboard_roundtrips},
        {"composition_protocol_and_cleanup",composition_protocol_and_cleanup},{"accepted_input_anchor",accepted_input_anchor},
        {"accepted_text_style_snapshot",accepted_text_style_snapshot},{"style_sheets",style_sheets},{"shared_tree_view",shared_tree_view},
        {"persistent_control_updates",persistent_control_updates},{"control_batch_validation",control_batch_validation},
        {"control_update_cleanup",control_update_cleanup},{"control_candidate_revision",control_candidate_revision},
        {"button_activation_semantics",button_activation_semantics},{"shared_menu_semantics",shared_menu_semantics},
        {"shared_table_view",shared_table_view}};
    for(const auto& [name,run]:cases)try{run();std::printf("UI %s: PASS\n",name);}catch(const std::exception& error){++failures;std::printf("UI %s: FAIL %s\n",name,error.what());}
    std::printf("UI acceptance: %s (%zu/%zu)\n",failures?"FAIL":"PASS",std::size(cases)-failures,std::size(cases));
    return failures?1:0;
}
