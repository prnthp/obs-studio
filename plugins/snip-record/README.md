# Snip Record

A Snipping Tool-style screen recorder that lives inside OBS. Press a
hotkey, pick a region, a window, or a full display, press Enter — OBS is
recording. Press the hotkey again to stop.

## "OBS for Dummies" simple mode

On launch, a small **OBS for Dummies** window replaces the main OBS
window (OBS itself keeps running in the tray): **Start Snipping**,
**Open Recording Folder**, and **Show Full OBS Interface** — the full
UI is always one click away. The window also has the snip shortcut
field (default `Alt+Shift+S`), a "tray icon click stops recording"
toggle, and a "show this window at startup" toggle.

First-launch defaults (only values never set by the user): tray icon
enabled, start minimized to tray, minimize to tray instead of the
taskbar, tray click stops recording, recordings as MP4 into the
system Videos folder. All of these remain changeable in OBS Settings.

The picker also has **Desktop audio**, **Microphone**, and **Mouse
pointer** checkboxes. Unchecking audio detaches OBS's global audio
devices (desktop channels 1-2, mic channels 3-6) for that snip only;
they are restored when the recording stops.

**Also export** (picker, GIF by default) converts each finished MP4
in the background - the MP4 is always kept as the master. GIF uses
the FFmpeg libraries OBS ships (decode, palette generation, encode;
capped at 960 px wide / 15 fps), or gifski when found on PATH. WebP
goes through a user-installed ffmpeg with libwebp, since OBS's
FFmpeg build has no WebP encoder. The launcher window shows export
progress/results.

## Usage

1. Bind the hotkey: **Settings → Hotkeys → "Snip Record: open picker /
   stop recording"** (for example `Ctrl+Shift+R`). Until then, use
   **Tools → Snip Record…** — it is the same action.
2. Press the hotkey. A dimmed overlay covers the desktop:
   - **Region** (default): drag to select an area.
   - **Window**: move to highlight a window, click to select it.
   - **Full Screen**: click a display (or press Enter for the primary one).
   - Keys `1` / `2` / `3` switch modes, `Enter` starts recording,
     `Esc` cancels. Double-click also starts.
3. Recording starts immediately, using your current OBS profile —
   encoder, format, frame rate, and recording folder are unchanged.
4. Stop with the same hotkey, or with OBS's normal Stop Recording button.

## How it works

The plugin keeps a dedicated scene named **Snip Record** in the current
scene collection. On each snip it clears that scene, adds one capture
source (`monitor_capture` for region/full-screen, `window_capture` for a
window), crops and fits it to your canvas, switches to the scene, and
calls `obs_frontend_recording_start()`. When the recording stops, you are
switched back to the scene you were on before.

Nothing about your existing scenes, sources, or profile is modified —
the only footprint is the extra `Snip Record` scene.

While a region recording runs, a thin red outline marks the recorded
area on screen (excluded from the capture itself). In window mode the
outline follows the recorded window as it moves and resizes; window
recordings wait for the capture to hook its window before the first
frame is recorded. The last region is remembered and pre-selected the
next time the picker opens.

Two more toolbar options, both remembered across restarts:
**Start instantly after selecting** records as soon as a region drag
ends, a window is clicked, or a display is clicked, without waiting
for Enter. **After recording** can open the finished video, show it
selected in Explorer, or do nothing (default).

## Optional: match canvas to selection

The picker's **Match canvas to selection** checkbox temporarily resizes
the OBS canvas (base and output) to the picked area — rounded down to
even dimensions, minimum 32×32 — so the file comes out at exactly the
selection's size. The original canvas size is restored when the
recording stops; it is saved to the profile *before* resizing, so even
a crash mid-recording is undone on the next OBS start. It is skipped
while streaming. It is on by default; uncheck it if you don't want
your global video settings touched, and that choice is remembered.

## Notes and limitations

- Windows and macOS 13+. On Windows it builds on `win-capture` and
  Win32 window picking; on macOS on the ScreenCaptureKit
  `screen_capture` source in `mac-capture` (window picking via
  `CGWindowList`, displays via `display_uuid`, the recording outline
  excluded with `NSWindowSharingNone`). macOS requires Screen
  Recording permission for OBS (System Settings → Privacy &
  Security → Screen Recording); without it window titles are empty
  and captures stay black. There is no `.iss` installer on macOS —
  the plugin ships inside the normal OBS `.app`/DMG.
- Without "Match canvas", output resolution is your OBS canvas size:
  the picked area is scaled to fit the canvas without distortion, not
  rendered at native size.
- A region dragged across two displays is clamped to the display it
  ends on.
- While OBS is already recording something else, the picker opens but
  Record stays disabled — stop the current recording first.
- The first ~150 ms after pressing Record are not captured, so the
  overlay never appears in the video.
