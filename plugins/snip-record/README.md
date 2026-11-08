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

While a region recording runs, a thin red outline marks the recorded
area on screen (excluded from the capture itself). The last region is
remembered and pre-selected the next time the picker opens.

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

- Windows only (it builds on `win-capture` and Win32 window picking).
- Without "Match canvas", output resolution is your OBS canvas size:
  the picked area is scaled to fit the canvas without distortion, not
  rendered at native size.
- A region dragged across two displays is clamped to the display it
  ends on.
- While OBS is already recording something else, the picker opens but
  Record stays disabled — stop the current recording first.
- The first ~150 ms after pressing Record are not captured, so the
  overlay never appears in the video.
