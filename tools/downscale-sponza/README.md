# New Sponza 2K 资源

把 Intel New Sponza（2022）的 glTF 版本离线缩成 2K，生成 `assets/examples/Examples/Sponza/` 下的 `NewSponza/`（主体）和 `SponzaExtend/`（布帘）。工具依赖 Python 3、numpy 和 Pillow，只在离线转换时需要；编辑器运行和普通构建不依赖它们。

在仓库根目录执行，源目录是解压后的原始发布包：

```powershell
python tools/downscale-sponza/downscale.py D:/NewSponza/NewSponza/NewSponza_Main_glTF_003.gltf assets/examples/Examples/Sponza/NewSponza --flip-normal-green
python tools/downscale-sponza/downscale.py D:/NewSponza/SponzaExtend/NewSponza_Curtains_glTF.gltf assets/examples/Examples/Sponza/SponzaExtend --flip-normal-green
```

`.gltf`、`.bin` 和 `credits_license.txt` 按字节复制，不做任何改动。只处理文档实际引用的图片，按原路径、原文件名写出 2048² PNG，glTF 里的 uri 不用改；没有被引用的图片不复制。缩小用 2×2 box，图片按材质里的用途分三类处理：

- baseColor、emissive：先把 sRGB 转成线性值再平均，最后编码回 sRGB；alpha 直接线性平均。
- metallicRoughness、occlusion 等线性数据：直接平均。
- 法线：先解码成向量再平均，然后重新归一化。

源包的法线图用的是 DirectX 约定（绿色朝下），和 glTF 规定的 +Y 朝上相反，所以需要加 `--flip-normal-green`，在解码后把 Y 取反。判断依据是对 23 张主体法线图做的两项检验：可积性残差，以及砖缝、瓦缝凹陷造成的斜率不对称。两项都判为 DirectX，起伏明显的贴图结论最清楚。glTF 导入器本身严格按规范读取，不对这类数据做特判。

输出目录里的 `source.json` 记录每个文件的源 SHA-256 和输出 SHA-256、生成命令、滤波方式、绿色通道的处理，以及 Python、numpy、Pillow、zlib 的版本。在同一套工具版本下重新生成，输出字节完全一致。`--jobs` 控制并行进程数，默认是逻辑核数的一半，最多 8 个。

发布资产中的 PNG 和 `.bin` 使用 Git LFS，`.gltf` 和许可文件禁止换行转换，见 `assets/examples/.gitattributes`。场景、许可和数据规模见[示例说明](../../assets/examples/Examples/Sponza/README.md)。
