#ifndef PLAYER_DECODER_H
#define PLAYER_DECODER_H

#include "player/pcm.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct Decoder Decoder;

Decoder *decoder_open(const char *path, bool stream, uint32_t seek_ms, const atomic_bool *abort);
PcmFmt decoder_format(const Decoder *d);
const char *decoder_desc(const Decoder *d);
int decoder_read(Decoder *d, uint8_t **out, int64_t *pos);
bool decoder_icy(Decoder *d, char *buf, size_t n);
void decoder_close(Decoder *d);

#endif
