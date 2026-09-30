// Included by business.cpp inside its anonymous namespace.
struct TypedUiController {
    float opacity{}, cursor{};
    std::uint32_t selected{}, revisions{};
    void reset_focus() { opacity = 1.0f; cursor = 2.0f; selected = 0; revisions = 1; }
    void apply_selection(float alpha, std::uint32_t choice, float offset, std::uint32_t repeats) {
        opacity += alpha; cursor += offset * static_cast<float>(repeats); selected = choice; revisions += repeats;
    }
    void accumulate(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d, std::uint32_t e,
        std::uint32_t f, std::uint32_t g, std::uint32_t h, std::uint32_t i) {
        selected += a + 2*b + 3*c + 4*d + 5*e + 6*f + 7*g + 8*h + 9*i;
    }
    void explode() { ++revisions; throw std::runtime_error("Typed UI native failure."); }
    std::uint32_t bump(std::uint32_t amount) { revisions += amount; return revisions; }
    float scale(float amount) const { return opacity * amount; }
    std::uint32_t first() { selected = selected * 10 + 1; return 11; }
    std::uint32_t next(std::uint32_t amount) { selected = selected * 10 + 2; return amount + 20; }
    std::uint32_t combine(std::uint32_t left, std::uint32_t right) {
        selected = selected * 10 + 3; return left * 100 + right;
    }
    std::uint32_t fail_result() { ++revisions; throw std::runtime_error("Typed UI native result failure."); }
};
FieldId typed_ui_field(CallContext& call, const char* name) {
    return take(call.runtime.find_field(take(call.runtime.type_of(call.self)), name));
}
Result<void> typed_ui_reset(CallContext& call) {
    take(call.runtime.write(call.self, typed_ui_field(call, "opacity"), 1.0f));
    take(call.runtime.write(call.self, typed_ui_field(call, "cursor"), 2.0f));
    take(call.runtime.write(call.self, typed_ui_field(call, "selected"), std::uint32_t{0}));
    return call.runtime.write(call.self, typed_ui_field(call, "revisions"), std::uint32_t{1});
}
Result<void> typed_ui_apply(CallContext& call) {
    const auto opacity = typed_ui_field(call, "opacity");
    const auto cursor = typed_ui_field(call, "cursor");
    const auto revisions = typed_ui_field(call, "revisions");
    take(call.runtime.write(call.self, opacity, std::get<float>(take(call.runtime.read(call.self, opacity))) +
        call.arguments[0].get<float>()));
    take(call.runtime.write(call.self, cursor, std::get<float>(take(call.runtime.read(call.self, cursor))) +
        call.arguments[2].get<float>() * static_cast<float>(call.arguments[3].get<std::uint32_t>())));
    take(call.runtime.write(call.self, typed_ui_field(call, "selected"), call.arguments[1].get<std::uint32_t>()));
    return call.runtime.write(call.self, revisions, std::get<std::uint32_t>(take(call.runtime.read(call.self, revisions))) +
        call.arguments[3].get<std::uint32_t>());
}
Result<void> typed_ui_accumulate(CallContext& call) {
    const auto selected = typed_ui_field(call, "selected");
    auto value = std::get<std::uint32_t>(take(call.runtime.read(call.self, selected)));
    for (std::size_t index = 0; index < call.arguments.size(); ++index)
        value += static_cast<std::uint32_t>(index + 1) * call.arguments[index].get<std::uint32_t>();
    return call.runtime.write(call.self, selected, value);
}
Result<void> typed_ui_explode(CallContext& call) {
    const auto revisions = typed_ui_field(call, "revisions");
    take(call.runtime.write(call.self, revisions,
        std::get<std::uint32_t>(take(call.runtime.read(call.self, revisions))) + 1));
    throw std::runtime_error("Typed UI native failure.");
}
Result<void> typed_ui_bump(CallContext& call) {
    const auto field = typed_ui_field(call, "revisions");
    const auto value = std::get<std::uint32_t>(take(call.runtime.read(call.self, field))) +
        call.arguments[0].get<std::uint32_t>();
    take(call.runtime.write(call.self, field, value));
    call.result = Scalar{value};
    return {};
}
Result<void> typed_ui_scale(CallContext& call) {
    call.result = Scalar{std::get<float>(take(call.runtime.read(call.self, typed_ui_field(call, "opacity")))) *
        call.arguments[0].get<float>()};
    return {};
}
Result<void> typed_ui_first(CallContext& call) {
    const auto field = typed_ui_field(call, "selected");
    const auto value = std::get<std::uint32_t>(take(call.runtime.read(call.self, field)));
    take(call.runtime.write(call.self, field, value * 10u + 1u));
    call.result = Scalar{std::uint32_t{11}};
    return {};
}
Result<void> typed_ui_next(CallContext& call) {
    const auto field = typed_ui_field(call, "selected");
    const auto value = std::get<std::uint32_t>(take(call.runtime.read(call.self, field)));
    take(call.runtime.write(call.self, field, value * 10u + 2u));
    call.result = Scalar{call.arguments[0].get<std::uint32_t>() + 20u};
    return {};
}
Result<void> typed_ui_combine(CallContext& call) {
    const auto field = typed_ui_field(call, "selected");
    const auto value = std::get<std::uint32_t>(take(call.runtime.read(call.self, field)));
    take(call.runtime.write(call.self, field, value * 10u + 3u));
    call.result = Scalar{call.arguments[0].get<std::uint32_t>() * 100u +
        call.arguments[1].get<std::uint32_t>()};
    return {};
}
Result<void> typed_ui_fail_result(CallContext& call) {
    const auto field = typed_ui_field(call, "revisions");
    take(call.runtime.write(call.self, field,
        std::get<std::uint32_t>(take(call.runtime.read(call.self, field))) + 1u));
    call.result = Scalar{std::uint32_t{0xdeadbeef}};
    throw std::runtime_error("Typed UI native result failure.");
}
std::array<TypeId, 2> register_typed_ui(Runtime& runtime) {
    const auto f32 = take(runtime.native_type<float>());
    const auto u32 = take(runtime.native_type<std::uint32_t>());
    const auto native = take(NativeRegistration<TypedUiController>{runtime, "ui.NativeController", TypeKind::object}
        .field<&TypedUiController::opacity>("opacity").field<&TypedUiController::selected>("selected")
        .field<&TypedUiController::cursor>("cursor").field<&TypedUiController::revisions>("revisions")
        .method<&TypedUiController::reset_focus>("reset_focus")
        .method<&TypedUiController::apply_selection>("apply_selection")
        .method<&TypedUiController::accumulate>("accumulate").method<&TypedUiController::explode>("explode")
        .method<&TypedUiController::bump>("bump").method<&TypedUiController::scale>("scale")
        .method<&TypedUiController::first>("first").method<&TypedUiController::next>("next")
        .method<&TypedUiController::combine>("combine").method<&TypedUiController::fail_result>("fail_result").publish());
    TypeDefinition described;
    described.name = "ui.DescribedController";
    described.fields = {{"revisions", {u32}}, {"cursor", {f32}}, {"selected", {u32}}, {"opacity", {f32}}};
    described.methods = {{"explode", {}, typed_ui_explode},
        {"accumulate", {{{u32},{u32},{u32},{u32},{u32},{u32},{u32},{u32},{u32}}}, typed_ui_accumulate},
        {"apply_selection", {{{f32},{u32},{f32},{u32}}}, typed_ui_apply}, {"reset_focus", {}, typed_ui_reset},
        {"bump", {{{u32}}, ValueDefinition{u32}}, typed_ui_bump},
        {"scale", {{{f32}}, ValueDefinition{f32}, ReceiverDefinition{TypeReference{described.name}, true}}, typed_ui_scale},
        {"first", {{}, ValueDefinition{u32}}, typed_ui_first},
        {"next", {{{u32}}, ValueDefinition{u32}}, typed_ui_next},
        {"combine", {{{u32},{u32}}, ValueDefinition{u32}}, typed_ui_combine},
        {"fail_result", {{}, ValueDefinition{u32}}, typed_ui_fail_result}};
    return {native, take(runtime.register_type(std::move(described)))};
}
void typed_value(BusinessScalar actual, BusinessScalar expected, const char* message) {
    require(actual.kind == expected.kind && actual.bits == expected.bits, message);
}
struct TypedTrace {
    std::uint32_t begins{}, finishes{};
    bool fail_begin{}, fail_poll{}, fail_finish{};
};
BusinessServices typed_trace_services() {
    auto callbacks = services;
    callbacks.begin = [](void* context) noexcept -> std::uint32_t {
        auto& invocation = *static_cast<Invocation*>(context);
        auto& trace = *static_cast<TypedTrace*>(fixture(invocation).hook_state);
        ++trace.begins;
        if (trace.fail_begin) { invocation.fail(ErrorCode::invocation_failed, "Typed begin failure."); return 31; }
        return begin(context);
    };
    callbacks.finish = [](void* context) noexcept -> std::uint32_t {
        auto& invocation = *static_cast<Invocation*>(context);
        const auto status = bridge(invocation, [&]() -> Result<void> {
            auto& trace = *static_cast<TypedTrace*>(fixture(invocation).hook_state);
            ++trace.finishes;
            for (std::size_t index = business_root_slots; index < invocation.roots.size(); ++index)
                require(!invocation.roots[index].registered(), "Typed return retained a user root into finish.");
            for (const auto& observation : invocation.observations)
                require(observation.description().type == TypeId{}, "Typed return retained a weak slot into finish.");
            if (trace.fail_finish)
                return std::unexpected(Error{ErrorCode::invocation_failed, "Typed finish failure."});
            return {};
        });
        return status == 0 ? finish(context) : 37;
    };
    callbacks.clear = [](void* context, std::uint32_t slot) noexcept {
        clear_slot(context, slot);
        auto& invocation = *static_cast<Invocation*>(context);
        if (invocation.failed) invocation.fail(ErrorCode::invalid_object, "Secondary cleanup diagnostic.");
    };
    return callbacks;
}
void typed_trace_poll(void* context, Invocation& invocation, const CollectionProgress&) {
    const auto& trace = *static_cast<TypedTrace*>(context);
    if (fixture(invocation).stats.polls == 2) {
        collect(invocation.runtime);
        if (trace.fail_poll) throw std::runtime_error("Typed poll failure.");
    }
}
struct TypedRawResult { std::uint32_t status; std::string error; std::size_t polls; };
TypedRawResult typed_raw(Runtime& runtime, const Binding& binding, BusinessCode* code, ObjectId receiver,
    const BusinessScalar* arguments, std::uint32_t count, BusinessScalar* result, TypedTrace& trace) {
    auto activation = ExecutionScope::enter(runtime);
    require(static_cast<bool>(activation), "Typed raw invocation could not enter execution.");
    FixtureInvocation observed{false, typed_trace_poll, &trace};
    Invocation invocation(runtime, binding, atelier_business_layout(code), {1}, observed.observer());
    if (receiver != ObjectId{}) take(invocation.frame.hold(receiver_slot, receiver));
    char diagnostic[512]{};
    const auto status = atelier_invoke_business(code, &invocation, arguments, count, result, diagnostic, sizeof(diagnostic));
    require(invocation.frame.empty(), "Typed raw invocation relied on RAII for strong-root cleanup.");
    for (const auto& observation : invocation.observations)
        require(observation.description().type == TypeId{}, "Typed raw invocation retained weak storage.");
    require(!fixture(invocation).output.registered(), "Typed invocation unexpectedly generated a snapshot.");
    return {status, invocation.failed ? invocation.error : diagnostic, fixture(invocation).stats.polls};
}
void verify_typed_scalars() {
    Runtime runtime;
    const auto binding = bind_type(runtime, {});
    auto zero = compile_bound(binding, "uint zero() { return 4294967295 + 1; }", "typed-zero.source");
    ExecutionStats stats;
    auto result = take(execute_fixture(runtime, binding, zero.get(), {}, {}, true, &stats));
    typed_value(result.value, scalar(std::uint32_t{0}), "Zero-argument uint wrap did not produce a successful zero result.");
    require(stats.entered && stats.explicit_cleanup && stats.polls == 1 && !result.snapshot.registered() &&
        runtime.root_count() == 0 && runtime.live_objects() == 0 && !atelier_business_info(zero.get()).requires_receiver,
        "Pure scalar node required a receiver, root or snapshot.");
    auto identity = compile_bound(binding, "float identity(float value) { return value; }", "typed-bits.source");
    for (const auto bits : {0u, 0x80000000u, 0x7fc12345u}) {
        const std::array arguments{BusinessScalar{ScalarKind::f32, bits}};
        typed_value(take(execute_fixture(runtime, binding, identity.get(), {}, arguments)).value, arguments[0],
            "float identity changed zero/sign/NaN payload bits.");
    }
    auto many = compile_bound(binding, R"(
        float evaluate(float a, uint x, float b, uint y, float c, uint z, float d, uint q, float e, uint r) {
            a = a + b * 2.0 + c * 3.0 + d * 4.0 + e * 5.0;
            x = x + y * 10 + z * 100 + q * 1000 + r * 10000;
            if (x == 54321) { return a; }
            return 0.0;
        })", "typed-many.source");
    const std::array arguments{scalar(1.0f),scalar(1u),scalar(2.0f),scalar(2u),scalar(3.0f),scalar(3u),
        scalar(4.0f),scalar(4u),scalar(5.0f),scalar(5u)};
    typed_value(take(execute_fixture(runtime, binding, many.get(), {}, arguments)).value, scalar(55.0f),
        "Mixed source parameters lost order, reassignment or their declared scalar kind.");
    auto nested = compile_bound(binding, R"(
        uint choose(uint value) {
            if (value < 2) { if (value == 0) { return 0; } return 1; }
            while (value > 2) { return 7; }
            return 2;
        })", "typed-nested.source");
    for (const auto input : {0u,1u,2u,3u}) {
        const std::array values{scalar(input)};
        typed_value(take(execute_fixture(runtime, binding, nested.get(), {}, values)).value,
            scalar(input == 3 ? 7u : input), "Nested branch/loop scalar return reached the wrong cleanup/result path.");
    }
    const auto callbacks = typed_trace_services();
    auto checked = compile_bound(binding, "float checked(float value, uint count) { return value; }", "typed-abi.source", callbacks);
    const BusinessScalar sentinel{ScalarKind::u32, 0xa55ac33cu};
    const std::array valid{scalar(1.5f), scalar(2u), scalar(3u)};
    for (unsigned scenario = 0; scenario != 9; ++scenario) {
        auto values = valid;
        const BusinessScalar* input = values.data();
        std::uint32_t count = 2;
        auto output = sentinel;
        auto* storage = &output;
        if (scenario == 0) count = 0;
        if (scenario == 1) count = 1;
        if (scenario == 2) count = 3;
        if (scenario == 3) input = nullptr;
        if (scenario == 4) values[0] = scalar(1u);
        if (scenario == 5) values[1] = scalar(2.0f);
        if (scenario == 6) values[0].kind = ScalarKind::void_value;
        if (scenario == 7) values[1].kind = static_cast<ScalarKind>(99);
        if (scenario == 8) storage = nullptr;
        TypedTrace trace;
        const auto rejected = typed_raw(runtime, binding, checked.get(), {}, input, count, storage, trace);
        require(rejected.status != 0 && trace.begins == 0 && trace.finishes == 0 && rejected.polls == 0,
            "Invalid typed ABI input entered a callback.");
        typed_value(output, sentinel, "Typed ABI rejection changed caller output.");
    }
    {
        TypedTrace trace;
        auto output = sentinel;
        char error[512]{};
        require(atelier_invoke_business(checked.get(), nullptr, valid.data(), 2, &output, error, sizeof(error)) != 0 &&
            atelier_invoke_business(nullptr, &trace, valid.data(), 2, &output, error, sizeof(error)) != 0 &&
            atelier_invoke_business(checked.get(), &trace, valid.data(), 2, &output, nullptr, 0) != 0,
            "Null invocation/code/diagnostic storage was accepted.");
        typed_value(output, sentinel, "Null input rejection changed caller output.");
    }
    for (const bool before_entry : {true, false}) {
        TypedTrace trace;
        trace.fail_begin = before_entry;
        trace.fail_finish = !before_entry;
        auto output = sentinel;
        const auto failed = typed_raw(runtime, binding, checked.get(), {}, valid.data(), 2, &output, trace);
        require(failed.status == (before_entry ? 31u : 37u) && trace.begins == 1 &&
            trace.finishes == (before_entry ? 0u : 1u) &&
            failed.error == (before_entry ? "Typed begin failure." : "Typed finish failure."),
            "Scalar status/first cause was lost at begin or finish.");
        typed_value(output, sentinel, "Begin/finish failure committed staged scalar output.");
    }
    auto nothing = compile_bound(binding, "void nothing() { return; }", "typed-void.source", callbacks);
    for (const bool with_output : {false, true}) {
        TypedTrace trace;
        auto output = sentinel;
        const auto success = typed_raw(runtime, binding, nothing.get(), {}, nullptr, 0, with_output ? &output : nullptr, trace);
        require(success.status == 0 && trace.begins == 1 && trace.finishes == 1, "Void entry rejected optional output.");
        typed_value(output, with_output ? BusinessScalar{} : sentinel, "Void success did not commit its explicit kind and zero bits.");
    }
    std::puts("[pass] Typed scalar nodes: zero/one/ten mixed parameters, reassignment, nested returns, uint wrap, zero/NaN bits, void and pre-callback ABI rejection");
}


