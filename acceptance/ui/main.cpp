#include "ui/session.hpp"
#include <Windows.h>
#include <icu.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
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
        {"composition_protocol_and_cleanup",composition_protocol_and_cleanup}};
    for(const auto& [name,run]:cases)try{run();std::printf("UI %s: PASS\n",name);}catch(const std::exception& error){++failures;std::printf("UI %s: FAIL %s\n",name,error.what());}
    std::printf("UI acceptance: %s (%zu/%zu)\n",failures?"FAIL":"PASS",std::size(cases)-failures,std::size(cases));
    return failures?1:0;
}
