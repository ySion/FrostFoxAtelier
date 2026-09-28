# 并行模块候选：实际依赖图与组合建议

> 历史快照：本文依据当轮 baseline.json；旧接口、文件名、行号与未实现判断仅描述当时状态，不是现行规则或待执行任务。后续结果与现行入口见[历史索引](../README.md)。

作者：WaveModuleMap。日期：2026-09-25。状态：双向质询完成，Main已裁决入选并完成两条本机路径验证。**路线已证实，两个生产Module尚未实现。** 本席位只读生产代码与Main生成的结果，未自行构建、运行或测试。

## 1. 意图与已确认结论

在 Wave 1 主线继续加固时，选出不等待其未决语义、能从真实入口独立验收的1–2个模块，而非同时建立六套基础设施。

**Main已裁决入选：UI布局核心 + 单后端窄离屏RHI。** UI采用Yoga 3.2.1作为窄Interface的内部求解候选；本机路径已通过真实DirectWrite测量。RHI采用基础D3D12单DIRECT queue资源提交切片；真实AMD Radeon RX 6800 XT硬件与强制debug layer下，两次精确读回通过。两者先不互相依赖，不创建第二套业务对象世界。

初稿曾将RHI列为条件推荐；Main的真实资源提交结果已满足本轮路线准入条件，因此不再处于“等GPU路径是否可行”的阶段。但平台探针仍不证明尚不存在的布局Module身份/失败快照或RHI录制取消/丢ticket等Interface合同已经实现。组合取舍不是性能排名，完整证据与限制见7.1。

## 2. 当前实际依赖图

| 当前模块/target | 实际依赖、所有权和限制 |
| --- | --- |
| atelier_runtime | 静态库编译runtime.cpp、execution.cpp，公开src，不链接LLVM。ExecutionFrame依赖Runtime；没有引擎任务调度。 |
| NativeRegistration<T> | native.hpp生产TypeDefinition，进入同一Runtime注册；不拥有原生专属业务类型表。 |
| compile_source | source.hpp的Environment→自有Program，不包含Runtime/LLVM。ValueType为编译分类，不是另一种业务身份。 |
| atelier_jit_probe | 隔离DLL编译平台/业务backend、source.cpp、windows_unwind.cpp；私有LLVM/mimalloc及内存管理回补。zlib/zstd是LLVM构建依赖。 |
| atelier_jit_acceptance | EXE链接Runtime、jit_probe、jit_native_client。Binding/Invocation/BusinessProgram仍在acceptance/jit/business.cpp，不能当通用宿主层。 |
| atelier_runtime_acceptance | EXE链接Runtime与allocator_client，是现有公开接口消费入口，不是图形/UI验收。 |
| atelier_target | 可执行文件直接编译全局new/delete覆盖并链接共享mimalloc；JIT DLL另有ABI/分配处理。GPU/系统专用资源不因此归GC管理。 |

当前src仅有runtime、language、jit、memory；正式target未包含RHI、UI、window/input、asset、scheduler、math。依赖锁只有mimalloc、zlib、zstd，不能将LLVM压缩依赖当现成资产管线。

当前方向：样例业务宿主→Runtime+JIT隔离层；执行根→Runtime；原生注册→Runtime；源码前端→标准库；展开机制→Windows/LLVM。候选模块不能倒逼Runtime链接RHI/UI。

**源码隔离不等于构建配置隔离。** 根CMake无条件调用atelier_add_jit_acceptance；前端虽无LLVM语义依赖，目前也没有独立target。不能承诺候选在原顶层配置里无需LLVM。根CMake、依赖锁和工具链统一由Main串行接入。

Main追加的精简链接实验证实了更强但有限的结论：UI探针只需共同分配器/系统库+Yoga/DirectWrite，RHI探针只需共同分配器/系统库+D3D12/DXGI，在显式移除Runtime/JIT库后仍能运行通过（§7.2）。因此业务语义和链接层面的独立路线已经证实；正式target、干净配置及共享构建接入尚未实现，不能把这三层混为一谈。

## 3. 六类候选对照

所有评价为定性，不伪造人日、吞吐或预算。下表保留独立初稿时的相对取舍（包括当时的“条件推荐”）；最终已入选UI布局核心与单后端窄离屏RHI，以第1节和7.1节Main裁决/实测为准。外部机制与两条真实路径已核实，但尚未形成正式生产Module，不能把探针复用当作生产集成。

