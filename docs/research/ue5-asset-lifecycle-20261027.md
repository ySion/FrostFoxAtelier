# UE5 资产现态、加载与消费寿命参考

日期：2026-10-27。目的：核对 UE 如何分离资产条目、加载对象、派生缓存和消费结果，为本项目的精悍资产方案提供证据，不直接更改产品合同。

## 研究基线与证据范围

- 使用本机已认证的 `gh api`，已成功读取私有仓库 `ySion/UnrealEngine-Angelscript` 的 `angelscript-master`。
- 固定提交为 [86158a28896e](https://github.com/ySion/UnrealEngine-Angelscript/commit/86158a28896e1d8af95671516f54c2e22dfe27e4)。[Engine/Build/Build.version](https://github.com/ySion/UnrealEngine-Angelscript/blob/86158a28896e1d8af95671516f54c2e22dfe27e4/Engine/Build/Build.version) 声明 5.8.4；这是该分支自报版本，不是对 Epic 正式发布版本的认证。
- 按路径读取源码，没有克隆整个引擎、构建或运行 UE。源码路径位于 UE 的常规模块，不据此声称每一行都与未修改的 Epic 上游相同。Asset Registry 的职责另与 Epic 官方文档交叉核对。
- 已核对目录、加载、DDC、材质失败路径、纹理重导入、事务 Undo 和公开资产删除入口。负责人复核了决定结论的原对象复用、bulk transaction、条件清空历史、编译失败和覆盖解析路径。没有运行 Ctrl+Z、失败重导入或材质失配场景，不把组合源码推断标作实测。

## 结论摘要

**借鉴 UE 的责任分离，不照搬它的规模和所有产品规则。** 静态目录、按需加载、稳定资产对象与可重建派生缓存符合用户方向；但 UE 的事务可以包含纹理大块源数据，删除有引用的资产可能改写引用或清空 Undo，材质编译失败有默认材质回退。它不等同于“只撤销参数、删除永远保留缺失键、失败实例固定旧完整版本”。

本项目不做导入/删除/Reimport Undo，是已经明确的自主收窄；不用为了对齐 UE 增加资产历史或 bulk Undo。旧帧寿命、失败保护、参数历史和持久资产历史分别定义。

## 1. 资产目录确实是未加载数据，不是一批已实例化对象

`FAssetData` 保存包名、包路径、资产名、类型路径和可搜索标签等信息。目录扫描路径为 `FAssetDataGatherer::ReadAssetFile → FPackageReader::ReadAssetRegistryData`，读取包内的 Asset Registry 区段并另读依赖信息，不必把每个资产加载成业务 UObject。[AR1]、[AR2]、[AR3]

Epic 的 [Asset Registry 文档](https://dev.epicgames.com/documentation/en-us/unreal-engine/asset-registry-in-unreal-engine)也明确：异步收集未加载资产信息，保存于内存，让 Content Browser 枚举而无需加载。文档示例有旧字段名，当前字段和签名以固定提交源码为准。

**可以借鉴：** 本项目已有 `ProjectEntry` 和 Reader 的职责方向正确；目录浏览不需要创建所有 Runtime 对象。资产索引可以维护当前状态、类型和真实依赖，不需要先建立历史版本图。

**不要混淆身份：** 本次查看的 `FAssetData` 相等判定使用 PackageName 与 AssetName；UE 这条资产路径机制不是本项目的独立 AssetId。可借鉴数据/加载分离，不必把现有 sidecar GUID 改成路径身份。`FAssetData` 的 transient 注释也不意味着 UE 不持久化资产注册表缓存；Gatherer 明确存在磁盘缓存，不能据一条注释推断整个索引无持久化。[AR1]、[AR4]

## 2. 加载与实例化不是同一个动作

`FAssetData::FastGetAsset` 先查已加载 Package；允许加载且包未加载时才调用 `LoadPackage`，最后查找该资产对象。`GetAsset()` 显式允许加载，`IsAssetLoaded()` 则不触发加载。[AR5]

因此常见资产读取会复用已有 UObject，不是每一次读取都 new 一份资产实例。需要独立游戏状态的对象/实例另有自己的创建和所有权合同。

`FSoftObjectPtr` 同时保存磁盘路径和弱对象状态；对象加载/卸载可使它在 Valid/Pending 之间转换，本身不阻止 GC。`LoadSynchronous()` 也是先取现有对象，缺失时才执行路径加载。[AR6]

**可以借鉴：** 作者引用是可在未加载或缺失时保留的数据；真正的加载、运行对象持有和 GPU 表示分别负责自己的寿命。对本项目而言，不能把 AssetRef 本身等同于 Runtime 根，也不必强制所有只读内容都先建立业务对象。

## 3. DDC 的内容键不等于资产修订历史

派生数据缓存接口以键查询记录/值/片段；`FCacheKey` 是 Bucket 与 Hash。`FCacheRecord` 明确要求同一个键不能复用于不同 values，metadata 则可以包含变化的生成信息。[DDC1]、[DDC2]

纹理的实际构建键路径包含来源标识、目标纹理格式及其版本、组合纹理来源、构建设置等。`UTexture::CachePlatformData` 比较所需派生键，复用匹配的平台数据或安排新的缓存/构建工作。这里证明的是构建条件与输出复用，而不是给用户列出可回退的 Texture 修订。[DDC3]、[DDC5]

一个不应照搬的同步细节：该函数在需要重建且已有 PlatformData 时先 `ReleaseResource()`，再 `FlushRenderingCommands()`，注释明确是等待仍访问 PlatformData 的旧 `InitRHI()` 完成后才修改它。它证明 UE 此路径也需要保护在途访问，但不是本项目“不阻塞帧循环、不可变 owner 延迟退休”的实现证明；FlushRenderingCommands 也不能被泛称为所有 GPU 工作都已完成。[DDC5]

**可以借鉴：** 资产身份稳定与派生产物键变化相容；旧缓存命中可因复用存在，不因此获得“必须保留供 Undo”的承诺。构建键也不能简单替代为文件名或单独 mtime。

这不能推出“UE 绝无资产历史功能”：源码管理、事务缓冲和外部版本控制是不同责任，本节只描述 DDC，不对未审查系统作否定结论。

## 4. UE 有本机缓存服务，但不是作者与运行对象的统一权威

该分支的 BaseEngine.ini 中，默认 DDC 图包含 ZenLocal，Zen 配置可自动启动本地服务；同时存在文件、Pak、共享和云节点的配置。`FZenCacheStore` 是 HTTP 缓存后端，实际发出 `PutCacheRecords`、`GetCacheRecords` 等 RPC。[ZEN1]、[ZEN2]

这些接口供给的是缓存记录与字节，不是“把运行中的 Texture UObject 迁移到服务端”，也不是 Editor/Play 共同使用一套对象指针。本文未审查 Zen 内部数据库、文件发布原子性或崩溃恢复实现，不根据客户端接口猜测其后端。

`ICache` 还明确允许请求乱序完成，回调可能来自任意线程、并发调用，甚至在请求函数返回前调用。因此缓存命中与完成通知不能直接充当作者操作的有序采用证明。[DDC4]

**可以借鉴：** 按需单机内容服务与精悍方案并不冲突；服务只承担有实际调用者的交付、缓存和请求寿命，作者历史与宿主采用仍留在客户端。不照搬 UE 的多后端图、云服务、构建调度或插件平台，也不能据 UE 使用 Zen 推出本项目必须选 LMDB/SQLite。

## 5. 材质并不是“合法 A 更新、失配 B 留旧完整结果”

### 新定义向依赖实例传播

`FMaterialUpdateContext` 收集已加载且依赖更新材质的 MI，先处理父 MI，再更新 cached data、uniform expressions 和静态置换资源，最后更新 draw lists 或组件渲染状态。这是保持渲染数据相容的传播机制，不是每个实例自行批准或拒绝新源版本的协议。[MAT1]

普通 MI 没有自己的静态置换资源时，渲染代理直接向当前父代理查询 shader 资源；有静态置换的 MI 可拥有自己的编译结果。它们并非一律各持一份可长期固定的完整材质历史。[MAT2]

### 参数移除/改型不天然阻止实例更新

关联/重命名路径按参数类型处理原覆盖，找不到声明时可以保留数组项，但并不因此返回一个阻止材质传播的失配错误。实际求值按 Scalar、Vector 等类型查询覆盖，未命中才走当前父链或当前 expression 的默认值。[MAT3]、[MAT4]

**源码推断，非运行验证：** B 原有 `Scalar Foo=0.2`，新定义改为 Vector Foo，且 B/父链没有 Vector Foo 覆盖，则旧 Scalar 不参与新 Vector 的解析；使用当前默认/父链值，而不是 B 保留旧完整材质。参数已经消失时，新逻辑也不会因为旧覆盖数据仍在就继续执行旧定义。删除 material layer 还有移除对应覆盖的路径，不能扩张为 UE 永不丢弃任何孤立覆盖。

本项目的策略只由[资源发布与默认替代](../modules/renderer/materials.md#资源发布与默认替代)定义，本页 UE 源码推断不替代本项目的运行证据。

### 编译失败有明确默认材质回退，不是必然保旧

异步编译失败时，`ShaderCompiler.cpp` 保存错误，执行 `MaterialsToUpdate.Add(Material, nullptr)`，并明确报告 Default Material 将用于游戏；之后该集合实际提交到材质资源。同步失败同样清空 map。[MAT5]、[MAT6]

编辑器 Apply 前也可因普通材质编译错误让用户选择 Continue/Abort。Abort 是整次编辑尚未覆盖回原材质，不是共享源已经更新后只让 B 拒收。[MAT7]

编译 pending 又是另一状态：可以安装编译中内容的 finalized clone；部分消费入口要求完整 map 并回退，另一些接受 incomplete map。不能笼统断言编译期间所有 pass 都保旧或都画默认材质。[MAT8]

**结论：** 此分支可独立推进不同置换，但失败 B 的默认行为不能解释成“旧完整 B 永久保留”。保留旧 shader 指针也不足以证明这一点，因为 BlendMode、ShadingModels、双面等属性还从当前 Material/MI 读取。[MAT9]

## 6. 旧资源寿命与新内容采用是两个问题

GT/RT 分别持有 shader map；结果提交先更新 GT，再通过渲染命令切换 RT 并重算相关 uniform。父对象 fence、显式延迟删除、shader map 引用计数、RenderCore cleanup 与 RHI pending-delete 分别保护相应层的在途访问。[MAT10]、[MAT11]

没有看到这条更新链要求所有引用者在同一帧完成业务级“接受”；但也不能由此推出任意实例可以继续固定旧源版本。旧资源迟些销毁只是安全寿命，不是编辑历史或版本拒收协议。

本次追到 RHI pending-delete，没有追完 D3D12/Vulkan 最终驱动 fence，也没有运行验证，因此不把渲染线程 flush 等同于全部 GPU 工作结束。

## 7. 重导入更新原资产，不自动建立旧版资产目录

### legacy 2D/Cube 的直接证据

`UReimportTextureFactory` 使用原 class、outer、name 调用导入，`CreateTexture2D/CreateTextureCube` 在匹配原对象类型时直接返回 `pOriginalTex`，随后新图像写入它的 Source。成功后更新导入信息并标记原包脏。[IMP1]、[IMP2]

因此在这些成功路径中，原 UObject 引用再读该对象时取得更新后内容，而不是自动成为旧图快照。结论限于已跟踪的同类型路径，不能推广到所有 importer、纹理类型或类型转换。既有渲染资源的在途寿命又是另一层。

`AssetImportData` 记录来源文件名、时间戳、MD5、显示标签；Update 更新当前来源记录。多条 SourceFiles 可以是 UDIM 等多输入，并不表示每次重导入追加一版历史。[IMP3]

### Interchange 也是实际路径

`FReimportManager::ReimportAsync` 可以设置 `ReimportAsset=Obj` 并调用 Interchange 的 `ImportAssetAsync`。实际纹理 factory 优先使用 `ReimportObject`，存在有效载荷检查、重导入设置策略，以及来源信息 backup/reinstate 钩子。[IMP4]、[IMP5]

这里同样看到更新当前资产的方向，但没有追完全部 Interchange 异步失败恢复，不能用 legacy 的失败细节概括它，也不能把用于重导入设置比较的 previous node 称为用户版本仓库。

### 不做 Undo 不代表失败处理可以省略

legacy manager 可在实际导入前改写来源路径，而所跟踪失败处理没有恢复它。返回 Failed、日志使用 atomic 或失败时调用 PostEditChange，都不足以证明全部状态回滚。[IMP6]

更深的 DDS 源码还存在 Source.Init 之后再因 mip 转换失败返回的分支；未构造输入确认可达性，本报告不把它说成已复现损坏。对本项目的启示是：**先准备、再发表，不用半成品破坏已提交文件，是需要明确实现的失败保护，不是“不提供 Undo”自然带来的属性。** 消费失败时显示默认与文件事务保护是两件事，不能据此继续要求旧完整材质长期显示。

## 8. UE 的 Undo 可以包含纹理源数据，不只参数

UE 事务由有效的 transactor、对象 `RF_Transactional` 等条件控制，`Modify → SaveToTransactionBuffer → SaveObject` 后通过对象 `Serialize` 保存状态；不是预先限定为“只保存 UI 参数”。[UNDO1]

**最直接的反例：** `UTexture::Serialize` 调用 `Source.BulkData.Serialize`；`FEditorBulkData::Serialize` 有明确 `Ar.IsTransacting()` 分支。某些未加载、非虚拟化 payload 会从磁盘读取 compressed payload 写入 Undo；源码甚至指出每次 metadata change 保存完整 8K 纹理 payload 会很快耗尽 Undo 栈。[UNDO2]、[UNDO3]

但不能反向说 UE 每次 Reimport 都会存一版贴图：是否记录由外层事务、对象参与和序列化路径共同决定；本次未遍历所有 UI/脚本调用者，也未运行 Ctrl+Z。Undo buffer 非持久、受预算淘汰，与长期可选资产版本库不同。[UNDO4]

引用参数的事务另有重要区别：序列化 UObject 指针时保存的是引用表索引，不递归深拷贝目标贴图的全部内容。因此只撤销 A→B 引用，并不自动撤销 A 随后的原位重导入。事务引用也可能保活对象，导致删除时必须处理历史引用。[UNDO5]

`FScopedTransaction::Cancel` 丢弃事务记录，不自动执行 Undo 恢复对象；不能拿 scope 或 Cancel 的名字替代失败原子性分析。[UNDO6]

**对本项目：** 不复制 UE 的通用对象/bulk Undo。保留用户明确要求的参数历史，恢复资产键后解析当前内容；导入、重导入、删除不入 Undo。已有场景结构和源码 Undo 不因这次资产范围收窄自动删除。

## 9. UE 删除资产与“保留缺失键”不是同一策略

已沿默认有确认 UI 的公开 `ObjectTools::DeleteObjects → DeleteItems → FAssetDeleteModel → SDeleteAssetsDialog` 跟踪，而非仅看一个底层函数名。[DEL1]

| 入口选择 | 已查行为 |
| --- | --- |
| 普通删除 | 待删除集合外仍有普通内存/磁盘引用时不按普通删除直接执行。 |
| 强制删除 | 对找到的引用做 null/特定类型 fallback 替换，再尝试删除；不保证所有未加载磁盘引用被原子重写。 |
| 替换引用后删除 | Consolidate 到选定资产，并用 redirector 处理旧路径；redirector 不是旧内容版本。 |

依据为删除模型决策、引用替换与资产 cleanup 的实际代码。[DEL2]、[DEL3]

当 UI 检测到 Undo 引用时，Delete/ForceDelete 会条件清空事务历史。底层注释还明确区分 actor/actor folder 的可撤销删除与 asset 删除；清理包括对象卸载、GC 和 package 文件删除。不能据此说所有删除都无条件清空历史，也不能把无 UI 的其他入口视为具有完全相同的保护。[DEL4]、[DEL5]

**对本项目：** “允许删除后保留作者键并诊断缺失，不复活文件、不清空无关参数历史”仍是可取的独立产品建议，但不是 UE 已证明的同一行为。采用身份数据而非历史里的内容对象，有助于定义这条较小的合同；实际在途 owner 仍安全收尾。

## 证据边界

上述源码支持资产目录与加载分离、派生缓存与作者历史分离，以及在途资源寿命与采用策略分离；不证明本项目需要 UE 的数据库、通用 bulk Undo、强删改写引用或条件清空历史。本项目方向由[资产路线](../modules/resources/asset-system-plan.md)维护。

本研究未运行 UE。仍未证明的范围包括全部重导入类型与外层事务入口、Interchange 全失败恢复、每个渲染 pass 及驱动最终 GPU fence。

## 源码证据

链接指向用户私有仓库的固定提交，需要相应仓库权限。

[AR1]: https://github.com/ySion/UnrealEngine-Angelscript/blob/86158a28896e1d8af95671516f54c2e22dfe27e4/Engine/Source/Runtime/CoreUObject/Public/AssetRegistry/AssetData.h#L158-L285
[AR2]: https://github.com/ySion/UnrealEngine-Angelscript/blob/86158a28896e1d8af95671516f54c2e22dfe27e4/Engine/Source/Runtime/AssetRegistry/Private/PackageReader.cpp#L715-L770
[AR3]: https://github.com/ySion/UnrealEngine-Angelscript/blob/86158a28896e1d8af95671516f54c2e22dfe27e4/Engine/Source/Runtime/AssetRegistry/Private/AssetDataGatherer.cpp#L5136-L5175
[AR4]: https://github.com/ySion/UnrealEngine-Angelscript/blob/86158a28896e1d8af95671516f54c2e22dfe27e4/Engine/Source/Runtime/AssetRegistry/Private/AssetDataGatherer.cpp#L199-L320
[AR5]: https://github.com/ySion/UnrealEngine-Angelscript/blob/86158a28896e1d8af95671516f54c2e22dfe27e4/Engine/Source/Runtime/CoreUObject/Public/AssetRegistry/AssetData.h#L515-L576
[AR6]: https://github.com/ySion/UnrealEngine-Angelscript/blob/86158a28896e1d8af95671516f54c2e22dfe27e4/Engine/Source/Runtime/CoreUObject/Public/UObject/SoftObjectPtr.h#L37-L92
[DDC1]: https://github.com/ySion/UnrealEngine-Angelscript/blob/86158a28896e1d8af95671516f54c2e22dfe27e4/Engine/Source/Developer/DerivedDataCache/Public/DerivedDataCacheKey.h#L91-L128
[DDC2]: https://github.com/ySion/UnrealEngine-Angelscript/blob/86158a28896e1d8af95671516f54c2e22dfe27e4/Engine/Source/Developer/DerivedDataCache/Public/DerivedDataCacheRecord.h#L65-L95
[DDC3]: https://github.com/ySion/UnrealEngine-Angelscript/blob/86158a28896e1d8af95671516f54c2e22dfe27e4/Engine/Source/Runtime/Engine/Private/TextureDerivedData.cpp#L419-L570
[DDC4]: https://github.com/ySion/UnrealEngine-Angelscript/blob/86158a28896e1d8af95671516f54c2e22dfe27e4/Engine/Source/Developer/DerivedDataCache/Public/DerivedDataCache.h#L46-L87
[DDC5]: https://github.com/ySion/UnrealEngine-Angelscript/blob/86158a28896e1d8af95671516f54c2e22dfe27e4/Engine/Source/Runtime/Engine/Private/TextureDerivedData.cpp#L3751-L3835
[ZEN1]: https://github.com/ySion/UnrealEngine-Angelscript/blob/86158a28896e1d8af95671516f54c2e22dfe27e4/Engine/Config/BaseEngine.ini#L2719-L2824
[ZEN2]: https://github.com/ySion/UnrealEngine-Angelscript/blob/86158a28896e1d8af95671516f54c2e22dfe27e4/Engine/Source/Developer/DerivedDataCache/Private/ZenCacheStore.cpp#L450-L490
[MAT1]: https://github.com/ySion/UnrealEngine-Angelscript/blob/86158a28896e1d8af95671516f54c2e22dfe27e4/Engine/Source/Runtime/Engine/Private/Materials/MaterialShared.cpp#L5096-L5180
[MAT2]: https://github.com/ySion/UnrealEngine-Angelscript/blob/86158a28896e1d8af95671516f54c2e22dfe27e4/Engine/Source/Runtime/Engine/Private/Materials/MaterialInstance.cpp#L235-L287
[MAT3]: https://github.com/ySion/UnrealEngine-Angelscript/blob/86158a28896e1d8af95671516f54c2e22dfe27e4/Engine/Source/Runtime/Engine/Public/Materials/MaterialInstanceUpdateParameterSet.h#L74-L231
[MAT4]: https://github.com/ySion/UnrealEngine-Angelscript/blob/86158a28896e1d8af95671516f54c2e22dfe27e4/Engine/Source/Runtime/Engine/Private/Materials/MaterialUniformExpressions.cpp#L1513-L1549
[MAT5]: https://github.com/ySion/UnrealEngine-Angelscript/blob/86158a28896e1d8af95671516f54c2e22dfe27e4/Engine/Source/Runtime/Engine/Private/ShaderCompiler/ShaderCompiler.cpp#L2277-L2441
[MAT6]: https://github.com/ySion/UnrealEngine-Angelscript/blob/86158a28896e1d8af95671516f54c2e22dfe27e4/Engine/Source/Runtime/Engine/Private/Materials/MaterialShared.cpp#L3778-L3816
[MAT7]: https://github.com/ySion/UnrealEngine-Angelscript/blob/86158a28896e1d8af95671516f54c2e22dfe27e4/Engine/Source/Editor/MaterialEditor/Private/MaterialEditor.cpp#L2764-L2859
[MAT8]: https://github.com/ySion/UnrealEngine-Angelscript/blob/86158a28896e1d8af95671516f54c2e22dfe27e4/Engine/Source/Runtime/Engine/Private/Materials/MaterialRenderProxy.cpp#L870-L908
[MAT9]: https://github.com/ySion/UnrealEngine-Angelscript/blob/86158a28896e1d8af95671516f54c2e22dfe27e4/Engine/Source/Runtime/Engine/Private/Materials/MaterialShared.cpp#L2128-L2155
[MAT10]: https://github.com/ySion/UnrealEngine-Angelscript/blob/86158a28896e1d8af95671516f54c2e22dfe27e4/Engine/Source/Runtime/Engine/Private/Materials/MaterialShared.cpp#L5730-L5814
[MAT11]: https://github.com/ySion/UnrealEngine-Angelscript/blob/86158a28896e1d8af95671516f54c2e22dfe27e4/Engine/Source/Runtime/RHI/Private/RHIResources.cpp#L46-L76
[IMP1]: https://github.com/ySion/UnrealEngine-Angelscript/blob/86158a28896e1d8af95671516f54c2e22dfe27e4/Engine/Source/Editor/UnrealEd/Private/Factories/EditorFactories.cpp#L5603-L5760
[IMP2]: https://github.com/ySion/UnrealEngine-Angelscript/blob/86158a28896e1d8af95671516f54c2e22dfe27e4/Engine/Source/Editor/UnrealEd/Private/Factories/EditorFactories.cpp#L3649-L3780
[IMP3]: https://github.com/ySion/UnrealEngine-Angelscript/blob/86158a28896e1d8af95671516f54c2e22dfe27e4/Engine/Source/Runtime/Engine/Private/EditorFramework/AssetImportData.cpp#L41-L227
[IMP4]: https://github.com/ySion/UnrealEngine-Angelscript/blob/86158a28896e1d8af95671516f54c2e22dfe27e4/Engine/Source/Editor/UnrealEd/Private/Editor.cpp#L554-L598
[IMP5]: https://github.com/ySion/UnrealEngine-Angelscript/blob/86158a28896e1d8af95671516f54c2e22dfe27e4/Engine/Plugins/Interchange/Runtime/Source/Import/Private/Texture/InterchangeTextureFactory.cpp#L1944-L2477
[IMP6]: https://github.com/ySion/UnrealEngine-Angelscript/blob/86158a28896e1d8af95671516f54c2e22dfe27e4/Engine/Source/Editor/UnrealEd/Private/Editor.cpp#L472-L619
[UNDO1]: https://github.com/ySion/UnrealEngine-Angelscript/blob/86158a28896e1d8af95671516f54c2e22dfe27e4/Engine/Source/Runtime/CoreUObject/Private/UObject/UObjectGlobals.cpp#L3207-L3233
[UNDO2]: https://github.com/ySion/UnrealEngine-Angelscript/blob/86158a28896e1d8af95671516f54c2e22dfe27e4/Engine/Source/Runtime/Engine/Private/Texture.cpp#L1086-L1112
[UNDO3]: https://github.com/ySion/UnrealEngine-Angelscript/blob/86158a28896e1d8af95671516f54c2e22dfe27e4/Engine/Source/Runtime/CoreUObject/Private/Serialization/EditorBulkData.cpp#L874-L1009
[UNDO4]: https://github.com/ySion/UnrealEngine-Angelscript/blob/86158a28896e1d8af95671516f54c2e22dfe27e4/Engine/Source/Editor/UnrealEd/Classes/Editor/TransBuffer.h#L68-L135
[UNDO5]: https://github.com/ySion/UnrealEngine-Angelscript/blob/86158a28896e1d8af95671516f54c2e22dfe27e4/Engine/Source/Runtime/CoreUObject/Private/Misc/TransactionCommon.cpp#L251-L338
[UNDO6]: https://github.com/ySion/UnrealEngine-Angelscript/blob/86158a28896e1d8af95671516f54c2e22dfe27e4/Engine/Source/Editor/UnrealEd/Private/EditorTransaction.cpp#L1373-L1455
[DEL1]: https://github.com/ySion/UnrealEngine-Angelscript/blob/86158a28896e1d8af95671516f54c2e22dfe27e4/Engine/Source/Editor/UnrealEd/Private/ObjectTools.cpp#L3077-L3230
[DEL2]: https://github.com/ySion/UnrealEngine-Angelscript/blob/86158a28896e1d8af95671516f54c2e22dfe27e4/Engine/Source/Editor/UnrealEd/Private/AssetDeleteModel.cpp#L280-L423
[DEL3]: https://github.com/ySion/UnrealEngine-Angelscript/blob/86158a28896e1d8af95671516f54c2e22dfe27e4/Engine/Source/Editor/UnrealEd/Private/ObjectTools.cpp#L3925-L4050
[DEL4]: https://github.com/ySion/UnrealEngine-Angelscript/blob/86158a28896e1d8af95671516f54c2e22dfe27e4/Engine/Source/Editor/UnrealEd/Private/Dialogs/SDeleteAssetsDialog.cpp#L820-L867
[DEL5]: https://github.com/ySion/UnrealEngine-Angelscript/blob/86158a28896e1d8af95671516f54c2e22dfe27e4/Engine/Source/Editor/UnrealEd/Private/ObjectTools.cpp#L2522-L2709
