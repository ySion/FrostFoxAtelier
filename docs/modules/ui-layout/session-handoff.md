# 生产首增量交接：自研布局核心

状态：**生产首增量已实现，开发/发布各17项正式场景及最小消费者通过。** 用户已授权从探索转入生产，不再等待第三方后端选型或首增量Interface审查。后续增量尚未实施，不将行列/锚定的通过扩大为完整游戏UI/编辑器系统完成。

## 接手顺序

1. [README](README.md)了解当前能力、依赖和目标。
2. [contract.md](contract.md)上半部分及[`layout.hpp`](../../../src/ui_layout/layout.hpp)了解权威Interface，尤其位置语义、回调寿命和结果借用。
3. [acceptance.md](acceptance.md)了解实际17场景、两配置运行/链接证据及未测项。
4. 需要后续方向时读[decisions.md](decisions.md)的S01–S08；研究报告和页尾Yoga材料仅作来源/历史。

## 当前实现与集成

- `src/ui_layout/layout.hpp`、`layout.cpp`：单一原生拥有者，Document身份＋slot/generation、有序树编辑、显式遍历、行列/覆盖/锚定、带界权重分配、测量缓存及事务发布。
- `acceptance/ui_layout/main.cpp`：只通过生产Interface验证，文字来自真实DirectWrite；未导入探索程序的Kernel或整数ID接口。
- `acceptance/ui_layout/minimal.cpp`：无文字库消费者，验证最小链接及基本生命周期。
- 根`CMakeLists.txt`：`atelier_ui_layout`、两个原生消费者、`ui_layout_acceptance`；总体`acceptance`也包含两者。复用已有`atelier_target`、分配器、工具链和presets，无新依赖锁或Runtime/JIT源码变更。
- 变更留在布局专属worktree；没有合并到其他worktree、提交或推送。共享文件集成已在该worktree完成，后续主线集成应串行协调并保留其他模块工作。

## 容易误用的约定

- move位置按移除后目标序列解释；无意义编辑不推进revision。根只能set_root，尺寸唯一来自viewport。
- Content不是自动fit父空间，Expand自然内容不是隐式硬min。主轴整池分配与槽内fill/align分离。
- 文字先解析实际宽度再按需测自然高；Fixed/确定填满高度不查询未使用的自然需求。
- 失败不提交新的几何或布局缓存；provider自身的可变排版对象不在回滚范围。几何副本不保活旧文字/绘制包。
- 旧view经过输入编辑及失败计算仍有效，下一次成功发布/析构结束借用；跨期消费者使用snapshot。
- 回调期间编辑和计算重入busy；销毁Document/provider违反调用寿命前提，不能把它当成安全关闭操作。

## 验证与证据

按正式环境和presets运行`cmake --build --preset dev --target ui_layout_acceptance`及对应release命令。记录在`out/review/ui-layout-production/`：四个原生进程exit0、两配置源码哈希一致、正式聚合入口exit0、链接命令与直接DLL导入表。

本轮没有重跑全项目Runtime/JIT验收；主线集成后由集成者按实际受影响范围运行总入口。out不保证随交接保留，缺失则按验收页命令重建，不沿用历史二进制声称当前源码通过。

## 下一完整增量

1. 共享列Inspector与换行/重复格：先明确共享轨道的参与、折叠和宽度依赖，再在同一拥有者/测量机制中实施真实算法；不新造控件框架或泛化任意约束图。
2. 真正局部调度：当前更新/移动是节点池编辑，但几何仍全树计算。应以固定/自然外框、共享依赖与结构编辑场景建立正确传播，再测访问量和真实provider成本。
3. 虚拟视图与延迟绘制：逻辑集合、extent、滚动锚点、旧文字/绘制包租期归各自拥有者，不能靠巨大float坐标或几何快照假装解决。

未覆盖generation/revision极限循环、core所有分配点失败注入、并发/异步、热卸载DLL ABI、非Windows构建、完整像素/渲染/输入或业务语言UI绑定。depth4096与当前文字数据只是验收场景，不是产品预算或性能承诺。

## 历史附录：旧 Yoga 交接，不再执行

以下原记录完整保留为当时阶段材料；不覆盖上方当前实现与合同。

### 历史交接：旧 Yoga 实施候选

> **本页不是当前开工入口。** 用户已决定自研布局库，当前从 [README](README.md) → [decisions.md](decisions.md) 的 S01–S08 → [真实验证结果](../../research/ui-layout-reassessment/validation-results.md) 接手。不再等待第三方 solver 选型，不按下文旧 Yoga 分工与合同直接实现。

以下保留上一阶段的实施交接作为历史资料。当前自研具体 Interface 仍需审查，生产库尚未实现；既有探针与历史证据保持不变。

#### 1. 先读取哪些权威

按顺序读 [README](README.md) → [contract](contract.md) → [decisions](decisions.md) → [acceptance](acceptance.md)。取包/许可或机制有疑问时再读 [research](research.md) 的对应一手条目，避免重读全部Wave1/JIT研究。

工程与共享业务身份依照 `docs/engineering.md`、`CONTEXT.md`、`docs/language-contract.md`；实现只涉及布局，不重新解释这些核心契约。真实机器编译参数由现有CMake/presets/toolchain-lock产生，不另建工具链表。

