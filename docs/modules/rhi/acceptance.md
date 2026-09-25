# RHI 验收与证据

状态：SDK路径已有Main实测；生产Module尚未实现。当前会议reviewer没有执行构建或GPU工作。未来模块必须从公开Interface进入，不拿直接SDK探针替代wrapper验收。

## 已有平台证据

| 项目 | 已观察结果 | 原始证据 |
| --- | --- | --- |
| 首次硬件debug探针 | `--require-debug`，exitCode=0，stderr空 | [rhi-path-probe-hardware-debug.json](../../../out/review/wave1-meeting/rhi-path-probe-hardware-debug.json) |
| 独立最小链接 | 正式release host编译标志，Clang C++23；移除Runtime/JIT静态/导入库，保留global_new_delete对象、共享mimalloc与Windows/D3D12/DXGI库 | [rhi-minimal-build.json](../../../out/review/wave1-meeting/rhi-minimal-build.json)、[build.log](../../../out/review/wave1-meeting/rhi-minimal-build.log) |
| 最小链接后的真实执行 | `--require-debug`，exitCode=0，stderr空；同样两轮正确 | [rhi-minimal-hardware-debug.json](../../../out/review/wave1-meeting/rhi-minimal-hardware-debug.json) |
| 实际adapter | AMD Radeon RX 6800 XT，vendor=0x1002、device=0x73BF、software=0；当次LUID=00000000:0001088E | 两份run JSON；LUID不是持久设备ID |
| 验证层 | debug_requested=1、debug_enabled=1、gpu_validation=0；messages/errors/warnings均0 | 仅D3D12 debug layer，不含GPU-based validation |
| 像素/布局 | 两次37×19；row_bytes=148、row_pitch=256、footprint_bytes=4756；mismatches=0 | 非紧致行布局确实被使用 |
| 提交/完成 | ticket=1/2分别completed=1/2；caller CPU owner在submit前撤销，submission_hold在fence后释放 | SDK COM持有演示，不是生产retirement manager |

探针源码：[rhi-path-probe.cpp](../../../out/review/wave1-meeting/rhi-path-probe.cpp)。两轮颜色为RGBA(255,0,255,255)和(0,255,0,255)，每轮703像素、2812有效颜色字节。上述数据归Main实跑，本目录作者只读证据，不重新运行已报告结果。

**不外推**：该证据不覆盖WARP、其他GPU/驱动、device-loss注入/恢复、性能、shader/draw/Present、尚不存在的Record取消/丢Ticket/陈旧句柄规则。JSON中的进程elapsed包含启动、驱动和同步，不是性能基线。最小链接仍使用来自既有正式target的global-new对象，因此证明路径级独立链接，不代表已建立独立Module target。

## 未来模块验收矩阵

在Main批准的正式dev/release目标上使用真实D3D12后端。行A01–A12均是首里程碑门槛；能力/环境不足时记录blocked/unsupported并由Main限制支持范围，不能计作PASS。

