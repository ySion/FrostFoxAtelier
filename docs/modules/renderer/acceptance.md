# 编辑器 renderer 验收

## 背景与参考网格（2026-10-05）

本轮在 `viewport-rendering/FrostFoxAtelier` 工作树、基线 `855433a840584b414073758d6587f5ce6ae66703` 上实现。以下是 Guides 独立生产者的验收，首次执行时的源码指纹保存在 `out/reference-guides-20261005/source-manifest.json`；基线 SHA 不包含本轮代码。原生 UI 语言 M1 位于另一工作树，两片组合后的实际接点由应用负责人统一验收，不能用这里的独立 PASS 代替。

| 验证 | Dev | Release |
| --- | --- | --- |
| 正式 Renderer / editor 构建 | PASS | PASS |
| 既有 editor_document_acceptance，含 Guides 栏三档 DPI 布局 | PASS | PASS |
| renderer_acceptance：既有模式、真实 GPU 背景/网格/轴线、遮挡和深度 | PASS | PASS |
| editor_viewport_acceptance：正式原生菜单、实际画面、显示独立性和 World TRS | PASS | PASS |

GPU 专项覆盖空 World 渐变/网格、独立轴线、实体与 Visible/All 线框遮挡、共面真实地面优先、删除最后物体后深度全 1、有限奇异相机仅跳过网格、相机在平面/上下方及远处、零 viewport、非法设置编码前拒绝。新增 `viewport_draw_calls` 单独计算辅助显示，不改变原物体 draw 统计。既有 UI-only、矩阵、网格版本、图像、resize、最小化与释放检查继续通过。

正式窗口先捕获空场景，再通过真实 Guides 菜单开关网格、轴线和渐变，检查颜色变化及场景深度、geometry/material/transform、选择和 undo 不变；继续切换 MatCap/Smooth/Flat 与全部线框模式。Release 截图场景区域独立 RGB 复核，网格开关有 96,766 像素通道差异大于 3；比较区域不含显示栏文字。两配置 `out/<preset>/viewport-evidence/acceptance.txt` 均为 PASS，图片包括 `empty-reference-grid.png`、`reference-grid-{off,on}.png`、`origin-axes-off.png`、`gradient-off.png` 和既有模式/TRS 图片。

最终 GPU 日志为 `out/reference-guides-20261005/{dev,release}-renderer-coplanar.log`，窗口日志为 `{dev,release}-viewport.log`，最终日志无 D3D12 warning/error。开发过程中修正了 HLSL 保留标识符，并将空场景 shader 的深度输出移除，避免无深度附件时的调试层警告；使用相同网格算法的有/无深度变体，无新增 RHI 能力或原生图形 API。

本轮没有实现 Play 或真实场景光照，但已明确两者独立：Edit/Play 不决定 MatCap/未来 Scene Lit 的选择；背景与参考辅助显示也不绑定着色模式。用户视口偏好尚不跨进程保存，没有新增工作平面编辑、刻度文字或角落方向标。没有声称已测得 GPU 帧耗时/显存改进；未重跑无关 Project 全窗口、Unicode 剪贴板或系统对话框专项。

提交前的 Standards/Spec 双轴审查及 RHI 负责人接点检查未发现确认的生产代码结构、幂等或生命周期缺陷；修正了前代“待验/等待激活”状态、显示菜单数量与研究文档的过时链接。审查后 15 份源代码、构建定义与验收文件的 SHA256 均与上述双配置 PASS 快照一致，改动仅为文档，因此沿用原端到端证据，没有重复启动窗口。低视角/远距检查证明深度与数值边界保持，未对这些角度的视觉质量做完整量化评估。

## 视口显示增量

2026-10-04，`codex/viewport-rendering` 基于 `bba15954` 实现 MatCap、Smooth/Flat、实体叠线、隐藏线与全线框、原始多边形边/三角形边选择。前代功能最终已随 PR #11 发布到主线 `855433a840584b414073758d6587f5ce6ae66703`；实际完整窗口验收代码为 `aa9b310c619beec1cc3c21a1b000f047ce2f3233`，见[最终集成记录](../../research/editor-product-integration-20261005.md)。以下列出各项当时的真实来源，首错及中间中断证据保留在后面的历史轨迹中。

