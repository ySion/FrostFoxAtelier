# 编辑器 renderer 当前契约

入口：[renderer.hpp](../../../src/renderer/renderer.hpp)，目标 `atelier_renderer`，由 [cmake/renderer.cmake](../../../cmake/renderer.cmake) 的 `atelier_add_renderer()` 创建。调用前由应用根构建创建 `atelier_rhi` 和 `atelier_ui_protocol`；可执行目标调用 `atelier_deploy_rhi_runtime(target)` 部署同一套 DXC、Agility 与 PIX DLL。shader 在配置时嵌入库，不依赖启动目录或另一 worktree。

## 一条执行链

应用持有 HWND、Runtime 对象与编辑状态 → CPU 场景数组和 UI 完整帧 → `Renderer::render` → 同一 RHI Context/gfx 录制与 Present → 应用只在 accepted 时替换命中帧。

renderer 不保留业务对象身份、GC 根或 JIT 回调。UI 布局、文本排版、输入处理和撤销归各自拥有者；renderer 消费 [公共 CPU 协议](../../../src/ui/frame.hpp)。同一 owner 线程完成打开、查询、绘制和关闭，窗口必须存活到 renderer 关闭之后。

## 场景和 UI

- `MeshInstance` 提供完整 local-to-world 矩阵、线性基础色、选中标记与共享不可变 Geometry。`geometry::Mesh` 是经构造验证的连续三角形顶点资源，`geometry::cube_mesh()` 只是一个内置资源。相邻且引用同一资源的实例合为一次 instanced draw，不同资源分批。GPU 缓存保有对应 CPU 资源，未使用项沿 gfx 机制退休；没有另一套业务对象身份。当前为不透明基础光照，不含资产导入、拾取或着色器图。
- 相机为左手系，+Y 向上，D3D 深度 `[0,1]`。矩阵为 row-major，行向量按 `p * world * view * projection` 执行，匹配 LookAtLH/PerspectiveFovLH。renderer 不维护相机控制状态。
- World/应用负责矩阵求值，renderer 只消费最终数学值；不依赖 World/Runtime，不重做 TRS 分解。World 已提供空间树，renderer 只接收其最终仿射矩阵。仿射矩阵平移位于索引 12/13/14，索引 3/7/11 为 0、15 为 1。非有限、非仿射或奇异矩阵使整帧在编码前返回 invalid_argument、terminal=false；保留旧 capture 和执行域，不用单位矩阵兜底，也不静默略过对象。
- 法线按 world 的 3×3 逆转置计算。CPU 用 double 计算余因子，整个矩阵共用一个缩放因子，保留任意法线所需的行间比例；pixel shader 先按最大分量缩放再规范化。镜像输入保留朝外法线，沿用无背面剔除。CPU 同时检查实际上传的 FP32 矩阵，极小数清零后失去秩的输入在编码前拒绝。统一极小/极大缩放继续支持；这不承诺任意病态非均匀矩阵的数值精度。World 局部 scale 仍限定正、非零、有限。
- 场景 viewport 与 UI 坐标均为窗口物理像素，左上原点；DPI 已由 UI 应用。先画场景，再按 UI 数组顺序绘制；应用/UI 应给场景 viewport 留出可见区域。
- UI 支持半开 scissor、实色矩形、R8 覆盖图与 UV。输入为线性 straight RGBA，pixel shader 乘 coverage 与 alpha 后输出预乘颜色，混合使用 ONE/INV_SRC_ALPHA。
- renderer 显式请求 SDR RGBA8 backbuffer，并校验实际 RTV 格式为 `R8G8B8A8_UNORM_SRGB`；sRGB RTV 执行写入编码与线性混合。RHI 未指定该选项的旧调用仍采用 gfx 的显示器自动选择，不能推导所有 Context 都保证 RGBA8。
- Coverage 的非零 identity 与 revision 一起标识不可变内容，renderer 缓存同时保有 `shared_ptr<const Coverage>`。CPU 行可有 padding；上传时收紧行。每个实际使用的版本只上传一次，无用缓存释放 CPU owner，GPU 资源沿 gfx 原有机制退休。当前没有字体图集、显存预算或资源管理器。

## 帧接受与失败

