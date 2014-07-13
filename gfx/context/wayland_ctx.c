/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 * 
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include "../../driver.h"
#include "../../general.h"
#include "../gfx_common.h"
#include "../gl_common.h"

#include <wayland-client.h>
#include <wayland-egl.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <signal.h>

static EGLContext g_egl_ctx;
static EGLContext g_egl_hw_ctx;
static EGLSurface g_egl_surf;
static EGLDisplay g_egl_dpy;
static EGLConfig g_config;
static bool g_resize;
static unsigned g_width, g_height;

static struct wl_display *g_dpy;
static struct wl_compositor *g_compositor;
static struct wl_surface *g_surface;
static struct wl_egl_window *g_win;
static int g_fd;

static unsigned g_interval;
static enum gfx_ctx_api g_api;
static unsigned g_major;
static unsigned g_minor;
static bool g_use_hw_ctx;

static volatile sig_atomic_t g_quit;

#ifndef EGL_OPENGL_ES3_BIT_KHR
#define EGL_OPENGL_ES3_BIT_KHR 0x0040
#endif

static void sighandler(int sig)
{
   (void)sig;
   g_quit = 1;
}

static void gfx_ctx_get_video_size(void *data, unsigned *width, unsigned *height);
static void gfx_ctx_destroy(void *data);

static void egl_report_error(void)
{
   EGLint error = eglGetError();
   const char *str = NULL;
   switch (error)
   {
      case EGL_SUCCESS:
         str = "EGL_SUCCESS";
         break;

      case EGL_BAD_DISPLAY:
         str = "EGL_BAD_DISPLAY";
         break;

      case EGL_BAD_SURFACE:
         str = "EGL_BAD_SURFACE";
         break;

      case EGL_BAD_CONTEXT:
         str = "EGL_BAD_CONTEXT";
         break;

      default:
         str = "Unknown";
         break;
   }

   RARCH_ERR("[Wayland/EGL]: #0x%x, %s\n", (unsigned)error, str);
}

static void gfx_ctx_swap_interval(void *data, unsigned interval)
{
   (void)data;
   g_interval = interval;
   if (g_egl_dpy && eglGetCurrentContext())
   {
      RARCH_LOG("[Wayland/EGL]: eglSwapInterval(%u)\n", g_interval);
      if (!eglSwapInterval(g_egl_dpy, g_interval))
      {
         RARCH_ERR("[Wayland/EGL]: eglSwapInterval() failed.\n");
         egl_report_error();
      }
   }
}

static void gfx_ctx_check_window(void *data, bool *quit,
      bool *resize, unsigned *width, unsigned *height, unsigned frame_count)
{
   (void)frame_count;

   wl_display_dispatch_pending(g_dpy);
   wl_display_flush(g_dpy);

   unsigned new_width = *width, new_height = *height;
   gfx_ctx_get_video_size(data, &new_width, &new_height);

   if (new_width != *width || new_height != *height)
   {
      *resize = true;
      *width  = new_width;
      *height = new_height;
   }

   *quit = g_quit;
}

static void gfx_ctx_swap_buffers(void *data)
{
   (void)data;
   eglSwapBuffers(g_egl_dpy, g_egl_surf);
}

static void gfx_ctx_set_resize(void *data, unsigned width, unsigned height)
{
   (void)data;
   (void)width;
   (void)height;
}

static void gfx_ctx_update_window_title(void *data)
{
   (void)data;
   char buf[128], buf_fps[128];
   bool fps_draw = g_settings.fps_show;
   gfx_get_fps(buf, sizeof(buf), fps_draw ? buf_fps : NULL, sizeof(buf_fps));
   if (fps_draw)
      msg_queue_push(g_extern.msg_queue, buf_fps, 1, 1);
}

static void gfx_ctx_get_video_size(void *data, unsigned *width, unsigned *height)
{
   (void)data;
   *width = 640;
   *height = 480;
}

static void registry_handle_global(void *data, struct wl_registry *reg, uint32_t id, const char *interface, uint32_t version)
{
   (void)data;
   (void)version;

   if (!strcmp(interface, "wl_compositor"))
      g_compositor = wl_registry_bind(reg, id, &wl_compositor_interface, 1);
}

static void registry_handle_global_remove(void *data, struct wl_registry *registry,
      uint32_t id)
{
   (void)data;
   (void)registry;
   (void)id;
}

static const struct wl_registry_listener registry_listener = {
   registry_handle_global,
   registry_handle_global_remove,
};

