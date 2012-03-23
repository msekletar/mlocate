/* Bind mount detection.  Note: if you change this, change tmpwatch as well.

Copyright (C) 2005, 2007, 2008, 2012 Red Hat, Inc. All rights reserved.
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

#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "obstack.h"

#include "bind-mount.h"
#include "conf.h"
#include "lib.h"

 /* mountinfo handling */

/* A single mountinfo entry */
struct mount
{
  int id, parent_id;
  unsigned dev_major, dev_minor;
  char *root;
  char *mount_point;
  char *fs_type;
  char *source;
};

/* Path to mountinfo */
static const char *mountinfo_path;

/* Pointers to struct mount. */
static void **mount_entries;
static size_t num_mount_entries;

/* Obstack of struct mount entries. */
static struct obstack mount_data_obstack;
static void *mount_data_mark;
/* Obstack of strings referenced in mount entries. */
static struct obstack mount_string_obstack;
static void *mount_string_mark;
/* Obstack of 'void *' (struct mount *) pointers, for mount_entries */
static struct obstack mount_list_obstack;

/* Obstack used for a mountinfo line buffer */
static struct obstack mountinfo_line_obstack;

/* Initialize state for read_mount_entries () */
static void
init_mount_entries (void)
{
  obstack_init (&mount_data_obstack);
  mount_data_mark = obstack_alloc (&mount_data_obstack, 0);
  obstack_init (&mount_string_obstack);
  obstack_alignment_mask (&mount_string_obstack) = 0;
  mount_string_mark = obstack_alloc (&mount_string_obstack, 0);
  obstack_init (&mount_list_obstack);
  mount_entries = obstack_alloc (&mount_list_obstack, 0);
  obstack_init (&mountinfo_line_obstack);
  obstack_alignment_mask (&mountinfo_line_obstack) = 0;
}

/* Read a line from F.
   Return a string pointer (to be freed in mountinfo_line_obstack), or NULL on
   error. */
static char *
read_mount_line (FILE *f)
{
  char *line;

  for (;;)
    {
      char buf[LINE_MAX];
      size_t chunk_length;

      if (fgets (buf, sizeof (buf), f) == NULL)
	{
	  if (feof (f))
	    break;
	  goto error;
	}
      chunk_length = strlen (buf);
      if (chunk_length > 0 && buf[chunk_length - 1] == '\n')
	{
	  obstack_grow (&mountinfo_line_obstack, buf, chunk_length - 1);
	  break;
	}
      obstack_grow (&mountinfo_line_obstack, buf, chunk_length);
    }
  obstack_1grow (&mountinfo_line_obstack, 0);
  return obstack_finish (&mountinfo_line_obstack);

 error:
  line = obstack_finish (&mountinfo_line_obstack);
  obstack_free (&mountinfo_line_obstack, line);
  return NULL;
}

/* Parse a space-delimited entry in STRING, decode octal escapes, write it to
   DEST (allocated from mount_string_obstack) if it is not NULL.
   Return 0 if OK, -1 on error. */
static int
parse_mount_string (char **dest, char **string)
{
  char *src, *ret;

  src = *string;
  while (*src == ' ' || *src == '\t')
    src++;
  if (*src == 0)
    goto error;
  for (;;)
    {
      char c;

      c = *src;
      switch (c)
	{
	case 0: case ' ': case '\t':
	  goto done;

	case '\\':
	  if (src[1] >= '0' && src[1] <= '7'
	      && src[2] >= '0' && src[2] <= '7'
	      && src[3] >= '0' && src[3] <= '7')
	    {
	      unsigned v;

	      v = ((src[1] - '0') << 6) | ((src[2] - '0') << 3)
		| (src[3] - '0');
	      if (v <= UCHAR_MAX)
		{
		  obstack_1grow (&mount_string_obstack, (char)v);
		  src += 4;
		  break;
		}
	    }
	  /* Else fall through */

	default:
	  obstack_1grow (&mount_string_obstack, c);
	  src++;
	}
    }
 done:
  *string = src;
  obstack_1grow (&mount_string_obstack, 0);
  ret = obstack_finish (&mount_string_obstack);
  if (dest != NULL)
    *dest = ret;
  else
    obstack_free (&mount_string_obstack, ret);
  return 0;

 error:
  ret = obstack_finish (&mount_string_obstack);
  obstack_free (&mount_string_obstack, ret);
  return -1;
}

