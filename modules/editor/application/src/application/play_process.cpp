#include "application/play_process.hpp"
#include "control/control.hpp"
#include "platform/error.hpp"
#include "platform/utf.hpp"
#include "logging/native.hpp"
#include <array>
#include <algorithm>
#include <cstdio>
#include <utility>
#include <vector>

namespace atelier::application {
namespace {
auto failure(std::string message, DWORD error = GetLastError()) {
    return std::unexpected(
        Error{ErrorCode::invalid_storage, std::move(message) + " (Windows " + std::to_string(error) + ")."});
}

void report_os_failure(std::string_view operation, DWORD error) noexcept {
    static_cast<void>(
        ATELIER_LOG(logging::Severity::error, "play", "play.os_failed", "{} failed: Windows {}", operation, error));
}

void report_forced(const std::filesystem::path& directory, std::chrono::steady_clock::duration waited, bool exited,
                   std::optional<DWORD> exit_code, bool acknowledged) noexcept {
    const auto path = platform::utf16_to_utf8(directory.generic_wstring());
    if (!path) {
        std::fputs("Play termination diagnostic capture failed.\n", stderr);
        return;
    }
    const std::array fields{
        logging::Field{"directory", *path},
        logging::Field{"waited_ms", std::chrono::duration_cast<std::chrono::milliseconds>(waited).count()},
        logging::Field{"exited", exited}, logging::Field{"exit_code", exit_code ? std::int64_t{*exit_code} : -1},
        logging::Field{"acknowledged", acknowledged}};
    static_cast<void>(ATELIER_LOG_WITH(logging::Severity::warning, "play", "play.forced",
                                       logging::native::Context{.fields = fields},
                                       "Play stop acknowledgement or exit timed out; terminating its process group"));
}

void report_exit_unobserved(std::chrono::steady_clock::duration waited, bool forced, bool acknowledged,
                            bool wait_failed) noexcept {
    static_cast<void>(ATELIER_LOG(logging::Severity::error, "play", "play.exit_unobserved",
                                  "Play exit unobserved: waited_ms={} forced={} acknowledged={} wait_failed={}",
                                  std::chrono::duration_cast<std::chrono::milliseconds>(waited).count(), forced,
                                  acknowledged, wait_failed));
}

std::wstring argument(std::wstring_view text) {
    std::wstring result = L"\"";
    std::size_t slashes{};
    for (const auto ch : text) {
        if (ch == L'\\') {
            ++slashes;
            continue;
        }
        result.append(ch == L'\"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        result += ch;
    }
    result.append(slashes * 2, L'\\');
    result += L'\"';
    return result;
}

struct Handle {
    HANDLE value{};

    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
};
}

PlayProcess::~PlayProcess() {
    close();
    // A failed bounded close has already reported an unobserved exit. Destruction
    // relinquishes OS ownership without restarting waits or claiming completion.
    release();
}

Result<void> PlayProcess::start(const std::filesystem::path& project, const std::filesystem::path& scene, bool headless,
                                std::uint64_t frames, const std::filesystem::path& report, bool preview_ui, bool halt,
                                std::unique_ptr<LiveLink>* live) {
    if (running())
        return failure("A project is already running", ERROR_BUSY);
    for (auto& output : output_)
        output.bytes.reserve(output_capacity);
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    Handle stop{CreateEventW(&security, TRUE, FALSE, nullptr)};
    if (!stop.value)
        return failure("Cannot create the run stop event");
    Handle stop_ack{CreateEventW(&security, TRUE, FALSE, nullptr)};
    if (!stop_ack.value)
        return failure("Cannot create the run stop acknowledgement event");
    Handle read_out;
    Handle write_out;
    Handle read_err;
    Handle write_err;
    if (!CreatePipe(&read_out.value, &write_out.value, &security, 65536) ||
        !SetHandleInformation(read_out.value, HANDLE_FLAG_INHERIT, 0) ||
        !CreatePipe(&read_err.value, &write_err.value, &security, 65536) ||
        !SetHandleInformation(read_err.value, HANDLE_FLAG_INHERIT, 0))
        return failure("Cannot create the run output channel");
    Handle input{
        CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr)};
    if (input.value == INVALID_HANDLE_VALUE)
        return failure("Cannot open run input");
    Handle to_play;
    Handle from_editor;
    Handle from_play;
    Handle to_editor; // Change link: editor->Play pipe, then Play->editor.
    if (live && (!CreatePipe(&from_editor.value, &to_play.value, &security, 1 << 20) ||
                 !SetHandleInformation(to_play.value, HANDLE_FLAG_INHERIT, 0) ||
                 !CreatePipe(&from_play.value, &to_editor.value, &security, 1 << 20) ||
                 !SetHandleInformation(from_play.value, HANDLE_FLAG_INHERIT, 0)))
        return failure("Cannot create the Play change link");
    Handle job{CreateJobObjectW(nullptr, nullptr)};
    if (!job.value)
        return failure("Cannot create the run process group");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        return failure("Cannot configure the run process group");
    std::wstring executable(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!length || length >= executable.size())
        return failure("Cannot locate the project runner");
    executable.resize(length);
    executable = (std::filesystem::path(executable).parent_path() / L"atelier_editor.exe").wstring();
    static std::uint64_t launch{}; // Owner thread only; not an object identity.
    const auto* parent = logging::current_run();
    std::filesystem::path directory;
    if (parent)
        directory = parent->info().directory / (L"runner-" + std::to_wstring(++launch));
    else {
        const auto storage = control::default_directory();
        if (!storage)
            return std::unexpected(Error{ErrorCode::invalid_storage, storage.error().what()});
        std::error_code error;
        directory = std::filesystem::absolute(storage->parent_path() / "Logs" / control::unique_id(), error);
        if (error)
            return std::unexpected(Error{ErrorCode::invalid_storage, platform::error_message(error)});
    }
    auto command = argument(executable) + L" --run-project " + argument(project.wstring()) + L" --stop-event " +
                   std::to_wstring(reinterpret_cast<std::uintptr_t>(stop.value)) + L" --stop-ack-event " +
                   std::to_wstring(reinterpret_cast<std::uintptr_t>(stop_ack.value)) + L" --log-directory " +
                   argument(directory.wstring());
    if (parent)
        command += L" --parent-run " + argument(platform::utf8_to_utf16_validated(parent->info().run_id));
    if (!scene.empty())
        command += L" --scene-snapshot " + argument(scene.wstring());
    if (preview_ui)
        command += L" --preview-ui";
    if (headless)
        command += L" --headless";
    if (halt)
        command += L" --halt-on-fault";
    if (frames)
        command += L" --frames " + std::to_wstring(frames);
    if (!report.empty())
        command += L" --report " + argument(report.wstring());
    if (live)
        command += L" --live-channel " + std::to_wstring(reinterpret_cast<std::uintptr_t>(from_editor.value)) + L" " +
                   std::to_wstring(reinterpret_cast<std::uintptr_t>(to_editor.value));
    SIZE_T bytes{};
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<std::byte> attributes(bytes);
    auto* list = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(list, 1, 0, &bytes))
        return failure("Cannot initialize run handle inheritance");
    struct Attributes {
        PPROC_THREAD_ATTRIBUTE_LIST value;

