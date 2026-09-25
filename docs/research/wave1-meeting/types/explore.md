# A 意图独立复核：共享类型、布局、值与对象生命周期

审查者：WaveTypesExplore。日期：2026-09-25。依据会议 baseline.json 当前内容哈希（含36项补充版），无 Git diff。生产代码只读，本席位未自行编译、运行或测量；Main统一执行的两份默认基线JSON、addressof隔离对照JSON及对应构建出处已只读核对，结果见§6。初稿内容在收到搭档结论前形成；写入曾受权限/编辑模式阻碍，成功落盘后才交换。

## 1. 独立初稿结论与边界

范围为 runtime.hpp/.cpp 的注册、布局、值操作、对象创建/销毁、借用/字段解析，native.hpp 及 runtime acceptance。GC 仅核对生命周期交给现有屏障/追踪计划的消费接口；不替 B 意图重审收集，不扩展发布/语言功能。

初稿未发现可按当前可信原生契约确认的生命周期或发布错误；互审后依据搭档的合法重载&反例，修正为：**存在一个默认字段Adapter误定位的P2缺陷，Main已在真实入口复现误读写及强边遗漏；仍须另行授权生产小修。注册原子性/生命周期展开尚未发现实现缺陷，所选失败清理×重入探针已由Main通过。** 搭档发现与本席位独立消费链确认见§4、§8。

源码已有事务staging、稳定共享Type节点、显式可重入工作栈、析构计划、同质标量快路，不应按文件长度重构或重新递归改迭代。成组注册二次冷路径成本值得量化，但不应挤占确定正确性反例的验证。

## 2. 意图图与责任

```text
原生 C++ 类型                         描述型 / 未来语言定义
 NativeRegistration<T>                     TypeDefinition
 只生产适配及定义、不拥有注册表                    │
          └────────────────┬────────────────┘
                           ▼
 register_types：私有临时身份 → 解析 → inline DAG布局
              → 能力/析构/引用/标量计划 → 预留 → 原子提交
                           ▼
 Impl::types : vector<unique_ptr<Type>>（共享、不可变、地址稳定）
           │                                │
     TypeId / FieldId                 const Type* / 共享计划
           ▼                                ▼
 describe / find_*                    值四操作 / create / destroy
           ▼                         原生：C++负责完整子对象
 resolve_object → resolve_path         描述：工作栈负责字段
   代际/身份       callback借用          ├→ 失败逆序清理
           ▼                           └→ shade_value→同一GC
      标量 / 引用读写
```

| 不变量 | 源码负责人 | 消费端证明 |
|---|---|---|
| 只有一个业务类型世界 | native.hpp:41-130；runtime.cpp:826-910 | build/publish→register_types；C++表示仅是解析键，发布后值描述只保存TypeId |
| 临时ID不外泄 | runtime.cpp:836-910,1072-1084 | TypeId输入只lookup已发布表；同批名字/表示在staged解析；find_type/describe只读正式表 |
| 元数据地址稳定且共享 | runtime.cpp:229-240,256,1078-1084 | vector只搬unique_ptr；FieldStorage.type、describe指针、工作帧共用节点 |
| 内联影响大小，引用只占槽 | runtime.cpp:935-956,983-1065 | resolve_path不跨引用；ReferenceSlot复制身份，不复制对象图 |
| 原生/描述型不重复管理子对象 | runtime.cpp:553-609,659-679；native.hpp:48-64 | 原生完整记录调用C++操作，描述型展开字段；原生析构不因无引用而跳过 |
| 构造失败无活目标；赋值基本保证 | runtime.cpp:595-623,1154-1180 | 完成字段计数控制逆序清理；赋值catch仍shade_value，部分新强边不丢失 |
| 深度不依赖C++栈，重入隔离 | runtime.cpp:282-311,563-608,636-678 | WalkScope使用suffix；复制frame及保存index，不跨回调持有vector元素引用 |
| 创建交付初始根，失败不封闭注册 | runtime.cpp:1207-1260 | slot预留→构造→加根→slot提交→sealed；加根失败销毁/释放 |
| 定位按owner校验，地址只作同步借用 | runtime.cpp:1396-1422,1505-1530 | locator受CallbackScope保护；create/destroy/GC禁入；定位返回至解引用间无用户回调 |

Runtime是深Module：小Interface隐藏事务、身份和生命周期复杂度；NativeRegistration是Adapter，不是第二个registry。可演进internal seam是事务内解析索引，不应把不变量外推给调用方，或拆出第二套对象存储。

## 3. Spec / Standards 分栏

