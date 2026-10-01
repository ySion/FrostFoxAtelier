/* Native parameter styling, calibrated against Houdini's dark UI. */
@tokens {
    panel: #3d3d3d;
    field: #131313;
    text: #cccccc;
    focus: #deb36f;
    button-face: linear-gradient(#565656, #3a3a3a);
    button-hover: linear-gradient(#626262, #414141);
    button-down: linear-gradient(#4a5e73, #768ea7);
}
* { color: token(text); }
:role(panel) { background: token(panel); }
:role(caption) { color: #a3a3a3; }
.toolbar {
    background-image: linear-gradient(#494949, #303030);
    border-color: #606060;
    border-width: 1dip;
    border-style: outset;
}
button, combo {
    background-image: token(button-face);
    border-color: #606060;
    border-width: 1dip;
    border-style: outset;
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
    border-color: #5b5b5d;
    border-style: inset;
    border-radius: 0dip;
    padding: 1dip 3dip;
    selection-color: #75521b;
}
number { font-family: 'Source Code Pro'; }
number:hover, text_edit:hover { border-color: #787878; }
number:focus, text_edit:focus { border-color: token(focus); border-style: solid; }
number:invalid, text_edit:invalid { border-color: #ef7766; border-style: solid; }
number:modified:enabled, text_edit:modified:enabled { color: #eeeeee; font-weight: 700; }
checkbox { padding: 1dip 0dip; indicator-size: 14dip; indicator-background: token(field); indicator-border-color: #3e3e3e; }
checkbox:selected:enabled { color: #dddddd; font-weight: 600; }
checkbox:hover:enabled { color: #dbaf61; }
checkbox:focus { border-color: token(focus); }
foldout { padding: 2dip 3dip; indicator-size: 10dip; }
foldout.section { background: #343434; border-color: #4e4e4e; border-width: 1dip; border-style: outset; font-weight: 600; }
foldout.section:hover { color: #eeeeee; }
foldout:focus { border-color: token(focus); border-style: solid; }
combo { indicator-size: 8dip; }
.parameter-list { background: transparent; }
.parameter-pane { background: token(panel); }
.parameter-label { padding: 2dip 0dip; text-align: right; }
.parameter-field { padding: 1dip 3dip; }
list { background: #2d2d2d; scrollbar-color: #747474; scrollbar-width: 3dip; }
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
.dock-tabs { background: #292929; border-color: #484848; border-width: 1dip; border-style: outset; }
.dock-tab-group { background: #272727; }
.dock-tab-group:selected { background: #3d3d3d; }
tab.dock-tab { background: transparent; border-width: 0dip; padding: 2dip 7dip; color: #bcbcbc; }
tab.dock-tab:selected { background: #3d3d3d; color: #eeeeee; }
tab.dock-tab:hover { background: #484848; color: #ffffff; }
tab.dock-tab:focus { border-width: 1dip; border-color: #b99357; }
.dock-close, .dock-tool, .dock-menu {
    background: transparent; background-image: none; border-width: 0dip; padding: 1dip 2dip; color: #bebebe;
}
.dock-close:selected { background: #3d3d3d; background-image: none; }
.dock-close:hover, .dock-tool:hover, .dock-menu:hover { background: #555555; background-image: none; color: #ffffff; }
.dock-close:focus, .dock-tool:focus, .dock-menu:focus { border-width: 1dip; border-color: #b99357; }
.dock-menu { indicator-size: 7dip; }
.dock-divider { background: #484848; border-color: #686868; border-width: 1dip; border-style: outset; }
.dock-divider:hover, .dock-divider:pressed { background: #686868; border-color: #929292; }
.dock-preview { background: #599ace50; border-color: #9ac6ea; border-width: 2dip; }
.pane-path { background: #292929; border-color: #555555; border-style: inset; border-width: 1dip; padding: 2dip 7dip; }

.dock-close { indicator-size: 8dip; }
.dock-tool { indicator-size: 13dip; }
