# Analysis: why mouse look is broken in Rogue Trooper Redux

This document records how the cause was found, what is certain, what is inferred, and what is still
open. All addresses refer to the analysed build:

| | |
|---|---|
| File | `bin/x64/RTR_x64.exe`, 4 617 936 bytes |
| SHA-256 | `dedf29126a0c06bdd0249e1e56b7ce77e27bb687a6212faefda6ac50f303bbb8` |
| Steam | app 529490, build 2255271 |
| Image base | `0x140000000` (RVA = VA - `0x140000000`) |

Names like `fcn.140178860` are radare2 auto-names (VA). The game ships RTTI, so several functions
have class names (`RT_ClientActor_Player`, `Asura_MenuMouseInput`, ...).

## Summary

1. The mouse arrives through DirectInput 8 as relative counts. They reach the game's per-frame input
   code unchanged (verified, see [Evidence](#evidence)).
2. The game converts counts to a look value linearly: `look = counts * fps / 512`.
3. The look value is passed through a **gamepad stick response curve** (`fcn.140178860`). Horizontally
   that curve is `|x|^1.5` x 1.25 x a hold-to-turn ramp (up to x6), followed by a **clamp at +-7.5**.
4. Once the clamp is reached, a faster mouse movement no longer increases the turn rate. A flick
   that covers the same distance in less time therefore rotates the camera by *less* than a slow
   sweep. That is the reported "negative acceleration".
5. The fix replaces the curve's output by a linear mapping when the source is the mouse.
6. Menu cursor speed and the FOV are separate, smaller patches described below.

Item 4 is derived from the code. The camera angle itself was not measured, so the quantitative
effect is not verified, only the structure and the (subjective) improvement.

## Getting readable code

`.text` is encrypted on disk (entropy 8.00; the entry point is in the `.bind` section, which is the
Steam DRM wrapper "SteamStub"). Static analysis of the file shows nothing. The code is decrypted
by the wrapper at start-up, so:

1. Start the game.
2. Read its `.text` from `/proc/<pid>/mem` (needs root on systems with `ptrace_scope=1`):
   `base + 0x1000`, `0x2d11b3` bytes.
3. Write those bytes into a copy of the original EXE at the section's raw offset `0x400`
   (`.text` raw size `0x2d1200`). The result loads in radare2/Ghidra with its imports intact
   (the entry point is stale, which does not matter for analysis).
4. Analyse (`aaa`, then `r2ghidra` for decompilation).

Nothing from the game is redistributed; this project only contains offsets.

## Input path

| RVA | Role |
|---|---|
| `0x073010` | Input init. Calls `DirectInput8Create` (call at `0x0730b6`) |
| `0x0e9760` | Device setup, see below |
| `0x0e9910` | Mouse poll |
| `0x0ea570` | Per-frame input update (calls the poll, converts counts to look values) |
| `0x178860` | **Stick response curve** |
| `0x179130` | **The only call of the curve** (inside `RT_ClientActor_Player` look handling) |

Device setup (`0x0e9760`):

- `CreateDevice(GUID_SysMouse)`, `SetDataFormat` with a `DIDATAFORMAT` of `dwDataSize=20`, 11 objects
  (`c_dfDIMouse2`), relative axes.
- `SetCooperativeLevel(hwnd, 5)` = `DISCL_EXCLUSIVE | DISCL_FOREGROUND`.
- `SetEventNotification`, then `SetProperty(DIPROP_BUFFERSIZE) = 64` (buffered data), `Acquire`.

Mouse poll (`0x0e9910`): `GetDeviceData` with a 64-entry buffer. On `DI_OK` it sums the events with
`dwOfs == 0` (X) and `dwOfs == 4` (Y). On `DI_BUFFEROVERFLOW` (return value 1) it only calls
`GetDeviceState` for the buttons and **discards the movement of that frame**. The sums go into
accumulators (floats) at `0x459654` (X) and `0x459658` (Y).

Per-frame update (`0x0ea570`), in this order:

1. `lookX = accX * fps * (1/512)`, `lookY = accY * fps * (1/512)` (stored at `0x41cce4` / `0x41cce8`,
   i.e. entries 1 and 2 of a float array at `0x41cce0`), then the accumulators are cleared.
2. Poll again (fills the accumulators for the *next* frame).
3. Menu cursor update `fcn.1400a5360(dx, dy)`.

So the counts are used one frame late, scaled with the *next* frame's `fps` (`0x41cb98`, set by the
frame timer as `QueryPerformanceFrequency / delta`). The camera later multiplies by the frame time
(`0x41bbc4`, equal to `1/fps` in all samples), so the two factors cancel: **the look value is exactly
proportional to the counts, independent of frame rate.**

## The curve (`fcn.140178860`)

Arguments (Windows x64): `rcx = &x` (horizontal), `rdx = &y` (vertical), `xmm2` = factor for x,
`xmm3` = factor for y; stack: argument 6 = "mode is 2" byte, argument 8 = "mode is not 2" byte,
argument 9 = pointer to a float timer. The caller builds x and y from the array entries via
`fcn.140155120(mouseIndex, gamepadIndex)`, which returns the larger magnitude of the mouse and the
gamepad axis.

Reconstructed from the decompiler output (constants read from the running process):

```
x, y = *arg1, *arg2
if invertFlag (0x41fbfb):  x = -x
r2 = x*x + y*y
if mode != 2:  x = x * sqrt|x|   y = y * sqrt|y|      // |v|^1.5
else:          x = x * |x|       y = y * |y|          // v^2
if r2 > 0.99:  x = x * 1.25                            // constant at 0x354344
if mode != 2:                                          // hold-to-turn-faster ramp
    if r2 <= 0.99: t = 0
    else:          t = dtScaled + *timer
                   if t > 0.1: x = x * (1 + min((t - 0.1) * 5, 5))     // up to x6
    *timer = t
x = x * factor_x
*arg1 = sign(x) * min(|x|, 7.5)                        // clamp, constant at 0x3544b8
*arg2 = y * factor_y                                   // vertical: no clamp, no ramp
```

The caller then updates the camera: `angle += dtScaled * out * (+-1) / camera->field_0xa0`. The
meaning of `field_0xa0` was not determined.

Consequences for the mouse, with `look = counts_per_second / 512`:

- The horizontal output reaches the clamp (7.5) at `look = 3.3` without the ramp, and at
  `look = 1.0` once the ramp is fully charged (after about 1.1 s of continuous movement). At 800 DPI,
  `look = 3.3` is about 5.4 cm/s.
- Above that, the horizontal turn rate is constant. Vertical keeps growing (`|y|^1.5`), which also
  makes the two axes feel different.

## Why the fix is linear instead of "less clamped"

The curve is designed for a stick whose input is limited to about [-1, 1]. A mouse produces
unbounded values (a fast flick gave `look` of 42 in the measurements), so any stick-style curve will
saturate or explode. A linear mapping is the only choice that behaves the same at all speeds, and
it is what players expect from a mouse.

## The patches

Three call instructions are redirected. `call rel32` only reaches +-2 GB, but a Wine-loaded DLL sits
far away from the EXE, so every patch uses the same mechanism:

| What | Call site (RVA) | Original function (RVA) | Replacement |
|---|---|---|---|
| Look curve | `0x179130` | `0x178860` | `hook_look` |
| Menu mouse update | `0x29b39a` | `0x29a500` | `hook_menu_mouse` |
| FOV | `0x147525` | `0x147d40` | `hook_fov` |

1. Check that the host module is the expected game: valid PE header, `SizeOfImage` large enough for all
   call sites, and at each site byte `0xE8` with a `rel32` that lands on the expected function. If anything
   differs, patch nothing and log it.
2. Find a run of at least 16 `0xCC` bytes (alignment padding) in `.text` and write
   `FF 25 00 00 00 00` + 8-byte address of the hook there (`jmp [rip+0]`). Each patch gets its own run.
3. Rewrite the `rel32` of the call to point at that stub.

### Look: `hook_look`

Same Windows x64 signature as the curve (nine arguments, two of them `float` in `xmm2`/`xmm3`, five on
the stack). It calls the original first, because the original updates the ramp timer, then overwrites the
outputs when `lookX != 0 || lookY != 0` (the mouse is moving):
`out_x = in_x * factor_x * gain_x`, `out_y = in_y * factor_y * gain_y`. The invert flag is applied as in
the original. Inputs with `|value| > 150` are zeroed, because the mouse reports large one-frame jumps
(observed around start-up, values above 4000) that the old clamp used to swallow.

If the mouse and a gamepad stick are used at the same time, the array merge (`fcn.140155120`) picks the
larger one; the hook cannot tell which one won and linearises either.

### Menu: `hook_menu_mouse`

`fcn.14029a500` is the update of `Asura_MenuMouseInput` and moves the menu pointer, see
[Menu pointer](#menu-pointer). The hook runs it unchanged and then rescales the movement it caused.

### FOV: `hook_fov`

See the next section.

### DirectInput hooks

By patching COM vtable entries in place:

- `IDirectInput8::CreateDevice` (index 3), to recognise the system-mouse device.
- On that device `SetProperty` (index 6) and `GetDeviceData` (index 10). Both hooks are only installed in
  the optional `RTR_DINPUT_FIX` mode; otherwise the proxy does not touch DirectInput at all.

## Field of view

`fcn.140147d40(mode)` (RVA `0x147d40`) returns the **horizontal** FOV in radians as a float in the low
32 bits of `xmm0`. Its only caller (`0x147525`, inside the camera update `fcn.140147400`) divides the
result by a zoom factor (`camera->field_0xa0`, the same value that divides the mouse look speed), passes
it to `fcn.1400575c0`, which builds the projection values, and stores `1/tan(fov/2)` in a global.

Reconstructed (constants read from the running process):

```
a     = projection->aspect                                    // width/height, as set by fcn.1400d4cf0
deg   = a / 0.4444 * 12.5 + 25                                // 62.5 at 4:3, 75 at 16:9
if deg > 75:  deg = 75 + clamp(deg - 75, 0, 100) / 100 * 0.7 * 75     // compressed above 16:9
if mode == 2: the "extra" part is scaled by 0.66 or 0.33 depending on the weapon/camera
return deg * (pi / 180)
```

`fcn.1400575c0(fov, 1.0)` treats `fov` as horizontal: `tan(fov/2)` is combined with the aspect ratio
(`atan2(tan(fov/2), aspect)` is the vertical half-angle). So:

| Aspect | stock horizontal | stock vertical |
|---|---|---|
| 4:3 | 62.5 | 48.9 |
| 16:10 | 70.0 | 47.3 |
| 16:9 | 75.0 | 46.7 |
| 21:9 (3440x1440) | 84.0 | 41.3 |
| 32:9 | 101.2 | 37.8 |

These come from the formula above, not from in-game measurements. The vertical FOV shrinks as the screen
gets wider, which is the opposite of what a "Hor+" game should do.

`hook_fov` lets the user choose the horizontal FOV `h` (degrees) for the normal camera. To keep the game's
per-mode behaviour it does not return `h` directly but scales the stock result for the current mode:

```
result = stock(mode) * h / stock(0)           // stock(0) = the normal camera at the same aspect ratio
result = min(result, 150 degrees)              // tan(fov/2) explodes towards 180
```

`stock(0)` is obtained by calling the original with mode 0; for mode 0 the function only does arithmetic on
globals. With the normal camera `result = h` exactly, on every aspect ratio (checked in `tests/test_config.c`
for 4:3, 5:4, 16:10, 16:9, 21:9 and 32:9), so the setting means the same as in most games. The vertical FOV
follows from the aspect ratio. Modes that are narrower in the stock game stay proportionally narrower.

Not verified in the game: how each camera mode (cutscenes, vehicles, scopes) reacts to a larger FOV.

## Evidence

All measurements: Proton Experimental (Wine 11.0), niri (Wayland) with xwayland-satellite,
a 2.4 GHz wireless mouse, game at 144 FPS.

**Counts reach the game unchanged.** The raw HID reports of the mouse (read-only) were summed per
second and compared with the counts DirectInput delivered to the game, over about 80 seconds of
play. Total absolute counts: **54 688 in the game vs 54 227 from the hardware, ratio 1.009**. The
ratios of individual 5-second blocks scattered between 0.6 and 1.25 because the two logs could not be
aligned better than about a tenth of a second, but they showed no trend with speed (blocks ranged
from about 40 to 2 000 counts/s). Wine, Xwayland and the compositor are therefore very unlikely to
be the cause.

**The conversion to look values is linear.** In a trace of 3 159 frames with movement,
`lx(i+1) == dx(i) * fps(i+1) / 512` with a standard deviation of 0.0000 (and 0.021 if `fps(i)` is
used instead, which shows the one-frame delay).

## Hypotheses that were ruled out

- **DirectInput buffer overflow.** The poll discards a frame on `DI_BUFFEROVERFLOW`, so this looked like
  the culprit and a fix for it was built first. In the real game the log showed `overflow=0` in every
  second (the game polled 144 times per second and received about one event per frame). A synthetic
  test (`tests/run_wine_test.sh`) shows that overflow does happen when 200 events queue up, so
  `RTR_DINPUT_FIX` is kept as an optional safety net for very high polling rates, off by default.
- **Loss between mouse and game** (Wine, Xwayland, compositor): see the 1.009 ratio above.
- **Frame-rate dependence of the look value**: the factors cancel exactly, see above.

## Diagnostics

`dinput8-proxy.log` (next to the DLL) records what the proxy did at start-up (patches installed, or the
reason one was skipped) and every reload of the ini. There is no per-frame or per-poll mouse logging in the
release.

The measurements in [Evidence](#evidence) and the menu values below were taken with temporary
instrumentation that is not part of the shipped DLL: a per-poll trace of the counts and of a few game
variables (frame rate, look values, menu cursor and divisors, aspect ratio), read from inside the game
process, plus a read-only comparison with the raw HID reports of the mouse. To repeat such a measurement,
add a trace around `GetDeviceData` (see `RTR_DINPUT_FIX` mode for where the hook sits) and read the
variables at the RVAs listed above.

## Menu pointer

Report: in the menus the pointer moves faster horizontally than vertically (3440x1440); the user's estimate
was "vertical about half as fast". Fixed and confirmed in game.

**Cause.** `Asura_MenuMouseInput`'s update (`fcn.14029a500`, RVA `0x29a500`) keeps the pointer as a position in
`[0,1] x [0,1]`. For each axis it does `pos += dt * speed * axis` and clamps to `[0,1]`, where `axis` is the mouse
look value from the same float array as the gameplay look (`lx`/`ly`, `counts * fps / 512`) and `speed` is a single
field (`+0x28`) used for both axes. So each mouse count moves both normalised coordinates by the same amount. In
pixels that is `W/512 * speed` horizontally against `H/512 * speed` vertically: the pointer is `W/H` times faster
sideways (2.39 on 3440x1440, 1.78 on 16:9). The user's rough estimate fits.

**Layout** (relative to `rcx` of that function): `rcx-0x20` = pointer x, `rcx-0x1c` = pointer y.
The caller is `fcn.14029b370` (the per-frame menu mouse/keyboard update, reached from
`RT_MenuPCInput` and an `Asura_MenuMouseKeyboardInput` thunk). It saves the old position (`[rcx-0x30]`, `[rcx-0x2c]`
in its own frame), calls the function at RVA `0x29b39a` with `rcx -= 0x10`, and then evaluates the movement.

**Fix.** `hook_menu_mouse` replaces that call: it remembers `(x0, y0)`, runs the original, and sets
`pos = clamp01(pos0 + (pos_game - pos0) * gain)` per axis, with `gain_x = menu_gain_x` and
`gain_y = menu_gain_y * aspect` when `menu_equalize` is on (aspect = `width/height`, read from the game at
RVA `0x3d72f4`). With that, one mouse count moves the pointer the same number of pixels on both axes
(`tests/test_config.c` checks five screen sizes); the sideways speed is unchanged.

A second route into the same function exists through a vtable thunk (`Asura_MenuMouseInput.virtual_40_40`,
RVA `0x29c2cc`: adjusts `this`, then jumps to `0x29a500`; vtable slot at RVA `0x3521e8`). It is not patched; the
menu behaves correctly with only the direct call hooked.

**A wrong turn, for the record.** `fcn.1400a5360` (RVA `0x0a5360`) also tracks a "cursor" (`cx += dx / W`,
`cy += dy / H`, at VA `0x14041e06c` / `0x14041e068`, with `W=3440`, `H=1440` read from the running game). Scaling it
changed that internal value (verified by reading it from game memory) but nothing the user could see, even at a
factor of 0.05. It is not the pointer that is drawn. The analysis that pointed to `0x29a500` was: the game imports
no pointer functions (only `LoadCursorA`) and its window procedure handles no `WM_MOUSEMOVE`, so the pointer must
be driven by DirectInput values; the consumer of the look-value array that clamps to `[0,1]` is this function.

## Camera cut at scope zoom (not changed)

Report: when aiming a sniper rifle from cover, the view jumps when zooming out of the scope. It was there
before this patch.

Measured with a temporary hook on the call that ends the player look handler, `fcn.140147400(camera, ...)`
(call site RVA `0x179715`, one call per frame). The camera object's orientation is a unit quaternion at
`camera+0x18`; the mode int is at `camera+8` (0 normal, 1 zoomed) and the zoom factor at `camera+0xa0`
(1.0 / 3.0). Over one 17 s session with 14 zoom changes:

- Ordinary mouse movement turns the camera by at most about 6 degrees per update, around the quaternion's
  y axis (yaw).
- At zoom changes the camera sometimes turns by 32-43 degrees within one update (7 ms), around the x axis
  (pitch). The other zoom changes move it by 4-5 degrees.
- The big jump alternates: in one zoom cycle it is in the zoom-out, in the next one in the zoom-in. The
  camera sits at one of two fixed pitches. The zoomed view is almost level (about -3 to +3 degrees). The
  normal view looks 33-43 degrees down. After zooming out the camera returns to the pitch it had before
  the scope (the four returns measured 37.4, 43.1, 33.5 and 42.7 degrees), not to the pitch it had in the
  scope.
- Mouse input around the jumps is small (look value about 0.3), and the camera hardly moves inside the scope
  (1-7 degrees). The jump is therefore not caused by input or by the look curve.
- The camera's set-mode function `fcn.1401475f0` (RVA `0x1475f0`) stores the new mode at `camera+8` and sets
  a flag at `camera+0x68` when the mode differs. The camera update `fcn.140147400` clears the flag again,
  and the look handler copies the current orientation to `camera+0x28` while it is set. This is
  consistent with an intentional camera cut without interpolation, but the effect of the flag on rendering
  was not tested.
- The look handler also clamps the pitch per player state: -72..-18 degrees by default (constants at
  `0x14035474c` and `0x1403547a0`, -0.3142 and -1.2566 rad), -72..+72 in state 2 (`0x14035434c`), and -54..+36
  in the cover states 3 and 4 (`0x140354790`, `0x1403541bc`). The measured landing pitches (-33 to -43
  degrees) are inside the default range and not on its border, so the clamp is not the cause of the jump.

Not tried: blending the displayed orientation over a few frames, or keeping the scope pitch when leaving the
scope. A blend would have to know whether the camera position also jumps (not measured), and keeping the
pitch interacts with the per-state clamps above. Left unchanged by decision.

## Porting to another build

1. Decrypt and dump `.text` as above.
2. Find `DirectInput8Create`'s caller, then the poll (`GetDeviceData` through vtable offset `0x50`),
   then the function that multiplies the sums by `fps` and `1/512`.
3. FOV: find the function that computes `aspect / 0.4444 * 12.5 + 25` (constants `0.444444`, `12.5`, `25.0`,
   `75.0`) and its call; the menu pointer is `Asura_MenuMouseInput`'s update, which integrates the look-value
   array entries into a `[0,1]` position.
4. Find the consumer of the resulting array entries: the function with `sqrtf`, `1.25`, `0.99`
   and a clamp constant. Update the `RVA_*` constants at the top of `src/dinput8_proxy.c`.
