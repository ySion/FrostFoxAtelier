// Included after Version: a project candidate owns every entry until one
// publication swaps the whole set. None of these objects owns a second Runtime.
struct Program::ModuleRequest {
    struct Unit {
        std::shared_ptr<const Contract> contract;
        language::MappedSource source;
        std::vector<std::string> names;
        std::string owner; // the class, "" for the project unit, or "engine/<module>"
        std::vector<std::vector<std::string>> parameter_names;
        bool receiver{};
    };
    std::shared_ptr<const SourceSnapshot> snapshot;
    CompileOptions options;
    language::SourceModule source;
    std::vector<language::TypeDeclaration> value_types;
    // Per class, the resolved field definitions that a published class must keep.
    std::vector<std::vector<FieldDefinition>> class_fields;
    std::vector<ClassInfo> classes;
    std::vector<Unit> units;
};
struct Program::ModuleVersion {
    std::shared_ptr<const ModuleRequest> request;
    std::vector<ClassInfo> classes;
    std::vector<EntryInfo> entries;
    std::vector<std::shared_ptr<const Version>> versions;
};
