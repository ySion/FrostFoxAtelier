#pragma once

#ifndef NDEBUG
#include "foundation/tasks.hpp"
#include "memory/fatal.hpp"
#endif

namespace atelier::detail {

// Diagnose task-body misuse, including caller execution and unwinding. This is
// neither an owner-thread check nor a proof that captures are Runtime-free.
inline void assert_serial_access() noexcept {
#ifndef NDEBUG
    if (tasks::in_parallel_body())
        fatal("Runtime: access from a parallel task body is forbidden.");
#endif
}

}
