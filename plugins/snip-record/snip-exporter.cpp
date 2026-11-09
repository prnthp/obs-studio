#include "snip-exporter.hpp"

#include <algorithm>
#include <cstdio>
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
}

namespace {

std::string AvError(int errnum)
{
	char buf[AV_ERROR_MAX_STRING_SIZE] = {};
	av_strerror(errnum, buf, sizeof(buf));
	return buf;
}

struct FormatCtxDeleter {
	void operator()(AVFormatContext *p) const { avformat_free_context(p); }
};
struct CodecCtxDeleter {
	void operator()(AVCodecContext *p) const { avcodec_free_context(&p); }
};
struct FrameDeleter {
	void operator()(AVFrame *p) const { av_frame_free(&p); }
};
struct PacketDeleter {
	void operator()(AVPacket *p) const { av_packet_free(&p); }
};
struct FilterGraphDeleter {
	void operator()(AVFilterGraph *p) const { avfilter_graph_free(&p); }
};

using FormatCtxPtr = std::unique_ptr<AVFormatContext, FormatCtxDeleter>;
using CodecCtxPtr = std::unique_ptr<AVCodecContext, CodecCtxDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using FilterGraphPtr = std::unique_ptr<AVFilterGraph, FilterGraphDeleter>;

} // namespace

