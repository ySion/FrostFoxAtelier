#include "application/play_link.hpp"
#include "application/diagnostics.hpp"
#include "assets/file.hpp"
#include "assets/json_format.hpp"
#include "assets/mesh_format.hpp"
#include "assets/png.hpp"
#include "editor/project_code.hpp"
#include "editor/session.hpp"
#include "memory/fatal.hpp"
#include "platform/utf.hpp"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace atelier::application {
namespace {
template <class T>
Result<T> message_value(json::Result<T> value) {
    if (!value)
        return std::unexpected(Error{ErrorCode::invalid_storage, value.error().describe()});
    if constexpr (std::is_void_v<T>)
        return {};
    else
        return std::move(*value);
}

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

    RingDeque<Array<char>> outbox;

    Array<Array<char>> inbox;

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
        while (!stopping) {
            std::uint32_t bytes{};
            if (!transfer(read, &bytes, sizeof(bytes), false) || !bytes || bytes > maximum_message)
                break;
            Array<char> frame;
            frame.resize_uninitialized(bytes);
            if (!transfer(read, frame.data(), bytes, false))
                break;
            std::lock_guard lock(mutex);
            inbox.add(std::move(frame));
        }
        finish();
    }

    void send_loop() {
        for (;;) {
            Array<char> frame;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [&] { return stopping || closed || !outbox.empty(); });
                if (stopping || closed)
                    break;
                frame = std::move(outbox.front());
                outbox.pop_front();
            }
            const auto bytes = static_cast<std::uint32_t>(frame.size());
            if (!transfer(write, const_cast<std::uint32_t*>(&bytes), sizeof(bytes), true) ||
                !transfer(write, frame.data(), frame.size(), true))
                break;
        }
        finish();
    }
};

LiveLink::LiveLink(HANDLE read, HANDLE write) noexcept : impl_(std::make_unique<Impl>(read, write)) {
    impl_->reader = std::thread([impl = impl_.get()] { impl->receive_loop(); });
    impl_->writer = std::thread([impl = impl_.get()] { impl->send_loop(); });
}

LiveLink::~LiveLink() = default;

void LiveLink::send(Array<char> frame) {
    if (frame.empty() || frame.size() > maximum_message)
        fatal("A Play link message is empty or exceeds 4 MiB.");
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->closed)
            return;
        impl_->outbox.emplace_back(std::move(frame));
    }
    impl_->wake.notify_all();
}

void LiveLink::send(StringView text) {
    Array<char> frame;
    frame.assign(Span<const char>{text.data(), text.size()});
    send(std::move(frame));
}

