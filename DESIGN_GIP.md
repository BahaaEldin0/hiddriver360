# Xbox One / Series (GIP) controller support — design & implementation notes

This document maps out how HidDriver360 works today and how wired Xbox One /
Xbox Series controller support was added on top of it. It is the reference for
anyone testing, debugging, or extending the GIP path.

> **Status: DO NOT EXPECT THIS TO WORK AS-IS.** The protocol layer (init
> handshake + frame decoding) is now verified correct against the Linux `xpad`
> driver. **But the device-acquisition layer is an unsolved blocker** (see §5):
> on a stock console the kernel will never hand a GIP controller to our hook,
> because no Xbox 360 USB driver binds vendor-class (`0xFF`) devices. Solving
> that requires reverse-engineering the specific kernel build — it cannot be
> done blind. The code here is the correct *back half*; the *front half*
> (getting the device to us) still needs RE work on hardware.
>
> The driver also cannot be built or run in CI — it needs the leaked Xbox 360
> SDK, Visual Studio 2010, and a modded console.

---

## 1. How the driver works (background)

HidDriver360 is a dashboard plugin (injected DLL) that makes third‑party USB
controllers appear to the Xbox 360 as native pads. It does this entirely with
kernel-level function detours.

```
USB controller plugged in
        │
        ▼
HidAddDeviceHook            detour on the kernel HID class driver's add-device
   • read device + interface descriptors
   • decide if we want to own this device                 ← interface filter
   • alloc HidControllerExtension, open default endpoint
   • start init state machine (g_InitState)
        │
        ▼
setConfigurationComplete    control-transfer completion state machine
   SET_CONFIGURATION → GET_HID_DESCRIPTOR → GET_REPORT_DESCRIPTOR
   • parse report descriptor (USB_ProcessHIDReport)
   • open interrupt-IN endpoint
   • XamUserBindDeviceCallback → register virtual pad in XAM
   • queue first interrupt transfer
        │
        ▼
interruptHandler            fires on every inbound interrupt packet
   • per-device special cases (Switch Pro, DS3/Nintendo handshakes)
   • else HidFillButtonsReport(): descriptor + mapping → ButtonsReport
   • store into connectedControllers[i].currentState
        │
        ▼
XInputdReadStateHook        detour on XInputdReadState
   • translate ButtonsReport → XINPUT_GAMEPAD for any caller
```

Two supporting detours (`XamInputSetStateHook`, `XamInputGetCapabilitiesExHook`)
make the virtual pad report as connected/capable. `ButtonsReport` (in `usb.h`)
is the driver's internal normalized controller state — every code path's job is
to fill it, and `XInputdReadStateHook`'s job is to read it.

### Key field conventions in `ButtonsReport`

`XInputdReadStateHook` reads it like this, which is what every producer must match:

| ButtonsReport field | Meaning | Range |
|---|---|---|
| `x`, `y` | left stick X / Y | int16, up = positive |
| `z`, `rz` | right stick X / Y | int16, up = positive |
| `rx` | left trigger (analog) | 0..255 (falls back to `l2` digital) |
| `ry` | right trigger (analog) | 0..255 (falls back to `r2` digital) |
| `*_button`, `l1/r1`, `l3/r3`, `start/back`, `xbox` | digital buttons | 0/1 |
| `dpad_*` / `has_hat_switch`+`hatSwitch` | d-pad | 0/1 / hat enum |

---

## 2. Why Xbox One / Series controllers are different

Modern Xbox controllers do **not** speak USB HID. They use **GIP — Game Input
Protocol** (some people loosely call it "GID"). This breaks every assumption in
the HID path:

| | HID pads (DS4, Switch Pro, generic) | Xbox One / Series (GIP) |
|---|---|---|
| Interface class/sub/proto | `0x03 / 0x00 / 0x00` | `0xFF / 0x47 / 0xD0` (vendor) |
| Self-describing | yes — report descriptor | **no** descriptor at all |
| Needs activation | mostly no | **yes** — must be powered on |
| Input format | parsed HID reports | fixed GIP frames |

Because GIP carries no descriptor, the entire mapping system (`mapping.cpp`,
`HidFillButtonsReport`, the on-screen mapping assistant) does not apply. The
upside: a GIP pad *is* an Xbox pad, so its layout is fixed and a single
hardcoded decoder is exact — no mapping needed.

---

## 3. GIP wire format (wired USB)

### Power-on (host → device, interrupt OUT)

The pad stays silent until it receives a power-on command:

```
05 20 00 01 00
│  │  │  │  └─ payload: power state = ON
│  │  │  └──── payload length = 1
│  │  └─────── sequence number
│  └────────── flags (0x20 = internal/system)
└───────────── command 0x05 = power
```

### Input frame (device → host, interrupt IN), command `0x20`

GIP fields are **little-endian**; the console is big-endian, so they are read
byte-by-byte.

```
off 0 : 0x20 command
off 1 : flags
off 2 : sequence
off 3 : payload length (0x0E)
off 4 : buttons1  bit2 menu(Start) bit3 view(Back) bit4 A bit5 B bit6 X bit7 Y
off 5 : buttons2  bit0..3 dpad U/D/L/R  bit4 LB bit5 RB bit6 LS bit7 RS
off 6 : left trigger   uint16 LE 0..1023
off 8 : right trigger  uint16 LE 0..1023
off 10: left stick X   int16 LE
off 12: left stick Y   int16 LE
off 14: right stick X  int16 LE
off 16: right stick Y  int16 LE
```

### Guide button frame, command `0x07`

The guide/Xbox button arrives in its **own** frame, separate from `0x20`:

