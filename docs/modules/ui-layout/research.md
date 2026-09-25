# 布局候选研究与证据

日期：2026-09-25。本页记录固定来源与证据边界；不是“最新版本”清单，也不是性能排名。正式依赖仍以仓库既有 `cmake/dependencies-lock.json` 为唯一权威；本次没有更新它。

## 1. 采用方向与版本

### Yoga：选定的第一实现基础

| 项目 | 本轮值 |
|---|---|
| 官方版本 | v3.2.1 |
| 固定 commit | `042f5013152eb81c1552dec945b88f7b95ca350f` |
| 官方源码归档 | `https://codeload.github.com/react/yoga/tar.gz/042f5013152eb81c1552dec945b88f7b95ca350f` |
| Main 下载 SHA-256 | `4742f41722a16f181e3da37abf943390db1e928f00f26402cb154662ae7f110f` |
| 归档长度 | 2,176,227 bytes |
| 许可 | MIT；保留原始 LICENSE 和上游来源 |
| 研究使用 | 仅 out 内源码和构建；没有改变正式依赖 |

出处：[yoga-provenance.json](../../../out/review/wave1-meeting/yoga-provenance.json)。facebook/yoga 官方 URL 现重定向 react/yoga；原始 tag ref 已核对到上述 commit，不靠仓库名猜版本。正式接入者应把此 pin、URL、hash 与许可按现有锁/打包约定落地，而不是从此文动态选“新版本”。

Yoga 核心 C++20，`yoga/` 子目录 CMake 生成静态 yogacore；不需要 JS/Java 绑定、Yarn、Chrome fixture generator 或上游测试框架。引擎消费端继续既有 C++23 与原生工具链。上游可用 C 函数 Interface 不意味着引擎必须把全部 setter 或 Yoga 指针公开。

### Taffy：保留的机制对照，不新增依赖

固定 0.9.1；annotated tag `32bf7a96d6aa0726005e77c469579f8b2394de09` 对应 commit `fa5930c84567545f55e6f848ca655aa9eec9bb75`。MIT，LICENSE.md 说明 stretch/Visly fork 来源。Cargo manifest 使用 Rust 2021、rust-version=1.65；strict_provenance 特性另需 Rust 1.84。高层 TaffyTree 管理树/缓存；低层 traits 可接调用者自有树，但缓存和算法 dispatch 由调用者负责。

若因真实 Grid/自有树需求重新选它，必须另行锁 Rust、精确 crate 和传递依赖，并定义 panic/分配释放的 C ABI；MSRV 不是已选择/测试的编译器版本。当前没有安装、编译或测量 Taffy。

## 2. 一手来源

访问日期均为本页日期。Yoga/Taffy 使用固定版本源码；Microsoft 和 yogalayout.dev 是持续文档，机制解释需与 pin 源码对应。未采用流行度、二手性能榜或搜索摘要作选型证据。

### Yoga