Array<Array<char>> LiveLink::receive() {
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

void PlayLink::forward(Span<const editor::LiveChange> ops) {
    if (!link_ || !divergence_.empty() || ops.empty())
        return;
    constexpr std::size_t envelope = StringView(R"({"ops":[]})").size();
    Array<char> batch;
    Pending resources;
    for (const auto& op : ops) {
        Array<char> encoded;
        Array<std::byte> mesh_bytes;
        std::string png_bytes;
        StringView text = op.text.view();
        Span<const std::byte> content;
        std::filesystem::path file;
        std::size_t resource_bytes{};
        if (op.texture && op.mesh) {
            fail("Cannot forward Play changes: A Play change cannot contain both texture and mesh content.");
            return;
        }
        const auto available = maximum_pending_resource_bytes - pending_resource_bytes_ - resources.resource_bytes;
        if (op.texture) {
            const auto pixels = op.texture->acquire_pixels();
            if (!pixels || pixels->empty()) {
                fail("Cannot forward Play changes: Texture pixel content is unavailable.");
                return;
            }
            auto png = assets::encode_png_bytes(op.texture->width(), op.texture->height(), *pixels);
            if (!png) {
                fail(String::concat("Cannot forward Play changes: ", png.error().message));
                return;
            }
            png_bytes = std::move(*png);
            resource_bytes = png_bytes.size();
            if (resource_bytes > maximum_pending_resource_bytes) {
                fail("A Play texture exceeds the 256 MiB in-flight resource limit.");
                return;
            }
            if (resource_bytes > available) {
                fail("Play stopped acknowledging resource publications (256 MiB resource budget exhausted).");
                return;
            }
            file = spill_ / (L"texture-" + std::to_wstring(next_texture_++) + L".png");
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
                    return std::unexpected(
                        root.error(json::Fault::range, "Image content requires a texture change with an asset key."));
                json::Builder result(*texture);
                ATELIER_TRY(result.set(result.root(), "width", op.texture->width()));
                ATELIER_TRY(result.set(result.root(), "height", op.texture->height()));
                result.set_validated(result.root(), "file", *path);
                return json::write(result.root());
            }();
            if (!written) {
                fail(String::concat("Cannot forward Play changes: ", written.error().describe()));
                return;
            }
            encoded = std::move(*written);
            text = StringView{encoded.data(), encoded.size()};
            content = as_bytes(StringView{png_bytes});
        } else if (op.mesh) {
            const auto parsed = [&]() -> json::Result<json::Document> {
                ATELIER_TRY_BIND(mesh, assets::json_format::parse(text));
                const auto root = mesh->root();
                ATELIER_TRY(root.allowed_members({"op", "key"}));
                ATELIER_TRY_BIND(kind, root.string({"op"}));
                ATELIER_TRY_BIND(key, root.string({"key"}));
                if (*kind != "mesh" || *key != op.mesh->key)
                    return std::unexpected(
                        root.error(json::Fault::range, "Mesh content requires a mesh change with the same asset key."));
                return std::move(*mesh);
            }();
            if (!parsed) {
                fail(String::concat("Cannot forward Play changes: ", parsed.error().describe()));
                return;
            }
            // Encoding checks its full size against the shared remaining budget
            // before allocating the payload. Never revisit project source files.
            auto snapshot = assets::mesh_format::encode(*op.mesh, available);
            if (!snapshot) {
                fail(String::concat("Cannot forward Play changes: ", snapshot.error().message));
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
            encoded = json::write(mesh.root());
            text = StringView{encoded.data(), encoded.size()};
        }
        if (text.size() + envelope > maximum_batch) {
            fail(String::concat("A change of ", text.size() >> 20, " MiB exceeds the 16 MiB Play batch limit."));
            return;
        }
        if (!batch.empty() && batch.size() + 1 + text.size() + envelope > maximum_batch) {
            send(StringView{batch.data(), batch.size()}, std::move(resources));
            batch.clear();
            if (!divergence_.empty())
                return;
        }
        if (op.texture || op.mesh) {
            // Register before writing so allocation failure cannot orphan an owned file.
            resources.files.add(file);
            if (auto saved = editor::write_file(file, content, editor::WriteMode::create); !saved) {
                resources.files.pop();
                fail(String::concat("Cannot write a Play ", String::validated(op.mesh ? "mesh" : "texture"),
                                    " publication: ", saved.error().message));
                return;
            }
            resources.resource_bytes += resource_bytes;
        }
        if (!batch.empty())
            batch.add(',');
        batch.append(Span<const char>{text.data(), text.size()});
    }
    send(StringView{batch.data(), batch.size()}, std::move(resources));
}

void PlayLink::send(StringView ops, Pending&& resources) {
    const auto seq = next_seq_++;
    const auto seq_text = String::from_number(seq);
    constexpr StringView prefix = R"({"type":"batch","seq":)";
    constexpr StringView mid = R"(,"ops":[)";
    constexpr StringView suffix = "]}";
    Array<char> message;
    message.reserve(prefix.size() + seq_text.size() + mid.size() + ops.size() + suffix.size());
    message.append(Span<const char>{prefix.data(), prefix.size()});
    message.append(Span<const char>{seq_text.c_str(), seq_text.size()});
    message.append(Span<const char>{mid.data(), mid.size()});
    message.append(Span<const char>{ops.data(), ops.size()});
    message.append(Span<const char>{suffix.data(), suffix.size()});
    auto bytes = message.size();
    if (message.size() > LiveLink::maximum_message) {
        const auto file = spill_ / (L"batch-" + std::to_wstring(seq) + L".json");
        constexpr StringView spill_prefix = R"({"ops":[)";
        Array<char> text;
        text.reserve(spill_prefix.size() + ops.size() + suffix.size());
        text.append(Span<const char>{spill_prefix.data(), spill_prefix.size()});
        text.append(Span<const char>{ops.data(), ops.size()});
        text.append(Span<const char>{suffix.data(), suffix.size()});
        bytes = text.size();
        resources.files.add(file);
        if (auto saved = editor::write_file(file, StringView{text.data(), text.size()}, editor::WriteMode::create);
            !saved) {
            resources.files.pop();
            fail(String::concat("Cannot write a large Play change batch: ", saved.error().message));
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
        message = json::write(notice.root());
    }
    // Transfer ownership before queuing: once sent, even a disconnect cannot retire
    // files the peer may still be reading. Only ACK or closing this run releases them.
    resources.seq = seq;
    resources.bytes = bytes;
    pending_.emplace_back(std::move(resources));
    pending_bytes_ += bytes;
    pending_resource_bytes_ += pending_.back().resource_bytes;
    resources.resource_bytes = 0;
    link_->send(std::move(message));
    if (pending_.size() > maximum_pending || pending_bytes_ > maximum_pending_bytes)
        fail(String::concat("Play stopped acknowledging changes (", pending_.size(), " batches pending)."));
}

void PlayLink::fail(Utf8Text reason) {
    if (divergence_.empty())
        divergence_ = std::move(reason).into_string();
}

void PlayLink::poll(bool running, bool stopping) {
    if (!link_)
        return;
    for (const auto& raw : link_->receive()) {
        const StringView text{raw.data(), raw.size()};
        const auto read = [&]() -> Result<void> {
            ATELIER_TRY_BIND(message, message_value(assets::json_format::parse(text)));
            const auto root = message->root();
            ATELIER_TRY_BIND(type, message_value(root.string({"type"})));
            if (*type == "inspect") {
                ATELIER_TRY_BIND(id, message_value(root.u64({"id"})));
                ATELIER_TRY_BIND(graph, message_value(root.find("graph")));
                if (*graph) {
                    ATELIER_TRY_BIND(value, message_value((**graph).string()));
                    inspected_.insert_or_assign(*id, String::from_utf8_validated(*value));
                } else {
                    ATELIER_TRY_BIND(error, message_value(root.string({"error"})));
                    inspected_.insert_or_assign(*id, String::concat("error: ", String::validated(*error)));
                }
            } else if (*type == "error") {
                ATELIER_TRY_BIND(error, message_value(root.string({"message"})));
                fail(String::from_utf8_validated(*error));
            } else if (*type == "ack") {
                ATELIER_TRY_BIND(seq, message_value(root.u64({"seq"})));
                if (pending_.empty() || pending_.front().seq != *seq)
                    return std::unexpected(Error{ErrorCode::invalid_storage,
                                                 String::concat("batch ", *seq, " acknowledged out of order")});
                pending_bytes_ -= pending_.front().bytes;
                pending_resource_bytes_ -= pending_.front().resource_bytes;
                pending_.pop_front();
                ATELIER_TRY_BIND(diagnostics, message_value(root.at("diagnostics")));
                ATELIER_TRY_BIND(items, message_value(diagnostics->array()));
                for (auto item : *items) {
                    ATELIER_TRY_BIND(severity, message_value(item.string({"severity"})));
                    if (*severity == "error") {
                        ATELIER_TRY_BIND(error, message_value(item.string({"message"})));
                        fail(String::from_utf8_validated(*error));
                        break;
                    }
                }
            } else
                return std::unexpected(Error{ErrorCode::invalid_storage, "unknown message type"});
            return {};
        }();
        if (!read)
            fail(String::concat("Unreadable Play reply: ", read.error().message));
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
    link_->send(json::write(message.root()));
    return request;
}

Optional<String> PlayLink::inspected(std::uint64_t request) {
    return inspected_.take(request);
}

void PlayEnd::service(editor::Session& document, editor::Project& project) {
    for (const auto& raw : link_->receive()) {
        const StringView text{raw.data(), raw.size()};
        json::Builder reply;
        const auto reject = [&](Utf8Text message_in, bool print) {
            diverged_ = true;
            world::Diagnostic item;
            item.severity = world::Severity::error;
            item.kind = world::DiagnosticKind::command_rejected;
            item.phase = world::DiagnosticPhase::command;
            item.message = std::move(message_in).into_string();
            if (print)
                report_world(document, {&item, 1}, project.root(), "live");
        };
        const auto error_reply = [&](Utf8Text message_in) {
            auto message = std::move(message_in).into_string();
            reply = json::Builder{};
            reply.set_validated(reply.root(), "type", "error");
            reply.set_validated(reply.root(), "message", message);
            reject(std::move(message), true);
        };
        const auto handled = [&]() -> Result<void> {
            ATELIER_TRY_BIND(message, message_value(assets::json_format::parse(text)));
            const auto root = message->root();
            ATELIER_TRY_BIND(type, message_value(root.string({"type"})));
            if (*type == "inspect") {
                reply.set_validated(reply.root(), "type", "inspect");
                ATELIER_TRY_BIND(id, message_value(root.at("id")));
                reply.set_validated(reply.root(), "id", *id);
                ATELIER_TRY_BIND(identity, message_value(root.u64({"object"})));
                auto graph = document.local_object(*identity).and_then(
                    [&](ObjectId object) { return document.serialize_live_subtree(object); });
                if (!graph)
                    reply.set_validated(reply.root(), "error", graph.error().message);
                else if (graph->size() > LiveLink::maximum_message / 2)
                    reply.set_validated(reply.root(), "error", "The inspected graph exceeds the reply limit.");
                else
                    reply.set_validated(reply.root(), "graph", *graph);
            } else if (*type == "batch") {
                ATELIER_TRY_BIND(sequence, message_value(root.u64({"seq"})));
                const auto seq = *sequence;
                reply.set_validated(reply.root(), "type", "ack");
                ATELIER_TRY(message_value(reply.set(reply.root(), "seq", seq)));
                ATELIER_TRY_BIND(out, message_value(reply.array(reply.root(), "diagnostics")));
                const auto refused = [&](Utf8Text message_in, bool print) {
                    auto message = std::move(message_in).into_string();
                    const auto item = reply.append_object(*out);
                    reply.set_validated(item, "severity", "error");
                    reply.set_validated(item, "kind", "command_rejected");
                    reply.set_validated(item, "message", message);
                    reject(std::move(message), print);
                };
                if (diverged_) {
                    refused(String::concat("Play diverged before change batch ", seq, "; nothing was applied."), false);
                } else if (seq != expected_) {
                    refused(String::concat("Play change sequence gap: expected batch ", expected_, ", received ", seq,
                                           "."),
                            true);
                } else {
                    ++expected_;
                    Array<std::byte> file_bytes;
                    Array<char> json_bytes;
                    StringView batch;
                    ATELIER_TRY_BIND(file, message_value(root.find("file")));
                    if (*file) {
                        ATELIER_TRY_BIND(path, message_value((**file).string()));
                        auto read = assets::read_file(platform::utf8_to_utf16_validated(*path));
                        if (read) {
                            file_bytes = std::move(*read);
                            batch = as_chars(file_bytes);
                        } else {
                            refused(
                                String::concat("Cannot read Play change batch ", seq, ": ", read.error().message),
                                true);
                        }
                    } else {
                        ATELIER_TRY_BIND(ops, message_value(root.at("ops")));
                        json::Builder envelope;
                        envelope.set_validated(envelope.root(), "ops", *ops);
                        json_bytes = json::write(envelope.root());
                        batch = StringView{json_bytes.data(), json_bytes.size()};
                    }
                    if (!batch.empty()) {
                        auto applied = document.apply_live(batch, project);
                        if (!applied) {
                            refused(String::concat("Play change batch ", seq, " failed: ", applied.error().message),
                                    true);
                        } else {
                            report_world(document, *applied, project.root(), "live");
                            for (const auto& item : *applied) {
                                if (item.severity == world::Severity::error)
                                    diverged_ = true;
                                auto description = editor::describe_diagnostic(document, item);
                                if (!description) {
                                    error_reply(
                                        String::concat("Unreadable editor message: ", description.error().message));
                                    break;
                                }
                                const auto entry = reply.append_object(*out);
                                reply.set_validated(entry, "severity", world::name(item.severity));
                                reply.set_validated(entry, "kind", world::name(item.kind));
                                reply.set_validated(entry, "message", *description);
                            }
                        }
                    }
                }
            } else
                return std::unexpected(Error{ErrorCode::invalid_storage, "Unknown editor message type."});
            return {};
        }();
        if (!handled)
            error_reply(String::concat("Unreadable editor message: ", handled.error().message));
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
                    summary.set_validated(entry, "message", message.utf8_prefix(4096));
                    break;
                }
            reply.set_validated(reply.root(), "diagnostics", summary.root());
            bytes = json::write(reply.root());
        }
        if (bytes.size() > LiveLink::maximum_message)
            reject("Cannot reply to the editor: A Play link message is empty or exceeds 4 MiB.", true);
        else
            link_->send(std::move(bytes));
    }
}
}
