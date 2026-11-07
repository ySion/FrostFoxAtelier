#include "detail/control_artifacts.hpp"
#include "editor/project.hpp"
#include "editor/session.hpp"
#include "assets/file.hpp"
#include "assets/json_format.hpp"
#include "assets/material_format.hpp"
#include "material/resources.hpp"
#include "foundation/sha256.hpp"
#include "foundation/result.hpp"
#include "platform/error.hpp"
#include "platform/utf.hpp"
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

void checkpoint(const std::atomic_bool& cancelled) {
    if (cancelled.load())
        throw Failure("cancelled", "Artifact export cancelled");
}

std::expected<void, Failure> write(const std::filesystem::path& path, std::string_view bytes) {
    const auto native = platform::windows_file_path(path);
    if (!native)
        return std::unexpected(Failure("storage_error", platform::error_message(native.error())));
    std::error_code error;
    std::filesystem::create_directories(native->parent_path(), error);
    if (error)
        return std::unexpected(Failure("storage_error", platform::error_message(error)));
    const auto saved = editor::write_file(*native, bytes, editor::WriteMode::create);
    if (!saved)
        return std::unexpected(Failure("storage_error", saved.error().message));
    return {};
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
            hashes[file.path] = foundation::sha256(std::as_bytes(std::span(file.source)));
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
        const auto semantic = version(record.at("semantic_version"));
        const auto encoding = version(record.at("digest_version"));
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
            {"rgba_sha256", foundation::sha256(std::as_bytes(image->pixels()))}};
}

std::string json_hash(const Json& value) {
    const auto bytes = value.dump();
    return foundation::sha256(std::as_bytes(std::span(bytes)));
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
                            {"pixels", foundation::sha256(std::as_bytes(map.pixels()))},
                            {"cube", foundation::sha256(std::as_bytes(map.cube()))}}
                           .dump();
    return foundation::sha256(std::as_bytes(std::span(bytes)));
}

struct Com {
    Com() {
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
            throw Failure("storage_error", "Cannot initialize image encoder");
    }

    ~Com() { CoUninitialize(); }
};

std::expected<void, Failure> png(std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> rgba,
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
    const auto native = platform::windows_file_path(path);
    if (!native)
        return std::unexpected(Failure("storage_error", platform::error_message(native.error())));
    hr(stream->InitializeFromFilename(native->c_str(), GENERIC_WRITE));
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
    return {};
}

std::expected<void, Failure> finish(const std::filesystem::path& directory, Json& manifest,
                                    const std::atomic_bool& cancel) {
    checkpoint(cancel);
    auto files = Json::array();
    std::error_code error;
    auto entry = std::filesystem::recursive_directory_iterator(directory, error);
    if (error)
        return std::unexpected(Failure("storage_error", platform::error_message(error)));
    while (entry != std::filesystem::recursive_directory_iterator{}) {
        const auto& item = *entry;
        const auto status = item.status(error);
        if (error && !std::filesystem::status_known(status))
            return std::unexpected(Failure("storage_error", platform::error_message(error)));
        if (std::filesystem::is_regular_file(status) && !generated_cache(item.path().lexically_relative(directory))) {
            checkpoint(cancel);
            const auto path = platform::utf16_to_utf8(item.path().lexically_relative(directory).generic_wstring());
            if (!path)
                return std::unexpected(Failure("invalid_request", "Invalid UTF-16 text"));
            const auto bytes = item.file_size(error);
            if (error)
                return std::unexpected(Failure("storage_error", platform::error_message(error)));
            const auto digest = foundation::sha256_file(item.path());
            if (!digest)
                return std::unexpected(Failure("storage_error", digest.error()));
            files.push_back({{"path", *path}, {"bytes", bytes}, {"sha256", *digest}});
        }
        entry.increment(error);
        if (error)
            return std::unexpected(Failure("storage_error", platform::error_message(error)));
    }
    checkpoint(cancel);
    manifest["files"] = std::move(files);
    manifest["complete"] = true;
    const auto encoded = manifest.dump(2) + "\n";
    if (encoded.size() > assets::json_format::maximum_bytes)
        throw Failure("resource_limit", "Artifact manifest exceeds the 16 MiB file limit");
    return write(directory / "manifest.json", encoded);
}

std::string html_text(std::string_view text) {
    std::string escaped;
    for (const auto c : text)
        switch (c) {
        case '&':
            escaped += "&amp;";
            break;
        case '<':
            escaped += "&lt;";
            break;
        case '>':
            escaped += "&gt;";
            break;
        case '"':
            escaped += "&quot;";
            break;
        case '\'':
            escaped += "&#39;";
            break;
        default:
            escaped += c;
            break;
        }
    return escaped;
}

