namespace {
Error source_error(const SourceSnapshot& snapshot, std::uint32_t file, language::SourcePosition position,
    std::string message, ErrorCode code = ErrorCode::invalid_definition) {
    Error error{code, std::move(message)};
    if (file < snapshot.files.size()) {
        const auto& unit = snapshot.files[file];
        error.source = SourceLocation{unit.path, unit.version, position.offset, position.line, position.column};
    }
    return error;
}
Error mapped_error(const SourceSnapshot& snapshot, const language::MappedSource& source,
    language::SourcePosition position, std::string message) {
    for (const auto& piece : source.pieces) if (position.offset >= piece.begin && position.offset <= piece.end) {
        const auto offset = piece.offset + std::min(position.offset, piece.end) - piece.begin;
        language::SourcePosition original{offset, 1, 1};
        const auto& text = snapshot.files[piece.file].source;
        for (std::size_t i = 0; i < offset; ++i)
            if (text[i] == '\n') { ++original.line; original.column = 1; } else ++original.column;
        return source_error(snapshot, piece.file, original, std::move(message));
    }
    return source_error(snapshot, 0, {}, std::move(message));
}
Result<Scalar> field_default(const SourceSnapshot& snapshot, const language::ClassDeclaration& owner, const language::ClassField& field) {
    if (field.initializer.text.empty()) {
        if (field.type == "bool") return Scalar{false};
        if (field.type == "float") return Scalar{0.0f};
        if (field.type == "double") return Scalar{0.0};
        if (field.type == "int") return Scalar{std::int32_t{}};
        return Scalar{std::uint32_t{}};
    }
    language::MappedSource text;
    text.text = "const " + field.type + " __source_default = "; text.append(field.initializer);
    text.text += "; " + field.type + " __source_value() { return __source_default; }";
    auto parsed = language::compile_source(snapshot.name, text.text, {}, "__source_value");
    if (!parsed) return std::unexpected(mapped_error(snapshot, text, parsed.error().position, parsed.error().message));
    const auto& function = parsed->functions[parsed->entry];
    const auto& value = function.expressions[function.statements[function.statements[function.body].children[0]].expression];
    if (value.kind != language::Expression::Kind::literal)
        return std::unexpected(source_error(snapshot, owner.file, field.position, "Field defaults require a constant scalar expression."));
    if (field.type == "bool") return Scalar{value.bits != 0};
    if (field.type == "float") return Scalar{std::bit_cast<float>(static_cast<std::uint32_t>(value.bits))};
    if (field.type == "double") return Scalar{std::bit_cast<double>(value.bits)};
    if (field.type == "int") return Scalar{std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value.bits))};
    return Scalar{static_cast<std::uint32_t>(value.bits)};
}
bool same_class_layout(const language::ClassDeclaration& a, const language::ClassDeclaration& b) {
    if (a.name != b.name || a.base != b.base || a.fields.size() != b.fields.size() || a.methods.size() != b.methods.size()) return false;
    for (std::size_t i = 0; i < a.fields.size(); ++i)
        if (a.fields[i].name != b.fields[i].name || a.fields[i].type != b.fields[i].type) return false;
    for (const auto& method : a.methods) {
        const auto found = std::ranges::find(b.methods, method.name, &language::ClassMethod::name);
        if (found == b.methods.end() || found->signature != method.signature) return false;
    }
    return true;
}
}

std::shared_ptr<const SourceSnapshot> Program::SourceRequest::snapshot() const { return module_ ? module_->snapshot : nullptr; }
std::span<const ClassInfo> Program::classes() const noexcept {
    return state_->module ? std::span<const ClassInfo>{state_->module->classes} : std::span<const ClassInfo>{};
}
std::span<const EntryInfo> Program::entries() const noexcept {
    return state_->module ? std::span<const EntryInfo>{state_->module->entries} : std::span<const EntryInfo>{};
}
Result<Program::Entry> Program::entry(std::string_view name) const {
    if (!state_->open) return std::unexpected(Error{ErrorCode::invalid_definition, "Business program is closed."});
    const auto index = state_->find(name);
    if (!index) return std::unexpected(Error{ErrorCode::invalid_member, "Unknown published source entry: " + std::string(name)});
    return Entry{state_, index};
}
Result<Program::Lease> Program::pin(std::string_view name) const {
    if (!state_->open) return std::unexpected(Error{ErrorCode::invalid_definition, "Business program is closed."});
    auto index = state_->find(name);
    auto version = index ? state_->select(index) : nullptr;
    if (!version) return std::unexpected(Error{ErrorCode::invalid_member, "Unknown published source entry: " + std::string(name)});
    return Lease{std::move(version)};
}
Result<FunctionSignature> Program::describe(std::string_view name) const {
    if (!state_->open) return std::unexpected(Error{ErrorCode::invalid_definition, "Business program is closed."});
    auto index = state_->find(name);
    auto version = index ? state_->select(index) : nullptr;
    if (!version) return std::unexpected(Error{ErrorCode::invalid_member, "Unknown published source entry: " + std::string(name)});
    return version->declaration->signature;
}