void typed_reject_source(const Binding& binding, BusinessCode* existing, std::string_view source) {
    const EnvironmentView storage(binding);
    const std::string type_name(binding.name);
    const auto environment = storage.view(type_name.c_str());
    auto* retained = existing;
    char diagnostic[1024]{};
    const auto status = atelier_compile_business(&services, "typed-invalid.source", source.data(),
        static_cast<std::uint32_t>(source.size()), &environment, &retained, diagnostic, sizeof(diagnostic));
    require(status != 0 && retained == existing, "Typed compile rejection replaced the prior executable.");
    unsigned line{}, column{};
    require(sscanf_s(diagnostic, "typed-invalid.source:%u:%u:", &line, &column) == 2 && line != 0 && column != 0,
        "Typed source rejection lost its source/line/column diagnostic.");
}
void verify_typed_source_errors() {
    Runtime runtime;
    const auto binding = bind_type(runtime, {});
    auto existing = compile_bound(binding, "uint kept() { return 17; }", "typed-kept.source");
    for (const std::string_view source : {
        "\nfloat invalid() {}",
        "\nfloat invalid() { return; }",
        "\nvoid invalid() { return 1; }",
        "\nuint invalid(float value) { return value; }",
        "\nfloat invalid(uint flag) { if (flag == 0) { return 1.0; } }",
        "\nuint invalid() { while (1 == 1) { return 1; } }",
        "\nvoid invalid(bool value) {}",
        "\nvoid invalid(void value) {}",
        "\nvoid invalid(float value, uint value) {}",
        "\nuint invalid() { return 1.0; }",
        "\nvoid invalid() { uint value = 1.0; }"})
        typed_reject_source(binding, existing.get(), source);
    typed_value(take(execute_fixture(runtime, binding, existing.get(), {}, {})).value, scalar(17u),
        "Rejected compilation damaged the old executable.");
    std::puts("[pass] Typed diagnostics: missing/wrong returns, unsupported/duplicate parameters and no implicit conversion preserve positioned errors and prior code");
}
void verify_typed_owned_metadata(Runtime& runtime, const Binding& binding) {
    CodeOwner code{nullptr, &atelier_destroy_business};
    {
        auto temporary = binding;
        std::string type_name(binding.name);
        std::string source_name = "typed-owned.source";
        std::string source = "float owned(float alpha, uint choice) { self.apply_selection(alpha, choice, 1.0, 1); return self.opacity; }";
        auto callbacks = services;
        const EnvironmentView storage(temporary);
        const auto environment = storage.view(type_name.c_str());
        BusinessCode* output{};
        char diagnostic[1024]{};
        require(atelier_compile_business(&callbacks, source_name.c_str(), source.data(),
            static_cast<std::uint32_t>(source.size()), &environment, &output, diagnostic, sizeof(diagnostic)) == 0, diagnostic);
        code.reset(output);
        for (auto& field : temporary.fields) { field.name.assign("destroyed_field"); field.kind = ScalarKind::void_value; }
        for (auto& method : temporary.methods) {
            method.name.assign("destroyed_method");
            for (auto& parameter : method.parameters) parameter.type = ScalarKind::void_value;
        }
        callbacks.begin = nullptr;
        type_name.assign("destroyed_type"); source_name.assign("destroyed_source"); source.assign("destroyed_program");
    }
    const auto info = atelier_business_info(code.get());
    require(info.requires_receiver && std::string_view(info.function_name) == "owned" &&
        std::string_view(info.source_name) == "typed-owned.source" && info.services == services &&
        info.signature.parameter_count == 2 && info.signature.parameters[0].type == ScalarKind::f32 &&
        info.signature.parameters[1].type == ScalarKind::u32 && info.signature.result == ScalarKind::f32 &&
        info.environment.object_type_name && std::string_view(info.environment.object_type_name) == binding.name &&
        same_environment(binding, info.environment), "Typed metadata borrowed destroyed catalog/name/signature storage.");
    auto receiver = take(runtime.create(binding.type));
    const std::array arguments{scalar(0.5f), scalar(7u)};
    typed_value(take(execute_fixture(runtime, binding, code.get(), receiver.id(), arguments)).value, scalar(0.5f),
        "Owned typed metadata no longer supports its original Runtime binding.");
}
void verify_typed_catalog_errors(const Binding& binding) {
    auto existing = compile_bound(binding, "void kept() {}", "typed-catalog-kept.source");
    constexpr std::string_view source = "void candidate() {}";
    for (unsigned scenario = 0; scenario != 14; ++scenario) {
        EnvironmentView storage(binding);
        const std::string type_name(binding.name);
        auto environment = storage.view(type_name.c_str());
        const ScalarKind invalid = static_cast<ScalarKind>(99);
        const BusinessParameter invalid_parameter{invalid};
        if (scenario == 0) environment.fields = nullptr;
        if (scenario == 1) environment.methods = nullptr;
        if (scenario == 2) storage.fields[0].name = nullptr;
        if (scenario == 3) storage.fields[0].name = "";
        if (scenario == 4) storage.fields[0].name = "bad.";
        if (scenario == 5) storage.fields[1].name = storage.fields[0].name;
        if (scenario == 6) storage.fields[0].kind = invalid;
        if (scenario == 7) storage.fields[0].kind = ScalarKind::void_value;
        if (scenario == 8) storage.methods[0].name = nullptr;
        if (scenario == 9) storage.methods[1].name = storage.methods[0].name;
        if (scenario == 10) { storage.methods[0].parameters = nullptr; storage.methods[0].parameter_count = 1; }
        if (scenario == 11) { storage.methods[0].parameters = &invalid_parameter; storage.methods[0].parameter_count = 1; }
        if (scenario == 12) storage.methods[0].name = "";
        if (scenario == 13) environment.object_type_name = "bad.";
        auto* retained = existing.get();
        char diagnostic[1024]{};
        require(atelier_compile_business(&services, "typed-invalid-catalog.source", source.data(),
            static_cast<std::uint32_t>(source.size()), &environment, &retained, diagnostic, sizeof(diagnostic)) != 0 &&
            retained == existing.get() && diagnostic[0] != '\0', "Malformed generic catalog replaced an executable or lacked a diagnostic.");
    }
    for (const std::string_view invalid : {
        "void invalid() { self.reset_focus(1); }",
        "void invalid() { self.apply_selection(1.0, 2, 3.0); }",
        "void invalid() { self.apply_selection(1.0, 2.0, 3.0, 4); }",
        "void invalid() { self.selected = 1.0; }",
        "void invalid() { self.opacity = 1; }",
        "float invalid() { return self.reset_focus(); }"})
        typed_reject_source(binding, existing.get(), invalid);
}
void verify_typed_ui(Runtime& runtime, const Binding& binding) {
    constexpr std::string_view source = R"(
        float paint(float alpha, uint choice, float offset, uint repeats) {
            self.reset_focus();
            self.apply_selection(alpha, choice, offset, repeats);
            self.accumulate(1, 2, 3, 4, 5, 6, 7, 8, 9);
            self.opacity = self.opacity + 0.25;
            self.cursor = self.cursor + 1.0;
            self.revisions = self.revisions + 2;
            self.selected = self.selected + 1;
            return self.opacity * 2.0 + self.cursor;
        })";
    const std::array arguments{scalar(0.5f), scalar(7u), scalar(1.25f), scalar(2u)};
    for (const bool reorder : {false, true}) {
        auto bound = binding;
        if (reorder) { std::reverse(bound.fields.begin(), bound.fields.end()); std::reverse(bound.methods.begin(), bound.methods.end()); }
        auto code = compile_bound(bound, source, "typed-ui.source");
        auto receiver = take(runtime.create(binding.type));
        ExecutionStats stats;
        auto result = take(execute_fixture(runtime, bound, code.get(), receiver.id(), arguments, true, &stats));
        typed_value(result.value, scalar(9.0f), "UI controller mutation/method chain returned the wrong scalar.");
        require(stats.explicit_cleanup && !result.snapshot.registered() && runtime.root_count() == 1 &&
            std::get<float>(take(runtime.read(receiver.id(), fixture_field(binding, "opacity")))) == 1.75f &&
            std::get<float>(take(runtime.read(receiver.id(), fixture_field(binding, "cursor")))) == 5.5f &&
            std::get<std::uint32_t>(take(runtime.read(receiver.id(), fixture_field(binding, "selected")))) == 293u &&
            std::get<std::uint32_t>(take(runtime.read(receiver.id(), fixture_field(binding, "revisions")))) == 5u,
            "Generic UI field/method indices or nine-argument packing changed business state.");
        if (reorder) {
            reject(execute_fixture(runtime, binding, code.get(), receiver.id(), arguments, false, &stats),
                ErrorCode::invalid_definition, "Mismatched binding order was silently executed.");
            require(!stats.entered, "Incompatible binding entered generated code.");
        }
        const auto stale = receiver.id();
        take(runtime.destroy(stale));
        receiver.reset();
        reject(execute_fixture(runtime, bound, code.get(), stale, arguments, false, &stats), ErrorCode::invalid_object,
            "Typed UI entry accepted a destroyed receiver.");
        require(!stats.entered && runtime.root_count() == 0, "Destroyed receiver validation ran source or leaked a root.");
    }
    verify_typed_owned_metadata(runtime, binding);
    collect(runtime);
    require(runtime.root_count() == 0 && runtime.live_objects() == 0, "Typed UI metadata scenario retained an object.");
    verify_typed_catalog_errors(binding);
}


