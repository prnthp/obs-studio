#include "snip-recorder.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <string>
#include <vector>

namespace {

const char *kSceneNameKey = "SnipRecord.Scene";
const char *kSourceNameKey = "SnipRecord.Source";

bool IsSceneSource(obs_source_t *source)
{
	const char *id = obs_source_get_id(source);
	return id && std::string(id) == "scene";
}

// Finds a source name that is either free or already used by a source of
// the wanted kind, so repeated snips reuse "Snip Capture" instead of
// piling up "Snip Capture 2", "Snip Capture 3", ...
std::string FindReusableName(const char *base, bool wantScene, bool &foundExisting)
{
	foundExisting = false;
	for (int i = 1; i < 100; i++) {
		std::string candidate = i == 1 ? base : std::string(base) + " " + std::to_string(i);
		OBSSourceAutoRelease existing = obs_get_source_by_name(candidate.c_str());
		if (!existing) {
			return candidate;
		}
		if (IsSceneSource(existing) == wantScene) {
			foundExisting = true;
			return candidate;
		}
	}
	return base;
}

void ClearSceneItems(obs_scene_t *scene)
{
	std::vector<obs_sceneitem_t *> items;
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			obs_sceneitem_addref(item);
			static_cast<std::vector<obs_sceneitem_t *> *>(param)->push_back(item);
			return true;
		},
		&items);

	for (obs_sceneitem_t *item : items) {
		obs_sceneitem_remove(item);
		obs_sceneitem_release(item);
	}
}

void FitItemToCanvas(obs_sceneitem_t *item)
{
	obs_video_info videoInfo = {};
	if (!obs_get_video_info(&videoInfo) || videoInfo.base_width == 0 || videoInfo.base_height == 0) {
		return;
	}

	vec2 bounds = {float(videoInfo.base_width), float(videoInfo.base_height)};
	vec2 center = {bounds.x / 2.0f, bounds.y / 2.0f};
	obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
	obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
	obs_sceneitem_set_bounds_crop(item, false);
	obs_sceneitem_set_bounds(item, &bounds);
	obs_sceneitem_set_pos(item, &center);
}

} // namespace

OBSSourceAutoRelease SnipRecorder::FindOrCreateScene()
{
	bool foundExisting = false;
	const std::string name = FindReusableName(obs_module_text(kSceneNameKey), true, foundExisting);
	if (foundExisting) {
		return OBSSourceAutoRelease(obs_get_source_by_name(name.c_str()));
	}

	OBSSourceAutoRelease sceneSource = obs_source_create("scene", name.c_str(), nullptr, nullptr);
	if (!sceneSource) {
		blog(LOG_ERROR, "[snip-record] Failed to create scene '%s'", name.c_str());
	}
	return sceneSource;
}

obs_source_t *SnipRecorder::CreateCaptureSource(const SnipSelection &selection)
{
	OBSDataAutoRelease settings = obs_data_create();
	const char *sourceId = nullptr;

	switch (selection.mode) {
	case SnipSelection::Mode::Window:
		sourceId = "window_capture";
		obs_data_set_string(settings, "window", selection.windowValue.c_str());
		obs_data_set_int(settings, "priority", 0); // match by class first; title/exe still recorded
		obs_data_set_bool(settings, "cursor", true);
		obs_data_set_bool(settings, "client_area", true);
		break;
	case SnipSelection::Mode::Region:
	case SnipSelection::Mode::FullScreen:
		sourceId = "monitor_capture";
		obs_data_set_string(settings, "monitor_id", selection.monitorId.c_str());
		obs_data_set_bool(settings, "capture_cursor", true);
		break;
	}

	bool foundExisting = false;
	const std::string name = FindReusableName(obs_module_text(kSourceNameKey), false, foundExisting);
	// A leftover source with this name can only be in the snip scene,
	// whose items were just cleared, so `foundExisting` should be false.
	// If it is not (the user moved/renamed it elsewhere), abort rather
	// than touching user content.
	if (foundExisting) {
		blog(LOG_ERROR, "[snip-record] Source name '%s' is already used outside the snip scene", name.c_str());
		return nullptr;
	}
	obs_source_t *source = obs_source_create(sourceId, name.c_str(), settings, nullptr);
	if (!source) {
		blog(LOG_ERROR, "[snip-record] Failed to create capture source '%s' (is win-capture loaded?)",
		     sourceId);
	}
	return source;
}

bool SnipRecorder::StartRecording(const SnipSelection &selection)
{
	if (obs_frontend_recording_active()) {
		blog(LOG_WARNING, "[snip-record] Not starting: OBS is already recording");
		return false;
	}

	if (selection.mode != SnipSelection::Mode::Window &&
	    (selection.monitorId.empty() ||
	     !snip::IsUsable({selection.rect.x, selection.rect.y, selection.rect.width, selection.rect.height}))) {
		blog(LOG_ERROR, "[snip-record] Not starting: invalid selection");
		return false;
	}
	if (selection.mode == SnipSelection::Mode::Window && selection.windowValue.empty()) {
		blog(LOG_ERROR, "[snip-record] Not starting: no window selected");
		return false;
	}

	OBSSourceAutoRelease sceneSource = FindOrCreateScene();
	if (!sceneSource) {
		return false;
	}

	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	if (!scene) {
		return false;
	}

	// Remember where to return to after the snip recording stops.
	OBSSourceAutoRelease currentScene = obs_frontend_get_current_scene();
	if (currentScene && currentScene.Get() != sceneSource.Get()) {
		previousScene = obs_source_get_weak_source(currentScene);
	} else {
		previousScene = nullptr;
	}
	snipScene = obs_source_get_weak_source(sceneSource);

	ClearSceneItems(scene);

	OBSSourceAutoRelease capture = CreateCaptureSource(selection);
	if (!capture) {
		return false;
	}

	obs_sceneitem_t *item = obs_scene_add(scene, capture);
	if (!item) {
		return false;
	}

	if (selection.mode == SnipSelection::Mode::Region) {
		const snip::Crop crop = snip::CropForRegion(selection.rect, selection.monitorRect);
		obs_sceneitem_crop itemCrop = {crop.left, crop.top, crop.right, crop.bottom};
		obs_sceneitem_set_crop(item, &itemCrop);
	}
	FitItemToCanvas(item);

	obs_frontend_set_current_scene(sceneSource);
	active = true;
	blog(LOG_INFO, "[snip-record] Starting recording (mode=%d, %dx%d)", int(selection.mode), selection.rect.width,
	     selection.rect.height);
	obs_frontend_recording_start();
	return true;
}

void SnipRecorder::HandleRecordingStopped()
{
	if (!active) {
		return;
	}
	active = false;

	OBSSourceAutoRelease snip = snipScene ? obs_weak_source_get_source(snipScene) : nullptr;
	OBSSourceAutoRelease previous = previousScene ? obs_weak_source_get_source(previousScene) : nullptr;
	snipScene = nullptr;
	previousScene = nullptr;

	if (!snip || !previous) {
		return;
	}

	OBSSourceAutoRelease currentScene = obs_frontend_get_current_scene();
	if (currentScene && currentScene.Get() == snip.Get()) {
		obs_frontend_set_current_scene(previous);
	}
}
