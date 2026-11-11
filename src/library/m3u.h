#ifndef LIBRARY_M3U_H
#define LIBRARY_M3U_H

#include "util/util.h"

bool m3u_parse(const char *path, StrVec *entries);

#endif
