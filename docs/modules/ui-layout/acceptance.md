# 布局正式验收

`ui_layout_acceptance` 运行 `acceptance/ui_layout/main.cpp` 的单个消费者，不创建窗口或 GPU 设备。

## 保留范围与独立真值

按[验收保留标准](../../engineering.md#验收保留标准)只保留以下两个代表，不展开方向、参数、规模或重复入口矩阵：

| 场景 | 类别与独立真值 |
|---|---|
| bounded_allocation | K3：总宽100，第一项min80、第二项max10，等权分配90/10 |
| owned_geometry_lifetime | K6：上述结果的拥有快照在 Document 析构后仍为三个盒、宽90/10 |

可精确表达的几何直接相等比较。检查使用运行时 `require`，Release 同样执行。共享列、Flex/Grid、虚拟列表、DirectWrite 自然高度、失败保旧与最终树事务目前没有专项代表。

构建与执行命令见[模块入口](README.md#构建与验收)；当前成本边界见[生产合同](contract.md#6-资源错误与成本)。

布局库不拥有文字、绘制或 GPU 资源；生产 UI 的资源寿命由[UI 验收](../ui/README.md#限额与验收)负责。没有全分配点故障注入、并发/异步求解或非 Windows 构建验证。