void verify_typed_reference_results(Runtime& runtime, const Binding& binding) {
    const auto source = reference_source(R"(
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
            if (branch == 0) { if (acquired != null) { return acquired.opacity + self.opacity; } }
            return value;
        })", binding.name);
    const auto callbacks = typed_trace_services();
    auto code = compile_bound(binding, source, "typed-reference-return.source", callbacks);
    const auto layout = atelier_business_layout(code.get());
    require(layout.strong_slots > business_root_slots && layout.weak_slots != 0,
        "Typed reference-return proof has no actual strong/weak slots.");
    for (unsigned scenario = 0; scenario != 5; ++scenario) {
        auto receiver = take(runtime.create(binding.type));
        TypedTrace trace;
        trace.fail_poll = scenario == 3;
        trace.fail_finish = scenario == 4;
        const auto branch = scenario == 1 ? 2u : scenario == 2 ? 1u : 0u;
        const std::array arguments{scalar(4.0f), scalar(branch)};
        const BusinessScalar sentinel{ScalarKind::u32, 0xbad00badu};
        auto output = sentinel;
        const auto result = typed_raw(runtime, binding, code.get(), receiver.id(), arguments.data(),
            static_cast<std::uint32_t>(arguments.size()), &output, trace);
        if (scenario < 2) {
            require(result.status == 0 && trace.finishes == 1 && result.polls == 2,
                "Typed nested/final scalar return skipped finish or backedge polling.");
            typed_value(output, scalar(scenario == 0 ? 6.0f : 4.0f), "Typed return lost its rooted scalar expression.");
        } else {
            require(result.status != 0 && trace.finishes == (scenario == 4 ? 1u : 0u) &&
                result.error == (scenario == 2 ? "Typed UI native failure." : scenario == 3 ?
                    "Typed poll failure." : "Typed finish failure."), "Typed reference failure lost its first cause or reached finish.");
            typed_value(output, sentinel, "Native/poll/finish failure committed a staged scalar result.");
        }
        require(trace.begins == 1 && runtime.root_count() == 1 &&
            std::get<float>(take(runtime.read(receiver.id(), fixture_field(binding, "opacity")))) == 2.0f &&
            std::get<std::uint32_t>(take(runtime.read(receiver.id(), fixture_field(binding, "selected")))) == 3u &&
            std::get<std::uint32_t>(take(runtime.read(receiver.id(), fixture_field(binding, "revisions")))) ==
                (scenario == 2 ? 1u : 0u), "Typed failure rolled back partial effects or leaked an invocation root.");
        collect(runtime);
        require(runtime.live_objects() == 1, "Scalar-return cleanup retained a source-created object.");
        receiver.reset();
        collect(runtime);
        require(runtime.root_count() == 0 && runtime.live_objects() == 0, "Typed reference scenario leaked its receiver.");
    }
}
void verify_typed_active_owner() {
    Runtime runtime;
    const auto binding = bind_type(runtime, {});
    for (const bool fail : {false, true}) {
        auto code = compile_bound(binding, "float identity(float value) { return value; }", "typed-active-owner.source");
        const auto address = atelier_business_info(code.get()).entry_address;
        struct State { CodeOwner* owner; std::uintptr_t address; bool fail; unsigned callbacks{}; } state{&code, address, fail};
        const std::array arguments{BusinessScalar{ScalarKind::f32, 0x7fc54321u}};
        ExecutionStats stats;
        auto result = execute_fixture(runtime, binding, code.get(), {}, arguments, false, &stats,
            [](void* context, Invocation&, const CollectionProgress&) {
                auto& state = *static_cast<State*>(context);
                ++state.callbacks;
                state.owner->reset();
                require_code_state(state.address, true);
                if (state.fail) throw std::runtime_error("Typed active-code failure.");
            }, &state);
        require(!code && state.callbacks == 1 && stats.explicit_cleanup && runtime.root_count() == 0,
            "Typed callback retirement lost its active code pin or cleanup.");
        if (fail) {
            reject(result, ErrorCode::invocation_failed, "Typed retiring callback failure escaped JIT cleanup.");
            require(result.error().message == "Typed active-code failure.", "Typed retirement lost its first callback error.");
        } else typed_value(take(std::move(result)).value, arguments[0], "Retiring outer code corrupted the typed result/signature.");
        require_code_state(address, false);
    }
}
void verify_typed_reload(Runtime& runtime, const Binding& binding) {
    const bool receiver_required = binding.type != TypeId{};
    BusinessProgram program(runtime, binding.type);
    const auto entry = program.entry();
    const std::string first_source = receiver_required ?
        "float evaluate(float value, uint choice, float bias) { self.apply_selection(value, choice, 1.0, 1); return self.opacity + bias; }" :
        "float evaluate(float value, uint choice, float bias) { return value + bias; }";
    const std::string second_source = receiver_required ?
        "float evaluate(float value, uint choice, float bias) { self.apply_selection(value * 2.0, choice, 1.0, 1); return self.opacity + bias; }" :
        "float evaluate(float value, uint choice, float bias) { return value * 2.0 + bias; }";
    auto first = take(BusinessProgram::prepare(take(program.request("typed-reload-first.source", first_source))));
    take(program.try_publish(first));
    auto old = take(program.pin());
    ObjectRoot receiver;
    if (receiver_required) receiver = take(runtime.create(binding.type));
    const std::array arguments{scalar(1.0f), scalar(9u), scalar(0.25f)};
    typed_value(take(invoke_fixture(entry, receiver.id(), arguments)).value, scalar(1.25f), "Typed reload initial entry returned the wrong result.");
    auto second = take(BusinessProgram::prepare(take(program.request("typed-reload-second.source", second_source))));
    take(program.try_publish(second));
    const auto current_address = take(program.pin()).address();
    require(current_address != old.address(), "Typed reload reused a still-leased executable address.");
    typed_value(take(invoke_fixture(entry, receiver.id(), arguments)).value, scalar(receiver_required ? 3.25f : 2.25f),
        "Same-signature typed publication did not reach the stable entry.");
    typed_value(take(invoke_fixture(old, receiver.id(), arguments)).value, scalar(receiver_required ? 4.25f : 1.25f),
        "Old typed lease silently redirected to the replacement.");
    for (const std::string_view incompatible : {
        "float evaluate(uint value, uint choice, float bias) { return 0.0; }",
        "float evaluate(uint choice, float value, float bias) { return value; }",
        "uint evaluate(float value, uint choice, float bias) { return 0; }",
        "void evaluate(float value, uint choice, float bias) {}",
        "float evaluate(float value) { return value; }",
        "float evaluate(float value, uint choice, float bias, uint extra) { return value; }"}) {
        auto rejected = BusinessProgram::prepare(take(program.request("typed-reload-incompatible.source", incompatible)));
        reject(rejected, ErrorCode::invalid_definition, "Typed reload accepted a changed parameter/result signature.");
        require(take(program.pin()).address() == current_address, "Rejected typed reload replaced the current executable.");
    }
    typed_value(take(invoke_fixture(entry, receiver.id(), arguments)).value, scalar(receiver_required ? 6.25f : 2.25f),
        "Failed typed reload damaged the last published entry.");
    typed_value(take(invoke_fixture(old, receiver.id(), arguments)).value, scalar(receiver_required ? 7.25f : 1.25f),
        "Signature rejection invalidated the old typed lease.");
    require(runtime.root_count() == (receiver_required ? 1u : 0u), "Typed reload calls retained invocation roots.");
}
void verify_native_result_consumers(Runtime& runtime, const Binding& binding) {
    const auto* info = take(runtime.describe(binding.type));
    const auto bump_id = fixture_method(binding, "bump");
    const auto scale_id = fixture_method(binding, "scale");
    const MethodInfo* bump{};
    const MethodInfo* scale{};
    for (const auto& method : info->methods) {
        if (method.id == bump_id) bump = &method;
        if (method.id == scale_id) scale = &method;
    }
    const auto u32 = take(runtime.native_type<std::uint32_t>());
    const auto f32 = take(runtime.native_type<float>());
    require(bump && scale && bump->signature.receiver && scale->signature.receiver && bump->signature.receiver->type == binding.type &&
        !bump->signature.receiver->read_only && bump->signature.parameters == std::vector<ParameterDescription>{{u32}} &&
        bump->signature.result == ValueDescription{u32} && scale->signature.receiver->read_only &&
        scale->signature.result == ValueDescription{f32}, "Native/described function metadata lost result or receiver semantics.");
    auto receiver = take(runtime.create(binding.type));
    const auto selected = fixture_field(binding, "selected");
    const auto revisions = fixture_field(binding, "revisions");
    const std::array input{Scalar{std::uint32_t{0}}};
    Scalar native_result{std::uint32_t{0xa55ac33c}};
    take(runtime.call(receiver.id(), bump_id, input, &native_result));
    require(native_result == Scalar{std::uint32_t{0}}, "A native zero result was mistaken for failure or void.");
    const std::array nonzero_input{Scalar{std::uint32_t{1}}};
    reject(runtime.call(receiver.id(), bump_id, nonzero_input), ErrorCode::invalid_storage,
        "A nonvoid native call entered without result storage.");
    require(std::get<std::uint32_t>(take(runtime.read(receiver.id(), revisions))) == 0,
        "Missing output storage entered a native method.");
    auto nested = compile_bound(binding,
        "uint nested() { return self.combine(self.first(), self.next(9)); }", "native-nested-results.source");
    ExecutionStats stats;
    typed_value(take(execute_fixture(runtime, binding, nested.get(), receiver.id(), {}, false, &stats)).value,
        scalar(1129u), "Nested native argument evaluation overwrote an outer argument.");
    require(stats.explicit_cleanup && std::get<std::uint32_t>(take(runtime.read(receiver.id(), selected))) == 123,
        "Native receiver/arguments were not evaluated in left-to-right order.");
    auto mixed = compile_bound(binding, R"(
        float mixed(float factor) {
            self.bump(0);
            if (self.bump(0) == 0) { return self.scale(factor) + self.scale(0.5); }
            return 0.0;
        })", "native-mixed-results.source");
    take(runtime.write(receiver.id(), fixture_field(binding, "opacity"), 2.0f));
    const std::array factors{scalar(1.5f)};
    typed_value(take(execute_fixture(runtime, binding, mixed.get(), receiver.id(), factors)).value, scalar(4.0f),
        "Discarded, conditional or float native results lost their declared meaning.");
    take(runtime.write(receiver.id(), selected, std::uint32_t{0}));
    const auto failure_source = reference_source(R"(
        uint failed() {
            $T held = new $T();
            weak $T observed = observe(held);
            return self.combine(self.first(), self.fail_result());
        })", binding.name);
    auto failure = compile_bound(binding, failure_source, "native-result-failure.source", typed_trace_services());
    const BusinessScalar sentinel{ScalarKind::u32, 0x5a5ac33c};
    auto output = sentinel;
    TypedTrace trace;
    const auto failed = typed_raw(runtime, binding, failure.get(), receiver.id(), nullptr, 0, &output, trace);
    require(failed.status != 0 && failed.error == "Typed UI native result failure." && trace.finishes == 0 &&
        runtime.root_count() == 1 && std::get<std::uint32_t>(take(runtime.read(receiver.id(), selected))) == 1 &&
        std::get<std::uint32_t>(take(runtime.read(receiver.id(), revisions))) == 1,
        "Native-result failure lost its first cause, ran the outer call, rolled back effects or retained roots.");
    typed_value(output, sentinel, "Failed native result escaped into the source result.");
    collect(runtime);
    require(runtime.live_objects() == 1, "Failure cleanup retained a source-created result temporary.");
    auto invalid = compile_bound(binding, "uint retained() { return 7; }", "native-result-type-retained.source");
    const EnvironmentView environment_storage(binding);
    const std::string type_name(binding.name);
    const auto environment = environment_storage.view(type_name.c_str());
    for (const std::string_view source : {
        "float invalid() { return self.bump(0); }",
        "uint invalid() { return self.reset_focus(); }",
        "uint invalid() { self.bump(0) = 2; return 0; }",
        "uint invalid() { return self.combine(self.scale(1.0), 2); }"}) {
        auto* retained = invalid.get();
        auto* candidate = retained;
        char error[1024]{};
        const auto status = atelier_compile_business(&services, "native-result-type-error.source", source.data(),
            static_cast<std::uint32_t>(source.size()), &environment, &candidate, error, sizeof(error));
        require(status != 0 && candidate == retained && std::string_view(error).find("native-result-type-error.source") != std::string_view::npos,
            "Invalid native result use replaced valid code or lost source diagnostics.");
    }
}

