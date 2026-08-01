#ifndef SO_UTIL_X64_COMPAT_H
#define SO_UTIL_X64_COMPAT_H
#include "so_util.h"

typedef int Module;
extern Module mod_game, mod_cxx;

#define so_symbol(m, name) so_find_addr(name)

// Map ARM64 hook name to our x86_64 hook function
#define hook_arm64(addr, dst) hook_x64((uintptr_t)(addr), (uintptr_t)(dst))

#endif
