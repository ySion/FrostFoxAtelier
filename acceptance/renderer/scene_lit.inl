// Ordinary product snapshots; benchmark samples only completed GPU timestamps.
namespace scene_lit_e2e {
using Vec3 = std::array<float, 3>;

constexpr unsigned warmup_frames = 96;

void png(const render::Capture& image, const std::filesystem::path& path) {
    // Encoding is outside all measured frames and preserves captured RGBA8.
    std::ofstream output(path, std::ios::binary);
    const auto integer = [](std::vector<std::uint8_t>& bytes, std::uint32_t value) {
        for (int shift : {24, 16, 8, 0})
            bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    };
    const auto chunk = [&](const char* name, const std::vector<std::uint8_t>& payload) {
        std::vector<std::uint8_t> bytes;
        integer(bytes, static_cast<std::uint32_t>(payload.size()));
        bytes.insert(bytes.end(), name, name + 4);
        bytes.insert(bytes.end(), payload.begin(), payload.end());
        std::uint32_t crc = ~0u;
        for (std::size_t index = 4; index < bytes.size(); ++index) {
            crc ^= bytes[index];
            for (int bit = 0; bit < 8; ++bit)
                crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
        }
        integer(bytes, ~crc);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    };
    constexpr std::uint8_t signature[]{137, 80, 78, 71, 13, 10, 26, 10};
    output.write(reinterpret_cast<const char*>(signature), sizeof(signature));
    std::vector<std::uint8_t> header;
    integer(header, image.extent.width);
    integer(header, image.extent.height);
    header.insert(header.end(), {8, 6, 0, 0, 0});
    chunk("IHDR", header);
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
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (auto value : raw) {
        a = (a + value) % 65521;
        b = (b + a) % 65521;
    }
    integer(compressed, (b << 16) | a);
    chunk("IDAT", compressed);
    chunk("IEND", {});
    check(output.good(), "SceneLit PNG evidence written");
}

Vec3 subtract(Vec3 a, Vec3 b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

float dot(Vec3 a, Vec3 b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

Vec3 cross(Vec3 a, Vec3 b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

Vec3 normalize(Vec3 value) {
    const float length = std::sqrt(dot(value, value));
    return {value[0] / length, value[1] / length, value[2] / length};
}

render::Camera camera(ui::PixelExtent extent) {
    const Vec3 eye{0, 3, -7};
    const Vec3 focus{0, .7f, 3};
    const auto z = normalize(subtract(focus, eye));
    const auto x = normalize(cross({0, 1, 0}, z));
    const auto y = cross(z, x);
    render::Camera result;
    result.view = {x[0], y[0], z[0], 0, x[1],         y[1],         z[1],         0,
                   x[2], y[2], z[2], 0, -dot(x, eye), -dot(y, eye), -dot(z, eye), 1};
    const float aspect = float(extent.width) / float(extent.height);
    result.projection = {1.8f / aspect, 0, 0, 0, 0, 1.8f, 0, 0, 0, 0, -.1f / 39.9f, 1, 0, 0, 4 / 39.9f, 0};
    return result;
}

std::array<float, 16> box(float x, float y, float z, float sx, float sy, float sz) {
    return {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, sz, 0, x, y, z, 1};
}

struct Room {
    std::vector<render::MeshInstance> meshes;

    Room() {
        const auto add = [&](std::array<float, 16> transform, ui::Color color) {
            render::MeshInstance mesh;
            mesh.geometry = atelier::geometry::cube_mesh();
            mesh.world = transform;
            mesh.color = color;
            meshes.push_back(std::move(mesh));
        };
        add(box(0, -.15f, 3.5f, 8, .3f, 7), {.62f, .62f, .62f, 1}); // floor
        add(box(0, 2, 7, 8, 4, .2f), {.62f, .62f, .62f, 1}); // back receiver
        add(box(-4, 2, 3.5f, .2f, 4, 7), {.8f, .06f, .025f, 1}); // warm bounce wall
        add(box(4, 2, 3.5f, .2f, 4, 7), {.025f, .1f, .8f, 1}); // cool bounce wall
        add(box(0, .8f, 3, 1.1f, 1.6f, 1.1f), {.48f, .48f, .48f, 1}); // common occluder
        add(box(-2.1f, .55f, 2, 1.1f, 1.1f, 1.1f), {.7f, .65f, .5f, 1}); // rough metal
        add(box(2.1f, .55f, 2, 1.1f, 1.1f, 1.1f), {.7f, .65f, .5f, 1}); // smooth metal
        add(box(0, 2.75f, 6.7f, 1.6f, .6f, .1f), {0, 0, 0, 1}); // RGB HDR emission
        for (std::size_t index = 0; index < meshes.size(); ++index)
            meshes[index].stable_key = index + 1;
        meshes[0].material.roughness = .18f;
        meshes[0].material.metallic = .65f;
        meshes[5].material = {.roughness = .75f, .metallic = 1};
        meshes[6].material = {.roughness = .08f, .metallic = 1};
        meshes[7].material.emissive = {4, .3f, .05f};
    }
};

struct Region {
    unsigned left{};

    unsigned top{};

    unsigned right{};

    unsigned bottom{};

    float expected_depth{};
};

Region project_region(const render::Camera& camera, ui::Rect viewport, ui::PixelExtent extent, Vec3 point,
                      unsigned radius = 5) {
    std::array<float, 4> value{point[0], point[1], point[2], 1};
    for (const auto& matrix : {camera.view, camera.projection}) {
        std::array<float, 4> next{};
        for (unsigned column = 0; column < 4; ++column)
            for (unsigned row = 0; row < 4; ++row)
                next[column] += value[row] * matrix[row * 4 + column];
        value = next;
    }
    const auto x = static_cast<int>(viewport.x + (value[0] / value[3] * .5f + .5f) * viewport.width);
    const auto y = static_cast<int>(viewport.y + (.5f - value[1] / value[3] * .5f) * viewport.height);
    check(x > int(radius) && y > int(radius) && x + int(radius) < int(extent.width) &&
              y + int(radius) < int(extent.height),
          "SceneLit world-space probe projects inside the capture");
    return {unsigned(x) - radius, unsigned(y) - radius, unsigned(x) + radius + 1, unsigned(y) + radius + 1,
            value[2] / value[3]};
}

std::array<double, 3> mean_rgb(const render::Capture& image, Region region) {
    check(std::abs(depth(image, (region.left + region.right) / 2, (region.top + region.bottom) / 2) -
                   region.expected_depth) < 2e-4f,
          "SceneLit receiver probe depth matches its intended world-space surface");
    std::array<double, 3> result{};
    unsigned count = 0;
    for (unsigned y = region.top; y < region.bottom; ++y)
        for (unsigned x = region.left; x < region.right; ++x) {
            check(depth(image, x, y) > 0, "SceneLit color probe samples a visible receiver");
            for (unsigned channel = 0; channel < 3; ++channel)
                result[channel] += pixel(image, x, y)[channel];
            ++count;
        }
    for (auto& channel : result)
        channel /= count;
    return result;
}

double percentile(std::vector<double> values, unsigned percent) {
    check(!values.empty(), "SceneLit completed GPU sample set is nonempty");
    std::ranges::sort(values);
    return values[(values.size() - 1) * percent / 100];
}

// Independent BRDF quadrature: Lambert, GGX/separable Smith/Schlick; no LTC lookup.
using Rgb3 = std::array<double, 3>;

struct PlaneResponse {
    Rgb3 diffuse{};

    Rgb3 specular{};
};

PlaneResponse plane_response(const Rgb3& v, const Rgb3& l, const Rgb3& base, double roughness, double metallic) {
    PlaneResponse result;
    const double nl = -l[2];
    const double nv = std::max(-v[2], 1e-4);
    if (nl <= 0)
        return result;
    const Rgb3 h{v[0] + l[0], v[1] + l[1], v[2] + l[2]};
    const double hl = std::sqrt(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
    const double nh = std::max(-h[2] / hl, 0.);
    const double vh = std::max((v[0] * h[0] + v[1] * h[1] + v[2] * h[2]) / hl, 0.);
    const double a = std::max(roughness * roughness, .0025);
    const double a2 = a * a;
    const double distribution = a2 / (std::numbers::pi * std::pow(nh * nh * (a2 - 1) + 1, 2));
    const double gv = 2 * nv / (nv + std::sqrt(a2 + (1 - a2) * nv * nv));
    const double gl = 2 * nl / (nl + std::sqrt(a2 + (1 - a2) * nl * nl));
    for (unsigned channel = 0; channel < 3; ++channel) {
        const double f0 = .04 + (base[channel] - .04) * metallic;
        const double fresnel = f0 + (1 - f0) * std::pow(1 - vh, 5);
        result.diffuse[channel] = (1 - fresnel) * base[channel] * (1 - metallic) / std::numbers::pi * nl;
        result.specular[channel] = distribution * gv * gl * fresnel / std::max(4 * nl * nv, 1e-5) * nl;
    }
    return result;
}

double display_code(double radiance) {
    const double x = std::max(radiance, 0.);
    const double mapped = std::clamp(x * (2.51 * x + .03) / (x * (2.43 * x + .59) + .14), 0., 1.);
    return 255 * (mapped <= .0031308 ? mapped * 12.92 : 1.055 * std::pow(mapped, 1 / 2.4) - .055);
}

struct Analytic {
    std::size_t pixels{};

    std::size_t failures{};

    double max_error{};

    double margin{std::numeric_limits<double>::infinity()};
};

// The authored plane covers the viewport. Select pixels from the camera and
// viewport, never captured depth: missing geometry must not remove a test point.
template <class Visit>
void plane_pixels(const render::Frame& frame, const render::Capture& image, float plane_z, Visit&& visit) {
    const auto& view = frame.camera.view;
    const auto& projection = frame.camera.projection;
    const auto& viewport = frame.scene_viewport;
    for (unsigned y = 0; y < image.extent.height; ++y)
        for (unsigned x = 0; x < image.extent.width; ++x) {
            if (x + .5 < viewport.x || x + .5 >= viewport.x + viewport.width || y + .5 < viewport.y ||
                y + .5 >= viewport.y + viewport.height)
                continue;
            const double nx = (x + .5 - viewport.x) / viewport.width * 2 - 1;
            const double ny = 1 - (y + .5 - viewport.y) / viewport.height * 2;
            const Rgb3 p{nx / projection[0] - view[12], ny / projection[5] - view[13], plane_z};
            visit(x, y, p, Rgb3{0, 0, -1});
        }
}

struct RectangleExpected {
    unsigned x;

    unsigned y;

    Rgb3 diffuse;

    Rgb3 specular;
};

std::vector<RectangleExpected> rectangle_expected(const render::Frame& frame, const render::Capture& image,
                                                  const render::Light& light, float plane_z, const Rgb3& base,
                                                  double roughness, double metallic, unsigned samples) {
    std::vector<RectangleExpected> expected;
    const Rgb3 centre{light.position[0], light.position[1], light.position[2]};
    const Rgb3 facing{light.direction[0], light.direction[1], light.direction[2]};
    const Rgb3 right{light.right[0] * double(light.width), light.right[1] * double(light.width),
                     light.right[2] * double(light.width)};
    const Rgb3 up{light.up[0] * double(light.height), light.up[1] * double(light.height),
                  light.up[2] * double(light.height)};
    const double extent = .5 * (double(light.width) + light.height);
    const double range = std::max(double(light.range), 1e-3);
    plane_pixels(frame, image, plane_z, [&](unsigned x, unsigned y, const Rgb3& p, const Rgb3& v) {
        Rgb3 to_centre{};
        double d2 = 0;
        double s = 0;
        double t = 0;
        for (unsigned i = 0; i < 3; ++i) {
            to_centre[i] = centre[i] - p[i];
            d2 += to_centre[i] * to_centre[i];
        }
        for (unsigned i = 0; i < 3; ++i) {
            s -= to_centre[i] * right[i];
            t -= to_centre[i] * up[i];
        }
        s = std::clamp(s / (light.width * double(light.width)), -.5, .5);
        t = std::clamp(t / (light.height * double(light.height)), -.5, .5);
        double nearest = 0;
        for (unsigned i = 0; i < 3; ++i) {
            const double q = to_centre[i] + s * right[i] + t * up[i];
            nearest += q * q;
        }
        const unsigned k = samples <= 1 ? 1
                                        : std::clamp(unsigned(std::ceil(12 * std::max(light.width, light.height) /
                                                                        std::max(std::sqrt(nearest), 1e-6))),
                                                     samples, 768u);
        const double window =
            std::pow(std::clamp(1 - std::pow(std::max(std::sqrt(d2) - extent, 0.) / range, 4), 0., 1.), 2);
        const double area = double(light.width) * light.height / (double(k) * k);
        Rgb3 diffuse{};
        Rgb3 specular{};
        for (unsigned i = 0; i < k; ++i)
            for (unsigned j = 0; j < k; ++j) {
                const double a = (i + .5) / k - .5;
                const double b = (j + .5) / k - .5;
                Rgb3 l{};
                double r2 = 0;
                for (unsigned c = 0; c < 3; ++c) {
                    l[c] = to_centre[c] + a * right[c] + b * up[c];
                    r2 += l[c] * l[c];
                }
                const double r = std::sqrt(r2);
                for (auto& value : l)
                    value /= r;
                double cosine = -(facing[0] * l[0] + facing[1] * l[1] + facing[2] * l[2]);
                cosine = light.two_sided ? std::abs(cosine) : std::max(cosine, 0.);
                if (cosine <= 0)
                    continue;
                const auto response = plane_response(v, l, base, roughness, metallic);
                const double weight = cosine / r2 * area;
                for (unsigned c = 0; c < 3; ++c) {
                    diffuse[c] += response.diffuse[c] * weight;
                    specular[c] += response.specular[c] * weight;
                }
            }
        RectangleExpected value{x, y, {}, {}};
        for (unsigned c = 0; c < 3; ++c) {
            const double radiance = double(light.color[c]) * light.intensity * window;
            value.diffuse[c] = diffuse[c] * radiance;
            value.specular[c] = specular[c] * radiance;
        }
        expected.push_back(value);
    });
    return expected;
}

// Existing LTC accuracy band: quarter specular plus 0.5% of peak radiance,
// HDR half precision and one display code. No tolerance comes from the image.
Analytic analytic_rectangle_plane(const render::Frame& frame, const render::Capture& image, const render::Light& light,
                                  float plane_z, const Rgb3& base, double roughness, double metallic, unsigned samples,
                                  double specular_slack, double peak_slack) {
    const auto expected = rectangle_expected(frame, image, light, plane_z, base, roughness, metallic, samples);
    double peak = 0;
    for (const auto& value : expected)
        for (unsigned c = 0; c < 3; ++c)
            peak = std::max(peak, value.diffuse[c] + value.specular[c]);
    Analytic result;
    result.pixels = expected.size();
    for (const auto& value : expected)
        for (unsigned c = 0; c < 3; ++c) {
            const double total = value.diffuse[c] + value.specular[c];
            const double slack = specular_slack * value.specular[c] + peak_slack * peak;
            const double observed = pixel(image, value.x, value.y)[c];
            const double low = display_code(total * (1 - 1. / 1024) - slack) - 1;
            const double high = display_code(total * (1 + 1. / 1024) + slack) + 1;
            result.max_error = std::max(result.max_error, std::abs(observed - display_code(total)));
            result.margin = std::min(result.margin, std::min(observed - low, high - observed));
            if (observed < low || observed > high) {
                if (result.failures < 4)
                    std::fprintf(
                        stderr,
                        "rectangle truth miss pixel=(%u,%u) channel=%u observed=%.0f band=[%.2f,%.2f] expected=%.5f\n",
                        value.x, value.y, c, observed, low, high, total);
                ++result.failures;
                break;
            }
        }
    return result;
}

std::vector<render::Light> room_lights() {
    std::vector<render::Light> lights(4);
    lights[0].kind = render::LightKind::directional;
    lights[0].direction = normalize({.4f, -1, .6f});
    lights[0].intensity = 1.5f;
    lights[1].position = {-2.5f, 3, 1};
    lights[1].color = {1, .65f, .35f};
    lights[1].intensity = 55;
    lights[2].position = {2.5f, 2.5f, 5};
    lights[2].color = {.35f, .55f, 1};
    lights[2].intensity = 55;
    lights[3].kind = render::LightKind::rectangle;
    lights[3].position = {0, 3.8f, 3};
    lights[3].direction = {0, -1, 0};
    lights[3].right = {1, 0, 0};
    lights[3].up = {0, 0, 1};
    lights[3].width = 2;
    lights[3].height = 1.2f;
    lights[3].intensity = 8;
    for (std::size_t index = 0; index < lights.size(); ++index)
        lights[index].stable_key = 100 + index;
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
          renderer(take(render::Renderer::open(
              window.hwnd, {.validation = benchmark ? atelier::rhi::Validation::disabled : validation,
                            .vsync = false,
                            .shader_cache = shader_cache_directory()}))),
          drawing{.id = 20000, .extent = take(renderer.target()).extent, .draws = {}},
          frame{drawing,
                camera(drawing.extent),
                {0, 0, float(drawing.extent.width), float(drawing.extent.height)},
                room.meshes},
          output(std::move(evidence_path)) {
        std::filesystem::create_directories(output);
        if (benchmark)
            check(drawing.extent == ui::PixelExtent{2560, 1440},
                  "SceneLit benchmark uses the requested native 1440p scene target");
        metrics.open(output / (benchmark ? "benchmark-summary.txt" : "image-metrics.csv"));
        check(metrics.good(), "SceneLit evidence directory opened");
        frame.lights = lights;
        frame.scene_identity = 1;
        frame.environment = {.025f, .035f, .055f};
        if (!benchmark) {
            frame.scene_viewport = {24, 32, float(drawing.extent.width) - 48, float(drawing.extent.height) - 56};
            frame.camera = camera({static_cast<unsigned>(frame.scene_viewport.width),
                                   static_cast<unsigned>(frame.scene_viewport.height)});
            drawing.draws.push_back({{4, 4, 16, 16}, {4, 4, 20, 20}, {0, 1, 0, 1}, {0, 0, 1, 1}, {}});
        }
        frame.view.shading = render::Shading::scene_lit;
        frame.view.lit.probe_volumes[0].minimum = {-5, -1, -1};
        frame.view.lit.probe_volumes[0].maximum = {5, 5, 8};
        frame.view.lit.probe_volumes[0].counts = {11, 7, 10};
        frame.view.lit.probe_update_budget = 96;
        frame.view.lit.probe_rays = 64;
        if (!benchmark)
            metrics << "scenario,mean_rgb_delta,p95_rgb_delta,changed_pixels,darker_pixels\n";
        initial_extent = drawing.extent;
        initial_view = frame.view;
        initial_environment = frame.environment;
    }

    render::FrameResult step(render::Capture* capture = nullptr) {
        render::FrameResult result;
        for (unsigned retry = 0; retry < 120; ++retry) {
            pump();
            ++drawing.id;
            result = take(prepared_render(renderer, frame, capture));
            if (result.status == render::FrameStatus::accepted)
                break;
            if (retry == 0)
                std::fprintf(
                    stderr,
                    "SceneLit transient skipped frame: visible=%d minimized=%d input=%ux%u next=%ux%u; waiting for accepted presentation.\n",
                    IsWindowVisible(window.hwnd), IsIconic(window.hwnd), drawing.extent.width, drawing.extent.height,
                    result.next_extent.width, result.next_extent.height);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(result.status == render::FrameStatus::accepted && result.accepted_id == drawing.id,
              "SceneLit accepted through the ordinary Renderer frame contract");
        if (capture) {
            check(capture->frame_id == drawing.id && capture->extent == drawing.extent &&
                      capture->rgba.size() == std::size_t(drawing.extent.width) * drawing.extent.height * 4,
                  "SceneLit real GPU capture publishes the accepted payload");
            check(std::ranges::all_of(capture->depth,
                                      [](float value) { return std::isfinite(value) && value >= 0 && value <= 1; }),
                  "SceneLit depth contains finite D3D depth values");
        }
        frame.camera_cut = false;
        return result;
    }

    render::Capture capture(const char* name, unsigned settle = 1) {
        for (unsigned index = 1; index < settle; ++index)
            step();
        render::Capture result;
        step(&result);
        png(result, output / (std::string(name) + ".png"));
        return result;
    }

    void edited() {
        frame.meshes = room.meshes;
        frame.lights = lights;
    }

    void restore() {
        frame.scene_viewport = {24, 32, float(initial_extent.width) - 48, float(initial_extent.height) - 56};
        frame.camera = camera(
            {static_cast<unsigned>(frame.scene_viewport.width), static_cast<unsigned>(frame.scene_viewport.height)});
        room = Room{};
        lights = room_lights();
        frame.view = initial_view;
        frame.environment = initial_environment;
        frame.lighting_focus = {};
        frame.camera_cut = true;
        ++frame.scene_identity;
        edited();
    }

    Region region(Vec3 point) const {
        return project_region(frame.camera, frame.scene_viewport, drawing.extent, point);
    }

    void close() {
        frame.release_materials();
        check(bool(renderer.close()), "SceneLit drains GPU resources before HWND destruction");
    }
};

void benchmark(Fixture& f) {
    for (unsigned frame = 0; frame < warmup_frames; ++frame)
        f.step();
    std::ofstream samples(f.output / "benchmark-gpu.csv");
    samples
        << "submission,completed_sequence,completed_frame_id,gbuffer_ms,as_ms,direct_ms,ddgi_ms,gtao_ms,sssr_ms,temporal_ms,composite_ms,sum_gpu_ms,current_logical_resource_bytes,completed_shadow_queries,completed_probe_queries,ddgi_update_ms,ddgi_gather_ms,ddgi_update_visibility_queries,ddgi_gather_visibility_queries,direct_cluster_ms,shadow_update_ms,direct_shading_ms,ddgi_primary_ms,ddgi_hit_lighting_ms,completed_pass_mask\n";
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
            stats.scene_lit_gpu_sample_frame_id <= first_measured_frame)
            continue;
        check(stats.scene_lit_gpu_sample_frame_id <= f.drawing.id,
              "SceneLit asynchronous GPU sample identifies an already submitted frame");
        previous_sequence = stats.scene_lit_gpu_sample_sequence;
        double sum = 0;
        samples << f.drawing.id << ',' << previous_sequence << ',' << stats.scene_lit_gpu_sample_frame_id;
        for (std::size_t pass = 0; pass < passes.size(); ++pass) {
            const double value = stats.scene_lit_gpu_ms[pass];
            check(std::isfinite(value) && value >= 0, "SceneLit GPU timestamps are finite nonnegative milliseconds");
            passes[pass].push_back(value);
            sum += value;
            samples << ',' << value;
        }
        totals.push_back(sum);
        resource_peak = std::max(resource_peak, stats.scene_lit_resource_bytes);
        samples << ',' << sum << ',' << stats.scene_lit_resource_bytes << ',' << stats.scene_lit_shadow_rays << ','
                << stats.scene_lit_probe_rays;
        double ddgi_sum = 0;
        for (std::size_t pass = 0; pass < ddgi_passes.size(); ++pass) {
            const double value = stats.scene_lit_ddgi_gpu_ms[pass];
            check(std::isfinite(value) && value >= 0, "DDGI subpass timestamps are finite nonnegative milliseconds");
            ddgi_passes[pass].push_back(value);
            ddgi_sum += value;
            samples << ',' << value;
        }
        check(ddgi_sum <= stats.scene_lit_gpu_ms[3] + .01,
              "DDGI subpass timestamps belong to the enclosing completed DDGI stage");
        std::uint64_t ddgi_visibility = 0;
        for (auto queries : stats.scene_lit_ddgi_visibility_rays) {
            samples << ',' << queries;
            ddgi_visibility += queries;
        }
        check(ddgi_visibility <= stats.scene_lit_shadow_rays,
              "DDGI classified queries are a subset of all visibility queries");
        const auto subpasses = [&](auto& series, const auto& values, double parent) {
            double sum = 0;
            for (std::size_t i = 0; i < values.size(); ++i) {
                const double value = values[i];
                check(std::isfinite(value) && value >= 0, "GPU subpass timestamps are finite nonnegative milliseconds");
                series[i].push_back(value);
                sum += value;
                samples << ',' << value;
            }
            check(sum <= parent + .01, "GPU subpass timestamps belong to their enclosing completed stage");
        };
        subpasses(direct_passes,
                  std::array{stats.scene_lit_direct_cluster_gpu_ms, stats.scene_lit_shadow_cache_gpu_ms,
                             stats.scene_lit_direct_shading_gpu_ms},
                  stats.scene_lit_gpu_ms[2]);
        subpasses(ddgi_update_passes, stats.scene_lit_ddgi_update_gpu_ms, stats.scene_lit_ddgi_gpu_ms[0]);
        // Shadow atlas updates (bit 13) run only when the static room changes.
        check((stats.scene_lit_gpu_pass_mask & 0x5fffu) == 0x5fffu,
              "fixed room executes every measured GPU scope in the same completed sample");
        samples << ',' << stats.scene_lit_gpu_pass_mask << '\n';
    }
    check(totals.size() >= 120 && percentile(totals, 95) > 0,
          "SceneLit benchmark obtains completed GPU timestamp samples");
    f.metrics
        << "fixed_scene=2560x1440, 8 shared-cube instances, 1 directional + 2 point + 1 rectangle plus DDGI mesh emission\n"
        << "warmup_frames=" << warmup_frames << " gpu_samples=" << totals.size() << "\n"
        << "GPU timestamps exclude capture/readback, CPU Present duration and display scanout.\n"
        << "sum_of_measured_passes_ms p50=" << percentile(totals, 50) << " p95=" << percentile(totals, 95)
        << " p99=" << percentile(totals, 99) << "\nlogical_resource_peak_bytes=" << resource_peak
        << " (logical current allocations, not physical VRAM peak)\n";
    constexpr const char* names[]{"GBuffer", "AS", "direct", "DDGI", "GTAO", "SSSR", "temporal", "composite"};
    for (std::size_t pass = 0; pass < passes.size(); ++pass)
        f.metrics << names[pass] << "_ms p50=" << percentile(passes[pass], 50)
                  << " p95=" << percentile(passes[pass], 95) << " p99=" << percentile(passes[pass], 99) << '\n';
    constexpr const char* ddgi_names[]{"DDGI_update", "DDGI_gather"};
    for (std::size_t pass = 0; pass < ddgi_passes.size(); ++pass)
        f.metrics << ddgi_names[pass] << "_ms p50=" << percentile(ddgi_passes[pass], 50)
                  << " p95=" << percentile(ddgi_passes[pass], 95) << " p99=" << percentile(ddgi_passes[pass], 99)
                  << '\n';
    constexpr const char* direct_names[]{"direct_cluster", "shadow_update", "direct_shading"};
    for (std::size_t pass = 0; pass < direct_passes.size(); ++pass)
        f.metrics << direct_names[pass] << "_ms p50=" << percentile(direct_passes[pass], 50)
                  << " p95=" << percentile(direct_passes[pass], 95) << " p99=" << percentile(direct_passes[pass], 99)
                  << '\n';
    constexpr const char* update_names[]{"DDGI_primary", "DDGI_hit_lighting"};
    for (std::size_t pass = 0; pass < ddgi_update_passes.size(); ++pass)
        f.metrics << update_names[pass] << "_ms p50=" << percentile(ddgi_update_passes[pass], 50)
                  << " p95=" << percentile(ddgi_update_passes[pass], 95)
                  << " p99=" << percentile(ddgi_update_passes[pass], 99) << '\n';
    check(samples.good() && f.metrics.good(), "SceneLit GPU benchmark evidence written");
    // Evidence capture happens only after the timestamp sample loop.
    f.capture("benchmark-1440p");
    std::puts("Renderer SceneLit benchmark: PASS (1440p fixed scene, completed GPU pass timestamps)");
}

#include "scene_lit_rectangle.inl"
#include "scene_lit_shadow.inl"
#include "scene_lit_debug.inl"
#include "scene_lit_sky.inl"
} // namespace scene_lit_e2e

int scene_lit_acceptance(bool benchmark_mode, const std::filesystem::path& output) {
    scene_lit_e2e::Fixture fixture(benchmark_mode, output);
    if (benchmark_mode) {
        scene_lit_e2e::benchmark(fixture);
        scene_lit_e2e::sky_benchmark(fixture);
    } else {
        scene_lit_e2e::rectangle_cases(fixture);
        scene_lit_e2e::shadow_cases(fixture);
        scene_lit_e2e::sky_background_geometry(fixture);
        fixture.restore();
        scene_lit_e2e::probe_debug_cases(fixture);
    }
    fixture.close();
    return 0;
}
