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
#include <libavutil/audio_fifo.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
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

namespace {

struct SwrDeleter {
	void operator()(SwrContext *p) const { swr_free(&p); }
};
struct AudioFifoDeleter {
	void operator()(AVAudioFifo *p) const { av_audio_fifo_free(p); }
};
using SwrPtr = std::unique_ptr<SwrContext, SwrDeleter>;
using AudioFifoPtr = std::unique_ptr<AVAudioFifo, AudioFifoDeleter>;

// Sends one frame (nullptr to flush) to an encoder and writes every
// packet it produces. Returns false and sets `error` on failure.
bool EncodeFrame(AVCodecContext *encCtx, AVStream *outStream, AVFormatContext *outCtx, AVFrame *frame,
		 std::string &error)
{
	if (avcodec_send_frame(encCtx, frame) < 0) {
		error = "MP4 encode failed while sending a frame";
		return false;
	}
	PacketPtr packet(av_packet_alloc());
	while (true) {
		const int ret = avcodec_receive_packet(encCtx, packet.get());
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
			return true;
		}
		if (ret < 0) {
			error = "MP4 encode failed: " + AvError(ret);
			return false;
		}
		packet->stream_index = outStream->index;
		av_packet_rescale_ts(packet.get(), encCtx->time_base, outStream->time_base);
		if (av_interleaved_write_frame(outCtx, packet.get()) < 0) {
			error = "Writing the MP4 failed";
			return false;
		}
		packet.reset(av_packet_alloc());
	}
}

} // namespace

