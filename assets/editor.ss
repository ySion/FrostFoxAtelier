/* Native parameter styling, calibrated against Houdini's dark UI. */
@tokens {
    panel: #3d3d3d;
    field: #131313;
    text: #cccccc;
    field-focus: #777777;
    menu-active: #b36600;
    chrome-recess: #2e2e2e;
    button-face: linear-gradient(#555555, #3b3b3b);
    button-hover: linear-gradient(#626262, #414141);
    button-down: linear-gradient(#3a3a3a, #4d4d4d);
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
button:selected { background: #4f4f4f; background-image: none; color: #ffffff; }
/* Focus preserves the control's frame; selection and press own the face. */
button:focus, combo:focus { color: #eeeeee; }
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
number:focus, text_edit:focus { border-color: token(field-focus); border-style: solid; }
number:invalid, text_edit:invalid { border-color: #ef7766; border-style: solid; }
number:modified:enabled { color: #eeeeee; font-weight: 700; }
text_edit:modified:enabled { color: #eeeeee; }
checkbox { padding: 1dip 0dip; border-width: 0dip; indicator-size: 14dip; indicator-background: token(field); indicator-border-color: #3e3e3e; }
checkbox:selected:enabled { color: #dddddd; font-weight: 600; }
checkbox:hover:enabled { color: #dddddd; }
checkbox:focus { indicator-border-color: #969696; }
foldout { padding: 2dip 3dip; indicator-size: 10dip; border-width: 0dip; }
foldout.section { background: #333333; border-width: 0dip; padding: 2dip 5dip; indicator-size: 12dip; font-weight: 400; }
foldout.section:focus { color: #dddddd; }
foldout.section:hover:enabled { background: #4f4f4f; color: token(text); }
foldout.section:pressed:enabled { background: #464646; color: token(text); }
combo { indicator-size: 14dip; indicator-color: #929292; combo-indicator: arrows; }
combo:disabled { indicator-color: #616161; }
.parameter-list { background: #3d3d3d; }
.parameter-page { padding: 4dip 0dip 0dip 2dip; background: #3a3a3a; }
.parameter-inset { padding: 0dip 0dip 0dip 8dip; }
.parameter-body { padding: 7.333dip 0dip 0dip 8dip; background: #3d3d3d; }
.parameter-nested-body { background: #363636; }
.parameter-pane { background: token(panel); }
.parameter-search { padding: 2dip 4dip 3dip 4dip; }
.status-bar { background: #191919; }
.parameter-label { padding: 1dip 0dip; text-align: right; text-vertical-align: center; }
.field-label { text-vertical-align: center; }
list, popup, tree, table {
    scrollbar-color: #3b3b3b; scrollbar-width: 16dip; scrollbar-inset: 1dip; scrollbar-min-thumb: 24dip;
    scrollbar-button-size: 21.333dip; scrollbar-image: linear-gradient(to right, #555555, #3b3b3b);
    indicator-background: token(chrome-recess); indicator-border-color: #494949;
}
list { background: #2d2d2d; }
table {
    background: #2d2d2d; tree-alternate-color: #353535; tree-line-color: #454545;
    selection-color: #55564c; border-color: #858585; border-width: 0dip;
    padding: 0dip; text-vertical-align: center; indicator-size: 16dip; indicator-color: #a7c0cb;
}
table:focus { border-color: #a0a0a0; }
item { padding: 2dip 6dip; border-width: 0dip; }
item:hover, item:focus { background: #424242; }
item:selected { background: #565656; color: #ffffff; }
tree {
    background: #2d2d2d; tree-alternate-color: #363636; tree-line-color: #686868;
    tree-row-height: 20dip; tree-indent: 18dip; padding: 0dip;
}
tree_item {
    background: transparent; border-width: 0dip; padding: 0dip 2dip;
    text-vertical-align: center; indicator-size: 15dip;
    indicator-background: #b8beb5; indicator-border-color: #363936;
    indicator-color: #bacbd0; font-weight: 400;
}
tree_item:hover { background: #414141; }
tree_item:focus { border-width: 0dip; color: #eeeeee; }
tree_item:selected { background: #56564e; color: #ffffff; }
tree_item.actor { indicator-color: #c0c7b2; }
tree_item.transform { indicator-color: #91b6cb; }
tree_item.component { indicator-color: #b7bec3; }
popup { background: #393939; border-color: #949494; border-style: outset; }
popup.menu {
    background: #3a3a3a; border-width: 0.667dip; border-color: #949494; border-shadow-color: #262626;
    border-style: outset; font-size: 13.333dip; scrollbar-gutter: overlay;
}
menu_item { padding: 0dip 9dip; background: transparent; border-width: 0dip; text-vertical-align: center; }
menu_item:hover, menu_item:focus { background: token(menu-active); color: #ffffff; }
menu_item:selected { color: #eeeeee; }
:disabled { color: #808080; }
button:disabled, combo:disabled { background-image: token(button-face); }

/* Pane chrome: one tab strip, one content area, no duplicate panel heading. */
.dock-tabs { background: token(chrome-recess); border-color: #262626; border-width: 0.667dip; border-edges: bottom; border-style: solid; }
.dock-tab-group {
    background: token(chrome-recess); border-color: #171717; border-width: 0.667dip; border-style: solid;
    border-edges: right bottom; padding: 0dip 1dip;
}
.dock-tab-group:selected { background: #494949; border-edges: right; }
tab.dock-tab { background: transparent; border-width: 0dip; padding: 0dip 7dip; color: #cccccc; font-style: normal; }
tab.dock-tab:selected { background: transparent; color: #cccccc; }
tab.dock-tab:hover { background: transparent; color: #cccccc; }
tab.dock-tab:focus { color: #eeeeee; }
.dock-close, .dock-tool, .dock-menu, .dock-add {
    background: transparent; background-image: none; border-width: 0dip; padding: 1dip 2dip; color: #bebebe;
}
.dock-close:selected { background: transparent; background-image: none; color: #d1d3c4; }
.dock-close:hover, .dock-tool:hover, .dock-menu:hover, .dock-add:hover { background: transparent; background-image: none; color: #ffffff; }
.dock-menu { indicator-size: 16dip; color: #c1d6db; }
.dock-add { indicator-size: 16dip; color: #d1d3c4; }
.dock-divider { background: #494949; border-color: #333333; border-shadow-color: #000000; border-width: 0.667dip; border-style: inset; }
.dock-divider.vertical { border-edges: left right; }
.dock-divider.horizontal { border-edges: top bottom; }
.dock-collapse, .dock-grip {
    background: transparent; background-image: none; border-width: 0dip;
    padding: 0dip; color: #909090; indicator-size: 6dip;
}
.dock-grip { indicator-size: 25.333dip; }
.dock-collapse:selected { background: #414141; background-image: none; }
.dock-collapse:hover, .dock-grip:hover { background: #565656; background-image: none; color: #cccccc; }
.dock-collapse:pressed, .dock-grip:pressed { background: #383838; background-image: none; color: #dddddd; }
.dock-collapse:focus { color: #dddddd; }
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
.object-header { background: #3d3d3d; accent-color: #202020; accent-width: 0.667dip; }
.object-type { font-weight: 600; padding: 1dip 0dip; text-vertical-align: center; indicator-size: 20dip; indicator-color: #a7c0cb; }
button.parameter-tool {
    background: transparent; background-image: none; border-width: 0dip; padding: 0dip;
    indicator-size: 22dip; color: #bacbd0;
}
button.parameter-tool:hover { background: #484848; background-image: none; color: #d9e1e4; }
button.parameter-tool:selected { background: #536976; background-image: none; color: #d9e1e4; }
button.parameter-tool:pressed { background: #455965; background-image: none; }
button.parameter-tool:focus { color: #ffffff; }
button.parameter-tool:disabled { background: transparent; background-image: none; color: #686868; }
popup.value-ladder {
    background: #131313; border-color: #818181; border-width: 0.667dip; border-style: solid;
    color: #cccccc; selection-color: #b88c1c; font-family: 'Sarasa UI SC'; font-weight: 400;
    text-align: center;
}
.parameter-tabs { background: token(chrome-recess); accent-color: #565656; accent-width: 0.667dip; }
button.parameter-tab { padding: 2dip 9dip; }
button.parameter-tab, button.parameter-tab:hover, button.parameter-tab:focus {
    background: token(chrome-recess); background-image: none; border-color: #262626; border-style: solid;
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

.menubar-item {
    background: transparent; background-image: none; border-width: 0dip;
    padding: 0dip 7dip; text-align: center; font-size: 13.5dip;
}
.bar-stow {
    background-image: linear-gradient(#555555, #3b3b3b);
    border-color: #111111; border-inner-color: #575757; border-width: 0.667dip; border-style: solid;
    padding: 0dip 2dip; indicator-size: 8dip; icon-align: left; color: #101010;
}
.bar-stow:hover, .bar-stow:focus, .bar-stow:selected {
    background-image: token(button-face); border-style: solid; color: #101010;
}
.bar-stow:pressed {
    background-image: linear-gradient(#5d7389, #748ca5); border-style: solid; color: #101010;
}
button.menubar-item, button.menubar-item:hover, button.menubar-item:focus {
    background: transparent; background-image: none; color: token(text);
}
button.menubar-item:selected, button.menubar-item:pressed {
    background: token(menu-active); background-image: none; color: #ffffff;
}
label.menu-shortcut:disabled { color: #a0a0a0; }
.menu-shortcut { padding: 0dip; text-align: right; text-vertical-align: center; font-style: italic; }
label.menu-shortcut.unavailable:disabled { color: #777777; }
.menu-separator { background: #949494; }

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
slider:focus { indicator-border-color: #cccccc; }
slider:disabled { accent-color: #565656; indicator-background: #444444; indicator-border-color: #606060; }
number:disabled { accent-color: #616161; }

/* Catalog and command shelf share the editor's compact pane vocabulary. */
.tool-shelf { background: #393939; border-color: #202020; border-width: 0.667dip; border-edges: bottom; }
.shelf-tab { font-size: 10.5dip; padding: 0dip 7dip; }
.shelf-tool {
    background: transparent; background-image: none; border-width: 0dip;
    text-align: center; text-vertical-align: bottom; font-size: 10.5dip;
    padding: 1dip 3dip; icon-placement: above; indicator-size: 18dip; indicator-color: #9bbdc8;
}
button.shelf-tool:hover { background: #484848; }
button.shelf-tool:pressed { background: #555555; }
tree_item.mesh-actor { indicator-color: #a7c0cb; }

/* File browser: shared selection and geometry across list/details/tiles. */
.browser-tool { padding: 1dip 5dip; font-size: 11dip; }
.icon-menu, .browser-icon {
    background: transparent; background-image: none; border-width: 0dip;
    padding: 0dip; indicator-size: 20dip; indicator-color: #a7c0cb; combo-indicator: down;
}
combo.icon-menu:hover, .browser-icon:hover { background: #484848; background-image: none; }
combo.icon-menu:selected, combo.icon-menu:pressed, .browser-icon:pressed, .browser-icon:selected { background: #555555; background-image: none; }
.browser-icon { indicator-size: 16dip; }
.browser-icon:disabled, .icon-menu:disabled { background: transparent; background-image: none; indicator-color: #686868; }
.browser-toolbar { background: #343434; }
.browser-path { padding: 0dip 4dip; combo-indicator: down; }
.browser-search-icon { padding: 0dip; indicator-size: 14dip; indicator-color: #aaaaaa; }
.browser-search-input { padding: 1dip 3dip; }
.browser-status { background: #343434; text-vertical-align: center; }
.browser-directories { background: #303030; }


code_editor.source-code {
    background: #242424; color: #cccccc; font-family: "Source Code Pro"; font-size: 12dip;
    padding: 0dip; border-width: 0dip; selection-color: #40566b;
    indicator-color: #868686; indicator-background: #2e2e2e;
    tree-alternate-color: #2b2b2b; scrollbar-width: 12dip;
}
.source-status { font-size: 10.5dip; padding: 0dip 4dip; }
button.source-problem {
    background: #3a2e29; background-image: none; color: #dfb38b;
    border-width: 0dip; padding: 1dip 4dip; text-align: left; font-size: 11dip;
}
button.source-problem:hover { background: #463b34; }

button.source-problem:disabled { color: #dfb38b; background: #3a2e29; }

/* Scene view chrome, sampled from Houdini 22 at UI scale 1.75 (2026-10-08).
   Operation band: #2e2e2e between a black top line and #424242/#666666 bevel.
   Tool rail: #3a3a3a with a black right separator; selected tools #5d7289. */
.viewport-options { background: #2e2e2e; border-color: #030303; border-inner-color: #424242; border-width: 0.667dip; border-edges: top; }
.viewport-option-label { padding: 0dip 3dip; text-vertical-align: center; color: #cccccc; }
.viewport-option-divider { background: #1f1f1f; }
combo.viewport-option-menu { padding: 0dip 8dip 0dip 10dip; combo-indicator: arrows; text-vertical-align: center; }
combo.viewport-option-menu { border-width: 0.667dip; border-style: solid; }
combo.viewport-option-menu, combo.viewport-option-menu:hover {
    background-image: linear-gradient(#555555, #424242); border-color: #1c1c1c;
}
combo.viewport-option-menu:hover { background-image: linear-gradient(#5f5f5f, #4a4a4a); color: #ffffff; }
checkbox.viewport-option-check { padding: 0dip 3dip; text-vertical-align: center; }
.viewport-rail { background: #3a3a3a; border-width: 0.667dip; border-edges: right; border-color: #000000; }
.viewport-mode, .viewport-operation {
    background: transparent; background-image: none; border-width: 0dip; padding: 0dip;
    indicator-size: 22dip; indicator-color: #c4ccd0;
}
button.viewport-mode:hover, combo.viewport-operation:hover { background: #4b4b4b; background-image: none; }
button.viewport-mode { border-style: solid; }
button.viewport-mode:selected, button.viewport-mode:selected:hover {
    background: #5d7289; background-image: none; border-color: #374552; indicator-color: #eeeeee;
}
button.viewport-mode:pressed, combo.viewport-operation:pressed { background: #738ba4; background-image: none; }
/* Passive HUD: white semibold title with a dark drop shadow over the scene. */
.viewport-mode-label {
    color: #ffffff; font-size: 15.5dip; font-weight: 600; indicator-size: 24dip; indicator-color: #e8eef0;
    padding: 0dip; text-vertical-align: center; text-shadow-color: #00000099;
}
.viewport-mode-label:disabled { color: #ffffff; }
/* Floating view pill: translucent slate fill, light hairline, inline caret. */
combo.viewport-view-menu {
    border-width: 0.667dip; border-radius: 11dip; padding: 0dip 9dip 0dip 11dip; combo-indicator: caret; indicator-size: 9dip;
}
combo.viewport-view-menu, combo.viewport-view-menu:hover {
    background: #5e6d75cc; background-image: none; color: #e6e6e6; border-color: #a1a4a5; indicator-color: #e6e6e6;
}
combo.viewport-view-menu:hover { background: #6e7e87dd; color: #ffffff; }
combo.viewport-view-menu:selected, combo.viewport-view-menu:pressed { background: #44535bdd; background-image: none; }