| 审查点 | Spec | Standards |
|---|---|---|
| 原子发布 | 完整解析/布局/结果准备后提交，无名称/ID/表示半发布 | reserve可能改变私有容量，不等于公开状态回滚失败 |
| OOM | README:137允许Runtime自身分配和错误字符串继续抛异常 | 不将缺少bad_alloc→Result转换误报；索引改动仍须保持不半提交 |
| 失败状态 | C++原生展开；描述型逆序清已完成字段；赋值允许部分改变 | 不需整值备份，不擅自升级强异常保证 |
| 值与引用 | 对象不可内联；记录按值；引用默认空且复制旧身份 | 元数据按类型共享；shared_ptr字段替代会改变图/循环语义 |
| 深度/重入 | 有10,000层、正常重入、失败与嵌套visitor场景 | 显式栈已存在；高水位容量复用不是泄漏；组合覆盖另见§8 |
| 默认原生定位Adapter | F1不符合：合法重载&成员可误定位另一活实例 | 须由适配内部保证真实地址；不是要求调用方猜到默认helper错误 |
| 可信借用 | 调用方负责真实C++表示、引用报告和活存储 | 不要求从任意字节证明活值；手写错误locator不是普通不可信输入，但F1使用默认locator |
| 成员访问 | 缓存FieldId后不查名字；对象入口检查代际 | 不应长期缓存裸地址或因单线程删失效检查 |
| 冷路径发现 | 线性名字查询是明确阶段选择 | 批处理嵌套扫描有二次成本；不能等同稳态访问瓶颈 |

## 4. 最终三项有源发现（互审后）

### F1：默认定位器返回另一实例的成员（P2，源码证实且Main本机复现）

搭档提出反例后，本席位独立沿生产/消费端复核：native.hpp:73-80 的默认成员适配在79行使用一元 `&`。合法值成员类若重载 `operator&()` 返回另一持续存活、同型同对齐的 sentinel，这个表达式调用重载而非取得实际成员地址。default_initializable、nothrow_destructible、非const/volatile均不禁止这种C++类型。

注册侧 runtime.cpp:949-962 只核对 native_representation 与目标表示，不能识别“同型但另一实例”；消费侧 resolve_path:1403-1420 只检查 FieldId.owner/index 并接受返回地址，read/write:1484-1501直接读写。因此两个Outer的inner.number可别名到sentinel，而真实inner未改变。没有不活存储、类型伪造、UB或手写错误locator；这是默认Adapter未兑现“成员指针定位”责任，不能归因可信调用方违约。

- Spec：违反README:116的原生字段通过成员指针访问本实例字段；身份及布局类型检查不等于地址正确。
- Standards：真实子对象地址的不变量应留在NativeRegistration，不能要求每位调用者补偿默认适配。
- A最小修正：默认表达式使用 `std::addressof(static_cast<T*>(object)->*Pointer)`，不改TypeId/布局/根机制。方案须由Main另行授权，本次未编辑生产。
- B否决：禁止所有重载&类型或要求每个使用者手写locator，属无必要契约收缩/外推复杂度。
- 一手机制：C++工作草案[specialized.addressof] p1保证重载&存在时仍取得真实地址。访问2026-09-25，滚动草案，只支持机制判断，不代表本机ABI/性能验收。官方草案源链接见§9。
- 证伪：未修改默认helper的真实read/write若得到真实成员值且sentinel不变，则撤回。Main后续默认基线JSON实测edges=0、target_alive=0，确认真实inner强引用被漏枚举且未保活目标；这是具体探针的已观察结果，不再仅是误回收风险推断。

### F2：注册存在二次查找工作（P2，性能假设；不是Spec缺陷）

runtime.cpp:855-870对每型扫描published/staged名字与表示；894-904每条字符串/表示依赖再扫两表；918-933成对查成员名。N=批大小、P=既有类型数、E=依赖数，F/M=每型字段/方法数：名字去重Θ(NP+N²)，名字/表示解析O(E(N+P))，成员去重含Σ(F²+FM+M²)。这是比较工作量，不是实测时间。

N个单字段记录均引用字符串core.u32时，每条依赖扫完整staged再命中既有标量，即便最终布局仅4字节。反例是少量类型/一次启动注册；新增索引分配可能净亏，FieldId稳态访问不走此路径。10,000层验收只证明能运行。

- A保留线性冷路径。B仅事务内建名字/表示索引并指向同一Type，失败销毁，提交仍move unique_ptr；可比较连续排序索引+二分与标准哈希，不先加持久双表/interner/pool。
- 否证：真实规模发布耗时不重要或主要成本不是扫描，即保留A。结构抽取没有已测加速。

