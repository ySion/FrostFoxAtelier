/* Editor additions to the shared native theme. Dimensions are logical DIPs. */
@tokens {
    control-radius: 4dip;
    compact-inset: 4dip 6dip;
    danger-border: #de765f;
}
button, number, text_edit {
    border-radius: token(control-radius);
}
button.compact {
    padding: token(compact-inset);
}
button.danger:hover {
    border-color: token(danger-border);
}

:role(caption) {
    color: #939eac;
}
checkbox, foldout {
    padding: 4dip 6dip;
    border-radius: 2dip;
}
foldout.section {
    background: #181e25;
}
foldout.section:hover {
    background: #283441;
}
button.quiet {
    background: #1b232c;
}
.parameter-list {
    background: #00000000;
}
.parameter-label {
    padding: 4dip 0dip;
}
.parameter-field {
    padding: 3dip 4dip;
    border-radius: 2dip;
}
.axis-label {
    color: #a4afbd;
}
:modified:enabled {
    color: #e6ba6e;
}
