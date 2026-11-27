// Directional sky expectations come from authored map directions and yaw.
using SkyDirection = std::array<double, 3>;

template <class Radiance>
atelier::rendering::SkyMapRef sky_map(std::uint32_t width, Radiance&& radiance) {
    const std::uint32_t height = width / 2;
    atelier::Array<float> rgb(std::size_t(width) * height * 3);
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x) {
            const double longitude = 2 * std::numbers::pi * ((x + .5) / width - .5);
            const double latitude = std::numbers::pi * (.5 - (y + .5) / height);
            const Vec3 value = radiance(SkyDirection{std::sin(longitude) * std::cos(latitude), std::sin(latitude),
                                                     std::cos(longitude) * std::cos(latitude)});
            for (unsigned c = 0; c < 3; ++c)
                rgb[(std::size_t(y) * width + x) * 3 + c] = value[c];
        }
    auto result = atelier::rendering::SkyMap::create(width, height, rgb);
    check(result.has_value(), "construct fixed sky map input");
    return std::move(*result);
}

SkyDirection sky_yawed(const SkyDirection& d, double degrees) {
    const double a = degrees * std::numbers::pi / 180;
    return {d[0] * std::cos(a) + d[2] * std::sin(a), d[1], -d[0] * std::sin(a) + d[2] * std::cos(a)};
}

SkyDirection sky_direction(double longitude_degrees, double latitude_degrees) {
    const double lon = longitude_degrees * std::numbers::pi / 180;
    const double lat = latitude_degrees * std::numbers::pi / 180;
    return {std::sin(lon) * std::cos(lat), std::sin(lat), std::cos(lon) * std::cos(lat)};
}

render::Camera sky_look(Vec3 eye, Vec3 forward, Vec3 up, float aspect, float scale) {
    render::Camera result;
    result.view = *atelier::math::try_look_to_lh(eye, forward, up);
    result.projection = {{scale / aspect, 0, 0, 0, 0, scale, 0, 0, 0, 0, -.1f / 39.9f, 1, 0, 0, 4 / 39.9f, 0}};
    return result;
}

std::array<double, 2> sky_pixel(const render::Frame& frame, const SkyDirection& direction) {
    const auto clip = (atelier::math::Vector4d{direction[0], direction[1], direction[2], 0} *
                       atelier::math::Matrix4d(frame.camera.view)) *
                      atelier::math::Matrix4d(frame.camera.projection);
    const auto& viewport = frame.scene_viewport;
    return {viewport.x + (clip.x / clip.w * .5 + .5) * viewport.width,
            viewport.y + (.5 - clip.y / clip.w * .5) * viewport.height};
}

bool sky_inside(const render::Frame& frame, unsigned x, unsigned y) {
    const auto& v = frame.scene_viewport;
    return x + .5 >= v.x && x + .5 < v.x + v.width && y + .5 >= v.y && y + .5 < v.y + v.height;
}

void sky_background_geometry(Fixture& f) {
    f.restore();
    auto& options = f.frame.view.lit;
    options.ddgi = options.gtao = options.sssr = options.bloom = options.shadows = false;
    options.antialiasing = render::Antialiasing::none;
    // Scene Lit needs a nonempty scene; this anchor is outside the authored view.
    f.room.meshes.assign(1, {});
    auto& anchor = f.room.meshes.front();
    anchor.geometry = atelier::geometry::cube_mesh();
    anchor.world = box(0, -30, 0, .1f, .1f, .1f);
    anchor.stable_key = 1;
    f.lights.clear();
    f.edited();
    const float aspect = f.frame.scene_viewport.width / f.frame.scene_viewport.height;
    f.frame.camera = sky_look({0, 0, 0}, {0, 0, 1}, {0, 1, 0}, aspect, 1.8f);
    f.frame.environment = {1, 1, 1};
    const auto marker = sky_direction(-20, 10);
    atelier::Array<SkyDirection> marker_texels;
    options.sky_map = sky_map(2048, [&](const SkyDirection& d) {
        const bool on = d[0] * marker[0] + d[1] * marker[1] + d[2] * marker[2] > std::cos(.8 * std::numbers::pi / 180);
        if (on)
            marker_texels.add(d);
        return on ? Vec3{4, 4, 4} : Vec3{};
    });
    options.sky_rotation = 35;
    const auto image = f.capture("sky-marker-yaw-35", 2);
    double ex = 0;
    double ey = 0;
    for (const auto& d : marker_texels) {
        const auto p = sky_pixel(f.frame, sky_yawed(d, 35));
        ex += p[0];
        ey += p[1];
    }
    ex /= double(marker_texels.size());
    ey /= double(marker_texels.size());
    double weight = 0;
    double ox = 0;
    double oy = 0;
    unsigned brightest = 0;
    for (unsigned y = 0; y < image.extent.height; ++y)
        for (unsigned x = 0; x < image.extent.width; ++x) {
            const unsigned code = pixel(image, x, y)[1];
            if (!sky_inside(f.frame, x, y) || code <= 8)
                continue;
            weight += code;
            ox += code * (x + .5);
            oy += code * (y + .5);
            brightest = std::max(brightest, code);
        }
    ox = weight > 0 ? ox / weight : -1;
    oy = weight > 0 ? oy / weight : -1;
    check(brightest > 200 && std::abs(ox - ex) <= 1 && std::abs(oy - ey) <= 1,
          "the background marker lies at its map direction turned by the sky yaw");
    std::puts("Renderer sky: PASS (authored yaw)");
}

