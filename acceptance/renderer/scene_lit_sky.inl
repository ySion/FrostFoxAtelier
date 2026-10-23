// Directional sky (rendering::SkyMap). Expectations come from the documented map
// space, not from the renderer: row 0 is +Y, u = 0.5 faces +Z, u = 0.75 faces +X,
// and a positive sky_rotation turns +X toward -Z like a Transform yaw.
using SkyDirection = std::array<double, 3>;
template<class Radiance> atelier::rendering::SkyMapRef sky_map(std::uint32_t width, Radiance&& radiance) {
    const std::uint32_t height = width / 2;
    std::vector<float> rgb(std::size_t(width) * height * 3);
    for (std::uint32_t y = 0; y < height; ++y) for (std::uint32_t x = 0; x < width; ++x) {
        const double longitude = 2*std::numbers::pi*((x+.5)/width-.5), latitude = std::numbers::pi*(.5-(y+.5)/height);
        const Vec3 value = radiance(SkyDirection{std::sin(longitude)*std::cos(latitude), std::sin(latitude),
                                                 std::cos(longitude)*std::cos(latitude)});
        for (unsigned c = 0; c < 3; ++c) rgb[(std::size_t(y)*width+x)*3+c] = value[c];
    }
    return std::make_shared<const atelier::rendering::SkyMap>(width, height, rgb);
}
SkyDirection sky_yawed(const SkyDirection& d, double degrees) {
    const double a = degrees*std::numbers::pi/180;
    return {d[0]*std::cos(a)+d[2]*std::sin(a), d[1], -d[0]*std::sin(a)+d[2]*std::cos(a)};
}
SkyDirection sky_direction(double longitude_degrees, double latitude_degrees) {
    const double lon = longitude_degrees*std::numbers::pi/180, lat = latitude_degrees*std::numbers::pi/180;
    return {std::sin(lon)*std::cos(lat), std::sin(lat), std::cos(lon)*std::cos(lat)};
}
render::Camera sky_look(Vec3 eye, Vec3 forward, Vec3 up, float aspect, float scale) {
    const auto z = normalize(forward), x = normalize(cross(up, z)), y = cross(z, x);
    render::Camera result;
    result.view = {x[0],y[0],z[0],0, x[1],y[1],z[1],0, x[2],y[2],z[2],0, -dot(x,eye),-dot(y,eye),-dot(z,eye),1};
    result.projection = {scale/aspect,0,0,0, 0,scale,0,0, 0,0,-.1f/39.9f,1, 0,0,4/39.9f,0};
    return result;
}
std::array<double, 2> sky_pixel(const render::Frame& frame, const SkyDirection& direction) {
    std::array<double, 4> value{direction[0], direction[1], direction[2], 0};
    for (const auto& matrix : {frame.camera.view, frame.camera.projection}) {
        std::array<double, 4> next{};
        for (unsigned column = 0; column < 4; ++column)
            for (unsigned row = 0; row < 4; ++row) next[column] += value[row]*matrix[row*4+column];
        value = next;
    }
    const auto& viewport = frame.scene_viewport;
    return {viewport.x+(value[0]/value[3]*.5+.5)*viewport.width, viewport.y+(.5-value[1]/value[3]*.5)*viewport.height};
}
bool sky_inside(const render::Frame& frame, unsigned x, unsigned y) {
    const auto& v = frame.scene_viewport;
    return x+.5 >= v.x && x+.5 < v.x+v.width && y+.5 >= v.y && y+.5 < v.y+v.height;
}
void sky_flat_options(render::SceneLitSettings& options) {
    options.ddgi = options.gtao = options.sssr = options.ssil = options.bloom = options.shadows = false;
    options.antialiasing = render::Antialiasing::none;
}
void sky_anchor(Fixture& f) {
    // Scene Lit runs only for a non-empty scene; this cube stays out of every view below.
    f.room.meshes.assign(1, {});
    auto& anchor = f.room.meshes.front();
    anchor.geometry = atelier::geometry::cube_mesh(); anchor.world = box(0,-30,0,.1f,.1f,.1f); anchor.stable_key = 1;
    f.lights.clear(); f.edited(); ++f.frame.scene_identity; f.frame.camera_cut = true;
}