### F3：失败清理×回调重入组合已补证据（P3，原覆盖风险；本例通过，未确认实现缺陷）

原正式acceptance/runtime/main.cpp:1507-1523打开回调并验证正常重入；1525在故障注入前清空exercise，1540恢复，原有案例确实分别覆盖而非组合覆盖。源码WalkScope suffix、frame副本/index及稳定Type节点已提供机制，但最初不能替代组合执行。

Main现已运行真实types-counter-unwind探针：initialize与copy两路各有callbacks=3、inner_rejections=3，逆序/原地址/恰好一次清理、失败目标重试均PASS；最终资源数0，后续注册PASS，exit0。具体路径从“可证伪风险”收敛为“本机实验通过”，没有发现需要生产修复的反例。

- A保留现有工作栈实现和这份组合证据；正式回归是否纳入后续授权修复另由Main决定。
- B继续否决无反例的通用事务/整值备份或全禁重入。该探针不是任意深度、任意回调与OOM交互的穷尽证明；也不授权在当前借用存储上重建，或让异常越过noexcept析构。

### 非发现：候选方案的保留约束

- 同质无填充f32/u32树已有scalar_leaf快路；异质记录逐字段分派有工作量，但尚无占比数据（runtime.cpp:535-559,575-584,627-654,1049-1065）。自定义NativeLayout即使只包u32也可有可见行为；acceptance:1650-1691反证“无引用即memcpy/无析构”。相邻内建叶运行段只可在真实热点证实后研究，不先新增计划层。
- GC trace_fields仅含强边（runtime.cpp:1027-1030），公共trace输出强/弱/失效身份并保留scalar locator顺序（682-709）。weak-only记录contains_references=true而trace_fields为空，直接统一消费计划会漏弱边；跳过native scalar locator也可能改变副作用。保留不同计划，必要时从同一权威字段派生全引用边计划，不复制类型世界。

## 5. 失败部分状态核查

1. staged拥有所有新Type，失败自动清理。definitions被消费不代表公开Runtime状态变化；FieldStorage.type在失败后也没有公开访问入口。
2. result、walk_stack reserve、types reserve都在push提交前；提交无用户回调、字符串查找或再分配。没有证据需要undo log或整表clone-and-swap。
3. acceptance/runtime/main.cpp:1229-1320覆盖逻辑拒绝、布局溢出、表示冲突与修正重试；453-510覆盖describe地址稳定。未发现逐分配点bad_alloc发布注入；只记录覆盖边界，不升级缺陷。
4. 构造失败子字段由C++展开或内层frame清理，外层只销毁完成字段；引用槽不拥有目标销毁，不应清理被指对象。赋值失败后补标是与B意图共用的关键消费契约。
5. TypeInfo公开成员数组与内部FieldStorage不是两套类型世界：后者含全部hidden字段和定位/执行信息，前者只暴露可发现字段；生成FieldId用内部全字段index（runtime.cpp:958-964），resolve_path消费内部fields（1411-1418），不把公开vector位置误作存储序号。TypeId/值描述的少量类型级重复不能等同逐值元数据。
6. 生命周期/引用热循环已通过FieldStorage.type直达稳定元数据；describe/find_*属于发现路径，缓存FieldId路径的read/write不再字符串查找。事务/计划的结构抽取可改善局部性维护，但在相同机器、语义与工作负载下未测就不得标为吞吐改善。

## 6. 最高价值真实入口实验：默认locator误址

复用搭档独占的 `out/review/wave1-meeting/types-counter-address.cpp`，不重复造探针，不复制Runtime逻辑。类型Inner拥有u32及ReferenceSlot，operator&返回另一活Inner sentinel；Owner内联Inner并完全使用NativeRegistration默认field适配。原生构造记录真正Owner地址，sentinel由std::addressof获取。

真实入口链：register_types → create → find_field → read/write(owner, [inner, number]) → write_reference(真实inner槽) → visit_references(owner) → begin_collection/collect_step。GC只用于显示同一定位错误的真实消费影响，不建设额外收集器。

### 预期与反例

- 正确契约：before=17，写73后actual=73、sentinel=91、operator&调用数=0；强引用边枚举数1，移除target外根后仍被Owner保活；最后撤Owner根并回收，对象/根归零，exit0。
- 当前源码预期：before=91，写后actual=17、sentinel=73，标量路径operator&调用2次；枚举漏掉真实inner中的强边（edges0），完整GC后target不再活；退出2表示已揭示契约失败，不误报构建错误。未知异常退出3。
- 反例排除：sentinel与actual均为完整活C++同型对象；没有悬空locator、假NativeLayout、无效span或绕过屏障写入。引用通过Runtime::write_reference真实入口写入actual槽。
- 只测当前源码，生产只读；std::addressof修正后的预期不得当作已经运行。若真实未修改helper给出exit0及正确数值，撤回F1并查输入/构建对应性。