| 候选 | 独立验收 | 等待Runtime/语言 | 真实复用方向 | 共享文件冲突 | 集成成本与收益 | 决定 |
| --- | --- | --- | --- | --- | --- | --- |
| UI布局 | 高：约束/树→几何，不需窗口 | 低：纯值/私有索引，业务绑定后置 | 成熟布局求解器，不复制其逻辑 | 低；单位/Rect/Size/节点所有者先定 | 封闭布局失效语义有益；扩成控件/文本/渲染系统则失独立性 | 推荐纯布局 |
| 离屏RHI | 中：真实提交→完成→读回 | 低：GPU资源不必是业务对象 | 单后端/成熟图形能力；不是复刻驱动 | 中：CMake、锁、错误、句柄语义 | 提前关闭GPU寿命风险；成本在状态/同步/诊断，不是画三角形 | 条件推荐 |
| 平台窗口/输入 | 高：消息泵/关闭/DPI/输入序列 | 低；事件线程不得任意重入Runtime | 成熟平台库或窄Win32路径 | 中高：surface/焦点/坐标/主循环相交 | 交互最终必需；现在第三路会争抢主循环与surface | 暂缓第三模块 |
| 资源/资产管线 | 中低：具体格式可，泛资产系统难 | 中高：持久ID/schema/加载卸载/业务绑定 | 实际格式的解码/压缩器；现有zlib/zstd不是资产接口 | 高：身份/文件版本/GPU上传/调度 | 长期收益高；早泛化易把ObjectId当存档ID或再建业务类型表 | 拒绝泛模块，未来格式竖切 |
| 任务调度 | 低至中：纯值队列通过不证明引擎调度 | 高：Runtime单线程；Frame同步稳定借用；GC安全点≠发布点 | 未来按真实任务采用成熟实现，不转移Runtime线程责任 | 高：执行根/GC/发布/宿主回调同时牵动 | 真实并行负载可获益；现在重新绑紧五条主线 | 暂缓 |
| 数学/几何 | 高：纯值输入输出 | 极低：无需反射/GC/JIT | 按需求选成熟实现；简单Size/Rect不值得框架 | 低，但单位/矩阵/颜色/坐标易全仓发散 | 泛复用是潜力，当前无正式几何消费者 | 不独立开工，先最少局部值 |

反证条件：真实近期目标若是可操作工具窗口，窗口/输入可能比RHI优先；若存在具体批量导入负载，格式资产切片可能胜过布局。当前仓库没有这些需求的实证，不能用潜在需求开工。

### 3.1 真实复用清单：不以库名替代接入证据

| 候选 | 已核实的一手能力 | 适用限制与本轮选择 |
| --- | --- | --- |
| UI布局 | Yoga **v3.2.1** 的YGNodeCalculateLayout、树所有者和dirty传播；YGNodeMarkDirty仅允许有measure的叶。版本源码[Y1]，官方增量说明[Y2] | 采用窄规则子集作为候选；不外翻全部style setter，不复刻第二份缓存。DirectWrite真实测量只属于叶Adapter/探针[D1] |
| RHI | Windows D3D12的command list、queue fence、readback；Map不等待GPU，command list不持有资源引用[G1–G3]。NVRHI固定提交的资源追踪/屏障可复用[N1] | 首选窄D3D12资源提交切片，非完整renderer；NVRHI确有深度，不能因“自写更轻”忽略它，但新增依赖与覆盖范围由RHI报告比较后再批准 |
| 窗口/输入 | Win32现有GetMessage/TranslateMessage/DispatchMessage消息泵与window procedure[W1] | 复用平台实际机制，不先造通用事件总线。系统消息分发与Runtime执行/发布点必须由宿主协调；“有窗口”不等于已有完整输入/IME/焦点策略 |
| 资产 | WIC IWICImagingFactory::CreateDecoderFromFilename是真实按文件创建decoder的SDK入口，明确HRESULT及Windowscodecs.lib[A1] | 以后可做指定图像格式→拥有的像素缓冲区竖切；不自动包含资产依赖图、持久ID、打包、热载或RHI上传。当前未接入 |
| 调度 | enkiTS **v1.11** 的AddTaskSetToPipe/WaitforTask、pinned task和依赖表达是可复用实现[J1] | 当前未接入。WaitforTask可能同时执行任务，不能把它当无重入含义的纯等待；仍须定义哪些任务能触达Runtime。仅库成熟不解除当前单线程约束 |
| 数学 | DirectXMath提供SIMD友好的C++线性代数/图形数学能力[M1] | 机制支持，不是本机clang-cl兼容/性能证明；本轮不增加依赖。常规Rect/Size先按值定义，实际矩阵消费者出现后才冻结矩阵约定和依赖版本 |

