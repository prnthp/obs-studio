#pragma once

#include <string>

#include "snip-geometry.hpp"

// What the picker hands to the recorder. Rect/monitorRect are in physical
// pixels in virtual-desktop coordinates (the space capture sources and
// scene-item crops work in).
struct SnipSelection {
	enum class Mode {
		Region,
		Window,
		FullScreen,
	};

	Mode mode = Mode::Region;

	// Selected area (a window rect in Window mode, a display rect in
	// FullScreen mode, the dragged rect in Region mode).
	snip::Rect rect;

	// Same selection in global logical (Qt) pixels; used to place
	// on-screen UI such as the recording outline.
	snip::Rect logicalRect;

	// Physical rect of the display `rect` belongs to, and its
	// monitor_capture "monitor_id" value (Windows display device id).
	// Unused in Window mode.
	snip::Rect monitorRect;
	std::string monitorId;

	// window_capture "window" setting: encoded "title:class:executable".
	// Only used in Window mode.
	std::string windowValue;
};