        ~Attributes() { DeleteProcThreadAttributeList(value); }
    } owner{list};
    std::vector<HANDLE> handles{write_out.value, write_err.value, stop.value, stop_ack.value, input.value};
    if (live)
        handles.insert(handles.end(), {from_editor.value, to_editor.value});
    if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles.data(),
                                   handles.size() * sizeof(HANDLE), nullptr, nullptr))
        return failure("Cannot restrict run handle inheritance");
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = list;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.value;
    startup.StartupInfo.hStdOutput = write_out.value;
    startup.StartupInfo.hStdError = write_err.value;
    PROCESS_INFORMATION process{};
    // Pipes and the stop/ack events are the entire console contract. A hidden console
    // would add a conhost descendant whose asynchronous exit looks like a writer
    // being killed when the root exits and we close its job.
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | DETACHED_PROCESS, nullptr, project.c_str(),
                        &startup.StartupInfo, &process))
        return failure("Cannot start the project runner");
    Handle child{process.hProcess};
    Handle thread{process.hThread};
    if (!AssignProcessToJobObject(job.value, child.value)) {
        const auto error = GetLastError();
        TerminateProcess(child.value, 1);
        WaitForSingleObject(child.value, 1000);
        return failure("Cannot attach the project runner to its process group", error);
    }
    if (ResumeThread(thread.value) == static_cast<DWORD>(-1)) {
        const auto error = GetLastError();
        TerminateJobObject(job.value, 1);
        WaitForSingleObject(child.value, 1000);
        return failure("Cannot resume the project runner", error);
    }
    process_ = std::exchange(child.value, nullptr);
    job_ = std::exchange(job.value, nullptr);
    stop_ = std::exchange(stop.value, nullptr);
    stop_ack_ = std::exchange(stop_ack.value, nullptr);
    output_[0].handle = std::exchange(read_out.value, nullptr);
    output_[1].handle = std::exchange(read_err.value, nullptr);
    for (auto& output : output_) {
        output.bytes.clear();
        output.status = {};
    }
    exit_code_.reset();
    stopping_ = acknowledged_ = forced_ = exited_ = closing_ = false;
    log_directory_ = std::move(directory);
    if (live)
        *live =
            std::make_unique<LiveLink>(std::exchange(from_play.value, nullptr), std::exchange(to_play.value, nullptr));
    return {};
}

