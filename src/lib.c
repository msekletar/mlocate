/* Common functions.

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
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include <error.h>
#include <obstack.h>

#include "db.h"
#include "lib.h"

/* Convert VAL to big endian */
uint64_t
htonll (uint64_t val)
{
  uint32_t low, high;
  uint64_t ret;

  low = htonl ((uint32_t)val);
  high = htonl (val >> 32);
  assert (sizeof (ret) == sizeof (high) + sizeof (low));
  memcpy (&ret, &high, sizeof (high));
  memcpy ((unsigned char *)&ret + sizeof (high), &low, sizeof (low));
  return ret;
}

/* Convert VAL from big endian */
uint64_t
ntohll (uint64_t val)
{
  uint32_t low, high;

  assert (sizeof (high) + sizeof (low) == sizeof (val));
  memcpy (&high, &val, sizeof (high));
  memcpy (&low, (unsigned char *)&val + sizeof (high), sizeof (low));
  return (uint64_t)ntohl (high) << 32 | ntohl (low);
}

/* A mapping table for dir_path_cmp: '\0' < '/' < anything else */
static unsigned char dir_path_cmp_table[UCHAR_MAX + 1];

/* Initialize dir_path_cmp_table */
void
dir_path_cmp_init (void)
{
  size_t i;
  unsigned char val;

  dir_path_cmp_table[0] = 0;
  dir_path_cmp_table[1] = '/';
  val = (unsigned char)1;
  for (i = 2; i < ARRAY_SIZE (dir_path_cmp_table); i++)
    {
      if (val == '/')
	val++;
      dir_path_cmp_table[i] = val;
      val++;
    }
}

/* Compare two path names using the database directory order. This is not
   exactly strcmp () order: "a" < "a.b", so "a/z" < "a.b". */
int
dir_path_cmp (const char *a, const char *b)
{
  while (*a == *b && *a != 0)
    {
      a++;
      b++;
    }
  return ((int)dir_path_cmp_table[(unsigned char)*a]
	  - (int)dir_path_cmp_table[(unsigned char)*b]);
}

/* Report read error or unexpected EOF STREAM for FILENAME, using ERROR */
void
read_error (const char *filename, FILE *stream, int err)
{
  if (ferror (stream))
    error (0, err, _("I/O error reading `%s'"), filename);
  else
    error (0, 0, _("unexpected EOF reading `%s'"), filename);
}

/* Allocate SIZE bytes, terminate on failure */
void *
xmalloc (size_t size)
{
  void *p;

  p = malloc (size);
  if (p != NULL || size == 0)
    return p;
  error (EXIT_FAILURE, errno, _("can not allocate memory"));
  abort (); /* Not reached */
}

/* Used by obstack code */
struct _obstack_chunk *
obstack_chunk_alloc (long size)
{
  return xmalloc (size);
}

 /* Reading of existing databases */

/* Open FILENAME (already open as FILE), as DB, report error on failure if not
   QUIET.  Store database header to *HEADER; return 0 if OK, -1 on error.
   Takes ownership of FILE: it will be closed by db_close () or before return
   from this function if it fails.

   FILENAME must stay valid until db_close (). */
int
db_open (struct db *db, struct db_header *header, FILE *file,
	 const char *filename, _Bool quiet)
{
  static const uint8_t magic[] = DB_MAGIC;

  if (fread (header, sizeof (*header), 1, file) != 1)
    {
      if (quiet == 0)
	read_error (filename, file, errno);
      goto err_file;
    }
  assert (sizeof (magic) == sizeof (header->magic));
  if (memcmp (header->magic, magic, sizeof (magic)) != 0)
    {
      if (quiet == 0)
	error (0, 0, _("`%s' does not seem to be a mlocate database"),
	       filename);
      goto err_file;
    }
  if (header->version != DB_VERSION_0)
    {
      if (quiet == 0)
	error (0, 0, _("`%s' has unknown version %u"), filename,
	       (unsigned)header->version);
      goto err_file;
    }
  if (header->check_visibility != 0 && header->check_visibility != 1)
    {
      if (quiet == 0)
	error (0, 0, _("`%s' has unknown visibility flag %u"), filename,
	       (unsigned)header->check_visibility);
      goto err_file;
    }
  db->file = file;
  db->filename = filename;
  db->buf_pos = db->buffer;
  db->buf_end = db->buffer;
  db->read_bytes = sizeof (*header);
  return 0;

 err_file:
  fclose (file);
  return -1;
}

