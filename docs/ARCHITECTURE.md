# GIGA-VLA Architecture (v1 decision record)

## Audit of current state

The repo contains a single README ("Repository for GIGA-VLA work. Hardware test pipeline scaffolding incoming via PR") and no code. There are no existing patterns to copy inside the repo, so this document is the greenfield decision record; the pattern we copy instead is the LeRobot SO-100/SO-101 ecosystem, which is the closest working system to what we are building. Scope of v1: **one arm, one policy, one transport.**

## System overview

All VLA inference (SmolVLA / ACT via LeRobot) runs on the host GPU in one Python process. The Giga R1 runs zero ML: it is a real-time chunk interpolator and the sole safety authority. The host ships whole 50-action chunks (float32 radians) over USB CDC at ~10 Hz; the Giga's M7 core owns the only clock, interpolating on a 100 Hz deadline-paced superloop and sync-writing Feetech STS3215 bus servos. Measured joint positions stream back at 20 Hz on the same link and become the policy's proprioception. Cameras attach to the host and never touch the Giga. The M4 core stays in reset; there is no RTOS thread of ours, no WiFi, no second transport.

```
 [USB webcam] --30fps--> +---------------------+
                         |  HOST (GPU)         |
                         |  LeRobot policy     |   ACTION_CHUNK (50 x dof f32, COBS+CRC16)
                         |  GigaRobot subclass | ------ ~10 Hz ------->  +------------------+
                         |  pyserial link      | <----- 20 Hz STATE ---- |  GIGA R1 (M7)    |
                         +---------------------+   (measured q, flags)   |  100 Hz superloop|
                              USB CDC (native USB-C, <2 ms, >1 MB/s)     |  interp + clamps |
                                                                         |  watchdog + IWDG |
                                                                         +--------+---------+
                                                              SYNC WRITE 100 Hz / |  SYNC READ 20 Hz
                                                              half-duplex TTL, 1 Mbps
                                                                         +--------v---------+
                                              [7.4-12V stall-sized PSU]--| STS3215 bus      |
                                              [NC e-stop in servo rail]  | servos (arm)     |
                                                                         +------------------+
```

## Decisions

