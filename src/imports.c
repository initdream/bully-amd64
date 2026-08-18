#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <ctype.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <SDL2/SDL.h>

#include "so_util.h"
#include "jni_shim.h"

unsigned long g_frame_no = 0;

/* ---- bionic libc bridges ---- */
static int *bionic___errno(void) { extern int *__errno_location(void); return __errno_location(); }
static size_t b_strlen_chk(const char *s, size_t n) { (void)n; return strlen(s); }
static char *b_strrchr_chk(const char *s, int c, size_t n) { (void)n; return (char *)strrchr(s, c); }
static char *b_strchr_chk(const char *s, int c, size_t n) { (void)n; return (char *)strchr(s, c); }
static char *b_strncpy_chk2(char *d, const char *s, size_t n, size_t dn, size_t sn) { (void)dn; (void)sn; return strncpy(d, s, n); }

static void b_assert2(const char *f, int l, const char *fn, const char *e) {
  fprintf(stderr, "assert: %s:%d %s: %s\n", f, l, fn, e); abort();
}

static int b_android_log(int prio, const char *tag, const char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  fprintf(stderr, "[ALOG:%d %s] ", prio, tag ? tag : "?");
  vfprintf(stderr, fmt, ap); fprintf(stderr, "\n"); va_end(ap);
  return 0;
}

/* Bionic __sF – 3 FILE structures (152 bytes each in 64-bit Bionic) */
static char bionic_sF[3][152];

/* Safe range-based mapping from Bionic FILE* to glibc FILE* */
static FILE *map_sF(void *fp) {
  if (!fp) return NULL;
  uintptr_t base = (uintptr_t)bionic_sF;
  uintptr_t ptr = (uintptr_t)fp;
  if (ptr >= base && ptr < base + sizeof(bionic_sF)) {
    uintptr_t off = ptr - base;
    if (off < 152) return stdin;
    if (off < 304) return stdout;
    return stderr;
  }
  return (FILE *)fp;
}

/* Stdio stubs mapping bionic streams to glibc */
static int w_fprintf(void *fp, const char *fmt, ...) {
  FILE *f = map_sF(fp); if (!f) return 0;
  va_list ap; va_start(ap, fmt); int r = vfprintf(f, fmt, ap); va_end(ap); return r;
}
static int w_vfprintf(void *fp, const char *fmt, va_list ap) {
  FILE *f = map_sF(fp); return f ? vfprintf(f, fmt, ap) : 0;
}
static size_t w_fwrite(const void *p, size_t s, size_t n, void *fp) {
  FILE *f = map_sF(fp);
  if (!f) return 0;
  size_t res = fwrite(p, s, n, f);
  if (f == stderr || f == stdout) fflush(f);
  return res;
}
static int w_fputs(const char *str, void *fp) {
  FILE *f = map_sF(fp); return f ? fputs(str, f) : EOF;
}
static int w_fputc(int c, void *fp) {
  FILE *f = map_sF(fp); return f ? fputc(c, f) : EOF;
}
static int w_fflush(void *fp) {
  FILE *f = map_sF(fp); return f ? fflush(f) : EOF;
}

/* Keep fopen wrapper to redirect asset paths */
static FILE *w_fopen(const char *path, const char *mode) {
  static FILE *(*real)(const char *, const char *) = NULL;
  if (!real) real = dlsym(RTLD_DEFAULT, "fopen");
  FILE *f = real ? real(path, mode) : NULL;
  if (!f && real && path && mode && mode[0] == 'r' && strncmp(path, "assets/", 7) != 0) {
    char alt[1024]; snprintf(alt, sizeof(alt), "assets/%s", path);
    f = real(alt, mode);
  }
  return f;
}

