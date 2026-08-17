#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <unistd.h>
#include "so_util.h"

#define OFS_SHADOW_SETTING 0x1C
#define OFS_SHADOW_PROFILE 0x38

static uintptr_t g_app_base = 0;
static uintptr_t g_shadow_tex = 0;
static int (*Real_GetMaxShadowOption)(void *) = NULL;
static int (*Bully_GetShadowLevel)(void) = NULL;
static int (*Base_GetShadowLevel)(void) = NULL;
static int g_debug_left = 6;

static uintptr_t g_renderer_global = 0;
static uintptr_t g_es3_vtable = 0;
static uintptr_t g_es2_vtable = 0;
static uintptr_t g_perform_tramp = 0;
static uintptr_t g_setup_tramp = 0;
static time_t g_post_last = 0;

static unsigned long g_trace_n_add_ped = 0;
static unsigned long g_trace_n_calc_values = 0;
static unsigned long g_trace_n_render_static = 0;
static time_t g_trace_last_add_ped = 0;
static time_t g_trace_last_calc_values = 0;
static time_t g_trace_last_render_static = 0;

static void shadows_trace_install(void);

static void *live_settings(void) {
  if (!g_app_base)
    return NULL;
  uintptr_t x = *(uintptr_t *)(g_app_base - 0x500b8);
  if (!x)
    return NULL;
  uintptr_t app = *(uintptr_t *)x;
  if (!app)
    return NULL;
  uintptr_t s = *(uintptr_t *)(app + 0xb0);
  return s ? (void *)s : NULL;
}

static int renderer_es_version(void) {
  if (!g_renderer_global || !g_es3_vtable || !g_es2_vtable)
    return 0;
  uintptr_t renderer = *(uintptr_t *)g_renderer_global;
  if (!renderer)
    return 0;
  uintptr_t vptr = *(uintptr_t *)renderer;
  if (vptr == g_es3_vtable + 16)
    return 3;
  if (vptr == g_es2_vtable + 16)
    return 2;
  return 0;
}

#define DETOUR_LEN 20

