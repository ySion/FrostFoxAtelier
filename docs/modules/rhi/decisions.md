# RHI 与 RDG 实施决策

本文件维护当前硬件抽象与渲染依赖图的选型及理由；行为契约以 [contract.md](contract.md) 为准，运行证据以 [acceptance.md](acceptance.md) 为准。

## 已落地选择

| 选择 | 理由 | 维护入口 |
| --- | --- | --- |
| 固定 NVIDIA NRI + 局部补丁替代旧 `gfx` | 原生暴露 D3D12 Enhanced Barriers、SM 6.6 Direct Heap Indexing、Mesh Shader、DXR 1.1 Inline RayQuery、Placed Resource 堆别名与三队列时间线，同时为中远期接入 Vulkan 保留同一 C++ 接口边界 | [dependencies-lock.json](../../../cmake/dependencies-lock.json)、[atelier.patch](../../../third_party/nri/atelier.patch) |
| 进程单 `rhi::Device` + 多 `WindowSurface` | 编辑器主窗口与独立弹出菜单共享同一设备、PSO 池与纹理/几何缓存，消除多套 D3D12 设备与重复着色器编译开销 | [rhi.hpp](../../../modules/render/rhi/include/rhi/rhi.hpp) |
| 全局唯一 SM 6.6 Bindless `PipelineLayout` | 16-DWORD 根常量 + `ResourceDescriptorHeap` / `SamplerDescriptorHeap` 彻底消除按材质/通道拼装根签名、CPU 描述符拷贝风暴及 64-DWORD 根预算回退复杂度 | [rhi.cpp](../../../modules/render/rhi/src/rhi.cpp) |
| RDNA 2 安全描述符索引守卫 `atelier_safe_desc` | AMD RDNA 2（`amdxc64.dll`）会将条件分支内的统一标量 `ResourceDescriptorHeap[idx]` 提升至 wave 序言做 `s_load_dwordx8`，并将 `idx << 5` 视为有符号 32 位偏移（`0xFFFFFFFFu << 5 == -32`），导致未映射页读取故障；掩码至堆容量 `[0, 262143]` 在零分支开销下根除投机越界 | [common_bindless.hlsl](../../../modules/render/renderer/shaders/common_bindless.hlsl) |
| 线性 Arena 编译期 `atelier_rdg` | 将 Enhanced Barriers、跨队列围栏、死通道裁剪、Placed Resource 瞬态内存别名与区间并集 GPU 计时收敛在一处，业务通道不再手工维护资源状态位 | [rdg.cpp](../../../modules/render/rdg/src/rdg.cpp) |
| 后台 DXC 工作线程池 + GBV 预热 + 自有 DXIL 缓存 | UI/主线程不阻塞于编译或首次 GBV 着色器打桩；缓存键覆盖编译器版本、参数与完整源码并做 SHA-256 校验 | [shader_compiler.cpp](../../../modules/render/rhi/src/shader_compiler.cpp) |

## 补丁与依赖维护

[third_party/nri/atelier.patch](../../../third_party/nri/atelier.patch) 基于锁定的上游 NRI 归档生成，[cmake/rhi.cmake](../../../cmake/rhi.cmake) 在配置阶段保存 `.atelier-upstream` 原始副本并校验重放，绝不在旧修改上叠加：

1. 将编译链接的 `AgilitySDK` 指向工程锁定的头文件与 `D3D12Core.dll`（导出 `D3D12SDKVersion` 与 `D3D12SDKPath`），解除上游 CMake 对外部联网拉取的依赖。
2. 修正 D3D12 Enhanced Barriers 下加速结构构建/读取的 `BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_*` 与 `Compute / RayTracing` 阶段映射，以及 Swapchain / Placed Resource 包装接口所需的原生访问入口。
3. 移除上游强制附加的特定 CPU 指令集开关，保持工程统一的 Windows x64 编译基线，并将许可证副本集中部署至 `out/<preset>/licenses/rhi/`。

## 当前范围

当前生产构建仅开启并验收 Windows x64 D3D12 Ultimate 后端；Vulkan 后端不在本阶段编译或宣称已验证。不引入 `D3D12 Work Graphs`（保持与中远期 Vulkan 间接派发模型一致）。设备热恢复与 HDR 交换链不在当前交付范围内。