// A constant map whose peak needs 2^-2 FP16 storage equals the uniform sky of
// the same radiance; its background is that radiance. A polar cap equals the
// uniform sky of its solid-angle mean, (1 - sin 60)/2 rather than the 1/6 of texels.
void sky_uniform_equivalence(Fixture& f) {
    f.restore();
    auto& options = f.frame.view.lit; sky_flat_options(options);
    f.lights.clear();
    for (auto& mesh : f.room.meshes) mesh.material.emissive = {};
    f.edited(); ++f.frame.scene_identity; f.frame.camera_cut = true;
    const Vec3 radiance{.12f,.08f,.04f};
    f.frame.environment = radiance;
    const auto uniform = f.capture("sky-uniform", 2);
    constexpr float peak = 131072;
    options.sky_map = sky_map(64, [](const SkyDirection&) { return Vec3{peak,peak,peak}; });
    check(options.sky_map->scale_exponent() == 2, "a 2^17 constant sky map is stored at 2^-2");
    f.frame.environment = {radiance[0]/peak, radiance[1]/peak, radiance[2]/peak};
    const auto constant = f.capture("sky-constant-map", 2);
    std::size_t surfaces = 0, background = 0, background_failures = 0;
    for (unsigned y = 0; y < constant.extent.height; ++y) for (unsigned x = 0; x < constant.extent.width; ++x) {
        const auto i = std::size_t(y)*constant.extent.width+x;
        if (constant.depth[i] > 0) {
            ++surfaces;
            check(std::equal(constant.rgba.begin()+i*4, constant.rgba.begin()+i*4+3, uniform.rgba.begin()+i*4),
                  "a constant sky map lights every surface exactly like the uniform sky of its radiance");
        } else if (sky_inside(f.frame, x, y)) {
            ++background;
            check(uniform.rgba[i*4] == 0 && uniform.rgba[i*4+1] == 0 && uniform.rgba[i*4+2] == 0,
                  "the uniform sky leaves the Scene Lit background black");
            for (unsigned c = 0; c < 3; ++c)
                if (std::abs(double(constant.rgba[i*4+c])-display_code(radiance[c])) > 1) {++background_failures; break;}
        }
    }
    std::printf("SceneLit sky constant map: surfaces=%zu background=%zu background_failures=%zu\n", surfaces, background, background_failures);
    check(surfaces > 1000 && background > 1000 && background_failures == 0,
          "a constant sky map's background shows its radiance, including the 2^k storage scale");

    const double cap = (1-std::sqrt(3.)/2)/2;
    options.sky_map = sky_map(192, [](const SkyDirection& d) { return d[1] > std::sqrt(3.)/2 ? Vec3{1,1,1} : Vec3{}; });
    f.frame.environment = {float(radiance[0]/cap), float(radiance[1]/cap), float(radiance[2]/cap)};
    const auto polar = f.capture("sky-polar-cap-mean", 2);
    unsigned worst = 0;
    for (std::size_t i = 0; i < polar.depth.size(); ++i) if (polar.depth[i] > 0)
        for (unsigned c = 0; c < 3; ++c) worst = std::max(worst, unsigned(std::abs(int(polar.rgba[i*4+c])-int(uniform.rgba[i*4+c]))));
    std::printf("SceneLit sky polar cap mean: max_code_delta=%u\n", worst);
    check(worst <= 1, "uniform consumers read the sky map's solid-angle mean");
    options.sky_map = {};
}