/* Read a single entry from F.
   Return the entry, or NULL on error. */
static struct mount *
read_mount_entry (FILE *f)
{
  struct mount *me;
  char *line;
  size_t offset;
  bool separator_found;

  line = read_mount_line (f);
  if (line == NULL)
    return NULL;
  me = obstack_alloc (&mount_data_obstack, sizeof (*me));
  if (sscanf (line, "%d %d %u:%u%zn", &me->id, &me->parent_id, &me->dev_major,
	      &me->dev_minor, &offset) != 4)
    goto error;
  line += offset;
  if (parse_mount_string (&me->root, &line) != 0
      || parse_mount_string (&me->mount_point, &line) != 0
      || parse_mount_string (NULL, &line) != 0)
    goto error;
  do
    {
      char *option;

      if (parse_mount_string (&option, &line) != 0)
	goto error;
      separator_found = strcmp (option, "-") == 0;
      obstack_free (&mount_string_obstack, option);
    }
  while (!separator_found);
  if (parse_mount_string (&me->fs_type, &line) != 0
      || parse_mount_string (&me->source, &line) != 0
      || parse_mount_string (NULL, &line) != 0)
    goto error;
  return me;

 error:
  /* "line" is the only thing we really need to free, the strings in "me" will
     be freed when read_mount_entries starts again. */
  obstack_free (&mount_data_obstack, me);
  obstack_free (&mountinfo_line_obstack, line);
  return NULL;
}

/* Read mount information from mountinfo_path, update mount_entries and
   num_mount_entries.
   Return 0 if OK, -1 on error. */
static int
read_mount_entries (void)
{
  FILE *f;
  struct mount *me;

  f = fopen (mountinfo_path, "r");
  if (f == NULL)
    return -1;
  obstack_free (&mount_data_obstack, mount_data_mark);
  mount_data_mark = obstack_alloc (&mount_data_obstack, 0);
  obstack_free (&mount_string_obstack, mount_string_mark);
  mount_string_mark = obstack_alloc (&mount_string_obstack, 0);
  obstack_free (&mount_list_obstack, mount_entries);
  while ((me = read_mount_entry (f)) != NULL)
    {
      if (conf_debug_pruning != false)
	/* This is debuging output, don't mark anything for translation */
	fprintf (stderr,
		 " `%s' (%d on %d) is `%s' of `%s' (%u:%u), type `%s'\n",
		 me->mount_point, me->id, me->parent_id, me->root, me->source,
		 me->dev_major, me->dev_minor, me->fs_type);
      obstack_ptr_grow (&mount_list_obstack, me);
    }
  fclose (f);
  num_mount_entries = OBSTACK_OBJECT_SIZE (&mount_list_obstack)
    / sizeof (*mount_entries);
  mount_entries = obstack_finish (&mount_list_obstack);
  return 0;
}

 /* Bind mount path list maintenace and top-level interface. */

/* mountinfo_path file descriptor, or -1 */
static int mountinfo_fd;

/* Known bind mount paths */
static struct string_list bind_mount_paths; /* = { 0, }; */

/* Next bind_mount_paths entry */
static size_t bind_mount_paths_index; /* = 0; */

static struct obstack bind_mount_paths_obstack;
static void *bind_mount_paths_mark;

/* Return a result of comparing A and B suitable for qsort () or bsearch () */
static int
cmp_ints (int a, int b)
{
  if (a > b)
    return 1;
  if (a < b)
    return -1;
  return 0;
}

static int
cmp_mount_entry_pointers (const void *xa, const void *xb)
{
  void *const *a, *const *b;

  a = xa;
  b = xb;
  return cmp_ints (((struct mount *)*a)->id, ((struct mount *)*b)->id);
}

