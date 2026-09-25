# RHI 首里程碑契约

状态：下一 session 的实现契约；Module 未实现，具体 C++ 拼写可调整。范围与非目标以 [README](README.md) 为准，外部机制依据见 [research](research.md)。本文件集中定义可观察行为，验收通过公开 Interface 进行。

## 1. 执行域与身份

- 一个 Device 对应一个 native device、一条 DIRECT queue、一条由该 queue 独占发信号的 fence 时间线、一个 owner thread。所有改变状态及 poll/wait/释放操作在 owner thread 执行；复制 ticket 数值不授权跨线程访问 Device。
- Device 是明确的生命周期拥有者，覆盖其资源 owners、recordings、映射及在飞记录。首版不支持并发提交、跨线程销毁、共享句柄导入或外部原生 queue 注入。
- 建议资源标识为 device域标识+slot+generation，texture/readback种类可静态或动态区分。它不是 Runtime ObjectId、持久资产ID或 GPU地址。每次进入公开 Interface 检查 device、generation、种类、逻辑有效性和用途。
- move-only ResourceOwner 是一个 CPU 所有权；可复制 ResourceId 是非拥有定位值。释放最后 CPU owner 后，旧 ResourceId 立即禁止发起新操作；已持有该资源的 Recording/in-flight 记录仍合法，物理对象尚可存活。槽位只有所有内部持有与映射结束后才可复用，复用改变 generation；耗尽不能回绕成旧身份。
- Ticket 为 device域/世代+严格递增的非零 serial，不拥有 GPU 资源。正常 serial 不得使用 UINT64_MAX，不可回绕。设备重建如将来发生，必须产生新设备世代；旧 ticket 不绑定新设备。

## 2. 最小 Interface 方向

| 操作 | 输入/输出与完成含义 |
| --- | --- |
| 创建 Device | 明确硬件 adapter选择或显式WARP；返回实际adapter信息/支持范围/debug状态。硬件失败不自动软件回退 |
| 创建 Texture | 有限正尺寸、RGBA8_UNORM、2D/单mip/单采样、render-target/copy-source用途；返回owner及非拥有标识 |
| 创建 ReadbackBuffer | 固定可接收的宽高/格式与计算后的footprint；返回owner。长度/乘法/设备限制先验证，不截断溢出 |
| begin_recording | 最多一个未提交Recording；创建可失败，成功后独占录制槽。GPU可仍执行之前已提交批次 |
| clear / copy_to_readback | 先校验资源，首次引用即获得内部持有；值参数在本次调用消费，调用方临时地址不跨调用保存 |
| submit | 成功消费Recording并交付Ticket；具体失败阶段见第4节。调用方无需另交资源生命列表 |
| cancel / Recording析构 | 未提交录制被放弃，不执行其中命令；撤销内部持有，不改变最后已提交资源状态 |
| poll / wait(deadline) | 区分pending/complete/timeout/device_lost及错误ticket；wait有限且只在宿主明确调用时发生 |
| map_completed | 检查buffer与对应copy ticket、完成及设备状态后返回只读RAII行视图；不能以Map隐式等GPU |
| collect_retired | 读取真实完成状态并释放合格内部持有；不执行业务回调、GC或JIT代码 |
| close(deadline) | 显式关停，结果和保留责任见第7节；析构不替代有界关停协议 |

C++本地可使用std::expected等明确结果；错误保留操作阶段、分类与原始HRESULT，不把C++容器布局当语言ABI。OOM沿工程已有边界，不承诺可恢复所有驱动/容器分配失败；资源owner/取消/退休析构必须不抛出、不隐式等待，也不为排队退休新分配内存。

## 3. 从录制到退休的持有链

1. **未引用**：资源由CPU owner持有。Device保存真实resource/footprint/状态信息，但登记存在不等于无限保活。
2. **录制**：首次clear/copy引用时，Recording取得texture/readback及必要native记录资源的内部持有。调用方之后释放CPU owner只使外部ID失效，不能破坏该已录制工作。
3. **取消**：撤Recording持有；若没有其他持有则释放物理对象。未提交命令不修改GPU状态，取消不得把“录制后的预计状态”写回device的最后提交状态。
4. **提交**：在调用ExecuteCommandLists前准备好不会失败的in-flight记录容量；将引用资源、相关allocator与必要描述符存储的持有移交Device。成功后Recording被消费，不能再次提交。
5. **在飞**：Device拥有批次，Ticket仅查询完成。调用方丢Ticket、释放资源owner或销毁Recording外壳均不撤该持有。资源状态按同queue提交顺序推进；后续录制所见状态来自最后已提交状态。
6. **完成**：只有实际fence完成覆盖ticket且设备未lost，才撤该批次持有/复用allocator。资源可由更晚批次继续持有；不能按“某一旧ticket已完成”释放后续还在用的资源。
7. **退休**：无CPU owner、无recording/in-flight/mapping持有才物理释放/回收slot。登记generation与fence serial是不同概念。不得只释放texture而提前复用其命令allocator或GPU可见descriptor区间。

首版单未提交Recording避免乱序录制状态合并；仍允许多个已提交批次在飞。内部COM引用/表计数/批次容器由实现者选择，不需要通用反射Handle框架。定量诊断用于证明持有/释放，不将COM::Release返回值当稳定引擎资源计数。

