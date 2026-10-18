    bool dismiss_tooltip() noexcept {
        const bool had=tooltip_hover.has_value();
        tooltip_hover.reset();
        return had;
    }
    bool tooltip_blocked() const {
        return !active() || press || keyboard_pressed || popup || (published && published->popup) ||
            (edit && (edit->composition || edit->buffer().active()));
    }
    bool tooltip_current(const UiFrame& frame,const ControlBox& target) const {
        for (const auto* box=&target;box;) {
            const auto found=states.find(box->key);
            if (found==states.end()) return false;
            const auto& state=found->second;
            const auto& control=state.control;
            if (state.incarnation!=box->incarnation || control.kind!=box->kind || control.parent!=box->parent ||
                control.tooltip!=box->tooltip || control.action_target!=box->action_target ||
                control.context_target!=box->context_target || control.context_group!=box->context_group ||
                control.layout.collapsed) return false;
            if (!box->parent) return true;
            box=frame.find(box->parent);
            if (!box) return false;
        }
        return true;
    }
    const ControlBox* tooltip_target(const UiFrame& frame,float x,float y) const {
        // Bars are painted above their owners, so help must not pass through them.
        for (const auto& bar:frame.scrollbars)
            if (contains(bar.bounds,x,y) && contains(bar.clip,x,y)) return nullptr;
        for (auto it=frame.controls.rbegin();it!=frame.controls.rend();++it) {
            if (!contains(it->bounds,x,y) || !contains(it->clip,x,y)) continue;
            // A stale topmost control is still an occluder until its replacement is accepted.
            const auto* box=&*it;
            if (!tooltip_current(frame,*box)) return nullptr;
            while (box) {
                if (box->tooltip) {
                    const auto& help=*box->tooltip;
                    return help.title.empty() && help.identity.empty() && help.description.empty()?nullptr:box;
                }
                box=box->parent?frame.find(box->parent):nullptr;
            }
            return nullptr;
        }
        return nullptr;
    }
    void validate_tooltip(const UiFrame& frame) {
        if (!tooltip_hover) return;
        const auto& help=*tooltip_hover;
        const auto* box=tooltip_blocked()?nullptr:tooltip_target(frame,help.x,help.y);
        if (!box || box->key!=help.owner || box->incarnation!=help.incarnation || box->tooltip!=help.content ||
            box->bounds!=help.anchor || box->clip!=help.clip) dismiss_tooltip();
    }
    bool tooltip_input(const Event& event) {
        if (event.kind!=EventKind::pointer_move) {
            if (event.kind==EventKind::pointer_leave || event.kind==EventKind::pointer_down ||
                event.kind==EventKind::pointer_up || event.kind==EventKind::wheel ||
                event.kind==EventKind::key_down || event.kind==EventKind::key_up ||
                event.kind==EventKind::text || event.kind==EventKind::composition ||
                event.kind==EventKind::focus_lost || event.kind==EventKind::capture_lost) return dismiss_tooltip();
            return false;
        }
        const auto* box=tooltip_blocked()?nullptr:tooltip_target(*published,event.x,event.y);
        if (!box) return dismiss_tooltip();
        const auto now=std::chrono::steady_clock::now();
        const bool unchanged=tooltip_hover && tooltip_hover->owner==box->key &&
            tooltip_hover->incarnation==box->incarnation && tooltip_hover->content==box->tooltip &&
            tooltip_hover->anchor==box->bounds && tooltip_hover->clip==box->clip &&
            std::abs(event.x-tooltip_hover->x)<=2*surface.dpi_scale && std::abs(event.y-tooltip_hover->y)<=2*surface.dpi_scale;
        if (unchanged) return false;
        tooltip_hover=TooltipHover{box->key,box->incarnation,box->bounds,box->clip,box->tooltip,event.x,event.y,now,detail::new_identity()};
        return true;
    }