void verify_native_result_abi(Runtime& runtime, const Binding& binding) {
    auto receiver = take(runtime.create(binding.type));
    enum class Fault { wrong_kind, missing_result, service_failure, none };
    struct Calls { std::uint32_t count{}; Fault fault{}; };
    auto callbacks = services;
    callbacks.call = [](void* context, std::uint32_t slot, std::uint32_t method,
        const BusinessScalar* arguments, std::uint32_t count, BusinessScalar* output) noexcept -> std::uint32_t {
        auto& invocation = *static_cast<Invocation*>(context);
        auto& state = *static_cast<Calls*>(fixture(invocation).hook_state);
        if (++state.count == 2 && state.fault == Fault::missing_result) return 0;
        const auto status = call_method(context, slot, method, arguments, count, output);
        if (status != 0 && state.fault == Fault::service_failure) return 2;
        if (status == 0 && state.count == 2 && state.fault == Fault::wrong_kind && output)
            output->kind = ScalarKind::f32;
        return status;
    };
    const auto source = reference_source(R"(
        uint abi() {
            $T held = new $T();
            weak $T observed = observe(held);
            self.bump(0);
            return self.bump(0);
        })", binding.name);
    auto code = compile_bound(binding, source,
        "native-result-abi.source", callbacks);
    const BusinessScalar sentinel{ScalarKind::u32, 0xcafebabe};
    const auto check_cleanup = [&](const Invocation& invocation) {
        require(invocation.frame.empty() && runtime.root_count() == 1,
            "Native ABI result path relied on RAII root cleanup.");
        for (const auto& observation : invocation.observations)
            require(observation.description().type == TypeId{}, "Native ABI result path retained a weak slot.");
    };
    for (const auto fault : {Fault::wrong_kind, Fault::missing_result}) {
        std::string backend_error;
        {
            Calls calls{0, fault};
            auto activation = take(ExecutionScope::enter(runtime));
            FixtureInvocation observed{false, nullptr, &calls};
            Invocation invocation(runtime, binding, atelier_business_layout(code.get()), {1}, observed.observer());
            take(invocation.frame.hold(receiver_slot, receiver.id()));
            auto output = sentinel;
            char error[512]{};
            const auto status = atelier_invoke_business(code.get(), &invocation, nullptr, 0, &output, error, sizeof(error));
            require(status != 0 && calls.count == 2 && !invocation.failed,
                "Wrong or missing second native result reused scratch or lost its backend failure.");
            check_cleanup(invocation);
            typed_value(output, sentinel, "Native ABI result rejection committed caller output.");
            backend_error = error;
            require(backend_error.find("Native call result contract violation") != std::string::npos,
                "Native ABI result rejection lost its contract-violation diagnostic.");
        }
        Calls calls{0, fault};
        ExecutionStats stats;
        const auto rejected = execute_fixture(runtime, binding, code.get(), receiver.id(), {}, false,
            &stats, nullptr, &calls);
        require(!rejected && rejected.error().code == ErrorCode::invocation_failed &&
            rejected.error().message == backend_error && stats.explicit_cleanup && calls.count == 2,
            "The host wrapper lost the generated result-contract diagnostic or explicit cleanup.");
        collect(runtime);
        require(runtime.live_objects() == 1 && runtime.root_count() == 1,
            "Native ABI result rejection retained source-created objects.");
    }
    {
        Calls calls{0, Fault::none};
        auto activation = take(ExecutionScope::enter(runtime));
        FixtureInvocation observed{false, nullptr, &calls};
        Invocation invocation(runtime, binding, atelier_business_layout(code.get()), {1}, observed.observer());
        take(invocation.frame.hold(receiver_slot, receiver.id()));
        auto output = sentinel;
        char error[512]{"Previous invocation failure."};
        require(atelier_invoke_business(code.get(), &invocation, nullptr, 0, &output, error, sizeof(error)) == 0 &&
            calls.count == 2 && error[0] == '\0', "A prior ABI failure contaminated a later successful invocation.");
        typed_value(output, scalar(0u), "A later successful ABI invocation did not commit its result.");
        check_cleanup(invocation);
    }
    auto native_failure = compile_bound(binding, "uint failed() { return self.fail_result(); }",
        "native-result-status.source", callbacks);
    {
        Calls calls{0, Fault::service_failure};
        auto activation = take(ExecutionScope::enter(runtime));
        FixtureInvocation observed{false, nullptr, &calls};
        Invocation invocation(runtime, binding, atelier_business_layout(native_failure.get()), {1}, observed.observer());
        take(invocation.frame.hold(receiver_slot, receiver.id()));
        auto output = sentinel;
        char error[512]{};
        require(atelier_invoke_business(native_failure.get(), &invocation, nullptr, 0, &output, error, sizeof(error)) == 2 &&
            invocation.failed && std::string_view(invocation.error) == "Typed UI native result failure." && error[0] == '\0',
            "A service status was changed or confused with a generated result-contract failure.");
        typed_value(output, sentinel, "Native service failure committed caller output.");
        check_cleanup(invocation);
    }
    Calls calls{0, Fault::service_failure};
    const auto rejected = execute_fixture(runtime, binding, native_failure.get(), receiver.id(), {}, false,
        nullptr, nullptr, &calls);
    require(!rejected && rejected.error().message == "Typed UI native result failure.",
        "The host wrapper replaced the native service first cause with a backend diagnostic.");
}

