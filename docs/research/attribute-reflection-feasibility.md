# Attribute 自动反射：首片方案与可行性

状态：2026-09-30 已完成官方资料与有限本机探针，尚未实施生产生成器或迁移业务类型。Core 基线为 `b272c3f`，工作分支为 `codex/attribute-reflection`。只读核对的真实消费者为 World 提交 `72859e7d`；应用集成候选 `fc9ee7f` 不由本任务修改。

**当前建议：** 标注使用 `clang::annotate`，可包一层薄宏；首片用锁定 SDK 的 C++ LibTooling 提取并生成 C++ 注册代码。C# + ClangSharp 的受限 C API 探针可以取到所需数据，但其包版本、一个释放函数缺陷和 clang-cl 参数适配仍需处理，暂不作为已验证的生产路线。若用户优先选择 C#，先关闭这些接入问题再定型；不同时建设两套生成器。

职责边界是“标注 → AST → 生成 `NativeRegistration<T>` → `TypeDefinition` → 现有 `Runtime::register_types`”。C# 若采用，仅为构建工具；对象身份、GC、调用 ABI 与运行时类型表保持现有单一来源。首片不涉及撤销、RHI/UI 重构或新构建系统。

## 工具路线

| 路线 | 官方依据与本项目判断 |
| --- | --- |
| C# + `ClangSharp.Interop`，限 libclang C API | 优先验证。当前托管包与 GitHub 最新发行均为 **21.1.8.4**（2026-07-15），目标 **net10.0**；Interop 仍依赖 `libClang >= 21.1.8`、`libClangSharp >= 21.1.8.2`。单独引用 Interop 不会自动消除 shim 包依赖。[NuGet](https://www.nuget.org/packages/ClangSharp.Interop/21.1.8.4)、[发行](https://github.com/dotnet/ClangSharp/releases/tag/v21.1.8.4) |
| C# + 高层 `ClangSharp` AST | 提供更完整的 C++ AST 包装，但需要配套 `libClangSharp`。当前原生包最新分别是 **libclang 22.1.8**、**libClangSharp 22.1.8.2**，并非本项目的 23.1.1；不能用“最新包”代替版本核对。[分层说明](https://github.com/dotnet/ClangSharp/blob/v21.1.8.4/docs/using-clangsharp-as-a-library.md)、[libclang](https://www.nuget.org/packages/libclang/)、[libClangSharp](https://www.nuget.org/packages/libClangSharp/) |
| C++ LibTooling | 完整 AST、现成 `CompilationDatabase`/`ClangTool`，可与锁定 LLVM 同源构建；本机 SDK 已包含所需开发库，无须引入另一份 LLVM。代价是链接体积及 C++ AST API 升级适配，官方不承诺该接口稳定。本机准确消费现有 clang-cl 编译命令的对照通过，因此当前优先于额外维护 C API 参数转换。[接口比较](https://clang.llvm.org/docs/Tooling.html)、[LibTooling](https://clang.llvm.org/docs/LibTooling.html) |

**Interop 避开 shim 的边界：** `clang.*` 导入 `libclang`；`CXTranslationUnit.Parse/TryParseFullArgv`、`CXCompilationDatabase.FromDirectory/GetCompileCommands`、`CXString.ToString/Dispose` 都走 C API。但 `CXCursor` 的 `NumBases`、`NumAttrs`、`DeclKind` 等便捷属性调用 `clangsharp.*`。应限制调用面，不能把整个 Interop 当作纯 C 包装。libclang 强调向后兼容，但未找到 **Interop 21.1.8.4 + libclang 23.1.1** 的官方配套保证。[导入源码](https://github.com/dotnet/ClangSharp/blob/v21.1.8.4/sources/ClangSharp.Interop/clang/clang.cs)、[Cursor 源码](https://github.com/dotnet/ClangSharp/blob/v21.1.8.4/sources/ClangSharp.Interop/Extensions/CXCursor.cs)、[libclang 边界](https://clang.llvm.org/docs/LibClang.html)

`clang` 静态构造会安装 DLL resolver；官方 `clang.ResolveLibrary` 事件先于默认解析，可用于强制加载锁定 SDK。自行设置 resolver 时，应先通过 `AppContext.SetData("ClangSharp.Interop.DisableResolveLibraryHook", true)` 禁用默认钩子，避免重复注册。该静态构造没有 shim 版本检查。[加载源码](https://github.com/dotnet/ClangSharp/blob/v21.1.8.4/sources/ClangSharp.Interop/clang.cs)、[配置源码](https://github.com/dotnet/ClangSharp/blob/v21.1.8.4/sources/ClangSharp.Interop/Configuration.cs)

## 标注与提取边界

推荐先验证下述示意语法，也可由薄宏封装；它尚不是项目定稿：

```cpp
struct [[clang::annotate("atelier.object")]] Counter {
    [[clang::annotate("atelier.field")]] float value;
};
```

Clang 正式支持 `clang::annotate`；未知 attribute 默认警告并忽略，不能假定 `[[atelier::object]]` 会保留为 AST 属性。自定义 namespace 若必须成为正式语法，需要插件注册 `ParsedAttrInfo` 或额外词法解析与声明关联；这有独立维护成本。[Attribute](https://clang.llvm.org/docs/AttributeReference.html#annotate)、[语言扩展](https://clang.llvm.org/docs/LanguageExtensions.html#specifying-an-attribute-for-multiple-declarations-pragma-clang-attribute)、[诊断](https://clang.llvm.org/docs/DiagnosticsReference.html#wunknown-attributes)、[插件](https://clang.llvm.org/docs/ClangPlugins.html#defining-attributes)

| 首片所需信息 | C API 能力及限制 |
| --- | --- |
| 标注内容 | 遍历 `CXCursor_AnnotateAttr` 后 `clang_getCursorSpelling` 返回 annotation 字符串。不要据此假设可读取所有可选常量参数；首片宜使用明确的单字符串标注。[实现](https://github.com/llvm/llvm-project/blob/main/clang/tools/libclang/CIndex.cpp) |
| 基类、字段、方法 | Cursor 遍历提供声明和 `CXXBaseSpecifier`；类型、参数/返回值、访问权限、const/static/virtual 信息有 C API。重载要按声明身份与完整签名区分。首片可据此生成受支持签名；不支持项必须定位报错。private 元数据可读，不等于生成代码具有访问权。[类型 API](https://clang.llvm.org/doxygen/group__CINDEX__TYPES.html)、[C++ API](https://clang.llvm.org/doxygen/group__CINDEX__CPP.html) |
| object/record、strong/weak/sequence | 这些属于引擎契约，AST 不会自行推断；由显式标注及已注册类型校验提供。生成成员指针、基类转换及既有注册调用，让 C++ 编译器处理布局、访问权限和构析约束，不生成硬编码偏移。 |
| 源位置、诊断、编译参数 | 有 cursor/diagnostic 位置及 severity API；编译数据库含工作目录、完整参数和每个配置的命令。须选择真实 TU/配置并沿用宏、include、target、资源目录与相关环境，不能仅解析孤立头文件。`TryParseFullArgv` 保留 argv[0] 的入口可用。[C API](https://clang.llvm.org/doxygen/group__CINDEX.html)、[数据库格式](https://clang.llvm.org/docs/JSONCompilationDatabase.html)、[TU 包装](https://github.com/dotnet/ClangSharp/blob/v21.1.8.4/sources/ClangSharp.Interop/Extensions/CXTranslationUnit.cs)、[数据库包装](https://github.com/dotnet/ClangSharp/blob/v21.1.8.4/sources/ClangSharp.Interop/Extensions/CXCompilationDatabase.cs) |

## 本机验证结果

探针位于本机 `out/reflection-probe/`，`verification.json` 记录配置、退出码和源码哈希。它是可丢弃的可行性实验，不是生产目标，也没有改动锁文件或自动加载到普通构建。使用 .NET SDK 10.0.204、Interop 21.1.8.4，并通过官方 resolver 事件明确加载本项目 SDK 的 libclang 23.1.1；进程模块列表确认只加载该 `libclang.dll`，未加载 libClangSharp。

| 项目 | 实际结果 |
| --- | --- |
| 属性保留 | `[[clang::annotate("...")]]` 及包裹它的宏均保留；前向声明与定义、单行多个字段也可见。宏属性的源位置需用 expansion location，不能仅用 `Location_isFromMainFile` 过滤属性，否则会漏项。 |
| C API 数据 | 取得 21 条 annotation、10 个字段、1 个基类、5 个方法及析构声明；包含 private、const/static、noexcept、规范化参数类型、重载和行列。dev/release 经适配后的结果一致。只证明数据可提取，尚未实现生成器的语义拒绝规则。 |
| 自定义 namespace | `[[atelier::object("...")]]` 在现有 `/WX` 下报 `unknown attribute ... ignored`，退出 1；不能靠关闭警告得到完整 AST 属性。 |
| ClangSharp 释放 | 此版本 `CXCompileCommands.Dispose()` 错调 `CompilationDatabase_dispose`，本机复现 0xC0000005，且[对应版本源码](https://github.com/dotnet/ClangSharp/blob/v21.1.8.4/sources/ClangSharp.Interop/Extensions/CXCompileCommands.cs)相符。探针改为复制所需数据后显式调用正确的 `clang.CompileCommands_dispose`。这不是 libclang 23 不兼容的证明。 |
| libclang 与真实命令 | 从 `compile_commands.json` 读取完整 argv、工作目录，移除源文件与输出动作后直接 `TryParseFullArgv`：libclang 注入的 `-fno-spell-checking`、`-fallow-editor-placeholders` 在 cl 驱动模式下触发参数错误，原命令的 `-clang:-std=c++23` 也未得到期望语言模式。实验增加 `-Xclang -std=c++23` 并抑制 unknown-argument 后可以提取；这只是定位对照，不能作为生产工具的参数保持方案。 |
| C++ tooling 对照 | 同一 SDK 的 `clang-check` 使用同源 TU 编译数据库，沿用 dev/release 的 include、宏、标准与警告参数，两配置均成功提取属性和声明，退出 0。只更换输入为探针 TU，未另配一组编译环境。 |

尚未验证：完整生成器链接、生成代码到 Runtime 的端到端行为、CMake 增量/重现性，以及真实 World 类型迁移。不得把上表升级为这些项目已经完成。

## 首片实施方案

### 标注与唯一生产链

下面是待用户收口的写法示意，底层 `annotate` 已验证，宏名称与载荷语法尚未定稿：

```cpp
struct ATELIER_OBJECT("world.Component") Component {
    // 授权生成的元数据生产者取成员指针；不改变业务访问权限。
    friend struct atelier::reflection::Definition<Component>;
private:
    ATELIER_STRONG("world.Actor", hidden) ReferenceSlot owner_;
};
struct ATELIER_OBJECT("world.Counter") Counter : Component {
    ATELIER_FIELD float value{};
    ATELIER_METHOD float advance(float delta);
};
```

- 只显式标注需要登记的类型与成员；object/record、引用强弱、序列元素类型及隐藏性由标注明确。字段名字默认沿用 C++ 声明，需要保持现有 Runtime 名字时显式指定，例如 `owner_` 对应 `owner`。
- 基类从 AST 取得并验证为现有正式单继承可表达的关系，生成 `.base<Base>()`。字段生成实际成员指针，构析和调整地址仍由 `NativeRegistration` 与 C++ 编译器负责，不计算或硬编码偏移。
- 私有引用字段通过窄范围 friend 生产者处理；不使用 `#define private public`、不公开字段，也不生成绕开 Runtime 写屏障的业务访问器。已有 `world::detail::Access` 的业务访问职责保留。
- 对反射类型中缺少标注的 `ReferenceSlot/ReferenceSequence` 报错；已反射且含引用的内联记录也不能无声漏出追踪定义。普通未标注的 bool、原生指针、ObjectId 等保持原生业务状态，不自动认作 GC 边。
- 首片支持现有标量、已登记记录、对象单继承、strong/weak/sequence，以及当前 `void/float/uint32_t` 成员方法签名。静态方法、模板、变参、位域、不可写字段、不可构析类型和未覆盖的重载形式给出源位置错误；不静默跳过。声明收集须合并前向声明与定义，避免重复注册。

### 构建接线与首个真实消费者

1. 在 `tools/` 增加一个生成器目标，复用锁定 LLVM。CMake 为目标选定真实 TU 与配置，使用原有编译命令、宏/include/target 和 Windows SDK 环境。先保证准确提取，再做过滤；不以孤立头文件的默认参数替代生产编译。
2. 生成器输出 C++ 定义和头文件依赖清单。生成内容不含时间戳、绝对路径或 AST 地址；声明顺序固定，内容不变不重写文件。CMake 使用 `OUTPUT/BYPRODUCTS/DEPFILE` 接线，包含文件或参数变化会触发重生成；首次构建不能依赖尚未生成的注册文件才能解析类型。
3. 首个消费者为 World 的 `Vec3/Quaternion`、`Actor/Component`、`Transform/Cube/Counter` 七种现有类型。World 负责人负责业务文件迁移；本任务提供工具和接线。唯一 `world::register_types` 继续成组发布，生成定义替换相应手写定义，随后建立 `Types` 与字段路径。生成顺序变化不能改变手写下标对应，优先按原生类型身份取回发布结果。
4. 先用一个真实注册消费者贯通 private 引用、基类、方法及析构；迁移七种类型时运行现有 dev/release World 端到端入口，覆盖注册、引用图、GC、成员调用与生命周期。增加必要失败输入验收，确认非法标注带文件/行列并让构建失败；不建立单元或 mock 框架。

**交付终点：** 同一组标注稳定生成同一注册源码；真实消费者通过现有唯一入口发布，行为与手写版本一致；变更头文件会重生成，无变化构建不重写；不存在并存的旧手写描述。产出工具、CMake 接线、迁移后的真实消费者及当前文档。编译器插件、自定义 C++ 属性扩展、通用编辑器元数据和全仓迁移均不在首片。

**待用户收口：** 薄宏或直接 `clang::annotate` 的写法偏好；是否优先要求 C# 承载生成工具（若要求，则先处理上文实际接入问题）。这两点在正式工具实现前确定；现有本机验证不依赖该选择。
