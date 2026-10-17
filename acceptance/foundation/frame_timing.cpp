#include "foundation/frame_timing.hpp"

#include <array>
#include <stdexcept>
#include <string>

namespace {
using namespace atelier;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

inline constinit timing::Marker outer{"test.outer", "Outer", &timing::frame};
inline constinit timing::Marker inner{"test.inner", "Inner", &outer};
inline constinit timing::Marker injected{"test.injected", "Injected", &outer};
inline constinit timing::Marker sibling{"test.sibling", "Sibling", &timing::frame};
inline constinit timing::Marker stray{"test.stray", "Stray", &timing::frame};
inline constinit timing::Marker same_key{"test.outer", "Duplicate key", &timing::frame};

template<class Exception, class Operation>
void rejects(Operation operation, const char* message) {
    try { operation(); } catch (const Exception&) { return; }
    throw std::runtime_error(message);
}
}

void verify_frame_timing() {
    // Rejected catalogs register nothing, so a corrected one can still be installed.
    constexpr std::array<timing::Marker*, 2> child_first{&inner, &outer};
    rejects<std::invalid_argument>([&] { timing::install({child_first}); }, "A child preceding its parent was installed.");
    constexpr std::array<timing::Marker*, 2> duplicate{&outer, &same_key};
    rejects<std::invalid_argument>([&] { timing::install({duplicate}); }, "A duplicate timing key was installed.");
    require(timing::catalog().empty() && !outer.registered() && !inner.registered() && !timing::frame.registered(),
        "A rejected timing catalog retained registrations.");

    timing::Recorder recorder;
    {
        timing::Recorder::Frame frame{recorder};
        timing::Scope scope{outer};
    }
    require(recorder.last().structure_errors == 2 && recorder.last().first_error == &timing::frame,
        "Recording before installation was not reported as a structure error.");

    constexpr std::array<timing::Marker*, 3> first{&outer, &inner, &injected};
    constexpr std::array<timing::Marker*, 1> second{&sibling};
    timing::install({first, second});
    rejects<std::logic_error>([&] { timing::install({}); }, "The timing catalog was installed twice.");
    const auto catalog = timing::catalog();
    require(catalog.size() == 5 && catalog[0] == &timing::frame && catalog[1] == &outer && catalog[2] == &inner &&
        catalog[3] == &injected && catalog[4] == &sibling && timing::depth(inner) == 2 && !stray.registered(),
        "The timing catalog does not follow module order.");

    { timing::Scope unbound{outer}; timing::record(injected, std::chrono::nanoseconds{5}); }
    {
        timing::Recorder::Frame frame{recorder};
        for (int i = 0; i < 3; ++i) {
            timing::Scope scope{outer};
            { timing::Scope nested{inner}; }
            timing::record(injected, std::chrono::nanoseconds{7});
        }
        { timing::Scope scope{sibling}; timing::Suspend suspend; timing::Scope hidden{inner}; }
        { timing::Scope misplaced{inner}; }
        { timing::Scope unknown{stray}; }
        timing::record(injected, std::chrono::nanoseconds{1});
        { timing::Suspend suspend; timing::Recorder::Frame again{recorder}; }
    }
    const auto& record = recorder.last();
    require(record.sequence == 2 && record.count(timing::frame) == 1 && record.count(outer) == 3 && record.count(inner) == 3 &&
        record.count(injected) == 3 && record.time(injected) == std::chrono::nanoseconds{21} && record.count(sibling) == 1,
        "Timing entries were not accumulated per frame.");
    require(record.structure_errors == 4 && record.first_error == &inner && record.count(stray) == 0,
        "Misplaced, unregistered, injected or nested records were not reported.");

    {
        timing::Recorder::Frame frame{recorder};
        timing::Recorder other;
        timing::Recorder::Frame nested{other};
        require(&nested.finish() != &other.last(), "A nested host frame bound a second Recorder.");
    }
    require(recorder.last().sequence == 3 && recorder.last().structure_errors == 1 && recorder.last().first_error == &timing::frame &&
        recorder.last().count(outer) == 0, "A nested host frame was not reported, or a previous frame leaked into the next.");
}
