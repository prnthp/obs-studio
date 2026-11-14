#pragma once

#include <cctype>
#include <functional>
#include <string>

namespace snip {

struct GifExportOptions {
	int maxWidth = 960; // downscale wider sources, never upscale
	int fps = 15;
};

// Converts the first video stream of `input` (an MP4 from a snip
// recording) into an animated GIF at `output`, using the FFmpeg
// libraries OBS already ships: decode -> fps/scale -> palettegen/
// paletteuse -> gif. Blocking; call from a worker thread. `progress`
// (0..1) may be empty. On failure returns false and sets `error`.
bool ConvertToGif(const std::string &inputPath, const std::string &outputPath, const GifExportOptions &options,
		  std::string &error, const std::function<void(double)> &progress = {});

struct Mp4ExportOptions {
	int crf = 23; // libx264 quality; ignored by fallback encoders
};

// Re-encodes the first video stream (and first audio stream, when
// present) of `input` into an MP4 at `output`: H.264 (libx264 when
// available) + AAC, same resolution and frame rate as the source.
// Blocking; call from a worker thread. `progress` (0..1) may be
// empty. On failure returns false and sets `error`.
bool ConvertToMp4(const std::string &inputPath, const std::string &outputPath, const Mp4ExportOptions &options,
		  std::string &error, const std::function<void(double)> &progress = {});

// Which extra exports to produce after a snip. Any combination may
// be on at once (the picker's three checkboxes).
struct ExportSelection {
	bool gif = false;
	bool webp = false;
	bool mp4 = false;

	bool Any() const { return gif || webp || mp4; }
};

// Old single-choice setting, kept so saved settings from earlier
// versions still mean what the user picked:
// 0 = none, 1 = GIF, 2 = WebP.
inline ExportSelection ExportSelectionFromLegacyFormat(int format)
{
	ExportSelection selection;
	selection.gif = format == 1;
	selection.webp = format == 2;
	return selection;
}

// Output path for a sibling export: same folder and base name as
// `inputPath`, with its extension replaced by `extension`
// (".gif", ".webp", ".mp4", ...). When the recording already has
// that extension (an MP4 re-encode of an MP4), the plain path
// would be the recording itself, so " (re-encoded)" is inserted
// instead of overwriting the master file.
inline std::string ExportPathFor(const std::string &inputPath, const std::string &extension)
{
	const size_t slash = inputPath.find_last_of("/\\");
	const size_t dot = inputPath.find_last_of('.');
	const bool hasExtension = dot != std::string::npos && (slash == std::string::npos || dot > slash);
	const std::string base = hasExtension ? inputPath.substr(0, dot) : inputPath;
	const std::string current = hasExtension ? inputPath.substr(dot) : std::string();

	auto equalIgnoreCase = [](const std::string &a, const std::string &b) {
		if (a.size() != b.size()) {
			return false;
		}
		for (size_t i = 0; i < a.size(); i++) {
			if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
				return false;
			}
		}
		return true;
	};

	if (hasExtension && equalIgnoreCase(current, extension)) {
		return base + " (re-encoded)" + extension;
	}
	return base + extension;
}

} // namespace snip
