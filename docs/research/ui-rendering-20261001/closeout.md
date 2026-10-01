# UI 样式与绘制阶段收口

日期：2026-10-01。状态：实现、双配置验收、review 和清理完成。阶段基线为 `8d774cf82b30d320a246246853e3359a4f6c83e9`；范围是基础 UI，未扩展业务语言或 Runtime 资源对象。

## 交付范围

- 接受帧持有文字段落和最终变换；绘制、选择、caret、IME 起点与指针命中共用文字快照。连续键盘编辑遵循接受帧的字体/宽度策略。
- C++ 与 SS 生成同一不可变样式表，支持类名、角色、状态、token、继承、作用域和属性覆盖；编辑器正式加载 `editor.ss`。
- 圆角、向内边框、RGBA 图片及底色在同一盒中合成。相邻兼容项使用实例合批，保留绘制顺序与资源退休合同。

当前用法和边界统一维护在 [SS 模块文档](../../modules/ui/style-sheets.md)，本目录其余研究文件保留实施前的观察。

## Review

两位 `gpt-6.1-sol` 分别按工程规范和已批准需求检查基线后的完整生产改动及新增文件；没有把源码 review 当作运行验收。

| 维度 | 发现与处理 |
| --- | --- |
| Standards | 1 项 P2：列表滚动上限仍读取结构 padding，遗漏 SS bottom padding。已改用与布局相同的 effective padding；真实 Session 场景覆盖仅由 padding 产生的溢出、滚到末尾、删除样式后的回正，以及 1×/1.5× DPI。定向复查通过。 |
| Spec | 未发现其他遗漏、范围扩张或合同错误。检查覆盖样式继承/缓存、接受帧文字、圆角合成、相邻合批与资源寿命。 |

结构清理删除了 `TextStore` 旧的当前段落几何查询包装；prepare 先取得一次快照，再查询 caret、字符与选择区。`TextStore::set` 直接接收完整 Font，删除重复 size/weight 参数，以及宽度可变时期遗留的无效分支和重复重置。现行文档中的旧 IME 状态、生产图片范围和跨工作树集成说明已同步，研究材料明确标为历史基线。

## 验收驱动修正

收尾窗口验收曾在数值预览断言失败。定向排入一条偏移 8 像素的原生 `WM_MOUSEMOVE` 后，旧驱动稳定表现为 `x=42`、历史 `10`（预期 `9`）：模拟 down 和 up 之间的轮询将移动解释成拖动并提交。

驱动现在连续投递完整点击后再推进窗口；排队移动在点击释放后处理。真实窗口回归保留该排队场景，并在输入数字前检查数值、历史与事务均未改变。拖动、按压截图、选择拖动及捕获取消仍逐步推进；生产输入代码未改变。定向源码复查通过，临时诊断输出已移除。

## 验收结果与证据

| 入口 | dev | release |
| --- | --- | --- |
| `ui_acceptance` | 15/15 PASS | 15/15 PASS |
| `editor_document_acceptance` | PASS | PASS |
| `atelier_editor --acceptance` | PASS，744 帧 | PASS，744 帧 |
| `renderer_acceptance` | PASS | PASS |

UI/编辑器在清理和修复后重新构建验收。renderer 复用同阶段已经通过的最终 GPU 版本证据；此轮清理未改变 renderer 或 shader。GPU 使用 AMD Radeon RX 6800 XT 与 D3D12 debug/GBV，包含颜色/深度读回、圆角边框、RGBA 行跨度/透明混合/版本回收和错误帧恢复。11 个可绘制项合为 7 次 draw，保持 A/B/A 顺序；这不是 GPU 耗时测量。

证据保存在 `out/ui-ss-evidence/`：

- `phase-{dev,release}-build.log`：UI 与文档消费者；后续驱动构建日志为 `phase-click-fix-build.log` 和 `phase-release-final-build.log`。
- `phase-{dev,release}-editor.log`、`phase-{dev,release}-editor/`：最终窗口报告、截图和既有采样。
- `dev-final-build.log`、`release-gpu.log`：最终 renderer GPU 验收。
- `phase-click-repro.log`：修复前定向复现，保留作失败证据。

真实系统 IME 没有在本阶段重跑；既有 S2 结论仅适用于其记录的版本和环境。后代裁剪与命中仍为矩形；图片解码、Runtime Texture 接线、图集、持久几何缓存和业务语言 UI 语法仍未实施。
