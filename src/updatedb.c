/* updatedb(8).

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
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <error.h>
#include <mntent.h>
#include <obstack.h>

#include "conf.h"
#include "db.h"
#include "lib.h"

/* A directory entry in memory */
struct entry
{
  size_t name_size;		/* Including the trailing NUL */
  _Bool is_directory;
  char name[];
};

/* Data for directory building */
struct dir_state
{
  /* Obstack of directory paths and 'struct entry' entries */
  struct obstack data_obstack;
  /* Obstack of 'void *' (struct entry *) lists */
  struct obstack list_obstack;
};

/* A directory in memory, using storage in obstacks */
struct directory
{
  uint64_t ctime;
  void **entries;		/* Pointers to struct entry */
  size_t num_entries;
  const char *path;		/* Absolute path */
};

 /* Directory obstack handling */

/* Prepare STATE for reading directories */
static void
dir_state_init (struct dir_state *state)
{
  obstack_init (&state->data_obstack);
  obstack_init (&state->list_obstack);
}

/* Finish building a directory in STATE, store its data to DIR. */
static void
dir_finish (struct directory *dir, struct dir_state *state)
{
  dir->num_entries
    = OBSTACK_OBJECT_SIZE (&state->list_obstack) / sizeof (*dir->entries);
  dir->entries = obstack_finish (&state->list_obstack);
}

 /* Reading of the existing database */

/* The old database or old_db.file == NULL */
static struct db old_db;
/* Next unprocessed directory from the old database or old_dir.path == NULL */
static struct directory old_dir; /* = { 0, }; */

/* Global obstacks for old database reading. */
static struct dir_state old_dir_state;
/* Marker for freeing old_dir_state.data_obstack */
static void *old_dir_data_mark;

/* Read next directory, if any, to old_dir */
static void
next_old_dir (void)
{
  struct db_directory dir;
  size_t i;

  if (old_db.file == NULL)
    return;
  if (old_dir.path != NULL)
    {
      obstack_free (&old_dir_state.list_obstack, old_dir.entries);
      obstack_free (&old_dir_state.data_obstack, old_dir_data_mark);
    }
  if (db_read (&old_db, &dir, sizeof (dir)) != 0)
    goto err;
  old_dir.ctime = ntohll (dir.ctime);
  if (db_read_name (&old_db, &old_dir_state.data_obstack, 1) != 0)
    goto err;
  obstack_1grow (&old_dir_state.data_obstack, 0);
  old_dir.path = obstack_finish (&old_dir_state.data_obstack);
  for (;;)
    {
      struct db_entry entry;
      struct entry *e;
      _Bool is_directory;
      size_t size;

      if (db_read (&old_db, &entry, sizeof (entry)) != 0)
	goto err;
      switch (entry.type)
	{
	case DBE_NORMAL:
	  is_directory = 0;
	  break;

	case DBE_DIRECTORY:
	  is_directory = 1;
	  break;

	case DBE_END:
	  goto done;

	default:
	  goto err;
	}
      assert (offsetof (struct entry, name) <= OBSTACK_SIZE_MAX);
      obstack_blank (&old_dir_state.data_obstack,
		     offsetof (struct entry, name));
      if (db_read_name (&old_db, &old_dir_state.data_obstack, 1) != 0)
	goto err;
      obstack_1grow (&old_dir_state.data_obstack, 0);
      size = (OBSTACK_OBJECT_SIZE (&old_dir_state.data_obstack)
	      - offsetof (struct entry, name));
      e = obstack_finish (&old_dir_state.data_obstack);
      e->name_size = size;
      e->is_directory = is_directory;
      obstack_ptr_grow (&old_dir_state.list_obstack, e);
    }
 done:
  dir_finish (&old_dir, &old_dir_state);
  for (i = 0; i + 1 < old_dir.num_entries; i++)
    {
      struct entry *a, *b;

      a = old_dir.entries[i];
      b = old_dir.entries[i + 1];
      if (strcmp (a->name, b->name) >= 0)
	goto err;
    }
  return;

 err:
  old_dir.path = NULL;
  db_close (&old_db);
  old_db.file = NULL;
}

