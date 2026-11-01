// Included beside typed_business.inl; exercises the real source/JIT/Runtime path.
void publish_coroutine(BusinessProgram& program, const char* name, std::string_view source) {
    auto candidate = take(BusinessProgram::prepare(take(program.request(name, source))));
    take(program.try_publish(candidate));
}

void verify_coroutine_references() {
    Runtime runtime;
    const auto type = register_typed_ui(runtime);
    const auto binding = bind_type(runtime, type);
    BusinessProgram program(runtime, type);
    const auto source = reference_source(R"(
        async uint references(uint amount) {
            $T held = new $T();
            weak $T observed = observe(held);
            held.bump(amount);
            yield;
            observed.bump(1);
            held = null;
            yield;
            if (try_acquire(observed) == null) { return 7; }
            return 99;
        })",
                                         binding.name);
    publish_coroutine(program, "coroutine-roots.source", source);
    const std::array arguments{scalar(5u)};
    auto task = take(start_fixture(program.entry(), {}, arguments));
    require(take(task.resume()) == CoroutineState::suspended, "Reference coroutine did not suspend.");
    collect(runtime);
    require(runtime.live_objects() == 1, "Suspended strong reference did not survive GC.");
    require(take(task.resume()) == CoroutineState::suspended, "Reference coroutine lost its second suspension.");
    collect(runtime);
    require(runtime.live_objects() == 0, "Suspended observation retained its target.");
    require(take(task.resume()) == CoroutineState::completed && std::get<std::uint32_t>(*take(task.result())) == 7 &&
                runtime.root_count() == 0,
            "Weak acquisition or completed frame cleanup was incorrect.");
    {
        auto abandoned = take(start_fixture(program.entry(), {}, arguments));
        take(abandoned.resume());
        collect(runtime);
        require(runtime.live_objects() == 1, "Drop scenario did not retain a live object.");
    }
    collect(runtime);
    require(runtime.root_count() == 0 && runtime.live_objects() == 0, "Dropping a suspended owner retained roots.");

    // Inspect cancellation before Invocation's destructor can mask missing generated cleanup.
    auto code = compile_bound(binding, source, "coroutine-raw-cleanup.source");
    Invocation invocation(runtime, binding, atelier_business_layout(code.get()));
    BusinessCoroutine* raw{};
    char diagnostic[512]{};
    require(atelier_create_business_coroutine(code.get(), &invocation, arguments.data(), 1, &raw, diagnostic,
                                              sizeof(diagnostic)) == 0,
            diagnostic);
    code.reset();
    bool suspended{};
    const auto sentinel = scalar(0xcafebabeu);
    auto output = sentinel;
    {
        auto scope = take(ExecutionScope::enter(runtime));
        require(atelier_resume_business_coroutine(raw, &suspended, &output, diagnostic, sizeof(diagnostic)) == 0 &&
                    suspended,
                "Raw coroutine failed to suspend after its outer code owner was released.");
    }
    typed_value(output, sentinel, "Suspension overwrote caller result storage.");
    atelier_destroy_business_coroutine(raw);
    require(invocation.resources_cleared(), "Cancellation relied on Invocation's RAII cleanup.");
    collect(runtime);
    require(runtime.root_count() == 0 && runtime.live_objects() == 0, "Cancelled coroutine retained source objects.");

    BusinessProgram stale(runtime, type);
    publish_coroutine(stale, "coroutine-stale.source", "async uint access() { yield; return self.bump(1); }");
    auto receiver = take(runtime.create(type));
    auto invalid = take(start_fixture(stale.entry(), receiver.id(), {}));
    take(invalid.resume());
    take(runtime.destroy(receiver.id()));
    receiver.reset();
    require(!invalid.resume() && invalid.state() == CoroutineState::failed && !invalid.result() &&
                runtime.root_count() == 0,
            "Destroyed-receiver resume retained roots or published a result.");

    BusinessProgram weak_only(runtime, type);
    publish_coroutine(weak_only, "coroutine-weak-access.source",
                      reference_source(R"(
        async uint access() {
            weak $T watched = observe(new $T());
            yield;
            return watched.bump(1);
        })",
                                       binding.name));
    auto weak = take(start_fixture(weak_only.entry(), {}, {}));
    take(weak.resume());
    collect(runtime);
    require(runtime.live_objects() == 0, "A suspended weak reference was silently strengthened.");
    require(!weak.resume() && weak.state() == CoroutineState::failed && runtime.root_count() == 0,
            "Expired weak access failed to clean up its coroutine.");

    auto callbacks = services;
    callbacks.call = [](void* context, std::uint32_t slot, std::uint32_t method, const BusinessValue* arguments,
                        std::uint32_t count, BusinessValue* result) noexcept -> std::uint32_t {
        const auto status = call_method(context, slot, method, arguments, count, result);
        if (status == 0 && result)
            result->kind = ValueKind::void_value;
        return status;
    };
    auto malformed_code = compile_bound(binding, "async uint malformed() { yield; return self.bump(0); }",
                                        "coroutine-abi-error.source", callbacks);
    auto target = take(runtime.create(type));
    Invocation malformed(runtime, binding, atelier_business_layout(malformed_code.get()));
    take(malformed.frame.hold(receiver_slot, target.id()));
    BusinessCoroutine* malformed_frame{};
    require(atelier_create_business_coroutine(malformed_code.get(), &malformed, nullptr, 0, &malformed_frame,
                                              diagnostic, sizeof(diagnostic)) == 0,
            diagnostic);
    output = sentinel;
    {
        auto scope = take(ExecutionScope::enter(runtime));
        require(atelier_resume_business_coroutine(malformed_frame, &suspended, &output, diagnostic,
                                                  sizeof(diagnostic)) == 0 &&
                    suspended,
                "ABI-error coroutine did not reach its first suspension.");
        require(atelier_resume_business_coroutine(malformed_frame, &suspended, &output, diagnostic,
                                                  sizeof(diagnostic)) != 0 &&
                    malformed.resources_cleared(),
                "Resumed ABI result failure skipped explicit cleanup.");
    }
    typed_value(output, sentinel, "Failed coroutine ABI invocation committed an output.");
    atelier_destroy_business_coroutine(malformed_frame);
    target.reset();
    collect(runtime);
}

