# C++ 自动反射：选型依据

核查日期：2026-09-30。本文记录为什么选择 C++ LibTooling 与预处理回调，而不是 ClangSharp 或自定义 attribute；当前接口、构建及能力范围见[自动反射](../reflection.md)。

**选型结论：** 使用锁定 SDK 的 C++ LibTooling。最终薄宏放在声明前，由预处理回调与 AST 配对；不能直接把类前的 `ACLASS()` 展开为 `clang::annotate`，因为该位置不符合类型属性语法。C# + ClangSharp 探针提供了对照，其包版本、释放函数缺陷和 clang-cl 参数适配成本没有进入生产链。

职责边界是“标注 → AST → 生成 `NativeRegistration<T>` → `TypeDefinition` → 现有 `Runtime::register_types`”。对象身份、GC、调用 ABI 与运行时类型表保持单一来源。

## 工具路线

| 路线 | 官方依据与本项目判断 |
| --- | --- |
| C# + `ClangSharp.Interop`，限 libclang C API | 评估过，未采用。当前托管包与 GitHub 最新发行均为 **21.1.8.4**（2026-07-15），目标 **net10.0**；Interop 仍依赖 `libClang >= 21.1.8`、`libClangSharp >= 21.1.8.2`。单独引用 Interop 不会自动消除 shim 包依赖。[NuGet](https://www.nuget.org/packages/ClangSharp.Interop/21.1.8.4)、[发行](https://github.com/dotnet/ClangSharp/releases/tag/v21.1.8.4) |
| C# + 高层 `ClangSharp` AST | 提供更完整的 C++ AST 包装，但需要配套 `libClangSharp`。当前原生包最新分别是 **libclang 22.1.8**、**libClangSharp 22.1.8.2**，并非本项目的 23.1.1；不能用“最新包”代替版本核对。[分层说明](https://github.com/dotnet/ClangSharp/blob/v21.1.8.4/docs/using-clangsharp-as-a-library.md)、[libclang](https://www.nuget.org/packages/libclang/)、[libClangSharp](https://www.nuget.org/packages/libClangSharp/) |
| C++ LibTooling | 完整 AST、现成 `CompilationDatabase`/`ClangTool`，可与锁定 LLVM 同源构建；本机 SDK 已包含所需开发库，无须引入另一份 LLVM。代价是链接体积及 C++ AST API 升级适配，官方不承诺该接口稳定。本机准确消费现有 clang-cl 编译命令的对照通过，因此当前优先于额外维护 C API 参数转换。[接口比较](https://clang.llvm.org/docs/Tooling.html)、[LibTooling](https://clang.llvm.org/docs/LibTooling.html) |

**Interop 避开 shim 的边界：** `clang.*` 导入 `libclang`；`CXTranslationUnit.Parse/TryParseFullArgv`、`CXCompilationDatabase.FromDirectory/GetCompileCommands`、`CXString.ToString/Dispose` 都走 C API。但 `CXCursor` 的 `NumBases`、`NumAttrs`、`DeclKind` 等便捷属性调用 `clangsharp.*`。应限制调用面，不能把整个 Interop 当作纯 C 包装。libclang 强调向后兼容，但未找到 **Interop 21.1.8.4 + libclang 23.1.1** 的官方配套保证。[导入源码](https://github.com/dotnet/ClangSharp/blob/v21.1.8.4/sources/ClangSharp.Interop/clang/clang.cs)、[Cursor 源码](https://github.com/dotnet/ClangSharp/blob/v21.1.8.4/sources/ClangSharp.Interop/Extensions/CXCursor.cs)、[libclang 边界](https://clang.llvm.org/docs/LibClang.html)

`clang` 静态构造会安装 DLL resolver；官方 `clang.ResolveLibrary` 事件先于默认解析，可用于强制加载锁定 SDK。自行设置 resolver 时，应先通过 `AppContext.SetData("ClangSharp.Interop.DisableResolveLibraryHook", true)` 禁用默认钩子，避免重复注册。该静态构造没有 shim 版本检查。[加载源码](https://github.com/dotnet/ClangSharp/blob/v21.1.8.4/sources/ClangSharp.Interop/clang.cs)、[配置源码](https://github.com/dotnet/ClangSharp/blob/v21.1.8.4/sources/ClangSharp.Interop/Configuration.cs)

## 标注与提取边界

C API 路线评估的标注语法如下：

```cpp
struct [[clang::annotate("atelier.object")]] Counter {
    [[clang::annotate("atelier.field")]] float value;
};
```

Clang 正式支持 `clang::annotate`；未知 attribute 默认警告并忽略，不能假定 `[[atelier::object]]` 会保留为 AST 属性。自定义 namespace 若必须成为正式语法，需要插件注册 `ParsedAttrInfo` 或额外词法解析与声明关联；这有独立维护成本。[Attribute](https://clang.llvm.org/docs/AttributeReference.html#annotate)、[语言扩展](https://clang.llvm.org/docs/LanguageExtensions.html#specifying-an-attribute-for-multiple-declarations-pragma-clang-attribute)、[诊断](https://clang.llvm.org/docs/DiagnosticsReference.html#wunknown-attributes)、[插件](https://clang.llvm.org/docs/ClangPlugins.html#defining-attributes)

| 所需信息 | C API 能力及限制 |
| --- | --- |
| 标注内容 | 遍历 `CXCursor_AnnotateAttr` 后 `clang_getCursorSpelling` 返回 annotation 字符串。不要据此假设可读取所有可选常量参数；应使用明确的单字符串标注。[实现](https://github.com/llvm/llvm-project/blob/main/clang/tools/libclang/CIndex.cpp) |
| 基类、字段、方法 | Cursor 遍历提供声明和 `CXXBaseSpecifier`；类型、参数/返回值、访问权限、const/static/virtual 信息有 C API。重载要按声明身份与完整签名区分。可据此生成受支持签名；不支持项必须定位报错。private 元数据可读，不等于生成代码具有访问权。[类型 API](https://clang.llvm.org/doxygen/group__CINDEX__TYPES.html)、[C++ API](https://clang.llvm.org/doxygen/group__CINDEX__CPP.html) |
| object/record、strong/weak/sequence | 这些属于引擎契约，AST 不会自行推断；由显式标注及已注册类型校验提供。生成成员指针、基类转换及既有注册调用，让 C++ 编译器处理布局、访问权限和构析约束，不生成硬编码偏移。 |
| 源位置、诊断、编译参数 | 有 cursor/diagnostic 位置及 severity API；编译数据库含工作目录、完整参数和每个配置的命令。须选择真实 TU/配置并沿用宏、include、target、资源目录与相关环境，不能仅解析孤立头文件。`TryParseFullArgv` 保留 argv[0] 的入口可用。[C API](https://clang.llvm.org/doxygen/group__CINDEX.html)、[数据库格式](https://clang.llvm.org/docs/JSONCompilationDatabase.html)、[TU 包装](https://github.com/dotnet/ClangSharp/blob/v21.1.8.4/sources/ClangSharp.Interop/Extensions/CXTranslationUnit.cs)、[数据库包装](https://github.com/dotnet/ClangSharp/blob/v21.1.8.4/sources/ClangSharp.Interop/Extensions/CXCompilationDatabase.cs) |

## 本机验证结果

可行性探针使用 .NET SDK 10.0.204、Interop 21.1.8.4，并通过官方 resolver 事件明确加载本项目 SDK 的 libclang 23.1.1；进程模块列表确认只加载该 `libclang.dll`，未加载 libClangSharp。

| 项目 | 实际结果 |
| --- | --- |
| 属性保留 | `[[clang::annotate("...")]]` 及包裹它的宏均保留；前向声明与定义、单行多个字段也可见。宏属性的源位置需用 expansion location，不能仅用 `Location_isFromMainFile` 过滤属性，否则会漏项。 |
| C API 数据 | 取得 21 条 annotation、10 个字段、1 个基类、5 个方法及析构声明；包含 private、const/static、noexcept、规范化参数类型、重载和行列。dev/release 经适配后的结果一致。只证明数据可提取，尚未实现生成器的语义拒绝规则。 |
| 自定义 namespace | `[[atelier::object("...")]]` 在现有 `/WX` 下报 `unknown attribute ... ignored`，退出 1；不能靠关闭警告得到完整 AST 属性。 |
| ClangSharp 释放 | 此版本 `CXCompileCommands.Dispose()` 错调 `CompilationDatabase_dispose`，本机复现 0xC0000005，且[对应版本源码](https://github.com/dotnet/ClangSharp/blob/v21.1.8.4/sources/ClangSharp.Interop/Extensions/CXCompileCommands.cs)相符。探针改为复制所需数据后显式调用正确的 `clang.CompileCommands_dispose`。这不是 libclang 23 不兼容的证明。 |
| libclang 与真实命令 | 从 `compile_commands.json` 读取完整 argv、工作目录，移除源文件与输出动作后直接 `TryParseFullArgv`：libclang 注入的 `-fno-spell-checking`、`-fallow-editor-placeholders` 在 cl 驱动模式下触发参数错误，原命令的 `-clang:-std=c++23` 也未得到期望语言模式。实验增加 `-Xclang -std=c++23` 并抑制 unknown-argument 后可以提取；这只是定位对照，不能作为生产工具的参数保持方案。 |
| C++ tooling 对照 | 同一 SDK 的 `clang-check` 使用同源 TU 编译数据库，沿用 dev/release 的 include、宏、标准与警告参数，两配置均成功提取属性和声明，退出 0。只更换输入为探针 TU，未另配一组编译环境。 |

## 决定

用户确认 `ACLASS/ASTRUCT/AFIELD/AFUNCTION` 与 `.gen.hpp` 的写法，生成器用 Clang `PPCallbacks::MacroExpands` 记录标记，再由 AST 确认归属和类型。锁定 SDK 已提供此回调；普通编译不需要插件或未知 attribute。[官方接口](https://clang.llvm.org/doxygen/classclang_1_1PPCallbacks.html)

生成头保留轻量特化声明，生成实现构建 `NativeRegistration<T>`。私有成员通过 `AGENERATED_BODY(Type)` 授权，模块显式成组发布。
