# gfx 实施裁决：深实现、窄宿主契约

状态：本轮研究与独立对抗后的实施裁决；GPU 结果以实际验收记录为准，不将静态分析记作 PASS。

## 采用什么

- 固定 gfx `e039b255efb327994d30b187caee400ca60110e0`，当前不纳入 OMM 专题提交。
- gfx 就是 RHI 的主体，继续独占资源、反射、绑定、descriptor、barrier、常量池、命令与退休机制。
- 自有 `atelier::rhi` 只集中硬件/调试门禁、execution owner、管线准备成功与生命周期。profile 是创建输入；prepare 成功直接给 Pipeline，不增加 Publisher、ticket、版本账或逐函数转发层。
- 三类管线复用现有 Program/Kernel。新候选完整成功后，由 owner 在线程内移动替换旧 Pipeline；失败不改变旧版，新 program 不暗中继承旧绑定参数。

## 明确的执行范围

一个 owner 线程串行使用一个 owned gfx context；这是对固定 gfx 的实际执行契约选择，不继承旧占位设计的权威。GPU 仍有帧槽与异步在飞工作。不宣称并行录制、多队列、透明设备恢复或外部原生资源导入已实现。

消费者借用 gfx context，可创建/销毁其自有普通资源、设置参数、录制 gfx 命令；不能调用 frame/finish/destroyContext/reloadAll、替换 swapchain、导入外部资源、直接原生提交或改 barrier。帧推进和关停由 execution owner 统一调用。未来 RDG 组织依赖与工作，不另建与 gfx 冲突的状态追踪；确需多队列/原生互操作时重新验证执行契约。

Context 与 Pipeline 使用真实所有权保持执行域有效。显式 close 先要求 Pipeline 所有者释放，再排空与销毁；普通资源退出仍沿 gfx 的延迟退休。不会在每次资源销毁时等待 GPU。

## 必须落地的局部修正

1. Mesh+PS 的 shader-visible resource state 包含 pixel 读取。
2. 任一存在/明确提供的 shader 阶段编译失败必须阻止该候选成为 Ready，不能仅看 PSO 指针；保留可选阶段不存在的语义。
3. terminate 的 ClearState 检查 command list，合法无 list 的 interop CPU 管线创建/销毁不可崩溃。
4. gfx 内部保留 Recording/Closed/Submitted 事实与独立失败原因。Execute 已发生但 Signal 失败也必须记为 Submitted；错误不能抹掉这一事实。
5. 等待有界并检查原生 Close/Signal/事件/Reset 返回值；失败后不再普通录制/提交、Reset、复用或推进 GC。destroy 只对原 queue 追加完成标记并 drain，不重 Execute；确认完成或真实 device removal 后才释放。失败时仍保留 context，不假装成功。
6. resize/swapchain 等内部调用贯通 finish/sync 失败，不能绕过安全门禁。

无法排空的最终析构不能继续释放未知在飞内存；显式 close 返回错误，放弃处理而进入析构则 fail-fast。该策略是错误终态，不是自动设备恢复。

## 对抗评审采纳记录

- 深度评审：拒绝独立 profile 对象、双阶段 Pipeline 对象、Publisher 与强制资源镜像；补足所请求 shader 阶段的成功定义与 raw 借用权限。
- 复用评审：拒绝仅替换 INFINITE 的表面修补；要求保留提交事实、贯通间接调用链、修复 device-loss/null-list 清理及 import 恢复时机。
- import 不是普通 profile 能力。专用回归可通过合法 interop 创建 CPU pipeline 而不设置 list 来复现空指针，避免真实设备故障实验。

## 验收合同

- dev/release 使用固定工具链、源码和运行库，通过同一正式 Interface 执行。
- Compute 实际生成间接参数与数据，Mesh 的 PS 先消费，再走 Vertex；核对颜色、深度和 GPU 参数。
- 跨帧与同帧参数更新，包含超过 root-constant 阈值的常量；调用方提前释放已编码资源仍能得到正确结果。
- 源码/文件准备失败、可选但已提供阶段编译失败及失败替换不得丢掉旧 Pipeline。
- 无 command list 的合法 interop 清理回归。
- 专用 owned queue 的真实 CPU fence gate 制造可控等待超时。计数器必须预先初始化并完成，受测 list 只递增并回读；解除 gate 后关闭只 drain，结果严格一次。无 TDR 修改、挂死 shader、伪造 driver 或真实 device removal。

许可证、依赖 hash、上游补丁与运行证据一并保留。尚未运行的场景不记为通过。
