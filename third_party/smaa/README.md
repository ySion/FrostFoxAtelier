# SMAA reference and lookup data

Upstream: [iryoku/smaa](https://github.com/iryoku/smaa).
Pinned revision: `71c806a838bdd7d517df19192a20f0c61b3ca29d`.
License: [MIT, including the upstream binary-distribution clarification](LICENSE).

Vendored without modification:

- `SMAA.hlsl` ← upstream `SMAA.hlsl`.
- `AreaTex.h` ← upstream `Textures/AreaTex.h`.
- `SearchTex.h` ← upstream `Textures/SearchTex.h`.

`modules/render/renderer/shaders/lit_smaa.hlsl` provides compute wrappers using the reference
weight calculation and neighborhood blending. The first pass ports the original
luma edge equations to explicit scene-linear-to-sRGB loads and zero stores
instead of pixel-shader discard. The temporal pass keeps two independent spatial
SMAA frames, with this engine's expected-depth/normal disocclusion checks.
`cmake/renderer.cmake` embeds the pinned reference at the wrapper's source marker;
runtime shader compilation requires no filesystem includes.

LUT upload uses the existing RHI/Gfx texture upload path:

| Data | Width × height | Format | Tight row pitch | Byte count |
|---|---|---|---|---|
| `smaa_lut::area_bytes()` | 160 × 560 | `R8G8_UNORM` | 320 | 179200 |
| `smaa_lut::search_bytes()` | 64 × 16 | `R8_UNORM` | 64 | 1024 |

No sRGB views, mip generation, channel swizzling or recomputation. Bind the
existing linear clamp sampler; compute texture samples explicitly use LOD 0.
Create/upload these device-lifetime textures once and retire them with the
renderer. LUT bytes and dimensions are available in `modules/render/renderer/src/detail/smaa_lut.hpp`.