// A marker's background position follows the map space and both yaw signs; a
// map varying smoothly in latitude stays continuous across the u seam behind -Z,
// where a level chosen from the unwrapped u difference would show the map's mean.
// A one-texel checker shows its mean where a pixel covers more than two texels,
// and removing the map blackens the background in the same frame and releases it.
void sky_background_geometry(Fixture& f) {
    f.restore();
    auto& options = f.frame.view.lit; sky_flat_options(options);
    sky_anchor(f);
    const float aspect = f.frame.scene_viewport.width/f.frame.scene_viewport.height;
    f.frame.camera = sky_look({0,0,0}, {0,0,1}, {0,1,0}, aspect, 1.8f);
    f.frame.environment = {1,1,1};
    const auto marker = sky_direction(-20, 10);
    std::vector<SkyDirection> marker_texels;
    options.sky_map = sky_map(2048, [&](const SkyDirection& d) {
        const bool on = d[0]*marker[0]+d[1]*marker[1]+d[2]*marker[2] > std::cos(.8*std::numbers::pi/180);
        if (on) marker_texels.push_back(d);
        return on ? Vec3{4,4,4} : Vec3{};
    });
    for (double yaw : {0., 35.}) {
        options.sky_rotation = float(yaw);
        const auto image = f.capture(yaw == 0 ? "sky-marker-yaw-0" : "sky-marker-yaw-35", 2);
        double ex = 0, ey = 0;
        for (const auto& d : marker_texels) {const auto p = sky_pixel(f.frame, sky_yawed(d, yaw)); ex += p[0]; ey += p[1];}
        ex /= double(marker_texels.size()); ey /= double(marker_texels.size());
        double weight = 0, ox = 0, oy = 0; unsigned brightest = 0;
        for (unsigned y = 0; y < image.extent.height; ++y) for (unsigned x = 0; x < image.extent.width; ++x) {
            const unsigned code = pixel(image, x, y)[1];
            if (!sky_inside(f.frame, x, y) || code <= 8) continue;
            weight += code; ox += code*(x+.5); oy += code*(y+.5); brightest = std::max(brightest, code);
        }
        ox = weight > 0 ? ox/weight : -1; oy = weight > 0 ? oy/weight : -1;
        std::printf("SceneLit sky marker yaw=%.0f: expected=(%.2f,%.2f) observed=(%.2f,%.2f) peak=%u\n", yaw, ex, ey, ox, oy, brightest);
        check(brightest > 200 && std::abs(ox-ex) <= 1 && std::abs(oy-ey) <= 1,
              "the background marker lies at its map direction turned by the sky yaw");
    }
    options.sky_rotation = 0;
    options.sky_map = sky_map(1024, [](const SkyDirection& d) {
        const float value = float(.15+.85*(.5+.5*std::sin(12*std::asin(std::clamp(d[1],-1.,1.)))));
        return Vec3{value,value,value};
    });
    f.frame.camera = sky_look({0,0,0}, {0,0,-1}, {0,1,0}, aspect, 1.8f);
    const auto seam = f.capture("sky-seam", 2);
    const auto centre = unsigned(f.frame.scene_viewport.x+f.frame.scene_viewport.width/2);
    unsigned worst = 0;
    for (auto y = unsigned(f.frame.scene_viewport.y+f.frame.scene_viewport.height*.2f); y < unsigned(f.frame.scene_viewport.y+f.frame.scene_viewport.height*.8f); ++y)
        for (unsigned x = centre-6; x < centre+6; ++x) for (unsigned c = 0; c < 3; ++c)
            worst = std::max(worst, unsigned(std::abs(int(pixel(seam,x,y)[c])-int(pixel(seam,x+1,y)[c]))));
    std::printf("SceneLit sky seam: max_horizontal_code_step=%u\n", worst);
    check(worst <= 2, "the background has no discontinuity at the map's u seam");

    constexpr std::uint32_t checker_width = 4096;
    {
        constexpr std::uint32_t width = checker_width, height = width/2;
        std::vector<float> rgb(std::size_t(width)*height*3);
        for (std::uint32_t y = 0; y < height; ++y) for (std::uint32_t x = 0; x < width; ++x)
            for (unsigned c = 0; c < 3; ++c) rgb[(std::size_t(y)*width+x)*3+c] = (x+y)%2 ? 2.f : 0.f;
        options.sky_map = std::make_shared<const atelier::rendering::SkyMap>(width, height, rgb);
    }
    constexpr float checker_scale = .6f;
    f.frame.camera = sky_look({0,0,0}, {0,0,1}, {0,1,0}, aspect, checker_scale);
    f.frame.environment = {.1f,.1f,.1f};
    const auto checker = f.capture("sky-minified-checker", 2);
    const auto& v = f.frame.scene_viewport;
    const auto pixel_direction = [&](double x, double y) {
        const SkyDirection d{((x-v.x)/v.width*2-1)*aspect/checker_scale, (1-(y-v.y)/v.height*2)/checker_scale, 1};
        const double length = std::sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);
        return SkyDirection{d[0]/length, d[1]/length, d[2]/length};
    };
    const auto angle = [](const SkyDirection& a, const SkyDirection& b) {
        const SkyDirection c{a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]};
        return std::atan2(std::sqrt(c[0]*c[0]+c[1]*c[1]+c[2]*c[2]), a[0]*b[0]+a[1]*b[1]+a[2]*b[2]);
    };
    // Map texels per radian are at least width/(2 pi) along both axes; only pixels
    // covering 2.5 texels or more are judged, so every level they read is exactly 1.
    double checker_worst = 0; std::size_t judged = 0;
    for (unsigned y = 0; y < checker.extent.height; ++y) for (unsigned x = 0; x < checker.extent.width; ++x) {
        if (!sky_inside(f.frame, x, y) || depth(checker, x, y) > 0) continue;
        const auto centre = pixel_direction(x+.5, y+.5);
        const double footprint = std::min(angle(centre, pixel_direction(x+1.5, y+.5)), angle(centre, pixel_direction(x+.5, y+1.5)))*checker_width/(2*std::numbers::pi);
        if (footprint < 2.5) continue;
        ++judged;
        for (unsigned c = 0; c < 3; ++c) checker_worst = std::max(checker_worst, std::abs(double(pixel(checker,x,y)[c])-display_code(.1)));
    }
    std::printf("SceneLit sky minified checker: judged=%zu expected_code=%.2f max_code_delta=%.2f\n", judged, display_code(.1), checker_worst);
    check(judged > 10000 && checker_worst <= 2, "a minified sky background shows the map's filtered mean");

    const std::weak_ptr<const atelier::rendering::SkyMap> released = options.sky_map;
    options.sky_map = {};
    const auto removed = f.capture("sky-removed", 1);
    std::size_t lit_background = 0;
    for (unsigned y = 0; y < removed.extent.height; ++y) for (unsigned x = 0; x < removed.extent.width; ++x) {
        const auto i = std::size_t(y)*removed.extent.width+x;
        if (removed.depth[i] <= 0 && sky_inside(f.frame, x, y) && (removed.rgba[i*4] || removed.rgba[i*4+1] || removed.rgba[i*4+2])) ++lit_background;
    }
    std::printf("SceneLit sky removed: lit_background=%zu released=%d\n", lit_background, int(released.expired()));
    check(lit_background == 0 && released.expired(), "removing the sky map blackens the background at once and releases the map");
}

