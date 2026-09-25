# 下一 session：布局核心实施交接

状态：Main已选中该Module，研究/对抗/真实候选路径已完成；**尚未创建生产布局库**。本文件是下一次获得实现授权后的执行入口，本轮只交文档，已验证probe保持不变。

## 1. 先读取哪些权威

按顺序读 [README](README.md) → [contract](contract.md) → [decisions](decisions.md) → [acceptance](acceptance.md)。取包/许可或机制有疑问时再读 [research](research.md) 的对应一手条目，避免重读全部Wave1/JIT研究。

工程与共享业务身份依照 `docs/engineering.md`、`CONTEXT.md`、`docs/language-contract.md`；实现只涉及布局，不重新解释这些核心契约。真实机器编译参数由现有CMake/presets/toolchain-lock产生，不另建工具链表。

本次成功证据：`out/review/wave1-meeting/ui-layout-path-probe-headless-layout.json`、`ui-layout-minimal-headless-layout.json`、`yoga-provenance.json`及对应build.json。out材料若缺失，证据摘要在acceptance，来源pin/hash在research；不能假装本地已有二进制或重建已发生。

完成本阶段读取的标准：能明确区分“固定候选已PASS”“Module合同待实现”“实现前三个局部决定”，并知道本轮无性能收益结论。无需扩展到源码前端/JIT调用链调查。

## 2. 文件所有权与集成锁

以下生产路径是**建议的实施归属**，不是已存在文件；开始修改前由Main确认分配。

| 路径/内容 | 所有者 | 修改界限 |
|---|---|---|
| `src/ui_layout/layout.hpp`、`layout.cpp`及确有必要的私有文件 | 单一布局实现owner | 窄Interface、局部身份、校验、Yoga适配、快照；不散落到Runtime |
| `acceptance/ui_layout/main.cpp` | 同一布局owner | 从公开Interface验证contract，沿仓库无测试框架的真实入口风格 |
| `acceptance/ui_layout/`中的真实DirectWrite测量Adapter | 布局owner，隔离平台依赖 | 只为真实内容路径；核心不包含DWrite/Windows头 |
| `docs/modules/ui-layout/` | 同一文档owner | 更新合同、实现决定、验收状态及来源，避免多方覆盖 |
| 根`CMakeLists.txt`、`cmake/ui-layout.cmake`、`cmake/dependencies-lock.json`、许可分发 | Main或明确的集成owner | 唯一串行编辑者；布局owner交接确切target/依赖需求 |
| 工具链锁、presets、全局README/正式acceptance聚合入口 | Main/集成owner | 不因上游方便擅自升级或改变全仓构建 |
| `src/runtime`、`src/language`、`src/jit`、现有acceptance/jit、版本化语言规范 | 不在布局任务内 | 如真遇依赖需求，报告并重新裁决，不顺手扩张 |
| 已验证 `out/review/wave1-meeting/ui-layout-path-probe.cpp` | 冻结研究证据 | 正式Module用新的真实consumer；不改原probe让历史PASS失去对应 |

若RHI或另一session并行，不共享编辑common math、error、handle头；本模块先使用局部Size/Rect/约束/错误，不为少量值建立通用框架。集成者汇总后统一执行全项目验收；不要让并行席位在对方半成品期间运行全仓格式化/构建/测试。

## 3. 实施顺序与阶段完成标准

### A. 落定局部表示，保持选中范围

先解决decisions中的三个未冻结项：NodeId/快照表示、真实测量失败桥接、公开容量/深度界限。用当前C++23和工程习惯选最少结构；不建立后端工厂、反射属性或新的业务对象表。

推荐外部形状仍只有Document生命周期、节点/描述更新、批量关系编辑、layout和snapshot读取。`std::expected`等显式结果可承载局部错误，但不要为了复用Runtime的Error而链接整个Runtime。对外不返回Yoga指针、STL对象的跨语言ABI或未来JIT函数地址。

