#include "application/play_link.hpp"
#include "assets/file.hpp"
#include "assets/json_format.hpp"
#include "assets/mesh_format.hpp"
#include "editor/project_code.hpp"
#include "editor/session.hpp"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace atelier::application {
namespace {
using Json = assets::json_format::Json;

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

std::string dump(const Json& value) {
    return value.dump(-1, ' ', false, Json::error_handler_t::replace);
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
                auto texture = assets::json_format::parse(text);
                assets::json_format::fields(texture, {"op", "key"});
                if (texture.at("op") != "texture" || texture.at("key").get_ref<const std::string&>().empty())
                    throw std::runtime_error("Image content requires a texture change with an asset key.");
                file = spill_ / (L"texture-" + std::to_wstring(next_texture_++) + L".rgba");
                texture["width"] = op.texture->width();
                texture["height"] = op.texture->height();
                texture["file"] = assets::utf8(file.wstring());
                encoded = dump(texture);
                text = encoded;
                const auto pixels = op.texture->pixels();
                content = {reinterpret_cast<const char*>(pixels.data()), pixels.size()};
            } else if (op.mesh) {
                auto mesh = assets::json_format::parse(text);
                assets::json_format::fields(mesh, {"op", "key"});
                if (mesh.at("op") != "mesh" || mesh.at("key").get_ref<const std::string&>() != op.mesh->key)
                    throw std::runtime_error("Mesh content requires a mesh change with the same asset key.");
                // Encoding checks its full size against the shared remaining budget
                // before allocating the payload. Never revisit project source files.
                mesh_bytes = assets::mesh_format::encode(*op.mesh, available);
                content = mesh_bytes;
                resource_bytes = content.size();
                file = spill_ / (L"mesh-" + std::to_wstring(next_mesh_++) + L".mesh");
                mesh["file"] = assets::utf8(file.wstring());
                encoded = dump(mesh);
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
        message = dump(Json{{"type", "batch"}, {"seq", seq}, {"file", assets::utf8(file.wstring())}});
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
            const auto message = assets::json_format::parse(text);
            const auto& type = message.at("type").get_ref<const std::string&>();
            if (type == "inspect") {
                inspected_[message.at("id").get<std::uint64_t>()] =
                    message.contains("graph") ? message.at("graph").get<std::string>()
                                              : "error: " + message.at("error").get<std::string>();
            } else if (type == "error")
                fail(message.at("message").get<std::string>());
            else if (type == "ack") {
                const auto seq = message.at("seq").get<std::uint64_t>();
                if (pending_.empty() || pending_.front().seq != seq)
                    throw std::runtime_error("batch " + std::to_string(seq) + " acknowledged out of order");
                pending_bytes_ -= pending_.front().bytes;
                pending_resource_bytes_ -= pending_.front().resource_bytes;
                pending_.pop_front();
                for (const auto& item : message.at("diagnostics"))
                    if (item.at("severity") == "error") {
                        fail(item.at("message").get<std::string>());
                        break;
                    }
            } else
                throw std::runtime_error("unknown message type");
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
    link_->send(dump(Json{{"type", "inspect"}, {"id", request}, {"object", identity}}));
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
        Json reply;
        const auto reject = [&](std::string message, bool print) {
            diverged_ = true;
            world::Diagnostic item;
            item.severity = world::Severity::error;
            item.kind = world::DiagnosticKind::command_rejected;
            item.phase = world::DiagnosticPhase::command;
            item.message = std::move(message);
            if (print)
                report({&item, 1});
            return Json{{"severity", "error"}, {"kind", "command_rejected"}, {"message", item.message}};
        };
        try {
            auto message = assets::json_format::parse(text);
            const auto type = message.at("type").get<std::string>();
            if (type == "inspect") {
                reply = {{"type", "inspect"}, {"id", message.at("id")}};
                auto graph =
                    document.local_object(message.at("object").get<std::uint64_t>()).and_then([&](ObjectId object) {
                        return document.serialize_live_subtree(object);
                    });
                if (!graph)
                    reply["error"] = graph.error().message;
                else if (graph->size() > LiveLink::maximum_message / 2)
                    reply["error"] = "The inspected graph exceeds the reply limit.";
                else
                    reply["graph"] = std::move(*graph);
            } else if (type == "batch") {
                const auto seq = message.at("seq").get<std::uint64_t>();
                reply = {{"type", "ack"}, {"seq", seq}, {"diagnostics", Json::array()}};
                auto& out = reply["diagnostics"];
                if (diverged_)
                    out.push_back(reject(
                        "Play diverged before change batch " + std::to_string(seq) + "; nothing was applied.", false));
                else if (seq != expected_)
                    out.push_back(reject("Play change sequence gap: expected batch " + std::to_string(expected_) +
                                             ", received " + std::to_string(seq) + ".",
                                         true));
                else {
                    ++expected_;
                    std::string batch;
                    if (message.contains("file")) {
                        const std::filesystem::path file = assets::wide(message.at("file").get<std::string>());
                        auto read = assets::read_file(file);
                        if (read)
                            batch = std::move(*read);
                        else
                            out.push_back(reject("Cannot read Play change batch " + std::to_string(seq) + ": " +
                                                     read.error().message,
                                                 true));
                    } else
                        batch = dump(Json{{"ops", std::move(message.at("ops"))}});
                    if (!batch.empty()) {
                        auto applied = document.apply_live(batch, project);
                        if (!applied)
                            out.push_back(reject("Play change batch " + std::to_string(seq) +
                                                     " failed: " + applied.error().message,
                                                 true));
                        else {
                            report(*applied);
                            for (const auto& item : *applied) {
                                if (item.severity == world::Severity::error)
                                    diverged_ = true;
                                out.push_back({{"severity", world::name(item.severity)},
                                               {"kind", world::name(item.kind)},
                                               {"message", assets::utf8(editor::describe_diagnostic(document, item))}});
                            }
                        }
                    }
                }
            } else
                throw std::runtime_error("Unknown editor message type.");
        } catch (const std::exception& error) {
            // A message without a readable number cannot be acknowledged; the editor treats this as divergence.
            const auto item = reject(std::string("Unreadable editor message: ") + error.what(), true);
            reply = {{"type", "error"}, {"message", item.at("message")}};
        }
        try {
            auto text = dump(reply);
            // Every diagnostic is already in the output; an oversized acknowledgement keeps only its verdict.
            if (text.size() > LiveLink::maximum_message && reply.contains("diagnostics")) {
                Json summary = Json::array();
                for (const auto& item : reply["diagnostics"])
                    if (item.at("severity") == "error") {
                        summary.push_back({{"severity", "error"},
                                           {"kind", item.at("kind")},
                                           {"message", item.at("message").get<std::string>().substr(0, 4096)}});
                        break;
                    }
                reply["diagnostics"] = std::move(summary);
                text = dump(reply);
            }
            link_->send(std::move(text));
        } catch (const std::exception& error) {
            static_cast<void>(reject(std::string("Cannot reply to the editor: ") + error.what(), true));
        }
    }
}
}
