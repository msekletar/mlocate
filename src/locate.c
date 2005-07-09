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

/* Search for PATTERN in DATABASE;
   return 0 if ok, -1 on error */
static int
search_in_db (const char *database, const char *pattern)
{
  FILE *f;
  struct db_directory dir;
  int res, err;
  struct growbuf gb;

  f = open_db (database);
  if (f == NULL)
    {
      res = -1;
      goto err;
    }
  gb.p = NULL;
  gb.len = 0;
  while (fread (&dir, sizeof (dir), 1, f) == 1)
    {
      size_t dir_name_len;
      uint32_t entries;

      dir_name_len = ntohl (dir.dir_name_len);
      if (dir_name_len != ntohl (dir.dir_name_len))
	{
	  error (0, _("Directory name too long in `%s'"), database);
	  res = -1;
	  goto err_gb;
	}
      gb_alloc (&gb, dir_name_len + 1);
      if (fread (gb.p, 1, dir_name_len, f) != dir_name_len)
	{
	  read_error (database, f, errno);
	  res = -1;
	  goto err_gb;
	}
      ((uint8_t *)gb.p)[dir_name_len] = '/';
      dir_name_len++;
      entries = ntohl (dir.entries);
      while (entries != 0)
	{
	  struct db_entry entry;
	  size_t name_len;

	  if (fread (&entry, sizeof (entry), 1, f) != 1)
	    {
	      read_error (database, f, errno);
	      res = -1;
	      goto err_gb;
	    }
	  name_len = ntohl (entry.name_len);
	  if (name_len != ntohl (entry.name_len)
	      || dir_name_len + name_len + 1 < dir_name_len)
	    {
	      error (0, _("File name too long in `%s'"), database);
	      res = -1;
	      goto err_gb;
	    }
	  gb_alloc (&gb, dir_name_len + name_len + 1);
	  if (fread ((uint8_t *)gb.p + dir_name_len, 1, name_len, f)
	      != name_len)
	    {
	      read_error (database, f, errno);
	      res = -1;
	      goto err_gb;
	    }
	  ((uint8_t *)gb.p)[dir_name_len + name_len] = 0;
	  if (fnmatch (pattern, gb.p, 0) == 0)
	    puts (gb.p);
	  entries--;
	}
    }
  err = errno;
  /* fread () returned 0 */
  if (ferror (f))
    {
      read_error (database, f, errno);
      res = -1;
      goto err_gb;
    }
  res = 0;
  /* Fall through */
 err_gb:
  free (gb.p);
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
