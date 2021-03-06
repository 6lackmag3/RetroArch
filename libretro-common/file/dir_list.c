/* Copyright  (C) 2010-2015 The RetroArch team
 *
 * ---------------------------------------------------------------------------------------
 * The following license statement only applies to this file (dir_list.c).
 * ---------------------------------------------------------------------------------------
 *
 * Permission is hereby granted, free of charge,
 * to any person obtaining a copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <file/dir_list.h>
#include <file/file_path.h>
#include <compat/strl.h>
#include <compat/posix_string.h>

#if defined(_WIN32)
#ifdef _MSC_VER
#define setmode _setmode
#endif
#ifdef _XBOX
#include <xtl.h>
#define INVALID_FILE_ATTRIBUTES -1
#else
#include <io.h>
#include <fcntl.h>
#include <direct.h>
#include <windows.h>
#endif
#elif defined(VITA)
#include <psp2/io/fcntl.h>
#include <psp2/io/dirent.h>
#else
#if defined(PSP)
#include <pspiofilemgr.h>
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#endif

#include <retro_miscellaneous.h>

static int qstrcmp_plain(const void *a_, const void *b_)
{
   const struct string_list_elem *a = (const struct string_list_elem*)a_; 
   const struct string_list_elem *b = (const struct string_list_elem*)b_; 

   return strcasecmp(a->data, b->data);
}

static int qstrcmp_dir(const void *a_, const void *b_)
{
   const struct string_list_elem *a = (const struct string_list_elem*)a_; 
   const struct string_list_elem *b = (const struct string_list_elem*)b_; 
   int a_type = a->attr.i;
   int b_type = b->attr.i;


   /* Sort directories before files. */
   if (a_type != b_type)
      return b_type - a_type;
   return strcasecmp(a->data, b->data);
}

/**
 * dir_list_sort:
 * @list      : pointer to the directory listing.
 * @dir_first : move the directories in the listing to the top?
 *
 * Sorts a directory listing.
 *
 **/
void dir_list_sort(struct string_list *list, bool dir_first)
{
   if (list)
      qsort(list->elems, list->size, sizeof(struct string_list_elem),
            dir_first ? qstrcmp_dir : qstrcmp_plain);
}

/**
 * dir_list_free:
 * @list : pointer to the directory listing
 *
 * Frees a directory listing.
 *
 **/
void dir_list_free(struct string_list *list)
{
   string_list_free(list);
}

/**
 *
 * dirent_is_directory:
 * @path         : path to the directory entry.
 * @entry        : pointer to the directory entry.
 *
 * Is the directory listing entry a directory?
 *
 * Returns: true if directory listing entry is
 * a directory, false if not.
 */

static bool dirent_is_directory(const char *path, const void *data)
{
#if defined(_WIN32)
   const WIN32_FIND_DATA *entry = (const WIN32_FIND_DATA*)data;
   return entry->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
#elif defined(PSP) || defined(VITA)

   const SceIoDirent *entry = (const SceIoDirent*)data;
#if defined(PSP)
   return (entry->d_stat.st_attr & FIO_SO_IFDIR) == FIO_SO_IFDIR;
#elif defined(VITA)
   return PSP2_S_ISDIR(entry->d_stat.st_mode);
#endif

#elif defined(DT_DIR)
   const struct dirent *entry = (const struct dirent*)data;
   if (entry->d_type == DT_DIR)
      return true;
   else if (entry->d_type == DT_UNKNOWN /* This can happen on certain file systems. */
         || entry->d_type == DT_LNK)
      return path_is_directory(path);
   return false;
#else /* dirent struct doesn't have d_type, do it the slow way ... */
   const struct dirent *entry = (const struct dirent*)data;
   return path_is_directory(path);
#endif
}

/**
 * parse_dir_entry:
 * @name               : name of the directory listing entry.
 * @file_path          : file path of the directory listing entry.
 * @is_dir             : is the directory listing a directory?
 * @include_dirs       : include directories as part of the finished directory listing?
 * @include_compressed : Include compressed files, even if not part of ext_list.
 * @list               : pointer to directory listing.
 * @ext_list           : pointer to allowed file extensions listing.
 * @file_ext           : file extension of the directory listing entry.
 *
 * Parses a directory listing.
 *
 * Returns: zero on success, -1 on error, 1 if we should
 * continue to the next entry in the directory listing.
 **/
