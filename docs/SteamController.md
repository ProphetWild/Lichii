# Steam Controller

Lichii drives Steam Controllers (the 2015 model and the 2026 model) directly, without the Steam client. Both models appear in **Settings → Controls** as a gamepad with their buttons, sticks, trackpads and gyro, and Switch HD Rumble plays on their haptics with its pitch.

## Turning it on

**Settings → Controls → Advanced → Other** has two switches, both on by default:

| Setting | Config key | Effect |
|---|---|---|
| Enable direct Steam Controller driver | `enable_steam_controller_driver` | SDL's HIDAPI Steam Controller drivers open the controller. While Lichii holds it, the controller's keyboard and mouse ("lizard") mode is off. Takes effect after a restart. |
| Use RenHDrum rumble translation for Steam Controllers | `steam_controller_hd_rumble` | HD Rumble is sent to the controller's haptic actuators as tones. When off, the controller gets plain SDL rumble (on/off for the 2026 model, nothing for the 2015 model). |

Steam can stay open: launch Lichii directly, not as a non-Steam game, and SDL still opens the controller natively. While Lichii's window has focus, though, Steam keeps applying its **Desktop Layout** (A = Enter, B = Escape, right trackpad = mouse). Those keys land in Lichii, and Escape exits fullscreen. To stop that, turn Steam Input off for the Desktop Layout under **Steam → Settings → Controller → Desktop Layout → Edit (gear) → Disable Steam Input**. The controller then stops acting as a mouse on the desktop as well, and you can switch it back from the same menu. If you prefer Steam Input instead, turn the driver off and launch Lichii through Steam. The controller then shows up as Steam's virtual gamepad.

Lizard mode comes back by itself after Lichii exits. SDL repeats the "lizard off" command every 3 s. The 2026 controller's firmware watchdog turns lizard mode back on about 10 s after the last one. SDL restores it on the 2015 controller as soon as it closes the device.

## What SDL provides

The bundled SDL (3.4.14, `cpmfile.json` `sdl3-ci`) has both HIDAPI drivers built in:

| Model | USB ids (VID 28DE) | SDL driver |
|---|---|---|
| 2015 | 1102 wired, 1142 receiver, 1105/1106 Bluetooth | `SDL_hidapi_steam.c` |
| 2026 | 1302 wired, 1303 Bluetooth, 1304 Puck, 1305 Steam Machine receiver | `SDL_hidapi_steam_triton.c` |

Both are switched by `SDL_HINT_JOYSTICK_HIDAPI_STEAM`, which now follows the setting above. Nothing else is needed for input. For the 2026 model, SDL reports 22 buttons, 6 axes, a hat, gyro, accelerometer and both trackpads.

## Mapping the 2026 controller

Auto-mapping works like any SDL gamepad: face buttons by position (Switch A is the right face button), View and Menu are − and +, the Steam button is Home and the `…` (QAM) button is Screenshot. Everything else can be mapped by hand:

| SDL button | Control | SDL button | Control |
|---|---|---|---|
| 11 | `…` (QAM) | 17 | Left trackpad click |
| 12 | R4 (upper right back button) | 16 | Right trackpad click |
| 13 | L4 (upper left back button) | 19 / 18 | Left / right stick touch |
| 14 | R5 (lower right back button) | 21 / 20 | Left / right grip touch |
| 15 | L5 (lower left back button) | | |

Each trackpad is also reported as a pair of axes: **axes 6/7** for the left pad and **8/9** for the right, centred when no finger is on the pad. To use a trackpad as a stick, click the stick in the mapping dialog and slide a finger across the pad, first sideways and then up and down. Gyro is mapped from Motion like any other controller.

## HD Rumble translation

`src/input_common/helpers/steam_controller_haptics.{h,cpp}`

A Switch actuator gets two bands per side: an amplitude and a frequency for each of the low and high bands. The Steam Controller can play one tone per actuator, so each emulated side becomes one tone:

