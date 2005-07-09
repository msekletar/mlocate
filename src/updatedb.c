/* updatedb(8).
   Copyright (C) 2005 FIXME */
#include <config.h>

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "db.h"
#include "lib.h"

/* A directory entry in memory */
struct entry
{
  int is_directory;
  size_t name_len;
  char name[];
};

static int
cmp_entries (const void *xa, const void *xb)
{
  const struct entry *a, *b;

  a = xa;
  b = xb;
  return strcmp (a->name, b->name);
}

/* FIXME: "//bin..." */
static void
scan (const char *path, FILE *f)
{
  DIR *dir;
  struct stat dir_st;
  struct dirent *d;
  /* FIXME: obstacks... */
  struct growbuf list, dir_path;
  size_t num_entries, dir_name_len, i;
  struct db_directory header;

  if (lstat (path, &dir_st) != 0 || !S_ISDIR (dir_st.st_mode))
    goto err;
  dir = opendir (path);
  if (dir == NULL)
    goto err;
  list.p = NULL;
  list.len = 0;
  dir_name_len = strlen (path);
  dir_path.p = xmalloc (dir_name_len + 1);
  dir_path.len = dir_name_len + 1;
  memcpy (dir_path.p, path, dir_name_len);
  ((char *)dir_path.p)[dir_name_len] = '/';
  num_entries = 0;
  while ((d = readdir (dir)) != NULL)
    {
      struct entry *e;
      size_t name_len;

      name_len = strlen (d->d_name);
      if (*d->d_name == '.' && (name_len == 1
				|| (d->d_name[1] == '.' && name_len == 2)))
	continue;
      e = xmalloc (sizeof (*e) + name_len + 1);
      e->is_directory = 0;
#ifdef HAVE_STRUCT_DIRENT_D_TYPE
      if (d->d_type == DT_DIR)
	e->is_directory = 1;
      else if (d->d_type == DT_UNKNOWN)
#endif
	{
	  struct stat st;

	  gb_alloc (&dir_path, dir_name_len + 1 + name_len + 1);
	  memcpy ((char *)dir_path.p + dir_name_len + 1, d->d_name,
		  name_len + 1);
	  if (lstat (dir_path.p, &st) == 0 && S_ISDIR (st.st_mode))
	    e->is_directory = 1;
	}
      e->name_len = name_len;
      memcpy (e->name, d->d_name, name_len + 1);
      gb_alloc (&list, (num_entries + 1) * sizeof (struct entry *));
      ((struct entry **)list.p)[num_entries] = e;
      num_entries++;
    }
  closedir (dir);
  qsort (list.p, num_entries, sizeof (struct entry *), cmp_entries);
  header.ctime = htonll (dir_st.st_ctime);
  header.entries = htonl (num_entries);
  if (header.entries != htonl (num_entries))
    {
      error (0, _("Too many entries in directory `%s'"), path);
      goto err_entries;
    }
  header.dir_name_len = htonl (dir_name_len);
  if (header.dir_name_len != htonl (dir_name_len))
    {
      error (0, _("Directory name `%s' too long"), path);
      goto err_entries;
    }
  fwrite (&header, sizeof (header), 1, f);
  fwrite (path, 1, dir_name_len, f);
  for (i = 0; i < num_entries; i++)
    {
      const struct entry *e;
      struct db_entry entry;

      e = ((struct entry **)list.p)[i];
      entry.name_len = htonl (e->name_len);
      if (entry.name_len != htonl (e->name_len))
	fatal (0, _("File name `%s' too long"), e->name);
      entry.is_directory = e->is_directory;
      fwrite (&entry, sizeof (entry), 1, f);
      fwrite (e->name, 1, e->name_len, f);
    }
  for (i = 0; i < num_entries; i++)
    {
      struct entry *e;

      e = ((struct entry **)list.p)[i];
      if (e->is_directory != 0)
	{
	  gb_alloc (&dir_path, dir_name_len + 1 + e->name_len + 1);
	  memcpy ((char *)dir_path.p + dir_name_len + 1, e->name,
		  e->name_len + 1);
	  scan (dir_path.p, f);
	}
    }
 err_entries:
  for (i = 0; i < num_entries; i++)
    free (((struct entry **)list.p)[i]);
  free (dir_path.p);
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
  scan ("/", f);
  if (ferror (f))
    fatal (0, _("I/O error while writing to `%s'"), DBFILE);
  if (fclose (f) != 0)
    fatal (errno, _("I/O error closing `%s'"), DBFILE);
  return EXIT_SUCCESS;
}
