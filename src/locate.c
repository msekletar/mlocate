/* locate(1).
   Copyright (C) 2005 FIXME */
#include <config.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fnmatch.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>

#include "db.h"
#include "lib.h"

/* Read a NUL-terminated string from DATABASE FILE to BUF from OFFSET on.
   Return offset after the read string, or (size_t)-1 on I/O error. */
static size_t
read_name (struct growbuf *buf, size_t offset, const char *database,
	   FILE *file)
{
  size_t i;

  /* Surprisingly, this seems to be faster than fread () with a known size. */
  flockfile (file);
  i = offset;
  for (;;)
    {
      int c;

      c = getc (file);
      if (c == 0)
	break;
      if (c == EOF)
	{
	  read_error (database, file, errno);
	  i = (size_t)-1;
	  break;
	}
      gb_alloc (buf, i + 1);
      ((char *)buf->p)[i] = c;
      i++;
    }
  funlockfile (file);
  return i;
}

/* Search for PATTERN in DATABASE;
   return 0 if ok, -1 on error */
static int
search_in_db (const char *database, const char *pattern)
{
  FILE *f;
  struct db_directory dir;
  struct growbuf path;
  int res, err;

  f = open_db (database);
  if (f == NULL)
    {
      res = -1;
      goto err;
    }
  path.p = NULL;
  path.len = 0;
  while (fread (&dir, sizeof (dir), 1, f) == 1)
    {
      size_t dir_name_len;
      uint32_t entries;

      dir_name_len = read_name (&path, 0, database, f);
      if (dir_name_len == (size_t)-1)
	{
	  res = -1;
	  goto err_path;
	}
      gb_alloc (&path, dir_name_len + 1);
      ((char *)path.p)[dir_name_len] = '/';
      dir_name_len++;
      for (entries = ntohl (dir.entries); entries != 0; entries--)
	{
	  struct db_entry entry;
	  size_t name_len;

	  if (fread (&entry, sizeof (entry), 1, f) != 1)
	    {
	      read_error (database, f, errno);
	      res = -1;
	      goto err_path;
	    }
	  name_len = read_name (&path, dir_name_len, database, f);
	  if (name_len == (size_t)-1)
	    {
	      res = -1;
	      goto err_path;
	    }
	  gb_alloc (&path, name_len + 1);
	  ((char *)path.p)[name_len] = 0;
	  if (fnmatch (pattern, path.p, 0) == 0)
	    puts (path.p);
	}
    }
  err = errno;
  /* fread () returned 0 */
  if (ferror (f))
    {
      read_error (database, f, errno);
      res = -1;
      goto err_path;
    }
  res = 0;
  /* Fall through */
 err_path:
  free (path.p);
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
  if (search_in_db (DBFILE, argv[1]) != 0)
    return EXIT_FAILURE;
  return EXIT_SUCCESS;
}
