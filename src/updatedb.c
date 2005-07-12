/* updatedb(8).
   Copyright (C) 2005 FIXME */
#include <config.h>

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <obstack.h>

#include "db.h"
#include "lib.h"

/* A directory entry in memory */
struct entry
{
  size_t name_len;
  _Bool is_directory;
  char name[];
};

/* Data for directory building */
struct dir_state
{
  struct obstack entry_obstack; /* Obstack of 'struct entry' entries */
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

/* Functions used by obstack code */
static struct _obstack_chunk *
obstack_chunk_alloc (long size)
{
  return xmalloc (size);
}

#define obstack_chunk_free free

 /* Common functions */

/* Prepare STATE for reading directories */
static void
dir_prepare (struct dir_state *state)
{
  obstack_init (&state->entry_obstack);
  obstack_init (&state->list_obstack);
}

/* FIXME: not used in common */
/* Create a new entry from DE, which was read from the current working
   directory, on STATE->entry_obstack, and add it to STATE->list_obstack.
   return the allocated entry */
static struct entry *
entry_new (struct dir_state *state, const struct dirent *de)
{
  struct entry *e;
  size_t len;
  
  len = strlen (de->d_name);
  e = obstack_alloc (&state->entry_obstack, sizeof (*e) + len + 1);
  e->name_len = len;
  memcpy (e->name, de->d_name, len + 1);
  e->is_directory = 0;
#ifdef HAVE_STRUCT_DIRENT_D_TYPE
  if (de->d_type == DT_DIR)
    e->is_directory = 1;
  else if (de->d_type == DT_UNKNOWN)
#endif
    {
      struct stat st;
      
      if (lstat (e->name, &st) == 0 && S_ISDIR (st.st_mode))
	e->is_directory = 1;
    }
  obstack_ptr_grow (&state->list_obstack, e);
  return e;
}

/* Finish building a directory in STATE, store its data to DIR.  Should be
   called before sorting the entries. */
static void
dir_finish (struct directory *dir, struct dir_state *state)
{
  dir->num_entries
    = obstack_object_size (&state->list_obstack) / sizeof (*dir->entries);
  dir->entries = obstack_finish (&state->list_obstack);
}

