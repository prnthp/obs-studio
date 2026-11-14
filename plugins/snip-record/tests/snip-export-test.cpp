// Headless check for the GIF and MP4 exporters: converts a real
// MP4 to GIF and re-encoded MP4, then re-opens both outputs to
// verify them. Build (from repo root):
//   cl /std:c++17 /EHsc /I .deps\obs-deps-2026-10-23-x64\include \
//      plugins\snip-record\tests\snip-export-test.cpp \
//      plugins\snip-record\snip-exporter.cpp \
//      /link /LIBPATH:.deps\obs-deps-2026-10-23-x64\lib \
//      avformat.lib avcodec.lib avfilter.lib avutil.lib swresample.lib
#include <cstdio>
#include <cstring>

#include "../snip-exporter.hpp"

extern "C" {
#include <libavcodec/codec_id.h>
#include <libavformat/avformat.h>
}

static int CountFrames(const char *path, int &width, int &height, int &videoCodec, bool &hasAudio)
{
	width = height = videoCodec = 0;
	hasAudio = false;
	AVFormatContext *ctx = nullptr;
	if (avformat_open_input(&ctx, path, nullptr, nullptr) < 0 || avformat_find_stream_info(ctx, nullptr) < 0) {
		return -1;
	}
	const int videoIndex = av_find_best_stream(ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (videoIndex < 0) {
		avformat_close_input(&ctx);
		return -1;
	}
	hasAudio = av_find_best_stream(ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0) >= 0;
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
	width = par->width;
	height = par->height;
	videoCodec = par->codec_id;
	avformat_close_input(&ctx);
	return frames;
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		std::printf("usage: snip-export-test <input.mp4> <output.gif> [output.mp4]\n");
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
	int width = 0, height = 0, codec = 0;
	bool hasAudio = false;
	const int frames = CountFrames(argv[2], width, height, codec, hasAudio);
	if (frames < 0) {
		std::printf("VERIFY FAILED: cannot re-open GIF\n");
		return 1;
	}
	std::printf("CONVERT OK: %d frames, %dx%d\n", frames, width, height);
	if (frames <= 5) {
		return 1;
	}

	// Optional MP4 re-encode check: must come back as H.264.
	if (argc >= 4) {
		error.clear();
		if (!snip::ConvertToMp4(argv[1], argv[3], {}, error)) {
			std::printf("MP4 CONVERT FAILED: %s\n", error.c_str());
			return 1;
		}
		const int mp4Frames = CountFrames(argv[3], width, height, codec, hasAudio);
		std::printf("MP4 CONVERT OK: %d frames, %dx%d, codec %d, audio %s\n", mp4Frames, width, height, codec,
			    hasAudio ? "yes" : "no");
		if (mp4Frames <= 5 || codec != AV_CODEC_ID_H264) {
			return 1;
		}
	}
	return 0;
}
