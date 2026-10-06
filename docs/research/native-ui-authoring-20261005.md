# 原生 UI 第二阶段：组件与保存更新

日期：2026-10-05。验收基线 `e42c383`，本阶段随 `codex/world-editor-integration` 分支交付，主线合并单独进行。当前语义的权威说明见[声明式 UI](../modules/editor/declarative-ui.md)。

本记录保留第二阶段首次落地的验收和性能样本。随后发现的生命周期问题、修复、增量代码复用及最新测量见[热更审计](ui-hot-reload-audit-20261005.md)；以下整模块重编数字是优化前的证据。

## 实际改动

- 单文件多个组件类型、显式有类型输入和实例组合；Repeat 按宿主项 key 保留实例；mounted 与 collapsed 分开。
- Text、TextEditBinding、Key 和 Items 输入；文本、树与共享单行布局接入原 Session。组件没有业务对象包装层。
- Parameters 查询、组件列表和 Scene Tree 的 C++ 控件构造迁入 `editor.ui.source`；查询栏由两处共享。旧 `position.ui.source` 入口删除。
- 文件监测、去抖、整模块候选编译、兼容状态/key 迁移；Session 接受描述并消费旧取消 effects 后才提交候选。
- 编译、属性或批次结构错误保留旧实例与可用输入；下一次有效保存可以恢复。旧 UI 帧独立拥有绘制数据。

运行时负责人确认当前 Program 不能在工作线程安全 prepare。实现遵守同 Runtime 线程合同；没有用后台 JIT 绕过准入。布局负责人确认兼容 key/kind/父链可保留编辑状态，因此没有每次保存全局 cancel_interaction。

## 后台验收

Dev / Release 均编译正式编辑器，并通过 `editor_document_acceptance` 与 `ui_acceptance`（20/20）。实际覆盖：

- 原工程读写、目录浏览、三种资源视图、1/1.5/2 DPI 与真实字段撤销/重做。
- 组合状态隔离、列表重排/移除/恢复、条件卸载清理、文字输入和字段身份。
- 实际文件保存，语法失败、候选属性失败、Session 拓扑拒绝及恢复；文件消失后恢复相同内容；活动文字草稿/token 与折叠状态保留。
- 重复项 key、递归组合和 Items 元素类型不匹配被拒绝。

两类型组合验收中的最后一次编译约为 Dev 89 ms、Release 33 ms。这是当前机器的单次实际样本，不代表完整编辑器或复杂模块的性能上限。

## 普通窗口

入口为 `atelier_editor --acceptance-project out/ui-authoring-20261005/<dev|release> E:/ffa_testproj`。工程的 Config/Content/Source 复制到独立 run 目录；作者文件也使用该目录下的副本。

首次 Dev 运行停在既有 About 验收驱动：已取得正确标题/内容，但一次性 posted WM_COMMAND 未关闭原生对话框。驱动改为通过原生 WM_CLOSE 请求关闭，并确认窗口已销毁；未修改产品 About 行为或放宽前台检查。

Dev 和 Release 最终均通过完整工程窗口链及新增保存更新验收：

- Dev：`out/ui-authoring-20261005/dev/run-88024-389613171/acceptance.txt`
- Release：`out/ui-authoring-20261005/release/run-86488-389689921/acceptance.txt`
- 两处目录均包含 `ui-live-updated.png`、`ui-live-error.png`、`ui-live-recovered.png` 和 `ui-authoring.csv`。
- 窗口中真实编译六个编辑器组件，改变折叠栏文案；语法错误保留旧画面及草稿，修复后恢复；随后仍可编辑真实 World 位置字段。

| 配置 | 成功保存 | 模块编译 | 保存至接受帧 |
| --- | --- | --- | --- |
| Dev | 有效修改 | 390.7 ms | 641 ms |
| Dev | 错误后恢复 | 385.7 ms | 625 ms |
| Release | 有效修改 | 141.5 ms | 438 ms |
| Release | 错误后恢复 | 141.5 ms | 375 ms |

耗时包含当前配置/机器的实际执行；保存至接受帧还包含文件轮询、去抖、实例建立及原布局/绘制。以上为单次流程样本，未宣称统计性能上限。

原 `E:/ffa_testproj` 的 Config、Content、Source 文件集合和 SHA-256 保持不变；前后清单位于 `out/ui-authoring-20261005/project-before.json` 与 `project-after.json`。

窗口通过后补充了 IO 恢复边界：文件消失、超限或无法读取后清除旧的尝试内容缓存；共享占用导致读失败时再次去抖重试。恢复相同内容也能清除旧诊断。该增量使用 Dev/Release 的真实文件/JIT/Session 端到端验收。

## 当前边界

本阶段没有迁移完整编辑器外壳，没有增加 slots/imports、UI 实例虚拟化或 HUD，也未加入 SS/C++ 热更新。首次落地采用整模块同步编译，后续审计已增加组件代码复用；普通帧仍由 Workspace 全量采样和描述。描述提交后的布局/渲染恢复遵循原 Session 合同，不承诺回滚已取消的输入事务。