void PlayProcess::stop() noexcept {
    if (!process_ || exited_ || stopping_)
        return;
    const auto path = platform::utf16_to_utf8(log_directory_.generic_wstring());
    if (path) {
        const std::array fields{logging::Field{"directory", *path}};
        static_cast<void>(ATELIER_LOG_WITH(logging::Severity::info, "play", "play.stop_requested",
                                           logging::native::Context{.fields = fields}, "Play stop requested"));
    } else {
        std::fputs("Play stop diagnostic capture failed.\n", stderr);
    }
    stopping_ = true;
    stop_began_ = std::chrono::steady_clock::now();
    if (!SetEvent(stop_))
        report_os_failure("SetEvent(stop)", GetLastError());
}

void PlayProcess::drain() noexcept {
    std::array<char, 4096> buffer{};
    // A byte limit alone can still perform 65,536 one-byte reads. Bound syscall
    // count too, and service both streams each round so a noisy stdout cannot starve stderr.
    for (unsigned round = 0; round < 8; ++round) {
        bool progressed{};
        for (auto& output : output_) {
            if (!output.handle)
                continue;
            const auto finish = [&](DWORD error) {
                if (error == ERROR_BROKEN_PIPE)
                    output.status.eof = true;
                else {
                    if (!output.status.read_error)
                        output.status.read_error = error;
                    output.status.tail_loss_unknown = true;
                }
                CloseHandle(output.handle);
                output.handle = nullptr;
            };
            DWORD available{};
            DWORD received{};
            if (!PeekNamedPipe(output.handle, nullptr, 0, nullptr, &available, nullptr)) {
                finish(GetLastError());
                continue;
            }
            if (!available)
                continue;
            const auto count = static_cast<DWORD>(std::min<std::size_t>(buffer.size(), available));
            if (!ReadFile(output.handle, buffer.data(), count, &received, nullptr)) {
                finish(GetLastError());
                continue;
            }
            if (!received) {
                finish(ERROR_READ_FAULT);
                continue;
            }
            progressed = true;
            output.status.received_bytes += received;
            const auto overflow =
                output.bytes.size() + received > output_capacity ? output.bytes.size() + received - output_capacity : 0;
            if (overflow) {
                output.bytes.erase(0, overflow);
                output.status.dropped_bytes += overflow;
            }
            output.bytes.append(buffer.data(), received);
        }
        if (!progressed)
            break;
    }
}

