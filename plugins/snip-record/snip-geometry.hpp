#pragma once

#include <algorithm>

// Pure coordinate math for snip-record. No Qt / libobs / Win32 dependencies,
// so it can be unit-tested standalone. All Rect values are in pixels; callers
// are responsible for keeping logical (Qt) and physical (capture) spaces
// separate and converting between them with MapRect().

namespace snip {

struct Rect {
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;

	int Right() const { return x + width; }   // exclusive
	int Bottom() const { return y + height; } // exclusive
	bool IsEmpty() const { return width <= 0 || height <= 0; }
};

struct Crop {
	int left = 0;
	int top = 0;
	int right = 0;
	int bottom = 0;
};

inline bool IsUsable(const Rect &rect, int minWidth = 8, int minHeight = 8)
{
	return rect.width >= minWidth && rect.height >= minHeight;
}

inline Rect Intersect(const Rect &a, const Rect &b)
{
	Rect result;
	result.x = std::max(a.x, b.x);
	result.y = std::max(a.y, b.y);
	result.width = std::min(a.Right(), b.Right()) - result.x;
	result.height = std::min(a.Bottom(), b.Bottom()) - result.y;
	if (result.IsEmpty()) {
		result.width = 0;
		result.height = 0;
	}
	return result;
}

// Maps a rect from one screen space to another by relative position, e.g.
// Qt logical pixels -> Win32 physical pixels for the same display. Using
// fractions of the display size keeps this correct under DPI scaling.
inline Rect MapRect(const Rect &rect, const Rect &fromSpace, const Rect &toSpace)
{
	if (fromSpace.width <= 0 || fromSpace.height <= 0) {
		return rect;
	}

	auto mapAxis = [](int value, int fromOrigin, int fromSize, int toOrigin, int toSize) {
		const double fraction = double(value - fromOrigin) / double(fromSize);
		return toOrigin + int(fraction * double(toSize) + (fraction >= 0.0 ? 0.5 : -0.5));
	};

	Rect result;
	result.x = mapAxis(rect.x, fromSpace.x, fromSpace.width, toSpace.x, toSpace.width);
	result.y = mapAxis(rect.y, fromSpace.y, fromSpace.height, toSpace.y, toSpace.height);
	const int right = mapAxis(rect.Right(), fromSpace.x, fromSpace.width, toSpace.x, toSpace.width);
	const int bottom = mapAxis(rect.Bottom(), fromSpace.y, fromSpace.height, toSpace.y, toSpace.height);
	result.width = right - result.x;
	result.height = bottom - result.y;
	return result;
}

// Placement for a floating toolbar of `width`x`height` at the top
// centre of `area` (a screen's available geometry, which already
// excludes the menu bar / Dock / taskbar), `margin` below its top.
// Centring on one screen's available area -- never on the whole
// virtual desktop, whose centre can be the seam between displays
// and whose top can be under a menu bar.
inline Rect PlaceTopCentered(const Rect &area, int width, int height, int margin)
{
	Rect result;
	result.x = area.x + std::max(0, (area.width - width) / 2);
	result.y = area.y + margin;
	result.width = width;
	result.height = height;
	return result;
}

// Crop (in capture-source pixels) that keeps only `selection` out of a
// full-display capture of `monitor`. The selection is clamped to the
// monitor first, so a region dragged across displays records the part on
// the display it was started on / clamped to.
inline Crop CropForRegion(const Rect &selection, const Rect &monitor)
{
	const Rect clamped = Intersect(selection, monitor);
	Crop crop;
	if (clamped.IsEmpty()) {
		return crop;
	}

	crop.left = clamped.x - monitor.x;
	crop.top = clamped.y - monitor.y;
	crop.right = monitor.Right() - clamped.Right();
	crop.bottom = monitor.Bottom() - clamped.Bottom();
	return crop;
}

} // namespace snip