Microsoft文档为持续更新入口，适用项目锁定Windows SDK **10.0.26100.0**所能提供的基础接口；只读锁文件不证明本机全部组件/可选功能可用。外部源码pin是本轮比较基准，不宣称“最新”。访问日期均2026-09-25。[Y1–M1]

### 3.2 组合比较与不选理由

- **布局 + RHI资源提交：推荐。** 输入/所有者/验收设备不同，除最终构建集成外几乎没有必须共同修改的生产文件；RHI不消费布局树，布局不等GPU完成。两条路径以后由composition连接，不预建万能数据总线。
- **布局 + 窗口/输入：保留替代。** 更适合近期目标为可交互工具而非GPU资源正确性；但焦点、DPI、hit-test和主循环会成为共享决策，当前没有工具窗口需求足以替换RHI研究。
- **RHI + 窗口：暂不选。** surface/swapchain创建、resize、关闭与在飞资源退休属于同一紧耦合生命周期，容易名义并行而实际串行；离屏首里程碑有意切断此硬依赖。
- **布局 + math：不选独立双模块。** 当前共享数学需求只有小量几何值，第二session大概率制造包装层，不能凭潜在消费者证明独立深度。
- **资产 + scheduler：不选。** 会同时争夺异步完成、取消、持久身份、Runtime线程和GPU上传契约；没有实际格式/负载时收益与边界都未定。

## 4. Spec与Standards

### Spec：候选准入条件，不是既有缺陷

- 业务定义只有一套权威身份。未来脚本/原生共享的Widget或RenderComponent若为业务对象，进入现有TypeDefinition/Runtime。布局NodeId、GPU槽、OS句柄可为私有资源标识，不构成另一套业务世界。
- 值不是对象：Rect、Size、约束、顶点数组、命令描述、布局结果可以内联/批量存储，不能因可编辑或可反射就逐个创建Runtime对象与根。
- GPU完成、窗口关闭、节点销毁有各自所有者和时序，GC可达性不能替代外部完成条件。包装对象析构若投递延后释放，设备/退休队列必须活到完成。
- Runtime当前单线程。后台工作仅接收明确拥有的快照/纯值，不能因有ObjectRoot就跨线程或任意挂起；ExecutionFrame仍是同步稳定借用。
- ObjectId/TypeId限于Runtime，不是永久资产ID；私有布局/GPU索引也不替代持久身份。

本切片未报告已证实生产缺陷。基线现有PASS不证明新候选已实现。

### Standards：设计取舍/可证伪风险

小Interface应隐藏求解与资源时序，不做大面积改名转发；没有实际第二后端时不预建全功能继承树。第三方版本、许可证、补丁沿单一来源链；C ABI不自动保证CRT/分配所有权兼容。不先开math/job/asset三框架当硬前置。runtime-model旧段落与后续已实现章节存在时间差，现状以README和当前入口为准。

## 5. 入选模块的硬依赖、软集成与禁止依赖

### UI布局

- **硬依赖**：固定Yoga求解实现、受支持样式/约束、树所有权、单位/有限数值、测量及失效Interface。
- **软集成**：真实文本测量、DPI/viewport输入、结果转绘制命令、宿主将业务状态转成布局输入。DirectWrite仅为本次真实叶测量Adapter/探针，不进入布局核心target。[D1]
- **不应依赖**：RHI设备、窗口句柄、Runtime根、JIT入口、一般资产库、任务调度。
- **第一里程碑**：真实库树操作→约束求解→读取/失效→销毁。Yoga树是唯一几何权威，外围仅有owner/代际解析、输入revision和发布后的几何；不复制Widget业务树、style/children表或另建通用测量缓存。失败不发布为新版本，不能用上游assert作为公共输入校验。
- **业务持有**：布局context只读内容快照；长期事件的业务引用/稳定入口由外部绑定owner拥有。默认弱绑定，事件发生时取得并校验；明确需要强持有时由绑定owner登记ObjectRoot，销毁解绑。当前BusinessProgram::Entry是私有验收适配器，不能声称通用UI事件现成。
- **像素转换**：保留未舍入逻辑几何，一份布局结果可派生多个target/DPI的不可变像素视图。UI composition/output Adapter单点拥有转换；RHI不二次round各child.width。具体Text测量叶与普通叶舍入差异必须列入验收，当前不冻结算法。固定尺寸叶可表示非文本内容，不冒充字体指标。

