#include "player/decoder.h"
#include "util/util.h"

#include <ctype.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <stdlib.h>
#include <string.h>

#define LOG_MODULE "decoder"
#define FILE_TIMEOUT_MS 10000
#define STREAM_TIMEOUT_MS 15000

struct Decoder {
  AVFormatContext *fmt;
  AVCodecContext *dec;
  SwrContext *swr;
  AVPacket *pkt;
  AVFrame *frame;
  int sidx;
  AVRational tb;
  int64_t start_pts;
  PcmFmt out;
  enum AVSampleFormat out_fmt;
  AVChannelLayout out_layout;
  int swr_in_fmt;
  int swr_in_rate;
  AVChannelLayout swr_in_layout;
  int bpf;
  bool flushing;
  bool eof;
  uint8_t *buf;
  int buf_frames;
  char *path;
  bool stream;
  const atomic_bool *abort;
  char desc[96];
  char icy[200];
  bool icy_changed;
  unsigned icy_tick;
  uint64_t deadline;
};

static bool aborted(const Decoder *d) {
  return atomic_load(d->abort);
}

static int interrupt_cb(void *opaque) {
  const Decoder *d = opaque;
  return aborted(d) || mono_ms() > d->deadline;
}

static void arm_deadline(Decoder *d) {
  d->deadline = mono_ms() + (d->stream ? STREAM_TIMEOUT_MS : FILE_TIMEOUT_MS);
}

static enum AVSampleFormat pick_format(const AVCodecContext *dec) {
  enum AVSampleFormat packed = av_get_packed_sample_fmt(dec->sample_fmt);
  switch (packed) {
    case AV_SAMPLE_FMT_U8:
    case AV_SAMPLE_FMT_S16:
      return AV_SAMPLE_FMT_S16;
    case AV_SAMPLE_FMT_S32:
    case AV_SAMPLE_FMT_S64:
      return (dec->bits_per_raw_sample > 0 && dec->bits_per_raw_sample <= 16) ? AV_SAMPLE_FMT_S16 : AV_SAMPLE_FMT_S32;
    default: {
      const AVCodecDescriptor *cd = avcodec_descriptor_get(dec->codec_id);
      return (cd && (cd->props & AV_CODEC_PROP_LOSSLESS)) ? AV_SAMPLE_FMT_S32 : AV_SAMPLE_FMT_S16;
    }
  }
}

static void describe(Decoder *d) {
  char codec[32];
  snprintf(codec, sizeof(codec), "%s", avcodec_get_name(d->dec->codec_id));
  for (char *p = codec; *p; p++) {
    *p = (char)toupper((unsigned char)*p);
  }
  int bits = d->dec->bits_per_raw_sample;
  const AVCodecDescriptor *cd = avcodec_descriptor_get(d->dec->codec_id);
  bool lossless = cd && (cd->props & AV_CODEC_PROP_LOSSLESS);
  if (lossless && bits > 0) {
    snprintf(d->desc, sizeof(d->desc), "%s %dHz %dbit %dch", codec, d->out.rate, bits, d->out.channels);
  } else {
    int64_t br = d->dec->bit_rate ? d->dec->bit_rate : d->fmt->bit_rate;
    snprintf(d->desc, sizeof(d->desc), "%s %dHz %lldkbps %dch", codec, d->out.rate, (long long)(br / 1000), d->out.channels);
  }
}

void decoder_close(Decoder *d) {
  if (!d) {
    return;
  }
  swr_free(&d->swr);
  avcodec_free_context(&d->dec);
  avformat_close_input(&d->fmt);
  av_packet_free(&d->pkt);
  av_frame_free(&d->frame);
  av_channel_layout_uninit(&d->out_layout);
  av_channel_layout_uninit(&d->swr_in_layout);
  free(d->buf);
  free(d->path);
  free(d);
}

