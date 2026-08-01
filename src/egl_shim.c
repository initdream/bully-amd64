#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

// These globals are used by imports.c (EGL stubs)
SDL_Window *g_window = NULL;
SDL_GLContext g_gl_context = NULL;

static int g_w = 1280, g_h = 720;
static int g_is_kmsdrm = 0;

int bully_is_kmsdrm(void) { return g_is_kmsdrm; }
int bully_screen_w(void) { return g_w; }
int bully_screen_h(void) { return g_h; }

int bully_init_gl(void) {
  if (g_gl_context) return 1;

  if (SDL_WasInit(SDL_INIT_VIDEO) == 0 && SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
    fprintf(stderr, "[sdl] InitVideo: %s\n", SDL_GetError());
    if (getenv("SDL_VIDEODRIVER")) {
      unsetenv("SDL_VIDEODRIVER");
      if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)
        fprintf(stderr, "[sdl] InitVideo (auto): %s\n", SDL_GetError());
      else
        fprintf(stderr, "[sdl] InitVideo OK driver auto='%s'\n",
                SDL_GetCurrentVideoDriver());
    }
  }

  SDL_DisplayMode dm;
  if (SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0) {
    g_w = dm.w; g_h = dm.h;
  }

  int msaa = 0;
  { const char *e = getenv("BULLY_MSAA"); if (e) msaa = atoi(e); }
  static const int alpha_try[] = {8, 0};
  int msaa_try[2] = {0, 0}, nmsaa = 1;
  if (msaa > 0) { msaa_try[0] = msaa; msaa_try[1] = 0; nmsaa = 2; }
  for (int j = 0; j < nmsaa && !g_window; j++)
    for (int i = 0; i < 2 && !g_window; i++) {
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
      SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
      SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
      SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
      SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, alpha_try[i]);
      SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
      SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
      SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, msaa_try[j] ? 1 : 0);
      SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, msaa_try[j]);
      g_window = SDL_CreateWindow("Bully", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                  g_w, g_h, SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP);
      if (!g_window)
        fprintf(stderr, "[sdl] CreateWindow alpha=%d msaa=%d: %s\n",
                alpha_try[i], msaa_try[j], SDL_GetError());
    }
    if (!g_window) return 0;

    const char *drv = SDL_GetCurrentVideoDriver();
  g_is_kmsdrm = (drv && SDL_strcmp(drv, "mali") != 0) ? 1 : 0;
  fprintf(stderr, "[gl] backend video='%s' kmsdrm=%d\n", drv?drv:"?", g_is_kmsdrm);

  g_gl_context = SDL_GL_CreateContext(g_window);
  if (!g_gl_context) {
    fprintf(stderr, "[sdl] GL_CreateContext: %s\n", SDL_GetError());
    return 0;
  }
  SDL_GL_MakeCurrent(g_window, g_gl_context);

  const GLubyte *r = glGetString(GL_RENDERER), *v = glGetString(GL_VERSION);
  fprintf(stderr, "[gl] SDL2 GLES2 %dx%d | EGL dpy=%p surf=%p ctx=%p | %s / %s\n",
          g_w, g_h, (void*)eglGetCurrentDisplay(), (void*)eglGetCurrentSurface(EGL_DRAW),
          (void*)eglGetCurrentContext(), r ? (const char*)r : "?", v ? (const char*)v : "?");
  return 1;
}

void bully_egl_objects(uintptr_t *d, uintptr_t *s, uintptr_t *c) {
  uintptr_t real_d = (uintptr_t)eglGetCurrentDisplay();
  uintptr_t real_s = (uintptr_t)eglGetCurrentSurface(EGL_DRAW);
  uintptr_t real_c = (uintptr_t)eglGetCurrentContext();

  if (d) *d = (real_d != 0 && real_d != (uintptr_t)EGL_NO_DISPLAY) ? real_d : 0x1001;
  if (s) *s = (real_s != 0 && real_s != (uintptr_t)EGL_NO_SURFACE) ? real_s : 0x2002;
  if (c) *c = (real_c != 0 && real_c != (uintptr_t)EGL_NO_CONTEXT) ? real_c : 0x3003;
}

int bully_make_current(void) {
  return SDL_GL_MakeCurrent(g_window, g_gl_context) == 0 ? 1 : 0;
}

void bully_release_current(void) {
  SDL_GL_MakeCurrent(g_window, NULL);
}

static void bully_maybe_screenshot(void) {
  static int chk = 0;
  if (++chk % 15) return;
  if (access("/dev/shm/bully_shot", F_OK) != 0) return;
  unlink("/dev/shm/bully_shot");
  int vp[4] = {0,0,0,0};
  glGetIntegerv(GL_VIEWPORT, vp);
  int w = vp[2], h = vp[3];
  if (w <= 0 || h <= 0) return;
  unsigned char *buf = malloc((size_t)w * h * 4);
  if (!buf) return;
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf);
  FILE *o = fopen("/dev/shm/bully_shot.raw", "wb");
  if (o) { fwrite(buf, 1, (size_t)w * h * 4, o); fclose(o); }
  FILE *t = fopen("/dev/shm/bully_shot.txt", "w");
  if (t) { fprintf(t, "%d %d\n", w, h); fclose(t); }
  free(buf);
  fprintf(stderr, "[shot] %dx%d saved to /dev/shm/bully_shot.raw\n", w, h);
}

void bully_swap_buffers(void) {
  if (g_window) {
    bully_maybe_screenshot();
    SDL_GL_SwapWindow(g_window);
  }
}
