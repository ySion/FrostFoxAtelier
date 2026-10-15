        self.validate_tooltip(*output);
        if (self.tooltip_hover && std::chrono::steady_clock::now()-self.tooltip_hover->since>=std::chrono::milliseconds(500)) {
            const auto& hover=*self.tooltip_hover;
            const auto& help=*hover.content;
            const float margin=6*scale,padding=8*scale,gap=5*scale;
            const float maximum_width=std::min(520*scale,std::max(0.f,static_cast<float>(draws->extent.width)-2*(margin+padding)));
            const float maximum_height=std::max(0.f,static_cast<float>(draws->extent.height)-2*(margin+padding));
            if (maximum_width>0 && maximum_height>0) {
                const auto width=maximum_width/scale;
                auto& cached=self.tooltip_cache;
                if (!cached || cached->content!=hover.content || cached->style_revision!=self.style_revision ||
                    cached->width!=width || cached->height!=maximum_height || cached->scale!=scale) {
                    Impl::TooltipCache candidate;
                    candidate.content=hover.content;candidate.style_revision=self.style_revision;candidate.width=width;candidate.height=maximum_height;candidate.scale=scale;
                    auto title_font=self.style.font;title_font.weight=self.style.title_weight;
                    self.text.set(tooltip_title_key,help.title,title_font,true);
                    std::wstring body=help.identity;
                    if (!body.empty() && !help.description.empty()) body+=L'\n';
                    body+=help.description;
                    self.text.set(tooltip_body_key,body,self.style.font,true);
                    if (!help.title.empty()) candidate.title_size=self.text.metrics(tooltip_title_key,width);
                    if (!body.empty()) candidate.body_size=self.text.metrics(tooltip_body_key,width);
                    const Clip text_view{0,0,static_cast<std::int32_t>(std::ceil(maximum_width)),static_cast<std::int32_t>(std::ceil(maximum_height))};
                    if (!help.title.empty()) candidate.title=self.text.raster(tooltip_title_key,width,scale,text_view);
                    if (!body.empty()) candidate.body=self.text.raster(tooltip_body_key,width,scale,text_view);
                    cached=std::move(candidate);
                }
                const bool has_title=!help.title.empty(),has_body=!help.identity.empty() || !help.description.empty();
                const auto content_height=(cached->title_size.height+cached->body_size.height)*scale+(has_title && has_body?gap:0);
                const auto w=std::min(maximum_width,std::max(cached->title_size.width,cached->body_size.width)*scale)+2*padding;
                const auto h=std::min(maximum_height,content_height)+2*padding;
                float x=hover.x+14*scale,y=hover.y+20*scale;
                if (x+w>draws->extent.width-margin) x=hover.x-14*scale-w;
                if (y+h>draws->extent.height-margin) y=hover.y-14*scale-h;
                x=std::clamp(x,margin,std::max(margin,static_cast<float>(draws->extent.width)-margin-w));
                y=std::clamp(y,margin,std::max(margin,static_cast<float>(draws->extent.height)-margin-h));
                const Rect bounds{std::round(x),std::round(y),std::ceil(w),std::ceil(h)};
                const auto clip=intersect(target,clip_rect(bounds));
                paint_box(bounds,clip,self.style.palette.field,{},{},std::max(1.f,std::round(scale)),self.style.palette.border,BorderStyle::solid);
                const auto text_clip=intersect(clip,clip_rect({bounds.x+padding,bounds.y+padding,bounds.width-2*padding,bounds.height-2*padding}));
                const auto paint_text=[&](const detail::RasterText& raster,float top) {
                    if (raster.coverage) quad({bounds.x+padding+raster.left,top+raster.top,
                        static_cast<float>(raster.coverage->width),static_cast<float>(raster.coverage->height)},text_clip,self.style.palette.text,raster.coverage);
                };
                paint_text(cached->title,bounds.y+padding);
                paint_text(cached->body,bounds.y+padding+cached->title_size.height*scale+(has_title && has_body?gap:0));
                output->tooltip=TooltipGeometry{hover.owner,hover.incarnation,hover.anchor,bounds,hover.content,hover.generation};
            }
        }
