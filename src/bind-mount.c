/* Bind mount detection.

Copyright (C) 2005, 2007, 2008 Red Hat, Inc. All rights reserved.
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

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <mntent.h>
#include "obstack.h"
#include "stat-time.h"
#include "timespec.h"

#include "bind-mount.h"
#include "conf.h"
#include "lib.h"

/* Known bind mount paths */
static struct string_list bind_mount_paths; /* = { 0, }; */

/* Next bind_mount_paths entry */
static size_t bind_mount_paths_index; /* = 0; */

/* _PATH_MOUNTED mtime at the time of last rebuild_bind_mount_paths () call */
static struct timespec last_path_mounted_mtime; /* = { 0, }; */

static struct obstack bind_mount_paths_obstack;
static void *bind_mount_paths_mark;

/* Rebuild bind_mount_paths */
static void
rebuild_bind_mount_paths (void)
{
  FILE *f;
  struct mntent *me;

  if (conf_debug_pruning != false)
    /* This is debuging output, don't mark anything for translation */
    fprintf (stderr, "Rebuilding bind_mount_paths:\n");
  obstack_free (&bind_mount_paths_obstack, bind_mount_paths_mark);
  bind_mount_paths_mark = obstack_alloc (&bind_mount_paths_obstack, 0);
  bind_mount_paths.len = 0;
  f = setmntent (_PATH_MOUNTED, "r");
  if (f == NULL)
    goto err;
  while ((me = getmntent (f)) != NULL)
    {
      if (conf_debug_pruning != false)
	/* This is debuging output, don't mark anything for translation */
	fprintf (stderr, " `%s' on `%s', opts `%s'\n", me->mnt_fsname,
		 me->mnt_dir, me->mnt_opts);
      /* Bind mounts "to self" can be used (e.g. by policycoreutils-sandbox) to
	 create another mount point to which Linux name space privacy flags can
	 be attached.  Such a bind mount is not duplicating any part of the
	 directory tree, so it should not be excluded. */
      if (hasmntopt (me, "bind") != NULL
	  && strcmp (me->mnt_fsname, me->mnt_dir) != 0)
	{
	  char dbuf[PATH_MAX], *dir;

	  dir = realpath (me->mnt_dir, dbuf);
	  if (dir == NULL)
	    dir = me->mnt_dir;
	  if (conf_debug_pruning != false)
	    /* This is debuging output, don't mark anything for translation */
	    fprintf (stderr, " => adding `%s'\n", dir);
	  dir = obstack_copy (&bind_mount_paths_obstack, dir, strlen (dir) + 1);
	  string_list_append (&bind_mount_paths, dir);
	}
    }
  endmntent (f);
  /* Fall through */
 err:
  if (conf_debug_pruning != false)
    /* This is debuging output, don't mark anything for translation */
    fprintf (stderr, "...done\n");
  string_list_dir_path_sort (&bind_mount_paths);
}

/* Return true if PATH is a destination of a bind mount.
   (Bind mounts "to self" are ignored.) */
bool
is_bind_mount (const char *path)
{
  struct timespec path_mounted_mtime;
  struct stat st;

  /* Unfortunately (mount --bind $path $path/subdir) would leave st_dev
     unchanged between $path and $path/subdir, so we must keep reparsing
     _PATH_MOUNTED (not PROC_MOUNTS_PATH) each time it changes. */
  if (stat (_PATH_MOUNTED, &st) != 0)
    return false;
  path_mounted_mtime = get_stat_mtime (&st);
  if (timespec_cmp (last_path_mounted_mtime, path_mounted_mtime) < 0)
    {
      rebuild_bind_mount_paths ();
      last_path_mounted_mtime = path_mounted_mtime;
      bind_mount_paths_index = 0;
    }
  return string_list_contains_dir_path (&bind_mount_paths,
					&bind_mount_paths_index, path);
}

/* Initialize state for is_bind_mount(). */
void
bind_mount_init (void)
{
  struct stat st;

  obstack_init (&bind_mount_paths_obstack);
  obstack_alignment_mask (&bind_mount_paths_obstack) = 0;
  bind_mount_paths_mark = obstack_alloc (&bind_mount_paths_obstack, 0);
  if (stat (_PATH_MOUNTED, &st) != 0)
    return;
  rebuild_bind_mount_paths ();
  last_path_mounted_mtime = get_stat_mtime (&st);
}
