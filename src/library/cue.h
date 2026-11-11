#ifndef LIBRARY_CUE_H
#define LIBRARY_CUE_H

#include "model/track.h"
#include "util/util.h"

typedef uint32_t (*CueDurationFn)(const char *audio_path);

bool cue_parse(const char *cue_path, CueDurationFn duration, TrackVec *out, StrVec *files);

#endif