/* Open the old database and prepare for reading it */
static void
old_db_open (void)
{
  struct obstack obstack;
  FILE *f;
  struct db_header hdr;
  const char *src;
  uint32_t size;

  f = fopen (conf_output, "rb");
  if (f == NULL)
    goto err;
  if (db_open (&old_db, &hdr, f, conf_output, 1) != 0)
    goto err;
  size = ntohl (hdr.conf_size);
  if (size != conf_block_size)
    goto err_old_db;
  obstack_init (&obstack);
  obstack_alignment_mask (&obstack) = 0;
  if (db_read_name (&old_db, &obstack, 1) != 0)
    goto err_obstack;
  obstack_1grow (&obstack, 0);
  if (strcmp (obstack_finish (&obstack), conf_scan_root) != 0)
    goto err_obstack;
  obstack_free (&obstack, NULL);
  src = conf_block;
  while (size != 0)
    {
      char buf[BUFSIZ];
      size_t run;

      run = sizeof (buf);
      if (run > size)
	run = size;
      if (db_read (&old_db, buf, run) != 0)
	goto err_old_db;
      if (memcmp (src, buf, run) != 0)
	goto err_old_db;
      src += run;
      size -= run;
    }
  dir_state_init (&old_dir_state);
  old_dir_data_mark = obstack_alloc (&old_dir_state.data_obstack, 0);
  next_old_dir ();
  return;

 err_obstack:
  obstack_free (&obstack, NULL);
 err_old_db:
  db_close (&old_db);
 err:
  old_db.file = NULL;
}

 /* $PRUNEFS handling */

static int
cmp_string_pointer (const void *xa, const void *xb)
{
  const char *a;
  char *const *b;

  a = xa;
  b = xb;
  return strcmp (a, *b);
}

/* Return 1 if PATH is a mount point of an excluded filesystem */
static _Bool
filesystem_is_excluded (const char *path_)
{
  struct obstack obstack;
  char pbuf[PATH_MAX];
  const char *path;
  FILE *f;
  struct mntent *me;
  _Bool res;

  path = realpath (path_, pbuf);
  if (path == NULL)
    path = path_;
  res = 0;
  f = setmntent (_PATH_MOUNTED, "r");
  if (f == NULL)
    goto err;
  obstack_init (&obstack);
  obstack_alignment_mask (&obstack) = 0;
  while ((me = getmntent (f)) != NULL)
    {
      char *type, *p;

      type = obstack_copy (&obstack, me->mnt_type, strlen (me->mnt_type) + 1);
      for (p = type; *p != 0; p++)
	*p = toupper((unsigned char)*p);
      if (bsearch (type, conf_prunefs, conf_prunefs_len,
		   sizeof (*conf_prunefs), cmp_string_pointer) != NULL)
	{
	  char dbuf[PATH_MAX], *dir;

	  dir = realpath (me->mnt_dir, dbuf);
	  if (dir == NULL)
	    dir = me->mnt_dir;
	  if (strcmp (path, dir) == 0)
	    {
	      res = 1;
	      goto err_f;
	    }
	}
      obstack_free (&obstack, type);
    }
 err_f:
  endmntent (f);
  obstack_free (&obstack, NULL);
 err:
  return res;
}

 /* Filesystem scanning */

/* The new database */
static FILE *new_db;
/* A _temporary_ file name, or NULL if there is no temporary file */
static char *new_db_filename;

/* Global obstacks for filesystem scanning */
static struct dir_state scan_dir_state;

/* Next conf_prunepaths entry */
static size_t conf_prunepaths_index; /* = 0; */

/* Forward declaration */
static int scan (const char *path, int *cwd_fd, const struct stat *st_parent,
		 const char *relative);

/* Write DIR to new_db. */
static void
write_directory (const struct directory *dir)
{
  struct db_directory header;
  struct db_entry entry;
  size_t i;

  header.ctime = htonll (dir->ctime);
  fwrite (&header, sizeof (header), 1, new_db);
  fwrite (dir->path, 1, strlen (dir->path) + 1, new_db);
  for (i = 0; i < dir->num_entries; i++)
    {
      struct entry *e;

      e = dir->entries[i];
      entry.type = e->is_directory != 0 ? DBE_DIRECTORY : DBE_NORMAL;
      fwrite (&entry, sizeof (entry), 1, new_db);
      fwrite (e->name, 1, e->name_size, new_db);
    }
  entry.type = DBE_END;
  fwrite (&entry, sizeof (entry), 1, new_db);
}

