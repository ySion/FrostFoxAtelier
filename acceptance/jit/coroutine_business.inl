// Included beside typed_business.inl; exercises the real source/JIT/Runtime path.
void publish_coroutine(BusinessProgram& program, const char* name, std::string_view source) {
    auto candidate = take(BusinessProgram::prepare(take(program.request(name, source))));
    take(program.try_publish(candidate));
}
void verify_coroutine_scalars() {
    Runtime runtime;
    BusinessProgram program(runtime, {});
    publish_coroutine(program, "coroutine-scalars.source", R"(
        async uint count(uint limit) {
            uint total = 0;
            uint index = 0;
            while (index < limit) {
                total = total + index;
                yield;
                index = index + 1;
            }
            return total;
        })");
    require(take(program.describe()).execution == FunctionExecution::coroutine,
        "Coroutine execution kind was lost from the shared declaration.");
    const auto bound = take(program.bind<std::uint32_t, std::uint32_t>(FunctionExecution::coroutine));
    reject(program.bind<std::uint32_t, std::uint32_t>(), ErrorCode::argument_mismatch,
        "Default synchronous binding accepted a coroutine declaration.");
    reject(program.bind<float, std::uint32_t>(FunctionExecution::coroutine), ErrorCode::argument_mismatch,
        "Coroutine binding accepted the wrong result type.");
    reject(program.bind<std::uint32_t, float>(FunctionExecution::coroutine), ErrorCode::argument_mismatch,
        "Coroutine binding accepted the wrong parameter type.");
    std::array arguments{scalar(3u)};
    auto first = take(start_fixture(bound, {}, arguments));
    arguments[0] = scalar(2u);
    auto second = take(start_fixture(program.entry(), {}, arguments));
    require(first.state() == CoroutineState::ready && !first.result(), "Coroutine executed during creation.");
    reject(invoke_fixture(bound, {}, arguments), ErrorCode::invalid_definition, "Synchronous invocation accepted coroutine code.");
    for (unsigned i = 0; i < 3; ++i) require(take(first.resume()) == CoroutineState::suspended, "Loop did not suspend.");
    require(take(first.resume()) == CoroutineState::completed && std::get<std::uint32_t>(*take(first.result())) == 3,
        "Coroutine lost its copied parameter or scalar state across suspension.");
    for (unsigned i = 0; i < 2; ++i) require(take(second.resume()) == CoroutineState::suspended, "Second instance did not suspend.");
    require(take(second.resume()) == CoroutineState::completed && std::get<std::uint32_t>(*take(second.result())) == 1,
        "Concurrent coroutine instances shared local state.");
    reject(first.resume(), ErrorCode::invalid_object, "Completed coroutine resumed again.");
    first.cancel();
    require(first.state() == CoroutineState::completed, "Cancellation changed an already completed coroutine.");
    auto cancelled = take(start_fixture(program.entry(), {}, arguments));
    cancelled.cancel(); cancelled.cancel();
    require(cancelled.state() == CoroutineState::cancelled && !cancelled.result(), "Ready cancellation published a result.");
    reject(cancelled.resume(), ErrorCode::invalid_object, "Cancelled coroutine resumed.");
    take(runtime.check_code_publication());
    std::puts("[pass] Coroutine source: typed binding checks execution kind and scalar types; LLVM split frames preserve independent state and reject terminal resumes");
}

