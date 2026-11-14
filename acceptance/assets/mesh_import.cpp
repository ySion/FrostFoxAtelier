#include "../helpers.hpp"
#include "assets/mesh_import.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>

void verify_mesh_import() {
    using namespace atelier;
    using namespace acceptance::helpers;
    const auto path =
        std::filesystem::temp_directory_path() /
        ("atelier-import-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".obj");
    {
        std::ofstream output(path, std::ios::binary);
        output << "v 0 0 0\nv 2 0 0\nv 0 3 0\nf 1 2 3\n";
        output.close();
        require(bool(output), "Cannot write OBJ fixture.");
    }
    const auto imported = take(assets::load_obj(path));
    std::filesystem::remove(path);
    const auto vertices = imported.mesh->vertices();
    require(vertices.size() == 3 && vertices[1].position[0] == 2 && vertices[2].position[1] == 3 &&
                imported.mesh->edge_mask(0) == 7,
            "Imported triangle differs from the authored OBJ coordinates.");
}
