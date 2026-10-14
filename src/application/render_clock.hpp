#pragma once
#include <algorithm>
#include <chrono>

namespace atelier::application {
// Presentation time belongs to accepted active rendering, not simulation.
// Both the editor and Project Run use this same pause/resume contract.
class RenderClock {
public:
    using Clock=std::chrono::steady_clock;
    float delta(Clock::time_point start,bool active) const noexcept {
        const float elapsed=std::chrono::duration<float>(start-previous_).count();
        // Long accepted frames still advance. Bounding the interval prevents
        // a stall from consuming the whole display history in one update.
        return active && previous_!=Clock::time_point{} && elapsed>=0?
            std::min(elapsed,.1f):0;
    }
    void accept(Clock::time_point start,bool active) noexcept {previous_=active?start:Clock::time_point{};}
    void reset() noexcept {previous_={};}
private:
    Clock::time_point previous_{};
};
}
