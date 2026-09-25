# Godot / GDScript：具名脚本与名称查找事实核查

调查日期：2026-09-21。本文只记录 Godot/GDScript 的一手资料事实，用作 FrostFoxAtelier 的名称设计参照，不把 Godot 的编辑器工作流外推为项目契约。

## 证据范围与限制

- 官方稳定文档：[GDScript reference](https://docs.godotengine.org/en/stable/tutorials/scripting/gdscript/gdscript_basics.html)，于本日期读取。文档的“stable”随 Godot 发布演进，故用下述固定源码核验具体实现。
- 源码：[Godot `4.5-stable` tag（提交 `876b290332ec6f2e6d173d08162a02aa7e6ca46d`）](https://github.com/godotengine/godot/tree/876b290332ec6f2e6d173d08162a02aa7e6ca46d)。
- 这是静态源码和文档核查，未创建或运行 Godot 工程；不据此承诺编辑器缓存刷新、语言服务器提示时机或所有版本的逐字诊断文本。

## 关键发现

| 议题 | 一手资料结论 |
| --- | --- |
| `class_name` 的性质 | `class_name` 是**显式声明**：官方文档称它把脚本注册为新类型；未具名脚本可通过文件路径加载，不因此获得全局类名。具名类才会“globally registered”。[官方文档](https://docs.godotengine.org/en/stable/tutorials/scripting/gdscript/gdscript_basics.html#registering-named-classes)；固定源码中 `ScriptServer` 以名称为键保存 `global_classes`，记录路径、基类和语言，[注册表实现](https://github.com/godotengine/godot/blob/876b290332ec6f2e6d173d08162a02aa7e6ca46d/core/object/script_language.cpp#L2643-L2708)。 |
| 为什么可直接写裸类型名 | 仅对已经 `class_name` 注册的类，官方文档明确说明其他脚本可直接使用、无需 `load` / `preload`。分析器在解析类型时才查询 `ScriptServer::is_global_class(first)`，并按登记路径加载对应脚本；`extends SomeClass` 同样先查询该表。[文档示例](https://docs.godotengine.org/en/stable/tutorials/scripting/gdscript/gdscript_basics.html#registering-named-classes)；[类型解析](https://github.com/godotengine/godot/blob/876b290332ec6f2e6d173d08162a02aa7e6ca46d/modules/gdscript/gdscript_analyzer.cpp#L3457-L3495)；[继承解析](https://github.com/godotengine/godot/blob/876b290332ec6f2e6d173d08162a02aa7e6ca46d/modules/gdscript/gdscript_analyzer.cpp#L2925-L2968)。 |
| 不是“扫描到全项目所有类型” | 正确因果是“带 `class_name` 的脚本进入全局类登记表”，不是任意 `.gd` 文件都自动成为可裸名引用的全局类型。引擎初始化从 `ProjectSettings::get_global_class_list()` 逐项装载登记表；而文档要求未具名脚本走路径引用。[初始化路径](https://github.com/godotengine/godot/blob/876b290332ec6f2e6d173d08162a02aa7e6ca46d/core/object/script_language.cpp#L2444-L2489)；[未具名类规则](https://docs.godotengine.org/en/stable/tutorials/scripting/gdscript/gdscript_basics.html#classes)。编辑器可以维护这份显式登记的元数据，但这不等价于语言把项目中全部声明纳入全局查找。 |
| 未限定名的作用域与遮蔽 | 对类型解析，固定源码先尝试局部常量/局部名称，随后再处理内建类型、原生类、最后是全局脚本类；局部常量若能代表脚本可作为类型使用。[局部解析](https://github.com/godotengine/godot/blob/876b290332ec6f2e6d173d08162a02aa7e6ca46d/modules/gdscript/gdscript_analyzer.cpp#L3308-L3349)；[后续全局查找](https://github.com/godotengine/godot/blob/876b290332ec6f2e6d173d08162a02aa7e6ca46d/modules/gdscript/gdscript_analyzer.cpp#L3351-L3495)。同时，类声明若遮蔽内建、原生、已登记全局脚本类或 autoload，分析器会产生错误，例如 `Class "%s" hides a global script class.`。[冲突诊断分支](https://github.com/godotengine/godot/blob/876b290332ec6f2e6d173d08162a02aa7e6ca46d/modules/gdscript/gdscript_analyzer.cpp#L2816-L2831)。 |
| 同名的结构性后果 | 全局类注册表是以单个名称为键的映射，同一短名在该表中只能取得一个登记项，解析也只通过该名称取得一个登记路径。[映射写入](https://github.com/godotengine/godot/blob/876b290332ec6f2e6d173d08162a02aa7e6ca46d/core/object/script_language.cpp#L2656-L2700)；[按名取路径](https://github.com/godotengine/godot/blob/876b290332ec6f2e6d173d08162a02aa7e6ca46d/modules/gdscript/gdscript_analyzer.cpp#L3467-L3484)。固定分析器源码已对“声明遮蔽既有全局类”给出错误；本轮没有动态复现两个外部 `class_name` 的编辑器完整诊断链。 |

## 对 FrostFoxAtelier 当前选择的适用性

Godot 在具名脚本引用上的便利来自一个明确 opt-in 的全局短名表：业务代码免手写加载，每个已登记类名承担全局唯一性和遮蔽风险。这只能说明免显式加载的类名引用可由共享登记表实现，不能推出全程序唯一短名是唯一设计。`class_name` 是注册机制，不能直接等同于本项目正在讨论的访问修饰符。

FrostFoxAtelier 当前讨论的是按作用域查找裸名、跨命名空间使用限定名；这与 Godot 的显式全局注册是不同的取舍，不能从 Godot 行为自动外推项目规则。