### RHI资源提交首里程碑（不是完整RHI能力）

- **硬依赖**：Windows x64基础D3D12、一个DIRECT queue、设备失败方式、资源/录制/提交寿命、完成证据、读回及诊断。不默认要求新Agility SDK或预览功能。
- **软集成**：window surface/swapchain、资产上传、布局结果转绘制命令、业务包装。
- **不应依赖**：业务Runtime/GC、源码编译器、UI树、一般scheduler、资产数据库。
- **完整资源闭环**：RGBA8离屏clear→barrier→copy到readback buffer→queue Signal/fence完成→按真实row pitch核对→退休；记录实际硬件/软件adapter。Map不是同步等待，device removed返回不能当完成成功。[G1–G3]
- **不在首里程碑内**：PSO/shader/draw/Present、swapchain、frame graph、多队列、一般材质与渲染器。完整是相对此切片的闭环，不宣称通用图形能力完成。
- **必须封闭的所有权状态**：Recording首次引用即持有资源；未提交取消撤销这些持有；提交把持有移交Device自有in-flight记录；ticket只是标识，丢ticket不释放GPU正在用的资源；完成后才能释放/复用。CPU owner撤销仅申请退休，GC不等待GPU、不成为fence根。未来资源槽/descriptor/allocator复用也遵守对应完成条件。[G1、N1]
- **证据限制**：直接D3D12探针可以证明平台资源路径，但不能证明尚不存在的Module已经正确处理取消、丢ticket、复用和关闭；这些是下一session真实Interface的验收，不准从probe成功外推。

现有一般原生方法仍是void返回、float/u32参数（native.hpp:22–26），且注册在首次成功创建对象后关闭（runtime.hpp:283–297）。因此“以后业务可绑定”不等于“当前语言已表达完整布局/RHI描述”。独立C++ Module先不等待该扩展；真正业务绑定必须提前注册或另经批准的定义演进，不偷偷解封Runtime注册。

## 6. 并行前必须裁决的Interface

| 决策 | 拥有者和方向 | 禁止冲突 |
| --- | --- | --- |
| 逻辑单位/物理像素、Rect/Size | 布局拥有内部几何，宿主转换层拥有DPI | RHI反向规定布局单位；两方同时扩公共math.hpp |
| 标识/代际/失效/错误 | 各资源模块私有标识；业务包装复用Runtime | 用ObjectId表示GPU/OS资源或发明万能反射Handle |
| 线程、提交/完成/取消 | RHI拥有设备时序；布局先同步单owner；宿主协调 | 工作线程/测量callback暗调Runtime |
| 分配/释放、DLL/CRT | 沿现有CMake；专用资源按提供方释放 | 把LLVM隔离DLL误当全引擎插件标准 |
| 根CMake/依赖锁/许可证 | Main单一集成者，各session提局部接入 | 多session同时修改同一共享文件 |
| 后端/求解库及支持范围 | 先一手证据和失败条件，再由Main冻结 | 上层绑定未冻结SDK对象，先追全平台最小公倍数 |

## 7. 本轮至多三项优先动作与Main最小本机路径

