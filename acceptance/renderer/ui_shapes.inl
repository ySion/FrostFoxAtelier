void ui_shape_cases(render::Renderer& renderer, ui::PixelExtent extent) {
    const ui::Clip full{0, 0, int(extent.width), int(extent.height)};
    ui::Frame drawing{.id = 2000, .extent = extent, .draws = {}};
    render::Frame frame{drawing, {}, {}, {}};
    render::Capture capture;
    const auto box = [&](ui::Rect bounds, ui::Color color, ui::CornerRadii radius = {}, float border = 0,
                         ui::Color stroke = {0, 0, 0, 0}) -> ui::DrawQuad& {
        drawing.draws.push_back({bounds, full, color, {0, 0, 1, 1}, {}, radius, border, stroke});
        return drawing.draws.back();
    };
    box({10, 10, 100, 80}, {0, 0, 0, 0}, {30, 10, 0, 20}, 4, {0, 0, 1, 1});
    box({130, 10, 100, 100}, {1, 0, 0, 1}, {80, 0, 0, 0});
    auto image = std::make_shared<ui::ColorImage>();
    image->identity = 5;
    image->revision = 1;
    image->width = 3;
    image->height = 2;
    image->row_pitch = 16;
    // RGB is sRGB(linear RGB * alpha); padding must not enter the upload.
    image->pixels = {255, 0, 0,   255, 0,   188, 0,   128, 0, 0, 0, 0, 77, 77, 77, 77,
                     0,   0, 255, 255, 188, 188, 188, 128, 0, 0, 0, 0, 77, 77, 77, 77};
    box({10, 130, 90, 60}, {1, 1, 1, 1}).image = image;
    box({110, 130, 90, 60}, {1, 1, 1, 1}).image = image;
    // The clipped image clips per instance inside the shared draw; later instances keep their own data.
    auto& clipped = box({210, 130, 90, 60}, {1, 1, 1, 1});
    clipped.image = image;
    clipped.clip = {220, 130, 250, 190};
    auto coverage = std::make_shared<ui::Coverage>();
    coverage->identity = 5;
    coverage->revision = 1;
    coverage->width = coverage->height = coverage->row_pitch = 1;
    coverage->pixels = {255};
    box({310, 130, 20, 20}, {1, 1, 0, 1}).coverage = coverage;
    // A/B/A must stay in order even when A's resource is identical.
    auto& a = box({350, 130, 40, 40}, {1, 1, 1, .5f});
    a.image = image;
    a.uv = {1.f / 6, .25f, 0, 0};
    box({350, 130, 40, 40}, {0, 1, 0, .5f});
    auto& last = box({350, 130, 40, 40}, {1, 1, 1, .5f});
    last.image = image;
    last.uv = {1.f / 6, .25f, 0, 0};
    auto& layered = box({410, 130, 80, 60}, {1, 1, 1, 1}, {20, 20, 20, 20});
    layered.image = image;
    layered.uv = {1.f / 6, .25f, 0, 0};
    layered.image_background = {0, 1, 0, 1};
    image.reset();
    coverage.reset();
    take(prepared_render(renderer, frame, &capture));
    check(pixel(capture, 60, 11)[2] > 250 && pixel(capture, 60, 50)[2] == 0 && pixel(capture, 10, 10)[2] == 0 &&
              pixel(capture, 109, 89)[2] > 250,
          "transparent fill retains an inside border with independent corner radii");
    check(pixel(capture, 186, 11)[0] == 0 && pixel(capture, 211, 11)[0] > 250 && pixel(capture, 131, 100)[0] > 250,
          "asymmetric corner larger than half an edge uses its actual corner region");
    check(pixel(capture, 15, 135)[0] > 250 && pixel(capture, 15, 185)[2] > 250 &&
              std::abs(int(pixel(capture, 55, 135)[1]) - 188) <= 3 && pixel(capture, 95, 135)[0] == 0 &&
              pixel(capture, 115, 135)[0] > 250 && pixel(capture, 215, 135)[0] == 0 &&
              pixel(capture, 225, 135)[0] > 230 && pixel(capture, 315, 135)[0] > 250 &&
              pixel(capture, 315, 135)[1] > 250,
          "padded color rows, premultiplied alpha, sampler edges, per-instance clip and texture slots");
    check(std::abs(int(pixel(capture, 370, 150)[0]) - 207) <= 2 &&
              std::abs(int(pixel(capture, 370, 150)[1]) - 137) <= 2,
          "A/B/A transparency keeps painter order inside one draw");
    check(pixel(capture, 418, 133)[0] > 120 && pixel(capture, 418, 133)[0] < 200 && pixel(capture, 418, 133)[1] == 0,
          "image and base fill share one antialiased mask without a colored fringe");
    snapshot(capture, "ui-shapes.ppm");
    std::puts("Renderer UI shapes/images: PASS (rounded borders, clip and ordered alpha)");
}
