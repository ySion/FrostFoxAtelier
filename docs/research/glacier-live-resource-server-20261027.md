# Glacier / 007 的 Live 编辑与资源服务器

研究日期：2026-10-27。用途是核对用户指出的技术来源并重审本项目 Live 的长期责任边界；不是新实施计划，不改变既有生产代码或授权。区分 IOI 明示事实、本地现状与本项目建议。

## 可核实来源

1. **IOI CTO 的直接采访，2025-06-16。** TechRadar 作者 Ural Garrett 采访 Ulas Karademir，谈《007 First Light》及 Glacier 模块化时，原话包含 “separation between the networked engine, the engine editor, and the resource server”。这直接证实 IOI 描述了引擎、编辑器与资源服务器的分离。原文章页面的本次全文提取被订阅浮层遮挡；搜索索引与 Yahoo 的 TechRadar 署名转载吻合，已读取转载全文。来源：[原采访](https://www.techradar.com/gaming/007-first-light-io-interactive-hitman-interview-sgf-2025)、[署名转载全文](https://tech.yahoo.com/gaming/articles/007-first-light-developer-io-140000118.html)。这是第一方受访者的直接陈述，但不是协议文档。
2. **IOI 官方 Glacier 页面。** “Real-time results” 明确描述游戏运行期间跨平台/主机修改、更新和编辑，目标是即时 export/update/play/stop；同时强调自动资产工作流及简单、以用户为中心的工具。该页本身没有展开资源服务器协议。来源：[IOI Glacier](https://ioi.dk/glacier)。
3. **devcom 2025 官方议程。** IOI Senior Tools Programmer Nicolai Herrmann 的《Accelerating the Resource System Using a Virtual File System in the Glacier Engine》明确以 Hitman、Project Fantasy、007 的共享技术和增长的数据量为背景，介绍资源系统、虚拟文件系统与 “instant everything”，目标包括将加入项目后开始工作的等待从小时缩短到分钟。只读到官方摘要，未据此声称看过演讲全文或确认服务器内部实现。来源：[官方议程](https://bizcommunity.gamescom.global/widget/event/devcom-developer-conference-2025/planning/UGxhbm5pbmdfMjY4MTQ5OQ==)。
4. **IOI CTO 的直接采访，2024-06-21。** GamesIndustry.biz 的 GI Sprint 采访强调迭代速度、工具要与实际使用者互动、按游戏需要缩窄引擎范围。它支持“为创作效率建立责任分工”，不支持不经需求验证照搬整套大型基础设施。来源：[原采访](https://www.gamesindustry.biz/io-interactives-mission-to-speed-up-game-development-gi-sprint)，该采访也由 IOI 官方 Glacier 页面链接。
5. **Digital Foundry 原视频，2026-05-20。** 《Inside 007 First Light - A Deep Dive Into The Evolved Glacier Engine [Sponsored]》，发布者注明 IOI 赞助。原站章节为 13:38 编辑环境、16:24 节点编辑性能、19:50 BRICKs。已取得部分原站自动转录，未人工听音校对：[13:49](https://www.youtube.com/watch?v=m0SJ9i9RtRg&t=829s) 明确说编辑器由 editor 与 engine 两个进程构成，engine 就是游戏；[14:00](https://www.youtube.com/watch?v=m0SJ9i9RtRg&t=840s) 谈编辑模式下实体的内存和所有权在编辑器侧；[14:09](https://www.youtube.com/watch?v=m0SJ9i9RtRg&t=849s) 说明游戏崩溃后编辑工作保留，重启游戏即可恢复。这支持编辑作者状态与游戏运行进程隔离，但不能据此推导跨进程对象 ABI 或所有运行模式的所有权合同。[1:07:13](https://www.youtube.com/watch?v=m0SJ9i9RtRg&t=4033s) 提到 resource system / asset system，措辞不是 server。本次转录返回被截断，未完整取得目标章节；没有核实视频中资源服务器的职责，也不能将未读到解释为视频没有提到。

以上尚不能确认用户记忆中的具体视频就是某一场分享，也不能推出 IOI 的 SHA 算法、内容寻址格式、ACK 屏障、保留几代基线、资源事务粒度、完整部署拓扑或 GPU 采用规则。资源服务器的明确措辞由第 1 项采访支持；不要用第 5 项的 resource system 偷换证据。

## 本项目现状与此前方案

- 现有 Live 是编辑器向独立 Play 副本转发操作。命令与 ACK 沿有序链路执行，4 MiB 消息、16 MiB 批次及 256 批/64 MiB 积压限制见 [play_link.hpp](../../src/application/play_link.hpp)。大命令已有私有文件 spill，不表示内容可以逃过批次限额。
- 现有材质更新主要传作者数据/资源键，再由 Play 的资源会话或目录解析；没有精确跨进程图片版本保证。见 [scene.cpp](../../src/editor/scene.cpp) 的 forward_material/apply_live，以及[既有合同](../modules/editor/projects.md#play-副本与变更批次)。
- 尚未实现的材质 Live r1 将完整 RGBA8 放入每次材质输入传输，一张 2048×2048 RGBA8 本身已占 16 MiB，加记录即超限。r2 用对端已确认基线复用未变图片，避免普通参数更新反复传图，但真正的新大图仍超限。该修订只是静态合同，不是运行修复。见[材质计划](../modules/resources/material-program-plan.md#资产编辑与单基线迁移)。
- 当前资产路线已区分作者、构建产物与运行消费，Texture 产物及本地 Cook 仍在后续阶段；“暂缓远程资产服务器”不应被解释为必须把全部本地资源字节永久塞进 Live 命令。见[资产路线](../modules/resources/asset-system-plan.md)。

## 长期判断：本项目建议，不是 IOI 内部实现的断言

应区分两种工作：编辑命令说明修改目标、作者值及所需精确内容版本；资源供给按版本提供不可变内容，并承担实际字节读取、传输、复用及寿命。游戏在对应内容与必要 GPU 准备完整后采用，再确认操作结果。

由此，命令队列限制不必等于贴图体积上限。大资源可以通过另一个有明确字节/并发/驻留预算的路径准备，完成后一次采用；这是显式责任分工，不是把图片藏入不计数的私有文件。资源键仍表示身份，精确版本仍须单独确定，不能退回读当前目录最新版。

首步可限定为同机、版本固定的内容供给，复用既有 Reader/不可变 Image/缓存能力；是否需要独立进程、远程分发、分布式构建或虚拟文件系统，由实际跨设备和生产需求决定。当前 RGBA8 内容可作为第一种输入，不为解决 Live 就强制提前完成通用 Cook 或纹理压缩管线。

若采用这一分工，应重新评估 r2 的“唯一已确认基线兼任图片复用集合”是否仍必要，而不是无条件叠加一个资源服务器。精确版本、失败不混版、准备后采用、旧内容按实际使用期保留依然必要；具体单基线、单在途与 ACK 如何限制资源缓存，不应被当作长期必然合同。

这份研究只建议重新审视边界，不授权修改生产实现、持久格式、现行计划或启动新基础设施。真实延迟、连续预览、取消/撤销、大纹理更新、断开和采用失败仍须后续实测。
