void verify_gltf_import() {
    namespace fs = std::filesystem;
    const auto source = fs::current_path() / "gltf-transform-evidence" /
                        std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(source);
    // Three interleaved vertices: position, normal, tangent, UV.
    const float d = .70710678f;
    const float vertices[]{0, 0, 0, d, 0, d, 1, 0, 0, 1, 0, 0, 1, 0, 0, d, 0, d,
                           1, 0, 0, 1, 1, 0, 0, 1, 0, d, 0, d, 1, 0, 0, 1, 0, 1};
    take(editor::write_file(source / "mesh.bin", as_bytes(Span{vertices})));
    take(editor::write_file(source / "mesh.gltf", R"({
        "asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],
        "nodes":[{"translation":[1,2,3],"scale":[2,1,1],
                  "rotation":[0,0.7071067811865476,0,0.7071067811865476],"mesh":0}],
        "meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TANGENT":2,"TEXCOORD_0":3}}]}],
        "buffers":[{"uri":"mesh.bin","byteLength":144}],
        "bufferViews":[{"buffer":0,"byteLength":144,"byteStride":48}],
        "accessors":[{"bufferView":0,"byteOffset":0,"componentType":5126,"count":3,"type":"VEC3"},
                     {"bufferView":0,"byteOffset":12,"componentType":5126,"count":3,"type":"VEC3"},
                     {"bufferView":0,"byteOffset":24,"componentType":5126,"count":3,"type":"VEC4"},
                     {"bufferView":0,"byteOffset":40,"componentType":5126,"count":3,"type":"VEC2"}]
    })"));
    const auto loaded = take(assets::load_gltf(source / "mesh.gltf"));
    const auto actual = loaded.mesh->vertices();
    require(actual.size() == 3, "glTF triangle lost vertices.");
    const auto close = [](const auto& actual, std::initializer_list<double> expected) {
        std::size_t i = 0;
        for (const auto value : expected)
            if (std::abs(actual[i++] - value) > 1e-5)
                return false;
        return true;
    };
    // Scale X by 2, rotate Y by 90 degrees, translate, then mirror X for engine coordinates.
    require(close(actual[0].position, {-1, 2, 3}) && close(actual[1].position, {-1, 2, 1}) &&
                close(actual[2].position, {-1, 3, 3}),
            "glTF transform changed vertex positions.");
    // The normal follows inverse transpose; the tangent follows the matrix and orthogonalization.
    require(close(actual[0].normal, {-.894427, 0, -.447214}) && close(actual[0].tangent, {.447214, 0, -.894427, -1}),
            "glTF normal or tangent did not follow its transform.");
}
