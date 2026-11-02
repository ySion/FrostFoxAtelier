# 天空贴图

`kloppenheim_05_4k.hdr` 是 Poly Haven 的 [kloppenheim_05](https://polyhaven.com/a/kloppenheim_05) 4K 等距柱状 HDRI（Radiance RGBE，4096×2048），作者 Greg Zaal，许可 CC0。它与 Intel Sponza（2022）包内附带的 `textures/kloppenheim_05_4k.hdr` 逐字节相同，SHA-256 为 `DA7BC90E8BB1DD13E6E3C57D7FBA27662BB487A3DEF5229F371DD99AF1459D25`。文件未修改，使用 LFS。

Renderer 独立测量入口 `--scene-lit-benchmark` 用它测量 room 有无天空贴图的耗时，不作为验收或时间稳定性证明。天空贴图的约定见 [Scene Lit](../../../../docs/modules/renderer/scene-lit.md)。

`kloppenheim_05_4k_nosun.hdr` 是它的派生图（CC0），Sponza 场景用它做天空。场景里的太阳由一盏 Directional 光提供；贴图里如果也有太阳，就会经 DDGI 再计入一次。处理方法：以原图最亮纹素中心为太阳方向，即贴图空间的 (0.1556, 0.9640, 0.2157)。贴图空间方向 = (sin λ·cos φ, sin φ, cos λ·cos φ)，其中经度 λ = 2π(u−½)，纬度 φ = π(½−v)。与这个方向夹角小于 2° 的 6119 个纹素，统一换成 2°–4° 环内纹素的逐通道立体角加权均值 (5.108, 5.033, 5.508)，RGBE 编码为 (163, 161, 176, 131)；其余纹素的 RGBE 字节不变。这样去掉了原图 65.7% 的亮度通量（R/G/B 分别为 68.6% / 65.6% / 58.1%）。立体角加权均值从 (0.819, 0.801, 0.834) 变为 (0.257, 0.275, 0.350)，峰值从 125952 变为 13.8。背景里没有太阳圆盘，原来的位置是一块均匀的亮斑。输出 SHA-256 为 `BBAAF2081D6F79632FF950430697CF7E9E6438D4B6E67B821A8E72152AAD1DA0`，使用 LFS。
