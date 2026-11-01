#pragma once

#include "foundation/frame_timing.hpp"

// CPU phases of World::update on the host thread. Each TickPhase has its own
// tick, commands and flush rows; world.schedule is entered once per schedule
// rebuild, before the phase that needs it. A stopped World records nothing.
namespace atelier::world::phases {
inline constinit timing::Marker update{"world.update", "World update", &timing::frame};

inline constinit timing::Marker schedule{"world.schedule", "Schedule rebuild", &update};

inline constinit timing::Marker pre_physics{"world.pre_physics", "Pre-physics", &update};

inline constinit timing::Marker pre_physics_tick{"world.pre_physics.tick", "Tick", &pre_physics};

inline constinit timing::Marker pre_physics_tasks{"world.pre_physics.tasks", "Tasks", &pre_physics};

inline constinit timing::Marker pre_physics_commands{"world.pre_physics.commands", "Commands", &pre_physics};

inline constinit timing::Marker pre_physics_flush{"world.pre_physics.flush", "Flush", &pre_physics};

// Registered simulations in order; their own phases are children of this one.
inline constinit timing::Marker simulation{"world.simulation", "Simulation", &update};

inline constinit timing::Marker post_physics{"world.post_physics", "Post-physics", &update};

inline constinit timing::Marker post_physics_tick{"world.post_physics.tick", "Tick", &post_physics};

inline constinit timing::Marker post_physics_commands{"world.post_physics.commands", "Commands", &post_physics};

inline constinit timing::Marker post_physics_flush{"world.post_physics.flush", "Flush", &post_physics};

inline constinit timing::Marker late{"world.late", "Late", &update};

inline constinit timing::Marker late_tick{"world.late.tick", "Tick", &late};

inline constinit timing::Marker late_commands{"world.late.commands", "Commands", &late};

inline constinit timing::Marker late_flush{"world.late.flush", "Flush", &late};

inline constexpr std::array<timing::Marker*, 16> markers{&update,
                                                         &schedule,
                                                         &pre_physics,
                                                         &pre_physics_tick,
                                                         &pre_physics_tasks,
                                                         &pre_physics_commands,
                                                         &pre_physics_flush,
                                                         &simulation,
                                                         &post_physics,
                                                         &post_physics_tick,
                                                         &post_physics_commands,
                                                         &post_physics_flush,
                                                         &late,
                                                         &late_tick,
                                                         &late_commands,
                                                         &late_flush};
}