// The sky behind the scene moves with the camera's rotation only. The camera turns
// 1.5 degrees and circles 0.5 m around the origin by 0.22 m every frame, so a
// reprojection that kept the translation would misplace the history by tens of
// degrees. Under SMAA T2x and TAA the marker stays where its direction projects,
// without a trailing copy and with the energy of a still camera's frame. The
// background samples the unjittered grid, so under TAA a still camera sees a steady sky.
void sky_background_temporal(Fixture& f) {
    f.restore();
    auto& options = f.frame.view.lit; sky_flat_options(options);
    sky_anchor(f);
    const float aspect = f.frame.scene_viewport.width/f.frame.scene_viewport.height;
    f.frame.environment = {1,1,1};
    const auto marker = sky_direction(0, 10);
    std::vector<SkyDirection> marker_texels;
    options.sky_map = sky_map(2048, [&](const SkyDirection& d) {
        const bool on = d[0]*marker[0]+d[1]*marker[1]+d[2]*marker[2] > std::cos(.8*std::numbers::pi/180);
        if (on) marker_texels.push_back(d);
        return on ? Vec3{4,4,4} : Vec3{};
    });
    const auto turn = [&](unsigned frame) {
        const double yaw = (1.5*frame-18)*std::numbers::pi/180, orbit = 25.*frame*std::numbers::pi/180;
        f.frame.camera = sky_look({float(.5*std::cos(orbit)), 0, float(.5*std::sin(orbit))}, {float(std::sin(yaw)), 0, float(std::cos(yaw))}, {0,1,0}, aspect, 1.8f);
    };
    struct Marker { double dx, dy, energy, trail; };
    const auto measure = [&](const render::Capture& image) {
        double ex = 0, ey = 0;
        for (const auto& d : marker_texels) {const auto q = sky_pixel(f.frame, d); ex += q[0]; ey += q[1];}
        ex /= double(marker_texels.size()); ey /= double(marker_texels.size());
        double radius = 0;
        for (const auto& d : marker_texels) {const auto q = sky_pixel(f.frame, d); radius = std::max(radius, std::hypot(q[0]-ex, q[1]-ey));}
        radius += 3;
        double weight = 0, outside = 0, ox = 0, oy = 0;
        for (unsigned y = 0; y < image.extent.height; ++y) for (unsigned x = 0; x < image.extent.width; ++x) {
            const unsigned code = pixel(image, x, y)[1];
            if (!sky_inside(f.frame, x, y) || code <= 8) continue;
            weight += code; ox += code*(x+.5); oy += code*(y+.5);
            if (std::hypot(x+.5-ex, y+.5-ey) > radius) outside += code;
        }
        return weight > 0 ? Marker{ox/weight-ex, oy/weight-ey, weight, outside/weight} : Marker{1e9, 1e9, 0, 1};
    };
    for (const auto aa : {render::Antialiasing::smaa_t2x, render::Antialiasing::taa}) {
        const bool t2x = aa == render::Antialiasing::smaa_t2x;
        options.antialiasing = aa; ++f.frame.scene_identity; f.frame.camera_cut = true;
        turn(24);
        const auto reference = measure(f.capture(t2x ? "sky-still-t2x" : "sky-still-taa", 8));
        ++f.frame.scene_identity; f.frame.camera_cut = true;
        for (unsigned frame = 0; frame < 24; ++frame) {turn(frame); f.step();}
        turn(24);
        const auto image = f.capture(t2x ? "sky-turning-t2x" : "sky-turning-taa", 1);
        const auto observed = measure(image);
        const double energy = observed.energy/reference.energy;
        if (t2x) {
            // Captured motion against the rotation-only reprojection between the
            // unjittered frame-23 and frame-24 cameras, computed here.
            auto previous = f.frame; turn(23); previous.camera = f.frame.camera; turn(24);
            const double yaw = (1.5*24-18)*std::numbers::pi/180;
            const SkyDirection x_axis{std::cos(yaw), 0, -std::sin(yaw)}, z_axis{std::sin(yaw), 0, std::cos(yaw)};
            const auto& v = f.frame.scene_viewport;
            double motion_error = 0; std::size_t nonfinite = 0;
            for (unsigned y = 0; y < image.extent.height; ++y) for (unsigned x = 0; x < image.extent.width; ++x) {
                if (!sky_inside(f.frame, x, y)) continue;
                const double a = ((x+.5-v.x)/v.width*2-1)*aspect/1.8, b = (1-(y+.5-v.y)/v.height*2)/1.8;
                const SkyDirection d{a*x_axis[0]+z_axis[0], b, a*x_axis[2]+z_axis[2]};
                const auto q = sky_pixel(previous, d);
                const auto i = (std::size_t(y)*image.extent.width+x)*2;
                if (!std::isfinite(image.motion[i]) || !std::isfinite(image.motion[i+1])) {++nonfinite; continue;}
                motion_error = std::max({motion_error, std::abs(image.motion[i]*image.extent.width-(q[0]-x-.5)),
                                         std::abs(image.motion[i+1]*image.extent.height-(q[1]-y-.5))});
            }
            std::printf("SceneLit sky background motion: max_error=%.3f px nonfinite=%zu\n", motion_error, nonfinite);
            check(nonfinite == 0 && motion_error <= .1, "background motion is the rotation-only reprojection on the unjittered grid");
        }
        std::printf("SceneLit sky turning %s: offset=(%.2f,%.2f) energy=%.3f trail=%.4f\n", t2x ? "t2x" : "taa", observed.dx, observed.dy, energy, observed.trail);
        check(std::abs(observed.dx) <= 1 && std::abs(observed.dy) <= 1 && std::abs(energy-1) <= .05 && observed.trail <= .02,
              "a turning, moving camera keeps the sky background registered without a trail");
    }
    // Flipping 180 degrees every frame without a cut, the camera sees only sky that
    // was behind the previous one. Projecting it there would mirror the marker into
    // the opposite view at a matching speed, which T2x would blend in at half weight.
    options.antialiasing = render::Antialiasing::smaa_t2x; ++f.frame.scene_identity; f.frame.camera_cut = true;
    const auto flip = [&](bool back) {f.frame.camera = sky_look({0,0,0}, {0,0,back ? -1.f : 1.f}, {0,1,0}, aspect, 1.8f);};
    flip(false);
    for (unsigned frame = 0; frame < 8; ++frame) f.step();
    for (unsigned frame = 0; frame < 8; ++frame) {flip(frame%2 == 0); f.step();}
    flip(true);
    const auto flipped = f.capture("sky-flipped-t2x", 1);
    double ghost = 0;
    for (unsigned y = 0; y < flipped.extent.height; ++y) for (unsigned x = 0; x < flipped.extent.width; ++x)
        if (sky_inside(f.frame, x, y)) ghost += pixel(flipped, x, y)[1];
    std::printf("SceneLit sky flipped t2x: background_energy=%.0f\n", ghost);
    check(ghost == 0, "sky behind the previous camera takes no T2x history");
    // Parallel rays see one sky direction in every pixel: the background keeps its place.
    options.antialiasing = render::Antialiasing::smaa_t2x; ++f.frame.scene_identity; f.frame.camera_cut = true;
    f.frame.camera = sky_look({0,0,0}, {0,0,1}, {0,1,0}, aspect, 1.8f);
    f.frame.camera.projection = {1/(3.8f*aspect),0,0,0, 0,1/3.8f,0,0, 0,0,-1/39.9f,0, 0,0,40/39.9f,1};
    const auto parallel = f.capture("sky-orthographic-t2x", 4);
    std::size_t background = 0, moving = 0;
    for (unsigned y = 0; y < parallel.extent.height; ++y) for (unsigned x = 0; x < parallel.extent.width; ++x) {
        if (!sky_inside(f.frame, x, y) || depth(parallel, x, y) > 0) continue;
        const auto i = (std::size_t(y)*parallel.extent.width+x)*2;
        ++background;
        if (!(parallel.motion[i] == 0 && parallel.motion[i+1] == 0)) ++moving;
    }
    std::printf("SceneLit sky orthographic t2x: background=%zu moving_or_nonfinite=%zu\n", background, moving);
    check(background > 1000 && moving == 0, "an orthographic camera's sky background keeps its place, with finite motion");
    options.antialiasing = render::Antialiasing::taa; ++f.frame.scene_identity; f.frame.camera_cut = true;
    f.frame.camera = sky_look({0,0,0}, {0,0,1}, {0,1,0}, aspect, 1.8f);
    for (unsigned frame = 0; frame < 32; ++frame) f.step();
    std::vector<render::Capture> still(16);
    for (auto& frame : still) f.step(&frame);
    double worst = 0; std::vector<double> varying;
    for (unsigned y = 0; y < still[0].extent.height; ++y) for (unsigned x = 0; x < still[0].extent.width; ++x) {
        if (!sky_inside(f.frame, x, y) || depth(still[0], x, y) > 0) continue;
        double pixel_worst = 0;
        for (unsigned c = 0; c < 3; ++c) {
            double sum = 0, squares = 0;
            for (const auto& frame : still) {const double v = pixel(frame, x, y)[c]; sum += v; squares += v*v;}
            const double mean = sum/still.size();
            pixel_worst = std::max(pixel_worst, std::sqrt(std::max(0., squares/still.size()-mean*mean)));
        }
        worst = std::max(worst, pixel_worst);
        if (pixel_worst > .5) varying.push_back(pixel_worst);
    }
    double varying_sum = 0; for (double v : varying) varying_sum += v;
    std::printf("SceneLit sky still TAA: max_temporal_deviation=%.2f p90=%.2f mean=%.2f over %zu varying pixels\n", worst,
                varying.empty() ? 0. : percentile(varying, 90), varying.empty() ? 0. : varying_sum/varying.size(), varying.size());
    check(worst <= 1, "a still camera under TAA sees a steady sky background");
    options.sky_map = {}; options.antialiasing = render::Antialiasing::none;
}

