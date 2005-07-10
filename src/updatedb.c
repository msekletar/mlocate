/* updatedb(8).
   Copyright (C) 2005 FIXME */
#include <config.h>

#include <assert.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "db.h"
#include "lib.h"

/* A directory entry in memory */
struct entry
{
  size_t name_len;
  _Bool is_directory;
  char name[];
};

/* Create a new entry from D, which was read from the current working
   directory */
static struct entry *
entry_new (const struct dirent *d)
{
  struct entry *e;
  size_t len;
  
  len = strlen (d->d_name);
  e = xmalloc (sizeof (*e) + len + 1);
  e->name_len = len;
  memcpy (e->name, d->d_name, len + 1);
  e->is_directory = 0;
#ifdef HAVE_STRUCT_DIRENT_D_TYPE
  if (d->d_type == DT_DIR)
    e->is_directory = 1;
  else if (d->d_type == DT_UNKNOWN)
#endif
    {
      struct stat st;
      
      if (lstat (e->name, &st) == 0 && S_ISDIR (st.st_mode))
	e->is_directory = 1;
    }
  return e;
}

/* Compare two "struct entry *" values */
static int
cmp_entries (const void *xa, const void *xb)
{
  struct entry *const *a, *const *b;

  a = xa;
  b = xb;
  return strcmp ((*a)->name, (*b)->name);
}

/* Write contents (NUM_ENTRIES ENTRIES) of current working directory, which is
   PATH, to F.  Silently ignore non-fatal errors except when we are throwing
   data away.
   return 0 if OK, -1 on error */
static int
write_directory (FILE *f, const char *path, struct entry **entries,
		 size_t num_entries)
{
  struct db_directory header;
  struct stat dir_st;
  size_t i;

  if (lstat (".", &dir_st) != 0)
    return -1;
  header.ctime = htonll (dir_st.st_ctime);
  header.entries = htonl (num_entries);
  if (header.entries != htonl (num_entries))
    {
      /* FIXME: end marker instead of a count */
      error (0, _("Too many entries in directory `%s'"), path);
      return -1;
    }
  fwrite (&header, sizeof (header), 1, f);
  fwrite (path, 1, strlen (path) + 1, f);
  for (i = 0; i < num_entries; i++)
    {
      const struct entry *e;
      struct db_entry entry;

      e = entries[i];
      entry.is_directory = e->is_directory;
      fwrite (&entry, sizeof (entry), 1, f);
      fwrite (e->name, 1, e->name_len + 1, f);
    }
  return 0;
}

/* Forward declaration */
static void scan (FILE *f, const char *path);

/* Scan subdirectories of the current working directory, which is PATH, among
   NUM_ENTRIES ENTRIES, and write results to F.  Silently ignore non-fatal
   errors except when we are throwing data away.  The current working directory
   is not guaranteed to be preserved on return from this function. */
static void
scan_subdirs (FILE *f, const char *path, struct entry **entries,
	      size_t num_entries)
{
  /* FIXME: obstacks... */
  struct growbuf buf;
  int dot_fd;
  size_t prefix_len, i;

  prefix_len = strlen (path);
  buf.p = xmalloc (prefix_len + 1);
  buf.len = prefix_len + 1;
  memcpy (buf.p, path, prefix_len);
  assert (prefix_len != 0);
  if (((char *)buf.p)[prefix_len - 1] != '/') /* "/" => "/bin", not "//bin" */
    {
      ((char *)buf.p)[prefix_len] = '/';
      prefix_len++;
    }
  dot_fd = open (".", O_RDONLY);
  if (dot_fd == -1)
    goto err_buf;
  for (i = 0; i < num_entries; i++)
    {
      struct entry *e;
      
      e = entries[i];
      if (e->is_directory != 0 && chdir (e->name) == 0)
	{
	  gb_alloc (&buf, prefix_len + e->name_len + 1);
	  memcpy ((char *)buf.p + prefix_len, e->name, e->name_len + 1);
	  scan (f, buf.p);
	  if (fchdir (dot_fd) != 0)
	    goto err_dot_fd;
	}
    }
 err_dot_fd:
  close (dot_fd);
 err_buf:
  free (buf.p);
}

/* Scan filesystem subtree rooted at the current working directory, which is
   PATH, and write results to F.  Silently ignore non-fatal errors except when
   we are throwing data away.  The current working directory is not guaranteed
   to be preserved on return from this function.

   Note that PATH may be longer than PATH_MAX, so relative file names should
   always be used. */
static void
scan (FILE *f, const char *path)
{
  DIR *dir;
  struct dirent *d;
  /* FIXME: obstacks... */
  struct growbuf list;
  size_t num_entries, i;
  _Bool have_subdir;

  dir = opendir (".");
  if (dir == NULL)
    goto err;
  list.p = NULL;
  list.len = 0;
  num_entries = 0;
  have_subdir = 0;
  while ((d = readdir (dir)) != NULL)
    {
      struct entry *e;

      if (strcmp (d->d_name, ".") == 0 || strcmp (d->d_name, "..") == 0)
	continue;
      e = entry_new (d);
      if (e->is_directory != 0)
	have_subdir = 1;
      gb_alloc (&list, (num_entries + 1) * sizeof (struct entry *));
      ((struct entry **)list.p)[num_entries] = e;
      num_entries++;
    }
  closedir (dir);
  qsort (list.p, num_entries, sizeof (struct entry *), cmp_entries);
  if (write_directory (f, path, list.p, num_entries) != 0)
    goto err_entries;
  if (have_subdir != 0)
    scan_subdirs (f, path, list.p, num_entries);
 err_entries:
  for (i = 0; i < num_entries; i++)
    free (((struct entry **)list.p)[i]);
  free (list.p);
 err:
  ;
}

int
main (int argc, char *argv[])
{
  static const struct db_header db_header = { DB_MAGIC, DB_VERSION_0 };
  
  FILE *f;
  
  setlocale (LC_ALL, "");
  bindtextdomain (PACKAGE_NAME, LOCALEDIR);
  textdomain (PACKAGE_NAME);
  /* FIXME: arguments... */ (void)argc, (void)argv;
  /* FIXME: move after finished */
  f = fopen (DBFILE, "wb");
  if (f == NULL)
    fatal (errno, _("Can not open `%s'"), DBFILE);
  fwrite (&db_header, sizeof (db_header), 1, f);
  if (chdir ("/") != 0)
    fatal (errno, _("Can not change directory to `/'"));
  scan (f, "/");
  if (ferror (f))
    fatal (0, _("I/O error while writing to `%s'"), DBFILE);
  if (fclose (f) != 0)
    fatal (errno, _("I/O error closing `%s'"), DBFILE);
  return EXIT_SUCCESS;
}
