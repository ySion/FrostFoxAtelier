// Bounded, asynchronous samples of the actual DDGI gather. These trajectories
// isolate channels; they are not paired-seed estimates of a noise improvement.
void stability_cases(Fixture& f) {
    struct Restore {
        Fixture& fixture;

        std::vector<render::MeshInstance> meshes = fixture.room.meshes;

        std::vector<render::Light> lights = fixture.lights;

        render::ViewSettings view = fixture.frame.view;

        render::Camera camera = fixture.frame.camera;

        Vec3 environment = fixture.frame.environment;

        Vec3 focus = fixture.frame.lighting_focus;

        std::uint64_t identity = fixture.frame.scene_identity;

        float render_delta = fixture.frame.render_delta_seconds;

        ~Restore() {
            fixture.room.meshes = std::move(meshes);
            fixture.lights = std::move(lights);
            fixture.frame.view = view;
            fixture.frame.camera = camera;
            fixture.frame.environment = environment;
            fixture.frame.lighting_focus = focus;
            fixture.frame.scene_identity = identity;
            fixture.frame.camera_cut = true;
            fixture.frame.render_delta_seconds = render_delta;
            fixture.edited();
        }
    } restore{f};

    f.room = Room{};
    f.lights = room_lights();
    f.lights.front().intensity = 5;
    for (unsigned i = 0; i < 8; ++i) {
        render::Light light;
        light.stable_key = 500 + i;
        light.position = {i % 2 ? 2.8f : -2.8f, 1.2f + .4f * float(i % 3), 1.f + .7f * float(i)};
        light.color = i % 2 ? Vec3{.25f, .45f, 1} : Vec3{1, .45f, .2f};
        light.intensity = 2.f + float(i % 3);
        f.lights.push_back(light);
    }
    f.frame.view = {};
    f.frame.view.shading = render::Shading::scene_lit;
    f.frame.camera = camera(
        {static_cast<unsigned>(f.frame.scene_viewport.width), static_cast<unsigned>(f.frame.scene_viewport.height)});
    f.frame.environment = {.005f, .007f, .01f};
    f.frame.lighting_focus = {};
    f.frame.render_delta_seconds = 1.f / 60.f;
    auto& options = f.frame.view.lit;
    options.ddgi = true;
    options.gtao = options.sssr = options.bloom = false;
    options.antialiasing = render::Antialiasing::smaa;
    options.indirect_gain = 5;
    options.probe_bounce = .85f;
    for (auto& volume : options.probe_volumes)
        volume.enabled = false;
    auto& volume = options.probe_volumes[0];
    volume.enabled = true;
    volume.placement = render::ProbePlacement::fixed;
    volume.blend_cells = 0;
    volume.minimum = {-4.4f, -.4f, 0};
    volume.maximum = {4.4f, 4.4f, 7.5f};
    volume.counts = {5, 5, 7};
    constexpr unsigned probe_count = 175;
    constexpr unsigned sample_count = 32;
    options.probe_update_budget = probe_count;
    options.probe_rays = 128;
    ++f.frame.scene_identity;
    f.frame.camera_cut = true;
    f.edited();
    const auto wall = f.region({-1.4f, 1.8f, 6.9f});
    const std::array<std::uint32_t, 2> wall_pixel{(wall.left + wall.right) / 2, (wall.top + wall.bottom) / 2};
    options.probe_inspect_pixel = wall_pixel;
    // Full coverage settles relocation before a hold mode is evaluated. This
    // is warmup, not a claim that a stochastic transport solution has converged.
    for (unsigned i = 0; i < warmup_frames; ++i)
        f.step();

    std::ofstream evidence(f.output / "ddgi-stability.csv");
    check(evidence.good(), "DDGI stability asynchronous evidence file opened");
    evidence.precision(std::numeric_limits<float>::max_digits10);
    evidence
        << "case,sequence,source_frame,scene_identity,pixel_x,pixel_y,history,rays,updates,record,index,"
           "identity_x,identity_y,stage,age,irradiance_r,irradiance_g,irradiance_b,observations_or_coverage,"
           "spatial_or_sum,facing_squared_or_dominance,moment_visibility,visible_weight,world_x,world_y,world_z,distance_or_valid,"
           "transport_r,transport_g,transport_b,transport_observations,"
           "response_epoch,response_min,response_max,active_visit\n";
    const auto write_record = [&](const char* name, const render::ProbeInspection& sample, const char* kind, int index,
                                  const render::ProbeContribution& record) {
        evidence << name << ',' << sample.sequence << ',' << sample.frame_id << ',' << sample.scene_identity << ','
                 << sample.pixel[0] << ',' << sample.pixel[1] << ',' << static_cast<unsigned>(sample.history) << ','
                 << sample.rays << ',' << sample.updates << ',' << kind << ',' << index;
        for (const auto value : record.identity)
            evidence << ',' << value;
        for (const auto value : record.irradiance)
            evidence << ',' << value;
        for (const auto value : record.weights)
            evidence << ',' << value;
        for (const auto value : record.position)
            evidence << ',' << value;
        for (const auto value : record.transport)
            evidence << ',' << value;
        for (const auto value : record.response)
            evidence << ',' << value;
        evidence << '\n';
    };
    const auto finite_record = [](const render::ProbeContribution& record) {
        const auto finite = [](float value) { return std::isfinite(value); };
        return std::ranges::all_of(record.irradiance, finite) && std::ranges::all_of(record.weights, finite) &&
               std::ranges::all_of(record.position, finite) && std::ranges::all_of(record.transport, finite);
    };
    struct Changes {
        float presentation{};

        float transport{};

        float maximum_presentation{};
    };
    const auto collect = [&](const char* name, render::ProbeHistory history,
                             std::array<std::uint32_t, 2> selected_pixel, bool receiver_expected, unsigned count,
                             bool freeze_presentation = false) {
        options.probe_history = history;
        options.probe_inspect = true;
        options.probe_inspect_pixel = selected_pixel;
        const auto first_frame = f.drawing.id + 1;
        const auto scene_identity = f.frame.scene_identity;
        std::uint64_t last_sequence = 0;
        unsigned accepted = 0;
        unsigned held_comparisons = 0;
        unsigned response_hold_comparisons = 0;
        Changes changes;
        std::shared_ptr<const render::ProbeInspection> previous;
        // Ordinary accepted render calls advance work; no Capture, sleep,
        // completion wait, or association with the CPU's current frame here.
        for (unsigned frame = 0; frame < 192 && accepted < count; ++frame) {
            const auto result = f.step();
            if (receiver_expected) {
                check(result.stats.scene_lit_history_resets == 0, "DDGI isolation does not reset lighting histories");
                for (const auto& state : result.stats.scene_lit_probe_volumes)
                    check(state.reset_probes == 0, "DDGI isolation does not recreate probe world cells");
            }
            const auto sample = result.stats.scene_lit_probe_inspection;
            if (!sample)
                continue;
            check(sample->scene_identity == scene_identity && sample->pixel == selected_pixel,
                  "DDGI published inspection belongs to the requested scene and physical pixel");
            if (sample->history != history || sample->frame_id < first_frame || sample->sequence <= last_sequence)
                continue;
            check(sample->frame_id <= f.drawing.id, "DDGI inspection identifies an already submitted source frame");
            check(sample->rays == 128 && sample->updates == probe_count && sample->volumes[0].resident == probe_count,
                  "DDGI inspection carries its own source sampling configuration");
            check(finite_record(sample->receiver), "DDGI receiver inspection contains finite linear values");
            const auto& receiver = sample->receiver;
            if (receiver_expected) {
                check(receiver.position[3] > 0 && std::abs(receiver.position[2] - 6.9f) < .005f,
                      "DDGI inspection samples the visible rear wall rather than an intervening object");
                check(receiver.identity[2] > 0 && receiver.identity[2] <= 32 && receiver.weights[0] > 0 &&
                          receiver.weights[1] >= 0 && receiver.weights[1] <= 1.00001f && receiver.irradiance[3] >= 0 &&
                          receiver.irradiance[3] <= 1.00001f,
                      "DDGI receiver has bounded coverage and actual contributing support");
                if (previous)
                    check(receiver.position == previous->receiver.position,
                          "DDGI isolation keeps the physical receiver fixed");
            } else {
                check(receiver.position[3] == 0, "DDGI background and out-of-bounds inspection have no receiver");
            }
            write_record(name, *sample, "receiver", -1, receiver);
            unsigned contributors = 0;
            for (std::size_t index = 0; index < sample->probes.size(); ++index) {
                const auto& probe = sample->probes[index];
                check(finite_record(probe), "DDGI neighbor inspection contains finite values");
                check(probe.identity[2] <= 6, "DDGI neighbor support stage is within the published contract");
                check(probe.response[1] <= probe.response[2] && probe.response[2] <= 12,
                      "DDGI response exposes ordered bounded effective-observation counts");
                if (!receiver_expected)
                    check(probe.identity[2] == 0 && probe.irradiance == std::array<float, 4>{} &&
                              probe.transport == std::array<float, 4>{},
                          "DDGI invalid receiver does not publish stale presentation or transport records");
                if (probe.identity[2] != 0) {
                    check(probe.identity[0] < render::probe_volume_limit && probe.identity[1] < probe_count,
                          "DDGI inspected neighbor has a valid resident identity");
                    for (const auto value : probe.irradiance)
                        check(value >= 0, "DDGI irradiance and observation count are nonnegative");
                    for (const auto value : probe.transport)
                        check(value >= 0, "DDGI authoritative transport and observation count are nonnegative");
                    for (const auto value : probe.weights)
                        check(value >= 0 && value <= 1.00001f, "DDGI interpolation factors are bounded");
                }
                if (probe.identity[2] == 6) {
                    ++contributors;
                    for (unsigned c = 0; c < 3; ++c)
                        changes.maximum_presentation = std::max(changes.maximum_presentation, probe.irradiance[c]);
                    check(probe.irradiance[3] > 0 && probe.weights[3] > 0,
                          "DDGI contributor has observed irradiance and positive visible weight");
                    if (previous) {
                        const auto found = std::ranges::find_if(previous->probes, [&](const auto& before) {
                            return before.identity[0] == probe.identity[0] && before.identity[1] == probe.identity[1] &&
                                   before.identity[2] == 6 && before.position == probe.position;
                        });
                        // Geometry invalidation has priority over diagnostic
                        // holds. Compare only the same valid support/position.
                        if (found != previous->probes.end()) {
                            for (unsigned c = 0; c < 3; ++c) {
                                changes.presentation = std::max(changes.presentation,
                                                                std::abs(probe.irradiance[c] - found->irradiance[c]));
                                changes.transport =
                                    std::max(changes.transport, std::abs(probe.transport[c] - found->transport[c]));
                            }
                            if (history == render::ProbeHistory::hold_irradiance || freeze_presentation)
                                check(std::equal(probe.irradiance.begin(), probe.irradiance.begin() + 3,
                                                 found->irradiance.begin()),
                                      "DDGI hold-irradiance or zero accepted delta freezes valid presentation RGB");
                            if (history == render::ProbeHistory::hold_irradiance)
                                check(std::equal(probe.transport.begin(), probe.transport.begin() + 3,
                                                 found->transport.begin()),
                                      "DDGI hold-irradiance also freezes the authoritative transport RGB");
                            if (history == render::ProbeHistory::hold_irradiance &&
                                probe.response[0] == found->response[0]) {
                                check(
                                    probe.response[1] == found->response[1] && probe.response[2] == found->response[2],
                                    "DDGI hold does not consume valid response observations within one authored epoch");
                                if (probe.response[1] < 12 && probe.response[3] > found->response[3])
                                    ++response_hold_comparisons;
                            }
                            if (history == render::ProbeHistory::hold_distance)
                                check(probe.weights[2] == found->weights[2],
                                      "DDGI hold-distance freezes moment visibility at the same receiver and probe");
                            ++held_comparisons;
                        }
                    }
                }
                write_record(name, *sample, "probe", static_cast<int>(index), probe);
            }
            if (receiver_expected)
                check(contributors > 0, "DDGI sample includes inspectable contributor records");
            last_sequence = sample->sequence;
            previous = sample;
            ++accepted;
        }
        check(accepted == count,
              "DDGI gathers the requested number of unique completed samples within its bounded run");
        if (receiver_expected && (history != render::ProbeHistory::live || freeze_presentation))
            check(held_comparisons >= count - 1, "DDGI hold assertion exercised persistent valid probe support");
        if (receiver_expected && history == render::ProbeHistory::hold_irradiance)
            check(response_hold_comparisons > 0,
                  "DDGI hold preserves an unfinished response while actual visits advance");
        evidence.flush();
        check(evidence.good(), "DDGI source-frame and per-neighbor CSV evidence written");
        return changes;
    };

    for (const auto history :
         {render::ProbeHistory::live, render::ProbeHistory::hold_irradiance, render::ProbeHistory::hold_distance}) {
        const char* name = history == render::ProbeHistory::live              ? "stability-live"
                           : history == render::ProbeHistory::hold_irradiance ? "stability-hold-irradiance"
                                                                              : "stability-hold-distance";
        const auto original_intensity = f.lights.front().intensity;
        // Enter hold with an unfinished authored epoch, rather than proving
        // only that an already saturated counter remains at twelve.
        if (history == render::ProbeHistory::hold_irradiance) {
            f.lights.front().intensity *= 1.01f;
            f.edited();
        }
        collect(name, history, wall_pixel, true, sample_count);
        // Only one image per completed trajectory, outside the async sample
        // loop. Depth confirms the selected physical pixel's intended surface.
        const auto image = f.capture(name);
        mean_rgb(image, wall);
        if (history == render::ProbeHistory::hold_irradiance) {
            f.lights.front().intensity = original_intensity;
            f.edited();
        }
    }
    collect("stability-resumed", render::ProbeHistory::live, wall_pixel, true, 4);

    // Source selection changes only the optional ray capture. The receiver
    // still exposes its full gather, including when a different probe is named.
    {
        const auto inspect_selection = [&](std::int32_t requested, bool require_rays) {
            options.probe_inspect = true;
            options.probe_inspect_source = requested;
            const auto first_frame = f.drawing.id + 1;
            std::uint64_t last_sequence = 0;
            unsigned samples = 0;
            unsigned selected_endpoints = 0;
            unsigned primary_distances = 0;
            std::shared_ptr<const render::ProbeInspection> last;
            for (unsigned frame = 0; frame < 128 && samples < 8; ++frame) {
                const auto result = f.step();
                check(result.stats.scene_lit_history_resets == 0,
                      "DDGI capture selection does not reset transport history");
                const auto sample = result.stats.scene_lit_probe_inspection;
                if (!sample)
                    continue;
                check(sample->requested_probe == requested && sample->scene_identity == f.frame.scene_identity &&
                          sample->pixel == wall_pixel,
                      "DDGI changed selection filters late samples from the previous request");
                if (sample->frame_id < first_frame || sample->sequence <= last_sequence)
                    continue;
                if (requested >= static_cast<std::int32_t>(probe_count)) {
                    check(sample->traced_probe == ~0u &&
                              std::ranges::none_of(sample->ray_samples,
                                                   [](const auto& ray) { return ray.identity[3] != 0; }),
                          "DDGI out-of-resident-range explicit selection publishes no old or fallback ray records");
                } else {
                    if (requested >= 0)
                        check(sample->traced_probe == static_cast<std::uint32_t>(requested),
                              "DDGI explicit selection captures the requested physical probe");
                    const auto origin = std::ranges::find_if(sample->probes, [&](const auto& probe) {
                        return probe.identity[2] != 0 && probe.identity[1] == sample->traced_probe;
                    });
                    for (const auto& ray : sample->ray_samples) {
                        const auto finite = [](float value) { return std::isfinite(value); };
                        check(std::ranges::all_of(ray.endpoint_distance, finite) &&
                                  std::ranges::all_of(ray.source_parameters, finite),
                              "DDGI selected-source geometry records remain finite, including cleared slots");
                        if (!ray.identity[3])
                            continue;
                        check(sample->traced_probe < probe_count && ray.identity[2] < sample->rays,
                              "DDGI selected ray belongs to the current resident field and ray budget");
                        if (origin != sample->probes.end()) {
                            double distance_squared = 0;
                            for (unsigned axis = 0; axis < 3; ++axis) {
                                const double delta = double(ray.surface[axis]) - origin->position[axis];
                                distance_squared += delta * delta;
                            }
                            const double distance = std::sqrt(distance_squared);
                            check(std::isfinite(ray.sampling[3]) &&
                                      std::abs(distance - ray.sampling[3]) <= 1e-4 + 3e-5 * distance,
                                  "DDGI sampling.w remains probe-to-hit distance rather than source distance");
                            ++primary_distances;
                        }
                        if (ray.identity[0] == ~0u) {
                            check(ray.endpoint_distance[3] == -1,
                                  "DDGI absent selected source has an explicit invalid endpoint distance");
                            continue;
                        }
                        double distance_squared = 0;
                        for (unsigned axis = 0; axis < 3; ++axis) {
                            const double delta = double(ray.endpoint_distance[axis]) - ray.surface[axis];
                            distance_squared += delta * delta;
                        }
                        const double distance = std::sqrt(distance_squared);
                        check(ray.endpoint_distance[3] >= 0 &&
                                  std::abs(distance - ray.endpoint_distance[3]) <= 1e-4 + 3e-5 * distance,
                              "DDGI selected-light endpoint distance matches its world endpoint and hit position");
                        ++selected_endpoints;
                    }
                }
                last_sequence = sample->sequence;
                last = sample;
                ++samples;
            }
            check(samples == 8, "DDGI capture selection receives bounded unique completed samples");
            if (require_rays)
                check(selected_endpoints > 0 && primary_distances > 0,
                      "DDGI capture selection exercises both source-endpoint and primary-hit distance records");
            return last;
        };
        const auto automatic = inspect_selection(-1, true);
        auto selected = automatic->probes.end();
        for (auto probe = automatic->probes.begin(); probe != automatic->probes.end(); ++probe)
            if (probe->identity[2] == 6 &&
                (selected == automatic->probes.end() || probe->weights[3] < selected->weights[3]))
                selected = probe;
        check(selected != automatic->probes.end(), "DDGI explicit capture can select a real receiver contributor");
        // Prefer a lesser contributor so explicit capture does not merely test
        // the same maximum-weight choice made by automatic selection.
        inspect_selection(static_cast<std::int32_t>(selected->identity[1]), true);
        inspect_selection(static_cast<std::int32_t>(render::probe_resident_limit - 1), false);
        inspect_selection(-1, true);
        options.probe_inspect = false;
        for (unsigned frame = 0; frame < 40; ++frame)
            check(!f.step().stats.scene_lit_probe_inspection,
                  "DDGI disabled capture suppresses late explicit and automatic readbacks");
    }

    // Explicit accepted render time, independent of fixture wall-clock speed.
    // Only source radiance changes: geometry, world cells and receiver stay put.
    // Record the darkening tail; this short lifecycle check does not impose an
    // unmeasured t90 or pretend that sixteen samples prove convergence.
    const auto active_lights = f.lights;
    const auto active_environment = f.frame.environment;
    const auto active_emission = f.room.meshes[7].material.emissive;
    f.frame.render_delta_seconds = 0;
    for (auto& light : f.lights)
        light.intensity = 0;
    f.frame.environment = {};
    f.room.meshes[7].material.emissive = {};
    f.edited();
    const auto paused =
        collect("stability-delta-zero-lights-off", render::ProbeHistory::live, wall_pixel, true, 16, true);
    check(paused.transport > 0, "DDGI transport continues observing while zero accepted delta holds presentation");
    f.frame.render_delta_seconds = 1.f / 60.f;
    const auto resumed =
        collect("stability-delta-resumed-lights-off", render::ProbeHistory::live, wall_pixel, true, 16);
    check(resumed.presentation > 0, "DDGI presentation responds again when positive accepted render time resumes");
    f.lights = active_lights;
    f.frame.environment = active_environment;
    f.room.meshes[7].material.emissive = active_emission;
    f.edited();
    collect("stability-lights-on", render::ProbeHistory::live, wall_pixel, true, 16);

    // Compare response implementations by running this identical fixture on
    // each binary. The CSV records observations, not a preselected t90 target.
    // Keep one volume so both fields can be reconstructed with exactly the
    // same visible weights and coverage as the actual receiver gather.
    {
        const auto bright_meshes = f.room.meshes;
        constexpr unsigned response_frames = 384;
        constexpr unsigned baseline_frames = 64;
        constexpr unsigned drain_frames = 32;
        constexpr float response_delta = 1.f / 60.f;
        static_assert(sizeof(render::ProbeInspection::Ray) == 96);
        std::ofstream response(f.output / "ddgi-response.csv");
        check(response.good(), "DDGI source-frame response evidence file opened");
        response.precision(std::numeric_limits<double>::max_digits10);
        response << "budget,bounce,phase,sequence,source_frame,scene_identity,pixel_x,pixel_y,"
                    "source_ordinal,phase_frame,phase_seconds,render_delta_seconds,coverage,weight_sum,contributors,"
                    "presentation_y,transport_y,presentation_r,presentation_g,presentation_b,"
                    "transport_r,transport_g,transport_b,traced_probe,valid_ray_records\n";
        for (const auto budget : {16u, probe_count})
            for (const auto bounce : {0.f, .85f}) {
                f.room.meshes = bright_meshes;
                f.lights = active_lights;
                f.frame.environment = active_environment;
                options.probe_bounce = bounce;
                options.probe_history = render::ProbeHistory::live;
                options.probe_inspect = false;
                options.probe_inspect_pixel = wall_pixel;
                options.probe_update_budget = probe_count;
                f.frame.render_delta_seconds = response_delta;
                ++f.frame.scene_identity;
                f.frame.camera_cut = true;
                f.edited();
                // Warm the bright field at full coverage before measuring either
                // budget. The baseline below exposes any remaining stochastic drift.
                for (unsigned frame = 0; frame < 512; ++frame)
                    f.step();
                options.probe_update_budget = budget;
                options.probe_inspect = true;
                const auto response_identity = f.frame.scene_identity;
                struct SourceFrame {
                    std::uint64_t id;

                    unsigned phase;

                    unsigned ordinal;

                    unsigned phase_frame;

                    float delta;
                };
                std::vector<SourceFrame> source_frames;
                source_frames.reserve(baseline_frames + 2 * response_frames + drain_frames);
                std::array<unsigned, 3> samples{};
                std::array<unsigned, 3> last_phase_frame{};
                struct ResponseStart {
                    bool valid{};

                    std::array<float, 4> position{};

                    std::uint32_t epoch{};

                    std::uint32_t visit{};
                };
                std::array<std::array<ResponseStart, probe_count>, 3> response_start{};
                std::array<bool, 3> mature_response{};
                float baseline_coverage = -1;
                std::uint64_t last_sequence = 0;
                unsigned valid_rays = 0;
                bool observed_bright_baseline = false;
                const std::array<const char*, 3> phase_names{"bright", "off", "on"};
                const auto consume = [&](const render::FrameResult& result) {
                    const auto sample = result.stats.scene_lit_probe_inspection;
                    if (!sample || sample->sequence <= last_sequence || sample->scene_identity != response_identity)
                        return;
                    if (sample->history != render::ProbeHistory::live || sample->pixel != wall_pixel)
                        return;
                    const auto source =
                        std::lower_bound(source_frames.begin(), source_frames.end(), sample->frame_id,
                                         [](const SourceFrame& value, std::uint64_t id) { return value.id < id; });
                    // A late sample belongs to its own source phase. Never consume
                    // warmup, an internal skipped attempt, or drain as a response row.
                    if (source == source_frames.end() || source->id != sample->frame_id)
                        return;
                    last_sequence = sample->sequence;
                    if (source->phase >= phase_names.size())
                        return;
                    check(sample->rays == 128 && sample->updates == budget &&
                              sample->volumes[0].resident == probe_count,
                          "DDGI response sample carries the requested source budget and resident field");
                    check(finite_record(sample->receiver) && sample->receiver.position[3] > 0 &&
                              std::abs(sample->receiver.position[2] - 6.9f) < .005f,
                          "DDGI response retains a finite visible rear-wall receiver");
                    for (std::size_t slot = 1; slot < sample->volumes.size(); ++slot)
                        check(!sample->volumes[slot].enabled, "DDGI response reconstructs exactly one volume");
                    std::array<double, 3> presented{};
                    std::array<double, 3> transported{};
                    double weight_sum = 0;
                    unsigned contributors = 0;
                    for (const auto& probe : sample->probes) {
                        check(finite_record(probe), "DDGI response probe records remain finite");
                        if (probe.identity[2] != 6)
                            continue;
                        check(probe.identity[0] == 0 && probe.identity[1] < probe_count && probe.weights[3] > 0 &&
                                  probe.irradiance[3] > 0 && probe.transport[3] > 0,
                              "DDGI response uses only observed contributors with legal positive weights");
                        check(probe.response[1] <= probe.response[2] && probe.response[2] <= 12,
                              "DDGI response counters stay within their published twelve-observation bound");
                        auto& start = response_start[source->phase][probe.identity[1]];
                        if (!start.valid || start.position != probe.position || start.epoch != probe.response[0])
                            start = {true, probe.position, probe.response[0], probe.response[3]};
                        // These curves use 128 rays, stable origins and continuous
                        // live updates. Demand maturity only after observing ample
                        // actual visits in this same epoch, never from frame count
                        // alone or from the later one/three-ray budget cases.
                        if (probe.response[3] >= start.visit && probe.response[3] - start.visit >= 24) {
                            check(probe.response[1] == 12 && probe.response[2] == 12,
                                  "DDGI stable 128-ray response matures after sufficient observed active visits");
                            mature_response[source->phase] = true;
                        }
                        const double weight = probe.weights[3];
                        weight_sum += weight;
                        ++contributors;
                        for (unsigned c = 0; c < 3; ++c) {
                            check(probe.irradiance[c] >= 0 && probe.transport[c] >= 0,
                                  "DDGI response fields are nonnegative");
                            presented[c] += weight * probe.irradiance[c];
                            transported[c] += weight * probe.transport[c];
                        }
                    }
                    const double coverage = sample->receiver.irradiance[3];
                    if (baseline_coverage < 0)
                        baseline_coverage = sample->receiver.irradiance[3];
                    check(std::abs(coverage - baseline_coverage) <= 1e-5,
                          "DDGI radiance-response epochs preserve existing receiver coverage");
                    check(contributors > 0 && contributors == sample->receiver.identity[2] && weight_sum > 0 &&
                              coverage > 0 && coverage <= 1.00001,
                          "DDGI response has actual bounded contributing support");
                    for (unsigned c = 0; c < 3; ++c) {
                        presented[c] *= coverage / weight_sum;
                        transported[c] *= coverage / weight_sum;
                        check(std::isfinite(presented[c]) && std::isfinite(transported[c]) &&
                                  std::abs(presented[c] - sample->receiver.irradiance[c]) <= 1e-5 + 2e-5 * presented[c],
                              "DDGI response reconstruction matches the actual presentation gather");
                    }
                    unsigned ray_records = 0;
                    for (const auto& ray : sample->ray_samples) {
                        const auto finite = [](float value) { return std::isfinite(value); };
                        check(std::ranges::all_of(ray.direct, finite) && std::ranges::all_of(ray.surface, finite) &&
                                  std::ranges::all_of(ray.sampling, finite) &&
                                  std::ranges::all_of(ray.endpoint_distance, finite) &&
                                  std::ranges::all_of(ray.source_parameters, finite),
                              "DDGI response raw 96-byte ray records are finite, including unused entries");
                        if (!ray.identity[3])
                            continue;
                        check(sample->traced_probe < probe_count && ray.identity[2] < sample->rays,
                              "DDGI response raw ray belongs to a resident probe and actual ray ordinal");
                        ++ray_records;
                    }
                    valid_rays += ray_records;
                    const auto luminance = [](const auto& rgb) {
                        return .2126 * rgb[0] + .7152 * rgb[1] + .0722 * rgb[2];
                    };
                    if (source->phase == 0 && luminance(presented) > 0 && luminance(transported) > 0)
                        observed_bright_baseline = true;
                    response << budget << ',' << bounce << ',' << phase_names[source->phase] << ',' << sample->sequence
                             << ',' << sample->frame_id << ',' << sample->scene_identity << ',' << sample->pixel[0]
                             << ',' << sample->pixel[1] << ',' << source->ordinal << ',' << source->phase_frame << ','
                             << double(source->phase_frame) * source->delta << ',' << source->delta << ',' << coverage
                             << ',' << weight_sum << ',' << contributors << ',' << luminance(presented) << ','
                             << luminance(transported);
                    for (const auto value : presented)
                        response << ',' << value;
                    for (const auto value : transported)
                        response << ',' << value;
                    response << ',' << sample->traced_probe << ',' << ray_records << '\n';
                    ++samples[source->phase];
                    last_phase_frame[source->phase] = source->phase_frame;
                };
                const auto run_phase = [&](unsigned phase, unsigned frames) {
                    for (unsigned frame = 1; frame <= frames; ++frame) {
                        const auto result = f.step();
                        source_frames.push_back({result.accepted_id, phase,
                                                 static_cast<unsigned>(source_frames.size() + 1), frame,
                                                 f.frame.render_delta_seconds});
                        check(result.stats.scene_lit_history_resets == 0,
                              "DDGI response does not reset the transport field");
                        for (const auto& state : result.stats.scene_lit_probe_volumes)
                            check(state.reset_probes == 0,
                                  "DDGI response retains its world cells across source changes");
                        consume(result);
                    }
                };
                run_phase(0, baseline_frames);
                for (auto& light : f.lights)
                    light.intensity = 0;
                f.frame.environment = {};
                for (auto& mesh : f.room.meshes)
                    mesh.material.emissive = {};
                f.edited();
                // Analytic lights, sky and every emissive mesh are zero. The
                // physical dark endpoint is zero even while old bounces decay.
                run_phase(1, response_frames);
                f.lights = active_lights;
                f.frame.environment = active_environment;
                f.room.meshes = bright_meshes;
                f.edited();
                run_phase(2, response_frames);
                run_phase(3, drain_frames);
                for (unsigned phase = 0; phase < samples.size(); ++phase) {
                    check(samples[phase] >= 16,
                          "DDGI response phase contains unique completed diagnostic observations");
                    const auto end = phase == 0 ? baseline_frames : response_frames;
                    check(last_phase_frame[phase] + drain_frames >= end,
                          "DDGI response observations reach the end of each bounded phase");
                }
                check(observed_bright_baseline,
                      "DDGI response starts from observed nonzero bright transport and presentation");
                check(mature_response[1] && mature_response[2],
                      "DDGI off/on curves exercise effective-response maturity after actual visits");
                check(valid_rays > 0, "DDGI response includes actual secondary-hit ray diagnostics");
                response.flush();
                check(response.good(), "DDGI response weighted transport and presentation CSV written");
                std::printf("DDGI response recorded: budget=%u bounce=%.2f samples=%u/%u/%u valid-rays=%u\n", budget,
                            bounce, samples[0], samples[1], samples[2], valid_rays);
            }
        f.room.meshes = bright_meshes;
        f.lights = active_lights;
        f.frame.environment = active_environment;
        options.probe_update_budget = probe_count;
        options.probe_bounce = .85f;
        options.probe_history = render::ProbeHistory::live;
        options.probe_inspect = true;
        f.frame.render_delta_seconds = response_delta;
        f.edited();
    }

    // A new dark scene cannot retain the former scene's displayed light even
    // when time is stopped. Consume the expected lifetime-reset frame before
    // collecting the new scene's stable, source-filtered metadata.
    f.frame.render_delta_seconds = 0;
    for (auto& light : f.lights)
        light.intensity = 0;
    f.frame.environment = {};
    f.room.meshes[7].material.emissive = {};
    ++f.frame.scene_identity;
    f.edited();
    f.step();
    const auto fresh = collect("stability-new-scene-zero-time", render::ProbeHistory::live, wall_pixel, true, 4, true);
    check(fresh.maximum_presentation == 0,
          "DDGI scene replacement invalidates old display energy immediately even at zero delta");
    options.probe_inspect = false;
    for (unsigned i = 0; i < 4; ++i)
        check(!f.step().stats.scene_lit_probe_inspection,
              "DDGI disabled inspection does not retain a published sample");

    // Replacing the scene must filter old completed payloads before publication,
    // not only when the consumer happens to compare a scene identity.
    ++f.frame.scene_identity;
    f.frame.meshes = {};
    for (unsigned i = 0; i < 4; ++i)
        check(!f.step().stats.scene_lit_probe_inspection,
              "DDGI empty scene skips SceneLit and publishes no inspection");
    // Keep the ordinary SceneLit path active with real geometry outside the
    // camera frustum. The selected client pixel is inside the viewport but has
    // background depth; empty-scene rendering is deliberately not expanded.
    f.room.meshes.resize(1);
    f.room.meshes.front().world = box(1000, 2, 7, 8, 4, .2f);
    ++f.frame.scene_identity;
    f.edited();
    collect("stability-background", render::ProbeHistory::live, wall_pixel, false, 4);
    collect("stability-outside", render::ProbeHistory::live, {f.drawing.extent.width, f.drawing.extent.height}, false,
            4);
    options.ddgi = false;
    for (unsigned i = 0; i < 4; ++i)
        check(!f.step().stats.scene_lit_probe_inspection, "DDGI inactive field does not publish inspection data");

    // The existing constant-sky contract uses a tiny open receiver and eight
    // surrounding probes. Reuse that geometry pattern at our known wall pixel;
    // this checks legal scratch strides and support, not low-ray convergence.
    {
        using atelier::geometry::Mesh;
        using atelier::geometry::Vertex;
        render::MeshInstance receiver;
        receiver.geometry = std::make_shared<const Mesh>(std::vector<Vertex>{{{-1.8f, 1.4f, 6.9f}, {0, 0, -1}},
                                                                             {{-1.f, 1.4f, 6.9f}, {0, 0, -1}},
                                                                             {{-1.f, 2.2f, 6.9f}, {0, 0, -1}},
                                                                             {{-1.8f, 1.4f, 6.9f}, {0, 0, -1}},
                                                                             {{-1.f, 2.2f, 6.9f}, {0, 0, -1}},
                                                                             {{-1.8f, 2.2f, 6.9f}, {0, 0, -1}}});
        receiver.world = box(0, 0, 0, 1, 1, 1);
        receiver.stable_key = 1;
        receiver.color = {.4f, .4f, .4f, 1};
        receiver.material = {.roughness = 1};
        f.room.meshes = {receiver};
        f.lights.clear();
        f.frame.environment = {.1f, .1f, .1f};
        f.frame.render_delta_seconds = 1.f / 60.f;
        options.ddgi = true;
        options.probe_bounce = 0;
        options.probe_history = render::ProbeHistory::live;
        options.probe_inspect = true;
        options.probe_inspect_pixel = wall_pixel;
        volume.minimum = {-2.4f, .8f, 5.9f};
        volume.maximum = {-.4f, 3.8f, 7.9f};
        volume.counts = {2, 2, 2};
        options.probe_update_budget = 8;
        options.probe_rays = 1;
        ++f.frame.scene_identity;
        f.frame.camera_cut = true;
        f.edited();
        f.step();
        const auto sky_identity = f.frame.scene_identity;
        for (const unsigned rays : {1u, 3u, 127u, 128u, 255u, 256u}) {
            options.probe_rays = rays;
            const auto first_frame = f.drawing.id + 1;
            std::uint64_t last_sequence = 0;
            unsigned samples = 0;
            bool observed_support = false;
            const auto name = "stability-sky-rays-" + std::to_string(rays);
            for (unsigned frame = 0; frame < 128 && samples < 16; ++frame) {
                const auto result = f.step();
                const auto sample = result.stats.scene_lit_probe_inspection;
                if (!sample || sample->scene_identity != sky_identity || sample->frame_id < first_frame ||
                    sample->rays != rays || sample->sequence <= last_sequence)
                    continue;
                check(sample->pixel == wall_pixel && sample->history == render::ProbeHistory::live &&
                          sample->updates == 8 && sample->volumes[0].resident == 8,
                      "DDGI sky budget sample belongs to the requested ray stride and receiver");
                check(finite_record(sample->receiver) && sample->receiver.position[3] > 0 &&
                          std::abs(sample->receiver.position[2] - 6.9f) < .005f &&
                          sample->receiver.irradiance[3] >= 0 && sample->receiver.irradiance[3] <= 1.00001f,
                      "DDGI sky receiver remains finite with bounded support across legal ray budgets");
                write_record(name.c_str(), *sample, "receiver", -1, sample->receiver);
                for (std::size_t index = 0; index < sample->probes.size(); ++index) {
                    const auto& probe = sample->probes[index];
                    check(finite_record(probe) && probe.identity[2] <= 6 && probe.response[1] <= probe.response[2] &&
                              probe.response[2] <= 12,
                          "DDGI sky probe values and effective counts remain legal at every ray budget");
                    if (probe.identity[2])
                        check(probe.identity[0] == 0 && probe.identity[1] < 8,
                              "DDGI sky support references the eight resident probes");
                    if (probe.identity[2] == 6) {
                        check(probe.weights[3] > 0 && probe.irradiance[3] > 0 && probe.transport[3] > 0,
                              "DDGI sky contributor has actual observed support and positive weight");
                        observed_support = true;
                    }
                    write_record(name.c_str(), *sample, "probe", static_cast<int>(index), probe);
                }
                for (const auto& ray : sample->ray_samples) {
                    const auto finite = [](float value) { return std::isfinite(value); };
                    check(std::ranges::all_of(ray.direct, finite) && std::ranges::all_of(ray.surface, finite) &&
                              std::ranges::all_of(ray.sampling, finite) &&
                              std::ranges::all_of(ray.endpoint_distance, finite) &&
                              std::ranges::all_of(ray.source_parameters, finite),
                          "DDGI sky ray diagnostics are finite across scratch strides");
                    if (ray.identity[3])
                        check(ray.identity[2] < rays, "DDGI sky valid ray ordinal respects the current budget");
                }
                last_sequence = sample->sequence;
                ++samples;
            }
            check(samples == 16 && observed_support, "DDGI legal ray budget produces actual finite supported samples");
        }
        evidence.flush();
        check(evidence.good(), "DDGI legal ray-budget and response-count evidence written");
    }
    std::puts(
        "Renderer DDGI stability: PASS (async E/w/transport and effective response, holds, source-time curves, lifecycle, legal ray budgets)");
}