void verify_shared_function_versions(Runtime& runtime, const Binding& binding) {
    BusinessProgram program(runtime, binding.type);
    const auto entry = program.entry();
    auto first = take(BusinessProgram::prepare(take(program.request("shared-first.source",
        "uint score(uint amount) { return self.bump(amount); }"))));
    take(program.try_publish(first));
    const auto signature = take(program.describe());
    const auto bump = fixture_method(binding, "bump");
    bool same_signature{};
    for (const auto& method : take(runtime.describe(binding.type))->methods)
        if (method.id == bump) same_signature = signature == method.signature;
    require(same_signature, "Source/native producers did not publish the same resolved function contract.");
    const auto bound = take(program.bind<std::uint32_t, std::uint32_t>());
    reject(program.bind<std::uint32_t, std::uint32_t>(FunctionExecution::coroutine), ErrorCode::argument_mismatch,
        "Coroutine binding accepted a synchronous declaration.");
    require(!program.bind<float, std::uint32_t>() && !program.bind<std::uint32_t, float>(),
        "Host expected types were not checked against the actual source declaration.");
    auto receiver = take(runtime.create(binding.type));
    const std::array arguments{scalar(2u)};
    auto old = take(program.pin());
    typed_value(take(invoke_fixture(bound, receiver.id(), arguments)).value, scalar(2u), "Typed host binding did not invoke the shared declaration.");
    auto replacement = take(BusinessProgram::prepare(take(program.request("shared-second.source",
        "uint score(uint amount) { return self.bump(amount) + 100; }"))));
    take(program.try_publish(replacement));
    typed_value(take(invoke_fixture(entry, receiver.id(), arguments)).value, scalar(104u), "Current entry did not select the new shared-signature body.");
    typed_value(take(invoke_fixture(old, receiver.id(), arguments)).value, scalar(6u), "Old lease changed its function body after publication.");
    auto without_self = take(BusinessProgram::prepare(take(program.request("shared-unused-self.source",
        "uint score(uint amount) { return amount; }"))));
    take(program.try_publish(without_self));
    require(take(program.describe()) == signature, "Unused self silently weakened the published receiver contract.");
    reject(invoke_fixture(entry, {}, arguments), ErrorCode::invalid_object, "A replacement removed a required receiver from the caller contract.");
    typed_value(take(invoke_fixture(entry, receiver.id(), arguments)).value, scalar(2u), "An unused declared receiver could not be provided.");
    BusinessProgram free_function(runtime, binding.type);
    auto independent = take(BusinessProgram::prepare(take(free_function.request("shared-free.source",
        "uint free_value(uint amount) { return amount; }"))));
    take(free_function.try_publish(independent));
    require(!take(free_function.describe()).receiver, "An object catalog was confused with a required receiver.");
    auto changed_receiver = BusinessProgram::prepare(take(free_function.request("shared-added-self.source",
        "uint free_value(uint amount) { return self.bump(amount); }")));
    require(!changed_receiver, "A free source contract silently acquired a required receiver.");
    typed_value(take(invoke_fixture(free_function.entry(), {}, arguments)).value, scalar(2u), "Rejected receiver change damaged the current free function.");
}