static unsigned char ctype_tab[1 + 256];
#define _CT_U 0x01
#define _CT_L 0x02
#define _CT_N 0x04
#define _CT_S 0x08
#define _CT_P 0x10
#define _CT_C 0x20
#define _CT_X 0x40
#define _CT_B 0x80
static void ctype_init(void) {
  for (int c = 0; c < 256; c++) {
    unsigned char f = 0;
    if (isupper(c)) f |= _CT_U;
    if (islower(c)) f |= _CT_L;
    if (isdigit(c)) f |= _CT_N;
    if (isspace(c)) f |= _CT_S;
    if (ispunct(c)) f |= _CT_P;
    if (iscntrl(c)) f |= _CT_C;
    if (isxdigit(c)) f |= _CT_X;
    if (c == ' ') f |= _CT_B;
    ctype_tab[1 + c] = f;
  }
}

static void *aw_fromSurface(void *env, void *surface) { (void)env; (void)surface; return (void *)0xAA11; }
static int aw_setBuffersGeometry(void *w, int x, int y, int f) { (void)w; (void)x; (void)y; (void)f; return 0; }
extern int bully_screen_w(void); extern int bully_screen_h(void);
static int aw_getWidth(void *w) { (void)w; return bully_screen_w(); }
static int aw_getHeight(void *w) { (void)w; return bully_screen_h(); }
static void aw_release(void *w) { (void)w; }

#ifndef ASSET_DIR
#define ASSET_DIR "assets"
#endif
typedef struct { FILE *fp; long len; } AAsset;
static void *am_fromJava(void *env, void *obj) { (void)env; (void)obj; return (void *)0xA55E7; }
static void *aa_open(void *mgr, const char *path, int mode) {
  (void)mgr; (void)mode;
  char full[1024]; snprintf(full, sizeof(full), "%s/%s", ASSET_DIR, path);
  FILE *fp = fopen(full, "rb");
  if (!fp) return NULL;
  AAsset *a = calloc(1, sizeof(AAsset)); a->fp = fp;
  fseek(fp, 0, SEEK_END); a->len = ftell(fp); fseek(fp, 0, SEEK_SET);
  return a;
}
static int aa_read(void *h, void *buf, size_t n) { AAsset *a = h; return a ? fread(buf, 1, n, a->fp) : -1; }
static long aa_seek64(void *h, long off, int wh) { AAsset *a = h; if (!a) return -1; fseek(a->fp, off, wh); return ftell(a->fp); }
static long aa_getLength64(void *h) { AAsset *a = h; return a ? a->len : 0; }
static long aa_getRemainingLength64(void *h) { AAsset *a = h; return a ? a->len - ftell(a->fp) : 0; }
static void aa_close(void *h) { AAsset *a = h; if (a) { fclose(a->fp); free(a); } }

static void b_set_abort_message(const char *m) { fprintf(stderr, "[abort_msg] %s\n", m ? m : "?"); }

extern int bully_opengles_version(void);
static int b_system_property_get(const char *name, char *value) {
  if (!name || !value) return 0;
  if (strcmp(name, "ro.opengles.version") == 0) {
    value[0] = 0;
    return snprintf(value, 16, "%d", bully_opengles_version());
  }
  value[0] = 0;
  return 0;
}

static void tl_noop(void) {}

static const unsigned char *w_glGetString(unsigned name) {
  const char *e = getenv("BULLY_GPU");
  const char *ven = "Qualcomm";
  const char *ren = "Adreno (TM) 740";
  if (e && *e) {
    if (strcmp(e, "mali") == 0) { ven = "ARM"; ren = "Mali-G72"; }
    else if (strcmp(e, "powervr") == 0) { ven = "Imagination Technologies"; ren = "PowerVR Rogue G6430"; }
    else if (strcmp(e, "off") == 0) { ven = NULL; ren = NULL; }
    else { ven = "Qualcomm"; ren = e; }
  }
  if (name == 0x1F00 && ven) return (const unsigned char *)ven;
  if (name == 0x1F01 && ren) return (const unsigned char *)ren;
  static const unsigned char *(*real)(unsigned) = NULL;
  if (!real) real = dlsym(RTLD_DEFAULT, "glGetString");
  const unsigned char *r = real ? real(name) : NULL;
  return r ? r : (const unsigned char *)"";
}

