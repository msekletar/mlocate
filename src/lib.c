/* Common functions.
   Copyright (C) 2005 FIXME */
#include <config.h>

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db.h"
#include "lib.h"

/* Convert VAL to big endian */
uint64_t
htonll(uint64_t val)
{
  uint8_t a[8];
  uint64_t ret;

  a[0] = (val >> 56) & 0xFF;
  a[1] = (val >> 48) & 0xFF;
  a[2] = (val >> 40) & 0xFF;
  a[3] = (val >> 32) & 0xFF;
  a[4] = (val >> 24) & 0xFF;
  a[5] = (val >> 16) & 0xFF;
  a[6] = (val >> 8) & 0xFF;
  a[7] = val & 0xFF;
  assert (sizeof (ret) == sizeof (a));
  memcpy (&ret, a, sizeof (ret));
  return ret;
}

/* Convert VAL from big endian */
uint64_t
ntohll(uint64_t val)
{
  uint8_t a[8];

  assert (sizeof (a) == sizeof (val));
  memcpy (a, &val, sizeof (val));
  return (uint64_t)a[0] << 56 | (uint64_t)a[1] << 48 | (uint64_t)a[2] << 40
    | (uint64_t)a[3] << 32 | (uint32_t)a[4] << 24
    | (uint32_t)a[5] << 16 | (unsigned)a[6] << 8 | a[7];
}

/* Report message FMT with AP and potentially ERR if non-zero */
static void
verror (int err, const char *fmt, va_list ap)
{
  vfprintf (stderr, fmt, ap);
  if (err == 0)
    putc ('\n', stderr);
  else
    fprintf (stderr, ": %s\n", strerror (err));
}

/* Report message FMT and potentially ERR if non-zero */
void
error (int err, const char *fmt, ...)
{
  va_list ap;

  va_start (ap, fmt);
  verror (err, fmt, ap);
  va_end (ap);
}

/* Report message FMT and potentially ERR if non-zero and
   exit (EXIT_FAILURE) */
void
fatal (int err, const char *fmt, ...)
{
  va_list ap;

  va_start (ap, fmt);
  verror (err, fmt, ap);
  va_end (ap);
  exit (EXIT_FAILURE);
}

/* Report read error or unexpected EOF STREAM for FILENAME, using ERROR */
void
read_error (const char *filename, FILE *stream, int err)
{
  if (ferror (stream))
    error (err, _("I/O error reading `%s'"), filename);
  else
    error (0, _("Unexpected EOF reading `%s'"), filename);
}

/* Allocate SIZE bytes, terminate on failure */
void *
xmalloc (size_t size)
{
  void *p;

  p = malloc (size);
  if (p != NULL || size == 0)
    return p;
  fatal (errno, _("Can not allocate memory"));
}

/* Reallocate PTR to SIZE bytes, terminate on failure */
void *
xrealloc (void *ptr, size_t size)
{
  ptr = realloc (ptr, size);
  if (ptr != NULL || size == 0)
    return ptr;
  fatal (errno, _("Can not allocate memory"));
}

/* Make sure GB is at least LEN bytes large */
void
gb_alloc (struct growbuf *gb, size_t len)
{
  /* TIME: Total allocations of buffer is O(N), where N is maximal requested
     buffer size rounded up */
  if (gb->len >= len)
    return;
  if (gb->len == 0)
    gb->len = 64; /* Arbitrary */
  while (gb->len < len)
    gb->len *= 2;
  gb->p = xrealloc (gb->p, gb->len);
}

/* Open FILENAME, report error on failure if not QUIET.
   Return open database or NULL on error. */
FILE *
open_db (const char *filename, _Bool quiet)
{
  static const uint8_t magic[] = DB_MAGIC;

  FILE *f;
  struct db_header header;

  f = fopen (filename, "rb");
  if (f == NULL)
    {
      if (quiet == 0)
	error (errno, _("Can not open `%s'"), filename);
      goto err;
    }
  if (fread (&header, sizeof (header), 1, f) != 1)
    {
      if (quiet == 0)
	read_error (filename, f, errno);
      goto err_f;
    }
  assert (sizeof (magic) == sizeof (header.magic));
  if (memcmp (header.magic, magic, sizeof (magic)) != 0)
    {
      if (quiet == 0)
	error (0, _("`%s' does not seem to be a mlocate database"), filename);
      goto err_f;
    }
  if (header.version != DB_VERSION_0)
    {
      if (quiet == 0)
	error (0, _("`%s' has unknown version %u"), filename,
	       (unsigned)header.version);
      goto err_f;
    }
  return f;

 err_f:
  fclose (f);
 err:
  return NULL;
}

/* Read a NUL-terminated string from DATABASE FILE to BUF from OFFSET on.
   Return offset after the read string, or (size_t)-1 on I/O error. */
size_t
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

      c = getc_unlocked (file);
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