### Main的精确构建与运行入口

先按README进入开发环境；复用已由Main统一生成的release库和正式global-new覆盖object，probe只编译自己的源文件。下列来自CMakeLists.txt的atelier_target与out/release/build.ninja:208-231，不修改CMake、不重编生产。若对应正式产物尚不存在，由Main统一构建后再运行。

```powershell
. ./tools/enter-dev.ps1
clang-cl /nologo /DWIN32 /D_WINDOWS /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_MBCS /EHsc /O2 /Ob2 /DNDEBUG -clang:-std=c++23 -MD /W4 /WX /permissive- /utf-8 /Isrc /c out/review/wave1-meeting/types-counter-address.cpp /Foout/review/wave1-meeting/types-counter-address-release.obj
if ($LASTEXITCODE -ne 0) { throw 'types address probe compile failed' }
lld-link /nologo /subsystem:console /include:mi_version /out:out/release/types-counter-address.exe out/review/wave1-meeting/types-counter-address-release.obj out/release/atelier_runtime.lib out/release/CMakeFiles/atelier_runtime_acceptance.dir/src/memory/global_new_delete.cpp.obj out/release/_deps/mimalloc-build/mimalloc.dll.lib kernel32.lib user32.lib gdi32.lib winspool.lib shell32.lib ole32.lib oleaut32.lib uuid.lib comdlg32.lib advapi32.lib
if ($LASTEXITCODE -ne 0) { throw 'types address probe link failed' }
& ./out/release/types-counter-address.exe
$probe_exit = $LASTEXITCODE
```

可执行文件与既有mimalloc.dll/mimalloc-redirect.dll保持同目录。Main记录stdout和probe_exit：2是预期揭示基线缺陷，0是未复现/修正后契约通过，3是意外异常。此席位没有执行上述命令；Main已经统一执行默认基线，下面记录实际结果，不以进程elapsed_ms推导性能改善。

### Main本机实测（默认生产基线）

| 证据文件 | 实际输出 | 判定 |
|---|---|---|
| out/review/wave1-meeting/types-counter-address-default.json | exitCode=2；scalar before=91、actual=17、sentinel=73、overloaded_address_calls=2；trace edges=0、target_alive=0；cleanup=PASS；stderr为空 | 真实默认helper复现错误实例读写、漏强边及目标失去保活，F1升级为本机复现缺陷 |
| out/review/wave1-meeting/types-counter-unwind-default.json | exitCode=0；initialize/copy各callbacks=3、inner_rejections=3；reverse_address_exactly_once=PASS、retry=PASS；registration_after_failure=PASS、final_live=0；stderr为空 | 所选失败清理×重入组合通过，F3不升级为实现缺陷 |

两份JSON已由本席位只读核对；copy路径remaining_live=4属于尚活着的四个源资源，最终source清理后为0，并非泄漏。未重新运行Main已报告的状态。

### 隔离addressof候选路径对照（不是生产修复）

Main后续输出 `out/review/wave1-meeting/types-addressof-probe-default.json`：exitCode=0，scalar before=17、actual=73、sentinel=91、overloaded_address_calls=0；trace edges=1、target_alive=1；cleanup=PASS，stderr为空。本席位直接读取该JSON。

构建出处已核对 `types-counter-address-build.json` 与 `types-addressof-probe-build.json`：均为release正式clang-cl参数并链接同一 `out/release/atelier_runtime.lib`、正式global_new_delete object及mimalloc库。隔离probe源码首行选择 `out/review/wave1-meeting/addressof/native.hpp`，该副本80行使用std::addressof；并未将生产src/runtime/native.hpp替换为修复版。

此A/B关闭了本反例的候选路径问题：默认helper失败，实际地址helper通过，同一Runtime消费者可保留原语义。它不是任意类型证明或性能基准，进程elapsed_ms不用于加速声明。

**修复边界**：默认生产头文件保持未修复。Main已通过的out隔离副本仅是候选路径对照，不是生产已修复、永久回归已增加或正式验收已通过。生产改动仍须独立授权；本席位只更新证据报告。

