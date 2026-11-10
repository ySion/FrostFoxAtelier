    void sync_code_edit() {
        if (edit && edit->document && edit->document_selection!=edit->buffer().selection_revision()) revise();
    }
    void select_code_at(Reply& reply,float x,float y,bool extend) {
        const auto* geometry=published?published->find_code(edit->key):nullptr;
        if (!geometry || geometry->revision!=edit->document->revision() || geometry->lines.empty()) {reply.redraw=true;return;}
        const auto at=std::min_element(geometry->lines.begin(),geometry->lines.end(),[&](const auto& a,const auto& b) {
            return std::abs(y-(a.y+geometry->line_height*.5f))<std::abs(y-(b.y+geometry->line_height*.5f));
        });
        auto position=at->paragraph->hit((x-at->x)/geometry->scale,(y-at->y)/geometry->scale);
        if (!text_ok(reply,position,edit->key)) return;
        position->offset+=at->start;
        if (edit->buffer().select(*position,extend)) {revise();reply.redraw=true;}
    }
    bool code_input(Reply& reply,const Event& event,const ControlBox* hit) {
        if (event.kind==EventKind::wheel && hit && hit->kind==ControlKind::code_editor) {
            auto& state=states.at(hit->key);
            auto& offset=event.shift?state.scroll_x:state.scroll;
            const auto limit=event.shift?state.scroll_x_limit:state.scroll_limit;
            offset=std::clamp(offset-event.wheel*state.resolved->font.size*4.5,0.0,limit);
            reply.consumed=reply.redraw=true;return true;
        }
        const auto* state=live(focus);
        if (!state || state->control.kind!=ControlKind::code_editor || event.kind!=EventKind::key_down || event.alt) return false;
        const auto* accepted=published?published->find(focus):nullptr;
        if (!accepted || !current(*accepted)) return false;
        // A read-only document participates in ordinary Tab focus navigation,
        // rather than the editable CodeEditor's indentation command.
        if (state->control.read_only && event.key==Key::tab) return false;
        if (!edit && !begin_edit(reply,focus,true)) return true;
        auto& buffer=edit->buffer();const auto& selected=buffer.current();
        const auto row=buffer.line_at(selected.caret.offset);
        if (!event.control && event.key==Key::enter) {
            const auto line=buffer.line(row);
            const auto indent=line.find_first_not_of(L" \t");
            auto inserted=std::wstring(L"\n")+std::wstring(line.substr(0,std::min<std::size_t>(indent==std::wstring_view::npos?line.size():indent,
                selected.caret.offset-buffer.line_start(row))));
            insert(reply,inserted,detail::TextEdit::Group::isolated);
        } else if (!event.control && event.key==Key::tab) {
            const auto first=std::min(selected.caret.offset,selected.anchor.offset),last=std::max(selected.caret.offset,selected.anchor.offset);
            if (first==last && !event.shift) insert(reply,L"    ",detail::TextEdit::Group::isolated);
            else {
                const auto first_row=buffer.line_at(first),last_row=buffer.line_at(last && last>first?last-1:last);
                const auto start=buffer.line_start(first_row),end=buffer.line_start(last_row)+static_cast<std::uint32_t>(buffer.line(last_row).size());
                std::wstring replacement;
                for (auto current=first_row;current<=last_row;++current) {
                    auto value=buffer.line(current);
                    if (event.shift) {
                        std::size_t count{};
                        if (!value.empty() && value[0]==L'\t') count=1;
                        else while (count<4 && count<value.size() && value[count]==L' ') ++count;
                        value.remove_prefix(count);
                    } else replacement+=L"    ";
                    replacement.append(value);if (current!=last_row) replacement+=L'\n';
                }
                buffer.select({start,false},false);buffer.select({end,true},true);
                insert(reply,replacement,detail::TextEdit::Group::isolated);
            }
        } else if (event.key==Key::left || event.key==Key::right || event.key==Key::up || event.key==Key::down ||
                   event.key==Key::home || event.key==Key::end || event.key==Key::page_up || event.key==Key::page_down) {
            auto offset=selected.caret.offset;
            const auto length=static_cast<std::uint32_t>(selected.text.size());
            const bool backwards=event.key==Key::left || event.key==Key::up || event.key==Key::home || event.key==Key::page_up;
            if ((event.key==Key::left || event.key==Key::right) && !event.shift && selected.caret.offset!=selected.anchor.offset)
                offset=backwards?std::min(selected.caret.offset,selected.anchor.offset):std::max(selected.caret.offset,selected.anchor.offset);
            else if (event.key==Key::home || event.key==Key::end) {
                offset=event.control?(backwards?0:length):buffer.line_start(row)+(backwards?0:static_cast<std::uint32_t>(buffer.line(row).size()));
            } else if (event.key==Key::left || event.key==Key::right) {
                const auto& boundaries=buffer.boundaries();auto at=std::lower_bound(boundaries.begin(),boundaries.end(),offset);
                if (backwards && at!=boundaries.begin()) offset=*--at;
                else if (!backwards && at+1!=boundaries.end()) offset=*++at;
                if (event.control) {
                    const auto word=[](wchar_t c){return std::iswalnum(c) || c==L'_';};
                    while (backwards && offset && word(selected.text[offset-1])) offset=buffer.snap(offset-1,false).offset;
                    while (!backwards && offset<length && word(selected.text[offset])) offset=buffer.snap(offset+1,true).offset;
                }
            } else {
                std::size_t distance=1;
                if (event.key==Key::page_up || event.key==Key::page_down) {
                    if (const auto* geometry=published->find_code(focus)) distance=std::max<std::size_t>(1,static_cast<std::size_t>(geometry->body.height/geometry->line_height));
                }
                const auto destination=backwards?(row>distance?row-distance:0):std::min(row+distance,buffer.line_count()-1);
                offset=buffer.line_start(destination)+static_cast<std::uint32_t>(std::min<std::size_t>(offset-buffer.line_start(row),buffer.line(destination).size()));
            }
            if (buffer.select({offset,!backwards},event.shift)) revise();
        } else return false;
        reply.consumed=reply.redraw=true;return true;
    }