1. **冻结依赖方向/文件所有权，不重复整套业务验收。** 本机只读路径：CMakeLists.txt的atelier_runtime及根include→cmake/jit.cmake的jit_probe/jit_acceptance→source.hpp的compile_source→execution.hpp同步槽约定。可核对“无Runtime/LLVM源码依赖”和“顶层配置仍需LLVM”的区别。以后模块session只拥有各自局部实现与文档，Main独占根CMake、唯一依赖锁、正式验收接线；本次不修改它们。
2. **真实布局探针。** 源文件out/review/wave1-meeting/ui-layout-path-probe.cpp，入口main→row_path/dpi_path/nested_rounding_observation/text_path，实际调用YGNodeCalculateLayout和DWriteCreateFactory→CreateTextLayout→GetMetrics。由Main按ui-layout.md的固定Yoga版本和编译命令生成同名程序后运行。精确首例预期：300×100根、padding10/gap5，A=(10,10,50,80)，B=(65,10,100,80)，C=(170,10,120,80)。同约束no-op不增加measure次数；外部文本改而不MarkDirty的负对照保留旧几何；显式MarkDirty后重测并等价fresh tree；系统字体缺失必须显式失败。成功标记为UI layout path probe: PASS，字体数值与访问计数记录实际输出，不预填。nested舍入输出是机制观察，不是文字渲染通过。
3. **真实离屏资源路径。** 源文件out/review/wave1-meeting/rhi-path-probe.cpp:50的main，真实CreateDevice→DIRECT queue→RGBA8 clear→barrier/CopyTextureRegion→queue Signal→fence→Map→逐像素核对。Main按rhi.md编译后可运行同名EXE，参数--debug请求诊断，--require-debug强制诊断存在，--warp显式选择软件设备；默认硬件失败不自动回退WARP。预期两次提交各mismatches=0且completed≥ticket、device状态成功；最终输出scope=SDK-offscreen-clear-copy-readback-fence-retirement、submitted=2、hwnd=0、swapchain=0。若debug可用，debug_errors须为0；不可用则记录debug_validation=not_performed，不当作诊断通过。该探针不证明未来RHI wrapper的录制取消、丢ticket和槽复用。

两份源文件及上述入口/检查已只读核对。以上保留可复现路径与事前预期；Main随后已经统一完成本机执行，实际结果如下。本席位未自行运行或复跑；probe通过仅关闭相应路线可行性问题，未来正式Module仍需从自己的Interface验收完整生命周期。

### 7.1 Main已执行的本机证据：路线通过，不是Module完成

| 路径/原始证据 | 本机已观察结果 | 不能外推的结论 |
| --- | --- | --- |
| out/review/wave1-meeting/ui-layout-path-probe-headless-layout.json | exitCode=0、stderr为空；row/constraints/order/removal、DPI/shared-edges/cold-equivalence、DirectWrite/dirty/font/constraint均PASS；真实measure_calls=5，最终Segoe UI/20/en-US几何为(97.548828,53.203125)；最终UI layout path probe: PASS | 不是正式LayoutDocument/NodeId/失败快照合同已实现；不保证跨字体/OS同位结果，不提供吞吐或帧时延结论 |
| out/review/wave1-meeting/rhi-path-probe-hardware-debug.json | --require-debug、exitCode=0、stderr为空；AMD Radeon RX 6800 XT，software=0，debug_enabled=1；两次37×19读回均mismatches=0，ticket/completed分别1/1、2/2；row_pitch=256、row_bytes=148；debug_messages/errors/warnings均0，hwnd=0、swapchain=0 | gpu_validation=0，未启用GPU-based validation；device_loss_recovery=not_tested；不证明未来RHI的取消、ticket丢弃、槽复用、关闭或重建策略 |

UI的额外观察为nested native local-sum=0、未舍入绝对位置=0.8、目标一次绝对边界snap=1。这支持将Yoga内部舍入关闭、逻辑结果保留并由composition单点处理目标像素视图的选型，不作为上游生产缺陷或完整文字渲染验收。

RHI实际输出明确CPU owner在submit前撤销，但显式submitted hold在fence后释放；它验证此次探针的真实持有和读回路径。不能从这份显式COM持有样例推导出尚未实现的Module自动持有机制已经正确。两份JSON中的elapsed_ms只是单次进程时长，未用于模块性能排名或收益估算。

**Main据此确认两个模块入选。** 原“RHI条件推荐”已转为“路线可推进”；任一未来实现仍须保持本报告的语义与所有权门槛，而不是以探针PASS豁免正式验收。

### 7.2 Main追加的精简链接证据：独立性不再只是源码推断

Main进一步删除Runtime/JIT库后重新链接并运行相同真实探针；以下构建清单和运行结果均已只读核对，本席位没有执行。

| 切片 | 精简构建清单与实际链接面 | 独立运行证据 |
| --- | --- | --- |
| UI布局 | out/review/wave1-meeting/ui-layout-minimal-build.json：探针obj + 共同global_new_delete.cpp.obj + mimalloc导入库 + Windows系统库 + yogacore.lib + dwrite.lib；不含atelier_runtime、atelier_jit_probe、atelier_jit_native_client或LLVM链接库；输出out/release/wave1-ui-layout-minimal.exe | ui-layout-minimal-headless-layout.json：exitCode=0、stderr为空；row/约束/顺序/删除、DPI/共边/冷热等价、真实DirectWrite/dirty/font/父约束均PASS；measure_calls=5，最后UI layout path probe: PASS |
| 单后端窄离屏RHI | out/review/wave1-meeting/rhi-minimal-build.json：探针obj + 同一全局new/delete覆盖 + mimalloc导入库 + Windows系统库 + d3d12.lib/dxgi.lib；不含Runtime/JIT/LLVM库；输出out/release/wave1-rhi-minimal.exe | rhi-minimal-hardware-debug.json：--require-debug、真实RX 6800 XT、software=0、debug_enabled=1；两次mismatches=0且完成值覆盖ticket；debug消息/错误/警告均0，exitCode=0、stderr为空 |

