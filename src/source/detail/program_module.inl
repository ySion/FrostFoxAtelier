namespace {
Error source_error(const SourceSnapshot& snapshot, std::uint32_t file, language::SourcePosition position,
                   std::string message, ErrorCode code = ErrorCode::invalid_definition) {
    Error error{code, std::move(message)};
    if (file < snapshot.files.size()) {
        const auto& unit = snapshot.files[file];
        error.source =
            SourceLocation{unit.path, unit.version, position.offset, position.line, position.column, unit.disk_path};
    }
    return error;
}

Error mapped_error(const SourceSnapshot& snapshot, const language::MappedSource& source,
                   language::SourcePosition position, std::string message) {
    for (const auto& piece : source.pieces)
        if (position.offset >= piece.begin && position.offset <= piece.end) {
            const auto offset = piece.offset + std::min(position.offset, piece.end) - piece.begin;
            language::SourcePosition original{offset, 1, 1};
            const auto& text = snapshot.files[piece.file].source;
            for (std::size_t i = 0; i < offset; ++i)
                if (text[i] == '\n') {
                    ++original.line;
                    original.column = 1;
                } else
                    ++original.column;
            return source_error(snapshot, piece.file, original, std::move(message));
        }
    return source_error(snapshot, 0, {}, std::move(message));
}

// Folds one constant scalar expression with the typed Source parser; floating results must be finite.
Result<Scalar> fold_scalar(const SourceSnapshot& snapshot, const language::MappedSource& expression,
                           std::string_view type) {
    language::MappedSource text;
    text.text = "const " + std::string(type) + " __source_default = ";
    text.append(expression);
    text.text += "; " + std::string(type) + " __source_value() { return __source_default; }";
    auto parsed = language::compile_source(snapshot.name, text.text, {}, "__source_value");
    if (!parsed) {
        // A position in the generated wrapper, such as the ';' after `1.0 +`, reports at the expression.
        const auto offset = parsed.error().position.offset;
        const bool inside = std::ranges::any_of(
            text.pieces, [&](const auto& piece) { return offset >= piece.begin && offset <= piece.end; });
        return std::unexpected(inside ? mapped_error(snapshot, text, parsed.error().position, parsed.error().message)
                                      : mapped_error(snapshot, expression, {}, parsed.error().message));
    }
    const auto& function = parsed->functions[parsed->entry];
    const auto& value =
        function.expressions[function.statements[function.statements[function.body].children[0]].expression];
    const auto at = [&](std::string message) {
        return std::unexpected(mapped_error(snapshot, expression, {}, std::move(message)));
    };
    if (value.kind != language::Expression::Kind::literal)
        return at("Field defaults and bounds require constant scalar expressions.");
    if (type == "bool")
        return Scalar{value.bits != 0};
    if (type == "int")
        return Scalar{std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value.bits))};
    if (type == "uint")
        return Scalar{static_cast<std::uint32_t>(value.bits)};
    const auto number = type == "float" ? double{std::bit_cast<float>(static_cast<std::uint32_t>(value.bits))}
                                        : std::bit_cast<double>(value.bits);
    if (!std::isfinite(number))
        return at("Field defaults and bounds must be finite.");
    return type == "float" ? Scalar{std::bit_cast<float>(static_cast<std::uint32_t>(value.bits))} : Scalar{number};
}

Scalar zero_leaf(std::string_view type) {
    if (type == "bool")
        return false;
    if (type == "float")
        return 0.0f;
    if (type == "double")
        return 0.0;
    if (type == "int")
        return std::int32_t{};
    return std::uint32_t{};
}

// Published classes keep their layout, defaults and methods; new methods may follow.
bool keeps_class(const language::ClassDeclaration& published, const language::ClassDeclaration& updated) {
    if (published.base != updated.base || published.fields.size() != updated.fields.size())
        return false;
    for (std::size_t i = 0; i < published.fields.size(); ++i)
        if (published.fields[i].name != updated.fields[i].name || published.fields[i].type != updated.fields[i].type)
            return false;
    for (const auto& method : published.methods) {
        const auto found = std::ranges::find(updated.methods, method.name, &language::ClassMethod::name);
        if (found == updated.methods.end() || found->signature != method.signature)
            return false;
    }
    return true;
}

