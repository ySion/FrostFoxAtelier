#include "application/application.hpp"
#include "atelier/build_info.hpp"
#include <wincodec.h>
#include <wrl/client.h>
#include <msctf.h>
#include <imm.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <stdexcept>
#include <thread>

namespace atelier::application {
struct EditorAcceptanceAccess {
    static void mark_saved(Application& app) { app.effects(app.ui_.cancel_interaction()); app.document_->mark_saved(); }
    static Result<void> command(Application& app,editor::Command command,std::string_view argument={}) { return app.execute(command,argument); }
    static auto asset_key(const Application& app,std::string_view resource) {
        return app.workspace_.assets.control(resource);
    }

    static auto name_control(const Application& app,ObjectId actor) { return app.workspace_.name_control(actor); }
    static void frame_failure(Application& app) {
        const auto frame=app.ui_.prepare();
        std::fprintf(stderr,"Status: %ls\n",app.status_text().c_str());
        if (frame) if (const auto* view=(*frame)->find(editor::control::viewport))
            std::fprintf(stderr,"Surface %u x %u; viewport %.3f %.3f %.3f %.3f\n",(*frame)->drawing->extent.width,(*frame)->drawing->extent.height,
                view->bounds.x,view->bounds.y,view->bounds.width,view->bounds.height);
    }
    static auto camera_view(const Application& app) { return app.camera_.matrices(1).view; }
    static void menu_items(Application& app,std::span<const ui::MenuItem> items) {
        if (!app.popup_ || !app.workspace_.menu) throw std::runtime_error("Menu fixture needs an open native host.");
        RECT anchor{};
        if (!GetWindowRect(app.menu_window(),&anchor)) throw std::runtime_error("Cannot locate native menu fixture.");
        anchor.bottom=anchor.top;
        app.popup_->update(app.workspace_.menu->source,anchor,false,items,app.ui_.style(),app.style_sheet_);
    }
    static void menu_end(Application& app) {
        static_cast<void>(app.popup_->input({.kind=ui::EventKind::key_down,.key=ui::Key::end}));
        static_cast<void>(app.popup_->input({.kind=ui::EventKind::key_up,.key=ui::Key::end}));
    }
    static Result<void> begin_edit(Application& app, ui::ControlKey control) {
        const auto* binding = app.workspace_.binding(control);
        if (!binding || binding->path.empty()) return std::unexpected(Error{ErrorCode::invalid_definition, "Missing field binding."});
        return app.document_->begin_edit(binding->object, binding->path, control);
    }
    static Result<void> edit_field(Application& app, ObjectId component, std::span<const FieldId> path, float value) {
        constexpr auto session = std::numeric_limits<ui::ControlKey>::max();
        auto began = app.document_->begin_edit(component, path, session);
        if (!began) return began;
        auto preview = app.document_->preview_edit(session, value);
        if (!preview) return preview;
        return app.document_->commit_edit(session);
    }
};
}


