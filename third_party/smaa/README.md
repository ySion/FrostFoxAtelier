# SMAA reference and lookup data

Upstream: [iryoku/smaa](https://github.com/iryoku/smaa).
Pinned revision: `71c806a838bdd7d517df19192a20f0c61b3ca29d`.
License: [MIT, including the upstream binary-distribution clarification](LICENSE).

Vendored without modification:

- `AreaTex.h` ← upstream `Textures/AreaTex.h`.
- `SearchTex.h` ← upstream `Textures/SearchTex.h`.

`modules/render/renderer/shaders/post_taau_unjitter_cs.hlsl` implements compute SMAA (luma edge detection with `2.0 * delta` local contrast adaptation, exact `SearchTex` + `AreaTex` orthogonal/diagonal pattern search with corner rounding, and neighborhood blending) directly in SM 6.6 Bindless HLSL without runtime filesystem includes.

LUT upload uses `atelier_rhi` persistent device textures (`modules/render/renderer/src/detail/lighting_lut.cpp`):

| Data | Width × height | Format | Tight row pitch | Byte count |
|---|---|---|---|---|
| `smaa_lut::area_bytes()` | 160 × 560 | `R8G8_UNORM` | 320 | 179200 |
| `smaa_lut::search_bytes()` | 64 × 16 | `R8_UNORM` | 64 | 1024 |

No sRGB views, mip generation, channel swizzling or recomputation. Bind the
existing linear clamp sampler; compute texture samples explicitly use LOD 0.
Create/upload these device-lifetime textures once and retire them with the
renderer. LUT bytes and dimensions are available in `modules/render/renderer/src/detail/smaa_lut.hpp`.