namespace snip {

bool ConvertToGif(const std::string &inputPath, const std::string &outputPath, const GifExportOptions &options,
		  std::string &error, const std::function<void(double)> &progress)
{
	auto fail = [&error](const std::string &message) {
		error = message;
		return false;
	};

	AVFormatContext *rawIn = nullptr;
	int ret = avformat_open_input(&rawIn, inputPath.c_str(), nullptr, nullptr);
	if (ret < 0) {
		return fail("Could not open recording: " + AvError(ret));
	}
	FormatCtxPtr inCtx(rawIn);
	if ((ret = avformat_find_stream_info(inCtx.get(), nullptr)) < 0) {
		return fail("Could not read recording: " + AvError(ret));
	}
	const int videoIndex = av_find_best_stream(inCtx.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (videoIndex < 0) {
		return fail("Recording has no video stream");
	}
	AVStream *inStream = inCtx->streams[videoIndex];
	const AVCodec *decoder = avcodec_find_decoder(inStream->codecpar->codec_id);
	if (!decoder) {
		return fail("No decoder for the recording's video codec");
	}
	CodecCtxPtr decCtx(avcodec_alloc_context3(decoder));
	if (!decCtx || avcodec_parameters_to_context(decCtx.get(), inStream->codecpar) < 0) {
		return fail("Could not set up the decoder");
	}
	if ((ret = avcodec_open2(decCtx.get(), decoder, nullptr)) < 0) {
		return fail("Could not open the decoder: " + AvError(ret));
	}

	const int fps = std::clamp(options.fps, 1, 60);
	const int outWidth = std::min(options.maxWidth, decCtx->width);
	const int outHeight = std::max(2, int(decCtx->height * int64_t(outWidth) / std::max(1, decCtx->width)) & ~1);

	// Filter graph: cap frame rate, scale, generate an optimal
	// palette from the actual frames, apply it with dithering.
	FilterGraphPtr graph(avfilter_graph_alloc());
	if (!graph) {
		return fail("Out of memory");
	}
	const AVFilter *bufferSrc = avfilter_get_by_name("buffer");
	const AVFilter *bufferSink = avfilter_get_by_name("buffersink");
	if (!bufferSrc || !bufferSink) {
		return fail("FFmpeg buffersrc/buffersink filters unavailable");
	}
	AVFilterContext *srcCtx = nullptr;
	AVFilterContext *sinkCtx = nullptr;
	const AVRational inTimeBase = inStream->time_base;
	const AVRational frameRate = av_guess_frame_rate(inCtx.get(), inStream, nullptr);
	const AVRational sar = decCtx->sample_aspect_ratio.num > 0 ? decCtx->sample_aspect_ratio : AVRational{1, 1};
	char srcArgs[512];
	std::snprintf(srcArgs, sizeof(srcArgs),
		      "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d:frame_rate=%d/%d",
		      decCtx->width, decCtx->height, int(decCtx->pix_fmt), inTimeBase.num, inTimeBase.den, sar.num,
		      sar.den, frameRate.num > 0 ? frameRate.num : 30, frameRate.den > 0 ? frameRate.den : 1);
	if ((ret = avfilter_graph_create_filter(&srcCtx, bufferSrc, "in", srcArgs, nullptr, graph.get())) < 0 ||
	    (ret = avfilter_graph_create_filter(&sinkCtx, bufferSink, "out", nullptr, nullptr, graph.get())) < 0) {
		return fail("Could not create filters: " + AvError(ret));
	}
	// (No sink format constraint needed: paletteuse already outputs
	// PAL8, which is what the GIF encoder consumes.)

	AVFilterInOut *graphOut = avfilter_inout_alloc();
	AVFilterInOut *graphIn = avfilter_inout_alloc();
	if (!graphOut || !graphIn) {
		avfilter_inout_free(&graphOut);
		avfilter_inout_free(&graphIn);
		return fail("Out of memory");
	}
	graphOut->name = av_strdup("in");
	graphOut->filter_ctx = srcCtx;
	graphOut->pad_idx = 0;
	graphOut->next = nullptr;
	graphIn->name = av_strdup("out");
	graphIn->filter_ctx = sinkCtx;
	graphIn->pad_idx = 0;
	graphIn->next = nullptr;

	char graphDesc[512];
	std::snprintf(graphDesc, sizeof(graphDesc),
		      "fps=%d,scale=w=%d:h=%d:flags=lanczos,split[s0][s1];"
		      "[s0]palettegen=stats_mode=diff[p];[s1][p]paletteuse=dither=sierra2_4a",
		      fps, outWidth, outHeight);
	ret = avfilter_graph_parse_ptr(graph.get(), graphDesc, &graphIn, &graphOut, nullptr);
	avfilter_inout_free(&graphIn);
	avfilter_inout_free(&graphOut);
	if (ret < 0 || (ret = avfilter_graph_config(graph.get(), nullptr)) < 0) {
		return fail("Could not configure the GIF filter graph: " + AvError(ret));
	}

	const AVCodec *gifEncoder = avcodec_find_encoder(AV_CODEC_ID_GIF);
	if (!gifEncoder) {
		return fail("FFmpeg GIF encoder unavailable");
	}
	CodecCtxPtr encCtx(avcodec_alloc_context3(gifEncoder));
	if (!encCtx) {
		return fail("Out of memory");
	}
	encCtx->width = outWidth;
	encCtx->height = outHeight;
	encCtx->pix_fmt = AV_PIX_FMT_PAL8;
	encCtx->time_base = {1, fps};
	encCtx->framerate = {fps, 1};

	AVFormatContext *rawOut = nullptr;
	ret = avformat_alloc_output_context2(&rawOut, nullptr, "gif", outputPath.c_str());
	if (ret < 0 || !rawOut) {
		return fail("Could not create the GIF file: " + AvError(ret));
	}
	FormatCtxPtr outCtx(rawOut);
	AVStream *outStream = avformat_new_stream(outCtx.get(), nullptr);
	if (!outStream) {
		return fail("Could not create the GIF stream");
	}
	if ((ret = avcodec_open2(encCtx.get(), gifEncoder, nullptr)) < 0) {
		return fail("Could not open the GIF encoder: " + AvError(ret));
	}
	if ((ret = avcodec_parameters_from_context(outStream->codecpar, encCtx.get())) < 0) {
		return fail("Could not set up the GIF stream: " + AvError(ret));
	}
	outStream->time_base = encCtx->time_base;
	if ((ret = avio_open(&outCtx->pb, outputPath.c_str(), AVIO_FLAG_WRITE)) < 0) {
		return fail("Could not write the GIF file: " + AvError(ret));
	}
	if ((ret = avformat_write_header(outCtx.get(), nullptr)) < 0) {
		return fail("Could not write the GIF header: " + AvError(ret));
	}

	// Frame count estimate for progress reporting.
	int64_t totalFrames = inStream->nb_frames;
	if (totalFrames <= 0 && inCtx->duration > 0) {
		totalFrames = inCtx->duration * fps / AV_TIME_BASE;
	}

	PacketPtr packet(av_packet_alloc());
	FramePtr frame(av_frame_alloc());
	FramePtr filtered(av_frame_alloc());
	if (!packet || !frame || !filtered) {
		return fail("Out of memory");
	}

	int64_t framesWritten = 0;
	bool failed = false;
	auto encodeFiltered = [&]() {
		while (!failed) {
			ret = av_buffersink_get_frame(sinkCtx, filtered.get());
			if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
				return;
			}
			if (ret < 0) {
				error = "Filtering failed: " + AvError(ret);
				failed = true;
				return;
			}
			filtered->pts = framesWritten++;
			if (avcodec_send_frame(encCtx.get(), filtered.get()) < 0) {
				error = "GIF encode failed while sending a frame";
				failed = true;
				av_frame_unref(filtered.get());
				return;
			}
			av_frame_unref(filtered.get());
			PacketPtr outPacket(av_packet_alloc());
			while (avcodec_receive_packet(encCtx.get(), outPacket.get()) == 0) {
				outPacket->stream_index = outStream->index;
				av_packet_rescale_ts(outPacket.get(), encCtx->time_base, outStream->time_base);
				if (av_interleaved_write_frame(outCtx.get(), outPacket.get()) < 0) {
					error = "Writing the GIF failed";
					failed = true;
					break;
				}
				outPacket.reset(av_packet_alloc());
			}
			if (progress && totalFrames > 0 && framesWritten % 20 == 0) {
				progress(std::min(1.0, double(framesWritten) / double(totalFrames)));
			}
		}
	};

	bool inputEof = false;
	while (!failed && !inputEof) {
		ret = av_read_frame(inCtx.get(), packet.get());
		if (ret == AVERROR_EOF) {
			inputEof = true;
		} else if (ret < 0) {
			error = "Reading the recording failed: " + AvError(ret);
			break;
		}
		if (!inputEof && packet->stream_index != videoIndex) {
			av_packet_unref(packet.get());
			continue;
		}
		if ((ret = avcodec_send_packet(decCtx.get(), inputEof ? nullptr : packet.get())) < 0) {
			error = "Decode failed while sending a packet";
			break;
		}
		av_packet_unref(packet.get());
		while (avcodec_receive_frame(decCtx.get(), frame.get()) == 0) {
			if ((ret = av_buffersrc_add_frame_flags(srcCtx, frame.get(), AV_BUFFERSRC_FLAG_KEEP_REF)) < 0) {
				error = "Filtering failed while adding a frame: " + AvError(ret);
				failed = true;
				break;
			}
			av_frame_unref(frame.get());
			encodeFiltered();
			if (failed) {
				break;
			}
		}
	}

	// Drain the filter graph and the encoder.
	if (!failed && error.empty()) {
		(void)av_buffersrc_add_frame_flags(srcCtx, nullptr, 0);
		encodeFiltered();
	}
	if (!failed && error.empty()) {
		avcodec_send_frame(encCtx.get(), nullptr);
		PacketPtr outPacket(av_packet_alloc());
		while (avcodec_receive_packet(encCtx.get(), outPacket.get()) == 0) {
			outPacket->stream_index = outStream->index;
			av_packet_rescale_ts(outPacket.get(), encCtx->time_base, outStream->time_base);
			av_interleaved_write_frame(outCtx.get(), outPacket.get());
			outPacket.reset(av_packet_alloc());
		}
		av_write_trailer(outCtx.get());
	}

	if (outCtx->pb) {
		avio_closep(&outCtx->pb);
	}
	if (!error.empty()) {
		return false;
	}
	if (framesWritten == 0) {
		return fail("No frames were converted");
	}
	if (progress) {
		progress(1.0);
	}
	return true;
}

} // namespace snip
