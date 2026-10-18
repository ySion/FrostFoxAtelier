#pragma once

#include "world/timing.hpp"

// CPU phases of PhysicsScene::step, which World::update runs under
// world.simulation. Substep, solve and write_back are entered once per fixed step.
namespace atelier::physics::phases {
inline constinit timing::Marker step{"physics.step", "Physics step", &world::phases::simulation};
inline constinit timing::Marker reconcile{"physics.reconcile", "Reconcile", &step};
inline constinit timing::Marker substep{"physics.substep", "Substep", &step};
// Wall time of the Jolt update, including any work its job system runs.
inline constinit timing::Marker solve{"physics.solve", "Solve", &substep};
// Pose readout and World transform write-back.
inline constinit timing::Marker write_back{"physics.write_back", "Write back", &substep};
inline constinit timing::Marker events{"physics.events", "Contact events", &step};
inline constexpr std::array<timing::Marker*, 6> markers{&step, &reconcile, &substep, &solve, &write_back, &events};
}
