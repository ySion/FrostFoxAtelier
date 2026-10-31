#include "application/control_artifacts.hpp"
#include "editor/project.hpp"
#include "editor/session.hpp"
#include "assets/file.hpp"
#include "assets/json_format.hpp"
#include "assets/material_format.hpp"
#include "material/resources.hpp"
#include "platform/windows_path.hpp"
#include "source/snapshot.hpp"
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <fstream>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace atelier::application::control_artifacts {
namespace {
using control::Json;
using control::Failure;
template <class T>
T take(Result<T> value) {
    if (!value)
        throw Failure("storage_error", value.error().message);
    return std::move(*value);
}
void take(Result<void> value) {
    if (!value)
        throw Failure("storage_error", value.error().message);
}
void checkpoint(const std::atomic_bool& cancelled) {
    if (cancelled.load())
        throw Failure("cancelled", "Artifact export cancelled");
}
void write(const std::filesystem::path& path, std::string_view bytes) {
    const auto native = platform::windows_file_path(path);
    std::filesystem::create_directories(native.parent_path());
    take(editor::write_file(native, bytes, editor::WriteMode::create));
}
std::string path_text(const std::filesystem::path& path) {
    return control::utf8(path.generic_wstring());
}
bool generated_cache(const std::filesystem::path& relative) {
    auto part = relative.begin();
    if (part == relative.end() || *part++ != L"Project")
        return false;
    return part != relative.end() && *part == L"Cache";
}
Json engine_source_hashes(std::span<const language::SourceFile> files) {
    auto hashes = Json::object();
    for (const auto& file : files)
        if (file.path.starts_with("engine/"))
            hashes[file.path] = control::sha256(std::as_bytes(std::span(file.source)));
    return hashes;
}
std::string mesh_hash(const assets::MeshAsset& asset) try {
    auto digest = assets::mesh_content_sha256(asset);
    if (!digest)
        throw Failure("dependency_mismatch", digest.error().message);
    return std::move(*digest);
} catch (const Failure&) {
    throw;
} catch (const std::exception& error) {
    throw Failure("storage_error", error.what());
}
assets::EngineDependency installed_asset(std::string_view key, assets::AssetKind kind) try {
    auto result = assets::Reader{}.engine_dependency(key, kind);
    if (!result)
        throw Failure("dependency_mismatch", result.error().message);
    return std::move(*result);
} catch (const Failure&) {
    throw;
} catch (const std::exception& error) {
    throw Failure("storage_error", error.what());
}
Json engine_assets_json(std::span<const assets::EngineDependency> assets) {
    auto records = Json::array();
    for (const auto& asset : assets)
        records.push_back({{"key", asset.key},
                           {"kind", asset.kind == assets::AssetKind::material ? "material" : "mesh"},
                           {"semantic_version", asset.semantic_version},
                           {"digest_version", asset.digest_version},
                           {"content_sha256", asset.content_sha256}});
    return {{"version", 1}, {"records", std::move(records)}};
}
std::vector<assets::EngineDependency> installed_assets(const Json& manifest) {
    if (!manifest.contains("engine_assets"))
        throw Failure("invalid_request", "Repro lacks engine assets; export it again");
    const auto version = [](const Json& value) -> std::uint32_t {
        if (!value.is_number_integer() ||
            (value.is_number_integer() && !value.is_number_unsigned() && value.get<std::int64_t>() <= 0) ||
            value.get<std::uint64_t>() == 0 || value.get<std::uint64_t>() > UINT32_MAX)
            throw Failure("invalid_request", "Engine asset versions require positive uint32 integers");
        return value.get<std::uint32_t>();
    };
    const auto& object = manifest.at("engine_assets");
    control::fields(object, {"version", "records"});
    if (version(object.at("version")) != 1)
        throw Failure("invalid_request", "Unknown engine assets schema; export it again");
    const auto& records = object.at("records");
    if (!records.is_array())
        throw Failure("invalid_request", "Engine assets require a record array");
    std::set<std::string> keys;
    std::vector<assets::EngineDependency> result;
    for (const auto& record : records) {
        control::fields(record, {"key", "kind", "semantic_version", "digest_version", "content_sha256"});
        const auto key = record.at("key").get<std::string>();
        if (!keys.insert(key).second)
            throw Failure("invalid_request", "Duplicate engine asset: " + key);
        const auto semantic = version(record.at("semantic_version")), encoding = version(record.at("digest_version"));
        const auto name = record.at("kind").get<std::string>();
        if (name != "mesh" && name != "material")
            throw Failure("dependency_mismatch", "Installed engine asset kind differs: " + key);
        const auto kind = name == "material" ? assets::AssetKind::material : assets::AssetKind::mesh;
        if (encoding !=
            (kind == assets::AssetKind::material ? assets::material_digest_version : assets::mesh_digest_version))
            throw Failure("invalid_request", "Unknown engine asset digest encoding; export it again");
        const auto digest = record.at("content_sha256").get<std::string>();
        if (digest.size() != 64 ||
            !std::ranges::all_of(digest, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
            throw Failure("invalid_request", "Invalid engine asset SHA-256: " + key);
        auto current = installed_asset(key, kind);
        if (current.semantic_version != semantic)
            throw Failure("dependency_mismatch", "Installed engine asset semantic version differs: " + key);
        if (current.content_sha256 != digest)
            throw Failure("dependency_mismatch", "Installed engine asset content differs: " + key);
        result.push_back(std::move(current));
    }
    std::ranges::sort(result, {}, &assets::EngineDependency::key);
    return result;
}
const Json& material_records(const Json& manifest) {
    const auto at = manifest.find("material_resources");
    if (at == manifest.end() || !at->is_object())
        throw Failure("invalid_request", "Repro lacks material content digests; export it again");
    control::fields(*at, {"version", "records"});
    if (!at->contains("version") || !at->contains("records") || !at->at("version").is_number_integer() ||
        at->at("version") != 1 || !at->at("records").is_object())
        throw Failure("invalid_request", "Unknown material content digest schema; export it again");
    for (const auto& [key, value] : at->at("records").items()) {
        if (key.empty() || !value.is_string())
            throw Failure("invalid_request", "Invalid material content digest record");
        const auto& digest = value.get_ref<const std::string&>();
        if (digest.size() != 64 ||
            !std::ranges::all_of(digest, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
            throw Failure("invalid_request", "Invalid material content SHA-256: " + key);
    }
    return *at;
}
std::string string_field(const Json& object, const char* name) {
    const auto at = object.find(name);
    return at != object.end() && at->is_string() ? at->get<std::string>() : std::string{};
}
// Only schema resource slots are references. Do not inspect arbitrary strings,
// cached drafts, copied files or unknown extension properties.
template <class Reference>
void scene_references(const Json& scene, const Reference& reference) {
    if (const auto materials = scene.find("materials"); materials != scene.end() && materials->is_array())
        for (const auto& definition : *materials)
            if (definition.is_object()) {
                reference(assets::AssetKind::material, string_field(definition, "source"));
                reference(assets::AssetKind::material, string_field(definition, "parent"));
                // Entries need their terminal MS declaration before a texture value
                // can be considered an execution dependency.
            }
    if (const auto entities = scene.find("entities"); entities != scene.end() && entities->is_array())
        for (const auto& entity : *entities)
            if (const auto components = entity.find("components"); components != entity.end() && components->is_array())
                for (const auto& component : *components)
                    if (component.is_object()) {
                        reference(assets::AssetKind::mesh, string_field(component, "mesh"));
                        reference(assets::AssetKind::sky, string_field(component, "sky_map"));
                    }
}
// Only inspect published Scene resource slots and their mesh defaults/material
// inputs, never all copied files. The strict loader still owns project failures;
// this preflight identifies invalid engine keys before it can reject the load.
void preflight_resources(editor::Project& project, const Json& scene) {
    std::set<std::pair<assets::AssetKind, std::string>> references;
    std::vector<std::string> materials;
    const auto reference = [&](assets::AssetKind kind, const std::string& key) {
        if (key.empty() || !references.emplace(kind, key).second)
            return;
        if (key.starts_with("builtin/"))
            static_cast<void>(installed_asset(key, kind));
        if (kind == assets::AssetKind::material)
            materials.push_back(key);
    };
    scene_references(scene, reference);
    // Collect before visiting materials; their ordinal need not follow meshes.
    std::vector<std::string> meshes;
    for (const auto& [kind, key] : references)
        if (kind == assets::AssetKind::mesh)
            meshes.push_back(key);
    for (const auto& key : meshes)
        if (auto mesh = project.assets().mesh(key))
            for (const auto& binding : mesh->materials)
                reference(assets::AssetKind::material, binding.key);
    std::map<std::string, assets::MaterialAsset, std::less<>> authors;
    for (std::size_t i = 0; i < materials.size(); ++i)
        if (auto asset = project.assets().material(materials[i])) {
            if (const auto* source = std::get_if<assets::MaterialSourceAsset>(&asset->author)) {
                for (const auto& input : source->inputs)
                    reference(assets::AssetKind::texture, input.binding.asset_key);
            } else
                reference(assets::AssetKind::material,
                          std::get<assets::MaterialInstanceAsset>(asset->author).parent_key);
            authors.emplace(asset->key, std::move(*asset));
        }
    const auto source_author = [&](std::string key) -> const assets::MaterialSourceAsset* {
        std::set<std::string> visited;
        while (!key.empty() && visited.insert(key).second) {
            const auto at = authors.find(key);
            if (at == authors.end())
                break;
            if (const auto* source = std::get_if<assets::MaterialSourceAsset>(&at->second.author))
                return source;
            key = std::get<assets::MaterialInstanceAsset>(at->second.author).parent_key;
        }
        return nullptr; // Strict scene loading diagnoses broken/cyclic parents.
    };
    const auto entries = [&](std::span<const material::Override> values, const assets::MaterialSourceAsset* source) {
        if (!source)
            return;
        for (const auto& value : values)
            if (value.enabled)
                if (const auto* texture = std::get_if<material::TextureBinding>(&value.value);
                    texture && std::ranges::find(source->inputs, value.path, &material::TextureDefault::path) !=
                                   source->inputs.end())
                    reference(assets::AssetKind::texture, texture->asset_key);
    };
    for (const auto& [key, asset] : authors)
        if (const auto* instance = std::get_if<assets::MaterialInstanceAsset>(&asset.author))
            entries(instance->entries, source_author(key));
    if (const auto definitions = scene.find("materials"); definitions != scene.end() && definitions->is_array()) {
        std::map<std::string, const Json*> locals;
        for (const auto& definition : *definitions)
            locals.emplace(definition.at("id").get<std::string>(), &definition);
        for (const auto& definition : *definitions)
            if (definition.contains("entries")) {
                const Json* current = &definition;
                std::set<const Json*> visited;
                const assets::MaterialSourceAsset* source{};
                while (current && visited.insert(current).second) {
                    const auto key = string_field(*current, "source");
                    if (!key.empty()) {
                        source = source_author(key);
                        break;
                    }
                    const auto parent = current->find("parent");
                    if (parent == current->end())
                        break;
                    if (parent->is_string()) {
                        source = source_author(parent->get<std::string>());
                        break;
                    }
                    if (!parent->is_object() || !parent->contains("$ref"))
                        break;
                    const auto at = locals.find(parent->at("$ref").get<std::string>());
                    current = at == locals.end() ? nullptr : at->second;
                }
                entries(assets::material_format::decode_entries(definition.at("entries")), source);
            }
    }
}
Json image_content(const material::ImageRef& image) {
    if (!image)
        return nullptr;
    return {{"width", image->width()},
            {"height", image->height()},
            {"rgba_sha256", control::sha256(std::as_bytes(image->pixels()))}};
}
std::string json_hash(const Json& value) {
    const auto bytes = value.dump();
    return control::sha256(std::as_bytes(std::span(bytes)));
}
std::string source_material_hash(const assets::MaterialAsset& asset, const language::SourceFile& file) {
    // The installed MS producer's versioned author/text encoding, not a runtime
    // program hash. Project/scene records additionally digest the input pixels.
    return json_hash({{"domain", "Atelier.MaterialSource"},
                      {"digest_version", assets::material_digest_version},
                      {"key", asset.key},
                      {"author", assets::material_format::encode(asset)},
                      {"source_path", file.path},
                      {"source", file.source}});
}
std::string sky_hash(const rendering::SkyMap& map) {
    const auto bytes = Json{{"width", map.width()},
                            {"height", map.height()},
                            {"scale_exponent", map.scale_exponent()},
                            {"mean", map.mean()},
                            {"pixels", control::sha256(std::as_bytes(map.pixels()))},
                            {"cube", control::sha256(std::as_bytes(map.cube()))}}
                           .dump();
    return control::sha256(std::as_bytes(std::span(bytes)));
}
struct Com {
    Com() {
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
            throw Failure("storage_error", "Cannot initialize image encoder");
    }
    ~Com() { CoUninitialize(); }
};
void png(std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> rgba,
         const std::filesystem::path& path) {
    using Microsoft::WRL::ComPtr;
    const auto hr = [](HRESULT result) {
        if (FAILED(result))
            throw Failure("storage_error", "PNG encoding failed: " + std::to_string(result));
    };
    ComPtr<IWICImagingFactory> factory;
    hr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
    ComPtr<IWICStream> stream;
    hr(factory->CreateStream(&stream));
    hr(stream->InitializeFromFilename(platform::windows_file_path(path).c_str(), GENERIC_WRITE));
    ComPtr<IWICBitmapEncoder> encoder;
    hr(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder));
    hr(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame;
    hr(encoder->CreateNewFrame(&frame, nullptr));
    hr(frame->Initialize(nullptr));
    hr(frame->SetSize(width, height));
    auto format = GUID_WICPixelFormat32bppBGRA;
    hr(frame->SetPixelFormat(&format));
    if (format != GUID_WICPixelFormat32bppBGRA)
        throw Failure("storage_error", "PNG codec changed BGRA format");
    // Channel permutation only: Image bytes may be linear data or sRGB. Never
    // apply a transfer function, premultiply alpha, or quantize through a capture.
    std::vector<std::uint8_t> bgra(rgba.begin(), rgba.end());
    for (std::size_t pixel = 0; pixel < bgra.size(); pixel += 4)
        std::swap(bgra[pixel], bgra[pixel + 2]);
    hr(frame->WritePixels(height, width * 4, static_cast<UINT>(bgra.size()), bgra.data()));
    hr(frame->Commit());
    hr(encoder->Commit());
}
void finish(const std::filesystem::path& directory, Json& manifest, const std::atomic_bool& cancel) {
    checkpoint(cancel);
    auto files = Json::array();
    for (const auto& item : std::filesystem::recursive_directory_iterator(directory))
        if (item.is_regular_file() && !generated_cache(item.path().lexically_relative(directory))) {
            checkpoint(cancel);
            files.push_back({{"path", path_text(item.path().lexically_relative(directory))},
                             {"bytes", item.file_size()},
                             {"sha256", control::sha256_file(item.path())}});
        }
    checkpoint(cancel);
    manifest["files"] = std::move(files);
    manifest["complete"] = true;
    const auto encoded = manifest.dump(2) + "\n";
    if (encoded.size() > assets::json_format::maximum_bytes)
        throw Failure("resource_limit", "Artifact manifest exceeds the 16 MiB file limit");
    write(directory / "manifest.json", encoded);
}
}
Repro snapshot_resources(const editor::Session& document) try {
    Repro data;
    data.scene = take(document.serialize());
    const auto scene = assets::json_format::parse(data.scene);
    std::map<std::string, assets::EngineDependency, std::less<>> engine;
    std::set<std::pair<assets::AssetKind, std::string>> references;
    std::vector<ObjectId> pending;
    const auto reference = [&](assets::AssetKind kind, const std::string& key) {
        if (key.empty() || !references.emplace(kind, key).second)
            return;
        if (key.starts_with("builtin/")) {
            const auto at = engine.find(key);
            if (at == engine.end())
                engine.emplace(key, installed_asset(key, kind));
            else if (at->second.kind != kind)
                throw Failure("dependency_mismatch", "Engine asset used with conflicting kinds: " + key);
        }
        if (kind == assets::AssetKind::material)
            if (const auto object = document.loaded_material(key); object != ObjectId{})
                pending.push_back(object);
    };
    // The serialized graph includes retained resource definitions, but not every
    // material a browser happened to load. Ordinary properties are not references.
    scene_references(scene, reference);
    auto& runtime = document.world().runtime();
    std::map<std::string, const Json*> locals;
    for (const auto& definition : scene.at("materials"))
        if (string_field(definition, "source").empty())
            locals.emplace(definition.at("id").get<std::string>(), &definition);
    if (!locals.empty()) {
        // local_object intentionally admits only Entities/Components. Enumerate
        // identities, but admit only the material LocalIds in the serialized graph
        // (including retained definitions), not unrelated drafts or Undo roots.
        std::vector<ObjectId> live;
        runtime.changed_objects(0, live);
        std::set<std::string> found;
        for (const auto object : live)
            if (const auto id = document.document_identity(object))
                if (const auto key = std::to_string(*id); locals.contains(key)) {
                    pending.push_back(object);
                    found.insert(key);
                }
        if (found.size() != locals.size())
            throw Failure("dependency_mismatch", "A scene material has no live author resource");
    }
    for (const auto& entity : take(document.world().members()))
        for (const auto& component : take(world::components(runtime, entity.id()))) {
            if (auto mesh = presentation::read_static_mesh(runtime, component.id())) {
                reference(assets::AssetKind::mesh, mesh->mesh.source);
                assets::MeshAsset snapshot{mesh->mesh.source, {}, mesh->mesh.resolved};
                for (const auto& binding : mesh->mesh.defaults) {
                    const auto key = take(material::material_key(runtime, binding.resource.id()));
                    reference(assets::AssetKind::material, key);
                    snapshot.materials.push_back({binding.slot, key});
                    pending.push_back(binding.resource.id());
                }
                const auto bindings = take(presentation::read_material_bindings(runtime, component.id()));
                for (const auto& binding : bindings)
                    pending.push_back(binding.resource.id());
                if (snapshot.mesh)
                    for (std::size_t i = 0; i < snapshot.mesh->materials().size(); ++i) {
                        const auto slot = presentation::material_slot_name(*snapshot.mesh, i);
                        if (std::ranges::find(bindings, slot, &presentation::MaterialBinding::slot) == bindings.end() &&
                            std::ranges::find(mesh->mesh.defaults, slot, &presentation::MaterialBinding::slot) ==
                                mesh->mesh.defaults.end()) {
                            // This implicit surface is not a cached editor draft root.
                            const std::string key = "builtin/material/default-grid";
                            references.emplace(assets::AssetKind::material, key);
                            engine.try_emplace(key, installed_asset(key, assets::AssetKind::material));
                        }
                    }
                if (snapshot.mesh && std::ranges::none_of(data.meshes, [&](const auto& entry) {
                        return entry.key == snapshot.key && entry.mesh == snapshot.mesh &&
                               entry.materials == snapshot.materials;
                    }))
                    data.meshes.push_back(std::move(snapshot));
            } else if (auto sky = presentation::read_sky_map(runtime, component.id())) {
                reference(assets::AssetKind::sky, sky->source);
                if (sky->resolved && std::ranges::none_of(data.skies, [&](const auto& entry) {
                        return entry.first == sky->source && entry.second == sky->resolved;
                    }))
                    data.skies.emplace_back(sky->source, sky->resolved);
            }
        }
    std::unordered_set<ObjectId> visited;
    auto records = Json::object();
    const auto identity = [&](ObjectId object) -> std::string {
        auto key = take(material::material_key(runtime, object));
        if (!key.empty())
            return key;
        const auto id = document.document_identity(object);
        if (!id || !locals.contains(std::to_string(*id)))
            throw Failure("dependency_mismatch", "Material parent is outside the serialized scene closure");
        return "scene/" + std::to_string(*id);
    };
    const auto texture = [&](std::string_view path, const std::string& key, const ObjectRoot& root, Json& inputs) {
        reference(assets::AssetKind::texture, key);
        material::ImageRef image;
        if (root.registered()) {
            auto state = take(material::read_texture(runtime, root.id()));
            if (state.source != key)
                throw Failure("dependency_mismatch", "Material input is bound to a different texture key: " + key);
            image = std::move(state.image);
        }
        inputs[std::string(path)] = {{"key", key}, {"image", image_content(image)}};
        if (!image)
            return;
        const auto at = std::ranges::find(data.textures, key, &assets::TextureAsset::key);
        if (at == data.textures.end())
            data.textures.push_back({key, {}, std::move(image)});
        else if (at->image != image && image_content(at->image) != image_content(image))
            throw Failure("dependency_mismatch", "One texture key has distinct loaded input pixels: " + key);
    };
    for (std::size_t i = 0; i < pending.size(); ++i) {
        const auto object = pending[i];
        if (!visited.insert(object).second)
            continue;
        const auto key = take(material::material_key(runtime, object));
        reference(assets::AssetKind::material, key);
        if (document.unavailable(assets::AssetKind::material, key))
            continue;
        auto asset = take(document.material_asset(object));
        Json content = {{"author", assets::material_format::encode(asset)}, {"inputs", Json::object()}};
        if (std::holds_alternative<assets::MaterialSourceAsset>(asset.author)) {
            const auto state = take(material::read_source(runtime, object));
            const auto view = take(document.inspect_material(object));
            content["source"] = nullptr;
            if (view.source_file) {
                const auto& file = *view.source_file;
                content["source"] = {{"path", file.path}, {"text", file.source}};
                const auto at = std::ranges::find(data.material_sources, file.path, &language::SourceFile::path);
                if (at == data.material_sources.end())
                    data.material_sources.push_back(file);
                else if (at->source != file.source)
                    throw Failure("dependency_mismatch", "One MS Source path has distinct loaded text: " + file.path);
                if (key.starts_with("builtin/") && source_material_hash(asset, file) != engine.at(key).content_sha256)
                    throw Failure("dependency_mismatch",
                                  "Loaded engine material differs from its installed producer: " + key);
            } else if (key.starts_with("builtin/"))
                throw Failure("dependency_mismatch", "Loaded engine material has no Source snapshot: " + key);
            if (state.definition)
                for (std::size_t input = 0; input < state.definition->textures().size(); ++input) {
                    const auto& value = state.definition->textures()[input];
                    texture(value.path, value.binding.asset_key, state.default_textures.at(input), content["inputs"]);
                }
        } else {
            const auto state = take(material::read_instance(runtime, object));
            const auto& author = std::get<assets::MaterialInstanceAsset>(asset.author);
            reference(assets::AssetKind::material, author.parent_key);
            content["parent"] = state.parent.registered() ? Json(identity(state.parent.id())) : Json();
            if (state.parent.registered())
                pending.push_back(state.parent.id());
            if (key.empty())
                content["author"] = *locals.at(std::to_string(*document.document_identity(object)));
            const auto surface = take(material::capture_program(runtime, object));
            if (!surface->uses_default_material())
                for (std::size_t entry = 0; entry < state.entries.size(); ++entry) {
                    const auto& value = state.entries[entry];
                    const auto* binding = std::get_if<material::TextureBinding>(&value.value);
                    if (value.enabled && binding &&
                        std::ranges::find(surface->program().textures(), value.path,
                                          &source::MaterialTextureInput::key) != surface->program().textures().end())
                        texture(value.path, binding->asset_key, state.textures.at(entry), content["inputs"]);
                }
        }
        records[identity(object)] = json_hash(content);
        if (!key.empty() && !key.starts_with("builtin/"))
            data.materials.push_back(std::move(asset));
    }
    // Compare every distinct loaded version before deduplicating the declaration.
    for (const auto& mesh : data.meshes)
        if (const auto at = engine.find(mesh.key); at != engine.end())
            if (mesh_hash(mesh) != at->second.content_sha256)
                throw Failure("dependency_mismatch",
                              "Loaded engine mesh differs from its installed producer: " + mesh.key);
    for (auto& [key, dependency] : engine) {
        static_cast<void>(key);
        data.engine_assets.push_back(std::move(dependency));
    }
    data.references.assign(references.begin(), references.end());
    data.material_resources = {{"version", 1}, {"records", std::move(records)}};
    return data;
} catch (const Failure&) {
    throw;
} catch (const std::exception& error) {
    throw Failure("storage_error", error.what());
}
void validate_candidate_assets(const editor::Session& candidate, const Json& manifest) {
    const auto expected = installed_assets(manifest);
    const auto actual = snapshot_resources(candidate);
    for (const auto& item : actual.engine_assets)
        if (std::ranges::find(expected, item) == expected.end())
            throw Failure("dependency_mismatch", "Repro omits or changes an actual engine dependency: " + item.key);
    for (const auto& item : expected)
        if (std::ranges::find(actual.engine_assets, item) == actual.engine_assets.end())
            throw Failure("dependency_mismatch", "Repro declares an unused engine dependency: " + item.key);
    if (material_records(manifest) != actual.material_resources)
        throw Failure("dependency_mismatch",
                      "Repro material author, Source, parent or input pixels differ from the frozen content");
}
Json export_repro(const std::filesystem::path& output, Repro data, const std::atomic_bool& cancel) {
    checkpoint(cancel);
    std::optional<Com> com;
    if (!data.textures.empty())
        com.emplace();
    // Keep all worker filesystem operations in the Win32 extended namespace,
    // including metadata reads and directory entries. Public paths keep their
    // original spelling; relative manifest identities never contain the prefix.
    const auto directory = platform::windows_file_path(output);
    if (!data.project.empty())
        data.project = platform::windows_file_path(data.project);
    // Resolve existing ancestors by identity, including case aliases/junctions,
    // before creating any output that a source traversal could encounter.
    if (!data.project.empty()) {
        for (const auto& section : data.roots) {
            const auto input = platform::windows_file_path(std::filesystem::canonical(data.project / section));
            for (auto parent = std::filesystem::canonical(directory.parent_path());;) {
                if (std::filesystem::equivalent(platform::windows_file_path(parent), input))
                    throw Failure("invalid_destination",
                                  "Repro output cannot be inside a project folder (" + path_text(section) + ")");
                const auto next = parent.parent_path();
                if (next == parent || next.empty())
                    break;
                parent = next;
            }
        }
    }
    if (!std::filesystem::create_directory(directory))
        throw Failure("storage_error", "Artifact directory already exists");
    const auto project = directory / "Project";
    if (data.project.empty())
        take(editor::Project::create(output / "Project"));
    else {
        std::uintmax_t bytes{};
        std::size_t files{};
        Json input_files = Json::array();
        const auto link = [](const std::filesystem::path& path) {
            const auto flags = GetFileAttributesW(platform::windows_file_path(path).c_str());
            return flags != INVALID_FILE_ATTRIBUTES && (flags & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        };
        const auto copy = [&](const std::filesystem::path& item) {
            checkpoint(cancel);
            if (link(item))
                throw Failure("dependency_mismatch", "Repro export does not follow links");
            const auto relative = item.lexically_relative(data.project);
            const auto target = platform::windows_file_path(project / relative);
            if (std::filesystem::is_directory(item)) {
                std::filesystem::create_directories(target);
                return;
            }
            const auto source = std::ranges::find_if(data.material_sources, [&](const auto& file) {
                const auto named = control::wide(file.path), path = relative.generic_wstring();
                return CompareStringOrdinal(named.c_str(), -1, path.c_str(), -1, TRUE) == CSTR_EQUAL;
            });
            material::ImageRef image;
            if (assets::is_texture_path(relative)) {
                auto identity = item;
                identity += L".asset.json";
                if (link(identity))
                    throw Failure("dependency_mismatch", "Repro export does not follow links");
                if (std::filesystem::is_regular_file(identity)) {
                    const auto key =
                        "project/" +
                        assets::json_format::parse(take(assets::read_file(identity))).at("id").get<std::string>();
                    if (const auto at = std::ranges::find(data.textures, key, &assets::TextureAsset::key);
                        at != data.textures.end())
                        image = at->image;
                }
            }
            if (source != data.material_sources.end() || image) {
                std::filesystem::create_directories(target.parent_path());
                // The asset key/path and sidecar stay unchanged. WIC detects the
                // lossless PNG container even when the original suffix was JPEG.
                if (image)
                    png(image->width(), image->height(), image->pixels(), target);
                else
                    write(target, source->source);
                // This is a file budget, not the decoded in-memory RGBA size.
                const auto length = std::filesystem::file_size(target);
                bytes += length;
                if (++files > 10000 || length > 256 * 1024 * 1024 || bytes > 1024ull * 1024 * 1024)
                    throw Failure("resource_limit", "Repro exceeds 10000 files / 1 GiB / 256 MiB per file");
                input_files.push_back(
                    {{"path", path_text(relative)}, {"sha256", control::sha256_file(target)}, {"snapshot", true}});
                return;
            }
            if (!std::filesystem::is_regular_file(item))
                throw Failure("dependency_mismatch", "Unsupported project file");
            const auto length = std::filesystem::file_size(item);
            bytes += length;
            if (++files > 10000 || length > 256 * 1024 * 1024 || bytes > 1024ull * 1024 * 1024)
                throw Failure("resource_limit", "Repro exceeds 10000 files / 1 GiB / 256 MiB per file");
            const auto stamp = std::filesystem::last_write_time(item);
            const auto hash = control::sha256_file(item);
            std::filesystem::create_directories(target.parent_path());
            if (!std::filesystem::copy_file(item, target))
                throw Failure("storage_error", "Cannot copy project dependency");
            if (stamp != std::filesystem::last_write_time(item) || length != std::filesystem::file_size(item) ||
                hash != control::sha256_file(item) || hash != control::sha256_file(project / relative))
                throw Failure("dependency_mismatch", "Project dependency changed during export");
            input_files.push_back({{"path", path_text(relative)}, {"sha256", hash}});
        };
        // Config and Source are copied whole: the configuration is rewritten and the
        // published code replaces Source below. Other folders hold assets; only the
        // files of the scene's assets and their references are copied.
        for (const auto* section : {"Config", "Source"}) {
            const auto root = data.project / section;
            if (link(root))
                throw Failure("dependency_mismatch", "Project sections cannot be links");
            std::filesystem::create_directories(project / section);
            for (const auto& item : std::filesystem::recursive_directory_iterator(root))
                copy(item.path());
        }
        const auto sections = files;
        for (const auto& relative : data.files) {
            if (const auto top = *relative.begin(); top == L"Config" || top == L"Source")
                continue;
            for (auto parent = relative.parent_path(); !parent.empty(); parent = parent.parent_path())
                if (link(data.project / parent))
                    throw Failure("dependency_mismatch", "Repro export does not follow links");
            copy(data.project / relative);
        }
        std::filesystem::create_directories(project / "Content");
        // Recheck the copied set after copying, including additions/removals in Config and Source.
        std::size_t current_files{};
        for (const auto* section : {"Config", "Source"})
            for (const auto& item : std::filesystem::recursive_directory_iterator(data.project / section))
                if (item.is_regular_file())
                    ++current_files;
        if (current_files != sections)
            throw Failure("dependency_mismatch", "Project file set changed during export");
        for (const auto& file : input_files) {
            checkpoint(cancel);
            if (file.value("snapshot", false))
                continue;
            if (control::sha256_file(data.project / control::wide(file.at("path").get<std::string>())) !=
                file.at("sha256").get<std::string>())
                throw Failure("dependency_mismatch", "Project changed during export");
        }
        data.manifest["input_files"] = std::move(input_files);
    }
    checkpoint(cancel);
    // Reproduce the code actually published into this Runtime. Drafts are separate.
    // Saved edits/additions may not have published. Keep them outside the
    // runnable Source tree, then materialize exactly the live inputs.
    std::unordered_map<std::string, const language::SourceFile*> published;
    if (data.sources)
        for (const auto& file : data.sources->files)
            published.emplace(file.path, &file);
    if (data.ui_source)
        published.emplace(data.ui_source->path, &*data.ui_source);
    std::vector<std::filesystem::path> copied_code;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(project / "Source"))
        if (entry.is_regular_file() && entry.path().extension() == L".source")
            copied_code.push_back(entry.path());
    for (const auto& path : copied_code) {
        checkpoint(cancel);
        const auto relative = path.lexically_relative(project);
        const auto found = published.find(path_text(relative));
        if (found != published.end() && std::filesystem::file_size(path) == found->second->source.size() &&
            take(assets::read_file(path)) == found->second->source) {
            std::filesystem::remove(path);
            continue;
        }
        const auto archive = platform::windows_file_path(directory / "unpublished" / relative);
        std::filesystem::create_directories(archive.parent_path());
        std::filesystem::rename(path, archive);
    }
    const auto publish_source = [&](const language::SourceFile& file) {
        const std::filesystem::path relative = control::wide(file.path);
        if (relative.is_absolute() || relative.empty() || *relative.begin() != L"Source" ||
            relative.lexically_normal() != relative)
            throw Failure("dependency_mismatch", "Published source is not project relative");
        write(project / relative, file.source);
    };
    // Engine definitions stay installation-owned. Pin the published inputs rather
    // than copying them into project scope or silently using a newer installation.
    data.manifest["engine_sources"] =
        data.sources ? engine_source_hashes(data.sources->files)
                     : engine_source_hashes(take(source::engine_sources(source::default_engine_root())));
    if (data.sources)
        for (const auto& file : data.sources->files)
            if (!file.path.starts_with("engine/"))
                publish_source(file);
    if (data.ui_source)
        publish_source(*data.ui_source);
    for (const auto& file : data.material_sources) {
        checkpoint(cancel);
        if (file.path.starts_with("builtin/"))
            continue;
        if (const auto at = published.find(file.path); at != published.end() && at->second->source != file.source)
            throw Failure("dependency_mismatch",
                          "Gameplay and material Source captured different text at " + file.path);
        // Containment compares canonical paths, not the worker's extended Win32 spelling.
        const auto target =
            take(assets::resolve_project_path(std::filesystem::canonical(project), control::wide(file.path)));
        std::filesystem::create_directories(platform::windows_file_path(target.parent_path()));
        take(editor::write_file(target, file.source));
    }
    const auto scene_path = std::filesystem::path("Content/Scenes") / ("Control-" + control::unique_id() + ".scene");
    write(project / scene_path, data.scene);
    auto config = assets::json_format::parse(take(assets::read_file(project / "Config/Project.json")));
    if (data.ui_source)
        config["ui_source"] = data.ui_source->path;
    else
        config.erase("ui_source");
    config["startup_scene"] = path_text(scene_path);
    take(editor::write_file(project / "Config/Project.json", config.dump(2) + "\n"));
    auto clone = take(editor::Project::open(output / "Project"));
    auto resources = Json::array();
    for (const auto& material : data.materials) {
        checkpoint(cancel);
        // Publish the live draft only in this independent bundle, never in the source project.
        take(clone.save_material(material));
        if (assets::material_format::encode(take(clone.assets().material(material.key))) !=
            assets::material_format::encode(material))
            throw Failure("dependency_mismatch", "Exported material author differs from its snapshot: " + material.key);
    }
    for (const auto& file : data.material_sources) {
        checkpoint(cancel);
        const auto loaded = take(clone.assets().source(control::wide(file.path)));
        if (loaded.path != file.path || loaded.source != file.source)
            throw Failure("dependency_mismatch", "Exported MS Source differs from its loaded text: " + file.path);
    }
    for (const auto& texture : data.textures) {
        checkpoint(cancel);
        const auto copied = take(clone.assets().texture(texture.key)).image;
        if (image_content(copied) != image_content(texture.image))
            throw Failure("dependency_mismatch", "Exported texture differs from the loaded RGBA input: " + texture.key);
    }
    std::set<std::string> recorded;
    for (const auto& mesh : data.meshes) {
        checkpoint(cancel);
        auto loaded = take(clone.assets().mesh(mesh.key));
        const auto expected = mesh_hash(mesh);
        if (!loaded.mesh || mesh_hash(loaded) != expected)
            throw Failure("dependency_mismatch",
                          "Copied dependency differs from the loaded mesh content or slot defaults: " + mesh.key);
        if (mesh.key.starts_with("builtin/")) {
            const auto at = std::ranges::find(data.engine_assets, mesh.key, &assets::EngineDependency::key);
            if (at == data.engine_assets.end() || *at != installed_asset(mesh.key, assets::AssetKind::mesh) ||
                at->content_sha256 != expected)
                throw Failure("dependency_mismatch",
                              "Frozen engine declaration differs from the loaded mesh: " + mesh.key);
        } else if (recorded.insert(mesh.key).second)
            resources.push_back(
                {{"key", mesh.key}, {"digest_version", assets::mesh_digest_version}, {"content_sha256", expected}});
    }
    for (const auto& [key, map] : data.skies) {
        checkpoint(cancel);
        const auto loaded = take(clone.assets().sky_map(key)).map;
        const auto expected = sky_hash(*map);
        if (!loaded || sky_hash(*loaded) != expected)
            throw Failure("dependency_mismatch", "Copied dependency differs from the loaded sky map: " + key);
        if (recorded.insert(key).second)
            resources.push_back({{"key", key}, {"content_sha256", expected}});
    }
    data.manifest["engine_assets"] = engine_assets_json(data.engine_assets);
    data.manifest["material_resources"] = std::move(data.material_resources);
    data.manifest["resources"] = std::move(resources);
    data.manifest["project"] = "Project";
    data.manifest["scene"] = path_text(scene_path);
    if (!data.drafts.empty())
        write(directory / "drafts.json", data.drafts.dump(2) + "\n");
    finish(directory, data.manifest, cancel);
    return {{"directory", path_text(output)},
            {"manifest", path_text(output / "manifest.json")},
            {"project", path_text(output / "Project")}};
}
Json validate_repro(const std::filesystem::path& input) {
    const auto directory = platform::windows_file_path(input);
    const auto text = take(assets::read_file(directory / "manifest.json"));
    Json manifest;
    // Format-limit and duplicate-field failures are input errors, not I/O failures.
    try {
        manifest = assets::json_format::parse(text);
    } catch (const std::runtime_error& error) {
        throw Failure("invalid_request", error.what());
    }
    if (manifest.value("kind", "") != "repro" || !manifest.value("complete", false) ||
        manifest.at("protocol") != control::protocol_version || manifest.at("project") != "Project")
        throw Failure("invalid_request", "Not a completed compatible repro bundle");
    static_cast<void>(installed_assets(manifest));
    static_cast<void>(material_records(manifest));
    if (!manifest.contains("engine_sources") || !manifest.at("engine_sources").is_object())
        throw Failure("invalid_request", "Repro lacks engine Source dependencies; export it again");
    const auto engine = source::engine_sources(source::default_engine_root());
    if (!engine || engine_source_hashes(*engine) != manifest.at("engine_sources"))
        throw Failure("dependency_mismatch", "Installed engine Source differs from the published repro inputs");
    const auto& files = manifest.at("files");
    if (!files.is_array() || files.size() > 11000)
        throw Failure("invalid_request", "Invalid repro file list");
    std::unordered_map<std::string, const Json*> expected;
    for (const auto& file : files) {
        const auto name = file.at("path").get<std::string>();
        const std::filesystem::path path = control::wide(name);
        if (path.empty() || path.has_root_path() || path.lexically_normal() != path || *path.begin() == L".." ||
            name.find(':') != std::string::npos || path == L"manifest.json" || generated_cache(path) ||
            !expected.emplace(path_text(path), &file).second)
            throw Failure("invalid_request", "Invalid or duplicate bundle path");
    }
    // Reject additions as well as missing/modified inputs. Generated Cache is
    // outside the publication and may be created when opening the project.
    for (auto it = std::filesystem::recursive_directory_iterator(directory);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
        const auto relative = it->path().lexically_relative(directory);
        if (relative == L"manifest.json")
            continue;
        if (GetFileAttributesW(platform::windows_file_path(it->path()).c_str()) & FILE_ATTRIBUTE_REPARSE_POINT)
            throw Failure("dependency_mismatch", "Repro bundle cannot contain links");
        if (generated_cache(relative)) {
            if (it->is_directory())
                it.disable_recursion_pending();
            continue;
        }
        if (it->is_directory())
            continue;
        const auto found = expected.find(path_text(relative));
        if (!it->is_regular_file() || found == expected.end() ||
            it->file_size() != found->second->at("bytes").get<std::uintmax_t>() ||
            control::sha256_file(it->path()) != found->second->at("sha256").get<std::string>())
            throw Failure("dependency_mismatch", "Repro bundle file set or content changed");
        expected.erase(found);
    }
    if (!expected.empty())
        throw Failure("dependency_mismatch", "Repro bundle file is missing");
    auto project = take(editor::Project::open(input / "Project"));
    const auto scene = take(project.resolve(project.startup_scene()));
    preflight_resources(project, assets::json_format::parse(take(assets::read_file(scene))));
    return manifest;
}
Json write_samples(const std::filesystem::path& output, std::vector<Sample> samples, Json manifest,
                   const std::atomic_bool& cancel) {
    const auto directory = platform::windows_file_path(output);
    checkpoint(cancel);
    Com com;
    if (!std::filesystem::create_directory(directory))
        throw Failure("storage_error", "Artifact directory already exists");
    manifest["samples"] = Json::array();
    std::size_t index{};
    for (auto& sample : samples) {
        checkpoint(cancel);
        const auto name = "sample-" + std::to_string(++index);
        const auto& pixels = sample.pixels;
        const auto count = std::uint64_t(pixels.extent.width) * pixels.extent.height;
        if (!count || count * 4 != pixels.rgba.size() || count != pixels.depth.size())
            throw Failure("render_failure", "Capture size does not match accepted extent");
        png(pixels.extent.width, pixels.extent.height, pixels.rgba, directory / (name + ".png"));
        checkpoint(cancel);
        write(directory / (name + ".depth.f32"),
              {reinterpret_cast<const char*>(pixels.depth.data()), pixels.depth.size() * sizeof(float)});
        sample.metadata["color"] = name + ".png";
        sample.metadata["depth"] = name + ".depth.f32";
        sample.metadata["extent"] = {pixels.extent.width, pixels.extent.height};
        sample.metadata["depth_format"] =
            "float32-le, top-left, tightly packed, reversed-Z near=1 background=0; camera.far_depth is the geometry clipping boundary; unjittered";
        sample.metadata["color_format"] = "sRGB RGBA8, top-left; complete editor client area";
        write(directory / (name + ".json"), sample.metadata.dump(2) + "\n");
        manifest["samples"].push_back(std::move(sample.metadata));
    }
    finish(directory, manifest, cancel);
    return {{"directory", path_text(output)},
            {"manifest", path_text(output / "manifest.json")},
            {"samples", manifest["samples"]}};
}
}