- [官方 ref](https://api.github.com/repos/react/yoga/git/ref/tags/v3.2.1)、[README](https://raw.githubusercontent.com/facebook/yoga/v3.2.1/README.md)：版本、C++/构建范围。
- [YGNode.h](https://raw.githubusercontent.com/facebook/yoga/v3.2.1/yoga/YGNode.h)、[YGNode.cpp](https://raw.githubusercontent.com/facebook/yoga/v3.2.1/yoga/YGNode.cpp)：真实节点创建/释放/计算/孩子修改；MarkDirty 限自定义测量叶。
- [外部测量](https://www.yogalayout.dev/docs/advanced/external-layout-systems)、[增量布局](https://www.yogalayout.dev/docs/advanced/incremental-layout)：exact/at-most/undefined、外部内容失效、父约束变化及 HasNewLayout 的适用范围。
- [YGConfig.h](https://raw.githubusercontent.com/facebook/yoga/v3.2.1/yoga/YGConfig.h)、[config/Config.cpp](https://raw.githubusercontent.com/facebook/yoga/v3.2.1/yoga/config/Config.cpp)：scale=0 禁舍入，配置版本及显式 defaults/errata。
- [PixelGrid.cpp](https://raw.githubusercontent.com/facebook/yoga/v3.2.1/yoga/algorithm/PixelGrid.cpp)、[node/Node.cpp](https://raw.githubusercontent.com/facebook/yoga/v3.2.1/yoga/node/Node.cpp)：局部位置/绝对尺寸舍入，以及 measure 默认赋 Text 类型；这不是一般图片/box 的隐含政策。
- [BoundAxis.h](https://raw.githubusercontent.com/facebook/yoga/v3.2.1/yoga/algorithm/BoundAxis.h)、[FlexLine.cpp](https://raw.githubusercontent.com/facebook/yoga/v3.2.1/yoga/algorithm/FlexLine.cpp)：padding 下限、min/max、grow 总量及临时容器。成熟实现不自动等于零分配或无深度限制。
- [核心 CMake](https://raw.githubusercontent.com/facebook/yoga/v3.2.1/yoga/CMakeLists.txt)、[构建默认值](https://raw.githubusercontent.com/facebook/yoga/v3.2.1/cmake/project-defaults.cmake)、[LICENSE](https://raw.githubusercontent.com/facebook/yoga/v3.2.1/LICENSE)。

### Taffy

- [0.9.1 Interface](https://docs.rs/taffy/0.9.1/taffy/)、[TaffyTree](https://docs.rs/taffy/0.9.1/taffy/struct.TaffyTree.html)：高低层 Interface 的责任差别。
- [Cargo.toml](https://raw.githubusercontent.com/DioxusLabs/taffy/v0.9.1/Cargo.toml)、[LICENSE.md](https://raw.githubusercontent.com/DioxusLabs/taffy/v0.9.1/LICENSE.md)、[annotated tag](https://api.github.com/repos/DioxusLabs/taffy/git/tags/32bf7a96d6aa0726005e77c469579f8b2394de09)。
- [cache.rs](https://raw.githubusercontent.com/DioxusLabs/taffy/v0.9.1/src/tree/cache.rs)：known_dimensions、available_space、run_mode 和多测量槽，不以指针单键代替失效。
- [round_layout](https://docs.rs/taffy/0.9.1/taffy/fn.round_layout.html)：用累积绝对边界和未舍入输入，宽高由舍入边界相减；可借鉴政策，不要求复制实现。

### Windows 真实测量与渲染接口

- [CreateTextLayout](https://learn.microsoft.com/en-us/windows/win32/api/dwrite/nf-dwrite-idwritefactory-createtextlayout)、[GetMetrics](https://learn.microsoft.com/en-us/windows/win32/api/dwrite/nf-dwrite-idwritetextlayout-getmetrics)、[DWRITE_TEXT_METRICS](https://learn.microsoft.com/en-us/windows/win32/api/dwrite/ns-dwrite-dwrite_text_metrics)：真实排版、DIP 单位；advance/layout box 不是 glyph ink/overhang。
- [D3D12_VIEWPORT](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_viewport)、[RSSetScissorRects](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-rssetscissorrects)、[RECT](https://learn.microsoft.com/en-us/windows/win32/api/windef/ns-windef-rect)：viewport 为 float、scissor 边界为整数，反驳“所有 GPU 坐标都应相同像素舍入”的扩大解释。

## 3. 本机实测来源与限制

Main 统一构建/运行，本作者未执行。原始材料：

- [探针源码](../../../out/review/wave1-meeting/ui-layout-path-probe.cpp)。已验证后冻结，本轮文档补全不再修改。
- [运行 JSON](../../../out/review/wave1-meeting/ui-layout-path-probe-headless-layout.json)：exitCode=0、signal=null、stderr为空；完整输出见 acceptance。
- [实际编译/链接命令](../../../out/review/wave1-meeting/ui-layout-path-probe-build.json)。
- [Yoga 构建日志](../../../out/review/wave1-meeting/yoga-core-build.log)、[下载来源](../../../out/review/wave1-meeting/yoga-provenance.json)。

**必须区分实际命令与研究建议命令**：yogacore 已独立 CMake 构建成功；原始 probe 使用正式 release host 的 C++23、`/MD`、`/W4 /WX` 等参数，链接行复用 mimalloc 覆盖及 host Runtime/JIT 库列表。这个原始结果本身不足以证明路径独立。

Main 随后保持同一已冻结源码，去掉 Runtime/JIT 静态及导入库，只保留当前 global new/mimalloc、yogacore、dwrite 和 Windows 系统库，**精简链接再次运行全部通过**。新增来源：[ui-layout-minimal-build.json](../../../out/review/wave1-meeting/ui-layout-minimal-build.json)、[ui-layout-minimal-headless-layout.json](../../../out/review/wave1-meeting/ui-layout-minimal-headless-layout.json)。这关闭了该真实候选源码需要 Runtime/JIT 链接的疑问，但仍是 out 中的实验 exe，不是已实现的布局核心 target，不证明 Windows-free 或 allocator-free。正式核心依赖需下一里程碑在真实 target/consumer 上再次验收。

源码来源、真实结果和将来合同是不同等级：

| 等级 | 本轮内容 |
|---|---|
| 本仓契约/源码 | Runtime 统一业务身份、根不等于 NodeId；当前尚无布局生产 target |
| 上游源码证据 | Yoga/Taffy 的测量、树、缓存、舍入及许可机制 |
| 本机实测 | 指定 Yoga/DirectWrite fixture 的几何、dirty、DPI热冷与嵌套观察 |
| 设计要求，尚未实现 | Document owner/generation、原子快照发布、批量验证、窄规则完整错误面 |
| 未测 | 性能、深树上限、跨OS/CPU重现、全字符 shaping、未来业务语言绑定 |

原始运行 elapsed_ms=47.77549999952316，精简运行=48.21009999513626，都是整个一次性进程用时，包含启动、字体等工作；**不是布局耗时基线、性能比较或预算**。两次输出相同，text measure_calls=5 是该程序路径总数，不是普遍测量上限。系统 Segoe UI 文件/dwrite.dll 未由 SDK pin 自动冻结，不使用本次文字宽高作跨机器 golden。

## 4. 与现有仓库的契约来源

[工程指南](../../engineering.md)、[语言契约](../../language-contract.md)、[术语](../../../CONTEXT.md)、[Runtime 声明](../../../src/runtime/runtime.hpp) 决定所有权和业务身份；布局不覆写这些契约。完整对抗过程在 [个人报告](../../research/wave1-meeting/parallel-modules/ui-layout.md) 第7节，最终提炼见 [decisions.md](decisions.md)。正式锁升级时重查上游许可、配置差异及本模块全部可观察验收；不要只换版本号沿用本轮 PASS。
