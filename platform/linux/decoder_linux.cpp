// Audio file decoding and tag reading through FFmpeg (libavformat, libavcodec, libswresample), linked
// dynamically (LGPL). Handles MP3, AAC/M4A, FLAC, WAV, AIFF, Ogg Vorbis/Opus and anything else FFmpeg opens.
// Output matches the Apple decoder: interleaved stereo float at the file's own sample rate.
#include <algorithm>
#include <memory>
#include <mutex>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include "deckhand/audio/track.hpp"

namespace deckhand {
namespace {

struct FormatDeleter {
  void operator()(AVFormatContext* c) const { avformat_close_input(&c); }
};
struct CodecDeleter {
  void operator()(AVCodecContext* c) const { avcodec_free_context(&c); }
};
struct SwrDeleter {
  void operator()(SwrContext* c) const { swr_free(&c); }
};
struct PacketDeleter {
  void operator()(AVPacket* p) const { av_packet_free(&p); }
};
struct FrameDeleter {
  void operator()(AVFrame* f) const { av_frame_free(&f); }
};
using FormatPtr = std::unique_ptr<AVFormatContext, FormatDeleter>;
using CodecPtr = std::unique_ptr<AVCodecContext, CodecDeleter>;
using SwrPtr = std::unique_ptr<SwrContext, SwrDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;

std::string av_error(int code) {
  char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
  av_strerror(code, buf, sizeof buf);
  return buf;
}

void quiet_ffmpeg() {
  static std::once_flag once;
  std::call_once(once, [] { av_log_set_level(AV_LOG_ERROR); });
}

// "file:" keeps a path such as "a:b.mp3" from being read as a protocol name.
int open_input(const std::string& path, FormatPtr& out) {
  quiet_ffmpeg();
  AVFormatContext* raw = nullptr;
  const int r = avformat_open_input(&raw, ("file:" + path).c_str(), nullptr, nullptr);
  if (r < 0) return r;
  out.reset(raw);
  return avformat_find_stream_info(raw, nullptr);
}

std::string tag(const AVDictionary* m, const char* key) {
  const AVDictionaryEntry* e = m ? av_dict_get(m, key, nullptr, 0) : nullptr;
  return e && e->value ? e->value : std::string();
}

// Appends `frames` interleaved stereo frames from `in` (mono is duplicated, not attenuated).
void append_stereo(std::vector<float>& pcm, const float* in, int frames, int in_channels) {
  const std::size_t at = pcm.size();
  pcm.resize(at + static_cast<std::size_t>(frames) * 2);
  float* out = pcm.data() + at;
  if (in_channels == 1) {
    for (int i = 0; i < frames; ++i) out[2 * i] = out[2 * i + 1] = in[i];
  } else {
    std::copy(in, in + static_cast<std::size_t>(frames) * 2, out);
  }
}

}  // namespace

std::shared_ptr<TrackAudio> decode_file(const std::string& path, std::string& error) {
  FormatPtr format;
  int r = open_input(path, format);
  if (r < 0 || !format) {
    error = "cannot open audio file (" + av_error(r) + "): " + path;
    return nullptr;
  }
  const AVCodec* codec = nullptr;
  const int stream_index = av_find_best_stream(format.get(), AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
  if (stream_index < 0 || !codec) {
    error = "no audio stream: " + path;
    return nullptr;
  }
  const AVStream* stream = format->streams[stream_index];
  CodecPtr ctx(avcodec_alloc_context3(codec));
  if (!ctx || avcodec_parameters_to_context(ctx.get(), stream->codecpar) < 0) {
    error = "unsupported audio format: " + path;
    return nullptr;
  }
  ctx->pkt_timebase = stream->time_base;
  r = avcodec_open2(ctx.get(), codec, nullptr);
  if (r < 0) {
    error = "unsupported audio format (" + av_error(r) + "): " + path;
    return nullptr;
  }
  const int rate = ctx->sample_rate;
  if (rate <= 0) {
    error = "unknown sample rate: " + path;
    return nullptr;
  }

  AVChannelLayout in_layout{};
  if (ctx->ch_layout.nb_channels > 0 && ctx->ch_layout.order != AV_CHANNEL_ORDER_UNSPEC) {
    av_channel_layout_copy(&in_layout, &ctx->ch_layout);
  } else {
    av_channel_layout_default(&in_layout, std::max(1, ctx->ch_layout.nb_channels));
  }
  const int in_channels = in_layout.nb_channels;
  AVChannelLayout out_layout{};
  if (in_channels == 1) av_channel_layout_default(&out_layout, 1);
  else av_channel_layout_from_mask(&out_layout, AV_CH_LAYOUT_STEREO);
  SwrContext* raw_swr = nullptr;
  r = swr_alloc_set_opts2(&raw_swr, &out_layout, AV_SAMPLE_FMT_FLT, rate, &in_layout, ctx->sample_fmt, rate, 0,
                          nullptr);
  SwrPtr swr(raw_swr);
  av_channel_layout_uninit(&in_layout);
  const int out_channels = out_layout.nb_channels;
  av_channel_layout_uninit(&out_layout);
  if (r < 0 || !swr || swr_init(swr.get()) < 0) {
    error = "unsupported channel layout: " + path;
    return nullptr;
  }

  auto audio = std::make_shared<TrackAudio>();
  audio->sample_rate = static_cast<std::uint32_t>(rate);
  if (format->duration > 0) {
    const double est = static_cast<double>(format->duration) / AV_TIME_BASE * rate;
    if (est < 1e9) audio->pcm.reserve(static_cast<std::size_t>(est) * 2 + 8192);
  }

  PacketPtr packet(av_packet_alloc());
  FramePtr frame(av_frame_alloc());
  if (!packet || !frame) {
    error = "out of memory: " + path;
    return nullptr;
  }
  std::vector<float> scratch;
  bool failed = false;

  auto drain = [&]() {
    for (;;) {
      const int got = avcodec_receive_frame(ctx.get(), frame.get());
      if (got == AVERROR(EAGAIN) || got == AVERROR_EOF) return;
      if (got < 0) {
        failed = true;
        return;
      }
      const int cap = swr_get_out_samples(swr.get(), frame->nb_samples);
      if (cap > 0) {
        scratch.resize(static_cast<std::size_t>(cap) * out_channels);
        uint8_t* dst[1] = {reinterpret_cast<uint8_t*>(scratch.data())};
        const int n = swr_convert(swr.get(), dst, cap, const_cast<const uint8_t**>(frame->extended_data),
                                  frame->nb_samples);
        if (n > 0) append_stereo(audio->pcm, scratch.data(), n, out_channels);
      }
      av_frame_unref(frame.get());
    }
  };

  while (!failed) {
    const int read = av_read_frame(format.get(), packet.get());
    if (read < 0) break;  // end of file (or an unreadable tail: keep what was decoded)
    if (packet->stream_index == stream_index) {
      const int sent = avcodec_send_packet(ctx.get(), packet.get());
      // A corrupt packet is skipped; the decoder resynchronises on the next one.
      if (sent >= 0 || sent == AVERROR_INVALIDDATA) drain();
      else if (sent != AVERROR(EAGAIN)) failed = true;
    }
    av_packet_unref(packet.get());
  }
  if (!failed) {
    avcodec_send_packet(ctx.get(), nullptr);
    drain();
  }
  // Flush any samples swresample still holds (none at equal rates, but cheap to be exact).
  for (;;) {
    scratch.resize(static_cast<std::size_t>(4096) * out_channels);
    uint8_t* dst[1] = {reinterpret_cast<uint8_t*>(scratch.data())};
    const int n = swr_convert(swr.get(), dst, 4096, nullptr, 0);
    if (n <= 0) break;
    append_stereo(audio->pcm, scratch.data(), n, out_channels);
  }

  audio->frames = audio->pcm.size() / 2;
  audio->pcm.shrink_to_fit();
  if (audio->frames == 0) {
    error = (failed ? "decode failed: " : "file contains no audio: ") + path;
    return nullptr;
  }
  build_waveform(*audio);
  return audio;
}

bool read_file_tags(const std::string& path, FileTags& out) {
  FormatPtr format;
  if (open_input(path, format) < 0 || !format) return false;
  const int idx = av_find_best_stream(format.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  const AVDictionary* streams = idx >= 0 ? format->streams[idx]->metadata : nullptr;  // Ogg keeps tags here
  auto pick = [&](const char* key) {
    std::string v = tag(format->metadata, key);
    return v.empty() ? tag(streams, key) : v;
  };
  out.title = pick("title");
  out.artist = pick("artist");
  out.album = pick("album");
  if (format->duration > 0) out.duration_ms = static_cast<std::int64_t>(format->duration / 1000);
  return true;
}

}  // namespace deckhand