/* Scan subdirectories of the current working directory, which has ST, among
   entries in DIR, and write results to new_db.  The current working directory
   is not guaranteed to be preserved on return from this function. */
static void
scan_subdirs (const const struct directory *dir, const struct stat *st)
{
  struct obstack obstack;
  int cwd_fd;
  size_t prefix_len, i;

  prefix_len = strlen (dir->path);
  obstack_init (&obstack);
  if (prefix_len > OBSTACK_SIZE_MAX)
    {
      error (0, 0, _("path name length %zu is too large"), prefix_len);
      goto err_obstack;
    }
  obstack_grow (&obstack, dir->path, prefix_len);
  assert (prefix_len != 0);
  if (dir->path[prefix_len - 1] != '/') /* "/" => "/bin", not "//bin" */
    {
      obstack_1grow (&obstack, '/');
      prefix_len++;
    }
  cwd_fd = -1;
  for (i = 0; i < dir->num_entries; i++)
    {
      struct entry *e;
      
      e = dir->entries[i];
      if (e->is_directory != 0)
	{
	  /* Verified in copy_old_dir () and scan_cwd () */
	  assert (e->name_size <= OBSTACK_SIZE_MAX);
	  obstack_grow (&obstack, e->name, e->name_size);
	  if (scan (obstack_base (&obstack), &cwd_fd, st, e->name) != 0)
	    goto err_cwd_fd;
	  obstack_blank (&obstack, -(ssize_t)e->name_size);
	}
    }
 err_cwd_fd:
  if (cwd_fd != -1)
    close (cwd_fd);
 err_obstack:
  obstack_free (&obstack, NULL);
}

/* If *CWD_FD != -1, open ".", store fd to *CWD_FD; then chdir (RELATIVE),
   failing if there is a race condition; RELATIVE is supposed to match OLD_ST.
   Return 0 if OK, -1 on error */
static int
safe_chdir (int *cwd_fd, const char *relative, const struct stat *old_st)
{
  struct stat st;

  if (*cwd_fd == -1)
    {
      int fd;
      
      fd = open (".", O_RDONLY);
      if (fd == -1)
	return -1;
      *cwd_fd = fd;
    }
  if (chdir (relative) != 0 || lstat (".", &st) != 0)
    return -1;
  if (old_st->st_dev != st.st_dev || old_st->st_ino != st.st_ino)
    return -1; /* Race condition, skip the subtree */
  return 0;
}

/* Copy old_dir to DEST in scan_dir_state;
   Return 1 if DEST contains a subdirectory, 0 otherwise. */
static _Bool
copy_old_dir (struct directory *dest)
{
  _Bool have_subdir;
  size_t i;
      
  /* FIXME: is there an elegant solution? */
  /* Reuse old data.  It is easier to copy them than to handle old_data
     lifetime issues (we must have lookahead, but we want to obstack_free ()
     allocated subtree data without the lookahead). */
  have_subdir = 0;
  for (i = 0; i < old_dir.num_entries; i++)
    {
      struct entry *e, *copy;
      size_t size;

      e = old_dir.entries[i];
      size = offsetof (struct entry, name) + e->name_size;
      if (size > OBSTACK_SIZE_MAX)
	{
	  error (0, 0, _("file name length %zu is too large"), e->name_size);
	  continue;
	}
      copy = obstack_copy (&scan_dir_state.data_obstack, e, size);
      if (copy->is_directory != 0)
	have_subdir = 1;
      obstack_ptr_grow (&scan_dir_state.list_obstack, copy);
      if (conf_verbose != 0)
	printf ("%s/%s\n", old_dir.path, e->name);
    }
  dir_finish (dest, &scan_dir_state);
  return have_subdir;
}

/* Compare two "void *" (struct entry *) values */
static int
cmp_entries (const void *xa, const void *xb)
{
  void *const *a_, *const *b_;
  struct entry *a, *b;

  a_ = xa;
  b_ = xb;
  a = *a_;
  b = *b_;
  return strcmp (a->name, b->name);
}

