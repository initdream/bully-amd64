#ifndef __SO_UTIL_H__
#define __SO_UTIL_H__

#include <stdint.h>
#include <stddef.h>

#define ALIGN_MEM(x, align) (((x) + ((align) - 1)) & ~((align) - 1))

typedef struct {
  char *symbol;
  uintptr_t func;
} DynLibFunction;

typedef struct {
  void *load_base;
  void *phdr;
  int phnum;
  const char *name;
} LoadedModule;

/* Override an undefined import resolved for the *last loaded* module.
 * If real_out != NULL it receives the original resolved address. */
typedef struct {
  const char *name;
  uintptr_t func;
  uintptr_t *real_out;
} ImportOverride;

#define MAX_MODULES 8
extern LoadedModule g_modules[MAX_MODULES];
extern int g_num_modules;

extern void *text_base, *data_base;
extern size_t text_size, data_size;
extern void *tls_template;
extern size_t tls_init_size, tls_total_size, tls_align;

void hook_x64(uintptr_t addr, uintptr_t dst);
void so_make_text_writable(void);
void so_make_text_executable(void);
void so_patch_all_movaps(void);
void so_flush_caches(void);
void so_override_imports(const ImportOverride *ovr, int n);
int so_load(const char *filename, void *base, size_t max_size);
int so_relocate(void);
int so_resolve(DynLibFunction *funcs, int num_funcs, int taint_missing_imports);
void so_execute_init_array(void);
uintptr_t so_find_addr(const char *symbol);
uintptr_t so_find_addr_safe(const char *symbol);
char *so_find_nearest_symbol(uintptr_t offset, uintptr_t *out_sym_offset);
void so_finalize(void);

DynLibFunction *so_snapshot_symbols(int *out_count);

#endif
