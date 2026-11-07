#include "application/play_link.hpp"
#include "application/diagnostics.hpp"
#include "assets/file.hpp"
#include "assets/json_format.hpp"
#include "assets/mesh_format.hpp"
#include "editor/project_code.hpp"
#include "editor/session.hpp"
#include "platform/utf.hpp"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace atelier::application {
namespace {
bool transfer(HANDLE pipe, void* storage, std::size_t bytes, bool write) {
    auto* cursor = static_cast<char*>(storage);
    while (bytes) {
        DWORD moved{};
        const auto count = static_cast<DWORD>(std::min<std::size_t>(bytes, 1 << 20));
        if (!(write ? WriteFile(pipe, cursor, count, &moved, nullptr)
                    : ReadFile(pipe, cursor, count, &moved, nullptr)) ||
            !moved)
            return false;
        cursor += moved;
        bytes -= moved;
    }
    return true;
}

}

struct LiveLink::Impl {
    HANDLE read;

    HANDLE write;

    std::mutex mutex;

    std::condition_variable wake;

    std::deque<std::string> outbox;

    std::vector<std::string> inbox;

    std::atomic_bool closed{};

    std::atomic_bool stopping{};

    std::thread reader;

    std::thread writer;

    Impl(HANDLE in, HANDLE out) : read(in), write(out) {}

    ~Impl() {
        stopping = true;
        wake.notify_all();
        // Blocking pipe I/O ends only when cancelled; a cancel issued between two
        // transfers is lost, so repeat it until the thread exits.
        for (auto* thread : {&reader, &writer})
            if (thread->joinable()) {
                while (WaitForSingleObject(thread->native_handle(), 10) == WAIT_TIMEOUT)
                    CancelSynchronousIo(thread->native_handle());
                thread->join();
            }
        CloseHandle(read);
        CloseHandle(write);
    }

    void finish() {
        closed = true;
        wake.notify_all();
    }

    void receive_loop() {
        try {
            while (!stopping) {
                std::uint32_t bytes{};
                if (!transfer(read, &bytes, sizeof(bytes), false) || !bytes || bytes > maximum_message)
                    break;
                std::string text(bytes, '\0');
                if (!transfer(read, text.data(), bytes, false))
                    break;
                std::lock_guard lock(mutex);
                inbox.push_back(std::move(text));
            }
        } catch (...) {
        }
        finish();
    }

    void send_loop() {
        try {
            for (;;) {
                std::string text;
                {
                    std::unique_lock lock(mutex);
                    wake.wait(lock, [&] { return stopping || closed || !outbox.empty(); });
                    if (stopping || closed)
                        break;
                    text = std::move(outbox.front());
                    outbox.pop_front();
                }
                const auto bytes = static_cast<std::uint32_t>(text.size());
                if (!transfer(write, const_cast<std::uint32_t*>(&bytes), sizeof(bytes), true) ||
                    !transfer(write, text.data(), text.size(), true))
                    break;
            }
        } catch (...) {
        }
        finish();
    }
};

LiveLink::LiveLink(HANDLE read, HANDLE write) : impl_(std::make_unique<Impl>(read, write)) {
    impl_->reader = std::thread([impl = impl_.get()] { impl->receive_loop(); });
    impl_->writer = std::thread([impl = impl_.get()] { impl->send_loop(); });
}

LiveLink::~LiveLink() = default;

void LiveLink::send(std::string text) {
    if (text.empty() || text.size() > maximum_message)
        throw std::length_error("A Play link message is empty or exceeds 4 MiB.");
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->closed)
            return;
        impl_->outbox.push_back(std::move(text));
    }
    impl_->wake.notify_all();
}

std::vector<std::string> LiveLink::receive() {
    std::lock_guard lock(impl_->mutex);
    return std::exchange(impl_->inbox, {});
}

bool LiveLink::closed() const noexcept {
    return impl_->closed;
}

PlayLink::Pending::~Pending() {
    std::error_code ignored;
    for (const auto& file : files)
        std::filesystem::remove(file, ignored);
}

void PlayLink::open(std::unique_ptr<LiveLink> link, std::filesystem::path spill) {
    static_cast<void>(close());
    link_ = std::move(link);
    spill_ = std::move(spill);
}

