# GIGA-VLA Wire Protocol v1

Canonical spec for the host ↔ Giga link. The Python codec
(`host/giga_host/`), the firmware codec (`firmware/giga_bridge/`), and the
golden vectors (`protocol/vectors/`) must all agree with this document. A
protocol change is one PR touching all of them atomically, and bumps
`PROTO_VER`.

## Framing

Every message is a payload of packed little-endian fields, followed by a
CRC, COBS-encoded, and terminated with a single `0x00` delimiter byte:

```
frame = COBS( payload ‖ crc16_le(payload) ) ‖ 0x00
```

- **CRC**: CRC-16/CCITT-FALSE (poly `0x1021`, init `0xFFFF`, MSB-first, no
  reflection, no final XOR) over every payload byte, appended little-endian.
  Check value: `crc16("123456789") == 0x29B1`.
- **COBS**: standard consistent-overhead byte stuffing; the encoded frame
  contains no `0x00` bytes, so the stream self-resynchronizes at the next
  delimiter after any corruption.
- A frame that fails COBS decode, is shorter than 3 decoded bytes, or fails
  CRC is **dropped** and counted (`crc_err_count`). It never reaches message
  parsing.

Decoded payloads below are shown **without** the trailing CRC.

## Constants

| name | value | meaning |
|---|---|---|
| `PROTO_VER` | 1 | bumped on any wire change; host refuses to ENABLE on mismatch |
| `DOF` | 6 | joints incl. gripper (gripper = joint `DOF-1`), compiled into firmware |
| `MAX_STEPS` | 100 | max `n` in a chunk |
| `MAX_FRAME` | 2048 | encoded-frame buffer size both sides |
| `WATCHDOG_MS` | 300 | no valid chunk while ENABLED → ramp to hold + latch FAULT |
| `MAX_FIRST_STEP_RAD` | 0.261799 (15°) | chunk admission discontinuity bound |
| `STATE_HZ` | 20 | STATE emission rate |

Units are **float32 radians** end-to-end. Tick↔radian conversion happens
only in the firmware servo-bus driver.

## Messages

### ACTION_CHUNK (`0x01`), host → Giga, ~10 Hz (n=1 at 30 Hz during teleop)

| field | type | meaning |
|---|---|---|
| type | u8 = 0x01 | |
| seq | u16 | monotonic, wraps at 65536 |
| dof | u8 | must equal firmware `DOF` |
| n | u8 | steps, 1..MAX_STEPS |
| dt_ms | u16 | per-step period (33 = 30 Hz action grid) |
| q | f32[n·dof] | joint targets, radians, step-major (`q[step*dof + joint]`) |

Payload length without CRC: `7 + 4·n·dof` (n=50, dof=6 → 1207 B; 1209 B
with CRC; ~1215 B on the wire).

**Admission** (firmware rejects the whole chunk, sets `CHUNK_REJECTED`,
holds position, when):

1. `dof != DOF`, or `n` outside 1..MAX_STEPS, or payload length inconsistent;
2. any `q` value is non-finite (NaN/±inf);
3. `seq` is not newer than the last accepted seq, using serial-number
   arithmetic: newer ⇔ `((seq - last) & 0xFFFF)` is in `1..0x7FFF`;
4. any joint of the first step is more than `MAX_FIRST_STEP_RAD` from the
   currently commanded position.

Admitted setpoints are still position- and velocity-clamped downstream;
clamping increments `clamp_count` but does not reject.

### ENABLE (`0x02`), host → Giga

| field | type | meaning |
|---|---|---|
| type | u8 = 0x02 | |
| enable | u8 | 1 = arm / clear latched FAULT (only if e-stop released); 0 = disarm, torque off |

Payload length without CRC: 2 B.

### STATE (`0x10`), Giga → host, fixed 20 Hz

| field | type | meaning |
|---|---|---|
| type | u8 = 0x10 | |
| proto_ver | u8 | |
| fw_ver | u8 | |
| reset_cause | u8 | 0 = power-on, 1 = IWDG, 2 = other |
| seq_echo | u16 | last accepted chunk seq |
| t_ms | u32 | MCU millis, wraps |
| flags | u8 | bit0 ENABLED, bit1 FAULT_LATCHED, bit2 CHUNK_STARVED, bit3 CHUNK_REJECTED, bit4 ESTOP |
| joint_valid | u8 | per-joint bitmask, bit j = joint j readback healthy |
| crc_err_count | u16 | wraps; host logs deltas |
| clamp_count | u16 | wraps |
| q_meas | f32[dof] | measured joint positions, radians |

Payload length without CRC: `16 + 4·dof` (dof inferred from length; must
equal `(len − 16) / 4` exactly).

There is no retransmission and no NACK: actions are absolute positions and
newest-wins, so a lost chunk costs freshness, never correctness.

## Golden vectors (`protocol/vectors/`)

Checked-in wire frames (encoded bytes, including the `0x00` terminator),
round-trip-tested by pytest **and** the natively compiled firmware codec.
Regenerate with `python3 protocol/vectors/generate.py` (output must be
byte-identical to what is checked in).

| file | contents | expected |
|---|---|---|
| `chunk_nominal.bin` | seq=1, dof=6, n=50, dt=33, q[k]=k/1024 | decodes; admissible from commanded=0 |
| `chunk_teleop_n1.bin` | seq=2, dof=6, n=1, dt=33, q[j]=j/1024 | decodes; admissible from commanded=0 |
| `chunk_bad_crc.bin` | `chunk_nominal` with last CRC byte XOR 0xFF | dropped at framing, +1 crc_err |
| `chunk_nan.bin` | `chunk_nominal` with q[7]=NaN | decodes; **admission rejects** (non-finite) |
| `chunk_dof_mismatch.bin` | seq=3, dof=5, n=2 | decodes; **admission rejects** (dof) |
| `enable_on.bin` / `enable_off.bin` | enable=1 / enable=0 | decodes |
| `state_nominal.bin` | proto=1 fw=1 reset=0 seq_echo=1 t=123456 flags=0x01 jv=0x3F crc_err=0 clamp=2, q_meas[j]=−j/1024 | decodes |

`manifest.json` lists each vector with its expected decode/admission
outcome; both test suites read it.
