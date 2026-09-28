#include "ui/session.hpp"
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
ui::Control leaf(std::uint64_t key, ui::ControlKind kind, float y, std::wstring text = {}) {
    ui::Control value; value.key = key; value.kind = kind; value.text = std::move(text);
    value.layout = ui_layout::NodeInput::box(180,30); value.layout.offset_x = 10; value.layout.offset_y = y;
    value.layout.padding = {6,4,6,4}; return value;
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
    require(action.actions.size()==1 && action.actions[0].kind==ui::ActionKind::activate,"button did not activate");
    consumer.controls[0].layout.offset_x = 100; consumer.prepare(false);
    require(consumer.ui.presented()->find(1)->bounds.x==10,"prepare prematurely switched presented geometry");
    const auto presented=consumer.ui.presented();
    consumer.controls[0].font_size=-1;
    require(!consumer.ui.set_controls(consumer.controls) && consumer.ui.presented()==presented,"invalid model discarded old frame");
}
void accepted_geometry_and_surface() {
    Consumer consumer; consumer.controls={leaf(1,ui::ControlKind::button,10,L"Button")}; consumer.prepare();
    const auto old=consumer.frame;
    consumer.controls[0].layout.offset_y=100; consumer.prepare(false);
    consumer.ui.input({.kind=ui::EventKind::pointer_down,.x=20,.y=20});
    const auto up=consumer.ui.input({.kind=ui::EventKind::pointer_up,.x=20,.y=20});
    require(up.actions.size()==1,"unaccepted candidate moved hit geometry");
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
    require(erased.consumed && erased.actions.size()==1 && erased.actions[0].kind==ui::ActionKind::edit_begin,"Delete escaped numeric focus");
    auto typed=consumer.ui.input({.kind=ui::EventKind::text,.character=U'5'});
    require(typed.actions.size()==1 && typed.actions[0].value==5,"numeric preview missing");
    require(consumer.ui.input({.kind=ui::EventKind::capture_lost}).actions.empty(),
        "normal pointer release notification cancelled keyboard editing");
    consumer.controls[0].value=5; consumer.prepare();
    auto commit=consumer.ui.input({.kind=ui::EventKind::key_down,.key=ui::Key::enter});
    require(commit.actions.size()==1 && commit.actions[0].kind==ui::ActionKind::edit_commit,"numeric text did not commit once");
    consumer.ui.input({.kind=ui::EventKind::pointer_down,.x=30,.y=20});
    auto drag=consumer.ui.input({.kind=ui::EventKind::pointer_move,.x=130,.y=20});
    require(drag.actions.size()==2 && std::abs(drag.actions.back().value-6)<1e-8,"drag begin/preview wrong");
    auto fine=consumer.ui.input({.kind=ui::EventKind::pointer_move,.x=140,.y=20,.shift=true});
    require(fine.actions.size()==1 && std::abs(fine.actions[0].value-6.01)<1e-8,"fine drag jumped");
    auto cancel=consumer.ui.input({.kind=ui::EventKind::key_down,.key=ui::Key::escape});
    require(cancel.capture==ui::Capture::release && cancel.actions.size()==1 && cancel.actions[0].kind==ui::ActionKind::edit_cancel &&
        cancel.actions[0].value==5,"Esc did not restore gesture origin");
    require(consumer.ui.input({.kind=ui::EventKind::pointer_up,.x=140,.y=20}).actions.empty(),"cancelled release committed again");
    consumer.ui.input({.kind=ui::EventKind::pointer_down,.x=30,.y=20});
    auto second_drag=consumer.ui.input({.kind=ui::EventKind::pointer_move,.x=80,.y=20});
    require(second_drag.actions.size()==2 && second_drag.actions[0].kind==ui::ActionKind::edit_begin,"drag did not start a new gesture");
    auto released=consumer.ui.input({.kind=ui::EventKind::pointer_up,.x=80,.y=20});
    require(released.actions.size()==1 && released.actions[0].kind==ui::ActionKind::edit_commit &&
        std::abs(released.actions[0].value-5.5)<1e-8,"release did not commit exactly one gesture");
    consumer.click(1);
    consumer.ui.input({.kind=ui::EventKind::text,.character=U'-'});
    auto invalid=consumer.ui.input({.kind=ui::EventKind::key_down,.key=ui::Key::enter});
    require(invalid.actions.size()==1 && invalid.actions[0].kind==ui::ActionKind::edit_cancel,"invalid number committed");
    consumer.click(1); consumer.ui.input({.kind=ui::EventKind::text,.character=U'7'});
    auto removed=accepted(consumer.ui.set_controls({}));
    require(removed.actions.size()==1 && removed.actions[0].kind==ui::ActionKind::edit_cancel &&
        removed.actions[0].value==5,"removed edit target did not cancel with its original value");
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
    require(drag.actions.size()==2 && drag.actions.back().value==-260,"negative splitter values rejected");
    auto lost=consumer.ui.input({.kind=ui::EventKind::capture_lost});
    require(lost.actions.size()==1 && lost.actions[0].value==-280,"capture loss did not cancel splitter");
    consumer.controls.erase(consumer.controls.begin()+2,consumer.controls.begin()+13); consumer.prepare();
    require(consumer.frame->find(20)->bounds.y==0,"shortened list retained out-of-range scroll");
}
void identity_and_lifetime() {
    std::shared_ptr<const ui::UiFrame> held;
    {
        Consumer consumer; consumer.controls={leaf(1,ui::ControlKind::button,10,L"Original")};consumer.prepare();held=consumer.frame;
        consumer.controls.clear(); accepted(consumer.ui.set_controls(consumer.controls));
        consumer.controls={leaf(1,ui::ControlKind::button,10,L"Reused key")};accepted(consumer.ui.set_controls(consumer.controls));
        require(!consumer.ui.input({.kind=ui::EventKind::pointer_down,.x=20,.y=20}).consumed,"reused key accepted old frame hit");
        consumer.prepare();require(consumer.click(1).actions.size()==1,"new incarnation could not activate");
    }
    require(held && held->drawing && !held->drawing->draws.empty(),"UI frame failed to outlive Session");
    for(const auto& draw:held->drawing->draws)if(draw.coverage)require(!draw.coverage->pixels.empty(),"frame lost text pixels after teardown");
}
}
int main() {
    unsigned failures=0;
    const std::pair<const char*,void(*)()> cases[]={{"text_and_frame_versions",text_and_frame_versions},
        {"accepted_geometry_and_surface",accepted_geometry_and_surface},{"numeric_text_and_drag",numeric_text_and_drag},
        {"list_and_splitter",list_and_splitter},{"identity_and_lifetime",identity_and_lifetime}};
    for(const auto& [name,run]:cases)try{run();std::printf("UI %s: PASS\n",name);}catch(const std::exception& error){++failures;std::printf("UI %s: FAIL %s\n",name,error.what());}
    std::printf("UI acceptance: %s (%zu/%zu)\n",failures?"FAIL":"PASS",std::size(cases)-failures,std::size(cases));
    return failures?1:0;
}