// DDGI probe misses read the map: tilted Lambert boards under a half-space sky
// receive (1 + n.a)/2 of their uniform-sky irradiance. Each channel is its own
// half-space, along +X, +Y and +Z, with its own tint, so a swapped map or tint
// channel changes the per-channel ratios. Turning the same map by 90 degrees (+X
// to -Z) is an authored change: frames 33-48 already show the turned ratios,
// which the probes' hysteresis alone reaches only after about 60. Ratios to a uniform-sky run
// cancel DDGI's own interpolation; the environment specular, which reads the map
// mean, biases them by at most about 0.025.
void sky_probe_misses(Fixture& f) {
    f.restore();
    auto& options = f.frame.view.lit;
    options.gtao = options.sssr = options.ssil = options.bloom = options.shadows = false; options.ddgi = true;
    options.antialiasing = render::Antialiasing::none;
    for (auto& volume : options.probe_volumes) volume.enabled = false;
    auto& volume = options.probe_volumes[0];
    volume = {}; volume.enabled = true; volume.minimum = {-6,-3,2}; volume.maximum = {6,5,14}; volume.counts = {7,5,7};
    options.probe_update_budget = 245; options.probe_rays = 128;
    struct Board { Vec3 centre, normal, tangent, bitangent; };
    const float s = std::sqrt(3.f)/2;
    const std::array<Board, 5> boards{{
        {{0,0,8},{0,1,0},{1,0,0},{0,0,1}},
        {{3,0,8},{s,.5f,0},{.5f,-s,0},{0,0,1}}, {{-3,0,8},{-s,.5f,0},{.5f,s,0},{0,0,1}},
        {{0,0,11},{0,.5f,s},{0,-s,.5f},{1,0,0}}, {{0,0,5},{0,.5f,-s},{0,s,.5f},{1,0,0}}}};
    f.lights.clear(); f.room.meshes.clear();
    for (std::size_t i = 0; i < boards.size(); ++i) {
        const auto& b = boards[i];
        const auto corner = [&](float u, float v) {
            return atelier::geometry::Vertex{{b.centre[0]+.6f*(u*b.tangent[0]+v*b.bitangent[0]), b.centre[1]+.6f*(u*b.tangent[1]+v*b.bitangent[1]),
                                              b.centre[2]+.6f*(u*b.tangent[2]+v*b.bitangent[2])}, {b.normal[0],b.normal[1],b.normal[2]}};
        };
        auto& mesh = f.room.meshes.emplace_back();
        mesh.geometry = std::make_shared<const atelier::geometry::Mesh>(std::vector<atelier::geometry::Vertex>{
            corner(-1,-1),corner(1,-1),corner(1,1),corner(-1,-1),corner(1,1),corner(-1,1)});
        mesh.world = box(0,0,0,1,1,1); mesh.color = {1,1,1,1}; mesh.material = {.roughness=1, .metallic=0}; mesh.stable_key = i+1;
    }
    f.edited(); ++f.frame.scene_identity; f.frame.camera_cut = true;
    const float aspect = f.frame.scene_viewport.width/f.frame.scene_viewport.height;
    f.frame.camera = sky_look({0,12,8}, {0,-1,0}, {0,0,1}, aspect, 2.5f);
    const auto board_radiance = [&](const render::Capture& image) {
        std::array<std::array<double, 3>, boards.size()> result{};
        for (std::size_t i = 0; i < boards.size(); ++i) {
            const auto region = project_region(f.frame.camera, f.frame.scene_viewport, image.extent, boards[i].centre, 4);
            unsigned count = 0;
            for (unsigned y = region.top; y < region.bottom; ++y) for (unsigned x = region.left; x < region.right; ++x) {
                check(depth(image,x,y) > 0, "each probe-miss board region lies on its board");
                for (unsigned c = 0; c < 3; ++c) result[i][c] += display_radiance(pixel(image,x,y)[c], 1);
                ++count;
            }
            for (auto& channel : result[i]) channel /= count;
        }
        return result;
    };
    constexpr unsigned settle = 3*warmup_frames, frames = 32;
    f.frame.environment = {1,1,1}; options.sky_map = {};
    const auto uniform = board_radiance(f.average("sky-boards-uniform", settle, frames));
    constexpr float peak = 131072;
    const Vec3 tint{1,.75f,.5f};
    options.sky_map = sky_map(256, [&](const SkyDirection& d) {
        Vec3 value{};
        for (unsigned c = 0; c < 3; ++c) value[c] = d[c] > 0 ? peak : 0;
        return value;
    });
    f.frame.environment = {tint[0]/peak, tint[1]/peak, tint[2]/peak};
    const auto judge = [&](const char* name, const render::Capture& image, double yaw, const char* claim) {
        const auto observed = board_radiance(image);
        for (std::size_t i = 0; i < boards.size(); ++i) for (unsigned c = 0; c < 3; ++c) {
            SkyDirection axis{}; axis[c] = 1; axis = sky_yawed(axis, yaw);
            const auto& n = boards[i].normal;
            const double expected = tint[c]*(1+n[0]*axis[0]+n[1]*axis[1]+n[2]*axis[2])/2, ratio = observed[i][c]/uniform[i][c];
            std::printf("SceneLit %s board %zu channel %u: ratio=%.3f expected=%.3f\n", name, i, c, ratio, expected);
            f.metrics << name << "-board-" << i << '-' << c << ',' << ratio << ',' << expected << ",0,0\n";
            check(std::abs(ratio-expected) <= .08*tint[c], claim);
        }
    };
    judge("sky-boards-rgb", f.average("sky-boards-rgb", settle, frames), 0, "DDGI probe misses read each sky map channel at its cube resolution");
    options.sky_rotation = 90;
    judge("sky-boards-rgb-turned-response", f.average("sky-boards-rgb-turned-response", 48, 16), 90,
          "turning the same sky map makes the probes respond within 48 frames");
    judge("sky-boards-rgb-yaw-90", f.average("sky-boards-rgb-yaw-90", settle, frames), 90, "DDGI probe misses read the turned sky map");
    options.sky_map = {}; options.sky_rotation = 0;
}

