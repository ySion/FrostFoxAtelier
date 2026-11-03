# UI 绘制与样式研究

资料查阅日期：2026-10-01。本目录保留外部框架的机制、来源和证据限制，不作为本项目的实施清单。当前行为由 [UI](../../modules/ui/README.md)、[SS](../../modules/ui/style-sheets.md)及 [Renderer 合同](../../modules/renderer/contract.md)维护。

| 材料 | 内容 |
| --- | --- |
| [Unity UI Toolkit](unity-ui-toolkit.md) | 分类更新、布局结果比较、UIR 合批、atlas、clip、mesh、文本与 panel 生命周期 |
| [Qt / RmlUi / ImGui](native-renderers.md) | 绘制表示、CPU/GPU 缓存、顺序、字体、资源、线程合同与许可 |
| [USS / Godot / Slate 样式](style-systems.md) | 样式来源、继承、状态、token、变化分类和声明式表达的成本 |

Unity 使用 6000.0 文档与固定的 6000.7.0b2 预览源码，两者不是同一稳定合同；其他框架的版本与固定源码见各材料。研究区分官方接口、特定源码实现与设计推论，不以机制分析代替性能测量，也不从参考源码推导复制或再分发许可。