上述四个JSON均位于out/review/wave1-meeting/。两份build.json保存了正式宿主release编译标志、实际工作目录和完整clang-cl/lld-link命令，是Main可直接追溯的最小链接路径；运行结果不是依赖完整业务可执行程序间接得到的PASS。可复现启动入口为out/release/wave1-ui-layout-minimal.exe（无参数）及out/release/wave1-rhi-minimal.exe --require-debug，须保留其共同分配器DLL与对应SDK环境。

**依赖裁决**：布局求解/测量切片和GPU资源提交切片都不需要链接业务Runtime、GC、JIT或LLVM；它们可以独立推进，复用共同原生分配政策并不意味着业务运行时耦合。DirectWrite依然只属于布局探针/叶Adapter，不能反向成为跨平台布局核心的硬依赖。

**证据限制**：这不是从空目录配置正式新target的验收。清单复用了既有release宿主标志、include路径和全局new/delete对象（对象存于现有atelier_jit_acceptance目标的输出目录），因此只证明该探针的链接/运行不依赖Runtime/JIT库，不抹去第2节“当前根CMake仍无条件配置LLVM”的事实。生产Module/正式CMake targets仍未实现，今后必须由Main把共同分配器接入与局部target组织到唯一构建定义，不能复制一套永久构建系统。原先未覆盖的局部ID/失败快照、录制取消/丢ticket/复用、GPU-based validation与设备恢复边界保持不变；单次elapsed_ms仍不用作性能比较。

## 8. 后续模块归属

当轮评估后，布局与 RHI 已分别建立正式模块；现行合同和验收入口见[布局](../../../modules/ui-layout/README.md)与[RHI](../../../modules/rhi/README.md)。原六份交接文件及下一任务启动说明已被模块现行文档取代，本报告只保留当轮独立链接实验和质询依据。

## 9. 支线质询记录（已完成至少一轮双向回应）

初稿先落本文件，再分别向WaveUi、WaveRhi发送报告路径和三项具体争议；其后完整读两方独立初稿并直接回应。没有把初始意见相近算作讨论完成。报告位置：同目录ui-layout.md、rhi.md；以下是本席位收到的论点、证据、回应与修正。

### 9.1 与WaveUi

- **反方主张**：不应因少依赖默认自写；Yoga 3.2.1已有约束、measure与dirty机制。LayoutNodeId是Document内几何身份，不是第二张业务对象表。
- **本方回应/反例**：认可成熟求解复用，只有永久固定框/无伸缩stack才可能让自写明显更窄；当前候选包含受约束测量和失效，不能把这些成本略去。runtime.hpp把值存储与ObjectId分开，source.hpp也有非业务编译分类，故私有计算身份本身不违规。反例是外层再保存style/children/cache并与Yoga同步：增加第二权威和失效路径，不符合工程指南。
- **对方回应与修正**：Yoga树作为唯一几何权威；外层仅owner/代际映射、revision、已发布几何。业务Root/事件Entry归外部绑定owner，默认内容快照不建Root；当前私有Business Entry不算通用UI接口。双方依据[Y1、Y2]区分树修改自带dirty与外部内容变更显式mark dirty，不要求重复缓存。
- **测量争议与修正**：我要求无窗口首验收不能使用假字体metric。对方提出现有Windows SDK的DWriteCreateFactory→CreateTextLayout→GetMetrics；已核实一手签名无需render target，metrics单位为DIP[D1、D2]。撤回“无独立文本库会阻断布局”的过宽疑虑；DirectWrite只在探针/叶Adapter，不是核心Module硬依赖。系统字体仍影响具体数值，必须对照本机同条件真实度量，不宣称跨OS一致。
- **未决/证伪条件**：若生产布局context暗藏业务裸指针/JIT地址、测量时访问Runtime、外围复制第二份style/children/cache，独立性结论失效。O(修改数)重排或恒定measure次数没有证据；错误/发布与代际检查仍是未来Module验收，不由上游probe代证。

