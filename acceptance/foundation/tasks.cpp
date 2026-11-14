#include "../helpers.hpp"
#include "foundation/tasks.hpp"
#include <atomic>
#include <memory>
#include <thread>

namespace {
using namespace atelier;

using namespace atelier::acceptance::helpers;

struct OwnedInput {
    std::thread::id owner;

    std::atomic<bool>& wrong_thread;

    ~OwnedInput() {
        if (std::this_thread::get_id() != owner)
            wrong_thread = true;
    }
};
}

void verify_tasks() {
    std::atomic<bool> wrong_thread = false;
    auto owned = std::make_shared<OwnedInput>(std::this_thread::get_id(), wrong_thread);
    const std::weak_ptr<OwnedInput> lifetime = owned;
    {
        tasks::JoinGroup group;
        group.run([owned = std::move(owned)]() noexcept -> std::expected<void, tasks::Failure> {
            require(owned != nullptr, "Task lost its input.");
            return std::unexpected(tasks::Failure{17});
        });
        const auto joined = group.join();
        require(!joined && lifetime.expired() && !wrong_thread && !tasks::in_parallel_body(),
                "Failed join retained work or released its capture on a worker.");
    }
    unsigned result{};
    tasks::JoinGroup next;
    next.run([&]() noexcept -> std::expected<void, tasks::Failure> {
        result = 123;
        return {};
    });
    require(next.join() == tasks::Completion::completed && result == 123,
            "A failed group poisoned the next operation.");
}
