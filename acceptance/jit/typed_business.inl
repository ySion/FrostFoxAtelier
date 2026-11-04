// Included by business.cpp inside its anonymous namespace.
struct TypedUiController {
    float opacity{};

    float cursor{};

    std::uint32_t selected{};

    std::uint32_t revisions{};

    math::Vector3 bounds{};

    void reset_focus() {
        opacity = 1.0f;
        cursor = 2.0f;
        selected = 0;
        revisions = 1;
    }

    void apply_selection(float alpha, std::uint32_t choice, float offset, std::uint32_t repeats) {
        opacity += alpha;
        cursor += offset * static_cast<float>(repeats);
        selected = choice;
        revisions += repeats;
    }

    void accumulate(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d, std::uint32_t e,
                    std::uint32_t f, std::uint32_t g, std::uint32_t h, std::uint32_t i) {
        selected += a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h + 9 * i;
    }

    void explode() {
        ++revisions;
        throw std::runtime_error("Typed UI native failure.");
    }

    std::uint32_t bump(std::uint32_t amount) {
        revisions += amount;
        return revisions;
    }

    std::uint32_t first() {
        selected = selected * 10 + 1;
        return 11;
    }

    std::uint32_t next(std::uint32_t amount) {
        selected = selected * 10 + 2;
        return amount + 20;
    }

    std::uint32_t combine(std::uint32_t left, std::uint32_t right) {
        selected = selected * 10 + 3;
        return left * 100 + right;
    }
};

TypeId register_typed_ui(Runtime& runtime) {
    return take(NativeRegistration<TypedUiController>{runtime, "ui.NativeController", TypeKind::object}
                    .field<&TypedUiController::opacity>("opacity")
                    .field<&TypedUiController::selected>("selected")
                    .field<&TypedUiController::cursor>("cursor")
                    .field<&TypedUiController::revisions>("revisions")
                    .field<&TypedUiController::bounds>("bounds")
                    .range({.hard_min = Scalar{-1.0f}, .hard_max = Scalar{2.0f}})
                    .method<&TypedUiController::reset_focus>("reset_focus")
                    .method<&TypedUiController::apply_selection>("apply_selection")
                    .method<&TypedUiController::accumulate>("accumulate")
                    .method<&TypedUiController::explode>("explode")
                    .method<&TypedUiController::bump>("bump")
                    .method<&TypedUiController::first>("first")
                    .method<&TypedUiController::next>("next")
                    .method<&TypedUiController::combine>("combine")
                    .publish());
}

void typed_value(BusinessValue actual, BusinessValue expected, const char* message) {
    require(actual.kind == expected.kind && actual.bits == expected.bits, message);
}

struct TypedTrace {
    bool fail_poll{};

    bool fail_finish{};

    bool receiver_at_finish{};
};

BusinessServices typed_trace_services() {
    auto callbacks = services;
    callbacks.finish = [](void* context) noexcept -> std::uint32_t {
        auto& invocation = *static_cast<Invocation*>(context);
        const auto status = bridge(invocation, [&]() -> Result<void> {
            auto& trace = *static_cast<TypedTrace*>(fixture(invocation).hook_state);
            trace.receiver_at_finish = invocation.frame.reference(receiver_slot).has_value();
            for (std::uint32_t index = business_root_slots; index < invocation.frame.size(); ++index)
                require(!invocation.frame.reference(index), "Typed return retained a user reference into finish.");
            for (const auto& observation : invocation.observations)
                require(observation.description().type == TypeId{}, "Typed return retained a weak slot into finish.");
            if (trace.fail_finish)
                return std::unexpected(Error{ErrorCode::invocation_failed, "Typed finish failure."});
            return {};
        });
        return status == 0 ? finish(context) : status;
    };
    return callbacks;
}

std::uint32_t typed_raw(Runtime& runtime, const Binding& binding, BusinessCode* code, ObjectId receiver,
                        std::span<const BusinessValue> arguments, BusinessValue& result, TypedTrace& trace) {
    auto activation = take(ExecutionScope::enter(runtime));
    FixtureInvocation observed{false,
                               [](void* context, Invocation& invocation) {
                                   if (static_cast<TypedTrace*>(context)->fail_poll &&
                                       fixture(invocation).stats.polls == 2)
                                       throw std::runtime_error("Typed poll failure.");
                               },
                               &trace};
    Invocation invocation(runtime, binding, atelier_business_layout(code), observed.observer(), nullptr, {},
                          &activation);
    take(invocation.frame.hold(receiver_slot, receiver));
    char diagnostic[512]{};
    const auto status =
        atelier_invoke_business(code, &invocation, arguments.data(), static_cast<std::uint32_t>(arguments.size()),
                                &result, diagnostic, sizeof(diagnostic));
    require(invocation.resources_cleared(), "Typed raw invocation relied on RAII cleanup.");
    return status;
}

