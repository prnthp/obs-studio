// Standalone regression test for the coordinate math in
// ../snip-geometry.hpp. Not part of the OBS build; compile and run
// manually, e.g. with MSVC:
//   cl /std:c++17 /EHsc /I .. snip-geometry-test.cpp && snip-geometry-test.exe
// Exits non-zero and prints FAIL lines when a check fails.

#include <cassert>
#include <cstdio>

#include "snip-geometry.hpp"

using namespace snip;

static int failures = 0;
#define CHECK(cond)                                                     \
	do {                                                            \
		if (!(cond)) {                                          \
			std::printf("FAIL line %d: %s\n", __LINE__, #cond); \
			failures++;                                     \
		}                                                       \
	} while (0)

int main()
{
	// Identity mapping at 100% scaling.
	Rect mon = {0, 0, 1920, 1080};
	Rect sel = {100, 50, 800, 600};
	CHECK(MapRect(sel, mon, mon).x == 100 && MapRect(sel, mon, mon).width == 800);

	// 150% DPI: logical 1920x1080 screen -> physical 2880x1620 monitor.
	Rect logicalScreen = {0, 0, 1920, 1080};
	Rect physicalMon = {0, 0, 2880, 1620};
	Rect phys = MapRect({100, 100, 200, 100}, logicalScreen, physicalMon);
	CHECK(phys.x == 150 && phys.y == 150 && phys.width == 300 && phys.height == 150);

	// Secondary monitor at negative offset.
	Rect logical2 = {-1920, 0, 1920, 1080};
	Rect physical2 = {-1920, 0, 1920, 1080};
	Rect mapped = MapRect({-1900, 10, 100, 50}, logical2, physical2);
	CHECK(mapped.x == -1900 && mapped.y == 10 && mapped.width == 100);

	// Full-screen logical rect maps to the full physical monitor.
	Rect full = MapRect(logicalScreen, logicalScreen, physicalMon);
	CHECK(full.x == 0 && full.y == 0 && full.width == 2880 && full.height == 1620);

	// Crop of an inset region.
	Crop crop = CropForRegion({100, 50, 800, 600}, mon);
	CHECK(crop.left == 100 && crop.top == 50 && crop.right == 1020 && crop.bottom == 430);

	// Crop of a full-monitor selection is zero.
	Crop zero = CropForRegion(mon, mon);
	CHECK(zero.left == 0 && zero.top == 0 && zero.right == 0 && zero.bottom == 0);

	// Region hanging off the right/bottom edge is clamped.
	Crop clamped = CropForRegion({1800, 1000, 400, 400}, mon);
	CHECK(clamped.left == 1800 && clamped.top == 1000 && clamped.right == 0 && clamped.bottom == 0);

	// Region completely outside -> empty crop.
	Crop outside = CropForRegion({5000, 0, 100, 100}, mon);
	CHECK(outside.left == 0 && outside.right == 0 && outside.top == 0 && outside.bottom == 0);

	// Intersect / IsUsable basics.
	CHECK(Intersect({0, 0, 100, 100}, {50, 50, 100, 100}).width == 50);
	CHECK(Intersect({0, 0, 10, 10}, {20, 20, 10, 10}).IsEmpty());
	CHECK(IsUsable({0, 0, 8, 8}));
	CHECK(!IsUsable({0, 0, 7, 8}));
	CHECK(!IsUsable({0, 0, 100, 0}));

	if (failures == 0) {
		std::printf("ALL GEOMETRY TESTS PASSED\n");
	}
	return failures;
}
