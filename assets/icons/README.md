# Editor tool icons

Selected Tabler Outline icons, under the adjacent MIT `LICENSE`. `manifest.json`
records the exact upstream commit, icon mapping, renderer version and the
stroke width applied at generation (`stroke`, default 2; 1.5 matches the
lighter weight of the Houdini UI at editor sizes). The SVGs are unmodified
upstream files (outline set, plus `circle-filled` from the filled set); no
Houdini artwork is included.

Regenerate from the repository root:

```powershell
npm ci --prefix tools/icons
npm run generate --prefix tools/icons
```

The generator writes 128×128 transparent PNGs for inspection and the same alpha
data, run-length encoded, into `src/ui/icon_data.inl`. This checked-in output
keeps ordinary C++ builds offline and avoids adding a PNG decoder or runtime
asset lookup for these small masks. Node and resvg are generation tools only.
The source SVGs are authoritative; do not edit the PNGs or generated C++ data.

`ui::raster_icon` area-samples the embedded masks to the requested physical pixel
size. Controls retain the existing `ControlIcon → TextStore::Symbol → Coverage`
path, theme tint and cache. Native cursors can use the same raster function and
cache their platform handles by DPI. PNG RGB is not used at runtime. The build
copies the upstream license to `LICENSE.tabler-icons.txt` beside other third-party
notices; keep it with distributed builds.

## Reference material

Tabler is the selected source for these tool masks. Simple arrows and checks
continue through the existing procedural Coverage path. Full-color object or
resource artwork would need RGBA content; the current alpha masks do not provide
that capability. Preserve source artwork and its license when adding assets, and
choose variants for the actual physical size rather than stretching one 16px image.

Other libraries are references, not bundled dependencies:

| Source | License and scope |
| --- | --- |
| [Fugue](https://p.yusukekamiyamane.com/) | 16px PNG artwork and source files; [CC BY 3.0](https://creativecommons.org/licenses/by/3.0/) requires attribution, a license link and modification notices. The author also offers a separate commercial license. |
| [Oxygen](https://develop.kde.org/frameworks/oxygen-icons/) | KDE artwork under LGPL 3 or later; follow the project's [distribution guidance](https://techbase.kde.org/Projects/Oxygen/Licensing) and retain the selected assets' sources and notices. |
| [Fluent System Icons](https://github.com/microsoft/fluentui-system-icons) | General-purpose SVG symbols under [MIT](https://github.com/microsoft/fluentui-system-icons/blob/main/LICENSE); a different visual language from colored desktop object icons. |