- **Pitch**: the bands' log frequencies, weighted by their energy. A single active band passes through unchanged. Two equal bands at 160 and 320 Hz give 226 Hz.
- **Loudness**: the combined RMS amplitude, √(low² + high²), capped at 1. This becomes a dB gain, `20·log10(a)`, between −40 and 0 dB. 0 dB is the actuator's unity level. Below 0.004 the side is silent.
- **Sides**: the emulated left Joy-Con actuator drives the controller's left side and the right one drives the right. Before this change, both sides overwrote one plain rumble call. `Common::Input::VibrationStatus` now carries a `side`, set by `EmulatedController::SetVibration`.

The 2026 model is driven through output report `0x83` (`MsgHapticLfoTone`: side, gain dB, frequency, duration, LFO rate, LFO depth) on the grip actuators. Side code 3 is the left grip, 4 the right grip and 5 both. When both sides want the same tone, a single report goes out. A tone is sent with a 2 s duration and is only re-sent when it changes by at least 1 dB or 3 %, or once a second while the game keeps updating it. A side the game stops updating therefore goes quiet after 2 s, which is the same limit as the plain SDL rumble path. A stop is a `0x83` report at −128 dB. SDL has no call for these reports, so they go through a second, shared HID handle on the same device.

The 2015 model is driven through feature report `0x8F` (`MsgFireHapticPulse`) via `SDL_SendGamepadEffect`, with one pulse per period of the tone. The on-time carries the loudness, like a PWM duty cycle. A 60 ms pulse train is re-armed every 40 ms while the side is active.

All writes happen on a worker thread with latest-value-wins pacing: 8 ms between reports on USB and the Puck, and 16 ms on Bluetooth. A slow wireless link therefore never stalls emulation.

Without the translation, the 2026 model's plain rumble (report `0x80`) plays a fixed-gain 65 Hz carrier on the grips. The motor "speed" values only change its modulation rate, so it is effectively on/off. The 2015 model has no plain rumble in SDL at all.

### Sources and licences

- **RenHDrum** (https://github.com/trzz1214/RenHDrum, Copyright (c) 2026 Three-Dola, MIT OR Apache-2.0): its Steam Controller design (`docs/research/08-steam-controller-haptics.md`) gives the approach used here: one tone per side picked from the dominant band, frequency in Hz with a dB gain on the 2026 model, pulse period ≈ 1/f on the 2015 model, and paced writes. RenHDrum has not implemented its Steam Controller path yet, so there was no code to port. The C++ here is new and follows that design, extending dominant-band selection to an energy-weighted pitch.
- **SDL** (zlib): report ids and struct layouts in `src/joystick/hidapi/steam/controller_structs.h`. The 2026 controller's own HID report descriptor confirms report `0x83` with a 9-byte payload.
- **sc26re** (https://github.com/mwdmwd/sc26re, AGPL-3.0, a reimplementation of the 2026 controller's firmware) and **PadForge** (https://github.com/hifihedgehog/PadForge): read for protocol facts only. These are the `0x83` side codes (0/1 trackpads, 3/4 grips, 5 both grips), 0 dB as unity, the −128 dB stop form and how report `0x80` is rendered. No code was copied.

## Not yet verified on hardware

- **Steam closed.** Detection, mapping and lizard mode were not tried with the Steam client fully closed. With Steam running, the Puck-connected 2026 controller opens natively as "Steam Controller" (PID 1304) with gyro, accelerometer, two trackpads and rumble. Upstream SDL testers report the same with Steam closed. The log now records `Opened Steam Controller (PID 1304) through SDL HIDAPI, natively` when it works.
- **Haptic feel.** The 2026 tone reports are accepted by the device (writes succeed through the Puck), and the encoder matches the byte layouts above. Whether the grips feel right, how loud 0 dB is, and whether retuning (gain offset, frequency range, resend steps) helps are still open questions.
- **2015 model, wired 2026 model and Bluetooth.** None of these were tried. Only the 2026 model through the Puck was connected.