void PlayLink::forward(const std::vector<editor::LiveChange>& ops) {
    if (!link_ || !divergence_.empty() || ops.empty())
        return;
    constexpr std::size_t envelope = std::string_view(R"({"ops":[]})").size();
    std::string batch;
    Pending resources;
    try {
        for (const auto& op : ops) {
            std::string encoded;
            std::string mesh_bytes;
            std::string_view text = op.text;
            std::string_view content;
            std::filesystem::path file;
            std::size_t resource_bytes{};
            if (op.texture && op.mesh)
                throw std::runtime_error("A Play change cannot contain both texture and mesh content.");
            const auto available = maximum_pending_resource_bytes - pending_resource_bytes_ - resources.resource_bytes;
            if (op.texture) {
                resource_bytes = op.texture->pixels().size();
                if (resource_bytes > maximum_pending_resource_bytes) {
                    fail("A Play texture exceeds the 256 MiB in-flight resource limit.");
                    return;
                }
                if (resource_bytes > available) {
                    fail("Play stopped acknowledging resource publications (256 MiB resource budget exhausted).");
                    return;
                }
                file = spill_ / (L"texture-" + std::to_wstring(next_texture_++) + L".rgba");
                auto path = platform::utf16_to_utf8(file.native());
                if (!path) {
                    fail("Cannot forward Play changes: Invalid UTF-16 text.");
                    return;
                }
                const auto written = [&]() -> json::Result<Array<char>> {
                    ATELIER_TRY_BIND(texture, assets::json_format::parse(text));
                    const auto root = texture->root();
                    ATELIER_TRY(root.allowed_members({"op", "key"}));
                    ATELIER_TRY_BIND(kind, root.string({"op"}));
                    ATELIER_TRY_BIND(key, root.string({"key"}));
                    if (*kind != "texture" || key->empty())
                        return std::unexpected(root.error(
                            json::Fault::range, "Image content requires a texture change with an asset key."));
                    json::Builder result(*texture);
                    ATELIER_TRY(result.set(result.root(), "width", op.texture->width()));
                    ATELIER_TRY(result.set(result.root(), "height", op.texture->height()));
                    result.set_validated(result.root(), "file", *path);
                    return json::write(result.root());
                }();
                if (!written) {
                    fail("Cannot forward Play changes: " + written.error().describe());
                    return;
                }
                encoded.assign(written->data(), written->size());
                text = encoded;
                const auto pixels = op.texture->pixels();
                content = {reinterpret_cast<const char*>(pixels.data()), pixels.size()};
            } else if (op.mesh) {
                const auto parsed = [&]() -> json::Result<json::Document> {
                    ATELIER_TRY_BIND(mesh, assets::json_format::parse(text));
                    const auto root = mesh->root();
                    ATELIER_TRY(root.allowed_members({"op", "key"}));
                    ATELIER_TRY_BIND(kind, root.string({"op"}));
                    ATELIER_TRY_BIND(key, root.string({"key"}));
                    if (*kind != "mesh" || *key != op.mesh->key)
                        return std::unexpected(root.error(
                            json::Fault::range, "Mesh content requires a mesh change with the same asset key."));
                    return std::move(*mesh);
                }();
                if (!parsed) {
                    fail("Cannot forward Play changes: " + parsed.error().describe());
                    return;
                }
                // Encoding checks its full size against the shared remaining budget
                // before allocating the payload. Never revisit project source files.
                auto snapshot = assets::mesh_format::encode(*op.mesh, available);
                if (!snapshot) {
                    fail("Cannot forward Play changes: " + snapshot.error().message);
                    return;
                }
                mesh_bytes = std::move(*snapshot);
                content = mesh_bytes;
                resource_bytes = content.size();
                file = spill_ / (L"mesh-" + std::to_wstring(next_mesh_++) + L".mesh");
                auto path = platform::utf16_to_utf8(file.native());
                if (!path) {
                    fail("Cannot forward Play changes: Invalid UTF-16 text.");
                    return;
                }
                json::Builder mesh(*parsed);
                mesh.set_validated(mesh.root(), "file", *path);
                const auto bytes = json::write(mesh.root());
                encoded.assign(bytes.data(), bytes.size());
                text = encoded;
            }
            if (text.size() + envelope > maximum_batch) {
                fail("A change of " + std::to_string(text.size() >> 20) + " MiB exceeds the 16 MiB Play batch limit.");
                return;
            }
            if (!batch.empty() && batch.size() + 1 + text.size() + envelope > maximum_batch) {
                send(batch, std::move(resources));
                batch.clear();
                if (!divergence_.empty())
                    return;
            }
            if (op.texture || op.mesh) {
                // Register before writing so allocation failure cannot orphan an owned file.
                resources.files.push_back(file);
                if (auto saved = editor::write_file(file, content, editor::WriteMode::create); !saved) {
                    resources.files.pop_back();
                    fail(std::string("Cannot write a Play ") + (op.mesh ? "mesh" : "texture") +
                         " publication: " + saved.error().message);
                    return;
                }
                resources.resource_bytes += resource_bytes;
            }
            if (!batch.empty())
                batch += ',';
            batch += text;
        }
        send(batch, std::move(resources));
    } catch (const std::exception& error) {
        fail(std::string("Cannot forward Play changes: ") + error.what());
    }
}

