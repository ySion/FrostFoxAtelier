# 编辑器字体

正式界面使用 Adobe 的 Source Sans 3；数字使用 Source Code Pro。文件按上游版本原样保留，未使用 Houdini 自带的修改版字体。

- [Source Sans 3 3.052](https://github.com/adobe-fonts/source-sans/releases/tag/3.052R)：Regular、Semibold、Bold。
- [Source Code Pro 2.042](https://github.com/adobe-fonts/source-code-pro/releases/tag/2.042R-u%2F1.062R-i%2F1.026R-vf)：Regular、Bold。

原始下载地址、固定提交、SHA-256 和文件大小见 [sources.json](sources.json)。两组字体均按 SIL Open Font License 1.1 随程序附带原许可证：

- [Source Sans 许可证](source-sans-LICENSE.md)
- [Source Code Pro 许可证](source-code-pro-LICENSE.md)

CMake 将这些文件复制到程序旁的 `fonts/`。应用通过 UI Session 的私有 DirectWrite 字体集合加载，仍合入系统字体用于缺字和用户字体回退，不进行全局安装。发布时保留字体文件和许可证。