完成标准：每种借用/拥有者和失效规则都能落到真实字段与函数，测量失败不会被当正常0尺寸，跨Document/旧ID不能解析；将确切选择写回contract/decisions。未决pixel snap/hit-test是composition后续范围，不阻塞纯布局实现。

### B. 接入一个真实求解器和最小消费者

按research固定来源，由集成owner将URL/hash/许可落入现有唯一依赖体系。正式构建只编译核心所需上游源码/target；不带JS/Java/上游tests/gentest。不从系统任意Yoga安装或浮动branch取依赖。

先让真实公开Interface创建root与三个孩子，求出acceptance表2的300×100结果；消费者只读snapshot，不触碰Yoga。然后闭合移除/重排/旧ID拒绝和Document销毁。上游树是唯一可变权威，局部映射不复制style/cache。

完成标准：真实library target + consumer完成生命周期与指定几何；没有输出硬编码、假solver或未实现入口。此时只是合同的一部分，不能交付为完整里程碑。

### C. 完成合同内的全部行为

按acceptance表4逐类实现/验证column、intrinsic、min/max/overflow、非法输入、局部身份、revision、warm=fresh、失败快照、真实provider错误与重入。DirectWrite Adapter只进入平台验收target；固定叶验证是合法非文本内容，不扮演文字shaping。

测量在同步稳定内容上执行。真实内容/字号/约束变更必须有负对照与正确dirty路径；相同constraint的重复求值不能依赖调用次数改变内容。失败不贴新revision、不保留错误测量cache供成功路径使用。深树上限先选择并检查，不靠发生栈溢出后捕获。

完成标准：表4每条有清楚输入/可观察输出与失败行为，从公开Interface通过；删除任何真实guard都能让对应场景失败。不要增加只检查source text、私有字段布局或“函数被调用”的无效测试。

### D. 完成正式集成与交付

Main/集成owner检查target依赖方向、分配器/CRT、许可证和唯一锁。核心consumer不需要Runtime/LLVM/RHI/DWrite；Windows文本验收另链接DWrite。当前根CMake无条件配置JIT的历史结构不等于允许核心反向链接JIT，构建配置隔离如需调整由集成owner决定。

使用现有开发/发布环境及正式CMake入口验证模块consumer；最后由Main统一运行全项目验收，避免与并行RHI/主线冲突。当前会议阶段的“reviewer不运行”限制不是永久免验收理由；下一实施session按授权完成真实运行。

完成标准：生产实现、所有调用者、正式targets、两个配置的证据、许可证与本文档状态一致；acceptance中的未测项被真实材料更新或明确仍是非目标。回报关键输出、精确target/退出码、文件归属和残余限制，不沿用本轮probe PASS宣布新Module通过。

## 4. 禁止用更大项目替代这个任务

本里程碑不做控件/皮肤/编辑器、DOM/CSS全语义、文字引擎、renderer、输入焦点、窗口、任务调度、资产系统或业务语言绑定。也不先实现Taffy备用后端再比较。确实遇到contract无法表达的真实用例，给输入与反例，先裁决是否扩范围；不要通过静默忽略属性伪装兼容。

## 5. 交付证据形状

每条行为证据记录源revision/依赖pin、实际构建配置、输入、预期与实际、退出状态；失败保留原始诊断。性能只在明确真实负载下独立测量，不将整个进程48ms或五次measure写成布局预算。

旧snapshot、失效NodeId、测量失败、content/font dirty与最小依赖target是本模块必须有证据的接口行为。两DPI的纯值消费说明“不回写逻辑结果”，不冒充RHI渲染通过。

## 6. 启动完成后应保持的状态

研究probe和个人报告保留为历史证据；正式代码、合同及正式验收成为当前实现权威。本目录用链接连接来源，不新增第二套版本清单。未选中的Taffy/自写只是有明确重开条件的对照，不保留空后端、别名或deprecated兼容壳。