void verify_typed_reference_results(Runtime& runtime, const Binding& binding) {
    auto code = compile_reference(binding, R"(
        float focus(float value, uint branch) {
            $T held = new $T();
            weak $T watched = observe(held);
            $T acquired = try_acquire(watched);
            self.opacity = 2.0;
            self.selected = 3;
            held.opacity = value;
            if (branch == 1) { self.explode(); }
            uint turn = 0;
            while (turn < 1) { turn = turn + 1; }
            return acquired.opacity + self.opacity;
        })",
                                  "typed-reference-return.source");
    auto receiver = take(runtime.create(binding.type));
    const BusinessValue sentinel{ValueKind::u32, 0xbad00badu};
    for (const unsigned scenario : {0u, 1u, 2u}) {
        TypedTrace trace;
        trace.fail_poll = scenario == 1;
        const std::array arguments{scalar(4.0f), scalar(scenario == 0 ? 1u : 0u)};
        auto output = sentinel;
        const auto status = typed_raw(runtime, binding, code.get(), receiver.id(), arguments, output, trace);
        require((status == 0) == (scenario == 2), "Native/poll failure or subsequent recovery was incorrect.");
        typed_value(output, scenario == 2 ? scalar(6.0f) : sentinel, "Failed invocation committed a scalar result.");
        require(
            runtime.root_count() == 1 &&
                std::get<float>(take(runtime.read(receiver.id(), fixture_field(binding, "opacity")))) == 2.0f &&
                std::get<std::uint32_t>(take(runtime.read(receiver.id(), fixture_field(binding, "selected")))) == 3 &&
                std::get<std::uint32_t>(take(runtime.read(receiver.id(), fixture_field(binding, "revisions")))) == 1,
            "Typed failure rolled back native effects or retained a root.");
        collect(runtime);
        require(runtime.live_objects() == 1, "Scalar-return cleanup retained a source-created object.");
    }
}

void verify_typed_return_cleanup(Runtime& runtime, const Binding& binding) {
    auto code = compile_bound(
        binding, reference_source("float focus(float value) { $T held = self; return value; }", binding.name),
        "typed-return-cleanup.source", typed_trace_services());
    auto receiver = take(runtime.create(binding.type));
    const std::array arguments{scalar(4.0f)};
    const BusinessValue sentinel{ValueKind::u32, 0xbad00badu};
    for (const bool fail : {true, false}) {
        TypedTrace trace;
        trace.fail_finish = fail;
        auto output = sentinel;
        const auto status = typed_raw(runtime, binding, code.get(), receiver.id(), arguments, output, trace);
        require((status != 0) == fail && trace.receiver_at_finish && runtime.root_count() == 1,
                "Finish failure/recovery changed receiver lifetime or leaked a root.");
        typed_value(output, fail ? sentinel : scalar(4.0f), "Finish failure committed or recovery lost its result.");
    }
    receiver.reset();
    collect(runtime);
    require(runtime.root_count() == 0 && runtime.live_objects() == 0, "Finish failure retained resources.");
}

void verify_native_result_abi(Runtime& runtime, const Binding& original) {
    struct Calls {
        bool reject;

        std::uint32_t count{};
    };
    static thread_local Calls calls{};
    using Entry = std::uint32_t (*)(NativeContext*, void*, std::uint32_t, void*, NativeFailure*) noexcept;
    static const auto entry = reinterpret_cast<Entry>(native_method<&TypedUiController::bump>("bump").native_entry);
    auto binding = original;
    for (auto& method : binding.methods)
        if (method.name == "bump")
            method.entry =
                reinterpret_cast<std::uintptr_t>(+[](NativeContext* context, void* receiver, std::uint32_t amount,
                                                     void* output, NativeFailure* failure) noexcept -> std::uint32_t {
                    const auto status = entry(context, receiver, amount, output, failure);
                    // The typed ABI cannot change a result tag or replace output storage.
                    // Fail after writing scratch to test success-only result commit.
                    if (++calls.count == 2 && calls.reject) {
                        *failure = {static_cast<std::uint32_t>(ErrorCode::invocation_failed), "Native result failure."};
                        return 1;
                    }
                    return status;
                });
    auto code = compile_bound(binding,
                              reference_source(R"(
        uint abi() {
            $T held = new $T();
            weak $T observed = observe(held);
            self.bump(0);
            return self.bump(0);
        })",
                                               binding.name),
                              "native-result-abi.source");
    auto receiver = take(runtime.create(binding.type));
    const BusinessValue sentinel{ValueKind::u32, 0xcafebabe};
    for (const bool reject : {true, false}) {
        {
            calls = {reject};
            auto activation = take(ExecutionScope::enter(runtime));
            Invocation invocation(runtime, binding, atelier_business_layout(code.get()), {}, nullptr, {}, &activation);
            take(invocation.frame.hold(receiver_slot, receiver.id()));
            auto output = sentinel;
            char diagnostic[512]{};
            const auto status =
                atelier_invoke_business(code.get(), &invocation, nullptr, 0, &output, diagnostic, sizeof(diagnostic));
            require((status != 0) == reject && calls.count == 2 && invocation.resources_cleared(),
                    "Failed second native result reused scratch or failed generated cleanup.");
            typed_value(output, reject ? sentinel : scalar(0u), "ABI failure/recovery changed caller output.");
        }
        collect(runtime);
        require(runtime.root_count() == 1 && runtime.live_objects() == 1, "Native result ABI retained a temporary.");
    }
}

