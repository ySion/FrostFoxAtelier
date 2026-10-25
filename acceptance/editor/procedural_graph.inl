// Procedural graph files: canonical version 1 text, reopening in another Runtime, and tolerant loading.
struct GraphMeshes final : procedural::Resources {
    geometry::MeshRef cube=geometry::cube_mesh();
    Result<geometry::MeshRef> mesh(StringView key) const override {
        if(key.std_view()!="fixture/cube") return std::unexpected(Error{ErrorCode::invalid_member,"No mesh "+std::string{key.std_view()}+"."});
        return cube;
    }
    std::uint64_t revision() const noexcept override {return 1;}
};
bool same_output(const procedural::Geometry* a,const procedural::Geometry* b) {
    require(a && b,"A graph produced no output.");
    const auto& x=a->instance_transforms();const auto& y=b->instance_transforms();
    return x.size()==y.size() && x.size()>0 && std::memcmp(x.data(),y.data(),x.size()*sizeof(math::Matrix4))==0 &&
        std::ranges::equal(a->references(),b->references());
}
class ThrowingNode : public procedural::Node {
public:
    ThrowingNode() {throw std::runtime_error("This node cannot be constructed.");}
    void evaluate(procedural::Geometry&) const {}
};
void verify_procedural_graph() {
    using namespace procedural;
    using Json=nlohmann::json;
    GraphMeshes meshes;
    Runtime runtime;take(register_types(runtime));
    editor::GraphDocument document;document.graph=take(runtime.create(take(runtime.native_type<Graph>())));
    const auto graph=document.graph.id();
    const auto add=[&](const char* name,TypeId type,std::array<float,2> position) {
        auto root=take(add_node(runtime,graph,type));const auto id=root.id();
        document.nodes.push_back({name,std::move(root),position});return id;
    };
    const auto grid=add("grid",take(runtime.native_type<Grid>()),{0,0}),moved=add("moved",take(runtime.native_type<Transform>()),{200,0});
    const auto line=add("line",take(runtime.native_type<Line>()),{0,120}),merged=add("merged",take(runtime.native_type<Merge>()),{400,60});
    const auto copied=add("copied",take(runtime.native_type<CopyToPoints>()),{600,60}),promote=add("promote",take(runtime.native_type<Promote>()),{100.5f,-40.25f});
    take(runtime.with_native<Graph>(graph,[](Graph& value)->Result<void> {value.seed=7;return {};}));
    take(runtime.with_native<Grid>(grid,[](Grid& node)->Result<void> {node.size={3,2};node.rows=3;node.columns=4;return {};}));
    take(runtime.with_native<Transform>(moved,[](Transform& node)->Result<void> {
        node.translate={1,0,0};node.rotate={0,.70710677f,0,.70710677f};node.scale={2,1,1};return {};
    }));
    take(runtime.with_native<Line>(line,[](Line& node)->Result<void> {node.end={0,2,0};node.count=3;return {};}));
    take(runtime.with_native<CopyToPoints>(copied,[](CopyToPoints& node)->Result<void> {node.meshes.add(String{"fixture/cube"});return {};}));
    take(runtime.with_native<Promote>(promote,[](Promote& node)->Result<void> {node.name=String{"Cd"};node.to=Layer::primitive;node.reduce=Reduce::sum;return {};}));
    take(connect(runtime,graph,grid,0,moved,0));take(connect(runtime,graph,moved,0,merged,0));
    take(connect(runtime,graph,line,0,merged,0));take(connect(runtime,graph,merged,0,copied,0));
    take(set_display(runtime,graph,copied));
    Evaluator first{runtime,document.graph,meshes};take(first.evaluate());

    // Saving reads the graph without stamping it; the text is canonical JSON with every parameter.
    const auto cursor=take(runtime.change_cursor());
    const auto text=take(editor::save_graph(runtime,document));
    std::vector<ObjectId> written;runtime.changed_objects(cursor,written);
    const auto saved=Json::parse(text);
    require(written.empty() && text==saved.dump(2)+"\n","Saving a graph stamped it, or its text is not canonical.");
    const auto& nodes=saved.at("nodes");
    require(saved.at("version")==1 && saved.at("settings")==Json{{"seed",7},{"display","copied"}} && nodes.size()==6,
        "The graph settings or node list were not saved.");
    require(nodes.at("merged").at("inputs")==Json{{"inputs",Json::array({"moved.output","line.output"})}} &&
        nodes.at("moved").at("inputs")==Json{{"input","grid.output"}} && nodes.at("grid").at("inputs")==Json::object() &&
        nodes.at("promote").at("position")==Json::array({100.5,-40.25}),
        "Connections were not saved by port and output name in connection order, or a position was lost.");
    // Scene properties write the shortest decimal that restores the float, not its widened double.
    require(nodes.at("promote").at("type")=="procedural.Promote" &&
        nodes.at("promote").at("properties")==Json{{"name","Cd"},{"to","primitive"},{"reduce","sum"}} &&
        nodes.at("copied").at("properties").at("meshes")==Json::array({"fixture/cube"}) &&
        nodes.at("moved").at("properties").at("rotate")==Json{{"x",0.0},{"y",.70710677},{"z",0.0},{"w",.70710677}} &&
        !nodes.at("merged").at("properties").contains("inputs"),
        "Node parameters were not saved as Scene properties, or the hidden connection list leaked into them.");

    // Another Runtime reopens the same graph: the same output, and the same bytes again.
    {
        Runtime other;take(register_types(other));
        auto reopened=take(editor::load_graph(other,text));
        require(reopened.issues.empty() && take(editor::save_graph(other,reopened))==text,"A reopened graph did not save the same bytes.");
        for(const auto& node:document.nodes) {
            const auto found=std::ranges::find(reopened.nodes,node.name,&editor::GraphNode::name);
            require(found!=reopened.nodes.end() && found->node.registered() && found->position==node.position,"A reopened node lost its name or position.");
        }
        Evaluator second{other,reopened.graph,meshes};take(second.evaluate());
        require(same_output(first.output(),second.output()),"A reopened graph evaluated to a different output.");
    }

    // Tolerant loading keeps what this Runtime cannot apply and writes it back unchanged.
    auto damaged=saved;
    damaged["nodes"]["mystery"]={{"type","game.Missing"},{"position",{10.5,20.0}},{"properties",{{"x",1}}},{"inputs",{{"source","grid.output"}}}};
    damaged["nodes"]["merged"]["inputs"]["inputs"]=Json::array({"moved.output","mystery.output","line.output"});
    damaged["nodes"]["copied"]["inputs"]["surface"]="grid.output";
    damaged["nodes"]["grid"]["properties"]["rows"]="many";
    damaged["nodes"]["moved"]["properties"]["shear"]=1;
    const auto damaged_text=damaged.dump(2)+"\n";
    {
        Runtime other;take(register_types(other));
        auto tolerant=take(editor::load_graph(other,damaged_text));
        using enum world::DiagnosticKind;
        const auto issue=[&](world::DiagnosticKind kind,std::vector<std::string> field) {
            return std::ranges::count_if(tolerant.issues,[&](const world::Diagnostic& item){return item.kind==kind && item.field==field;})==1;
        };
        require(tolerant.issues.size()==5 && issue(unknown_type,{"nodes","mystery"}) && issue(mismatch,{"nodes","grid","properties","rows"}) &&
            issue(unknown_field,{"nodes","moved","properties","shear"}) && issue(reference_dropped,{"nodes","merged","inputs","inputs"}) &&
            issue(reference_dropped,{"nodes","copied","inputs","surface"}),
            "Tolerant loading did not report each unknown node, unfit leaf and unresolved connection once.");
        const auto& merged_entry=*std::ranges::find(tolerant.nodes,std::string{"merged"},&editor::GraphNode::name);
        const auto& mystery=*std::ranges::find(tolerant.nodes,std::string{"mystery"},&editor::GraphNode::name);
        require(!mystery.node.registered() && mystery.position==std::array<float,2>{10.5f,20} && take(inputs(other,merged_entry.node.id())).size()==2,
            "An unknown node was loaded, or the resolvable connections of a partly resolved port were not made.");
        require(take(editor::save_graph(other,tolerant))==damaged_text,"Kept nodes, leaves and connections were not written back unchanged.");
        // A kept node still moves on the canvas.
        std::ranges::find(tolerant.nodes,std::string{"mystery"},&editor::GraphNode::name)->position={30,-40};
        require(Json::parse(take(editor::save_graph(other,tolerant)))["nodes"]["mystery"]["position"]==Json::array({30.0,-40.0}),
            "A kept node did not save its new position.");
        Evaluator kept{other,tolerant.graph,meshes};take(kept.evaluate());
        require(kept.output()!=nullptr,"A tolerantly loaded graph did not evaluate.");
    }
    // Settings keep their unfit leaves and a display naming a node that did not load.
    {
        auto hidden=saved;hidden["nodes"]["mystery"]=damaged["nodes"]["mystery"];
        hidden["settings"]["display"]="mystery";hidden["settings"]["seed"]=-1;
        const auto hidden_text=hidden.dump(2)+"\n";
        Runtime other;take(register_types(other));
        auto tolerant=take(editor::load_graph(other,hidden_text));
        const auto at=[&](std::vector<std::string> field){return std::ranges::count(tolerant.issues,field,&world::Diagnostic::field)==1;};
        require(tolerant.issues.size()==3 && at({"nodes","mystery"}) && at({"settings","display"}) && at({"settings","seed"}) &&
            take(contents(other,tolerant.graph.id())).display==ObjectId{} && take(editor::save_graph(other,tolerant))==hidden_text,
            "Unfit settings or a display naming an unloaded node were not kept and written back.");
    }
    // A connection that would close a cycle is kept like an unresolved one; a node that fails to
    // construct is the Runtime's failure, not the file's.
    {
        auto cyclic=saved;cyclic["nodes"]["moved"]["inputs"]["input"]="copied.output";
        const auto cyclic_text=cyclic.dump(2)+"\n";
        Runtime other;take(register_types(other));
        auto tolerant=take(editor::load_graph(other,cyclic_text));
        require(tolerant.issues.size()==1 && tolerant.issues[0].kind==world::DiagnosticKind::reference_dropped &&
            tolerant.issues[0].field==std::vector<std::string>{"nodes","moved","inputs","input"} && take(editor::save_graph(other,tolerant))==cyclic_text,
            "A connection closing a cycle was not kept and written back.");
        take(NativeRegistration<ThrowingNode>{other,"graphAcceptance.Throwing",TypeKind::object}.base<Node>()
            .method<&ThrowingNode::evaluate>("evaluate").parameter_names({"output"}).publish());
        auto throwing=saved;throwing["nodes"]["grid"]["type"]="graphAcceptance.Throwing";throwing["nodes"]["grid"]["properties"]=Json::object();
        const auto failed=editor::load_graph(other,throwing.dump());
        require(!failed && failed.error().code==ErrorCode::construction_failed,"A node that failed to construct was kept as an unknown type.");
    }

    // Structural errors fail the load.
    const auto rejects=[&](Json file,const char* message) {
        Runtime other;take(register_types(other));
        const auto result=editor::load_graph(other,file.dump());
        require(!result && result.error().code==ErrorCode::argument_mismatch,message);
    };
    auto broken=saved;broken["version"]=2;rejects(broken,"A graph of another version loaded.");
    broken=saved;broken["nodes"]=Json::array();rejects(broken,"A node list that is not a record loaded.");
    broken=saved;broken["extra"]=1;rejects(broken,"An unknown top-level key loaded.");
    broken=saved;broken["nodes"]["9lives"]=broken["nodes"]["grid"];rejects(broken,"An invalid node name loaded.");
    broken=saved;broken["nodes"]["moved"]["inputs"]["input"]=5;rejects(broken,"A malformed connection loaded.");
    broken=saved;broken["nodes"]["moved"]["inputs"]["input"]="gridoutput";rejects(broken,"A connection without an output name loaded.");
    broken=saved;broken["nodes"]["moved"]["inputs"]="grid.output";rejects(broken,"Inputs that are not a record loaded.");
    broken=saved;broken["nodes"]["moved"].erase("type");rejects(broken,"A node without a type loaded.");
    broken=saved;broken["settings"]["display"]=5;rejects(broken,"A display that names no node loaded.");

    // Saving needs one valid entry per graph node.
    auto duplicate=take(editor::load_graph(runtime,text));
    duplicate.nodes[1].name=duplicate.nodes[0].name;
    require(!editor::save_graph(runtime,duplicate),"A document with a repeated node name saved.");
    auto missing=take(editor::load_graph(runtime,text));missing.nodes.pop_back();
    require(!editor::save_graph(runtime,missing),"A document without an entry for a graph node saved.");
    std::puts("Procedural graph files: PASS (canonical v1 text, reopen in a new Runtime, same output and bytes, tolerant write-back, structural errors)");
}