namespace {
using atelier::application::Application;
using atelier::ui::ControlKey;
namespace control = atelier::editor::control;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void hr(HRESULT result) { if (FAILED(result)) throw std::runtime_error("PNG encoding failed."); }
struct KeyStateScope {
    BYTE original[256]{};
    explicit KeyStateScope(bool control = false, bool shift = false, bool alt = false) {
        require(GetKeyboardState(original) != FALSE, "Cannot preserve acceptance thread key state.");
        BYTE state[256]{}; std::copy(std::begin(original), std::end(original), state);
        for (const auto code : {VK_CONTROL, VK_LCONTROL, VK_RCONTROL, VK_SHIFT, VK_LSHIFT, VK_RSHIFT, VK_MENU, VK_LMENU, VK_RMENU})
            state[code] = 0;
        state[VK_CONTROL] = state[VK_LCONTROL] = control ? 0x80 : 0;
        state[VK_SHIFT] = state[VK_LSHIFT] = shift ? 0x80 : 0;
        state[VK_MENU] = state[VK_LMENU] = alt ? 0x80 : 0;
        require(SetKeyboardState(state) != FALSE, "Cannot set acceptance thread modifiers.");
    }
    ~KeyStateScope() { SetKeyboardState(original); }
};
void require_input_focus(Application& app) {
    require(GetFocus() == app.window() && GetForegroundWindow() == app.window(),
        "Acceptance interrupted by external focus loss; no further synthetic input was sent.");
}
void tick(Application& app, unsigned count = 2) {
    const auto before = app.accepted_frames();
    for (unsigned attempt = 0; app.accepted_frames() < before + count && attempt < 180; ++attempt)
        require(app.step(), "Editor closed during input acceptance.");
    if (app.accepted_frames()<before+count) atelier::application::EditorAcceptanceAccess::frame_failure(app);
    require(app.accepted_frames() >= before + count, "Editor did not accept a frame within 180 iterations.");
}
POINT centre(Application& app, ControlKey key) {
    require(app.presented() != nullptr, "Missing presented UI frame.");
    auto* box = app.presented()->find(key);
    const bool popup=!box && app.menu_frame();
    if (popup) box=app.menu_frame()->find(key);
    if (!box) {
        std::string menu;
        if (app.menu_frame()) for (const auto& item:app.menu_frame()->controls) menu+=" "+std::to_string(item.key);
        POINT pointer{}; GetCursorPos(&pointer); ScreenToClient(app.window(),&pointer);
        throw std::runtime_error("Missing control "+std::to_string(key)+" in presented main/menu frames; popup keys:"+menu+
            "; OS pointer="+std::to_string(pointer.x)+","+std::to_string(pointer.y));
    }
    const auto left = std::max(box->bounds.x, static_cast<float>(box->clip.left));
    const auto right = std::min(box->bounds.x + box->bounds.width, static_cast<float>(box->clip.right));
    const auto top = std::max(box->bounds.y, static_cast<float>(box->clip.top));
    const auto bottom = std::min(box->bounds.y + box->bounds.height, static_cast<float>(box->clip.bottom));
    if (!(right>left && bottom>top)) throw std::runtime_error("Control "+std::to_string(key)+
        " has no visible hit area (y="+std::to_string(box->bounds.y)+", height="+std::to_string(box->bounds.height)+
        ", clip="+std::to_string(box->clip.top)+".."+std::to_string(box->clip.bottom)+").");
    POINT result{static_cast<LONG>((left + right) * .5f), static_cast<LONG>((top + bottom) * .5f)};
    if (popup) MapWindowPoints(app.menu_window(),app.window(),&result,1);
    return result;
}
void mouse(Application& app, UINT message, POINT point) {
    require_input_focus(app);
    KeyStateScope modifiers;
    // Unlike atomic click_at(), this helper deliberately holds gestures across
    // rendered frames. Capture-generated native movement must start at the same point.
    if (message==WM_LBUTTONDOWN) {
        auto native_point=point; ClientToScreen(app.window(),&native_point);
        require(SetCursorPos(native_point.x,native_point.y)!=FALSE,"Cannot align the held gesture pointer.");
        tick(app,2);
    }
    SendMessageW(app.window(), message, message == WM_LBUTTONUP ? 0 : MK_LBUTTON, MAKELPARAM(point.x, point.y));
    tick(app, 1);
    if (message==WM_LBUTTONUP) {
        RECT host{}; GetWindowRect(app.window(),&host);
        require(SetCursorPos(host.left+32,host.top+12)!=FALSE,"Cannot park the pointer after the held gesture.");
        tick(app,1);
    }
}
void click_at(Application& app, POINT point) {
    require_input_focus(app);
    KeyStateScope modifiers;
    // Deliver a complete synthetic click before polling queued native movement.
    // Deliberate drags and pressed-state captures still use mouse() with ticks.
    SendMessageW(app.window(),WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(point.x,point.y));
    SendMessageW(app.window(),WM_LBUTTONUP,0,MAKELPARAM(point.x,point.y));
    tick(app,2);
}
void click(Application& app, ControlKey key) {
    if (!app.presented()->find(key) && (!app.menu_frame() || !app.menu_frame()->find(key)))
        if (const auto menu=control::command_menu(key)) click(app,menu);
    // Docked views can be shorter than their content. Reach the target through
    // actual wheel messages, using the same accepted clip as a user would see.
    for (unsigned attempt=0;attempt<40;++attempt) {
        const auto* box=app.presented()->find(key); if (!box) break;
        if (box->bounds.y>=box->clip.top && box->bounds.y+box->bounds.height<=box->clip.bottom) break;
        auto parent=box->parent;
        while (parent) {
            const auto* ancestor=app.presented()->find(parent); if (!ancestor) { parent=0; break; }
            if (ancestor->kind==atelier::ui::ControlKind::list || ancestor->kind==atelier::ui::ControlKind::tree) break;
            parent=ancestor->parent;
        }
        if (!parent) break;
        POINT point=centre(app,parent); ClientToScreen(app.window(),&point);
        const auto delta=box->bounds.y<box->clip.top?WHEEL_DELTA:-WHEEL_DELTA;
        SendMessageW(app.window(),WM_MOUSEWHEEL,MAKEWPARAM(0,static_cast<WORD>(delta)),MAKELPARAM(point.x,point.y));
        tick(app,1);
    }
    const auto point = centre(app, key);
    mouse(app, WM_MOUSEMOVE, point); click_at(app, point);
}
void key(Application& app, WPARAM code, bool ctrl = false, bool shift = false) {
    require_input_focus(app);
    KeyStateScope modifiers(ctrl, shift);
    SendMessageW(app.window(), WM_KEYDOWN, code, 0);
    tick(app, 1);
    SendMessageW(app.window(), WM_KEYUP, code, 0);
    tick(app, 1);
}
void type(Application& app, const wchar_t* value) {
    KeyStateScope modifiers;
    key(app, 'A', true);
    if (!*value) { key(app, VK_DELETE); return; }
    require_input_focus(app);
    while (*value) SendMessageW(app.window(), WM_CHAR, static_cast<WPARAM>(*value++), 0);
    tick(app, 1);
}
template<class T> T model(atelier::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    if constexpr (!std::is_void_v<T>) return std::move(*result);
}
std::span<const atelier::world::Member> actors(const Application& app) { return model(app.document().world().members()); }
bool contains(const Application& app, atelier::ObjectId actor) {
    const auto members = actors(app);
    return std::any_of(members.begin(), members.end(), [actor](const auto& member) { return member.id() == actor; });
}
atelier::ObjectId component(const Application& app, atelier::ObjectId actor, atelier::TypeId type) {
    auto& runtime = app.document().world().runtime();
    for (const auto& root : model(atelier::world::components(runtime, actor)))
        if (model(runtime.is_a(model(runtime.type_of(root.id())), type))) return root.id();
    throw std::runtime_error("Required component is absent.");
}
ControlKey property_control(const Application& app, atelier::ObjectId actor, std::size_t axis) {
    if (actor == atelier::ObjectId{}) return control::empty_field_base + axis;
    const auto target = model(atelier::world::root_component(app.document().world().runtime(), actor));
    if (!target) return control::empty_field_base + axis;
    const auto key = app.field_control(*target, app.document().parameters()[axis].path);
    require(key != 0, "Missing live component-field control binding.");
    return key;
}
float x(const Application& app, atelier::ObjectId actor) {
    const auto target = model(atelier::world::root_component(app.document().world().runtime(), actor));
    require(target.has_value(), "Missing SceneComponent root in current World.");
    return std::get<float>(model(app.document().world().read(*target, app.document().parameters()[0].path)));
}
bool intersects(const atelier::ui::Rect& a, const atelier::ui::Rect& b) {
    return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
}
std::shared_ptr<const atelier::ui::Coverage> text_coverage(const atelier::ui::UiFrame& frame, ControlKey key) {
    const auto* box = frame.find(key);
    require(box != nullptr, "Text control disappeared from the frame.");
    for (const auto& draw : frame.drawing->draws) {
        if (draw.coverage && intersects(draw.bounds, box->bounds) &&
            draw.clip.left < box->bounds.x + box->bounds.width && draw.clip.right > box->bounds.x &&
            draw.clip.top < box->bounds.y + box->bounds.height && draw.clip.bottom > box->bounds.y)
            return draw.coverage;
    }
    return {};
}
std::vector<atelier::ui::Color> solid_colors(const atelier::ui::UiFrame& frame, ControlKey key) {
    const auto* box = frame.find(key);
    require(box != nullptr, "Paint control disappeared from the frame.");
    std::vector<atelier::ui::Color> colors;
    for (const auto& draw : frame.drawing->draws)
        if (!draw.coverage && intersects(draw.bounds, box->bounds)) {
            colors.push_back(draw.color);
            if (draw.border_width>0) colors.push_back(draw.border_color);
        }
    return colors;
}
void wheel(Application& app, ControlKey key, short notches) {
    auto point = centre(app, key);
    ClientToScreen(app.window(), &point);
    SendMessageW(app.window(), WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(notches * WHEEL_DELTA)), MAKELPARAM(point.x, point.y));
    tick(app, 1);
}
void png(const atelier::renderer::Capture& image, const std::filesystem::path& path) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    hr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
    ComPtr<IWICStream> stream; hr(factory->CreateStream(&stream));
    hr(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
    ComPtr<IWICBitmapEncoder> encoder; hr(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder));
    hr(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame; hr(encoder->CreateNewFrame(&frame, nullptr)); hr(frame->Initialize(nullptr));
    hr(frame->SetSize(image.extent.width, image.extent.height));
    auto format = GUID_WICPixelFormat32bppBGRA; hr(frame->SetPixelFormat(&format));
    require(format == GUID_WICPixelFormat32bppBGRA, "PNG codec changed pixel format.");
    auto bgra = image.rgba;
    for (std::size_t pixel = 0; pixel < bgra.size(); pixel += 4) std::swap(bgra[pixel], bgra[pixel + 2]);
    hr(frame->WritePixels(image.extent.height, image.extent.width * 4, static_cast<UINT>(bgra.size()), bgra.data()));
    hr(frame->Commit()); hr(encoder->Commit());
}
void capture(Application& app, const std::filesystem::path& path) {
    atelier::renderer::Capture image;
    require(app.step(&image) && image.frame_id != 0, "Renderer did not capture an accepted frame.");
    require(image.rgba.size() == static_cast<std::size_t>(image.extent.width) * image.extent.height * 4,
        "Capture has an invalid pixel payload.");
    const auto scene_pixels = std::count_if(image.depth.begin(), image.depth.end(), [](float value) { return value > 0 && value < .999999f; });
    if (app.presented()->find(control::viewport)) require(scene_pixels>32,"Real scene geometry is absent from the depth capture.");
    else require(scene_pixels==0,"Hidden Scene tab continued rendering geometry.");
    png(image, path);
    if (app.menu_window()) {
        atelier::renderer::Capture popup; app.capture_menu(popup);
        png(popup,path.parent_path()/(path.stem().wstring()+L"-popup.png"));
    }
}
void verify_chrome_pixels(Application& app,const std::filesystem::path& output) {
    atelier::renderer::Capture pixels;
    require(app.step(&pixels) && pixels.frame_id,"Chrome pixel capture failed.");
    const auto pixel=[&](int x,int y,int gray) {
        require(x>=0 && y>=0 && x<static_cast<int>(pixels.extent.width) && y<static_cast<int>(pixels.extent.height),"Chrome probe left the surface.");
        const auto offset=(static_cast<std::size_t>(y)*pixels.extent.width+static_cast<std::size_t>(x))*4;
        for (std::size_t channel=0;channel<3;++channel)
            require(std::abs(static_cast<int>(pixels.rgba[offset+channel])-gray)<=1,"Chrome edge did not match the reference sRGB value.");
    };
    const auto field=app.presented()->find(control::parameter_search)->bounds;
    for (const auto& control:app.presented()->controls)
        if (control.kind==atelier::ui::ControlKind::number || control.kind==atelier::ui::ControlKind::text_edit)
            require(std::abs(control.bounds.height-field.height)<.1f,"Single-line editors have inconsistent heights.");
    const auto x=static_cast<int>(std::round(field.x)),y=static_cast<int>(std::round(field.y));
    // Current display: a sub-DIP authored edge rounds to one physical pixel.
    const auto stroke=std::max(1,static_cast<int>(std::round(.667f*app.presented()->drawing->dpi_scale)));
    pixel(x,y+4*stroke,36); pixel(x+stroke,y+4*stroke,46); pixel(x+3*stroke,y+4*stroke,19);
    pixel(x+8*stroke,y,36); pixel(x+8*stroke,y+stroke,46);
    const auto selected=app.presented()->find(control::parameter_transform)->bounds;
    const auto tx=static_cast<int>(std::round(selected.x)),ty=static_cast<int>(std::round(selected.y));
    pixel(tx+5*stroke,ty,38); pixel(tx+5*stroke,ty+2*stroke,79);
    pixel(tx+5*stroke,static_cast<int>(std::round(selected.y+selected.height))-1,79);
    const auto tab_bar=app.presented()->find(control::parameter_pages)->bounds;
    const auto list=app.presented()->find(control::parameter_list)->bounds;
    const auto fold=app.presented()->find(control::position_fold)->bounds;
    require(std::abs(selected.x-tab_bar.x)<.1f && std::abs(fold.x-list.x)<.1f,
        "Parameter tabs or fold header added an independent left inset.");
    pixel(static_cast<int>(std::round(fold.x))+stroke,static_cast<int>(std::round(fold.y))+stroke,51);
    for (const auto key:{control::scene_tab,control::parameters_tab}) {
        const auto* tab=app.presented()->find(key);
        const auto group=app.presented()->find(tab->parent)->bounds;
        // The first tab continues the selected face after the rail's trailing edge.
        pixel(static_cast<int>(std::round(group.x)),static_cast<int>(std::round(group.y))+3*stroke,73);
    }
    for (const auto axis:{atelier::ui::DragAxis::horizontal,atelier::ui::DragAxis::vertical}) {
        const bool vertical=axis==atelier::ui::DragAxis::horizontal;
        const auto divider=app.docking().divider(vertical?control::scene_tab:control::parameters_tab,axis);
        const auto rail=app.presented()->find(divider)->bounds;
        const auto rx=static_cast<int>(std::round(rail.x)),ry=static_cast<int>(std::round(rail.y));
        if (vertical) {
            pixel(rx,ry+8*stroke,0); pixel(rx+2*stroke,ry+8*stroke,73);
            pixel(static_cast<int>(std::round(rail.x+rail.width))-1,ry+8*stroke,51);
        } else {
            pixel(rx+8*stroke,ry,0); pixel(rx+8*stroke,ry+2*stroke,73);
            pixel(rx+8*stroke,static_cast<int>(std::round(rail.y+rail.height))-1,51);
        }
    }
    png(pixels,output/"chrome-pixels.png");
}
void verify_control_states(Application& app, const std::filesystem::path& output, atelier::ObjectId object) {
    const auto paint=[&] {
        const auto bounds=app.presented()->find(control::file_menu)->bounds;
        std::shared_ptr<const atelier::ui::ColorImage> image;
        for (const auto& draw:app.presented()->drawing->draws) if (draw.bounds==bounds && draw.image) image=draw.image;
        return std::pair{solid_colors(*app.presented(),control::file_menu),image};
    };
    mouse(app, WM_MOUSEMOVE, centre(app, control::viewport));
    const auto normal = paint();
    const auto button = centre(app, control::file_menu);
    mouse(app, WM_MOUSEMOVE, button);
    const auto hover = paint();
    require(hover==normal && !app.menu_window(),"Closed menubar reacted to pointer hover.");
    mouse(app, WM_LBUTTONDOWN, button);
    require(paint() != hover, "Pressed state did not change the shared paint.");
    capture(app, output / "s1-pressed.png");
    mouse(app, WM_LBUTTONUP, button);
    key(app,VK_ESCAPE);
    require(app.presented()->find(control::file_menu)->style->border_width==0,
        "Menu focus added an outer frame after pointer activation.");

    auto field = property_control(app, object, 0);
    click(app, field);
    const auto focused = solid_colors(*app.presented(), field);
    const auto edge=app.presented()->find(field)->style->border;
    require(edge.r==edge.g && edge.g==edge.b,"Editing focus did not use a neutral field edge.");
    capture(app, output / "s1-focused.png");
    type(app, L"10001");
    require(solid_colors(*app.presented(), field) != focused, "Invalid draft did not change the shared field state.");
    capture(app, output / "s1-invalid.png");
    key(app, VK_ESCAPE);

    const auto before = x(app, object);
    click(app, control::remove_actor);
    const auto* disabled = app.presented()->find(property_control(app, {}, 0));
    require(disabled && !disabled->enabled && text_coverage(*app.presented(), disabled->key),
        "Unselected parameter state did not retain disabled text and geometry.");
    capture(app, output / "s1-disabled.png");
    click(app, control::undo);
    require(app.document().selected() == object && x(app, object) == before, "State verification did not restore the edited object.");
    const auto* selected = app.presented()->find(app.object_control(object));
    require(selected && selected->enabled, "Selected object row lost its enabled control geometry.");
}
void verify_style_changes(Application& app, const std::filesystem::path& output, atelier::ObjectId object) {
    const auto initial_style = app.style();
    auto field = property_control(app, object, 0);
    click(app, field);
    key(app, VK_END);
    const auto original = app.presented();
    const auto original_coverage = text_coverage(*original, field);
    require(original_coverage != nullptr, "Focused numeric field has no real text coverage.");
    const auto original_pixels = original_coverage->pixels;
    const auto original_bounds = original->find(field)->bounds;
    const auto original_provider = original->measure_provider_revision;
    const auto caret = [&](const atelier::ui::UiFrame& frame) {
        const auto* box = frame.find(field);
        for (const auto& draw : frame.drawing->draws)
            if (!draw.coverage && draw.color == app.style().palette.text &&
                draw.bounds.width <= app.style().caret_width * frame.drawing->dpi_scale + .01f &&
                draw.bounds.height > 5 && intersects(draw.bounds, box->bounds)) return draw.bounds;
        throw std::runtime_error("Focused field has no caret using the shared text style.");
    };
    const auto original_caret = caret(*original);
    auto colors = initial_style;
    colors.palette.text = {.64f, .88f, .72f, 1};
    colors.palette.panel = {.04f, .052f, .048f, 1};
    const auto before_color = app.ui_statistics();
    require(app.set_style(colors).has_value() && app.presented() == original,
        "Setting a style changed the already accepted frame.");
    tick(app, 1);
    require(text_coverage(*app.presented(), field) == original_coverage &&
        app.ui_statistics().rasterizations == before_color.rasterizations &&
        app.frame_statistics().coverage_upload_count == 0 &&
        app.presented()->measure_provider_revision == original_provider,
        "Color-only style change invalidated text coverage or metrics.");
    capture(app, output / "s1-color.png");

    const auto colored_frame = app.presented();
    auto invalid = colors;
    invalid.font.size = -1;
    invalid.palette.panel = {.4f, .1f, .1f, 1};
    const auto rejected = app.set_style(invalid);
    require(!rejected && rejected.error().code == atelier::ui::ErrorCode::invalid_style &&
        app.style() == colors && app.presented() == colored_frame, "Invalid style partially modified the theme or accepted frame.");

    auto font = colors;
    font.font.family = L"Segoe UI";
    font.font.size = 16;
    font.font.weight = 500;
    font.title_weight = 700;
    require(app.set_style(font).has_value(), "Unified font change was rejected.");
    tick(app, 1);
    const auto current_coverage = text_coverage(*app.presented(), field);
    const auto current_caret = caret(*app.presented());
    require(current_coverage && current_coverage != original_coverage &&
        app.presented()->measure_provider_revision > original_provider &&
        current_caret.x != original_caret.x && current_caret.height != original_caret.height,
        "Font identity did not invalidate text layout, coverage and caret together.");
    require(current_caret.x >= original_bounds.x && current_caret.x <= original_bounds.x + original_bounds.width &&
        original->find(field)->bounds == original_bounds && original_coverage->pixels == original_pixels,
        "Font update changed old-frame geometry/content or misplaced the new caret.");
    for (const auto& group : atelier::editor::command_groups) {
        const auto command=group.key;
        const auto* box = app.presented()->find(command);
        const auto coverage = text_coverage(*app.presented(), command);
        require(box && coverage && box->bounds.x + box->bounds.width <= app.presented()->drawing->extent.width,
            "Unified font change pushed a menubar title outside the window.");
        for (const auto& draw : app.presented()->drawing->draws)
            if (draw.coverage == coverage) require(draw.bounds.x >= box->bounds.x &&
                draw.bounds.x + draw.bounds.width <= box->bounds.x + box->bounds.width,
                "Measured menubar width clipped the changed font's glyphs.");
    }
    capture(app, output / "s1-font.png");
    key(app, VK_ESCAPE);
    require(app.set_style(initial_style).has_value(), "Could not restore the editor style.");
    tick(app, 1);
}
struct TimingSeries {
    std::vector<double> prepare, render;
    atelier::renderer::FrameStats total;
    std::uint64_t peak_live_coverages{}, peak_live_bytes{};
    atelier::ui::Statistics start, finish;
    explicit TimingSeries(const Application& app) : start(app.ui_statistics()) {}
    void record(const Application& app) {
        const auto& sample = app.frame_timing();
        require(sample.frame_id == app.presented()->drawing->id, "Timing did not describe the accepted frame.");
        prepare.push_back(sample.prepare_us);
        render.push_back(sample.render_us);
        const auto& stats = app.frame_statistics();
        total.ui_draw_calls += stats.ui_draw_calls;
        total.scene_draw_calls += stats.scene_draw_calls;
        total.coverage_upload_count += stats.coverage_upload_count;
        total.coverage_upload_bytes += stats.coverage_upload_bytes;
        total.cached_coverage_count = std::max(total.cached_coverage_count, stats.cached_coverage_count);
        total.cached_coverage_texel_bytes = std::max(total.cached_coverage_texel_bytes, stats.cached_coverage_texel_bytes);
        total.cached_coverage_cpu_bytes = std::max(total.cached_coverage_cpu_bytes, stats.cached_coverage_cpu_bytes);
        finish = app.ui_statistics();
        peak_live_coverages = std::max(peak_live_coverages, finish.live_coverages);
        peak_live_bytes = std::max(peak_live_bytes, finish.live_coverage_bytes);
    }
    void write(std::ostream& report, const char* name) const {
        const auto distribution = [&](const char* stage, std::vector<double> values) {
            require(!values.empty(), "No timing samples were collected.");
            std::sort(values.begin(), values.end());
            const auto p95 = (values.size() * 95 + 99) / 100 - 1;
            report << name << ',' << stage << ',' << values.size() << ','
                   << std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size()) << ','
                   << values[values.size() / 2] << ',' << values[p95] << ',' << values.back() << '\n';
        };
        distribution("prepare_cpu_us", prepare);
        distribution("render_cpu_us", render);
    }
    void resources(std::ostream& report, const char* name) const {
        report << name << ',' << prepare.size() << ',' << finish.prepares - start.prepares << ','
               << finish.rasterizations - start.rasterizations << ',' << finish.raster_bytes - start.raster_bytes << ','
               << finish.culled_text_controls - start.culled_text_controls << ','
               << total.ui_draw_calls << ',' << total.scene_draw_calls << ',' << total.coverage_upload_count << ','
               << total.coverage_upload_bytes << ',' << peak_live_coverages << ',' << peak_live_bytes << ','
               << total.cached_coverage_count << ',' << total.cached_coverage_texel_bytes << ',' << total.cached_coverage_cpu_bytes << '\n';
    }
};
void verify_shared_text(Application& app, const std::filesystem::path& output, atelier::ObjectId object) {
    auto field = property_control(app, object, 0);
    const auto original_position = x(app, object);
    const auto original_history = app.document().undo_count();
    click(app, control::parameter_search);
    type(app, L"位置");
    require(app.query() == L"位置" && app.presented()->find(field), "Owned Unicode search did not match real parameter aliases.");
    capture(app, output / "s2-search-unicode.png"); // WM_CHAR coverage, not a claim of actual IME input.
    key(app, VK_RETURN);
    require(app.query() == L"位置" && app.wants_keyboard(), "Search Enter did not commit while retaining focus.");
    type(app, L"x");
    require(app.presented()->find(field) && !app.presented()->find(property_control(app, object, 1)), "Search did not filter actual parameter rows.");
    key(app, 'Z', true);
    require(app.query() == L"位置", "Local undo did not restore the committed search.");
    key(app, 'Y', true);
    require(app.query() == L"x", "Local redo did not restore the search draft.");
    key(app, VK_ESCAPE);
    require(app.query() == L"位置", "Search cancellation did not restore the transaction origin.");
    type(app, L"z"); click(app, control::reset);
    require(app.query() == L"z", "Ordinary blur did not commit search preview.");
    click(app, control::parameter_search); type(app, L"x");
    SetFocus(nullptr); tick(app, 1); SetFocus(app.window()); tick(app, 1);
    require(app.query() == L"z", "OS blur did not roll search preview back.");
    click(app, control::parameter_search); type(app, L""); key(app, VK_RETURN);
    require(app.query().empty() && app.presented()->find(property_control(app, object, 2)), "Empty search did not restore all parameter rows.");

    constexpr auto graphemes = L"a\U0001F600e\u0301\U0001F469\u200d\U0001F4BB";
    type(app, graphemes); key(app, VK_END);
    key(app, VK_BACK);
    require(app.query() == L"a\U0001F600e\u0301", "Backspace split a ZWJ grapheme.");
    key(app, VK_BACK);
    require(app.query() == L"a\U0001F600", "Backspace split a combining sequence.");
    key(app, VK_BACK);
    require(app.query() == L"a", "Backspace split a surrogate pair.");
    key(app, 'Z', true);
    require(app.query() == graphemes, "Contiguous backspace was not one local history group.");
    key(app, 'A', true); key(app, 'C', true);
    {
        const auto holder = CreateWindowExW(0, L"STATIC", L"Atelier clipboard acceptance", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        require(holder != nullptr, "Cannot create a clipboard contention holder.");
        struct Holder { HWND window; ~Holder() { CloseClipboard(); DestroyWindow(window); } } held{holder};
        require(OpenClipboard(holder) != FALSE, "Cannot establish clipboard contention.");
        key(app, 'V', true);
        require(app.query() == graphemes && app.status_text().find(L"Input operation failed") != std::wstring::npos,
            "Failed native clipboard read changed the draft or lost its error.");
    }
    key(app, 'X', true);
    require(app.query().empty(), "Successful native clipboard cut did not remove its selection.");
    key(app, 'V', true);
    require(app.query() == graphemes, "Native Unicode clipboard paste did not restore the exact text.");
    { KeyStateScope unrelated_shift(false, true); key(app, 'Z', true); }
    require(app.query().empty(), "Paste undo or explicit modifier normalization failed.");
    key(app, 'Z', true); require(app.query() == graphemes, "Cut was not one local undo unit.");
    key(app, VK_ESCAPE);
    require(app.query().empty() && app.document().undo_count() == original_history, "Search history escaped into scene history.");
    type(app, L"ab אבג xy"); key(app, 'A', true);
    const auto colors = solid_colors(*app.presented(), control::parameter_search);
    require(std::count(colors.begin(), colors.end(), app.presented()->find(control::parameter_search)->style->selection) > 1,
        "Bidirectional selection did not produce multiple visual ranges.");
    capture(app, output / "s2-bidi-selection.png");
    key(app, VK_ESCAPE);

    const auto field_point=centre(app,field);
    mouse(app,WM_MOUSEMOVE,field_point);
    require(PostMessageW(app.window(),WM_MOUSEMOVE,0,MAKELPARAM(field_point.x+8,field_point.y))!=FALSE,
        "Could not enqueue native mouse movement for click acceptance.");
    click_at(app,field_point);
    require(x(app,object)==original_position && app.document().undo_count()==original_history && !app.document().editing(),
        "Queued native movement turned a synthetic click into a committed drag.");
    type(app, L"42");
    require(x(app, object) == 42 && app.document().undo_count() == original_history, "Numeric draft did not preview without business history.");
    key(app, 'Z', true);
    require(x(app, object) == original_position, "Numeric local undo did not restore the original preview.");
    key(app, 'Z', true);
    require(x(app, object) == original_position && app.document().undo_count() == original_history, "Exhausted local undo fell through to business history.");
    key(app, 'Y', true);
    require(x(app, object) == 42, "Local redo did not remain owned after undo returned to the origin.");
    key(app, VK_RETURN);
    require(app.wants_keyboard() && x(app, object) == 42, "Commit did not retain passive field focus.");
    key(app, 'Z', true); require(x(app, object) == original_position, "Passive-focus undo did not reach scene history.");
    key(app, 'Z', true, true); require(x(app, object) == 42, "Ctrl+Shift+Z did not redo scene history.");
    key(app, 'Z', true); require(x(app, object) == original_position, "Text acceptance did not restore the scene value.");
    click(app, field); key(app, VK_END);
    auto start = centre(app, field); start.x = static_cast<LONG>(app.presented()->find(field)->bounds.x + 9 * app.presented()->drawing->dpi_scale);
    mouse(app, WM_LBUTTONDOWN, start); start.x += 24; mouse(app, WM_MOUSEMOVE, start); mouse(app, WM_LBUTTONUP, start);
    require(x(app, object) == original_position && !app.document().editing(), "Text-mode mouse selection started numeric dragging.");
    capture(app, output / "s2-number-selection.png");
    key(app, VK_ESCAPE);
    const auto failure_history = app.document().undo_count();
    require(atelier::application::EditorAcceptanceAccess::begin_edit(app, field).has_value(), "Could not seed an active backend transaction.");
    auto drag = centre(app, field);
    mouse(app, WM_LBUTTONDOWN, drag); drag.x += 30; mouse(app, WM_MOUSEMOVE, drag);
    require(!app.document().editing() && x(app, object) == original_position && GetCapture() != app.window() &&
        app.document().undo_count() == failure_history && app.status_text().find(L"already active") != std::wstring::npos,
        "Failed edit_begin did not stop preview, roll back, release capture and preserve the first error.");
    capture(app, output / "s2-failed-edit.png");
    mouse(app, WM_LBUTTONUP, drag);
    click(app, control::reset);
}
void verify_transform_projection(Application& app, const std::filesystem::path& output, atelier::ObjectId actor) {
    const auto transform = *model(atelier::world::root_component(app.document().world().runtime(), actor));
    const auto before = model(atelier::world::read_transform(app.document().world().runtime(), transform));
    atelier::renderer::Capture original, rotated, scaled, restored;
    require(app.step(&original), "Could not capture the original World transform.");
    require(atelier::application::EditorAcceptanceAccess::edit_field(app, transform,
        app.document().types().rotation_fields[2], .7f).has_value(), "The real Session rotation transaction failed.");
    tick(app);
    require(app.step(&rotated) && rotated.depth != original.depth, "World rotation did not reach actual GPU geometry.");
    capture(app, output / "world-trs-rotated.png");
    require(atelier::application::EditorAcceptanceAccess::edit_field(app, transform,
        app.document().types().scale_fields[0], 1.75f).has_value(), "The real Session scale transaction failed.");
    tick(app);
    require(app.step(&scaled) && scaled.depth != rotated.depth, "World nonuniform scale did not reach actual GPU geometry.");
    capture(app, output / "world-trs-scaled.png");
    click(app, control::undo); click(app, control::undo);
    require(model(atelier::world::read_transform(app.document().world().runtime(), transform)) == before &&
        app.step(&restored) && restored.depth == original.depth, "TRS history did not restore the original value and GPU projection.");
}
void verify_list_workload(Application& app, const std::filesystem::path& output) {
    // Populate through the real Create menu and its native popup surface.
    while (actors(app).size() + 1 < atelier::editor::Session::maximum_actors) {
        const auto before_count = actors(app).size();
        click(app,control::add_actor);
        require(actors(app).size() == before_count + 1, "Create menu failed to populate the real list workload.");
    }
    click(app, control::add_static_mesh);
    const auto first = actors(app).front().id();
    const auto last = actors(app).back().id();
    require(app.presented()->find(app.object_control(last)) &&
        !text_coverage(*app.presented(), app.object_control(last)),
        "Offscreen list row lost geometry or emitted text before scrolling.");
    wheel(app, 23, -240);
    require(text_coverage(*app.presented(), app.object_control(last)) &&
        !text_coverage(*app.presented(), app.object_control(first)), "Scrolling did not exchange visible list text.");
    capture(app, output / "s1-list-256.png");
    wheel(app, 23, 240);
    require(text_coverage(*app.presented(), app.object_control(first)) != nullptr,
        "Scrolling back did not regenerate visible list text.");

    const auto bar=[&]() {
        const auto& bars=app.presented()->scrollbars;
        const auto found=std::find_if(bars.begin(),bars.end(),[](const auto& value){return value.key==23;});
        require(found!=bars.end(),"Overflowing Actor list has no accepted scrollbar geometry.");
        return *found;
    };
    const auto middle=[](atelier::ui::Rect box) { return POINT{static_cast<LONG>(box.x+box.width*.5f),static_cast<LONG>(box.y+box.height*.5f)}; };
    click_at(app,middle(bar().increment));
    require(bar().value>0,"Scrollbar step button did not scroll.");
    click_at(app,middle(bar().decrement));
    require(bar().value==0,"Scrollbar reverse step did not restore the start.");
    auto page_point=middle(bar().track);
    click_at(app,page_point);
    require(bar().value>=bar().page-1,"Scrollbar track click did not advance a page.");
    const auto history_before_scroll=app.document().undo_count();
    const auto from=middle(bar().thumb);
    auto to=from; to.y=static_cast<LONG>(bar().track.y+bar().track.height+80);
    {
        require_input_focus(app); KeyStateScope modifiers;
        SendMessageW(app.window(),WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(from.x,from.y));
        SendMessageW(app.window(),WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(to.x,to.y));
        SendMessageW(app.window(),WM_LBUTTONUP,0,MAKELPARAM(to.x,to.y)); tick(app,2);
    }
    require(bar().value==bar().maximum && text_coverage(*app.presented(),app.object_control(last)),"Scrollbar thumb did not reach the last Actor.");
    mouse(app,WM_LBUTTONDOWN,middle(bar().thumb));
    key(app,VK_ESCAPE);
    require(GetCapture()!=app.window() && app.document().undo_count()==history_before_scroll,"Scrollbar cancellation changed document history or retained capture.");
    require(app.presented()->find(app.object_control(last))->clip.right<=bar().bounds.x,"Actor rows overlapped the scrollbar gutter.");
    capture(app,output/"scrollbar-thumb-end.png");
    wheel(app,23,240);

    TimingSeries scroll(app);
    app.sample_frames(true);
    for (unsigned frame = 0; frame < 48; ++frame) {
        wheel(app, 23, frame < 24 ? -8 : 8);
        scroll.record(app);
    }
    const auto field = property_control(app, last, 0);
    const auto before = x(app, last);
    const auto history = app.document().undo_count();
    auto point = centre(app, field);
    mouse(app, WM_LBUTTONDOWN, point);
    TimingSeries editing(app);
    for (unsigned frame = 0; frame < 64; ++frame) {
        point.x += frame < 32 ? 2 : -2;
        mouse(app, WM_MOUSEMOVE, point);
        editing.record(app);
    }
    point.x += 12;
    mouse(app, WM_MOUSEMOVE, point);
    mouse(app, WM_LBUTTONUP, point);
    require(x(app, last) != before && app.document().undo_count() == std::min(history + 1, std::size_t{128}),
        "Dense-scene drag did not keep the existing position/history behavior.");
    app.sample_frames(false);
    const auto after = x(app, last);
    // A committed drag leaves passive focus, so document history owns Z/Y.
    key(app, 'Z', true);
    require(x(app, last) == before, "One undo did not restore the dense-scene drag.");
    key(app, 'Y', true);
    require(x(app, last) == after, "Redo did not restore the dense-scene drag.");
    std::ofstream report(output / "s1-performance.csv");
    report << "workload,stage,samples,mean,p50,p95,max\n";
    scroll.write(report, "256_actors_scroll");
    editing.write(report, "256_actors_field_drag");
    require(report.good(), "Cannot save window sampling evidence.");
    std::ofstream resources(output / "s1-resources.csv");
    resources << "workload,accepted_samples,prepare_calls,rasterizations,raster_bytes,culled_text_controls,ui_draw_calls,scene_draw_calls,coverage_upload_count,coverage_payload_bytes,peak_live_cpu_coverages,peak_live_cpu_bytes,peak_renderer_cache_count,peak_renderer_cache_texel_bytes,peak_renderer_cache_cpu_bytes\n";
    scroll.resources(resources, "256_actors_scroll");
    editing.resources(resources, "256_actors_field_drag");
    require(resources.good(), "Cannot save resource sampling evidence.");
}
#include "controls.inl"
#include "docking.inl"
#include "shelf.inl"
RECT await_acceptance_activation(Application& app,const std::filesystem::path& output,const char* kind) {
    const auto label=std::string("Editor ")+kind+" acceptance";
    std::ofstream(output/"acceptance.txt")<<label<<": WAITING FOR ACTIVATION (no synthetic input)\n";
    std::printf("%s: waiting up to 120 seconds; click the editor title bar to begin. No synthetic input has started.\n",label.c_str());
    std::fflush(stdout);
    SetForegroundWindow(app.window());SetFocus(app.window());tick(app);
    const auto deadline=GetTickCount64()+120000;
    while ((GetForegroundWindow()!=app.window() || GetFocus()!=app.window()) && GetTickCount64()<deadline)
        require(app.step(),"Editor closed while waiting for initial foreground.");
    if (GetFocus()!=app.window() || GetForegroundWindow()!=app.window()) {
        const auto foreground=GetForegroundWindow();
        DWORD process{};GetWindowThreadProcessId(foreground,&process);
        std::ofstream(output/"acceptance.txt")<<label<<": BLOCKED before input; initial activation timed out\n"
            <<"editor="<<app.window()<<"; foreground="<<foreground<<"; focus="<<GetFocus()<<"; foreground PID="<<process<<'\n';
        throw std::runtime_error("Initial foreground activation was not granted within 120 seconds; no synthetic input was sent.");
    }
    std::ofstream(output/"acceptance.txt")<<label<<": RUNNING (activation acquired)\n";
    std::printf("%s: activation acquired; starting input checks.\n",label.c_str());std::fflush(stdout);
    // Keep native pointer movement away from menu headings during message-driven gestures.
    RECT host{};GetWindowRect(app.window(),&host);
    require(SetCursorPos(host.left+32,host.top+12)!=FALSE,"Cannot park the acceptance pointer.");
    return host;
}

#include "menus.inl"
#include "tree.inl"
#include "project_window.inl"
#include "viewport.inl"
}

