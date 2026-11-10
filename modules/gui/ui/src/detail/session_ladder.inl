        if (self.press && self.press->ladder) {
            const auto& ladder=*self.press->ladder;
            const auto& geometry=ladder.geometry;
            const auto& appearance=*ladder.style;
            const auto bounds=geometry.bounds;
            const auto clip=intersect(target,clip_rect(bounds));
            const float stroke=std::max(1.f,std::round(appearance.border_width*scale));
            output->value_ladder=geometry;
            paint_box(bounds,clip,appearance.background.value_or(self.style.palette.field),appearance.image,{},
                stroke,appearance.border,appearance.border_style);
            const auto text_at=[&](const detail::RasterText& raster,float center_y,float height) {
                if (raster.coverage) quad({bounds.x+raster.left,center_y-height*scale*.5f+raster.top,
                    static_cast<float>(raster.coverage->width),static_cast<float>(raster.coverage->height)},
                    clip,appearance.foreground,raster.coverage);
            };
            for (int row=0;row<7;++row) {
                const auto y=bounds.y+geometry.row_height*static_cast<float>(row);
                const bool selected=row==geometry.selected_row;
                if (selected) quad({bounds.x+stroke,y+stroke,bounds.width-2*stroke,geometry.row_height-stroke},clip,appearance.selection);
                if (row) quad({bounds.x+stroke,y,bounds.width-2*stroke,stroke},clip,appearance.border);
                text_at(ladder.labels[row],y+geometry.row_height*(selected?.25f:.5f),ladder.text_height);
                if (selected) {
                    // Slot zero is reserved for transient text. Stable frames reuse its paragraph and bitmap.
                    self.text.set(0,L"*"+format(ladder.multiply?ladder.multiplier:(ladder.fine?.1:1)),appearance.font,false,TextAlign::center);
                    ATELIER_TRY_BIND(metrics,self.text.metrics(0,bounds.width/scale));
                    ATELIER_TRY_BIND(raster,self.text.raster(0,bounds.width/scale,scale));
                    text_at(*raster,y+geometry.row_height*.75f,metrics->height);
                }
            }
        }
