# JSON 库选型研究

核查日期：2026-10-03。本文记录候选和选择依据；当前工程仍使用 nlohmann/json 3.12.0，本轮没有替换依赖或执行候选基准。

## 结论

建议先测 **yyjson**，用 **simdjson DOM** 做性能对照。当前编辑器需要完整校验场景并写回文件，运行时类型与对象关系仍由引擎管理。这个优先级是结合当前用途的判断，并非同机比较的结果。

| 候选 | 适合的用途 | 本项目需要注意 |
| --- | --- | --- |
| yyjson | 紧凑的 DOM 读写、显式内存控制 | 保留重复键，需要应用拒绝；C API 需要明确所有权 |
| simdjson | 完整 DOM 解析或按需读取大量数据 | On-Demand 与完整验证的工作量不同；当前也有写入 Builder |
| Glaze | 稳定 C++ 结构直接读写 | 严格整文档检查需要配置；不为它另建权威类型体系 |
| Boost.JSON | C++ DOM、流式解析、分配器控制 | 可作成熟备选；本项目收益仍需实测 |

## yyjson：首测候选

当前官方 Latest 为 **0.13.0**，MIT 许可，ANSI C 实现；输入与值使用连续存储，读出的 DOM 与可变 DOM 分开。[发布说明](https://github.com/ibireme/yyjson/releases/tag/0.13.0)、[许可证](https://github.com/ibireme/yyjson/blob/0.13.0/LICENSE)、[数据结构](https://ibireme.github.io/yyjson/doc/doxygen/html/data-structures.html)

默认检查 UTF-8、JSON 数字语法，拒绝注释、尾逗号和 Inf/NaN。整数使用 `uint64_t`／`int64_t`，越界整数会转为 `double`；浮点按正确舍入转换。因此 ID 仍须检查整数类型和范围。重复键会保留；不能直接按键取值后认为重复字段已被拒绝。[API](https://ibireme.github.io/yyjson/doc/doxygen/html/api.html)、[限制](https://github.com/ibireme/yyjson#limitations)

文档释放后，其值与字符串引用失效；原地解析还要求输入缓冲区可修改、保留至文档销毁并提供尾部 padding。写入的借用字符串也有寿命要求。接入时宜使用 RAII 与明确的字符串复制接口，并顺序遍历字段，避免反复线性查找。支持自定义分配器；0.13.0 可配置读写深度上限，默认无限。[API](https://ibireme.github.io/yyjson/doc/doxygen/html/api.html)、[构建选项](https://ibireme.github.io/yyjson/doc/doxygen/html/building-and-testing.html)

正确性证据包括 JSONTestSuite 测试、OSS-Fuzz 的地址／未定义行为／内存检测配置；这些证据不意味着没有缺陷。官方 CI 包含 Windows MSVC、MinGW 和其他平台 Clang，本轮未验证本项目 Windows Clang 工具链组合。[测试源码](https://github.com/ibireme/yyjson/blob/0.13.0/test/test_json_reader.c)、[OSS-Fuzz 配置](https://github.com/google/oss-fuzz/blob/master/projects/yyjson/project.yaml)、[编译器 CI](https://github.com/ibireme/yyjson/blob/0.13.0/.github/workflows/compilers.yml)

## 其他候选

**simdjson** 的 DOM 会完整解析、验证并物化；On-Demand 只处理所访问的值及相关结构，前向访问规则影响调用方式。因此只取几个字段的成绩不能代表完整场景验证。当前已有 `string_builder`，不能再说它不能写 JSON；输出含不可靠 UTF-8 时须显式调用 `validate_unicode()`。[DOM](https://simdjson.github.io/simdjson/md_doc_2dom.html)、[On-Demand](https://github.com/simdjson/simdjson/blob/master/doc/ondemand_design.md)、[Builder](https://github.com/simdjson/simdjson/blob/master/doc/builder.md)

**Glaze** 当前读取默认验证 UTF-8，但完整严格验证还需启用 `validate_skipped`、`validate_trailing_whitespace`，或调用 `validate_json`；写入不检查 UTF-8。`string_view` 借用原缓冲区且不反转义。它适合稳定 C++ 结构，但不足以替代引擎自己的类型、身份与关系校验。[符合性](https://stephenberry.github.io/glaze/json/#json-conformance)、[选项](https://stephenberry.github.io/glaze/options/)、[借用字符串](https://stephenberry.github.io/glaze/optimizing-performance/)

**Boost.JSON** 支持 DOM 读写、流式解析及分配器，默认严格 JSON。官方提供 CI、模糊测试及历史审计证据；历史审计不代表当前版本没有问题。[项目与质量保证](https://github.com/boostorg/json)、[解析契约](https://www.boost.org/latest/libs/json/doc/html/ref/parse.html)

## 接入边界与性能判断

无论选择哪个库，都保留[工程格式](projects.md)中的重复／未知字段、整数 ID、数组长度、文件大小、嵌套深度和对象图校验，以及保存失败保护原文件的机制。语法库不负责 World 重建和 Runtime 身份。

官方 GB/s 成绩与本项目先前的 75–100 MB/s 不具备直接比较条件。后续比较应使用同一批场景、编译选项和完整校验要求，分别记录读取、解析、业务校验、World 重建、写入及内存；本轮尚无候选实测收益结论。
