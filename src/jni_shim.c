#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "jni_shim.h"
#include "so_util_x64.h"
#include "util.h"

extern Module mod_game;
extern void bully_swap_buffers(void);
extern int bully_screen_w(void);
extern int bully_screen_h(void);
extern int bully_init_gl(void);
extern int bully_make_current(void);
extern void bully_release_current(void);
extern void bully_egl_objects(uintptr_t *d, uintptr_t *s, uintptr_t *c);
extern void shadows_init(void);
extern void shadows_apply(void);

#define DATA_PATH "."

enum {
  UNKNOWN = 0, INIT_EGL_AND_GLES2, SWAP_BUFFERS, MAKE_CURRENT, UN_MAKE_CURRENT, SHARE_TEXT, SHARE_IMAGE,
  HAS_APP_LOCAL_VALUE, GET_APP_LOCAL_VALUE, SET_APP_LOCAL_VALUE, GET_PARAMETER, FILE_GET_ARCHIVE_NAME,
  DELETE_FILE, GET_DEVICE_INFO, GET_DEVICE_TYPE, GET_DEVICE_LOCALE,
  ROCKSTAR_SHOW_INITIAL, ROCKSTAR_SHOW_GATE,
};

static struct { const char *name; int id; } method_ids[] = {
  {"rockstarShowInitial", ROCKSTAR_SHOW_INITIAL}, {"rockstarShowGate", ROCKSTAR_SHOW_GATE},
  {"InitEGLAndGLES2", INIT_EGL_AND_GLES2}, {"swapBuffers", SWAP_BUFFERS},
  {"makeCurrent", MAKE_CURRENT}, {"unMakeCurrent", UN_MAKE_CURRENT},
  {"ShareText", SHARE_TEXT}, {"ShareImage", SHARE_IMAGE},
  {"hasAppLocalValue", HAS_APP_LOCAL_VALUE}, {"getAppLocalValue", GET_APP_LOCAL_VALUE},
  {"setAppLocalValue", SET_APP_LOCAL_VALUE}, {"getParameter", GET_PARAMETER},
  {"FileGetArchiveName", FILE_GET_ARCHIVE_NAME}, {"DeleteFile", DELETE_FILE},
  {"GetDeviceInfo", GET_DEVICE_INFO}, {"GetDeviceType", GET_DEVICE_TYPE},
  {"GetDeviceLocale", GET_DEVICE_LOCALE},
};

static char fake_vm[0x1000];
static char fake_env[0x1000];
static SDL_GameController *g_pad;

static int GetDeviceType(void) { return (2048 << 6) | (3 << 2) | 0x1; }
static int swapBuffers(void) { bully_swap_buffers(); return 1; }
static int InitEGLAndGLES2(void) { return bully_init_gl(); }
static char *getAppLocalValue(char *key) {
  if (key && strcmp(key, "STORAGE_ROOT") == 0) return (char *)DATA_PATH;
  return NULL;
}
static int hasAppLocalValue(char *key) { return (key && strcmp(key, "STORAGE_ROOT") == 0) ? 1 : 0; }
static void setAppLocalValue(char *k, char *v) { (void)k; (void)v; }
static char *getParameter(char *key) { (void)key; return NULL; }
static char *FileGetArchiveName(int type) {
  if (type == 1) return (char *)"main.obb";
  if (type == 2) return (char *)"patch.obb";
  return NULL;
}

static void check_exit_hotkey(void) {
  if (g_pad && SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_BACK) &&
    SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_START)) {
    _exit(0);
    }
}

static const struct { int sdl; int game; } g_btnmap[] = {
  {SDL_CONTROLLER_BUTTON_A, 0}, {SDL_CONTROLLER_BUTTON_B, 1}, {SDL_CONTROLLER_BUTTON_X, 2},
  {SDL_CONTROLLER_BUTTON_Y, 3}, {SDL_CONTROLLER_BUTTON_START, 4}, {SDL_CONTROLLER_BUTTON_BACK, 5},
  /* L1/R1 (shoulder) and L3/R3 (stick click) were reversed: the game's pad-type
   * action map treats id 6 as the left-shoulder action and id 16 as left-stick,
   * so map the physical buttons accordingly. */
  {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 6}, {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 7},
  {SDL_CONTROLLER_BUTTON_DPAD_UP, 8}, {SDL_CONTROLLER_BUTTON_DPAD_DOWN, 9},
  {SDL_CONTROLLER_BUTTON_DPAD_LEFT, 10}, {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 11},
  {SDL_CONTROLLER_BUTTON_LEFTSTICK, 16}, {SDL_CONTROLLER_BUTTON_RIGHTSTICK, 18},
};

