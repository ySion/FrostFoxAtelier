    static Result<TableIndex> validate_table(const Control& control) {
        const auto& data=*control.table;
        const auto invalid=[&] { return std::unexpected(Error{ErrorCode::invalid_control,control.key}); };
        if (data.columns.empty() || data.columns.size()>128 || data.rows.size()>1000000 ||
            static_cast<unsigned>(data.sort.direction)>static_cast<unsigned>(TableSortDirection::descending)) return invalid();
        Set<TableKey> columns,cells;
        columns.reserve(data.columns.size());
        cells.reserve(std::min<std::size_t>(data.rows.size()*data.columns.size(),65536));
        TableIndex rows; rows.reserve(data.rows.size());
        for (const auto& column:data.columns) {
            if (!column.key || !columns.add(column.key) || column.title.size()>4096 || !valid_wide_text(column.title,false) ||
                !std::isfinite(column.width) || !std::isfinite(column.min_width) || column.min_width<12 ||
                column.width<column.min_width || column.width>32768 ||
                static_cast<unsigned>(column.alignment)>static_cast<unsigned>(TextAlign::right)) return invalid();
        }
        if (data.sort.column && !columns.contains(data.sort.column)) return invalid();
        for (const auto& row:data.rows) {
            if (!row.key || !rows.try_emplace(row.key,rows.size()).inserted || row.cells.size()!=data.columns.size()) return invalid();
            for (const auto& cell:row.cells)
                if (!cell.key || !cells.add(cell.key) || cell.text.size()>4096 || !valid_wide_text(cell.text,false) ||
                    static_cast<unsigned>(cell.icon)>static_cast<unsigned>(last_control_icon)) return invalid();
        }
        return rows;
    }
    const TableCellGeometry* table_cell(const TableGeometry& geometry,float x,float y) const {
        for (const auto& cell:geometry.cells)
            if (contains(cell.bounds,x,y) && contains(cell.clip,x,y)) return &cell;
        return nullptr;
    }
    static bool table_row_enabled(const State& state,TableKey key) {
        const auto* row=state.table_index.find(key);
        return row && state.control.table->rows[*row].enabled;
    }
    bool table_input(Reply& reply,const Event& event,const ControlBox* hit,ControlKey released_keyboard) {
        if (press && states.at(press->key).control.kind==ControlKind::table && !press->scrollbar) {
            auto& held=*press;
            const auto* box=published->find(held.key);
            if (!box || !current(*box)) { reply=cancel(); return true; }
            const auto* geometry=published->find_table(held.key);
            reply.consumed=true;
            if (event.kind==EventKind::key_down && event.key==Key::escape) { reply=cancel(); reply.consumed=true; return true; }
            if (event.kind==EventKind::pointer_move) {
                if (held.table_resize) {
                    const auto& columns=box->table->columns;
                    const auto column=std::find_if(columns.begin(),columns.end(),[&](const auto& column){return column.key==held.table_column;});
                    if (column==columns.end()) { reply=cancel(); return true; }
                    held.value=std::clamp(held.original+(event.x-held.x)/surface.dpi_scale,static_cast<double>(column->min_width),32768.0);
                    reply.cursor=Cursor::resize_horizontal; reply.redraw=true;
                }
                return true;
            }
            if (event.kind!=EventKind::pointer_up || event.button!=held.button) return true;
            const auto released=held; press.reset(); reply.capture=Capture::release; reply.redraw=true;
            if (released.table_resize) {
                if (released.value!=released.original) action(reply,ActionKind::table_column_resize,released.key,
                    TableColumnResize{released.table_column,static_cast<float>(released.value)});
                return true;
            }
            if (!geometry) return true;
            if (released.table_column) {
                for (const auto& header:geometry->columns) if (header.column==released.table_column &&
                    contains(header.bounds,event.x,event.y) && contains(header.clip,event.x,event.y)) {
                    const auto& data=*geometry->data;
                    const auto column=std::find_if(data.columns.begin(),data.columns.end(),[&](const auto& column){return column.key==header.column;});
                    if (column!=data.columns.end() && column->sortable) action(reply,ActionKind::table_sort,released.key,TableSort{header.column,
                        data.sort.column==header.column && data.sort.direction==TableSortDirection::ascending?
                            TableSortDirection::descending:TableSortDirection::ascending});
                    break;
                }
            } else if (const auto* cell=table_cell(*geometry,event.x,event.y);cell &&
                cell->row==released.table_cell.row && cell->column==released.table_cell.column) {
                action(reply,ActionKind::select,released.key,released.table_cell);
                if (released.table_activate) action(reply,ActionKind::activate,released.key,released.table_cell,box->action_target);
            }
            return true;
        }
        if (press) return false;
        if (hit && hit->kind==ControlKind::table) {
            auto& state=states.at(hit->key);
            const auto* geometry=published->find_table(hit->key);
            if (!geometry) return false;
            if (event.kind==EventKind::wheel) {
                if (edit && states.at(edit->key).control.transient_edit && states.at(edit->key).control.rename_anchor) {
                    if (!finish_edit(reply,true)) return true;
                }
                auto& offset=event.shift?state.scroll_x:state.scroll;
                const auto limit=event.shift?state.scroll_x_limit:state.scroll_limit;
                offset=std::clamp(offset-static_cast<double>(event.wheel)*48,0.0,limit);
                reply.consumed=reply.redraw=true; return true;
            }
            if (event.kind==EventKind::pointer_move) {
                const bool header_zone=contains(geometry->header,event.x,event.y);
                reply.redraw|=hover!=hit->key || header_zone || contains(geometry->header,hover_x,hover_y);
                hover=hit->key; hover_x=event.x; hover_y=event.y; reply.consumed=true;
                for (const auto& header:geometry->columns) if (contains(header.resize,event.x,event.y) && contains(header.clip,event.x,event.y))
                    reply.cursor=Cursor::resize_horizontal;
                return true;
            }
            if (event.kind==EventKind::pointer_down) {
                reply.consumed=true;
                if (event.button!=PointerButton::primary && event.button!=PointerButton::secondary) return true;
                if (!finish_edit(reply,true)) return true;
                retire_keyboard(reply); set_focus(reply,hit->key);
                const auto* cell=table_cell(*geometry,event.x,event.y);
                if (cell && !table_row_enabled(state,cell->row)) return true;
                if (event.button==PointerButton::secondary) {
                    if (cell) action(reply,ActionKind::select,hit->key,TableSelection{cell->row,cell->column});
                    if (state.control.context_target) context_request(reply,*hit,event.x,event.y,
                        cell?Optional{TableSelection{cell->row,cell->column}}:Optional<TableSelection>{});
                    reply.redraw=true; return true;
                }
                Press next{hit->key,event.x,event.y,event.x,event.y};
                if (cell) { next.table_cell={cell->row,cell->column}; next.table_activate=event.clicks>=2; }
                else {
                    for (const auto& header:geometry->columns) if (contains(header.clip,event.x,event.y) &&
                        (contains(header.bounds,event.x,event.y) || contains(header.resize,event.x,event.y))) {
                        next.table_column=header.column; next.table_resize=contains(header.resize,event.x,event.y);
                        const auto col_it=std::find_if(geometry->data->columns.begin(),geometry->data->columns.end(),[&](const auto& column){return column.key==header.column;});
                        next.original=next.value=col_it->width;
                        if (next.table_resize) {
                            reply.cursor=Cursor::resize_horizontal;
                            if (event.clicks>=2) {
                                const std::size_t col_idx=static_cast<std::size_t>(col_it-geometry->data->columns.begin());
                                const auto& app_style=*state.resolved;
                                const float pad_x=std::max(4.f,app_style.padding.left)+std::max(4.f,app_style.padding.right);
                                float fitted=col_it->min_width;
                                const String title_u8 = wide_to_utf8(col_it->title);
                                text.set(0,title_u8.view(),app_style.font,false,col_it->alignment);
                                const auto hm=text.metrics(0,32768.f);
                                if (!text_ok(reply,hm,hit->key)) return true;
                                fitted=std::max(fitted,std::ceil(hm->width+pad_x+(geometry->data->sort.column==col_it->key?16.f:4.f)));
                                const auto& all_rows=geometry->data->rows;
                                const std::size_t start_row=geometry->row_stride>0?std::min(all_rows.size(),static_cast<std::size_t>(std::max(0.0,state.scroll*surface.dpi_scale/geometry->row_stride))):0;
                                const std::size_t end_row=std::min(all_rows.size(),start_row+256);
                                for (std::size_t r=start_row;r<end_row;++r) {
                                    const auto& cell_val=all_rows[r].cells[col_idx];
                                    const float icon_extra=cell_val.icon!=ControlIcon::none?app_style.indicator_size+6.f:0.f;
                                    if (!cell_val.text.empty()) {
                                        const String cell_u8 = wide_to_utf8(cell_val.text);
                                        text.set(0,cell_u8.view(),app_style.font,false,col_it->alignment);
                                        const auto cm=text.metrics(0,32768.f);
                                        if (!text_ok(reply,cm,hit->key)) return true;
                                        fitted=std::max(fitted,std::ceil(cm->width+pad_x+icon_extra+4.f));
                                    } else if (icon_extra>0.f) {
                                        fitted=std::max(fitted,std::ceil(pad_x+icon_extra));
                                    }
                                }
                                fitted=std::clamp(fitted,col_it->min_width,std::max(col_it->min_width,640.f));
                                if (fitted!=col_it->width)
                                    action(reply,ActionKind::table_column_resize,hit->key,TableColumnResize{col_it->key,fitted});
                                reply.redraw=true;
                                return true;
                            }
                        }
                        break;
                    }
                }
                if (next.table_cell.row || next.table_column) { press=next; reply.capture=Capture::acquire; }
                reply.redraw=true; return true;
            }
        }
        const auto* box=published->find(focus);
        if (!box || box->kind!=ControlKind::table || !current(*box) ||
            (event.kind!=EventKind::key_down && event.kind!=EventKind::key_up)) return false;
        const auto& data=*box->table;
        const auto selected=box->table_selection;
        const bool details=box->table_layout.view==TableView::details;
        const auto selected_column=details && selected.column?selected.column:data.columns.front().key;
        if (event.key==Key::tab) return false; // A table contributes one stop to the ordinary focus order.
        if (event.kind==EventKind::key_up) {
            if (released_keyboard==focus) {
                if (event.key==Key::enter && selected.row && table_row_enabled(states.at(focus),selected.row))
                    action(reply,ActionKind::activate,focus,TableSelection{selected.row,selected_column},box->action_target);
                reply.consumed=reply.redraw=true; return true;
            }
            return false;
        }
        if (event.control && !event.alt && event.key==Key::c) {
            reply.consumed=true;
            const auto* row_idx=states.at(focus).table_index.find(selected.row);
            if (row_idx) {
                const auto& row_data=data.rows[*row_idx];
                String copied;
                if (details && !event.shift) {
                    const auto col_it=std::find_if(data.columns.begin(),data.columns.end(),[&](const auto& col){return col.key==selected_column;});
                    if (col_it!=data.columns.end())
                        copied=wide_to_utf8(row_data.cells[static_cast<std::size_t>(col_it-data.columns.begin())].text);
                }
                if (copied.empty()) {
                    for (std::size_t i=0;i<row_data.cells.size();++i) {
                        if (i) (void)copied.append("\t");
                        (void)copied.append(wide_to_utf8(row_data.cells[i].text));
                    }
                }
                if (!copied.empty()) {
                    if (copied.size()>65536)
                        copied=String::from_utf8_validated(copied.view().utf8_prefix(65536));
                    table_clipboard=ClipboardRequest{ClipboardKind::copy,detail::new_identity(),
                        EditToken{focus,box->incarnation,detail::new_identity(),detail::new_identity()},std::move(copied),65536u};
                    platform(reply,*table_clipboard);
                }
            }
            return true;
        }
        if (event.control || event.alt) return false;
        if (event.key==Key::enter) {
            keyboard_pressed=focus; keyboard_key=event.key; reply.consumed=true; return true;
        }
        if (event.key==Key::menu || (event.key==Key::f10 && event.shift)) {
            if (states.at(focus).control.context_target) {
                const TableSelection selection{selected.row,selected_column};
                const auto* geometry=published->find_table(focus);
                float x=box->bounds.x,y=box->bounds.y;
                if (geometry) for (const auto& cell:geometry->cells) if (cell.row==selection.row && cell.column==selection.column) {
                    x=static_cast<float>(std::max(cell.clip.left,static_cast<int>(cell.bounds.x)));
                    y=static_cast<float>(std::min(cell.clip.bottom,static_cast<int>(cell.bounds.y+cell.bounds.height))); break;
                }
                context_request(reply,*box,x,y,selection.row?Optional{selection}:Optional<TableSelection>{});
            }
            reply.consumed=true; return true;
        }
        const bool horizontal=event.key==Key::left || event.key==Key::right;
        if (!horizontal && event.key!=Key::up && event.key!=Key::down && event.key!=Key::home && event.key!=Key::end &&
            event.key!=Key::page_up && event.key!=Key::page_down) return false;
        reply.consumed=true;
        if (data.rows.empty()) return true;
        const auto& index=states.at(focus).table_index;
        const auto* row=index.find(selected.row);
        const auto column=std::find_if(data.columns.begin(),data.columns.end(),[&](const auto& column){return column.key==selected.column;});
        auto r=!row?0:static_cast<int>(*row);
        auto c=!details || column==data.columns.end()?0:static_cast<int>(column-data.columns.begin());
        const auto direction=event.key==Key::left || event.key==Key::up || event.key==Key::page_up || event.key==Key::home?-1:1;
        if (horizontal && details) c=std::clamp(c+direction,0,static_cast<int>(data.columns.size())-1);
        else if (event.key==Key::home) r=0;
        else if (event.key==Key::end) r=static_cast<int>(data.rows.size())-1;
        else if (row) {
            const auto* geometry=published->find_table(focus);
            const auto columns=geometry?static_cast<int>(geometry->columns_per_row):1;
            const auto page=(event.key==Key::page_up || event.key==Key::page_down) && geometry?
                std::max(1,static_cast<int>(geometry->body.height/geometry->row_stride)):1;
            const auto step=horizontal?1:page*columns;
            r=std::clamp(r+direction*step,0,static_cast<int>(data.rows.size())-1);
        }
        if (!data.rows[r].enabled) {
            const auto start=r;
            while (r>=0 && r<static_cast<int>(data.rows.size()) && !data.rows[r].enabled) r+=direction;
            if (r<0 || r>=static_cast<int>(data.rows.size())) {
                r=start; while (r>=0 && r<static_cast<int>(data.rows.size()) && !data.rows[r].enabled) r-=direction;
            }
            if (r<0 || r>=static_cast<int>(data.rows.size())) return true;
        }
        action(reply,ActionKind::select,focus,TableSelection{data.rows[r].key,data.columns[c].key});
        reply.redraw=true; return true;
    }
