#ifndef PROTO_HANDLERS_H
#define PROTO_HANDLERS_H

#include "model/track.h"

void scan_on_done(void *ud, short revents);
void r_track(const Track *t);

#endif
