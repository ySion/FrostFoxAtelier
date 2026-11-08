# JSON 核心

[json/json.hpp](../modules/core/json/include/json/json.hpp) 提供拥有型 `Document`、短期可变 `Builder` 和借用型 `Value`。底层为依赖锁固定的 yyjson；输入语法、编码与类型错误通过 `json::Result` 返回，不经异常桥。

## 所有权与验证

Document 不可变，复制共享其存储；Builder 复制外部子树，`finish()` 生成独立不可变快照。Value 借用稳定的 DOM 节点与根，不借用父 Value 包装对象；移动拥有者不使节点失效，销毁拥有者或替换子树后必须重新获取相关 Value。错误分支定位完整 JSON Pointer，成功导航不构造路径。

parse 的大小及深度政策由入口负责。解析检查 UTF-8、重名键及指定深度，schema 解码不重复这些检查。Builder 的普通字符串入口验证一次；`set_validated` 的键与文本都必须已是完整有效 UTF-8，调用方承担前提，两个配置均不重扫，也不承诺发现任意伪造字节。

parse、校验和错误定位不受嵌套深度的调用栈限制；复制、冻结、比较和写出仍是递归的，深层值的上限由各入口的深度政策负责。

`Document::parse_gltf` 是唯一的 glTF 重名键政策入口，唯一产品调用方为 glTF 导入。它允许 BOM，校验 JSON/UTF-8，以解码后的键比较并保留最后一个成员；不增加深度上限。所有 glTF 输入都用公开的迭代复制入口转入 yyjson mutable 存储，迭代去重后作为只读 Document 发布，析构用 `yyjson_mut_doc_free`；不经过冻结、文本桥接、比较或 writer。存储布局与写权限分开，Value 大小不变；Builder 的写入口只接受自身可写 DOM 的节点，不能改写只读 Document 或其他 Builder。普通 parse 仍拒绝重名键。

writer 返回字节缓冲，按成员键的字节序排序引用，不复制整树。文件或管道直接消费字节；只有真正需要拥有型 String 的消费者才转换。

数组按输入顺序遍历与追加；`ArrayView` 不分配节点集合。i64/u64 读取检查整数种类与范围，不经过 double；浮点到整数不隐式截断。f32 读取允许舍入但拒绝溢出。float 构造使用最短十进制值转成 double；若读回 float 不同则使用原 float 的精确 double 值，保留负零，始终存为 real。double 使用 yyjson 原生写出，不降精度为 float。

文件写出使用两空格缩进与 LF（调用方选择末尾 LF）；compact 用于协议和摘要。字节格式不承诺与旧 nlohmann writer 一致。持久摘要和 Repro 的版本合同见[复现工程](modules/editor/control.md#复现工程)。Scene v10、Material v2 本身不升版；严格 UTF-8 DOM 在 parse 准入时拒绝坏字节，因此属性解码只处理合法文本的类型错误，不再生成坏 UTF-8 的 PropertyReport。

## 启动表字面量

`Document::literal<固定文本>()` 只接受编译期模板字面量，无运行时字符串重载。UTF-8 在编译期用 static_assert 校验；首次调用用函数局部 static 线程安全地严格解析，失败为 contract-fatal，诊断包含字面量内容与语法错误 offset；无字节 offset 的结构错误注明不可用并给出结构路径。

返回值是静态不可变 Document 的 const 引用，需要拥有句柄的启动表显式复制它。所有产品调用必须位于启动即构造的 keymap/menu_bindings 等表中，不能延迟到首次点击才暴露拼写错误；动态参数与按需构造的值使用 Builder。不用字面量接口模拟 initializer-list，也不做 DOM→文本→DOM 适配。

与 [String::literal](foundation.md) 一样，字面量所在原生模块必须保持加载，直到全部引用和拥有型副本结束；不向可卸载模块或 JIT 常量池借存储。普通 Document 的共享并不放宽模块代码和静态对象的存续要求。
