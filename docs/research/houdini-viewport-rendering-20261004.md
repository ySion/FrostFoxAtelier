# Houdini 场景视口：线框与 MatCap

研究日期：2026-10-04。仅研究官方资料，未修改代码或执行验收。在线帮助页眉为 Houdini 22.0；20.5 官方 Foundations 教程亦列出下述模式。不据此推断用户安装版本或首次引入版本。[当前帮助](https://www.sidefx.com/docs/houdini/basics/view.html)、[20.5 教程](https://media.sidefx.com/files/tutorials/h205-foundations-welcome/01_welcome_tutorial.pdf)

## 正式模式与效果

以下名称按官方教程大小写整理；在视口工具栏 Shading 菜单切换。[模式说明](https://www.sidefx.com/docs/houdini/basics/view.html)

| 模式 | 已确认效果 |
| --- | --- |
| Wireframe | 显示曲线、曲面等参线、多边形边；纯线框不显示面 |
| Wireframe Ghost | 被表面遮住的线变淡 |
| Hidden Line Invisible | 名称确认；当前帮助效果栏为空。官方员工说明 Hidden Line 隐藏正常不可见多边形的线 |
| Hidden Line Ghost | 在 Hidden Line Invisible 基础上，将遮挡线改为淡显 |
| Flat Shaded | 多边形之间不混合着色，保留面片感 |
| Smooth Shaded | 混合相邻多边形着色，显示更平滑 |
| Flat Wire Shaded / Smooth Wire Shaded | 对应表面着色上叠加线框 |
| MatCap Shaded | 用 MatCap 纹理着色，忽略其他材质赋值与场景灯光 |
| MatCap Wire Shaded | MatCap 表面上叠加线框 |

纯线框不显示面的补充来自 [Display Options](https://www.sidefx.com/docs/houdini/ref/windows/displayopts_3d.html)。Hidden Line 的补充来自 SideFX Staff `malexander` 的[官方论坛解释](https://www.sidefx.com/forum/topic/71298/?page=1)。该帖普通用户关于两种 Hidden Line 的面填充、背景图与网格穿透差异，本轮没有足够第一方说明，不作为已确认契约。

## 三个维度不要混为一谈

**着色与线框方式**决定面片感、平滑、边的可见性及叠加；**照明方式**是独立控制，当前帮助区分 No Lights、Work Lights、Scene Lights。工作灯可选 Headlight、Dome、Physical Sky、Three Point。**材质显示/覆盖**另有控制。因此 Smooth Shaded 不是“PBR模式”，线框也不是一种灯光。[视口工具栏](https://www.sidefx.com/docs/houdini/basics/view.html)

MatCap 有两种正式入口：Shading 菜单的 MatCap 模式对整个视图覆盖其他材质；`D → Display Options → Material → Default Material → MatCap` 则为未赋材质几何选择默认材质，并设置 MatCap Texture 与 Intensity。两者作用范围不同。[默认材质参数](https://www.sidefx.com/docs/houdini/ref/windows/displayopts_3d.html)

MatCap 纹理包含固定照明和材质信息，不响应 Work Lights 或 Scene Lights；Houdini 提供默认贴图。它适合检查形体，不承担真实场景照明响应。[Advanced Viewport Shading](https://www.sidefx.com/docs/houdini/basics/viewporteffects.html)

## 内置能力与历史 Labs 节点

正式视口 MatCap 已在 20.5 教程和当前帮助中明确存在，不需要把第三方插件当作该功能来源。另一个 `Labs MatCap Shader` 属于 SideFX Labs；当前节点帮助标为隐藏且不再支持。该旧节点文档中的“不支持 high quality viewport modes”限定应留在该节点上下文，不能套到当前正式 MatCap 模式。[Labs 节点](https://www.sidefx.com/docs/houdini/nodes/vop/labs--matcap_shader.html)

官方 shader properties 还提供 `ogl_use_emission_matcap`：将 emission map 按法线而非 UV 采样，配合关闭照明和清除其他分量形成 MatCap 材质；这是材质属性入口，不是另一个线框模式。[Shader properties](https://www.sidefx.com/docs/houdini/props/material.html)

## 可用于项目方案的判断

以下保留研究时形成的项目建议，不是 Houdini 的出厂默认值结论。其后 MatCap、实体着色、边线及视口控件已在待验分支实现；当前合同与验证状态分别见[Renderer 合同](../modules/renderer/contract.md#视口显示策略)和[验收记录](../modules/renderer/acceptance.md#视口显示增量)。GPU/窗口项仍待许可与实际执行。

首个完整增量建议提供 MatCap、基础实体着色（Smooth/Flat），以及线框叠加、纯线框和隐藏不可见边。默认可选中性陶土 MatCap；保留对象基础色的实体模式，方便场景辨识；Ghost 淡显遮挡边可后续补充。界面提供快捷预设，内部组合表面着色、边线显示与遮挡政策，继续通过同一个 `Renderer::render(Frame)` 消费场景。视口显示设置由应用持有并随帧提交，切换模式不改 World 材质数据。[当前 Renderer 边界](../../src/renderer/renderer.hpp)、[应用接线](../../src/application/application.cpp)

MatCap 根据相对相机的法线方向采样一张已包含明暗与材质外观的图片；本身不能计算物体间投影、接触遮蔽或真实场景反射。这些限制是由采样机制得到的判断；若后续需要接触层次，可另行评估 AO。预期无需场景灯或阴影贴图，但实际帧耗时仍需在真实场景测量。[采样原理](https://docs.blender.org/UATEST/manual/en/4.5/render/workbench/lighting.html)

研究开始时 [Mesh](../../src/geometry/mesh.hpp) 只有连续三角形的位置和法线，[场景 shader](../../src/renderer/shaders/scene_ps.hlsl) 为固定方向的基础光照。MatCap 可复用几何与法线链；线框存在必须明确的语义：GPU 三角形边不等于原始多边形边，当时方块每面三角化产生的对角线没有独立的显示标记。针对干净的多边形线框，后续实现已让生产者保留三角形边标记，随同一不可变 Mesh 发布。原始拓扑已经丢失时不能可靠重建，不用共面猜测冒充拓扑，也无需因此先建设完整建模内核。

可观察验收关注：同一场景切换各模式，远近缩放下线宽和抗锯齿稳定、遮挡正确、叠线无明显深度闪烁；硬/软法线、非均匀缩放、实例共享与选择状态正确；模式切换沿既有资源缓存和退休路径。渲染质量优先做好这些基本项，再由真实场景需求推动 AO、灯光、阴影或 PBR。当前集成后的 GPU 矩阵/mesh 复验仍未执行，本研究不更改其状态。

官方资料未在本轮确认某个模式为所有版本的首次启动默认；也未证实 Houdini 内部渲染算法或 GPU 开销，不能把视觉模式直接当作实现方案。
