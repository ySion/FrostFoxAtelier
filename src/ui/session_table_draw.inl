                const auto& data=*state.control.table;
                const auto selection=state.control.table_selection;
                const auto padding=effective_padding(state.control,appearance);
                Rect inner{bounds.x+(padding.left+border/scale)*scale,bounds.y+(padding.top+border/scale)*scale,
                    std::max(0.f,bounds.width-(padding.left+padding.right)*scale-2*border),
                    std::max(0.f,bounds.height-(padding.top+padding.bottom)*scale-2*border)};
                const auto table_clip=intersect(clip,clip_rect(inner));
                const auto header_height=std::min(data.header_height*scale,inner.height);
                const auto row_height=data.row_height*scale;
                const auto gutter=(appearance.scrollbar_width+2*appearance.scrollbar_inset)*scale;
                std::vector<float> widths; widths.reserve(data.columns.size());
                float content_width{};
                for (const auto& column:data.columns) {
                    const auto width=self.press && self.press->key==key && self.press->table_resize && self.press->table_column==column.key?
                        static_cast<float>(self.press->value):column.width;
                    widths.push_back(width*scale); content_width+=width*scale;
                }
                const auto content_height=static_cast<double>(data.rows.size())*row_height;
                bool vertical=content_height>inner.height-header_height;
                const bool stable=appearance.scrollbar_gutter==ScrollbarGutter::stable;
                auto body_width=std::max(0.f,inner.width-((vertical || stable)?gutter:0.f));
                bool horizontal=content_width>body_width;
                auto body_height=std::max(0.f,inner.height-header_height-(horizontal?gutter:0.f));
                vertical=content_height>body_height;
                body_width=std::max(0.f,inner.width-((vertical || stable)?gutter:0.f));
                horizontal=content_width>body_width;
                body_height=std::max(0.f,inner.height-header_height-(horizontal?gutter:0.f));
                state.scroll_limit=std::max(0.0,static_cast<double>((content_height-body_height)/scale));
                state.scroll_x_limit=std::max(0.0,static_cast<double>((content_width-body_width)/scale));
                if (state.table_reveal && body_height>0 && body_width>0) {
                    if (const auto row=state.table_index.find(selection.row);row!=state.table_index.end()) {
                        const auto top=row->second*static_cast<double>(data.row_height);
                        if (top<state.scroll) state.scroll=top;
                        if (top+data.row_height>state.scroll+body_height/scale) state.scroll=top+data.row_height-body_height/scale;
                    }
                    float left{};
                    for (std::size_t column=0;column<data.columns.size();++column) {
                        if (data.columns[column].key==selection.column) {
                            if (widths[column]>=body_width || left/scale<state.scroll_x) state.scroll_x=left/scale;
                            else if ((left+widths[column])/scale>state.scroll_x+body_width/scale)
                                state.scroll_x=(left+widths[column]-body_width)/scale;
                            break;
                        }
                        left+=widths[column];
                    }
                    state.table_reveal=false;
                }
                state.scroll=std::clamp(state.scroll,0.0,state.scroll_limit);
                state.scroll_x=std::clamp(state.scroll_x,0.0,state.scroll_x_limit);
                TableGeometry table{key,state.control.table,selection,
                    {inner.x,inner.y,body_width,header_height},{inner.x,inner.y+header_height,body_width,body_height},table_clip};
                const auto header_clip=intersect(table_clip,clip_rect(table.header));
                const auto body_clip=intersect(table_clip,clip_rect(table.body));
                quad(table.header,header_clip,appearance.indicator_background);
                std::vector<detail::TextKey> visible_text;
                const auto paint_text=[&](TableKey id,bool header,std::wstring_view value,Rect cell,Clip cell_clip,
                    TextAlign alignment,ControlIcon icon,bool row_enabled,float trailing=0.f) {
                    float x=cell.x+std::max(4.f,appearance.padding.left)*scale;
                    auto width=std::max(0.f,cell.width-(std::max(4.f,appearance.padding.left)+std::max(4.f,appearance.padding.right)+trailing)*scale);
                    const auto color=row_enabled?appearance.foreground:self.style.palette.muted;
                    if (icon!=ControlIcon::none) {
                        const auto size=std::min({appearance.indicator_size*scale,cell.height-2*scale,width});
                        if (size>0) {
                            using Icon=ControlIcon;
                            if (icon==Icon::grip_horizontal || icon==Icon::grip_vertical) {
                                const auto dot=std::max(1.f,scale),pitch=size/4;
                                for (int i=1;i<=3;++i) quad({x+(icon==Icon::grip_horizontal?i*pitch:size*.5f),
                                    cell.y+cell.height*.5f+(icon==Icon::grip_vertical?(i-2)*pitch:0.f),dot,dot},cell_clip,color);
                            } else quad({x,cell.y+(cell.height-size)*.5f,size,size},cell_clip,
                                row_enabled?appearance.indicator_foreground.value_or(color):color,
                                self.text.symbol(icon_symbol(icon),static_cast<std::uint32_t>(std::ceil(size))));
                            x+=size+4*scale; width=std::max(0.f,width-size-4*scale);
                        }
                    }
                    if (value.empty() || width<=0 || !visible({x,cell.y,width,cell.height},cell_clip)) return;
                    const detail::TextKey text_key{key,id,header}; visible_text.push_back(text_key);
                    self.text.set(text_key,value,appearance.font,false,alignment);
                    const auto metrics=self.text.metrics(text_key,width/scale);
                    const auto y=cell.y+std::max(0.f,cell.height-metrics.height*scale)*.5f;
                    const auto text_clip=intersect(cell_clip,clip_rect({x,cell.y,width,cell.height}));
                    const Clip viewport{static_cast<int>(std::floor(text_clip.left-x)),static_cast<int>(std::floor(text_clip.top-y)),
                        static_cast<int>(std::ceil(text_clip.right-x)),static_cast<int>(std::ceil(text_clip.bottom-y))};
                    const auto raster=self.text.raster(text_key,width/scale,scale,viewport);
                    if (raster.coverage) quad({x+raster.left,y+raster.top,static_cast<float>(raster.coverage->width),
                        static_cast<float>(raster.coverage->height)},text_clip,color,raster.coverage);
                };
                float column_x=inner.x-static_cast<float>(state.scroll_x)*scale;
                for (std::size_t column=0;column<data.columns.size();++column) {
                    const auto& descriptor=data.columns[column];
                    const Rect header{column_x,inner.y,widths[column],header_height};
                    if (visible(header,header_clip)) {
                        const auto cell_clip=intersect(header_clip,clip_rect(header));
                        table.columns.push_back({descriptor.key,header,{header.x+header.width-3*scale,header.y,6*scale,header.height},header_clip});
                        paint_text(descriptor.key,true,descriptor.title,header,cell_clip,descriptor.alignment,ControlIcon::none,enabled,
                            data.sort.column==descriptor.key?12.f:0.f);
                        if (data.sort.column==descriptor.key) {
                            const auto size=std::min(10*scale,header_height);
                            quad({header.x+header.width-size-5*scale,header.y+(header.height-size)*.5f,size,size},cell_clip,appearance.foreground,
                                self.text.symbol(data.sort.direction==TableSortDirection::ascending?detail::TextStore::Symbol::up:detail::TextStore::Symbol::down,
                                    static_cast<std::uint32_t>(std::ceil(size))));
                        }
                    }
                    column_x+=widths[column];
                }
                const auto first=std::min(data.rows.size(),static_cast<std::size_t>(std::max(0.0,
                    std::floor((static_cast<double>(body_clip.top)-table.body.y+state.scroll*scale)/row_height))));
                for (auto row=first;row<data.rows.size();++row) {
                    const auto y=table.body.y+static_cast<float>(row*static_cast<double>(row_height)-state.scroll*scale);
                    if (y>=body_clip.bottom) break;
                    const auto& descriptor=data.rows[row];
                    if (row%2) quad({table.body.x,y,body_width,row_height},body_clip,appearance.tree_alternate);
                    if (descriptor.key==selection.row) quad({table.body.x,y,body_width,row_height},body_clip,appearance.selection);
                    float x=inner.x-static_cast<float>(state.scroll_x)*scale;
                    for (std::size_t column=0;column<data.columns.size();++column) {
                        const Rect cell{x,y,widths[column],row_height}; x+=widths[column];
                        if (!visible(cell,body_clip)) continue;
                        const auto& content=descriptor.cells[column];
                        const auto cell_clip=intersect(body_clip,clip_rect(cell));
                        table.cells.push_back({descriptor.key,data.columns[column].key,content.key,cell,cell_clip});
                        paint_text(content.key,false,content.text,cell,cell_clip,data.columns[column].alignment,content.icon,enabled && descriptor.enabled);
                        if (self.focus==key && selection.row==descriptor.key && selection.column==data.columns[column].key)
                            paint_box(cell,cell_clip,{0,0,0,0},{},{},std::max(1.f,scale),appearance.border,BorderStyle::solid);
                    }
                    quad({table.body.x,y+row_height-scale,body_width,scale},body_clip,appearance.tree_line);
                }
                column_x=inner.x-static_cast<float>(state.scroll_x)*scale;
                for (auto width:widths) {
                    column_x+=width;
                    quad({column_x-scale,inner.y,scale,header_height+body_height},intersect(table_clip,clip_rect({inner.x,inner.y,body_width,header_height+body_height})),appearance.tree_line);
                }
                quad({inner.x,inner.y+header_height-scale,body_width,scale},header_clip,appearance.tree_line);
                const auto paint_bar=[&](bool horizontal_bar,Rect bar_bounds,double maximum,double value,double page) {
                    if (maximum<=0 || bar_bounds.width<=0 || bar_bounds.height<=0) return;
                    ScrollbarGeometry bar;
                    bar.key=key; bar.bounds=bar_bounds; bar.clip=table_clip; bar.maximum=maximum;
                    bar.value=value; bar.page=page; bar.horizontal=horizontal_bar;
                    paint_scrollbar(bar,appearance,enabled);
                };
                const auto inset=appearance.scrollbar_inset*scale,bar_width=appearance.scrollbar_width*scale;
                if (vertical) paint_bar(false,{inner.x+body_width+inset,table.body.y,bar_width,body_height},state.scroll_limit,state.scroll,body_height/scale);
                if (horizontal) paint_bar(true,{inner.x,table.body.y+body_height+inset,body_width,bar_width},state.scroll_x_limit,state.scroll_x,body_width/scale);
                std::sort(visible_text.begin(),visible_text.end());
                for (auto previous:state.table_text) if (!std::binary_search(visible_text.begin(),visible_text.end(),previous)) self.text.erase(previous);
                state.table_text=std::move(visible_text);
                output->tables.push_back(std::move(table));
