/* locate(1).

Copyright (C) 2005 Red Hat, Inc. All rights reserved.
This copyrighted material is made available to anyone wishing to use, modify,
copy, or redistribute it subject to the terms and conditions of the GNU General
Public License v.2.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 51 Franklin
Street, Fifth Floor, Boston, MA 02110-1301, USA.

Author: Miloslav Trmac <mitr@redhat.com> */
#include <config.h>

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fnmatch.h>
#include <limits.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <error.h>
#include <obstack.h>

#include "db.h"
#include "lib.h"

/* Contains a single, never-obstack_finish ()'ed object */
static struct obstack path_obstack;
/* ... after this zero-length marker */
static void *path_obstack_mark;

/* Contains a single object */
static struct obstack check_obstack;

/* Skip SIZE bytes in FILENAME F;
   Return 0 if OK, -1 on error */
static int
f_skip (FILE *f, const char *filename, uint32_t size)
{
  _Bool do_fseek;

  do_fseek = 1;
  while (size != 0)
    {
      size_t run;

      if (do_fseek != 0)
	{
	  run = LONG_MAX;
	  if (run > size)
	    run = size;
	  if (fseek (f, size, SEEK_CUR) != 0)
	    {
	      if (errno != ESPIPE)
		{
		  error (0, errno, _("I/O error seeking in `%s'"), filename);
		  goto err;
		}
	      run = 0;
	      do_fseek = 0;
	    }
	}
      else
	{
	  char buf[BUFSIZ];

	  run = sizeof (buf);
	  if (run > size)
	    run = size;
	  run = fread (buf, 1, run, f);
	  if (run == 0)
	    {
	      read_error (filename, f, errno);
	      goto err;
	    }
	}
      size -= run;
    }
  return 0;

 err:
  return -1;
}

/* Check permissions of parent directory of PATH; it should be accessible and
   readable.
   Return 0 if OK, -1 on error */
static int
check_directory_perms (const char *path)
{
  size_t size;
  char *copy, *p, *slash, *last_slash;
  int res;

  res = -1;
  size = strlen (path) + 1;
  assert (size > 1);
  if (size > OBSTACK_SIZE_MAX)
    goto err; /* The error was already reported */
  copy = obstack_copy (&check_obstack, path, size);
  last_slash = strrchr (copy, '/');
  p = copy + 1; /* "/" was checked in main () */
  while ((slash = strchr (p, '/')) != NULL)
    {
      char old;

      old = *slash;
      *slash = 0;
      /* FIXME: caching */
      if (access (copy, slash == last_slash ? R_OK : R_OK | X_OK) != 0)
	goto err_copy;
      *slash = old;
      p = slash + 1;
    }
  res = 0;
 err_copy:
  obstack_free (&check_obstack, copy);
 err:
  return res;
}

/* PATH was found, handle it as necessary (using PATTERN); maintain *VISIBLE:
   if it is -1, check whether the directory containing PATH is accessible
   and readable and set *VISIBLE accordingly; otherwise just use the value */
static void
handle_path (const char *path, int *visible, const char *pattern)
{
  if (fnmatch (pattern, path, 0) == 0)
    {
      if (*visible == -1)
	*visible = check_directory_perms (path) == 0;
      if (*visible == 1)
	puts (path);
    }
}

/* Search for PATTERN in DATABASE;
   return 0 if ok, -1 on error */
static int
search_in_db (const char *database, const char *pattern)
{
  FILE *f;
  struct db_header hdr;
  struct db_directory dir;
  size_t size;
  int res, err, visible;

  res = -1;
  f = open_db (&hdr, database, 0);
  if (f == NULL)
    goto err;
  if (read_name (&path_obstack, f, database) != 0)
    goto err_path;
  obstack_1grow (&path_obstack, 0);
  visible = hdr.check_visibility ? -1 : 1;
  handle_path (obstack_finish (&path_obstack), &visible, pattern);
  obstack_free (&path_obstack, path_obstack_mark);
  if (f_skip (f, database, ntohl (hdr.conf_size)) != 0)
    goto err_path;
  while (fread (&dir, sizeof (dir), 1, f) == 1)
    {
      size_t dir_name_len;

      if (read_name (&path_obstack, f, database) != 0)
	goto err_path;
      size = OBSTACK_OBJECT_SIZE (&path_obstack);
      if (size == 0)
	{
	  error (0, 0, _("invalid empty directory name in `%s'"), database);
	  goto err_path;
	}
      if (size != 1 || *(char *)obstack_base (&path_obstack) != '/')
	obstack_1grow (&path_obstack, '/');
      dir_name_len = OBSTACK_OBJECT_SIZE (&path_obstack);
      visible = hdr.check_visibility ? -1 : 1;
      for (;;)
	{
	  struct db_entry entry;

	  if (fread (&entry, sizeof (entry), 1, f) != 1)
	    {
	      read_error (database, f, errno);
	      goto err_path;
	    }
	  if (entry.type == DBE_END)
	    break;
	  if (read_name (&path_obstack, f, database) != 0)
	    goto err_path;
	  obstack_1grow (&path_obstack, 0);
	  handle_path (obstack_base (&path_obstack), &visible, pattern);
	  size = OBSTACK_OBJECT_SIZE (&path_obstack) - dir_name_len;
	  if (size > OBSTACK_SIZE_MAX)
	    {
	      /* No surprises, please */
	      error (0, 0, _("file name length %zu in `%s' is too large"),
		     size, database);
	      goto err_path;
	    }
	  obstack_blank (&path_obstack, -(ssize_t)size);
	}
      obstack_finish (&path_obstack);
      obstack_free (&path_obstack, path_obstack_mark);
    }
  err = errno;
  /* fread () returned 0 */
  if (ferror (f))
    {
      read_error (database, f, errno);
      goto err_path;
    }
  res = 0;
  /* Fall through */
 err_path:
  obstack_finish (&path_obstack);
  obstack_free (&path_obstack, path_obstack_mark);
  fclose (f);
 err:
  return res;
}

int
main (int argc, char *argv[])
{
  setlocale (LC_ALL, "");
  bindtextdomain (PACKAGE_NAME, LOCALEDIR);
  textdomain (PACKAGE_NAME);
  /* FIXME: arguments... */ (void)argc;
  obstack_init (&path_obstack);
  obstack_alignment_mask (&path_obstack) = 0;
  path_obstack_mark = obstack_alloc (&path_obstack, 0);
  obstack_init (&check_obstack);
  obstack_alignment_mask (&check_obstack) = 0;
  /* Don't call access ("/", R_OK | X_OK) all the time.  This is too strict,
     it is possible to have "/" --x and have a database describing a
     subdirectory, but that is just too improbable. */
  if (access ("/", R_OK | X_OK) == 0)
    {
      if (search_in_db (DBFILE, argv[1]) != 0)
	return EXIT_FAILURE;
    }
  return EXIT_SUCCESS;
}