// The real HDRI with a 74-degree sun keeps the room's DDGI no noisier than
// twice a uniform sky of the same mean radiance (or one code of temporal deviation).
void sky_real_stability(Fixture& f, const atelier::rendering::SkyMapRef& map) {
    f.restore();
    auto& options = f.frame.view.lit; options.bloom = false; options.antialiasing = render::Antialiasing::smaa;
    f.lights.clear();
    for (auto& mesh : f.room.meshes) mesh.material.emissive = {};
    f.edited();
    const auto& mean = map->mean();
    const float scale = float(.5/(.2126*mean[0]+.7152*mean[1]+.0722*mean[2]));
    const auto noise = [&](const char* name, bool mapped) {
        options.sky_map = mapped ? map : atelier::rendering::SkyMapRef{};
        f.frame.environment = mapped ? Vec3{scale,scale,scale} : Vec3{scale*mean[0],scale*mean[1],scale*mean[2]};
        ++f.frame.scene_identity; f.frame.camera_cut = true;
        for (unsigned i = 0; i < 2*warmup_frames; ++i) f.step();
        std::vector<render::Capture> frames(32);
        for (auto& frame : frames) f.step(&frame);
        png(frames.back(), f.output/(std::string(name)+".png"));
        std::vector<double> deviations;
        for (std::size_t i = 0; i < frames[0].depth.size(); ++i) {
            if (frames[0].depth[i] <= 0) continue;
            double worst = 0;
            for (unsigned c = 0; c < 3; ++c) {
                double sum = 0, squares = 0;
                for (const auto& frame : frames) {const double v = frame.rgba[i*4+c]; sum += v; squares += v*v;}
                const double m = sum/frames.size();
                worst = std::max(worst, std::sqrt(std::max(0., squares/frames.size()-m*m)));
            }
            deviations.push_back(worst);
        }
        return percentile(deviations, 95);
    };
    const double uniform = noise("sky-real-uniform-mean", false), mapped = noise("sky-real-kloppenheim", true);
    std::printf("SceneLit sky real HDRI temporal deviation p95: uniform=%.3f map=%.3f codes\n", uniform, mapped);
    f.metrics << "sky-real-temporal-deviation-p95," << mapped << ',' << uniform << ",0,0\n";
    check(mapped <= std::max(2*uniform, 1.), "a real HDRI sun keeps DDGI temporally stable");
    options.sky_map = {};
}