static int
cmp_id_mount_entry (const void *xa, const void *xb)
{
  const int *a;
  void *const *b;

  a = xa;
  b = xb;
  return cmp_ints (*a, ((struct mount *)*b)->id);
}

/* Rebuild bind_mount_paths */
static void
rebuild_bind_mount_paths (void)
{
  size_t i;

  if (conf_debug_pruning != false)
    /* This is debuging output, don't mark anything for translation */
    fprintf (stderr, "Rebuilding bind_mount_paths:\n");
  if (read_mount_entries () != 0)
    return;
  if (conf_debug_pruning != false)
    /* This is debuging output, don't mark anything for translation */
    fprintf (stderr, "Matching bind_mount_paths:\n");
  obstack_free (&bind_mount_paths_obstack, bind_mount_paths_mark);
  bind_mount_paths_mark = obstack_alloc (&bind_mount_paths_obstack, 0);
  bind_mount_paths.len = 0;
  /* Sort by ID to allow quick lookup */
  qsort (mount_entries, num_mount_entries, sizeof (*mount_entries),
	 cmp_mount_entry_pointers);
  for (i = 0; i < num_mount_entries; i++)
    {
      struct mount *me, *parent;
      void **pp;

      me = mount_entries[i];
      pp = bsearch (&me->parent_id, mount_entries, num_mount_entries,
		    sizeof (*mount_entries), cmp_id_mount_entry);
      if (pp == NULL)
	continue;
      parent = *pp;
      if (me->dev_major == parent->dev_major
	  && me->dev_minor == parent->dev_minor
	  && strcmp (me->fs_type, parent->fs_type) == 0
	  && strcmp (me->source, parent->source) == 0)
	{
	  /* We have two mounts from the same device.  Is it a no-op bind
	     mount? */
	  size_t p_mount_len, p_root_len;

	  p_mount_len = strlen (parent->mount_point);
	  p_root_len = strlen (parent->root);
	  /* parent->mount_point should always be a prefix of me->mount_point,
	     don't take any chances. */
	  if (strncmp (me->mount_point, parent->mount_point, p_mount_len) != 0
	      || strncmp (me->root, parent->root, p_root_len) != 0
	      || strcmp (me->mount_point + p_mount_len,
			 me->root + p_root_len) != 0)
	    {
	      char *copy;

	      if (conf_debug_pruning != false)
		/* This is debuging output, don't mark anything for
		   translation */
		fprintf (stderr, " => adding `%s'\n", me->mount_point);
	      copy = obstack_copy (&bind_mount_paths_obstack, me->mount_point,
				   strlen (me->mount_point) + 1);
	      string_list_append (&bind_mount_paths, copy);
	    }
	}
    }
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
  struct pollfd pfd;

  /* Unfortunately (mount --bind $path $path/subdir) would leave st_dev
     unchanged between $path and $path/subdir, so we must keep reparsing
     mountinfo_path each time it changes. */
  pfd.fd = mountinfo_fd;
  pfd.events = POLLPRI;
  if (poll (&pfd, 1, 0) < 0)
    return false;
  if ((pfd.revents & POLLPRI) != 0)
    {
      rebuild_bind_mount_paths ();
      bind_mount_paths_index = 0;
    }
  return string_list_contains_dir_path (&bind_mount_paths,
					&bind_mount_paths_index, path);
}

/* Initialize state for is_bind_mount(), to read data from MOUNTINFO. */
void
bind_mount_init (const char *mountinfo)
{
  mountinfo_path = mountinfo;
  init_mount_entries ();
  obstack_init (&bind_mount_paths_obstack);
  obstack_alignment_mask (&bind_mount_paths_obstack) = 0;
  bind_mount_paths_mark = obstack_alloc (&bind_mount_paths_obstack, 0);
  mountinfo_fd = open (mountinfo_path, O_RDONLY);
  if (mountinfo_fd == -1)
    return;
  rebuild_bind_mount_paths ();
}
