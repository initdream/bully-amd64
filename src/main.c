#define _GNU_SOURCE
#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <ucontext.h>
#include <unistd.h>
#include <execinfo.h>

#include <SDL2/SDL.h>

#include "so_util.h"
#include "asset_archive.h"
#include "jni_shim.h"

#define CXX_SO  "libc++_shared.so"
#define GAME_SO "libGame.so"
#define CXX_HEAP_MB  48
#define GAME_HEAP_MB 128

int mod_game, mod_cxx;

extern DynLibFunction dynlib_functions[];
extern const int dynlib_functions_count;
extern void bully_imports_init(void);
extern unsigned long g_frame_no;

static void crash_handler(int sig, siginfo_t *info, void *uc) {
  uintptr_t fault = (uintptr_t)info->si_addr;
  uintptr_t tb = (uintptr_t)text_base;
  fprintf(stderr, "\n=== CRASH sig=%d addr=%p ===\n", sig, info->si_addr);

  Dl_info dlinfo;
  if (dladdr((void*)fault, &dlinfo) && dlinfo.dli_fname) {
    fprintf(stderr, "  fault address in: %s (offset 0x%lx)\n",
            dlinfo.dli_fname, (unsigned long)(fault - (uintptr_t)dlinfo.dli_fbase));
    if (dlinfo.dli_sname)
      fprintf(stderr, "  symbol: %s\n", dlinfo.dli_sname);
  }

  if (tb && fault >= tb && fault < tb + text_size)
    fprintf(stderr, "  libGame+0x%lx (text_base=%p)\n", (unsigned long)(fault - tb), text_base);

  #if defined(__x86_64__)
  ucontext_t *u = (ucontext_t *)uc;
  uintptr_t pc = u->uc_mcontext.gregs[REG_RIP];
  fprintf(stderr, "  RIP=%p", (void *)pc);

  if (tb && pc >= tb && pc < tb + text_size)
    fprintf(stderr, " = libGame+0x%lx", (unsigned long)(pc - tb));
  fprintf(stderr, "\n");
  #endif

  void *bt[32];
  int n = backtrace(bt, 32);
  fprintf(stderr, "  Backtrace (%d frames):\n", n);
  char **strings = backtrace_symbols(bt, n);
  if (strings) {
    for (int i = 0; i < n; i++) {
      fprintf(stderr, "    %s\n", strings[i]);
    }
    free(strings);
  }

  fflush(stderr);
  _exit(128 + sig);
}

static void install_crash_handler(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = crash_handler;
  sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
  sigaction(SIGILL, &sa, NULL);
  sigaction(SIGABRT, &sa, NULL);
}

static void preload_device_libs(void) {
  static const char *libs[] = {
    "libSDL2-2.0.so.0", "libGLESv2.so", "libEGL.so", "libopenal.so.1",
    "libmpg123.so.0", "libm.so.6", "libc.so.6", "libpthread.so.0", NULL
  };
  for (int i = 0; libs[i]; i++) {
    void *h = dlopen(libs[i], RTLD_NOW | RTLD_GLOBAL);
    fprintf(stderr, "preload: %s %s\n", libs[i], h ? "OK" : dlerror());
  }
}

static void load_module(const char *name, int heap_mb, DynLibFunction *tbl, int n) {
  size_t hs = (size_t)heap_mb * 1024 * 1024;
  void *heap = mmap(NULL, hs, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (heap == MAP_FAILED) { exit(1); }
  fprintf(stderr, "== loading %s (heap %p, %d MB) ==\n", name, heap, heap_mb);
  if (so_load(name, heap, hs) < 0) { exit(1); }
  if (so_relocate() < 0) { exit(1); }
  so_resolve(tbl, n, 0);
  so_finalize();
  so_flush_caches();
  so_execute_init_array();
}

static void check_data(void) {
  struct stat st;
  if (stat(GAME_SO, &st) < 0) {
    fprintf(stderr, "FATAL: Missing %s\n", GAME_SO);
    exit(1);
  }
  if (stat(CXX_SO, &st) < 0) {
    fprintf(stderr, "FATAL: Missing %s\n", CXX_SO);
    exit(1);
  }

  const char *need[] = {
    "assets/data_0.zip", "assets/data_0.zip.idx",
    "assets/data_1.zip", "assets/data_1.zip.idx",
    "assets/data_2.zip", "assets/data_2.zip.idx",
    "assets/data_3.zip", "assets/data_3.zip.idx",
    "assets/data_4.zip", "assets/data_4.zip.idx",
    NULL,
  };
  int missing = 0;
  for (int i = 0; need[i]; i++) {
    if (stat(need[i], &st) < 0) {
      fprintf(stderr, "ERROR: Required data file '%s' not found.\n", need[i]);
      missing = 1;
    }
  }
  if (missing) {
    fprintf(stderr, "FATAL: Missing game data files in the 'assets/' directory.\n");
    exit(1);
  }
}

static void run_game_loop(void) {
  void *env = NVThreadGetCurrentJNIEnv();
  Uint64 last_time = SDL_GetPerformanceCounter();
  Uint64 perf_freq = SDL_GetPerformanceFrequency();

  for (unsigned long f = 0; ; f++) {
    g_frame_no = f;

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) return;
      if (e.type == SDL_CONTROLLERDEVICEADDED) jni_gamepad_connect(e.cdevice.which);
      else if (e.type == SDL_CONTROLLERDEVICEREMOVED) jni_gamepad_disconnect(e.cdevice.which);
    }

    jni_pump_gamepad();
    jni_mark_can_render();
    jni_update_rockstar();

    Uint64 current_time = SDL_GetPerformanceCounter();
    float dt = (float)(current_time - last_time) / (float)perf_freq;
    last_time = current_time;

    if (dt <= 0.0f) dt = 1.0f / 30.0f;

    if (!jni_draw_frame(env, dt)) return;
  }
}

int main(int argc, char *argv[]) {
  (void)argc; (void)argv;
  install_crash_handler();
  fprintf(stderr, "=== Bully (Android) so-loader / x86_64 ===\n");

  bully_imports_init();
  preload_device_libs();
  check_data();

  load_module(CXX_SO, CXX_HEAP_MB, dynlib_functions, dynlib_functions_count);
  int cxx_n = 0;
  DynLibFunction *cxx_tbl = so_snapshot_symbols(&cxx_n);

  int comb_n = dynlib_functions_count + cxx_n;
  DynLibFunction *comb = malloc(sizeof(DynLibFunction) * comb_n);
  if (!comb) {
    fprintf(stderr, "out of memory building combined import table\n");
    exit(1);
  }
  memcpy(comb, dynlib_functions, sizeof(DynLibFunction) * dynlib_functions_count);
  memcpy(comb + dynlib_functions_count, cxx_tbl, sizeof(DynLibFunction) * cxx_n);

  load_module(GAME_SO, GAME_HEAP_MB, comb, comb_n);
  free(comb);

  extern void debug_hooks_install(void);
  if (getenv("BULLY_DEBUG_THROWS"))
    debug_hooks_install();

  asset_archive_init();

  fprintf(stderr, "=== running jni_load (driver) ===\n");
  jni_load();

  run_game_loop();

  return 0;
}
