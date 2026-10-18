// Material and view edits leave every transform fixed. Observe only the actual
// reflection footprint: the emissive object's own immediate color response
// must not dilute a stale reflection's measured tail.
void quality_signal_edits(Fixture& f) {
    const auto meshes=f.room.meshes;
    const auto lights=f.lights;
    const auto view=f.frame.view;
    const auto camera=f.frame.camera;
    const auto environment=f.frame.environment;
    const Room defaults;
    bool passed=true;
    std::ofstream evidence(f.output/"signal-edits.csv");
    evidence<<"case,frames_after_edit,relative_error,mean_rgb8_error,static_noise_relative,changed_pixels,max_off_reference_rgb8_error\n";
    constexpr const char* names[]{"signal-emission-off","signal-equal-luma-red-to-blue",
                                  "signal-material-roughness-cutoff","signal-view-roughness-cutoff"};
    constexpr std::array<unsigned,6> times{0,1,2,4,8,16};
    for(unsigned mode=0;mode<4;++mode) {
        f.frame.view={};f.frame.view.shading=render::Shading::scene_lit;
        f.frame.camera=scene_lit_e2e::camera({static_cast<unsigned>(f.frame.scene_viewport.width),
                                            static_cast<unsigned>(f.frame.scene_viewport.height)});
        f.frame.environment={};f.lights.clear();
        auto& options=f.frame.view.lit;
        options.antialiasing=render::Antialiasing::smaa;
        options.ddgi=options.gtao=options.sssr=options.ssil=options.bloom=false;
        f.room.meshes={defaults.meshes[0],defaults.meshes[7]};
        auto& floor=f.room.meshes[0];auto& emitter=f.room.meshes[1];
        floor.color={.7f,.7f,.7f,1};
        floor.material={.roughness=mode==2?.58f:.18f,.metallic=1};
        emitter.world=box(-1.4f,2.1f,6.2f,1.6f,1.8f,.1f);
        emitter.color={0,0,0,1};emitter.material={.roughness=1,.emissive={12,0,0}};
        ++f.frame.scene_identity;f.frame.camera_cut=true;f.edited();
        const std::string name=names[mode];
        const auto initial_off=f.capture((name+"-initial-sssr-off").c_str(),2);
        options.sssr=true;f.frame.camera_cut=true;
        const auto before=f.capture((name+"-before").c_str(),128);
        std::vector<std::size_t> receivers;
        for(std::size_t i=0;i<before.depth.size();++i) {
            if(before.depth[i]<=0 || before.depth[i]!=initial_off.depth[i])continue;
            unsigned delta=0;
            for(unsigned c=0;c<3;++c) delta=std::max(delta,
                unsigned(std::abs(int(before.rgba[i*4+c])-int(initial_off.rgba[i*4+c]))));
            if(delta>=4)receivers.push_back(i);
        }
        check(receivers.size()>=64,"signal edit starts with an observable SSR footprint independent of visible self-emission");
        if(mode==0)emitter.material.emissive={0,0,0};
        else if(mode==1) {
            // Scene-linear Rec.709 luminance is unchanged; only chroma changes.
            emitter.material.emissive={0,0,12.f*.2126f/.0722f};
        } else if(mode==2)floor.material.roughness=.65f;
        else options.reflection_roughness=.1f;
        f.edited();
        std::array<render::Capture,times.size()> samples;
        f.step(&samples[0]);
        for(unsigned step=1;step<=16;++step) {
            const auto found=std::ranges::find(times,step);
            f.step(found==times.end()?nullptr:&samples[static_cast<std::size_t>(found-times.begin())]);
        }
        // The endpoint reference is independently initialized. Letting the
        // edited history itself settle would let a persistent error self-pass.
        f.frame.camera_cut=true;
        const auto reference=f.capture((name+"-clean-endpoint").c_str(),128);
        const auto reference_later=f.capture((name+"-clean-endpoint-later").c_str(),16);
        check(before.depth==reference.depth && reference.depth==reference_later.depth,
              "signal edits preserve exact FP32 geometry depth");
        double signal=0,noise=0;std::size_t changed=0;
        std::array<double,times.size()> errors{};
        for(const auto i:receivers) {
            unsigned delta=0,static_delta=0;
            for(unsigned c=0;c<3;++c) {
                const auto k=i*4+c;
                delta=std::max(delta,unsigned(std::abs(int(before.rgba[k])-int(reference.rgba[k]))));
                static_delta=std::max(static_delta,unsigned(std::abs(int(reference_later.rgba[k])-int(reference.rgba[k]))));
            }
            signal+=delta;noise+=static_delta;changed+=delta>=8;
            for(std::size_t sample=0;sample<samples.size();++sample) {
                unsigned error=0;
                for(unsigned c=0;c<3;++c) {
                    const auto k=i*4+c;
                    error=std::max(error,unsigned(std::abs(int(samples[sample].rgba[k])-int(reference.rgba[k]))));
                }
                errors[sample]+=error;
            }
        }
        check(changed>=64 && signal>0,"signal edit visibly changes the reflection receiver, not only the emitter itself");
        // A noise-dominated scene cannot establish a prompt response. This
        // bound keeps the measured-noise allowance below a 50% stale tail.
        passed&=noise<=signal*.1;
        passed&=errors[4]<=std::max(signal*.25,noise*3) && errors[5]<=std::max(signal*.15,noise*3);
        std::array<unsigned,times.size()> off_errors{};
        if(mode>=2) {
            options.sssr=false;
            const auto disabled=f.capture((name+"-endpoint-sssr-off").c_str(),2);
            check(disabled.depth==reference.depth,"SSR eligibility changes preserve exact depth against the disabled reference");
            for(std::size_t sample=0;sample<samples.size();++sample) {
                // Eligibility is immediate; no sampled frame may retain an
                // ineligible signal, even where the original footprint was dim.
                for(std::size_t i=0;i<disabled.depth.size();++i)
                    for(unsigned c=0;c<3;++c)off_errors[sample]=std::max(off_errors[sample],
                        unsigned(std::abs(int(samples[sample].rgba[i*4+c])-int(disabled.rgba[i*4+c]))));
                passed&=off_errors[sample]<=1;
            }
        }
        for(std::size_t sample=0;sample<samples.size();++sample) {
            check(samples[sample].depth==reference.depth,"material and reflection-setting edits leave every sampled FP32 depth unchanged");
            png(samples[sample],f.output/(name+"-after-"+std::to_string(times[sample])+".png"));
            evidence<<name<<','<<times[sample]<<','<<errors[sample]/signal<<','
                    <<errors[sample]/double(receivers.size())<<','<<noise/signal<<','<<changed<<','<<off_errors[sample]<<'\n';
        }
        evidence.flush();
        std::printf("SceneLit %s: relative error 0/8/16 %.4f / %.4f / %.4f, static noise %.4f, changed=%zu, cutoff max RGB8 error 0/16=%u/%u\n",
            names[mode],errors[0]/signal,errors[4]/signal,errors[5]/signal,noise/signal,changed,off_errors[0],off_errors[5]);
    }
    f.room.meshes=meshes;f.lights=lights;f.frame.view=view;f.frame.camera=camera;f.frame.environment=environment;
    f.frame.camera_cut=true;f.edited();
    check(evidence.good(),"signal edit accepted-frame trajectories and independent endpoint evidence written");
    check(passed,"material radiance and reflection eligibility edits remove stale SSR promptly without chroma trails");
    std::puts("Renderer SceneLit signal edits: PASS");
}