Decoder *decoder_open(const char *path, bool stream, uint32_t seek_ms, const atomic_bool *abort) {
  Decoder *d = xcalloc(1, sizeof(*d));
  d->path = xstrdup(path);
  d->stream = stream;
  d->abort = abort;
  d->swr_in_fmt = -1;

  AVDictionary *opts = NULL;
  if (stream) {
    av_dict_set(&opts, "icy", "1", 0);
    av_dict_set(&opts, "reconnect", "1", 0);
    av_dict_set(&opts, "reconnect_delay_max", "5", 0);
    av_dict_set(&opts, "rw_timeout", "15000000", 0);
  }
  d->fmt = avformat_alloc_context();
  if (!d->fmt) {
    av_dict_free(&opts);
    goto fail;
  }
  d->fmt->interrupt_callback = (AVIOInterruptCB){ interrupt_cb, d };
  arm_deadline(d);
  int rc = avformat_open_input(&d->fmt, path, NULL, &opts);
  av_dict_free(&opts);
  if (rc < 0 || avformat_find_stream_info(d->fmt, NULL) < 0) {
    goto fail;
  }
  const AVCodec *codec = NULL;
  d->sidx = av_find_best_stream(d->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
  if (d->sidx < 0 || !codec) {
    goto fail;
  }
  for (unsigned i = 0; i < d->fmt->nb_streams; i++) {
    if ((int)i != d->sidx) {
      d->fmt->streams[i]->discard = AVDISCARD_ALL;
    }
  }
  AVStream *st = d->fmt->streams[d->sidx];
  d->dec = avcodec_alloc_context3(codec);
  if (!d->dec || avcodec_parameters_to_context(d->dec, st->codecpar) < 0) {
    goto fail;
  }
  d->dec->pkt_timebase = st->time_base;
  if (avcodec_open2(d->dec, codec, NULL) < 0) {
    goto fail;
  }

  d->tb = st->time_base;
  d->start_pts = st->start_time != AV_NOPTS_VALUE ? st->start_time : 0;
  d->out.rate = d->dec->sample_rate;
  d->out.channels = d->dec->ch_layout.nb_channels;
  d->out_fmt = pick_format(d->dec);
  d->out.fmt = d->out_fmt == AV_SAMPLE_FMT_S32 ? SF_S32 : SF_S16;
  if (d->out.rate <= 0 || d->out.channels <= 0 || d->out.channels > 8) {
    goto fail;
  }
  if (av_channel_layout_copy(&d->out_layout, &d->dec->ch_layout) < 0 || d->out_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
    av_channel_layout_uninit(&d->out_layout);
    av_channel_layout_default(&d->out_layout, d->out.channels);
  }
  d->bpf = pcm_bpf(d->out);
  d->pkt = av_packet_alloc();
  d->frame = av_frame_alloc();
  if (!d->pkt || !d->frame) {
    goto fail;
  }

  if (seek_ms > 0 && !stream) {
    int64_t ts = av_rescale_q(seek_ms, (AVRational){ 1, 1000 }, st->time_base) + d->start_pts;
    if (av_seek_frame(d->fmt, d->sidx, ts, AVSEEK_FLAG_BACKWARD) >= 0) {
      avcodec_flush_buffers(d->dec);
    }
  }
  describe(d);
  return d;

fail:
  if (!aborted(d)) {
    LOG("%s %s", mono_ms() > d->deadline ? "timed out opening" : "cannot open", path);
  }
  decoder_close(d);
  return NULL;
}

PcmFmt decoder_format(const Decoder *d) {
  return d->out;
}

const char *decoder_desc(const Decoder *d) {
  return d->desc;
}

static bool swr_setup(Decoder *d, const AVFrame *f) {
  if (d->swr && f->format == d->swr_in_fmt && f->sample_rate == d->swr_in_rate && av_channel_layout_compare(&f->ch_layout, &d->swr_in_layout) == 0) {
    return true;
  }
  swr_free(&d->swr);
  if (swr_alloc_set_opts2(&d->swr, &d->out_layout, d->out_fmt, d->out.rate, &f->ch_layout, f->format, f->sample_rate, 0, NULL) < 0 || swr_init(d->swr) < 0) {
    swr_free(&d->swr);
    return false;
  }
  d->swr_in_fmt = f->format;
  d->swr_in_rate = f->sample_rate;
  av_channel_layout_uninit(&d->swr_in_layout);
  av_channel_layout_copy(&d->swr_in_layout, &f->ch_layout);
  return true;
}

static int convert(Decoder *d, uint8_t **out, int64_t *pos) {
  AVFrame *f = d->frame;
  if (!swr_setup(d, f)) {
    return AVERROR(EINVAL);
  }
  int max_out = swr_get_out_samples(d->swr, f->nb_samples);
  if (max_out > d->buf_frames) {
    d->buf = xrealloc(d->buf, (size_t)max_out * (size_t)d->bpf);
    d->buf_frames = max_out;
  }
  int n = swr_convert(d->swr, &d->buf, max_out, (const uint8_t **)f->extended_data, f->nb_samples);
  if (n <= 0) {
    return n;
  }
  *pos = f->best_effort_timestamp != AV_NOPTS_VALUE ? av_rescale_q(f->best_effort_timestamp - d->start_pts, d->tb, (AVRational){ 1, d->out.rate }) : -1;
  *out = d->buf;
  return n;
}

static void check_icy(Decoder *d) {
  uint8_t *meta = NULL;
  if (av_opt_get(d->fmt->pb, "icy_metadata_packet", AV_OPT_SEARCH_CHILDREN, &meta) < 0 || !meta) {
    return;
  }
  const char *p = strstr((const char *)meta, "StreamTitle='");
  if (p) {
    p += 13;
    const char *e = strstr(p, "';");
    size_t len = e ? (size_t)(e - p) : strlen(p);
    if (len >= sizeof(d->icy)) {
      len = sizeof(d->icy) - 1;
    }
    if (strncmp(d->icy, p, len) != 0 || d->icy[len] != '\0') {
      memcpy(d->icy, p, len);
      d->icy[len] = '\0';
      d->icy_changed = true;
    }
  }
  av_free(meta);
}

bool decoder_icy(Decoder *d, char *buf, size_t n) {
  if (!d->icy_changed) {
    return false;
  }
  d->icy_changed = false;
  snprintf(buf, n, "%s", d->icy);
  return true;
}

static int fail(Decoder *d, int ret) {
  if (!aborted(d)) {
    char msg[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, msg, sizeof(msg));
    LOG("%s in %s", mono_ms() > d->deadline ? "read timed out" : msg, d->path);
  }
  return -1;
}

int decoder_read(Decoder *d, uint8_t **out, int64_t *pos) {
  for (;;) {
    if (d->eof) {
      return 0;
    }
    int ret = avcodec_receive_frame(d->dec, d->frame);
    if (ret == AVERROR(EAGAIN)) {
      if (d->flushing) {
        d->eof = true;
        return 0;
      }
      arm_deadline(d);
      ret = av_read_frame(d->fmt, d->pkt);
      if (ret == AVERROR_EOF || (ret < 0 && d->fmt->pb && avio_feof(d->fmt->pb))) {
        avcodec_send_packet(d->dec, NULL);
        d->flushing = true;
        continue;
      }
      if (ret == AVERROR(EAGAIN)) {
        continue;
      }
      if (ret < 0) {
        return fail(d, ret);
      }
      if (d->pkt->stream_index == d->sidx) {
        avcodec_send_packet(d->dec, d->pkt);
      }
      av_packet_unref(d->pkt);
      if (d->stream && (++d->icy_tick % 32) == 0) {
        check_icy(d);
      }
      continue;
    }
    if (ret == AVERROR_EOF) {
      d->eof = true;
      return 0;
    }
    if (ret == AVERROR_INVALIDDATA) {
      continue;
    }
    if (ret < 0) {
      return fail(d, ret);
    }
    int frames = convert(d, out, pos);
    av_frame_unref(d->frame);
    if (frames < 0) {
      return fail(d, frames);
    }
    if (frames > 0) {
      return frames;
    }
  }
}
