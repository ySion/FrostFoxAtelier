#include "editor/viewport_tool.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <numbers>
#include <unordered_set>

namespace atelier::editor {
namespace {
using V2=math::Vector2;
using V3=math::Vector3;
using Handle=ViewportTool::Handle;
using Shape=renderer::OverlayShape;
constexpr float pi=std::numbers::pi_v<float>;
constexpr std::array<V3,3> axes{{{1,0,0},{0,1,0},{0,0,1}}};
// Houdini handle palette, sampled from the Move/Rotate/Scale handles: #ff1414 / #14ff14 / #1414ff, #ffd900 when picked.
// ui::Color is linear; reference colours are sampled as sRGB bytes.
ui::Color srgb(int r,int g,int b,float a=1) {
    const auto f=[](int v){const double c=v/255.;return float(c<=.04045?c/12.92:std::pow((c+.055)/1.055,2.4));};
    return {f(r),f(g),f(b),a};
}
const std::array<ui::Color,3> colors{srgb(0xff,0x14,0x14),srgb(0x14,0xff,0x14),srgb(0x14,0x14,0xff)};
const ui::Color picked=srgb(0xff,0xd9,0x00);
auto invalid(const char* text) {return std::unexpected(Error{ErrorCode::invalid_definition,text});}
float length(V2 v) {return std::hypot(v.x,v.y);}
float distance(V2 p,V2 a,V2 b) {
    const auto d=b-a;const auto square=math::dot(d,d);
    return length(p-(a+d*(square>0?std::clamp(math::dot(p-a,d)/square,0.f,1.f):0.f)));
}
math::Quaternion rotation(V3 axis,float angle) {
    const auto s=std::sin(angle*.5f);return {axis.x*s,axis.y*s,axis.z*s,std::cos(angle*.5f)};
}
math::Quaternion frame_rotation(V3 x,V3 y,V3 z) {
    const float trace=x.x+y.y+z.z;math::Quaternion q;
    if(trace>0) {const float s=std::sqrt(trace+1)*2;q={(y.z-z.y)/s,(z.x-x.z)/s,(x.y-y.x)/s,s*.25f};}
    else if(x.x>y.y && x.x>z.z) {const float s=std::sqrt(1+x.x-y.y-z.z)*2;q={s*.25f,(y.x+x.y)/s,(z.x+x.z)/s,(y.z-z.y)/s};}
    else if(y.y>z.z) {const float s=std::sqrt(1+y.y-x.x-z.z)*2;q={(y.x+x.y)/s,s*.25f,(z.y+y.z)/s,(z.x-x.z)/s};}
    else {const float s=std::sqrt(1+z.z-x.x-y.y)*2;q={(z.x+x.z)/s,(z.y+y.z)/s,s*.25f,(x.y-y.x)/s};}
    return math::try_normalize(q).value_or(math::Quaternion{});
}
std::optional<V3> on_plane(const ViewportView& view,V2 point,V3 origin,V3 normal) {
    const auto ray=view.ray(point.x,point.y);const auto denominator=math::dot(ray.direction,normal);
    if (std::abs(denominator)<1e-5f) return {};
    const auto t=math::dot(origin-ray.origin,normal)/denominator;
    if (!std::isfinite(t) || t<0) return {};
    return ray.origin+ray.direction*t;
}
// Drawing and hit testing visit exactly the same physical geometry. Proportions
// follow Houdini 22's object handles at 1x: 1 px axis lines reaching ~84 px,
// solid 15 px tip markers, outlined plane diamonds at 0.3 of the axis, and a
// rotate ball whose rings fade on the far hemisphere inside a purple view ring.
template<class Emit> void handles(const ViewportView& view,V3 pivot,math::Quaternion orientation,TransformTool tool,Emit emit) {
    const auto center=view.project(pivot);if (!center || tool==TransformTool::select) return;
    const float scale=view.dpi_scale, unit=view.pixel_size(pivot)*scale;
    const auto line=[&](Handle h,V2 a,V2 b,ui::Color color,float width=1.f) {
        emit(h,Shape{Shape::Kind::segment,{a.x,a.y,b.x,b.y},width*scale,color});
    };
    const auto box=[&](Handle h,V2 at,float size,ui::Color color,ui::Color edge={0,0,0,.55f}) {
        emit(h,Shape{Shape::Kind::rectangle,{at.x-size*scale*.5f,at.y-size*scale*.5f,size*scale,size*scale},scale,color,edge});
    };
    const auto shade=[](ui::Color c,float k){return ui::Color{c.r*k,c.g*k,c.b*k,c.a};};
    const auto ring=[&](Handle h,V3 u,V3 v,float radius,ui::Color color,float width) {
        for (int step=0;step<96;++step) {
            const auto at=[&](int n) {const auto a=2*pi*float(n)/96;return pivot+(u*std::cos(a)+v*std::sin(a))*(unit*radius);};
            const auto a=at(step),b=at(step+1);const auto pa=view.project(a),pb=view.project(b);if(!pa || !pb) continue;
            // Front arcs are opaque; arcs behind the ball's silhouette plane read as faded.
            const bool front=math::dot((a+b)*.5f-pivot,view.forward)<=0;
            line(h,*pa,*pb,front?color:ui::Color{color.r,color.g,color.b,.42f},front?width:width*.8f);
        }
    };
    if (tool==TransformTool::rotate || tool==TransformTool::transform) {
        const float radius=tool==TransformTool::rotate?78.f:62.f;
        for (int axis=0;axis<3;++axis)
            ring(static_cast<Handle>(int(Handle::rotate_x)+axis),axes[(axis+1)%3],axes[(axis+2)%3],radius,colors[axis],1.5f);
        if (tool==TransformTool::rotate) for(int step=0;step<96;++step) {
            const auto at=[&](int n){const auto a=2*pi*float(n)/96;return *center+V2{std::cos(a),std::sin(a)}*(radius*1.18f*scale);};
            line(Handle::rotate_view,at(step),at(step+1),srgb(0x80,0x33,0x80,.95f),1.5f);
        }
    }
    const float reach=tool==TransformTool::transform?92.f:84.f;
    for(int axis=0;axis<3;++axis) {
        const auto color=colors[axis];
        if(tool==TransformTool::transform || tool==TransformTool::move) {
            const auto tip=view.project(pivot+axes[axis]*(reach*unit));if(!tip) continue;
            const auto d=*tip-*center;const auto size=length(d);if(size<8*scale) continue;
            const auto direction=d/size,side=V2{-direction.y,direction.x};
            const auto h=static_cast<Handle>(int(Handle::move_x)+axis);
            line(h,*center,*tip,shade(color,.62f),1.25f);
            // Solid cone: filled by a fan of strokes from the tip to the base.
            const auto base=*tip-direction*(15*scale);
            for(int k=-4;k<=4;++k) line(h,*tip,base+side*(float(k)*1.f*scale),color,1.5f);
            line(h,base-side*(5*scale),base+side*(5*scale),shade(color,.45f),1.25f);
            // Plane handle: a diamond centred at 0.3 of the two other axes.
            const auto u=axes[(axis+1)%3],v=axes[(axis+2)%3];const float at=reach*.3f,half=reach*.045f;
            const std::array corners{pivot+(u*at+v*(at-half))*unit,pivot+(u*(at+half)+v*at)*unit,
                pivot+(u*at+v*(at+half))*unit,pivot+(u*(at-half)+v*at)*unit};
            std::array<std::optional<V2>,4> p{};for(int i=0;i<4;++i) p[i]=view.project(corners[i]);
            const auto plane=static_cast<Handle>(int(Handle::plane_yz)+axis);
            if(p[0]&&p[1]&&p[2]&&p[3]) for(int i=0;i<4;++i) line(plane,*p[i],*p[(i+1)%4],{color.r*.62f+.24f,color.g*.62f+.24f,color.b*.62f+.24f,1},1.25f);
        }
        if(tool==TransformTool::transform || tool==TransformTool::scale) {
            const float length=tool==TransformTool::transform?reach*.62f:reach;
            const auto tip=view.project(pivot+math::rotate(orientation,axes[axis])*(length*unit));
            if(tip) {
                const auto h=static_cast<Handle>(int(Handle::scale_x)+axis);
                if(tool==TransformTool::scale) line(h,*center,*tip,shade(color,.62f),1.25f);
                box(h,*tip,tool==TransformTool::scale?13.f:9.f,color);
            }
        }
    }
    if(tool==TransformTool::scale) box(Handle::scale_uniform,*center,11,srgb(0xae,0x8b,0xd2));
    else if(tool!=TransformTool::rotate) box(Handle::move_view,*center,7,srgb(0xee,0xee,0xe6));
}
struct TriangleHit {float distance,u,v;};
std::optional<TriangleHit> triangle(const ViewportView::Ray& ray,V3 a,V3 b,V3 c) {
    const auto edge1=b-a,edge2=c-a,h=math::cross(ray.direction,edge2);const auto det=math::dot(edge1,h);
    if(std::abs(det)<1e-8f) return {};
    const auto offset=ray.origin-a;const float u=math::dot(offset,h)/det;
    if(u<0 || u>1) return {};
    const auto q=math::cross(offset,edge1);const float v=math::dot(ray.direction,q)/det;
    if(v<0 || u+v>1) return {};
    const float t=math::dot(edge2,q)/det;return t>=0?std::optional(TriangleHit{t,u,v}):std::nullopt;
}
}

math::Vector3 euler_degrees(math::Quaternion value) noexcept {
    // Convert in double and round only at the float storage boundary.
    const auto q=math::try_normalize(math::Quaterniond{value.x,value.y,value.z,value.w}).value_or(math::Quaterniond{});
    constexpr double degrees=180/std::numbers::pi;
    const double y=std::asin(std::clamp(2*(q.w*q.y-q.z*q.x),-1.,1.));
    if(std::abs(std::cos(y))<1e-5) return {float(2*std::atan2(q.x,q.w)*degrees),float(y*degrees),0};
    return {float(std::atan2(2*(q.w*q.x+q.y*q.z),1-2*(q.x*q.x+q.y*q.y))*degrees),
        float(y*degrees),float(std::atan2(2*(q.w*q.z+q.x*q.y),1-2*(q.y*q.y+q.z*q.z))*degrees)};
}
math::Quaternion from_euler_degrees(math::Vector3 value) noexcept {
    const auto axis_rotation=[](V3 axis,double degrees) {
        const auto half_angle=degrees*std::numbers::pi/360,s=std::sin(half_angle);
        return math::Quaterniond{axis.x*s,axis.y*s,axis.z*s,std::cos(half_angle)};
    };
    const auto q=axis_rotation(axes[2],value.z)*axis_rotation(axes[1],value.y)*axis_rotation(axes[0],value.x);
    return {float(q.x),float(q.y),float(q.z),float(q.w)};
}
ViewportView ViewportView::make(const Camera& camera,ui::Rect bounds,float dpi) {
    ViewportView result;result.bounds=bounds;result.dpi_scale=dpi;result.eye=camera.eye();
    result.orthographic=camera.view()!=CameraView::perspective;
    result.construction_normal=camera.construction_normal();
    const auto matrices=camera.matrices(std::max(bounds.width,1.f)/std::max(bounds.height,1.f));
    result.view=matrices.view;result.projection=matrices.projection;
    math::Matrix4d vp;const auto m=matrices.view*matrices.projection;
    std::copy(m.begin(),m.end(),vp.begin());result.inverse=math::try_inverse(vp).value_or(math::Matrix4d::identity());
    result.forward=math::try_normalize(camera.target()-camera.eye()).value_or(V3{0,0,1});
    result.right=math::try_normalize(math::cross(camera.up_hint(),result.forward)).value_or(V3{1,0,0});
    result.up=math::cross(result.forward,result.right);return result;
}
bool ViewportView::contains(float x,float y) const noexcept {
    return x>=bounds.x && y>=bounds.y && x<bounds.x+bounds.width && y<bounds.y+bounds.height;
}
std::optional<V2> ViewportView::project(V3 p) const noexcept {
    const auto clip=math::Vector4{p.x,p.y,p.z,1}*view*projection;
    if(!math::is_finite(clip) || clip.w<=0 || clip.z<0 || clip.z>clip.w) return {};
    return V2{bounds.x+(clip.x/clip.w+1)*bounds.width*.5f,bounds.y+(1-clip.y/clip.w)*bounds.height*.5f};
}
ViewportView::Ray ViewportView::ray(float x,float y) const noexcept {
    const double nx=2*(x-bounds.x)/std::max(bounds.width,1.f)-1,ny=1-2*(y-bounds.y)/std::max(bounds.height,1.f);
    const auto p=math::Vector4d{nx,ny,1,1}*inverse;
    const auto far=math::Vector4d{nx,ny,.5,1}*inverse;
    const V3 near{float(p.x/p.w),float(p.y/p.w),float(p.z/p.w)};
    return {near,math::try_normalize(V3{float(far.x/far.w)-near.x,float(far.y/far.w)-near.y,float(far.z/far.w)-near.z}).value_or(forward)};
}
float ViewportView::pixel_size(V3 at) const noexcept {
    return (orthographic?1.f:std::max(math::dot(at-eye,forward),.05f))*2/(std::max(bounds.height,1.f)*projection[5]);
}
std::optional<ViewportTool::Targets> ViewportTool::targets(const Session& session,PivotMode mode,V3 cursor) {
    Targets result;
    for(const auto actor:session.selected_actors()) {
        const auto root=world::root_component(session.world().runtime(),actor);if(!root || !*root) continue;
        const auto value=world::read_transform(session.world().runtime(),**root);if(!value) continue;
        result.components.push_back(**root);result.transforms.push_back(*value);result.pivot+=value->position;
    }
    if(result.components.empty()) return {};
    result.pivot=mode==PivotMode::cursor?cursor:result.pivot/float(result.components.size());return result;
}
ViewportTool::Handle ViewportTool::hit(const ViewportView& view,const Targets& selected,TransformTool tool,V2 p) const {
    Handle best=Handle::none;float closest=7*view.dpi_scale;
    handles(view,selected.pivot,selected.transforms.back().rotation,tool,[&](Handle handle,const Shape& shape) {
        float d;
        if(shape.kind==Shape::Kind::segment) d=distance(p,{shape.points[0],shape.points[1]},{shape.points[2],shape.points[3]});
        else d=length(p-V2{shape.points[0]+shape.points[2]*.5f,shape.points[1]+shape.points[3]*.5f})-shape.points[2]*.5f;
        if(d<=closest) {closest=d;best=handle;}
    });return best;
}
std::optional<ObjectId> ViewportTool::pick(const Session& session,const ViewportView& view,float x,float y) const {
    const auto found=trace(session,view,x,y,true);return found?std::optional(found->actor):std::nullopt;
}
std::optional<ViewportTool::Hit> ViewportTool::trace(const Session& session,const ViewportView& view,float x,float y,bool locators) {
    const auto ray=view.ray(x,y);float nearest=std::numeric_limits<float>::max();std::optional<Hit> found;
    const auto items=session.world().render_items();if(!items) return {};
    std::unordered_set<ObjectId> rendered;
    for(const auto& item:*items) {
        rendered.insert(item.actor);const auto inverse=math::try_inverse(item.world);if(!inverse) continue;
        const ViewportView::Ray local{math::transform_point(ray.origin,*inverse),math::transform_direction(ray.direction,*inverse)};
        const auto vertices=item.geometry->vertices();
        for(std::size_t i=0;i<vertices.size();i+=3) {
            const auto at=[&](std::size_t n){const auto& p=vertices[n].position;return V3{p[0],p[1],p[2]};};
            const auto t=triangle(local,at(i),at(i+1),at(i+2));
            if(t && t->distance<nearest) {
                const auto slot=item.geometry->material_slot(i/3);
                const auto& surface=(item.surfaces?(*item.surfaces)[slot]:item.geometry->materials()[slot].surface)->description();
                if(!surface.parameters.double_sided) {
                    auto normal=math::cross(at(i+1)-at(i),at(i+2)-at(i));
                    V3 authored{};
                    for(std::size_t vertex=i;vertex<i+3;++vertex) {
                        const auto& n=vertices[vertex].normal;authored+=V3{n[0],n[1],n[2]};
                    }
                    if(math::dot(normal,authored)<0)normal=-normal;
                    if(math::dot(normal,local.direction)>=0)continue;
                }
                std::array<double,2> uv{};
                for(std::size_t axis=0;axis<2;++axis)
                    uv[axis]=vertices[i].uv[axis]*(1.-t->u-t->v)+vertices[i+1].uv[axis]*t->u+vertices[i+2].uv[axis]*t->v;
                if(!material::covered(surface,uv))continue;
                const auto point=ray.origin+ray.direction*t->distance;
                if(view.project(point)) {
                    // World normal of the hit face, facing the viewer.
                    const auto edge1=math::transform_direction(at(i+1)-at(i),item.world),edge2=math::transform_direction(at(i+2)-at(i),item.world);
                    auto normal=math::try_normalize(math::cross(edge1,edge2)).value_or(V3{0,1,0});
                    if(math::dot(normal,ray.direction)>0) normal=-normal;
                    nearest=t->distance;found=Hit{item.actor,point,normal};
                }
            }
        }
    }
    if(!locators) return found;
    const auto members=session.world().members();if(!members) return found;
    for(const auto& actor:*members) if(!rendered.contains(actor.id())) {
        const auto root=world::root_component(session.world().runtime(),actor.id());if(!root || !*root) continue;
        const auto matrix=world::world_matrix(session.world().runtime(),**root);if(!matrix) continue;
        const V3 origin{(*matrix)[12],(*matrix)[13],(*matrix)[14]};const auto p=view.project(origin);
        const auto depth=math::dot(origin-ray.origin,ray.direction);
        if(p && length(*p-V2{x,y})<=8*view.dpi_scale && depth<nearest && depth>=0) {nearest=depth;found=Hit{actor.id(),origin};}
    }
    return found;
}
Result<void> ViewportTool::place_cursor(const Session& session,const ViewportView& view,V2 p,Settings settings,bool control) {
    std::optional<V3> position;V3 normal=view.construction_normal;
    if(settings.cursor_surface) if(const auto hit=trace(session,view,p.x,p.y,false)) {position=hit->position;normal=hit->normal;}
    if(!position) position=on_plane(view,p,{},view.construction_normal);
    if(!position) position=on_plane(view,p,cursor,view.forward);
    if(!position) return invalid("The cursor cannot be placed on the current construction plane.");
    if(settings.grid_snap!=control) for(int axis=0;axis<3;++axis) (*position)[axis]=std::round((*position)[axis]/settings.grid_spacing)*settings.grid_spacing;
    for(int axis=0;axis<3;++axis) if(!std::isfinite((*position)[axis]) || std::abs((*position)[axis])>10000)
        return invalid("Cursor position is outside the editor range.");
    cursor=*position;
    // Blender's "Surface Project" orientation: Z follows the surface normal, X stays
    // as close to the view's right vector as the plane allows.
    const auto z=math::try_normalize(normal).value_or(V3{0,1,0});
    auto x=math::try_normalize(view.right-z*math::dot(view.right,z));
    if(!x) x=math::try_normalize(math::cross(V3{0,0,1},z));
    const auto y=math::cross(z,*x);
    cursor_rotation=frame_rotation(*x,y,z);
    hint_=L"3D Cursor placed · Right click selects · W opens the viewport menu";return {};
}
TransformTool ViewportTool::displayed_tool(Settings settings) const noexcept {
    if(gesture_) return gesture_->operation==Operation::move?TransformTool::move:gesture_->operation==Operation::rotate?TransformTool::rotate:TransformTool::scale;
    return marquee_?TransformTool::select:settings.tool;
}
void ViewportTool::cursor_to_grid(Settings settings) noexcept {
    for(int axis=0;axis<3;++axis) cursor[axis]=std::round(cursor[axis]/settings.grid_spacing)*settings.grid_spacing;
}
Result<void> ViewportTool::cursor_to_selection(const Session& session) {
    const auto selected=targets(session,PivotMode::median,cursor);if(!selected) return invalid("Select an Actor with a spatial root.");
    cursor=selected->pivot;return {};
}
Result<void> ViewportTool::snap_selection(Session& session,Settings settings,bool to_cursor) {
    const auto selected=targets(session,PivotMode::median,cursor);if(!selected) return invalid("Select an Actor with a spatial root.");
    auto values=selected->transforms;
    for(auto& value:values) {
        if(to_cursor) value.position+=cursor-selected->pivot;
        else for(int axis=0;axis<3;++axis) value.position[axis]=std::round(value.position[axis]/settings.grid_spacing)*settings.grid_spacing;
    }
    const auto token=next_session_++;auto result=session.begin_transform(selected->components,token);if(!result) return result;
    result=session.preview_transform(token,values);
    if(result) return session.commit_edit(token);
    const auto cancelled=session.cancel_edit();return cancelled?result:cancelled;
}
std::vector<ObjectId> ViewportTool::pick_box(const Session& session,const ViewportView& view,ui::Rect box) const {
    std::vector<ObjectId> found;const auto items=session.world().render_items();if(!items) return found;
    const auto intersects=[&](float left,float top,float right,float bottom) {
        return left<=box.x+box.width && right>=box.x && top<=box.y+box.height && bottom>=box.y;
    };
    for(const auto& item:*items) {
        if(std::ranges::find(found,item.actor)!=found.end()) continue;
        float left=std::numeric_limits<float>::max(),top=left,right=-left,bottom=-left;
        for(const auto& vertex:item.geometry->vertices()) {
            const auto& p=vertex.position;const auto projected=view.project(math::transform_point(V3{p[0],p[1],p[2]},item.world));
            if(!projected) continue;
            left=std::min(left,projected->x);right=std::max(right,projected->x);top=std::min(top,projected->y);bottom=std::max(bottom,projected->y);
        }
        if(left<=right && intersects(left,top,right,bottom)) found.push_back(item.actor);
    }
    const auto members=session.world().members();if(!members) return found;
    for(const auto& actor:*members) {
        if(std::ranges::find(found,actor.id())!=found.end()) continue;
        const auto root=world::root_component(session.world().runtime(),actor.id());if(!root || !*root) continue;
        const auto transform=world::read_transform(session.world().runtime(),**root);if(!transform) continue;
        const auto p=view.project(transform->position);if(p && intersects(p->x,p->y,p->x,p->y)) found.push_back(actor.id());
    }
    return found;
}
Result<void> ViewportTool::select_click(Session& session,const ViewportView& view,V2 p,bool shift,bool control) {
    const auto object=pick(session,view,p.x,p.y);std::vector<ObjectId> selected;
    if(shift || control) selected.assign(session.selected_actors().begin(),session.selected_actors().end());
    if(object) {
        if(control || (shift && std::ranges::find(selected,*object)!=selected.end())) std::erase(selected,*object);
        else selected.push_back(*object);
    }
    return session.select_actors(selected,object.value_or(ObjectId{}));
}
Result<void> ViewportTool::select_box(Session& session,const ViewportView& view,const Marquee& marquee) {
    const auto a=marquee.start,b=marquee.current;
    const auto hits=pick_box(session,view,{std::min(a.x,b.x),std::min(a.y,b.y),std::abs(a.x-b.x),std::abs(a.y-b.y)});
    std::vector<ObjectId> selected;
    if(marquee.shift || marquee.control) selected.assign(session.selected_actors().begin(),session.selected_actors().end());
    for(const auto actor:hits) {
        if(marquee.control) std::erase(selected,actor);
        else if(std::ranges::find(selected,actor)==selected.end()) selected.push_back(actor);
    }
    return session.select_actors(selected,selected.empty()?ObjectId{}:selected.back());
}
Result<void> ViewportTool::place_duplicates(Session& session,const ViewportView& view,Settings settings,V2 pointer) {
    if (!targets(session,settings.pivot,cursor)) return {}; // Nonspatial Actors still duplicate normally.
    return begin(session,view,settings,Operation::move,Handle::none,true,pointer,true);
}
Result<void> ViewportTool::begin(Session& session,const ViewportView&,Settings settings,Operation operation,Handle handle,bool modal,V2 p,bool amend_creation) {
    auto selected=targets(session,settings.pivot,cursor);if(!selected) return invalid("Select an Actor with a spatial root.");
    const auto token=next_session_++;
    if(auto begun=session.begin_transform(selected->components,token,amend_creation);!begun) return begun;
    Gesture gesture;gesture.operation=operation;gesture.handle=handle;gesture.modal=modal;gesture.session=token;gesture.settings=settings;
    gesture.before=std::move(selected->transforms);gesture.pivot=selected->pivot;
    gesture.start=gesture.last=p;
    if(handle>=Handle::move_x && handle<=Handle::move_z) gesture.axis=int(handle)-int(Handle::move_x);
    if(handle>=Handle::rotate_x && handle<=Handle::rotate_z) gesture.axis=int(handle)-int(Handle::rotate_x);
    if(handle>=Handle::scale_x && handle<=Handle::scale_z) gesture.axis=int(handle)-int(Handle::scale_x);
    if(handle>=Handle::plane_yz && handle<=Handle::plane_xy) {gesture.axis=int(handle)-int(Handle::plane_yz);gesture.plane=true;}
    gesture_=std::move(gesture);hint_=L"X/Y/Z constrain · Ctrl snap · Enter confirm · Esc cancel";return {};
}
Result<void> ViewportTool::update(Session& session,const ViewportView& view,V2 p,bool snap,bool fine) {
    auto& g=*gesture_;g.valid=false;std::optional<float> typed;
    if(!g.typed.empty()) {
        const std::string text(g.typed.begin(),g.typed.end());float value{};
        const auto parsed=std::from_chars(text.data(),text.data()+text.size(),value);
        if(parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size() || !std::isfinite(value)) {
            hint_=L"Value: "+g.typed+L" · Enter a complete number / Esc cancel";return {};
        }
        typed=value;
    }
    const float precision=fine?.1f:1.f;std::vector<world::Trs> values=g.before;
    std::wstring readout; // Blender header text, e.g. "D: 0.95 m (0.95 m) along global Z".
    const auto number=[](double value,int digits) {
        wchar_t text[48];std::swprintf(text,48,L"%.*f",digits,value);return std::wstring(text);
    };
    const auto basis=[&]{
        if(g.axis<0) return std::wstring{};
        return std::wstring(g.plane?L" locking global ":L" along global ")+L"XYZ"[g.axis];
    };
    if(g.operation==Operation::move) {
        V3 delta{};
        if(g.axis>=0 && !g.plane) {
            const auto axis=axes[g.axis];
            const auto normal=math::try_normalize(view.forward-axis*math::dot(view.forward,axis));
            if(normal) {
                const auto a=on_plane(view,g.start,g.pivot,*normal),b=on_plane(view,p,g.pivot,*normal);
                if(a&&b) delta=axis*math::dot(*b-*a,axis);
            }
            if(typed) delta=axis**typed;
        } else {
            const auto normal=g.plane?axes[g.axis]:view.forward;
            const auto a=on_plane(view,g.start,g.pivot,normal),b=on_plane(view,p,g.pivot,normal);
            if(a&&b) delta=*b-*a;
            if(typed) delta=math::try_normalize(delta).value_or(g.plane?axes[(g.axis+1)%3]:view.right)**typed;
        }
        if(!typed) delta*=precision;
        if((g.settings.grid_snap!=snap) && !typed) for(int axis=0;axis<3;++axis) {
            if(g.axis>=0 && (g.plane?axis==g.axis:axis!=g.axis)) continue;
            const float step=g.settings.grid_spacing,origin=g.before.back().position[axis];
            delta[axis]=std::round((origin+delta[axis])/step)*step-origin;
        }
        for(auto& value:values) value.position+=delta;
        if(g.axis>=0 && !g.plane) {
            const auto d=delta[g.axis];readout=L"D: "+number(d,4)+L" m ("+number(d,4)+L" m)"+basis();
        } else readout=L"Dx: "+number(delta.x,4)+L" m   Dy: "+number(delta.y,4)+L" m   Dz: "+number(delta.z,4)+L" m ("+number(math::length(delta),4)+L" m)"+basis();
    } else if(g.operation==Operation::rotate) {
        const auto axis=g.axis>=0?axes[g.axis]:-view.forward;
        const auto a=on_plane(view,g.last,g.pivot,axis),b=on_plane(view,p,g.pivot,axis);
        if(a&&b) {
            const auto u=math::try_normalize(*a-g.pivot),v=math::try_normalize(*b-g.pivot);
            if(u&&v) g.angle+=std::atan2(math::dot(axis,math::cross(*u,*v)),math::dot(*u,*v));
        } else g.angle+=(p.x-g.last.x)*.01f;
        float angle=typed?*typed*pi/180:g.angle*precision;
        if(snap && !typed) angle=std::round(angle/(pi/12))*(pi/12);
        const auto q=rotation(axis,angle);
        for(auto& value:values) {value.position=g.pivot+math::rotate(q,value.position-g.pivot);value.rotation=q*value.rotation;}
        readout=L"Rotation: "+number(angle*180/pi,2)+(g.axis>=0?basis():std::wstring{});
    } else {
        float amount=1+(p.x-g.start.x-(p.y-g.start.y))*.007f*precision;
        const auto orientation=g.before.back().rotation;
        if(g.axis>=0 && !g.plane) {
            const auto center=view.project(g.pivot);
            const auto tip=view.project(g.pivot+math::rotate(orientation,axes[g.axis])*(84*view.pixel_size(g.pivot)*view.dpi_scale));
            if(center && tip) {const auto direction=*tip-*center;const auto square=math::dot(direction,direction);
                if(square>1) amount=1+math::dot(p-g.start,direction)/square*precision;}
        }
        if(typed) amount=*typed;
        else if(snap) amount=std::round(amount*10)/10;
        if(amount<=0) return invalid("Scale must stay positive.");
        V3 factors{1,1,1};
        for(int axis=0;axis<3;++axis) if(g.axis<0 || (g.plane?axis!=g.axis:axis==g.axis)) factors[axis]=amount;
        for(auto& value:values) {
            auto offset=math::rotate(math::conjugate(orientation),value.position-g.pivot);
            for(int axis=0;axis<3;++axis) {value.scale[axis]*=factors[axis];offset[axis]*=factors[axis];}
            value.position=g.pivot+math::rotate(orientation,offset);
        }
        // Blender: "Scale X: 1.8273   Y: 1.8273  Z: 1.8273", or "Scale: 1.8273 along global X".
        readout=g.axis>=0 && !g.plane?L"Scale: "+number(amount,4)+basis():
            L"Scale X: "+number(factors.x,4)+L"   Y: "+number(factors.y,4)+L"  Z: "+number(factors.z,4)+basis();
    }
    g.last=p;
    auto changed=session.preview_transform(g.session,values);if(!changed) return changed;g.valid=true;
    // Typed input shows the raw entry in brackets like Blender: "[2|]".
    hint_=g.typed.empty()?readout:readout+L"  ["+g.typed+L"|]";return {};
}
Result<void> ViewportTool::clear_channel(Session& session,Operation channel) {
    const auto selected=targets(session,PivotMode::median,cursor);if(!selected) return invalid("Select an Actor with a spatial root.");
    auto values=selected->transforms;
    for(auto& value:values) {
        if(channel==Operation::move) value.position={};
        else if(channel==Operation::rotate) value.rotation={};
        else value.scale={1,1,1};
    }
    const auto token=next_session_++;auto result=session.begin_transform(selected->components,token);if(!result) return result;
    result=session.preview_transform(token,values);
    if(result) result=session.commit_edit(token);
    if(!result) {const auto cancelled=session.cancel_edit();return cancelled?result:cancelled;}
    hint_=channel==Operation::move?L"Clear Location":channel==Operation::rotate?L"Clear Rotation":L"Clear Scale";return {};
}
Result<void> ViewportTool::finish(Session& session) {
    if(!gesture_) return {};
    if(!gesture_->valid) return invalid("Correct the invalid transform value before confirming, or press Esc to cancel.");
    auto result=session.commit_edit(gesture_->session);if(!result) return result;
    gesture_.reset();hover_=Handle::none;hint_.clear();return {};
}
Result<void> ViewportTool::cancel(Session& session) {
    if(gesture_ && session.owns_edit(gesture_->session)) {
        auto result=session.cancel_edit();if(!result) return result;
    }
    gesture_.reset();marquee_.reset();hover_=Handle::none;hint_.clear();return {};
}
void ViewportTool::forget(const Session& session) noexcept {
    if(gesture_ && !session.owns_edit(gesture_->session)) {gesture_.reset();hover_=Handle::none;hint_.clear();}
}
ui::Cursor ViewportTool::pointer_cursor() const noexcept {return active() || hover_!=Handle::none?ui::Cursor::crosshair:ui::Cursor::arrow;}
Result<bool> ViewportTool::pointer(Session& session,const ViewportView& view,const ui::Event& event,Settings settings) {
    const V2 p{event.x,event.y};
    if(event.kind==ui::EventKind::pointer_down && event.button==ui::PointerButton::secondary && (gesture_ || (marquee_ && marquee_->await_press))) {
        auto result=cancel(session);if(!result) return std::unexpected(result.error());return true;
    }
    if(gesture_) {
        Result<void> result;
        if(event.kind==ui::EventKind::pointer_down && event.button==ui::PointerButton::middle && gesture_->modal) {
            // Blender: middle click during G/R/S constrains to the world axis whose
            // screen direction best matches the pointer's travel since the start.
            const auto travel=p-gesture_->start;const auto center=view.project(gesture_->pivot);
            int best=-1;float score=0;
            if(center && length(travel)>2*view.dpi_scale) for(int axis=0;axis<3;++axis) {
                const auto tip=view.project(gesture_->pivot+axes[axis]*view.pixel_size(gesture_->pivot)*view.dpi_scale*60);if(!tip) continue;
                const auto direction=math::try_normalize(*tip-*center);if(!direction) continue;
                const float match=std::abs(math::dot(*direction,travel/length(travel)));
                if(match>score) {score=match;best=axis;}
            }
            gesture_->axis=best;gesture_->plane=false;gesture_->angle=0;gesture_->last=gesture_->start;
            result=update(session,view,p,event.control,event.shift);
            if(!result) return std::unexpected(result.error());return true;
        }
        if(event.kind==ui::EventKind::pointer_move) result=update(session,view,p,event.control,event.shift);
        else if((event.kind==ui::EventKind::pointer_up && !gesture_->modal && event.button==ui::PointerButton::primary) ||
            (event.kind==ui::EventKind::pointer_down && gesture_->modal && event.button==ui::PointerButton::primary)) result=finish(session);
        if(!result) return std::unexpected(result.error());return true;
    }
    if(marquee_) {
        if(event.kind==ui::EventKind::pointer_down && event.button==marquee_->button && marquee_->await_press) {
            if(!view.contains(p.x,p.y)) return true;
            marquee_->start=marquee_->current=p;marquee_->await_press=false;
        }
        if(event.kind==ui::EventKind::pointer_move && !marquee_->await_press) {
            marquee_->current={std::clamp(p.x,view.bounds.x,view.bounds.x+view.bounds.width),std::clamp(p.y,view.bounds.y,view.bounds.y+view.bounds.height)};
            marquee_->dragging|=length(marquee_->current-marquee_->start)>=4*view.dpi_scale;
        }
        if(event.kind==ui::EventKind::pointer_up && event.button==marquee_->button && !marquee_->await_press) {
            const auto result=marquee_->dragging?select_box(session,view,*marquee_):select_click(session,view,p,marquee_->shift,marquee_->control);
            marquee_.reset();hint_.clear();if(!result) return std::unexpected(result.error());
        }
        return true;
    }
    if(!view.contains(p.x,p.y)) {hover_=Handle::none;return false;}
    if(event.kind==ui::EventKind::pointer_down && event.button==ui::PointerButton::secondary) {
        marquee_=Marquee{p,p,false,event.shift,event.control,false,ui::PointerButton::secondary};
        return true;
    }
    if(event.kind==ui::EventKind::pointer_move || (event.kind==ui::EventKind::pointer_down && event.button==ui::PointerButton::primary)) {
        const auto selected=targets(session,settings.pivot,cursor);hover_=selected?hit(view,*selected,settings.tool,p):Handle::none;
        if(event.kind==ui::EventKind::pointer_move) return false;
        if(hover_!=Handle::none && !event.shift && !event.control) {
            const auto operation=hover_>=Handle::scale_x?Operation::scale:hover_>=Handle::rotate_x?Operation::rotate:Operation::move;
            const auto result=begin(session,view,settings,operation,hover_,false,p);if(!result) return std::unexpected(result.error());
        } else {
            const auto placed=place_cursor(session,view,p,settings,event.control);if(!placed) return std::unexpected(placed.error());
        }
        return true;
    }
    return false;
}
Result<bool> ViewportTool::key(Session& session,const ViewportView& view,const ui::Event& event,std::uint32_t vk,Settings settings,V2 pointer) {
    if(active() && event.kind==ui::EventKind::key_down && event.key==ui::Key::escape) {
        const auto result=cancel(session);if(!result) return std::unexpected(result.error());return true;
    }
    if(gesture_) {
        if(event.kind==ui::EventKind::text) {
            if(const auto* text=std::get_if<std::wstring>(&event.data)) for(const auto c:*text) if((c>=L'0' && c<=L'9') || c==L'.' || c==L'-') {
                if(gesture_->typed.size()<24) gesture_->typed+=c;
            }
            const auto result=update(session,view,pointer,event.control);if(!result) return std::unexpected(result.error());return true;
        }
        if(event.kind!=ui::EventKind::key_down) return true;
        if(event.key==ui::Key::enter) {const auto result=finish(session);if(!result) return std::unexpected(result.error());return true;}
        if(!event.control && !event.alt && (vk=='X' || vk=='Y' || vk=='Z')) {
            const int axis=int(vk-'X');gesture_->axis=gesture_->axis==axis && gesture_->plane==event.shift?-1:axis;
            gesture_->plane=gesture_->axis>=0 && event.shift;gesture_->angle=0;gesture_->last=gesture_->start;
        } else if(event.key==ui::Key::backspace && !gesture_->typed.empty()) gesture_->typed.pop_back();
        const auto result=update(session,view,pointer,event.control);if(!result) return std::unexpected(result.error());return true;
    }
    if(event.kind!=ui::EventKind::key_down) return false;
    const auto members=[&]()->Result<std::vector<ObjectId>> {
        std::vector<ObjectId> all;const auto items=session.world().members();if(!items) return std::unexpected(items.error());
        for(const auto& actor:*items) all.push_back(actor.id());return all;
    };
    // Blender object mode: Alt+A deselects, Ctrl+I inverts, Alt+G/R/S clear one TRS
    // channel of every selected root as a single history entry.
    if(event.alt && !event.control && vk=='A') {
        const auto result=session.select_actors({},{});if(!result) return std::unexpected(result.error());hint_=L"Deselect All";return true;
    }
    if(event.control && !event.alt && vk=='I') {
        auto all=members();if(!all) return std::unexpected(all.error());
        std::erase_if(*all,[&](ObjectId actor){return session.is_selected(actor);});
        const auto result=session.select_actors(*all,all->empty()?ObjectId{}:all->back());if(!result) return std::unexpected(result.error());
        hint_=L"Invert Selection";return true;
    }
    if(event.alt && !event.control && (vk=='G' || vk=='R' || vk=='S')) {
        const auto result=clear_channel(session,vk=='G'?Operation::move:vk=='R'?Operation::rotate:Operation::scale);
        if(!result) return std::unexpected(result.error());return true;
    }
    if(event.control || event.alt) return false;
    if(vk=='G' || vk=='R' || vk=='S') {
        const auto result=begin(session,view,settings,vk=='G'?Operation::move:vk=='R'?Operation::rotate:Operation::scale,Handle::none,true,pointer);
        if(!result) return std::unexpected(result.error());return true;
    }
    if(vk=='B') {marquee_=Marquee{pointer,pointer,false,event.shift,false,true};hint_=L"Box select · Drag to select · Esc cancel";return true;}
    if(vk=='A') {
        // Blender 2.8+ keymap: A selects all; A again with everything selected clears.
        auto all=members();if(!all) return std::unexpected(all.error());
        const bool everything=!all->empty() && std::ranges::all_of(*all,[&](ObjectId actor){return session.is_selected(actor);});
        if(everything) all->clear();
        const auto result=session.select_actors(*all,all->empty()?ObjectId{}:all->back());
        if(!result) return std::unexpected(result.error());hint_=everything?L"Deselect All":L"Select All";return true;
    }
    return false;
}
void ViewportTool::overlay(const Session& session,const ViewportView& view,Settings settings,std::vector<Shape>& out) const {
    const auto stroke=[&](V2 a,V2 b,float width,ui::Color color){out.push_back({Shape::Kind::segment,{a.x,a.y,b.x,b.y},width*view.dpi_scale,color});};
    const auto items=session.world().render_items();std::unordered_set<ObjectId> rendered;
    if(items) for(const auto& item:*items) rendered.insert(item.actor);
    const auto members=session.world().members();
    if(members) for(const auto& actor:*members) if(!rendered.contains(actor.id())) {
        const auto root=world::root_component(session.world().runtime(),actor.id());if(!root || !*root) continue;
        const auto matrix=world::world_matrix(session.world().runtime(),**root);if(!matrix) continue;
        const auto p=view.project({(*matrix)[12],(*matrix)[13],(*matrix)[14]});if(!p) continue;
        const float r=6*view.dpi_scale;const ui::Color color=session.is_selected(actor.id())?picked:srgb(0xd8,0xdc,0xe0);
        stroke(*p-V2{r,0},*p+V2{r,0},1.5f,color);stroke(*p-V2{0,r},*p+V2{0,r},1.5f,color);
    }
    if(const auto selected=targets(session,settings.pivot,cursor))
        handles(view,selected->pivot,selected->transforms.back().rotation,settings.tool,[&](Handle handle,Shape shape){
            if(handle==hover_ || (gesture_ && handle==gesture_->handle)) shape.color=picked;out.push_back(shape);
        });
    if(gesture_) {
        // Blender modal feedback: the active constraint as an infinite world line
        // through the pivot, and a dashed rubber band from the pivot to the pointer.
        const auto center=view.project(gesture_->pivot);
        if(gesture_->axis>=0 && center) {
            const auto draw_axis=[&](int axis) {
                const auto direction=axes[axis];const float reach=view.pixel_size(gesture_->pivot)*view.dpi_scale*4000;
                const auto a=view.project(gesture_->pivot-direction*reach),b=view.project(gesture_->pivot+direction*reach);
                const auto color=colors[axis];
                if(a && b) stroke(*a,*b,1.5f,{color.r,color.g,color.b,.9f});
                else if(const auto tip=view.project(gesture_->pivot+direction*(reach*.01f))) {
                    const auto d=math::try_normalize(*tip-*center);
                    if(d) stroke(*center-*d*4000.f,*center+*d*4000.f,1.5f,{color.r,color.g,color.b,.9f});
                }
            };
            if(gesture_->plane) {for(int axis=0;axis<3;++axis) if(axis!=gesture_->axis) draw_axis(axis);}
            else draw_axis(gesture_->axis);
        }
        if(center && gesture_->modal && gesture_->operation!=Operation::move) {
            const auto d=gesture_->last-*center;const float total=length(d);const float dash=6*view.dpi_scale;
            for(float at=0;at<total;at+=dash*2) {
                const auto a=*center+d*(at/total),b=*center+d*(std::min(at+dash,total)/total);
                stroke(a,b,1.f,{0,0,0,.85f});
            }
        }
    }
    if(const auto p=view.project(cursor)) {
        // Blender 4.x 3D cursor, measured at 1x: 10 px dashed red/white ring and
        // four black ticks crossing it along the view's screen axes.
        const float scale=view.dpi_scale,r=10*scale;
        for(int segment=0;segment<48;++segment) {
            // 16 alternating dashes, each drawn as three short chords.
            const auto point=[&](int n){const auto angle=2*pi*float(n)/48;return *p+V2{std::cos(angle),std::sin(angle)}*r;};
            stroke(point(segment),point(segment+1),1.5f,(segment/3)%2?ui::Color{1,1,1,1}:ui::Color{1,0,0,1});
        }
        // Ticks are the cursor frame's X and Y axes in 3D, so a surface-aligned
        // cursor visibly lies on that surface. The ring stays screen-facing.
        const float metres=view.pixel_size(cursor)*scale;
        for(const auto local:{V3{1,0,0},V3{0,1,0}}) {
            const auto axis=math::rotate(cursor_rotation,local);
            const auto tip=view.project(cursor+axis*(metres*10));if(!tip) continue;
            auto direction=math::try_normalize(*tip-*p);if(!direction) continue;
            for(const float sign:{1.f,-1.f})
                stroke(*p+*direction*(sign*r*.55f),*p+*direction*(sign*r*1.85f),1.25f,{0,0,0,1});
        }
    }
    {
        // Houdini orientation triad: fixed lower-left corner, world axes projected
        // through the camera rotation only, coloured discs with dark axis letters.
        const float scale=view.dpi_scale,arm=22*scale,disc=7.5f*scale;
        // Above the tool hint line, as in Houdini.
        const V2 origin{view.bounds.x+40*scale,view.bounds.y+view.bounds.height-62*scale};
        struct Arm { int axis; V2 tip; float depth; };
        std::array<Arm,3> arms{};
        for(int axis=0;axis<3;++axis) {
            const auto a=axes[axis];
            arms[axis]={axis,origin+V2{math::dot(a,view.right),-math::dot(a,view.up)}*arm,math::dot(a,view.forward)};
        }
        // Painter's order: the axis pointing away from the viewer is drawn first.
        std::ranges::sort(arms,std::greater{},&Arm::depth);
        const std::array discs{srgb(0xe1,0x67,0x67),srgb(0x66,0xe1,0x6a),srgb(0x5b,0x81,0xdb)};
        const std::array lines{srgb(0xf2,0x40,0x40),srgb(0x40,0xf2,0x40),srgb(0x33,0x66,0xe5)};
        for(const auto& item:arms) {
            stroke(origin,item.tip,1.5f,lines[item.axis]);
            stroke(item.tip,item.tip,disc*2/scale,discs[item.axis]);
            // Axis letters: X as a cross, Y as a fork, Z as a zig-zag; 6 px glyph box.
            const float g=3*scale;const auto c=item.tip;const ui::Color ink{0,0,0,1};
            if(item.axis==0) {stroke(c+V2{-g,-g},c+V2{g,g},1.6f,ink);stroke(c+V2{g,-g},c+V2{-g,g},1.6f,ink);}
            else if(item.axis==1) {stroke(c+V2{-g,-g},c,1.6f,ink);stroke(c+V2{g,-g},c,1.6f,ink);stroke(c,c+V2{0,g},1.6f,ink);}
            else {stroke(c+V2{-g,-g},c+V2{g,-g},1.6f,ink);stroke(c+V2{g,-g},c+V2{-g,g},1.6f,ink);stroke(c+V2{-g,g},c+V2{g,g},1.6f,ink);}
        }
    }
    if(marquee_ && marquee_->dragging) {
        const auto a=marquee_->start,b=marquee_->current;
        out.push_back({Shape::Kind::rectangle,{std::min(a.x,b.x),std::min(a.y,b.y),std::abs(a.x-b.x),std::abs(a.y-b.y)},view.dpi_scale,
            {.35f,.55f,.72f,.15f},{.65f,.8f,.9f,1}});
    }
}
}