static void (*g_pump_down)(void *, void *, int, int) = NULL;
static void (*g_pump_up)(void *, void *, int, int) = NULL;
static void (*g_pump_axes)(void *, void *, int, float, float, float, float, float, float) = NULL;
static void (*g_pump_count)(void *, void *, int) = NULL;
static int g_pump_inited = 0;
static int g_pump_last[20] = {0};
static float g_pump_la[6] = {0};

static void gamepad_reset(void) {
  g_pump_inited = 0;
  memset(g_pump_last, 0, sizeof(g_pump_last));
  memset(g_pump_la, 0, sizeof(g_pump_la));
}

void jni_gamepad_connect(int which) {
  if (!SDL_IsGameController(which) || g_pad) return;
  g_pad = SDL_GameControllerOpen(which);
  if (!g_pad) return;
  fprintf(stderr, "[pad] connected: %s\n",
          SDL_GameControllerName(g_pad) ? SDL_GameControllerName(g_pad) : "?");
  gamepad_reset();
}

void jni_gamepad_disconnect(int instance) {
  if (!g_pad) return;
  if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pad)) != (SDL_JoystickID)instance) return;
  SDL_GameControllerClose(g_pad);
  g_pad = NULL;
  gamepad_reset();
  fprintf(stderr, "[pad] disconnected\n");
}

void jni_pump_gamepad(void) {
  if (!g_pad) return;
  if (!g_pump_inited) {
    #define GP(n) (void *)so_symbol(&mod_game, "Java_com_rockstargames_oswrapper_GameNative_" n)
    g_pump_down = GP("implOnGamepadButtonDown");
    g_pump_up = GP("implOnGamepadButtonUp");
    g_pump_axes = GP("implOnGamepadAxesChanged");
    g_pump_count = GP("implOnGamepadCountChanged");
    #undef GP
    if (g_pump_count) g_pump_count(fake_env, NULL, 1);
    g_pump_inited = 1;
  }
  SDL_GameControllerUpdate();
  check_exit_hotkey();
  for (unsigned i = 0; i < sizeof(g_btnmap) / sizeof(g_btnmap[0]); i++) {
    int g = g_btnmap[i].game;
    int p = SDL_GameControllerGetButton(g_pad, g_btnmap[i].sdl) ? 1 : 0;
    if (p != g_pump_last[g]) {
      if (p) { if (g_pump_down) g_pump_down(fake_env, NULL, 0, g); }
      else { if (g_pump_up) g_pump_up(fake_env, NULL, 0, g); }
      g_pump_last[g] = p;
    }
  }
  int lt = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 12000 ? 1 : 0;
  int rt = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 12000 ? 1 : 0;
  if (lt != g_pump_last[17]) {
    if (lt) { if (g_pump_down) g_pump_down(fake_env, NULL, 0, 17); } else if (g_pump_up) g_pump_up(fake_env, NULL, 0, 17);
    g_pump_last[17] = lt;
  }
  if (rt != g_pump_last[19]) {
    if (rt) { if (g_pump_down) g_pump_down(fake_env, NULL, 0, 19); } else if (g_pump_up) g_pump_up(fake_env, NULL, 0, 19);
    g_pump_last[19] = rt;
  }
  float a[6];
  a[0] = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTX) / 32768.0f;
  a[1] = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTY) / 32768.0f;
  a[2] = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTX) / 32768.0f;
  a[3] = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTY) / 32768.0f;
  a[4] = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) / 32768.0f;
  a[5] = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 32768.0f;
  for (int i = 0; i < 4; i++) if (fabsf(a[i]) < 0.15f) a[i] = 0.0f;
  if (a[4] < 0.15f) a[4] = 0.0f;
  if (a[5] < 0.15f) a[5] = 0.0f;
  int ch = 0;
  for (int i = 0; i < 6; i++) if (fabsf(a[i] - g_pump_la[i]) > 0.02f) { ch = 1; break; }
  if (ch && g_pump_axes) {
    g_pump_axes(fake_env, NULL, 0, a[0], a[1], a[2], a[3], a[4], a[5]);
    for (int i = 0; i < 6; i++) g_pump_la[i] = a[i];
  }
}