| 验证 | Dev | Release |
| --- | --- | --- |
| 正式 editor、renderer、专项验收消费者编译 | PASS | PASS |
| Project/Workspace/editor 文档端到端链 | PASS | PASS |
| World 生命周期、空间树与 StaticMeshActor | PASS（8/8） | PASS（8/8） |
| 视口显示栏 1×/1.5×/2× DPI 布局 | PASS | PASS |
| 新场景 VS/PS，DXC Shader Model 6.5 编译及 GPU 结构布局核对 | PASS（两配置共享同一 HLSL） | 同左 |
| Renderer 真实 GPU 颜色/深度与模式矩阵 | PASS（2026-10-05，5c1f21a） | PASS（2026-10-05，5c1f21a） |
| 正式编辑器原生菜单切换、MatCap/线框画面与 World TRS 投影 | PASS（aa9b310，2a7e 集成树） | PASS（aa9b310，2a7e 集成树） |

窗口 fixture 会显示/恢复窗口，编辑器消费者还会执行窗口内输入。原后台日志在视口工作树 `out/viewport-integration/`，Renderer GPU 日志在下述 `math-project-sync` 集成树；最终正式窗口证据来自应用 `2a7e` 集成树，来源及固定 SHA 详见最终集成记录，不以编译或 CPU 布局结果替代。

最初阶段同步合入 `a57cb0ca5cd15aef61e79cef42513105b1c9bd3e`，后台验证代码为 `f70d4e5e5f1059764a6ea35c6f2c1f3205a97d36`。执行 `atelier_renderer_acceptance atelier_editor editor_document_acceptance world_acceptance`：前两项仅编译，后两项编译并运行，双配置均退出 0；日志为 `stage-sync-dev.log`、`stage-sync-release.log`。该阶段未修改 RHI，后续真实 GPU 执行发现的根签名缺口由 RHI 负责人修复，见下。World 已确认边标记与资源来源键恢复兼容，没有更改 Scene 格式。

已接好的可重复入口（会显示真实窗口，需依次执行）：

```powershell
. ./tools/enter-dev.ps1
cmake --build --preset dev --target renderer_acceptance
cmake --build --preset dev --target editor_viewport_acceptance
# release 使用相同目标，对应 --preset release。
```

Renderer 消费者检查 MatCap 对基础色的覆盖与视空间响应、Flat/顶点法线差异、多边形对角线过滤、全场景遮挡、纯线模式深度、线宽与透视距离、无效参数的编码前拒绝、异步模式切换；沿原消费者继续执行矩阵、网格版本、UI 与关停验收。正式编辑器专项仍使用 `atelier_editor`，通过真实资源浏览器创建 sphere、点击共享原生下拉菜单切换各显示模式，检查几何/材质引用、矩阵、选中对象与撤销历史保持不变，并复验 World TRS 到 GPU 的投影。专项图片将保存在 `out/<配置>/viewport-evidence/`。

当前只内置一张程序生成的中性陶土 MatCap；没有新增自定义图片导入、Ghost 淡显、AO 或 PBR。OBJ 输入继续沿原三角形合同，不能恢复源文件未保留的多边形边。尚无本轮帧耗时或显存性能测量结论。

### 2026-10-05 集成历史：首错、修复与最终收口

最初在 `math-project-sync/FrostFoxAtelier` 的固定 `495bf513c4f3cb7e7a2a0ab9506559e34ee3f3c7` 开始执行；本小节前半段的程序与日志来自该树。中间“未完成”均为当时状态，最终收口见本小节末。

首次 Dev Renderer 的纯 UI 部分通过，场景管线创建因根签名超过 64 DWORD 失败。RHI 负责人修复了各 shader stage 根常量与描述符表成本没有聚合的缺口，复用既有 CBV 常量池回退；修复提交为 `fb77b6a04063cb3e1df7f6d6e1872a9804890cc2`，直接父为上述固定候选。集成树正常快进该提交后双配置重新编译，Dev `renderer_acceptance` 完整 PASS，真实硬件为 AMD Radeon RX 6800 XT，启用 D3D12 debug/GBV。覆盖矩阵、任意网格、MatCap、线框、隐藏线、选择、线宽及资源寿命；日志未报告 D3D12 warning/error。

随后 Dev `editor_viewport_acceptance` 因前台焦点检查退出 1，未生成模式 PNG。原错误共用于初始激活失败和运行中失焦，因此该次日志不能断言用户曾切走。验收诊断提交 `e9c2210010fcf9d8f66d671e92c6898deab7132e` 仅区分初始 20 秒未获前台与运行中失焦，原输入保护不变。