int verify_editor_project(Application& app,const char* output_directory,const wchar_t* source_project) {
    const auto com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);hr(com);
    struct ComScope { ~ComScope() { CoUninitialize(); } } com_scope;
    const auto base=std::filesystem::weakly_canonical(atelier::editor::wide(output_directory));
    const auto output=base/(L"run-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    if (source_project) {
        const auto source=std::filesystem::canonical(source_project);
        const auto relative=base.lexically_relative(source);
        require(relative.empty() || *relative.begin()==L"..","Project acceptance output must be outside the source project.");
        const auto folder=output/L"项目";
        std::filesystem::create_directories(folder);
        for (const auto name:{L"Config",L"Content",L"Source"}) if (std::filesystem::exists(source/name))
            std::filesystem::copy(source/name,folder/name,std::filesystem::copy_options::recursive|std::filesystem::copy_options::skip_symlinks);
    } else std::filesystem::create_directories(output);
    await_acceptance_activation(app,output,"project");
    verify_project_window(app,output,source_project!=nullptr);
    atelier::application::EditorAcceptanceAccess::mark_saved(app);
    SendMessageW(app.window(),WM_CLOSE,0,0);require(!app.step(),"Project acceptance did not close the editor.");
    std::ofstream report(output/"acceptance.txt");
    report<<"Editor project acceptance: PASS\nBuild: "<<atelier::build_info::revision<<"\nConfiguration: "<<atelier::build_info::configuration
        <<"\nSource project: "<<(source_project?atelier::editor::utf8(source_project):"new fixture")
        <<"\nSource project is read only; Config/Content/Source copied without links or Cache.\n"
        <<"Shared commands, About identity/current root, Actor creation, active-field save, asset views/search/filter, OBJ import/create, Scene Tree selection, position edit/undo/redo, actual MatCap/wireframe pixels, save/reopen, failed open, refresh failure after save, movable project and disposable Cache.\n";
    std::printf("Editor project acceptance: PASS; evidence: %s\n",atelier::editor::utf8(output.wstring()).c_str());
    return 0;
}

