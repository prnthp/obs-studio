#pragma once

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

} // namespace snip
