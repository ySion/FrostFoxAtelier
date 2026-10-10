// Run only in the isolated conversion project, never ship this Editor helper.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.SceneManagement;

public static class DungeonExport
{
    [Serializable] public class MaterialData { public string id, name, source, texture; public float[] color; }
    [Serializable] public class MeshData { public string id, name, source, file; public int vertices, triangles; }
    [Serializable] public class InstanceData {
        public string path, mesh; public string[] materials;
        public float[] position, rotation, scale, matrix;
        public bool active, enabled, skinned; public int lod;
    }
    [Serializable] public class LightData { public string path, type; public float[] position, rotation, color; public float intensity, range, angle; public bool enabled; }
    [Serializable] public class CameraData { public float[] position, rotation; public float fieldOfView; }
    [Serializable] public class SceneData {
        public string source, name; public List<InstanceData> instances = new List<InstanceData>();
        public List<LightData> lights = new List<LightData>(); public List<CameraData> cameras = new List<CameraData>();
        public int missingScripts, particleSystems;
    }
    [Serializable] public class ExportData {
        public string unityVersion; public List<MeshData> meshes = new List<MeshData>();
        public List<MaterialData> materials = new List<MaterialData>(); public List<SceneData> scenes = new List<SceneData>();
        public List<InstanceData> library = new List<InstanceData>();
    }
    static ExportData data = new ExportData();
    static HashSet<string> exportedMeshes = new HashSet<string>(), exportedMaterials = new HashSet<string>();
    static string output;
    static string Id(UnityEngine.Object obj) {
        if (!obj) return "";
        if (!AssetDatabase.TryGetGUIDAndLocalFileIdentifier(obj, out string guid, out long local))
            throw new Exception("Object has no persistent identity: " + obj.name);
        return guid + "_" + local.ToString(CultureInfo.InvariantCulture);
    }
    static float[] Vec(Vector3 value) { return new[] { value.x, value.y, value.z }; }
    static float[] Quat(Quaternion value) { return new[] { value.x, value.y, value.z, value.w }; }
    static string Hierarchy(Transform t) { return t.parent ? Hierarchy(t.parent) + "/" + t.name : t.name; }
    static string ExportMesh(Mesh mesh) {
        if (!mesh) return "";
        string id = Id(mesh);
        if (!exportedMeshes.Add(id)) return id;
        Vector3[] vertices = mesh.vertices, normals = mesh.normals; Vector2[] uv = mesh.uv;
        if (normals.Length != vertices.Length) { mesh.RecalculateNormals(); normals = mesh.normals; }
        string file = id + ".bin"; int triangles = 0;
        using (var writer = new BinaryWriter(File.Create(Path.Combine(output, file)))) {
            writer.Write(vertices.Length); writer.Write(mesh.subMeshCount);
            for (int i = 0; i < vertices.Length; ++i) {
                foreach (float value in Vec(vertices[i])) writer.Write(value);
                foreach (float value in Vec(normals[i])) writer.Write(value);
                writer.Write(uv.Length == vertices.Length ? uv[i].x : 0);
                writer.Write(uv.Length == vertices.Length ? uv[i].y : 0);
            }
            for (int sub = 0; sub < mesh.subMeshCount; ++sub) {
                if (mesh.GetTopology(sub) != MeshTopology.Triangles) throw new Exception("Non-triangle mesh: " + mesh.name);
                int[] indices = mesh.GetTriangles(sub); writer.Write(indices.Length); triangles += indices.Length / 3;
                foreach (int index in indices) writer.Write(index);
            }
        }
        data.meshes.Add(new MeshData { id = id, name = mesh.name, source = AssetDatabase.GetAssetPath(mesh), file = file, vertices = vertices.Length, triangles = triangles });
        return id;
    }
    static string ExportMaterial(Material material) {
        if (!material) return "";
        string id = Id(material); if (!exportedMaterials.Add(id)) return id;
        var record = new MaterialData { id = id, name = material.name, source = AssetDatabase.GetAssetPath(material), color = new[] {1f,1f,1f,1f}, texture = "" };
        // Read serialized values even when the original render-pipeline shader is unavailable.
        var serialized = new SerializedObject(material);
        var textures = serialized.FindProperty("m_SavedProperties.m_TexEnvs");
        foreach (string property in new[] {"_Albedo_Map", "_BaseMap", "_MainTex"}) {
            if (textures == null) break;
            for (int i = 0; i < textures.arraySize; ++i) {
                var item = textures.GetArrayElementAtIndex(i);
                if (item.FindPropertyRelative("first").stringValue != property) continue;
                var texture = item.FindPropertyRelative("second.m_Texture").objectReferenceValue;
                if (texture) record.texture = AssetDatabase.GetAssetPath(texture);
            }
            if (record.texture.Length != 0) break;
        }
        var colors = serialized.FindProperty("m_SavedProperties.m_Colors");
        foreach (string property in new[] {"_Albedo_Color", "_BaseColor", "_Color"}) {
            bool found = false;
            if (colors != null) for (int i = 0; i < colors.arraySize; ++i) {
                var item = colors.GetArrayElementAtIndex(i);
                if (item.FindPropertyRelative("first").stringValue != property) continue;
                var c = item.FindPropertyRelative("second").colorValue;
                record.color = new[] {c.r,c.g,c.b,c.a}; found = true; break;
            }
            if (found) break;
        }
        data.materials.Add(record); return id;
    }
    static void ExportScene(string path) {
        var scene = EditorSceneManager.OpenScene(path, OpenSceneMode.Single);
        var record = new SceneData {source = path, name = path.Contains("PolygonDungeon") ? (path.Contains("Demo") ? "Dungeon" : "Dungeon Overview") : "Generic Overview"};
        foreach (GameObject root in scene.GetRootGameObjects()) {
            foreach (Transform transform in root.GetComponentsInChildren<Transform>(true)) {
                record.missingScripts += GameObjectUtility.GetMonoBehavioursWithMissingScriptCount(transform.gameObject);
                var renderer = transform.GetComponent<Renderer>();
                var filter = transform.GetComponent<MeshFilter>();
                var skinned = renderer as SkinnedMeshRenderer;
                Mesh mesh = skinned ? skinned.sharedMesh : filter ? filter.sharedMesh : null;
                if (mesh && renderer) {
                    int lod = 0; var group = renderer.GetComponentInParent<LODGroup>();
                    if (group) {
                        var levels = group.GetLODs();
                        for (int i = 0; i < levels.Length; ++i) if (levels[i].renderers.Contains(renderer)) { lod = i; break; }
                    }
                    var matrix = transform.localToWorldMatrix; float[] values = new float[16];
                    for (int i = 0; i < 16; ++i) values[i] = matrix[i];
                    record.instances.Add(new InstanceData {path = Hierarchy(transform), mesh = ExportMesh(mesh), materials = renderer.sharedMaterials.Select(ExportMaterial).ToArray(),
                        position = Vec(transform.position), rotation = Quat(transform.rotation), scale = Vec(transform.lossyScale), matrix = values,
                        active = transform.gameObject.activeInHierarchy, enabled = renderer.enabled, skinned = skinned != null, lod = lod});
                }
                var light = transform.GetComponent<Light>();
                if (light) record.lights.Add(new LightData {path = Hierarchy(transform), type = light.type.ToString(), position = Vec(transform.position), rotation = Quat(transform.rotation),
                    color = new[] {light.color.r, light.color.g, light.color.b}, intensity = light.intensity, range = light.range, angle = light.spotAngle, enabled = light.enabled && light.gameObject.activeInHierarchy});
                var camera = transform.GetComponent<Camera>();
                if (camera) record.cameras.Add(new CameraData {position = Vec(transform.position), rotation = Quat(transform.rotation), fieldOfView = camera.fieldOfView});
            }
            record.particleSystems += root.GetComponentsInChildren<ParticleSystem>(true).Length;
        }
        data.scenes.Add(record); Debug.Log("DUNGEON_EXPORT scene " + record.name + " meshes=" + record.instances.Count + " lights=" + record.lights.Count);
    }
    public static void Run() {
        try {
            output = Path.GetFullPath(Path.Combine(Application.dataPath,"../../export")); Directory.CreateDirectory(output);
            data.unityVersion = Application.unityVersion;
            var assets = AssetDatabase.GetAllAssetPaths().Where(p => p.StartsWith("Assets/Synty/", StringComparison.Ordinal)).OrderBy(p => p).ToArray();
            int count = 0;
            foreach (string path in assets) {
                string extension = Path.GetExtension(path).ToLowerInvariant();
                if (extension == ".fbx" || extension == ".asset") {
                    foreach (var asset in AssetDatabase.LoadAllAssetsAtPath(path)) if (asset is Mesh mesh && mesh.vertexCount > 0) ExportMesh(mesh);
                }
                if (extension == ".mat") ExportMaterial(AssetDatabase.LoadAssetAtPath<Material>(path));
                if (extension == ".prefab" || extension == ".fbx") {
                    var model = AssetDatabase.LoadAssetAtPath<GameObject>(path);
                    if(model) foreach(var renderer in model.GetComponentsInChildren<Renderer>(true)) {
                        var filter = renderer.GetComponent<MeshFilter>(); var skin = renderer as SkinnedMeshRenderer;
                        var mesh = skin ? skin.sharedMesh : filter ? filter.sharedMesh : null;
                        if(mesh && mesh.vertexCount > 0) data.library.Add(new InstanceData {path = path + "/" + Hierarchy(renderer.transform),
                            mesh = ExportMesh(mesh), materials = renderer.sharedMaterials.Select(ExportMaterial).ToArray()});
                    }
                }
                if (++count % 300 == 0) Debug.Log("DUNGEON_EXPORT scanned " + count);
            }
            foreach (string path in assets.Where(p => p.EndsWith(".unity", StringComparison.Ordinal))) ExportScene(path);
            File.WriteAllText(Path.Combine(output,"export.json"), JsonUtility.ToJson(data,true));
            Debug.Log("DUNGEON_EXPORT PASS meshes=" + data.meshes.Count + " materials=" + data.materials.Count);
            EditorApplication.Exit(0);
        } catch (Exception error) { Debug.LogException(error); EditorApplication.Exit(1); }
    }
}
