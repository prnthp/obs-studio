// Standalone regression test for the export planning helpers in
// ../snip-exporter.hpp (which formats get produced, and where they
// land on disk). No FFmpeg/Qt/libobs needed; compile and run
// manually, e.g. with MSVC:
//   cl /std:c++17 /EHsc /I .. snip-export-plan-test.cpp && snip-export-plan-test.exe
// Exits non-zero and prints FAIL lines when a check fails.

#include <cstdio>

#include "../snip-exporter.hpp"

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
	// Sibling exports replace the recording's extension.
	CHECK(ExportPathFor("C:\\Videos\\snip.mp4", ".gif") == "C:\\Videos\\snip.gif");
	CHECK(ExportPathFor("C:\\Videos\\snip.mkv", ".mp4") == "C:\\Videos\\snip.mp4");
	CHECK(ExportPathFor("/home/user/snip.webm", ".webp") == "/home/user/snip.webp");

	// An MP4 re-encode of an MP4 must never target the master
	// file itself (that would overwrite the only recording).
	CHECK(ExportPathFor("C:\\Videos\\snip.mp4", ".mp4") == "C:\\Videos\\snip (re-encoded).mp4");
	CHECK(ExportPathFor("C:\\Videos\\snip.MP4", ".mp4") == "C:\\Videos\\snip (re-encoded).mp4");

	// No extension, and a dot in a folder name, are not extensions.
	CHECK(ExportPathFor("C:\\Videos\\snip", ".mp4") == "C:\\Videos\\snip.mp4");
	CHECK(ExportPathFor("C:\\My.Videos\\snip", ".gif") == "C:\\My.Videos\\snip.gif");

	// Legacy single-choice values map to exactly one checkbox.
	ExportSelection legacy = ExportSelectionFromLegacyFormat(0);
	CHECK(!legacy.Any());
	legacy = ExportSelectionFromLegacyFormat(1);
	CHECK(legacy.gif && !legacy.webp && !legacy.mp4);
	legacy = ExportSelectionFromLegacyFormat(2);
	CHECK(!legacy.gif && legacy.webp && !legacy.mp4);
	legacy = ExportSelectionFromLegacyFormat(99);
	CHECK(!legacy.Any());

	// Combinations are allowed now: GIF + MP4 at once.
	ExportSelection both;
	both.gif = true;
	both.mp4 = true;
	CHECK(both.Any() && both.gif && both.mp4 && !both.webp);
	CHECK(!ExportSelection().Any());

	if (failures == 0) {
		std::printf("ALL EXPORT PLAN TESTS PASSED\n");
	}
	return failures;
}
