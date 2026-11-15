#include "player/audio.h"
#include "util/util.h"

#include <alsa/asoundlib.h>
#include <ctype.h>
#include <errno.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#define LOG_MODULE "audio"
#define EVENT_RING 64
#define MAX_BOUNDARIES 4
#define BUFFER_US 500000
#define PERIOD_US 50000

typedef struct {
  uint64_t token;
  char *path;
  uint32_t begin_ms;
  uint32_t end_ms;
  bool stream;
} Item;

typedef struct {
  int rate;
  int channels;
  enum AVSampleFormat fmt;
} PcmFmt;

typedef struct {
  AVFormatContext *fmt;
  AVCodecContext *dec;
  SwrContext *swr;
  AVPacket *pkt;
  AVFrame *frame;
  int sidx;
  AVRational tb;
  int64_t start_pts;
  PcmFmt out;
  AVChannelLayout out_layout;
  int swr_in_fmt;
  int swr_in_rate;
  AVChannelLayout swr_in_layout;
  int bpf;
  int64_t skip_until;
  int64_t end_sample;
  int64_t next_sample;
  bool flushing;
  bool eof;
  uint8_t *buf;
  int buf_frames;
  Item item;
  char desc[96];
  char icy[200];
  bool icy_changed;
  unsigned icy_tick;
} Decoder;

typedef struct {
  Item item;
  uint64_t frame_at;
  uint32_t start_ms;
  char desc[96];
} Boundary;

static struct {
  pthread_t thread;
  pthread_mutex_t mu;
  pthread_cond_t cv;
  int efd;
  bool running;
  char *device;

  bool c_quit;
  char *c_device;
  bool c_stop;
  bool c_play;
  bool c_play_paused;
  uint32_t c_play_start;
  Item c_play_item;
  bool c_next;
  bool c_next_has;
  uint64_t c_next_for;
  Item c_next_item;
  int c_pause;

  AudioEvent ev[EVENT_RING];
  unsigned ev_head;
  unsigned ev_tail;

  atomic_int volume;
  atomic_uint pos_ms;
} A = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER, .efd = -1, .c_pause = -1 };

static struct {
  snd_pcm_t *pcm;
  PcmFmt fmt;
  int bpf;
  snd_pcm_uframes_t period;
  bool can_pause;
  bool hw_paused;
  uint64_t written;
  Decoder *cur;
  bool paused;
  bool waiting;
  uint64_t waiting_for;
  bool reopen_on_resume;
  uint32_t reopen_pos;
  Boundary b[MAX_BOUNDARIES];
  int nb;
  Boundary audible;
  bool has_audible;
} T;

static void item_set(Item *dst, const AudioItem *src) {
  free(dst->path);
  dst->token = src->token;
  dst->path = xstrdup(src->path);
  dst->begin_ms = src->begin_ms;
  dst->end_ms = src->end_ms;
  dst->stream = src->stream;
}

static void item_dup(Item *dst, const Item *src) {
  *dst = *src;
  dst->path = src->path ? xstrdup(src->path) : NULL;
}

static void item_clear(Item *it) {
  free(it->path);
  memset(it, 0, sizeof(*it));
}

