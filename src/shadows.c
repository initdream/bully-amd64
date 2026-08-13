#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "so_util.h"

#define OFS_SHADOW_SETTING 0x1C
#define OFS_SHADOW_PROFILE 0x38

static uintptr_t g_app_base = 0;
static uintptr_t g_shadow_tex = 0;
static int (*Real_GetMaxShadowOption)(void *) = NULL;
static int (*Bully_GetShadowLevel)(void) = NULL;
static int (*Base_GetShadowLevel)(void) = NULL;
static int g_debug_left = 6;

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