void PlayProcess::observe_acknowledgement() noexcept {
    if (!stopping_ || !stop_ack_ || acknowledged_)
        return;
    const auto waited = WaitForSingleObject(stop_ack_, 0);
    if (waited == WAIT_FAILED)
        report_os_failure("WaitForSingleObject(stop acknowledgement)", GetLastError());
    if (waited != WAIT_OBJECT_0)
        return;
    acknowledged_ = true;
    try {
        const std::array fields{logging::Field{"waited_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                                                                std::chrono::steady_clock::now() - stop_began_)
                                                                .count()}};
        static_cast<void>(ATELIER_LOG_WITH(logging::Severity::info, "play", "play.stop_acknowledged",
                                           logging::native::Context{.fields = fields}, "Play stop acknowledged"));
    } catch (...) {
        std::fputs("Play acknowledgement diagnostic capture failed.\n", stderr);
    }
}

DWORD PlayProcess::observe_exit(DWORD timeout_ms) noexcept {
    if (!process_ || exited_)
        return WAIT_OBJECT_0;
    const auto waited = WaitForSingleObject(process_, timeout_ms);
    if (waited == WAIT_FAILED)
        report_os_failure("WaitForSingleObject(observe exit)", GetLastError());
    observe_acknowledgement();
    if (waited != WAIT_OBJECT_0)
        return waited;
    DWORD code{};
    if (!GetExitCodeProcess(process_, &code)) {
        report_os_failure("GetExitCodeProcess", GetLastError());
        return WAIT_FAILED;
    }
    exited_ = true;
    exit_code_ = code;
    // Observation owns the event, including a late exit after close's budget.
    const auto path = platform::utf16_to_utf8(log_directory_.generic_wstring());
    if (path) {
        const std::array fields{logging::Field{"directory", *path}, logging::Field{"forced", forced_},
                                logging::Field{"exit_code", std::uint64_t{code}}};
        static_cast<void>(ATELIER_LOG_WITH(logging::Severity::info, "play", "play.process_ended",
                                           logging::native::Context{.fields = fields},
                                           "Runner process ended; retained JSONL tail remains readable."));
    } else {
        std::fputs("Play exit diagnostic capture failed.\n", stderr);
    }
    // Preserve the existing process-tree ownership: descendants do not outlive
    // their root. Killing a remaining writer can lose bytes not yet in its pipe.
    if (job_) {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        const bool queried = QueryInformationJobObject(job_, JobObjectBasicAccountingInformation, &accounting,
                                                       sizeof(accounting), nullptr) != FALSE;
        if (!queried || accounting.ActiveProcesses)
            for (auto& output : output_)
                output.status.tail_loss_unknown = true;
        CloseHandle(job_);
        job_ = nullptr;
    }
    return WAIT_OBJECT_0;
}

void PlayProcess::force() noexcept {
    if (forced_)
        return;
    forced_ = true;
    for (auto& output : output_)
        output.status.tail_loss_unknown = true;
    if (job_ && !TerminateJobObject(job_, 1))
        report_os_failure("TerminateJobObject", GetLastError());
}

void PlayProcess::poll() noexcept {
    if (!process_)
        return;
    observe_exit();
    if (!exited_ && !forced_ && stopping_) {
        const auto observed = observe_exit(); // Zero wait, immediately before any force decision.
        const auto waited = std::chrono::steady_clock::now() - stop_began_;
        if (observed != WAIT_OBJECT_0 && waited >= std::chrono::seconds(acknowledged_ ? 4 : 2)) {
            report_forced(log_directory_, waited, exited_, exit_code_, acknowledged_);
            force();
            if (acknowledged_)
                report_exit_unobserved(waited, forced_, acknowledged_, observed == WAIT_FAILED);
        }
    }
    drain();
    observe_exit();
    if (exited_ && !output_status().pending)
        release();
}

PlayProcess::OutputStatus PlayProcess::output_status() const noexcept {
    return {output_[0].status, output_[1].status, exited_, output_[0].handle || output_[1].handle};
}