/* ---- dispatchers JNI ---- */
static int GetMethodID(void *e, void *c, const char *name, const char *sig) {
  (void)e; (void)c; (void)sig;
  for (unsigned i = 0; i < sizeof(method_ids) / sizeof(method_ids[0]); i++) {
    if (strcmp(name, method_ids[i].name) == 0) return method_ids[i].id;
  }
  return 0x7777;
}

static int CallBooleanMethodV(void *e, void *o, int id, va_list a) {
  (void)e; (void)o;
  switch (id) {
    case INIT_EGL_AND_GLES2: return InitEGLAndGLES2();
    case SWAP_BUFFERS: return swapBuffers();
    case MAKE_CURRENT: return bully_make_current();
    case UN_MAKE_CURRENT: bully_release_current(); return 1;
    case HAS_APP_LOCAL_VALUE: return hasAppLocalValue(va_arg(a, char *));
    case DELETE_FILE: return 0;
  }
  return 0;
}

static float CallFloatMethodV(void *e, void *o, int id, va_list a) {
  (void)e; (void)o; (void)id; (void)a;
  return 0.0f;
}

static int CallIntMethodV(void *e, void *o, int id, va_list a) {
  (void)e; (void)o;
  switch (id) {
    case GET_DEVICE_TYPE: return GetDeviceType();
    case GET_DEVICE_INFO:
    case GET_DEVICE_LOCALE: return 0;
  }
  return 0;
}

static void *CallObjectMethodV(void *e, void *o, int id, va_list a) {
  (void)e; (void)o;
  switch (id) {
    case GET_APP_LOCAL_VALUE: { char *r = getAppLocalValue(va_arg(a, char *)); return r ? r : (void *)""; }
    case GET_PARAMETER: { char *r = getParameter(va_arg(a, char *)); return r ? r : (void *)""; }
    case FILE_GET_ARCHIVE_NAME: { char *r = FileGetArchiveName(va_arg(a, int)); return r ? r : (void *)""; }
  }
  return (void *)"";
}

volatile int g_rk_pending_initial = 0, g_rk_pending_gate = 0, g_rk_pending_gate_type = 0;
static void CallVoidMethodV(void *e, void *o, int id, va_list a) {
  (void)e; (void)o;
  if (id == SET_APP_LOCAL_VALUE) {
    char *k = va_arg(a, char *); char *v = va_arg(a, char *); setAppLocalValue(k, v);
  } else if (id == ROCKSTAR_SHOW_INITIAL) {
    g_rk_pending_initial = 1;
  } else if (id == ROCKSTAR_SHOW_GATE) {
    g_rk_pending_gate_type = va_arg(a, int); g_rk_pending_gate = 1;
  }
}

static void *FindClass(void *e, const char *n) { (void)e; (void)n; return (void *)0x41414141; }
static void *NewGlobalRef(void *e, void *o) { (void)e; return o ? o : (void *)0x42424242; }
static char *NewStringUTF(void *e, char *b) { (void)e; return b ? b : (char *)""; }
static char *GetStringUTFChars(void *e, char *s, int *c) { (void)e; if (c) *c = 0; return s ? s : (char *)""; }
static void RegisterNatives(void *e, void *cls, void *methods, int n) { (void)e; (void)cls; (void)methods; (void)n; }
void *NVThreadGetCurrentJNIEnv(void) { return fake_env; }

