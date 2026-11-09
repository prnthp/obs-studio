// Headless check for the GIF exporter: converts a real MP4 and then
// re-opens the GIF to count frames. Build (from repo root):
//   cl /std:c++17 /EHsc /I .deps\obs-deps-2026-10-23-x64\include \
//      plugins\snip-record\tests\snip-export-test.cpp \
//      plugins\snip-record\snip-exporter.cpp \
//      /link /LIBPATH:.deps\obs-deps-2026-10-23-x64\lib \
//      avformat.lib avcodec.lib avfilter.lib avutil.lib
#include <cstdio>
#include <cstring>

#include "../snip-exporter.hpp"

extern "C" {
#include <libavformat/avformat.h>
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		std::printf("usage: snip-export-test <input.mp4> <output.gif>\n");
		return 2;
	}

	snip::GifExportOptions options;
	options.maxWidth = 640;
	options.fps = 12;
	std::string error;
	double lastProgress = -1.0;
	const bool ok = snip::ConvertToGif(argv[1], argv[2], options, error, [&lastProgress](double p) {
		if (p - lastProgress >= 0.25) {
			lastProgress = p;
			std::printf("progress %.0f%%\n", p * 100.0);
		}
	});
	if (!ok) {
		std::printf("CONVERT FAILED: %s\n", error.c_str());
		return 1;
	}

	// Re-open the GIF and count frames + read its dimensions.
	AVFormatContext *ctx = nullptr;
	if (avformat_open_input(&ctx, argv[2], nullptr, nullptr) < 0 || avformat_find_stream_info(ctx, nullptr) < 0) {
		std::printf("VERIFY FAILED: cannot re-open GIF\n");
		return 1;
	}
	const int videoIndex = av_find_best_stream(ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	AVPacket *packet = av_packet_alloc();
	int frames = 0;
	while (av_read_frame(ctx, packet) >= 0) {
		if (packet->stream_index == videoIndex) {
			frames++;
		}
		av_packet_unref(packet);
	}
	av_packet_free(&packet);
	const AVCodecParameters *par = ctx->streams[videoIndex]->codecpar;
	std::printf("CONVERT OK: %d frames, %dx%d\n", frames, par->width, par->height);
	avformat_close_input(&ctx);
	return frames > 5 ? 0 : 1;
}