void verify_versioned_call_scratch(Runtime& runtime, const Binding& binding) {
    struct Scratch { std::size_t expanded{}; std::size_t begins{}; };
    auto callbacks = services;
    callbacks.begin = [](void* context) noexcept -> std::uint32_t {
        auto& invocation = *static_cast<Invocation*>(context);
        auto& observed = *static_cast<Scratch*>(fixture(invocation).hook_state);
        observed.expanded = invocation.expanded_arguments.size();
        ++observed.begins;
        return begin(context);
    };
    const std::array<std::string_view, 3> sources{
        "uint demand() { return self.selected; }",
        "uint demand() { self.accumulate(1,2,3,4,5,6,7,8,9); return self.selected; }",
        "uint demand() { return self.bump(0); }"};
    const std::array<std::uint32_t, 3> demands{0, 9, 1};
    auto receiver = take(runtime.create(binding.type));
    for (std::size_t index = 0; index < sources.size(); ++index) {
        auto code = compile_bound(binding, sources[index], "actual-call-demand.source", callbacks);
        require(atelier_business_layout(code.get()).max_call_arguments == demands[index],
            "Code layout used catalog maximum instead of actual native call demand.");
        Scratch observed;
        take(execute_fixture(runtime, binding, code.get(), receiver.id(), {}, false, nullptr, nullptr, &observed));
        require(observed.begins == 1 && observed.expanded == (index == 1 ? 9u : 0u),
            "Unused nine-argument methods still allocated an expanded invocation buffer.");
    }
    BusinessProgram program(runtime, binding.type);
    const auto entry = program.entry();
    std::array<BusinessProgram::Lease, 3> retained;
    for (std::size_t index = 0; index < sources.size(); ++index) {
        auto candidate = take(BusinessProgram::prepare(take(program.request("version-call-demand.source", sources[index]))));
        take(program.try_publish(candidate));
        retained[index] = take(program.pin());
        require(ProgramAccess::layout(retained[index]).max_call_arguments == demands[index], "New version retained stale call-storage requirements.");
        take(runtime.write(receiver.id(), fixture_field(binding, "selected"), std::uint32_t{0}));
        typed_value(take(invoke_fixture(entry, receiver.id(), {})).value, scalar(index == 1 ? 285u : 0u),
            "Version-specific scratch could not execute its actual native call demand.");
    }
    for (std::size_t index = 0; index < retained.size(); ++index) {
        require(ProgramAccess::layout(retained[index]).max_call_arguments == demands[index], "A current publication altered an older lease's layout.");
        take(runtime.write(receiver.id(), fixture_field(binding, "selected"), std::uint32_t{0}));
        typed_value(take(invoke_fixture(retained[index], receiver.id(), {})).value, scalar(index == 1 ? 285u : 0u),
            "An older lease used the current version's native call storage.");
    }
}

