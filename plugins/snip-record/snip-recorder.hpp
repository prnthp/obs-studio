#pragma once

#include <obs.hpp>

#include "snip-selection.hpp"

// Builds (and reuses) a dedicated "Snip Record" scene holding a single
// capture source for the picked region/window/display, switches to it and
// starts recording with the current OBS profile/output settings.
// When that recording stops, the previously active scene is restored.
class SnipRecorder {
public:
	// Must be called on the UI thread. Returns false (and changes
	// nothing) if OBS is already recording or the capture source for
	// the requested mode is unavailable.
	bool StartRecording(const SnipSelection &selection);

	// Called from the frontend event callback on RECORDING_STOPPED.
	void HandleRecordingStopped();

	bool IsActive() const { return active; }

	// Optional: resize the OBS canvas (base + output) to the picked
	// area for the duration of the snip, then restore it. Off by
	// default; persisted by the module's save callback.
	void SetMatchCanvas(bool match) { matchCanvas = match; }
	bool MatchCanvas() const { return matchCanvas; }

	// Restores the pre-snip canvas size if one is pending (after the
	// recording stopped, or at startup after a crash mid-snip).
	void RestoreCanvasIfPending();

private:
	OBSSourceAutoRelease FindOrCreateScene();
	obs_source_t *CreateCaptureSource(const SnipSelection &selection);
	bool MaybeResizeCanvas(int width, int height);

	bool active = false;
	bool matchCanvas = false;
	bool canvasResized = false;
	int originalBaseCX = 0;
	int originalBaseCY = 0;
	int originalOutputCX = 0;
	int originalOutputCY = 0;
	OBSWeakSourceAutoRelease previousScene;
	OBSWeakSourceAutoRelease snipScene;
};