/* Scan current working directory (PATH) to DEST in scan_dir_state;
   Return -1 if "." can't be opened, 1 if DEST contains a subdirectory,
   0 otherwise. */
static int
scan_cwd (struct directory *dest, const char *path)
{
  DIR *dir;
  struct dirent *de;
  _Bool have_subdir;

  dir = opendir (".");
  if (dir == NULL)
    return -1;
  have_subdir = 0;
  while ((de = readdir (dir)) != NULL)
    {
      struct entry *e;
      size_t name_size, entry_size;
        
      if (strcmp (de->d_name, ".") == 0 || strcmp (de->d_name, "..") == 0)
	continue;
      name_size = strlen (de->d_name) + 1;
      assert (name_size > 1);
      entry_size = offsetof (struct entry, name) + name_size;
      if (entry_size > OBSTACK_SIZE_MAX)
	{
	  error (0, 0, _("file name length %zu too large"), name_size);
	  continue;
	}
      e = obstack_alloc (&scan_dir_state.data_obstack, entry_size);
      e->name_size = name_size;
      memcpy (e->name, de->d_name, name_size);
      e->is_directory = 0;
      /* The check for DT_DIR is to handle platforms which have d_type, but
	 require a feature macro to define DT_* */
#if defined (HAVE_STRUCT_DIRENT_D_TYPE) && defined (DT_DIR)
      if (de->d_type == DT_DIR)
	e->is_directory = 1;
      else if (de->d_type == DT_UNKNOWN)
#endif
	{
	  struct stat st;
	  
	  if (lstat (e->name, &st) == 0 && S_ISDIR (st.st_mode))
	    e->is_directory = 1;
	}
      if (e->is_directory != 0)
	have_subdir = 1;
      obstack_ptr_grow (&scan_dir_state.list_obstack, e);
      if (conf_verbose != 0)
	printf ("%s/%s\n", path, e->name);
    }
  closedir (dir);
  dir_finish (dest, &scan_dir_state);
  qsort (dest->entries, dest->num_entries, sizeof (*dest->entries),
	 cmp_entries);
  return have_subdir;
}

/* Scan filesystem subtree rooted at PATH, which is "./RELATIVE", and write
   results to new_db.  Try to preserve current working directory (opening a
   file descriptor to it in *CWD_FD, if *CWD_FD == -1).  Use ST_PARENT for
   checking whether a PATH is a mount point.  Return -1 if the current working
   directory couldn't be preserved, 0 otherwise.

   Note that PATH may be longer than PATH_MAX, so relative file names should
   always be used. */
static int
scan (const char *path, int *cwd_fd, const struct stat *st_parent,
      const char *relative)
{
  struct directory dir;
  struct stat st;
  void *entries_mark;
  int cmp;
  _Bool have_subdir, did_chdir;

  did_chdir = 0;
  while (conf_prunepaths_index < conf_prunepaths_len
	 && (cmp = dir_path_cmp (conf_prunepaths[conf_prunepaths_index],
				 path)) < 0)
    conf_prunepaths_index++;
  if (conf_prunepaths_index < conf_prunepaths_len && cmp == 0)
    {
      conf_prunepaths_index++;
      goto err;
    }
  if (lstat (relative, &st) != 0)
    goto err;
  if (st.st_dev != st_parent->st_dev && filesystem_is_excluded (path))
    goto err;
  /* "relative" may now become a symlink to somewhere else.  So we use it only
     in safe_chdir (). */
  entries_mark = obstack_alloc (&scan_dir_state.data_obstack, 0);
  cmp = 0;
  while (old_dir.path != NULL && (cmp = dir_path_cmp (old_dir.path, path)) < 0)
    next_old_dir ();
  have_subdir = 0;
  if (old_dir.path != NULL && cmp == 0 && st.st_ctime == (time_t)old_dir.ctime)
    {
      have_subdir = copy_old_dir (&dir);
      next_old_dir ();
    }
  else
    {
      int res;

      did_chdir = 1;
      if (safe_chdir (cwd_fd, relative, &st) != 0)
	goto err;
      res = scan_cwd (&dir, path);
      if (res == -1)
	goto err_chdir;
      have_subdir = res;
    }
  dir.path = path;
  dir.ctime = st.st_ctime;
  write_directory (&dir);
  if (have_subdir != 0)
    {
      if (did_chdir == 0)
	{
	  did_chdir = 1;
	  if (safe_chdir (cwd_fd, relative, &st) != 0)
	    goto err_entries;
	}
      scan_subdirs (&dir, &st);
    }
 err_entries:
  obstack_free (&scan_dir_state.list_obstack, dir.entries);
  obstack_free (&scan_dir_state.data_obstack, entries_mark);
 err_chdir:
  if (did_chdir != 0 && *cwd_fd != -1 && fchdir (*cwd_fd) != 0)
    return -1;
 err:
  return 0;
}

 /* Top level */