// static void (*real_glClear)(unsigned) = NULL;
// static void my_glClear(unsigned mask) {
//   if (!real_glClear) real_glClear = dlsym(RTLD_DEFAULT, "glClear");
//   if (real_glClear) real_glClear(mask | 0x100);
// }

// static void (*real_glTexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *) = NULL;
// static void my_glTexImage2D(unsigned tgt, int lvl, int ifmt, int w, int h, int bord, unsigned fmt, unsigned type, const void *px) {
//   if (!real_glTexImage2D) real_glTexImage2D = dlsym(RTLD_DEFAULT, "glTexImage2D");
//
//   if (ifmt == 0x8058) ifmt = 0x1908;
//   else if (ifmt == 0x8051) ifmt = 0x1907;
//   if (!px && (type == 0x8363 || type == 0x8033 || type == 0x8034)) {
//     type = 0x1401;
//     fmt = 0x1908;
//     ifmt = 0x1908;
//   }
//
//   if (real_glTexImage2D) real_glTexImage2D(tgt, lvl, ifmt, w, h, bord, fmt, type, px);
// }

void bully_imports_init(void) {
  ctype_init();
}

static unsigned my_eglSwapInterval(void *dpy, int interval) {
  (void)dpy;
  if (SDL_GL_SetSwapInterval(interval) == 0) {
    return 1;
  }
  return 0;
}

extern void bully_swap_buffers(void);
static unsigned my_eglSwapBuffers(void *dpy, void *surf) {
  (void)dpy; (void)surf;
  bully_swap_buffers();
  return 1;
}

extern int b_pthread_attr_init(void *a);
extern int b_pthread_attr_destroy(void *a);
extern int b_pthread_attr_setstacksize(void *a, size_t s);
extern int b_pthread_attr_setdetachstate(void *a, int s);
extern int b_pthread_create(pthread_t *thread, const void *attr,
                            void *(*start_routine)(void *), void *arg);
extern int b_mutexattr_init(void *a);
extern int b_mutexattr_destroy(void *a);
extern int b_mutexattr_settype(void *a, int type);
extern int b_mutex_init(void *m, const void *attr);
extern int b_mutex_lock(void *m);
extern int b_mutex_unlock(void *m);
extern int b_mutex_trylock(void *m);
extern int b_mutex_destroy(void *m);
extern int b_cond_init(void *c, const void *attr);
extern int b_cond_wait(void *c, void *m);
extern int b_cond_timedwait(void *c, void *m, const struct timespec *t);
extern int b_cond_signal(void *c);
extern int b_cond_broadcast(void *c);
extern int b_cond_destroy(void *c);
extern int b_rwlock_rdlock(void *r);
extern int b_rwlock_wrlock(void *r);
extern int b_rwlock_unlock(void *r);
extern int b_once(void *once_ctl, void (*init)(void));