void verify_coroutine_references(Runtime& runtime, TypeId type) {
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
        })", binding.name);
    publish_coroutine(program, "coroutine-roots.source", source);
    const std::array arguments{scalar(5u)};
    auto task = take(start_fixture(program.entry(), {}, arguments));
    {
        auto outer = take(ExecutionScope::enter(runtime));
        require(take(task.resume()) == CoroutineState::suspended, "Reference coroutine did not suspend.");
        reject(runtime.begin_collection(), ErrorCode::busy, "Yield released an outer execution interval.");
        reject(runtime.collect_step({1024}), ErrorCode::busy, "Yield allowed GC inside an outer execution interval.");
    }
    collect(runtime);
    require(runtime.live_objects() == 1, "Suspended strong reference did not survive a complete GC cycle.");
    take(runtime.visit_roots({&task, [](void* context, const ObjectReference&) -> Result<void> {
        auto& paused = *static_cast<Coroutine*>(context);
        reject(paused.resume(), ErrorCode::busy, "Coroutine resumed inside root traversal.");
        require(paused.state() == CoroutineState::suspended, "Rejected admission terminated the paused frame.");
        return {};
    }}));
    take(runtime.check_code_publication());
    require(take(task.resume()) == CoroutineState::suspended, "Reference coroutine did not reach its second suspend.");
    collect(runtime);
    require(runtime.live_objects() == 0, "Suspended observation retained its target after the strong reference was cleared.");
    require(take(task.resume()) == CoroutineState::completed && std::get<std::uint32_t>(*take(task.result())) == 7 &&
        runtime.root_count() == 0, "Weak acquisition or completed frame cleanup was incorrect.");
    {
        auto abandoned = take(start_fixture(program.entry(), {}, arguments));
        take(abandoned.resume());
        collect(runtime);
        require(runtime.live_objects() == 1, "Drop scenario did not retain a live object.");
    }
    collect(runtime);
    require(runtime.root_count() == 0 && runtime.live_objects() == 0, "Dropping a suspended owner retained its roots.");

    // Inspect cancellation before Invocation's destructor can mask missing generated cleanup.
    auto code = compile_bound(binding, source, "coroutine-raw-cleanup.source");
    Invocation invocation(runtime, binding, atelier_business_layout(code.get()));
    BusinessCoroutine* raw{};
    char diagnostic[512]{};
    require(atelier_create_business_coroutine(code.get(), &invocation, arguments.data(), 1,
        &raw, diagnostic, sizeof(diagnostic)) == 0, diagnostic);
    code.reset();
    bool suspended{};
    const auto sentinel = scalar(0xcafebabeu);
    auto output = sentinel;
    {
        auto scope = take(ExecutionScope::enter(runtime));
        require(atelier_resume_business_coroutine(raw, &suspended, &output, diagnostic, sizeof(diagnostic)) == 0 && suspended,
            "Raw coroutine failed to suspend after its outer code owner was released.");
    }
    typed_value(output, sentinel, "Suspension overwrote caller result storage.");
    atelier_destroy_business_coroutine(raw);
    require(invocation.frame.empty(), "Cancellation relied on Invocation's RAII cleanup.");
    for (const auto& observed : invocation.observations)
        require(observed.description().type == TypeId{}, "Cancellation retained weak execution storage.");
    collect(runtime);
    require(runtime.root_count() == 0 && runtime.live_objects() == 0, "Cancelled coroutine retained source objects.");

    BusinessProgram stale(runtime, type);
    publish_coroutine(stale, "coroutine-stale.source", "async uint access() { yield; return self.bump(1); }");
    auto receiver = take(runtime.create(type));
    auto invalid = take(start_fixture(stale.entry(), receiver.id(), {}));
    take(invalid.resume());
    take(runtime.destroy(receiver.id()));
    receiver.reset();
    reject(invalid.resume(), ErrorCode::invalid_object, "Resume accessed an explicitly destroyed receiver.");
    require(invalid.state() == CoroutineState::failed && !invalid.result() && runtime.root_count() == 0,
        "Failed resumed access retained roots or published a result.");

    BusinessProgram weak_only(runtime, type);
    publish_coroutine(weak_only, "coroutine-weak-access.source", reference_source(R"(
        async uint access() {
            weak $T watched = observe(new $T());
            yield;
            return watched.bump(1);
        })", binding.name));
    auto weak = take(start_fixture(weak_only.entry(), {}, {}));
    take(weak.resume());
    collect(runtime);
    require(runtime.live_objects() == 0, "A suspended weak reference was silently strengthened.");
    reject(weak.resume(), ErrorCode::invalid_object, "Weak member access revived a collected target on resume.");
    require(weak.state() == CoroutineState::failed && runtime.root_count() == 0,
        "Expired weak access failed to clean up its coroutine.");

    auto callbacks = services;
    callbacks.call = [](void* context, std::uint32_t slot, std::uint32_t method,
        const BusinessValue* arguments, std::uint32_t count, BusinessValue* result) noexcept -> std::uint32_t {
        auto status = call_method(context, slot, method, arguments, count, result);
        if (status == 0 && result) result->kind = ValueKind::void_value;
        return status;
    };
    auto malformed_code = compile_bound(binding, "async uint malformed() { yield; return self.bump(0); }",
        "coroutine-abi-error.source", callbacks);
    auto target = take(runtime.create(type));
    Invocation malformed(runtime, binding, atelier_business_layout(malformed_code.get()));
    take(malformed.frame.hold(receiver_slot, target.id()));
    BusinessCoroutine* malformed_frame{};
    require(atelier_create_business_coroutine(malformed_code.get(), &malformed, nullptr, 0, &malformed_frame,
        diagnostic, sizeof(diagnostic)) == 0, diagnostic);
    output = sentinel;
    {
        auto scope = take(ExecutionScope::enter(runtime));
        require(atelier_resume_business_coroutine(malformed_frame, &suspended, &output, diagnostic, sizeof(diagnostic)) == 0 && suspended,
            "ABI-error coroutine did not reach its first suspend.");
        require(atelier_resume_business_coroutine(malformed_frame, &suspended, &output, diagnostic, sizeof(diagnostic)) != 0 &&
            std::string_view(diagnostic).find("Native call result contract violation") != std::string_view::npos && malformed.frame.empty(),
            "Resumed ABI result failure lost its diagnostic or explicit cleanup.");
    }
    typed_value(output, sentinel, "Failed coroutine ABI invocation committed an output.");
    atelier_destroy_business_coroutine(malformed_frame);
    target.reset(); collect(runtime);
}