 /* Reading the existing database */

/* The old database or NULL */
static FILE *old_db;
static const char *old_db_filename;
/* Next unprocessed directory from the old database or old_dir.path == NULL */
static struct directory old_dir; /* = { 0, }; */

/* Global obstacks for old database reading */
static struct dir_state old_dir_state;
/* Marker for freeing old_dir_state.entry_obstack */
static void *old_dir_entry_mark;

/* Read next directory, if any, to old_dir */
static void
next_old_dir (void)
{
  /* Buffer for old_dir.path */
  static struct growbuf path; /* = { 0, }; */

  struct db_directory dir;
  size_t len, i;

  if (old_db == NULL)
    return;
  if (old_dir.path != NULL)
    {
      obstack_free (&old_dir_state.list_obstack, old_dir.entries);
      obstack_free (&old_dir_state.entry_obstack, old_dir_entry_mark);
    }
  if (fread (&dir, sizeof (dir), 1, old_db) != 1)
    goto err;
  old_dir.ctime = ntohll (dir.ctime);
  len = read_name (&path, 0, old_db_filename, old_db);
  if (len == (size_t)-1)
    goto err;
  gb_alloc (&path, len + 1);
  ((char *)path.p)[len] = 0;
  old_dir.path = path.p;
  for (;;)
    {
      static struct growbuf name; /* = { 0, }; */

      struct db_entry entry;
      struct entry *e;
      _Bool is_directory;

      if (fread (&entry, sizeof (entry), 1, old_db) != 1)
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
      /* FIXME: append directly to target obstack? */
      len = read_name (&name, 0, old_db_filename, old_db);
      if (len == (size_t)-1)
	goto err;
      gb_alloc (&name, len + 1);
      ((char *)name.p)[len] = 0;
      e = obstack_alloc (&old_dir_state.entry_obstack, sizeof (*e) + len + 1);
      e->name_len = len;
      memcpy (e->name, name.p, len + 1);
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
  fclose (old_db);
  old_db = NULL;
}

/* Open the old database FILENAME and prepare for reading it */
static void
old_db_open (const char *filename)
{
  old_db_filename = filename;
  old_db = open_db (filename, 1);
  dir_prepare (&old_dir_state);
  old_dir_entry_mark = obstack_alloc (&old_dir_state.entry_obstack, 0);
  next_old_dir ();
}

 /* Filesystem scanning */

/* Global obstacks for filesystem scanning */
static struct dir_state scan_dir_state;

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

/* Write DIR, contents of current working directory, which is PATH, to F.
   Silently ignore non-fatal errors. */
static void
write_directory (FILE *f, const struct directory *dir)
{
  struct db_directory header;
  struct db_entry entry;
  size_t i;

  header.ctime = htonll (dir->ctime);
  fwrite (&header, sizeof (header), 1, f);
  fwrite (dir->path, 1, strlen (dir->path) + 1, f);
  for (i = 0; i < dir->num_entries; i++)
    {
      struct entry *e;

      e = dir->entries[i];
      entry.type = e->is_directory != 0 ? DBE_DIRECTORY : DBE_NORMAL;
      fwrite (&entry, sizeof (entry), 1, f);
      fwrite (e->name, 1, e->name_len + 1, f);
    }
  entry.type = DBE_END;
  fwrite (&entry, sizeof (entry), 1, f);
}

/* Forward declaration */
static int scan (FILE *f, const char *path, int *cwd, const char *relative);

/* Scan subdirectories of the current working directory, which is PATH, among
   entries in DIR, and write results to F.  Silently ignore non-fatal errors.
   The current working directory is not guaranteed to be preserved on return
   from this function. */
static void
scan_subdirs (FILE *f, const struct directory *dir)
{
  /* FIXME: obstacks... */
  struct growbuf buf;
  int dot_fd;
  size_t prefix_len, i;

  prefix_len = strlen (dir->path);
  buf.p = xmalloc (prefix_len + 1);
  buf.len = prefix_len + 1;
  memcpy (buf.p, dir->path, prefix_len);
  assert (prefix_len != 0);
  if (((char *)buf.p)[prefix_len - 1] != '/') /* "/" => "/bin", not "//bin" */
    {
      ((char *)buf.p)[prefix_len] = '/';
      prefix_len++;
    }
  dot_fd = -1;
  for (i = 0; i < dir->num_entries; i++)
    {
      struct entry *e;
      
      e = dir->entries[i];
      if (e->is_directory != 0)
	{
	  gb_alloc (&buf, prefix_len + e->name_len + 1);
	  memcpy ((char *)buf.p + prefix_len, e->name, e->name_len + 1);
	  if (scan (f, buf.p, &dot_fd, e->name) != 0)
	    goto err_dot_fd;
	}
    }
 err_dot_fd:
  if (dot_fd != -1)
    close (dot_fd);
  free (buf.p);
}

/* If *CWD != -1, open ".", store fd to *CWD; then chdir (RELATIVE).
   Return 0 if OK, -1 on error */
static int
save_chdir (int *cwd, const char *relative)
{
  if (*cwd == -1)
    {
      int fd;
      
      fd = open (".", O_RDONLY);
      if (fd == -1)
	return -1;
      *cwd = fd;
    }
  return chdir (relative);
}

/* Scan filesystem subtree rooted PATH, which is "./RELATIVE", and write
   results to F.  Silently ignore non-fatal errors.  Try to preserve current
   working directory (opening a file descriptor to in in *CWD, if
   *CWD == -1).
   Return -1 if the current working directory couldn't be preserved,
   0 otherwise.

   Note that PATH may be longer than PATH_MAX, so relative file names should
   always be used. */
static int
scan (FILE *f, const char *path, int *cwd, const char *relative)
{
  struct directory directory;
  struct stat st;
  void *entries_mark;
  int cmp;
  _Bool have_subdir, did_chdir;

  did_chdir = 0;
  if (lstat (relative, &st) != 0)
    goto err;
  entries_mark = obstack_alloc (&scan_dir_state.entry_obstack, 0);
  cmp = 0;
  while (old_dir.path != NULL && (cmp = strcmp (old_dir.path, path)) < 0)
    next_old_dir ();
  have_subdir = 0;
  if (old_dir.path != NULL && cmp == 0 && st.st_ctime == (time_t)old_dir.ctime)
    {
      size_t i;

      /* FIXME: is there an elegant solution? */
      /* Reuse old data.  It is easier to copy them than to handle old_data
	 lifetime issues (we must have lookahead, but we want to obstack_free
	 () allocated subtree data without the lookahead). */
      for (i = 0; i < old_dir.num_entries; i++)
	{
	  struct entry *e, *copy;

	  e = old_dir.entries[i];
	  copy = obstack_copy (&scan_dir_state.entry_obstack, e,
			       sizeof (*e) + e->name_len + 1);
	  obstack_ptr_grow (&scan_dir_state.list_obstack, copy);
	  if (copy->is_directory != 0)
	      have_subdir = 1;
	}
      dir_finish (&directory, &scan_dir_state);
      next_old_dir ();
    }
  else
    {
      DIR *dir;
      struct dirent *de;

      if (save_chdir (cwd, relative) != 0)
	goto err;
      did_chdir = 1;
      dir = opendir (".");
      if (dir == NULL)
	goto err;
      while ((de = readdir (dir)) != NULL)
	{
	  struct entry *e;

	  if (strcmp (de->d_name, ".") == 0 || strcmp (de->d_name, "..") == 0)
	    continue;
	  e = entry_new (&scan_dir_state, de);
	  if (e->is_directory != 0)
	    have_subdir = 1;
	}
      closedir (dir);
      dir_finish (&directory, &scan_dir_state);
      qsort (directory.entries, directory.num_entries,
	     sizeof (*directory.entries), cmp_entries);
    }
  directory.path = path;
  directory.ctime = st.st_ctime;
  write_directory (f, &directory);
  if (have_subdir != 0)
    {
      if (did_chdir == 0)
	{
	  if (save_chdir (cwd, relative) != 0)
	    goto err_entries;
	  did_chdir = 1;
	}
      scan_subdirs (f, &directory);
    }
 err_entries:
  obstack_free (&scan_dir_state.list_obstack, directory.entries);
  obstack_free (&scan_dir_state.entry_obstack, entries_mark);
 err:
  if (did_chdir != 0 && fchdir (*cwd) != 0)
    return -1;
  return 0;
}

int
main (int argc, char *argv[])
{
  static const struct db_header db_header = { DB_MAGIC, DB_VERSION_0 };
  
  FILE *f;
  int cwd;
  
  setlocale (LC_ALL, "");
  bindtextdomain (PACKAGE_NAME, LOCALEDIR);
  textdomain (PACKAGE_NAME);
  /* FIXME: arguments... */ (void)argc, (void)argv;
  /* FIXME: ignore if config file changed after creating the database */
  old_db_open (DBFILE);
  /* FIXME: security, opening with O_EXCL etc... */
  /* FIXME? Left there on fatal error */
  f = fopen (DBFILE ".tmp", "wb");
  if (f == NULL)
    fatal (errno, _("Can not open `%s'"), DBFILE ".tmp");
  fwrite (&db_header, sizeof (db_header), 1, f);
  dir_prepare (&scan_dir_state);
  if (chdir ("/") != 0)
    fatal (errno, _("Can not change directory to `/'"));
  cwd = -1;
  scan (f, "/", &cwd, ".");
  if (cwd != -1)
    close (cwd);
  if (ferror (f))
    fatal (0, _("I/O error while writing to `%s'"), DBFILE ".tmp");
  if (fclose (f) != 0)
    fatal (errno, _("I/O error closing `%s'"), DBFILE ".tmp");
  if (rename (DBFILE ".tmp", DBFILE) != 0)
    fatal (errno, _("Error replacing `%s'"), DBFILE);
  chdir ("/home/mitr/mlocate/mlocate");
  return EXIT_SUCCESS;
}