DynLibFunction dynlib_functions[] = {
  {"__sF", (uintptr_t)bionic_sF},
  {"fprintf", (uintptr_t)w_fprintf},
  {"vfprintf", (uintptr_t)w_vfprintf},
  {"fwrite", (uintptr_t)w_fwrite},
  {"fputs", (uintptr_t)w_fputs},
  {"fputc", (uintptr_t)w_fputc},
  {"fflush", (uintptr_t)w_fflush},
  {"eglSwapBuffers", (uintptr_t)my_eglSwapBuffers},
  {"eglSwapInterval", (uintptr_t)my_eglSwapInterval},
  {"__errno", (uintptr_t)bionic___errno},
  {"__assert2", (uintptr_t)b_assert2},
  {"__strlen_chk", (uintptr_t)b_strlen_chk},
  {"__strrchr_chk", (uintptr_t)b_strrchr_chk},
  {"__strchr_chk", (uintptr_t)b_strchr_chk},
  {"__strncpy_chk2", (uintptr_t)b_strncpy_chk2},
  {"__android_log_print", (uintptr_t)b_android_log},
  {"android_set_abort_message", (uintptr_t)b_set_abort_message},
  {"__system_property_get", (uintptr_t)b_system_property_get},
  {"_ctype_", (uintptr_t)(ctype_tab + 1)},
  {"ANativeWindow_fromSurface", (uintptr_t)aw_fromSurface},
  {"ANativeWindow_setBuffersGeometry", (uintptr_t)aw_setBuffersGeometry},
  {"ANativeWindow_getWidth", (uintptr_t)aw_getWidth},
  {"ANativeWindow_getHeight", (uintptr_t)aw_getHeight},
  {"ANativeWindow_release", (uintptr_t)aw_release},
  {"AAssetManager_fromJava", (uintptr_t)am_fromJava},
  {"AAssetManager_open", (uintptr_t)aa_open},
  {"AAsset_read", (uintptr_t)aa_read},
  {"AAsset_seek64", (uintptr_t)aa_seek64},
  {"AAsset_getLength64", (uintptr_t)aa_getLength64},
  {"AAsset_getRemainingLength64", (uintptr_t)aa_getRemainingLength64},
  {"AAsset_close", (uintptr_t)aa_close},
  {"glGetString", (uintptr_t)w_glGetString},
  // {"glTexImage2D", (uintptr_t)my_glTexImage2D},
  // {"glClear", (uintptr_t)my_glClear},
  {"fopen", (uintptr_t)w_fopen},
  {"_ZTH7gString", (uintptr_t)tl_noop},
  {"_ZTH8gString2", (uintptr_t)tl_noop},
  {"_ZTHN10ALCcontext13sLocalContextE", (uintptr_t)tl_noop},
  {"_Z24NVThreadGetCurrentJNIEnvv", (uintptr_t)NVThreadGetCurrentJNIEnv},

  {"pthread_mutexattr_init", (uintptr_t)&b_mutexattr_init},
  {"pthread_mutexattr_destroy", (uintptr_t)&b_mutexattr_destroy},
  {"pthread_mutexattr_settype", (uintptr_t)&b_mutexattr_settype},
  {"pthread_mutex_init", (uintptr_t)&b_mutex_init},
  {"pthread_mutex_lock", (uintptr_t)&b_mutex_lock},
  {"pthread_mutex_unlock", (uintptr_t)&b_mutex_unlock},
  {"pthread_mutex_trylock", (uintptr_t)&b_mutex_trylock},
  {"pthread_mutex_destroy", (uintptr_t)&b_mutex_destroy},
  {"pthread_cond_init", (uintptr_t)&b_cond_init},
  {"pthread_cond_wait", (uintptr_t)&b_cond_wait},
  {"pthread_cond_timedwait", (uintptr_t)&b_cond_timedwait},
  {"pthread_cond_signal", (uintptr_t)&b_cond_signal},
  {"pthread_cond_broadcast", (uintptr_t)&b_cond_broadcast},
  {"pthread_cond_destroy", (uintptr_t)&b_cond_destroy},
  {"pthread_rwlock_rdlock", (uintptr_t)&b_rwlock_rdlock},
  {"pthread_rwlock_wrlock", (uintptr_t)&b_rwlock_wrlock},
  {"pthread_rwlock_unlock", (uintptr_t)&b_rwlock_unlock},
  {"pthread_once", (uintptr_t)&b_once},
  {"pthread_attr_init", (uintptr_t)&b_pthread_attr_init},
  {"pthread_attr_destroy", (uintptr_t)&b_pthread_attr_destroy},
  {"pthread_attr_setstacksize", (uintptr_t)&b_pthread_attr_setstacksize},
  {"pthread_attr_setdetachstate", (uintptr_t)&b_pthread_attr_setdetachstate},
  {"pthread_create", (uintptr_t)&b_pthread_create},
};
const int dynlib_functions_count = sizeof(dynlib_functions) / sizeof(dynlib_functions[0]);
