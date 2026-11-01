# 布局正式验收

`ui_layout_acceptance` 运行 `acceptance/ui_layout/main.cpp` 及其包含的 `practical.inl`、`scaling.inl`、`flex_grid.inl`、`audit.inl`、`topology.inl`。只有一个消费者入口，不创建窗口或 GPU 设备；文字度量使用真实 DirectWrite。

## 保留范围与独立真值

14 个场景按[验收保留标准](../../engineering.md#验收保留标准)检查以下代表，不展开方向、参数、规模或重复入口矩阵：

| 场景 | 类别与独立真值 |
|---|---|
| bounded_allocation | K3：总宽100，第一项min80、第二项max10，等权分配90/10 |
| slots_padding_and_columns | K3：宽108扣padding4和gap4，两个slot各50；fit20居中x17、第二x56、垂直y15 |
| known_width_natural_height | K3：独立创建的DirectWrite段落给出宽100自然高，以及字体版本变化后宽220自然高 |
| transactional_failure_and_reentry | K6：第三次测量失败后保留原publication、input revision、借用缓冲和拥有快照，解除注入后重试 |
| anchors_and_logical_precision | K3：800×450中80×40右下框和offset−10给出710/400；两层0.4偏移相加为0.8 |
| owned_geometry_lifetime | K6：输入2.25/10.25/5.5的快照跨后续发布及Document析构仍可访问 |
| shared_columns_structure_and_intrinsic | K3：两列最大值70/100，加gap10得180；行高20+30+3=53，第二列x80 |
| wrap_flow_and_repeated_grid | K3：宽199、最小slot60、gap10形成两列94.5；输入行高得尾项104.5/60、总高70 |
| virtual_range_and_anchor | K3：100项高20/gap2，scroll225、视口100、overscan22得[9,16)、origin−27、extent2198；首项增高20后scroll245 |
| flex_allocation_and_distribution | K3：basis200/100、shrink2/1分摊缺口60得152/88；两宽20+gap10的space_evenly余隙各50/3 |
| flex_wrapping_and_alignment | K3：70+10+70每行150，行高20，高120反向两行得首项80/100、次行y70 |
| grid_tracks_spans_and_placement | K3：固定60及gap20后220按1:2分配；行高30+6+20=56；同批两个重叠100宽双列span独立约束得50/50/50 |
| grid_collision_boundaries | K3：0..63占用，自动2×2块首空位64/0，锁column0下一位置y1 |
| final_tree_transactions | K3/K6：输入指定的祖先反转、后代逃逸与回收；叶高5/10得y5/y15，旧快照仍持有原父子关系 |

几何比较容差为 `0.0002 + 0.000002 × max(abs(a),abs(b))`；可精确表达的几何直接相等比较。DirectWrite 是布局组合的外部真值，不验证字体库自身，也不声称跨字体环境逐位一致。检查使用运行时 `require`，Release 同样执行。

构建与执行命令见[模块入口](README.md#构建与验收)。历史 PASS 和性能数据只适用于[审计记录](audit.md)及[历史性能](../../research/ui-layout-reassessment/native-performance-history.md)所列版本，不代表当前检出的运行结果；其中旧测量入口已移除。

布局库不拥有文字、绘制或 GPU 资源；生产 UI 的资源寿命由[UI 验收](../ui/README.md#限额与验收)负责。没有全分配点故障注入、并发/异步求解或非 Windows 构建验证。
