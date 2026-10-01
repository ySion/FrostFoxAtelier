# FrostFoxAtelier

面向 Windows x64 游戏的精简工具包（引擎 kit），自有语言服务于实际游戏与工具需求。已确认只采用 JIT 执行业务语言：代码必须编译完成后才能执行，不建设解释器或字节码 VM；类型与对象语义仍由共享核心运行时负责。当前已实现共享类型/对象运行时、对象引用最小闭环与宿主显式推进的分步 GC，完成 Windows x64 JIT 平台验收，并用最小源码前端打通机器生产推进、执行根、安全点、显式协程、单入口兼容方法体热更新，以及原生 String/Array/记录的同步借用、直接存储访问、String/Array/记录拥有型局部、同模块多函数调用及源码 enum/struct 的共享类型发布。首个场景编辑器的入口见下节；完整语言、通用方法分派/布局迁移及完整渲染器仍在后续范围。

另已实现独立的原生 UI 布局核心：稳定局部身份、共享列与换行、Flex/Grid、虚拟列表、真实内容测量、事务几何发布，以及完整内容版本消费的验收。布局核心继续保持独立；窗口、生产控件与绘制由编辑器上层接入，详见[布局模块](docs/modules/ui-layout/README.md)和[编辑器模块](docs/modules/editor/README.md)。

项目协作入口为 [AGENTS.md](AGENTS.md)，编程范式与性能约定见 [工程指南](docs/engineering.md)，共享术语见 [CONTEXT.md](CONTEXT.md)。Runtime、源码与 JIT 的模块归属、调用链、所有权和已知边界集中在[执行结构说明](docs/runtime-structure.md)。现行源码语法统一由 [LANGUAGE.md](LANGUAGE.md) 维护。