交付前已只读核对搭档最终两份源码，并用带路径搜索确认身份：types-counter-address.cpp的Inner在19行，types-counter-unwind.cpp的Resource在26行、depth=512在130行。地址探针符合上述默认helper/真实强边/持续活sentinel条件；unwind探针以外层预算3触发三个已完成资源的逆序析构，各回调对不重叠更深存储做一次失败及一次成功重试。已独立检查资源ID过滤、地址/恰好一次和root/live断言自洽；这一步仅为静态审查，不冒充运行证据；其后的Main实际结果以上述两份JSON为准。第二探针精确构建命令见搭档challenge.md§9，不重复其文件。当前无研究或报告阻塞，本轮两项默认基线验证已完成，未批准的生产修复不在此次报告写作任务内。

初稿注册规模实验降为条件性第三动作：N=256/1024/4096，TypeId依赖/core.u32字符串依赖/前向inline链三组，各5个fresh Runtime，输入准备与行为检查不计入register_types计时；每型4字节，输出原样本及中位数。现阶段不另造该probe；F1和组合覆盖优先，不给事务索引预先通行证。

## 7. 最终优先动作（最多三项）

1. **P2**：默认locator反例已复现；建议另行授权std::addressof一表达式生产小修，并纳入read/write、引用枚举/保活的正式回归。隔离副本通过不等于生产修复。
2. **P3**：保存已通过的失败清理×重入组合证据；没有反例就保留现有实现，不先加事务备份，正式回归纳入与否随授权增量决定。
3. **P2/条件性性能**：实际批量发布时间值得关注时再测F2，结果支持后才评估事务局部索引；不做持久缓存、对象池或第二类型世界。

## 8. 对抗记录

初稿内容独立形成后落盘，并直接hub send给WaveTypesChallenge；已完整读对方types/challenge.md，完成第一轮带源码回应。初稿未发现缺陷的结论因F1反例修正，不将对方发现冒充独立发现。

| 对方主张 | 本席位证据回应 | 修正 / 未决及证伪 |
|---|---|---|
| 默认一元&可返回另一活实例，须std::addressof | 接受。定位到native.hpp:79；注册仅核representation，resolve_path只核owner/index；持续活同型sentinel避开可信接口违约 | 新增F1；随后Main默认基线已复现scalar误址及trace漏强边，见§6。候选隔离副本不冒充生产修复 |
| 失败×重入不能由分别通过推出 | 接受。acceptance:1525明确关闭exercise；frame副本/index和suffix仍构成源码反证，未发现实现bug | 最初改为F3覆盖风险；随后Main选定组合探针exit0，只补本例证据，不授权通用事务重写 |
| 冷路径O(N²)不授权缓存/pool | 同意，但建议保留输入准备不计时的最小发布规模实验，区分布局深度和查找工作 | 不做持久缓存/第二registry。实际发布不可见或索引成本抵消即保留原实现 |

本席位反质询：公共枚举不得直接复用strong-only trace_fields，weak-only记录是确切反例；runtime.cpp:696保留native scalar locator顺序，不能当纯查询跳过。搭档已直接hub回复接受，并引用1027生产强边计划、386消费屏障、682公共trace及696保序作为其回应证据；明确不倡议同步trace缓存，也接受F2的完整复杂度细化。双方完成至少一轮互读、源证据回应和反例质询，不以意见一致代替实测。

最终收敛：Main已复现地址缺陷，失败清理×重入的选定组合已通过；注册索引仍只测后决。已读默认基线JSON，将两类结果严格区分；随后Main的addressof隔离对照也已通过并经JSON/构建出处核实；只报告实际运行路径，不把双方共识或已通过的隔离副本当生产修复。

## 9. 证据边界

完整读取会议README、baseline.json；遵循engineering.md、CONTEXT.md、runtime-model.md、language-contract.md。源码和acceptance为主证据，位置逐项列出。旧开发/发布通过仅来自baseline，不是本轮实测。初稿无需外部机制研究；互审F1后使用Exa fetch直接读取已知一手URL，核对C++工作草案20.2.11 [specialized.addressof] p1（访问2026-09-25，滚动草案）：`https://eel.is/c++draft/specialized.addressof`。该条明确std::addressof取得真实对象地址，即使重载operator&。只支撑机制，不冒充固定编译器/ABI或本机证据。搭档另核对固定N4950版本，本报告不把未由本人读取的固定PDF当本席位证据。

不搬外部运行时语义、不宣称未测吞吐。事务/工作栈/消费链属源码证实；F1默认误址及漏强边为Main本机复现，addressof隔离候选为Main本机通过，F3选定组合为Main本机通过；F2仅源码复杂度证据，性能收益未测。所有执行由Main负责，生产未修复。