void PlayLink::send(std::string_view ops, Pending&& resources) {
    const auto seq = next_seq_++;
    auto message = R"({"type":"batch","seq":)" + std::to_string(seq) + R"(,"ops":[)";
    message.append(ops);
    message += "]}";
    auto bytes = message.size();
    if (message.size() > LiveLink::maximum_message) {
        const auto file = spill_ / (L"batch-" + std::to_wstring(seq) + L".json");
        std::string text = R"({"ops":[)";
        text.append(ops);
        text += "]}";
        bytes = text.size();
        resources.files.push_back(file);
        if (auto saved = editor::write_file(file, text, editor::WriteMode::create); !saved) {
            resources.files.pop_back();
            fail("Cannot write a large Play change batch: " + saved.error().message);
            return;
        }
        auto path = platform::utf16_to_utf8(file.native());
        if (!path) {
            fail("Cannot forward Play changes: Invalid UTF-16 text.");
            return;
        }
        json::Builder notice;
        notice.set_validated(notice.root(), "type", "batch");
        (void)notice.set(notice.root(), "seq", seq);
        notice.set_validated(notice.root(), "file", *path);
        const auto encoded = json::write(notice.root());
        message.assign(encoded.data(), encoded.size());
    }
    // Transfer ownership before queuing: once sent, even a disconnect cannot retire
    // files the peer may still be reading. Only ACK or closing this run releases them.
    resources.seq = seq;
    resources.bytes = bytes;
    pending_.push_back(std::move(resources));
    pending_bytes_ += bytes;
    pending_resource_bytes_ += pending_.back().resource_bytes;
    resources.resource_bytes = 0;
    link_->send(std::move(message));
    if (pending_.size() > maximum_pending || pending_bytes_ > maximum_pending_bytes)
        fail("Play stopped acknowledging changes (" + std::to_string(pending_.size()) + " batches pending).");
}

void PlayLink::fail(std::string reason) {
    if (divergence_.empty())
        divergence_ = std::move(reason);
}