| ID | 真实入口/触发 | 必须观察 | 可捕获的错误 |
| --- | --- | --- | --- |
| A01 | Device创建，明确hardware选择；另可显式WARP | 记录adapter/debug/能力；硬件失败不自动软件回退；非法格式/尺寸在副作用前拒绝 | 静默fallback、溢出/不支持usage |
| A02 | Texture+Readback→record clear/copy→submit→wait→map | 37×19两颜色mismatch=0，按真实pitch取行；ticket匹配，Map在完成后 | 只创建device、读旧数据、忽略padding |
| A03 | 录制引用texture后释放CPU owner，再submit | 外部ID已不允许新record，但既有Recording仍成功读回；native资源保持至完成 | 只按CPU作用域保活、submit重验owner导致错误拒绝 |
| A04 | 录制引用后释放owner，再取消；另测Recording析构取消 | 未提交命令不执行；内部持有撤销；无其他持有时资源恰好释放；最后已提交状态不变 | 未提交泄漏、取消污染state tracker |
| A05 | submit后销毁Recording外壳、丢弃Ticket、释放CPU owner，继续驱动Device | in-flight由Device持有；完成前不释放，完成后回收到基线；无需保留用户Ticket | 将完成查询凭证误当唯一所有者 |
| A06 | 两批使用同一texture/先后不同readback，早期ticket完成 | 较晚批次仍在飞时resource不退休；其余已完成allocator可合法复用 | 以最早fence当last-use或全局stall |
| A07 | slot复用、generation改变、跨device/种类错误ID与ticket | 明确拒绝；不绑定新资源；失效检查不误撤内部record持有 | ABA、跨域误解析 |
| A08 | Map借用期间释放CPU owner/尝试复写；以错误或旧copy-ticket查询 | Mapping保持存储至Unmap；复写/close busy；错误ticket拒绝 | 已完成旧ticket读取新/未完成数据 |
| A09 | 真pending工作→poll、零/有限deadline wait→稍后放行 | pending/timeout不退休；后续正常完成；事件唤醒后仍校验真实fence | timeout当完成、stale event假完成 |
| A10 | Close/录制失败、Execute前分配失败；Execute后Signal故障路径 | 不发成功ticket；未提交失败可撤持有，可能已提交则保留/停止新工作；旧有效数据不伪标新结果 | 部分提交被当作无副作用回滚 |
| A11 | 显式正常close；仍有Recording/Mapping/owner；有在飞timeout | busy类别明确；释放孩子后可关闭；timeout保留Device；违规销毁有明确诊断 | 析构无限wait、Device先死悬空孩子 |
| A12 | 独立进程真实device removal；owner/Recording释放与关停 | lost而非complete，fence最大值不授权Map/复用，新提交拒绝，有界退出 | `UINT64_MAX >= ticket`误判成功 |

计数诊断应覆盖逻辑live资源、未提交持有、in-flight批次/资源、pending退休与native释放，足够证明恰好一次和归零即可；不是新生产telemetry服务。不能用“没有崩溃”或COM::Release返回值替代整个所有权断言。验收从Module Interface进入，诊断只观察真实实现，不另写mock资源世界。

## 有界负向实验

**确定性pending**：不能用大量GPU负载或假fence强行撞时序。需要A05/A06/A09确定覆盖未完成分支时，可在仅验收可访问的窄后端入口用真实queue::Wait等待一个独立gate fence；仍经真实Module submit。在CPU释放gate后用真实完成fence核对。gate与生产completion fence严格分开；不得CPU Signal生产completion fence。实验设有限退出期限，不交付一般native逃生Interface或永久测试分支。

**提交故障**：参数/句柄失败走真实输入；native Signal/设备失败优先使用独立进程中的真实RemoveDevice场景，而非替换driver返回值。不同设备可能在不同调用首先报告lost，应以实际失败阶段记录，不硬编码期待某一个HRESULT。暂时无法安全触发的内存准备/驱动故障，明确保留其源级事务审查证据与动态覆盖缺口，由Main裁决验收边界；不能另写mock backend、伪造fence或将未经触发的分支计作实测PASS。

**设备丢失**：官方`ID3D12Device5::RemoveDevice()`可用于隔离负向进程；先QueryInterface，官方页面列最低Windows Build20348。预期GetDeviceRemovedReason失败、fence变UINT64_MAX、wrapper返回device_lost而非normal-complete。它验证失效分类/清理，不证明物理GPU故障全集、DRED全功能或自动恢复。禁止故意挂死GPU、修改TDR或影响其他运行中session。

## 执行与记录规则

现有probe已验证，不在文档任务中修改或重跑。实现session建立正式Module target后，Main按既有presets统一构建dev/release、执行矩阵并保留命令、源码/依赖hash、配置、exit code、debug状态、adapter/OS/驱动与逐场景结果。缺debug层的functional结果单列；`--require-debug`缺层失败不能被忽略。

最终报告分三栏：既有SDK证据、当次Module真实Interface证据、未覆盖/unsupported。只有Module门槛全部满足且没有把unsupported算成功，才能标记本里程碑完成。性能和自动设备恢复没有本轮验收承诺。

