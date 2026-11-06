# 可编程材质

本页描述当前 MS/MI 实现。CPU、GPU 与编辑闭环的实际验收结果以 `out/acceptance-gate/` 的逐目标日志为准，不以历史无纹理批次代替当前链路验证。未覆盖的 GPU 分支见 [Renderer 验收](acceptance.md#--material-programs)。语言合同见[材质纯值编译](../../../LANGUAGE.md#材质纯值编译与具名纹理输入)，编辑和文件操作见[工程工作流](../editor/projects.md)。

## 数据与入口

- **MS（Material Source）** 定义源码、参数默认值、具名纹理输入及静态策略。`source::MaterialProgram` 从同一类型化前端生成 CPU 代码、point HLSL、coverage-only HLSL 和 footprint HLSL。
- **MI（Material Instance）** 引用一个 MS 或 MI；父链无环。作者项为稳定声明路径、类型化本地值和 `enabled`，支持 float、vector2/3/4、纹理键和寻址方式。没有固定参数数量或一层继承限制。
- Runtime 中的 `material.Source` / `material.Instance` 共用 `material.Resource` 角色；Texture 也是正式 Runtime 对象。`TextureInput` 只是程序局部计算值，不是业务资源或 GPU 描述符。
- `SourceDefinition`、`ProgramSurface`、`Image` 是不可变内容。`ProgramSurface` 自持代码、参数和精确图片，不持有 Runtime 根；局部槽属于该编译版本，不持久化。声明重排按路径重绑，不能以输入数组下标代替 descriptor 的真实 slot。

网格只保存几何、命名槽及默认材质关联。每槽依次使用组件显式绑定、网格默认材质、内置 **Default World Grid**。损坏材质使用独立的品红错误材质，不把正常未绑定网格当错误。组件没有内联固定 StandardPBR 参数，也不再将组件颜色乘入网格表面。

`resolve_source` / `resolve_instance` 只消费明确的定义、父快照、作者项和图片；`capture_program` 从活 Runtime 捕获并复用未变内容。两条入口都返回 `ProgramSurface`，不会回读磁盘或回写作者值。

`capture_program_with_dependencies` 在同一次 live 捕获中返回 `ProgramCapture { surface, dependencies }`。依赖是实际验证/读取的 live 材质与纹理 ObjectId 去重集合，包含顶层、父链、空图像和 disabled/dormant 纹理，不等于最终采样输入。它不保活对象，也不改变 Surface 的代码/图片租约；已死祖先或纹理成功回退后不进入集合，捕获失败不交付部分集合。原 `capture_program` 复用同一解析逻辑，但不分配依赖集合。Scene 消费它建立自己的[变化索引](../presentation/README.md#提取缓存)，不引入全局 dirty 消费或销毁通知。

## 作者格式、编辑与历史

唯一材质 codec 为 v2（[`material_format.hpp`](../../../modules/engine/assets/include/assets/material_format.hpp)）：

| 角色 | 文件 | 作者字段 |
|---|---|---|
| MS | `.ms.material` | `source_path`、`policy`、`inputs` |
| MI | `.material` | `parent_key`、`entries` |

MS 输入包含 `path`、`binding`、`color`、线性 float4 `empty_value`。MI 项包含 `path`、`type`、`enabled` 和 `value`；纹理 value 为 `asset_key` 与 `address`。Scene v10 保存场景 MI、共享关系与槽绑定。旧格式明确拒绝，不保留兼容 loader。

Parameters 按实际声明生成数值、向量、颜色和纹理控件。`Create Scene Material Instance` 从槽当前材质建立子实例；无绑定槽继承 Grid。内置作者不可修改，可建立 MI。MS 文本通过源码编辑入口处理，不能当 MI 参数编辑。

同值覆盖仍是显式作者意图；关闭覆盖保留本地值，丢弃本地项才恢复 absence。首次预览取消恢复原先的缺项状态。删除声明、类型失配和休眠项保留并诊断，不自动删值；休眠或失配纹理不作为执行依赖读取。

Scene 的 strict/tolerant 加载都保留这些格式合法的稀疏项，并使用当前继承值；声明失配是信息诊断，不是文件损坏。未知格式、非法类型值、必要资源或 Source 定义失败仍按严格加载规则拒绝。

参数 Undo 只保存所触及路径的 before/after 作者项及编辑对象根，不保存父材质、完整表面或图片版本。共享资产与场景实例分别使用各自历史；Save Scene 不隐式保存材质资产草稿。复制场景对象保留复制范围内的共享关系，并继续共享命名祖先资产。
Undo/Redo 重新启用当前声明匹配的纹理项时，宿主传入当前 Project，由 Session 按需装配此前未加载的图片；已有 Texture 根继续使用当前内容，包括后续 Reimport。休眠、失配和空键不触发读取，文件不可用时保留作者项并诊断，不把历史图片或 I/O 加入纯捕获路径。

## 资源发布与默认替代

`Session::publish_material` 对同键同角色原位发布，传入的精确 Source 文本优先于磁盘。`publish_texture` / 手动 Texture Reimport 更新同一 Texture 身份；新捕获读新像素，旧快照仍持有旧像素直到消费者退休。模型 Reimport 按真实槽名保留既有 MI，不覆盖用户作者文件；文件失败沿事务回滚。导入不进入参数 Undo，不自动监控文件系统。

MS 成功发布时，Session 按新声明补齐其已加载命名子链和场景 MI 中已启用、类型匹配且尚未装配的纹理引用。冷加载时 Source 编译失败或声明缺失不会使这些作者键永久停留在空绑定；修复只更新消费资源，不重载子 MI 作者文件、不清除未保存编辑或参数历史，休眠与失配项仍不读取图片。

单项不可用时继承当前父内容或输入默认；显式空纹理键读取 MS 的线性 float4 空值，不回退父图。定义或父链无法消费时使用真实 Source 编译的 Opaque 品红程序，且不把子 MI 的布局套入错误程序。Grid 与错误材质均沿普通材质路径执行，Renderer 没有 StandardPBR 或错误标志特判。

默认替代不改写源码、资源键或覆盖值，不创建历史。`ProgramSurface::diagnostics()` 属于该精确结果；Session 另保留原始加载/编译诊断。strict 加载、Repro 完整性与身份/关系校验仍可失败，不能因为存在显示替代而当作输入合法。

材质资源验收检查消费者经产品 `capture_program` 取得的当前内容带诊断；同次捕获的 Surface 同时用于值与诊断检查，需要检查父实例时显式捕获父实例。捕获可能解析依赖并刷新派生缓存，检查的不是此前发布缓存中的诊断；稳态捕获不改写 Runtime 的守卫仍独立保留。

Live 将参数命令与精确资源内容分开传输；纹理和网格 spill 文件按在途预算与 ACK/close 寿命管理。Repro 冻结实际可达 MS/MI 父链、Source 文本和活动纹理像素，休眠项仍保留作者数据但不引入执行依赖。它不是通用资源服务或可回退的资产历史。

## GPU 批次与实际显示

`Renderer::prepare_materials(span<ProgramSurfaceRef>)` 返回 move-only、owner-thread 的 `MaterialBatch`。必须调用 `preparation(batch)`；即使复用管线，也要完成该批次的数据表和图片准备。ready 仅表示同 Context 有序可用，不代表 GPU 完成或已呈现。

有网格的 `Frame` 必须提供同 Renderer 的 ready `materials`，每个 `MeshInstance::material_slots` 完整映射几何槽到该批次表。UI/空场景可以没有批次。批次须在 Renderer 关闭前释放。

17 个材质管线按精确 MaterialProgram 对象集合及**每个程序是否被任一 Mask 表面使用**的分类共享。分类遍历整个 Surface 表，同一程序经不同 SourceDefinition 使用不同 Policy 时取所有实例的并集，不取去重后的第一个定义。分类变化必须选择匹配的 program set/PSO，不能只按程序指针复用；仍存活的同分类管线可复用。MI 参数、图片、表顺序和不改变上述分类的策略修改不重新编译相同集合。

全 Opaque 集合在 RayQuery 模板标志中设置 `RAY_FLAG_FORCE_OPAQUE`，并在编译期移除所有 candidate 检查；含 Mask 的集合只为实际需要 Mask 的程序生成 coverage-only 分派。`MATERIAL_HAS_MASKED` 始终表达整个材质集合的真实分类，不因某个管线采用近似而改写。

**只有 GI 内部管线把 Mask 当作不透明几何**：`MaterialProgramSet::submit` 的 compute 拼接在公共材质 prefix 与 shader source 之间，为 `probe_trace` / `probe_update` / `inspect` / `inspect_update`（入口 `DDGITrace` / `DDGIUpdate` / `DDGIInspect` / `DDGIUpdateInspect`）加入 `#define MATERIAL_DDGI_INTERNAL_FORCE_OPAQUE 1`。其余管线默认该宏为 0，包括相机直接光 `sun_trace` / `compose`、接收面 `probe_resolve` / `probe_upsample` / `probe_exact` 和接收面覆盖显示；shadow atlas 也不采用此宏。

`MATERIAL_TEST_RAY_CANDIDATES` 由 `MATERIAL_HAS_MASKED && !MATERIAL_DDGI_INTERNAL_FORCE_OPAQUE` 决定，同时控制 `lit_accept_candidate` 和四个 RayQuery 的候选代码；为 0 时模板标志 `MATERIAL_COVERAGE_RAY_FLAGS` 为 `RAY_FLAG_FORCE_OPAQUE`。因此 GI 主射线、命中点光源可见性和反馈 LOS 都裁掉候选，而相机直接阴影、shadow atlas 和接收面 LOS 仍正确评估 Mask。没有运行时近似开关，也不更改材质表、BLAS 策略、闭合或双面合同。

Renderer 保留最后实际使用的代码 owner，但不因此保留旧参数/图片表。图片按不可变 owner 和 linear/sRGB 使用方式共享上传及 mip；准备中但未采用的批次也持有图片，不能被旧帧清扫淘汰。退休沿现有 RHI/gfx，不逐图等待 GPU。

宿主使用 `SceneSnapshot::refresh → prepare → draw()`。候选冻结网格、变换、槽表、光源和 CPU 拾取数据；只有仍属当前请求且 ready 的完整组才能采用。准备期间保留上一完整组，首组未 ready 不提交网格场景；不是将新参数拼到旧程序上。

`ViewportView.scene` 只在 Renderer 接受该帧后替换；`skipped/not_ready` 不改变屏幕拾取快照。点拾取使用这一帧的精确几何、变换、身份和 ProgramSurface，CPU 执行完整 Context 与 LOD0 双线性覆盖；框选保持几何范围语义。

## 采样与各通道

- CPU 的 `MaterialTexture::sample/orientation` 以 noexcept Result 返回业务失败；`MaterialProgram::evaluate` 保留原错误，只有状态跨私有 DLL ABI。缺失绑定、非有限坐标/采样值/输出仍返回错误并保持调用方输出不变；可选 orientation 仅在执行到查询时要求存在。CPU 内存耗尽遵循统一 fatal 合同，不以采样错误恢复。
- 工作室、GBuffer、编辑深度在分派及 discard 前获取透视重心变化，构造完整 Context 的两方向变化，再经程序计算得到最终采样坐标足迹。
- 相机直接阴影、shadow atlas 与接收面 inline RT 候选使用完整 Context、LOD0 覆盖。`MaterialProgram::hlsl_coverage(namespace)` 提供 `ms_Context`、`ms_Parameters`、`ms_load_parameters` 和返回 float 的 `ms_coverage(environment, context, parameters)`；Renderer 以 `MaterialCoverageN` 及 `material_coverage(surface, context)` 分派，不为候选调用完整 `material_shade`。其具名纹理仍经 `atelier_material_sample` 读取同一输入表，沿真实 linear/sRGB、寻址和空值合同在 LOD0 采样；没有候选 footprint 或独立 coverage 参数表。
- DDGI 内部遍历按上述不透明近似提交命中，之后仍按 `.01t` 与命中平面两轴构造足迹执行完整材质着色，不用过滤后的 coverage 再否定已提交命中。主表面的 footprint 覆盖与保留候选路径的 LOD0 覆盖是不同采样合同，不能互相替换。
- 非有限最终梯度或纹素尺度溢出读取真实最粗 mip，不把坏梯度交给硬件。
- `sample_orientation` 查询实际输入的 address：Repeat/Clamp 为正方向；Mirror 按折叠区间返回方向，精确折点取正。footprint 中它是中心值上的分段常量，两方向变化为零。
- 除 GI 内部近似外，Mask 保留 `coverage >= 0`，Opaque 不用 coverage 裁剪。MatCap 保留自身配色和几何法线预览，但遵循材质 Mask。

GI 近似会把叶片、栅栏等镂空处视为实体遮挡；被相机 Mask 丢弃的区域也可能以其完整材质参与间接反射或发光。这是为限制 GI 遍历候选状态而接受的质量取舍，不是精确 alpha 传输；直接光阴影、主视图镂空、CPU 拾取和接收面 LOS 不采用该近似。

DDGI 的世界尺度为 \(\rho=0.01t\)。两轴沿射线在命中平面的投影及其正交方向，长度分别为 \(\rho/|N\cdot D|\) 与 \(\rho\)；正入射用最长世界边定轴，不封顶有限掠射足迹。有效世界三角形上零足迹给零变化，退化世界几何或溢出给不可用变化；退化 UV chart 不废弃其他有效属性。法线变化对应同次插值归一化，切线 XYZ 不额外归一化，几何法线和手性变化为零。这是局部切平面近似，不是精确射线微分；其他消费者须提供自己的足迹。编译器的局部分支、奇点和非有限传播约定见[语言规范](../../../LANGUAGE.md#材质局部足迹的边界约定)。

### Coverage 与策略切换验收

既有 `renderer_acceptance_material_programs`（可执行文件 `--material-programs`）包含：

- 一个 ProgramB 采样记录：真实 Source → HLSL → GPU，完整 point 和 coverage-only 各对作者 repeat UV、纹素权重、颜色空间及字面参数推导的同一真值，不互相比，也不以 CPU evaluator 输出作期望。
- 一个 authored math 输入元组，检查 clamp/lerp/fract 的字面结果，不展开参数矩阵。
- 四张原生批次图：`native-smooth` 的作者红/绿 Mask 和 .8/.4 深度，`native-default-smooth` 的公开不透明品红，`native-final-uv-mips` 的等面积红蓝棋盘均值和 .8 深度，以及 `native-ddgi-masked` 的无灯/环境、视锥外负 coverage 发光面照亮接收面。每个特性一图，沿 [Renderer 验收](acceptance.md#--material-programs) 的唯一代表，不以另一帧作真值。
- 同一批次序列覆盖 live batch 阻止 close、已准备但未采用图片保活、采用、最后一次使用不 capture/finish、释放后继续提交及正常关闭。

这些入口不新增生产导出 API。性能、驱动 scratch 和长期真实场景稳定性需单独测量；验收源码的存在不代表本次运行已通过。

## StandardPBR 与边界

StandardPBR 是安装的普通 MS，而非全部材质的固定 schema。它提供基础色、粗糙度、金属度、切线空间法线、自发光和透明度六路输入及独立 UV 变换。颜色图仅 RGB 作 sRGB 解码，alpha 保持线性；标量图可选通道。Repeat/Clamp/Mirror 来自输入绑定；未绑定法线的类型化空值为精确中性值，不制造量化默认图片。

图片是左上原点的不可变 RGBA8，单边最多 8192，经 WIC 解码。HDR 图片仍仅用于天空。StandardPBR 法线方差粗糙度修正属于过滤近似；发光范围受 FP16 GBuffer 限制。几何法线继续用于偏移、闭合分类、支持平面和历史深度，着色法线用于 BRDF/反射；Mask 会影响闭合与 RT 不透明性。

OBJ/glTF 导入生成普通 MI 与网格默认表；内置几何和模型沿现有 MikkTSpace/作者切线规则。普通参数与图片替换不重建几何；覆盖策略变化更新 RT 不透明性。网格发光经 DDGI 影响其他表面，SSSR 边界见 [Scene Lit](scene-lit.md)。

没有节点图、透明混合/折射、虚拟纹理、压缩纹理管线、覆盖率保持 alpha mip，或任意 Shader ABI。性能必须另行测量；管线复用和缓存策略本身不等于已获得帧率提升。
