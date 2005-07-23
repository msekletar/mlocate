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

/* Open FILENAME, report error on failure if not QUIET.  Store database
   header to *HEADER;
   Return open database or NULL on error. */
FILE *
open_db (struct db_header *header, const char *filename, _Bool quiet)
{
  static const uint8_t magic[] = DB_MAGIC;

  FILE *f;

  f = fopen (filename, "rb");
  if (f == NULL)
    {
      if (quiet == 0)
	error (0, errno, _("can not open `%s'"), filename);
      goto err;
    }
  if (fread (header, sizeof (*header), 1, f) != 1)
    {
      if (quiet == 0)
	read_error (filename, f, errno);
      goto err_f;
    }
  assert (sizeof (magic) == sizeof (header->magic));
  if (memcmp (header->magic, magic, sizeof (magic)) != 0)
    {
      if (quiet == 0)
	error (0, 0, _("`%s' does not seem to be a mlocate database"),
	       filename);
      goto err_f;
    }
  if (header->version != DB_VERSION_0)
    {
      if (quiet == 0)
	error (0, 0, _("`%s' has unknown version %u"), filename,
	       (unsigned)header->version);
      goto err_f;
    }
  if (header->check_visibility != 0 && header->check_visibility != 1)
    {
      if (quiet == 0)
	error (0, 0, _("`%s' has unknown visibility flag %u"), filename,
	       (unsigned)header->check_visibility);
      goto err_f;
    }
  return f;

 err_f:
  fclose (f);
 err:
  return NULL;
}

/* Read a NUL-terminated string from FILE to current object in OBSTACK (without
   the terminating NULL), report error on failure about DATABASE if it is not
   NULL.
   Return 0 if OK, or -1 on I/O error. */
int
read_name (struct obstack *h, FILE *file, const char *database)
{
  int res;

  /* Surprisingly, this seems to be faster than fread () with a known size. */
  res = 0;
  flockfile (file);
  for (;;)
    {
      int c;

      c = getc_unlocked (file);
      if (c == 0)
	break;
      if (c == EOF)
	{
	  if (database != NULL)
	    read_error (database, file, errno);
	  res = -1;
	  break;
	}
      obstack_1grow (h, c);
    }
  funlockfile (file);
  return res;
}