// Direct radiance changes have a bounded response. A light and material edit
// that is restored returns to its baseline; continuously animated radiance
// keeps a filtered penumbra instead of a single-frame estimate. Only direct
// light is enabled, so DDGI's own response cannot mask the DI history. Both
// paths share the resolve under test; the A path is used because ReSTIR's
// spatiotemporal reuse leaves low-frequency mottling above this residual.
void quality_direct_radiance_response(Fixture& f,Region penumbra) {
    const auto lights=f.lights;const auto meshes=f.room.meshes;const auto view=f.frame.view;
    auto& options=f.frame.view.lit;
    options.antialiasing=render::Antialiasing::none;
    options.ddgi=options.gtao=options.sssr=options.ssil=options.bloom=false;
    options.direct_reference=false;options.direct_clustered=true;
    // The edit is chromatic, so its stale residual shows as red minus blue,
    // where estimator noise of the mostly neutral lighting largely cancels.
    const auto red_shift=[](const render::Capture& a,const render::Capture& b) {
        double sum=0;std::size_t count=0;
        for(std::size_t i=0;i<a.depth.size();++i) {
            if(a.depth[i]==0 || std::abs(a.depth[i]-b.depth[i])>1e-5f)continue;
            sum+=(int(b.rgba[i*4])-int(a.rgba[i*4]))-(int(b.rgba[i*4+2])-int(a.rgba[i*4+2]));
            ++count;
        }
        return sum/double(count);
    };
    f.frame.camera_cut=true;f.edited();
    const auto baseline=f.average("radiance-baseline",warmup_frames,32);
    f.lights[3].color={1,.25f,.05f};f.lights[3].intensity=lights[3].intensity*4;
    f.room.meshes[4].color={.9f,.2f,.1f,1};f.edited();
    for(unsigned i=0;i<8;++i)f.step();
    f.lights=lights;f.room.meshes=meshes;f.edited();
    const auto early=red_shift(baseline,f.average("radiance-restored-16",16,16));
    const auto settled=red_shift(baseline,f.average("radiance-restored-64",48,16));
    std::printf("SceneLit radiance-restore red shift: frames 1-16=%+.3f frames 49-64=%+.3f\n",early,settled);
    check(early>=1,"a restored chromatic light and material edit reaches the direct history");
    check(std::abs(settled)<=early*.1,"restored direct radiance returns to its baseline within 64 frames");
    const auto roi_noise=[&](auto&& advance) {
        render::Capture previous,current;
        advance();f.step(&previous);
        double sum=0;std::size_t count=0;
        for(unsigned frame=0;frame<32;++frame) {
            advance();f.step(&current);
            for(unsigned y=penumbra.top;y<penumbra.bottom;++y) for(unsigned x=penumbra.left;x<penumbra.right;++x) {
                unsigned delta=0;
                for(unsigned c=0;c<3;++c)delta=std::max(delta,unsigned(std::abs(int(pixel(current,x,y)[c])-int(pixel(previous,x,y)[c]))));
                sum+=delta;++count;
            }
            std::swap(previous,current);
        }
        return sum/double(count);
    };
    for(unsigned i=0;i<warmup_frames;++i)f.step();
    const auto still=roi_noise([]{});
    unsigned phase=0;
    const auto animated=roi_noise([&] {
        // A 0.1% alternating intensity marks radiance changed every frame
        // without moving the converged signal.
        f.lights[3].intensity=lights[3].intensity*(++phase%2?1.001f:1.f);f.edited();
    });
    f.lights=lights;f.edited();
    const auto single=roi_noise([&]{f.frame.camera_cut=true;});
    std::printf("SceneLit radiance-animated penumbra noise: still=%.3f animated=%.3f single-frame=%.3f\n",still,animated,single);
    check(animated<=single*.25,"continuously animated direct radiance keeps a filtered penumbra");
    f.lights=lights;f.room.meshes=meshes;f.frame.view=view;f.frame.camera_cut=true;f.edited();
}