void verify_coroutine_versions() {
    Runtime runtime;
    auto program = std::make_unique<BusinessProgram>(runtime, TypeId{});
    publish_coroutine(*program, "coroutine-v1.source", "async uint value() { yield; return 11; }");
    auto old_lease = take(program->pin());
    const auto old_address = old_lease.address();
    auto old = take(start_fixture(old_lease, {}, {}));
    old_lease = {};
    require(take(old.resume()) == CoroutineState::suspended, "Old coroutine did not suspend.");
    publish_coroutine(*program, "coroutine-v2.source", "async uint value() { yield; return 22; }");
    require_code_state(old_address, true);
    auto changed_kind = BusinessProgram::prepare(take(program->request("coroutine-sync.source", "uint value() { return 33; }")));
    require(!changed_kind, "Compatible publication changed a coroutine to a synchronous function.");
    auto current = take(start_fixture(program->entry(), {}, {}));
    require(take(old.resume()) == CoroutineState::completed && std::get<std::uint32_t>(*take(old.result())) == 11,
        "Old coroutine switched to a new version while suspended.");
    require_code_state(old_address, false);
    auto lease = take(program->pin());
    const auto current_address = lease.address();
    lease = {};
    program.reset();
    require_code_state(current_address, true);
    take(current.resume());
    require(take(current.resume()) == CoroutineState::completed && std::get<std::uint32_t>(*take(current.result())) == 22,
        "Closing the program invalidated an existing coroutine frame.");
    require_code_state(current_address, false);
    take(runtime.check_code_publication());
}

