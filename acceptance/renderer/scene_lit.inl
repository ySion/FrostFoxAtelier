// This fixture submits ordinary snapshots to the product Renderer. Captures are
// used only for image evidence; the benchmark samples completed GPU timestamps.
namespace scene_lit_e2e {
using Vec3 = std::array<float, 3>;
constexpr unsigned warmup_frames = 96;
// Temporal filters (DI history, DDGI transport) keep estimator noise correlated
// over tens of frames, so converged appearances compare means of this window.
constexpr unsigned convergence_frames = 64;

void png(const render::Capture& image, const std::filesystem::path& path) {
    // PNG's uncompressed DEFLATE blocks keep evidence dependency-free. Encoding
    // is outside all measured frames and preserves the exact captured RGBA8.
    std::ofstream output(path, std::ios::binary);
    const auto integer = [](std::vector<std::uint8_t>& bytes, std::uint32_t value) {
        for (int shift : {24, 16, 8, 0}) bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    };
    const auto chunk = [&](const char* name, const std::vector<std::uint8_t>& payload) {
        std::vector<std::uint8_t> bytes;
        integer(bytes, static_cast<std::uint32_t>(payload.size()));
        bytes.insert(bytes.end(), name, name + 4);
        bytes.insert(bytes.end(), payload.begin(), payload.end());
        std::uint32_t crc = ~0u;
        for (std::size_t index = 4; index < bytes.size(); ++index) {
            crc ^= bytes[index];
            for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
        }
        integer(bytes, ~crc);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    };
    constexpr std::uint8_t signature[]{137, 80, 78, 71, 13, 10, 26, 10};
    output.write(reinterpret_cast<const char*>(signature), sizeof(signature));
    std::vector<std::uint8_t> header;
    integer(header, image.extent.width); integer(header, image.extent.height);
    header.insert(header.end(), {8, 6, 0, 0, 0}); chunk("IHDR", header);
    std::vector<std::uint8_t> raw;
    raw.reserve(image.rgba.size() + image.extent.height);
    for (unsigned y = 0; y < image.extent.height; ++y) {
        raw.push_back(0);
        const auto begin = image.rgba.begin() + std::size_t(y) * image.extent.width * 4;
        raw.insert(raw.end(), begin, begin + image.extent.width * 4);
    }
    std::vector<std::uint8_t> compressed{0x78, 0x01};
    for (std::size_t offset = 0; offset < raw.size();) {
        const auto count = static_cast<std::uint16_t>(std::min<std::size_t>(65535, raw.size() - offset));
        compressed.push_back(offset + count == raw.size() ? 1 : 0);
        compressed.push_back(static_cast<std::uint8_t>(count));
        compressed.push_back(static_cast<std::uint8_t>(count >> 8));
        compressed.push_back(static_cast<std::uint8_t>(~count));
        compressed.push_back(static_cast<std::uint8_t>((~count) >> 8));
        compressed.insert(compressed.end(), raw.begin() + offset, raw.begin() + offset + count);
        offset += count;
    }
    std::uint32_t a = 1, b = 0;
    for (auto value : raw) { a = (a + value) % 65521; b = (b + a) % 65521; }
    integer(compressed, (b << 16) | a); chunk("IDAT", compressed); chunk("IEND", {});
    check(output.good(), "SceneLit PNG evidence written");
}

Vec3 subtract(Vec3 a, Vec3 b) { return {a[0]-b[0], a[1]-b[1], a[2]-b[2]}; }
float dot(Vec3 a, Vec3 b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
Vec3 cross(Vec3 a, Vec3 b) { return {a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]}; }
Vec3 normalize(Vec3 value) {
    const float length = std::sqrt(dot(value, value));
    return {value[0]/length, value[1]/length, value[2]/length};
}
render::Camera camera(ui::PixelExtent extent, bool orthographic = false) {
    const Vec3 eye{0, 3, -7}, focus{0, .7f, 3};
    const auto z = normalize(subtract(focus, eye));
    const auto x = normalize(cross({0, 1, 0}, z));
    const auto y = cross(z, x);
    render::Camera result;
    result.view = {x[0],y[0],z[0],0, x[1],y[1],z[1],0, x[2],y[2],z[2],0,
                   -dot(x,eye),-dot(y,eye),-dot(z,eye),1};
    const float aspect = float(extent.width) / float(extent.height);
    if (orthographic)
        result.projection = {1/(3.8f*aspect),0,0,0, 0,1/3.8f,0,0, 0,0,-1/39.9f,0, 0,0,40/39.9f,1};
    else
        result.projection = {1.8f/aspect,0,0,0, 0,1.8f,0,0, 0,0,-.1f/39.9f,1, 0,0,4/39.9f,0};
    return result;
}
std::array<float, 16> box(float x, float y, float z, float sx, float sy, float sz) {
    return {sx,0,0,0, 0,sy,0,0, 0,0,sz,0, x,y,z,1};
}

struct Room {
    std::vector<render::MeshInstance> meshes;
    Room() {
        const auto add = [&](std::array<float, 16> transform, ui::Color color) {
            render::MeshInstance mesh;
            mesh.geometry = atelier::geometry::cube_mesh();
            mesh.world = transform; mesh.color = color;
            meshes.push_back(std::move(mesh));
        };
        add(box(0,-.15f,3.5f,8,.3f,7), {.62f,.62f,.62f,1}); // floor
        add(box(0,2,7,8,4,.2f), {.62f,.62f,.62f,1});          // back receiver
        add(box(-4,2,3.5f,.2f,4,7), {.8f,.06f,.025f,1});    // warm bounce wall
        add(box(4,2,3.5f,.2f,4,7), {.025f,.1f,.8f,1});       // cool bounce wall
        add(box(0,.8f,3,1.1f,1.6f,1.1f), {.48f,.48f,.48f,1}); // common occluder
        add(box(-2.1f,.55f,2,1.1f,1.1f,1.1f), {.7f,.65f,.5f,1}); // rough metal
        add(box(2.1f,.55f,2,1.1f,1.1f,1.1f), {.7f,.65f,.5f,1});  // smooth metal
        add(box(0,2.75f,6.7f,1.6f,.6f,.1f), {0,0,0,1});       // RGB HDR emission
        for (std::size_t index = 0; index < meshes.size(); ++index) meshes[index].stable_key = index + 1;
        meshes[0].material.roughness = .18f;
        meshes[0].material.metallic = .65f;
        meshes[5].material = {.roughness=.75f, .metallic=1};
        meshes[6].material = {.roughness=.08f, .metallic=1};
        meshes[7].material.emissive = {4,.3f,.05f};
    }
};
struct Region { unsigned left{}, top{}, right{}, bottom{}; float expected_depth{}; };
Region project_region(const render::Camera& camera, ui::Rect viewport, ui::PixelExtent extent, Vec3 point, unsigned radius = 5) {
    std::array<float, 4> value{point[0], point[1], point[2], 1};
    for (const auto& matrix : {camera.view, camera.projection}) {
        std::array<float, 4> next{};
        for (unsigned column = 0; column < 4; ++column)
            for (unsigned row = 0; row < 4; ++row) next[column] += value[row] * matrix[row*4+column];
        value = next;
    }
    const auto x = static_cast<int>(viewport.x+(value[0]/value[3]*.5f+.5f)*viewport.width);
    const auto y = static_cast<int>(viewport.y+(.5f-value[1]/value[3]*.5f)*viewport.height);
    check(x > int(radius) && y > int(radius) && x + int(radius) < int(extent.width) &&
              y + int(radius) < int(extent.height), "SceneLit world-space probe projects inside the capture");
    return {unsigned(x)-radius,unsigned(y)-radius,unsigned(x)+radius+1,unsigned(y)+radius+1,value[2]/value[3]};
}
std::array<double, 3> mean_rgb(const render::Capture& image, Region region) {
    // Having geometry at the projected pixel does not prove that this receiver
    // is visible: an intervening metallic object has no diffuse GI response.
    check(std::abs(depth(image,(region.left+region.right)/2,(region.top+region.bottom)/2)-region.expected_depth)<2e-4f,
          "SceneLit receiver probe depth matches its intended world-space surface");
    std::array<double, 3> result{};
    unsigned count = 0;
    for (unsigned y = region.top; y < region.bottom; ++y)
        for (unsigned x = region.left; x < region.right; ++x) {
            check(depth(image, x, y) > 0, "SceneLit color probe samples a visible receiver");
            for (unsigned channel = 0; channel < 3; ++channel) result[channel] += pixel(image, x, y)[channel];
            ++count;
        }
    for (auto& channel : result) channel /= count;
    return result;
}

struct Difference {
    double mean{};
    unsigned p95{};
    std::size_t changed{};
    std::size_t darker{};
};
Difference difference(const render::Capture& a, const render::Capture& b, bool scene_only = true) {
    check(a.extent == b.extent && a.depth.size() == b.depth.size(), "SceneLit comparison extents match");
    std::vector<unsigned> deltas;
    Difference result;
    for (std::size_t i = 0; i < a.depth.size(); ++i) {
        if (scene_only && (a.depth[i] == 0 || b.depth[i] == 0 || std::abs(a.depth[i] - b.depth[i]) > 1e-5f)) continue;
        unsigned delta = 0;
        int total_a = 0, total_b = 0;
        for (unsigned channel = 0; channel < 3; ++channel) {
            delta = std::max(delta, unsigned(std::abs(int(a.rgba[i*4+channel])-int(b.rgba[i*4+channel]))));
            total_a += a.rgba[i*4+channel]; total_b += b.rgba[i*4+channel];
        }
        result.mean += delta;
        result.changed += delta >= 4;
        result.darker += total_b + 12 < total_a;
        deltas.push_back(delta);
    }
    check(!deltas.empty(), "SceneLit comparison contains actual geometry pixels");
    result.mean /= static_cast<double>(deltas.size());
    std::ranges::sort(deltas);
    result.p95 = deltas[(deltas.size()-1)*95/100];
    return result;
}
double percentile(std::vector<double> values, unsigned percent) {
    check(!values.empty(), "SceneLit completed GPU sample set is nonempty");
    std::ranges::sort(values);
    return values[(values.size()-1)*percent/100];
}

// Implementation-independent radiometry on the plane z=plane_z facing -z, seen
// through a translation-only view. Surface response per unit incident radiance
// and solid angle: Lambert diffuse weighted by (1-F) plus GGX / separable Smith /
// Schlick specular, F0 = lerp(0.04, base, metallic). Display: exposure 1,
// rational filmic curve, sRGB. A receiver pixel must lie within one 8-bit code
// (display dither) of the band that two HDR half-precision roundings allow.
using Rgb3 = std::array<double,3>;
struct PlaneResponse { Rgb3 diffuse{}, specular{}; };
PlaneResponse plane_response(const Rgb3& v,const Rgb3& l,const Rgb3& base,double roughness,double metallic) {
    PlaneResponse result;
    const double nl=-l[2],nv=std::max(-v[2],1e-4);
    if(nl<=0)return result;
    const Rgb3 h{v[0]+l[0],v[1]+l[1],v[2]+l[2]};
    const double hl=std::sqrt(h[0]*h[0]+h[1]*h[1]+h[2]*h[2]);
    const double nh=std::max(-h[2]/hl,0.),vh=std::max((v[0]*h[0]+v[1]*h[1]+v[2]*h[2])/hl,0.);
    const double a=std::max(roughness*roughness,.0025),a2=a*a;
    const double distribution=a2/(std::numbers::pi*std::pow(nh*nh*(a2-1)+1,2));
    const double gv=2*nv/(nv+std::sqrt(a2+(1-a2)*nv*nv)),gl=2*nl/(nl+std::sqrt(a2+(1-a2)*nl*nl));
    for(unsigned channel=0;channel<3;++channel) {
        const double f0=.04+(base[channel]-.04)*metallic,fresnel=f0+(1-f0)*std::pow(1-vh,5);
        result.diffuse[channel]=(1-fresnel)*base[channel]*(1-metallic)/std::numbers::pi*nl;
        result.specular[channel]=distribution*gv*gl*fresnel/std::max(4*nl*nv,1e-5)*nl;
    }
    return result;
}
double display_code(double radiance) {
    const double x=std::max(radiance,0.);
    const double mapped=std::clamp(x*(2.51*x+.03)/(x*(2.43*x+.59)+.14),0.,1.);
    return 255*(mapped<=.0031308?mapped*12.92:1.055*std::pow(mapped,1/2.4)-.055);
}
struct Analytic { std::size_t pixels{}, failures{}; double max_error{}; };
// Visits each receiver pixel with its plane point p and unit view vector v.
template<class Visit> void plane_pixels(const render::Frame& frame,const render::Capture& image,float plane_z,Visit&& visit) {
    const auto& view=frame.camera.view;const auto& projection=frame.camera.projection;
    const bool perspective=projection[11]!=0;
    const Rgb3 eye{-view[12],-view[13],-view[14]};
    const auto& viewport=frame.scene_viewport;
    for(unsigned y=0;y<image.extent.height;++y) for(unsigned x=0;x<image.extent.width;++x) {
        if(depth(image,x,y)<=0)continue;
        const double nx=(x+.5-viewport.x)/viewport.width*2-1,ny=1-(y+.5-viewport.y)/viewport.height*2;
        const double view_z=plane_z+view[14],scale=perspective?view_z:1;
        const Rgb3 p{nx*scale/projection[0]-view[12],ny*scale/projection[5]-view[13],plane_z};
        Rgb3 v{0,0,-1};
        if(perspective) {
            double length=0;for(unsigned i=0;i<3;++i){v[i]=eye[i]-p[i];length+=v[i]*v[i];}
            for(auto& value:v)value/=std::sqrt(length);
        }
        visit(x,y,p,v);
    }
}
// Radiance from one unshadowed point light (zero radius): color*I*window/max(d^2,0.0025)
// with window (1-(d/range)^4)^2.
Analytic analytic_point_plane(const render::Frame& frame,const render::Capture& image,const render::Light& light,
                              float plane_z,double base,double roughness) {
    Analytic result;
    plane_pixels(frame,image,plane_z,[&](unsigned x,unsigned y,const Rgb3& p,const Rgb3& v) {
        Rgb3 l{};double d2=0;for(unsigned i=0;i<3;++i){l[i]=light.position[i]-p[i];d2+=l[i]*l[i];}
        const double d=std::sqrt(d2);for(auto& value:l)value/=d;
        const double window=std::pow(std::clamp(1-std::pow(d/light.range,4),0.,1.),2);
        const double irradiance=light.intensity*window/std::max(d2,.0025);
        const auto response=plane_response(v,l,{base,base,base},roughness,0);
        ++result.pixels;
        for(unsigned channel=0;channel<3;++channel) {
            const double expected=irradiance*(response.diffuse[channel]+response.specular[channel])*light.color[channel];
            const double observed=pixel(image,x,y)[channel];
            const double low=display_code(expected*(1-1./1024))-1,high=display_code(expected*(1+1./1024))+1;
            result.max_error=std::max(result.max_error,std::abs(observed-display_code(expected)));
            if(observed<low||observed>high){++result.failures;break;}
        }
    });
    return result;
}
// Radiance from one unshadowed rectangle: its radiance color*I times the window
// (1-((d-e)/range)^4)^2 at the centre distance d, e = (width+height)/2, integrated
// over its area with cos_light*cos_receiver/r^2 (|cos_light| when two-sided) by
// the midpoint rule. One sample is the point limit; otherwise each side takes at
// least `samples` points and twelve per receiver distance to the rectangle, so
// near receivers stay resolved. The band widens by `specular_slack` of the
// expected specular part, which LTC approximates pointwise, and by `peak_slack`
// of the brightest expected channel in the image.
Analytic analytic_rectangle_plane(const render::Frame& frame,const render::Capture& image,const render::Light& light,
                                  float plane_z,const Rgb3& base,double roughness,double metallic,unsigned samples,
                                  double specular_slack,double peak_slack) {
    struct Expected { unsigned x,y; Rgb3 total,specular; };
    std::vector<Expected> expected;double peak=0;
    const Rgb3 centre{light.position[0],light.position[1],light.position[2]};
    const Rgb3 facing{light.direction[0],light.direction[1],light.direction[2]};
    const Rgb3 right{light.right[0]*double(light.width),light.right[1]*double(light.width),light.right[2]*double(light.width)};
    const Rgb3 up{light.up[0]*double(light.height),light.up[1]*double(light.height),light.up[2]*double(light.height)};
    const double extent=.5*(double(light.width)+light.height),range=std::max(double(light.range),1e-3);
    plane_pixels(frame,image,plane_z,[&](unsigned x,unsigned y,const Rgb3& p,const Rgb3& v) {
        Rgb3 to_centre{};double d2=0,s=0,t=0;
        for(unsigned i=0;i<3;++i){to_centre[i]=centre[i]-p[i];d2+=to_centre[i]*to_centre[i];}
        for(unsigned i=0;i<3;++i){s-=to_centre[i]*right[i];t-=to_centre[i]*up[i];}
        s=std::clamp(s/(light.width*double(light.width)),-.5,.5);t=std::clamp(t/(light.height*double(light.height)),-.5,.5);
        double nearest=0;for(unsigned i=0;i<3;++i){const double q=to_centre[i]+s*right[i]+t*up[i];nearest+=q*q;}
        const unsigned k=samples<=1?1:std::clamp(unsigned(std::ceil(12*std::max(light.width,light.height)/std::max(std::sqrt(nearest),1e-6))),samples,768u);
        const double window=std::pow(std::clamp(1-std::pow(std::max(std::sqrt(d2)-extent,0.)/range,4),0.,1.),2);
        const double area=double(light.width)*light.height/(double(k)*k);
        Rgb3 diffuse{},specular{};
        for(unsigned i=0;i<k;++i) for(unsigned j=0;j<k;++j) {
            const double a=(i+.5)/k-.5,b=(j+.5)/k-.5;
            Rgb3 l{};double r2=0;for(unsigned c=0;c<3;++c){l[c]=to_centre[c]+a*right[c]+b*up[c];r2+=l[c]*l[c];}
            const double r=std::sqrt(r2);for(auto& value:l)value/=r;
            double cosine=-(facing[0]*l[0]+facing[1]*l[1]+facing[2]*l[2]);
            cosine=light.two_sided?std::abs(cosine):std::max(cosine,0.);
            if(cosine<=0)continue;
            const auto response=plane_response(v,l,base,roughness,metallic);
            const double weight=cosine/r2*area;
            for(unsigned c=0;c<3;++c){diffuse[c]+=response.diffuse[c]*weight;specular[c]+=response.specular[c]*weight;}
        }
        Expected value{x,y,{},{}};
        for(unsigned c=0;c<3;++c) {
            const double radiance=double(light.color[c])*light.intensity*window;
            value.specular[c]=specular[c]*radiance;value.total[c]=(diffuse[c]+specular[c])*radiance;
            peak=std::max(peak,value.total[c]);
        }
        expected.push_back(value);
    });
    Analytic result;result.pixels=expected.size();
    for(const auto& value:expected) for(unsigned c=0;c<3;++c) {
        const double slack=specular_slack*value.specular[c]+peak_slack*peak,observed=pixel(image,value.x,value.y)[c];
        const double low=display_code(value.total[c]*(1-1./1024)-slack)-1,high=display_code(value.total[c]*(1+1./1024)+slack)+1;
        result.max_error=std::max(result.max_error,std::abs(observed-display_code(value.total[c])));
        if(observed<low||observed>high){
            if(result.failures<4)std::fprintf(stderr,"rectangle truth miss pixel=(%u,%u) channel=%u observed=%.0f band=[%.2f,%.2f] expected=%.5f\n",
                value.x,value.y,c,observed,low,high,value.total[c]);
            ++result.failures;break;
        }
    }
    return result;
}

// Visibility truth from geometry, not from another renderer path. World
// positions come from captured depth (translation-only view). A receiver whose
// segment to the light hits any blocker box shrunk by `margin` is occluded and
// must be black; one that misses every box grown by `margin` must match the same
// frame rendered without shadows. Pixels in the margin band are not judged.
struct Box { Vec3 center, half; };
struct ShadowTruth { std::size_t occluded{}, lit{}, failures{}; unsigned max_lit_delta{}, max_occluded{}; };
ShadowTruth shadow_truth(const render::Frame& frame,const render::Capture& shadowed,const render::Capture& unshadowed,
                         const render::Light& light,const std::vector<Box>& blockers,double margin) {
    const auto& view=frame.camera.view;const auto& projection=frame.camera.projection;
    check(view[0]==1&&view[5]==1&&view[10]==1&&view[1]==0&&view[2]==0&&view[4]==0&&view[6]==0&&view[8]==0&&view[9]==0,
          "shadow truth reconstructs positions through a translation-only view");
    const bool perspective=projection[11]!=0;
    const auto hits=[&](const std::array<double,3>& origin,const std::array<double,3>& direction,double length,double grow) {
        for(const auto& box:blockers) {
            double t_in=1e-4,t_out=length;bool empty=false;
            for(unsigned axis=0;axis<3;++axis) {
                const double low=box.center[axis]-box.half[axis]-grow,high=box.center[axis]+box.half[axis]+grow;
                if(low>=high){empty=true;break;}
                if(std::abs(direction[axis])<1e-12){if(origin[axis]<low||origin[axis]>high){empty=true;break;}continue;}
                double t0=(low-origin[axis])/direction[axis],t1=(high-origin[axis])/direction[axis];
                if(t0>t1)std::swap(t0,t1);
                t_in=std::max(t_in,t0);t_out=std::min(t_out,t1);
            }
            if(!empty&&t_in<=t_out)return true;
        }
        return false;
    };
    ShadowTruth result;
    const auto& viewport=frame.scene_viewport;
    for(unsigned y=0;y<shadowed.extent.height;++y) for(unsigned x=0;x<shadowed.extent.width;++x) {
        const double z=depth(shadowed,x,y);
        if(z<=0||z!=depth(unshadowed,x,y))continue;
        const double nx=(x+.5-viewport.x)/viewport.width*2-1,ny=1-(y+.5-viewport.y)/viewport.height*2;
        const double view_z=perspective?projection[14]/(z-projection[10]):(z-projection[14])/projection[10];
        const double scale=perspective?view_z:1;
        const std::array<double,3> p{nx*scale/projection[0]-view[12],ny*scale/projection[5]-view[13],view_z-view[14]};
        bool on_blocker=false;
        for(const auto& box:blockers) {
            bool inside=true;
            for(unsigned axis=0;axis<3;++axis)inside&=std::abs(p[axis]-box.center[axis])<=box.half[axis]+margin;
            on_blocker|=inside;
        }
        if(on_blocker)continue;
        std::array<double,3> direction{};double length=1e30;
        if(light.kind==render::LightKind::directional) {
            for(unsigned axis=0;axis<3;++axis)direction[axis]=-light.direction[axis];
        } else {
            double squared=0;
            for(unsigned axis=0;axis<3;++axis){direction[axis]=light.position[axis]-p[axis];squared+=direction[axis]*direction[axis];}
            length=std::sqrt(squared);for(auto& value:direction)value/=length;
        }
        const auto* image=pixel(shadowed,x,y);const auto* reference=pixel(unshadowed,x,y);
        if(hits(p,direction,length,-margin)) {
            ++result.occluded;
            const unsigned brightest=std::max({image[0],image[1],image[2]});
            result.max_occluded=std::max(result.max_occluded,brightest);
            result.failures+=brightest>2;
        } else if(!hits(p,direction,length,margin)) {
            ++result.lit;
            unsigned delta=0;
            for(unsigned c=0;c<3;++c)delta=std::max(delta,unsigned(std::abs(int(image[c])-int(reference[c]))));
            result.max_lit_delta=std::max(result.max_lit_delta,delta);
            result.failures+=delta>2;
        }
    }
    return result;
}

std::vector<render::Light> room_lights() {
    std::vector<render::Light> lights(4);
    lights[0].kind = render::LightKind::directional;
    lights[0].direction = normalize({.4f,-1,.6f});
    lights[0].intensity = 1.5f;
    lights[1].position = {-2.5f,3,1}; lights[1].color = {1,.65f,.35f}; lights[1].intensity = 55;
    lights[2].position = {2.5f,2.5f,5}; lights[2].color = {.35f,.55f,1}; lights[2].intensity = 55;
    lights[3].kind = render::LightKind::rectangle;
    lights[3].position = {0,3.8f,3}; lights[3].direction = {0,-1,0};
    lights[3].right = {1,0,0}; lights[3].up = {0,0,1};
    lights[3].width = 2; lights[3].height = 1.2f; lights[3].intensity = 8;
    for (std::size_t index = 0; index < lights.size(); ++index) lights[index].stable_key = 100 + index;
    return lights;
}
struct Fixture {
    Window window;
    render::Renderer renderer;
    Room room;
    std::vector<render::Light> lights = room_lights();
    ui::Frame drawing;
    render::Frame frame;
    std::filesystem::path output;
    std::ofstream metrics;
    ui::PixelExtent initial_extent;
    render::ViewSettings initial_view;
    decltype(render::Frame::environment) initial_environment{};
    Fixture(bool benchmark, std::filesystem::path evidence_path,
            atelier::rhi::Validation validation = atelier::rhi::Validation::gpu)
        : window(benchmark ? 2560 : 960, benchmark ? 1440 : 540),
          renderer(take(render::Renderer::open(window.hwnd,
              {.validation=benchmark ? atelier::rhi::Validation::disabled : validation,
               .vsync=false}))),
          drawing{.id=20000,.extent=take(renderer.target()).extent,.draws={}},
          frame{drawing,camera(drawing.extent),{0,0,float(drawing.extent.width),float(drawing.extent.height)},room.meshes},
          output(std::move(evidence_path)) {
        std::filesystem::create_directories(output);
        if (benchmark) check(drawing.extent == ui::PixelExtent{2560,1440},
                             "SceneLit benchmark uses the requested native 1440p scene target");
        metrics.open(output / (benchmark ? "benchmark-summary.txt" : "image-metrics.csv"));
        check(metrics.good(), "SceneLit evidence directory opened");
        frame.lights = lights; frame.scene_identity = 1;
        // This fixture authors a sky explicitly; the renderer default is dark.
        frame.environment = {.025f,.035f,.055f};
        if (!benchmark) {
            frame.scene_viewport = {24,32,float(drawing.extent.width)-48,float(drawing.extent.height)-56};
            frame.camera = camera({static_cast<unsigned>(frame.scene_viewport.width),static_cast<unsigned>(frame.scene_viewport.height)});
            drawing.draws.push_back({{4,4,16,16},{4,4,20,20},{0,1,0,1},{0,0,1,1},{}});
        }
        frame.view.shading = render::Shading::scene_lit;
        frame.view.lit.probe_volumes[0].minimum = {-5,-1,-1}; frame.view.lit.probe_volumes[0].maximum = {5,5,8};
        frame.view.lit.probe_volumes[0].counts = {11,7,10};
        frame.view.lit.probe_update_budget = 96; frame.view.lit.probe_rays = 64;
        if (!benchmark) metrics << "scenario,mean_rgb_delta,p95_rgb_delta,changed_pixels,darker_pixels\n";
        initial_extent = drawing.extent; initial_view = frame.view; initial_environment = frame.environment;
    }
    render::FrameResult step(render::Capture* capture = nullptr) {
        render::FrameResult result;
        for (unsigned retry=0;retry<120;++retry) {
            pump(); ++drawing.id;
            result = take(prepared_render(renderer,frame, capture));
            if (result.status == render::FrameStatus::accepted) break;
            if (retry==0) std::fprintf(stderr,
                "SceneLit transient skipped frame: visible=%d minimized=%d input=%ux%u next=%ux%u; waiting for accepted presentation.\n",
                IsWindowVisible(window.hwnd),IsIconic(window.hwnd),drawing.extent.width,drawing.extent.height,
                result.next_extent.width,result.next_extent.height);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(result.status == render::FrameStatus::accepted && result.accepted_id == drawing.id,
              "SceneLit accepted through the ordinary Renderer frame contract");
        if (capture) {
            check(capture->frame_id == drawing.id && capture->extent == drawing.extent &&
                      capture->rgba.size() == std::size_t(drawing.extent.width)*drawing.extent.height*4,
                  "SceneLit real GPU capture publishes the accepted payload");
            check(std::ranges::all_of(capture->depth, [](float value) { return std::isfinite(value) && value >= 0 && value <= 1; }),
                  "SceneLit depth contains finite D3D depth values");
        }
        frame.camera_cut = false;
        return result;
    }
    render::Capture capture(const char* name, unsigned settle = 1) {
        for (unsigned index = 1; index < settle; ++index) step();
        render::Capture result; step(&result);
        png(result, output / (std::string(name) + ".png"));
        return result;
    }
    // The last `frames` of `settle` accepted images, averaged per channel:
    // per-frame estimator noise averages out, the converged signal remains.
    render::Capture average(const char* name, unsigned settle, unsigned frames) {
        for (unsigned index = frames; index < settle; ++index) step();
        render::Capture result;
        std::vector<unsigned> sum;
        for (unsigned index = 0; index < frames; ++index) {
            step(&result);
            sum.resize(result.rgba.size());
            for (std::size_t i = 0; i < sum.size(); ++i) sum[i] += result.rgba[i];
        }
        for (std::size_t i = 0; i < sum.size(); ++i) result.rgba[i] = static_cast<std::uint8_t>((sum[i] + frames / 2) / frames);
        png(result, output / (std::string(name) + ".png"));
        return result;
    }
    Difference compare(const char* name, const render::Capture& a, const render::Capture& b) {
        const auto delta = difference(a, b);
        metrics << name << ',' << delta.mean << ',' << delta.p95 << ',' << delta.changed << ',' << delta.darker << '\n';
        metrics.flush();
        std::printf("SceneLit %s: mean=%.3f p95=%u changed=%zu darker=%zu\n",
                    name, delta.mean, delta.p95, delta.changed, delta.darker);
        return delta;
    }
    void edited() { frame.meshes = room.meshes; frame.lights = lights; }
    // At most 24 shadow faces redraw per frame, whole lights at a time: once a
    // cube of k<=6 faces does not fit, at least 25-k faces were drawn, lights with
    // undrawn faces first. A light adds direct light once all its faces exist,
    // then fades in over eight frames. Pending lights must clear within
    // ceil(faces/19)+8 frames from `since` (default: the next submission).
    void settle_shadows(std::uint64_t since = 0) {
        if (!since) since = drawing.id + 1;
        for (unsigned i = 0; i < 240; ++i) {
            const auto stats = step().stats;
            const auto id = stats.scene_lit_gpu_sample_frame_id;
            if (!stats.scene_lit_gpu_timing_valid || id < since) continue;
            if (stats.scene_lit_shadow_cache_pending_lights == 0) {
                for (unsigned fade = 0; fade < 8; ++fade) step();
                return;
            }
            check(id - since <= (stats.scene_lit_shadow_cache_faces + 18) / 19 + 8,
                  "pending shadow lights clear within ceil(faces/19)+8 frames");
        }
        check(false, "pending shadow lights clear");
    }
    // Returns a case-modified windowed fixture to its constructed target, room,
    // lights and view as a new scene domain.
    void restore() {
        window.resize(int(initial_extent.width),int(initial_extent.height));
        drawing.extent = take(renderer.sync_window_target()).extent;
        check(drawing.extent == initial_extent, "SceneLit fixture restores its constructed target");
        frame.scene_viewport = {24,32,float(drawing.extent.width)-48,float(drawing.extent.height)-56};
        frame.camera = camera({static_cast<unsigned>(frame.scene_viewport.width),static_cast<unsigned>(frame.scene_viewport.height)});
        room = Room{}; lights = room_lights();
        frame.view = initial_view; frame.environment = initial_environment; frame.lighting_focus = {};
        frame.camera_cut = true; ++frame.scene_identity; edited();
    }
    Region region(Vec3 point) const { return project_region(frame.camera,frame.scene_viewport,drawing.extent,point); }
    void close() { check(bool(renderer.close()), "SceneLit drains GPU resources before HWND destruction"); }
};

std::array<double,3> lighting_mean(Fixture& f,const char* name,Region region,unsigned settle=64,unsigned samples=16) {
    for(unsigned i=0;i<settle;++i) f.step();
    std::array<double,3> sum{};
    render::Capture image;
    for(unsigned i=0;i<samples;++i) {
        f.step(&image);const auto value=mean_rgb(image,region);
        for(unsigned channel=0;channel<3;++channel) sum[channel]+=value[channel];
    }
    for(auto& value:sum)value/=samples;
    png(image,f.output/(std::string(name)+".png"));
    return sum;
}

void benchmark(Fixture& f) {
    for (unsigned frame = 0; frame < warmup_frames; ++frame) f.step();
    std::ofstream samples(f.output / "benchmark-gpu.csv");
    samples << "submission,completed_sequence,completed_frame_id,gbuffer_ms,as_ms,direct_ms,ddgi_ms,gtao_ms,sssr_ms,temporal_ms,composite_ms,sum_gpu_ms,current_logical_resource_bytes,completed_shadow_queries,completed_probe_queries,ddgi_update_ms,ddgi_gather_ms,ddgi_update_visibility_queries,ddgi_gather_visibility_queries,direct_cluster_ms,shadow_update_ms,direct_shading_ms,ddgi_primary_ms,ddgi_hit_lighting_ms,completed_pass_mask\n";
    std::vector<double> totals;
    std::array<std::vector<double>, 8> passes;
    std::array<std::vector<double>, 2> ddgi_passes;
    std::array<std::vector<double>, 3> direct_passes;
    std::array<std::vector<double>, 2> ddgi_update_passes;
    std::uint64_t resource_peak = 0;
    std::uint64_t previous_sequence = 0;
    const auto first_measured_frame = f.drawing.id;
    for (unsigned index = 0; index < 480 && totals.size() < 240; ++index) {
        const auto stats = f.step().stats;
        if (!stats.scene_lit_gpu_timing_valid || stats.scene_lit_gpu_sample_sequence <= previous_sequence ||
            stats.scene_lit_gpu_sample_frame_id <= first_measured_frame) continue;
        check(stats.scene_lit_gpu_sample_frame_id <= f.drawing.id,
              "SceneLit asynchronous GPU sample identifies an already submitted frame");
        previous_sequence = stats.scene_lit_gpu_sample_sequence;
        double sum = 0;
        samples << f.drawing.id << ',' << previous_sequence << ',' << stats.scene_lit_gpu_sample_frame_id;
        for (std::size_t pass = 0; pass < passes.size(); ++pass) {
            const double value = stats.scene_lit_gpu_ms[pass];
            check(std::isfinite(value) && value >= 0, "SceneLit GPU timestamps are finite nonnegative milliseconds");
            passes[pass].push_back(value); sum += value; samples << ',' << value;
        }
        totals.push_back(sum); resource_peak = std::max(resource_peak, stats.scene_lit_resource_bytes);
        samples << ',' << sum << ',' << stats.scene_lit_resource_bytes << ',' << stats.scene_lit_shadow_rays
                << ',' << stats.scene_lit_probe_rays;
        double ddgi_sum=0;
        for(std::size_t pass=0;pass<ddgi_passes.size();++pass) {
            const double value=stats.scene_lit_ddgi_gpu_ms[pass];
            check(std::isfinite(value)&&value>=0,"DDGI subpass timestamps are finite nonnegative milliseconds");
            ddgi_passes[pass].push_back(value);ddgi_sum+=value;samples<<','<<value;
        }
        check(ddgi_sum<=stats.scene_lit_gpu_ms[3]+.01,"DDGI subpass timestamps belong to the enclosing completed DDGI stage");
        std::uint64_t ddgi_visibility=0;
        for(auto queries:stats.scene_lit_ddgi_visibility_rays){samples<<','<<queries;ddgi_visibility+=queries;}
        check(ddgi_visibility<=stats.scene_lit_shadow_rays,"DDGI classified queries are a subset of all visibility queries");
        const auto subpasses=[&](auto& series,const auto& values,double parent) {
            double sum=0;
            for(std::size_t i=0;i<values.size();++i) {
                const double value=values[i];
                check(std::isfinite(value)&&value>=0,"GPU subpass timestamps are finite nonnegative milliseconds");
                series[i].push_back(value);sum+=value;samples<<','<<value;
            }
            check(sum<=parent+.01,"GPU subpass timestamps belong to their enclosing completed stage");
        };
        subpasses(direct_passes,std::array{stats.scene_lit_direct_cluster_gpu_ms,stats.scene_lit_shadow_cache_gpu_ms,
            stats.scene_lit_direct_shading_gpu_ms},stats.scene_lit_gpu_ms[2]);
        subpasses(ddgi_update_passes,stats.scene_lit_ddgi_update_gpu_ms,stats.scene_lit_ddgi_gpu_ms[0]);
        // Shadow atlas updates (bit 13) run only when the static room changes.
        check((stats.scene_lit_gpu_pass_mask&0x5fffu)==0x5fffu,"fixed room executes every measured GPU scope in the same completed sample");
        samples<<','<<stats.scene_lit_gpu_pass_mask<<'\n';
    }
    check(totals.size() >= 120 && percentile(totals, 95) > 0, "SceneLit benchmark obtains completed GPU timestamp samples");
    f.metrics << "fixed_scene=2560x1440, 8 shared-cube instances, 1 directional + 2 point + 1 rectangle plus DDGI mesh emission\n"
              << "warmup_frames=" << warmup_frames << " gpu_samples=" << totals.size() << "\n"
              << "GPU timestamps exclude capture/readback, CPU Present duration and display scanout.\n"
              << "sum_of_measured_passes_ms p50=" << percentile(totals,50) << " p95=" << percentile(totals,95)
              << " p99=" << percentile(totals,99) << "\nlogical_resource_peak_bytes=" << resource_peak
              << " (logical current allocations, not physical VRAM peak)\n";
    constexpr const char* names[]{"GBuffer","AS","direct","DDGI","GTAO","SSSR","temporal","composite"};
    for (std::size_t pass = 0; pass < passes.size(); ++pass)
        f.metrics << names[pass] << "_ms p50=" << percentile(passes[pass],50) << " p95=" << percentile(passes[pass],95)
                  << " p99=" << percentile(passes[pass],99) << '\n';
    constexpr const char* ddgi_names[]{"DDGI_update","DDGI_gather"};
    for(std::size_t pass=0;pass<ddgi_passes.size();++pass)
        f.metrics<<ddgi_names[pass]<<"_ms p50="<<percentile(ddgi_passes[pass],50)<<" p95="<<percentile(ddgi_passes[pass],95)
                 <<" p99="<<percentile(ddgi_passes[pass],99)<<'\n';
    constexpr const char* direct_names[]{"direct_cluster","shadow_update","direct_shading"};
    for(std::size_t pass=0;pass<direct_passes.size();++pass)
        f.metrics<<direct_names[pass]<<"_ms p50="<<percentile(direct_passes[pass],50)<<" p95="<<percentile(direct_passes[pass],95)
                 <<" p99="<<percentile(direct_passes[pass],99)<<'\n';
    constexpr const char* update_names[]{"DDGI_primary","DDGI_hit_lighting"};
    for(std::size_t pass=0;pass<ddgi_update_passes.size();++pass)
        f.metrics<<update_names[pass]<<"_ms p50="<<percentile(ddgi_update_passes[pass],50)<<" p95="<<percentile(ddgi_update_passes[pass],95)
                 <<" p99="<<percentile(ddgi_update_passes[pass],99)<<'\n';
    check(samples.good() && f.metrics.good(), "SceneLit GPU benchmark evidence written");
    // Evidence capture happens only after the timestamp sample loop.
    f.capture("benchmark-1440p");
    std::puts("Renderer SceneLit benchmark: PASS (1440p fixed scene, completed GPU pass timestamps)");
}

void environment_cases(Fixture& f) {
    const auto meshes=f.room.meshes;
    const auto lights=f.lights;
    const auto view=f.frame.view;
    const auto environment=f.frame.environment;
    f.lights.clear();
    for(auto& mesh:f.room.meshes) mesh.material.emissive={};
    f.edited();++f.frame.scene_identity;
    f.frame.environment={};
    f.frame.view.background=render::Background::gradient;
    f.frame.view.lit.bloom=false;f.frame.view.lit.antialiasing=render::Antialiasing::none;
    const auto dark=f.capture("sky-absent-black",warmup_frames);
    std::size_t surfaces=0;
    for(std::size_t i=0;i<dark.depth.size();++i) if(dark.depth[i]>0) {
        ++surfaces;
        check(dark.rgba[i*4]==0 && dark.rgba[i*4+1]==0 && dark.rgba[i*4+2]==0,
            "Scene Lit has no implicit ambient light without authored sky, lights or emission");
    }
    check(surfaces>1000,"dark environment check contains real visible geometry");
    f.frame.environment={.12f,.08f,.04f};
    const auto sky=f.capture("sky-authored-on",warmup_frames);
    check(f.compare("sky-authored-response",dark,sky).changed>1000,
        "explicit scene sky radiance illuminates real surfaces");
    check(f.step().stats.scene_lit_shadow_sources==0,"sky is independent of direct shadow sources");
    f.frame.environment={};
    // Allow the budgeted multi-bounce histories to decay to a one-code-value
    // SDR endpoint; the scene radiance input itself is already exactly zero.
    const auto removed=f.capture("sky-removed-dark",384);
    for(std::size_t i=0;i<removed.depth.size();++i) if(removed.depth[i]>0)
        check(removed.rgba[i*4]<=1 && removed.rgba[i*4+1]<=1 && removed.rgba[i*4+2]<=1,
            "removing scene sky converges to darkness without frozen ambient history");
    f.room.meshes.clear();f.edited();
    const auto empty=f.capture("lit-empty-black");
    for(unsigned y=unsigned(f.frame.scene_viewport.y);y<unsigned(f.frame.scene_viewport.y+f.frame.scene_viewport.height);++y)
        for(unsigned x=unsigned(f.frame.scene_viewport.x);x<unsigned(f.frame.scene_viewport.x+f.frame.scene_viewport.width);++x)
            check(pixel(empty,x,y)[0]==0 && pixel(empty,x,y)[1]==0 && pixel(empty,x,y)[2]==0 && depth(empty,x,y)==0,
                "empty Scene Lit viewport ignores the studio gradient preference");
    f.room.meshes=meshes;f.lights=lights;f.edited();f.frame.view=view;f.frame.environment=environment;
    ++f.frame.scene_identity;
    std::puts("Renderer SceneLit environment: PASS (explicit sky, absent/removed darkness, black empty viewport)");
}
void image_cases(Fixture& f) {
    const auto original_meshes = f.room.meshes;
    const auto original_lights = f.lights;
    const auto original_lit = f.frame.view.lit;
    const auto original_environment = f.frame.environment;
    f.room.meshes.clear(); f.edited();
    const auto cold_empty = f.capture("00-empty-scene");
    check(std::ranges::all_of(cold_empty.depth,[](float value) { return value == 0; }),
          "cold empty SceneLit capture publishes background depth");
    f.room.meshes = original_meshes; f.edited();
    const auto combined = f.capture("01-combined", warmup_frames);
    const auto initial = f.step().stats;
    check(initial.scene_lit_shadow_sources >= 4 && initial.scene_lit_shadow_sources == initial.scene_lit_light_sources &&
              initial.scene_lit_resource_bytes > 0 && initial.scene_draw_calls > 0,
          "SceneLit shades a real room with all four requested shadow lights");
    check(pixel(combined,12,12)[1] == 255 && depth(combined,12,12) == 0 && depth(combined,20,270) == 0,
          "offset SceneLit viewport preserves HUD colors and exterior background depth");
    auto overflow_light = original_lights[1];
    overflow_light.stable_key = 104; overflow_light.position = {0,3,5};
    f.lights.push_back(overflow_light); f.edited();
    render::Capture overflow;
    const auto overflow_result = f.step(&overflow);
    png(overflow,f.output/"01-five-shadow-lights.png");
    check(overflow_result.stats.scene_lit_shadow_sources == initial.scene_lit_shadow_sources+1 &&
              overflow_result.stats.scene_lit_shadow_sources == overflow_result.stats.scene_lit_light_sources &&
              overflow_result.stats.ui_draw_calls == initial.ui_draw_calls &&
              overflow_result.stats.scene_draw_calls == initial.scene_draw_calls &&
              pixel(overflow,12,12)[1] == 255 && depth(overflow,12,12) == 0 &&
              std::count_if(overflow.depth.begin(),overflow.depth.end(),[](float value) { return value > 0; }) > 1000,
          "fifth casting light remains a shadow source with scene/UI/depth intact");
    std::puts("SceneLit all-light visibility: accepted (all five analytic lights retain shadows)");
    f.lights = original_lights; f.edited();

    // Hold unrelated contributions at zero; compare the same receiver/depth
    // pixels with and without actual occluder visibility for each light kind.
    auto& options = f.frame.view.lit;
    options.ddgi = options.gtao = options.sssr = options.bloom = false;
    options.antialiasing = render::Antialiasing::none;
    f.frame.environment = {0,0,0};
    f.room.meshes[7].material.emissive = {0,0,0};
    for (std::size_t index = 0; index < original_lights.size(); ++index) {
        f.lights = {original_lights[index]};
        f.lights[0].casts_shadow = false; f.edited();
        const auto unshadowed = f.capture(("02-light-"+std::to_string(index)+"-unshadowed").c_str());
        f.lights[0].casts_shadow = true; f.edited();
        const auto shadowed = f.capture(("02-light-"+std::to_string(index)+"-shadowed").c_str());
        const auto delta = f.compare(("light-"+std::to_string(index)+"-shadow").c_str(),unshadowed,shadowed);
        check(unshadowed.depth == shadowed.depth && delta.darker >= 64,
              "each directional/point/rectangle light casts visible geometric receiver shadows");
    }
    const auto rect_small = f.capture("03-rect-small");
    f.lights[0].width = 3.5f; f.lights[0].height = 2.5f; f.edited();
    const auto rect_large = f.capture("03-rect-large");
    check(f.compare("rectangle-size",rect_small,rect_large).changed >= 64,
          "rectangle emitting size affects actual illumination and shadow pixels");
    f.lights[0].direction = {0,1,0}; f.lights[0].up = {0,0,-1}; f.edited();
    const auto rect_back = f.capture("03-rect-back");
    check(f.compare("rectangle-backface",rect_large,rect_back).darker >= 64,
          "single-sided rectangle suppresses back-facing illumination");

    // Self-emission is scene-linear HDR RGB and remains distinct from exposure.
    f.lights.clear(); f.room.meshes[7].material.emissive = {4,.3f,.05f}; f.edited();
    const auto red = f.capture("04-emissive-red");
    const auto emission_region = f.region({0,2.75f,6.65f});
    const auto red_rgb = mean_rgb(red,emission_region);
    f.room.meshes[7].material.emissive = {.05f,.3f,4}; f.edited();
    const auto blue = f.capture("04-emissive-blue");
    const auto blue_rgb = mean_rgb(blue,emission_region);
    check(red_rgb[0] > red_rgb[2]+50 && blue_rgb[2] > blue_rgb[0]+50,
          "HDR emission retains independent RGB components above linear intensity one");
    f.room.meshes[7].material.emissive = {.05f,.3f,.5f}; f.edited();
    const auto dim = f.capture("04-emissive-dim");
    check(mean_rgb(dim,emission_region)[2]+10 < blue_rgb[2], "HDR emission intensity is not clamped before tone mapping");
    options.exposure_ev = 2;
    const auto exposed = f.capture("04-exposure");
    check(mean_rgb(exposed,emission_region)[2] > mean_rgb(dim,emission_region)[2]+10,
          "display exposure independently changes the HDR result");
    options.exposure_ev = 0;

    f.lights = original_lights; f.room.meshes = original_meshes; f.edited();
    f.frame.environment = original_environment;
    const auto metal = f.capture("05-material-metals");
    f.room.meshes[5].material.metallic = f.room.meshes[6].material.metallic = 0;
    f.room.meshes[6].material.roughness = .95f; f.edited();
    const auto diffuse = f.capture("05-material-diffuse");
    check(metal.depth == diffuse.depth && f.compare("roughness-metallic",metal,diffuse).changed >= 64,
          "roughness and metallic change surface response without changing geometry");

    // AO is permitted to modulate indirect light only. Direct light and the
    // emissive panel remain identical even where occluder contact is present.
    f.room.meshes = original_meshes;
    // Settle the new light set before comparing AO: radiance edits preserve
    // history, so a cold first frame can still contain the prior four lights.
    // A zero-radius directional source then keeps direct sampling constant.
    f.lights = {original_lights[0]};
    f.lights[0].source_radius = 0;
    f.edited();
    options.gtao = false; f.frame.environment = {0,0,0};
    const auto direct = f.capture("06-direct-no-ao",warmup_frames);
    options.gtao = true;
    const auto direct_ao = f.capture("06-direct-with-ao",16);
    check(f.compare("ao-direct-emission-preserved",direct,direct_ao).p95 <= 1,
          "GTAO does not darken direct illumination or self-emission");

    // Neutral direct sources isolate colored surface bounce. Subtract the
    // no-DDGI receiver color so direct-light tint cannot satisfy this check.
    f.lights = original_lights;
    for (auto& light : f.lights) light.color = {1,1,1};
    f.room.meshes[0].material.metallic = 0; f.room.meshes[0].material.roughness = .8f;
    f.room.meshes[7].material.emissive = {0,0,0}; f.edited();
    options.gtao = false; options.ddgi = false; f.frame.environment = {0,0,0};
    // Both sides need settled direct radiance; otherwise the DDGI difference
    // would also include direct-history convergence after the light edit.
    const auto gi_off = f.capture("07-ddgi-off",warmup_frames);
    options.ddgi = true;
    const auto gi_on = f.capture("07-ddgi-color-bounce",warmup_frames);
    check(f.compare("ddgi-visible-bounce",gi_off,gi_on).changed >= 64, "world-space DDGI contributes actual indirect receiver color");
    const auto warm_region = f.region({-3.15f,.01f,1.f});
    const auto cool_region = f.region({3.15f,.01f,1.f});
    const auto warm0 = mean_rgb(gi_off,warm_region), warm1 = mean_rgb(gi_on,warm_region);
    const auto cool0 = mean_rgb(gi_off,cool_region), cool1 = mean_rgb(gi_on,cool_region);
    f.metrics << "warm_bounce_rgb," << warm1[0]-warm0[0] << ',' << warm1[1]-warm0[1] << ',' << warm1[2]-warm0[2] << '\n'
              << "cool_bounce_rgb," << cool1[0]-cool0[0] << ',' << cool1[1]-cool0[1] << ',' << cool1[2]-cool0[2] << '\n';
    check(warm1[0]-warm0[0] > warm1[2]-warm0[2]+1 && cool1[2]-cool0[2] > cool1[0]-cool0[0]+1,
          "DDGI transfers warm and cool wall color to separate neutral receivers");
    f.room.meshes[2].color = {.025f,.1f,.8f,1}; f.edited();
    const auto gi_updated = f.capture("07-ddgi-dynamic-update",warmup_frames);
    options.ddgi = false;
    const auto gi_updated_off = f.capture("07-ddgi-dynamic-off",warmup_frames);
    const auto updated = mean_rgb(gi_updated,warm_region), updated0 = mean_rgb(gi_updated_off,warm_region);
    check(updated[2]-updated0[2] > updated[0]-updated0[0]+1,
          "DDGI updates a changed surface's indirect color instead of retaining stale probes");
    // A fixed environment isolates AO from changing probe updates and random
    // direct-light samples. It is still an indirect diffuse contribution.
    f.lights.clear(); f.edited();
    options.ddgi = false; f.frame.environment = {.15f,.15f,.15f}; options.gtao = true;
    const auto gi_ao = f.capture("08-gtao-indirect",warmup_frames);
    options.gtao = false;
    const auto gi_no_ao = f.capture("08-gtao-disabled",warmup_frames);
    check(f.compare("ao-indirect-contact",gi_no_ao,gi_ao).darker >= 32,
          "GTAO visibly attenuates indirect receiver contacts");

    // Screen-space tracing adds visible reflection while the environment
    // remains a usable fallback with the emissive reflector off-screen.
    f.room.meshes = original_meshes; f.lights = original_lights; f.edited();
    options = original_lit; f.frame.environment = original_environment; options.bloom = options.ddgi = false;
    options.antialiasing = render::Antialiasing::none;
    options.sssr = false;
    const auto fallback = f.capture("09-reflection-fallback",warmup_frames);
    options.sssr = true;
    const auto reflected = f.capture("09-sssr",warmup_frames);
    check(f.compare("sssr-reflection",fallback,reflected).changed >= 32,
          "SSSR adds actual reflected screen-space radiance over environment fallback");
    f.room.meshes[7].world[12] = 15; f.edited();
    f.lights.clear(); f.edited(); options.ddgi = false;
    const auto offscreen = f.capture("09-offscreen-fallback",warmup_frames);
    const auto metal_region = f.region({2.1f,.55f,1.45f});
    const auto fallback_rgb = mean_rgb(offscreen,metal_region);
    check(fallback_rgb[0]+fallback_rgb[1]+fallback_rgb[2] > 12,
          "reflective surface retains finite environment fallback after its visible source exits the screen");
    f.frame.environment = {0,0,0};
    const auto no_environment = f.capture("09-fallback-environment-off");
    const auto no_environment_rgb = mean_rgb(no_environment,metal_region);
    check(fallback_rgb[0]+fallback_rgb[1]+fallback_rgb[2] >
              no_environment_rgb[0]+no_environment_rgb[1]+no_environment_rgb[2]+10,
          "off-screen reflective receiver visibly uses the configured environment fallback");

    f.room.meshes = original_meshes; f.lights = original_lights; f.edited(); options = original_lit; f.frame.environment = original_environment;
    const auto stable0 = f.capture("10-temporal-stable-0",warmup_frames);
    const auto stable1 = f.capture("10-temporal-stable-1",8);
    const auto stable_delta = f.compare("temporal-stable",stable0,stable1);
    check(stable_delta.mean <= 3 && stable_delta.p95 <= 8,
          "converged combined SceneLit remains stable in real geometry pixels");
    const auto converged_edit = f.average("10-converged-edit", convergence_frames, convergence_frames);
    f.frame.camera_cut = true;
    render::Capture cut; const auto cut_result = f.step(&cut); png(cut,f.output/"10-camera-cut.png");
    check(cut_result.stats.scene_lit_history_resets > 0,
          "camera cut invalidates effect histories before accepting the current frame");
    check(f.step().stats.scene_lit_history_resets == 0,
          "ordinary accepted frame reuses valid histories after a camera cut");
    ++f.frame.scene_identity;
    const auto replaced = f.step();
    check(replaced.stats.scene_lit_history_resets > 0,
          "replacement scene/Edit-Play execution domain cannot reuse another world's histories");
    const auto simulation = f.average("10-simulation-independent",warmup_frames+convergence_frames,convergence_frames);
    const auto simulation_delta = f.compare("scene-lit-simulation-independent",converged_edit,simulation);
    check(simulation_delta.mean <= 3 && simulation_delta.p95 <= 8,
          "identical Edit and Play snapshots converge to the same SceneLit appearance");
    f.lights.clear(); f.edited();
    render::Capture deleted; const auto deleted_result = f.step(&deleted); png(deleted,f.output/"10-lights-deleted.png");
    check(deleted_result.stats.scene_lit_history_resets == 0 && deleted_result.stats.scene_lit_shadow_sources == 0,
          "deleting analytic lights leaves no shadow sources without resetting the geometry domain");
    check(f.compare("light-deletion-immediate",stable1,deleted).changed >= 64,
          "light deletion visibly changes the current frame rather than leaving a temporal ghost");

    f.lights = original_lights; f.edited();
    f.frame.camera = camera({static_cast<unsigned>(f.frame.scene_viewport.width),static_cast<unsigned>(f.frame.scene_viewport.height)},true);
    f.frame.camera_cut = true;
    const auto ortho = f.capture("11-orthographic",warmup_frames);
    check(std::count_if(ortho.depth.begin(),ortho.depth.end(),[](float value) { return value > 0; }) > 1000,
          "SceneLit supports a real orthographic camera with reconstructed world positions");
    f.lights.clear(); f.edited();
    // Keep the same phase for temporal AA policies: illumination must not
    // alter geometry, while a different jitter can change raster depth.
    const auto ortho_unlit = f.capture("11-orthographic-lights-off",8);
    check(ortho.depth == ortho_unlit.depth && f.compare("orthographic-light-response",ortho,ortho_unlit).darker >= 64,
          "orthographic reconstructed positions participate in the same scene illumination");
    f.lights = original_lights; f.edited();
    f.frame.view.shading = render::Shading::matcap;
    const auto matcap = f.capture("12-matcap");
    for (auto& mesh : f.room.meshes) { mesh.color = {1,0,1,1}; mesh.material.emissive = {30,20,10}; }
    f.lights.clear(); ++f.frame.scene_identity; f.edited();
    const auto matcap_simulation = f.capture("12-matcap-independent");
    check(matcap.rgba == matcap_simulation.rgba && matcap.depth == matcap_simulation.depth,
          "MatCap remains independent of material/light edits and simulation execution domain");
    f.frame.view.shading = render::Shading::scene_lit;
    f.room.meshes = original_meshes; f.lights = original_lights; f.edited();
    f.capture("12-scene-lit-restored",warmup_frames);

    f.step();
    const auto resized_extent = ui::PixelExtent{1009,577};
    f.window.resize(int(resized_extent.width),int(resized_extent.height));
    const auto transition = f.step();
    check(transition.next_extent == resized_extent, "SceneLit resize reports the next physical target");
    f.drawing.extent = resized_extent;
    f.frame.scene_viewport = {24,32,float(resized_extent.width)-48,float(resized_extent.height)-56};
    f.frame.camera = camera({resized_extent.width-48,resized_extent.height-56});
    const auto resized = f.step();
    check(resized.stats.scene_lit_history_resets > 0,
          "SceneLit target resize invalidates dependent histories and updates resource extents");
    f.capture("13-resized",16);
    f.room.meshes.clear(); f.lights.clear(); f.edited();
    const auto empty = f.capture("14-delete-all");
    check(std::ranges::all_of(empty.depth,[](float value) { return value == 0; }),
          "deleting all SceneLit geometry publishes uniform background depth");
    f.frame.scene_viewport = {};
    const auto ui_only = f.capture("14-ui-only");
    check(std::ranges::all_of(ui_only.depth,[](float value) { return value == 0; }) && pixel(ui_only,12,12)[1] == 255,
          "UI-only capture after SceneLit never exposes retained scene depth");
    check(f.metrics.good(), "SceneLit image verification evidence written");
    std::puts("Renderer SceneLit acceptance: PASS (four shadow lights, HDR RGB/materials, DDGI update, GTAO indirect, SSSR/fallback, temporal resets, orthographic, modes, empty/UI depth)");
}
#include "scene_lit_motion.inl"
#include "scene_lit_signal_edits.inl"

void temporal_isolation(Fixture& f) {
    quality_signal_edits(f);
    quality_motion_history(f);
    const auto defaults=f.frame.view.lit;
    constexpr const char* names[]{"all","no-sssr","no-ddgi","direct-only"};
    bool stable=true;
    for (unsigned mode=0;mode<4;++mode) {
        f.frame.view.lit=defaults;
        if(mode==1 || mode>=3) f.frame.view.lit.sssr=false;
        if(mode>=2) f.frame.view.lit.ddgi=false;
        f.edited();
        const auto first=f.capture((std::string(names[mode])+"-0").c_str(),warmup_frames);
        const auto second=f.capture((std::string(names[mode])+"-1").c_str(),8);
        const auto delta=f.compare(names[mode],first,second);
        stable&=delta.mean<=3 && delta.p95<=8;
        check(f.step().stats.scene_lit_history_resets==0,"static temporal isolation retains scene histories");
    }
    check(stable,"all SceneLit temporal isolation configurations meet the unchanged stability bounds");
    f.frame.view.lit=defaults;f.edited();
    const auto before_motion=f.capture("rigid-before",warmup_frames);
    render::Capture moving,stopped;
    for(unsigned index=1;index<=16;++index) {
        f.room.meshes[4].world[12]=float(index)*.1f;
        check(f.step(index==16?&moving:nullptr).stats.scene_lit_history_resets==0,
              "continuous rigid motion updates AS/probes without resetting the stable scene domain");
    }
    f.step(&stopped);
    const auto after_motion=f.capture("rigid-after",7);
    check(f.compare("rigid-motion-light-response",before_motion,after_motion).changed>=64,
          "continuous rigid motion changes real receiver illumination with stable object identity");
    // A camera cut rebuilds every instance from its current transform, so this
    // reference cannot inherit a stale previous transform from the motion above.
    f.frame.camera_cut=true;
    const auto still=f.capture("rigid-still");
    const auto motion_values=std::size_t(still.extent.width)*still.extent.height*2;
    check(moving.motion.size()==motion_values&&stopped.motion.size()==motion_values&&still.motion.size()==motion_values,
          "Scene Lit captures publish per-pixel surface motion");
    std::size_t moving_pixels=0,stale_pixels=0;
    for(std::size_t i=0;i<motion_values;i+=2) {
        const auto delta=[&](const render::Capture& c) {
            return std::max(std::abs(c.motion[i]-still.motion[i]),std::abs(c.motion[i+1]-still.motion[i+1]));
        };
        moving_pixels+=delta(moving)>1e-3f;stale_pixels+=delta(stopped)>1e-4f;
    }
    std::printf("SceneLit rigid stop: moving pixels %zu, stale motion pixels after stopping %zu\n",moving_pixels,stale_pixels);
    check(moving_pixels>=64,"a moving object writes surface motion");
    check(stale_pixels==0,"the first frame after an object stops has the same surface motion as a still reference");
    std::puts("Renderer SceneLit temporal isolation: PASS");
}

Difference region_difference(const render::Capture& a, const render::Capture& b, Region region) {
    Difference result;
    std::vector<unsigned> deltas;
    for (unsigned y=region.top;y<region.bottom;++y)
        for (unsigned x=region.left;x<region.right;++x) {
            const auto index=std::size_t(y)*a.extent.width+x;
            // These projected ROIs lie inside fixed receiver faces. Temporal
            // AA jitter changes the sampled positions and their device depth;
            // requiring equal depth would discard the pixels being evaluated.
            if (a.depth[index]<=0 || b.depth[index]<=0) continue;
            unsigned delta=0;
            for (unsigned channel=0;channel<3;++channel)
                delta=std::max(delta,unsigned(std::abs(int(a.rgba[index*4+channel])-int(b.rgba[index*4+channel]))));
            result.mean+=delta; result.changed+=delta>=4; deltas.push_back(delta);
        }
    check(!deltas.empty(),"SceneLit quality ROI contains comparable real receiver pixels");
    result.mean/=static_cast<double>(deltas.size());
    std::ranges::sort(deltas); result.p95=deltas[(deltas.size()-1)*95/100];
    return result;
}

void quality_shape(const render::Capture& image) {
    std::size_t visible=0, illuminated=0;
    for (std::size_t index=0;index<image.depth.size();++index) {
        if (image.depth[index]<=0) continue;
        ++visible;
        illuminated+=unsigned(image.rgba[index*4])+image.rgba[index*4+1]+image.rgba[index*4+2]>12;
    }
    check(visible>1000 && illuminated>visible/2,
          "every SceneLit AA policy preserves actual visible geometry and avoids a black frame");
    check(depth(image,12,12)==0 && pixel(image,12,12)[1]==255,
          "SceneLit AA leaves the ordinary HUD outside the scene viewport intact");
}

render::Capture quality_static(Fixture& f, const std::string& name, Region reflection, Region penumbra) {
    auto previous=f.capture((name+"-begin").c_str(),warmup_frames);
    mean_rgb(previous,reflection); mean_rgb(previous,penumbra);
    for (unsigned index=0;index<16;++index) {
        render::Capture current;
        check(f.step(&current).stats.scene_lit_history_resets==0,
              "static quality sampling retains the accepted scene domain");
        check(previous.depth==current.depth,
              "static Capture depth follows the public unjittered camera for every AA policy");
        const auto reflected=region_difference(previous,current,reflection);
        const auto shadowed=region_difference(previous,current,penumbra);
        f.metrics<<name<<"-reflection-"<<index<<','<<reflected.mean<<','<<reflected.p95<<','<<reflected.changed<<",0\n"
                 <<name<<"-penumbra-"<<index<<','<<shadowed.mean<<','<<shadowed.p95<<','<<shadowed.changed<<",0\n";
        if (reflected.mean>3 || reflected.p95>8 || shadowed.mean>3 || shadowed.p95>8) {
            png(previous,f.output/(name+"-failure-previous.png"));
            png(current,f.output/(name+"-failure-current.png"));
            std::ofstream samples(f.output/(name+"-failure-receivers.csv"));
            samples<<"x,y,previous_depth,current_depth,previous_r,previous_g,previous_b,current_r,current_g,current_b\n";
            for (const auto& roi : {reflection,penumbra})
                for (auto y=roi.top;y<roi.bottom;++y) for (auto x=roi.left;x<roi.right;++x) {
                    samples<<x<<','<<y<<','<<depth(previous,x,y)<<','<<depth(current,x,y);
                    for (const auto* image : {&previous,&current}) for (unsigned c=0;c<3;++c) samples<<','<<unsigned(pixel(*image,x,y)[c]);
                    samples<<'\n';
                }
        }
        check(reflected.mean<=3 && reflected.p95<=8,
              "converged reflection ROI meets the existing temporal stability bounds on consecutive frames");
        check(shadowed.mean<=3 && shadowed.p95<=8,
              "converged broad-light receiver ROI meets the existing temporal stability bounds on consecutive frames");
        previous=std::move(current);
    }
    png(previous,f.output/(name+"-end.png"));
    return previous;
}

void quality_depth(Fixture& f) {
    constexpr float near_plane=.1f,far_plane=2000.f;
    const auto mesh=f.room.meshes[0];
    f.room.meshes={mesh,mesh};
    f.room.meshes[0].world=box(-.15f,0,.5f,.2f,.2f,.2f);
    f.room.meshes[1].world=box(350,0,1000,200,200,200);
    f.room.meshes[0].stable_key=201; f.room.meshes[1].stable_key=202;
    f.frame.view.lit.antialiasing=render::Antialiasing::none;
    f.frame.view.lit.ddgi=f.frame.view.lit.gtao=f.frame.view.lit.sssr=false;
    f.frame.camera={};
    const float aspect=f.frame.scene_viewport.width/f.frame.scene_viewport.height;
    f.frame.camera.projection={1.8f/aspect,0,0,0, 0,1.8f,0,0,
        0,0,-near_plane/(far_plane-near_plane),1, 0,0,near_plane*far_plane/(far_plane-near_plane),0};
    f.frame.camera_cut=true; f.edited();
    const auto perspective=f.capture("quality-depth-near-far",16);
    const auto near_roi=f.region({-.15f,0,.4f}),far_roi=f.region({350,0,900});
    mean_rgb(perspective,near_roi); mean_rgb(perspective,far_roi);
    const float near_depth=depth(perspective,(near_roi.left+near_roi.right)/2,(near_roi.top+near_roi.bottom)/2);
    const float far_depth=depth(perspective,(far_roi.left+far_roi.right)/2,(far_roi.top+far_roi.bottom)/2);
    check(near_depth>far_depth && far_depth>0 && near_depth<1 && far_depth<.001f,
          "finite Reversed-Z preserves a visible distant FP32 receiver below .001 and orders near greater than far");

    // Two nearly coplanar remote front faces exercise the real depth compare:
    // the farther face is submitted last and must not replace the nearer one.
    auto farther=f.room.meshes[1]; farther.world[14]+=.02f; farther.stable_key=203;
    f.room.meshes.push_back(farther); f.edited();
    const auto coplanar=f.capture("quality-depth-far-coplanar");
    check(depth(coplanar,(far_roi.left+far_roi.right)/2,(far_roi.top+far_roi.bottom)/2)==far_depth,
          "distant nearly coplanar geometry submitted last cannot overwrite the nearer FP32 reversed depth");
    f.room.meshes.erase(f.room.meshes.begin()+1);f.edited();
    const auto remote_farther=f.capture("quality-depth-far-separated");
    const float farther_depth=depth(remote_farther,(far_roi.left+far_roi.right)/2,(far_roi.top+far_roi.bottom)/2);
    check(farther_depth>0 && farther_depth<far_depth,
          "FP32 Reversed-Z distinguishes remote surfaces separated by two centimeters");

    f.room.meshes[0].world=box(0,0,.02f,.01f,.01f,.01f);
    f.room.meshes[1].world=box(350,0,2100,100,100,100);f.edited();
    const auto perspective_clipped=f.capture("quality-depth-perspective-clipped");
    check(std::ranges::all_of(perspective_clipped.depth,[](float value){return value==0;}),
          "finite perspective Reversed-Z preserves both authored clipping boundaries");

    f.room.meshes.resize(2);
    f.room.meshes[0].world=box(-1,0,1,1,1,1);
    f.room.meshes[1].world=box(1,0,30,1,1,1);
    f.frame.camera.projection={1/(2.f*aspect),0,0,0, 0,.5f,0,0,
        0,0,-1/39.9f,0, 0,0,40/39.9f,1};
    f.frame.camera_cut=true;f.edited();
    const auto orthographic=f.capture("quality-depth-orthographic",16);
    const auto ortho_near=f.region({-1,0,.5f}),ortho_far=f.region({1,0,29.5f});
    mean_rgb(orthographic,ortho_near);mean_rgb(orthographic,ortho_far);
    check(depth(orthographic,(ortho_near.left+ortho_near.right)/2,(ortho_near.top+ortho_near.bottom)/2)>
          depth(orthographic,(ortho_far.left+ortho_far.right)/2,(ortho_far.top+ortho_far.bottom)/2),
          "orthographic Reversed-Z uses the same near-greater-than-far depth contract");

    f.room.meshes[0].world=box(-1,0,.02f,.01f,.01f,.01f);
    f.room.meshes[1].world=box(1,0,42,1,1,1);f.edited();
    const auto clipped=f.capture("quality-depth-clipped");
    check(std::ranges::all_of(clipped.depth,[](float value){return value==0;}),
          "finite Reversed-Z clips geometry beyond both near and far planes to background zero");
}

// Mesh emission reaches receivers only through DDGI probe hits. Every probe
// updates each frame and each measurement settles before averaging.
void quality_emissive_ddgi(Fixture& f) {
    using atelier::geometry::Vertex;
    // Both representations cover precisely the same quad. The off-centre fan
    // has unequal triangle areas; probe hits must not weight emission by them.
    const auto emitter_geometry=[](bool fan, bool reversed=false) {
        const std::array<Vec3,4> corners{{{1,-1,2},{3,-1,2},{3,1,2},{1,1,2}}};
        std::vector<Vertex> vertices;
        const auto triangle=[&](Vec3 a,Vec3 b,Vec3 c) {
            if(reversed) std::swap(b,c);
            for(const auto p:{a,b,c}) vertices.push_back({{p[0],p[1],p[2]},{0,0,reversed?1.f:-1.f}});
        };
        if(fan) for(unsigned i=0;i<4;++i) triangle(corners[i],corners[(i+1)%4],{1.1f,-.85f,2});
        else {triangle(corners[0],corners[1],corners[2]);triangle(corners[0],corners[2],corners[3]);}
        return std::make_shared<const atelier::geometry::Mesh>(std::move(vertices));
    };
    f.room.meshes.resize(2);
    auto& receiver=f.room.meshes[0];
    receiver={};receiver.stable_key=1;receiver.world=box(0,0,0,1,1,1);
    receiver.geometry=std::make_shared<const atelier::geometry::Mesh>(std::vector<Vertex>{
        {{-3,-2,4},{0,0,-1}},{{3,-2,4},{0,0,-1}},{{3,2,4},{0,0,-1}},
        {{-3,-2,4},{0,0,-1}},{{3,2,4},{0,0,-1}},{{-3,2,4},{0,0,-1}}});
    receiver.color={.5f,.5f,.5f,1};receiver.material={.roughness=1};
    auto& emitter=f.room.meshes[1];
    emitter={};emitter.stable_key=2;emitter.world=box(0,0,0,1,1,1);
    emitter.geometry=emitter_geometry(false);emitter.color={0,0,0,1};
    emitter.material={.roughness=1,.metallic=1};
    f.lights.clear();f.edited();++f.frame.scene_identity;
    f.frame.environment={0,0,0};
    auto& options=f.frame.view.lit;
    options={};options.gtao=options.sssr=options.ssil=options.bloom=false;
    options.antialiasing=render::Antialiasing::none;
    // The receiver plane at z=4 lies between the last two probe slices.
    options.probe_volumes[0].minimum={-4,-3,-.5f};options.probe_volumes[0].maximum={4,3,4.6f};
    options.probe_volumes[0].counts={9,7,7};options.probe_update_budget=441;options.probe_rays=128;
    f.frame.camera.view={1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,4,1};
    const float aspect=f.frame.scene_viewport.width/f.frame.scene_viewport.height;
    f.frame.camera.projection={1/(2.5f*aspect),0,0,0, 0,1/2.5f,0,0, 0,0,-1/39.9f,0, 0,0,40/39.9f,1};
    f.frame.camera_cut=true;
    // This broad, unoccluded receiver remains visible when the blocker is
    // inserted between it and the emitter, away from the camera ray.
    const auto region=project_region(f.frame.camera,f.frame.scene_viewport,f.drawing.extent,{-.7f,0,4},16);
    const auto measure=[&](const char* name,unsigned settle=192) {
        const double value=lighting_mean(f,name,region,settle,8)[0];
        f.metrics<<name<<','<<value<<",0,0,0\n";
        std::printf("SceneLit %s receiver red=%.4f\n",name,value);
        return value;
    };
    const double dark=measure("mesh-emission-none",8);
    f.room.meshes[1].material.emissive={12,12,12};f.edited();
    const double lit=measure("mesh-emission-ddgi");
    check(dark<=1 && lit>=24 && lit<=200,"mesh emission lights an unsaturated real receiver through converged DDGI");
    options.ddgi=false;
    const double no_gi=measure("mesh-emission-ddgi-off",8);
    check(no_gi<=dark+1,"without DDGI, mesh emission lights no other surface");
    options.ddgi=true;
    f.room.meshes[1].geometry=emitter_geometry(true);f.edited();
    const double triangulated=measure("mesh-emission-unequal-area-fan");
    check(std::abs(triangulated-lit)<=4,"equal emitting area retains energy under unequal-area retriangulation");
    const auto original_geometry=emitter_geometry(false);
    std::vector<Vertex> transformed(original_geometry->vertices().begin(),original_geometry->vertices().end());
    for(auto& vertex:transformed){vertex.position[0]*=-.5f;vertex.position[1]*=2;}
    f.room.meshes[1].geometry=std::make_shared<const atelier::geometry::Mesh>(std::move(transformed));
    f.room.meshes[1].world=box(0,0,0,-2,.5f,1);f.edited();
    const double scaled=measure("mesh-emission-mirrored-nonuniform-transform");
    check(std::abs(scaled-lit)<=4,"world-space emitter area and radiance survive mirrored nonuniform instance transforms");
    f.room.meshes[1].world=box(0,0,0,1,1,1);
    f.room.meshes[1].geometry=emitter_geometry(false,true);f.edited();
    const double reversed=measure("mesh-emission-reversed-winding");
    check(std::abs(reversed-lit)<=4,"two-sided mesh emission retains receiver energy when emitter winding is reversed");
    f.room.meshes[1].geometry=emitter_geometry(false);
    f.room.meshes[1].material.emissive={24,24,24};f.edited();
    const double doubled=measure("mesh-emission-double-radiance");
    check(doubled>=lit+12,"probe-hit emission scales with authored radiance");
    f.room.meshes[1].material.emissive={12,12,12};
    render::MeshInstance blocker;
    blocker.stable_key=3;blocker.geometry=atelier::geometry::cube_mesh();
    blocker.world=box(.65f,0,3,1.6f,2.4f,.05f);blocker.color={0,0,0,1};
    blocker.material={.roughness=1,.metallic=1};
    f.room.meshes.push_back(blocker);f.edited();
    const double blocked=measure("mesh-emission-occluded");
    check(blocked<lit*.5,"geometry between emitter and receiver blocks probe-carried emission");
    f.room.meshes.pop_back();f.edited();
    const double restored=measure("mesh-emission-blocker-removed");
    check(std::abs(restored-lit)<=4,"removing the blocker restores probe-carried emission");
    f.room.meshes.pop_back();f.edited();
    const double removed=measure("mesh-emission-emitter-removed");
    check(removed<=dark+1,"removing emissive geometry removes its light without stale probe energy");
    check(f.metrics.good(),"mesh emission real-renderer energy and visibility evidence written");
}

void quality_ddgi(Fixture& f) {
    quality_emissive_ddgi(f);
    // Minimal real scene for the reported dirty wall/floor junction and moving
    // bright emissive mesh. Use the product's default probe field, not the
    // denser field used by the older general lighting fixture.
    f.room.meshes.resize(3);
    for (std::size_t i=0;i<f.room.meshes.size();++i) {
        auto& mesh=f.room.meshes[i];
        mesh.geometry=atelier::geometry::cube_mesh();mesh.stable_key=i+1;
        mesh.color={.42f,.43f,.46f,1};mesh.material={.roughness=.82f};
    }
    f.room.meshes[0].world=box(0,-.25f,.5f,10,.5f,10);
    f.room.meshes[1].world=box(0,1.6f,5.5f,10,3.2f,.24f);
    f.room.meshes[2].world=box(3.6844656f,1.2760136f,.10473156f,1.0767804f,1.4f,6.205793f);
    f.room.meshes[2].color={.12f,.03f,.01f,1};
    f.room.meshes[2].material.emissive={55.506668f,.45f,.06f};
    f.lights.clear();f.edited();++f.frame.scene_identity;
    auto& options=f.frame.view.lit;
    options={};options.gtao=options.sssr=options.bloom=false;
    f.frame.environment={.025f,.035f,.055f};
    options.antialiasing=render::Antialiasing::none;
    f.frame.camera.view={1,0,0,0, 0,1,0,0, 0,0,1,0, 0,-1.6f,8,1};
    const float aspect=f.frame.scene_viewport.width/f.frame.scene_viewport.height;
    f.frame.camera.projection={1/(3*aspect),0,0,0, 0,1/3.f,0,0, 0,0,-1/39.9f,0, 0,0,40/39.9f,1};
    f.frame.camera_cut=true;
    auto settled=f.capture("ddgi-emissive-wall-before",192);
    std::ofstream profile(f.output/"ddgi-wall-profile.csv");
    profile<<"x,y,red,green,blue\n";
    std::vector<double> seam_ratios;
    for (unsigned column=0;column<61;++column) {
        const float x=-4.5f+float(column)*.1f;
        double near_red=0,above_red=0;
        for (const float y : {.035f,.10f,.2f,.35f,.6f,1.f,1.6f,2.4f}) {
            const auto region=project_region(f.frame.camera,f.frame.scene_viewport,f.drawing.extent,{x,y,5.38f},1);
            const auto rgb=mean_rgb(settled,region);
            profile<<x<<','<<y<<','<<rgb[0]<<','<<rgb[1]<<','<<rgb[2]<<'\n';
            if(y==.035f) near_red=rgb[0];
            if(y==.35f) above_red=rgb[0];
        }
        seam_ratios.push_back(near_red/std::max(above_red,1.0));
    }
    const double seam_p10=percentile(seam_ratios,10);
    std::printf("SceneLit DDGI wall/floor red ratio p10=%.4f (minimum .95)\n",seam_p10);
    f.metrics<<"ddgi-seam-ratio-p10,"<<seam_p10<<",0,0,0\n";f.metrics.flush();
    const auto upper_left=project_region(f.frame.camera,f.frame.scene_viewport,f.drawing.extent,{-4.4f,3,5.38f},1);
    const auto lower_right=project_region(f.frame.camera,f.frame.scene_viewport,f.drawing.extent,{1.4f,.05f,5.38f},1);
    const Region wall{upper_left.left,upper_left.top,lower_right.right,lower_right.bottom,upper_left.expected_depth};
    double static_mean=0;unsigned static_p95=0;
    for(unsigned i=0;i<16;++i) {
        render::Capture current;f.step(&current);
        const auto delta=region_difference(settled,current,wall);
        static_mean=std::max(static_mean,delta.mean);static_p95=std::max(static_p95,delta.p95);
        settled=std::move(current);
    }
    std::printf("SceneLit DDGI whole-wall static max mean=%.4f p95=%u\n",static_mean,static_p95);
    f.metrics<<"ddgi-whole-wall-static-max,"<<static_mean<<','<<static_p95<<",0,0\n";
    png(settled,f.output/"ddgi-emissive-wall-static.png");
    const auto before_motion=settled;
    double moving_mean=0;unsigned moving_p95=0;
    for(unsigned i=1;i<=32;++i) {
        f.room.meshes[2].world[12]=3.6844656f-float(i)/32;
        f.room.meshes[2].material.emissive[0]=55.506668f*(1-float(i)/64);
        f.edited();
        render::Capture current;
        const auto result=f.step(&current);
        check(result.stats.scene_lit_history_resets==0,"moving emissive mesh keeps the scene's history domain");
        const auto delta=region_difference(settled,current,wall);
        moving_mean=std::max(moving_mean,delta.mean);moving_p95=std::max(moving_p95,delta.p95);
        f.metrics<<"ddgi-emissive-moving-"<<i<<','<<delta.mean<<','<<delta.p95<<','<<delta.changed<<",0\n";
        if(i%4==0) png(current,f.output/("ddgi-emissive-moving-"+std::to_string(i)+".png"));
        settled=std::move(current);
    }
    const auto converged=f.capture("ddgi-emissive-wall-converged",192);
    check(region_difference(before_motion,converged,wall).changed>=64,
          "emissive edits visibly change the fixed wall receiver instead of freezing its old illumination");
    std::printf("SceneLit DDGI slow-emitter whole-wall max mean=%.4f p95=%u\n",moving_mean,moving_p95);
    f.metrics<<"ddgi-whole-wall-moving-max,"<<moving_mean<<','<<moving_p95<<",0,0\n";
    check(region_difference(converged,f.capture("ddgi-emissive-wall-convergence-check",64),wall).p95<=8,
          "moving emitter reaches a stable endpoint");
    // A divider extends past the entire probe volume: the left receiver must
    // stay dark while the bright emitter lights the right. This guards against
    // hiding contact artifacts by discarding visibility or adding ambient light.
    auto divider=f.room.meshes[1];divider.stable_key=4;
    divider.world=box(0,1.6f,0,.12f,20,24);divider.color={0,0,0,1};
    f.room.meshes.push_back(divider);f.edited();++f.frame.scene_identity;
    f.frame.environment={0,0,0};
    const auto divided=f.capture("ddgi-thin-divider",192);
    const auto dark=mean_rgb(divided,f.region({-.35f,1.6f,5.38f}));
    const auto bright=mean_rgb(divided,f.region({1.3f,2.6f,5.38f}));
    std::printf("SceneLit DDGI divider dark red=%.4f, lit red=%.4f\n",dark[0],bright[0]);
    f.metrics<<"ddgi-divider-dark-red,"<<dark[0]<<",0,0,0\n"
             <<"ddgi-divider-lit-red,"<<bright[0]<<",0,0,0\n";f.metrics.flush();
    // Place a thinner-than-pixel divider between adjacent pixel centers.
    // Both visible receivers are the same back-wall plane: normal/depth-only
    // spatial filtering would blend across the unseen intervening wall.
    const float world_pixel=6.f/f.frame.scene_viewport.height;
    f.room.meshes.back().world=box(world_pixel,1.6f,0,world_pixel*.4f,20,24);
    f.edited();++f.frame.scene_identity;
    const auto subpixel=f.capture("ddgi-subpixel-divider",192);
    const auto dark_edge=mean_rgb(subpixel,project_region(f.frame.camera,f.frame.scene_viewport,
        f.drawing.extent,{0,1.6f,5.38f},0));
    const auto bright_edge=mean_rgb(subpixel,project_region(f.frame.camera,f.frame.scene_viewport,
        f.drawing.extent,{world_pixel*2,1.6f,5.38f},0));
    std::printf("SceneLit DDGI subpixel divider dark red=%.4f, lit red=%.4f\n",dark_edge[0],bright_edge[0]);
    f.metrics<<"ddgi-subpixel-divider-dark-red,"<<dark_edge[0]<<",0,0,0\n"
             <<"ddgi-subpixel-divider-lit-red,"<<bright_edge[0]<<",0,0,0\n";f.metrics.flush();
    check(profile.good() && f.metrics.good(),"DDGI whole-wall evidence written");
    check(seam_p10>=.95,"DDGI visible wall/floor contact retains illumination without a false dark strip");
    check(static_mean<=3 && static_p95<=8,"converged bright-emitter whole-wall temporal stability");
    check(moving_mean<=3 && moving_p95<=8,"slow emissive motion does not expose budgeted probe update blocks");
    check(dark[0]<=8 && bright[0]>=64,"thin divider retains visibility rejection under bright emissive lighting");
    check(dark_edge[0]<=8 && bright_edge[0]>=64,
          "lighting reconstruction does not cross a subpixel blocker between coplanar receivers");
    std::puts("Renderer SceneLit DDGI quality: PASS");
}

#include "scene_lit_work.inl"
#include "scene_lit_lights.inl"
#include "scene_lit_clustered.inl"
#include "scene_lit_rectangle.inl"
#include "scene_lit_cost.inl"
#include "scene_lit_shadow.inl"
#include "scene_lit_contracts.inl"
#include "scene_lit_audit.inl"
#include "scene_lit_debug.inl"
#include "scene_lit_stability.inl"

void quality_smaa_t2x_coverage(Fixture& f) {
    const auto meshes=f.room.meshes;
    const auto lights=f.lights;
    const auto view=f.frame.view;
    const auto environment=f.frame.environment;
    f.room.meshes={meshes[4]};f.lights.clear();f.frame.environment={};
    auto& object=f.room.meshes.front();
    object.world=box(0,1.2f,3,1.4f,2,1);
    object.world[0]=1.4f*std::cos(.3f);object.world[1]=1.4f*std::sin(.3f);
    object.world[4]=-2*std::sin(.3f);object.world[5]=2*std::cos(.3f);
    object.material.emissive={.4f,.18f,.07f};
    auto& options=f.frame.view.lit;
    options.antialiasing=render::Antialiasing::smaa_t2x;
    options.ddgi=options.gtao=options.sssr=options.ssil=options.bloom=false;
    f.frame.camera_cut=true;f.edited();
    const auto stable=[&](const char* name) {
        const auto a=f.capture((std::string(name)+"-a").c_str(),4);
        const auto b=f.capture((std::string(name)+"-b").c_str());
        const auto c=f.capture((std::string(name)+"-c").c_str());
        unsigned maximum=0;std::size_t visible=0;
        for(std::size_t i=0;i<a.depth.size();++i) {
            visible+=a.depth[i]>0;
            for(unsigned channel=0;channel<3;++channel) {
                const auto index=i*4+channel;
                maximum=std::max({maximum,unsigned(std::abs(int(a.rgba[index])-int(b.rgba[index]))),
                    unsigned(std::abs(int(b.rgba[index])-int(c.rgba[index])))});
            }
        }
        std::printf("SceneLit %s: maximum consecutive RGB8 delta=%u visible=%zu\n",name,maximum,visible);
        check(visible>256,"T2x coverage fixture contains a visible emissive silhouette");
        check(a.depth==b.depth && b.depth==c.depth,"T2x public depth and static geometry remain fixed");
        check(maximum<=2,"T2x combines complementary static silhouette coverage without phase flicker");
    };
    stable("quality-t2x-coverage");
    object.world[12]+=.3f;f.edited();f.step();
    stable("quality-t2x-motion-settled");
    // A new identity at the same transform has invalid surface correspondence,
    // but still has raster coverage and must not reintroduce sampling jitter.
    ++object.stable_key;f.edited();
    stable("quality-t2x-new-identity");
    f.room.meshes=meshes;f.lights=lights;f.frame.view=view;f.frame.environment=environment;
    f.frame.camera_cut=true;f.edited();
}

void quality_cases(Fixture& f) {
    quality_smaa_t2x_coverage(f);
    const auto meshes=f.room.meshes;
    const auto lights=f.lights;
    const auto defaults=f.frame.view.lit;
    const auto camera=f.frame.camera;
    constexpr std::array modes{render::Antialiasing::none,render::Antialiasing::taa,
                              render::Antialiasing::smaa,render::Antialiasing::smaa_t2x};
    constexpr const char* names[]{"none","taa","smaa","smaa-t2x"};
    // The reflected emissive panel projects onto the foreground floor. The
    // second receiver is separate from this specular footprint, below the
    // broad rectangle emitter and adjacent to the central occluder.
    const auto reflection=f.region({0,.01f,.4f});
    // Keep the entire ROI on the floor even at the extreme jitter phase;
    // this point remains inside the rectangle emitter's geometric penumbra.
    const auto penumbra=f.region({.85f,.01f,3.6f});
    std::ofstream trajectory(f.output/"quality-light-trajectory.csv");
    trajectory<<"aa,accepted_frame,rectangle_x,rectangle_intensity,history_resets\n";
    for (std::size_t mode=0;mode<modes.size();++mode) {
        f.room.meshes=meshes;f.lights=lights;f.frame.camera=camera;
        f.frame.view.lit=defaults;f.frame.view.lit.bloom=false;
        f.frame.view.lit.antialiasing=modes[mode];
        f.frame.camera_cut=true;f.edited();
        const auto prefix=std::string("quality-")+names[mode];
        const auto before=quality_static(f,prefix+"-static",reflection,penumbra);
        quality_shape(before);
        for (unsigned index=1;index<=32;++index) {
            const float progress=float(index)/32;
            f.lights[3].position[0]=1.5f*progress;
            f.lights[3].intensity=lights[3].intensity*(1+progress);
            f.edited();
            const auto result=f.step();
            trajectory<<names[mode]<<','<<result.accepted_id<<','<<f.lights[3].position[0]<<','
                      <<f.lights[3].intensity<<','<<result.stats.scene_lit_history_resets<<'\n';
            check(result.stats.scene_lit_history_resets==0,
                  "continuous stable-key light movement and intensity edits preserve global effect histories");
        }
        // Moving lighting is an expected signal, not temporal noise. Noise is
        // measured only after the endpoint has remained fixed and converged.
        const auto moved=f.capture((prefix+"-light-endpoint").c_str());quality_shape(moved);
        const auto settled=quality_static(f,prefix+"-converged",reflection,penumbra);
        check(f.compare((prefix+"-lighting-response").c_str(),before,settled).changed>=64,
              "continuous light edits reach a visibly changed converged illumination");
        f.compare((prefix+"-endpoint-convergence").c_str(),moved,settled);

        // Geometry motion uses the same stable scene domain, while an actual
        // camera cut rejects that domain's image histories exactly once.
        f.room.meshes[4].world[12]+=1;f.edited();
        check(f.step().stats.scene_lit_history_resets==0,
              "geometry motion preserves the scene domain while invalidating affected signal support");
        const auto geometry=f.capture((prefix+"-geometry-recovery").c_str(),32);quality_shape(geometry);
        f.frame.camera.view[12]+=.002f;
        check(f.step().stats.scene_lit_history_resets==0,
              "subpixel camera motion does not create a replacement scene domain");
        f.frame.camera=camera;f.frame.camera_cut=true;
        render::Capture cut;
        check(f.step(&cut).stats.scene_lit_history_resets>0,
              "each AA policy rejects incompatible camera-cut history before accepting the frame");
        quality_shape(cut);png(cut,f.output/(prefix+"-camera-cut.png"));
        check(f.step().stats.scene_lit_history_resets==0,"camera-cut history recovers on the following accepted frame");
    }

    // Direct light alone has no estimator or history: once the shadow atlas is
    // warm, the receiver is identical in every frame.
    f.room.meshes=meshes;f.lights=lights;f.frame.camera=camera;f.edited();
    auto& options=f.frame.view.lit;
    options=defaults;options.antialiasing=render::Antialiasing::none;
    options.ddgi=options.gtao=options.sssr=options.ssil=options.bloom=false;
    const auto reference=lighting_mean(f,"quality-direct-converged",penumbra,warmup_frames,32);
    const auto reference_later=f.capture("quality-direct-depth");
    const auto direct_delta=f.compare("quality-direct-deterministic",f.capture("quality-direct-0"),f.capture("quality-direct-1",8));
    check(direct_delta.mean==0&&direct_delta.p95==0,"settled direct lighting is identical across frames");
    // Legacy mesh index 4 used to occupy the same scalar key as an authored
    // UINT64_MAX-4. Both blockers must survive in their distinct object domains.
    const auto central_key=f.room.meshes[4].stable_key;
    const auto metal_key=f.room.meshes[5].stable_key;
    f.room.meshes[4].stable_key=0;
    f.room.meshes[5].stable_key=std::numeric_limits<std::uint64_t>::max()-4;
    f.edited();
    const auto distinct_mean=lighting_mean(f,"quality-legacy-authored-key-domains",penumbra,1,32);
    const auto distinct_keys=f.capture("quality-legacy-authored-key-depth");
    check(distinct_keys.depth==reference_later.depth,
          "legacy fallback and authored uint64 keys preserve both blockers and their exact geometry depth");
    for(unsigned channel=0;channel<3;++channel)
        check(std::abs(distinct_mean[channel]-reference[channel])<=3,
              "legacy fallback and authored uint64 keys preserve the reference lighting mean");
    f.room.meshes[4].stable_key=central_key;f.room.meshes[5].stable_key=metal_key;f.edited();
    quality_direct_radiance_response(f);
    quality_depth(f);
    quality_ddgi(f);
    quality_contracts(f);
    check(f.metrics.good() && trajectory.good(),"SceneLit quality evidence and accepted-frame trajectory written");
    std::puts("Renderer SceneLit quality: PASS (four AA policies, separate reflection/penumbra temporal ROIs, continuous light edits, convergence, deterministic direct light and immediate radiance edits, geometry/camera recovery, reversed depth, DDGI mesh emission, emissive DDGI wall continuity/motion/divider)");
}
} // namespace scene_lit_e2e

int scene_lit_acceptance(bool benchmark_mode, const std::filesystem::path& output, bool temporal_only, bool quality_only) {
    scene_lit_e2e::Fixture fixture(benchmark_mode,output);
    if (benchmark_mode) scene_lit_e2e::benchmark(fixture);
    else if(quality_only) scene_lit_e2e::quality_cases(fixture);
    else if(temporal_only) scene_lit_e2e::temporal_isolation(fixture);
    else {
        scene_lit_e2e::environment_cases(fixture);scene_lit_e2e::image_cases(fixture);
        fixture.restore();scene_lit_e2e::light_set_cases(fixture);
        fixture.restore();scene_lit_e2e::probe_debug_cases(fixture);
        fixture.restore();scene_lit_e2e::quality_audit(fixture);
    }
    fixture.close();
    return 0;
}