void PlayLink::poll(bool running, bool stopping) {
    if (!link_)
        return;
    for (const auto& text : link_->receive()) {
        try {
            const auto read = [&]() -> json::Result<void> {
                ATELIER_TRY_BIND(message, assets::json_format::parse(text));
                const auto root = message->root();
                ATELIER_TRY_BIND(type, root.string({"type"}));
                if (*type == "inspect") {
                    ATELIER_TRY_BIND(id, root.u64({"id"}));
                    ATELIER_TRY_BIND(graph, root.find("graph"));
                    if (*graph) {
                        ATELIER_TRY_BIND(value, (**graph).string());
                        inspected_[*id] = *value;
                    } else {
                        ATELIER_TRY_BIND(error, root.string({"error"}));
                        inspected_[*id] = "error: " + std::string(*error);
                    }
                } else if (*type == "error") {
                    ATELIER_TRY_BIND(error, root.string({"message"}));
                    fail(std::string(*error));
                } else if (*type == "ack") {
                    ATELIER_TRY_BIND(seq, root.u64({"seq"}));
                    if (pending_.empty() || pending_.front().seq != *seq)
                        throw std::runtime_error("batch " + std::to_string(*seq) + " acknowledged out of order");
                    pending_bytes_ -= pending_.front().bytes;
                    pending_resource_bytes_ -= pending_.front().resource_bytes;
                    pending_.pop_front();
                    ATELIER_TRY_BIND(diagnostics, root.at("diagnostics"));
                    ATELIER_TRY_BIND(items, diagnostics->array());
                    for (auto item : *items) {
                        ATELIER_TRY_BIND(severity, item.string({"severity"}));
                        if (*severity == "error") {
                            ATELIER_TRY_BIND(error, item.string({"message"}));
                            fail(std::string(*error));
                            break;
                        }
                    }
                } else
                    throw std::runtime_error("unknown message type");
                return {};
            }();
            if (!read)
                fail("Unreadable Play reply: " + read.error().describe());
        } catch (const std::exception& error) {
            fail(std::string("Unreadable Play reply: ") + error.what());
        }
    }
    // A child closes its handles before its process ends; only a link that stays
    // closed while Play keeps running means Play cannot follow anymore.
    if (!link_->closed() || !running || stopping) {
        closed_since_.reset();
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!closed_since_)
        closed_since_ = now;
    else if (now - *closed_since_ >= std::chrono::seconds(2))
        fail("The Play change link closed while Play kept running.");
}

std::error_code PlayLink::close() noexcept {
    link_.reset();
    pending_.clear();
    pending_bytes_ = pending_resource_bytes_ = 0;
    divergence_.clear();
    closed_since_.reset();
    inspected_.clear();
    next_seq_ = next_request_ = next_texture_ = next_mesh_ = 1;
    std::error_code error;
    if (!spill_.empty())
        std::filesystem::remove_all(spill_, error);
    spill_.clear();
    return error;
}

std::uint64_t PlayLink::inspect(std::uint64_t identity) {
    if (!link_)
        return 0;
    const auto request = next_request_++;
    json::Builder message;
    message.set_validated(message.root(), "type", "inspect");
    (void)message.set(message.root(), "id", request);
    (void)message.set(message.root(), "object", identity);
    const auto bytes = json::write(message.root());
    link_->send(std::string(bytes.data(), bytes.size()));
    return request;
}

std::optional<std::string> PlayLink::inspected(std::uint64_t request) {
    const auto at = inspected_.find(request);
    if (at == inspected_.end())
        return {};
    auto result = std::move(at->second);
    inspected_.erase(at);
    return result;
}

