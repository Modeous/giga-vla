# giga-vla

Run a VLA (vision-language-action) policy on a GPU host and stream actions to
an Arduino GIGA R1 WiFi controlling a robot arm.

- **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** — the v1 decision record:
  system topology, wire protocol, failure modes, and milestones. Read it
  before changing the wire protocol, control loop, or safety behavior.
- **[CLAUDE.md](CLAUDE.md)** — house rules for model-assisted development in
  this repo.

## v1 in one paragraph

All inference (LeRobot SmolVLA/ACT) runs on the host; the Giga runs zero ML —
it is a 100 Hz chunk interpolator and the sole safety authority. The host
ships 50-action chunks (float32 radians, COBS+CRC16 over USB CDC) at ~10 Hz;
the Giga interpolates, clamps, and sync-writes Feetech STS3215 bus servos,
streaming measured joint positions back at 20 Hz as the policy's
proprioception. Scope: one arm, one policy, one transport.
