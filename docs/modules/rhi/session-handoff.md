# 下一 session：实现离屏 RHI 资源提交闭环

状态：Main已选定本模块独立推进；目前只有SDK路径证据与本目录契约，没有生产RHI。当前文档任务不改已验证探针或生产源。下一implementation session可按本交接启动，不等待布局、窗口、语言或GC新功能。

## 开工读取与产出边界

顺序读本目录[README](README.md)→[contract](contract.md)→[acceptance](acceptance.md)，再读[engineering](../../engineering.md)、[CONTEXT](../../../CONTEXT.md)及当前CMake/toolchain lock。只有调整后端/依赖或核对原生细节时再读[decisions](decisions.md)/[research](research.md)。会议全过程可按需查[个人报告](../../research/wave1-meeting/parallel-modules/rhi.md)。

交付不是新CreateDevice示例，而是公开Interface能够完成资源/录制/提交/完成/回读/退休及错误关停的Module。第一个真实调用方和正式验收都从同一Interface进入。禁止复制一套“测试资源管理器”代替生产行为；平台探针只用来定位SDK调用与环境，不搬入生产充当封装。

## 文件与集成所有权

实施起点建议：`src/rhi/rhi.hpp`（窄公开Interface）与`src/rhi/rhi.cpp`（D3D12 implementation），`acceptance/rhi/main.cpp`（真实消费者/矩阵）。这些路径现在不是已存在实现；Main给implementation session授权后创建。先用现有命名规则，不预建backend base类/工厂或公共万能Handle库。

该session独占RHI局部源/验收和本目录；不修改runtime、language、jit、版本化语言规范或UI模块。根CMake、依赖锁、license打包和正式target合并由Main单一集成：建议`atelier_rhi`静态库及`atelier_rhi_acceptance`可执行目标，使用`atelier_target`并链接D3D12/DXGI/所需系统库。target名经Main落CMake后才是权威，不在本目录另造永久构建脚本。

当前SDK路线不需要安装Agility/DXC/Vulkan/NVRHI，不新增第三方源码锁。准确工具版本从现有锁读取；默认native分配沿mimalloc接入，GPU资源遵D3D/COM。Main已实际移除Runtime/JIT链接库并成功跑同路径，见acceptance；现有根CMake仍会配置JIT验收，不能承诺在现有顶层跳过LLVM环境。

## 具体实施顺序与阶段出口

### 1. 建立真实Device与资源域

实现明确adapter选择/显式WARP、debug层状态报告、单owner thread；最少RGBA8 texture/readback描述和错误类型。ResourceOwner move-only、ResourceId非拥有，device域/slot/generation和逻辑失效分开。Device覆盖孩子寿命，关停不能留到默认析构猜测。

出口：正式Module可创建受支持资源、拒绝非法参数/跨域ID，报告实际adapter/debug；无Runtime/LLVM/UI链接依赖。仅这一步完成不能宣布里程碑完成。

### 2. 贯通一条公开操作链

以`begin_recording→clear→copy_to_readback→submit→wait→map_completed`为调用方向；调用方只传typed资源标识与值，不负责COM保活或手写barrier。封装native资源状态与footprint/pitch，提交前准备持有存储，Close后提交，queue Signal生成ticket。

出口：模块Interface重现37×19两颜色逐字节匹配，真实fence完成，第二次allocator/descriptor在正确时机复用。正式验收不直接绕到native queue来替代module submit。

### 3. 完成所有权、取消与回读借用

Recording从首次引用获得内部持有；外部owner撤销立即使新借用ID失效但保住已录制内容。取消撤持有/丢弃预测状态；提交移交Device在飞记录，Ticket只保存完成值。完成时逐批撤持有，slots/generation最后回收。Readback绑定最后写入ticket，Mapping借用防止覆盖/关停失效。

出口：acceptance A03–A08逐项覆盖，尤其record未提交、取消、丢ticket、较晚批次继续使用同资源、旧ID重用及映射期间owner消失。必须有真实持有/退休计数和GPU完成证据，不以“没崩”通过。

### 4. 错误、有限等待与关停

完整实现contract的pre-submit失败和possible-submit后Signal失败区别、pending/timeout/lost/complete分层，最大fence值先判lost；失效设备不继续Map或发新工作。close要求孩子先撤，等待有限；超时保留Device/in-flight，确认lost走终态清理。明确Device违规先析构的诊断策略，资源析构不等待GPU或分配退休节点。

出口：acceptance A09–A12通过或明确记录真实环境阻塞并由Main决定支持边界；设备恢复仍不是此项通过含义。不能靠无限wait、提前释放、伪造fence或吞掉故障满足出口。

### 5. 正式集成与整体交付

Main合入CMake后，在准备好的锁定环境使用现有presets。示例入口是`cmake --preset dev`、`cmake --build --preset dev --target atelier_rhi_acceptance`，release同理；仅在该target实际建立后使用，当前不能声称命令已可运行。将可执行目标实际运行，记录矩阵结果；Main再按现有`--target acceptance`统一回归既有功能，避免多个session抢占GPU/测量资源。

出口：代码、公开注释、正式消费者、契约/验收证据相符；Module行为矩阵完整，尚未支持项准确排除，默认分配/链接和所有者退出无残留。把“已有平台PASS”和“本次模块PASS”分开报告。

## 可独立推进的Seam

- 单D3D12实现隐藏同步、资源状态、持有与回读，深度来自调用者不再重复不变量，不来自更多抽象层。
- UI只在未来composition侧把值快照转渲染命令；窗口只在presentation增量出现；两者不阻塞离屏验收。
- 未来业务包装复用Runtime权威类型/身份，释放GPU owner与业务对象销毁分开；首版无通用语言绑定和JIT回调。
- 单线程是明确支持范围，不是加一个mutex就自动升级为线程安全。并发/多queue时重新设计状态与完成关系，不能静默扩范围。

## 完成回交格式

给Main：实际修改文件和target、公开Interface行为、dev/release编译及真实执行命令/退出码、adapter/debug/OS/驱动、逐场景验收与资源计数、与原SDK证据的区别、明确未支持/阻塞。没有数据不报速度改进；device loss分类成功不报自动恢复。最多提出实际需要的后续增量，不把非目标表变成自动任务清单。

