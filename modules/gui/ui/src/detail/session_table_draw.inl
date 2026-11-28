                const auto& data=*state.control.table;
                const auto& layout=state.control.table_layout;
                const bool details=layout.view==TableView::details,tiles=layout.view==TableView::tiles;
                const auto selection=state.control.table_selection;
                const auto padding=effective_padding(state.control,appearance);
                Rect inner{bounds.x+(padding.left+border/scale)*scale,bounds.y+(padding.top+border/scale)*scale,
                    std::max(0.f,bounds.width-(padding.left+padding.right)*scale-2*border),
                    std::max(0.f,bounds.height-(padding.top+padding.bottom)*scale-2*border)};
                const auto table_clip=intersect(clip,clip_rect(inner));
                const auto header_height=details?std::min(layout.header_height*scale,inner.height):0.f;
                const auto row_height=(tiles?layout.tile_height:layout.row_height)*scale;
                const auto gap=details?0.f:layout.item_gap*scale;
                const auto stride=row_height+gap;
                const auto gutter=(appearance.scrollbar_width+2*appearance.scrollbar_inset)*scale;
                Array<float> widths; widths.reserve(data.columns.size());
                float content_width{};
                if (details) for (const auto& column:data.columns) {
                    const auto width=self.press && self.press->key==key && self.press->table_resize && self.press->table_column==column.key?
                        static_cast<float>(self.press->value):column.width;
                    widths.add(width*scale); content_width+=width*scale;
                }
                const bool stable=appearance.scrollbar_gutter==ScrollbarGutter::stable;
                auto body_width=std::max(0.f,inner.width-(stable?gutter:0.f));
                auto body_height=std::max(0.f,inner.height-header_height);
                std::size_t columns_per_row=1;
                double content_height{};
                const auto measure_rows=[&] {
                    columns_per_row=tiles?std::max(std::size_t{1},static_cast<std::size_t>((body_width+gap)/(layout.tile_width*scale+gap))):1;
                    const auto rows=(data.rows.size()+columns_per_row-1)/columns_per_row;
                    content_height=rows?static_cast<double>(rows)*stride-gap:0;
                };
                measure_rows();
                bool vertical=content_height>body_height;
                bool horizontal=details && content_width>body_width;
                if (vertical && !stable) body_width=std::max(0.f,inner.width-gutter);
                horizontal=details && content_width>body_width;
                body_height=std::max(0.f,inner.height-header_height-(horizontal?gutter:0.f));
                measure_rows();
                vertical=content_height>body_height;
                body_width=std::max(0.f,inner.width-((vertical || stable)?gutter:0.f));
                measure_rows();
                if (!details) content_width=body_width;
                state.scroll_limit=std::max(0.0,static_cast<double>((content_height-body_height)/scale));
                state.scroll_x_limit=std::max(0.0,static_cast<double>((content_width-body_width)/scale));
                if (const auto* previous=self.published?self.published->find_table(key):nullptr;
                    previous && previous->columns_per_row!=columns_per_row) state.table_reveal=true;
                if (state.table_reveal && body_height>0 && body_width>0) {
                    if (const auto* row=state.table_index.find(selection.row)) {
                        const auto top=(*row/columns_per_row)*static_cast<double>(stride/scale);
                        if (top<state.scroll) state.scroll=top;
                        if (top+row_height/scale>state.scroll+body_height/scale) state.scroll=top+(row_height-body_height)/scale;
                    }
                    float left{};
                    if (details) for (std::size_t column=0;column<data.columns.size();++column) {
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
                table.columns_per_row=columns_per_row;table.row_stride=stride;
                const auto header_clip=intersect(table_clip,clip_rect(table.header));
                const auto body_clip=intersect(table_clip,clip_rect(table.body));
                if (paint) quad(table.header,header_clip,appearance.indicator_background);
                Array<detail::TextKey> visible_text;
                InlineArray<char, 256> cell_u8_scratch;
                const auto paint_text=[&](TableKey id,bool header,const std::wstring& wide_value,Rect cell,Clip cell_clip,
                    TextAlign alignment,ControlIcon icon,bool row_enabled,float trailing=0.f,bool above=false) -> Result<void> {
                    float x=cell.x+std::max(4.f,appearance.padding.left)*scale;
                    auto width=std::max(0.f,cell.width-(std::max(4.f,appearance.padding.left)+std::max(4.f,appearance.padding.right)+trailing)*scale);
                    const auto color=row_enabled?appearance.foreground:self.style.palette.muted;
                    if (icon!=ControlIcon::none) {
                        const auto size=std::min(appearance.indicator_size*scale,std::min(cell.height-2*scale,width));
                        if (size>0) {
                            if (paint) {
                                using Icon=ControlIcon;
                                if (icon==Icon::grip_horizontal || icon==Icon::grip_vertical) {
                                    const auto dot=std::max(1.f,scale),pitch=size/4;
                                    for (int i=1;i<=3;++i) quad({x+(icon==Icon::grip_horizontal?i*pitch:size*.5f),
                                        cell.y+cell.height*.5f+(icon==Icon::grip_vertical?(i-2)*pitch:0.f),dot,dot},cell_clip,color);
                                } else quad({above?cell.x+(cell.width-size)*.5f:x,above?cell.y+4*scale:cell.y+(cell.height-size)*.5f,size,size},cell_clip,
                                    row_enabled?appearance.indicator_foreground.value_or(color):color,
                                    self.text.symbol(icon_symbol(icon),static_cast<std::uint32_t>(std::ceil(size))));
                            }
                            if (above) {const auto used=std::min(cell.height,size+8*scale);cell.y+=used;cell.height-=used;}
                            else {x+=size+4*scale; width=std::max(0.f,width-size-4*scale);}
                        }
                    }
                    if (wide_value.empty() || width<=0 || !visible({x,cell.y,width,cell.height},cell_clip)) return {};
                    const StringView u8_value = wide_to_utf8_scratch(wide_value, cell_u8_scratch);
                    const detail::TextKey text_key{key,id,header}; visible_text.add(text_key);
                    self.text.set(text_key,u8_value,appearance.font,false,alignment,true);
                    ATELIER_TRY_BIND(metrics,self.text.metrics(text_key,width/scale));
                    const auto y=cell.y+std::max(0.f,cell.height-metrics->height*scale)*.5f;
                    if (!paint) return {};
                    const auto text_clip=intersect(cell_clip,clip_rect({x,cell.y,width,cell.height}));
                    const Clip viewport{static_cast<int>(std::floor(text_clip.left-x)),static_cast<int>(std::floor(text_clip.top-y)),
                        static_cast<int>(std::ceil(text_clip.right-x)),static_cast<int>(std::ceil(text_clip.bottom-y))};
                    ATELIER_TRY_BIND(raster,self.text.raster(text_key,width/scale,scale,viewport));
                    if (raster->coverage) quad({x+raster->left,y+raster->top,static_cast<float>(raster->coverage->width),
                        static_cast<float>(raster->coverage->height)},text_clip,color,raster->coverage);
                    return {};
                };
                float column_x=inner.x-static_cast<float>(state.scroll_x)*scale;
                if (details) for (std::size_t column=0;column<data.columns.size();++column) {
                    const auto& descriptor=data.columns[column];
                    const Rect header{column_x,inner.y,widths[column],header_height};
                    if (visible(header,header_clip)) {
                        const auto cell_clip=intersect(header_clip,clip_rect(header));
                        const Rect resize_zone{header.x+header.width-3*scale,header.y,6*scale,header.height};
                        table.columns.add({descriptor.key,header,resize_zone,header_clip});
                        if (paint && enabled && self.hover==key && descriptor.sortable &&
                            contains(header,self.hover_x,self.hover_y) && !contains(resize_zone,self.hover_x,self.hover_y) &&
                            !(column>0 && self.hover_x<header.x+3*scale))
                            quad(header,cell_clip,Color{1.f,1.f,1.f,.055f});
                        ATELIER_TRY(paint_text(descriptor.key,true,descriptor.title,header,cell_clip,descriptor.alignment,ControlIcon::none,enabled,
                            data.sort.column==descriptor.key?12.f:0.f));
                        if (paint && data.sort.column==descriptor.key) {
                            const auto size=std::min(9*scale,header_height);
                            quad({header.x+header.width-size-5*scale,header.y+(header.height-size)*.5f,size,size},cell_clip,appearance.foreground,
                                self.text.symbol(data.sort.direction==TableSortDirection::ascending?detail::TextStore::Symbol::up:detail::TextStore::Symbol::down,
                                    static_cast<std::uint32_t>(std::ceil(size))));
                        }
                    }
                    column_x+=widths[column];
                }
                const auto first=std::min(data.rows.size(),static_cast<std::size_t>(std::max(0.0,
                    std::floor((static_cast<double>(body_clip.top)-table.body.y+state.scroll*scale)/stride)))*columns_per_row);
                for (auto row=first;row<data.rows.size();++row) {
                    const auto y=table.body.y+static_cast<float>((row/columns_per_row)*static_cast<double>(stride)-state.scroll*scale);
                    if (y>=body_clip.bottom) break;
                    const auto& descriptor=data.rows[row];
                    if (!details) {
                        const auto width=tiles?std::min(layout.tile_width*scale,body_width):body_width;
                        const Rect cell{table.body.x+static_cast<float>(row%columns_per_row)*(layout.tile_width*scale+gap),y,width,row_height};
                        if (!visible(cell,body_clip)) continue;
                        const auto cell_clip=intersect(body_clip,clip_rect(cell));
                        const auto& content=descriptor.cells.front();
                        table.cells.add({descriptor.key,data.columns.front().key,content.key,cell,cell_clip});
                        if (paint && descriptor.key==selection.row) quad(cell,cell_clip,appearance.selection);
                        ATELIER_TRY(paint_text(content.key,false,content.text,cell,cell_clip,tiles?TextAlign::center:TextAlign::left,content.icon,
                            enabled && descriptor.enabled,0,tiles));
                        if (paint && self.focus==key && descriptor.key==selection.row)
                            paint_box(cell,cell_clip,{0,0,0,0},{},{},std::max(1.f,scale),appearance.border,BorderStyle::solid);
                        continue;
                    }
                    if (paint && row%2) quad({table.body.x,y,body_width,row_height},body_clip,appearance.tree_alternate);
                    if (paint && descriptor.key==selection.row) quad({table.body.x,y,body_width,row_height},body_clip,appearance.selection);
                    float x=inner.x-static_cast<float>(state.scroll_x)*scale;
                    for (std::size_t column=0;column<data.columns.size();++column) {
                        const Rect cell{x,y,widths[column],row_height}; x+=widths[column];
                        if (!visible(cell,body_clip)) continue;
                        const auto& content=descriptor.cells[column];
                        const auto cell_clip=intersect(body_clip,clip_rect(cell));
                        table.cells.add({descriptor.key,data.columns[column].key,content.key,cell,cell_clip});
                        ATELIER_TRY(paint_text(content.key,false,content.text,cell,cell_clip,data.columns[column].alignment,content.icon,enabled && descriptor.enabled));
                        if (paint && self.focus==key && selection.row==descriptor.key && selection.column==data.columns[column].key)
                            paint_box(cell,cell_clip,{0,0,0,0},{},{},std::max(1.f,scale),appearance.border,BorderStyle::solid);
                    }
                    if (paint) quad({table.body.x,y+row_height-scale,body_width,scale},body_clip,appearance.tree_line);
                }
                if (paint) {
                    const auto divider_clip=intersect(table_clip,clip_rect({inner.x,inner.y,body_width,header_height+body_height}));
                    column_x=inner.x-static_cast<float>(state.scroll_x)*scale;
                    for (std::size_t col_idx=0;col_idx<widths.size();++col_idx) {
                        column_x+=widths[col_idx];
                        const auto col_key=data.columns[col_idx].key;
                        const bool resizing=self.press && self.press->key==key && self.press->table_resize && self.press->table_column==col_key;
                        const bool hover_grip=!self.press && self.hover==key &&
                            contains(Rect{column_x-3*scale,inner.y,6*scale,header_height},self.hover_x,self.hover_y);
                        const float div_w=(resizing || hover_grip)?std::max(1.5f*scale,scale):scale;
                        quad({column_x-div_w,inner.y,div_w,header_height+body_height},divider_clip,
                            (resizing || hover_grip)?appearance.border:appearance.tree_line);
                    }
                    if (details) quad({inner.x,inner.y+header_height-scale,body_width,scale},header_clip,appearance.tree_line);
                }
                if (data.rows.empty()) ATELIER_TRY(paint_text(0,false,state.control.text,
                    {table.body.x,table.body.y,body_width,layout.row_height*scale},body_clip,TextAlign::left,ControlIcon::none,false));
                const auto paint_bar=[&](bool horizontal_bar,Rect bar_bounds,double maximum,double value,double page) {
                    if (maximum<=0 || bar_bounds.width<=0 || bar_bounds.height<=0) return;
                    ScrollbarGeometry bar;
                    bar.key=key; bar.bounds=bar_bounds; bar.clip=table_clip; bar.maximum=maximum;
                    bar.value=value; bar.page=page; bar.horizontal=horizontal_bar;
                    prepare_scrollbar(bar,appearance,enabled);
                };
                const auto inset=appearance.scrollbar_inset*scale,bar_width=appearance.scrollbar_width*scale;
                if (vertical) paint_bar(false,{inner.x+body_width+inset,table.body.y,bar_width,body_height},state.scroll_limit,state.scroll,body_height/scale);
                if (horizontal) paint_bar(true,{inner.x,table.body.y+body_height+inset,body_width,bar_width},state.scroll_x_limit,state.scroll_x,body_width/scale);
                std::sort(visible_text.begin(),visible_text.end());
                for (auto previous:state.table_text) if (!std::binary_search(visible_text.begin(),visible_text.end(),previous)) self.text.erase(previous);
                state.table_text=std::move(visible_text);
                output->tables.add(std::move(table));