static void *CallObjectMethod(void *e, void *o, int id, ...) { va_list a; va_start(a, id); void *r = CallObjectMethodV(e, o, id, a); va_end(a); return r; }
static int CallBooleanMethod(void *e, void *o, int id, ...) { va_list a; va_start(a, id); int r = CallBooleanMethodV(e, o, id, a); va_end(a); return r; }
static int CallIntMethod(void *e, void *o, int id, ...) { va_list a; va_start(a, id); int r = CallIntMethodV(e, o, id, a); va_end(a); return r; }
static float CallFloatMethod(void *e, void *o, int id, ...) { va_list a; va_start(a, id); float r = CallFloatMethodV(e, o, id, a); va_end(a); return r; }
static void CallVoidMethod(void *e, void *o, int id, ...) { va_list a; va_start(a, id); CallVoidMethodV(e, o, id, a); va_end(a); }

static int GetEnv(void *vm, void **env, int v) { (void)vm; (void)v; *env = fake_env; return 0; }
static int AttachCurrentThread(void *vm, void **env, void *args) { (void)vm; (void)args; *env = fake_env; return 0; }

#define SET(off, fn) *(uintptr_t *)(fake_env + (off)) = (uintptr_t)(fn)
static void build_env(void) {
  for (unsigned i = 0; i < sizeof(fake_env) / sizeof(uintptr_t); i++)
    ((uintptr_t *)fake_env)[i] = (uintptr_t)ret0;
  *(uintptr_t *)(fake_env + 0x00) = (uintptr_t)fake_env;
  SET(0x30, FindClass);
  SET(0x88, ret0);
  SET(0xA8, NewGlobalRef);
  SET(0xB0, ret0);
  SET(0xB8, ret0);
  SET(0x108, GetMethodID);
  SET(0x110, CallObjectMethod);
  SET(0x118, CallObjectMethodV);
  SET(0x128, CallBooleanMethod);
  SET(0x130, CallBooleanMethodV);
  SET(0x188, CallIntMethod);
  SET(0x190, CallIntMethodV);
  SET(0x1B8, CallFloatMethod);
  SET(0x1C0, CallFloatMethodV);
  SET(0x1E8, CallVoidMethod);
  SET(0x1F0, CallVoidMethodV);
  SET(0x538, NewStringUTF);
  SET(0x548, GetStringUTFChars);
  SET(0x550, ret0);
  SET(0x6B8, RegisterNatives);
}

void jni_init_input(void) {
  int n = SDL_NumJoysticks();
  for (int i = 0; i < n; i++) {
    if (SDL_IsGameController(i) && !g_pad) {
      jni_gamepad_connect(i);
    }
  }
  if (!g_pad && n > 0) {
    SDL_GameControllerAddMapping(
      "03000000000000000000000000000000,USB Gamepad,"
      "a:b2,b:b1,x:b3,y:b0,start:b9,back:b8,"
      "leftshoulder:b4,rightshoulder:b5,"
      "dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,"
      "leftx:a0,lefty:a1,rightx:a2,righty:a3,platform:Linux,");
    jni_gamepad_connect(0);
  }
}

extern int asset_archive_init(void);
extern void *asset_open(const char *path);
extern void asset_close(void *h);
extern size_t asset_read(void *buf, size_t s, size_t n, void *h);
extern int asset_seek(void *h, long off, int wh);
extern long asset_tell(void *h);
extern long asset_size(void *h);
extern int asset_eof(void *h);
extern int asset_getc(void *h);
extern char *asset_gets(char *b, int m, void *h);

static int nv_init(void *a, void *b, void *c) { (void)a; (void)b; (void)c; asset_archive_init(); return 0; }
static void *nv_open(const char *p) {
  return asset_open(p);
}

static size_t nv_read(void *buf, size_t s, size_t n, void *h) { return h ? asset_read(buf, s, n, h) : 0; }
static int nv_seek(void *h, long o, int w) { return h ? asset_seek(h, o, w) : -1; }
static void nv_close(void *h) { asset_close(h); }
static long nv_tell(void *h) { return h ? asset_tell(h) : -1; }
static long nv_size(void *h) { return h ? asset_size(h) : 0; }
static int nv_eof(void *h) { return h ? asset_eof(h) : 1; }
static int nv_getc(void *h) { return h ? asset_getc(h) : -1; }
static char *nv_gets(char *b, int m, void *h) { return h ? asset_gets(b, m, h) : NULL; }

