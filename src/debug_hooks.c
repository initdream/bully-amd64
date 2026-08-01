/* debug_hooks.c -- diagnostic-only exception instrumentation.
 *
 * Adds a GOT-level override of libGame's __cxa_throw import so that every C++
 * exception thrown by the game is logged (type name, what() string, and a
 * runtime backtrace mapped to libGame offsets) before being re-thrown normally.
 *
 * This is diagnostic scaffolding; it does not change game behaviour (the real
 * __cxa_throw is still called afterwards). Remove/hide behind an env var when
 * no longer needed.
 */
#define _GNU_SOURCE
#include <execinfo.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "so_util.h"

/* ---- __cxa_throw hook ---- */
typedef void (*cxa_throw_fn)(void *, void *, void (*)(void *));
static cxa_throw_fn g_real_throw = NULL;

static const char *ti_name(void *ti) {
  if (!ti) return "?";
  /* Itanium/libc++ std::type_info: vptr @0, const char* name @8 */
  uintptr_t *p = (uintptr_t *)ti;
  const char *n = (const char *)p[1];
  if ((uintptr_t)n < 0x1000 || (uintptr_t)n > 0x0000800000000000ULL)
    n = (const char *)&p[1];
  return n;
}

static void print_what(void *thrown) {
  if (!thrown) return;
  /* std::out_of_range : std::logic_error : std::exception
   * object[0] = vptr, object[8..32) = std::string (libc++: SSO heap ptr @0,
   * size @8, capacity @16 within the string member). */
  size_t sz = *(size_t *)((char *)thrown + 16);
  const char *data = (const char *)thrown + 8;
  if (sz >= 24)
    data = *(const char **)((char *)thrown + 8);
  char buf[128];
  if (sz > sizeof(buf) - 1) sz = sizeof(buf) - 1;
  memcpy(buf, data, sz);
  buf[sz] = 0;
  fprintf(stderr, "[dbg]   what(%zu): \"%s\"\n", sz, buf);
}

static uintptr_t g_text_base_hint = 0;
static size_t g_text_size_hint = 0;

static void dbg_cxa_throw(void *thrown, void *ti, void (*dtor)(void *)) {
  /* The inline unordered_map::at in decodeStringRef keeps the lookup key in
   * %ecx/%rcx, and the out_of_range throw-helper (a14a10) never touches rcx,
   * so %ecx still holds the missing key when we are entered. Capture it before
   * the compiler clobbers it. */
  unsigned volatile key_ecx = 0;
  __asm__ volatile("movl %%ecx, %0" : "=m"(key_ecx) : : "memory");
  fprintf(stderr, "\n[dbg] === __cxa_throw === type='%s' thrown=%p [ecx]key=0x%x (%u)\n",
          ti_name(ti), thrown, key_ecx, key_ecx);
  print_what(thrown);

  void *bt[40];
  int n = backtrace(bt, 40);
  char **ss = backtrace_symbols(bt, n);
  for (int i = 0; i < n; i++) {
    uintptr_t a = (uintptr_t)bt[i];
    if (g_text_base_hint && a >= g_text_base_hint &&
        a < g_text_base_hint + g_text_size_hint) {
      fprintf(stderr, "[dbg]   bt[%2d] libGame+0x%lx  (%s)\n", i,
              (unsigned long)(a - g_text_base_hint), ss ? ss[i] : "?");
    } else {
      fprintf(stderr, "[dbg]   bt[%2d] 0x%lx  (%s)\n", i,
              (unsigned long)a, ss ? ss[i] : "?");
    }
  }
  if (ss) free(ss);
  fflush(stderr);

  g_real_throw(thrown, ti, dtor);
}

void debug_hooks_install(void) {
  g_text_base_hint = (uintptr_t)text_base;
  g_text_size_hint = text_size;

  ImportOverride ovr[] = {
      {"__cxa_throw", (uintptr_t)dbg_cxa_throw, (uintptr_t *)&g_real_throw},
  };
  so_override_imports(ovr, sizeof(ovr) / sizeof(ovr[0]));

  fprintf(stderr, "[dbg] debug_hooks installed (__cxa_throw -> 0x%lx)\n",
          (unsigned long)(uintptr_t)dbg_cxa_throw);
}