template <class T>
const T* find_named(const std::vector<T>& values, std::string_view name) {
    const auto found = std::ranges::find(values, name, &T::name);
    return found == values.end() ? nullptr : &*found;
}

// A class field's Runtime definition. Defaults and bounds fold against the field's leaf type: each
// component of a vector; an enum default names one item and becomes its underlying integer.
Result<FieldDefinition> class_field(const SourceSnapshot& snapshot, const language::ClassDeclaration& owner,
                                    const language::ClassField& field,
                                    const std::vector<language::TypeDeclaration>& declarations, const Binding& binding,
                                    Runtime& runtime) {
    const auto at = [&](language::SourcePosition position, std::string message) {
        return std::unexpected(source_error(snapshot, owner.file, position, std::move(message)));
    };
    const auto unsupported = [&] {
        return at(
            field.position,
            "Source object fields require bool, int, uint, float, double, a float or double vector, or an enum: " +
                field.type);
    };
    FieldDefinition result;
    result.name = field.name;
    std::string_view leaf; // empty for an enum
    const BoundValue* vector{};
    const std::vector<std::pair<std::string, std::uint32_t>>* items{};
    bool signed_items{};
    const auto names_vector = [&](std::string_view name) {
        return vector && (name == vector->name || name == math::short_name(vector->math));
    };
    if (field.type == "bool" || field.type == "int" || field.type == "uint" || field.type == "float" ||
        field.type == "double") {
        leaf = field.type;
        result.value = {leaf == "bool"    ? binding.boolean
                        : leaf == "int"   ? binding.i32
                        : leaf == "uint"  ? binding.u32
                        : leaf == "float" ? binding.f32
                                          : binding.f64};
    } else if (const auto* declared = find_named(declarations, field.type)) {
        if (!declared->enumeration)
            return unsupported();
        result.value = {TypeReference{field.type}};
        items = &declared->enumerators;
        signed_items = declared->signed_enum;
    } else if (const auto found = std::ranges::find_if(
                   binding.values, [&](const BoundValue& value) { return value.math && field.type == value.name; });
               found != binding.values.end()) {
        if (!found->math.vector())
            return at(field.position, "Source object fields cannot hold quaternions or matrices: " + field.type);
        vector = &*found;
        result.value = {vector->type};
        leaf = vector->math.precision == math::Precision::f64 ? "double" : "float";
    } else if (const auto type = runtime.find_type(field.type); type && !language::editor_reserved(field.type)) {
        const auto* info = take(runtime.describe(*type));
        if (!info->enumeration)
            return unsupported();
        result.value = {*type};
        items = &info->enumeration->values;
        signed_items = info->enumeration->underlying == binding.i32;
    } else
        return unsupported();
    const auto& initializer = field.initializer;
    if (items) {
        std::uint32_t bits = 0;
        if (!initializer.text.empty()) {
            const auto item =
                std::ranges::find(*items, field.initializer_item, &std::pair<std::string, std::uint32_t>::first);
            if (field.initializer_item.empty() || field.initializer_name != field.type || item == items->end())
                return std::unexpected(mapped_error(snapshot, initializer, {},
                                                    "Enum field defaults name one item: " + field.type + "::item."));
            bits = item->second;
        }
        result.default_leaves = {signed_items ? Scalar{std::bit_cast<std::int32_t>(bits)} : Scalar{bits}};
    } else if (vector) {
        const auto components = vector->math.components();
        if (!initializer.text.empty()) {
            const auto& arguments = field.initializer_arguments;
            if (!field.initializer_item.empty() || !names_vector(field.initializer_name) ||
                (!arguments.empty() && arguments.size() != components))
                return std::unexpected(mapped_error(snapshot, initializer, {},
                                                    "Vector field defaults are " + field.type + "() or " + field.type +
                                                        " with one constant per component."));
            for (const auto& argument : arguments) {
                auto value = fold_scalar(snapshot, argument, leaf);
                if (!value)
                    return std::unexpected(value.error());
                result.default_leaves.push_back(*value);
            }
        }
        if (result.default_leaves.empty())
            result.default_leaves.assign(components, zero_leaf(leaf));
    } else {
        auto value =
            initializer.text.empty() ? Result<Scalar>{zero_leaf(leaf)} : fold_scalar(snapshot, initializer, leaf);
        if (!value)
            return std::unexpected(value.error());
        result.default_leaves = {*value};
    }
    if (field.range) {
        if (leaf.empty() || leaf == "bool")
            return at(*field.range,
                      "Field ranges require an int, uint, float or double field, or a float or double vector.");
        const std::array sides{&result.range.ui_min, &result.range.ui_max, &result.range.hard_min,
                               &result.range.hard_max};
        for (std::size_t side = 0; side < sides.size(); ++side) {
            if (field.bounds[side].text.empty())
                continue;
            auto bound = fold_scalar(snapshot, field.bounds[side], leaf);
            if (!bound)
                return std::unexpected(bound.error());
            *sides[side] = *bound;
        }
    }
    result.label = field.label;
    result.category = field.category;
    result.description = field.description;
    return result;
}