static void and_create_egl(void) { bully_make_current(); }
static void and_destroy_egl(void) {}
static void os_thread_makecurrent(void) { bully_make_current(); }
static void os_thread_unmakecurrent(void) { bully_release_current(); }

static void hook_egl(void) {
  hook_x64(so_symbol(&mod_game, "_Z20AND_CreateEglSurfacev"), (uintptr_t)and_create_egl);
  hook_x64(so_symbol(&mod_game, "_Z21AND_DestroyEglSurfacev"), (uintptr_t)and_destroy_egl);
  hook_x64(so_symbol(&mod_game, "_Z20OS_ThreadMakeCurrentv"), (uintptr_t)os_thread_makecurrent);
  hook_x64(so_symbol(&mod_game, "_Z22OS_ThreadUnmakeCurrentv"), (uintptr_t)os_thread_unmakecurrent);
}

static int os_screen_w(void) { return bully_screen_w(); }
static int os_screen_h(void) { return bully_screen_h(); }
static int os_can_render(void) { return 1; }
static int os_is_suspended(void) { return 0; }
static void hook_screen(void) {
  hook_x64(so_symbol(&mod_game, "_Z17OS_ScreenGetWidthv"), (uintptr_t)os_screen_w);
  hook_x64(so_symbol(&mod_game, "_Z18OS_ScreenGetHeightv"), (uintptr_t)os_screen_h);
  hook_x64(so_symbol(&mod_game, "_Z16OS_CanGameRenderv"), (uintptr_t)os_can_render);
  hook_x64(so_symbol(&mod_game, "_Z18OS_IsGameSuspendedv"), (uintptr_t)os_is_suspended);
}

static int my_cxa_guard_acquire(char *g) { return g && *g == 0; }
static void my_cxa_guard_release(char *g) { if (g) *g = 1; }
static void my_cxa_guard_abort(char *g) { (void)g; }
static void hook_cxa(void) {
  hook_x64(so_symbol(&mod_game, "__cxa_guard_acquire"), (uintptr_t)my_cxa_guard_acquire);
  hook_x64(so_symbol(&mod_game, "__cxa_guard_release"), (uintptr_t)my_cxa_guard_release);
  hook_x64(so_symbol(&mod_game, "__cxa_guard_abort"), (uintptr_t)my_cxa_guard_abort);
}

static void (*g_AND_FileUpdated)(double) = NULL;
static volatile uintptr_t *g_first_async = NULL;
static void *async_file_worker(void *a) {
  (void)a;
  for (;;) {
    if (g_AND_FileUpdated && g_first_async && __atomic_load_n(g_first_async, __ATOMIC_ACQUIRE))
      g_AND_FileUpdated(0.002);
    else
      usleep(2000);
  }
  return NULL;
}
static void start_async_file_worker(void) {
  g_AND_FileUpdated = (void (*)(double))so_symbol(&mod_game, "_Z14AND_FileUpdated");
  g_first_async = (volatile uintptr_t *)so_symbol(&mod_game, "_ZN11AndroidFile14firstAsyncFileE");
  if (g_AND_FileUpdated && g_first_async) {
    pthread_t t;
    if (pthread_create(&t, NULL, async_file_worker, NULL) == 0) {
      pthread_detach(t);
    }
  }
}

typedef struct {
  unsigned (*func)(void *); void *arg; char *handle;
} OsThreadData;

static void *os_thread_entry(void *p) {
  OsThreadData *td = p;
  unsigned (*func)(void *) = td->func;
  void *arg = td->arg;
  char *h = td->handle;
  free(td);
  if (h) h[0x69] = 1;
  int ret = func ? (int)func(arg) : 0;
  if (h) h[0x69] = 0;
  return (void *)(intptr_t)ret;
}