| Decision | Choice | Why | Rejected alternatives |
|---|---|---|---|
| Inference placement | All inference on host GPU; Giga = interpolator + safety only; M7 only, M4 left in reset; no RTOS threads, no control ISR — one cooperative superloop with `micros()` deadline pacing | VLA weights need a GPU; 1 MB RAM on the MCU. Chunking already bridges the 3–15 Hz inference / 100 Hz control gap. Superloop body is tens of µs against a 10 ms period — an ISR or second core adds shared-state hazards for zero timing gain | On-device inference (impossible); M4 split via OpenAMP RPC (flakiest part of the mbed core, solves no v1 problem); host-streamed 100 Hz setpoints (makes every Python GC pause a physical jerk) |
| Transport | USB CDC serial (native USB-C), only transport in v1. Host side pyserial; port discovered by **USB serial number**, never `/dev/ttyACM0` | Robot is bench-tethered for servo power anyway. USB is an in-order byte stream with one failure mode (unplug/stall), already covered by the watchdog. Link budget ≈16 KB/s vs >1 MB/s capacity | WiFi UDP/TCP (jitter, association drops, loss-handling machinery); "both with fallback" (doubles the test matrix); CAN (multi-node answer for a one-node problem). COBS frames are transport-agnostic; a v2 transport is a new link class, zero protocol change — documented, not built |
| Framing | COBS with 0x00 delimiters + CRC-16/CCITT-FALSE, hand-written (~40 lines each side) | Guaranteed resync at the next delimiter by construction (magic-byte sync can false-lock on 0xAA55 inside a float payload); bounded 1-in-254 overhead; visible code we own (rule 8) | nanopb/protobuf (codegen + schema-evolution machinery for 3 message types); JSON (parse cost, float round-trip bugs); magic-byte header (2 of 3 judges preferred COBS's provable resync); standalone `cobs`/`crcmod` packages (each replaces <15 lines) |
| Action encoding | float32 radians end-to-end. Tick conversion happens in exactly one place: the firmware servo-bus driver | Deletes the per-joint scale table that must silently match across two hand-mirrored codebases — the classic silent-divergence bug. M7 has an FPU; bandwidth is a non-issue. Cost: NaN/inf become encodable → explicit non-finite rejection at chunk admission, host-side and firmware-side, with a golden corrupt-vector | int16 servo ticks (bakes one vendor's 0–4095 convention into the wire); u16 centidegrees (still a mirrored scale constant); normalized [-1,1] (the "interpreted as degrees" violent-motion bug) |
| Calibration ownership | **Firmware owns tick↔radian.** A compiled-in per-joint calibration header (`firmware/config/joint_calib.h`: zero offset, direction sign, ticks/rad) lives in the repo; the wire, the host, the dataset, and the policy see only radians. A one-off host-guided calibration script generates the header | Exactly one component maps units; record-time and inference-time share it automatically because both only ever see radians. This was the largest unclaimed silent-divergence risk in the council | Host-side calibration file (two things must agree at runtime); host-uploadable calibration (persistence + validation surface, and a way to smuggle wrong values) |
| Gripper | Joint index `dof-1` in the action vector (LeRobot convention), same f32 radians, mapped by the same firmware calibration table | One wire contract, one dataset feature shape, zero special cases | Separate gripper command (second code path); normalized open/close unit (reintroduces a scale constant) |
| Servos | Feetech STS3215 bus servos, one half-duplex TTL bus on Serial1 @ 1 Mbps: SYNC WRITE @ 100 Hz (~60 B ≈ 0.6 ms), SYNC READ @ 20 Hz scheduled inside the tick after the write, ~2 ms bounded reply timeout | Measured position readback: real proprioception (no commanded-echo train/deploy skew), no first-PWM-snap hazard, torque-off/hold control, in-band voltage/load/temp telemetry. Copies the SO-100/SO-101 pattern SmolVLA was pretrained on. Choosing the servo that deletes safety code is the simplicity move — it removed an entire tier of the safety proposal | Hobby PWM servos (write-only: fake proprioception, un-mitigatable boot-snap, no compliant failsafe); Dynamixel (2–3× cost, same architecture — a one-module swap later); CAN motor drivers (over-built for a tabletop arm) |
| Control loop | 100 Hz tick, deadline advanced by adding the period (never re-reading now). Linear interpolation on the chunk's dt grid; depth-2 chunk buffer (active + pending); each new segment interpolates **from the currently commanded position**, not the chunk's nominal start; fixed 20 ms playback smoothing offset absorbs host jitter | 100 Hz is 3× the 30 Hz waypoint grid — smoother is unobservable through the servo's own position loop. Interpolate-from-commanded guarantees positional continuity at chunk boundaries by construction (~5 lines). Relative timestamps + the 20 ms offset delete the entire clock-sync problem | 200 Hz / 1 kHz tick (buys nothing measurable, shrinks the bus budget); cubic/spline (overshoot risk, invisible at 2–3 samples/segment); deep chunk FIFO (queued chunks are stale intentions — newest wins, depth 2); host-synced absolute timestamps (an NTP subsystem for one link) |
| Chunk admission | Firmware rejects a whole chunk if: any value non-finite, `dof` mismatch, `seq` stale, or first action >15° from current commanded position on any joint (sets CHUNK_REJECTED flag, holds). Admitted setpoints are then position-clamped to compiled-in per-joint limits and velocity-clamped (rad/s from datasheet) at the last stage before the bus write; clamping sets a sticky counter in STATE | Slewing toward a hallucinated pose is executing the hallucination slowly — gross discontinuity must be refused, not smoothed. Ordinary grazes are clamped, not faulted, because policies graze limits constantly. Limits compile-time, not host-writable: the host is the component we defend against | Clamp-everything-including-teleports (judged unsafe, minority position); reject any slightly-out-of-range value (turns a clamp into a stutter); host-uploadable limit tables; acceleration limits / torque estimation (velocity clamp covers the damage mode at this actuator class) |
| Watchdog & faults | Single tier: no valid chunk for **300 ms** → ramp to torque-**hold** over 200 ms (never limp — gravity collapse is the crash), latch FAULT, refuse chunks until explicit `ENABLE(1)`. No auto-resume: a restarted host has stale state. STM32 **IWDG** (~120 ms) kicked from the loop covers our own firmware hang → resets to DISARMED (torque off) with reset-cause reported in STATE. Boot state is DISARMED; firmware reads real positions (bus servos) and first motion after ENABLE is slew-limited by the always-on clamp. Constants are transport-coupled: re-derive from measured USB jitter at bring-up, then freeze here | The 10 Hz chunk stream **is** the heartbeat. Hold-not-limp + latch + IWDG are the three invariants from the safety lens; everything else it proposed was apparatus this servo choice or the clamp already covers | Neutral/detach on timeout (drops a gravity-loaded arm — explicitly overruled); auto-resume on fresh chunk (a crashed host lurching back to life — overruled); three-tier FREEZE/SOFT_STOP + 20 Hz HEARTBEAT frame (tiers observationally near-identical, frame is pure traffic) |
| E-stop | NC mushroom switch: contact A in series with the servo power rail (hardware kill, works with the MCU dead — verified by measuring 0 V with firmware intentionally hung); contact B to a GPIO so firmware latches FAULT and reports. NC = broken wire fails safe. Recovery: button released **and** `ENABLE(1)` | The rail cut is the guarantee; the GPIO is one input pin so the host sees why the arm stopped. Dual-condition recovery prevents the stale-buffer-replay-into-a-hand incident | Firmware-only e-stop (a request, not a stop); NO button (loose wire silently disarms your e-stop); dedicated CLEAR_ESTOP/ARM message pair (ENABLE already latches/clears) |
| Power | Two rails, common ground: Giga logic from host USB; servos from a dedicated 7.4–12 V supply sized for **simultaneous stall** (n × ~1 A for STS3215 @ 7.4 V, so ≥10 A for 6–8 joints), 2200 µF bulk + ceramics at the distribution point. Never through the Giga's 5 V/VIN. Rail health read **in-band** from the servos (voltage/load/temp registers), no ADC divider | A stalled servo is a brownout and a jam at once; separate rails guarantee the brownout can't reset the safety controller. The rejected proposal's supply math was for PWM servos — this is the redone spec for the actual BOM | Shared rail + regulator (stall reboots the watchdog); ADC rail-sag divider + FAULT_POWER (duplicates telemetry the bus already provides — cut); per-servo shunt sensing (v2-if-ever) |
| Host stack | LeRobot (`lerobot[smolvla]`, version-pinned) + `pyserial` — the only two direct dependencies. One class, `GigaRobot(lerobot.robots.Robot)` (`connect/get_observation/send_action/disconnect`) over our link; teleop, record, replay, train, eval inherited unmodified. Policy path: **ACT from scratch first** (<2 h, pipeline validator), then fine-tuned `lerobot/smolvla_base`. `policy.predict_action_chunk()` → whole chunk to MCU; re-infer and resend at ~50% chunk consumption (receding horizon). Checkpoint owns normalization stats — never hand-rolled | Rule 1 at ecosystem scale: recording is configuration, not code; `lerobot-replay` is a standing end-to-end regression test; teleop shares the inference wire path so data collection integration-tests the protocol continuously | Hand-rolled inference client (reimplements normalization/preprocessing/queueing — every mismatch a silent quality bug); openpi (JAX, A100-class fine-tuning); OpenVLA (7B, no chunking); LeRobot async gRPC server (a network service for a single-host deployment); ROS 2 (middleware for one serial port) |
| Host runtime guards | (a) **Observation freshness gate**: every camera frame and STATE sample is timestamped; if either exceeds its staleness bound, the host stops sending chunks and sends `ENABLE(0)` — this covers the *alive-but-blind* host no MCU watchdog can see. (b) **Inference overrun policy**: log latency every cycle; after N consecutive starvation events (CHUNK_STARVED echoed), the host halts and reports rather than stutter-holding forever. (c) **Flight recorder**: append-only binary log of every TX/RX frame, flag transition, and latency sample — every incident becomes a replayable trace. (d) On CDC re-enumeration: treat as FAULT, reopen by serial number, require re-ENABLE | Every firmware watchdog defends against a dead host; these three defend against a healthy-looking link carrying garbage — the largest gap all judges flagged | "The MCU watchdog handles it" (it can't see a frozen cv2 frame); auto-retry/resume on reconnect (same stale-state hazard as watchdog auto-resume) |
| Observations | One USB webcam 640×480@30fps via LeRobot `OpenCVCamera`, rigidly mounted, capture config frozen in code; `observation.state` = latest 20 Hz measured joint positions (radians). Feature names match the dataset exactly (`observation.images.front`, `observation.state`) | Inference-time distribution must match training: same camera, mount, resolution, units. Measured (not echoed) proprioception thanks to the servo choice | Second/wrist camera (add only if fine-tune plateaus); depth (SmolVLA doesn't consume it); image bytes over the Giga link (1 MB RAM, no reason) |
| Repo & toolchain | Monorepo: `host/` (Python pkg: `link.py`, `messages.py`, `robot.py` GigaRobot, `tests/`), `firmware/giga_bridge/` (`.ino`, `protocol.h`, `cobs.h`, `crc16.h`, `chunk_buffer.h`, `servo_bus.h`, `config/joint_limits.h`, `config/joint_calib.h`), `protocol/` (`PROTOCOL.md` + `vectors/` golden bytes), `tools/fake_giga.py` (pty emulator), `docs/ARCHITECTURE.md`. Build: `arduino-cli` with `arduino:mbed_giga` **version-pinned** in `sketch.yaml`; CI compiles firmware from clean checkout and runs both test suites on every PR. Firmware logic (parser, state machine, chunk buffer, watchdog timers) sits behind a `millis()`/bus-write seam so it compiles and tests **natively**, off-target | Protocol changes must be atomic across two languages: one PR touches `protocol.h`, `messages.py`, `PROTOCOL.md`, and vectors in one reviewable diff. The mbed core has shipped broken CDC between minor versions — pinning is load-bearing | Two repos (guaranteed drift); submodules (standing tax); schema codegen (a build tool to generate ~60 lines); PlatformIO (community board defs lag the official core); Arduino IDE as build system (not CI-runnable) |

### Deliberately not in v1 (cut, with the reason each is covered)

- **WiFi / any second transport** — frames are transport-agnostic; a v2 transport is a new link class, not a protocol change.
- **M4 core, RTOS threads, control ISR** — <1% M7 utilization; revisit only for a ≥1 kHz torque loop (and even then, a smarter servo is the better answer).
- **HEARTBEAT, NACK, HELLO, PING/PONG, STATS, BOOT_NOTICE, CLEAR_ESTOP message types** — the chunk stream is the heartbeat; the host's resend timer already does what NACK would trigger; version/reset-cause/counters are fields in STATE.
- **Multi-tier watchdog (150 ms FREEZE stage)** — after freezing there is no residual velocity to ramp; one threshold + latch has the same observable behavior at half the test matrix.
- **Limits-table CRC handshake, host-uploadable limits/calibration** — one arm, one compiled firmware; the version fields in STATE identify the build.
- **ADC servo-rail sag monitor / FAULT_POWER** — STS3215 reports voltage, load, and temperature in-band.
- **Assumed-pose ARM handshake, guarded 2 s first move, cradle procedure** — existed solely to mitigate feedback-less PWM servos; bus servos read real positions at boot.
- **Temporal ensembling / cross-fade of overlapping chunks** — interpolate-from-commanded + velocity clamp covers it; the named trigger to revisit is *visible* jerk at chunk boundaries on the bench, a contained change to `chunk_buffer.h`.
- **200 Hz control / 100 Hz telemetry** — over-spec against 30 Hz waypoints and 3–15 Hz observation consumption; violates the half-duplex bus budget.
- **Leader-arm purchase, 50-episode dataset, Hub push, SmolVLA fine-tune as v1 gates** — real work, but it is v1.5; the v1 done-line is *one arm follows one policy* (a pretrained checkpoint or from-scratch ACT on throwaway episodes).
- **Acceleration limits, per-servo current sensing, protobuf, ROS, gRPC, VR teleop, depth cameras** — each defends against a failure the clamp, the servo choice, or the existing stream already covers.

## Wire protocol

Framing: every message is COBS-encoded and delimited by `0x00`. Payloads are packed little-endian structs; the last two bytes before encoding are CRC-16/CCITT-FALSE over everything preceding them. A corrupted or truncated frame is dropped (counter incremented); the stream self-resynchronizes at the next `0x00`. Canonical spec: `protocol/PROTOCOL.md`; golden byte vectors in `protocol/vectors/` are round-trip-tested by pytest **and** the natively-compiled firmware codec — including a corrupted-CRC vector, a NaN-payload vector (must be rejected), and an **n=1 teleop chunk** vector.

**ACTION_CHUNK (0x01), host → Giga, ~10 Hz (n=1 at 30 Hz during teleop):**

| field | type | meaning |
|---|---|---|
| type | u8 = 0x01 | |
| seq | u16 | monotonic, wraps; stale seq rejected |
| dof | u8 | must match firmware build, else chunk rejected |
| n | u8 | steps, 1..100 |
| dt_ms | u16 | per-step period (33 = 30 Hz action grid) |
| q | f32[n·dof] | joint targets, radians (gripper = joint dof−1); non-finite ⇒ whole chunk rejected |
| crc | u16 | |

(n=50, dof=6 ⇒ 1209 B payload, ~1215 B on the wire, ~12 KB/s at 10 Hz.)

**ENABLE (0x02), host → Giga:**

| field | type | meaning |
|---|---|---|
| type | u8 = 0x02 | |
| enable | u8 | 1 = arm / clear latched fault (only if e-stop released); 0 = disarm, torque off |
| crc | u16 | |

**STATE (0x10), Giga → host, fixed 20 Hz:**

| field | type | meaning |
|---|---|---|
| type | u8 = 0x10 | |
| proto_ver | u8 | host refuses to ENABLE on mismatch |
| fw_ver | u8 | |
| reset_cause | u8 | power-on vs IWDG — host must distinguish crash from fresh start |
| seq_echo | u16 | last accepted chunk (delivery confirmation; RTT of send→echo is the latency measurement procedure) |
| t_ms | u32 | MCU millis |
| flags | u8 | bit0 ENABLED, bit1 FAULT_LATCHED, bit2 CHUNK_STARVED, bit3 CHUNK_REJECTED, bit4 ESTOP |
| joint_valid | u8 | per-joint bitmask; a joint failing N consecutive SYNC READs clears its bit and latches FAULT |
| crc_err_count | u16 | wraps; host logs deltas |
| clamp_count | u16 | position/velocity clamp events; frequent clamping must be visible, not silent |
| q_meas | f32[dof] | measured joint positions, radians |
| crc | u16 | |

Three message types total. There is no retransmission: actions are absolute positions and newest-wins (max one pending chunk), so a lost chunk costs freshness, never correctness.

## Failure modes and responses

| # | Failure | Detection | Designed response | Verified by |
|---|---|---|---|---|
| 1 | Host process crashes / GPU hang / cable pulled | No valid chunk for 300 ms | Ramp to torque-**hold** over 200 ms, latch FAULT; STATE keeps broadcasting; chunks refused until `ENABLE(1)` | `kill -9` mid-stream: hold within 500 ms total on logic analyzer; restart without ENABLE moves nothing |
| 2 | Byte corruption on the wire | CRC fail / truncated frame | Drop frame, resync at next 0x00, increment `crc_err_count`; a partial frame never moves a servo | Native fuzz (1M malformed frames: zero accepted, zero hangs); injected-corruption count equals reported count |
| 3 | Policy emits garbage (NaN, teleport) | Chunk admission: non-finite, dof mismatch, or first action >15° from commanded | Reject whole chunk, set CHUNK_REJECTED, hold; ordinary grazes clamped + counted instead | Golden NaN vector; crafted 90° jump chunk on hardware: arm does not move |
| 4 | Firmware hangs (our bug) | IWDG not kicked for ~120 ms | Hardware reset → DISARMED (torque off — the one case hold is impossible: the holder died), `reset_cause=IWDG` in STATE; host alerts loudly, no silent re-arm | Debug build with induced `while(1)`: reset ≤120 ms, correct reset_cause |
| 5 | One servo dies / bus read fails | N consecutive SYNC READ timeouts (~2 ms bound each) | Clear joint's `joint_valid` bit, latch FAULT, hold remaining joints; host never feeds the policy stale positions marked fresh | Unplug one servo mid-motion: FAULT within N read cycles, loop jitter budget holds |
| 6 | Host alive but blind (frozen cv2 frame, stale STATE) | Host-side freshness gate on frame + proprio timestamps | Host stops sending chunks, sends ENABLE(0), logs; MCU watchdog then holds | Fake_giga test: freeze the state stream, assert host disarms within bound |
| 7 | Inference chronically slower than half the chunk horizon | CHUNK_STARVED echoed N consecutive cycles + host latency log | Host halts and reports (never indefinite stutter-hold); fix is a longer horizon or a smaller model, decided by a human | Soak test with artificially delayed inference |
| 8 | USB CDC re-enumeration (host reboot, odd reopen) | Port vanishes / serial-number reopen | Firmware side: falls out of the same 300 ms watchdog. Host side: treat as FAULT, reopen by USB serial number, require re-ENABLE | Explicit reconnect test: reopen without firmware reset or robot power cycle |
| 9 | Human hits e-stop | NC contact A cuts servo rail (hardware); contact B GPIO latches ESTOP flag | Rail dead regardless of firmware state; recovery = button released **and** ENABLE(1) | Press with firmware intentionally hung: measure 0 V on rail (physical checklist — software review cannot verify wiring) |
| 10 | Servo stalls against an obstacle while holding | In-band load/temp telemetry; STS3215 internal overload protection | Servo's own protection backstops; host watches load fields and disarms on sustained overload | Deliberate hand-stall on the bench: logic rail stays up (separate rails), overload visible in telemetry |

## Next steps

**Milestone 0 — Hardware decision executed (procurement).**
Order the STS3215-based arm (or SO-101 kit), bus adapter, stall-sized 7.4–12 V supply, bulk caps, NC mushroom switch. *Success criterion:* BOM checked into `docs/` with the supply-sizing math; nothing below blocks on delivery except milestones 2–5's hardware halves.

**Milestone 1 — Protocol truth + both codecs + emulator, zero hardware.**
Write `protocol/PROTOCOL.md` (the tables above), golden vectors (nominal 50×6 chunk, n=1 teleop chunk, corrupted-CRC, NaN-reject, dof-mismatch), implement `host/link.py`+`messages.py` and firmware `cobs.h`/`crc16.h`/`protocol.h` (native-compiled tests), the native fuzz target, and `tools/fake_giga.py`. *Success criterion:* CI on a clean checkout compiles the pinned-core firmware, passes pytest and native tests on identical golden vectors, survives 1M-frame fuzz with zero bad-CRC acceptances and zero hangs, and runs a 10-minute host↔fake_giga soak at 10 Hz chunks / 20 Hz STATE with zero errors — **no board, no robot**.

**Milestone 2 — Firmware skeleton on the board, no servos.**
Parser + 100 Hz deadline loop + chunk buffer + watchdog + fault latch, bus writes stubbed to setpoint echo; state machine and timers tested natively behind the seam first. *Success criterion:* p99 tick jitter <1 ms over a 10-minute logic-analyzer capture; echoed setpoint trace continuous across every chunk swap with per-tick deltas never exceeding the clamp (plotted and asserted by a host script); `kill -9` produces hold-within-500 ms and latched FAULT; host reconnect works without firmware reset; STATE emission never blocks the loop when the host stops reading.

**Milestone 3 — Servo bus + safety on real hardware, arm bolted down, no policy.**
STS3215 SYNC WRITE/READ, calibration script → `joint_calib.h`, limits in `joint_limits.h`, e-stop wiring. *Success criterion:* one servo tracks a host-generated 0.5 Hz sinusoid chunk stream with no visible stepping; commanded sweep past a limit stops exactly at the limit; 90° teleport chunk is rejected (arm motionless, CHUNK_REJECTED set); USB yank mid-motion → hold + latch, resume only via ENABLE(1); e-stop pressed with firmware hung reads 0 V on the servo rail; unplugging one servo latches FAULT with the correct `joint_valid` bit. All constants (300 ms, 200 ms ramp, 15°, vmax, 20 ms offset) re-derived from measured jitter and frozen in this document.

**Milestone 4 — GigaRobot + teleop + record/replay.**
`GigaRobot` LeRobot subclass over the link (tested against fake_giga in CI, then hardware); keyboard/gamepad teleop (n=1 chunks); record 5 throwaway episodes; replay one through the chunk pipeline. *Success criterion:* `lerobot-teleoperate` drives all joints; dataset loads with correct feature names/shapes/fps; the replayed episode reproduces the recorded motion within a stated per-joint RMS error against `q_meas` — replay is now the standing end-to-end regression test; freshness gate proven by freezing fake_giga's state stream.

**Milestone 5 — One policy closes the loop (v1 done-line).**
Train ACT from scratch on the throwaway episodes (pipeline validator, <2 h consumer GPU); **before the arm moves**, run the checkpoint open-loop over a held-out episode and assert action MSE against the demonstration (offline eval, no robot). Then deploy through the identical path. *Success criterion:* 5-minute closed-loop run on the physical arm with CHUNK_STARVED never set, logged inference latency consistently under half the chunk horizon, flight-recorder trace captured, and a clean stop via ENABLE(0). SmolVLA fine-tuning, the leader arm, and the 50-episode dataset are v1.5 and do not gate this line.