static bool gfx_ctx_init(void *data)
{
#define EGL_ATTRIBS_BASE \
   EGL_SURFACE_TYPE,    EGL_WINDOW_BIT, \
   EGL_RED_SIZE,        1, \
   EGL_GREEN_SIZE,      1, \
   EGL_BLUE_SIZE,       1, \
   EGL_ALPHA_SIZE,      0, \
   EGL_DEPTH_SIZE,      0

   static const EGLint egl_attribs_gl[] = {
      EGL_ATTRIBS_BASE,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
      EGL_NONE,
   };

   static const EGLint egl_attribs_gles[] = {
      EGL_ATTRIBS_BASE,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_NONE,
   };

#ifdef EGL_KHR_create_context
   static const EGLint egl_attribs_gles3[] = {
      EGL_ATTRIBS_BASE,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
      EGL_NONE,
   };
#endif

   static const EGLint egl_attribs_vg[] = {
      EGL_ATTRIBS_BASE,
      EGL_RENDERABLE_TYPE, EGL_OPENVG_BIT,
      EGL_NONE,
   };

   const EGLint *attrib_ptr;
   switch (g_api)
   {
      case GFX_CTX_OPENGL_API:
         attrib_ptr = egl_attribs_gl;
         break;
      case GFX_CTX_OPENGL_ES_API:
#ifdef EGL_KHR_create_context
         if (g_major >= 3)
            attrib_ptr = egl_attribs_gles3;
         else
#endif
            attrib_ptr = egl_attribs_gles;
         break;
      case GFX_CTX_OPENVG_API:
         attrib_ptr = egl_attribs_vg;
         break;
      default:
         attrib_ptr = NULL;
   }

   g_quit = 0;

   g_dpy = wl_display_connect(NULL);
   if (!g_dpy)
   {
      RARCH_ERR("Failed to connect to Wayland server.\n");
      goto error;
   }

   wl_registry_add_listener(wl_display_get_registry(g_dpy), &registry_listener, NULL);
   wl_display_dispatch(g_dpy);
   if (!g_compositor)
   {
      RARCH_ERR("Failed to create compositor.\n");
      goto error;
   }

   g_fd = wl_display_get_fd(g_dpy);

   g_egl_dpy = eglGetDisplay((EGLNativeDisplayType)g_dpy);
   if (!g_egl_dpy)
   {
      RARCH_ERR("Failed to create EGL window.\n");
      goto error;
   }

   EGLint egl_major = 0, egl_minor = 0;
   if (!eglInitialize(g_egl_dpy, &egl_major, &egl_minor))
   {
      RARCH_ERR("Failed to initialize EGL.\n");
      goto error;
   }
   RARCH_LOG("[Wayland/EGL]: EGL version: %d.%d\n", egl_major, egl_minor);

   EGLint num_configs;
   if (!eglChooseConfig(g_egl_dpy, attrib_ptr, &g_config, 1, &num_configs))
   {
      RARCH_ERR("[Wayland/EGL]: eglChooseConfig failed with 0x%x.\n", eglGetError());
      goto error;
   }

   if (num_configs == 0 || !g_config)
   {
      RARCH_ERR("[Wayland/EGL]: No EGL configurations available.\n");
      goto error;
   }

   return true;

error:
   gfx_ctx_destroy(data);
   return false;
}

static EGLint *egl_fill_attribs(EGLint *attr)
{
   switch (g_api)
   {
#ifdef EGL_KHR_create_context
      case GFX_CTX_OPENGL_API:
      {
         unsigned version = g_major * 1000 + g_minor;
         bool core = version >= 3001;
#ifdef GL_DEBUG
         bool debug = true;
#else
         bool debug = g_extern.system.hw_render_callback.debug_context;
#endif

         if (core)
         {
            *attr++ = EGL_CONTEXT_MAJOR_VERSION_KHR;
            *attr++ = g_major;
            *attr++ = EGL_CONTEXT_MINOR_VERSION_KHR;
            *attr++ = g_minor;
            // Technically, we don't have core/compat until 3.2.
            // Version 3.1 is either compat or not depending on GL_ARB_compatibility.
            if (version >= 3002)
            {
               *attr++ = EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR;
               *attr++ = EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR;
            }
         }

         if (debug)
         {
            *attr++ = EGL_CONTEXT_FLAGS_KHR;
            *attr++ = EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR;
         }

         break;
      }
#endif

      case GFX_CTX_OPENGL_ES_API:
         *attr++ = EGL_CONTEXT_CLIENT_VERSION; // Same as EGL_CONTEXT_MAJOR_VERSION
         *attr++ = g_major ? (EGLint)g_major : 2;
#ifdef EGL_KHR_create_context
         if (g_minor > 0)
         {
            *attr++ = EGL_CONTEXT_MINOR_VERSION_KHR;
            *attr++ = g_minor;
         }
#endif
         break;

      default:
         break;
   }

   *attr = EGL_NONE;
   return attr;
}