bool ConvertToMp4(const std::string &inputPath, const std::string &outputPath, const Mp4ExportOptions &options,
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
	const int audioIndex = av_find_best_stream(inCtx.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
	AVStream *inVideo = inCtx->streams[videoIndex];
	AVStream *inAudio = audioIndex >= 0 ? inCtx->streams[audioIndex] : nullptr;

	const AVCodec *videoDecoder = avcodec_find_decoder(inVideo->codecpar->codec_id);
	if (!videoDecoder) {
		return fail("No decoder for the recording's video codec");
	}
	CodecCtxPtr decVideo(avcodec_alloc_context3(videoDecoder));
	if (!decVideo || avcodec_parameters_to_context(decVideo.get(), inVideo->codecpar) < 0 ||
	    (ret = avcodec_open2(decVideo.get(), videoDecoder, nullptr)) < 0) {
		return fail("Could not open the video decoder: " + AvError(ret));
	}

	// Optional audio decoder; losing audio is not fatal here, the
	// caller asked for an MP4 of a screen recording first of all.
	CodecCtxPtr decAudio;
	if (inAudio) {
		if (const AVCodec *audioDecoder = avcodec_find_decoder(inAudio->codecpar->codec_id)) {
			decAudio.reset(avcodec_alloc_context3(audioDecoder));
			if (!decAudio || avcodec_parameters_to_context(decAudio.get(), inAudio->codecpar) < 0 ||
			    avcodec_open2(decAudio.get(), audioDecoder, nullptr) < 0) {
				decAudio.reset();
			}
		}
	}

	const AVRational frameRate = [&] {
		const AVRational guessed = av_guess_frame_rate(inCtx.get(), inVideo, nullptr);
		return guessed.num > 0 && guessed.den > 0 ? guessed : AVRational{30, 1};
	}();
	const int outWidth = std::max(2, decVideo->width & ~1);
	const int outHeight = std::max(2, decVideo->height & ~1);

	// Video filter: convert to yuv420p at even dimensions, which
	// H.264 in MP4 requires.
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
	const AVRational sar = decVideo->sample_aspect_ratio.num > 0 ? decVideo->sample_aspect_ratio : AVRational{1, 1};
	char srcArgs[512];
	std::snprintf(srcArgs, sizeof(srcArgs),
		      "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d:frame_rate=%d/%d",
		      decVideo->width, decVideo->height, int(decVideo->pix_fmt), inVideo->time_base.num,
		      inVideo->time_base.den, sar.num, sar.den, frameRate.num, frameRate.den);
	if ((ret = avfilter_graph_create_filter(&srcCtx, bufferSrc, "in", srcArgs, nullptr, graph.get())) < 0 ||
	    (ret = avfilter_graph_create_filter(&sinkCtx, bufferSink, "out", nullptr, nullptr, graph.get())) < 0) {
		return fail("Could not create filters: " + AvError(ret));
	}
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
	ret = avfilter_graph_parse_ptr(graph.get(),
				       "format=pix_fmts=yuv420p,scale=w=trunc(iw/2)*2:h=trunc(ih/2)*2:flags=lanczos",
				       &graphIn, &graphOut, nullptr);
	avfilter_inout_free(&graphIn);
	avfilter_inout_free(&graphOut);
	if (ret < 0 || (ret = avfilter_graph_config(graph.get(), nullptr)) < 0) {
		return fail("Could not configure the MP4 filter graph: " + AvError(ret));
	}

	const AVCodec *videoEncoder = avcodec_find_encoder_by_name("libx264");
	const bool useX264 = videoEncoder != nullptr;
	if (!videoEncoder) {
		videoEncoder = avcodec_find_encoder(AV_CODEC_ID_H264);
	}
	if (!videoEncoder) {
		return fail("No H.264 encoder available for MP4 export");
	}
	CodecCtxPtr encVideo(avcodec_alloc_context3(videoEncoder));
	if (!encVideo) {
		return fail("Out of memory");
	}
	encVideo->width = outWidth;
	encVideo->height = outHeight;
	encVideo->pix_fmt = AV_PIX_FMT_YUV420P;
	encVideo->time_base = av_inv_q(frameRate);
	encVideo->framerate = frameRate;
	if (useX264) {
		av_opt_set(encVideo->priv_data, "preset", "medium", 0);
		av_opt_set_int(encVideo->priv_data, "crf", std::clamp(options.crf, 0, 51), 0);
	} else {
		encVideo->bit_rate = inVideo->codecpar->bit_rate > 0 ? inVideo->codecpar->bit_rate : 8000000;
	}

	AVFormatContext *rawOut = nullptr;
	ret = avformat_alloc_output_context2(&rawOut, nullptr, "mp4", outputPath.c_str());
	if (ret < 0 || !rawOut) {
		return fail("Could not create the MP4 file: " + AvError(ret));
	}
	FormatCtxPtr outCtx(rawOut);
	if (outCtx->oformat->flags & AVFMT_GLOBALHEADER) {
		encVideo->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	}
	AVStream *outVideo = avformat_new_stream(outCtx.get(), nullptr);
	if (!outVideo) {
		return fail("Could not create the MP4 video stream");
	}
	if ((ret = avcodec_open2(encVideo.get(), videoEncoder, nullptr)) < 0) {
		return fail("Could not open the H.264 encoder: " + AvError(ret));
	}
	if ((ret = avcodec_parameters_from_context(outVideo->codecpar, encVideo.get())) < 0) {
		return fail("Could not set up the MP4 video stream: " + AvError(ret));
	}
	outVideo->time_base = encVideo->time_base;
	outVideo->avg_frame_rate = frameRate;
	outVideo->r_frame_rate = frameRate;

	// Audio encoder (AAC) + resampler into the encoder's layout.
	CodecCtxPtr encAudio;
	SwrPtr swr;
	AudioFifoPtr fifo;
	AVStream *outAudio = nullptr;
	int audioFrameSize = 0;
	if (decAudio) {
		const AVCodec *audioEncoder = avcodec_find_encoder(AV_CODEC_ID_AAC);
		if (audioEncoder) {
			encAudio.reset(avcodec_alloc_context3(audioEncoder));
		}
		if (encAudio) {
			const AVSampleFormat *sampleFmts = nullptr;
			int sampleFmtCount = 0;
			encAudio->sample_fmt =
				avcodec_get_supported_config(nullptr, audioEncoder, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0,
							     reinterpret_cast<const void **>(&sampleFmts),
							     &sampleFmtCount) >= 0 &&
						sampleFmts && sampleFmtCount > 0
					? sampleFmts[0]
					: AV_SAMPLE_FMT_FLTP;
			encAudio->sample_rate = decAudio->sample_rate > 0 ? decAudio->sample_rate : 48000;
			if (av_channel_layout_copy(&encAudio->ch_layout, &decAudio->ch_layout) < 0 ||
			    encAudio->ch_layout.nb_channels <= 0) {
				av_channel_layout_default(&encAudio->ch_layout, 2);
			}
			encAudio->time_base = {1, encAudio->sample_rate};
			if (outCtx->oformat->flags & AVFMT_GLOBALHEADER) {
				encAudio->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
			}
			outAudio = avformat_new_stream(outCtx.get(), nullptr);
			if (!outAudio || (ret = avcodec_open2(encAudio.get(), audioEncoder, nullptr)) < 0 ||
			    avcodec_parameters_from_context(outAudio->codecpar, encAudio.get()) < 0) {
				encAudio.reset();
				outAudio = nullptr;
			} else {
				outAudio->time_base = encAudio->time_base;
				audioFrameSize = encAudio->frame_size > 0 ? encAudio->frame_size : 1024;

				SwrContext *rawSwr = nullptr;
				if (swr_alloc_set_opts2(&rawSwr, &encAudio->ch_layout, encAudio->sample_fmt,
							encAudio->sample_rate, &decAudio->ch_layout,
							decAudio->sample_fmt, decAudio->sample_rate, 0, nullptr) < 0 ||
				    !rawSwr || swr_init(rawSwr) < 0) {
					swr_free(&rawSwr);
					encAudio.reset();
					outAudio = nullptr;
				} else {
					swr.reset(rawSwr);
					fifo.reset(av_audio_fifo_alloc(encAudio->sample_fmt,
								       encAudio->ch_layout.nb_channels, audioFrameSize * 2));
					if (!fifo) {
						encAudio.reset();
						outAudio = nullptr;
					}
				}
			}
		}
	}

	if ((ret = avio_open(&outCtx->pb, outputPath.c_str(), AVIO_FLAG_WRITE)) < 0) {
		return fail("Could not write the MP4 file: " + AvError(ret));
	}
	AVDictionary *headerOptions = nullptr;
	av_dict_set(&headerOptions, "movflags", "+faststart", 0);
	ret = avformat_write_header(outCtx.get(), &headerOptions);
	av_dict_free(&headerOptions);
	if (ret < 0) {
		avio_closep(&outCtx->pb);
		return fail("Could not write the MP4 header: " + AvError(ret));
	}

	int64_t totalFrames = inVideo->nb_frames;
	if (totalFrames <= 0 && inCtx->duration > 0) {
		totalFrames = int64_t(inCtx->duration * av_q2d(frameRate) / AV_TIME_BASE);
	}

	PacketPtr packet(av_packet_alloc());
	FramePtr frame(av_frame_alloc());
	FramePtr filtered(av_frame_alloc());
	if (!packet || !frame || !filtered) {
		return fail("Out of memory");
	}

	int64_t videoFramesWritten = 0;
	int64_t audioSamplesWritten = 0;
	bool failed = false;

	auto encodeFilteredVideo = [&]() {
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
			if (filtered->pts != AV_NOPTS_VALUE) {
				filtered->pts = av_rescale_q(filtered->pts, inVideo->time_base, encVideo->time_base);
			} else {
				filtered->pts = videoFramesWritten;
			}
			videoFramesWritten++;
			if (!EncodeFrame(encVideo.get(), outVideo, outCtx.get(), filtered.get(), error)) {
				failed = true;
			}
			av_frame_unref(filtered.get());
			if (progress && totalFrames > 0 && videoFramesWritten % 20 == 0) {
				progress(std::min(1.0, double(videoFramesWritten) / double(totalFrames)));
			}
			if (failed) {
				return;
			}
		}
	};

	// Pulls fixed-size frames out of the audio FIFO into the AAC
	// encoder. With `flush`, a final partial frame is allowed.
	FramePtr audioFrame(av_frame_alloc());
	auto drainAudioFifo = [&](bool flush) {
		while (!failed && audioFrame) {
			const int available = av_audio_fifo_size(fifo.get());
			if (available < audioFrameSize && !(flush && available > 0)) {
				return;
			}
			const int toRead = std::min(available, audioFrameSize);
			av_frame_unref(audioFrame.get());
			audioFrame->format = encAudio->sample_fmt;
			audioFrame->sample_rate = encAudio->sample_rate;
			audioFrame->nb_samples = toRead;
			if (av_channel_layout_copy(&audioFrame->ch_layout, &encAudio->ch_layout) < 0 ||
			    av_frame_get_buffer(audioFrame.get(), 0) < 0) {
				error = "Out of memory";
				failed = true;
				return;
			}
			if (av_audio_fifo_read(fifo.get(), reinterpret_cast<void **>(audioFrame->data), toRead) < toRead) {
				error = "MP4 audio conversion failed";
				failed = true;
				return;
			}
			audioFrame->pts = audioSamplesWritten;
			audioSamplesWritten += toRead;
			if (!EncodeFrame(encAudio.get(), outAudio, outCtx.get(), audioFrame.get(), error)) {
				failed = true;
				return;
			}
		}
	};

	auto convertAudioFrame = [&](AVFrame *decoded) {
		FramePtr converted(av_frame_alloc());
		if (!converted) {
			error = "Out of memory";
			failed = true;
			return;
		}
		converted->format = encAudio->sample_fmt;
		converted->sample_rate = encAudio->sample_rate;
		if (av_channel_layout_copy(&converted->ch_layout, &encAudio->ch_layout) < 0) {
			error = "Out of memory";
			failed = true;
			return;
		}
		converted->nb_samples = int(av_rescale_rnd(swr_get_delay(swr.get(), decAudio->sample_rate) + decoded->nb_samples,
							   encAudio->sample_rate, decAudio->sample_rate, AV_ROUND_UP));
		if (av_frame_get_buffer(converted.get(), 0) < 0) {
			error = "Out of memory";
			failed = true;
			return;
		}
		const int convertedSamples = swr_convert(swr.get(), converted->data, converted->nb_samples,
							 const_cast<const uint8_t **>(decoded->data), decoded->nb_samples);
		if (convertedSamples < 0) {
			error = "MP4 audio resampling failed";
			failed = true;
			return;
		}
		converted->nb_samples = convertedSamples;
		if (av_audio_fifo_write(fifo.get(), reinterpret_cast<void **>(converted->data), convertedSamples) < convertedSamples) {
			error = "Out of memory";
			failed = true;
			return;
		}
		drainAudioFifo(false);
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

		const bool isVideo = !inputEof && packet->stream_index == videoIndex;
		const bool isAudio = !inputEof && encAudio && packet->stream_index == audioIndex;
		if (!inputEof && !isVideo && !isAudio) {
			av_packet_unref(packet.get());
			continue;
		}

		AVCodecContext *decCtx = isAudio ? decAudio.get() : decVideo.get();
		if ((ret = avcodec_send_packet(decCtx, inputEof ? nullptr : packet.get())) < 0) {
			error = "Decode failed while sending a packet";
			break;
		}
		av_packet_unref(packet.get());
		while (avcodec_receive_frame(decCtx, frame.get()) == 0) {
			if (isAudio) {
				convertAudioFrame(frame.get());
			} else {
				if ((ret = av_buffersrc_add_frame_flags(srcCtx, frame.get(), AV_BUFFERSRC_FLAG_KEEP_REF)) < 0) {
					error = "Filtering failed while adding a frame: " + AvError(ret);
					failed = true;
				} else {
					encodeFilteredVideo();
				}
			}
			av_frame_unref(frame.get());
			if (failed) {
				break;
			}
		}
		// An EOF packet flushes only the stream it was sent to;
		// the other decoder is flushed in the drain below by
		// re-entering this loop is not possible, so flush the
		// remaining decoder explicitly when the file ends.
		if (inputEof) {
			break;
		}
	}

	// Drain the audio decoder (video was flushed by the EOF packet
	// above when the last packet happened to be video; flush both
	// defensively -- a second flush is a no-op for libavcodec).
	if (!failed && error.empty() && encAudio) {
		if (avcodec_send_packet(decAudio.get(), nullptr) == 0) {
			while (avcodec_receive_frame(decAudio.get(), frame.get()) == 0) {
				convertAudioFrame(frame.get());
				av_frame_unref(frame.get());
				if (failed) {
					break;
				}
			}
		}
		if (!failed) {
			// Flush resampler delay into the FIFO, then encode
			// everything that is left.
			FramePtr tail(av_frame_alloc());
			if (tail) {
				tail->format = encAudio->sample_fmt;
				tail->sample_rate = encAudio->sample_rate;
				if (av_channel_layout_copy(&tail->ch_layout, &encAudio->ch_layout) == 0) {
					tail->nb_samples = int(av_rescale_rnd(swr_get_delay(swr.get(), decAudio->sample_rate),
									      encAudio->sample_rate, decAudio->sample_rate,
									      AV_ROUND_UP)) +
							   1024;
					if (tail->nb_samples > 0 && av_frame_get_buffer(tail.get(), 0) == 0) {
						const int flushed = swr_convert(swr.get(), tail->data, tail->nb_samples, nullptr, 0);
						if (flushed > 0) {
							tail->nb_samples = flushed;
							(void)av_audio_fifo_write(fifo.get(),
										  reinterpret_cast<void **>(tail->data), flushed);
						}
					}
				}
			}
			drainAudioFifo(true);
		}
	}

	if (!failed && error.empty()) {
		(void)av_buffersrc_add_frame_flags(srcCtx, nullptr, 0);
		encodeFilteredVideo();
	}
	if (!failed && error.empty()) {
		if (!EncodeFrame(encVideo.get(), outVideo, outCtx.get(), nullptr, error)) {
			failed = true;
		}
	}
	if (!failed && error.empty() && encAudio) {
		if (!EncodeFrame(encAudio.get(), outAudio, outCtx.get(), nullptr, error)) {
			failed = true;
		}
	}
	if (!failed && error.empty()) {
		av_write_trailer(outCtx.get());
	}

	if (outCtx->pb) {
		avio_closep(&outCtx->pb);
	}
	if (!error.empty()) {
		return false;
	}
	if (videoFramesWritten == 0) {
		return fail("No frames were converted");
	}
	if (progress) {
		progress(1.0);
	}
	return true;
}

} // namespace snip
