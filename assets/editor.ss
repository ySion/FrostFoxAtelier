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