static void *my_OS_ThreadLaunch(unsigned (*func)(void *), void *arg, unsigned r2, const char *name, int r4, int prio) {
  (void)name; (void)r2; (void)r4; (void)prio;
  char *h = calloc(1, 0x400);
  if (!h) return NULL;
  OsThreadData *td = malloc(sizeof(*td));
  td->func = func; td->arg = arg; td->handle = h;
  pthread_t t;
  if (pthread_create(&t, NULL, os_thread_entry, td) != 0) {
    free(td); free(h); return NULL;
  }
  h[0x69] = 1;
  memcpy(h + 0x28, &t, sizeof(t));
  return h;
}

static void my_OS_ThreadWait(void *thread) {
  if (!thread) return;
  pthread_t t;
  memcpy(&t, (char *)thread + 0x28, sizeof(t));
  pthread_join(t, NULL);
}

static int my_NVThreadSpawnJNIThread(long *out, const void *attr, const char *name, void *(*entry)(void *), void *arg) {
  (void)attr; (void)name;
  if (!entry) return -1;
  pthread_t t;
  int rc = pthread_create(&t, NULL, entry, arg);
  if (rc == 0 && out) memcpy(out, &t, sizeof(*out) < sizeof(t) ? sizeof(*out) : sizeof(t));
  return rc;
}

static void hook_threads(void) {
  hook_x64(so_symbol(&mod_game, "_Z15OS_ThreadLaunchPFjPvES_jPKci16OSThreadPriority"), (uintptr_t)my_OS_ThreadLaunch);
  hook_x64(so_symbol(&mod_game, "_Z13OS_ThreadWaitPv"), (uintptr_t)my_OS_ThreadWait);
  hook_x64(so_symbol(&mod_game, "_Z22NVThreadSpawnJNIThreadPlPK14pthread_attr_tPKcPFPvS5_ES5_"), (uintptr_t)my_NVThreadSpawnJNIThread);
}

static void hook_nvapk(void) {
  #define HK(sym, fn) hook_x64(so_symbol(&mod_game, sym), (uintptr_t)(fn))
  HK("_Z9NvAPKInitP8_jobjectP13_jobjectArrayS2_", nv_init);
  HK("_Z9NvAPKOpenPKc", nv_open);
  HK("_Z17NvAPKOpenFromPackPKc", nv_open);
  HK("_Z9NvAPKReadPvmmS_", nv_read);
  HK("_Z9NvAPKSeekPvli", nv_seek);
  HK("_Z10NvAPKClosePv", nv_close);
  HK("_Z9NvAPKTellPv", nv_tell);
  HK("_Z9NvAPKSizePv", nv_size);
  HK("_Z8NvAPKEOFPv", nv_eof);
  HK("_Z9NvAPKGetcPv", nv_getc);
  HK("_Z9NvAPKGetsPciPv", nv_gets);
  #undef HK
}

static void hook_x86_jmp(uintptr_t addr, uintptr_t dst) {
  size_t page_size = sysconf(_SC_PAGESIZE);
  uintptr_t page_start = addr & ~(page_size - 1);
  size_t len = ((addr + 14) - page_start + page_size - 1) & ~(page_size - 1);

  mprotect((void *)page_start, len, PROT_READ | PROT_WRITE | PROT_EXEC);

  uint8_t *hook = (uint8_t *)addr;
  int64_t diff = (int64_t)dst - (int64_t)(addr + 5);

  if (diff >= -2147483648LL && diff <= 2147483647LL) {
    hook[0] = 0xE9;
    *(int32_t *)(hook + 1) = (int32_t)diff;
    memset(hook + 5, 0x90, 9);
  } else {
    hook[0] = 0xFF;
    hook[1] = 0x25;
    hook[2] = 0x00;
    hook[3] = 0x00;
    hook[4] = 0x00;
    hook[5] = 0x00;
    *(uint64_t *)(hook + 6) = (uint64_t)dst;
  }

  mprotect((void *)page_start, len, PROT_READ | PROT_EXEC);
  __builtin___clear_cache((char *)hook, (char *)hook + 14);
}

