# Editor tool icons

Selected Tabler Outline icons, under the adjacent MIT `LICENSE`. `manifest.json`
records the exact upstream commit, icon mapping and renderer version. The SVGs
are unmodified upstream files; no Houdini artwork is included.

Regenerate from the repository root:

```powershell
npm ci --prefix tools/icons
npm run generate --prefix tools/icons
```

The generator writes 128×128 transparent PNGs for inspection and the same alpha
data, run-length encoded, into `src/ui/icon_data.inl`. This checked-in output
keeps ordinary C++ builds offline and avoids adding a PNG decoder or runtime
asset lookup for these ten small masks. Node and resvg are generation tools only.
The source SVGs are authoritative; do not edit the PNGs or generated C++ data.

`ui::raster_icon` area-samples the embedded masks to the requested physical pixel
size. Controls retain the existing `ControlIcon → TextStore::Symbol → Coverage`
path, theme tint and cache. Native cursors can use the same raster function and
cache their platform handles by DPI. PNG RGB is not used at runtime. The build
copies the upstream license to `LICENSE.tabler-icons.txt` beside other third-party
notices; keep it with distributed builds.