Result<Program::Request> Program::request(SourceSnapshot snapshot, CompileOptions options) {
    if (!state_->open || state_->declaration || state_->contract->binding.type != TypeId{})
        return std::unexpected(Error{ErrorCode::invalid_definition, "Project snapshots require an open, unbound Program."});
    if (state_->latest_request == std::numeric_limits<std::uint64_t>::max())
        return std::unexpected(Error{ErrorCode::capacity_exceeded, "Source request sequence exhausted."});
    auto request = std::shared_ptr<SourceRequest>{new SourceRequest(state_, state_->contract, {},
        ++state_->latest_request, snapshot.name, {}, {}, options)};
    request->module_ = std::make_shared<ModuleRequest>();
    auto& module = *request->module_;
    module.snapshot = std::make_shared<const SourceSnapshot>(std::move(snapshot));
    const auto& input = *module.snapshot;
    const auto fail = [&](Error error) -> Result<Request> { request->declaration_error_ = std::move(error); return request; };
    std::size_t bytes = 0;
    for (const auto& file : input.files) {
        if (file.source.size() > 32 * 1024 * 1024 - bytes)
            return fail({ErrorCode::capacity_exceeded, "Source snapshot exceeds 32 MiB."});
        bytes += file.source.size();
    }
    auto collected = language::collect_module(input);
    if (!collected) return fail(source_error(input, collected.error().file, collected.error().diagnostic.position, collected.error().diagnostic.message));
    module.source = std::move(*collected);
    auto values = language::collect_declarations(module.source.common.text);
    if (!values) return fail(mapped_error(input, module.source.common, values.error().position, values.error().message));
    module.value_types = std::move(*values);
    if (state_->module) {
        const auto& old = *state_->module->request;
        if (old.source.functions.size() != module.source.functions.size())
            return fail({ErrorCode::rebuild_required, "Source export set changed; rebuild the Runtime context."});
        for (const auto& function : module.source.functions) {
            const auto previous = std::ranges::find(old.source.functions, function.name, &language::FunctionDeclaration::name);
            if (previous == old.source.functions.end() || previous->signature != function.signature)
                return fail({ErrorCode::rebuild_required, "Source export signature changed; rebuild the Runtime context: " + function.name});
        }
        if (old.source.classes.size() != module.source.classes.size() || old.value_types.size() != module.value_types.size() ||
            !std::equal(old.value_types.begin(), old.value_types.end(), module.value_types.begin(),
                [](const auto& a, const auto& b) { return a.same_definition(b); }) ||
            !std::equal(old.source.classes.begin(), old.source.classes.end(), module.source.classes.begin(), same_class_layout))
            return fail({ErrorCode::rebuild_required, "Source type layout or method signature changed; rebuild the Runtime context."});
    }
    auto definitions = type_definitions(module.value_types, state_->contract->binding);
    if (!definitions) return fail(definitions.error());
    auto& runtime = *state_->contract->binding.runtime;
    for (const auto& cls : module.source.classes) {
        if (std::ranges::any_of(state_->contract->binding.values, [&](const auto& value) {
            return value.math && cls.name == math::short_name(value.math);
        })) return fail(source_error(input, cls.file, cls.position, "Source class conflicts with a builtin math name: " + cls.name));
        const auto base = runtime.find_type(cls.base);
        if (!base) return fail(source_error(input, cls.file, cls.position, "Unknown native base type: " + cls.base));
        const auto info = runtime.describe(*base);
        if (!info || (*info)->kind != TypeKind::object || !(*info)->native_storage)
            return fail(source_error(input, cls.file, cls.position, "Source classes require an existing native object base."));
        TypeDefinition definition; definition.kind = TypeKind::object; definition.name = cls.name; definition.base = *base;
        std::vector<std::optional<Scalar>> defaults;
        for (const auto& field : cls.fields) {
            auto value = field_default(input, cls, field);
            if (!value) return fail(value.error());
            TypeId type;
            if (field.type == "float") type = state_->contract->binding.f32;
            else if (field.type == "double") type = state_->contract->binding.f64;
            else if (field.type == "int") type = state_->contract->binding.i32;
            else if (field.type == "uint") type = state_->contract->binding.u32;
            else type = state_->contract->binding.boolean;
            FieldDefinition declared; declared.name = field.name; declared.value = {type}; declared.default_value = *value;
            definition.fields.push_back(std::move(declared)); defaults.emplace_back(std::move(*value));
        }
        module.defaults.push_back(std::move(defaults)); definitions->push_back(std::move(definition));
    }
    if (state_->module) {
        const auto& old = *state_->module->request;
        if (!std::equal(old.defaults.begin(), old.defaults.end(), module.defaults.begin(), [](const auto& a, const auto& b) {
            return std::equal(a.begin(), a.end(), b.begin(), same_default);
        }))
            return fail({ErrorCode::rebuild_required, "Source type layout or defaults changed; rebuild the Runtime context."});
    } else if (!definitions->empty()) {
        auto publication = atelier::detail::TypePublication::prepare(runtime, std::move(*definitions));
        if (!publication) return fail(publication.error());
        request->type_publication_ = *publication;
    }
    const auto catalog = request->type_publication_ ? request->type_publication_->catalog() : std::span<const TypeInfo* const>{};
    request->compilation_contract_ = std::make_shared<const Contract>(runtime, TypeId{}, catalog);
    if (!module.source.functions.empty()) {
        ModuleRequest::Unit unit{request->compilation_contract_, {}, {}, {}};
        for (const auto& function : module.source.functions) unit.names.push_back(function.name);
        module.units.push_back(std::move(unit));
    }
    for (const auto& cls : module.source.classes) {
        const TypeInfo* info{};
        if (catalog.empty()) { auto found = runtime.find_type(cls.name); if (found) info = take(runtime.describe(*found)); }
        else for (const auto* candidate : catalog) if (candidate->name == cls.name) { info = candidate; break; }
        if (!info || !info->base) return fail(source_error(input, cls.file, cls.position, "Source class has no prepared Runtime layout."));
        module.classes.push_back({cls.name, info->id, *info->base});
        if (cls.methods.empty()) continue;
        ModuleRequest::Unit unit{std::make_shared<const Contract>(runtime, info->id, catalog), {}, {}, cls.name};
        for (const auto& method : cls.methods) { unit.source.append(method.source); unit.names.push_back(method.name); }
        module.units.push_back(std::move(unit));
    }
    // An empty export set still uses the same parser to check all declarations.
    if (module.units.empty()) module.units.push_back({request->compilation_contract_, {}, {}, {}});
    return request;
}

