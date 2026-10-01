/* Compact native parameter theme. Dimensions are logical DIPs. */
@tokens {
    panel: #383838;
    field: #242424;
    line: #555555;
    accent: #e5ad53;
}
:role(panel), popup { background: token(panel); }
:role(caption) { color: #a4a4a4; }
button, combo {
    background: #4a4a4a;
    border-color: token(line);
    border-radius: 2dip;
    padding: 3dip 6dip;
}
button { text-align: center; }
button:hover, combo:hover { background: #575757; }
button:pressed, combo:pressed { background: #2e2e2e; }
button:selected { background: #635037; border-color: token(accent); }
button.danger:hover { border-color: #de765f; }
number, text_edit {
    background: token(field);
    border-color: #515151;
    border-radius: 1dip;
    padding: 3dip 5dip;
    selection-color: #655038;
}
number:hover, text_edit:hover { border-color: #777777; }
number:focus, text_edit:focus, combo:focus { border-color: token(accent); }
button:focus, checkbox:focus, foldout:focus { border-color: token(accent); }
number:invalid, text_edit:invalid { border-color: #ee7255; }
checkbox, foldout { padding: 3dip 4dip; border-radius: 1dip; }
checkbox:hover { background: #424242; }
checkbox:selected:enabled { color: token(accent); }
foldout.section { background: #303030; }
foldout.section:hover { background: #444444; }
.parameter-list { background: #00000000; }
.parameter-label { padding: 3dip 0dip; text-align: right; }
.parameter-field { padding: 2dip 4dip; }
popup { border-color: #777777; }
menu_item { padding: 3dip 8dip; }
menu_item:hover, menu_item:focus { background: #585858; border-width: 0dip; }
menu_item:selected { color: token(accent); }
:modified:enabled { color: token(accent); }
:disabled { color: #777777; }