void verify_coroutine_control() {
    Runtime runtime;
    BusinessProgram program(runtime, {});
    publish_coroutine(program, "coroutine-control.source", "async uint control() { yield; return 9; }");
    struct Control {
        std::optional<Coroutine>& owner;

        bool discard;

        bool visited{};
    };
    for (const bool discard : {false, true}) {
        FixtureInvocation observed;
        std::optional<Coroutine> task;
        Control control{task, discard};
        observed.hook = [](void* context, Invocation& invocation) {
            if (fixture(invocation).stats.polls != 2)
                return;
            auto& control = *static_cast<Control*>(context);
            control.visited = true;
            if (control.discard) {
                control.owner.reset();
                return;
            }
            control.owner->cancel();
            throw std::runtime_error("Coroutine failure after cancel.");
        };
        observed.hook_state = &control;
        task.emplace(take(ProgramAccess::start(program.entry(), {}, {}, observed.observer())));
        require(take(task->resume()) == CoroutineState::suspended, "Control coroutine did not suspend.");
        const auto outcome = task->resume();
        require(control.visited && runtime.root_count() == 0, "Control callback retained roots or did not execute.");
        if (discard)
            require(outcome && *outcome == CoroutineState::cancelled && !task,
                    "Public owner deletion published a completed result.");
        else
            require(!outcome && task->state() == CoroutineState::failed && !task->result(),
                    "Cancellation replaced the native failure or exposed a result.");
    }
}

void verify_coroutines() {
    verify_coroutine_references();
    verify_coroutine_control();
}