// A published field keeps its exact default leaves, bounds and presentation text; source spacing is not part of them.
bool same_field(const FieldDefinition& a, const FieldDefinition& b) {
    const auto same = [](const std::optional<Scalar>& x, const std::optional<Scalar>& y) {
        return x && y ? same_scalar(*x, *y) : x.has_value() == y.has_value();
    };
    return same_leaves(a.default_leaves, b.default_leaves) && same(a.range.ui_min, b.range.ui_min) &&
           same(a.range.ui_max, b.range.ui_max) && same(a.range.hard_min, b.range.hard_min) &&
           same(a.range.hard_max, b.range.hard_max) && a.label == b.label && a.category == b.category &&
           a.description == b.description;
}
}

std::shared_ptr<const SourceSnapshot> Program::SourceRequest::snapshot() const {
    return module_ ? module_->snapshot : nullptr;
}

std::span<const ClassInfo> Program::classes() const noexcept {
    return state_->module ? std::span<const ClassInfo>{state_->module->classes} : std::span<const ClassInfo>{};
}

std::span<const EntryInfo> Program::entries() const noexcept {
    return state_->module ? std::span<const EntryInfo>{state_->module->entries} : std::span<const EntryInfo>{};
}

std::uint64_t Program::revision() const noexcept {
    return state_->revision;
}

Result<Program::Entry> Program::entry(std::string_view name) const {
    if (!state_->open)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Business program is closed."});
    const auto index = state_->find(name);
    if (!index)
        return std::unexpected(
            Error{ErrorCode::invalid_member, "Unknown published source entry: " + std::string(name)});
    return Entry{state_, index};
}

Result<Program::Lease> Program::pin(std::string_view name) const {
    if (!state_->open)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Business program is closed."});
    auto index = state_->find(name);
    auto version = index ? state_->select(index) : nullptr;
    if (!version)
        return std::unexpected(
            Error{ErrorCode::invalid_member, "Unknown published source entry: " + std::string(name)});
    return Lease{std::move(version)};
}

Result<FunctionSignature> Program::describe(std::string_view name) const {
    if (!state_->open)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Business program is closed."});
    auto index = state_->find(name);
    auto version = index ? state_->select(index) : nullptr;
    if (!version)
        return std::unexpected(
            Error{ErrorCode::invalid_member, "Unknown published source entry: " + std::string(name)});
    return version->declaration->signature;
}