static int parse_dir_entry(const char *name, char *file_path,
      bool is_dir, bool include_dirs, bool include_compressed,
      struct string_list *list, struct string_list *ext_list,
      const char *file_ext)
{
   union string_list_elem_attr attr;
   bool is_compressed_file = false;
   bool supported_by_core  = false;

   attr.i                  = RARCH_FILETYPE_UNSET;

   if (!is_dir)
   {
      is_compressed_file = path_is_compressed_file(file_path);
      if (string_list_find_elem_prefix(ext_list, ".", file_ext))
         supported_by_core = true;
   }

   if (!include_dirs && is_dir)
      return 1;

   if (!strcmp(name, ".") || !strcmp(name, ".."))
      return 1;

   if (!is_dir && ext_list &&
           ((!is_compressed_file && !supported_by_core) ||
            (!supported_by_core && !include_compressed)))
      return 1;

   if (is_dir)
      attr.i = RARCH_DIRECTORY;
   if (is_compressed_file)
      attr.i = RARCH_COMPRESSED_ARCHIVE;
   /* The order of these ifs is important.
    * If the file format is explicitly supported by the libretro-core, we
    * need to immediately load it and not designate it as a compressed file.
    *
    * Example: .zip could be supported as a image by the core and as a
    * compressed_file. In that case, we have to interpret it as a image.
    *
    * */
   if (supported_by_core)
      attr.i = RARCH_PLAIN_FILE;

   if (!string_list_append(list, file_path, attr))
      return -1;

   return 0;
}

#if defined(_WIN32)
#define dirent_opendir(directory, dir) \
{ \
   char path_buf[PATH_MAX_LENGTH]; \
   snprintf(path_buf, sizeof(path_buf), "%s\\*", dir); \
   directory = FindFirstFile(path_buf, &entry); \
}
#elif defined(VITA) || defined(PSP)
#define dirent_opendir(directory, dir) directory = sceIoDopen(dir)
#else
#define dirent_opendir(directory, dir) directory = opendir(dir)
#endif

#if defined(_WIN32)
#define dirent_error(directory) ((directory) == INVALID_HANDLE_VALUE)
#elif defined(VITA) || defined(PSP)
#define dirent_error(directory) ((directory) < 0)
#else
#define dirent_error(directory) (!(directory))
#endif

#if defined(_WIN32)
#define dirent_readdir(directory, entry) (FindNextFile((directory), &(entry)) != 0)
#elif defined(VITA) || defined(PSP)
#define dirent_readdir(directory, entry) (sceIoDread((directory), &(entry)) > 0)
#else
#define dirent_readdir(directory, entry) (entry = readdir(directory))
#endif

#if defined(_WIN32)
#define dirent_closedir(directory) if (directory != INVALID_HANDLE_VALUE) FindClose(directory)
#elif defined(VITA) || defined(PSP)
#define dirent_closedir(directory) sceIoDclose(directory)
#else
#define dirent_closedir(directory) if (directory) closedir(directory)
#endif


/**
 * dir_list_new:
 * @dir                : directory path.
 * @ext                : allowed extensions of file directory entries to include.
 * @include_dirs       : include directories as part of the finished directory listing?
 * @include_compressed : Only include files which match ext. Do not try to match compressed files, etc.
 *
 * Create a directory listing.
 *
 * Returns: pointer to a directory listing of type 'struct string_list *' on success,
 * NULL in case of error. Has to be freed manually.
 **/
struct string_list *dir_list_new(const char *dir,
      const char *ext, bool include_dirs, bool include_compressed)
{
#if defined(_WIN32)
   WIN32_FIND_DATA entry;
   HANDLE directory = INVALID_HANDLE_VALUE;
#elif defined(VITA) || defined(PSP)
   SceUID directory;
   SceIoDirent entry;
#else
   DIR *directory                 = NULL;
   const struct dirent *entry     = NULL;
#endif
   struct string_list *ext_list   = NULL;
   struct string_list *list       = NULL;

   if (!(list = string_list_new()))
      return NULL;

   if (ext)
      ext_list = string_split(ext, "|");

   dirent_opendir(directory, dir);

   if (dirent_error(directory))
      goto error;

   while (dirent_readdir(directory, entry))
   {
      char file_path[PATH_MAX_LENGTH];
      int ret                         = 0;
#ifdef _WIN32
      const char *name                = entry.cFileName;
      bool is_dir = dirent_is_directory(file_path, &entry);
#elif defined(VITA) || defined(PSP)
      const char *name                = entry.d_name;
      bool is_dir = dirent_is_directory(file_path, &entry);
#else
      const char *name                = entry->d_name;
      bool is_dir = dirent_is_directory(file_path, entry);
#endif
      const char *file_ext            = path_get_extension(name);

      fill_pathname_join(file_path, dir, name, sizeof(file_path));

      ret    = parse_dir_entry(name, file_path, is_dir,
            include_dirs, include_compressed, list, ext_list, file_ext);

      if (ret == -1)
         goto error;

      if (ret == 1)
         continue;
   }

   dirent_closedir(directory);

   string_list_free(ext_list);
   return list;

error:
   dirent_closedir(directory);

   string_list_free(list);
   string_list_free(ext_list);
   return NULL;
}
