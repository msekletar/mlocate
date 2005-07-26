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

#ifndef LIB_H__
#define LIB_H__

#include <config.h>

#include <stdio.h>
#include <stdint.h>
#include <sys/types.h>

#include <obstack.h>

#include "db.h"

#ifdef ENABLE_NLS
#include <libintl.h>
#else
#define gettext(X) (X)
#define ngettext(S, P, N) ((N) != 1 ? (P) : (S))
#endif

#define _(X) gettext(X)

/* Number of entries in ARRAY */
#define ARRAY_SIZE(array) (sizeof (array) / sizeof (*array))

/* Convert VAL to big endian */
extern uint64_t htonll (uint64_t val);

/* Convert VAL from big endian */
extern uint64_t ntohll (uint64_t val);

/* Initialize dir_path_cmp_table */
extern void dir_path_cmp_init (void);

/* Compare two path names using the database directory order. This is not
   exactly strcmp () order: "a" < "a.b", so "a/z" < "a.b". */
extern int dir_path_cmp (const char *a, const char *b);

/* Report read error or unexpected EOF STREAM for FILENAME, using ERR */
extern void read_error (const char *filename, FILE *stream, int err);

/* Allocate SIZE bytes, terminate on failure */
extern void *xmalloc (size_t size);

/* Functions used by obstack code */
extern struct _obstack_chunk *obstack_chunk_alloc (long size);

#define obstack_chunk_free free

/* The obstack code is too portable to be perfect :(. So we need some
   better substitutes to avoid 32-bit overflows. */
#define OBSTACK_OBJECT_SIZE(H) \
  (size_t)((char *)obstack_next_free (H) - (char *)obstack_base (H))

/* To avoid obstack integer overflows, limit large obstack allocations to this
   value. */
enum { OBSTACK_SIZE_MAX = 1024 * 1024 };

/* An open database */
struct db
{
  FILE *file;
  const char *filename;
  char *buf_pos, *buf_end;
  uintmax_t read_bytes;
  char buffer[BUFSIZ];
};

/* Open FILENAME (already open as FILE), as DB, report error on failure if not
   QUIET.  Store database header to *HEADER; return 0 if OK, -1 on error.
   Takes ownership of FILE: it will be closed by db_close () or before return
   from this function if it fails.

   FILENAME must stay valid until db_close (). */
extern int db_open (struct db *db, struct db_header *header, FILE *file,
		    const char *filename, _Bool quiet);

/* Close DB */
extern void db_close (struct db *db);

/* Read SIZE (!= 0) bytes from DB to BUF;
   return 0 if OK, -1 on error */
extern int db_read (struct db *db, void *buf, size_t size);

/* Read a NUL-terminated string from DB to current object in OBSTACK (without
   the terminating NUL), report error on failure if not QUIET.
   return 0 if OK, or -1 on I/O error. */
extern int db_read_name (struct db *db, struct obstack *h, _Bool quiet);

/* Skip SIZE bytes in DB, report error on failure if not QUIET;
   return 0 if OK, -1 on error */
extern int db_skip (struct db *db, off_t size, _Bool quiet);

/* Return number of bytes read from DB so far  */
extern uintmax_t db_bytes_read (struct db *db);

#endif
