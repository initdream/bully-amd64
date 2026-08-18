#ifndef SO_UTIL_X64_COMPAT_H
#define SO_UTIL_X64_COMPAT_H
#include "so_util.h"

typedef int Module;
extern Module mod_game;

#define so_symbol(m, name) so_find_addr(name)

#endif