atelier::rendering::SkyMapRef sky_real_map() {
    const auto path = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path()/
                      "assets/examples/Examples/Sponza/Sky/kloppenheim_05_4k.hdr";
    std::ifstream input(path, std::ios::binary);
    check(input.good(), "the bundled kloppenheim_05 HDRI is readable");
    const std::string bytes{std::istreambuf_iterator<char>(input), {}};
    return atelier::rendering::decode_radiance_hdr(bytes);
}

// Cost of the real HDRI on the benchmark room: composition and the DDGI primary
// trace with and without it in alternating blocks, and the frame that uploads it.
void sky_benchmark(Fixture& f) {
    const auto decode_start = std::chrono::steady_clock::now();
    const auto map = sky_real_map();
    const double decode_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-decode_start).count();
    auto& options = f.frame.view.lit;
    const auto uniform = f.frame.environment;
    const auto& mean = map->mean();
    const auto set = [&](bool mapped) {
        options.sky_map = mapped ? map : atelier::rendering::SkyMapRef{};
        f.frame.environment = mapped ? Vec3{uniform[0]/mean[0],uniform[1]/mean[1],uniform[2]/mean[2]} : uniform;
    };
    const auto timed_step = [&] {
        const auto start = std::chrono::steady_clock::now();
        const auto stats = f.step().stats;
        return std::pair{std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-start).count(), stats};
    };
    std::vector<double> steady;
    for (unsigned i = 0; i < 31; ++i) steady.push_back(timed_step().first);
    set(true);
    const auto upload_frame = f.drawing.id+1;
    const auto [upload_ms, upload_stats] = timed_step();
    double upload_gpu_ms = -1;
    std::array<std::array<std::vector<double>, 3>, 2> series;
    std::uint64_t previous = 0;
    for (unsigned block = 0; block < 4; ++block) {
        const bool mapped = block%2 == 0;
        set(mapped);
        const auto first = f.drawing.id+8;
        for (unsigned i = 0, taken = 0; i < 400 && taken < 60; ++i) {
            const auto stats = f.step().stats;
            if (!stats.scene_lit_gpu_timing_valid || stats.scene_lit_gpu_sample_sequence <= previous) continue;
            previous = stats.scene_lit_gpu_sample_sequence;
            if (stats.scene_lit_gpu_sample_frame_id == upload_frame) upload_gpu_ms = stats.scene_lit_gpu_ms[1];
            if (stats.scene_lit_gpu_sample_frame_id <= first) continue;
            double total = 0; for (float value : stats.scene_lit_gpu_ms) total += value;
            series[mapped][0].push_back(stats.scene_lit_direct_shading_gpu_ms);
            series[mapped][1].push_back(stats.scene_lit_ddgi_update_gpu_ms[0]);
            series[mapped][2].push_back(total); ++taken;
        }
    }
    set(false);
    check(series[0][0].size() >= 100 && series[1][0].size() >= 100, "sky benchmark obtains completed GPU samples with and without the map");
    f.metrics << "sky_map=kloppenheim_05_4k " << map->width() << 'x' << map->height() << " k=" << map->scale_exponent()
              << " decode_and_derive_ms=" << decode_ms << " upload_frame_cpu_ms=" << upload_ms
              << " steady_frame_cpu_ms_p50=" << percentile(steady, 50) << " upload_frame_as_uploads_gpu_ms=" << upload_gpu_ms
              << " resource_bytes_with_map=" << upload_stats.scene_lit_resource_bytes << "\n";
    const char* names[]{"direct_shading_ms", "ddgi_primary_ms", "sum_gpu_ms"};
    for (unsigned i = 0; i < 3; ++i)
        f.metrics << "sky " << names[i] << " p50 without=" << percentile(series[0][i], 50) << " with=" << percentile(series[1][i], 50) << "\n";
    std::printf("SceneLit sky benchmark: decode=%.1f ms upload frame cpu=%.2f ms (steady p50 %.2f) as/uploads gpu=%.3f ms; compose p50 %.3f/%.3f ms; ddgi primary p50 %.3f/%.3f ms\n",
                decode_ms, upload_ms, percentile(steady, 50), upload_gpu_ms, percentile(series[0][0], 50), percentile(series[1][0], 50),
                percentile(series[0][1], 50), percentile(series[1][1], 50));
}

void sky_cases(Fixture& f) {
    sky_uniform_equivalence(f);
    sky_background_geometry(f);
    sky_background_temporal(f);
    sky_probe_misses(f);
    sky_real_stability(f, sky_real_map());
    f.restore();
    std::puts("Renderer SceneLit sky: PASS (constant map and 2^k background, solid-angle mean, marker under two yaws, u seam, minified mean, same-frame removal, turning-camera registration under T2x and TAA, no history from behind the previous camera, orthographic background motion, steady sky under a still TAA camera, per-channel DDGI half-space ratios, same-map turn response, real HDRI stability)");
}
