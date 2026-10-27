# 单机资源服务：LMDB、SQLite 与 LevelDB

日期：2026-10-27。范围为 Windows x64 单机资源服务的嵌入式存储选型；只核对官方资料和项目现有责任，不安装依赖、不建库、不做性能测量。用户已澄清讨论的是 LMDB 这类嵌入式数据库，不是 [LLDB 调试器](https://lldb.llvm.org/)；这不等于已经指定最终后端。三种数据库都是嵌入式库，选择它们不意味着额外部署一个数据库服务器。

## 建议与理由

**范围修订：先按当前资产与实际缓存需求定模型，再决定是否接入数据库及选择哪个后端。** 用户已明确不需要资产操作 Undo 或可回退的导入历史，见[资产现态与参数撤销](../modules/resources/asset-system-plan.md#当前范围资产现态与参数撤销)。以下官方事实仍有效，持久精确内容图是可选布局，不是首版必需；一次请求冻结输入不要求建立资产版本仓库。

先前倾向资源服务独占 SQLite 元数据/索引、较大不可变内容外置，理由是精确内容直接/反向依赖和清理关系较多时可减少手工辅助索引维护，不是性能排名。若首版只需当前条目、实际构建命中与请求期保活，这一前提应重评；LMDB 或更小的现有文件/内存索引也可能足够。具体责任见 [Live 重评](../modules/resources/live-resource-reassessment.md)，不为了证明先前建议而增加关系查询。

不做数据库抽象框架或同时维护多个后端。客户端只看资源模块的内容/版本 interface，数据库是服务内部 implementation；更换数据库不应要求重写 Live 或改变资产身份。

## 官方事实对照

| 项 | SQLite | LMDB | LevelDB |
| --- | --- | --- | --- |
| 数据模型 | SQL，表、索引、约束与事务 | 有序 KV，事务 | 有序 KV，原子 WriteBatch、读快照 |
| 写并发 | 同一时刻一个写事务 | 同一时刻一个写事务 | 多线程共享 DB；业务读改写需另行协调 |
| 多进程 | 可访问同一库；WAL 要求同机 | 可访问同一 environment | 一个数据库目录只能由一个进程打开 |
| 关系/反向查询 | 可用表和索引表达 | 应用设计键空间及辅助索引 | 应用设计键空间及辅助索引 |
| 长读的影响 | WAL checkpoint/回收可能受阻 | 旧页不能复用，数据库可能增长 | 快照与迭代器有各自使用约束；本轮不作横向空间性能结论 |

### SQLite

- 官方明确 “SQLite does not have a separate server process”；它运行在调用它的程序内部。见 [About](https://sqlite.org/about.html)。
- 支持多条语句组成原子事务；多个连接/线程/进程可以读，但只允许一个同时进行的写事务。见 [Transactions](https://sqlite.org/lang_transaction.html)。
- WAL 模式允许读者与写者并行，仍不是多写者；长读事务可能阻碍 checkpoint，持续重叠的读者可使 WAL 增长，也仍可能返回 `SQLITE_BUSY`。自动 checkpoint 默认阈值为 1000 页，不能把它当作适合本项目的已测配置。WAL 协作要求同一台机器。见 [WAL](https://sqlite.org/wal.html)。
- 活跃 WAL 是数据库持久状态的一部分，不可将在线复制单个 `.db` 文件当作完整安全备份。采用 WAL 时应使用修复 WAL-reset 竞争问题的版本：官方当前文档注明 3.51.3 及以后修复，并有 3.44.6 / 3.50.7 回补；实际依赖版本另在接入时锁定。见 [WAL §11](https://sqlite.org/wal.html)。
- SQLite 可以将 BLOB 和元数据放在同一数据库事务中；外置大内容是本项目的布局建议，不是 SQLite 不能存图片。见 [Application File Format](https://sqlite.org/appfileformat.html)。

### LMDB

本轮进一步核对 OpenLDAP 官方源码 [2d3cb4c0](https://github.com/openldap/openldap/commit/2d3cb4c0dee9366fec37b8897e858a7a6fb85a6e)，其 [lmdb.h](https://github.com/openldap/openldap/blob/2d3cb4c0dee9366fec37b8897e858a7a6fb85a6e/libraries/liblmdb/lmdb.h) 标为 1.0.2（2026-08-06）。这是研究版本，不是依赖选定；实际接入须锁定正式版本及 Windows 构建配置。

- 定位是 “transactional embedded key/value store”，提供 ACID 事务，支持多个进程/线程并发访问；只有一个活动写事务，读者使用快照。
- 读取结果直接指向映射内存。官方规定返回值有效期只到后续更新操作或事务结束，不能修改/释放数据库持有的数据。不能把这样的借用指针直接当成跨异步任务、跨进程或 GPU 帧的长期资源 owner。
- 官方指出 “Read transactions prevent reuse of pages freed by newer write transactions, thus the database can grow quickly.” 因而不要为了资源保活长期保留数据库读事务；复制必要的小元数据并结束事务，内容保活另由资源模块负责。
- map size 同时限制数据库最大大小，达到限制会遇到 `MDB_MAP_FULL`。打开后扩 map 要求调用进程没有活动事务；其他进程可能需处理 `MDB_MAP_RESIZED`。单服务进程集中访问可减轻协调负担，但不消除容量管理。
- 官方告诫不要在远程文件系统上使用 LMDB。当前单机本地盘目标不受这一限制，但不能据多进程能力顺推到网络共享盘。

LMDB 能胜任内容/构建缓存的 KV 索引，依赖关系并不要求 SQL。代价是辅助索引的一致更新和 schema 演进由我们编写；“不用 SQL”不等于不需要数据模型。本方案会复制小元数据后结束事务，大内容另走文件，因此不能把裸 KV 读取或映射读取优势直接当作 Live 性能收益。

### LMDB 最小模型：可行性草图，不冻结 schema

若最终选择保存跨请求复用的精确内容图，其查询可收窄为精确内容获取、有界直接/反向边查询，以及实际存在的构建缓存命中。一个服务独占一个 environment，两个 named DB 即可表达，不需要通用图数据库；不需要这类图时应删去相应记录，而不是先完整实现再设法使用：

| 存储 | 候选记录 | 用途 |
| --- | --- | --- |
| `items`，普通唯一键 | 内容键 → 格式/长度/定位、唯一直接引用列表、`Ready/Deleting` 状态 | 精确内容与可读取状态；规范清单另存不可变文件 |
| 同一 `items` | 构建键 → 产物清单键；缓存 schema 标识 | 有真实构建消费者时才加，不为首批材质交付建立空构建系统 |
| `referrers`，`MDB_DUPSORT` | 子内容键 → 父清单键；产物清单键 → 构建键，以固定标签区分 | 前者是供给完整清单所需的拥有边，后者仅用于逐出时清理缓存别名 |

- 资产身份、精确内容键、内容发布版本、请求代次各有作用域，见 [CONTEXT](../../CONTEXT.md#资产与内容)。构建键还包含实际输入及编译器/目标/选项等构建条件，不等于单个产物键。首批不需要让数据库权威维护 `AssetId → latest`；由生产者选定精确清单。
- 规范清单保留作者身份、关系角色、槽顺序和绑定；索引里的去重直接引用列表不能替代这些语义。反向查询得到的是**所有留存精确版本**的引用，不是当前 Editor/Play 消费者清单。
- 一次短写事务维护主记录、每条反向拥有边和实际构建别名；LMDB 保证事务原子性，不会替应用补上漏写的辅助索引。同构建键若产出不同内容，应诊断键不完整或非确定性，不能静默覆盖。
- 构建缓存别名不是永久保活根，否则成功产物永远无法逐出。首步可做有界分段扫描，不需要精确全局 LRU，也不应每次命中都同步写“最近访问”。无入边逐出只适用于已验证的无环必要内容图，不将任意作者图套进它。
- 键明确编码，不持久化原生结构体或指针；`MDB_DUPSORT` 的值也受最大键长限制，使用 `mdb_env_get_maxkeysize()` 核对。DBI 在初始化事务打开并保留，`maxdbs` 在打开 environment 前设置，不由工作线程并发打开。见 [DBI/put 合同](https://github.com/openldap/openldap/blob/2d3cb4c0dee9366fec37b8897e858a7a6fb85a6e/libraries/liblmdb/lmdb.h#L1307-L1378)。

相同责任在 SQLite 可表达为内容、依赖、构建三类表，并为依赖目标和构建产物声明索引；反向索引的更新由数据库完成。[外键检查](https://sqlite.org/foreignkeys.html)仍须逐连接启用，且不能校验外部文件是否完好。SQLite 的维护优势是少写辅助索引代码，不是替代资源保活或自动解决 Live。

### LMDB 的 Windows 与事务寿命约束

- `MDB_MAP_FULL` 后先中止失败事务，暂停准入并排空活动事务，再扩 map、重试完整元数据事务；期间保留发表预留。扩 map 会重映射，不能保留旧页地址；失败须受控关闭/恢复。map 满与磁盘满是不同错误。所查普通 x64 Windows [映射实现](https://github.com/openldap/openldap/blob/2d3cb4c0dee9366fec37b8897e858a7a6fb85a6e/libraries/liblmdb/mdb.c#L5075-L5227)默认渐增物理文件，`MDB_FIXEDSIZE` 会改变路径，不能将虚拟 map 上限直接当作已经分配的磁盘空间。
- 不跨 IPC、I/O、编译或帧等待持有读事务；大扫描分段后不再声称具有全程同一快照。`mdb_txn_reset()` 只释放读快照对旧页的保持，不是内容租约；[mdb_reader_check 实现](https://github.com/openldap/openldap/blob/2d3cb4c0dee9366fec37b8897e858a7a6fb85a6e/libraries/liblmdb/mdb.c#L12794-L12838)跳过本进程 PID，不能用来补救自身泄漏的活动读事务，更不会恢复客户端保活。
- 初始正确性合同保留正常持久性设置，不因为叫缓存就打开 `MDB_NOSYNC/MDB_NOMETASYNC/MDB_MAPASYNC`。具体影响以锁定版本的[标志文档](https://github.com/openldap/openldap/blob/2d3cb4c0dee9366fec37b8897e858a7a6fb85a6e/libraries/liblmdb/lmdb.h#L631-L689)为准；数据库正常持久提交仍不覆盖独立内容文件。

### LevelDB

- 官方 [README](https://github.com/google/leveldb/blob/main/README.md) 描述其为有序 KV 库，没有内置 client-server 支持、关系模型、SQL 或二级索引。
- [使用文档](https://github.com/google/leveldb/blob/main/doc/index.md) 明确一个数据库只能由一个进程打开，通过 OS 锁防止误用；同一 `DB` 对象可以多线程共享。因此若采用它，Editor/Play 必须经资源服务访问，而非分别打开同一目录。
- `WriteBatch` 可原子应用一组写入，Snapshot 提供一致的只读视图；单独 `Get` 后再 `WriteBatch` 不是自动隔离的读改写事务。[公开 DB interface](https://github.com/google/leveldb/blob/main/include/leveldb/db.h) 没有将上述读写包成通用事务的入口。
- 默认写入并非等待稳定存储，`WriteOptions::sync = true` 才请求同步持久化。原子性和断电持久性不能混淆。见上述使用文档。
- 官方提供 Windows/x64 构建说明，不能声称不支持 Windows；同时 README 明示 “very limited maintenance”，接入前需核对本项目锁定工具链和维护成本。

## 数据库该保存什么

按最终操作选择当前目录的派生索引、真实依赖或构建缓存记录。只有选择了上述精确内容缓存模型，才保存其内容描述、拥有边、发表与回收状态；参数 Undo 不要求这些历史记录。只随真实消费者增加表，不先建全资产平台的 schema。

作者文件、GUID/sidecar、材质单一 codec 仍是权威；数据库不成为另一份可编辑材质定义。未保存草稿由 Editor 冻结后提交，资源服务不能只监听磁盘后把最新版当成该草稿。目录可以只表示资产的当前内容，但已冻结请求必须另持自己的精确输入，不能因当前条目改变而变化。

数据库持久事务不等于以下任何事情：编辑器 Undo、Runtime 引用事务、跨进程采用 ACK、GPU 就绪/完成、文件系统上任意其他文件的原子变化。实际消费保活要与缓存索引分开；数据库短事务不能拖着整个异步构建或一帧渲染不结束。

## 外部内容文件与数据库：需要自己的发表顺序

**外部 blob 与数据库元数据不是一个自动原子事务。** 内容寻址和不可变命名也不能补上这个保证。若选择上述布局，以下是候选发表/回收协议；不表示所有首版实现都要有持久拥有图或 tombstone：

1. 在服务内预留本次发表的内容及必要依赖，防止清理并发删除；事务外写临时内容、验证长度/摘要，完成不可变文件的持久发表。
2. 在数据库短事务中核对必要内容完整可取，提交相应版本、依赖和实际产物索引；不能让清单只有部分必要内容就标为可取。
3. 建立或转交请求保活后才撤预留并返回“资源已发表”；客户端仍需独立完成 CPU/GPU 准备与采用。

文件完成而事务未提交会留下孤儿；事务已提交而回复未送达则结果可能已经成功，重试须按精确键核查。不能先提交索引再补文件，也不能仅凭一次“无索引”扫描删除正在发表的文件。以下协调在 SQLite 与 LMDB 下都不可省：

- **获取即保活。** 查询精确 `Ready` 记录与建立请求保活必须相对逐出不可分割；然后结束数据库事务，再读取/交接文件。不能先给路径，稍后再 pin。数据库读快照只保护数据库页，不保护外部文件。
- **逐出先禁止新获取，再删文件。** 核对没有请求/发表保活和必要拥有入边，事务标 `Deleting` 并清理相关边/别名；事务外删文件，成功或确认不存在后才清 tombstone。同键重新发表须等旧删除结束，避免迟到删除删掉新文件。删除失败保留待清理状态，不冒称释放成功。
- **内存 owner 与文件 owner 分开。** 自持完整字节的旧帧/实际消费者不依赖缓存文件；仍借用文件映射、句柄或异步读取的 owner 必须继续保文件。hash 本身不保活，参数历史也不是旧导入内容的保留理由。服务重启后空 pin 表不证明旧访问结束，沿 Live 重评的停止/排空/新域策略恢复。

Windows 的 [DeleteFileW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-deletefilew)可因未允许删除共享的既有句柄而失败，这只能作额外保护，不能替代上述合同。[FlushFileBuffers](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers)也不证明任意 rename 序列与数据库事务构成一次断电原子提交。具体文件发表/恢复须真实实现验证，进程终止证据不能冒充断电证据。

**索引重建有前提：规范清单也须作为不可变内容留存，不能只剩图片字节与 hash。** 校验清单及其必要内容后可重建内容记录/关系；构建映射仅在留存产物记录含足够构建来源时重建，否则丢弃该缓存命中并重新构建。不能由此恢复 Editor 历史、请求资格或前一连接的采用 ACK。旧版本缺失应报告不可用或由仍持有精确内容的生产者重新提供，不用最新作者文件冒充旧版。

## 成熟引擎实证：O3DE 能证明什么

按用户要求，独立调研核对了 O3DE `development` 的 [106242cb](https://github.com/o3de/o3de/commit/106242cbde6038313d3edae88b1e90ccc98cab37)（2026-10-26 提交），只读取官方源代码及文档，没有克隆、构建或运行。它证明了责任分离的可行性，不证明本项目应选择同一数据库，更不代表 IOI 的内部实现。

- **处理进程、元数据库与运行消费分离。** Asset Processor 独立运行并启动 AssetBuilder 子进程；Editor/Launcher 通过协议交互，不作为 SQLite 运行时客户端。`Cache/assetdb.sqlite` 保存开发/构建记录，运行时发现使用 Asset Catalog。见 [Builder.cpp](https://github.com/o3de/o3de/blob/106242cbde6038313d3edae88b1e90ccc98cab37/Code/Tools/AssetProcessor/native/utilities/Builder.cpp#L154-L169) 和[官方数据库说明](https://docs.o3de.org/docs/user-guide/assets/asset-processor/asset-database/)。本项目可借鉴单一服务拥有索引，不需同时引入 builder 进程池或另存一份运行时 catalog。
- **实际关系不是只有 hash 查表。** [数据库 schema](https://github.com/o3de/o3de/blob/106242cbde6038313d3edae88b1e90ccc98cab37/Code/Tools/AssetProcessor/native/AssetDatabase/AssetDatabase.cpp#L30-L175) 包含 source→job→products、源码依赖和产物依赖；[反向查询](https://github.com/o3de/o3de/blob/106242cbde6038313d3edae88b1e90ccc98cab37/Code/Framework/AzToolsFramework/AzToolsFramework/AssetDatabase/AssetDatabaseConnection.cpp#L737-L784) 从记录的边查依赖者。构建失效依赖与运行加载依赖是不同用途，不应合并成“改一项就更新所有游戏对象”。
- **它不是精确版本服务器的证据。** [资源变更消息](https://github.com/o3de/o3de/blob/106242cbde6038313d3edae88b1e90ccc98cab37/Code/Framework/AzFramework/AzFramework/Asset/AssetProcessorMessages.h#L612-L663) 携带身份、路径、类型、大小、平台和依赖，而不是不可变内容摘要/版本凭据。普通产品位于可变缓存路径，不能照搬后声称已解决同键两版或冻结后改盘。
- **文件＋数据库的原子性仍需项目自己保证。** [产物复制](https://github.com/o3de/o3de/blob/106242cbde6038313d3edae88b1e90ccc98cab37/Code/Tools/AssetProcessor/native/resourcecompiler/rcjob.cpp#L767-L793) 逐文件执行；[文件工具](https://github.com/o3de/o3de/blob/106242cbde6038313d3edae88b1e90ccc98cab37/Code/Tools/AssetProcessor/native/utilities/assetUtils.cpp#L77-L146) 还会先删除既有目标。所查路径没有跨文件、数据库、catalog 和通知的一次事务，不能将它作为“失败必留完整旧组”的证明。[官方缓存说明](https://docs.o3de.org/docs/user-guide/assets/pipeline/asset-cache/) 也明确外部更改/删除可能未被发现。
- **保留旧内容与采用通知是两件事。** [AssignAssetData](https://github.com/o3de/o3de/blob/106242cbde6038313d3edae88b1e90ccc98cab37/Code/Framework/AzCore/AzCore/Asset/AssetManager.cpp#L1704-L1729) 替换内存查找项但保留仍被引用的旧数据；这里的内部“DB”不是 SQLite。[运行时热重载说明](https://docs.o3de.org/docs/user-guide/assets/runtime/#hot-reloading) 指出消费者已复制到游戏状态的值需要自己更新。这与本项目让内容 owner 和游戏采用分别负责相符，不等于已有全材质/GPU 成组采用。

本项目应借鉴服务单写、记录真实边、元数据与内容分离、消费者拥有旧内容；不复制远程 FileIO/VFS、跨平台任务调度、插件 builder 池、旧身份迁移或完整打包基础设施。变更通知只能提示有新版本，不能强制采用；将来重连时即使查询当前版本，也必须与本请求冻结的精确凭据核对，不能用“查询最新”替代旧请求恢复。

## 后续验证范围

先以最小布局完成真实资源服务的发表/获取/释放与重启路径；若需要数据库只接一个后端，不先建双后端基准工程。测已命中内容、参数连续更新、实际存在的依赖查询、2K/4K 新图、取消和磁盘预算，区分元数据耗时与哈希、文件、解码/编译和 GPU 准备；记录真实节点/边规模、尾延迟、常驻/映射内存和长期磁盘增长。

验收还须覆盖发表/删除各中断点、同键重新发表与迟到删除、磁盘满、所有候选都在保活时的有界背压、重启时尚有客户端文件访问，以及哪些索引确实可重建。采用 SQLite 时验证 checkpoint 与短事务策略；采用 LMDB 时验证 map 满/增长、暂停时长及读事务结束。若之后要以性能选择 LMDB，比较必须保持相同内容布局、持久性和保活合同，不能以裸 `get()` 排名替代端到端收益。

两路调研均为只读官方资料/源码研究；未安装、运行或测量数据库。**SQLite 与 LMDB 均可行，没有已测性能优胜者；范围收窄后，不能仅凭原内容图的维护成本继续确定 SQLite 优先。** 最终若选择 LMDB，应明确接纳实际需要的辅助索引与 map 生命周期责任，不扩大成数据库抽象层、分布式租约或全资产平台。
