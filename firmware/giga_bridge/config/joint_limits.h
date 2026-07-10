// Compile-time joint limits, enforced at the last stage before the bus
// write regardless of what the host or policy commands (see
// docs/ARCHITECTURE.md). These are conservative bench-bring-up placeholders;
// Milestone 3 re-derives them on the real arm and freezes them here.
// Not host-writable by design: the host is the component we defend against.
#pragma once
#include "../protocol.h"

namespace giga {

constexpr float kJointMinRad[kDof] = {-1.57f, -1.57f, -1.57f,
                                      -1.57f, -1.57f, -1.57f};
constexpr float kJointMaxRad[kDof] = {1.57f, 1.57f, 1.57f,
                                      1.57f, 1.57f, 1.57f};
// STS3215 no-load speed is ~3.7 rad/s; clamp below it so a runaway setpoint
// can never demand more than the servo can physically track.
constexpr float kJointVmaxRadS[kDof] = {3.0f, 3.0f, 3.0f, 3.0f, 3.0f, 3.0f};

}  // namespace giga
