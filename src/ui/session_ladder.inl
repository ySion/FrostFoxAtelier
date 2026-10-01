        if (self.press && self.press->button==PointerButton::middle) {
            const auto& press=*self.press;
            const auto& state=self.states.at(press.key);
            const auto& appearance=*state.resolved;
            const float band=press.ladder_band*scale, width=78*scale, height=band*7;
            const float x=std::clamp(press.x+12*scale,0.f,std::max(0.f,static_cast<float>(self.surface.extent.width)-width));
            const float y=std::clamp(press.y-band*3.5f,0.f,std::max(0.f,static_cast<float>(self.surface.extent.height)-height));
            quad({x,y,width,height},target,appearance.background.value_or(self.style.palette.panel));
            std::wstring labels;
            for (int row=0;row<7;++row) {
                if (row==3-press.ladder_level) quad({x,y+band*static_cast<float>(row),width,band},target,appearance.selection);
                if (row) labels+=L'\n';
                labels+=format((press.ladder_fine?state.control.fine_step:state.control.drag_step)*std::pow(10.0,3-row));
            }
            // Zero is the reserved layout root. A single cached paragraph keeps
            // this decoration on the ordinary DirectWrite/coverage path.
            self.text.set(0,labels,appearance.font,false,TextAlign::right);
            const auto raster=self.text.raster(0,70,scale);
            if (raster.coverage) quad({x+4*scale+raster.left,y+raster.top,
                static_cast<float>(raster.coverage->width),static_cast<float>(raster.coverage->height)},target,
                appearance.foreground,raster.coverage);
        }