/* Close DB */
void
db_close (struct db *db)
{
  fclose (db->file);
}

/* Refill DB->buffer;
   return number of bytes in buffer OK, 0 on error or EOF */
static size_t
db_refill (struct db *db)
{
  size_t size;

  size = fread (db->buffer, 1, sizeof (db->buffer), db->file);
  db->buf_pos = db->buffer;
  db->buf_end = db->buffer + size;
  db->read_bytes += size;
  return size;
}

/* Read SIZE (!= 0) bytes from DB to BUF;
   return 0 if OK, -1 on error */
int
db_read (struct db *db, void *buf, size_t size)
{
  assert (size != 0);
  do
    {
      size_t run;

      run = db->buf_end - db->buf_pos;
      if (run == 0)
	{
	  run = db_refill (db);
	  if (run == 0)
	    return -1;
	}
      if (run > size)
	run = size;
      memcpy (buf, db->buf_pos, run);
      buf = (char *)buf + run;
      db->buf_pos += run;
      size -= run;
    }
  while (size != 0);
  return 0;
}

/* Read a NUL-terminated string from DB to current object in OBSTACK (without
   the terminating NUL), report error on failure if not QUIET.
   return 0 if OK, or -1 on I/O error. */
int
db_read_name (struct db *db, struct obstack *h, _Bool quiet)
{
  for (;;)
    {
      size_t run;
      char *nul;

      run = db->buf_end - db->buf_pos;
      if (run == 0)
	{
	  run = db_refill (db);
	  if (run == 0)
	    goto err;
	}
      nul = memchr (db->buf_pos, 0, run);
      if (nul != NULL)
	{
	  obstack_grow (h, db->buf_pos, nul - db->buf_pos);
	  db->buf_pos = nul + 1;
	  break;
	}
      obstack_grow (h, db->buf_pos, run);
      db->buf_pos = db->buf_end;
    }
  return 0;

 err:
  if (quiet == 0)
    read_error (db->filename, db->file, errno);
  return -1;
}

/* Skip SIZE bytes in DB, report error on failure if not QUIET;
   return 0 if OK, -1 on error */
int
db_skip (struct db *db, off_t size, _Bool quiet)
{
  _Bool do_fseek;
  size_t run;
  
  run = db->buf_end - db->buf_pos;
  if (run > size)
    run = size;
  db->buf_pos += run;
  size -= run;
  do_fseek = 1;
  while (size != 0)
    {
      if (do_fseek != 0)
	{
	  run = LONG_MAX;
	  if (run > size)
	    run = size;
	  if (fseek (db->file, size, SEEK_CUR) != 0)
	    {
	      if (errno != ESPIPE)
		{
		  if (quiet == 0)
		    error (0, errno, _("I/O error seeking in `%s'"),
			   db->filename);
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
	  run = fread (buf, 1, run, db->file);
	  db->read_bytes += size;
	  if (run == 0)
	    {
	      if (quiet == 0)
		read_error (db->filename, db->file, errno);
	      goto err;
	    }
	}
      size -= run;
    }
  return 0;

 err:
  return -1;
}

/* Return number of bytes read from DB so far  */
uintmax_t
db_bytes_read (struct db *db)
{
  return db->read_bytes - (db->buf_end - db->buf_pos);
}