static void emit(AudioEventType type, uint64_t token, const char *text) {
  pthread_mutex_lock(&A.mu);
  if (A.ev_tail - A.ev_head < EVENT_RING) {
    AudioEvent *e = &A.ev[A.ev_tail % EVENT_RING];
    e->type = type;
    e->token = token;
    snprintf(e->text, sizeof(e->text), "%s", text ? text : "");
    A.ev_tail++;
  } else {
    LOG("event ring overflow");
  }
  pthread_mutex_unlock(&A.mu);
  uint64_t one = 1;
  if (write(A.efd, &one, sizeof(one)) < 0) {
    LOG("eventfd write: %s", strerror(errno));
  }
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

static void decoder_close(Decoder *d) {
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
  item_clear(&d->item);
  free(d);
}

static Decoder *decoder_open(const Item *it, uint32_t start_ms) {
  Decoder *d = xcalloc(1, sizeof(*d));
  item_dup(&d->item, it);
  d->next_sample = -1;
  d->swr_in_fmt = -1;

  AVDictionary *opts = NULL;
  if (it->stream) {
    av_dict_set(&opts, "icy", "1", 0);
    av_dict_set(&opts, "reconnect", "1", 0);
    av_dict_set(&opts, "rw_timeout", "15000000", 0);
  }
  int rc = avformat_open_input(&d->fmt, it->path, NULL, &opts);
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
  d->out.fmt = pick_format(d->dec);
  if (d->out.rate <= 0 || d->out.channels <= 0 || d->out.channels > 8) {
    goto fail;
  }
  if (av_channel_layout_copy(&d->out_layout, &d->dec->ch_layout) < 0 || d->out_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
    av_channel_layout_uninit(&d->out_layout);
    av_channel_layout_default(&d->out_layout, d->out.channels);
  }
  d->bpf = d->out.channels * av_get_bytes_per_sample(d->out.fmt);
  d->pkt = av_packet_alloc();
  d->frame = av_frame_alloc();
  if (!d->pkt || !d->frame) {
    goto fail;
  }

  int64_t target = (int64_t)it->begin_ms + start_ms;
  if (target > 0 && !it->stream) {
    int64_t ts = av_rescale_q(target, (AVRational){ 1, 1000 }, st->time_base) + d->start_pts;
    if (av_seek_frame(d->fmt, d->sidx, ts, AVSEEK_FLAG_BACKWARD) >= 0) {
      avcodec_flush_buffers(d->dec);
    }
  }
  d->skip_until = it->stream ? 0 : target * d->out.rate / 1000;
  d->end_sample = it->end_ms ? (int64_t)it->end_ms * d->out.rate / 1000 : 0;
  describe(d);
  return d;

fail:
  LOG("cannot open %s", it->path);
  decoder_close(d);
  return NULL;
}

static bool decoder_swr_setup(Decoder *d, const AVFrame *f) {
  if (d->swr && f->format == d->swr_in_fmt && f->sample_rate == d->swr_in_rate && av_channel_layout_compare(&f->ch_layout, &d->swr_in_layout) == 0) {
    return true;
  }
  swr_free(&d->swr);
  if (swr_alloc_set_opts2(&d->swr, &d->out_layout, d->out.fmt, d->out.rate, &f->ch_layout, f->format, f->sample_rate, 0, NULL) < 0 || swr_init(d->swr) < 0) {
    swr_free(&d->swr);
    return false;
  }
  d->swr_in_fmt = f->format;
  d->swr_in_rate = f->sample_rate;
  av_channel_layout_uninit(&d->swr_in_layout);
  av_channel_layout_copy(&d->swr_in_layout, &f->ch_layout);
  return true;
}

static int decoder_convert(Decoder *d, uint8_t **out) {
  AVFrame *f = d->frame;
  if (!decoder_swr_setup(d, f)) {
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

  int64_t pos;
  if (f->best_effort_timestamp != AV_NOPTS_VALUE) {
    pos = av_rescale_q(f->best_effort_timestamp - d->start_pts, d->tb, (AVRational){ 1, d->out.rate });
  } else {
    pos = d->next_sample >= 0 ? d->next_sample : d->skip_until;
  }
  d->next_sample = pos + n;

  int64_t off = 0;
  if (pos < d->skip_until) {
    off = d->skip_until - pos;
    if (off >= n) {
      return 0;
    }
  }
  int64_t count = n - off;
  if (d->end_sample) {
    int64_t keep = d->end_sample - (pos + off);
    if (keep <= 0) {
      d->eof = true;
      return 0;
    }
    if (keep <= count) {
      count = keep;
      d->eof = true;
    }
  }
  *out = d->buf + off * d->bpf;
  return (int)count;
}

static void decoder_check_icy(Decoder *d) {
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

static int decoder_pull(Decoder *d, uint8_t **out) {
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
        return ret;
      }
      if (d->pkt->stream_index == d->sidx) {
        avcodec_send_packet(d->dec, d->pkt);
      }
      av_packet_unref(d->pkt);
      if (d->item.stream && (++d->icy_tick % 32) == 0) {
        decoder_check_icy(d);
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
      return ret;
    }
    int frames = decoder_convert(d, out);
    av_frame_unref(d->frame);
    if (frames != 0) {
      return frames;
    }
  }
}

static bool same_fmt(PcmFmt a, PcmFmt b) {
  return a.rate == b.rate && a.channels == b.channels && a.fmt == b.fmt;
}

static void pcm_close(void) {
  if (T.pcm) {
    snd_pcm_drop(T.pcm);
    snd_pcm_close(T.pcm);
    T.pcm = NULL;
  }
  T.hw_paused = false;
  T.written = 0;
}

static bool pcm_open(PcmFmt f) {
  pcm_close();
  int rc = snd_pcm_open(&T.pcm, A.device, SND_PCM_STREAM_PLAYBACK, 0);
  if (rc < 0) {
    LOG("snd_pcm_open %s: %s", A.device, snd_strerror(rc));
    T.pcm = NULL;
    return false;
  }
  snd_pcm_hw_params_t *hw;
  snd_pcm_hw_params_alloca(&hw);
  unsigned rate = (unsigned)f.rate;
  unsigned buffer_time = BUFFER_US;
  unsigned period_time = PERIOD_US;
  snd_pcm_format_t sf = f.fmt == AV_SAMPLE_FMT_S32 ? SND_PCM_FORMAT_S32 : SND_PCM_FORMAT_S16;
  if ((rc = snd_pcm_hw_params_any(T.pcm, hw)) < 0 ||
      (rc = snd_pcm_hw_params_set_access(T.pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
      (rc = snd_pcm_hw_params_set_format(T.pcm, hw, sf)) < 0 ||
      (rc = snd_pcm_hw_params_set_channels(T.pcm, hw, (unsigned)f.channels)) < 0 ||
      (rc = snd_pcm_hw_params_set_rate_near(T.pcm, hw, &rate, NULL)) < 0 ||
      (rc = snd_pcm_hw_params_set_buffer_time_near(T.pcm, hw, &buffer_time, NULL)) < 0 ||
      (rc = snd_pcm_hw_params_set_period_time_near(T.pcm, hw, &period_time, NULL)) < 0 ||
      (rc = snd_pcm_hw_params(T.pcm, hw)) < 0) {
    LOG("cannot set %s %dHz %dch: %s", snd_pcm_format_name(sf), f.rate, f.channels, snd_strerror(rc));
    pcm_close();
    return false;
  }
  if (rate != (unsigned)f.rate) {
    LOG("device does not support %dHz (got %u), use a plug device", f.rate, rate);
    pcm_close();
    return false;
  }
  snd_pcm_uframes_t period = 0;
  snd_pcm_hw_params_get_period_size(hw, &period, NULL);
  T.period = period ? period : 1024;
  snd_pcm_sw_params_t *sw;
  snd_pcm_sw_params_alloca(&sw);
  if (snd_pcm_sw_params_current(T.pcm, sw) == 0) {
    snd_pcm_sw_params_set_start_threshold(T.pcm, sw, T.period * 2);
    snd_pcm_sw_params(T.pcm, sw);
  }
  T.can_pause = snd_pcm_hw_params_can_pause(hw);
  T.fmt = f;
  T.bpf = f.channels * av_get_bytes_per_sample(f.fmt);
  T.written = 0;
  return snd_pcm_prepare(T.pcm) == 0;
}

static bool pcm_ready(PcmFmt f) {
  if (T.pcm && same_fmt(T.fmt, f)) {
    T.hw_paused = false;
    T.written = 0;
    return snd_pcm_prepare(T.pcm) == 0;
  }
  return pcm_open(f);
}

static void update_position(void);

static bool pcm_write(const uint8_t *data, int frames) {
  while (frames > 0) {
    snd_pcm_uframes_t chunk = (snd_pcm_uframes_t)frames < T.period ? (snd_pcm_uframes_t)frames : T.period;
    snd_pcm_sframes_t n = snd_pcm_writei(T.pcm, data, chunk);
    if (n == -EAGAIN) {
      continue;
    }
    if (n < 0) {
      int rc = snd_pcm_recover(T.pcm, (int)n, 1);
      if (rc < 0) {
        LOG("snd_pcm_writei: %s", snd_strerror(rc));
        return false;
      }
      continue;
    }
    data += n * T.bpf;
    frames -= (int)n;
    T.written += (uint64_t)n;
    update_position();
  }
  return true;
}

static void apply_volume(uint8_t *data, int frames, PcmFmt f) {
  int v = atomic_load(&A.volume);
  if (v >= 100) {
    return;
  }
  int64_t gain = (int64_t)v * v * 65536 / 10000;
  size_t samples = (size_t)frames * (size_t)f.channels;
  if (f.fmt == AV_SAMPLE_FMT_S32) {
    int32_t *s = (int32_t *)data;
    for (size_t i = 0; i < samples; i++) {
      s[i] = (int32_t)(((int64_t)s[i] * gain) >> 16);
    }
  } else {
    int16_t *s = (int16_t *)data;
    for (size_t i = 0; i < samples; i++) {
      s[i] = (int16_t)(((int32_t)s[i] * gain) >> 16);
    }
  }
}

static void boundaries_clear(void) {
  for (int i = 0; i < T.nb; i++) {
    item_clear(&T.b[i].item);
  }
  T.nb = 0;
}

static void boundary_push(const Item *it, uint64_t frame_at, uint32_t start_ms, const char *desc) {
  if (T.nb == MAX_BOUNDARIES) {
    item_clear(&T.b[0].item);
    memmove(T.b, T.b + 1, sizeof(Boundary) * (MAX_BOUNDARIES - 1));
    T.nb--;
  }
  Boundary *b = &T.b[T.nb++];
  item_dup(&b->item, it);
  b->frame_at = frame_at;
  b->start_ms = start_ms;
  snprintf(b->desc, sizeof(b->desc), "%s", desc);
}

static void update_position(void) {
  if (!T.pcm) {
    return;
  }
  snd_pcm_sframes_t delay = 0;
  if (snd_pcm_delay(T.pcm, &delay) < 0 || delay < 0) {
    delay = 0;
  }
  int64_t played = (int64_t)T.written - delay;
  if (played < 0) {
    played = 0;
  }
  while (T.nb && played >= (int64_t)T.b[0].frame_at) {
    item_clear(&T.audible.item);
    T.audible = T.b[0];
    T.has_audible = true;
    memmove(T.b, T.b + 1, sizeof(Boundary) * (size_t)(T.nb - 1));
    T.nb--;
    atomic_store(&A.pos_ms, T.audible.start_ms);
    emit(AEV_STARTED, T.audible.item.token, T.audible.desc);
  }
  if (T.has_audible) {
    int64_t ms = T.audible.start_ms + (played - (int64_t)T.audible.frame_at) * 1000 / T.fmt.rate;
    atomic_store(&A.pos_ms, (unsigned)(ms < 0 ? 0 : ms));
  }
}

static void reset_playback(void) {
  decoder_close(T.cur);
  T.cur = NULL;
  boundaries_clear();
  T.waiting = false;
  T.paused = false;
  T.reopen_on_resume = false;
}

static void do_stop(void) {
  reset_playback();
  pcm_close();
  item_clear(&T.audible.item);
  T.has_audible = false;
  atomic_store(&A.pos_ms, 0);
}

static void wait_for_next(uint64_t token) {
  T.waiting = true;
  T.waiting_for = token;
}

static void do_play(const Item *it, uint32_t start_ms, bool paused) {
  reset_playback();
  if (T.pcm) {
    snd_pcm_drop(T.pcm);
    T.hw_paused = false;
  }
  Decoder *d = decoder_open(it, start_ms);
  if (!d || !pcm_ready(d->out)) {
    decoder_close(d);
    pcm_close();
    emit(AEV_ERROR, it->token, "cannot play");
    wait_for_next(it->token);
    return;
  }
  boundary_push(it, 0, start_ms, d->desc);
  T.cur = d;
  T.paused = paused;
  atomic_store(&A.pos_ms, start_ms);
}

static void finish_output(void) {
  if (T.pcm && !T.paused) {
    snd_pcm_drain(T.pcm);
  }
  update_position();
}

static void do_next(const Item *it, uint64_t for_token) {
  if (!T.waiting || for_token != T.waiting_for) {
    return;
  }
  T.waiting = false;
  if (!it) {
    uint64_t last = T.has_audible ? T.audible.item.token : for_token;
    finish_output();
    do_stop();
    emit(AEV_STOPPED, last, NULL);
    return;
  }
  Decoder *d = decoder_open(it, 0);
  if (!d) {
    emit(AEV_ERROR, it->token, "cannot play");
    wait_for_next(it->token);
    return;
  }
  if (!T.pcm || !same_fmt(T.fmt, d->out)) {
    finish_output();
    if (!pcm_open(d->out)) {
      decoder_close(d);
      emit(AEV_ERROR, it->token, "cannot open output");
      wait_for_next(it->token);
      return;
    }
  }
  boundary_push(it, T.written, 0, d->desc);
  T.cur = d;
}

static void do_pause(bool pause) {
  if (pause == T.paused || (!T.cur && !T.waiting)) {
    return;
  }
  if (pause) {
    update_position();
    if (T.pcm && T.can_pause && snd_pcm_state(T.pcm) == SND_PCM_STATE_RUNNING && snd_pcm_pause(T.pcm, 1) == 0) {
      T.hw_paused = true;
    } else if (T.pcm && snd_pcm_state(T.pcm) == SND_PCM_STATE_RUNNING) {
      T.reopen_on_resume = T.has_audible;
      T.reopen_pos = atomic_load(&A.pos_ms);
      snd_pcm_drop(T.pcm);
    }
    T.paused = true;
    return;
  }
  T.paused = false;
  if (T.hw_paused) {
    snd_pcm_pause(T.pcm, 0);
    T.hw_paused = false;
  } else if (T.reopen_on_resume) {
    Item it;
    item_dup(&it, &T.audible.item);
    do_play(&it, T.reopen_pos, false);
    item_clear(&it);
  }
}

static void do_set_device(char *device) {
  free(A.device);
  A.device = device;
  if (!T.pcm) {
    return;
  }
  if (T.reopen_on_resume || !T.has_audible) {
    pcm_close();
    return;
  }
  update_position();
  pcm_close();
  Item it;
  item_dup(&it, &T.audible.item);
  do_play(&it, atomic_load(&A.pos_ms), T.paused);
  item_clear(&it);
}

static void step(void) {
  uint8_t *data = NULL;
  int frames = decoder_pull(T.cur, &data);
  if (T.cur->icy_changed) {
    T.cur->icy_changed = false;
    emit(AEV_ICY, T.cur->item.token, T.cur->icy);
  }
  if (frames > 0) {
    apply_volume(data, frames, T.cur->out);
    if (pcm_write(data, frames)) {
      return;
    }
    pcm_close();
    frames = -1;
  }
  if (frames < 0) {
    LOG("decode error in %s", T.cur->item.path);
  }
  uint64_t token = T.cur->item.token;
  decoder_close(T.cur);
  T.cur = NULL;
  wait_for_next(token);
  emit(AEV_NEED_NEXT, token, NULL);
}

static bool has_command(void) {
  return A.c_quit || A.c_device || A.c_stop || A.c_play || A.c_next || A.c_pause >= 0;
}

static void *audio_main(void *arg) {
  (void)arg;
  for (;;) {
    pthread_mutex_lock(&A.mu);
    while (!has_command() && !(T.cur && !T.paused && !T.waiting)) {
      if (T.pcm && !T.paused && (T.waiting || T.nb)) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 20 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
          ts.tv_sec++;
          ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&A.cv, &A.mu, &ts);
        break;
      }
      pthread_cond_wait(&A.cv, &A.mu);
    }
    bool quit = A.c_quit;
    bool stop = A.c_stop;
    bool play = A.c_play;
    bool play_paused = A.c_play_paused;
    uint32_t play_start = A.c_play_start;
    Item play_item = A.c_play_item;
    bool next = A.c_next;
    bool next_has = A.c_next_has;
    uint64_t next_for = A.c_next_for;
    Item next_item = A.c_next_item;
    int pause = A.c_pause;
    char *device = A.c_device;
    A.c_device = NULL;
    memset(&A.c_play_item, 0, sizeof(Item));
    memset(&A.c_next_item, 0, sizeof(Item));
    A.c_stop = A.c_play = A.c_next = false;
    A.c_pause = -1;
    pthread_mutex_unlock(&A.mu);

    if (quit) {
      item_clear(&play_item);
      item_clear(&next_item);
      free(device);
      break;
    }
    if (device) {
      do_set_device(device);
    }
    if (stop) {
      do_stop();
    }
    if (play) {
      do_play(&play_item, play_start, play_paused);
    } else if (next) {
      do_next(next_has ? &next_item : NULL, next_for);
    }
    if (pause >= 0) {
      do_pause(pause == 1);
    }
    item_clear(&play_item);
    item_clear(&next_item);

    if (T.cur && !T.paused && !T.waiting) {
      step();
    }
    update_position();
  }
  do_stop();
  return NULL;
}

bool audio_init(const char *device, int *event_fd) {
  A.device = xstrdup(device);
  atomic_store(&A.volume, 100);
  A.efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (A.efd < 0) {
    return false;
  }
  if (pthread_create(&A.thread, NULL, audio_main, NULL) != 0) {
    close(A.efd);
    return false;
  }
  A.running = true;
  *event_fd = A.efd;
  return true;
}

void audio_shutdown(void) {
  if (!A.running) {
    return;
  }
  pthread_mutex_lock(&A.mu);
  A.c_quit = true;
  pthread_cond_signal(&A.cv);
  pthread_mutex_unlock(&A.mu);
  pthread_join(A.thread, NULL);
  A.running = false;
  item_clear(&A.c_play_item);
  item_clear(&A.c_next_item);
  item_clear(&T.audible.item);
  close(A.efd);
  A.efd = -1;
  free(A.device);
  A.device = NULL;
  free(A.c_device);
  A.c_device = NULL;
}

void audio_set_device(const char *device) {
  pthread_mutex_lock(&A.mu);
  free(A.c_device);
  A.c_device = xstrdup(device);
  pthread_cond_signal(&A.cv);
  pthread_mutex_unlock(&A.mu);
}

void audio_play(const AudioItem *item, uint32_t start_ms, bool paused) {
  pthread_mutex_lock(&A.mu);
  item_set(&A.c_play_item, item);
  A.c_play = true;
  A.c_play_start = start_ms;
  A.c_play_paused = paused;
  A.c_stop = false;
  A.c_next = false;
  A.c_pause = -1;
  item_clear(&A.c_next_item);
  pthread_cond_signal(&A.cv);
  pthread_mutex_unlock(&A.mu);
}

void audio_next(const AudioItem *item, uint64_t for_token) {
  pthread_mutex_lock(&A.mu);
  if (!A.c_play && !A.c_stop) {
    A.c_next = true;
    A.c_next_for = for_token;
    A.c_next_has = item != NULL;
    if (item) {
      item_set(&A.c_next_item, item);
    } else {
      item_clear(&A.c_next_item);
    }
    pthread_cond_signal(&A.cv);
  }
  pthread_mutex_unlock(&A.mu);
}

void audio_pause(bool pause) {
  pthread_mutex_lock(&A.mu);
  A.c_pause = pause ? 1 : 0;
  pthread_cond_signal(&A.cv);
  pthread_mutex_unlock(&A.mu);
}

void audio_stop(void) {
  pthread_mutex_lock(&A.mu);
  A.c_stop = true;
  A.c_play = false;
  A.c_next = false;
  A.c_pause = -1;
  item_clear(&A.c_play_item);
  item_clear(&A.c_next_item);
  pthread_cond_signal(&A.cv);
  pthread_mutex_unlock(&A.mu);
}

void audio_set_volume(int volume) {
  atomic_store(&A.volume, volume < 0 ? 0 : (volume > 100 ? 100 : volume));
}

bool audio_poll_event(AudioEvent *ev) {
  pthread_mutex_lock(&A.mu);
  bool got = A.ev_head != A.ev_tail;
  if (got) {
    *ev = A.ev[A.ev_head % EVENT_RING];
    A.ev_head++;
  }
  pthread_mutex_unlock(&A.mu);
  return got;
}

void audio_ack_events(void) {
  uint64_t v;
  if (read(A.efd, &v, sizeof(v)) < 0 && errno != EAGAIN) {
    LOG("eventfd read: %s", strerror(errno));
  }
}

uint32_t audio_position_ms(void) {
  return atomic_load(&A.pos_ms);
}
