# 编辑器字体

全局 UI 使用 **Sarasa UI SC（更纱黑体 UI 简体中文）1.0.42**，数值字段保留 **Source Code Pro 2.042**。

## 选择依据

- [Sarasa UI SC](https://github.com/be5invis/Sarasa-Gothic#what-are-the-names)：西文基于 Inter，中文基于思源黑体；UI 变体使用窄引号。一套字体覆盖编辑器的中英文标签、对象名称和菜单，避免日常中文依赖机器上的默认回退字体。
- [Inter](https://rsms.me/inter/)：小字号界面可读性好，但单独使用仍需补上中文；本项目通过 Sarasa 使用它的西文字形。
- [Source Han Sans](https://github.com/adobe-fonts/source-han-sans)：完整的东亚无衬线方案；本轮选择已有中西文整合的 Sarasa UI 变体。
- [Source Sans 3](https://github.com/adobe-fonts/source-sans)接近 Houdini 西文参照，但中文依赖系统回退；本项目选择统一中英文正文字形，不部署该字体。

这些是本编辑器的视觉与部署取舍，不意味着某款字体普遍优于其他字体。Houdini 自带的修改版字体和图标未被复制。

## 字体层级与成本

正文、参数和 Tab 为 12.5 DIP，顶部菜单为 13.5 DIP，标题沿现有 title_scale 放大。Tab 直接使用全局字号，不另设固定字号；折叠标题使用 Regular 400。固定视图名称（包括 Scene View）统一使用正体，以灰色底面表达选中状态；不将 Houdini 路径标签的斜体套用于固定名称。按实际小写字高重新校准字号，布局尺寸及系统 DPI 入口不变。

仅部署 Sarasa 的 Regular 400 和 SemiBold 600，两份原始 TTF 合计 **48,028,368 字节（约 45.8 MiB）**。未携带其他地域、等宽变体或整个十字面下载包；未裁剪中文字符集。斜体由 DirectWrite 合成。数字使用 Source Code Pro 的 Regular/Bold，让符号、位数和拖动数值保持等宽。

字符轮廓在私有 DirectWrite 集合中使用；UI 仍只栅格化可见文字、缓存已使用的字形覆盖图，不将整份字体作为 GPU 纹理上传。文件体积不等于每帧内存分配或实测常驻内存。

## 来源与部署

- [Sarasa 1.0.42 官方发布](https://github.com/be5invis/Sarasa-Gothic/releases/tag/v1.0.42)：`SarasaUiSC-TTF-1.0.42.7z` 中的两份原始文件；归档 SHA-256 已与官方 asset digest 核对。
- [Source Code Pro 官方发布](https://github.com/adobe-fonts/source-code-pro/releases/tag/2.042R-u%2F1.062R-i%2F1.026R-vf)：Regular、Bold。

下载地址、归档成员、SHA-256 和文件大小见 [sources.json](sources.json)。两组字体按 SIL OFL 1.1 随程序附带原许可证：[Sarasa](sarasa-LICENSE.txt)、[Source Code Pro](source-code-pro-LICENSE.md)。字体文件未修改。

CMake 将清单所用文件复制到程序旁的 `fonts/`；`editor_fonts()` 给主窗口和原生弹出菜单提供同一字体集合，私有加载使用 [DirectWrite AddFontFile](https://learn.microsoft.com/en-us/windows/win32/api/dwrite_3/nf-dwrite_3-idwritefontsetbuilder1-addfontfile)。系统字体继续服务额外缺字及 emoji 回退，不进行全局安装。发布时保留字体文件和许可证。

字体快照、单行编辑器与参数组合由[编辑器验收入口](../../docs/modules/editor/README.md#验收入口)覆盖；普通文字或窗口检查不替代真实系统 IME 验收。