PlayProcess::RawOutput PlayProcess::take_raw_output() {
    // Copy before clearing either buffer. A failed allocation leaves all bytes
    // available, and keeping capacity makes subsequent byte draining allocation-free.
    RawOutput result{{output_[0].bytes, output_[0].status.received_bytes - output_[0].bytes.size()},
                     {output_[1].bytes, output_[1].status.received_bytes - output_[1].bytes.size()},
                     output_status()};
    for (auto& output : output_)
        output.bytes.clear();
    return result;
}

void PlayProcess::abandon(OutputPipe& output) noexcept {
    if (!output.handle)
        return;
    DWORD available{};
    if (PeekNamedPipe(output.handle, nullptr, 0, nullptr, &available, nullptr)) {
        output.status.abandoned_bytes += available;
        output.status.tail_loss_unknown = true;
    } else if (const auto error = GetLastError(); error == ERROR_BROKEN_PIPE)
        output.status.eof = true;
    else {
        if (!output.status.read_error)
            output.status.read_error = error;
        output.status.tail_loss_unknown = true;
    }
    CloseHandle(output.handle);
    output.handle = nullptr;
}

void PlayProcess::release() noexcept {
    for (auto* handle : {&process_, &job_, &stop_, &stop_ack_})
        if (*handle) {
            CloseHandle(*handle);
            *handle = nullptr;
        }
    stopping_ = false;
}

void PlayProcess::close() noexcept {
    if (closing_) {
        poll();
        return;
    }
    closing_ = true;
    const bool active = process_ != nullptr;
    const auto began = std::chrono::steady_clock::now();
    const auto deadline = began + std::chrono::seconds(4);
    if (active)
        static_cast<void>(ATELIER_LOG(logging::Severity::info, "play", "play.close_begin",
                                      "Closing Play: exited={} forced={}", exited_, forced_));
    stop();
    // Acknowledgement allows normal cleanup within the same total exit budget.
    // Keep the existing four initial and four final output-drain budgets.
    DWORD observed = WAIT_TIMEOUT;
    unsigned drains{};
    while (process_ && !exited_ && !forced_) {
        if (drains < 4) {
            drain();
            ++drains;
        }
        observe_acknowledgement();
        const auto until = acknowledged_ ? deadline : began + std::chrono::milliseconds(500);
        const auto remaining =
            std::chrono::ceil<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count();
        observed = observe_exit(static_cast<DWORD>(std::clamp<std::int64_t>(remaining, 0, 125)));
        if (observed == WAIT_FAILED ||
            std::chrono::steady_clock::now() >= (acknowledged_ ? deadline : began + std::chrono::milliseconds(500)))
            break;
    }
    // Do not force based on an earlier timed-out wait if exit has since become observable.
    if (process_ && !exited_ && observed != WAIT_FAILED)
        observed = observe_exit();
    bool exit_unobserved{};
    if (process_ && !exited_) {
        if (!forced_)
            report_forced(log_directory_, std::chrono::steady_clock::now() - began, exited_, exit_code_, acknowledged_);
        force();
        const auto remaining =
            std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0 || observed == WAIT_FAILED) {
            report_exit_unobserved(std::chrono::steady_clock::now() - began, forced_, acknowledged_,
                                   observed == WAIT_FAILED);
            exit_unobserved = true;
        }
        if (observed != WAIT_FAILED)
            observed = observe_exit(static_cast<DWORD>(std::max<std::int64_t>(0, remaining)));
    }
    for (unsigned pass = 0; pass < 4 && process_; ++pass) {
        drain();
        if (observed != WAIT_FAILED)
            observed = observe_exit();
    }
    for (auto& output : output_)
        abandon(output);
    if (!process_ || exited_)
        release();
    else if (!exit_unobserved)
        report_exit_unobserved(std::chrono::steady_clock::now() - began, forced_, acknowledged_,
                               observed == WAIT_FAILED);
    if (active)
        static_cast<void>(ATELIER_LOG(logging::Severity::info, "play", "play.close_end",
                                      "Play closed: running={} exited={} forced={} exit_code={}", running(), exited_,
                                      forced_, exit_code_ ? std::int64_t{*exit_code_} : -1));
}
}
