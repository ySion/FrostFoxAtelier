// Included after Version: a project candidate owns every entry until one
// publication swaps the whole set. None of these objects owns a second Runtime.
struct Program::ModuleRequest {
    struct Unit {
        std::shared_ptr<const Contract> contract;

        language::MappedSource source;

        Array<std::string> names;

        std::string owner; // the class, "" for the project unit, or "engine/<module>"

        Array<Array<std::string>> parameter_names;

        Array<Array<MetadataList>> parameter_metadata;

        bool receiver{};
    };

    std::shared_ptr<const SourceSnapshot> snapshot;

    CompileOptions options;

    language::SourceModule source;

    // Per class, the resolved field definitions that a published class must keep.
    Array<Array<FieldDefinition>> class_fields;

    Array<ClassInfo> classes;

    Array<Unit> units;
};

struct Program::ModuleVersion {
    std::shared_ptr<const ModuleRequest> request;

    Array<ClassInfo> classes;

    Array<EntryInfo> entries;

    Array<FunctionInfo> functions;

    Array<std::shared_ptr<const Version>> versions;
};
