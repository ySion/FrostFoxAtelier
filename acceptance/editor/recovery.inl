void verify_scene_recovery() {
    namespace fs=std::filesystem;
    const auto evidence=fs::absolute("recovery-evidence")/(std::to_string(GetCurrentProcessId())+"-"+
        std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
    auto project=take(editor::Project::create(evidence/"Project"));
    const auto original=take(project.resolve(project.startup_scene()));
    const auto original_bytes=take(assets::read_file(original));
    editor::Session document;
    changed(document.create_static_mesh(take(project.assets().mesh("builtin/cube"))));
    take(document.rename(document.selection().entity,L"Recovered entity"));
    const auto entity=document.selection().entity;
    const auto committed=take(document.serialize());
    const auto history=document.undo_count();
    take(document.begin_edit(entity,document.parameters().front().path,801));
    take(document.preview_edit(801,17.f));
    require(document.editing(),"Recovery fixture did not enter a real edit.");

    application::SceneRecovery recovery;
    const auto& result=recovery.preserve(document,project.root(),project.startup_scene(),"renderer execution failed",evidence/"Recovery");
    require(result && *result,"Dirty author scene was not preserved.");
    const auto path=result->value().scene;
    require(take(assets::read_file(path))==committed && document.dirty() && !document.editing() && document.undo_count()==history,
        "Recovery saved a tentative gesture, changed history, or marked the original saved.");
    auto reopened=take(editor::Session::deserialize(take(assets::read_file(path)),project));
    require(take(reopened->world().members()).size()==1 && reopened->name(take(reopened->world().members()).front().id())==L"Recovered entity",
        "Recovered scene did not reopen through the real scene loader.");
    const auto metadata=assets::json_format::parse(take(assets::read_file(path.parent_path()/"recovery.json")));
    require(metadata.at("reason")=="renderer execution failed" && metadata.at("unfinished_edit_cancelled")==true &&
        metadata.at("assets_validated")==false,"Recovery origin or failure reason was lost.");
    const auto& repeated=recovery.preserve(document,project.root(),project.startup_scene(),"cleanup failed",evidence/"Other");
    require(repeated && *repeated && repeated->value().scene==path && !fs::exists(evidence/"Other") &&
        assets::json_format::parse(take(assets::read_file(path.parent_path()/"recovery.json")))==metadata,
        "Repeated failure rewrote the snapshot or replaced its first cause.");

    editor::Session missing;
    const auto mesh=take(project.assets().mesh("builtin/cube"));
    changed(missing.create_static_mesh(mesh));
    const auto missing_root=component(missing,missing.selection().entity,missing.presentation_types().static_mesh_component);
    const std::string missing_key="project/{00000000-0000-0000-0000-000000000099}";
    take(presentation::set_mesh_binding(missing.world().runtime(),missing_root,{missing_key,mesh.mesh}));
    application::SceneRecovery missing_recovery;
    const auto& missing_result=missing_recovery.preserve(missing,project.root(),{},"missing asset",evidence/"Recovery");
    require(missing_result && *missing_result && missing_result->value().scene!=path,"Missing asset blocked raw scene preservation.");
    const auto missing_bytes=take(assets::read_file(missing_result->value().scene));
    require(missing_bytes.find(missing_key)!=std::string::npos && !editor::Session::deserialize(missing_bytes,project,{},editor::LoadPolicy::strict),
        "Recovery silently removed a missing asset reference or claimed a complete project.");
    const auto tolerated=take(editor::Session::deserialize(missing_bytes,project));
    require(std::ranges::any_of(tolerated->load_issues(),[](const auto& item){return item.kind==world::DiagnosticKind::missing_resource;}) &&
        take(tolerated->serialize()).find(missing_key)!=std::string::npos,"Tolerant recovery loading lost or hid the missing asset reference.");

    application::SceneRecovery clean_recovery;
    reopened->mark_saved();
    const auto& clean=clean_recovery.preserve(*reopened,project.root(),{},"clean failure",evidence/"Clean");
    require(clean && !*clean && !fs::exists(evidence/"Clean"),"Clean document created a recovery artifact.");
    take(editor::write_file(evidence/"blocked","sentinel"));
    application::SceneRecovery denied_recovery;
    const auto& denied=denied_recovery.preserve(document,project.root(),{},"disk failure",evidence/"blocked");
    require(!denied && document.dirty() && take(assets::read_file(evidence/"blocked"))=="sentinel",
        "Recovery IO failure was hidden or replaced existing data.");
    const auto error=denied.error().message;
    const auto& again=denied_recovery.preserve(document,project.root(),{},"repeat",evidence/"Unexpected");
    require(!again && again.error().message==error && !fs::exists(evidence/"Unexpected"),"Failed recovery retried during cleanup.");
    require(take(assets::read_file(original))==original_bytes,"Recovery overwrote the original scene.");
    take(document.close());take(missing.close());take(reopened->close());
    std::puts("Scene recovery acceptance: PASS (committed author state, atomic file/reopen, missing assets, idempotence, IO failure, original preserved)");
}
