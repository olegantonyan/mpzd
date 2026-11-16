#include "player/sink.h"
#include "util/util.h"

#include <alsa/asoundlib.h>
#include <errno.h>
#include <stdlib.h>

#define LOG_MODULE "alsa"
#define BUFFER_US 500000
#define PERIOD_US 50000

static struct {
  snd_pcm_t *pcm;
  char *device;
  snd_pcm_uframes_t period;
  int bpf;
  bool can_pause;
} S;

void sink_close(void) {
  if (S.pcm) {
    snd_pcm_drop(S.pcm);
    snd_pcm_close(S.pcm);
    S.pcm = NULL;
  }
  free(S.device);
  S.device = NULL;
}

bool sink_open(const char *device, PcmFmt f) {
  sink_close();
  S.device = xstrdup(device);
  int rc = snd_pcm_open(&S.pcm, device, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
  if (rc < 0) {
    LOG("snd_pcm_open %s: %s", device, snd_strerror(rc));
    S.pcm = NULL;
    sink_close();
    return false;
  }
  snd_pcm_hw_params_t *hw;
  snd_pcm_hw_params_alloca(&hw);
  unsigned rate = (unsigned)f.rate;
  unsigned buffer_time = BUFFER_US;
  unsigned period_time = PERIOD_US;
  snd_pcm_format_t sf = f.fmt == SF_S32 ? SND_PCM_FORMAT_S32 : SND_PCM_FORMAT_S16;
  if ((rc = snd_pcm_hw_params_any(S.pcm, hw)) < 0 ||
      (rc = snd_pcm_hw_params_set_access(S.pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
      (rc = snd_pcm_hw_params_set_format(S.pcm, hw, sf)) < 0 ||
      (rc = snd_pcm_hw_params_set_channels(S.pcm, hw, (unsigned)f.channels)) < 0 ||
      (rc = snd_pcm_hw_params_set_rate_near(S.pcm, hw, &rate, NULL)) < 0 ||
      (rc = snd_pcm_hw_params_set_buffer_time_near(S.pcm, hw, &buffer_time, NULL)) < 0 ||
      (rc = snd_pcm_hw_params_set_period_time_near(S.pcm, hw, &period_time, NULL)) < 0 ||
      (rc = snd_pcm_hw_params(S.pcm, hw)) < 0) {
    LOG("cannot set %s %dHz %dch: %s", snd_pcm_format_name(sf), f.rate, f.channels, snd_strerror(rc));
    sink_close();
    return false;
  }
  if (rate != (unsigned)f.rate) {
    LOG("device does not support %dHz (got %u), use a plug device", f.rate, rate);
    sink_close();
    return false;
  }
  snd_pcm_uframes_t period = 0;
  snd_pcm_hw_params_get_period_size(hw, &period, NULL);
  S.period = period ? period : 1024;
  snd_pcm_sw_params_t *sw;
  snd_pcm_sw_params_alloca(&sw);
  if (snd_pcm_sw_params_current(S.pcm, sw) == 0) {
    snd_pcm_sw_params_set_start_threshold(S.pcm, sw, S.period * 2);
    snd_pcm_sw_params(S.pcm, sw);
  }
  S.can_pause = snd_pcm_hw_params_can_pause(hw);
  S.bpf = pcm_bpf(f);
  return snd_pcm_prepare(S.pcm) == 0;
}

bool sink_prepare(void) {
  return S.pcm && snd_pcm_prepare(S.pcm) == 0;
}

void sink_drop(void) {
  if (S.pcm) {
    snd_pcm_drop(S.pcm);
  }
}

static bool recover(int err) {
  int rc = snd_pcm_recover(S.pcm, err, 1);
  if (rc < 0) {
    LOG("output %s: %s", S.device, snd_strerror(rc));
    return false;
  }
  return true;
}

int sink_write(const uint8_t *data, int frames) {
  snd_pcm_uframes_t chunk = (snd_pcm_uframes_t)frames < S.period ? (snd_pcm_uframes_t)frames : S.period;
  snd_pcm_sframes_t n = snd_pcm_writei(S.pcm, data, chunk);
  if (n == -EAGAIN) {
    return 0;
  }
  if (n < 0) {
    return recover((int)n) ? 0 : -1;
  }
  return (int)n;
}

bool sink_wait(int timeout_ms) {
  int w = snd_pcm_wait(S.pcm, timeout_ms);
  return w >= 0 || recover(w);
}

int64_t sink_delay(void) {
  snd_pcm_sframes_t delay = 0;
  if (!S.pcm || snd_pcm_delay(S.pcm, &delay) < 0 || delay < 0) {
    return 0;
  }
  return delay;
}

bool sink_running(void) {
  return S.pcm && snd_pcm_state(S.pcm) == SND_PCM_STATE_RUNNING;
}

bool sink_pause(bool on) {
  return S.pcm && S.can_pause && snd_pcm_pause(S.pcm, on ? 1 : 0) == 0;
}

bool sink_drain(void) {
  if (!S.pcm) {
    return false;
  }
  int rc = snd_pcm_drain(S.pcm);
  return rc == 0 || rc == -EAGAIN;
}

bool sink_draining(void) {
  return S.pcm && snd_pcm_state(S.pcm) == SND_PCM_STATE_DRAINING;
}