用户确认时段后，重跑成功执行原生菜单并生成 MatCap、Smooth、Flat PNG，在 Flat/Smooth 画面断言失败。截图中的球体约 70 像素宽，选中边线覆盖大量表面；实际 RGB 各通道差异大于 3 的像素分别为 33、180、366，而原判据只统计红色通道，要求超过 100。提交 `5c1f21a29fd281cca6e9b87545d46e7c318dd48d` 通过既有 Focus 命令和滚轮放大验收球体，比较全部 RGB 通道，保留差异阈值与文档/深度不变检查；没有修改生产渲染行为。两配置编译通过。其后一次启动明确报告初始 20 秒未获前台，未发送合成输入；该结果与前一次画面断言失败分别记录。

同一固定 `5c1f21a` 上 Dev/Release `renderer_acceptance` 均完整 PASS，日志未报告 D3D12 warning/error；当时正式编辑器专项尚未完成，因此该阶段没有将 Renderer 结果记作编辑器输入链 PASS。

为避免手动激活窗口短暂出现即退出，随后将同一验收入口的初始等待延长到 120 秒，并显式记录 `WAITING FOR ACTIVATION` 与 `RUNNING`。等待期间继续 `app.step()` 泵消息，直接检查 Win32 当前 foreground/focus，激活后在同一进程继续；超时记录 editor/foreground/focus HWND 与 foreground PID。只尝试一次前台激活，运行中失焦保护不变。诊断改动的双配置编译日志为 `{dev,release}-activation-state-build.log`，随后由应用接手统一窗口验收。

证据目录为该集成树 `out/viewport-validation-20261005/`：首次失败 `dev-renderer-initial.log`、修复后 `dev-renderer.log`、最终双配置 `dev-renderer-final.log` / `release-renderer.log`、首次编辑器中断 `dev-viewport.log`、真实画面断言失败 `dev-viewport-retry.log` 与 `small-sphere-evidence/`、放大 fixture 后初始前台拒绝 `dev-viewport-framed.log`，以及两配置重建 `{dev,release}-build-rhi-fix.log` 和 `{dev,release}-activation-diagnostic-build.log`。RHI 自有树的双配置离屏回归是底层补丁证据，不代替本集成树的窗口结果。旧 Project 普通窗口和 Unicode 剪贴板问题未在本专项修复或验收。

最终应用 `2a7e` 树在 `aa9b310` 上完成 Dev/Release 统一 `--acceptance-project` 全链 PASS，包含同一 `verify_viewport_display` 与 World TRS。Project 叠线判据改为比较未选中网格，避免把选中对象 Off/Overlay 原本相同的轮廓误判为缺陷；两配置观察到 1,126 个 RGB 变化像素。最终文档提交 `1d79598` 没有生产代码变化，PR #11 已发布到 `855433a`。证据路径和仍未覆盖的 Unicode/普通系统对话框边界见[正式集成收口](../../research/editor-product-integration-20261005.md)。

## 历史验收

2026-09-28，Windows x64、AMD Radeon RX 6800 XT，dev 与 release 的正式 `renderer_acceptance` 均通过。fixture 使用真实 HWND、正式 Renderer、D3D12 debug layer 与 GPU validation；不使用 mock。最终程序输出必须包含 `Renderer acceptance: PASS` 且退出 0，runner 保留日志并限制进程为 120 秒。

## 可重复入口

应用根构建先创建 RHI 与 `atelier_ui_protocol`，再 include `cmake/renderer.cmake` 并调用 `atelier_add_renderer()`。然后：

```powershell
. ./tools/enter-dev.ps1
cmake --preset dev
cmake --build --preset dev --target renderer_acceptance
cmake --preset release
cmake --build --preset release --target renderer_acceptance
```

本次独立 RHI worktree 未修改由应用拥有的根 CMake；通过忽略目录 `out/renderer-development.cmake` 的 CMake DEFER 在既有根构建末尾调用同一 `atelier_add_renderer()`，用 `CMAKE_PROJECT_FrostFoxAtelier_INCLUDE` 注入。没有复制生产实现、建立另一个产品入口或引用其他 worktree；应用集成后使用上面的普通 presets 命令。

## 已核对的结果

