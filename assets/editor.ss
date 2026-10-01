/* Native parameter styling, calibrated against Houdini's dark UI. */
@tokens {
    panel: #3d3d3d;
    field: #131313;
    text: #cccccc;
    focus: #deb36f;
    button-face: linear-gradient(#555555, #3b3b3b);
    button-hover: linear-gradient(#626262, #414141);
    button-down: linear-gradient(#4a5e73, #768ea7);
}
* { color: token(text); }
:role(panel) { background: token(panel); }
:role(caption) { color: #a3a3a3; }
.menubar {
    background-image: linear-gradient(#3d3d3d, #303030);
    border-color: #111111;
    border-inner-color: #575757;
    border-width: 0.667dip;
    border-style: solid;
}
button, combo {
    background-image: token(button-face);
    border-color: #252525;
    border-width: 0.667dip;
    border-style: solid;
    border-radius: 0dip;
    padding: 1dip 7dip;
}
button { text-align: center; }
button:hover, combo:hover { background-image: token(button-hover); color: #ffffff; }
button:pressed, combo:pressed { background-image: token(button-down); border-style: inset; color: #ffffff; }
button:selected { background-image: linear-gradient(#525b64, #3f474f); color: #ffffff; }
button.danger:hover { color: #f4c5bb; }
button:focus, combo:focus { border-color: token(focus); border-style: solid; }
number, text_edit {
    background: token(field);
    border-color: #242424;
    border-inner-color: #2e2e2e;
    border-width: 0.667dip;
    border-style: solid;
    border-radius: 0dip;
    padding: 1dip 3dip;
    selection-color: #75521b;
}
number { font-family: 'Source Code Pro'; }
number:hover, text_edit:hover { border-color: #3a3a3a; }
number:focus, text_edit:focus { border-color: token(focus); border-style: solid; }
number:invalid, text_edit:invalid { border-color: #ef7766; border-style: solid; }
number:modified:enabled { color: #eeeeee; font-weight: 700; }
text_edit:modified:enabled { color: #eeeeee; }
checkbox { padding: 1dip 0dip; indicator-size: 14dip; indicator-background: token(field); indicator-border-color: #3e3e3e; }
checkbox:selected:enabled { color: #dddddd; font-weight: 600; }
checkbox:hover:enabled { color: #dbaf61; }
checkbox:focus { border-color: token(focus); }
foldout { padding: 2dip 3dip; indicator-size: 10dip; }
foldout.section { background: #333333; border-width: 0dip; padding: 2dip 5dip; indicator-size: 12dip; font-weight: 400; }
foldout.section:hover { color: #eeeeee; }
foldout:focus { border-color: token(focus); border-style: solid; }
foldout.section:focus { border-width: 0.667dip; border-edges: bottom; }
combo { indicator-size: 14dip; indicator-color: #929292; combo-indicator: arrows; }
combo:disabled { indicator-color: #616161; }
.parameter-list { background: #3d3d3d; }
.parameter-page { padding: 4dip 0dip 0dip 2dip; background: #3a3a3a; }
.parameter-inset { padding: 0dip 0dip 0dip 8dip; }
.parameter-body { padding: 7.333dip 0dip 0dip 8dip; background: #3d3d3d; }
.parameter-nested-body { background: #363636; }
.parameter-pane { background: token(panel); }
.parameter-label { padding: 1dip 0dip; text-align: right; text-vertical-align: center; }
.field-label { text-vertical-align: center; }
.parameter-field { padding: 1dip 3dip; }
list, popup {
    scrollbar-color: #3b3b3b; scrollbar-width: 16dip; scrollbar-inset: 1dip; scrollbar-min-thumb: 24dip;
    scrollbar-button-size: 21.333dip; scrollbar-image: linear-gradient(to right, #555555, #3b3b3b);
    indicator-background: #262626; indicator-border-color: #494949;
}
list { background: #2d2d2d; }
item { padding: 2dip 6dip; }
item:hover { background: #424242; }
item:selected { background: #56514a; color: #ffffff; }
popup { background: #393939; border-color: #949494; border-style: outset; }
menu_item { padding: 2dip 7dip; background: transparent; }
menu_item:hover, menu_item:focus { background: #b36b00; color: #ffffff; border-width: 0dip; }
menu_item:selected { color: #eeeeee; font-weight: 600; }
:disabled { color: #808080; }
button:disabled, combo:disabled { background-image: token(button-face); }

/* Pane chrome: one tab strip, one content area, no duplicate panel heading. */
.dock-tabs { background: #2e2e2e; border-color: #262626; border-width: 0.667dip; border-edges: bottom; border-style: solid; }
.dock-tab-group {
    background: #2e2e2e; border-color: #171717; border-width: 0.667dip; border-style: solid;
    border-edges: left right bottom; padding: 0dip 1dip;
}
.dock-tab-group:selected { background: #494949; border-edges: left right; }
.dock-tab-group.joined { border-edges: right bottom; }
.dock-tab-group.joined:selected { border-edges: right; }
tab.dock-tab { background: transparent; border-width: 0dip; padding: 0dip 7dip; color: #cccccc; font-size: 13dip; font-style: normal; }
tab.dock-tab:selected { background: transparent; color: #cccccc; }
tab.dock-tab:hover { background: transparent; color: #cccccc; }
.dock-close, .dock-tool, .dock-menu, .dock-add {
    background: transparent; background-image: none; border-width: 0dip; padding: 1dip 2dip; color: #bebebe;
}
.dock-close:selected { background: transparent; background-image: none; color: #d1d3c4; }
.dock-close:hover, .dock-tool:hover, .dock-menu:hover, .dock-add:hover { background: transparent; background-image: none; color: #ffffff; }
.dock-menu { indicator-size: 16dip; color: #c1d6db; }
.dock-add { indicator-size: 16dip; color: #d1d3c4; }
.dock-divider { background: #484848; border-color: #686868; border-width: 1dip; border-style: outset; }
.dock-divider:hover, .dock-divider:pressed { background: #505050; border-color: #777777; }
.dock-preview { background: #599ace50; border-color: #9ac6ea; border-width: 2dip; }
.path-bar { background-image: linear-gradient(#494949,#3b3b3b); }
combo.pane-path { padding: 1dip 4dip; indicator-size: 22dip; }
combo.pane-path, combo.pane-path:hover {
    background-image: linear-gradient(#3d3d3d,#3a3a3a); border-color: #000000; border-inner-color: #5c5c5c;
    border-style: solid; border-width: 0.667dip;
    combo-indicator: down; color: #cccccc;
}
button.path-navigation { padding: 0dip; indicator-size: 26dip; }
button.path-navigation, button.path-navigation:hover, button.path-navigation:pressed, button.path-navigation:disabled {
    background: transparent; background-image: none; border-width: 0dip;
    indicator-border-color: #263235; color: #bbcdcf;
}
button.path-navigation:disabled { color: #646b6c; }
.object-header { background: #343434; accent-color: #202020; accent-width: 1dip; }
.object-type { font-weight: 600; padding: 1dip 0dip; text-vertical-align: center; }
.object-name { padding: 1dip 3dip; }
popup.value-ladder {
    background: #131313; border-color: #818181; border-width: 0.667dip; border-style: solid;
    color: #cccccc; selection-color: #b88c1c; font-family: 'Sarasa UI SC'; font-weight: 400;
    text-align: center;
}
.parameter-tabs { background: transparent; accent-color: #565656; accent-width: 0.667dip; }
button.parameter-tab { padding: 2dip 9dip; }
button.parameter-tab, button.parameter-tab:hover, button.parameter-tab:focus {
    background-image: linear-gradient(#555555,#2f2f2f); border-color: #262626; border-style: solid;
    border-width: 0.667dip; color: #cccccc;
}
button.parameter-tab.joined { border-edges: top right bottom; }
button.parameter-tab:selected, button.parameter-tab:selected:hover {
    background: #4f4f4f; background-image: none; border-edges: top right left; color: #cccccc;
}
button.parameter-tab.joined:selected { border-edges: top right; }
.parameter-section { background: #4a4a4a; padding: 2dip 7dip; }
.color-swatch { border-color: #777777; border-width: 1dip; border-style: inset; }

.dock-close { indicator-size: 8dip; }
.dock-tool { indicator-size: 17dip; color: #cccccc; indicator-background: #9eafb4; }

/* Divider actions share the same splitter and accepted-frame input path. */
.dock-collapse, .dock-grip {
    background: transparent; background-image: none; border-width: 0dip;
    padding: 0dip; color: #909090; indicator-size: 6dip;
}
.menubar-item {
    background: transparent; background-image: none; border-width: 0dip;
    padding: 0dip 7dip; text-align: center; font-size: 14dip;
}
.bar-stow {
    background-image: linear-gradient(#555555, #3b3b3b);
    border-color: #111111; border-inner-color: #575757; border-width: 0.667dip; border-style: solid;
    padding: 0dip; indicator-size: 14dip; color: #101010;
}
.bar-stow:hover { background-image: linear-gradient(#666666, #4a4a4a); color: #dddddd; }
.bar-stow:focus { border-color: token(focus); border-style: solid; }
button.menubar-item:hover, button.menubar-item:selected, button.menubar-item:pressed {
    background: #555555; background-image: none; color: #ffffff;
}
button.menubar-item:focus { border-width: 1dip; border-color: #b99357; }
button.menubar-item:pressed { background: #686868; }
label.menu-shortcut:disabled { color: #b5b5b5; }
.menu-shortcut { padding: 2dip 0dip; }
label.menu-shortcut.unavailable:disabled { color: #808080; }
.menu-separator { background: #393939; accent-color: #575757; accent-width: 1dip; }
.dock-collapse:hover, .dock-grip:hover { background: #565656; background-image: none; color: #dddddd; }
.dock-collapse:selected { background: #383838; background-image: none; }
.dock-collapse:focus { border-width: 1dip; border-color: token(focus); }

/* Explicit vector-axis semantics in Atelier; animation status is separate. */
.axis-x { accent-color: #c5644b; accent-width: 2dip; }
.axis-y { accent-color: #4c994f; accent-width: 2dip; }
.axis-z { accent-color: #608baf; accent-width: 2dip; }
.axis-w { accent-color: #8f829e; accent-width: 2dip; }
slider {
    background: transparent; border-width: 0dip; border-color: #171717;
    accent-color: #4388b5; indicator-background: #686868; indicator-border-color: #969696;
}
slider:hover, slider:pressed { indicator-background: #8a8a8a; }
slider:focus { indicator-border-color: token(focus); }
slider:disabled { accent-color: #565656; indicator-background: #444444; indicator-border-color: #606060; }
number:disabled { accent-color: #616161; }