`Frame::ui_frame` 是帧 id、extent、DPI 和 surface revision 的唯一来源；所有输入借用到同步 `render` 返回。renderer 的 `target()` 报告实际 backbuffer 尺寸，窗口 client 尺寸可能在 resize 时领先一帧。

- 隐藏、最小化、零 client 尺寸或输入 extent 与实际 target 不符：编码前返回 skipped，capture 不变。空 scissor 不会落入 gfx 的“零尺寸代表整个目标”惯例。
- 完整 `Context::frame` 成功且 Present 未返回 `DXGI_STATUS_OCCLUDED`：accepted。`accepted_extent` 是编码前捕获的实际尺寸；gfx 在旧目标 Present 后 resize，`next_extent` 只适用于下一帧。accepted 不承诺已经完成扫描显示。
- CPU 校验和编码前 CPU 分配失败：`Error::terminal=false`，可重新准备帧。开始 GPU 创建、上传或录制后失败：terminal，后续只可关闭，不宣称回滚或继续使用上一 GPU 帧。
- Present 之后的 resize、等待、Reset 等失败沿 RHI 终态返回，不会提前发布 accepted。遮挡是 skipped，GPU 可能已经提交；它不是 GPU 回滚。
- 应用只根据 accepted 替换完整命中数据，并负责在窗口大小/DPI 与命中帧不匹配时暂停命中、取消 press/capture；renderer 不建立第二份输入状态。

`close` 退休 GPU 资源、释放 Pipeline、通过原 Context 排空和关闭。重复关闭成功，关闭失败保留清理入口；无法安全清理的最终析构 fail-fast。

## 诊断捕获

### 每帧计数

`FrameResult::stats` 是该次 `render` 返回的 `FrameStats` 值快照，七项均为 `uint64_t`。它不持有额外资源，也不增加 GPU 查询、等待或动态分配。

| 字段 | 统计边界 |
| --- | --- |
| `ui_draw_calls` | 实际成功编码的 UI draw 次数；空 clip、零尺寸和透明项不计入 |
| `scene_draw_calls` | 实际成功编码的场景 draw 次数；相邻同网格资源实例共享一次 instanced draw，空场景为 0 |
| `coverage_upload_count` | 本次新建内容版本后成功编码的 R8 上传次数；命中已有版本为 0 |
| `coverage_upload_bytes` | 上述上传的紧密 R8 有效字节数；不含 CPU 行 padding、D3D 复制对齐与 scratch，也不含网格实例上传 |
| `cached_coverage_count` | 返回时 renderer 缓存持有的 Coverage/GPU texture 项数 |
| `cached_coverage_texel_bytes` | 缓存项宽×高的逻辑 R8 字节总量，不是物理显存占用 |
| `cached_coverage_cpu_bytes` | 缓存持有的 Coverage 的 `pixels.size()` 总量，包含 CPU 行 padding；不是 vector capacity 或整个 UI 的 CPU 内存 |

编码前 skipped 的前四项为 0，后三项反映仍被持有的缓存。原生遮挡发生在编码之后，skipped 仍报告已编码活动。unexpected 失败不返回完整统计；原有接受状态与终态规则不变。缓存量不包含已交给 gfx 等待 GPU 退休的资源，也不包含几何、深度缓冲与上传暂存。每帧计数重置，缓存总量随现有插入/退休动作更新，未改变淘汰策略。

应用可在正式交互中测量 prepare/render 的 CPU 墙钟时间；render 时间包含 Present 或必要等待，不能称为 GPU 时长。使用同一 `FrameResult` 关联活动计数，显式 Capture 另行记录，避免把它的同步成本混入普通交互样本。

### 显式图像读回

`render(frame, &capture)` 显式请求 GPU 同步，返回本帧目标的紧密 sRGB RGBA8 和 float32 深度，行自上向下。仅在 accepted 时替换输出。UI 不写场景深度，背景深度为 1。普通 render 不调用 finish；诊断捕获不应放入交互帧热路径。

捕获命令经现有 gfx 录制，使用 256 字节对齐的 GPU 行距，完成后拆成紧密 CPU 行。读回源是 resize 前真正绘制的目标。没有原生队列逃生口，也不改变原有紧密 texture copy 入口。