void PlayEnd::service(editor::Session& document, editor::Project& project, const Report& report) {
    for (auto& text : link_->receive()) {
        json::Builder reply;
        const auto reject = [&](const std::string& message, bool print) {
            diverged_ = true;
            world::Diagnostic item;
            item.severity = world::Severity::error;
            item.kind = world::DiagnosticKind::command_rejected;
            item.phase = world::DiagnosticPhase::command;
            item.message = message;
            if (print)
                report({&item, 1});
        };
        const auto error_reply = [&](std::string message) {
            reject(message, true);
            reply = json::Builder{};
            reply.set_validated(reply.root(), "type", "error");
            reply.set_validated(reply.root(), "message", diagnostic_utf8(message));
        };
        try {
            const auto handled = [&]() -> json::Result<void> {
                ATELIER_TRY_BIND(message, assets::json_format::parse(text));
                const auto root = message->root();
                ATELIER_TRY_BIND(type, root.string({"type"}));
                if (*type == "inspect") {
                    reply.set_validated(reply.root(), "type", "inspect");
                    ATELIER_TRY_BIND(id, root.at("id"));
                    reply.set_validated(reply.root(), "id", *id);
                    ATELIER_TRY_BIND(identity, root.u64({"object"}));
                    auto graph = document.local_object(*identity).and_then(
                        [&](ObjectId object) { return document.serialize_live_subtree(object); });
                    if (!graph)
                        reply.set_validated(reply.root(), "error", diagnostic_utf8(graph.error().message));
                    else if (graph->size() > LiveLink::maximum_message / 2)
                        reply.set_validated(reply.root(), "error", "The inspected graph exceeds the reply limit.");
                    else
                        reply.set_validated(reply.root(), "graph", *graph);
                } else if (*type == "batch") {
                    ATELIER_TRY_BIND(sequence, root.u64({"seq"}));
                    const auto seq = *sequence;
                    reply.set_validated(reply.root(), "type", "ack");
                    ATELIER_TRY(reply.set(reply.root(), "seq", seq));
                    ATELIER_TRY_BIND(out, reply.array(reply.root(), "diagnostics"));
                    const auto refused = [&](std::string message, bool print) {
                        reject(message, print);
                        const auto item = reply.append_object(*out);
                        reply.set_validated(item, "severity", "error");
                        reply.set_validated(item, "kind", "command_rejected");
                        reply.set_validated(item, "message", diagnostic_utf8(message));
                    };
                    if (diverged_) {
                        refused("Play diverged before change batch " + std::to_string(seq) + "; nothing was applied.",
                                false);
                    } else if (seq != expected_) {
                        refused("Play change sequence gap: expected batch " + std::to_string(expected_) +
                                    ", received " + std::to_string(seq) + ".",
                                true);
                    } else {
                        ++expected_;
                        std::string batch;
                        ATELIER_TRY_BIND(file, root.find("file"));
                        if (*file) {
                            ATELIER_TRY_BIND(path, (**file).string());
                            auto read = assets::read_file(platform::utf8_to_utf16_validated(*path));
                            if (read)
                                batch = std::move(*read);
                            else {
                                refused("Cannot read Play change batch " + std::to_string(seq) + ": " +
                                            read.error().message,
                                        true);
                            }
                        } else {
                            ATELIER_TRY_BIND(ops, root.at("ops"));
                            json::Builder envelope;
                            envelope.set_validated(envelope.root(), "ops", *ops);
                            const auto bytes = json::write(envelope.root());
                            batch.assign(bytes.data(), bytes.size());
                        }
                        if (!batch.empty()) {
                            auto applied = document.apply_live(batch, project);
                            if (!applied) {
                                refused("Play change batch " + std::to_string(seq) +
                                            " failed: " + applied.error().message,
                                        true);
                            } else {
                                report(*applied);
                                for (const auto& item : *applied) {
                                    if (item.severity == world::Severity::error)
                                        diverged_ = true;
                                    auto description = editor::describe_diagnostic(document, item);
                                    if (!description) {
                                        error_reply("Unreadable editor message: " + description.error().message);
                                        break;
                                    }
                                    const auto entry = reply.append_object(*out);
                                    reply.set_validated(entry, "severity", world::name(item.severity));
                                    reply.set_validated(entry, "kind", world::name(item.kind));
                                    reply.set_validated(entry, "message", diagnostic_utf8(*description));
                                }
                            }
                        }
                    }
                } else
                    throw std::runtime_error("Unknown editor message type.");
                return {};
            }();
            if (!handled)
                error_reply("Unreadable editor message: " + handled.error().describe());
        } catch (const std::exception& error) {
            // A message without a readable number cannot be acknowledged; the editor treats this as divergence.
            error_reply(std::string("Unreadable editor message: ") + error.what());
        }
        try {
            auto bytes = json::write(reply.root());
            // Every diagnostic is already in the output; an oversized acknowledgement keeps only its verdict.
            const auto diagnostics = *reply.root().find("diagnostics");
            if (bytes.size() > LiveLink::maximum_message && diagnostics) {
                json::Builder summary(json::Kind::array);
                const auto items = *diagnostics->array();
                for (auto item : items)
                    if (*item.string({"severity"}) == "error") {
                        const auto entry = summary.append_object(summary.root());
                        summary.set_validated(entry, "severity", "error");
                        summary.set_validated(entry, "kind", *item.string({"kind"}));
                        const auto message = *item.string({"message"});
                        summary.set_validated(entry, "message", StringView{message}.utf8_prefix(4096).std_view());
                        break;
                    }
                reply.set_validated(reply.root(), "diagnostics", summary.root());
                bytes = json::write(reply.root());
            }
            link_->send(std::string(bytes.data(), bytes.size()));
        } catch (const std::exception& error) {
            reject(std::string("Cannot reply to the editor: ") + error.what(), true);
        }
    }
}
}