/* Bypass DecodeTree caller at offset 0x427582 directly */
static void *my_DecodeTreeStub(void *thiz) {
  (void)thiz;
  static int called = 0;
  if (!called) {
    fprintf(stderr, "[drv] Stubbed DecodeTree (caller of decodeStringRef), returning 1 (success)\n");
    called = 1;
  }
  return (void *)1;
}

static void (*g_on_draw_frame)(void *, void *, float) = NULL;
static volatile uint8_t *g_can_render = NULL;
static volatile uint8_t *g_is_init = NULL;
static volatile uint8_t *g_suspended = NULL;
static void (*g_os_state_changed)(int) = NULL;
static void (*g_os_initial_complete)(void) = NULL;
static void (*g_os_gate_complete)(int, int) = NULL;
static void (*g_os_signin_complete)(void) = NULL;
static void (*g_os_app_event)(int, void *) = NULL;
static void (*g_rk_setup)(void *, void *, void *, void *) = NULL;

void jni_mark_can_render(void) {
  if (g_can_render) *g_can_render = 1;
}

void jni_update_rockstar(void) {
  static int rk_fired = 0, rk_signin = 0;
  static Uint32 start_ticks = 0;
  if (!start_ticks) start_ticks = SDL_GetTicks();
  Uint32 elapsed_ms = SDL_GetTicks() - start_ticks;

  if (!rk_fired && (g_rk_pending_initial || g_rk_pending_gate) && elapsed_ms > 450) {
    rk_fired = 1;
    int gt = g_rk_pending_gate ? g_rk_pending_gate_type : 0;
    if (g_os_state_changed) g_os_state_changed(0);
    if (g_os_initial_complete) g_os_initial_complete();
    if (g_os_gate_complete) g_os_gate_complete(gt, 1);
    if (g_os_app_event) g_os_app_event(9, NULL);
    if (g_rk_setup) g_rk_setup(fake_env, NULL, (void *)"pc_user", (void *)"pc_ticket");
    if (g_can_render) *g_can_render = 1;
    if (g_suspended) *g_suspended = 0;
    if (g_is_init) *g_is_init = 1;
    g_rk_pending_initial = g_rk_pending_gate = 0;
    rk_signin = 1;
  }

  if (rk_signin && elapsed_ms > 700) {
    rk_signin = 0;
    if (g_os_signin_complete) g_os_signin_complete();
  }
}

int jni_draw_frame(void *env, float dt) {
  if (!g_on_draw_frame) return 0;
  g_on_draw_frame(env, NULL, dt);
  return 1;
}