本次成功证据：`out/review/wave1-meeting/ui-layout-path-probe-headless-layout.json`、`ui-layout-minimal-headless-layout.json`、`yoga-provenance.json`及对应build.json。out材料若缺失，证据摘要在acceptance，来源pin/hash在research；不能假装本地已有二进制或重建已发生。

完成本阶段读取的标准：能明确区分“固定候选已PASS”“Module合同待实现”“实现前三个局部决定”，并知道本轮无性能收益结论。无需扩展到源码前端/JIT调用链调查。

#### 2. 文件所有权与集成锁

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

#### 3. 实施顺序与阶段完成标准

##### A. 落定局部表示，保持选中范围

先解决decisions中的三个未冻结项：NodeId/快照表示、真实测量失败桥接、公开容量/深度界限。用当前C++23和工程习惯选最少结构；不建立后端工厂、反射属性或新的业务对象表。

推荐外部形状仍只有Document生命周期、节点/描述更新、批量关系编辑、layout和snapshot读取。`std::expected`等显式结果可承载局部错误，但不要为了复用Runtime的Error而链接整个Runtime。对外不返回Yoga指针、STL对象的跨语言ABI或未来JIT函数地址。

完成标准：每种借用/拥有者和失效规则都能落到真实字段与函数，测量失败不会被当正常0尺寸，跨Document/旧ID不能解析；将确切选择写回contract/decisions。未决pixel snap/hit-test是composition后续范围，不阻塞纯布局实现。

##### B. 接入一个真实求解器和最小消费者

按research固定来源，由集成owner将URL/hash/许可落入现有唯一依赖体系。正式构建只编译核心所需上游源码/target；不带JS/Java/上游tests/gentest。不从系统任意Yoga安装或浮动branch取依赖。

先让真实公开Interface创建root与三个孩子，求出acceptance表2的300×100结果；消费者只读snapshot，不触碰Yoga。然后闭合移除/重排/旧ID拒绝和Document销毁。上游树是唯一可变权威，局部映射不复制style/cache。

完成标准：真实library target + consumer完成生命周期与指定几何；没有输出硬编码、假solver或未实现入口。此时只是合同的一部分，不能交付为完整里程碑。

##### C. 完成合同内的全部行为

按acceptance表4逐类实现/验证column、intrinsic、min/max/overflow、非法输入、局部身份、revision、warm=fresh、失败快照、真实provider错误与重入。DirectWrite Adapter只进入平台验收target；固定叶验证是合法非文本内容，不扮演文字shaping。

测量在同步稳定内容上执行。真实内容/字号/约束变更必须有负对照与正确dirty路径；相同constraint的重复求值不能依赖调用次数改变内容。失败不贴新revision、不保留错误测量cache供成功路径使用。深树上限先选择并检查，不靠发生栈溢出后捕获。

完成标准：表4每条有清楚输入/可观察输出与失败行为，从公开Interface通过；删除任何真实guard都能让对应场景失败。不要增加只检查source text、私有字段布局或“函数被调用”的无效测试。

##### D. 完成正式集成与交付

Main/集成owner检查target依赖方向、分配器/CRT、许可证和唯一锁。核心consumer不需要Runtime/LLVM/RHI/DWrite；Windows文本验收另链接DWrite。当前根CMake无条件配置JIT的历史结构不等于允许核心反向链接JIT，构建配置隔离如需调整由集成owner决定。

使用现有开发/发布环境及正式CMake入口验证模块consumer；最后由Main统一运行全项目验收，避免与并行RHI/主线冲突。当前会议阶段的“reviewer不运行”限制不是永久免验收理由；下一实施session按授权完成真实运行。

完成标准：生产实现、所有调用者、正式targets、两个配置的证据、许可证与本文档状态一致；acceptance中的未测项被真实材料更新或明确仍是非目标。回报关键输出、精确target/退出码、文件归属和残余限制，不沿用本轮probe PASS宣布新Module通过。

#### 4. 禁止用更大项目替代这个任务

本里程碑不做控件/皮肤/编辑器、DOM/CSS全语义、文字引擎、renderer、输入焦点、窗口、任务调度、资产系统或业务语言绑定。也不先实现Taffy备用后端再比较。确实遇到contract无法表达的真实用例，给输入与反例，先裁决是否扩范围；不要通过静默忽略属性伪装兼容。

#### 5. 交付证据形状

每条行为证据记录源revision/依赖pin、实际构建配置、输入、预期与实际、退出状态；失败保留原始诊断。性能只在明确真实负载下独立测量，不将整个进程48ms或五次measure写成布局预算。

旧snapshot、失效NodeId、测量失败、content/font dirty与最小依赖target是本模块必须有证据的接口行为。两DPI的纯值消费说明“不回写逻辑结果”，不冒充RHI渲染通过。

#### 6. 启动完成后应保持的状态

研究probe和个人报告保留为历史证据；正式代码、合同及正式验收成为当前实现权威。本目录用链接连接来源，不新增第二套版本清单。未选中的Taffy/自写只是有明确重开条件的对照，不保留空后端、别名或deprecated兼容壳。