## 4. 提交与失败保证

- 参数、设备能力、句柄/用途和Recording状态不合法时，在副作用前拒绝。若某次编码/Close失败导致native list不可再用，该Recording转失败状态，只允许取消；之前已提交工作不受影响。
- 准备期间可失败的分配在Execute之前完成；失败不得交付有效Ticket，不遗留本次未提交的in-flight持有。首版可消费失败Recording并取消，错误结果必须明确其已不可重试，不能隐式再次提交半个list。
- `ExecuteCommandLists`为void，没有可用的同步“本批一定执行成功”HRESULT。Close成功和queue::Signal成功只建立后续完成证据，不直接算GPU完成。
- Execute已发生、Signal失败时，不能回滚释放为“没提交”。Device进入faulted状态停止新工作，保留该批次持有；若GetDeviceRemovedReason确认lost，按lost清理；否则保留直到后续安全处置。此时不得交付成功Ticket，也不得把资源/allocator回池。
- 不用CPU `ID3D12Fence::Signal`冒充GPU完成。生产完成fence仅由本queue写入；测试中的独立gate fence不是该时间线。
- 任何方法发现UINT64_MAX或GetDeviceRemovedReason失败，先进入lost，再处理ticket；`completed >= serial`必须在lost判定之后。

## 5. 完成、回读与借用

`poll`只观察状态并可驱动合格退休。`wait`接受明确期限，事件被唤醒后重新检查fence与device，不能把旧事件信号或device-removal唤醒当完成。timeout是暂时未知，不允许回收在飞资源；调用方保留Device并可稍后poll/wait。

ReadbackBuffer记录最后成功提交的写入ticket及格式/尺寸/footprint。查询旧ticket、别的buffer ticket、未提交/失败ticket均拒绝。映射期间禁止新的copy覆盖此buffer；在先前copy尚未完成时也拒绝并发重写，以保持首版单结果所有权简单。复用一个buffer的新copy使旧结果查询失效，不声称保存所有历史结果。

只有写入完成后才Map。行视图按offset/row_pitch/有效row_bytes迭代，不假设连续width×height×4；不把padding字节当颜色内容。只读Mapping拥有必要保持关系，释放对应Unmap；其存活期间CPU owner释放仍不使借用悬空。Device关停必须等Mapping释放，而非偷偷使其指针失效。

## 6. Device lost与错误范围

Device状态至少可区分ready、closing、closed、faulted/lost；资源/操作错误不等于device永久lost。对lost设备，所有尚未确认完成的ticket返回device_lost，创建/录制/submit/map拒绝；释放owners和取消recordings仍允许，用于正常清理。曾经完成的ticket不能授权在lost后重新映射失效设备资源。

记录首个故障阶段/HRESULT、GetDeviceRemovedReason、adapter与debug状态。DRED仅在接口/设备能力可用且实际采集时声明，首版不依赖它才能失败返回。诊断不执行JIT回调，不要求活动业务对象存活。

device_lost允许进入终态清理，不代表命令正确完成；timeout或Signal失败而未确认lost不能走同一释放捷径。透明设备恢复不在范围；若以后重建，由上层自有CPU/资产数据恢复内容，旧resource/ticket永久失效。

## 7. 显式关停与CPU析构

宿主顺序：停止业务产生新GPU工作→释放/取消Recording→释放Mapping和CPU资源owners（例如Runtime包装对象先撤owner）→`close(deadline)`等待剩余已提交批次→完成退休→销毁Device。Device本身晚于这些CPU孩子，整个期间保持owner thread。

`close`进入closing后拒绝新创建/record/submit，但允许必要释放、poll/wait。仍有未提交Recording/Mapping/CPU owners时返回busy并指出持有类别；不静默取消调用方的有效借用。已无CPU孩子时，正常close在全部GPU持有安全退休后返回成功closed；timeout返回且保持Device/记录有效，可继续等待；确认lost则清理并返回device_lost而非成功执行。

首版推荐明确Device不可复制/移动、析构只允许无CPU孩子且无在飞记录的静止状态。违规销毁属于宿主生命周期错误，应在释放native存储前清楚诊断并fail-fast，而不是暗中无限等待或继续造成悬空借用；该策略必须在公开注释写出。若实现采用共享control block达到同等安全，可调整内部机制，但超时仍须有明确活着的拥有者，不能靠永久泄漏/孤儿全局池伪装通过。

native ResourceOwner析构只撤CPU持有、使外部ID失效或链接已预留退休记录，不调用wait。业务GC可在任意合规host安全点回收包装对象，其时间线与GPU完成独立。Runtime::~Runtime、显式destroy、JIT静止点都不是GPU排空操作。

## 8. 上层Seam

RHI不链接Runtime/LLVM，也不访问UI树、字体测量或窗口。未来语言Adapter通过现有共享业务类型定义包装ResourceOwner；非拥有资源ID不被GC当业务根，失效ID在RHI入口诊断。跨JIT调用不暴露ComPtr、STL对象或永久保存借用地址；完成事件由宿主在原有执行准入下消费，不把JIT函数裸地址挂在in-flight记录。

未来presentation Adapter才接platform拥有的HWND、surface世代和resize/close；layout→composition产生值快照/绘制输入。RHI只处理原生图形坐标规则，不反向持Node/业务root，不在每个child.width再次round。

