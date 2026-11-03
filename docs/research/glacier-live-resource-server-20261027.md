# Glacier / 007 的 Live 编辑与资源服务器

研究日期：2026-10-27。本文核对 IOI 明示的技术事实与证据限制，不推定其私有实现。本项目的当前消费链见 [Play 合同](../modules/editor/projects.md#play-副本与变更批次)，后续方向只维护于[资产路线](../modules/resources/asset-system-plan.md#单机资源服务待设计)。

## 可核实来源

1. **IOI CTO 的直接采访，2025-06-16。** TechRadar 作者 Ural Garrett 采访 Ulas Karademir，谈《007 First Light》及 Glacier 模块化时，原话包含 “separation between the networked engine, the engine editor, and the resource server”。这直接证实 IOI 描述了引擎、编辑器与资源服务器的分离。原文章页面的本次全文提取被订阅浮层遮挡；搜索索引与 Yahoo 的 TechRadar 署名转载吻合，已读取转载全文。来源：[原采访](https://www.techradar.com/gaming/007-first-light-io-interactive-hitman-interview-sgf-2025)、[署名转载全文](https://tech.yahoo.com/gaming/articles/007-first-light-developer-io-140000118.html)。这是第一方受访者的直接陈述，但不是协议文档。
2. **IOI 官方 Glacier 页面。** “Real-time results” 明确描述游戏运行期间跨平台/主机修改、更新和编辑，目标是即时 export/update/play/stop；同时强调自动资产工作流及简单、以用户为中心的工具。该页本身没有展开资源服务器协议。来源：[IOI Glacier](https://ioi.dk/glacier)。
3. **devcom 2025 官方议程。** IOI Senior Tools Programmer Nicolai Herrmann 的《Accelerating the Resource System Using a Virtual File System in the Glacier Engine》明确以 Hitman、Project Fantasy、007 的共享技术和增长的数据量为背景，介绍资源系统、虚拟文件系统与 “instant everything”，目标包括将加入项目后开始工作的等待从小时缩短到分钟。只读到官方摘要，未据此声称看过演讲全文或确认服务器内部实现。来源：[官方议程](https://bizcommunity.gamescom.global/widget/event/devcom-developer-conference-2025/planning/UGxhbm5pbmdfMjY4MTQ5OQ==)。
4. **IOI CTO 的直接采访，2024-06-21。** GamesIndustry.biz 的 GI Sprint 采访强调迭代速度、工具要与实际使用者互动、按游戏需要缩窄引擎范围。它支持“为创作效率建立责任分工”，不支持不经需求验证照搬整套大型基础设施。来源：[原采访](https://www.gamesindustry.biz/io-interactives-mission-to-speed-up-game-development-gi-sprint)，该采访也由 IOI 官方 Glacier 页面链接。
5. **Digital Foundry 原视频，2026-05-20。** 《Inside 007 First Light - A Deep Dive Into The Evolved Glacier Engine [Sponsored]》，发布者注明 IOI 赞助。原站章节为 13:38 编辑环境、16:24 节点编辑性能、19:50 BRICKs。已取得部分原站自动转录，未人工听音校对：[13:49](https://www.youtube.com/watch?v=m0SJ9i9RtRg&t=829s) 明确说编辑器由 editor 与 engine 两个进程构成，engine 就是游戏；[14:00](https://www.youtube.com/watch?v=m0SJ9i9RtRg&t=840s) 谈编辑模式下实体的内存和所有权在编辑器侧；[14:09](https://www.youtube.com/watch?v=m0SJ9i9RtRg&t=849s) 说明游戏崩溃后编辑工作保留，重启游戏即可恢复。这支持编辑作者状态与游戏运行进程隔离，但不能据此推导跨进程对象 ABI 或所有运行模式的所有权合同。[1:07:13](https://www.youtube.com/watch?v=m0SJ9i9RtRg&t=4033s) 提到 resource system / asset system，措辞不是 server。本次转录返回被截断，未完整取得目标章节；没有核实视频中资源服务器的职责，也不能将未读到解释为视频没有提到。

以上尚不能确认用户记忆中的具体视频就是某一场分享，也不能推出 IOI 的 SHA 算法、内容寻址格式、ACK 屏障、保留几代基线、资源事务粒度、完整部署拓扑或 GPU 采用规则。资源服务器的明确措辞由第 1 项采访支持；不要用第 5 项的 resource system 偷换证据。
