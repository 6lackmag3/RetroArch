/* RetroArch - A frontend for libretro.
 * Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 * Copyright (C) 2011-2015 - Daniel De Matteis
 *
 * RetroArch is free software: you can redistribute it and/or modify it under the terms
 * of the GNU General Public License as published by the Free Software Found-
 * ation, either version 3 of the License, or (at your option) any later version.
 *
 * RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with RetroArch.
 * If not, see <http://www.gnu.org/licenses/>.
 */

#include <psp2/moduleinfo.h>
#include <psp2/power.h>

#include <stdint.h>
#include <boolean.h>
#include <stddef.h>
#include <string.h>

#include <file/file_path.h>
#ifndef IS_SALAMANDER
#include <file/file_list.h>
#endif

#include "../../defines/psp_defines.h"
#include "../../general.h"

PSP2_MODULE_INFO(0, 0, "RetroArch");

char retroarch_path[512];

static bool exit_spawn = false;
static bool exitspawn_start_game = false;

#ifdef IS_SALAMANDER
#include "../../file_ext.h"
#endif

static void frontend_vita_get_environment_settings(int *argc, char *argv[],
      void *args, void *params_data)
{
   (void)args;
#ifndef IS_SALAMANDER
#if defined(HAVE_LOGGER)
   logger_init();
#elif defined(HAVE_FILE_LOGGER)
   global_t *global  = global_get_ptr();
   global->log_file = fopen("cache0:/retroarch-log.txt", "w");
#endif
#endif

   strlcpy(retroarch_path, "cache0:/retroarch/", sizeof(retroarch_path));

   strlcpy(g_defaults.dir.port, retroarch_path, sizeof(g_defaults.dir.port));
   RARCH_LOG("port dir: [%s]\n", g_defaults.dir.port);

   fill_pathname_join(g_defaults.dir.assets, g_defaults.dir.port,
         "media", sizeof(g_defaults.dir.assets));
   fill_pathname_join(g_defaults.dir.core, g_defaults.dir.port,
         "cores", sizeof(g_defaults.dir.core));
   fill_pathname_join(g_defaults.dir.core_info, g_defaults.dir.port,
         "cores", sizeof(g_defaults.dir.core_info));
   fill_pathname_join(g_defaults.dir.savestate, g_defaults.dir.core,
         "savestates", sizeof(g_defaults.dir.savestate));
   fill_pathname_join(g_defaults.dir.sram, g_defaults.dir.core,
         "savefiles", sizeof(g_defaults.dir.sram));
   fill_pathname_join(g_defaults.dir.system, g_defaults.dir.core,
         "system", sizeof(g_defaults.dir.system));
   fill_pathname_join(g_defaults.dir.playlist, g_defaults.dir.core,
         "playlists", sizeof(g_defaults.dir.playlist));
   fill_pathname_join(g_defaults.path.config, g_defaults.dir.port,
         "retroarch.cfg", sizeof(g_defaults.path.config));
   fill_pathname_join(g_defaults.dir.cheats, g_defaults.dir.cheats,
         "cheats", sizeof(g_defaults.dir.cheats));
   fill_pathname_join(g_defaults.dir.remap, g_defaults.dir.remap,
         "remaps", sizeof(g_defaults.dir.remap));

   struct rarch_main_wrap *params = NULL;
   params = (struct rarch_main_wrap*)params_data;

   params->verbose = true;

#ifndef IS_SALAMANDER
   if (argv[1] && (argv[1][0] != '\0'))
   {
      static char path[PATH_MAX_LENGTH];
      struct rarch_main_wrap *args = NULL;

      *path = '\0';
      args = (struct rarch_main_wrap*)params_data;

      if (args)
      {
         strlcpy(path, argv[1], sizeof(path));

         args->touched        = true;
         args->no_content     = false;
         args->verbose        = false;
         args->config_path    = NULL;
         args->sram_path      = NULL;
         args->state_path     = NULL;
         args->content_path   = path;
         args->libretro_path  = NULL;

         RARCH_LOG("argv[0]: %s\n", argv[0]);
         RARCH_LOG("argv[1]: %s\n", argv[1]);
         RARCH_LOG("argv[2]: %s\n", argv[2]);

         RARCH_LOG("Auto-start game %s.\n", argv[1]);
      }
   }
#endif
}