void verify_typed_ui(Runtime& runtime, const Binding& binding) {
    auto many = compile_bound(binding, R"(
        float evaluate(float a, uint x, float b, uint y, float c, uint z, float d, uint q, float e, uint r) {
            a = a + b * 2.0 + c * 3.0 + d * 4.0 + e * 5.0;
            x = x + y * 10 + z * 100 + q * 1000 + r * 10000;
            if (x == 54321) { return a; }
            return 0.0;
        })",
                              "typed-many.source");
    const std::array arguments{scalar(1.0f), scalar(1u),   scalar(2.0f), scalar(2u),   scalar(3.0f),
                               scalar(3u),   scalar(4.0f), scalar(4u),   scalar(5.0f), scalar(5u)};
    typed_value(take(execute_fixture(runtime, binding, many.get(), {}, arguments)).value, scalar(55.0f),
                "Mixed source parameters lost order or scalar kind.");
    auto paint = compile_bound(binding, R"(
        float paint(float alpha, uint choice, float offset, uint repeats) {
            self.reset_focus();
            self.apply_selection(alpha, choice, offset, repeats);
            self.accumulate(1, 2, 3, 4, 5, 6, 7, 8, 9);
            self.opacity = self.opacity + 0.25;
            self.cursor = self.cursor + 1.0;
            self.revisions = self.revisions + 2;
            self.selected = self.selected + 1;
            return self.opacity * 2.0 + self.cursor;
        })",
                               "typed-ui.source");
    auto receiver = take(runtime.create(binding.type));
    const std::array paint_arguments{scalar(0.5f), scalar(7u), scalar(1.25f), scalar(2u)};
    typed_value(take(execute_fixture(runtime, binding, paint.get(), receiver.id(), paint_arguments)).value,
                scalar(9.0f), "UI field/method chain returned the wrong scalar.");
    require(std::get<float>(take(runtime.read(receiver.id(), fixture_field(binding, "opacity")))) == 1.75f &&
                std::get<float>(take(runtime.read(receiver.id(), fixture_field(binding, "cursor")))) == 5.5f &&
                std::get<std::uint32_t>(take(runtime.read(receiver.id(), fixture_field(binding, "selected")))) == 293 &&
                std::get<std::uint32_t>(take(runtime.read(receiver.id(), fixture_field(binding, "revisions")))) == 5,
            "Generic field/method indices or nine-argument packing changed UI state.");
    auto nested = compile_bound(binding, "uint nested() { return self.combine(self.first(), self.next(9)); }",
                                "native-nested-results.source");
    auto fresh = take(runtime.create(binding.type));
    typed_value(take(execute_fixture(runtime, binding, nested.get(), fresh.id(), {})).value, scalar(1129u),
                "Nested native evaluation overwrote an outer argument.");
    require(std::get<std::uint32_t>(take(runtime.read(fresh.id(), fixture_field(binding, "selected")))) == 123,
            "Native arguments were not evaluated left to right.");

    auto bounded = compile_bound(binding, R"(
        void bounded(float value) {
            self.bounds = vector3(-3.0, 4.0, 0.5);
            self.bounds.z = -5.0;
            self.bounds = vector3(1.0, 0.75, value);
        })",
                                 "native-field-bounds.source");
    for (const bool fail : {false, true}) {
        const auto cursor = take(runtime.change_cursor());
        const auto value = fail ? std::numeric_limits<float>::infinity() : 1.25f;
        const auto result = execute_fixture(runtime, binding, bounded.get(), fresh.id(), std::array{scalar(value)});
        math::Vector3 actual;
        take(runtime.read(fresh.id(), fixture_field(binding, "bounds"),
                          ValueOutput{take(runtime.native_type<math::Vector3>()), &actual}));
        require(static_cast<bool>(result) != fail && runtime.changed_since(fresh.id(), cursor) &&
                    actual == (fail ? math::Vector3{-1, 2, -1} : math::Vector3{1, 0.75f, 1.25f}),
                "Direct field bounds lost clamp/stamp or partially committed a failed vector write.");
    }
}

void verify_typed_business() {
    Runtime runtime;
    const auto binding = bind_type(runtime, register_typed_ui(runtime));
    verify_typed_reference_results(runtime, binding);
    collect(runtime);
    verify_typed_return_cleanup(runtime, binding);
    verify_native_result_abi(runtime, binding);
    collect(runtime);
    verify_typed_ui(runtime, binding);
}
