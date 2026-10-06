#include "application/play_process.hpp"
#include <array>
#include <algorithm>
#include <utility>
#include <vector>

namespace atelier::application {
namespace {
auto failure(std::string message,DWORD error=GetLastError()) {
    return std::unexpected(Error{ErrorCode::invalid_storage,std::move(message)+" (Windows "+std::to_string(error)+")."});
}
std::wstring argument(std::wstring_view text) {
    std::wstring result=L"\"";std::size_t slashes{};
    for (const auto ch:text) {
        if (ch==L'\\') {++slashes;continue;}
        result.append(ch==L'\"'?slashes*2+1:slashes,L'\\');slashes=0;result+=ch;
    }
    result.append(slashes*2,L'\\');result+=L'\"';return result;
}
struct Handle {
    HANDLE value{};
    ~Handle(){if(value && value!=INVALID_HANDLE_VALUE) CloseHandle(value);}
};
}
PlayProcess::~PlayProcess(){close();}
Result<void> PlayProcess::start(const std::filesystem::path& project,const std::filesystem::path& scene,
    bool headless,std::uint64_t frames,const std::filesystem::path& report,bool preview_ui) {
    if (running()) return failure("A project is already running",ERROR_BUSY);
    exit_code_.reset();stopping_=forced_=false;pending_output_.clear();
    SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
    Handle stop{CreateEventW(&security,TRUE,FALSE,nullptr)};if (!stop.value) return failure("Cannot create the run stop event");
    Handle read,write;
    if (!CreatePipe(&read.value,&write.value,&security,65536) || !SetHandleInformation(read.value,HANDLE_FLAG_INHERIT,0))
        return failure("Cannot create the run output channel");
    Handle input{CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,0,nullptr)};
    if (input.value==INVALID_HANDLE_VALUE) return failure("Cannot open run input");
    Handle job{CreateJobObjectW(nullptr,nullptr)};if (!job.value) return failure("Cannot create the run process group");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits))) return failure("Cannot configure the run process group");
    std::wstring executable(32768,L'\0');const auto length=GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));
    if (!length || length>=executable.size()) return failure("Cannot locate the project runner");
    executable.resize(length);
    executable=(std::filesystem::path(executable).parent_path()/L"atelier_editor.exe").wstring();
    auto command=argument(executable)+L" --run-project "+argument(project.wstring())+
        L" --stop-event "+std::to_wstring(reinterpret_cast<std::uintptr_t>(stop.value));
    if (!scene.empty()) command+=L" --scene-snapshot "+argument(scene.wstring());
    if (preview_ui) command+=L" --preview-ui";
    if (headless) command+=L" --headless";
    if (frames) command+=L" --frames "+std::to_wstring(frames);
    if (!report.empty()) command+=L" --report "+argument(report.wstring());
    SIZE_T bytes{};InitializeProcThreadAttributeList(nullptr,1,0,&bytes);
    std::vector<std::byte> attributes(bytes);auto* list=reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(list,1,0,&bytes)) return failure("Cannot initialize run handle inheritance");
    struct Attributes {PPROC_THREAD_ATTRIBUTE_LIST value;~Attributes(){DeleteProcThreadAttributeList(value);}} owner{list};
    std::array handles{write.value,stop.value,input.value};
    if (!UpdateProcThreadAttribute(list,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,handles.data(),sizeof(handles),nullptr,nullptr))
        return failure("Cannot restrict run handle inheritance");
    STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);startup.lpAttributeList=list;
    startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput=input.value;startup.StartupInfo.hStdOutput=startup.StartupInfo.hStdError=write.value;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,TRUE,
        EXTENDED_STARTUPINFO_PRESENT|CREATE_SUSPENDED|CREATE_NO_WINDOW,nullptr,project.c_str(),&startup.StartupInfo,&process))
        return failure("Cannot start the project runner");
    Handle child{process.hProcess},thread{process.hThread};
    if (!AssignProcessToJobObject(job.value,child.value)) {
        const auto error=GetLastError();TerminateProcess(child.value,1);WaitForSingleObject(child.value,1000);
        return failure("Cannot attach the project runner to its process group",error);
    }
    if (ResumeThread(thread.value)==static_cast<DWORD>(-1)) {
        const auto error=GetLastError();TerminateJobObject(job.value,1);WaitForSingleObject(child.value,1000);
        return failure("Cannot resume the project runner",error);
    }
    process_=std::exchange(child.value,nullptr);job_=std::exchange(job.value,nullptr);
    stop_=std::exchange(stop.value,nullptr);output_=std::exchange(read.value,nullptr);return {};
}
void PlayProcess::stop() noexcept {
    if (!process_ || stopping_) return;
    stopping_=true;stop_deadline_=std::chrono::steady_clock::now()+std::chrono::seconds(2);SetEvent(stop_);
}
void PlayProcess::drain() {
    if (!output_) return;
    std::array<char,4096> buffer{};DWORD available{},received{};std::size_t budget=64*1024;
    while (budget && PeekNamedPipe(output_,nullptr,0,nullptr,&available,nullptr) && available) {
        const auto count=static_cast<DWORD>(std::min<std::size_t>({buffer.size(),available,budget}));
        if (!ReadFile(output_,buffer.data(),count,&received,nullptr) || !received) break;
        pending_output_.append(buffer.data(),received);budget-=received;
    }
    if (pending_output_.size()>64*1024) {
        auto first=pending_output_.find('\n',pending_output_.size()-64*1024);
        pending_output_.erase(0,first==std::string::npos?pending_output_.size()-64*1024:first+1);
    }
}
void PlayProcess::poll() {
    if (!process_) return;
    drain();
    if (stopping_ && std::chrono::steady_clock::now()>=stop_deadline_ && WaitForSingleObject(process_,0)==WAIT_TIMEOUT) {
        forced_=true;TerminateJobObject(job_,1);
    }
    if (WaitForSingleObject(process_,0)!=WAIT_OBJECT_0) return;
    DWORD code{};if (GetExitCodeProcess(process_,&code)) exit_code_=code;
    drain();release();
}
std::string PlayProcess::take_output() {return std::exchange(pending_output_,{});}
void PlayProcess::release() noexcept {
    for (auto* handle:{&process_,&job_,&stop_,&output_}) if (*handle) {CloseHandle(*handle);*handle=nullptr;}
    stopping_=false;
}
void PlayProcess::close() noexcept {
    if (process_) {
        SetEvent(stop_);
        if (WaitForSingleObject(process_,500)==WAIT_TIMEOUT) {forced_=true;TerminateJobObject(job_,1);WaitForSingleObject(process_,1000);}
    }
    release();
}
}