void jni_load(void) {
  build_env();
  for (unsigned i = 0; i < sizeof(fake_vm) / sizeof(uintptr_t); i++)
    ((uintptr_t *)fake_vm)[i] = (uintptr_t)ret0;
  *(uintptr_t *)(fake_vm + 0x00) = (uintptr_t)fake_vm;
  *(uintptr_t *)(fake_vm + 0x20) = (uintptr_t)AttachCurrentThread;
  *(uintptr_t *)(fake_vm + 0x30) = (uintptr_t)GetEnv;
  *(uintptr_t *)(fake_vm + 0x38) = (uintptr_t)AttachCurrentThread;

  so_make_text_writable();

  /* Hook DecodeTree caller directly to bypass decoding completely */
  uintptr_t decode_tree_addr = (uintptr_t)text_base + 0x427582;
  if (decode_tree_addr) {
    fprintf(stderr, "[drv] Hooking DecodeTree @ %p\n", (void *)decode_tree_addr);
    hook_x86_jmp(decode_tree_addr, (uintptr_t)my_DecodeTreeStub);
  }

  hook_nvapk();
  hook_egl();
  hook_threads();
  hook_screen();
  hook_cxa();

  shadows_init();

  so_make_text_executable();
  so_flush_caches();
  asset_archive_init();

  #define R(n) so_symbol(&mod_game, "Java_com_rockstargames_oswrapper_GameNative_" n)
  void (*OnInitialSetup)(void *, void *, void *, void *, void *, void *) = (void *)R("implOnInitialSetup");
  void (*OnActivityCreated)(void *, void *, void *, int) = (void *)R("implOnActivityCreated");
  void (*OnSurfaceCreated)(void *, void *) = (void *)R("implOnSurfaceCreated");
  void (*OnSurfaceChanged)(void *, void *, void *, int, int) = (void *)R("implOnSurfaceChanged");
  void (*OnDrawFrame)(void *, void *, float) = (void *)R("implOnDrawFrame");
  void (*OnResume)(void *, void *) = (void *)R("implOnResume");
  #undef R

  uintptr_t srp = so_symbol(&mod_game, "StorageRootPath");
  volatile uint8_t *isInit = srp ? (volatile uint8_t *)(srp - 0x174) : NULL;
  volatile uint8_t *suspended = srp ? (volatile uint8_t *)(srp - 0x17c) : NULL;
  volatile uint8_t *canRender = srp ? (volatile uint8_t *)(srp - 0x2e8) : NULL;
  if (suspended) *suspended = 0;

  int (*JNI_OnLoad)(void *, void *) = (void *)so_symbol(&mod_game, "JNI_OnLoad");
  if (JNI_OnLoad) JNI_OnLoad(fake_vm, NULL);

  if (OnInitialSetup) OnInitialSetup(fake_env, NULL, NULL, NULL, NULL, NULL);

  void (*OS_ZipAdd)(const char *) = (void *)so_symbol(&mod_game, "_Z9OS_ZipAddPKc");
  if (OS_ZipAdd) {
    OS_ZipAdd("data_0.zip");
    OS_ZipAdd("data_1.zip");
  }

  if (isInit && *isInit != 1) *isInit = 1;
  if (suspended) *suspended = 0;
  if (canRender) *canRender = 1;

  if (OnActivityCreated) OnActivityCreated(fake_env, NULL, (void *)0x42424242, 1);

  bully_init_gl();
  SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER);
  jni_init_input();

  uintptr_t egl_d = 0, egl_s = 0, egl_c = 0;
  bully_egl_objects(&egl_d, &egl_s, &egl_c);

  volatile uintptr_t *OS_EGLDisplay = srp ? (volatile uintptr_t *)(srp - 0x2d0) : NULL;
  volatile uintptr_t *OS_EGLSurface = srp ? (volatile uintptr_t *)(srp - 0x2c8) : NULL;
  volatile uintptr_t *OS_EGLContext = srp ? (volatile uintptr_t *)(srp - 0x2c0) : NULL;

  if (OS_EGLDisplay) *OS_EGLDisplay = egl_d;
  if (OS_EGLSurface) *OS_EGLSurface = egl_s;
  if (OS_EGLContext) *OS_EGLContext = egl_c;

  bully_release_current();

  if (OnSurfaceCreated) OnSurfaceCreated(fake_env, NULL);
  if (OnSurfaceChanged) OnSurfaceChanged(fake_env, NULL, NULL, bully_screen_w(), bully_screen_h());

  if (OS_EGLDisplay) *OS_EGLDisplay = egl_d;
  if (OS_EGLSurface) *OS_EGLSurface = egl_s;
  if (OS_EGLContext) *OS_EGLContext = egl_c;

  if (OnResume) OnResume(fake_env, NULL);
  start_async_file_worker();

  g_on_draw_frame = OnDrawFrame;
  g_can_render = canRender;
  g_is_init = isInit;
  g_suspended = suspended;
  g_os_state_changed = (void (*)(int))so_symbol(&mod_game, "_Z25OS_OnRockstarStateChangedb");
  g_os_initial_complete = (void (*)(void))so_symbol(&mod_game, "_Z28OS_OnRockstarInitialCompletev");
  g_os_gate_complete = (void (*)(int, int))so_symbol(&mod_game, "_Z25OS_OnRockstarGateCompleteib");
  g_os_signin_complete = (void (*)(void))so_symbol(&mod_game, "_Z27OS_OnRockstarSignInCompletev");
  g_os_app_event = (void (*)(int, void *))so_symbol(&mod_game, "_Z19OS_ApplicationEvent11OSEventTypePv");
  g_rk_setup = (void (*)(void *, void *, void *, void *))so_symbol(&mod_game, "Java_com_rockstargames_oswrapper_GameNative_implOnRockstarSetup");
}
