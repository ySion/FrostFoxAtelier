# V8 cppgc / Oilpan：对象引用与根的参考

核查日期：2026-09-21。只使用 V8/Chromium 一手文档与源码。项目约束为自有静态语言、共享引擎对象体系、**nonmoving**；本文不把 Oilpan 当作直接可嵌入的运行时。

## 已核实的事实

- Oilpan（cppgc）是可独立使用或与 V8 JavaScript GC 协作的 C++ trace GC；实现 mark-sweep，且只对选定 space 做**非增量、非并发**压缩。故它不是“默认永不移动”的通用承诺。[V8 cppgc README](https://chromium.googlesource.com/v8/v8.git/%2B/HEAD/include/cppgc/)
- cppgc 的堆边必须可见：`Member<T>` 是强边，字段必须在对象 `Trace()` 中访问；`WeakMember<T>` 不保活，目标不可达时自动置空；`UntracedMember` 明确警告不得作为常规堆对象引用。[`member.h`](https://chromium.googlesource.com/v8/node/%2B/bec32ce903083e2d861a2170404eaaa0afd0d582/deps/v8/include/cppgc/member.h)
- `Persistent<T>`/`WeakPersistent<T>` 是堆外根的强/弱版本；源码另有 cross-thread persistent region，说明跨线程根不能被当作普通字段边。[persistent policy](https://chromium.googlesource.com/v8/v8/%2B/refs/heads/main/include/cppgc/internal/pointer-policies.h)
- 精确性来自 trace 契约：`TraceDescriptor` 包含基对象地址和 `Trace()` 回调，访问器据此遍历所有相关字段。[`trace-trait.h`](https://chromium.googlesource.com/v8/node/%2B/bec32ce903083e2d861a2170404eaaa0afd0d582/deps/v8/include/cppgc/trace-trait.h)
- Oilpan 的标记顺序是根、从根经 `Trace()` 的传递闭包、弱边清除/弱回调。增量/并发时根标记和最终确认各有短暂停顿，中间传递扫描与 mutator 交错；因此所有非栈边都必须使用受管理 handle，裸堆指针会造成 UAF。[V8 cppgc README：marking](https://chromium.googlesource.com/v8/v8.git/%2B/HEAD/include/cppgc/)
- Oilpan 宣称堆内布局精确、native 栈保守；但嵌入器可在事件循环末端（无栈引用）触发 precise GC。它的原生对象析构顺序和时机不保证；析构函数不应访问其他堆对象，跨对象收尾须在 pre-finalizer 或显式生命周期完成。[V8 cppgc README：precision/sweeping](https://chromium.googlesource.com/v8/v8.git/%2B/HEAD/include/cppgc/)
- “Unified Heap”在此指 cppgc 与 V8 JS heap 可连接、协同调度/标记；V8 通过 `AttachCppHeap` 连接 CppHeap，并有 cross-heap remembered set 实现。它**不**直接证明 C++ 与 JS 共享同一类型定义、对象 ID、布局或析构契约。[V8 attach source](https://chromium.googlesource.com/v8/v8/%2B/850b225033671d7cac735d9334a62c8482b2a793/src/heap/heap.cc)；[cppgc-js 源码目录](https://github.com/v8/v8/tree/main/src/heap/cppgc-js)
- Oilpan 是 thread-local heap 模型；对象通常由分配线程访问/回收，跨线程引用建模为 cross-thread roots。这和游戏引擎多线程对象访问的契约不同，不能省略同步设计。[V8 cppgc README：threading](https://chromium.googlesource.com/v8/v8.git/%2B/HEAD/include/cppgc/)

## 对下一轮最小闭环的适用与不适用

- **适用：**以 `Trace(Visitor&)` 或等价的类型元数据形成唯一精确边入口；把强字段、弱字段和堆外根做成不同引用类型；根注册/释放走单一 RootSet API。该模型直接服务“全部业务对象共用类型、身份、分配销毁”的目标。
- **适用：**借鉴弱处理、析构与内存回收的阶段分工；业务关系不能依赖析构顺序。本项目已约定失效引用保留旧身份，因此不照搬 `WeakMember` 自动置空的行为；pre-finalizer 是否引入仍待真实需求决定。
- **暂不引入：**保守栈扫描、cross-thread root 和并发 collector；当前闭环由宿主精确报告根，未来 VM 活动槽位接入同一规则。跨业务片段的增量标记仍需要写屏障或等效保护，这属于当前闭环，不能以单线程为理由省略。
- **明确排除：**项目 nonmoving，不采用 Oilpan 的 compaction 路径，也不因 V8 的 Unified Heap 引入第二套脚本对象类型或生命周期体系。
