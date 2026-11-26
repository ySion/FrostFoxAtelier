        self.validate_tooltip(*output);
        if (self.tooltip_hover && std::chrono::steady_clock::now()-self.tooltip_hover->since>=std::chrono::milliseconds(180)) {
            const auto& hover=*self.tooltip_hover;
            const auto& help=*hover.content;
            const float margin=6*scale,padding=8*scale,gap=4*scale,badge_pad_x=5*scale,badge_pad_y=2*scale;
            const float maximum_width=std::min(500*scale,std::max(0.f,static_cast<float>(draws->extent.width)-2*(margin+padding)));
            const float maximum_height=std::max(0.f,static_cast<float>(draws->extent.height)-2*(margin+padding));
            if (maximum_width>0 && maximum_height>0) {
                const auto width=maximum_width/scale;
                const auto badge_width=std::max(8.f,(maximum_width-2*badge_pad_x)/scale);
                auto& cached=self.tooltip_cache;
                if (!cached || cached->content!=hover.content || cached->style_revision!=self.style_revision ||
                    cached->width!=width || cached->height!=maximum_height || cached->scale!=scale) {
                    Impl::TooltipCache candidate;
                    candidate.content=hover.content;candidate.style_revision=self.style_revision;candidate.width=width;candidate.height=maximum_height;candidate.scale=scale;
                    auto title_font=self.style.font;title_font.weight=self.style.title_weight;
                    auto id_font=self.style.font;id_font.size=std::max(9.f,self.style.font.size*.90f);
                    const String title_u8 = wide_to_utf8(help.title);
                    const String id_u8 = wide_to_utf8(help.identity);
                    const String body_u8 = wide_to_utf8(help.description);
                    self.text.set(tooltip_title_key,title_u8.view(),title_font,true);
                    self.text.set(tooltip_identity_key,id_u8.view(),id_font,true);
                    self.text.set(tooltip_body_key,body_u8.view(),self.style.font,true);
                    if (!help.title.empty()) {ATELIER_TRY_BIND(metrics,self.text.metrics(tooltip_title_key,width));candidate.title_size=*metrics;}
                    if (!help.identity.empty()) {ATELIER_TRY_BIND(metrics,self.text.metrics(tooltip_identity_key,badge_width));candidate.identity_size=*metrics;}
                    if (!help.description.empty()) {ATELIER_TRY_BIND(metrics,self.text.metrics(tooltip_body_key,width));candidate.body_size=*metrics;}
                    const Clip text_view{0,0,static_cast<std::int32_t>(std::ceil(maximum_width)),static_cast<std::int32_t>(std::ceil(maximum_height))};
                    if (!help.title.empty()) {ATELIER_TRY_BIND(raster,self.text.raster(tooltip_title_key,width,scale,text_view));candidate.title=*raster;}
                    if (!help.identity.empty()) {ATELIER_TRY_BIND(raster,self.text.raster(tooltip_identity_key,badge_width,scale,text_view));candidate.identity=*raster;}
                    if (!help.description.empty()) {ATELIER_TRY_BIND(raster,self.text.raster(tooltip_body_key,width,scale,text_view));candidate.body=*raster;}
                    cached=std::move(candidate);
                }
                const bool has_title=!help.title.empty(),has_id=!help.identity.empty(),has_body=!help.description.empty();
                const float id_box_w=has_id?cached->identity_size.width*scale+2*badge_pad_x:0.f;
                const float id_box_h=has_id?cached->identity_size.height*scale+2*badge_pad_y:0.f;
                float content_height=0.f;
                if (has_title) content_height+=cached->title_size.height*scale;
                if (has_id) content_height+=(content_height>0?gap:0)+id_box_h;
                if (has_body) content_height+=(content_height>0?gap+scale:0)+cached->body_size.height*scale;
                const auto content_width=std::max({cached->title_size.width*scale,id_box_w,cached->body_size.width*scale});
                const auto w=std::min(maximum_width,content_width)+2*padding;
                const auto h=std::min(maximum_height,content_height)+2*padding;
                float x=hover.x+14*scale,y=hover.y+20*scale;
                if (x+w>draws->extent.width-margin) x=hover.x-14*scale-w;
                if (y+h>draws->extent.height-margin) y=hover.y-14*scale-h;
                x=std::clamp(x,margin,std::max(margin,static_cast<float>(draws->extent.width)-margin-w));
                y=std::clamp(y,margin,std::max(margin,static_cast<float>(draws->extent.height)-margin-h));
                const Rect bounds{std::round(x),std::round(y),std::ceil(w),std::ceil(h)};
                const auto clip=intersect(target,clip_rect(bounds));
                const float radius=2.5f*scale;
                paint_box(bounds,clip,Color{.024f,.027f,.032f,.98f},{},{radius,radius,radius,radius},std::max(1.f,std::round(scale*.667f)),
                    Color{.16f,.19f,.22f,1.f},BorderStyle::solid);
                const auto text_clip=intersect(clip,clip_rect({bounds.x+padding,bounds.y+padding,bounds.width-2*padding,bounds.height-2*padding}));
                const auto paint_text=[&](const detail::RasterText& raster,float left,float top,Color color) {
                    if (raster.coverage) quad({left+raster.left,top+raster.top,
                        static_cast<float>(raster.coverage->width),static_cast<float>(raster.coverage->height)},text_clip,color,raster.coverage);
                };
                float cursor_y=bounds.y+padding;
                if (has_title) {
                    paint_text(cached->title,bounds.x+padding,cursor_y,Color{.92f,.94f,.97f,1.f});
                    cursor_y+=cached->title_size.height*scale;
                }
                if (has_id) {
                    if (has_title) cursor_y+=gap;
                    const Rect pill{bounds.x+padding,cursor_y,std::min(bounds.width-2*padding,id_box_w),id_box_h};
                    const float pr=2.f*scale;
                    paint_box(pill,text_clip,Color{.011f,.015f,.021f,1.f},{},{pr,pr,pr,pr},std::max(1.f,std::round(scale*.667f)),
                        Color{.07f,.11f,.16f,1.f},BorderStyle::solid);
                    paint_text(cached->identity,pill.x+badge_pad_x,cursor_y+badge_pad_y,Color{.46f,.68f,.88f,1.f});
                    cursor_y+=id_box_h;
                }
                if (has_body) {
                    if (has_title || has_id) cursor_y+=gap+scale;
                    paint_text(cached->body,bounds.x+padding,cursor_y,Color{.68f,.72f,.76f,1.f});
                }
                output->tooltip=TooltipGeometry{hover.owner,hover.incarnation,hover.anchor,bounds,hover.content,hover.generation};
            }
        }
