// Real accepted-frame trajectories. The final reference starts with clean image
// histories, so a stale signal cannot validate itself merely by settling slowly.
void quality_motion_history(Fixture& f) {
    const auto meshes=f.room.meshes;
    const auto lights=f.lights;
    const auto view=f.frame.view;
    const auto camera=f.frame.camera;
    const auto environment=f.frame.environment;
    bool passed=true;
    std::ofstream evidence(f.output/"motion-history.csv");
    evidence<<"case,frames_after_motion,relative_error,mean_rgb8_error,static_noise_relative,changed_pixels\n";
    constexpr const char* names[]{"moving-area-shadow","moving-red-reflection","moving-gray-reflection","moving-local-gi"};
    for(unsigned mode=0;mode<4;++mode) {
        f.frame.view=view;f.frame.camera=camera;f.frame.environment={};f.lights.clear();
        auto& options=f.frame.view.lit;
        options.antialiasing=render::Antialiasing::smaa;
        options.ddgi=options.gtao=options.sssr=options.ssil=options.bloom=false;
        options.shadows=true;options.direct_reference=false;
        f.room.meshes={meshes[0],meshes[4]};
        auto& floor=f.room.meshes[0];auto& mover=f.room.meshes[1];
        floor.color={.7f,.7f,.7f,1};floor.material={.roughness=1};
        mover.color={.6f,.6f,.6f,1};mover.material={.roughness=1};
        mover.world=box(-1.4f,1.1f,3,1.2f,2.2f,.6f);
        if(mode==0) {
            f.lights.assign(1,{});auto& light=f.lights[0];
            light.stable_key=100;light.kind=render::LightKind::rectangle;
            light.position={0,4,6};light.direction=normalize({0,-4,-5});
            light.right={1,0,0};light.up=normalize(cross(light.direction,light.right));
            light.width=3;light.height=2;light.intensity=12;
        } else if(mode<=2) {
            options.sssr=true;floor.material={.roughness=.18f,.metallic=1};
            mover.world=box(-1.4f,2.1f,6.2f,1.6f,1.8f,.1f);
            mover.color={0,0,0,1};
            mover.material.emissive=mode==1?Vec3{12,0,0}:Vec3{2.5512f,2.5512f,2.5512f};
        } else {
            options.ssil=true;options.exposure_ev=2;
            mover.world=box(-1.4f,.7f,3,1,1.4f,.1f);mover.color={.8f,.03f,.01f,1};
            f.lights.assign(1,{});auto& light=f.lights[0];
            light.stable_key=100;light.kind=render::LightKind::directional;
            light.direction={0,0,1};light.intensity=8;
        }
        ++f.frame.scene_identity;f.frame.camera_cut=true;f.edited();
        const std::string name=names[mode];
        const auto before=f.capture((name+"-before").c_str(),128);
        constexpr std::array<unsigned,6> times{0,1,2,4,8,16};
        std::array<render::Capture,times.size()> samples;
        for(unsigned step=1;step<=16;++step) {
            mover.world[12]=-1.4f+2.8f*float(step)/16;f.edited();
            const auto result=f.step(step==16?&samples[0]:nullptr);
            check(result.stats.scene_lit_history_resets==0,"moving objects preserve the scene domain during history response");
        }
        for(unsigned step=1;step<=16;++step) {
            auto found=std::ranges::find(times,step);
            f.step(found==times.end()?nullptr:&samples[static_cast<std::size_t>(found-times.begin())]);
        }
        f.frame.camera_cut=true;
        const auto reference=f.capture((name+"-reference").c_str(),128);
        const auto reference_later=f.capture((name+"-reference-later").c_str(),16);
        double signal=0,noise=0;std::size_t pixels=0,changed=0;unsigned cross_channel=0;
        std::array<double,times.size()> errors{};
        for(std::size_t i=0;i<reference.depth.size();++i) {
            // Both initial and endpoint observations must see the stationary
            // receiver. Include the entire travel path, not only its endpoints.
            if(before.depth[i]<=0 || std::abs(before.depth[i]-reference.depth[i])>1e-8f)continue;
            unsigned delta=0,static_delta=0;
            for(unsigned c=0;c<3;++c) {
                const auto k=i*4+c;
                delta=std::max(delta,unsigned(std::abs(int(before.rgba[k])-int(reference.rgba[k]))));
                static_delta=std::max(static_delta,unsigned(std::abs(int(reference_later.rgba[k])-int(reference.rgba[k]))));
            }
            signal+=delta;noise+=static_delta;changed+=delta>=8;++pixels;
            for(std::size_t sample=0;sample<samples.size();++sample) {
                unsigned error=0;
                for(unsigned c=0;c<3;++c) {
                    const auto k=i*4+c;
                    error=std::max(error,unsigned(std::abs(int(samples[sample].rgba[k])-int(reference.rgba[k]))));
                }
                errors[sample]+=error;
                if(mode==1)cross_channel=std::max({cross_channel,unsigned(samples[sample].rgba[i*4+1]),
                                                                  unsigned(samples[sample].rgba[i*4+2])});
            }
        }
        check(changed>=64 && signal>0,"motion history fixture produces an observable lighting change on stationary receivers");
        for(std::size_t sample=0;sample<samples.size();++sample) {
            png(samples[sample],f.output/(name+"-after-"+std::to_string(times[sample])+".png"));
            evidence<<name<<','<<times[sample]<<','<<errors[sample]/signal<<','<<errors[sample]/double(pixels)
                    <<','<<noise/signal<<','<<changed<<'\n';
        }
        evidence.flush();
        std::printf("SceneLit %s: relative error at 0/8/16 frames %.4f / %.4f / %.4f, static noise %.4f, changed=%zu\n",
            names[mode],errors[0]/signal,errors[4]/signal,errors[5]/signal,noise/signal,changed);
        // Permit measured stochastic variation, but do not accept a long trail
        // merely because the final post-AA image is stable after a long wait.
        passed&=errors[4]<=std::max(signal*.25,noise*3) && errors[5]<=std::max(signal*.15,noise*3);
        if(mode==1) {
            std::printf("SceneLit red-only reflection: maximum unlit green/blue channel=%u\n",cross_channel);
            passed&=cross_channel<=1;
        }
        if(mode<=1) {
            // Negative control: geometry beyond every visible light/ray moves
            // continuously. A global response hint must not erase a stationary
            // stochastic signal or turn a neighboring entity into a noise switch.
            auto distant=meshes[4];distant.stable_key=99999;
            distant.world=box(50,1,3,1,1,1);distant.material={.roughness=1};
            f.room.meshes.push_back(distant);f.edited();
            std::array<double,4> brightness{},variation{};
            for(unsigned variant=0;variant<(mode==1?4u:2u);++variant) {
                const bool moving=variant%2!=0, t2x=variant>=2;
                options.antialiasing=t2x?render::Antialiasing::smaa_t2x:render::Antialiasing::smaa;
                const unsigned last=mode==1?256u:80u;
                f.room.meshes.back().world[12]=50;f.frame.camera_cut=true;f.edited();
                render::Capture previous;std::size_t observations=0,differences=0;
                for(unsigned step=1;step<=last;++step) {
                    if(moving){f.room.meshes.back().world[12]=50+float(step)*.01f;f.edited();}
                    render::Capture image;
                    f.step(step>=last-16?&image:nullptr);
                    if(step<last-16)continue;
                    for(std::size_t i=0;i<image.depth.size();++i) {
                        if(before.depth[i]<=0 || std::abs(before.depth[i]-reference.depth[i])>1e-8f ||
                            reference.rgba[i*4]<8)continue;
                        brightness[variant]+=image.rgba[i*4];++observations;
                        if(!previous.rgba.empty()) {
                            unsigned delta=0;
                            for(unsigned c=0;c<3;++c)delta=std::max(delta,
                                unsigned(std::abs(int(image.rgba[i*4+c])-int(previous.rgba[i*4+c]))));
                            variation[variant]+=delta;++differences;
                        }
                    }
                    previous=std::move(image);
                }
                check(observations>0 && differences>0,"unrelated-motion control observes the stationary receiver");
                brightness[variant]/=double(observations);variation[variant]/=double(differences);
                png(previous,f.output/(name+(t2x?"-t2x":"")+(moving?"-unrelated-moving.png":"-unrelated-static.png")));
                if(variant==2) {
                    // Track the same AA policy and phase over another 256 frames.
                    // Cross-policy brightness is not a source-anchor drift test.
                    const auto late=f.capture("moving-red-reflection-t2x-persistent-late",256);
                    double late_mean=0;std::size_t late_pixels=0;
                    for(std::size_t i=0;i<late.depth.size();++i) {
                        if(before.depth[i]<=0 || std::abs(before.depth[i]-reference.depth[i])>1e-8f || reference.rgba[i*4]<8)continue;
                        late_mean+=late.rgba[i*4];++late_pixels;
                    }
                    check(late_pixels>0,"persistent source check observes reflective receivers");
                    late_mean/=double(late_pixels);
                    const double drift=std::abs(late_mean-brightness[variant])/std::max(brightness[variant],1.0);
                    std::printf("SceneLit T2x source-anchor drift at 256/512 frames: mean %.4f / %.4f, change %.4f\n",
                        brightness[variant],late_mean,drift);
                    evidence<<name<<"-t2x-drift,512,"<<drift<<','<<late_mean<<','<<brightness[variant]<<','<<changed<<'\n';
                    passed&=drift<=.03;
                }
            }
            const double brightness_error=std::abs(brightness[1]-brightness[0])/std::max(brightness[0],1.0);
            std::printf("SceneLit %s unrelated motion: mean %.4f / %.4f, change %.4f, temporal variation %.4f / %.4f\n",
                names[mode],brightness[0],brightness[1],brightness_error,variation[0],variation[1]);
            evidence<<name<<"-unrelated,"<<(mode==1?256:80)<<','<<brightness_error<<','<<variation[1]<<','<<variation[0]<<','<<changed<<'\n';
            passed&=brightness_error<=.08 && variation[1]<=std::max(1.0,variation[0]*2);
            if(mode==1) {
                const double t2x_brightness_error=std::abs(brightness[3]-brightness[2])/std::max(brightness[2],1.0);
                std::printf("SceneLit T2x persistent reflection source: mean %.4f / %.4f, change %.4f, temporal variation %.4f / %.4f\n",
                    brightness[2],brightness[3],t2x_brightness_error,variation[2],variation[3]);
                evidence<<name<<"-t2x-unrelated,256,"<<t2x_brightness_error<<','<<variation[3]<<','<<variation[2]<<','<<changed<<'\n';
                passed&=t2x_brightness_error<=.08 && variation[3]<=std::max(1.0,variation[2]*2);
            }
        }
    }
    f.room.meshes=meshes;f.lights=lights;f.frame.view=view;f.frame.camera=camera;f.frame.environment=environment;
    f.frame.camera_cut=true;f.edited();
    check(evidence.good(),"motion history evidence written");
    check(passed,"moving shadow/reflection/local-GI histories converge promptly without long colored trails");
    std::puts("Renderer SceneLit moving history: PASS");
}
