#pragma once

#include <obs.hpp>

#include <atomic>

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

	// Window-mode starts wait for the capture source to hook its
	// window before recording begins. Cancels such a pending start.
	void CancelPendingStart();
	// Called (on the UI thread) when the capture hooked, or when the
	// wait timed out and recording should start anyway.
	void BeginRecording();

	// Optional: resize the OBS canvas (base + output) to the picked
	// area for the duration of the snip, then restore it. On by
	// default until the user turns it off; persisted by the module's
	// save callback.
	// From the picker checkbox: a deliberate user choice.
	void SetMatchCanvas(bool match)
	{
		matchCanvas = match;
		matchCanvasUserSet = true;
	}
	// From persisted settings at load: not a user choice unless the
	// stored setting says the user made one.
	void LoadMatchCanvas(bool match, bool userSet)
	{
		matchCanvas = match;
		matchCanvasUserSet = userSet;
	}
	bool MatchCanvas() const { return matchCanvas; }
	bool MatchCanvasUserSet() const { return matchCanvasUserSet; }

	// Restores the pre-snip canvas size if one is pending (after the
	// recording stopped, or at startup after a crash mid-snip).
	void RestoreCanvasIfPending();

private:
	OBSSourceAutoRelease FindOrCreateScene();
	obs_source_t *CreateCaptureSource(const SnipSelection &selection);
	bool MaybeResizeCanvas(int width, int height);
	void DeferStartUntilHooked(obs_source_t *capture);
	void DisconnectPendingHook();

	bool active = false;
	std::atomic<bool> pendingStart{false};
	OBSWeakSourceAutoRelease pendingCapture;
	bool matchCanvas = true;
	bool matchCanvasUserSet = false;
	bool canvasResized = false;
	int originalBaseCX = 0;
	int originalBaseCY = 0;
	int originalOutputCX = 0;
	int originalOutputCY = 0;
	OBSWeakSourceAutoRelease previousScene;
	OBSWeakSourceAutoRelease snipScene;
};