int verify_editor_window(Application& app, const char* output_directory, bool viewport_only) {
    const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    hr(com);
    struct ComScope { ~ComScope() { CoUninitialize(); } } com_scope;
    const std::filesystem::path output(output_directory);
    std::filesystem::create_directories(output);
    const auto host=await_acceptance_activation(app,output,viewport_only?"viewport":"window");
    tick(app);
    require(actors(app).empty(),"The production editor did not start with an empty scene.");
    if (viewport_only) {
        verify_viewport_display(app,output);
        atelier::application::EditorAcceptanceAccess::mark_saved(app);
        SendMessageW(app.window(),WM_CLOSE,0,0);
        require(!app.step(),"Viewport acceptance did not close normally.");
        std::ofstream(output/"acceptance.txt")<<"Editor viewport acceptance: PASS\n";
        std::puts("Editor viewport acceptance: PASS");
        return 0;
    }
    click(app,control::file_menu);
    const auto first_open_us=app.menu_open_cpu_us(); const auto retained_popup=app.menu_window(); key(app,VK_ESCAPE);
    click(app,control::file_menu);
    const auto warm_open_us=app.menu_open_cpu_us();
    require(app.menu_window()==retained_popup,"Warm open replaced the native popup window."); key(app,VK_ESCAPE);
    std::ofstream(output/"popup-open.csv") << "path,cpu_us\nfirst," << first_open_us << "\nwarm," << warm_open_us << "\n";
    click(app,control::add_static_mesh);
    require(actors(app).size()==1 && app.document().undo_count()==1,"Static mesh creation was not a single document command.");
    require(model(app.document().world().runtime().type_of(app.document().selected()))==app.document().types().static_mesh_actor,
        "Create shelf did not construct the real StaticMeshActor type.");
    verify_tool_shelf(app,output);
    capture(app, output / "m0.png");
    verify_chrome_pixels(app,output);
    const auto named_actor=app.document().selected();
    const auto name_key=atelier::application::EditorAcceptanceAccess::name_control(app,named_actor);
    const std::wstring original_name(app.document().name(named_actor));
    click(app,name_key); type(app,L"主角 Hero"); key(app,VK_RETURN);
    require(app.document().name(named_actor)==L"主角 Hero","Object header did not commit the document name.");
    key(app,'Z',true);
    require(app.document().name(named_actor)==original_name,"Object name bypassed scene Undo.");
    key(app,'Y',true);
    require(app.document().name(named_actor)==L"主角 Hero","Object name Redo failed.");
    key(app,'Z',true); key(app,VK_ESCAPE);
    mouse(app,WM_MOUSEMOVE,centre(app,control::viewport));
    const auto tab_style=*app.presented()->find(control::parameters_tab)->style;
    mouse(app,WM_MOUSEMOVE,centre(app,control::parameters_tab));
    require(*app.presented()->find(control::parameters_tab)->style==tab_style,"Workspace tab hover changed its appearance.");
    auto cursor_point=centre(app,property_control(app,named_actor,0));
    ClientToScreen(app.window(),&cursor_point);
    require(SetCursorPos(cursor_point.x,cursor_point.y)!=FALSE,"Cannot position the cursor over its accepted field.");
    tick(app,3);
    const auto adjust_cursor=GetCursor();
    require(adjust_cursor && adjust_cursor!=LoadCursorW(nullptr,IDC_ARROW) && adjust_cursor!=LoadCursorW(nullptr,IDC_SIZEWE),"Number adjustment did not use its own cursor.");
    SendMessageW(app.window(),WM_SETCURSOR,reinterpret_cast<WPARAM>(app.window()),MAKELPARAM(HTCLIENT,WM_MOUSEMOVE));
    require(GetCursor()==adjust_cursor,"WM_SETCURSOR overwrote the adjustment cursor.");
    require(SetCursorPos(host.left+32,host.top+12)!=FALSE,"Cannot restore the acceptance pointer.");
    tick(app,3);
    const auto navigation_history=app.document().undo_count();
    click(app,control::parameter_back);
    require(app.document().selected()==atelier::ObjectId{} && app.presented()->find(control::scene_forward)->enabled,"Path back did not reach World context.");
    click(app,control::scene_forward);
    require(app.document().selected()==named_actor,"Path forward did not restore the selected Actor.");
    click(app,control::actor_path); capture(app,output/"path-context-menu.png");
    click(app,control::world_context);
    click(app,control::parameter_path); key(app,VK_DOWN); key(app,VK_RETURN);
    require(app.document().selected()==named_actor && app.document().undo_count()==navigation_history,"Path menu selection changed document history or chose the wrong Actor.");
    click(app, control::reset);
    const auto original_width = app.parameter_width();
    auto divider = centre(app, app.parameter_splitter());
    {
        require_input_focus(app); KeyStateScope modifiers;
        SendMessageW(app.window(),WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(divider.x,divider.y));
        divider.x-=50;
        SendMessageW(app.window(),WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(divider.x,divider.y));
        SendMessageW(app.window(),WM_LBUTTONUP,0,MAKELPARAM(divider.x,divider.y)); tick(app,2);
    }
    require(app.parameter_width() > original_width + 20, "Pane divider did not change the workspace.");

    const auto initial_count = actors(app).size();
    click(app, control::add_actor); click(app, control::add_actor);
    require(actors(app).size() == initial_count + 2, "Create command did not enter the scene.");
    const auto added_key = app.document().selected();
    const auto added_id = added_key;
    click(app, control::undo);
    require(!contains(app, added_key), "Undo add did not remove membership.");
    click(app, control::redo);
    require(contains(app, added_id), "Redo add replaced object identity.");
    require(model(atelier::world::components(app.document().world().runtime(), added_key)).empty(),
        "Add Actor did not produce an empty Actor.");
    const auto render_count = model(app.document().world().render_items()).size();
    const auto creation_history=app.document().undo_count();
    click(app,atelier::application::EditorAcceptanceAccess::asset_key(app,"builtin/sphere"));
    require(app.document().undo_count()==creation_history,"Browsing a resource created a scene object.");
    click(app,atelier::editor::browser_control::add);
    const auto mesh_actor=app.document().selected();
    const auto mesh_component=component(app,mesh_actor,app.document().types().static_mesh_component);
    require(app.document().undo_count()==creation_history+1 &&
        model(app.document().world().render_items()).size()==render_count+1 &&
        model(app.document().world().runtime().type_of(mesh_actor))==app.document().types().static_mesh_actor,
        "Asset shelf did not create one complete typed Actor through scene history.");
    click(app,control::parameter_components); capture(app,output/"world-components.png");
    click(app,control::undo);
    require(!contains(app,mesh_actor) && model(app.document().world().render_items()).size()==render_count,
        "Undo did not remove the whole Actor created by the shelf.");
    click(app,control::redo);
    require(contains(app,mesh_actor) && component(app,mesh_actor,app.document().types().static_mesh_component)==mesh_component,
        "Redo changed the created Actor/component identity.");
    const auto object = actors(app).front().id();
    click(app,control::parameter_transform);
    click(app, app.object_control(object));
    require(app.document().selected() == object, "List selection did not select the clicked object.");
    auto field = property_control(app, object, 0);
    const auto before = x(app, object);
    const auto history = app.document().undo_count();
    auto point = centre(app, field);
    mouse(app, WM_LBUTTONDOWN, point);
    for (unsigned move = 0; move < 4; ++move) { point.x += 12; mouse(app, WM_MOUSEMOVE, point); }
    if (!(x(app,object)>before && app.document().undo_count()==history)) {
        capture(app,output/"failed-field-drag.png");
        throw std::runtime_error("Drag failed to preview without history: before="+std::to_string(before)+
            ", value="+std::to_string(x(app,object))+", undo="+std::to_string(app.document().undo_count())+
            ", expected undo="+std::to_string(history)+", editing="+std::to_string(app.document().editing()));
    }
    mouse(app, WM_LBUTTONUP, point);
    const auto after = x(app, object);
    require(app.document().undo_count() == history + 1, "Continuous drag did not commit exactly one undo.");
    click(app, control::undo); require(x(app, object) == before, "Single undo did not restore the drag origin.");
    click(app, control::redo); require(x(app, object) == after, "Redo did not restore the committed position.");
    point = centre(app, field); mouse(app, WM_LBUTTONDOWN, point); point.x += 32; mouse(app, WM_MOUSEMOVE, point);
    key(app, VK_ESCAPE); mouse(app, WM_LBUTTONUP, point);
    require(x(app, object) == after && app.document().undo_count() == history + 1, "Esc did not cancel a continuous edit.");

    click(app, field);
    require(app.wants_keyboard(), "Numeric control did not enter text input.");
    const auto count = actors(app).size();
    key(app, 'A', true); key(app, VK_DELETE);
    require(actors(app).size() == count, "Delete in text focus deleted a scene object.");
    key(app, VK_ESCAPE);
    click(app, field); type(app, L"-2.5"); key(app, VK_RETURN);
    require(std::abs(x(app, object) + 2.5f) < .0001f, "Numeric text commit did not update the Runtime field.");
    click(app, field); type(app, L"10001");
    const auto last_valid_preview = x(app, object);
    const auto invalid_history = app.document().undo_count();
    key(app, VK_RETURN);
    require(x(app, object) == last_valid_preview && app.document().undo_count() == invalid_history && app.wants_keyboard(),
        "Invalid Enter wrote the scene, recorded history or exited text focus.");
    key(app, VK_ESCAPE);
    require(std::abs(x(app, object) + 2.5f) < .0001f, "Esc did not roll back the invalid draft's earlier preview.");
    const auto identity = object;
    const auto old_actor_control = app.object_control(object);
    const auto old_field_control = field;
    click(app, control::remove_actor);
    require(!contains(app, object), "Deleted object remained a member.");
    tick(app, 64); // host GC crosses complete cycles while only history holds this object
    click(app, control::undo);
    require(contains(app, identity) && std::abs(x(app, object) + 2.5f) < .0001f,
        "Window delete/GC/undo failed to restore identity and position.");
    field = property_control(app, object, 0);
    require(field != old_field_control && app.object_control(object) != old_actor_control,
        "Actor restore reused retired control identities.");
    click(app, control::focus); click(app, control::reset);

    verify_transform_projection(app, output, object);

    atelier::renderer::Capture camera_before, camera_orbit, camera_zoom;
    require(app.step(&camera_before), "Camera capture failed.");
    auto view = centre(app, control::viewport);
    SendMessageW(app.window(), WM_RBUTTONDOWN, MK_RBUTTON, MAKELPARAM(view.x, view.y)); tick(app, 1);
    view.x += 42; view.y += 18;
    SendMessageW(app.window(), WM_MOUSEMOVE, MK_RBUTTON, MAKELPARAM(view.x, view.y)); tick(app, 1);
    SendMessageW(app.window(), WM_RBUTTONUP, 0, MAKELPARAM(view.x, view.y)); tick(app, 1);
    require(app.step(&camera_orbit) && camera_orbit.depth != camera_before.depth, "Right-drag orbit did not change the rendered scene.");
    ClientToScreen(app.window(), &view);
    SendMessageW(app.window(), WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM(view.x, view.y)); tick(app, 1);
    require(app.step(&camera_zoom) && camera_zoom.depth != camera_orbit.depth, "Wheel zoom did not change the rendered scene.");
    click(app, control::reset);
    view = centre(app, control::viewport);
    SendMessageW(app.window(), WM_RBUTTONDOWN, MK_RBUTTON, MAKELPARAM(view.x, view.y)); tick(app, 1);
    require(GetCapture() == app.window(), "Orbit did not acquire window capture.");
    SetFocus(nullptr); tick(app, 2);
    require(GetCapture() != app.window(), "Focus loss retained camera-owned capture.");
    SetFocus(app.window()); tick(app, 1);

    const auto before_loss = x(app, object);
    point = centre(app, field); mouse(app, WM_LBUTTONDOWN, point); point.x += 20; mouse(app, WM_MOUSEMOVE, point);
    ReleaseCapture(); tick(app, 2);
    require(!app.document().editing() && x(app, object) == before_loss, "Unexpected capture loss retained a partial edit.");
    point = centre(app, field); mouse(app, WM_LBUTTONDOWN, point); point.x += 20; mouse(app, WM_MOUSEMOVE, point);
    SetFocus(nullptr); tick(app, 2); SetFocus(app.window()); tick(app, 1);
    require(!app.document().editing() && x(app, object) == before_loss, "Focus loss retained a partial edit.");

    const auto before_resize = x(app, object);
    point = centre(app, field); mouse(app, WM_LBUTTONDOWN, point); point.x += 20; mouse(app, WM_MOUSEMOVE, point);
    SetWindowPos(app.window(), nullptr, 0, 0, 1120, 740, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    tick(app, 4);
    require(!app.document().editing() && x(app, object) == before_resize, "Resize retained a partial edit.");
    ShowWindow(app.window(), SW_MINIMIZE);
    const auto accepted = app.accepted_frames(); app.step(); app.step();
    require(app.accepted_frames() == accepted, "Minimized window accepted an interactive frame.");
    ShowWindow(app.window(), SW_RESTORE); tick(app, 4);
    const auto revision = app.presented()->drawing->surface_revision;
    point = centre(app, field); mouse(app, WM_LBUTTONDOWN, point); point.x += 20; mouse(app, WM_MOUSEMOVE, point);
    RECT bounds{}; GetWindowRect(app.window(), &bounds);
    const auto dpi = GetDpiForWindow(app.window());
    SendMessageW(app.window(), WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&bounds));
    tick(app, 3);
    require(app.presented()->drawing->surface_revision > revision && !app.document().editing() && x(app, object) == before_resize,
        "DPI transition policy did not cancel and publish matching hit data.");
    verify_stow_alignment(app);
    verify_control_states(app, output, object);
    verify_style_changes(app, output, object);
    verify_shared_text(app, output, object);
    verify_docking(app, output, object);
    verify_native_menus(app, output, object);
    verify_basic_controls(app, output, object);
    verify_tree_view(app,output,object);
    view = centre(app, control::viewport); ClientToScreen(app.window(), &view);
    SendMessageW(app.window(), WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-2 * WHEEL_DELTA)), MAKELPARAM(view.x, view.y)); tick(app, 1);
    capture(app, output / "editor.png");
    verify_list_workload(app, output);
    verify_project_window(app,output);
    atelier::application::EditorAcceptanceAccess::mark_saved(app);
    SendMessageW(app.window(), WM_CLOSE, 0, 0);
    require(!app.step(), "WM_CLOSE did not close the application.");
    {
        Application quit_app(true);
        SetForegroundWindow(quit_app.window()); SetFocus(quit_app.window());
        RECT quit_host{}; GetWindowRect(quit_app.window(),&quit_host);
        require(SetCursorPos(quit_host.left+32,quit_host.top+12)!=FALSE,"Cannot park the quit acceptance pointer.");
        tick(quit_app);
        key(quit_app,VK_F10);
        require(quit_app.menu_window() && quit_app.menu_frame(),"Quit acceptance did not open an accepted native menu.");
        PostQuitMessage(0);
        atelier::renderer::Capture quit_capture;
        quit_app.capture_menu(quit_capture);
        require(quit_capture.frame_id!=0,"Quit acceptance did not render the native menu capture.");
        require(!quit_app.step(),"Native menu capture consumed the thread quit before the main window.");
        require(!quit_app.menu_window() && GetCapture()!=quit_app.window(),"Thread quit retained an interactive native menu.");
    }
    std::ofstream report(output / "acceptance.txt");
    report << "Editor window acceptance: PASS\n"
           << "Real HWND messages -> platform adapter -> UI accepted-frame input -> Session -> World -> Runtime -> matrix-value renderer\n"
           << "World: empty Actor; typed StaticMeshActor creation from shelf; one-step creation undo/redo preserves component identity; Actor delete/GC/undo same composition/new bindings; AttachedInactive presentation/editing\n"
           << "TRS: actual Session field transactions set rotation/nonuniform scale, World value matrices alter captured GPU depth, two UI undo commands restore the original value and depth; XYZ controls remain position-only\n"
           << "splitter; add/undo/redo identity; list selection; drag preview/one undo; Esc; text/Delete; numeric range; delete/GC/undo; camera buttons/orbit/zoom; capture/focus loss; resize; minimize; DPI transition; WM_CLOSE shutdown; thread quit preserved through native menu capture\n"
           << "S1 retained: shared states; color-only coverage reuse/zero upload; invalid-style rejection; font/metrics/caret/raster; old-frame lifetime; 256 Actors/culling/scroll-back\n"
           << "Scene Tree: actual World membership and Actor types; Actor-only rows; World expansion; type/name filtering; close/reopen retains expansion; no scene history changes.\n"
           << "S2: Unicode parameter search; preview/commit/Esc/OS blur; ZWJ/combining/surrogate deletion; grouped local undo/redo; native clipboard cut/paste; bidi selection; numeric text selection; invalid Enter; retained focus; passive-focus business Z/Y and active local history ownership\n"
           << "Dock workspace: accepted-frame tab drag/preview/cancel, edge split, strip merge/reorder, both divider axes, maximize/restore, Ctrl+Tab/Ctrl+W, reopen and final-pane reset, retained field identity and separate histories; collapse either side, thin restore strip, exact ratio restoration, Show reveals ancestors, central swap and Alt rotation; tab-first chrome, adjacent add button, native add menu moving the existing view.\n"
           << "Parameter controls: shared aligned text/choice/vector rows, modified/default states, atomic tuple reset, native context menus and clipboard, invalid paste cancellation, modal outside click/wheel/Delete, Apps/Shift+F10/Esc, combo keyboard selection/history, middle-drag ladder/fine steps/defaults/cancellation/cache reuse, checkbox/foldout, disabled inheritance, narrow Vector3/Vector4, isolated sample history\n"
           << "Parameter slider: separate navigation/legal ranges, native track click and drag preview, one commit/undo, Esc and capture-loss rollback, original value outside navigation range, Home/End and Shift keyboard steps, disabled input; shared axis-accent SS and GPU capture.\n"
           << "Chrome: GPU pixels match ordinary field outer/inner/fill and selected tab open-bottom colors; shared tab edges, horizontal scrollbar gradient/dots/step availability, combo arrows, leading path icon, context menu selection and back/forward without document history.\n"
           << "Shelf: 56 DIP expanded / 6 DIP stowed; 64 DIP single command hit target with icon above short caption; category and scene state retained across pointer/keyboard stow.\n"
           << "Native menus: actual owned WS_POPUP HWND; shared UI/SS/GPU renderer; beyond-editor placement and screen-work-area containment; F10/Alt mnemonic, arrows/hover switching, reused host, restored owner focus, cancel/focus-loss cleanup; menu commands, context clipboard and combo choices use the original consumers; narrow menubar stow rail, restored geometry/history and F10 reveal.\n"
           << "Failure evidence: real clipboard lock contention preserved the draft; a seeded real Session transaction made native drag edit_begin fail, then preview stopped, rollback/capture release ran, and the first error remained visible.\n"
           << "IME evidence: Unicode WM_CHAR and clipboard checks do not establish system IME acceptance. Real profile/physical-key/composition/candidate verification remains a separate completion gate.\n"
           << "Sampling: s1-performance.csv contains CPU call microseconds, including Present/waits in render; capture frames excluded. No GPU duration or before/after speedup claimed.\n"
           << "Sampling environment: this acceptance application enables hardware D3D12 debug/GBV and renderer vsync, including in release; results do not represent validation-disabled production frame time.\n"
           << "s1-resources.csv: UI cumulative counters use interval differences; renderer activity sums sampled render results. Resource peaks are sampled ownership counts, not total process memory or physical VRAM; R8 upload bytes exclude transfer padding.\n"
           << "DPI coverage: WM_DPICHANGED policy at the current monitor DPI; physical multi-monitor DPI migration not exercised.\n"
           << "Accepted frames: " << app.accepted_frames() << "\n";
    std::puts("Editor window acceptance: PASS");
    std::fflush(stdout);
    return 0;
}