atelier::rendering::SkyMapRef sky_real_map() {
    const auto path = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
                      "examples/Content/Sponza/Sky/kloppenheim_05_4k.hdr";
    std::ifstream input(path, std::ios::binary);
    check(input.good(), "the bundled kloppenheim_05 HDRI is readable");
    const std::string bytes{std::istreambuf_iterator<char>(input), {}};
    auto result = atelier::rendering::decode_radiance_hdr(bytes);
    check(result.has_value(), "decode bundled HDRI");
    return std::move(*result);
}

// Cost of the real HDRI on the benchmark room: composition and the DDGI primary
// trace with and without it in alternating blocks, and the frame that uploads it.
void sky_benchmark(Fixture& f) {
    const auto decode_start = std::chrono::steady_clock::now();
    const auto map = sky_real_map();
    const double decode_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - decode_start).count();
    auto& options = f.frame.view.lit;
    const auto uniform = f.frame.environment;
    const auto& mean = map->mean();
    const auto set = [&](bool mapped) {
        options.sky_map = mapped ? map : atelier::rendering::SkyMapRef{};
        f.frame.environment = mapped ? atelier::math::cwise_div(uniform, mean) : uniform;
    };
    const auto timed_step = [&] {
        const auto start = std::chrono::steady_clock::now();
        const auto stats = f.step().stats;
        return std::pair{std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(),
                         stats};
    };
    atelier::Array<double> steady;
    for (unsigned i = 0; i < 31; ++i)
        steady.add(timed_step().first);
    set(true);
    const auto upload_frame = f.drawing.id + 1;
    const auto [upload_ms, upload_stats] = timed_step();
    double upload_gpu_ms = -1;
    std::array<std::array<atelier::Array<double>, 3>, 2> series;
    std::uint64_t previous = 0;
    for (unsigned block = 0; block < 4; ++block) {
        const bool mapped = block % 2 == 0;
        set(mapped);
        const auto first = f.drawing.id + 8;
        for (unsigned i = 0, taken = 0; i < 400 && taken < 60; ++i) {
            const auto stats = f.step().stats;
            if (!stats.scene_lit_gpu_timing_valid || stats.scene_lit_gpu_sample_sequence <= previous)
                continue;
            previous = stats.scene_lit_gpu_sample_sequence;
            if (stats.scene_lit_gpu_sample_frame_id == upload_frame)
                upload_gpu_ms = stats.scene_lit_gpu_ms[1];
            if (stats.scene_lit_gpu_sample_frame_id <= first)
                continue;
            double total = 0;
            for (float value : stats.scene_lit_gpu_ms)
                total += value;
            series[mapped][0].add(stats.scene_lit_direct_shading_gpu_ms);
            series[mapped][1].add(stats.scene_lit_ddgi_update_gpu_ms[0]);
            series[mapped][2].add(total);
            ++taken;
        }
    }
    set(false);
    check(series[0][0].size() >= 100 && series[1][0].size() >= 100,
          "sky benchmark obtains completed GPU samples with and without the map");
    f.metrics << "sky_map=kloppenheim_05_4k " << map->width() << 'x' << map->height() << " k=" << map->scale_exponent()
              << " decode_and_derive_ms=" << decode_ms << " upload_frame_cpu_ms=" << upload_ms
              << " steady_frame_cpu_ms_p50=" << percentile(steady, 50)
              << " upload_frame_as_uploads_gpu_ms=" << upload_gpu_ms
              << " resource_bytes_with_map=" << upload_stats.scene_lit_resource_bytes << "\n";
    const char* names[]{"direct_shading_ms", "ddgi_primary_ms", "sum_gpu_ms"};
    for (unsigned i = 0; i < 3; ++i)
        f.metrics << "sky " << names[i] << " p50 without=" << percentile(series[0][i], 50)
                  << " with=" << percentile(series[1][i], 50) << "\n";
    std::printf(
        "SceneLit sky benchmark: decode=%.1f ms upload frame cpu=%.2f ms (steady p50 %.2f) as/uploads gpu=%.3f ms; compose p50 %.3f/%.3f ms; ddgi primary p50 %.3f/%.3f ms\n",
        decode_ms, upload_ms, percentile(steady, 50), upload_gpu_ms, percentile(series[0][0], 50),
        percentile(series[1][0], 50), percentile(series[0][1], 50), percentile(series[1][1], 50));
}
