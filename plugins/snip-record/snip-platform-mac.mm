#include "snip-platform-mac.hpp"

#ifdef __APPLE__

#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>

#include <QGuiApplication>
#include <QScreen>
#include <QWidget>

#include <unistd.h>

namespace snip_mac {

namespace {

QRect RectFromDict(NSDictionary *window)
{
	NSDictionary *bounds = window[(NSString *)kCGWindowBounds];
	CGRect rect = CGRectNull;
	if (!bounds || !CGRectMakeWithDictionaryRepresentation((CFDictionaryRef)bounds, &rect)) {
		return {};
	}
	return QRect(int(rect.origin.x), int(rect.origin.y), int(rect.size.width + 0.5),
		     int(rect.size.height + 0.5));
}

WindowInfo InfoFromDict(NSDictionary *window)
{
	WindowInfo info;
	info.windowId = [window[(NSString *)kCGWindowNumber] unsignedIntValue];
	info.pid = [window[(NSString *)kCGWindowOwnerPID] unsignedLongValue];
	info.title = QString::fromNSString(window[(NSString *)kCGWindowName] ?: @"");
	info.ownerName = QString::fromNSString(window[(NSString *)kCGWindowOwnerName] ?: @"");
	info.logicalRect = RectFromDict(window);
	return info;
}

} // namespace

std::vector<WindowInfo> OnScreenWindows(uint32_t excludeWindowId)
{
	std::vector<WindowInfo> result;
	// Front-to-back order, desktop elements (Dock, menubar) excluded.
	NSArray *windows = (NSArray *)CGWindowListCopyWindowInfo(
		kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
	for (NSDictionary *window in windows) {
		if ([window[(NSString *)kCGWindowLayer] intValue] != 0) {
			continue; // normal application windows only
		}
		if ([window[(NSString *)kCGWindowAlpha] doubleValue] < 0.01) {
			continue;
		}
		WindowInfo info = InfoFromDict(window);
		if (info.windowId == 0 || info.windowId == excludeWindowId) {
			continue;
		}
		if (info.logicalRect.width() < 16 || info.logicalRect.height() < 16) {
			continue;
		}
		result.push_back(info);
	}
	[windows release];
	return result;
}

bool WindowBounds(uint32_t windowId, QRect &outRect, unsigned long *outPid)
{
	NSArray *windows = (NSArray *)CGWindowListCopyWindowInfo(kCGWindowListOptionIncludingWindow, windowId);
	if (windows.count != 1) {
		[windows release];
		return false;
	}
	WindowInfo info = InfoFromDict(windows.firstObject);
	[windows release];
	if (info.logicalRect.isEmpty()) {
		return false;
	}
	outRect = info.logicalRect;
	if (outPid) {
		*outPid = info.pid;
	}
	return true;
}

std::vector<DisplayInfo> Displays()
{
	std::vector<DisplayInfo> result;
	uint32_t count = 0;
	CGGetActiveDisplayList(0, nullptr, &count);
	if (count == 0) {
		return result;
	}
	std::vector<CGDirectDisplayID> ids(count);
	CGGetActiveDisplayList(count, ids.data(), &count);
	ids.resize(count);

	for (CGDirectDisplayID id : ids) {
		const CGRect bounds = CGDisplayBounds(id); // points, top-left origin
		DisplayInfo info;
		info.displayId = id;
		info.logicalRect = QRect(int(bounds.origin.x), int(bounds.origin.y), int(bounds.size.width + 0.5),
					 int(bounds.size.height + 0.5));

		// Pixel size of the current mode; origin scaled by the same
		// factor so MapRect-based crops stay consistent per display.
		double scale = 1.0;
		for (NSScreen *screen in NSScreen.screens) {
			NSNumber *number = screen.deviceDescription[@"NSScreenNumber"];
			if (number && (CGDirectDisplayID)number.unsignedIntValue == id) {
				scale = screen.backingScaleFactor;
				break;
			}
		}
		int pixelWidth = int(bounds.size.width * scale + 0.5);
		int pixelHeight = int(bounds.size.height * scale + 0.5);
		if (CGDisplayModeRef mode = CGDisplayCopyDisplayMode(id)) {
			pixelWidth = (int)CGDisplayModeGetPixelWidth(mode);
			pixelHeight = (int)CGDisplayModeGetPixelHeight(mode);
			CGDisplayModeRelease(mode);
		}
		info.physicalRect = QRect(int(bounds.origin.x * scale), int(bounds.origin.y * scale), pixelWidth,
					  pixelHeight);

		if (CFUUIDRef uuid = CGDisplayCreateUUIDFromDisplayID(id)) {
			CFStringRef uuidString = CFUUIDCreateString(kCFAllocatorDefault, uuid);
			info.uuid = QString::fromCFString(uuidString);
			CFRelease(uuidString);
			CFRelease(uuid);
		}
		result.push_back(info);
	}
	return result;
}

bool DisplayForScreen(QScreen *screen, DisplayInfo &out)
{
	const std::vector<DisplayInfo> displays = Displays();
	if (displays.empty()) {
		return false;
	}
	if (screen) {
		const QRect geometry = screen->geometry();
		for (const DisplayInfo &info : displays) {
			if (info.logicalRect == geometry) {
				out = info;
				return true;
			}
		}
		const QPoint center = geometry.center();
		for (const DisplayInfo &info : displays) {
			if (info.logicalRect.contains(center)) {
				out = info;
				return true;
			}
		}
	}
	out = displays.front();
	return true;
}

uint32_t WindowIdForWidget(QWidget *widget)
{
	if (!widget) {
		return 0;
	}
	NSView *view = (NSView *)widget->winId();
	NSWindow *window = view ? view.window : nullptr;
	return window ? (uint32_t)window.windowNumber : 0;
}

void ExcludeFromCapture(QWidget *widget)
{
	NSView *view = widget ? (NSView *)widget->winId() : nullptr;
	NSWindow *window = view ? view.window : nullptr;
	if (window) {
		// NSWindowSharingNone: the window is invisible to
		// ScreenCaptureKit/CGWindowList-based captures.
		window.sharingType = NSWindowSharingNone;
	}
}

void ConfigureOutlineWindow(QWidget *widget)
{
	NSView *view = widget ? (NSView *)widget->winId() : nullptr;
	NSWindow *window = view ? view.window : nullptr;
	if (!window) {
		return;
	}
	ExcludeFromCapture(widget);
	// Qt's WA_TransparentForMouseEvents alone does not make an
	// NSWindow click-through; without this the border swallows
	// every click inside the recorded area.
	window.ignoresMouseEvents = YES;
	// Tool windows hide when their app deactivates; the outline
	// must stay up while the user works in the recorded app.
	window.hidesOnDeactivate = NO;
	// Recording is in global screen coordinates, not tied to one
	// Space: show the outline on whichever Space is current.
	// (CanJoinAllSpaces alone: AppKit asserts against combining it
	// with Stationary, which aborts the app.)
	@try {
		window.collectionBehavior |= NSWindowCollectionBehaviorCanJoinAllSpaces;
	} @catch (NSException *) {
		// Keep the outline on its creation Space rather than crash.
	}
	// Above normal floating windows (the recorded app included).
	if (window.level < NSStatusWindowLevel) {
		window.level = NSStatusWindowLevel;
	}
}

} // namespace snip_mac

#endif // __APPLE__