### 9.2 与WaveRhi

- **反方主张**：clear/readback只是资源提交切片，不是完整RHI；我的初稿仅强调提交后fence，遗漏未提交Recording和ticket丢弃路径。[G1]明确D3D12 command list不持有传入资源引用。
- **本方回应/修正**：接受，改名“RHI资源提交首里程碑”，明确不含PSO/shader/draw/Present。采用Recording首引用保活→取消撤持有/提交移交Device in-flight→完成退休；ticket是标识，丢失不影响device所有权。这些真实不变量给单后端Module提供深度，不需要为了抽象名义复制D3D12全部接口。NVRHI已有类似内部持有/退休机制[N1]，重用优先级应随真实范围复评，不一概排斥。
- **本方再质询**：直接SDK探针只保持COM对象直到等待完成，不能证明尚不存在的wrapper正确处理录制取消、丢ticket和槽复用。
- **对方明确回应**：接受“平台探针通过≠Module里程碑通过”；rhi-path-probe.cpp只验证SDK资源/提交/读回/完成与显式持有，未来Module须另从真实Interface验收取消、丢ticket、复用/关闭。本机硬件和debug layer状态由Main执行后填写，不能由SDK文件存在推断。
- **已关闭与未决**：初稿要求真实设备能力、精确读回与队列完成，否则不批准第二模块；Main在7.1的硬件debug证据已关闭该路线可行性问题。仍未决的是未来Module实现：若用GC代替GPU完成、ticket析构释放在飞资源或未提交录制保留裸引用，所有权方案仍不成立。设备丢失后的工程恢复不在本次平台探针证明范围。

### 9.3 三方交叉：DPI与多surface

RHI提出同一布局服务两个DPI目标，不能把单个像素结果当布局权威。我提出“未舍入逻辑结果→按target/DPI派生不可变像素视图”；WaveUi接受并在其4.4修正为composition/output Adapter单点拥有转换，Yoga scale=0禁内部舍入，RHI不二次round子宽。Text测量节点的PixelGrid特例不因此强加给所有普通叶。仍未冻结glyph ink/overhang、scissor/clip和hit-test政策；DWRITE_TEXT_METRICS不等于glyph overhang[D2]，box snap不能证明文本不会裁切。该未决项不需要阻塞无渲染布局与离屏资源两个首切片。

## 10. 来源、版本与证据等级

### 本仓源码证据

完整读会议章程和修订后36项baseline.json；保留其既有dev/release验收来源，不自行重复执行。依赖事实来自CMakeLists.txt、cmake/jit.cmake、cmake/dependencies-lock.json、cmake/toolchain-lock.json及src目录。关键公开契约：runtime.hpp:283–330（单线程、注册/值/对象）、369–380（执行/发布准入与宿主收集）；native.hpp:22–26、37–60、118–124（受限方法签名与同一类型定义生产者）；execution.hpp的ExecutionScope/ExecutionFrame（同步稳定槽）；source.hpp的Environment/Program/compile_source（编译分类与自有产物）。business.cpp:400起BusinessProgram及try_publish只证明私有适配器所在层，不外推通用宿主能力。

README、docs/engineering.md、CONTEXT.md、docs/language-contract.md为现状和契约；runtime-model已实现章节为补充。来源版本是会议固定哈希，不存在Git diff，本报告不编造补丁引入缺陷。

### 外部机制证据

访问日期均为2026-09-25。新检索使用Exa，已知一手URL直接读取；持续文档不是对本机可选组件、驱动或clang-cl兼容性的证明。下列条目均已读对应能力段，未把二手摘要当结论。

