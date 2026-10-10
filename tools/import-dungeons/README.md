# Dungeons 资源转换

把用户提供的 `POLYGON - Dungeons Pack - Art by Synty 1.10.2.unitypackage` 转成现有 OBJ/MTL 和 Scene v2，不改变引擎场景格式。原包保持不变，导入目标为 `assets/examples/Examples/Dungeons/`。转换工具使用 Python 3、Pillow，以及本机 Unity Editor **6000.3.12f1**；这些仅为离线转换依赖，编辑器运行与普通构建不需要 Unity。

在仓库根目录执行，使用全新的临时转换目录。以下示例沿用本次导入目录；已有导出结果时应另选空目录，避免旧资源混入：

```powershell
$package = 'D:/BaiduNetdiskDownload/POLYGON - Dungeons Pack - Art by Synty/POLYGON - Dungeons Pack - Art by Synty 1.10.2.unitypackage'
$conversion = Join-Path $PWD 'out/dungeons-import'
$unityProject = Join-Path $conversion 'unity'
python tools/import-dungeons/extract.py $package $unityProject
New-Item -ItemType Directory -Force "$unityProject/Assets/Editor", "$unityProject/Packages", "$unityProject/ProjectSettings" | Out-Null
Copy-Item tools/import-dungeons/DungeonExport.cs "$unityProject/Assets/Editor/DungeonExport.cs"
@'
{"dependencies":{"com.unity.modules.animation":"1.0.0","com.unity.modules.imageconversion":"1.0.0","com.unity.modules.jsonserialize":"1.0.0","com.unity.modules.physics":"1.0.0","com.unity.modules.particlesystem":"1.0.0"}}
'@ | Set-Content "$unityProject/Packages/manifest.json" -Encoding utf8
$unity = Start-Process 'D:/Unity/Editor/6000.3.12f1/Editor/Unity.exe' -WindowStyle Hidden -PassThru -Wait -ArgumentList @('-batchmode', '-nographics', '-projectPath', ('"' + $unityProject + '"'), '-executeMethod', 'DungeonExport.Run', '-logFile', ('"' + "$conversion/unity-export.log" + '"'))
if ($unity.ExitCode -ne 0) { throw 'Unity export failed; inspect unity-export.log.' }
python tools/import-dungeons/extract.py $package assets/examples/Examples/Dungeons/Originals
python tools/import-dungeons/convert.py "$conversion/export" assets/examples/Examples/Dungeons
```

`extract.py` 校验包内路径，按 GUID 还原数据和 metadata，排除 C# 等可执行脚本及对应 MonoBehaviour。输出的 `source-manifest.json` 位于目标目录的父目录，记录源包和每份资源转换前后的 SHA-256。不要把原包直接导入现有 Unity 工程；隔离工程中唯一执行的自定义代码是仓库维护的 `DungeonExport.cs`。

`DungeonExport.cs` 利用 Unity 官方 FBX 和 Prefab 解析恢复网格、子网格、材质槽、UV、法线、完整场景实例及灯光。它从序列化材质属性读取颜色与贴图，不依赖源 Shader Graph 成功编译。临时网格和 `export.json` 输出到 Unity 工程旁的 `export/`。成功日志包含 `DUNGEON_EXPORT PASS meshes=2245 materials=158`。

`convert.py` 生成全部网格及场景实际用到的材质变体，保留独立模型身份和实例世界变换；以源 GUID/local ID/材质槽/镜像轴计算确定性 UUID。镜像烘入网格顶点、法线和绕序，实例使用正缩放；转换时核对 Unity 世界矩阵与 TRS，拒绝不能表示的剪切。MTL 与 OBJ 位于同一目录，贴图在其子目录，符合现有资源依赖边界。用于颜色的 TGA/TIF 转为 PNG，原文件仍保留。转换数据的范围、数量和未执行的 Unity 功能见 [示例说明](../../assets/examples/Examples/Dungeons/README.md)。

运行 Dev/Release 的 `editor_dungeons_acceptance` 检查正式引擎对转换结果的消费。发布资产的 FBX、OBJ 与图片使用 Git LFS；Originals 禁止换行转换，确保源摘要可核对。