void verify_coroutine_control() {
    Runtime runtime;
    BusinessProgram program(runtime, {});
    publish_coroutine(program, "coroutine-control.source", "async uint control() { yield; return 9; }");
    enum class Mode { cancel, fail, discard };
    struct Control { std::optional<Coroutine>* owner; Mode mode; bool visited{}; };
    const auto hook = [](void* context, Invocation& invocation) {
        auto& control = *static_cast<Control*>(context);
        if (fixture(invocation).stats.polls != 2) return;
        control.visited = true;
        reject(control.owner->value().resume(), ErrorCode::busy, "A coroutine reentered itself.");
        reject(invocation.runtime.check_code_publication(), ErrorCode::busy, "Running coroutine allowed code publication.");
        if (control.mode == Mode::discard) { control.owner->reset(); return; }
        control.owner->value().cancel();
        if (control.mode == Mode::fail) throw std::runtime_error("Coroutine first cause.");
    };
    for (const auto mode : {Mode::cancel, Mode::fail, Mode::discard}) {
        FixtureInvocation observed;
        std::optional<Coroutine> task;
        Control control{&task, mode};
        observed.hook = hook;
        observed.hook_state = &control;
        task.emplace(take(ProgramAccess::start(program.entry(), {}, {}, observed.observer())));
        require(take(task->resume()) == CoroutineState::suspended, "Control coroutine did not suspend.");
        const auto outcome = task->resume();
        require(control.visited && runtime.root_count() == 0, "Control callback did not execute or retained roots.");
        if (mode == Mode::fail) {
            require(!outcome && outcome.error().message == "Coroutine first cause." &&
                task->state() == CoroutineState::failed && !task->result(), "Cancellation replaced the original failure.");
        } else {
            require(outcome && *outcome == CoroutineState::cancelled, "Running cancellation published a completed result.");
            if (task) require(!task->result(), "Cancelled coroutine exposed a result.");
        }
        take(runtime.check_code_publication());
    }
    BusinessProgram floating(runtime, {});
    publish_coroutine(floating, "coroutine-float.source", "async float value(float number) { yield; return number; }");
    for (const auto bits : {0x80000000u, 0x7fc12345u}) {
        const std::array arguments{BusinessValue{ValueKind::f32, bits}};
        auto task = take(start_fixture(floating.entry(), {}, arguments));
        take(task.resume()); take(task.resume());
        require(std::bit_cast<std::uint32_t>(std::get<float>(*take(task.result()))) == bits,
            "Coroutine frame changed float sign/NaN bits.");
    }
    BusinessProgram nothing(runtime, {});
    publish_coroutine(nothing, "coroutine-void.source", "async void nothing() { return; }");
    auto task = take(start_fixture(nothing.entry(), {}, {}));
    require(take(task.resume()) == CoroutineState::completed && !take(task.result()), "Void completion fabricated a scalar.");
    const auto invalid = BusinessProgram::prepare(take(nothing.request("invalid-yield.source", "void nothing() { yield; }")));
    require(!invalid && invalid.error().message.find("yield requires an async") != std::string::npos,
        "Synchronous source silently accepted yield.");
    const auto machine = register_machines(runtime)[0];
    const auto binding = bind_type(runtime, machine);
    auto receiver = take(runtime.create(machine));
    auto paused = take(start_fixture(program.entry(), {}, {}));
    take(paused.resume());
    NativeAction action{&paused, [](void* context, CallContext&) {
        auto& coroutine = *static_cast<Coroutine*>(context);
        reject(coroutine.resume(), ErrorCode::busy, "A coroutine resumed through a live native borrow.");
        require(coroutine.state() == CoroutineState::suspended, "Native-borrow admission failure terminated the coroutine.");
    }};
    {
        ActionScope scope(action);
        const std::array<Scalar, 1> arguments{0.0f};
        take(runtime.call(receiver.id(), fixture_method(binding, "advance"), arguments));
    }
    require(take(paused.resume()) == CoroutineState::completed, "Rejected native-borrow resume was not retryable.");
}

void verify_coroutines() {
    verify_coroutine_scalars();
    Runtime runtime;
    for (const auto type : register_typed_ui(runtime)) verify_coroutine_references(runtime, type);
    verify_coroutine_versions();
    verify_coroutine_control();
    std::puts("[pass] Coroutine lifecycle: suspended strong/weak roots, explicit cancellation cleanup, stale receivers and retriable admission");
    std::puts("[pass] Coroutine versions: old frames resume old code across publication and release Windows code/unwind state after terminal cleanup");
    std::puts("[pass] Coroutine control: reentry rejection, cooperative cancellation, active owner destruction, first-cause errors and result isolation");
}
