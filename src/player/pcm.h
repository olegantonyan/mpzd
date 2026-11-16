#ifndef PLAYER_PCM_H
#define PLAYER_PCM_H

#include <stdbool.h>

typedef enum { SF_S16, SF_S32 } SampleFmt;

typedef struct {
  int rate;
  int channels;
  SampleFmt fmt;
} PcmFmt;

static inline int pcm_bpf(PcmFmt f) {
  return f.channels * (f.fmt == SF_S32 ? 4 : 2);
}

static inline bool pcm_same(PcmFmt a, PcmFmt b) {
  return a.rate == b.rate && a.channels == b.channels && a.fmt == b.fmt;
}

#endif