Result<Program::Request> Program::request(SourceSnapshot snapshot, CompileOptions options) {
    if (!state_->open || state_->declaration || state_->contract->binding.type != TypeId{})
        return std::unexpected(
            Error{ErrorCode::invalid_definition, "Project snapshots require an open, unbound Program."});
    if (state_->latest_request == std::numeric_limits<std::uint64_t>::max())
        return std::unexpected(Error{ErrorCode::capacity_exceeded, "Source request sequence exhausted."});
    auto request = std::shared_ptr<SourceRequest>{
        new SourceRequest(state_, state_->contract, {}, ++state_->latest_request, snapshot.name, {}, {}, options)};
    request->module_ = std::make_shared<ModuleRequest>();
    auto& module = *request->module_;
    module.snapshot = std::make_shared<const SourceSnapshot>(std::move(snapshot));
    module.options = options;
    const auto& input = *module.snapshot;
    const auto fail = [&](Error error) -> Result<Request> {
        request->declaration_error_ = std::move(error);
        return request;
    };
    std::size_t bytes = 0;
    for (const auto& file : input.files) {
        if (file.source.size() > 32 * 1024 * 1024 - bytes)
            return fail({ErrorCode::capacity_exceeded, "Source snapshot exceeds 32 MiB."});
        bytes += file.source.size();
    }
    auto collected = language::collect_module(input);
    if (!collected)
        return fail(source_error(input, collected.error().file, collected.error().diagnostic.position,
                                 collected.error().diagnostic.message));
    module.source = std::move(*collected);
    // A math short name and its canonical name are one type, so a published field keeps it under either spelling.
    for (auto& cls : module.source.classes)
        for (auto& field : cls.fields)
            for (const auto& value : state_->contract->binding.values)
                if (value.math && field.type == math::short_name(value.math))
                    field.type = value.name;
    language::MappedSource types;
    for (const auto& declaration : module.source.declarations)
        if (declaration.kind == language::TopLevel::Kind::type)
            types.append(declaration.source);
    auto values = language::collect_declarations(types.text);
    if (!values)
        return fail(mapped_error(input, types, values.error().position, values.error().message));
    module.value_types = std::move(*values);
    auto& runtime = *state_->contract->binding.runtime;
    // Functions, constants and accessor projections share names with the complete type catalog,
    // including empty classes that produce no bound compile unit.
    const auto type_named = [&](std::string_view name) {
        return (!language::editor_reserved(name) && runtime.find_type(name)) ||
               find_named(module.source.classes, name) || find_named(module.value_types, name);
    };
    for (const auto& declaration : module.source.declarations) {
        if (declaration.kind == language::TopLevel::Kind::type)
            continue;
        const std::string_view name = declaration.name;
        if (type_named(name))
            return fail(
                source_error(input, declaration.file, declaration.position,
                             "A source function or constant cannot share a name with a type: " + declaration.name));
        if (!declaration.property.empty() && type_named(declaration.property))
            return fail(source_error(input, declaration.file, declaration.position,
                                     "A source property cannot share a name with a type: " + declaration.property));
        if (const auto dot = name.rfind('.'); dot != std::string_view::npos && type_named(name.substr(0, dot)))
            return fail(
                source_error(input, declaration.file, declaration.position,
                             "A source function or constant name cannot extend a type name: " + declaration.name));
    }
    // A published module only grows: deleting, renaming or changing a published
    // export, type or class needs a rebuild; new ones publish compatibly.
    const ModuleRequest* published = state_->module ? state_->module->request.get() : nullptr;
    if (published) {
        for (const auto& function : published->source.functions) {
            const auto* found = find_named(module.source.functions, function.name);
            if (!found || found->signature != function.signature)
                return fail(
                    {ErrorCode::rebuild_required,
                     "Published source export was removed or changed; rebuild the Runtime context: " + function.name});
        }
        for (const auto& type : published->value_types) {
            const auto* found = find_named(module.value_types, type.name);
            if (!found || !found->same_definition(type))
                return fail(
                    {ErrorCode::rebuild_required,
                     "Published source type was removed or changed; rebuild the Runtime context: " + type.name});
        }
        for (const auto& cls : published->source.classes) {
            const auto* found = find_named(module.source.classes, cls.name);
            if (!found || !keeps_class(cls, *found))
                return fail(
                    {ErrorCode::rebuild_required,
                     "Published source class layout or method signature changed; rebuild the Runtime context: " +
                         cls.name});
        }
    }
    auto definitions = type_definitions(module.value_types, state_->contract->binding);
    if (!definitions)
        return fail(definitions.error());
    // Existing source types stay published; new ones still reference them by name.
    if (published)
        std::erase_if(*definitions,
                      [&](const auto& definition) { return find_named(published->value_types, definition.name); });
    for (const auto& cls : module.source.classes) {
        if (std::ranges::any_of(state_->contract->binding.values, [&](const auto& value) {
                return value.math && cls.name == math::short_name(value.math);
            }))
            return fail(source_error(input, cls.file, cls.position,
                                     "Source class conflicts with a builtin math name: " + cls.name));
        // Hidden editor. types resolve as if they were not registered.
        const auto base = runtime.find_type(cls.base);
        if (!base || language::editor_reserved(cls.base))
            return fail(source_error(input, cls.file, cls.position, "Unknown native base type: " + cls.base));
        const auto info = runtime.describe(*base);
        if (!info || (*info)->kind != TypeKind::object || !(*info)->native_storage)
            return fail(
                source_error(input, cls.file, cls.position, "Source classes require an existing native object base."));
        TypeDefinition definition;
        definition.kind = TypeKind::object;
        definition.name = cls.name;
        definition.base = *base;
        std::vector<FieldDefinition> fields;
        for (const auto& field : cls.fields) {
            auto declared = class_field(input, cls, field, module.value_types, state_->contract->binding, runtime);
            if (!declared)
                return fail(declared.error());
            fields.push_back(std::move(*declared));
        }
        const auto* kept = published ? find_named(published->source.classes, cls.name) : nullptr;
        if (kept) {
            const auto& before =
                published->class_fields[static_cast<std::size_t>(kept - published->source.classes.data())];
            for (std::size_t i = 0; i < fields.size(); ++i)
                if (!same_field(before[i], fields[i]))
                    return fail({ErrorCode::rebuild_required,
                                 "Published source class field changed its default, range or presentation; "
                                 "rebuild the Runtime context: " +
                                     cls.name + "." + fields[i].name});
        } else {
            definition.fields = fields;
            definitions->push_back(std::move(definition));
        }
        module.class_fields.push_back(std::move(fields));
    }
    if (!definitions->empty()) {
        auto publication = atelier::detail::TypePublication::prepare(runtime, std::move(*definitions));
        if (!publication) {
            // Runtime field rules end with `Class.field`; report them at that field.
            const auto& error = publication.error();
            for (const auto& cls : module.source.classes)
                for (const auto& field : cls.fields)
                    if (error.message.ends_with(": " + cls.name + "." + field.name))
                        return fail(source_error(input, cls.file, field.position, error.message, error.code));
            return fail(error);
        }
        request->type_publication_ = *publication;
    }
    const auto catalog =
        request->type_publication_ ? request->type_publication_->catalog() : std::span<const TypeInfo* const>{};
    request->compilation_contract_ = std::make_shared<const Contract>(runtime, TypeId{}, catalog);
    // A unit compiles its own text with the closure of the declarations it spells. Every top-level
    // declaration is checked in its home unit: the project's, whose functions are the global
    // entries, or its engine module's, which only checks.
    const auto home = [&](std::string_view engine) {
        std::vector<std::uint32_t> roots;
        for (std::uint32_t i = 0; i < module.source.declarations.size(); ++i)
            if (module.source.declarations[i].engine == engine)
                roots.push_back(i);
        return roots;
    };
    if (const auto roots = home({}); !roots.empty()) {
        ModuleRequest::Unit unit{request->compilation_contract_, module.source.closure(roots), {}, {}, {}, false};
        for (const auto& function : module.source.functions) {
            unit.names.push_back(function.name);
            unit.parameter_names.push_back(function.parameters);
        }
        module.units.push_back(std::move(unit));
    }
    std::vector<std::string_view> engines;
    for (const auto& declaration : module.source.declarations)
        if (!declaration.engine.empty() && std::ranges::find(engines, declaration.engine) == engines.end())
            engines.push_back(declaration.engine);
    for (const auto engine : engines)
        module.units.push_back({request->compilation_contract_,
                                module.source.closure(home(engine)),
                                {},
                                "engine/" + std::string(engine),
                                {},
                                false});
    for (const auto& cls : module.source.classes) {
        const TypeInfo* info{};
        if (catalog.empty()) {
            auto found = runtime.find_type(cls.name);
            if (found)
                info = take(runtime.describe(*found));
        } else
            for (const auto* candidate : catalog)
                if (candidate->name == cls.name) {
                    info = candidate;
                    break;
                }
        if (!info || !info->base)
            return fail(source_error(input, cls.file, cls.position, "Source class has no prepared Runtime layout."));
        module.classes.push_back({cls.name, info->id, *info->base});
        if (cls.methods.empty())
            continue;
        ModuleRequest::Unit unit{std::make_shared<const Contract>(runtime, info->id, catalog),
                                 module.source.closure(cls.uses),
                                 {},
                                 cls.name,
                                 {},
                                 true};
        for (const auto& method : cls.methods) {
            unit.source.append(method.source);
            unit.names.push_back(method.name);
            unit.parameter_names.push_back(method.parameters);
        }
        module.units.push_back(std::move(unit));
    }
    return request;
}

