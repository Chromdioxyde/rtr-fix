# Changelog

## 0.2.0

- **Field of view:** `fov_horizontal` sets the horizontal FOV in degrees for the normal camera, on any aspect
  ratio. The game's per-mode behaviour (e.g. narrower while aiming) is kept proportionally. The game's own
  horizontal FOV is about 62.5 / 75 / 84 / 101 degrees at 4:3 / 16:9 / 21:9 / 32:9, which makes the vertical
  FOV shrink on wide screens.
- **Menu pointer:** the game moves the menu pointer by the same fraction of the screen per mouse count on
  both axes, which makes it `width/height` times faster sideways than up/down in pixels (2.39 on 3440x1440).
  `menu_equalize` (default on) makes it equally fast on both axes; `menu_gain_x` / `menu_gain_y` scale each axis.
  Confirmed in game.
- The ini is now documented with all keys; new keys are optional (an older ini keeps working).
- Config parsing: an invalid value keeps the current setting instead of resetting it; decimal commas work.
- The patcher refuses to run in a host that is not the analysed game (no crash in other programs).
- The per-poll mouse diagnostics are removed from the DLL: `RTR_DINPUT_LOG`, `RTR_DINPUT_TRACE` and
  `RTR_MOUSE_LINEAR=2` no longer exist. `dinput8-proxy.log` keeps the start-up and ini-reload messages.
- Without `RTR_DINPUT_FIX` the proxy no longer hooks DirectInput at all.
- The FOV setting is implemented from the code analysis and unit-tested. Confirmed in normal gameplay on
  3440x1440 (looks good); cutscenes have not been checked yet.
- Investigated, not changed: the view jump when zooming a scope in or out in cover is a hard camera cut of
  the game (two fixed camera pitches), not mouse input. See `docs/ANALYSIS.md`, "Camera cut at scope zoom".

## 0.1.0

First public version.

- Linear mouse look replaces the gamepad-stick curve for mouse input (the cause of the "negative
  mouse acceleration"). Gamepad input is unchanged.
- Mouse speed via `dinput8-proxy.ini` (`gain_x`, `gain_y`), reloaded while the game runs.
- The patch is only applied if the game executable matches the analysed build; otherwise nothing is
  changed and the reason is logged.
- Optional `RTR_DINPUT_FIX` mode: merges buffered mouse events and enlarges the DirectInput buffer.
- Diagnostics: `RTR_DINPUT_LOG`, `RTR_DINPUT_TRACE`, `RTR_MOUSE_LINEAR=2`.
