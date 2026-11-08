# Snip Record

A Snipping Tool-style screen recorder that lives inside OBS. Press a
hotkey, pick a region, a window, or a full display, press Enter — OBS is
recording. Press the hotkey again to stop.

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

## Notes and limitations

- Windows only (it builds on `win-capture` and Win32 window picking).
- Output resolution is your OBS canvas size: the picked area is scaled
  to fit the canvas without distortion, not rendered at native size.
  Set your canvas to the size you want before a snip if that matters.
- A region dragged across two displays is clamped to the display it
  ends on.
- While OBS is already recording something else, the picker opens but
  Record stays disabled — stop the current recording first.
- The first ~150 ms after pressing Record are not captured, so the
  overlay never appears in the video.
