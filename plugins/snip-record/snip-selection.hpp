#pragma once

#include <cstdint>
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

	// Physical rect of the display `rect` belongs to, and its capture
	// source display value: monitor_capture "monitor_id" on Windows
	// (display device id), screen_capture "display_uuid" on macOS.
	// Unused in Window mode.
	snip::Rect monitorRect;
	std::string monitorId;

	// window_capture "window" setting: encoded "title:class:executable".
	// Only used in Window mode on Windows.
	std::string windowValue;

	// macOS Window mode: CGWindowID for the ScreenCaptureKit
	// `screen_capture` source's "window" setting. 0 elsewhere.
	uint32_t windowId = 0;
};