Runtime 已支持以 `core.Object` 为根的正式单继承、基类引用视图、继承成员访问，以及原生和描述型派生对象的完整 GC/构析。现有同步与协程源码入口可接受符合绑定基类的派生对象；源码 class 声明、虚方法覆盖和布局迁移尚未开放。具体合同见[对象单继承](docs/runtime-model.md#正式对象单继承)。

对象字段已支持定型强引用序列，宿主通过 `with_native` 同步借用原生载荷，通过 `apply_reference_changes` 原子提交序列与引用槽的成组修改。序列参与同一 GC，支持暂停标记期间的扩容、移位和清空；该对象引用序列仍由原生宿主操作，尚未开放对应源码操作；无 GC 引用的值数组已有同步借用及拥有型局部。具体合同见[引用序列与受保护原生访问](docs/runtime-model.md#引用序列与受保护原生访问)。

C++ 自动反射使用 `ACLASS/ASTRUCT/AENUM/AFIELD/AFUNCTION` 与 `AGENERATED_BODY`。锁定版本的 Clang LibTooling 生成 `.gen.hpp/.gen.cpp`，继续通过现有 Runtime 发布；World 的七种真实类型已迁移。i32、枚举及三种参数传递方式已贯通原生、源码与 JIT，见[调用链验收](acceptance/reflection/calls.cpp)。写法、构建接线、首片限制和端到端入口见[自动反射](docs/reflection.md)。

引擎 C++ 基础库已提供拥有型 `Array<T>/String` 和借用型 `Span<T>/StringView`，支持宏反射字段、同一 Runtime 类型身份及原生/描述型值构析。String 使用 UTF-8，具备 15 字节 SSO、赋值容量复用与已验证文本追加路径；Array 对符合实际平凡操作的元素使用批量复制、扩容和保序删除，平凡复制赋值复用容量。反射数组首片限于无 GC 引用的值元素。接口、所有权、接入示例及边界见[基础库](docs/foundation.md)。

## 启动首个场景编辑器

在仓库根目录的 PowerShell 中执行唯一产品入口：

```powershell
. ./tools/enter-dev.ps1
cmake --preset dev
cmake --build --preset dev --target atelier_editor
./out/dev/atelier_editor.exe
```

编辑器直接消费正式 Actor/Component/World。启动时创建一个带 Transform 和 Cube 的示例 Actor；`+ Add Actor` 创建空 Actor，右侧 `+ Transform`、`+ Cube` 添加实际组件，`− Cube` 只移除方块能力并保留 Actor。位置 XYZ 编辑实际 Transform 字段，数值拖调实时预览，松手只记一次撤销；Esc 取消，文本输入焦点下 Delete 不删除 Actor。列表、可拖分隔和相机观察沿用同一窗口入口。
右键拖动视口观察，滚轮缩放；操作与数值/坐标约定见[编辑器模块](docs/modules/editor/README.md)。窗口 resize、最小化恢复与 DPI 过渡使用同一正式帧/输入路径。
World 提供完整 TRS 与权威矩阵，renderer 仅消费矩阵值；当前面板仍只提供位置 XYZ，尚无旋转/缩放控件、保存重开、JIT 单步、节点图、视口拾取或 gizmo。[首版方案](docs/first-editor-plan.md)作为 M0/M1 历史保留；当前 World 接入遵循[对象模型合同](docs/research/game-world-object-model.md)，不启动 S3 面板体系。
原生 UI 的 S1 已收口。S2 已接入共享 Unicode 单行编辑、参数搜索、剪贴板与 IMM32 平台往返；dev/release UI 13/13、真实搜狗输入及最终普通窗口各 709 帧验收均通过。字段切换统一取消并解除旧原生输入关联，再恢复新目标，已验证旧候选不会落入新字段。数值非法 Enter 保留草稿，提交/取消后保留焦点；无本地事务时 Ctrl+Z/Y 进入场景历史。最终证据归档于 `out/{dev,release}/editor-evidence-s2/`，[原生 UI 方案](docs/ui-foundation-plan.md)维护唯一排期，S3 未启动。

样式/绘制基础已扩展为 [SS（Style Sheets）](docs/modules/ui/style-sheets.md)：类名、角色、交互状态、token、局部作用域及属性覆盖共用最终样式；接受帧保有文字命中快照。GPU 支持独立圆角、向内边框、RGBA 图片与相邻实例合批。编辑器启动读取可执行文件旁的 `editor.ss`，源文件为 [assets/editor.ss](assets/editor.ss)。这些能力仍在基础 UI 模块中，尚未上升到业务语言语法。

真实窗口验收使用同一个应用可执行文件，依次运行以免多个窗口争用焦点：

```powershell
cmake --build --preset dev --target editor_acceptance
cmake --preset release
cmake --build --preset release --target editor_acceptance
```

每种配置输出 `Editor document acceptance: PASS` 和 `Editor window acceptance: PASS`；只运行无窗口文档链使用 `editor_document_acceptance`。GPU 颜色/深度捕获对应的 PNG 与操作报告保存在 `out/<配置>/editor-evidence/`，包括状态/字体/256 Actor 列表、组件增删和完整 TRS 投影。采样条件和实测边界见[编辑器验收](docs/modules/editor/README.md#验收入口)。
窗口验收启用真实硬件 D3D12 debug/GBV。DPI 项验证当前显示器的过渡消息政策，不替代跨不同 DPI 显示器的实测。运行目录须保留构建部署的 mimalloc、DXC、Agility、PIX DLL 与许可证。

## 构建与端到端验收

在项目根目录的 PowerShell 中执行：

```powershell
# 每个新会话执行一次。缺少 LLVM 时，增加 -InstallToolchain 参数。
. ./tools/enter-dev.ps1

cmake --preset dev
cmake --build --preset dev --target acceptance

cmake --preset release
cmake --build --preset release --target acceptance
```

总入口先完成基础库、反射（含 CLI）和 RHI 验收依赖，再执行 Runtime、源码公共消费者、JIT 业务、布局、生产 UI 和编辑器消费者；真实窗口按顺序运行。成功时输出各模块 `PASS`，包括 `UI acceptance: PASS (15/15)`、`Editor document acceptance: PASS` 和 `Editor window acceptance: PASS`，返回码为 0。失败返回非 0；发布构建也执行全部验收检查。真实输入法使用独立的 `editor_ime_acceptance` 目标，前置条件与证据边界见[编辑器验收](docs/modules/editor/README.md#s2-集成状态与输入法验收)。只构建时使用 `cmake --build --preset dev`；只运行源码公共消费者及 JIT 平台/业务贯通验收使用 `cmake --build --preset dev --target jit_acceptance`（发布配置相应替换为 `release`）。

2026-09-27 的 Wave 1 三线集成已通过上述 dev/release 总验收；提交来源、补丁签出修正、运行证据和已知限制见[集成记录](docs/research/wave1-integration.md)。

首次配置新机器：先按工具链锁文件安装所需 MSVC 工具集、Windows SDK、CMake/Ninja 的 Visual Studio C++ 开发工具，再执行：

```powershell
. ./tools/enter-dev.ps1 -InstallToolchain
```

脚本只在项目 `.tools/` 内下载、校验和解压完整 LLVM SDK（编译工具、开发头文件、CMake 配置和库），并准备当前进程环境，不改系统 PATH 或 PowerShell 配置。已有仅含编译工具的旧目录需要重新执行安装命令补齐 SDK。脚本不负责安装或升级 Visual Studio。`.tools/`、`out/` 和本机用户 presets 均不提交。

唯一版本清单位于 [cmake/toolchain-lock.json](cmake/toolchain-lock.json)，版本升级同时更新锁文件并验证构建环境，不在文档中维护第二份版本表。

第三方依赖由 [cmake/dependencies-lock.json](cmake/dependencies-lock.json) 锁定版本、下载地址和 SHA-256。首次 CMake 配置取得 mimalloc、LLVM SDK 所需的 zlib/zstd，以及 gfx 和它使用的 DXC、Agility、D3D12MA、PIX；压缩包缓存于 `.tools/downloads/`，源码和构建结果位于各配置的 `out/` 下。不要求编译 LLVM 或 compiler-rt 全量源码。

原生代码使用 C++23，警告视为错误；编译使用 clang-cl，静态归档使用 llvm-lib，链接使用 lld-link。只维护 CMake 构建定义。IDE 应使用相同 preset 和准备好的工具链环境。

## RHI 图形验收

RHI 采用固定 gfx 与局部补丁，提供 Compute、Mesh、Vertex 管线和有界执行域。单独构建并运行图形矩阵：`cmake --build --preset dev --target rhi_acceptance`，发布配置替换为 `release`；命令要求支持 Mesh Shader 的真实硬件及 D3D12 debug/GBV，不自动降级为软件或跳过场景。当前实测与边界见 [RHI 模块](docs/modules/rhi/README.md) 和 [验收证据](docs/modules/rhi/acceptance.md)。

构建输出同时部署锁定的 DXC、Agility 与 PIX DLL，并保留 `licenses/rhi`。`atelier_rhi` 不链接业务 Runtime/JIT；顶层 CMake 配置仍需要完整工具链。

## UI 布局核心

正式静态目标 `atelier_ui_layout` 的唯一公开入口是 `ui_layout/layout.hpp`。`LayoutDocument` 统一管理节点身份、结构编辑、测量缓存和事务几何发布；核心不链接 Runtime/JIT/RHI/DirectWrite。DirectWrite 只在验收的文字 Adapter 中使用；正式验收包含最小消费者和 39 个场景。

- 尺寸与排布：Fixed/Content/Expand、数值 min/max、对齐/锚定、table 共享列、wrap 自然换行与自适应重复槽、collapsed；原生 Flex 支持 basis/grow/shrink、分行/分布/reverse 和首基线，二维 Grid 支持轨道、跨度及显式或 sparse 自动放置。
- `VirtualList` 管理逻辑行高、可见范围与滚动锚点；10 万逻辑行的真实消费者只实例化窗口中的行。滚动控件本身仍由上层实现。
- `LayoutView::find` 按完整身份提供常数时间查询。编辑和失败保留当前借用，下次成功发布终止借用；跨发布使用 `LayoutSnapshot` 保有几何。
- 几何快照不拥有内容资源。真实 `TextFrame` 消费者固定相应内容版本，包括未参与测量的固定尺寸叶，完整帧成功后才替换；headless 编辑命令、ghost 取消/提交、失败恢复及旧资源寿命已有验收。

环境准备和配置仍使用上方正式 presets；仅运行布局验收：

```powershell
cmake --build --preset dev --target ui_layout_acceptance
cmake --build --preset release --target ui_layout_acceptance
```

总体 `acceptance` 也包含这些消费者。几何变化仍求解参与布局的实例树；不承诺 CSS 全量、一般循环约束、通用局部调度、异步布局或硬帧预算。窗口输入、生产控件与 GPU 绘制由[UI 消费模块](docs/modules/ui/README.md)和编辑器接入；RGBA 图片绘制与 SS 样式已接入，图片解码、Runtime 资源 composition 及业务语言 UI 接线仍未实施。现行布局行为和测量边界见[合同](docs/modules/ui-layout/contract.md)、[验收](docs/modules/ui-layout/acceptance.md)和[审计与性能](docs/modules/ui-layout/audit.md)。

## 引擎资源连接方案

[图片资源首例](docs/modules/resources/design-proposal.md)目前是设计提案：宿主建立共享 Runtime Texture 对象，业务引用共享，布局和绘制消费固定内容版本。正式源码宿主和受保护原生载荷访问已可用，基础UI已消费不可变CPU图片快照；跨类型源码目录、引用字段编译及Runtime Texture到UI的资源接线仍未实现。这条跨模块业务链尚未通过验收。

## 默认内存分配

原生进程使用共享 mimalloc DLL 与官方 Windows CRT 重定向，普通代码继续调用默认分配函数。`atelier_target` 为每个可执行目标直接编译 [global_new_delete.cpp](src/memory/global_new_delete.cpp)，在一个翻译单元中采用官方完整的 `new/delete` 覆盖，并链接同一 mimalloc DLL。静态库内的代码随最终进程使用该分配器。

本项目的可执行文件和 DLL 集中输出到各配置目录。运行验收程序时，需要保留同目录的 `mimalloc-debug.dll`（开发配置）或 `mimalloc.dll`（发布配置）、`mimalloc-redirect.dll`，以及用于验证跨 DLL CRT 分配的 `atelier_allocator_client.dll`。mimalloc 和重定向器采用上游实现，构建同时保留 `LICENSE.mimalloc.txt`。

实际覆盖包括全局 C++ 分配、采用默认分配器的标准容器，以及经过动态 CRT 的 C 分配。独立堆、直接操作系统分配、GPU/JIT 专用内存和第三方自定义分配不因 CRT 重定向而自动改用 mimalloc。原有析构、所有权和指定释放接口的契约保持不变。

验收会检查 CRT 重定向状态，关闭重定向时返回失败；仅链接 mimalloc 或仅覆盖 `new/delete` 不算通过。开发配置保留上游调试检查，当前尚未测量分配器带来的性能收益。引擎基础容器通过相同分配入口取得存储，具体契约见基础库文档，演进约定见工程指南。

## Windows JIT 平台验收

[acceptance/jit](acceptance/jit) 同时包含真实本机平台实验和最小源码业务验收；两者范围分别说明。开发与发布配置均通过运行时、JIT 平台及业务源码验收。构建接入见 [cmake/jit.cmake](cmake/jit.cmake)。

- 宿主 EXE → JIT → 宿主/独立 DLL 的混合整型、浮点、八参数、64 位值及 32 字节对齐记录互调；原生异常在 DLL 内转换成明确错误，失败不覆盖结果，清理恰好一次。
- Windows 动态函数表登记及 `RtlLookupFunctionEntry` / `RtlVirtualUnwind` 回溯，跨过实际栈重对齐、四层递归及不同生成单元，并确认回到原生调用者；覆盖 3.5/4/4.5/32 KiB 生成栈。当前机器上的 `CaptureStackBackTrace` 在这些动态帧处停止，故验收不依赖它。
- 发布后的代码页 RX、常量页 RO、可变数据页 RW 且不可执行；没有永久 RWX 代码堆。
- 非法 IR 保留文件/行诊断，缺失外部符号保留名称；在实际展开表登记之后注入 finalize/action 与 emission 失败，检查撤表且对应地址回到 `MEM_FREE`，失败候选不影响现有代码；独立版本标签可在代码退休后保留。
- `ResourceTracker` 所有权转移、移除、反复生成/退休和 JIT 退出；退休后符号不可查、展开表撤销、旧代码页不再可执行，其他代码单元仍可调用。跨单元调用的使用者先于被调用者退休；不会把资源追踪器误当作活动调用保护。

采用显式 `ObjectLinkingLayer` / JITLink 与原生 COFF。LLVM 生成并保留 `.pdata` / `.xdata`；一个进程内插件通过 LLVM allocation actions 在链接完成后登记 Windows 展开表、在释放对应内存前撤销，失败回退和所有权转移沿用同一分配生命周期，不另建一套代码资源表。撤表失败会终止进程，不能继续释放仍被 Windows 引用的页。生成代码使用 Large code model，不假设宿主 DLL 位于 JIT 内存的 ±2 GiB 内。

锁定 LLVM 的 finalization 错误出口存在已复现的代码页泄漏，使用 [JITLinkMemoryManager.cpp](third_party/llvm/JITLinkMemoryManager.cpp) 的最小源级修复：失败时清理分配，已执行的动作先逆向撤销，正常分配策略不变。该完整翻译单元保留上游许可证、原始 SHA-256 与编译期版本检查，通过正式目标链接，不手改 SDK 二进制；升级 LLVM 时必须重新核查此修复。展开表安装/撤销的参数包分别为 24/8 字节，均留在 SDK 的内联缓冲内。

预编译 LLVM 的 Release 静态 CRT ABI 分别隔离在正式源码后端 `atelier_source_backend.dll` 和平台验收 `atelier_jit_probe.dll`；宿主与 `atelier_jit_native_client.dll` 保留原有开发/发布 CRT 配置。两个 LLVM DLL 共用构建中的展开支持与修复对象，LLVM/STL 对象、异常和分配所有权不跨隔离接口。运行完整 JIT 验收需保留上述 DLL 及本配置的 mimalloc/重定向 DLL；输出目录保留 LLVM、zlib、zstd 的许可证副本。

已完成长会话反复物化/退休、最多 512 个同时存活单元及定型成功调用的本机测量，方法、数据与限制见[二次验收记录](docs/research/jit-industrial-research.md#二次验收记录2026-09-24)。这只是平台微型样例，不替代真实业务负载的编译延迟、稳态吞吐或行为热重载验收。

**平台范围限制**：只验证受控、已定型 LLVM IR 的进程内同步执行，不支持任意 C++ 目标文件、C++ 静态构造/TLS 初始化或 C++ 异常跨 JIT 帧。语言源码行映射、PDB/源码断点和变量查看、真正的行为热重载仍未接入；共享业务与执行根的最小贯通见下节。

## JIT 业务执行闭环

正式消费者包含 [source/program.hpp](src/source/program.hpp)，链接 `atelier_source`，使用同一个 `source::Program` 完成请求、编译、发布、绑定、同步调用及协程启动。标量/枚举值参数和返回值使用 Runtime 的 `Scalar`，借用参数使用 `Argument`，不会暴露 LLVM 或私有 DLL 参数布局。原生记录、Array 和只读 String 也通过 `Argument::borrow` 进入同一接口；布局来自 Runtime，生成代码访问调用方原有缓冲。[独立最小消费者](acceptance/source/main.cpp)仅依赖该公共入口，验证同步结果、兼容替换、旧版本租约和程序关闭后的挂起帧存活。[关停消费者](acceptance/source/shutdown.cpp)覆盖关闭入口、取消任务、等待执行静止及最后释放 Runtime 的完整顺序；具体契约见[宿主关停](docs/runtime-model.md#宿主关停闭环)。

[原生值借用消费者](acceptance/source/native_values.cpp)验证 C++ 持有 `Array<原生记录>`、源码按引用原地更新、C++ 直接读取结果；包括 64 字节对齐、零元素复制、嵌套数组、UTF-8 字节读取、只读/别名/越界与兼容版本替换。调用链及边界见[原生值借用](docs/native-value-borrows.md)。

[拥有值消费者](acceptance/source/owned_values.cpp)进一步验证源码临时 String/Array、原生批处理、记录/元素的直接可写引用、字面量、深复制和全部退出路径的清理。类型与调用效果声明沿同一 Runtime 发布，`AFUNCTION(BorrowSafe)` 开放受控原生算法。实际语法与范围见[拥有型基础值](docs/source-owned-values.md)，端到端入口仍为 `source_acceptance`。

[模块函数消费者](acceptance/source/modules.cpp)覆盖前向/嵌套调用、直接定型参数、跨函数引用别名、逐层资源清理、对象根与 GC，以及 async 入口调用同步助手。宿主通过 `Program::request(name, source, entry_name)` 选择公开入口；整模块随代码版本一起发布和退休。当前内部调用图无环，不支持递归或调用 async 函数。详见[同模块多函数](docs/source-function-modules.md)。

[类型声明消费者](acceptance/source/types.cpp)验证源码 `enum/struct` 的前向嵌套声明、Runtime 布局与反射、C++ 和 JIT 共用存储、值复制清理及类型与代码联合发布。新类型在注册封闭前发布，代码更新保持类型定义与身份；原生方法可按引用直接消费这些描述型记录。执行链与边界见[源码类型声明](docs/source-type-declarations.md)。

单独运行该消费者：`cmake --build --preset dev --target source_acceptance`（release 同理），成功输出 `Source public consumer: PASS`。配置时设置 `-DATELIER_BUILD_JIT_ACCEPTANCE=OFF` 可关闭平台/JIT 完整验收及后端故障注入，正式源码库和该最小消费者仍可构建；默认 ON 保留既有完整验收入口。

同一入口包含[连续会话验收](acceptance/source/session.cpp)：原生与描述型对象各在同一个 Runtime 中交错运行任务、取消、分步 GC 和兼容更新，核对预热后的资源变化、旧版本集中退休及最后关停。工作负载和实际结果见[阶段收尾](docs/research/runtime-stage-closeout.md#连续运行补充验收)。

本阶段的正式接口性能与资源基线见[测量记录](docs/research/runtime-stage-measurements.md)。测量不随普通构建或验收执行；复跑命令：

```powershell
cmake --build --preset release --target atelier_source_measure
./out/release/atelier_source_measure.exe --batches 7 --calls 20000 --max-tasks 1024
```

[机器推进源码](acceptance/jit/advance_machine.source)经过[词法/语法与类型检查](src/language/source.cpp)、定型语法树、[LLVM 降低与本机编译](src/jit/source_backend.cpp)，操作既有原生/描述型机器。循环、正值分支、`advance(float)` 调用及 `state.cycles` 更新由源码决定，不再由固定手写业务 IR 决定；`state.heat` 保持不变。宿主适配器另行创建并返回同类型执行后快照，它仍由原 Runtime 管理，并非源码的隐式克隆或 `void` 返回值。公共数据结构复用 [machine.hpp](acceptance/machine.hpp)。

- [ExecutionFrame](src/runtime/execution.hpp)沿用 `ObjectRoot` 及共享屏障/撤销机制。生成帧只预留接收者槽；机器验收自行持有快照根，源码引用局部和临时值按编译布局准备额外稳定强槽与不保活的观察存储。持有局部跨完整新 GC 周期存活，块退出后只剩观察则可回收，循环/早退/错误均显式清理。
- 顶层以 `ExecutionScope` 覆盖入场至帧清理，内部准入检查拒绝原生借用或遍历内重入；同一 Runtime 的活动调用统一计数。真正原生调用仍保留 `Runtime::call` 的保护；普通函数的生成安全点按宿主请求与单步预算推进同一个 GC；原生聚合借用及拥有值区间把收集留到返回宿主后，不引入另一个收集器或 Tick 调度器。
- 错误通过定型 thunk 的状态与固定诊断返回；IR 显式清根，宿主在 RAII 兜底之前验证清理完成。跨 Runtime、错类型、失效代际与槽位复用均按原身份规则拒绝，已发生的业务修改不回滚。
- LLVM 与 C++ 所有权继续隔离。活动调用在 DLL 内持有代码；已实际验证原生回调销毁外层代码句柄后继续执行，返回后释放代码页和展开表。两条生成路径共用 [Windows 展开机制](src/jit/windows_unwind.cpp)。
- 源码入口接受零个或多个 float/uint/int/枚举参数，支持值、只读引用及可写引用，同步入口另支持已注册原生记录/Array 的引用和只读 String 引用，返回仍为 void 或标量/枚举值；已定型数值不隐式转换。调用先校验数量、类型和结果存储；失败状态与成功标量分开，只有原生调用、结束回调和生成清理全部成功才提交结果，失败不覆盖调用方结果。
- 成员名称、字段路径和原生方法签名来自同一 Runtime 的元数据；前端/后端只使用宿主绑定表索引，不知道机器字段名字。机器快照的创建、轮询后刷新与结果根交接归宿主验收适配器；不访问对象的标量或原生值借用入口无需接收者、快照或登记对象根。
- 原生注册和源码发布共用 `FunctionSignature` 及 Runtime 解析规则；原生 void 或标量/枚举值结果已经贯通动态调用、源码表达式和 LLVM 降低，保留原生成员的 const 接收者信息。嵌套实参先顺序求值再打包，原生失败、漏写或错误种类的结果不提交给源码调用方。

开发/发布的正式目标均运行上述[真实端到端验收](acceptance/jit/business.cpp)。当前支持单一公开入口与模块内同步函数、标量、同步值借用、String/Array/记录拥有型局部、源码 enum/struct、受控原生算法调用、既有成员、`if/while/return`，以及单绑定对象类型的持有/观察局部、`null/new/observe/try_acquire`、身份比较与词法根清理。完整现行词法、数值、引用与协程规则见 [LANGUAGE.md](LANGUAGE.md)；长期设计方向见[语言契约](docs/language-contract.md)，执行协议见[运行时说明](docs/runtime-model.md#已实现的最小业务执行协议)。新增引用在原生/描述型对象上各执行 12 个独立场景；其中多返回场景覆盖首/中/末早退、正常落尾和原生/快照发布失败，检查发布前撤根、词法 GC 与部分副作用。旧数值、错误、GC、展开和代码退休验收继续通过。

构建将 `advance_machine.source` 复制到可执行文件旁，业务验收在启动时读取并编译它；运行时须保留此文件。修改项目中的源码后重新构建即可更新副本，新增验收也会在同一进程中独立编译不同源码并执行。编译诊断带源名/行/列；热更新请求另附单调请求号。超长源名仅在诊断与 COFF 文件标签中按首尾缩略，避免吞掉请求号、位置或原因，并保持 UTF-8 边界；请求快照与编译元数据仍保留完整名字，短名诊断格式不变。这不等于已有文件监视、机器码源码行映射或断点调试。

当前仍不是完整语言、任意引用值执行帧、永久通用 ABI、通用跨方法热重载/布局迁移或业务吞吐优化的交付。前端不链接 LLVM/Runtime，只共享参数模式与序列操作的轻量声明，降低位于 `src/jit`，发布与成员桥接位于 `src/source`；机器快照、统计与故障模式留在验收消费者。没有解释执行回退。字段/方法访问使用缓存路径与 ID，既有描述型回调的动态查找仍保留。共享签名与原生标量返回的完整约束见[函数契约](docs/runtime-model.md#共享函数签名与结果)，记录/容器的按值参数与结果、对象引用参数与结果及借用返回尚未开放。

前端与源码执行链已完成一轮[专项审查及真实机器批量基线](docs/research/jit-industrial-research.md#v001-源码专项审查与真实机器基线)：修复业务链接根因诊断丢失，补充 owner 撤销后原生异常的组合验收，执行固定种子源码变异/数值对照，并分别测量编译、批量调用和回收。已有基线不等于完成业务吞吐优化或全面安全审计。

保持语义的[快照、字段桥接与安全点单因素实验](docs/research/jit-industrial-research.md#快照字段桥接与安全点单因素实验)已完成，三个变体未因不稳定的收益进入正式路径。随后源码引用局部与[单入口兼容方法体发布](docs/runtime-model.md#单入口兼容方法体发布)均通过双配置验收。这两个增量没有重新测量吞吐，不沿用旧基线宣称提速。

早退路径现在共用按需发射的用户槽清理块，避免每个 `return` 重复生成整份清理 IR；仍在快照复制与发布之前结束用户引用。该窄优化已通过[小输入交错对照与开发/发布验收](docs/research/jit-cleanup-experiment.md#92-小输入交错对照与清理共享晋升)，改善多早退源码的编译成本，不改变槽数量、安全点或清理行为，也不据此宣称业务执行/GC 提速。槽复用仍未加入。

### 单入口兼容方法体热更新

正式 [source::Program](src/source/program.hpp) 先绑定 Runtime 与对象目录，首次成功发布从真实编译产物解析并固定函数声明，不重复手写源码签名。`request` 保存不可变源码和声明/依赖快照，并准备源码类型布局；`prepare` 完成编译、链接和物化，`try_publish` 在 Runtime 静止点提交。后续兼容性包含函数名、参数/结果、接收者要求、成员目录及实际服务地址。它不是通用 `Runtime::MethodId` 分派器，不修改原生方法表或类型布局。

- 同一 Runtime 任一语言调用仍活动，或正处于原生借用、遍历/GC 回调时，发布返回 `busy`，候选保留供外层返回后重试。暂停的 GC 周期本身允许兼容发布；安全点不暗中切换版本。
- 最新请求一经接受，旧候选不能再提交；最新源码的解析、类型检查或真实链接失败，均保留上次已发布版本，不回退发布过期候选。重复提交及不兼容入口/签名/接收者/服务表明确拒绝。
- 模拟 Tick 与长期工具事件通过稳定弱入口选择当前版本，不保活接收者；显式 `Lease` 固定旧版本并保留其代码和展开信息。程序所有者在回调中销毁也不损坏本次调用，但后续稳定入口立即关闭；最后一个代码持有者释放后才能退休。
- 首次声明固定接收者要求：无 self 的入口不能在兼容替换中新增 self；已有接收者即使新版不再读取，也仍需提供。`describe()` 返回共同签名，`bind<R,Args...>(expected_execution)` 冷端核对宿主期望并返回原入口；执行种类默认同步，协程传入 `FunctionExecution::coroutine` 后通过同一入口的 `start()` 创建实例。
- Version 保存自己的已验证元数据和实际调用/引用槽需求。发布后的调用不再比较整份成员目录；原始未绑定入口仍完整检查。每次 Invocation 独占暂存空间，同目录下不用九参方法的入口不再因此扩展缓冲。0→9→1 需求变化及旧租约各自存储已通过验收，不据此宣称吞吐或帧时提升。
- 原生/描述型对象各执行 12 个隔离场景，开发/发布配置均通过。覆盖短名、2100/8192字节及UTF-8长源名的请求诊断与失败保旧，完整源名成功发布，原对象身份、地址和存量状态保留，引用槽需求改变，乱序完成，作用域退出，暂停 GC，以及 `RtlLookupFunctionEntry` / `VirtualQuery` 观察到的旧展开表撤销和代码页释放。

没有加入后台编译线程、文件监视、活动帧迁移、跨发布单元源码调用/内联失效或对象布局迁移；Runtime、宿主回调与 DLL 仍须活得比使用它们的调用/租约长。具体发布与退休协议见[运行时说明](docs/runtime-model.md#单入口兼容方法体发布)。

## 执行链路

源码请求、编译、发布、同步调用、协程恢复及代码退役的完整链路见[结构与执行链](docs/runtime-structure.md#从源码到发布)；下图说明同一类型与对象体系的定义来源和访问入口。

协程使用[现行规范](LANGUAGE.md#显式协程) 的 `async` 入口和 `yield` 语句。源码仍经 LLVM/LLJIT，新增显式协程转换；核心 [Coroutine](src/runtime/execution.hpp) 管理帧状态、恢复准入、合作取消和结果。挂起帧复用共享 GC 根并持有原代码版本；宿主决定恢复时机。真实源码验收覆盖挂起期间 GC、取消、失效接收者、错误首因、活动拥有者销毁和热更新后的旧代码退休，包含在既有 `jit_acceptance` 与总 `acceptance` 中。

Runtime、宿主回调和 DLL 须活到所有非终态帧清理完成。当前没有 `await` 组合、跨线程恢复、原生深层挂起或挂起帧迁移；LLVM 降低与宿主绑定通过正式源码模块提供。设计取舍见[工业协程研究](docs/research/coroutine-runtime-research.md)，当前行为见[核心协议](docs/runtime-model.md#协程核心协议)。

```text
原生 NativeRegistration<T>::build() ─┐
直接构造 TypeDefinition ────────────┼─ Runtime::register_types(一组定义)
单条 register_type(TypeDefinition) ─┘
                                    │
                          唯一类型表与不可变元数据
                                    ├─ 对象：create → ObjectRoot → id() → read / write / call / destroy
                                    └─ 值：initialize_value / copy_construct_value / assign_value / destroy_value
                                           使用调用方提供的存储，无独立对象身份
```

- [src/runtime/runtime.hpp](src/runtime/runtime.hpp)：运行时公开契约、定义输入和元数据。
- [src/runtime/runtime.cpp](src/runtime/runtime.cpp)：注册验证、布局、存储、生命周期和动态调用。
- [src/runtime/native.hpp](src/runtime/native.hpp)：从 C++ 成员指针生成类型定义与调用适配，不拥有第二份类型表。
- [acceptance/runtime/main.cpp](acceptance/runtime/main.cpp)：完整验收入口。两种来源的机器由同一消费函数操作。

原生 C++ 类型通过成员指针访问字段，默认适配使用 `std::addressof` 取得真实子对象地址，不调用成员类型重载的 `operator&`；同一定位用于标量访问、引用枚举和 GC 强边追踪，正式验收覆盖误址与漏边反例。注册顺序不需要匹配内存布局。描述型记录/对象由运行时计算布局，不需要对应的 C++ 类。值记录内联存储，没有独立对象身份，可以嵌套；原生值记录也可以嵌入描述型对象。

定义输入与已发布元数据的区别见[成组类型发布](docs/runtime-model.md#已实现的成组类型发布)，引用存储、外部根与观察取得见[定型引用与外部根](docs/runtime-model.md#已实现的定型引用与外部根)。

## 当前契约与范围

- 每个 `Runtime` 显式拥有自己的类型和对象实例，单线程使用，不可复制或移动。值操作使用的外部存储及其生命周期由调用方管理。
- 类型先注册后使用，实例首次创建成功后关闭注册。支持原生与描述型定义成组发布，前向依赖和引用环不要求按依赖顺序提交；整组验证与布局计算成功后才公开，失败不会留下半注册类型。单条注册复用同一路径。
- `TypeId`、`FieldId`、`MethodId` 和 `ObjectId` 都限于所属运行时；限定类型名是声明名称，进程内标识不是存档 ID。
- `create` 交付已登记的 `ObjectRoot`，`ObjectId` 仍只标识身份。根复制增加独立登记，移动转移登记；释放根不立即销毁目标，也不延长 `Runtime` 自身的生命。显式销毁及运行时退出仍释放对象；不可达对象由宿主显式推进 GC 回收。
- 当前支持 float/u32/i32、i32/u32 底层枚举、含引用内联记录与单继承对象；原生成员方法支持标量/枚举的值、只读引用及可写引用参数；原生方法与源码入口还支持无 GC 引用的原生记录/Array/String 借用，源码入口的 String 参数限于只读。对象强引用序列见上方合同；记录按值调用、对象引用参数与结果、借用返回、通用值容器及记录方法尚未实现。
- 标量和记录支持调用方存储上的初始化、复制构造、赋值和销毁。含引用记录的复制保持共享目标，不克隆对象；原生记录执行 C++ 操作，描述型记录按字段执行。`TypeInfo` 分别提供复制构造和赋值能力，缺少能力时在操作开始前拒绝。
- `root_value` 用可移动的 `ValueRoot` 登记调用方的活值存储，不接管或延长存储寿命。登记撤销前禁止重建或销毁该存储；赋值仍可更新其中的引用。值接口检查类型、容量、对齐和重叠，调用方负责实际表示、构造状态和存续期。这仍是可信原生接口，不是任意字节的安全验证器，见[值操作入口](docs/runtime-model.md#已实现的值操作入口)。
- `Scalar` 是保存 float/u32/i32 或带类型身份枚举值的无堆分配值载体，不承担语言对象或通用动态值的存储。
- 通过名称查找成员后可缓存成员标识。类型/成员名称查询目前是线性扫描，定位于注册和发现阶段；缓存后的字段路径访问与方法调用不重新查名字，不分配堆内存（错误报告除外）。
- `read/write` 的终字段必须是标量，`read_reference/write_reference` 操作引用字段；字段路径只穿过内联记录，不隐式解引用对象。引用写入检查目标类型和形式，弱引用不能直接写入持有字段；`try_acquire` 对活的观察目标交付根句柄，空或失效目标返回空结果。已定型的失效引用仍可复制，旧身份不自动清空，也不绑定复用槽位的新对象。
- `visit_roots` 枚举外部根入口，`visit_references` 枚举一个对象或外部值的非空引用边，包括持有、观察和失效身份；它们不执行可达性收集。原生 `hidden_field` 不暴露公共字段标识，但参与布局、值能力与引用遍历。访问者执行期间拒绝 Runtime 修改操作和新增根，异常转换为调用错误。
- `begin_collection` 开始一轮非移动增量标记—清扫；宿主在同一运行时线程的安全点调用 `collect_step`，按根处理、标记、清扫分步推进。分配、普通值操作和撤根不隐式触发收集；尚无引擎 Tick 调度接入。
- 标记完成时统一提交死亡判定，`live_objects` 立即排除待回收对象，`pending_reclamation` 单独报告待析构数量。即使内存尚未释放，观察取得和对象访问也不能复活已判死目标；旧引用保留身份。
- 收集步支持工作量上限和可选的软时间上限，返回阶段、实际工作量、回收数量、待回收数量与耗时。单次原生析构不可中断；写入屏障与值操作不计入 GC 步预算，不承诺硬帧时间。具体计量与存活规则见[分步回收](docs/runtime-model.md#已实现的分步回收)。
- 方法参数或非 void 输出存储检查失败不会进入用户代码。成功值先在 `CallContext::result` 暂存，核对声明后才写 output；缺值、错类型、未声明结果或异常不覆盖调用方原值。已发生的业务修改不自动回滚。
- 初始化/复制构造失败会清理已构造部分，不留下半成品；赋值失败保证可销毁、基本不变量成立，允许部分更新，不自动回滚。原生操作必须遵守这些保证，方法异常转换为调用错误。回调中抛出的 `std::bad_alloc` 同样由这些异常边界转换；运行时自身存储、元数据、根登记和错误信息的分配失败可能继续向外传播分配异常，当前不提供完整的内存耗尽恢复保证。
- 原生绑定属于可信代码，必须提供正确的布局和生命周期操作，并声明全部对象引用，包括不公开的原生成员；析构不得抛出异常。原生引用槽通过 Runtime 的引用接口修改，整条原生记录复制走值操作；不能绕过这些入口任意覆盖引用存储。C++ 表示标记只验证原生字段兼容性，不是语言类型身份。
- 普通回调（包括原生字段定位器）期间禁止创建、销毁对象、注册类型或启动/推进 GC；允许访问有效字段、同步方法调用，以及合法的引用写入和根登记。`CallContext::storage` 只在本次同步调用期间有效，不能缓存；只读引用遍历另行禁止这些修改操作。
- 首个 JIT 业务切片通过定型 thunk 使用已有 `read/write/call`，不把 C++ `Scalar`、`Result` 或私有身份布局暴露给生成代码；尚未实现描述型字段的直接偏移访问或通用语言调用 ABI。

## 已验收场景

验收从公开接口执行，未引入单元测试框架：

- 注册、发现、创建两种来源的类型，读取和修改内联字段，执行生产进度方法。
- 拒绝重复定义、非法内联对象、错误参数、错误接收类型和跨运行时标识。
- 原生与描述型定义同批发布，前向内联布局保留原生对齐、复制能力和可执行字段/方法；已有元数据地址保持有效。
- 自引用、混合来源的持有/观察引用环及嵌套记录已可实例化；引用默认为空，创建结果交付时已登记外部根，含引用记录支持初始化、复制、赋值与销毁。
- 整组拒绝重名、未解析/跨运行时目标、非法值形式、原生表示冲突、内联布局环、布局溢出与无效方法；失败不发布名称或原生映射，修正后可重试。
- 显式销毁、重复销毁、槽位复用及过期句柄拒绝。
- 对象根的复制、移动和释放；观察取得成功时已受持有保护，显式销毁后取得失败；根句柄可以晚于 Runtime 安全释放，但不会使 Runtime 或其对象延长存活。
- 原生隐藏字段、嵌套记录与外部值共同参与引用遍历；字段不各自成为根，记录复制不克隆目标，失效引用复制及槽位复用保持原身份。
- 外部值根的存储保护、遍历期间的修改拒绝与异常恢复；撤销最后一个根不触发隐式销毁或 GC。
- 混合来源的 `外部根 → A ↔ B`：有根时保活且地址不变，撤根后分步回收整个环；死亡判定早于原生析构，观察取得失败，槽位复用不复活旧身份。
- 两次步进之间的隐藏/公开引用写入、新增及复制根、观察取得和创建；弱边不保活，清扫期间的新对象不被本轮清理。
- 外部值引用改写、晚登记值根、复制构造与原生部分赋值失败后的标记保护；未登记的外部值和本轮保守标记不成为永久根。
- 暂停中的对象销毁与槽位复用、值根撤销与登记项复用、排队值存储释放、各收集阶段的运行时退出、预算参数和收集重入拒绝。
- 嵌套原生记录构造失败后的回收与再次创建。
- 调用方存储上的标量、原生与描述型记录值操作，独立复制、嵌套构造失败的逆序清理、赋值失败后的部分更新与销毁。
- 值操作的能力预检、原址自赋值、非法存储及重叠拒绝；值回调遵守现有生命周期保护。
- 原生字段定位回调的生命周期保护；在定位回调内撤销当前值根后，立即停止内联/引用叶追踪，不恢复已取消的借用游标。
- 活跃值根的重复登记、移动、中间项撤销及历史槽位复用仍保护存储；重叠检查不扫描对象根或已撤销登记。
- 10,000 层混合内联记录的构造、复制、赋值、逆序失败清理、引用遍历、GC、显式销毁及 Runtime 退出；原生值操作和只读遍历的重入不破坏外层工作栈。
- 同质标量块初始化、复制和赋值保持值、存储范围与重叠约束；标量外形的自定义原生回调不会被批处理或平凡析构优化绕过。
- 调用期间的生命周期保护、原生异常报告与运行时最终清理。
- 原生/描述型标量返回、const 接收者元数据、共享签名解析、嵌套实参次序、第二次 ABI 回调漏写/错种类、结果不提交与显式清根；源码首次声明、宿主期望核验、接收者要求固定及各版本实际调用存储。开发/发布完整日志见 `out/review/function-contract-meeting/dev-acceptance-corrected.log` 和 `release-acceptance.log`。
- mimalloc 实际覆盖：C++/标准容器、对齐分配、DLL 初始化期及跨 DLL CRT 分配/扩容/释放、线程退出后的释放、分配失败的 C++ 语义。独立验收 DLL 不链接 mimalloc，用于确认 CRT 路径的实际重定向。

首阶段的历史目标与取舍见[第一步记录](docs/first-step.md)。当前源码引用局部与临时值根、兼容方法体更新和显式协程均已贯通，实际合同与已验收范围见[运行时协议](docs/runtime-model.md#已实现的最小业务执行协议)。下一增量按真实消费者需求选择，尚未实现的范围见[阶段边界](docs/research/runtime-stage-closeout.md#后续阶段边界)。

现行源码语法见 [LANGUAGE.md](LANGUAGE.md)，长期方向与待定问题见[业务语言设计契约](docs/language-contract.md)；待定方案不代表已实现能力。