int verify_editor_ime(Application& app, const char* output_directory) try {
    using Microsoft::WRL::ComPtr;
    require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "Cannot initialize IME acceptance COM.");
    struct ComScope { ~ComScope() { CoUninitialize(); } } com_scope;
    const std::filesystem::path output(output_directory);
    std::filesystem::create_directories(output);
    std::ofstream(output / "ime-acceptance.txt") << "Sogou Pinyin acceptance: RUNNING\n";
    std::ofstream setup(output / "ime-platform.txt");
    const auto record_profile = [&](const char* label, HRESULT result, const TF_INPUTPROCESSORPROFILE& value) {
        wchar_t clsid[40]{}, profile_id[40]{};
        StringFromGUID2(value.clsid, clsid, 40); StringFromGUID2(value.guidProfile, profile_id, 40);
        setup << label << ": HRESULT=" << std::hex << result << "; type=" << value.dwProfileType
              << "; language=" << value.langid << "; CLSID=" << std::string(clsid, clsid + wcslen(clsid))
              << "; profile=" << std::string(profile_id, profile_id + wcslen(profile_id)) << "; HKL=" << value.hkl << '\n';
        setup.flush();
    };
    CLSID sogou{}; GUID profile{};
    require(SUCCEEDED(CLSIDFromString(L"{E7EA138E-69F8-11D7-A6EA-00065B844310}", &sogou)) &&
        SUCCEEDED(CLSIDFromString(L"{E7EA138F-69F8-11D7-A6EA-00065B844311}", &profile)), "Invalid Pinyin profile identity.");
    ComPtr<ITfInputProcessorProfiles> languages;
    require(SUCCEEDED(CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&languages))), "Cannot inspect input profiles.");
    BOOL enabled{};
    require(SUCCEEDED(languages->IsEnabledLanguageProfile(sogou, 0x0804, profile, &enabled)), "Cannot query Sogou Pinyin enablement.");
    if (!enabled) {
        std::ofstream(output / "ime-acceptance.txt") << "Sogou Pinyin acceptance: BLOCKED (profile is not enabled; no settings changed)\n";
        throw std::runtime_error("Sogou Pinyin is not enabled; real IME acceptance is blocked.");
    }
    const auto require_foreground = [&] {
        const auto foreground = GetForegroundWindow();
        if (foreground == app.window()) return;
        DWORD owner{}; GetWindowThreadProcessId(foreground, &owner);
        wchar_t class_name[256]{}; GetClassNameW(foreground, class_name, 256);
        setup << "Foreground interruption: HWND=" << foreground << "; editor=" << app.window()
              << "; focus=" << GetFocus() << "; PID=" << std::dec << owner
              << "; class=" << std::string(class_name, class_name + wcslen(class_name));
        if (const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, owner)) {
            wchar_t name[32768]{}; DWORD size = 32768;
            if (QueryFullProcessImageNameW(process, 0, name, &size)) {
                const auto filename = std::filesystem::path(name).filename().wstring();
                setup << "; executable=" << std::string(filename.begin(), filename.end());
            }
            CloseHandle(process);
        }
        setup << '\n'; setup.flush();
        throw std::runtime_error("IME acceptance lost foreground ownership; no further key was sent (see ime-platform.txt).");
    };
    SetForegroundWindow(app.window()); SetFocus(app.window()); tick(app);
    const auto foreground_deadline = GetTickCount64() + 20000;
    while (GetForegroundWindow() != app.window() && GetTickCount64() < foreground_deadline)
        require(app.step(), "Editor closed while waiting for initial foreground.");
    require_foreground(); // Wait for the user once; never repeatedly seize foreground.
    click(app, control::parameter_search);
    ComPtr<ITfInputProcessorProfileMgr> manager;
    require(SUCCEEDED(languages.As(&manager)), "Cannot inspect active input profile.");
    TF_INPUTPROCESSORPROFILE original_profile{};
    const auto original_result = manager->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &original_profile);
    record_profile("Original thread profile", original_result, original_profile);
    require(original_result == S_OK, "Cannot preserve original input profile (S_FALSE means no active profile).");
    struct ProfileScope {
        ITfInputProcessorProfiles* languages;
        ITfInputProcessorProfileMgr* manager;
        TF_INPUTPROCESSORPROFILE original;
        std::ofstream& evidence;
        bool restored{};
        bool restore() {
            if (restored) return true;
            TF_INPUTPROCESSORPROFILE current{};
            const auto matches = [&] {
                return manager->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &current) == S_OK &&
                    current.dwProfileType == original.dwProfileType && current.langid == original.langid &&
                    IsEqualGUID(current.clsid, original.clsid) && IsEqualGUID(current.guidProfile, original.guidProfile) && current.hkl == original.hkl;
            };
            if (!matches()) {
                if (languages->ChangeCurrentLanguage(original.langid) != S_OK) return false;
                if (!matches() && manager->ActivateProfile(original.dwProfileType, original.langid, original.clsid, original.guidProfile,
                    original.dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR ? nullptr : original.hkl, 0) != S_OK) return false;
            }
            restored = matches();
            evidence << "Original thread profile restore verified=" << restored << "; type=" << current.dwProfileType
                     << "; language=" << std::hex << current.langid << "; HKL=" << current.hkl << '\n'; evidence.flush();
            return restored;
        }
        ~ProfileScope() { if (!restore()) std::fputs("IME acceptance: restoring the original thread profile failed.\n", stderr); }
    } restore{languages.Get(), manager.Get(), original_profile, setup};
    if (original_profile.dwProfileType != TF_PROFILETYPE_INPUTPROCESSOR || original_profile.langid != 0x0804 ||
        !IsEqualGUID(original_profile.clsid, sogou) || !IsEqualGUID(original_profile.guidProfile, profile)) {
        const auto language_result = languages->ChangeCurrentLanguage(0x0804);
        setup << "ChangeCurrentLanguage(0804): HRESULT=" << std::hex << language_result << '\n'; setup.flush();
        require(language_result == S_OK, "Cannot select Chinese on the acceptance thread.");
        TF_INPUTPROCESSORPROFILE after_language{};
        const auto after_result = manager->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &after_language);
        record_profile("After language selection", after_result, after_language);
        if (after_result != S_OK || after_language.dwProfileType != TF_PROFILETYPE_INPUTPROCESSOR ||
            after_language.langid != 0x0804 || !IsEqualGUID(after_language.clsid, sogou) || !IsEqualGUID(after_language.guidProfile, profile)) {
            const auto activated = manager->ActivateProfile(TF_PROFILETYPE_INPUTPROCESSOR, 0x0804, sogou, profile, nullptr, 0);
            setup << "ActivateProfile(Sogou,flags=0): HRESULT=" << std::hex << activated << '\n'; setup.flush();
            require(activated == S_OK, "Cannot select the enabled Sogou profile on the acceptance thread.");
        }
        tick(app, 2);
    }
    const auto check_active_profile = [&] {
        TF_INPUTPROCESSORPROFILE current{};
        const auto result = manager->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &current);
        record_profile("Active thread profile", result, current);
        require(result == S_OK && current.dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR && current.langid == 0x0804 &&
            IsEqualGUID(current.clsid, sogou) && IsEqualGUID(current.guidProfile, profile), "Active input profile is not Sogou Pinyin.");
    };
    check_active_profile(); // Only this thread's enabled profile may change; no enablement or default setters.
    const auto record_input = [&](const char* phase) {
        const auto context = ImmGetContext(app.window());
        DWORD conversion{}, sentence{};
        const auto conversion_found = context && ImmGetConversionStatus(context, &conversion, &sentence);
        const auto stats = app.input_statistics();
        setup << phase << ": IMC=" << context << "; open=" << (context && ImmGetOpenStatus(context))
              << "; conversion-valid=" << conversion_found << "; conversion=" << std::hex << conversion
              << "; native-comp-bytes=" << std::dec << (context ? ImmGetCompositionStringW(context, GCS_COMPSTR, nullptr, 0) : -1)
              << "; starts=" << stats.compositions << "; updates=" << stats.updates << "; results=" << stats.results
              << "; input=" << app.text_input().has_value() << "; composing=" << (app.text_input() && app.text_input()->composing)
              << "; query-units=" << app.query().size() << '\n';
        setup.flush();
        if (context) ImmReleaseContext(app.window(), context);
    };
    record_input("Before physical input");
    const auto verify_native_origin = [&] {
        const auto state = app.text_input();
        require(state && state->accepted_anchor, "No accepted insertion/composition origin for native query.");
        const auto& geometry = *state->accepted_anchor;
        IMECHARPOSITION position{}; position.dwSize = sizeof(position);
        require(SendMessageW(app.window(), WM_IME_REQUEST, IMR_QUERYCHARPOSITION, reinterpret_cast<LPARAM>(&position)) != 0,
            "Native composition-origin query was rejected.");
        POINT expected{static_cast<LONG>(std::floor(geometry.bounds.x)), static_cast<LONG>(std::floor(geometry.bounds.y))};
        POINT document[2]{{geometry.clip.left, geometry.clip.top}, {geometry.clip.right, geometry.clip.bottom}};
        require(ClientToScreen(app.window(), &expected) && position.pt.x == expected.x && position.pt.y == expected.y &&
            position.cLineHeight == static_cast<UINT>(std::ceil(geometry.bounds.height)), "Native origin is not the accepted physical position in screen coordinates.");
        require(ClientToScreen(app.window(), &document[0]) && ClientToScreen(app.window(), &document[1]) &&
            position.rcDocument.left == document[0].x && position.rcDocument.top == document[0].y &&
            position.rcDocument.right == document[1].x && position.rcDocument.bottom == document[1].y,
            "Native text document is not the accepted field clip in screen coordinates.");
        position.dwCharPos = 1;
        require(SendMessageW(app.window(), WM_IME_REQUEST, IMR_QUERYCHARPOSITION, reinterpret_cast<LPARAM>(&position)) == 0,
            "Unsupported composition offset reused the insertion origin.");
        position.dwCharPos = 0; position.dwSize = 0;
        require(SendMessageW(app.window(), WM_IME_REQUEST, IMR_QUERYCHARPOSITION, reinterpret_cast<LPARAM>(&position)) == 0 &&
            SendMessageW(app.window(), WM_IME_REQUEST, IMR_QUERYCHARPOSITION, 0) == 0, "Invalid native geometry request was accepted.");
    };
    verify_native_origin();
    const auto capture_native_candidate = [&](const char* filename) {
        // Desktop pixels inside the foreground editor include the OS candidate popup.
        RECT rect{}; require(GetWindowRect(app.window(), &rect) != FALSE, "Cannot locate IME screenshot region.");
        const auto width = rect.right - rect.left, height = rect.bottom - rect.top;
        HDC screen = GetDC(nullptr), memory = CreateCompatibleDC(screen);
        HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
        struct CaptureScope {
            HDC screen, memory; HBITMAP bitmap;
            ~CaptureScope() { DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(nullptr, screen); }
        } resources{screen, memory, bitmap};
        require(screen && memory && bitmap, "Cannot allocate native candidate capture.");
        const auto old = SelectObject(memory, bitmap);
        const auto copied = BitBlt(memory, 0, 0, width, height, screen, rect.left, rect.top, SRCCOPY | CAPTUREBLT);
        SelectObject(memory, old);
        require(copied != FALSE, "Cannot capture native candidate pixels.");
        atelier::renderer::Capture image;
        image.extent = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
        image.rgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
        require(GetDIBits(memory, bitmap, 0, static_cast<UINT>(height), image.rgba.data(), &info, DIB_RGB_COLORS) == height,
            "Cannot read native candidate pixels.");
        for (std::size_t i = 0; i < image.rgba.size(); i += 4) { std::swap(image.rgba[i], image.rgba[i + 2]); image.rgba[i + 3] = 255; }
        png(image, output / filename);
    };
    const auto physical_key = [&](WORD code, bool control = false) {
        require_foreground();
        for (const auto modifier : {VK_CONTROL, VK_MENU, VK_SHIFT, VK_LWIN, VK_RWIN})
            require((GetAsyncKeyState(modifier) & 0x8000) == 0, "A physical modifier is held; IME acceptance stopped.");
        INPUT input[4]{};
        const UINT count = control ? 4 : 2;
        for (UINT index = 0; index < count; ++index) input[index].type = INPUT_KEYBOARD;
        const UINT first = control ? 1 : 0;
        input[first].ki.wScan = input[first + 1].ki.wScan = static_cast<WORD>(MapVirtualKeyW(code, MAPVK_VK_TO_VSC));
        input[first].ki.dwFlags = KEYEVENTF_SCANCODE;
        input[first + 1].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        if (control) {
            input[0].ki.wScan = input[3].ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_LCONTROL, MAPVK_VK_TO_VSC));
            input[0].ki.dwFlags = KEYEVENTF_SCANCODE;
            input[3].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        }
        require(SendInput(count, input, sizeof(INPUT)) == count, "SendInput did not insert the full physical key sequence.");
        tick(app, 2);
    };
    const auto wait_for = [&](const auto& predicate, const char* message) {
        const auto deadline = GetTickCount64() + 5000;
        while (!predicate() && GetTickCount64() < deadline) {
            require_foreground();
            require(app.step(), "Editor closed during real IME acceptance.");
        }
        if (!predicate()) { record_input(message); capture_native_candidate("s2-ime-failure-native.png"); }
        require(predicate(), message);
    };
    const auto candidate_count = [&] {
        const auto context = ImmGetContext(app.window());
        if (!context) return DWORD{0};
        struct ContextScope { HWND window; HIMC context; ~ContextScope() { ImmReleaseContext(window, context); } } context_scope{app.window(), context};
        const auto bytes = ImmGetCandidateListW(context, 0, nullptr, 0);
        if (bytes < sizeof(CANDIDATELIST)) return DWORD{0};
        require(bytes <= 65536, "Native candidate state exceeded the inspection bound.");
        std::vector<DWORD> storage((bytes + sizeof(DWORD) - 1) / sizeof(DWORD));
        auto* candidates = reinterpret_cast<CANDIDATELIST*>(storage.data());
        require(ImmGetCandidateListW(context, 0, candidates, bytes) == bytes, "Cannot inspect native candidate state.");
        return candidates->dwCount;
    };
    const auto compose_position = [&] {
        for (const auto code : {'W','E','I','Z','H','I'}) physical_key(static_cast<WORD>(code));
        wait_for([&] { return candidate_count() != 0; }, "System IME did not produce candidates.");
    };
    const auto native_before = app.input_statistics();
    compose_position();
    capture(app, output / "s2-ime-preedit.png");
    capture_native_candidate("s2-ime-native-candidate.png");
    physical_key(VK_SPACE); // The captured local Sogou candidate page shows position as item 1.
    wait_for([&] { return app.query() == L"位置" && app.text_input() && !app.text_input()->composing; }, "Pinyin candidate did not confirm exactly one position query.");
    require(actors(app).size() == 1, "IME confirmation triggered a scene command.");
    capture(app, output / "s2-ime-confirmed.png");
    const auto confirmed_results = app.input_statistics().results;
    physical_key('X');
    wait_for([&] { return candidate_count() != 0; }, "Sogou cancel scenario did not show candidates.");
    const auto reading_history = app.document().undo_count();
    physical_key('Z', true);
    require(app.query() == L"位置" && app.document().undo_count() == reading_history,
        "Ctrl+Z during native candidates changed the field or scene history.");
    if (!candidate_count()) { physical_key('X'); wait_for([&] { return candidate_count() != 0; }, "Sogou redo scenario did not show candidates."); }
    physical_key('Y', true);
    require(app.query() == L"位置" && app.document().undo_count() == reading_history,
        "Ctrl+Y during native candidates changed the field or scene history.");
    if (!candidate_count()) { physical_key('X'); wait_for([&] { return candidate_count() != 0; }, "Sogou Escape scenario did not show candidates."); }
    physical_key(VK_ESCAPE);
    wait_for([&] { return candidate_count() == 0 && app.text_input() && !app.text_input()->composing; }, "IME Escape did not close candidates.");
    require(app.query() == L"位置", "Cancelling IME also cancelled confirmed field text.");
    physical_key('A'); physical_key('B'); physical_key('C');
    wait_for([&] { return candidate_count() != 0; }, "Sogou Enter scenario did not show candidates.");
    physical_key(VK_RETURN);
    wait_for([&] { return app.input_statistics().results > confirmed_results && app.text_input() && !app.text_input()->composing; }, "IME Enter did not produce a confirmed result.");
    key(app, 'Z', true);
    require(app.query() == L"位置" && actors(app).size() == 1, "IME Enter also committed the field or reached scene history.");
    physical_key(VK_RETURN); // A new key sequence commits the search field.
    physical_key(VK_SPACE);
    const auto clean_space_query = app.query();
    require(clean_space_query.size() > 2 && clean_space_query.starts_with(L"位置"), "Clean-state Space did not insert text at the search caret.");
    physical_key(VK_ESCAPE);
    require(app.query() == L"位置", "Clean-state Space cancellation did not restore the committed query.");
    if (actors(app).empty()) click(app,control::add_static_mesh);
    const auto object = app.document().selected();
    click(app, property_control(app, object, 0));
    compose_position(); physical_key(VK_SPACE); // The captured local Sogou candidate page shows position as item 1.
    wait_for([&] { return app.text_input() && !app.text_input()->composing; }, "Numeric Pinyin result did not end composition.");
    const auto history = app.document().undo_count();
    physical_key(VK_RETURN);
    require(x(app, object) == 0 && app.document().undo_count() == history && app.wants_keyboard(), "Invalid IME numeric draft wrote the scene or exited focus.");
    physical_key(VK_ESCAPE);
    require(x(app, object) == 0, "Numeric IME cancellation did not restore its value.");

    click(app, control::parameter_search); compose_position();
    SetFocus(nullptr);
    IMECHARPOSITION stale_origin{}; stale_origin.dwSize = sizeof(stale_origin);
    require(SendMessageW(app.window(), WM_IME_REQUEST, IMR_QUERYCHARPOSITION, reinterpret_cast<LPARAM>(&stale_origin)) == 0,
        "Native focus loss retained an input-origin snapshot before UI delivery.");
    tick(app, 2);
    require(!app.text_input() && app.query() == L"位置", "OS blur did not cancel the real composition and retain the committed query.");
    require_foreground(); SetFocus(app.window()); tick(app, 1);
    click(app, control::parameter_search);
    physical_key(VK_SPACE);
    require(app.query() == clean_space_query, "Space after OS blur differed from the same IME's clean-state input.");
    physical_key(VK_ESCAPE);
    compose_position();
    RECT original_bounds{}; require(GetWindowRect(app.window(), &original_bounds) != FALSE, "Cannot preserve editor bounds.");
    SetWindowPos(app.window(), nullptr, 0, 0, original_bounds.right - original_bounds.left + 24,
        original_bounds.bottom - original_bounds.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    require(SendMessageW(app.window(), WM_IME_REQUEST, IMR_QUERYCHARPOSITION, reinterpret_cast<LPARAM>(&stale_origin)) == 0,
        "Native surface change retained an input-origin snapshot before UI delivery.");
    tick(app, 3);
    require((!app.text_input() || !app.text_input()->composing) && app.query() == L"位置", "Surface change retained a real composition or changed the query.");
    SetWindowPos(app.window(), nullptr, 0, 0, original_bounds.right - original_bounds.left,
        original_bounds.bottom - original_bounds.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    tick(app, 3);
    click(app, control::parameter_search);
    capture_native_candidate("s2-ime-surface-cleared.png");
    physical_key(VK_SPACE);
    require(app.query() == clean_space_query && x(app, object) == 0,
        "Space after a surface change differed from clean-state input or changed the object.");
    physical_key(VK_ESCAPE);
    click(app, property_control(app, object, 0));
    compose_position();
    require(!app.document().editing(), "Native reading began a numeric business edit before confirmation.");
    click(app, control::parameter_search);
    require(app.text_input() && app.text_input()->token.key == control::parameter_search,
        "Field-switch input target is not the search field.");
    physical_key(VK_SPACE);
    setup << "Field-switch expected UTF16:";
    for (const auto unit : clean_space_query) setup << ' ' << std::hex << static_cast<unsigned>(unit);
    setup << "\nField-switch actual UTF16:";
    for (const auto unit : app.query()) setup << ' ' << std::hex << static_cast<unsigned>(unit);
    setup << "\nOld object X=" << x(app, object) << "; business editing=" << app.document().editing() << '\n'; setup.flush();
    if (app.query() != clean_space_query || x(app, object) != 0 || app.document().editing())
        capture(app, output / "s2-ime-field-switch-failure.png");
    require(app.query() == clean_space_query, "Space after switching editable fields differed from clean-state input.");
    require(x(app, object) == 0, "Space after switching editable fields changed the old object's value.");
    require(!app.document().editing(), "Space in the search field retained a numeric business edit.");
    physical_key(VK_ESCAPE);
    click(app, control::add_static_mesh);
    const auto other = app.document().selected();
    const auto other_x = x(app, other);
    click(app, app.object_control(object));
    click(app, property_control(app, object, 0)); compose_position();
    click(app, app.object_control(other));
    require(app.document().selected() == other && !app.document().editing() && x(app, object) == 0 &&
        x(app, other) == other_x && (!app.text_input() || !app.text_input()->composing),
        "Changing the parameter target retained composition or edited either object.");

    click(app, control::parameter_search);
    physical_key(VK_SPACE);
    require(app.query() == clean_space_query, "Space after changing the parameter target differed from clean-state input.");
    physical_key(VK_ESCAPE);
    const std::wstring long_query(100, L'x');
    type(app, long_query.c_str()); key(app, VK_RETURN); // Owned WM_CHAR only seeds the horizontal-scroll fixture.
    key(app, VK_END); compose_position();
    const auto text_state = app.text_input();
    require(text_state && candidate_count() != 0 && text_state->accepted_caret, "Long-field native candidates have no accepted caret.");
    const auto& caret = *text_state->accepted_caret;
    verify_native_origin();
    require(caret.bounds.x >= static_cast<float>(caret.clip.left) &&
        caret.bounds.x + caret.bounds.width <= static_cast<float>(caret.clip.right), "Scrolled composition caret escaped the field clip.");
    const auto input_context = ImmGetContext(app.window());
    require(input_context != nullptr, "Long-field composition lost its input context.");
    CANDIDATEFORM candidate{};
    const auto candidate_found = ImmGetCandidateWindow(input_context, 0, &candidate);
    ImmReleaseContext(app.window(), input_context);
    require(candidate_found && candidate.dwStyle == CFS_EXCLUDE &&
        candidate.rcArea.left == static_cast<LONG>(std::floor(caret.bounds.x)) &&
        candidate.rcArea.top == static_cast<LONG>(std::floor(caret.bounds.y)) &&
        candidate.rcArea.bottom == static_cast<LONG>(std::ceil(caret.bounds.y + caret.bounds.height)),
        "Native candidate anchor does not match the accepted scrolled caret.");
    capture(app, output / "s2-ime-horizontal.png"); capture_native_candidate("s2-ime-horizontal-native.png");
    physical_key(VK_ESCAPE);
    require(app.query() == long_query, "Cancelling scrolled composition changed confirmed text.");
    type(app, L""); key(app, VK_RETURN);
    const auto scene_history=app.document().undo_count();
    namespace sample=atelier::editor::workbench_control;
    click(app,control::workbench);
    click(app,sample::name); type(app,L""); key(app,VK_RETURN);
    compose_position(); verify_native_origin();
    physical_key(VK_SPACE);
    wait_for([&] { return app.control_workbench().values().name==L"位置" && app.text_input() && !app.text_input()->composing; },
        "Workbench IME Space did not confirm the selected candidate.");
    physical_key(VK_RETURN);
    compose_position();
    click(app,sample::disable);
    require(!app.text_input() && !app.presented()->find(sample::name)->enabled && app.control_workbench().values().name==L"位置",
        "Disabling the workbench retained native composition or changed confirmed text.");
    key(app,VK_SPACE);
    click(app,sample::name);
    compose_position();
    click(app,control::parameters_tab);
    require(!app.text_input() && app.control_workbench().values().name==L"位置" && app.document().undo_count()==scene_history,
        "Switching from workbench to scene retained composition or changed scene history.");
    click(app,control::workbench);
    click(app,sample::name);
    capture(app,output/"controls-ime-confirmed.png");
    click(app,control::parameters_tab);
    const auto native_after = app.input_statistics();
    capture(app, output / "s2-ime-final.png");
    check_active_profile();
    require(restore.restore(), "Cannot restore the original thread input profile.");
    TF_INPUTPROCESSORPROFILE restored_profile{};
    record_profile("Restored thread profile", manager->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &restored_profile), restored_profile);
    atelier::application::EditorAcceptanceAccess::mark_saved(app);
    SendMessageW(app.window(), WM_CLOSE, 0, 0); require(!app.step(), "IME acceptance did not close normally.");
    std::ofstream report(output / "ime-acceptance.txt");
    report << "Sogou Pinyin acceptance: PASS\n"
           << "Enabled profile 0804:{E7EA138E-69F8-11D7-A6EA-00065B844310}{E7EA138F-69F8-11D7-A6EA-00065B844311}\n"
           << "Physical SendInput scan codes -> system IME -> owned IMM32 snapshots -> shared TextEdit -> search/number\n"
           << "Composition starts: " << native_after.compositions - native_before.compositions
           << "; result snapshots: " << native_after.results - native_before.results
           << "; candidate opens: " << native_after.candidate_opens - native_before.candidate_opens << '\n'
           << "Candidate changes: " << native_after.candidate_changes - native_before.candidate_changes << '\n'
           << "Candidate/Space confirm/Escape cancel; physical Ctrl+Z/Y during candidates preserve field and scene history; IME Enter ownership; invalid numeric result; OS blur/surface/editable-field/target cancellation; horizontal composition/accepted candidate anchor; original thread profile restored; no enable/default/list setters used.\n"
           << "The long-field fixture uses owned WM_CHAR for its confirmed prefix; all IME compositions use physical SendInput and native IMM32 events.\n";
    report << "Control workbench: real candidate Space confirmation, accepted native anchor, foldout and scene-tab composition cancellation, sample/scene history isolation.\n";
    require(report.good(), "Cannot save real IME acceptance evidence.");
    std::puts("Sogou Pinyin acceptance: PASS"); return 0;
} catch (const std::exception& error) {
    const std::string_view reason(error.what());
    const auto outcome = reason.starts_with("IME acceptance lost foreground") ? "INTERRUPTED" :
        reason.find("not enabled") != std::string_view::npos ? "BLOCKED" : "FAIL";
    std::ofstream(std::filesystem::path(output_directory) / "ime-acceptance.txt")
        << "Sogou Pinyin acceptance: " << outcome << '\n' << error.what() << '\n';
    throw;
}