Result<Program::Candidate> Program::prepare_module(const Request& request) {
    const auto owner = request->owner_.lock();
    if (!owner || !owner->open)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Project program is closed."});
    auto module = std::make_shared<ModuleVersion>();
    module->request = request->module_;
    module->classes = request->module_->classes;
    // Diagnostic and debug builds resolve every site once, with one line index per
    // file; otherwise a Version maps only its failing site. Export views share the
    // unit tables, while completed captures own no project snapshot.
    const auto& snapshot = *request->module_->snapshot;
    std::vector<std::vector<std::size_t>> line_starts(snapshot.files.size());
    if (request->options.diagnostics || request->options.debugging) {
        for (std::size_t file = 0; file < snapshot.files.size(); ++file) {
            auto& lines = line_starts[file];
            lines.push_back(0);
            const auto& source = snapshot.files[file].source;
            for (std::size_t i = 0; i < source.size(); ++i)
                if (source[i] == '\n')
                    lines.push_back(i + 1);
        }
    }
    for (const auto& unit : request->module_->units) {
        const auto& text = unit.source;
        // A unit whose input matches the published unit keeps its machine code and entry codes without
        // being checked or compiled again; its Versions are rebuilt for this snapshot and sequence.
        const ModuleVersion* kept = nullptr;
        if (owner->module) {
            const auto& old = *owner->module->request;
            const auto found = std::ranges::find(old.units, unit.owner, &ModuleRequest::Unit::owner);
            // The unit text holds every declaration it compiles; a declaration moving between an engine and a
            // project file keeps the text but changes the exports, so the names are compared as well.
            if (found != old.units.end() && found->source.text == unit.source.text && found->names == unit.names &&
                old.snapshot->name == snapshot.name && old.options == request->module_->options &&
                found->contract->binding == unit.contract->binding)
                kept = owner->module.get();
        }
        const auto entry_name = [&](const std::string& method) {
            return unit.receiver ? unit.owner + "." + method : method;
        };
        const auto kept_index = [&](const std::string& name) {
            return static_cast<std::size_t>(std::ranges::find(kept->entries, name, &EntryInfo::name) -
                                            kept->entries.begin());
        };
        std::vector<std::shared_ptr<BusinessCode>> codes;
        codes.reserve(unit.names.size());
        if (kept) {
            for (const auto& method : unit.names)
                codes.push_back(kept->versions[kept_index(entry_name(method))]->code);
        } else {
            const EnvironmentView storage(unit.contract->binding);
            const auto environment = storage.view(unit.contract->type_name.c_str());
            std::vector<const char*> names;
            for (const auto& name : unit.names)
                names.push_back(name.c_str());
            std::vector<BusinessCode*> output(names.size());
            std::vector<CodeOwner> owned;
            owned.reserve(names.size());
            CompileLocation location{};
            char diagnostic[2048]{};
            const auto status = atelier_compile_business_entries(
                &unit.contract->actual_services, request->source_name.c_str(), text.text.data(),
                static_cast<std::uint32_t>(text.text.size()), &environment, names.data(),
                static_cast<std::uint32_t>(names.size()), unit.receiver, output.data(), &location, diagnostic,
                sizeof(diagnostic), request->options);
            for (auto* code : output)
                owned.emplace_back(code, &atelier_destroy_business);
            if (status)
                return std::unexpected(
                    mapped_error(*request->module_->snapshot, text,
                                 {static_cast<std::size_t>(location.offset), static_cast<std::size_t>(location.line),
                                  static_cast<std::size_t>(location.column)},
                                 diagnostic));
            for (auto& code : owned)
                codes.push_back(std::move(code));
        }
        auto map = std::make_shared<const SourceMap>(SourceMap{request->module_->snapshot, text.pieces});
        std::shared_ptr<const std::vector<DiagnosticLocation>> locations;
        std::shared_ptr<const DebugTables> debug_tables;
        if (!codes.empty()) {
            const auto info = atelier_business_info(codes.front().get());
            debug_tables = copy_debug_tables(info);
            locations = diagnostic_locations(info, map.get(), request->source_name, request->sequence, &line_starts);
        }
        for (std::size_t i = 0; i < codes.size(); ++i) {
            auto compatible = unit.contract->compatible(codes[i].get(), request->source_name);
            if (!compatible)
                return std::unexpected(compatible.error());
            auto info = atelier_business_info(codes[i].get());
            auto signature = unit.contract->resolve_signature(info, request->type_publication_.get());
            if (!signature)
                return std::unexpected(signature.error());
            const auto& method = unit.names[i];
            if (unit.receiver) {
                const bool tick = method == "tick" || method == "post_physics_tick" || method == "late_tick";
                if ((tick || method == "begin" || method == "end") &&
                    (signature->result || signature->parameters.size() != (tick ? 1u : 0u) ||
                     (tick && (signature->parameters[0].type != unit.contract->binding.f32 ||
                               signature->parameters[0].passing != ParameterPassing::value))))
                    return std::unexpected(Error{
                        ErrorCode::invalid_definition,
                        "Lifecycle methods require void begin(), void tick(float), void post_physics_tick(float), "
                        "void late_tick(float), or void end()."});
            }
            const auto name = entry_name(method);
            const auto code =
                kept ? kept->entries[kept_index(name)].code : next_entry_code.fetch_add(1, std::memory_order_relaxed);
            module->entries.push_back({name, *signature, unit.parameter_names[i], code});
            auto declaration =
                std::make_shared<const Declaration>(Declaration{info.function_name, std::move(*signature)});
            auto metadata = std::make_shared<const DiagnosticMetadata>(
                DiagnosticMetadata{owner->id, request->sequence, name, request->source_name, locations});
            auto debug = make_debug_metadata(*metadata, info, debug_tables);
            module->versions.push_back(std::make_shared<const Version>(unit.contract, std::move(declaration),
                                                                       request->sequence, std::move(metadata),
                                                                       std::move(debug), std::move(codes[i]), map));
        }
    }
    if (owner->module) {
        const auto& old = owner->module->entries;
        // Preserve published slots, even when declarations move, and append new
        // entries after them. Bound Entry calls select versions without name lookup.
        for (std::size_t i = 0; i < old.size(); ++i) {
            const auto found = std::ranges::find(module->entries, old[i].name, &EntryInfo::name);
            if (found == module->entries.end() || found->signature != old[i].signature)
                return std::unexpected(
                    Error{ErrorCode::rebuild_required,
                          "Source export signature changed; rebuild the Runtime context: " + old[i].name});
            const auto index = static_cast<std::size_t>(found - module->entries.begin());
            std::swap(module->entries[i], module->entries[index]);
            std::swap(module->versions[i], module->versions[index]);
        }
    }
    auto prepared = std::make_unique<Prepared>(
        request->owner_, request->contract_, request->compilation_contract_, request->type_publication_,
        request->sequence, CodeOwner{nullptr, &atelier_destroy_business}, request->source_name, std::move(module));
    return Candidate{std::move(prepared)};
}

Result<void> Program::publish_module(Candidate& candidate) {
    auto& prepared = *candidate.prepared_;
    if (prepared.owner.lock() != state_ || prepared.contract != state_->contract)
        return std::unexpected(Error{ErrorCode::invalid_definition, "Candidate belongs to another Program contract."});
    if (prepared.sequence != state_->latest_request || prepared.sequence <= state_->published_request)
        return std::unexpected(
            Error{ErrorCode::invalid_definition, "Source snapshot candidate is stale or already published."});
    auto ready = state_->contract->binding.runtime->check_code_publication();
    if (!ready)
        return ready;
    auto unchanged = state_->contract->unchanged();
    if (!unchanged)
        return unchanged;
    if (prepared.type_publication) {
        ready = prepared.type_publication->reserve_commit(*state_->contract->binding.runtime);
        if (!ready)
            return ready;
        prepared.type_publication->commit(*state_->contract->binding.runtime);
    }
    state_->contract = prepared.compilation_contract;
    state_->published_request = prepared.sequence;
    ++state_->revision;
    state_->module = std::move(prepared.module);
    return {};
}
