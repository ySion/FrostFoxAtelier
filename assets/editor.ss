/* Industrial dark parameter & workspace styling (Houdini + Blender calibrated). */
@tokens {
    chrome-recess: #242424;
    chrome-bar: #2c2c2c;
    panel: #383838;
    panel-elevated: #404040;
    panel-recess: #2c2c2c;
    section-header: #313131;
    field: #161616;
    field-readonly: #222222;
    field-border: #252525;
    field-border-hover: #3e3e3e;
    field-focus: #6897bb;
    text: #d0d0d0;
    text-bright: #f0f0f0;
    text-muted: #989898;
    text-disabled: #6e6e6e;
    selection-row: #4b5c6b;
    selection-text: #3d5a73;
    menu-active: #b36600;
    accent-active: #56738c;
    accent-hover: #484848;
    accent-pressed: #5b6d7d;
    severity-error: #e56b65;
    severity-warn: #e2a84b;
    severity-info: #78a9cc;
    scrollbar-track: #222222;
    scrollbar-thumb: #4b4b4b;
    scrollbar-border: #1b1b1b;
    button-face: linear-gradient(#535353, #3c3c3c);
    button-hover: linear-gradient(#616161, #474747);
    button-down: linear-gradient(#313131, #424242);
    button-selected: linear-gradient(#5d86aa, #405f7b);
    button-selected-hover: linear-gradient(#6994ba, #496c8b);
    indicator-socket: #2d4459;
    indicator-socket-hover: #36526b;
    indicator-socket-border: #5d8fb8;
    indicator-socket-glyph: #a8daff;
}
* { color: token(text); }
:role(panel) { background: token(panel); }
:role(caption) { color: token(text-muted); }
.menubar {
    background-image: linear-gradient(#363636, #2d2d2d);
    border-color: #141414;
    border-inner-color: #404040;
    border-width: 0.667dip;
    border-style: solid;
}
button, combo {
    background-image: token(button-face);
    border-color: #1b1b1b;
    border-width: 0.667dip;
    border-style: solid;
    border-radius: 2dip;
    padding: 1dip 7dip;
    color: #e0e4e8;
}
button { text-align: center; }
button:hover, combo:hover { background-image: token(button-hover); border-color: #686868; color: #ffffff; }
button:pressed, combo:pressed { background-image: token(button-down); border-style: solid; border-color: #141414; color: #ffffff; }
button:selected { background-image: token(button-selected); border-color: #82b2dc; color: #ffffff; }
button:selected:hover { background-image: token(button-selected-hover); border-color: #98c6ee; color: #ffffff; }
/* Focus preserves the control's frame; selection and press own the face. */
button:focus, combo:focus { color: token(text-bright); }
number, text_edit {
    background: token(field);
    border-color: token(field-border);
    border-inner-color: #202020;
    border-width: 0.667dip;
    border-style: solid;
    border-radius: 1.5dip;
    padding: 1dip 4dip;
    selection-color: token(selection-text);
}
number { font-family: 'Source Code Pro'; }
number:hover, text_edit:hover { border-color: token(field-border-hover); }
number:focus, text_edit:focus { border-color: token(field-focus); border-style: solid; }
number:invalid, text_edit:invalid { border-color: token(severity-error); border-style: solid; }
number:modified:enabled { color: token(text-bright); font-weight: 700; }
text_edit:modified:enabled { color: token(text-bright); }
text_edit.readonly-field {
    background: token(field-readonly);
    border-color: #292929;
    border-inner-color: transparent;
    color: #c6ccd2;
}
text_edit.readonly-field:hover { border-color: #383838; }
text_edit.readonly-field:focus { border-color: token(field-focus); color: token(text-bright); }
text_edit.readonly-inline {
    background: transparent;
    border-color: transparent;
    border-inner-color: transparent;
    border-width: 0.667dip;
    padding: 0dip 2dip;
    color: token(text);
}
text_edit.readonly-inline:hover { background: #ffffff08; border-color: #3a3a3a; }
text_edit.readonly-inline:focus { background: token(field-readonly); border-color: token(field-focus); }
checkbox {
    background: transparent; padding: 1dip 0dip; border-width: 0dip;
    indicator-size: 13.5dip; indicator-background: token(field); indicator-border-color: #424242;
}
checkbox:hover { indicator-border-color: #5c5c5c; }
checkbox:selected:enabled { color: #e4e4e4; indicator-color: #82b8e8; }
foldout { padding: 2dip 4dip; indicator-size: 10dip; border-width: 0dip; indicator-color: #b4bec4; }
foldout.section {
    background: token(section-header);
    border-width: 0.667dip; border-edges: top bottom; border-color: #252525;
    padding: 2.5dip 6dip; indicator-size: 11dip; font-weight: 600; color: #d8dce0;
}
foldout.section:hover { background: #363636; color: #ffffff; }
foldout.section:focus { color: token(text-bright); }
combo { indicator-size: 8dip; indicator-color: #9aa2a8; combo-indicator: down; }
combo:disabled { indicator-color: #5c5c5c; }
/* Parameter layout, one rule for every page (Parameters, World, Material,
   Resource, Controls): a list starts flush with its parent; section bars span
   the full width; only the rows under a section are inset, by .section-body. */
.parameter-list { background: token(panel); padding: 0dip; }
.section-body { padding: 4dip 4dip 6dip 8dip; }
/* Placeholder lines share the header rows' 6 DIP leading inset. */
.empty-state { padding: 6dip 6dip; color: token(text-muted); }
.parameter-nested-body { background: #333333; border-width: 0.667dip; border-edges: left; border-color: #464646; }
.parameter-pane { background: token(panel); }
.parameter-search { padding: 2dip 4dip 3dip 4dip; background: #323232; border-width: 0.667dip; border-edges: bottom; border-color: #242424; }
.search-box {
    background: token(field);
    border-color: token(field-border);
    border-width: 0.667dip;
    border-style: solid;
    border-radius: 2dip;
}
.search-box:hover { border-color: token(field-border-hover); }
.search-box-field { padding: 1dip 2dip; }
.search-box-field, .search-box-field:hover, .search-box-field:focus {
    background: transparent; border-width: 0dip; border-color: transparent; border-inner-color: transparent;
}
.status-bar { background: #1c1c1c; border-width: 0.667dip; border-edges: top; border-color: #0e0e0e; border-style: solid; }
.parameter-label { padding: 1dip 0dip; text-align: right; text-vertical-align: center; color: #b8bec4; }
.field-label { text-vertical-align: center; color: #b8bec4; }
list, popup, tree, table {
    scrollbar-color: token(scrollbar-thumb);
    scrollbar-width: 7dip;
    scrollbar-inset: 1dip;
    scrollbar-min-thumb: 22dip;
    scrollbar-button-size: 0dip;
    scrollbar-image: none;
    indicator-background: token(scrollbar-track);
    indicator-border-color: token(scrollbar-border);
}
list { background: token(panel-recess); }
table {
    background: #282828; tree-alternate-color: #2e2e2e; tree-line-color: #3c3c3c;
    selection-color: token(selection-row); border-color: #555555; border-width: 0dip;
    padding: 0dip; text-vertical-align: center; indicator-size: 14dip; indicator-color: #a7c0cb;
}
table:focus { border-color: token(field-focus); }
table.data-table {
    font-family: 'Source Code Pro';
    font-size: 11.5dip;
    background: #252525;
    tree-alternate-color: #2a2a2a;
    tree-line-color: #383838;
}
table.browser-table {
    background: #292929;
    tree-alternate-color: #2f2f2f;
    tree-line-color: #3a3a3a;
}
item { padding: 2dip 6dip; border-width: 0dip; }
item:hover, item:focus { background: #3e3e3e; }
item:selected { background: token(selection-row); color: #ffffff; }
/* Scene Outliner & Tree View: high-density alternating industrial rows. */
tree {
    background: #292929; tree-alternate-color: #2f2f2f; tree-line-color: #464646;
    tree-row-height: 19dip; tree-indent: 15dip; padding: 0dip;
}
tree_item {
    background: transparent; border-width: 0dip; padding: 0dip 3dip;
    text-vertical-align: center; indicator-size: 13.5dip;
    indicator-background: #9aa4ab; indicator-border-color: #464646;
    indicator-color: #b4c7ce; font-weight: 400;
}
tree_item:hover { background: #3a3e42; }
tree_item:focus { border-width: 0dip; color: token(text-bright); }
tree_item:selected { background: token(selection-row); color: #ffffff; }
tree_item.entity { indicator-color: #c2cab4; }
tree_item.transform { indicator-color: #8db5cc; }
tree_item.component { indicator-color: #aeb8c0; color: #b8c0c7; }
tree_item.inactive { color: #7a8086; indicator-color: #6b7379; }
tree_item.inactive:selected { color: #c8cfd6; }
popup { background: #333333; border-color: #585858; border-width: 0.667dip; border-style: solid; border-radius: 2dip; }
popup.menu {
    background: #303030; border-width: 0.667dip; border-color: #525252; border-shadow-color: #161616;
    border-style: solid; font-size: 12.5dip; scrollbar-gutter: overlay;
}
menu_item {
    padding: 0dip 9dip; background: transparent; border-width: 0dip; text-vertical-align: center;
    indicator-size: 12dip; indicator-background: #222222; indicator-border-color: #888888; indicator-color: #eeeeee;
}
menu_item:hover, menu_item:focus { background: token(menu-active); color: #ffffff; }
menu_item:selected { color: #eeeeee; }
menu_item:disabled { color: token(text-disabled); indicator-border-color: #555555; indicator-color: token(text-disabled); }
combo.cavity-menu { padding: 0dip 6dip; combo-indicator: down; indicator-size: 8dip; }
:disabled { color: token(text-disabled); }
button:disabled, combo:disabled { background-image: none; background: #323232; border-color: #262626; }

/* Pane chrome: one tab strip, one content area, active-pane accent cue. */
.dock-tabs { background: #1c1c1c; border-color: #111111; border-width: 0.667dip; border-edges: bottom; border-style: solid; }
.dock-tabs.focused-pane { background: #1f2429; }
.dock-tab-group {
    background-image: linear-gradient(#313131, #272727); border-color: #121212; border-inner-color: #3e3e3e;
    border-width: 0.667dip; border-style: solid; border-edges: top right bottom; padding: 0dip 2dip;
}
.dock-tab-group:hover { background-image: linear-gradient(#3c3c3c, #303030); border-inner-color: #4c4c4c; }
.dock-tab-group:selected {
    background-image: linear-gradient(#474747, #383838); border-edges: top left right;
    border-color: #5e5e5e; border-inner-color: transparent;
}
.dock-tab-group.focused-tab:selected {
    background-image: linear-gradient(#4b5662, #383838); border-edges: top left right;
    border-color: #74aad8; border-inner-color: transparent;
}
tab.dock-tab { background: transparent; border-width: 0dip; padding: 0dip 7dip; color: #a8b0b8; font-style: normal; }
tab.dock-tab:selected { background: transparent; color: #eef2f6; font-weight: 600; }
tab.dock-tab:hover { background: transparent; color: #e8ecf0; }
tab.dock-tab:focus { color: #ffffff; }
.dock-close, .dock-tool, .dock-menu, .dock-add {
    background: transparent; background-image: none; border-width: 0.667dip; border-style: solid; border-color: transparent;
    padding: 1dip 2dip; color: #a6aeb6; border-radius: 2dip;
}
.dock-close:selected { background: transparent; background-image: none; border-color: transparent; color: #dce2e8; }
.dock-close:hover, .dock-tool:hover, .dock-menu:hover, .dock-add:hover {
    background-image: linear-gradient(#4d4d4d, #3d3d3d); border-color: #626262; color: #ffffff;
}
.dock-close:pressed, .dock-tool:pressed, .dock-menu:pressed, .dock-add:pressed {
    background-image: token(button-down); border-color: #141414; color: #ffffff;
}
.dock-close:disabled, .dock-tool:disabled, .dock-menu:disabled, .dock-add:disabled {
    background: transparent; background-image: none; border-color: transparent; color: token(text-disabled);
}
.dock-menu { indicator-size: 11dip; color: #bdd0d8; }
.dock-add { indicator-size: 12.5dip; color: #d0d8e0; }
.dock-divider { background: #222222; border-color: #181818; border-width: 0.667dip; border-style: solid; }
.dock-divider.vertical { border-edges: left right; }
.dock-divider.horizontal { border-edges: top bottom; }
.dock-collapse, .dock-grip {
    background: transparent; background-image: none; border-width: 0dip;
    padding: 0dip; color: #7c7c7c; indicator-size: 6dip;
}
.dock-grip { indicator-size: 18dip; }
.dock-collapse:selected { background: #383838; background-image: none; }
.dock-collapse:hover, .dock-grip:hover { background: #464646; background-image: none; color: #d8d8d8; }
.dock-collapse:pressed, .dock-grip:pressed { background: token(accent-active); background-image: none; color: #ffffff; }
.dock-collapse:focus { color: #dddddd; }
.dock-preview { background: #599ace44; border-color: #82b8e8; border-width: 1.5dip; }
.path-bar { background: #313131; border-width: 0.667dip; border-edges: bottom; border-color: #202020; }
combo.pane-path { padding: 1dip 6dip; indicator-size: 15dip; border-radius: 2dip; }
combo.pane-path {
    background: #252525; background-image: none; border-color: #1a1a1a;
    border-style: solid; border-width: 0.667dip;
    combo-indicator: down; color: token(text);
}
combo.pane-path:hover { background: #2c2c2c; background-image: none; border-color: #3d3d3d; color: token(text-bright); }
button.path-navigation { padding: 0dip; indicator-size: 17dip; }
button.path-navigation, button.path-navigation:hover, button.path-navigation:pressed, button.path-navigation:disabled {
    background: transparent; background-image: none; border-width: 0dip;
    indicator-border-color: #263235; color: #b8cad0;
}
button.path-navigation:hover { background: token(accent-hover); color: #ffffff; border-radius: 2dip; }
button.path-navigation:disabled { background: transparent; color: #5c6366; }
.object-header { background: #343434; accent-color: #222222; accent-width: 0.667dip; }
.object-type { font-weight: 600; padding: 1dip 0dip; text-vertical-align: center; indicator-size: 15dip; indicator-color: #a7c0cb; }
button.parameter-tool {
    background: transparent; background-image: none; border-width: 0.667dip; border-style: solid; border-color: transparent;
    padding: 0dip; indicator-size: 15.5dip; color: #a8b8c2; border-radius: 1.5dip;
}
button.parameter-tool:hover { background: #ffffff12; background-image: none; border-color: #50565c; color: #ffffff; }
button.parameter-tool:selected { background: token(indicator-socket); background-image: none; border-color: token(indicator-socket-border); color: token(indicator-socket-glyph); }
button.parameter-tool:selected:hover { background: token(indicator-socket-hover); background-image: none; border-color: #78add8; color: #ffffff; }
button.parameter-tool:pressed { background: #243647; background-image: none; border-color: #466e91; color: #ffffff; }
button.parameter-tool:focus { color: #ffffff; }
button.parameter-tool:disabled { background: transparent; background-image: none; border-color: transparent; color: #646a6e; }
popup.value-ladder {
    background: #161616; border-color: #6897bb; border-width: 0.667dip; border-style: solid;
    color: token(text); selection-color: #b88c1c; font-family: 'Sarasa UI SC'; font-weight: 400;
    text-align: center;
}
/* Switch Tab Architecture:
   1. .segmented-switch / button.segmented-item — rectilinear industrial recessed trough + cold-slate active block (toolbar & inline mode switches).
   2. .subtab-bar / button.subtab-item — flat sub-header strip + 2 DIP steel-blue bottom indicator bar (full-width intra-pane pages). */
.segmented-switch {
    background: #1b1b1b; border-color: #111111; border-width: 0.667dip; border-style: solid; border-radius: 2dip;
}
button.segmented-item {
    padding: 0dip 8dip; font-size: 11.5dip; font-weight: 400; border-radius: 1.5dip;
    background: transparent; background-image: none;
    border-width: 0.667dip; border-style: solid; border-color: transparent; color: #9ea6ae;
}
button.segmented-item:hover {
    background: #2b2f34; background-image: none; border-color: transparent; color: #e4e9ee;
}
button.segmented-item:pressed {
    background: #343940; background-image: none; border-color: #4e565e; color: #ffffff;
}
button.segmented-item:focus { color: #ffffff; }
button.segmented-item:selected {
    background-image: linear-gradient(#4d545c, #3c4248); border-color: #6b7682; color: #ffffff; font-weight: 600;
}
button.segmented-item:selected:hover, button.segmented-item:selected:focus {
    background-image: linear-gradient(#565e67, #434a52); border-color: #7e8b98; color: #ffffff;
}
button.segmented-item:disabled {
    background: transparent; background-image: none; border-color: transparent; color: token(text-disabled);
}
.subtab-bar {
    background: #292929; border-width: 0.667dip; border-style: solid; border-edges: bottom; border-color: #181818;
}
button.subtab-item {
    padding: 0dip 10dip; font-size: 12dip; font-weight: 400; border-radius: 0dip;
    background: transparent; background-image: none; border-width: 0dip;
    color: #9ea6ae; accent-width: 2dip; accent-color: transparent;
}
button.subtab-item:hover {
    background: #31353a; background-image: none; color: #e8edf2;
}
button.subtab-item:pressed {
    background: #373c42; background-image: none; color: #ffffff;
}
button.subtab-item:focus { color: #ffffff; }
button.subtab-item:selected {
    background: #33383e; background-image: none; color: #ffffff; font-weight: 600; accent-color: #74aad8;
}
button.subtab-item:selected:hover, button.subtab-item:selected:focus {
    background: #393f46; background-image: none; color: #ffffff; accent-color: #86baea;
}
.parameter-section { background: #424242; padding: 2dip 7dip; }
.color-swatch { border-color: #202020; border-width: 0.667dip; border-style: solid; border-radius: 1.5dip; }
button.color-swatch { padding: 0dip; }
button.color-swatch, button.color-swatch:hover, button.color-swatch:pressed { background-image: none; }
button.color-swatch:hover { border-color: #b0b0b0; }
color_plane.color-plane {
    border-color: #181818; border-width: 0.667dip; border-style: solid; border-radius: 2dip;
}
popup.color-editor {
    background: #323232; border-width: 0.667dip; border-color: #5a5a5a; border-shadow-color: #181818;
    border-style: solid; border-radius: 3dip;
}

.dock-close { indicator-size: 8dip; }
.dock-tool { indicator-size: 16dip; color: #c4ccd2; indicator-background: #9eafb4; }

.menubar-item {
    background: transparent; background-image: none; border-width: 0dip;
    padding: 0dip 7dip; text-align: center; font-size: 12.5dip; border-radius: 0dip;
}
.bar-stow {
    background-image: linear-gradient(#353535, #2a2a2a); border-inner-color: #424242;
    border-color: #141414; border-width: 0.667dip; border-style: solid; border-radius: 0dip;
    padding: 0dip 2dip; indicator-size: 7dip; icon-align: center; color: #adb5bd;
}
.bar-stow:hover, .bar-stow:focus, .bar-stow:selected {
    background-image: linear-gradient(#454545, #373737); border-inner-color: #545454; border-style: solid; color: #f0f4f8;
}
.bar-stow:pressed {
    background-image: token(button-selected); border-color: #7eaed8; border-inner-color: transparent; border-style: solid; color: #ffffff;
}
button.menubar-item, button.menubar-item:focus {
    background: transparent; background-image: none; color: token(text);
}
button.menubar-item:hover {
    background: #424242; background-image: none; color: #ffffff;
}
button.menubar-item:selected, button.menubar-item:pressed {
    background: token(menu-active); background-image: none; color: #ffffff;
}
label.menu-shortcut:disabled { color: #9a9a9a; }
.menu-shortcut { padding: 0dip; text-align: right; text-vertical-align: center; font-family: 'Source Code Pro'; font-size: 11dip; }
label.menu-shortcut.unavailable:disabled { color: #686868; }
.menu-separator { background: #484848; }
.menu-heading { padding: 4dip 9dip 2dip 9dip; font-weight: 600; }
label.menu-heading:disabled { color: #949ca4; }
popup.pie-menu { background: transparent; background-image: none; border-width: 0dip; padding: 0dip; scrollbar-gutter: overlay; }
.pie-hub { background: #242424f0; color: #d8d8d8; text-align: center; text-vertical-align: center; border-radius: 11dip; }
menu_item.pie-item { background: #2e2e2ef5; color: #d8d8d8; border-style: solid; border-width: 0.667dip; border-color: #161616; border-radius: 4dip; }
menu_item.pie-item:hover, menu_item.pie-item:focus { background: token(accent-active); color: #ffffff; border-color: #7aa7cf; }
menu_item.pie-item:pressed { background: #6c8ba8; }

/* Explicit vector-axis semantics in Atelier; animation status is separate. */
.axis-x { accent-color: #cc624b; accent-width: 2dip; }
.axis-y { accent-color: #549e57; accent-width: 2dip; }
.axis-z { accent-color: #5c8db8; accent-width: 2dip; }
.axis-w { accent-color: #9382a6; accent-width: 2dip; }
slider {
    background: transparent; border-width: 0dip; border-color: #181818;
    accent-color: #4a85b5; indicator-background: #b8c2ca; indicator-border-color: #202020;
}
slider:hover, slider:pressed { accent-color: #5a98ca; indicator-background: #e6ecf0; }
slider:focus { indicator-border-color: token(field-focus); }
slider:disabled { accent-color: #4a4a4a; indicator-background: #606060; indicator-border-color: #2a2a2a; }
number:disabled { accent-color: #555555; background: #1e1e1e; }

/* Single-row condensed industrial command shelf. */
.tool-shelf { background: #303030; border-color: #1c1c1c; border-width: 0.667dip; border-edges: bottom; border-style: solid; }
.shelf-tabs { background: #222222; border-radius: 2dip; border-color: #141414; border-width: 0.667dip; border-style: solid; }
.shelf-tab { font-size: 11.5dip; padding: 0dip 8dip; }
button.shelf-tool {
    background: transparent; background-image: none; border-width: 0.667dip; border-style: solid; border-color: transparent;
    border-radius: 2dip; text-align: center; text-vertical-align: center; font-size: 11.5dip;
    padding: 0dip 7dip; indicator-size: 14.5dip; indicator-color: #b2ccd8; color: #dce2e8;
}
button.shelf-tool:hover { background-image: linear-gradient(#505050, #3f3f3f); border-color: #666666; color: #ffffff; indicator-color: #ffffff; }
button.shelf-tool:pressed { background-image: token(button-down); border-color: #141414; color: #ffffff; indicator-color: #ffffff; }
button.shelf-tool:disabled { background: transparent; background-image: none; border-color: transparent; color: token(text-disabled); indicator-color: #62686c; }
tree_item.mesh-entity { indicator-color: #a7c0cb; }
tree_item.retained { color: #e8b04a; }
tree_item.drop-target { background: #4f6a80; color: #ffffff; }
text_edit.inline-rename { padding: 0dip 3dip; border-color: token(field-focus); background: #121212; }

/* File browser: shared selection and geometry across list/details/tiles. */
.browser-tool { padding: 1dip 5dip; font-size: 11dip; }
.icon-menu, .browser-icon {
    background: transparent; background-image: none; border-width: 0.667dip; border-style: solid; border-color: transparent;
    border-radius: 1.5dip; padding: 0dip; indicator-size: 16dip; indicator-color: #aebec6; combo-indicator: down;
}
combo.icon-menu:hover, .browser-icon:hover { background: #ffffff12; background-image: none; border-color: #50565c; indicator-color: #ffffff; }
combo.icon-menu:selected {
    background: token(menu-active); background-image: none; border-color: #d47a00; indicator-color: #ffffff;
}
.browser-icon:selected {
    background: token(indicator-socket); background-image: none; border-color: token(indicator-socket-border); indicator-color: token(indicator-socket-glyph);
}
combo.icon-menu:pressed, .browser-icon:pressed {
    background: #243647; background-image: none; border-color: #466e91; indicator-color: #ffffff;
}
.browser-icon { indicator-size: 15dip; }
.browser-icon:disabled, .icon-menu:disabled { background: transparent; background-image: none; border-color: transparent; indicator-color: #62686c; }
.browser-toolbar { background: #313131; border-width: 0.667dip; border-edges: bottom; border-color: #202020; }
.browser-path { padding: 0dip 5dip; combo-indicator: down; }
.browser-search {
    background: token(field); border-color: token(field-border);
    border-width: 0.667dip; border-style: solid; border-radius: 2dip;
}
.browser-search:hover { border-color: token(field-border-hover); }
.browser-search-icon { padding: 0dip; indicator-size: 13.5dip; indicator-color: #969ea4; }
.browser-search-input { padding: 1dip 3dip; }
.browser-search-input, .browser-search-input:hover, .browser-search-input:focus {
    background: transparent; border-width: 0dip; border-color: transparent; border-inner-color: transparent;
}
.browser-status { background: #2b2b2b; text-vertical-align: center; border-width: 0.667dip; border-edges: top; border-color: #1e1e1e; }
.browser-directories { background: #292929; }

/* Code editor & open-document tab strip. */
code_editor.source-code {
    background: #1c1d1f; color: #d6dce2; font-family: "Source Code Pro"; font-size: 12dip;
    padding: 0dip; border-width: 0dip; selection-color: #2f4b66;
    indicator-color: #6e7680; indicator-background: #222326;
    tree-alternate-color: #232528; scrollbar-width: 10dip;
}
.code-tab-bar { background: #1e1f21; border-width: 0.667dip; border-edges: bottom; border-color: #121212; }
.code-empty-tab { padding: 0dip 8dip; color: token(text-muted); font-size: 11.5dip; text-vertical-align: center; }
.code-tab-item {
    background-image: linear-gradient(#313336, #26282a); border-width: 0.667dip; border-style: solid;
    border-edges: top right bottom; border-color: #141414; border-inner-color: #3d4044;
}
.code-tab-item:selected {
    background-image: linear-gradient(#384450, #1c1d1f); border-edges: top left right;
    border-color: #6ea4d4; border-inner-color: transparent;
}
button.code-tab-btn {
    background: transparent; background-image: none; border-width: 0dip; border-radius: 0dip;
    padding: 0dip 5dip; font-size: 11.5dip; indicator-size: 13dip; indicator-color: #9cb4c0; color: #a8b0b8;
}
button.code-tab-btn:hover { background: transparent; background-image: none; color: #eceff2; }
button.code-tab-btn:selected { background: transparent; background-image: none; color: #ffffff; font-weight: 600; }
.code-breadcrumb-bar {
    background: #252629; border-width: 0.667dip; border-style: solid; border-edges: bottom; border-color: #151515;
}
.code-breadcrumb-path {
    padding: 0dip 3dip; font-size: 11dip; color: #9aa2aa; text-vertical-align: center;
}
.code-breadcrumb-sep {
    padding: 0dip 2dip; font-size: 11dip; color: #687078; text-vertical-align: center;
}
combo.code-outline-combo {
    font-family: "Source Code Pro"; font-size: 11dip; padding: 0dip 6dip; indicator-size: 13dip;
}
.code-find-bar {
    background: #282a2d; border-width: 0.667dip; border-style: solid; border-edges: bottom; border-color: #141414;
}
button.code-find-opt { font-family: "Source Code Pro"; font-size: 11dip; }
.code-find-count {
    padding: 0dip 6dip; font-size: 11dip; font-family: "Source Code Pro"; color: #9ea6ae; text-vertical-align: center;
}
.code-completion-popup {
    background: #25272a; border-width: 0.667dip; border-style: solid; border-color: #5b87ad;
    border-inner-color: #36393e; border-radius: 2dip;
}
.code-completion-row { background: transparent; }
.code-completion-row-active { background: #314659; }
button.code-completion-btn {
    background: transparent; background-image: none; border-width: 0dip; border-radius: 0dip;
    text-align: left; padding: 0dip 4dip; font-family: "Source Code Pro"; font-size: 11.5dip; color: #dce2e8;
}
button.code-completion-btn:hover { background: #ffffff10; background-image: none; color: #ffffff; }
button.code-completion-btn:selected { background: transparent; background-image: none; color: #ffffff; font-weight: 600; }
.code-completion-detail {
    padding: 0dip 2dip; font-family: "Source Code Pro"; font-size: 10.5dip; color: #8c96a0; text-vertical-align: center;
}
.code-completion-doc {
    background: #1e2022; border-width: 0.667dip; border-style: solid; border-edges: top; border-color: #35383d; color: #b8c2cc;
}
.comp-badge-fn { font-family: "Source Code Pro"; font-size: 10dip; font-weight: 600; color: #dec078; text-align: center; text-vertical-align: center; }
.comp-badge-fld { font-family: "Source Code Pro"; font-size: 10dip; font-weight: 600; color: #8cc2eb; text-align: center; text-vertical-align: center; }
.comp-badge-type { font-family: "Source Code Pro"; font-size: 10dip; font-weight: 600; color: #66c4b0; text-align: center; text-vertical-align: center; }
.comp-badge-kw { font-family: "Source Code Pro"; font-size: 10dip; font-weight: 600; color: #c78fd6; text-align: center; text-vertical-align: center; }
.comp-badge-prop { font-family: "Source Code Pro"; font-size: 10dip; font-weight: 600; color: #96cf88; text-align: center; text-vertical-align: center; }
.code-signature-popup {
    background: #27292ce8; border-width: 0.667dip; border-style: solid; border-color: #546474; border-radius: 2dip;
}
.code-sig-text { font-family: "Source Code Pro"; font-size: 11dip; color: #aeb6be; text-vertical-align: center; }
.code-sig-text:disabled { color: #aeb6be; }
.code-sig-active { font-family: "Source Code Pro"; font-size: 11dip; font-weight: 700; color: #78b6eb; text-vertical-align: center; }
.code-sig-active:disabled { color: #78b6eb; }
.code-hover-card {
    background: #242629f2; border-width: 0.667dip; border-style: solid; border-color: #4c5864; border-radius: 2dip;
}
.code-hover-header { font-family: "Source Code Pro"; font-size: 11.5dip; font-weight: 600; color: #e2e8ee; }
.code-hover-header:disabled { color: #e2e8ee; }
.code-hover-body { font-size: 11.5dip; color: #b6c0ca; }
.code-hover-body:disabled { color: #b6c0ca; }
.code-problems-pane {
    background: #212224; border-width: 0.667dip; border-style: solid; border-edges: top; border-color: #131313;
}
.code-status-bar {
    background: #262729; border-width: 0.667dip; border-style: solid; border-edges: top; border-color: #161616;
}
button.code-status-item {
    background: transparent; background-image: none; border-width: 0dip; border-radius: 1.5dip;
    padding: 0dip 6dip; font-family: "Source Code Pro"; font-size: 11dip; indicator-size: 13dip;
    indicator-color: #9aa4ae; color: #b4bcc4;
}
button.code-status-item:hover { background: #ffffff12; background-image: none; color: #ffffff; indicator-color: #ffffff; }
button.code-status-ok { color: #9ea8b2; indicator-color: #7e8a94; }
button.code-status-err { background: #422926; color: #f5b6ac; indicator-color: token(severity-error); }
button.code-status-err:hover { background: #54332f; color: #ffd4cc; indicator-color: #ff9c8f; }
.code-status-meta {
    padding: 0dip 4dip; font-family: "Source Code Pro"; font-size: 11dip; color: #8c949c; text-vertical-align: center;
}
.conflict-banner {
    background: #3d2c22; border-width: 0.667dip; border-style: solid; border-edges: bottom; border-color: #7a4f34; color: #f2c9a0;
}
.conflict-text { color: #f2c9a0; indicator-size: 14dip; indicator-color: token(severity-warn); }
.source-status { font-size: 11dip; padding: 0dip 4dip; color: token(text-muted); text-vertical-align: center; }

/* Output panel severity & health surfaces. */
.output-toolbar { background: #313131; border-width: 0.667dip; border-edges: bottom; border-color: #202020; }
.output-health-drawer { background: #292929; border-width: 0.667dip; border-edges: bottom; border-color: #202020; }
.output-detail-pane { background: #242424; }
.badge-error { color: token(severity-error); font-weight: 600; }
.badge-warn { color: token(severity-warn); font-weight: 600; }
.badge-info { color: token(severity-info); }

/* Scene view chrome: air-state operation band & tool rails. */
.viewport-options { background: #2c2c2c; border-color: #161616; border-inner-color: #3b3b3b; border-width: 0.667dip; border-edges: top bottom; }
.viewport-option-label { padding: 0dip 3dip; text-vertical-align: center; color: token(text-muted); }
.viewport-option-divider { background: #1e1e1e; }
combo.viewport-option-menu { padding: 0dip 8dip; combo-indicator: down; text-vertical-align: center; indicator-size: 14dip; indicator-color: #c4ccd0; }
combo.viewport-option-menu { border-width: 0.667dip; border-style: solid; }
combo.viewport-option-menu {
    background-image: linear-gradient(#4c4c4c, #3c3c3c); border-color: #1c1c1c;
}
combo.viewport-option-menu:hover { background-image: linear-gradient(#585858, #464646); color: #ffffff; }
checkbox.viewport-option-check { padding: 0dip 3dip; text-vertical-align: center; }
.viewport-rail { background: #323232; border-width: 0.667dip; border-edges: right; border-color: #181818; }
.viewport-rail-separator { background: #464646; }
button.viewport-mode.grouped { border-edges: top; border-width: 0.667dip; border-color: #464646; }
button.viewport-mode.grouped:selected { border-edges: all; }
.viewport-mode, .viewport-operation {
    background: transparent; background-image: none; border-width: 0.667dip; border-style: solid; border-color: transparent;
    padding: 0dip; border-radius: 1.5dip; indicator-size: 19dip; indicator-color: #b8c4cc; indicator-background: #e04a1c;
}
button.viewport-mode:hover, combo.viewport-operation:hover {
    background: #ffffff12; background-image: none; border-color: #52585e; indicator-color: #ffffff;
}
button.viewport-mode:selected {
    background: token(indicator-socket); background-image: none; border-color: token(indicator-socket-border); indicator-color: #ffffff;
}
button.viewport-mode:selected:hover {
    background: token(indicator-socket-hover); background-image: none; border-color: #78add8; indicator-color: #ffffff;
}
button.viewport-mode:pressed, combo.viewport-operation:pressed {
    background: #243647; background-image: none; border-color: #466e91;
}
button.viewport-mode:disabled, combo.viewport-operation:disabled {
    background: transparent; background-image: none; border-color: transparent; indicator-color: #62686c;
}
/* Passive HUD: white semibold title with a dark drop shadow over the scene. */
.viewport-mode-label {
    color: #ffffff; font-size: 15dip; font-weight: 600; indicator-size: 21dip; indicator-color: #e8eef0; indicator-background: #e04a1c;
    padding: 0dip; text-vertical-align: center; text-shadow-color: #00000099;
}
.viewport-mode-label:disabled { color: #ffffff; }
.viewport-fps {
    color: #ffffff; font-size: 11.5dip; font-family: 'Source Code Pro'; padding: 0dip; text-shadow-color: #000000cc;
}
.viewport-fps:disabled { color: #ffffff; }
.viewport-fly-reticle {
    color: #ffffff; font-size: 20dip; font-weight: 400; padding: 0dip;
    text-align: center; text-vertical-align: center; text-shadow-color: #000000ee;
}
.viewport-fly-reticle:disabled { color: #ffffff; }
/* Floating view pill: translucent slate fill, light hairline, inline caret. */
combo.viewport-view-menu {
    border-style: solid;
    border-width: 0.667dip; border-radius: 11dip; padding: 0dip 9dip 0dip 11dip; combo-indicator: caret; indicator-size: 9dip;
}
combo.viewport-view-menu, combo.viewport-view-menu:hover {
    background: #526169cc; background-image: none; color: #e8ecef; border-color: #8f969a; indicator-color: #e8ecef;
}
combo.viewport-view-menu:hover { background: #62737cdd; color: #ffffff; }
combo.viewport-view-menu:selected, combo.viewport-view-menu:pressed { background: #3f4e56dd; background-image: none; }
.viewport-readout { color: #e6e6e6; text-vertical-align: center; font-size: 12dip; font-family: 'Source Code Pro'; }
.parameter-swatch { border-color: #131313; border-width: 0.667dip; border-style: solid; }
.viewport-tool-hint { color: #1a1a1a; font-size: 12dip; padding: 0dip; text-vertical-align: bottom; }
.viewport-tool-hint:disabled { color: #1a1a1a; }
.viewport-tool-hint.dark-background, .viewport-tool-hint.dark-background:disabled { color: #d9d9d9; }
.viewport-frame { background: transparent; border-width: 0.667dip; border-edges: right bottom; border-style: solid; border-color: #383838; border-inner-color: #505050; }
.status-text { text-vertical-align: center; padding: 0dip; }
combo.parameter-menu { padding: 0dip 10dip; }
.status-facts { color: #8c9298; font-family: 'Source Code Pro'; font-size: 11.5dip; }

/* Blender-style semantic header in the Houdini palette. */
combo.viewport-header-mode {
    padding: 0dip 7dip 0dip 6dip; combo-indicator: down; text-vertical-align: center; indicator-size: 15dip; indicator-color: #c4ccd0;
    border-width: 0.667dip; border-style: solid; border-color: #1c1c1c; background-image: linear-gradient(#484848, #383838);
}
combo.viewport-header-mode:hover { background-image: linear-gradient(#565656, #444444); color: #ffffff; }
combo.viewport-header-menu {
    background: transparent; background-image: none; border-width: 0dip; padding: 0dip 7dip; border-radius: 2dip;
    combo-indicator: none; text-vertical-align: center; color: token(text);
}
combo.viewport-header-menu:hover { background: token(accent-hover); background-image: none; color: #ffffff; }
combo.viewport-header-menu:selected, combo.viewport-header-menu:pressed { background: token(menu-active); background-image: none; color: #ffffff; }
combo.viewport-header-menu:disabled { background: transparent; background-image: none; color: token(text-disabled); }
combo.viewport-header-icon { padding: 0dip 0dip 0dip 3dip; indicator-size: 16dip; icon-align: left; }
combo.viewport-header-step {
    padding: 0dip 5dip; combo-indicator: down; text-vertical-align: center; indicator-size: 8dip;
    border-width: 0.667dip; border-style: solid; border-color: #1c1c1c; background-image: linear-gradient(#484848, #383838);
}
combo.viewport-header-step:hover { background-image: linear-gradient(#565656, #444444); color: #ffffff; }
button.viewport-header-toggle {
    padding: 0dip; indicator-size: 16dip; indicator-color: #b8c4cc; border-width: 0.667dip; border-color: #1b1b1b;
    background: #252525; background-image: none; border-radius: 1.5dip;
}
button.viewport-header-toggle:hover { background: #32363a; background-image: none; border-color: #545a60; indicator-color: #ffffff; }
button.viewport-header-toggle:selected {
    background: token(indicator-socket); background-image: none; indicator-color: token(indicator-socket-glyph); border-color: token(indicator-socket-border);
}
button.viewport-header-toggle:selected:hover {
    background: token(indicator-socket-hover); background-image: none; indicator-color: #ffffff; border-color: #78add8;
}
combo.viewport-header-more {
    background: transparent; background-image: none; border-width: 0dip; padding: 0dip; border-radius: 2dip;
    indicator-size: 9dip; indicator-color: #b4bcc2; icon-align: center;
}
combo.viewport-header-more:hover { background: token(accent-hover); background-image: none; }
combo.viewport-header-more:selected, combo.viewport-header-more:pressed { background: token(menu-active); background-image: none; indicator-color: #ffffff; }
.viewport-segment { background: transparent; }
button.viewport-segment-item {
    padding: 0dip; indicator-size: 15dip; indicator-color: #b8c4cc; border-width: 0.667dip; border-style: solid; border-radius: 0dip;
    border-color: #181818; border-edges: top bottom right; background: #252525; background-image: none;
}
button.viewport-segment-item.first { border-edges: all; border-radius: 1.5dip; }
button.viewport-segment-item:hover { background: #32363a; background-image: none; border-color: #4c5258; indicator-color: #ffffff; }
button.viewport-segment-item:selected {
    background-image: linear-gradient(#4d545c, #3c4248); indicator-color: #ffffff; border-color: #6b7682;
}
button.viewport-segment-item:selected:hover {
    background-image: linear-gradient(#565e67, #434a52); indicator-color: #ffffff; border-color: #7e8b98;
}
.viewport-display-bar { border-edges: left; }
button.display-toggle {
    background: transparent; background-image: none; border-width: 0.667dip; border-style: solid; border-color: transparent;
    padding: 0dip; border-radius: 1.5dip; indicator-size: 16.5dip; indicator-color: #b8c4cc;
}
button.display-toggle:hover { background: #ffffff12; background-image: none; border-color: #50565c; indicator-color: #ffffff; }
button.display-toggle:selected { background: token(indicator-socket); background-image: none; border-color: token(indicator-socket-border); indicator-color: token(indicator-socket-glyph); }
button.display-toggle:selected:hover { background: token(indicator-socket-hover); background-image: none; border-color: #78add8; indicator-color: #ffffff; }
button.display-toggle:pressed { background: #243647; background-image: none; border-color: #466e91; indicator-color: #ffffff; }
button.display-toggle:disabled { background: transparent; background-image: none; border-color: transparent; indicator-color: #62686c; }
button.display-toggle.grouped { border-edges: top; border-width: 0.667dip; border-color: #464646; border-radius: 0dip; }
button.display-toggle.grouped:selected { border-edges: all; border-color: token(indicator-socket-border); }
/* Stateful icon indicators vs one-shot click buttons vs recessed filter selectors. */
button.ghost-tool {
    background: transparent; background-image: none; border-width: 0.667dip; border-style: solid; border-color: transparent;
    border-radius: 1.5dip; padding: 0dip; indicator-size: 15dip; indicator-color: #b4c0ca; color: token(text);
}
button.ghost-tool:hover {
    background: #ffffff12; background-image: none; border-color: #52585e; indicator-color: #ffffff; color: #ffffff;
}
button.ghost-tool:pressed {
    background: #243647; background-image: none; border-color: #466e91; indicator-color: #ffffff; color: #ffffff;
}
button.ghost-tool:selected {
    background: token(indicator-socket); background-image: none; border-color: token(indicator-socket-border);
    indicator-color: token(indicator-socket-glyph); color: #ffffff;
}
button.ghost-tool:selected:hover {
    background: token(indicator-socket-hover); background-image: none; border-color: #78add8;
    indicator-color: #ffffff; color: #ffffff;
}
button.ghost-tool:disabled {
    background: transparent; background-image: none; border-color: transparent; indicator-color: #5c6268; color: token(text-disabled);
}
button.filter-selector {
    background: #232323; background-image: none; border-width: 0.667dip; border-style: solid; border-color: #171717;
    border-radius: 1.5dip; padding: 0dip 7dip; font-size: 11.5dip; indicator-size: 14dip; indicator-color: #a4b4be; color: token(text);
}
button.filter-selector:hover {
    background: #2b2b2b; background-image: none; border-color: #444444; indicator-color: #ffffff; color: token(text-bright);
}
button.filter-selector:pressed {
    background: #1d1d1d; background-image: none; border-color: token(field-focus); color: #ffffff;
}
button.ghost-button {
    background-image: linear-gradient(#464646, #363636); border-width: 0.667dip; border-style: solid; border-color: #1b1b1b;
    border-radius: 1.5dip; padding: 0dip 7dip; font-size: 11.5dip; indicator-size: 15dip; indicator-color: #ccd6de; color: #dee4ea;
}
button.ghost-button:hover {
    background-image: linear-gradient(#565656, #434343); border-color: #6c6c6c; indicator-color: #ffffff; color: #ffffff;
}
button.ghost-button:pressed {
    background-image: token(button-down); border-color: #141414; indicator-color: #ffffff; color: #ffffff;
}
button.ghost-button:disabled {
    background: #2b2b2b; background-image: none; border-color: #202020; indicator-color: #60666a; color: token(text-disabled);
}
button.viewport-header-toggle:disabled, button.viewport-segment-item:disabled {
    background-image: linear-gradient(#353535, #2e2e2e); indicator-color: #646c72; color: token(text-disabled);
}
button.network-node-btn {
    background: transparent; background-image: none; border-width: 0dip;
    text-align: left; padding: 1dip 8dip; color: #d6dde3;
}
button.network-node-btn:hover { background: #424242; background-image: none; color: #ffffff; }
button.network-node-btn:selected { background: token(accent-active); background-image: none; color: #ffffff; }
.network-cook-summary { font-size: 11dip; color: #a7b6c2; text-vertical-align: center; padding: 0dip 4dip; }
.network-cook-error { font-size: 11dip; color: token(severity-error); font-weight: 600; text-vertical-align: center; padding: 0dip 4dip; }
.network-canvas { background: #232323; border-width: 0.667dip; border-style: solid; border-color: #181818; }
.network-canvas-bg { background: #232323; }
.network-tab-popup {
    background: #2c2c2c; border-width: 0.667dip; border-style: solid; border-color: #6897bb; border-radius: 3dip;
}
.network-tab-row-active { background: #404d59; }
