                auto& document=*state.control.code;
                if (state.code_revision!=document.revision()) {state.code_width=0;state.code_revision=document.revision();}
                const auto& buffer=document.edit();const auto& selection=buffer.current();
                const auto line_height=std::ceil(appearance.font.size*1.5f);
                const auto gutter=(appearance.scrollbar_width+2*appearance.scrollbar_inset)*scale;
                const auto margin=(12.f+static_cast<float>(std::to_wstring(buffer.line_count()).size())*appearance.font.size*.65f)*scale;
                Rect body{bounds.x+margin,bounds.y+2*scale,std::max(0.f,bounds.width-margin-gutter),std::max(0.f,bounds.height-4*scale-gutter)};
                const auto body_clip=intersect(clip,clip_rect(body));
                const auto editing=self.edit && self.edit->key==key;
                const auto* composition=editing && self.edit->composition?&*self.edit->composition:nullptr;
                const auto caret_offset=composition?composition->first:selection.caret.offset;
                const auto caret_row=buffer.line_at(caret_offset);
                const auto row_text=[&](std::size_t row) {
                    auto value=std::wstring(buffer.line(row));
                    if (composition) {
                        const auto first_row=buffer.line_at(composition->first),last_row=buffer.line_at(composition->last);
                        if (row==first_row) {
                            value.resize(composition->first-buffer.line_start(row));value+=composition->preedit.text;
                            value+=buffer.line(last_row).substr(composition->last-buffer.line_start(last_row));
                        } else if (row>first_row && row<=last_row) value.clear();
                    }
                    return value;
                };
                std::vector<detail::TextKey> visible_text;
                const auto cache_line=[&](std::size_t row) {
                    const detail::TextKey text_key{key,row+1,false};
                    self.text.set(text_key,row_text(row),appearance.font,false);
                    visible_text.push_back(text_key);
                    return text_key;
                };
                const auto caret_key=cache_line(caret_row);
                ATELIER_TRY_BIND(caret_paragraph,self.text.snapshot(caret_key,0));
                auto caret_position=selection.caret;caret_position.offset=caret_offset-buffer.line_start(caret_row);
                if (composition) caret_position.offset+=composition->preedit.cursor;
                ATELIER_TRY_BIND(caret_result,(*caret_paragraph)->caret(caret_position));
                const auto& caret=*caret_result;
                state.scroll_limit=std::max(0.0,static_cast<double>(buffer.line_count()*line_height-body.height/scale));
                if (state.code_selection_revision!=buffer.selection_revision() && body.height>0 && body.width>0) {
                    const auto y=caret_row*line_height;
                    if (y<state.scroll) state.scroll=y;
                    if (y+line_height>state.scroll+body.height/scale) state.scroll=y+line_height-body.height/scale;
                    if (caret.x<state.scroll_x) state.scroll_x=caret.x;
                    if (caret.x+appearance.caret_width>state.scroll_x+body.width/scale) state.scroll_x=caret.x+appearance.caret_width-body.width/scale;
                    state.code_selection_revision=buffer.selection_revision();
                }
                state.scroll=std::clamp(state.scroll,0.0,state.scroll_limit);
                CodeGeometry code{key,document.revision(),buffer.selection_revision(),body,line_height*scale,scale,{}};
                const auto first_row=std::min(buffer.line_count()-1,static_cast<std::size_t>(state.scroll/line_height));
                const auto end_row=std::min(buffer.line_count(),first_row+static_cast<std::size_t>(std::ceil(body.height/(line_height*scale)))+1);
                ATELIER_TRY_BIND(caret_metrics,self.text.metrics(caret_key,0));
                float width=caret_metrics->width;
                for (auto row=first_row;row<end_row;++row) {
                    const auto line_key=cache_line(row);ATELIER_TRY_BIND(metrics,self.text.metrics(line_key,0));width=std::max(width,metrics->width);
                }
                state.code_width=std::max(state.code_width,width+appearance.caret_width+4);
                state.scroll_x_limit=std::max(0.0,static_cast<double>(state.code_width-body.width/scale));
                state.scroll_x=std::clamp(state.scroll_x,0.0,state.scroll_x_limit);
                const auto first=std::min(selection.caret.offset,selection.anchor.offset),last=std::max(selection.caret.offset,selection.anchor.offset);
                for (auto row=first_row;row<end_row;++row) {
                    const detail::TextKey line_key{key,row+1,false},number_key{key,row+1,true};
                    ATELIER_TRY_BIND(paragraph_result,self.text.snapshot(line_key,0));
                    const auto& paragraph=*paragraph_result;
                    const auto x=body.x-static_cast<float>(state.scroll_x)*scale,y=body.y+(row*line_height-static_cast<float>(state.scroll))*scale;
                    const auto start=buffer.line_start(row),end=start+static_cast<std::uint32_t>(buffer.line(row).size());
                    code.lines.push_back({row,start,paragraph,x,y});
                    if (paint && row==caret_row && editing) quad({body.x,y,body.width,line_height*scale},body_clip,appearance.tree_alternate);
                    if (paint && first!=last && first<=end && last>=start && !composition) {
                        ATELIER_TRY_BIND(ranges,paragraph->selection(std::max(first,start)-start,std::min(last,end)-start));
                        for (auto range:*ranges)
                            quad({x+range.x*scale,y+range.y*scale,range.width*scale,line_height*scale},body_clip,appearance.selection);
                        if (last>end) {ATELIER_TRY_BIND(line_end,paragraph->caret({end-start,true}));quad({x+line_end->x*scale,y,appearance.font.size*.6f*scale,line_height*scale},body_clip,appearance.selection);}
                    }
                    if (paint) {
                        ATELIER_TRY_BIND(raster,self.text.raster(line_key,0,scale,clip_rect({static_cast<float>(body_clip.left)-x,static_cast<float>(body_clip.top)-y,
                            static_cast<float>(body_clip.right-body_clip.left),static_cast<float>(body_clip.bottom-body_clip.top)})));
                        if (raster->coverage) quad({x+raster->left,y+raster->top,static_cast<float>(raster->coverage->width),static_cast<float>(raster->coverage->height)},body_clip,appearance.foreground,raster->coverage);
                    }
                    self.text.set(number_key,std::to_wstring(row+1),appearance.font,false,TextAlign::right);visible_text.push_back(number_key);
                    if (paint) {
                        ATELIER_TRY_BIND(numbers,self.text.raster(number_key,margin/scale-8,scale));
                        if (numbers->coverage) quad({bounds.x+numbers->left,y+numbers->top,static_cast<float>(numbers->coverage->width),static_cast<float>(numbers->coverage->height)},
                            intersect(clip,clip_rect({bounds.x,body.y,margin-4*scale,body.height})),appearance.indicator_foreground.value_or(appearance.foreground),numbers->coverage);
                    }
                    if (row==caret_row && editing) {
                        const Rect physical{x+caret.x*scale,y+caret.y*scale,appearance.caret_width*scale,std::max(caret.height,line_height)*scale};
                        if (paint) quad(physical,body_clip,appearance.caret);
                        if (visible(physical,body_clip)) output->caret=TextCaret{self.edit->token,physical,body_clip};
                        ATELIER_TRY_BIND(anchor,composition?paragraph->caret({composition->first-start,false}):Result<Rect>{caret});
                        if (!state.control.read_only) output->input_anchor=TextInputAnchor{self.edit->token,composition?composition->id:0,composition?composition->revision:0,
                            {x+anchor->x*scale,y+anchor->y*scale,anchor->width*scale,line_height*scale},body_clip};
                        if (paint && composition) {
                            ATELIER_TRY_BIND(ranges,paragraph->selection(composition->first-start,composition->first-start+static_cast<std::uint32_t>(composition->preedit.text.size())));
                            for (auto range:*ranges)
                            quad({x+range.x*scale,y+line_height*scale-scale,range.width*scale,scale},body_clip,appearance.caret);
                        }
                    }
                }
                output->code_editors.push_back(std::move(code));
                prepare_scrollbar({key,{bounds.x+bounds.width-gutter,body.y,appearance.scrollbar_width*scale,body.height},{},{},{},{},clip,state.scroll_limit,state.scroll,body.height/scale,false},appearance,enabled);
                prepare_scrollbar({key,{body.x,body.y+body.height+scale,body.width,appearance.scrollbar_width*scale},{},{},{},{},clip,state.scroll_x_limit,state.scroll_x,body.width/scale,true},appearance,enabled);
                std::sort(visible_text.begin(),visible_text.end());visible_text.erase(std::unique(visible_text.begin(),visible_text.end()),visible_text.end());
                for (auto previous:state.table_text) if (!std::binary_search(visible_text.begin(),visible_text.end(),previous)) self.text.erase(previous);
                state.table_text=std::move(visible_text);