static bool gfx_ctx_set_video_mode(void *data,
      unsigned width, unsigned height,
      bool fullscreen)
{
   struct sigaction sa = {{0}};
   sa.sa_handler = sighandler;
   sa.sa_flags   = SA_RESTART;
   sigemptyset(&sa.sa_mask);
   sigaction(SIGINT, &sa, NULL);
   sigaction(SIGTERM, &sa, NULL);

   EGLint egl_attribs[16];
   EGLint *attr = egl_attribs;
   attr = egl_fill_attribs(attr);

   g_surface = wl_compositor_create_surface(g_compositor);
   g_win = wl_egl_window_create(g_surface, 640, 480);

   g_egl_surf = eglCreateWindowSurface(g_egl_dpy, g_config, (EGLNativeWindowType)g_win, NULL);

   g_egl_ctx = eglCreateContext(g_egl_dpy, g_config, EGL_NO_CONTEXT,
         attr != egl_attribs ? egl_attribs : NULL);

   RARCH_LOG("[Wayland/EGL]: Created context: %p.\n", (void*)g_egl_ctx);
   if (g_egl_ctx == EGL_NO_CONTEXT)
      goto error;

   if (g_use_hw_ctx)
   {
      g_egl_hw_ctx = eglCreateContext(g_egl_dpy, g_config, g_egl_ctx,
            attr != egl_attribs ? egl_attribs : NULL);
      RARCH_LOG("[Wayland/EGL]: Created shared context: %p.\n", (void*)g_egl_hw_ctx);

      if (g_egl_hw_ctx == EGL_NO_CONTEXT)
         goto error;
   }

   g_egl_surf = eglCreateWindowSurface(g_egl_dpy, g_config, (EGLNativeWindowType)g_win, NULL);
   if (!g_egl_surf)
      goto error;

   if (!eglMakeCurrent(g_egl_dpy, g_egl_surf, g_egl_surf, g_egl_ctx))
      goto error;

   RARCH_LOG("[Wayland/EGL]: Current context: %p.\n", (void*)eglGetCurrentContext());

   gfx_ctx_swap_interval(data, g_interval);
   wl_display_dispatch_pending(g_dpy);
   wl_display_flush(g_dpy);
   return true;

error:
   gfx_ctx_destroy(data);
   return false;
}

static void gfx_ctx_destroy(void *data)
{
   (void)data;
   if (g_egl_dpy)
   {
      if (g_egl_ctx)
      {
         eglMakeCurrent(g_egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
         eglDestroyContext(g_egl_dpy, g_egl_ctx);
      }

      if (g_egl_hw_ctx)
         eglDestroyContext(g_egl_dpy, g_egl_hw_ctx);

      if (g_egl_surf)
         eglDestroySurface(g_egl_dpy, g_egl_surf);
      eglTerminate(g_egl_dpy);
   }

   g_egl_ctx     = NULL;
   g_egl_hw_ctx  = NULL;
   g_egl_surf    = NULL;
   g_egl_dpy     = NULL;
   g_config      = 0;

   // Do not close g_dpy. We'll keep one for the entire application lifecycle to work-around nVidia EGL limitations.
}

static void gfx_ctx_input_driver(void *data, const input_driver_t **input, void **input_data)
{
   (void)data;
   //void *wl    = input_wayland.init();
   //*input      = wl ? &input_wayland : NULL;
   //*input_data = wl;
   *input = NULL;
   *input_data = NULL;
}

static bool gfx_ctx_has_focus(void *data)
{
   (void)data;
   return true;
}

static gfx_ctx_proc_t gfx_ctx_get_proc_address(const char *symbol)
{
   return eglGetProcAddress(symbol);
}

static bool gfx_ctx_bind_api(void *data, enum gfx_ctx_api api, unsigned major, unsigned minor)
{
   (void)data;
   g_major = major;
   g_minor = minor;
   g_api = api;
   switch (api)
   {
      case GFX_CTX_OPENGL_API:
#ifndef EGL_KHR_create_context
         if ((major * 1000 + minor) >= 3001)
            return false;
#endif
         return eglBindAPI(EGL_OPENGL_API);
      case GFX_CTX_OPENGL_ES_API:
#ifndef EGL_KHR_create_context
         if (major >= 3)
            return false;
#endif
         return eglBindAPI(EGL_OPENGL_ES_API);
      case GFX_CTX_OPENVG_API:
         return eglBindAPI(EGL_OPENVG_API);
      default:
         return false;
   }
}

static void gfx_ctx_bind_hw_render(void *data, bool enable)
{
   (void)data;
   g_use_hw_ctx = enable;
   if (g_egl_dpy && g_egl_surf)
      eglMakeCurrent(g_egl_dpy, g_egl_surf, g_egl_surf, enable ? g_egl_hw_ctx : g_egl_ctx);
}

const gfx_ctx_driver_t gfx_ctx_wayland = {
   gfx_ctx_init,
   gfx_ctx_destroy,
   gfx_ctx_bind_api,
   gfx_ctx_swap_interval,
   gfx_ctx_set_video_mode,
   gfx_ctx_get_video_size,
   NULL,
   gfx_ctx_update_window_title,
   gfx_ctx_check_window,
   gfx_ctx_set_resize,
   gfx_ctx_has_focus,
   gfx_ctx_swap_buffers,
   gfx_ctx_input_driver,
   gfx_ctx_get_proc_address,
   NULL,
   NULL,
   NULL,
   "wayland",
   gfx_ctx_bind_hw_render,
};

