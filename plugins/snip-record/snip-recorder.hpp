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

private:
	OBSSourceAutoRelease FindOrCreateScene();
	obs_source_t *CreateCaptureSource(const SnipSelection &selection);

	bool active = false;
	OBSWeakSourceAutoRelease previousScene;
	OBSWeakSourceAutoRelease snipScene;
};