- [Y1] [Yoga v3.2.1 YGNode.cpp](https://raw.githubusercontent.com/facebook/yoga/v3.2.1/yoga/YGNode.cpp)：CalculateLayout、父子所有权、插入/删除的dirty传播、MarkDirty仅适用于measure叶。以固定tag为准；所需完整规则仍见ui-layout.md。
- [Y2] [Yoga官方incremental layout](https://www.yogalayout.dev/docs/advanced/incremental-layout)：style/children变化传播dirty；父约束变化可使原clean节点重新计算。滚动文档，仅为机制解释。
- [D1] [IDWriteFactory::CreateTextLayout](https://learn.microsoft.com/en-us/windows/win32/api/dwrite/nf-dwrite-idwritefactory-createtextlayout)、[GetMetrics](https://learn.microsoft.com/en-us/windows/win32/api/dwrite/nf-dwrite-idwritetextlayout-getmetrics)：真实无绘制目标的排版/度量入口；本机font结果不跨系统量化承诺。
- [D2] [DWRITE_TEXT_METRICS](https://learn.microsoft.com/en-us/windows/win32/api/dwrite/ns-dwrite-dwrite_text_metrics)：DIP、宽度/尾空格和glyph overhang的区分；不能把box几何当最终glyph绘制范围。
- [G1] [D3D12 recording command lists](https://learn.microsoft.com/en-us/windows/win32/direct3d12/recording-command-lists-and-bundles)：command list不保留传入资源引用；allocator复用需GPU完成。
- [G2] [D3D12 readback data](https://learn.microsoft.com/en-us/windows/win32/direct3d12/readback-data-using-heaps)：readback资源是buffer；Map不与GPU同步，须先确认fence和资源barrier。
- [G3] [ID3D12Fence::GetCompletedValue](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12fence-getcompletedvalue)：device removed返回UINT64_MAX，不是成功完成全部提交。
- [N1] [NVRHI ProgrammingGuide](https://raw.githubusercontent.com/NVIDIA-RTX/NVRHI/d0c8e30d5f8d58c3b838b06aa1d8d0a912bea076/doc/ProgrammingGuide.md)、[同提交CMakeLists](https://raw.githubusercontent.com/NVIDIA-RTX/NVRHI/d0c8e30d5f8d58c3b838b06aa1d8d0a912bea076/CMakeLists.txt)：固定提交d0c8e30d5f8d58c3b838b06aa1d8d0a912bea076，具内部资源持有/状态追踪；native device仍由应用创建。其DX12配置需要DirectX-Headers/Guids targets，默认fetch的tag为v1.717.0-preview，因此不声称现有SDK零新增依赖。这里的runGarbageCollection是GPU资源退休维护，不是本项目业务GC。
- [W1] [Win32消息与消息队列](https://learn.microsoft.com/en-us/windows/win32/winmsg/using-messages-and-message-queues)：真实线程消息队列、GetMessage/TranslateMessage/DispatchMessage，window procedure完成前分发不返回；不预设Runtime可任意重入。
- [A1] [WIC CreateDecoderFromFilename](https://learn.microsoft.com/en-us/windows/win32/api/wincodec/nf-wincodec-iwicimagingfactory-createdecoderfromfilename)：具体文件decoder入口、HRESULT和Windowscodecs.lib；仅支持提出格式解码切片，不证明整个资产管线。
- [J1] [enkiTS v1.11 README](https://raw.githubusercontent.com/dougbinks/enkiTS/v1.11/README.md)：C/C++任务入口、pinned task、依赖及WaitforTask可执行工作；仅候选实现能力，不接受其性能宣传作为本项目结果。
- [M1] [DirectXMath官方说明](https://learn.microsoft.com/en-us/windows/win32/dxmath/directxmath-portal)：SIMD友好线性代数/图形数学能力。滚动机制文档，未固定新工程依赖、未核实本机该头文件或测量。

### 路径状态与阻塞

- **源码证实**：当前target依赖、同步Runtime契约、值/对象区分、两份探针的真实入口存在。
- **外部机制支持**：布局dirty/measure、DirectWrite度量、D3D12录制/完成/readback、可复用库入口。
- **设计取舍/仅推断**：定性成本收益与未来Module Interface；没有虚构性能数值。组合与两个独立handoff目录已经由Main确认，不再标作未决选型。
- **本机实验通过（Main执行）**：7.1/7.2的四份运行JSON与两份精简build.json已只读核对；真实Yoga/DirectWrite和RX 6800 XT硬件D3D12 debug路径均exitCode=0，显式删除Runtime/JIT/LLVM链接库后仍通过。该证据支持链接独立性，不代表干净配置/正式targets完成；不把WARP、GPU-based validation或未覆盖的Module合同混入结论。本席位没有执行或复跑。
- **当前状态**：报告、双向质询、候选裁决与路线可行性证据均已完成，无本席位阻塞。两个生产Module尚未实现，其局部ID/失败快照、录制取消/丢ticket/资源复用等合同由后续各自session履行；Main已把六份handoff文档分别交WaveUi/WaveRhi，本席位不编辑那两个目录。
