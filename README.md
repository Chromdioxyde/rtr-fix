# rtr-mousefix

Mouse and field-of-view fixes for **Rogue Trooper Redux** (Steam, PC).

| Fix | What it does | Status |
|---|---|---|
| **Mouse look** | Replaces the gamepad-stick curve that caps fast flicks with a linear one; adjustable speed | Confirmed in game |
| **Menu pointer** | The menu pointer moves equally fast in pixels on both axes (the game moves it aspect-ratio times faster sideways); per-axis speed | Confirmed in game |
| **Field of view** | Choose the horizontal FOV in degrees; works on every aspect ratio (4:3 to 32:9) | New in 0.2.0. Confirmed in normal gameplay (3440x1440); cutscenes not yet checked |

In the stock game, mouse look is fed through a gamepad-stick response curve that ends in a hard
cap. Past a certain speed, a **faster flick turns the camera less than a slow sweep over the same
distance** ("negative mouse acceleration"). The game has no option to turn this off, and players have
been asking for a fix since 2017 (see [this Steam thread](https://steamcommunity.com/app/529490/discussions/0/1621726179573537787),
comments up to 2023, no fix). The game's text tables contain no FOV setting, and on wide screens the
game makes the vertical FOV *smaller*.

`rtr-mousefix` is a small `dinput8.dll` proxy with a live-reloaded config file. It does not modify
`RTR_x64.exe` on disk.

> **Disclaimer.** Unofficial, not affiliated with or endorsed by Rebellion. This software is provided
> **"as is", without warranty of any kind** (see [LICENSE](LICENSE)); use it at your own risk. It was only
> used in single player. I have not checked how it interacts with the multiplayer/online features, so
> don't use it there.
>
> **Antivirus warning.** `dinput8.dll` is a proxy DLL that is loaded by the game in place of the system
> one and patches the game's code in memory. Antivirus programs often flag such DLLs as suspicious
> (false positives), especially on Windows. The full source is in this repository and the DLL can be
> built from it (see [Build from source](#build-from-source)); the release checksums are in
> `SHA256SUMS`. If you do not trust a binary, build it yourself.
>
> **Windows is untested.** It was developed and tested only on Linux with Proton. The DLL is plain Win32
> and should work on Windows, but nobody has tried it yet.

## Download

| File | For | Contents |
|---|---|---|
| `rtr-mousefix-<version>.zip` | **Everyone.** Nothing to compile | `dinput8.dll`, example `dinput8-proxy.ini`, `INSTALL.txt` (step by step), changelog, license |
| `rtr-mousefix-<version>-source.zip` | Developers | Everything above plus sources, tests, build scripts and `docs/ANALYSIS.md` |

If you just want to use it, get the first one and follow `INSTALL.txt`.

## Status

| | |
|---|---|
| Tested | Steam version, `RTR_x64.exe` SHA-256 `dedf29126a0c06bdd0249e1e56b7ce77e27bb687a6212faefda6ac50f303bbb8` (4 617 936 bytes, Steam build 2255271) |
| Tested on | Linux (CachyOS, niri/Wayland), Proton Experimental (Wine 11.0), 144 Hz, one 2.4 GHz wireless mouse |
| Mouse look | Confirmed in game ("very nice"); the default strength was halved after a first, too strong test. No camera-angle measurement was done, so the effect is derived from the code and judged by feel |
| Menu pointer | Confirmed in game ("works"). Found by reading the menu mouse code; unit-tested on five screen sizes |
| Field of view | Implemented from the code analysis and unit-tested (the maths is checked on six aspect ratios). Confirmed in normal gameplay on 3440x1440. **Cutscenes not yet checked.** Please report how it behaves |
| Not tested | Native Windows (the DLL is plain Win32 and should work), other game builds, GOG/other stores, other Proton/Wine versions |

If the game is a different build, the proxy detects that and patches nothing (it logs
`call site does not match`), so the game just behaves like stock.

## Install

### Linux / Steam (Proton)

1. Copy `dinput8.dll` next to the game executable:
   `.../steamapps/common/Rogue Trooper Redux/bin/x64/`
2. In Steam: right-click the game, **Properties, Launch Options**, and set

   ```
   WINEDLLOVERRIDES="dinput8=n,b" %command%
   ```

   (Proton only loads a native DLL from the game folder with this override. With extra tools,
   e.g. MangoHud: `WINEDLLOVERRIDES="dinput8=n,b" mangohud %command%`.)
3. Start the game. On first start the proxy creates `dinput8-proxy.ini` next to the DLL.

### Windows (untested, see the disclaimer above)

Copy `dinput8.dll` into `bin\x64\` next to `RTR_x64.exe`. No launch options are needed.

### Uninstall

Delete `dinput8.dll`, `dinput8-proxy.ini` and `dinput8-proxy.log` from `bin/x64/`, and remove the
launch option. The game executable was never changed.

## Configure

Edit `dinput8-proxy.ini` (next to the DLL). **Changes apply while the game is running**, within about
a second; no restart needed. That includes the field of view. A decimal point or comma both work, and an
invalid value is ignored (the current setting stays).

```ini
gain_x=0.5          ; mouse look, horizontal
gain_y=0.5          ; mouse look, vertical
menu_equalize=1     ; menu pointer: equal pixel speed on both axes (0 = the game's own behaviour)
menu_gain_x=1.0     ; menu pointer speed, horizontal
menu_gain_y=1.0     ; menu pointer speed, vertical
fov_horizontal=0    ; horizontal field of view in degrees, 0 = leave the game alone
```

| Key | Default | Range | Meaning |
|---|---|---|---|
| `gain_x`, `gain_y` | 0.5 | 0.05 to 20 | Mouse look speed. Higher = faster. The author ended up at 0.3 (depends on your mouse DPI). The game's own SENSITIVITY X/Y options still multiply on top |
| `menu_equalize` | 1 | 0 or 1 | The game moves the menu pointer by the same fraction of the screen per mouse movement on both axes, so on a wide screen it travels more pixels sideways than up/down (about 2.4 times on 21:9, 1.8 on 16:9). With 1 the pointer is equally fast on both axes (the sideways speed stays as it is, the vertical speed is raised). 0 = the game's behaviour |
| `menu_gain_x`, `menu_gain_y` | 1.0 | 0.05 to 20 | Extra speed factor per axis for the menu pointer, on top of `menu_equalize` |
| `fov_horizontal` | 0 (off) | 0, or 30 to 150 | The horizontal FOV you want for the normal camera, in degrees, the way most games do it. Try 90 to 110 |

About `fov_horizontal`: the game computes its own *horizontal* FOV from the aspect ratio (about 62.5 degrees
at 4:3, 75 at 16:9, 84 at 21:9, 101 at 32:9) and derives the vertical one from it, so the vertical FOV shrinks on
wide screens (roughly 49 / 47 / 41 / 38 degrees). With `fov_horizontal=100` the normal camera gets exactly 100
degrees horizontally on any screen; the vertical FOV follows from your aspect ratio, as in most games. Camera modes
that are narrower in the stock game (for example while aiming) stay proportionally narrower. The result is capped at
150 degrees. (The stock values are computed from the game's formula, not measured in game.)

## Options (environment variables, all optional)

Put them in the launch options before `%command%`, e.g. `RTR_MOUSE_LINEAR=0 %command%`.

| Variable | Default | Meaning |
|---|---|---|
| `RTR_MOUSE_LINEAR` | `1` | Master switch. `1` = all fixes on. `0` = everything off (pure pass-through, stock behaviour) |
| `RTR_MOUSE_GAIN` | `0.5` | Initial look strength for both axes. `dinput8-proxy.ini` overrides it |
| `RTR_DINPUT_FIX` | off | `1` = merge buffered mouse events and enlarge the DirectInput buffer. Not needed with ordinary mice (see [docs](docs/ANALYSIS.md#hypotheses-that-were-ruled-out)); a safety net for very high polling rates |

`dinput8-proxy.log` records what the proxy did at start-up (which patches were installed, or why one was
skipped) and every reload of the ini. Attach it when reporting a problem.

## What it does, in one paragraph

Three call instructions in the game are redirected to small replacement functions; the originals are
still called where that matters. **Look:** the game sends mouse look through a stick curve (horizontally
`|x|^1.5`, a hold-to-turn-faster multiplier that grows to x6, then a hard clamp at +-7.5). When the mouse
is the source, the result is overwritten with `input * sensitivity * gain`; values above 150 are dropped
(one-frame jumps seen around start-up). **Menu:** the menu mouse update runs unchanged, then the pointer movement it caused is rescaled per axis
(vertical by the aspect ratio, so one mouse count moves the pointer the same number of pixels on both axes). **FOV:** the horizontal FOV the game computes is replaced by `fov_horizontal` (modes that are
narrower in stock stay proportionally narrower). Gamepad input is not touched. Technical details, addresses and evidence are in
[docs/ANALYSIS.md](docs/ANALYSIS.md).

## Known issues

- The menu pointer fix is derived from the menu mouse code and confirmed on one setup (3440x1440). Other
  aspect ratios use the same formula (aspect read from the game) but have not been tried.
- The vertical "hold-to-turn-faster" ramp and the horizontal clamp no longer apply to the mouse.
  That is the point of the fix, but it means very fast flicks now really turn far.
- `fov_horizontal` applies to all camera modes, including cutscene cameras. A wider FOV can show parts of a
  scene the developers never intended to be seen. Report anything odd, and use `fov_horizontal=0` to compare.
- In cover, when zooming a scope in or out, the view can jump by roughly 30-45 degrees in pitch within one
  frame. This is a hard camera cut in the game itself (two fixed camera pitches), not mouse input, and it
  happens with and without this patch. It is not changed. See `docs/ANALYSIS.md`, "Camera cut at scope
  zoom".
- If the game is updated and the code moves, the fixes silently stop working until the addresses
  are updated (the log says so).

## Build from source

Needs `clang`, `lld`, and the mingw-w64 headers and CRT (Arch: `pacman -S clang lld mingw-w64-headers mingw-w64-crt`).

```sh
./build.sh                 # -> build/dinput8.dll
tests/run_tests.sh         # config parser and FOV maths unit tests
PROTON_FILES="$HOME/.local/share/Steam/steamapps/common/Proton - Experimental/files" tests/run_tests.sh   # + Wine test
./release.sh               # -> dist/rtr-mousefix-<version>.zip and -source.zip
```

The Wine test needs an X display and no mouse movement while it runs. It checks that the optional
event merging works when 200 events overflow the 64-entry DirectInput buffer, and that the proxy
does nothing (and does not crash) when it is loaded by a program that is not the game.

The DLL is built without a C runtime (kernel32 and user32 only), so it has no extra dependencies.

## Layout

```
src/dinput8_proxy.c   the proxy (hooks, patches, logging)
src/config.h          dependency-free ini parser and FOV scaling (shared with the unit test)
src/dinput8.def       exported symbol list
tests/                unit tests (host) and Wine integration test
docs/ANALYSIS.md      reverse-engineering notes, addresses, evidence
INSTALL.txt           step-by-step guide for users (shipped in both zips)
build.sh, release.sh  build and packaging
```

## License

MIT, see [LICENSE](LICENSE). The release contains no game code; it only contains offsets and patch
routines that run inside your own copy of the game.
