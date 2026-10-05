    static Result<TableIndex> validate_table(const Control& control) {
        const auto& data=*control.table;
        const auto invalid=[&] { return std::unexpected(Error{ErrorCode::invalid_control,control.key}); };
        if (data.columns.empty() || data.columns.size()>128 || data.rows.size()>1000000 ||
            static_cast<unsigned>(data.sort.direction)>static_cast<unsigned>(TableSortDirection::descending)) return invalid();
        std::unordered_set<TableKey> columns,cells;
        TableIndex rows; rows.reserve(data.rows.size());
        for (const auto& column:data.columns) {
            if (!column.key || !columns.insert(column.key).second || column.title.size()>4096 || !detail::valid_text(column.title,false) ||
                !std::isfinite(column.width) || !std::isfinite(column.min_width) || column.min_width<12 ||
                column.width<column.min_width || column.width>32768 ||
                static_cast<unsigned>(column.alignment)>static_cast<unsigned>(TextAlign::right)) return invalid();
        }
        if (data.sort.column && !columns.contains(data.sort.column)) return invalid();
        for (const auto& row:data.rows) {
            if (!row.key || !rows.emplace(row.key,rows.size()).second || row.cells.size()!=data.columns.size()) return invalid();
            for (const auto& cell:row.cells)
                if (!cell.key || !cells.insert(cell.key).second || cell.text.size()>4096 || !detail::valid_text(cell.text,false) ||
                    static_cast<unsigned>(cell.icon)>static_cast<unsigned>(ControlIcon::more)) return invalid();
        }
        return rows;
    }
    const TableCellGeometry* table_cell(const TableGeometry& geometry,float x,float y) const {
        for (const auto& cell:geometry.cells)
            if (contains(cell.bounds,x,y) && contains(cell.clip,x,y)) return &cell;
        return nullptr;
    }
    static bool table_row_enabled(const State& state,TableKey key) {
        const auto row=state.table_index.find(key);
        return row!=state.table_index.end() && state.control.table->rows[row->second].enabled;
    }
    bool table_input(Reply& reply,const Event& event,const ControlBox* hit) {
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
                    if (column->sortable) action(reply,ActionKind::table_sort,released.key,TableSort{header.column,
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
                auto& offset=event.shift?state.scroll_x:state.scroll;
                const auto limit=event.shift?state.scroll_x_limit:state.scroll_limit;
                offset=std::clamp(offset-static_cast<double>(event.wheel)*48,0.0,limit);
                reply.consumed=reply.redraw=true; return true;
            }
            if (event.kind==EventKind::pointer_move) {
                reply.redraw=hover!=hit->key; hover=hit->key; reply.consumed=true;
                for (const auto& header:geometry->columns) if (contains(header.resize,event.x,event.y) && contains(header.clip,event.x,event.y))
                    reply.cursor=Cursor::resize_horizontal;
                return true;
            }
            if (event.kind==EventKind::pointer_down) {
                reply.consumed=true;
                if (event.button!=PointerButton::primary && event.button!=PointerButton::secondary) return true;
                finish_edit(reply,true); focus=hit->key; keyboard_pressed=0; keyboard_key=Key::none;
                const auto* cell=table_cell(*geometry,event.x,event.y);
                if (cell && !table_row_enabled(state,cell->row)) return true;
                if (event.button==PointerButton::secondary) {
                    if (cell) action(reply,ActionKind::select,hit->key,TableSelection{cell->row,cell->column});
                    if (state.control.context_target) context_request(reply,*hit,event.x,event.y,
                        cell?std::optional{TableSelection{cell->row,cell->column}}:std::nullopt);
                    reply.redraw=true; return true;
                }
                Press next{hit->key,event.x,event.y,event.x,event.y};
                if (cell) { next.table_cell={cell->row,cell->column}; next.table_activate=event.clicks>=2; }
                else {
                    for (const auto& header:geometry->columns) if (contains(header.clip,event.x,event.y) &&
                        (contains(header.bounds,event.x,event.y) || contains(header.resize,event.x,event.y))) {
                        next.table_column=header.column; next.table_resize=contains(header.resize,event.x,event.y);
                        const auto column=std::find_if(geometry->data->columns.begin(),geometry->data->columns.end(),[&](const auto& column){return column.key==header.column;});
                        next.original=next.value=column->width;
                        if (next.table_resize) reply.cursor=Cursor::resize_horizontal;
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
            if (event.key==keyboard_key && keyboard_pressed==focus) {
                keyboard_pressed=0; keyboard_key=Key::none;
                if (event.key==Key::enter && selected.row && table_row_enabled(states.at(focus),selected.row))
                    action(reply,ActionKind::activate,focus,TableSelection{selected.row,selected_column},box->action_target);
                reply.consumed=reply.redraw=true; return true;
            }
            return false;
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
                context_request(reply,*box,x,y,selection.row?std::optional{selection}:std::nullopt);
            }
            reply.consumed=true; return true;
        }
        const bool horizontal=event.key==Key::left || event.key==Key::right;
        if (!horizontal && event.key!=Key::up && event.key!=Key::down && event.key!=Key::home && event.key!=Key::end &&
            event.key!=Key::page_up && event.key!=Key::page_down) return false;
        reply.consumed=true;
        if (data.rows.empty()) return true;
        const auto& index=states.at(focus).table_index;
        const auto row=index.find(selected.row);
        const auto column=std::find_if(data.columns.begin(),data.columns.end(),[&](const auto& column){return column.key==selected.column;});
        auto r=row==index.end()?0:static_cast<int>(row->second);
        auto c=!details || column==data.columns.end()?0:static_cast<int>(column-data.columns.begin());
        const auto direction=event.key==Key::left || event.key==Key::up || event.key==Key::page_up || event.key==Key::home?-1:1;
        if (horizontal && details) c=std::clamp(c+direction,0,static_cast<int>(data.columns.size())-1);
        else if (event.key==Key::home) r=0;
        else if (event.key==Key::end) r=static_cast<int>(data.rows.size())-1;
        else if (row!=index.end()) {
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