Result<Program::Candidate> Program::prepare_module(const Request& request) {
    const auto owner = request->owner_.lock();
    if (!owner || !owner->open) return std::unexpected(Error{ErrorCode::invalid_definition, "Project program is closed."});
    auto module = std::make_shared<ModuleVersion>(); module->request = request->module_; module->classes = request->module_->classes;
    // One line index per original file and one location table per compiled unit.
    // Export views share it, while completed captures own no project snapshot.
    const auto& snapshot = *request->module_->snapshot;
    std::vector<std::vector<std::size_t>> line_starts(snapshot.files.size());
    if (request->options.diagnostics || request->options.debugging) {
        for (std::size_t file = 0; file < snapshot.files.size(); ++file) {
            auto& lines = line_starts[file]; lines.push_back(0);
            const auto& source = snapshot.files[file].source;
            for (std::size_t i = 0; i < source.size(); ++i) if (source[i] == '\n') lines.push_back(i + 1);
        }
    }
    for (const auto& unit : request->module_->units) {
        auto text = request->module_->source.common;
        text.append(unit.source);
        const EnvironmentView storage(unit.contract->binding);
        const auto environment = storage.view(unit.contract->type_name.c_str());
        std::vector<const char*> names;
        for (const auto& name : unit.names) names.push_back(name.c_str());
        std::vector<BusinessCode*> output(names.size());
        std::vector<CodeOwner> codes;
        codes.reserve(names.size());
        CompileLocation location{}; char diagnostic[2048]{};
        const auto status = atelier_compile_business_entries(&unit.contract->actual_services, request->source_name.c_str(),
            text.text.data(), static_cast<std::uint32_t>(text.text.size()), &environment,
            names.data(), static_cast<std::uint32_t>(names.size()), !unit.owner.empty(), output.data(), &location, diagnostic, sizeof(diagnostic), request->options);
        for (auto* code : output) codes.emplace_back(code, &atelier_destroy_business);
        if (status) return std::unexpected(mapped_error(*request->module_->snapshot, text,
            {static_cast<std::size_t>(location.offset), static_cast<std::size_t>(location.line), static_cast<std::size_t>(location.column)}, diagnostic));
        auto locations = std::make_shared<std::vector<DiagnosticLocation>>();
        std::shared_ptr<const DebugTables> debug_tables;
        if (!codes.empty()) {
            const auto info = atelier_business_info(codes.front().get());
            debug_tables = copy_debug_tables(info);
            locations->reserve(info.location_count);
            for (std::uint32_t index = 0; index < info.location_count; ++index) {
                const auto& position = info.locations[index];
                SourceLocation mapped{request->source_name, request->sequence,
                    static_cast<std::size_t>(position.offset), static_cast<std::size_t>(position.line), static_cast<std::size_t>(position.column)};
                for (const auto& piece : text.pieces) {
                    if (position.offset < piece.begin || position.offset >= piece.end) continue;
                    const auto& file = snapshot.files[piece.file];
                    const auto offset = std::min(file.source.size(), piece.offset + static_cast<std::size_t>(position.offset) - piece.begin);
                    const auto& lines = line_starts[piece.file];
                    const auto line = std::upper_bound(lines.begin(), lines.end(), offset);
                    const auto line_index = static_cast<std::size_t>(line - lines.begin() - 1);
                    mapped = {file.path, file.version, offset, line_index + 1, offset - lines[line_index] + 1};
                    break;
                }
                locations->push_back({position.function ? position.function : "", std::move(mapped)});
            }
        }
        for (std::size_t i = 0; i < codes.size(); ++i) {
            auto compatible = unit.contract->compatible(codes[i].get(), request->source_name);
            if (!compatible) return std::unexpected(compatible.error());
            auto info = atelier_business_info(codes[i].get());
            auto signature = unit.contract->resolve_signature(info, request->type_publication_.get());
            if (!signature) return std::unexpected(signature.error());
            const auto& method = unit.names[i];
            if (!unit.owner.empty()) {
                const bool tick = method == "tick";
                if ((tick || method == "begin" || method == "end") &&
                    (signature->result || signature->parameters.size() != (tick ? 1u : 0u) ||
                        (tick && (signature->parameters[0].type != unit.contract->binding.f32 ||
                            signature->parameters[0].passing != ParameterPassing::value))))
                    return std::unexpected(Error{owner->module ? ErrorCode::rebuild_required : ErrorCode::invalid_definition,
                        "Lifecycle methods require void begin(), void tick(float), or void end()."});
            }
            const auto name = unit.owner.empty() ? method : unit.owner + "." + method;
            module->entries.push_back({name, *signature});
            auto declaration = std::make_shared<const Declaration>(Declaration{info.function_name, std::move(*signature)});
            auto metadata = std::make_shared<const DiagnosticMetadata>(DiagnosticMetadata{owner->id, request->sequence,
                name, request->source_name, locations});
            auto debug = make_debug_metadata(*metadata, info, debug_tables);
            module->versions.push_back(std::make_shared<const Version>(unit.contract, std::move(declaration), request->sequence,
                std::move(metadata), std::move(debug), std::move(codes[i])));
        }
    }
    if (owner->module) {
        const auto& old = owner->module->entries;
        if (old.size() != module->entries.size())
            return std::unexpected(Error{ErrorCode::rebuild_required, "Source export set changed; rebuild the Runtime context."});
        // Preserve the first publication's slots, even when declarations move.
        // Bound Entry calls can then select their version without name lookup.
        for (std::size_t i = 0; i < old.size(); ++i) {
            const auto found = std::ranges::find(module->entries, old[i].name, &EntryInfo::name);
            if (found == module->entries.end() || found->signature != old[i].signature)
                return std::unexpected(Error{ErrorCode::rebuild_required, "Source export signature changed; rebuild the Runtime context: " + old[i].name});
            const auto index = static_cast<std::size_t>(found - module->entries.begin());
            std::swap(module->entries[i], module->entries[index]);
            std::swap(module->versions[i], module->versions[index]);
        }
    }
    auto prepared = std::make_unique<Prepared>(request->owner_, request->contract_, request->compilation_contract_,
        request->type_publication_, request->sequence, CodeOwner{nullptr, &atelier_destroy_business}, request->source_name, std::move(module));
    return Candidate{std::move(prepared)};
}

Result<void> Program::publish_module(Candidate& candidate) {
    auto& prepared = *candidate.prepared_;
    if (prepared.owner.lock() != state_ || prepared.contract != state_->contract)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Candidate belongs to another Program contract."});
    if (prepared.sequence != state_->latest_request || prepared.sequence <= state_->published_request)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Source snapshot candidate is stale or already published."});
    auto ready = state_->contract->binding.runtime->check_code_publication(); if (!ready) return ready;
    auto unchanged = state_->contract->unchanged(); if (!unchanged) return unchanged;
    if (prepared.type_publication) {
        ready = prepared.type_publication->reserve_commit(*state_->contract->binding.runtime); if (!ready) return ready;
        prepared.type_publication->commit(*state_->contract->binding.runtime);
    }
    state_->contract = prepared.compilation_contract;
    state_->published_request = prepared.sequence;
    state_->module = std::move(prepared.module);
    return {};
}
