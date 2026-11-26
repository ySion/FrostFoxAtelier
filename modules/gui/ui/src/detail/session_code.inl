    void sync_code_edit() {
        if (edit && edit->document && edit->document_selection != edit->buffer().selection_revision())
            revise();
    }
    void ensure_code_block_comments(State& st, const detail::TextEdit& buffer, CodeLanguage lang) {
        const std::uint64_t rev = st.control.code ? st.control.code->revision() : 0;
        if (st.code_comment_revision != rev || st.code_comment_lang != lang ||
            st.code_block_comments.size() != buffer.line_count()) {
            detail::compute_block_comment_states(buffer, lang, st.code_block_comments);
            st.code_comment_revision = rev;
            st.code_comment_lang = lang;
        }
    }
    bool resolve_code_hit(Reply& reply, ControlKey key, float x, float y, detail::TextPosition& out_pos,
                          std::size_t& out_row) {
        const auto* geometry = published ? published->find_code(key) : nullptr;
        const auto* st = live(key);
        if (!st || !st->control.code || !geometry || geometry->revision != st->control.code->revision() ||
            geometry->lines.empty()) {
            reply.redraw = true;
            return false;
        }
        const auto at =
            std::min_element(geometry->lines.begin(), geometry->lines.end(), [&](const auto& a, const auto& b) {
                return std::abs(y - (a.y + geometry->line_height * .5f)) <
                       std::abs(y - (b.y + geometry->line_height * .5f));
            });
        out_row = at->row;
        if (x < geometry->body.x) {
            out_pos = {at->start, false};
            return true;
        }
        auto position = at->paragraph->hit((x - at->x) / geometry->scale, (y - at->y) / geometry->scale);
        if (!text_ok(reply, position, key))
            return false;
        position->offset += at->start;
        out_pos = *position;
        return true;
    }
    static std::pair<std::size_t, std::size_t> selected_line_span(const detail::TextEdit& buffer, std::uint32_t first,
                                                                  std::uint32_t last) noexcept {
        const std::size_t r0 = buffer.line_at(first);
        const std::size_t r1 =
            buffer.line_at(last > first && buffer.current().text[last - 1] == '\n' ? last - 1 : last);
        return {r0, r1};
    }
    static std::uint32_t line_select_end(const detail::TextEdit& buffer, std::size_t row) noexcept {
        const std::uint32_t ls = buffer.line_start(row);
        const auto total = static_cast<std::uint32_t>(buffer.current().text.size());
        return std::min(total, ls + static_cast<std::uint32_t>(buffer.line(row).size()) +
                                   (row + 1 < buffer.line_count() ? 1u : 0u));
    }
    void select_code_at(Reply& reply, float x, float y, bool extend) {
        detail::TextPosition pos{};
        std::size_t hit_row = 0;
        if (!resolve_code_hit(reply, edit->key, x, y, pos, hit_row))
            return;
        auto& buffer = edit->buffer();
        const CodeLanguage lang = edit->document ? edit->document->language() : CodeLanguage::plain;
        const std::uint8_t mode = press && press->key == edit->key ? press->code_select_mode : 0;
        if (extend && mode == 1) {
            const std::uint32_t ls = buffer.line_start(hit_row);
            const auto wr = detail::word_range_at(buffer.line(hit_row), pos.offset - ls, lang);
            const std::uint32_t w0 = ls + wr.first;
            const std::uint32_t w1 = ls + wr.second;
            const std::uint32_t a0 = press->code_anchor_start;
            const std::uint32_t a1 = press->code_anchor_end;
            const bool forward = w1 >= a1;
            const std::uint32_t sel_anchor = forward ? a0 : a1;
            const std::uint32_t sel_caret = forward ? std::max(a1, w1) : std::min(a0, w0);
            bool moved = buffer.select({sel_anchor, false}, false);
            moved |= buffer.select({sel_caret, forward}, true);
            if (moved) {
                revise();
                reply.redraw = true;
            }
            return;
        }
        if (extend && mode == 2) {
            const std::uint32_t ls = buffer.line_start(hit_row);
            const std::uint32_t le = line_select_end(buffer, hit_row);
            const std::uint32_t a0 = press->code_anchor_start;
            const std::uint32_t a1 = press->code_anchor_end;
            const bool forward = le >= a1;
            const std::uint32_t sel_anchor = forward ? a0 : a1;
            const std::uint32_t sel_caret = forward ? std::max(a1, le) : std::min(a0, ls);
            bool moved = buffer.select({sel_anchor, false}, false);
            moved |= buffer.select({sel_caret, forward}, true);
            if (moved) {
                revise();
                reply.redraw = true;
            }
            return;
        }
        if (buffer.select(pos, extend)) {
            revise();
            reply.redraw = true;
        }
    }
    bool code_input(Reply& reply, const Event& event, const ControlBox* hit) {
        if (event.kind == EventKind::wheel && hit && hit->kind == ControlKind::code_editor) {
            auto& state = states.at(hit->key);
            auto& offset = event.shift ? state.scroll_x : state.scroll;
            const auto limit = event.shift ? state.scroll_x_limit : state.scroll_limit;
            offset = std::clamp(offset - event.wheel * state.resolved->font.size * 4.5, 0.0, limit);
            reply.consumed = reply.redraw = true;
            return true;
        }
        // Specialized mouse press handling: Ctrl+Click Go-to-Definition, Gutter line select, Double-click word,
        // Triple-click line.
        if (event.kind == EventKind::pointer_down && event.button == PointerButton::primary && !press && hit &&
            hit->kind == ControlKind::code_editor) {
            const auto* geometry = published ? published->find_code(hit->key) : nullptr;
            const bool in_gutter = geometry && event.x < geometry->body.x;
            const bool ctrl_click = event.control && !event.shift && !event.alt && event.clicks == 1 && !in_gutter;
            if (ctrl_click || in_gutter || event.clicks >= 2) {
                reply.consumed = true;
                retire_keyboard(reply);
                if (hit->key != focus) {
                    if (!finish_edit(reply, true))
                        return true;
                    set_focus(reply, hit->key);
                }
                if ((!edit || edit->key != hit->key) && !begin_edit(reply, hit->key, true))
                    return true;
                if (edit->composition) {
                    cancel_composition(reply);
                    return true;
                }
                edit->buffer().break_group();
                detail::TextPosition pos{};
                std::size_t hit_row = 0;
                if (!resolve_code_hit(reply, hit->key, event.x, event.y, pos, hit_row))
                    return true;
                auto& buffer = edit->buffer();
                if (ctrl_click) {
                    if (buffer.select(pos, false))
                        revise();
                    action(reply, ActionKind::activate, hit->key, static_cast<double>(pos.offset));
                    reply.redraw = true;
                    return true;
                }
                std::uint32_t s0 = pos.offset;
                std::uint32_t s1 = pos.offset;
                std::uint8_t mode = 0;
                const CodeLanguage click_lang = edit->document ? edit->document->language() : CodeLanguage::plain;
                if (in_gutter || event.clicks >= 3) {
                    mode = 2;
                    s0 = buffer.line_start(hit_row);
                    s1 = line_select_end(buffer, hit_row);
                } else {
                    mode = 1;
                    const std::uint32_t ls = buffer.line_start(hit_row);
                    const auto wr = detail::word_range_at(buffer.line(hit_row), pos.offset - ls, click_lang);
                    s0 = ls + wr.first;
                    s1 = ls + wr.second;
                }
                bool moved = buffer.select({s0, false}, false);
                moved |= buffer.select({s1, true}, true);
                if (moved)
                    revise();
                const auto& control = states.at(hit->key).control;
                press = Press{hit->key, event.x, event.y, event.x, event.y, control.value, control.value, false, true};
                press->button = event.button;
                press->code_select_mode = mode;
                press->code_anchor_start = s0;
                press->code_anchor_end = s1;
                reply.capture = Capture::acquire;
                reply.redraw = true;
                return true;
            }
        }

        auto* state = live(focus);
        if (!state || state->control.kind != ControlKind::code_editor)
            return false;
        const auto* accepted = published ? published->find(focus) : nullptr;
        if (!accepted || !current(*accepted))
            return false;

        // Smart auto-pairing, selection wrapping, closing skip-over, and '}' auto-dedent on typed text.
        if (event.kind == EventKind::text && !state->control.read_only && !event.control && !event.alt) {
            const auto* value = std::get_if<String>(&event.data);
            if (!value || value->size() != 1 || static_cast<unsigned char>((*value)[0]) < 32u)
                return false;
            if (edit && edit->composition)
                return false;
            if (!edit && !begin_edit(reply, focus, true))
                return true;
            const char ch = (*value)[0];
            auto& buffer = edit->buffer();
            const auto& selected = buffer.current();
            const std::uint32_t first = std::min(selected.caret.offset, selected.anchor.offset);
            const std::uint32_t last = std::max(selected.caret.offset, selected.anchor.offset);
            const bool is_open = ch == '(' || ch == '[' || ch == '{';
            const bool is_close = ch == ')' || ch == ']' || ch == '}';
            const bool is_quote = ch == '"';
            if ((is_open || is_quote) && first != last && buffer.line_at(first) == buffer.line_at(last)) {
                const char closer = is_quote ? '"' : detail::matching_bracket(ch);
                String wrapped;
                wrapped.reserve(last - first + 2);
                (void)wrapped.append(StringView{&ch, 1});
                (void)wrapped.append(selected.text.slice(first, last - first));
                (void)wrapped.append(StringView{&closer, 1});
                reply.consumed = reply.redraw = true;
                edit->text_mode = true;
                insert(reply, wrapped.view(), detail::TextEdit::Group::isolated);
                if (!reply.error) {
                    buffer.select({first + 1, false}, false);
                    buffer.select({last + 1, true}, true);
                    revise();
                }
                return true;
            }
            if (first == last && (is_close || is_quote) && first < selected.text.size() && selected.text[first] == ch) {
                // Skip over existing closing delimiter instead of duplicating it.
                const bool escaped = is_quote && first > 0 && selected.text[first - 1] == '\\';
                if (!escaped) {
                    reply.consumed = reply.redraw = true;
                    edit->text_mode = true;
                    buffer.break_group();
                    if (buffer.select({first + 1, true}, false))
                        revise();
                    return true;
                }
            }
            if (first == last && ch == '}') {
                const std::size_t row = buffer.line_at(first);
                const std::uint32_t ls = buffer.line_start(row);
                const StringView before_caret = buffer.line(row).slice(0, first - ls);
                if (!before_caret.empty() && detail::is_blank_line(before_caret) && before_caret.size() >= 4) {
                    const CodeLanguage lang = state->control.code->language();
                    ensure_code_block_comments(*state, buffer, lang);
                    const bool in_blk = row < state->code_block_comments.size() && state->code_block_comments[row] != 0;
                    if (!in_blk) {
                        const auto scope =
                            detail::find_active_indent_scope(buffer, lang, state->code_block_comments.view(), first);
                        if (scope.guide_column != std::numeric_limits<std::uint32_t>::max() &&
                            before_caret.size() == scope.guide_column + 4) {
                            reply.consumed = reply.redraw = true;
                            edit->text_mode = true;
                            buffer.select({first - 4, false}, false);
                            buffer.select({first, true}, true);
                            insert(reply, "}", detail::TextEdit::Group::isolated);
                            return true;
                        }
                    }
                }
            }
            if (first == last && (is_open || is_quote)) {
                const char next_ch = first < selected.text.size() ? selected.text[first] : '\0';
                const char prev_ch = first > 0 ? selected.text[first - 1] : '\0';
                const bool boundary_after = next_ch == '\0' || next_ch == '\n' || next_ch == ' ' || next_ch == '\t' ||
                                            next_ch == ')' || next_ch == ']' || next_ch == '}' || next_ch == ';' ||
                                            next_ch == ',';
                const bool allow_quote =
                    !is_quote || (!is_ascii_ident_continue(prev_ch) && prev_ch != '"' && prev_ch != '\\');
                if (boundary_after && allow_quote) {
                    const char closer = is_quote ? '"' : detail::matching_bracket(ch);
                    const char pair_str[2]{ch, closer};
                    reply.consumed = reply.redraw = true;
                    edit->text_mode = true;
                    insert(reply, StringView{pair_str, 2}, detail::TextEdit::Group::insert);
                    if (!reply.error && buffer.select({first + 1, false}, false))
                        revise();
                    return true;
                }
            }
            return false;
        }

        if (event.kind != EventKind::key_down)
            return false;
        const bool alt_line_move =
            event.alt && !event.control && !event.shift && (event.key == Key::up || event.key == Key::down);
        if (event.alt && !alt_line_move)
            return false;
        // A read-only document participates in ordinary Tab focus navigation,
        // rather than the editable CodeEditor's indentation command.
        if (state->control.read_only && event.key == Key::tab)
            return false;
        if (!edit && !begin_edit(reply, focus, true))
            return true;
        auto& buffer = edit->buffer();
        const auto& selected = buffer.current();
        const auto row = buffer.line_at(selected.caret.offset);
        const auto first = std::min(selected.caret.offset, selected.anchor.offset);
        const auto last = std::max(selected.caret.offset, selected.anchor.offset);

        // Alt+Up / Alt+Down: move current line or selected line block up/down.
        if (alt_line_move) {
            if (state->control.read_only) {
                reply.consumed = true;
                return true;
            }
            const auto [r0, r1] = selected_line_span(buffer, first, last);
            const bool up = event.key == Key::up;
            if ((up && r0 == 0) || (!up && r1 + 1 >= buffer.line_count())) {
                reply.consumed = true;
                return true;
            }
            const std::size_t swap_r0 = up ? r0 - 1 : r0;
            const std::size_t swap_r1 = up ? r1 : r1 + 1;
            const std::uint32_t block_start = buffer.line_start(r0);
            const std::uint32_t block_end = buffer.line_start(r1) + static_cast<std::uint32_t>(buffer.line(r1).size());
            const StringView moving = selected.text.slice(block_start, block_end - block_start);
            const StringView neighbor = buffer.line(up ? r0 - 1 : r1 + 1);
            const std::uint32_t range_start = buffer.line_start(swap_r0);
            const std::uint32_t range_end =
                buffer.line_start(swap_r1) + static_cast<std::uint32_t>(buffer.line(swap_r1).size());
            String combined;
            combined.reserve(moving.size() + neighbor.size() + 1);
            if (up) {
                (void)combined.append(moving);
                (void)combined.append("\n");
                (void)combined.append(neighbor);
            } else {
                (void)combined.append(neighbor);
                (void)combined.append("\n");
                (void)combined.append(moving);
            }
            const std::int64_t delta = up ? -static_cast<std::int64_t>(neighbor.size() + 1)
                                          : static_cast<std::int64_t>(neighbor.size() + 1);
            const auto next_anchor =
                static_cast<std::uint32_t>(static_cast<std::int64_t>(selected.anchor.offset) + delta);
            const auto next_caret =
                static_cast<std::uint32_t>(static_cast<std::int64_t>(selected.caret.offset) + delta);
            buffer.select({range_start, false}, false);
            buffer.select({range_end, true}, true);
            insert(reply, combined.view(), detail::TextEdit::Group::isolated);
            if (!reply.error) {
                buffer.select({next_anchor, false}, false);
                buffer.select({next_caret, selected.caret.trailing}, true);
                revise();
            }
            reply.consumed = reply.redraw = true;
            return true;
        }

        // Ctrl+/ : toggle line or block comments across [r0, r1].
        if (event.control && !event.shift && event.key == Key::slash) {
            if (state->control.read_only) {
                reply.consumed = true;
                return true;
            }
            const CodeLanguage lang = state->control.code->language();
            const auto [r0, r1] = selected_line_span(buffer, first, last);
            const std::uint32_t range_start = buffer.line_start(r0);
            const std::uint32_t range_end = buffer.line_start(r1) + static_cast<std::uint32_t>(buffer.line(r1).size());
            String replacement;
            std::size_t edit_col_r0 = 0;
            if (lang == CodeLanguage::style_sheet) {
                const StringView slice = selected.text.slice(range_start, range_end - range_start);
                std::size_t a = 0;
                while (a < slice.size() && (slice[a] == ' ' || slice[a] == '\t'))
                    ++a;
                std::size_t b = slice.size();
                while (b > a && (slice[b - 1] == ' ' || slice[b - 1] == '\t'))
                    --b;
                edit_col_r0 = a;
                if (b - a >= 4 && slice.slice(a, 2) == "/*" && slice.slice(b - 2, 2) == "*/" &&
                    !slice.slice(a + 2, b - a - 4).contains("*/")) {
                    (void)replacement.append(slice.slice(0, a));
                    StringView inner = slice.slice(a + 2, b - a - 4);
                    if (!inner.empty() && inner.front() == ' ')
                        inner.remove_prefix(1);
                    if (!inner.empty() && inner.back() == ' ')
                        inner.remove_suffix(1);
                    (void)replacement.append(inner);
                    (void)replacement.append(slice.slice(b));
                } else {
                    (void)replacement.append(slice.slice(0, a));
                    (void)replacement.append("/* ");
                    (void)replacement.append(slice.slice(a));
                    (void)replacement.append(" */");
                }
            } else {
                bool all_commented = true;
                bool any_non_blank = false;
                std::size_t min_indent = std::numeric_limits<std::size_t>::max();
                for (std::size_t r = r0; r <= r1; ++r) {
                    const StringView ln = buffer.line(r);
                    const std::size_t p = ln.find_first_not_of(" \t");
                    if (p == StringView::npos)
                        continue;
                    any_non_blank = true;
                    min_indent = std::min(min_indent, p);
                    if (ln.slice(p).size() < 2 || ln[p] != '/' || ln[p + 1] != '/')
                        all_commented = false;
                }
                if (!any_non_blank)
                    min_indent = 0;
                for (std::size_t r = r0; r <= r1; ++r) {
                    const StringView ln = buffer.line(r);
                    const std::size_t p = ln.find_first_not_of(" \t");
                    if (p == StringView::npos) {
                        if (r == r0)
                            edit_col_r0 = ln.size();
                        (void)replacement.append(ln);
                    } else if (all_commented) {
                        if (r == r0)
                            edit_col_r0 = p;
                        (void)replacement.append(ln.slice(0, p));
                        const std::size_t skip = (p + 2 < ln.size() && ln[p + 2] == ' ') ? 3u : 2u;
                        (void)replacement.append(ln.slice(p + skip));
                    } else {
                        const std::size_t ins = std::min(min_indent, ln.size());
                        if (r == r0)
                            edit_col_r0 = ins;
                        (void)replacement.append(ln.slice(0, ins));
                        (void)replacement.append("// ");
                        (void)replacement.append(ln.slice(ins));
                    }
                    if (r != r1)
                        (void)replacement.append("\n");
                }
            }
            const bool single_caret = first == last && r0 == r1;
            const std::uint32_t old_len = range_end - range_start;
            buffer.select({range_start, false}, false);
            buffer.select({range_end, true}, true);
            insert(reply, replacement.view(), detail::TextEdit::Group::isolated);
            if (!reply.error) {
                const std::uint32_t new_end = range_start + static_cast<std::uint32_t>(replacement.size());
                if (single_caret) {
                    const std::uint32_t anchor_off = range_start + static_cast<std::uint32_t>(edit_col_r0);
                    std::uint32_t next_c = std::min(first, new_end);
                    if (first > anchor_off) {
                        const std::int64_t diff =
                            static_cast<std::int64_t>(replacement.size()) - static_cast<std::int64_t>(old_len);
                        next_c = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
                            static_cast<std::int64_t>(first) + diff, std::min(anchor_off, new_end), new_end));
                    }
                    buffer.select({next_c, true}, false);
                } else {
                    buffer.select({range_start, false}, false);
                    buffer.select({new_end, true}, true);
                }
                revise();
            }
            reply.consumed = reply.redraw = true;
            return true;
        }

        // Ctrl+Shift+K : delete line(s).
        if (event.control && event.shift && event.key == Key::k) {
            if (state->control.read_only) {
                reply.consumed = true;
                return true;
            }
            const auto [r0, r1] = selected_line_span(buffer, first, last);
            std::uint32_t del_start = buffer.line_start(r0);
            std::uint32_t del_end = buffer.line_start(r1) + static_cast<std::uint32_t>(buffer.line(r1).size());
            if (del_end < selected.text.size() && selected.text[del_end] == '\n')
                ++del_end;
            else if (del_start > 0 && selected.text[del_start - 1] == '\n')
                --del_start;
            buffer.select({del_start, false}, false);
            buffer.select({del_end, true}, true);
            insert(reply, "", detail::TextEdit::Group::isolated);
            reply.consumed = reply.redraw = true;
            return true;
        }

        // Ctrl+D : select word at caret, or select next matching occurrence.
        if (event.control && !event.shift && event.key == Key::d) {
            const CodeLanguage lang = state->control.code->language();
            if (first == last) {
                const std::uint32_t ls = buffer.line_start(row);
                const auto wr = detail::word_range_at(buffer.line(row), first - ls, lang);
                if (wr.second > wr.first) {
                    buffer.select({ls + wr.first, false}, false);
                    buffer.select({ls + wr.second, true}, true);
                    revise();
                }
            } else {
                const StringView needle = selected.text.slice(first, last - first);
                bool all_ident = !needle.empty();
                for (std::size_t i = 0; i < needle.size() && all_ident; ++i)
                    all_ident = is_code_ident_char(needle[i], lang);
                const bool whole_ident =
                    all_ident && (first == 0 || !is_code_ident_char(selected.text[first - 1], lang)) &&
                    (last == selected.text.size() || !is_code_ident_char(selected.text[last], lang));
                const auto find_from = [&](std::size_t from, std::size_t limit) -> std::size_t {
                    std::size_t p = from;
                    while (p < limit) {
                        const std::size_t hit_p = selected.text.find(needle, p);
                        if (hit_p == StringView::npos || hit_p >= limit)
                            return StringView::npos;
                        if (!whole_ident)
                            return hit_p;
                        const bool left_ok = hit_p == 0 || !is_code_ident_char(selected.text[hit_p - 1], lang);
                        const std::size_t end_p = hit_p + needle.size();
                        const bool right_ok =
                            end_p == selected.text.size() || !is_code_ident_char(selected.text[end_p], lang);
                        if (left_ok && right_ok)
                            return hit_p;
                        p = hit_p + 1;
                    }
                    return StringView::npos;
                };
                std::size_t pos = find_from(last, selected.text.size());
                if (pos == StringView::npos && first > 0)
                    pos = find_from(0, first);
                if (pos != StringView::npos && pos != first) {
                    buffer.select({static_cast<std::uint32_t>(pos), false}, false);
                    buffer.select({static_cast<std::uint32_t>(pos + needle.size()), true}, true);
                    revise();
                }
            }
            reply.consumed = reply.redraw = true;
            return true;
        }

        // Smart Backspace: delete empty auto-pair (), [], {}, "" or 4-space soft-tab indent.
        if (!event.control && !event.shift && event.key == Key::backspace && !state->control.read_only &&
            first == last && first > 0) {
            const char prev = selected.text[first - 1];
            const char next = first < selected.text.size() ? selected.text[first] : '\0';
            const bool empty_pair =
                (prev == '(' && next == ')') || (prev == '[' && next == ']') ||
                (prev == '{' && next == '}') || (prev == '"' && next == '"');
            if (empty_pair) {
                buffer.select({first - 1, false}, false);
                buffer.select({first + 1, true}, true);
                insert(reply, "", detail::TextEdit::Group::backspace);
                reply.consumed = reply.redraw = true;
                return true;
            }
            const std::uint32_t ls = buffer.line_start(row);
            const std::uint32_t col = first - ls;
            const StringView prefix_ln = buffer.line(row).slice(0, col);
            if (col >= 4 && detail::is_blank_line(prefix_ln) && !prefix_ln.contains('\t')) {
                const std::uint32_t step = col % 4 == 0 ? 4u : (col % 4);
                if (step > 1) {
                    buffer.select({first - step, false}, false);
                    buffer.select({first, true}, true);
                    insert(reply, "", detail::TextEdit::Group::backspace);
                    reply.consumed = reply.redraw = true;
                    return true;
                }
            }
        }

        if (!event.control && event.key == Key::enter) {
            if (state->control.read_only) {
                reply.consumed = true;
                return true;
            }
            const StringView line = buffer.line(row);
            const std::uint32_t ls = buffer.line_start(row);
            const std::size_t caret_in_line = first >= ls ? std::min<std::size_t>(first - ls, line.size()) : 0;
            const std::size_t indent_end = line.find_first_not_of(" \t");
            const StringView base_indent = line.slice(
                0, std::min<std::size_t>(indent_end == StringView::npos ? line.size() : indent_end, caret_in_line));
            std::size_t trim_prev = caret_in_line;
            while (trim_prev > 0 && (line[trim_prev - 1] == ' ' || line[trim_prev - 1] == '\t'))
                --trim_prev;
            const char prev_non_ws = trim_prev > 0 ? line[trim_prev - 1] : '\0';
            const char next_ch = last < selected.text.size() ? selected.text[last] : '\0';
            if (prev_non_ws == '{' && next_ch == '}') {
                String inner = String::literal<"\n">();
                (void)inner.append(base_indent);
                (void)inner.append("    ");
                String full = inner;
                (void)full.append("\n");
                (void)full.append(base_indent);
                insert(reply, full.view(), detail::TextEdit::Group::isolated);
                if (!reply.error && buffer.select({first + static_cast<std::uint32_t>(inner.size()), true}, false))
                    revise();
            } else if (prev_non_ws == '{' || prev_non_ws == '(' || prev_non_ws == '[') {
                String ins = String::literal<"\n">();
                (void)ins.append(base_indent);
                (void)ins.append("    ");
                insert(reply, ins.view(), detail::TextEdit::Group::isolated);
            } else {
                String ins = String::literal<"\n">();
                (void)ins.append(base_indent);
                insert(reply, ins.view(), detail::TextEdit::Group::isolated);
            }
        } else if (!event.control && event.key == Key::tab) {
            if (first == last && !event.shift)
                insert(reply, "    ", detail::TextEdit::Group::isolated);
            else {
                const auto [first_row, last_row] = selected_line_span(buffer, first, last);
                const auto start = buffer.line_start(first_row);
                const auto end =
                    buffer.line_start(last_row) + static_cast<std::uint32_t>(buffer.line(last_row).size());
                String replacement;
                for (auto current = first_row; current <= last_row; ++current) {
                    StringView value = buffer.line(current);
                    if (event.shift) {
                        std::size_t count{};
                        if (!value.empty() && value[0] == '\t')
                            count = 1;
                        else
                            while (count < 4 && count < value.size() && value[count] == ' ')
                                ++count;
                        value.remove_prefix(count);
                    } else if (!value.empty()) {
                        (void)replacement.append("    ");
                    }
                    (void)replacement.append(value);
                    if (current != last_row)
                        (void)replacement.append("\n");
                }
                buffer.select({start, false}, false);
                buffer.select({end, true}, true);
                insert(reply, replacement.view(), detail::TextEdit::Group::isolated);
            }
        } else if (event.key == Key::left || event.key == Key::right || event.key == Key::up ||
                   event.key == Key::down || event.key == Key::home || event.key == Key::end ||
                   event.key == Key::page_up || event.key == Key::page_down) {
            auto offset = selected.caret.offset;
            const auto length = static_cast<std::uint32_t>(selected.text.size());
            const bool backwards =
                event.key == Key::left || event.key == Key::up || event.key == Key::home || event.key == Key::page_up;
            if ((event.key == Key::left || event.key == Key::right) && !event.shift &&
                selected.caret.offset != selected.anchor.offset)
                offset = backwards ? std::min(selected.caret.offset, selected.anchor.offset)
                                   : std::max(selected.caret.offset, selected.anchor.offset);
            else if (event.key == Key::home || event.key == Key::end) {
                if (event.control) {
                    offset = backwards ? 0 : length;
                } else if (backwards) {
                    const std::uint32_t ls = buffer.line_start(row);
                    const StringView ln = buffer.line(row);
                    const std::size_t non_ws = ln.find_first_not_of(" \t");
                    const std::uint32_t smart_home =
                        ls + static_cast<std::uint32_t>(non_ws == StringView::npos ? ln.size() : non_ws);
                    offset = (offset == smart_home && smart_home > ls) ? ls : smart_home;
                } else {
                    offset = buffer.line_start(row) + static_cast<std::uint32_t>(buffer.line(row).size());
                }
            } else if (event.key == Key::left || event.key == Key::right) {
                if (event.control) {
                    const auto char_cls = [](char c) noexcept -> int {
                        const auto u = static_cast<unsigned char>(c);
                        if (u == ' ' || u == '\t')
                            return 0;
                        if (u == '\n')
                            return 1;
                        if (is_ascii_ident_continue(c) || u >= 0x80u)
                            return 2;
                        return 3;
                    };
                    if (backwards) {
                        if (offset > 0 && selected.text[offset - 1] == '\n') {
                            offset = buffer.snap(offset - 1, false).offset;
                        } else {
                            while (offset > 0 && char_cls(selected.text[offset - 1]) == 0)
                                --offset;
                            if (offset > 0 && char_cls(selected.text[offset - 1]) > 1) {
                                const int cls = char_cls(selected.text[offset - 1]);
                                while (offset > 0 && char_cls(selected.text[offset - 1]) == cls)
                                    offset = buffer.snap(offset - 1, false).offset;
                            }
                        }
                    } else {
                        if (offset < length && selected.text[offset] == '\n') {
                            offset = buffer.snap(offset + 1, true).offset;
                        } else {
                            while (offset < length && char_cls(selected.text[offset]) == 0)
                                ++offset;
                            if (offset < length && char_cls(selected.text[offset]) > 1) {
                                const int cls = char_cls(selected.text[offset]);
                                while (offset < length && char_cls(selected.text[offset]) == cls)
                                    offset = buffer.snap(offset + 1, true).offset;
                            }
                        }
                    }
                } else {
                    const auto boundaries = buffer.boundaries();
                    auto at = std::lower_bound(boundaries.begin(), boundaries.end(), offset);
                    if (backwards && at != boundaries.begin())
                        offset = *--at;
                    else if (!backwards && at + 1 != boundaries.end())
                        offset = *++at;
                }
            } else {
                std::size_t distance = 1;
                if (event.key == Key::page_up || event.key == Key::page_down) {
                    if (const auto* geometry = published->find_code(focus))
                        distance = std::max<std::size_t>(
                            1, static_cast<std::size_t>(geometry->body.height / geometry->line_height));
                }
                const auto destination = backwards ? (row > distance ? row - distance : 0)
                                                   : std::min(row + distance, buffer.line_count() - 1);
                const auto raw_target =
                    buffer.line_start(destination) +
                    static_cast<std::uint32_t>(
                        std::min<std::size_t>(offset - buffer.line_start(row), buffer.line(destination).size()));
                offset = buffer.snap(raw_target, false).offset;
            }
            if (buffer.select({offset, !backwards}, event.shift))
                revise();
        } else {
            return false;
        }
        reply.consumed = reply.redraw = true;
        return true;
    }