| 行为 | 真实观察 |
| --- | --- |
| 窗口格式 | renderer 显式请求 RGBA8，打开时检查实际 RTV 为 `R8G8B8A8_UNORM_SRGB`；当前 10 位显示器环境下打开成功 |
| 颜色与独立深度同时绑定 | 两个相同屏幕位置的实例，先画近红块再画远蓝块；中心颜色保持红，深度为 0.15，空白深度为 1 |
| 相机矩阵 | 非对角 view 平移与左手透视矩阵，中心读回深度符合 `10/9.9 - 1/(9.9*3.5)` |
| 选中与 UI 覆盖 | 选中后颜色变为金色；半透明 UI 覆盖场景，场景深度不变 |
| 线性颜色与混合 | 绿色上覆盖半透明红色，sRGB 读回 R/G 约为 188；半开 scissor 外保持背景 |
| R8 内容 | CPU 行距 5、有效宽度 3，验证 padding 没进入纹理；白色覆盖与透明部分可区分 |
| 内容版本与退休 | 相同 identity 的新 revision 改变像素；12 帧普通异步绘制后旧 CPU owner 已释放，新像素正确 |
| 任意窗口宽度 | 641×385 与 703×427 的 RGBA8、float32 深度经对齐 GPU 行距读回为紧密 CPU 行，指定像素均正确 |
| resize 接受 | resize 当帧 accepted/capture 仍为 641×385，next extent 为 703×427；旧 extent 再提交 skipped，新帧重建深度后 accepted |
| 窗口生命周期 | 最小化与隐藏均 skipped；恢复后继续 accepted；关闭 renderer 后才销毁 HWND，重复 close 成功 |
| 输入与 owner | 非有限矩阵在编码前拒绝且可恢复；异线程 target 查询拒绝 |
| 调试层 | renderer 检查编码/完成/关停后的 InfoQueue；本次两种配置日志无 D3D12 warning/error |

证据在各配置 `out/<preset>/renderer-acceptance.log` 与 `renderer-acceptance.ppm`；这些本机文件不随仓库提交。窗口是端到端验收 fixture，正式产品窗口由 application 唯一拥有。fixture 没有文字排版与用户控件；真实文字、Reset Camera、场景对象操作、撤销/取消和 DPI 输入路由由应用与 UI 的共同验收证明。

## S1 诊断计数增量

从 `fcb9ddf` 增加 `FrameResult::stats` 后，dev/release 分别再次运行现有 `renderer_acceptance`，均 PASS。只扩展同一真实窗口消费者的检查，没有另建测试或 benchmark 工程，没有重跑无改动的完整 RHI/Runtime/JIT 矩阵。

- 首帧 3 次 UI draw、1 次包含两个方块的 scene draw；一次覆盖图上传为 6 个紧密 R8 字节，缓存为 1 项/6 个 texel 字节/10 个含行 padding 的 CPU 字节。
- 仅改变选中状态时，绘制数保持，上传量为 0；同 identity 的新 revision 只在首帧上传，旧版本退出缓存后 CPU 字节由 10 变为 6。连续 12 帧计数未累计漂移。
- 非空输入数组中额外加入空 clip 图元，实际 UI draw 与上传数不增加，未使用的覆盖图不进入 GPU 缓存。
- extent 不匹配的 skipped 帧活动计数全为 0，当前缓存量保留；删除最后一个可见覆盖图并清空场景后，缓存三项归零、scene draw 为 0。
- 原有颜色/深度、矩阵、UI 混合、resize、最小化恢复和关停检查仍通过。原生遮挡分类未另行强制制造，旧的驱动故障与硬件证据边界保持不变。

该结果证明计数对应已编码操作和现有缓存持有量，不是 256 对象的性能结论；真实交互 prepare/render 耗时与上传分布由应用侧在同一正式入口采样。日志为本机 `out/<preset>/renderer-stats-run.log` 和正式 runner 生成的 `renderer-acceptance.log`。

## 已有 RHI 回归

同一 gfx 补丁在 dev/release 分别运行 `rhi_acceptance`，两种配置均通过：Compute/Mesh/Vertex 场景颜色 24,576 像素、深度 12,288 像素、90 个参数字匹配；管线准备失败、资源使用期、owner/关停、null-list、mesh-pixel-state 和真实 timeout 矩阵通过。timeout 检查继续报告 `poisoned-admission=PASS drain-only=PASS increments=1`。

对应日志为 `out/<preset>/rhi-acceptance.log`、`rhi-regression-*.log`。没有重复运行无改动的 Runtime/JIT 矩阵。

## 未覆盖

没有动态注入 Present 后的 ResizeBuffers、Signal、Reset、device removal 或 native OOM 故障；该处失败继续沿原 RHI 终态返回，代码仅在整个 frame 成功后接受。原 timeout 证明不能替代这些驱动故障的证明。未强制制造 `DXGI_STATUS_OCCLUDED`；其分类有实现，隐藏/最小化是编码前可见性检查，不能记作 native occlusion 的验收。

没有扩展到其他显卡/驱动、不支持 Mesh 的硬件、HDR、多个窗口、并行录制或设备恢复。真实产品交互的完成以应用侧共同验收为准。
