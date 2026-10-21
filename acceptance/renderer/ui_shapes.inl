void ui_shape_cases(render::Renderer& renderer, ui::PixelExtent extent) {
    const ui::Clip full{0,0,int(extent.width),int(extent.height)};
    ui::Frame drawing{.id=2000,.extent=extent,.draws={}};
    render::Frame frame{drawing,{}, {}, {}};
    render::Capture capture;
    const auto box=[&](ui::Rect bounds,ui::Color color,ui::CornerRadii radius={},float border=0,
                       ui::Color stroke={0,0,0,0}) -> ui::DrawQuad& {
        drawing.draws.push_back({bounds,full,color,{0,0,1,1},{},radius,border,stroke});
        return drawing.draws.back();
    };
    box({10,10,100,80},{0,0,0,0},{30,10,0,20},4,{0,0,1,1});
    box({130,10,100,100},{1,0,0,1},{80,0,0,0});
    const auto huge=std::numeric_limits<float>::max();
    box({250,10,80,80},{1,0,0,1},{huge,huge,huge,huge},huge,{0,0,1,1});
    auto image=std::make_shared<ui::ColorImage>();
    image->identity=5; image->revision=1; image->width=3; image->height=2; image->row_pitch=16;
    // RGB is sRGB(linear RGB * alpha); padding must not enter the upload.
    image->pixels={255,0,0,255, 0,188,0,128, 0,0,0,0, 77,77,77,77,
                   0,0,255,255, 188,188,188,128, 0,0,0,0, 77,77,77,77};
    std::weak_ptr<const ui::ColorImage> old=image;
    box({10,130,90,60},{1,1,1,1}).image=image;
    box({110,130,90,60},{1,1,1,1}).image=image;
    // The clipped image clips per instance inside the shared draw; later instances keep their own data.
    auto& clipped=box({210,130,90,60},{1,1,1,1}); clipped.image=image; clipped.clip={220,130,250,190};
    auto coverage=std::make_shared<ui::Coverage>();
    coverage->identity=5; coverage->revision=1; coverage->width=coverage->height=coverage->row_pitch=1;
    coverage->pixels={255};
    box({310,130,20,20},{1,1,0,1}).coverage=coverage;
    // A/B/A must stay in order even when A's resource is identical.
    auto& a=box({350,130,40,40},{1,1,1,.5f}); a.image=image; a.uv={1.f/6, .25f,0,0};
    box({350,130,40,40},{0,1,0,.5f});
    auto& last=box({350,130,40,40},{1,1,1,.5f}); last.image=image; last.uv={1.f/6,.25f,0,0};
    auto& layered=box({410,130,80,60},{1,1,1,1},{20,20,20,20});
    layered.image=image; layered.uv={1.f/6,.25f,0,0}; layered.image_background={0,1,0,1};
    image.reset(); coverage.reset();
    auto result=take(prepared_render(renderer,frame,&capture));
    check(result.status==render::FrameStatus::accepted && result.stats.ui_quads==11 &&
        result.stats.ui_draw_calls==1 && result.stats.ui_instance_upload_bytes==11*112 &&
        result.stats.image_upload_count==1 && result.stats.image_upload_bytes==24 &&
        result.stats.cached_image_count==1 && result.stats.cached_image_texel_bytes==24 &&
        result.stats.cached_image_cpu_bytes==32 && result.stats.coverage_upload_count==1,
        "one draw across clips, R8 coverage and RGBA images with separate resource identities");
    check(pixel(capture,60,11)[2]>250 && pixel(capture,60,50)[2]==0 &&
        pixel(capture,10,10)[2]==0 && pixel(capture,109,89)[2]>250,
        "transparent fill retains an inside border with independent corner radii");
    check(pixel(capture,186,11)[0]==0 && pixel(capture,211,11)[0]>250 && pixel(capture,131,100)[0]>250,
        "asymmetric corner larger than half an edge uses its actual corner region");
    check(pixel(capture,250,10)[2]==0 && pixel(capture,290,50)[2]>250 && pixel(capture,290,10)[2]>245,
        "oversized radius and border normalize without overflowing");
    check(pixel(capture,15,135)[0]>250 && pixel(capture,15,185)[2]>250 &&
        std::abs(int(pixel(capture,55,135)[1])-188)<=3 && pixel(capture,95,135)[0]==0 &&
        pixel(capture,115,135)[0]>250 && pixel(capture,215,135)[0]==0 && pixel(capture,225,135)[0]>230 &&
        pixel(capture,315,135)[0]>250 && pixel(capture,315,135)[1]>250,
        "padded color rows, premultiplied alpha, sampler edges, per-instance clip and texture slots");
    check(std::abs(int(pixel(capture,370,150)[0])-207)<=2 &&
        std::abs(int(pixel(capture,370,150)[1])-137)<=2,
        "A/B/A transparency keeps painter order inside one draw");
    check(pixel(capture,418,133)[0]>120 && pixel(capture,418,133)[0]<200 && pixel(capture,418,133)[1]==0,
        "image and base fill share one antialiased mask without a colored fringe");
    ++drawing.id;
    result=take(prepared_render(renderer,frame));
    check(result.stats.image_upload_count==0 && result.stats.coverage_upload_count==0 && result.stats.ui_draw_calls==1,
        "unchanged image revision reuses GPU upload");
    const auto previous=capture.frame_id;
    const auto valid=drawing.draws.front();
    for (unsigned bad=0;bad<4;++bad) {
        drawing.draws.front()=valid;
        auto& candidate=drawing.draws.front();
        if (bad==0) candidate.radius.top_left=-1;
        if (bad==1) candidate.border_width=std::numeric_limits<float>::quiet_NaN();
        if (bad==2) { candidate.image=drawing.draws[3].image; candidate.coverage=drawing.draws[6].coverage; }
        if (bad==3) { auto malformed=std::make_shared<ui::ColorImage>(*drawing.draws[3].image);
            malformed->row_pitch=3; candidate.image=std::move(malformed); }
        const auto rejected=prepared_render(renderer,frame,&capture);
        check(!rejected && !rejected.error().terminal && capture.frame_id==previous,
            "invalid shape/image rejects before encoding and preserves accepted capture");
    }
    drawing.draws.front()=valid;
    auto replacement=std::make_shared<ui::ColorImage>(*drawing.draws[3].image);
    replacement->revision=2;
    replacement->pixels={0,255,0,255, 0,0,0,0, 0,0,0,0, 77,77,77,77,
                         0,255,0,255, 0,0,0,0, 0,0,0,0, 77,77,77,77};
    for (auto& draw:drawing.draws) if (draw.image) draw.image=replacement;
    replacement.reset(); ++drawing.id;
    result=take(prepared_render(renderer,frame,&capture));
    check(result.stats.image_upload_count==1 && result.stats.cached_image_count==1 && old.expired() &&
        pixel(capture,15,135)[0]==0 && pixel(capture,15,135)[1]>250,
        "immutable image revision replaces pixels and retires old CPU ownership");
    drawing.draws.clear(); ++drawing.id;
    result=take(prepared_render(renderer,frame));
    check(result.stats.ui_draw_calls==0 && result.stats.ui_quads==0 && result.stats.ui_instance_upload_bytes==0 &&
        result.stats.cached_image_count==0 && result.stats.cached_image_texel_bytes==0 &&
        result.stats.cached_image_cpu_bytes==0 && result.stats.cached_coverage_count==0,
        "empty frame retires UI resources without uploading an instance buffer");
    std::puts("Renderer UI shapes/images: PASS (rounded borders, RGBA revisions, one UI draw, ordered alpha, invalid-frame recovery)");
}
