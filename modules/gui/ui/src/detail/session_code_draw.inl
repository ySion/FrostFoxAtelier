                auto& document = *state.control.code;
                const CodeAnnotations* annotations = state.control.code_annotations.get();
                const CodeLanguage lang = document.language();
                if (state.code_revision != document.revision()) {
                    state.code_width = 0;
                    state.code_revision = document.revision();
                }
                const auto& buffer = document.edit();
                const auto& selection = buffer.current();
                self.ensure_code_block_comments(state, buffer, lang);

                const auto& pal = detail::code_palette();
                const auto line_height = std::ceil(appearance.font.size * 1.5f);
                const auto gutter = (appearance.scrollbar_width + 2 * appearance.scrollbar_inset) * scale;
                const auto total_num_str = String::from_number(buffer.line_count());
                const auto digits = std::max<std::size_t>(2, total_num_str->size());
                const auto margin = (20.f + static_cast<float>(digits) * appearance.font.size * .62f) * scale;
                Rect body{bounds.x + margin, bounds.y + 2 * scale, std::max(0.f, bounds.width - margin - gutter),
                          std::max(0.f, bounds.height - 4 * scale - gutter)};
                const auto body_clip = intersect(clip, clip_rect(body));
                const auto gutter_clip = intersect(clip, clip_rect({bounds.x, bounds.y, margin, bounds.height}));
                if (paint) {
                    quad({bounds.x, bounds.y, margin, bounds.height}, gutter_clip, pal.gutter_bg);
                    const float stroke = std::max(1.f, std::round(scale * .667f));
                    quad({std::round(bounds.x + margin - stroke), bounds.y, stroke, bounds.height}, gutter_clip,
                         pal.gutter_divider);
                }

                const auto editing = self.edit && self.edit->key == key;
                const auto* composition = editing && self.edit->composition ? &*self.edit->composition : nullptr;
                const auto caret_offset = composition ? composition->first : selection.caret.offset;
                const auto caret_row = buffer.line_at(caret_offset);
                const std::size_t comp_first_row = composition ? buffer.line_at(composition->first) : 0;
                const std::size_t comp_last_row = composition ? buffer.line_at(composition->last) : 0;
                String composed_scratch;
                const auto row_view = [&](std::size_t row) -> StringView {
                    if (!composition)
                        return buffer.line(row);
                    if (row == comp_first_row) {
                        composed_scratch.clear();
                        (void)composed_scratch.append(
                            buffer.line(row).slice(0, composition->first - buffer.line_start(row)));
                        (void)composed_scratch.append(composition->preedit.text);
                        (void)composed_scratch.append(
                            buffer.line(comp_last_row).slice(composition->last - buffer.line_start(comp_last_row)));
                        return composed_scratch.view();
                    }
                    if (row > comp_first_row && row <= comp_last_row)
                        return {};
                    return buffer.line(row);
                };
                std::vector<detail::TextKey> visible_text;
                const auto cache_line = [&](std::size_t row) {
                    const detail::TextKey text_key{key, row + 1, false};
                    self.text.set(text_key, row_view(row), appearance.font, false);
                    visible_text.push_back(text_key);
                    return text_key;
                };
                const auto caret_key = cache_line(caret_row);
                ATELIER_TRY_BIND(caret_paragraph, self.text.snapshot(caret_key, 0));
                auto caret_position = selection.caret;
                caret_position.offset = caret_offset - buffer.line_start(caret_row);
                if (composition)
                    caret_position.offset += composition->preedit.cursor;
                ATELIER_TRY_BIND(caret_result, (*caret_paragraph)->caret(caret_position));
                const auto& caret = *caret_result;
                state.scroll_limit =
                    std::max(0.0, static_cast<double>(buffer.line_count() * line_height - body.height / scale));
                if (state.code_selection_revision != buffer.selection_revision() && body.height > 0 && body.width > 0) {
                    const auto y = caret_row * line_height;
                    if (y < state.scroll)
                        state.scroll = y;
                    if (y + line_height > state.scroll + body.height / scale)
                        state.scroll = y + line_height - body.height / scale;
                    if (caret.x < state.scroll_x)
                        state.scroll_x = caret.x;
                    if (caret.x + appearance.caret_width > state.scroll_x + body.width / scale)
                        state.scroll_x = caret.x + appearance.caret_width - body.width / scale;
                    state.code_selection_revision = buffer.selection_revision();
                }
                state.scroll = std::clamp(state.scroll, 0.0, state.scroll_limit);
                CodeGeometry code{key, document.revision(), buffer.selection_revision(), body, line_height * scale,
                                  scale, {}};
                const auto first_row =
                    std::min(buffer.line_count() - 1, static_cast<std::size_t>(state.scroll / line_height));
                const auto end_row =
                    std::min(buffer.line_count(),
                             first_row + static_cast<std::size_t>(std::ceil(body.height / (line_height * scale))) + 1);
                ATELIER_TRY_BIND(caret_metrics, self.text.metrics(caret_key, 0));
                float width = caret_metrics->width;
                for (auto row = first_row; row < end_row; ++row) {
                    if (row == caret_row)
                        continue;
                    const auto line_key = cache_line(row);
                    ATELIER_TRY_BIND(metrics, self.text.metrics(line_key, 0));
                    width = std::max(width, metrics->width);
                }
                state.code_width = std::max(state.code_width, width + appearance.caret_width + 16);
                state.scroll_x_limit = std::max(0.0, static_cast<double>(state.code_width - body.width / scale));
                state.scroll_x = std::clamp(state.scroll_x, 0.0, state.scroll_x_limit);

                const auto first = std::min(selection.caret.offset, selection.anchor.offset);
                const auto last = std::max(selection.caret.offset, selection.anchor.offset);

                // Resolve bracket matching, active indent scope, and caret identifier for occurrence highlighting.
                Optional<detail::CodeBracketMatch> bracket_match;
                detail::ActiveIndentScope active_scope{0, 0, std::numeric_limits<std::uint32_t>::max()};
                if (editing && !composition && lang != CodeLanguage::plain) {
                    if (state.code_scope_revision != document.revision() || state.code_scope_lang != lang ||
                        state.code_scope_caret != caret_offset) {
                        state.code_bracket_match = detail::find_matching_bracket(
                            buffer, lang, state.code_block_comments.view(), caret_offset);
                        state.code_active_scope = detail::find_active_indent_scope(
                            buffer, lang, state.code_block_comments.view(), caret_offset);
                        state.code_scope_revision = document.revision();
                        state.code_scope_lang = lang;
                        state.code_scope_caret = caret_offset;
                    }
                    bracket_match = state.code_bracket_match;
                    active_scope = state.code_active_scope;
                }

                StringView caret_word{};
                if (editing && !composition) {
                    if (first == last) {
                        const StringView c_ln = buffer.line(caret_row);
                        const std::uint32_t c_ls = buffer.line_start(caret_row);
                        const auto wr = detail::word_range_at(c_ln, caret_offset - c_ls, lang);
                        if (wr.second - wr.first >= 2 && is_ascii_ident_start(c_ln[wr.first]))
                            caret_word = c_ln.slice(wr.first, wr.second - wr.first);
                    } else if (last - first >= 2 && last - first <= 64 &&
                               buffer.line_at(first) == buffer.line_at(last)) {
                        const StringView cand = selection.text.slice(first, last - first);
                        bool valid_ident = is_ascii_ident_start(cand.front());
                        for (std::size_t ci = 1; ci < cand.size() && valid_ident; ++ci)
                            valid_ident = is_code_ident_char(cand[ci], lang);
                        if (valid_ident)
                            caret_word = cand;
                    }
                }

                // Measure monospace space advance once for crisp 4-space indent guides.
                const float space_advance_dip = appearance.font.size * .6f;
                Array<detail::CodeTokenSpan> token_spans;
                Array<detail::CodeHexSwatch> swatches;
                Font lens_font = appearance.font;
                lens_font.size = std::max(10.f, appearance.font.size - 1.f);
                lens_font.style = FontStyle::italic;

                for (auto row = first_row; row < end_row; ++row) {
                    const detail::TextKey line_key{key, row + 1, false};
                    const detail::TextKey number_key{key, row + 1, true};
                    ATELIER_TRY_BIND(paragraph_result, self.text.snapshot(line_key, 0));
                    const auto& paragraph = *paragraph_result;
                    const auto x = body.x - static_cast<float>(state.scroll_x) * scale;
                    const auto y = body.y + (row * line_height - static_cast<float>(state.scroll)) * scale;
                    const StringView rendered_line = row_view(row);
                    const auto start = buffer.line_start(row);
                    const auto end = start + static_cast<std::uint32_t>(composition ? rendered_line.size()
                                                                                    : buffer.line(row).size());
                    code.lines.push_back({row, start, paragraph, x, y});

                    // 1. Active line background in body and gutter.
                    if (paint && row == caret_row && editing) {
                        quad({body.x, y, body.width, line_height * scale}, body_clip, pal.active_line_bg);
                        quad({bounds.x, y, margin - std::max(1.f, std::round(scale * .667f)), line_height * scale},
                             gutter_clip, pal.active_gutter_bg);
                    }

                    // 2. Indent guides (4-space columns; interpolate blank lines from neighbors).
                    if (paint && lang != CodeLanguage::plain) {
                        std::uint32_t indent_spaces = detail::line_indent_spaces(rendered_line);
                        if (detail::is_blank_line(rendered_line)) {
                            std::size_t prev_r = row;
                            while (prev_r > 0 && detail::is_blank_line(buffer.line(prev_r - 1)))
                                --prev_r;
                            std::size_t next_r = row + 1;
                            while (next_r < buffer.line_count() && detail::is_blank_line(buffer.line(next_r)))
                                ++next_r;
                            const std::uint32_t p_ind =
                                prev_r > 0 ? detail::line_indent_spaces(buffer.line(prev_r - 1)) : 0;
                            const std::uint32_t n_ind =
                                next_r < buffer.line_count() ? detail::line_indent_spaces(buffer.line(next_r)) : 0;
                            indent_spaces = std::max(p_ind, n_ind);
                        }
                        const float stroke = std::max(1.f, std::round(scale * .667f));
                        for (std::uint32_t col = 4; col < indent_spaces; col += 4) {
                            const std::uint32_t guide_col = col - 4;
                            const bool is_active_guide = guide_col == active_scope.guide_column &&
                                                         row > active_scope.start_row && row <= active_scope.end_row;
                            const float gx = std::round(x + static_cast<float>(guide_col) * space_advance_dip * scale);
                            quad({gx, y, stroke, line_height * scale}, body_clip,
                                 is_active_guide ? pal.indent_guide_active : pal.indent_guide);
                        }
                    }

                    // 3. Word occurrence highlights (when not covered by primary selection).
                    if (paint && !caret_word.empty() && !rendered_line.empty() && !composition) {
                        std::size_t pos = 0;
                        while ((pos = rendered_line.find(caret_word, pos)) != StringView::npos) {
                            const auto w0 = static_cast<std::uint32_t>(pos);
                            const auto w1 = w0 + static_cast<std::uint32_t>(caret_word.size());
                            const bool left_ok = w0 == 0 || !is_code_ident_char(rendered_line[w0 - 1], lang);
                            const bool right_ok =
                                w1 == rendered_line.size() || !is_code_ident_char(rendered_line[w1], lang);
                            if (left_ok && right_ok && !(first != last && start + w0 == first && start + w1 == last)) {
                                ATELIER_TRY_BIND(occ_ranges, paragraph->selection(w0, w1));
                                for (const auto& r : *occ_ranges) {
                                    const Rect box_r{x + r.x * scale, y + scale, r.width * scale,
                                                     line_height * scale - 2 * scale};
                                    draws->draws.push_back({box_r,
                                                            body_clip,
                                                            pal.occurrence_bg,
                                                            {0, 0, 1, 1},
                                                            {},
                                                            {scale * 1.5f, scale * 1.5f, scale * 1.5f, scale * 1.5f},
                                                            std::max(1.f, std::round(scale * .667f)),
                                                            pal.occurrence_border});
                                }
                            }
                            pos = w1;
                        }
                    }

                    // 4. Find / Search match highlights.
                    if (paint && annotations && !annotations->search_matches.empty() && !composition) {
                        auto it = std::lower_bound(annotations->search_matches.begin(),
                                                   annotations->search_matches.end(), start,
                                                   [](const CodeSearchMatch& m, std::uint32_t s) { return m.end <= s; });
                        for (; it != annotations->search_matches.end() && it->start < end; ++it) {
                            const std::uint32_t m0 = std::max(it->start, start) - start;
                            const std::uint32_t m1 = std::min(it->end, end) - start;
                            if (m1 <= m0)
                                continue;
                            const auto match_idx =
                                static_cast<std::uint32_t>(it - annotations->search_matches.begin());
                            const bool active_m = match_idx == annotations->active_search_match;
                            ATELIER_TRY_BIND(m_ranges, paragraph->selection(m0, m1));
                            for (const auto& r : *m_ranges) {
                                const Rect box_r{x + r.x * scale, y + scale, r.width * scale,
                                                 line_height * scale - 2 * scale};
                                draws->draws.push_back({box_r,
                                                        body_clip,
                                                        active_m ? pal.search_active_bg : pal.search_bg,
                                                        {0, 0, 1, 1},
                                                        {},
                                                        {scale * 1.5f, scale * 1.5f, scale * 1.5f, scale * 1.5f},
                                                        std::max(1.f, std::round(scale * .667f)),
                                                        active_m ? pal.search_active_border : pal.search_border});
                            }
                        }
                    }

                    // 5. Primary selection highlight.
                    if (paint && first != last && first <= end && last >= start && !composition) {
                        ATELIER_TRY_BIND(ranges,
                                         paragraph->selection(std::max(first, start) - start, std::min(last, end) - start));
                        for (auto range : *ranges)
                            quad({x + range.x * scale, y + range.y * scale, range.width * scale, line_height * scale},
                                 body_clip, appearance.selection);
                        if (last > end) {
                            ATELIER_TRY_BIND(line_end, paragraph->caret({end - start, true}));
                            quad({x + line_end->x * scale, y, appearance.font.size * .6f * scale, line_height * scale},
                                 body_clip, appearance.selection);
                        }
                    }

                    // 6. Bracket matching box.
                    if (paint && bracket_match && !composition) {
                        const auto draw_bracket = [&](std::uint32_t b_off) -> Result<void> {
                            if (b_off < start || b_off >= end)
                                return {};
                            ATELIER_TRY_BIND(b_ranges, paragraph->selection(b_off - start, b_off - start + 1));
                            for (const auto& r : *b_ranges) {
                                const Rect br{x + r.x * scale, y + scale, std::max(4 * scale, r.width * scale),
                                              line_height * scale - 2 * scale};
                                draws->draws.push_back(
                                    {br,
                                     body_clip,
                                     bracket_match->matched ? pal.bracket_bg : Color{0, 0, 0, 0},
                                     {0, 0, 1, 1},
                                     {},
                                     {scale, scale, scale, scale},
                                     std::max(1.f, std::round(scale * .667f)),
                                     bracket_match->matched ? pal.bracket_border : pal.bracket_unmatched});
                            }
                            return {};
                        };
                        ATELIER_TRY(draw_bracket(bracket_match->first));
                        if (bracket_match->second != bracket_match->first)
                            ATELIER_TRY(draw_bracket(bracket_match->second));
                    }

                    // 7. Single-Coverage multi-color syntax-highlighted text rendering + inline .ss color swatches!
                    if (paint) {
                        ATELIER_TRY_BIND(
                            raster,
                            self.text.raster(line_key, 0, scale,
                                             clip_rect({static_cast<float>(body_clip.left) - x,
                                                        static_cast<float>(body_clip.top) - y,
                                                        static_cast<float>(body_clip.right - body_clip.left),
                                                        static_cast<float>(body_clip.bottom - body_clip.top)})));
                        if (raster->coverage) {
                            const Rect glyph_rect{x + raster->left, y + raster->top,
                                                  static_cast<float>(raster->coverage->width),
                                                  static_cast<float>(raster->coverage->height)};
                            const bool in_blk =
                                row < state.code_block_comments.size() && state.code_block_comments[row] != 0;
                            detail::tokenize_code_line(rendered_line, lang, in_blk, annotations, token_spans, swatches);
                            if (token_spans.size() <= 1) {
                                const Color fg = token_spans.empty() ? appearance.foreground : token_spans.front().color;
                                quad(glyph_rect, body_clip, fg, raster->coverage);
                            } else {
                                std::int32_t prev_right = body_clip.left;
                                for (std::size_t ti = 0; ti < token_spans.size(); ++ti) {
                                    const auto& sp = token_spans[ti];
                                    std::int32_t next_right = body_clip.right;
                                    if (ti + 1 < token_spans.size()) {
                                        ATELIER_TRY_BIND(edge_caret, paragraph->caret({sp.end, false}));
                                        next_right = std::clamp<std::int32_t>(
                                            static_cast<std::int32_t>(std::lround(x + edge_caret->x * scale)),
                                            prev_right, body_clip.right);
                                    }
                                    if (next_right > prev_right) {
                                        Clip span_clip = body_clip;
                                        span_clip.left = prev_right;
                                        span_clip.right = next_right;
                                        quad(glyph_rect, span_clip, sp.color, raster->coverage);
                                        prev_right = next_right;
                                    }
                                }
                            }
                            // Inline .ss color preview swatches.
                            for (const auto& sw : swatches) {
                                ATELIER_TRY_BIND(sw_end, paragraph->caret({sw.end, false}));
                                const float sz = std::round(9.5f * scale);
                                const Rect sw_rect{std::round(x + sw_end->x * scale + 2.f * scale),
                                                   std::round(y + (line_height * scale - sz) * .5f), sz, sz};
                                draws->draws.push_back({sw_rect,
                                                        body_clip,
                                                        sw.color,
                                                        {0, 0, 1, 1},
                                                        {},
                                                        {scale, scale, scale, scale},
                                                        std::max(1.f, std::round(scale * .667f)),
                                                        pal.gutter_divider});
                            }
                        }
                    }

                    // 8. Ctrl+Hover navigable symbol underline.
                    if (paint && annotations && annotations->link_range && !composition) {
                        const auto [l0, l1] = *annotations->link_range;
                        if (l0 < end && l1 > start) {
                            ATELIER_TRY_BIND(
                                l_ranges, paragraph->selection(std::max(l0, start) - start, std::min(l1, end) - start));
                            const float stroke = std::max(1.f, std::round(scale));
                            for (const auto& r : *l_ranges)
                                quad({x + r.x * scale, y + line_height * scale - 2 * stroke, r.width * scale, stroke},
                                     body_clip, pal.link_underline);
                        }
                    }

                    // 9. Diagnostics on this row: wavy squiggle + gutter badge + inline Error Lens!
                    const CodeDiagnostic* primary_diag = nullptr;
                    if (annotations && !annotations->diagnostics.empty()) {
                        for (const auto& diag : annotations->diagnostics) {
                            if (diag.line != row)
                                continue;
                            if (!primary_diag || diag.severity < primary_diag->severity)
                                primary_diag = &diag;
                            if (paint && !composition) {
                                const auto line_len = static_cast<std::uint32_t>(rendered_line.size());
                                std::uint32_t d0 = std::min(diag.col_start, line_len);
                                std::uint32_t d1 = diag.col_end > d0 ? std::min(diag.col_end, line_len) : d0;
                                if (d1 == d0 && line_len > 0) {
                                    const auto wr = detail::word_range_at(rendered_line, d0, lang);
                                    d0 = wr.first;
                                    d1 = wr.second > wr.first ? wr.second : std::min(line_len, d0 + 1);
                                }
                                Color sq_col = diag.severity == CodeDiagnosticSeverity::error     ? pal.error_color
                                               : diag.severity == CodeDiagnosticSeverity::warning ? pal.warning_color
                                                                                                  : pal.info_color;
                                float sx0 = x;
                                float sx1 = x + 12.f * scale;
                                if (d1 > d0) {
                                    ATELIER_TRY_BIND(d_ranges, paragraph->selection(d0, d1));
                                    if (!d_ranges->empty()) {
                                        sx0 = x + d_ranges->front().x * scale;
                                        sx1 = x + (d_ranges->back().x + d_ranges->back().width) * scale;
                                    }
                                } else {
                                    ATELIER_TRY_BIND(dc, paragraph->caret({d0, false}));
                                    sx0 = x + dc->x * scale;
                                    sx1 = sx0 + std::max(8.f * scale, space_advance_dip * scale);
                                }
                                const float step_w = std::max(2.f, std::round(2.5f * scale));
                                const float stroke_h = std::max(1.f, std::round(1.1f * scale));
                                const float base_y = std::round(y + line_height * scale - 2.5f * scale);
                                bool up_zig = false;
                                for (float wx = sx0; wx < sx1; wx += step_w) {
                                    const float seg_w = std::min(step_w, sx1 - wx);
                                    quad({wx, base_y + (up_zig ? -stroke_h : 0.f), seg_w, stroke_h}, body_clip, sq_col);
                                    up_zig = !up_zig;
                                }
                            }
                        }
                    }

                    if (paint && primary_diag && !primary_diag->message.empty()) {
                        // Gutter severity dot.
                        const Color badge_col = primary_diag->severity == CodeDiagnosticSeverity::error ? pal.error_color
                                                : primary_diag->severity == CodeDiagnosticSeverity::warning
                                                    ? pal.warning_color
                                                    : pal.info_color;
                        const float dot_sz = std::round(5.5f * scale);
                        draws->draws.push_back({{std::round(bounds.x + 4.f * scale),
                                                 std::round(y + (line_height * scale - dot_sz) * .5f), dot_sz, dot_sz},
                                                gutter_clip,
                                                badge_col,
                                                {0, 0, 1, 1},
                                                {},
                                                {dot_sz * .5f, dot_sz * .5f, dot_sz * .5f, dot_sz * .5f}});
                        // Inline Error Lens at end of line!
                        ATELIER_TRY_BIND(eol_caret,
                                         paragraph->caret({static_cast<std::uint32_t>(rendered_line.size()), true}));
                        const float lens_x = std::round(x + eol_caret->x * scale + 18.f * scale);
                        if (lens_x < static_cast<float>(body_clip.right) - 24.f * scale) {
                            String lens_msg = String::literal<"  \xe2\x80\x94 ">();
                            const std::size_t nl = primary_diag->message.find('\n');
                            const StringView first_ln = primary_diag->message.slice(
                                0, nl == StringView::npos ? primary_diag->message.size() : nl);
                            if (first_ln.size() > 96) {
                                (void)lens_msg.append(first_ln.utf8_prefix(95));
                                (void)lens_msg.append(String::literal<"\xe2\x80\xa6">());
                            } else {
                                (void)lens_msg.append(first_ln);
                            }
                            const detail::TextKey lens_key{key, (1ULL << 32) | (row + 1), true};
                            self.text.set(lens_key, lens_msg.view(), lens_font, false);
                            visible_text.push_back(lens_key);
                            ATELIER_TRY_BIND(lens_raster, self.text.raster(lens_key, 0, scale));
                            if (lens_raster->coverage) {
                                quad({lens_x + lens_raster->left, y + lens_raster->top + scale,
                                      static_cast<float>(lens_raster->coverage->width),
                                      static_cast<float>(lens_raster->coverage->height)},
                                     body_clip,
                                     primary_diag->severity == CodeDiagnosticSeverity::error ? pal.error_lens
                                                                                             : pal.warning_lens,
                                     lens_raster->coverage);
                            }
                        }
                    }

                    // 10. Gutter line number + unsaved line-diff bar.
                    const auto row_num_str = String::from_number(row + 1);
                    self.text.set(number_key, row_num_str->view(), appearance.font, false, TextAlign::right);
                    visible_text.push_back(number_key);
                    if (paint) {
                        ATELIER_TRY_BIND(numbers, self.text.raster(number_key, margin / scale - 10, scale));
                        if (numbers->coverage)
                            quad({bounds.x + numbers->left, y + numbers->top,
                                  static_cast<float>(numbers->coverage->width),
                                  static_cast<float>(numbers->coverage->height)},
                                 intersect(clip, clip_rect({bounds.x, body.y, margin - 6 * scale, body.height})),
                                 (row == caret_row && editing) ? pal.line_num_active : pal.line_num, numbers->coverage);
                        if (!state.control.read_only) {
                            const CodeLineDiff diff = document.line_diff(row);
                            if (diff != CodeLineDiff::clean) {
                                const float bar_w = std::max(2.f, std::round(2.f * scale));
                                quad({std::round(bounds.x + margin - bar_w - 2.f * scale), y, bar_w,
                                      line_height * scale},
                                     intersect(gutter_clip, clip_rect({bounds.x, body.y, margin, body.height})),
                                     diff == CodeLineDiff::added ? pal.diff_added : pal.diff_modified);
                            }
                        }
                    }

                    // 11. Caret & IME preedit anchor.
                    if (row == caret_row && editing) {
                        const Rect physical{x + caret.x * scale, y + caret.y * scale, appearance.caret_width * scale,
                                            std::max(caret.height, line_height) * scale};
                        if (paint)
                            quad(physical, body_clip, appearance.caret);
                        if (visible(physical, body_clip))
                            output->caret = TextCaret{self.edit->token, physical, body_clip};
                        ATELIER_TRY_BIND(anchor, composition ? paragraph->caret({composition->first - start, false})
                                                             : Result<Rect>{caret});
                        if (!state.control.read_only)
                            output->input_anchor = TextInputAnchor{
                                self.edit->token,
                                composition ? composition->id : 0,
                                composition ? composition->revision : 0,
                                {x + anchor->x * scale, y + anchor->y * scale, anchor->width * scale,
                                 line_height * scale},
                                body_clip};
                        if (paint && composition && !composition->preedit.text.empty()) {
                            ATELIER_TRY_BIND(ranges,
                                             paragraph->selection(composition->first - start,
                                                                  composition->first - start +
                                                                      static_cast<std::uint32_t>(
                                                                          composition->preedit.text.size())));
                            for (auto range : *ranges)
                                quad({x + range.x * scale, y + line_height * scale - scale, range.width * scale, scale},
                                     body_clip, appearance.caret);
                        }
                    }
                }
                output->code_editors.push_back(std::move(code));
                const Rect vbar_rect{bounds.x + bounds.width - gutter, body.y, appearance.scrollbar_width * scale,
                                     body.height};
                prepare_scrollbar(
                    {key, vbar_rect, {}, {}, {}, {}, clip, state.scroll_limit, state.scroll, body.height / scale, false},
                    appearance, enabled);
                prepare_scrollbar({key,
                                   {body.x, body.y + body.height + scale, body.width, appearance.scrollbar_width * scale},
                                   {},
                                   {},
                                   {},
                                   {},
                                   clip,
                                   state.scroll_x_limit,
                                   state.scroll_x,
                                   body.width / scale,
                                   true},
                                  appearance, enabled);

                // 12. Scrollbar Overview Ruler ticks (search matches, diagnostics, caret row).
                if (paint && body.height > 8.f * scale && buffer.line_count() > 0) {
                    const Clip ruler_clip = intersect(clip, clip_rect(vbar_rect));
                    const float total_lines = static_cast<float>(std::max<std::size_t>(1, buffer.line_count()));
                    const auto draw_tick = [&](std::size_t r_idx, Color col, float h_px) {
                        const float ratio = std::clamp(static_cast<float>(r_idx) / total_lines, 0.f, 1.f);
                        const float ty = std::round(vbar_rect.y + ratio * std::max(0.f, vbar_rect.height - h_px));
                        quad({vbar_rect.x + scale, ty, std::max(2.f, vbar_rect.width - 2 * scale), h_px}, ruler_clip,
                             col);
                    };
                    if (annotations) {
                        const std::size_t step = std::max<std::size_t>(1, annotations->search_matches.size() / 128);
                        for (std::size_t mi = 0; mi < annotations->search_matches.size(); mi += step)
                            draw_tick(buffer.line_at(annotations->search_matches[mi].start), pal.warning_color,
                                      std::max(1.5f, std::round(1.5f * scale)));
                        for (const auto& diag : annotations->diagnostics)
                            draw_tick(diag.line,
                                      diag.severity == CodeDiagnosticSeverity::error ? pal.error_color
                                                                                     : pal.warning_color,
                                      std::max(2.f, std::round(2.5f * scale)));
                    }
                    if (editing)
                        draw_tick(caret_row, Color{1.f, 1.f, 1.f, .65f}, std::max(1.5f, std::round(1.5f * scale)));
                }

                std::sort(visible_text.begin(), visible_text.end());
                visible_text.erase(std::unique(visible_text.begin(), visible_text.end()), visible_text.end());
                for (auto previous : state.table_text)
                    if (!std::binary_search(visible_text.begin(), visible_text.end(), previous))
                        self.text.erase(previous);
                state.table_text = std::move(visible_text);