void verify_typed_business() {
    verify_typed_scalars();
    verify_typed_source_errors();
    verify_typed_active_owner();
    {
        Runtime runtime;
        const auto binding = bind_type(runtime, {});
        verify_typed_reload(runtime, binding);
        BusinessProgram zero(runtime, {});
        const auto entry = zero.entry();
        for (const auto expected : {0u, 7u}) {
            const auto source = "uint sample() { return " + std::to_string(expected) + "; }";
            auto candidate = take(BusinessProgram::prepare(take(zero.request("typed-zero-reload.source", source))));
            take(zero.try_publish(candidate));
            typed_value(take(invoke_fixture(entry, {}, {})).value, scalar(expected), "Zero-argument typed reload changed its empty signature or result.");
        }
        require(runtime.root_count() == 0 && runtime.live_objects() == 0, "Pure scalar reload created host objects.");
    }
    Runtime runtime;
    const auto types = register_typed_ui(runtime);
    for (const auto type : types) {
        const auto binding = bind_type(runtime, type);
        verify_typed_ui(runtime, binding);
        verify_typed_reference_results(runtime, binding);
        verify_typed_reload(runtime, binding);
        verify_native_result_consumers(runtime, binding);
        verify_native_result_abi(runtime, binding);
        verify_shared_function_versions(runtime, binding);
        verify_versioned_call_scratch(runtime, binding);
        collect(runtime);
        require(runtime.root_count() == 0 && runtime.live_objects() == 0, "Typed UI consumer retained state after teardown.");
    }
    std::puts("[pass] Typed UI consumers: native/described controllers, generic fields, zero/mixed/nine-argument methods, reordered catalogs and destroyed-receiver rejection");
    std::puts("[pass] Typed ownership and failures: deep-owned schema/signatures, active owner retirement, scalar strong/weak returns, native/poll/finish first-cause and explicit cleanup");
    std::puts("[pass] Typed reload: pure scalar and both UI representations accept matching signatures, reject parameter/result changes and retain old leases");
    std::puts("[pass] Shared function contracts: inferred native/source results, readonly receiver metadata, nested argument SSA, discarded results and first-cause cleanup");
    std::puts("[pass] Native result ABI violations retain diagnostics through explicit cleanup and host propagation without changing service status or later success");
    std::puts("[pass] Bound source declarations: host signature checking, fixed receiver requirements, old leases and per-version actual native-call scratch demand");
}