// This view consumes only the exported facts. It has no pass names, settings
// branches, resource-state model or inferred producer/consumer edges.
std::string trace_html(const Json& trace) {
    std::string html = R"HTML(<!doctype html>
<html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline'">
<title>Recorded post-processing commands</title><style>
:root{color-scheme:dark;font:14px/1.5 'Segoe UI',sans-serif;background:#2e2e2e;color:#ccc}
body{margin:0}main{max-width:1200px;margin:auto;padding:24px}h1{font-size:24px;line-height:1.2;margin:0 0 12px;color:#eee}
h2{font-size:18px;margin:28px 0 10px;color:#eee}p{max-width:80ch}code,pre,svg text{font:13px/1.5 Consolas,monospace}
a{color:#b8cfff}a:focus-visible,summary:focus-visible{outline:2px solid #ddd;outline-offset:3px}
summary{cursor:pointer;padding:12px 4px;color:#eee}summary:hover{background:#3d3d3d}
details{border-bottom:1px solid #666}details[open]{padding-bottom:16px}pre{white-space:pre-wrap;overflow-wrap:anywhere}
.scroll{overflow:auto;margin:12px 0}.facts{padding:12px 16px;background:#191919}.notice{color:#f0d4a2}
svg{display:block;width:100%;min-width:850px}svg rect{fill:#191919;stroke:#888}svg text{fill:#ddd}
svg path{fill:none;stroke:#aaa}svg .binding{stroke-dasharray:3 4}svg .arrow{fill:#aaa}
table{border-collapse:collapse;width:100%;font-variant-numeric:tabular-nums}td,th{text-align:left;padding:8px;border-bottom:1px solid #555;vertical-align:top}
th{color:#eee}td code{overflow-wrap:anywhere}caption{text-align:left;margin-bottom:8px}
@media(max-width:600px){main{padding:16px}h1{font-size:22px}}
</style><main><h1>Recorded post-processing commands</h1>
<p>CPU command recording only. This is not GPU completion or proof of display.
Solid arrows show recording order; dashed lines show bindings, not dependencies.</p>
)HTML";
    const auto text = [](const Json& value) {
        return html_text(value.is_string() ? value.get<std::string>() : value.dump());
    };
    html += "<div class=\"facts\">Scope: <code>" + text(trace.at("scope")) +
            "</code><br>Recording sequence: " + text(trace.at("recording_sequence")) +
            " · UI frame: " + text(trace.at("ui_frame_id")) + "<br>Attempt: " + text(trace.at("attempt")) +
            " · Frame result: <strong>" + text(trace.at("frame_status")) + "</strong> · Observation: <strong>" +
            text(trace.at("observation").at("status")) + "</strong></div>";
    if (trace.contains("error"))
        html += "<pre class=\"notice\">" + html_text(trace.at("error").dump(2)) + "</pre>";
    if (trace.at("observation").at("status") != "complete")
        html += "<p class=\"notice\">This observation is not complete. Missing commands or bindings are not inferred. "
                "See observation flags in the snapshot metadata below.</p>";
    const auto& commands = trace.at("commands");
    html += "<h2>Recording order</h2>";
    if (commands.empty())
        html += "<p>No dispatch was observed in this scope for this render attempt. No pipeline template is shown.</p>";
    else {
        html +=
            "<div class=\"scroll\"><svg role=\"img\" aria-label=\"Actual dispatch recording order\" viewBox=\"0 0 1000 " +
            std::to_string(commands.size() * 60) + "\">";
        for (std::size_t i = 0; i < commands.size(); ++i) {
            const auto y = std::to_string(i * 60);
            const auto& command = commands[i];
            html += "<g transform=\"translate(0 " + y + ")\" id=\"command-" + text(command.at("ordinal")) +
                    "\"><rect x=\"40\" y=\"2\" width=\"510\" height=\"40\"/>";
            html += "<text x=\"54\" y=\"28\">" + text(command.at("ordinal")) + " · " + text(command.at("entry")) +
                    "</text><text x=\"580\" y=\"28\">" + text(command.at("kind")) + " · groups " +
                    text(command.at("groups")) + "</text>";
            if (i + 1 < commands.size())
                html += "<path d=\"M56 42 V55\"/><path class=\"arrow\" d=\"M52 52 L56 58 L60 52 Z\"/>";
            html += "</g>";
        }
        html += "</svg></div><h2>Actual bindings</h2><p>Expand a command to inspect its binding ports and view ranges. "
                "R numbers identify resources only within this snapshot. Resource origins are not inferred.</p>";
    }
    for (const auto& command : commands) {
        const auto& bindings = command.at("bindings");
        html += "<details><summary><code>" + text(command.at("ordinal")) + " · " + text(command.at("entry")) +
                "</code> — " + std::to_string(bindings.size()) + " bindings</summary>";
        html += "<div class=\"scroll\"><svg role=\"img\" aria-label=\"Actual resource bindings\" viewBox=\"0 0 1000 " +
                std::to_string(std::max<std::size_t>(80, bindings.size() * 36 + 12)) + "\">";
        html += "<rect x=\"8\" y=\"8\" width=\"110\" height=\"40\"/><text x=\"20\" y=\"34\">Command " +
                text(command.at("ordinal")) + "</text>";
        for (std::size_t i = 0; i < bindings.size(); ++i) {
            const auto& binding = bindings[i];
            const auto y = std::to_string(i * 36 + 28);
            const auto label =
                binding.at("resource").is_null() ? text(binding.at("status")) : "R" + text(binding.at("resource"));
            html += "<g id=\"binding-" + text(command.at("ordinal")) + "-" + text(binding.at("root_parameter")) + "-" +
                    text(binding.at("array_index")) + "\">";
            html += "<path class=\"binding\" d=\"M118 28 H140 V" + y + " H170 M720 " + y + " H760\"/>";
            html += "<text x=\"178\" y=\"" + std::to_string(i * 36 + 33) + "\">root " +
                    text(binding.at("root_parameter")) + ", " + text(binding.at("kind")) + " · " +
                    text(binding.at("name")) + "[" + text(binding.at("array_index")) + "]</text>";
            html += "<rect x=\"760\" y=\"" + std::to_string(i * 36 + 12) +
                    "\" width=\"220\" height=\"30\"/><text x=\"774\" y=\"" + std::to_string(i * 36 + 33) + "\">" +
                    label + "</text></g>";
        }
        html +=
            "</svg></div><div class=\"scroll\"><table><caption>Installed binding details</caption>"
            "<thead><tr><th>Root / element</th><th>Name</th><th>Kind / status</th><th>Resource</th><th>View</th></tr></thead><tbody>";
        for (const auto& binding : bindings)
            html += "<tr><td>" + text(binding.at("root_parameter")) + " / " + text(binding.at("array_index")) +
                    "</td><td><code>" + text(binding.at("name")) + "</code></td><td>" + text(binding.at("kind")) +
                    " / " + text(binding.at("status")) + "</td><td>" + text(binding.at("resource")) +
                    "</td><td><code>" + text(binding.at("view")) + "</code></td></tr>";
        html += "</tbody></table></div></details>";
    }
    html +=
        "<h2>Resources</h2><div class=\"scroll\"><table><thead><tr><th>ID</th><th>Kind / name</th><th>Extent / DXGI format</th></tr></thead><tbody>";
    for (const auto& resource : trace.at("resources"))
        html += "<tr><td>R" + text(resource.at("index")) + "</td><td>" + text(resource.at("kind")) + " / <code>" +
                text(resource.at("name")) + "</code></td><td>" + text(resource.at("extent")) + " / " +
                text(resource.at("format")) + "</td></tr>";
    html += "</tbody></table></div><details><summary>Snapshot metadata</summary><pre>";
    auto metadata = trace;
    metadata.erase("commands");
    metadata.erase("resources");
    return html + html_text(metadata.dump(2)) + "</pre></details></main></html>\n";
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

std::expected<Json, Failure> export_repro(const std::filesystem::path& output, Repro data,
                                          const std::atomic_bool& cancel) {
    checkpoint(cancel);
    std::optional<Com> com;
    if (!data.textures.empty())
        com.emplace();
    // Keep all worker filesystem operations in the Win32 extended namespace,
    // including metadata reads and directory entries. Public paths keep their
    // original spelling; relative manifest identities never contain the prefix.
    const auto native = platform::windows_file_path(output);
    if (!native)
        return std::unexpected(Failure("storage_error", platform::error_message(native.error())));
    const auto& directory = *native;
    if (!data.project.empty()) {
        auto input = platform::windows_file_path(data.project);
        if (!input)
            return std::unexpected(Failure("storage_error", platform::error_message(input.error())));
        data.project = std::move(*input);
    }
    std::error_code error;
    // Resolve existing ancestors by identity, including case aliases/junctions,
    // before creating any output that a source traversal could encounter.
    if (!data.project.empty()) {
        for (const auto& section : data.roots) {
            const auto resolved = std::filesystem::canonical(data.project / section, error);
            if (error)
                return std::unexpected(Failure("storage_error", platform::error_message(error)));
            const auto input = platform::windows_file_path(resolved);
            if (!input)
                return std::unexpected(Failure("storage_error", platform::error_message(input.error())));
            auto parent = std::filesystem::canonical(directory.parent_path(), error);
            if (error)
                return std::unexpected(Failure("storage_error", platform::error_message(error)));
            for (;;) {
                const auto native_parent = platform::windows_file_path(parent);
                if (!native_parent)
                    return std::unexpected(Failure("storage_error", platform::error_message(native_parent.error())));
                const auto same = std::filesystem::equivalent(*native_parent, *input, error);
                if (error)
                    return std::unexpected(Failure("storage_error", platform::error_message(error)));
                if (same) {
                    const auto name = platform::utf16_to_utf8(section.generic_wstring());
                    if (!name)
                        return std::unexpected(Failure("invalid_request", "Invalid UTF-16 text"));
                    throw Failure("invalid_destination",
                                  "Repro output cannot be inside a project folder (" + *name + ")");
                }
                const auto next = parent.parent_path();
                if (next == parent || next.empty())
                    break;
                parent = next;
            }
        }
    }
    const auto created = std::filesystem::create_directory(directory, error);
    if (error)
        return std::unexpected(Failure("storage_error", platform::error_message(error)));
    if (!created)
        return std::unexpected(Failure("storage_error", "Artifact directory already exists"));
    const auto project = directory / "Project";
    if (data.project.empty()) {
        const auto initialized = editor::Project::create(output / "Project");
        if (!initialized)
            return std::unexpected(Failure("storage_error", initialized.error().message));
    } else {
        std::uintmax_t bytes{};
        std::size_t files{};
        Json input_files = Json::array();
        const auto link = [](const std::filesystem::path& path) {
            // Both roots were normalized above; all callers append native children.
            const auto flags = GetFileAttributesW(path.c_str());
            return flags != INVALID_FILE_ATTRIBUTES && (flags & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        };
        const auto copy = [&](const std::filesystem::path& item) -> std::expected<void, Failure> {
            checkpoint(cancel);
            if (link(item))
                throw Failure("dependency_mismatch", "Repro export does not follow links");
            const auto relative = item.lexically_relative(data.project);
            const auto target = project / relative;
            const auto status = std::filesystem::status(item, error);
            if (error && !std::filesystem::status_known(status))
                return std::unexpected(Failure("storage_error", platform::error_message(error)));
            if (std::filesystem::is_directory(status)) {
                std::filesystem::create_directories(target, error);
                if (error)
                    return std::unexpected(Failure("storage_error", platform::error_message(error)));
                return {};
            }
            auto source = data.material_sources.begin();
            for (; source != data.material_sources.end(); ++source) {
                const auto named = platform::utf8_to_utf16(source->path);
                if (!named)
                    return std::unexpected(Failure("invalid_request", "Invalid UTF-8 text"));
                const auto path = relative.generic_wstring();
                if (CompareStringOrdinal(named->c_str(), -1, path.c_str(), -1, TRUE) == CSTR_EQUAL)
                    break;
            }
            material::ImageRef image;
            if (assets::is_texture_path(relative)) {
                auto identity = item;
                identity += L".asset.json";
                if (link(identity))
                    throw Failure("dependency_mismatch", "Repro export does not follow links");
                const auto status = std::filesystem::status(identity, error);
                if (error && !std::filesystem::status_known(status))
                    return std::unexpected(Failure("storage_error", platform::error_message(error)));
                if (std::filesystem::is_regular_file(status)) {
                    const auto bytes = assets::read_file(identity);
                    if (!bytes)
                        return std::unexpected(Failure("storage_error", bytes.error().message));
                    const auto key = "project/" + assets::json_format::parse(*bytes).at("id").get<std::string>();
                    if (const auto at = std::ranges::find(data.textures, key, &assets::TextureAsset::key);
                        at != data.textures.end())
                        image = at->image;
                }
            }
            if (source != data.material_sources.end() || image) {
                std::filesystem::create_directories(target.parent_path(), error);
                if (error)
                    return std::unexpected(Failure("storage_error", platform::error_message(error)));
                // The asset key/path and sidecar stay unchanged. WIC detects the
                // lossless PNG container even when the original suffix was JPEG.
                if (image)
                    ATELIER_TRY(png(image->width(), image->height(), image->pixels(), target));
                else
                    ATELIER_TRY(write(target, source->source));
                // This is a file budget, not the decoded in-memory RGBA size.
                const auto length = std::filesystem::file_size(target, error);
                if (error)
                    return std::unexpected(Failure("storage_error", platform::error_message(error)));
                bytes += length;
                if (++files > 10000 || length > 256 * 1024 * 1024 || bytes > 1024ull * 1024 * 1024)
                    throw Failure("resource_limit", "Repro exceeds 10000 files / 1 GiB / 256 MiB per file");
                const auto name = platform::utf16_to_utf8(relative.generic_wstring());
                if (!name)
                    return std::unexpected(Failure("invalid_request", "Invalid UTF-16 text"));
                const auto digest = foundation::sha256_file(target);
                if (!digest)
                    return std::unexpected(Failure("storage_error", digest.error()));
                input_files.push_back({{"path", *name}, {"sha256", *digest}, {"snapshot", true}});
                return {};
            }
            const auto current_status = std::filesystem::status(item, error);
            if (error && !std::filesystem::status_known(current_status))
                return std::unexpected(Failure("storage_error", platform::error_message(error)));
            if (!std::filesystem::is_regular_file(current_status))
                throw Failure("dependency_mismatch", "Unsupported project file");
            const auto length = std::filesystem::file_size(item, error);
            if (error)
                return std::unexpected(Failure("storage_error", platform::error_message(error)));
            bytes += length;
            if (++files > 10000 || length > 256 * 1024 * 1024 || bytes > 1024ull * 1024 * 1024)
                throw Failure("resource_limit", "Repro exceeds 10000 files / 1 GiB / 256 MiB per file");
            const auto stamp = std::filesystem::last_write_time(item, error);
            if (error)
                return std::unexpected(Failure("storage_error", platform::error_message(error)));
            const auto hash = foundation::sha256_file(item);
            if (!hash)
                return std::unexpected(Failure("storage_error", hash.error()));
            std::filesystem::create_directories(target.parent_path(), error);
            if (error)
                return std::unexpected(Failure("storage_error", platform::error_message(error)));
            const auto copied = std::filesystem::copy_file(item, target, error);
            if (error)
                return std::unexpected(Failure("storage_error", platform::error_message(error)));
            if (!copied)
                return std::unexpected(Failure("storage_error", "Cannot copy project dependency"));
            const auto current_stamp = std::filesystem::last_write_time(item, error);
            if (error)
                return std::unexpected(Failure("storage_error", platform::error_message(error)));
            if (stamp != current_stamp)
                throw Failure("dependency_mismatch", "Project dependency changed during export");
            const auto current_size = std::filesystem::file_size(item, error);
            if (error)
                return std::unexpected(Failure("storage_error", platform::error_message(error)));
            if (length != current_size)
                throw Failure("dependency_mismatch", "Project dependency changed during export");
            const auto current_hash = foundation::sha256_file(item);
            if (!current_hash)
                return std::unexpected(Failure("storage_error", current_hash.error()));
            if (*hash != *current_hash)
                throw Failure("dependency_mismatch", "Project dependency changed during export");
            const auto copied_hash = foundation::sha256_file(target);
            if (!copied_hash)
                return std::unexpected(Failure("storage_error", copied_hash.error()));
            if (*hash != *copied_hash)
                throw Failure("dependency_mismatch", "Project dependency changed during export");
            const auto name = platform::utf16_to_utf8(relative.generic_wstring());
            if (!name)
                return std::unexpected(Failure("invalid_request", "Invalid UTF-16 text"));
            input_files.push_back({{"path", *name}, {"sha256", *hash}});
            return {};
        };
        // Config and Source are copied whole: the configuration is rewritten and the
        // published code replaces Source below. Other folders hold assets; only the
        // files of the scene's assets and their references are copied.
        for (const auto* section : {"Config", "Source"}) {
            const auto root = data.project / section;
            if (link(root))
                throw Failure("dependency_mismatch", "Project sections cannot be links");
            std::filesystem::create_directories(project / section, error);
            if (error)
                return std::unexpected(Failure("storage_error", platform::error_message(error)));
            auto entry = std::filesystem::recursive_directory_iterator(root, error);
            if (error)
                return std::unexpected(Failure("storage_error", platform::error_message(error)));
            while (entry != std::filesystem::recursive_directory_iterator{}) {
                ATELIER_TRY(copy(entry->path()));
                entry.increment(error);
                if (error)
                    return std::unexpected(Failure("storage_error", platform::error_message(error)));
            }
        }
        const auto sections = files;
        for (const auto& relative : data.files) {
            if (const auto top = *relative.begin(); top == L"Config" || top == L"Source")
                continue;
            for (auto parent = relative.parent_path(); !parent.empty(); parent = parent.parent_path())
                if (link(data.project / parent))
                    throw Failure("dependency_mismatch", "Repro export does not follow links");
            ATELIER_TRY(copy(data.project / relative));
        }
        std::filesystem::create_directories(project / "Content", error);
        if (error)
            return std::unexpected(Failure("storage_error", platform::error_message(error)));
        // Recheck the copied set after copying, including additions/removals in Config and Source.
        std::size_t current_files{};
        for (const auto* section : {"Config", "Source"}) {
            auto entry = std::filesystem::recursive_directory_iterator(data.project / section, error);
            if (error)
                return std::unexpected(Failure("storage_error", platform::error_message(error)));
            while (entry != std::filesystem::recursive_directory_iterator{}) {
                const auto status = entry->status(error);
                if (error && !std::filesystem::status_known(status))
                    return std::unexpected(Failure("storage_error", platform::error_message(error)));
                if (std::filesystem::is_regular_file(status))
                    ++current_files;
                entry.increment(error);
                if (error)
                    return std::unexpected(Failure("storage_error", platform::error_message(error)));
            }
        }
        if (current_files != sections)
            throw Failure("dependency_mismatch", "Project file set changed during export");
        for (const auto& file : input_files) {
            checkpoint(cancel);
            if (file.value("snapshot", false))
                continue;
            const auto hash = foundation::sha256_file(
                data.project / platform::utf8_to_utf16_validated(file.at("path").get<std::string>()));
            if (!hash)
                return std::unexpected(Failure("storage_error", hash.error()));
            if (*hash != file.at("sha256").get<std::string>())
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
    auto entry = std::filesystem::recursive_directory_iterator(project / "Source", error);
    if (error)
        return std::unexpected(Failure("storage_error", platform::error_message(error)));
    while (entry != std::filesystem::recursive_directory_iterator{}) {
        const auto status = entry->status(error);
        if (error && !std::filesystem::status_known(status))
            return std::unexpected(Failure("storage_error", platform::error_message(error)));
        if (std::filesystem::is_regular_file(status) && entry->path().extension() == L".source")
            copied_code.push_back(entry->path());
        entry.increment(error);
        if (error)
            return std::unexpected(Failure("storage_error", platform::error_message(error)));
    }
    for (const auto& path : copied_code) {
        checkpoint(cancel);
        const auto relative = path.lexically_relative(project);
        const auto name = platform::utf16_to_utf8(relative.generic_wstring());
        if (!name)
            return std::unexpected(Failure("invalid_request", "Invalid UTF-16 text"));
        const auto found = published.find(*name);
        if (found != published.end()) {
            const auto bytes = std::filesystem::file_size(path, error);
            if (error)
                return std::unexpected(Failure("storage_error", platform::error_message(error)));
            if (bytes == found->second->source.size()) {
                const auto text = assets::read_file(path);
                if (!text)
                    return std::unexpected(Failure("storage_error", text.error().message));
                if (*text == found->second->source) {
                    std::filesystem::remove(path, error);
                    if (error)
                        return std::unexpected(Failure("storage_error", platform::error_message(error)));
                    continue;
                }
            }
        }
        const auto archive = directory / "unpublished" / relative;
        std::filesystem::create_directories(archive.parent_path(), error);
        if (error)
            return std::unexpected(Failure("storage_error", platform::error_message(error)));
        std::filesystem::rename(path, archive, error);
        if (error)
            return std::unexpected(Failure("storage_error", platform::error_message(error)));
    }
    const auto publish_source = [&](const language::SourceFile& file) -> std::expected<void, Failure> {
        const auto name = platform::utf8_to_utf16(file.path);
        if (!name)
            return std::unexpected(Failure("invalid_request", "Invalid UTF-8 text"));
        const std::filesystem::path relative = *name;
        if (relative.is_absolute() || relative.empty() || *relative.begin() != L"Source" ||
            relative.lexically_normal() != relative)
            throw Failure("dependency_mismatch", "Published source is not project relative");
        return write(project / relative, file.source);
    };
    // Engine definitions stay installation-owned. Pin the published inputs rather
    // than copying them into project scope or silently using a newer installation.
    if (data.sources)
        data.manifest["engine_sources"] = engine_source_hashes(data.sources->files);
    else {
        const auto files = source::engine_sources(source::default_engine_root());
        if (!files)
            return std::unexpected(Failure("storage_error", files.error().message));
        data.manifest["engine_sources"] = engine_source_hashes(*files);
    }
    if (data.sources)
        for (const auto& file : data.sources->files)
            if (!file.path.starts_with("engine/"))
                ATELIER_TRY(publish_source(file));
    if (data.ui_source)
        ATELIER_TRY(publish_source(*data.ui_source));
    for (const auto& file : data.material_sources) {
        checkpoint(cancel);
        if (file.path.starts_with("builtin/"))
            continue;
        if (const auto at = published.find(file.path); at != published.end() && at->second->source != file.source)
            throw Failure("dependency_mismatch",
                          "Gameplay and material Source captured different text at " + file.path);
        // Containment compares canonical paths, not the worker's extended Win32 spelling.
        const auto canonical = std::filesystem::canonical(project, error);
        if (error)
            return std::unexpected(Failure("storage_error", platform::error_message(error)));
        const auto name = platform::utf8_to_utf16(file.path);
        if (!name)
            return std::unexpected(Failure("invalid_request", "Invalid UTF-8 text"));
        const auto target = assets::resolve_project_path(canonical, *name);
        if (!target)
            return std::unexpected(Failure("storage_error", target.error().message));
        const auto parent = platform::windows_file_path(target->parent_path());
        if (!parent)
            return std::unexpected(Failure("storage_error", platform::error_message(parent.error())));
        std::filesystem::create_directories(*parent, error);
        if (error)
            return std::unexpected(Failure("storage_error", platform::error_message(error)));
        const auto saved = editor::write_file(*target, file.source);
        if (!saved)
            return std::unexpected(Failure("storage_error", saved.error().message));
    }
    const auto scene_path = std::filesystem::path("Content/Scenes") / ("Control-" + control::unique_id() + ".scene");
    ATELIER_TRY(write(project / scene_path, data.scene));
    const auto config_text = assets::read_file(project / "Config/Project.json");
    if (!config_text)
        return std::unexpected(Failure("storage_error", config_text.error().message));
    auto config = assets::json_format::parse(*config_text);
    if (data.ui_source)
        config["ui_source"] = data.ui_source->path;
    else
        config.erase("ui_source");
    const auto scene_name = platform::utf16_to_utf8(scene_path.generic_wstring());
    if (!scene_name)
        return std::unexpected(Failure("invalid_request", "Invalid UTF-16 text"));
    config["startup_scene"] = *scene_name;
    const auto saved = editor::write_file(project / "Config/Project.json", config.dump(2) + "\n");
    if (!saved)
        return std::unexpected(Failure("storage_error", saved.error().message));
    auto opened = editor::Project::open(output / "Project");
    if (!opened)
        return std::unexpected(Failure("storage_error", opened.error().message));
    auto& clone = *opened;
    auto resources = Json::array();
    for (const auto& material : data.materials) {
        checkpoint(cancel);
        // Publish the live draft only in this independent bundle, never in the source project.
        const auto saved = clone.save_material(material);
        if (!saved)
            return std::unexpected(Failure("storage_error", saved.error().message));
        if (assets::material_format::encode(take(clone.assets().material(material.key))) !=
            assets::material_format::encode(material))
            throw Failure("dependency_mismatch", "Exported material author differs from its snapshot: " + material.key);
    }
    for (const auto& file : data.material_sources) {
        checkpoint(cancel);
        std::wstring name;
        if (file.path.starts_with("builtin/")) {
            auto converted = platform::utf8_to_utf16(file.path);
            if (!converted)
                return std::unexpected(Failure("invalid_request", "Invalid UTF-8 text"));
            name = std::move(*converted);
        } else
            name = platform::utf8_to_utf16_validated(file.path); // Checked before publishing this unchanged path.
        const auto loaded = clone.assets().source(name);
        if (!loaded)
            return std::unexpected(Failure("storage_error", loaded.error().message));
        if (loaded->path != file.path || loaded->source != file.source)
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
    data.manifest["scene"] = *scene_name;
    if (!data.drafts.empty())
        ATELIER_TRY(write(directory / "drafts.json", data.drafts.dump(2) + "\n"));
    ATELIER_TRY(finish(directory, data.manifest, cancel));
    const auto output_name = platform::utf16_to_utf8(output.generic_wstring());
    if (!output_name)
        return std::unexpected(Failure("invalid_request", "Invalid UTF-16 text"));
    const auto manifest_name = platform::utf16_to_utf8((output / "manifest.json").generic_wstring());
    if (!manifest_name)
        return std::unexpected(Failure("invalid_request", "Invalid UTF-16 text"));
    const auto project_name = platform::utf16_to_utf8((output / "Project").generic_wstring());
    if (!project_name)
        return std::unexpected(Failure("invalid_request", "Invalid UTF-16 text"));
    return Json{{"directory", *output_name}, {"manifest", *manifest_name}, {"project", *project_name}};
}

std::expected<Json, Failure> validate_repro(const std::filesystem::path& input) {
    const auto native = platform::windows_file_path(input);
    if (!native)
        return std::unexpected(Failure("operation_failed", platform::error_message(native.error())));
    const auto& directory = *native;
    const auto text = assets::read_file(directory / "manifest.json");
    if (!text)
        return std::unexpected(Failure("storage_error", text.error().message));
    Json manifest;
    // Format-limit and duplicate-field failures are input errors, not I/O failures.
    try {
        manifest = assets::json_format::parse(*text);
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
        const std::filesystem::path path = platform::utf8_to_utf16_validated(name);
        const auto generic = platform::utf16_to_utf8(path.generic_wstring());
        if (!generic)
            return std::unexpected(Failure("invalid_request", "Invalid UTF-16 text"));
        if (path.empty() || path.has_root_path() || path.lexically_normal() != path || *path.begin() == L".." ||
            name.find(':') != std::string::npos || path == L"manifest.json" || generated_cache(path) ||
            !expected.emplace(*generic, &file).second)
            throw Failure("invalid_request", "Invalid or duplicate bundle path");
    }
    // Reject additions as well as missing/modified inputs. Generated Cache is
    // outside the publication and may be created when opening the project.
    std::error_code error;
    auto it = std::filesystem::recursive_directory_iterator(directory, error);
    if (error)
        return std::unexpected(Failure("operation_failed", platform::error_message(error)));
    for (; it != std::filesystem::recursive_directory_iterator{}; it.increment(error)) {
        if (error)
            return std::unexpected(Failure("operation_failed", platform::error_message(error)));
        const auto relative = it->path().lexically_relative(directory);
        if (relative == L"manifest.json")
            continue;
        if (GetFileAttributesW(it->path().c_str()) & FILE_ATTRIBUTE_REPARSE_POINT)
            throw Failure("dependency_mismatch", "Repro bundle cannot contain links");
        const auto status = it->status(error);
        if (error && !std::filesystem::status_known(status))
            return std::unexpected(Failure("operation_failed", platform::error_message(error)));
        if (generated_cache(relative)) {
            if (std::filesystem::is_directory(status))
                it.disable_recursion_pending();
            continue;
        }
        if (std::filesystem::is_directory(status))
            continue;
        const auto name = platform::utf16_to_utf8(relative.generic_wstring());
        if (!name)
            return std::unexpected(Failure("invalid_request", "Invalid UTF-16 text"));
        const auto found = expected.find(*name);
        if (!std::filesystem::is_regular_file(status) || found == expected.end())
            throw Failure("dependency_mismatch", "Repro bundle file set or content changed");
        const auto bytes = it->file_size(error);
        if (error)
            return std::unexpected(Failure("operation_failed", platform::error_message(error)));
        if (bytes != found->second->at("bytes").get<std::uintmax_t>())
            throw Failure("dependency_mismatch", "Repro bundle file set or content changed");
        const auto digest = foundation::sha256_file(it->path());
        if (!digest)
            return std::unexpected(Failure("storage_error", digest.error()));
        if (*digest != found->second->at("sha256").get<std::string>())
            throw Failure("dependency_mismatch", "Repro bundle file set or content changed");
        expected.erase(found);
    }
    if (error)
        return std::unexpected(Failure("operation_failed", platform::error_message(error)));
    if (!expected.empty())
        throw Failure("dependency_mismatch", "Repro bundle file is missing");
    auto project = editor::Project::open(input / "Project");
    if (!project)
        return std::unexpected(Failure("storage_error", project.error().message));
    const auto scene = project->resolve(project->startup_scene());
    if (!scene)
        return std::unexpected(Failure("storage_error", scene.error().message));
    const auto scene_text = assets::read_file(*scene);
    if (!scene_text)
        return std::unexpected(Failure("storage_error", scene_text.error().message));
    preflight_resources(*project, assets::json_format::parse(*scene_text));
    return manifest;
}

std::expected<Json, Failure> write_samples(const std::filesystem::path& output, std::vector<Sample> samples,
                                           Json manifest, const std::atomic_bool& cancel) {
    const auto native = platform::windows_file_path(output);
    if (!native)
        return std::unexpected(Failure("storage_error", platform::error_message(native.error())));
    const auto& directory = *native;
    checkpoint(cancel);
    Com com;
    std::error_code error;
    const auto created = std::filesystem::create_directory(directory, error);
    if (error)
        return std::unexpected(Failure("storage_error", platform::error_message(error)));
    if (!created)
        return std::unexpected(Failure("storage_error", "Artifact directory already exists"));
    manifest["samples"] = Json::array();
    std::size_t index{};
    for (auto& sample : samples) {
        checkpoint(cancel);
        const auto name = "sample-" + std::to_string(++index);
        const auto& pixels = sample.pixels;
        const auto count = std::uint64_t(pixels.extent.width) * pixels.extent.height;
        if (!count || count * 4 != pixels.rgba.size() || count != pixels.depth.size())
            throw Failure("render_failure", "Capture size does not match accepted extent");
        ATELIER_TRY(png(pixels.extent.width, pixels.extent.height, pixels.rgba, directory / (name + ".png")));
        checkpoint(cancel);
        ATELIER_TRY(write(directory / (name + ".depth.f32"),
                          {reinterpret_cast<const char*>(pixels.depth.data()), pixels.depth.size() * sizeof(float)}));
        sample.metadata["color"] = name + ".png";
        sample.metadata["depth"] = name + ".depth.f32";
        sample.metadata["extent"] = {pixels.extent.width, pixels.extent.height};
        sample.metadata["depth_format"] =
            "float32-le, top-left, tightly packed, reversed-Z near=1 background=0; camera.far_depth is the geometry clipping boundary; unjittered";
        sample.metadata["color_format"] = "sRGB RGBA8, top-left; complete editor client area";
        ATELIER_TRY(write(directory / (name + ".json"), sample.metadata.dump(2) + "\n"));
        manifest["samples"].push_back(std::move(sample.metadata));
    }
    ATELIER_TRY(finish(directory, manifest, cancel));
    const auto output_name = platform::utf16_to_utf8(output.generic_wstring());
    if (!output_name)
        return std::unexpected(Failure("invalid_request", "Invalid UTF-16 text"));
    const auto manifest_name = platform::utf16_to_utf8((output / "manifest.json").generic_wstring());
    if (!manifest_name)
        return std::unexpected(Failure("invalid_request", "Invalid UTF-16 text"));
    return Json{{"directory", *output_name}, {"manifest", *manifest_name}, {"samples", manifest["samples"]}};
}

std::expected<Json, Failure> write_trace(const std::filesystem::path& output, rhi::CommandTrace snapshot, Json manifest,
                                         const std::atomic_bool& cancel) {
    checkpoint(cancel);
    const auto native = platform::windows_file_path(output);
    if (!native)
        return std::unexpected(Failure("storage_error", platform::error_message(native.error())));
    const auto& directory = *native;
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error)
        return std::unexpected(Failure("storage_error", platform::error_message(error)));
    auto trace = manifest;
    trace["schema"] = "atelier.render.trace";
    trace["version"] = 1;
    const auto& info = snapshot.info();
    const auto status = !info.entered                                                                   ? "not_entered"
                        : !info.closed || info.truncated || info.unobserved_binding || info.interrupted ? "incomplete"
                                                                                                        : "complete";
    trace["recording_sequence"] = info.recording_sequence;
    trace["observation"] = {{"status", status},
                            {"truncated", info.truncated},
                            {"unobserved_binding", info.unobserved_binding},
                            {"interrupted", info.interrupted},
                            {"closed", info.closed}};
    trace["commands"] = Json::array();
    trace["resources"] = Json::array();
    std::map<std::pair<int, std::uint64_t>, std::size_t> resources;
    std::map<std::uint64_t, std::size_t> kernels;
    constexpr std::array kinds{"unsupported", "texture_srv", "texture_uav",       "sampler",
                               "constants",   "cbv",         "indirect_arguments"};
    constexpr std::array statuses{"unknown", "valid", "dummy"};
    for (const auto& command : snapshot.commands()) {
        checkpoint(cancel);
        const auto [kernel, inserted] = kernels.try_emplace(command.kernel_key, kernels.size() + 1);
        Json record{{"ordinal", trace["commands"].size() + 1},
                    {"kernel", kernel->second},
                    {"entry", command.entry.data()},
                    {"kind", command.indirect ? "dispatch_indirect" : "dispatch"},
                    {"groups", command.indirect ? Json(nullptr) : Json(command.groups)},
                    {"bindings", Json::array()}};
        for (const auto& owned : snapshot.bindings().subspan(command.first_binding, command.binding_count)) {
            const auto& binding = owned.detail;
            Json resource = nullptr;
            if (binding.resource_key && binding.status == kGfxObserved_Valid) {
                // SRV and UAV views share texture identity; sampler slots and GPU
                // allocations use separate namespaces. Native keys never escape.
                const auto kind = binding.kind == kGfxObserved_TextureUAV ? kGfxObserved_TextureSRV : binding.kind;
                const auto [item, added] = resources.try_emplace({kind, binding.resource_key}, resources.size() + 1);
                resource = item->second;
                if (added)
                    trace["resources"].push_back({{"index", item->second},
                                                  {"kind", kind == kGfxObserved_TextureSRV ? "texture" : kinds[kind]},
                                                  {"name", owned.resource_name.data()},
                                                  {"extent", {binding.width, binding.height, binding.depth}},
                                                  {"format", static_cast<std::uint32_t>(binding.resource_format)}});
            }
            record["bindings"].push_back({{"name", owned.name.data()},
                                          {"root_parameter", binding.root_parameter},
                                          {"array_index", binding.array_index},
                                          {"kind", kinds[binding.kind]},
                                          {"status", statuses[binding.status]},
                                          {"resource", std::move(resource)},
                                          {"view",
                                           {{"format", static_cast<std::uint32_t>(binding.format)},
                                            {"first_mip", binding.first_mip},
                                            {"mip_count", binding.mip_count},
                                            {"first_slice", binding.first_slice},
                                            {"slice_count", binding.slice_count},
                                            {"byte_offset", binding.byte_offset},
                                            {"byte_size", binding.byte_size}}}});
        }
        trace["commands"].push_back(std::move(record));
    }
    ATELIER_TRY(write(directory / "trace.json", trace.dump(2) + "\n"));
    checkpoint(cancel);
    ATELIER_TRY(write(directory / "trace.html", trace_html(trace)));
    manifest["trace"] = {{"json", "trace.json"},
                         {"html", "trace.html"},
                         {"observation", status},
                         {"command_count", trace["commands"].size()}};
    ATELIER_TRY(finish(directory, manifest, cancel));
    const auto output_name = platform::utf16_to_utf8(output.generic_wstring());
    if (!output_name)
        return std::unexpected(Failure("invalid_request", "Invalid UTF-16 text"));
    const auto manifest_name = platform::utf16_to_utf8((output / "manifest.json").generic_wstring());
    if (!manifest_name)
        return std::unexpected(Failure("invalid_request", "Invalid UTF-16 text"));
    const auto json_name = platform::utf16_to_utf8((output / "trace.json").generic_wstring());
    if (!json_name)
        return std::unexpected(Failure("invalid_request", "Invalid UTF-16 text"));
    const auto html_name = platform::utf16_to_utf8((output / "trace.html").generic_wstring());
    if (!html_name)
        return std::unexpected(Failure("invalid_request", "Invalid UTF-16 text"));
    return Json{{"directory", *output_name}, {"manifest", *manifest_name}, {"json", *json_name},
                {"html", *html_name},        {"observation", status},      {"frame_status", trace["frame_status"]}};
}
}