```
off 0 : 0x07 command
off 4 : bit0 = guide pressed
```

It must be **latched** and merged into the state produced by `0x20` frames,
because the two frame types interleave.

---

## 4. Implementation

New module `gip.h` / `gip.cpp` is self-contained protocol logic:

- `GipIsInterface(cls, sub, proto)` — identifies a GIP gamepad interface.
- `GIP_POWER_ON[5]` — the power-on packet.
- `GipDecodeInput(data, out, guideState)` — decode a `0x20` frame into a
  `ButtonsReport` (sticks as int16, triggers scaled 1023→255, guide merged in).
- `GipDecodeGuide(data)` — pull the guide bit out of a `0x07` frame.

Wiring in `main.cpp`:

1. **`Controller` struct** gains `uint8_t protocol` (`PROTO_HID`/`PROTO_GIP`) and
   `uint8_t gipGuide` (latched guide bit).
2. **`HidAddDeviceHook`** accepts the GIP interface in addition to HID, and tags
   the controller with `protocol`. GIP pads get `map = nullptr`.
3. **`setConfigurationComplete`** — for `PROTO_GIP`, after `SET_CONFIGURATION`
   it skips the HID descriptor stages entirely: opens the interrupt-IN endpoint,
   allocates the buffer, registers the pad in XAM, opens the interrupt-OUT
   endpoint and sends `GIP_POWER_ON`, then queues the first IN transfer.
4. **`interruptHandler`** — for `PROTO_GIP`, branches before the HID logic:
   `0x20` → `GipDecodeInput` into `currentState`; `0x07` → latch `gipGuide` and
   patch `currentState.xbox`.
5. `XInputdReadStateHook` is unchanged — it already turns `ButtonsReport` into
   XInput, and the GIP decoder fills the same fields.

The mapping-manager thread ignores GIP pads automatically, because it only
spawns the mapping assistant for controllers that have a parsed `reportInfo`,
which GIP pads never get.

Build files (`hiddriver.vcxproj`, `hiddriver.vcxproj.filters`) updated to
compile `gip.cpp` and include `gip.h`.

---

## 5. THE BLOCKER: device acquisition (resolved by research → confirmed NOT solved)

**`HidAddDevice` will not fire for a GIP controller.** This was the open
question; web research settled it:

- USB class drivers bind by interface class. The kernel HID class driver (the
  one whose add-device routine we hook) binds **only** to interface class
  `0x03`. The 360 controller itself is class `0xFF/0x5D/0x01` and is handled by
  a *separate* XUSB/gamepad driver — and that driver matches `0x5D/0x01`, not
  the GIP triple `0x47/0xD0`. A wired Xbox One/Series pad (`0xFF/0x47/0xD0`)
  therefore matches **no** driver on the console and lands on an unrecognized
  port (`UsbdTitleDriverSetUnrecognizedPort` / `...ResetAllUnrecognizedPorts`).
- Corroborated by community practice: the only documented way to use Xbox
  One/Series pads on a 360 today is a **hardware adapter** (e.g. Mayflash
  Magic-X/Magic-NS in XInput mode) that converts GIP into a native XInput
  controller. There is no known software-only GIP path. `hiddriver360` works
  for DS4/DualSense/Switch Pro precisely because those are HID-class (`0x03`)
  devices the HID driver *does* bind.

### What it would take to actually acquire the device

This is the real work, and it requires IDA/Ghidra on the exact dashboard kernel
(17559 retail / 17489 devkit) — it cannot be produced blind:

1. **Hook USB driver matching.** Find the `Usbd` routine that selects a driver
   for a freshly enumerated device (candidate: `UsbdGetRequiredDrivers`,
   ordinal 754, resolvable at runtime via `XexGetProcedureAddress` like the
   other USB functions). Detour it so a `0xFF/0x47/0xD0` device is claimed by
   the HID driver — then our existing `HidAddDeviceHook` fires for it.
2. **Or register our own driver object** via `UsbdRegisterDriverObject`
   (ordinal 755) matching the GIP class. Needs the driver-object struct layout.
3. **Or hook the unrecognized-port path** and drive enumeration ourselves.

Until one of these is in place, the GIP branch added in this PR is **dead code
on hardware** — correct, but never reached. The decode/init layer does not
change once acquisition is solved.

### Lower-risk items (only matter once acquisition works)

2. **Interrupt OUT endpoint** — confirm Xbox One/Series interface 0 actually
   exposes an interrupt OUT endpoint and that `UsbdGetEndpointDescriptor(..,
   INTERRUPT, OUT)` returns it. Power-on is sent there.

3. **Series / Xbox One S init** — some newer pads want a follow-up init frame
   (`05 20 00 0F 06`) after power-on. If a Series pad powers on but sends no
   input, send that as a second OUT packet (chain it from the power-on
   completion callback). Hook point is clearly marked in
   `setConfigurationComplete`.

4. **Endpoint packet size / frame length** — verify `0x20` frames fit in the
   allocated `pktSize*2` buffer and that offsets match on real captures.

5. **Sequence numbers** — only one OUT packet (power-on, seq 0) is sent today.
   If we later add rumble or multiple init frames, the sequence byte must
   increment per outbound packet.

---

## 6. Out of scope

- Xbox Wireless Adapter / dongle (this is wired USB only).
- Rumble (the base driver has no rumble support yet either).
- Authentication: retail Xbox One pads are not auth-gated over plain GIP input,
  but if a specific pad refuses to power on, an auth/announce handshake may be
  required — to be investigated only if hardware testing shows it's needed.
