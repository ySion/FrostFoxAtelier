# MS/MI 阶段基线 · 2026-10-29

基线标签：`baseline/ms-mi-2026-10-29`。集成起点为 `7ade1ab159a9698583680a3c24af784993a8cd8e`，在同一 main 工作树完成实现、审核、定向红绿与清理；没有从已过时的负责人分支重新合并。本页是一次阶段记录，不是发行认证或性能基线。

## 已交付范围

- 自有语言 MS、可继承 MI、具名纹理、CPU/point/footprint HLSL；完整材质批次贯通光栅、阴影、DDGI 和接受帧拾取。当前合同见[材质系统](../modules/renderer/materials.md)与[语言规范](../../LANGUAGE.md#材质纯值编译与具名纹理输入)。
- 统一材质 v2、Scene v10；StandardPBR/Grid 走普通 MS，Examples、导入器和生成器同步升级，旧固定材质入口/格式不保留兼容 loader。
- 手动纹理/模型 Reimport、纯作者参数历史、具名与场景 MI 的 Undo/Redo/Save 归属；精确 Source/纹理/网格 Live、大资源 spill、类型化 Repro 闭包。ACK 仍是命令应用及 CPU 内容交接，不是 GPU/显示证明。
- 审核修复：冷失败 MS 修复后的纹理恢复及 Undo、无工程场景 MI 编辑、内联具名 MI 历史/保存、Stop/文档替换取消排队 Restart、World relocation 异常回滚、物理 before/after/contact 回调停止或关闭后的退出。
- RHI 存活检查不再清空并漏报普通 warning/error/corruption；保留预期 LIVE_* 报告、原四类计数及阳性对照。菜单和 Renderer 只保留失败时的状态快照，临时故障注入、窗口探针及运行脚本已撤下。

## 验证证据

Windows x64、RX 6800 XT，工具链版本以锁文件为准。以下是逐次实际运行，不伪称最终一次跑过所有目标；日志保存在本机 `out/acceptance-gate/<运行号>/`，不提交生成目录。

| 范围 | 结果与证据 |
|---|---|
| 最终构建与 Project 原生窗口 | `20261029-044822-174-17612`：Dev/Release all PASS；Release `--acceptance-project` 完整 PASS（38.67s，debug），包括内联 MI、焦点切换、场景 MI 对照、菜单、保存重开；此轮没有重跑 Source 子命令 |
| 最终 RHI | `20261029-044219-650-39904`：Dev/Release 完整 rhi_acceptance PASS，包含主进程及三个后端回归；gate 为 GBV，各入口范围见[RHI 验收](../modules/rhi/acceptance.md) |
| RHI 漏检反例 | `20261029-043215-721-19504`：真实 InfoQueue warning 入队后旧检查仍 PASS；`20261029-043800-657-39200`：同一注入被修补后的检查拒绝，退出 1。去掉注入后才跑正常双配置绿轮 |
| 联合 CPU/编辑链路 | `20261028-233948-063-67820`：editor_material、procedural_graph、editor_gltf、mesh_reimport、project_integration、project_texture_reimport 六目标双配置 PASS |
| Control、Source、应用 Scene Lit | 同上：Release Control PASS（101.81s）、Scene Lit PASS（38.82s）；独立 Source CLI 退出 0，普通 Restart/显式 Stop/切文档三分支新增启动数为 1/0/0。该批 Project 子命令菜单失败，故组合目标仍记 FAIL；后由上述最终 Project 运行覆盖 |
| Renderer | `20261028-231018-738-28932`：两配置 material_programs、Scene Lit 和 Release default PASS；Dev default 当时失败，后在 `20261028-233948-063-67820` 全入口 GBV PASS（676.83s）。原失败仍保留，不把重跑通过当作原因已确认 |
| 生命周期与叶入口 | `.amp/in/artifacts/20261028-lifecycle-dev.log`、`20261028-pruning-dev-leaf.log`、`20261028-pruning-release-leaf.log`：World 22/22、Physics 27/27 双配置 PASS；相关 JIT/Source、source_material、procedural、mesh_import 均 PASS |

Scene Lit Room 的真实 MatCap、Scene Lit 和保存重开截图已检查，未见空白视口或大片错误材质。复制部署的 headless 场景运行通过，但开发树仍存在，不等于干净机器安装认证。较早阶段汇总与原始失败见本机 `.amp/in/artifacts/20261028-stage-closeout.md`、`20261028-boundary-pruning-review.md`、`20261029-review-logging.md`；这些历史记录中的交付状态以其记录时点为准。

### 窗口失败调查的结论边界

- Editor：在 F10 与 Left 之间短暂失焦再恢复，真实 WM_KILLFOCUS/capture changed 关闭菜单，后续键盘入口的焦点检查仍能通过，却出现同一导航失败。受控反例为 `20261029-044541-153-32708`；证明这种机制足以致错，**不证明历史失败确实由它引起**。
- Renderer：同一已准备 Lit 输入下，正常焦点、移交焦点、完全覆盖在本机都 accepted；最小化得到 skipped/draws=0，恢复后 accepted。见 `20261029-043215-721-19504` 的 Dev renderer 日志。诊断子集省略完整 PASS 标记，运行器记 FAIL 是预期，不能算完整 Renderer 通过。
- 未增加 sleep/盲重试、放宽断言或改变产品失焦语义；旧 Renderer 失败缺少当帧 status/capture/draws/HRESULT，仍未定因。

## 剪枝与文档清理

- 删除无调用 Reader 入口、重复 Mesh 扫描、程序化二次查找、无用计数/状态与部分内部重复准入；RHI 复用既有 admit。外部数据校验、失败隔离、取消/join、真实 owner、GPU 退休及 close 幂等均保留。16 个纯精简源文件相对剪枝前任务内快照净减 10,852 字节，不是整个材质迁移或二进制的净变化。
- 移除过时材质实施计划、空白研究问卷和旧 CPU 迭代记录；现行合同只维护一处。资产路线及独立服务提案压缩为真正未完成的范围，保留采样数值约定和仍待产品决策的边界。
- 清除 54 份本地临时 patch/diff（3,333,076 字节），以及与正式资源 SHA256 相同的 12 份历史部署字体副本（144,880,548 字节），本次合计 141.35 MiB。清单为本机 `.amp/in/artifacts/20261029-closeout-removed.csv`；必要的 `third_party/gfx/atelier.patch` 及当前构建的部署副本保留。
- 保留原始红绿日志、测量数据、截图、正式资源、当前构建及其他工作树；不重写历史，也不把清缓存当成产品体积优化。

## 未包含的能力

独立资源服务、完整 Cook/发行、干净机器安装、更多 GPU/驱动矩阵、极端足迹的专项 GPU 覆盖、自然合法 MS→DXC/PSO 失败夹具及性能收益测量均未完成。后续范围见[资产路线](../modules/resources/asset-system-plan.md)，不是本轮隐含实施授权。

[日志评估](logging-assessment.md)确认已有 World/Source 领域诊断和 Project Output，但没有统一日志服务。推荐探索自有薄语义与 spdlog 后端；本轮未引入库、未迁移调用或宣称性能收益。