static uintptr_t detour_install(uintptr_t fn, uintptr_t hook) {
  uint8_t *fc = (uint8_t *)fn;
  size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
  uintptr_t page_start = fn & ~(page_size - 1);
  size_t plen = ((fn + DETOUR_LEN) - page_start + page_size - 1) & ~(page_size - 1);

  void *tramp = mmap(NULL, DETOUR_LEN * 2, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (tramp == MAP_FAILED)
    return 0;
  memcpy(tramp, fc, DETOUR_LEN);
  uint8_t *t = (uint8_t *)tramp + DETOUR_LEN;
  t[0] = 0xFF; t[1] = 0x25; t[2] = 0; t[3] = 0; t[4] = 0; t[5] = 0;
  *(uint64_t *)(t + 6) = fn + DETOUR_LEN;
  if (mprotect(tramp, DETOUR_LEN * 2, PROT_READ | PROT_EXEC) != 0) {
    munmap(tramp, DETOUR_LEN * 2);
    return 0;
  }
  __builtin___clear_cache((char *)tramp, (char *)tramp + DETOUR_LEN * 2);

  if (mprotect((void *)page_start, plen, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
    munmap(tramp, DETOUR_LEN * 2);
    return 0;
  }
  fc[0] = 0xFF; fc[1] = 0x25; fc[2] = 0; fc[3] = 0; fc[4] = 0; fc[5] = 0;
  *(uint64_t *)(fc + 6) = hook;
  mprotect((void *)page_start, plen, PROT_READ | PROT_EXEC);
  __builtin___clear_cache((char *)fc, (char *)fc + DETOUR_LEN);
  return (uintptr_t)tramp;
}

static uintptr_t my_perform_post_process(void *self, unsigned stage) {
  time_t now = time(NULL);
  if (now != g_post_last) {
    g_post_last = now;
    fprintf(stderr, "[post] PerformPostProcess stage=%u renderer=ES%d\n",
            stage, renderer_es_version());
    fflush(stderr);
  }
  return g_perform_tramp
      ? ((uintptr_t(*)(void *, unsigned))g_perform_tramp)(self, stage)
      : 0;
}

static uintptr_t my_setup_post_process(void *self) {
  fprintf(stderr, "[post] SetupPostProcess called renderer=ES%d\n",
          renderer_es_version());
  fflush(stderr);
  return g_setup_tramp
      ? ((uintptr_t(*)(void *))g_setup_tramp)(self)
      : 0;
}

static void shadows_post_debug_install(void) {
  g_renderer_global = so_find_addr("globalRenderer");
  g_es3_vtable = so_find_addr("_ZTV11RendererES3");
  g_es2_vtable = so_find_addr("_ZTV11RendererES2");
  uintptr_t perform = so_find_addr("_ZN14WorldSceneView18PerformPostProcessE16PostProcessStage");
  uintptr_t setup = so_find_addr("_ZN17BullyGameRenderer16SetupPostProcessEv");
  fprintf(stderr,
          "[post] resolver renderer_global=%p es3vt=%p es2vt=%p perform=%p setup=%p\n",
          (void *)g_renderer_global, (void *)g_es3_vtable, (void *)g_es2_vtable,
          (void *)perform, (void *)setup);
  if (g_perform_tramp || g_setup_tramp)
    return;
  if (perform)
    g_perform_tramp = detour_install(perform, (uintptr_t)my_perform_post_process);
  if (setup)
    g_setup_tramp = detour_install(setup, (uintptr_t)my_setup_post_process);
  so_flush_caches();
  fprintf(stderr, "[post] trampolines perform=%p setup=%p\n",
          (void *)g_perform_tramp, (void *)g_setup_tramp);
}

void shadows_init(void) {
  g_app_base = so_find_addr("application");
  g_shadow_tex = g_app_base + 0x16ef50;
  Real_GetMaxShadowOption =
      (int (*)(void *))so_find_addr("_ZN13BullySettings18GetMaxShadowOptionEv");
  Bully_GetShadowLevel =
      (int (*)(void))so_find_addr("_ZN17BullyGameRenderer14GetShadowLevelEv");
  Base_GetShadowLevel =
      (int (*)(void))so_find_addr("_ZN12GameRenderer14GetShadowLevelEv");

  shadows_trace_install();

  if (getenv("BULLY_POST_DEBUG"))
    shadows_post_debug_install();
}

void shadows_apply(void) {
  void *s = live_settings();
  if (!s || !Real_GetMaxShadowOption)
    return;
  int max_shadow = Real_GetMaxShadowOption(s);
  int *setting = (int *)((char *)s + OFS_SHADOW_SETTING);
  if (*setting < 0)
    *setting = 0;
  if (max_shadow >= 0 && *setting > max_shadow)
    *setting = max_shadow;
  int *profile = (int *)((char *)s + OFS_SHADOW_PROFILE);
  *profile = *setting > 0 ? *setting + 1 : 0;

  if (getenv("BULLY_FORCE_PED_SHADOW") && g_shadow_tex) {
    *(uint8_t *)(g_shadow_tex + 0x18) = 1;
    *(uint8_t *)(g_shadow_tex + 0x19) = 1;
  }

  if (getenv("BULLY_SHADOW_DEBUG") && g_debug_left > 0) {
    g_debug_left--;
    int bgsl = Bully_GetShadowLevel ? Bully_GetShadowLevel() : -99;
    int gsl = Base_GetShadowLevel ? Base_GetShadowLevel() : -99;
    void *ped = g_shadow_tex ? (void *)*(uintptr_t *)g_shadow_tex : NULL;
    void *expl = g_shadow_tex ? (void *)*(uintptr_t *)(g_shadow_tex + 8) : NULL;
    void *head = g_shadow_tex ? (void *)*(uintptr_t *)(g_shadow_tex + 0x10) : NULL;
    int doubleped = g_shadow_tex ? *(uint8_t *)(g_shadow_tex + 0x18) : -1;
    int has = g_shadow_tex ? *(uint8_t *)(g_shadow_tex + 0x19) : -1;
    fprintf(stderr,
            "[shadows] settings=%p max=%d setting=%d profile=%d gsl_bully=%d "
            "gsl_base=%d texped=%p texexpl=%p texhead=%p doubleped=%d has=%d\n",
            (void *)s, max_shadow, *setting, *profile, bgsl, gsl,
            ped, expl, head, doubleped, has);
    fprintf(stderr, "[shadows] renderer=%p vptr=%p ES%d\n",
            g_renderer_global ? *(void **)g_renderer_global : NULL,
            g_renderer_global && *(uintptr_t *)g_renderer_global
                ? *(void **)*(uintptr_t *)g_renderer_global
                : NULL,
            renderer_es_version());
  }
}

static void trace_AddShadowPed(void *this, const void *vec, float dist, int arg3) {
  g_trace_n_add_ped++;
  time_t now = time(NULL);
  if (now > g_trace_last_add_ped) {
    g_trace_last_add_ped = now;
    fprintf(stderr, "[shadows-trace] AddShadowPed count=%lu\n", g_trace_n_add_ped);
    fflush(stderr);
  }
}

static void trace_CalcPedShadowValues(const void *vec) {
  g_trace_n_calc_values++;
  time_t now = time(NULL);
  if (now > g_trace_last_calc_values) {
    g_trace_last_calc_values = now;
    fprintf(stderr, "[shadows-trace] CalcPedShadowValues count=%lu\n", g_trace_n_calc_values);
    fflush(stderr);
  }
}

static void trace_RenderStaticShadows(void *this) {
  g_trace_n_render_static++;
  time_t now = time(NULL);
  if (now > g_trace_last_render_static) {
    g_trace_last_render_static = now;
    fprintf(stderr, "[shadows-trace] RenderStaticShadows count=%lu\n", g_trace_n_render_static);
    fflush(stderr);
  }
}

void shadows_trace_install(void) {
  const char *env = getenv("BULLY_SHADOW_TRACE");
  if (!env || !env[0])
    return;

  so_make_text_writable();

  uintptr_t add_ped = so_find_addr("_ZN7Shadows12AddShadowPedER7CVectorfb");
  uintptr_t calc_vals = so_find_addr("_ZN7Shadows19CalcPedShadowValuesE7Vector3");
  uintptr_t render_static = so_find_addr("_ZN7Shadows19RenderStaticShadowsEv");

  if (add_ped)
    hook_x64(add_ped, (uintptr_t)trace_AddShadowPed);
  if (calc_vals)
    hook_x64(calc_vals, (uintptr_t)trace_CalcPedShadowValues);
  if (render_static)
    hook_x64(render_static, (uintptr_t)trace_RenderStaticShadows);

  so_make_text_executable();
  so_flush_caches();

  fprintf(stderr, "[shadows-trace] installed\n");
}