static void frontend_vita_deinit(void *data)
{
   (void)data;
#ifndef IS_SALAMANDER
   global_t *global   = global_get_ptr();
   global->verbosity = false;

#ifdef HAVE_FILE_LOGGER
   if (global->log_file)
      fclose(global->log_file);
   global->log_file = NULL;
#endif

#endif
}

static void frontend_vita_shutdown(bool unused)
{
   (void)unused;
   sceKernelExitProcess(0);
}

static void frontend_vita_init(void *data)
{
   (void)data;
}

static void frontend_vita_exec(const char *path, bool should_load_game)
{
#if defined(HAVE_KERNEL_PRX) || defined(IS_SALAMANDER)
   char argp[512] = {0};
   SceSize   args = 0;

   argp[0] = '\0';
   strlcpy(argp, retroarch_path, sizeof(argp));
   args = strlen(argp) + 1;

#ifndef IS_SALAMANDER
   global_t *global   = global_get_ptr();

   if (should_load_game && global->path.fullpath[0] != '\0')
   {
      argp[args] = '\0';
      strlcat(argp + args, global->path.fullpath, sizeof(argp) - args);
      args += strlen(argp + args) + 1;
   }
#endif

   RARCH_LOG("Attempt to load executable: [%s].\n", path);

   exitspawn_kernel(path, args, argp);

#endif
}

static void frontend_vita_set_fork(bool exit, bool start_game)
{
   exit_spawn = true;
   exitspawn_start_game = start_game;
}

static void frontend_vita_exitspawn(char *s, size_t len)
{
   bool should_load_game = false;
#ifndef IS_SALAMANDER
   should_load_game = exitspawn_start_game;

   if (!exit_spawn)
      return;
#endif
   frontend_vita_exec(s, should_load_game);
}

static int frontend_vita_get_rating(void)
{
   return 4;
}

static enum frontend_powerstate frontend_vita_get_powerstate(int *seconds, int *percent)
{
   enum frontend_powerstate ret = FRONTEND_POWERSTATE_NONE;
   int plugged                  = scePowerIsPowerOnline();
   int charging                 = scePowerIsBatteryCharging();

   *percent = scePowerGetBatteryLifePercent();
   *seconds = scePowerGetBatteryLifeTime() * 60;

   if (charging)
      ret = FRONTEND_POWERSTATE_CHARGING;
   else if (plugged)
      ret = FRONTEND_POWERSTATE_CHARGED;
   else
      ret = FRONTEND_POWERSTATE_ON_POWER_SOURCE;

   return ret;
}

enum frontend_architecture frontend_vita_get_architecture(void)
{
   return FRONTEND_ARCH_MIPS;
}

static int frontend_vita_parse_drive_list(void *data)
{
#ifndef IS_SALAMANDER
   file_list_t *list = (file_list_t*)data;

   menu_list_push(list,
         "cache0:/", "", MENU_FILE_DIRECTORY, 0, 0);
#endif

   return 0;
}

frontend_ctx_driver_t frontend_ctx_vita = {
   frontend_vita_get_environment_settings,
   frontend_vita_init,
   frontend_vita_deinit,
   frontend_vita_exitspawn,
   NULL,                         /* process_args */
   frontend_vita_exec,
   frontend_vita_set_fork,
   frontend_vita_shutdown,
   NULL,                         /* get_name */
   NULL,                         /* get_os */
   frontend_vita_get_rating,
   NULL,                         /* load_content */
   frontend_vita_get_architecture,
   frontend_vita_get_powerstate,
   frontend_vita_parse_drive_list,
   "vita",
};
