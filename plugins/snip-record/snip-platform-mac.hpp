#pragma once

#ifdef __APPLE__

#include <QRect>
#include <QString>

#include <cstdint>
#include <vector>

class QScreen;
class QWidget;

// macOS counterparts of the Win32 helpers used on Windows: window
// enumeration/hit-testing via CGWindowList, display identification via
// CoreGraphics display UUIDs (what the ScreenCaptureKit `screen_capture`
// source's `display_uuid` setting expects), and capture exclusion via
// NSWindowSharingNone (the WDA_EXCLUDEFROMCAPTURE counterpart).
// All rects here are in global logical pixels (points), the same
// top-left-origin space Qt uses; physical (pixel) rects are derived by
// the picker with snip::MapRect, exactly as on Windows.
namespace snip_mac {

struct WindowInfo {
	uint32_t windowId = 0; // CGWindowID; 0 = none
	QRect logicalRect;
	unsigned long pid = 0;
	QString title;
	QString ownerName;
};

struct DisplayInfo {
	uint32_t displayId = 0; // CGDirectDisplayID
	QRect logicalRect;      // points, global top-left origin
	QRect physicalRect;     // pixels, same relative layout scaled
	QString uuid;           // display_uuid for `screen_capture`
};

// Normal (layer 0) on-screen windows, front-to-back (first hit under a
// point is the topmost), excluding `excludeWindowId` (the picker).
std::vector<WindowInfo> OnScreenWindows(uint32_t excludeWindowId);

// Current bounds/pid of one window. False if it no longer exists.
bool WindowBounds(uint32_t windowId, QRect &outRect, unsigned long *outPid);

std::vector<DisplayInfo> Displays();

// Display whose logical rect matches the Qt screen (falling back to
// the display containing the screen's centre).
bool DisplayForScreen(QScreen *screen, DisplayInfo &out);

// CGWindowID of a Qt widget's native window (0 before it is created).
uint32_t WindowIdForWidget(QWidget *widget);

// Keep this widget's window out of screen captures (outline window).
void ExcludeFromCapture(QWidget *widget);

// Native setup for the recording outline window: excluded from
// captures, click-through (mouse events fall through to the apps
// below), present on every Space, and not auto-hidden when OBS is
// not the active app (Qt::Tool windows hide on deactivate by
// default on macOS).
void ConfigureOutlineWindow(QWidget *widget);

} // namespace snip_mac

#endif // __APPLE__