/* Unlink new_db_filename */
static void
unlink_db (void)
{
  if (new_db_filename != NULL)
    unlink (new_db_filename);
}

/* Open a temporary file for the new database and initialize its header
   and configuration block.  Exit on error. */
static void
new_db_open (void)
{
  static const uint8_t magic[] = DB_MAGIC;

  struct db_header db_header;
  char *filename;
  int db_fd;
  
  filename = xmalloc (strlen (conf_output) + 8);
  sprintf (filename, "%s.XXXXXX", conf_output);
  db_fd = mkstemp (filename);
  if (db_fd == -1)
    error (EXIT_FAILURE, 0, _("can not open a temporary file for `%s'"),
	   conf_output);
  new_db_filename = filename;
  /* We still allow termination by signals without removing the file, because
     it is unsafe to set db_to_unlink to NULL in presence of signals and
     blind unlinking of the file can remove a file we don't own. */
  new_db = fdopen (db_fd, "wb");
  if (new_db == NULL)
    error (EXIT_FAILURE, errno, _("can not open `%s'"), new_db_filename);
  memset (&db_header, 0, sizeof (db_header));
  assert (sizeof (db_header.magic) == sizeof (magic));
  memcpy (db_header.magic, &magic, sizeof (magic));
  db_header.conf_size = htonl (conf_block_size);
  db_header.version = DB_VERSION_0;
  db_header.check_visibility = conf_check_visibility;
  fwrite (&db_header, sizeof (db_header), 1, new_db);
  fwrite (conf_scan_root, 1, strlen (conf_scan_root) + 1, new_db);
  fwrite (conf_block, 1, conf_block_size, new_db);
}

int
main (int argc, char *argv[])
{
  struct stat st;
  int cwd_fd;

  dir_path_cmp_init ();
  setlocale (LC_ALL, "");
  bindtextdomain (PACKAGE_NAME, LOCALEDIR);
  textdomain (PACKAGE_NAME);
  conf_prepare (argc, argv);
  old_db_open ();
  atexit (unlink_db); /* Relevant only while new_db_filename != NULL */
  new_db_open ();
  dir_state_init (&scan_dir_state);
  if (chdir (conf_scan_root) != 0)
    error (EXIT_FAILURE, errno, _("can not change directory to `%s'"),
	   conf_scan_root);
  if (lstat (".", &st) != 0)
    error (EXIT_FAILURE, errno, _("can not stat () `%s'"), conf_scan_root);
  cwd_fd = -1;
  scan (conf_scan_root, &cwd_fd, &st, ".");
  if (cwd_fd != -1)
    close (cwd_fd);
  if (ferror (new_db))
    error (EXIT_FAILURE, 0, _("I/O error while writing to `%s'"),
	   new_db_filename);
  if (fclose (new_db) != 0)
    error (EXIT_FAILURE, errno, _("I/O error closing `%s'"), new_db_filename);
  if (rename (new_db_filename, conf_output) != 0)
    error (EXIT_FAILURE, errno, _("error replacing `%s'"), conf_output);
  free (new_db_filename);
  new_db_filename = NULL;
  fflush (stdout);
  if (ferror (stdout))
    error (EXIT_FAILURE, 0, _("I/O error while writing to standard output"));
  return EXIT_SUCCESS;
}
