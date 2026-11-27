#include "detail/control_artifacts.hpp"
#include "editor/project.hpp"
#include "editor/session.hpp"
#include "assets/file.hpp"
#include "assets/json_format.hpp"
#include "assets/material_format.hpp"
#include "material/resources.hpp"
#include "foundation/map.hpp"
#include "foundation/set.hpp"
#include "foundation/sha256.hpp"
#include "foundation/result.hpp"
#include "platform/error.hpp"
#include "platform/utf.hpp"
#include "platform/windows_path.hpp"
#include "source/snapshot.hpp"
#include <Windows.h>
#include "assets/png.hpp"
#include <fstream>
#include <map>
#include <set>

namespace atelier::application::control_artifacts {
namespace {
using control::Failure;

Failure storage_error(const Error& error) {
    return Failure("storage_error", error.message);
}

std::unexpected<Failure> storage_failure(std::error_code error) {
    return std::unexpected(Failure("storage_error", platform::error_message(error)));
}

Failure dependency_error(const Error& error) {
    return Failure("dependency_mismatch", error.message);
}

std::expected<void, Failure> checkpoint(const WriteState& work) {
    if (work.cancelled())
        return std::unexpected(Failure("cancelled", "Artifact export cancelled"));
    return {};
}

std::expected<void, Failure> write(const std::filesystem::path& path, std::string_view bytes) {
    return editor::write_file(path, bytes, editor::WriteMode::create).transform_error(storage_error);
}

bool generated_cache(const std::filesystem::path& relative) {
    auto part = relative.begin();
    if (part == relative.end() || *part++ != L"Project")
        return false;
    return part != relative.end() && *part == L"Cache";
}

json::Result<json::Document> engine_source_hashes(Span<const language::SourceFile> files) {
    json::Builder hashes;
    for (const auto& file : files)
        if (file.path.starts_with("engine/"))
            ATELIER_TRY(
                hashes.set(hashes.root(), file.path, foundation::sha256(as_bytes(Span<const char>{file.source}))));
    return hashes.finish();
}

std::expected<std::string, Failure> mesh_hash(const assets::MeshAsset& asset) {
    return assets::mesh_content_sha256(asset).transform_error(dependency_error);
}

std::expected<assets::EngineDependency, Failure> installed_asset(std::string_view key, assets::AssetKind kind) {
    return assets::Reader{}.engine_dependency(key, kind).transform_error(dependency_error);
}

json::Result<json::Document> engine_assets_json(Span<const assets::EngineDependency> assets) {
    json::Builder result;
    ATELIER_TRY(result.set(result.root(), "version", 1u));
    ATELIER_TRY_BIND(records, result.array(result.root(), "records"));
    for (const auto& asset : assets) {
        const auto record = result.append_object(*records);
        ATELIER_TRY(result.set(record, "key", asset.key));
        result.set_validated(record, "kind", asset.kind == assets::AssetKind::material ? "material" : "mesh");
        ATELIER_TRY(result.set(record, "semantic_version", asset.semantic_version));
        ATELIER_TRY(result.set(record, "digest_version", asset.digest_version));
        ATELIER_TRY(result.set(record, "content_sha256", asset.content_sha256));
    }
    return result.finish();
}

bool valid_sha256(std::string_view digest) {
    return digest.size() == 64 &&
           std::ranges::all_of(digest, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::expected<Array<assets::EngineDependency>, Failure> installed_assets(json::Value manifest) {
    ATELIER_TRY_BIND(found, manifest.find("engine_assets"));
    if (!*found)
        return std::unexpected(Failure("invalid_request", "Repro lacks engine assets; export it again"));
    const auto version = [](json::Value value) -> std::expected<std::uint32_t, Failure> {
        ATELIER_TRY_BIND(number, value.u32());
        if (*number == 0)
            return std::unexpected(
                Failure("invalid_request", "Engine asset versions require positive uint32 integers"));
        return *number;
    };
    const auto object = **found;
    ATELIER_TRY(object.allowed_members({"version", "records"}));
    ATELIER_TRY_BIND(schema_value, object.at("version"));
    ATELIER_TRY_BIND(schema, version(*schema_value));
    if (*schema != 1)
        return std::unexpected(Failure("invalid_request", "Unknown engine assets schema; export it again"));
    ATELIER_TRY_BIND(records_value, object.at("records"));
    ATELIER_TRY_BIND(records, records_value->array());
    Set<std::string_view> keys;
    Array<assets::EngineDependency> result;
    for (const auto record : *records) {
        ATELIER_TRY(record.allowed_members({"key", "kind", "semantic_version", "digest_version", "content_sha256"}));
        ATELIER_TRY_BIND(key_text, record.string({"key"}));
        const std::string key{*key_text};
        if (!keys.add(*key_text))
            return std::unexpected(Failure("invalid_request", "Duplicate engine asset: " + key));
        ATELIER_TRY_BIND(semantic_value, record.at("semantic_version"));
        ATELIER_TRY_BIND(semantic, version(*semantic_value));
        ATELIER_TRY_BIND(encoding_value, record.at("digest_version"));
        ATELIER_TRY_BIND(encoding, version(*encoding_value));
        ATELIER_TRY_BIND(name, record.string({"kind"}));
        if (*name != "mesh" && *name != "material")
            return std::unexpected(Failure("dependency_mismatch", "Installed engine asset kind differs: " + key));
        const auto kind = *name == "material" ? assets::AssetKind::material : assets::AssetKind::mesh;
        if (*encoding !=
            (kind == assets::AssetKind::material ? assets::material_digest_version : assets::mesh_digest_version))
            return std::unexpected(Failure("invalid_request", "Unknown engine asset digest encoding; export it again"));
        ATELIER_TRY_BIND(digest, record.string({"content_sha256"}));
        if (!valid_sha256(*digest))
            return std::unexpected(Failure("invalid_request", "Invalid engine asset SHA-256: " + key));
        ATELIER_TRY_BIND(current, installed_asset(key, kind));
        if (current->semantic_version != *semantic)
            return std::unexpected(
                Failure("dependency_mismatch", "Installed engine asset semantic version differs: " + key));
        if (current->content_sha256 != *digest)
            return std::unexpected(Failure("dependency_mismatch", "Installed engine asset content differs: " + key));
        result.add(std::move(*current));
    }
    std::ranges::sort(result, {}, &assets::EngineDependency::key);
    return result;
}

std::expected<void, Failure> validate_material_records(json::Value manifest) {
    ATELIER_TRY_BIND(at, manifest.find("material_resources"));
    if (!*at || (**at).kind() != json::Kind::object)
        return std::unexpected(Failure("invalid_request", "Repro lacks material content digests; export it again"));
    const auto object = **at;
    ATELIER_TRY(object.allowed_members({"version", "records"}));
    ATELIER_TRY_BIND(version, object.u32({"version"}));
    if (*version != assets::json_format::json_bytes_version)
        return std::unexpected(Failure("invalid_request", "Unknown material content digest schema; export it again"));
    ATELIER_TRY_BIND(records, object.at("records"));
    ATELIER_TRY_BIND(items, records->object_items());
    for (const auto& [key, value] : *items) {
        if (key.empty() || value.kind() != json::Kind::string)
            return std::unexpected(Failure("invalid_request", "Invalid material content digest record"));
        const auto digest = *value.string();
        if (!valid_sha256(digest))
            return std::unexpected(Failure("invalid_request", "Invalid material content SHA-256: " + std::string{key}));
    }
    return {};
}

std::string string_field(json::Value object, std::string_view name) {
    const auto at = object.find(name);
    return at && *at && (**at).kind() == json::Kind::string ? std::string{*(**at).string()} : std::string{};
}

// Only schema resource slots are references. Do not inspect arbitrary strings,
// cached drafts, copied files or unknown extension properties.
template <class Reference>
std::expected<void, Failure> scene_references(json::Value scene, const Reference& reference) {
    if (const auto materials = scene.find("materials");
        materials && *materials && (**materials).kind() == json::Kind::array)
        for (const auto definition : *(**materials).array())
            if (definition.kind() == json::Kind::object) {
                ATELIER_TRY(reference(assets::AssetKind::material, string_field(definition, "source")));
                ATELIER_TRY(reference(assets::AssetKind::material, string_field(definition, "parent")));
                // Entries need their terminal MS declaration before a texture value
                // can be considered an execution dependency.
            }
    if (const auto entities = scene.find("entities"); entities && *entities && (**entities).kind() == json::Kind::array)
        for (const auto entity : *(**entities).array())
            if (const auto components = entity.find("components");
                components && *components && (**components).kind() == json::Kind::array)
                for (const auto component : *(**components).array())
                    if (component.kind() == json::Kind::object) {
                        ATELIER_TRY(reference(assets::AssetKind::mesh, string_field(component, "mesh")));
                        ATELIER_TRY(reference(assets::AssetKind::sky, string_field(component, "sky_map")));
                        ATELIER_TRY(reference(assets::AssetKind::procedural_graph, string_field(component, "graph")));
                    }
    return {};
}

// Only inspect published Scene resource slots and their mesh defaults/material
// inputs, never all copied files. The strict loader still owns project failures;
// this preflight identifies invalid engine keys before it can reject the load.
std::expected<void, Failure> preflight_resources(editor::Project& project, json::Value scene) {
    Set<std::pair<assets::AssetKind, std::string>> references;
    Array<std::string> materials;
    const auto reference = [&](assets::AssetKind kind, const std::string& key) -> std::expected<void, Failure> {
        if (key.empty() || !references.add({kind, key}))
            return {};
        if (key.starts_with("builtin/"))
            ATELIER_TRY(installed_asset(key, kind));
        if (kind == assets::AssetKind::material)
            materials.add(key);
        return {};
    };
    ATELIER_TRY(scene_references(scene, reference));
    // The cooked candidate supplies SOP dependencies; disconnected or bypassed nodes may name missing assets.
    // Collect before visiting materials; their ordinal need not follow meshes.
    Array<std::string> meshes;
    for (const auto& [kind, key] : references)
        if (kind == assets::AssetKind::mesh)
            meshes.add(key);
    for (const auto& key : meshes)
        if (auto mesh = project.assets().mesh(key))
            for (const auto& binding : mesh->materials)
                ATELIER_TRY(reference(assets::AssetKind::material, binding.key));
    Map<std::string, assets::MaterialAsset> authors;
    for (std::size_t i = 0; i < materials.size(); ++i)
        if (auto asset = project.assets().material(materials[i])) {
            if (const auto* source = std::get_if<assets::MaterialSourceAsset>(&asset->author)) {
                for (const auto& input : source->inputs)
                    ATELIER_TRY(reference(assets::AssetKind::texture, input.binding.asset_key));
            } else
                ATELIER_TRY(reference(assets::AssetKind::material,
                                      std::get<assets::MaterialInstanceAsset>(asset->author).parent_key));
            auto material_key = asset->key;
            authors.try_emplace(std::move(material_key), std::move(*asset));
        }
    const auto source_author = [&](std::string key) -> const assets::MaterialSourceAsset* {
        Set<std::string> visited;
        while (!key.empty() && visited.add(key)) {
            const auto* at = authors.find(key);
            if (!at)
                break;
            if (const auto* source = std::get_if<assets::MaterialSourceAsset>(&at->author))
                return source;
            key = std::get<assets::MaterialInstanceAsset>(at->author).parent_key;
        }
        return nullptr; // Strict scene loading diagnoses broken/cyclic parents.
    };
    const auto entries = [&](Span<const material::Override> values,
                             const assets::MaterialSourceAsset* source) -> std::expected<void, Failure> {
        if (!source)
            return {};
        for (const auto& value : values)
            if (value.enabled)
                if (const auto* texture = std::get_if<material::TextureBinding>(&value.value);
                    texture && std::ranges::find(source->inputs, value.path, &material::TextureDefault::path) !=
                                   source->inputs.end())
                    ATELIER_TRY(reference(assets::AssetKind::texture, texture->asset_key));
        return {};
    };
    for (const auto& [key, asset] : authors)
        if (const auto* instance = std::get_if<assets::MaterialInstanceAsset>(&asset.author))
            ATELIER_TRY(entries(instance->entries, source_author(key)));
    ATELIER_TRY_BIND(definitions, scene.find("materials"));
    if (*definitions && (**definitions).kind() == json::Kind::array) {
        ATELIER_TRY_BIND(items, (**definitions).array());
        Map<std::string, json::Value> locals;
        for (const auto definition : *items) {
            ATELIER_TRY_BIND(id, definition.string({"id"}));
            locals.try_emplace(*id, definition);
        }
        for (const auto definition : *items) {
            ATELIER_TRY_BIND(values, definition.find("entries"));
            if (*values) {
                // Wrappers live for this walk; all views borrow the immutable scene.
                const json::Value* current = &definition;
                Set<const json::Value*> visited;
                const assets::MaterialSourceAsset* source{};
                while (current && visited.add(current)) {
                    const auto key = string_field(*current, "source");
                    if (!key.empty()) {
                        source = source_author(key);
                        break;
                    }
                    ATELIER_TRY_BIND(parent, current->find("parent"));
                    if (!*parent)
                        break;
                    if ((**parent).kind() == json::Kind::string) {
                        source = source_author(std::string{*(**parent).string()});
                        break;
                    }
                    if ((**parent).kind() != json::Kind::object)
                        break;
                    ATELIER_TRY_BIND(ref, (**parent).find("$ref"));
                    if (!*ref)
                        break;
                    ATELIER_TRY_BIND(id, (**ref).string());
                    current = locals.find(*id);
                }
                ATELIER_TRY_BIND(decoded, assets::material_format::decode_entries(**values));
                ATELIER_TRY(entries(*decoded, source));
            }
        }
    }
    return {};
}

json::Result<json::Document> image_content(const material::ImageRef& image) {
    if (!image)
        return json::Builder{json::Kind::null}.finish();
    json::Builder content;
    ATELIER_TRY(content.set(content.root(), "width", image->width()));
    ATELIER_TRY(content.set(content.root(), "height", image->height()));
    content.set_validated(content.root(), "rgba_sha256", foundation::sha256(as_bytes(image->pixels())));
    return content.finish();
}

std::string json_hash(json::Value value) {
    const auto bytes = json::write(value);
    return foundation::sha256(as_bytes(bytes.view()));
}

json::Result<std::string> source_material_hash(const assets::MaterialAsset& asset, const language::SourceFile& file) {
    // The installed MS producer's versioned author/text encoding, not a runtime
    // program hash. Project/scene records additionally digest the input pixels.
    json::Builder content;
    content.set_validated(content.root(), "domain", "Atelier.MaterialSource");
    ATELIER_TRY(content.set(content.root(), "digest_version", assets::material_digest_version));
    ATELIER_TRY(content.set(content.root(), "key", asset.key));
    ATELIER_TRY_BIND(author, assets::material_format::encode(asset));
    content.set_validated(content.root(), "author", author->root());
    ATELIER_TRY(content.set(content.root(), "source_path", file.path));
    ATELIER_TRY(content.set(content.root(), "source", file.source));
    return json_hash(content.root());
}

json::Result<std::string> sky_hash(const rendering::SkyMap& map) {
    json::Builder content;
    ATELIER_TRY(content.set(content.root(), "width", map.width()));
    ATELIER_TRY(content.set(content.root(), "height", map.height()));
    ATELIER_TRY(content.set(content.root(), "scale_exponent", std::int64_t{map.scale_exponent()}));
    ATELIER_TRY_BIND(mean, content.array(content.root(), "mean"));
    for (const auto value : {map.mean().x, map.mean().y, map.mean().z})
        ATELIER_TRY(content.append(*mean, value));
    content.set_validated(content.root(), "pixels", foundation::sha256(as_bytes(map.pixels())));
    content.set_validated(content.root(), "cube", foundation::sha256(as_bytes(map.cube())));
    return json_hash(content.root());
}

struct ComApartment {
    HRESULT status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    ComApartment() = default;

    ComApartment(const ComApartment&) = delete;

    ComApartment& operator=(const ComApartment&) = delete;

    ~ComApartment() {
        if (SUCCEEDED(status))
            CoUninitialize();
    }

    std::expected<void, Failure> ready() const {
        if (FAILED(status))
            return std::unexpected(Failure("storage_error", "Cannot initialize image encoder"));
        return {};
    }
};

std::expected<void, Failure> output_path(json::Builder& result, std::string_view key,
                                         const std::filesystem::path& path) {
    const auto name = platform::utf16_to_utf8(path.generic_wstring());
    if (!name)
        return std::unexpected(Failure("invalid_request", "Invalid UTF-16 text"));
    result.set_validated(result.root(), key, *name);
    return {};
}

std::expected<json::Document, Failure> finish(const std::filesystem::path& output,
                                              const std::filesystem::path& directory, json::Builder& manifest,
                                              json::Builder& result, WriteState& work) {
    ATELIER_TRY(output_path(result, "directory", output));
    ATELIER_TRY(output_path(result, "manifest", output / "manifest.json"));
    ATELIER_TRY(checkpoint(work));
    ATELIER_TRY_BIND(files, manifest.array(manifest.root(), "files"));
    std::error_code error;
    auto entry = std::filesystem::recursive_directory_iterator(directory, error);
    if (error)
        return storage_failure(error);
    while (entry != std::filesystem::recursive_directory_iterator{}) {
        const auto& item = *entry;
        const auto status = item.status(error);
        if (error && !std::filesystem::status_known(status))
            return storage_failure(error);
        if (std::filesystem::is_regular_file(status) && !generated_cache(item.path().lexically_relative(directory))) {
            ATELIER_TRY(checkpoint(work));
            const auto path = platform::utf16_to_utf8(item.path().lexically_relative(directory).generic_wstring());
            if (!path)
                return std::unexpected(Failure("invalid_request", "Invalid UTF-16 text"));
            const auto bytes = item.file_size(error);
            if (error)
                return storage_failure(error);
            const auto digest = foundation::sha256_file(item.path());
            if (!digest)
                return std::unexpected(Failure("storage_error", digest.error()));
            const auto file = manifest.append_object(*files);
            manifest.set_validated(file, "path", *path);
            ATELIER_TRY(manifest.set(file, "bytes", bytes));
            manifest.set_validated(file, "sha256", *digest);
        }
        entry.increment(error);
        if (error)
            return storage_failure(error);
    }
    ATELIER_TRY(checkpoint(work));
    ATELIER_TRY(manifest.set(manifest.root(), "json_bytes_version", assets::json_format::json_bytes_version));
    ATELIER_TRY(manifest.set(manifest.root(), "complete", true));
    const auto encoded = json::write(manifest.root(), {true, true});
    if (encoded.size() > assets::json_format::maximum_bytes)
        return std::unexpected(Failure("resource_limit", "Artifact manifest exceeds the 16 MiB file limit"));
    if (!work.claim_outcome())
        return std::unexpected(Failure("cancelled", "Artifact export cancelled"));
    ATELIER_TRY(write(directory / "manifest.json", {encoded.data(), encoded.size()}));
    return result.finish();
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
std::string trace_html(json::Value trace) {
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
    // The trace writer supplies every field below; this is its presentation,
    // not a second schema decoder or an external JSON entry point.
    const auto text = [](json::Value value) {
        if (value.kind() == json::Kind::string)
            return html_text(*value.string());
        const auto bytes = json::write(value);
        return html_text({bytes.data(), bytes.size()});
    };
    html += "<div class=\"facts\">Scope: <code>" + text(*trace.at("scope")) +
            "</code><br>Recording sequence: " + text(*trace.at("recording_sequence")) +
            " · UI frame: " + text(*trace.at("ui_frame_id")) + "<br>Attempt: " + text(*trace.at("attempt")) +
            " · Frame result: <strong>" + text(*trace.at("frame_status")) + "</strong> · Observation: <strong>" +
            text(*trace.path({"observation", "status"})) + "</strong></div>";
    if (const auto error = *trace.find("error")) {
        const auto bytes = json::write(*error, {true});
        html += "<pre class=\"notice\">" + html_text({bytes.data(), bytes.size()}) + "</pre>";
    }
    if (*trace.string({"observation", "status"}) != "complete")
        html += "<p class=\"notice\">This observation is not complete. Missing commands or bindings are not inferred. "
                "See observation flags in the snapshot metadata below.</p>";
    const auto commands = *trace.at("commands")->array();
    html += "<h2>Recording order</h2>";
    if (commands.size() == 0)
        html += "<p>No dispatch was observed in this scope for this render attempt. No pipeline template is shown.</p>";
    else {
        html +=
            "<div class=\"scroll\"><svg role=\"img\" aria-label=\"Actual dispatch recording order\" viewBox=\"0 0 1000 " +
            std::to_string(commands.size() * 60) + "\">";
        std::size_t i = 0;
        for (const auto command : commands) {
            const auto y = std::to_string(i * 60);
            html += "<g transform=\"translate(0 " + y + ")\" id=\"command-" + text(*command.at("ordinal")) +
                    "\"><rect x=\"40\" y=\"2\" width=\"510\" height=\"40\"/>";
            html += "<text x=\"54\" y=\"28\">" + text(*command.at("ordinal")) + " · " + text(*command.at("entry")) +
                    "</text><text x=\"580\" y=\"28\">" + text(*command.at("kind")) + " · groups " +
                    text(*command.at("groups")) + "</text>";
            if (i + 1 < commands.size())
                html += "<path d=\"M56 42 V55\"/><path class=\"arrow\" d=\"M52 52 L56 58 L60 52 Z\"/>";
            html += "</g>";
            ++i;
        }
        html += "</svg></div><h2>Actual bindings</h2><p>Expand a command to inspect its binding ports and view ranges. "
                "R numbers identify resources only within this snapshot. Resource origins are not inferred.</p>";
    }
    for (const auto command : commands) {
        const auto bindings = *command.at("bindings")->array();
        html += "<details><summary><code>" + text(*command.at("ordinal")) + " · " + text(*command.at("entry")) +
                "</code> — " + std::to_string(bindings.size()) + " bindings</summary>";
        html += "<div class=\"scroll\"><svg role=\"img\" aria-label=\"Actual resource bindings\" viewBox=\"0 0 1000 " +
                std::to_string(std::max<std::size_t>(80, bindings.size() * 36 + 12)) + "\">";
        html += "<rect x=\"8\" y=\"8\" width=\"110\" height=\"40\"/><text x=\"20\" y=\"34\">Command " +
                text(*command.at("ordinal")) + "</text>";
        std::size_t i = 0;
        for (const auto binding : bindings) {
            const auto y = std::to_string(i * 36 + 28);
            const auto label =
                binding.at("resource")->is_null() ? text(*binding.at("status")) : "R" + text(*binding.at("resource"));
            html += "<g id=\"binding-" + text(*command.at("ordinal")) + "-" + text(*binding.at("root_parameter")) +
                    "-" + text(*binding.at("array_index")) + "\">";
            html += "<path class=\"binding\" d=\"M118 28 H140 V" + y + " H170 M720 " + y + " H760\"/>";
            html += "<text x=\"178\" y=\"" + std::to_string(i * 36 + 33) + "\">root " +
                    text(*binding.at("root_parameter")) + ", " + text(*binding.at("kind")) + " · " +
                    text(*binding.at("name")) + "[" + text(*binding.at("array_index")) + "]</text>";
            html += "<rect x=\"760\" y=\"" + std::to_string(i * 36 + 12) +
                    "\" width=\"220\" height=\"30\"/><text x=\"774\" y=\"" + std::to_string(i * 36 + 33) + "\">" +
                    label + "</text></g>";
            ++i;
        }
        html +=
            "</svg></div><div class=\"scroll\"><table><caption>Installed binding details</caption>"
            "<thead><tr><th>Root / element</th><th>Name</th><th>Kind / status</th><th>Resource</th><th>View</th></tr></thead><tbody>";
        for (const auto binding : bindings)
            html += "<tr><td>" + text(*binding.at("root_parameter")) + " / " + text(*binding.at("array_index")) +
                    "</td><td><code>" + text(*binding.at("name")) + "</code></td><td>" + text(*binding.at("kind")) +
                    " / " + text(*binding.at("status")) + "</td><td>" + text(*binding.at("resource")) +
                    "</td><td><code>" + text(*binding.at("view")) + "</code></td></tr>";
        html += "</tbody></table></div></details>";
    }
    html +=
        "<h2>Resources</h2><div class=\"scroll\"><table><thead><tr><th>ID</th><th>Kind / name</th><th>Extent / DXGI format</th></tr></thead><tbody>";
    for (const auto resource : *trace.at("resources")->array())
        html += "<tr><td>R" + text(*resource.at("index")) + "</td><td>" + text(*resource.at("kind")) + " / <code>" +
                text(*resource.at("name")) + "</code></td><td>" + text(*resource.at("extent")) + " / " +
                text(*resource.at("format")) + "</td></tr>";
    html += "</tbody></table></div><details><summary>Snapshot metadata</summary><pre>";
    json::Builder metadata{trace};
    metadata.erase(metadata.root(), "commands");
    metadata.erase(metadata.root(), "resources");
    const auto bytes = json::write(metadata.root(), {true});
    return html + html_text({bytes.data(), bytes.size()}) + "</pre></details></main></html>\n";
}
}

std::expected<Repro, Failure> snapshot_resources(const editor::Session& document) {
    Repro data;
    ATELIER_TRY_BIND(serialized, document.serialize().transform_error(storage_error));
    data.scene = std::move(*serialized);
    ATELIER_TRY_BIND(scene, assets::json_format::parse(data.scene));
    std::map<std::string, assets::EngineDependency, std::less<>> engine;
    std::set<std::pair<assets::AssetKind, std::string>> references;
    Array<ObjectId> pending;
    const auto add_engine_asset = [&](assets::AssetKind kind, const std::string& key) -> std::expected<void, Failure> {
        const auto at = engine.find(key);
        if (at == engine.end()) {
            ATELIER_TRY_BIND(asset, installed_asset(key, kind));
            engine.emplace(key, std::move(*asset));
        } else if (at->second.kind != kind) {
            return std::unexpected(Failure("dependency_mismatch", "Engine asset used with conflicting kinds: " + key));
        }
        return {};
    };
    const auto reference = [&](assets::AssetKind kind, const std::string& key) -> std::expected<void, Failure> {
        if (key.empty() || !references.emplace(kind, key).second)
            return {};
        if (key.starts_with("builtin/"))
            ATELIER_TRY(add_engine_asset(kind, key));
        if (kind == assets::AssetKind::material)
            if (const auto object = document.loaded_material(key); object != ObjectId{})
                pending.add(object);
        return {};
    };
    const auto implicit_surface = [&]() -> std::expected<void, Failure> {
        const std::string key = "builtin/material/default-grid";
        references.emplace(assets::AssetKind::material, key);
        return add_engine_asset(assets::AssetKind::material, key);
    };
    // The serialized graph includes retained resource definitions, but not every
    // material a browser happened to load. Ordinary properties are not references.
    ATELIER_TRY(scene_references(scene->root(), reference));
    auto& runtime = document.world().runtime();
    Map<std::string, json::Value> locals;
    ATELIER_TRY_BIND(materials, scene->root().at("materials"));
    ATELIER_TRY_BIND(definitions, materials->array());
    for (const auto definition : *definitions)
        if (string_field(definition, "source").empty()) {
            ATELIER_TRY_BIND(id, definition.string({"id"}));
            locals.try_emplace(*id, definition);
        }
    if (!locals.empty()) {
        // local_object intentionally admits only Entities/Components. Enumerate
        // identities, but admit only the material LocalIds in the serialized graph
        // (including retained definitions), not unrelated drafts or Undo roots.
        Array<ObjectId> live;
        runtime.changed_objects(0, live);
        Set<std::string> found;
        for (const auto object : live)
            if (const auto id = document.document_identity(object))
                if (const auto key = std::to_string(*id); locals.contains(key)) {
                    pending.add(object);
                    found.add(key);
                }
        if (found.size() != locals.size())
            return std::unexpected(Failure("dependency_mismatch", "A scene material has no live author resource"));
    }
    const auto push_unique_mesh = [&](assets::MeshAsset snapshot) {
        if (snapshot.mesh && std::ranges::none_of(data.meshes, [&](const auto& entry) {
                return entry.key == snapshot.key && entry.mesh == snapshot.mesh &&
                       entry.materials == snapshot.materials;
            }))
            data.meshes.add(std::move(snapshot));
    };
    ATELIER_TRY_BIND(entities, document.world().members().transform_error(storage_error));
    const auto& types = document.presentation_types();
    for (const auto& entity : *entities) {
        ATELIER_TRY_BIND(components, world::components(runtime, entity.id()).transform_error(storage_error));
        for (const auto& component : *components) {
            ATELIER_TRY_BIND(type, runtime.type_of(component.id()).transform_error(storage_error));
            ATELIER_TRY_BIND(is_mesh, runtime.is_a(*type, types.static_mesh_component).transform_error(storage_error));
            ATELIER_TRY_BIND(is_generator,
                             runtime.is_a(*type, types.procedural_generator_component).transform_error(storage_error));
            ATELIER_TRY_BIND(is_environment,
                             runtime.is_a(*type, types.environment_component).transform_error(storage_error));
            if (*is_mesh) {
                ATELIER_TRY_BIND(
                    mesh, presentation::read_static_mesh(runtime, component.id()).transform_error(storage_error));
                ATELIER_TRY(reference(assets::AssetKind::mesh, mesh->mesh.source));
                assets::MeshAsset snapshot{mesh->mesh.source, {}, mesh->mesh.resolved};
                for (const auto& binding : mesh->mesh.defaults) {
                    ATELIER_TRY_BIND(
                        key, material::material_key(runtime, binding.resource.id()).transform_error(storage_error));
                    ATELIER_TRY(reference(assets::AssetKind::material, *key));
                    snapshot.materials.add({binding.slot, *key});
                    pending.add(binding.resource.id());
                }
                ATELIER_TRY_BIND(
                    bindings,
                    presentation::read_material_bindings(runtime, component.id()).transform_error(storage_error));
                for (const auto& binding : *bindings)
                    pending.add(binding.resource.id());
                if (snapshot.mesh)
                    for (std::size_t i = 0; i < snapshot.mesh->materials().size(); ++i) {
                        const auto slot = presentation::material_slot_name(*snapshot.mesh, i);
                        if (std::ranges::find(*bindings, slot, &presentation::MaterialBinding::slot) ==
                                bindings->end() &&
                            std::ranges::find(mesh->mesh.defaults, slot, &presentation::MaterialBinding::slot) ==
                                mesh->mesh.defaults.end()) {
                            // This implicit surface is not a cached editor draft root.
                            ATELIER_TRY(implicit_surface());
                        }
                    }
                push_unique_mesh(std::move(snapshot));
            } else if (*is_generator) {
                ATELIER_TRY_BIND(
                    proc,
                    presentation::read_procedural_binding(runtime, component.id()).transform_error(storage_error));
                ATELIER_TRY(reference(assets::AssetKind::procedural_graph, proc->source));
                if (!proc->source.empty() && !proc->source.starts_with("builtin/") &&
                    std::ranges::find(data.procedural_graphs, proc->source,
                                      &std::pair<std::string, std::string>::first) == data.procedural_graphs.end()) {
                    ATELIER_TRY_BIND(
                        encoded, document.procedural_graph_snapshot(proc->source).transform_error(dependency_error));
                    if (*encoded)
                        data.procedural_graphs.emplace(proc->source, std::move(**encoded));
                }
                ATELIER_TRY_BIND(
                    bindings,
                    presentation::read_material_bindings(runtime, component.id()).transform_error(storage_error));
                for (const auto& binding : *bindings)
                    pending.add(binding.resource.id());
                if (proc->output && proc->output->surface_mesh &&
                    std::ranges::find(*bindings, std::string_view{"default"}, &presentation::MaterialBinding::slot) ==
                        bindings->end()) {
                    ATELIER_TRY(implicit_surface());
                }
                for (const auto& asset : proc->resolved_assets) {
                    ATELIER_TRY(reference(assets::AssetKind::mesh, asset.key));
                    assets::MeshAsset snapshot{asset.key, {}, asset.mesh};
                    for (const auto& binding : asset.defaults) {
                        ATELIER_TRY_BIND(
                            key, material::material_key(runtime, binding.resource.id()).transform_error(storage_error));
                        ATELIER_TRY(reference(assets::AssetKind::material, *key));
                        snapshot.materials.add({binding.slot, *key});
                        pending.add(binding.resource.id());
                    }
                    if (snapshot.mesh) {
                        for (std::size_t i = 0; i < snapshot.mesh->materials().size(); ++i) {
                            const auto slot = presentation::material_slot_name(*snapshot.mesh, i);
                            if (std::ranges::find(asset.defaults, slot, &presentation::MaterialBinding::slot) ==
                                asset.defaults.end()) {
                                ATELIER_TRY(implicit_surface());
                            }
                        }
                        push_unique_mesh(std::move(snapshot));
                    }
                }
                if (proc->output) {
                    for (const auto& dep : proc->output->mesh_dependencies) {
                        if (dep.key.empty())
                            continue;
                        ATELIER_TRY(reference(assets::AssetKind::mesh, dep.key));
                        if (!dep.mesh)
                            continue;
                        assets::MeshAsset snapshot{dep.key, {}, dep.mesh};
                        snapshot.materials.reserve(dep.slot_materials.size());
                        for (const auto& [slot, mat_key] : dep.slot_materials) {
                            ATELIER_TRY(reference(assets::AssetKind::material, mat_key));
                            snapshot.materials.add({slot, mat_key});
                        }
                        push_unique_mesh(std::move(snapshot));
                    }
                }
            } else if (*is_environment) {
                ATELIER_TRY_BIND(sky,
                                 presentation::read_sky_map(runtime, component.id()).transform_error(storage_error));
                ATELIER_TRY(reference(assets::AssetKind::sky, sky->source));
                if (sky->resolved && std::ranges::none_of(data.skies, [&](const auto& entry) {
                        return entry.first == sky->source && entry.second == sky->resolved;
                    }))
                    data.skies.emplace(sky->source, sky->resolved);
            }
        }
    }
    Set<ObjectId> visited;
    json::Builder resources;
    ATELIER_TRY(resources.set(resources.root(), "version", assets::json_format::json_bytes_version));
    ATELIER_TRY_BIND(records, resources.object(resources.root(), "records"));
    const auto identity = [&](ObjectId object, const std::string& key) -> std::expected<std::string, Failure> {
        if (!key.empty())
            return key;
        const auto id = document.document_identity(object);
        if (!id || !locals.contains(std::to_string(*id)))
            return std::unexpected(
                Failure("dependency_mismatch", "Material parent is outside the serialized scene closure"));
        return "scene/" + std::to_string(*id);
    };
    const auto texture = [&](std::string_view path, const std::string& key, const ObjectRoot& root,
                             json::Builder& content, json::Value inputs) -> std::expected<void, Failure> {
        ATELIER_TRY(reference(assets::AssetKind::texture, key));
        material::ImageRef image;
        if (root.registered()) {
            ATELIER_TRY_BIND(state, material::read_texture(runtime, root.id()).transform_error(storage_error));
            if (state->source != key)
                return std::unexpected(
                    Failure("dependency_mismatch", "Material input is bound to a different texture key: " + key));
            image = std::move(state->image);
        }
        ATELIER_TRY_BIND(input, content.object(inputs, path));
        ATELIER_TRY(content.set(*input, "key", key));
        ATELIER_TRY_BIND(pixels, image_content(image));
        content.set_validated(*input, "image", pixels->root());
        if (!image)
            return {};
        const auto at = std::ranges::find(data.textures, key, &assets::TextureAsset::key);
        if (at == data.textures.end())
            data.textures.add({key, {}, std::move(image)});
        else if (at->image != image) {
            ATELIER_TRY_BIND(previous, image_content(at->image));
            if (*previous != *pixels)
                return std::unexpected(
                    Failure("dependency_mismatch", "One texture key has distinct loaded input pixels: " + key));
        }
        return {};
    };
    for (std::size_t i = 0; i < pending.size(); ++i) {
        const auto object = pending[i];
        if (!visited.add(object))
            continue;
        ATELIER_TRY_BIND(key, material::material_key(runtime, object).transform_error(storage_error));
        ATELIER_TRY(reference(assets::AssetKind::material, *key));
        if (document.unavailable(assets::AssetKind::material, *key))
            continue;
        ATELIER_TRY_BIND(object_identity, identity(object, *key));
        ATELIER_TRY_BIND(asset, document.material_asset(object).transform_error(storage_error));
        json::Builder content;
        ATELIER_TRY_BIND(encoded, assets::material_format::encode(*asset));
        content.set_validated(content.root(), "author", encoded->root());
        ATELIER_TRY_BIND(inputs, content.object(content.root(), "inputs"));
        if (std::holds_alternative<assets::MaterialSourceAsset>(asset->author)) {
            ATELIER_TRY_BIND(state, material::read_source(runtime, object).transform_error(storage_error));
            ATELIER_TRY_BIND(view, document.inspect_material(object).transform_error(storage_error));
            ATELIER_TRY(content.set_null(content.root(), "source"));
            if (view->source_file) {
                const auto& file = *view->source_file;
                const auto source = content.object_validated(content.root(), "source");
                ATELIER_TRY(content.set(source, "path", file.path));
                ATELIER_TRY(content.set(source, "text", file.source));
                const auto at = std::ranges::find(data.material_sources, file.path, &language::SourceFile::path);
                if (at == data.material_sources.end())
                    data.material_sources.add(file);
                else if (at->source != file.source)
                    return std::unexpected(
                        Failure("dependency_mismatch", "One MS Source path has distinct loaded text: " + file.path));
                if (key->starts_with("builtin/")) {
                    ATELIER_TRY_BIND(hash, source_material_hash(*asset, file));
                    if (*hash != engine.at(*key).content_sha256)
                        return std::unexpected(
                            Failure("dependency_mismatch",
                                    "Loaded engine material differs from its installed producer: " + *key));
                }
            } else if (key->starts_with("builtin/"))
                return std::unexpected(
                    Failure("dependency_mismatch", "Loaded engine material has no Source snapshot: " + *key));
            if (state->definition && state->default_textures.size() != state->definition->textures().size())
                return std::unexpected(
                    Failure("dependency_mismatch", "Material default texture bindings differ from its definition"));
            if (state->definition)
                for (std::size_t input = 0; input < state->definition->textures().size(); ++input) {
                    const auto& value = state->definition->textures()[input];
                    ATELIER_TRY(
                        texture(value.path, value.binding.asset_key, state->default_textures[input], content, *inputs));
                }
        } else {
            ATELIER_TRY_BIND(state, material::read_instance(runtime, object).transform_error(storage_error));
            const auto& author = std::get<assets::MaterialInstanceAsset>(asset->author);
            ATELIER_TRY(reference(assets::AssetKind::material, author.parent_key));
            if (state->parent.registered()) {
                ATELIER_TRY_BIND(parent_key,
                                 material::material_key(runtime, state->parent.id()).transform_error(storage_error));
                ATELIER_TRY_BIND(parent, identity(state->parent.id(), *parent_key));
                ATELIER_TRY(content.set(content.root(), "parent", *parent));
                pending.add(state->parent.id());
            } else
                ATELIER_TRY(content.set_null(content.root(), "parent"));
            if (key->empty()) {
                const auto doc_id = document.document_identity(object);
                content.set_validated(content.root(), "author", locals.at(std::to_string(*doc_id)));
            }
            ATELIER_TRY_BIND(surface, material::capture_program(runtime, object).transform_error(storage_error));
            if (state->textures.size() != state->entries.size())
                return std::unexpected(
                    Failure("dependency_mismatch", "Material texture bindings differ from its overrides"));
            if (!(*surface)->uses_default_material())
                for (std::size_t entry = 0; entry < state->entries.size(); ++entry) {
                    const auto& value = state->entries[entry];
                    const auto* binding = std::get_if<material::TextureBinding>(&value.value);
                    if (value.enabled && binding &&
                        std::ranges::find((*surface)->program().textures(), value.path,
                                          &source::MaterialTextureInput::key) != (*surface)->program().textures().end())
                        ATELIER_TRY(texture(value.path, binding->asset_key, state->textures[entry], content, *inputs));
                }
        }
        ATELIER_TRY(resources.set(*records, *object_identity, json_hash(content.root())));
        if (!key->empty() && !key->starts_with("builtin/"))
            data.materials.add(std::move(*asset));
    }
    // Compare every distinct loaded version before deduplicating the declaration.
    for (const auto& mesh : data.meshes)
        if (const auto at = engine.find(mesh.key); at != engine.end()) {
            ATELIER_TRY_BIND(hash, mesh_hash(mesh));
            if (*hash != at->second.content_sha256)
                return std::unexpected(Failure("dependency_mismatch",
                                               "Loaded engine mesh differs from its installed producer: " + mesh.key));
        }
    for (auto& [key, dependency] : engine) {
        static_cast<void>(key);
        data.engine_assets.add(std::move(dependency));
    }
    data.references.assign(references.begin(), references.end());
    data.material_resources = resources.finish();
    return data;
}

std::expected<void, Failure> validate_candidate_assets(const editor::Session& candidate, const ReproBundle& bundle) {
    const auto& expected = bundle.engine_assets;
    ATELIER_TRY_BIND(generators, candidate.world()
                                     .query(candidate.presentation_types().procedural_generator_component)
                                     .transform_error(storage_error));
    for (const auto component : *generators)
        if (const auto* error = candidate.procedural_generator_error(component))
            return std::unexpected(
                Failure("dependency_mismatch", "Repro generator could not reproduce its output: " + error->message));
    ATELIER_TRY_BIND(actual, snapshot_resources(candidate));
    for (const auto& item : actual->engine_assets)
        if (std::ranges::find(expected, item) == expected.end())
            return std::unexpected(
                Failure("dependency_mismatch", "Repro omits or changes an actual engine dependency: " + item.key));
    for (const auto& item : expected)
        if (std::ranges::find(actual->engine_assets, item) == actual->engine_assets.end())
            return std::unexpected(
                Failure("dependency_mismatch", "Repro declares an unused engine dependency: " + item.key));
    if (*bundle.manifest.root().at("material_resources") != actual->material_resources.root())
        return std::unexpected(
            Failure("dependency_mismatch",
                    "Repro material author, Source, parent or input pixels differ from the frozen content"));
    return {};
}

std::expected<json::Document, Failure> export_repro(const std::filesystem::path& output, Repro data, WriteState& work) {
    ATELIER_TRY(checkpoint(work));
    json::Builder manifest{data.manifest};
    Optional<ComApartment> com;
    if (!data.textures.empty()) {
        ATELIER_TRY(com.emplace().ready());
    }
    // Keep all worker filesystem operations in the Win32 extended namespace,
    // including metadata reads and directory entries. Public paths keep their
    // original spelling; relative manifest identities never contain the prefix.
    const auto native = platform::windows_file_path(output);
    if (!native)
        return storage_failure(native.error());
    const auto& directory = *native;
    if (!data.project.empty()) {
        auto input = platform::windows_file_path(data.project);
        if (!input)
            return storage_failure(input.error());
        data.project = std::move(*input);
    }
    std::error_code error;
    // Resolve existing ancestors by identity, including case aliases/junctions,
    // before creating any output that a source traversal could encounter.
    if (!data.project.empty()) {
        for (const auto& section : data.roots) {
            const auto resolved = std::filesystem::canonical(data.project / section, error);
            if (error)
                return storage_failure(error);
            const auto input = platform::windows_file_path(resolved);
            if (!input)
                return storage_failure(input.error());
            auto parent = std::filesystem::canonical(directory.parent_path(), error);
            if (error)
                return storage_failure(error);
            for (;;) {
                const auto native_parent = platform::windows_file_path(parent);
                if (!native_parent)
                    return storage_failure(native_parent.error());
                const auto same = std::filesystem::equivalent(*native_parent, *input, error);
                if (error)
                    return storage_failure(error);
                if (same) {
                    const auto name = platform::utf16_to_utf8(section.generic_wstring());
                    if (!name)
                        return std::unexpected(Failure("invalid_request", "Invalid UTF-16 text"));
                    return std::unexpected(Failure("invalid_destination",
                                                   "Repro output cannot be inside a project folder (" + *name + ")"));
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
        return storage_failure(error);
    if (!created)
        return std::unexpected(Failure("storage_error", "Artifact directory already exists"));
    const auto project = directory / "Project";
    if (data.project.empty()) {
        ATELIER_TRY(editor::Project::create(output / "Project").transform_error(storage_error));
    } else {
        std::uintmax_t bytes{};
        std::size_t files{};
        const auto count_file = [&](std::uintmax_t length) -> std::expected<void, Failure> {
            if (files == 10000 || length > 256 * 1024 * 1024 || bytes + length > 1024ull * 1024 * 1024)
                return std::unexpected(
                    Failure("resource_limit", "Repro exceeds 10000 files / 1 GiB / 256 MiB per file"));
            ++files;
            bytes += length;
            return {};
        };
        ATELIER_TRY_BIND(input_files, manifest.array(manifest.root(), "input_files"));
        const auto link = [](const std::filesystem::path& path) {
            // Both roots were normalized above; all callers append native children.
            const auto flags = GetFileAttributesW(path.c_str());
            return flags != INVALID_FILE_ATTRIBUTES && (flags & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        };
        const auto copy = [&](const std::filesystem::path& item) -> std::expected<void, Failure> {
            ATELIER_TRY(checkpoint(work));
            if (link(item))
                return std::unexpected(Failure("dependency_mismatch", "Repro export does not follow links"));
            const auto relative = item.lexically_relative(data.project);
            const auto target = project / relative;
            const auto status = std::filesystem::status(item, error);
            if (error && !std::filesystem::status_known(status))
                return storage_failure(error);
            if (std::filesystem::is_directory(status)) {
                std::filesystem::create_directories(target, error);
                if (error)
                    return storage_failure(error);
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
                    return std::unexpected(Failure("dependency_mismatch", "Repro export does not follow links"));
                const auto status = std::filesystem::status(identity, error);
                if (error && !std::filesystem::status_known(status))
                    return storage_failure(error);
                if (std::filesystem::is_regular_file(status)) {
                    ATELIER_TRY_BIND(bytes, assets::read_file(identity).transform_error(storage_error));
                    ATELIER_TRY_BIND(identity_data, assets::json_format::parse(*bytes));
                    ATELIER_TRY_BIND(id, identity_data->root().string({"id"}));
                    const auto key = "project/" + std::string{*id};
                    if (const auto at = std::ranges::find(data.textures, key, &assets::TextureAsset::key);
                        at != data.textures.end())
                        image = at->image;
                }
            }
            if (source != data.material_sources.end() || image) {
                std::filesystem::create_directories(target.parent_path(), error);
                if (error)
                    return storage_failure(error);
                // The asset key/path and sidecar stay unchanged. WIC detects the
                // lossless PNG container even when the original suffix was JPEG.
                if (image)
                    ATELIER_TRY(assets::write_png(image->width(), image->height(), image->pixels(), target)
                                    .transform_error(storage_error));
                else
                    ATELIER_TRY(write(target, source->source));
                // This is a file budget, not the decoded in-memory RGBA size.
                const auto length = std::filesystem::file_size(target, error);
                if (error)
                    return storage_failure(error);
                ATELIER_TRY(count_file(length));
                const auto name = platform::utf16_to_utf8(relative.generic_wstring());
                if (!name)
                    return std::unexpected(Failure("invalid_request", "Invalid UTF-16 text"));
                const auto digest = foundation::sha256_file(target);
                if (!digest)
                    return std::unexpected(Failure("storage_error", digest.error()));
                const auto file = manifest.append_object(*input_files);
                manifest.set_validated(file, "path", *name);
                manifest.set_validated(file, "sha256", *digest);
                ATELIER_TRY(manifest.set(file, "snapshot", true));
                return {};
            }
            if (!std::filesystem::is_regular_file(status))
                return std::unexpected(Failure("dependency_mismatch", "Unsupported project file"));
            const auto length = std::filesystem::file_size(item, error);
            if (error)
                return storage_failure(error);
            ATELIER_TRY(count_file(length));
            const auto stamp = std::filesystem::last_write_time(item, error);
            if (error)
                return storage_failure(error);
            const auto hash = foundation::sha256_file(item);
            if (!hash)
                return std::unexpected(Failure("storage_error", hash.error()));
            std::filesystem::create_directories(target.parent_path(), error);
            if (error)
                return storage_failure(error);
            const auto copied = std::filesystem::copy_file(item, target, error);
            if (error)
                return storage_failure(error);
            if (!copied)
                return std::unexpected(Failure("storage_error", "Cannot copy project dependency"));
            const auto current_stamp = std::filesystem::last_write_time(item, error);
            if (error)
                return storage_failure(error);
            if (stamp != current_stamp)
                return std::unexpected(Failure("dependency_mismatch", "Project dependency changed during export"));
            const auto current_size = std::filesystem::file_size(item, error);
            if (error)
                return storage_failure(error);
            if (length != current_size)
                return std::unexpected(Failure("dependency_mismatch", "Project dependency changed during export"));
            const auto current_hash = foundation::sha256_file(item);
            if (!current_hash)
                return std::unexpected(Failure("storage_error", current_hash.error()));
            if (*hash != *current_hash)
                return std::unexpected(Failure("dependency_mismatch", "Project dependency changed during export"));
            const auto copied_hash = foundation::sha256_file(target);
            if (!copied_hash)
                return std::unexpected(Failure("storage_error", copied_hash.error()));
            if (*hash != *copied_hash)
                return std::unexpected(Failure("dependency_mismatch", "Project dependency changed during export"));
            const auto name = platform::utf16_to_utf8(relative.generic_wstring());
            if (!name)
                return std::unexpected(Failure("invalid_request", "Invalid UTF-16 text"));
            const auto file = manifest.append_object(*input_files);
            manifest.set_validated(file, "path", *name);
            manifest.set_validated(file, "sha256", *hash);
            return {};
        };
        // Config and Source are copied whole: the configuration is rewritten and the
        // published code replaces Source below. Other folders hold assets; only the
        // files of the scene's assets and their references are copied.
        for (const auto* section : {"Config", "Source"}) {
            const auto root = data.project / section;
            if (link(root))
                return std::unexpected(Failure("dependency_mismatch", "Project sections cannot be links"));
            std::filesystem::create_directories(project / section, error);
            if (error)
                return storage_failure(error);
            auto entry = std::filesystem::recursive_directory_iterator(root, error);
            if (error)
                return storage_failure(error);
            while (entry != std::filesystem::recursive_directory_iterator{}) {
                ATELIER_TRY(copy(entry->path()));
                entry.increment(error);
                if (error)
                    return storage_failure(error);
            }
        }
        const auto sections = files;
        for (const auto& relative : data.files) {
            if (const auto top = *relative.begin(); top == L"Config" || top == L"Source")
                continue;
            for (auto parent = relative.parent_path(); !parent.empty(); parent = parent.parent_path())
                if (link(data.project / parent))
                    return std::unexpected(Failure("dependency_mismatch", "Repro export does not follow links"));
            ATELIER_TRY(copy(data.project / relative));
        }
        std::filesystem::create_directories(project / "Content", error);
        if (error)
            return storage_failure(error);
        // Recheck the copied set after copying, including additions/removals in Config and Source.
        std::size_t current_files{};
        for (const auto* section : {"Config", "Source"}) {
            auto entry = std::filesystem::recursive_directory_iterator(data.project / section, error);
            if (error)
                return storage_failure(error);
            while (entry != std::filesystem::recursive_directory_iterator{}) {
                const auto status = entry->status(error);
                if (error && !std::filesystem::status_known(status))
                    return storage_failure(error);
                if (std::filesystem::is_regular_file(status))
                    ++current_files;
                entry.increment(error);
                if (error)
                    return storage_failure(error);
            }
        }
        if (current_files != sections)
            return std::unexpected(Failure("dependency_mismatch", "Project file set changed during export"));
        for (const auto file : *input_files->array()) {
            ATELIER_TRY(checkpoint(work));
            if (*file.find("snapshot"))
                continue;
            const auto hash =
                foundation::sha256_file(data.project / platform::utf8_to_utf16_validated(*file.string({"path"})));
            if (!hash)
                return std::unexpected(Failure("storage_error", hash.error()));
            if (*hash != *file.string({"sha256"}))
                return std::unexpected(Failure("dependency_mismatch", "Project changed during export"));
        }
    }
    ATELIER_TRY(checkpoint(work));
    // Reproduce the code actually published into this Runtime. Drafts are separate.
    // Saved edits/additions may not have published. Keep them outside the
    // runnable Source tree, then materialize exactly the live inputs.
    Map<std::string, const language::SourceFile*> published;
    if (data.sources)
        for (const auto& file : data.sources->files)
            published.try_emplace(file.path, &file);
    if (data.ui_source)
        published.try_emplace(data.ui_source->path, &*data.ui_source);
    Array<std::filesystem::path> copied_code;
    auto entry = std::filesystem::recursive_directory_iterator(project / "Source", error);
    if (error)
        return storage_failure(error);
    while (entry != std::filesystem::recursive_directory_iterator{}) {
        const auto status = entry->status(error);
        if (error && !std::filesystem::status_known(status))
            return storage_failure(error);
        if (std::filesystem::is_regular_file(status) && entry->path().extension() == L".source")
            copied_code.add(entry->path());
        entry.increment(error);
        if (error)
            return storage_failure(error);
    }
    for (const auto& path : copied_code) {
        ATELIER_TRY(checkpoint(work));
        const auto relative = path.lexically_relative(project);
        const auto name = platform::utf16_to_utf8(relative.generic_wstring());
        if (!name)
            return std::unexpected(Failure("invalid_request", "Invalid UTF-16 text"));
        if (const auto* found = published.find(*name)) {
            const auto bytes = std::filesystem::file_size(path, error);
            if (error)
                return storage_failure(error);
            if (bytes == (*found)->source.size()) {
                ATELIER_TRY_BIND(text, assets::read_file(path).transform_error(storage_error));
                if (*text == (*found)->source) {
                    std::filesystem::remove(path, error);
                    if (error)
                        return storage_failure(error);
                    continue;
                }
            }
        }
        const auto archive = directory / "unpublished" / relative;
        std::filesystem::create_directories(archive.parent_path(), error);
        if (error)
            return storage_failure(error);
        std::filesystem::rename(path, archive, error);
        if (error)
            return storage_failure(error);
    }
    const auto publish_source = [&](const language::SourceFile& file) -> std::expected<void, Failure> {
        const auto name = platform::utf8_to_utf16(file.path);
        if (!name)
            return std::unexpected(Failure("invalid_request", "Invalid UTF-8 text"));
        const std::filesystem::path relative = *name;
        if (relative.is_absolute() || relative.empty() || *relative.begin() != L"Source" ||
            relative.lexically_normal() != relative)
            return std::unexpected(Failure("dependency_mismatch", "Published source is not project relative"));
        return write(project / relative, file.source);
    };
    // Engine definitions stay installation-owned. Pin the published inputs rather
    // than copying them into project scope or silently using a newer installation.
    if (data.sources) {
        ATELIER_TRY_BIND(hashes, engine_source_hashes(data.sources->files));
        manifest.set_validated(manifest.root(), "engine_sources", hashes->root());
    } else {
        ATELIER_TRY_BIND(files, source::engine_sources(source::default_engine_root()).transform_error(storage_error));
        ATELIER_TRY_BIND(hashes, engine_source_hashes(*files));
        manifest.set_validated(manifest.root(), "engine_sources", hashes->root());
    }
    if (data.sources)
        for (const auto& file : data.sources->files)
            if (!file.path.starts_with("engine/"))
                ATELIER_TRY(publish_source(file));
    if (data.ui_source)
        ATELIER_TRY(publish_source(*data.ui_source));
    for (const auto& file : data.material_sources) {
        ATELIER_TRY(checkpoint(work));
        if (file.path.starts_with("builtin/"))
            continue;
        if (const auto* at = published.find(file.path); at && (*at)->source != file.source)
            return std::unexpected(
                Failure("dependency_mismatch", "Gameplay and material Source captured different text at " + file.path));
        // Containment compares canonical paths, not the worker's extended Win32 spelling.
        const auto canonical = std::filesystem::canonical(project, error);
        if (error)
            return storage_failure(error);
        const auto name = platform::utf8_to_utf16(file.path);
        if (!name)
            return std::unexpected(Failure("invalid_request", "Invalid UTF-8 text"));
        ATELIER_TRY_BIND(target, assets::resolve_project_path(canonical, *name).transform_error(storage_error));
        ATELIER_TRY(editor::write_file(*target, file.source).transform_error(storage_error));
    }
    ATELIER_TRY_BIND(scene_id, control::unique_id());
    const auto scene_path = std::filesystem::path("Content/Scenes") / ("Control-" + *scene_id + ".scene");
    ATELIER_TRY(write(project / scene_path, data.scene));
    ATELIER_TRY_BIND(config_text, assets::read_file(project / "Config/Project.json").transform_error(storage_error));
    ATELIER_TRY_BIND(config_document, assets::json_format::parse(*config_text));
    json::Builder config{*config_document};
    if (data.ui_source)
        ATELIER_TRY(config.set(config.root(), "ui_source", data.ui_source->path));
    else
        config.erase(config.root(), "ui_source");
    const auto scene_name = platform::utf16_to_utf8(scene_path.generic_wstring());
    if (!scene_name)
        return std::unexpected(Failure("invalid_request", "Invalid UTF-16 text"));
    config.set_validated(config.root(), "startup_scene", *scene_name);
    const auto config_bytes = json::write(config.root(), {true, true});
    ATELIER_TRY(editor::write_file(project / "Config/Project.json", {config_bytes.data(), config_bytes.size()})
                    .transform_error(storage_error));
    ATELIER_TRY_BIND(opened, editor::Project::open(output / "Project").transform_error(storage_error));
    auto& clone = *opened;
    ATELIER_TRY_BIND(resources, manifest.array(manifest.root(), "resources"));
    for (const auto& material : data.materials) {
        ATELIER_TRY(checkpoint(work));
        // Publish the live draft only in this independent bundle, never in the source project.
        ATELIER_TRY(clone.save_material(material).transform_error(storage_error));
        ATELIER_TRY_BIND(copied, clone.assets().material(material.key).transform_error(storage_error));
        ATELIER_TRY_BIND(actual, assets::material_format::encode(*copied));
        ATELIER_TRY_BIND(expected, assets::material_format::encode(material));
        if (*actual != *expected)
            return std::unexpected(
                Failure("dependency_mismatch", "Exported material author differs from its snapshot: " + material.key));
    }
    for (const auto& [key, text] : data.procedural_graphs) {
        ATELIER_TRY(checkpoint(work));
        ATELIER_TRY(clone.save_procedural_graph(key, text).transform_error(storage_error));
        ATELIER_TRY_BIND(actual, clone.assets().procedural_graph(key).transform_error(storage_error));
        if (actual->text != text)
            return std::unexpected(
                Failure("dependency_mismatch", "Exported procedural graph differs from its snapshot: " + key));
    }
    for (const auto& file : data.material_sources) {
        ATELIER_TRY(checkpoint(work));
        std::wstring name;
        if (file.path.starts_with("builtin/")) {
            auto converted = platform::utf8_to_utf16(file.path);
            if (!converted)
                return std::unexpected(Failure("invalid_request", "Invalid UTF-8 text"));
            name = std::move(*converted);
        } else
            name = platform::utf8_to_utf16_validated(file.path); // Checked before publishing this unchanged path.
        ATELIER_TRY_BIND(loaded, clone.assets().source(name).transform_error(storage_error));
        if (loaded->path != file.path || loaded->source != file.source)
            return std::unexpected(
                Failure("dependency_mismatch", "Exported MS Source differs from its loaded text: " + file.path));
    }
    for (const auto& texture : data.textures) {
        ATELIER_TRY(checkpoint(work));
        ATELIER_TRY_BIND(copied, clone.assets().texture(texture.key).transform_error(storage_error));
        ATELIER_TRY_BIND(actual, image_content(copied->image));
        ATELIER_TRY_BIND(expected, image_content(texture.image));
        if (*actual != *expected)
            return std::unexpected(
                Failure("dependency_mismatch", "Exported texture differs from the loaded RGBA input: " + texture.key));
    }
    Set<std::string_view> recorded;
    for (const auto& mesh : data.meshes) {
        ATELIER_TRY(checkpoint(work));
        ATELIER_TRY_BIND(loaded, clone.assets().mesh(mesh.key).transform_error(storage_error));
        ATELIER_TRY_BIND(expected, mesh_hash(mesh));
        if (!loaded->mesh)
            return std::unexpected(
                Failure("dependency_mismatch",
                        "Copied dependency differs from the loaded mesh content or slot defaults: " + mesh.key));
        ATELIER_TRY_BIND(actual, mesh_hash(*loaded));
        if (*actual != *expected)
            return std::unexpected(
                Failure("dependency_mismatch",
                        "Copied dependency differs from the loaded mesh content or slot defaults: " + mesh.key));
        if (mesh.key.starts_with("builtin/")) {
            const auto at = std::ranges::find(data.engine_assets, mesh.key, &assets::EngineDependency::key);
            if (at != data.engine_assets.end()) {
                ATELIER_TRY_BIND(installed, installed_asset(mesh.key, assets::AssetKind::mesh));
                if (*at == *installed && at->content_sha256 == *expected)
                    continue;
            }
            return std::unexpected(
                Failure("dependency_mismatch", "Frozen engine declaration differs from the loaded mesh: " + mesh.key));
        } else if (recorded.add(mesh.key)) {
            const auto resource = manifest.append_object(*resources);
            ATELIER_TRY(manifest.set(resource, "key", mesh.key));
            ATELIER_TRY(manifest.set(resource, "digest_version", assets::mesh_digest_version));
            manifest.set_validated(resource, "content_sha256", *expected);
        }
    }
    for (const auto& [key, map] : data.skies) {
        ATELIER_TRY(checkpoint(work));
        ATELIER_TRY_BIND(loaded, clone.assets().sky_map(key).transform_error(storage_error));
        if (!loaded->map)
            return std::unexpected(
                Failure("dependency_mismatch", "Copied dependency differs from the loaded sky map: " + key));
        ATELIER_TRY_BIND(expected, sky_hash(*map));
        ATELIER_TRY_BIND(actual, sky_hash(*loaded->map));
        if (*actual != *expected)
            return std::unexpected(
                Failure("dependency_mismatch", "Copied dependency differs from the loaded sky map: " + key));
        if (recorded.add(key)) {
            const auto resource = manifest.append_object(*resources);
            ATELIER_TRY(manifest.set(resource, "key", key));
            ATELIER_TRY(manifest.set(resource, "digest_version", assets::json_format::json_bytes_version));
            manifest.set_validated(resource, "content_sha256", *expected);
        }
    }
    ATELIER_TRY_BIND(engine_assets, engine_assets_json(data.engine_assets));
    manifest.set_validated(manifest.root(), "engine_assets", engine_assets->root());
    manifest.set_validated(manifest.root(), "material_resources", data.material_resources.root());
    manifest.set_validated(manifest.root(), "project", "Project");
    manifest.set_validated(manifest.root(), "scene", *scene_name);
    if (data.drafts.root().size() != 0) {
        const auto bytes = json::write(data.drafts.root(), {true, true});
        ATELIER_TRY(write(directory / "drafts.json", {bytes.data(), bytes.size()}));
    }
    json::Builder result;
    ATELIER_TRY(output_path(result, "project", output / "Project"));
    return finish(output, directory, manifest, result, work);
}

std::expected<ReproBundle, Failure> validate_repro(const std::filesystem::path& input) {
    const auto io_failure = [](std::error_code cause) {
        return std::unexpected(Failure("operation_failed", platform::error_message(cause)));
    };
    const auto native = platform::windows_file_path(input);
    if (!native)
        return io_failure(native.error());
    const auto& directory = *native;
    ATELIER_TRY_BIND(text, assets::read_file(directory / "manifest.json").transform_error(storage_error));
    // Format-limit and duplicate-field failures are input errors, not I/O failures.
    ATELIER_TRY_BIND(document, assets::json_format::parse(*text));
    const auto manifest = document->root();
    ATELIER_TRY_BIND(kind, manifest.string({"kind"}));
    ATELIER_TRY_BIND(complete, manifest.boolean({"complete"}));
    ATELIER_TRY_BIND(protocol, manifest.u32({"protocol"}));
    ATELIER_TRY_BIND(project_name, manifest.string({"project"}));
    ATELIER_TRY_BIND(bytes_version, manifest.u32({"json_bytes_version"}));
    if (*kind != "repro" || !*complete || *protocol != control::protocol_version || *project_name != "Project" ||
        *bytes_version != assets::json_format::json_bytes_version)
        return std::unexpected(Failure("invalid_request", "Not a completed compatible repro bundle"));
    ATELIER_TRY_BIND(engine_assets, installed_assets(manifest));
    ATELIER_TRY(validate_material_records(manifest));
    ATELIER_TRY_BIND(resources_value, manifest.at("resources"));
    ATELIER_TRY_BIND(resources, resources_value->array());
    for (const auto resource : *resources) {
        ATELIER_TRY_BIND(version, resource.u32({"digest_version"}));
        if (*version != assets::mesh_digest_version && *version != assets::json_format::json_bytes_version)
            return std::unexpected(Failure("invalid_request", "Unknown resource digest encoding; export it again"));
    }
    ATELIER_TRY_BIND(engine_sources, manifest.find("engine_sources"));
    if (!*engine_sources || (**engine_sources).kind() != json::Kind::object)
        return std::unexpected(Failure("invalid_request", "Repro lacks engine Source dependencies; export it again"));
    ATELIER_TRY_BIND(engine, source::engine_sources(source::default_engine_root()).transform_error(dependency_error));
    ATELIER_TRY_BIND(hashes, engine_source_hashes(*engine));
    if (hashes->root() != **engine_sources)
        return std::unexpected(
            Failure("dependency_mismatch", "Installed engine Source differs from the published repro inputs"));
    ATELIER_TRY_BIND(files_value, manifest.at("files"));
    ATELIER_TRY_BIND(files, files_value->array());
    if (files->size() > 11000)
        return std::unexpected(Failure("invalid_request", "Invalid repro file list"));
    Map<std::string, json::Value> expected;
    for (const auto file : *files) {
        ATELIER_TRY_BIND(name, file.string({"path"}));
        const std::filesystem::path path = platform::utf8_to_utf16_validated(*name);
        const auto generic = platform::utf16_to_utf8(path.generic_wstring());
        if (!generic)
            return std::unexpected(Failure("invalid_request", "Invalid UTF-16 text"));
        if (path.empty() || path.has_root_path() || path.lexically_normal() != path || *path.begin() == L".." ||
            name->find(':') != std::string::npos || path == L"manifest.json" || generated_cache(path) ||
            !expected.try_emplace(*generic, file).inserted)
            return std::unexpected(Failure("invalid_request", "Invalid or duplicate bundle path"));
    }
    // Reject additions as well as missing/modified inputs. Generated Cache is
    // outside the publication and may be created when opening the project.
    const auto file_changed = [] {
        return std::unexpected(Failure("dependency_mismatch", "Repro bundle file set or content changed"));
    };
    std::error_code error;
    auto it = std::filesystem::recursive_directory_iterator(directory, error);
    if (error)
        return io_failure(error);
    for (; it != std::filesystem::recursive_directory_iterator{}; it.increment(error)) {
        if (error)
            return io_failure(error);
        const auto relative = it->path().lexically_relative(directory);
        const auto attributes = GetFileAttributesW(it->path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
            return io_failure({static_cast<int>(GetLastError()), std::system_category()});
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
            return std::unexpected(Failure("dependency_mismatch", "Repro bundle cannot contain links"));
        if (relative == L"manifest.json")
            continue;
        const auto status = it->status(error);
        if (error && !std::filesystem::status_known(status))
            return io_failure(error);
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
        const auto* found = expected.find(*name);
        if (!std::filesystem::is_regular_file(status) || !found)
            return file_changed();
        const auto bytes = it->file_size(error);
        if (error)
            return io_failure(error);
        ATELIER_TRY_BIND(expected_bytes, found->u64({"bytes"}));
        if (bytes != *expected_bytes)
            return file_changed();
        const auto digest = foundation::sha256_file(it->path());
        if (!digest)
            return std::unexpected(Failure("storage_error", digest.error()));
        ATELIER_TRY_BIND(expected_digest, found->string({"sha256"}));
        if (*digest != *expected_digest)
            return file_changed();
        expected.remove(*name);
    }
    if (error)
        return io_failure(error);
    if (!expected.empty())
        return std::unexpected(Failure("dependency_mismatch", "Repro bundle file is missing"));
    ATELIER_TRY_BIND(project, editor::Project::open(input / "Project").transform_error(storage_error));
    ATELIER_TRY_BIND(scene, manifest.string({"scene"}));
    if (platform::utf8_to_utf16_validated(*scene) != project->startup_scene().generic_wstring())
        return std::unexpected(Failure("dependency_mismatch", "Repro scene differs from the project startup scene"));
    ATELIER_TRY_BIND(scene_text, project->read_scene(project->startup_scene()).transform_error(storage_error));
    ATELIER_TRY_BIND(scene_document, assets::json_format::parse(*scene_text));
    ATELIER_TRY(preflight_resources(*project, scene_document->root()));
    return ReproBundle{std::move(*document), std::move(*engine_assets), std::move(*project), std::move(*scene_text)};
}

std::expected<json::Document, Failure> write_samples(const std::filesystem::path& output, Array<Sample> samples,
                                                     json::Document metadata, WriteState& work) {
    const auto native = platform::windows_file_path(output);
    if (!native)
        return storage_failure(native.error());
    const auto& directory = *native;
    ATELIER_TRY(checkpoint(work));
    ComApartment com;
    ATELIER_TRY(com.ready());
    std::error_code error;
    const auto created = std::filesystem::create_directory(directory, error);
    if (error)
        return storage_failure(error);
    if (!created)
        return std::unexpected(Failure("storage_error", "Artifact directory already exists"));
    json::Builder manifest{metadata};
    ATELIER_TRY_BIND(records, manifest.array(manifest.root(), "samples"));
    std::size_t index{};
    for (auto& sample : samples) {
        ATELIER_TRY(checkpoint(work));
        const auto name = "sample-" + std::to_string(++index);
        const auto& pixels = sample.pixels;
        const auto count = std::uint64_t(pixels.extent.width) * pixels.extent.height;
        if (!count || count > UINT_MAX / 4 || count * 4 != pixels.rgba.size() || count != pixels.depth.size())
            return std::unexpected(Failure("render_failure", "Capture size does not match accepted extent"));
        ATELIER_TRY(
            assets::write_png(pixels.extent.width, pixels.extent.height, pixels.rgba, directory / (name + ".png"))
                .transform_error(storage_error));
        ATELIER_TRY(checkpoint(work));
        ATELIER_TRY(write(directory / (name + ".depth.f32"),
                          {reinterpret_cast<const char*>(pixels.depth.data()), pixels.depth.size() * sizeof(float)}));
        json::Builder record{sample.metadata};
        record.set_validated(record.root(), "color", name + ".png");
        record.set_validated(record.root(), "depth", name + ".depth.f32");
        ATELIER_TRY_BIND(extent, record.array(record.root(), "extent"));
        record.append(*extent, std::uint64_t{pixels.extent.width});
        record.append(*extent, std::uint64_t{pixels.extent.height});
        record.set_validated(
            record.root(), "depth_format",
            "float32-le, top-left, tightly packed, reversed-Z near=1 background=0; camera.far_depth is the geometry clipping boundary; unjittered");
        record.set_validated(record.root(), "color_format", "sRGB RGBA8, top-left; complete editor client area");
        const auto bytes = json::write(record.root(), {true, true});
        ATELIER_TRY(write(directory / (name + ".json"), {bytes.data(), bytes.size()}));
        manifest.append(*records, record.root());
    }
    json::Builder result;
    result.set_validated(result.root(), "samples", *records);
    return finish(output, directory, manifest, result, work);
}

std::expected<json::Document, Failure> write_trace(const std::filesystem::path& output, rhi::CommandTrace snapshot,
                                                   json::Document metadata, WriteState& work) {
    ATELIER_TRY(checkpoint(work));
    const auto native = platform::windows_file_path(output);
    if (!native)
        return storage_failure(native.error());
    const auto& directory = *native;
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error)
        return storage_failure(error);
    json::Builder trace{metadata};
    trace.set_validated(trace.root(), "schema", "atelier.render.trace");
    ATELIER_TRY(trace.set(trace.root(), "version", 1u));
    const auto& info = snapshot.info();
    const auto status = !info.entered                                                                   ? "not_entered"
                        : !info.closed || info.truncated || info.unobserved_binding || info.interrupted ? "incomplete"
                                                                                                        : "complete";
    ATELIER_TRY(trace.set(trace.root(), "recording_sequence", info.recording_sequence));
    const auto observation = trace.object_validated(trace.root(), "observation");
    trace.set_validated(observation, "status", status);
    ATELIER_TRY(trace.set(observation, "truncated", info.truncated));
    ATELIER_TRY(trace.set(observation, "unobserved_binding", info.unobserved_binding));
    ATELIER_TRY(trace.set(observation, "interrupted", info.interrupted));
    ATELIER_TRY(trace.set(observation, "closed", info.closed));
    ATELIER_TRY_BIND(commands, trace.array(trace.root(), "commands"));
    ATELIER_TRY_BIND(resource_records, trace.array(trace.root(), "resources"));
    Map<std::pair<int, std::uint64_t>, std::size_t> resources;
    Map<std::uint64_t, std::size_t> kernels;
    constexpr std::array kinds{"unsupported", "texture_srv", "texture_uav",       "sampler",
                               "constants",   "cbv",         "indirect_arguments"};
    constexpr std::array statuses{"unknown", "valid", "dummy"};
    for (const auto& command : snapshot.commands()) {
        ATELIER_TRY(checkpoint(work));
        const auto [kernel, inserted] = kernels.try_emplace(command.kernel_key, kernels.size() + 1);
        const auto record = trace.append_object(*commands);
        ATELIER_TRY(trace.set(record, "ordinal", commands->size()));
        ATELIER_TRY(trace.set(record, "kernel", *kernel));
        ATELIER_TRY(trace.set(record, "entry", std::string_view{command.entry.data()}));
        trace.set_validated(record, "kind", command.indirect ? "dispatch_indirect" : "dispatch");
        if (command.indirect)
            ATELIER_TRY(trace.set_null(record, "groups"));
        else {
            ATELIER_TRY_BIND(groups, trace.array(record, "groups"));
            for (const auto count : command.groups)
                trace.append(*groups, std::uint64_t{count});
        }
        ATELIER_TRY_BIND(bindings, trace.array(record, "bindings"));
        for (const auto& owned : snapshot.bindings().slice(command.first_binding, command.binding_count)) {
            const auto& binding = owned.detail;
            const auto entry = trace.append_object(*bindings);
            ATELIER_TRY(trace.set_null(entry, "resource"));
            if (binding.resource_key && binding.status == kGfxObserved_Valid) {
                // SRV and UAV views share texture identity; sampler slots and GPU
                // allocations use separate namespaces. Native keys never escape.
                const auto kind = binding.kind == kGfxObserved_TextureUAV ? kGfxObserved_TextureSRV : binding.kind;
                const auto [item, added] = resources.try_emplace({kind, binding.resource_key}, resources.size() + 1);
                ATELIER_TRY(trace.set(entry, "resource", *item));
                if (added) {
                    const auto resource = trace.append_object(*resource_records);
                    ATELIER_TRY(trace.set(resource, "index", *item));
                    trace.set_validated(resource, "kind", kind == kGfxObserved_TextureSRV ? "texture" : kinds[kind]);
                    ATELIER_TRY(trace.set(resource, "name", std::string_view{owned.resource_name.data()}));
                    ATELIER_TRY_BIND(extent, trace.array(resource, "extent"));
                    for (const auto size : std::array<std::uint64_t, 3>{binding.width, binding.height, binding.depth})
                        trace.append(*extent, size);
                    ATELIER_TRY(trace.set(resource, "format", static_cast<std::uint32_t>(binding.resource_format)));
                }
            }
            ATELIER_TRY(trace.set(entry, "name", std::string_view{owned.name.data()}));
            ATELIER_TRY(trace.set(entry, "root_parameter", binding.root_parameter));
            ATELIER_TRY(trace.set(entry, "array_index", binding.array_index));
            trace.set_validated(entry, "kind", kinds[binding.kind]);
            trace.set_validated(entry, "status", statuses[binding.status]);
            const auto view = trace.object_validated(entry, "view");
            ATELIER_TRY(trace.set(view, "format", static_cast<std::uint32_t>(binding.format)));
            ATELIER_TRY(trace.set(view, "first_mip", binding.first_mip));
            ATELIER_TRY(trace.set(view, "mip_count", binding.mip_count));
            ATELIER_TRY(trace.set(view, "first_slice", binding.first_slice));
            ATELIER_TRY(trace.set(view, "slice_count", binding.slice_count));
            ATELIER_TRY(trace.set(view, "byte_offset", binding.byte_offset));
            ATELIER_TRY(trace.set(view, "byte_size", binding.byte_size));
        }
    }
    const auto bytes = json::write(trace.root(), {true, true});
    ATELIER_TRY(write(directory / "trace.json", {bytes.data(), bytes.size()}));
    ATELIER_TRY(checkpoint(work));
    ATELIER_TRY(write(directory / "trace.html", trace_html(trace.root())));
    json::Builder manifest{metadata};
    const auto entry = manifest.object_validated(manifest.root(), "trace");
    manifest.set_validated(entry, "json", "trace.json");
    manifest.set_validated(entry, "html", "trace.html");
    manifest.set_validated(entry, "observation", status);
    ATELIER_TRY(manifest.set(entry, "command_count", commands->size()));
    json::Builder result;
    ATELIER_TRY(output_path(result, "json", output / "trace.json"));
    ATELIER_TRY(output_path(result, "html", output / "trace.html"));
    result.set_validated(result.root(), "observation", status);
    result.set_validated(result.root(), "frame_status", *trace.root().at("frame_status"));
    return finish(output, directory, manifest, result, work);
}
}
